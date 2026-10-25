/**
 * @file llcleanweathervk_accum.cpp
 * @brief Per-avatar REST-POSE ACCUMULATION of snow and wetness (#901 follow-up): surfaces gather
 *        them only while actually exposed, over time. See llcleanweathervk.h (#901 REST-POSE
 *        ACCUMULATION) and clean_weather_accum.comp / .glsl.
 *
 * The GPU does the work: the kernels run right after the shelter chain (its work list, flagged by
 * the filter for the avatars whose grid is due, and the atlas it just published). This side owns
 * the pool (soft allocation, fewer grids on a refusal), assigns grids to the avatars with a
 * shelter layer (by UUID: a re-slotted avatar keeps its grid; freed after a grace period without a
 * layer), schedules the updates (each grid at RenderVKWeatherAvatarAccumRate, at most kAcSlots a
 * recording, the oldest first), writes the record's header and the rest skeleton (once), keeps the
 * own avatar's grid as a CPU snapshot that a new module restores (it outlives the module), and
 * reports the stats.
 *
 * COST (on paper, RTX 5090 class): ACCUM evaluates one vertex per triangle of every visible draw
 * of a due avatar (~20-60k samples for a typical mesh avatar: a roof gather + ~2-5 atlas loads + 3
 * atomics each; a few M ops a grid update: ~10-20 us); RESOLVE / STAGE touch 196 608 cells per grid
 * update (rgba8 load / store + 3 scratch words: ~3 MB of traffic: ~5-10 us); the scratch clear is a
 * fill of <= 9.4 MB (~5-10 us at 4 grids). 15 Hz x 4 avatars = 60 grid updates / s, spread at most
 * 4 a recording: ~0.03-0.05 ms in a recording with updates, well under 0.2 ms. Shading: one
 * trilinear 3D fetch per avatar fragment (two in the up / side / down blend bands) + the record's
 * directory and box (cached).
 */
#include "linden_common.h"

#include "llcleanweathervk.h"
#include "vk_pipeline.h"

#include "llagent.h"
#include "llerror.h"
#include "llformat.h"
#include "llmath.h"
#include "llmemtrend.h"
#include "lltimer.h"
#include "llviewercontrol.h"
#include "llviewerjointattachment.h"
#include "llvoavatarself.h"
#include "slweather.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace
{
// RenderVKWeatherAvatarAccumRes: the cells per grid (x, y, z: multiples of 4 -- the resolve's
// workgroups never straddle a grid), 3 buckets each.
constexpr uint32_t kAcResTable[3][3] = {{16, 16, 32}, {24, 24, 48}, {32, 32, 64}};
constexpr uint32_t kAcBuckets = 3;           // clean_weather_accum.glsl WX_AC_BUCKETS
constexpr uint32_t kAcScratchWords = 3;      // per cell / bucket: count, snow | wet, exposure
constexpr double   kAcRetrySecs = 10.0;
constexpr double   kAcSnapEvery = 2.0;       // s between own-grid snapshots
constexpr double   kAcSnapMaxAge = 900.0;    // s: an older snapshot is not restored
constexpr uint32_t kAcStatOffset = 1824;     // record: stat[16]
// (llcleanweathervk_shelter.cpp's constants: the per-avatar snow model)
constexpr float kAcCoverMelt = 0.0008f;      // m/s fully under cover (#903: snowing or not)
constexpr float kAcShedSpeed = 3.6f, kAcShedVSpeed = 1.6f;
constexpr float kAcSnowFull = 0.02f;         // clean_weather_accum.glsl kAcSnowFull

// clean_weather_accum.comp AcPush (80 B; the shelter layout's 128 B range).
struct AcPush
{
    uint32_t a[4];
    float    b[4], c[4], d[4], e[4];
};
static_assert(sizeof(AcPush) == 80, "AcPush = clean_weather_accum.comp AcPush");

// THE OWN AVATAR'S GRID SNAPSHOT (static: it outlives the module -- the weather may free it, e.g.
// after a teleport out of region weather, while the own avatar still carries snow; #904 master off
// clears it): the grid's texels, its rest box, the resolution, who, when; its max snow.
std::vector<uint8_t> s_snap;
float    s_snapBox[8] = {};
uint32_t s_snapRes[3] = {0, 0, 0};
LLUUID   s_snapId;
F64      s_snapT = 0.0;
bool     s_snapValid = false;
float    s_ownMaxSnow = 0.f;                 // m, the last readback (the module's snow hold)
F64      s_ownSnowT = 0.0;

// THE REST SKELETON (clean_weather_accum.glsl ac_rest; built once from the own avatar): per joint
// number the 3 rows of its rest transform (identity bone rotations: the skeleton's bind pose;
// attachment points keep their own rotation).
float s_rest[216 * 12] = {};
bool  s_restOk = false;

bool buildRestSkeleton()
{
    if (s_restOk) return true;
    LLVOAvatarSelf* av = gAgentAvatarp.get();
    if (!av || av->isDead() || !av->isBuilt()) return false;
    for (S32 j = 0; j < 216; ++j)
    {
        LLVector3 pos(0.f, 0.f, 0.f);
        LLQuaternion rot;   // identity
        if (LLJoint* jt = static_cast<LLVOAvatar*>(av)->getJoint(j))   // (LLVOAvatarSelf hides the S32 overload)
        {
            // the chain below the skeleton's root (the root's own position is the avatar's)
            std::vector<LLJoint*> chain;
            for (LLJoint* p = jt; p && p->getParent() && chain.size() < 64; p = p->getParent()) chain.push_back(p);
            for (auto it = chain.rbegin(); it != chain.rend(); ++it)
            {
                LLJoint* p = *it;
                const bool attach = dynamic_cast<LLViewerJointAttachment*>(p) != nullptr;
                const LLVector3 lp = attach ? p->getPosition() : p->getDefaultPosition();
                const LLQuaternion lr = attach ? p->getRotation() : LLQuaternion();
                pos += lp * rot;   // (LL: v * q rotates v by q)
                rot = lr * rot;    // (LL: a * b = a, then b)
            }
        }
        if (!pos.isFinite()) pos.clearVec();
        // rows r = (column 0 [r], column 1 [r], column 2 [r], translation [r]); column k = e_k * rot
        const LLVector3 c0 = LLVector3::x_axis * rot, c1 = LLVector3::y_axis * rot, c2 = LLVector3::z_axis * rot;
        float* w = s_rest + j * 12;
        for (int r = 0; r < 3; ++r)
        {
            w[r * 4 + 0] = c0.mV[r];
            w[r * 4 + 1] = c1.mV[r];
            w[r * 4 + 2] = c2.mV[r];
            w[r * 4 + 3] = pos.mV[r];
        }
    }
    s_restOk = true;
    LL_INFOS("VKWeather") << "weather avatar accumulation: rest skeleton built (pelvis rest z "
                          << s_rest[0 * 12 + 11] << " m)" << LL_ENDL;
    return true;
}

void acBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
               VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
{
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = srcStage; mb.srcAccessMask = srcAccess;
    mb.dstStageMask = dstStage; mb.dstAccessMask = dstAccess;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; d.memoryBarrierCount = 1; d.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &d);
}
} // namespace

