/**
 * @file llcelshadevk.h
 * @brief Avatar mask (#549) and built-in cel shading (#550) for the Vulkan clean renderer.
 *
 * AVATAR MASK (P6). A per-pixel avatar identity, GPU-driven (no per-face CPU work):
 *   - The world VS (clean_draw.vert / clean_draw_rigged.vert) resolves each instance's
 *     avatar slot -- rigged mesh + the system body from SkinMeta.avatar_idx (draw set 0
 *     binding 38), worn rigid attachments (bit 9) from skin_id -- and the cel scope, into
 *     the flat varying v_cel (clean_cel_common.glsl). Both slot sources are the joint-table
 *     slot of LLCleanSkinStore::assignAvatarSlot (verified: one index space). Flexi chains
 *     (bit 19, pseudo slots), HUD and water carry no id.
 *   - The ID PASS (LLCleanRendererVK::recordCelPasses) runs after pass 1, before the god
 *     rays / pass 2, against the finished opaque depth (read-only): it re-issues the main
 *     view's rigged indirect lists (buckets 3 4 8 9, depth EQUAL) and the worn rigid
 *     attachments compacted out of the static lists by clean_cel_filter.comp (instead of
 *     re-rasterizing the world), then the alpha-blended rigged lists (bucket 5 + post-water,
 *     hair: LEQUAL + texel alpha >= 0.5) and alpha attachments. Output: an R16_UINT target
 *     at the RENDER extent, jittered like the scene: avatar slot + 1, 0 = none.
 *     (Not written by the depth pre-pass twins as the ticket sketched: those come in three
 *     attachment families x 4 x 2 cull modes, the opaque ones have no FS, and the pass works
 *     with RenderVKDepthPrepass off and in offscreen snapshots as is.)
 *   - When the FX stack asks (requestAvatarMask), a DISPLAY-resolution, UNJITTERED copy
 *     (clean_avatar_id_view.frag ID_RESOLVE, clean_depth_up.comp's mapping) -- or the
 *     render target itself when the frame is neither scaled nor jittered.
 *   - Allocated only while a consumer wants it (outlines, the debug view, FX): cel
 *     lighting alone needs no image (the VS flag carries it).
 *   Probe / mirror captures and shadow passes are separate recordings: no ids there.
 *
 * CEL SHADING (P7). Toon lighting inside the shared lightSurface() (forward + deferred:
 * G2.w bit 3), per-avatar selection by slot bitset (no re-ingest on toggles), outlines
 * as a fullscreen pass into the render-res scene HDR before pass 2 / TAA.
 */
#ifndef LL_LLCELSHADEVK_H
#define LL_LLCELSHADEVK_H

#include "vk_device.h"
#include "vk_descpool.h"
#include "lluuid.h"
#include "../llavatarslots.h"

#include <cstdint>
#include <set>

class LLCelShadeVK
{
public:
    // Draw set 0 binding 37 (clean_cel_common.glsl CelParamsSSBO), std430.
    struct Params
    {
        uint32_t flags = 0;
        uint32_t bands = 3;
        float    softness = 0.1f;
        float    flatten = 0.5f;
        float    shadow[4] = {0.f, 0.f, 0.f, 0.f};   // rgb tint, a strength
        float    rim[4] = {0.f, 0.f, 0.f, 0.f};      // rgb colour x strength, a width
        float    spec[4] = {0.5f, 0.1f, 1.f, 0.f};   // threshold, softness, strength
        uint32_t sel[kAvatarSlotWords] = {};          // avatar-slot bitset (1024 slots)
    };
    static_assert(sizeof(Params) == 64 + 4 * kAvatarSlotWords && sizeof(Params) <= 256,
                  "LLCelShadeVK::Params must match clean_cel_common.glsl (std430, 192 B; the 256 B params buffer)");
    static constexpr uint32_t kFlagLight = 1u, kFlagIds = 2u, kFlagAll = 4u, kFlagScene = 8u,
                              kFlagMeta = 16u, kFlagSpec = 32u;
    // RenderVKCelScope
    enum Scope : uint32_t { SCOPE_SCENE = 0, SCOPE_AVATARS = 1, SCOPE_SELF = 2, SCOPE_CHOSEN = 3, SCOPE_FRIENDS = 4 };

