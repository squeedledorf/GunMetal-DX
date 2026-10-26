// LLRenderVK — a Vulkan reimplementation of LLRender's immediate-mode geometry
// + render-state API (the `gGL` surface that ALL of the viewer's UI, text, and
// legacy rendering call through).
//
// This is the PHASE 3 CORE de-risk: it proves the classic immediate-mode +
// matrix-stack + render-state model maps cleanly onto modern Vulkan, using the
// well-known "buffer up begin..end batches, flush on state change" pattern over
// a growable dynamic vertex buffer and a small set of state-keyed pipelines.
//
// The method names deliberately mirror LLRender (gGL) so the eventual real seam
// is a coarse per-call dispatch inside LLRender:
//
//     void LLRender::vertex3f(F32 x,F32 y,F32 z) { sBackend->vertex3f(x,y,z); }
//
// Everything here is standalone: it depends only on the Vulkan headers, glm and
// the tiny RHI (vk_device / vk_offscreen) shared with the vktest spike. It does
// NOT touch the viewer build.

#pragma once

#include "vk_device.h"
#include "vk_descpool.h"
#include "vk_offscreen.h"
#include "vk_stagering.h"

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_RADIANS
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// Mirror of LLRender's public enums (kept independent so the module doesn't pull
// in the whole viewer). Values need not match LLRender numerically; the real
// seam translates at the boundary.
namespace llrvk {

enum eGeomMode : uint8_t {
    TRIANGLES = 0,
    TRIANGLE_STRIP,
    TRIANGLE_FAN,
    LINES,
    LINE_STRIP,
    LINE_LOOP,      // emulated: LINE_STRIP + closing edge appended at end()
    POINTS,
    QUADS,          // emulated: expanded to two triangles per quad at emit time
    NUM_MODES
};

// Subset of LLRender::eBlendFactor, same GL->VK meaning.
enum eBlendFactor : uint8_t {
    BF_ONE = 0,
    BF_ZERO,
    BF_DEST_COLOR,
    BF_SOURCE_COLOR,
    BF_ONE_MINUS_DEST_COLOR,
    BF_ONE_MINUS_SOURCE_COLOR,
    BF_DEST_ALPHA,
    BF_SOURCE_ALPHA,
    BF_ONE_MINUS_DEST_ALPHA,
    BF_ONE_MINUS_SOURCE_ALPHA
};

enum eBlendType : uint8_t {
    BT_ALPHA = 0,   // src_alpha, one_minus_src_alpha
    BT_ADD,         // one, one
    BT_MULT,        // dst_color, zero
    BT_REPLACE      // one, zero  (blend disabled)
};

// Depth compare func — mirrors LLRender::eCompareFunc / GL depth funcs. The real
// seam translates GL_LESS etc. at the boundary; values here need not match GL.
enum eCompareFunc : uint8_t {
    CF_NEVER = 0,
    CF_ALWAYS,
    CF_LESS,
    CF_LESS_EQUAL,
    CF_EQUAL,
    CF_NOT_EQUAL,
    CF_GREATER_EQUAL,
    CF_GREATER
};

// Texture sampler address mode — mirrors LLTexUnit::eTextureAddressMode. UI/text
// use CLAMP; some tiled UI art uses WRAP. MIRROR included for completeness.
enum eTextureAddressMode : uint8_t {
    TAM_WRAP = 0,
    TAM_MIRROR,
    TAM_CLAMP
};

// Texture filter — mirrors the point/bilinear axis of LLTexUnit::eTextureFilterOptions.
enum eTextureFilter : uint8_t {
    TF_POINT = 0,   // nearest
    TF_BILINEAR     // linear
};

// Storage format of a sampled texture (#405). Every one samples as RGBA in the shaders
// (the view swizzles), exactly what the RGBA8 expansion of the same pixels gave:
//   RGBA8        4 B/texel, as is
//   LA8          2 B/texel R8G8 (luminance, alpha)  -> (L, L, L, A)
//   L8           1 B/texel R8 luminance             -> (L, L, L, 1)
//   A8           1 B/texel R8 alpha (GL_ALPHA)      -> (0, 0, 0, A)
//   COVERAGE8    1 B/texel R8 alpha, white colour   -> (1, 1, 1, A)  (glyph pages)
enum eTexelFormat : uint8_t {
    TXF_RGBA8 = 0,
    TXF_LA8,
    TXF_L8,
    TXF_A8,
    TXF_COVERAGE8
};
inline uint32_t texelBytes(eTexelFormat f) { return f == TXF_RGBA8 ? 4u : f == TXF_LA8 ? 2u : 1u; }

// A texture the facade can bind. In the real seam this is the LLTextureData/LLTexture
// analog; here it is an owned sampled image built from pixels in one eTexelFormat.
struct VkTexture {
    GpuImage image{};
    VkImageView view = VK_NULL_HANDLE; // == image.view; kept for clarity
    VkDescriptorSet dset = VK_NULL_HANDLE;
    uint32_t id = 0;                   // stable handle for bindTexture()
    // The filter this texture was uploaded with (mirrors the filtering an
    // LLTextureData carries). bindTexture() adopts it into the active unit unless the
    // caller overrides via setTextureFilter() afterwards.
    eTextureFilter defaultFilter = TF_BILINEAR;
    eTexelFormat texFmt = TXF_RGBA8;   // storage format (render targets: RGBA8)
    bool pendingFree = false;          // destroyTexture queued; slot recycled later
};

} // namespace llrvk

