/**
 * @file llcleanrtdenoisevk.cpp
 * @brief RT shadow denoiser (#85): the spatial stage's SVGF-style a-trous iterations
 *        and the denoiser debug views, on top of the existing mask pipeline
 *        (clean_rt_mask_common.glsl: trace A -> temporal B -> spatial C).
 *
 * The temporal pass (B) already is the reprojection / disocclusion / variance-clamp /
 * adaptive-length half of a ReLAX / SVGF denoiser. What this adds is the other half: an
 * edge-aware a-trous wavelet with variance-guided luminance weights for the sun / moon
 * mask (clean_rt_mask_spatial.comp header), gated by the penumbra radius so contact
 * shadows stay crisp.
 *
 * SVGF MODE (RenderVKRTShadowSvgf, default on; push flag bit 1): the temporal pass clamps
 * the static-caster sun history to its fast history's neighbourhood instead of one noisy
 * frame's 3x3 (penumbra tails keep their history), rejects taps whose plane slope
 * changed, keeps 8 frames on hard static edges and shortens the history while the sun
 * moves (rtMaskPushFill measures the step); the a-trous runs on the accumulated
 * estimate's variance and refills young (disoccluded, #608) texels from converged
 * neighbours. Shader headers have the details. No extra VRAM, no extra passes.
 *
 * VRAM: none. The iterations ping-pong through the READ history slot's sun
 * accumulation and meta (rgba16f; mask set bindings 16 / 17), which only the temporal
 * pass reads and the next mask frame overwrites (F3a ping-pong: it becomes the write
 * slot; copy mode: rtMaskHistory copies over it first).
 * GPU: N x (25 taps x 3 image loads) per mask texel for the sun; roughly 0.1-0.15 ms per
 * iteration at a half-res 4K mask on a mid-range card (unmeasured; sub-mark c_rtmask_sp
 * covers all iterations).
 *
 * #666 DENOISER EVERYWHERE:
 *   * PROJECTORS (RenderVKRTShadowProjDenoise, default on; push p0.y bit 2): every ray-
 *     traced projector slot on the sun's SVGF path -- per-slot history length + fast mean
 *     (mRtMaskProjMeta, rgba16ui x 2 history slots = 16 B per mask px, in kRtMaskFmt so
 *     rtMaskBytesAt / the VRAM ladder count it), the fast-history clamp box, the moving-
 *     caster detector against the fast mean, the light-motion cap (ProjShadow texel.w,
 *     selectProjShadows) and the same a-trous iterations in the same dispatches (scratch:
 *     the read slot's projector accumulation, binding 18, and the final rgba8 image -- no
 *     extra VRAM). Cost scales with the projector slots a texel traced (0-4): a texel with
 *     none early-outs, lit / umbra slots with no variance left pass through. Sub-marks
 *     c_rtmask_tmp / c_rtmask_sp include it (same dispatches).
 *   * NO TAA DEPENDENCE (RenderVKRTShadowTemporalAlways, default on): the history runs on
 *     "last frame's camera is valid" (p0.y bit 1), not "TAA jitter on"; ensureTemporal
 *     keeps the motion / previous-instance plumbing alive for the mask (rtMaskWantsTemporal).
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

#include <algorithm>
#include <cmath>
#include <iomanip>

void LLCleanRendererVK::writeRtMaskAtrousDescriptors()
{
    // Set p writes history slot p and reads slot 1 - p (ensureRtMask): slot 0 = the Acc
    // images, slot 1 = mRtMaskHist. Set 0 is also the copy path's set (reads mRtMaskHist).
    // Bindings 16 / 17 = the READ slot's sun accumulation / meta.
    if (!mDsetRtMask[0] || !mDsetRtMask[1]) return;
    // Binding 18 (#666): the READ slot's projector accumulation (rgba16f, 4 layers) -- the
    // projector a-trous scratch, dead after the temporal pass like 16 / 17.
    const GpuImage* sun[2]  = {&mRtMaskHist[0], &mRtMaskAccSun};
    const GpuImage* meta[2] = {&mRtMaskHist[4], &mRtMaskMeta};
    const GpuImage* proj[2] = {&mRtMaskHist[1], &mRtMaskAccProj};
    for (int p = 0; p < 2; ++p)
    {
        // (#1075: the 2D_ARRAY views, a layer per view -- the projector image's default view is the
        //  array over every view's 4 slots already)
        if (!sun[p]->arrayView || !meta[p]->arrayView || !proj[p]->view) continue;
        const VkDescriptorImageInfo ii[3] = {{VK_NULL_HANDLE, sun[p]->arrayView, VK_IMAGE_LAYOUT_GENERAL},
                                             {VK_NULL_HANDLE, meta[p]->arrayView, VK_IMAGE_LAYOUT_GENERAL},
                                             {VK_NULL_HANDLE, proj[p]->view, VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet w[3]{};
        for (int i = 0; i < 3; ++i)
        {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet = mDsetRtMask[p];
            w[i].dstBinding = 16u + (uint32_t)i; w[i].descriptorCount = 1;
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[i].pImageInfo = &ii[i];
        }
        vkUpdateDescriptorSets(mCtx->device(), 3, w, 0, nullptr);
    }
}

// #674 SUN DIAGNOSTIC (RenderVKRTShadowSunDiag = N): "static sun shadows bounce without
// TAA / FSR". Logs, once per recording for N recordings after the setting changes, what
// every shadow consumer of this frame sees: the light direction the frame info carries
// (atmos lane 7 = ssr.atmos[7], what the RT mask / lit FS trace toward) and its step since
// the last logged frame (degrees), the light colour's sum (a sun <-> moon switch), the eye
// and its step (mm), whether the sun is ray traced this frame (mSunRtNow, frame-info
// shadow_params.w: 2 = RT, 1 = cascades -- an alternation would swap shadow techniques
// frame by frame), the lit variant, the mask state / ping-pong set / history validity,
// and the temporal / jitter state. A per-frame A / B alternation (which a TAA / FSR
// resolve would average away) shows as alternating rows. Host-side only, no GPU readback.
void LLCleanRendererVK::rtSunDiagLog()
{
    U32 n = 0;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<U32> s_n(gSavedSettings, "RenderVKRTShadowSunDiag", 0U);
    n = std::min<U32>(s_n, 2000u);
#endif
    if (n != mRtSunDiagLatched)
    {
        mRtSunDiagLatched = n;
        mRtSunDiagLeft = n;
        mRtSunDiagPrevOk = false;
    }
    if (mRtSunDiagLeft == 0) return;
    --mRtSunDiagLeft;
    const float* ld = &mAtmos[7 * 4];
    const float* eye = mLightInfoCPU.cam_pos;
    double ddeg = 0.0, deye = 0.0;
    if (mRtSunDiagPrevOk)
    {
        const double l0 = std::sqrt((double)ld[0] * ld[0] + (double)ld[1] * ld[1] + (double)ld[2] * ld[2]);
        const double l1 = std::sqrt((double)mRtSunDiagPrevDir[0] * mRtSunDiagPrevDir[0]
                                    + (double)mRtSunDiagPrevDir[1] * mRtSunDiagPrevDir[1]
                                    + (double)mRtSunDiagPrevDir[2] * mRtSunDiagPrevDir[2]);
        const double c = ((double)ld[0] * mRtSunDiagPrevDir[0] + (double)ld[1] * mRtSunDiagPrevDir[1]
                          + (double)ld[2] * mRtSunDiagPrevDir[2]) / std::max(l0 * l1, 1e-30);
        ddeg = std::acos(std::clamp(c, -1.0, 1.0)) * 57.29577951;
        const double dx = (double)eye[0] - mRtSunDiagPrevEye[0], dy = (double)eye[1] - mRtSunDiagPrevEye[1],
                     dz = (double)eye[2] - mRtSunDiagPrevEye[2];
        deye = std::sqrt(dx * dx + dy * dy + dz * dz) * 1000.0;
    }
    for (int i = 0; i < 3; ++i) { mRtSunDiagPrevDir[i] = ld[i]; mRtSunDiagPrevEye[i] = eye[i]; }
    mRtSunDiagPrevOk = true;
    LL_INFOS("VKShadow") << std::fixed << std::setprecision(7)
                         << "#674 sun diag [" << (mRtSunDiagLatched - mRtSunDiagLeft) << "/" << mRtSunDiagLatched << "]"
                         << " taaFrame " << mTaaFrame << (mOffscreen ? " OFFSCREEN" : "")
                         << " | light (" << ld[0] << ", " << ld[1] << ", " << ld[2] << ") step " << ddeg << " deg"
                         << " colour sum " << (mAtmos[8] + mAtmos[9] + mAtmos[10])
                         << " | eye (" << eye[0] << ", " << eye[1] << ", " << eye[2] << ") step " << deye << " mm"
                         << " | sunRT " << mSunRtNow << " shadow_params.w " << mShadowParams[3] << " litRT " << mLitRt
                         << " | mask on " << mRtMaskOn << " set " << (mRtMaskSet & 1u) << " " << mRtMaskW << "x" << mRtMaskH
                         << " scale " << mRtMaskScale << " denoise " << mRtMaskDenoise << " histOk " << mRtMaskHistOk
                         << " | temporal " << mTemporalOn << " camHist " << !mTaaResolveReset
                         << " jitter " << mTaaJitterOn << " (" << mTaaJitter[0] << ", " << mTaaJitter[1] << ")"
                         << std::defaultfloat << LL_ENDL;
}

// #666: the mask's temporal history without TAA / FSR (ensureTemporal keeps the motion /
// previous-instance plumbing for it). RenderVKRTShadowTemporalAlways 0 = the old coupling
// (history only while TAA / FSR jitter the view; A/B).
bool LLCleanRendererVK::rtMaskWantsTemporal() const
{
    bool on = true;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<bool> s_on(gSavedSettings, "RenderVKRTShadowTemporalAlways", true);
    on = s_on;
#endif
    return on && mRtMaskDenoise && mRtMaskW > 1 && mRtMaskH > 1;
}

bool LLCleanRendererVK::rtMaskSpatialDispatch(VkCommandBuffer cmd)
{
    U32 iters = 0, dbg = 0;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<U32> s_iters(gSavedSettings, "RenderVKRTShadowAtrous", 3U);
    static LLCachedControl<U32> s_dbg(gSavedSettings, "RenderVKRTShadowDebug", 0U);
    iters = std::min<U32>(s_iters, 4u);
    dbg = (U32)s_dbg <= 7u ? (U32)s_dbg : 0u;   // 8: the terminator view (#664, shader-side)
    if ((U32)s_dbg == 9u || (U32)s_dbg == 10u) dbg = 6u;   // #674: the temporal pass wrote it; pass it through as 6
    if ((U32)s_dbg == 11u) dbg = 1u;   // the self-hit detector: the trace wrote it into the raw view
    if ((U32)s_dbg == 12u) dbg = 1u;   // the terrain raster-under-RT gap (#211): likewise the raw view
#endif
    // #870 raw routing (DLSS RR reconstructs): the final sun mask is this frame's raw trace
    // (debug view 1's pass-through; the temporal pass still runs, so the history is warm when RR
    // stops). Projector slots in the mask stay denoised (the stochastic path owns them when RR
    // matters: RenderVKProjStochastic).
    if (rrRawSignal(kRrRawMask, mRtMaskW, mRtMaskH) && dbg == 0u) dbg = 1u;
    if (!mRtMaskDenoise) iters = 0;   // legacy blend: the plain copy pass
    if (iters == 0 && dbg == 0) return false;   // the plain disc pass (rtMaskStage)
    // The scratch images must exist at the mask size (both slots are always allocated
    // together with the mask, rtMaskBytesAt).
    if (iters > 0 && (!mRtMaskHist[0].view || !mRtMaskHist[4].view || !mRtMaskAccSun.view || !mRtMaskMeta.view))
        iters = 0;

    // (set 4: the #771 cache's carry bitmap -- the spatial pass skips carried blocks)
    const VkDescriptorSet sets[5] = {mDsetDraw0, mDsetDraw1, mRT->consumerSet(), mDsetRtMask[mRtMaskSet & 1u],
                                     mDsetRtCache[mFrameSlot & 1u]};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mRtMaskLayout, 0, sets[4] ? 5u : 4u, sets, 0, nullptr);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mRtMaskSpPipe);
    const float cap = mRtMaskPush.p1[2];   // RenderVKRTShadowSpatial (0..16)
    const uint32_t gx = (mRtMaskW + 7u) / 8u, gy = (mRtMaskH + 7u) / 8u;
    const uint32_t n = std::max<uint32_t>(iters, 1u);
    for (uint32_t k = 0; k < n; ++k)
    {
        if (k > 0)
        {
            // Iteration k - 1's scratch writes -> iteration k's neighbourhood reads.
            VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; d.memoryBarrierCount = 1; d.pMemoryBarriers = &mb;
            vkCmdPipelineBarrier2(cmd, &d);
        }
        // clean_rt_mask_spatial.comp: p1.z = cap + 32 x (1 + k + 8 N + 64 debug).
        const uint32_t code = 1u + k + 8u * iters + 64u * dbg;
        RtMaskPushData push = mRtMaskPush;
        push.p1[2] = cap + 32.f * (float)code;
        if (mRtCacheNow) push.p0[1] = (float)((uint32_t)std::lround(mRtMaskPush.p0[1]) | (1u << 18));   // #771 carry
        VK_PUSH_BUDGET_CHECK(push);
        vkCmdPushConstants(cmd, mRtMaskLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, gx, gy, viewCount());   // (#1075: every view's layer, z = N)
    }
    gpuSubMark(cmd, "c_rtmask_sp");
    return true;
}
