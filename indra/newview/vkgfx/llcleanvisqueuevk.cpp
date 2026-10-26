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
 * (2 + K) x 16 (K = RenderVKGIHitShadowRays; 144 B at K 1) -- capped by RenderVKVisQueueMB (0 = the VRAM tier:
 * 384 MB above 24 GB, 192 MB above 12.5 GB, 64 MB above 8.5 GB, else 32 MB). At 1440p (GI at half extent,
 * the RT records at the default ray budget: ~0.92 M entries each): set 0 ~59 MB per consumer, set 1 ~206 MB
 * RT + ~133 MB GI. Fixed by the extent / the settings: no growth. Under texture pressure no new buffer is
 * allocated (the consumers stay fused); a refusal (#697) retries in 30 s.
 * GPU timers: c_/g_defer_rtrefl_walk and c_/g_gi_walk (the walks); the material / lighting marks keep their
 * names (now: + the requests / the resolve and its tail). Counters (GPU timers on): per region requests,
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

// The pipeline sets a RenderVKVisQueue mask asks for: bit s = set s (set 0 = M1, bit 1; set 1 = M2, bit 2).
static uint32_t vqSetsOf(U32 mask)
{
    return ((mask & 1u) != 0u ? 1u : 0u) | ((mask & 2u) != 0u ? 2u : 0u);
}
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
        {3u, kWorldDeferVisWalkSpv, kWorldDeferVisWalkSpv_bytes, "DeferVisWalk", "mVisWalkPipe (DeferVisWalk)", &mVisWalkPipe},
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
        {2u, kWorldDeferRtSplitLayersVq2Spv, kWorldDeferRtSplitLayersVq2Spv_bytes, "DeferRtSplitLayersVq2",
         "mRtVqPipe (DeferRtSplitLayersVq2)", &mRtVqPipe[1][VQS_LAYERS]},
        {2u, kWorldDeferRtSplitLightVq2Spv, kWorldDeferRtSplitLightVq2Spv_bytes, "DeferRtSplitLightVq2",
         "mRtVqPipe (DeferRtSplitLightVq2)", &mRtVqPipe[1][VQS_LIGHT]},
        {2u, kWorldDeferGiSplitHitMatVq2Spv, kWorldDeferGiSplitHitMatVq2Spv_bytes, "DeferGiSplitHitMatVq2",
         "mGiVqPipe (DeferGiSplitHitMatVq2)", &mGiVqPipe[1][VQS_MAT]},
        {2u, kWorldDeferGiSplitHitLightVq2Spv, kWorldDeferGiSplitHitLightVq2Spv_bytes, "DeferGiSplitHitLightVq2",
         "mGiVqPipe (DeferGiSplitHitLightVq2)", &mGiVqPipe[1][VQS_LIGHT]}};
    for (uint32_t s = 0; s < 2u; ++s)
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
    if (mVq.buffer) { mCtx->destroyBuffer(mVq); mVq = GpuBuffer{}; }
    if (mVqStatRb.buffer) { mCtx->destroyBuffer(mVqStatRb); mVqStatRb = GpuBuffer{}; }
    for (auto& s : mVqStatUsed)
        for (bool& u : s) u = false;
    for (VisQueueRegion& r : mVqRegion) r = VisQueueRegion{};
    mVqRtNow = mVqGiNow = false;
    mVqSet = 0;
    mVqPipesQueued[0] = mVqPipesQueued[1] = false;
}