LLCleanWeatherVK::AccumStats LLCleanWeatherVK::sAccumStats;

void LLCleanWeatherVK::clearAccumSnapshot()
{
    if (s_snapValid || s_ownMaxSnow > 0.f)
        LL_INFOS("VKWeather") << "weather avatar accumulation: own grid snapshot cleared (weather off)" << LL_ENDL;
    s_snapValid = false;
    s_snap.clear();
    s_snap.shrink_to_fit();
    s_ownMaxSnow = 0.f;
}

bool LLCleanWeatherVK::avatarSnowHold()
{
    static LLCachedControl<bool> s_acc(gSavedSettings, "RenderVKWeatherAvatarAccum", true);
    // (the last readback within a minute said the own avatar carries snow)
    return s_acc && s_ownMaxSnow > 1.0e-4f && LLTimer::getTotalSeconds() - s_ownSnowT < 60.0;
}

void LLCleanWeatherVK::registerAccumProbe()
{
    static bool s_done = false;
    if (s_done) return;
    s_done = true;
    LLMemTrend::registerProbe("vk-weather-accum", []
    {
        const AccumStats& s = sAccumStats;
        LLMemTrend::Sample out;
        out.count = s.grids;
        out.bytes = s.bytes;
        out.cap = 0;   // (a fixed pool: the bytes are the cap)
        out.peak = s.peakBytes;
        return out;
    });
}

void LLCleanWeatherVK::destroyAccum()
{
    if (mCtx)
    {
        if (mAcImg.image) mCtx->destroyImage(mAcImg);
        for (GpuBuffer* b : {&mAcRec, &mAcScratch, &mAcRead[0], &mAcRead[1], &mAcSnap[0], &mAcSnap[1]})
            if (b->buffer) mCtx->destroyBuffer(*b);
    }
    mAcImg = GpuImage{};
    mAcRec = mAcScratch = mAcRead[0] = mAcRead[1] = mAcSnap[0] = mAcSnap[1] = GpuBuffer{};
    mAcReadWritten[0] = mAcReadWritten[1] = mAcSnapWritten[0] = mAcSnapWritten[1] = false;
    mAcSnapGrid[0] = mAcSnapGrid[1] = -1;
    for (AcGrid& g : mAcGrid) g = AcGrid{};
    mAcPoolGrids = 0;
    mAcDue = false;
    mAcHeadDirty = true;
    mAcRestSent = false;
    mAcSlotsUsed = 0;
    mAcSeedGrid = mAcSelfGrid = -1;
    for (bool& d : mAcLayerDue) d = false;
    sAccumStats.bytes = 0;
    sAccumStats.grids = sAccumStats.gridCap = 0;
}

