/**
 * @file llcleanprojstochvk.cpp
 * @brief Ray-traced projector shadows, STOCHASTIC + DENOISED (#85 phase 2,
 *        RenderVKProjStochastic): the first producer of the diffuse / specular denoiser
 *        (llcleandenoisevk.h) -- and the test bed of the API ReSTIR DI (#869) will use.
 *
 * EXACT PATH (the default, RenderVKProjStochastic 0): the RT shadow mask traces up to 4
 * ray-traced projectors per mask texel (clean_rt_shadow_mask.comp; a 5th+ overlapping one is
 * lit unshadowed), its denoiser filters each slot, and the deferred light walk evaluates every
 * projector at every pixel -- gobo fetch, BRDF, the slot lookup (c_rtmask*, c_defer_local).
 *
 * STOCHASTIC PATH (1; split deferred frames with ray-traced projector records and a valid TLAS):
 *   1. the RT mask leaves the projector slots empty (rtMaskPushFill: negative clamp width) --
 *      its trace keeps the sun only;
 *   2. PASS 15 (clean_deferred_light.comp CLEAN_DEFER_PASS 15, mRtReflLayout), after the
 *      reflect passes: per denoiser texel (RenderVKDenoiseScale x the render extent) RIS over the
 *      pixel cluster's ray-traced projectors with a gobo-free target, K = RenderVKProjStochastic
 *      Samples picks, the full terms + ONE shadow ray each (a soft disc sampled once), weighted
 *      W / (K p) -- unbiased shadowed direct light of EVERY projector (no 4-slot cap); exact
 *      (no noise) where a pixel has <= K candidates. Written demodulated with the guides;
 *   3. the DENOISER: temporal (c_dn_tmp) + RenderVKDenoiseAtrous a-trous iterations (c_dn_sp);
 *   4. the combine / LOCAL (DEFER_FLAG_PSTOCH): RATIO ESTIMATOR (Heitz et al. 2018) -- pass 15
 *      writes the per-pixel VISIBILITY RATIO (shadowed / unshadowed estimate of the same picks,
 *      diffuse and specular, rgb), the denoiser filters that, and localLight evaluates every RT
 *      projector's light exactly and fresh (colour, gobo, motion: nothing accumulated) times the
 *      denoised ratio instead of the mask's slot lookup. A light changing colour / intensity /
 *      position updates at once; only the shadow lags (cut on a W change). The cost moved: the
 *      mask's projector traces and slot lookups are gone, the per-pixel light evaluation stays.
 *   5. (#869) THE SPLIT (RenderVKProjStochasticSplitK, default 2, live; clean_pdn_common.glsl SPLIT
 *      CHANNEL): ONE shared ratio leaked -- a club projector behind a sealed room's wall took the
 *      room lamps' ratio (its gobo on the room's wall, sharp, dimmer) and darkened the lamps. Now
 *      the K strongest lights per texel (gobo-free target, the pixel cluster's list, hysteresis 1.5,
 *      ties by id) get their own ray and their own denoised visibility, keyed by a persistent
 *      per-light split id (Light.tex.w, setLights -> splitIdsFrame); the shared ratio covers the
 *      rest only. Generic core only (forced while K > 0; the per-light-type kernel writes empty
 *      slots). Cost: <= K rays more per lit texel; +64 B per denoiser texel (+80 B without the sun
 *      channel): 1280x720 full ~56 MB, 4K half ~127 MB, 4K full ~506 MB; no extra dispatch (the
 *      temporal / a-trous passes carry the channel). Debug: RenderVKDenoiseDebug 8 / 9. DLSS RR raw
 *      routing (kRrRawProj): the split lights' visibility goes raw with the rest's ratio.
 * Not covered (they keep their own rules, in either mode): water and probe / mirror captures
 * (RT projectors unshadowed), particles (their own per-particle / inline rule), alpha-blend
 * surfaces (RenderVKRTBlendProjShadows: unshadowed by default, 1 = one inline ray per projector
 * per fragment -- before that setting they ALWAYS traced inline, this comment said otherwise),
 * the uber kernel (debug twins, split refused), cel-shaded surfaces.
 * MERGED RT SHADOWS (#195 + #85, RenderVKRTShadowMerge, default on): when the sun / moon is ray
 * traced too, pass 15's merge variant (mPdnMergePipe) traces each texel's sun / moon visibility
 * with its OWN ray(s) (never in the projector pick: its intensity would starve the projectors)
 * beside the projector pick -- the same G-buffer reads, receiver and normal -- into the denoiser's
 * sun channel (llcleandenoisevk.h), and runs BEFORE the SHADOW kernel, which reads the denoised
 * sun instead of the RT shadow mask (clean_deferred_light.comp deferMergedSunT). The mask does not
 * run that frame (c_rtmask* gone; c_defer_shadowmerge instead of c_defer_pstoch) and the lighting
 * stays deferred without it. Only then: every other combination (sun mapped / off, projectors
 * mapped / off, the stochastic path off, RT off, the setting 0) runs exactly the previous path --
 * see shadowPathLog. A WEAK sun / moon (RenderVKRTShadowMergeSunMin: the light colour's luminance
 * x the sun scale against a threshold, a smooth ramp over [t, 2t]) fades its shadow toward lit and
 * below t traces no sun ray at all (night, a dim moon).
 * VRAM: the denoiser's images only (llcleandenoisevk.h), held kRtReflHoldSecs after the last
 * wanting frame, freed under texture pressure; refusals step full -> half extent -> exact path
 * (the sun channel first: a refusal of it keeps the projector-only images and today's paths).
 * GPU: c_defer_pstoch (pass 15), c_dn_tmp, c_dn_sp (g_ prefixes when the lighting is on the
 * graphics queue); compare with c_rtmask / c_rtmask_tmp / c_rtmask_sp / c_defer_local.
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
#include <iomanip>
#include <sstream>
#include <unordered_set>

namespace
{
VkShaderModule pdnModule(VkDevice dev, const uint32_t* code, size_t bytes, const char* name)
{
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(dev, &ci, nullptr, &m));
    vkNoteShaderModule(m, code, bytes, name);
    return m;
}
// The lighting kernels' debug specialization OFF (llcleanrenderervk.cpp litDebugSpec(false):
// clean_draw_common.glsl kDebugViews, constant_id 1). Static: a queued pipeline keeps the pointer.
const VkSpecializationInfo* pdnLitSpec()
{
    static const VkBool32 kOff = VK_FALSE;
    static const VkSpecializationMapEntry kEntry{1u, 0u, sizeof(VkBool32)};
    static const VkSpecializationInfo kSiOff{1u, &kEntry, sizeof(VkBool32), &kOff};
    return &kSiOff;
}
double pdnClock()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
uint32_t pdnDim(uint32_t r, float s) { return std::max(1u, (uint32_t)std::ceil((float)r * s)); }
} // namespace

// Init (initDeferSplit, inside the world PipelineBatchScope): the denoiser always -- set 3
// bindings 13-16 are declared by the combine / LOCAL kernels and must name its placeholders --
// and pass 15 when the RT layout exists.
void LLCleanRendererVK::pdnInit(VkDevice dev)
{
    if (!mPdn)
    {
        mPdn = new LLCleanDenoiserVK();
        if (!mPdn->init(mCtx, mDslDraw0, mDepthSampler, "Projector denoiser"))
        {
            LL_WARNS("Vulkan") << "Projector denoiser: init failed; split lighting unavailable" << LL_ENDL;
            mPdn->destroy();
            delete mPdn;
            mPdn = nullptr;
            return;
        }
        writePdnMotion();
    }
    if (mRtReflLayout && !mPdnSamplePipe)
    {
        VkShaderModule m = pdnModule(dev, kWorldDeferProjStochSpv, kWorldDeferProjStochSpv_bytes, "DeferProjStoch");
        createComputePipelineInto(dev, m, mRtReflLayout, 0u, pdnLitSpec(), &mPdnSamplePipe,
                                  "mPdnSamplePipe (DeferProjStoch)");
        destroyShaderModuleDeferred(dev, m, nullptr);
    }
    if (mRtReflLayout && !mPdnMergePipe)
    {
        // (#195 + #85) the merged RT shadow pass: pass 15 + the sun / moon trace (CLEAN_SHADOW_MERGE)
        VkShaderModule m = pdnModule(dev, kWorldDeferShadowMergeSpv, kWorldDeferShadowMergeSpv_bytes, "DeferShadowMerge");
        createComputePipelineInto(dev, m, mRtReflLayout, 0u, pdnLitSpec(), &mPdnMergePipe,
                                  "mPdnMergePipe (DeferShadowMerge)");
        destroyShaderModuleDeferred(dev, m, nullptr);
    }
    // #85 / #869 Phase 1: the generic-core twins (RenderVKShadowGenericCore; both compiled so the
    // setting stays live -- recordProjStoch falls back to the pipes above until they land)
    if (mRtReflLayout && !mPdnSampleCorePipe)
    {
        VkShaderModule m = pdnModule(dev, kWorldDeferProjStochCoreSpv, kWorldDeferProjStochCoreSpv_bytes, "DeferProjStochCore");
        createComputePipelineInto(dev, m, mRtReflLayout, 0u, pdnLitSpec(), &mPdnSampleCorePipe,
                                  "mPdnSampleCorePipe (DeferProjStochCore)");
        destroyShaderModuleDeferred(dev, m, nullptr);
    }
    if (mRtReflLayout && !mRsEmPipe)
    {
        // (#91 / #869) the emissive light records (RenderVKReSTIREmissive): pass 15's layout
        VkShaderModule m = pdnModule(dev, kWorldDeferRsEmissiveSpv, kWorldDeferRsEmissiveSpv_bytes, "DeferRsEmissive");
        createComputePipelineInto(dev, m, mRtReflLayout, 0u, pdnLitSpec(), &mRsEmPipe, "mRsEmPipe (DeferRsEmissive)");
        destroyShaderModuleDeferred(dev, m, nullptr);
    }
    if (mRtReflLayout && !mPdnMergeCorePipe)
    {
        VkShaderModule m = pdnModule(dev, kWorldDeferShadowMergeCoreSpv, kWorldDeferShadowMergeCoreSpv_bytes, "DeferShadowMergeCore");
        createComputePipelineInto(dev, m, mRtReflLayout, 0u, pdnLitSpec(), &mPdnMergeCorePipe,
                                  "mPdnMergeCorePipe (DeferShadowMergeCore)");
        destroyShaderModuleDeferred(dev, m, nullptr);
    }
    if (mRtReflLayout)
    {
        // (#1031) the generic core split: SELECT (plain, merged), SUNVIS (merged), VIS + SHADE (both passes)
        // (distinct names: RenderVKPipelineStats)
        struct Stage { const uint32_t* code; size_t bytes; const char* mod; const char* name; VkPipeline* pipe; };
        const Stage st[7] = {
            {kWorldDeferShadowMergeSunVisIsoSpv, kWorldDeferShadowMergeSunVisIsoSpv_bytes, "DeferShadowMergeSunVisIso",
             "mPdnSunVisIsoPipe (DeferShadowMergeSunVisIso)", &mPdnSunVisIsoPipe},
            {kWorldDeferShadowSplitVisIsoSpv, kWorldDeferShadowSplitVisIsoSpv_bytes, "DeferShadowSplitVisIso",
             "mPdnVisIsoPipe (DeferShadowSplitVisIso)", &mPdnVisIsoPipe},
            {kWorldDeferProjStochSelectSpv, kWorldDeferProjStochSelectSpv_bytes, "DeferProjStochSelect",
             "mPdnSelectPipe (DeferProjStochSelect)", &mPdnSelectPipe[0]},
            {kWorldDeferShadowMergeSelectSpv, kWorldDeferShadowMergeSelectSpv_bytes, "DeferShadowMergeSelect",
             "mPdnSelectPipe (DeferShadowMergeSelect)", &mPdnSelectPipe[1]},
            {kWorldDeferShadowMergeSunVisSpv, kWorldDeferShadowMergeSunVisSpv_bytes, "DeferShadowMergeSunVis",
             "mPdnSunVisPipe (DeferShadowMergeSunVis)", &mPdnSunVisPipe},
            {kWorldDeferShadowSplitVisSpv, kWorldDeferShadowSplitVisSpv_bytes, "DeferShadowSplitVis",
             "mPdnVisPipe (DeferShadowSplitVis)", &mPdnVisPipe},
            {kWorldDeferShadowSplitShadeSpv, kWorldDeferShadowSplitShadeSpv_bytes, "DeferShadowSplitShade",
             "mPdnShadePipe (DeferShadowSplitShade)", &mPdnShadePipe}};
        for (const Stage& sg : st)
        {
            if (*sg.pipe) continue;
            VkShaderModule m = pdnModule(dev, sg.code, sg.bytes, sg.mod);
            createComputePipelineInto(dev, m, mRtReflLayout, 0u, pdnLitSpec(), sg.pipe, sg.name);
            destroyShaderModuleDeferred(dev, m, nullptr);
        }
    }
    if (mRtReflLayout) sunCertInit(dev);   // (#771 Phase 2) the sun exposure certificates' maintenance kernel
    giInit(dev);   // (#87) the GI denoiser (set 3 bindings 18-23) + the GI kernels
}

void LLCleanRendererVK::pdnDestroy()
{
    if (!mCtx) return;
    if (mPdnSamplePipe) { vkDestroyPipeline(mCtx->device(), mPdnSamplePipe, nullptr); mPdnSamplePipe = VK_NULL_HANDLE; }
    if (mPdnMergePipe) { vkDestroyPipeline(mCtx->device(), mPdnMergePipe, nullptr); mPdnMergePipe = VK_NULL_HANDLE; }
    if (mPdnSampleCorePipe) { vkDestroyPipeline(mCtx->device(), mPdnSampleCorePipe, nullptr); mPdnSampleCorePipe = VK_NULL_HANDLE; }
    if (mPdnMergeCorePipe) { vkDestroyPipeline(mCtx->device(), mPdnMergeCorePipe, nullptr); mPdnMergeCorePipe = VK_NULL_HANDLE; }
    for (VkPipeline* sp : {&mPdnSelectPipe[0], &mPdnSelectPipe[1], &mPdnSunVisPipe, &mPdnVisPipe, &mPdnShadePipe,
                           &mPdnSunVisIsoPipe, &mPdnVisIsoPipe})   // (#1031)
        if (*sp) { vkDestroyPipeline(mCtx->device(), *sp, nullptr); *sp = VK_NULL_HANDLE; }
    if (mPdnSel.buffer) { mCtx->destroyBuffer(mPdnSel); mPdnSel = GpuBuffer{}; }
    if (mPdnVis.buffer) { mCtx->destroyBuffer(mPdnVis); mPdnVis = GpuBuffer{}; }
    mPdnSelW = mPdnSelH = mPdnVisSlots = 0;
    sunCertDestroy();
    giDestroy();   // (#87)
    if (mRsEmPipe) { vkDestroyPipeline(mCtx->device(), mRsEmPipe, nullptr); mRsEmPipe = VK_NULL_HANDLE; }
    mShadowMergeTry = mShadowMergeNow = false;
    if (mPdn)
    {
        mPdn->destroy();
        delete mPdn;
        mPdn = nullptr;
    }
    mPdnNow = false;
    mPdnFlags = 0u;
    mPdnReq = PdnReq{};
    mPdnStressPhase = -1;
    mRsResSampled = false;
}

// Set 3 bindings 13-16: the denoiser's IN / GEOM / MOT / OUT (its extent or its placeholders).
// (#869 Phase 5b: + binding 17, the reservoirs -- freed with the denoiser's images.)
void LLCleanRendererVK::writePdnBindings()
{
    if (!mDsetDeferSplit || !mPdn || !mCtx) return;
    if (!mPdn->allocated() && mRsRes.buffer)
    {   // (the callers waited for the device: the images were just released)
        mCtx->destroyBuffer(mRsRes);
        mRsRes = GpuBuffer{};
        mRsResW = mRsResH = 0;
        mRsResPrevOk = false;
        writeRsResBinding();
    }
    if (!mPdn->allocated() && (mPdnSel.buffer || mPdnVis.buffer))
        pdnSelFree();   // (#1031: the split core's descriptors + visibilities go with them)
    const VkImageView v[4] = {mPdn->inView(), mPdn->geomView(), mPdn->motView(), mPdn->outView()};
    VkDescriptorImageInfo ii[4]{};
    VkWriteDescriptorSet w[4]{};
    for (uint32_t i = 0; i < 4; ++i)
    {
        ii[i] = {VK_NULL_HANDLE, v[i], VK_IMAGE_LAYOUT_GENERAL};
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet = mDsetDeferSplit;
        w[i].dstBinding = kDsBindingPdnIn + i; w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[i].pImageInfo = &ii[i];
    }
    vkUpdateDescriptorSets(mCtx->device(), 4, w, 0, nullptr);
    writeGiBindings();   // (#87: 18-23 -- and their fallback views follow this denoiser's)
}

// The motion vectors for the denoiser's reprojection (rides on writeRtMaskMotionDescriptor:
// same callers, same "no frame in flight" guarantee).
void LLCleanRendererVK::writePdnMotion()
{
    if (mPdn)
    {
        if (mMotion.view) mPdn->setMotion(mMotion.view, mDepthSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        else              mPdn->setMotion(VK_NULL_HANDLE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL);
    }
    writeGiMotion();   // (#87: the GI denoiser reprojects with the same motion vectors; #1038 + the GI trace)
}

// Once per split frame (deferSplitFrame, after the split targets): the settings, the denoiser's
// images (held / released / stepped down), and whether this frame samples (mPdnNow + the
// DEFER_FLAG_PSTOCH bits the caller ORs into the split flags). sample = false (a frame without the
// split that the merged RT shadow pass wants): the images only, mPdnNow stays false.
void LLCleanRendererVK::pdnFrame(uint32_t rw, uint32_t rh, bool sample)
{
    mPdnNow = false;
    mPdnFlags = 0u;
    U32 mode = 0, k = 1, dbg = 0, atrous = 3, splitK = 2;
    F32 scale = 0.5f;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<U32> s_mode(gSavedSettings, "RenderVKProjStochastic", 1U);
    static LLCachedControl<U32> s_k(gSavedSettings, "RenderVKProjStochasticSamples", 1U);
    static LLCachedControl<F32> s_scale(gSavedSettings, "RenderVKDenoiseScale", 0.5f);
    static LLCachedControl<U32> s_dbg(gSavedSettings, "RenderVKDenoiseDebug", 0U);
    static LLCachedControl<U32> s_atrous(gSavedSettings, "RenderVKDenoiseAtrous", 3U);
    static LLCachedControl<U32> s_split(gSavedSettings, "RenderVKProjStochasticSplitK", 2U);
    mode = s_mode; k = s_k; scale = s_scale; dbg = s_dbg; atrous = s_atrous; splitK = s_split;
#endif
    scale = rtSignalScale(scale);   // (RenderVKDlssFullResSignalsMode)
    mPdnWant = mode != 0u;
    mPdnK = std::clamp<U32>(k, 1u, 2u);
    // (7: the ReSTIR view, #869; 8 / 9: the split lights by id / x their visibility -- the denoiser
    //  writes those into layer 0, the lit pass shows layer 0 as view 4)
    mPdnDebug = dbg <= 9u ? dbg : 0u;
    // (#869) THE SPLIT (RenderVKProjStochasticSplitK, live, 0..4): the K dominant lights' own
    // visibility (clean_pdn_common.glsl SPLIT CHANNEL); its images only while K > 0
    mPdnSplitK = std::min<U32>(splitK, 4u);
    const bool split = mPdnSplitK > 0u;
    mPdnAtrous = LLCleanDenoiserVK::clampIters(atrous);
    if (!mPdn) return;
    // This frame can sample: the setting, the kernels, ray-traced projector records, a valid TLAS.
    // (#869 / #439 ReSTIR DI: a scene whose only shadowed lights are point lights / record-less
    //  projectors samples too -- once this recording's light pass built the ReSTIR population,
    //  mRsNow: without it pass 15 would have nothing to trace)
    const bool can = sample && mPdnWant && mPdnSamplePipe && mRtReflLayout && mPdn->pipelinesReady() && mRT
                  && mRT->tlasValid() && mRT->consumerSet() != VK_NULL_HANDLE && mProjMode == 2u
                  && (mProjRecords > 0u || mRsNow) && mDsetDraw0;
    const double now = pdnClock();
    if (can) mPdnLastWant = now;
    // (#195 + #85) THE SUN / MOON CHANNEL: while the merged RT shadow pass wants it (+ the same
    // hold); a refusal of it backs off 30 s with the projector-only images (today's paths).
    const bool sunWant = shadowMergeWanted(false) && now >= mPdnSunRetryAt;
    if (sunWant) mPdnSunLastWant = mPdnLastWant = now;
    bool sun = sunWant || (mPdn->hasSun() && !mTexPressure && now - mPdnSunLastWant < kRtReflHoldSecs
                           && now >= mPdnSunRetryAt);
    // Held past the last sampling frame (projectors drifting out of view and back must not
    // re-allocate every time); freed at once under texture pressure or with the setting off.
    const bool keep = can || sunWant || (mPdnWant && !mTexPressure && now - mPdnLastWant < kRtReflHoldSecs);
    if (!keep)
    {
        mPdnStressPhase = -1;
        if (mPdn->allocated()) pdnRequestRelease();   // (applied before the next recording: pdnPrestep)
        return;
    }
    // REALLOCATION STRESS (RenderVKDenoiseReallocStress, seconds; 0 = off): the sun channel is
    // flipped every N seconds while the images are held, so the denoiser re-allocates on demand
    // through exactly the request path below (pdnPrestep) -- the Beta 5 Windows DEVICE_LOST
    // trigger, reproducible for a soak test (Vulkan validation on Linux; Windows NVIDIA).
    {
        F32 stress = 0.f;
#if defined(LL_HAS_CLEAN_PIPELINE)
        static LLCachedControl<F32> s_stress(gSavedSettings, "RenderVKDenoiseReallocStress", 0.f);
        stress = s_stress;
#endif
        if (stress > 0.f && std::isfinite(stress))
        {
            const int64_t phase = (int64_t)(now / std::max(stress, 0.25f));
            const bool flip = (phase & 1) != 0;
            sun = sun != flip;
            if (phase != mPdnStressPhase)
            {
                if (mPdnStressPhase >= 0)
                    LL_INFOS("Vulkan") << "Projector denoiser STRESS (RenderVKDenoiseReallocStress " << stress
                                       << " s): forcing the sun / moon channel " << (sun ? "ON" : "OFF")
                                       << " -> re-allocation " << (resizeAtFrameStart() ? "before the next recording"
                                                                                      : "mid-recording (old path)")
                                       << LL_ENDL;
                mPdnStressPhase = phase;
            }
        }
        else
            mPdnStressPhase = -1;
    }
    const float want_sc = scale >= 0.75f ? 1.f : 0.5f;   // (full or half extent)
    const uint32_t w0 = pdnDim(rw, want_sc), h0 = pdnDim(rh, want_sc);
    if (!mPdn->allocated() || mPdn->width() != w0 || mPdn->height() != h0 || mPdn->hasSun() != sun
        || mPdn->hasSplit() != split)
    {
        // Already at a stepped-down size for this request: keep it until the retry time.
        const bool stepped = mPdn->allocated() && mPdn->hasSun() == sun && mPdn->hasSplit() == split && want_sc > 0.5f
                          && mPdn->width() == pdnDim(rw, 0.5f) && mPdn->height() == pdnDim(rh, 0.5f);
        if (now < mPdnRetryAt && !stepped) return;   // (refused recently: the exact path meanwhile)
        if (now >= mPdnRetryAt)
        {
            // VRAM PRE-CHECK (#226 / #697): the requested extent, else half, else none (the exact path).
            // With the sun channel wanted: those two with it first, then without it (the merge
            // waits; the projector path keeps its images).
            struct Opt { float s; bool sun; };
            std::vector<Opt> opts;
            std::vector<VramCand> cands;
            for (int pass = 0; pass < (sun ? 2 : 1); ++pass)
            {
                const bool osun = sun && pass == 0;
                float last = 2.f;
                for (float s : {want_sc, 0.5f})
                {
                    if (s >= last) continue;
                    last = s;
                    opts.push_back(Opt{s, osun});
                    cands.push_back(VramCand{std::string(s > 0.75f ? "full" : "half") + (osun ? " + sun" : "") + " ("
                                                 + std::to_string(pdnDim(rw, s)) + "x" + std::to_string(pdnDim(rh, s)) + ")",
                                             mPdn->bytesAt(pdnDim(rw, s), pdnDim(rh, s), osun, split)});
                }
            }
            // The current allocation (a stepped-down half extent) needs nothing new: vramPick's keep
            // index, so a re-allocation it defers or refuses never drops a working set.
            int keep = -1;
            for (size_t i = 0; i < opts.size(); ++i)
                if (mPdn->allocated() && mPdn->width() == pdnDim(rw, opts[i].s) && mPdn->height() == pdnDim(rh, opts[i].s)
                    && mPdn->hasSun() == opts[i].sun && mPdn->hasSplit() == split)
                    keep = (int)i;
            VramRoom room;
            const int pick = vramPick("projector denoiser", cands, mPdn->bytes(), keep, &room);
            if (pick < 0)
            {
                mPdnRetryAt = now + 30.0;
                if (sun) mPdnSunRetryAt = now + 30.0;
                if (mPdn->allocated()) pdnRequestRelease();
                return;
            }
            const Opt o = opts[(size_t)pick];
            const uint32_t w = pdnDim(rw, o.s), h = pdnDim(rh, o.s);
            if (sun && !o.sun) mPdnSunRetryAt = now + 30.0;   // (the sun channel did not fit: later)
            if (o.s < want_sc) mPdnRetryAt = now + 30.0;   // (re-try the requested extent later)
            if (!mPdn->allocated() || mPdn->width() != w || mPdn->height() != h || mPdn->hasSun() != o.sun
                || mPdn->hasSplit() != split)
            {
                // THE DEVICE_LOST FIX (Beta 5.1): never re-create the images here. This runs inside
                // the frame's recording (deferSplitFrame, after the front end / culls / RT build and
                // the graphics work before pass 1 are recorded); ensure() waits for the device,
                // destroys the images, re-creates them and rewrites the denoiser's set + set 3 b13-16
                // -- vkDeviceWaitIdle does not cover the recorded-but-unsubmitted commands of this
                // frame, and none of these sets is UPDATE_AFTER_BIND. The request is applied by
                // pdnPrestep before the next frame's command buffer begins (#521's rule).
                pdnRequestEnsure(w, h, o.sun, sun, split);
                if (resizeAtFrameStart())
                {
                    // This frame: the current images while they fit the extent (only the sun / split
                    // channel differs: the projector path does not read the sun's, the merge is gated
                    // on hasSun(), and pass 15 splits only into images that carry the split layers --
                    // it writes empty slots into images that carry them while K = 0); a different
                    // extent -- or none yet -- takes the exact path this frame.
                    if (!mPdn->allocated() || mPdn->width() != w || mPdn->height() != h) return;
                }
                else
                {
                    // RenderVKResizeAtFrameStart off: the old mid-recording re-allocation (A/B only).
                    pdnPrestep();
                    if (!mPdn->allocated()) return;
                }
            }
        }
    }
    if (!can || !mPdn->allocated())   // (held, not sampled this frame)
    {
        mRsResPrevOk = false;   // (#869: the reservoirs' history is a frame old at most)
        return;
    }
    mPdnNow = true;
    mPdn->beginFrame();
    ++mPdnFrame;
    // (#869 Phase 5b: the reservoirs at this extent -- re-created before the next recording, like
    // the denoiser's images (syncFromStoresPrestep: rsResFrame writes set 3 b17 behind a device
    // wait); recordProjStoch uses them only once they match this extent)
    mRsResSampled = true;
    if (!resizeAtFrameStart()) { rsResFrame(); pdnSelFrame(); }
    // clean_deferred_light.comp DEFER_FLAG_PSTOCH (18) / _HALF (19) / _IN (20) / debug (21-23).
    const bool half = mPdn->width() < rw || mPdn->height() < rh;
    // (the lit pass's debug field is 3 bits: the #869 split views 8 / 9 show the denoised layer 0 --
    //  view 4 -- into which the denoiser's last a-trous iteration writes them)
    const uint32_t litDbg = mPdnDebug >= 8u ? 4u : mPdnDebug;
    mPdnFlags = (1u << 18) | (half ? (1u << 19) : 0u) | (LLCleanDenoiserVK::finalIsIn(mPdnAtrous) ? (1u << 20) : 0u)
              | (litDbg << 21);
}

// Frames without the split (uber / forward / split refused): nothing samples; the images are
// released once the hold ran out (or at once under texture pressure).
void LLCleanRendererVK::pdnIdle()
{
    if (!mPdn || !mPdn->allocated()) return;
    if (!mTexPressure && pdnClock() - mPdnLastWant < kRtReflHoldSecs) return;
    mPdnStressPhase = -1;
    pdnRequestRelease();   // (applied before the next recording: pdnPrestep)
}

// ---- DEFERRED (RE)ALLOCATION (the Beta 5 Windows NVIDIA DEVICE_LOST, 2026-10-22) ----
// pdnFrame / pdnIdle run INSIDE the frame's recording; they only record what the images should
// be. pdnPrestep (syncFromStoresPrestep, before the frame's command buffer begins -- the place
// resizeProbesIfNeeded moved to for #521) applies it: the device wait there covers every
// submitted frame and nothing of the next one is recorded yet, so destroying the images and
// rewriting the denoiser's set + set 3 b13-16 cannot touch a command buffer that names them.
bool LLCleanRendererVK::resizeAtFrameStart() const
{
    bool on = true;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<bool> s_on(gSavedSettings, "RenderVKResizeAtFrameStart", true);
    on = s_on;
#endif
    return on;
}

void LLCleanRendererVK::pdnRequestEnsure(uint32_t w, uint32_t h, bool sun, bool sunWanted, bool split)
{
    mPdnReq.pending = true;
    mPdnReq.release = false;
    mPdnReq.w = w;
    mPdnReq.h = h;
    mPdnReq.sun = sun;
    mPdnReq.sunWanted = sunWanted;
    mPdnReq.split = split;
}

void LLCleanRendererVK::pdnRequestRelease()
{
    mPdnReq = PdnReq{};
    mPdnReq.pending = true;
    mPdnReq.release = true;
    if (!resizeAtFrameStart()) pdnPrestep();   // (RenderVKResizeAtFrameStart off: the old path, A/B only)
}

void LLCleanRendererVK::pdnPrestep()
{
    if (!mPdnReq.pending) return;
    const PdnReq r = mPdnReq;
    mPdnReq = PdnReq{};
    if (!mPdn || !mCtx) return;
    if (r.release)
    {
        if (!mPdn->allocated()) return;
        mPdn->release();   // (waits for the device)
        writePdnBindings();
        return;
    }
    if (mPdn->allocated() && mPdn->width() == r.w && mPdn->height() == r.h && mPdn->hasSun() == r.sun
        && mPdn->hasSplit() == r.split)
        return;
    const double now = pdnClock();
    bool ok = mPdn->ensure(r.w, r.h, r.sun, r.split);   // (soft; waits for the device)
    if (!ok && r.sun)
    {
        // the sun channel refused after the pre-check: the projector-only images now, the sun
        // channel later (today's paths meanwhile)
        mPdnSunRetryAt = now + 30.0;
        ok = mPdn->ensure(r.w, r.h, false, r.split);
    }
    writePdnBindings();
    if (!ok)
    {
        mPdnRetryAt = now + 30.0;
        if (r.sunWanted) mPdnSunRetryAt = now + 30.0;
    }
}

// ======================= THE LIGHT PREP =======================
// The lighting's head that reads nothing the lighting's other inputs are made of: the SUN EXPOSURE CERTIFICATES
// (C: the CPU's sun state + its own table, a push-only kernel), the ReSTIR EMISSIVE BUILD (E: the instance store,
// the RT geometry rows of this frame's RT build, the materials / bindless textures, the light table and the grid
// header of the front end -- no G-buffer, no depth, no screen-space data; the draw view only for its time), the
// OCCLUSION RESOLVE (O: last frame's notes + this frame's light remap in the grid header) and (#1039) the GI
// CHANGE TAGS' bits 0-2 (stage 9: last frame's reservoirs + this frame's GI header and light table). recordWorld
// plans + records it BEFORE pass 1a: its own async segment q_light_prep (after the RT build on the async queue,
// beside the pre-pass / G-buffer / shadow views on graphics), or inline on graphics there. The lighting chain
// later on the same queue reads it through the closing barrier below (queue order), a graphics lighting through
// its join (asyncJoinUpTo before recordDeferLight). Pass 15, the lit kernels and the GI read the results;
// recordProjStoch reads the plan.
bool LLCleanRendererVK::lightPrepPlan(uint32_t rw, uint32_t rh)
{
    LightPrep& lp = mLightPrep;
    lp = LightPrep{};
    lp.planned = true;
    lightPrepPlanShadow();
    // the GI split this frame (recordGI's own test; giTraceReady: this frame's TLAS)
    const bool giSplit = mGiSplitNow && giTraceReady() && giSplitReady(mGiDn->width(), mGiDn->height());
    // (#1039) the GI CHANGE TAGS: on the frames whose header has changed lights + reservoirs (mGiHdrChg) and whose
    // GI runs the split (REUSE reads the tags)
    lp.chg = giSplit && mGiHdrChg && mGiSplitPipe[GIS_CHGTAG];
    // (#1036) the GI shade split's HIT / MISS list headers (recordGI's shadeSplit) and (#827) the feature split's
    // tile list header (recordDeferredLighting's feat)
    lp.giLists = giSplit && mGiShadeSplitNow && giShadeSplitReady();
    lp.dsTiles = deferFeatFrame(rw, rh);
    return lp.sxc || lp.em || lp.occ || lp.split || lp.chg || lp.giLists || lp.dsTiles;
}

void LLCleanRendererVK::lightPrepPlanShadow()
{
    LightPrep& lp = mLightPrep;
    lp.pdn = lp.merged = lp.sunSkip = lp.split = lp.sxc = lp.em = lp.occ = false;
    lp.pipe = VK_NULL_HANDLE;
    if (!mPdnNow || !mPdn || !mPdnSamplePipe || !mRtReflLayout || !mRT || mRT->consumerSet() == VK_NULL_HANDLE)
        return;
    lp.pdn = true;
    U32 core = 0;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<U32> s_core(gSavedSettings, "RenderVKShadowGenericCore", 1U);
    core = s_core;
#endif
    // (#195 + #85) merged RT shadows: the same pass + the sun / moon trace (recordWorld's settle
    // checked the pipeline and the denoiser's sun images).
    lp.merged = mShadowMergeNow && mPdnMergePipe && mPdn->hasSun();
    // SUN-CHANNEL SKIP: the weak-sun gate traces no sun / moon ray this frame (weight 0: every
    // texel's raw sun is exactly lit), so the denoiser skips the sun channel's temporal + a-trous
    // work and the SHADOW kernel reads lit without it (frame info rt_mask.x = -2, rtMaskFrameInfo).
    // Its history is not written: the next traced frame restarts it (DN_F_SUN_HIST off) while
    // the weight ramps in from 0 (the shadow fades in with the light, never pops).
    lp.sunSkip = lp.merged && !(mShadowMergeSunW > 0.f);
    const uint32_t dw = mPdn->width(), dh = mPdn->height();
    const uint32_t splitK = mPdn->hasSplit() ? mPdnSplitK : 0u;
    lp.pipe = lp.merged ? mPdnMergePipe : mPdnSamplePipe;
    // (#869 Phase 5) a ReSTIR frame runs the generic core (its candidates, reservoirs and light
    // samples live there; rsWanted made sure both core kernels exist)
    // (#869 SPLIT: only the generic core splits -- the per-light-type kernel writes empty slots)
    // (#1031) ... as SELECT -> SUNVIS + VIS -> SHADE (RenderVKShadowSplitKernels) while the descriptors and
    // the visibility slots fit this extent and the settings' longest sample list
    // (one decision per frame: the certificates below and pass 15 take the same kernel even if a core
    //  pipeline finishes compiling in between)
    if (core != 0u || mRsNow || splitK != 0u)
    {
        const VkPipeline cp = lp.merged ? mPdnMergeCorePipe : mPdnSampleCorePipe;
        if (cp != VK_NULL_HANDLE) lp.pipe = cp;   // (still compiling: the per-light-type kernel this frame)
        lp.split = pdnSplitWanted() && mPdnSel.buffer && mPdnVis.buffer && mPdnSelW == dw && mPdnSelH == dh
                && mPdnVisSlots >= pdnVisSlotsWanted();
    }
    lp.sxc = lp.merged && !lp.sunSkip;
    lp.em = mRsEmNow && mRsNow && mRsEmPipe && mRegir.buffer;
    lp.occ = mRsOccNow && mRsNow && mRegir.buffer;
}

// The planned work not recorded yet, on cmd (once per frame). viewIdx / viewOff: the main view's draw-view entry
// (the emissive build reads its time: texture animation).
void LLCleanRendererVK::recordLightPrep(VkCommandBuffer cmd, uint32_t viewIdx, uint32_t viewOff, bool onAsync, bool early)
{
    LightPrep& lp = mLightPrep;
    const bool shadow = lp.pdn && !lp.shadowDone && (lp.sxc || lp.em || lp.occ || lp.split);
    const bool chg = lp.chg && !lp.chgDone;
    const bool hdrs = early && !lp.headersDone && (lp.dsTiles || lp.giLists);
    if (!shadow && !chg && !hdrs) return;
    lp.shadowDone = lp.shadowDone || shadow;
    lp.chgDone = lp.chgDone || chg;
    lp.headersDone = lp.headersDone || hdrs;
    const bool sxc = shadow && lp.sxc, em = shadow && lp.em, occ = shadow && lp.occ;
    auto memBarrier = [cmd](VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da)
    {
        VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        b.srcStageMask = ss; b.srcAccessMask = sa; b.dstStageMask = ds; b.dstAccessMask = da;
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.memoryBarrierCount = 1; di.pMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &di);
    };
    constexpr VkAccessFlags2 kRW = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    // OPENING BARRIER (one for C, E, O and the tags -- each had its own WAR barrier): RAW from this frame's RT build
    // (the geometry rows E reads; on the async queue before this segment, or on graphics before the fork: the
    // semaphore) and the front end's header uploads, and WAR against every earlier reader of their regions on this
    // queue (last frame's pass 15, lit kernels, GI trace / REUSE, indirect reads, stats copies; last frame's
    // q_gi_lag). Last frame's graphics-side readers (the #978 combine) are behind the fork's semaphore.
    memBarrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
               VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
               VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT
               | VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
    // THE CHAIN'S LIST HEADERS (each was a COMPUTE -> TRANSFER -> COMPUTE drain pair inside the lighting chain):
    // reset here, after the opening barrier (WAR: last frame's appends + indirect reads) and published by the
    // closing barrier (the appends' atomics + the indirect dispatches). Nothing between here and the chain
    // touches them.
    if (shadow && lp.split)
    {   // pass 15's VIS job list: count 0, (0, 1, 1) workgroups -- SELECT appends, VIS dispatches indirect over it
        static const uint32_t kHdr[4] = {0u, 0u, 1u, 1u};
        vkCmdUpdateBuffer(cmd, mPdnVis.buffer, 0, sizeof(kHdr), kHdr);
    }
    if (hdrs && lp.dsTiles)
    {   // (#827) the feature split's tile list: VkDispatchIndirectCommand (0, 1, 1) + pad -- CLASSIFY counts x up,
        // BASE / LOCAL dispatch indirect over it
        static const uint32_t kHdr[4] = {0u, 1u, 1u, 0u};
        vkCmdUpdateBuffer(cmd, mDsTileList.buffer, 0, sizeof(kHdr), kHdr);
    }
    if (hdrs && lp.giLists)
    {   // (#1036) the GI shade split's HIT / MISS lists: count 0, dispatch (0, 0, 1) each -- TRACE appends, MISS /
        // HIT-MAT / HIT-LIGHT dispatch indirect over them
        static const uint32_t kHdr[8] = {0u, 0u, 0u, 1u, 0u, 0u, 0u, 1u};
        vkCmdUpdateBuffer(cmd, mGiHit.buffer, 16u, sizeof(kHdr), kHdr);
    }
    // (#771 Phase 2) SUN EXPOSURE CERTIFICATES: the table's header + maintenance (its own pipeline); pass 15
    // finds the table through the push (rtr3.xy, mLightPrep.sxcAddr) and skips the sun sample where a
    // certificate proves it blocked. (#1031: the split core's TRACE reads it -- the generic core's rules)
    if (sxc) lp.sxcAddr = sunCertRecord(cmd, onAsync, lp.split || lp.pipe == mPdnMergeCorePipe);
    if (em || occ || chg)
    {
        // (pass 15's / the GI's layout and sets: set 2 is never read by these kernels -- the plain one)
        const VkDescriptorSet sets[5] = {mDsetDraw0, mDsetDraw1, mDsetDefer, mDsetDeferSplit, mRT->consumerSet()};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mRtReflLayout, 0, 5, sets, 1, &viewOff);
    }
    // the build kernel's push: the draw view (its time) + rtr3.zw = the mode / the record parity
    VKRtReflPush rp{};
    rp.d.view = viewIdx;
    if (em)
    {
        // (#91, clean_regir.glsl EMISSIVE LIGHTS) this parity's instance hash cleared, then the EMITTERS
        // (mode 0: a grid-stride loop over the instance store; push rtr3.w = this build's record parity),
        // their record BLOCKS (mode 1: one workgroup, a prefix sum; the grid header's rs_em: the triangle
        // cap this frame), the TRIANGLE records (mode 2: one invocation per record; #986 an unchanged
        // emitter's copied from the last build -- the other parity), the #986 CELL LISTS (modes 3-6: count,
        // scan, sums, fill; their counts cleared first), then POPULATION 2 (mode 7: one invocation per grid
        // cell) -- pass 15 samples them, the lit kernels and the GI trace read them later this frame. Fixed
        // regions of mRegir (no resource / descriptor change).
        // (#1006 INCREMENTAL CELLS, with the lists) + mode 12 (the ORIGIN SHIFT: the cells moved with the
        // clipmap, from a scratch copy) after the records, the dirty state in mode 4, the budget in mode 5, mode
        // 11 (SELECT: the cells to re-gather this build, the kept ones' slots carried to this build's records)
        // after the lists, and mode 7 over mode 11's build list through an INDIRECT dispatch (its group count
        // written by modes 5 / 11; a gather build: the whole grid).
        // (sub-timers: everything recorded since the previous mark -- the sun certificates, the binds --
        //  closes here, so the emissive stages below time only themselves)
        gpuSubMark(cmd, onAsync ? "c_rs_em_pre" : "g_rs_em_pre");
        const uint32_t par = mRsEmParity;
        mRsEmParity ^= 1u;
        mRsEmBuiltOnce = true;   // (the next header's EM_F_REUSE: this build fills parity par)
        // (WAR -- the hash's earlier readers: last build's GI trace / pass 15 / lit kernels; #1006 + last build's
        //  indirect read of mode 7's dispatch words, the stats copy of the cache header's per-build words and the
        //  last shift's scratch copy; the cells' last writers -> the shift's copy: the prep's opening barrier)
        vkCmdFillBuffer(cmd, mRegir.buffer, kRsEmHashOffset + (VkDeviceSize)par * kRsEmHashN * 8u,
                        (VkDeviceSize)kRsEmHashN * 8u, 0u);
        // (LIGHT TAKEOVER) the emitter entries reserved for the table's rows start empty (a row whose face
        // emits nothing this frame stays an empty block)
        if (mRsEmTakeNow != 0u)
            vkCmdFillBuffer(cmd, mRegir.buffer, kRsEmEntOffset + (VkDeviceSize)par * kRsEmInstCap * kRsEmEntBytes,
                            (VkDeviceSize)mRsEmTakeNow * kRsEmEntBytes, 0u);
        // (#986 CELL LISTS) every cell's count and fill cursor start at 0
        if (mRsEmListsNow) vkCmdFillBuffer(cmd, mRegir.buffer, kRsEmLCellOffset, kRsEmLCellBytes, 0u);
        // (#986 LONG CELLS) the long cells' count and the longest list start at 0
        if (mRsEmListsNow) vkCmdFillBuffer(cmd, mRegir.buffer, kRsEmLongOffset, 8u, 0u);
        if (mRsEmListsNow)
        {   // (#1006) the cells' signatures + the overlay's list counts (one region), the forward map (all NONE),
            // the cache header's per-build words (counters, mode 7's dispatch, stats, the age histogram)
            vkCmdFillBuffer(cmd, mRegir.buffer, kRsEmSigOffset, kRsEmSigBytes, 0u);
            vkCmdFillBuffer(cmd, mRegir.buffer, kRsEmFwdOffset, kRsEmFwdBytes, 0xFFFFFFFFu);
            vkCmdFillBuffer(cmd, mRegir.buffer, kRsEmCchFrameOffset, kRsEmCchFrameBytes, 0u);
            if (mRsEmShiftNow)
            {   // (ORIGIN SHIFT) population 2's cell words, then the overlay's + the cache, to the scratch
                const VkBufferCopy sc[2] = {{kRsEmCellOffset, kRsEmScrOffset, kRsEmCellBytes},
                                            {kRsEmOvCellOffset, kRsEmScrOffset + kRsEmCellBytes, kRsEmMoveBytes}};
                vkCmdCopyBuffer(cmd, mRegir.buffer, mRegir.buffer, 2u, sc);
            }
        }
        memBarrier(VK_PIPELINE_STAGE_2_CLEAR_BIT | VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, kRW);
        VKRtReflPush ep = rp;
        ep.rtr3[2] = 0u;   // mode 0: the emitters
        ep.rtr3[3] = par;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mRsEmPipe);
        vkCmdPushConstants(cmd, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ep), &ep);
        vkCmdDispatch(cmd, 64u, 4u, 1u);
        memBarrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, kRW, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, kRW);
        gpuSubMark(cmd, onAsync ? "c_rs_em_emitters" : "g_rs_em_emitters");   // (the clears + mode 0)
        ep.rtr3[2] = 1u;   // mode 1: the emitters' record blocks (one workgroup)
        vkCmdPushConstants(cmd, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ep), &ep);
        vkCmdDispatch(cmd, 1u, 1u, 1u);
        memBarrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, kRW, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, kRW);
        ep.rtr3[2] = 2u;   // mode 2: the triangle records, one invocation per record (grid-stride)
        vkCmdPushConstants(cmd, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ep), &ep);
        vkCmdDispatch(cmd, kRsEmCap / 64u, 1u, 1u);
        memBarrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, kRW, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, kRW);
        if (mRsEmListsNow && mRsEmShiftNow)
        {   // (#1006) mode 12: the ORIGIN SHIFT -- every cell from the scratch copy of the one that held its box
            ep.rtr3[2] = 12u;
            vkCmdPushConstants(cmd, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ep), &ep);
            vkCmdDispatch(cmd, kRsEmNCellsAll / 64u, 1u, 1u);
            memBarrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, kRW, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, kRW);
        }
        gpuSubMark(cmd, onAsync ? "c_rs_em_records" : "g_rs_em_records");   // (modes 1 + 2, #1006 + 12)
        if (mRsEmListsNow)
        {   // (#986) the CELL LISTS: COUNT (one workgroup per record), SCAN (one per 64-cell block), SUMS
            // (one), FILL (one per record) -- past the pairs' cap mode 5 flags the gather for mode 7.
            // (#1006: SCAN covers the overlay's blocks too and updates the cells' dirty state; SUMS decides the
            //  budget and starts mode 7's indirect dispatch)
            const uint32_t listWGs[4] = {kRsEmCap, kRsEmNCellsAll / 64u, 1u, kRsEmCap};
            for (uint32_t m = 0; m < 4u; ++m)
            {
                ep.rtr3[2] = 3u + m;
                vkCmdPushConstants(cmd, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ep), &ep);
                vkCmdDispatch(cmd, listWGs[m], 1u, 1u);
                memBarrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, kRW, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, kRW);
                static const char* const kListMark[2][4] = {
                    {"g_rs_em_count", "g_rs_em_scan", "g_rs_em_sums", "g_rs_em_fill"},
                    {"c_rs_em_count", "c_rs_em_scan", "c_rs_em_sums", "c_rs_em_fill"}};
                gpuSubMark(cmd, kListMark[onAsync ? 1 : 0][m]);
            }
            // (#1006) mode 11 SELECT: one invocation per cell id -> the build list, the long list, the kept cells
            ep.rtr3[2] = 11u;
            vkCmdPushConstants(cmd, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ep), &ep);
            vkCmdDispatch(cmd, kRsEmNCellsAll / 64u, 1u, 1u);
            // (+ the indirect read of mode 7's dispatch words)
            memBarrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, kRW,
                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                       kRW | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
            gpuSubMark(cmd, onAsync ? "c_rs_em_select" : "g_rs_em_select");
        }
        ep.rtr3[2] = 7u;   // mode 7: population 2, 64 cells per workgroup
        vkCmdPushConstants(cmd, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ep), &ep);
        if (mRsEmListsNow) vkCmdDispatchIndirect(cmd, mRegir.buffer, kRsEmCchArgsOffset);   // (#1006: mode 11's cells)
        else vkCmdDispatch(cmd, std::max(mRegirLevels, 1u) * (kRegirCellsPerLevel / 64u), 1u, 1u);
        if (mRsEmLongNow)
        {   // (#986) mode 9: the LONG CELLS, one workgroup each (grid-stride over mode 11's list; the cells are
            // disjoint from mode 7's, so no barrier between the two; mode 8 is the #869 occlusion resolve).
            // (#1108) On a GATHER build (mode 5: the pairs passed the cap) mode 9 builds every cell of the grid and
            // mode 7's indirect dispatch is 0 groups -- c_rs_em_long then carries the gather's cost.
            gpuSubMark(cmd, onAsync ? "c_rs_emissive" : "g_rs_emissive");   // (mode 7, the short cells)
            ep.rtr3[2] = 9u;
            vkCmdPushConstants(cmd, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ep), &ep);
            vkCmdDispatch(cmd, kRegirLevelsMax * kRegirCellsPerLevel / 64u, 1u, 1u);
            gpuSubMark(cmd, onAsync ? "c_rs_em_long" : "g_rs_em_long");
        }
        else
            gpuSubMark(cmd, onAsync ? "c_rs_emissive" : "g_rs_emissive");   // (now: mode 7, population 2, alone)
        // (-> pass 15, the lit kernels, the GI: the prep's closing barrier)
    }
    if (occ)
    {
        // (#869 OCCLUSION CULL, clean_deferred_light.comp) the cache: zeroed when new / re-enabled / the
        // last light list's frame did not resolve it (its counts are carried by rs_remap, one list to the
        // next), its stats every frame, then THE RESOLVE (mode 8 of the emissive build kernel: one
        // invocation per patch) turns last frame's notes into this frame's counts -- before pass 15, so
        // pass 15 and the lit kernels read the same counts. Fixed region of mRegir (6.25 MiB).
        if (!mRsOccRemapOk) mRsOccClear = true;
        // (WAR -- last frame's resolve / pass 15 / lit kernels -> the fill: the prep's opening barrier)
        vkCmdFillBuffer(cmd, mRegir.buffer, kRegirOccOffset, mRsOccClear ? kRsOccBytes : kRsOccStatBytes, 0u);
        memBarrier(VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, kRW);
        mRsOccClear = false;
        mRsOccRan = true;
        {
            VKRtReflPush op = rp;
            op.rtr3[2] = 8u;   // mode 8: the occlusion resolve
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mRsEmPipe);
            vkCmdPushConstants(cmd, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(op), &op);
            vkCmdDispatch(cmd, kRsOccCells / 64u, 1u, 1u);
            gpuSubMark(cmd, onAsync ? "c_rs_occ" : "g_rs_occ");   // (-> pass 15 / the lit kernels: the closing barrier)
        }
    }
    if (chg)
    {
        // (#1039) the GI CHANGE TAGS' bits 0-2 (stage 9, clean_deferred_light.comp CHG-TAG): one lane per GI texel --
        // last frame's reservoir there (layer parity ^ 1: nothing writes it this frame) against this frame's changed
        // / hot lights (the GI header + the light table, front end) -> the texel's tag word (bit 3 clear; HIT-LIGHT /
        // SHADE set it for the fresh hit). REUSE reads them. Push: rtr2.xy = the GI extent, rtr2.w = its parity.
        const uint32_t gw = mGiDn->width(), gh = mGiDn->height();
        VKRtReflPush tp{};
        tp.d.view = viewIdx;
        tp.rtr2[0] = (float)gw;
        tp.rtr2[1] = (float)gh;
        tp.rtr2[3] = (float)mGiDn->parity();
        vkCmdPushConstants(cmd, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(tp), &tp);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mGiSplitPipe[GIS_CHGTAG]);
        vkCmdDispatch(cmd, (gw + 7u) / 8u, (gh + 7u) / 8u, 1);
        gpuSubMark(cmd, onAsync ? "c_gi_chgtag" : "g_gi_chgtag");
    }
    // CLOSING BARRIER (one for C, E, O and the tags -- each had its own): their results -> pass 15 (SELECT /
    // SUNVIS: the counts, the cells, the certificates), the lit kernels, the GI hit lighting (+ HIT-LIGHT's /
    // SHADE's bit 3 onto the tag words) and REUSE, later on this queue.
    memBarrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
               VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
               kRW | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
}

// After the reflect passes (+ RT reflections), before the AO join / BASE / COMBINE; the split
// layout's sets + push are bound again when it returns (as recordRtReflect).
void LLCleanRendererVK::recordProjStoch(VkCommandBuffer cmd, const VKDeferPush& p, uint32_t viewOff, bool onAsync)
{
    if (!mPdnNow || !mPdn || !mPdnSamplePipe || !mRtReflLayout || !mRT) return;
    const VkDescriptorSet rtSet = mRT->consumerSet();
    if (rtSet == VK_NULL_HANDLE) return;
    // THE LIGHT PREP (C / E / O, the plan): recordWorld records it ahead of the lighting. Here only when that
    // point did not plan it (never expected: the same gate) -- inline, where it ran before the move.
    if (!mLightPrep.pdn)
    {
        LLVKHitch::count("lightPrepLate", 1u);
        lightPrepPlanShadow();
        if (!mLightPrep.pdn) return;   // (the same gate as above: cannot fail -- never a null pass-15 kernel)
        recordLightPrep(cmd, p.view, viewOff, onAsync, false);
    }
    const bool merged = mLightPrep.merged;
    // (No barrier of its own. Its only job was WAR on the denoiser's images -- last frame's lighting read the final
    //  image, its temporal pass the history: an execution dependency, which recordDeferLight's opening barrier
    //  (src COMPUTE, dst COMPUTE, ahead of every path into here) already gives against every earlier compute on
    //  this queue, and the fork's semaphore against last frame's graphics readers. Pass 15 reads the G-buffer /
    //  depth (graphics: the semaphore / that barrier), the light tables (front end) and the light prep's results
    //  (its closing barrier); nothing recorded before it in the chain -- RT mirror, lit-guard scan, SHADOW /
    //  CLASSIFY, REFLECT, the RT reflections -- reads what it writes (the selection, GEOM, the reservoirs, the job
    //  list) or writes what it reads (llcleanvisqueuevk.cpp THE FRAME WALK's dependency audit).)
    const uint32_t dw = mPdn->width(), dh = mPdn->height();
    VKRtReflPush rp{};
    rp.d = p;
    rp.rtr[0] = (float)dw;
    rp.rtr[1] = (float)dh;
    rp.rtr[2] = (float)(mPdnFrame & 0xFFFFFu);                   // (exact in a float)
    rp.rtr[3] = (float)(mPdnK | (mPdn->parity() << 4));          // K | the GEOM parity << 4
    // (#869 SPLIT, clean_deferred_light.comp pdnSplitSelect) rtr3.z: bits 0-2 the split lights per
    // texel (only into images that carry the split layers), bit 3 last frame's split ids are valid
    // here (the hysteresis reads them)
    const uint32_t splitK = mPdn->hasSplit() ? mPdnSplitK : 0u;
    rp.rtr3[2] = splitK | (splitK != 0u && mPdn->historyValid() ? 8u : 0u);
    U32 slope = 2, bleed = 1, facing = 1, oneRay = 0, awayTrace = 1;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<U32> s_slope(gSavedSettings, "RenderVKRTShadowSlopeBias", 2U);
    static LLCachedControl<U32> s_bleed(gSavedSettings, "RenderVKProjStochBleedFix", 1U);
    static LLCachedControl<U32> s_facing(gSavedSettings, "RenderVKProjStochFacingGate", 1U);
    static LLCachedControl<U32> s_one(gSavedSettings, "RenderVKShadowSunOneRay", 0U);
    static LLCachedControl<U32> s_away(gSavedSettings, "RenderVKRTShadowMergeAwayTrace", 1U);
    slope = std::min<U32>(s_slope, 3u);
    bleed = s_bleed; facing = s_facing; oneRay = s_one; awayTrace = s_away;
#endif
    const bool sunSkip = mLightPrep.sunSkip;   // (SUN-CHANNEL SKIP: lightPrepPlan)
    // clean_deferred_light.comp pass 15: rtr2.x the weak-sun weight (0 = no sun ray), rtr2.y the
    // view-slope origin offset (RenderVKRTShadowSlopeBias: 0, 1/8, 1/4, 1/2 of the render-pixel
    // footprint -- the RT shadow mask's rtSlopeBiasPx; the generic core gives it to the projectors
    // too), rtr2.z the PDN_OPT_* bits (exact integer).
    if (merged) rp.rtr2[0] = mShadowMergeSunW;
    rp.rtr2[1] = slope == 0u ? 0.f : 0.0625f * (float)(1u << slope);
    // (PDN_OPT_BLEED: the no-information flag, RenderVKProjStochBleedFix; PDN_OPT_FACING: the
    // turned-away receiver gate, RenderVKProjStochFacingGate -- clean_deferred_light.comp)
    // (#1031 PDN_OPT_OFIRST, RenderVKShadowOpaqueFirst: the generic core's walks try the opaque geometry
    //  alone first -- clean_shadow_core.glsl OPAQUE FIRST)
    bool ofirst = true;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<bool> s_ofirst(gSavedSettings, "RenderVKShadowOpaqueFirst", false);
    ofirst = s_ofirst;
#endif
    rp.rtr2[2] = (float)((bleed != 0u ? 1u : 0u) | (merged && oneRay != 0u ? 2u : 0u) | (sunSkip ? 4u : 0u)
                         | (facing != 0u ? 8u : 0u) | (mPdnDebug == 7u ? 16u : 0u)
                         | (merged && awayTrace != 0u ? 32u : 0u)   // (PDN_OPT_SUN_AWAY)
                         | (ofirst ? 64u : 0u));                    // (PDN_OPT_OFIRST)
    // (#869 Phase 5b / 5c) rtr2.w: RS_RES_ON = the reservoirs are bound at this extent (pass 15 writes
    // this parity's), RS_RES_PREV = last frame's pass 15 wrote the other parity's at this extent
    uint32_t rsRes = 0u;
    if (mRsNow && mRsMode >= 2u && mRsRes.buffer && mRsResW == dw && mRsResH == dh)
        rsRes = 1u | (mRsResPrevOk ? 2u : 0u);
    rp.rtr2[3] = (float)rsRes;
    mRsResPrevOk = rsRes != 0u;
    // pass 15's kernel and the split (lightPrepPlan: the generic core on a ReSTIR / split frame)
    VkPipeline pipe = mLightPrep.pipe;
    const bool split = mLightPrep.split;
    if (split) rp.rtr3[3] = mPdnVisSlots;   // (clean_deferred_light.comp pdnVisSlots)
    // (#771 Phase 2) the SUN EXPOSURE CERTIFICATES' table (the light prep's maintenance; 0 = SXC off): pass 15
    // skips the sun sample where a certificate proves it blocked
    rp.rtr3[0] = (uint32_t)(mLightPrep.sxcAddr & 0xFFFFFFFFull);
    rp.rtr3[1] = (uint32_t)(mLightPrep.sxcAddr >> 32);
    const VkDescriptorSet sets[5] = {mDsetDraw0, mDsetDraw1, mDsetDefer, mDsetDeferSplit, rtSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mRtReflLayout, 0, 5, sets, 1, &viewOff);
    VK_PUSH_BUDGET_CHECK(rp);
    vkCmdPushConstants(cmd, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(rp), &rp);
    if (split)
    {
        // (#1031) SELECT (the candidates, the reservoirs, the sample list + its gate -> the descriptors + GEOM);
        // SUNVIS (merged: the sun channel, whole) and VIS (the gated light samples' walks -> mPdnVis), both
        // reading the descriptors, independent of each other; then SHADE (the terms x the visibilities +
        // every other store). Compute -> compute RAW between the steps. VIS runs one thread per gated light
        // sample: SELECT appends them to mPdnVis's job list, whose header (count 0, (0, 1, 1) workgroups) THE LIGHT
        // PREP reset ahead of the lighting (recordLightPrep: no transfer drain pair here) is VIS's indirect
        // dispatch.
        constexpr VkAccessFlags2 kRW = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        auto raw = [cmd]()
        {
            VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            m.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; m.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            m.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
            m.dstAccessMask = kRW | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
            VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            d.memoryBarrierCount = 1; d.pMemoryBarriers = &m;
            vkCmdPipelineBarrier2(cmd, &d);
        };
        // (#1039, RenderVKVisQueue bit 8) the sun / light-sample visibility through the visibility queue
        // (llcleanvisqueuevk.cpp THE SHADOW CORE): GEN -> WALK (+ VIS's TAIL) in place of SUNVIS + VIS, where
        // visQueueDecide gave this recording the regions (the sun's only with every denoiser texel). Bit 8 off:
        // exactly the commands below.
        const bool qSun = mVqShSunNow && merged && !sunSkip && (uint64_t)mVqRegion[VQR_SHSUN].entries >= (uint64_t)dw * dh;
        const bool qVis = mVqShVisNow;
        if (qSun || qVis) visQueueShadowBegin(cmd, qSun, qVis);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPdnSelectPipe[merged ? 1 : 0]);
        vkCmdDispatch(cmd, (dw + 7u) / 8u, (dh + 7u) / 8u, 1);
        if (merged) gpuSubMark(cmd, onAsync ? "c_defer_shadowsel" : "g_defer_shadowsel");
        else        gpuSubMark(cmd, onAsync ? "c_defer_pstochsel" : "g_defer_pstochsel");
        raw();
        // (RenderVKShadowIsoLod: the walk kernels' isotropic alpha-test LOD -- the footprint ones meanwhile
        //  if those still compile)
        bool isoLod = true;
#if defined(LL_HAS_CLEAN_PIPELINE)
        static LLCachedControl<bool> s_iso(gSavedSettings, "RenderVKShadowIsoLod", false);
        isoLod = s_iso;
#endif
        // (#1128: the hang beacon's twins while RenderVKRTHangBeacon is on)
        const VkPipeline sunVis = rtBcPipe(isoLod && mPdnSunVisIsoPipe ? mPdnSunVisIsoPipe : mPdnSunVisPipe);
        const VkPipeline vis = rtBcPipe(isoLod && mPdnVisIsoPipe ? mPdnVisIsoPipe : mPdnVisPipe);
        // (#1039 M6, RenderVKVisQueue bit 16) the lighting chain collects producers: GEN here, the walks in the
        // frame walk, SHADE + the denoiser after it (llcleanvisqueuevk.cpp THE FRAME WALK)
        const bool m6 = (qSun || qVis) && mVqM6Collect && !mVqM6Walk[VQC_SHADOW] && !mVqM6Post[VQC_SHADOW];
        if (m6)
        {
            const bool sunQ = merged && !sunSkip;
            visQueueShadowGen(cmd, rp, dw, dh, sunQ, qSun, qVis, merged, onAsync, sunVis, vis);
            const VkPipeline shade = mPdnShadePipe;
            visQueueM6Defer(VQC_SHADOW,
                [this, rp, sunQ, qSun, qVis, isoLod, merged, onAsync](VkCommandBuffer c)
                { visQueueShadowWalk(c, rp, sunQ, qSun, qVis, isoLod, merged, onAsync, false); },
                [this, rp, p, viewOff, rtSet, sunQ, qSun, qVis, onAsync, shade, merged, sunSkip, bleed, rsRes, oneRay](VkCommandBuffer c)
                {
                    // (after the frame walk's barrier: the RT layout's sets again -- other consumers ran between)
                    const VkDescriptorSet s5[5] = {mDsetDraw0, mDsetDraw1, mDsetDefer, mDsetDeferSplit, rtSet};
                    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, mRtReflLayout, 0, 5, s5, 1, &viewOff);
                    visQueueShadowFinish(c, rp, sunQ, qSun, qVis);   // (the counters; SHADE's push)
                    recordProjStochTail(c, p, viewOff, onAsync, shade, merged, sunSkip, bleed, rsRes, oneRay);
                });
            return;
        }
        if (qSun || qVis)
            visQueueShadowCore(cmd, rp, dw, dh, merged && !sunSkip, qSun, qVis, isoLod, merged, onAsync, sunVis, vis);
        else
        {
            if (merged && !sunSkip)
            {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, sunVis);
                vkCmdDispatch(cmd, (dw + 7u) / 8u, (dh + 7u) / 8u, 1);
                gpuSubMark(cmd, onAsync ? "c_defer_shadowsun" : "g_defer_shadowsun");
            }
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vis);
            vkCmdDispatchIndirect(cmd, mPdnVis.buffer, 4u);   // (one thread per gated light sample, 64 per workgroup)
            if (merged) gpuSubMark(cmd, onAsync ? "c_defer_shadowvis" : "g_defer_shadowvis");
            else        gpuSubMark(cmd, onAsync ? "c_defer_pstochvis" : "g_defer_pstochvis");
        }
        raw();
        pipe = mPdnShadePipe;
    }
    recordProjStochTail(cmd, p, viewOff, onAsync, pipe, merged, sunSkip, bleed, rsRes, oneRay);
}

// recordProjStoch from its last kernel on (SHADE / the fused pass, the ReSTIR counters, the denoiser, the split
// layout re-bound) -- a function of its own so the M6 frame walk can record it after the walks.
void LLCleanRendererVK::recordProjStochTail(VkCommandBuffer cmd, const VKDeferPush& p, uint32_t viewOff, bool onAsync,
                                            VkPipeline pipe, bool merged, bool sunSkip, uint32_t bleed, uint32_t rsRes,
                                            uint32_t oneRay)
{
    const uint32_t dw = mPdn->width(), dh = mPdn->height();
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdDispatch(cmd, (dw + 7u) / 8u, (dh + 7u) / 8u, 1);
    if (merged) gpuSubMark(cmd, onAsync ? "c_defer_shadowmerge" : "g_defer_shadowmerge");
    else        gpuSubMark(cmd, onAsync ? "c_defer_pstoch" : "g_defer_pstoch");
    if (mRsNow) rsStatsRecord(cmd);   // (#869: the ReSTIR counters of this frame)

    LLCleanDenoiserVK::Params dp;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<F32> s_hd(gSavedSettings, "RenderVKDenoiseHistory", 32.f);
    static LLCachedControl<F32> s_hs(gSavedSettings, "RenderVKDenoiseSpecHistory", 24.f);
    static LLCachedControl<F32> s_hm(gSavedSettings, "RenderVKDenoiseSpecMinHistory", 4.f);
    static LLCachedControl<F32> s_gamma(gSavedSettings, "RenderVKDenoiseClamp", 1.5f);
    static LLCachedControl<F32> s_lphi(gSavedSettings, "RenderVKDenoiseLumaPhi", 4.f);
    static LLCachedControl<F32> s_wcut(gSavedSettings, "RenderVKDenoiseLightChange", 0.25f);
    dp.maxHistDiffuse = s_hd;
    dp.maxHistSpecular = s_hs;
    dp.minHistSpecular = s_hm;
    dp.clampGamma = s_gamma;
    dp.lumPhi = s_lphi;
    dp.wCut = s_wcut;
#endif
    dp.atrous = mPdnAtrous;
    dp.debug = mPdnDebug;
    dp.frame = mPdnFrame;
    // #870 raw routing (DLSS RR reconstructs): the lighting reads this frame's raw projector /
    // merged-sun samples; the denoiser passes them through (the a-trous loop still runs: its last
    // iteration writes the depth the consumer's upsample reads). (#869 SPLIT: the split lights'
    // one-ray visibility passes raw with the rest's ratio -- the projector signal is both; with
    // RenderVKProjStochasticSplitK 0 exactly the old routing.)
    dp.raw = rrRawSignal(kRrRawProj, mPdn->width(), mPdn->height());
    dp.rawSun = rrRawSignal(kRrRawMergedSun, mPdn->width(), mPdn->height());
    dp.info = bleed != 0u;   // (the producer flags no-information texels with PDN_OPT_BLEED)
    // (#1038) DISOCCLUSION: the GI denoiser's young-history rules for the local-light shadows too (an
    // avatar walking out from behind a wall restarted its ReSTIR DI / projector shadow ratio from one
    // ray, with only the plain a-trous): the history fix (RenderVKDenoiseDisoccFix frames), the young-
    // texel boost and the moving-surface depth tolerance. The split lights' own channel keeps its rules.
    {
        U32 dfix = 4;
        bool yboost = true, mztol = true;
#if defined(LL_HAS_CLEAN_PIPELINE)
        static LLCachedControl<U32>  s_dfix(gSavedSettings, "RenderVKDenoiseDisoccFix", 4U);
        static LLCachedControl<bool> s_yboost(gSavedSettings, "RenderVKDenoiseYoungBoost", true);
        static LLCachedControl<bool> s_mztol(gSavedSettings, "RenderVKDenoiseMovingDepthTol", true);
        dfix = s_dfix; yboost = s_yboost; mztol = s_mztol;
#endif
        dp.histFix = std::min<U32>(dfix, 7u);
        dp.youngBoost = yboost;
        dp.moveZTol = mztol;
    }
    // (#869 / #85) a ReSTIR-converged diffuse takes RenderVKReSTIRDenoiseIters a-trous iterations
    {
        U32 convIt = 1;
#if defined(LL_HAS_CLEAN_PIPELINE)
        static LLCachedControl<U32> s_conv(gSavedSettings, "RenderVKReSTIRDenoiseIters", 1U);
        convIt = s_conv;
#endif
        dp.convIters = (rsRes != 0u && convIt != 0u) ? std::min<U32>(convIt, 7u) : 0u;
        // (#869) ... but never on a surface that moved on its own (an animated avatar: a still
        // light's shadow slides over it -- the reservoir's M is no sign of a converged shadow)
        F32 surfMove = 0.5f;
#if defined(LL_HAS_CLEAN_PIPELINE)
        static LLCachedControl<F32> s_smove(gSavedSettings, "RenderVKReSTIRSurfaceMoveVeto", 0.5f);
        surfMove = s_smove;
#endif
        dp.surfMoveTexels = dp.convIters != 0u ? std::clamp(surfMove, 0.f, 3.75f) : 0.f;
    }
    // (#869) the moving-light rule (RenderVKReSTIRMoveCut): the history of a texel lit by a light that
    // moved this frame -- only on a ReSTIR frame (pass 15 writes the moved share into MOT w then;
    // regirHeaderFill sets the producer's half from the same setting)
    {
        U32 moveCut = 1;
        F32 moveHist = 4.f;
#if defined(LL_HAS_CLEAN_PIPELINE)
        static LLCachedControl<U32> s_mcut(gSavedSettings, "RenderVKReSTIRMoveCut", 1U);
        static LLCachedControl<F32> s_mhist(gSavedSettings, "RenderVKReSTIRMoveHistory", 4.f);
        moveCut = s_mcut; moveHist = s_mhist;
#endif
        dp.moveHist = (mRsNow && moveCut != 0u) ? (U32)std::clamp(moveHist + 0.5f, 1.f, 15.f) : 0u;
    }
    if (merged && !sunSkip)
    {
        // THE SUN / MOON CHANNEL: the RT shadow mask's own denoiser settings (its history, the
        // moving-caster rules, the spatial radius cap, the exact reprojection) -- the sun keeps
        // the mask's quality on the denoiser's guides (llcleandenoisevk.h SUN / MOON CHANNEL).
        F32 hist = 32.f, dyn = 2.f, spatial = 12.f, gamma = 1.5f, soft = 0.01f;
        U32 dmax = 4, persist = 4;
        bool exact = true;
#if defined(LL_HAS_CLEAN_PIPELINE)
        static LLCachedControl<F32> s_shist(gSavedSettings, "RenderVKRTShadowHistory", 32.f);
        static LLCachedControl<F32> s_sdyn(gSavedSettings, "RenderVKRTShadowDynamicHistory", 2.f);
        static LLCachedControl<U32> s_sdmax(gSavedSettings, "RenderVKRTShadowDynamicMaxHistory", 4U);
        static LLCachedControl<F32> s_ssp(gSavedSettings, "RenderVKRTShadowSpatial", 12.f);
        static LLCachedControl<F32> s_sgamma(gSavedSettings, "RenderVKRTShadowClamp", 1.5f);
        static LLCachedControl<U32> s_spersist(gSavedSettings, "RenderVKRTShadowMoverPersist", 4U);
        static LLCachedControl<bool> s_sexact(gSavedSettings, "RenderVKRTMaskExactReproj", true);
        static LLCachedControl<F32> s_ssoft(gSavedSettings, "RenderVKRTShadowSoftness", 0.01f);
        hist = s_shist; dyn = s_sdyn; dmax = s_sdmax; spatial = s_ssp; gamma = s_sgamma;
        persist = s_spersist; exact = s_sexact; soft = s_ssoft;
#endif
        dp.sun = true;
        dp.sunCut = mShadowMergeSunCut;
        dp.sunHard = soft <= 0.f;
        dp.sunExact = exact;
        dp.sunMaxHist = std::clamp(hist, 1.f, 63.f);
        dp.sunDynHist = std::clamp(dyn, 1.f, 15.f);
        dp.sunDynMax = (F32)std::min<U32>(dmax, 15u);
        dp.sunMove = mShadowMergeSunMove;
        // (the mask's cap is in mask texels; the denoiser's extent is the same 1 or 1/2 of render)
        dp.sunSpatialCap = std::clamp(spatial, 0.f, 16.f);
        dp.sunClamp = std::clamp(gamma, 0.5f, 8.f);
        dp.sunPersist = std::min<U32>(persist, 7u);
        dp.sunRays = oneRay != 0u ? 1.f : 0.f;   // (RenderVKShadowSunOneRay: one disc ray per texel)
    }
    mPdn->recordTemporal(cmd, mDsetDraw0, p.width, p.height, dp);
    gpuSubMark(cmd, onAsync ? "c_dn_tmp" : "g_dn_tmp");
    mPdn->recordSpatial(cmd, p.width, p.height, dp);
    gpuSubMark(cmd, onAsync ? "c_dn_sp" : "g_dn_sp");
    // Back to the split layout (incompatible push range: every set re-bound) for the passes after.
    const VkDescriptorSet ssets[4] = {mDsetDraw0, mDsetDraw1, mDsetDefer, mDsetDeferSplit};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mDeferSplitLayout, 0, 4, ssets, 1, &viewOff);
    vkCmdPushConstants(cmd, mDeferSplitLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
}

// ======================= ReSTIR DI RESERVOIRS (#869 Phase 5b / 5c) =======================
// A sampling frame (pdnFrame, the denoiser allocated at its extent): with reuse wanted
// (RenderVKReSTIRDI >= 2) the reservoir buffer at that extent -- created once per extent (behind a
// device wait: set 3 may be bound by frames in flight), freed when reuse is switched off; a refused
// allocation (#697) retries after 30 s, meanwhile pass 15 runs without reuse (5a).
void LLCleanRendererVK::rsResFrame()
{
    if (!mCtx || !mPdn || !mPdn->allocated()) return;
    const uint32_t w = mPdn->width(), h = mPdn->height();
    const bool want = mRsMode >= 2u;
    if (!want)
    {
        if (mRsRes.buffer)
        {
            vkDeviceWaitIdle(mCtx->device());
            mCtx->destroyBuffer(mRsRes);
            mRsRes = GpuBuffer{};
            mRsResW = mRsResH = 0;
            mRsResPrevOk = false;
            writeRsResBinding();
        }
        return;
    }
    if (mRsRes.buffer && mRsResW == w && mRsResH == h) return;
    const double now = pdnClock();
    if (now < mRsResRetryAt) return;
    const VkDeviceSize bytes = (VkDeviceSize)w * h * 2u * 8u;
    vkDeviceWaitIdle(mCtx->device());
    if (mRsRes.buffer) { mCtx->destroyBuffer(mRsRes); mRsRes = GpuBuffer{}; }
    mRsResW = mRsResH = 0;
    mRsResPrevOk = false;
    {
        VkMemScope memcat(VKMEM_SCENE);   // #407 (with the lights / grid)
        VkDeviceContext::SoftAllocScope soft(mCtx);
        mRsRes = mCtx->createBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    }
    if (!mRsRes.buffer)
    {
        mRsResRetryAt = now + 30.0;
        LL_WARNS("Vulkan") << "ReSTIR DI reservoirs: " << ((bytes + (1u << 19)) >> 20) << " MB refused (VRAM); no"
                              " temporal / spatial reuse (RenderVKReSTIRDI acts as 1) -- retry in 30 s" << LL_ENDL;
        writeRsResBinding();
        return;
    }
    mCtx->setObjectName(VK_OBJECT_TYPE_BUFFER, (uint64_t)mRsRes.buffer, "restirReservoirs");
    mRsResW = w; mRsResH = h;
    writeRsResBinding();
    LL_INFOS("Vulkan") << "ReSTIR DI reservoirs: " << w << "x" << h << " x 2 parities, "
                       << ((bytes + (1u << 19)) >> 20) << " MB" << LL_ENDL;
}

void LLCleanRendererVK::writeRsResBinding()
{
    if (!mCtx || !mDsetDeferSplit) return;
    const VkBuffer b = mRsRes.buffer ? mRsRes.buffer : mRsResDummy.buffer;
    if (!b) return;
    const VkDescriptorBufferInfo bi{b, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = mDsetDeferSplit; w.dstBinding = kDsBindingRsRes; w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w.pBufferInfo = &bi;
    vkUpdateDescriptorSets(mCtx->device(), 1, &w, 0, nullptr);
}

// ======================= THE SHADOW CORE SPLIT (#1031) =======================
// RenderVKShadowSplitKernels: the generic core (pass 15 -DCLEAN_SHADOW_CORE) as SELECT -> SUNVIS (merged)
// + VIS -> SHADE over a 64 B selection descriptor per denoiser texel (mPdnSel, set 3 binding 26) and the
// light samples' visibilities (mPdnVis, binding 27: pdnVisSlotsWanted() x 4 B per texel). Allocated on a
// sampling frame at the denoiser's extent (rsResFrame's rule: syncFromStoresPrestep, behind a device
// wait), freed with the denoiser's images or the setting; a refusal (#697) runs the fused core, retry in
// 30 s; a settings change that lengthens the sample list runs the fused core until the next prestep.
bool LLCleanRendererVK::pdnSplitWanted() const
{
    bool on = true;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<bool> s_on(gSavedSettings, "RenderVKShadowSplitKernels", true);
    on = s_on;
#endif
    return on && mPdnSelectPipe[0] && mPdnSelectPipe[1] && mPdnSunVisPipe && mPdnVisPipe && mPdnShadePipe;
}

// The longest light-sample list a texel can have under the settings (clean_deferred_light.comp: nProj +
// spN + the probe): the K picks or the exact list (<= K), or the dense window (RenderVKReSTIRMoveLights,
// the header's rs_cfg.w bits 8-11) + the emitter stratum; + the split lights; + the occlusion probe.
uint32_t LLCleanRendererVK::pdnVisSlotsWanted() const
{
    U32 moveCut = 1, moveLights = 4;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<U32> s_mcut(gSavedSettings, "RenderVKReSTIRMoveCut", 1U);
    static LLCachedControl<U32> s_mlights(gSavedSettings, "RenderVKReSTIRMoveLights", 4U);
    moveCut = s_mcut; moveLights = s_mlights;
#endif
    const uint32_t dense = moveCut != 0u ? std::min<uint32_t>(moveLights, kRegirResMax - 1u) + 1u : 0u;
    return std::max<uint32_t>(std::clamp<U32>(mPdnK, 1u, 2u), dense) + std::min<U32>(mPdnSplitK, 4u) + 1u;
}

void LLCleanRendererVK::pdnSelFree()
{
    if (mPdnSel.buffer) { mCtx->destroyBuffer(mPdnSel); mPdnSel = GpuBuffer{}; }
    if (mPdnVis.buffer) { mCtx->destroyBuffer(mPdnVis); mPdnVis = GpuBuffer{}; }
    mPdnSelW = mPdnSelH = mPdnVisSlots = 0;
    writePdnSelBinding();
}

void LLCleanRendererVK::pdnSelFrame()
{
    if (!mCtx || !mPdn || !mPdn->allocated()) return;
    const uint32_t w = mPdn->width(), h = mPdn->height();
    if (!pdnSplitWanted())
    {
        if (mPdnSel.buffer || mPdnVis.buffer)
        {
            vkDeviceWaitIdle(mCtx->device());
            pdnSelFree();
        }
        return;
    }
    const uint32_t slots = pdnVisSlotsWanted();
    if (mPdnSel.buffer && mPdnVis.buffer && mPdnSelW == w && mPdnSelH == h && mPdnVisSlots == slots) return;
    const double now = pdnClock();
    if (now < mPdnSelRetryAt) return;
    const VkDeviceSize selBytes = (VkDeviceSize)w * h * 64u;
    const VkDeviceSize visBytes = 16u + (VkDeviceSize)w * h * slots * 8u;   // (the job list header, the jobs, the visibilities)
    vkDeviceWaitIdle(mCtx->device());
    pdnSelFree();
    {
        VkMemScope memcat(VKMEM_DENOISE);
        VkDeviceContext::SoftAllocScope soft(mCtx);
        mPdnSel = mCtx->createBuffer(selBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mPdnSel.buffer)
            mPdnVis = mCtx->createBuffer(visBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT
                                         | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    }
    if (!mPdnSel.buffer || !mPdnVis.buffer)
    {
        pdnSelFree();
        mPdnSelRetryAt = now + 30.0;
        LL_WARNS("Vulkan") << "split shadow core: the selection descriptors + visibilities ("
                           << ((selBytes + visBytes + (1u << 19)) >> 20)
                           << " MB) were refused (VRAM); the fused core instead -- retry in 30 s" << LL_ENDL;
        return;
    }
    mCtx->setObjectName(VK_OBJECT_TYPE_BUFFER, (uint64_t)mPdnSel.buffer, "pdnSelect");
    mCtx->setObjectName(VK_OBJECT_TYPE_BUFFER, (uint64_t)mPdnVis.buffer, "pdnVis");
    mPdnSelW = w; mPdnSelH = h;
    mPdnVisSlots = slots;
    writePdnSelBinding();
    LL_INFOS("Vulkan") << "split shadow core (RenderVKShadowSplitKernels): " << w << "x" << h << ", descriptors "
                       << ((selBytes + (1u << 19)) >> 20) << " MB + light-sample jobs / visibilities (" << slots << " slots) "
                       << ((visBytes + (1u << 19)) >> 20) << " MB" << LL_ENDL;
}

void LLCleanRendererVK::writePdnSelBinding()
{
    if (!mCtx || !mDsetDeferSplit) return;
    const VkBuffer s = mPdnSel.buffer ? mPdnSel.buffer : mRsResDummy.buffer;
    const VkBuffer v = mPdnVis.buffer ? mPdnVis.buffer : mRsResDummy.buffer;
    if (!s || !v) return;
    const VkDescriptorBufferInfo bi[2] = {{s, 0, VK_WHOLE_SIZE}, {v, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet w[2]{};
    for (uint32_t i = 0; i < 2; ++i)
    {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = mDsetDeferSplit; w[i].dstBinding = i == 0 ? kDsBindingPdnSel : kDsBindingPdnVis;
        w[i].descriptorCount = 1; w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[i].pBufferInfo = &bi[i];
    }
    vkUpdateDescriptorSets(mCtx->device(), 2, w, 0, nullptr);
}

// ======================= POINT LIGHT SHADOW MAPS (#439, the non-RT fallback) =======================
// On the RT tier point lights are shadowed by ReSTIR DI (RenderVKReSTIRLights 1: every point light, a
// soft disc, the own-fixture rule). Cards without ray queries get CUBE SHADOW MAPS for the most
// important few. LANDED HERE: the per-frame SELECTION and its VRAM BUDGET (RenderVKPointShadowMaps 1,
// default 0) -- what the render path will consume -- logged on change. NOT LANDED (the plan; it
// shares the projector atlas's machinery, which #567 is reworking -- every change there additive):
//   1. STORAGE: one D32 cube-map array per resolution tier (512 / 256 / 128, by apparent size), a
//      static layer + a dynamic overlay per light (the projector atlas's static-cache split, #567:
//      kShadowSplitStatic / kShadowSplitDynamic), allocated on first use under the VRAM ladder's
//      STEP_PROJ_SHADOWS rung (+ its own rung below it), freed after a grace period unused.
//   2. CULL: ONE compute cull for every face of every selected light (clean_cull.comp's caster mode
//      with a sphere of the light's radius -- <= 20 m in SL -- instead of a frustum, writing 6 face
//      bits per instance), the casters' own light excluded (the instance box holding the light and
//      under RenderVKReSTIRFixtureSize: the RT path's own-fixture rule, as data).
//   3. RENDER: one layered multi-draw-indirect per tier (gl_Layer = light x 6 + face, the shadow
//      pipelines' depth-only VS + a linear-distance FS), static layer only when something static in
//      range changed (gVKChangedSpheres, the atlas's rule), the dynamic overlay on a budget
//      (RenderVKPointShadowDynamicInterval) -- a per-frame cap on face renders.
//   4. LOOKUP: localLight's point branch (the lit FS / deferred kernels) samples the cube by
//      direction with normal-offset + slope bias and a jittered 4-tap PCF; record index in the light's
//      tex.z like a projector's (a point light's record = its tier + layer), so the same "no record =
//      unshadowed" rule holds and the ReSTIR set (rsLightSet) keeps excluding mapped lights.
// Selection: brightness x apparent size (the projectors' score), 1.5 x hysteresis for last frame's,
// attached lights only with RenderVKReSTIRAttachedLights, inert / degenerate records never; only
// while the RT tier does not shadow point lights already.
void LLCleanRendererVK::pointShadowMapSelect(const LLVKWorldFrame& wf)
{
    U32 mode = 0, maxN = 4;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<U32> s_mode(gSavedSettings, "RenderVKPointShadowMaps", 0U);
    static LLCachedControl<U32> s_max(gSavedSettings, "RenderVKPointShadowMapMax", 4U);
    mode = s_mode; maxN = s_max;
#endif
    mPsmSel.clear();
    mPsmBytes = 0;
    const bool rtCovers = mProjMode == 2u && mRsMode != 0u && mRsExt;   // (ReSTIR shadows them)
    if (mode != 0u && !rtCovers && maxN > 0u)
    {
        const float* cam = wf.cam_pos;
        const size_t n = std::min<size_t>(mLightsCPU.size(), std::min<size_t>(wf.lights.size(), 256));
        for (size_t i = 0; i < n; ++i)
        {
            const VKLightGPU& g = mLightsCPU[i];
            if (g.proj_origin[3] >= 0.5f || !(g.pos_radius[3] > 0.f) || g.pos_radius[2] < -1.0e8f) continue;
            if (wf.lights[i].proj_axis[1] >= 0.5f && !mRsAttached) continue;   // (attached: facelights)
            const float dx = g.pos_radius[0] - cam[0], dy = g.pos_radius[1] - cam[1], dz = g.pos_radius[2] - cam[2];
            const float dist = std::max(std::sqrt(dx * dx + dy * dy + dz * dz), 0.1f);
            const float lum = 0.2126f * g.color_falloff[0] + 0.7152f * g.color_falloff[1] + 0.0722f * g.color_falloff[2];
            const float size = g.pos_radius[3] / dist;
            float score = lum * std::min(size, 8.f);
            if (std::binary_search(mPsmKeys.begin(), mPsmKeys.end(), wf.lights[i].key)) score *= 1.5f;
            if (!std::isfinite(score) || !(score > 0.f)) continue;
            const uint32_t res = size > 1.f ? 512u : (size > 0.25f ? 256u : 128u);
            mPsmSel.push_back({(uint32_t)i, wf.lights[i].key, res, score});
        }
        std::sort(mPsmSel.begin(), mPsmSel.end(), [](const PsmSel& a, const PsmSel& b) {
            return a.score != b.score ? a.score > b.score : a.light < b.light; });
        if (mPsmSel.size() > maxN) mPsmSel.resize(maxN);
        for (const PsmSel& s : mPsmSel) mPsmBytes += 2ull * 6ull * s.res * s.res * 4ull;   // static + dynamic, D32
    }
    mPsmKeys.clear();
    for (const PsmSel& s : mPsmSel) mPsmKeys.push_back(s.key);
    std::sort(mPsmKeys.begin(), mPsmKeys.end());
    const uint32_t key = mode == 0u ? 0u : (rtCovers ? 1u : 2u + (uint32_t)mPsmSel.size() * 4u + (uint32_t)(mPsmBytes >> 20) * 1024u);
    if (key != mPsmLogKey && !mOffscreen)
    {
        mPsmLogKey = key;
        if (mode != 0u)
            LL_INFOS("VKShadow") << "point light shadow maps (RenderVKPointShadowMaps 1, #439 non-RT fallback): "
                                 << (rtCovers ? "not needed -- ray traced through ReSTIR DI (RenderVKReSTIRLights 1)"
                                              : std::to_string(mPsmSel.size()) + " light(s) selected (RenderVKPointShadowMapMax "
                                                    + std::to_string(maxN) + "), " + std::to_string((mPsmBytes + (1u << 19)) >> 20)
                                                    + " MB of cube maps planned -- the cube render path is not implemented yet:"
                                                      " they stay unshadowed")
                                 << LL_ENDL;
    }
}

// ======================= ReSTIR DI STATS (#869 Phase 5) =======================
// After pass 15 (a ReSTIR frame): harvest this ring slot's copy from 3 frames ago, then copy this
// frame's counters (the grid header's rs_stat: the build's + pass 15's) into it. GPU timers on (the line
// is logged with them), or (#1108) the emissive build on: its WHOLE-GRID GATHER note reads the same words
// (never a wait: the copy is 3 frames old). Bounded: 3 slots of a few hundred bytes.
void LLCleanRendererVK::rsStatsRecord(VkCommandBuffer cmd)
{
    VkMemScope memcat(VKMEM_READBACK);   // #407
    if (!mRegir.buffer || !mCtx) return;
    if (!mTsOn && !mRsEmNow)
    {   // (nothing to read: the slots' copies would be stale when the ring restarts)
        mRsStatUsed[0] = mRsStatUsed[1] = mRsStatUsed[2] = false;
        return;
    }
    const uint32_t slot = mRsStatSlot;
    mRsStatSlot = (mRsStatSlot + 1u) % 3u;
    // (rs_stat + rs_em: contiguous in the header; + #91 rs_em_stat and rs_em_take_cfg and #986 rs_em_cache,
    // a second copy region)
    constexpr VkDeviceSize kBytesA = sizeof(RegirHeader::rs_stat) + sizeof(RegirHeader::rs_em);
    constexpr VkDeviceSize kBytesB = sizeof(RegirHeader::rs_em_stat) + sizeof(RegirHeader::rs_em_take_cfg)
                                   + sizeof(RegirHeader::rs_em_cache);
    constexpr VkDeviceSize kBytesC = 8u;   // (#986 LONG CELLS: their count, the longest list -- kRsEmLongOffset)
    constexpr VkDeviceSize kBytesD = kRsOccStatBytes;   // (#869 OCCLUSION CULL: OCC_STAT_WORDS -- kRegirOccOffset)
    constexpr VkDeviceSize kBytesE = 32u;   // (#1006 INCREMENTAL CELLS: EM_CCH_STAT, 4 x 8 B -- kRsEmCchStatOffset)
    constexpr VkDeviceSize kBytesF = 40u;   // (#1009 CLUSTER STATICS + SKIP DIAGNOSTIC + EM_CCH_OUT: CC_HDR [41..45] -- kRsEmCchClusterOffset)
    constexpr VkDeviceSize kBytes = kBytesA + kBytesB + kBytesC + kBytesD + kBytesE + kBytesF;
    static_assert(offsetof(RegirHeader, rs_em) == offsetof(RegirHeader, rs_stat) + sizeof(RegirHeader::rs_stat),
                  "rs_stat / rs_em contiguous");
    static_assert(offsetof(RegirHeader, rs_em_take_cfg) == offsetof(RegirHeader, rs_em_stat) + sizeof(RegirHeader::rs_em_stat),
                  "rs_em_stat / rs_em_take_cfg contiguous");
    static_assert(offsetof(RegirHeader, rs_em_cache) == offsetof(RegirHeader, rs_em_take_cfg) + sizeof(RegirHeader::rs_em_take_cfg),
                  "rs_em_take_cfg / rs_em_cache contiguous");
    if (mRsStatUsed[slot] && mRsStatRb.mapped)
    {   // (#1108) THE WHOLE-GRID GATHER NOTE (clean_regir.glsl GATHER): that build's cells were built without the
        // cell lists -- the pairs passed the cap (rs_em_cache.w EM_C_GATHER), or the lists were off by setting
        const uint32_t* s = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(mRsStatRb.mapped)
                                                               + (size_t)slot * kBytes);
        const bool emBuilt = s[8 + 2] != 0u;   // (rs_em.z: the triangle cap, written while the build runs)
        const bool overflow = emBuilt && mRsStatLists[slot] && (s[20 + 3] & 1u) != 0u;
        const bool bySetting = emBuilt && !mRsStatLists[slot];
        if (overflow || (bySetting && !mRsEmGatherSetLogged))
        {
            ++mRsEmGatherN;
            const double now = pdnClock();
            if (bySetting || now - mRsEmGatherLogT >= kRsEmGatherLogSecs)
            {
                LL_INFOS("Vulkan") << "emissive light grid: WHOLE-GRID GATHER build ("
                                   << (overflow ? "the cell candidate pairs passed the cap"
                                                : "RenderVKReSTIREmissiveCellLists 0")
                                   << "): " << mRsEmGatherN << " such build(s) since the last note; this one: emitters "
                                   << (s[8 + 3] >> 1) << ", triangle records " << std::min(s[8], s[8 + 2]) << " / cap "
                                   << s[8 + 2] << " (skipped or dropped " << s[8 + 1] << "), cell candidate pairs "
                                   << s[20 + 2] << " / cap " << kRsEmPairCap << "; cells built "
                                   << (overflow && mRsStatLong[slot]
                                           ? "a workgroup per cell (mode 9)"
                                           : "a lane per cell (mode 7; RenderVKReSTIREmissiveLongCells 0 or the lists off)")
                                   << LL_ENDL;
                if (bySetting) mRsEmGatherSetLogged = true;
                else mRsEmGatherLogT = now;
                mRsEmGatherN = 0;
            }
        }
        if (mRsEmListsNow) mRsEmGatherSetLogged = false;   // (the setting back on: a later switch-off is noted again)
        // (clean_regir.glsl SUBSET STRIDE) the rest's stride (rs_em_take_cfg.w): a STEP re-subsets every emitter at
        // once (records rebuilt, emissive reservoirs rejected, cells forced) -- noted, at most a line per
        // kRsEmStrideLogSecs with the steps since the last one
        const uint32_t stride = emBuilt ? s[16 + 3] : 0u;
        if (stride != 0u)
        {
            if (mRsEmStrideLast != 0u && stride != mRsEmStrideLast)
            {
                ++mRsEmStrideSteps;
                const double now = pdnClock();
                if (now - mRsEmStrideLogT >= kRsEmStrideLogSecs)
                {
                    LL_INFOS("Vulkan") << "emissive light build: subset stride " << mRsEmStrideLast << " -> " << stride
                                       << " (" << mRsEmStrideSteps << " step(s) since the last note): triangle records "
                                       << std::min(s[8], s[8 + 2]) << " / cap " << s[8 + 2] << ", skipped or dropped "
                                       << s[8 + 1] << ", emitters " << (s[8 + 3] >> 1) << LL_ENDL;
                    mRsEmStrideLogT = now;
                    mRsEmStrideSteps = 0;
                }
            }
            mRsEmStrideLast = stride;
        }
    }
    if (mRsStatUsed[slot] && mRsStatRb.mapped && mRsStatTs[slot] && mTsOn)
    {
        const uint32_t* s = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(mRsStatRb.mapped)
                                                               + (size_t)slot * kBytes);
        for (int k = 0; k < 8; ++k) mRsStatAcc.stat[k] += s[k];
        if (s[8 + 2] != 0u)   // (#91: the emissive records' cap: built that frame)
        {
            const double em = (double)std::min(s[8], s[8 + 2]);
            mRsStatAcc.em += em;
            mRsStatAcc.emDrop += s[8 + 1];
            mRsStatAcc.emPeak = std::max(mRsStatAcc.emPeak, em);
            mRsStatAcc.emInst += (double)(s[8 + 3] >> 1);   // (rs_em.w: parity | emitters << 1)
            mRsStatAcc.emCap = std::max(mRsStatAcc.emCap, (double)s[8 + 2]);   // (RenderVKReSTIREmissiveTriangles)
            ++mRsStatAcc.emFrames;
            for (int k = 0; k < 4; ++k) mRsStatAcc.emStat[k] += s[12 + k];
            // (LIGHT TAKEOVER: rs_em_take_cfg -- this frame's table lights, taken, refused)
            mRsStatAcc.emTake[0] += mRsStatTakeN[slot];
            mRsStatAcc.emTake[1] += s[16 + 1];
            mRsStatAcc.emTake[2] += s[16 + 2];
            // (#986: rs_em_cache -- records copied / built, cell candidate pairs, EM_C_GATHER)
            mRsStatAcc.emCopied += s[20 + 0];
            mRsStatAcc.emBuilt += s[20 + 1];
            mRsStatAcc.emPairs += s[20 + 2];
            mRsStatAcc.emPairsPeak = std::max(mRsStatAcc.emPairsPeak, (double)s[20 + 2]);
            if ((s[20 + 3] & 1u) != 0u) ++mRsStatAcc.emGather;
            if (mRsStatLists[slot])   // (the region is cleared and copied with the cell lists only)
            {
                mRsStatAcc.emLong += s[24 + 0];
                mRsStatAcc.emLongest = std::max(mRsStatAcc.emLongest, (double)s[24 + 1]);
            }
            if (mRsStatInc[slot])   // (#1006: EM_CCH_STAT, after the occlusion words -- see rsStatsText)
            {
                const uint32_t* x = s + (kBytesA + kBytesB + kBytesC + kBytesD) / 4u;
                mRsStatAcc.emDirty += x[0];
                mRsStatAcc.emRegather += x[1];
                mRsStatAcc.emForced += x[2];
                mRsStatAcc.emStaleMax = std::max(mRsStatAcc.emStaleMax, (double)x[3]);
                mRsStatAcc.emWeak += x[4];
                mRsStatAcc.emMiss += x[5];
                mRsStatAcc.emKept += x[6];
                mRsStatAcc.emFull += x[7];
                mRsStatAcc.emClInst += x[8];   // (#1009: CC_HDR [41..44], right after EM_CCH_STAT -- kBytesF)
                mRsStatAcc.emClTri += x[9];
                mRsStatAcc.emOutInst += x[16];   // (CC_HDR [45], after the skip words [42..44])
                mRsStatAcc.emOutTri += x[17];
                if (mRsStatSkip[slot])
                {
                    for (int k = 0; k < 5; ++k) mRsStatAcc.emSkip[k] += x[10 + k];
                    ++mRsStatAcc.emSkipFrames;
                }
                ++mRsStatAcc.emIncFrames;
            }
        }
        if (mRsStatOcc[slot])   // (#869 OCCLUSION CULL: words 0-12, see clean_deferred_light.comp STATS)
        {
            const uint32_t* o = s + (kBytesA + kBytesB + kBytesC) / 4u;
            for (int k = 0; k < 13; ++k) mRsStatAcc.occ[k] += o[k];
            ++mRsStatAcc.occFrames;
        }
        mRsStatAcc.setN += mRsStatSetN[slot];
        mRsStatAcc.pointN += mRsStatPointN[slot];
        mRsStatAcc.movedN += mRsStatMovedN[slot];
        mRsStatAcc.flickN += mRsStatFlickN[slot];
        ++mRsStatAcc.frames;
    }
    mRsStatUsed[slot] = false;
    if (!mRsStatRb.buffer)
        mRsStatRb = mCtx->createBuffer(3u * kBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!mRsStatRb.buffer) return;
    VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    m.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; m.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    m.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT; m.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1; d.pMemoryBarriers = &m;
    vkCmdPipelineBarrier2(cmd, &d);
    const VkBufferCopy c[6] = {{offsetof(RegirHeader, rs_stat), (VkDeviceSize)slot * kBytes, kBytesA},
                               {offsetof(RegirHeader, rs_em_stat), (VkDeviceSize)slot * kBytes + kBytesA, kBytesB},
                               {kRegirOccOffset, (VkDeviceSize)slot * kBytes + kBytesA + kBytesB + kBytesC, kBytesD},
                               {kRsEmLongOffset, (VkDeviceSize)slot * kBytes + kBytesA + kBytesB, kBytesC},
                               {kRsEmCchStatOffset, (VkDeviceSize)slot * kBytes + kBytesA + kBytesB + kBytesC + kBytesD,
                                kBytesE},
                               {kRsEmCchClusterOffset,
                                (VkDeviceSize)slot * kBytes + kBytesA + kBytesB + kBytesC + kBytesD + kBytesE, kBytesF}};
    // (the regions present this frame, packed: A, B, + D with the occlusion cache, + C and (#1006) E, (#1009) F
    //  with the cell lists -- the per-build words they hold are cleared with the lists)
    VkBufferCopy cc[6];
    uint32_t nc = 0;
    cc[nc++] = c[0];
    cc[nc++] = c[1];
    if (mRsOccNow) cc[nc++] = c[2];
    if (mRsEmListsNow) cc[nc++] = c[3];
    if (mRsEmListsNow) cc[nc++] = c[4];
    if (mRsEmListsNow) cc[nc++] = c[5];
    vkCmdCopyBuffer(cmd, mRegir.buffer, mRsStatRb.buffer, nc, cc);
    mRsStatLists[slot] = mRsEmListsNow;
    mRsStatLong[slot] = mRsEmLongNow;
    mRsStatTs[slot] = mTsOn;
    mRsStatInc[slot] = mRsEmListsNow;
    mRsStatSkip[slot] = mRsEmListsNow && mRsEmSkipNow;
    mRsStatOcc[slot] = mRsOccNow;
    mRsStatSetN[slot] = mRsSetN;
    mRsStatPointN[slot] = mRsPointN;
    mRsStatMovedN[slot] = mRsMovedN;
    mRsStatFlickN[slot] = mRsFlickN;
    mRsStatTakeN[slot] = mRsEmTakeNow != 0u ? mRsEmTakeLights : 0u;
    mRsStatUsed[slot] = true;
}

std::string LLCleanRendererVK::rsStatsText()
{
    RsStatAcc& a = mRsStatAcc;
    if (!a.frames) return std::string();
    const double k = 1.0 / a.frames;
    const double* s = a.stat;
    const double tex = s[3] + s[4];
    std::ostringstream os;
    os << std::fixed << std::setprecision(1) << "ReSTIR DI over " << a.frames << " frames (RenderVKReSTIRDI " << mRsMode
       << "): light set " << a.setN * k << " lights (" << a.pointN * k << " point; "
       << a.movedN * k << " moving, " << a.flickN * k << " flickering); grid cells with candidates "
       << std::setprecision(0) << s[0] * k << ", candidates/cell " << std::setprecision(2)
       << (s[0] > 0.0 ? s[1] / s[0] : 0.0) << " (max " << std::setprecision(0) << s[2] * k << ")"
       << "; pass-15 texels from the grid " << std::setprecision(3) << (tex > 0.0 ? s[3] / tex : 0.0)
       << " (cluster walk outside it " << (tex > 0.0 ? s[4] / tex : 0.0) << ")";
    if (mRsMode >= 2u)
        os << "; reservoir M/texel " << std::setprecision(2) << (s[3] > 0.0 ? s[5] / s[3] : 0.0)
           << ", temporal kept " << std::setprecision(3) << (s[3] > 0.0 ? s[6] / s[3] : 0.0)
           << ", spatial taps kept/texel " << (s[3] > 0.0 ? s[7] / s[3] : 0.0);
    if (a.emFrames)
    {
        const double ef = 1.0 / a.emFrames;
        const double* e = a.emStat;
        os << "; emissive (RenderVKReSTIREmissive " << mRsEmMode << ") emitters " << std::setprecision(1)
           << a.emInst * ef << ", triangles live " << std::setprecision(0) << a.em * ef
           << " / cap " << a.emCap << " (peak " << a.emPeak << ", skipped or dropped/frame " << std::setprecision(1)
           << a.emDrop * ef << "), grid cells with emitting triangles (#1006: of the cells re-gathered that frame) " << std::setprecision(0) << e[0] * ef
           << ", candidate triangles/cell " << std::setprecision(2) << (e[0] > 0.0 ? e[1] / e[0] : 0.0)
           << "; pass-15 texels with emitter candidates " << std::setprecision(3) << (tex > 0.0 ? e[2] / tex : 0.0)
           << ", emitter samples traced/frame " << std::setprecision(0) << e[3] * ef
           << "; light takeover (RenderVKReSTIREmissiveLightTakeover): point lights with emitting faces "
           << std::setprecision(1) << a.emTake[0] * ef << ", taken over " << a.emTake[1] * ef
           << ", refused (a block dropped) " << a.emTake[2] * ef
           << "; records reused / rebuilt per frame (RenderVKReSTIREmissiveReuse) " << a.emCopied * ef
           << " / " << a.emBuilt * ef << "; cell candidate pairs (RenderVKReSTIREmissiveCellLists) "
           << std::setprecision(0) << a.emPairs * ef << " / cap " << kRsEmPairCap << " (peak " << a.emPairsPeak
           << "), gather builds " << a.emGather
           << "; long cells (RenderVKReSTIREmissiveLongCells) " << std::setprecision(1) << a.emLong * ef
           << "/frame, longest list " << std::setprecision(0) << a.emLongest;
        if (a.emIncFrames)
        {   // (#1006 INCREMENTAL CELLS + WEAK OVERLAY, clean_regir.glsl: EM_CCH_STAT per lists build; the cells
            // with emitting triangles above count only the cells re-gathered that frame)
            const double xf = 1.0 / a.emIncFrames;
            os << std::setprecision(1) << "; incremental cells (RenderVKReSTIREmissiveIncremental): dirty/frame "
               << a.emDirty * xf << ", re-gathered/frame " << a.emRegather * xf << " (forced or refreshed "
               << a.emForced * xf << ", forward-map misses among them " << a.emMiss * xf << "), kept/frame "
               << a.emKept * xf << ", max staleness " << std::setprecision(0) << a.emStaleMax << " frames, full builds "
               << a.emFull << " / " << a.emIncFrames << "; weak emitters on the overlay "
               << "(RenderVKReSTIREmissiveWeakPower)/frame " << std::setprecision(1) << a.emWeak * xf
               // (#1009 CLUSTER STATICS, clean_regir.glsl: statics traced on their cluster RT cut, emitting through
               //  their legacy row -- of the emitters above, and their triangles before the cap)
               << "; cluster-LOD statics among them (RenderVKReSTIREmissiveClusterStatic): emitters "
               << a.emClInst * xf << ", their triangles " << std::setprecision(0) << a.emClTri * xf
               // (mode 0: emitters whose reach misses the grid's outermost level -- not listed, not in the cap)
               << "; emitters outside the grid (not listed, not in the triangle cap)/frame " << std::setprecision(1)
               << a.emOutInst * xf << ", their triangles " << std::setprecision(0) << a.emOutTri * xf;
            if (a.emSkipFrames)
            {   // (#1009 SKIP DIAGNOSTIC, RenderVKReSTIREmissiveSkipStats: emitting instances left out, by why)
                const double sf = 1.0 / a.emSkipFrames;
                const double* q = a.emSkip;
                os << std::setprecision(1) << "; emitting instances left out/frame (RenderVKReSTIREmissiveSkipStats): "
                   << "rigged legacy " << q[0] * sf << ", rigged cluster " << q[1] * sf
                   << ", cluster static without a legacy row " << q[2] * sf << ", worn / own attachment " << q[3] * sf
                   << ", not in the TLAS " << q[4] * sf;
            }
        }
    }
    if (a.occFrames)
    {   // (#869 OCCLUSION CULL, RenderVKReSTIROcclusionCull: clean_deferred_light.comp STATS)
        const double of = 1.0 / a.occFrames;
        const double* o = a.occ;
        os << std::fixed << std::setprecision(1)
           << "; occlusion cull: patches live " << o[0] * of << " / " << kRsOccCells << ", culling " << o[1] * of
           << ", culled lights/culling patch " << (o[1] > 0.0 ? o[2] / o[1] : 0.0)
           << " (patches with 1-3 / 4-15 / 16-63 / 64+: " << o[3] * of << " / " << o[4] * of << " / " << o[5] * of
           << " / " << o[6] * of << "), new culls/frame " << o[8] * of << ", re-admissions/frame " << o[7] * of
           << ", probes/frame " << std::setprecision(0) << o[9] * of << std::setprecision(1)
           << ", patch claims refused (chain full)/frame " << o[10] * of << ", patches freed/frame " << o[11] * of
           << ", counts dropped with their light/frame " << o[12] * of;
    }
    mRsStatAcc = RsStatAcc{};
    return os.str();
}

// (#91) RenderVKReSTIREmissive 1 with ReSTIR DI on and the emissive kernel built: the emitters may
// light this frame, so ReSTIR DI (and pass 15) run even without a light in the set.
bool LLCleanRendererVK::rsEmLightWanted() const
{
    U32 em = 0;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<U32> s_em(gSavedSettings, "RenderVKReSTIREmissive", 1U);
    em = s_em;
#endif
    return em == 1u && mRsMode != 0u && mRsEmPipe != VK_NULL_HANDLE;
}

// (#91) LIGHT TAKEOVER (clean_regir.glsl LIGHT TAKEOVER): this frame's table from the world frame's
// (instance, light) pairs -- the point lights' emitting faces. Rows grouped by light in light order (a
// light's rows are its emitter entries [first, first + rows): mode 1 reads them as one range), at most
// kRsEmTakeN (a light that does not fit whole is left out: it keeps its point light); then the rows again
// sorted by instance for mode 0's binary search. Bounded: (256 + kRsEmTakeN) x 2 words.
void LLCleanRendererVK::rsEmTakeTable(const LLVKWorldFrame& wf)
{
    mRsEmTakeRows = mRsEmTakeLights = 0;
    if (wf.light_takeover.empty())
    {
        mRsEmTake.clear();
        return;
    }
    const size_t nL = std::min<size_t>(std::min<size_t>(mLightsCPU.size(), wf.lights.size()), 256);
    static std::vector<std::pair<uint32_t, uint32_t>> byLight;   // (light, instance)
    byLight.clear();
    for (const auto& p : wf.light_takeover)
    {
        const uint32_t li = p.second;
        if (li >= nL) continue;
        const VKLightGPU& g = mLightsCPU[li];
        // (a point light with a live record; the GPU checks the rest: the grid, the radius, the colour)
        if (g.proj_origin[3] >= 0.5f || !(g.pos_radius[3] > 0.f) || g.pos_radius[2] < -1.0e8f) continue;
        byLight.emplace_back(li, p.first);
    }
    std::sort(byLight.begin(), byLight.end());
    byLight.erase(std::unique(byLight.begin(), byLight.end()), byLight.end());
    {   // (an instance belongs to one prim, so to one light: a repeated instance keeps its first light)
        static std::unordered_set<uint32_t> seen;
        seen.clear();
        byLight.erase(std::remove_if(byLight.begin(), byLight.end(),
                                     [](const std::pair<uint32_t, uint32_t>& p) { return !seen.insert(p.second).second; }),
                      byLight.end());
    }
    mRsEmTake.assign((size_t)(256u + kRsEmTakeN) * 2u, 0u);
    static std::vector<std::pair<uint32_t, uint32_t>> byInst;   // (instance, row | light << 16)
    byInst.clear();
    uint32_t rows = 0;
    for (size_t i = 0; i < byLight.size();)
    {
        size_t j = i;
        while (j < byLight.size() && byLight[j].first == byLight[i].first) ++j;
        const uint32_t n = (uint32_t)(j - i);
        if (rows + n > kRsEmTakeN) break;   // (lights are in importance order: the rest keep their light)
        const uint32_t li = byLight[i].first;
        mRsEmTake[li * 2u] = rows;
        mRsEmTake[li * 2u + 1u] = n;
        for (size_t k = i; k < j; ++k) byInst.emplace_back(byLight[k].second, (rows + (uint32_t)(k - i)) | (li << 16));
        rows += n;
        ++mRsEmTakeLights;
        i = j;
    }
    std::sort(byInst.begin(), byInst.end());   // (unique instances: one row each)
    for (size_t k = 0; k < byInst.size(); ++k)
    {
        mRsEmTake[(256u + k) * 2u] = byInst[k].first;
        mRsEmTake[(256u + k) * 2u + 1u] = byInst[k].second;
    }
    mRsEmTakeRows = rows;   // (= byInst.size(): the rows ARE the reserved emitter entries [0, rows))
}

// ======================= MERGED RT SHADOWS (#195 + #85, RenderVKRTShadowMerge) =======================

// The merge's preconditions this frame: the setting, the stochastic projector path and the split
// deferred lighting asked for, on screen, the sun / moon ray traced THIS frame (selectLitVariant's
// mSunRtNow: RT warm, TLAS valid, not over the RT budget) AND ray-traced projector records, the
// kernels and a valid TLAS. needImages: the denoiser's sun channel is allocated too (selectLitVariant's
// prediction -- pdnFrame allocates it on frames that want it, so a first frame runs today's path)
// and no lit debug twin is wanted (those frames light with the uber kernel / forward).
bool LLCleanRendererVK::shadowMergeWanted(bool needImages) const
{
    U32 merge = 1, stoch = 0, split = 1;
    bool defer = true;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<U32>  s_merge(gSavedSettings, "RenderVKRTShadowMerge", 1U);
    static LLCachedControl<U32>  s_stoch(gSavedSettings, "RenderVKProjStochastic", 1U);
    static LLCachedControl<bool> s_defer(gSavedSettings, "RenderVKDeferredLighting", true);
    merge = s_merge; stoch = s_stoch; defer = s_defer;
#endif
    split = deferSplitMode();   // (#824: RenderVKDeferredSplit 2 = auto per GPU class)
    // (#1075: stereo frames light forward -- the merged pass is deferred-only)
    if (merge == 0u || stoch == 0u || split == 0u || !defer || mOffscreen || stereoActive()) return false;
    if (!mSunRtNow || mProjMode != 2u || (mProjRecords == 0u && !(mRsMode != 0u && mRsSetN > 0u) && !rsEmLightWanted()))
        return false;
    if (!mPdn || !mPdnMergePipe || !mPdnSamplePipe || !mRtReflLayout || !mPdn->pipelinesReady() || !mDeferSplitOk
        || !mDsetDraw0)
        return false;
    if (!mRT || !mRT->tlasValid() || mRT->consumerSet() == VK_NULL_HANDLE) return false;
    if (needImages && (!mPdn->hasSun() || litDebugWanted())) return false;
    return true;
}

// Once per frame (selectLitVariant): the WEAK-SUN weight and the light's motion for the merged
// pass's sun / moon channel.
//   * WEIGHT: the active light's colour (frame info atmos lane 2: the sun's, or the moon's at night)
//     luminance x the sun scale (lane 12.z), against RenderVKRTShadowMergeSunMin = t: 0 below t (no
//     sun ray at all -- night, a dim moon cost nothing), a smooth ramp to 1 at 2t (the shadow fades
//     toward lit: never a pop), 1 above. 0 = off (always traced).
//   * MOTION: the light direction's step since last frame in disc radii (the RT shadow mask's
//     precise atan2 measure: rtMaskPushFill), which caps the sun history (dnSunMoveCap); a sun <->
//     moon switch (lane 9.w) or a jump past 10 degrees (time of day set, a teleport to another
//     sky) restarts it.
void LLCleanRendererVK::shadowMergeSunState()
{
    F32 tmin = 0.02f, soft = 0.01f;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<F32> s_min(gSavedSettings, "RenderVKRTShadowMergeSunMin", 0.02f);
    static LLCachedControl<F32> s_soft(gSavedSettings, "RenderVKRTShadowSoftness", 0.01f);
    tmin = s_min; soft = s_soft;
#endif
    const float* lc = &mAtmos[2 * 4];
    float lum = (0.2126f * lc[0] + 0.7152f * lc[1] + 0.0722f * lc[2]) * std::max(mAtmos[12 * 4 + 2], 0.f);
    // <GMDX> Physical night (gmdxnightlight.h THE NIGHT'S AMBIENT): the moon is dimmed, and so is the
    // ambient it is seen against (atmos[6].z = 2 - amb; the exposure lifts both). Its shadow shows
    // by that ratio, so the light is measured against the ambient: as bright as it would be with
    // Second Life's ambient (sRGB-ish space, as the colour). 1 by day and with physical night off.
    const float night_amb = std::clamp(2.f - mAtmos[6 * 4 + 2], 1e-4f, 1.f);
    if (night_amb < 1.f) lum *= std::pow(1.f / night_amb, 1.f / 2.4f);
    float w = 1.f;
    if (tmin > 0.f)
    {
        const float t = std::clamp((lum - tmin) / tmin, 0.f, 1.f);
        w = t * t * (3.f - 2.f * t);
    }
    mShadowMergeSunW = std::isfinite(w) ? w : 1.f;
    mShadowMergeSunLum = std::isfinite(lum) ? lum : 0.f;
    if (mOffscreen) return;   // (a snapshot's frame: the live sequence's motion is measured on screen)
    // CALIBRATION LOG (merged frames only; the band changes with the time of day, not per frame):
    // the measure against RenderVKRTShadowMergeSunMin whenever the sun / moon ray turns off, fades
    // or is full.
    if (mShadowMergeNow || mShadowMergeTry)
    {
        const int band = mShadowMergeSunW <= 0.f ? 0 : (mShadowMergeSunW >= 1.f ? 2 : 1);
        if (band != mShadowMergeSunBand)
        {
            mShadowMergeSunBand = band;
            static const char* const kBand[3] = {"no sun / moon ray (weak light)", "fading toward lit", "full"};
            LL_INFOS("VKShadow") << "merged RT shadows: sun / moon " << kBand[band] << " -- light measure "
                                 << mShadowMergeSunLum << " (colour luminance x sun scale; RenderVKRTShadowMergeSunMin "
                                 << tmin << ": no ray below it, full from " << 2.f * tmin << ")" << LL_ENDL;
        }
    }
    const float* ld = &mAtmos[7 * 4];
    const float sunUp = mAtmos[9 * 4 + 3];
    mShadowMergeSunCut = false;
    mShadowMergeSunMove = 0.f;
    if (mShadowMergePrevOk)
    {
        double a[3] = {ld[0], ld[1], ld[2]};
        double b[3] = {mShadowMergePrevDir[0], mShadowMergePrevDir[1], mShadowMergePrevDir[2]};
        const double la = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
        const double lb = std::sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
        double ang = 0.0;
        if (la > 1e-12 && lb > 1e-12)
        {
            for (int i = 0; i < 3; ++i) { a[i] /= la; b[i] /= lb; }
            const double cx = a[1] * b[2] - a[2] * b[1], cy = a[2] * b[0] - a[0] * b[2], cz = a[0] * b[1] - a[1] * b[0];
            ang = std::atan2(std::sqrt(cx * cx + cy * cy + cz * cz), a[0] * b[0] + a[1] * b[1] + a[2] * b[2]);
        }
        const double disc = std::tan(std::max((double)std::clamp(soft, 0.f, 1.f) * 5.0, 0.05) * 0.01745329252);
        mShadowMergeSunMove = std::isfinite(ang) ? (float)std::min(ang / disc, 64.0) : 64.f;
        if (!std::isfinite(ang) || ang > 10.0 * 0.01745329252 || (sunUp > 0.5f) != (mShadowMergePrevSunUp > 0.5f))
            mShadowMergeSunCut = true;
    }
    mShadowMergePrevDir[0] = ld[0]; mShadowMergePrevDir[1] = ld[1]; mShadowMergePrevDir[2] = ld[2];
    mShadowMergePrevSunUp = sunUp;
    mShadowMergePrevOk = true;
}

// One line whenever a shadow source changes path (on screen; at most one per 2 s, the switches in
// between counted): sun / moon = merged RT | RT mask | inline RT (forward) | maps | off, projectors =
// merged RT | stochastic RT | RT mask | inline RT (forward) | maps | RT, none in view | off.
void LLCleanRendererVK::shadowPathLog()
{
    uint32_t sun = 0, proj = 0;
    if (!mShadowOn) sun = 0;                                   // off
    else if (!mSunRtNow) sun = 1;                              // maps
    else if (mShadowMergeNow) sun = 2;                         // merged RT
    else if (mRtMaskOn) sun = 3;                               // RT mask
    else sun = 4;                                              // inline RT (the forward ray-query pass)
    if (mProjMode == 0u) proj = 0;                             // off
    else if (mProjMode == 1u) proj = 1;                        // maps
    else if (mProjRecords == 0u) proj = 2;                     // RT, none in view
    else if (mShadowMergeNow) proj = 3;                        // merged RT
    else if (mPdnNow) proj = 4;                                // stochastic RT (denoised)
    else if (mRtMaskOn) proj = 5;                              // RT mask (exact, <= 4 per pixel)
    else proj = 6;                                             // inline RT (forward)
    const uint32_t state = sun | (proj << 4);
    static double s_next = 0.0;
    static uint32_t s_flips = 0;
    if (state == mShadowPathLogged) return;
    ++s_flips;
    const double now = pdnClock();
    if (now < s_next) return;
    s_next = now + 2.0;
    static const char* const kSun[5] = {"off", "shadow maps", "merged RT (one pass + denoiser with the projectors)",
                                        "RT shadow mask", "ray traced inline (forward lit pass)"};
    static const char* const kProj[7] = {"off", "shadow maps", "ray traced (none in view)",
                                         "merged RT (one pass + denoiser with the sun / moon)",
                                         "stochastic RT (denoised)", "RT shadow mask (exact, up to 4 per pixel)",
                                         "ray traced inline (forward lit pass)"};
    LL_INFOS("VKShadow") << "shadow paths: sun / moon = " << kSun[sun]
                         << (sun == 2u ? " [light measure " + std::to_string(mShadowMergeSunLum) + ", weight "
                                             + std::to_string(mShadowMergeSunW) + "]"
                                       : std::string())
                         << "; projectors = " << kProj[proj]
                         << (s_flips > 1 ? " [" + std::to_string(s_flips) + " changes since the last line]" : std::string())
                         << LL_ENDL;
    s_flips = 0;
    mShadowPathLogged = state;
}

// recordDeferredLighting, merged frames: the merged pass + the denoiser BEFORE the SHADOW kernel
// (which reads the denoised sun / moon), then their final image -> its reads.
void LLCleanRendererVK::recordShadowMergeFirst(VkCommandBuffer cmd, const VKDeferPush& p, uint32_t viewOff, bool onAsync)
{
    recordProjStoch(cmd, p, viewOff, onAsync);
    auto barrier = [](VkCommandBuffer c)
    {
        VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        m.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; m.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        m.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        m.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.memoryBarrierCount = 1; d.pMemoryBarriers = &m;
        vkCmdPipelineBarrier2(c, &d);
    };
    if (mVqM6Post[VQC_SHADOW])
    {
        // (#1039 M6: the pass's SHADE + denoiser were deferred past the frame walk -- the barrier follows them)
        std::function<void(VkCommandBuffer)> inner = std::move(mVqM6Post[VQC_SHADOW]);
        mVqM6Post[VQC_SHADOW] = [inner, barrier](VkCommandBuffer c) { inner(c); barrier(c); };
        return;
    }
    barrier(cmd);
}