// recordWorld, after giDecide (the GI shade split, its extent) and deferSplitFrame (the RT split, its
// records): which consumers queue their hit sun rays this recording, the regions, the buffer.
void LLCleanRendererVK::visQueueDecide()
{
    mVqRtNow = mVqGiNow = false;
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
        if (mVqStatAcc.frames[VQR_RT] > 0.0 || mVqStatAcc.frames[VQR_GI] > 0.0)
        {
            static const char* kName[VQR_COUNT] = {"RT reflections", "GI"};
            std::ostringstream os;
            os << "VK vis queue (#1039): buffer " << ((mVq.size + (1u << 19)) >> 20) << " MB of cap " << (mVqCapBytes >> 20) << " MB";
            for (uint32_t r = 0; r < VQR_COUNT; ++r)
            {
                const double f = mVqStatAcc.frames[r];
                if (!(f > 0.0)) continue;
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
    const uint32_t missing = sets & ~((mVqPipesQueued[0] ? 1u : 0u) | (mVqPipesQueued[1] ? 2u : 0u));
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
    const uint64_t want[VQR_COUNT] = {wantRt ? (uint64_t)mRtRecCap : 0u,
                                      wantGi ? (uint64_t)mGiDn->width() * mGiDn->height() : 0u};
    // per entry: R requests (the most it can ask: never refused) + S result slots. Set 1's hit shadow-ray
    // budget K = the one the consumer's push carries this recording (RenderVKRTReflectionsHitShadowRays,
    // RenderVKGIHitShadowRays; 4 = the shaders' RT_SHADE_PROJ_MAXK).
    uint32_t reqPer[VQR_COUNT] = {1u, 1u}, slots[VQR_COUNT] = {1u, 1u}, budgetK[VQR_COUNT] = {0u, 0u};
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
    if (!want[VQR_RT] && !want[VQR_GI])
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
        const uint64_t vram = mVramStatus.valid ? mVramStatus.capacity : 0u;
        const uint64_t gib = 1ull << 30;
        // (24 GB+: room for set 1 at 1440p -- ~340 MB with both consumers at K 1)
        cap = vram > 24u * gib ? (384ull << 20)
            : (vram > 25u * gib / 2u ? (192ull << 20) : (vram > 17u * gib / 2u ? (64ull << 20) : (32ull << 20)));
    }
    cap = std::clamp<uint64_t>(cap, 4ull << 20, 1024ull << 20);
    mVqCapBytes = (uint32_t)std::min<uint64_t>(cap, 0xFFFFFFFFull);
    const uint64_t room = cap - VQR_COUNT * (kVqHdrBytes + kVqAlign);
    const uint64_t demand = want[VQR_RT] * entryBytes[VQR_RT] + want[VQR_GI] * entryBytes[VQR_GI];
    uint32_t entries[VQR_COUNT] = {};
    for (uint32_t r = 0; r < VQR_COUNT; ++r)
    {
        uint64_t e = want[r];
        if (demand > room) e = (uint64_t)((double)e * ((double)room / (double)demand));   // (the cap: shared by demand)
        e &= ~(uint64_t)63u;                                                               // (whole walk workgroups)
        // a sliver is not worth the extra dispatches: below 4 K entries the consumer stays fused (and the
        // request / slot indices stay well inside 32 bits: <= 2^26 entries)
        entries[r] = (want[r] != 0u && e >= 4096u) ? (uint32_t)std::min<uint64_t>(e, 1ull << 26) : 0u;
    }
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
                                   << " of " << want[VQR_GI] << " x " << entryBytes[VQR_GI] << " B)" << LL_ENDL;
            }
            else
            {
                if (nb.buffer) mCtx->destroyBuffer(nb);
                mVqRetryAt = now + 30.0;
                LL_WARNS("Vulkan") << "VK vis queue (#1039): the " << ((bytes + (1u << 19)) >> 20)
                                   << " MB buffer was refused (VRAM); the fused hit shading instead, retry in 30 s" << LL_ENDL;
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
}

// Region r's header for this recording: (0, 0, 0, 1) -- no request, the walk's dispatch empty --, the
// capacities, the RT records' address / capacity, the counters zeroed. WAR: last frame's walk / resolve
// reads (storage + indirect) and the counters' copy, then the producer's appends see it.
void LLCleanRendererVK::visQueueBegin(VkCommandBuffer cmd, uint32_t r, VkDeviceAddress recAddr, uint32_t recCap)
{
    if (!mVq.buffer || r >= VQR_COUNT || !mVqRegion[r].entries) return;
    VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    m.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    m.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    m.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT; m.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1; d.pMemoryBarriers = &m;
    vkCmdPipelineBarrier2(cmd, &d);
    const uint32_t e = mVqRegion[r].entries;
    const uint32_t hdr[20] = {0u, 0u, 0u, 1u,
                              e * mVqRegion[r].reqPerEntry, e, mVqRegion[r].slots, mVqRegion[r].budgetK,
                              (uint32_t)(recAddr & 0xFFFFFFFFull), (uint32_t)(recAddr >> 32), recCap, 0u,
                              0u, 0u, 0u, 0u,
                              0u, 0u, 0u, 0u};
    vkCmdUpdateBuffer(cmd, mVq.buffer, mVqRegion[r].offset, sizeof(hdr), hdr);
    m.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT; m.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    m.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
    m.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                    | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
    vkCmdPipelineBarrier2(cmd, &d);
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
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mVisWalkPipe);
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
