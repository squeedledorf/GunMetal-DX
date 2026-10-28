/**
 * @file llcleanrtcachevk.cpp
 * @brief RT sun shadow cache (#771): re-trace only the mask tiles whose sun shadow can
 *        have changed; the rest reuse their fully lit / fully shadowed history.
 *
 * The RT shadow mask (RenderVKSunShadows 2, RenderVKRTShadowMask) traced every mask texel
 * every frame: trace (c_rtmask) -> temporal -> spatial, all full screen. With
 * RenderVKRTShadowCache on, a GPU tile classifier (clean_rt_mask_cache.comp; the reasons
 * are in clean_rt_mask_cache.glsl) splits the mask's 8x8 blocks into a TRACE list and a
 * REUSE list (no CPU readback), and the trace runs twice, indirectly: the trace list as
 * before, the reuse list without the sun's rays (receiver depth plane / ids / projectors
 * rebuilt as always; the sun's raw value = the exact class, 1 or 0, of the reprojected
 * accumulated history -- what the rays return there, so nothing can drift). The
 * temporal and spatial passes run unchanged over the whole mask, so a reused texel is
 * ordinary, valid history to them.
 *
 * WHAT IS REUSED: only texels whose history 3x3 is UNIFORM (fully lit or fully blocked
 * within RenderVKRTShadowCacheEps), converged (RenderVKRTShadowCacheMinHistory frames) and
 * free of moving casters, in tiles outside every SHADOW FOOTPRINT: each character /
 * animesh that MOVED (LLCleanAnimStore's per-slot proofs: world-clean = held / still, never
 * counts; pose-clean = only the root's motion counts, above RenderVKRTShadowCacheMoveTexels
 * mask texels), each moving / spinning ingest object (gVKDynSpheres + gVKDynSphereMoving)
 * and each static change sphere (gVKChangedSpheres, kept RenderVKRTShadowCacheChangeFrames
 * frames: a new BLAS can land in the TLAS a few frames late) -- its box (a character's
 * united with last frame's: the trailing shadow) extruded along the light down to the
 * ground under it (RenderVKRTShadowCacheFloorMargin, at most RenderVKRTShadowCacheMaxShadow)
 * -- the tile re-traces when its receivers' world AABB meets that SWEPT VOLUME (exact slab test;
 * the projected hull binned into screen cells keeps the per-tile list short). Shadow
 * EDGES, penumbrae and translucent tints are always traced; a hashed round-robin slice
 * re-traces every tile within RenderVKRTShadowCacheRefreshFrames
 * (RenderVKRTShadowCacheSunRefreshFrames while the sun drifts). A sun step above
 * RenderVKRTShadowCacheSunJump degrees per frame, a camera cut / teleport / origin shift,
 * a mask resize, a settings change or an overflow of the sphere list re-traces every tile.
 *
 * REUSE PATH: the reused texel's raw sun value is the EXACT class value (1 / 0: what its
 * rays return), never the history value -- the history fed back as its own input drifted
 * under the temporal pass's bilinear reprojection (the "lighter blotch" in solid shadows).
 * With the class value the accumulation converges as without the cache.
 *
 * STATIC CARRY (RenderVKRTShadowCacheCarry): while the camera is bit-identical to the last
 * mask frame's and no ray-traced projector exists, reused blocks skip ALL mask work: the
 * reuse dispatch copies last frame's per-texel outputs into this frame's history slot, the
 * temporal and spatial passes skip those blocks (their final texels keep last frame's).
 *
 * FALLBACKS: GPU side, a frame whose traced share is above RenderVKRTShadowCacheFullAbove
 * runs one plain full trace dispatch (raster order, no reuse dispatch); host side, when the
 * share stays above RenderVKRTShadowCachePlainAbove for 3 frames the classifier is skipped
 * (the cache-off path) and probed every 30 frames.
 *
 * PROJECTORS: their rays run in both dispatches, every frame; static carry is off while
 * any ray-traced projector exists.
 *
 * VRAM: the work buffer (~8.1 B per 8x8 block + 176 B) and two ~420 KB host caster / bin buffers.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
 * $/LicenseInfo$
 */
#include "linden_common.h"

#include "llcleanrenderervk.h"
#include "llcleanrtvk.h"

#include "llviewercontrol.h"
#include "llcharacter.h"
#include "llvoavatar.h"
#include "llcontrolavatar.h"
#include "llvovolume.h"
#include "lldrawable.h"
#include "llworld.h"            // terrain height under a caster (footprint floor)
#include "llappviewer.h"        // gFrameCount
#include "llcleanskinstore.h"   // joint-table slots
#include "llcleananimstore.h"   // per-slot pose / world proofs (moving casters)
#include "../llvkhitch.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <array>
#include <cstring>
#include <iterator>
#include <sstream>

extern std::vector<float> gVKChangedSpheres;   // llcleaningest.cpp: static changes this frame (agent space)
extern bool gVKChangedOverflow;
extern std::vector<float> gVKDynSpheres;       // llcleaningest.cpp: dynamic casters (agent space)
extern std::vector<uint8_t> gVKDynSphereMoving; // llcleaningest.cpp: ... that moved recently / spin