// The pool while wanted (device wait on a change: draw set 0 binding 50 / 51 and the shelter sets
// name it -- the renderer rewrites 50 / 51 before anything binds set 0 this recording). A refused
// allocation frees what it got, logs, and retries kAcRetrySecs later with half the grids.
bool LLCleanWeatherVK::ensureAccum(VkCommandBuffer cmd, bool want)
{
    static LLCachedControl<U32> s_res(gSavedSettings, "RenderVKWeatherAvatarAccumRes", 2);
    static LLCachedControl<U32> s_count(gSavedSettings, "RenderVKWeatherAvatarAccumCount", 16);
    if (want == (mAcRec.buffer != VK_NULL_HANDLE)) return want;
    if (want && mNow < mAcRetryAt) return false;
    if (!mCtx) return false;
    wxWaitIdle(mCtx->device(), "accumulation grids");
    destroyAccum();
    if (!want)
    {
        writeShelterSets(0, nullptr);
        writeShelterSets(1, nullptr);
        LL_INFOS("VKWeather") << "weather avatar accumulation freed" << LL_ENDL;
        return false;
    }
    // #946: the SMALL VRAM tier allocates 16 x 16 x 32 cells and at most 4 grids (1.6 MB with the
    // scratch, against 22 MB at the defaults' 32 x 32 x 64 x 16): it applies at the next allocation.
    const uint32_t* res = kAcResTable[sSmallVram ? 0u : std::min<U32>(s_res, 2u)];
    const uint32_t want0 = std::min(llclamp((U32)s_count, 1u, kAcGrids), sSmallVram ? 4u : kAcGrids);
    const uint32_t G = std::max(want0 >> std::min(mAcFailures, 4u), 1u);
    const uint64_t cells = (uint64_t)res[0] * res[1] * res[2] * kAcBuckets;
    const uint64_t gridBytes = cells * 4u;
    const uint64_t scratchBytes = (uint64_t)kAcSlots * cells * kAcScratchWords * 4u;
    bool ok;
    {
        ++sSync.recreates;   // #946 (the "weather sync" line)
        VkMemScope memcat(VKMEM_PARTICLE);   // #407 (the weather's category)
        VkDeviceContext::SoftAllocScope soft(mCtx);
        mAcImg = mCtx->createImage3D(res[0] * kAcBuckets, res[1], res[2] * G, VK_FORMAT_R8G8B8A8_UNORM,
                                     VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                                   | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        const VkBufferUsageFlags su = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
                                    | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        mAcRec = mCtx->createBuffer(kAcRecordBytes, su, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        mAcScratch = mCtx->createBuffer(scratchBytes, su, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        const VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (GpuBuffer& r : mAcRead)
            r = mCtx->createBuffer(kAcGpuBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, hv);
        for (GpuBuffer& r : mAcSnap)
            r = mCtx->createBuffer(gridBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, hv);
        ok = mAcImg.image && mAcImg.view && mAcRec.buffer && mAcScratch.buffer && mAcRead[0].mapped && mAcRead[1].mapped
          && mAcSnap[0].mapped && mAcSnap[1].mapped;
    }
    if (!ok)
    {
        ++mAcFailures;
        ++sAccumStats.failures;
        LL_WARNS("VKWeather") << "weather avatar accumulation (" << G << " grids, " << ((gridBytes * G + scratchBytes) >> 10)
                              << " KB) failed to allocate; the avatars keep the per-avatar values, retrying in " << kAcRetrySecs
                              << " s with " << std::max(want0 >> std::min(mAcFailures, 4u), 1u) << " grids" << LL_ENDL;
        destroyAccum();
        mAcRetryAt = mNow + kAcRetrySecs;
        return false;
    }
    mAcPoolGrids = G;
    std::copy(res, res + 3, mAcRes);
    registerAccumProbe();
    const uint64_t bytes = gridBytes * G + scratchBytes + kAcRecordBytes;
    sAccumStats.bytes = bytes;
    sAccumStats.peakBytes = std::max(sAccumStats.peakBytes, bytes);
    sAccumStats.gridCap = G;
    std::copy(res, res + 3, sAccumStats.res);
    // cleared (empty cells everywhere, an empty record, a zero scratch); GENERAL
    {
        VkImageMemoryBarrier2 ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        ib.dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT; ib.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        ib.image = mAcImg.image; ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; dep.imageMemoryBarrierCount = 1; dep.pImageMemoryBarriers = &ib;
        vkCmdPipelineBarrier2(cmd, &dep);
        const VkImageSubresourceRange rng{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkClearColorValue zero{};
        vkCmdClearColorImage(cmd, mAcImg.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &rng);
        vkCmdFillBuffer(cmd, mAcRec.buffer, 0, VK_WHOLE_SIZE, 0u);
        vkCmdFillBuffer(cmd, mAcScratch.buffer, 0, VK_WHOLE_SIZE, 0u);
        acBarrier(cmd, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT
                | VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
    }
    for (AcGrid& g : mAcGrid) g = AcGrid{};
    mAcHeadDirty = true;
    mAcRestSent = false;
    writeShelterSets(0, nullptr);
    writeShelterSets(1, nullptr);
    LL_INFOS("VKWeather") << "weather avatar accumulation: " << G << " grids of " << res[0] << " x " << res[1] << " x " << res[2]
                          << " cells x " << kAcBuckets << " buckets (rgba8, " << (gridBytes >> 10) << " KB a grid), scratch "
                          << (scratchBytes >> 10) << " KB, " << (bytes >> 10) << " KB" << LL_ENDL;
    return true;
}

// The shelter set's bindings 12 / 13 / 14 while the pool exists (appended to wr at n).
void LLCleanWeatherVK::writeAccumSets(uint32_t slot, VkWriteDescriptorSet* wr, VkDescriptorBufferInfo* bi,
                                      VkDescriptorImageInfo* ii, uint32_t& n)
{
    if (!mAcRec.buffer || !mAcScratch.buffer || !mAcImg.view || slot > 1) return;
    bi[0] = {mAcRec.buffer, 0, VK_WHOLE_SIZE};
    bi[1] = {mAcScratch.buffer, 0, VK_WHOLE_SIZE};
    *ii = {VK_NULL_HANDLE, mAcImg.view, VK_IMAGE_LAYOUT_GENERAL};
    for (uint32_t i = 0; i < 3; ++i)
    {
        VkWriteDescriptorSet& w = wr[n + i];
        w = VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = mShSet[slot]; w.dstBinding = 12 + i; w.descriptorCount = 1;
        w.descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        if (i < 2) w.pBufferInfo = &bi[i];
        else w.pImageInfo = ii;
    }
    n += 3;
}

// This recording's plan (planShelter, the layers chosen): the pool, the grids' owners, which are
// due (mAcLayerDue -> the shelter header's bit 4), the record's header, the push block.
void LLCleanWeatherVK::planAccum(VkCommandBuffer cmd, const Frame& f, uint32_t used)
{
    static LLCachedControl<bool> s_on(gSavedSettings, "RenderVKWeatherAvatarAccum", true);
    static LLCachedControl<F32>  s_rate(gSavedSettings, "RenderVKWeatherAvatarAccumRate", 15.f);
    static LLCachedControl<F32>  s_grace(gSavedSettings, "RenderVKWeatherAvatarAccumGrace", 20.f);
    static LLCachedControl<bool> s_snow(gSavedSettings, "RenderVKWeatherSnowAvatars", true);
    mAcDue = false;
    for (bool& d : mAcLayerDue) d = false;
    const bool pipes = mAcPipe[0] && mAcPipe[1] && mAcPipe[2] && mAcPipe[3];
    if (!ensureAccum(cmd, s_on && pipes && mShBuf.buffer && !f.offscreen) || f.offscreen) return;
    const SLWeather::State& st = SLWeather::instance().state();
    const uint32_t G = mAcPoolGrids;
    // OWNERS: each layer's avatar keeps / gets a grid (by id: a re-slotted avatar keeps its grid);
    // a grid whose avatar has no layer is freed after the grace period.
    int32_t layerOf[kAcGrids];
    std::fill(layerOf, layerOf + kAcGrids, -1);
    for (uint32_t g = 0; g < G; ++g)
    {
        AcGrid& gr = mAcGrid[g];
        if (gr.id.isNull()) continue;
        for (uint32_t L = 0; L < used; ++L)
            if (mShLayerSlot[L] >= 0 && mShLayerId[L] == gr.id)
            {
                layerOf[g] = (int32_t)L;
                if (gr.slot != mShLayerSlot[L]) mAcHeadDirty = true;
                gr.slot = mShLayerSlot[L];
                gr.lastSeen = mNow;
                break;
            }
        if (layerOf[g] < 0 && mNow - gr.lastSeen > (double)llclamp((F32)s_grace, 1.f, 600.f))
        {
            gr = AcGrid{};   // (left / not near for a while: free on liveness)
            mAcHeadDirty = true;
        }
    }
    for (uint32_t L = 0; L < used; ++L)
    {
        if (mShLayerSlot[L] < 0 || mShLayerId[L].isNull()) continue;
        bool has = false;
        for (uint32_t g = 0; g < G && !has; ++g) has = mAcGrid[g].id == mShLayerId[L];
        if (has) continue;
        // a free grid; else the one whose avatar has gone longest without a layer (never the own avatar's)
        int32_t pick = -1;
        double oldest = 1e30;
        for (uint32_t g = 0; g < G; ++g)
        {
            if (mAcGrid[g].id.isNull()) { pick = (int32_t)g; break; }
            if (layerOf[g] < 0 && !mAcGrid[g].self && mAcGrid[g].lastSeen < oldest) { oldest = mAcGrid[g].lastSeen; pick = (int32_t)g; }
        }
        if (pick < 0) continue;   // (pool full: this avatar keeps the per-avatar values)
        AcGrid& gr = mAcGrid[pick];
        gr = AcGrid{};
        gr.id = mShLayerId[L];
        gr.slot = mShLayerSlot[L];
        gr.self = mShLayerSelf[L];
        gr.lastSeen = mNow;
        gr.fresh = true;
        // the own avatar's snapshot (a new module after a weather-free spell / a teleport): restored
        if (gr.self && s_snapValid && s_snapId == gr.id && LLTimer::getTotalSeconds() - s_snapT < kAcSnapMaxAge
            && std::equal(s_snapRes, s_snapRes + 3, mAcRes) && s_snap.size() == (size_t)mAcRes[0] * mAcRes[1] * mAcRes[2] * kAcBuckets * 4u
            && mAcSeedGrid < 0)
        {
            gr.seed = true;
            mAcSeedGrid = pick;
        }
        layerOf[pick] = (int32_t)L;
        mAcHeadDirty = true;
    }
    // DUE: every grid with a layer at the update rate, the oldest first, at most kAcSlots
    const double period = 1.0 / (double)llclamp((F32)s_rate, 1.f, 60.f);
    struct Due { double age; uint32_t g; };
    Due dues[kAcGrids];
    uint32_t nd = 0;
    mAcSelfGrid = -1;
    for (uint32_t g = 0; g < G; ++g)
    {
        const AcGrid& gr = mAcGrid[g];
        if (gr.id.isNull()) continue;
        if (gr.self) mAcSelfGrid = (int32_t)g;
        if (layerOf[g] < 0) continue;
        const double age = gr.lastUpdate < 0.0 ? 1e9 : mNow - gr.lastUpdate;
        if (age >= period || gr.fresh) dues[nd++] = Due{age, g};
    }
    std::sort(dues, dues + nd, [](const Due& a, const Due& b) { return a.age > b.age; });
    nd = std::min(nd, kAcSlots);
    // the header (clean_weather_accum.glsl WxAccum, the first kAcHeadBytes)
    if (!mAcRestSent) buildRestSkeleton();
    uint32_t prevSlot[kAcGrids];
    for (uint32_t g = 0; g < kAcGrids; ++g) prevSlot[g] = mAcHead[8 + 4 * g];
    const uint32_t prevNear = mAcHead[2];
    std::memset(mAcHead, 0, sizeof(mAcHead));
    uint32_t* h = mAcHead;
    h[0] = G;
    h[1] = mAcRes[0] | (mAcRes[1] << 8) | (mAcRes[2] << 16);
    {
        // #909 NEAR SELF-COVER (clean_weather_accum.glsl wxAcNear; the ACCUM samples and the lit FS):
        // range cm | threshold mm << 8 | softness mm << 16 (range 0: off). The buckets are the
        // shaders' constant (WX_AC_BUCKETS), not read from here.
        static LLCachedControl<F32> s_nearRange(gSavedSettings, "RenderVKWeatherAvatarNearCoverRange", 0.25f);
        static LLCachedControl<F32> s_nearThr(gSavedSettings, "RenderVKWeatherAvatarNearCoverThreshold", 0.012f);
        static LLCachedControl<F32> s_nearSoft(gSavedSettings, "RenderVKWeatherAvatarNearCoverSoftness", 0.015f);
        const uint32_t rcm = (uint32_t)std::lround(llclamp((F32)s_nearRange, 0.f, 2.55f) * 100.f);
        const uint32_t tmm = (uint32_t)std::lround(llclamp((F32)s_nearThr, 0.f, 0.255f) * 1000.f);
        const uint32_t smm = (uint32_t)std::lround(llclamp((F32)s_nearSoft, 0.f, 0.255f) * 1000.f);
        h[2] = rcm | (tmm << 8) | (smm << 16);
        if (h[2] != prevNear) mAcHeadDirty = true;
    }
    h[3] = s_restOk ? 1u : 0u;
    for (uint32_t k = 0; k < 4; ++k) h[4 + k] = 0xFFFFFFFFu;
    for (uint32_t g = 0; g < kAcGrids; ++g)
    {
        uint32_t* c = &h[8 + 4 * g];
        // (the directory names a grid only while its avatar has a layer -- the slot it had may hold
        // another avatar by now; a grid in its grace period is kept, not shown)
        c[0] = (g < G && layerOf[g] >= 0 && mAcGrid[g].slot >= 0) ? (uint32_t)mAcGrid[g].slot : 0xFFFFFFFFu;
        if (c[0] != prevSlot[g]) mAcHeadDirty = true;
    }
    for (uint32_t k = 0; k < nd; ++k)
    {
        const uint32_t g = dues[k].g;
        AcGrid& gr = mAcGrid[g];
        uint32_t* c = &h[8 + 4 * g];
        const float dt = gr.fresh ? 0.f : (float)std::min(mNow - gr.lastUpdate, 0.5);
        c[1] = 1u | (gr.fresh && !gr.seed ? 2u : 0u) | (gr.seed ? 4u : 0u);
        c[2] = k;
        std::memcpy(&c[3], &dt, 4);
        h[4 + k] = g;
        gr.fresh = false;
        gr.seed = false;
        gr.lastUpdate = mNow;
        if (layerOf[g] >= 0) mAcLayerDue[layerOf[g]] = true;
        ++sAccumStats.updates;
    }
    mAcSlotsUsed = nd;
    // the rates (AcPush b .. e) -- per second; dt per grid
    {
        float* p = mAcPush;
        std::memset(mAcPush, 0, sizeof(mAcPush));
        p[3] = mShWetPush[3];                                                                   // b.w world wetness
        p[4] = st.snowDepth;                                                                    // c.x
        p[5] = std::max(st.snowDepthBuild, 0.f);                                                // c.y (#783: the state's rates)
        p[6] = std::max(st.snowDepthMelt, 0.f);                                                 // c.z
        p[7] = kAcCoverMelt;                                                                    // c.w
        p[8] = mShWetPush[0]; p[9] = mShWetPush[1]; p[10] = mShWetPush[2];                      // d.xyz rain, wet / s, dry / s
        p[11] = mTilt[0];                                                                       // d.w
        p[12] = mTilt[1]; p[13] = kAcShedSpeed; p[14] = kAcShedVSpeed;                          // e
        mAcFlags = (s_snow ? 1u : 0u) | (mShWetPush[6] > 0.5f ? 2u : 0u) | (mAvState.buffer ? 4u : 0u);
    }
    uint32_t live = 0;
    for (uint32_t g = 0; g < G; ++g) live += mAcGrid[g].id.isNull() ? 0u : 1u;
    sAccumStats.grids = live;
    sAccumStats.gridPeak = std::max(sAccumStats.gridPeak, live);
    mAcDue = nd > 0 || mAcHeadDirty || !mAcRestSent || mAcSeedGrid >= 0;
}

// After recordShelter (its work list / dispatch, its atlas just published), before the G-buffer.
void LLCleanWeatherVK::recordAccum(VkCommandBuffer cmd, VkDescriptorSet dsetDraw0, const ShelterInputs& in)
{
    if (!accumDue() || !dsetDraw0 || !mShSet[mSlot] || !mAcRec.buffer || !mAcImg.image || !mShBuf.buffer) return;
    mAcDue = false;
    const VkPipelineStageFlags2 C = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    const VkPipelineStageFlags2 T = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    const VkAccessFlags2 RW = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    // Earlier readers (the VS / FS of earlier recordings: the record and the grids; the last update's
    // kernels; the readback copies) -> the header / seed / scratch writes.
    acBarrier(cmd, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | C | VK_PIPELINE_STAGE_2_COPY_BIT,
              VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | RW | VK_ACCESS_2_TRANSFER_READ_BIT,
              T | C, VK_ACCESS_2_TRANSFER_WRITE_BIT | RW);
    vkCmdUpdateBuffer(cmd, mAcRec.buffer, 0, kAcHeadBytes, mAcHead);
    mAcHeadDirty = false;
    if (!mAcRestSent && s_restOk)
    {
        vkCmdUpdateBuffer(cmd, mAcRec.buffer, kAcRestOffset, sizeof(s_rest), s_rest);
        mAcRestSent = true;
    }
    if (mAcSeedGrid >= 0 && (uint32_t)mAcSeedGrid < mAcPoolGrids && mAcSnap[mSlot].mapped && s_snapValid)
    {
        // THE OWN AVATAR'S GRID, restored: its texels through this slot's snapshot buffer (its last
        // copy was read back in prepare()), its rest box into the record
        const uint32_t g = (uint32_t)mAcSeedGrid;
        std::memcpy(mAcSnap[mSlot].mapped, s_snap.data(), s_snap.size());
        mAcSnapWritten[mSlot] = false;
        VkBufferImageCopy bc{};
        bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bc.imageOffset = {0, 0, (int32_t)(g * mAcRes[2])};
        bc.imageExtent = {mAcRes[0] * kAcBuckets, mAcRes[1], mAcRes[2]};
        vkCmdCopyBufferToImage(cmd, mAcSnap[mSlot].buffer, mAcImg.image, VK_IMAGE_LAYOUT_GENERAL, 1, &bc);
        vkCmdUpdateBuffer(cmd, mAcRec.buffer, kAcGpuOffset + 32u * g, 32u, s_snapBox);
        LL_INFOS("VKWeather") << "weather avatar accumulation: own grid restored from its snapshot ("
                              << (F32)(LLTimer::getTotalSeconds() - s_snapT) << " s old)" << LL_ENDL;
    }
    mAcSeedGrid = -1;
    const uint64_t cells = (uint64_t)mAcRes[0] * mAcRes[1] * mAcRes[2] * kAcBuckets;
    if (mAcSlotsUsed)
        vkCmdFillBuffer(cmd, mAcScratch.buffer, 0, (VkDeviceSize)mAcSlotsUsed * cells * kAcScratchWords * 4u, 0u);
    acBarrier(cmd, T | C, VK_ACCESS_2_TRANSFER_WRITE_BIT | RW, C, RW);
    AcPush p{};
    p.a[0] = mShTilesH;
    p.a[1] = mAcFlags | (in.skinMeta ? 8u : 0u) | (in.skinVbo ? 16u : 0u);
    p.a[2] = mAcCounter++;
    p.a[3] = mAcSlotsUsed;
    p.b[0] = in.palEye[0]; p.b[1] = in.palEye[1]; p.b[2] = in.palEye[2];
    std::memcpy(&p.b[3], &mAcPush[3], 13 * sizeof(float));   // (b.w, c, d, e)
    // (the per-avatar wetness parameters: planShelter computed this recording's after planAccum ran)
    p.b[3] = mShWetPush[3];
    p.d[0] = mShWetPush[0]; p.d[1] = mShWetPush[1]; p.d[2] = mShWetPush[2];
    p.a[1] = (p.a[1] & ~2u) | (mShWetPush[6] > 0.5f ? 2u : 0u);
    // (the sets written by recordShelter this recording; #901 / #909 follow-up: + the cluster set)
    const bool cl = ensureShelterCluster(in);
    bindShelterSets(cmd, dsetDraw0, in, cl, &p, sizeof(p));
    // PREP: the directory, the due grids' boxes
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mAcPipe[0]);
    vkCmdDispatch(cmd, 1024u / 64u, 1, 1);
    if (mAcSlotsUsed)
    {
        acBarrier(cmd, C, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, C, RW);
        // ACCUM over the shelter chain's work list (its indirect dispatch: one workgroup an item)
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mAcPipe[1]);
        vkCmdDispatchIndirect(cmd, mShBuf.buffer, 16);
        if (cl)
        {
            // (#901 / #909 follow-up) the cluster-drawn parts of the due avatars (the skinned arena)
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mShClPipe[2]);
            vkCmdDispatchIndirect(cmd, in.clArgs, in.clArgsOffset);
        }
        acBarrier(cmd, C, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, C, RW);
        const uint32_t gx = mAcRes[0] * kAcBuckets / 4u, gy = mAcRes[1] / 4u, gz = mAcRes[2] * mAcSlotsUsed / 4u;
        // STAGE (a grown box: the old cells into the scratch), RESOLVE
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mAcPipe[2]);
        vkCmdDispatch(cmd, gx, gy, gz);
        acBarrier(cmd, C, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, C, RW);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mAcPipe[3]);
        vkCmdDispatch(cmd, gx, gy, gz);
    }
    // -> the VS (the record: the rest skeleton), the FS (the record, the grids), the next update, the
    // readback copies
    acBarrier(cmd, C | T, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
              VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | C | VK_PIPELINE_STAGE_2_COPY_BIT,
              VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | RW | VK_ACCESS_2_TRANSFER_READ_BIT);
    // READBACK: the boxes and stats every update; the own grid's texels every kAcSnapEvery s
    if (mAcSlotsUsed && mAcRead[mSlot].buffer)
    {
        const VkBufferCopy rc{kAcGpuOffset, 0, kAcGpuBytes};
        vkCmdCopyBuffer(cmd, mAcRec.buffer, mAcRead[mSlot].buffer, 1, &rc);
        mAcReadWritten[mSlot] = true;
    }
    if (mAcSelfGrid >= 0 && mNow >= mAcSnapAt && mAcSnap[mSlot].buffer && !mAcGrid[mAcSelfGrid].id.isNull()
        && mAcGrid[mAcSelfGrid].lastUpdate >= 0.0)
    {
        mAcSnapAt = mNow + kAcSnapEvery;
        const uint32_t g = (uint32_t)mAcSelfGrid;
        VkBufferImageCopy bc{};
        bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bc.imageOffset = {0, 0, (int32_t)(g * mAcRes[2])};
        bc.imageExtent = {mAcRes[0] * kAcBuckets, mAcRes[1], mAcRes[2]};
        vkCmdCopyImageToBuffer(cmd, mAcImg.image, VK_IMAGE_LAYOUT_GENERAL, mAcSnap[mSlot].buffer, 1, &bc);
        if (!mAcReadWritten[mSlot] && mAcRead[mSlot].buffer)
        {
            const VkBufferCopy rc{kAcGpuOffset, 0, kAcGpuBytes};   // (the box with it)
            vkCmdCopyBuffer(cmd, mAcRec.buffer, mAcRead[mSlot].buffer, 1, &rc);
            mAcReadWritten[mSlot] = true;
        }
        mAcSnapWritten[mSlot] = true;
        mAcSnapGrid[mSlot] = (int32_t)g;
        mAcSnapId[mSlot] = mAcGrid[g].id;
    }
    if (mAcReadWritten[mSlot] || mAcSnapWritten[mSlot])
        acBarrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                  VK_ACCESS_2_HOST_READ_BIT);
}

