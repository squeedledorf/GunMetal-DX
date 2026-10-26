/**
 * @file llcleangivk.cpp
 * @brief RAY-TRACED GLOBAL ILLUMINATION (#87; ReSTIR GI + a world-space radiance cache;
 *        RenderVKGI, default off). The shader side and the data contract are in
 *        indra/vkclean/shaders/clean_gi_common.glsl; this file is the host side.
 *
 * FRAME (split deferred lighting with ray tracing up; one decision per recording):
 *   1. giDecide (the light pass, before the frame info): the settings, the GI denoiser's images
 *      at the render extent x RenderVKGIScale, the state buffer (header + radiance cache), the
 *      reservoirs (GI-2+) -- mGiNow, and the frame info's foliage_rt.x bit 128: the consumers read
 *      the GI images only on such a frame;
 *   2. giWriteHeader (the light pass, with the other uploads): flags, parameters, the change
 *      spheres (GI-4), the counters zeroed;
 *   3. recordGI (after the RT reflections and the projector sampler, before BASE / COMBINE, on the
 *      lighting's queue): the GI trace (pass 11 -DCLEAN_GI_TRACE: one ray per GI texel, its hit shaded
 *      as the RT reflections shade theirs, reservoir reuse; #1031 RenderVKGISplitKernels: as the
 *      TRACE / SHADE / REUSE / RESHADE / TAIL stages over mGiHit; #1036 RenderVKGIShadeSplit: SHADE as
 *      MISS / HIT-MAT / HIT-LIGHT over TRACE's hit / miss lists), the cache's resolve (GI-4), the GI
 *      denoiser (the #85 temporal + a-trous, a second LLCleanDenoiserVK: its own images, never the
 *      shadow channels'), the stats copy;
 *   4. the combine / BASE (clean_deferred_light.comp deferReflIn): the denoised GI REPLACES the
 *      probes' diffuse irradiance where it has an estimate (x RenderVKGIIntensity, floored by
 *      RenderVKGIMinAmbient x the probes' value); the probes stay the specular and the far field.
 * VRAM: the GI denoiser (112 B per GI texel: ~103 MB at 1440p half extent), the reservoirs
 * (64 B per GI texel), the split stages' records (100 B per GI texel + 48 B: ~88 MB at 1440p half extent;
 * the #1036 shade split's lists and material records live in the same space),
 * the state (1.2 KB + 48 B per cache slot: 12 MB at 2^18). All fixed by the
 * extent / the settings, allocated soft (#697: a refusal turns GI off and retries in 30 s), freed
 * when GI is switched off or held unused for kRtReflHoldSecs.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
 * $/LicenseInfo$
 */
#include "linden_common.h"

#include "llcleanrenderervk.h"
#include "llcleandenoisevk.h"
#include "llcleanrtvk.h"

#include "llviewercontrol.h"
#include "../llvkhitch.h"
#include "vk_common.h"
#include "vk_pipeline.h"
#include "vk_world_shaders_spv.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace
{
double giClock()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
uint32_t giDim(uint32_t r, float s) { return std::max(1u, (uint32_t)std::ceil((float)r * s)); }
VkShaderModule giModule(VkDevice dev, const uint32_t* code, size_t bytes, const char* name)
{
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(dev, &ci, nullptr, &m));
    vkNoteShaderModule(m, code, bytes, name);
    return m;
}
// The lighting kernels' debug specialization OFF (as llcleanprojstochvk.cpp pdnLitSpec).
const VkSpecializationInfo* giLitSpec()
{
    static const VkBool32 kOff = VK_FALSE;
    static const VkSpecializationMapEntry kEntry{1u, 0u, sizeof(VkBool32)};
    static const VkSpecializationInfo kSiOff{1u, &kEntry, sizeof(VkBool32), &kOff};
    return &kSiOff;
}
// clean_gi_common.glsl GI_F_*
constexpr uint32_t GI_F_ON = 1u, GI_F_FINAL_IN = 2u, GI_F_TEMPORAL = 4u, GI_F_SPATIAL = 8u, GI_F_CACHE = 16u,
                   GI_F_RES = 32u, GI_F_RES_PREV = 64u, GI_F_CACHE_FILL = 128u, GI_F_LIGHT_CHG = 65536u,
                   GI_F_BACK_STOP = 131072u, GI_F_POINT_SHADOW = 262144u, GI_F_REPROJ_SEARCH = 524288u,
                   GI_F_UPS_NORMAL = 1048576u, GI_F_STATS = 2097152u, GI_F_LAG = 8388608u, GI_F_LAG_STALE = 16777216u;
// One change sphere (xyz centre, w radius) into the next header's list: up to the header's 64; past
// them the last sphere grows to enclose it (a coarser invalidation instead of wiping the whole
// cache -- a sim with many small changes per frame kept it empty).
void giAddSphere(std::vector<float>& v, const float* s)
{
    if (v.size() / 4 < 64)
    {
        v.insert(v.end(), s, s + 4);
        return;
    }
    float* b = &v[63 * 4];
    const float dx = s[0] - b[0], dy = s[1] - b[1], dz = s[2] - b[2];
    const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (d + s[3] <= b[3]) return;
    if (d + b[3] <= s[3]) { std::memcpy(b, s, 4 * sizeof(float)); return; }
    const float r = 0.5f * (d + b[3] + s[3]);
    const float t = d > 1e-6f ? (r - b[3]) / d : 0.f;
    b[0] += dx * t; b[1] += dy * t; b[2] += dz * t; b[3] = r;
}
// The settings (all live).
struct GiCfg
{
    U32 mode = 0;          // RenderVKGI: 0 off, 1 one bounce, 2 + temporal reuse, 3 + spatial reuse
    F32 scale = 0.5f;      // RenderVKGIScale
    F32 intensity = 1.f;   // RenderVKGIIntensity
    F32 minAmb = 0.f;      // RenderVKGIMinAmbient
    F32 rayLen = 64.f;     // RenderVKGIRayLength
    U32 hitAmb = 2;        // RenderVKGIHitAmbient
    F32 firefly = 4.f;     // RenderVKGIFireflyClamp
    U32 atrous = 3;        // RenderVKGIAtrous
    F32 history = 32.f;    // RenderVKGIHistory
    F32 clamp = 2.f;       // RenderVKGIClamp
    bool hitGrid = true;   // RenderVKGIHitLightGrid
    U32 hitShadow = 1;     // RenderVKGIHitShadowRays
    U32 debug = 0;         // RenderVKGIDebug
    U32 mcap = 16;         // RenderVKGIMCap
    U32 taps = 3;          // RenderVKGISpatialTaps
    U32 radius = 16;       // RenderVKGISpatialRadius
    bool cache = true;     // RenderVKGICache
    U32 slotsLog2 = 18;    // RenderVKGICacheSlotsLog2
    F32 cell = 0.25f;      // RenderVKGICacheCellSize
    F32 levelDist = 8.f;   // RenderVKGICacheLevelDistance
    F32 window = 32.f;     // RenderVKGICacheFrames
    F32 evict = 600.f;     // RenderVKGICacheEvictFrames
    U32 specOcc = 1;       // RenderVKGISpecOcclusion
    U32 everyLog = 0;      // RenderVKGITraceEvery 1 / 2 / 4 -> 0 / 1 / 2
    bool missProbes = true;   // RenderVKGIMissProbes
    bool cullBack = true;  // RenderVKGICullBackFaces
    bool celOcc = true;    // RenderVKGICelOcclusion
    bool hazeOcc = true;   // RenderVKGIHazeOcclusion
    bool specOccRtRes = true;   // RenderVKGISpecOcclusionRTResidual
    bool lightChg = true;  // RenderVKGILightChangeCut
    F32 chgHist = 8.f;     // RenderVKGILightChangeHistory
    F32 cosMin = 0.05f;    // RenderVKGIReuseCosMin
    F32 reshadeGain = 2.f; // RenderVKGIReshadeGain
    F32 reshadeThr = 0.1f; // RenderVKGIReshadeThreshold
    bool backStop = true;  // RenderVKGIBackFaceStop
    bool pointShadow = true;   // RenderVKGIHitPointShadows
    U32 disoccFix = 4;     // RenderVKGIDisoccFix
    bool reprojSearch = true;  // RenderVKGIReprojSearch
    U32 disoccTaps = 4;    // RenderVKGIDisoccTaps
    bool upsNormal = true; // RenderVKGIUpsampleNormal
    bool split = true;     // RenderVKGISplitKernels (#1031)
    bool shadeSplit = true;    // RenderVKGIShadeSplit (#1036)
};
GiCfg giCfg()
{
    GiCfg c;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<U32>  s_mode(gSavedSettings, "RenderVKGI", 0U);
    static LLCachedControl<F32>  s_scale(gSavedSettings, "RenderVKGIScale", 0.5f);
    static LLCachedControl<F32>  s_int(gSavedSettings, "RenderVKGIIntensity", 1.f);
    static LLCachedControl<F32>  s_min(gSavedSettings, "RenderVKGIMinAmbient", 0.f);
    static LLCachedControl<F32>  s_len(gSavedSettings, "RenderVKGIRayLength", 64.f);
    static LLCachedControl<U32>  s_amb(gSavedSettings, "RenderVKGIHitAmbient", 2U);
    static LLCachedControl<F32>  s_ff(gSavedSettings, "RenderVKGIFireflyClamp", 4.f);
    static LLCachedControl<U32>  s_at(gSavedSettings, "RenderVKGIAtrous", 3U);
    static LLCachedControl<F32>  s_hist(gSavedSettings, "RenderVKGIHistory", 32.f);
    static LLCachedControl<F32>  s_clamp(gSavedSettings, "RenderVKGIClamp", 2.f);
    static LLCachedControl<bool> s_grid(gSavedSettings, "RenderVKGIHitLightGrid", true);
    static LLCachedControl<U32>  s_hsr(gSavedSettings, "RenderVKGIHitShadowRays", 1U);
    static LLCachedControl<U32>  s_dbg(gSavedSettings, "RenderVKGIDebug", 0U);
    static LLCachedControl<U32>  s_mcap(gSavedSettings, "RenderVKGIMCap", 16U);
    static LLCachedControl<U32>  s_taps(gSavedSettings, "RenderVKGISpatialTaps", 3U);
    static LLCachedControl<U32>  s_rad(gSavedSettings, "RenderVKGISpatialRadius", 16U);
    static LLCachedControl<bool> s_cache(gSavedSettings, "RenderVKGICache", true);
    static LLCachedControl<U32>  s_slots(gSavedSettings, "RenderVKGICacheSlotsLog2", 18U);
    static LLCachedControl<F32>  s_cell(gSavedSettings, "RenderVKGICacheCellSize", 0.25f);
    static LLCachedControl<F32>  s_ld(gSavedSettings, "RenderVKGICacheLevelDistance", 8.f);
    static LLCachedControl<F32>  s_win(gSavedSettings, "RenderVKGICacheFrames", 32.f);
    static LLCachedControl<F32>  s_ev(gSavedSettings, "RenderVKGICacheEvictFrames", 600.f);
    static LLCachedControl<U32>  s_so(gSavedSettings, "RenderVKGISpecOcclusion", 1U);
    static LLCachedControl<U32>  s_every(gSavedSettings, "RenderVKGITraceEvery", 1U);
    static LLCachedControl<bool> s_missp(gSavedSettings, "RenderVKGIMissProbes", true);
    static LLCachedControl<bool> s_cull(gSavedSettings, "RenderVKGICullBackFaces", true);
    static LLCachedControl<bool> s_celocc(gSavedSettings, "RenderVKGICelOcclusion", true);
    static LLCachedControl<bool> s_hazeocc(gSavedSettings, "RenderVKGIHazeOcclusion", true);
    static LLCachedControl<bool> s_sortres(gSavedSettings, "RenderVKGISpecOcclusionRTResidual", true);
    static LLCachedControl<U32>  s_lchg(gSavedSettings, "RenderVKGILightChangeCut", 1U);
    static LLCachedControl<F32>  s_lchgh(gSavedSettings, "RenderVKGILightChangeHistory", 8.f);
    static LLCachedControl<F32>  s_cosmin(gSavedSettings, "RenderVKGIReuseCosMin", 0.05f);
    static LLCachedControl<F32>  s_reshadeGain(gSavedSettings, "RenderVKGIReshadeGain", 2.f);
    static LLCachedControl<F32>  s_reshadeThr(gSavedSettings, "RenderVKGIReshadeThreshold", 0.1f);
    static LLCachedControl<U32>  s_bstop(gSavedSettings, "RenderVKGIBackFaceStop", 1U);
    static LLCachedControl<U32>  s_pshad(gSavedSettings, "RenderVKGIHitPointShadows", 1U);
    static LLCachedControl<U32>  s_dfix(gSavedSettings, "RenderVKGIDisoccFix", 4U);
    static LLCachedControl<U32>  s_rsearch(gSavedSettings, "RenderVKGIReprojSearch", 1U);
    static LLCachedControl<U32>  s_dtaps(gSavedSettings, "RenderVKGIDisoccTaps", 4U);
    static LLCachedControl<U32>  s_upsn(gSavedSettings, "RenderVKGIUpsampleNormal", 1U);
    static LLCachedControl<bool> s_split(gSavedSettings, "RenderVKGISplitKernels", true);
    static LLCachedControl<bool> s_shsplit(gSavedSettings, "RenderVKGIShadeSplit", true);
    c.mode = std::min<U32>(s_mode, 3u);
    c.scale = s_scale >= 0.75f ? 1.f : (s_scale >= 0.375f ? 0.5f : 0.25f);
    c.intensity = std::clamp((F32)s_int, 0.f, 16.f);
    c.minAmb = std::clamp((F32)s_min, 0.f, 1.f);
    c.rayLen = std::clamp((F32)s_len, 1.f, 1024.f);
    c.hitAmb = std::min<U32>(s_amb, 3u);
    c.firefly = std::max((F32)s_ff, 0.f);
    c.atrous = s_at;
    c.history = std::clamp((F32)s_hist, 1.f, 63.f);
    c.clamp = std::clamp((F32)s_clamp, 0.5f, 8.f);
    c.hitGrid = s_grid;
    c.hitShadow = std::min<U32>(s_hsr, 4u);
    c.debug = std::min<U32>(s_dbg, 9u);
    c.mcap = std::clamp<U32>(s_mcap, 1u, 64u);
    c.taps = std::min<U32>(s_taps, 6u);
    c.radius = std::clamp<U32>(s_rad, 1u, 64u);
    c.cache = s_cache;
    c.slotsLog2 = std::clamp<U32>(s_slots, 12u, 22u);
    c.cell = std::clamp((F32)s_cell, 0.05f, 8.f);
    c.levelDist = std::clamp((F32)s_ld, 1.f, 256.f);
    c.window = std::clamp((F32)s_win, 1.f, 1024.f);
    c.evict = std::clamp((F32)s_ev, 16.f, 100000.f);
    c.specOcc = std::min<U32>(s_so, 2u);
    c.everyLog = s_every >= 4u ? 2u : (s_every >= 2u ? 1u : 0u);
    c.missProbes = s_missp;
    c.cullBack = s_cull;
    c.celOcc = s_celocc;
    c.hazeOcc = s_hazeocc;
    c.specOccRtRes = s_sortres;
    c.lightChg = s_lchg != 0u;
    c.chgHist = std::clamp((F32)s_lchgh, 0.f, 15.f);
    c.cosMin = std::clamp((F32)s_cosmin, 0.f, 0.5f);
    c.reshadeGain = std::clamp((F32)s_reshadeGain, 0.f, 16.f);
    c.reshadeThr = std::clamp((F32)s_reshadeThr, 0.f, 4.f);
    c.backStop = s_bstop != 0u;
    c.pointShadow = s_pshad != 0u;
    c.disoccFix = std::min<U32>(s_dfix, 7u);
    c.reprojSearch = s_rsearch != 0u;
    c.disoccTaps = std::min<U32>(s_dtaps, 8u);
    c.upsNormal = s_upsn != 0u;
    c.split = s_split;
    c.shadeSplit = s_shsplit;
#endif
    return c;
}
} // namespace