namespace
{
struct RtCacheCfg
{
    bool on = true, log = true, carry = true;
    uint32_t tile = 8, refresh = 60, sun_refresh = 16, change_frames = 12;
    float jump_deg = 0.01f, eps = 0.004f, min_len = 4.f, pad = 0.5f, soft = 0.01f;
    float max_shadow = 128.f, floor_margin = 8.f, move_texels = 0.25f, plain_above = 0.85f, full_above = 0.8f;
    uint32_t spinners = 0;
    bool tight_boxes = true;
    float box_pad = 0.5f;
};

RtCacheCfg rtCacheCfg()
{
    RtCacheCfg c;
    static LLCachedControl<bool> s_on(gSavedSettings, "RenderVKRTShadowCache", false);
    static LLCachedControl<bool> s_log(gSavedSettings, "RenderVKRTShadowCacheLog", true);
    static LLCachedControl<U32>  s_tile(gSavedSettings, "RenderVKRTShadowCacheTile", 8U);
    static LLCachedControl<U32>  s_ref(gSavedSettings, "RenderVKRTShadowCacheRefreshFrames", 60U);
    static LLCachedControl<U32>  s_sref(gSavedSettings, "RenderVKRTShadowCacheSunRefreshFrames", 16U);
    static LLCachedControl<U32>  s_chg(gSavedSettings, "RenderVKRTShadowCacheChangeFrames", 12U);
    static LLCachedControl<F32>  s_jump(gSavedSettings, "RenderVKRTShadowCacheSunJump", 0.01f);
    static LLCachedControl<F32>  s_eps(gSavedSettings, "RenderVKRTShadowCacheEps", 0.004f);
    static LLCachedControl<F32>  s_len(gSavedSettings, "RenderVKRTShadowCacheMinHistory", 4.f);
    static LLCachedControl<F32>  s_pad(gSavedSettings, "RenderVKRTShadowCachePad", 0.5f);
    static LLCachedControl<F32>  s_soft(gSavedSettings, "RenderVKRTShadowSoftness", 0.01f);
    static LLCachedControl<bool> s_carry(gSavedSettings, "RenderVKRTShadowCacheCarry", true);
    static LLCachedControl<F32>  s_maxsh(gSavedSettings, "RenderVKRTShadowCacheMaxShadow", 128.f);
    static LLCachedControl<F32>  s_floor(gSavedSettings, "RenderVKRTShadowCacheFloorMargin", 8.f);
    static LLCachedControl<F32>  s_move(gSavedSettings, "RenderVKRTShadowCacheMoveTexels", 0.25f);
    static LLCachedControl<F32>  s_plain(gSavedSettings, "RenderVKRTShadowCachePlainAbove", 0.85f);
    static LLCachedControl<F32>  s_full(gSavedSettings, "RenderVKRTShadowCacheFullAbove", 0.8f);
    static LLCachedControl<U32>  s_spin(gSavedSettings, "RenderVKRTShadowCacheSpinners", 0U);
    static LLCachedControl<bool> s_tight(gSavedSettings, "RenderVKRTShadowCacheTightBoxes", true);
    static LLCachedControl<F32>  s_bpad(gSavedSettings, "RenderVKRTShadowCacheBoxPad", 0.5f);
    c.on = s_on; c.log = s_log;
    c.tile = (U32)s_tile >= 16u ? 16u : 8u;
    c.refresh = std::clamp<U32>(s_ref, 1u, 100000u);
    c.sun_refresh = std::clamp<U32>(s_sref, 1u, 100000u);
    c.change_frames = std::clamp<U32>(s_chg, 1u, 600u);
    c.jump_deg = llmax((F32)s_jump, 0.f);
    c.eps = llclamp((F32)s_eps, 0.f, 0.05f);   // (the reuse writes the exact class value: keep it tight)
    c.min_len = llclamp((F32)s_len, 1.f, 64.f);
    c.pad = llclamp((F32)s_pad, 0.f, 16.f);
    c.soft = s_soft;
    c.carry = s_carry;
    c.max_shadow = llclamp((F32)s_maxsh, 1.f, 1024.f);
    c.floor_margin = llclamp((F32)s_floor, 0.f, 256.f);
    c.move_texels = llclamp((F32)s_move, 0.f, 64.f);
    c.plain_above = llclamp((F32)s_plain, 0.2f, 1.01f);   // (> 1: never)
    c.full_above = llclamp((F32)s_full, 0.f, 1.01f);
    c.spinners = std::min<U32>(s_spin, 1u);
    c.tight_boxes = s_tight;
    c.box_pad = llclamp((F32)s_bpad, 0.f, 8.f);
    return c;
}

uint32_t cfgKey(const RtCacheCfg& c)
{
    uint32_t h = c.tile;
    auto mix = [&](float v) { uint32_t u; std::memcpy(&u, &v, 4); h = (h ^ u) * 16777619u; };
    mix(c.eps); mix(c.min_len); mix(c.pad); mix(c.soft);
    return h | 1u;   // (0 = never configured)
}

void cacheBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 ss, VkAccessFlags2 sa,
                  VkPipelineStageFlags2 ds, VkAccessFlags2 da)
{
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = ss; mb.srcAccessMask = sa; mb.dstStageMask = ds; mb.dstAccessMask = da;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1; d.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &d);
}

constexpr VkDeviceSize kStatsOff = 48, kStatsBytes = 128;   // rc_stats (clean_rt_mask_cache.glsl)
constexpr uint32_t kFinalize = 1u << 20;   // p0.y (bits 18 / 19: rtMaskStage, the trace's lists)
} // namespace

bool LLCleanRendererVK::rtMaskCacheWanted() const
{
    return mRtCachePipe && mDslRtCache && mSunRtNow && rtCacheCfg().on;
}

