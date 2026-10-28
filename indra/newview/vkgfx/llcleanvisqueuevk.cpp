/**
 * @file llcleanvisqueuevk.cpp
 * @brief THE VISIBILITY QUEUE (#1039 M1; RenderVKVisQueue, default 0): the host side. The shader side and
 *        the data contract are in indra/vkclean/shaders/clean_vis_queue.glsl.
 *
 * WHAT IT DOES (bit 1, M1): the RT hit shading's SUN rays leave the lighting kernels. The hit-shading
 * splits of #1036 run, per consumer,
 *   material stage -> lighting stage
 * and the lighting stage walked the sun ray (opaque / translucent / foliage, the walk library's single-ray
 * core) with the whole lit surface live around it: its register peak. With the queue:
 *   MAT-VQ (producer: the record + the sun ray as a 48 B request, or a constant result) ->
 *   WALK (one lane per request, nothing but the walk: DeferVisWalk) ->
 *   LIGHT-VQ (resolve: the sun from the 16 B result -- the kernel holds no sun walk)
 * for the GI (HIT-MAT -> WALK -> HIT-LIGHT, recordGI) and the RT reflections (MAT -> WALK -> LIGHT,
 * recordRtReflect). The walk runs the same function on the same ray, so the result is bit-identical.
 *
 * BIT 2 (M2; the pipeline SET 1, which supersedes set 0 when both bits are on): the rest of the hit rays leave
 * the lighting kernels too. The producer (DeferRtSplitMatVq2 / DeferGiSplitHitMatVq2) also queues
 *   * the RT reflection hits' OCCLUSION ray (rtrHitEnv; GI frames with RenderVKRTReflectionsHitOcclusion),
 *   * the hit shadow-ray budget's K projector / point-light rays: it runs the local-light walk itself with every
 *     share unshadowed (the sum + the picks into result slots), the walk leaves each pick's blocked part,
 *     the resolve subtracts them -- the lighting stage holds no light walk either;
 * and the RT split gains a LAYERS stage (DeferRtSplitLayersVq2: the blend-layer gather + its per-layer sun
 * rays per entry, beside the walk) whose composite the resolve finishes. DeferRtSplitLightVq2 and
 * DeferGiSplitHitLightVq2 then hold no ray query. Same image (the same functions on the same rays and cones).
 * The M2 producer is a stage of its own (REQ: DeferRtSplitReqVq2 / DeferGiSplitHitReqVq2, after the material
 * stage, one barrier between): it reads the RT LIT RECORD back and queues, so the material kernels
 * (DeferRtSplitMatVq2 / DeferGiSplitHitMatVq2) hold only the material -- fused, the two working sets cost
 * 165 / 149 registers (12 warps / SM); split, each stage is 128 (16).
 *
 * BIT 4 (M3; pipeline SET 2, beside either of the above): the GI split's REUSE stage stops walking its reused
 * picks' visibility rays. recordGI runs
 *   REUSE-VQ (DeferGiSplitReuseVq: the reservoir reuse; a pick that needs its visibility ray queues it and
 *             leaves the texel, as a re-shade pick does; its pick stored in the texel's GI record) -> WALK (+ RESHADE
 *             beside it) ->
 *   the re-shade TAIL (the same binary: the pick re-derived) and the VIS TAIL (DeferGiSplitVisTail over the queue's
 *             requests: the stored pick finished with the result, no candidate loop) -- REUSE-VQ holds no ray query.
 * Region VQR_GIR: one 48 B request + one 16 B slot per GI texel (~59 MB at 1440p), taken first and whole: a frame
 * it does not fit (the cap / pressure) runs the fused REUSE. Same accept / reject decisions (the same ray, cone
 * and alpha-test state through the walk library's one walk).
 *
 * BIT 8 (pipeline SET 3, beside any of the above): pass 15's split shadow core (RenderVKShadowSplitKernels) --
 * SUNVIS (the sun / moon sample, 255 registers) and VIS (the gated light samples, ~230) each walked their
 * sample with the receiver rebuild, the probes and the sample state around it (#1031 parked the ray inputs in
 * shared memory to survive: on RDNA that LDS sat beside the traversal stack). recordProjStoch runs
 *   SELECT -> GEN (DeferShadowQSunGen / DeferShadowQVisGen: the kernels up to the walk -- the sample's
 *             ray-generation inputs + walk state as a COMPACT request, 64 / 48 B: what the walk cannot rebuild
 *             from the texel / the light index -- clean_vis_queue.glsl SHADOW CORE REQUESTS) ->
 *   WALK (DeferShadowQSunWalk[Iso] / DeferShadowQVisWalk[Iso]: the sample's rays from the request -- no
 *             receiver, no shared memory -- then the kernel's own store) + VIS's TAIL (DeferShadowQVisTail[Iso]:
 *             VIS over the jobs past the region) -> SHADE (unchanged).
 * Regions VQR_SHSUN (every denoiser texel, whole or none: else the fused SUNVIS) and VQR_SHVIS (kVqShJobsPerTexel
 * jobs per texel, within the cap: the TAIL serves the rest). Same samples, rays and stores (THE SHADOW CORE below).
 *
 * BIT 16 (M6; no pipelines of its own, beside any of the above): THE FRAME WALK below -- the lighting chain records
 * every queued consumer's producers first, then ONE walk wave over all their requests (two barriers in all instead
 * of two per consumer, the region headers in one barrier pair), then the resolves.
 *
 * FRAME (every step on the consumer's own queue segment -- q_defer_light / q_gi_lag -- no new semaphore):
 *   1. visQueueDecide (recordWorld, after giDecide and deferSplitFrame's rtSplitDecide): the setting, the
 *      regions' entries (each consumer's own: the RT records' capacity, the GI texels; within the cap),
 *      the buffer (allocated first, the old one retired -- no device wait), the stats harvest;
 *   2. visQueueBegin (each consumer, before its producer): the region header (counters zeroed, the walk's
 *      indirect dispatch (0, 0, 1), the capacities, the RT records' address);
 *   3. the producer appends (subgroup-aggregated: one atomic per subgroup) and raises the dispatch;
 *   4. visQueueWalk: the walk, indirect over the region's requests;
 *   5. the resolve -- and, for the entries past the region's capacity, the consumer's fused lighting stage
 *      as a TAIL (push rtr3.y = the first such entry): the queue never drops a ray.
 * VRAM: 256 B per region + per entry R x 48 B requests and S x 16 B result slots -- set 0: 64 B; set 1: RT
 * (2 + K) x 48 + (4 + K) x 16 (K = RenderVKRTReflectionsHitShadowRays; 224 B at K 1), GI (1 + K) x 48 +
 * (2 + K) x 16 (K = RenderVKGIHitShadowRays; 144 B at K 1); set 3 64 B per denoiser texel (sun) + 48 B per
 * light-sample job (2 per texel) -- capped by RenderVKVisQueueMB (0 = the tier of the device-local heap's size:
 * 896 MB above 24 GB, 192 MB above 12.5 GB, 64 MB above 8.5 GB, else 32 MB). Regions in order: the GI reuse (whole),
 * the RT / GI hits (by demand), the shadow sun (whole), the shadow light samples (the rest). At 1440p (GI at half extent,
 * the RT records at the default ray budget: ~0.92 M entries each): set 0 ~59 MB per consumer, set 1 ~206 MB
 * RT + ~133 MB GI; set 3 (a 1280 x 720 denoiser) ~59 MB sun + ~88 MB light samples. Fixed by the extent / the
 * settings: no growth. Under texture pressure no new buffer is
 * allocated (the consumers stay fused); a refusal (#697) retries in 30 s.
 * GPU timers: c_/g_defer_rtrefl_walk and c_/g_gi_walk (the walks); the material / lighting marks keep their
 * names (now: + the requests / the resolve and its tail; set 1: the requests under c_/g_defer_rtrefl_req and
 * c_/g_gi_hitreq). Counters (GPU timers on): per region requests,
 * constants, entries past the cap -- logged every 10 s ("VK vis queue: ...").
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
#include "vk_common.h"
#include "vk_pipeline.h"
#include "vk_world_shaders_spv.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <sstream>

namespace
{
double vqClock()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
VkShaderModule vqModule(VkDevice dev, const uint32_t* code, size_t bytes, const char* name)
{
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(dev, &ci, nullptr, &m));
    vkNoteShaderModule(m, code, bytes, name);
    return m;
}
// The lighting kernels' debug specialization OFF (as llcleangivk.cpp giLitSpec).
const VkSpecializationInfo* vqLitSpec()
{
    static const VkBool32 kOff = VK_FALSE;
    static const VkSpecializationMapEntry kEntry{1u, 0u, sizeof(VkBool32)};
    static const VkSpecializationInfo kSiOff{1u, &kEntry, sizeof(VkBool32), &kOff};
    return &kSiOff;
}
constexpr VkDeviceSize kVqAlign = 256u;
VkDeviceSize vqAlignUp(VkDeviceSize v) { return (v + kVqAlign - 1u) & ~(kVqAlign - 1u); }
constexpr uint32_t kVqStatBytes = 5u * 16u;   // the region header's words 0-4 (clean_vis_queue.glsl)
} // namespace

// The pipeline sets a RenderVKVisQueue mask asks for: bit s = set s (set 0 = M1, bit 1; set 1 = M2, bit 2;
// set 2 = M3, bit 4 -- the GI reuse, independent of the other two; set 3 = bit 8, the shadow core).
static uint32_t vqSetsOf(U32 mask)
{
    return ((mask & 1u) != 0u ? 1u : 0u) | ((mask & 2u) != 0u ? 2u : 0u) | ((mask & 4u) != 0u ? 4u : 0u)
         | ((mask & 8u) != 0u ? 8u : 0u);
}
constexpr uint32_t kVqSets = 4u;
// After the RT layout exists (initDeferSplit's RT block, inside the world batch): the walk + the producer /
// resolve variants of the RT split and the GI shade split (distinct module names: RenderVKPipelineStats
// logs each one's registers) -- each set queued only while its RenderVKVisQueue bit is on (setMask 0: the
// setting's, at init; else the sets the background build of visQueueDecide asks for, inside its own batch).
void LLCleanRendererVK::visQueueInit(VkDevice dev, uint32_t setMask)
{
    if (!mRtReflLayout) return;
    if (setMask == 0u)
    {
        U32 mask = 0;
#if defined(LL_HAS_CLEAN_PIPELINE)
        static LLCachedControl<U32> s_vq(gSavedSettings, "RenderVKVisQueue", 0U);
        mask = s_vq;
#endif
        setMask = vqSetsOf(mask);
        if (setMask == 0u) return;   // (off: nothing compiles for it)
    }
    struct Stage { uint32_t set; const uint32_t* code; size_t bytes; const char* mod; const char* name; VkPipeline* out; };
    const Stage st[] = {
        {7u, kWorldDeferVisWalkSpv, kWorldDeferVisWalkSpv_bytes, "DeferVisWalk", "mVisWalkPipe (DeferVisWalk)", &mVisWalkPipe},
        // (#1039 M3) set 2
        {4u, kWorldDeferGiSplitReuseVqSpv, kWorldDeferGiSplitReuseVqSpv_bytes, "DeferGiSplitReuseVq",
         "mGirVqPipe (DeferGiSplitReuseVq)", &mGirVqPipe},
        {4u, kWorldDeferGiSplitVisTailSpv, kWorldDeferGiSplitVisTailSpv_bytes, "DeferGiSplitVisTail",
         "mGirVisTailPipe (DeferGiSplitVisTail)", &mGirVisTailPipe},
        {1u, kWorldDeferRtSplitMatVqSpv, kWorldDeferRtSplitMatVqSpv_bytes, "DeferRtSplitMatVq", "mRtVqPipe (DeferRtSplitMatVq)",
         &mRtVqPipe[0][VQS_MAT]},
        {1u, kWorldDeferRtSplitLightVqSpv, kWorldDeferRtSplitLightVqSpv_bytes, "DeferRtSplitLightVq",
         "mRtVqPipe (DeferRtSplitLightVq)", &mRtVqPipe[0][VQS_LIGHT]},
        {1u, kWorldDeferGiSplitHitMatVqSpv, kWorldDeferGiSplitHitMatVqSpv_bytes, "DeferGiSplitHitMatVq",
         "mGiVqPipe (DeferGiSplitHitMatVq)", &mGiVqPipe[0][VQS_MAT]},
        {1u, kWorldDeferGiSplitHitLightVqSpv, kWorldDeferGiSplitHitLightVqSpv_bytes, "DeferGiSplitHitLightVq",
         "mGiVqPipe (DeferGiSplitHitLightVq)", &mGiVqPipe[0][VQS_LIGHT]},
        // (#1039 M2) set 1
        {2u, kWorldDeferRtSplitMatVq2Spv, kWorldDeferRtSplitMatVq2Spv_bytes, "DeferRtSplitMatVq2",
         "mRtVqPipe (DeferRtSplitMatVq2)", &mRtVqPipe[1][VQS_MAT]},
        {2u, kWorldDeferRtSplitReqVq2Spv, kWorldDeferRtSplitReqVq2Spv_bytes, "DeferRtSplitReqVq2",
         "mRtVqPipe (DeferRtSplitReqVq2)", &mRtVqPipe[1][VQS_REQ]},
        {2u, kWorldDeferRtSplitLayersVq2Spv, kWorldDeferRtSplitLayersVq2Spv_bytes, "DeferRtSplitLayersVq2",
         "mRtVqPipe (DeferRtSplitLayersVq2)", &mRtVqPipe[1][VQS_LAYERS]},
        {2u, kWorldDeferRtSplitLightVq2Spv, kWorldDeferRtSplitLightVq2Spv_bytes, "DeferRtSplitLightVq2",
         "mRtVqPipe (DeferRtSplitLightVq2)", &mRtVqPipe[1][VQS_LIGHT]},
        {2u, kWorldDeferGiSplitHitMatVq2Spv, kWorldDeferGiSplitHitMatVq2Spv_bytes, "DeferGiSplitHitMatVq2",
         "mGiVqPipe (DeferGiSplitHitMatVq2)", &mGiVqPipe[1][VQS_MAT]},
        {2u, kWorldDeferGiSplitHitReqVq2Spv, kWorldDeferGiSplitHitReqVq2Spv_bytes, "DeferGiSplitHitReqVq2",
         "mGiVqPipe (DeferGiSplitHitReqVq2)", &mGiVqPipe[1][VQS_REQ]},
        {2u, kWorldDeferGiSplitHitLightVq2Spv, kWorldDeferGiSplitHitLightVq2Spv_bytes, "DeferGiSplitHitLightVq2",
         "mGiVqPipe (DeferGiSplitHitLightVq2)", &mGiVqPipe[1][VQS_LIGHT]},
        // (#1039 bit 8) set 3: pass 15's shadow core -- GEN / WALK per sample kind (the walks in both LOD modes,
        // RenderVKShadowIsoLod) + VIS's TAIL (likewise)
        {8u, kWorldDeferShadowQSunGenSpv, kWorldDeferShadowQSunGenSpv_bytes, "DeferShadowQSunGen",
         "mShVqPipe (DeferShadowQSunGen)", &mShVqPipe[VQSH_SUN_GEN]},
        {8u, kWorldDeferShadowQSunWalkSpv, kWorldDeferShadowQSunWalkSpv_bytes, "DeferShadowQSunWalk",
         "mShVqPipe (DeferShadowQSunWalk)", &mShVqPipe[VQSH_SUN_WALK]},
        {8u, kWorldDeferShadowQSunWalkIsoSpv, kWorldDeferShadowQSunWalkIsoSpv_bytes, "DeferShadowQSunWalkIso",
         "mShVqPipe (DeferShadowQSunWalkIso)", &mShVqPipe[VQSH_SUN_WALK_ISO]},
        {8u, kWorldDeferShadowQVisGenSpv, kWorldDeferShadowQVisGenSpv_bytes, "DeferShadowQVisGen",
         "mShVqPipe (DeferShadowQVisGen)", &mShVqPipe[VQSH_VIS_GEN]},
        {8u, kWorldDeferShadowQVisWalkSpv, kWorldDeferShadowQVisWalkSpv_bytes, "DeferShadowQVisWalk",
         "mShVqPipe (DeferShadowQVisWalk)", &mShVqPipe[VQSH_VIS_WALK]},
        {8u, kWorldDeferShadowQVisWalkIsoSpv, kWorldDeferShadowQVisWalkIsoSpv_bytes, "DeferShadowQVisWalkIso",
         "mShVqPipe (DeferShadowQVisWalkIso)", &mShVqPipe[VQSH_VIS_WALK_ISO]},
        {8u, kWorldDeferShadowQVisTailSpv, kWorldDeferShadowQVisTailSpv_bytes, "DeferShadowQVisTail",
         "mShVqPipe (DeferShadowQVisTail)", &mShVqPipe[VQSH_VIS_TAIL]},
        {8u, kWorldDeferShadowQVisTailIsoSpv, kWorldDeferShadowQVisTailIsoSpv_bytes, "DeferShadowQVisTailIso",
         "mShVqPipe (DeferShadowQVisTailIso)", &mShVqPipe[VQSH_VIS_TAIL_ISO]}};
    for (uint32_t s = 0; s < kVqSets; ++s)
        if ((setMask & (1u << s)) != 0u) mVqPipesQueued[s] = true;
    for (const Stage& s : st)
    {
        if ((s.set & setMask) == 0u || *s.out) continue;
        VkShaderModule m = vqModule(dev, s.code, s.bytes, s.mod);
        createComputePipelineInto(dev, m, mRtReflLayout, 0u, vqLitSpec(), s.out, s.name);
        destroyShaderModuleDeferred(dev, m, nullptr);
    }
}

void LLCleanRendererVK::visQueueDestroy()
{
    if (!mCtx) return;
    mVqJob.wait();   // (a background build writes these handles)
    VkDevice dev = mCtx->device();
    if (mVisWalkPipe) { vkDestroyPipeline(dev, mVisWalkPipe, nullptr); mVisWalkPipe = VK_NULL_HANDLE; }
    for (uint32_t s = 0; s < 2u; ++s)
        for (uint32_t k = 0; k < VQS_COUNT; ++k)
            for (VkPipeline* q : {&mRtVqPipe[s][k], &mGiVqPipe[s][k]})
                if (*q) { vkDestroyPipeline(dev, *q, nullptr); *q = VK_NULL_HANDLE; }
    if (mGirVqPipe) { vkDestroyPipeline(dev, mGirVqPipe, nullptr); mGirVqPipe = VK_NULL_HANDLE; }
    if (mGirVisTailPipe) { vkDestroyPipeline(dev, mGirVisTailPipe, nullptr); mGirVisTailPipe = VK_NULL_HANDLE; }
    for (VkPipeline& p : mShVqPipe)
        if (p) { vkDestroyPipeline(dev, p, nullptr); p = VK_NULL_HANDLE; }
    if (mVq.buffer) { mCtx->destroyBuffer(mVq); mVq = GpuBuffer{}; }
    if (mVqStatRb.buffer) { mCtx->destroyBuffer(mVqStatRb); mVqStatRb = GpuBuffer{}; }
    for (auto& s : mVqStatUsed)
        for (bool& u : s) u = false;
    for (VisQueueRegion& r : mVqRegion) r = VisQueueRegion{};
    mVqRtNow = mVqGiNow = mVqGirNow = false;
    mVqShSunNow = mVqShVisNow = false;
    mVqM6Now = false;
    visQueueM6End();
    mVqSet = 0;
    for (bool& q : mVqPipesQueued) q = false;
}

// recordWorld, after giDecide (the GI shade split, its extent) and deferSplitFrame (the RT split, its
// records): which consumers queue their hit sun rays this recording, the regions, the buffer.
void LLCleanRendererVK::visQueueDecide()
{
    mVqRtNow = mVqGiNow = mVqGirNow = false;
    mVqShSunNow = mVqShVisNow = false;
    mVqM6Now = false;
    visQueueM6End();   // (nothing collected may cross into this recording)
    if (!mCtx || mOffscreen) return;   // (offscreen recordings run neither split: the buffer stays as it is)
    const double now = vqClock();
    U32 mask = 0, capMB = 0;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<U32> s_vq(gSavedSettings, "RenderVKVisQueue", 0U);
    static LLCachedControl<U32> s_mb(gSavedSettings, "RenderVKVisQueueMB", 0U);
    mask = s_vq;
    capMB = s_mb;
#endif
    // ---- the counters of the recording 3 slots ago (frames in flight <= 2) ----
    mVqStatSlot = (mVqStatSlot + 1u) % 3u;
    for (uint32_t r = 0; r < VQR_COUNT; ++r)
    {
        if (!mVqStatUsed[mVqStatSlot][r]) continue;
        mVqStatUsed[mVqStatSlot][r] = false;
        if (!mVqStatRb.mapped) continue;
        const uint32_t* h = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(mVqStatRb.mapped)
                                                               + ((size_t)mVqStatSlot * VQR_COUNT + r) * kVqStatBytes);
        mVqStatAcc.frames[r] += 1.0;
        mVqStatAcc.req[r] += h[0];
        mVqStatAcc.peak[r] = std::max(mVqStatAcc.peak[r], h[0]);
        mVqStatAcc.wanted[r] += h[12];
        mVqStatAcc.consts[r] += h[13];
        mVqStatAcc.past[r] += h[14];
        mVqStatAcc.refused[r] += h[15];
        mVqStatAcc.occ[r] += h[16];        // (set 1: header word 4)
        mVqStatAcc.lightRays[r] += h[17];
        mVqStatAcc.litHits[r] += h[18];
    }
    if (now - mVqStatAcc.lastLog >= 10.0)
    {
        bool any = false;
        for (uint32_t r = 0; r < VQR_COUNT; ++r) any = any || mVqStatAcc.frames[r] > 0.0;
        if (any)
        {
            static const char* kName[VQR_COUNT] = {"RT reflections", "GI", "GI reuse", "shadow sun", "shadow light samples"};
            std::ostringstream os;
            os << "VK vis queue (#1039): buffer " << ((mVq.size + (1u << 19)) >> 20) << " MB of cap " << (mVqCapBytes >> 20) << " MB";
            for (uint32_t r = 0; r < VQR_COUNT; ++r)
            {
                const double f = mVqStatAcc.frames[r];
                if (!(f > 0.0)) continue;
                if (r == VQR_GIR)
                {
                    // (M3: one request per texel whose reused pick needs its visibility ray; the region holds
                    //  every texel, so nothing is past it -- a frame it does not fit runs the fused REUSE)
                    os << " | " << kName[r] << ": " << mVqRegion[r].entries << " texels, per frame "
                       << (uint64_t)(mVqStatAcc.req[r] / f + 0.5) << " visibility requests (peak " << mVqStatAcc.peak[r]
                       << ")";
                    if (mVqStatAcc.refused[r] > 0.0) os << ", " << (uint64_t)mVqStatAcc.refused[r] << " refused";
                    continue;
                }
                if (r == VQR_SHSUN || r == VQR_SHVIS)
                {
                    // (bit 8: one request per sample that needs its walk; the producer stores the rest itself. The
                    //  light samples: the job list's length (header word 4.x, in the occ accumulator) and the jobs
                    //  past the region -- the VIS tail's)
                    os << " | " << kName[r] << ": " << mVqRegion[r].entries << (r == VQR_SHSUN ? " texels" : " jobs")
                       << ", per frame " << (uint64_t)(mVqStatAcc.req[r] / f + 0.5) << " requests (peak " << mVqStatAcc.peak[r]
                       << "), " << (uint64_t)(mVqStatAcc.consts[r] / f + 0.5) << " of "
                       << (uint64_t)(mVqStatAcc.wanted[r] / f + 0.5) << " samples without a ray";
                    if (r == VQR_SHVIS)
                        os << ", " << (uint64_t)(mVqStatAcc.occ[r] / f + 0.5) << " jobs, "
                           << (uint64_t)(mVqStatAcc.past[r] / f + 0.5) << " past the region (VIS tail)";
                    if (mVqStatAcc.refused[r] > 0.0) os << ", " << (uint64_t)mVqStatAcc.refused[r] << " refused";
                    continue;
                }
                os << " | " << kName[r] << ": " << mVqRegion[r].entries << " entries, per frame "
                   << (uint64_t)(mVqStatAcc.req[r] / f + 0.5) << " sun requests (peak " << mVqStatAcc.peak[r] << "), "
                   << (uint64_t)(mVqStatAcc.consts[r] / f + 0.5) << " of " << (uint64_t)(mVqStatAcc.wanted[r] / f + 0.5)
                   << " sun-facing hits without a ray, " << (uint64_t)(mVqStatAcc.past[r] / f + 0.5)
                   << " past the cap (fused tail)";
                if (mVqStatAcc.litHits[r] > 0.0 || mVqStatAcc.occ[r] > 0.0)
                    os << "; " << (uint64_t)(mVqStatAcc.occ[r] / f + 0.5) << " occlusion requests, "
                       << (uint64_t)(mVqStatAcc.lightRays[r] / f + 0.5) << " light-ray requests (budget K "
                       << mVqRegion[r].budgetK << ") from " << (uint64_t)(mVqStatAcc.litHits[r] / f + 0.5)
                       << " light walks";
                if (mVqStatAcc.refused[r] > 0.0) os << ", " << (uint64_t)mVqStatAcc.refused[r] << " refused";
            }
            LL_INFOS("Vulkan") << os.str() << LL_ENDL;
        }
        const double t = now;
        mVqStatAcc = VqStatAcc{};
        mVqStatAcc.lastLog = t;
    }

    // ---- the pipelines: switched on after init, they compile in the background (the frame never waits on a
    //      cold compile); the consumers stay fused until every one landed ----
    if (mVqJob.pending()) mVqJob.poll();
    const uint32_t sets = vqSetsOf(mask);
    uint32_t queued = 0u;
    for (uint32_t s = 0; s < kVqSets; ++s) queued |= mVqPipesQueued[s] ? (1u << s) : 0u;
    const uint32_t missing = sets & ~queued;
    if (missing != 0u && !mVqJob.pending() && mRtReflLayout)
    {
        VkDevice dev = mCtx->device();
        {
            PipelineBatchScope scope(dev);
            visQueueInit(dev, missing);
            PipelineJobOpts o;
            o.what = "visibility queue";
            o.gate = false;   // (optional: never holds the login screen)
            mVqJob.start(pipelineBatchFlushAsync(dev, o));
        }
        LL_INFOS("Vulkan") << "VK vis queue (#1039): pipelines (set mask " << missing
                           << ") compiling in the background (the queue starts when they land)" << LL_ENDL;
    }
    // ---- this recording's consumers: set 1 (bit 2, M2) where asked and built, else set 0 (bit 1, M1) ----
    const bool jobFree = !mVqJob.pending();
    const bool set1 = (sets & 2u) != 0u && jobFree && vqSetReady(1u);
    const bool set0 = !set1 && (sets & 1u) != 0u && jobFree && vqSetReady(0u);
    const uint32_t vs = set1 ? 1u : 0u;
    const bool on = set0 || set1;
    const bool wantRt = on && mRtSplitNow && mRtRec.buffer && mRtRecCap != 0u;
    const bool wantGi = on && mGiNow && mGiShadeSplitNow && mGiDn;
    // (M3, bit 4: set 2, beside either of the others) the GI split's REUSE: every GI texel
    const bool wantGir = (sets & 4u) != 0u && jobFree && vqSetReady(2u) && mGiNow && mGiSplitNow && mGiDn;
    // (bit 8: set 3, beside any of the others) pass 15's split shadow core this recording (pdnFrame decided the
    // sampling and the denoiser's extent; recordProjStoch runs the split when its buffers match that extent): the
    // light samples always, the sun when the merged pass is about to run (recordWorld settles mShadowMergeNow
    // right after this from the same terms)
    // (the light-sample request packs the texel in 23 bits: a larger denoiser stays fused)
    const bool shCore = (sets & 8u) != 0u && jobFree && vqSetReady(3u) && mPdnNow && mPdn && mPdn->allocated()
                     && pdnSplitWanted() && mPdnSel.buffer && mPdnVis.buffer && mPdnSelW == mPdn->width()
                     && mPdnSelH == mPdn->height() && mPdnVisSlots >= pdnVisSlotsWanted()
                     && (uint64_t)mPdn->width() * mPdn->height() < (1ull << 23);
    const uint64_t shTexels = shCore ? (uint64_t)mPdn->width() * mPdn->height() : 0u;
    const bool shSun = shCore && mShadowMergeTry && mDeferSplitNow && mPdn->hasSun() && mPdnMergePipe != VK_NULL_HANDLE;
    const uint64_t want[VQR_COUNT] = {wantRt ? (uint64_t)mRtRecCap : 0u,
                                      wantGi ? (uint64_t)mGiDn->width() * mGiDn->height() : 0u,
                                      wantGir ? (uint64_t)mGiDn->width() * mGiDn->height() : 0u,
                                      shSun ? shTexels : 0u,
                                      shTexels * std::min<uint32_t>(kVqShJobsPerTexel, std::max<uint32_t>(mPdnVisSlots, 1u))};
    // per entry: R requests (the most it can ask: never refused) + S result slots. Set 1's hit shadow-ray
    // budget K = the one the consumer's push carries this recording (RenderVKRTReflectionsHitShadowRays,
    // RenderVKGIHitShadowRays; 4 = the shaders' RT_SHADE_PROJ_MAXK). The GI reuse: one visibility ray + its
    // slot per texel. The shadow core: one request per entry (a texel's sun sample / a light-sample job), no slot.
    uint32_t reqPer[VQR_COUNT] = {1u, 1u, 1u, 1u, 1u}, slots[VQR_COUNT] = {1u, 1u, 1u, 0u, 0u},
             budgetK[VQR_COUNT] = {0u, 0u, 0u, 0u, 0u};
    if (set1)
    {
        U32 giK = 1;
#if defined(LL_HAS_CLEAN_PIPELINE)
        static LLCachedControl<U32> s_gik(gSavedSettings, "RenderVKGIHitShadowRays", 1U);
        giK = s_gik;
#endif
        budgetK[VQR_RT] = std::min<uint32_t>(mRtReflHitShadows, 4u);
        budgetK[VQR_GI] = std::min<uint32_t>(giK, 4u);
        reqPer[VQR_RT] = 2u + budgetK[VQR_RT];   // sun, occlusion, K lights
        slots[VQR_RT] = 4u + budgetK[VQR_RT];    // sun, light sum, K, occlusion, blend layers
        reqPer[VQR_GI] = 1u + budgetK[VQR_GI];   // sun, K lights
        slots[VQR_GI] = 2u + budgetK[VQR_GI];    // sun, light sum, K
    }
    uint64_t entryBytes[VQR_COUNT];
    for (uint32_t r = 0; r < VQR_COUNT; ++r) entryBytes[r] = (uint64_t)reqPer[r] * kVqReqBytes + (uint64_t)slots[r] * kVqSlotBytes;
    entryBytes[VQR_SHSUN] = kVqShSunBytes;   // (the shadow core's sample requests: clean_vis_queue.glsl)
    entryBytes[VQR_SHVIS] = kVqShVisBytes;
    bool wantAny = false;
    for (uint32_t r = 0; r < VQR_COUNT; ++r) wantAny = wantAny || want[r] != 0u;
    if (!wantAny)
    {
        // (off / nothing to queue: freed -- a transient stand-down re-allocates in the next queued frame)
        if (mVq.buffer)
        {
            retireBuffer(mVq);
            mVq = GpuBuffer{};
            LL_INFOS("Vulkan") << "VK vis queue (#1039): freed (no consumer queues its rays)" << LL_ENDL;
        }
        for (VisQueueRegion& r : mVqRegion) r = VisQueueRegion{};
        return;
    }
    // ---- the cap (RenderVKVisQueueMB; 0 = the VRAM tier's) and the regions within it ----
    uint64_t cap = (uint64_t)capMB << 20;
    if (cap == 0u)
    {
        // the tier from the GPU's device-local heap SIZE -- not mVramStatus.capacity, which is min(user cap, the
        // OS budget): the budget is what is free for this process right now (a 32 GB RTX 5090 with a desktop read
        // below 24 GB and got the 192 MB tier). VRAM pressure is handled by the allocation rules below (no new
        // buffer under texture pressure, #697 retry), not by the tier.
        if (mVqHeapBytes == 0u)
        {
            VkPhysicalDeviceMemoryProperties mp{};
            vkGetPhysicalDeviceMemoryProperties(mCtx->physicalDevice(), &mp);
            for (uint32_t h = 0; h < mp.memoryHeapCount; ++h)
                if ((mp.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0u)
                    mVqHeapBytes = std::max<uint64_t>(mVqHeapBytes, mp.memoryHeaps[h].size);
        }
        const uint64_t vram = mVqHeapBytes;
        const uint64_t gib = 1ull << 30;
        // (24 GB+: room for set 1 at 1440p -- ~340 MB with both consumers at K 1 -- + the GI reuse's ~59 MB + the
        //  shadow core's ~59 MB sun + ~88 MB light samples at a half-extent denoiser; allocated by demand)
        cap = vram > 24u * gib ? (896ull << 20)
            : (vram > 25u * gib / 2u ? (192ull << 20) : (vram > 17u * gib / 2u ? (64ull << 20) : (32ull << 20)));
    }
    cap = std::clamp<uint64_t>(cap, 4ull << 20, 1024ull << 20);
    mVqCapBytes = (uint32_t)std::min<uint64_t>(cap, 0xFFFFFFFFull);
    uint64_t room = cap - VQR_COUNT * (kVqHdrBytes + kVqAlign);
    uint32_t entries[VQR_COUNT] = {};
    // (M3) the GI reuse first and WHOLE: its tail re-derives each pick in the binary that derived it, so a
    // partial region would need the fused REUSE for the rest -- and a second binary deriving picks for the
    // re-shade TAIL. Every texel fits (64 B each, ~59 MB at 1440p) or the frame runs the fused REUSE.
    {
        const uint64_t girBytes = want[VQR_GIR] * entryBytes[VQR_GIR];
        const bool fits = want[VQR_GIR] != 0u && want[VQR_GIR] <= (1ull << 26) && girBytes <= room;
        if (fits)
        {
            entries[VQR_GIR] = (uint32_t)want[VQR_GIR];
            room -= girBytes;
        }
        if (want[VQR_GIR] != 0u && fits != mVqGirFit)
            LL_INFOS("Vulkan") << "VK vis queue (#1039): the GI reuse (" << want[VQR_GIR] << " texels, "
                               << ((girBytes + (1u << 19)) >> 20) << " MB) " << (fits ? "queued" : "does not fit the cap: fused")
                               << LL_ENDL;
        if (want[VQR_GIR] != 0u) mVqGirFit = fits;
    }
    // A partial region: e of want entries within room (shared by demand when demand > room), whole walk
    // workgroups; a sliver is not worth the extra dispatches -- below 4 K entries the consumer stays fused (and the
    // request / slot indices stay well inside 32 bits: <= 2^26 entries).
    auto partial = [](uint64_t want, uint64_t demand, uint64_t room) -> uint32_t
    {
        uint64_t e = want;
        if (demand > room) e = (uint64_t)((double)e * ((double)room / (double)demand));   // (the cap: shared by demand)
        e &= ~(uint64_t)63u;
        return (want != 0u && e >= 4096u) ? (uint32_t)std::min<uint64_t>(e, 1ull << 26) : 0u;
    };
    // the hit shading's regions next (RT reflections + GI hits, shared by demand): their entries past the region run
    // the fused lighting stage, at the full cost of a lighting kernel with its walks inline -- so they come before
    // the shadow core, whose fallback is its own fused kernel (a frame with bit 8 on gave the GI hits 37824 of
    // 222720 entries when the sun region was placed first: ~62 k GI requests a frame fell to the fused tail)
    {
        const uint64_t demand = want[VQR_RT] * entryBytes[VQR_RT] + want[VQR_GI] * entryBytes[VQR_GI];
        for (uint32_t r : {(uint32_t)VQR_RT, (uint32_t)VQR_GI})
        {
            entries[r] = partial(want[r], demand, room);
            room -= std::min<uint64_t>(room, (uint64_t)entries[r] * entryBytes[r]);
        }
    }
    // (bit 8) the shadow core's sun from what is left, WHOLE: one request per denoiser texel or the frame runs the
    // fused SUNVIS (a partial region would need a tail binary of its own)
    {
        const uint64_t sunBytes = want[VQR_SHSUN] * entryBytes[VQR_SHSUN];
        const bool fits = want[VQR_SHSUN] != 0u && want[VQR_SHSUN] <= (1ull << 26) && sunBytes <= room;
        if (fits)
        {
            entries[VQR_SHSUN] = (uint32_t)want[VQR_SHSUN];
            room -= sunBytes;
        }
        if (want[VQR_SHSUN] != 0u && fits != mVqShSunFit)
            LL_INFOS("Vulkan") << "VK vis queue (#1039): the shadow core's sun samples (" << want[VQR_SHSUN] << " texels, "
                               << ((sunBytes + (1u << 19)) >> 20) << " MB) "
                               << (fits ? "queued" : "do not fit the cap: fused SUNVIS") << LL_ENDL;
        if (want[VQR_SHSUN] != 0u) mVqShSunFit = fits;
    }
    // the shadow core's light samples take the remainder (the jobs past the region run VIS as its TAIL)
    entries[VQR_SHVIS] = partial(want[VQR_SHVIS], want[VQR_SHVIS] * entryBytes[VQR_SHVIS], room);
    VkDeviceSize off = 0;
    VisQueueRegion reg[VQR_COUNT];
    for (uint32_t r = 0; r < VQR_COUNT; ++r)
    {
        reg[r].wanted = (uint32_t)want[r];
        reg[r].entries = entries[r];
        reg[r].reqPerEntry = reqPer[r];
        reg[r].slots = slots[r];
        reg[r].budgetK = budgetK[r];
        if (!entries[r]) continue;
        reg[r].offset = off;
        off = vqAlignUp(off + kVqHdrBytes + (VkDeviceSize)entries[r] * entryBytes[r]);
    }
    const VkDeviceSize bytes = off;
    if (bytes == 0u)
    {
        if (mVq.buffer) { retireBuffer(mVq); mVq = GpuBuffer{}; }
        for (VisQueueRegion& r : mVqRegion) r = VisQueueRegion{};
        return;
    }
    // ---- the buffer: a new size allocates first and retires the old one (no descriptor names it) ----
    if (!mVq.buffer || mVq.size != bytes)
    {
        if (mTexPressure || now < mVqRetryAt)
        {
            // (soft: under texture pressure / after a refusal no new allocation -- a buffer at least this large
            //  keeps serving the new regions, else the consumers stay fused until it may allocate again)
            if (mVq.buffer && mVq.size < bytes) { retireBuffer(mVq); mVq = GpuBuffer{}; }
            if (!mVq.buffer) { for (VisQueueRegion& r : mVqRegion) r = VisQueueRegion{}; return; }
        }
        else
        {
            GpuBuffer nb{};
            {
                VkMemScope memcat(VKMEM_TARGET);
                VkDeviceContext::SoftAllocScope soft(mCtx);
                nb = mCtx->createBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT
                                               | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true);
            }
            if (nb.buffer && nb.address)
            {
                if (mVq.buffer) retireBuffer(mVq);
                mVq = nb;
                mCtx->setObjectName(VK_OBJECT_TYPE_BUFFER, (uint64_t)mVq.buffer, "visQueue");
                LL_INFOS("Vulkan") << "VK vis queue (#1039): " << ((bytes + (1u << 19)) >> 20) << " MB (cap "
                                   << (cap >> 20) << " MB; set " << vs << "; RT reflections " << entries[VQR_RT] << " of "
                                   << want[VQR_RT] << " entries x " << entryBytes[VQR_RT] << " B, GI " << entries[VQR_GI]
                                   << " of " << want[VQR_GI] << " x " << entryBytes[VQR_GI] << " B, GI reuse "
                                   << entries[VQR_GIR] << " of " << want[VQR_GIR] << " x " << entryBytes[VQR_GIR] << " B, shadow sun "
                                   << entries[VQR_SHSUN] << " of " << want[VQR_SHSUN] << " x " << entryBytes[VQR_SHSUN]
                                   << " B, shadow light samples " << entries[VQR_SHVIS] << " of " << want[VQR_SHVIS] << " x "
                                   << entryBytes[VQR_SHVIS] << " B)" << LL_ENDL;
            }
            else
            {
                if (nb.buffer) mCtx->destroyBuffer(nb);
                mVqRetryAt = now + 30.0;
                LL_WARNS("Vulkan") << "VK vis queue (#1039): the " << ((bytes + (1u << 19)) >> 20)
                                   << " MB buffer was refused (VRAM); the fused hit shading / GI reuse instead, retry in 30 s" << LL_ENDL;
                if (mVq.buffer) { retireBuffer(mVq); mVq = GpuBuffer{}; }
                for (VisQueueRegion& r : mVqRegion) r = VisQueueRegion{};
                return;
            }
        }
    }
    for (uint32_t r = 0; r < VQR_COUNT; ++r) mVqRegion[r] = reg[r];
    mVqSet = vs;
    mVqRtNow = mVqRegion[VQR_RT].entries != 0u;
    mVqGiNow = mVqRegion[VQR_GI].entries != 0u;
    mVqGirNow = mVqRegion[VQR_GIR].entries != 0u;
    mVqShSunNow = mVqRegion[VQR_SHSUN].entries != 0u;
    mVqShVisNow = mVqRegion[VQR_SHVIS].entries != 0u;
    // (M6, bit 16: no pipelines of its own) the frame-level walk, when any consumer queues this recording
    mVqM6Now = (mask & 16u) != 0u && (mVqRtNow || mVqGiNow || mVqShSunNow || mVqShVisNow);
}

// Region r's header for this recording: (0, 0, 0, 1) -- no request, the walk's dispatch empty --, the
// capacities, the RT records' address / capacity, the counters zeroed. WAR: last frame's walk / resolve
// reads (storage + indirect) and the counters' copy, then the producer's appends see it.
void LLCleanRendererVK::visQueueBegin(VkCommandBuffer cmd, uint32_t r, VkDeviceAddress recAddr, uint32_t recCap)
{
    const uint32_t w2[4] = {(uint32_t)(recAddr & 0xFFFFFFFFull), (uint32_t)(recAddr >> 32), recCap, 0u};
    visQueueBeginW(cmd, r, w2);
}
// (header word 2 as given: the RT records' address / capacity, or the shadow core's TAIL preset)
void LLCleanRendererVK::visQueueBeginW(VkCommandBuffer cmd, uint32_t r, const uint32_t w2[4])
{
    if (!mVq.buffer || r >= VQR_COUNT || !mVqRegion[r].entries) return;
    if ((mVqHdrDone & (1u << r)) != 0u) return;   // (M6: visQueueBeginFrame wrote it with the others)
    VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    m.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    m.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    m.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT; m.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1; d.pMemoryBarriers = &m;
    vkCmdPipelineBarrier2(cmd, &d);
    visQueueHdrWrite(cmd, r, w2);
    m.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT; m.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    m.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
    m.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                    | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
    vkCmdPipelineBarrier2(cmd, &d);
}

// The header write alone (the caller orders it: visQueueBeginW, visQueueBeginFrame).
void LLCleanRendererVK::visQueueHdrWrite(VkCommandBuffer cmd, uint32_t r, const uint32_t w2[4])
{
    const uint32_t e = mVqRegion[r].entries;
    const uint32_t hdr[20] = {0u, 0u, 0u, 1u,
                              e * mVqRegion[r].reqPerEntry, e, mVqRegion[r].slots, mVqRegion[r].budgetK,
                              w2[0], w2[1], w2[2], w2[3],
                              0u, 0u, 0u, 0u,
                              0u, 0u, 0u, 0u};
    vkCmdUpdateBuffer(cmd, mVq.buffer, mVqRegion[r].offset, sizeof(hdr), hdr);
}

// The walk over region r's requests: one lane per request, the producer's indirect dispatch (header words
// 1-3). The caller bound the RT layout's sets and ordered the producer's writes before it (+ indirect).
void LLCleanRendererVK::visQueueWalk(VkCommandBuffer cmd, uint32_t r, const VKRtReflPush& base)
{
    if (!mVq.buffer || !mVisWalkPipe || r >= VQR_COUNT || !mVqRegion[r].entries) return;
    VKRtReflPush wp = base;
    const VkDeviceAddress a = visQueueAddr(r);
    wp.rtr3[0] = 0u;
    wp.rtr3[1] = 0u;
    wp.rtr3[2] = (uint32_t)(a & 0xFFFFFFFFull);
    wp.rtr3[3] = (uint32_t)(a >> 32);
    VK_PUSH_BUDGET_CHECK(wp);
    vkCmdPushConstants(cmd, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(wp), &wp);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, rtBcPipe(mVisWalkPipe));   // (#1128 beacon twin when on)
    vkCmdDispatchIndirect(cmd, mVq.buffer, mVqRegion[r].offset + 4u);
}

// Region r's counters (header words 0-4) into this recording's readback slot (GPU timers on): after the
// consumer's resolve. visQueueDecide harvests the slot three recordings later.
void LLCleanRendererVK::visQueueStats(VkCommandBuffer cmd, uint32_t r)
{
    if (!mTsOn || !mVq.buffer || r >= VQR_COUNT || !mVqRegion[r].entries) return;
    if (!mVqStatRb.buffer)
    {
        VkMemScope memcat(VKMEM_READBACK);   // #407
        mVqStatRb = mCtx->createBuffer(3u * VQR_COUNT * kVqStatBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (!mVqStatRb.buffer) return;   // (stats only: none this time, retried next frame)
    }
    VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    m.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; m.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    m.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT; m.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1; d.pMemoryBarriers = &m;
    vkCmdPipelineBarrier2(cmd, &d);
    VkBufferCopy c{};
    c.srcOffset = mVqRegion[r].offset;
    c.dstOffset = ((VkDeviceSize)mVqStatSlot * VQR_COUNT + r) * kVqStatBytes;
    c.size = kVqStatBytes;
    vkCmdCopyBuffer(cmd, mVq.buffer, mVqStatRb.buffer, 1, &c);
    mVqStatUsed[mVqStatSlot][r] = true;
}

// ======================= (#1039, RenderVKVisQueue bit 8) THE SHADOW CORE =======================
// recordProjStoch's split branch (RenderVKShadowSplitKernels): SELECT -> [SUNVIS + VIS] -> SHADE becomes
//   SELECT -> [GEN sun + GEN light samples] -> [WALK sun + WALK light samples + VIS TAIL] -> SHADE
// GEN is SUNVIS / VIS up to the walk (the receiver, the sample, the receiver rules and their probes, the
// certificates' lookup) writing the sample as a request; WALK is the sample's rays from the request + what the
// kernel stored after its walk (the visibility word / the sun channel's texel, the certificates' post rays); the
// light-sample jobs past the region run VIS over themselves (TAIL). The same samples, the same rays, the same
// stores -- SHADE and the denoiser read exactly what they read before. Barriers only, on the segment's queue.

// The regions' headers (before SELECT: nothing of pass 15 has touched the queue yet this frame).
void LLCleanRendererVK::visQueueShadowBegin(VkCommandBuffer cmd, bool qSun, bool qVis)
{
    if (qSun) visQueueBegin(cmd, VQR_SHSUN, 0u, 0u);
    if (qVis)
    {
        // word 2: the TAIL's indirect dispatch (0, 1, 1) until GEN's first lane writes it, the first tail job
        const uint32_t w2[4] = {0u, 1u, 1u, mVqRegion[VQR_SHVIS].entries};
        visQueueBeginW(cmd, VQR_SHVIS, w2);
    }
}

void LLCleanRendererVK::visQueueShadowCore(VkCommandBuffer cmd, const VKRtReflPush& rp, uint32_t dw, uint32_t dh, bool sun,
                                           bool qSun, bool qVis, bool isoLod, bool merged, bool onAsync,
                                           VkPipeline fusedSun, VkPipeline fusedVis)
{
    visQueueShadowGen(cmd, rp, dw, dh, sun, qSun, qVis, merged, onAsync, fusedSun, fusedVis);
    visQueueShadowWalk(cmd, rp, sun, qSun, qVis, isoLod, merged, onAsync, true);
    visQueueShadowFinish(cmd, rp, sun, qSun, qVis);
}
namespace
{
// The shadow core's pushes: the sun's region in rtr3.zw (its rtr3.xy: the certificates), the light samples' in
// rtr3.xy (their rtr3.w: the visibility slots)
void vqShPushes(const VKRtReflPush& rp, VkDeviceAddress sunAddr, VkDeviceAddress visAddr, bool qSun, bool qVis,
                VKRtReflPush& ps, VKRtReflPush& pv)
{
    ps = rp;
    pv = rp;
    if (qSun)
    {
        ps.rtr3[2] = (uint32_t)(sunAddr & 0xFFFFFFFFull);
        ps.rtr3[3] = (uint32_t)(sunAddr >> 32);
    }
    if (qVis)
    {
        pv.rtr3[0] = (uint32_t)(visAddr & 0xFFFFFFFFull);
        pv.rtr3[1] = (uint32_t)(visAddr >> 32);
    }
}
} // namespace
void LLCleanRendererVK::visQueueShadowGen(VkCommandBuffer cmd, const VKRtReflPush& rp, uint32_t dw, uint32_t dh, bool sun,
                                          bool qSun, bool qVis, bool merged, bool onAsync, VkPipeline fusedSun,
                                          VkPipeline fusedVis)
{
    // (the GPU sub-marks: c_ on the async queue, g_ on graphics; the merged pass's names, else the projector-only pass's)
    auto mark = [this, cmd, onAsync, merged](const char* c, const char* g, const char* cp, const char* gp)
    { gpuSubMark(cmd, merged ? (onAsync ? c : g) : (onAsync ? cp : gp)); };
    qSun = qSun && sun;
    VKRtReflPush ps, pv;
    vqShPushes(rp, qSun ? visQueueAddr(VQR_SHSUN) : 0u, qVis ? visQueueAddr(VQR_SHVIS) : 0u, qSun, qVis, ps, pv);
    auto push = [this, cmd](const VKRtReflPush& p)
    {
        VK_PUSH_BUDGET_CHECK(p);
        vkCmdPushConstants(cmd, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
    };
    // ---- GEN (or the fused kernel of the part not queued): sun + light samples, independent of each other ----
    if (sun)
    {
        if (qSun) push(ps);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, qSun ? mShVqPipe[VQSH_SUN_GEN] : fusedSun);
        vkCmdDispatch(cmd, (dw + 7u) / 8u, (dh + 7u) / 8u, 1);
        if (qSun) mark("c_defer_shadowsungen", "g_defer_shadowsungen", "c_defer_shadowsungen", "g_defer_shadowsungen");
        else      mark("c_defer_shadowsun", "g_defer_shadowsun", "c_defer_shadowsun", "g_defer_shadowsun");
    }
    push(qVis ? pv : rp);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, qVis ? mShVqPipe[VQSH_VIS_GEN] : fusedVis);
    vkCmdDispatchIndirect(cmd, mPdnVis.buffer, 4u);   // (one thread per gated light sample, 64 per workgroup)
    if (qVis) mark("c_defer_shadowvisgen", "g_defer_shadowvisgen", "c_defer_pstochvisgen", "g_defer_pstochvisgen");
    else      mark("c_defer_shadowvis", "g_defer_shadowvis", "c_defer_pstochvis", "g_defer_pstochvis");
}
void LLCleanRendererVK::visQueueShadowWalk(VkCommandBuffer cmd, const VKRtReflPush& rp, bool sun, bool qSun, bool qVis,
                                           bool isoLod, bool merged, bool onAsync, bool lead)
{
    auto mark = [this, cmd, onAsync, merged](const char* c, const char* g, const char* cp, const char* gp)
    { gpuSubMark(cmd, merged ? (onAsync ? c : g) : (onAsync ? cp : gp)); };
    qSun = qSun && sun;
    VKRtReflPush ps, pv;
    vqShPushes(rp, qSun ? visQueueAddr(VQR_SHSUN) : 0u, qVis ? visQueueAddr(VQR_SHVIS) : 0u, qSun, qVis, ps, pv);
    auto push = [this, cmd](const VKRtReflPush& p)
    {
        VK_PUSH_BUDGET_CHECK(p);
        vkCmdPushConstants(cmd, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
    };
    // ---- WALK (+ VIS's TAIL over the jobs past the region): the requests GEN appended, their indirect dispatches ----
    if ((qSun || qVis) && lead)
    {
        VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        m.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; m.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        m.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
        m.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                        | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.memoryBarrierCount = 1; d.pMemoryBarriers = &m;
        vkCmdPipelineBarrier2(cmd, &d);
    }
    if (qSun)
    {
        push(ps);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, rtBcPipe(mShVqPipe[isoLod ? VQSH_SUN_WALK_ISO : VQSH_SUN_WALK]));
        vkCmdDispatchIndirect(cmd, mVq.buffer, mVqRegion[VQR_SHSUN].offset + 4u);
        mark("c_defer_shadowsunwalk", "g_defer_shadowsunwalk", "c_defer_shadowsunwalk", "g_defer_shadowsunwalk");
    }
    if (qVis)
    {
        push(pv);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, rtBcPipe(mShVqPipe[isoLod ? VQSH_VIS_WALK_ISO : VQSH_VIS_WALK]));
        vkCmdDispatchIndirect(cmd, mVq.buffer, mVqRegion[VQR_SHVIS].offset + 4u);
        mark("c_defer_shadowviswalk", "g_defer_shadowviswalk", "c_defer_pstochviswalk", "g_defer_pstochviswalk");
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, rtBcPipe(mShVqPipe[isoLod ? VQSH_VIS_TAIL_ISO : VQSH_VIS_TAIL]));
        vkCmdDispatchIndirect(cmd, mVq.buffer, mVqRegion[VQR_SHVIS].offset + 32u);   // (header word 2)
        mark("c_defer_shadowvistail", "g_defer_shadowvistail", "c_defer_pstochvistail", "g_defer_pstochvistail");
    }
}
void LLCleanRendererVK::visQueueShadowFinish(VkCommandBuffer cmd, const VKRtReflPush& rp, bool sun, bool qSun, bool qVis)
{
    if (qSun && sun) visQueueStats(cmd, VQR_SHSUN);
    if (qVis) visQueueStats(cmd, VQR_SHVIS);
    VK_PUSH_BUDGET_CHECK(rp);
    vkCmdPushConstants(cmd, mRtReflLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(rp), &rp);   // (SHADE's push)
}

// ======================= (#1039 M6, RenderVKVisQueue bit 16) THE FRAME WALK =======================
// recordDeferLight's split chains, with bit 16 and at least one queued consumer this recording:
//   visQueueBeginFrame (every active region's header: one barrier pair instead of one per region)
//   PRODUCERS: the shadow core's SELECT + GEN; REFLECT; the RT reflections' TRACE + MAT + REQ (+ the fused tail);
//              [the projector-only pass 15's SELECT + GEN]; the GI's TRACE + MISS + HIT-MAT + HIT-REQ
//   visQueueFrameWalk: ONE barrier, every consumer's walk back to back (the sun / light-sample walks + VIS's tail,
//              the RT walk + its blend layers, the GI walk -- disjoint outputs, so no barrier between them: their
//              traversals overlap and fill each other's tails), ONE barrier
//   RESOLVES:  the shadow core's SHADE + denoiser, SHADOW (it reads the denoised sun), the RT LIGHT + forward +
//              resolve, the GI HIT-LIGHT + REUSE (its own second wave: the reuse's visibility rays follow its
//              reservoirs) + the rest
// The walks stay one kernel per kind group (regprobe, RTX 5090: the sun walk at 128 registers, the RT / GI hit walk
// at 166, the light samples' at 168 -- merged into one dispatch the sun's would run at the largest count, 12 warps
// per SM instead of 16; back-to-back dispatches with no barrier between them overlap all the same). Everything stays on the chain's queue segment;
// the GI early trace (#978) keeps its segment (the chain), the one-frame-latency GI (mGiLagNow) runs after the
// combine with its own walk (it is recorded outside the collecting window).
// Dependencies the reorder relies on (checked against clean_deferred_light.comp's bindings): REFLECT, the RT and the
// GI producers read neither pass 15's outputs (set 3 bindings 13 / 16: read by the combine passes and SHADOW only)
// nor SHADOW / CLASSIFY's (uVisSun / uVisProj / the tile grid: the combine passes; REFLECT already ran beside SHADOW
// without a barrier); the RT and GI hit lighting takes its lights from the clusters and the ReGIR grid, built before
// pass 15.
void LLCleanRendererVK::visQueueBeginFrame(VkCommandBuffer cmd, bool withGi, bool onAsync)
{
    mVqHdrDone = 0u;
    if (!mVq.buffer) return;
    uint32_t want = 0u;
    if (mVqRtNow && mRtRec.buffer) want |= 1u << VQR_RT;
    if (withGi && mVqGiNow) want |= 1u << VQR_GI;
    if (withGi && mVqGirNow) want |= 1u << VQR_GIR;
    if (mVqShSunNow) want |= 1u << VQR_SHSUN;
    if (mVqShVisNow) want |= 1u << VQR_SHVIS;
    for (uint32_t r = 0; r < VQR_COUNT; ++r)
        if (!mVqRegion[r].entries) want &= ~(1u << r);
    if (want == 0u) return;
    VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    m.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    m.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    m.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT; m.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1; d.pMemoryBarriers = &m;
    vkCmdPipelineBarrier2(cmd, &d);
    for (uint32_t r = 0; r < VQR_COUNT; ++r)
    {
        if ((want & (1u << r)) == 0u) continue;
        uint32_t w2[4] = {0u, 0u, 0u, 0u};
        if (r == VQR_RT)
        {
            w2[0] = (uint32_t)(mRtRec.address & 0xFFFFFFFFull);
            w2[1] = (uint32_t)(mRtRec.address >> 32);
            w2[2] = mRtRecCap;
        }
        else if (r == VQR_SHVIS)
        {
            // (visQueueShadowBegin's: the TAIL's indirect dispatch (0, 1, 1) until GEN's first lane writes it)
            w2[1] = 1u; w2[2] = 1u; w2[3] = mVqRegion[VQR_SHVIS].entries;
        }
        visQueueHdrWrite(cmd, r, w2);
    }
    m.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT; m.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    m.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
    m.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                    | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
    vkCmdPipelineBarrier2(cmd, &d);
    mVqHdrDone = want;
    gpuSubMark(cmd, onAsync ? "c_vq_begin" : "g_vq_begin");
}
bool LLCleanRendererVK::visQueueM6Defer(uint32_t c, std::function<void(VkCommandBuffer)> walk,
                                        std::function<void(VkCommandBuffer)> post)
{
    if (!mVqM6Collect || c >= VQC_COUNT || mVqM6Walk[c] || mVqM6Post[c]) return false;
    mVqM6Walk[c] = std::move(walk);
    mVqM6Post[c] = std::move(post);
    return true;
}
void LLCleanRendererVK::visQueueFrameWalk(VkCommandBuffer cmd, uint32_t viewOff, bool onAsync)
{
    mVqM6Collect = false;
    bool any = false;
    for (uint32_t c = 0; c < VQC_COUNT; ++c) any = any || (bool)mVqM6Walk[c];
    if (!any || !mRT) return;
    const VkDescriptorSet rtSet = mRT->consumerSet();
    // every producer's appends + raised dispatches -> the walks (storage + indirect)
    VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    m.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; m.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    m.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
    m.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                    | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1; d.pMemoryBarriers = &m;
    vkCmdPipelineBarrier2(cmd, &d);
    gpuSubMark(cmd, onAsync ? "c_vq_prod" : "g_vq_prod");   // (the producers' tail: the wave starts here)
    // (every walk runs on the RT layout with the chain's sets; each pushes its own region)
    const VkDescriptorSet sets[5] = {mDsetDraw0, mDsetDraw1, mDsetDefer, mDsetDeferSplit, rtSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mRtReflLayout, 0, 5, sets, 1, &viewOff);
    for (uint32_t c = 0; c < VQC_COUNT; ++c)
    {
        if (!mVqM6Walk[c]) continue;
        std::function<void(VkCommandBuffer)> f = std::move(mVqM6Walk[c]);
        mVqM6Walk[c] = nullptr;
        f(cmd);
    }
    // the walks' results -> the resolves
    m.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    m.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    vkCmdPipelineBarrier2(cmd, &d);
}
void LLCleanRendererVK::visQueueFramePost(VkCommandBuffer cmd, uint32_t c)
{
    if (c >= VQC_COUNT || !mVqM6Post[c]) return;
    std::function<void(VkCommandBuffer)> f = std::move(mVqM6Post[c]);
    mVqM6Post[c] = nullptr;
    f(cmd);
}
void LLCleanRendererVK::visQueueM6End()
{
    mVqM6Collect = false;
    for (uint32_t c = 0; c < VQC_COUNT; ++c)
    {
        mVqM6Walk[c] = nullptr;
        mVqM6Post[c] = nullptr;
    }
    mVqHdrDone = 0u;
}
