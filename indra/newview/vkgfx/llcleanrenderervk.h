/**
 * @file llcleanrenderervk.h
 * @brief GPU-driven STATIC-OPAQUE world renderer for the Vulkan present path.
 *
 * This is the viewer-integrated promotion of the `indra/vkclean/` native
 * prototype (port plan phase 1). It renders the clean
 * pipeline's GPU-driven design on Vulkan:
 *
 *     persistent Instance SSBO  ->  compute cull (mirror of cleanCullC.glsl)
 *       ->  VkDrawIndexedIndirectCommand buckets  ->  vkCmdDrawIndexedIndirectCount
 *       ->  bindless per-material base-color textures.
 *
 * It draws directly into the acquired swapchain color image (with a
 * renderer-owned depth buffer) BEFORE the UI pass in LLVulkanContext::renderFrame
 * (world first, UI on top). It uses the REAL LLViewerCamera modelview/projection.
 *
 * DATA SOURCE. The GPU structs (Instance 192B / SlotInfo 32B / LodGroup 16B /
 * Material 96B) are byte-identical to the clean-pipeline C++ stores
 * (LLCleanInstanceStore / LLCleanMeshAsset / LLCleanMaterialStore). When those
 * stores are compiled into this build (LL_HAS_CLEAN_PIPELINE), syncFromStores()
 * mirrors their live CPU-side arrays into the VK buffers. When they are NOT
 * present on this branch, syncFromStores() builds the prototype's representative
 * scene so the end-to-end path (upload -> cull -> indirect draw -> swapchain)
 * is exercised and visible. Either way the wiring is identical; only the source
 * of the CPU arrays differs.
 *
 * SCOPE (this chunk): STATIC OPAQUE bucket, basic directional shading. Rigged /
 * alpha-blend / mask / water / sky / deferred lighting are follow-on chunks.
 *
 * GL default is entirely untouched: this file is only compiled/used behind the
 * RenderUseVulkan flag.
 */
#ifndef LL_LLCLEANRENDERERVK_H
#define LL_LLCLEANRENDERERVK_H

#include <functional>
#include "vk_device.h"
#include "vk_descpool.h"
#include "vk_stagering.h"   // TexStageRing
#include "vk_pipeline.h"   // GraphicsPipelineDesc (mLitDesc)
#include "llvulkancontext.h"   // LLVKWorldFrame / LLVKLight
#include "llcleantexencvk.h"   // GPU texture block compression (#263)
#include "llvrambudgetvk.h"    // one VRAM budget + soft pressure ladder (#405, #407)
#include "llcleandlssrrvk.h"   // #870 DLSS Ray Reconstruction (LLCleanDlssRRVK::Want)
#include "../llvkautotune.h"   // #422 auto-tuner: perf caps + measurement sample

#include <cstddef>   // offsetof (VKSSRInfo layout asserts, #814)
#include <cstdint>
#include <vector>
#include <string>
#include <array>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <atomic>
#include <future>
#include <thread>
#include <memory>
#include <mutex>
#include <condition_variable>
#include "lluuid.h"
#include "llpointer.h"
#include "llrefcount.h"
#include "../llavatarslots.h"
#include "../llcleanclustercache.h"   // LLClusterKey (cluster-owned mirror release)

class LLViewerFetchedTexture;
class LLImageRaw;
class LLVOAvatar;
class LLCleanRTVK;
class LLCleanGodRaysVK;   // god rays / volumetric sun shafts (#208)
class LLCleanVolFogVK;    // volumetric fog: froxel grid + local volumes (#718)
class LLCleanPartFogVK;   // particle fog: fog-like particle systems as dense local bricks (#1003)
class LLCleanDenoiserVK;  // diffuse / specular lighting denoiser (#85 phase 2)
class LLCleanVolCloudsVK; // volumetric sky cloud layer (#719)
class LLCleanWeatherVK;   // weather: precipitation, occlusion map, wet surfaces (#783)
class LLCleanPartLowResVK;   // reduced-resolution particles (RenderVKParticleLowRes)
class LLCleanMotionBlurVK;   // motion blur (#118)
class LLCleanSnapshotVK;     // live snapshots (#873)
class LLCleanPickVK;      // GPU raster cursor pick (#702) + particle pick (#481)
class LLCleanAudioRTVK;   // acoustics probe (#683): room + occlusion rays against the TLAS
class LLCelShadeVK;       // avatar mask + cel shading (#549 / #550)
struct GMDXSelOutlineVK;  // <GMDX> selection styles (gmdxseloutlinevk.cpp)
class LLCleanFSR3VK;      // AMD FSR 3.1 upscaler (RenderVKUpscaler 1-6)
class LLCleanClusterVK;   // cluster LOD (#642; RenderVKClusterLOD, llcleanclustervk.h)

// A column-major 4x4 (matches std140/std430 mat4 upload and glm::mat4 memory).
struct VKMat4 { float m[16]; };

// WORLD VERTEX INPUT (#573). The static world VS (clean_draw.vert) reads the QUANTIZED
// mega-VBO (llcleanqvertex.h, 20 B: int16 lattice position + flags, int16 lattice UV,
// octahedral snorm16 normal / tangent; decoded with the slot header via gl_BaseVertex);
// the rigged VS (clean_draw_rigged.vert) reads the pre-skinned COMPACT stream (mSkinVbo,
// #616, 20 B: fp32 pos3 + octahedral normal word + octahedral tangent / sign word, decoded
// in the VS -- clean_skinvert.glsl; the texcoord comes from the source QVertex). Every
// world pipeline sets the one matching its VS.
inline void vkWorldStaticVI(GraphicsPipelineDesc& d)
{
    d.bindings   = {{0, 20u, VK_VERTEX_INPUT_RATE_VERTEX}};
    d.attributes = {{0, 0, VK_FORMAT_R16G16B16A16_SINT, 0u},
                    {1, 0, VK_FORMAT_R16G16_SINT,       8u},
                    {2, 0, VK_FORMAT_R16G16_SNORM,      12u},
                    {3, 0, VK_FORMAT_R16G16_SNORM,      16u}};
}
inline void vkWorldRiggedVI(GraphicsPipelineDesc& d)
{
    d.bindings   = {{0, 20u, VK_VERTEX_INPUT_RATE_VERTEX}};
    d.attributes = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0u},
                    {2, 0, VK_FORMAT_R32_UINT,         12u},
                    {3, 0, VK_FORMAT_R32_UINT,         16u}};
}

// ===========================================================================
// PUSH-CONSTANT BUDGET (#238). Every push block is <= kVkPushConstantBudget (128 B),
// the Vulkan spec minimum maxPushConstantsSize (AMD Polaris / RX 470-590 exposes
// exactly that). One path on every device: no 256 B assumption, no fallback.
//
// Per-VIEW / per-PASS data (view_proj, camera, sun, fog, cascade / tile / probe-face /
// mirror matrices, cull planes, TAA reprojection ...) lives in the PARAM RING: one
// persistently mapped host-coherent buffer (BAR / device-local when the device offers
// it), one region per frame in flight, written by the CPU while recording -- before
// the submit, so no vkCmdUpdateBuffer and no barriers (a queue submit makes prior host
// writes to coherent memory visible). Shaders index it with a pushed, dynamically
// uniform slot. See LLCleanRendererVK::ParamRegion / pushDrawView / pushCullParams.
// ===========================================================================
static constexpr uint32_t kVkPushConstantBudget = 128;

// DRAW VIEW block: everything a world draw (VS + FS) reads per view / pass. Formerly the
// 240 B draw push constant; now ONE ENTRY of the draw-view ring (set 0 binding 36,
// clean_draw_view.glsl DrawView, std430 stride 256 since #555's origin), selected by
// VKDrawViewPush::view.
// Every producer still builds it exactly as before (field reuse included: water_fog
// = selection colour / particle pair id + pool size / rigged main eye / probe light
// list, inv_view_proj = particle billboard basis, sun_color.w = probe / mirror mode,
// ambient_color.w = hide-self) and hands it to pushDrawView(), which stores it (deduped
// against the last few entries) and pushes the 16 B index. sky_zenith.w stays 0 except on
// the main-view rigged opaque / mask passes, where 1 drops the cluster LOD path's avatars
// (#747, clean_draw_rigged.vert); the F8 alpha collapse mode moved to VKDrawViewPush::f8.
struct VKDrawPush
{
    float view_proj[16];   // 0   : column-major world->clip MVP (VK NDC)
    float sun_dir[4];      // 64  : .xyz world-space sun direction (normalized)
    float sun_color[4];    // 80  : sunlight RGB (.w unused)
    float ambient_color[4];// 96  : ambient RGB (.w unused)
    // Extra data the water branch needs (Fresnel view vector, sky reflection, wave anim).
    float cam_time[4];     // 112 : .xyz camera world pos, .w = time (seconds)
    float sky_horizon[4];  // 128 : sky gradient at horizon (water reflection)
    float sky_zenith[4];   // 144 : sky gradient at zenith. .w unused (0): the F8 alpha collapse
                           //       mode is VKDrawViewPush::f8 now (#238).
    // Water depth-fog: reconstruct the solid-scene world position behind each
    // water fragment from the sampled depth, measure the water column, and fog.
    float inv_view_proj[16]; // 160 : inverse of view_proj (clip->world), water FS
    float water_fog[4];      // 224 : .rgb LLEnvironment water fog color, .w density
    // CAMERA-RELATIVE ORIGIN (#555): view_proj maps (world - origin.xyz) to clip for the
    // world VS (clean_draw.vert) and the particle VS, which subtract it before the
    // projection. All zero (every copy of drawPushIn, probe faces, HUD) = a world-space
    // view_proj, exactly as before. The rigged VS ignores it (its view_proj is already
    // folded with the eye its pre-skinned vertices are relative to). vkSetDrawOrigin.
    float origin[4];         // 240 : .xyz origin, .w unused
};                         // 256 bytes: a draw-view ring entry, NOT a push constant (#238)
static_assert(sizeof(VKDrawPush) == 256, "VKDrawPush must match clean_draw_view.glsl DrawView (std430 stride 256)");

// The world draw push constant (#238): 16 B, identical range {VS|FS, 0, 16} on EVERY
// draw-family layout (mDrawLayout / mDrawLayoutRt / mWaterLayout / mPartDrawLayout(Rt))
// so a push survives binding a pipeline of another of these layouts, as before.
struct VKDrawViewPush
{
    uint32_t view;      // absolute draw-view ring index (frame slot base + local)
    uint32_t f8;        // F8 alpha collapse (main-view alpha pass only): 0 off, 1 the
                        // cull-FRONT draw, 2 the cull-BACK draw (clean_draw*.vert)
    uint32_t pad0;      // main-view LIT_ALPHA draws (pass 2): mVisReuseFlags (clean_draw_common.glsl
                        // VISR_*: split-lighting visibility reuse); the cluster draws reuse it otherwise
    uint32_t pad1;
};
static_assert(sizeof(VKDrawViewPush) == 16 && sizeof(VKDrawViewPush) <= kVkPushConstantBudget, "VKDrawViewPush");

// The deferred lighting kernel's embedded SPIR-V (llcleanspecvk.cpp vkDeferLightingSpv): pass =
// CLEAN_DEFER_PASS (0 uber .. 5 reflect sharp, 6 light grid, 7-9 the #827 feature split: base, local, tile
// classify; 10 = #831's shadow + tile classify), g11 = compact glow output, clock = the
// RenderVKShaderClockLighting twin. name = the array's symbol (module label, VKPipeStats).
struct VKDeferSpv { const uint32_t* code; size_t bytes; const char* name; };
VKDeferSpv vkDeferLightingSpv(uint32_t pass, bool g11, bool clock);

// Deferred lighting compute push (clean_deferred_light.comp DeferPush): the main view's
// draw-view ring index first (the shared lighting code reads pc = draw_views[view]), then
// the eye-relative inverse of the main view's jittered view-projection.
struct VKDeferPush
{
    uint32_t view;            // draw-view ring index of pass 1's main-view entry
    uint32_t flags;           // split: mDeferSplitFlags (DEFER_FLAG_*); + DEFER_FLAG_LIGHT_GRID / _FROXEL (uber too)
    uint32_t width, height;   // render extent
    float    inv_vp_rel[16];  // VK clip -> eye-relative world (column-major)
};
static_assert(sizeof(VKDeferPush) == 80 && sizeof(VKDeferPush) <= kVkPushConstantBudget, "VKDeferPush");
// RT reflection kernels' push (#859; clean_deferred_light.comp CLEAN_DEFER_PASS 11 / 12 / 14): the
// deferred push + rtr = (max ray distance m, 1 = blend layers, the max receiver distance m -- pass 12
// only; the split kernels read the RT tile list header's --, the sampler anisotropy).
struct VKRtReflPush
{
    VKDeferPush d;
    float       rtr[4];
    float       rtr2[4];   // (#859) size-class segments: bound (m), footprint px, fixed distance (m), 0
    uint32_t    rtr3[4];   // (#771 pass 15) [0] / [1] the sun exposure certificate table's device
                           // address (lo, hi; 0 = SXC off), [2] / [3] 0
};
static_assert(sizeof(VKRtReflPush) == 128 && sizeof(VKRtReflPush) <= kVkPushConstantBudget, "VKRtReflPush");

// Sky gradient full-screen pass push constant.
struct VKSkyPush
{
    float horizon[4];      // 0  : horizon-band color
    float zenith[4];       // 16 : zenith color
    float inv_view_proj[16]; // 32 : VK clip -> world (direction-based sky); with cam.w = 2
                             //      VK clip -> CAMERA-RELATIVE world (#555: altitude-proof rays)
    float cam[4];          // 96 : .xyz camera world pos, .w 1 = valid (world inverse),
                           //      2 = valid, inv_view_proj is camera-relative
};                         // 112 bytes
static_assert(sizeof(VKSkyPush) <= kVkPushConstantBudget, "VKSkyPush over the 128 B push budget");

// Per-pass cull constants (#238: a CULL-PARAMS ring entry, set binding 25 of mDslCull,
// clean_cull.comp CullParams, std430 stride 224 -- no longer a push constant). Built
// exactly as before and handed to pushCullParams(), which stores it once per dispatch
// group (phase zeroed: the phase travels in VKCullSlotPush) and pushes the slot.
struct VKCullPush
{
    float    frustum_planes[6][4]; // ax+by+cz+d>=0 => inside
    float    cam_pos[4];           // .xyz this-pass cam, .w lod_factor
    float    lod_cam_pos[4];       // .xyz main camera (LOD distance), .w px/unit at dist 1 (texel demand)
    uint32_t instance_count;
    uint32_t bucket_cap;
    uint32_t idx_valid;
    uint32_t hud_pass;
    uint32_t skin_count;       // skin palettes built this frame (valid skin_id < this)
    uint32_t skin_vert_cap;    // mSkinVbo capacity in vertices
    uint32_t work_cap;         // pre-skin work-list capacity in items
    uint32_t phase;      // stable-compaction phase (0 cull, 1 scan, 2 emit)
    uint32_t cascade;    // shadow pass (hud_pass 3): cascade index for the caster test
    uint32_t flags;      // bit0: hide the agent's own world attachments (world pass 0 only)
    // Reduced-tier skin cache (#31, pass 0): arena first vertex / capacity in mSkinVbo
    // (cache_cap 0 = off) and the current epoch (entries of other epochs are invalid).
    uint32_t cache_base = 0;
    uint32_t cache_cap = 0;
    uint32_t cache_epoch = 1;
    uint32_t cache_frame = 0;   // frame number (an entry not drawn cached last frame re-skins)
    // Reflection-probe capture (hud_pass 2) only; 0 in every other pass: the capture's
    // clips, the face's LOD scale (face / main-view px per unit) and the face index.
    float    probe_near = 0.f;
    float    probe_far = 0.f;
    float    probe_lod_scale = 0.f;
    uint32_t probe_face = 0;
    uint32_t vtx_valid = 0;    // vertex-buffer extent in vertices: the cull's OOB guard (#238; 0 = unchecked)
    uint32_t tex_demand = 0u;  // halfs: x texel demand bias, y sampler anisotropy (pass 0; 0 = 1, 1)
    uint32_t rig_off_cap = 0u; // #517: draws per off-screen rigged region (mRigOff; main cull; 0 = none)
    // CLUSTER LOD legacy exclusion (#745; main cull with kCullModeCluster, 0 elsewhere): the
    // device address of the cluster module's per-instance bitset and its length in words
    // (clean_cull.comp clusterOwned). Formerly the std430 tail padding (stride stays 224).
    uint32_t cluster_owned_lo = 0;
    uint32_t cluster_owned_hi = 0;
    uint32_t cluster_owned_words = 0;
};
static_assert(sizeof(VKCullPush) == 224, "VKCullPush must match clean_cull.comp CullParams (std430 stride 224)");

// The cull push constant (#238): ring slot + compaction phase.
struct VKCullSlotPush
{
    uint32_t slot;      // absolute cull-params ring index
    uint32_t phase;     // stable-compaction phase (0 cull, 1 scan, 2 emit; probe 3 / 4)
    uint32_t slot_b;    // multi-view probe cull: the scheduled face's params entry (~0u = none)
    uint32_t mode;      // LLCleanRendererVK::kCullMode* bits (clean_cull.comp CULL_MODE_*)
};
static_assert(sizeof(VKCullSlotPush) == 16 && sizeof(VKCullSlotPush) <= kVkPushConstantBudget, "VKCullSlotPush");

class LLCleanRendererVK
{
    friend class LLCelShadeVK;  // avatar mask + cel shading (#549 / #550)
    friend class LLCleanRTVK;   // ray tracing foundation reads the packed tables / buffers
    friend class LLCleanPickVK; // cursor pick reads cand / the vertex streams / the draw sets / the view ring
    friend class LLCleanAudioRTVK; // acoustics probe (#683): mCtx, mFrameSlot, the RT consumer set
public:
    // Mouselook, "Show me in Mouselook" off: skip the own avatar in the main view
    // (set per frame from LLVKWorldFrame::hide_self).
    bool mHideSelf = false;
    // !needsRenderHead(): hide only the own system-body head parts (instance bit 18) in
    // the main view (LLVKWorldFrame::hide_self_head).
    bool mHideSelfHead = false;
    // Main-view rigged draw push ambient_color.w (clean_draw_view.glsl): 1 hides the own
    // avatar, 0.25 only its system-body head parts, 0 nothing.
    float selfHideW() const { return mHideSelf ? 1.f : (mHideSelfHead ? 0.25f : 0.f); }
    // <GMDX> region map capture: this offscreen recording is orthographic (LLVKWorldFrame::ortho /
    // ortho_ppm, set by renderOffscreen; clean_cull.comp CULL_ORTHO = kCullOrtho).
    bool  mOrtho = false;
    float mOrthoPpm = 0.f;
    static constexpr uint32_t kCullOrtho = 131072u;
    // Build pipelines + descriptor layouts against `ctx`. `colorFormat` is the
    // swapchain format the world renders into. Returns false (logged) on failure;
    // the caller then simply skips world rendering (UI-only present).
    bool init(VkDeviceContext& ctx, VkFormat colorFormat);
    void shutdown();
    bool ready() const { return mReady; }
    // Async world-pipeline compile (init queues, background threads build): call each
    // frame; wait = block until done (shutdown, offscreen tests).
    void pollPipelines(bool wait = false);
    // The world batch is still compiling (graphics + init's compute pipelines). The login
    // shader gate (LLVulkanContext::worldPipelinesPending) ORs in every other gate job --
    // the ray-query lit variant and the first-use batches warmFirstUse queues on landing.
    bool pipelinesPending() const { return mPipelineJob != nullptr; }

    // Pull the latest CPU-side store contents (or the prototype scene) into the
    // VK buffers. Cheap when nothing changed (dirty tracking); re-uploads on
    // change. Call once per frame before recordWorld(). Safe to call every frame.
    void syncFromStores();
    // CPU overlap: start the store fill on a worker before the UI pass (renderFrame);
    // syncFromStores() joins it. Safe to skip: syncFromStores() then fills inline.
    void syncFromStoresBegin();
    void syncFromStoresPrestep();
    uint32_t mSelfHealMode = 1;   // C8 RenderVKSelfHealRate (0 off, 1 low, 2 legacy), latched in the prestep
    // Persistent fill worker (one thread for the renderer's lifetime; a std::async per
    // frame cost ~0.35 ms of thread creation on the main thread).
    struct FillWorker
    {
        std::thread             thread;
        std::mutex              mtx;
        std::condition_variable cv;
        bool                    job = false, done = false, quit = false, result = false;
    };
    FillWorker           mFillWorker;
    bool                 mFillPending = false;
    void fillWorkerStart();
    void fillWorkerStop();
    bool fillWorkerJoin();
    bool                 mSyncPrepped = false;
    std::vector<uint8_t> mSyncPrevMats;
    bool                 mSyncMatsMayChange = false;
    bool                 mSyncWasReal = false;

    // Record the world render into `colorView` (the acquired swapchain image
    // view, already in COLOR_ATTACHMENT_OPTIMAL) for the given extent, using the
    // supplied view-projection and cull constants. The color attachment is LOADed
    // (preserves the prior clear); depth is cleared here. Does compute cull +
    // indirect-count draw of the STATIC opaque bucket. No-op if not ready or no
    // instances.
    void recordWorld(VkCommandBuffer cmd, VkImageView colorView,
                     uint32_t width, uint32_t height,
                     const VKDrawPush& draw, const VKCullPush& cull,
                     const VKSkyPush& sky, const float* hud_view_proj = nullptr,
                     const int* hud_viewport = nullptr);

    // World-overlay hook (LL's depth-tested 3D overlays: property/ban lines, parcel
    // highlight, beacons, edit manipulators). Invoked inside recordWorld after the
    // world image is final (post AA/selection) and BEFORE the HUD pass clears the
    // depth, with the world depth in DEPTH_ATTACHMENT_OPTIMAL. Also lays down the
    // in-world text layer (name tags, hover text: #468) so the HUD pass covers it.
    // depthImage (#487): the same display-size world depth (sampleable), for the text
    // layer's per-label occlusion; the hook leaves it in DEPTH_ATTACHMENT_OPTIMAL.
    std::function<void(VkCommandBuffer, VkImageView color, VkImageView depth,
                       VkImage depthImage, uint32_t w, uint32_t h)> mWorldOverlayHook;
    VkFormat worldDepthFormat() const { return mDepthFormat; }

    // Local lights for this frame (called by the context before recordWorld).
    void setLights(const LLVKWorldFrame& wf);
    // Edit-mode selection outline for this frame (indirect draws of selected faces).
    void setSelection(const LLVKWorldFrame& wf);
    // Atmospheric haze parameters for this frame (SL distance fog).
    void setAtmosphere(const LLVKWorldFrame& wf);
    // Reflection probes for this frame: slot assignment + capture scheduling.
    void setProbes(const LLVKWorldFrame& wf);
    // Raster cursor pick (#702, llcleanpickvk.h): this frame's request (the cursor ray),
    // the newest completed answer, a ray answered now against the last frame (blocking,
    // #704).
    void setPick(const LLVKWorldFrame& wf);
    bool pickLatest(LLVKPickResult& out) const;
    void takeAuxPicks(std::vector<LLVKPickResult>& out);   // <SS:Skoomastream> sscombatray.cpp
    // Frame slot `slot`'s last frame completed (its fence signaled): harvest its pick answer now.
    void pickHarvestDone(uint32_t slot);
    bool pickNow(const LLVKPickRequest& req, LLVKPickResult& out);
    bool faceQuery(const std::vector<uint32_t>& ids, const LLVKRectQuery* rect, std::vector<LLVKFaceQueryResult>& out);
    bool pickAvailable() const;
    const char* pickFailure() const;
    // Particle cursor pick (#481; llcleanpickvk.h).
    bool partPickLatest(LLVKPartPickResult& out) const;
    bool partPickNow(const LLVKPickRequest& req, LLVKPartPickResult& out);
    // Acoustics probe (#683, llcleanaudiortvk.h): this frame's request (listener + sound
    // sources, agent space) and the newest completed answer.
    void setAcoustics(const LLVKWorldFrame& wf);
    bool acousticsLatest(LLVKAcousticsResult& out) const;
    bool acousticsBounceLatest(LLVKAcousticsBounce& out, uint32_t seen) const;   // #683 phase 1
    uint32_t acousticsBounceDispatched() const;   // #683 early gain: the request serial last dispatched
    const char* acousticsStandDownReason() const;   // nullptr = free to use RT (VRAM ladder / RT budget)
    // Why an OPTIONAL RT consumer (acoustics, RT reflections #859) must not use RT now (nullptr =
    // free to): the VRAM ladder's RT release rung, both RT shadow consumers shed, the RT budget.
    const char* optionalRtStandDownReason() const;
    // Sun/moon shadow cascades for this frame (matrices, splits, cull planes).
    void setShadows(const LLVKWorldFrame& wf);
    // Planar mirror (LL hero probe) for this frame: reflected view + probe box.
    void setMirror(const LLVKWorldFrame& wf);
    // Temporal history for this frame (#84): the app's reset request (teleport, see
    // LLVKWorldFrame::temporal_reset) + the camera-cut test against the last frame.
    void setTemporal(const LLVKWorldFrame& wf);
    // The acquired swapchain image behind recordWorld's colorView (SSR scene copy).
    void setSwapImage(VkImage img) { mSwapImage = img; }
    // Swapchain image bytes (driver-owned: not in our allocation tracker), set by the
    // context each frame so the VRAM accounting can name that part of the overhead (#407).
    void setSwapchainBytes(uint64_t b) { mSwapchainBytes = b; }
    // The VRAM picture (#405 / #407): budget, usage, per-category accounting, slack,
    // overhead, ladder state. Refreshed on the texture pass's budget checks.
    const LLVramStatus& vramStatus() const { return mVramStatus; }
    // TEXTURE INSPECTOR (#685; RenderVKTexInspector, shown by LLDebugText): the VK
    // residency state of the texture map `map` (0 base colour, 1 normal, 2 metal-rough /
    // ORM, 3 emissive) of the face (object, face) -- through the face's clean instance ->
    // its material -> the map's bindless slot -> the shared image -- with full / resident /
    // requested levels, why the resident level is below full, the storage format and the
    // texel density. uv_px (optional, 2 x vec2): sampled-UV change per screen pixel along
    // x and y at the cursor (the caller's CPU ray casts); null = estimate from the demand.
    // Multi-line; main thread.
    std::string texInspectText(const LLUUID& obj, S32 face, U32 map, const F32* uv_px = nullptr);
    // #684: one short residency line for a texture id (Texture Console columns); "" if not resident.
    std::string texResidencyBrief(const LLUUID& id) const;
    // <GMDX> A Vulkan image holds this texture now (either variant: plain or alpha-mask), so a face
    // using it can draw it rather than the white default (GMDXRezFx waits on it).
    bool texOnGpu(const LLUUID& id) const;

    // Number of instances currently uploaded (diagnostic).
    uint32_t instanceCount() const { return mInstanceCount; }

private:
    void ensureDepth(uint32_t width, uint32_t height);
    // Glow (LL per-face glow bloom): full-res glow attachment (resized with the
    // swapchain) + fixed 512^2 blur ping-pong; (re)writes the post descriptor sets.
    void ensureGlow(uint32_t width, uint32_t height);
    // Downsample + blur the glow target and composite it onto colorView (additive).
    void recordGlow(VkCommandBuffer cmd, VkImageView colorView, uint32_t width, uint32_t height);
    // <SS:Skoomastream> The glow chain's blur passes (E -> ... -> P1) and the additive
    // composite of P1 onto colorView, shared by the world glow and the HUD glow.
    void recordGlowBlur(VkCommandBuffer cmd);
    static F32 glowLdrFlag();   // <SS:Skoomastream> clean_glow.comp storeFormat flag
    void recordGlowCombine(VkCommandBuffer cmd, VkImageView colorView, uint32_t width, uint32_t height);
    // HUD glow (SSHudGlow): any HUD row whose material glows; the display-size HUD glow
    // target (false: none); extract + blur + composite it over the drawn HUD.
    bool hudHasGlow() const;
    bool ensureHudGlow(uint32_t width, uint32_t height);
    void recordHudGlow(VkCommandBuffer cmd, VkImageView colorView, uint32_t width, uint32_t height);
    // </SS:Skoomastream>
    // Ambient occlusion (RenderDeferredSSAO; #21, RTAO #86 writes the same target):
    // AO ping-pong images (full res, or half with RenderVKAOHalfRes) + the half-res upsample
    // meta, kept in GENERAL for good (draw set 0 binding 21 must always be valid). ensureAO
    // (re)creates them and the descriptors, and latches the half / upsample mode.
    void ensureAO(VkCommandBuffer cmd, uint32_t width, uint32_t height);
    // #648 (RenderVKAODepthMips): the AO linear view-depth MIP chain (clean_ao_depth.comp),
    // allocated only while AO + the depth pre-pass + the setting are on (freed otherwise),
    // and AO set binding 4 pointed at it (or at the depth as a placeholder).
    void ensureAoDepthMips(VkCommandBuffer cmd, uint32_t width, uint32_t height);
    void destroyAoDepthMips();
    void writeAoViewZBinding();
    // Builds the chain (one dispatch) when it exists and the setting is on, and latches
    // mAoMipsNow for this frame's AO stages; marks g_ao_depthmip. Depth must already be
    // DEPTH_READ_ONLY and visible to compute; leaves the chain visible to compute reads.
    void recordAoDepthMips(VkCommandBuffer cmd, uint32_t width, uint32_t height);
    // ---- RAY-TRACED SHADOW MASK (#195, clean_rt_shadow_mask.comp) ----
    // After the depth pre-pass, a compute pass traces the sun / moon and up to 4
    // ray-traced projectors per pixel; pass 1 then binds the PLAIN lit FS and reads the
    // mask (set 0 bindings 31-34). Off: 1x1 placeholders keep those bindings valid.
    void ensureRtMask(VkCommandBuffer cmd, uint32_t renderW, uint32_t renderH);
    // #664: the RT terminator triangle target (allocated while RT shadows trace and
    // RenderVKRTTerminatorSource = 1 on a device with fragment-shader barycentrics).
    void ensureTermTri(VkCommandBuffer cmd, uint32_t renderW, uint32_t renderH);
    void recordRtMask(VkCommandBuffer cmd, const VKDrawPush& drawPush, uint32_t renderW, uint32_t renderH);
    void destroyRtMaskImages();
    // SHADOW DENOISER (clean_rt_mask_common.glsl): trace (A) -> raw, temporal (B) ->
    // accumulated, spatial (C) -> mRtMaskSun / mRtMaskProj (what the lit FS reads).
    // mRtMaskProj is also A's raw projector estimate: raw is dead after B, C writes the
    // final slots, so one image serves both (17 images, 176 B per mask px with the #666 projector history).
    GpuImage         mRtMaskSun{}, mRtMaskProj{}, mRtMaskIds{}, mRtMaskDepth{};   // depth = r32f view depth
    GpuImage         mRtMaskGrad{}, mRtMaskHistGrad{};   // rgba16f d(vz)/dx, d(vz)/dy, flags: slot 0 / slot 1 (history)
    GpuImage         mRtMaskRawSun{};                                 // A: one-frame sun estimate (rgba8)
    GpuImage         mRtMaskAccSun{}, mRtMaskAccProj{}, mRtMaskMeta{}; // B: accumulated (rgba16f), lengths
    GpuImage         mRtMaskHist[5]{};        // last frame's acc sun/acc proj/ids/depth/meta (temporal)
    // #666 PROJECTOR SLOT HISTORY (rgba16ui, one per history slot like the rest: [0] slot 0,
    // [1] slot 1): per projector slot k (component k) its own history length and fast
    // (~3-frame) mean -- what the sun keeps in mRtMaskMeta. 16 B per mask px for both.
    GpuImage         mRtMaskProjMeta[2]{};
    bool             mRtMaskProjFull = false; // RenderVKRTShadowProjDenoise (latched in rtMaskSelectSet)
    // #666: keep the temporal plumbing (prev camera, motion vectors, previous instances)
    // alive for the mask's history without TAA / FSR (llcleanrtdenoisevk.cpp).
    bool rtMaskWantsTemporal() const;
    // F3a PING-PONG HISTORY (RenderVKRTMaskPingPong): two slots instead of per-frame copies.
    // Slot 0 = {mRtMaskAccSun, mRtMaskAccProj, mRtMaskMeta, mRtMaskIds, mRtMaskDepth},
    // slot 1 = mRtMaskHist[0, 1, 4, 2, 3]. Set p (mDsetRtMask[p]) writes slot p and samples
    // slot 1 - p; the lit FS reads slot p's ids / depth (set 0 bindings 33 / 34 hold both
    // slots, frame info rt_mask.x carries p). The copy path is set 0 + the copies into
    // mRtMaskHist (slot 1).
    uint32_t         mRtMaskSet = 0;          // set the mask records with this frame
    bool             mRtMaskPingPong = true;  // this frame (latched in rtMaskSelectSet)
    float            mRtMaskPrevJitter[2] = {0.f, 0.f};   // last frame's TAA jitter (render px)
    // #841: the mask's own noise / filter-rotation frame counter (push p0.x), advanced once per
    // live mask frame whatever the AA mode or temporal-plumbing state (mTaaFrame only moves
    // while mTemporalOn).
    uint32_t         mRtMaskFrame = 0;
    float            mRtMaskPrevSun[3] = {0.f, 0.f, 1.f};  // last mask frame's sun / moon direction (#85)
    bool             mRtMaskPrevSunOk = false;
    // #674 SUN DIAGNOSTIC (RenderVKRTShadowSunDiag = N frames; llcleanrtdenoisevk.cpp
    // rtSunDiagLog): per frame, the sun direction / camera / RT and mask state the shadow
    // passes see, with the frame-to-frame deltas. One run of N lines per setting change.
    void             rtSunDiagLog();
    uint32_t         mRtSunDiagLeft = 0, mRtSunDiagLatched = 0;
    float            mRtSunDiagPrevDir[3] = {0.f, 0.f, 0.f}, mRtSunDiagPrevEye[3] = {0.f, 0.f, 0.f};
    bool             mRtSunDiagPrevOk = false;
    bool             mRtMaskDenoise = true;   // RenderVKRTShadowDenoise (this frame)
    VkPipeline       mRtMaskTmpPipe = VK_NULL_HANDLE, mRtMaskSpPipe = VK_NULL_HANDLE;
    // Moving-caster shadow change -> mReact.R (pass 2b fullscreen, RenderVKRTShadowReactive;
    // clean_rt_mask_react.frag). mDrawLayout, so the pass's bound sets stay compatible.
    VkPipeline       mRtMaskReactPipe = VK_NULL_HANDLE;
    bool             mRtMaskRecorded = false; // the mask ran this frame
    bool             mRtMaskHistOk = false;   // ... and last frame (history is current)
    bool             mRtMaskProjPrev = true;  // the last mask frame had RT projector records (B5)
    bool             mRtMaskSunTraced = false; // the last mask frame traced the sun (mSunRtNow)
    uint32_t         mRtMaskW = 0, mRtMaskH = 0;
    // MASK DIAGNOSTIC (RenderVKRTMaskDiag): at most every 10 s, a mask frame copies an 8x8
    // grid of texels of each sun stage (raw A, accumulated B + meta, final C, depth plane),
    // projector slot 0 (final visibility + the slot ids)
    // and the frame info's shadow_params / rt_mask as the GPU saw them into a per-frame-
    // slot host buffer; the slot's next recording logs them with the host state, so one
    // line says which stage (gate, trace, temporal, spatial, consumer) lost the shadow.
    void rtMaskDiagRecord(VkCommandBuffer cmd);
    void rtMaskDiagHarvest();
    struct RtMaskDiagHost
    {
        bool pending = false;
        bool sunRtNow = false, litRt = false, histOk = false, deferNow = false;
        uint32_t set = 0, projRecords = 0, shadowRes = 0, w = 0, h = 0;
        float shadowParamW = 0.f, shadowClip3 = 0.f, lightDir[3] = {0.f, 0.f, 0.f};
    };
    GpuBuffer        mRtMaskDiagBuf[2]{};
    RtMaskDiagHost   mRtMaskDiag[2]{};
    double           mRtMaskDiagNext = 0.0;
    bool             mRtMaskDiagDefer = false;   // this frame's pass 1b lighting path (recordWorld)
    VkImageView      mRtMaskDepthView = VK_NULL_HANDLE;   // depth view the set was written with
    uint64_t         mRtMaskDepthGen = 0;                 // mDepthGen the set was written with
    VkDescriptorSetLayout mDslRtMask = VK_NULL_HANDLE;
    VkDescPool       mRtMaskPool;
    VkDescriptorSet  mDsetRtMask[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};   // per ping-pong slot (F3a)
    VkPipelineLayout mRtMaskLayout = VK_NULL_HANDLE;
    VkPipeline       mRtMaskPipe = VK_NULL_HANDLE;
    bool             mRtMaskOn = false;      // this frame (selectLitVariant)
    float            mRtMaskScale = 1.f;     // mask px per render px
    double           mRtMaskOomUntil = 0.0;  // #226: wall s before which a failed mask size is not retried
    uint64_t rtMaskBytesAt(uint32_t w, uint32_t h) const;   // the 17 mask images at w x h
    // #771 RT SUN SHADOW CACHE (RenderVKRTShadowCache; clean_rt_mask_cache.glsl): a GPU tile
    // classifier picks the mask blocks whose sun shadow can have changed (no history, edges /
    // penumbrae, movers, dynamic casters / static changes on the sun corridor, refresh slice);
    // the trace runs indirectly on those, the rest reuse the reprojected history (no sun rays).
    // Mask set layout 4: binding 0 the per-frame-slot host header + caster spheres
    // (mRtCacheParams), 1 the device work buffer (counts, indirect args, stats, block lists).
    void rtMaskCacheInit(VkDevice dev);
    void rtMaskCacheDestroy(VkDevice dev);
    void rtMaskCacheResize(VkCommandBuffer cmd, uint32_t w, uint32_t h);   // ensureRtMask, device idle
    void rtMaskCacheRecord(VkCommandBuffer cmd);   // after rtMaskPushFill, before the trace
    void rtMaskCacheHarvest();                     // this slot's stats readback -> the log line
    void rtMaskCacheNoteSpheres();                 // invalidateProjShadows: snapshot before the clear
    // Round 3: this frame's MOVING casters + static change spheres as SWEPT VOLUMES (box swept
    // along -L to its floor) + their projected hulls binned into screen cells, into the slot's
    // params block (base; nullptr = only run the trackers); counts into mRtCacheHost.
    uint32_t rtMaskCacheFootprints(uint8_t* base, uint32_t W, uint32_t H, bool* binned);
    bool rtMaskCacheWanted() const;                // setting on + the sun ray traced
    // RtCacheParams (clean_rt_mask_cache.glsl): 96 B header, rc_cast[4096] (32 B), rc_cells[4096]
    // (first, count), rc_bin[kRtCacheMaxBinned].
    static constexpr uint32_t kRtCacheMaxCasters = 4096;
    static constexpr uint32_t kRtCacheMaxCells = 4096;
    static constexpr uint32_t kRtCacheMaxBinned = 65536;
    static constexpr VkDeviceSize kRtCacheHeader = 96;
    static constexpr VkDeviceSize kRtCacheCellsOff = kRtCacheHeader + (VkDeviceSize)kRtCacheMaxCasters * 32;
    static constexpr VkDeviceSize kRtCacheBinOff = kRtCacheCellsOff + (VkDeviceSize)kRtCacheMaxCells * 8;
    static constexpr VkDeviceSize kRtCacheWorkHead = 176;       // rc_count + rc_args + rc_stats[32]
    VkDescriptorSetLayout mDslRtCache = VK_NULL_HANDLE;
    VkDescPool       mRtCachePool;
    VkDescriptorSet  mDsetRtCache[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};   // per frame slot (binding 0)
    GpuBuffer        mRtCacheParams[2]{};    // host-visible header + casters, per frame slot
    GpuBuffer        mRtCacheWork{};         // device: counts / args / stats / lists
    uint32_t         mRtCacheCap = 0;        // block capacity of each list in mRtCacheWork
    VkPipeline       mRtCachePipe = VK_NULL_HANDLE;
    bool             mRtCacheNow = false;    // this frame's trace runs from the block lists
    bool             mRtCacheWas = false;    // ... and last frame's (a switch forces every tile once)
    uint32_t         mRtCacheFrame = 0;      // refresh-slice phase
    uint32_t         mRtCacheCfgKey = 0;     // tile size / eps / min length: a change forces every tile
    float            mRtCacheSunStep = -1.f; // this mask frame's sun step (rad, rtMaskPushFill; -1 unknown)
    struct RtCacheSphere { float s[4]; uint32_t until; };
    std::vector<RtCacheSphere> mRtCacheChanged;   // static change spheres (agent space), kept N frames
    uint32_t         mRtCacheOverflowUntil = 0;   // the ingest's sphere list overflowed: force all until
    // Stats readback (every ~5 s, 2 frame slots) and the host's own counters for the log.
    GpuBuffer        mRtCacheStatsRb[2]{};
    bool             mRtCacheStatsPending[2] = {false, false};
    double           mRtCacheLogNext = 0.0;
    struct RtCacheHost
    {
        uint32_t frames = 0, forced = 0, dyn = 0, chg = 0, jump = 0, drift = 0, tiles = 0;
        // round 2: casters by kind (moving / still), footprints (on screen / culled), the
        // footprints' summed screen share, static-carry frames, plain-fallback frames
        uint32_t av_mov = 0, av_still = 0, am_mov = 0, am_still = 0, mv_mov = 0, mv_still = 0;
        uint32_t rects = 0, rects_off = 0, carry = 0, plain = 0;
        uint32_t sp_mov = 0, sp_still = 0, unbinned = 0;   // round 3: spinners, bin overflow frames
        uint64_t binned = 0;                                // bin entries
        double area = 0.0;
    };
    RtCacheHost      mRtCacheHost{};              // since the last readback
    RtCacheHost      mRtCacheHostRb[2]{};         // at the readback of each slot
    double           mRtCacheMs[3] = {-1.0, -1.0, -1.0};   // c_rtmask_cls / c_rtmask / c_rtmask_tmp+sp (5 s GPU timer window)
    // Round 2: per-character motion state (last frame's box / root) for the moving test and
    // the trailing footprint; the camera of the last cached frame (static carry); the per-
    // frame block counts read back for the plain fallback (traced share too high -> the
    // classifier is skipped, probed again every 30 frames).
    struct RtCacheAv { float box[6]; float pos[3]; float rot[4]; uint32_t seen = 0; };
    std::unordered_map<LLUUID, RtCacheAv> mRtCacheAv;
    float            mRtCachePrevVP[16] = {};
    double           mRtCachePrevCam[3] = {0.0, 0.0, 0.0};
    uint32_t         mRtCachePrevWH[2] = {0u, 0u};
    bool             mRtCachePrevCamOk = false;
    bool             mRtCachePrevProj = true;     // last frame had ray-traced projector records
    GpuBuffer        mRtCacheCountRb[2]{};
    bool             mRtCacheCountPending[2] = {false, false};
    bool             mRtCachePlain = false;
    uint32_t         mRtCachePlainProbe = 0, mRtCacheHighRun = 0;
    // ---- SUN EXPOSURE CERTIFICATES (SXC; #771 Phase 2, RenderVKSunCert; llcleansuncertvk.cpp,
    //      clean_sun_cert.glsl / .comp): a world-space cache of "this surface cannot see the sun for
    //      any sun direction along the next stretch of the sun's arc", so the merged RT shadow pass
    //      (generic core) skips the sun / moon sample where it is proven blocked, and keeps skipping
    //      while the sun moves. The table lives in one device-address buffer (pass 15's push rtr3.xy)
    //      -- no descriptor binding anywhere. ----
    struct SunCertState;                           // (llcleansuncertvk.cpp)
    SunCertState* mSunCert = nullptr;
    void     sunCertInit(VkDevice dev);            // pdnInit: the maintenance kernel
    void     sunCertDestroy();                     // pdnDestroy
    bool     sunCertWanted() const;                // the setting + the core kernel + the sun ray traced
    void     sunCertNoteSpheres();                 // invalidateProjShadows: this frame's static change spheres
    // recordProjStoch (merged frames on the core kernel), before pass 15: the header, the change
    // spheres, the maintenance kernel; the table's device address for the push (0: off this frame)
    uint64_t sunCertRecord(VkCommandBuffer cmd, bool onAsync, bool coreKernel);
    void     sunCertHarvest();                     // this slot's stats readback -> the VKSunCert line
    void     sunCertGpuWindow(double k);           // the GPU timer window: c_sxc + c_defer_shadowmerge ms
    // SSAO + 2-pass depth-aware blur from the pre-pass depth into mAoA. Called between
    // the pre-pass and the lit pass; depth goes ATTACHMENT -> READ_ONLY -> ATTACHMENT.
    // depthHeld (deferred AO after the G-buffer, RenderVKPrepassDepthOnlyDeferred): the caller
    // already holds depth in DEPTH_READ_ONLY (the lighting compute reads it alongside) and
    // returns it to the attachment layout itself -- no depth transition here.
    void recordAO(VkCommandBuffer cmd, uint32_t width, uint32_t height, bool depthHeld = false);
    // Occlusion culling (#167, RenderVKOcclusionCull): Hi-Z from the pre-pass depth, then
    // drop the main view's static opaque/mask draws that are hidden behind it (pass 1
    // only; the pre-pass already drew them). Ends/does not own the rendering scope.
    void recordOcclusion(VkCommandBuffer cmd, const float* view_proj);
    bool ensureOcclusion();
    // hiz = false: only the single-phase scratch (the Hi-Z stays for the two-phase path).
    // midFrame: called while a frame is being recorded -- the Hi-Z image / views are retired
    // (freed with this frame's slot), as ensureHiz does, not destroyed.
    void destroyOcclusionResources(bool hiz = true, bool midFrame = false);
    // The Hi-Z image + its per-level sets (shared by both occlusion paths).
    bool ensureHiz();
    // #697: Hi-Z / occlusion scratch allocate SOFT; a refusal disables occlusion culling
    // (the plain cull runs) until this LLFrameTimer time -- no device wait per frame.
    double mOcclRetryAt = 0.0;
    void occlRefused(uint64_t bytes);
    // Hi-Z build from mDepth, which the caller has put in DEPTH_READ_ONLY_OPTIMAL.
    void recordHizBuild(VkCommandBuffer cmd);
    // TWO-PHASE OCCLUSION (RenderVKOcclusion2Phase; clean_occl2.comp). Inside pass 1a:
    // split (P1 = static draws visible last frame) before the pre-pass, then after P1's
    // depth: Hi-Z + test (in-place compaction of the static + alpha lists, P2 = revealed
    // draws, visibility stamps). recordOcclusion2Test ends with mDepth back in
    // DEPTH_ATTACHMENT_OPTIMAL; both run outside any rendering scope.
    bool ensureOcclusion2();
    void destroyOcclusion2Resources();
    void recordOcclusion2Seed(VkCommandBuffer cmd);    // camera-cut frame: vis = frustum set
    void recordOcclusion2Split(VkCommandBuffer cmd);
    void recordOcclusion2Test(VkCommandBuffer cmd, const float* view_proj);

    // POST-PROCESS AA (LL RenderFSAAType: 0 none, 1 FXAA, 2 SMAA 1x, 3 TAA = recordTAA; RenderFSAASamples
    // 0..3 = Low/Medium/High/Ultra preset). When on, tonemap + glow render into
    // mAAColor (gamma-encoded LDR, swapchain format) and the AA pass resolves it into
    // the swapchain, BEFORE selection/HUD/UI and the world-only snapshot capture.
    GpuImage  mAAColor{}, mSmaaEdge{}, mSmaaBlend{}, mSmaaArea{}, mSmaaSearch{};
    uint32_t  mAAW = 0, mAAH = 0;
    VkSampler mAASampler = VK_NULL_HANDLE;               // linear, clamp
    VkDescriptorSetLayout mDslAA = VK_NULL_HANDLE;       // b0..b2 combined image samplers
    VkDescPool            mAAPool;   // layout-sized + accounted (vk_descpool.h)
    VkDescriptorSet mDsetAAColor = VK_NULL_HANDLE;       // FXAA / SMAA edge: b0 = colour
    VkDescriptorSet mDsetSmaaWeights = VK_NULL_HANDLE;   // b0 edges, b1 area, b2 search
    VkDescriptorSet mDsetSmaaNeighbor = VK_NULL_HANDLE;  // b0 colour, b1 weights
    VkPipelineLayout mAALayout = VK_NULL_HANDLE;         // push: vec4 (all AA passes)
    VkPipeline mFxaaPipe[4] = {};
    VkPipeline mSmaaEdgePipe[4] = {}, mSmaaWeightPipe[4] = {};
    VkPipeline mSmaaNeighborPipe = VK_NULL_HANDLE;
    bool mAAInitFailed = false;
    // FIRST-USE post pipelines (llcleanrenderervk.cpp warmPostPipelines): AA presets, bloom /
    // cross / lens dirt / flare and final, ONE background batch; staged, installed when done.
    static constexpr uint32_t kPostStageMax = 24;
    PipelineJobSlot mPostJob;
    VkPipeline   mPostStage[kPostStageMax] = {};
    VkPipeline*  mPostDest[kPostStageMax] = {};
    uint32_t     mPostStageN = 0;
    bool         mPostCollecting = false;   // initAA / initBloom / initFinal queue (postInto)
    bool         mPostWarmStarted = false;
    bool         mFirstUseWarmed = false;
    void postInto(VkDevice dev, const GraphicsPipelineDesc& d, VkPipeline* dest, const char* name);
    void warmPostPipelines();
    void postPipelinesLanded();
    bool postPipelinesReady();                 // false while the batch compiles
    void warmFirstUse();                       // finalizePipelines: queue every first-use pipeline
    bool ensureClusterModule();                // the cluster LOD module (clusterTick / warmFirstUse)
    void initAA();                                       // pipelines + LUTs (once)
    bool ensureAA(uint32_t width, uint32_t height);      // window-sized targets
    void freeAATargets();                                // post AA off (TAA / FSR / none): release them
    // Resolve mAAColor -> colorView with FXAA (type 1) or SMAA (type 2).
    void recordAA(VkCommandBuffer cmd, VkImageView colorView, uint32_t width, uint32_t height,
                  uint32_t type, uint32_t quality);
    // COLOUR GRADE (Lightbox, upstream Alchemy colorCorrectF / colorGradeUtilF) in the
    // tonemap pass: set 1 = b0 grade UBO, b1 3D LUT (RenderColorGradeLUT), b2 tone-curve
    // row (RenderColorGradeCurve*, baked by SLToneCurveSet).
    VkDescriptorSetLayout mDslGrade = VK_NULL_HANDLE;
    VkDescPool            mGradePool;
    VkDescriptorSet       mDsetGrade = VK_NULL_HANDLE;
    GpuBuffer mGradeUBO{};
    GpuImage  mGradeLut{}, mGradeCurve{};
    VkSampler mGradeSampler = VK_NULL_HANDLE;   // linear, clamp
    std::string mGradeLutName;                  // loaded RenderColorGradeLUT ("" = none)
    bool      mGradeLutLoaded = false;          // a real LUT (not the 1^3 placeholder)
    float     mGradeLutSize[4] = {1.f, 0.f, 0.f, 0.f};
    bool      mGradeCurveDirty = true, mGradeCurveIdentity = true;
    bool      mGradeSignalsHooked = false;
    void initGrade();
    void updateGradeResources();                // LUT / curve (re)uploads; waits idle on change
    void recordGradeUBO(VkCommandBuffer cmd);   // this frame's grade constants
    // TONEMAP / GRADE LUT (#820, RenderVKTonemapLut; clean_tonemap_common.glsl): the tonemap FS's
    // whole post-exposure tail (grade linear stage, tonemapper + mix, split tone, sRGB, legacy
    // gamma, display grade with the user 3D LUT and tone curves) baked into an N^3 RGBA16F table
    // over a log shaper of the exposed colour (clean_tonemap_lut.comp), re-baked only when its
    // inputs change (mTmLutKey), read by a tetrahedral lookup. Set 1 binding 8 of the grade set
    // (a 1^3 placeholder until first use); the image lives in GENERAL. One image, <= 2 MB (N 64).
    float     mGradeKey[56] = {};               // this frame's grade constants (VKGradeUBO up to ca0)
    uint32_t  mGradeResGen = 0;                 // bumps when the grade LUT / tone-curve images change
    VkDescriptorSetLayout mDslTmLut = VK_NULL_HANDLE;   // b0: the LUT as a storage image (the bake)
    VkDescPool            mTmLutPool;
    VkDescriptorSet       mDsetTmLut = VK_NULL_HANDLE;
    VkPipelineLayout      mTmLutLayout = VK_NULL_HANDLE;   // {mDslTmLut, mDslGrade} + the tonemap push
    VkPipeline            mTmLutPipe = VK_NULL_HANDLE;
    GpuImage  mTmLut{};
    uint32_t  mTmLutN = 0;                      // mTmLut's edge (1 = the placeholder)
    bool      mTmLutValid = false;              // mTmLut holds the tail of mTmLutKey
    float     mTmLutKey[68] = {};
    bool      mTmLutLogged = false;
    bool      ensureTmLut(VkCommandBuffer cmd, uint32_t n);   // (re)creates mTmLut at edge n (+ its bindings)
    // Bakes the LUT when wanted and stale; sets tp[15] (the FS push's p2.w) = N, or 0 = direct tail.
    void      recordTonemapLut(VkCommandBuffer cmd, float* tp);
    // MOTION BLUR (#118, RenderVKMotionBlur; llcleanmotionblurvk.{h,cpp}): a tile-based
    // reconstruction filter on the render-size scene after pass 2, before the TAA / FSR
    // resolve, from the pre-pass motion target (temporal #84: camera + object + rigged
    // motion). Wanted keeps the temporal plumbing (ensureTemporal) alive with TAA off.
    bool motionBlurWanted() const;
    // Records the effect's passes (GPU sub-marks p_mblur_tile / p_mblur_nmax / p_mblur);
    // frees it when off. true = the scene was blurred.
    bool recordMotionBlur(VkCommandBuffer cmd, uint32_t rw, uint32_t rh, bool mvNow);

    // HDR BLOOM (Lightbox / upstream generateBloomHDR, #113): extract from the HDR
    // scene + per-face glow into a half-res-chain pyramid, composited pre-exposure in
    // the tonemap pass. Replaces the LDR glow while active (RenderHDREnabled and
    // RenderBloomStrength > 0), as upstream.
    static constexpr uint32_t BLOOM_MAX_MIPS = 7;
    GpuImage  mBloomMip[BLOOM_MAX_MIPS]{};
    uint32_t  mBloomCount = 0, mBloomW = 0, mBloomH = 0, mBloomKey = 0;
    VkDescriptorSetLayout mDslBloom = VK_NULL_HANDLE;   // b0 source, b1 glow
    VkDescPool            mBloomPool;
    VkDescriptorSet       mDsetBloomExtract = VK_NULL_HANDLE;
    VkDescriptorSet       mDsetBloomMip[BLOOM_MAX_MIPS]{};
    VkPipelineLayout      mBloomLayout = VK_NULL_HANDLE;
    VkPipeline            mBloomPipe = VK_NULL_HANDLE, mBloomUpPipe = VK_NULL_HANDLE;
    bool                  mBloomInitFailed = false;
    bool                  mBloomActive = false;   // this frame (tonemap composites it)
    // Cross filter (streaks from the bloom extract, #113): scratch A/B + accumulator
    // at half the pyramid base.
    GpuImage  mCrossA{}, mCrossB{}, mCrossAcc{};
    VkDescriptorSet mDsetCrossSrc = VK_NULL_HANDLE, mDsetCrossA = VK_NULL_HANDLE, mDsetCrossB = VK_NULL_HANDLE;
    VkPipeline mCrossPipe = VK_NULL_HANDLE, mCrossAddPipe = VK_NULL_HANDLE;
    bool      mCrossActive = false;
    // Lens dirt: generated R8 plate, regenerated when its parameters change.
    GpuImage  mLensDirt{};
    VkPipeline mDirtPipe = VK_NULL_HANDLE;
    float     mDirtKey[10] = {};
    bool      mDirtActive = false;
    // Lens flare: 2x1 sun state, ping-ponged per frame.
    GpuImage  mFlareState[2]{};
    uint32_t  mFlareCur = 0;
    bool      mFlareStateInit = false, mFlareActive = false, mFlareSunUp = true;
    float     mFlareSunUV[2] = {0.5f, 0.5f};
    VkDescriptorSetLayout mDslFlare = VK_NULL_HANDLE;   // b0 scene, b1 depth, b2 previous state
    VkDescPool            mFlarePool;
    VkDescriptorSet       mDsetFlare[2]{};
    VkPipelineLayout      mFlareLayout = VK_NULL_HANDLE;
    VkPipeline            mFlarePipe = VK_NULL_HANDLE;
    VkImageView           mFlareDepthView = VK_NULL_HANDLE, mFlareHdrView = VK_NULL_HANDLE;
    void  recordCrossFilter(VkCommandBuffer cmd);
    void  updateLensDirt(VkCommandBuffer cmd, uint32_t width, uint32_t height);
    void  recordFlareState(VkCommandBuffer cmd, uint32_t width, uint32_t height);
    void  writeGradeLensBindings();   // grade set b4..b6 (cross / dirt / flare state or placeholders)
    bool  initBloom();
    bool  ensureBloom(uint32_t width, uint32_t height);
    void  recordBloom(VkCommandBuffer cmd);

    // FINAL EFFECTS (Lightbox: lens distortion, vignette, CVD, grain, dither, preview):
    // when active, the chain (tonemap .. AA) writes mFinalColor and clean_final.frag
    // resolves it into the swapchain.
    GpuImage  mFinalColor{};
    uint32_t  mFinalW = 0, mFinalH = 0;
    VkDescriptorSetLayout mDslFinal = VK_NULL_HANDLE;   // b0 sampler, b1 UBO
    VkDescPool            mFinalPool;
    VkDescriptorSet       mDsetFinal = VK_NULL_HANDLE;
    GpuBuffer             mFinalUBO{};
    VkPipelineLayout      mFinalLayout = VK_NULL_HANDLE;
    VkPipeline            mFinalPipe = VK_NULL_HANDLE;
    bool                  mFinalInitFailed = false;
    float                 mLensFitKey[10] = {};   // lens distortion auto-fit cache (inputs)
    float                 mLensFitScale = 1.f;
    bool  initFinal();
    bool  ensureFinal(uint32_t width, uint32_t height);
    // Fills the UBO (outside rendering); false when no effect is active this frame.
    // force: run it regardless (the RLVa sphere passes end in mFinalColor).
    bool  prepareFinal(VkCommandBuffer cmd, uint32_t width, uint32_t height, bool force = false);
    void  recordFinal(VkCommandBuffer cmd, VkImageView dstView, uint32_t width, uint32_t height);

    // RLVa @SETSPHERE (vision restriction, clean_rlv_sphere.frag): with any effect active
    // the final pass is forced on and the chain's last stage plus the sphere passes
    // ping-pong between mSphereTmp and mFinalColor, the last pass landing in mFinalColor.
    // Nothing is allocated or recorded while no effect is active.
    LLVKWorldFrame::RlvSphere mRlvSpheres[LLVKWorldFrame::kMaxRlvSpheres] = {};
    uint32_t  mRlvSphereCount = 0;
    GpuImage  mSphereTmp{};
    uint32_t  mSphereW = 0, mSphereH = 0;
    VkDescriptorSetLayout mDslSphere = VK_NULL_HANDLE;   // b0 colour, b1 world depth
    VkDescPool            mSpherePool;
    VkDescriptorSet       mDsetSphere[2]{};   // [0] reads mFinalColor, [1] reads mSphereTmp
    VkPipelineLayout      mSphereLayout = VK_NULL_HANDLE;
    VkPipeline            mSpherePipe = VK_NULL_HANDLE;
    bool                  mSphereInitFailed = false;
    VkImageView           mSphereFinalView = VK_NULL_HANDLE, mSphereTmpView = VK_NULL_HANDLE, mSphereDepthView = VK_NULL_HANDLE;
    uint64_t              mSphereDepthGen = 0;
    uint32_t  rlvSpherePassCount() const;
    bool  initSphere();
    bool  ensureSphere(uint32_t width, uint32_t height);
    void  freeSphereTargets();
    void  recordSphere(VkCommandBuffer cmd, uint32_t width, uint32_t height);

    // LIGHTBOX READBACKS (LLPipeline hooks): reference still (copy of the final pass
    // input, sampled at clean_final binding 2), the scopes' point-decimated sample of
    // that same frame, and the scene-pixel probe (one linear texel of the HDR scene).
    // Readbacks are collected LB_READBACK_LAG frames after issue, never stalling.
    static constexpr uint64_t LB_READBACK_LAG = 3;
    GpuImage  mRefStill{};
    GpuImage  mScopeImg{};
    GpuBuffer mScopeBuf{};
    uint32_t  mScopeW = 0, mScopeH = 0;
    bool      mScopeInFlight = false;
    uint64_t  mScopeIssueFrame = 0;
    double    mScopeNext = 0.0;
    GpuBuffer mPixBuf{};
    bool      mPixInFlight = false;
    uint64_t  mPixIssueFrame = 0;
    uint64_t  mLbFrame = 0;   // counts on-screen recordWorld calls
    void  writeFinalStillBinding();
    void  serviceLightboxReadbacks();
    void  recordScenePixel(VkCommandBuffer cmd, uint32_t width, uint32_t height);
    void  recordStillAndScope(VkCommandBuffer cmd, uint32_t width, uint32_t height);

    // DEPTH OF FIELD (RenderDepthOfField, Camera* lens + RenderBokeh* settings), on the
    // linear HDR scene ahead of bloom and tonemap (upstream Alchemy): cofF -> mDofCoC
    // (colour + CoC), the bokeh gather -> mDofHalf (CameraDoFResScale), and the combine
    // writes mHdr's colour in place.
    GpuImage  mDofCoC{}, mDofHalf{};
    GpuBuffer mDofBokehUBO{};
    VkPipeline mDofGatherShapedPipe = VK_NULL_HANDLE;
    VkImageView mDofHdrView = VK_NULL_HANDLE;            // HDR view mDsetDofCof points at
    uint32_t  mDofW = 0, mDofH = 0, mDofHalfW = 0, mDofHalfH = 0;
    VkSampler mDofPointSampler = VK_NULL_HANDLE;         // nearest, clamp
    VkDescriptorSetLayout mDslDof = VK_NULL_HANDLE;      // b0, b1 combined image samplers, b2 bokeh UBO
    VkDescPool            mDofPool;
    VkDescriptorSet mDsetDofCof = VK_NULL_HANDLE;        // b0 colour, b1 depth
    VkDescriptorSet mDsetDofGather = VK_NULL_HANDLE;     // b0 CoC
    VkDescriptorSet mDsetDofCombine = VK_NULL_HANDLE;    // b0 half-res DoF, b1 CoC
    VkPipelineLayout mDofLayout = VK_NULL_HANDLE;        // push: DofPush (112 B)
    VkPipeline mDofCofPipe = VK_NULL_HANDLE, mDofGatherPipe = VK_NULL_HANDLE, mDofCombinePipe = VK_NULL_HANDLE;
    VkImageView mDofDepthView = VK_NULL_HANDLE;          // depth view mDsetDofCof points at
    bool  mDofInitFailed = false;
    bool  mDofEnabled = false;                           // this frame (LLVKWorldFrame)
    float mDofLens[4] = {}, mDofParams[4] = {};
    void initDoF();
    bool ensureDoF(uint32_t width, uint32_t height);
    void freeDoFTargets();   // depth of field off: release the display-size CoC / half images
    // mHdr (SHADER_READ_ONLY) in place. Depth must be in DEPTH_ATTACHMENT_OPTIMAL; it
    // is sampled and returned to that layout. Outside rendering (updates the UBO).
    void recordDoF(VkCommandBuffer cmd, uint32_t width, uint32_t height);
    void buildBuffers();          // (re)create/resize SSBOs from mCpu* arrays
    void writeDescriptors();
    bool ensureRigCache(uint32_t inst_cap);   // reduced-tier skin cache state (#31)
    void writeTextureDescriptors(uint32_t set); // diff-write the bindless array into a per-frame set
    // Per-frame texture work, recorded at the top of recordWorld (after acquire waited
    // this frame slot's fence): free what this slot retired last time around, wait the
    // worker staging fills, record the queued uploads/downgrades into THIS frame's
    // command buffer, and diff-write the slot's bindless set.
    void beginTextureFrame(VkCommandBuffer cmd);
    void writePreskinDescriptors(); // (re)bind the rigged pre-skin compute set
    void writeSkinPaletteDescriptors(); // (re)bind the palette compute set
    void checkSkinPaletteCompare();     // RenderVKSkinPaletteCompare: diff a landed readback
    void destroyBuffers();

    // ---- RIGGED (opaque) --------------------------------------------------
    // Per-frame prep for the rigged opaque bucket: refresh the skin store's CPU
    // palettes (VK mode), build the pre-skin work list from the mirrored rigged
    // instances, and upload both to the host-visible palette/work SSBOs. Sets
    // mRiggedWorkCount (0 => the rigged pass is skipped this frame). No-op unless
    // the clean stores are compiled in. Called at the top of recordWorld.
    void buildRiggedFrame();
    // Ensure the per-frame palette / work SSBOs are at least the requested sizes;
    // (re)creates them (host-visible, persistently mapped) on growth. Returns true
    // if any buffer was (re)created (so the pre-skin descriptor set must be rewritten).
    bool ensureRiggedBuffers(uint32_t palette_count, uint32_t work_count, uint32_t skin_verts,
                             uint32_t cache_verts, bool allow_shrink);
    // Byte ceiling for one rigged buffer: min(1/4 of the device-local heap budget,
    // maxStorageBufferRange). (G-2)
    uint64_t rigBufferBudgetBytes() const;
    uint32_t mRigGrowRetryFrame = 0;   // a failed soft grow: no new attempt before this frame
    uint32_t mRigGrowFailStreak = 0;   // consecutive failed / skipped rig grows (backoff exponent)
    // FRESH RIGGED BUFFERS ARE ZEROED (transient-corruption hardening). A buffer re-created by
    // ensureRiggedBuffers starts with whatever the allocation held (often a just-freed
    // buffer's or texture's bytes). Every consumer is supposed to write before it reads, but
    // a single read of an unwritten range (an indirect command past this frame's count, a
    // skinned range a stale draw names, a palette row past skin_count) then draws garbage --
    // whole-screen noise / exploding triangles -- instead of nothing. A zeroed DrawCmd is
    // an empty draw, a zeroed skinned vertex / palette collapses to a degenerate point. Bits
    // of kRigFill* for the buffers re-created since the last recording; recordRigFreshFills
    // zeroes them in the frame's command buffer before any rigged pass (after the device
    // wait the re-create did, so nothing older can touch them). Bit set -> one log line.
    enum : uint32_t
    {
        kRigFillPalette = 1u << 0, kRigFillWork = 1u << 1, kRigFillProbe = 1u << 2,
        kRigFillOff = 1u << 3, kRigFillShadow = 1u << 4, kRigFillSkin = 1u << 5,
    };
    uint32_t mRigFillPending = 0;
    uint32_t mRigFillFrame = ~0u;      // gFrameCount of the last recorded fill (no pipelined front end then)
    uint32_t mRigBufGen = 0;           // bumped on every rigged buffer re-create (diagnostics)
    uint32_t mRigBufGenFrame = 0;      // gFrameCount of the last bump
    void recordRigFreshFills(VkCommandBuffer cmd);
    // #559 RIGGED CAPACITY: one combined budget for every rigged buffer (+ TAA copies),
    // sized from the cull's visible demand; LOD pressure on far avatars past it.
    struct RigBudget
    {
        uint64_t bytes = 0;        // the whole rigged share of VRAM
        uint64_t verts = 0;        // skinned vertices it leaves (per-frame ranges + cache arena)
        uint64_t items = 0;        // work items it grants
        uint32_t vert_bytes = 0;   // per skinned vertex, TAA history included
        uint32_t item_bytes = 0;   // per work item (+ probe / off-screen draw lists)
        float    scale = 1.f;      // the VRAM ladder's reduction (1 = none)
    };
    RigBudget rigBudget(uint32_t skins, uint64_t items) const;
    void      rigPressure(const RigBudget& bud, uint64_t gpu_v, bool dropped);
    // #561 SCREEN-SIZE RIGGED LOD (the same controller, rigPressure). #613: two triggers.
    //  * memory: the VRAM ladder's rigged rungs (LLVramBudgetVK::STEP_RIGGED, rigLadderLevel):
    //    decimated steps for every unprotected avatar (mRigSlodMem at the top), then
    //    creator-LOD caps on far tiers as far as the rigged budget needs (mRigPressure),
    //    then a smaller rigged budget -- in the ladder's own order, pacing and recovery.
    //  * perf: the measured GPU time of the rigged work (rigGpuTimeBegin: pre-skin, weld,
    //    rigged raster) over RenderVKRiggedGpuBudgetMs on a GPU-bound frame slower than
    //    RenderVKRiggedLodTargetFPS. CPU-bound frames never engage it. It first raises the
    //    pixel tolerance level A (0..kRigSlodMax) of the decimated steps
    //    (LLCleanRigDecimator), and only past the top of that -- decimation done, still
    //    over -- the creator-LOD pressure (mRigPerfP; RenderVKRiggedPerfCreatorLOD 0 = never).
    //    Released in reverse order with hysteresis.
    // #734 SIZE FLOOR: no pressure (perf or memory) ever moves an avatar to a creator LOD
    // finer-than-LL would not: the cap is floored at LL's own rigged LOD for that avatar at
    // a lod-factor scale between RenderVKReducedAvatarLODScale (small / far avatars) and 1
    // (large on screen / near: LL's own choice, i.e. what every viewer shows) by the
    // avatar's protection weight (mRigProtW). Dense avatars' creator LODs 0-1 often leave
    // whole sections empty; at a size where LL itself would not use them they must not be
    // drawn. (mRigCapMin)
    // rigVertBudget (RenderVKRiggedLodVertBudgetK, from the VRAM budget as the GPU-tier
    // proxy) now only sizes the protected avatars' vertex budget (rigAssignTiers).
    uint64_t  rigVertBudget() const;
    static constexpr uint32_t kRigSlodMax = 4u;   // tolerance 0.5 / 1 / 2 / 4 px
    // Highest main-view level (8 px) with the small-on-screen bonus: the decimator's
    // unlocked-border steps carry their error x16 (LLCleanRigDecimator::kUnlockedErrScale),
    // so their cracks stay under 1 px up to this tolerance.
    static constexpr uint32_t kRigSlodMainMax = 5u;
    uint32_t  rigSizeBonus(uint32_t slot) const;
    void      rigUpdateSizeBonus(uint32_t slot, float px);
    uint8_t   mRigSizeBon[kAvatarSlotCap] = {};   // per slot: small-on-screen tolerance bonus (0..2, held)
    uint32_t  mRigSlod = 0;            // effective tolerance level A = max(perf, memory) (0 = off)
    uint32_t  mRigSlodPerf = 0;        // ... what the perf budget asks for
    uint32_t  mRigSlodMem = 0;         // ... what the VRAM ladder's rigged rungs ask for (#613)
    double    mRigSlodMemAt = 0.0;     // last memory-driven A change (s)
    // #613: the VRAM ladder's rigged level (LLVramBudgetVK::RIG_*): the copy of the rigged
    // rungs for this card's size (rigSmallCard: capacity <= kRigSmallCardBytes -> late).
    uint32_t  rigLadderLevel() const;
    bool      rigSmallCard() const;
    uint64_t  rigHeldBytes() const;    // bytes the rigged buffers hold now
    uint32_t  mRigLadderLvl = 0;       // last level rigPressure applied (transition log)
    uint32_t  mRigPerfP = 0;           // creator-LOD pressure from the perf budget (0..kRigPerfPMax)
    double    mRigSlodAt = 0.0;        // last A change (s)
    double    mRigPerfPAt = 0.0;       // last perf-pressure change (s)
    double    mRigOverSince = 0.0;     // start of the current over-budget run (0 = not over)
    double    mRigUnderSince = 0.0;    // start of the current comfortably-under run (0 = not under)
    double    mRigReleaseAt = 0.0;     // last release step (flip-flop backoff)
    uint32_t  mRigFlip = 0;            // raise soon after a release -> longer dwell (0..4)
    float     mRigFrameMs = 0.f;       // smoothed frame time (ms)
    bool      mRigDecWant = false;     // the scan requests decimated steps (engaged / about to)
    uint64_t  mRigSlodEst[kRigSlodMax + 1] = {};   // predictor: visible verts at each A (decimation applied)
    uint8_t   mRigSlodLvl[kAvatarSlotCap] = {};   // per slot: av_vis screen-LOD level (0 = off)
    uint64_t  mRigDecBudgetBytes = 0;  // the decimated-step cache's byte budget (VKMem)
    uint64_t  rigDecimBudgetBytes() const;
    GpuBuffer mRigDecimSSBO{};         // clean_cull.comp binding 29 (LLCleanRigDecimator table)
    std::vector<uint32_t> mRigDecimDirty;   // scratch: table entries to upload
    static uint32_t rigTierCap(uint32_t p, uint32_t tier);   // pressure cap + 1 (0 none)
    void      rigAssignTiers(const std::vector<std::pair<LLUUID, U32>>& slots,
                             const std::vector<LLVOAvatar*>& slot_av, float px_per_unit);
    // #561 / #734 PROTECTION, smooth: per avatar a weight w (mRigProtW, 0..1) from its
    // distance (1 inside RenderVKRiggedProtectMeters, 0 past twice that) and on-screen height
    // (0 below RenderVKRiggedProtectPixels, 1 from 4x that), the larger of the two. It scales
    // the decimation level down (level x (1 - w)) and raises the creator-LOD size floor
    // toward LL's own LOD. FULLY protected (mRigProtect: w = 1, largest first within the
    // protected vertex budget RenderVKRiggedProtectVertsK; our own avatar always): never
    // pressure-capped, never decimated, allocated first (tier 0), RT refits never throttled.
    // (#734 replaced the "top RenderVKRiggedProtectCount" rule: the 11th avatar fell off a
    // cliff to creator LOD 0 at any size.)
    uint8_t  mRigProtect[kAvatarSlotCap] = {};
    float    mRigProtW[kAvatarSlotCap] = {};      // per slot: protection weight (held, +-0.08)
    // per slot: the coarsest pressure cap allowed (cap + 1 encoding: a cap c != 0 is raised
    // to at least this; 0 = no creator-LOD cap at all) -- the #734 size floor
    uint8_t  mRigCapMin[kAvatarSlotCap] = {};
    static uint32_t rigFloorCap(uint32_t cap, uint8_t cap_min)
    { return cap == 0u || cap_min == 0u ? 0u : (cap > cap_min ? cap : (uint32_t)cap_min); }
    static constexpr uint32_t kRigPerfPMax = 9u;   // = 3 x (kRigTiers - 1): never the nearest tier
    bool     rigPerfCreatorOn() const;  // RenderVKRiggedPerfCreatorLOD (and the screen LOD on)
    // #734 state log (rigAssignTiers): the controller's levels and how many avatars sit on
    // creator LODs / decimated steps, on change (counts at most every 10 s).
    uint32_t mRigLogLvl[4] = {~0u, ~0u, ~0u, ~0u};
    uint32_t mRigLogCnt[6] = {};
    double   mRigLogAt = 0.0;
    uint32_t mRigProtFrame = 0;        // last protected-set ranking
    float    mRigAvPx[kAvatarSlotCap] = {};       // per slot: on-screen height (px), this frame
    uint64_t mRigAvVerts[kAvatarSlotCap] = {};    // per slot: visible full-LOD skinned verts (last scan)
    uint64_t mRigProtVerts = 0;        // visible skinned verts of the protected avatars (budget floor)
    // The VRAM ladder's squeeze of the rigged budget, held (#561): it follows the ladder's
    // rigged rungs (#613: LLVramBudgetVK::rigBudgetScale; before, avatarLodScale x the
    // reserve-mode halving) only after the new value held 15 s (down) / 5 s (up), so ladder
    // rungs flapping do not flip the LOD pressure.
    float    mRigScaleHeld = 1.f;
    double   mRigScaleDiffSince = 0.0;
    void     rigUpdateScale();
    static constexpr uint32_t kRigTiers = 4;   // = clean_cull.comp RIG_TIERS
    uint8_t  mRigTier[kAvatarSlotCap] = {};       // per joint-table slot: distance tier (0 nearest .. 3)
    uint8_t  mRigPressCap[kAvatarSlotCap] = {};   // per slot: pressure LOD cap + 1 (0 none) -> av_vis
    uint8_t  mRigSelfSlot[kAvatarSlotCap] = {};   // per slot: our own avatar (never capped)
    uint32_t mRigTierFrame = 0;        // last tier assignment
    uint32_t mRigPressure = 0;         // global pressure level 0..3 * kRigTiers
    uint32_t mRigPressFrame = 0;       // last pressure change
    uint32_t mRigResizeFrame = 0;      // last rigged buffer re-size (shrink hysteresis)
    // RESIZE HYSTERESIS (2026-10-17: every rigged re-size used to cost a full GPU frame --
    // ~30 ms at 4K on Pascal -- and normal camera movement re-sized as avatars entered / left
    // the view). Calm (no VRAM pressure, see rigPressed): the targets take
    // RenderVKRigGrowHeadroomPct over the recent visible peak and a buffer shrinks only once
    // its capacity stayed past RenderVKRigShrinkRatioPct of its target for
    // RenderVKRigShrinkHoldSecs (sustained, not "since the last re-size"). Pressed: the old
    // +1/4 headroom, 2x (1.25x with a rigged rung, #613) shrink after ~10 s -- pressure
    // degrades free memory fast, as before.
    double   mRigLowSince = 0.0;       // wall s the rigged capacities went past the shrink ratio (0 = not)
    bool     rigPressed() const;       // #613 rigged rung, or the VRAM ladder engaged / not calm
    uint64_t rigShrinkX4() const;      // shrink once capacity * 4 > target * this
    uint32_t rigGrowHeadroomPct() const;
    double   rigLowFor(bool over);     // keeps mRigLowSince; seconds the capacities have been over
    double   rigShrinkHoldSecs() const;
    // RESIZE WITHOUT A DEVICE WAIT (2026-10-17; RenderVKResizeNoStall). The rigged buffers
    // (ensureRiggedBuffers), their temporal history (ensureTemporal) and the animation tables
    // (buildAnimFrame) used to re-size behind a vkDeviceWaitIdle: frames in flight read the old
    // buffers, and the descriptor sets naming them are not per frame, so they could not be
    // rewritten while a pending command buffer used them. While VRAM is calm (!rigPressed())
    // and the new buffer fits beside the old one (vramRoom), a re-size instead allocates the
    // new buffer FIRST and RETIRES the old one (retireBuffer: freed once every frame that may
    // use it has completed, on both queues -- beginTextureFrame), and the sets are re-pointed
    // COPY-ON-WRITE: every registered long-lived set (mCow: the world sets writeDescriptors /
    // writeAnimDescriptors write) is replaced by a spare of its layout holding a copy of its
    // written descriptors (vkCowCopySet), the old set is retired with the buffers, and the
    // rewrite lands in sets no submitted frame has seen. Under pressure, a refused allocation,
    // no spare, or the setting off: the old wait + free-first path exactly. Contents: nothing
    // is copied -- every buffer re-sized here is rewritten every frame (or re-streamed in full).
    struct CowMember
    {
        VkDescriptorSet*             set = nullptr;      // the member every writer / binder reads
        const VkDescriptorSetLayout* layout = nullptr;
        const char*                  name = "";
        std::unique_ptr<VkDescPool>  pool;               // the spares' pool (lazy, kCowSpares sets)
        std::vector<VkDescriptorSet> spare;              // sets of this layout no frame uses
        uint32_t                     gen = 1;            // bumped when the pool goes (stale returns dropped)
        uint32_t                     freshFrame = ~0u;   // gFrameCount of the last re-point
        VkDescriptorSet              freshSet = VK_NULL_HANDLE;
    };
    static constexpr uint32_t kCowSpares = 3;   // a re-point per frame for 3 frames before a retire returns
    std::vector<CowMember> mCow;
    bool     mCowBroken = false;       // a guard miss: wait-idle path for the session
    bool     mRigEnsureWaited = false; // ensureRiggedBuffers waited for the device (sets may be written in place)
    uint32_t mNoStallResizes = 0, mNoStallFallbacks = 0, mNoStallRepoints = 0;   // (log)
    bool     resizeNoStallOn() const;  // setting on, no guard miss, VRAM calm
    void     cowRegistry();            // (idempotent) the member list
    void     cowTrackNew(VkDescriptorSet set);   // a registered member was (re)allocated: track its writes
    bool     cowRepointAll(std::vector<VkDescriptorSet>& fresh, const VkDescriptorSet* only = nullptr);   // all-or-nothing
    void     cowRelease(VkDescriptorSet* member);   // the member's own pool is going away
    void     cowShutdown();
    // Rewrite the long-lived sets after re-sizes: in place when the device was waited (or
    // after waiting now), else copy-on-write + the guard. `write` is writeDescriptors & co;
    // `only` (a member): re-point just that one (write() must write nothing else).
    void     rewriteSetsNoStall(bool waited, const std::function<void()>& write, const VkDescriptorSet* only = nullptr);
    GpuBuffer createNoStall(VkDeviceSize bytes, VkBufferUsageFlags usage);   // soft, device-local, if it fits beside the old
    // RIGGED RE-SIZE BEFORE THE RECORDING (2026-10-22, Beta 4/5 Windows NVIDIA DEVICE_LOST; the
    // pdnPrestep rule). buildRiggedFrame / buildAnimFrame run INSIDE recordWorld, after the frame's
    // command buffer began; every re-size they did there (ensureRiggedBuffers + the set re-point,
    // the joint-table / skin-meta grows behind a vkDeviceWaitIdle and an in-place set rewrite, the
    // skin-cache state retry, the anim tables) now only fills mRigReq, and rigPrestep
    // (syncFromStoresPrestep, before the next frame's command buffer begins) applies it. The asking
    // frame stays on the old buffers: every consumer clamps to the current capacities (the culls'
    // pushes, the palette compute, the joint / meta streams), skins past the skin-meta capacity
    // are left out (mSkinCount), and a frame whose anim tables do not fit skips the GPU animation
    // (the joint table keeps last frame's pose, as #697's refused allocation). RenderVKResizeAtFrameStart
    // 0 = the old in-recording path (A/B only). RenderVKRiggedResizeStress N = force a rigged
    // re-size every N frames (soak test).
    struct RigReq
    {
        bool         rig = false;        // ensureRiggedBuffers with the targets below
        bool         rigIdle = false;    // (the no-legacy-rigged shrink: its bookkeeping)
        bool         rigShrink = false;
        uint32_t     palettes = 0, items = 0, verts = 0, cache = 0;
        VkDeviceSize joints = 0;         // joint table bytes wanted (0: none)
        uint8_t      metaOp = 0;         // skin meta: 1 grow / create, 2 shrink
        VkDeviceSize meta = 0;           // ... to this many bytes
        bool         rigCache = false;   // the skin-cache state's refused grow: retry
        bool         anim = false;       // anim tables / local-out (the needs below)
        VkDeviceSize animWords = 0, animDesc = 0, animKeyT = 0, animKeyV = 0;
        uint32_t     animRb = 0;
        uint32_t     animShrink = 0;     // tables whose shrink came due at the request (kAnimShrink*)
    };
    RigReq   mRigReq;
    uint32_t mRigReqLast[4] = {};      // the last rigged targets (palettes, items, verts, cache): stress
    bool     mRigStressForce = false;  // RenderVKRiggedResizeStress: re-create at the current size
    uint32_t mRigStressCount = 0;      // (log)
    bool     mAnimHeld = false;        // last frame's built anim program not dispatched: dispatch it next (buildAnimFrame)
    uint32_t mAnimHeldEntries = 0;     // its entry count
    uint32_t mAnimHeldFrames = 0;      // (log)
    void     rigPrestep();             // syncFromStoresPrestep: apply mRigReq
    void     ensureRigHistory();       // ensureTemporal's rigged history (rigPrestep; inline when RenderVKResizeAtFrameStart 0)
    void     recreatePrevInstances();  // the previous-instance rows at the instance capacity (caller waited)
    // Other runtime re-sizes recordWorld asked for (2026-10-22, the same rule): the alpha-sort key
    // scratch (wait + the alpha-sort sets), the particle emitter table (wait + submitImmediate +
    // the particle sets). The asking frame: the single-workgroup sort / the emitters the old table
    // holds -- each the path a refused allocation already takes.
    uint32_t     mAlphaSortKeysReq = 0;
    VkDeviceSize mPartEmitReq = 0;
    void     partEmitGrow(VkDeviceSize bytes);
    void     frameStartResizePrestep();
    // The RT reflections' uber pre-trace target likewise (ensureRtUber: wait + submitImmediate + set 2
    // b11, from deferSplitFrame); the asking frame takes no pre-trace (the probe fallback).
    struct RtUberReq { bool pending = false; uint32_t w = 0, h = 0; bool full = false; };
    RtUberReq mRtUberReq;
    bool     rtUberAtFrameStart(uint32_t rw, uint32_t rh, bool full);
    // The mirror capture target likewise (ensureMirror: wait + submitImmediate + draw set 0 b17);
    // the asking frame has no mirror capture, as after a refused allocation.
    struct MirrorReq { bool pending = false; uint32_t w = 0, h = 0; bool rt = false; };
    MirrorReq mMirrorReq;
    bool     mirrorTargetAtFrameStart(uint32_t w, uint32_t h, bool rt);
    void     mirrorPrestep();
    // The rigged re-size + its set rewrite and bookkeeping (the request path or the old in-place one).
    void     rigApplyResize(uint32_t palettes, uint32_t items, uint32_t verts, uint32_t cache, bool shrink, bool idle);
    // buildAnimFrame's table sizing: wanted (no side effect past tableShrinkDue's clock) / applied.
    enum : uint32_t { kAnimShrinkWords = 1u, kAnimShrinkDesc = 2u, kAnimShrinkKeyT = 4u, kAnimShrinkKeyV = 8u };
    uint32_t animResizeWanted(VkDeviceSize words, VkDeviceSize desc, VkDeviceSize keyT, VkDeviceSize keyV,
                              uint32_t rb, double now, uint32_t& shrink_mask);   // 0 none, 1 shrink due, 2 must grow
    bool     animResizeApply(VkDeviceSize words, VkDeviceSize desc, VkDeviceSize keyT, VkDeviceSize keyV,
                             uint32_t rb, double now, bool& tables_grew,
                             int32_t shrink_mask);   // false: the local-out refused; shrink_mask < 0: ask tableShrinkDue
    uint64_t mRigEst[4][4] = {};       // predictor: visible verts per tier, per LOD cap (3 = none)
    uint64_t mRigUbItems = 0, mRigUbVerts = 0, mRigUbCache = 0;   // CPU scan (every live instance)
    uint32_t mRigDemTier[4] = {};      // last GPU demand per tier (verts)
    uint32_t mRigDropItems = 0, mRigDropVerts = 0;   // last GPU capacity drops
    uint64_t mRigPeakCurV = 0, mRigPeakPrevV = 0, mRigPeakCurW = 0, mRigPeakPrevW = 0;   // ~300-frame windows
    uint32_t mRigPeakWinFrame = 0;
    // 2026-10-14 VRAM: the skin cache arena's MEASURED use (rig_alloc word 4, the epoch bump
    // pointer, copied into readback word kDbgRbCacheUsed after the main cull; it counts past the
    // capacity, leaks included). Compact = the use right after an epoch reset (what the arena
    // really needs: every cached instance re-allocates once); used = the latest (with leaks).
    // Grow from the compact demand, shrink from the used one (buildRiggedFrame).
    uint64_t mRigCacheUsedCur = 0, mRigCacheUsedPrev = 0;   // ~300-frame windows (clamped to the cap)
    uint64_t mRigCacheCompact = 0;     // max use sampled in the frames right after the last reset
    bool     mRigCacheSeen = false;    // the GPU has reported cache use (else the CPU estimate)
    // 2026-10-14 LOD-OFF VANISHING AVATARS: a 0-capacity arena measures nothing (the cull caches
    // nothing at u_cache_cap 0 and skins every cache-eligible instance into a per-frame range), so
    // it is grown from the CPU estimate -- at most once per ~30 s when the measurement then took it
    // back to 0 (no grow/shrink cycle). mRigCacheBootFrame: the last such CPU-driven grow.
    uint32_t mRigCacheBootFrame = 0;
    bool     mRigCacheBooted = false;
    // CLUSTER LOD MODE FLIP (clusterTick, render thread): the rigged sizing restarts from the new
    // mode (forced re-scan, re-measured arena) and one summary line follows once the flip settled.
    bool     mRigFlipPending = false;  // buildRiggedFrame applies the restart
    bool     mRigFlipTo = false;       // the new mode (true = cluster owns rigged)
    uint32_t mRigFlipFrame = 0;        // when it flipped (0 = no report pending)
    uint64_t mRigFlipDynBefore = 0, mRigFlipCacheBefore = 0, mRigFlipWorkBefore = 0;
    uint32_t mRigFlipResizes = 0;      // rigged buffer re-sizes since the flip
    uint64_t mRigFlipItemsBefore = 0, mRigFlipRoutedSlotsBefore = 0, mRigFlipRoutedGroupsBefore = 0;
    void     rigFlipReport();          // the one summary line ~3 s after a flip
    // CPU scan (every ~15 frames): live rows with NEITHER path -- no resident legacy LOD slot and
    // not cluster-routed in the cluster-owns-rigged mode (a mesh still loading counts too).
    uint32_t mRowsNoPath = 0, mRowsNoPathRigged = 0;
    uint64_t mRigRoutedVerts = 0;      // CPU scan: routable rows of still-packed groups skipped (owned mode)
    uint64_t mRigDemAtLevel[13] = {};  // GPU demand last measured at each pressure level
    uint32_t mRigDemAtFrame[13] = {};  // ... and when (0 = never)
    uint32_t mTemporalRetryFrame = 0;  // a failed (soft) TAA history allocation: retry after this frame
    uint64_t mRigPeakBytes = 0;        // high-water of the rigged buffers' bytes (VKMem)
    bool     mRigShrinkTight = false;  // #613: a rigged rung is engaged -> shrink to the target sooner

    // ---- #613 RIGGED GPU TIME (always on, sampled; independent of RenderVKGpuTimers) ----
    // Every kRigTsEvery-th recording writes a few timestamps into an own 4-deep ring: the
    // frame's first and last (its GPU span) and both ends of each rigged span (the graphics
    // sub-marks around the pre-skin, the weld and the rigged raster blocks). Read
    // kRigTsRing-1 frames later without waiting. Feeds the rigged perf trigger.
    static constexpr uint32_t kRigTsRing = 4, kRigTs = 48, kRigTsEvery = 8;   // (48: + the c_cl_sk_* links, 2026-10-14)
    VkQueryPool mRigTsPool[kRigTsRing] = {};
    uint32_t mRigTsN[kRigTsRing] = {};               // timestamps written per slot
    uint8_t  mRigTsKind[kRigTsRing][kRigTs] = {};    // 0 frame begin, 1 frame end, 2 span begin, 3 span end, 4 both
    uint32_t mRigTsSeqAt[kRigTsRing][kRigTs] = {};   // graphics sub-mark sequence of each
    bool     mRigTsPend[kRigTsRing] = {};
    uint32_t mRigTsSlot = 0, mRigTsFrame = 0, mRigTsSeq = 0;
    bool     mRigTsInit = false, mRigTsOk = false, mRigTsRec = false;
    uint32_t mRigTsBits = 64;
    float    mRigTsPeriodNs = 1.f;
    float    mRigGpuMs = 0.f;          // smoothed GPU time of the rigged work (ms)
    // #749 CLUSTER LOD spans in the same sampled ring (graphics marks only; written only while
    // the cluster path records them, so with RenderVKClusterLOD off nothing changes):
    // [0] selection (c_cull -> c_cluster_t*), [1] pre-pass (g_pre_cl_begin -> g_pre_cl_end),
    // [2] G-buffer (g_cl_begin -> g_gbuf_cluster_t*), [3] the cluster pre-skin (#747:
    // c_cl_preskin_begin -> c_cluster_preskin). -1 = no sample yet. Kept OUT of mRigGpuMs:
    // the old controller's input is unchanged.
    float    mClGpuMs[4] = {-1.f, -1.f, -1.f, -1.f};
    // Fixed-cost audit (2026-10-14): the cluster LOD sub-timers (and their legacy counterparts) of
    // the last 5 s GPU-timer window, "name=avg/worst" -- the CLOD gpu sub line (LLClusterDiagIn)
    std::string mClGpuSubLine;
    uint8_t  mRigTsCls[kRigTsRing][kRigTs] = {};   // span class of each timestamp (0 rigged, 1..4 cluster)
    float    mRigGpuFrameMs = 0.f;     // smoothed GPU span of the world recording (ms)
    double   mRigGpuAt = 0.0;          // last sample (LLFrameTimer s; 0 = none)
    bool     mRigGpuBound = false;     // last controller tick: the frame is GPU-bound
    void     rigGpuTimeBegin(VkCommandBuffer cmd);
    void     rigGpuTimeMark(VkCommandBuffer cmd, const char* name, int kind = -1);
    void     rigGpuTimeDestroy();

    // Fixed capacity of the bindless base-color descriptor array. Sized once at
    // init (partially-bound) so real textures can be added post-login without
    // recreating the descriptor set layout / pipeline.
    // Bindless table size: min(this, device sampled-image limits), set at init.
    static constexpr uint32_t kMaxBindlessTextures = 65536;
    uint32_t mMaxBindless = 4096;
    // Slot lifetime: stamped with mTexPass whenever a LIVE material (one a live
    // instance uses) or a light resolves it; slots unstamped for kTexEvictPasses are
    // freed (image destroyed, slot recycled via mFreeTexSlots).
    std::vector<uint32_t> mTexLastUsed;
    std::vector<uint32_t> mFreeTexSlots;
    uint32_t mTexPass = 0;
    static constexpr uint32_t kTexEvictPasses = 600;
    // #737: eviction window in wall time (RenderVKAssetReleaseGraceSec): passes per second
    // measured between eviction sweeps.
    double   mTexEvictSweepT = 0.0;
    uint32_t mTexEvictSweepPass = 0;
    double   mTexPassRate = 0.0;

    VkDeviceContext* mCtx = nullptr;
    VkFormat mColorFormat = VK_FORMAT_UNDEFINED;
    bool mReady = false;
    // Ray tracing foundation: exists only when the device enabled the RT extensions.
    // Raw pointer (not unique_ptr): other TUs destroy this class with LLCleanRTVK
    // incomplete. Owned; deleted in shutdown().
    LLCleanRTVK* mRT = nullptr;
    // God rays (#208, RenderVKGodRays; llcleangodraysvk.h): exists only while enabled.
    LLCleanGodRaysVK* mGodRays = nullptr;
    // VOLUMETRIC FOG (#718, RenderVKVolumetricFog; llcleanvolfogvk.h): exists only while
    // enabled (created on the first enabled frame, destroyed + deleted once it is off).
    // Draw set 0 binding 45 names its integrated volume while it exists, else mVolFogStub
    // (1 x 1 x 1 RGBA16F identity: no in-scatter, T = 1; made at init, never freed before
    // shutdown) -- every pipeline on the draw set 0 layout always sees a valid descriptor.
    LLCleanVolFogVK* mVolFog = nullptr;
    GpuImage    mVolFogStub;
    VkSampler   mVolFogSampler = VK_NULL_HANDLE;      // linear / clamp (binding 45)
    VkImageView mVolFogBoundView = VK_NULL_HANDLE;    // what binding 45 names now
    bool        mVolFogNow = false;        // this recording runs the fog (prepare returned true)
    bool        mVolFogRecorded = false;   // its compute is recorded this recording
    uint64_t    mVolFogAsyncValue = 0;     // its async segment's compute timeline value (0: on graphics / joined)
    // The target-ensure step (recordWorld, before anything binds draw set 0): settings,
    // lifetime, binding 45. camTime: the main view's eye + time.
    void ensureVolFog(VkCommandBuffer cmd, const float* camTime, uint32_t rw, uint32_t rh);
    // PARTICLE FOG (#1003, RenderVKParticleFog; llcleanpartfogvk.h): exists only while enabled.
    // Fog-like particle systems' particles become density carriers splatted into local bricks,
    // marched after pass 1 (recordPartFogCompute, right after the per-particle lighting) and
    // composited in pass 2 right after the froxel apply. Needs the froxel fog (mVolFogNow).
    LLCleanPartFogVK* mPartFog = nullptr;
    bool        mPartFogNow = false;       // prepare returned true this recording
    // The target-ensure step (after ensureVolFog): settings, classification, slots, resources.
    void ensurePartFog(VkCommandBuffer cmd, const float* camTime, uint32_t rw, uint32_t rh);
    // After recordParticleLighting (its records final; pass 1's depth finished).
    void recordPartFogCompute(VkCommandBuffer cmd);
    void writeVolFogBinding(VkImageView view);   // binding 45 (device idle / set not in flight)
    void createVolFogStub();                     // init: the stub + sampler, written to binding 45
    // SKY-VIEW LUT (#819, RenderVKSkyLUT; clean_atmos_common.glsl skyLutUV / skyColor,
    // clean_sky_lut.comp): skyColor() over its two coordinates (the dome direction's height and
    // its angle to the light norm), baked once per frame after the frame-info upload into a
    // kSkyLutW x kSkyLutH RGBA16F table (256 KB, created at init, GENERAL) at draw set 0
    // binding 52; the lit FS / deferred kernels / water / particle FS sample it instead of the
    // analytic haze (ssr.ao_up.z = 1). Bake: set 0 = draw set 0 (frame info), set 1 = the table
    // as a storage image.
    static constexpr uint32_t kSkyLutW = 256, kSkyLutH = 128;
    GpuImage              mSkyLut{};
    VkSampler             mSkyLutSampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout mDslSkyLut = VK_NULL_HANDLE;
    VkDescPool            mSkyLutPool;
    VkDescriptorSet       mDsetSkyLut = VK_NULL_HANDLE;
    VkPipelineLayout      mSkyLutLayout = VK_NULL_HANDLE;
    VkPipeline            mSkyLutPipe = VK_NULL_HANDLE;
    bool                  mSkyLutNow = false;    // this recording bakes it (and the frame info says so)
    bool                  mSkyLutLogged = false;
    void initSkyLut();                           // init: table + sampler + bake set, binding 52 written
    void recordSkyLut(VkCommandBuffer cmd);      // after the frame-info upload, when mSkyLutNow
    // The fog's four compute passes (once per recording, outside any rendering scope):
    // on the async queue when RenderVKAsyncVolumetrics allows, else on graphics.
    void recordVolFogCompute(VkCommandBuffer& cmd, bool allowAsync);
    // WEATHER (#783, slweather.h + llcleanweathervk.h): exists only while there is weather to
    // draw (precipitation, or surfaces still drying); created on the first such recording,
    // destroyed + deleted once there is none. mWeatherNow: it runs this recording.
    LLCleanWeatherVK* mWeather = nullptr;
    bool mWeatherNow = false;
    uint32_t mWeatherResetSeen = 0;   // #904: SLWeather::clearSerial() the module was last checked against (master off: freed)
    uint32_t mWeatherPlaceSeen = 0;   // #895: SLWeather::placeSerial() last seen (a teleport: the ground maps reset)
    // #783 M3: the snow surface mesh is drawn this recording (deferred lighting this frame and
    // the module built it): the G-buffer scope and the sun cascades draw it.
    bool mWeatherMeshNow = false;
    // The target-ensure step (before anything binds draw set 0): the weather state's frame
    // update, lifetime, binding 49. dp: the main view's draw push (camera, sun, ambient).
    void ensureWeather(VkCommandBuffer cmd, const VKDrawPush& dp, uint32_t rw, uint32_t rh);
    // The occlusion map, when due: this frame's static casters straight down into the weather
    // module's target map with the projector depth pipelines (right after the projector tiles:
    // the shadow cull scratch is free there). cullBase: the main cull push; dp: drawPushIn.
    void recordWeatherOcclusion(VkCommandBuffer cmd, const VKCullPush& cullBase, const VKDrawPush& dp);
    // WEATHER (#783): draw set 0 binding 49 [2], the weather occlusion maps (the lit / G-buffer
    // FS's wet-surface sky test) -- the weather module's maps while it has them, else the 1 x 1
    // projector depth stand-in (written at init; never read then).
    // [4]: the coarse cascade's two maps, then the near cascade's (llcleanweathervk.h).
    // M2: [4] / [5] the persistent snow-cover grids, [6] the moving objects' snow (GENERAL);
    // M4: [7] the run-off flow map (GENERAL).
    static constexpr uint32_t kWeatherOcclSlots = 10;   // (#901: + the 2 avatar shelter atlas images)
    // #783 M3: this frame's snow mesh draws the forward lit pass's ray-query variant (a forward
    // frame that traces without an RT mask); set with mWeatherMeshNow.
    bool mWeatherMeshFwdRt = false;
    VkImageView mWeatherBoundViews[kWeatherOcclSlots] = {};   // what binding 49 names now
    // views: kWeatherOcclSlots entries; null (or a null entry) = the stand-in (device idle /
    // set not in flight); layouts: each real view's layout (null: DEPTH_READ_ONLY)
    void writeWeatherOcclBinding(const VkImageView* views, const VkImageLayout* layouts = nullptr);
    // #901 REST-POSE ACCUMULATION: draw set 0 binding 50 (the grids' record; null: mWxAccumStub, a
    // zeroed buffer of the record's size, made on first use) and 51 (the grids; null: mVolFogStub),
    // device idle / set not in flight; what they name now.
    void writeWeatherAccumBinding(VkBuffer rec, VkImageView grids);
    GpuBuffer   mWxAccumStub;
    VkBuffer    mWxAccumBoundRec = VK_NULL_HANDLE;
    VkImageView mWxAccumBoundView = VK_NULL_HANDLE;
    // Volumetric clouds (#719, RenderVKVolumetricClouds; llcleanvolcloudsvk.h): exists only
    // while enabled (or while its resources are being released).
    LLCleanVolCloudsVK* mVolClouds = nullptr;
    // This recording composites the volumetric clouds over the main view (set before the sky
    // draw: clean_sky.frag then leaves the clouds to the composite instead of the panorama).
    bool mVolCloudsComposite = false;
    // Reduced-resolution particles (RenderVKParticleLowRes; llcleanpartlowresvk.h): created
    // on the first frame that wants it, freed after ~10 s unused. Adaptive switch state:
    // on / frames calm below the exit threshold / frames unused.
    LLCleanPartLowResVK* mPartLowRes = nullptr;
    bool     mPartLowResOn = false;
    uint32_t mPartLowResCalm = 0;
    uint32_t mPartLowResIdle = 0;
    // Motion blur (#118, RenderVKMotionBlur; llcleanmotionblurvk.h): exists only while enabled.
    LLCleanMotionBlurVK* mMotionBlur = nullptr;
    // LIVE SNAPSHOTS (#873; llcleansnapshotvk.h). Created by the first live snapshot.
    // ACCUMULATION FRAMES (upscaled shots, mLiveCapNow): the main view is jittered with the
    // shot's own Halton sequence and the TAA / FSR resolve and the post AA are skipped, so the
    // raw jittered sample reaches the swap image; mTaaFrame does not advance. The live TAA /
    // FSR history images are not touched by those frames: the first frame after the shot
    // restores mFsrRanLast and keeps the history when the camera held still (same unjittered
    // view-projection as when the shot began), else invalidates it (a clean reset).
    LLCleanSnapshotVK* mLiveSnap = nullptr;
    bool      mLiveCapNow = false;        // this frame is a live-snapshot accumulation sample
    bool      mLiveCapWas = false;        // the previous frame was
    bool      mLiveCapFsrRanLast = false; // mFsrRanLast when the shot began
    float     mLiveCapVP[16] = {};        // unjittered view-projection when the shot began
    // Raster cursor pick (#702) + particle pick (#481) (llcleanpickvk.h). Created on the
    // first request.
    LLCleanPickVK* mPick = nullptr;
    bool mAcousticsStandDown = false;       // last logged state (log once per transition)
    LLCleanAudioRTVK* mAudioRT = nullptr;   // acoustics probe (#683; created on the first request)
    // AVATAR MASK + CEL SHADING (#549 / #550; vkgfx/llcelshadevk.{h,cpp}, which also
    // defines the member functions below). Created with the draw set 0 descriptors (its
    // 96 B parameter buffer is draw set 0 binding 37); targets only while a consumer wants them.
    LLCelShadeVK* mCel = nullptr;
    bool mCelFxWantsMask = false;   // requestAvatarMask before mCel exists (copied in at creation)
    void writeCelDescriptors();                     // draw set 0 bindings 37 / 38
    void recordCelFrameStart(VkCommandBuffer cmd);  // settings -> params upload (before any draw)
    // After pass 1 (depth DEPTH_READ_ONLY_OPTIMAL, scene HDR COLOR_ATTACHMENT_OPTIMAL), before
    // the god rays / pass 2: the avatar-id pass, its FX resolve, the debug view and outlines.
    void recordCelPasses(VkCommandBuffer cmd, const VkDescriptorSet* dsets, const VKDrawPush& drawPush,
                         const float* viewProjRelMain, bool do_rigged, uint32_t rw, uint32_t rh,
                         uint32_t width, uint32_t height);
    void destroyCel();
    void rtSlotChanged(uint32_t sl, bool from_repack);
    // ensureCap growth of the geometry buffers: + device address / AS build input when RT.
    VkBufferUsageFlags rtGeomUsage() const;
    // Usage of the three geometry buffers (ensureCap and the #584 compaction).
    VkBufferUsageFlags vboUsage() const;
    VkBufferUsageFlags iboUsage() const;

    // Mega geometry.
    GpuBuffer mVbo{}, mIbo{};
    uint32_t  mIdxValid = 0;

    // Stores (device-local).
    GpuBuffer mInstanceSSBO{}, mSlotSSBO{}, mLodGroupSSBO{}, mMaterialSSBO{};
    uint32_t  mInstanceCount = 0;
    uint32_t  mInstanceCap = 0;
    // Instance streaming: the instance SSBO is sized to CAPACITY (not exact count) and
    // its contents are updated every frame by a GPU copy from a host-visible staging
    // ring (indexed by swapchain frame slot), so animating instances no longer force a
    // full buildBuffers() rebuild. Grown only when the live count exceeds capacity.
    uint32_t  mInstanceBufCap = 0;            // capacity in Instances of SSBO + staging
    GpuBuffer mInstanceStaging[2]{};          // host-visible upload ring
    uint32_t  mFrameSlot = 0;                 // current swapchain frame-in-flight slot
    double    mLastBuildMs = 0.0;             // wall-clock of the last grow (ensureBuffers)

    // Streamed geometry/material buffers (same pattern as the instance SSBO): persistent
    // device-local sized to CAPACITY + host-visible staging ring, content GPU-copied
    // per-frame when dirty. A full realloc (device wait + descriptor rewrite) happens
    // ONLY when a buffer must grow -- geometry LOD churn during camera pan streams
    // instead of rebuilding, so it no longer stalls.
    GpuBuffer mVboStg[2]{}, mIboStg[2]{}, mWeightStg[2]{}, mSlotStg[2]{}, mGroupStg[2]{}, mMatStg[2]{};
    // Per-map UV transforms (LLCleanMaterialStore::MaterialUV, 96 B, parallel to the
    // materials): draw set 0 binding 16, VS + FS. Streamed when the bytes change.
    GpuBuffer mMatUVSSBO{}, mMatUVStg[2]{};
    VkDeviceSize mMatUVCap = 0;
    std::vector<uint8_t> mCpuMatUV, mPrevMatUV;
    // Per-region terrain records (LLCleanMaterialStore::TerrainRecord, 176 B): draw set
    // 0 binding 22, FS. Copied with the materials (same store generation), streamed
    // when the bytes change. A terrain instance's skin_id indexes it.
    GpuBuffer mTerrainSSBO{}, mTerrainStg[2]{};
    VkDeviceSize mTerrainCap = 0;
    std::vector<uint8_t> mCpuTerrain, mPrevTerrain;
    // C5: set when mCpuMatUV / mCpuTerrain were re-copied (copyMaterialUV) or their
    // GPU buffer is fresh; recordWorld only compares + streams the blobs when set
    // (RenderVKMatUVDirtyGate; the full ~1.7 MB compares ran every frame).
    bool mMatUVDirty = true, mTerrainDirty = true;
    void copyMaterialUV(const class LLCleanMaterialStore& mm, uint32_t mat_n);
    VkDeviceSize mVboCap=0, mIboCap=0, mWeightCap=0, mSlotCap=0, mGroupCap=0, mMatCap=0;  // bytes
    VkDeviceSize mSkinCap=0;                   // mSkinVbo capacity (co-grows with mVbo)
    bool mGeomDirty=false, mMatDirty=false;   // content changed -> stream in recordWorld
    bool mLastGrew=false;                      // did the last buildBuffers() actually grow?
    bool mTexHandlesChanged=false;             // sliced walk rewrote a material's handles/flags
    uint32_t mTexWalkCoverage=0;               // existing materials walked since the last full cycle
    bool mTexResolvePending=false;             // uploadRealMaterialTextures hit its per-frame
                                               // image-op cap -> keep resolving next frame
    // Grow-only capacity ensure (2x headroom); returns true if it (re)allocated `buf`
    // (caller must have device-waited and must rewrite descriptors afterward). Records
    // into `cap`. Staging is NOT sized here any more: streamBuf / streamRuns size their
    // ring on demand to the bytes actually uploaded (stageFor), so growing a 500 MB
    // geometry buffer no longer allocates 2 x 500 MB of host-visible staging.
    // preserve: an existing buffer's contents carry over by a GPU copy recorded at the
    // start of this frame's uploads (flushGrowCopies) instead of a full CPU re-stream;
    // the old buffer is retired after that copy. Only for buffers whose GPU contents
    // mirror their CPU copy (the syncFromStores path, before recordWorld).
    //
    // RUNTIME OUT OF MEMORY (#595 #597 #599): never fatal. The headroom is budget-aware
    // (2x / 1.5x only while the live free budget holds it; else need + 1/16, then the exact
    // need), every attempt is SOFT, and after them: `restream` (the caller can re-upload
    // the whole contents from its CPU copy / the mesh store mirror) frees the old buffer
    // first and re-creates it fresh (*restream set, never cleared: re-stream everything); without it, or
    // when that fails too, the OLD buffer and capacity stay (false; cap < need afterwards:
    // the caller defers what does not fit). Only a buffer that does not exist at all and
    // cannot be created even at its minimum allocates hard.
    bool ensureCap(GpuBuffer& buf, GpuBuffer stg[2], VkDeviceSize& cap, VkDeviceSize need,
                   VkBufferUsageFlags usage, bool deviceAddress = false, bool preserve = false,
                   bool* restream = nullptr);
    // A refused grow (#595): what was asked and refused, for the grow backoff.
    uint32_t mGrowRefused = 0;           // ensureCap refusals since start (stats)
    // Staging for an upload of `bytes` (#160). Rings serve the steady per-frame uploads
    // and are capped (kStageRingCap); a bigger one-off upload (full re-stream after a
    // repack, a fresh buffer) gets a TRANSIENT buffer that is retired right after its copy
    // is recorded -- freed when the frame completes instead of pinning GBs of host-visible
    // memory as a ring sized to the largest upload ever seen. Returns the buffer to write
    // into (the ring or `tmp`); null on failure. Caller: if the result is &tmp, retire it
    // (retireBuffer) after recording the copy.
    // soft (#595): out of memory returns null instead of aborting (the caller re-queues).
    // #697: SOFT BY DEFAULT -- every caller handles null (streamBuf / streamRuns return
    // false and their callers keep the rows dirty for the next frame). A failure keeps the
    // existing ring (a grow no longer retires it first) and is counted (VKLM_STAGING).
    GpuBuffer* stageFor(GpuBuffer& ring, VkDeviceSize bytes, GpuBuffer& tmp, bool soft = true);
    // TABLE SHRINK (scene tables, skin meta, animation arenas): a table whose live need
    // stays under RenderVKTableShrinkPct (33%) of its capacity for RenderVKTableShrinkSecs
    // (60 s) is re-created at 1.5x need; under VRAM pressure (the ladder engaged / not calm,
    // or a #613 rigged rung) the old fast rule: under 40% for 10 s -> 1.25x need
    // (4 MB capacity floor: below that a device wait is not worth it). tableShrinkDue keeps
    // the per-table "low since" clock (call it every frame the table is checked);
    // shrinkCap re-creates with the device idle -- preserve: GPU copy of the first
    // 1.25x-need bytes (flushGrowCopies), else the caller re-streams in full. Soft: a
    // failed allocation keeps the old buffer.
    enum TableId { TBL_SLOT, TBL_GROUP, TBL_MAT, TBL_MATUV, TBL_TERRAIN, TBL_SKINMETA,
                   TBL_ANIM_WORDS, TBL_ANIM_DESC, TBL_ANIM_KEYT, TBL_ANIM_KEYV, TBL_COUNT };
    double mTblLowSince[TBL_COUNT] = {};   // wall s the table went under the shrink share (0 = not)
    bool tableShrinkDue(const GpuBuffer& buf, VkDeviceSize cap, VkDeviceSize need, TableId t, double now);
    bool shrinkCap(GpuBuffer& buf, VkDeviceSize& cap, VkDeviceSize need, VkBufferUsageFlags usage, bool preserve);
    // Last live byte needs of the animation arenas (suballocSlack; set by buildAnimFrame).
    VkDeviceSize mAnimWordsNeed = 0, mAnimDescNeed = 0, mAnimKeyTNeed = 0, mAnimKeyVNeed = 0;
    // regions empty = copy [0, bytes); else exactly these (#584: geometry compaction moves
    // each live slot range from the old buffer to its compacted offset in the new one).
    struct GrowCopy { GpuBuffer src; VkBuffer dst; VkDeviceSize bytes; std::vector<VkBufferCopy> regions; };
    std::vector<GrowCopy> mGrowCopies;   // pending old -> new contents copies (preserve)
    void flushGrowCopies(VkCommandBuffer cmd);
    // #584: one upload piece -- `len` bytes from `src` (a mesh-store mirror range, or the
    // proto prefix) to byte offset `dst` of the destination buffer.
    struct GeomPiece { const uint8_t* src; VkDeviceSize dst; VkDeviceSize len; };
    // Like streamRuns, but each piece has its own source pointer. One vkCmdCopyBuffer needs
    // disjoint destinations: a piece overlapping an earlier one (slots sharing a range) goes
    // into a later copy behind a barrier, last writer wins (B6 validation, pRegions-12482).
    // streamRuns sorts + merges its runs instead (one source: identical bytes).
    bool streamPieces(VkCommandBuffer cmd, GpuBuffer& dst, GpuBuffer stg[2], VkDeviceSize cap_bytes,
                      const std::vector<GeomPiece>& pieces, VkPipelineStageFlags2 stage, VkAccessFlags2 access);
    // memcpy `bytes` from src into stg[frameSlot], then GPU-copy stg->dst inside `cmd`
    // bracketed by barriers (prior reads at stage/access <-> transfer).
    // Upload only the element runs [first, last) (same offsets in src and dst).
    // rowFix (#568, optional): called per copied range with (staging dst, src, bytes, ctx)
    // after the memcpy, to rewrite lanes in the STAGING copy only (instance rows: store
    // frame -> agent space). It may only WRITE the staging memory (write-combined).
    typedef void (*StreamRowFix)(uint8_t* dst, const uint8_t* src, VkDeviceSize bytes, const void* ctx);
    // #697: false = NOT uploaded (staging could not be allocated: out of memory); the caller
    // keeps the rows dirty / forces a full stream next frame. True also when there was
    // nothing to do (no destination / no bytes).
    bool streamRuns(VkCommandBuffer cmd, GpuBuffer& dst, GpuBuffer stg[2], const uint8_t* src,
                    VkDeviceSize cap_bytes, const std::vector<std::pair<uint32_t, uint32_t>>& runs,
                    VkDeviceSize elem, VkPipelineStageFlags2 stage, VkAccessFlags2 access,
                    StreamRowFix fix = nullptr, const void* fix_ctx = nullptr);
    bool streamBuf(VkCommandBuffer cmd, GpuBuffer& dst, GpuBuffer stg[2], const void* src,
                   VkDeviceSize bytes, VkPipelineStageFlags2 stage, VkAccessFlags2 access,
                   StreamRowFix fix = nullptr, const void* fix_ctx = nullptr);
    // #697 staging failures carried to the next frame (the dirty flags below are recomputed
    // by syncFromStores every frame, so a failed full stream needs its own sticky bit).
    bool mStageRetryMat = false, mStageRetrySlots = false, mStageRetryGroups = false;
public:
    void setFrameSlot(uint32_t s) { mFrameSlot = s; }

    // ---- ASYNC COMPUTE (roadmap phase 3a) ----
    // LLVulkanContext::renderFrame opts a frame in: asyncBeginFrame(cmd) after
    // setFrameSlot (host-waits this slot's previous compute work, resets its pools),
    // then recordWorld, which may END `cmd` and continue in new command buffers at its
    // fork / join points. The caller then records the rest of the frame (UI, present
    // barrier) into asyncCurrentCmd(cmd), ends it, and submits everything with
    // asyncSubmitFrame (falls back to one submit when nothing was split). Frames not
    // opted in (offscreen renders, the PNG milestone path) record single-queue exactly
    // as before. Toggles: RenderVKAsyncCompute + RenderVKAsync{RTBuild,MaskAO,GodRays,Post}.
    struct AsyncWsi
    {
        VkSemaphore acquire = VK_NULL_HANDLE;           // binary: first graphics submission waits
        VkPipelineStageFlags2 acquireStage = 0;
        VkSemaphore renderFinished = VK_NULL_HANDLE;    // binary: last graphics submission signals
        VkFence fence = VK_NULL_HANDLE;                 // last graphics submission
    };
    bool asyncAvailable() const { return mAsyncAvail; }
    void asyncBeginFrame(VkCommandBuffer cmd);
    // Opted-in frames: a separate graphics command buffer (begun; the caller ends it) for
    // the swapchain image's clear + transition. asyncSubmitFrame submits it first and it
    // is the only submission that waits the acquire semaphore. Null when not opted in.
    VkCommandBuffer asyncWsiCmd();
    VkCommandBuffer asyncCurrentCmd(VkCommandBuffer fallback) const
    {
        return (mAsyncOptIn && mAsyncGfx) ? mAsyncGfx : fallback;
    }
    // Submits the frame's segments in recording order + the tail (asyncCurrentCmd, already
    // ended by the caller). Returns false (nothing submitted) when the frame was not opted
    // in: the caller then does its own single submit.
    bool asyncSubmitFrame(VkQueue gfxQueue, const AsyncWsi& wsi);
    // ---- #697 DROPPED FRAMES (out of memory at vkEndCommandBuffer / vkQueueSubmit2) ----
    // A frame whose command buffers could not be ended or submitted is DROPPED instead of
    // aborting: whatever was already submitted is waited out, the async timelines are
    // host-signalled to every value handed out (nothing later waits forever), and an EMPTY
    // submission keeps the swapchain contract -- it waits the acquire semaphore (when not
    // consumed yet), signals render-finished and the in-flight fence -- so present and the
    // frame-slot ring proceed (one frame of undefined swap image). onFrameDropped() then
    // forces the next frame to re-stream every streamed table in full and to rebuild what
    // the lost recording would have built (RT structures, temporal history). If even the
    // empty submission fails, the device is treated as lost (graceful logout path) -- never
    // LL_ERRS. vkEndOrNoteOom: vkEndCommandBuffer (+ SLVkInjectOomCalls); false = out of
    // memory (noted; the frame will be dropped); other errors stay fatal (VK_CHECK).
    bool vkEndOrNoteOom(VkCommandBuffer cmd);
    void noteFrameOom() { mFrameOom = true; }
    bool frameOom() const { return mFrameOom; }
    static bool submitEmptyFrame(VkQueue q, const AsyncWsi& wsi, bool waitAcquire);
    void onFrameDropped();
    uint32_t framesDropped() const { return mFramesDropped; }
private:
    bool mFrameOom = false;
    uint32_t mFramesDropped = 0;
    void dropAsyncFrame(VkQueue gfxQueue, const AsyncWsi& wsi, bool acquireConsumed);
    struct AsyncSeg
    {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        bool compute = false;                        // async compute queue (else graphics)
        uint64_t waitGfx = 0;                        // graphics timeline value to wait (0 none)
        uint64_t waitComp = 0;                       // compute timeline value to wait (0 none)
        VkPipelineStageFlags2 waitCompStage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        uint64_t waitComp2 = 0;                      // second compute wait (cross-frame post)
        uint64_t signal = 0;                         // this queue's timeline value (0 none)
        const char* name = nullptr;                  // compute: its q_* end mark (device-lost log)
    };
    // #845 / #917: the last submitted async frames, for the device-lost report
    // (gVkAsyncLostHook): every segment's queue, waits and signal, in submission order.
    struct AsyncFrameRec
    {
        uint32_t frame = 0;      // address-registry frame (vkAddrCurrentFrame) at the submit
        uint32_t slot = 0;
        uint32_t n = 0;
        struct Seg { bool compute; const char* name; uint64_t waitGfx, waitComp, waitComp2, signal; } seg[32];
    };
    static constexpr uint32_t kAsyncFrameLog = 4;
    AsyncFrameRec mAsyncFrameLog[kAsyncFrameLog] = {};
    uint32_t mAsyncFrameLogN = 0;
    static void asyncLostHook();
    struct AsyncFlags
    {
        bool on = false;        // segmenting this recording (opted in, queue present, master on)
        bool rt = false;        // RenderVKAsyncRTBuild
        bool maskAo = false;    // RenderVKAsyncMaskAO
        bool godRays = false;   // RenderVKAsyncGodRays
        bool volumetrics = false;   // RenderVKAsyncVolumetrics (#718: the froxel passes beside pass 1b)
        bool post = false;      // RenderVKAsyncPost (graphics-capable async family only)
        bool probeRt = false;   // RenderVKAsyncProbeRT (ray-traced probe capture beside pass 1b)
        bool pipe = false;      // RenderVKFramePipelining (phase 3b; forces post off)
    };
    AsyncFlags mAsyncNow;
    bool mAsyncAvail = false;        // queue + timelines + pools created (initAsync)
    bool mAsyncComputeOnly = false;  // the async family has no graphics bit
    bool mAsyncTs = false;           // the async family writes timestamps
    bool mAsyncOptIn = false;        // this frame: asyncBeginFrame was called
    VkQueue mAsyncQueue = VK_NULL_HANDLE;
    VkSemaphore mTlGfx = VK_NULL_HANDLE;    // timeline: graphics fork points
    VkSemaphore mTlComp = VK_NULL_HANDLE;   // timeline: compute segment ends
    uint64_t mTlGfxNext = 0, mTlCompNext = 0;   // last value handed out on each
    static constexpr uint32_t kAsyncSlots = 2;   // == kRingSlots (frames in flight; static_assert in the .cpp)
    static constexpr uint32_t kAsyncCompPerSlot = 11;   // A B C D, probe trace, front end F, deferred lighting E, volumetric fog V, GI lag L (#978), + headroom
    // (+2: the ray-traced probe capture's fork; +2 frame pipelining: the front end's join and the P split;
    //  +2: the async deferred lighting fork / join; +2: the volumetric fog's fork / join-up-to, #718;
    //  +2: the one-frame-latency GI segment's fork / join-up-to, #978 phase 2.)
    static constexpr uint32_t kAsyncGfxPerSlot = 20;
    VkCommandPool   mAsyncCompPool[kAsyncSlots] = {};
    VkCommandBuffer mAsyncCompCmds[kAsyncSlots][kAsyncCompPerSlot] = {};
    VkCommandBuffer mAsyncGfxCmds[kAsyncSlots][kAsyncGfxPerSlot] = {};
    uint32_t mAsyncCompUsed = 0, mAsyncGfxUsed = 0;
    uint64_t mAsyncSlotCompLast[kAsyncSlots] = {};   // host wait before reusing the slot's pool
    uint64_t mAsyncJoinedLast = 0;      // newest compute value already joined by graphics
    uint64_t mAsyncPostNew = 0;         // this frame's post segment (not joined in-frame)
    uint64_t mAsyncPostPrev = 0;        // last frame's: joined after this frame's front end
    uint64_t mAsyncCompWaitGfx = 0;     // the open compute segment's fork value
    VkCommandBuffer mAsyncWsi = VK_NULL_HANDLE;   // swapchain clear (acquire wait only here)
    uint32_t mAsyncSlot = 0;
    std::vector<AsyncSeg> mAsyncSegs;   // closed segments, recording order
    VkCommandBuffer mAsyncGfx = VK_NULL_HANDLE;   // the open graphics command buffer
    AsyncSeg mAsyncGfxOpen;             // its pending waits
    uint64_t mAsyncOutstanding = 0;     // newest compute value graphics has not joined yet
    uint32_t mAsyncForks = 0;           // per frame (diagnostic)
    bool initAsync();
    void destroyAsync();
    void latchAsync();
    // RenderVKAsyncDeferredLighting resolved: -1 (default) = auto -- on when the async queue is
    // a dedicated compute-only family (mAsyncComputeOnly), off on a second graphics-family
    // queue; 0 / 1 = the user's explicit choice. (The other gates -- an async queue this frame,
    // the early pre-pass, RenderVKProbeEarly -- still apply at the call site.) why: log text.
    bool asyncDeferLightingWanted(const char** why = nullptr) const;
    // Split points (recordWorld top level only, outside rendering scopes; `cmd` is the
    // open graphics command buffer and is REPLACED by its continuation):
    //  fork  -- ends `cmd` (signals the graphics timeline), returns a compute command
    //           buffer whose submission waits that value (ALL_COMMANDS): everything
    //           recorded on graphics so far happens-before it.
    //  end   -- ends the compute command buffer (signals the compute timeline).
    //  join  -- ends `cmd`; its continuation waits every compute value handed out so far
    //           (ALL_COMMANDS): everything on compute happens-before what follows.
    // label: the segment's q_* name (#845 / #917 diagnostics: the compute-queue transition log).
    VkCommandBuffer asyncFork(VkCommandBuffer& cmd, const char* label = nullptr);
    void asyncEndCompute(VkCommandBuffer cc, const char* mark);
    void asyncJoin(VkCommandBuffer& cmd);
    // join UP TO -- as join, but its continuation waits only compute value `v` (one segment
    // and everything before it on the compute queue): later segments stay outstanding
    // (volumetric fog #718: its consumers must not wait for the ray-traced probe faces
    // queued behind it). No-op when `v` is 0 or already joined.
    void asyncJoinUpTo(VkCommandBuffer& cmd, uint64_t v);
    VkCommandBuffer asyncNextGfxCmd();
    bool mAsyncSegStart = false;        // next sub-mark on the async queue opens a segment
    bool mAsyncSegEnd = false;          // ... closes it

    // ---- FRAME PIPELINING (roadmap phase 3b) ----
    // Frame N's front end (streams, anim, fills, main cull, lights / clusters / frame info,
    // skin palettes, pre-skin) is recorded into its own async compute segment F(N) whose
    // submission waits ONLY P(N-1): a graphics timeline value frame N-1 signals right after
    // its last graphics reader of anything the front end writes (after the HUD; after the
    // late probe capture when the faces are not early). So F(N) runs on the compute queue
    // beside frame N-1's tail (probe filters, UI replay, present prep) and frame N's own
    // pre-front-end graphics (UI uploads / offscreen targets, texture streaming, media);
    // frame N's graphics joins F(N) where the front end used to end. No resource is
    // duplicated: P(N-1) is after every cross-frame reader, the semaphore chain orders the
    // rest. Serial fallback (front end on graphics, the 3a recording exactly): toggle off,
    // no P from the previous recording, grow copies this frame, offscreen.
    uint64_t mPipeSignal = 0;           // P of the last recording (graphics timeline; 0 none)
    uint64_t mPipeWait = 0;             // this recording's: the previous one's P (consumed at entry)
    VkCommandBuffer mFeCmd = VK_NULL_HANDLE;   // the open front-end segment (its own timer pool)
    // Opens a compute segment waiting `waitGfx` on the graphics timeline WITHOUT ending the
    // open graphics command buffer (unlike asyncFork); reserves the graphics continuation
    // its join needs. Null = record on graphics.
    VkCommandBuffer asyncOpenCompute(uint64_t waitGfx, const char* label = nullptr);
    // Ends the open graphics command buffer, signals the graphics timeline, continues in a
    // fresh one; returns the signalled value (0 = not split).
    uint64_t asyncSplitGfx(VkCommandBuffer& cmd);
    void pipeSignal(VkCommandBuffer& cmd);   // P: the split above, remembered for the next frame
private:

    // ---- PARAM RING (#238: push constants <= 128 B) ----
    // One persistently mapped, host-coherent buffer (DEVICE_LOCAL | HOST_VISIBLE when the
    // device has such a type -- the BAR heap -- else plain host memory), never resized,
    // bound ONCE at init. Regions, each [frame slot 0 | frame slot 1]:
    //   draw views  : kDrawViewsPerSlot x 256 B (VKDrawPush)  -> draw set 0 binding 36 (SSBO)
    //   cull params : kCullParamsPerSlot x 224 B (VKCullPush) -> cull sets binding 25 (SSBO)
    //   TAA params  : kTaaParamsPerSlot x 192 B (TAAParams)   -> TAA sets binding 14 (UBO)
    // VIEW-SLOT ALLOCATION: a per-frame bump allocator, reset at the top of recordWorld
    // for mFrameSlot (the slot's previous submission has completed by then: acquire()
    // waited its fence; renderOffscreen / the synth test wait idle / their fence). Each
    // pushDrawView / pushCullParams / TAA dispatch takes the next entry unless its bytes
    // equal one of the last kRingRecent entries (probe faces alternate static / rigged
    // views; the F8 alpha pass and the cull's three phases reuse one). Pushed indices are
    // ABSOLUTE (slot base + local) because the descriptors are static across both slots.
    // There is no fixed view numbering: main, rigged-main (eye-relative), HUD, mirror
    // (+ rigged), every probe face (static / rigged, scheduled + realtime), every cascade
    // (+ rigged, + translucency), every projector tile (static / dynamic), selection
    // colours, particle passes -- each gets whatever entry it is handed this frame.
    // Overflow (never expected: ~a few hundred used at most) logs once and reuses the
    // last entry (wrong view data, no out-of-range read).
    static constexpr uint32_t kRingSlots          = 2;     // == swapchain framesInFlight
    static constexpr uint32_t kDrawViewsPerSlot   = 2048;
    static constexpr uint32_t kCullParamsPerSlot  = 256;
    static constexpr uint32_t kTaaParamsPerSlot   = 4;
    static constexpr uint32_t kTaaParamsBytes     = 192;   // clean_taa.comp TaaParams (std140)
    static constexpr uint32_t kRingRecent         = 4;
    struct ParamRegion
    {
        VkDeviceSize offset = 0;     // byte offset of slot 0 in mParamRing
        uint32_t     stride = 0;     // entry bytes
        uint32_t     perSlot = 0;    // entries per frame slot
        uint32_t     base = 0;       // this frame's first absolute entry
        uint32_t     used = 0;       // entries taken this frame
        uint32_t     recent[kRingRecent] = {};             // absolute indices ...
        uint8_t      recentBytes[kRingRecent][256] = {};   // ... and their CPU copies
        uint32_t     nrecent = 0;
        uint32_t     head = 0;       // next recent[] slot to overwrite
        uint32_t     peak = 0;       // most entries one frame used (log)
        uint32_t     loggedPeak = 0; // last peak logged
        bool         overflowLogged = false;
        const char*  name = "";
    };
    GpuBuffer   mParamRing{};
    ParamRegion mViewRgn, mCullRgn, mTaaRgn;
    bool        mParamRingDeviceLocal = false;
    // #814: the dynamic offset of draw-view entry `view` (absolute) in the deferred set's
    // binding 10 (a one-entry UNIFORM_BUFFER_DYNAMIC window at mViewRgn.offset). The stride is
    // 256 (VKDrawPush), a multiple of every minUniformBufferOffsetAlignment (<= 256 by the spec).
    uint32_t deferViewOffset(uint32_t view) const { return view * mViewRgn.stride; }
    // Create + map the ring (init, before any layout that binds it).
    bool createParamRing();
    // Reset every region to frame slot mFrameSlot (top of recordWorld).
    void beginParamRings();
    // Store `bytes` (<= region stride) in the region; returns the ABSOLUTE entry index.
    uint32_t ringAlloc(ParamRegion& r, const void* data, uint32_t bytes);
    // Store an entry the GPU will (partly) overwrite (#478: probe faces): never deduplicated
    // and never offered to later ringAlloc dedupes. Returns the ABSOLUTE entry index.
    uint32_t ringReserve(ParamRegion& r, const void* data, uint32_t bytes);
    // Push an already stored draw view / cull params entry (index from ringReserve).
    void pushDrawViewIndex(VkCommandBuffer cmd, VkPipelineLayout layout, uint32_t view);
    void pushCullSlot(VkCommandBuffer cmd, uint32_t slot, uint32_t phase, uint32_t mode, uint32_t slot_b = ~0u);
    // Store a draw view and push {view, f8 = 0} on `layout` (a draw-family layout).
    uint32_t pushDrawView(VkCommandBuffer cmd, VkPipelineLayout layout, const VKDrawPush& v);
    // Store the cull params (phase zeroed) and push {slot, p.phase, slot_b, mode} on
    // mCullLayout; returns the entry's ring index (a multi-view dispatch names it again).
    uint32_t pushCullParams(VkCommandBuffer cmd, const VKCullPush& p, uint32_t mode = 0u,
                            uint32_t slot_b = ~0u);
    // Cull dispatch mode bits (VKCullSlotPush::mode = clean_cull.comp CULL_MODE_*).
    static constexpr uint32_t kCullModeIdList = 1u;   // HUD: phases 0-2 over the HUD id list (binding 26)
    static constexpr uint32_t kCullModeStats  = 2u;   // probe: funnel counts (kProbeStat*)
    static constexpr uint32_t kCullModeWidth  = 4u;   // probe: size cutoff on the oriented middle extent
    static constexpr uint32_t kCullModeTight  = 8u;   // probe: face masks from the bounds, no origin box
    static constexpr uint32_t kCullModeCluster = 16u; // main cull: skip cluster-drawn instances (#745)
    // PROBE COUNTER BLOCKS (private probe set): one block of kMvCtrStride words per view
    // (multi-view: 6 realtime faces, the union alpha list, the scheduled face); a block
    // holds the usual counterSlot()/6/7 slots and, in view 0's, the funnel counts.
    static constexpr uint32_t kMvCtrStride = 32;   // = clean_cull.comp MV_CTR_STRIDE
    static constexpr uint32_t kProbeStatKept = 28, kProbeStatSizeCut = 29, kProbeStatOver = 30;
    static constexpr uint32_t kProbeViews = 8;
    static constexpr uint32_t kMvScanViews = 7;    // phase 6 workgroups: six faces + the union alpha list
    static constexpr uint32_t kMvWgStride = 36;    // = clean_cull.comp MV_WG_STRIDE (wg_counts per workgroup)
    uint32_t probeCullMode() const;   // the probe-capture mode bits from the settings
    // Write the ring's descriptors into a cull set (binding 25).
    void writeCullParamDescriptor(VkDescriptorSet set);

    // Cull outputs: buckets + counters.
    // 0 opaque 1 mask 2 alpha 3 r_opaque 4 r_mask 5 r_alpha, then the DOUBLE-SIDED
    // (glTF mDoubleSided) sets drawn without back-face culling: 6 ds_opaque 7 ds_mask
    // 8 ds_r_opaque 9 ds_r_mask. Everything else is culled back like LL.
    static constexpr uint32_t NUM_BUCKETS = 10;
    GpuBuffer mCmd[NUM_BUCKETS]{};
    // Alpha ordering (clean_alpha_sort.comp): post-water alpha draws (0 static, 1
    // rigged); counters[6]/[7] hold their counts, [2]/[5] the pre-water counts. The
    // double-sided buckets 6..9 count in slots 8..11 (counterSlot, = clean_cull.comp).
    static constexpr uint32_t NUM_COUNTERS = 19;
    static constexpr uint32_t counterSlot(uint32_t b) { return b < 6 ? b : b + 2; }
    // [F2 waterlists] slots 17 / 18: the alpha sort's dedicated WATER and EXCLUSION
    // (invisiprim) lists (mWaterList / mExclList; clean_alpha_sort*.comp). The cull
    // never writes them (the frame's counter fill zeroes them).
    static constexpr uint32_t WATER_LIST_SLOT = 17;
    static constexpr uint32_t EXCL_LIST_SLOT  = 18;
    // (the probe counter blocks keep their funnel counts past every counter slot)
    static_assert(NUM_COUNTERS <= 28, "probe counter block: slots 28..30 are the probe funnel counts");
    // REDUCED-DETAIL TIER (#31): the main cull places a no-shadow avatar's rigged draws
    // AFTER its bucket's casters (clean_cull.comp twin buckets); slots 12..16 count the
    // casters of rigged buckets 3 4 5 8 9 -- what the shadow / projector passes draw.
    static constexpr uint32_t shadowCounterSlot(uint32_t b)
    { return b == 3 ? 12 : b == 4 ? 13 : b == 5 ? 14 : b == 8 ? 15 : b == 9 ? 16 : counterSlot(b); }
    // = clean_cull.comp WGS (wg_counts stride: NBI + off-screen columns + #559's rigged
    // demand columns, RIG_TIERS x {skinned verts, work items} + the terrain list column)
    static constexpr uint32_t kCullScanBuckets = 44;
    // G-BUFFER TERRAIN LIST (RenderVKGbufTerrainSplit; clean_cull.comp binding 30 TERR_COL,
    // written by the main cull only): the terrain draws of buckets 0 / 1 again, in their bucket
    // order -- a count word, then mBucketCap DrawCmds at kTerrListHdrBytes. The deferred
    // G-buffer draws terrain from it with the terrain pipeline; every other pass keeps reading
    // the buckets (terrain stays in them). Zeroed per frame with the counters.
    static constexpr VkDeviceSize kTerrListHdrBytes = 32;   // = uint terr_count; uint terr_pad[7]
    GpuBuffer mTerrList{};
    // OFF-SCREEN RIGGED REGIONS (#517; clean_cull.comp binding 28, written by the main cull
    // only): the rigged draws of avatars the main view does not see, per rigged bucket
    // (3 4 5 8 9 -> k 0..4) and pass (kRigOff*: sun cascades, projector tiles, planar
    // mirror, realtime probe capture) -- each pass adds exactly its own with drawRigOff.
    // Layout: kRigOffHdrBytes of counts (word k * kRigOffRegions + r), then rigOffCap()
    // DrawCmds per region at region (k * kRigOffRegions + r). A region holds at most one
    // draw per pre-skinned rigged instance: sized to the work list (ensureRiggedBuffers,
    // as mProbeRigCmds), its capacity in VKCullPush::rig_off_cap / the probe rig cull push.
    static constexpr uint32_t kRigOffSun = 0, kRigOffProj = 1, kRigOffMirror = 2, kRigOffProbe = 3;
    static constexpr uint32_t kRigOffRegions = 4;
    static constexpr VkDeviceSize kRigOffHdrBytes = 128;   // = uint rig_off_count[32]
    GpuBuffer mRigOff{};
    uint32_t  mRigOffCap = 0;   // draws per region (0 = no buffer)
    uint32_t  rigOffCap() const { return mRigOffCap; }
    static constexpr uint32_t rigOffIndex(uint32_t b) { return b == 3 ? 0 : b == 4 ? 1 : b == 5 ? 2 : b == 8 ? 3 : 4; }
    // One indirect draw of region (rigged bucket b, pass r); the caller binds the pipeline.
    void drawRigOff(VkCommandBuffer cmd, uint32_t b, uint32_t r) const;
    // Rigged alpha-blend casters (bucket 5) as the cull emitted them, copied before the
    // alpha sort reorders the bucket in place: the shadow translucency layer draws its
    // leading shadowCounterSlot(5) casters.
    GpuBuffer mShadowRigAlpha{};
    bool      mShadowRigAlphaValid = false;
    GpuBuffer mAlphaPost[2]{};
    // F2 (RenderVKWaterLists): the main sort scatters water and invisiprims out of the
    // static alpha lists into these (alpha-sort bindings 11 / 12, counters
    // WATER_LIST_SLOT / EXCL_LIST_SLOT); the water / exclusion pipes draw only them.
    // The probe water split (sort mode 3) writes its water to binding 11 too.
    GpuBuffer mWaterList{}, mExclList{};
    bool      mWaterListsNow = false;   // this frame's main sort scattered them (latched at the sort)
    VkDescriptorSetLayout mDslAlphaSort = VK_NULL_HANDLE;
    VkDescriptorSet       mDsetAlphaSort = VK_NULL_HANDLE;
    VkDescriptorSet       mDsetProbeSort = VK_NULL_HANDLE;   // same layout, mode 2: mask / opaque buckets
    VkPipelineLayout      mAlphaSortLayout = VK_NULL_HANDLE;
    VkPipeline            mAlphaSortPipe = VK_NULL_HANDLE;
    // Alpha sort past the 4096-draw single-workgroup cap (B9, clean_alpha_sort_large.comp):
    // key scratch (binding 10 of mDsetAlphaSort), its per-bucket capacity (pow2), and the
    // hysteresis state of the readback hint that records it.
    VkPipelineLayout      mAlphaSortLargeLayout = VK_NULL_HANDLE;
    VkPipeline            mAlphaSortLargePipe = VK_NULL_HANDLE;
    GpuBuffer             mAlphaSortKeys{};
    uint32_t              mAlphaSortKeysCap = 0;
    double   mAlphaSortRetryAt = 0.0;   // #697: a refused key-buffer grow backs off until then
    bool                  mAlphaSortLargeOn = false;
    // Single-workgroup capacity of the sort pipelines in use (#238): 4096, or 2048 with the
    // small-shared-memory variants (device maxComputeSharedMemorySize < kAlphaSortSmemBytes).
    uint32_t              mAlphaSortMaxS = 4096;
    bool ensureAlphaSortKeys(uint32_t per_bucket);
    void recordAlphaSortLarge(VkCommandBuffer cmd, const float cam[4], uint32_t under, uint32_t nmax, uint32_t flags);
    void writeAlphaSortDescriptors();
    GpuBuffer mCounters{};
    // Stable-compaction scratch (cull phases): per-instance candidate draw + bucket/
    // rank, per-workgroup bucket counts -> offsets. Indexed by instance id: mCullIdCap.
    GpuBuffer mCand{}, mCandMeta{}, mWgCounts{};
    uint32_t  mCullIdCap = 0;   // instance ids the scratch covers (>= mInstanceCount)
    // Run the three-phase cull (candidate -> scan -> emit) with the given push.
    // `set` = the cull descriptor set (main buckets by default; the shadow set for cascades).
    // `mode` = kCullMode* bits for VKCullSlotPush::mode, `slot_b` its slot_b word (HUD id
    // list: the list's first entry in mHudIdBuf; push.instance_count = its length).
    void dispatchCull(VkCommandBuffer cmd, VKCullPush push, VkDescriptorSet set = VK_NULL_HANDLE,
                      uint32_t mode = 0u, uint32_t slot_b = ~0u);
    // F7: HUD instance list -- the ascending ids of the HUD-flagged rows (flags bit 3) of
    // the CPU instance mirror, kept from the dirty instance runs; the HUD cull walks it
    // (cull binding 26, this frame slot's half of mHudIdBuf) instead of every row.
    static constexpr uint32_t kHudIdCap = 16384;   // per frame slot; more -> the full-scan HUD cull
    GpuBuffer             mHudIdBuf{};
    std::vector<uint8_t>  mIsHudInst;
    std::vector<uint32_t> mHudIds;
    bool                  mHudIdsDirty = true;
    void refreshHudIds(bool full);
    // FUNNEL DIAG (behavior-neutral): host-visible mirror of mCounters. recordWorld
    // copies the per-bucket draw counts here each frame so the CPU can log how many
    // draws the compute cull emitted (reads the prior frame's values; the every-frame
    // vkDeviceWaitIdle in syncFromStores keeps it coherent).
    GpuBuffer mCounterReadback{};
    // FUNNEL DIAG: per-reason cull reject histogram (binding 10 in the cull shader).
    // Device-local, zeroed each frame; a host-visible mirror is read back like the
    // draw counters so the CPU can log WHERE the live instances die.
    // 11..15: reduced tier #31 (14/15: skin cache); 16/17: #517 off-screen rigged;
    // 18..23: #559 rigged demand (always on): skinned verts per distance tier (18..21),
    // work items (22), skinned verts dropped for capacity (23)
    // 24 / 25: #561 rigged draws at a decimated step / skinned vertices saved (always on)
    // 26 / 27: cluster LOD legacy-skin skip: rigged instances skipped / their skinned vertices
    // (always on; clean_cull.comp R_CLSKIP_*)
    static constexpr uint32_t NUM_DBG_REASONS = 28;
    // The host readback holds the reasons + extra words copied from other buffers:
    // kDbgRbCacheUsed = rig_alloc.cache_used (word 4 of mRigAlloc) after the main cull.
    static constexpr uint32_t kDbgRbCacheUsed = NUM_DBG_REASONS;
    static constexpr uint32_t kDbgRbWords = NUM_DBG_REASONS + 4;
    GpuBuffer mDbgReasons{};
    GpuBuffer mDbgReadback{};
    // Output-list capacity (every DrawCmd list / count clamp). A list holds at most one
    // entry per live instance, so it follows the live high-water (mInstLiveCount, captured
    // with the mirror), x1.5, grow-only, never above the instance-id range.
    uint32_t  mBucketCap = 0;
    uint32_t  mdiMax() const;   // the bucket lists' live-instance bound (mdiMaxSlot / mdiMaxBucket tighten it)
    // Per-bucket draw bounds (updateInstClassCounts): live mirror rows per bucket class.
    uint32_t  mdiMaxBucket(uint32_t b, bool dyn = false) const;   // dyn: dynamic casters only
    uint32_t  mdiMaxSlot(uint32_t counter_slot) const;
    void      updateInstClassCounts(size_t run_from);
    void      checkBucketBounds(const uint32_t* main_counts);
    std::vector<uint8_t> mInstClass;   // per row: bucket | 0x10 dynamic caster, 0xFF free
    uint32_t  mClassCount[NUM_BUCKETS] = {};
    uint32_t  mClassDynCount[NUM_BUCKETS] = {};
    static constexpr uint32_t kClassRing = 4;
    uint32_t  mClassRing[kClassRing][2][NUM_BUCKETS] = {};   // [frame][all, dynamic][bucket]
    uint32_t  mClassRingPos = 0;
    void      bucketDraw(VkCommandBuffer cmd, VkBuffer buf, VkDeviceSize off, VkBuffer cnt, VkDeviceSize cnt_off,
                         uint32_t max_draws, uint32_t stride) const;   // every mdiMax() draw
    uint32_t  mInstLiveCount = 0xFFFFFFFFu;   // store live count at the last mirror copy (~0 = unknown)
    // Probe private buckets: per-view region basis, grow-only. Kept on the instance-id
    // range (x1.5, the pre-split mBucketCap) so the per-face drop threshold is unchanged.
    uint32_t  mProbeBasis = 0;

    // Depth (renderer-owned; resized to the swapchain).
    GpuImage  mDepth{};
    uint32_t  mDepthW = 0, mDepthH = 0;
    VkFormat  mDepthFormat = VK_FORMAT_D32_SFLOAT;

    // Glow. mGlowTarget is the world passes' 2nd color attachment (color * glow);
    // mGlowE (extract) / mGlowP0 / mGlowP1 are the 512^2 blur chain (GENERAL layout,
    // storage-written + sampled). Result ends in mGlowP1.
    static constexpr uint32_t GLOW_RES = 512;   // LL RenderGlowResolutionPow 9
    // mGlowFormat = the world passes' glow ATTACHMENT (mGlowTarget; every pass 1 / 2
    // pipeline's 2nd colour format). I-6 (RenderVKGlowTargetCompact, startup): the packed
    // B10G11R11 float format when it blends + samples -- half the bandwidth of a 4K target
    // that is zero almost everywhere; nothing reads the target's alpha (blending uses the
    // fragment's). The storage images downstream (TAA glow history, the 512^2 chain)
    // stay kGlowChainFormat: their shaders declare rgba16f.
    VkFormat  mGlowFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    static constexpr VkFormat kGlowChainFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    GpuImage  mGlowTarget{};
    // LINEAR HDR scene target: sky + world passes render here; the tonemap pass
    // resolves it to the swapchain (LL: screen -> tonemap/gamma -> post).
    VkFormat  mHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    GpuImage  mHdr{};
    uint32_t  mHdrW = 0, mHdrH = 0;
    GpuImage  mExposureImg{};            // 1x1 R32F dynamic exposure (GENERAL)
    bool      mExposureReset = true;     // snap (no easing) on the first dispatch
    VkDescriptorSet  mDsetTonemap = VK_NULL_HANDLE;   // b0 = HDR scene, b1 = exposure
    VkPipelineLayout mTonemapLayout = VK_NULL_HANDLE, mExposureLayout = VK_NULL_HANDLE;
    VkPipeline       mTonemapPipe = VK_NULL_HANDLE, mExposurePipe = VK_NULL_HANDLE;
    // Exposure set 1 (#28, LL luminanceF weighting): b0 = world depth (sky test), b1 =
    // HDR bloom mip 0 (LL adds its glow buffer). Rewritten when either view / the target
    // generation changes (recordTonemap).
    VkDescriptorSetLayout mDslExposureAux = VK_NULL_HANDLE;
    VkDescPool       mExposureAuxPool;
    VkDescriptorSet  mDsetExposureAux = VK_NULL_HANDLE;
    VkImageView      mExpAuxDepthView = VK_NULL_HANDLE, mExpAuxBloomView = VK_NULL_HANDLE;
    uint64_t         mExpAuxGen = 0;
    void ensureHdr(uint32_t w, uint32_t h);
    // RENDER SCALE (#181, RenderVKRenderScale). The main view's scene stages (depth
    // pre-pass, passes 1/2, AO, SSR, water, motion vectors, occlusion) run at the render
    // extent mRenderW x mRenderH; everything from the TAA resolve on (post chain,
    // selection, overlays, HUD, UI) at the display extent. At scale 1 the render extent
    // IS the display one and the scene / display-depth accessors return mHdr / mDepth
    // themselves: no extra image, copy or pass exists. Below 1, the scene renders into
    // mSceneHdr, the resolve (or a linear blit when TAA can't run) upsamples it into
    // mHdr, and mDepthDisp holds a nearest upscale of the world depth for the display-
    // space passes that depth-test against it.
    GpuImage  mSceneHdr{};
    uint32_t  mSceneW = 0, mSceneH = 0;
    // RENDER-TARGET GENERATION REGISTRY (#181, snapcrash #1 / #2). The contract every
    // size-dependent image and every descriptor set that names one follows:
    //  1. Every re-creation of a size-dependent target (depth, HDR, scene, glow, motion,
    //     SSR / refraction / water-exclusion / reactive copies, AO, RT mask, TAA history,
    //     Hi-Z, VRS rate, G-buffer, scale targets) bumps mTargetGen; the depth also bumps
    //     mDepthGen. Offscreen renders (snapshots, 360, the stress test) re-create them at
    //     their own size, and endOffscreen() bumps both again for the next on-screen frame.
    //  2. A descriptor set that names such an image is EITHER written in the same function,
    //     right after the image's re-creation (owner-written: tonemap, glow extract, water
    //     depth, set 0 bindings 7/8/17/21/31-34, AA, bloom mips, final, ...), OR it records
    //     the generation it was written with and is rewritten before use when that differs
    //     (gen-keyed: TAA, RCAS, deferred, VRS, flare, bloom extract, DoF, AO / Hi-Z / RT
    //     mask (mDepthGen), occlusion, depth upscale, god rays (depthGen), FSR (targetGen),
    //     RT consumer set (rewritten every frame)).
    //  3. A VIEW-HANDLE COMPARE ALONE IS NEVER A VALID "unchanged" TEST: a freed view's
    //     handle value comes straight back for its replacement (same-size allocation), and
    //     the old set keeps pointing at the freed image. #181 (bloom), snapcrash #1
    //     (deferred lighting), snapcrash #2 (FSR 3.1: its own resources keep their size, so
    //     only its handle compare could notice the re-created inputs -- it did not).
    //  4. Modules outside this class (FSR, god rays, RT) get the generation through their
    //     Frame / a friend read, never a handle.
    // RenderVKDebugDescriptorTracking (on with RenderVKDebugSnapshotStress; vk_common.h
    // vkDescTrack*) checks rule 3 at bind time: a bound set naming a destroyed or recycled
    // view is logged ("STALE DESCRIPTOR bound") with the image's name and frames.
    uint64_t  mTargetGen = 1;
    uint64_t  mFlareGen = 0, mDofGen = 0, mAoGen = 0, mOcclGen = 0, mTaaGen = 0, mBloomGen = 0;
    uint64_t  mDeferGen = 0, mVrsGen = 0, mOccl2Gen = 0;
    // Bumped only when mDepth is re-created (ensureDepth). For sets that point at the
    // depth but are written BEFORE the frame's other targets (the RT mask set): keyed on
    // mTargetGen they would rewrite (and wait idle) again on the next frame.
    uint64_t  mDepthGen = 1;
    GpuImage  mDepthDisp{};
    uint32_t  mDepthDispW = 0, mDepthDispH = 0;
    uint32_t  mRenderW = 0, mRenderH = 0;
    bool      mScaled = false;           // this frame: render extent < display extent
    int8_t    mDepthBlitOk = -1;         // -1 unknown; depth format supports blit src/dst
    const GpuImage& sceneHdr() const     { return mScaled ? mSceneHdr : mHdr; }
    const GpuImage& displayDepth() const { return mScaled ? mDepthDisp : mDepth; }
    float renderScaleWanted();
    void  ensureScaleTargets(uint32_t w, uint32_t h, uint32_t rw, uint32_t rh);
    void  destroyScaleTargets();
    double mScaleRetryAt = 0.0;   // #697: refused scaled targets -> native until then
    void  recordSceneUpscaleBlit(VkCommandBuffer cmd);
    void  recordDepthUpscale(VkCommandBuffer cmd);
    // clean_depth_up.comp: the unjittered display depth, via a buffer (a depth image
    // can't be a storage image) copied into mDepthDisp. Blit fallback without it.
    VkDescriptorSetLayout mDslDepthUp = VK_NULL_HANDLE;
    VkDescPool            mDepthUpPool;
    VkDescriptorSet       mDsetDepthUp = VK_NULL_HANDLE;
    VkPipelineLayout      mDepthUpLayout = VK_NULL_HANDLE;
    VkPipeline            mDepthUpPipe = VK_NULL_HANDLE;
    GpuBuffer             mDepthUpBuf{};
    VkDeviceSize          mDepthUpCap = 0;
    uint64_t              mDepthUpGen = 0;          // mTargetGen the set was written for
    VkBuffer              mDepthUpSetBuf = VK_NULL_HANDLE;
    // SHARPENING (#183; clean_rcas.comp, RenderCASSharpness). Sets: [0] / [1] read TAA
    // history 0 / 1, [2] reads the SSR copy (non-TAA frames); all write mHdr (storage).
    VkDescriptorSetLayout mDslRcas = VK_NULL_HANDLE;
    VkDescPool            mRcasPool;
    VkDescriptorSet       mDsetRcas[3] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkPipelineLayout      mRcasLayout = VK_NULL_HANDLE;
    VkPipeline            mRcasPipe = VK_NULL_HANDLE;
    uint64_t              mRcasGen = 0;
    float sharpenAmount() const;
    bool  updateRcasSets();
    void  dispatchRcas(VkCommandBuffer cmd, uint32_t set, uint32_t w, uint32_t h, float amount);
    void  recordSharpenNoTaa(VkCommandBuffer cmd, uint32_t w, uint32_t h);
    // GPU pass timers (RenderVKGpuTimers).
    static constexpr uint32_t TS_COUNT = 9;
    // Query pools are a RING (one per recent frame): a frame's results are read
    // TS_RING-1 frames later, when they are certainly complete. A single pool read one
    // frame late lost most samples once frames overlapped on the GPU (phase 2).
    static constexpr uint32_t TS_RING = 4;
    VkQueryPool mTsPoolR[TS_RING] = {};
    uint32_t    mTsSlot = 0;
    bool        mTsPendingR[TS_RING] = {};
    VkQueryPool mTsPool = VK_NULL_HANDLE;   // = mTsPoolR[mTsSlot] while recording
    float    mTsPeriodNs = 1.f;
    uint64_t mTsMask = ~0ull;           // timestampValidBits of the graphics family (#790)
    bool     mTsUnavailable = false;    // no timestamps / pools failed: timers stay off (#790)
    bool     mTsOn = false, mTsPending = false;
    double   mTsAccum[TS_COUNT] = {};
    double   mTsWorst[TS_COUNT] = {};   // the window's slowest GPU frame (per pass)
    uint32_t mTsFrames = 0;
    double   mTsNextLog = 0.0;
    // #946 OUTSIDE THE TIMERS: one timestamp at the very TOP of each world recording, per ring
    // slot. Timestamp 0 is written only after the prologue -- the rigged fills, the texture
    // uploads / media updates, the target ensures and the weather's prepare (its snow mesh build,
    // run-off flow build, re-creation clears) -- so that GPU time was in no timer. "prologue" =
    // timestamp 0 - top; "gap" = top - the previous frame's last timestamp: the queue's time
    // between our frames (other submissions, WDDM residency / paging, the present). A GPU-bound
    // frame (the CPU waiting in vk_paceWait) with a large gap is stalled OUTSIDE our work.
    VkQueryPool mTsTopPool = VK_NULL_HANDLE;   // TS_RING timestamp queries
    bool     mTsTopR[TS_RING] = {};            // that ring slot's recording wrote its top
    int32_t  mTsTopPending = -1;               // the slot this recording's top went to
    uint64_t mTsPrevEnd = 0;                   // the last harvested frame's final timestamp (raw)
    bool     mTsPrevEndValid = false;
    double   mTsPreAccum = 0.0, mTsGapAccum = 0.0, mTsGapWorst = 0.0;
    uint32_t mTsPreN = 0, mTsGapN = 0;
    void gpuTimersTop(VkCommandBuffer cmd);
    void gpuTimersBegin(VkCommandBuffer cmd);
    void gpuTimerMark(VkCommandBuffer cmd, uint32_t idx);
    // Named sub-marks (RenderVKGpuTimers): timestamps between the pass marks, each
    // interval attributed to the mark that ENDS it; averaged into the VKGpu log line.
    static constexpr uint32_t SUB_TS = 256;  // (realtime probe: 6 faces x 4 marks + mips/filter; +4 two-phase occlusion;
                                             //  #567 RenderVKShadowGpuDetail: up to 3 per projector tile)
    VkQueryPool mSubPoolR[TS_RING] = {};
    const char* mSubNamesR[TS_RING][SUB_TS] = {};
    // Async compute (phase 3a): which queue wrote each sub-mark. Timestamps from two
    // queues are not comparable, so deltas are taken per queue: 0 graphics (delta from
    // the previous graphics mark), 1 async (from the previous async mark), 2 async
    // segment begin (no delta), 3 async segment end (= the segment's total, q_*).
    uint8_t     mSubQueueR[TS_RING][SUB_TS] = {};
    uint8_t*    mSubQueue = mSubQueueR[0];
    uint32_t    mSubCountR[TS_RING] = {};
    VkQueryPool mSubPool = VK_NULL_HANDLE;   // = mSubPoolR[mTsSlot] while recording
    const char** mSubNames = mSubNamesR[0];
    uint32_t    mSubCount = 0;
    // Frame pipelining (3b): the front-end segment's marks go to their own pool, reset by
    // the segment itself -- it can run BEFORE this frame's graphics reset of mSubPool.
    static constexpr uint32_t kFeTs = 40;   // (40: + the cluster pre-skin c_cl_sk_* sub-marks, 2026-10-14)
    VkQueryPool mFePoolR[TS_RING] = {};
    const char* mFeNamesR[TS_RING][kFeTs] = {};
    uint32_t    mFeCountR[TS_RING] = {};
    VkQueryPool mFePool = VK_NULL_HANDLE;    // = mFePoolR[mTsSlot] while recording
    const char** mFeNames = mFeNamesR[0];
    uint32_t    mFeCount = 0;
    // async: measured on the async compute queue (its own clock; logged as a separate
    // group so a mixed list is never read as graphics time).
    struct SubAcc { const char* name; double ms; double worst = 0.0; bool async = false; };
    std::vector<SubAcc> mSubAcc;
    void gpuSubMark(VkCommandBuffer cmd, const char* name);
    // RenderVKGpuSubMarks off: only timestamps 0 and TS_COUNT-1 are written (the frame
    // total), so the timer queries themselves cannot serialize overlapped work.
    bool mTsLean = false;
    bool mTsLeanRec = false;   // the recording frame's mode
    bool mTsLeanR[TS_RING] = {};   // each ring slot's recorded mode
    uint32_t mFrameJobs = 0;          // staggered jobs of the recording frame (RenderVKFrameJobLog)
    uint32_t mTsJobR[TS_RING] = {};   // ... per timer ring slot

    // ---- PHASE-2 OVERLAP (roadmap 2a/2b) ----
    // Latched ONCE per recordWorld (latchOverlap): every deferred release below is
    // issued by the same frame's flags, so a mid-frame settings flip cannot drop one.
    // RenderVKPreciseBarriers off = every item off = the pre-phase-2 recording.
    struct OverlapFlags
    {
        bool maskAo = false;      // RenderVKOverlapMaskAO: one depth transition, RT mask + SSAO interleaved
        bool godRays = false;     // RenderVKOverlapGodRays: march/temporal released at the composite, not before 2a
        bool tlas = false;        // RenderVKOverlapTlas: TLAS -> consumer barrier at the first consumer
        bool particles = false;   // RenderVKOverlapParticles: particle compute -> draw barrier at the first draw
        bool probePrivate = false;// RenderVKProbePrivateCull: probe faces cull into their own buffers
        bool probeEarly = false;  // RenderVKProbeEarly: faces recorded before pass 1, filter at the frame end
    };
    OverlapFlags mOv;
    void latchOverlap();
    // TLAS -> consumers (stages still owed this frame; LLCleanRTVK::releaseToConsumers).
    void releaseTlas(VkCommandBuffer cmd, VkPipelineStageFlags2 stages);
    // Particle compute -> its draws (indirect + VS), owed since recordParticleCompute.
    bool mPartReleasePending = false;
    void releaseParticles(VkCommandBuffer cmd);
    // Probe capture: private cull buckets (opaque / mask / alpha / ds opaque / ds mask,
    // as the mirror's) + counters, its cull set and its two alpha-sort sets (mode 3
    // static alpha back-to-front; mode 2 front-to-back mask / opaque).
    GpuBuffer       mProbeCmd[5]{};
    GpuBuffer       mProbeCounters{};
    GpuBuffer       mProbePost{};   // the probe water split's water list (private alpha-sort binding 11)
    VkDescriptorSet mDsetCullProbe = VK_NULL_HANDLE;
    VkDescriptorSet mDsetProbeAlphaP = VK_NULL_HANDLE;   // mode 3 over mProbeCmd[2]
    VkDescriptorSet mDsetProbeSortP = VK_NULL_HANDLE;    // mode 2 over mProbeCmd[1] / [0]
    bool probePrivateReady() const
    {
        return mDsetCullProbe && mDsetProbeAlphaP && mDsetProbeSortP && mProbeCounters.buffer
            && mProbeCmd[0].buffer && mProbeCmd[4].buffer && mProbePost.buffer && mProbeSortPWritten;
    }
    bool mProbeSortPWritten = false;   // the private alpha-sort pair names live buffers
    bool mProbePrivNow = false;   // this frame's probe faces use the private set
    // Probe filters deferred to the frame end (probe faces recorded early): the main
    // view keeps sampling last frame's filtered probes, as before.
    struct ProbeFilterJob
    {
        VkImage image = VK_NULL_HANDLE;
        const std::vector<VkDescriptorSet>* sets = nullptr;
        uint32_t slot = 0;
        const char* markMips = nullptr;
        const char* markFilter = nullptr;
        uint32_t tsQuery = ~0u;   // mPrTsPool query written after it (~0u none)
        uint32_t traced = 0;      // capture layers the RT trace wrote (GENERAL), see recordProbeFilter
        int      gpuJob = -1;     // #478: the slot / sizes come from this GPU capture job (PJ_*)
        VkDeviceSize jobOff = 0;  // that frame slot's job records (byte offset in mPrbJobs)
    };
    std::vector<ProbeFilterJob> mProbeFilterPending;
    bool mProbeDeferFilter = false;
    // traced: bit per capture layer last written by the ray-traced capture (left in
    // GENERAL by a compute write); the other layers come from raster faces (COLOR_ATTACHMENT).
    void probeFilterOrDefer(VkCommandBuffer cmd, VkImage capImage, const std::vector<VkDescriptorSet>& sets,
                            uint32_t slot, const char* markMips, const char* markFilter, uint32_t tsQuery,
                            uint32_t traced = 0u, int gpuJob = -1);
    void flushProbeFilters(VkCommandBuffer cmd);
    // RT shadow mask + SSAO in phases (recordRtMask / recordAO are the sequential
    // composition; recordAoMaskOverlap interleaves them under one depth transition).
    struct RtMaskPushData { float inv_view_proj[16]; float cam_time[4]; float dims[4]; float p0[4]; float p1[4]; };
    RtMaskPushData mRtMaskPush{};
    void rtMaskSelectSet();                    // this mask frame's set / history slot (F3a)
    void rtMaskHistory(VkCommandBuffer cmd);   // copies, or (F3a) the slot barriers
    void rtMaskPushFill(const VKDrawPush& drawPush, uint32_t rw, uint32_t rh);
    void rtMaskStage(VkCommandBuffer cmd, uint32_t stage);   // 0 trace, 1 temporal, 2 spatial
    // #85 (llcleanrtdenoisevk.cpp): the spatial stage as RenderVKRTShadowAtrous a-trous
    // iterations and / or a RenderVKRTShadowDebug view; false = the plain disc dispatch.
    bool rtMaskSpatialDispatch(VkCommandBuffer cmd);
    // Mask set bindings 16 / 17: each set's READ history slot's sun accumulation / meta
    // (dead after the temporal pass) as the a-trous scratch.
    void writeRtMaskAtrousDescriptors();
    // 0 SSAO / GTAO / RTAO (RenderVKAOMethod), 1 blur H, 2 blur V, 3 the temporal accumulator
    // (#843). rtContact (#86, RTAO's contact blend): stage 0 as the GTAO kernel at the contact
    // radius, without re-latching the method or advancing the noise frame.
    void aoStage(VkCommandBuffer cmd, uint32_t width, uint32_t height, uint32_t stage, bool rtContact = false);
    // #86 RAY-TRACED AO (RenderVKAOMethod 2; clean_rtao.comp): the stage-0 kernel over the TLAS
    // (+ the GTAO contact pass before it, RenderVKRTAOContact). rtaoWanted: the build gate
    // (rtConsumerActive) -- the setting, AO on, the pipeline, no RT stand-down. rtaoReady: this
    // recording can trace (TLAS + consumer set valid); else stage 0 falls back to GTAO.
    bool rtaoWanted() const;
    bool rtaoReady() const;
    void aoStageRt(VkCommandBuffer cmd, uint32_t width, uint32_t height);
    // #843 AO TEMPORAL ACCUMULATOR (RenderVKAOTemporal; clean_ao.comp mode 4).
    // aoWantsTemporal: the setting + AO on (ensureAO sizes the history by it; ensureTemporal
    // keeps the motion / previous-camera plumbing alive for it without TAA / FSR).
    // aoTemporalBegin: latches mAoTemporalNow for this recording (before stage 0).
    // aoTemporalRecord: stage 3 + its sub-mark + the history copy cur -> prev; the barrier
    // after it also orders the kernel -> blur H chain (the caller's next stage).
    bool aoWantsTemporal() const;
    void aoTemporalBegin(uint32_t width, uint32_t height);
    void aoTemporalRecord(VkCommandBuffer cmd, uint32_t width, uint32_t height);
    // AO set binding 8: the motion target, or the depth view as a placeholder (all four sets;
    // callers have waited for the device -- ensureTemporal's re-creation paths).
    void writeAoMotionBinding();
    // GPU sub-mark name for the AO kernel of the method stage 0 latched (A/B timing).
    const char* aoKernelMark() const;
    float aoTapBlendOffset() const;   // #648 round 3: RenderVKAOTapBlend kernel-mode offset
    // Effective AO world radius (m) for this frame's settings (the MIP filter's falloff).
    float aoEffectRadius(uint32_t height) const;
    // depthHeld (#845 / #917, RenderVKAsyncLayoutsOnGraphics): depth is already DEPTH_READ_ONLY
    // (the caller transitioned it on the graphics queue before the fork) and stays so -- the
    // caller returns it to the attachment layout on graphics after the join.
    void recordAoMaskOverlap(VkCommandBuffer cmd, const VKDrawPush& drawPush, uint32_t rw, uint32_t rh,
                             bool ao, bool mask, bool depthHeld = false);
    // AVATAR VISIBILITY for the cull (binding 22): per joint-table slot, the passes that
    // draw the avatar (kAvVis* bits below; 0 = none: hidden by policy, or no pass sees
    // it); [256] = 1 when binding 23 holds the skin meta. RenderVKAvatarVisCull.
    static constexpr uint32_t kAvVisSlots = kAvatarSlotCap;   // (llavatarslots.h: 1024, was 256)
    GpuBuffer mAvVisSSBO{};
    // [0, kAvVisSlots) avatar visibility, [kAvVisSlots] skin-meta valid flag, [+1] the
    // reduced LOD scale, [+2, +3] this frame's realtime probe capture job record (device
    // address lo / hi; 0 = none: clean_cull.comp avProbeHit, #517), then at kCasterBase the
    // shadow-caster tail: 4 cascades x 6 planes x vec4 (96 words), 4 cascade texel sizes,
    // min-caster texels, enable -- all read by clean_cull.comp.
    static constexpr uint32_t kAvVisPrbJob = kAvVisSlots + 2;
    static constexpr uint32_t kCasterBase  = kAvVisSlots + 4;
    static constexpr uint32_t kCasterWords = 96 + 4 + 2;
    // Then per slot {distance from the camera to the avatar's bounds, bounds diameter}
    // (floats; 0 = unknown): texture demand for rigged meshes and worn attachments,
    // whose model_matrix is not a world pose (clean_cull.comp avatarDemand).
    static constexpr uint32_t kAvDemBase = kCasterBase + kCasterWords;
    // REDUCED-DETAIL TIER (#31) bits of av_vis[slot]: bit1 casts no shadow (cascades,
    // projector tiles, RT), bit2 reduced (LOD bias), bits 4..6 = the reduced rigged LOD
    // cap + 1 (LLVOAvatar::vkReducedRiggedLOD; 0 = none). av_vis[kAvVisSlots + 1] = the
    // LOD scale (float) the cull applies to worn attachments. = clean_cull.comp AVV_*.
    // PER-PASS VISIBILITY (#517): bit0 (kAvVisAny) = some CPU-decided pass draws it (test
    // the bit, not the word: an off-screen avatar keeps its tier bits and kAvVisProbeOk);
    // kAvVisMain = the main view (the cull frustum -- frozen with "Freeze culling camera" --
    // or within 16 m of the LOD camera; self, UI and unknown always). Only an avatar
    // WITHOUT kAvVisMain carries the off-screen pass bits, each pass drawing it from its
    // own off-screen region (mRigOff): kAvVisSun (light-swept bounds in the cull frustum),
    // kAvVisProj (a shadowed projector's frustum), kAvVisMirror (the dynamic mirror's),
    // kAvVisProbeOk (the realtime probe capture may pull it in: decided on the GPU against
    // this frame's job, clean_cull.comp avProbeHit). kAvVisRtProbe: RT candidacy only (a
    // ray-traced capture may need it; the TLAS walk can't wait for the GPU's decision).
public:
    static constexpr uint32_t kAvVisAny = 1u;
    static constexpr uint32_t kAvVisNoShadow = 2u, kAvVisReduced = 4u, kAvVisLodShift = 4u;
    // bit3: the reduced avatar's skeleton pose changed this frame -> its cached
    // pelvis-local skinned vertices are re-skinned (skin cache, clean_cull.comp).
    static constexpr uint32_t kAvVisPoseDirty = 8u;
    // #632 HELD-FRAME SKIN CACHE (RenderVKRigHeldSkip): the avatar's animation has held
    // frames (LLVOAvatar::vkAnimRateLimited, GPU-animated, not self) -> its rigged meshes
    // use the persistent pelvis-local skin cache like the reduced tier (#31), shadows and
    // RT included: re-skinned only on kAvVisPoseDirty frames (= clean_cull.comp AVV_HELD).
    static constexpr uint32_t kAvVisHeldCache = 128u;
    static constexpr uint32_t kAvVisMain = 256u, kAvVisSun = 512u, kAvVisProj = 1024u, kAvVisMirror = 2048u;
    static constexpr uint32_t kAvVisProbeOk = 4096u, kAvVisRtProbe = 8192u;
    // #559: bits 16..18 rigged LOD pressure cap + 1 (0 none), bits 20..21 distance tier.
    static constexpr uint32_t kAvVisPressShift = 16u, kAvVisTierShift = 20u;
    // #561: bits 24..26 screen-size rigged LOD tolerance level (0 off; clean_cull.comp AVV_SLOD_SHIFT).
    static constexpr uint32_t kAvVisSlodShift = 24u;
    // CLUSTER LOD LEGACY-SKIN SKIP (clusterLegacySkipPlan; = clean_cull.comp AVV_CL_SKIP): no
    // consumer needs this switched avatar's LEGACY pre-skinned vertices this frame -- its
    // cluster-owned rigged instances get no legacy draw / pre-skin / weld / RT rig copy, and
    // its switched RT groups are cluster-only (no legacy rigged BLAS).
    static constexpr uint32_t kAvVisClSkip = 1u << 27;
    // RT NO-SELF-SHADOW (LLCleanRTVK::classifyNoSelf; = clean_rt_instances.comp AVV_RT_NOSELF):
    // the avatar's rigged RT update may be throttled this frame (RenderVKRigRefitGate 2), so its
    // RT group records are tagged and it never self-shadows in RT (clean_rt_common.glsl
    // rtSelfCandidate). Only ever set on a non-zero word.
    static constexpr uint32_t kAvVisRtNoSelf = 1u << 28;
    // The CPU-decided pass bits (the RT candidate test adds kAvVisRtProbe).
    static constexpr uint32_t kAvVisPasses = kAvVisAny | kAvVisMain | kAvVisSun | kAvVisProj | kAvVisMirror;
private:
    // Then per slot the wearer's padded world bounds {min xyz, max xyz} (floats; min.x >
    // max.x = unknown): the dynamic probe faces cull avatars by them (clean_cull.comp
    // avBoxOutside, clean_probe_rigcull.comp).
    static constexpr uint32_t kAvBoxBase = kAvDemBase + 2 * kAvVisSlots;
    // #561: then per slot the wearer's screen-LOD scale (float, metres per pixel at its
    // distance, with hysteresis; 0 = none) -- clean_cull.comp AV_LOD_BASE, read by the
    // rigged screen-size LOD and replayed by the RT rigged plan.
    static constexpr uint32_t kAvLodBase = kAvBoxBase + 6 * kAvVisSlots;
    // HIDE SELECTED (#29): after the per-slot LOD scales, [kAvHideBase] = n, then n sorted
    // instance ids (<= kAvHideCap) every cull pass drops (clean_cull.comp avHidden).
    static constexpr uint32_t kAvHideBase = kAvDemBase + 2 * kAvVisSlots + 6 * kAvVisSlots + kAvVisSlots;
    static constexpr uint32_t kAvHideCap = 2048;
    std::vector<uint32_t> mHideIds;   // this frame's (setSelection), sorted, capped
    uint32_t  mAvVis[kAvVisSlots + 4 + kCasterWords + 2 * kAvVisSlots + 6 * kAvVisSlots + kAvVisSlots
                     + 1 + kAvHideCap] = {};
    float     mRigLodMpp[kAvVisSlots] = {};   // #561: the hysteresis state of those words
    LLUUID    mRigLodAv[kAvVisSlots];         // ... and whose they are (a reused slot starts fresh)
    // Per joint-table slot: 1 = the avatar is hidden by render policy (muted, Do Not
    // Render, over complexity, friends-only; avatarHiddenByPolicy). Set by
    // computeAvatarVis; the rigged demand scan skips their instances (no skin memory
    // reserved for avatars nobody draws).
    uint8_t   mAvPolicyHidden[kAvVisSlots] = {};
    // Per joint-table slot: 1 = the avatar is an animesh (control avatar). Set by
    // computeAvatarVis; only the "RT avatars:" log line reads it (animesh breakout).
    uint8_t   mAvAnimesh[kAvVisSlots] = {};
    // Store generations seen by the last fill: unchanged -> skip the whole-array
    // group/material/UV copies and compares.
    uint32_t  mLastMatGen = 0xFFFFFFFFu, mLastGrpGen = 0xFFFFFFFFu;
    bool      mMatRawChanged = false, mGrpRawChanged = false;
    float     mShadowCasterPlanes[4][6][4] = {};
    void      fillShadowCasterTail();
    void computeAvatarVis(const VKCullPush& push, const VKDrawPush& dp);
    void writeCullAvatarDescriptors();
    void recordTonemap(VkCommandBuffer cmd, VkImageView colorView, uint32_t width, uint32_t height);
    // LOGOUT SCREENSHOT (LL saveFinalSnapshot -> screen_last). The world frame (post
    // tonemap/glow, pre selection/HUD/UI) is copied into a host-visible buffer every
    // ~2 s; readSnapshotRGB converts the newest capture to bottom-up RGB (LLImageRaw).
    GpuBuffer mSnapBuf{};
    uint32_t  mSnapW = 0, mSnapH = 0;
    bool      mSnapValid = false;
    double    mSnapNext = 0.0;
    void recordSnapshotCopy(VkCommandBuffer cmd, uint32_t width, uint32_t height);
    // Copy the swap image (COLOR_ATTACHMENT_OPTIMAL, restored after) into a host-
    // visible buffer, (re)allocating it when too small. Shared by the logout capture
    // and user snapshots.
    bool recordSwapCopy(VkCommandBuffer cmd, GpuBuffer& buf, uint64_t& cap, uint32_t width, uint32_t height);
    void swapBytesToRGB(const uint8_t* src, uint32_t w, uint32_t h, std::vector<uint8_t>& rgb) const;
    uint64_t  mSnapCap = 0;
    // USER SNAPSHOTS (LLViewerWindow::rawSnapshot under VK): a one-shot copy of the
    // swap image at the requested point of the next recorded frame --
    //   1 world only (post tonemap/glow, before selection outline + HUD),
    //   2 world + HUD (after the world pass),
    //   3 the full presented frame incl. UI (after the UI replay).
    GpuBuffer mCapBuf{};
    uint64_t  mCapCap = 0;
    uint32_t  mCapW = 0, mCapH = 0, mCapPending = 0;
    bool      mCapValid = false;
public:
    bool readSnapshotRGB(std::vector<uint8_t>& rgb, uint32_t& w, uint32_t& h);
    // One RGBA pixel of the newest world-frame capture (the ~2 s logout-screenshot
    // copy: world only, no HUD/UI), for DebugShowColor. (x, y) are GL-convention
    // window pixels (origin bottom-left). Does NOT wait for the GPU: the buffer is
    // host-coherent, so this reads the last completed copy (a debug readout may see
    // the copy in flight). False when nothing has been captured yet.
    bool readWorldPixel(int32_t x, int32_t y, uint8_t rgba[4]) const;
    void requestCapture(uint32_t point) { mCapPending = point; mCapValid = false; }
    uint32_t capturePending() const { return mCapPending; }   // 0 = none
    // LIVE SNAPSHOTS (#873; llcleansnapshotvk.h): the shot object (created on first use), its
    // capture-point hook (recordWorld: 1; the context: 2 / 3 -- the swap image in
    // COLOR_ATTACHMENT_OPTIMAL) and the post-submit fence hand-over.
    LLCleanSnapshotVK* liveSnap();
    void recordLiveSnap(VkCommandBuffer cmd, uint32_t point, uint32_t width, uint32_t height);
    void liveSnapSubmitted(VkFence fence);
    // The next live snapshot point-capture is 2 (world + HUD): the pre-HUD text layer waits
    // for the UI replay (as capturePending() == 2 does for the old capture).
    bool liveSnapPoint2Pending() const;
    // #932 SNAPSHOT SAMPLE INDEX: 1..N on an accumulation sample of a shot -- a live
    // snapshot's jittered frame (its Halton index) or a supersampled offscreen recording
    // (mOffscreenFrame) -- else 0. Derived from the shot's state (valid anywhere in the
    // recording, also before mLiveCapNow / mSnapAccNow are decided). Every noise seed that
    // would otherwise repeat across a shot's samples adds it, so N averaged samples see N
    // distinct patterns. Read-only: no live counter moves, the live view's sequences and
    // histories after the shot are untouched.
    uint32_t snapSample() const;
    // The post chain's noise frame id (clean_final.frag / clean_tonemap.frag dither and film
    // grain, mod 2^20). LLFrameTimer's frame count advances only in LLAppViewer::idle(), and
    // a shot renders its samples in a display() loop with no idle in between (#932: the
    // dither was IDENTICAL in every sample -- the regular grain), so the sample index is
    // added.
    uint32_t postNoiseFrame() const;
    // Records the capture copy if one is pending for this point (swap image must be
    // in COLOR_ATTACHMENT_OPTIMAL). Called by recordWorld (1) and the context (2, 3).
    void recordCaptureAt(VkCommandBuffer cmd, uint32_t point, uint32_t width, uint32_t height);
    // Waits for the GPU (one-off, snapshot time) and returns the capture as bottom-up
    // RGB (LLImageRaw convention). False if no capture was recorded.
    bool readCaptureRGB(std::vector<uint8_t>& rgb, uint32_t& w, uint32_t& h);
    // OFFSCREEN renders (LLVulkanContext::renderOffscreen): suppresses the periodic
    // logout-screenshot copy so a snapshot-sized frame never lands in it.
    bool mOffscreen = false;
    // 0-based index of the current offscreen recording within its sequence (set by
    // LLVulkanContext::renderOffscreen before each recordWorld). Recordings after the
    // first re-render the first one's scene: the rig / GPU animation / flexi step and the
    // particle spawn run once per snapshot (#417), so nothing moves between them.
    uint32_t mOffscreenFrame = 0;
    // SNAPSHOT SUPERSAMPLING (#417). LLVulkanContext::renderOffscreen brackets its
    // recordings with these. beginOffscreen saves the live view's temporal state (camera
    // history, TAA / FSR / RT-mask / exposure / flare flags) and parks the SSR previous
    // frame (the snapshot gets its own at its size); with accumFrames > 0 it allocates
    // the snapshot's accumulation targets (w x h, scene + glow, ~16 B / px) and returns
    // accumFrames, or 0 when they don't fit (the old unjittered path then). Recording 0
    // is then a warm-up (SSR's previous frame); 1..N are jittered (Halton(2,3), N
    // phases) and averaged with equal weights (clean_snap_accum.comp) where the TAA
    // resolve would run. endOffscreen frees the snapshot-only targets and restores
    // everything saved, so the live view continues its histories (no reset, no shimmer).
    uint32_t beginOffscreen(uint32_t w, uint32_t h, uint32_t accumFrames);
    // The next on-screen recordWorld rewrites every set keyed on mTargetGen (the
    // offscreen sizes re-created the shared screen targets, and a recycled view handle
    // can make a handle compare say "unchanged"). Histories: see the post-offscreen
    // block in recordWorld -- TAA, FSR, SSR's previous frame, the temporal camera, the
    // exposure and the flare state are restored; the god rays' history was never
    // touched; VRS and two-phase occlusion restart (re-created / view-dependent); the RT
    // shadow mask continues when the snapshot traced inline (supersampled snapshots),
    // restarts when the snapshot ran its own mask at another size. (Exposure: offscreen
    // recordings don't run the adaptation; they tonemap with the main view's value.)
    void endOffscreen();
    // The render-target generation (see mTargetGen): external consumers key their
    // descriptor sets on it.
    uint64_t targetGen() const { return mTargetGen; }
    bool mPostOffscreen = false;
    // World depth readback (depth snapshots): copies the D32 world depth (attachment
    // layout, i.e. after recordWorld with no HUD pass) into a host buffer.
    void recordDepthCapture(VkCommandBuffer cmd, uint32_t width, uint32_t height);
    // Waits for the GPU; rows top-down, raw [0,1] VK depth. One-shot.
    bool readDepthCapture(std::vector<float>& depth, uint32_t& w, uint32_t& h);
private:
    GpuBuffer mDepthCapBuf{};
    uint64_t  mDepthCapCap = 0;
    uint32_t  mDepthCapW = 0, mDepthCapH = 0;
    bool      mDepthCapValid = false;
    uint32_t  mGlowW = 0, mGlowH = 0;
    GpuImage  mGlowE{}, mGlowP0{}, mGlowP1{};
    VkSampler mGlowSampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout mDslPost = VK_NULL_HANDLE;   // b0 sampler, b1 storage image
    VkDescPool            mPostPool;   // layout-sized + accounted (vk_descpool.h)
    VkDescriptorSet mDsetGlowExtract = VK_NULL_HANDLE;   // target -> E
    VkDescriptorSet mDsetGlowBlurA   = VK_NULL_HANDLE;   // E  -> P0
    VkDescriptorSet mDsetGlowBlurB   = VK_NULL_HANDLE;   // P0 -> P1
    VkDescriptorSet mDsetGlowBlurC   = VK_NULL_HANDLE;   // P1 -> P0
    VkDescriptorSet mDsetGlowCombine = VK_NULL_HANDLE;   // P1 (sampled)
    // <SS:Skoomastream> HUD glow (SSHudGlow): the HUD glow pipelines' 2nd attachment
    // (display size, colour x amount, created on the first glowing HUD) and its extract
    // set (mHudGlow -> E; also bound as the extract's set 1, which mode 2 never reads).
    static constexpr VkFormat kHudGlowFormat = VK_FORMAT_R8G8B8A8_UNORM;
    GpuImage        mHudGlow{};
    uint32_t        mHudGlowW = 0, mHudGlowH = 0;
    VkDescriptorSet mDsetHudGlowExtract = VK_NULL_HANDLE;
    // </SS:Skoomastream>
    // <SS:Skoomastream> Glow extract set 1 (mDslPost layout, b0 only): the final LDR image
    // the extract multiplies the glow amount by (LL glowExtract). One per possible tone
    // target [0] mAAColor, [1] mFinalColor, [2] mSphereTmp; written lazily by recordGlow,
    // the cached view cleared when the ensure* that owns the image recreates it.
    VkDescriptorSet mDsetGlowScene[3] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkImageView     mGlowSceneView[3] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkPipelineLayout mGlowLayout = VK_NULL_HANDLE;
    VkPipeline       mGlowPipe = VK_NULL_HANDLE;
    VkPipelineLayout mGlowCombineLayout = VK_NULL_HANDLE;
    VkPipeline       mGlowCombinePipe = VK_NULL_HANDLE;

    // Ambient occlusion target: mAoA = final AO (sampled by the lit FS), mAoB = blur
    // intermediate. Set A: src B -> dst A (SSAO, blur V); set B: src A -> dst B (blur H).
    // mAoW / mAoH: the RENDER size the AO resources were made for (mAoNormal's size; the AO
    // kernels' pc.screen). A / B are that size, or ceil(/ 2) of it on the half grid (mAoHalf).
    uint32_t  mAoW = 0, mAoH = 0;
    GpuImage  mAoA{}, mAoB{};
    // HALF-RESOLUTION AO (RenderVKAOHalfRes / RenderVKAOUpsample; latched by ensureAO for the
    // frame): A / B on the half grid; mAoMeta the depth-aware upsample's per-texel surface
    // (rg32f plane offset + oct Ng, clean_ao.comp aoMeta; a 1 x 1 stub unless mAoUpMode == 2:
    // draw set 0 binding 21 [1] and AO set binding 5 always name it). mAoUpMode = frame info
    // ao_up.x: 0 full res, 1 half + bilinear, 2 half + depth-aware.
    bool      mAoHalf = false;
    uint8_t   mAoUpMode = 0;
    GpuImage  mAoMeta{};
    uint32_t  mAoMetaW = 0, mAoMetaH = 0;
    // Pre-pass normals, view space, octahedral: .xy the shading (normal-mapped) normal,
    // .zw the smooth unperturbed vertex normal (#648: the AO depth-MIP path's normal).
    GpuImage  mAoNormal{};
    static constexpr VkFormat kAoNormalFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    VkFormat  mAoFormat = VK_FORMAT_UNDEFINED;
    bool      mAoLinear = false;     // format supports linear filtering (blur taps)
    uint8_t   mAoMethod = 0;         // latched at aoStage 0: 0 SSAO, 1 GTAO, 2 RTAO (#86)
    // #843: the AO's own noise frame counter (GTAO kern_y.z: XeGTAO's temporal index), advanced
    // once per live (non-offscreen) AO frame whatever the AA mode -- mTaaFrame only moves while
    // the temporal plumbing is on (#841's flaw).
    uint32_t  mAoFrame = 0;
    // #843 AO TEMPORAL HISTORY (r32ui, clean_ao.comp aoPackHist): [0] this frame's (written by
    // stage 3), [1] last frame's (stage 3 reads it at the reprojected position; the copy
    // [0] -> [1] follows stage 3). The AO targets' size while aoWantsTemporal, 1 x 1 stubs
    // otherwise (AO set b6 / b7 always name valid images). GENERAL for good.
    GpuImage  mAoHist[2]{};
    uint32_t  mAoHistW = 0, mAoHistH = 0;
    bool      mAoTemporalNow = false;   // this recording runs stage 3 (aoTemporalBegin)
    bool      mAoHistValid = false;     // [1] holds a usable history (reset on re-create / cut / skip)
    uint64_t  mAoHistLb = 0;            // mLbFrame when [1] was last written (a skipped frame resets)
    uint8_t   mAoHistMethod = 0;        // the AO method that wrote it
    uint32_t  mAoHistRetryFrame = 0;    // gFrameCount before which a refused history is not retried
    VkImageView mAoDepthView = VK_NULL_HANDLE;   // depth view the AO sets point at
    VkDescriptorSetLayout mDslAO = VK_NULL_HANDLE;   // b0 depth, b1 src AO, b2 dst AO
    VkDescPool            mAoPool;
    VkDescriptorSet  mDsetAoA = VK_NULL_HANDLE, mDsetAoB = VK_NULL_HANDLE;
    VkPipelineLayout mAoLayout = VK_NULL_HANDLE;
    VkPipeline       mAoPipe = VK_NULL_HANDLE;
    bool             mAoThisFrame = false;   // AO computed for this frame's main view
    // DEPTH-ONLY DEFERRED PRE-PASS (RenderVKPrepassDepthOnlyDeferred): deferred frames pre-pass
    // depth only (+ motion) and AO runs AFTER the G-buffer pass, reading G1 (world oct N / Ng)
    // instead of mAoNormal: mAoPipeG = clean_ao.comp with kAoGbufNormals (spec constant 0)
    // through the G sets (binding 3 = G1; rewritten when the G1 view changes: mAoGbufView).
    // Both set pairs bind the frame-info block at binding 9 (ssr.cur_view).
    VkPipeline       mAoPipeG = VK_NULL_HANDLE;
    VkDescriptorSet  mDsetAoGA = VK_NULL_HANDLE, mDsetAoGB = VK_NULL_HANDLE;
    // #86 RTAO (clean_rtao.comp, the AO format's variant): sets {AO set, draw set 1, RT consumer
    // set, draw set 0}; mRtaoPipeG = the G1-normal twin (kAoGbufNormals), as mAoPipeG.
    VkPipelineLayout mRtaoLayout = VK_NULL_HANDLE;
    VkPipeline       mRtaoPipe = VK_NULL_HANDLE;
    VkPipeline       mRtaoPipeG = VK_NULL_HANDLE;
    uint8_t          mRtaoLogged = 0xFF;   // last logged RTAO state (0 fallback, 1 tracing)
    float            mRtaoTime = 0.f;      // this recording's time (s): the alpha test's texture animation
    VkImageView      mAoGbufView = VK_NULL_HANDLE;   // G1 view the G sets point at
    VkImageView      mAoGbufDepthView = VK_NULL_HANDLE;   // depth view the G sets point at
    uint64_t         mAoGbufGen = 0;   // mTargetGen when written (#snapcrash: handles get reused)
    bool             mAoFromGbuf = false;    // this frame's AO stages read G1 (mAoPipeG + G sets)
    bool             mAoNormalWritten = false;   // this frame's pass 1a wrote mAoNormal (FX normals)
    // (Re)write the G sets for the current G-buffer / depth / AO images; false = unavailable.
    bool ensureAoGbufSets();
    void writeAoSet(VkDescriptorSet set, VkImageView src, VkImageView dst, VkImageView normal);
    // #648 view-depth MIP chain (R32F linear view depth, kAoZMips levels, level 0 = AO res).
    static constexpr uint32_t kAoZMips = 5;
    GpuImage         mAoZ{};
    VkImageView      mAoZMipView[kAoZMips] = {};
    uint32_t         mAoZW = 0, mAoZH = 0, mAoZLevels = 0;
    VkDescriptorSetLayout mDslAoZ = VK_NULL_HANDLE;   // b0 depth, b1 storage[kAoZMips]
    VkDescPool       mAoZPool;
    VkDescriptorSet  mDsetAoZ = VK_NULL_HANDLE;
    VkPipelineLayout mAoZLayout = VK_NULL_HANDLE;
    VkPipeline       mAoZPipe = VK_NULL_HANDLE;
    VkPipeline       mAoZSpdPipe = VK_NULL_HANDLE;   // #828 64 x 64-tile build (RenderVKAODepthSPD)
    VkImageView      mAoZDepthView = VK_NULL_HANDLE;   // depth view mDsetAoZ points at
    uint64_t         mAoZGen = 0;
    VkImageView      mAoViewZBound = VK_NULL_HANDLE;   // what AO set binding 4 points at
    bool             mAoMipsNow = false;   // latched by recordAoDepthMips for this frame's stages

    // Occlusion culling (#167). Hi-Z = R32F max-depth pyramid (level 0 = half the depth
    // buffer). Scratch buffers mirror the cull's stable compaction for the 4 static
    // buckets (0, 1, 6, 7). Everything but the pipelines is created on first use and
    // freed when the setting goes off.
    GpuImage         mHiz{};
    uint32_t         mHizW = 0, mHizH = 0, mHizMips = 0;
    static constexpr uint32_t kHizMaxMips = 16;
    VkImageView      mHizMipView[kHizMaxMips] = {};
    bool             mHizFresh = true;        // next use transitions from UNDEFINED
    VkDescriptorSetLayout mDslHiz = VK_NULL_HANDLE, mDslOccl = VK_NULL_HANDLE;
    VkPipelineLayout mHizLayout = VK_NULL_HANDLE, mOcclLayout = VK_NULL_HANDLE;
    VkPipeline       mHizPipe = VK_NULL_HANDLE, mOcclPipe = VK_NULL_HANDLE;
    VkDescPool       mOcclPool;
    VkDescriptorSet  mDsetHiz[kHizMaxMips] = {};
    VkDescriptorSet  mDsetOccl = VK_NULL_HANDLE;
    GpuBuffer        mOcclCand{}, mOcclMeta{}, mOcclWg{}, mOcclStats{};
    uint32_t         mOcclCap = 0;            // bucket capacity the scratch is sized for
    uint32_t         mOcclFrame = 0;
    bool             mOcclStatsValid[3] = {};
    // Handles the descriptor sets were written with (rewritten when any changes).
    VkBuffer         mOcclBound[8] = {};
    uint64_t         mOcclBoundSerial[8] = {};   // GpuBuffer::serial: handles get reused
    VkImageView      mOcclDepthView = VK_NULL_HANDLE;
    VkImageView      mHizDepthView = VK_NULL_HANDLE;   // the Hi-Z level-0 sets' depth view
    uint64_t         mHizGen = 0;
    VkImageView      mOcclHizView = VK_NULL_HANDLE;    // mDsetOccl's Hi-Z view

    // Two-phase occlusion (RenderVKOcclusion2Phase). P list: 4 rows (buckets 0 1 6 7) x
    // mBucketCap draws, holding P1 (visible last frame) and then P2 (revealed) -- P2 is
    // written after P1 has been drawn. Candidates are indexed by instance id; meta / wg
    // counts per row entry (6 rows: + the static alpha pre / post-water lists). Visibility
    // = the stamp of the last frame the instance survived the test (no per-frame clear).
    VkDescriptorSetLayout mDslOccl2 = VK_NULL_HANDLE;
    VkPipelineLayout mOccl2Layout = VK_NULL_HANDLE;
    VkPipeline       mOccl2Pipe = VK_NULL_HANDLE;
    VkDescPool       mOccl2Pool;
    VkDescriptorSet  mDsetOccl2 = VK_NULL_HANDLE;
    GpuBuffer        mOccl2Cand{}, mOccl2Meta{}, mOccl2Wg{}, mOccl2List{}, mOccl2Count{}, mOccl2Vis{}, mOccl2Stats{};
    uint32_t         mOccl2Cap = 0;           // bucket capacity the per-row scratch is sized for
    uint32_t         mOccl2InstCap = 0;       // instance capacity of cand / vis
    VkBuffer         mOccl2Bound[10] = {};
    uint64_t         mOccl2BoundSerial[10] = {};
    VkImageView      mOccl2HizView = VK_NULL_HANDLE;
    uint32_t         mOccl2Stamp = 2;         // this frame's stamp (vis == stamp - 1: visible last frame)
    bool             mOccl2Valid = false;     // last frame ran the test (or a cut-frame seed) with this camera
    bool             mOccl2Cut = false;       // camera cut / teleport (setTemporal): skip the two phases, reseed
    bool             mOccl2VisFresh = true;   // vis buffer contents undefined: zero-fill first
    uint32_t         mOccl2Frame = 0;
    uint32_t         mOccl2Slot = 0;          // this frame's stats ring slot
    bool             mOccl2StatsValid[3] = {};

    // Local lights (clustered forward). Byte layout = clean_lights_common.glsl.
    static constexpr uint32_t MAX_VK_LIGHTS = 256;
    static constexpr uint32_t CLUSTER_GX = 16, CLUSTER_GY = 9, CLUSTER_GZ = 24, CLUSTER_CAP = 64;
    struct VKLightGPU
    {
        float pos_radius[4], color_falloff[4], proj_params[4], proj_origin[4], proj_axis[4];
        float proj_mat[16];
        uint32_t tex[4];
    };
    struct VKLightInfo
    {
        float cam_pos[4], cam_fwd[4], zinfo[4], grid[4];
        float view[16];
        float cull_planes[6][4];   // the main cull frustum (clean_probe_gpu.comp off-screen probes, #517)
        // Froxel grid (RenderVKLightCulling 2; clean_lights_common.glsl FROXEL LIGHT MASKS),
        // filled per extent in recordWorld (froxelSetup): the main view's jittered P * rotation-
        // only V (= the deferred lighting's inv_vp_rel source), its draw origin, the slices
        // (near, (n - 1) / ln(far / near), far), the dims (tiles x / y, slices, log2 tile px),
        // the render extent (w, h, 1 / w, 1 / h).
        float fx_vp[16], fx_eye[4], fx_slices[4];
        uint32_t fx_dims[4];
        float fx_extent[4];
        // Forward lit FS (clean_lights_common.glsl LightInfo.fwd): [0] / [1] the froxel mask
        // buffer's device address (lo, hi; 0 = the forward walk reads the cluster lists), [2]
        // options (bit 0: alpha-blend receivers trace RT projector shadows inline), [3] 0.
        uint32_t fwd[4];
    };
    static_assert(sizeof(VKLightInfo) == 368, "VKLightInfo = clean_lights_common.glsl LightInfo (std430 = std140: draw set 0 binding 6 is a UBO, #814)");
    std::vector<VKLightGPU> mLightsCPU;
    VKLightInfo mLightInfoCPU{};
    float mViewProjRel[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};   // P * rotation-only view (rigged)

    // Screen-space reflections (LL's approach: trace against the PREVIOUS frame's lit
    // scene + depth, reprojected through its view). Byte layout = clean_draw.frag SSRInfo.
    struct VKSSRInfo
    {
        float prev_view[16], prev_proj[16], prev_inv_proj[16];
        float params0[4];   // iterations, ray step, distance bias, depth reject bias
        float params1[4];   // adaptive step multiplier, SSR enabled, debug view, AO enabled
        float atmos[64];    // atmosphere + post block (clean_atmos_common.glsl)
        float water[24];    // water surface block (clean_frame_info.glsl)
        float shadow_mat[4][16];   // sun/moon cascades (clean_frame_info.glsl)
        float shadow_clip[4], shadow_params[4], shadow_texel[4];
        float mirror_box[16], mirror_plane[4], mirror_params[4];   // planar mirror
        float ao_params[4];   // irradiance scale, irradiance max, effect diag, nondiag
        float cur_view[16];   // current world -> view (normal pre-pass)
        float ao_params2[4];  // x specular occlusion on, y standard AO multiply, z highlight
                              // transparent, w opaque water (RenderTransparentWater off)
        float sky_fx[4];      // aurora intensity, aurora time, meteor count, rad/pixel
        float sky_fx2[4];     // star density
        float meteors[192][4];   // 64 meteors x {head+width, tail, colour}
        float clouds[4][4];      // cloud layer (clean_clouds.glsl)
        float bodies[10][4];     // sun/moon billboards, stars, rainbow/halo (clean_sky_bodies.glsl)
        // Temporal (#84, clean_frame_info.glsl): unjittered current / previous world ->
        // clip, this frame's jitter (px xy, NDC zw), params (prev time, MV valid, reset,
        // jitter on).
        float taa_cur_vp[16], taa_prev_vp[16];
        float taa_jitter[4], taa_params[4];
        // Render scale (#181): x material mip bias while jittered, y render / display;
        // z RT sun alpha-test distance, w RT soft sun terminator floor (disc radii).
        float taa_scale[4];
        // Ray-traced sun / moon shadows (#185): disc radius (rad), rays, extra origin offset (m), length (m).
        float sun_rt[4];
        // x shadow-map filter (SlipstreamRenderShadowFilterQuality 0-3), y terrain hex
        // tiling (SlipstreamRenderTerrainHexTiling), z translucent shadows (#212, cascades +
    // ray traced), w the projector atlas translucency layer.
        float render_opts[4];
        // RT shadow mask (#195): x on (pass 1 reads it), y mask px per render px, zw size.
        // Written by a separate update after selectLitVariant (needs the TLAS state).
        float rt_mask[4];
        // Lit-pass cost switches (RenderVKLitDebugMask): x = bit mask (clean_draw_common.glsl LDBG_*).
        float lit_dbg[4];   // x RenderVKLitDebugMask, y RenderVKProbeTopN, z RenderVKLightCutoff, w RT blend-caster options (rtBlendOpts)
        // Smooth terrain (#211, clean_terrain_lod.glsl): xyz this recording's cull LOD
        // camera (every view's terrain geomorph follows the main camera), w 0 = off, else
        // 1 + flags (+1 RT mask terrain re-anchor, +2 its debug view; clean_frame_info.glsl).
        float terrain_lod[4];
        // CAMERA-RELATIVE ORIGIN (#555, clean_frame_info.glsl cam_origin): xyz the main
        // eye of this recording; shadow_mat[] and taa_cur_vp / taa_prev_vp map
        // (world - cam_origin.xyz). w: temporal-exclusion bits (#605; 1 media, 2 animated
        // textures; 0 when not jittered).
        float cam_origin[4];
        // ALPHA-BLEND DEPTH TOLERANCE (#731, clean_frame_info.glsl blend_tol): base (m),
        // distance scale (m per m), grazing multiplier cap, debug tint on.
        float blend_tol[4];
        // RT FOLIAGE TRANSMISSION (#735, clean_frame_info.glsl foliage_rt): mode | 4 facing
        // flip | 8 rigged / worn facing parity | 16 RT back-face self-shadow
        // (RenderVKRTBackfaceSelfShadow), strength, tint amount, layer cap.
        float foliage_rt[4];
        // CLUSTER LOD SHADOW RECEIVER LIFT (#747 interim, clean_frame_info.glsl cluster_rt):
        // metres of lift per metre of camera distance (main tolerance / px_scale x
        // RenderVKClusterShadowLiftScale), the near clip (m), 1 = an avatar is switched.
        float cluster_rt[4];
        // AO RESOLUTION (clean_frame_info.glsl ao_up): 0 full res, 1 half res + bilinear,
        // 2 half res + depth-aware upsample; the upsample's absolute plane tolerance (m).
        float ao_up[4];
        // PER-FRAME HAZE CONSTANTS (#816, clean_frame_info.glsl atmos_k; clean_atmos_common.glsl
        // header has the formulas): fillAtmosK, from atmos[] once per frame.
        float atmos_k[6][4];
        // VOLUMETRIC FOG (#718, clean_frame_info.glsl vol_*; LLCleanVolFogVK::FrameInfo, same
        // order): grid matrices, eye + on flag, grid, slices, fog, albedo, misc. Zero = off.
        float vol_vp[16], vol_inv_vp[16];
        float vol_eye[4], vol_grid[4], vol_slice[4], vol_fog[4], vol_albedo[4], vol_misc[4];
        // VOLUMETRIC CLOUDS (#719, clean_frame_info.glsl vol_clouds; LLCleanVolCloudsVK::fillFrameInfo):
        // [0] mode (0 = classic 2D layer, 2 = volumetric layer live), base / top altitude (m),
        //     cloud shadow strength (0 = no cloud shadow map)
        // [1] shadow map: centre xy (agent world, on the base plane), 1 / extent (per m), panorama valid
        // [2] light direction the shadow map was built for (xyz), unused
        // [3] reserved
        float vol_clouds[4][4];
        // WEATHER (#783, clean_frame_info.glsl weather; LLCleanWeatherVK::fillFrameInfo, the
        // contract in clean_weather_surface.glsl): [0] wetness, rain, time, ripple strength;
        // [1] coarse occlusion map corner xy, 1 / extent, which; [2] its top, span, cover bias,
        // texel; [3] / [4] the same for the near cascade; [5] snow cover, jitter phase, PCF
        // radius (m), PCF radius (texels); M2 [6] - [12]: the maps' shears, lightning, cloud /
        // sky, persistent snow cover; M4 [13] - [14]: the run-off flow map's frame and strength
        // (the full contract: clean_weather_surface.glsl); #954 [15] the fog-off precipitation veil,
        // [16] the precipitation medium in the volumetric fog (clean_weather_veil.glsl). Zeros = no
        // weather (the lit FS's branch never runs).
        float weather[17][4];
        // SUN / MOON SHADOW-MAP RECEIVER FIT (clean_frame_info.glsl shadow_fit): x on
        // (RenderVKShadowReceiverFit), y plane slope tolerance (RenderVKShadowPlaneTolerance),
        // z PCSS filter radius cap in metres (RenderVKShadowPCSSMaxRadius, 0 = none), w unused.
        float shadow_fit[4];
        // <GMDX> PHYSICAL SKY parameters (clean_frame_info.glsl physky; clean_physky.glsl has the
        // layout): physSkyFill, from atmos[] and the camera, once per frame.
        float physky[8][4];
        // <GMDX> clean_frame_info.glsl gmdx_misc: the alpha-mask pull (x base m, y m per m, z graze cap).
        float gmdx_misc[4];
    };
    // #814: clean_frame_info.glsl declares this block std140 (a UNIFORM BUFFER, draw set 0 /
    // AO set binding 9). Every member is a vec4 / mat4 / array of them, so the std140 offsets
    // equal the std430 ones this struct was laid out for (verified on the SPIR-V: every
    // OpMemberDecorate Offset identical before / after); these pin the ends + a few anchors.
    static_assert(sizeof(VKSSRInfo) == 5552, "VKSSRInfo = clean_frame_info.glsl SSRBuf (std140, 5552 B)");   // <GMDX> + physky[8], gmdx_misc after shadow_fit
    static_assert(sizeof(VKSSRInfo) % 16 == 0, "std140 block size");
    static_assert(offsetof(VKSSRInfo, atmos) == 224 && offsetof(VKSSRInfo, shadow_mat) == 576
                  && offsetof(VKSSRInfo, meteors) == 1104 && offsetof(VKSSRInfo, ao_up) == 4720
                  && offsetof(VKSSRInfo, atmos_k) == 4736 && offsetof(VKSSRInfo, vol_vp) == 4832
                  && offsetof(VKSSRInfo, vol_misc) == 5040 && offsetof(VKSSRInfo, vol_clouds) == 5056
                  && offsetof(VKSSRInfo, weather) == 5120 && offsetof(VKSSRInfo, shadow_fit) == 5392
                  && offsetof(VKSSRInfo, physky) == 5408 && offsetof(VKSSRInfo, gmdx_misc) == 5536,
                  "VKSSRInfo member offsets = clean_frame_info.glsl SSRBuf (std140)");
    // Lightbox sky effects (upstream LLDrawPoolWLSky aurora/meteors, RenderStarCount).
    void fillSkyFx(VKSSRInfo& si, uint32_t height);
    // #816: si.atmos_k from si.atmos (the per-pixel atmosScatter's per-frame terms).
    static void fillAtmosK(VKSSRInfo& si);
    float mAtmos[64] = {};
    // Incremental geometry placement (see fillFromCleanStores): slot-row runs to stream
    // this frame, garbage accounting, and the deferred full-repack / full-stream flags.
    std::vector<std::pair<uint32_t, uint32_t>> mSlotRuns;
    // #584 NO CPU IMAGE of the packed VBO / IBO / weight arena. The allocators below keep
    // offsets only; the bytes live in the GPU buffers and in the mesh store's per-slot
    // mirror (LLCleanMeshAsset::cpuSlotPacked / cpuSlotIndices / cpuSlotWeights), which
    // recordGeomUploads copies straight into the upload staging at record time.
    // High-water marks ("end") of each arena, in elements (QVertex incl. the 2-vertex slot
    // headers / U16 index / 10 B weight record) -- what the CPU vectors' sizes used to be.
    uint32_t mGeomVertEnd = 0, mGeomIdxEnd = 0, mGeomWgtEnd = 0;
    // Slots placed by the fill whose bytes still have to be streamed from the mirror
    // (recordGeomUploads; deduplicated there, bounded by the slot-row count). The
    // destination is always the slot's CURRENT SlotInfo row, so an entry that survives a
    // skipped record (acquire failed) or a re-placement stays correct.
    std::vector<uint32_t> mGeomUpSlots;
    void queueGeomUpload(uint32_t sl);
    // A geometry buffer was created empty (first build): stream every packed row.
    bool mGeomStreamAll = false;
    // The proto scene's packed prefix (KB; its slots SHARE ranges, so it streams as one
    // blob): ranges below mGeomOwned*From. Dropped by the first full repack.
    std::vector<uint8_t>  mProtoVerts;   // QVertex bytes, header-prefixed per proto slot
    std::vector<uint16_t> mProtoIdx;
    // recordWorld: stream this frame's placed slots (or every packed row) from the mirror.
    // A row whose mirror no longer matches it (rewritten / freed after the fill, e.g. by
    // the #561 decimator pump) is dropped for a frame: its next placement re-adds it.
    void recordGeomUploads(VkCommandBuffer cmd, VkPipelineStageFlags2 vStage, VkAccessFlags2 vAccess,
                           VkPipelineStageFlags2 iStage, VkAccessFlags2 iAccess);
    void dropGeomSlot(uint32_t sl);
    std::vector<uint32_t> mGeomUpScratch;
    // #584 GPU COMPACTION. A full repack (fill) lays the live slots out back to back; a
    // slot whose packed bytes are current is MOVED by a GPU copy (old buffer range -> its
    // compacted range in a freshly allocated buffer, buildBuffers -> compactGeometryBuffers,
    // copied by flushGrowCopies; the old buffer is retired after that frame), the rest
    // re-stream from the mirror. Element units (QVertex incl. header / U16 / 10 B record).
    struct GeomMove { uint32_t sl, v_from, v_to, v_n, i_from, i_to, i_n, w_from, w_to, w_n; };
    std::vector<GeomMove> mGeomMoves;     // consumed (cleared) by the next buildBuffers
    bool mGeomCompactPending = false;     // a repack ran: buildBuffers reallocates + moves
    // buildBuffers, after its device wait. False = the compacted buffers could not be
    // allocated: the moves were turned into mirror uploads into the old buffers instead.
    bool compactGeometryBuffers(VkDeviceSize vbo_need, VkDeviceSize ibo_need, VkDeviceSize wgt_need);
    std::vector<GeomPiece> mGeomPieces[3];   // (scratch: vertices / indices / weights)
public:
    // #584: a packed slot's SOURCE bytes for CPU readers (RT, opacity micromaps): its
    // header-prefixed vertex range (header at verts, vertex v at verts + (2 + v) * 20 B),
    // its face-local U16 indices and its 10 B weight records (null for static slots) --
    // the mesh-store mirror (or the proto prefix), and only while it still matches the
    // slot's SlotInfo row (false otherwise: the slot is re-placed next frame). Render thread.
    bool geomSlotSource(uint32_t sl, const uint8_t*& verts, const uint16_t*& idx, const uint8_t*& wgts) const;
private:
    // LOD-group rows copied from the store's dirty list this frame (streamed as runs
    // instead of the whole table). mGrpFromStore: mCpuGroups rows mirror the store's
    // (false after the proto scene wrote its own rows) -- the incremental copy needs it.
    std::vector<std::pair<uint32_t, uint32_t>> mGrpRuns;
    bool mGrpFromStore = false;
    // FREE LISTS (element units): a slot's superseded / freed range is reused by later
    // placements (best fit, neighbours coalesce) instead of piling up as garbage --
    // a GC pass freeing ~60 MB used to force a full repack (~120 ms) plus a rebuild of
    // every BLAS (~110 ms): a 250 ms freeze. Compaction is now the rare fallback.
    struct GeomFreeList
    {
        std::map<uint32_t, uint32_t> byOff;            // offset -> length
        std::multimap<uint32_t, uint32_t> bySize;      // length -> offset
        uint64_t total = 0;
        void clear() { byOff.clear(); bySize.clear(); total = 0; }
        void eraseSize(uint32_t len, uint32_t off)
        {
            auto r = bySize.equal_range(len);
            for (auto it = r.first; it != r.second; ++it) if (it->second == off) { bySize.erase(it); return; }
        }
        uint32_t rejected = 0;   // overlapping releases refused (would double-allocate)
        bool release(uint32_t off, uint32_t len)
        {
            if (!len) return true;
            auto next = byOff.lower_bound(off);
            // Never accept a range that overlaps free space: handing it out twice
            // corrupts whoever got it first.
            if ((next != byOff.end() && next->first < off + len)
                || (next != byOff.begin() && std::prev(next)->first + std::prev(next)->second > off))
            {
                ++rejected;
                return false;
            }
            total += len;
            if (next != byOff.begin())
            {
                auto prev = std::prev(next);
                if (prev->first + prev->second == off)
                {
                    eraseSize(prev->second, prev->first);
                    off = prev->first; len += prev->second;
                    byOff.erase(prev);
                }
            }
            if (next != byOff.end() && off + len == next->first)
            {
                eraseSize(next->second, next->first);
                len += next->second;
                byOff.erase(next);
            }
            byOff[off] = len;
            bySize.emplace(len, off);
            return true;
        }
        // #595: give the free tail back to the arena (`end` drops to the last live range).
        void trimTail(uint32_t& end)
        {
            while (!byOff.empty())
            {
                auto it = std::prev(byOff.end());
                if ((uint64_t)it->first + it->second != end) break;
                end = it->first;
                total -= it->second;
                eraseSize(it->second, it->first);
                byOff.erase(it);
            }
        }
        bool take(uint32_t len, uint32_t& off)
        {
            auto it = bySize.lower_bound(len);
            if (it == bySize.end()) return false;
            const uint32_t blen = it->first, boff = it->second;
            bySize.erase(it);
            byOff.erase(boff);
            total -= len;
            off = boff;
            if (blen > len) { byOff[boff + len] = blen - len; bySize.emplace(blen - len, boff + len); }
            return true;
        }
    };
    GeomFreeList mFreeVerts, mFreeIdx, mFreeWgts;
    // #595 GEOMETRY ADMISSION. A refused VBO / IBO / weight grow keeps the old buffers; the
    // slots past them are un-placed (geomOverflow: index_count 0 = not drawn, pop-in) and
    // re-placed from the mirror once a grow may be tried again (backoff mGeomGrowRetryAt).
    // While it runs, the fill's placement refuses ranges past the live buffers.
    double mGeomGrowRetryAt = 0.0;     // LLFrameTimer s: no geometry grow attempt before this
    uint32_t mGeomGrowStreak = 0;      // consecutive refused geometry grows (backoff exponent)
    std::vector<uint32_t> mGeomDeferred;   // un-placed slots waiting for room
    uint64_t mGeomDeferredTotal = 0;   // stats: slots deferred since start
    size_t   mGeomDeferredSnap = 0;     // (main-thread copies for the VKMem line)
    uint64_t mGeomDeferredTotalSnap = 0;
    void geomOverflow(VkDeviceSize vbo_need, VkDeviceSize ibo_need, VkDeviceSize wgt_need);
    // Cull-list backoff (#595): a refused list grow keeps the old lists (the cull clamps to
    // mBucketCap) until this LLFrameTimer time.
    double mListGrowRetryAt = 0.0;
    bool makeCullLists(uint32_t n, bool soft);
    // #697 INSTANCE-ID CAPACITY UNDER PRESSURE. The instance SSBO and the cull scratch are
    // indexed by instance id; a refused grow keeps the old buffers (soft at every step,
    // hard only when there is no buffer at all) and backs off until mInstGrowRetryAt
    // (LLFrameTimer s). clampInstanceIds() then bounds mInstanceCount -- the cull's
    // dispatch, RT's instance walk, the HUD id list -- to the ids the buffers hold:
    // instances past it are not drawn until the grow goes through (objects pop in later,
    // nothing reads past a buffer).
    double mInstGrowRetryAt = 0.0;
    uint32_t mInstGrowStreak = 0;
    uint32_t mInstClampDropped = 0;   // ids past the capacity at the last clamp (stats)
    void clampInstanceIds();
    // Ranges below these marks are NOT owned by one slot (proto scene / pre-repack
    // layout): never released into the free lists until a full repack resets them.
    uint32_t mGeomOwnedVertFrom = 0, mGeomOwnedIdxFrom = 0, mGeomOwnedWgtFrom = 0;
    size_t mGeomGarbageVerts = 0, mGeomGarbageIdx = 0, mGeomGarbageWgts = 0;
    uint32_t mGeomInPlace = 0;   // same-shape slot rewrites this frame (diagnostic)
    bool   mGeomForceRepack = false, mGeomFullStream = false, mGroupsDirty = false;
    // ---- CLUSTER-ROUTED GEOMETRY (2026-10-14: with cluster LOD on the cluster path REPLACES the
    // legacy one) ----
    // In the cluster-owns-rigged mode (clusterOwnsRigged, latched per fill in mGeomRoutedMode) a
    // LOD group referenced ONLY by cluster-routable rigged rows
    // (LLCleanClusterSelectVK::rowClusterRoutable) gets no legacy GPU geometry: its slots are
    // never placed in the packed VBO / IBO / weight arena (the mesh store's CPU mirror stays: the
    // cluster builder reads it), and placed ones are dropped the moment the group becomes
    // routed-only. A group any legacy row references (alpha-blend, flexi, HUD, system body,
    // statics) is placed as before; an unreferenced one too (the old rule). A mode flip repacks
    // (on: the routed slots fall out of the pack and the buffers shrink to 1.5x the need; off:
    // every referenced slot streams back from the mirror -- a one-time hitch). Fill thread only.
    // CLUSTER-OWNED MIRROR RELEASE (2026-10-16): once such a group's DAG is durable on disk
    // (stream takeMirrorCandidates), clusterMirrorTick frees the store's CPU mirror of its
    // slots too (LLCleanMeshAsset::queueClusterMirrorRelease). A slot the pack then needs again
    // (mode flip off, a legacy row on the group) has no mirror: placeSlot leaves it unplaced and
    // records it in mRematAsk; clusterMirrorTick asks the store to re-materialize it (async
    // re-decode / LL re-fetch; the slot is not drawn on legacy until it is back).
    std::atomic<bool> mClOwnsRigged{false};          // written by clusterTick (render thread)
    uint32_t mForwardStreak = 0;                     // consecutive on-screen frames lit forward
    static constexpr uint32_t kForwardStreakLegacy = 120;   // ... after which rigged go back to legacy
    bool   mGeomRoutedMode = false;                  // the mode the pack was laid out with
    std::vector<uint32_t> mGrpLegacyRefs, mGrpRoutedRefs;   // per LOD group: rows of each kind
    std::vector<uint32_t> mRowGrp;                   // per instance row: its LOD group (refs bookkeeping)
    std::vector<uint8_t>  mRowKind;                  // 0 none, 1 legacy, 2 cluster-routed
    std::vector<uint32_t> mSlotGrp;                  // slot -> LOD group (LOD_SENTINEL unknown)
    std::vector<uint32_t> mGrpReleased;              // groups that became routed-only (scratch)
    std::vector<uint32_t> mRematAsk;                 // fill: unplaced slots whose mirror was freed (bounded)
    std::vector<LLClusterKey> mMirrorCand;           // clusterMirrorTick scratch
    std::vector<uint32_t> mMirrorPubs;               // clusterMirrorTick scratch
    bool mMirrorOwnLast = false;                     // clusterMirrorTick: the mode last seen
    // Render thread, every clusterTick (also with LOD off): the releases / re-materializations
    // above. Only between the fill join and the next fill start (the store is not being read).
    void clusterMirrorTick(bool own);
    uint32_t mParkScanTick = 0;
    std::atomic<uint64_t> mGeomRiggedPackedBytes{0}; // packed rigged slots (verts + idx + weights), ~5 s old
    std::atomic<uint64_t> mGeomRoutedSlots{0}, mGeomRoutedBytes{0}, mGeomRoutedGroups{0};   // (logMemory)
    std::atomic<uint64_t> mGeomRoutedVerts{0};       // routed-only meshes' finest-LOD verts (LOD-off estimate)
    std::atomic<uint64_t> mGeomRoutedDropped{0};     // placed slots released for routing (total)
    float mWater[24] = {};
    LLUUID mWaterNormalId[2];
    int32_t mWaterNormalIdx[2] = {-1, -1};
    // Cloud layer block (clean_clouds.glsl) + its two noise textures (bindless indices).
    float   mClouds[16] = {};
    LLUUID  mCloudNoiseId[2];
    int32_t mCloudNoiseIdx[2] = {-1, -1};
    // Sky bodies block (clean_sky_bodies.glsl) + its six textures: sun A, sun B, moon,
    // bloom (stars), rainbow, halo (bindless indices, UNORM like LL's raw samples).
    float   mBodies[40] = {};
    LLUUID  mSkyTexId[6];
    int32_t mSkyTexIdx[6] = {-1, -1, -1, -1, -1, -1};
    // Refraction source for water: the solid-scene HDR colour, copied after pass 1.
    GpuImage mRefract{};
    uint32_t mRefractW = 0, mRefractH = 0;
    // Water exclusion mask (#27, LL's water exclusion target): R8 at render resolution,
    // cleared to 1, invisiprims write 0; the water FS discards where < 1. B3: with no
    // invisiprim in the store the scope is skipped while the mask is known all-1.
    GpuImage mWaterExcl{};
    bool     mWaterExclAllOne = false;   // last written: cleared, no exclusion draw (reset on recreate)
    // REACTIVE + WATER MASK (F5 / F2): render pass 2's third colour attachment (RGBA8,
    // render resolution, MAX blend). R = post-water alpha + particle coverage (the TAA
    // resolve's reactive mask, RenderVKReactiveMask), G = the water surface (the TAA
    // water mask, #204; written by the water FS itself), B = the coverage of blended
    // no-history faces (live media / animated textures, #605; clean_draw.frag), A unused.
    // 2a's writes are discarded (DONT_CARE), 2b clears it first. Sampled by
    // clean_taa.comp (binding 10) and the FSR inputs.
    static constexpr VkFormat kReactFormat = VK_FORMAT_R8G8B8A8_UNORM;
    GpuImage mReact{};
    bool     mReactMaskNow = false;   // this frame's TAA reads mReact.R (no reactive copy, F5)
    void recordWaterExclusion(VkCommandBuffer cmd, uint32_t width, uint32_t height,
                              const VkViewport& vp, const VkRect2D& sc, const VKDrawPush& push);
    void recordRefractCopy(VkCommandBuffer cmd, uint32_t width, uint32_t height);
    VkImage   mSwapImage = VK_NULL_HANDLE;
    GpuImage  mPrevColor{}, mPrevDepth{};
    bool      mPrevColorFresh = false;   // the last SSR copy took the colour (B2: skipped without SSR)
    uint32_t  mPrevW = 0, mPrevH = 0;
    bool      mPrevValid = false;
    GpuBuffer mSSRInfoSSBO{};
    float     mCurView[16]{}, mCurProj[16]{}, mCurInvProj[16]{};
    float     mPrevView[16]{}, mPrevProj[16]{}, mPrevInvProj[16]{};
    void ensureSSR(uint32_t width, uint32_t height);

    // ---- TEMPORAL, stage 1 (#84): motion vectors + projection jitter ----
    // Active (mTemporalOn) while RenderVKTAA or RenderVKDebugMotionVectors is on; off,
    // nothing below is allocated or maintained and rendering is unchanged.
    // PREVIOUS INSTANCES: mPrevInstSSBO holds every Instance row as of the previous
    // frame (draw set 0 binding 23, same 224 B stride as the instance SSBO so each
    // changed run is one contiguous GPU copy). Before this frame's instance stream the
    // rows about to change -- this frame's runs AND last frame's (an instance that
    // moved last frame and stopped this one must catch up) -- are copied instance ->
    // prev; after it, ids that just came alive get their own row (prev = current). A
    // reset copies every row after the stream. Off, binding 23 aliases the instance
    // SSBO (never read: only the motion pre-pass consumes it).
    GpuBuffer mPrevInstSSBO{};
    uint32_t  mPrevInstCap = 0;                                  // rows
    // #1033 SLIM VERTEX-SHADER INSTANCE RECORDS (clean_vs_inst.glsl, 112 B per row): the world
    // vertex shaders read these instead of the 224 B Instance (draw set 0 binding 54 = current,
    // 55 = previous-frame, motion pre-pass only). Derived EVERY frame by one compute dispatch
    // (clean_vs_inst.comp, recordVsInst) from the instance SSBO / previous-instance SSBO after
    // the stream + previous-row copies, so every Instance writer (stream, grow copy, agent
    // shift, previous-row copy) stays authoritative and nothing else writes these. Sized to the
    // instance capacity (the high-water pattern of the SSBO itself); the previous one exists
    // only while mPrevInstSSBO does (binding 55 aliases the current one otherwise).
    GpuBuffer mVsInstSSBO{};
    GpuBuffer mVsInstPrevSSBO{};
    uint32_t  mVsInstCap = 0;                                    // rows of mVsInstSSBO
    uint32_t  mVsInstPrevCap = 0;                                // rows of mVsInstPrevSSBO
    VkPipelineLayout mVsInstLayout = VK_NULL_HANDLE;
    VkPipeline       mVsInstPipe = VK_NULL_HANDLE;
    void ensureVsInst();                          // (re)size to the instance capacity (device wait on change)
    void writeVsInstDescriptor();                 // draw set 0 bindings 54 / 55
    void recordVsInst(VkCommandBuffer cmd);       // after the stream + previous-row copies
    std::vector<std::pair<uint32_t, uint32_t>> mPrevInstRuns;   // last frame's upload runs
    std::vector<std::pair<uint32_t, uint32_t>> mInstCopyRuns;   // scratch (merged runs)
    std::vector<uint8_t> mInstLive;       // per id: live in the mirror at the last upload
    // MOTION VECTORS: RG16F, UV units, current - previous (clean_prepass_normal.frag),
    // written by the depth pre-pass as the 2nd attachment of pass 1a (the normal target
    // is the 1st), then SHADER_READ_ONLY for the rest of the frame (tonemap debug b7,
    // later the TAA resolve). Cleared to 0 when the pre-pass is off.
    static constexpr VkFormat kMotionFormat = VK_FORMAT_R16G16_SFLOAT;
    GpuImage  mMotion{};
    uint32_t  mMotionW = 0, mMotionH = 0;
    // NO-HISTORY MASK (#605): R8, the motion target's twin (same size, lifetime and
    // layouts; the pre-pass's 3rd attachment, plain write): 1 where the nearest opaque /
    // mask surface is a live media (or, by setting, animated-texture) face. The TAA
    // resolve (binding 15) and the FSR inputs (binding 12) keep no history there.
    // Cleared to 0 when the pre-pass is off. (Blended faces: mReact.B.)
    static constexpr VkFormat kNoTemporalFormat = VK_FORMAT_R8_UNORM;
    GpuImage  mNoTemporal{};
    // Depth + normal + motion pre-pass twins (MRT: normal, motion, no-history mask).
    // Index [0] back-culled, [1] cull-none, like the other pre-pass pipes.
    VkPipeline mPreMvPipe[2]        = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkPipeline mPreMvMaskPipe[2]    = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkPipeline mPreMvRigPipe[2]     = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkPipeline mPreMvRigMaskPipe[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    // Depth + MOTION pre-pass, NO normals (RenderVKPrepassDepthOnlyDeferred: deferred temporal
    // frames -- AO reads the G-buffer's normals): clean_prepass_motion.frag, MRT {motion,
    // no-history mask}. [0] back-culled, [1] cull-none.
    VkPipeline mPreMoPipe[2]        = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkPipeline mPreMoMaskPipe[2]    = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkPipeline mPreMoRigPipe[2]     = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkPipeline mPreMoRigMaskPipe[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    // RT TERMINATOR TRIANGLE PASS (#664, clean_term_tri.frag): EQUAL after the pre-pass,
    // [0] static (mVbo) / [1] rigged (mSkinVbo) x [cull back, cull none]. Writes mTermTri:
    // per pixel the Hanika lift of the exact rasterized triangle (facet normal, lift
    // height, smooth normal), read by the RT shadow mask and the inline lit FS through
    // the RT consumer set (binding 10). R32G32_UINT, 8 B / render px.
    VkPipeline mTermTriPipe[2][2] = {{VK_NULL_HANDLE, VK_NULL_HANDLE}, {VK_NULL_HANDLE, VK_NULL_HANDLE}};
    static constexpr VkFormat kTermTriFormat = VK_FORMAT_R32G32_UINT;
    GpuImage   mTermTri{};
    uint32_t   mTermTriW = 0, mTermTriH = 0;
    bool       mTermTriUse = false;          // this frame: valid size, bound, written by pass 1a
    double     mTermTriWantedAt = -1e9;      // wall s last needed (freed 10 s after, like the mask)
    bool      mTemporalOn = false;        // this frame: motion vectors + prev maintenance
    bool      mTemporalReset = true;      // this frame: prev = current everywhere, history invalid
    bool      mTaaJitterOn = false;       // this frame: main view jittered (RenderVKTAA)
    bool      mTaaPrevValid = false;      // mTaaPrevVP / mTaaPrevTime hold a real previous frame
    uint32_t  mTaaFrame = 0;              // Halton phase counter (on-screen frames)
    uint32_t  mTaaShiftGen = 0;           // gVKOriginShiftGen at the last frame
    float     mTaaPrevVP[16] = {};        // previous frame's unjittered world -> clip
    float     mTaaPrevVPRel[16] = {};     // ... as P * rotation-only V: maps (P - mTaaPrevEye) (#555)
    float     mTaaPrevEye[3] = {};
    float     mTaaPrevTime = 0.f;
    float     mTaaPrevBoost = 0.f;        // speed-FoV boost (rad) of the frame mTaaPrevVP* is from (#663)
    float     mTaaJitter[4] = {};         // this frame: px x, px y (y down), NDC x, NDC y
    float     mTaaCam[3] = {}, mTaaFwd[3] = {};   // last frame's camera (cut test)
    bool      mTaaCamValid = false;
    // ---- TEMPORAL, stage 2 (#84): rigged + worn-attachment motion vectors ----
    // DESIGN: keep last frame's JOINT TABLE and re-derive everything else from it, so
    // nothing depends on the per-frame rig allocations (the cull hands each visible
    // rigged item a fresh mSkinVbo range every frame).
    //  - mPrevJoints: the joint table (jointTable()) as of the previous frame, same
    //    stride / slots. Before this frame's joint stream + GPU animation the live slots
    //    are copied table -> prev; after them, slots with no history (avatar new or back
    //    in view this frame, a reset, a fresh buffer) get this frame's rows (prev ==
    //    current: camera-only motion). Draw set 0 binding 25 (worn attachments:
    //    prev joint[prev row's slot / bone] * prev row's offset).
    //  - mPrevPalette (RenderVKPreskinPrevHistory off; see F6 below): last frame's
    //    palettes, rebuilt on motion-vector frames by a 2nd
    //    clean_skin_palette.comp dispatch (mDsetSkinPalPrev: mPrevJoints + the static
    //    meta) relative to THIS frame's eye, so previous and current skinned positions
    //    share one eye-relative origin and a static avatar gets exactly zero motion.
    //  - mPrevSkinPos: clean_preskin.comp re-skins each vertex with mPrevPalette and
    //    writes its previous eye-relative position at the SAME index as its mSkinVbo
    //    vertex (packed xyz, 12 B per skinned vertex); the rigged VS reads it at gl_VertexIndex
    //    (draw set 0 binding 24). No range remap is ever needed.
    // Off, bindings 24 / 25 and pre-skin 5 / 6 alias live buffers and nothing is kept.
    GpuBuffer mPrevJoints{}, mPrevPalette{}, mPrevSkinPos{};
    // F6 (RenderVKPreskinPrevHistory): the previous position comes from LAST frame's
    // pre-skin output instead of a 2nd palette dispatch + a 2nd skin per vertex.
    //  - mSkinHistPos: eye-relative skinned positions, 2 slots x mSkinHistCap vertices
    //    (xyz floats), slot = history frame & 1;
    //  - mSkinHistTab: per instance, per slot {dst base, src base, skin id, frame} -- where
    //    each instance's range was (the cull hands out a fresh one every frame).
    // clean_preskin.comp u_prev 3. Misses (new / back in view / LOD or skin change) give
    // previous = current. mPrevPalette is then not rebuilt.
    // Both exist only while the setting is on (ensureTemporal allocates / frees them live).
    GpuBuffer mSkinHistPos{}, mSkinHistTab{};
    uint32_t  mSkinHistCap = 0;                 // skinned vertices per slot
    uint32_t  mSkinHistInstCap = 0;             // table rows (instances)
    uint32_t  mSkinHistFrame = 1;               // last history frame dispatched (first used: 2)
    bool      mSkinHistClear = true;            // zero the table before its next use (new buffer)
    bool      mSkinHistGap = true;              // last frame wrote no usable history (reset, skip)
    uint32_t  mSkinHistTaa = 0;                 // mTaaFrame of the last history dispatch
    float     mSkinHistEye[3] = {0.f, 0.f, 0.f};   // mPalEye of that dispatch
    VkBuffer  mPrevJointsSrc = VK_NULL_HANDLE;   // the joint table mPrevJoints mirrors
    uint32_t  mPrevPalCap = 0;                  // skins
    uint32_t  mPrevSkinCap = 0;                 // skinned vertices
    bool      mPrevJointsValid = false;          // mPrevJoints holds a real previous frame
    std::vector<uint8_t> mCpuAvatarAlive;        // per joint slot: avatar alive this frame
    std::vector<uint8_t> mPrevAvatarAlive;       // ... as of the last history update
    VkDescriptorSet mDsetSkinPalPrev = VK_NULL_HANDLE;   // palette compute: prev joints -> prev palette
    // (De)allocate the prev-instance store + motion target (+ the stage-2 rigged history)
    // per the settings (device wait + descriptor rewrite only when something changes);
    // sets mTemporalOn.
    void ensureTemporal(uint32_t width, uint32_t height);
    // Pre-stream copy (runs about to change) or post-stream copy (reset / new ids).
    void recordPrevInstances(VkCommandBuffer cmd, bool post);
    // Joint-table history: pre (before the joint stream / GPU animation: live slots ->
    // prev) or post (after them: slots with no history -> prev).
    void recordPrevJoints(VkCommandBuffer cmd, bool post);
    void writePrevInstDescriptor();   // draw set 0 bindings 23..25 (prev instances, skinned, joints)
    void writeMotionDescriptor();     // grade set binding 7 (tonemap debug view)
    void writeRtMaskMotionDescriptor();   // RT mask set 3 binding 15 (motion vectors)

    // ---- TEMPORAL, stage 3 (#84): TAA resolve (clean_taa.comp) ----
    // Wanted when RenderFSAAType == 3 ("TAA" in Preferences) or RenderVKTAA (debug);
    // it replaces FXAA / SMAA. Runs on the on-screen main view only, after pass 2 and
    // before the SSR copy, so SSR's previous frame, DoF, bloom, exposure and tonemap
    // all consume the resolved image. HISTORY: two RGBA16F images at render size
    // (GENERAL for good, like the AO targets), ping-ponged: the resolve reads
    // mTaaHist[mTaaCur ^ 1] and writes mTaaHist[mTaaCur], which is then copied over
    // mHdr. Freed while TAA is off.
    bool taaRequested() const;
    // RenderVKSunShadows 2 (#185): ray-traced sun / moon REQUESTED (setting, RT up and
    // within budget). Keeps the RT build running (rtConsumerActive) -- it does NOT mean
    // this frame traces: see mSunRtNow. (Automatic downgrades write the setting itself.)
    bool sunRtWanted() const;
    // THE per-frame decision (#268), made once in selectLitVariant after the RT build:
    // the sun is traced this frame (requested, RT warm, TLAS valid, ray-query variant).
    // Everything that picks "traced vs cascades" -- recordShadows, the particle casters,
    // shadow_params.w in the frame info, god rays -- reads this one bool. (They used to
    // re-evaluate sunRtWanted() at different times; setShadows ran before the RT build,
    // so the frame where RT went over budget published w = 2 with an evicted TLAS.)
    bool        mSunRtNow = false;
    int         mSunRtLogged = -1;          // last logged mode (-1 none, 0 maps, 1 traced)
    int         mSunRtPrev = -1;            // last frame's mode (switch counting)
    double      mSunRtLogNext = 0.0;        // rate limit (wall s)
    uint32_t    mSunRtFlips = 0;            // switches since the last log line
    const char* sunRtOffReason() const;     // why this frame is on the cascades (logging)
    GpuImage  mTaaHist[2]{};
    GpuImage  mTaaAcc[2]{};               // accumulation weight (R16F, else R32F: mTaaAccFormat; #182), paired with mTaaHist
    VkFormat  mTaaAccFormat = VK_FORMAT_R32_SFLOAT;
    // GLOW RESOLVE: the per-face glow target is rasterized with the jitter too. The same
    // dispatch resolves it into this display-sized pair (RGBA16F, GENERAL, ping-ponged
    // with mTaaHist); bloom's extract and the LDR glow read mTaaGlow[mTaaGlowSlot] when
    // mTaaGlowNow, the raw target otherwise (TAA off, offscreen / snapshots, RenderVKTAAGlow 0).
    GpuImage  mTaaGlow[2]{};
    bool      mTaaGlowValid = false;      // mTaaGlow[mTaaCur ^ 1] holds last frame's resolved glow
    bool      mTaaGlowNow = false;        // this frame's glow consumers read mTaaGlow[mTaaGlowSlot]
    uint32_t  mTaaGlowSlot = 0;
    VkDescriptorSet mDsetGlowExtractTaa[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};    // mTaaGlow[i] -> E
    VkDescriptorSet mDsetBloomExtractTaa[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};   // mHdr + mTaaGlow[i]
    bool taaGlowWanted() const;           // a glow consumer runs and RenderVKTAAGlow is on
    uint32_t  mTaaW = 0, mTaaH = 0;
    uint32_t  mTaaCur = 0;                // history written this frame
    bool      mTaaHistValid = false;      // mTaaHist[mTaaCur ^ 1] holds last frame's resolve
    bool      mTaaNow = false;            // this frame resolves (set by ensureTAA)
    // Single sharpening (B1 / F4a, RenderVKSharpenOnce): RCAS ran on this frame's
    // resolve (recordTAA), so the final pass skips its CAS; the tonemap is the last
    // full-screen pass (no FXAA / SMAA, no LDR glow), so it takes the final pass's dither.
    bool      mRcasRan = false;
    bool      mTonemapLast = false;
    bool      mDitherInTonemap = false;
    bool      mTaaResolveReset = true;    // this frame's temporal reset (frame-info fill)
    float     mTaaReproj[16] = {};        // this frame's unjittered clip -> last frame's clip
    // The same with last frame's eye moved onto this frame's (rotation / zoom only): the
    // motion blur's camera split (#118; mTaaReproj - this = the translation part).
    float     mMbReprojRot[16] = {};
    // Motion blur's copy of mTaaReproj with the speed-FoV boost change removed (#663), and the
    // clip-xy scale that did it (the pre-pass motion target gets the same in the shader).
    float     mMbReprojCam[16] = {};
    float     mMbFovK = 1.f;
    float     mTaaWaterPlane[4] = {};     // water plane in this frame's unjittered clip space (#204)
    VkDescriptorSetLayout mDslTaa = VK_NULL_HANDLE;   // see clean_taa.comp
    VkDescPool            mTaaPool;
    VkDescriptorSet mDsetTaa[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};   // [i]: writes mTaaHist[i]
    VkPipelineLayout mTaaLayout = VK_NULL_HANDLE;
    VkPipeline       mTaaPipe = VK_NULL_HANDLE;
    // Views the sets point at (rewritten when any changes).
    VkImageView mTaaViews[9] = {};
    // (De)allocate the history per the settings / size; sets mTaaNow.
    void ensureTAA(VkCommandBuffer cmd, uint32_t width, uint32_t height);
    // One-shot (first on-screen frame after a snapshot, #417): ensureTAA's descriptor
    // rewrite keeps the history valid -- the history images were never touched, only
    // the inputs the sets name were re-created.
    bool      mTaaKeepHist = false;

    // ---- SNAPSHOT SUPERSAMPLING (#417; clean_snap_accum.comp) ----
    // See beginOffscreen. mSnapAcc (mHdrFormat, so it copies back over mHdr) and
    // mSnapAccGlow (kGlowChainFormat) are GENERAL for life and exist only during a
    // supersampled snapshot. The consumer sets read the averaged glow instead of the raw
    // target: mDsetSnapBloom (mDslBloom: mHdr + mSnapAccGlow) for the bloom extract,
    // mDsetSnapGlow (mDslPost: mSnapAccGlow -> mGlowE) for the LDR glow.
    uint32_t  mSnapAccFrames = 0;         // N of the current snapshot (0: not supersampling)
    bool      mSnapAccNow = false;        // this recording accumulates (offscreen frame 1..N)
    GpuImage  mSnapAcc{}, mSnapAccGlow{};
    uint32_t  mSnapAccW = 0, mSnapAccH = 0;
    VkDescriptorSetLayout mDslSnapAcc = VK_NULL_HANDLE;
    VkDescPool       mSnapAccPool;
    VkDescriptorSet  mDsetSnapAcc = VK_NULL_HANDLE;
    VkDescriptorSet  mDsetSnapBloom = VK_NULL_HANDLE;
    VkDescriptorSet  mDsetSnapGlow = VK_NULL_HANDLE;
    VkPipelineLayout mSnapAccLayout = VK_NULL_HANDLE;
    VkPipeline       mSnapAccPipe = VK_NULL_HANDLE;
    bool      mSnapAccInitFailed = false;
    bool initSnapAccum();                 // pipeline / layout / pool / sets (first supersampled snapshot)
    void destroySnapAccumTargets();       // the two accumulators (device idle)
    // Per accumulate recording, after ensureHdr / ensureGlow: the three sets name this
    // recording's mHdr / glow target (re-created on the first offscreen recording).
    bool writeSnapAccumSets();
    // At the resolve site: average this frame into the accumulators and copy the mean
    // over mHdr. In: scene COLOR_ATTACHMENT_OPTIMAL (pass 2), glow target attachment.
    // Out: mHdr COLOR_ATTACHMENT_OPTIMAL (recordSSRCopy's input layout), the glow target
    // SHADER_READ_ONLY (as on TAA frames), mSnapAccGlow visible to the glow consumers.
    void recordSnapAccum(VkCommandBuffer cmd, uint32_t width, uint32_t height);
    // The live view's state across an offscreen sequence (beginOffscreen / endOffscreen).
    struct OffscreenSave
    {
        bool      valid = false;
        GpuImage  prevColor{}, prevDepth{};   // SSR previous frame (parked; the TAA reads the depth)
        uint32_t  prevW = 0, prevH = 0;
        bool      prevValid = false, prevColorFresh = false;
        float     taaPrevVP[16] = {};
        float     taaPrevVPRel[16] = {};
        float     taaPrevBoost = 0.f;
        float     taaPrevEye[3] = {};
        float     taaPrevTime = 0.f;
        bool      taaPrevValid = false;
        float     taaCam[3] = {}, taaFwd[3] = {};
        bool      taaCamValid = false;
        uint32_t  taaShiftGen = 0, taaFrame = 0, taaCur = 0;
        bool      temporalReset = false;
        bool      taaHistValid = false, taaGlowValid = false, fsrRanLast = false;
        bool      rtMaskRecorded = false;
        bool      rtMaskKept = false;         // the snapshot did not run the RT mask (traced inline)
        bool      exposureReset = false;
        bool      flareStateInit = false;
    };
    OffscreenSave mOffSave;
    bool      mExposureRestore = false;   // post-offscreen frame: ensureHdr's snap request -> mOffSave's
    // ensureSSR's descriptor writes (draw set 0 bindings 7 / 8 / 9, the water-depth set).
    void writeSSRDescriptors();
    // ---- FSR 3.1 (RenderVKUpscaler 1-6; llcleanfsr3vk.h) ----
    // Rides on the TAA plumbing (mTaaNow: jitter, motion, reactive mask, the input
    // barriers) and replaces the resolve; the TAA dispatch still runs for its glow
    // resolve when a glow consumer wants it. Created on the first FSR frame, deleted
    // once FSR is off (VRAM discipline).
    LLCleanFSR3VK* mFsr = nullptr;
    bool      mFsrRanLast = false;        // the previous main-view frame upscaled with FSR
    uint32_t  mTaaJitterPhases = 16;      // this frame's Halton cycle length (FSR's lock logic)
    bool fsrRequested() const;            // an FSR mode is selected and the device can run it
    // Fills the FSR frame and makes its pipelines / resources / sets (before any barrier
    // of the resolve). false = FSR can't run this frame: the TAA resolve runs instead.
    bool prepareFSR(VkCommandBuffer cmd, uint32_t width, uint32_t height, bool mv_valid);
    // The upscale itself, after recordTaaInputBarriers (and the glow-only TAA dispatch);
    // leaves the layouts recordTAA leaves (see recordTAA).
    void recordFSR(VkCommandBuffer cmd, uint32_t width, uint32_t height);
    // ---- #870 / #962 DLSS: RAY RECONSTRUCTION + SUPER RESOLUTION (RenderVKUpscaler 7-11;
    // llcleandlssrrvk.h) ----
    // Takes FSR 3.1's slot (same inputs, same exit layouts) when the user-installed slip_nr
    // shim is available (LLVKNeuralRecon). RenderVKUpscaler 7-11 = DLSS at a quality level;
    // the network is RR or SR (RenderVKDlssRayReconstruction: 0 SR, 1 RR, 2 Auto = RR while a
    // noisy RT signal is on, dlssNoisyRtOn; a missing feature takes the other). Fixed modes: the
    // render extent is the network's optimal size for the mode (no dynamic resolution: autotune
    // / RLV / the VRAM ladder's render-scale rung put the frame on FSR at the equivalent preset
    // instead). Whatever it needs is made / freed in rrPrestep (syncFromStoresPrestep) -- an
    // RR <-> SR switch included; a frame whose state it was not built for falls back to FSR 3.1
    // (or TAA) and requests the change.
    LLCleanDlssRRVK* mRr = nullptr;
    struct RrReq
    {
        bool pending = false; bool release = false;
        LLVKNeuralRecon::FeatureKind kind = LLVKNeuralRecon::F_RR;
        uint32_t rw = 0, rh = 0, dw = 0, dh = 0, q = 0;
    };
    RrReq     mRrReq;
    bool      mDlssFrame = false;         // DLSS (RR or SR) is predicted for this main-view frame (rrPlan, recordWorld)
    LLVKNeuralRecon::FeatureKind mDlssKind = LLVKNeuralRecon::F_RR;   // ... and its network (valid with mDlssFrame)
    bool      dlssRrFrame() const { return mDlssFrame && mDlssKind == LLVKNeuralRecon::F_RR; }
    // RenderVKDlssFullResSignalsMode: under DLSS (either network) the RT signals (GI, the RT shadow mask,
    // the stochastic denoiser) run at the full render extent -- DLSS already renders at a fraction
    // of the display, and an upsampled signal hands it silhouette noise to spread. Else s.
    F32       rtSignalScale(F32 s) const;
    uint32_t  mRrQuality = 0;             // its mode (LLVKNeuralRecon::Quality)
    bool      mRrRanLast = false;         // the previous main-view frame ran DLSS (either network) ...
    LLVKNeuralRecon::FeatureKind mDlssRanKind = LLVKNeuralRecon::F_RR;   // ... with this network
    // #962 Auto (RenderVKDlssRayReconstruction 2): the adopted answer of dlssNoisyRtOn and the
    // time its raw answer started to differ (a 2 s dwell: no RR <-> SR flapping, each switch
    // re-creates the network and restarts its history).
    bool      mDlssAutoRr = false;
    bool      mDlssAutoInit = false;
    double    mDlssAutoSince = -1.0;
    uint32_t  mDlssLogKey[6] = {~0u, 0u, 0u, 0u, 0u, 0u};   // the last "DLSS: running" line's state
    std::string mDlssKindWhy;             // why this network (the log line)
    bool      mRrRaw = false;             // #870 raw routing: the lighting consumes the raw RT signals this frame
    uint32_t  mRrRawMask = 0;             // ... which ones (RenderVKDlssRawSignals bits; 0 when !mRrRaw)
    // RenderVKDlssRawSignals bits (#870): kRrRawMask the RT shadow mask's sun (ALSO read by the
    // forward-lit transparents, which the overlay hands over undenoised: off by default),
    // kRrRawProj the stochastic projectors, kRrRawRefl RT reflections, kRrRawGi GI, kRrRawMergedSun
    // the merged RT shadow pass's sun channel (deferred only), kRrRawAnyExtent: also when the
    // signal runs below the render extent (else such a signal stays denoised: its raw samples reach
    // the network as 2x2-block-correlated noise through the upsample, which RR cannot denoise).
    static constexpr uint32_t kRrRawMask = 1u, kRrRawProj = 2u, kRrRawRefl = 4u, kRrRawGi = 8u,
                              kRrRawMergedSun = 16u, kRrRawAnyExtent = 32u;
    // This signal goes raw this frame (its bit, and its extent w x h is the render extent or forced).
    bool rrRawSignal(uint32_t bit, uint32_t w, uint32_t h) const
    {
        return mRrRaw && (mRrRawMask & bit) != 0u
            && ((mRrRawMask & kRrRawAnyExtent) != 0u || (w >= mRenderW && h >= mRenderH));
    }
    double    mRrRetryAt = 0.0;           // a failed prestep / evaluate: next attempt
    std::string mRrWhy;                   // the last reason RR did not run (logged on change)
    uint64_t  mDsRtEpoch = 0;             // bumped when the RT reflection targets are (re)made / freed
    // RenderVKUpscaler 7-11 -> DLSS at that quality (true), else false.
    bool dlssModeSelected(uint32_t* quality = nullptr) const;
    // #962: a noisy RT signal RR denoises (RT reflections, GI, ReSTIR DI) is on (settings level).
    bool dlssNoisyRtOn() const;
    // #962: this frame's network from RenderVKDlssRayReconstruction, Auto's dwell and what the
    // shim offers (false: neither feature can run; why says so).
    bool dlssPickKind(LLVKNeuralRecon::FeatureKind& kind, std::string& why);
    // This main-view frame's plan: DLSS wanted + available (+ the network, the exact render extent).
    bool rrPlan(uint32_t dw, uint32_t dh, uint32_t& rw, uint32_t& rh);
    void rrNote(const std::string& why);  // logs a changed reason once
    LLCleanDlssRRVK::Want rrWant(LLVKNeuralRecon::FeatureKind kind, uint32_t rw, uint32_t rh, uint32_t dw,
                                 uint32_t dh, uint32_t q) const;
    // In the resolve block: records the reconstruction (true) or requests the state it needs
    // and returns false (the caller runs FSR / TAA).
    bool rrTry(VkCommandBuffer cmd, uint32_t width, uint32_t height, uint32_t rw, uint32_t rh, bool mvNow, bool deferNow);
    void rrPrestep();                     // syncFromStoresPrestep: apply mRrReq
    // TRANSPARENCY OVERLAY (RenderVKDlssTransparencyLayer + a shim that reads it): this RR frame
    // copies the opaque scene (mRefract) after the lighting and hands pass 2's transparents over
    // as the overlay. mRrOverlayNow: decided at the refraction copy (before pass 2).
    bool      mRrOverlayNow = false;
    bool rrOverlayWanted() const;
    // recordTAA's prologue: every resolve input's last write -> compute reads; the scene,
    // glow target and pass 2b mask -> SHADER_READ_ONLY (shared by the TAA and FSR paths).
    void recordTaaInputBarriers(VkCommandBuffer cmd);

    // ---- OPAQUE VARIABLE-RATE SHADING (VRS tier 2; RenderVKOpaqueShadingRate) ----
    // clean_vrs_rate.comp writes one R8_UINT rate texel per shadingRateTexel() tile after
    // the depth pre-pass (last frame's lit scene reprojected by this frame's motion,
    // luminance-gradient error vs a Weber threshold, motion relax, depth-edge guard,
    // hysteresis); pass 1b carries it as the fragment shading-rate attachment and only
    // the static OPAQUE buckets take its rate (mask buckets: a coarse fragment would
    // discard as a unit). The image stays GENERAL for life (its last texel feeds the
    // next frame's hysteresis).
    GpuImage         mVrsRate{};
    uint32_t         mVrsW = 0, mVrsH = 0;          // rate image size (texels)
    VkDescriptorSetLayout mDslVrs = VK_NULL_HANDLE;
    VkDescPool       mVrsPool;
    VkDescriptorSet  mDsetVrs[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};   // [0] motion, [1] no motion (dummy)
    VkPipelineLayout mVrsLayout = VK_NULL_HANDLE;
    VkPipeline       mVrsPipe = VK_NULL_HANDLE;
    VkImageView      mVrsViews[5] = {};             // views the sets point at (rewritten on change)
    bool             mVrsHistOk = false;            // mVrsRate holds a previous decision
    bool             mVrsLogged = false;
    // (Re)create the rate image / sets for the render size; false = VRS unavailable.
    bool ensureVrs(VkCommandBuffer cmd, uint32_t rw, uint32_t rh);
    // The rate compute (outside a rendering scope; depth in DEPTH_ATTACHMENT_OPTIMAL).
    void recordVrsRate(VkCommandBuffer cmd, uint32_t rw, uint32_t rh, bool motion, uint32_t forceMode);
    void destroyVrs();
    // glowOnly (FSR frames): the input barriers were recorded already; only the glow
    // resolve's output is used -- no copy / sharpening / layout epilogue.
    void recordTAA(VkCommandBuffer cmd, uint32_t width, uint32_t height, bool mv_valid, bool glowOnly = false);

    // ---- DEFERRED LIGHTING (RenderVKDeferredLighting; phase 5.2) ----
    // Pass 1b writes a G-buffer (clean_draw.frag -DCLEAN_GBUFFER, EQUAL after the depth
    // pre-pass) and clean_deferred_light.comp lights it into the scene HDR + glow target
    // with the forward FS's own lightSurface(). Layout (clean_gbuffer.glsl): G0 albedo
    // RGBA8 sRGB, G1 oct normals RGBA16F, G2 rough / metal / glow / model RGBA8, G3
    // emissive / legacy spec B10G11R11 -- 20 B / px. Forward stays for captures, the
    // mirror, debug views, Highlight Transparent, the VRS overlay, overdraw stats,
    // ray-query lit frames (no RT mask) and offscreen renders.
    static constexpr uint32_t kGbufCount = 4;
    static constexpr VkFormat kGbufFormats[kGbufCount] = {
        VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B10G11R11_UFLOAT_PACK32};
    GpuImage         mGbuf[kGbufCount]{};
    uint32_t         mGbufW = 0, mGbufH = 0;
    // G-buffer draw pipelines (plain layout; G-buffer draws never trace): [0] static,
    // [1] static double-sided (both with the dynamic VRS rate, like the EQUAL twins),
    // [2] rigged, [3] rigged double-sided.
    // Built with the SLIM G-buffer VS (clean_draw.vert / clean_draw_rigged.vert CLEAN_GBUFFER_VS:
    // no previous-frame pose, no pick outputs); mGbufFullVsPipe = the same with the full world
    // VS, for the RenderVKGbufSlimVS 0 A/B.
    VkPipeline       mGbufPipe[4] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkPipeline       mGbufFullVsPipe[4] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    // G-BUFFER TERRAIN SPLIT (RenderVKGbufTerrainSplit): the terrain-free static twins of [0] /
    // [1] (clean_draw.frag CLEAN_GBUFFER_NO_TERRAIN; the slim VS collapsing terrain draws,
    // clean_draw.vert CLEAN_VS_SKIP_TERRAIN) for buckets 0 1 6 7, and the terrain pipeline (CLEAN_GBUFFER_TERRAIN,
    // back-culled, [hex tiling off, on]: spec constant 2) for the main cull's terrain list
    // (mTerrList). A missing one -> the unsplit [0] / [1] that frame.
    VkPipeline       mGbufNtPipe[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkPipeline       mGbufTerrPipe[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    static constexpr uint32_t kGbufProbes = 5;   // RenderVKGbufRegProbe (measurement only, never drawn)
    VkPipeline       mGbufProbe[kGbufProbes] = {};
    // (#821) the slim G-buffer VS's register probes (clean_draw.vert CLEAN_VSPROBE_*): texture
    // animation, tangent, encloses-camera, rest pose, cel word, attachment / spin pose, all six --
    // with the G-buffer FS; RenderVKGbufRegProbe builds them, RenderVKGbufProbeDraw 11..17 draws one.
    static constexpr uint32_t kGbufVsProbes = 7;
    VkPipeline       mGbufVsProbe[kGbufVsProbes] = {};
    // ---- TERRAIN BAKE (#811, RenderVKTerrainBake; llcleanterrainbakevk.cpp) ----
    // Per region, the blended terrain surface (every material fetch + hex tiling of the live
    // terrain FS) baked once into region-local textures (clean_terrain_bake.comp); far terrain
    // samples them (clean_terrain_bake.glsl), near terrain keeps the live FS, crossfaded over a
    // band. The G-buffer draws the terrain list twice: mGbufTerrPipe (live FS, VS side 1: drops
    // the patches the bake covers) + mGbufTerrBakePipe (slim bake-only FS, VS side 2).
    //  - Table: draw set 0 binding 44 (mTerrBakeTable, kTerrBakeCap rows of TerrBakeRow by
    //    terrain record index), uploaded when it changes (terrBakeRecord).
    //  - Images: per region A (RGBA8 sRGB albedo + occlusion), B (PBR: RGBA8 normal / rough /
    //    metal), E (emissive, only when a material emits); full mip chains, registered in the
    //    bindless table (slot-owned, CLAMP_TO_EDGE sampler override) once complete.
    //  - Jobs: one at a time, a band of rows per frame (RenderVKTerrainBakeKTexels), mips by
    //    blits after the last band; aborted when the inputs change underneath.
    //  - Invalidation: CONTENT hash (heights + composition: LLCleanTerrainInputs; the terrain
    //    record; the materials' factors / flags / texture ids; hex tiling; region geometry) --
    //    a change hides the bake at once (live shading). RESIDENCY (the source textures'
    //    resident texels) growing, or the wanted resolution changing, re-bakes after a quiet
    //    period while the old bake stays shown (a bake is self-contained: textures mipping
    //    down later never invalidate it).
    //  - Budget: RenderVKTerrainBakeMB (LRU by distance: nearest regions first), halved under
    //    texture / VRAM ladder pressure and 0 at the ladder's geometry rung (bakes freed: the
    //    live FS draws everything, settings untouched); a region that leaves frees its bake.
    //  - Failures: a refused allocation retries 5 / 10 s later, the third drops the region for
    //    60 s (logged); never a hard allocation.
    static constexpr uint32_t kTerrBakeCap = 1024;   // table rows (terrain record indices)
    static constexpr uint32_t kTerrBakeSlots = 3;    // = kTexDescSets (declared below; asserted in the .cpp)
    struct TerrBakeRow { uint32_t tex[4]; float xf[4]; };   // = clean_terrain_bake.glsl TerrBake
    struct TerrBakeSet
    {
        GpuImage    img[3];                 // A, B, E (B / E empty when unused)
        VkImageView stor[3] = {};           // UNORM storage views of level 0 (the bake writes)
        uint32_t    slot[3] = {};           // bindless slots (0 = not registered)
        uint32_t    res = 0, levels = 0;
        bool        pbr = false, emis = false;
        uint64_t    bytes = 0;
        uint64_t    content = 0, qual = 0;  // the content hash / source texels it was baked from
        bool        live() const { return img[0].image != VK_NULL_HANDLE; }
    };
    struct TerrBakeRegion
    {
        uint64_t    handle = 0;
        uint32_t    rec = ~0u;              // terrain record index (table row)
        uint32_t    mat = 0;                // legacy: the detail-texture material
        bool        pbr = false, emis = false, hex = false;
        float       size = 0.f;             // metres per edge
        float       dist = 0.f;             // camera -> region box (m)
        uint32_t    seen = 0;               // tick that last saw the region
        uint64_t    content = 0;            // the current content hash
        uint64_t    qual = 0;               // the current source-texture texels (residency)
        double      content_t = 0.0, qual_t = 0.0;   // when each last changed
        uint32_t    want_res = 0;
        double      want_t = 0.0;           // when want_res last changed
        TerrBakeSet cur;                    // shown (its content == content)
        TerrBakeSet job;                    // being baked
        uint32_t    job_row = 0;            // next row band
        bool        job_started = false;    // grid copied + layouts set (recorded)
        bool        job_done = false;       // mips recorded: swapped in by the next tick
        double      job_t = 0.0;            // when the job started (wall s)
        GpuBuffer   job_grid{}, job_stage{};
        double      fail_until = 0.0;
        uint32_t    fails = 0;
        double      ready_t = -1.0e9;       // when cur landed (the sweep-in)
    };
    std::unordered_map<uint64_t, TerrBakeRegion> mTerrBakes;
    std::vector<TerrBakeRow> mTerrBakeRows, mTerrBakeRowsGpu;
    bool             mTerrBakeTableValid = false;   // the GPU table was initialised
    GpuBuffer        mTerrBakeTable{};
    VkPipeline       mGbufTerrBakePipe = VK_NULL_HANDLE;
    VkDescriptorSetLayout mDslTerrBake = VK_NULL_HANDLE;   // bake set 2: grid + 3 storage images
    VkDescPool       mTerrBakePool;
    VkDescriptorSet  mDsetTerrBake[kTerrBakeSlots] = {};     // per frame slot (written each baking frame)
    VkPipelineLayout mTerrBakeLayout = VK_NULL_HANDLE;     // draw sets 0 / 1 + set 2, 48 B push
    VkPipeline       mTerrBakePipe = VK_NULL_HANDLE;
    VkSampler        mTerrBakeSampler = VK_NULL_HANDLE;    // trilinear + aniso, clamp to edge
    float            mTerrBakeSamplerAniso = -1.f;
    VkQueryPool      mTerrBakeQuery = VK_NULL_HANDLE;      // 2 timestamps per frame slot
    bool             mTerrBakeTimed[kTerrBakeSlots] = {};
    uint32_t         mTerrBakeTick = 0;
    uint64_t         mTerrBakeLiveBytes = 0, mTerrBakePeakBytes = 0, mTerrBakeCapBytes = 0;
    uint32_t         mTerrBakeDone = 0, mTerrBakeAborted = 0, mTerrBakeRefused = 0, mTerrBakeEvicted = 0;
    double           mTerrBakeGpuMs = 0.0, mTerrBakeGpuMsMax = 0.0, mTerrBakeJobMs = 0.0;
    uint32_t         mTerrBakeGpuFrames = 0;
    double           mTerrBakeLogT = 0.0;
    // Per bindless slot: a sampler other than mSampler (the bakes' clamp sampler), applied by
    // writeTextureDescriptors; null = mSampler.
    std::vector<VkSampler> mTexSamplerOv;
    void     terrBakeInit();                         // init: layout, pool, sets, sampler, query pool
    void     terrBakeTick(uint32_t rh);              // recordWorld, before beginTextureFrame
    void     terrBakeRecord(VkCommandBuffer cmd);    // recordWorld, after the material streams
    void     terrBakeDestroy();                      // shutdown (device idle), before the slot teardown
    bool     terrBakeOn() const;
    // <GMDX> REZ EFFECTS (gmdxrezfx.h; gmdxrezfxvk.cpp): the effect table, draw set 0 binding 45
    // (GMDXRezFx::MAX_SLOTS rows of 80 B, device local), uploaded by rezFxRecord when its content
    // changed -- only while effects run.
    void     rezFxRecord(VkCommandBuffer cmd);       // recordWorld, beside terrBakeRecord
    GpuBuffer mRezFxTable{};
    uint32_t mRezFxGen = 0;                          // GMDXRezFx::table generation on the GPU
    bool     mRezFxValid = false;                    // the GPU table was zeroed once
    // PHYSICAL SKY (RenderVKPhysicalSky; gmdxphysskyvk.cpp, clean_physky*.glsl): Hillaire's
    // lookup tables, the sky-view and transmittance ones at draw set 0 bindings 46 / 47 (made
    // and written once in writeDescriptors, in GENERAL; valid even while the mode is off).
    void     physSkyFill(VKSSRInfo& si);             // frame info fill: si.physky from si.atmos
    void     physSkyInit();                          // writeDescriptors: images, set 0 46 / 47
    void     physSkyRecord(VkCommandBuffer cmd);     // recordWorld, beside rezFxRecord: the LUTs
    void     physSkyDestroy();                       // shutdown (device idle)
    GpuImage mPkTrans{}, mPkMs{}, mPkView{};
    VkSampler mPkSampler = VK_NULL_HANDLE;           // linear / clamp
    VkDescriptorSetLayout mPkDsl = VK_NULL_HANDLE;   // 0 out (storage), 1 transmittance, 2 MS
    VkDescPool mPkPool;
    VkDescriptorSet mPkSet[3] = {};                  // trans, MS, sky view
    VkPipelineLayout mPkLayout = VK_NULL_HANDLE;
    VkPipeline mPkPipe[3] = {};
    float    mPkPush[8][4] = {};                     // this frame's parameters (= si.physky[0..7])
    float    mPkMediumKey[16] = {};                  // what the transmittance / MS tables were built for
    bool     mPkMediumValid = false;
    bool     mPkLayoutsReady = false;                // the images left UNDEFINED once
    bool     mPkPipeFailed = false;
    // </GMDX>
    bool     terrBakeStartJob(TerrBakeRegion& r, uint32_t res);
    void     terrBakeFreeSet(TerrBakeSet& s);
    void     terrBakeAbortJob(TerrBakeRegion& r);
    uint32_t terrBakeAllocSlot(const GpuImage& img);
    VkDescriptorSetLayout mDslDefer = VK_NULL_HANDLE;   // set 2: G0-G3, depth, HDR out, glow out
    VkDescPool       mDeferPool;
    VkDescriptorSet  mDsetDefer = VK_NULL_HANDLE;
    VkPipelineLayout mDeferLayout = VK_NULL_HANDLE;    // draw sets 0 / 1 + set 2, 80 B push
    VkPipeline       mDeferPipe = VK_NULL_HANDLE;
    VkImageView      mDeferViews[7] = {};              // views the set points at (rewritten on change)
    int              mDeferLogged = -1;                // last logged path (0 forward, 1 deferred)
    // Whether this frame's pass 1b goes deferred, and why not (for the one-line log).
    bool deferredWanted(bool prepass, bool od, const char** why) const;
    // (Re)create the G-buffer for the render size and point set 2 at it, the depth and
    // this frame's HDR / glow targets; false = deferred unavailable this frame.
    bool ensureDeferred(VkCommandBuffer cmd, uint32_t rw, uint32_t rh);
    // The lighting dispatch (outside any rendering scope; layouts handled by the caller).
    // beforeCombine (split kernels only): called between the shadow / reflection resolves and
    // the combine -- the async AO's join (RenderVKAOAsync), which may replace `cmd`.
    void recordDeferredLighting(VkCommandBuffer& cmd, uint32_t rw, uint32_t rh, uint32_t viewIndex,
                                const float viewProjRel[16],
                                const std::function<void(VkCommandBuffer&)>* beforeCombine = nullptr);
    // The split kernels' combine (feat: BASE + LOCAL; else COMBINE) + the lit-guard / shader-clock
    // ends; the split layout's sets + push already bound (recordDeferredLighting, recordDeferTail).
    void recordDeferCombine(VkCommandBuffer cmd, bool feat, uint32_t rw, uint32_t rh, bool onAsync);
    void destroyDeferredTargets();

    // ---- ALIASED TRANSIENT TARGETS (#615, RenderVKAliasTargets; VkAliasArena) ----
    // INVENTORY (R = render extent, D = display extent; B/px; G = graphics queue, A = async
    // compute; "hist" = read in a later frame -> never aliased):
    //  depth D32 4R (pre-pass .. post: SSR copy, DoF, god rays, FX, depth upscale; hist via
    //   mPrevDepth) | HDR RGBA16F 8D, scene HDR 8R (scaled) | glow RGBA16F (or B10G11R11,
    //   RenderVKGlowTargetCompact) 8R | G-buffer 4+8+4+4 R: written pass 1b (G), read by the
    //   lighting compute (G or A, joined before scope 2), dead after it | refraction RGBA16F
    //   8R (stale-contents skip path: kept) | water exclusion R8 1R (mWaterExclAllOne: its
    //   contents survive frames) | reactive RGBA8 4R | motion RG16F 4R + no-temporal R8 1R
    //   (TAA, motion blur, FX, RT mask, VRS) | AO normal + AO A/B (read by the lighting
    //   and FX) | hist: TAA colour 2x8D, accumulation 2x R32F 4D, glow 2x 8D; SSR previous
    //   colour 8R + depth 4R; VRS rate; Hi-Z; RT mask histories; exposure; flare state;
    //   probe arrays | post (G): bloom pyramid RGBA16F ~10.7D (mip 0 at RenderBloom-
    //   ResolutionScale), cross filter 3x RGBA16F at D/4 = 6D, SMAA 10D, DoF, final 4D,
    //   sphere 4D, LDR glow chain | snapshot accumulators (offscreen).
    // ALIASED (phase 0 | phase 1 share one block): the G-buffer | the bloom pyramid + the
    // cross-filter buffers. The G-buffer is dead once the lighting compute has read it
    // (an async lighting segment is joined, ALL_COMMANDS, before scope 2); the bloom /
    // cross images are first written after the TAA / FSR resolve and last read by the
    // tonemap; nothing reads either in a later frame. Every first use transitions from
    // UNDEFINED behind kAliasSrcStages / COLOR_ATTACHMENT_WRITE (beginGbuffer,
    // recordBloom, recordCrossFilter), and the tonemap's two static bloom / cross bindings
    // are re-established on frames that did not write them. ~17 B / display px saved.
    // Below render scale 1 (FSR / upscaling: the bloom phase is the larger one) phase 0 also
    // holds the split lighting's sun visibility + reflection intermediates (16 B / render
    // px, RenderVKDeferredSplit; dedicated at scale 1, see mDslDeferSplit): written and
    // read inside the lighting segment, beside the G-buffer (disjoint from it); first use
    // UNDEFINED behind kAliasSrcStages (recordDeferredLighting).
    // Offscreen renders (snapshots, 360) create these the plain way: the arena keeps the
    // on-screen layout. Everything else above stays dedicated: history, cross-frame
    // contents, or a reader in the other phase's window (AO / motion / normals feed both
    // the lighting and the post chain's FX).
    enum AliasSlot : uint32_t
    {
        AS_GBUF0 = 0,
        AS_BLOOM0 = AS_GBUF0 + kGbufCount,
        AS_CROSS_A = AS_BLOOM0 + BLOOM_MAX_MIPS,
        AS_CROSS_B, AS_CROSS_ACC,
        AS_DSPLIT0,                       // phase 0: the split lighting's intermediates (mDsImg)
        AS_COUNT = AS_DSPLIT0 + 6         // (DS_ALIASED: static_assert at mAlias.setup)
    };
    static constexpr VkPipelineStageFlags2 kAliasSrcStages = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
        | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkAliasArena mAlias;
    uint64_t     mGbufAliasEpoch = 0, mBloomAliasEpoch = 0;   // mAlias.epoch() at creation
    bool         mGbufAliased = false, mBloomAliased = false;
    // The set was created at another arena epoch (a retired block, or the setting moved):
    // re-create it. Never offscreen (those renders keep / make plain targets).
    bool aliasStale(uint64_t epoch) const { return !mOffscreen && epoch != mAlias.epoch(); }
    void logAlias(const char* who, bool ok);

    // ---- Reflection probes (see setProbes / recordPrbGpu / recordProbeCapture) ----
    float    mProbeEnvSnap[64] = {};   // atmos lanes at the last full probe invalidation
    int      mProbeVintageSnap = -1;   // RenderHDREnabled | emissive buffer << 1 the captures were made with

    // ---- GPU-DRIVEN REFLECTION PROBES (#478; clean_probe_gpu.comp, clean_probe_job.glsl) ----
    // Placement (automatic probes from a world-anchored 16 m occupancy grid over the clean
    // instances), residency (cube slots, hysteresis, GPU free list) and capture scheduling
    // all run on the GPU; nothing is read back. The CPU uploads world data only -- manual
    // probe objects (stable table positions) and the regions' land / water heights -- and
    // records a fixed stream each frame: the GPU passes, then the scheduled job's face
    // (CPU face cycle) and, with a dynamic manual probe present at detail >= 2, the realtime
    // job's faces; their cameras come from the GPU job records (ring-entry patch + device
    // address), empty jobs cull nothing and dispatch zero filter / trace workgroups.
    // Sizes / layouts = clean_probe_gpu.comp (static_assert'd below).
    static constexpr uint32_t kPrbWX = 48, kPrbWZ = 16;                  // occupancy window (cells)
    static constexpr uint32_t kPrbCells = kPrbWX * kPrbWX * kPrbWZ;      // 36864
    static constexpr uint32_t kPrbManCap = 256, kPrbRegCap = 32;
    static constexpr uint32_t kPrbGridCap = kPrbRegCap * 64;
    static constexpr uint32_t kPrbCandMan = 1, kPrbCandGrid = kPrbCandMan + kPrbManCap;
    static constexpr uint32_t kPrbCandAuto = kPrbCandGrid + kPrbGridCap;
    static constexpr uint32_t kPrbCands = kPrbCandAuto + kPrbCells;
    static constexpr uint32_t kPrbSlotCap = 256, kPrbBatch = 64, kPrbHist = 512;
    static constexpr uint32_t kPrbJobs = 2;          // PJ_SCHED, PJ_REALTIME
    static constexpr uint32_t kPrbJobSched = 0, kPrbJobRealtime = 1;
    static constexpr uint32_t kPrbPatchMax = 24;     // clean_probe_patch.comp PATCH_MAX
    enum PrbPhase : uint32_t { PH_CLEAR = 0, PH_BIN, PH_RESOLVE, PH_PREP, PH_RAYS, PH_FINAL,
                               PH_RANK_A, PH_RANK_B, PH_RANK_C, PH_RANK_D, PH_LIGHTS };
    static constexpr uint32_t PF_ENABLED = 1, PF_ENV_RESET = 2, PF_RT_OK = 4, PF_REALTIME = 8, PF_MANUAL = 16;
    static constexpr uint32_t PF_SPLIT = 32, PF_RTU = 64;   // #565 (job record index in bits 16+)
    static constexpr uint32_t PF_ENV_DRIFT = 128, PF_IDLE = 256, PF_RT_IDLE = 512;   // #772
    static constexpr uint32_t PJ_SKY = 1, PJ_DYNAMIC = 2, PJ_TRACED = 4;   // job info.z
    static constexpr uint32_t MF_VALID = 1, MF_DYNAMIC = 2, MF_PARALLAX = 4;   // manual key.z
    struct PrbJobGPU          // clean_probe_job.glsl PrbJob
    {
        float    origin[4];   // xyz agent, w near
        float    clip[4];     // x far, y radius
        uint32_t info[4];     // valid, slot, PJ_*, candidate
        uint32_t info2[4];    // key lo, key hi, light list, -
        float    vp[6][16];
        uint32_t filt[16][4]; // VkDispatchIndirectCommand (+ pad) per filter dispatch
        uint32_t trace[4];
    };
    static_assert(sizeof(PrbJobGPU) == 720, "PrbJobGPU = clean_probe_job.glsl PrbJob");
    static constexpr VkDeviceSize kPrbJobFiltOff = 448, kPrbJobTraceOff = 704;
    static constexpr uint32_t kPrbIrrArg = 15;       // PJ_IRR_ARG
    struct PrbCellGPU { uint32_t w[28]; };           // clean_probe_gpu.comp PrbCell (opaque to the CPU)
    static_assert(sizeof(PrbCellGPU) == 112, "PrbCellGPU = PrbCell");
    struct PrbRegionGPU       // clean_probe_gpu.comp PrbRegion
    {
        int32_t cell[4];      // SW corner (global 16 m cells) x / y, width in cells, valid
        float   info[4];      // water height, land sample spacing (m)
        float   grid_z[64];
        float   land[289];
        float   pad[3];
    };
    static_assert(sizeof(PrbRegionGPU) == 1456, "PrbRegionGPU = PrbRegion");
    struct PrbManualGPU       // clean_probe_gpu.comp PrbManual
    {
        float    sphere[4];
        float    box[16];
        float    params[4];   // ambiance, near clip, is_box
        uint32_t key[4];      // key lo, key hi, MF_*
    };
    static_assert(sizeof(PrbManualGPU) == 112, "PrbManualGPU = PrbManual");
    static constexpr VkDeviceSize kPrbCandBytes = 32, kPrbSlotBytes = 64;   // PrbCand, PrbSlot
    static constexpr VkDeviceSize kPrbGlobalsFixed = 5696;                  // PrbGlobals (then the queue)
    static constexpr VkDeviceSize kPrbCntOff = 5568;                        // PrbGlobals::cnt[16] (#772)
    // #772 CHANGE LISTS (clean_probe_gpu.comp binding 13): header, then the changed static
    // spheres (agent space, w radius) and the dynamic contents (sphere + DYN_* flags).
    static constexpr uint32_t kPrbChgCap = 1024, kPrbDynCap = 512;   // PRB_CHG_CAP / PRB_DYN_CAP
    static constexpr uint32_t kPrbDynMoving = 1, kPrbDynAvatar = 2, kPrbDynChurn = 4;  // DYN_*
    struct PrbChangeHdr
    {
        uint32_t n[4];    // changed spheres, dynamic entries, overflow, -
        float    p0[4];   // dynamic refresh s, safety s, env trickle s, min angle (rad)
        float    p1[4];   // realtime still-dynamic refresh s, realtime safety s, churn refresh s
    };
    static_assert(sizeof(PrbChangeHdr) == 48, "PrbChangeHdr = clean_probe_gpu.comp Change header");
    struct PrbDynGPU { float s[4]; uint32_t f[4]; };
    static_assert(sizeof(PrbDynGPU) == 32, "PrbDynGPU = clean_probe_gpu.comp DynEnt");
    static constexpr VkDeviceSize kPrbChgSphOff = sizeof(PrbChangeHdr);
    static constexpr VkDeviceSize kPrbChgDynOff = kPrbChgSphOff + (VkDeviceSize)kPrbChgCap * 16;
    static constexpr VkDeviceSize kPrbChgBytes = kPrbChgDynOff + (VkDeviceSize)kPrbDynCap * sizeof(PrbDynGPU);
    struct PrbPushData        // clean_probe_gpu.comp PrbPush
    {
        uint32_t phase, count, first, flags;
        int32_t  agent_cell[4];   // xyz agent origin cell, w probe level
        float    cam[4];          // xyz camera, w now (s)
        float    eye_rt[4];       // xyz main eye, w TLAS radius
        uint32_t slot_n, face, detail, mips;
        float    far_clip, period, sky_amb;
        uint32_t res, cap_res, light_count, rt_faces, inst_groups, job_base;
        uint32_t alpha_pol;       // #834 RenderVKProbeAlpha (0 none, 1 near + realtime, 2 all)
        float    alpha_dist;      // #834 RenderVKProbeAlphaDist (m past the probe's influence sphere)
    };
    static_assert(sizeof(PrbPushData) == 124 && sizeof(PrbPushData) <= kVkPushConstantBudget, "PrbPushData");
    struct PrbPatchPushData   // clean_probe_patch.comp PatchPush
    {
        uint32_t jobs[2], views[2], culls[2];
        uint32_t job, n;
        uint32_t ent[kPrbPatchMax];
    };
    static_assert(sizeof(PrbPatchPushData) == 128 && sizeof(PrbPatchPushData) <= kVkPushConstantBudget, "PrbPatchPushData");
    GpuBuffer mPrbGlobals{}, mPrbCells{}, mPrbRegions{}, mPrbManual{}, mPrbCands{}, mPrbSlots{}, mPrbJobs{};
    GpuBuffer mPrbChange{};           // #772 change lists (binding 13)
    VkDeviceAddress mPrbJobsAddr = 0, mPrbRingAddr = 0;
    VkDescriptorSetLayout mDslPrb = VK_NULL_HANDLE;
    VkDescriptorSet       mDsetPrb = VK_NULL_HANDLE;
    VkDescPool            mPrbPool;
    VkPipelineLayout      mPrbLayout = VK_NULL_HANDLE, mPrbPatchLayout = VK_NULL_HANDLE;
    VkPipeline            mPrbPipe = VK_NULL_HANDLE, mPrbPatchPipe = VK_NULL_HANDLE;
    bool     mPrbDescOk = false;      // the set names live buffers (writePrbDescriptors)
    bool     mPrbNeedClear = true;    // occupancy cells + globals + candidates: reset once
    bool     mPrbNeedSlotReset = true;// cube slots reallocated: the GPU slot table restarts
    bool     mPrbActive = false;      // this frame: probes on and allocated (setProbes)
    bool     mPrbRealtime = false;    // this frame records the realtime job
    bool     mPrbEnvReset = false;    // this frame: the environment changed, every capture stale
    bool     mPrbAnyDynamic = false;  // a dynamic manual probe is uploaded (avatars in captures)
    uint32_t mPrbFace = 0;            // the scheduled job's face this frame (cycle 0..5)
    uint32_t mPrbBinFrame = 0;        // frames since the last occupancy re-bin
    uint64_t mPrbRegionSig = 0;       // region set + agent region at the last re-bin
    int32_t  mPrbAgentCell[3] = {0, 0, 0};
    float    mPrbCam[3] = {0.f, 0.f, 0.f};   // main camera (agent space)
    int      mPrbLevel = 3;
    float    mPrbSkyAmb = 0.f, mPrbPeriod = 2.f;
    double   mPrbClock0 = -1.0;       // renderer clock origin (the GPU's "now" in s)
    // Stable table positions (bounded: kPrbManCap / kPrbRegCap): key -> index; CPU copies of
    // what the GPU holds, uploaded per changed entry.
    std::unordered_map<uint64_t, uint32_t> mPrbManIndex;
    std::vector<PrbManualGPU> mPrbManCPU;          // kPrbManCap entries
    std::vector<uint8_t>      mPrbManDirty;        // per entry: upload this frame
    std::unordered_map<uint64_t, uint32_t> mPrbRegIndex;
    std::vector<PrbRegionGPU> mPrbRegCPU;          // kPrbRegCap entries
    std::vector<uint32_t>     mPrbRegVersion;      // uploaded LLVKProbeRegion::version (0 = none)
    std::vector<uint8_t>      mPrbRegDirty;
    std::vector<std::array<float, 4>> mPrbDynSpheres;   // dynamic manual probes at detail 3 (RT candidacy only, #517)
    // ---- #772 RECAPTURE ON CHANGE (clean_probe_gpu.comp IDLE SCHEDULER) ----
    // prbGatherChanges (recordWorld, after computeAvatarVis, before recordPrbGpu) builds this
    // frame's change lists from the ingest's changed / mover spheres (gVKChangedSpheres,
    // gVKDynSpheres: read only, invalidateProjShadows still owns and clears them), changed
    // region terrain and the avatars; recordPrbGpu uploads them. It also decides the planar
    // mirror's idle skip. Settings: RenderVKProbeIdleScheduler, RenderVKProbeRealtimeIdle,
    // RenderVKMirrorIdleSkip (false = the old behaviour) and their tunables.
    bool     mPrbIdle = false;          // this frame: the idle scheduler (PF_IDLE)
    bool     mPrbRtIdle = false;        // this frame: realtime skips allowed (PF_RT_IDLE)
    bool     mPrbEnvDrift = false;      // this frame: small environment drift (PF_ENV_DRIFT)
    bool     mPrbEnvChanged = false;    // this frame: any atmos lane moved since last frame (mirror)
    bool     mPrbTrackChanges = false;  // the ingest must collect changed spheres for the probes / mirror
    float    mProbeEnvFrame[64] = {};   // last frame's atmos lanes (mPrbEnvChanged)
    double   mPrbSettleUntil = 0.0;     // arrival (region set / agent region changed): short safety age until then
    std::vector<float>     mPrbChgCPU;  // this frame's changed spheres (x, y, z, r)
    std::vector<PrbDynGPU> mPrbDynCPU;  // this frame's dynamic contents
    std::vector<PrbDynGPU> mPrbAvDyn;   // computeAvatarVis: every drawn avatar (sphere, moving bit)
    // CLASSIFICATION (prbGatherChanges): a change that repeats at the same place within
    // RenderVKProbeChurnSec (a script cycling colours, a texture-animated or re-registering
    // prim) and every MOTION sphere (mover start / settle, pose / seat refreshes, a mover's
    // appearance change) is held as moving DYNAMIC content for RenderVKProbeChurnSec past its
    // last occurrence (refresh at RenderVKProbeDynamicRefreshSec, once more when it expires),
    // not a static content change every frame. Keys: quantized sphere (1 m centre, 0.5 m radius).
    struct PrbHeld { float s[4]; double until; uint32_t flags; };   // flags: kPrbDynMoving / kPrbDynChurn
    std::unordered_map<uint64_t, std::pair<double, uint32_t>> mPrbChurnSeen;   // static change key -> last seen (s, frame)
    std::unordered_map<uint64_t, PrbHeld> mPrbHeld;        // held dynamic entries
    uint32_t mPrbAppStatsPrev[8] = {};  // gVKProbeSphereStats at the last log
    std::vector<std::array<float, 4>> mPrbAvPrev;   // per avatar slot: last render position, w valid
    std::vector<std::array<float, 4>> mPrbRegChanged;   // setProbes: changed / new region terrain spheres
    bool     mPrbChgOverflow = false;
    uint32_t mPrbCntPrev[16] = {};      // last logged GPU counters (cumulative)
    bool     mPrbCntHave = false;
    double   mPrbLogNext = 0.0, mPrbLogT0 = 0.0;
    struct PrbLogAcc
    {
        uint64_t frames = 0, chg_spheres = 0, chg_overflow = 0, dyn = 0, dyn_trunc = 0, env_drift = 0, env_reset = 0;
        uint64_t mir_frames = 0, mir_cap = 0, mir_idle = 0, mir_rate = 0, mir_occ = 0;
        uint64_t tag[8] = {};          // gVKChangedSpheres by producer tag (0..6; 7 = untagged)
        uint64_t to_static = 0, to_churn = 0, to_motion = 0, to_animesh = 0, held = 0;   // probe routing
    } mPrbLog;
    std::string mPrbGpuSubLine;         // pr_* / c_prb_* GPU ms of the last 5 s timer window (RenderVKGpuTimers)
    void prbGatherChanges();
    void prbIdleLog(const uint32_t* cnt);
    // RenderVKProbeGPUStats: the globals' debug counters copied to a host-visible ring of 3
    // (read three frames later: frames in flight <= 2), logged every ~2 s. Off: no copy.
    GpuBuffer mPrbStatRb{};
    uint32_t  mPrbStatFrame = 0;
    uint8_t   mPrbStatIssued[3] = {0, 0, 0};
    double    mPrbStatNext = 0.0;
    void      prbStatsCopy(VkCommandBuffer cmd);
    bool initPrb();
    void destroyPrb();
    void writePrbDescriptors();
    // The probe GPU passes (re-bin / placement / residency / scheduling / light lists),
    // recorded in the light block before the light + probe clustering.
    void recordPrbGpu(VkCommandBuffer cmd);
    void prbDispatch(VkCommandBuffer cmd, PrbPushData& pp, uint32_t phase, uint32_t groups, uint32_t count = 0,
                     uint32_t first = 0);
    VkDeviceSize prbJobOff() const;   // this frame slot's job records in mPrbJobs
    // Byte layout = clean_probes_common.glsl.
    struct VKProbeGPU
    {
        float   sphere[4];
        float   box[16];
        float   params[4];   // ambiance, fade, is_box, manual
        int32_t idx[4];      // slot, complete, parallax sphere, class (#478: written by clean_probe_gpu.comp)
    };
    static_assert(sizeof(VKProbeGPU) == 112, "VKProbeGPU = clean_probes_common.glsl Probe");
    static constexpr uint32_t PROBE_CLUSTER_CAP = 24;   // = clean_probes_common.glsl (#249)
    static constexpr uint32_t PROBE_IRR_RES = 32;
    uint32_t  mProbeSlotsN = 0, mProbeRes = 128, mProbeMips = 7, mProbeCapRes = 256, mProbeCapMips = 9;
    GpuImage  mProbeRad{}, mProbeIrr{}, mProbeCap{}, mProbeCapDepth{};
    std::vector<VkImageView> mProbeRadMipViews;   // storage views, one per radiance mip
    VkImageView mProbeIrrStoreView = VK_NULL_HANDLE;
    VkImageView mProbeCapFaceViews[6] = {};
    // REALTIME capture cube (RenderReflectionProbeDetail 2, LL closestDynamic): all six
    // faces every frame, separate from the scheduled job's cube (which keeps the faces
    // earlier frames rendered); shares mProbeCapDepth (faces render one at a time).
    GpuImage    mProbeCapRT{};
    VkImageView mProbeCapRTFaceViews[6] = {};
    VkSampler   mProbeSampler = VK_NULL_HANDLE;
    GpuBuffer   mProbeSSBO{}, mProbeClusterCount{}, mProbeClusterIndex{};
    VkDescriptorSetLayout mDslProbeFilter = VK_NULL_HANDLE, mDslProbeCluster = VK_NULL_HANDLE;
    VkDescPool            mProbePool;   // layout-sized + accounted (vk_descpool.h)
    std::vector<VkDescriptorSet> mDsetProbeFilter;   // radiance mips..., irradiance
    std::vector<VkDescriptorSet> mDsetProbeFilterRT; // same, sourcing mProbeCapRT
    VkDescriptorSet       mDsetProbeCluster = VK_NULL_HANDLE;
    VkPipelineLayout mProbeFilterLayout = VK_NULL_HANDLE, mProbeClusterLayout = VK_NULL_HANDLE, mProbeSkyLayout = VK_NULL_HANDLE;
    VkPipeline mProbeFilterPipe = VK_NULL_HANDLE, mProbeClusterPipe = VK_NULL_HANDLE, mProbeSkyPipe = VK_NULL_HANDLE;
    VkPipeline mProbeDrawPipe = VK_NULL_HANDLE, mProbeMaskPipe = VK_NULL_HANDLE;
    VkPipeline mProbeDrawDsPipe = VK_NULL_HANDLE, mProbeMaskDsPipe = VK_NULL_HANDLE;   // cull none
    // ---- PROBE STORAGE FORMAT (#565, RenderVKProbeFormat) ----
    // 0 RGBA16F, 1 RGB9E5, 2 R11G11B10 (both written through an R32_UINT alias view by the
    // packing filter variants), 3 BC6H (radiance only; written through R32G32B32A32_UINT
    // block-texel views -- one SINGLE-LAYER view per array layer per mip, indexed by the
    // GPU's slot -- by clean_probe_bc6h.comp after each scheduled filter). Format 3 keeps
    // the default probe and the realtime probe UNCOMPRESSED in mProbeLive (RGBA16F,
    // kProbeLiveCubes cubes: 0 = slot 0, 1 = the realtime capture) -- draw set 0 binding 13
    // element [1]; the scheduled filter writes the one-probe RGBA16F mProbeScratch that the
    // encoder reads. The irradiance array is RGBA16F in formats 0 and 3, packed in 1 / 2.
    static constexpr uint32_t kProbeLiveCubes = 2;
    uint32_t    mProbeFmt = 0;                  // effective format of the allocated set
    mutable uint32_t mProbeFmtLogReq = ~0u;     // (the fallback is logged once per request)
    mutable int8_t   mProbeFmtSupported[4] = {-1, -1, -1, -1};   // per-device support, lazily
    GpuImage    mProbeLive{}, mProbeScratch{};
    std::vector<VkImageView> mProbeLiveMipViews, mProbeScratchMipViews;
    std::vector<VkImageView> mProbeBcMipViews;   // [mip * 6n + layer]: single-layer uint4 views
    bool        mProbeBcNeedClear = false;       // zero the BC6H array before its first encode
    VkDescriptorSetLayout mDslProbeEnc = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> mDsetProbeEnc;   // per radiance mip
    VkPipelineLayout mProbeEncLayout = VK_NULL_HANDLE;
    VkPipeline  mProbeEncPipe = VK_NULL_HANDLE;
    // The realtime filter recorded this frame (its job record index): the next frame's GPU
    // probe pass learns which slot live cube 1 holds (PF_RTU).
    bool        mProbeRtFiltRecorded = false;
    uint32_t    mProbeRtFiltJob = 0;
    bool     probeFormatSupported(uint32_t f) const;
    uint32_t probeFormatWanted() const;         // RenderVKProbeFormat -> effective (fallback logged)
    // PROBE-FACE MASK PRE-PASS (#124): depth-only alpha test (clean_prepass_mask.frag,
    // probe formats; [0] cull back, [1] none), then the lit probe FS at depth EQUAL with
    // no depth write -- stacked see-through cutouts are shaded once, not per layer.
    VkPipeline mProbePreMaskPipe[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkPipeline mProbeMaskEqPipe = VK_NULL_HANDLE, mProbeMaskDsEqPipe = VK_NULL_HANDLE;   // live (mLitVar)
    // CAPTURE FS pipelines (#829, LIT_CAP_*; live aliases of mLitVar, selectLitVariant): what
    // recordProbeFace binds with RenderVKProbeCaptureSlim on (a pure bind-time choice -- both
    // sets live for the session, nothing is created or freed by the toggle, #809).
    VkPipeline mCapDrawPipe = VK_NULL_HANDLE, mCapMaskPipe = VK_NULL_HANDLE;
    VkPipeline mCapDrawDsPipe = VK_NULL_HANDLE, mCapMaskDsPipe = VK_NULL_HANDLE;
    VkPipeline mCapMaskEqPipe = VK_NULL_HANDLE, mCapMaskDsEqPipe = VK_NULL_HANDLE;
    VkPipeline mCapAlphaPipe = VK_NULL_HANDLE;
    VkPipeline mCapRigPipe = VK_NULL_HANDLE, mCapRigDsPipe = VK_NULL_HANDLE, mCapRigAlphaPipe = VK_NULL_HANDLE;

    // ---- Sun/moon shadow cascades (LL generateSunShadow / shadowUtil) ----
    // A D32 array (one layer per cascade) rendered after the main cull + pre-skin and
    // sampled by every world FS (set 0 binding 15, comparison sampler). Static casters
    // are culled per cascade (cull mode 3) into their OWN bucket buffers (pass 1 still
    // reads mCmd/mCounters); skinned casters reuse the main pre-skinned buckets 3/4.
    static constexpr uint32_t NUM_CASCADES = 4;
    GpuImage    mShadowMap{};
    VkImageView mShadowLayerView[NUM_CASCADES] = {};
    VkSampler   mShadowSampler = VK_NULL_HANDLE;
    VkSampler   mShadowRawSampler = VK_NULL_HANDLE;   // same maps, raw depth (PCSS blocker search)
    S32         mAvatarShadowDetail = 2;              // RenderAvatarShadowDetail, read per frame (recordWorld)
    // Translucency layer (#212): per cascade at half resolution, the nearest
    // translucent caster's depth (D32, binding 27) and the product of the translucent
    // casters' transmittance (RGBA8, white = clear, binding 28). Rendered after each
    // cascade's depth pass from the alpha-blend buckets (static 2, rigged 5).
    GpuImage    mShadowTransDepth{}, mShadowTransColor{};
    VkImageView mShadowTransDepthView[NUM_CASCADES] = {}, mShadowTransColorView[NUM_CASCADES] = {};
    VkSampler   mShadowTransColorSampler = VK_NULL_HANDLE;
    uint32_t    mShadowTransRes = 0;
    bool        mShadowTransOn = false;       // RenderVKTranslucentShadows, this frame
    bool        mShadowTransWasOn = false;    // toggles drop the cascade cache
    VkPipeline  mShadowTransDepthPipe = VK_NULL_HANDLE, mShadowTransColorPipe = VK_NULL_HANDLE;
    VkPipeline  mShadowTransDepthRigPipe = VK_NULL_HANDLE, mShadowTransColorRigPipe = VK_NULL_HANDLE;
    uint32_t    mShadowRes = 0;
    GpuBuffer   mShadowCmd[5]{};        // opaque / mask / alpha / ds opaque / ds mask casters
    GpuBuffer   mShadowCounters{};
    VkDescriptorSet mDsetCullShadow = VK_NULL_HANDLE;
    VkPipeline  mShadowPipe = VK_NULL_HANDLE, mShadowMaskPipe = VK_NULL_HANDLE;
    VkPipeline  mShadowRigPipe = VK_NULL_HANDLE, mShadowRigMaskPipe = VK_NULL_HANDLE;
    // Cull-none variants: double-sided casters (+ alpha casters, whose FS discards
    // single-sided back faces).
    VkPipeline  mShadowDsPipe = VK_NULL_HANDLE, mShadowMaskDsPipe = VK_NULL_HANDLE;
    VkPipeline  mShadowRigDsPipe = VK_NULL_HANDLE, mShadowRigMaskDsPipe = VK_NULL_HANDLE;
    // <SS:Skoomastream> Translucent shadows on: the LL-solid texels of alpha-blend casters
    // (clean_shadow_mask.frag BLEND_SOLID) into the depth cascade / projector tile.
    VkPipeline  mShadowBlendSolidPipe = VK_NULL_HANDLE, mShadowRigBlendSolidPipe = VK_NULL_HANDLE;
    VkPipeline  mProjBlendSolidPipe = VK_NULL_HANDLE;
    bool        mShadowOn = false;
    float       mShadowMat[NUM_CASCADES][16] = {};   // maps (world - mShadowEye) (#555)
    float       mShadowEye[3] = {};                   // this frame's eye (LLVKWorldFrame::cam_pos)
    float       mShadowPlanes[NUM_CASCADES][6][4] = {};
    float       mShadowClip[4] = {};
    float       mShadowParams[4] = {};    // bias, light offset, res, enabled
    float       mShadowTexel[4] = {};
    // Cascade caching (RenderVKShadowUpdate, #98): what each cascade layer currently
    // holds (the matrix + texel it was rendered with -- sampled as-is while cached)
    // and whether it is re-rendered this frame.
    struct ShadowCache
    {
        bool     valid = false;
        uint32_t frame = 0;          // mShadowFrame when rendered
        float    mat[16] = {};       // relative to eye[] (#555)
        float    eye[3] = {};        // mShadowEye when rendered
        float    texel = 0.f;
        uint32_t deferred = 0;       // #825: frames a due re-render has waited (0 = none latched)
    };
    ShadowCache mShadowCache[NUM_CASCADES];
    bool        mShadowRender[NUM_CASCADES] = {};
    bool        mShadowDeferredNow = false;   // #825: setShadows held a far cascade this frame
    // #564 DYNAMIC-CASTER OVERLAY (cached modes): per cascade the STATIC casters' depth
    // (D32, TRANSFER_SRC between frames); recordShadows copies it into the sampled layer
    // and draws this frame's rigged casters on top, so cached cascades never hold stale
    // avatars. Allocated only while RenderVKShadowUpdate != 0 (RenderVKShadowDynamicOverlay).
    GpuImage    mShadowStatic{};
    VkImageView mShadowStaticLayerView[NUM_CASCADES] = {};
    uint32_t    mShadowStaticRes = 0, mShadowStaticFailedRes = 0;
    // <GMDX> The translucency layer's static half (same lifetime / layout rules as
    // mShadowStatic): the static alpha-blend casters' nearest depth + transmittance. The
    // overlay copies it into the sampled pair and multiplies this frame's dynamic
    // alpha-blend casters (movers, texture-animated faces), worn ones and particles in,
    // so they keep their tint. Null when it could not be allocated (soft): then the
    // translucency layer stays fully cached, as before.
    GpuImage    mShadowTransStaticDepth{}, mShadowTransStaticColor{};
    VkImageView mShadowTransStaticDepthView[NUM_CASCADES] = {}, mShadowTransStaticColorView[NUM_CASCADES] = {};
    bool        mShadowOverlay = false;   // this frame (setShadows)
    // #567: static / dynamic caster split in the cached cascades (overlay + RenderVKShadow-
    // MoverSettleSecs > 0): static layers cull kShadowSplitStatic, the overlay draws the
    // dynamic casters (one cull over the valid cascades' union box) + particles per frame.
    bool        mShadowSplit = false;
    bool        ensureShadowStatic();
    // retire: mid-recording (setShadows) -- the layers go through the retire ring instead
    // of a device wait + immediate destroy (#790 class; 273b17321d).
    void        destroyShadowStatic(bool retire = false);
    uint32_t    mShadowFrame = 0;
    uint32_t    mShadowCacheShift = 0;   // gVKOriginShiftGen at the last render
    // STAGGERED FAR-CASCADE OVERLAY (RenderVKShadowFarCascadeInterval): cascades 2 / 3 refresh
    // their dynamic overlay (static copy + this frame's dynamic / rigged / particle casters)
    // every N frames, alternating; a skipped cascade's sampled layer keeps its last overlaid
    // content. What that content is: valid, the overlay frame, and the cascade cache's
    // render frame it was built on (a re-render -- new matrix -- changes it: refresh).
    struct ShadowOvlState
    {
        bool     valid = false;
        uint32_t frame = 0;     // mShadowFrame of the last overlay
        uint32_t src = 0;       // mShadowCache[c].frame it was overlaid on
    };
    ShadowOvlState mShadowOvl[NUM_CASCADES];
    bool        mShadowOvlRefresh[NUM_CASCADES] = {};   // this frame (setShadows): overlay cascade c
    // The overlay draws cascade c this frame (valid cached map, overlay on, refresh scheduled).
    bool        shadowOverlayDraws(uint32_t c) const
    { return mShadowOverlay && mShadowStatic.image && mShadowCache[c].valid && mShadowOvlRefresh[c]; }
    void        invalidateShadowCache()
    {
        for (ShadowCache& c : mShadowCache) c.valid = false;
        for (ShadowOvlState& o : mShadowOvl) o.valid = false;
    }
    // PER-CASCADE CASTER COUNTS (the hitch / section log: shadowDynC* rigid dynamic draws,
    // shadowRigC* rigged draws per overlaid cascade): the overlay's per-cascade counters
    // copied into a 3-deep host ring, harvested when the entry is reused (3 frames later).
    // Per entry: [c * NUM_COUNTERS ..] the dynamic cull's counters of cascade c, then
    // [kCasRbRig + c * 20 ..] the shadow rig cull's counts of cascade c's view (5 lists x 4).
    static constexpr uint32_t kCasRbRig = NUM_CASCADES * NUM_COUNTERS;
    static constexpr uint32_t kCasRbWords = kCasRbRig + NUM_CASCADES * 20;
    GpuBuffer   mShadowCasRb[3]{};
    uint32_t    mShadowCasRbMask[3] = {};   // bit c: rigid counts of cascade c; bit 4 + c: rigged
    uint32_t    mShadowCasRbFrame = 0;
    void        harvestShadowCasterCounts();
    bool initShadows();
    void finalizePipelines();
    ::PipelineJob* mPipelineJob = nullptr;   // world pipelines compiling (vk_pipeline.h)
    // RAY-QUERY LIT VARIANT, compiled AFTER the world pipelines (cold-start ordering): its
    // pipelines are the heaviest compiles of the renderer and the first frames draw
    // correctly without them (plain variant + sun cascades + shadow-mapped projectors,
    // the state RT warm-up already uses), so the login reveal does not wait for them.
    // A deferred batch (vk_pipeline.h PipelineBatchDeferScope) started once mPipelineJob
    // is done; its handles land in mLitVarRt and move to mLitVar[1] when it finishes.
    ::PipelineJob* mRtPipelineJob = nullptr;
    bool           mRtPipelineStarted = false;
    double         mRtPipesSince = 0.0;   // steady-clock seconds at its start (log)
    void pollRtPipelines(bool wait);
    void destroyShadowTrans();
    bool projTransOn() const { return mShadowTransOn && mProjTransDepth.image && mProjTransColor.image
                                      && mProjTransDepthPipe && mProjTransColorPipe; }
    bool createShadowMap(uint32_t res, bool soft = false);   // cascade array + views + clear + set 0 / 15
    void resizeShadowsIfNeeded();         // live RenderVKShadowResolution change
    void resizeProbesIfNeeded();          // live RenderReflectionProbeCount / Resolution change (#521)
    void configureShadows(bool live);     // #226: pre-checked, soft, steps down (init + live)
    uint64_t shadowBytes(uint32_t res) const;
    uint64_t shadowHeldBytes() const;
    void destroyShadowMap();              // cascades + their views + the translucency layer
    void recordShadows(VkCommandBuffer cmd, const VKCullPush& cullBase,
                       const VKDrawPush& drawPush, bool do_rigged);
    // PARTICLE SHADOW CASTERS (#39, RenderVKParticleShadows): each rendered cascade's own
    // culled particle list (#518: alpha-over pairs in its caster volume) -- alpha-tested depth, or
    // the translucency layer's depth + colour with RenderVKTranslucentShadows. Pipelines
    // on mPartDrawLayout (sets 0/1 + the particle VS set 2), created after the particles.
    VkPipeline  mPartShadowPipe = VK_NULL_HANDLE;
    VkPipeline  mPartShadowTransDepthPipe = VK_NULL_HANDLE, mPartShadowTransColorPipe = VK_NULL_HANDLE;
    void initParticleShadows();
    void destroyParticleShadows();
    bool particleShadowsWanted() const;
    // trans: the translucency layer's depth + colour pipes (else the alpha-tested depth
    // pipe). Binds the particle sets: draw it LAST in the pass.
    void recordParticleShadow(VkCommandBuffer cmd, const VKDrawPush& drawPush, uint32_t cascade, bool trans);

    // ---- PROJECTOR SHADOWS (spot lights stop at the first surface) ----
    // RenderVKProjectorShadows: 0 off, 1 shadow maps, 2 ray traced. Mode 2 (needs the RT
    // foundation, mRT) renders NO atlas tiles: every selected projector (up to
    // RenderVKRTProjShadowMax, kMaxProjRecords) gets a record with params.z = 2 and the
    // lit FS traces one shadow ray per pixel against the scene TLAS (clean_draw_common
    // projShadowRay). Without mRT, mode 2 falls back to 1 (logged once).
    // MODE 1: up to kMaxProjShadows projectors per frame (importance with
    // hysteresis) get a tile of one D32 atlas (4x4 cells, tile 1/1, 1/2 or 1/4 of a
    // cell by screen coverage), rendered depth-only with the projector's own frustum
    // (cull mode 3, cascade 4 = no caster planes) and the shadow pipelines WITHOUT depth
    // clamp (geometry in front of the projector's near plane must clip, not pancake).
    // Tiles are cached: re-rendered only when the light moved / changed tier, something
    // moved or changed in its frustum (gVKChangedSpheres, avatar boxes), or as a
    // periodic safety net. Sampled via set 0 binding 18 (atlas, comparison) + 19 (per
    // shadow records); lights carry their record index + 1 in tex.z.
    // Atlas: kProjCols x kProjRows cells (cell = RenderVKProjShadowAtlasSize / 4) in a
    // D32 image; a STATIC-CASTER cache atlas of the same size holds each tile's
    // world-geometry depth, so a tile with an avatar in its cone is refreshed by a
    // tile copy + rigged draws only (no re-cull / static redraw).
    static constexpr uint32_t kMaxProjShadows = 32;
    static constexpr uint32_t kMaxProjRecords = 256;   // RT mode: no atlas budget (28 KB of records)
    static constexpr uint32_t kProjCols = 8, kProjRows = 4;
    // params: .x world texel per unit distance, .y RT near distance, .z technique (2 = RT), .w valid
    // texel: map: .xy atlas texel (uv), .z receiver normal-offset scale, .w light-offset scale
    //        RT:  .x light radius (m), .y receiver normal offset (m)
    struct ProjShadowGPU { float vp[16]; float rect[4]; float params[4]; float texel[4]; };   // 112 B
    struct ProjSlot
    {
        uint64_t key = 0;
        bool     used = false, have_image = false;
        uint32_t res = 0, last_render = 0, dyn_render = 0;
        float    vp[16] = {}, planes[6][4] = {}, tan[2] = {};
        float    pos[3] = {}, radius = 0.f;
        float    cov = 0.f;                     // apparent size (radius / camera distance)
        bool     dirty = true, dynamic = false;
        bool     dyn_avatar = false;            // #567: an avatar box in the cone (rigged overlay views)
        bool     need_static = false, need_dyn = false;   // this frame's work (invalidate)
        bool     pending_move = false;          // light moved; next_vp not yet rendered
        float    next_vp[16] = {};
        // #567 batched static cull: a region of a batch overflowed for this tile (readback
        // a few frames later) -> it re-renders alone (the full single-tile buckets) until
        // the slot is re-assigned or changes tile size.
        bool     mv_solo = false;
        // #567 static re-render budget: a soft re-render (change sphere / overflow / refresh)
        // held for a later frame; the tile keeps its cached depth until then.
        bool     soft_due = false;
    };
    // #567 BATCHED PROJECTOR TILE CULL (RenderVKProjShadowBatchCull): the frame's re-rendered
    // tiles' static casters culled up to kProjMvViews at a time in ONE three-dispatch cull
    // (clean_cull.comp phases 8-10: the union box once per instance, then each tile's own
    // frustum / caster test), each tile's buckets in its region (R = mBucketCap / tiles,
    // >= kProjMvMinRegion) of the shadow buckets and its counter block (kMvCtrStride words;
    // word kProjMvOverWord = the largest bucket total) of mShadowCounters. A region overflow
    // is read back (mProjMvRb, harvested a ring turn later): that tile is marked mv_solo +
    // dirty and re-renders through the single-tile cull.
    static constexpr uint32_t kProjMvViews = 8;          // = clean_cull.comp TL_MAX_VIEWS (5 x 8 <= the wg_counts stride 44)
    static constexpr uint32_t kProjMvMinRegion = 2048;   // smallest per-tile region worth batching
    static constexpr uint32_t kProjMvOverWord = 31;      // = clean_cull.comp TL_OVER_WORD
    static constexpr uint32_t kCullModeTiles = 32u;      // = clean_cull.comp CULL_MODE_TILES (+ tiles << 8)
    static constexpr uint32_t kProjMvRbSlots = 4;      // (an entry is read >= this many frames after it was written)
    GpuBuffer   mProjMvRb[kProjMvRbSlots]{};            // host-visible: one overflow word per batched tile
    struct ProjMvRbMeta
    {
        uint32_t n = 0;                                  // words written (tiles batched that frame)
        uint8_t  slot[kMaxProjShadows] = {};
        uint32_t region[kMaxProjShadows] = {};           // that batch's R
        uint32_t frame[kMaxProjShadows] = {};            // mProjFrame of the render (stale check)
        uint64_t key[kMaxProjShadows] = {};
        uint32_t rec_frame = 0;                          // mProjFrame when recorded
    };
    ProjMvRbMeta mProjMvRbMeta[kProjMvRbSlots];
    uint32_t    mProjMvRbFrame = 0;                     // ring position (advances per frame that batched)
    void        harvestProjMvOverflow(uint32_t ring_slot);
    void        harvestProjMvOverflowDue();             // every entry old enough (main view, every frame)
public:
    // #567 SHADOW VIEW API (cube-face hosting; additive). A non-projector perspective shadow
    // view -- e.g. one face of a point light's cube shadow map (six requests, keys distinct
    // per face, 90-degree frusta: tan {1, 1}) -- shares the projector atlas and everything
    // its tiles get: caching (static cache + dynamic overlay), invalidation (change spheres,
    // movers, avatar boxes, the sub-texel light-move rule), the batched static cull, the
    // soft re-render budget, tile size by apparent size (radius / camera distance of apex).
    // Queue the frame's requests BEFORE selectProjShadows (setLights) -- they are consumed
    // there, so a view not re-queued next frame frees its tile. Requests compete with the
    // projectors for RenderVKProjShadowMax tiles by `score` (projectors: max colour channel
    // x min(radius / camera distance, 8), x1.5 while holding a tile). Shadow maps only (RT
    // mode traces: requests are ignored). After selection, shadowViewRecord(key) is the
    // view's ProjShadowGPU record index in draw set 0 binding 19 (-1: not shadowed this
    // frame): rect = its atlas tile, vp = its matrix (camera-relative on the GPU, as every
    // record), params.z = 1 (shadow-view map; 0 projector map, 2 ray traced), params.x its
    // world texel per unit distance. Sampling is the consumer's (pick the face by the major
    // axis of P - apex, then the projShadowMap lookup on that record; no light's tex.z
    // names a view record).
    struct ShadowViewReq
    {
        uint64_t key = 0;        // unique per view, never 0 (e.g. hash of light key and face)
        float    vp[16] = {};    // world -> VK clip, column-major, perspective, depth 0..1 (conventional)
        float    tan[2] = {};    // half-angle tangents x / y (1, 1 for a cube face)
        float    apex[3] = {};   // the eye (light position, agent space)
        float    radius = 0.f;   // reach (m): tile size by radius / camera distance
        float    near_d = 0.f;   // near-plane distance (informational: record params.y)
        float    score = 0.f;    // importance against the projectors (see above)
    };
    void    queueShadowView(const ShadowViewReq& r);   // up to kMaxProjShadows per frame
    int32_t shadowViewRecord(uint64_t key) const;     // this frame's record index, -1 none
private:
    std::vector<ShadowViewReq> mShadowViewReqs;                  // queued for the next selectProjShadows
    std::vector<std::pair<uint64_t, uint32_t>> mShadowViewRec;   // this frame's (key, record)
    uint32_t    mProjCntNew = 0;                // per-frame invalidation reasons (select)
    // #567 STATIC / DYNAMIC CASTER SPLIT (RenderVKShadowMoverSettleSecs > 0): tile statics
    // cull with kShadowSplitStatic; the overlay culls the dynamic casters (movers, GPU
    // spinners, worn rigid attachments) once over the dynamic tiles' union box.
    static constexpr uint32_t kShadowSplitStatic = 32768u, kShadowSplitDynamic = 65536u;
    bool        mProjSplit = false;             // this frame (invalidateProjShadows)
    bool        mProjSplitWas = false;          // a toggle re-renders every tile
    GpuImage    mProjAtlas{};
    GpuImage    mProjCache{};                   // static-caster depth per tile (TRANSFER_SRC)
    // Translucency atlas (#212): half resolution, same tile layout in uv: nearest
    // translucent caster depth (D32, binding 29) + transmittance product (RGBA8,
    // binding 30). Re-rendered with each tile's static casters.
    GpuImage    mProjTransDepth{}, mProjTransColor{};
    VkPipeline  mProjTransDepthPipe = VK_NULL_HANDLE, mProjTransColorPipe = VK_NULL_HANDLE;
    uint32_t    mProjAtlasRes = 0;              // cell size in texels
    uint32_t    mProjAtlasW = 0, mProjAtlasH = 0, mProjCapacity = 0;   // cells allocated
    GpuBuffer   mProjShadowSSBO{};
    ProjSlot    mProjSlots[kMaxProjShadows];
    ProjShadowGPU mProjShadowCPU[kMaxProjRecords] = {};
    uint32_t    mProjRecords = 0;               // records in use this frame (upload range)
    uint32_t    mProjMode = 0;                  // effective technique this frame: 0 off, 1 map, 2 RT
    bool        mProjRTFallbackLogged = false;
    // RT mode hysteresis: keys traced last frame (sorted) get the same 1.5x score bonus
    // the atlas slots give in mode 1, so a projector near the RenderVKRTProjShadowMax cut
    // doesn't flip between traced and untraced as other (moving) lights re-rank.
    std::vector<uint64_t> mProjRTKeys;
    // #666: last frame's apex of each ray-traced projector, sorted by key (light motion ->
    // ProjShadowGPU texel[3], the denoiser's history cap; projRtLightMove).
    struct ProjRtPrev { uint64_t key; float origin[3]; };
    std::vector<ProjRtPrev> mProjRTPrev, mProjRTPrevNext;
    // ---- FLICKER DIAGNOSTIC (RenderVKRTDebugFlicker = N > 0; off by default) ----
    // Focus = the N-th ranked ray-traced projector this frame. LLCleanRTVK uses it for
    // the TLAS membership stats (instances near the apex) and debug view 15.
    struct ProjFocus
    {
        bool     valid = false;
        uint64_t key = 0;
        uint32_t light = 0, record = 0;         // index in this frame's light list / record
        float    apex[3] = {}, axis[3] = {};
        float    near_d = 0.f, reach = 0.f, bias = 0.f;
    };
    ProjFocus   mProjFocus;
    std::vector<std::pair<uint64_t, uint32_t>> mProjDbgPrev;   // last frame's (key, record)
    double      mProjDbgNextLog = 0.0;
    uint32_t    mProjDbgChanges = 0, mProjDbgFrames = 0;
    // Bind the draw sets 0/1 (+ the RT consumer set 2 through mDrawLayoutRt when this
    // frame's lit variant is the ray-query one, which statically uses it). Use for EVERY
    // draw-set bind; push constants always go through mDrawLayout (identical range).
    void bindDrawSets(VkCommandBuffer cmd, const VkDescriptorSet* dsets);

    // ---- Lit FS variants (RT split) ----
    // With the RT foundation, every lit pipeline that may trace (world opaque / mask /
    // alpha, rigged, probe capture -- the mirror's static draws share those -- and the
    // particle draws) exists TWICE: plain (kWorldFragSpv, layout sets 0/1) and ray-query
    // (kWorldFragRtSpv, + RT consumer set 2 / particles set 3). recordWorld picks one per
    // frame (selectLitVariant) and copies its handles into the live members (mDrawPipe
    // etc.), so every pass binds the right one. HUD, the mirror's alpha/rigged pipes,
    // shadow / projector / selection pipes never trace: plain only.
    enum LitPipe { LIT_DRAW, LIT_DRAW_DS, LIT_MASK, LIT_MASK_DS, LIT_ALPHA,
                   LIT_RIG, LIT_RIG_DS, LIT_RIG_ALPHA,
                   LIT_PROBE_DRAW, LIT_PROBE_MASK, LIT_PROBE_DRAW_DS, LIT_PROBE_MASK_DS,
                   LIT_EQ, LIT_EQ_DS, LIT_RIG_EQ, LIT_RIG_EQ_DS,   // depth pre-pass EQUAL twins
                   LIT_PROBE_MASK_EQ, LIT_PROBE_MASK_DS_EQ,         // probe-face mask EQUAL (#124)
                   // REFLECTION-PROBE CAPTURE FS (#829, RenderVKProbeCaptureSlim; clean_draw.frag
                   // -DCLEAN_CAPTURE, frag kind LITF_CAP): the probe faces' own twins of
                   // LIT_PROBE_* (which the planar mirror keeps drawing with) + of the mirror's
                   // plain rigged / alpha pipes. The static opaque / mask slots exist in both
                   // variants (ray-query: one hard sun ray); alpha and rigged are PLAIN only, as
                   // the mMirror* pipes they replace (litLive falls back to variant 0).
                   LIT_CAP_DRAW, LIT_CAP_MASK, LIT_CAP_DRAW_DS, LIT_CAP_MASK_DS,
                   LIT_CAP_MASK_EQ, LIT_CAP_MASK_DS_EQ,
                   LIT_CAP_ALPHA, LIT_CAP_RIG, LIT_CAP_RIG_DS, LIT_CAP_RIG_ALPHA,
                   LIT_COUNT };
    VkPipeline*      litLive(LitPipe i);                 // the live member for slot i
    VkPipeline       mLitVar[2][LIT_COUNT] = {};         // [0] plain, [1] ray-query (owned)
    VkPipeline       mLitVarRt[LIT_COUNT] = {};          // [1]'s deferred compile targets (mRtPipelineJob)
    // DEBUG TWINS (clean_draw.frag kDebugViews, specialization constant 1): mLitVar is built
    // with it FALSE -- no debug views / RenderVKLitDebugMask switches / Highlight Transparent
    // tint / RT terminator view in the shipping lit FS. While one of those is on
    // (litDebugWanted) selectLitVariant installs the TRUE twins, built on first use from the
    // descs recorded at init (ensureLitDebug; one synchronous batch, a one-time hitch). A twin
    // that failed / is not built leaves the normal pipeline bound (debug output missing, no
    // crash). Owned; destroyed with mLitVar.
    // LITF_CAP (#829): the capture FS -- kDebugViews is a plain false there, so its slots have
    // no debug twin (ensureLitDebug skips them).
    enum LitFragKind : uint8_t { LITF_LIT, LITF_BLEND, LITF_EZ, LITF_CAP };
    struct LitDbgDesc { GraphicsPipelineDesc desc; bool rigged = false; uint8_t frag = LITF_LIT; bool valid = false; };
    LitDbgDesc       mLitDbgDesc[2][LIT_COUNT];
    VkPipeline       mLitDbg[2][LIT_COUNT] = {};
    bool             mLitDbgBuilt[2] = {};
    // Built in the BACKGROUND (an async pipeline batch, as the spec sets): the workers write
    // mLitDbgStage, copied into mLitDbg when the job is done (pollLitDebug, every frame) --
    // until then the debug view / cost switch is simply not active yet (logged).
    VkPipeline       mLitDbgStage[2][LIT_COUNT] = {};
    ::PipelineJob*   mLitDbgJob[2] = {};
    double           mLitDbgSince[2] = {};
    bool             litDebugWanted() const;             // a debug feature reads the lit FS this frame
    void             ensureLitDebug(int v);
    void             pollLitDebug(bool wait);            // harvest a finished twin job (wait: shutdown)
    // Deferred lighting compute's twin (RenderVKLitDebugMask cost switches in deferred frames).
    VkPipeline       mDeferPipeDbg = VK_NULL_HANDLE;
    bool             mDeferDbgBuilt = false;
    std::future<VkPipeline> mDeferDbgJob;                // (background build, as the lit twins)
    VkShaderModule   mDeferDbgMod = VK_NULL_HANDLE;      // (held until the job lands)
    bool             mDeferDbgNow = false;               // this frame's lighting binds the twin
    bool             mDeferG11 = false;                  // mDeferPipe's module (glow R11G11B10 variant)
    bool             ensureDeferDebug();

    // ---- LIGHT GRID (RenderVKLightCulling 1, Live; clean_deferred_light.comp
    // CLEAN_DEFER_PASS 6) ----
    // The uber / combine kernels' per-16x16-tile light masks, built once per frame by their
    // own small kernel (the in-kernel test + a 2.5D depth-bin test, + the lit-guard cluster
    // scan) instead of inside the lighting kernel; the lit kernels read their tile's 8 words
    // (DEFER_FLAG_LIGHT_GRID). Image-identical to the in-kernel path (the shader header's
    // argument). mLightGrid: kLightGridWords uint per tile of the render extent's 16 px grid
    // (4K: 240 x 129 tiles, ~1 MB), set 2 binding 8 (bound always: the lit kernels declare
    // it; read only under the flag), re-created with the G-buffer's extent (ensureDeferred).
    // [0] on mDeferLayout (the uber frames), [1] on mDeferSplitLayout (the split frames:
    // dispatched before SHADOW / REFLECT, independent of them, behind the combine's barrier).
    static constexpr uint32_t kLightGridWords = 8;   // = TILE_LIGHT_WORDS (MAX_VK_LIGHTS / 32)
    GpuBuffer        mLightGrid{};
    uint32_t         mLightGridTiles = 0;            // tiles mLightGrid holds (set 2 binding 8 points at it)
    VkPipeline       mLightGridPipe[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};

    // ---- FROXEL LIGHT MASKS (RenderVKLightCulling 2, the default; clean_light_froxel.comp) ----
    // The deferred lighting's per-pixel light sets, built from the lights alone in the light pass
    // (recordWorld, after the cluster pass; "c_light_froxel"): tiles of 2^kFroxel shift px (the
    // power of two nearest render height / 32: 64 px at 4K, 32 at 1080p, ~32 tile rows) x
    // kFroxelSlices exponential depth slices (slice 0 to kFroxelNear m, then log-spaced to the
    // draw distance, the last open), FROXEL_WORDS (8) uints per cell: lossless (256 lights =
    // MAX_VK_LIGHTS). 4K (3840 x 2062): 60 x 33 x 48 cells = 3.0 MB. One workgroup column per
    // light ((lights, kFroxelSplit) x 64 lanes) over the cells its bounds cover; zero-filled
    // before (vkCmdFillBuffer). mFroxel is grow-only (high-water: snapshots / resizes never
    // thrash it; re-created behind a device wait, its two descriptors rewritten): mDsetFroxel
    // binding 2 (the build) and mDsetDefer binding 9 (the uber / combine kernels; bound always,
    // read only under DEFER_FLAG_LIGHT_FROXEL). mFroxelNow: this recording built the masks for
    // mFroxelW x mFroxelH (recordDeferredLighting falls back to the in-kernel tiles otherwise).
    static constexpr uint32_t kFroxelSlices = 48, kFroxelSplit = 8, kFroxelWords = 8;
    static constexpr float    kFroxelNear = 1.0f;     // end of slice 0 (m; >= the cluster near)
    GpuBuffer             mFroxel{};
    uint32_t              mFroxelCapCells = 0;
    VkDescriptorSetLayout mDslFroxel = VK_NULL_HANDLE;
    VkDescPool            mFroxelPool;
    VkDescriptorSet       mDsetFroxel = VK_NULL_HANDLE;
    VkPipelineLayout      mFroxelLayout = VK_NULL_HANDLE;
    VkPipeline            mFroxelPipe = VK_NULL_HANDLE;
    bool                  mFroxelNow = false;
    uint32_t              mFroxelW = 0, mFroxelH = 0;
    bool                  ensureFroxelBuffer(uint32_t cells);   // grow-only; false: refused (VRAM)
    void                  writeFroxelDescriptors();             // mFroxel -> both sets (those that exist)
    uint32_t              froxelSetup(uint32_t rw, uint32_t rh, const float viewProjRelJit[16], const float eye[3]);

    // ---- WORLD-SPACE LIGHT RESERVOIR GRID (#869 part 1, ReGIR-style; RenderVKLightGrid, default
    // off; clean_regir.glsl / clean_regir_build.comp) ----
    // A camera-centred clipmap (kRegirLevelsMax nested levels of 32 x 32 x 16 cells, cell edge
    // RenderVKLightGridCellSize x 2^level, RenderVKLightGridLevels of them) whose cells hold R
    // (RenderVKLightGridReservoirs) RIS light samples -- or their whole candidate list when it is no
    // longer than the picks per hit -- rebuilt every frame from the frame's light records by one
    // compute pass in the light pass ("c_light_regir", after the clusters / froxels). Read by the RT
    // reflection hits (clean_deferred_light.comp passes 11 / 12 / 14, clean_rt_shade.glsl
    // RT_SHADE_REGIR) through set 2 binding 12; any later consumer includes clean_regir.glsl.
    // FIXED BUDGET: mRegir is allocated once on first use at the cap (header + 4 x 16384 cells x
    // (16 + 1) x 8 B = 8.5 MiB per population, two populations: 17 MiB) and never grows; the stats
    // line reports live / cap. (#869 Phase 5: population 1 = ReSTIR DI's candidates, see rsWanted.)
    struct RegirHeader   // = clean_regir.glsl RegirHeader (std430)
    {
        float    origin[4][4];   // level l: xyz world min corner (snapped), w cell edge (m)
        uint32_t cfg[4];         // levels, reservoirs R, picks N, frame seed
        float    params[4];      // footprint threshold (m), -, -, -
        uint32_t changed[8];     // lights whose record changed since last frame (host)
        uint32_t reach[8];       // lights that are a candidate of some cell (build)
        uint32_t stat[12];       // GPU counters (clean_regir.glsl REGIR_STATS)
        // (#869 Phase 5) ReSTIR DI: population 1 (clean_regir.glsl ReSTIR DI POPULATION)
        uint32_t rs_cfg[4];      // slots R1, candidates per texel (0 = all), RS_F_* flags, 0
        uint32_t rs_cfg2[4];     // M cap, spatial taps, spatial radius (texels), light-set cut x 1000
        float    rs_params[4];   // point disc radius, fixture size limit, record-less projector disc
                                 // radius, its extra origin bias (m)
        uint32_t rs_stat[8];     // GPU counters (clean_regir.glsl RS_STATS)
        uint32_t rs_em[4];       // (#91) emissive triangle records allocated / skipped + dropped (GPU), the
                                 // cap (host), this build's parity | emitters << 1 (GPU)
        float    rs_em_params[4];// (#91) emission luminance threshold, legacy glow scale, PBR scale, (#1006) weak power
        uint32_t rs_set[8];      // pass 15's light set (bit = light index)
        uint32_t rs_remap[64];   // last frame's light index -> this frame's (bytes; 0xFF gone)
        uint32_t rs_moved[8];    // lights that MOVED since last frame (bit = light index; host,
                                 // RenderVKReSTIRMoveCut: the moving-light shadow rule)
        uint32_t rs_flick[8];    // lights whose RADIANCE changed without moving (host, mRsFlick;
                                 // RenderVKReSTIRFlickerCut: the flickering-light rule)
        uint32_t rs_em_cfg[4];   // (#91) emitter slots per cell, GPU: the emitters' triangles, EM_F_* (1 lit,
                                 // 2 GI split, 4 light takeover), GPU: emitter entries (host: starts at the
                                 // takeover table's rows)
        float    rs_em_params2[4];// (#91) range cutoff (reflected radiance), longest range (m), (#1006) cell budget, rest refresh
        uint32_t rs_em_stat[4];  // (#91) GPU: cells with an emitter, their emitters, texels, rays
        uint32_t rs_em_take_cfg[4]; // (#91 LIGHT TAKEOVER) the table's rows T (host), GPU: lights taken over,
                                 // lights refused (a block dropped), 0
        uint32_t rs_em_cache[4]; // (#986) GPU: records copied from the last build, records built, cell
                                 // candidate pairs, EM_C_* (1: the cell build gathered: pairs past the cap)
        uint32_t rs_em_take[8];  // (#91) GPU: the point lights taken over this frame (bit = light index)
    };
    static_assert(sizeof(RegirHeader) == 784, "RegirHeader = clean_regir.glsl RegirHeader");
    static constexpr uint32_t kRegirLevelsMax = 4, kRegirCellsPerLevel = 32u * 32u * 16u;
    static constexpr uint32_t kRegirResMax = 16, kRegirPicksMax = 4;
    // two populations at a fixed stride (clean_regir.glsl REGIR_RS_BASE): 2 x 8.5 MiB + the header
    static constexpr VkDeviceSize kRegirPopBytes
        = (VkDeviceSize)kRegirLevelsMax * kRegirCellsPerLevel * (kRegirResMax + 1u) * 8u;
    // (#91) + the emissive lights after both populations (clean_regir.glsl EMISSIVE LIGHTS): two parities
    // of triangle records (REGIR_EM_CAP x 112 B each), two parities of the instance hash (REGIR_EM_HASH_N x
    // 8 B) and of the emitter entries (REGIR_EM_INST_CAP x 104 B; #986 RECORD REUSE reads the last build's),
    // the record groups' boxes (REGIR_EM_CAP / 64 x 24 B), population 2 (every cell x (4 +
    // REGIR_EM_SLOTS_MAX) x 8 B: header, slots, the rest's aggregate), the LIGHT TAKEOVER table (256 light
    // ranges + REGIR_EM_TAKE_N rows x 8 B), the #986 CELL LISTS (every cell x 8 B, every 64-cell block x 8 B,
    // REGIR_EM_PAIR_CAP x 4 B; the LONG CELLS after the #869 occlusion cache, every cell x 4 B) -- 15.2 MiB, fixed
    static constexpr uint32_t kRsEmCap = 16384;         // triangle records per parity (= REGIR_EM_CAP)
    static constexpr uint32_t kRsEmRecBytes = 112;      // (= REGIR_EM_WORDS x 8)
    static constexpr uint32_t kRsEmHashN = 8192;        // (= REGIR_EM_HASH_N)
    static constexpr uint32_t kRsEmInstCap = 4096;      // emitter entries (= REGIR_EM_INST_CAP)
    static constexpr uint32_t kRsEmEntBytes = 104;      // (= REGIR_EM_ENT_WORDS x 8)
    static constexpr uint32_t kRsEmTakeN = 1024;        // LIGHT TAKEOVER table rows (= REGIR_EM_TAKE_N)
    static constexpr uint32_t kRsEmGrpBytes = 24;       // per 64 records (3 x 8)
    static constexpr uint32_t kRsEmSlotsMax = 8;        // (= REGIR_EM_SLOTS_MAX)
    static constexpr uint32_t kRsEmPairCap = 1048576;   // (#986) cell candidate pairs (= REGIR_EM_PAIR_CAP)
    static constexpr VkDeviceSize kRsEmHashOffset       // byte offset of parity 0's hash in mRegir
        = sizeof(RegirHeader) + 2u * kRegirPopBytes + 2ull * kRsEmCap * kRsEmRecBytes;
    static constexpr VkDeviceSize kRsEmCellBytes
        = (VkDeviceSize)kRegirLevelsMax * kRegirCellsPerLevel * (kRsEmSlotsMax + 4u) * 8u;   // (REGIR_EM_CELL_WORDS)
    static constexpr VkDeviceSize kRsEmEntOffset = kRsEmHashOffset + 2ull * kRsEmHashN * 8u;   // (parity 0's)
    static constexpr VkDeviceSize kRsEmTakeOffset       // (= REGIR_EM_TAKE_BASE x 8 + the header)
        = kRsEmEntOffset + 2ull * kRsEmInstCap * kRsEmEntBytes
        + (VkDeviceSize)(kRsEmCap / 64u) * kRsEmGrpBytes + kRsEmCellBytes;
    // (#986 CELL LISTS: = REGIR_EM_LCELL_BASE x 8 + the header) the per-cell counts the host zeroes per build
    static constexpr VkDeviceSize kRsEmLCellOffset = kRsEmTakeOffset + (VkDeviceSize)(256u + kRsEmTakeN) * 8u;
    static constexpr VkDeviceSize kRsEmLCellBytes = (VkDeviceSize)kRegirLevelsMax * kRegirCellsPerLevel * 8u;
    // (#869 OCCLUSION CULL, RenderVKReSTIROcclusionCull; clean_deferred_light.comp OCCLUSION CULL) + the
    // occlusion cache after the pairs (= REGIR_OCC_BASE x 8 + the header): 64 B of stats (OCC_STAT_WORDS) + kRsOccCells
    // patches x 200 B (a 4-bit count per light index + the B / V note bits: the whole light set) = 6.25 MiB, fixed
    static constexpr VkDeviceSize kRegirOccOffset = kRsEmLCellOffset + kRsEmLCellBytes
        + (VkDeviceSize)(kRegirLevelsMax * kRegirCellsPerLevel / 64u) * 8u + (VkDeviceSize)kRsEmPairCap * 4u;
    static constexpr uint32_t kRsOccCells = 32768u;      // (= OCC_CELLS)
    static constexpr VkDeviceSize kRsOccStatBytes = 64u; // (= OCC_STAT_U2 x 8: OCC_STAT_WORDS counters, zeroed per frame)
    static constexpr VkDeviceSize kRsOccBytes = kRsOccStatBytes + (VkDeviceSize)kRsOccCells * 200u;   // (OCC_CELL_U2 x 8)
    // (#986 LONG CELLS: = REGIR_EM_LONG_BASE x 8 + the header) after the occlusion cache: the long cells'
    // count (zeroed per build) + the longest list, then the long cells (4 B each, every cell at most)
    static constexpr VkDeviceSize kRsEmLongOffset = kRegirOccOffset + kRsOccBytes;
    static_assert(kRsOccBytes == (8ull + 32768ull * 25u) * 8u, "clean_regir.glsl REGIR_EM_LONG_BASE: OCC_STAT_U2 + OCC_CELLS x OCC_CELL_U2");
    // (#1006 INCREMENTAL CELLS + WEAK OVERLAY, clean_regir.glsl REGIR_EM_X_BASE ..) after the long cells: the cache
    // header (64 x 8 B: the build stamp, VALID, the stored origins; per build from word 16: counters, the
    // indirect dispatch of mode 7, the stats, the age histogram), the forward map (kRsEmCap x 4 B), the records'
    // and entries' side words (two parities x 8 B), the cells' signatures (+ the overlay's list counts: zeroed
    // together per build), the overlay's block words, the build list, the overlay's cell words, the CELL CACHE,
    // then the scratch copies mode 12 moves the cells from (population 2's cell words; the overlay's + the cache)
    static constexpr uint32_t kRsEmOvCells = 2624u;    // (= REGIR_EM_OV_CELLS: 17 x 17 x 9 padded to 41 blocks)
    static constexpr uint32_t kRsEmNCells = kRegirLevelsMax * kRegirCellsPerLevel;   // (= REGIR_EM_NCELLS)
    static constexpr uint32_t kRsEmNCellsAll = kRsEmNCells + kRsEmOvCells;           // (= REGIR_EM_NCELLS_ALL)
    static constexpr uint32_t kRsEmCellWordBytes = (kRsEmSlotsMax + 4u) * 8u;        // (REGIR_EM_CELL_WORDS x 8)
    static constexpr VkDeviceSize kRsEmXOffset = kRsEmLongOffset + 8u + (VkDeviceSize)kRsEmNCells * 4u;   // CC_HDR
    static constexpr VkDeviceSize kRsEmCchFrameOffset = kRsEmXOffset + 16u * 8u;    // (EM_CCH_FRAME: zeroed per build)
    static constexpr VkDeviceSize kRsEmCchFrameBytes = 48u * 8u;
    static constexpr VkDeviceSize kRsEmCchStatOffset = kRsEmXOffset + 20u * 8u;     // (EM_CCH_STAT: 4 x 8 B)
    static constexpr VkDeviceSize kRsEmCchArgsOffset = kRsEmXOffset + 18u * 8u;     // (mode 7's indirect dispatch)
    static constexpr VkDeviceSize kRsEmCchClusterOffset = kRsEmXOffset + 41u * 8u;  // (#1009 EM_CCH_CLUSTER + the skip
                                                                                    //  diagnostic [42..44]: 32 B, stats)
    static constexpr VkDeviceSize kRsEmFwdOffset = kRsEmXOffset + 64u * 8u;
    static constexpr VkDeviceSize kRsEmFwdBytes = (VkDeviceSize)kRsEmCap * 4u;
    static constexpr VkDeviceSize kRsEmSigOffset = kRsEmFwdOffset + kRsEmFwdBytes
        + 2ull * kRsEmCap * 8u + 2ull * kRsEmInstCap * 8u;                         // (after RECX, ENTX)
    static constexpr VkDeviceSize kRsEmSigBytes = (VkDeviceSize)(kRsEmNCellsAll + kRsEmOvCells) * 8u;   // (+ the overlay's LCELL)
    static constexpr VkDeviceSize kRsEmOvCellOffset = kRsEmSigOffset + kRsEmSigBytes
        + (VkDeviceSize)(kRsEmOvCells / 64u) * 8u + (VkDeviceSize)kRsEmNCellsAll * 4u;   // (after the overlay's LBLK, BLD)
    static constexpr VkDeviceSize kRsEmMoveBytes                                    // (the overlay's cells + the cache)
        = (VkDeviceSize)kRsEmOvCells * kRsEmCellWordBytes + (VkDeviceSize)kRsEmNCellsAll * 16u;
    static constexpr VkDeviceSize kRsEmScrOffset = kRsEmOvCellOffset + kRsEmMoveBytes;
    static constexpr VkDeviceSize kRsEmCellOffset = kRsEmTakeOffset - kRsEmCellBytes;   // (population 2's cells)
    static constexpr VkDeviceSize kRegirBytes = kRsEmScrOffset + kRsEmCellBytes + kRsEmMoveBytes;
    static_assert(kRsEmCellBytes == (VkDeviceSize)kRsEmNCells * kRsEmCellWordBytes, "population 2's cell words");
    bool                  mRsOccNow = false;        // this recording's header says RS_F_OCC (regirHeaderFill)
    bool                  mRsOccPrev = false;       // ... and the last one's (a re-enable clears the cache)
    bool                  mRsOccClear = true;       // zero the cache before its next resolve (new buffer / re-enabled)
    bool                  mRsOccRan = false;        // this light list's frame resolved the cache (recordProjStoch)
    bool                  mRsOccRemapOk = false;    // ... the last list's did (setLights): its counts follow rs_remap
    bool                  mRsStatOcc[3] = {false, false, false};   // (the stats slot holds the occlusion counters)
    GpuBuffer             mRegir{};
    VkDescriptorSetLayout mDslRegir = VK_NULL_HANDLE;
    VkDescPool            mRegirPool;
    VkDescriptorSet       mDsetRegir = VK_NULL_HANDLE;
    VkPipelineLayout      mRegirLayout = VK_NULL_HANDLE;
    VkPipeline            mRegirPipe = VK_NULL_HANDLE;
    bool                  mRegirNow = false;        // this recording built population 0 (push rtr.y bit 4)
    bool                  mRegirAllocFailed = false;
    uint32_t              mRegirFrame = 0;          // the build's random seed
    uint32_t              mRegirLevels = 4, mRegirRes = 8, mRegirPicks = 2;
    float                 mRegirCell = 1.f, mRegirFootCm = 2.f;
    bool                  mRegirLogged = false;     // the settings line (on change)
    uint32_t              mRegirLogKey[5] = {~0u, ~0u, ~0u, ~0u, ~0u};
    // CHANGED LIGHTS (setLights, every frame): bit i = light i's record (position, radius, colour,
    // falloff, projector params / origin / axis / matrix, gobo) differs from the same key's last
    // frame (or the key is new / 0). Bounded: one (key, hash) per light of the last frame (<= 256).
    uint32_t              mRegirChanged[8] = {};
    uint32_t              mRegirChangedN = 0;
    std::vector<std::pair<uint64_t, uint64_t>> mRegirPrevSig;
    // (#869) MOVED LIGHTS (RenderVKReSTIRMoveCut; the header's rs_moved): bit i = light i's shadow
    // origin (a point light's centre, a projector's apex / axis) moved more than
    // RenderVKReSTIRMoveEps since its key's last frame. Bounded: one entry per light of the last
    // frame (<= 256), sorted by key.
    // (+ r: the light's radiance last frame -- linear rgb, radius, falloff: the FLICKERING-LIGHT
    // rule, RenderVKReSTIRFlickerCut; mRsFlick bit i = light i's radiance changed by more than
    // RenderVKReSTIRFlickerEps (relative) while its shadow origin did not move)
    struct RsPrevPos { uint64_t key; float p[6]; float r[5]; };
    uint32_t              mRsMoved[8] = {};
    uint32_t              mRsMovedN = 0;
    uint32_t              mRsFlick[8] = {};
    uint32_t              mRsFlickN = 0;
    std::vector<RsPrevPos> mRsPrevPos;
    // (#980) GI CHANGED LIGHTS (GI_F_LIGHT_CHG; the GI header's chg / chg_prev): GI's own mask, not
    // DI's per-frame one. Bit i = light i drifted from its GI REFERENCE (its record when GI last
    // flagged it) by more than max(RenderVKReSTIRMoveEps, RenderVKGILightMoveRel x its radius) --
    // its centre / apex, or its projector axis by that many radians -- or its radiance by more than
    // RenderVKReSTIRFlickerEps (relative, the flicker rule's test). Measured against the reference,
    // so a slow drift / colour fade still flags once it adds up, and a worn light's idle sway (mm
    // per frame, back and forth: DI's 1 mm per-frame rule flagged every facelight every frame) does
    // not. A flagged light's reference becomes its current record. Bounded: one entry per light of
    // the last frame (<= 256), sorted by key.
    struct GiRefLight { uint64_t key; float pr[4]; float cf[4]; float po[3]; float pa[3]; };
    std::vector<GiRefLight> mGiRef;
    uint32_t              mGiChg[8] = {};
    // the flagged lights' OLD records (their reference before this frame's flag), in ascending light
    // index, at most GI_CHG_PREV_MAX (64): [2k] pos_radius (w < 0: the light MOVED -- the old
    // projector frustum is unknown), [2k + 1] color_falloff
    float                 mGiChgPrev[128][4] = {};
    uint32_t              mGiChgPrevN = 0;
    // (#980) the changed-light debug log's window (GPU timers on): flagged lights by reason, and DI's
    // per-frame moved count (the old GI mask) by kind
    struct GiChgLog { double next = 0.0; uint32_t frames = 0; double giMoved = 0, giRad = 0, diMovedAttach = 0,
                      diMovedActive = 0, diMovedOther = 0, diFlick = 0; } mGiChgLog;
    bool                  regirWanted();             // the settings say build population 0 this recording
    // ---- ReSTIR DI (#869 Phase 5, RenderVKReSTIRDI; clean_regir.glsl ReSTIR DI POPULATION,
    // clean_deferred_light.comp pass 15 / deferRsStoch) ----
    // Pass 15's candidates come from population 1 of the light grid (5a), its reservoirs are reused
    // temporally (5b) and from last frame's neighbours (5c). Decided in the light pass (rsWanted):
    // the frame info's bit (foliage_rt.x 64) and the header's flags are the ONE switch both the
    // producer (pass 15) and the consumer (combine / LOCAL) read.
    uint32_t              mRsMode = 0;               // RenderVKReSTIRDI: 0 off, 1 grid candidates, 2 + temporal, 3 + spatial
    bool                  mRsExt = true;             // RenderVKReSTIRLights 1: + every projector and point light
    bool                  mRsAttached = false;       // RenderVKReSTIRAttachedLights
    bool                  mRsNow = false;            // this recording built population 1 (the ReSTIR frame)
    bool                  mRsLogged = false;
    uint32_t              mRsLogKey[4] = {~0u, ~0u, ~0u, ~0u};
    uint32_t              mRsSet[8] = {};            // this frame's light set (rsLightSet)
    uint32_t              mRsSetN = 0, mRsPointN = 0, mRsExtProjN = 0;
    std::vector<uint64_t> mRsPrevKeys;               // last frame's light keys by index (the remap)
    // (#869) SPLIT IDS (setLights -> Light.tex.w; clean_pdn_common.glsl SPLIT CHANNEL): a persistent
    // 16-bit id per light key, unique while held; freed kSplitIdHoldFrames frames after its key was
    // last seen, handed out round-robin (a freed id returns only after all others). Bounded: the keys
    // seen within the hold, hard cap kSplitIdCap (past it, keys not seen this frame are freed).
    struct SplitIdEnt { uint16_t id; uint32_t seen; };
    std::unordered_map<uint64_t, SplitIdEnt> mSplitIds;
    std::vector<uint64_t> mSplitIdUsed;              // 65536-bit in-use set
    uint32_t              mSplitIdNext = 1u, mSplitIdFrame = 0u;
    static constexpr uint32_t kSplitIdHoldFrames = 600u, kSplitIdCap = 8192u;
    void                  splitIdsFrame(const LLVKWorldFrame& wf, size_t n);
    uint8_t               mRsRemap[256] = {};
    bool                  rsWanted();                // the settings + the path say build population 1
    uint32_t              mRsWhyLogged = ~0u;        // the failing conditions last logged (bit mask)
    uint32_t              mGiWhyLogged = ~0u;
    uint32_t              mGiModeSetting = 0;        // RenderVKGI as last read (rtConsumerActive)
    void                  rsLightSet(const LLVKWorldFrame& wf);   // setLights: the set + the remap
    // Stats (clean_regir.glsl RS_STATS; the "ReSTIR DI over N frames" VKGpu line with the GPU timers):
    // the header's rs_stat words copied after pass 15 into a 3-slot host ring, read 3 frames later.
    GpuBuffer             mRsStatRb{};
    bool                  mRsStatUsed[3] = {false, false, false};
    uint32_t              mRsStatSlot = 0;
    uint32_t              mRsStatSetN[3] = {0, 0, 0}, mRsStatPointN[3] = {0, 0, 0};
    uint32_t              mRsStatMovedN[3] = {0, 0, 0};   // (#869: mRsMovedN, the moving lights)
    uint32_t              mRsStatFlickN[3] = {0, 0, 0};   // (#869: mRsFlickN, the flickering lights)
    struct RsStatAcc { uint32_t frames = 0; double stat[8] = {}; double setN = 0, pointN = 0, movedN = 0, flickN = 0;
                       double em = 0, emDrop = 0, emPeak = 0, emInst = 0, emCap = 0; uint32_t emFrames = 0;
                       double emStat[4] = {};    // (#91 rs_em_stat: cells, their emitters, texels, rays)
                       double emTake[3] = {};   // (#91 takeover: table lights, taken, refused)
                       // (#986) records copied / built, cell candidate pairs (sum, peak), gather builds
                       double emCopied = 0, emBuilt = 0, emPairs = 0, emPairsPeak = 0; uint32_t emGather = 0;
                       double emLong = 0, emLongest = 0;   // (#986 LONG CELLS: cells per frame, the longest list)
                       // (#1006 INCREMENTAL CELLS: EM_CCH_STAT -- dirty, re-gathered, forced, forward misses, kept,
                       //  weak emitters per build; the stalest cell left dirty; full builds)
                       double emDirty = 0, emRegather = 0, emForced = 0, emMiss = 0, emKept = 0, emWeak = 0;
                       double emStaleMax = 0; uint32_t emFull = 0, emIncFrames = 0;
                       double emClInst = 0, emClTri = 0;   // (#1009 CLUSTER STATICS: CC_HDR [41] per lists build)
                       double emSkip[5] = {}; uint32_t emSkipFrames = 0;   // (#1009 SKIP DIAGNOSTIC: [42..44])
                       double occ[13] = {}; uint32_t occFrames = 0;   // (#869 OCCLUSION CULL: OCC_STAT words 0-12)
                     } mRsStatAcc;
    // (#91) EMISSIVE LIGHTS (RenderVKReSTIREmissive; clean_regir.glsl EMISSIVE LIGHTS): pass 15's
    // emissive variant, ten dispatches before pass 15 on a ReSTIR frame -- the emitters, their record
    // blocks (+ the instance hash, cleared first), the TRIANGLE records (#986: an unchanged emitter's copied
    // from the last build), the CELL LISTS (count, scan, sums, fill), (#1006) the cell SELECT and population 2
    // (the triangles per grid cell: only the changed cells re-gathered, through an indirect dispatch; + the
    // ORIGIN SHIFT when the clipmap stepped). 1: pass 15 samples them with the lights and the lit kernels add
    // their light (+ the GI split); 2: the records + cells only (cost A/B).
    VkPipeline            mRsEmPipe = VK_NULL_HANDLE;
    bool                  mRsEmNow = false;          // this recording's header asked for the build
    uint32_t              mRsEmMode = 0;             // RenderVKReSTIREmissive as this header read it
    uint32_t              mRsEmParity = 0;           // the NEXT build's record parity (toggled per build)
    // (#986 RECORD REUSE) a build has been recorded since the buffer was made: the other parity holds the
    // last build (the header's EM_F_REUSE); the cell lists this header announced (EM_F_GATHER off)
    bool                  mRsEmBuiltOnce = false;
    bool                  mRsEmListsNow = false;
    bool                  mRsEmLongNow = false;      // (#986 LONG CELLS: mode 9 runs; EM_F_LONG)
    // (#1006 INCREMENTAL CELLS, clean_regir.glsl) the cell cache holds a complete lists build (false: a new
    // buffer, a frame without the lists build -> the next header says EM_F_FULL), the origins and settings it
    // was built with (a change of settings / cell size / a teleport-sized jump: FULL; a step: EM_F_SHIFT, mode
    // 12 moves the cells), and this recording's decisions
    bool                  mRsEmCellsValid = false;
    float                 mRsEmCellOrigin[4][4] = {};
    uint32_t              mRsEmCellKey[12] = {};
    bool                  mRsEmShiftNow = false;     // (EM_F_SHIFT: recordProjStoch copies the cells + runs mode 12)
    bool                  mRsStatInc[3] = {false, false, false};   // (#1006: the slot holds the EM_CCH_STAT words)
    bool                  mRsEmSkipNow = false;      // (#1009: this header says EM_F_SKIPSTAT -- the skip diagnostic)
    bool                  mRsStatSkip[3] = {false, false, false};  // (#1009: the slot holds its counters)
    // RenderVKReSTIREmissive 1 with its kernel and ReSTIR DI on: a ReSTIR frame is wanted even without a
    // light in the set (the emitters are found on the GPU)
    bool                  rsEmLightWanted() const;
    // (#91) LIGHT TAKEOVER (RenderVKReSTIREmissiveLightTakeover; clean_regir.glsl LIGHT TAKEOVER): this
    // frame's table from LLVKWorldFrame::light_takeover (setLights) -- 256 per-light (first row, rows),
    // then the rows (instance, row | light << 16) sorted by instance; uploaded after the grid header when
    // the emissive build runs (mRsEmTakeNow rows: the emitter entries the build reserves for them).
    std::vector<uint32_t> mRsEmTake;                 // (256 + rows) x 2 words
    uint32_t              mRsEmTakeRows = 0;         // the table's rows this frame
    uint32_t              mRsEmTakeLights = 0;       // ... and its lights (stats)
    uint32_t              mRsEmTakeNow = 0;          // the rows this recording's header announced (0: off)
    uint32_t              mRsStatTakeN[3] = {0, 0, 0};
    bool                  mRsStatLists[3] = {false, false, false};   // (#986: the slot holds the long-cell words)
    void                  rsEmTakeTable(const LLVKWorldFrame& wf);
    // ---- (#87) RAY-TRACED GI (ReSTIR GI; RenderVKGI; llcleangivk.cpp, clean_gi_common.glsl) ----
    struct GiHeader   // = clean_gi_common.glsl GiHeader (std430)
    {
        uint32_t cfg[4];
        float    prm[4], prm2[4], prm3[4];
        uint32_t stat[4], stat2[4], stat3[4], stat4[4];
        uint32_t sph_n[4];   // x change spheres, y overflow, z (#980) picked reused samples whose changed
                             // lights were insignificant there (no re-shade; a stat, read back with stat..stat4)
        float    prm4[4];    // x the reservoir candidates' smallest receiver cosine (RenderVKGIReuseCosMin),
                             // y the re-shade gain limit (RenderVKGIReshadeGain; 0 = none)
        uint32_t chg[8];     // GI_F_LIGHT_CHG: the lights moved / radiance-changed (mGiChg: GI's reference rule)
        float    prm5[4];    // (#980) x the re-shade significance threshold (RenderVKGIReshadeThreshold; 0 = any
                             // reach), y the old records in chg_prev (<= 64), z (#1041) the own-fixture box
                             // limit of the hits' point-light rays (RenderVKReSTIRFixtureSize, m)
        float    sph[64][4];
        float    chg_prev[128][4];   // (#980) mGiChgPrev: the flagged lights' old records, ascending light index
    };
    static_assert(sizeof(GiHeader) == 3280, "GiHeader = clean_gi_common.glsl GiHeader");
    class LLCleanDenoiserVK* mGiDn = nullptr;       // the GI signal's own denoiser instance
    VkPipeline            mGiTracePipe = VK_NULL_HANDLE;   // pass 11 -DCLEAN_GI_TRACE
    VkPipeline            mGiResolvePipe = VK_NULL_HANDLE; // (GI-4) the radiance cache's resolve
    // (#1031, RenderVKGISplitKernels) the GI trace as stages (clean_deferred_light.comp THE GI KERNEL
    // SPLIT): TRACE, SHADE, REUSE (+ TAIL), RESHADE -- over mGiHit (set 3 binding 25: the list headers
    // + list + 96 B of records per GI texel); without it (refused / off) the fused mGiTracePipe
    enum GiStage : uint32_t { GIS_TRACE = 0, GIS_SHADE, GIS_REUSE, GIS_RESHADE, GIS_COUNT };
    VkPipeline            mGiSplitPipe[GIS_COUNT] = {};
    GpuBuffer             mGiHit{};
    uint32_t              mGiHitW = 0, mGiHitH = 0;
    double                mGiHitRetryAt = 0.0;       // the records were refused (#697): the next attempt
    bool                  mGiSplitNow = false;       // this recording runs the stages (giDecide)
    bool                  giSplitReady(uint32_t w, uint32_t h) const;
    // (#1036, RenderVKGIShadeSplit) SHADE as MISS / HIT-MAT / HIT-LIGHT, indirect over TRACE's hit / miss
    // lists (mGiHit's headers 1 / 2 + the list space; the material records in the hit records' space);
    // while these compile, SHADE
    enum GiShadeStage : uint32_t { GSS_MISS = 0, GSS_HITMAT, GSS_HITLIGHT, GSS_COUNT };
    VkPipeline            mGiShadePipe[GSS_COUNT] = {};
    bool                  mGiShadeSplitNow = false;  // this recording splits SHADE (giDecide)
    bool                  giShadeSplitReady() const;
    static VkDeviceSize   giHitBytes(uint32_t w, uint32_t h)
    {
        const VkDeviceSize n = (VkDeviceSize)w * h;
        // headers (the re-shade list, #1036 the hit / miss lists), list (4 B / texel), records
        return 16u * (3u + (n + 3u) / 4u) + n * 96u;
    }
    GpuBuffer             mGiState{};                // header + radiance cache (fixed at its size)
    uint32_t              mGiStateSlots = ~0u;       // the cache slots mGiState was made for
    GpuBuffer             mGiRes{};                  // (GI-2) reservoirs: 32 B x 2 parities per GI texel
    uint32_t              mGiResW = 0, mGiResH = 0;
    bool                  mGiResPrevOk = false;
    bool                  mGiNow = false;            // this recording runs GI (decided in the light pass)
    bool                  mGiWanted = false;         // the settings + the path (giDecide)
    uint32_t              mGiFrame = 0, mGiFlags = 0, mGiDebug = 0, mGiAtrous = 4;
    double                mGiRetryAt = 0.0;          // a refused allocation: retry later (#697)
    bool                  mGiLogged = false;
    uint32_t              mGiLogKey = ~0u;
    std::vector<float>    mGiSpheres;                // (GI-4) change spheres for the next header (<= 64)
    bool                  mGiStateFresh = false;     // the state buffer is new: the header write clears the cache
    double                mGiLastActive = 0.0;       // the last frame GI ran (the resources' hold)
    uint32_t              mGiCacheRestarts = 0;      // change-sphere overflows that wiped the cache (session)
    bool                  mRegirReflOn = false;      // the light grid is wanted by the RT reflections themselves
    bool                  mGiSphOverflow = false;
    GpuBuffer             mGiStatRb{};
    bool                  mGiStatUsed[3] = {false, false, false};
    uint32_t              mGiStatSlot = 0;
    struct GiStatAcc { uint32_t frames = 0; double s[20] = {}; double livePeak = 0;
                             double chgLights = 0, allLights = 0; uint32_t hdrFrames = 0; } mGiStatAcc;
    void                  giInit(VkDevice dev);
    void                  giDestroy();
    void                  writeGiBindings();         // set 3 bindings 18-23 (no frame in flight)
    void                  writeGiMotion();
    void                  giDecide(uint32_t rw, uint32_t rh);   // light pass: settings, mGiNow (requests resources)
    // (2026-10-22, the pdnPrestep rule) giDecide runs inside the recording (the light pass, after
    // draw set 0 was bound): it only REQUESTS a resource change; giPrestep (syncFromStoresPrestep,
    // before the next command buffer begins) applies it. RenderVKResizeAtFrameStart 0: in place.
    // (w != 0: giApply; lag / lagFree: the #978 phase-2 resources, giLagEnsure / giLagFree)
    struct GiReq { bool pending = false, free = false; uint32_t w = 0, h = 0, slots = 0; bool wantRes = false; float scale = 0.f;
                   bool wantHit = false;
                   bool lag = false, lagFree = false; uint32_t lagW = 0, lagH = 0; };
    GiReq                 mGiReq;
    void                  giPrestep();
    void                  giFreeAll();               // (device wait) the denoiser, state, reservoirs, records; set 3 18-23 / 25
    // false: refused (wantHit: #1031's records -- a refusal only drops the split)
    bool                  giApply(uint32_t w, uint32_t h, uint32_t slots, bool wantRes, float scale, bool wantHit);
    void                  giWriteHeader(VkCommandBuffer cmd);   // light pass, with the uploads
    void                  recordGI(VkCommandBuffer cmd, const VKDeferPush& p, uint32_t viewOff, bool onAsync);
    bool                  giTraceReady() const;      // recordGI would record the trace (this frame's TLAS, the images)
    // (#978) GI EARLY TRACE (RenderVKGIEarlyTrace; the frame graph: llcleangivk.cpp GI EARLY TRACE):
    // the async lighting chain stops after the GI trace + its denoiser; the combine (BASE + LOCAL /
    // COMBINE) is stashed and recorded on graphics in pass 1b (recordDeferTail) after a join up to
    // the chain's segment alone -- graphics records the probe faces, the acoustics trace, the vol fog
    // / RT probe forks beside it. mGiEarlyArm: recordWorld asks for it on this chain (consumed by
    // deferStashCombine); mGiEarlyValue: the chain segment's compute timeline value.
    bool                  mGiEarlyArm = false;
    uint64_t              mGiEarlyValue = 0;
    bool                  deferStashCombine(bool feat, const VKDeferPush& p, uint32_t viewOff, uint32_t rw, uint32_t rh);
    // (giLag: #978 phase 2 -- this frame's GI trace is recorded after the stashed combine, giLagRecord)
    struct DeferTail { bool pending = false, feat = false, giLag = false; VKDeferPush p{}; uint32_t viewOff = 0, rw = 0, rh = 0; };
    DeferTail             mDeferTail;
    void                  recordDeferTail(VkCommandBuffer& cmd);   // the stashed combine, on graphics
    // ---- (#978 PHASE 2) ONE-FRAME-LATENCY GI (RenderVKGILatencyFrame, default 0, live; the frame graph:
    // llcleangivk.cpp GI LATENCY FRAME). The combine reads LAST frame's GI (reprojected: clean_deferred_light.comp
    // deferGiFetchLag, header GI_F_LAG); this frame's trace + cache resolve + GI denoiser run AFTER the combine --
    // on their own async segment (q_gi_lag) beside the rest of the frame's graphics (scope 2, particles, cel,
    // god rays, pass 2, motion blur, the TAA / upscaler resolve), joined up to its value right before the
    // SSR copy (g_join_gi_lag; at the latest after the HUD), before the bloom / probe filters / P / the tail.
    // RESOURCES (only while the setting is on; giLagEnsure at the frame start, giLagFree when off):
    //  - mGiLagDepth: a copy of the pre-pass depth taken right after the combine (the alpha-depth pass
    //    writes the live depth beside the trace); the trace reads it through mGiLagSet (set 2 with
    //    binding 4 = the copy, the rest copied from mDsetDefer each frame -- one set per frame slot);
    //  - mGiStateFwd: the forward lit FS's copy of the state (header + resolved cells; draw set 0 b53)
    //    -- the trace inserts / accumulates / resolves into mGiState while pass 2 draws.
    bool                  mGiLagWant = false;        // the setting (giDecide)
    bool                  mGiLagNow = false;         // this recording: GI after the combine (GI_F_LAG)
    bool                  mGiLagStale = true;        // ... and last frame's images are unusable (GI_F_LAG_STALE)
    bool                  mGiLagHdrFinalIn = false;  // ... the header's GI_F_FINAL_IN / prm4.w: last frame's trace's
    uint32_t              mGiLagHdrPar = 0;
    struct GiLagLast { bool have = false, finalIn = false; uint32_t par = 0, w = 0, h = 0, rw = 0, rh = 0, frame = 0; };
    GiLagLast             mGiLagLast;                // the last recorded trace (recordGI)
    GpuBuffer             mGiStateFwd{};
    uint32_t              mGiStateFwdSlots = ~0u;
    bool                  mGiFwdBound = false;       // draw set 0 b53 names mGiStateFwd (writeGiBindings)
    bool                  mGiFwdCopied = false;      // this recording copied mGiState -> mGiStateFwd
    GpuImage              mGiLagDepth{};
    uint32_t              mGiLagDepthW = 0, mGiLagDepthH = 0;
    VkFormat              mGiLagDepthFmt = VK_FORMAT_UNDEFINED;
    VkDescPool            mGiLagPool;
    VkDescriptorSet       mGiLagSet[kAsyncSlots] = {};
    VkDescriptorSet       mGiSet2 = VK_NULL_HANDLE;  // recordGI's set 2 (null: mDsetDefer)
    uint64_t              mGiLagValue = 0;           // the q_gi_lag segment not joined yet (compute timeline; 0 none)
    double                mGiLagRetryAt = 0.0;       // a refused allocation (#697): retry later
    bool                  giLagReady(uint32_t rw, uint32_t rh) const;
    bool                  giLagEnsure(uint32_t rw, uint32_t rh);   // (device waits) false: refused
    void                  giLagFree();                             // (device wait)
    void                  giLagDestroyRes();                       // (no wait) the buffers / images
    void                  giFwdCopy(VkCommandBuffer cmd);          // mGiState -> mGiStateFwd (+ barriers)
    void                  giLagRecord(VkCommandBuffer& cmd, bool& depthRoHeld);   // after the stashed combine
    void                  giLagJoin(VkCommandBuffer& cmd);         // before the SSR copy (backstop: after the HUD)
    void                  giNoteSpheres();           // invalidateProjShadows: the frame's change spheres
    std::string           giStatsText();
    // ---- (#439) POINT LIGHT SHADOW MAPS: the non-RT fallback (RenderVKPointShadowMaps; PLAN +
    // the per-frame selection / budget; llcleanprojstochvk.cpp POINT LIGHT SHADOW MAPS) ----
    struct PsmSel { uint32_t light; uint64_t key; uint32_t res; float score; };
    std::vector<PsmSel>   mPsmSel;                   // this frame's selection (<= RenderVKPointShadowMapMax)
    std::vector<uint64_t> mPsmKeys;                  // last frame's keys, sorted (hysteresis)
    uint64_t              mPsmBytes = 0;             // the planned cube-array bytes of the selection
    uint32_t              mPsmLogKey = ~0u;
    void                  pointShadowMapSelect(const LLVKWorldFrame& wf);   // setLights
    void                  rsStatsRecord(VkCommandBuffer cmd);   // recordProjStoch, after pass 15
    // (5b / 5c) THE RESERVOIRS: 8 B per denoiser texel x 2 GEOM parities (half-res 4K: 33 MB),
    // allocated with the denoiser's images while RenderVKReSTIRDI >= 2, freed with them (set 3
    // binding 17; a 16-byte placeholder otherwise). Re-created only when the extent changes.
    GpuBuffer             mRsRes{}, mRsResDummy{};
    uint32_t              mRsResW = 0, mRsResH = 0;
    bool                  mRsResPrevOk = false;      // last frame's pass 15 wrote them at this extent
    double                mRsResRetryAt = 0.0;       // a refused allocation: retry later (#697)
    void                  rsResFrame();              // a sampling frame: ensure / free (pdnPrestep rule: syncFromStoresPrestep)
    bool                  mRsResSampled = false;     // the last recording sampled (pdnFrame): rsResFrame at the prestep
    void                  writeRsResBinding();       // set 3 binding 17 (no frame in flight)
    std::string           rsStatsText();             // "ReSTIR DI over N frames: ..." and resets
    bool                  ensureRegirBuffer();       // once; false: refused (VRAM) / failed before
    void                  writeRegirDescriptors();   // mRegir (or the placeholder) -> both sets
    void                  regirHeaderFill(RegirHeader& h);
    // Stats (GPU timers on; harvested with the RT reflection stats): the header copied into a
    // 3-slot host ring after the trace, + the host's changed mask of that frame.
    GpuBuffer             mRegirStatRb{};
    bool                  mRegirStatUsed[3] = {false, false, false};
    uint32_t              mRegirStatChanged[3][8] = {};
    uint32_t              mRegirStatChangedN[3] = {0, 0, 0};
    uint32_t              mRegirStatLights[3] = {0, 0, 0};
    struct RegirStatAcc { uint32_t frames = 0; double stat[12] = {}; double reach = 0, changedReach = 0,
                          changedAll = 0, lights = 0; double rs[4] = {}; uint32_t rsFrames = 0; } mRegirStatAcc;

    // ---- SPLIT DEFERRED LIGHTING (RenderVKDeferredSplit; clean_deferred_light.comp
    // CLEAN_DEFER_PASS 1-5) ----
    // SHADOW resolve + REFLECT resolve (independent) -> one barrier -> COMBINE, in place
    // of the uber kernel (RenderVKDeferredSplit 1; 0 = the uber kernel; 2, the default = per
    // GPU class, #824 deferSplitMode: split with hardware RT, uber without). Set 3's layout / pool / the pipeline layout at init, the three
    // pipelines (the combine as mDeferG11 picks) in the world batch beside the uber kernel
    // (initDeferSplit: the login shader gate; mDeferSplitOk once it finished). The uber
    // kernel stays built too: debug frames (its RenderVKLitDebugMask twin), snapshots
    // (mOffscreen: no intermediates at a second size), RenderVKDeferredSplit 0, and frames
    // whose intermediates were refused (#697: retried, 1 s backing off to 30 s). The
    // intermediates at first use, freed only with RenderVKDeferredSplit 0 /
    // RenderVKDeferredLighting 0, a resize (re-created), or shutdown. Set 3 is rewritten
    // only when they are re-created, behind vkDeviceWaitIdle (as ensureDeferred). The
    // uber kernel's set 2 / layout are untouched (set 3 is the split's own).
    // Formats + precision: clean_deferred_light.comp's header. Per render px:
    //   mDsImg[DS_SUN]  RGBA8 sqrt(rgb sun tint, sun visibility)            4 B
    //   mDsImg[DS_IRR / DS_SPEC / DS_ENV]  R32UI RGB9E5 irradiance, specular radiance,
    //                   legacy sharp env (the env only written / read on legacy env faces)  3 x 4 B
    //   HALF-RESOLUTION REFLECTIONS (RenderVKDeferReflectHalfRes, default on; passes
    //   REFLECT_HALF + REFLECT_SHARP instead of REFLECT): DS_HALF RGBA32UI at ceil(w/2) x
    //   ceil(h/2) (irradiance + soft specular + the block's plane / normal, 16 B per block =
    //   4 B / px), DS_SPEC full size (the sharp pixels' word), DS_IRR / DS_ENV 1 x 1
    //   placeholders: 8 B / px of reflection data instead of 12 (sharp pixels keep a full-
    //   size word: no pixel list / compaction). Off: DS_HALF is the 1 x 1 placeholder.
    //   VISIBILITY REUSE (RenderVKDeferVisReuse, default on): DS_VDEPTH R32UI (4 B / px; half
    //   view depth + oct Ng of the surface the shadow pass resolved), else 1 x 1. DS_SUN,
    //   mDsVisProj and DS_VDEPTH are ALSO draw set 0 bindings 41-43 (PARTIALLY_BOUND: stale /
    //   unwritten while unused): pass 2's LIT_ALPHA FS reads them (mVisReuseFlags in its push
    //   pad0), so they live past the lighting segment into pass 2 -- still phase 0 of the
    //   alias arena (the bloom / cross phase starts after the TAA / FSR resolve).
    //   mDsVisProj      R32UI array, 4 shadowed projector slots: layer 0 ids, layers 1..3
    //                   R / G / B sqrt bytes -- 2 layers (8 B) while the visibility can't be
    //                   coloured (map projectors without the #212 translucency layer), 4
    //                   (16 B) otherwise; a 1x1 placeholder while the frame has no shadowed
    //                   projector record (never read)
    // mDsImg sit in the #615 alias arena (phase 0, AS_DSPLIT0..) while the render extent is
    // below the display extent (mScaled): only then is the bloom phase the larger one, so
    // they cost nothing there; at scale 1 phase 0 (G-buffer 20 + 16 B / px) is already the
    // larger and the block would just grow by them -- and an arena block only shrinks at
    // its next re-plan, so a freed split would keep its bytes. Dedicated there (a free is
    // a real free). mDsVisProj is always dedicated (its size flips with projectors in
    // view: no arena re-plans for it).
    enum DeferSplitPipe : uint32_t { DSP_SHADOW = 0, DSP_REFLECT, DSP_COMBINE, DSP_REFLECT_HALF, DSP_REFLECT_SHARP,
                                     DSP_COUNT };
    enum DeferSplitImg : uint32_t { DS_SUN = 0, DS_IRR, DS_SPEC, DS_ENV, DS_HALF, DS_VDEPTH, DS_ALIASED };   // mDsImg (AS_DSPLIT0 + i)
    // set 3: mDsImg + mDsVisProj (binding 1) = bindings 0..6 (storage images), then the feature
    // split's (#827) binding 7 mDsGain (storage image) and binding 8 mDsTileList (storage buffer).
    // (#859) RT reflections: binding 9 mDsRtMark, 10 mDsRtTiles (storage buffer), 11 mDsRtTrace,
    // 12 mDsRtFwd (storage images).
    // (#85 phase 2) the diffuse / specular denoiser (mPdn): 13 IN, 14 GEOM, 15 MOT, 16 OUT (storage
    // images, its extent or its 1 x 1 placeholders; writePdnBindings).
    static constexpr uint32_t kDsImgBindings = DS_ALIASED + 1u;
    static constexpr uint32_t kDsBindingGain = kDsImgBindings, kDsBindingTiles = kDsImgBindings + 1u;
    static constexpr uint32_t kDsBindingRtMark = kDsImgBindings + 2u, kDsBindingRtTiles = kDsImgBindings + 3u;
    static constexpr uint32_t kDsBindingRtTrace = kDsImgBindings + 4u, kDsBindingRtFwd = kDsImgBindings + 5u;
    static constexpr uint32_t kDsBindingPdnIn = kDsImgBindings + 6u;   // 13 .. 16
    static_assert(kDsBindingPdnIn == 13u, "clean_deferred_light.comp set 3 binding 13 (uPdnIn)");
    // (#869 Phase 5b) 17 the ReSTIR DI reservoirs (storage buffer: mRsRes, else mRsResDummy)
    static constexpr uint32_t kDsBindingRsRes = kDsImgBindings + 10u;
    static_assert(kDsBindingRsRes == 17u, "clean_deferred_light.comp set 3 binding 17 (rs_res)");
    // (#87) 18-21 the GI denoiser's IN / GEOM / MOT / OUT (images), 22 the GI reservoirs, 23 the GI
    // state (header + radiance cache) (buffers; mRsResDummy until they exist)
    static constexpr uint32_t kDsBindingGiIn = kDsImgBindings + 11u;
    static_assert(kDsBindingGiIn == 18u, "clean_deferred_light.comp set 3 binding 18 (uGiIn)");
    static constexpr uint32_t kDsBindingGiRes = kDsImgBindings + 15u, kDsBindingGiState = kDsImgBindings + 16u;
    // (#859 noise) 24 the RT reflection TEMPORAL history (storage image array, 2 layers: mDsRtHist)
    static constexpr uint32_t kDsBindingRtHist = kDsImgBindings + 17u;
    static_assert(kDsBindingRtHist == 24u, "clean_deferred_light.comp set 3 binding 24 (uRtHist)");
    // (#1031) 25 the split GI stages' records + re-shade list (storage buffer: mGiHit, else mRsResDummy)
    static constexpr uint32_t kDsBindingGiHit = kDsImgBindings + 18u;
    static_assert(kDsBindingGiHit == 25u, "clean_deferred_light.comp set 3 binding 25 (gi_hit)");
    // (#1031) 26 the split shadow core's selection descriptors (storage buffer: mPdnSel, else mRsResDummy)
    static constexpr uint32_t kDsBindingPdnSel = kDsImgBindings + 19u;
    static_assert(kDsBindingPdnSel == 26u, "clean_deferred_light.comp set 3 binding 26 (pdn_sel)");
    // (#1031) 27 the split shadow core's light-sample visibilities (storage buffer: mPdnVis, else mRsResDummy)
    static constexpr uint32_t kDsBindingPdnVis = kDsImgBindings + 20u;
    static_assert(kDsBindingPdnVis == 27u, "clean_deferred_light.comp set 3 binding 27 (pdn_vis)");
    static constexpr uint32_t kDsBindings = kDsImgBindings + 21u;
    VkDescriptorSetLayout mDslDeferSplit = VK_NULL_HANDLE;   // set 3: the storage images
    VkDescPool       mDeferSplitPool;
    VkDescriptorSet  mDsetDeferSplit = VK_NULL_HANDLE;
    VkPipelineLayout mDeferSplitLayout = VK_NULL_HANDLE;     // draw sets 0 / 1 + set 2 + set 3, 80 B push
    VkPipeline       mDeferSplitPipe[DSP_COUNT] = {};
    bool             mDeferSplitOk = false;      // all three compiled (finalizePipelines)
    GpuImage         mDsImg[DS_ALIASED]{}, mDsVisProj{};
    uint32_t         mDsW = 0, mDsH = 0;
    bool             mDsAliased = false;         // mDsImg on the arena (mDsAliasEpoch: its epoch)
    bool             mDsAliasWant = false;       // ... was asked for (render scale < 1, arena on)
    uint64_t         mDsAliasEpoch = 0;
    bool             mDsProjFull = false;        // mDsVisProj at render extent (else the 1x1 placeholder)
    bool             mDsProjRgb = false;         // ... with the G / B planes (4 layers, else 2)
    bool             mDsReflHalf = false;        // mDsImg sized for the half-resolution reflections
    bool             mDsVisReuse = false;        // ... with DS_VDEPTH (visibility reuse) at full size
    uint32_t         mVisReuseFlags = 0;         // this recording's LIT_ALPHA push pad0 (VISR_*; 0 = off)
    double           mDsLastProj = -1e9;
    double           mDsRetryAt = 0.0;           // intermediates refused (#697): next attempt
    uint32_t         mDsAllocFails = 0;          // ... refused attempts in a row (logged once + on recovery)
    bool             mDeferSplitNow = false;     // this frame's lighting records the split
    // (#823, RenderVKDeferShadowSkip) this split frame has nothing that casts on the deferred
    // surfaces -- sun / moon shadows off (mShadowOn: the setting, the bound spec key), no shadowed
    // projector records (DEFER_FLAG_PROJ), not a merged RT shadow frame: the SHADOW pass is not
    // recorded (the combine / BASE take the cloud layer as the sun visibility, as that pass would
    // have stored), visibility reuse is off for the frame, and a fused feature split records the
    // tile classification alone.
    bool             mDeferShadowSkip = false;
    uint32_t         mDeferSplitFlags = 0;       // its push flags (1 = projector slots, 2 = their colour planes,
                                                 // 4 = half-res reflections + bits 8-15 the sharp roughness x 255,
                                                 // 8 = the shadow pass writes DS_VDEPTH)
    static constexpr double kDsProjHoldSecs = 30.0;   // projector slots kept after the last shadowed projector
    static constexpr double kDsRetrySecs = 1.0, kDsRetryMaxSecs = 30.0;   // refused intermediates: retry backoff
    // ---- FEATURE SPLIT (#827, RenderVKDeferredFeatureSplit, live, default on; split frames only;
    // clean_deferred_light.comp CLEAN_DEFER_PASS 7-9, its header's FEATURE SPLIT) ----
    // The combine (DSP_COMBINE, ~120 registers: the local-light walk) replaced by TILE CLASSIFY
    // (beside SHADOW / REFLECT: the grid masks + a list of the tiles any light reaches) -> BASE
    // (the combine without local lights, every pixel, ~half the registers) -> LOCAL (the light
    // walk over the listed tiles only, vkCmdDispatchIndirect; HDR += gain x local sum). The uber
    // kernel stays as it is (debug twin, snapshots, RenderVKDeferredSplit 0).
    //   mDsGain      R32_SFLOAT, render extent (4 B / px, ~32 MB at 4K): BASE's per-pixel slope of
    //                the output on the local sum (clean_surface_light.glsl surfaceLocalGain)
    //   mDsTileList  VkDispatchIndirectCommand (16 B) + one uint per 16 px tile (4K: ~130 KB)
    // Both live with the split's intermediates: allocated on the first frame the setting asks for
    // them (an ensureDeferSplitTargets re-creation, behind its device wait) and then KEPT -- turning
    // the setting off leaves them allocated, so a live toggle never frees memory a recording may
    // still reference (a toggle only changes which kernels the next recording dispatches); freed
    // with the rest (RenderVKDeferredSplit 0 / RenderVKDeferredLighting 0, resize, shutdown).
    // Until then set 3 bindings 7 / 8 hold 1 x 1 / 16 B placeholders (bound, never read).
    // DFP_SHADOW_CLS (#831, RenderVKDeferClassifyFused): the SHADOW kernel with the tile
    // classification as its prologue (CLEAN_DEFER_PASS 10; one 16 px tile per workgroup) in place
    // of CLASSIFY + SHADOW. Optional: outside mDeferFeatOk's verdict -- missing, the frame records
    // the separate CLASSIFY (same outputs).
    enum DeferFeatPipe : uint32_t { DFP_CLASSIFY = 0, DFP_BASE, DFP_LOCAL, DFP_SHADOW_CLS, DFP_COUNT };
    VkPipeline       mDeferFeatPipe[DFP_COUNT] = {};   // CLASSIFY generic only (reads no spec setting); BASE / LOCAL / SHADOW_CLS per spec set too
    bool             mDeferFeatOk = false;       // CLASSIFY / BASE / LOCAL compiled (finalizePipelines; else the plain combine)
    // (#831) The tile builds (CLEAN_DEFER_PASS 6 / 9 / 10: 256-lane workgroups, subgroup
    // reductions) were created with REQUIRE_FULL_SUBGROUPS (VkContext::wgScanFullSubgroups: the
    // device enabled computeFullSubgroups, compute subgroup arithmetic, 256 % max size == 0). Their
    // reductions are correct either way (subgroup-size independent); logged at init.
    bool             mDeferTileFullSubgroups = false;
    VkPipelineShaderStageCreateFlags deferTileStageFlags() const
    {
        return mDeferTileFullSubgroups ? VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT : 0u;
    }
    GpuImage         mDsGain{};
    GpuBuffer        mDsTileList{};
    bool             mDsFeatFull = false;        // mDsGain / mDsTileList at the split's extent (else placeholders)
    bool             mDeferFeatNow = false;      // this frame's split records the feature split
    // ---- HYBRID RT REFLECTIONS (#859, RenderVKRTReflections 1; clean_deferred_light.comp HYBRID RT
    // REFLECTIONS, CLEAN_DEFER_PASS 11-14) ----
    // Split frames: the reflect passes mark the smooth pixels SSR left to the probe in whole 8 px
    // tiles and list those tiles (mDsRtTiles); RT TRACE (11) and RT RESOLVE (13) run indirect over
    // the list (0 groups on an empty screen) and splice the traced reflection into the words BASE /
    // COMBINE read. RT FORWARD (14): the front-most alpha-blend / water layer's reflection at half
    // resolution for the forward lit FS / water FS (draw set 0 binding 45). Uber frames: RT
    // PRE-TRACE (12) -> mRtUber (set 2 binding 11), mixed in by the uber kernel. An OPTIONAL RT
    // consumer: stands down like the acoustics probe (optionalRtStandDownReason), then frees
    // its targets after kRtReflHoldSecs.
    //   mDsRtMark   RGBA32UI, render extent (16 B / px) | 1 x 1 (set 3 binding 9)
    //   mDsRtTiles  kRtTilesHdrBytes header (the dispatch, the ray density counters / level / params)
    //               + a flag and a list entry per 8 px tile + the listed count + the RAY LIST
    //               (count, dispatch, capacity, 64 entries per tile: 4 B / render px; set 3 binding 10)
    //   mDsRtTrace  RGBA16F, render extent (8 B / px) | 1 x 1 (set 3 binding 11)
    //   mDsRtFwd    RGBA16F, half extent (2 B / render px) | 1 x 1 (set 3 binding 12 + draw set 0 b45)
    //   mDsRtHist   RGBA16F x 2 layers, render extent while RenderVKRTReflectionsTemporal (16 B / px) |
    //               1 x 1 x 2 (set 3 binding 24; the resolve's temporal history, ping-pong by parity)
    //   mRtUber     RGBA16F, render extent while uber frames trace | 1 x 1 (set 2 binding 11)
    VkPipelineLayout mRtReflLayout = VK_NULL_HANDLE;   // draw sets 0 / 1 + set 2 + set 3 + RT set 4, 112 B push
    VkPipeline       mRtTracePipe = VK_NULL_HANDLE;    // pass 11 (mRtReflLayout)
    // RAY LIST (#859 perf, RenderVKRTReflectionsRayList): pass 16 selects each listed tile's rays
    // into the list at the end of mDsRtTiles, pass 11 -DCLEAN_RT_LIST traces it one lane per ray.
    VkPipeline       mRtSelectPipe = VK_NULL_HANDLE;    // pass 16 (mRtReflLayout)
    VkPipeline       mRtTraceListPipe = VK_NULL_HANDLE; // pass 11 -DCLEAN_RT_LIST (mRtReflLayout)
    bool             mRtReflRayList = true;              // this frame's RenderVKRTReflectionsRayList
    // (#1036, RenderVKRTReflectionsShadeSplit) the list trace as TRACE / MAT / LIGHT (clean_deferred_light.comp
    // THE RT REFLECTION SPLIT) over per-ray records: mRtRec, kRtRecBytes per list entry for the first
    // mRtRecCap entries, found by device address (push rtr3.zw; rtr3.x = the capacity -- no descriptor);
    // the entries past it take mRtTraceListPipe as a TAIL dispatch. Sized for RenderVKRTReflectionsRayBudget
    // (rtSplitDecide); refused (#697): the fused list trace, retried after mRtRecRetryAt.
    enum RtSplitStage : uint32_t { RTS_TRACE = 0, RTS_MAT, RTS_LIGHT, RTS_COUNT };
    VkPipeline       mRtSplitPipe[RTS_COUNT] = {};
    GpuBuffer        mRtRec{};
    uint32_t         mRtRecCap = 0;              // list entries mRtRec holds
    double           mRtRecRetryAt = 0.0;
    bool             mRtSplitWant = true;        // this frame's RenderVKRTReflectionsShadeSplit
    bool             mRtSplitNow = false;        // this recording traces through the split (rtSplitDecide)
    static constexpr uint32_t kRtRecBytes = 112u;   // (7 uvec4: clean_deferred_light.comp RTR_REC_WORDS)
    void             rtSplitDecide(uint32_t rw, uint32_t rh, double now);
    // (#1039 M1, RenderVKVisQueue; llcleanvisqueuevk.cpp) THE VISIBILITY QUEUE (clean_vis_queue.glsl): the
    // hit-shading splits' sun rays as requests -- the material stage queues (DeferRtSplitMatVq /
    // DeferGiSplitHitMatVq), ONE walk kernel traces them (DeferVisWalk), the lighting stage resolves
    // (DeferRtSplitLightVq / DeferGiSplitHitLightVq: no sun walk). One device-address buffer (no
    // descriptor), a region per consumer: a 256 B header + 64 B per entry (a 48 B request + a 16 B result),
    // entries = the consumer's (the RT records' capacity, the GI texels) within the cap
    // (RenderVKVisQueueMB, 0 = the VRAM tier's: 192 / 64 / 32 MB); the entries past it take the fused
    // lighting stage as a TAIL (push rtr3.y = the first). Sized from the extent / settings only (no growth);
    // a new size allocates first and retires the old buffer. Refused (#697): the fused stages, retry in 30 s.
    // (#1039 M2, RenderVKVisQueue bit 2) the SET 2 variants (DeferRtSplitMatVq2 / DeferRtSplitLayersVq2 /
    // DeferRtSplitLightVq2, DeferGiSplitHitMatVq2 / DeferGiSplitHitLightVq2) also queue the hits' occlusion ray
    // and the hit shadow-ray budget's K light rays (the producer walks the local lights unshadowed) and give the
    // RT split a LAYERS stage (the blend layers): no ray query left in the lighting stages. An entry then holds
    // R requests (RT 2 + K, GI 1 + K: the most it can ask) and S 16 B result slots (RT 4 + K, GI 2 + K).
    enum VisQueueRegionId : uint32_t { VQR_RT = 0, VQR_GI, VQR_COUNT };
    struct VisQueueRegion
    {
        VkDeviceSize offset = 0;    // in mVq
        uint32_t     entries = 0;   // entry capacity; 0 = this consumer is not queued
        uint32_t     wanted = 0;    // the consumer's entries this recording (the tail covers wanted - entries)
        uint32_t     reqPerEntry = 1;   // R: the request capacity is entries x R (never refuses: the most an entry asks)
        uint32_t     slots = 1;         // S: result slots per entry
        uint32_t     budgetK = 0;       // the hit shadow-ray budget K the slots were sized for (set 2)
    };
    GpuBuffer        mVq{};
    VisQueueRegion   mVqRegion[VQR_COUNT];
    uint32_t         mVqCapBytes = 0;            // this recording's cap
    double           mVqRetryAt = 0.0;
    bool             mVqRtNow = false;           // this recording queues the RT reflections' hit rays
    bool             mVqGiNow = false;           // ... and the GI's
    uint32_t         mVqSet = 0;                 // ... through set 0 (M1: the sun) or set 1 (M2: every hit ray)
    VkPipeline       mVisWalkPipe = VK_NULL_HANDLE;   // pass 11 -DCLEAN_RT_LIST -DCLEAN_VIS_WALK (mRtReflLayout)
    enum VisQueueStage : uint32_t { VQS_MAT = 0, VQS_LIGHT, VQS_LAYERS, VQS_COUNT };   // (LAYERS: RT set 1 only)
    VkPipeline       mRtVqPipe[2][VQS_COUNT] = {};  // the RT split's MAT / LIGHT (/ LAYERS) -DCLEAN_VIS_QUEUE [_M2]
    VkPipeline       mGiVqPipe[2][VQS_COUNT] = {};  // the GI shade split's HIT-MAT / HIT-LIGHT -DCLEAN_VIS_QUEUE [_M2]
    // the pipelines compile with the world batch at init only when their RenderVKVisQueue bit is on then;
    // switched on later, in the background (mVqJob): the consumers stay fused until they land
    PipelineJobSlot  mVqJob;
    bool             mVqPipesQueued[2] = {};     // each set queued once (the world batch or mVqJob): never twice
    bool             vqSetReady(uint32_t s) const
    {
        return mVisWalkPipe && mRtVqPipe[s][VQS_MAT] && mRtVqPipe[s][VQS_LIGHT] && (s == 0u || mRtVqPipe[s][VQS_LAYERS])
            && mGiVqPipe[s][VQS_MAT] && mGiVqPipe[s][VQS_LIGHT];
    }
    static constexpr uint32_t kVqHdrBytes = 256u, kVqReqBytes = 48u, kVqSlotBytes = 16u;   // (clean_vis_queue.glsl)
    // the producers' counters (GPU timers on): the regions' header words 0-4, a 3-slot readback ring
    GpuBuffer        mVqStatRb{};
    uint32_t         mVqStatSlot = 0;
    bool             mVqStatUsed[3][VQR_COUNT] = {};
    struct VqStatAcc
    {
        double frames[VQR_COUNT] = {}, req[VQR_COUNT] = {}, wanted[VQR_COUNT] = {}, consts[VQR_COUNT] = {},
               past[VQR_COUNT] = {}, refused[VQR_COUNT] = {}, occ[VQR_COUNT] = {}, lightRays[VQR_COUNT] = {},
               litHits[VQR_COUNT] = {};
        uint32_t peak[VQR_COUNT] = {};
        double lastLog = 0.0;
    } mVqStatAcc;
    // the pipelines (after mRtReflLayout) of the sets in setMask (bit s = set s; 0: the ones the setting asks
    // for -- the world batch at init); the background build passes the sets it wants
    void             visQueueInit(VkDevice dev, uint32_t setMask);
    void             visQueueDestroy();
    void             visQueueDecide();                 // per recording, after giDecide + rtSplitDecide
    VkDeviceAddress  visQueueAddr(uint32_t r) const { return mVq.address + mVqRegion[r].offset; }
    // A region's header for this recording (the WAR barrier on last frame's readers, the write, the barrier
    // to the producer); recAddr / recCap: the RT records' (the RT region's resolve finds them there).
    void             visQueueBegin(VkCommandBuffer cmd, uint32_t r, VkDeviceAddress recAddr, uint32_t recCap);
    // The WALK over a region's requests (indirect; the RT layout's sets bound, base = the consumer's push).
    void             visQueueWalk(VkCommandBuffer cmd, uint32_t r, const VKRtReflPush& base);
    void             visQueueStats(VkCommandBuffer cmd, uint32_t r);   // the region's counters into the ring
    VkPipeline       mRtFwdPipe = VK_NULL_HANDLE;      // pass 14 (mRtReflLayout)
    VkPipeline       mRtResolvePipe = VK_NULL_HANDLE;  // pass 13 (mDeferSplitLayout: no RT set)
    VkPipelineLayout mRtUberLayout = VK_NULL_HANDLE;   // draw sets 0 / 1 + set 2 + RT set 3, 112 B push
    VkPipeline       mRtPreTracePipe = VK_NULL_HANDLE; // pass 12 (mRtUberLayout)
    GpuImage         mDsRtMark{}, mDsRtTrace{}, mDsRtFwd{};
    GpuImage         mDsRtHist{};                // (#859 noise) the resolve's temporal history (set 3 binding 24)
    bool             mDsRtHistFull = false;      // ... at the split's extent (RT targets full + temporal on)
    bool             mDsRtHistWant = false;      // ... asked for at the last creation (a VRAM refusal keeps Full false)
    GpuBuffer        mDsRtTiles{};
    uint32_t         mDsRtTileCount = 0;         // 8 px tiles of mDsRtTiles (0: placeholder)
    bool             mDsRtMarkFull = false;      // the RT targets at the split's extent (RT reflections wanted)
    GpuImage         mRtFwdPlaceholder{};        // 1 x 1 zero (draw set 0 b45 while mDsRtFwd is not full size)
    GpuImage         mRtUber{};                  // set 2 binding 11
    bool             mRtUberFull = false;
    bool             mRtReflNow = false;         // this frame's split marks + traces (TLAS valid + warm)
    bool             mRtFwdNow = false;          // ... + the forward / water layer (RenderVKRTReflectionsForward)
    bool             mRtFwdDirty = false;        // mDsRtFwd holds a traced frame (cleared once tracing stops)
    bool             mRtUberNow = false;         // this frame's uber kernel pre-traces
    bool             mRtReflStandDown = false;   // last logged stand-down state (log once per transition)
    float            mRtReflMaxDist = 64.f;      // this frame's settings (deferSplitFrame)
    float            mRtReflRough = 0.35f;
    float            mRtReflMaxRecv = 48.f;      // receiver gate (m; RenderVKRTReflectionsMaxRecvDist)
    float            mRtReflSharp = 0.1f;        // full-density roughness (RenderVKRTReflectionsSharpRough)
    float            mRtReflBudget = 0.25f;      // rays per render pixel (RenderVKRTReflectionsRayBudget; 0 = none)
    uint32_t         mRtReflFloor = 1u;          // the density level floor (RenderVKRTReflectionsScale)
    uint32_t         mRtReflParity = 0u;         // the level ping-pong's frame parity (recordRtMarkInit)
    bool             mRtReflBlend = true;
    bool             mRtReflHWCull = true;       // reflection rays cull single-sided back faces in hardware (RenderVKRTReflectionsHWCull)
    uint32_t         mRtReflMaskTest = 0u;       // reflection rays' alpha-mask test mode (RenderVKRTReflectionsMaskTest, 0..3)
    bool             mRtReflHitOcc = true;       // GI frames: reflection hits' env occlusion ray (RenderVKRTReflectionsHitOcclusion)
    bool             mRtReflHitOccDbg = false;   // its debug view (RenderVKRTReflectionsHitOccDebug)
    bool             mRtReflHitTermsDbg = false; // the hits' light split (RenderVKRTReflectionsHitTermsDebug)
    bool             mRtReflHazeOcc = true;      // haze in-scatter x the hit occlusion (RenderVKRTReflectionsHazeOcclusion)
    bool             mRtReflBackStop = true;     // reflection rays stop at a near single-sided back face (RenderVKRTReflectionsBackFaceStop)
    bool             mRtReflSizeClasses = true;  // size-class segments (RenderVKRTReflectionsSizeClasses)
    float            mRtReflSmallSize = 0.5f;    // the small class bound (m; RenderVKRTReflectionsSmallSize)
    float            mRtReflSmallPx = 1.f;       // footprint threshold (reflected px; RenderVKRTReflectionsSizeFootprintPx)
    float            mRtReflSmallDist = 16.f;    // fixed segment distance (m; RenderVKRTReflectionsSmallDist)
    bool             mRtReflGradient = true;     // continuous density by roughness (RenderVKRTReflectionsDensityGradient)
    float            mRtReflDensityMin = 1.f / 64.f;   // rho_min (RenderVKRTReflectionsDensityMin)
    float            mRtReflDensityCurve = 1.f;  // ramp exponent (RenderVKRTReflectionsDensityCurve)
    bool             mRtReflStrength = true;     // density by reflection strength (RenderVKRTReflectionsStrength)
    float            mRtReflStrFull = 0.25f;     // full-density strength (RenderVKRTReflectionsStrengthFull)
    float            mRtReflStrMin = 0.03f;      // rho_min strength (RenderVKRTReflectionsStrengthMin)
    float            mRtReflStrCut = 0.02f;      // no ray below (RenderVKRTReflectionsStrengthCut)
    float            mRtReflCurvRad = 0.f;       // CURVATURE DENSITY limit, rad (RenderVKRTReflectionsCurvatureDensity; 0 off)
    bool             mRtReflLobe = true;         // LOBE FILTER (RenderVKRTReflectionsLobeFilter)
    bool             mRtReflTemporal = true;     // TEMPORAL history (RenderVKRTReflectionsTemporal; mDsRtHist)
    bool             mRtReflDebugView = false;   // RenderVKDebugView 28: the ray density heat map (rtt_param3.x bit 1)
    uint32_t         mRtReflHitShadows = 1u;     // projector shadow rays per hit (RenderVKRTReflectionsHitShadowRays, 0..4)
    uint32_t         mRtReflHitShadowsLogged = ~0u;   // last logged value (the budget line on change)
    // RT TILE LIST header (clean_deferred_light.comp RtReflTiles): args[4], rays[3], cand, cast,
    // level[2], hit[5] (pass 11's hit-shading counters), param uvec4 (16-19), param2 uvec4 (20-23,
    // the density gradient's), scale[2] (24-25, GPU-owned), grays (26), rhosum (27), cost[24]
    // (28-51, pass 11's trace-cost counters), param3 uvec4 (52-55, the strength density's),
    // scand[3] (56-58, candidates by type), ssum[3] (59-61, their strength sums x 256), scut (62,
    // pixels the strength cutoff took out), amresc (63), tiles (64, listed this frame), ovf (65, ray
    // list overflow), curv (66, host: the curvature density limit), opts4 (67, host: bit 0 the lobe
    // filter) -> 68 words; the tile flags / list / count / ray list follow.
    static constexpr uint32_t kRtTilesHdrWords = 68u;
    static constexpr uint32_t kRtCostWords = 24u;   // (clean_deferred_light.comp RTR_COST_WORDS)
    static constexpr uint32_t kRtTilesHdrBytes = kRtTilesHdrWords * 4u;
    // Ray density stats (GPU timers on): the header copied into a 3-slot host ring after the
    // resolve, harvested 3 frames later (frames in flight <= 2); one VKGpu line per timer log.
    GpuBuffer        mRtrStatRb{};
    bool             mRtrStatUsed[3] = {false, false, false};
    uint32_t         mRtrStatFrame = 0u;
    struct RtrStatAcc { uint32_t frames = 0; double cand = 0, cast = 0, rays[3] = {0, 0, 0};
                        uint32_t level[3] = {0, 0, 0}; double budget = 0;
                        double hit[5] = {0, 0, 0, 0, 0};    // (rtt_hit: see rtReflStatsText)
                        double cost[kRtCostWords] = {};     // (rtt_cost: the trace-cost counters)
                        double amresc = 0;                  // (rtt_amresc, header word 63: avatar mask hits the coverage rule kept)
                        double tiles = 0, ovf = 0;          // (header words 64 / 65: tiles listed, ray list overflow)
                        double grays = 0, rhosum = 0, scale = 0, gcand = 0;   // (the density gradient's header words)
                        double scand[3] = {0, 0, 0}, ssum[3] = {0, 0, 0}, scut = 0;   // (the strength density's)
                        uint32_t costFrames = 0, gradFrames = 0; } mRtrStatAcc;
    void  rtReflStatsRecord(VkCommandBuffer cmd);   // harvest this frame's slot, copy the header into it
    std::string rtReflStatsText();                 // "RT reflections over N frames: ..." and resets
    double           mRtReflRetryAt = 0.0;       // targets refused (#697): next attempt
    double           mRtReflLastWant = -1e9;     // last frame that wanted RT reflections (the targets' hold)
    static constexpr double kRtReflHoldSecs = 10.0;   // targets kept after the last wanting frame
    bool  rtReflWanted() const;                  // the RT build gate (rtConsumerActive): setting + deferred + no stand-down
    void  rtReflSettings();                      // this frame's RT reflection settings + stand-down log
    void  recordRtReflect(VkCommandBuffer cmd, const VKDeferPush& p, uint32_t viewOff, bool onAsync);
    // ---- #85 phase 2: the DIFFUSE / SPECULAR DENOISER (llcleandenoisevk.h) and its first
    // producer, the PROJECTOR STOCHASTIC SAMPLER (RenderVKProjStochastic; llcleanprojstochvk.cpp,
    // clean_deferred_light.comp pass 15). Split frames with ray-traced projector records only:
    // pass 15 (RT layout) -> the denoiser's temporal + a-trous -> the combine / LOCAL remodulate
    // (DEFER_FLAG_PSTOCH; the walk leaves the RT projectors' shadowed direct light out, the RT
    // mask leaves their slots empty: rtMaskPushFill's negative clamp width).
    LLCleanDenoiserVK* mPdn = nullptr;                 // initDeferSplit (always: set 3's placeholders)
    VkPipeline       mPdnSamplePipe = VK_NULL_HANDLE;  // pass 15 (mRtReflLayout; RT only)
    bool             mPdnNow = false;                  // this frame samples + denoises (pdnFrame)
    bool             mPdnWant = false;                 // the setting asks for it (ensureTemporal: motion vectors)
    uint32_t         mPdnFlags = 0u;                   // this frame's DEFER_FLAG_PSTOCH* bits
    uint32_t         mPdnK = 1u;                       // samples per texel (RenderVKProjStochasticSamples)
    uint32_t         mPdnDebug = 0u;                   // RenderVKDenoiseDebug
    uint32_t         mPdnSplitK = 0u;                  // (#869) split lights per texel (RenderVKProjStochasticSplitK)
    uint32_t         mPdnAtrous = 3u;                  // RenderVKDenoiseAtrous (clamped 1..5)
    uint32_t         mPdnFrame = 0u;                   // the sampler's noise phase
    double           mPdnLastWant = -1e9;              // the images' hold (kRtReflHoldSecs)
    double           mPdnRetryAt = 0.0;                // refused (#697): next attempt
    void  pdnInit(VkDevice dev);                       // initDeferSplit (+ pass 15 with RT)
    void  pdnDestroy();
    // deferSplitFrame (split frames): settings, images, mPdnNow. sample = false: the images only
    // (a forward frame warming the merged RT shadow pass up; never sets mPdnNow).
    void  pdnFrame(uint32_t rw, uint32_t rh, bool sample = true);
    void  pdnIdle();                                   // deferSplitFrame, non-split frames: release after the hold
    // DEFERRED (RE)ALLOCATION (Beta 5 Windows NVIDIA DEVICE_LOST, 2026-10-22): pdnFrame / pdnIdle run
    // inside the frame's recording and only REQUEST a change; pdnPrestep (syncFromStoresPrestep,
    // before the next frame's command buffer begins) waits for the device, re-creates / releases
    // the images and rewrites the denoiser's set + set 3 b13-16. RenderVKResizeAtFrameStart 0 = the
    // old mid-recording path (A/B only); RenderVKDenoiseReallocStress N = flip the sun channel every
    // N s (a re-allocation on demand, for soak tests).
    struct PdnReq { bool pending = false; bool release = false; uint32_t w = 0, h = 0; bool sun = false; bool sunWanted = false;
                    bool split = false; };
    PdnReq           mPdnReq;
    int64_t          mPdnStressPhase = -1;             // RenderVKDenoiseReallocStress: the current flip phase
    bool  resizeAtFrameStart() const;                  // RenderVKResizeAtFrameStart (default on)
    void  pdnRequestEnsure(uint32_t w, uint32_t h, bool sun, bool sunWanted, bool split);
    void  pdnRequestRelease();
    void  pdnPrestep();                                // syncFromStoresPrestep: apply mPdnReq
    // ---- MERGED RT SHADOWS (#195 + #85, RenderVKRTShadowMerge; llcleanprojstochvk.cpp) ----
    // When the sun / moon AND the projectors are ray traced with the stochastic path on (split
    // deferred frames), pass 15's merge variant (mPdnMergePipe, CLEAN_SHADOW_MERGE) also traces
    // the sun / moon (its own ray, never the projector pick) into the denoiser's sun channel,
    // recorded BEFORE the SHADOW kernel, which reads the denoised sun instead of the RT shadow
    // mask: one trace pass, one denoiser, the lighting stays deferred, the mask does not run (its
    // c_rtmask* timers vanish). Every other combination keeps its path exactly (shadowPathLog).
    // PREDICT + SETTLE: selectLitVariant predicts the merge (mShadowMergeTry: the settings, this
    // frame's sun / projector state, the denoiser's sun images exist) and holds the mask off (frame
    // info rt_mask.x = -1, the lit slots keep the ray-query variant); after deferSplitFrame
    // recordWorld's settle confirms it (mShadowMergeNow: the split + the denoiser run) or puts the
    // frame back on today's path -- the mask re-armed, or the forward ray-query pass without one.
    VkPipeline mPdnMergePipe = VK_NULL_HANDLE;   // pass 15 + the sun / moon trace (mRtReflLayout; RT only)
    // #85 / #869 Phase 1 (RenderVKShadowGenericCore): the same two kernels on the GENERIC SHADOW
    // CORE (clean_shadow_core.glsl: one receiver setup, one light-sample loop, one shadow walk;
    // the projectors get the mask's terminator / rig / slope receiver bias). Picked per frame by
    // recordProjStoch while compiled; the per-light-type pipes above stay the fallback.
    VkPipeline mPdnSampleCorePipe = VK_NULL_HANDLE;
    VkPipeline mPdnMergeCorePipe = VK_NULL_HANDLE;
    // (#1031, RenderVKShadowSplitKernels) the generic core as SELECT (plain / merged), SUNVIS (merged),
    // VIS and SHADE (shared) -- clean_deferred_light.comp THE SHADOW CORE SPLIT -- over mPdnSel (set 3
    // binding 26: 64 B per denoiser texel) and mPdnVis (binding 27: 16 B + mPdnVisSlots x 8 B per denoiser
    // texel: VIS's job list + the visibilities), both at the denoiser's extent, allocated / freed like mRsRes
    VkPipeline mPdnSelectPipe[2] = {};
    VkPipeline mPdnSunVisPipe = VK_NULL_HANDLE, mPdnVisPipe = VK_NULL_HANDLE, mPdnShadePipe = VK_NULL_HANDLE;
    // (RenderVKShadowIsoLod) SUNVIS / VIS with the isotropic alpha-test LOD (the defaults; the above: the
    // receiver-footprint LOD, setting 0 -- and the fallback while these compile)
    VkPipeline mPdnSunVisIsoPipe = VK_NULL_HANDLE, mPdnVisIsoPipe = VK_NULL_HANDLE;
    GpuBuffer  mPdnSel{}, mPdnVis{};
    uint32_t   mPdnSelW = 0, mPdnSelH = 0, mPdnVisSlots = 0;
    double     mPdnSelRetryAt = 0.0;   // a refused allocation (#697): the fused core meanwhile
    void       pdnSelFrame();          // a sampling frame: ensure / free (the rsResFrame rule)
    void       writePdnSelBinding();   // set 3 bindings 26 + 27 (no frame in flight)
    bool       pdnSplitWanted() const; // RenderVKShadowSplitKernels + the five pipelines
    uint32_t   pdnVisSlotsWanted() const;   // the settings' longest light-sample list (mPdnVis's slots)
    void       pdnSelFree();           // (the device waited) both buffers, bindings to the placeholder
    bool   mShadowMergeTry = false;      // predicted this frame (selectLitVariant)
    bool   mShadowMergeNow = false;      // confirmed this frame (recordWorld, after deferSplitFrame)
    bool   mRtMaskWant = false;          // selectLitVariant's RT mask verdict before the merge held it off
    float  mShadowMergeSunW = 1.f;       // weak-sun weight: 0 = no sun ray (RenderVKRTShadowMergeSunMin)
    float  mShadowMergeSunLum = 0.f;     // ... its measure: the light colour's luminance x the sun scale
    int    mShadowMergeSunBand = -1;     // logged band: 0 no ray, 1 fading, 2 full (calibration log)
    float  mShadowMergeSunMove = 0.f;    // the light's step since the last merged frame (disc radii)
    bool   mShadowMergeSunCut = false;   // restart the sun history (sun <-> moon switch, a jump)
    float  mShadowMergePrevDir[3] = {0.f, 0.f, 0.f};
    float  mShadowMergePrevSunUp = -1.f;
    bool   mShadowMergePrevOk = false;
    double mPdnSunLastWant = -1e9;       // the denoiser's sun channel hold (kRtReflHoldSecs)
    double mPdnSunRetryAt = 0.0;         // the sun channel refused (#697): next attempt (projector-only meanwhile)
    uint32_t mShadowPathLogged = 0xFFFFFFFFu;   // shadowPathLog's last state
    bool  shadowMergeWanted(bool needImages) const;  // settings + this frame's sun / projector / RT state
    void  shadowMergeSunState();                     // the weak-sun weight + the light's step (once per frame)
    void  rtMaskFrameInfo(VkCommandBuffer cmd, bool again);   // frame info rt_mask + shadow_params patch
    void  installLitSlots(int v, bool plainOpaque);  // the lit FS slots (+ the plain RT-mask slots, debug twins)
    int   mLitSlotsV = 0;                            // selectLitVariant's variant (installLitSlots on a revert)
    void  shadowPathLog();                           // "shadow paths: sun = ..., projectors = ..." on change
    // recordDeferredLighting, merged frames: recordProjStoch (merge variant) before the SHADOW
    // kernel + the barrier that publishes the denoised sun to it.
    void  recordShadowMergeFirst(VkCommandBuffer cmd, const VKDeferPush& p, uint32_t viewOff, bool onAsync);
    void  writePdnBindings();                          // set 3 bindings 13-16 (callers: no frame in flight)
    void  writePdnMotion();                            // the denoiser's motion-vector binding (as the mask's)
    bool  pdnMaskSkipsProjectors() const { return mPdnNow; }
    void  recordProjStoch(VkCommandBuffer cmd, const VKDeferPush& p, uint32_t viewOff, bool onAsync);
    void  recordRtMarkInit(VkCommandBuffer cmd);
    void  recordRtPreTrace(VkCommandBuffer cmd, const VKDeferPush& p, uint32_t viewOff);
    void  ensureRtPlaceholders();                // init: the 1 x 1 zero targets + their bindings
    void  writeRtFwdBinding();                   // draw set 0 b45: mDsRtFwd at full size, else the placeholder
    bool  ensureRtUber(uint32_t rw, uint32_t rh, bool full);
    VKRtReflPush rtReflPush(const VKDeferPush& p) const;
    void  initDeferSplit();                      // init, inside the world PipelineBatchScope
    bool  ensureDeferSplitTargets(uint32_t rw, uint32_t rh, bool projFull, bool projRgb, bool reflHalf, bool visReuse,
                                  bool feat, bool rtMark);
    // The current intermediates already are what ensureDeferSplitTargets would make (no re-creation).
    bool  deferSplitTargetsMatch(uint32_t rw, uint32_t rh, bool projFull, bool projRgb, bool reflHalf, bool visReuse,
                                 bool feat, bool rtMark) const;
    // DEFERRED (RE)ALLOCATION (2026-10-22, the pdnPrestep rule): deferSplitFrame runs inside the
    // recording, after draw set 0 was bound (volumetric clouds, weather passes), and a
    // re-creation rewrites set 0 b41-43 / b48 + set 3 -- so it only records the request (that frame
    // lights with the uber kernel, as on a refused allocation) and deferSplitPrestep applies it
    // before the next recording begins. Freeing them (split off / a stale alias block) likewise.
    struct DsReq { bool pending = false; uint32_t rw = 0, rh = 0; bool projFull = false, projRgb = false, reflHalf = false,
                   visReuse = false, feat = false, rtMark = false; bool free = false, unused = false; };
    DsReq            mDsReq;
    void  deferSplitPrestep();                   // syncFromStoresPrestep: apply mDsReq
    bool  deferSplitApply(uint32_t rw, uint32_t rh, bool projFull, bool projRgb, bool reflHalf, bool visReuse,
                          bool feat, bool rtMark);   // ensureDeferSplitTargets + the #697 failure backoff
    void  destroyDeferSplitTargets();
    void  destroyDeferSplitProj();
    void  destroyDeferSplit();                   // shutdown
    // Once per on-screen recording (deferred = this frame lights deferred, no debug twin):
    // the intermediates (allocation retry, free when switched off), mDeferSplitNow / Flags.
    void  deferSplitFrame(bool deferred, uint32_t rw, uint32_t rh);
    // (#824) RenderVKDeferredSplit resolved: 0 uber, 1 split. The setting's 2 (AUTO, the
    // default) picks per GPU class once per session (deferSplitAuto); 0 / 1 are the user's.
    uint32_t deferSplitMode() const;
    mutable int mDeferSplitAuto = -1;            // AUTO's decision (-1: not made yet)
    VkPipelineLayout mDrawLayoutRt = VK_NULL_HANDLE;     // sets 0/1 + RT consumer (ray-query variant)
    bool             mLitRt = false;                     // this frame's variant (true = ray-query)
    int              mLitVariantLogged = -1;             // last logged variant (-1 none)
    // Decide this frame's variant (after the RT build) and install its handles.
    void selectLitVariant();
    // True when this frame has a TLAS consumer (projector shadows mode 2 with records,
    // or the RT debug views 13/14). Decided before LLCleanRTVK::recordBuild.
    bool rtConsumerActive() const;

    // ---- SETTING SPECIALIZATION (RenderVKSpecPipelines; llcleanspecvk.cpp) ----
    // The heavy lit shaders read ten graphics settings through specialization constants 8-17
    // (+ #818: the environment's classic-sky mode, 18, and the AO upsample mode, 19)
    // (clean_frame_info.glsl; kSpecTable in llcleanspecvk.cpp is the one table of setting ->
    // constant id -> value). A SPEC SET is every pipeline of those shaders built for one
    // CONFIGURATION KEY (the tuple of the settings' spec values): the lit FS slots of both
    // variants (from mLitDbgDesc; the ray-query ones only when the key can trace), the
    // deferred uber kernel and split passes, the water / capture water FS, the per-pixel
    // particle FS of the pairs in use and the particle lighting compute. The GENERIC
    // pipelines (built at init, owned where they always were, nothing specialized) stay
    // available: every bind site asks an accessor (litVar, deferPipe, deferSplitPipe,
    // waterPipe, waterCapPipe, partLightPipe, partPipe) that returns the bound set's handle
    // when it has one. mSpecLive's key is what the frame info carries (specApply: EXACT
    // fields take the key's value, CAP fields are clamped to it), so the folded code and the
    // fields always agree; with no set bound (generic) the live settings apply. Flow
    // (specFrameTick, once per frame before any frame state): a changed key whose set is
    // resident binds at once; otherwise the OLD key keeps rendering (old values) while the
    // new set compiles in the background (one build at a time: per-key VkPipelineCache
    // seeded from disk + modules on a worker, then an async pipeline batch + a compute
    // worker), and after RenderVKSpecApplyTimeoutSec the new values apply on the generic
    // pipelines until it lands. Sets are LRU'd within RenderVKSpecCacheMB (resident sets
    // and the per-key cache files vk_spec_<key>.bin next to vk_pipeline_cache.bin, each
    // budgeted on its own; the bound / wanted / building keys are never evicted); evicted
    // pipelines go through the frame retire lists. Idle prewarm: the previous key (flip
    // back), then one-flip neighbours. Cluster LOD / G-buffer / HUD / mirror alpha pipelines
    // stay generic (correct: they read the clamped fields).
public:   // (the file-local table in llcleanspecvk.cpp is indexed by it)
    // (#818) SPEC_SKY (constant 18): the environment's classic (legacy, non-auto-adjusted) vs
    // linear sky -- NOT a setting: it follows the current sky (specKeyFromSettings), 2 = run time
    // (RenderVKSpecClassicSky off). SPEC_AOUP (constant 19): the AO upsample mode (ao_up.x).
    enum SpecId : uint32_t { SPEC_FILTER, SPEC_SUN, SPEC_PROJ, SPEC_TRANS, SPEC_SSR, SPEC_MIRROR,
                             SPEC_CEL, SPEC_TOPN, SPEC_AO, SPEC_AOMODE, SPEC_SKY, SPEC_AOUP, SPEC_COUNT };
private:
    static constexpr uint64_t kSpecNone = ~0ull;     // no key (generic / nothing wanted)
    struct SpecSet;                                   // llcleanspecvk.cpp
    struct SpecSetDeleter { void operator()(SpecSet* s) const; };
    std::vector<std::unique_ptr<SpecSet, SpecSetDeleter>> mSpecSets;   // resident + building
    SpecSet*  mSpecLive = nullptr;        // bound (nullptr = the generic pipelines)
    SpecSet*  mSpecBuild = nullptr;       // the set compiling for the login key / a setting change
    SpecSet*  mSpecPrewarmBuild = nullptr; // the speculative set compiling (its own lane, low priority)
    double    mSpecLastPrewarmEnd = -1e9; // a prewarm build finished (neighbour spacing)
    // Prewarm's BUSY signals (specPrewarmBusy): frame time, smoothed, and the last slow frame.
    double    mSpecTickLast = 0.0, mSpecFrameEma = 0.0, mSpecSlowAt = -1e9;
    uint64_t  mSpecWanted = kSpecNone;    // the settings' key (last tick)
    uint64_t  mSpecPrev = kSpecNone;      // the key before the last change (prewarm: flip back)
    double    mSpecPendSince = -1.0;      // the wanted key is not bound yet since (-1: none)
    double    mSpecIdleSince = -1.0;      // nothing pending / building since
    bool      mSpecFallback = false;      // the wanted key's values apply on the generic pipelines
    bool      mSpecInit = false;          // disk index loaded
    bool      mSpecPrewarmDone = false;   // every prewarm candidate tried for this wanted key
    uint64_t  mSpecPeakBytes = 0, mSpecDiskPeak = 0;
    double    mSpecLogNext = 0.0;
    struct SpecDiskEntry { int64_t used = 0; uint64_t bytes = 0; };
    std::map<uint64_t, SpecDiskEntry> mSpecDisk;      // per-key cache files (vk_spec_index.txt)
    bool      mSpecDiskDirty = false;
    // The value an in-scope field / decision takes this frame: `live` (what the settings and
    // the frame give) with no set bound; else the bound key's value (EXACT) or `live` clamped
    // to it (CAP). Every renderer read of an in-scope setting goes through it.
    uint32_t  specApply(SpecId id, uint32_t live) const;
    void      specShutdown();                          // device idle: joins builds, frees sets
    uint64_t  specKeyFromSettings() const;
    bool      specStartBuild(uint64_t key, const char* why);
    void      specPollBuild();
    void      specAdvanceBuild(SpecSet*& lane);
    bool      specPrewarmBusy(double now);
    void      specEvict(bool force_disk);
    void      specSaveIndex();
    void      specLog(const char* what, const SpecSet* s) const;
    std::string specKeyText(uint64_t key) const;
    std::string specCachePath(uint64_t key) const;
    // Bind-site accessors: the bound set's pipeline, else the generic one.
    VkPipeline litVar(int v, int i) const;
    VkPipeline deferPipe() const;
    VkPipeline deferSplitPipe(uint32_t i) const;
    VkPipeline deferFeatPipe(uint32_t i) const;   // DeferFeatPipe (#827)
    VkPipeline waterPipe() const;
    VkPipeline waterCapPipe() const;
    VkPipeline partLightPipe(bool rt) const;
    VkPipeline specPartPipe(uint32_t variant, uint32_t pair) const;   // VK_NULL_HANDLE: none in the set
public:
    // Once per rendered frame, before any frame state (LLVulkanContext, before setLights):
    // harvests builds, binds / falls back / prewarms / evicts (see above).
    void      specFrameTick();
    // Graphics preferences (llfloaterpreferencesgraphicsadvanced.cpp): "" when every setting
    // is applied with its specialized pipelines; else what is pending ("Applying shadow
    // filter, SSR...") and whether the new values already apply on the generic pipelines
    // (fallback). Any thread (a snapshot the render thread updates in specFrameTick).
    static std::string specStatusText(bool* fallback = nullptr);
private:
    std::vector<uint32_t> mProjRender;          // slots to (re)render this frame
    // #564 dynamic-tile budget (invalidateProjShadows): engaged only under measured load.
    bool        mProjDynThrottle = false;
    double      mProjDynSlowSince = 0.0, mProjDynFastSince = 0.0;
    uint32_t    mProjFrame = 0;
    uint32_t    mProjActive = 0;                // slots in use this frame
    VkPipeline  mProjPipe = VK_NULL_HANDLE, mProjMaskPipe = VK_NULL_HANDLE;
    VkPipeline  mProjRigPipe = VK_NULL_HANDLE, mProjRigMaskPipe = VK_NULL_HANDLE;
    VkPipeline  mProjDsPipe = VK_NULL_HANDLE, mProjMaskDsPipe = VK_NULL_HANDLE;
    VkPipeline  mProjRigDsPipe = VK_NULL_HANDLE, mProjRigMaskDsPipe = VK_NULL_HANDLE;
    bool initProjShadows();
    // #600 lazy projector shadow atlas (see initProjShadows).
    bool allocProjAtlas();                       // atlas + cache + translucency at the current budget; soft
    void freeProjAtlas(const char* why);         // device idle; stand-ins re-bound
    void bindProjAtlasDummies();                 // draw set 0 bindings 18 / 29 / 30 -> 1x1 stand-ins
    // #719: draw set 0 bindings 45 / 46 -> the volumetric clouds' shadow map / panorama, or (any
    // null) the 1x1 white stand-in. Device idle.
    void bindVolCloudImages(VkSampler sampler, VkImageView shadow, VkImageView pano);
    void volCloudsTick();                        // #719: create / free the module (prestep, device idle on toggles)
    double mVolCloudsRetryAt = 0.0;              // #719: a refused allocation's retry time (steady clock s)
    void projAtlasTick();                        // prestep: allocate when wanted, free after the grace period
    uint64_t projAtlasHeldBytes() const;
    static constexpr double kProjAtlasGraceSecs = 45.0;
    GpuImage mProjDummyDepth{}, mProjDummyColor{};   // 1x1: depth 1.0 (lit), white (no tint)
    double   mProjAtlasWantedAt = 0.0;           // steady s: last frame shadow-mapped projectors wanted it
    double   mProjAtlasRetryAt = 0.0;            // steady s: no allocation attempt before this
    // #600 sun cascades: while the sun is ray traced, the cascades shrink to a small fallback
    // set after kProjAtlasGraceSecs without shadow-map use (full size again the frame after).
    double   mSunMapsUsedAt = 0.0;               // steady s: last frame the sun used the cascades
    bool     sunCascadesIdle() const;
    void selectProjShadows(const LLVKWorldFrame& wf);   // setLights: pick + assign slots, tex.z
    // RenderVKRTDebugFlicker: focus projector + record-assignment log (rec_light[k] =
    // light list index of RT record k).
    void debugProjRT(const LLVKWorldFrame& wf, const std::vector<uint32_t>& rec_light);
    void invalidateProjShadows();               // recordWorld: dynamic / changed tests
    void recordProjShadows(VkCommandBuffer cmd, const VKCullPush& cullBase,
                           const VKDrawPush& drawPush, bool do_rigged);
    // Avatar boxes (padded) from computeAvatarVis, for projector-tile invalidation.
    std::vector<std::array<float, 6>> mAvBoxes;
    // ---- Planar MIRROR (LL hero reflection probe) ----
    // The nearest visible mirror probe gets a capture of the world from the view
    // reflected about its plane: static geometry culled in mode 4 into its OWN buckets
    // (like the shadow cascades), avatars drawn from the main cull's pre-skinned
    // buckets (their visibility includes the mirror frustum), sky + static alpha +
    // rigged alpha, into an HDR target (mirror-res) that the main view samples at the
    // same screen pixel inside the probe box (clean_draw_common tapMirror).
    bool        mMirrorOn = false;          // a mirror is captured + sampled this frame
    bool        mMirrorStatic = false;      // view + mirror unchanged since last frame
    bool        mMirrorHaveImage = false;   // mMirrorColor holds a real capture
    float       mMirrorVP[16] = {}, mMirrorVPSky[16] = {}, mMirrorPlanes[6][4] = {};
    float       mMirrorVPRel[16] = {};   // mMirrorVP relative to mMirrorCam (#555)
    float       mMirrorCam[4] = {}, mMirrorPlane[4] = {}, mMirrorBox[16] = {};
    float       mMirrorScale = 1.f;
    uint32_t    mMirrorMaxRes = 1024;     // RenderHeroProbeResolution (capture long-side cap)
    bool        mMirrorDynamic = false;   // the mirror's Dynamic flag: avatars drawn
    uint32_t    mMirrorW = 0, mMirrorH = 0, mMainW = 0, mMainH = 0;
    uint32_t    mMirrorFrame = 0;
    GpuImage    mMirrorColor{}, mMirrorDepth{};
    GpuBuffer   mMirrorCmd[5]{};            // opaque / mask / alpha / ds opaque / ds mask
    GpuBuffer   mMirrorCounters{};
    VkDescriptorSet mDsetCullMirror = VK_NULL_HANDLE;
    VkPipeline  mMirrorSkyPipe = VK_NULL_HANDLE, mMirrorAlphaPipe = VK_NULL_HANDLE;
    VkPipeline  mMirrorRigPipe = VK_NULL_HANDLE, mMirrorRigDsPipe = VK_NULL_HANDLE;
    VkPipeline  mMirrorRigAlphaPipe = VK_NULL_HANDLE;
    // OCCLUSION GATE (RenderVKMirrorOcclusionQuery). At the end of pass 1 (scene opaque
    // depth complete) the selected mirror's probe box -- inflated a few cm so the
    // mirror prim's own coincident face cannot z-fight it -- is drawn with depth LEQUAL,
    // no depth / colour writes, no culling, inside a VK_QUERY_TYPE_OCCLUSION query.
    // One query per frame-in-flight slot; setMirror reads the slot's result after
    // acquire() waited that slot's fence (no WAIT bit, no stall => framesInFlight
    // frames of lag). A result is applied only to the mirror it was recorded for
    // (mMirrorKey); a new mirror starts "unknown" (captured). While the last result
    // for the current mirror is 0 samples and the eye is not inside the box, the
    // capture is skipped (the main view samples the stale capture on its zero pixels).
    static constexpr uint32_t MIRROR_OCC_SLOTS = 4;
    struct MirrorOccRec { bool pending = false; uint64_t key = 0; uint32_t serial = 0; };
    MirrorOccRec mMirrorOccRec[MIRROR_OCC_SLOTS];
    VkQueryPool      mMirrorOccPool = VK_NULL_HANDLE;
    VkPipelineLayout mMirrorOccLayout = VK_NULL_HANDLE;
    VkPipeline       mMirrorOccPipe = VK_NULL_HANDLE;
    bool        mMirrorOccOn = false;       // record the query this frame
    bool        mMirrorSkipOcc = false;     // capture skipped this frame (occluded)
    // #772 RenderVKMirrorIdleSkip: the view is static (wf.mirror_static) and nothing that the
    // capture shows changed since the last capture -> no capture this frame (prbGatherChanges).
    bool        mMirrorViewStatic = false;
    bool        mMirrorIdleSkip = false;
    double      mMirrorLastCap = -1.0e9;    // last capture (s)
    uint64_t    mMirrorCapKey = 0, mMirrorCapLightSig = 0, mMirrorLightSig = 0;
    uint64_t    mMirrorKey = 0;             // selected mirror identity (0 = none)
    int         mMirrorOccState = -1;       // -1 unknown, 0 occluded, 1 visible
    uint64_t    mMirrorOccSamples = 0;      // last applied result (samples passed)
    uint32_t    mMirrorOccSerial = 0;       // per-recorded-query serial
    uint32_t    mMirrorOccApplied = 0;      // serial of the last applied result
    int         mMirrorLogState = -2;       // last logged visibility state
    float       mMirrorOccModel[16] = {};   // inflated box model (local [-1,1]^3 -> world)
    bool        mMirrorCamInside = false;
    float       mMirrorDist = 0.f, mMirrorSize = 0.f, mMirrorPx = 0.f;
    uint32_t    mMirrorLocalId = 0;
    uint8_t     mMirrorUuid[16] = {};
    void readMirrorOcclusion();
    void logMirror(const char* what);
    void recordMirrorOcclusion(VkCommandBuffer cmd, const float* view_proj);
    bool ensureMirror(uint32_t w, uint32_t h, bool rt = false);   // #697: false = refused (old pair kept); rt: #860 target
    double mMirrorRetryAt = 0.0;                  // #697: refused mirror targets back off until then
    void writeMirrorDescriptor();
    void recordMirror(VkCommandBuffer cmd, const VKCullPush& cullBase,
                      const VKDrawPush& drawPush, const VKSkyPush& skyPush, bool do_rigged);
    // ---- RT MIRRORS (#860, RenderVKRTMirrors) ----
    // On ray-tracing hardware the selected mirror's image (mMirrorColor, draw set 0 binding 17) is
    // RAY TRACED instead of rendered from the reflected view: clean_deferred_light.comp pass 12
    // -DCLEAN_RT_MIRROR (mRtMirrorPipe, mRtUberLayout's sets + set 4 = the image) at the start of
    // recordDeferredLighting, one perfect-specular ray per mirror pixel at the render extent. Every
    // tapMirror consumer is unchanged (frame info mirror_params.w = 1: sampled at the jittered pixel,
    // no shift). The selection, the probe box / plane, the occlusion gate and the avatar visibility
    // (kAvVisMirror: avatars seen only in the mirror stay pre-skinned and in the TLAS) are the planar
    // path's; only the capture is replaced (recordMirror returns at once; no mirror cull, rig cull,
    // particle cull or scene draws). The planar capture stays for non-RT cards, the setting's A/B and
    // the RT stand-down (VRAM ladder / RT budget) -- a MODE: switching reallocates the target.
    //   mMirrorRtMode   this recording's target is the RT image (STORAGE | SAMPLED, GENERAL, render
    //                   extent, no depth) -- decided before ensureMirror (rtMirrorModeWanted)
    //   mRtMirrorNow    this frame traces it (mode + the TLAS valid / warm + deferred lighting)
    bool             mMirrorRtMode = false;
    bool             mMirrorTargetRt = false;    // mMirrorColor was allocated for the RT mode
    bool             mRtMirrorNow = false;
    bool             mRtMirrorHaveImage = false; // the RT image holds a traced frame since its allocation
    int              mRtMirrorModeLogged = -1;   // last logged mode (log once per change)
    float            mRtMirrorMaxDist = 512.f;   // RenderVKRTMirrorsMaxDist (push rtr.x)
    uint32_t         mRtMirrorHitShadows = 4u;   // RenderVKRTMirrorsHitShadowRays (rtrOpts bits 1-3)
    VkPipeline       mRtMirrorPipe = VK_NULL_HANDLE;    // pass 12 -DCLEAN_RT_MIRROR (mRtMirrorLayout)
    VkPipelineLayout mRtMirrorLayout = VK_NULL_HANDLE;  // mRtUberLayout's 4 sets + set 4, 112 B push
    VkDescriptorSetLayout mDslRtMirror = VK_NULL_HANDLE; // binding 0: the mirror image (storage)
    VkDescPool       mRtMirrorPool;
    VkDescriptorSet  mDsetRtMirror = VK_NULL_HANDLE;
    bool rtMirrorModeWanted();               // the setting + RT up + deferred lighting (sticky per state)
    bool rtMirrorFrameReady() const;         // this frame can trace (TLAS valid + warm, consumer set)
    void writeRtMirrorDescriptor();
    void recordRtMirror(VkCommandBuffer cmd, const VKDeferPush& p, uint32_t viewOff);
    // ---- GPU particles (port of the GL clean particle system) ----
    // LLCleanParticleStore (CPU-only under VK) feeds one emitter descriptor per live
    // source each frame; the GPU owns emission, simulation, compaction into per-blend-
    // pair draw segments, and the billboard/ribbon draw (pass 2b, after post-water alpha).
    // Every SL blend pair (8 x 8 PSYS_PART_BF_* factors) is its own pipeline: the six
    // common ones built at init, any other on a worker thread the first time an emitter
    // uses it (drawn with the alpha pipeline until it is ready).
    static constexpr uint32_t NUM_PART_PAIRS = 64;   // = LLCleanParticleStore::NUM_BLEND_PAIRS
    // Draw segments (indirect cmds): [0, 64) the pairs on the far side of the water plane
    // (drawn with the pre-water alpha), [64, 128) the near side (post-water).
    static constexpr uint32_t NUM_PART_SEGS  = 2 * NUM_PART_PAIRS;
    // Per-pass culled lists (#518; clean_particle_compute_common.glsl): indirect commands
    // [0, SEGS) main, [SEGS, 2 SEGS) mirror, then one per cascade; the count buffer the
    // same, then PART_STATS words (live, main-visible, mirror-only, shadow-only).
    static constexpr uint32_t PART_CMD_MIRROR = NUM_PART_SEGS;
    static constexpr uint32_t PART_CMD_SHADOW = 2 * NUM_PART_SEGS;
    static constexpr uint32_t PART_CMDS       = PART_CMD_SHADOW + 4;
    static constexpr uint32_t PART_STATS      = 4;
    static constexpr uint32_t PART_COUNT_WORDS = PART_CMDS + PART_STATS;
    // Draw-list entries per pool slot: main + mirror (sorted) + 4 cascades (unsorted).
    static constexpr uint32_t PART_LISTS      = 6;
    static constexpr uint32_t PART_PAIR_ALPHA = 6 * 8 + 7;       // SOURCE_ALPHA, ONE_MINUS_SOURCE_ALPHA
    // MIRROR: the planar mirror capture (HDR colour only, no glow, CONVENTIONAL depth --
    // the main-view variants are reversed-Z).
    // LIT: the main view with PER-PARTICLE LIGHTING (RenderVKParticleLighting 1): the VS
    // forwards clean_particle_light.comp's record, the FS only applies it (no tracing, so
    // no ray-query twin); dynamic shading rate (RenderVKParticleShadingRate). LOWRES: the
    // same shaders into the reduced-resolution accumulator (RenderVKParticleLowRes): the
    // pair's colour factors, alpha channel = the destination's transmittance
    // (partAccum), only for partLowResPair pairs. MAIN / MAIN_RT: the per-pixel lit
    // path (RenderVKParticleLighting 0, or no lighting record this frame).
    enum PartPipeVariant : uint32_t { PART_PIPE_MAIN = 0, PART_PIPE_MAIN_RT = 1, PART_PIPE_MIRROR = 2,
                                      PART_PIPE_LIT = 3, PART_PIPE_LOWRES = 4, PART_PIPE_VARIANTS = 5 };
    enum PartDrawPass : uint32_t { PART_DRAW_PRE_WATER, PART_DRAW_POST_WATER, PART_DRAW_MIRROR };
    // Which pairs a main-view particle draw records: all, only those the reduced-resolution
    // pass takes (into its targets, PART_PIPE_LOWRES), or only the rest (full resolution).
    // LOWRES_SMALL (#879 size routing): the reduced-resolution pairs' SMALL sprites (below
    // RenderVKParticleSmallPx), at full resolution and full shading rate (PART_PIPE_LIT),
    // drawn before the composite; PART_SET_LOWRES then takes only the large ones.
    enum PartDrawSet : uint32_t { PART_SET_ALL, PART_SET_LOWRES, PART_SET_FULLRES, PART_SET_LOWRES_SMALL };
    static bool partLowResPair(uint32_t pair);   // = clean_particle_common.glsl partLowResPair
    GpuBuffer   mPartPool{}, mPartEmit{}, mPartEmitStg[2]{}, mPartFree{}, mPartFreeCount{};
    GpuBuffer   mPartDrawList{}, mPartDrawCount{}, mPartIndirect{}, mPartFreeReadback{};
    GpuBuffer   mPartRibbonTail{};   // uvec4 per emitter slot (clean_particle_emit.comp)
    // Per-pass cull volumes (#518): CPU-written each frame (vkCmdUpdateBuffer), read by
    // the compaction only. = clean_particle_compute_common.glsl PartCull (all vec4).
    struct PartCullGPU
    {
        float main_planes[6][4];       // the main GPU cull's frustum (frozen with the cull camera)
        float mirror_planes[6][4];
        float mirror_cam[4];           // reflected eye
        float shadow_planes[4][6][4];  // per cascade: its ortho box
        float caster_planes[4][6][4];  // per cascade: the view-slice caster planes
    };
    static_assert(sizeof(PartCullGPU) == 61 * 16, "PartCullGPU must match the std430 PartCull (61 vec4)");
    GpuBuffer   mPartCull{};
    // This frame's lists (recordParticleCompute): the mirror's, and the cascades' (bit c).
    bool        mPartMirrorList = false;
    uint32_t    mPartCascadeMask = 0;
    // RenderVKParticleDiag: pool readback (copied every ~2 s, parsed a few frames later).
    GpuBuffer   mPartDiagRb{};
    uint32_t    mPartDiagPending = 0;   // frames until the copy is parsed (0 = none)
    double      mPartDiagNext = 0.0;
    void        parseParticleDiag();
    VkDeviceSize mPartEmitCap = 0;       // bytes (GPU emitter table + each staging ring slot)
    double   mPartGrowRetryAt = 0.0;   // #697: a refused emitter-table grow backs off until then
    uint32_t mPartGrowStreak = 0;
    uint32_t    mPartMax = 0;            // pool size (particles)
    uint32_t    mPartEmitN = 0;          // emitter slots streamed this frame
    uint32_t    mPartCap = 0;            // live cap this frame (RenderMaxPartCount; 0 = off)
    // Particle compute push (clean_particle_compute_common.glsl PartPush).
    struct PartCompPush { float dt; uint32_t frame, maxp, nbuckets; float shift[4]; float cam[4]; uint32_t ctl[4];
                          uint32_t cull[4]; };   // x: list bits (CULL_MIRROR 1, cascade c 2 << c), y: sort length
    static_assert(sizeof(PartCompPush) == 80, "PartCompPush must match clean_particle_compute_common.glsl PartPush");
    bool        mPartOn = false;         // simulate + draw this frame
    std::vector<uint8_t> mPartEmitCPU;   // this frame's emitters with resolved texture indices
    std::vector<LLUUID>  mPartTexWanted; // particle textures (resolved UNORM, like GL's raw texel)
    std::map<LLUUID, int32_t> mPartTexIndex;
    uint32_t mPartTexIndexGen = 1;       // bumped when mPartTexIndex is rebuilt
    // prepareParticles cache: emitter slot -> texture handle (0xFFFFFFFF = unresolved),
    // rebuilt only when the store's emitter textures or mPartTexIndex change.
    std::vector<std::pair<uint32_t, uint32_t>> mPartTexSlots;
    uint32_t mPartTexSlotsStoreGen = 0, mPartTexSlotsIndexGen = 0, mPartTexSlotsN = ~0u;
    bool     mPartTexSlotsUnresolved = false;
    VkDescriptorSetLayout mDslPartC = VK_NULL_HANDLE, mDslPartG = VK_NULL_HANDLE;
    VkDescPool            mPartDescPool;   // layout-sized + accounted (vk_descpool.h)
    VkDescriptorSet       mDsetPartC = VK_NULL_HANDLE, mDsetPartG = VK_NULL_HANDLE;
    VkPipelineLayout      mPartCompLayout = VK_NULL_HANDLE, mPartDrawLayout = VK_NULL_HANDLE;
    VkPipelineLayout      mPartDrawLayoutRt = VK_NULL_HANDLE;   // + RT consumer set 3 (ray-query variant)
    VkPipeline mPartSimPipe = VK_NULL_HANDLE, mPartEmitPipe = VK_NULL_HANDLE;
    VkPipeline mPartCompactPipe = VK_NULL_HANDLE, mPartIndirectPipe = VK_NULL_HANDLE;
    VkPipeline mPartSortPipe = VK_NULL_HANDLE;   // bitonic depth sort (clean_particle_sort.comp)
    // [variant][blend pair]; PART_PIPE_MAIN_RT only with mRT (ray-query lit variant).
    VkPipeline mPartPipe[PART_PIPE_VARIANTS][NUM_PART_PAIRS] = {};
    std::future<VkPipeline> mPartPipeJob[PART_PIPE_VARIANTS][NUM_PART_PAIRS];   // async builds
    bool       mPartPipeFailed[PART_PIPE_VARIANTS][NUM_PART_PAIRS] = {};   // build returned null: don't retry
    VkShaderModule mPartVS = VK_NULL_HANDLE, mPartFS = VK_NULL_HANDLE, mPartFSRt = VK_NULL_HANDLE;
    VkShaderModule mPartVSLit = VK_NULL_HANDLE, mPartFSLit = VK_NULL_HANDLE;   // PART_PIPE_LIT / LOWRES
    // PER-PARTICLE LIGHTING (clean_particle_light.comp; RenderVKParticleLighting): records
    // (PART_LIGHT_VEC4 vec4 per pool slot; compute set binding 9, draw set binding 3),
    // the coverage counters (binding 10: 64-px units, [0] reduced-resolution pairs, [1]
    // all) and their 3-slot host readback. Layouts {draw 0, draw 1, particle compute set
    // [, RT consumer set]}.
    GpuBuffer        mPartLight{}, mPartCover{}, mPartCoverRb{};
    VkPipelineLayout mPartLightLayout = VK_NULL_HANDLE, mPartLightLayoutRt = VK_NULL_HANDLE;
    VkPipeline       mPartLightPipe = VK_NULL_HANDLE, mPartLightPipeRt = VK_NULL_HANDLE;
    struct PartLightPush { uint32_t view, maxp, width, height; float fade[4]; };   // fade[2]: size-routing px (#879)
    static_assert(sizeof(PartLightPush) == 32, "PartLightPush must match clean_particle_light.comp");
    bool       mPartLitNow = false;      // this frame's main-view draws use the records
    uint32_t   mPartCoverFrame = 0;      // readback ring cursor
    uint8_t    mPartCoverUsed = 0;       // bit s: ring slot s holds a copy
    float      mPartCoverLowRes = 0.f;   // last estimate: shaded layers / pixel, reduced-res pairs
    float      mPartCoverAll = 0.f;      //                                        all pairs
    // Records the lighting compute (after pass 1, before pass 2) and decides mPartLitNow.
    void recordParticleLighting(VkCommandBuffer cmd, const VKDrawPush& drawPush, uint32_t rw, uint32_t rh);
    // Dynamic shading rate of the LIT draws this frame (recordWorld: RenderVKParticleShadingRate).
    uint32_t   mPartVrsW = 1, mPartVrsH = 1;
    // SIZE ROUTING (#879; RenderVKParticleSmallPx, set by recordParticleLighting): sprites
    // projected below mPartSmallPx render px draw at full resolution and full shading rate
    // (never in the reduced-resolution pass, never at the coarse rate). 0 = off.
    // mPartPxPerM = render px per metre at view depth 1 (rh / (2 tan(fovy / 2))).
    float      mPartSmallPx = 0.f;
    float      mPartPxPerM = 0.f;
    uint32_t   mPartRouteSmall = 0, mPartRouteLarge = 0;   // on-screen billboards (3 frames old)
    double     mPartRouteLogAt = 0.0;                      // RenderVKParticleRouteDiag cadence
    double     mPartPairSeen[NUM_PART_PAIRS] = {};   // last time an emitter spawned this pair (s)
    // Ready pipeline for (variant, pair); kicks off its async build on first ask and
    // returns the alpha-pair pipeline (or null) until it lands.
    VkPipeline partPipe(uint32_t variant, uint32_t pair);
    bool partPipeDesc(uint32_t variant, uint32_t pair, GraphicsPipelineDesc& d) const;
    bool initParticles();
    void destroyParticles();
    void writeParticleDescriptors();
    void prepareParticles();                         // CPU: emitter copy + texture indices
    // sim -> emit -> compact (+ per-pass cull, #518) -> sort -> indirect. main_planes: the
    // main GPU cull's frustum; mirror / cascades: whether those passes draw particles
    // this frame (their lists are built only then).
    void recordParticleCompute(VkCommandBuffer cmd, const float main_planes[6][4], bool mirror, bool cascades);
    // PRE_WATER: the main-visible particles on the far side of the water plane (pass 2a);
    // POST_WATER: the near side (pass 2b, after the post-water alpha); MIRROR: the
    // mirror list (both sides, #518), in the planar mirror capture (drawPush = the
    // mirror's push).
    void recordParticleDraw(VkCommandBuffer cmd, const VKDrawPush& drawPush, PartDrawPass pass,
                            PartDrawSet set = PART_SET_ALL);
    std::vector<uint8_t> mProbeUpload;        // header (probes off: written by the CPU)
    // Realtime faces per frame (#238 fallback, probeRtFacesPerFrame() < 6): the next face
    // to render (a rotating cursor: the GPU picks the probe, the CPU only the faces).
    uint32_t mProbeRtCursor = 0;
    uint32_t mProbeRtTraced = 0;   // mProbeCapRT layers whose current face was traced
    uint32_t probeRtFacesPerFrame() const;
    float    mProbeFar = 64.f;   // capture far clip (RenderReflectionProbeDrawDistance)
    // DYNAMIC captures (LL getIsDynamic: the probe's Dynamic flag, detail >= 1) draw
    // avatars -- rigged meshes from the main cull's pre-skinned buckets (their main
    // counts snapshotted into mProbeRigCounters before the HUD cull resets them; culled
    // per face by clean_probe_rigcull.comp, which keeps none for a job that is not
    // dynamic) and worn rigid attachments through the probe cull (cull flag bit 11, cleared
    // by the GPU job patch when the job is not dynamic). Non-dynamic captures exclude them,
    // as LL's cubeSnapshot drops RENDER_TYPE_AVATAR/CONTROL_AV.
    // PER-PROBE LIGHT LISTS (LL culls the light set per probe camera): the frame's lights
    // within each capture's sphere, nearest first, capped at CLUSTER_CAP; [0] scheduled
    // job, [1] realtime. Stored past the main view's light clusters (cluster CLUSTER_COUNT
    // + list); written by clean_probe_gpu.comp (PH_LIGHTS) from the GPU job records.
    static constexpr uint32_t PROBE_LIGHT_LISTS = 2;
    GpuBuffer mProbeRigCounters{};
    bool     mProbeRigCountsValid = false;   // snapshot taken this frame
    // PER-FACE AVATAR CULL (clean_probe_rigcull.comp): each dynamic face compacts the
    // snapshot's six rigged lists (r_opaque, r_mask, ds_r_opaque, ds_r_mask, r_alpha
    // pre / post water) to the draws whose wearer touches the face, in list order, into
    // mProbeRigCmds[L * mProbeRigStride] with counts in mProbeRigOutCounts[L]. Stride =
    // the pre-skin work capacity (every rigged draw holds one work item).
    GpuBuffer mProbeRigCmds{}, mProbeRigOutCounts{};
    uint32_t  mProbeRigStride = 0;
    bool      mProbeRigDescOk = false;   // the set names every buffer (written once complete)
    VkDescriptorSetLayout mDslProbeRig = VK_NULL_HANDLE;
    VkDescriptorSet       mDsetProbeRig = VK_NULL_HANDLE;
    VkPipelineLayout      mProbeRigLayout = VK_NULL_HANDLE;
    VkPipeline            mProbeRigPipe = VK_NULL_HANDLE;
    void writeProbeRigDescriptors();
    // PER-SHADOW-VIEW RIGGED CASTER CULL (#564, clean_shadow_rigcull.comp). The cascades
    // and projector tiles drew the main cull's WHOLE rigged caster lists (+ the pass's
    // off-screen region) into every view; one dispatch per frame now compacts, per view
    // (rendered cascade / projector tile with an avatar in its cone) and rigged list
    // (L = rigOffIndex: 3 4 5 8 9), the draws whose wearer box touches that view, into
    // mShadowRigCmds[((v * 5 + L) * 2 + h) * mShadowRigStride] (h 1 reserved for caster
    // LOD), counts in mShadowRigCounts[(v * 5 + L) * 4 + {kept h0, kept h1, fallback
    // main, fallback off}]: a list whose kept draws overflow the stride draws uncull'd
    // through the fallback counts (the old path) -- correctness never depends on the stride.
    static constexpr uint32_t kShadowRigViews = 4 + 16;   // cascades + projector tiles per frame
    struct ShadowRigViewGPU { float planes[6][4]; float lod[4]; uint32_t info[4]; };   // 128 B (= RigView)
    GpuBuffer mShadowRigCmds{}, mShadowRigCounts{}, mShadowRigViews{};
    uint32_t  mShadowRigStride = 0;      // DrawCmds per (view, list, half); 0 = no buffer
    uint64_t  mShadowRigPeakViews = 0;   // stats: most views culled in one frame
    bool      mShadowRigDescOk = false;
    bool      mShadowRigFrame = false;   // the lists were built this frame
    int32_t   mShadowRigViewOfCascade[4] = {-1, -1, -1, -1};
    int32_t   mShadowRigViewOfProj[kMaxProjShadows] = {};   // per projector slot, -1 = none
    // #564 shadow-caster LOD (LLCleanRigShadowLod): its index buffer (RenderVKShadowRigLodMB)
    // and table (clean_shadow_rigcull.comp binding 13), uploaded in recordWorld.
    GpuBuffer mShadowLodIbo{}, mShadowLodTable{};
    uint32_t  mShadowLodIboWords = 0;   // capacity in 16-bit indices (#573)
    std::vector<uint32_t> mShadowLodDirty;
    bool      mShadowLodFrame = false;   // this frame's lists may hold LOD draws (half 1)
    bool      mShadowLodAllocWanted = false, mShadowLodAllocTried = false;
    bool      mShadowLodTableInit = false;  // the GPU table holds valid keys (first full upload done)
    void      uploadShadowLod(VkCommandBuffer cmd);
    VkDescriptorSetLayout mDslShadowRig = VK_NULL_HANDLE;
    VkDescriptorSet       mDsetShadowRig = VK_NULL_HANDLE;
    VkPipelineLayout      mShadowRigLayout = VK_NULL_HANDLE;
    VkPipeline            mShadowRigCullPipe = VK_NULL_HANDLE;   // (mShadowRigPipe: the rigged cascade raster pipe)
    void writeShadowRigDescriptors();
    // Builds this frame's per-view lists (after invalidateProjShadows, before the cascades).
    void recordShadowRigCull(VkCommandBuffer cmd, const VKCullPush& cullBase, bool do_rigged);
    // Rigged-cull subgroup scan (clean_wg_scan.glsl; rigged-cull crash, TheSlowly report):
    // the probe / shadow rigged-cull pipelines were created with REQUIRE_FULL_SUBGROUPS
    // (the device enabled computeFullSubgroups + has compute subgroup arithmetic). Without
    // it the shaders never take the subgroup path (its prefix assumes full subgroups).
    bool mRigScanFullSubgroups = false;
    // Push bit (probe u_pad / shadow u_flags bit 8) that lets the shaders take the subgroup
    // scan: mRigScanFullSubgroups AND RenderVKRigCullSubgroupScan (live).
    uint32_t rigCullScanBits() const;
    // One rigged caster list (bucket b) of a shadow view: the culled list when built this
    // frame (+ its two zero-count-unless-overflow fallback draws), else the uncull'd main
    // list + off-screen region `r` as before. The caller binds the pipeline.
    void drawShadowRig(VkCommandBuffer cmd, uint32_t b, int32_t view, uint32_t r) const;
    // One capture face: cull (probe mode) + sky + world into `faceView`.
    struct ProbeFaceJob
    {
        VkImage     image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        int         face = 0;
        const float* origin = nullptr;
        float       nearc = 1.f, farc = 64.f;
        bool        skyOnly = false;
        bool        dynamic = false;    // avatars (rigged + worn attachments) included
        uint32_t    lightList = 0;      // PROBE_LIGHT_LISTS index
        bool        stats = false;      // the scheduled job's PR stats queries
        const char* markCull = "pr_cull";
        const char* markFace = "pr_face";
        const char* markAlpha = "pr_alpha";
        const char* markRig = "pr_rigcull";   // per-face avatar cull (dynamic faces)
        const char* markGbuf = "pr_gbuf";     // #833 deferred faces: the G-buffer pass ...
        const char* markLight = "pr_light";   // ... and the probe-resolution lighting
        // First face of its capture job: runs the job's shared pre-cull (cull phase 3)
        // and the avatar cull of all its faces (faceCount from this face on).
        bool        jobStart = true;
        uint32_t    faceCount = 1;
        bool        rtStats = false;    // the realtime job's per-face draw / triangle stats
        bool        mv = false;         // multi-view: culled + sorted by recordProbeMvCull (face = region)
        bool        mvSched = false;    // + the scheduled face riding along (region / counter block 7)
        // #478 GPU-DRIVEN: the GPU capture job (kPrbJobSched / kPrbJobRealtime) this face
        // belongs to, and the ring entries prbPrepareJob reserved for it (patched with the
        // job's cameras): per face the draw view, the rigged draw view and the cull params
        // (multi-view: one cull entry, ringCull[0]).
        int         gpuJob = -1;
        uint32_t    ringView[6] = {~0u, ~0u, ~0u, ~0u, ~0u, ~0u};
        uint32_t    ringViewRig[6] = {~0u, ~0u, ~0u, ~0u, ~0u, ~0u};
        uint32_t    ringCull[6] = {~0u, ~0u, ~0u, ~0u, ~0u, ~0u};
        // Cluster LOD (#702 integration, legacy-skin skip): a GPU job's faces draw the switched
        // avatars from the shared capture rig list (clusterRigDraw(8, CLPASS_PROBE) at prepare
        // time) under ringViewCl[f] (patch kind 3: the face's camera when the GPU job draws
        // avatars, else a view that clips everything); its rigged draw views drop them.
        bool        clRig = false;
        uint32_t    ringViewCl[6] = {~0u, ~0u, ~0u, ~0u, ~0u, ~0u};
    };
    // #478: reserve the ring entries of a GPU capture job's faces [face0, face0 + nfaces)
    // (base contents written by the CPU: everything but the camera) and record the patch
    // (clean_probe_patch.comp) that writes the job's cameras into them. mvCull: one cull
    // entry for the multi-view cull (face 0's). traceable: this frame can trace the job
    // (else the patch clears the job's ray-traced flag: raster faces, no trace).
    // rideCull (#832, the scheduled job riding in the realtime job's multi-view cull):
    // its face's cull entry is the multi-view cull's slot_b -- the per-view region as
    // bucket_cap, patched with the scheduled job's camera like any other cull entry.
    void prbPrepareJob(VkCommandBuffer cmd, ProbeFaceJob& job, const VKDrawPush& drawPush,
                       const VKCullPush& cullBase, uint32_t face0, uint32_t nfaces, bool mvCull, bool traceable,
                       bool rideCull = false);
    // The probe cull params of one face (the shared fields; `M` its view-projection).
    VKCullPush probeCullParams(const VKCullPush& cullBase, const ProbeFaceJob& job, const float M[16],
                               bool avCullOn, bool shared, float& face_px, float& min_px) const;
    // MULTI-VIEW PROBE CULL (RenderVKMultiViewCull): the realtime job's six faces culled,
    // compacted into per-face regions and alpha-sorted in one go (clean_cull.comp phases
    // 5-7, clean_alpha_sort.comp mode 4); the faces then only draw.
    static uint32_t probeRegionFor(uint32_t basis);
    uint32_t probeMvRegion() const;     // per-view region of the private buckets (entries)
    uint32_t probeBufEntries() const;   // private bucket / post-list capacity (all regions)
    bool     probeMvUsable() const;
    // sched: the frame's scheduled face riding along as view 7 (RenderVKMultiViewScheduled;
    // its avatar cull and the realtime faces' then run after its draws, recordProbeRigCull).
    // #832: its params entry is sched->ringCull[sched->face] (prbPrepareJob rideCull: the
    // GPU job's camera patched in), the realtime job's ringCull[0] the primary entry.
    void     recordProbeMvCull(VkCommandBuffer cmd, const VKCullPush& cullBase, const ProbeFaceJob& job,
                               const ProbeFaceJob* sched);
    bool     probeRigCullOn(const ProbeFaceJob& job) const;
    void     recordProbeRigCull(VkCommandBuffer cmd, const VKCullPush& cullBase, const ProbeFaceJob& job,
                                uint32_t face0, uint32_t nfaces);
    GpuBuffer mCandB{};   // multi-view: the scheduled face's candidates (cull binding 27)
    bool     mProbeMvRan = false;       // since the last probe log (log only)
    void recordProbeFace(VkCommandBuffer cmd, const VKDrawPush& drawPush, const VKCullPush& cullBase,
                         const ProbeFaceJob& job);
    // Mip chain of a finished capture cube (6 faces in COLOR_ATTACHMENT_OPTIMAL, or GENERAL
    // for the layers set in `traced`: written by the ray-traced capture) + GGX radiance /
    // irradiance filter into `slot`.
    // gpuJob >= 0 (#478): the target slot and the dispatch sizes are that GPU capture job's
    // (job records at jobOff in mPrbJobs; an empty job dispatches nothing).
    void recordProbeFilter(VkCommandBuffer cmd, VkImage capImage, const std::vector<VkDescriptorSet>& sets,
                           uint32_t slot, const char* markMips, const char* markFilter, uint32_t traced = 0u,
                           int gpuJob = -1, VkDeviceSize jobOff = 0);

    // ---- RAY-TRACED PROBE CAPTURE (RenderReflectionProbeDetail 3 "Raytraced") ----
    // Same probes and cadence as Realtime (the realtime probe's six faces every frame +
    // the scheduled job's face), but the faces are TRACED against the TLAS by
    // clean_probe_rt.comp into mip 0 of the same capture cubes; the filter is unchanged.
    // Per job it falls back to the raster faces when RT can't serve it (no RT / pipeline,
    // TLAS not valid this frame, or the capture sphere reaching past the TLAS radius).
    VkDescriptorSetLayout mDslProbeTrace = VK_NULL_HANDLE;   // binding 0: capture mip 0 (storage, 2D array)
    VkPipelineLayout      mProbeTraceLayout = VK_NULL_HANDLE;   // draw sets 0 / 1, RT set, mDslProbeTrace
    VkPipeline            mProbeTracePipe = VK_NULL_HANDLE;
    // #1064: mProbeTracePipe compiles in the background the first time Raytraced is selected
    // (probeTraceEnsure), not at init: until it lands (or if it fails / is quarantined) detail 3
    // runs as Realtime, as with any missing piece. Queued: asked once per renderer lifetime.
    PipelineJobSlot       mProbeTraceJob;
    bool                  mProbeTracePipeQueued = false;
    void probeTraceEnsure();
    VkDescPool            mProbeTracePool;                     // (initProbes: the sets name the cubes)
    VkDescriptorSet       mDsetProbeTrace[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};   // [0] mProbeCap, [1] mProbeCapRT
    VkImageView           mProbeCapStoreView = VK_NULL_HANDLE, mProbeCapRTStoreView = VK_NULL_HANDLE;
    int                   mProbeDetailNow = 1;    // EFFECTIVE RenderReflectionProbeDetail (setProbes, #736)
    int                   mProbeDetailReq = -99;  // the requested one (the setting) last frame (-99: none yet)
    const char*           mProbeTraceWhy = nullptr;   // why Raytraced runs as Realtime (null: it does not)

    // ---- DEFERRED PROBE CAPTURE (#833, RenderVKProbeDeferred, live) ----
    // A probe face's opaque / mask / rigged draws go into a probe-resolution G-buffer
    // (mProbeGb, the main G-buffer's formats, one face at a time; mProbeCapDepth is its depth)
    // through the capture G-buffer FS, then clean_probe_defer.comp lights it -- the capture
    // FS's lightSurface (probe light list, sun / one hard sun ray, no probe walk) in compute
    // -- into the face's layer of the capture cube. Sky first, water / cluster avatars /
    // blended alpha forward after. Sub-marks pr_gbuf / pr_light (realtime: pr_rt_*).
    struct ProbeDeferPushData { uint32_t view, face, res, flags; };   // clean_probe_defer.comp
    static_assert(sizeof(ProbeDeferPushData) == 16, "ProbeDeferPushData");
    GpuImage              mProbeGb[kGbufCount]{};                     // C x C (allocProbes)
    VkDescriptorSetLayout mDslProbeDefer = VK_NULL_HANDLE;            // b0-3 G-buffer, b4 depth, b5 cube mip 0
    VkPipelineLayout      mProbeDeferLayout = VK_NULL_HANDLE;         // draw sets 0 / 1 + mDslProbeDefer
    VkPipelineLayout      mProbeDeferLayoutRt = VK_NULL_HANDLE;       // + the RT consumer set (set 3)
    VkPipeline            mProbeDeferPipe = VK_NULL_HANDLE, mProbeDeferRtPipe = VK_NULL_HANDLE;
    VkDescPool            mProbeDeferPool;                            // (allocProbes: the sets name the cubes)
    VkDescriptorSet       mDsetProbeDefer[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};   // [0] mProbeCap, [1] mProbeCapRT
    // G-buffer pass pipelines (plain layout; [0] cull back, [1] none): opaque / mask, the mask
    // pre-pass (colour masked), mask at EQUAL, rigged.
    VkPipeline mProbeGbPipe[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkPipeline mProbeGbPreMaskPipe[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkPipeline mProbeGbEqPipe[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkPipeline mProbeGbRigPipe[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    // This face can take the deferred path (setting on, a GPU job's raster face, every
    // pipeline / set / image ready -- the ray-query twin in frames that trace). Else forward.
    bool probeDeferredFace(const ProbeFaceJob& job) const;
    // #736 readiness gate: null when everything a traced capture touches exists (RT, the
    // trace pipeline + layout, the capture cubes, their trace / filter sets, draw set 1),
    // else what is missing. Built at init / probe allocation, never on the switch.
    const char* probeTraceMissing() const;
    uint32_t              mProbeCapTraced = 0;    // mProbeCap layers whose current face was traced
    // clean_probe_rt.comp ProbeTracePush (std430, 48 B).
    struct ProbeTracePushData
    {
        float    origin[4];     // xyz probe origin, w near clip
        float    farc;
        uint32_t face0, flags, light_list, res;
        float    time, cone_spread;
        uint32_t pad;
        uint32_t job[2];        // PRT_JOB (#478): the capture job records' device address
    };
    static_assert(sizeof(ProbeTracePushData) == 56, "ProbeTracePushData = clean_probe_rt.comp ProbeTracePush");
    struct ProbeTraceJob
    {
        VkImage         image = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
        float           origin[3] = {0.f, 0.f, 0.f};
        float           nearc = 1.f, farc = 64.f;
        uint32_t        face0 = 0, nfaces = 1;
        uint32_t        flags = 0;      // clean_probe_rt.comp PRT_*
        uint32_t        lightList = 0;  // PROBE_LIGHT_LISTS index
        float           time = 0.f;     // draw time (texture animation, water)
        const char*     mark = "pr_trace";
        int             gpuJob = -1;    // #478: origin / clips / flags / dispatch size from this GPU job
        VkDeviceSize    jobOff = 0;     // that frame slot's job records in mPrbJobs
    };
    // Early probe faces (RenderVKProbeEarly): the traces wait here until pass 1b, where
    // they go to the async compute queue (RenderVKAsyncProbeRT) beside the lit pass, or
    // are recorded on the graphics queue; joined before the deferred filters.
    std::vector<ProbeTraceJob> mProbeTracePending;
    bool mProbeTraceDefer = false;
    bool mProbeTraceJoin = false;     // an async trace segment is not joined yet
    bool probeTraceWanted() const;    // detail 3 with probes on: a traced capture is possible (RT build gate)
    // RT can trace probe captures this frame (detail 3, the pipeline / sets, a VALID TLAS,
    // the VRAM ladder): the per-job test (the sphere inside the TLAS radius) runs on the GPU.
    bool probeTraceReady() const;
    void probeTraceSubmit(VkCommandBuffer cmd, const ProbeTraceJob& j);   // record now or defer
    // layoutDone (#845 / #917): the job's layers were already taken to GENERAL on the graphics
    // queue (probeTraceLayouts, before the async fork): only the memory dependency here.
    void recordProbeTrace(VkCommandBuffer cmd, const ProbeTraceJob& j, bool layoutDone = false);
    void probeTraceLayouts(VkCommandBuffer cmd, const ProbeTraceJob& j);
    // Records the deferred traces: on an async segment when allowAsync and the queue is
    // usable (cmd = the open graphics command buffer, replaced by the fork), else on cmd.
    void flushProbeTraces(VkCommandBuffer& cmd, bool allowAsync);
    // PROBE CAPTURE STATS (#124, GPU timers on): every capture frame gets 3 timestamps
    // (start, face done, end) + a pipeline-statistics query over the face draw, in a
    // 3-deep ring harvested without waits -- the frame timers sample every other frame,
    // which aliases with the 6-frame face cycle and never saw the filter frame.
    VkQueryPool mPrTsPool = VK_NULL_HANDLE, mPrStatPool = VK_NULL_HANDLE;
    uint32_t    mPrFrame = 0;
    // Stats queries per draw group (slot * PR_GROUPS + g): sky, opaque, mask, ds
    // opaque, ds mask, water, rigged opaque+mask (dynamic faces; reported apart from
    // the static tris / vs totals).
    static constexpr uint32_t PR_GROUPS = 7;
    struct PrRec { bool issued = false, filter = false, draws = false, rigdraws = false; uint32_t face = 0, groups = 0; };
    // REALTIME probe faces (detail 2, six per frame): per face its draw counts (after the
    // size cutoff) and one pipeline-statistics query over its world draws. 3-deep ring.
    VkQueryPool mPrRtStatPool = VK_NULL_HANDLE;
    GpuBuffer   mPrRtDrawRb{};        // 3 x 6 faces x 16 words (layout as mPrDrawRb)
    uint32_t    mPrRtFrame = 0, mPrRtUsed[3] = {0, 0, 0};   // per slot: faces recorded (bits)
    struct PrRtAcc { uint32_t nfaces = 0, nframes = 0; double draws = 0, water = 0, rig_draws = 0, tris = 0, tris_worst = 0;
                     double bucket[6] = {};
                     double kept = 0, sizecut = 0, over = 0, uni = 0, uni_worst = 0;
                     uint32_t nfunnel = 0; } mPrRtAcc;   // funnel + multi-view union length (face 0)
    bool probeRtStatsBegin(VkCommandBuffer cmd);   // harvest + reset this frame's ring slot (false: stats off)
    // ctrBase: the face's counter block in mProbeCounters (words; multi-view faces).
    // funnel: copy view 0's funnel counts too (else zero them); mvUnion: the multi-view
    // union alpha list's length -> word 15 (else zero).
    void probeDrawCountCopy(VkCommandBuffer cmd, VkBuffer dst, VkDeviceSize base, bool rig, uint32_t face,
                            uint32_t ctrBase = 0u, bool funnel = true, bool mvUnion = false);
    // Draw counts of the stats face (host-visible, 3 x 16 words): [0..2] opaque / mask /
    // alpha, [3..4] ds opaque / ds mask, [5..10] the avatar-culled rigged lists, [11]
    // the water draws (split off the alpha bucket by the probe alpha sort), [12..14] the
    // cull's funnel counts (kept instances, size-cut, region overflow; private set only,
    // filled by the face that ran the job's pre-cull), [15] the multi-view union alpha
    // list's length (realtime face 0).
    GpuBuffer   mPrDrawRb{};
    PrRec       mPrRec[3];
    struct PrAcc { uint32_t n = 0, nfilter = 0; double face_ms = 0, face_worst = 0, filter_ms = 0, filter_worst = 0;
                   double prims = 0, prims_worst = 0, vs = 0, fs = 0; uint32_t worst_face = 0;
                   double gfs[PR_GROUPS] = {}, worst_gfs[PR_GROUPS] = {}, worst_tris = 0;
                   double rig_prims = 0, rig_vs = 0; uint32_t nrig = 0;
                   double draws = 0, rig_draws = 0; uint32_t ndraws = 0, nrigdraws = 0;
                   double kept = 0, sizecut = 0; uint32_t nfunnel = 0; } mPrAcc;
    // true = this frame's PR slot was reset (timers on): only then may the scheduled face
    // record its queries / timestamps (#790: a pool left from an earlier timers-on stretch
    // was written every frame without a reset once the timers went off).
    bool probeStatsBegin(VkCommandBuffer cmd);
    // PASS-2 STATS (GPU timers on): pipeline statistics per draw group, 3-deep ring.
    enum P2Group { P2_PRE_STATIC, P2_PRE_RIG, P2_WATER, P2_POST_STATIC, P2_POST_RIG, P2_PARTICLES, P2_PRE_PARTICLES, P2_GROUPS };
    VkQueryPool mP2Pool = VK_NULL_HANDLE;
    uint32_t    mP2Frame = 0, mP2Used[3] = {0, 0, 0};
    uint64_t    mP2Px = 0, mP2PxSlot[3] = {0, 0, 0};
    double      mP2Acc[P2_GROUPS][3] = {};   // per group: IA prims, VS, FS invocations (summed)
    double      mP2PxAcc = 0;
    uint32_t    mP2Frames = 0;
    void p2StatsBegin(VkCommandBuffer cmd);
    void p2Query(VkCommandBuffer cmd, P2Group g, bool begin);
    std::string p2StatsText();   // harvest + reset this ring slot, timestamp 0
    // Store occupancy for leak watching (VKMem): live / high-water / capacity per clean
    // store and the GPU table each mirrors into. detailed = also free / pending counts,
    // lifetime totals and CPU / GPU bytes (RenderVKMemStatsLog, once a minute).
    std::string memStoreStatsText(bool detailed);
    double mMemStatsNextDetail = 0.0;   // RenderVKMemStatsLog: next detailed dump (steady s)
    // #737 ASSET RELEASE LOG (CleanRelease): once a minute the per-minute release counts
    // by kind (LLCleanAssetLife) and the live counts they act on; and 120 s after a
    // teleport ends, one "released since TP" line comparing live counts with the moment
    // the teleport started. Per frame: a time / teleport-state check only.
    struct ReleaseLive
    {
        uint32_t mats = 0, mats_hw = 0, tex_live = 0, tex_hw = 0, mesh_slots = 0, groups = 0, groups_ref = 0;
        uint32_t inst = 0;
        uint64_t tex_bytes = 0, geom_cap_bytes = 0, mirror_bytes = 0;
    };
    ReleaseLive releaseLiveSnapshot();
    void logReleaseStats();
    double      mRelNextLog = 0.0;
    uint64_t    mRelPrev[16] = {};
    uint64_t    mRelTpBase[16] = {};
    ReleaseLive mRelTpLive;
    bool        mRelTpActive = false;    // a teleport is in progress
    double      mRelTpReportAt = 0.0;    // > 0: log the TP summary at this steady time
    uint64_t    mTexVramBytes = 0;       // bindless texture device bytes (5 s VRAM log)
    void probeStatsHarvest(uint32_t slot);
    std::string probeStatsText();
    // Selection outline (polygonMode LINE over the selected instances).
    static constexpr uint32_t SEL_CAP = 8192;
    VkPipeline mSelVisPipe = VK_NULL_HANDLE, mSelHidPipe = VK_NULL_HANDLE;
    // HUD pass twins: conventional depth (the HUD pass's own buffer), not reversed-Z.
    VkPipeline mSelHudVisPipe = VK_NULL_HANDLE, mSelHudHidPipe = VK_NULL_HANDLE;
    GpuBuffer  mSelCmds[2]{};
    // Lists of SEL_KINDS (root, child, hover preview) each: world [0..2], hud [3..5],
    // RIGGED world [6..8]. The rigged lists stay last + contiguous (clean_sel_rig.comp
    // completes them as one range).
    static constexpr uint32_t SEL_KINDS = 3, SEL_LISTS = 3 * SEL_KINDS;
    uint32_t   mSelCount[SEL_LISTS] = {};
    float      mSelParentColor[4] = {1,1,0,1}, mSelChildColor[4] = {0,0.5f,1,1};
    float      mSelHoverColor[4] = {1,0.35f,1,1};   // #252 links/faces list hover preview
    const float* selColor(uint32_t kind) const
        { return kind == 0 ? mSelParentColor : kind == 1 ? mSelChildColor : mSelHoverColor; }
    bool       mSelHidden = true;
    // Rigged selection (#170): the rigged lists' DrawCmds are completed on the GPU from
    // the main cull's cand[] (clean_sel_rig.comp) and drawn from mSkinVbo.
    VkPipeline mSelRigVisPipe = VK_NULL_HANDLE, mSelRigHidPipe = VK_NULL_HANDLE;
    VkPipeline mSelRigFixPipe = VK_NULL_HANDLE;
    VkPipelineLayout mSelRigLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout mDslSelRig = VK_NULL_HANDLE;
    VkDescriptorSet mDsetSelRig[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};   // per mSelCmds slot
    void writeSelRigDescriptors();
    void recordSelRigFixup(VkCommandBuffer cmd);
    void recordSelection(VkCommandBuffer cmd, const VKDrawPush& push, bool hud);
    // <GMDX> SELECTION STYLE (GMDXSelectionStyle; vkgfx/gmdxseloutlinevk.cpp defines these):
    // 0 Classic (Firestorm's mesh wireframe for what has no LL silhouette), 1 Outline (the
    // screen-space outline, everything), 2 Wireframe (Slipstream's, the pipelines above).
    int  selectionStyle() const;
    // Classic pipelines [pair * 2 + pass] (pair 0 world, 1 HUD, 2 rigged; pass 0 hidden,
    // 1 visible), built on first use; null = unavailable (the wireframe pipelines then).
    const VkPipeline* selClassicPipes();
    void selPassColor(uint32_t kind, int pass, bool classic, float out[4]) const;
    bool selOutlineWanted(bool hud) const;   // Outline style and something in the lists
    // After the selection pass (world: against the display depth; HUD: after the HUD
    // buckets, its depth stored): the mask, then the edge pass over colorView.
    void recordSelOutline(VkCommandBuffer cmd, const VkDescriptorSet* dsets, const VKDrawPush& push, bool hud,
                          VkImageView colorView, const GpuImage& depth, uint32_t width, uint32_t height,
                          const VkViewport& vp, const VkRect2D& sc);
    void destroySelOutline();
    GMDXSelOutlineVK* mSelOutline = nullptr;
    // </GMDX>
    // Advanced > Rendering > Wireframe (gUseWireframe, #29): every world draw this frame's
    // main cull emitted, edges only, over a cleared target (LL: glPolygonMode LINE around
    // the world render). Replays the cull's indirect lists through the selection-outline
    // pipelines (polygonMode LINE): no new pipelines, no CPU per-face work.
    void recordWireframe(VkCommandBuffer cmd, const VKDrawPush& push, bool do_rigged);
    bool initProbes();
    void destroyProbes();   // probe resources (not the capture draw pipelines)
    // ---- SETTING-DRIVEN VRAM ALLOCATIONS (#226) ----
    // Reflection probes, sun shadow cascades, the RT shadow mask and the projector shadow
    // atlas are sized by user settings and can each be GBs. Before (re)allocating, the
    // exact bytes (VkDeviceContext::imageMemoryBytes) are checked against the free
    // device-local budget plus what the old resources free, minus a margin; a size that
    // does not fit steps down to the largest that does, the setting is written back
    // (latched, the UI shows it) and a notice is posted (merged, from the idle loop). The
    // allocation itself runs soft: out of memory frees the partial set and steps down.
    struct VramCand { std::string label; uint64_t need; };
    // deferred: a re-allocation was NOT taken because the driver's live free memory does
    // not cover it (vramDriverFits); the caller keeps what it holds and re-checks later.
    struct VramRoom { uint64_t budget = 0, usage = 0, margin = 0, avail = 0; bool deferred = false;
                      uint64_t drvFree = 0, drvMargin = 0; };
    // avail = budget - usage + freed - margin (0 when negative). RenderVKDebugFakeVramBudgetMB
    // caps the budget here (these checks only), for testing on big cards.
    VramRoom vramRoom(uint64_t freed) const;
    // Index of the first candidate (most wanted first) that fits, keep_idx (the current
    // allocation, needs nothing new) at worst; -1 if none and no keep_idx. One log line.
    // RE-ALLOCATION GATE (freed > 0: the old set would be freed and a new one allocated):
    // every candidate other than keep_idx must also fit the driver's LIVE free memory
    // without counting what the old set frees (vramDriverFits); otherwise it is skipped and
    // room_out->deferred is set. NVIDIA 615.71 segfaults inside vkAllocateMemory at the
    // OOM wall when another process takes the VRAM (probe resize crash, 2026-10-20).
    int  vramPick(const char* what, const std::vector<VramCand>& cands, uint64_t freed, int keep_idx,
                  VramRoom* room_out) const;
    // The driver's live numbers (VK_EXT_memory_budget, fresh query; budget capped by
    // RenderVKDebugFakeVramBudgetMB for testing): does `need` fit its free memory with the
    // large-request pre-flight margin (max(budget / 32, 256 MB))? Freed memory is NOT
    // counted -- under contention another process can take it first. true without the
    // budget extension (nothing to check against: the old behaviour).
    bool vramDriverFits(uint64_t need, uint64_t* free_out = nullptr, uint64_t* margin_out = nullptr) const;
    static std::string vramText(uint64_t bytes);   // "3.1 GB" / "640 MB"
    void notifyVramRevert(const std::string& text);
    uint64_t probeBytes(uint32_t r, uint32_t n) const;
    uint64_t probeHeldBytes() const;
    void configureProbes(bool live);            // settings -> check -> (re)allocate / step down
    bool allocProbes(uint32_t r, uint32_t n, bool soft);   // false: out of memory, partial set freed
    // #599: no probe set could be allocated live (out of video memory even at the minimum):
    // draw set 0 bindings 10-14 name a 1x1 black cube array + zeroed buffers (probe count 0)
    // and the set is retried with backoff (resizeProbesIfNeeded). Never fatal.
    bool probesAbsentFallback();
    void destroyProbeDummy();
    GpuImage  mProbeDummyCube{};         // 1x1, 6 layers, CUBE_ARRAY view, black, GENERAL
    GpuBuffer mProbeDummyBuf{};          // zeroed: header (count 0) + cluster counts
    VkSampler mProbeDummySampler = VK_NULL_HANDLE;
    bool      mProbesAbsent = false;     // the fallback is bound (no probe set)
    double    mProbeRetryAt = 0.0;       // steady s: next live re-try of the probe set
    uint32_t  mProbeRetryStreak = 0;
    void recordProbeCapture(VkCommandBuffer cmd, const VKDrawPush& drawPush, const VKCullPush& cullBase,
                            bool do_rigged);
    void recordSSRCopy(VkCommandBuffer cmd, uint32_t width, uint32_t height);
    GpuBuffer mLightSSBO{}, mLightInfoSSBO{}, mClusterCount{}, mClusterIndex{};
    VkDescriptorSetLayout mDslLights = VK_NULL_HANDLE;
    VkDescriptorSet       mDsetLights = VK_NULL_HANDLE;
    VkPipelineLayout      mLightsLayout = VK_NULL_HANDLE;
    VkPipeline            mLightsPipe = VK_NULL_HANDLE;
    // Projector textures wanted this frame -> resolved bindless index (-1 = not yet).
    std::vector<LLUUID>           mLightTexWanted;
    std::map<LLUUID, int32_t>     mLightTexIndex;
    bool                          mLightTexDirty = false;

    // Bindless textures.
    std::vector<GpuImage> mTextures;
    VkSampler mSampler = VK_NULL_HANDLE;
    float     mSamplerAniso = 1.f;   // mSampler's maxAnisotropy (texel demand allowance)
    float     mAnisoLevel = -1.f;    // the setting level mSampler was built for (0 = off)
    float     wantedAnisoLevel() const;
    void      createWorldSampler();
    void      applyAnisoChange();
    uint32_t  mTextureCount = 0;

    // Descriptors / pipelines.
    VkDescriptorSetLayout mDslCull = VK_NULL_HANDLE;
    VkDescriptorSet       mDsetCull = VK_NULL_HANDLE;
    VkDescriptorSetLayout mDslDraw0 = VK_NULL_HANDLE;
    VkDescriptorSet       mDsetDraw0 = VK_NULL_HANDLE;
    VkDescriptorSetLayout mDslDraw1 = VK_NULL_HANDLE;
    VkDescriptorSet       mDsetDraw1 = VK_NULL_HANDLE;   // = mDsetDraw1Slots[frame slot] while recording
    // Bindless texture set per frame-in-flight slot: a frame only ever binds its own set,
    // written after acquire() waited that slot's fence -- descriptor rewrites never touch
    // a set the GPU may still read, so no device-idle wait is needed.
    static constexpr uint32_t kTexDescSets = 3;
    VkDescriptorSet       mDsetDraw1Slots[kTexDescSets] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    // Per-slot descriptor versions: bumped (touchTexSlot) whenever a slot's view is
    // (re)assigned or released; each set remembers the version it last wrote. Versions,
    // not view handles -- a destroyed view's handle value can be reused by a new one.
    std::vector<uint32_t> mTexVer;
    uint32_t mTexVerNext = 1;
    std::vector<uint32_t> mDescWrittenVer[kTexDescSets];
    // The slot's CONTENT version (what the pixels are; OMM bakes key on it): bumped with
    // mTexVer except by a texture pool compaction move (#614: same pixels, new image).
    std::vector<uint32_t> mTexContentVer;
    void touchTexSlot(uint32_t i, bool content = true)
    {
        if (i >= mTexVer.size()) mTexVer.resize(i + 1, 0u);
        mTexVer[i] = mTexVerNext++;
        if (!content) return;
        if (i >= mTexContentVer.size()) mTexContentVer.resize(i + 1, 0u);
        mTexContentVer[i] = mTexVer[i];
    }
    // C11: per set, mTexVerNext + slot count at its last full version scan: equal ->
    // no slot was touched since, so writeTextureDescriptors skips the O(slots) scan.
    uint32_t mDescScanVer[kTexDescSets] = {}, mDescScanN[kTexDescSets] = {};
    std::vector<VkDescriptorImageInfo> mDescII;                 // scan scratch (no per-call allocs)
    std::vector<VkWriteDescriptorSet> mDescWS;
    std::vector<std::pair<uint32_t, uint32_t>> mDescRuns;
    void resetTexDescWritten()
    {
        for (auto& w : mDescWrittenVer) w.clear();
        for (uint32_t& v : mDescScanVer) v = 0;
        // (mDescImg / mDescView are NOT cleared: they mirror what each set holds on the GPU
        // until its next write, which the retire guard must keep seeing)
    }
    // RETIRE GUARD MIRROR (device lost on a texture freed while a frame in flight sampled it,
    // 2026-10-22): per set and array element, the image + view the set's descriptor names as
    // last written by writeTextureDescriptors (VK_NULL_HANDLE: the white slot). A retired image
    // or view is destroyed only when no set another frame may be reading still names it.
    std::vector<VkImage>     mDescImg[kTexDescSets];
    std::vector<VkImageView> mDescView[kTexDescSets];
    VkDescPool            mPool;   // layout-sized + accounted (vk_descpool.h)

    VkPipelineLayout mCullLayout = VK_NULL_HANDLE;
    VkPipeline       mCullPipe = VK_NULL_HANDLE;
    VkPipelineLayout mDrawLayout = VK_NULL_HANDLE;
    VkPipeline       mDrawPipe = VK_NULL_HANDLE;   // opaque: depth test+write, no blend
    VkPipeline       mMaskPipe = VK_NULL_HANDLE;   // alpha-mask: depth test+write, discard
    VkPipeline       mAlphaPipe = VK_NULL_HANDLE;  // alpha-blend: depth test, no write, blend
    VkPipeline       mRiggedPipe = VK_NULL_HANDLE; // rigged opaque+mask: skinned verts, depth test+write
    VkPipeline       mRiggedAlphaPipe = VK_NULL_HANDLE; // rigged alpha-blend: depth test, no write
    // Back-face culling: mDrawPipe/mMaskPipe/mRiggedPipe cull BACK (LL GL_CULL_FACE);
    // these cull-NONE twins draw the double-sided buckets 6..9.
    VkPipeline       mDrawDsPipe = VK_NULL_HANDLE, mMaskDsPipe = VK_NULL_HANDLE, mRiggedDsPipe = VK_NULL_HANDLE;
    // ---- DEPTH PRE-PASS (RenderVKDepthPrepass) ----
    // Pre-pass: depth-only over pass 1's opaque/mask indirect draws (same VS modules,
    // invariant gl_Position), depth LESS + write, colour masked (same rendering scope
    // as pass 1, so the attachment formats match). Index [0] back-culled, [1] cull-none.
    // Opaque buckets have NO FS; mask buckets run clean_prepass_mask.frag.
    VkPipeline       mPreOpaquePipe[2]  = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkPipeline       mPreMaskPipe[2]    = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkPipeline       mPreRigPipe[2]     = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkPipeline       mPreRigMaskPipe[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
public:
    bool worldReverseZ() const;   // main-view depth is reversed-Z (#188)
    // ---- AVATAR MASK contract (#549; for the FX stack, P5) ----
    // R16_UINT, value = avatar slot + 1 (0 = no avatar; slot = LLCleanSkinStore joint-table
    // slot). DISPLAY resolution (the extent of the post chain / FX), UNJITTERED (TAA jitter
    // removed with clean_depth_up.comp's mapping). Hard-edged ids: sample with texelFetch /
    // a NEAREST sampler (usampler2D), never filtered; feathering is the consumer's job.
    // Layout SHADER_READ_ONLY_OPTIMAL, written before pass 2 and valid until the end of the
    // frame's recording (visible to FRAGMENT + COMPUTE reads). Main view + offscreen
    // snapshots only (no probe / mirror / HUD ids). Re-created on size changes:
    // key descriptor sets on avatarMaskGen(), never on the view handle.
    // requestAvatarMask(true) keeps it produced each frame (default: only while cel
    // outlines / the debug view need the render-res ids); hasAvatarMask() = this frame's.
    // The view can change frame to frame (render target vs display copy): re-query both
    // after recordWorld's pass 1 every frame; safe to call before the renderer is ready.
    VkImageView getAvatarMaskView() const;
    bool        hasAvatarMask() const;
    uint64_t    avatarMaskGen() const;
    void        requestAvatarMask(bool want);
private:
    // Alpha-blend depth pass (RenderVKAlphaDepthPrepass): near-opaque blend texels
    // write depth at the end of pass 1. [0] static (mVbo), [1] rigged (mSkinVbo).
    VkPipeline       mPreBlendPipe[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    // ---- DEPTH-ONLY VS TWINS (RenderVKDepthOnlyVS, live A/B) ----
    // Every depth-only pipeline below (pre-pass opaque / mask / rigged / alpha-blend depth /
    // probe-face mask, sun cascades, projector tiles, both translucency layers) is built a
    // second time with the reduced VS (clean_draw{,_rigged}.vert -DCLEAN_DEPTH_ONLY, + MASK
    // when it has an alpha-test FS): same desc, only the vertex module differs. The twins sit
    // in mDepthVsAlt; applyDepthOnlyVS() (top of recordWorld, after the async batch is in)
    // SWAPS each member with its twin when the setting differs from what is installed, so
    // every bind site keeps naming the member and ownership stays one handle per slot
    // (destroy releases members + twins). A twin that failed to compile (#672) is never
    // swapped in: that slot keeps the full VS. Slot order: depthVsSlots().
    // The normal / motion / motion-only pre-pass pipes (slots 34..57) take the SLIM pre-pass VS
    // flavours (+normalVS / +normalMotionVS / +motionVS: -DCLEAN_DEPTH_NORMAL / _MOTION).
    static constexpr int kDepthVsSlots = 58;
    VkPipeline       mDepthVsAlt[kDepthVsSlots] = {};
    bool             mDepthVsLive[kDepthVsSlots] = {};   // true: the member holds the depth-only VS twin
    std::array<VkPipeline*, kDepthVsSlots> depthVsSlots();
    void             applyDepthOnlyVS();
    // Depth + NORMAL pre-pass twins (ambient occlusion frames): same state, plus
    // clean_prepass_normal.frag writing the view-space shading normal to mAoNormal.
    VkPipeline       mPreNrmPipe[2]        = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkPipeline       mPreNrmMaskPipe[2]    = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkPipeline       mPreNrmRigPipe[2]     = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkPipeline       mPreNrmRigMaskPipe[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    // Pass-1 twins after the pre-pass: depth EQUAL, no write, early_fragment_tests FS.
    // Serve both the opaque and mask buckets (the FS alpha-tests mask instances).
    VkPipeline       mDrawEqPipe[2]     = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkPipeline       mRiggedEqPipe[2]   = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    // Base desc of the pass-1 lit pipelines (formats / vertex input / layout), kept for
    // the lazily-built overdraw-stats variants.
    GraphicsPipelineDesc mLitDesc;
    // CLUSTER LOD (#642 / #743, RenderVKClusterLOD; off by default). The module is created
    // the first frame the setting is on and never exists otherwise; every hook below is a
    // few lines guarded by clusterLodOn() / mCluster (see CLUSTER_LOD_CONTRACT.md).
    // mGbufDesc: template of the deferred G-buffer pipelines (vert / frag cleared) the
    // cluster draw pipelines are built compatible with.
    LLCleanClusterVK* mCluster = nullptr;
    GraphicsPipelineDesc mGbufDesc;
    bool mGbufDescValid = false;
    uint64_t mClusterRetryFrame = 0;                  // init failed: retry after this frame
    uint32_t mHizImageGen = 0;
    uint32_t mOcclHizGen = ~0u, mOccl2HizGen = ~0u;   // mHizImageGen their sets were written with                        // bumped on every Hi-Z re-create (cluster descriptor refresh)
    uint64_t mHizBuiltFrame = ~0ull;                  // frame the Hi-Z pyramid was last built (#745)
    // #749: templates of the depth pre-pass pipelines ([0] pass-1 depth only, [1] + AO normal,
    // [2] + motion + no-history mask; vert / frag / layout / vertex input cleared) the cluster
    // pre-pass draws are built against, the camera's near clip, and whether this recording
    // ran the cluster tick (its GPU half is recorded by clusterFrameStart).
    GraphicsPipelineDesc mPreDescCl[4];
    bool mPreDescClValid = false;
    float mCamNear = 0.1f;
    bool mClusterTicked = false;
    bool clusterLodOn() const;                         // RenderVKClusterLOD and the module ready
    // 2026-10-14: the session mode in which the cluster path OWNS every cluster-eligible rigged
    // mesh (clusterTick: LOD on, module + 2a working, RenderVKClusterRigged, deferred lighting):
    // no legacy geometry upload, draw, pre-skin, weld or RT BLAS for them, drawn or not yet.
public:
    bool clusterOwnsRigged() const { return mClOwnsRigged.load(std::memory_order_relaxed); }
private:
    // Frame start (after beginTextureFrame): the module's CPU half (settings, the #749
    // controller, streaming + selection frame setup). Records nothing (#749).
    void clusterTick(VkCommandBuffer cmd, const VKCullPush& cullIn, uint32_t height);
    // #749: the module's GPU frame start, recorded where the front end runs: inside the
    // front-end segment F under RenderVKFramePipelining (in_fe), else on graphics -- where page
    // copies may go to a forked async compute segment (cmd is replaced by the fork).
    void clusterFrameStart(VkCommandBuffer& cmd, bool in_fe);
    uint32_t clusterCullMain(VkCommandBuffer cmd, const VKCullPush& push, const VKDrawPush& drawPush,
                             uint32_t rw, uint32_t rh);   // main view selection (kNone = none)
    // #749: selection-only cluster views of the other passes (their selected lists feed 2a /
    // 2b; the passes keep their legacy draws). RenderVKClusterViews bit per role: 1 sun
    // cascades, 2 probe faces, 4 mirror.
    bool clusterViewWanted(uint32_t bit) const;
    // Returns the cluster view index (LLClusterFmt::kNone = not selected). skip_dyn: the view
    // skips moving casters (#567 split, #702 integration).
    uint32_t clusterCullView(VkCommandBuffer cmd, uint32_t role, uint32_t sub, uint32_t flags, const float view_proj[16],
                             const float origin[3], const float planes[6][4], const float cam[3], float px_scale,
                             uint32_t vw, uint32_t vh, bool skip_dyn = false);
    // #702 INTEGRATION: the cluster path drawn in the SECONDARY passes too. RenderVKClusterViewDraw
    // bit per pass: 1 sun cascades, 2 projector tiles, 4 planar mirror, 8 reflection-probe faces,
    // 16 GPU pick. Templates of those passes' pipelines (vert / frag / layout / vertex input
    // cleared), captured where the renderer builds them.
    bool clusterViewDrawWanted(uint32_t bit) const;
    GraphicsPipelineDesc mShadowDescCl, mProjDescCl, mCaptureDescCl;
    bool mShadowDescClValid = false, mProjDescClValid = false, mCaptureDescClValid = false;
    // Cluster LOD, rigged ALPHA-BLEND (LLClusterRenderEnv::alphaDesc ...): the legacy rigged alpha
    // pipelines' state as templates -- pass 2's blend (LIT_RIG_ALPHA), the alpha-blend depth
    // pre-pass, the cascades' translucency layer (depth, colour), the captures' rigged alpha.
    GraphicsPipelineDesc mAlphaDescCl, mAlphaDepthDescCl, mTransDescCl[2], mCaptureAlphaDescCl;
    bool mAlphaDescClValid = false, mAlphaDepthDescClValid = false, mTransDescClValid = false,
         mCaptureAlphaDescClValid = false;
    // Cluster LOD, CLPASS_FORWARD (LLClusterRenderEnv::forwardDesc): LIT_RIG's state, for the
    // main view's cluster draw in a FORWARD lit pass 1 (snapshots that are not deferred, ...).
    GraphicsPipelineDesc mFwdDescCl;
    bool mFwdDescClValid = false;
    // The main view's cluster records in the forward lit pass 1 (open scope; the world draw view
    // `view_idx` pushed). Restores the pass's sets / streams. True = recorded.
    bool clusterDrawForward(VkCommandBuffer cmd, uint32_t main_view, uint32_t view_idx);
    // This recording draws the main view's cluster records in the forward lit pass (the variant
    // the frame's lit FS needs exists): the forward-lit fallback no longer drops them.
    bool clusterForwardReady() const;
    // Snapshot diagnostics (#snapshot-rigged): per offscreen recording, the cluster material pass
    // the main view drew in (0 none, 1 G-buffer, 2 forward) and the owned rigged instances.
    uint32_t mClSnapPass = 0;
    // Pass 2's cluster alpha list (pre / post water) inside the open pass-2 scope; restores the
    // pass's sets / streams / view after. Returns true when drawn.
    bool clusterDrawAlpha(VkCommandBuffer cmd, uint32_t pass, uint32_t alpha_list, const VKDrawPush& view);
    // The main view's per-avatar rig-list bits (computeAvatarVis' per-pass visibility), set
    // before its cluster selection (clusterCullMain).
    void clusterSetRigLists(bool do_rigged);
    // This pass draws the shared rig list `list` (CLLIST_RIG_*) this frame: the rigged legacy
    // draws of the pass then drop the cluster path's avatars (u_sky_zenith.w = 1).
    bool clusterRigDraw(uint32_t bit, uint32_t pass) const;
    // #748 perf pass: a shadow map draws rigged casters this frame (the shadow rig list's consumer).
    bool clusterShadowMapsRig() const;
    // A static exclusion bitset for a secondary pass's cull (CULL_MODE_CLUSTER), when its own
    // cluster view `view` draws this frame in `pass`. False = the pass keeps its legacy statics.
    bool clusterStaticExclusion(uint32_t view, uint32_t pass, VKCullPush& cp, uint32_t& mode) const;
    // Inside a pass's rendering scope: the cluster draw of `view` / `list`, under the draw view
    // `vp` (pushed here: view_proj maps (world - origin)); then the pass's sets, the static
    // vertex / index streams and the given draw-view index are restored. True = drawn.
    // rig_culled (#748 perf pass, MDI tiers): clusterRigCullView ran for this view's rig list
    // right before this scope -- draw its culled copy.
    bool clusterDrawSecondary(VkCommandBuffer cmd, uint32_t pass, uint32_t view, uint32_t list, const VKDrawPush& vp,
                              uint32_t restore_view, bool rig_culled = false, bool alpha = false);
    // alpha: the list's rigged ALPHA-BLEND records (the ordered alpha list) instead of its opaque
    // buckets (pass CLPASS_SHADOW / PROJ: alpha-tested; CLPASS_PROBE: blended; CLPASS_TRANS_*).
    bool clusterDrawSecondaryIdx(VkCommandBuffer cmd, uint32_t pass, uint32_t view, uint32_t list, uint32_t view_idx,
                                 uint32_t restore_view, bool rig_culled = false, bool alpha = false);
    // #748 perf pass, MDI tiers: OUTSIDE a rendering scope, right before the scope that draws rig
    // list `list` (CLLIST_RIG_*) under draw view `view_idx` (param ring index): the per-view
    // rig-list cull (the select module's recordRigCull). True = pass rig_culled to the draw.
    bool clusterRigCullView(VkCommandBuffer cmd, uint32_t list, uint32_t view_idx);
    // ---- CLUSTER LOD LEGACY-SKIN SKIP (the last step of the cluster LOD avatar migration;
    // CLUSTER_LOD_CONTRACT.md §11, RenderVKClusterLegacySkinSkip) ----
    // Inside recordMainCull, after the main view's cluster selection and before the legacy
    // main cull: per switched avatar, the NEED MASK -- every consumer that would still read its
    // LEGACY pre-skinned vertices this frame (kClNeed* below). An empty mask sets kAvVisClSkip
    // (av_vis re-uploaded here): the main cull then gives its owned rigged instances no legacy
    // draw / pre-skin / weld, and RT keeps their switched groups cluster-only. Any bit set: the
    // legacy work runs exactly as before (never half, never neither).
    void clusterLegacySkipPlan(VkCommandBuffer cmd, bool do_rigged, uint32_t main_view);
    enum : uint32_t
    {
        kClNeedSun = 0,   // sun cascades without their rig list (ViewDraw bit 1 / pipelines / list)
        kClNeedProj,      // projector tiles without their rig list (bit 2)
        kClNeedMirror,    // dynamic mirror without the capture rig list (bit 4)
        kClNeedProbe,     // probe faces (CPU or GPU scheduled) without the capture rig list (bit 8)
        kClNeedPick,      // the GPU pick without its cluster pick (bit 16)
        kClNeedRt,        // RT without a confirmed cluster cut (fallback / transition / tier 0)
        kClNeedSel,       // the selection outline (a selected rigged face) / wireframe
        kClNeedGlobal,    // setting off, no rigged frame, no main selection / pre-skin
        kClNeedCount
    };
    uint32_t mClSkipAv = 0, mClSwitchedAv = 0;          // this frame: avatars skipped / switched
    uint32_t mClKeepWhy[kClNeedCount] = {};             // switched avatars kept legacy, per reason
    uint32_t mClRtNotConsumer = 0;                      // this frame: RT reads no legacy skinned vertex of a
                                                        // switched avatar (1 RT not tracing, 2 RT keeps them
                                                        // cluster-only: over-budget / pressure drop)
    // A skipped avatar relies on the capture rig list this frame: the captures draw it even in a
    // ray-query lit frame (plain lit FS, its shading there is approximate) -- never neither.
    bool     mClCapCommit = false;
    uint32_t mClSkipInst = 0, mClSkipVerts = 0;         // GPU (R_CLSKIP_*), read back kFrameSlots late
    // Avatars (joint-table slots) owning a selected rigged face (setSelection: the outline reads
    // their legacy vertices); All = one could not be resolved (no skin meta yet).
    uint32_t mSelRigAv[kAvatarSlotWords] = {};
    bool     mSelRigAvAll = false;
    // ---- OVERDRAW DIAGNOSTIC (RenderVKOverdrawStats) ----
    // Lit-FS invocation counter (draw set 0 binding 20; the FS atomics only when the
    // kOverdrawStats specialization constant is on) + a 3-deep host readback ring, and a
    // FRAGMENT_SHADER_INVOCATIONS pipeline-statistics query over the same draws as an
    // independent cross-check. Stats pipelines: [prepass off/on][static, static-ds,
    // rigged, rigged-ds], built on first enable.
    GpuBuffer        mOdCounter{};
    GpuBuffer        mOdRb[3]{};
    VkQueryPool      mOdQueryPool = VK_NULL_HANDLE;   // 3 pipeline-statistics queries (ring)
    bool             mOdQueryOk = false;              // pipelineStatisticsQuery supported
    bool             mOdAtomicOk = false;             // fragmentStoresAndAtomics supported
    bool             mOdQueryIssued[3] = {false, false, false};
    bool             mOdRbValid[3] = {false, false, false};
    uint32_t         mOdPx[3] = {0, 0, 0};            // screen pixels of that ring entry's frame
    uint64_t         mOdFrame = 0;
    VkPipeline       mOdPipe[2][4] = {};
    bool             mOdPipesBuilt = false;
    // Accumulated for the VKGpu log line (reset with the timers).
    double           mOdShadedAcc = 0.0, mOdStatAcc = 0.0, mOdPxAcc = 0.0, mOdPxStatAcc = 0.0;
    uint32_t         mOdFrames = 0, mOdStatFrames = 0;
    double           mOdNextLog = 0.0;
    bool             mOdLastPrepass = false;
    bool             buildOverdrawPipes();
    void             readOverdrawStats();
    std::string      overdrawStatsText();   // " overdraw[...]" and resets the accumulators
    // ---- LIT GUARDS (white / pink / cyan tile hunt; clean_draw_common.glsl lgFinite) ----
    // The deferred lighting compute counts its guard trips and scans the light / probe
    // cluster lists into words 4.. of mOdCounter (draw set 0 binding 20; words 0-3 are the
    // overdraw counter's): zeroed before the dispatch, copied after it into a 3-deep host
    // ring, harvested 3 frames later (readLitGuardStats). Logged once per new kind of trip
    // (WARN) and as a ~10 s summary while anything is non-zero (tag VKLitGuard).
    static constexpr uint32_t kOdCounterBytes = 256;   // 64 words: [0..3] overdraw, [4..37] lit guards
    static constexpr uint32_t kLgFirstWord = 4, kLgWords = 34;
    // PROBE TEXEL GUARD (clean_draw_common.glsl lgProbeTexel): lit-guard word indices.
    static constexpr uint32_t kLgPt = 13;                 // + LGPT_*: bad texels per store (4)
    static constexpr uint32_t kLgPtNonFinite = 17, kLgPtMax = 18, kLgPtMinInv = 19, kLgPtBadIn = 20;
    static constexpr uint32_t kLgPtSample = 21, kLgPtLive1 = 22, kLgPtSlots = 23;   // slots: 8 words
    // (#1049) RT REFLECTION HANG GUARD (clean_deferred_light.comp rtrStatsGuard, LG_RT_WORD): + 0 rays
    // the per-ray query budget cut, + 1 rays rtRaySane refused, + 2 walks over the candidate budget.
    static constexpr uint32_t kLgRt = 31;
    static_assert(kLgPtSlots + kPrbSlotCap / 32 == kLgRt, "lit guard words = clean_draw_common.glsl LGPT_WORD");
    static_assert(kLgRt + 3 == kLgWords, "the RT hang guard's words end the lit guard words");
    static_assert((kLgFirstWord + kLgWords) * 4 <= kOdCounterBytes, "mOdCounter holds every lit guard word");
    GpuBuffer        mLgRb[3]{};
    bool             mLgRbValid[3] = {false, false, false};
    uint64_t         mLgRbFrame[3] = {0, 0, 0};  // mLgFrame of the dispatch each ring entry holds
    uint64_t         mLgFrame = 0;
    bool             mLgActive = false;          // this dispatch zeroed the words (main view)
    uint64_t         mLgAcc[kLgWords] = {};      // summed counts (max words: the max, masks: OR)
    uint32_t         mLgFrames = 0;
    // CLUSTER SCAN SAMPLING (#831): the feature split runs lgClusterScan (words 6..12) as its own
    // small dispatch on every kLgScanEvery-th guarded frame only; the uber / combine / grid paths
    // still scan every frame. mLgScanNow = this recording scanned (recordLitGuardBegin sets it
    // true, the feature split overrides), latched per ring slot; the scan words' per-frame
    // averages divide by mLgScanFrames (the harvested frames that scanned).
    static constexpr uint32_t kLgScanEvery = 64;
    bool             mLgScanNow = true;
    bool             mLgRbScan[3] = {true, true, true};
    uint32_t         mLgScanFrames = 0;
    uint64_t         mLgSeen = 0;                // kinds already WARNed once (bit per word)
    double           mLgNextLog = 0.0;
    // The GPU slot table (mPrbSlots, 64 B / slot) copied beside the counters while probe
    // texels trip (mLgSlotsUntil), so the log can say what each bad slot holds (#478: the
    // CPU knows nothing about the slots otherwise).
    GpuBuffer        mLgSlotRb[3]{};
    bool             mLgSlotRbValid[3] = {false, false, false};
    double           mLgSlotsUntil = 0.0;
    std::vector<uint8_t> mLgSlotSnap;            // the last harvested snapshot (empty: none)
    // SELF-HEAL (readLitGuardStats -> recordPrbGpu): a slot whose stored cube held a bad
    // texel is cleared (recordProbeHealClear) and recaptured (clean_probe_gpu.comp chg_n.w).
    // Bounded: one slot per frame and kLgHealGap s apart; a slot that re-corrupts after its
    // heal is logged LOUDLY (that is the write side failing again) and, after
    // kLgHealMaxRecorrupt of those, left alone (logged) for kLgHealBackoff s.
    static constexpr double   kLgHealGap = 0.5, kLgHealSlotGap = 2.0, kLgHealBackoff = 60.0;
    static constexpr double   kLgHealRecorruptWindow = 30.0;
    static constexpr uint32_t kLgHealMaxRecorrupt = 3;
    struct LgHeal
    {
        uint32_t heals = 0, recorrupt = 0;
        uint64_t frame = 0;          // mLgFrame from which the last heal is in effect
        bool     armed = false;      // a heal is in effect: the next bad texel is a RE-corruption
        bool     pending = false;    // requested, not yet recorded (recordPrbGpu)
        double   at = -1e9, backoffUntil = 0.0, warnAt = -1e9;
        uint32_t key[2] = {0, 0};    // the slot's probe key at the heal (snapshot; 0: unknown)
    };
    std::vector<LgHeal> mLgHeal;                 // kPrbSlotCap (lazily)
    int32_t          mLgHealReq = -1;            // slot to heal in the next recordPrbGpu
    double           mLgHealNext = 0.0;
    uint32_t         mLgHealsTotal = 0;
    void             recordLitGuardBegin(VkCommandBuffer cmd);
    void             recordLitGuardEnd(VkCommandBuffer cmd);
    void             readLitGuardStats();
    void             litGuardProbeHeal(const uint32_t* w, uint64_t frame, double now);
    std::string      litGuardSlotText(uint32_t slot) const;
    void             recordProbeHealClear(VkCommandBuffer cmd, uint32_t slot);
    // ---- SHADER CLOCK (RenderVKShaderClockLighting; clean_shader_clock.glsl) ----
    // With VK_KHR_shader_clock enabled at startup, every lighting pipeline is built from its
    // *ClockSpv twin (vkDeferLightingSpv), which charges subgroup-clock cycles per stage and
    // sums them into mClkCounter (set 2 binding 7: [bucket][kernel][slot] x (lo, hi) words).
    // Zeroed before the lighting dispatches, copied after them into a 3-deep host ring
    // (beside the lit guards'), harvested 3 frames later; ~5 s summaries (tag VKShaderClock).
    static constexpr uint32_t kClkStages = 17, kClkSlots = 19, kClkKernels = 11, kClkBuckets = 16;   // = clean_shader_clock.glsl
    static constexpr uint32_t kClkWords = kClkBuckets * kClkKernels * kClkSlots * 2u;
    static constexpr uint32_t kClkSlotInv = 17, kClkSlotSurf = 18;
    bool             mClkOn = false;             // the session's lighting pipelines are the clock twins
    GpuBuffer        mClkCounter{};
    GpuBuffer        mClkRb[3]{};
    bool             mClkRbValid[3] = {false, false, false};
    uint64_t         mClkFrame = 0;
    bool             mClkActive = false;         // this recording zeroed the counters (main view)
    uint64_t         mClkAcc[kClkKernels][kClkSlots] = {};   // summed over the harvested frames
    uint32_t         mClkKernelFrames[kClkKernels] = {};     // ... frames each kernel ran in
    uint32_t         mClkFrames = 0;
    double           mClkNextLog = 0.0;
    void             initShaderClock();          // the counter buffer + set 2 binding 7 (init)
    void             recordShaderClockBegin(VkCommandBuffer cmd);
    void             recordShaderClockEnd(VkCommandBuffer cmd);
    void             readShaderClockStats();
    // HUD pass: the static opaque/mask/alpha states with BACK-FACE CULLING, like LL.
    // HUD scripts show/hide and flip panels by rotating prims so faces turn away;
    // without culling the turned-away faces keep drawing (and win the near-coplanar
    // depth fight in LL's compressed HUD depth range).
    VkPipeline       mHudDrawPipe = VK_NULL_HANDLE, mHudMaskPipe = VK_NULL_HANDLE, mHudAlphaPipe = VK_NULL_HANDLE;
    // <SS:Skoomastream> The same three with the HUD glow attachment (kHudGlowFormat), drawn
    // only in frames where a HUD face glows (SSHudGlow).
    VkPipeline       mHudGlowDrawPipe = VK_NULL_HANDLE, mHudGlowMaskPipe = VK_NULL_HANDLE, mHudGlowAlphaPipe = VK_NULL_HANDLE;

    // Water depth-fog: its own FS samples the solid-scene depth (pass-1 output) so
    // ONLY water statically references the depth sampler. Drawn in pass 2 over the
    // alpha bucket (discard separates it from regular alpha). Layout adds set 2 =
    // the depth combined-image-sampler; reuses the shared VS + draw sets 0/1.
    VkDescriptorSetLayout mDslWaterDepth = VK_NULL_HANDLE;
    VkDescriptorSet       mDsetWaterDepth = VK_NULL_HANDLE;
    VkPipelineLayout      mWaterLayout = VK_NULL_HANDLE;
    VkPipeline            mWaterPipe = VK_NULL_HANDLE; // alpha-blend, depth test/no-write, samples depth
    VkPipeline            mWaterExclPipe = VK_NULL_HANDLE; // exclusion mask: R8, depth test/no-write
    VkPipeline            mWaterCapPipe = VK_NULL_HANDLE;  // opaque capture water (probe faces + mirror, #36)
    // Their descs (modules nulled; static VS + water / capture-water FS) for the spec sets.
    GraphicsPipelineDesc  mWaterDesc, mWaterCapDesc;
    bool                  mWaterDescValid = false;
    VkSampler             mDepthSampler = VK_NULL_HANDLE;

    // Rigged pre-skin compute (skins bind-pose verts -> mSkinVbo). Reuses the draw
    // pipeline layout (mDrawLayout) + draw sets for the rigged draw itself.
    VkDescriptorSetLayout mDslPreskin = VK_NULL_HANDLE;
    VkDescriptorSet       mDsetPreskin = VK_NULL_HANDLE;
    VkPipelineLayout      mPreskinLayout = VK_NULL_HANDLE;
    VkPipeline            mPreskinPipe = VK_NULL_HANDLE;
    // GPU skin palettes (clean_skin_palette.comp): avatar joints (b0) + static skin
    // meta (b1) -> palette SSBO (b2). One workgroup per skin, before the pre-skin.
    VkDescriptorSetLayout mDslSkinPal = VK_NULL_HANDLE;
    VkDescriptorSet       mDsetSkinPal = VK_NULL_HANDLE;
    VkPipelineLayout      mSkinPalLayout = VK_NULL_HANDLE;
    VkPipeline            mSkinPalPipe = VK_NULL_HANDLE;

    // RIGGED VERTEX WELDING (#574, RenderVKRigWeld; llcleanrigweldvk.cpp +
    // clean_rig_weld.comp): after the pre-skin, coincident open-boundary vertices of one
    // avatar's meshes (paired in bind space, flagged at ingest) get one position and
    // one normal. Own set / pool (mirrors the pre-skin buffers + the weld buffer),
    // rewritten after a device wait when one of those buffers was re-created. The weld
    // buffer exists only while the setting is on (cells + entries + results).
    VkDescriptorSetLayout mDslRigWeld = VK_NULL_HANDLE;
    VkDescPool            mRigWeldPool;
    VkDescriptorSet       mDsetRigWeld = VK_NULL_HANDLE;
    VkPipelineLayout      mRigWeldLayout = VK_NULL_HANDLE;
    VkPipeline            mRigWeldPipe = VK_NULL_HANDLE;
    bool                  mRigWeldInitFailed = false;
    GpuBuffer             mRigWeldBuf{};
    uint32_t              mRigWeldCap = 0, mRigWeldCells = 0;   // entries, cells (power of two)
    static constexpr uint32_t kRigWeldBindings = 13;
    VkBuffer              mRigWeldBound[kRigWeldBindings] = {};        // handles the set was written with
    uint64_t              mRigWeldBoundSerial[kRigWeldBindings] = {};  // ...and their GpuBuffer::serial (handles get reused)
    // Stats (live / cap / peak / dropped): the weld buffer's 2 header words copied to a
    // host-visible ring, read 3 frames later, logged every 30 s while on ("VKRigWeld").
    // Mode 1 (#641) also copies the pair pool's per-avatar headers + counters there.
    GpuBuffer             mRigWeldRb{};
    uint32_t              mRigWeldRbFrame = 0;
    bool                  mRigWeldRbValid[3] = {false, false, false};
    uint32_t              mRigWeldLive = 0, mRigWeldPeak = 0, mRigWeldDropped = 0;
    double                mRigWeldNextLog = 0.0;
    // #641 REST-POSE PAIRS (RenderVKRigWeldMode 1): weld pairs decided once per avatar at
    // its rest pose (clean_rig_weld.comp modes 3..8). mRigWeldPairBuf = the persistent pair
    // pool (per-avatar headers + regions on a CPU free list), mRigWeldUp = the host upload
    // ring (segment table + rest joint rows). CPU bookkeeping in RigWeldPairs
    // (llcleanrigweldvk.cpp). mRigWeldDiscNext: avatars discovered NEXT frame -- their
    // skin-cache items are forced to re-skin then (avatarPoseDirtyBit), so the discovery
    // sees every mesh of the avatar in the work list.
    struct RigWeldPairs;
    RigWeldPairs*         mRigWeldPairs = nullptr;
    GpuBuffer             mRigWeldPairBuf{};
    GpuBuffer             mRigWeldUp{};
    uint8_t               mRigWeldDiscNext[kAvatarSlotCap] = {};
    bool initRigWeld();
    void destroyRigWeld();
    void releaseRigWeldPairs(bool wait);       // frees the pool / upload ring + resets the CPU lists
    void recordRigWeld(VkCommandBuffer cmd);   // after the pre-skin dispatch, same command buffer
    void recordRigWeldSearch(VkCommandBuffer cmd);   // mode 0: per-frame bind-space search (#574)
    void recordRigWeldPairs(VkCommandBuffer cmd);    // mode 1: rest-pose pairs (#641)
    // #641 topology signature, fed by the rigged demand rescan (every rigged instance at the
    // level the cull picks): a change re-discovers the avatar's pairs.
    void rigWeldTopoBegin();
    void rigWeldTopoNote(uint32_t av, uint32_t inst, uint32_t skin, uint32_t slot, uint32_t vert_count);
    void rigWeldTopoEnd();

    // Skinned-vertex output (same layout/size as mVbo; pre-skin writes world-space
    // skinned verts here at each slot's base_vertex, the rigged draw reads them).
    GpuBuffer mSkinVbo{};
    // Rigged-only skin weight arena (10 B records, #582; pre-skin SSBO input).
    GpuBuffer mWeightSSBO{};
    // Bone palettes (device-local, written ONLY by the palette compute) + pre-skin work
    // list (written by the cull).
    GpuBuffer mSkinPaletteSSBO{}, mWorkSSBO{};
    // Static per-skin meta (LLCleanSkinStore::SkinMeta[]) the palette compute reads,
    // streamed only for entries the store dirtied (mSkinMetaRuns) or in full after a grow.
    GpuBuffer mSkinMetaSSBO{}, mSkinMetaStg[2]{};
    VkDeviceSize mSkinMetaCap = 0;
    std::vector<uint8_t> mCpuSkinMeta;
    std::vector<std::pair<uint32_t, uint32_t>> mSkinMetaRuns;
    bool mSkinMetaFull = true;
    uint32_t mAvatarJointCount = 0;      // live AvatarJoints entries streamed this frame
    float    mPalEye[3] = {0.f, 0.f, 0.f};   // eye the palettes are relative to (this frame)
    // Debug compare (RenderVKSkinPaletteCompare): CPU reference palettes for a sample of
    // skins + a readback of the GPU result from the same frame, compared once it lands.
    GpuBuffer mPalReadback{};
    std::vector<float> mPalCmpCPU;
    uint32_t mPalCmpSkins = 0;
    uint32_t mPalCmpFrame = 0;
    bool     mPalCmpPending = false;
    bool     mPalCmpRecord = false;      // copy the palette into mPalReadback this frame
    // GPU-driven rigged: {dispatch_x (= work count), y, z, skin_vert_used}, written by
    // the cull, consumed by vkCmdDispatchIndirect for the pre-skin. Zeroed per frame.
    GpuBuffer mRigAlloc{};
    // Rigid attachments: per-avatar joint WORLD matrices (AvatarJoints[], indexed by
    // the attachment's skin_id), streamed every frame. Draw set 0 binding 2.
    GpuBuffer mAvatarJointsSSBO{}, mAvatarJointsStg[2]{};
    VkDeviceSize mAvatarJointsCap = 0;
    std::vector<uint8_t> mCpuAvatarJoints;
    // C7 (RenderVKJointRowStream): with GPU animation, clean_anim.comp reads the CPU joint
    // table only for mode-0 (CPU-evaluated) slots. mCpuAvatarJoints is then the MIRROR of
    // the GPU table: only this frame's mode-0 rows are copied from the store, and only the
    // ones that changed are streamed (mJointRowRuns, appended until streamed). Full copy +
    // full stream when off, without GPU animation (the CPU table IS the joint table),
    // and whenever the GPU buffer was (re)created.
    std::vector<std::pair<uint32_t, uint32_t>> mJointRowRuns;
    std::vector<uint8_t> mJointRowWant;          // per slot: mode 0 this frame
    bool      mJointRowsFull = true;             // next stream: the whole table
    bool      mJointRowsMode = false;            // last frame used the row mode
    VkBuffer  mJointRowsBuf = VK_NULL_HANDLE;    // the GPU table the mirror describes
    // VK GPU ANIMATION (clean_anim.comp, program built by LLCleanAnimStore). The joint
    // world table the palette pass (b0) and the rigid-attachment draw (set 0 b2) read is
    // mAnimJointsOut, written ONLY by the anim compute: GPU-evaluated avatars are
    // animated there, CPU-evaluated ones (self, UI) are copied through from
    // mAvatarJointsSSBO (the CPU-streamed input). mAnimLocal keeps every GPU avatar's
    // local pose between frames; mAnimLocalOut -> mAnimLocalRb[ring] carries the joints the
    // CPU requested (sparse: local pose + root-relative world) back to its LLJoints RING
    // frames later. Falls back to the CPU table if the pipeline
    // could not be created.
    VkDescriptorSetLayout mDslAnim = VK_NULL_HANDLE;
    VkDescPool            mAnimPool;   // layout-sized + accounted (vk_descpool.h)
    VkDescriptorSet       mDsetAnim = VK_NULL_HANDLE;
    VkPipelineLayout      mAnimLayout = VK_NULL_HANDLE;
    VkPipeline            mAnimPipe = VK_NULL_HANDLE;
    GpuBuffer mAnimWords{}, mAnimWordsStg[2]{};
    VkDeviceSize mAnimWordsCap = 0;
    GpuBuffer mAnimTopo{}, mAnimTopoStg[2]{};
    VkDeviceSize mAnimTopoCap = 0;
    GpuBuffer mAnimDesc{}, mAnimDescStg[2]{};
    VkDeviceSize mAnimDescCap = 0;
    GpuBuffer mAnimKeyT{}, mAnimKeyTStg[2]{};
    VkDeviceSize mAnimKeyTCap = 0;
    GpuBuffer mAnimKeyV{}, mAnimKeyVStg[2]{};
    VkDeviceSize mAnimKeyVCap = 0;
    GpuBuffer mAnimLocal{};              // [slot][216][pos,rot,scale] vec4, GPU only
    GpuBuffer mAnimJointsOut{};          // AvatarJoints[slot], GPU only
    GpuBuffer mAnimLocalOut{};           // sparse readback source: [record][5] vec4
    uint32_t  mAnimLocalOutCap = 0;      // records (one requested joint each)
    GpuBuffer mAnimLocalRb[3]{};         // host-visible readback ring
    uint32_t  mAnimLocalRbCap[3] = {0, 0, 0};
    bool      mAnimRbValid[3] = {false, false, false};
    GpuBuffer mAnimCmpRb{};              // RenderVKAnimCompare: GPU rows of the compared slots
    std::vector<uint32_t> mAnimCmpSlots; // slots to copy into mAnimCmpRb this frame
    uint32_t  mAnimCmpRecFrame = 0;
    bool      mAnimCmpInFlight = false;
    uint32_t  mAnimEntryCount = 0;       // anim workgroups this frame (0 => no dispatch)
    uint32_t  mAnimRbCount = 0;          // entries with a local-pose readback this frame
    uint32_t  mAnimRing = 0;             // readback ring slot this frame
    bool      mAnimTopoUpload = false, mAnimArenaFull = false;
    std::vector<std::pair<uint32_t, uint32_t>> mAnimDescRuns, mAnimKeyRuns;   // element runs
    void buildAnimFrame();
    void recordAnim(VkCommandBuffer cmd);
    void writeAnimDescriptors();
    const GpuBuffer& jointTable() const { return (mAnimPipe && mAnimJointsOut.buffer) ? mAnimJointsOut : mAvatarJointsSSBO; }
    uint32_t  mSkinVertCap = 0;      // mSkinVbo capacity in vertices (per-frame ranges + skin cache)
    // REDUCED-TIER SKIN CACHE (#31). mSkinVbo = [0, mSkinDynCap) per-frame ranges (the
    // cull's bump allocator, reset every frame) + [mSkinDynCap, mSkinVertCap) the
    // persistent cache arena of reduced no-shadow avatars (pelvis-local vertices,
    // re-skinned only when the pose changes). mRigCache: kRigCacheWords per instance
    // (cache range, key, epoch, per-frame state, the draw's source vertex base #616, and
    // the pre-skin's posed-box centre + its range key + its extent: the rigged alpha sort's
    // position and size bias, #835) --
    // clean_cull.comp RigCache.
    uint32_t  mSkinDynCap = 0, mSkinCacheCap = 0;
    static constexpr uint32_t kRigCacheWords = 16;   // = clean_cull.comp / clean_skinvert.glsl RC_WORDS
    GpuBuffer mRigCache{};
    uint32_t  mRigCacheInstCap = 0;  // instances mRigCache holds
    double   mRigCacheRetryAt = 0.0;   // #697: a refused skin-cache grow backs off until then
    bool     mRigCacheShort = false;   // #697: a grow was refused (the cache holds fewer ids than wanted)
    uint32_t  mRigCacheEpoch = 1;    // bumped on reset (never 0: a zeroed entry is invalid)
    bool      mRigCacheReset = true; // record: zero the bump pointer (+ mRigCacheClear: the entries)
    bool      mRigCacheClear = true;
    uint32_t  mRigCacheResetFrame = 0;
    uint32_t  mAnimBuiltFrame = 0xFFFFFFFFu;   // gFrameCount of the last LLCleanAnimStore::buildFrame
    uint8_t   mRigCacheSlotDirty[kAvatarSlotCap] = {};    // skin meta changed this frame (per joint-table slot)
    void      resetRigCache(bool clear);
    uint32_t  avatarPoseDirtyBit(uint32_t slot, uint32_t tier) const;
    uint32_t  mSkinPaletteCap = 0;   // # SkinBlocks the palette buffer holds
    uint32_t  mWorkCap = 0;          // # WorkItems the work buffer holds
    uint32_t  mRiggedWorkCount = 0;  // rigged work items THIS frame (0 => skip)
    uint32_t  mSkinCount = 0;        // live skin count this frame (u_skin_count)

    // Sky gradient (full-screen triangle, no vertex buffer).
    VkPipelineLayout mSkyLayout = VK_NULL_HANDLE;
    VkPipeline       mSkyPipe = VK_NULL_HANDLE;
    VkPipeline       mSkyDepthPipe = VK_NULL_HANDLE;   // F9: EQUAL-at-far against the pre-pass depth

    // CPU-side staging arrays (mirrored from the stores or the proto scene).
    // Raw byte blobs so this header need not include the store structs.
    std::vector<uint8_t> mCpuInstances;  // Instance[]  (224 B each) -- persistent incremental mirror
    std::vector<std::pair<uint32_t, uint32_t>> mInstRuns;   // dirty id runs pending upload
    bool mInstFullUpload = true;         // instance SSBO (re)allocated: stream it all once
    // #568 RENDER-WORLD FRAME (LLCleanInstanceStore::rowAgentSpace). mCpuInstances holds the
    // store frame (agent + d); the GPU rows stay agent space: the instance stream writes
    // row - mInstD into staging, and an agent-origin shift (region crossing) moves the
    // GPU-resident rows with one compute pass (clean_inst_shift.comp) instead of a
    // dirty-all + full re-stream. Agent-space caches are re-based by the same offset.
    float mInstD[3] = {0.f, 0.f, 0.f};          // the mirror rows' frame offset (last copyDirtyVK)
    float mInstSyncShift[3] = {0.f, 0.f, 0.f};  // shift reported by copyDirtyVK (fill, maybe a worker),
                                                 // applied to the caches in syncFromStores
    float mGpuInstShift[3] = {0.f, 0.f, 0.f};   // shift the GPU rows still need (recordWorld)
    bool  mGpuInstShiftPending = false;
    VkPipelineLayout mInstShiftLayout = VK_NULL_HANDLE;
    VkPipeline       mInstShiftPipe = VK_NULL_HANDLE;
    // copyDirtyVK + the frame sync above (both fill paths).
    void copyInstancesFromStore();
    // Main thread, before setShadows / setTemporal: re-base every agent-space cache by the
    // agent-origin shift `off` (new agent = old agent + off) -- cached shadow cascades,
    // projector tiles, TAA history, skin history, the RT TLAS camera -- so a crossing keeps
    // them instead of invalidating them.
    void applyOriginRebase(const float off[3]);
    // recordWorld, before the previous-instance copy and the instance stream: the GPU pass.
    void recordInstShift(VkCommandBuffer cmd);
    uint32_t mLastDemandSkins = 0, mLastRigItems = 0, mLastRigVerts = 0;   // cached rigged demand
    uint32_t mLastRigCacheVerts = 0;   // reduced no-shadow share (skin cache arena)
    std::vector<uint8_t> mCpuSlots;       // SlotInfo[]  ( 32 B each)
    std::vector<uint8_t> mCpuGroups;      // LodGroup[]  ( 16 B each)
    std::vector<uint8_t> mCpuMaterials;   // Material[]  ( 96 B each)
    // Previous frame's RAW (pre-mutation) material bytes, for a stable frame-to-frame
    // change test. uploadRealMaterialTextures() rewrites bindless indices INTO
    // mCpuMaterials, so comparing a fresh raw fill against the mutated copy was always
    // unequal -> every-frame full rebuild. Compare raw-to-raw against this instead.
    std::vector<uint8_t> mPrevRawMaterials;
    // Cached RESOLVED material bytes (bindless indices rewritten by
    // uploadRealMaterialTextures). fillFromCleanStores overwrites mCpuMaterials with
    // RAW bytes every frame; on frames where the raw UUIDs didn't change we restore
    // this cache so buildBuffers uploads resolved indices instead of raw (which would
    // fall back to white -> textures flash in/out on instance-only-changed frames).
    std::vector<uint8_t> mResolvedMaterials;
    // (#584: no mCpuVerts / mCpuIndices / mCpuWeights -- see mGeomVertEnd / geomSlotSource.)
    // Geometry-generation gate for fillFromCleanStores: skip the per-frame mega-
    // geometry repack while the mesh store's generation + slot count are unchanged.
    uint64_t mLastGeomGen = ~0ull;        // force a repack on the first real frame
    uint32_t mLastSlotN   = 0;
    // C3: bumped by every full geometry repack (slots move without rtSlotChanged when
    // their extent is unchanged) and every skin-meta change -- part of the RT rigged
    // plan's reuse key (LLCleanRTVK::recordBuild).
    uint32_t mGeomPackGen = 0;
    uint32_t mSkinMetaGen = 0;
    bool mBuffersDirty = true;            // CPU arrays changed -> re-upload
    bool mGeometryBuilt = false;          // proto geometry built once

    // Fills the CPU arrays. Returns true if the clean stores supplied data.
    bool fillFromCleanStores();
    void fillPrototypeScene();

    // Upload the real materials' base-color textures to VK images and build the
    // bindless descriptor array, remapping each material's base_color_tex to a
    // VK texture index. Called from init()/syncFromStores() when real materials are
    // present. Returns the number of textures now resident. Missing/not-yet-resident
    // textures fall back to a neutral tint (index 0). Keyed by texture UUID so the
    // same texture shared across materials uploads once. (Body compiled only under
    // LL_HAS_CLEAN_PIPELINE; declared unconditionally so the class layout is
    // IDENTICAL in every TU -- see the member note below.)
    // full = the material resolver + the residency lap (the classic per-call pass);
    // !full = the residency lap only (C2, runTexResidencyLap): the per-frame texture
    // clock, pressure, the every-4th-pass liveness / slot demand scan, a small settled
    // trickle, the throttled fixed sets, residency up/downgrades, batch hand-off and
    // eviction -- without the patch, priority pass, new/media/unsettled walk.
    uint32_t uploadRealMaterialTextures(bool full = true);
    uint32_t runTexResidencyLap() { return uploadRealMaterialTextures(false); }
    // C2: the MATERIAL side still has work (budget-starved resolves, an unfinished
    // unsettled walk, pending media, waiting self/HUD materials) -> run a full pass next
    // frame. Residency progress no longer keeps the full pass hot (RenderVKTexResidencyLap).
    bool mTexMatPending = false;
    // VRAM pressure stopgap (see uploadRealMaterialTextures): pressure mode pauses
    // non-priority texture uploads and evicts LRU unreferenced slots.
    void measureTexVram();
    bool     mTexPressure = false;
    // TEXTURE STEP (#282): ONE owner (texStepTick) sets the texture pool's reduction from
    // the residency lap's per-step sums, whether the trigger is the texture budget
    // (RenderVKTexBudgetMB / 60% of the heap) or pressure mode. mTexStep 0..kTexPressMax are
    // the demand-sorted levels at bias 0; above that, all levels plus global bias
    // mTexStep - kTexPressMax (the last texture resort). mTexPressLvl / mTexBias are derived
    // (setTexStep). Raised at once to what fits; lowered one useful step at a time
    // (2 laps eligible, >= 5 s apart -- 10+ s with the back-off during a pressure episode),
    // never in pressure or while RT is held reduced, only when the step fits under 90% of
    // the texture budget and what it re-adds fits under 80% of the heap.
    static constexpr uint32_t kTexPressMax = 8;   // 2 + far levels (classes up to 4096 px)
    static constexpr uint32_t kTexStepMax = kTexPressMax + 7;
    uint32_t mTexStep = 0;
    uint64_t mTexStepBytes[kTexStepMax + 1] = {0};   // live non-pinned target bytes per step (last lap)
    uint64_t mLapStep[kTexStepMax + 1] = {0};
    uint64_t mTexFixedBytes = 0;         // pinned / not-live bytes (last lap)
    bool     mTexStepLowPending = false; // the last lap wanted a lower step
    double   mTexStepLastLower = -1e9;   // wall s of the last lowering
    void     setTexStep(uint32_t s, const char* trigger);
    void     texStepTick(uint64_t budget, uint32_t cap_bias);
    // ONE VRAM BUDGET + SOFT LADDER (#405 part A; LLVramBudgetVK in llvrambudgetvk.h owns
    // the budget, the rung list, pacing and recovery). budgetTick runs on every texture
    // pass: measure, mBudget.update(), then applyBudgetCaps() turns the engaged rungs into
    // EFFECTIVE values below the user's settings -- never a setting write:
    //   texture rungs  -> a floor on mTexStep (texStepTick never lowers below it; the
    //                     queued downgrades already run largest-savings first);
    //   probes         -> probeWanted() (resolution, then count) -> live reallocation;
    //   mesh detail    -> vramMeshLodScale() / vramAvatarLodScale() / vramTerrainLodScale():
    //                     the LL side multiplies its EFFECTIVE LOD factors (sLODFactor etc.,
    //                     applyVramDetailCaps in llviewercontrol.cpp); the geometry store
    //                     frees the levels no instance can select any more (pressure GC,
    //                     LLCleanMeshAsset::setVramPressureTrim) and the pack compacts
    //                     and shrinks sooner (mGeomTrim);
    //   far shadows    -> vramShadowRangeScale() (GL-side cascade range) + rtRadiusEffective();
    //   far geometry   -> vramDrawDistScale(): EFFECTIVE draw distance (gAgentCamera.
    //                     mDrawDistance, interest list, VK cull far); cacheable objects past
    //                     it are killed back into the region object cache (their instances,
    //                     geometry and textures follow) and re-ingest as it is restored;
    //   RT             -> LLCleanRTVK BLAS cap / rigged drop, RT mask scale cap, RT range;
    //   mirrors        -> mMirrorOn false + targets released (ensureMirror(1, 1));
    //   projector / sun shadows -> off (sun: cascades shrink to the minimum);
    //   render scale   -> renderScaleWanted() cap.
    // Restored automatically in reverse order (LLVramBudgetVK). The texture "pressure mode"
    // (mTexPressure: non-priority uploads paused) is the ladder's FAST mode only (usage
    // inside the 10% reserve, or an allocation failed), not every soft step.
    // HARD STEP (budgetHardStep): only when no rung is left AND allocations fail / usage
    // stays over the capacity: writes a real setting (RT shadows -> maps, then probe
    // resolution / count, then shadow map size), latched, one notification each.
    LLVramBudgetVK mBudget;
    LLVramShareVK  mShare;               // #403: smoothed OS budget + share next to other viewers
    uint64_t       mShareCap = 0;        // mShare.cap() of the last measurement (0 = none)
    LLVramBudgetVK::Caps mBudgetCaps;    // what applyBudgetCaps last applied
    bool     mGeomTrim = false;          // mesh-detail / far-geometry rung engaged: compact + shrink geometry sooner
    bool     mTightHeadroom = false;     // ensureCap: 1.25x (geometry buffers under mGeomTrim); wins over mGeomHeadroom
    bool     mGeomHeadroom = false;      // ensureCap: 2x (geometry buffers); otherwise tables grow 1.5x
    double   mGeomTrimRepackAt = 0.0;    // wall s of the last trim-triggered repack
    double   mGeomLiveCompactAt = 0.0;   // #737 wall s of the last liveness compaction
    bool     mGeomLiveCompact = false;   // #737 next compaction sizes buffers at 1.5x need
    // 2026-10-14 VRAM: the packed buffers held > 1.5x their packed need + 64 MB for 60 s (the 2x
    // grow headroom left over after a peak, garbage too small for #737): a compaction at 1.25x.
    double   mGeomOversizeSince = 0.0;   // wall s the oversize was first seen (0 = not oversized)
    bool     mGeomShrinkTight = false;   // next compaction sizes buffers at 1.25x need
    uint32_t mHardStage = 0;             // next hard-step action (0 probes, 1 shadow size, 2 RT sun / reflections, 3 RT projectors, 4 none left)
    bool     mHardFloorWarned = false;   // "nothing left to give" logged
    double   mFitRetryNext = 0.0;        // wall s of the next #226 fit-cap re-try check
    void     budgetTick(bool new_fail);
    void     applyBudgetCaps(const LLVramBudgetVK::Caps& prev, const LLVramBudgetVK::Caps& cur);
    bool     budgetRungUseful(uint32_t rung);
    uint32_t texLadderCapStep() const;   // deepest texture step the ladder may hold
    uint32_t texLadderFloor() const;     // the floor the engaged texture rungs hold
    bool     mPoolsTrimmed = false;      // idle encoder pools trimmed (once per pressure episode)
    bool     budgetHardStep();
    void     fitRetryTick(double now);
    std::string fitNotesText() const;
    static uint32_t texPressExtra(uint32_t demand, uint32_t lvl);
    // Mip levels the residency target drops from an sw x sh source so its top level still
    // covers `demand` texels at mip bias `bias` (floored for live-but-unseen; full demand =
    // 0). THE residency target: the scan, debug views 23 / 24 and the inspector (#685).
    static uint32_t texTargetSkip(uint32_t sw, uint32_t sh, uint32_t demand, uint32_t bias);
    // The global bias + this demand's extra at the texture pressure level (#282).
    uint32_t texBiasFor(uint32_t demand) const { return mTexBias + texPressExtra(demand, mTexPressLvl); }
    static const char* texPressName(uint32_t lvl);
    uint32_t mTexPressLvl = 0;           // demand-sorted texture level (derived from mTexStep)
    bool     mTexCompTipLogged = false;  // compression advice logged (once per session)
    // #226 FIT CAPS: a setting-driven allocation that does not fit (pre-check) or fails is
    // applied at the largest size that fits; the SETTING keeps the user's value. Keyed by
    // the request it answered: a different request (setting or ladder change) re-checks;
    // fitRetryTick clears a cap once the request fits again (pre-checked, no realloc
    // until it does). Shown in the readout / log (fitNotesText).
    // deferred: the cap is a re-allocation the driver had no live room for (vramPick's
    // gate) -- fitRetryTick re-checks it every 30 s even while the ladder is engaged.
    struct FitCap { bool valid = false, deferred = false; uint32_t key_a = 0, key_b = 0, got_a = 0, got_b = 0; };
    FitCap   mProbeFit;                  // a/b = resolution / count
    FitCap   mShadowFit;                 // a = cascade size
    FitCap   mMaskFit;                   // a = RT mask scale in 1/100
    std::string mProjFitNote;            // projector atlas (startup only)
    void     noteVramFit(const std::string& text);
    // Effective sizes (setting -> ladder caps -> fit cap).
    void     probeWanted(const LLVramBudgetVK::Caps& c, uint32_t& r, uint32_t& n, bool with_fit = true) const;
    uint32_t effectiveShadowRes() const;
    float    rtRadiusEffective(float setting) const;
    // RT over its own budget (#227 replacement): shadows fall back to the maps for a
    // while, settings untouched. Held >= mRtFallbackHold s (doubled when it trips again
    // within 2 min of a lift, up to 10 min).
    bool     mRtFallback = false;
    double   mRtFallbackSince = 0.0;
    double   mRtFallbackLift = -1.0e9;
    double   mRtFallbackHold = 60.0;
    double   mMirrorLastOn = 0.0;        // wall s a mirror was last rendered (targets freed after 30 s)
    // #422 AUTO-TUNE ("Keep my FPS", LLVKAutoTune): its temporary caps, merged with the
    // VRAM ladder's caps (most restrictive wins) into mBudgetCaps -- the one set every clamp
    // point reads (probes, mirrors, shadows, render scale, LOD / draw distance). Never a
    // setting write; the ladder's own state (mBudget, rung usefulness, recovery, readouts)
    // keeps seeing only its own caps.
    LLVKPerfCaps mPerfCaps;
    bool     mPerfCapsDirty = false;     // set by setPerfCaps, applied by budgetTick
    LLVramBudgetVK::Caps budgetCapsWithPerf() const;
public:
    // Far-shadow ladder rung (#405 step 5): scale for the GL-side cascade range (1 = none).
    float    vramShadowRangeScale() const { return mBudgetCaps.farShadowsOff ? 0.5f : 1.f; }
    // Mesh detail / far geometry rungs (#405 steps 4 and 6): multipliers the LL side
    // applies to its EFFECTIVE LOD factors and draw distance (1 = none). Settings untouched.
    float    vramMeshLodScale() const { return mBudgetCaps.meshLodScale; }
    float    vramAvatarLodScale() const { return mBudgetCaps.avatarLodScale; }
    float    vramTerrainLodScale() const { return mBudgetCaps.terrainLodScale; }
    float    vramDrawDistScale() const { return mBudgetCaps.drawDistScale; }
    // #422 AUTO-TUNE: set the continuous mode's caps (merged with the VRAM ladder's, applied
    // by the next budgetTick like a ladder move), and the tuner's measurement sample: the always-on sampled GPU span and
    // rigged time, the rigged controller's perf state, per-avatar visible skinned vertices,
    // and what the VRAM ladder holds. Main thread, outside recording.
    void     setPerfCaps(const LLVKPerfCaps& caps);
    void     autoTuneSample(LLVKAutoTuneSample& out) const;
    // Sun cascade size actually allocated (0 = none): the GL side snaps cascades to it.
    uint32_t shadowMapResolution() const { return mShadowMap.image ? mShadowRes : 0u; }
private:
    // Notices (notifications.xml VKGraphicsAutoAdjusted): everything decided within 2 s is
    // merged into one notification, posted from the idle loop (never during recording).
    std::string vramReasonText() const;
    void     notifySettingsLowered(const std::string& reason, const std::string& changes);
    void     notifyAutoReduced(const std::string& reason, const std::string& changes);
    double   mRtOverSince = 0.0;         // wall s RT went over its budget (0 = not over)
    bool     mRigNotified = false;       // the VRAM ladder dropped avatars from RT: logged (once per session)
    double   mRtMaskWantedAt = -1e9;     // wall s the RT shadow mask was last needed (free after 10 s)
    uint64_t rtMaskBytes() const;        // RT shadow mask images (all of them)
    uint32_t mTexAllocFailSeen = 0;      // last seen VkDeviceContext::allocFailures()
    bool     mVramOomKick = false;       // #595: the relief ran (out of memory): the ladder answers at the next pass
    uint32_t mTexPressureUntil = 0;      // mTexPass until which an OOM holds pressure mode
    uint32_t mTexVramHeap = 0;
    double   mTexVramUsage = 0.0, mTexVramBudget = 0.0;   // budget = the CAPACITY (min(user cap, OS[, debug]))
    uint64_t mTexVramOsBudget = 0;       // the OS per-process budget of that heap (raw)
    uint32_t mPrioMatCursor = 0;         // self/HUD priority material rotation
    // SETTLED MATERIALS. A live material whose four textures all resolved to resident
    // slots (or are missing assets) needs no per-frame re-resolve: slot sizing is the
    // residency pass's job and slots keep their index across resizes. Settled ones are
    // skipped by the walk (a small rotating trickle re-validates them); a material is
    // unsettled when it goes dead, and media materials never settle.
    std::vector<uint8_t> mMatSettled;
    uint32_t mSettleCursor = 0;
    // Materials the rotating resolve window still has to visit: live-but-unsettled, or
    // dead with handles left to clear. Rebuilt on the every-4th-pass material scan and
    // appended to when a material unsettles in between; the window walks THIS list, not
    // every material (a settled static scene walked ~13k materials per frame).
    std::vector<uint32_t> mUnsettled;
    std::vector<uint8_t>  mInUnsettled;
    uint32_t              mUnsettledCursor = 0;
    // LL texture lookups for still-resolving textures: the std::map<LLTextureKey> find
    // behind getFetchedTexture cost ~3.5% of a crowded frame at one lookup per texture
    // per material per walk. Keyed by the unsalted texture key; holds a reference so a
    // cached pointer can't dangle; validated (id + still in LL's image list) on hit and
    // pruned when unused.
    struct FetchCacheEntry { LLPointer<LLRefCount> hold; LLViewerFetchedTexture* tex = nullptr; uint32_t pass = 0; };
    std::unordered_map<uint64_t, FetchCacheEntry> mFetchCache;

    // VISIBILITY-DRIVEN TEXTURE RESIDENCY. The world cull writes a per-material texel
    // demand (clean_cull.comp binding 20); it is copied to a 3-deep host-visible ring
    // (frame F writes [F%3]; the sync for frame F reads the same entry, written by
    // F-3 whose fence acquire(F-1) already waited). Each texture keeps the mip chain
    // its demand needs; the texture step (demand-sorted levels, then a global mip bias)
    // fits the sum to RenderVKTexBudgetMB.
    GpuBuffer mTexDemand{};
    GpuBuffer mTexDemandRb[3]{};
    uint32_t  mTexDemandCap = 0;         // entries (materials)
    uint32_t  mTexDemandFrame = 0;       // recordWorld count (ring index)
    uint32_t  mTexBias = 0;              // global mip bias for non-priority textures (derived from mTexStep)
    uint32_t  mTexBiasLowPasses = 0;     // consecutive laps a lower texture step was eligible
    uint32_t  mTexPressureBumpPass = 0;  // last pass pressure mode raised the bias
    bool      mTexHadFocus = true;       // app focus last pass (alt-tab recovery)
    bool      mTexFastRecover = false;   // regained focus: lower the bias straight to fit
    static constexpr double   kTexHoldSec    = 2.0;   // demand hold before it may fall
    static constexpr uint32_t kTexFloorDim   = 64;    // live-but-unseen floor (px)
    // ---- TEXTURE BLOCK COMPRESSION (#263; RenderVKTextureCompression) ----
    // 0 off, 1 high quality (BC7 HQ), 2 balanced (BC7 fast), 3 max savings (BC1/BC3),
    // 4 adaptive (per texture: BC1/BC3 where it measured good, else BC7 HQ); alpha-less
    // normal maps go BC5 at 1-4. A level change re-encodes resident images progressively
    // through the residency pass (mResReenc), no restart.
    LLCleanTexEncVK* mTexEnc = nullptr;
    uint32_t mTexCompLevel = 0;          // applied level (0 when BC is unsupported)
    uint32_t mTexCompLevelSeen = 0xFFFFFFFFu;
    uint32_t mTexSmallHQDim = 128;       // RenderVKTexSmallHQDim (read each texture pass)
    // Why an image got its encoding (SharedTex::enc_why, stats, RenderVKDebugView 19).
    enum TexEncWhy : uint8_t
    {
        TEXWHY_LEVEL = 0,      // the level's format (levels 1-3, BC5 normals)
        TEXWHY_SMALL,          // <= RenderVKTexSmallHQDim: BC7 HQ
        TEXWHY_AVATAR,         // avatar bake / skin (adaptive): BC7 HQ
        TEXWHY_REDUCED,        // adaptive, uploaded below full size: BC1 / BC3
        TEXWHY_CHEAP,          // content rule: BC1 / BC3 measured good
        TEXWHY_BC7,            // content rule: BC1 / BC3 measured not good enough -> BC7 HQ
        TEXWHY_TRIAL,          // BC7 HQ while its BC1 / BC3 trial is measured
        TEXWHY_UNMEASURABLE,   // not a multiple of 4 (no whole-block stats): BC7 HQ
        TEXWHY_COUNT
    };
    // The encoding for one upload: enc, plus a cheaper TRIAL encoding to measure alongside
    // it (adaptive content rule, no verdict yet).
    struct TexEncPick { uint8_t enc = TEXENC_NONE, trial = TEXENC_NONE, why = TEXWHY_LEVEL; };
    struct TexEncIn
    {
        uint64_t key = 0;                // shared-image key (the verdict cache's key)
        uint32_t comps = 4, w = 0, h = 0;   // channels; the image's size at upload
        uint32_t full = 0;               // the asset's full size (largest side; 0 = unknown)
        bool normal_only = false, srgb = true, pinned = false, avatar = false;
        bool hud = false;                // a HUD material uses it (#532): never block-compressed
    };
    TexEncPick pickTexEnc(const TexEncIn& in) const;
    // ADAPTIVE CONTENT RULE (level 4; level 3 small textures). A colour texture's first
    // full-size upload encodes BC7 HQ (shown at once) AND a BC1 / BC3 candidate from the
    // same source; when that frame has finished, the encoder's error measures of the
    // candidate decide: good enough -> the candidate is swapped in (no re-encode), else it
    // is dropped. The MEASURES (not the verdict) are remembered per texture, so later
    // uploads (residency changes) reuse them and a threshold change applies at the next
    // upload. A resident BC7 HQ image without measures (level switch, refused trial) gets
    // a one-off candidate encoded from its own pixels. The measures live in mTexFacts
    // (bounded, persisted across sessions by texture UUID).
    struct AdaptMeas
    {
        float psnr = 0.f, smooth_psnr = 99.f, smooth_frac = 0.f, bad_frac = 0.f;
        float ref_psnr = 0.f;            // the BC7 HQ encode's PSNR (dual trials; 0 = unknown)
        bool  alpha = false;             // measured as BC3 (alpha) / BC1
        bool  vs_bc7 = false;            // measured against BC7 HQ pixels (+1 dB required)
    };
    // Persistent per-texture facts (llcleantexencvk.h): adaptive measures, avatar,
    // constant colour at full size, alpha unused -- by UUID, across sessions.
    LLCleanTexFacts mTexFacts;
    bool     mTexFactsStarted = false;
    bool     adaptMeas(uint64_t key, bool alpha, AdaptMeas& out) const;
    void     adaptRemember(const LLUUID& id, const AdaptMeas& m);
    // facts use: content-rule picks served from stored measures vs trials started (misses);
    // constant / alpha-unused facts applied to a texture's first upload
    uint64_t mFactHits = 0, mFactMisses = 0, mFactConstRestored = 0, mFactOpaqueRestored = 0;
    uint8_t  adaptReject(const AdaptMeas& m) const;   // 0 = BC1 / BC3 good; 1 psnr, 2 smooth, 3 bad blocks
    float    mAdaptMinPSNR = 42.f, mAdaptSmoothMargin = 3.f;
    bool     mAdaptReducedCheap = true;
    struct AdaptTrial
    {
        uint64_t key = 0;
        LLUUID   id;                     // the texture (facts are stored by UUID)
        VkImage  prim = VK_NULL_HANDLE;  // the image the candidate would replace
        GpuImage cand{};                 // encoded candidate (owned until swapped / retired)
        uint8_t  cand_enc = TEXENC_NONE;
        bool     alpha = false, vs_bc7 = false;
        float    prim_psnr = 0.f;
        uint32_t pass = 0;               // queued (timeout)
    };
    std::unordered_map<uint32_t, AdaptTrial> mAdaptTrials;   // by encoder tag; bounded by encodes in flight
    uint32_t mAdaptTagNext = 1;
    uint32_t adaptTag() { if (++mAdaptTagNext == 0) mAdaptTagNext = 1; return mAdaptTagNext; }
    std::vector<LLCleanTexEncVK::Result> mAdaptResults;
    std::vector<uint8_t> mVkTexWhy;      // per bindless slot: its image's enc_why (debug view 19)
    // adaptive stats: uploads by reason; trial outcomes; tuning extremes (VKTex log)
    uint64_t mAdaptUp[TEXWHY_COUNT] = {};
    uint64_t mAdaptTrialsStarted = 0, mAdaptKeptCheap = 0, mAdaptKeptBC7 = 0, mAdaptSwaps = 0,
             mAdaptUnused = 0, mAdaptTimeouts = 0, mAdaptRej[4] = {};
    struct AdaptEdge { bool set = false; uint64_t key = 0; AdaptMeas m; uint8_t why = 0; };
    AdaptEdge mAdaptWorstKept, mAdaptBestRejected;   // since the last VKTex report
    std::vector<uint64_t> mResReenc;               // keys awaiting a format re-encode
    std::unordered_set<uint64_t> mResReencQueued;
    uint32_t mLapEncN[TEXENC_COUNT] = {}, mTexEncN[TEXENC_COUNT] = {};   // images per encoding (lap / last lap)
    uint64_t mLapEncB[TEXENC_COUNT] = {}, mTexEncB[TEXENC_COUNT] = {};   // their bytes
    uint32_t mLapReenc = 0, mTexReencBacklog = 0;  // mismatched images found in the lap
    // Why shared images are RGBA8 (lap / last lap): compression off, pinned (light /
    // particle / water / sky use), below RenderVKTexCompressMinDim, not live, queued for a
    // re-encode (refused by the encoder last time / not yet), made this pass, waiting on a
    // decode, other (a bug: logged once), HUD (#532: HUD textures are never compressed).
    enum { RGBA_OFF = 0, RGBA_PINNED, RGBA_SMALL, RGBA_NOT_LIVE, RGBA_REFUSED, RGBA_QUEUED, RGBA_FRESH, RGBA_WAIT,
           RGBA_OTHER, RGBA_SMALL_RES, RGBA_HUD, RGBA_WHY_COUNT };
    uint32_t mLapRgbaWhy[RGBA_WHY_COUNT] = {}, mTexRgbaWhy[RGBA_WHY_COUNT] = {};
    bool     mRgbaOtherLogged = false;
    uint32_t mDbgBaseFmtN[10] = {};      // debug view 19: live materials' base maps per format code
    // Debug views 23 / 24 (#685, texture mip level): per bindless slot, its shared image's
    // packed level code (clean_draw.frag kMip*; fillDbgMipCodes, every frame either view is
    // on). Bits 0-3 resident levels below full, 4-7 requested (target incl. bias) below
    // full, 8-11 demand-only target below full, bit 12 small asset (full <= 256 px), 13
    // own-slot image (media / dynamic: no residency), 14 constant collapsed to 1x1, 15
    // pinned (full by rule), 16 full size unknown, 17 resident above the requested
    // (downgrade pending), 31 valid.
    std::vector<uint32_t> mDbgMipSlot;
    void fillDbgMipCodes();
    uint64_t mReencRaw = 0, mReencGpu = 0, mReencTranscode = 0, mLossyRefresh = 0;
    // constant-colour images: uploads scanned, constant at the scanned decode, constant at
    // the asset's full size; collapsed to 1x1 now (lap) and the bytes that saves
    uint64_t mConstScanned = 0, mConstFound = 0, mConstFull = 0, mAlphaUnused = 0;
    uint32_t mLapCollapsed = 0, mTexCollapsed = 0;
    uint64_t mLapCollapseSaved = 0, mTexCollapseSaved = 0;
    double   mTexPsnrLogNext = 0.0;
    // + IDLE HOLD (RenderVKTexHoldIdleSec): images kept above their target while VRAM is
    // not under pressure (held), the bytes they keep over it, the allowance they fit in,
    // and the ones the allowance let go (oldest out of view first) at the last lap's end.
    struct TexResStats { uint32_t full = 0, reduced = 0, floor = 0, up_pending = 0, down_pending = 0;
                         uint64_t bytes_full = 0, bytes_res = 0, budget = 0;
                         uint32_t held = 0, held_cut = 0;
                         uint64_t held_res = 0, held_extra = 0, held_allow = 0;
                         double   held_oldest_s = 0.0; bool held_on = false; };
    TexResStats mTexResStats;
    // IDLE HOLD (pressure-aware texture hold). A live image whose demand fell below what it
    // holds (it left view / moved far) is normally shrunk on the GPU once the demand hold
    // (RenderVKTexHoldSec, 2 s) lapses, and read back from the GPU-ready cache in full when
    // it comes back. While VRAM is NOT under pressure (no texture step, no ladder rung, no
    // recent OOM) it keeps its resident size for up to RenderVKTexHoldIdleSec after it left
    // (SharedTex::seen_t) instead -- only the downgrade waits: its demand (budget sums, LL's
    // decode demand) falls as before, so the texture step never moves for held images.
    // Bounded: every completed lap sorts the held images newest-first and keeps them while
    // their bytes over target fit the allowance = 90% of the texture budget minus the live
    // demand at the current step (and the heap's headroom under 85%); the rest -- the
    // longest out of view -- get a cutoff (mHoldCutT) and shrink on their next visit. Under
    // pressure the hold is off and the backlog drains oldest-first (downgrade queue order).
    struct HoldCand { double seen_t; uint64_t extra; };
    std::vector<HoldCand> mLapHold;      // this lap's held candidates (reused; <= mSharedTex)
    double   mHoldCutT = 0.0;            // held images left view at or before this: not kept

    // CRITICAL: these members are declared UNCONDITIONALLY. They used to sit inside
    // `#if defined(LL_HAS_CLEAN_PIPELINE)`, but that define is set for ONLY the
    // llcleanrenderervk.cpp TU (see newview/CMakeLists.txt). Other TUs that include
    // this header -- notably llvulkancontext.cpp, which does `new LLCleanRendererVK()`
    // in runWorldSynthTest() -- compiled a SMALLER object, while init()/upload code
    // (compiled WITH the define) wrote these members past the allocation: an ODR
    // layout mismatch that heap-corrupted on the first mVkTexKey.push_back(). Keep
    // them unconditional.
    // UUID (as 2x U64 words, matching the GL bindless handle slot) -> VK tex index.
    std::vector<uint64_t> mVkTexKey;        // parallel to mTextures: source UUID.first
    std::vector<int32_t>  mVkTexGen;        // parallel to mTextures: LLTextureData::getVKGeneration()
    // parallel to mTextures: the resolved image dims. Re-resolve is UPGRADE-ONLY (bigger
    // than what we hold) instead of gen-chasing -- gens bump on every re-decode so a gen
    // gate never converges (perpetual 96 re-resolves/frame + device-wait = the stall
    // reintroduced); sizes converge (a texture upgrades 16->2048 a handful of times ever).
    std::vector<uint16_t> mVkTexW, mVkTexH;
    // key -> texture index, so uploadRealMaterialTextures resolves each material in O(1)
    // instead of a linear scan of mVkTexKey (was O(materials * textures) per frame ==
    // the dominant CPU cost at ~3000 materials x ~2700 textures).
    std::unordered_map<uint64_t, uint32_t> mVkTexIndex;
    // sRGB/UNORM DEDUP. A texture used both as colour (sRGB view) and as data (UNORM
    // view) owns ONE VkImage (VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT + format list) with a
    // view per bindless slot. Keyed by the unsalted UUID key (|1). mVkTexShared is
    // parallel to mTextures: the registry key, or 0 when the slot owns its image (white,
    // proto, media). A shared slot's GpuImage has memory == VK_NULL_HANDLE; release it
    // ONLY through releaseTexSlot() -- destroyImage() would free the shared image.
    // RESIDENCY (visibility-driven mips): the image holds the source's mip chain from a
    // target level down -- w/h are the RESIDENT dims, src_w/src_h the largest decode
    // seen, full_w/full_h the asset's full size (under LL's resolution cap; the decode
    // itself now follows demand, so it can be smaller). hold/hold_t: the texel demand
    // held for kTexHoldSec before it may fall (no thrash while panning). pinned:
    // lights/particles/water -- always full. ll_dem/ll_dem_t: the demand last sent to
    // LL's texture (LLViewerFetchedTexture::setVKDemand) and when.
    struct SharedTex { VkImage image = VK_NULL_HANDLE; VkDeviceMemory memory = VK_NULL_HANDLE;
                       uint32_t w = 0, h = 0, levels = 0; bool base_srgb = true;
                       uint32_t slot[2] = {0, 0};   // [0] sRGB view slot, [1] UNORM view slot (0 = none)
                       LLUUID id; uint32_t src_w = 0, src_h = 0;
                       uint32_t hold = 0; double hold_t = 0.0; bool pinned = false;
                       uint32_t made_pass = 0;      // pass its image was (re)made: not resized again that pass
                       uint32_t full_w = 0, full_h = 0;
                       uint32_t ll_dem = 0; double ll_dem_t = -1.0e9;
                       // IDLE HOLD: last time the resident image was within 2x of its
                       // target (on screen at its size); 0 = not scanned yet.
                       double seen_t = 0.0;
                       uint32_t up_wait_until = 0;  // pass: an upgrade waiting on a sharper decode retries after this
                       // BLOCK COMPRESSION (#263): fmt = the image's base format, enc = how it
                       // was encoded (LLTexEnc). comps = the decode's channel count (3 =
                       // opaque); normal_only = every view so far is a normal map (BC5
                       // eligible). lossy = transcoded on the GPU from already-compressed
                       // pixels (refreshed from a decode when one arrives); reenc_force =
                       // a view the current format can't serve (BC5 asked for colour).
                       VkFormat fmt = VK_FORMAT_R8G8B8A8_SRGB; uint8_t enc = 0; uint8_t comps = 4;
                       bool normal_only = false, lossy = false, reenc_force = false;
                       uint32_t reenc_wait_until = 0;
                       uint8_t enc_level = 0;       // RenderVKTextureCompression level it was encoded under
                       uint8_t enc_why = 0;         // TexEncWhy of the current encoding
                       bool avatar = false;         // avatar texture (bake / skin): adaptive keeps BC7 HQ
                       // SERVER BAKE (BOM; BOOST_AVATAR_BAKED[_SELF], sticky, #786): plain mips,
                       // never a coverage chain -- see covWant.
                       bool bake = false;
                       // HUD texture (#532, sticky): a HUD material resolved it. Kept RGBA8
                       // at every compression level (text / icon edges are what BC blocks
                       // smear; HUD images are few) and its base image is UNORM, so the
                       // mip cascade averages stored (gamma) values like LL's GL mips --
                       // world users still get their sRGB view of the mutable image.
                       bool hud = false;
                       uint32_t trial_tag = 0;      // the adaptive candidate being measured (encoder tag; 0 = none)
                       uint32_t pin_pass = 0;       // last pass a pinned (light / particle / sky) use resolved it
                       bool mat_used = false;       // a material resolved it (sticky): compressed even while pinned
                       bool enc_refused = false;    // its last upload wanted a BC format but the encoder refused
                       uint32_t trial_wait_until = 0;   // pass: a timed-out trial retries after this
                       // CONSTANT COLOUR: the whole decode was one RGBA value (verified on a
                       // const_w x const_h decode; 0 = not constant / not scanned).
                       uint32_t const_w = 0, const_h = 0, const_rgba = 0;
                       // GPU-READY DISK CACHE (tier 2, #274): the largest side of this
                       // texture's chain known to be on disk in the current encoding (served
                       // from there, captured to it, or found in its index); a capture is
                       // queued only for a larger resident image. t2_queued: in mT2CapCand.
                       uint16_t t2_top = 0; bool t2_queued = false;
                       bool t2_src = false;         // the current image came from tier 2 (kept through downgrades: still the chain's blocks, #865)
                       // COVERAGE-PRESERVING ALPHA MIPS (RenderVKAlphaCoverageMips): mask_cut =
                       // the HIGHEST alpha cutoff (8-bit, 0 = no mask use) of the alpha-MASK
                       // materials that resolved this image as its base colour (#786: was the
                       // FIRST one's, sticky -- a face that first registered in a transitional
                       // auto-mask state (default cutoff 0.33) before its material's own cutoff
                       // arrived kept a chain built for 84 while the FS tested e.g. 128: a chain
                       // built for a lower cutoff than the test scales coarse mips' alpha DOWN
                       // (s < 1), so near-cutoff texels fall under the real test -> mottled /
                       // missing surface growing with distance. Built for a higher cutoff than a
                       // user tests only thickens.) cov = the cutoff the current image's mips
                       // were built for (0 = plain box / blit mips). A mismatch with what is
                       // wanted: a CovJob builds the chain (or the lossy lever refreshes plainly).
                       uint8_t mask_cut = 0, cov = 0;
                       uint8_t cov_why = 0;         // CovWhy: the path that last made it with plain mips (diag)
                       // DEBUG (#685, views 23 / 24 + inspector): the residency scan's last
                       // target for it -- dbg_k = mip levels below full it is requested at
                       // (demand + bias), dbg_kd = at demand alone (bias 0); dbg_state 0 not
                       // scanned yet, 1 live, 2 pinned (full by rule), 3 not live, 4
                       // constant colour collapsed to 1x1.
                       uint8_t dbg_k = 0, dbg_kd = 0, dbg_state = 0;
                     };
    std::unordered_map<uint64_t, SharedTex> mSharedTex;
    // #1032: bumped whenever a bindless slot's shared image mapping (mVkTexShared) or a shared
    // texture's alpha verdict (SharedTex::comps) may have changed -- LLCleanRTVK rebuilds its
    // per-slot "no alpha" bits (clean_rt_instances.comp maskKnownOpaque) when it moves.
    uint32_t mTexAlphaGen = 0;
    // RenderVKAlphaCoverageMips as last seen (2 = not yet); images refreshed for it.
    uint8_t  mCovMipsSeen = 2;
    uint64_t mCovRefresh = 0;
    // COVERAGE CHAIN JOBS: an alpha-mask image is uploaded with plain mips; its coverage-
    // preserving chain (buildCoverageChain, ~10 ms per 1024^2) is built on its own worker
    // (mCovPool, one thread) that NO frame waits on, then swapped in through the batch as
    // a prebuilt chain (a memcpy into staging). Bounded: kCovJobsMax in flight, starts and
    // swaps per pass capped (uploadRealMaterialTextures). Keyed by shared-image key.
    // + what the build measured (diag, #786): level-0 coverage at the cutoff (target), the
    // fraction of partial (16 < a < 240) and of near-cutoff (|a - cut| <= 24) texels, and
    // the range of the per-level alpha scales applied (1 = none).
    struct CovResult { std::atomic<bool> done{false}; std::shared_ptr<const std::vector<uint8_t>> chain;
                       float target = 0.f, partial = 0.f, near_cut = 0.f, s_min = 1.f, s_max = 1.f; };
    struct CovJob { LLPointer<LLThreadSafeRefCount> hold; const LLImageRaw* raw = nullptr;   // hold keeps raw alive
                    uint32_t w = 0, h = 0, skip = 0, iw = 0, ih = 0;
                    uint32_t from_w = 0, from_h = 0;   // the image size it replaces (an upgrade: smaller)
                    bool srgb = true; uint8_t cut = 0; uint8_t why = 0; uint32_t pass = 0; std::shared_ptr<CovResult> res; };
    // Why an image needed a (new) coverage chain (diagnostic: RenderVKAlphaCoverageDebug).
    enum CovWhy : uint8_t { COVW_NONE = 0, COVW_FRESH, COVW_REBUILD_PINNED, COVW_UPGRADE_PLAIN, COVW_REENCODE_DECODE,
                            COVW_TIER2, COVW_UPGRADE, COVW_CUTOFF, COVW_COUNT };
    // n swaps; cut = the cutoff of the last chain; sizes = distinct-size bits (bit log2(max side)):
    // many swaps over few sizes = residency resizing back and forth, not a cutoff problem.
    struct CovSwapStat { uint32_t n = 0; uint8_t why = 0; uint8_t cut = 0; uint16_t sizes = 0; LLUUID id; uint32_t w = 0, h = 0; };
    std::unordered_map<uint64_t, CovSwapStat> mCovSwapStats;   // bounded (cleared at 8192 keys)
    double mCovDiagNext = 0.0;
    std::unordered_map<uint64_t, CovJob> mCovJobs;
    void* mCovPool = nullptr;     // LL::ThreadPool* (opaque here; created in init)
    uint64_t mCovBuilt = 0, mCovSwapped = 0, mCovDropped = 0;
    // ---- GPU-READY DISK CACHE (tier 2, #274; LLGpuTexCache) ----
    // WRITE: the residency scan nominates settled, exactly-encoded BC images bigger than
    // what the cache holds (mT2CapCand, bounded); the end of the texture pass turns a few
    // into CAPTURES (a host-visible readback buffer each, bounded bytes); beginTextureFrame
    // records the image -> buffer copy (before the frame's uploads, so a same-frame
    // downgrade from that image can't race its layout) and, once that frame slot's fence
    // has been waited, hands the mapped bytes to the cache's disk worker; the buffer is
    // destroyed when the worker has copied them (`released`). hdr = LLGpuTexHeader bytes
    // (kept opaque here: other TUs include this header without newview's include path).
    struct TexT2Capture { uint64_t key = 0; VkImage image = VK_NULL_HANDLE; VkFormat fmt = VK_FORMAT_UNDEFINED;
                          uint32_t w = 0, h = 0, levels = 0; GpuBuffer buf{}; uint64_t bytes = 0;
                          uint8_t hdr[128] = {}; std::shared_ptr<std::atomic<bool>> released;
                          double handed_t = 0.0; };
    std::vector<uint64_t>     mT2CapCand;                    // shared keys nominated by the scan
    std::vector<TexT2Capture> mT2CapQueued;                  // recorded by the next beginTextureFrame
    std::vector<TexT2Capture> mT2CapSlot[kTexDescSets];      // recorded in that slot's frame
    std::vector<TexT2Capture> mT2CapWriting;                 // with the disk worker (buffer alive)
    uint64_t mT2CapBytes = 0, mT2CapPeak = 0;                // readback bytes alive (cap kT2CapMaxBytes)
    uint64_t mT2Captures = 0, mT2CapNoAlpha = 0, mT2CapRefused = 0;
    // READ (LLGpuTexCache keeps the pending / ready reads by UUID): images made from tier 2
    // (fresh / upgrade), entries rejected by the format checks, LL decodes held coarse.
    uint64_t mT2Served = 0, mT2ServedUp = 0, mT2Rejected = 0;
    // Shared keys whose cached chain failed the format checks (not what the streamer would
    // make now): neither served nor used as LL's "GPU covers it" hint. Bounded (cleared at
    // 4096 entries and on a compression level change).
    std::unordered_set<uint64_t> mT2RejectKeys;
    uint32_t mT2Level = 0xFFFFFFFFu;
    void t2RecordCaptures(VkCommandBuffer cmd, uint32_t slot);
    void t2HarvestCaptures(uint32_t slot);
    void t2DestroyCaptures();   // device idle / shutdown: bounded wait on the disk worker
    // The pick for an upload of st at w x h (#263) / at its current size; acceptable =
    // its stored encoding may stay (levels 1-3: exactly the pick; adaptive: any of the
    // formats the level chooses between, once encoded under it -- residency changes and
    // verdicts never force a re-encode, only a level switch or a channel change does).
    TexEncPick texEncFor(const SharedTex& st, uint32_t w, uint32_t h) const;
    uint8_t  texEncWanted(const SharedTex& st) const { return texEncFor(st, st.w, st.h).enc; }
    bool     texEncAcceptable(const SharedTex& st) const;
    bool     texTrialWanted(const SharedTex& st) const;   // a one-off candidate from its own pixels
    // Residency work queued by the incremental scan, drained under per-pass budgets.
    struct TexResUp   { uint32_t prio; float ratio; uint64_t key; };
    // (seen_s: SharedTex::seen_t in whole seconds -- the queue drains oldest out of view first)
    struct TexResDown { uint64_t saved; uint64_t key; uint32_t drop; uint32_t from_w, from_h; int64_t seen_s = 0; };
    std::vector<TexResUp>   mResUps;
    std::vector<TexResDown> mResDowns;
    // Per-material liveness / priority / texel demand, rebuilt every 4th pass.
    std::vector<uint8_t>  mLiveMatCache;
    // Material ids the store recycled for new content since the last resolver pass
    // (LLCleanMaterialStore::takeRecycled): their per-id resolve state is reset.
    std::vector<uint32_t> mMatRecycled;
    // INCREMENTAL MATERIAL LIVENESS: per material, how many live instances use it (and
    // how many priority ones); per terrain record, how many terrain patches name it.
    // Maintained from the dirtied instance runs (+ a rolling self-heal slice) instead
    // of walking every instance (~6 ms per scan at 60k instances).
    struct InstMatRef { uint32_t mat = 0xFFFFFFFFu, trec = 0xFFFFFFFFu; uint8_t prio = 0; };
    std::vector<InstMatRef> mInstMatRef;
    std::vector<uint32_t> mMatRef, mMatPrioRef, mTrecRef;
    std::vector<std::pair<uint32_t, uint32_t>> mMatRefRuns;
    bool     mMatRefFull = true;
    uint32_t mMatRefCursor = 0;
    // Runs every resolver pass (liveness must not lag an instance moving onto a material:
    // a stale "dead" verdict cleared materials that were being drawn -> white flashes).
    // heal: also run the rolling self-heal slice (scan passes only). Returns true when it
    // recounted everything (the caller then rebuilds the whole liveness cache).
    bool updateMaterialRefs(uint32_t n_mat, uint32_t n_trec, bool heal);
    // Material ids whose instance count changed in the last updateMaterialRefs (the
    // non-scan passes refresh only these liveness entries).
    std::vector<uint32_t> mMatRefTouched;
    // WHITE-BASE DIAG (a drawn face whose base colour is unresolved shades the bare
    // factor = white): counts since the last VKTexUpload summary line.
    struct WhiteBaseStats
    {
        uint32_t white_live = 0;      // live material resolves left without a base map
        uint32_t regress = 0;         //   ...of which had one resolved before
        uint32_t revived = 0;         // dead -> live (an instance moved onto it)
        uint32_t revived_cleared = 0; //   ...whose resolution had been cleared (was white before this fix)
        uint32_t clear_avoided = 0;   // non-live clears refused: an instance still uses it
        uint32_t flips = 0;           // instance rows that switched material id
    } mWbStats;
    std::vector<uint32_t> mPrioMatsCache;
    std::vector<uint32_t> mMatDemandCache;
    std::vector<uint64_t> mVkTexShared;
    uint64_t releaseTexSlot(uint32_t slot);   // device bytes of an image it freed (0: a view only)

    // ---- ASYNC TEXTURE STREAMING ----
    // Uploads/downgrades created in uploadRealMaterialTextures (syncFromStores, before
    // acquire) are queued here and recorded into the frame's own command buffer by
    // beginTextureFrame -- no submitImmediate / CPU wait. Anything a frame in flight may
    // still reference (old images/views, staging, transients) is RETIRED: queued in
    // mPendRetire, handed to the recording slot's list, and destroyed the next time that
    // slot comes round (its fence waited => that frame and every earlier one finished).
public:   // shared with the file-local TexUploadBatch
    // Constant-colour scan of one upload's decode (worker side; read after the stage wait).
    // + whether a 4-channel decode's alpha is all 255 (stored as if 3-channel: BC1 / BC5
    // eligible; a later decode with real alpha flips it back and re-encodes).
    struct TexConstScan { bool constant = false, opaque = false; uint8_t comps = 4; uint32_t rgba = 0, w = 0, h = 0; };
    struct TexPendingItem { GpuImage img; GpuBuffer stg; uint32_t w = 0, h = 0, levels = 0;
                            GpuImage transient{}; uint32_t skip = 0; VkImage copy_src = VK_NULL_HANDLE;
                            VkDeviceSize stg_off = 0; bool stg_ring = false;
                            // GPU block-compression encode of img (#263): the transient (pooled)
                            // or copy_src (re-encode) is the source; see LLCleanTexEncVK.
                            bool encode = false, transient_pooled = false;
                            // a block-compressed chain from the GPU-ready disk cache (#274):
                            // staging holds levels 0..levels-1 tightly packed, copied as is;
                            // #865 upgrade: with copy_src, staging holds only levels
                            // 0..stg_levels-1 and the rest are copied from copy_src's levels
                            // 0.. (the resident image the chain's lower levels came from)
                            bool blocks = false;
                            uint32_t stg_levels = 0;
                            // a coverage-preserving RGBA8 chain (alpha-mask image,
                            // RenderVKAlphaCoverageMips): staging holds every level the
                            // staging worker built (same layout as blocks); copied into img,
                            // or into the transient when img is encoded -- no blit cascade
                            bool chain = false;
                            // a texture pool compaction move (#614): img = a same-size copy
                            // of copy_src (every mip) in a fuller pool block
                            bool move = false;
                            LLCleanTexEncVK::Job job;
                            uint64_t key = 0;                        // shared-image key
                            std::shared_ptr<TexConstScan> cscan; };
    // STAGING RING: one persistent host-visible buffer sub-allocated for texture uploads
    // (per-upload vkAllocateMemory + free was a large part of streaming cost). Virtual
    // offsets grow monotonically (physical = v % cap); beginTextureFrame(slot) stores
    // the head as that slot's mark after recording, and frees up to a slot's mark when
    // the slot comes round (its fence waited). Full / oversized -> dedicated buffer.
    using TexStageRing = VkStageRing;   // vk_stagering.h (shared with LLRenderVK)
    TexStageRing mStageRing;
    uint64_t     mStageMark[kTexDescSets] = {};
    std::vector<TexPendingItem> mPendUploads;
    struct TexRetire { std::vector<VkImageView> views; std::vector<GpuImage> images; std::vector<GpuBuffer> buffers;
                       std::vector<VkSampler> samplers;   // live aniso change: the replaced world sampler
                       std::vector<VkPipeline> pipelines; // evicted setting-specialization pipelines
                       // copy-on-write re-points (mCow): the replaced sets go back to their member's spares
                       struct CowRet { VkDescriptorSet set; uint32_t member; uint32_t gen; };
                       std::vector<CowRet> dsets;
                       bool empty() const { return views.empty() && images.empty() && buffers.empty() && samplers.empty()
                                                   && pipelines.empty() && dsets.empty(); } };
    TexRetire mPendRetire;
    TexRetire mRetire[kTexDescSets];
    // The async compute timeline value handed out (mTlCompNext) when mRetire[s] was filled:
    // the slot's list is freed only once the compute queue has passed it too (the graphics
    // half is the slot's in-flight fence). 0 = no compute work to wait for.
    uint64_t  mRetireComp[kTexDescSets] = {};
    void retireView(VkImageView v)  { if (v) mPendRetire.views.push_back(v); }
    void retireImage(const GpuImage& g) { if (g.image || g.memory || g.view) mPendRetire.images.push_back(g); }
    void retireBuffer(const GpuBuffer& b) { if (b.buffer) mPendRetire.buffers.push_back(b); }
    void retirePipeline(VkPipeline p) { if (p) mPendRetire.pipelines.push_back(p); }
    // Device idle only (shutdown, vramRelief): frees everything in `r` now.
    void destroyRetired(TexRetire& r);
    // RETIRE GUARD (beginTextureFrame, slot s's fence waited): frees `r` like destroyRetired,
    // except an image / view that a live bindless slot, the shared-texture registry, another
    // retire list or the descriptor set of a frame that may still be in flight (any set but
    // s's) still names -- those are repaired / dropped / deferred (mRetireLate) and logged
    // (texture freed while in use: a GPU page fault / device lost otherwise).
    void destroyRetiredGuarded(TexRetire& r, uint32_t s);
    // Images / views the guard held back; re-checked every beginTextureFrame. Bounded: an
    // entry is freed (with an error line) after kRetireLateMaxBegins begins whatever names it.
    struct TexLate { GpuImage img; VkImageView view = VK_NULL_HANDLE; uint32_t begins = 0; };
    std::vector<TexLate> mRetireLate;
    static constexpr uint32_t kRetireLateMaxBegins = 3u * kTexDescSets;
    // Guard counters (the VKMem line and the device-lost report): bookkeeping repairs (a live
    // slot still viewing a retired image), duplicate / registry-owned retire records dropped,
    // frees deferred because an in-flight set named the object, compute-timeline waits needed,
    // forced frees after the cap.
    uint64_t mGuardSlotRepairs = 0, mGuardDupDrops = 0, mGuardOwnedDrops = 0, mGuardDeferred = 0,
             mGuardCompWaits = 0, mGuardForced = 0;
    // Wait (host) until the async compute queue has passed `value` (a no-op when it already
    // has, without async compute, or when the timeline was re-created since the stamp).
    void texRetireCompWait(uint64_t value);
    // #697 asset-level drop for textures (see texOomNote in the .cpp)
    struct TexOomRec { uint32_t fails = 0; double last = -1.0e9; double until = 0.0; };
    std::unordered_map<uint64_t, TexOomRec> mTexOom;
    uint32_t mTexOomDropped = 0;
    bool texOomBlocked(uint64_t key) const;
    void texOomNote(uint64_t key);
    // ---- TEXTURE POOL COMPACTION (#614; VkDeviceContext::texCompactPlan) ----
    // Shared images in the emptiest texture-pool blocks move into fuller blocks so the
    // sparse blocks free. A move = a new image placed in a fuller block (TexCompactScope),
    // a queued pending item that copies every mip (recorded by beginTextureFrame before any
    // pass samples), both bindless views repointed (descriptor versions bumped, content
    // version kept) and the old image + views RETIRED -- destroyed when the frame that
    // records the copy has finished, so nothing samples freed or half-copied memory.
    // Background: RenderVKTexCompactMBPerFrame while the reclaimable slack exceeds
    // RenderVKTexCompactMinSlackMB. Pressure PRE-STEP: when the VRAM ladder would step down
    // (or an allocation failed), RenderVKTexCompactPressureMBPerFrame until nothing is left
    // to reclaim; the ladder waits for it (LLVramBudgetVK::Input::holdLadder) unless a
    // failed request still does not fit.
    struct TexMove { VkImage img; uint64_t key; VkDeviceMemory block; };
    // left: queued moves not yet tried; failed: one was busy / impossible (the block will
    // not empty); noroom: no fuller block had a slot -- the block's other moves are dropped;
    // counted: all its moves done (in texCompactEvacuated).
    struct TexEvac { VkDeviceMemory block; uint32_t left; uint32_t last_pass; bool failed; bool noroom; bool counted; };
    std::vector<TexMove> mCompactQueue;         // planned moves, drained per pass
    std::vector<TexEvac> mCompactEvac;          // source blocks being evacuated (destinations avoid them)
    uint32_t mCompactNextPlan = 0;              // pass of the next background plan
    uint32_t mCompactIdle = 0;                  // consecutive plans that found nothing movable
    bool     mCompactPendRetire = false;        // mPendRetire holds move sources
    bool     mCompactRetiring[kTexDescSets] = {};   // that slot's retire list holds move sources
    // Pressure pre-step (one episode at a time; logged at start and finish).
    bool     mCompactAggr = false;
    bool     mCompactKick = false;              // relief / a failed allocation asked for it
    bool     mCompactDrain = false;             // nothing left to move: waiting for the frees to land
    uint32_t mCompactDrainPass = 0;
    double   mCompactAggrT0 = 0.0, mCompactCoolUntil = 0.0;
    uint64_t mCompactAggrSlack0 = 0, mCompactAggrBlocks0 = 0, mCompactAggrMoved0 = 0, mCompactAggrRel0 = 0;
    uint64_t mCompactReclaim = 0;               // last measured reclaimable bytes (texPoolReclaimable)
    bool     mCompactOn = false;                // RenderVKTexCompact (and texture pools active)
    double   mCompactOverSince = 0.0;           // background: slack over the threshold since (0: not over)
    uint64_t mCompactPinnedSkips = 0;           // queued moves dropped: the image became pinned
    // FIXED-SET SLOT CHECK (#614 dark projectors, diagnostic): after each fixed-set resolve,
    // every projector-texture slot must name its shared image's current image + a live view.
    // Per slot: the image and content version last seen -- an image change without a content
    // change is a compaction move (pinned images are excluded, so it must not happen).
    struct FixedSlotSeen { VkImage image = VK_NULL_HANDLE; uint32_t content = 0; };
    std::unordered_map<uint32_t, FixedSlotSeen> mLightSlotSeen;
    void     checkLightTexSlots();
    void     texCompactPass(bool blocked);      // texture pass, after the batch hand-off
    void     moveSharedImage(SharedTex& st, const GpuImage& dst);
    bool     texCompactHoldLadder(double now);  // budgetTick: may the pre-step hold the ladder?
    void     texCompactPressure(double now, bool pressured, bool new_fail);   // budgetTick: start / end the pre-step
    void     texCompactEnd(double now, const char* why);
    // Worker staging: pixel conversion into mapped staging runs on mStagePool; the frame
    // waits the outstanding count (normally already zero) before recording the copies.
    struct TexStageSync { std::atomic<uint32_t> pending{0}; std::mutex m; std::condition_variable cv; };
private:
    std::shared_ptr<TexStageSync> mStageSync = std::make_shared<TexStageSync>();
    void* mStagePool = nullptr;   // LL::ThreadPool* (opaque here; created in init)
    uint64_t mStagedBytes = 0;    // worker-staged bytes since the last hitch count

    // ---- INCREMENTAL RESIDENCY SCAN ----
    // A rolling cursor over a key snapshot of mSharedTex visits a time-budgeted slice
    // per pass (RenderVKTexResidencyBudgetMs); per-texture-step byte sums accumulate over
    // a lap and the step decision uses the last COMPLETED lap.
    std::vector<uint64_t> mResKeys;
    uint32_t mResCursor = 0;
    uint64_t mLapFixed = 0;
    TexResStats mLapStats;   // counts/bytes accumulating over the current lap
    std::unordered_set<uint64_t> mResUpQueued, mResDownQueued;
    bool mResUpsUnsorted = false, mResDownsUnsorted = false;

    // ---- PERSISTENT RESOLVED MATERIAL HANDLES ----
    // fillFromCleanStores re-copies the raw store bytes every frame; the resolved
    // handles + has-texture flags live here and are patched back in one tight loop,
    // instead of a per-frame restore + stamp walk over every material. Liveness stamps
    // and per-slot demand are rebuilt on the every-4th-pass material scan.
    // emis_free: resolved, and the material has NO emissive map (its emissive factor is
    // the whole emission). Otherwise, while the map is not resident (has-texture bit 8
    // clear), the mirror's emissive factor rgb is zeroed: the shaders use the bare factor
    // when bit 8 is clear, so an emissive MASK (factor 1, map mostly black) still loading
    // would light the whole face full white + bloom (RenderVKEmissivePendingGate).
    // base_pending: a base-colour map is named but not resident (the face shades the
    // bare factor; RenderVKDebugUnresolvedTint paints it magenta).
    struct MatRes { uint32_t h[4] = {0, 0, 0, 0}; uint32_t flags = 0; bool emis_free = false; bool base_pending = false; };
    std::vector<MatRes>   mMatRes;
    std::vector<uint32_t> mMatLastTry;   // pass an unsettled material was last resolved
    std::vector<uint32_t> mSlotDemand;   // per-slot texel demand (every-4th-pass scan)
    void patchResolvedHandles();       // mMatRes -> mCpuMaterials (after a raw fill)
    // VRAM log (RenderVKMemoryLog): one summary line every ~5 s.
    void logMemory();
    // #407: rebuild mVramStatus' accounting from the tracker (memStats) + our suballocators.
    void updateVramStatus(const VkDeviceContext::MemStats& ms);
    // Suballocator slack into v.slack* (staging ring / geometry / tables / instance SSBO,
    // split device-local vs system heap) + the packed geometry's garbage.
    void suballocSlack(LLVramStatus& v) const;
    uint64_t pendingFreeBytes() const;     // retired images + buffers not yet destroyed
    // #563 heap-budget pre-flight relief (VkDeviceContext::setPressureRelief): device idle,
    // then free what EARLIER frames retired (never this frame's: its recording may still
    // name them) and idle encoder pools. Returns the bytes freed.
    uint64_t vramRelief();
    // #563 elastic texture room + the ladder's re-add hints (see the .cpp).
    uint64_t budgetRungReadd(uint32_t r) const;
    void     texRoomUpdate();
    uint64_t mTexRoom = 0;            // texture residency ceiling from the room others leave (0 = not measured)
    double   mTexNonTexSm = 0.0;      // smoothed usage minus texture bytes
    double   mTexNonTexAt = 0.0;
    LLVramStatus mVramStatus;
    uint64_t mSwapchainBytes = 0;
    double mMemLogNext = 0.0;
    double mMemStatsNext = 0.0;   // RenderVKMemStatsLog per-minute detail
    // #535: logMemory is STAGED across frames (it was one 5-10 ms block every 5 s inside
    // vk_syncBegin). 0 = idle; 1 = shared-texture byte tally (bucket-sliced); 2 = LL
    // texture-list tally (sliced); 3..6 = one log section per frame.
    uint32_t mMemLogStage = 0;
    size_t   mMemLogTexBucket = 0, mMemLogTexBuckets = 0;
    uint32_t mMemLogTexRestarts = 0;
    uint64_t mMemLogTexBytes = 0;
    uint32_t mMemLogTexImgs = 0, mMemLogTexDual = 0;
    uint32_t mMemLogDriven = 0, mMemLogReduced = 0, mMemLogSharpen = 0;
    uint64_t mMemLogT1Req = 0, mMemLogT1Opt = 0;
    bool memLogTallySharedTex(double budget_s);   // stage 1: true when the pass is done
    // LIVE MEDIA slots (web/video on prims): slot -> media texture id, the generation
    // last streamed, and per-frame-slot staging. Refreshed every frame by
    // recordMediaUpdates(); tracked/untracked with the slot's lifetime.
    struct MediaSlot { LLUUID id; int32_t gen = -1; GpuBuffer stg[2]{}; size_t stg_bytes[2] = {0, 0}; };
    std::unordered_map<uint32_t, MediaSlot> mMediaSlots;
    std::unordered_map<uint32_t, uint32_t> mMediaPendingMats;   // material -> first pass waiting on a live media frame
    void trackMediaSlot(uint32_t slot, const LLUUID& id, int32_t gen);
    void untrackMediaSlot(uint32_t slot);
    void recordMediaUpdates(VkCommandBuffer cmd);
    uint32_t mTexResolveTick = 0;   // periodic re-resolve (liveness changes)
                                            // captured into this slot; a newer gen triggers a
                                            // re-upload so grow-only slots converge as decodes sharpen.
    bool mRealDataActive = false;           // last fill came from the clean stores
};

#endif // LL_LLCLEANRENDERERVK_H