void LLCleanRendererVK::rtMaskCacheInit(VkDevice dev)
{
    if (mDslRtCache) return;   // (once: the RT mask init block has no re-entry guard)
    VkMemScope memcat(VKMEM_RT_MASK);
    VkDescriptorSetLayoutBinding b[2]{};
    for (uint32_t i = 0; i < 2; ++i)
    {
        b[i].binding = i; b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    }
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 2; li.pBindings = b;
    VK_CHECK(vkdCreateSetLayout(dev, &li, &mDslRtCache, "mDslRtCache"));
    VK_CHECK(mRtCachePool.create(dev, "rtcache", {{mDslRtCache, 2}}));
    VK_CHECK(mRtCachePool.allocate(mDslRtCache, &mDsetRtCache[0]));
    VK_CHECK(mRtCachePool.allocate(mDslRtCache, &mDsetRtCache[1]));
    const VkDeviceSize pbytes = kRtCacheBinOff + (VkDeviceSize)kRtCacheMaxBinned * 4;   // header, casters, cells, bins
    for (GpuBuffer& p : mRtCacheParams)
    {
        p = mCtx->createBuffer(pbytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (p.mapped) std::memset(p.mapped, 0, (size_t)pbytes);
    }
    // Placeholder work buffer (the trace pipeline statically uses set 4): sized by
    // rtMaskCacheResize once a mask exists.
    mRtCacheWork = mCtx->createBuffer(kRtCacheWorkHead + 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                                      | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
                                      | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    mRtCacheCap = 0;
    for (int s = 0; s < 2; ++s)
    {
        if (!mDsetRtCache[s] || !mRtCacheParams[s].buffer || !mRtCacheWork.buffer) continue;
        const VkDescriptorBufferInfo bi[2] = {{mRtCacheParams[s].buffer, 0, VK_WHOLE_SIZE},
                                              {mRtCacheWork.buffer, 0, VK_WHOLE_SIZE}};
        VkWriteDescriptorSet w[2]{};
        for (int i = 0; i < 2; ++i)
        {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet = mDsetRtCache[s];
            w[i].dstBinding = (uint32_t)i; w[i].descriptorCount = 1;
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[i].pBufferInfo = &bi[i];
        }
        vkUpdateDescriptorSets(dev, 2, w, 0, nullptr);
    }
}

void LLCleanRendererVK::rtMaskCacheDestroy(VkDevice dev)
{
    if (mRtCachePipe) vkDestroyPipeline(dev, mRtCachePipe, nullptr);
    mRtCachePipe = VK_NULL_HANDLE;
    for (GpuBuffer* b : {&mRtCacheParams[0], &mRtCacheParams[1], &mRtCacheWork, &mRtCacheStatsRb[0], &mRtCacheStatsRb[1],
                         &mRtCacheCountRb[0], &mRtCacheCountRb[1]})
        if (b->buffer) { mCtx->destroyBuffer(*b); *b = GpuBuffer{}; }
    mRtCachePool.destroy();
    if (mDslRtCache) vkdDestroySetLayout(dev, mDslRtCache);
    mDslRtCache = VK_NULL_HANDLE;
    mDsetRtCache[0] = mDsetRtCache[1] = VK_NULL_HANDLE;
    mRtCacheCap = 0;
    mRtCacheNow = mRtCacheWas = false;
    mRtCacheStatsPending[0] = mRtCacheStatsPending[1] = false;
    mRtCacheCountPending[0] = mRtCacheCountPending[1] = false;
}

// The mask was (re)allocated at w x h (ensureRtMask's resize branch, device idle).
void LLCleanRendererVK::rtMaskCacheResize(VkCommandBuffer cmd, uint32_t w, uint32_t h)
{
    if (!mDslRtCache || !mDsetRtCache[0] || !mDsetRtCache[1]) return;
    VkMemScope memcat(VKMEM_RT_MASK);
    const uint32_t blocks = w > 1u && h > 1u ? ((w + 7u) / 8u) * ((h + 7u) / 8u) : 0u;
    if (blocks == mRtCacheCap && mRtCacheWork.buffer) return;
    if (mRtCacheWork.buffer) { mCtx->destroyBuffer(mRtCacheWork); mRtCacheWork = GpuBuffer{}; }
    mRtCacheCap = 0;
    const VkBufferUsageFlags u = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT
                               | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    // two block lists + the static-carry bitmap (clean_rt_mask_cache.glsl rcCarried)
    const VkDeviceSize bytes = kRtCacheWorkHead + 8ull * std::max(blocks, 1u) + 4ull * (std::max(blocks, 1u) / 32u + 1u);
    mRtCacheWork = mCtx->createBuffer(bytes, u, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!mRtCacheWork.buffer)   // (tiny: should never fail; the cache then stays off)
        mRtCacheWork = mCtx->createBuffer(kRtCacheWorkHead + 16, u, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    else
        mRtCacheCap = blocks;
    if (!mRtCacheWork.buffer) return;
    vkCmdFillBuffer(cmd, mRtCacheWork.buffer, 0, VK_WHOLE_SIZE, 0u);
    cacheBarrier(cmd, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT
                 | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                 VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                 | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
    for (int s = 0; s < 2; ++s)
    {
        const VkDescriptorBufferInfo bi{mRtCacheWork.buffer, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        wr.dstSet = mDsetRtCache[s]; wr.dstBinding = 1; wr.descriptorCount = 1;
        wr.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; wr.pBufferInfo = &bi;
        vkUpdateDescriptorSets(mCtx->device(), 1, &wr, 0, nullptr);
    }
    // (stats restart with the buffer)
    mRtCacheStatsPending[0] = mRtCacheStatsPending[1] = false;
    mRtCacheCountPending[0] = mRtCacheCountPending[1] = false;
    mRtCacheHost = RtCacheHost{};
    mRtCacheWas = false;
    mRtCachePrevCamOk = false;
}

// invalidateProjShadows, before it clears the ingest's list: this frame's static change
// spheres, kept for RenderVKRTShadowCacheChangeFrames mask frames.
void LLCleanRendererVK::rtMaskCacheNoteSpheres()
{
    if (!rtMaskCacheWanted())
    {
        mRtCacheChanged.clear();
        mRtCacheOverflowUntil = 0;
        return;
    }
    const RtCacheCfg cfg = rtCacheCfg();
    const uint32_t until = mRtCacheFrame + cfg.change_frames;
    if (gVKChangedOverflow)
    {
        mRtCacheOverflowUntil = until;
        mRtCacheChanged.clear();
        return;
    }
    const size_t n = gVKChangedSpheres.size() / 4;
    if (mRtCacheChanged.size() + n > kRtCacheMaxCasters / 2)
    {
        mRtCacheOverflowUntil = until;
        mRtCacheChanged.clear();
        return;
    }
    for (size_t i = 0; i < n; ++i)
    {
        RtCacheSphere s;
        std::memcpy(s.s, &gVKChangedSpheres[i * 4], sizeof(s.s));
        if (!std::isfinite(s.s[0]) || !std::isfinite(s.s[1]) || !std::isfinite(s.s[2]) || !std::isfinite(s.s[3])) continue;
        s.until = until;
        mRtCacheChanged.push_back(s);
    }
}

// ---- SWEPT CASTER VOLUMES (round 3) ----
// A caster's box swept along the light's travel (-L) down to the ground under it (terrain -
// RenderVKRTShadowCacheFloorMargin, at most RenderVKRTShadowCacheMaxShadow m). The GPU tests
// each tile's receivers' world AABB against the swept box exactly (clean_rt_mask_cache.glsl
// rcSweptHit). For the per-tile list the host projects the volume with the mask's own
// unjittered camera-relative view-projection -- the convex hull of its 16 corners, near-
// clipped -- and bins that HULL (not its bounding rectangle: a long low-sun strip's
// rectangle covered a third of the screen) into coarse screen cells.
namespace
{
struct FpCtx
{
    const float* M = nullptr;   // mViewProjRel (column-major: clip = M * (P - cam))
    double cam[3] = {0.0, 0.0, 0.0};
    float W = 1.f, H = 1.f;     // mask texels
    float L[3] = {0.f, 0.f, 1.f};
    float max_len = 128.f, floor_margin = 8.f;
};

struct Fp
{
    float mn[3], mx[3];   // camera-relative box
    float t;              // sweep length along -L
    float kind;
    std::vector<float> hull;   // projected convex hull (mask texels), x, y pairs, CCW
    float bb[4];               // its bound
};

float cross2(const float* o, const float* a, const float* b)
{
    return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0]);
}

// Monotone chain; pts as x, y pairs (modified). Returns the hull, CCW.
void convexHull(std::vector<float>& pts, std::vector<float>& out)
{
    const size_t n = pts.size() / 2;
    out.clear();
    if (n == 0) return;
    std::vector<std::array<float, 2>> p(n);
    for (size_t i = 0; i < n; ++i) p[i] = {pts[i * 2], pts[i * 2 + 1]};
    std::sort(p.begin(), p.end());
    if (n < 3) { for (auto& q : p) { out.push_back(q[0]); out.push_back(q[1]); } return; }
    std::vector<std::array<float, 2>> h(2 * n);
    size_t k = 0;
    for (size_t i = 0; i < n; ++i)
    {
        while (k >= 2 && cross2(h[k - 2].data(), h[k - 1].data(), p[i].data()) <= 0.f) --k;
        h[k++] = p[i];
    }
    for (size_t i = n - 1, t = k + 1; i > 0; --i)
    {
        while (k >= t && cross2(h[k - 2].data(), h[k - 1].data(), p[i - 1].data()) <= 0.f) --k;
        h[k++] = p[i - 1];
    }
    for (size_t i = 0; i + 1 < k; ++i) { out.push_back(h[i][0]); out.push_back(h[i][1]); }
}

// false: entirely behind the camera / off screen.
bool sweptFootprint(const FpCtx& c, const float mn[3], const float mx[3], Fp& f)
{
    float zf = mn[2];
    {
        const LLVector3 at(0.5f * (mn[0] + mx[0]), 0.5f * (mn[1] + mx[1]), mn[2]);
        if (LLWorld* w = LLWorld::getInstance())
        {
            const F32 land = w->resolveLandHeightAgent(at);
            if (std::isfinite(land)) zf = std::min(zf, land);
        }
    }
    zf -= c.floor_margin;
    float t = c.max_len;
    if (c.L[2] > 0.02f) t = std::clamp((mx[2] - zf) / c.L[2], 0.f, c.max_len);
    for (int k = 0; k < 3; ++k)
    {
        f.mn[k] = (float)((double)mn[k] - c.cam[k]);
        f.mx[k] = (float)((double)mx[k] - c.cam[k]);
    }
    f.t = t;
    float px[16], py[16], pw[16];
    for (int i = 0; i < 16; ++i)
    {
        float r[3] = {(i & 1) ? f.mx[0] : f.mn[0], (i & 2) ? f.mx[1] : f.mn[1], (i & 4) ? f.mx[2] : f.mn[2]};
        if (i >= 8) for (int k = 0; k < 3; ++k) r[k] -= c.L[k] * t;
        const float* M = c.M;
        px[i] = M[0] * r[0] + M[4] * r[1] + M[8] * r[2] + M[12];
        py[i] = M[1] * r[0] + M[5] * r[1] + M[9] * r[2] + M[13];
        pw[i] = M[3] * r[0] + M[7] * r[1] + M[11] * r[2] + M[15];
    }
    constexpr float wn = 0.05f;
    static std::vector<float> pts;
    pts.clear();
    auto add = [&](float x, float y, float w)
    {
        const float sx = (x / w * 0.5f + 0.5f) * c.W, sy = (y / w * 0.5f + 0.5f) * c.H;
        if (std::isfinite(sx) && std::isfinite(sy)) { pts.push_back(sx); pts.push_back(sy); }
    };
    for (int i = 0; i < 16; ++i)
        if (pw[i] > wn) add(px[i], py[i], pw[i]);
    // Near-plane crossings of every pair (all inside the hull: conservative).
    for (int i = 0; i < 16; ++i)
        for (int j = i + 1; j < 16; ++j)
            if ((pw[i] > wn) != (pw[j] > wn))
            {
                const float s = (wn - pw[i]) / (pw[j] - pw[i]);
                add(px[i] + s * (px[j] - px[i]), py[i] + s * (py[j] - py[i]), wn);
            }
    if (pts.empty()) return false;
    convexHull(pts, f.hull);
    f.bb[0] = f.bb[1] = 1e30f; f.bb[2] = f.bb[3] = -1e30f;
    for (size_t i = 0; i < f.hull.size(); i += 2)
    {
        f.bb[0] = std::min(f.bb[0], f.hull[i]); f.bb[2] = std::max(f.bb[2], f.hull[i]);
        f.bb[1] = std::min(f.bb[1], f.hull[i + 1]); f.bb[3] = std::max(f.bb[3], f.hull[i + 1]);
    }
    constexpr float pad = 2.f;   // texels (jittered samples, rounding)
    return !(f.bb[2] + pad <= 0.f || f.bb[3] + pad <= 0.f || f.bb[0] - pad >= c.W || f.bb[1] - pad >= c.H);
}

// Does the cell [x0, x1) x [y0, y1) meet the hull grown by pad? (SAT on the hull's edges;
// the bound was tested by the caller's cell range.)
bool cellMeetsHull(const std::vector<float>& h, float x0, float y0, float x1, float y1, float pad)
{
    const size_t n = h.size() / 2;
    if (n < 3) return true;   // (degenerate: the bound decides)
    for (size_t i = 0; i < n; ++i)
    {
        const float* a = &h[i * 2];
        const float* b = &h[((i + 1) % n) * 2];
        // CCW: outward normal of edge a -> b = (dy, -dx)
        float nx = b[1] - a[1], ny = -(b[0] - a[0]);
        const float l = std::sqrt(nx * nx + ny * ny);
        if (l < 1e-6f) continue;
        nx /= l; ny /= l;
        // the cell corner furthest against the normal
        const float cx = nx > 0.f ? x0 : x1, cy = ny > 0.f ? y0 : y1;
        if ((cx - a[0]) * nx + (cy - a[1]) * ny > pad) return false;
    }
    return true;
}

// Loose (RenderVKRTShadowCacheTightBoxes 0, computeAvatarVis' cull box): LL's spatial
// extents united with the render position +-2 m, padded 1 m + 25%, animesh root volume at
// least 4 m. A cull must never drop a draw; here a box that is a little small only costs a
// leading shadow edge for one frame (the rays' moving-caster bits re-trace it from then on),
// while the loose box turned a 0.5 m fish into an 8 m cube swept along a low sun. Tight
// (default): LL's extents united with the render position, padded `pad` m; animesh adds its
// root volume's own radius.
bool avatarBox(LLVOAvatar* av, float box[6], bool tight, float tpad)
{
    const LLVector4a* ext = av->mDrawable->getSpatialExtents();
    const LLVector3 pos = av->getRenderPosition();
    if (!std::isfinite(pos.mV[0]) || !std::isfinite(pos.mV[1]) || !std::isfinite(pos.mV[2])) return false;
    float* mn = box;
    float* mx = box + 3;
    bool ok = true;
    for (int k = 0; k < 3; ++k)
    {
        mn[k] = ext[0].getF32ptr()[k]; mx[k] = ext[1].getF32ptr()[k];
        if (!std::isfinite(mn[k]) || !std::isfinite(mx[k]) || mx[k] - mn[k] > 512.f || mx[k] < mn[k]) ok = false;
    }
    if (!ok)
        for (int k = 0; k < 3; ++k) { mn[k] = mx[k] = pos.mV[k]; }
    for (int k = 0; k < 3; ++k)
    {
        if (tight)
        {
            mn[k] = std::min(mn[k], pos.mV[k]) - tpad; mx[k] = std::max(mx[k], pos.mV[k]) + tpad;
            continue;
        }
        mn[k] = std::min(mn[k], pos.mV[k] - 2.f); mx[k] = std::max(mx[k], pos.mV[k] + 2.f);
        const float pad = 1.f + 0.25f * (mx[k] - mn[k]);
        mn[k] -= pad; mx[k] += pad;
    }
    if (av->isControlAvatar())
    {
        LLVOVolume* root = ((LLControlAvatar*)av)->mRootVolp;
        if (root && !root->isDead() && root->mDrawable.notNull())
        {
            const LLVector3 rp = root->getRenderPosition();
            const float rr = root->mDrawable->getRadius();
            const float r = tight ? (std::isfinite(rr) ? std::max(rr, 0.f) + tpad : 4.f) : std::max(rr, 4.f);
            for (int k = 0; k < 3; ++k)
                if (std::isfinite(rp.mV[k]))
                { mn[k] = std::min(mn[k], rp.mV[k] - r); mx[k] = std::max(mx[k], rp.mV[k] + r); }
        }
    }
    return true;
}
} // namespace

// This frame's swept casters + bins into the slot's params block (base; nullptr: only run
// the trackers). Returns the caster count (> kRtCacheMaxCasters: too many -- the caller
// forces every tile); *binned: the cell lists fit.
uint32_t LLCleanRendererVK::rtMaskCacheFootprints(uint8_t* base, uint32_t W, uint32_t H, bool* binned)
{
    const RtCacheCfg cfg = rtCacheCfg();
    FpCtx c;
    c.M = mViewProjRel;
    c.cam[0] = mRtMaskPush.cam_time[0]; c.cam[1] = mRtMaskPush.cam_time[1]; c.cam[2] = mRtMaskPush.cam_time[2];
    c.W = (float)W; c.H = (float)H;
    {
        const float* ld = &mAtmos[7 * 4];
        const float l = std::sqrt(ld[0] * ld[0] + ld[1] * ld[1] + ld[2] * ld[2]);
        if (l > 1e-6f) for (int k = 0; k < 3; ++k) c.L[k] = ld[k] / l;
    }
    c.max_len = cfg.max_shadow;
    c.floor_margin = cfg.floor_margin;
    // World size of one mask texel per metre of distance (the moving threshold's unit):
    // |row 1| of P x rotation-only view = P11.
    const float p11 = std::sqrt(mViewProjRel[1] * mViewProjRel[1] + mViewProjRel[5] * mViewProjRel[5]
                                + mViewProjRel[9] * mViewProjRel[9]);
    const float texel_per_m = p11 > 1e-6f ? 2.f / (p11 * (float)H) : 0.f;
    static std::vector<Fp> s_fp;
    size_t n = 0;
    auto emit = [&](const float mn[3], const float mx[3], float kind)
    {
        if (!base) return;
        if (s_fp.size() <= n) s_fp.resize(n + 1);
        Fp& f = s_fp[n];
        if (!sweptFootprint(c, mn, mx, f)) { ++mRtCacheHost.rects_off; return; }
        f.kind = kind;
        ++mRtCacheHost.rects;
        double a = 0.0;   // hull area (shoelace), clipped share approximated by the bound
        const size_t hn = f.hull.size() / 2;
        for (size_t i = 0; i < hn; ++i)
        {
            const float* p = &f.hull[i * 2];
            const float* q = &f.hull[((i + 1) % hn) * 2];
            a += (double)p[0] * q[1] - (double)q[0] * p[1];
        }
        mRtCacheHost.area += std::min(0.5 * std::fabs(a) / ((double)W * H), 1.0);
        ++n;
    };

    // ---- characters: only the ones that MOVED ----
    // Joint-table slot -> LLCleanAnimStore's proofs: slotWorldClean = pose AND root world
    // transform identical to the previous frame (held / still: no shadow change);
    // slotPoseClean = the pose relative to the root unchanged (then only the root's motion
    // counts, against RenderVKRTShadowCacheMoveTexels mask texels at the character's distance).
    static std::vector<std::pair<LLUUID, U32>> s_slots;
    static std::unordered_map<LLUUID, U32> s_slot_of;
    LLCleanSkinStore::instance().copyAvatarSlots(s_slots);
    s_slot_of.clear();
    for (const auto& sl : s_slots) s_slot_of[sl.first] = sl.second;
    const uint32_t frame = (uint32_t)gFrameCount;
    const bool built = mAnimBuiltFrame == frame;
    const LLCleanAnimStore& anim = LLCleanAnimStore::instance();
    for (LLCharacter* ch : LLCharacter::sInstances)
    {
        LLVOAvatar* av = static_cast<LLVOAvatar*>(ch);
        if (!av || av->isDead() || av->isUIAvatar() || av->mDrawable.isNull()) continue;
        float box[6];
        if (!avatarBox(av, box, cfg.tight_boxes, cfg.box_pad)) continue;
        const LLVector3 pos = av->getRenderPosition();
        const LLQuaternion rot = av->getRenderRotation();
        RtCacheAv& st = mRtCacheAv[av->getID()];
        const bool known = st.seen != 0u && st.seen + 2u >= mRtCacheFrame;
        bool moving = true;
        {
            const auto it = s_slot_of.find(av->getID());
            const U32 slot = it != s_slot_of.end() ? it->second : 0xFFFFFFFFu;
            const bool slot_ok = built && slot < kAvatarSlotCap && !mRigCacheSlotDirty[slot];
            // world-clean: still whatever we tracked; pose-clean needs last frame's root
            if (slot_ok && anim.slotWorldClean(slot)) moving = false;
            else if (known && slot_ok && anim.slotPoseClean(slot))
            {
                // root-only motion: translation + rotation x the box's half extent
                const float dx = pos.mV[0] - st.pos[0], dy = pos.mV[1] - st.pos[1], dz = pos.mV[2] - st.pos[2];
                const float d = std::fabs(rot.mQ[0] * st.rot[0] + rot.mQ[1] * st.rot[1] + rot.mQ[2] * st.rot[2]
                                          + rot.mQ[3] * st.rot[3]);
                const float ang = 2.f * std::acos(std::min(d, 1.f));
                const float half = 0.5f * std::sqrt((box[3] - box[0]) * (box[3] - box[0]) + (box[4] - box[1]) * (box[4] - box[1])
                                                    + (box[5] - box[2]) * (box[5] - box[2]));
                const float disp = std::sqrt(dx * dx + dy * dy + dz * dz) + ang * half;
                const double ex = (double)pos.mV[0] - c.cam[0], ey = (double)pos.mV[1] - c.cam[1], ez = (double)pos.mV[2] - c.cam[2];
                const float dist = (float)std::sqrt(ex * ex + ey * ey + ez * ez);
                moving = disp > cfg.move_texels * texel_per_m * std::max(dist, 1.f);
            }
        }
        const bool animesh = av->isControlAvatar();
        if (moving)
        {
            // this frame's box united with last frame's (the trailing shadow)
            float mn[3], mx[3];
            for (int k = 0; k < 3; ++k)
            {
                mn[k] = known ? std::min(box[k], st.box[k]) : box[k];
                mx[k] = known ? std::max(box[3 + k], st.box[3 + k]) : box[3 + k];
            }
            emit(mn, mx, 0.f);
            ++(animesh ? mRtCacheHost.am_mov : mRtCacheHost.av_mov);
        }
        else ++(animesh ? mRtCacheHost.am_still : mRtCacheHost.av_still);
        std::memcpy(st.box, box, sizeof(st.box));
        st.pos[0] = pos.mV[0]; st.pos[1] = pos.mV[1]; st.pos[2] = pos.mV[2];
        for (int k = 0; k < 4; ++k) st.rot[k] = rot.mQ[k];
        st.seen = mRtCacheFrame;
    }
    // forget characters not seen for a while
    if ((mRtCacheFrame & 255u) == 0u)
        for (auto it = mRtCacheAv.begin(); it != mRtCacheAv.end();)
            it = (it->second.seen + 600u < mRtCacheFrame) ? mRtCacheAv.erase(it) : std::next(it);

    // ---- the ingest's dynamic objects: moving ones; spinners by RenderVKRTShadowCacheSpinners ----
    // (<GMDX> 3 = a still texture-animated caster: its shadow changes every frame, re-traced as 1)
    // gVKDynSphereMoving: 1 moved within 2 frames, 2 a GPU spinner (its sphere covers every
    // rotation, so its swept volume holds still while it spins). Spinners 0 (default): the
    // volume re-traces every frame (an asymmetric spinner's shadow changes inside it every
    // frame); 1: only when its sphere changed since last frame -- inside, the rays' moving-
    // caster bits re-trace where its shadow falls and the edge ring follows a slow spinner,
    // but a fast asymmetric one (a windmill blade) can leave its new shadow untraced.
    static std::vector<std::array<float, 4>> s_spin_prev, s_spin_now;
    s_spin_now.clear();
    const size_t nmv = gVKDynSpheres.size() / 4;
    for (size_t i = 0; i < nmv; ++i)
    {
        const uint8_t m = i < gVKDynSphereMoving.size() ? gVKDynSphereMoving[i] : 1u;
        const float* s = &gVKDynSpheres[i * 4];
        if (m == 0u) { ++mRtCacheHost.mv_still; continue; }
        if (m == 2u)
        {
            s_spin_now.push_back({s[0], s[1], s[2], s[3]});
            bool same = false;
            if (cfg.spinners == 1u)
                for (const auto& q : s_spin_prev)
                    if (q[0] == s[0] && q[1] == s[1] && q[2] == s[2] && q[3] == s[3]) { same = true; break; }
            if (same) { ++mRtCacheHost.sp_still; continue; }
            ++mRtCacheHost.sp_mov;
        }
        else ++mRtCacheHost.mv_mov;
        const float r = std::max(s[3], 0.25f) + 0.25f;
        const float mn[3] = {s[0] - r, s[1] - r, s[2] - r}, mx[3] = {s[0] + r, s[1] + r, s[2] + r};
        emit(mn, mx, 0.f);
    }
    s_spin_prev.swap(s_spin_now);
    // ---- static changes (kept ChangeFrames) ----
    for (const RtCacheSphere& sp : mRtCacheChanged)
    {
        const float r = std::max(sp.s[3], 0.25f) + 0.25f;
        const float mn[3] = {sp.s[0] - r, sp.s[1] - r, sp.s[2] - r}, mx[3] = {sp.s[0] + r, sp.s[1] + r, sp.s[2] + r};
        emit(mn, mx, 1.f);
    }
    if (!base) return 0u;
    if (n > kRtCacheMaxCasters) return (uint32_t)n;

    // ---- upload the casters (RcCaster: a = min, t; b = max, kind) ----
    float* cast = (float*)(base + kRtCacheHeader);
    for (size_t i = 0; i < n; ++i)
    {
        const Fp& f = s_fp[i];
        float* o = cast + i * 8;
        o[0] = f.mn[0]; o[1] = f.mn[1]; o[2] = f.mn[2]; o[3] = f.t;
        o[4] = f.mx[0]; o[5] = f.mx[1]; o[6] = f.mx[2]; o[7] = f.kind;
    }
    // ---- bin the projected hulls into screen cells ----
    uint32_t cs = 64u;
    while (((W + cs - 1u) / cs) * ((H + cs - 1u) / cs) > kRtCacheMaxCells) cs *= 2u;
    const uint32_t cw = (W + cs - 1u) / cs, chh = (H + cs - 1u) / cs, ncell = cw * chh;
    static std::vector<std::vector<uint32_t>> s_cell;
    if (s_cell.size() < ncell) s_cell.resize(ncell);
    for (uint32_t i = 0; i < ncell; ++i) s_cell[i].clear();
    size_t total = 0;
    constexpr float pad = 2.f;
    for (size_t i = 0; i < n && total <= kRtCacheMaxBinned; ++i)
    {
        const Fp& f = s_fp[i];
        const int cx0 = std::max(0, (int)std::floor((f.bb[0] - pad) / (float)cs));
        const int cy0 = std::max(0, (int)std::floor((f.bb[1] - pad) / (float)cs));
        const int cx1 = std::min((int)cw - 1, (int)std::floor((f.bb[2] + pad) / (float)cs));
        const int cy1 = std::min((int)chh - 1, (int)std::floor((f.bb[3] + pad) / (float)cs));
        for (int y = cy0; y <= cy1; ++y)
            for (int x = cx0; x <= cx1; ++x)
                if (cellMeetsHull(f.hull, (float)(x * cs), (float)(y * cs), (float)((x + 1) * cs), (float)((y + 1) * cs), pad))
                {
                    s_cell[(size_t)y * cw + x].push_back((uint32_t)i);
                    ++total;
                }
    }
    const bool fit = total <= kRtCacheMaxBinned;
    uint32_t* cells = (uint32_t*)(base + kRtCacheCellsOff);
    uint32_t* bins = (uint32_t*)(base + kRtCacheBinOff);
    if (fit)
    {
        uint32_t at = 0;
        for (uint32_t i = 0; i < ncell; ++i)
        {
            cells[i * 2] = at;
            cells[i * 2 + 1] = (uint32_t)s_cell[i].size();
            for (uint32_t ci : s_cell[i]) bins[at++] = ci;
        }
        mRtCacheHost.binned += total;
    }
    // header word h2 (cell size, cells per row, rows, binned)
    const uint32_t h2[4] = {cs, cw, chh, fit ? 1u : 0u};
    std::memcpy(base + 64, h2, 16);
    if (binned) *binned = fit;
    return (uint32_t)n;
}

// After rtMaskPushFill, before the trace (stage 0): decide this frame's tiles on the GPU.
void LLCleanRendererVK::rtMaskCacheRecord(VkCommandBuffer cmd)
{
    rtMaskCacheHarvest();   // this slot's stats readback (acquire waited its fence)
    mRtCacheNow = false;
    const uint32_t slot = mFrameSlot & 1u;
    const RtCacheCfg cfg = rtCacheCfg();
    const uint32_t W = mRtMaskW, H = mRtMaskH;
    const uint32_t blocks = ((W + 7u) / 8u) * ((H + 7u) / 8u);

    // ---- PLAIN FALLBACK: this slot's per-frame block counts (2 frames old) ----
    if (mRtCacheCountPending[slot] && mRtCacheCountRb[slot].mapped)
    {
        mRtCacheCountPending[slot] = false;
        uint32_t cnt[4];
        std::memcpy(cnt, mRtCacheCountRb[slot].mapped, sizeof(cnt));
        const uint32_t tot = cnt[0] + cnt[1];
        if (tot > 0u)
        {
            const float frac = (float)cnt[0] / (float)tot;
            if (!mRtCachePlain)
            {
                mRtCacheHighRun = frac > cfg.plain_above ? mRtCacheHighRun + 1u : 0u;
                if (mRtCacheHighRun >= 3u)
                {
                    mRtCachePlain = true;   // the classifier does not pay here: skip it
                    mRtCachePlainProbe = mRtCacheFrame + 30u;
                    mRtCacheHighRun = 0;
                }
            }
            else if (frac < cfg.plain_above - 0.15f) mRtCachePlain = false;   // a probe found reuse again
        }
    }

    // (#1075: not in a stereo frame -- its block lists / carry are one view's; the plain trace runs)
    const bool ok = rtMaskCacheWanted() && mRtMaskDenoise && !mOffscreen && !stereoActive() && W > 1u && H > 1u
                 && mRtCacheCap >= blocks && mRtCacheWork.buffer && mRtCacheParams[slot].mapped
                 && mDsetRtCache[slot] && mDsetRtMask[mRtMaskSet & 1u];
    // STATIC CARRY: this frame's camera vs the last mask frame's (bit-identical), and no ray-
    // traced projector this frame or last (a carried block copies last frame's outputs whole).
    const bool proj_now = mProjMode == 2u && mProjRecords > 0u;
    const double cx = mRtMaskPush.cam_time[0], cy = mRtMaskPush.cam_time[1], cz = mRtMaskPush.cam_time[2];
    const bool cam_same = mRtCachePrevCamOk && std::memcmp(mRtCachePrevVP, mViewProjRel, sizeof(mRtCachePrevVP)) == 0
                       && mRtCachePrevCam[0] == cx && mRtCachePrevCam[1] == cy && mRtCachePrevCam[2] == cz
                       && mRtCachePrevWH[0] == W && mRtCachePrevWH[1] == H;
    const bool carry = cfg.carry && cam_same && !proj_now && !mRtCachePrevProj;
    std::memcpy(mRtCachePrevVP, mViewProjRel, sizeof(mRtCachePrevVP));
    mRtCachePrevCam[0] = cx; mRtCachePrevCam[1] = cy; mRtCachePrevCam[2] = cz;
    mRtCachePrevWH[0] = W; mRtCachePrevWH[1] = H;
    mRtCachePrevCamOk = ok;
    mRtCachePrevProj = proj_now;
    if (!ok)
    {
        mRtCacheWas = false;
        return;
    }
    ++mRtCacheFrame;
    mRtCacheChanged.erase(std::remove_if(mRtCacheChanged.begin(), mRtCacheChanged.end(),
                                         [&](const RtCacheSphere& s) { return s.until < mRtCacheFrame; }),
                          mRtCacheChanged.end());
    // Plain fallback frames: the full trace as without the cache (the history stays valid:
    // every texel traced), the classifier probed again every 30 frames.
    if (mRtCachePlain && mRtCacheFrame < mRtCachePlainProbe)
    {
        mRtCacheHost.frames++;
        mRtCacheHost.plain++;
        // (the character tracker keeps running: at the probe every character would
        // otherwise be "unknown" -> moving, and the probe would never leave plain mode)
        rtMaskCacheFootprints(nullptr, W, H, nullptr);
        return;
    }
    if (mRtCachePlain) mRtCachePlainProbe = mRtCacheFrame + 30u;

    // ---- force every tile (no usable history / a global change) ----
    const uint32_t key = cfgKey(cfg);
    const uint32_t modeBits = (uint32_t)std::lround(mRtMaskPush.p0[1]);
    const bool hist_valid = (modeBits & 2u) != 0u;
    const float step_deg = mRtCacheSunStep < 0.f ? 1e9f : mRtCacheSunStep * 57.2957795f;
    const bool jump = step_deg > cfg.jump_deg;
    bool force = !mRtMaskHistOk || !hist_valid || !mRtCacheWas || key != mRtCacheCfgKey || jump
              || mRtCacheOverflowUntil >= mRtCacheFrame;
    mRtCacheCfgKey = key;
    const bool drift = !jump && mRtCacheSunStep > 1e-6f;

    // ---- swept caster volumes + their screen bins ----
    uint8_t* base = (uint8_t*)mRtCacheParams[slot].mapped;
    uint32_t nrect = 0;
    bool binned = false;
    if (!force)
    {
        nrect = rtMaskCacheFootprints(base, W, H, &binned);
        if (nrect > kRtCacheMaxCasters) { force = true; nrect = 0; }
    }
    if (force)
    {
        const uint32_t h2[4] = {64u, 1u, 1u, 0u};
        std::memcpy(base + 64, h2, 16);
    }
    else if (!binned) mRtCacheHost.unbinned++;
    // ---- header (clean_rt_mask_cache.glsl RtCacheParams) ----
    {
        const bool carry_now = carry && !force;
        uint32_t h0[4] = {cfg.tile, force ? 0u : nrect, 0u,
                          (force ? 1u : 0u) | (cfg.log ? 2u : 0u) | (carry_now ? 4u : 0u)};
        const uint32_t period = drift ? std::min(cfg.refresh, cfg.sun_refresh) : cfg.refresh;
        uint32_t h1[4] = {period, mRtCacheFrame, (W + 7u) / 8u, mRtCacheCap};
        float f0[4] = {cfg.eps, cfg.min_len, cfg.pad, cfg.full_above};
        // view depth of a camera-relative point = the clip w of mViewProjRel (row 3)
        float vz[4] = {mViewProjRel[3], mViewProjRel[7], mViewProjRel[11], mViewProjRel[15]};
        std::memcpy(base + 0, h0, 16);
        std::memcpy(base + 16, h1, 16);
        std::memcpy(base + 32, f0, 16);
        std::memcpy(base + 48, vz, 16);
        // rc_D: the shadow direction (-L) and tan(sun disc radius) (the penumbra spread)
        {
            const float* ld = &mAtmos[7 * 4];
            const float l = std::sqrt(ld[0] * ld[0] + ld[1] * ld[1] + ld[2] * ld[2]);
            const float disc = (float)std::tan(std::max((double)llclamp(cfg.soft, 0.f, 1.f) * 5.0, 0.05) * 0.01745329252);
            float D[4] = {0.f, 0.f, -1.f, disc};
            if (l > 1e-6f) { D[0] = -ld[0] / l; D[1] = -ld[1] / l; D[2] = -ld[2] / l; }
            std::memcpy(base + 80, D, 16);
        }
        if (carry_now) mRtCacheHost.carry++;
    }
    mRtCacheHost.frames++;
    if (force) mRtCacheHost.forced++;
    if (jump) mRtCacheHost.jump++;
    if (drift) mRtCacheHost.drift++;
    mRtCacheHost.dyn += force ? 0u : nrect;
    mRtCacheHost.chg += force ? 0u : (uint32_t)mRtCacheChanged.size();
    const uint32_t T = cfg.tile;
    const uint32_t tx = (W + T - 1u) / T, ty = (H + T - 1u) / T;
    mRtCacheHost.tiles += tx * ty;

    // ---- record: zero counts / args, classify, finalize ----
    // WAR: last frame's indirect / list reads and this frame's earlier users -> the fill.
    cacheBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT
                      | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                 VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    vkCmdFillBuffer(cmd, mRtCacheWork.buffer, 0, kStatsOff, 0u);   // rc_count + rc_args
    cacheBarrier(cmd, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    const VkDescriptorSet sets[5] = {mDsetDraw0, mDsetDraw1, mRT->consumerSet(), mDsetRtMask[mRtMaskSet & 1u],
                                     mDsetRtCache[slot]};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mRtMaskLayout, 0, 5, sets, 0, nullptr);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mRtCachePipe);
    RtMaskPushData p = mRtMaskPush;
    vkCmdPushConstants(cmd, mRtMaskLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
    vkCmdDispatch(cmd, tx, ty, 1);
    cacheBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    p.p0[1] = (float)(modeBits | kFinalize);
    vkCmdPushConstants(cmd, mRtMaskLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
    vkCmdDispatch(cmd, 1, 1, 1);
    // Args / lists / carry bitmap -> the indirect trace dispatches, the temporal / spatial
    // passes' carry test (and the copies below).
    cacheBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                 | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                 VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT
                 | VK_ACCESS_2_TRANSFER_READ_BIT);
    gpuSubMark(cmd, "c_rtmask_cls");

    // ---- per-frame counts (the plain fallback) ----
    {
        GpuBuffer& cb = mRtCacheCountRb[slot];
        if (!cb.buffer)
            cb = mCtx->createBuffer(16, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (cb.buffer && cb.mapped)
        {
            const VkBufferCopy c{0, 0, 16};
            vkCmdCopyBuffer(cmd, mRtCacheWork.buffer, cb.buffer, 1, &c);
            cacheBarrier(cmd, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
            mRtCacheCountPending[slot] = true;
        }
    }
    // ---- stats readback (~5 s; harvested when this frame slot comes round again) ----
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (cfg.log && now >= mRtCacheLogNext && !mRtCacheStatsPending[slot])
    {
        GpuBuffer& rb = mRtCacheStatsRb[slot];
        if (!rb.buffer)
            rb = mCtx->createBuffer(kStatsBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (rb.buffer && rb.mapped)
        {
            mRtCacheLogNext = now + 5.0;
            const VkBufferCopy c{kStatsOff, 0, kStatsBytes};
            vkCmdCopyBuffer(cmd, mRtCacheWork.buffer, rb.buffer, 1, &c);
            cacheBarrier(cmd, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_HOST_BIT,
                         VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_HOST_READ_BIT);
            vkCmdFillBuffer(cmd, mRtCacheWork.buffer, kStatsOff, kStatsBytes, 0u);
            cacheBarrier(cmd, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                         VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
            mRtCacheStatsPending[slot] = true;
            mRtCacheHostRb[slot] = mRtCacheHost;
            mRtCacheHost = RtCacheHost{};
        }
    }
    mRtCacheNow = true;
    mRtCacheWas = true;
}

// The slot's stats copy (its fence was waited by acquire) -> one log line.
void LLCleanRendererVK::rtMaskCacheHarvest()
{
    const uint32_t slot = mFrameSlot & 1u;
    if (!mRtCacheStatsPending[slot] || !mRtCacheStatsRb[slot].mapped) return;
    mRtCacheStatsPending[slot] = false;
    uint32_t st[32];
    std::memcpy(st, mRtCacheStatsRb[slot].mapped, sizeof(st));
    const RtCacheHost& h = mRtCacheHostRb[slot];
    const uint32_t tiles = st[0] + st[1];
    if (!h.frames) return;
    const double pt = tiles ? 100.0 / (double)tiles : 0.0;
    const uint32_t blocks = st[12] + st[13];
    const uint32_t gf = std::max(st[14], 1u);
    std::ostringstream os;
    os.setf(std::ios::fixed);
    os.precision(1);
    os << "RT shadow cache (#771): " << h.frames << " frames (classified " << st[14] << ", plain fallback "
       << h.plain << ", GPU full fallback " << st[15] << ", static carry " << h.carry << "), "
       << tiles / gf << " tiles/frame (" << rtCacheCfg().tile << "x" << rtCacheCfg().tile
       << " mask texels, mask " << mRtMaskW << "x" << mRtMaskH << ")"
       << " | traced " << st[0] * pt << "% reused " << st[1] * pt << "% (sky-only " << st[2] * pt
       << "%, carried whole " << st[19] * pt << "%)"
       << " | traced by (% of tiles): forced " << st[3] * pt << " nohist " << st[4] * pt
       << " (off-view " << st[16] * pt << " sky " << st[17] * pt << " depth " << st[18] * pt << ")"
       << " young " << st[5] * pt << " mover " << st[6] * pt << " edge " << st[7] * pt << " fade " << st[8] * pt
       << " change " << st[9] * pt << " dyn " << st[10] * pt << " refresh " << st[11] * pt
       << " | blocks traced " << (blocks ? 100.0 * st[12] / blocks : 0.0) << "%"
       << " | host: forced frames " << h.forced << " (sun jump " << h.jump << "), sun drift frames " << h.drift
       << " | casters/frame: avatars moving " << (double)h.av_mov / h.frames << " still " << (double)h.av_still / h.frames
       << ", animesh moving " << (double)h.am_mov / h.frames << " still " << (double)h.am_still / h.frames
       << ", objects moving " << (double)h.mv_mov / h.frames << " settling " << (double)h.mv_still / h.frames
       << ", spinners traced " << (double)h.sp_mov / h.frames << " held " << (double)h.sp_still / h.frames
       << ", changed " << (double)h.chg / h.frames
       << " | swept volumes/frame " << (double)h.rects / h.frames << " (off screen " << (double)h.rects_off / h.frames
       << ", summed hull screen share " << 100.0 * h.area / h.frames << "%, bin entries/frame "
       << (double)h.binned / h.frames << ", unbinned frames " << h.unbinned << ", tests/tile "
       << (tiles ? (double)st[20] / tiles : 0.0) << ")";
    os.precision(3);
    os << " | GPU ms/frame c_rtmask_cls=";
    if (mRtCacheMs[0] >= 0.0) os << mRtCacheMs[0]; else os << "n/a";
    os << " c_rtmask=";
    if (mRtCacheMs[1] >= 0.0) os << mRtCacheMs[1]; else os << "n/a";
    os << " c_rtmask_tmp+sp=";
    if (mRtCacheMs[2] >= 0.0) os << mRtCacheMs[2]; else os << "n/a";
    os << " (RenderVKGpuTimers window)";
    LL_INFOS("VKRTCache") << os.str() << LL_ENDL;
}
