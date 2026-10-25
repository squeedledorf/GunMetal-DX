/**
 * @file llcleanweathervk_runoff.cpp
 * @brief Weather run-off (#783 M4): water streaming off covered surfaces. See llcleanweathervk.h
 *        (M4) and clean_weather_runoff.comp.
 */
#include "linden_common.h"

#include "llcleanweathervk.h"

#include "llerror.h"
#include "llmath.h"
#include "llmemtrend.h"
#include "llviewercontrol.h"
#include "slweather.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
// Per RenderVKWeatherQuality (0 / 1 / 2): the emitter list's capacity, the drips (two
// precipitation slots each; at most a quarter of the pool).
constexpr uint32_t kEmitCaps[3] = {2048u, 4096u, 8192u};
constexpr uint32_t kDripCaps[3] = {1024u, 2048u, 4096u};
// Drips per second per m2 of catchment at full run-off (one drawn drip stands for many real ones),
// the stream threshold (catchment m2 x strength: an eave draining a few m2 of roof in full rain
// pours), a drip's typical life (s: a 1-3 m fall) -- the budget is the drip slots per life.
constexpr float kDripsPerM2 = 3.f;
constexpr float kStreamArea = 2.5f;
constexpr float kDripLife = 0.7f;
// The flow build (clean_weather_runoff.comp RunoffPush.c): the potential's pull toward the
// nearest edge (m per m: under any real slope), the same-surface tolerance of the jump flood (m),
// the least catchment that discharges (texels).
constexpr float kFlowEps = 0.002f;
constexpr float kSameSurface = 0.15f;
constexpr float kMinOutlet = 2.f;
constexpr double kRunoffRetrySecs = 10.0;   // (#697: as the module's other allocations)

struct RunoffPush
{
    uint32_t a[4];   // pass, R, near texels per flow texel, the published near map
    uint32_t b[4];   // source parity, destination parity, jump-flood step, emitter capacity
    float    fr0[4]; // the near frame: x0, y0, extent, top
    float    fr1[4]; // span, flow texel (m), shear x, y
    float    c[4];   // outlet drop (m), eps (m / m), same-surface tolerance (m), least outlet (texels)
};
static_assert(sizeof(RunoffPush) == 80, "RunoffPush = clean_weather_runoff.comp");

void rbarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
              VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
{
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = srcStage; mb.srcAccessMask = srcAccess;
    mb.dstStageMask = dstStage; mb.dstAccessMask = dstAccess;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; d.memoryBarrierCount = 1; d.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &d);
}

uint32_t floorPow2(uint32_t v)
{
    uint32_t p = 1;
    while (p * 2u <= v) p *= 2u;
    return p;
}
} // namespace

LLCleanWeatherVK::RunoffStats LLCleanWeatherVK::sRunoffStats;

// MemTrend (#883): the run-off's VRAM (it lives only while it runs; the stats are static).
void LLCleanWeatherVK::registerRunoffProbe()
{
    static bool s_done = false;
    if (s_done) return;
    s_done = true;
    LLMemTrend::registerProbe("vk-weather-runoff", []
    {
        const RunoffStats& s = sRunoffStats;
        LLMemTrend::Sample out;
        out.count = s.emitters;
        out.bytes = s.bytes;
        out.cap = 0;
        out.peak = s.peakBytes;
        return out;
    });
}

void LLCleanWeatherVK::destroyRunoff()
{
    if (mCtx)
    {
        for (GpuBuffer* b : {&mRunoffScratch, &mEmitters})
            if (b->buffer) mCtx->destroyBuffer(*b);
        if (mFlowImg.image) mCtx->destroyImage(mFlowImg);
    }
    mRunoffScratch = mEmitters = GpuBuffer{};
    mFlowImg = GpuImage{};
    mRunoffRes = 0;
    mEmitCap = 0;
    mFlowValid = false;
    mFlowGen = ~0ull;
    mRunoffNow = false;
    mDripsClean = false;
    sRunoffStats.flowRes = 0;
    sRunoffStats.bytes = 0;
    sRunoffStats.emitters = sRunoffStats.emitterCap = 0;
    sRunoffStats.drips = sRunoffStats.dripCap = 0;
}