// The facade. One instance owns all Vulkan objects; a frame is:
//     gVK.beginFrame(target);
//     gVK.ortho(...); gVK.begin(...); gVK.color4ub(...); gVK.vertex2f(...); gVK.end();
//     ...
//     gVK.endFrame();      // flushes + submits the single command buffer
class LLRenderVK {
public:
    bool init(VkDeviceContext& ctx, const std::string& shaderDir);
    // Viewer path: init from SPIR-V already in memory (embedded byte arrays), so
    // the facade needs no runtime shader files / packaging. vertSpv/fragSpv are
    // raw SPIR-V words; sizes are in BYTES.
    bool initFromSpv(VkDeviceContext& ctx,
                     const uint32_t* vertSpv, size_t vertBytes,
                     const uint32_t* fragSpv, size_t fragBytes);
    // Alpha-tested variant of the fragment shader (offscreen segments with
    // setMinimumAlpha > 0). Optional: without it the alpha test is skipped.
    void initAlphaTestFragSpv(const uint32_t* fragSpv, size_t fragBytes);
    // In-world label occlusion variant of the vertex shader (#487, ui_occl.vert).
    // Optional: without it the pre-HUD text layer stays depth-free.
    void initOcclVertSpv(const uint32_t* vertSpv, size_t vertBytes);
    void shutdown();
    bool isInitialized() const { return mCtx != nullptr; }

    // ---- texture creation (analog of uploading an LLTextureData) ----------------
    // Creates a sampled RGBA8 texture and returns a handle usable with
    // bindTexture(). `nearest` picks NEAREST vs LINEAR filtering.
    // `fmt` (#405): the layout of `pixels` and the storage format (texelBytes(fmt) per
    // texel, tightly packed); RGBA8 by default. pixels == nullptr: the image is created
    // without contents; follow with a full-rectangle beginTextureUpdate (#458).
    // <GMDX> levels > 1 (RGBA8 with pixels only): a mip chain, made from the upload on the GPU
    // (box-filtered blits) -- for art the UI draws well below its size (textures.xml use_mips).
    uint32_t createTexture(uint32_t w, uint32_t h, const uint8_t* pixels,
                           bool nearest, llrvk::eTexelFormat fmt = llrvk::TXF_RGBA8, uint32_t levels = 1);

    // ---- texture update (analog of LLTextureData::setImage / setSubImage) --------
    // Re-uploads the full pixels of an existing texture id (font atlas growth, UI
    // images whose CPU copy changed). A size or format change recreates the image.
    // Live frames queue the copy for the next recordUploads(); outside one it is
    // submitted immediately.
    void updateTexture(uint32_t texId, uint32_t w, uint32_t h,
                       const uint8_t* pixels, llrvk::eTexelFormat fmt = llrvk::TXF_RGBA8);
    // Two-phase (sub-rectangle) update, #458: the caller writes straight into staging,
    // so a per-frame media update needs no scratch copy and no per-call allocation.
    // beginTextureUpdate() returns rw * rh * texelBytes(fmt) bytes (rows of rw texels,
    // tightly packed) to fill with the rectangle (x, y, rw, rh) of the texture, whose
    // full size / format is (w, h, fmt); commitTextureUpdate() then queues the copy.
    // Only the rectangle's texels change (the rest of the image is preserved). A size
    // or format change recreates the image, which needs the full rectangle: nullptr
    // then (as for an unknown id or a failed staging allocation) -- upload in full.
    // Nothing may touch this facade's textures between begin and commit.
    // mayDefer: with the staging ring at its cap and full (frames not being recorded, or
    // more upload traffic than the ring holds), return nullptr instead of allocating a
    // one-off buffer -- the caller keeps its old generation and retries on a later bind.
    // Pass false when the image must get its pixels now (a just-created image).
    uint8_t* beginTextureUpdate(uint32_t texId, uint32_t w, uint32_t h, llrvk::eTexelFormat fmt,
                                uint32_t x, uint32_t y, uint32_t rw, uint32_t rh, bool mayDefer = true);
    void commitTextureUpdate();

    // ---- texture release (analog of LLTextureData destruction) --------------------
    // Frees a texture id. The image and its descriptor sets are released only after
    // kDeferFrames more frames have been recorded (in-flight frames may still sample
    // it; batches already queued this frame still bind it), then the id is recycled.
    // Id 0 (white) is never freed.
    void destroyTexture(uint32_t texId);
    // Live (swapchain) frames: texture uploads queued by createTexture / updateTexture
    // are recorded into the frame's command buffer here, right after it begins and
    // before any pass samples them -- instead of a submit-and-WAIT per upload, which
    // blocked the main thread behind a whole GPU frame whenever a UI image changed
    // (minimap, font atlas, previews). No-op when nothing is queued.
    void recordUploads(VkCommandBuffer cmd);
    // Frames recorded so far (advanced by recordFrame / endFrame).
    uint64_t frameCounter() const { return mFrameCounter; }