    // Session list of avatars picked with the avatar context menu ("Cel shade this
    // avatar"); RenderVKCelScope 3 shades exactly these. Main thread.
    static void toggleChosen(const LLUUID& id);
    static bool isChosen(const LLUUID& id);

    LLCelShadeVK() = default;
    ~LLCelShadeVK() = default;   // destroy() is explicit (needs the device)
    LLCelShadeVK(const LLCelShadeVK&) = delete;
    LLCelShadeVK& operator=(const LLCelShadeVK&) = delete;

    void destroy(VkDevice dev);

    // ---- state the renderer's members (llcelshadevk.cpp) drive ----
    VkDeviceContext* mCtx = nullptr;
    GpuBuffer mParamsBuf{};            // device local, 96 B (+ copy dst)
    Params    mParams{};               // CPU copy of what the GPU holds / should hold
    bool      mParamsDirty = true;     // upload at the next frame start
    uint32_t  mSelFrame = 0;           // frame counter for the selection refresh
    bool      mFxWantsMask = false;    // requestAvatarMask (FX stack)

    // This frame (set by recordCelFrameStart).
    bool      mCelOn = false;          // RenderVKCel
    uint32_t  mScope = SCOPE_AVATARS;
    bool      mIdsNow = false;         // the id pass runs this frame
    bool      mOutlineNow = false;     // outlines this frame (P7)
    bool      mDebugNow = false;       // id debug view this frame
    // Outline controls (RenderVKCelOutline*), latched at the frame start.
    float     mOutlineWidth = 1.5f;    // px at 1080p output height (scaled with the output)
    float     mOutlineColor[4] = {0.f, 0.f, 0.f, 1.f};   // linear rgb x intensity, a opacity
    float     mOutlineFade = 96.f;     // metres: lines fade out over [0.5, 1] x this
    float     mOutlineDepth = 0.04f;   // relative depth jump that draws a line
    float     mOutlineCrease = 0.02f;  // relative depth second difference (0 = no creases)

    // Id target (render extent) + its display-res unjittered copy (FX).
    static constexpr VkFormat kIdFormat = VK_FORMAT_R16_UINT;
    GpuImage  mId{};
    uint32_t  mIdW = 0, mIdH = 0;
    GpuImage  mIdDisp{};
    uint32_t  mIdDispW = 0, mIdDispH = 0;
    uint64_t  mGen = 1;                // bumped on every (re)creation of either image
    bool      mMaskValid = false;      // this frame produced the mask (hasAvatarMask)
    bool      mMaskIsRender = false;   // ...and it is the render target itself (unscaled, unjittered)

    // Worn-attachment compaction (clean_cel_filter.comp): 16 B counts + 3 lists.
    GpuBuffer mFilterBuf{};
    uint32_t  mFilterCap = 0;          // commands per list
    VkDescriptorSetLayout mFilterDsl = VK_NULL_HANDLE;
    VkPipelineLayout      mFilterLayout = VK_NULL_HANDLE;
    VkPipeline            mFilterPipe = VK_NULL_HANDLE;
    VkDescriptorSet       mFilterSet[2] = {};   // per frame slot, rewritten each use (buffers regrow)

    // Id pass pipelines (mDrawLayout; built on first use, immediate):
    // [0] static back-culled, [1] static double-sided, [2] static alpha,
    // [3] rigged back-culled, [4] rigged double-sided, [5] rigged alpha.
    VkPipeline mIdPipe[6] = {};
    bool       mIdPipesFailed = false;

    // Fullscreen consumers: set {0 id (usampler), 1 depth, 2 params SSBO}.
    VkSampler             mSampler = VK_NULL_HANDLE;   // nearest / clamp (texelFetch)
    VkDescriptorSetLayout mViewDsl = VK_NULL_HANDLE;
    VkPipelineLayout      mViewLayout = VK_NULL_HANDLE;
    VkDescriptorSet       mViewSet[2] = {};     // per frame slot, rewritten each use (targets resize)
    VkPipeline            mResolvePipe = VK_NULL_HANDLE;   // ID_RESOLVE -> mIdDisp
    VkPipeline            mDebugPipe = VK_NULL_HANDLE;     // ids as colours over the scene HDR
    VkPipeline            mOutlinePipe = VK_NULL_HANDLE;   // P7 outlines
    VkFormat              mHdrFormatBuilt = VK_FORMAT_UNDEFINED;
    VkDescPool            mPool;
};

#endif