// The build's own set: the near maps (the coarse ones stand in while there are none -- never
// read then), the scratch, the flow map, the emitters. Device idle (the callers re-created what
// it names).
void LLCleanWeatherVK::writeRunoffSets()
{
    if (!mCtx || !mRunoffSet || !mRunoffScratch.buffer || !mEmitters.buffer || !mFlowImg.view || !mSampler) return;
    const GpuImage* nearImg = mMap[OCCL_NEAR].img;
    const GpuImage* coarse = mMap[OCCL_COARSE].img;
    const bool haveNear = nearImg[0].view && nearImg[1].view;
    if (!haveNear && (!coarse[0].view || !coarse[1].view)) return;
    const VkDescriptorImageInfo ni[2] = {{mSampler, haveNear ? nearImg[0].view : coarse[0].view, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},
                                         {mSampler, haveNear ? nearImg[1].view : coarse[1].view, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL}};
    const VkDescriptorBufferInfo sb{mRunoffScratch.buffer, 0, VK_WHOLE_SIZE};
    const VkDescriptorImageInfo fi{VK_NULL_HANDLE, mFlowImg.view, VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorBufferInfo eb{mEmitters.buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet wr[4]{};
    for (uint32_t i = 0; i < 4; ++i)
    {
        wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[i].dstSet = mRunoffSet; wr[i].dstBinding = i; wr[i].descriptorCount = 1;
    }
    wr[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr[0].descriptorCount = 2; wr[0].pImageInfo = ni;
    wr[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; wr[1].pBufferInfo = &sb;
    wr[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; wr[2].pImageInfo = &fi;
    wr[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; wr[3].pBufferInfo = &eb;
    vkUpdateDescriptorSets(mCtx->device(), 4, wr, 0, nullptr);
}

// The run-off's resources while wanted (the flow grid at `res`, the emitter list at `emitCap`),
// freed otherwise -- each change behind a device wait (binding 49 and the own sets' binding 8
// name them in frames in flight). A refused allocation (#697) skips the run-off and retries
// kRunoffRetrySecs later. true = they exist.
bool LLCleanWeatherVK::ensureRunoff(VkCommandBuffer cmd, bool want, uint32_t res, uint32_t emitCap)
{
    if (!want || !mRunoffSet || res == 0)
    {
        if (mRunoffScratch.buffer || mFlowImg.image || mEmitters.buffer)
        {
            wxWaitIdle(mCtx->device(), "run-off freed");
            destroyRunoff();
            mBindingDirty = true;
            writeSets();   // (binding 8 back to its stand-in)
            LL_INFOS("VKWeather") << "weather run-off freed" << LL_ENDL;
        }
        return false;
    }
    if (mRunoffScratch.buffer && mRunoffRes == res && mEmitCap == emitCap) return true;
    if (mNow < mRunoffRetryAt) return false;
    wxWaitIdle(mCtx->device(), "run-off");
    const bool had = mRunoffScratch.buffer != VK_NULL_HANDLE;
    destroyRunoff();
    mBindingDirty = mBindingDirty || had;
    const VkDeviceSize scratchBytes = (VkDeviceSize)res * res * 9u * 4u;
    const VkDeviceSize listBytes = 64u + (VkDeviceSize)emitCap * 32u;
    bool ok = true;
    {
        ++sSync.recreates;   // #946 (the "weather sync" line)
        VkMemScope memcat(VKMEM_PARTICLE);   // #407 (the weather's category)
        VkDeviceContext::SoftAllocScope soft(mCtx);
        const VkBufferUsageFlags su = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        mRunoffScratch = mCtx->createBuffer(scratchBytes, su, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        mEmitters = mCtx->createBuffer(listBytes, su | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        mFlowImg = mCtx->createImage2D(res, res, VK_FORMAT_R32G32B32A32_SFLOAT,
                                       VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                       VK_IMAGE_ASPECT_COLOR_BIT);
        ok = mRunoffScratch.buffer && mEmitters.buffer && mFlowImg.image && mFlowImg.view;
    }
    if (!ok)
    {
        LL_WARNS("VKWeather") << "weather run-off (" << res << "^2 flow grid, " << emitCap << " emitters) failed to allocate; "
                              << "run-off skipped, retrying in " << kRunoffRetrySecs << " s" << LL_ENDL;
        destroyRunoff();
        mRunoffRetryAt = mNow + kRunoffRetrySecs;
        writeSets();
        return false;
    }
    mRunoffRes = res;
    mEmitCap = emitCap;
    mBindingDirty = true;
    // An empty list (no spawns, no streams) and a zero flow map (GENERAL: storage + sampled).
    vkCmdFillBuffer(cmd, mEmitters.buffer, 0, VK_WHOLE_SIZE, 0u);
    {
        VkImageMemoryBarrier2 ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        ib.dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT; ib.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        ib.image = mFlowImg.image; ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; dep.imageMemoryBarrierCount = 1; dep.pImageMemoryBarriers = &ib;
        vkCmdPipelineBarrier2(cmd, &dep);
        const VkClearColorValue zero{};
        const VkImageSubresourceRange rng{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, mFlowImg.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &rng);
        ib.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT; ib.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        ib.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        ib.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        vkCmdPipelineBarrier2(cmd, &dep);
    }
    rbarrier(cmd, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT
                 | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
             VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT
                 | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
    writeSets();   // (binding 8 -> the list; the build's set)
    registerRunoffProbe();
    const uint64_t bytes = (uint64_t)scratchBytes + listBytes + (uint64_t)res * res * 16u;
    sRunoffStats.flowRes = res;
    sRunoffStats.emitterCap = emitCap;
    sRunoffStats.bytes = bytes;
    sRunoffStats.peakBytes = std::max(sRunoffStats.peakBytes, bytes);
    LL_INFOS("VKWeather") << "weather run-off: " << res << "^2 flow grid, " << emitCap << " emitters ("
                          << bytes / 1024u << " KB)" << LL_ENDL;
    return true;
}

// This recording's run-off (prepare(), after the maps' publish and the frame info's map rows):
// the resources, the flow build when the published near map is new to it, the drips' share of
// the pool, the parameters and ssr.weather[13] / [14].
void LLCleanWeatherVK::planRunoff(VkCommandBuffer cmd, const Frame& f, uint32_t quality)
{
    static LLCachedControl<bool> s_on(gSavedSettings, "RenderVKWeatherRunoff", true);
    static LLCachedControl<U32>  s_res(gSavedSettings, "RenderVKWeatherRunoffRes", 256);
    static LLCachedControl<bool> s_streams(gSavedSettings, "RenderVKWeatherRunoffStreams", true);
    static LLCachedControl<bool> s_rivulets(gSavedSettings, "RenderVKWeatherRunoffRivulets", true);
    const bool wasOn = mRunoffNow;
    mRunoffNow = false;
    mFlowDue = false;
    mDripCount = 0;
    const SLWeather::State& st = SLWeather::instance().state();
    const OcclMap& nm = mMap[OCCL_NEAR];
    const bool nearOk = mOcclOn && nm.img[0].image && nm.res;
    const bool want = s_on && st.runoff > SLWeather::kEpsilon && nearOk && mRunoffPipe && mSpawnPipe && mRunoffLayout;
    uint32_t factor = 1, res = 0;
    if (nearOk)
    {
        factor = std::max<uint32_t>(nm.res / floorPow2(llclamp((U32)s_res, 128u, 512u)), 1u);
        res = nm.res / factor;
    }
    const uint32_t q = std::min<uint32_t>(quality, 2u);
    if (!ensureRunoff(cmd, want, res, kEmitCaps[q]))
    {
        mFlowValid = false;
        mDripsClean = false;
        return;
    }
    mRunoffFactor = factor;
    if (nm.pubIdx < 0)
        mFlowValid = false;
    else if (mFlowGen != mNearGen || !mFlowValid)
    {
        std::memcpy(mFlowF, nm.pub, sizeof(mFlowF));
        mFlowDue = true;
        recordRunoffBuild(cmd);
        mFlowGen = mNearGen;
        mFlowValid = true;
    }
    if (!mFlowValid) { mDripsClean = false; return; }
    mRunoffNow = true;
    if (!wasOn || f.cameraCut) mDripsClean = false;   // (stale drips, or an origin shift: cleared before the spawn)

    // The drips' share of the precipitation pool: its tail (two slots per drip, at most a quarter).
    mDripCount = std::min(kDripCaps[q], mCap / 8u);
    mDripBase = mCap - 2u * mDripCount;
    sRunoffStats.dripCap = mDripCount;
    Params& P = mP;
    const float S = llclamp(st.runoff, 0.f, 1.f);
    P.runoff[0] = S;
    P.runoff[1] = kDripsPerM2;
    P.runoff[2] = kStreamArea;
    P.runoff[3] = (float)mDripCount / kDripLife;
    P.runoff2[0] = (float)mDripCount;
    P.runoff2[1] = (float)mDripBase;
    P.runoff2[2] = s_streams ? 1.f : 0.f;
    P.runoff2[3] = (float)mEmitCap;

    // ssr.weather[13] / [14]: the flow map's frame (its near map's top plane; the grid spans
    // res x the flow texel from the near map's corner) and the strength / flags.
    const float texel = mFlowF[5] * (float)mRunoffFactor;
    mWx[13][0] = mFlowF[0]; mWx[13][1] = mFlowF[1];
    mWx[13][2] = 1.f / std::max(texel * (float)mRunoffRes, 1e-3f);
    mWx[13][3] = S;
    mWx[14][0] = mFlowF[3]; mWx[14][1] = mFlowF[6]; mWx[14][2] = mFlowF[7];
    mWx[14][3] = (float)(1u | (s_rivulets ? 2u : 0u) | 4u);
}

// The flow build (clean_weather_runoff.comp): ~25 dispatches over the R^2 grid from the published
// near map, each after the previous one's writes.
void LLCleanWeatherVK::recordRunoffBuild(VkCommandBuffer cmd)
{
    static LLCachedControl<F32> s_drop(gSavedSettings, "RenderVKWeatherRunoffEdgeDrop", 0.3f);
    const OcclMap& nm = mMap[OCCL_NEAR];
    const VkPipelineStageFlags2 C = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    const VkAccessFlags2 RW = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    // The last recordings' readers (the lit FS's flow map, the spawn kernel, the stream draw's
    // indirect / vertex reads) -> the header reset and this build's writes.
    rbarrier(cmd, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | C | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
             VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT,
             VK_PIPELINE_STAGE_2_COPY_BIT | C, VK_ACCESS_2_TRANSFER_WRITE_BIT | RW);
    const uint32_t head[16] = {};
    vkCmdUpdateBuffer(cmd, mEmitters.buffer, 0, sizeof(head), head);
    rbarrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, C, RW);

    RunoffPush pc{};
    const uint32_t R = mRunoffRes;
    pc.a[1] = R; pc.a[2] = mRunoffFactor; pc.a[3] = (uint32_t)std::max(nm.pubIdx, 0);
    pc.b[3] = mEmitCap;
    pc.fr0[0] = mFlowF[0]; pc.fr0[1] = mFlowF[1]; pc.fr0[2] = mFlowF[2]; pc.fr0[3] = mFlowF[3];
    pc.fr1[0] = mFlowF[4]; pc.fr1[1] = mFlowF[5] * (float)mRunoffFactor; pc.fr1[2] = mFlowF[6]; pc.fr1[3] = mFlowF[7];
    pc.c[0] = llclamp((F32)s_drop, 0.1f, 2.f); pc.c[1] = kFlowEps; pc.c[2] = kSameSurface; pc.c[3] = kMinOutlet;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mRunoffPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mRunoffLayout, 0, 1, &mRunoffSet, 0, nullptr);
    const uint32_t g = (R + 7u) / 8u;
    auto pass = [&](uint32_t mode, uint32_t src, uint32_t dst, uint32_t step) {
        pc.a[0] = mode; pc.b[0] = src; pc.b[1] = dst; pc.b[2] = step;
        vkCmdPushConstants(cmd, mRunoffLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        if (mode == 8u) vkCmdDispatch(cmd, 1, 1, 1);
        else vkCmdDispatch(cmd, g, g, 1);
        rbarrier(cmd, C, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, C, RW);
    };
    pass(0u, 0u, 0u, 0u);   // heights (+ the accumulators cleared)
    pass(1u, 0u, 0u, 0u);   // outlets: the jump flood's seeds
    uint32_t par = 0;
    for (uint32_t k = floorPow2(std::max(R / 2u, 1u)); k >= 1u; k /= 2u)
    {
        pass(2u, par, par ^ 1u, k);
        par ^= 1u;
    }
    pass(3u, par, 0u, 0u);  // D8 receivers -> pointers [0]
    uint32_t jumps = 2;
    for (uint32_t v = R; v > 1u; v /= 2u) ++jumps;   // log2 R + 2: paths up to 4 R hops
    par = 0;
    for (uint32_t j = 0; j < jumps; ++j)
    {
        pass(4u, par, par ^ 1u, 0u);
        par ^= 1u;
    }
    pass(5u, par, 0u, 0u);  // catchments
    pass(6u, par, 0u, 0u);  // flow + emitters
    pass(7u, 0u, 0u, 0u);   // the surfaces' flow map
    pass(8u, 0u, 0u, 0u);   // the list's dispatch / draw arguments
    // -> the spawn kernel (indirect + storage), the stream draw (indirect + vertex), the lit FS.
    rbarrier(cmd, C, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
             C | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
             VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    ++sRunoffStats.builds;
}

// The drips' spawn (recordSim, before the simulation; the module's own set bound): one
// invocation per emitter, indirect from the list's header.
void LLCleanWeatherVK::recordRunoffSpawn(VkCommandBuffer cmd)
{
    if (!mRunoffNow || !mSpawnPipe || !mEmitters.buffer || mDripCount == 0) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mSpawnPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mLayout, 0, 1, &mDset[mSlot], 0, nullptr);
    vkCmdDispatchIndirect(cmd, mEmitters.buffer, 0);
    // -> the simulation's reads of the new drips and the ring heads.
    const VkAccessFlags2 RW = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    rbarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, RW);
}

// The spawn kernel's words of the listener readback ([1] loudness x 1000, [2] the list's appends,
// [3] the frame's drips): the stats and the audio's run-off level.
void LLCleanWeatherVK::readRunoffWords(const uint32_t* w)
{
    RunoffStats& s = sRunoffStats;
    if (s.emitterCap)
    {
        s.emitters = std::min(w[2], s.emitterCap);
        s.emitterPeak = std::max(s.emitterPeak, s.emitters);
        if (w[2] > s.emitterCap) ++s.emitterOverflow;
    }
    s.drips = w[3];
    s.dripPeak = std::max(s.dripPeak, s.drips);
    const float loud = (float)w[1] * 1e-3f;
    SLWeather::instance().setRunoffNear(1.f - std::exp(-0.5f * loud));
}