    // ---- offscreen render targets (#83: LLViewerDynamicTexture under VK) -----
    // A render target is an ordinary facade texture id (bindTexture() samples it in
    // the UI) whose image can also be rendered into: R8G8B8A8_UNORM colour
    // (+ optional D32_SFLOAT depth, CONVENTIONAL: cleared to 1.0, LEQUAL -- this is
    // not the reversed-Z main view). Released with destroyTexture().
    uint32_t createRenderTarget(uint32_t w, uint32_t h, bool withDepth);
    // True once a recorded frame has rendered into it (its image is in
    // SHADER_READ_ONLY_OPTIMAL and safe to sample / copy from).
    bool isRenderTargetReady(uint32_t texId) const;
    VkImage textureImage(uint32_t texId) const;

    // ---- VRAM accounting (#405: the UI line of the VKMem log) ----
    // Device memory of one texture id's image (its allocation size; 0 = none).
    uint64_t textureAllocBytes(uint32_t texId) const;
    // The texture's image is suballocated from a texture pool (#411): no page padding.
    bool texturePooled(uint32_t texId) const;
    // Everything the facade holds, by role (allocation sizes, no page padding).
    struct MemStats
    {
        uint64_t texBytes = 0;     uint32_t texCount = 0;     // sampled textures (white included)
        uint64_t rtBytes = 0;      uint32_t rtCount = 0;      // render targets: colour
        uint64_t rtDepthBytes = 0; uint32_t rtDepthCount = 0; //   + depth
        uint64_t pendingBytes = 0; uint32_t pendingCount = 0; // released, waiting on frames in flight
        uint64_t ringBytes = 0;                               // vertex ring, all slots (host-visible)
        uint64_t stagingBytes = 0;                            // one-off upload staging not yet retired (host)
        // #458: the upload staging ring (persistent, host-visible; grows to a cap, released
        // when idle): capacity, bytes in use, peak use, bytes staged by the last recorded
        // frame, one-off buffers since start (too big for / no room in the ring), and
        // uploads since start: all / sub-rectangle.
        uint64_t stageRingCap = 0, stageRingUsed = 0, stageRingPeak = 0, stageFrameBytes = 0;
        uint64_t stageFallbacks = 0, stageUploads = 0, stagePartial = 0, stageDeferred = 0;
    };
    void memStats(MemStats& out) const;
    // Upload staging ring cap (#458), clamped to [32 MB, 256 MB]. Lowering it affects the
    // next growth (a bigger ring in use stays until its idle release).
    void setStageRingMax(uint64_t bytes);
    // Offscreen segments. Batches issued between beginOffscreen() and endOffscreen()
    // draw into that target instead of the swapchain; they are recorded by
    // recordOffscreen() at the start of the frame's command buffer (after
    // recordUploads, before the world and the UI), so the same frame's world and UI
    // sample the result. Segments nest (an appearance composite rendered from inside
    // a visual-param hint); a resumed parent LOADs what it drew before. Inside a
    // segment: the GL bottom row lands in image row 0 (LL texcoords assume it), the
    // depth state is honoured, the alpha blend factors equal the colour factors (GL
    // glBlendFunc semantics), setMinimumAlpha() is an alpha test, and textures
    // sample bilinear unless the caller asks for point. All render state is saved at
    // begin and restored at end.
    bool beginOffscreen(uint32_t texId, bool clearColor, const float clearRGBA[4], bool clearDepth);
    void endOffscreen();
    // Clear the current offscreen target's depth from here on (glClear(DEPTH) mid-render).
    void clearOffscreenDepth();
    bool inOffscreen() const { return !mOffStack.empty(); }
    void recordOffscreen(VkCommandBuffer cmd, uint32_t frameSlot);
    // Alpha test threshold (LLRender::setMinimumAlpha); offscreen segments only.
    void setMinimumAlpha(float a);

    // ---- frame lifecycle (OFFSCREEN — the self-test path) -------------------
    void beginFrame(OffscreenTarget& target);
    void endFrame();   // flush + submit + wait

    // ---- frame lifecycle (SWAPCHAIN — the live-viewer path) -----------------
    // The viewer owns the command buffer, acquire/submit/present and the color
    // image (a swapchain image view). beginFrameToView() opens a frame that will
    // record into `cmd` (already in the recording state, inside no render pass);
    // recordFrame() lays down all batches via a single vkCmdBeginRendering over
    // `colorView` (LOAD to preserve a prior clear/blit) and vkCmdEndRendering. The
    // caller transitions the image to COLOR_ATTACHMENT_OPTIMAL before, and to
    // PRESENT_SRC after. No submit/wait happens here.
    void beginFrameToView(VkImageView colorView, VkFormat colorFormat,
                          uint32_t w, uint32_t h);
    // The acquired swapchain image view is only known after acquire(), which the
    // viewer does AFTER beginFrameToView()+UI buffering; set it here before
    // recordFrame(). The format was fixed at beginFrameToView() (all swapchain
    // images share it) so pipelines built during UI buffering stay valid.
    void setFrameColorView(VkImageView colorView) { mFrameColorView = colorView; }
    // flush + record draws into cmd, uploading this frame's vertices into the VBO
    // ring slot `frameSlot` (0..framesInFlight-1). The caller guarantees that slot
    // is retired (its in-flight fence was waited) before calling, so the previous
    // frame that shared the SAME swapchain image is done reading that buffer.
    void recordFrame(VkCommandBuffer cmd, uint32_t frameSlot);