// pdnInit (initDeferSplit): the GI denoiser always -- set 3 bindings 18-21 name its images or its
// placeholders in every split frame -- and the GI kernels when the RT layout exists.
void LLCleanRendererVK::giInit(VkDevice dev)
{
    if (!mGiDn)
    {
        mGiDn = new LLCleanDenoiserVK();
        if (!mGiDn->init(mCtx, mDslDraw0, mDepthSampler, "GI denoiser"))
        {
            LL_WARNS("Vulkan") << "GI denoiser: init failed; ray-traced GI unavailable (RenderVKGI acts as 0)" << LL_ENDL;
            mGiDn->destroy();
            delete mGiDn;
            mGiDn = nullptr;
        }
        else writeGiMotion();
    }
    if (mRtReflLayout && !mGiTracePipe)
    {
        VkShaderModule m = giModule(dev, kWorldDeferGiTraceSpv, kWorldDeferGiTraceSpv_bytes, "DeferGiTrace");
        createComputePipelineInto(dev, m, mRtReflLayout, 0u, giLitSpec(), &mGiTracePipe, "mGiTracePipe (DeferGiTrace)");
        destroyShaderModuleDeferred(dev, m, nullptr);
    }
    if (mRtReflLayout && !mGiResolvePipe)
    {
        // (GI-4) the radiance cache's resolve
        VkShaderModule m = giModule(dev, kWorldDeferGiResolveSpv, kWorldDeferGiResolveSpv_bytes, "DeferGiResolve");
        createComputePipelineInto(dev, m, mRtReflLayout, 0u, giLitSpec(), &mGiResolvePipe, "mGiResolvePipe (DeferGiResolve)");
        destroyShaderModuleDeferred(dev, m, nullptr);
    }
    if (mRtReflLayout)
    {
        // (#1031) the split stages (distinct module names: RenderVKPipelineStats logs each one's registers)
        struct Stage { const uint32_t* code; size_t bytes; const char* mod; const char* name; };
        const Stage st[GIS_COUNT] = {
            {kWorldDeferGiSplitTraceSpv, kWorldDeferGiSplitTraceSpv_bytes, "DeferGiSplitTrace", "mGiSplitPipe (DeferGiSplitTrace)"},
            {kWorldDeferGiSplitShadeSpv, kWorldDeferGiSplitShadeSpv_bytes, "DeferGiSplitShade", "mGiSplitPipe (DeferGiSplitShade)"},
            {kWorldDeferGiSplitReuseSpv, kWorldDeferGiSplitReuseSpv_bytes, "DeferGiSplitReuse", "mGiSplitPipe (DeferGiSplitReuse)"},
            {kWorldDeferGiSplitReshadeSpv, kWorldDeferGiSplitReshadeSpv_bytes, "DeferGiSplitReshade",
             "mGiSplitPipe (DeferGiSplitReshade)"}};
        for (uint32_t i = 0; i < GIS_COUNT; ++i)
        {
            if (mGiSplitPipe[i]) continue;
            VkShaderModule m = giModule(dev, st[i].code, st[i].bytes, st[i].mod);
            createComputePipelineInto(dev, m, mRtReflLayout, 0u, giLitSpec(), &mGiSplitPipe[i], st[i].name);
            destroyShaderModuleDeferred(dev, m, nullptr);
        }
        // (#1036) the shade split's stages
        const Stage sh[GSS_COUNT] = {
            {kWorldDeferGiSplitMissSpv, kWorldDeferGiSplitMissSpv_bytes, "DeferGiSplitMiss", "mGiShadePipe (DeferGiSplitMiss)"},
            {kWorldDeferGiSplitHitMatSpv, kWorldDeferGiSplitHitMatSpv_bytes, "DeferGiSplitHitMat",
             "mGiShadePipe (DeferGiSplitHitMat)"},
            {kWorldDeferGiSplitHitLightSpv, kWorldDeferGiSplitHitLightSpv_bytes, "DeferGiSplitHitLight",
             "mGiShadePipe (DeferGiSplitHitLight)"}};
        for (uint32_t i = 0; i < GSS_COUNT; ++i)
        {
            if (mGiShadePipe[i]) continue;
            VkShaderModule m = giModule(dev, sh[i].code, sh[i].bytes, sh[i].mod);
            createComputePipelineInto(dev, m, mRtReflLayout, 0u, giLitSpec(), &mGiShadePipe[i], sh[i].name);
            destroyShaderModuleDeferred(dev, m, nullptr);
        }
    }
    writeGiBindings();
}

// (#1031) the split stages can run this recording: every pipeline (async builds landed) + the records
bool LLCleanRendererVK::giSplitReady(uint32_t w, uint32_t h) const
{
    for (uint32_t i = 0; i < GIS_COUNT; ++i)
        if (!mGiSplitPipe[i]) return false;
    return mGiHit.buffer && mGiHitW == w && mGiHitH == h;
}

// (#1036) the shade split's three pipelines exist (async builds landed); else SHADE
bool LLCleanRendererVK::giShadeSplitReady() const
{
    for (uint32_t i = 0; i < GSS_COUNT; ++i)
        if (!mGiShadePipe[i]) return false;
    return true;
}

void LLCleanRendererVK::giDestroy()
{
    if (!mCtx) return;
    VkDevice dev = mCtx->device();
    if (mGiTracePipe) { vkDestroyPipeline(dev, mGiTracePipe, nullptr); mGiTracePipe = VK_NULL_HANDLE; }
    if (mGiResolvePipe) { vkDestroyPipeline(dev, mGiResolvePipe, nullptr); mGiResolvePipe = VK_NULL_HANDLE; }
    for (VkPipeline& sp : mGiSplitPipe)
        if (sp) { vkDestroyPipeline(dev, sp, nullptr); sp = VK_NULL_HANDLE; }
    for (VkPipeline& sp : mGiShadePipe)
        if (sp) { vkDestroyPipeline(dev, sp, nullptr); sp = VK_NULL_HANDLE; }
    if (mGiHit.buffer) { mCtx->destroyBuffer(mGiHit); mGiHit = GpuBuffer{}; }
    mGiHitW = mGiHitH = 0;
    mGiSplitNow = false;
    mGiShadeSplitNow = false;
    if (mGiState.buffer) { mCtx->destroyBuffer(mGiState); mGiState = GpuBuffer{}; }
    if (mGiRes.buffer) { mCtx->destroyBuffer(mGiRes); mGiRes = GpuBuffer{}; }
    if (mGiStatRb.buffer) { mCtx->destroyBuffer(mGiStatRb); mGiStatRb = GpuBuffer{}; }
    giLagDestroyRes();   // (#978 phase 2)
    mGiLagPool.destroy();
    for (VkDescriptorSet& s : mGiLagSet) s = VK_NULL_HANDLE;
    for (bool& u : mGiStatUsed) u = false;
    mGiStateSlots = ~0u;
    mGiResW = mGiResH = 0;
    mGiResPrevOk = false;
    if (mGiDn)
    {
        mGiDn->destroy();
        delete mGiDn;
        mGiDn = nullptr;
    }
    mGiNow = mGiWanted = false;
    mGiReq = GiReq{};
}