// prepare(): what this frame slot's last recording copied back (its fence waited): the boxes and
// stats (the own avatar's snow / wetness, the log), the own grid's snapshot.
void LLCleanWeatherVK::readAccumBack()
{
    static LLCachedControl<bool> s_dbg(gSavedSettings, "RenderVKWeatherAvatarAccumDebug", false);
    const F64 now = LLTimer::getTotalSeconds();
    static float s_ownWetMean = -1.f;
    static uint32_t s_ownCells = 0;
    if (mAcReadWritten[mSlot] && mAcRead[mSlot].mapped)
    {
        uint32_t w[kAcGpuBytes / 4];
        std::memcpy(w, mAcRead[mSlot].mapped, sizeof(w));
        if (mAcSelfGrid >= 0)
        {
            const uint32_t* st = &w[(kAcStatOffset - kAcGpuOffset) / 4 + 4 * (uint32_t)mAcSelfGrid];
            if (st[0] || st[3])
            {
                s_ownMaxSnow = (float)st[1] / 65535.f * kAcSnowFull;
                s_ownSnowT = now;
                s_ownCells = st[3];
                s_ownWetMean = st[3] ? (float)st[2] / (255.f * (float)st[3]) : 0.f;
            }
        }
        if (mAcSnapWritten[mSlot] && mAcSnap[mSlot].mapped && mAcSnapGrid[mSlot] >= 0)
        {
            const uint32_t g = (uint32_t)mAcSnapGrid[mSlot];
            float box[8];
            std::memcpy(box, &w[8 * g], sizeof(box));   // (box[2g], box[2g + 1]: the record's GPU part starts at box)
            if (box[3] > 0.5f)
            {
                const size_t bytes = (size_t)mAcRes[0] * mAcRes[1] * mAcRes[2] * kAcBuckets * 4u;
                s_snap.resize(bytes);
                std::memcpy(s_snap.data(), mAcSnap[mSlot].mapped, bytes);
                std::memcpy(s_snapBox, box, sizeof(box));
                std::copy(mAcRes, mAcRes + 3, s_snapRes);
                s_snapId = mAcSnapId[mSlot];
                s_snapT = now;
                s_snapValid = true;
            }
        }
    }
    mAcReadWritten[mSlot] = false;
    mAcSnapWritten[mSlot] = false;
    // the log: every ~10 s while the pool exists (2 s with the debug view)
    static F64 s_next = 0.0;
    static uint64_t s_prevUpdates = 0;
    if (!mAcRec.buffer || now < s_next) return;
    const F64 span = s_next > 0.0 ? now - (s_next - (s_dbg ? 2.0 : 10.0)) : 0.0;
    s_next = now + (s_dbg ? 2.0 : 10.0);
    const uint64_t du = sAccumStats.updates - s_prevUpdates;
    s_prevUpdates = sAccumStats.updates;
    LL_INFOS("VKWeather") << "weather avatar accumulation: grids " << sAccumStats.grids << "/" << sAccumStats.gridCap << " (peak "
                          << sAccumStats.gridPeak << "), " << mAcRes[0] << " x " << mAcRes[1] << " x " << mAcRes[2] << " x "
                          << kAcBuckets << ", " << (sAccumStats.bytes >> 10) << " KB; avatar updates " << du << " ("
                          << (span > 0.0 ? (F64)du / span : 0.0) << "/s); own grid: "
                          << (mAcSelfGrid >= 0 ? llformat("max snow %.2f cm, mean wetness %.2f over %u cells", s_ownMaxSnow * 100.f,
                                                          s_ownWetMean, s_ownCells)
                                               : std::string("none"))
                          << "; snapshot " << (s_snapValid ? llformat("%.0f s old", (F32)(now - s_snapT)) : std::string("none"))
                          << ", alloc failures " << sAccumStats.failures << LL_ENDL;
}