    // ---- world-overlay segment (swapchain frames) ---------------------------
    // LL's 3D overlays (property lines, ban lines, parcel highlight, tracking
    // beacons, edit manipulators) are drawn by the pipeline against the SCENE depth.
    // Batches issued between beginWorldOverlay() and endWorldOverlay() form a
    // separate segment that the world renderer records mid-frame (before the HUD
    // pass clears depth) with the world depth attached, honouring LLRender::DepthScope.
    // Everything else in a swapchain frame (HUD overlays, name tags, the 2D UI)
    // never depth-tests. setWorldOverlayDepthFormat() is set by the context each
    // frame (UNDEFINED = no world this frame: beginWorldOverlay() returns false).
    void setWorldOverlayDepthFormat(VkFormat fmt) { mOverlayDepthFormat = fmt; }
    // The world depth the overlay tests against is REVERSED-Z (near 1, far 0): LL's
    // GL-style compares (LEQUAL / GREATER ...) and the camera-ward bias are mirrored.
    void setWorldOverlayReverseZ(bool r) { mOverlayReverseZ = r; }
    bool beginWorldOverlay();
    void endWorldOverlay();
    // ---- pre-HUD text layer (swapchain frames, #468) -------------------------
    // In-world 2D text (name tags, chat bubbles, prim hover text, HUD effects) must
    // sit UNDER the HUD attachments, as in LL (render_hud_elements before
    // render_hud_attachments). Batches issued between beginPreHudLayer() and
    // endPreHudLayer() are plain 2D UI batches (no depth: same pipelines as the UI
    // pass) that the world renderer records right after the overlay segment, before
    // its HUD pass. If the world doesn't record them this frame, recordFrame() draws
    // them in buffer order with the UI (the pre-#468 placement). One per frame.
    // proj / view (#487): the world camera (VK clip convention, the projection the
    // world depth was drawn with) -- enables label occlusion for the layer; null = none.
    bool beginPreHudLayer(const float* proj = nullptr, const float* view = nullptr);
    void endPreHudLayer();
    // ---- in-world label occlusion (#487) -------------------------------------
    // LL depth-tests name tags / hover text against the scene. Here a label is tested
    // ONCE, at its anchor (the agent-space point it is attached to), against the
    // resolved world depth: batches issued in the pre-HUD layer with the depth test
    // ON while an anchor is set fade / hide as a whole (ui_occl.vert). No anchor, the
    // depth test off, or outside the layer: plain depth-free 2D, as before. The
    // anchor is projected here with the layer's viewProj; one behind the camera or
    // off screen is never occluded. Setting a different anchor closes the open batch.
    void setLabelAnchor(bool on, float x = 0.f, float y = 0.f, float z = 0.f, float bias = 0.15f);
    // Called from inside the world renderer's command recording: uploads this
    // frame's vertices (once) and records the overlay segment over colorView with
    // `depthView` (DEPTH_ATTACHMENT_OPTIMAL, world depth) attached, then (with
    // `preHud`) the pre-HUD text layer over colorView alone. Anchored labels sample
    // `depthImage`/`depthView` (the display-size world depth): the layer moves it to
    // DEPTH_READ_ONLY_OPTIMAL for its scope and back to DEPTH_ATTACHMENT_OPTIMAL.
    void recordWorldOverlay(VkCommandBuffer cmd, uint32_t frameSlot,
                            VkImageView colorView, VkImageView depthView,
                            VkImage depthImage, uint32_t w, uint32_t h,
                            bool preHud = true);

    // ---- immediate-mode geometry (mirrors gGL) ------------------------------
    void begin(llrvk::eGeomMode mode);
    void end();