void LLCleanRendererVK::writeGiMotion()
{
    if (!mGiDn) return;
    if (mMotion.view) mGiDn->setMotion(mMotion.view, mDepthSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    else              mGiDn->setMotion(VK_NULL_HANDLE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL);
}

// Set 3 bindings 18-23 + 25 (callers guarantee no frame in flight uses the set).
void LLCleanRendererVK::writeGiBindings()
{
    if (!mDsetDeferSplit || !mCtx) return;
    VkDescriptorImageInfo ii[4]{};
    VkDescriptorBufferInfo bi[3]{};
    VkWriteDescriptorSet w[7]{};
    uint32_t n = 0;
    // (the GI denoiser's images; without it -- its init failed -- the projector denoiser's views, which
    //  have the same formats: the combine / BASE declare 18-21 statically and read them only on a GI
    //  frame, which never comes then)
    const LLCleanDenoiserVK* dn = mGiDn ? mGiDn : mPdn;
    if (dn)
    {
        const VkImageView v[4] = {dn->inView(), dn->geomView(), dn->motView(), dn->outView()};
        for (uint32_t i = 0; i < 4; ++i)
        {
            ii[i] = {VK_NULL_HANDLE, v[i], VK_IMAGE_LAYOUT_GENERAL};
            w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[n].dstSet = mDsetDeferSplit;
            w[n].dstBinding = kDsBindingGiIn + i; w[n].descriptorCount = 1;
            w[n].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[n].pImageInfo = &ii[i];
            ++n;
        }
    }
    const VkBuffer res = mGiRes.buffer ? mGiRes.buffer : mRsResDummy.buffer;
    const VkBuffer st = mGiState.buffer ? mGiState.buffer : mRsResDummy.buffer;
    if (res && st)
    {
        bi[0] = {res, 0, VK_WHOLE_SIZE};
        bi[1] = {st, 0, VK_WHOLE_SIZE};
        for (uint32_t i = 0; i < 2; ++i)
        {
            w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[n].dstSet = mDsetDeferSplit;
            w[n].dstBinding = (i == 0 ? kDsBindingGiRes : kDsBindingGiState); w[n].descriptorCount = 1;
            w[n].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[n].pBufferInfo = &bi[i];
            ++n;
        }
    }
    // (#1031) 25: the split stages' records (only the split pipelines declare it)
    if (const VkBuffer hb = mGiHit.buffer ? mGiHit.buffer : mRsResDummy.buffer)
    {
        bi[2] = {hb, 0, VK_WHOLE_SIZE};
        w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[n].dstSet = mDsetDeferSplit;
        w[n].dstBinding = kDsBindingGiHit; w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[n].pBufferInfo = &bi[2];
        ++n;
    }
    if (n) vkUpdateDescriptorSets(mCtx->device(), n, w, 0, nullptr);
    // draw set 0 binding 53: the same state buffer for the forward lit FS (CLEAN_GI_FORWARD) -- or (#978
    // phase 2, one-frame-latency GI) its copy, mGiStateFwd, while one exists for this cache size: the trace
    // then writes mGiState beside pass 2 (giFwdCopy refreshes the copy every GI frame before pass 2).
    mGiFwdBound = false;
    if (mDsetDraw0 && st)
    {
        const bool fwd = mGiStateFwd.buffer && mGiState.buffer && mGiStateFwdSlots == mGiStateSlots;
        VkDescriptorBufferInfo fb{fwd ? mGiStateFwd.buffer : st, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet d{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        d.dstSet = mDsetDraw0; d.dstBinding = 53; d.descriptorCount = 1;
        d.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; d.pBufferInfo = &fb;
        vkUpdateDescriptorSets(mCtx->device(), 1, &d, 0, nullptr);
        mGiFwdBound = fwd;
    }
}

// The light pass, before the frame info and the light-grid decision: does this recording run GI,
// with which resources. Allocation changes wait for the device (set 3 may be bound by frames in
// flight) and rewrite set 3's GI bindings.
void LLCleanRendererVK::giDecide(uint32_t rw, uint32_t rh)
{
    mGiNow = false;
    mGiSplitNow = false;    // (#1031: decided below, on a GI frame)
    mGiShadeSplitNow = false;   // (#1036: ditto)
    mGiLagNow = false;      // (#978 phase 2: decided below, on a GI frame)
    mGiFwdCopied = false;
    mGiLagValue = 0;        // (joined by the last recording, at the latest by its tail: asyncSubmitFrame)
    GiCfg cfg = giCfg();
    cfg.scale = rtSignalScale(cfg.scale);   // (RenderVKDlssFullResSignalsMode)
    U32 split = 1;
    bool defer = true;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<U32>  s_split(gSavedSettings, "RenderVKDeferredSplit", 1U);
    static LLCachedControl<bool> s_defer(gSavedSettings, "RenderVKDeferredLighting", true);
    split = s_split; defer = s_defer;
#endif
    mGiModeSetting = cfg.mode;
    // (Decided in the light pass, BEFORE this frame's TLAS build: tlasValid() is false there by
    // construction -- it wants this frame's slot -- so the decision takes last frame's build;
    // recordGI re-checks tlasValid() and skips the trace without one -- the consumers then read the
    // GI images' last content, or nothing (alpha 0 -> the probes).)
    uint32_t why = 0u;
    if (split == 0u || !defer) why |= 1u;
    if (mOffscreen) why |= 2u;
    if (!mDeferSplitOk || !mDsetDeferSplit) why |= 4u;
    if (!mGiDn || !mGiDn->pipelinesReady() || !mGiTracePipe || !mRtReflLayout) why |= 8u;
    if (!mRT) why |= 16u;
    else if (!(mRT->tlasValid() || mRT->tlasBuiltLast())) why |= 32u;
    else if (mRT->consumerSet() == VK_NULL_HANDLE) why |= 64u;
    if (!mRsResDummy.buffer) why |= 128u;
    if (mTexPressure) why |= 256u;
    const bool path = cfg.mode != 0u && why == 0u;
    mGiWanted = path;
    const double now = giClock();
    // the settings line (on change -- of the settings or of the failing conditions)
    {
        const uint32_t key = cfg.mode | (path ? 16u : 0u) | (cfg.cache ? 32u : 0u) | (cfg.hitAmb << 8) | (why << 12);
        if (cfg.mode != 0u && (!mGiLogged || key != mGiLogKey))
        {
            mGiLogged = true;
            mGiLogKey = key;
            static const char* const kMode[4] = {"off", "one bounce (GI-1)", "+ temporal reuse (GI-2)",
                                                 "+ temporal and spatial reuse (GI-3)"};
            static const char* const kAmb[4] = {"nothing (one bounce)", "the probes", "the radiance cache, else the probes",
                                                "the radiance cache, else nothing"};
            LL_INFOS("VKShadow") << "ray-traced GI (RenderVKGI " << cfg.mode << "): " << kMode[cfg.mode]
                                 << "; hit ambient = " << kAmb[cfg.hitAmb] << "; radiance cache "
                                 << (cfg.cache ? "on" : "off") << "; ";
            if (path) LL_CONT << "ACTIVE";
            else
            {
                LL_CONT << "waiting on:";
                if (why & 1u)   LL_CONT << " the deferred lighting is not split (RenderVKDeferredSplit / RenderVKDeferredLighting);";
                if (why & 2u)   LL_CONT << " an offscreen (snapshot) recording;";
                if (why & 4u)   LL_CONT << " the split lighting's kernels / set not ready;";
                if (why & 8u)   LL_CONT << " the GI kernels / GI denoiser not ready;";
                if (why & 16u)  LL_CONT << " no ray tracing;";
                if (why & 32u)  LL_CONT << " no TLAS built last frame;";
                if (why & 64u)  LL_CONT << " no RT consumer set;";
                if (why & 128u) LL_CONT << " set 3's placeholder buffer missing;";
                if (why & 256u) LL_CONT << " texture memory pressure;";
            }
            LL_CONT << LL_ENDL;
        }
        else if (cfg.mode == 0u && mGiLogged)
        {
            mGiLogged = false;
            LL_INFOS("VKShadow") << "ray-traced GI off (RenderVKGI 0): the probes' diffuse irradiance" << LL_ENDL;
        }
    }
    if (!path)
    {
        // off (or under texture pressure): free everything at once; a transient stand-down (no TLAS
        // this frame) keeps the resources for the hold time
        const bool hold = cfg.mode != 0u && !mTexPressure && mGiDn && mGiDn->allocated()
                       && now - mGiLastActive < kRtReflHoldSecs;
        if (!hold && mCtx && (mGiState.buffer || mGiRes.buffer || (mGiDn && mGiDn->allocated())))
        {
            if (resizeAtFrameStart())
            {
                // (freed before the next recording: giPrestep)
                mGiReq = GiReq{};
                mGiReq.pending = true;
                mGiReq.free = true;
            }
            else
                giFreeAll();
        }
        mGiResPrevOk = false;
        mGiLagLast.have = false;   // (#978 phase 2: no trace this recording -- offscreen, no TLAS, ...)
        return;
    }
    if (now < mGiRetryAt) return;
    const uint32_t w = giDim(rw, cfg.scale), h = giDim(rh, cfg.scale);
    // the state buffer's radiance cache (GI-4: its slots; 0 without the cache); (GI-2+) the reservoirs
    const uint32_t slots = cfg.cache ? (1u << cfg.slotsLog2) : 0u;
    const bool wantRes = cfg.mode >= 2u;
    const bool needImg = !mGiDn->allocated() || mGiDn->width() != w || mGiDn->height() != h || !mGiState.buffer
                      || mGiStateSlots != slots;
    const bool needRes = wantRes ? (!mGiRes.buffer || mGiResW != w || mGiResH != h) : (mGiRes.buffer != VK_NULL_HANDLE);
    // (#1031) the split stages' records: wanted while RenderVKGISplitKernels is on and every stage's
    // pipeline exists (a refusal: the fused kernel, retried after mGiHitRetryAt)
    bool splitPipes = true;
    for (uint32_t i = 0; i < GIS_COUNT; ++i) splitPipes = splitPipes && mGiSplitPipe[i] != VK_NULL_HANDLE;
    const bool wantHit = cfg.split && splitPipes && (mGiHit.buffer || now >= mGiHitRetryAt);
    const bool needHit = wantHit ? (!mGiHit.buffer || mGiHitW != w || mGiHitH != h) : (mGiHit.buffer != VK_NULL_HANDLE);
    if (needImg || needRes || needHit)
    {
        // (2026-10-22, the Beta 5 DEVICE_LOST rule) never re-created under this recording: draw set 0
        // is bound already and set 3 / the GI denoiser's set would be rewritten behind a device wait
        // that does not cover this frame's recorded commands. giPrestep applies it; this frame runs
        // without GI while the images / state do not fit (reservoirs alone: without reuse).
        if (resizeAtFrameStart())
        {
            mGiReq = GiReq{};
            mGiReq.pending = true;
            mGiReq.w = w; mGiReq.h = h; mGiReq.slots = slots; mGiReq.wantRes = wantRes; mGiReq.scale = cfg.scale;
            mGiReq.wantHit = wantHit;
            if (needImg) { mGiResPrevOk = false; return; }
        }
        else if (!giApply(w, h, slots, wantRes, cfg.scale, wantHit))   // (RenderVKResizeAtFrameStart 0: in place)
            return;
    }
    // (#978 phase 2) ONE-FRAME-LATENCY GI (RenderVKGILatencyFrame; llcleangivk.cpp GI LATENCY FRAME): its
    // resources at this render extent (requested like the images above: applied before the next recording);
    // this frame runs the mode only when they are all in place (the forward copy bound to b53 above all --
    // the trace must never write the buffer pass 2 reads). Off: freed (b53 back on mGiState).
    {
#if defined(LL_HAS_CLEAN_PIPELINE)
        static LLCachedControl<bool> s_lag(gSavedSettings, "RenderVKGILatencyFrame", false);
        mGiLagWant = s_lag;
#endif
        const bool ready = mGiLagWant && giLagReady(rw, rh);
        if (mGiLagWant && !ready && now >= mGiLagRetryAt)
        {
            if (resizeAtFrameStart())
            {
                mGiReq.pending = true;
                mGiReq.lag = true; mGiReq.lagFree = false;
                mGiReq.lagW = rw; mGiReq.lagH = rh;
            }
            else giLagEnsure(rw, rh);
        }
        else if (!mGiLagWant && (mGiStateFwd.buffer || mGiLagDepth.image))
        {
            if (resizeAtFrameStart()) { mGiReq.pending = true; mGiReq.lagFree = true; mGiReq.lag = false; }
            else giLagFree();
        }
        mGiLagNow = mGiLagWant && giLagReady(rw, rh);
    }
    mGiNow = true;
    mGiSplitNow = wantHit && giSplitReady(w, h);   // (#1031: else the fused kernel this recording)
    // (#1036) SHADE as MISS / HIT-MAT / HIT-LIGHT over TRACE's lists (same records: no allocation of
    // its own); while its pipelines compile, SHADE
    mGiShadeSplitNow = mGiSplitNow && cfg.shadeSplit && giShadeSplitReady();
    mGiLastActive = now;
    mGiDn->beginFrame();
    ++mGiFrame;
    mGiDebug = cfg.debug;
    mGiAtrous = LLCleanDenoiserVK::clampIters(cfg.atrous);
    // (#870) RAW under DLSS RR (recordGI's dp.raw, the same test): every a-trous iteration only COPIES
    // the temporal pass's output (dnRawView skips the filter) and the last one writes the consumer's
    // depth / fill sign -- one iteration does exactly that (the final image then IN: GI_F_FINAL_IN below).
    if (rrRawSignal(kRrRawGi, mGiDn->width(), mGiDn->height())) mGiAtrous = 1u;
    const bool res = mGiRes.buffer && mGiResW == w && mGiResH == h;
    mGiFlags = GI_F_ON | (LLCleanDenoiserVK::finalIsIn(mGiAtrous) ? GI_F_FINAL_IN : 0u)
             | (res ? GI_F_RES : 0u) | (res && mGiResPrevOk ? GI_F_RES_PREV : 0u)
             | (res && cfg.mode >= 2u ? GI_F_TEMPORAL : 0u) | (res && cfg.mode >= 3u && cfg.taps > 0u ? GI_F_SPATIAL : 0u)
             | (slots != 0u ? GI_F_CACHE | GI_F_CACHE_FILL : 0u);
    mGiResPrevOk = res;   // (this frame's trace writes them: next frame's previous)
    // (#978 phase 2) the combine reads LAST frame's trace: usable only when it ran the frame before this
    // one (consecutive GI frames: no recording without a trace in between) into these very images, at this
    // render extent (the reprojection maps render pixels); its final image (IN / OUT) and GEOM parity go
    // into this header for the consumers (the trace itself takes its own from the push / the denoiser).
    if (mGiLagNow)
    {
        mGiLagStale = !(mGiLagLast.have && mGiLagLast.frame + 1u == mGiFrame && mGiLagLast.w == mGiDn->width()
                        && mGiLagLast.h == mGiDn->height() && mGiLagLast.rw == rw && mGiLagLast.rh == rh);
        mGiLagHdrFinalIn = mGiLagLast.finalIn;
        mGiLagHdrPar = mGiLagLast.par;
    }
}

// The GI resources freed (the device waits): the denoiser's images, the state buffer, the
// reservoirs; set 3 bindings 18-23 back on the fallbacks. giPrestep (or in place with
// RenderVKResizeAtFrameStart 0).
void LLCleanRendererVK::giFreeAll()
{
    if (!mCtx) return;
    vkDeviceWaitIdle(mCtx->device());
    if (mGiDn && mGiDn->allocated()) mGiDn->release();
    if (mGiState.buffer) { mCtx->destroyBuffer(mGiState); mGiState = GpuBuffer{}; }
    if (mGiRes.buffer) { mCtx->destroyBuffer(mGiRes); mGiRes = GpuBuffer{}; }
    if (mGiHit.buffer) { mCtx->destroyBuffer(mGiHit); mGiHit = GpuBuffer{}; }   // (#1031)
    mGiHitW = mGiHitH = 0;
    mGiSplitNow = false;
    mGiShadeSplitNow = false;
    giLagDestroyRes();   // (#978 phase 2: the forward copy, the depth copy)
    mGiStateSlots = ~0u;
    mGiResW = mGiResH = 0;
    mGiResPrevOk = false;
    writeGiBindings();
}

// The GI resources at this extent / cache size (each change waits for the device; set 3's GI
// bindings rewritten). False: the denoiser's images or the state buffer were refused (#697: GI
// off, retry in 30 s); refused reservoirs only drop the reuse.
bool LLCleanRendererVK::giApply(uint32_t w, uint32_t h, uint32_t slots, bool wantRes, float scale, bool wantHit)
{
    if (!mCtx || !mGiDn) return false;
    const double now = giClock();
    bool changed = false;
    // the GI denoiser's images
    if (!mGiDn->allocated() || mGiDn->width() != w || mGiDn->height() != h)
    {
        if (!mGiDn->ensure(w, h, false))
        {
            mGiRetryAt = now + 30.0;
            LL_WARNS("Vulkan") << "ray-traced GI: the GI denoiser's " << w << "x" << h << " images were refused (VRAM);"
                                  " GI off, retry in 30 s" << LL_ENDL;
            writeGiBindings();
            return false;
        }
        changed = true;
    }
    // the state buffer: the header + the radiance cache (GI-4: its slots; 0 without the cache)
    if (!mGiState.buffer || mGiStateSlots != slots)
    {
        vkDeviceWaitIdle(mCtx->device());
        if (mGiState.buffer) { mCtx->destroyBuffer(mGiState); mGiState = GpuBuffer{}; }
        const VkDeviceSize bytes = sizeof(GiHeader) + (VkDeviceSize)slots * 48u + 16u;
        {
            VkMemScope memcat(VKMEM_DENOISE);
            VkDeviceContext::SoftAllocScope soft(mCtx);
            mGiState = mCtx->createBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
                                                 | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        }
        mGiStateSlots = mGiState.buffer ? slots : ~0u;
        changed = true;
        if (!mGiState.buffer)
        {
            mGiRetryAt = now + 30.0;
            LL_WARNS("Vulkan") << "ray-traced GI: the state buffer (" << ((bytes + (1u << 19)) >> 20)
                               << " MB) was refused (VRAM); GI off, retry in 30 s" << LL_ENDL;
            writeGiBindings();
            return false;
        }
        mCtx->setObjectName(VK_OBJECT_TYPE_BUFFER, (uint64_t)mGiState.buffer, "giState");
        mGiSpheres.clear();
        mGiStateFresh = true;   // (the cache starts empty: giWriteHeader clears it)
    }
    // (GI-2+) the reservoirs at the GI extent
    if (!wantRes && mGiRes.buffer)
    {
        vkDeviceWaitIdle(mCtx->device());
        mCtx->destroyBuffer(mGiRes);
        mGiRes = GpuBuffer{};
        mGiResW = mGiResH = 0;
        changed = true;
    }
    if (wantRes && (!mGiRes.buffer || mGiResW != w || mGiResH != h))
    {
        vkDeviceWaitIdle(mCtx->device());
        if (mGiRes.buffer) { mCtx->destroyBuffer(mGiRes); mGiRes = GpuBuffer{}; }
        const VkDeviceSize bytes = (VkDeviceSize)w * h * 2u * 32u;
        {
            VkMemScope memcat(VKMEM_DENOISE);
            VkDeviceContext::SoftAllocScope soft(mCtx);
            mGiRes = mCtx->createBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        }
        mGiResW = mGiRes.buffer ? w : 0u;
        mGiResH = mGiRes.buffer ? h : 0u;
        mGiResPrevOk = false;
        changed = true;
        if (!mGiRes.buffer)
            LL_WARNS("Vulkan") << "ray-traced GI: the reservoirs (" << ((bytes + (1u << 19)) >> 20)
                               << " MB) were refused (VRAM); no reuse (RenderVKGI acts as 1)" << LL_ENDL;
        else mCtx->setObjectName(VK_OBJECT_TYPE_BUFFER, (uint64_t)mGiRes.buffer, "giReservoirs");
    }
    // (#1031) the split stages' records at the GI extent (the re-shade list + 96 B per GI texel)
    if (!wantHit && mGiHit.buffer)
    {
        vkDeviceWaitIdle(mCtx->device());
        mCtx->destroyBuffer(mGiHit);
        mGiHit = GpuBuffer{};
        mGiHitW = mGiHitH = 0;
        changed = true;
    }
    if (wantHit && (!mGiHit.buffer || mGiHitW != w || mGiHitH != h))
    {
        vkDeviceWaitIdle(mCtx->device());
        if (mGiHit.buffer) { mCtx->destroyBuffer(mGiHit); mGiHit = GpuBuffer{}; }
        const VkDeviceSize bytes = giHitBytes(w, h);
        {
            VkMemScope memcat(VKMEM_DENOISE);
            VkDeviceContext::SoftAllocScope soft(mCtx);
            // (#1036: + transfer dst -- recordGI clears the hit / miss lists' headers)
            mGiHit = mCtx->createBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT
                                               | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        }
        mGiHitW = mGiHit.buffer ? w : 0u;
        mGiHitH = mGiHit.buffer ? h : 0u;
        changed = true;
        if (!mGiHit.buffer)
        {
            mGiHitRetryAt = now + 30.0;
            LL_WARNS("Vulkan") << "ray-traced GI: the split stages' records (" << ((bytes + (1u << 19)) >> 20)
                               << " MB) were refused (VRAM); the fused GI kernel instead, retry in 30 s" << LL_ENDL;
        }
        else mCtx->setObjectName(VK_OBJECT_TYPE_BUFFER, (uint64_t)mGiHit.buffer, "giRecords");
    }
    if (changed)
    {
        vkDeviceWaitIdle(mCtx->device());
        mGiLagLast.have = false;   // (#978 phase 2: new images / state -- no last frame's GI to reproject)
        writeGiBindings();
        LL_INFOS("Vulkan") << "ray-traced GI: " << w << "x" << h << " (RenderVKGIScale " << scale << "), denoiser "
                           << ((mGiDn->bytes() + (1u << 19)) >> 20) << " MB, reservoirs "
                           << ((mGiRes.buffer ? (uint64_t)w * h * 64u : 0u) >> 20) << " MB, split-stage records "
                           << ((mGiHit.buffer ? (uint64_t)giHitBytes(w, h) : 0u) >> 20) << " MB, state + cache "
                           << ((sizeof(GiHeader) + (uint64_t)slots * 48u) >> 20) << " MB (" << slots << " cache slots)"
                           << LL_ENDL;
    }
    return true;
}

// syncFromStoresPrestep (before the frame's command buffer begins): what the last recording's
// giDecide asked for.
void LLCleanRendererVK::giPrestep()
{
    if (!mGiReq.pending) return;
    const GiReq r = mGiReq;
    mGiReq = GiReq{};
    if (!mCtx || !mGiDn) return;
    if (r.free) { giFreeAll(); return; }
    if (r.w) giApply(r.w, r.h, r.slots, r.wantRes, r.scale, r.wantHit);
    // (#978 phase 2: after the state buffer -- the forward copy takes its cache size)
    if (r.lag) giLagEnsure(r.lagW, r.lagH);
    else if (r.lagFree) giLagFree();
}

// The light pass, after the WAR barrier on last frame's reads, with the other uploads (the barrier
// after them makes it visible to every compute stage).
void LLCleanRendererVK::giWriteHeader(VkCommandBuffer cmd)
{
    if (!mGiNow || !mGiState.buffer) return;
    const GiCfg cfg = giCfg();
    GiHeader h{};
    h.cfg[0] = mGiFlags | (cfg.specOcc << 8) | (cfg.everyLog << 10)   // (clean_gi_common.glsl GI_SPECOCC_SHIFT /
             | (cfg.missProbes ? 4096u : 0u) | (cfg.cullBack ? 8192u : 0u)       //  GI_EVERY_SHIFT / GI_F_MISS_PROBES / _CULL_BACK
             | (cfg.celOcc ? 16384u : 0u) | (cfg.specOccRtRes ? 32768u : 0u)     //  / GI_F_CEL_OCC / GI_F_SPECOCC_RT_RES
             | (cfg.hazeOcc ? 4194304u : 0u);                                    //  / GI_F_HAZE_OCC)
    h.cfg[1] = mGiFrame;
    h.cfg[2] = mGiStateSlots == ~0u ? 0u : mGiStateSlots;
    h.cfg[3] = mGiDebug;
    h.prm[0] = cfg.intensity;
    h.prm[1] = cfg.minAmb;
    h.prm[2] = cfg.rayLen;
    h.prm[3] = (float)cfg.hitAmb;
    h.prm2[0] = (float)cfg.mcap;
    h.prm2[1] = (float)cfg.taps;
    h.prm2[2] = (float)cfg.radius;
    h.prm2[3] = cfg.firefly;
    h.prm3[0] = cfg.cell;
    h.prm3[1] = cfg.levelDist;
    h.prm3[2] = cfg.window;
    h.prm3[3] = cfg.evict;
    h.prm4[0] = cfg.cosMin;
    h.prm4[1] = cfg.reshadeGain;
    // (#87) DISOCCLUSION (camera zoom / pan: silhouettes lose their history): the reservoirs' extra
    // small-radius taps where no temporal candidate survived, the reprojection search, the edge-safe
    // upsample (its GEOM parity: the one this frame's trace writes -- beginFrame flipped it above)
    h.prm4[2] = (float)cfg.disoccTaps;
    h.prm4[3] = mGiDn ? (float)mGiDn->parity() : 0.f;
    // (the counters only while recordGI copies them out: the GPU timers -- else ~13 same-line atomics
    //  per subgroup of the trace for nothing)
    if (mTsOn) h.cfg[0] |= GI_F_STATS;
    if (cfg.reprojSearch) h.cfg[0] |= GI_F_REPROJ_SEARCH;
    if (cfg.upsNormal) h.cfg[0] |= GI_F_UPS_NORMAL;
    // (GI_F_BACK_STOP / GI_F_POINT_SHADOW: the edge sparkles' leaks -- clean_gi_common.glsl)
    if (cfg.backStop) h.cfg[0] |= GI_F_BACK_STOP;
    if (cfg.pointShadow) h.cfg[0] |= GI_F_POINT_SHADOW;
    // (#978 phase 2, GI_F_LAG) the consumers read LAST frame's trace: its final image and GEOM parity
    if (mGiLagNow)
    {
        h.cfg[0] = (h.cfg[0] & ~GI_F_FINAL_IN) | (mGiLagHdrFinalIn ? GI_F_FINAL_IN : 0u) | GI_F_LAG
                 | (mGiLagStale ? GI_F_LAG_STALE : 0u);
        h.prm4[3] = (float)(mGiLagHdrPar & 1u);
    }
    // CHANGED LIGHTS (GI_F_LIGHT_CHG, RenderVKGILightChangeCut): the lights that MOVED (centre / apex /
    // axis: max(RenderVKReSTIRMoveEps, RenderVKGILightMoveRel x radius)) or whose RADIANCE changed
    // (colour / intensity / radius / falloff beyond RenderVKReSTIRFlickerEps) against their GI
    // reference -- mGiChg (#980; DI's per-frame 1 mm masks flagged every worn light every frame) --
    // plus their OLD records (chg_prev). A picked reused sample is re-shaded (clean_deferred_light.comp)
    // only where those lights' unshadowed light, old or new, is at least RenderVKGIReshadeThreshold of
    // its stored radiance (#980: the significance test; 0 = wherever one reaches). The radiance cache
    // is NOT restarted for them (a club's cycling lights restarted the cells under them every frame: the
    // hits' ambient fell back to the probes -- more noise, a different colour); its exponential mean
    // follows the new colour.
    h.prm5[0] = cfg.reshadeThr;
    // (#1041) the own-fixture rule's box limit for the hits' budgeted point-light rays (pass 15's
    // RenderVKReSTIRFixtureSize: a bulb's own shade / lantern / sconce never shadows its light)
    {
        F32 fixture = 1.5f;
#if defined(LL_HAS_CLEAN_PIPELINE)
        static LLCachedControl<F32> s_fix(gSavedSettings, "RenderVKReSTIRFixtureSize", 1.5f);
        fixture = s_fix;
#endif
        h.prm5[2] = std::clamp(fixture, 0.f, 16.f);
    }
    if (cfg.lightChg)
    {
        h.cfg[0] |= GI_F_LIGHT_CHG;
        uint32_t n = 0;
        for (int i = 0; i < 8; ++i)
        {
            h.chg[i] = mGiChg[i];
            for (uint32_t b = h.chg[i]; b; b &= b - 1u) ++n;
        }
        h.prm5[1] = (float)mGiChgPrevN;
        std::memcpy(h.chg_prev, mGiChgPrev, sizeof(float) * 8u * mGiChgPrevN);
        mGiStatAcc.chgLights += n;
        mGiStatAcc.allLights += (double)std::min<size_t>(mLightsCPU.size(), 256u);
        ++mGiStatAcc.hdrFrames;
    }
    // (GI-4) the change spheres since the last header (bounded; an overflow restarts the cache)
    const size_t ns = std::min<size_t>(mGiSpheres.size() / 4, 64);
    h.sph_n[0] = (uint32_t)ns;
    h.sph_n[1] = mGiSphOverflow ? 1u : 0u;
    for (size_t i = 0; i < ns; ++i) std::memcpy(h.sph[i], &mGiSpheres[i * 4], sizeof(h.sph[i]));
    mGiSpheres.clear();
    if (mGiSphOverflow && !mGiStateFresh)
    {
        // (a change-sphere overflow wipes the cache: counted + logged at most every 10 s -- a sim whose
        //  changes overflow every frame would keep the cache near empty)
        ++mGiCacheRestarts;
        static double s_next = 0.0;
        const double now = giClock();
        if (now >= s_next)
        {
            s_next = now + 10.0;
            LL_INFOS("VKShadow") << "GI radiance cache restarted: more scene changes than its 64 change spheres this "
                                    "frame (a region loading / a rez storm); " << mGiCacheRestarts << " restarts so far"
                                 << LL_ENDL;
        }
    }
    if (mGiStateFresh || mGiSphOverflow)
    {   // a new / restarted cache: every slot empty (the WAR barrier above covers the fill)
        if (mGiStateSlots != ~0u && mGiStateSlots != 0u)
            vkCmdFillBuffer(cmd, mGiState.buffer, sizeof(GiHeader), (VkDeviceSize)mGiStateSlots * 48u, 0u);
        mGiStateFresh = false;
    }
    mGiSphOverflow = false;
    vkCmdUpdateBuffer(cmd, mGiState.buffer, 0, sizeof(h), &h);
}

// invalidateProjShadows (the ingest's static change spheres this frame, before they are cleared):
// kept for the next GI header (the cache invalidates the cells they touch).
void LLCleanRendererVK::giNoteSpheres()
{
    extern std::vector<float> gVKChangedSpheres;
    extern bool gVKChangedOverflow;
    if (!mGiNow && !mGiWanted) { mGiSpheres.clear(); return; }
    const size_t n = gVKChangedSpheres.size() / 4;
    if (gVKChangedOverflow)
    {   // (the ingest itself lost track: nothing local is trustworthy -- the cache restarts)
        mGiSpheres.clear();
        mGiSphOverflow = true;
        return;
    }
    for (size_t i = 0; i < n; ++i)
    {
        const float* s = &gVKChangedSpheres[i * 4];
        if (!std::isfinite(s[0]) || !std::isfinite(s[1]) || !std::isfinite(s[2]) || !std::isfinite(s[3])) continue;
        giAddSphere(mGiSpheres, s);
    }
}

bool LLCleanRendererVK::giTraceReady() const
{
    // (this frame's TLAS: giDecide decided on last frame's build)
    return mGiNow && mGiDn && mGiDn->allocated() && mGiTracePipe && mRtReflLayout && mRT && mRT->tlasValid()
        && mRT->consumerSet() != VK_NULL_HANDLE;
}

// ---- (#978) GI EARLY TRACE (RenderVKGIEarlyTrace, default 1, live) ----
// THE FRAME GRAPH (split deferred lighting, async queue on, the early pre-pass + early probe faces;
// G = graphics queue, A = async compute queue, in recording order; "=>" = a semaphore edge):
//   G: front-end join, terrain bake, alpha sort, #901 shelter + accum (the G-buffer reads them),
//      pre-pass (1a), VRS rate, G-buffer
//   A: RT build (BLAS refit / TLAS: q_rt_build)            => joined on G before the shadow culls
//   G: shadow rig cull, cascades, projector tiles (+ records b19), weather occlusion / sim / snow
//   A: mask / SSAO (q_mask_ao; forked after the projector tiles: the trace reads b19)
//   G: mirror (+ the late particle compute)
//   A: THE DEFERRED CHAIN (q_defer_light, recordDeferredLighting's split / feature-split kernels):
//      prep -> [merged RT shadows: sun certificates, the ReSTIR DI EMISSIVE BUILD (#91 / #986), the
//      #869 occlusion resolve, pass 15 + the projector denoiser] -> SHADOW(+CLASSIFY) -> REFLECT ->
//      RT reflections (#859) -> [unmerged: pass 15 + the emissive build here] -> GI (the trace,
//      pass 11 -DCLEAN_GI_TRACE; the radiance-cache resolve; the GI denoiser) -> BASE + LOCAL (or
//      COMBINE): the lit HDR + glow
//   G: probe faces (pr_early)                              => [old: join the chain: g_join_defer]
//   G: vol fog (A: q_volumetrics), RT probe faces (A: q_probe_rt), sky, mirror occlusion, alpha
//      depth, particle lighting, cel, god rays / vol clouds, ACOUSTICS TRACE, pass 2, post.
// WHAT FORCES THE SERIALIZATION: every graphics pass after the join but the acoustics trace reads
// or draws over the lit HDR, or writes the depth the chain reads (alpha depth); every graphics
// pass before the fork is an input of the chain (the G-buffer, the TLAS, the projector tiles, the
// shelter maps the G-buffer reads). The GI trace itself reads the G-buffer + depth, this frame's
// TLAS, the light records / clusters / light grid, the projector records + atlas (GI hits take
// shadow-mapped projectors: clean_rt_shade.glsl rtProjShadowMapQ0), the weather maps at hits
// (binding 49), last frame's filtered probes, and THIS FRAME'S EMISSIVE BUILD + OCCLUSION LISTS
// (mRegir: EM_F_GI_SPLIT, the light-grid picks) -- built at the chain's head, inside the ReSTIR
// pass (recordProjStoch). It may not run beside the RT reflection kernels (they read the GI header
// + radiance cache the trace inserts / accumulates into and the resolve rewrites, and write the
// light grid's stat words) nor pass 15 (writes mRegir): so it cannot start before them, and on the
// same queue its order there costs nothing. Its consumers: BASE / COMBINE (the denoised GI + the
// header); after the combine the forward lit FS (draw set 0 b53, the cache), next frame's RT
// reflections / GI hits (the cache). Not the vol fog (its own sky-visibility rays), the probe RT
// faces, the acoustics trace.
// EARLY TRACE: the chain stays one async segment (its head + the GI trace start exactly as early
// as before -- nothing on the async queue precedes them but the mask / SSAO), but it STOPS before
// the combine (deferStashCombine: BASE + LOCAL / COMBINE stashed in mDeferTail); the graphics queue
// no longer joins it after the probe faces but records the acoustics trace and forks the vol fog /
// RT probe segments first, and joins it UP TO its own value (the later segments stay outstanding)
// in pass 1b right before the combine, which then runs on graphics (recordDeferTail).
// Needs RenderVKAsyncLayoutsOnGraphics (depth stays DEPTH_READ_ONLY on graphics across the window).
// Timers: g_gi_fork (the fork on graphics), g_gi_window (the window's unmarked rest), g_join_gi (the
// pure wait), g_defer_tail (the combine), derived q_gi_beside (graphics work in the window) and
// q_gi_hidden (q_defer_light - g_join_gi: the chain time graphics did not wait for).
bool LLCleanRendererVK::deferStashCombine(bool feat, const VKDeferPush& p, uint32_t viewOff, uint32_t rw, uint32_t rh)
{
    const bool armed = mGiEarlyArm;
    mGiEarlyArm = false;
    if (!armed) return false;
    mDeferTail.pending = true;
    mDeferTail.giLag = mGiLagNow;   // (#978 phase 2: the trace follows the combine -- giLagRecord)
    mDeferTail.feat = feat;
    mDeferTail.p = p;
    mDeferTail.viewOff = viewOff;
    mDeferTail.rw = rw;
    mDeferTail.rh = rh;
    return true;
}

void LLCleanRendererVK::recordDeferTail(VkCommandBuffer& cmd)
{
    if (!mDeferTail.pending) return;
    mDeferTail.pending = false;
    // A continuation after the join: nothing bound. The split layout's sets + this frame's push,
    // as recordGI / the beforeCombine join leave them.
    const VkDescriptorSet sets[4] = {mDsetDraw0, mDsetDraw1, mDsetDefer, mDsetDeferSplit};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mDeferSplitLayout, 0, 4, sets, 1, &mDeferTail.viewOff);
    vkCmdPushConstants(cmd, mDeferSplitLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(mDeferTail.p), &mDeferTail.p);
    recordDeferCombine(cmd, mDeferTail.feat, mDeferTail.rw, mDeferTail.rh, false);
}

// ---- (#978 PHASE 2) GI LATENCY FRAME (RenderVKGILatencyFrame, default 0, live) ----
// Phase 1 left the GI trace (c_gi_trace 9-13 ms in the owner's sim) inside this frame's q_defer_light:
// graphics still waited for it (g_join_gi) before the combine, because the combine reads its result.
// ONE FRAME OF LATENCY: the combine reads LAST frame's GI -- reprojected (clean_deferred_light.comp
// deferGiFetchLag; header GI_F_LAG) -- and this frame's trace + cache resolve + GI denoiser run AFTER
// the combine, on their own async segment, beside everything graphics still has to record:
//   G: ... G-buffer, cascades / projector tiles / weather, mirror      (as phase 1)
//   A: q_mask_ao, q_defer_light = THE HEAD ONLY: prep -> [merged RT shadows: sun certificates, ReSTIR DI
//      emissive build, occlusion resolve, pass 15 + projector denoiser] -> SHADOW(+CLASSIFY) -> REFLECT ->
//      RT reflections -> [unmerged: pass 15] -- stops BEFORE recordGI (deferStashCombine, giLag)
//   G: probe faces, acoustics trace, vol fog / RT probe forks (the phase-1 window) -> join up to the head
//      (g_join_gi: now the shadows + reflections only) -> the combine (g_defer_tail; reads GI(N-1))
//      -> giLagRecord: depth snapshot + the forward state copy -> FORK q_gi_lag
//   A: q_gi_lag: [GI trace -> cache resolve -> GI denoiser] for frame N (reads the snapshot, writes the
//      GI images / reservoirs / state the combine of frame N + 1 reads)
//   G: scope 2 (sky, mirror occlusion, ALPHA DEPTH), particle lighting, cel, god rays (kept on graphics:
//      an async segment forked after q_gi_lag would queue behind it and its join would wait for GI),
//      pass 2 (water, alpha, particles), motion blur, the TAA / DLSS / FSR resolve
//      -> g_gi_lag_window | join up to q_gi_lag (g_join_gi_lag: the pure wait, ~0 when GI fit beside)
//   G: the SSR copy, bloom, exposure, tonemap, FX, selection + HUD, P (pipelining), probe filters (q_post
//      or graphics), the caller's UI replay, present. (giLagJoin again after the HUD: a backstop, no-op.)
// WHY THE JOIN BEFORE THE SSR COPY: the GI kernel's static descriptor use (what sync validation checks,
// whatever runs at run time) covers draw set 0's previous colour / depth (b7 / b8: the SSR copy writes
// them) and set 2 / 3 images on the render-target alias block (the G-buffer, below render scale 1 the
// split intermediates mDsImg: the bloom pyramid / cross filter -- written after the resolve -- share that
// memory). Everything between the combine and the SSR copy writes none of the GI kernel's resources (the
// HDR / glow / reactive / refraction targets, the TAA / DLSS state, particle records, cel targets: not
// bound to it), so nothing else is double-buffered. Going further (overlapping bloom .. HUD too) needs a
// draw set 0 copy without b7 / b8 and the G-buffer + mDsImg off the alias block (~20 B / render px).
// WHY NOT ACROSS THE FRAME BOUNDARY (to frame N + 1's combine): besides the G-buffer and depth the trace
// reads this frame's TLAS (frame N + 1's build refits it in place), the light records / light info /
// frame info (single buffers: frame N + 1's light pass rewrites them, in the front end -- possibly on the
// async queue itself, frame pipelining), the projector records + atlas, the weather maps, the emissive
// build + occlusion lists in mRegir (frame N + 1's ReSTIR pass), the probe arrays (this frame's filters)
// and bindless textures (retired against the in-flight FENCE, which covers the graphics tail only).
// Double-buffering all of that is several hundred MB plus a ring for each of those writers; joining in the
// frame keeps every lifetime as it is (the tail, and so the fence, follows the join; asyncBeginFrame's
// re-wait of mAsyncJoinedLast orders it before frame N + 1's first graphics submission too).
// WHAT IS SNAPSHOT / DOUBLE-BUFFERED (only while the setting is on; logged with sizes at creation):
//   - the pre-pass DEPTH (4 B / render px): the alpha-depth pass writes the live depth in scope 2 and the
//     layout returns to ATTACHMENT; copied right after the combine (graphics) into mGiLagDepth, which the
//     trace reads through mGiLagSet (set 2: binding 4 = the copy, the rest copied from mDsetDefer every
//     lag frame -- one set per frame slot, updated while no submitted work uses it: asyncBeginFrame
//     host-waited the slot's compute work). The G-buffer itself is not written again before frame N + 1's
//     G-buffer pass (and its alias partner only after the join);
//   - the FORWARD STATE (header + resolved cells, 32 B / cache slot): the trace inserts / accumulates /
//     resolves into mGiState while pass 2's forward lit FS reads the cache (draw set 0 b53): b53 names
//     mGiStateFwd, copied from mGiState before the fork (the forward pass reads GI(N-1)'s cache with this
//     frame's header -- the deferred pixels' lag exactly);
//   - the GI OUTPUTS need no second copy: the combine (graphics, before the fork) is their only reader in
//     the frame; the next writer is this frame's q_gi_lag (ordered by the fork's semaphore).
// INVALIDATION: the header's GI_F_LAG_STALE (giDecide: mGiLagLast -- last recording traced into these
// images at this extent, consecutive GI frames; new images / state, a resize, a GI setting change that
// re-creates them, an offscreen / dropped recording in between) and the frame info's history reset
// (teleport / region crossing / camera cut: ssr.taa_params.z) -> the combine takes the radiance cache's
// cell, else the probes. Disocclusions (camera motion) per pixel the same. Probe faces / cube snapshots /
// the mirror never run the trace (forward views: they read the forward copy); an offscreen recording runs
// no GI at all. Resources are (re)created only at the frame start (giPrestep), never in a recording; a
// frame whose resources are not in place runs GI_F_LAG off or the trace on graphics after the combine.
// FALLBACKS (always "trace after the combine", so the header's GI_F_LAG stays true for the frame): no
// async fork / the snapshot at another extent / the set missing -> the trace on graphics right after
// the combine (live depth, no snapshot); the chain not stashed (RenderVKGIEarlyTrace paths off, the
// lighting on graphics) -> the trace right after the combine inside recordDeferredLighting.
// TIMERS: g_join_gi (the head), g_gi_lag_fork (the snapshot + copy + fork), q_gi_lag (the segment, async
// clock: c_gi_trace / c_gi_cache / c_gi_dn_* inside), g_gi_lag_window (graphics work beside it, since the
// last mark), g_join_gi_lag (the wait), derived q_gi_lag_hidden = q_gi_lag - g_join_gi_lag.
bool LLCleanRendererVK::giLagReady(uint32_t rw, uint32_t rh) const
{
    return mGiFwdBound && mGiStateFwd.buffer && mGiState.buffer && mGiStateFwdSlots == mGiStateSlots
        && mGiLagDepth.image && mGiLagDepthW == rw && mGiLagDepthH == rh && mGiLagDepthFmt == mDepthFormat
        && mGiLagSet[0] && mGiLagSet[kAsyncSlots - 1];
}

void LLCleanRendererVK::giLagDestroyRes()
{
    if (!mCtx) return;
    if (mGiStateFwd.buffer) { mCtx->destroyBuffer(mGiStateFwd); mGiStateFwd = GpuBuffer{}; }
    if (mGiLagDepth.image) { mCtx->destroyImage(mGiLagDepth); mGiLagDepth = GpuImage{}; }
    mGiStateFwdSlots = ~0u;
    mGiLagDepthW = mGiLagDepthH = 0;
    mGiLagDepthFmt = VK_FORMAT_UNDEFINED;
    mGiLagLast.have = false;
}

void LLCleanRendererVK::giLagFree()
{
    if (!mCtx || (!mGiStateFwd.buffer && !mGiLagDepth.image)) return;
    vkDeviceWaitIdle(mCtx->device());
    giLagDestroyRes();
    writeGiBindings();   // (b53 back on mGiState)
    LL_INFOS("Vulkan") << "one-frame-latency GI (#978) off: its depth snapshot / forward state copy freed" << LL_ENDL;
}

// The mode's resources at render extent rw x rh (giPrestep, or in place with RenderVKResizeAtFrameStart 0):
// each change waits for the device; b53 rewritten. False: refused (#697: retry in 30 s, the mode stays off).
bool LLCleanRendererVK::giLagEnsure(uint32_t rw, uint32_t rh)
{
    if (!mCtx || !mGiState.buffer || !mDslDefer || !mDsetDefer || !rw || !rh) return false;
    VkDevice dev = mCtx->device();
    const double now = giClock();
    bool changed = false;
    const uint32_t slots = mGiStateSlots == ~0u ? 0u : mGiStateSlots;
    if (!mGiStateFwd.buffer || mGiStateFwdSlots != slots)
    {
        vkDeviceWaitIdle(dev);
        if (mGiStateFwd.buffer) { mCtx->destroyBuffer(mGiStateFwd); mGiStateFwd = GpuBuffer{}; }
        const VkDeviceSize bytes = sizeof(GiHeader) + (VkDeviceSize)slots * 32u + 16u;
        {
            VkMemScope memcat(VKMEM_DENOISE);
            VkDeviceContext::SoftAllocScope soft(mCtx);
            mGiStateFwd = mCtx->createBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        }
        if (!mGiStateFwd.buffer)
        {
            mGiLagRetryAt = now + 30.0;
            LL_WARNS("Vulkan") << "one-frame-latency GI (#978): the forward state copy (" << ((bytes + (1u << 19)) >> 20)
                               << " MB) was refused (VRAM); the mode stays off, retry in 30 s" << LL_ENDL;
            writeGiBindings();
            return false;
        }
        mCtx->setObjectName(VK_OBJECT_TYPE_BUFFER, (uint64_t)mGiStateFwd.buffer, "giStateFwd");
        // (zeroed: a forward pass before the first copy reads GI_F_ON clear -- the probes)
        const VkBuffer fb = mGiStateFwd.buffer;
        mCtx->submitImmediate([fb](VkCommandBuffer c) { vkCmdFillBuffer(c, fb, 0, VK_WHOLE_SIZE, 0u); });
        mGiStateFwdSlots = slots;
        changed = true;
    }
    if (!mGiLagDepth.image || mGiLagDepthW != rw || mGiLagDepthH != rh || mGiLagDepthFmt != mDepthFormat)
    {
        vkDeviceWaitIdle(dev);
        if (mGiLagDepth.image) { mCtx->destroyImage(mGiLagDepth); mGiLagDepth = GpuImage{}; }
        {
            VkMemScope memcat(VKMEM_DENOISE);
            VkDeviceContext::SoftAllocScope soft(mCtx);
            mGiLagDepth = mCtx->createImage2D(rw, rh, mDepthFormat, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                              VK_IMAGE_ASPECT_DEPTH_BIT);
        }
        mGiLagDepthW = mGiLagDepth.image ? rw : 0u;
        mGiLagDepthH = mGiLagDepth.image ? rh : 0u;
        mGiLagDepthFmt = mGiLagDepth.image ? mDepthFormat : VK_FORMAT_UNDEFINED;
        changed = true;
        if (!mGiLagDepth.image)
        {
            mGiLagRetryAt = now + 30.0;
            LL_WARNS("Vulkan") << "one-frame-latency GI (#978): the " << rw << "x" << rh << " depth snapshot was refused (VRAM);"
                                  " the mode stays off, retry in 30 s" << LL_ENDL;
            writeGiBindings();
            return false;
        }
        mCtx->setObjectName(VK_OBJECT_TYPE_IMAGE, (uint64_t)mGiLagDepth.image, "giLagDepth");
    }
    if (!mGiLagSet[0])
    {
        // set 2 copies for the q_gi_lag segment (one per frame slot; same layout as mDsetDefer)
        if (mGiLagPool.handle() == VK_NULL_HANDLE
            && mGiLagPool.create(dev, "GI lag set 2", {{mDslDefer, kAsyncSlots}}) != VK_SUCCESS)
        {
            mGiLagRetryAt = now + 30.0;
            return false;
        }
        for (uint32_t i = 0; i < kAsyncSlots; ++i)
            if (mGiLagPool.allocate(mDslDefer, &mGiLagSet[i]) != VK_SUCCESS) mGiLagSet[i] = VK_NULL_HANDLE;
        if (!mGiLagSet[0] || !mGiLagSet[kAsyncSlots - 1])
        {
            mGiLagRetryAt = now + 30.0;
            LL_WARNS("Vulkan") << "one-frame-latency GI (#978): its descriptor sets were refused; the mode stays off" << LL_ENDL;
            return false;
        }
    }
    if (changed)
    {
        vkDeviceWaitIdle(dev);
        mGiLagLast.have = false;
        writeGiBindings();
        LL_INFOS("Vulkan") << "one-frame-latency GI (#978, RenderVKGILatencyFrame): depth snapshot " << rw << "x" << rh
                           << " " << (((uint64_t)rw * rh * 4u + (1u << 19)) >> 20) << " MB, forward state copy "
                           << ((sizeof(GiHeader) + (uint64_t)slots * 32u + (1u << 19)) >> 20) << " MB (" << slots
                           << " cache slots)" << LL_ENDL;
    }
    return true;
}

// mGiState (header + resolved cells) -> mGiStateFwd for pass 2's forward lit FS (b53): once per GI frame,
// on graphics -- before q_gi_lag's fork on a lag frame, else after the lighting (+ its GI) is complete.
void LLCleanRendererVK::giFwdCopy(VkCommandBuffer cmd)
{
    if (mGiFwdCopied) return;
    mGiFwdCopied = true;
    if (!mGiFwdBound || !mGiStateFwd.buffer || !mGiState.buffer || mGiStateFwdSlots != mGiStateSlots) return;
    const VkDeviceSize slots = mGiStateSlots == ~0u ? 0u : mGiStateSlots;
    // WAR: last frame's forward reads (FRAGMENT) / copy; RAW: this frame's header upload (transfer), the
    // lighting's / an in-queue trace's storage writes (COMPUTE). (q_gi_lag of the last frame: joined.)
    VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    m.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
                   | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    m.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
    m.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    m.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1; d.pMemoryBarriers = &m;
    vkCmdPipelineBarrier2(cmd, &d);
    const VkBufferCopy c{0, 0, sizeof(GiHeader) + slots * 32u};   // (the accumulators: the trace's own)
    vkCmdCopyBuffer(cmd, mGiState.buffer, mGiStateFwd.buffer, 1, &c);
    m.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    m.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    m.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    m.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    vkCmdPipelineBarrier2(cmd, &d);
}

// recordWorld pass 1b, right after the stashed combine (recordDeferTail; depth still DEPTH_READ_ONLY
// when depthRoHeld): this frame's GI trace. On the async queue (q_gi_lag) when everything it needs is in
// place -- then depth leaves for the attachment layout here (depthRoHeld cleared); else on graphics now.
void LLCleanRendererVK::giLagRecord(VkCommandBuffer& cmd, bool& depthRoHeld)
{
    if (!mDeferTail.giLag) return;
    mDeferTail.giLag = false;
    const VKDeferPush p = mDeferTail.p;
    const uint32_t viewOff = mDeferTail.viewOff;
    // (no trace: the next frame's header says GI_F_LAG_STALE. Depth not READ_ONLY -- never expected: the
    //  stash needs RenderVKAsyncLayoutsOnGraphics, which holds it so -- set 2 binding 4 could not be read.)
    if (!giTraceReady() || !depthRoHeld) return;
    const VkDescriptorSet set = mGiLagSet[mAsyncSlot % kAsyncSlots];
    const bool async = mAsyncNow.on && set && mDepth.image
                    && giLagReady(mDepthW, mDepthH) && mDepthW == p.width && mDepthH == p.height;
    if (!async)
    {
        // (graphics, the live depth still READ_ONLY: set 2 as the combine had it)
        if (mAsyncNow.on) LLVKHitch::count("giLagOnGraphics", 1u);
        recordGI(cmd, p, viewOff, false);
        return;
    }
    // 1. THE DEPTH SNAPSHOT (the combine's / the head's sampled reads are done: the join + this queue)
    {
        VkImageMemoryBarrier2 b[2]{};
        b[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        b[0].srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        b[0].srcAccessMask = VK_ACCESS_2_NONE;
        b[0].dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT; b[0].dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        b[0].oldLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL; b[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b[0].image = mDepth.image; b[0].subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        // (last frame's q_gi_lag reads of the copy: joined last frame; contents discarded)
        b[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        b[1].srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; b[1].srcAccessMask = VK_ACCESS_2_NONE;
        b[1].dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT; b[1].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b[1].image = mGiLagDepth.image; b[1].subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.imageMemoryBarrierCount = 2; d.pImageMemoryBarriers = b;
        vkCmdPipelineBarrier2(cmd, &d);
        VkImageCopy r{};
        r.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
        r.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
        r.extent = {mDepthW, mDepthH, 1};
        vkCmdCopyImage(cmd, mDepth.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, mGiLagDepth.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        // the live depth -> the attachment layout for scope 2 (what depthToAttachGfx does from READ_ONLY);
        // the copy -> the trace's sampled reads (set 2 binding 4: DEPTH_READ_ONLY)
        b[0].srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT; b[0].srcAccessMask = VK_ACCESS_2_NONE;
        b[0].dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        b[0].dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        b[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; b[0].newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        b[1].srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT; b[1].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b[1].dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; b[1].dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        b[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b[1].newLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier2(cmd, &d);
        depthRoHeld = false;
    }
    // 2. pass 2's forward copy of the state (GI(N-1)'s cache, this frame's header)
    giFwdCopy(cmd);
    // 3. this slot's set 2: mDsetDefer's descriptors (the G-buffer, the light grid, the
    //    froxels, the view UBO, the reservoir grid; not 7 / 11: the trace uses neither) + binding 4 = the copy.
    //    Legal here: nothing submitted uses it (asyncBeginFrame host-waited this slot's compute work) and
    //    nothing recorded so far binds it.
    {
        static const uint32_t kCopy[] = {0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u, 10u, 12u};
        VkCopyDescriptorSet cp[sizeof(kCopy) / sizeof(kCopy[0])]{};
        uint32_t n = 0;
        for (uint32_t b : kCopy)
        {
            VkCopyDescriptorSet& c = cp[n++];
            c.sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET;
            c.srcSet = mDsetDefer; c.srcBinding = b; c.srcArrayElement = 0;
            c.dstSet = set; c.dstBinding = b; c.dstArrayElement = 0;
            c.descriptorCount = 1;
        }
        const VkDescriptorImageInfo di{VK_NULL_HANDLE, mGiLagDepth.view, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set; w.dstBinding = 4; w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; w.pImageInfo = &di;
        vkUpdateDescriptorSets(mCtx->device(), 1, &w, n, cp);
    }
    // 4. the fork: q_gi_lag waits everything recorded on graphics so far (the combine's reads of the GI
    //    images / state, the copies above); NOT joined here -- giLagJoin before the SSR copy. The outstanding
    //    value of the segments forked before it (vol fog, RT probe faces) is kept for their own joins.
    const uint64_t prevOut = mAsyncOutstanding;
    VkCommandBuffer lc = asyncFork(cmd, "q_gi_lag");
    mGiSet2 = set;
    if (!lc)
    {
        // (no fork -- the segment budget: on graphics, from the snapshot)
        LLVKHitch::count("giLagOnGraphics", 1u);
        recordGI(cmd, p, viewOff, false);
        mGiSet2 = VK_NULL_HANDLE;
        return;
    }
    recordGI(lc, p, viewOff, true);
    mGiSet2 = VK_NULL_HANDLE;
    asyncEndCompute(lc, "q_gi_lag");
    mGiLagValue = mAsyncOutstanding;
    mAsyncOutstanding = prevOut;
    gpuSubMark(cmd, "g_gi_lag_fork");
}

// recordWorld, after the TAA / upscaler resolve, before the SSR copy (+ a backstop after the HUD, before
// P / the probe filters): the q_gi_lag segment's join (up to its value: anything forked after it stays
// outstanding).
void LLCleanRendererVK::giLagJoin(VkCommandBuffer& cmd)
{
    if (!mGiLagValue) return;
    const uint64_t v = mGiLagValue;
    gpuSubMark(cmd, "g_gi_lag_window");   // (graphics work beside the segment since the last mark)
    asyncJoinUpTo(cmd, v);
    if (mAsyncJoinedLast >= v)
    {
        mGiLagValue = 0;
        gpuSubMark(cmd, "g_join_gi_lag");   // (the pure wait)
        return;
    }
    // (no graphics continuation left: asyncSubmitFrame's tail waits it -- correct, later)
    LLVKHitch::count("giLagJoinDeferred", 1u);
    LL_WARNS_ONCE("Vulkan") << "#978: the one-frame-latency GI segment's join moved to the frame's tail "
                               "(graphics continuation budget)" << LL_ENDL;
}

// After the RT reflections and the projector sampler, before BASE / COMBINE (both split paths):
// the GI trace, the cache's resolve, the GI denoiser; the split layout's sets + push re-bound after.
void LLCleanRendererVK::recordGI(VkCommandBuffer cmd, const VKDeferPush& p, uint32_t viewOff, bool onAsync)
{
    if (!giTraceReady()) return;
    const VkDescriptorSet rtSet = mRT->consumerSet();
    const GiCfg cfg = giCfg();
    const uint32_t dw = mGiDn->width(), dh = mGiDn->height();
    const bool split = mGiSplitNow && giSplitReady(dw, dh);
    const bool shadeSplit = split && mGiShadeSplitNow && giShadeSplitReady();   // (#1036)
    // (#1039 M1, RenderVKVisQueue) THE VISIBILITY QUEUE: HIT-MAT queues the hits' sun rays, the walk traces
    // them, HIT-LIGHT resolves (no sun walk in it); the hits past the region take the fused HIT-LIGHT as a tail
    // (#1039 M2, bit 2: pipeline set 1) + the budget's light rays queued: HIT-LIGHT holds no ray query
    const uint32_t vs = mVqSet;
    const bool vq = shadeSplit && mVqGiNow && mVq.buffer && mVisWalkPipe && mGiVqPipe[vs][VQS_MAT] && mGiVqPipe[vs][VQS_LIGHT];
    if (vq) visQueueBegin(cmd, VQR_GI, 0u, 0u);
    if (shadeSplit)
    {
        // (#1036) the HIT / MISS lists' headers (count 0, dispatch (0, 0, 1)) before TRACE appends: last
        // frame's stages read them (storage + indirect) -- WAR -- then the clear
        VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        m.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
        m.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT; m.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.memoryBarrierCount = 1; d.pMemoryBarriers = &m;
        vkCmdPipelineBarrier2(cmd, &d);
        const uint32_t hdr[8] = {0u, 0u, 0u, 1u, 0u, 0u, 0u, 1u};
        vkCmdUpdateBuffer(cmd, mGiHit.buffer, 16u, sizeof(hdr), hdr);
    }
    {   // WAR on the GI images / reservoirs / cache (last frame's readers) + the header upload
        // (#1031: + the re-shade list's header, last frame's indirect dispatches read it)
        // (#1036: + the HIT / MISS headers' clear -> TRACE's appends and the stages' indirect reads)
        VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        m.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
        m.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        m.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        m.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        if (shadeSplit)
        {
            m.srcStageMask |= VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            m.srcAccessMask |= VK_ACCESS_2_TRANSFER_WRITE_BIT;
            m.dstStageMask |= VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
            m.dstAccessMask |= VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
        }
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.memoryBarrierCount = 1; d.pMemoryBarriers = &m;
        vkCmdPipelineBarrier2(cmd, &d);
    }
    VKRtReflPush rp{};
    rp.d = p;
    rp.d.flags = p.flags & 0x00FFFFFFu;
    rp.rtr[0] = cfg.rayLen;
    // rtrOpts: bits 1-3 the hit's projector shadow-ray budget, bit 4 the light grid (built this
    // recording), bit 7 off (no RT reflection cost counters), bits 8-9 0 (raster-parity alpha test)
    rp.rtr[1] = (float)((std::min<U32>(cfg.hitShadow, 4u) << 1) | (mRegirNow ? 16u : 0u));
    rp.rtr[2] = 0.f;
    rp.rtr[3] = std::max(mSamplerAniso, 1.f);
    rp.rtr2[0] = (float)dw;
    rp.rtr2[1] = (float)dh;
    rp.rtr2[2] = (float)(mGiFrame & 0xFFFFFu);
    rp.rtr2[3] = (float)mGiDn->parity();
    // (set 2: #978 phase 2's per-slot copy with binding 4 = the depth snapshot, on the q_gi_lag segment)
    const VkDescriptorSet set2 = mGiSet2 ? mGiSet2 : mDsetDefer;
    const VkDescriptorSet sets[5] = {mDsetDraw0, mDsetDraw1, set2, mDsetDeferSplit, rtSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mRtReflLayout, 0, 5, sets, 1, &viewOff);
    VK_PUSH_BUDGET_CHECK(rp);
    vkCmdPushConstants(cmd, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(rp), &rp);
    if (split)
    {
        // (#1031) THE STAGES (clean_deferred_light.comp THE GI KERNEL SPLIT), each reading what the one
        // before it wrote into the records (compute -> compute RAW); RESHADE and TAIL take their
        // workgroup count from the re-shade list's header (REUSE's appends: + the indirect read).
        auto raw = [cmd](bool indirect)
        {
            VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            m.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; m.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            m.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            m.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            if (indirect)
            {
                m.dstStageMask |= VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
                m.dstAccessMask |= VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
            }
            VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            d.memoryBarrierCount = 1; d.pMemoryBarriers = &m;
            vkCmdPipelineBarrier2(cmd, &d);
        };
        // 1 TRACE: the fresh rays' walks -> hit records (+ the list's header reset; #1036: + the HIT /
        // MISS lists, push rtr.z = 1)
        if (shadeSplit)
        {
            VKRtReflPush tp = rp;
            tp.rtr[2] = 1.f;
            vkCmdPushConstants(cmd, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(tp), &tp);
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mGiSplitPipe[GIS_TRACE]);
        vkCmdDispatch(cmd, (dw + 7u) / 8u, (dh + 7u) / 8u, 1);
        gpuSubMark(cmd, onAsync ? "c_gi_trace" : "g_gi_trace");
        if (shadeSplit)
        {
            // (#1036) 2 as MISS + HIT-MAT (independent texels: no barrier between them) -> HIT-LIGHT, each
            // indirect over its list (TRACE's appends: + the indirect read)
            vkCmdPushConstants(cmd, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(rp), &rp);
            raw(true);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mGiShadePipe[GSS_MISS]);
            vkCmdDispatchIndirect(cmd, mGiHit.buffer, 2u * 16u + 4u);
            gpuSubMark(cmd, onAsync ? "c_gi_miss" : "g_gi_miss");
            VKRtReflPush vp = rp;   // (#1039: the queue's stages -- push rtr3.zw = the region)
            if (vq)
            {
                const VkDeviceAddress qa = visQueueAddr(VQR_GI);
                vp.rtr3[2] = (uint32_t)(qa & 0xFFFFFFFFull);
                vp.rtr3[3] = (uint32_t)(qa >> 32);
                vkCmdPushConstants(cmd, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(vp), &vp);
            }
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vq ? mGiVqPipe[vs][VQS_MAT] : mGiShadePipe[GSS_HITMAT]);
            vkCmdDispatchIndirect(cmd, mGiHit.buffer, 1u * 16u + 4u);
            gpuSubMark(cmd, onAsync ? "c_gi_hitmat" : "g_gi_hitmat");
            if (vq)
            {
                // HIT-MAT's requests + its raised dispatch -> the walk; the results -> HIT-LIGHT
                raw(true);
                visQueueWalk(cmd, VQR_GI, rp);
                gpuSubMark(cmd, onAsync ? "c_gi_walk" : "g_gi_walk");
                raw(false);
                vkCmdPushConstants(cmd, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(vp), &vp);
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mGiVqPipe[vs][VQS_LIGHT]);
                vkCmdDispatchIndirect(cmd, mGiHit.buffer, 1u * 16u + 4u);
                if (mVqRegion[VQR_GI].entries < dw * dh)
                {
                    // the hits past the region: the fused HIT-LIGHT from the first of them (independent texels)
                    VKRtReflPush tp = rp;
                    tp.rtr3[1] = mVqRegion[VQR_GI].entries;
                    vkCmdPushConstants(cmd, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(tp), &tp);
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mGiShadePipe[GSS_HITLIGHT]);
                    vkCmdDispatchIndirect(cmd, mGiHit.buffer, 1u * 16u + 4u);
                }
                gpuSubMark(cmd, onAsync ? "c_gi_hitlight" : "g_gi_hitlight");
                vkCmdPushConstants(cmd, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(rp), &rp);
                visQueueStats(cmd, VQR_GI);
            }
            else
            {
                raw(false);
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mGiShadePipe[GSS_HITLIGHT]);
                vkCmdDispatchIndirect(cmd, mGiHit.buffer, 1u * 16u + 4u);
                gpuSubMark(cmd, onAsync ? "c_gi_hitlight" : "g_gi_hitlight");
            }
        }
        else
        {
            // 2 SHADE: the hits' (or the sky's) radiance -> the samples
            raw(false);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mGiSplitPipe[GIS_SHADE]);
            vkCmdDispatch(cmd, (dw + 7u) / 8u, (dh + 7u) / 8u, 1);
            gpuSubMark(cmd, onAsync ? "c_gi_shade" : "g_gi_shade");
        }
        // 3 REUSE: the reservoirs, the visibility rays, every store but the listed texels'
        raw(false);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mGiSplitPipe[GIS_REUSE]);
        vkCmdDispatch(cmd, (dw + 7u) / 8u, (dh + 7u) / 8u, 1);
        gpuSubMark(cmd, onAsync ? "c_gi_reuse" : "g_gi_reuse");
        // 4 RESHADE + 3 again as TAIL over the re-shade list: only a frame whose picks can want a
        // re-shade (the temporal reuse + GI_F_LIGHT_CHG); empty lists dispatch no workgroup
        if ((mGiFlags & GI_F_TEMPORAL) != 0u && cfg.lightChg)
        {
            raw(true);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mGiSplitPipe[GIS_RESHADE]);
            vkCmdDispatchIndirect(cmd, mGiHit.buffer, 4u);
            gpuSubMark(cmd, onAsync ? "c_gi_reshade" : "g_gi_reshade");
            raw(true);
            VKRtReflPush tp = rp;
            tp.rtr[2] = 1.f;   // (TAIL: the list's texels, with their re-shade results)
            vkCmdPushConstants(cmd, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(tp), &tp);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mGiSplitPipe[GIS_REUSE]);
            vkCmdDispatchIndirect(cmd, mGiHit.buffer, 4u);
            gpuSubMark(cmd, onAsync ? "c_gi_tail" : "g_gi_tail");
            vkCmdPushConstants(cmd, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(rp), &rp);
        }
    }
    else
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mGiTracePipe);
        vkCmdDispatch(cmd, (dw + 7u) / 8u, (dh + 7u) / 8u, 1);
        gpuSubMark(cmd, onAsync ? "c_gi_trace" : "g_gi_trace");
    }
    if ((mGiFlags & GI_F_CACHE) != 0u && mGiResolvePipe && mGiStateSlots != ~0u && mGiStateSlots != 0u)
    {
        // (GI-4) the radiance cache: this frame's accumulations into the means, eviction, invalidation
        VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        m.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; m.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        m.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        m.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.memoryBarrierCount = 1; d.pMemoryBarriers = &m;
        vkCmdPipelineBarrier2(cmd, &d);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mGiResolvePipe);
        const uint32_t groups = (mGiStateSlots + 63u) / 64u;   // (<= 2^22 / 64 = 65536 -> split in y)
        const uint32_t gx = std::min<uint32_t>(groups, 32768u);
        vkCmdDispatch(cmd, gx, (groups + gx - 1u) / gx, 1);
        gpuSubMark(cmd, onAsync ? "c_gi_cache" : "g_gi_cache");
    }
    // stats: the header's counters into the ring (GPU timers on)
    if (mTsOn)
    {
        VkMemScope memcat(VKMEM_READBACK);   // #407
        constexpr VkDeviceSize kBytes = 5u * 16u;   // stat .. stat4, sph_n (z: the re-shades skipped, #980)
        static_assert(offsetof(GiHeader, stat4) == offsetof(GiHeader, stat) + 3u * 16u, "GI stat words contiguous");
        static_assert(offsetof(GiHeader, sph_n) == offsetof(GiHeader, stat) + 4u * 16u, "GI stat words contiguous");
        const uint32_t slot = mGiStatSlot;
        mGiStatSlot = (mGiStatSlot + 1u) % 3u;
        if (mGiStatUsed[slot] && mGiStatRb.mapped)
        {
            const uint32_t* s = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(mGiStatRb.mapped)
                                                                   + (size_t)slot * kBytes);
            for (int k = 0; k < 20; ++k) mGiStatAcc.s[k] += s[k];
            mGiStatAcc.livePeak = std::max(mGiStatAcc.livePeak, (double)s[12]);
            ++mGiStatAcc.frames;
        }
        mGiStatUsed[slot] = false;
        if (!mGiStatRb.buffer)
            mGiStatRb = mCtx->createBuffer(3u * kBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (mGiStatRb.buffer)
        {
            VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            m.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; m.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            m.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT; m.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
            VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            d.memoryBarrierCount = 1; d.pMemoryBarriers = &m;
            vkCmdPipelineBarrier2(cmd, &d);
            const VkBufferCopy c{offsetof(GiHeader, stat), (VkDeviceSize)slot * kBytes, kBytes};
            vkCmdCopyBuffer(cmd, mGiState.buffer, mGiStatRb.buffer, 1, &c);
            mGiStatUsed[slot] = true;
        }
    }
    // the GI denoiser (its own images; the #85 passes unchanged)
    LLCleanDenoiserVK::Params dp;
    dp.atrous = mGiAtrous;
    dp.maxHistDiffuse = cfg.history;
    dp.clampGamma = cfg.clamp;
    dp.wCut = 0.f;   // (no light-set reference: the clamp is the GI history's anti-lag)
    dp.info = true;   // (an untraced texel without reuse writes a < 0: a weight-0 sample, RenderVKGITraceEvery)
    // (GI_F_LIGHT_CHG) CHANGED LIGHTS: the trace's MOT w = the changed lights' share of the texel's
    // candidates -> the moving-light history cap (clean_dn_temporal.comp kMv): a club light cycling
    // its colour keeps RenderVKGILightChangeHistory frames of GI history there, not RenderVKGIHistory
    // (the per-channel clamp alone kept the old hue for ~30 frames: the noisy GI raw's box is wide)
    dp.moveHist = cfg.lightChg && cfg.chgHist > 0.f ? (U32)std::clamp(cfg.chgHist + 0.5f, 1.f, 15.f) : 0u;
    dp.moveDiffuseOnly = true;   // (layer 1 is the sky visibility: no light in it)
    // (#87) DISOCCLUSION: young histories (< RenderVKGIDisoccFix frames) get the SVGF / ReBLUR spatial
    // boost; a failed bilinear reprojection searches the 3x3 (RenderVKGIReprojSearch) -- GI only, the
    // projector denoiser keeps its rules (clean_dn_common.glsl DN_HFIX_SHIFT / DN_F_REPROJ_SEARCH)
    dp.histFix = cfg.disoccFix;
    dp.reprojSearch = cfg.reprojSearch;
    // RenderVKGIDebug -> the denoiser's view (clean_pdn_common.glsl PDN_DBG_*): 2 / 5 / 6 / 8 / 9 raw (the
    // trace writes the false colour of 5 / 6 / 8 / 9), 3 the history length, 4 the variance; 1 the final image
    static const uint32_t kDnDbg[10] = {0u, 0u, 1u, 2u, 3u, 1u, 1u, 0u, 1u, 1u};
    dp.debug = kDnDbg[std::min<uint32_t>(mGiDebug, 9u)];
    dp.frame = mGiFrame;
    dp.raw = rrRawSignal(kRrRawGi, mGiDn->width(), mGiDn->height());   // #870 raw routing (DLSS RR reconstructs the GI)
    mGiDn->recordTemporal(cmd, mDsetDraw0, p.width, p.height, dp);
    gpuSubMark(cmd, onAsync ? "c_gi_dn_tmp" : "g_gi_dn_tmp");
    mGiDn->recordSpatial(cmd, p.width, p.height, dp);
    gpuSubMark(cmd, onAsync ? "c_gi_dn_sp" : "g_gi_dn_sp");
    // (#978 phase 2) the trace the next frame's combine reads under GI_F_LAG (giDecide: mGiLagStale)
    mGiLagLast.have = true;
    mGiLagLast.finalIn = LLCleanDenoiserVK::finalIsIn(mGiAtrous);
    mGiLagLast.par = mGiDn->parity();
    mGiLagLast.w = dw; mGiLagLast.h = dh;
    mGiLagLast.rw = p.width; mGiLagLast.rh = p.height;
    mGiLagLast.frame = mGiFrame;
    // back to the split layout (incompatible push range: every set re-bound)
    const VkDescriptorSet ssets[4] = {mDsetDraw0, mDsetDraw1, set2, mDsetDeferSplit};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mDeferSplitLayout, 0, 4, ssets, 1, &viewOff);
    vkCmdPushConstants(cmd, mDeferSplitLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
}

std::string LLCleanRendererVK::giStatsText()
{
    GiStatAcc& a = mGiStatAcc;
    if (!a.frames) return std::string();
    const double k = 1.0 / a.frames;
    const double* s = a.s;
    const double rays = s[0];
    std::ostringstream os;
    os << std::fixed << std::setprecision(0) << "GI over " << a.frames << " frames (RenderVKGI): rays/frame "
       << rays * k << ", hits " << std::setprecision(3) << (rays > 0.0 ? s[1] / rays : 0.0) << " (sky "
       << (rays > 0.0 ? s[2] / rays : 0.0) << "), mean hit distance " << std::setprecision(2)
       << (s[1] > 0.0 ? s[3] * 0.1 / s[1] : 0.0) << " m";
    if (s[4] + s[5] + s[6] + s[7] > 0.0)
        os << "; reuse: temporal kept/ray " << std::setprecision(3) << (rays > 0.0 ? s[4] / rays : 0.0)
           << ", spatial kept/ray " << (rays > 0.0 ? s[5] / rays : 0.0) << ", reused samples rejected by visibility/ray "
           << (rays > 0.0 ? s[6] / rays : 0.0) << ", re-shaded for a changed light/ray "
           << (rays > 0.0 ? s[15] / rays : 0.0) << " (skipped, its changed lights insignificant there: "
           << (rays > 0.0 ? s[18] / rays : 0.0) << "/ray, RenderVKGIReshadeThreshold), mean M " << std::setprecision(2)
           << (rays > 0.0 ? s[7] * 0.25 / rays : 0.0);
    if (s[8] + s[12] > 0.0)
        os << "; cache: hit lookups/frame " << std::setprecision(0) << s[8] * k << " found " << std::setprecision(3)
           << (s[8] > 0.0 ? s[9] / s[8] : 0.0) << ", inserts/frame " << std::setprecision(0) << s[10] * k
           << " (table full " << s[11] * k << "), live cells " << s[12] * k << " (peak " << a.livePeak << ") of "
           << (mGiStateSlots == ~0u ? 0u : mGiStateSlots) << ", evicted/frame " << s[13] * k
           << ", invalidated/frame " << s[14] * k << ", restarts (change-sphere overflow) " << mGiCacheRestarts;
    if (a.hdrFrames)
        os << "; changed lights/frame " << std::setprecision(1) << a.chgLights / a.hdrFrames << " of "
           << a.allLights / a.hdrFrames << " (moved or radiance changed against their GI reference: RenderVKGILightChangeCut, RenderVKGILightMoveRel)";
    mGiStatAcc = GiStatAcc{};
    return os.str();
}