    void vertex2f(float x, float y);
    void vertex3f(float x, float y, float z);
    void texCoord2f(float u, float v);
    void color4ub(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
    void color4f(float r, float g, float b, float a);

    // Pre-transformed batch (mirrors gGL::vertexBatchPreTransformed). This is the
    // exact call LLFontGL uses to submit glyph quads: parallel arrays of already
    // screen-space positions, UVs and per-vertex RGBA8 colors, 6 verts per glyph.
    // Must be called inside begin()/end(). The positions are NOT transformed by
    // the modelview here (they already are, in gGL) — but they DO go through the
    // batch MVP, so callers set the projection/UI ortho as usual and leave the
    // modelview at identity, matching how gGL's UI path calls it.
    struct PreVert { float x, y, z; float u, v; uint8_t r, g, b, a; };
    void vertexBatchPreTransformed(const PreVert* verts, int count);

    // ---- matrix stack (mirrors gGL; MODELVIEW / PROJECTION / TEXTURE0..3) ---
    // MM_TEXTURE0..3 mirror LLRender's texture-matrix stacks (used by scrolling
    // UI textures, animated textures). MM_TEXTURE is gGL's alias for "the active
    // texture unit's matrix"; we resolve it to MM_TEXTURE0+activeUnit.
    enum eMatrixMode : uint8_t {
        MM_MODELVIEW = 0,
        MM_PROJECTION,
        MM_TEXTURE0,
        MM_TEXTURE1,
        MM_TEXTURE2,
        MM_TEXTURE3,
        NUM_MM,
        MM_TEXTURE   // alias, resolved to MM_TEXTURE0 + active tex unit
    };
    void matrixMode(eMatrixMode m);
    void pushMatrix();
    void popMatrix();
    void loadIdentity();
    void loadMatrix(const float* m16);
    void multMatrix(const float* m16);
    void translatef(float x, float y, float z);
    void scalef(float x, float y, float z);
    void rotatef(float degrees, float x, float y, float z);
    // Builds a Vulkan-correct ortho (Y-down framebuffer, Z in [0,1]) and
    // multiplies it into the current (usually PROJECTION) matrix, like gGL.ortho.
    void ortho(float l, float r, float b, float t, float zn, float zf);

    // ---- render state (mirrors gGL) -----------------------------------------
    void blendFunc(llrvk::eBlendFactor s, llrvk::eBlendFactor d);
    void setSceneBlendType(llrvk::eBlendType t);
    void setLineWidth(float w);        // real Vulkan dynamic lineWidth
    void bindTexture(uint32_t texId);  // 0 == unbind (white); binds to active unit

    // Color write mask (mirrors gGL::setColorMask). Baked into the pipeline key.
    void setColorMask(bool writeColor, bool writeAlpha);
    void setColorMask(bool r, bool g, bool b, bool a);

    // Depth test/mask/func (mirrors LLRender::DepthScope semantics: disabling the test
    // forces the write off, exactly like the GL class).
    void setDepthTest(bool enabled, bool writeEnabled, llrvk::eCompareFunc func);
    // Convenience mirrors of the common LLRender::DepthScope constructions.
    void enableDepthTest(bool writeEnabled = true,
                         llrvk::eCompareFunc func = llrvk::CF_LESS_EQUAL);
    void disableDepthTest();

    // Scissor rectangle (mirrors LLScissorRect / glScissor). Coordinates are in
    // framebuffer pixels with a BOTTOM-LEFT origin (GL convention); we flip to
    // Vulkan's top-left at record time. `enable=false` disables scissoring.
    void setScissor(bool enable, int x = 0, int y = 0, int w = 0, int h = 0);

    // ---- texture units (mirrors gGL.getTexUnit(i) subset) -------------------
    // Selects the active texture unit for subsequent bindTexture / texture-matrix
    // ops. UI overwhelmingly uses unit 0; text + a few multitexture UI effects
    // use unit 1. We support a small fixed number (kNumTexUnits).
    void setActiveTextureUnit(uint32_t unit);
    // Per-unit sampler state. Address/filter are mirrored from LLTexUnit and
    // resolved to a concrete VkSampler (nearest/linear x clamp/wrap/mirror).
    void setTextureAddressMode(llrvk::eTextureAddressMode mode);
    void setTextureFilter(llrvk::eTextureFilter filter);

    // Flush the currently open batch to the command buffer (state-change point).
    void flush();

    // Validation tally passthrough (for the test's pass/fail gate).
    VkDeviceContext& ctx() { return *mCtx; }

private:
    // A vertex matches gGL's layout: pos(3) + color(4 float) + uv(2).
    struct Vertex {
        float pos[3];
        float col[4];
        float uv[2];
    };

    // The static (pipeline-baked) state that, when it changes, forces both a
    // batch flush AND a distinct pipeline. Line width and scissor are DYNAMIC in
    // Vulkan, so they are not part of the pipeline key — but they still force a
    // draw split (recorded per-PendingDraw), handled separately below.
    struct BatchKey {
        VkPrimitiveTopology topo;
        bool blend;
        llrvk::eBlendFactor sf, df;
        uint8_t colorMask;          // bit0=R bit1=G bit2=B bit3=A
        bool depthTest;
        bool depthWrite;
        llrvk::eCompareFunc depthFunc;
        VkFormat depthFmt;          // pipeline depthAttachmentFormat (UNDEFINED = none)
        uint8_t depthBias;          // world-overlay segment: bias toward the camera
                                    // (1 conventional depth, 2 reversed-Z, 0 none)
        VkFormat colorFmt;          // colour attachment format (swapchain or render target)
        bool alphaTest;             // alpha-tested fragment shader (offscreen only)
        bool glAlpha;               // alpha blend factors = colour factors (offscreen: GL glBlendFunc)
        bool labelOccl;             // in-world label occlusion vertex shader (#487)
        bool operator==(const BatchKey& o) const {
            return topo == o.topo && blend == o.blend && sf == o.sf &&
                   df == o.df && colorMask == o.colorMask &&
                   depthTest == o.depthTest && depthWrite == o.depthWrite &&
                   depthFunc == o.depthFunc && depthFmt == o.depthFmt &&
                   depthBias == o.depthBias && colorFmt == o.colorFmt &&
                   alphaTest == o.alphaTest && glAlpha == o.glAlpha &&
                   labelOccl == o.labelOccl;
        }
    };

    // A closed batch, replayed as one vkCmdDraw inside endFrame's single command
    // buffer. The whole frame's batches accumulate here, then draw in order.
    struct PendingDraw {
        VkPipeline pipe;
        VkDescriptorSet dset;
        glm::mat4 mvp;
        uint32_t first;
        uint32_t count;
        float lineWidth;            // dynamic: vkCmdSetLineWidth per draw
        bool scissorEnable;         // dynamic: vkCmdSetScissor per draw
        VkRect2D scissor;           // valid iff scissorEnable
        bool isLine;                // topology is a line family (lineWidth applies)
        bool offscreen;             // belongs to an offscreen segment (recordOffscreen)
        bool alphaTest;             // pipeline is the alpha-tested variant
        float minAlpha;             // its threshold (fragment push constant)
        // #487: an anchored in-world label. `pipe` is the occlusion variant; the
        // depth-free `pipePlain` draws it wherever no scene depth is bound (the UI
        // pass fallback). anchor = {depth UV x, y, hide depth, 1/(hide - show)}.
        bool labelOccl;
        VkPipeline pipePlain;
        float anchor[4];
    };
    std::vector<PendingDraw> mPending;

    bool initCommon(VkDeviceContext& ctx);   // shared by init() / initFromSpv()
    VkPipeline pipelineFor(const BatchKey& key);
    void emitVertex(float x, float y, float z);
    glm::mat4 currentMVP() const;
    void resetFrameState(uint32_t fbHeight);
    // Record all buffered PendingDraws into an already-open dynamic-rendering
    // scope on `cmd` (shared by the offscreen submit and the swapchain record).
    // labelDepth (#487): the scene-depth set for anchored labels (set 1); null =
    // draw them with their depth-free pipelines.
    void recordDraws(VkCommandBuffer cmd, uint32_t w, uint32_t h, VkBuffer vbo,
                     size_t begin = 0, size_t end = (size_t)-1,
                     VkDescriptorSet labelDepth = VK_NULL_HANDLE);
    // Grow (if needed) and upload the frame's vertices into VBO ring slot `slot`,
    // returning the buffer to bind. slot 0 is used by the offscreen path.
    VkBuffer uploadFrameVerts(uint32_t slot);

    VkDeviceContext* mCtx = nullptr;
    OffscreenTarget* mTarget = nullptr;   // non-null only in the offscreen path
    VkCommandBuffer mCmd = VK_NULL_HANDLE;
    bool mInFrame = false;

    // Frame target description (source-agnostic: offscreen OR swapchain). Pipelines
    // key their color/depth attachment formats off these, so pipelineFor() no
    // longer reaches into mTarget directly.
    VkImageView mFrameColorView = VK_NULL_HANDLE; // swapchain path only
    VkFormat mFrameColorFormat = VK_FORMAT_R8G8B8A8_UNORM;
    bool     mFrameHasDepth = false;
    VkFormat mFrameDepthFormat = VK_FORMAT_UNDEFINED;
    bool     mFrameToSwapchain = false;
    // World-overlay segment state (swapchain frames only).
    VkFormat mOverlayDepthFormat = VK_FORMAT_UNDEFINED; // world depth format this frame
    bool     mOverlayReverseZ = false;                   // world depth is reversed-Z
    bool     mInOverlay = false;        // batches being issued go to the overlay segment
    size_t   mOverlayBegin = 0, mOverlayEnd = 0;   // [begin,end) of mPending
    bool     mOverlayRecorded = false;  // world renderer recorded the segment
    bool     mInPreHud = false;         // batches being issued go to the pre-HUD layer
    size_t   mPreHudBegin = 0, mPreHudEnd = 0;     // [begin,end) of mPending
    bool     mPreHudRecorded = false;   // world renderer recorded the layer
    // In-world label occlusion (#487).
    static constexpr uint32_t kPushAnchorOffset = 64;     // vertex: vec4 anchor after mat4 mvp
    static constexpr uint32_t kPushMinAlphaOffset = 80;   // fragment: alpha-test threshold
    bool      mPreHudOccl = false;     // the layer has a world camera (anchors project)
    glm::mat4 mPreHudViewProj{1.0f};
    glm::vec3 mPreHudCamPos{0.0f};
    bool      mAnchorOn = false;        // live anchor state (setLabelAnchor)
    float     mAnchor[4] = {0, 0, 0, 0};
    float     mOpenAnchor[4] = {0, 0, 0, 0};
    // Scene-depth descriptor (set 1, one combined image sampler), one per frame-in-
    // flight slot, rewritten every frame that has anchored labels: the slot's previous
    // frame is retired (its fence was waited), and the depth view may have been
    // recreated -- with the same handle value -- since the set was last written.
    std::vector<VkDescriptorSet> mLabelDepthSets;
    VkDescriptorSet labelDepthSet(uint32_t frameSlot, VkImageView depthView);
    VkBuffer mFrameVbo = VK_NULL_HANDLE;  // this frame's uploaded VBO (once per frame)
    uint32_t mSwapW = 0, mSwapH = 0;      // swapchain path render extent

    // Shaders / layouts.
    VkShaderModule mVert = VK_NULL_HANDLE;
    VkShaderModule mFrag = VK_NULL_HANDLE;
    VkShaderModule mVertOccl = VK_NULL_HANDLE;   // ui_occl.vert (#487)
    VkPipelineLayout mLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout mSetLayout = VK_NULL_HANDLE;
    VkDescPool mDescPool;   // layout-sized + accounted, FREE_DESCRIPTOR_SET_BIT

    // Pipeline cache keyed by (topology, blend on/off, blend factors).
    struct KeyHash {
        size_t operator()(const BatchKey& k) const {
            size_t h = (size_t)k.topo;
            h = h * 131 + (size_t)k.blend;
            h = h * 31 + (size_t)k.sf;
            h = h * 7 + (size_t)k.df;
            h = h * 17 + (size_t)k.colorMask;
            h = h * 3 + (size_t)k.depthTest;
            h = h * 5 + (size_t)k.depthWrite;
            h = h * 13 + (size_t)k.depthFunc;
            h = h * 11 + (size_t)k.depthFmt;
            h = h * 2 + (size_t)k.depthBias;
            h = h * 19 + (size_t)k.colorFmt;
            h = h * 2 + (size_t)k.alphaTest;
            h = h * 2 + (size_t)k.glAlpha;
            h = h * 2 + (size_t)k.labelOccl;
            return h;
        }
    };
    std::unordered_map<BatchKey, VkPipeline, KeyHash> mPipelines;

    // Growable host-visible dynamic vertex buffer (persistently mapped), one PER
    // frame-in-flight slot. A single shared VBO overwritten every frame was a
    // GPU-read/CPU-write hazard: frame N+1's memcpy could stomp vertices frame N's
    // in-flight draw was still reading (garbled UI). Sized lazily to the highest
    // slot requested. Slot 0 doubles as the offscreen path's buffer.
    std::vector<GpuBuffer> mVBO;
    std::vector<VkDeviceSize> mVBOCapacityBytes;
    std::vector<Vertex> mVerts;      // CPU accumulation for the whole frame
    uint32_t mFrameVertBase = 0;     // first vertex not yet drawn (batch start)

    // Textures. Index 0 = white. Each texture owns its image + view; the sampler
    // is chosen per-BIND from mSamplers (filter x address), so a texture can be
    // sampled with different filter/address modes at different draws. Descriptor
    // sets are cached per (textureId, samplerIndex).
    std::vector<llrvk::VkTexture> mTextures;
    // 6 samplers: index = filter(0=nearest,1=linear)*3 + address(0=wrap,1=mirror,2=clamp)
    static constexpr int kNumSamplers = 6;
    std::array<VkSampler, kNumSamplers> mSamplers{};
    // descriptor cache keyed by texId*kNumSamplers + samplerIndex.
    std::unordered_map<uint32_t, VkDescriptorSet> mDescCache;
    VkDescriptorSet descriptorFor(uint32_t texId, int samplerIndex);

    // DEFERRED RELEASE. Images / descriptor sets / texture ids released at frame N
    // are freed once N + kDeferFrames frames have been recorded, so no in-flight
    // frame can still sample them (frames in flight <= 2; +1 margin).
    static constexpr uint64_t kDeferFrames = 3;
    // One queued copy: staging (a one-off buffer `owned`, or a range of the ring at
    // `buf`/`off`) -> the rectangle (x, y, w, h) of `image`. preserve: the image already
    // holds pixels outside the rectangle (SHADER_READ_ONLY -> keep), else its old
    // contents are discarded (UNDEFINED; new / recreated image, full rectangle).
    struct PendingUpload
    {
        VkImage image = VK_NULL_HANDLE;
        GpuBuffer owned{};
        VkBuffer buf = VK_NULL_HANDLE;
        VkDeviceSize off = 0;
        uint32_t x = 0, y = 0, w = 0, h = 0;
        bool preserve = false;
        uint32_t levels = 1;   // <GMDX> > 1: the upload fills level 0, the blits the rest
    };
    std::vector<PendingUpload> mPendingUploads;
    // UPLOAD STAGING RING (#458). Live-frame uploads sub-allocate it (recorded by the
    // next recordUploads, which marks the head with the frame; processDeferred frees up
    // to a mark kDeferFrames later). It grows (x2, from kStageRingMin) when a request
    // does not fit, up to kStageRingMax; a request above max / 4, or with the ring at max
    // and full, takes a one-off buffer (counted). An idle, empty ring is released after
    // kStageRingIdleFrames so a burst (the login splash) does not pin it.
    static constexpr uint64_t kStageRingMin = 8ull << 20;
    static constexpr uint64_t kStageRingMax = 256ull << 20;
    uint64_t mStageRingMax = kStageRingMax;   // setStageRingMax (RAM-scaled by the viewer)
    static constexpr uint64_t kStageRingIdleFrames = 600;
    VkStageRing mStageRing;
    struct StageMark { uint64_t frame = 0, head = 0; };
    std::vector<StageMark> mStageMarks;     // <= kDeferFrames + 1 entries
    std::vector<GpuBuffer> mStageRetired;   // replaced ring buffers: deferred at recordUploads
    uint64_t mStageLastUse = 0;             // frame of the last ring allocation
    uint64_t mStagePeak = 0, mStageFrameBytes = 0, mStageFrameAccum = 0;
    uint64_t mStageFallbacks = 0, mStageUploads = 0, mStagePartial = 0, mStageDeferred = 0;
    // Staging for an upload of `bytes`: ring (live frames) or a one-off buffer. Fills
    // `u.owned` or `u.buf`/`u.off`; returns the mapped pointer, nullptr on failure (or,
    // mayDefer, when a live upload finds the ring at its cap and full).
    uint8_t* allocStaging(VkDeviceSize bytes, PendingUpload& u, bool mayDefer = false);
    bool growStageRing(uint64_t need);
    // Queue `u` (live frame) or record + submit it now and free its staging.
    void submitUpload(PendingUpload& u);
    PendingUpload mOpenUpload;              // between begin/commitTextureUpdate
    bool mUploadOpen = false;
    // Between recordUploads() and recordFrame(): the frame is being recorded, so a new
    // upload can't wait for the next frame's flush (this frame may already sample it).
    bool mRecordingFrame = false;
    void recordUploadCmds(VkCommandBuffer cmd, const PendingUpload& u);
    // Device-local sampled image in `fmt` with the swizzled view (see eTexelFormat).
    GpuImage createSampledImage(uint32_t w, uint32_t h, llrvk::eTexelFormat fmt, uint32_t levels = 1);   // <GMDX> levels
    struct DeferredFree
    {
        uint64_t frame = 0;
        GpuBuffer staging{};                     // may be empty (upload staging)
        GpuImage image{};                        // may be empty
        std::vector<VkDescriptorSet> sets;
        uint32_t recycleId = 0;                  // 0 = none
    };
    std::vector<DeferredFree> mDeferred;
    std::vector<uint32_t> mFreeTexIds;
    uint64_t mFrameCounter = 0;
    // Pull every cached descriptor set of texId out of mDescCache (caller defers them).
    void takeDescSets(uint32_t texId, std::vector<VkDescriptorSet>& out);
    void processDeferred(bool all);
    int samplerIndex(llrvk::eTextureFilter f, llrvk::eTextureAddressMode a) const {
        return (int)f * 3 + (int)a;
    }

    // ---- current immediate-mode state ----
    llrvk::eGeomMode mMode = llrvk::TRIANGLES;
    bool mInBegin = false;
    uint32_t mBeginVertStart = 0;    // where this begin() started in mVerts
    float mCurColor[4] = {1, 1, 1, 1};
    float mCurUV[2] = {0, 0};

    // matrix stacks
    static constexpr int kStackDepth = 32;
    std::array<std::array<glm::mat4, kStackDepth>, NUM_MM> mMat;
    std::array<int, NUM_MM> mMatIdx{};
    eMatrixMode mMatrixMode = MM_MODELVIEW;

    // blend / line state
    bool mBlendEnabled = true;
    llrvk::eBlendFactor mSF = llrvk::BF_SOURCE_ALPHA;
    llrvk::eBlendFactor mDF = llrvk::BF_ONE_MINUS_SOURCE_ALPHA;
    float mLineWidth = 1.0f;

    // color mask / depth / scissor state
    uint8_t mColorMask = 0xF;               // RGBA all on
    bool mDepthTest = false;
    bool mDepthWrite = false;
    llrvk::eCompareFunc mDepthFunc = llrvk::CF_LESS_EQUAL;
    bool mScissorEnable = false;
    int mScissorX = 0, mScissorY = 0, mScissorW = 0, mScissorH = 0;

    // texture-unit state
    static constexpr int kNumTexUnits = 4;
    struct TexUnit {
        uint32_t boundTex = 0;
        llrvk::eTextureFilter filter = llrvk::TF_BILINEAR;
        llrvk::eTextureAddressMode address = llrvk::TAM_CLAMP;
    };
    std::array<TexUnit, kNumTexUnits> mTexUnits{};
    uint32_t mActiveUnit = 0;

    // Resolve MM_TEXTURE alias -> concrete stack index for the active unit.
    int resolveMM(eMatrixMode m) const {
        if (m == MM_TEXTURE) return MM_TEXTURE0 + (int)mActiveUnit;
        return (int)m;
    }

    // The state under which the currently-buffered (undrawn) vertices were
    // recorded. When live state diverges, flush() closes them out first.
    BatchKey mOpenKey{};
    uint32_t mOpenTex = 0;           // unit-0 texture (the sampled descriptor)
    int mOpenSamplerIndex = 0;
    glm::mat4 mOpenMVP{1.0f};
    float mOpenLineWidth = 1.0f;
    bool mOpenScissorEnable = false;
    VkRect2D mOpenScissor{};
    bool mOpenIsLine = false;
    bool mHaveOpen = false;

    // Current framebuffer height, needed to flip GL-origin scissor to Vulkan.
    uint32_t mFbHeight = 0;

    // Build the BatchKey from current live state.
    BatchKey liveKey(VkPrimitiveTopology topo) const;

    // ---- offscreen segments / render targets (#83) ----
    VkShaderModule mFragATest = VK_NULL_HANDLE;   // alpha-tested fragment shader
    float mMinAlpha = 0.0f;
    struct RenderTargetInfo
    {
        GpuImage depth{};                // empty without depth
        bool ready = false;              // rendered by a recorded frame (SHADER_READ_ONLY)
        bool depthInit = false;          // depth image has left UNDEFINED
    };
    std::unordered_map<uint32_t, RenderTargetInfo> mRenderTargets;
    struct SavedState
    {
        bool blend; llrvk::eBlendFactor sf, df; uint8_t colorMask;
        bool depthTest, depthWrite; llrvk::eCompareFunc depthFunc;
        bool scissor; int sx, sy, sw, sh; float lineWidth; float minAlpha;
        std::array<TexUnit, kNumTexUnits> units; uint32_t activeUnit;
        std::array<std::array<glm::mat4, kStackDepth>, NUM_MM> mat;
        std::array<int, NUM_MM> matIdx; eMatrixMode matrixMode;
        float color[4]; float uv[2];
    };
    struct OffFrame { uint32_t tex; SavedState saved; };
    std::vector<OffFrame> mOffStack;
    struct OffRun
    {
        uint32_t tex; size_t begin, end;
        bool clearColor; float clear[4]; bool clearDepth;
    };
    std::vector<OffRun> mOffRuns;
    bool mRecordingOffscreen = false;   // recordDraws: which draws to lay down
    void openOffRun(uint32_t tex, bool clearColor, const float* clear, bool clearDepth);
    void closeOffRun();
    void saveState(SavedState& s) const;
    void restoreState(const SavedState& s);
};

extern LLRenderVK gVK;
