/**
 * @file llcleanclusterstreamvk.cpp
 * @brief Cluster LOD residency / streaming (#746, OWNER: 1c): mesh registry, page pool,
 *        page table, request readback, closure-respecting LRU eviction, budgeted
 *        fence-gated page uploads (staging copies / indirect copies). See the header.
 *
 * Method: published material only (virtual geometry page streaming as presented in the
 * SIGGRAPH 2021 Nanite talk: fixed-size pages, root pages pinned, request feedback from the
 * GPU, LRU eviction with parent/child dependency closure). No engine source.
 */
#include "linden_common.h"

#include "llcleanclusterstreamvk.h"
#include "llcleanclustervk.h"
#include "llvrambudgetvk.h"
#include "../llcleanclusterbuild.h"

#include "llerror.h"
#include "llmemorygovernor.h"

#if defined(LL_HAS_CLEAN_PIPELINE)
#  include "llviewercontrol.h"
#  include "../llcleanmeshasset.h"
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <new>
#include <functional>

using namespace LLClusterFmt;

namespace
{
constexpr uint32_t kRequestCap      = 8192;                       // entries per frame slot
constexpr VkDeviceSize kReqStride   = ((16u + kRequestCap * 8u + 255u) / 256u) * 256u;
constexpr uint32_t kUpdateBytes     = 2u << 20;                   // per slot: table / mesh / group writes
constexpr uint32_t kMaxCopiesPerFrame = 1024;
// STAGING RING follows its demand (fixed-cost audit 2026-10-14): the page area per slot is the
// peak bytes staged per frame (+ 1/2) over two ~5 s windows, at least kMinStagingMB, at most
// RenderVKClusterUploadMBPerFrame; a frame whose staging was cut short by the ring grows it to the
// setting at once (next frame), a ring at 2x its want for kStagingShrinkFrames shrinks.
constexpr uint32_t kMinStagingMB    = 2;
constexpr uint64_t kStagingWinFrames = 300;
constexpr uint64_t kStagingShrinkFrames = 600;                     // page copies per frame
constexpr uint32_t kIndirectBytes   = kMaxCopiesPerFrame * 24u;   // VkCopyMemoryIndirectCommandKHR list
constexpr uint32_t kMaxTableWrites  = 8192;                       // flips + evictions per frame
constexpr uint32_t kMaxMeshWrites   = 512;                        // directory records per frame
constexpr uint32_t kMinPoolPages    = 64;                         // 5 MB
// Fixed-cost audit (2026-10-14): 1024 pages (80 MB), was 3277 (256 MB) -- the pool grows from
// there by growStep (x1.5 or the need), so a light scene never holds 256 MB for ~100 MB of pages.
constexpr uint32_t kFirstPoolPages  = 1024;
constexpr uint64_t kAutoPoolMin     = 64ull << 20;
constexpr uint64_t kAutoPoolMax     = 2048ull << 20;
constexpr uint64_t kGrowMarginBytes = 512ull << 20;               // VRAM room kept when growing
constexpr uint32_t kEvictMinAge     = 8;                          // frames unused before LRU may take it
// 2026-10-14 VRAM (the pool follows its demand): a page unused this long is COLD -- trimmed when
// the resident set is over its demand, and taken before the pool grows (an eighth of that under
// VRAM pressure). Pool sizes move in kPoolStep steps; a shrink waits kShrinkSec over the want.
constexpr uint32_t kColdAge         = 600;
constexpr uint32_t kPoolStep        = 1024;                       // 80 MB (the largest granularity)
// Pool size granularity for a pool of about `pages`: an eighth of it (power of two), 64 .. kPoolStep
// pages (5 .. 80 MB). A fixed 80 MB step + a 160 MB shrink margin held a light scene's pool at up to
// 3x its resident pages (Cloudsdale 2026-10-14: 1061 resident of 3277, the shrink rule never fired).
uint32_t poolGran(uint64_t pages)
{
    uint64_t p = 1;
    while (p < pages && p < (1ull << 31)) p <<= 1;
    return (uint32_t)std::clamp<uint64_t>(p / 8u, 64u, kPoolStep);
}
uint64_t roundUpTo(uint64_t v, uint64_t g) { return (v + g - 1) / g * g; }
constexpr double   kShrinkSec       = 10.0;
constexpr uint32_t kTrimPerTick     = 512;                        // cold pages trimmed per demand tick
// Registry entries not yet live. = the builder's queue bound (kQueueMax): a crowd's backlog is
// registered (and so prioritised by the builder) at once instead of first-come behind a full
// registry (1024 held a 52-avatar crowd's arrival order, 2026-10-13).
constexpr uint32_t kMaxPending      = 4096;
constexpr uint32_t kMetaPerFrame    = 64;                         // cache metadata asks per frame
constexpr uint32_t kDirFirst        = 256;
constexpr uint32_t kGroupFirst      = 16384;
constexpr float    kRootPrio        = 1.0e30f;
// Builder v2 (the lower DAG floor): a root is a handful of triangles, so a mesh also requests
// its first kWarmPages non-root pages (the coarsest levels, G5 order) right after its roots. Not
// pinned (LRU as usual). 2026-10-14 (the cluster path REPLACES the legacy one): the mesh goes Live
// -- drawn -- as soon as its ROOTS are resident; it no longer waits for the warm pages (they were
// there to hide the switch from a legacy copy, which no longer exists: first draw is what counts).
constexpr uint32_t kWarmPages       = 1;
constexpr uint32_t kRegsPerFrame    = 96;                         // registrations per frame
constexpr size_t   kTtfdSamples     = 4096;                       // time-to-drawable samples per summary
constexpr uint64_t kTicketTimeout   = 1200;                       // frames before a page read is abandoned
constexpr uint64_t kLoadedTimeout   = 600;                        // frames a loaded page may wait for a slot
constexpr double   kBuildTimeoutSec = 300.0;
constexpr double   kPayloadHoldSec  = 20.0;                       // a fresh build's payload serves its pages
constexpr double   kCalmRestoreSec  = 2.0;                        // pressure cap released after this calm
constexpr uint32_t kRebuildPerFrame = 8;                          // Live-mesh rebuild asks per frame
constexpr double   kStallLogSec     = 2.0;                        // summary reports a stall this old

// Creator LOD (0..3) at which the face publishes `slot`, -1 when none (the lookup of
// LLCleanClusterBuilder::request, which builds from that LOD).
int32_t slotCreatorLod(const LLClusterKey& key, uint32_t slot)
{
    if (slot == kNone) return -1;
#if defined(LL_HAS_CLEAN_PIPELINE)
    LLCleanMeshAsset& store = LLCleanMeshAsset::instance();
    for (S32 L = LLCleanMeshAsset::NUM_LOD - 1; L >= 0; --L)
        if (store.groupSlotAt(key.mesh_id, key.face, L) == slot) return L;
#else
    (void)key;
#endif
    return -1;
}

double nowSec()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

uint32_t alignUp32(uint32_t v, uint32_t a) { return (v + a - 1u) / a * a; }

VkPipelineStageFlags2 shaderStages(const VkClusterFeatures& f)
{
    VkPipelineStageFlags2 s = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT
                            | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT
                            | VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
#ifdef VK_EXT_mesh_shader
    if (f.meshShader) s |= VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT | VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT;
#else
    (void)f;
#endif
    return s;
}

void memBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds,
                VkAccessFlags2 da)
{
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = ss;
    mb.srcAccessMask = sa;
    mb.dstStageMask = ds;
    mb.dstAccessMask = da;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1;
    d.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &d);
}
} // namespace

// ============================================================================================
// small helpers
// ============================================================================================
uint32_t LLCleanClusterStreamVK::RangeAlloc::alloc(uint32_t n)
{
    if (!n) return 0;
    for (size_t i = 0; i < free.size(); ++i)
    {
        if (free[i].second < n) continue;
        const uint32_t s = free[i].first;
        free[i].first += n;
        free[i].second -= n;
        if (!free[i].second) free.erase(free.begin() + (ptrdiff_t)i);
        used += n;
        return s;
    }
    return kNone;
}

void LLCleanClusterStreamVK::RangeAlloc::release(uint32_t start, uint32_t n)
{
    if (!n || start == kNone) return;
    auto it = std::lower_bound(free.begin(), free.end(), std::make_pair(start, 0u));
    it = free.insert(it, {start, n});
    size_t i = (size_t)(it - free.begin());
    if (i + 1 < free.size() && free[i].first + free[i].second == free[i + 1].first)
    {
        free[i].second += free[i + 1].second;
        free.erase(free.begin() + (ptrdiff_t)(i + 1));
    }
    if (i > 0 && free[i - 1].first + free[i - 1].second == free[i].first)
    {
        free[i - 1].second += free[i].second;
        free.erase(free.begin() + (ptrdiff_t)i);
    }
    used = used >= n ? used - n : 0;
}

uint32_t LLCleanClusterStreamVK::RangeAlloc::highWater() const
{
    if (free.empty()) return cap;
    const auto& last = free.back();
    return last.first + last.second == cap ? last.first : cap;
}

void LLCleanClusterStreamVK::Rate::tick(double now, uint64_t add)
{
    n += add;
    if (t0 <= 0.0) { t0 = now; return; }
    const double dt = now - t0;
    if (dt >= 1.0)
    {
        per_sec = (double)n / dt;
        n = 0;
        t0 = now;
    }
}

void LLCleanClusterStreamVK::logOnce(const std::string& what, const std::string& text)
{
    // Throttled: one line per key per 10 s.
    auto it = mLogged.find(what);
    if (it != mLogged.end() && mNow - it->second < 10.0) return;
    if (mLogged.size() > 256) mLogged.clear();   // bounded
    mLogged[what] = mNow;
    LL_WARNS("Vulkan") << "Cluster LOD stream: " << text << LL_ENDL;
}

// ============================================================================================
// lifetime
// ============================================================================================
LLCleanClusterStreamVK::LLCleanClusterStreamVK(LLCleanClusterVK& owner) : mOwner(owner)
{
    static_assert(kSlots == LLCleanClusterVK::kFrameSlots, "one deferral list per frame slot");
}

LLCleanClusterStreamVK::~LLCleanClusterStreamVK() = default;

bool LLCleanClusterStreamVK::init()
{
    VkDeviceContext& ctx = mOwner.ctx();
    const VkClusterFeatures& f = mOwner.caps().feat;
    const VkDevice dev = ctx.device();

    // Indirect copies: fetched only when the extension was enabled (maintenance5 returns NULL
    // for names of extensions that are not).
#ifdef VK_KHR_copy_memory_indirect
    if (f.copyIndirectKHR)
    {
        mCopyIndirect = vkGetDeviceProcAddr(dev, "vkCmdCopyMemoryIndirectKHR");
        mCopyIndirectKHR = mCopyIndirect != nullptr;
    }
#endif
#ifdef VK_NV_copy_memory_indirect
    if (!mCopyIndirect && f.copyIndirectNV) mCopyIndirect = vkGetDeviceProcAddr(dev, "vkCmdCopyMemoryIndirectNV");
#endif

    // Queue family facts: indirect copies are only valid on the queues the device lists;
    // timestamps need timestampValidBits.
    uint32_t nfam = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(ctx.physicalDevice(), &nfam, nullptr);
    std::vector<VkQueueFamilyProperties> fams(nfam);
    if (nfam) vkGetPhysicalDeviceQueueFamilyProperties(ctx.physicalDevice(), &nfam, fams.data());
    const uint32_t qf = ctx.queueFamily();
    const VkQueueFlags qflags = qf < nfam ? fams[qf].queueFlags : 0;
    const uint32_t tsBits = qf < nfam ? fams[qf].timestampValidBits : 0;
    mIndirectQueueOk = mCopyIndirect != nullptr;
#ifdef VK_KHR_copy_memory_indirect
    if (mCopyIndirect)
    {
        // (the NV properties struct is an alias with the same sType)
        VkPhysicalDeviceCopyMemoryIndirectPropertiesKHR cp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COPY_MEMORY_INDIRECT_PROPERTIES_KHR};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &cp;
        vkGetPhysicalDeviceProperties2(ctx.physicalDevice(), &p2);
        mIndirectQueueOk = (cp.supportedQueues & qflags) != 0;
    }
#else
    (void)qflags;
#endif
    if (mCopyIndirect && !mIndirectQueueOk)
        LL_INFOS("Vulkan") << "Cluster LOD stream: indirect copies not supported on the graphics queue family; "
                              "staging copies used" << LL_ENDL;
    // #749: the async compute family's facts (page copies on a forked segment, or the whole
    // frame start inside the front-end segment under frame pipelining).
    if (ctx.asyncComputeAvailable())
    {
        const uint32_t cf = ctx.computeFamily();
        const VkQueueFlags cflags = cf < nfam ? fams[cf].queueFlags : 0;
        const uint32_t cts = cf < nfam ? fams[cf].timestampValidBits : 0;
        mIndirectAsyncOk = false;
#ifdef VK_KHR_copy_memory_indirect
        if (mCopyIndirect)
        {
            VkPhysicalDeviceCopyMemoryIndirectPropertiesKHR cp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COPY_MEMORY_INDIRECT_PROPERTIES_KHR};
            VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
            p2.pNext = &cp;
            vkGetPhysicalDeviceProperties2(ctx.physicalDevice(), &p2);
            mIndirectAsyncOk = (cp.supportedQueues & cflags) != 0;
        }
#else
        (void)cflags;
#endif
        mAsyncTsOk = cts != 0;
        mAsyncTsMask = cts >= 64 ? ~0ull : ((1ull << cts) - 1ull);
        LL_INFOS("Vulkan") << "Cluster LOD stream: async family " << cf << ": indirect copies "
                           << (mIndirectAsyncOk ? "yes" : "no") << ", timestamps " << (mAsyncTsOk ? "yes" : "no") << LL_ENDL;
    }

    const float period = ctx.limits().timestampPeriod;
    if (tsBits && period > 0.f)
    {
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = kSlots * 2;
        if (vkCreateQueryPool(dev, &qi, nullptr, &mTsPool) == VK_SUCCESS)
        {
            mTsOk = true;
            mTsPeriodNs = period;
            mTsMask = tsBits >= 64 ? ~0ull : ((1ull << tsBits) - 1ull);
        }
        else mTsPool = VK_NULL_HANDLE;
    }

    mVAlloc.reset(kMaxVirtualPages);
    mGAlloc.reset(kMaxGroupsTotal);
    if ((f.memDecompEXT || f.memDecompNV) && (f.decompressionMethods & 1ull))
        LL_INFOS("Vulkan") << "Cluster LOD stream: GPU memory decompression detected but held at indirect copies "
                              "(no GDeflate encoder in the tree yet; the cache stores uncompressed pages)" << LL_ENDL;
    return true;   // buffers are created lazily (the first beginFrame knows the VRAM room)
}

void LLCleanClusterStreamVK::destroy()
{
    VkDeviceContext& ctx = mOwner.ctx();
    for (GrowCopy& g : mGrowCopies) if (g.from.buffer) ctx.destroyBuffer(g.from);
    mGrowCopies.clear();
    for (GpuBuffer* b : {&mPool, &mTable, &mMeshes, &mGroups, &mReq, &mUsage, &mUsageRead, &mStaging})
        if (b->buffer) ctx.destroyBuffer(*b);
    if (mTsPool) { vkDestroyQueryPool(ctx.device(), mTsPool, nullptr); mTsPool = VK_NULL_HANDLE; }
    mTsOk = false;
    mMesh.clear();
    mShadowQ.clear();
    mFreeDirs.clear();
    mByKey.clear();
    mVRanges.clear();
    mSlotOwner.clear();
    mSlotUsed.clear();
    mFreeSlots.clear();
    mLoaded.clear();
    mLoadedFree.clear();
    mTickets.clear();
    for (Deferred& d : mDef) d = Deferred{};
    mPoolPages = mDirCap = mGroupCap = mUsageCap = 0;
    mUsedSlots = mLoadedCount = 0;
    mPayloadBytes = 0;
}

void LLCleanClusterStreamVK::releaseBuffers()
{
    for (GrowCopy& g : mGrowCopies) mOwner.retire(g.from);
    mGrowCopies.clear();
    mBufFills.clear();
    mRangeFills.clear();
    for (GpuBuffer* b : {&mPool, &mTable, &mMeshes, &mGroups, &mReq, &mUsage, &mUsageRead, &mStaging})
        mOwner.retire(*b);   // (null-safe; resets the handle)
    mPoolPages = mDirCap = mGroupCap = mUsageCap = 0;
    mStagingMB = 0;
    mStagingSlotBytes = mStagingPageBytes = 0;
    mPoolSettingMB = ~0u;
}

void LLCleanClusterStreamVK::releaseAll()
{
    // Free on liveness: nothing is kept for a return. The cache keeps the built meshes on disk.
    resetResidency(nullptr);
    mMesh.clear();
    mShadowQ.clear();
    mFreeDirs.clear();
    mByKey.clear();
    mPayloadBytes = 0;
    releaseBuffers();
    mSlotOwner.clear();
    mSlotUsed.clear();
    mFreeSlots.clear();
    mUsedSlots = 0;
    mPoolPeak = 0;
    mFrameOpen = false;
}

// Every registered mesh leaves the GPU at once (pool replaced / module released): the whole
// table and directory are refilled (kNone / generation 0 never matches), the meshes keep
// their metadata and re-register (state Ready) when the pool exists again.
void LLCleanClusterStreamVK::resetResidency(const char* why)
{
    if (why) LL_INFOS("Vulkan") << "Cluster LOD stream: residency reset (" << why << ")" << LL_ENDL;
    for (uint32_t i = 0; i < (uint32_t)mMesh.size(); ++i)
    {
        Mesh& m = mMesh[i];
        if (m.state == MS::Free) continue;
        if (m.registered)
        {
            mVAlloc.release(m.page_base, (uint32_t)m.pages.size());
            mGAlloc.release(m.group_base, m.data ? m.data->header.group_count : 0);
            m.registered = false;
        }
        m.pages.clear();
        m.roots_resident = 0;
        m.generation = m.generation + 1 ? m.generation + 1 : 1;
        if (m.state == MS::Rooting || m.state == MS::Live)
        {
            m.state = m.data ? MS::Ready : MS::Meta;
            m.retry_frame = 0;
        }
    }
    mVRanges.clear();
    // Ranges handed to deferral lists were released above or belong to freed meshes:
    // nothing on the GPU can reach them once the table / directory are refilled.
    for (Deferred& d : mDef)
    {
        for (auto& r : d.vranges) mVAlloc.release(r.first, r.second);
        for (auto& r : d.granges) mGAlloc.release(r.first, r.second);
        for (uint32_t dir : d.dirs) mFreeDirs.push_back(dir);
        d = Deferred{};
    }
    mLoaded.clear();
    mLoadedFree.clear();
    mLoadedCount = 0;
    mTickets.clear();   // late reads are ignored (no ticket)
    std::fill(mSlotOwner.begin(), mSlotOwner.end(), PageRef{kNone, 0, 0});
    mFreeSlots.clear();
    for (uint32_t s = mPoolPages; s-- > 0;) mFreeSlots.push_back(s);
    mUsedSlots = 0;
    mRootsPendingSlots = 0;
    mResidentPages = mResidentRoots = mLiveMeshes = 0;
    mTableWrites.clear();
    mMeshWrites.clear();
    mGroupWrites.clear();
    mCopies.clear();
    mRangeFills.clear();
    mTableFill = mTable.buffer != VK_NULL_HANDLE;
    mDirFill = mMeshes.buffer != VK_NULL_HANDLE;
}

// ============================================================================================
// settings, budgets, pressure
// ============================================================================================
void LLCleanClusterStreamVK::readSettings(const LLClusterFrameIn& in)
{
    uint32_t pool_mb = 0, upload_mb = 24;
    float grace = 30.f, build_ms = 2.f;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<U32> s_pool(gSavedSettings, "RenderVKClusterPagePoolMB", 0);
    static LLCachedControl<U32> s_up(gSavedSettings, "RenderVKClusterUploadMBPerFrame", 24);
    static LLCachedControl<F32> s_grace(gSavedSettings, "RenderVKClusterMeshGraceSec", 30.f);
    static LLCachedControl<F32> s_build(gSavedSettings, "RenderVKClusterBuildMsPerFrame", 2.f);
    pool_mb = s_pool;
    upload_mb = s_up;
    grace = s_grace;
    build_ms = s_build;
#endif
    upload_mb = std::clamp<uint32_t>(upload_mb, 1u, 256u);
    mGraceSec = std::max(0.f, grace);
    mBuildMs = std::max(0.f, build_ms);

    // Pool target (pages). A change of the setting re-targets; below the allocated size the
    // pool is rebuilt (ensurePool).
    if (pool_mb != mPoolSettingMB)
    {
        uint64_t bytes;
        if (pool_mb == 0)
        {
            const uint64_t room = in.vram_avail == ~0ull ? kAutoPoolMax * 6 : in.vram_avail;
            bytes = std::clamp<uint64_t>(room / 6, kAutoPoolMin, kAutoPoolMax);
        }
        else bytes = (uint64_t)std::min<uint32_t>(pool_mb, 4095u) << 20;
        uint64_t pages = bytes / kPageBytes;
        pages = std::min<uint64_t>(pages, kMaxPoolPages);
        const uint64_t range = mOwner.caps().maxStorageBufferRange;
        if (range) pages = std::min<uint64_t>(pages, range / kPageBytes);
        mPoolTarget = (uint32_t)std::max<uint64_t>(pages, kMinPoolPages);
        if (mPool.buffer && mPoolTarget < mPoolPages)
        {
            resetResidency("page pool setting below the current pool");
            mOwner.retire(mPool);
            mOwner.retire(mUsage);
            mOwner.retire(mUsageRead);
            mPoolPages = mUsageCap = 0;
            mSlotOwner.clear();
            mSlotUsed.clear();
            mFreeSlots.clear();
            for (Deferred& d : mDef) d.usage_copied = false;
        }
        mPoolSettingMB = pool_mb;
    }

    // RAM pressure (LLMemoryGovernor): CPU-side page buffers + retained payloads shrink.
    const LLMemoryGovernor::EState ram = LLMemoryGovernor::getState();
    const uint32_t ram_div = ram == LLMemoryGovernor::CRITICAL ? 4u : ram == LLMemoryGovernor::TIGHT ? 2u : 1u;
    mRamPressure = ram_div > 1u;
    // The staging ring's page area: the DEMAND (see kMinStagingMB), never past the setting. Before,
    // it was the setting itself: 3 x (24 + 2) MB of host-visible staging held whether anything
    // streamed or not (a still scene stages nothing).
    {
        const uint64_t peak = std::max(mStagePeakCur, mStagePeakPrev);
        const uint32_t demand_mb = (uint32_t)std::min<uint64_t>((peak + peak / 2 + (1ull << 20) - 1) >> 20, 4096u);
        const uint32_t want = std::clamp<uint32_t>(demand_mb, kMinStagingMB, std::max(upload_mb, kMinStagingMB));
        uint32_t next = mStagingWantMB ? mStagingWantMB : kMinStagingMB;
        if (mStagingStarved || next > upload_mb) { next = std::max(upload_mb, kMinStagingMB); mStagingSmallSince = 0; }
        else if (want * 2u <= next)
        {
            if (!mStagingSmallSince) mStagingSmallSince = mFrame ? mFrame : 1;
            else if (mFrame >= mStagingSmallSince + kStagingShrinkFrames) { next = want; mStagingSmallSince = 0; }
        }
        else mStagingSmallSince = 0;
        mStagingWantMB = next;
        mStagingStarved = false;
    }
    mUploadBytesBudget = ((uint64_t)upload_mb << 20) / ram_div;   // (planUploads caps it to the ring)
    const uint32_t pages_per_frame = std::max<uint32_t>(1u, (uint32_t)(mUploadBytesBudget / kPageBytes));
    mLoadCap = std::clamp<uint32_t>(pages_per_frame * 2u, 16u, 512u) / ram_div;
    mLoadCap = std::max<uint32_t>(mLoadCap, 8u);
    mPayloadBudget = ram == LLMemoryGovernor::CRITICAL ? 0 : ram == LLMemoryGovernor::TIGHT ? (64ull << 20) : (256ull << 20);

    // VRAM pressure: the pool's EFFECTIVE cap drops at once and is restored after a calm
    // period (soft degrade; settings untouched).
    // Ladder depth as a fraction of its rungs: any rung -> 3/4, a third -> 1/2, two thirds
    // (the soft steps nearly spent) -> 1/4.
    uint32_t cap = mPoolPages;
    const uint32_t rungs = in.vram_ladder;
    const uint32_t total = std::max<uint32_t>(LLVramBudgetVK::rungCount(), 3u);
    if (rungs * 3u >= total * 2u) cap = mPoolPages / 4;
    else if (rungs * 3u >= total) cap = mPoolPages / 2;
    else if (rungs >= 1) cap = mPoolPages * 3 / 4;
    if (in.vram_avail != ~0ull && in.vram_avail < (256ull << 20)) cap = std::min(cap, mPoolPages / 2);
    if (cap < mPoolPages)
    {
        mPressureCap = mPressureCap ? std::min(mPressureCap, cap) : cap;
        mCalmSince = 0.0;
    }
    else if (mPressureCap)
    {
        if (mCalmSince <= 0.0) mCalmSince = mNow;
        else if (mNow - mCalmSince >= kCalmRestoreSec) { mPressureCap = 0; mCalmSince = 0.0; }
    }
    mVramPressure = rungs > 0 || (in.vram_avail != ~0ull && in.vram_avail < kGrowMarginBytes);
    mVramAvail = in.vram_avail;
    mEffCap = effectiveCap();
}

uint32_t LLCleanClusterStreamVK::pendingFree() const
{
    size_t n = 0;
    for (const Deferred& d : mDef) n += d.slots.size();
    return (uint32_t)n;
}

uint32_t LLCleanClusterStreamVK::effectiveCap() const
{
    uint32_t cap = mPoolPages;
    if (mPressureCap) cap = std::min(cap, mPressureCap);
    // Never below the pinned roots (they cannot leave while their mesh lives).
    return std::max(cap, std::min(mPoolPages, mResidentRoots + 16u));
}

// ============================================================================================
// buffers
// ============================================================================================
bool LLCleanClusterStreamVK::ensureBuffers(const LLClusterFrameIn& in)
{
    VkDeviceContext& ctx = mOwner.ctx();
    if (mFrame < mAllocRetryFrame) return mTable.buffer && mMeshes.buffer && mGroups.buffer && mReq.buffer
                                          && mStaging.buffer && mPool.buffer;
    bool failed = false;
    if (!mTable.buffer)
    {
        VkMemScope scope(VKMEM_SCENE);
        VkDeviceContext::SoftAllocScope soft(&ctx);
        mTable = ctx.createBuffer((VkDeviceSize)kMaxVirtualPages * 4u,
                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mTable.buffer)
        {
            mOwner.setBuffer(CLB_PAGE_TABLE, mTable);
            mTableFill = true;
        }
        else failed = true;
    }
    if (!mReq.buffer)
    {
        VkMemScope scope(VKMEM_CULL);
        VkDeviceContext::SoftAllocScope soft(&ctx);
        const VkBufferUsageFlags u = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT
                                   | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        mReq = ctx.createBuffer(kReqStride * kSlots, u,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                    | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        if (mReq.buffer && !mReq.mapped) ctx.destroyBuffer(mReq);
        if (!mReq.buffer)
        {
            mReq = ctx.createBuffer(kReqStride * kSlots, u,
                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            if (mReq.buffer && !mReq.mapped) ctx.destroyBuffer(mReq);
        }
        if (mReq.buffer)
        {
            std::memset(mReq.mapped, 0, kReqStride * kSlots);
            for (uint32_t s = 0; s < kSlots; ++s)
            {
                ClusterRequestHeader* h = (ClusterRequestHeader*)((uint8_t*)mReq.mapped + kReqStride * s);
                h->count = 0;
                h->cap = kRequestCap;
                h->frame = 0;
                mOwner.setBuffer(CLB_REQUESTS, mReq, kReqStride * s, kReqStride, s);
            }
        }
        else failed = true;
    }
    if (!failed && !ensureDirCap(kDirFirst)) failed = true;
    if (!failed && !ensureGroupCap(kGroupFirst)) failed = true;
    if (!failed && !ensureStaging()) failed = true;
    if (!failed && !ensurePool(in)) failed = true;
    if (failed)
    {
        ++mAllocFails;
        const uint64_t backoff = std::min<uint64_t>(120ull << std::min<uint32_t>(mAllocFails, 4u), 1800ull);
        mAllocRetryFrame = mFrame + backoff;
        logOnce("alloc", "buffer allocation failed (VRAM / RAM); cluster meshes stay on the legacy path, retry in "
                             + std::to_string(backoff) + " frames");
        return mTable.buffer && mMeshes.buffer && mGroups.buffer && mReq.buffer && mStaging.buffer && mPool.buffer;
    }
    mAllocFails = 0;
    return true;
}

bool LLCleanClusterStreamVK::ensureDirCap(uint32_t n)
{
    if (n <= mDirCap) return true;
    if (n > kMaxMeshes) return false;
    uint32_t cap = std::max(kDirFirst, mDirCap);
    while (cap < n) cap *= 2;
    cap = std::min(cap, kMaxMeshes);
    VkDeviceContext& ctx = mOwner.ctx();
    VkMemScope scope(VKMEM_SCENE);
    VkDeviceContext::SoftAllocScope soft(&ctx);
    GpuBuffer nb = ctx.createBuffer((VkDeviceSize)cap * sizeof(ClusterMesh),
                                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
                                        | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!nb.buffer) return false;
    // Fresh entries: zero (generation 0 never matches an instance).
    mBufFills.push_back({nb.buffer, 0, nb.size, 0u});
    if (mMeshes.buffer) mGrowCopies.push_back({mMeshes, nb, (VkDeviceSize)mDirCap * sizeof(ClusterMesh)});
    mMeshes = nb;
    mDirCap = cap;
    mOwner.setBuffer(CLB_MESHES, mMeshes);
    return true;
}

bool LLCleanClusterStreamVK::ensureGroupCap(uint32_t n)
{
    if (n <= mGroupCap) return true;
    if (n > kMaxGroupsTotal) return false;
    uint32_t cap = std::max(kGroupFirst, mGroupCap);
    while (cap < n) cap *= 2;
    cap = std::min(cap, kMaxGroupsTotal);
    VkDeviceContext& ctx = mOwner.ctx();
    VkMemScope scope(VKMEM_SCENE);
    VkDeviceContext::SoftAllocScope soft(&ctx);
    GpuBuffer nb = ctx.createBuffer((VkDeviceSize)cap * sizeof(ClusterGroup),
                                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
                                        | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!nb.buffer) return false;
    if (mGroups.buffer) mGrowCopies.push_back({mGroups, nb, (VkDeviceSize)mGroupCap * sizeof(ClusterGroup)});
    mGroups = nb;
    mGroupCap = cap;
    mOwner.setBuffer(CLB_GROUPS, mGroups);
    return true;
}

bool LLCleanClusterStreamVK::ensureStaging()
{
    if (mStaging.buffer && mStagingMB == mStagingWantMB) return true;
    if (mStaging.buffer && mFrame < mStagingRetryFrame) return true;   // keep the old size meanwhile
    VkDeviceContext& ctx = mOwner.ctx();
    const uint64_t page_area = (uint64_t)mStagingWantMB << 20;
    const uint64_t slot_bytes = page_area + kUpdateBytes + kIndirectBytes;
    VkMemScope scope(VKMEM_STAGING);
    VkDeviceContext::SoftAllocScope soft(&ctx);
    const bool addr = mOwner.caps().native[CLCAP_UPLOAD] >= CLT_UPLOAD_INDIRECT;
    GpuBuffer nb = ctx.createBuffer(slot_bytes * kSlots,
                                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, addr);
    if (nb.buffer && !nb.mapped) ctx.destroyBuffer(nb);
    if (!nb.buffer)
    {
        mStagingRetryFrame = mFrame + 600;
        if (mStaging.buffer)
        {
            logOnce("staging", "staging resize failed; keeping " + std::to_string(mStagingMB) + " MB per frame");
            return true;
        }
        return false;
    }
    mOwner.retire(mStaging);   // (frames in flight may still copy from it)
    mStaging = nb;
    mStagingMB = mStagingWantMB;
    mStagingSlotBytes = slot_bytes;
    mStagingPageBytes = page_area;
    mStagingDeviceLocal = ctx.allocDeviceLocal(mStaging.memory);
    return true;
}

bool LLCleanClusterStreamVK::ensurePool(const LLClusterFrameIn& /*in*/)
{
    if (mPool.buffer) return true;
    return growPool(std::min(mPoolTarget, kFirstPoolPages));
}

// The pool grows in steps: a new buffer, the old content copied (slot indices survive),
// the old one retired. The usage stamps and their readback follow.
bool LLCleanClusterStreamVK::growPool(uint32_t pages)
{
    pages = std::min(pages, kMaxPoolPages);
    if (pages <= mPoolPages) return false;
    mShrinkTo = 0;   // (a grow ends any shrink in progress)
    mShrinkSince = 0.0;
    VkDeviceContext& ctx = mOwner.ctx();
    const LLCleanClusterCaps& caps = mOwner.caps();
    VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
                             | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
                             | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (caps.native[CLCAP_RT] >= CLT_RT_BLAS) usage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    GpuBuffer np, nu, nr;
    {
        VkMemScope scope(VKMEM_GEOMETRY);
        VkDeviceContext::SoftAllocScope soft(&ctx);
        np = ctx.createBuffer((VkDeviceSize)pages * kPageBytes, usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true);
    }
    if (np.buffer)
    {
        VkMemScope scope(VKMEM_CULL);
        VkDeviceContext::SoftAllocScope soft(&ctx);
        nu = ctx.createBuffer((VkDeviceSize)pages * 4u,
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT
                                  | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    }
    if (nu.buffer)
    {
        VkMemScope scope(VKMEM_READBACK);
        VkDeviceContext::SoftAllocScope soft(&ctx);
        nr = ctx.createBuffer((VkDeviceSize)pages * 4u * kSlots, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                  | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        if (nr.buffer && !nr.mapped) ctx.destroyBuffer(nr);
    }
    if (!np.buffer || !nu.buffer || !nr.buffer)
    {
        if (np.buffer) ctx.destroyBuffer(np);
        if (nu.buffer) ctx.destroyBuffer(nu);
        if (nr.buffer) ctx.destroyBuffer(nr);
        mLastGrowFrame = mFrame + 600;   // back off (the recordUploads "grew" test never matches it)
        logOnce("pool", "page pool step to " + std::to_string(((uint64_t)pages * kPageBytes) >> 20)
                            + " MB failed; running with " + std::to_string(((uint64_t)mPoolPages * kPageBytes) >> 20)
                            + " MB");
        return false;
    }
    std::memset(nr.mapped, 0, (size_t)pages * 4u * kSlots);
    mBufFills.push_back({nu.buffer, 0, nu.size, 0u});
    if (mPool.buffer)
    {
        mGrowCopies.push_back({mPool, np, (VkDeviceSize)mPoolPages * kPageBytes});
        mPool = GpuBuffer{};
    }
    if (mUsage.buffer)
    {
        mGrowCopies.push_back({mUsage, nu, (VkDeviceSize)mPoolPages * 4u});
        mUsage = GpuBuffer{};
    }
    mOwner.retire(mUsageRead);
    for (Deferred& d : mDef) d.usage_copied = false;   // (the old readback is gone)
    mPool = np;
    mUsage = nu;
    mUsageRead = nr;
    mUsageCap = pages;
    mSlotOwner.resize(pages, PageRef{kNone, 0, 0});
    mSlotUsed.resize(pages, 0);
    {   // new slots go under the existing free ones (popped from the back: low slots first)
        std::vector<uint32_t> fresh;
        fresh.reserve(pages - mPoolPages);
        for (uint32_t s = pages; s-- > mPoolPages;) fresh.push_back(s);
        mFreeSlots.insert(mFreeSlots.begin(), fresh.begin(), fresh.end());
    }
    mPoolPages = pages;
    mOwner.setBuffer(CLB_POOL, mPool);
    mOwner.setBuffer(CLB_PAGE_USAGE, mUsage);
    mLastGrowFrame = mFrame;
    LL_INFOS("Vulkan") << "Cluster LOD stream: page pool " << (((uint64_t)pages * kPageBytes) >> 20) << " MB ("
                       << pages << " pages, target " << mPoolTarget << ")" << LL_ENDL;
    return true;
}

uint32_t LLCleanClusterStreamVK::growStep(uint32_t need_free) const
{
    // half again the pool or what is needed past the slots in use, in kPoolStep steps (the old
    // doubling quantised the pool to 256 / 512 / 1024 / 2048 MB)
    const uint64_t want = std::max<uint64_t>((uint64_t)mPoolPages + mPoolPages / 2u, (uint64_t)mUsedSlots + need_free + 256u);
    const uint64_t stepped = roundUpTo(want, poolGran(want));
    return (uint32_t)std::min<uint64_t>(std::min<uint64_t>(stepped, mPoolTarget), kMaxPoolPages);
}

// Every 30 frames. THE DEMAND: resident pages the selection used in the last kColdAge frames
// (the cut + its warm neighbourhood) + the pinned roots. Over it (+1/4, +1/8 under VRAM pressure,
// + an LRU slack of an eighth of the resident set, 64 .. 512 pages) the coldest pages are trimmed;
// a pool over the want (demand + 1/8, in poolGran granules) + max(two granules, a quarter) for
// kShrinkSec shrinks to it -- slots past the new size are drained first (allocations
// take the lowest free slots, pages there are evicted and stream back below), then the smaller
// pool replaces the old one with the prefix copied. Roots never move: a root past the want caps
// the shrink (logged; a relocation pass would lift it).
void LLCleanClusterStreamVK::demandTick()
{
    if (!mPool.buffer || !mPoolPages) return;
    const uint32_t now = (uint32_t)mFrame;
    uint32_t ws = 0, roots = 0, max_root = 0, max_busy = 0;
    uint64_t fill = 0, fill_roots = 0;
    uint32_t fill_n = 0;
    for (uint32_t s = 0; s < mPoolPages; ++s)
    {
        const PageRef& o = mSlotOwner[s];
        if (o.mesh >= mMesh.size()) continue;
        max_busy = s + 1u;
        const Mesh& m = mMesh[o.mesh];
        if (m.generation != o.gen || o.page >= m.pages.size()) continue;
        const Page& pg = m.pages[o.page];
        if (pg.slot != s) continue;
        if (pg.st == PG_RESIDENT) { fill += pg.used; ++fill_n; if (pg.root) fill_roots += pg.used; }
        if (pg.root) { ++roots; max_root = s + 1u; continue; }
        if (pg.last_used + kColdAge >= now) ++ws;
    }
    mWsPages = ws;
    mWsRoots = roots;
    mFillBytes = fill;
    mFillRootBytes = fill_roots;
    mFillPages = fill_n;
    mFillRoots = roots;
    mMaxRootSlot = max_root;
    const uint32_t head = mVramPressure ? ws / 8u : ws / 4u;
    // LRU slack in proportion to the working set (fixed-cost audit 2026-10-14: was a flat 512 pages
    // = 40 MB, under pressure 128): an eighth of it, 64 .. 512 pages (32 .. 128 under pressure).
    const uint32_t slack = mVramPressure ? std::clamp<uint32_t>((roots + ws) / 16u, 32u, 128u)
                                         : std::clamp<uint32_t>((roots + ws) / 8u, 64u, 512u);
    mDemandPages = roots + ws + head + slack;
    // Trim: cold pages past the demand (oldest first; non-force: nothing wanted this frame).
    const uint32_t held = mUsedSlots - std::min(mUsedSlots, pendingFree());
    if (held > mDemandPages)
        mDemandEvicts += evictLRU(std::min(held - mDemandPages, kTrimPerTick), mVramPressure ? kColdAge / 8u : kColdAge, false);
    // Pool want: never past the target; never below the highest root (roots do not move).
    // (+ an eighth: the head room and slack are already in the demand; was + a quarter)
    uint64_t want = (uint64_t)mDemandPages + mDemandPages / 8u;
    const uint32_t gran = poolGran(want);
    want = roundUpTo(want, gran);
    want = std::clamp<uint64_t>(want, kMinPoolPages, std::max<uint32_t>(mPoolTarget, kMinPoolPages));
    mPoolWant = (uint32_t)want;
    const uint32_t floor_root = (max_root + 63u) / 64u * 64u;
    const uint32_t to = std::max<uint32_t>(mPoolWant, floor_root);
    if (mShrinkTo)
    {
        // demand came back (or a root landed past it): give up; else drain the slots past it
        if (mPoolWant > mShrinkTo || floor_root > mShrinkTo) { mShrinkTo = 0; mShrinkSince = 0.0; return; }
        // allocations take the lowest free slots (mFreeSlots pops from the back)
        std::sort(mFreeSlots.begin(), mFreeSlots.end(), std::greater<uint32_t>());
        for (uint32_t s = mShrinkTo, n = 0; s < mPoolPages && n < kTrimPerTick && mTableWrites.size() < kMaxTableWrites; ++s)
        {
            const PageRef o = mSlotOwner[s];
            if (o.mesh >= mMesh.size()) continue;
            const Mesh& m = mMesh[o.mesh];
            if (m.generation != o.gen || o.page >= m.pages.size()) continue;
            const Page& pg = m.pages[o.page];
            if (pg.slot != s || pg.st != PG_RESIDENT || pg.root || pg.last_used + kEvictMinAge > now) continue;
            if (evictPage(o.mesh, o.page)) ++n;   // (a page with dependents goes on a later tick)
        }
        uint32_t free_past = 0;
        for (uint32_t s : mFreeSlots) free_past += s >= mShrinkTo ? 1u : 0u;
        // (#749: the prefix copy reads the old pool on graphics -- copies forked to an async segment
        // in the last kFrameSlots frames must have aged out first, as for a grow)
        if (free_past == mPoolPages - mShrinkTo && max_busy <= mShrinkTo
            && !(mAsyncCopyFrame && mFrame < mAsyncCopyFrame + kSlots))
            shrinkPool(mShrinkTo);
        return;
    }
    // (shrink margin: two granules or a quarter of the want, whichever is larger -- was 2 x 80 MB)
    if (mPoolPages <= to + std::max<uint32_t>(2u * gran, to / 4u)) { mShrinkSince = 0.0; mShrinkBlocked = 0; return; }
    if (mShrinkSince <= 0.0) { mShrinkSince = mNow; return; }
    if (mNow - mShrinkSince < (mVramPressure ? kShrinkSec / 4.0 : kShrinkSec)) return;
    if (mFrame < mLastGrowFrame + 600u) return;
    mShrinkTo = to;
    mShrinkBlocked = floor_root > mPoolWant ? floor_root - mPoolWant : 0u;
    LL_INFOS("Vulkan") << "Cluster LOD stream: page pool shrink " << (((uint64_t)mPoolPages * kPageBytes) >> 20) << " -> "
                       << (((uint64_t)to * kPageBytes) >> 20) << " MB (demand " << mDemandPages << " pages: working set "
                       << ws << " + roots " << roots << "; resident " << mResidentPages << ")"
                       << (mShrinkBlocked ? " -- capped by a root page at slot " + std::to_string(max_root - 1u) : std::string())
                       << LL_ENDL;
}

bool LLCleanClusterStreamVK::shrinkPool(uint32_t pages)
{
    if (!mPool.buffer || pages >= mPoolPages || pages < kMinPoolPages) { mShrinkTo = 0; return false; }
    VkDeviceContext& ctx = mOwner.ctx();
    const LLCleanClusterCaps& caps = mOwner.caps();
    VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
                             | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
                             | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (caps.native[CLCAP_RT] >= CLT_RT_BLAS) usage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    GpuBuffer np, nu, nr;
    {
        VkMemScope scope(VKMEM_GEOMETRY);
        VkDeviceContext::SoftAllocScope soft(&ctx);
        np = ctx.createBuffer((VkDeviceSize)pages * kPageBytes, usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true);
    }
    if (np.buffer)
    {
        VkMemScope scope(VKMEM_CULL);
        VkDeviceContext::SoftAllocScope soft(&ctx);
        nu = ctx.createBuffer((VkDeviceSize)pages * 4u,
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT
                                  | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    }
    if (nu.buffer)
    {
        VkMemScope scope(VKMEM_READBACK);
        VkDeviceContext::SoftAllocScope soft(&ctx);
        nr = ctx.createBuffer((VkDeviceSize)pages * 4u * kSlots, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                  | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        if (nr.buffer && !nr.mapped) ctx.destroyBuffer(nr);
    }
    if (!np.buffer || !nu.buffer || !nr.buffer)
    {
        // (old + new do not fit for the copy: keep the old pool; retried after the next wait)
        if (np.buffer) ctx.destroyBuffer(np);
        if (nu.buffer) ctx.destroyBuffer(nu);
        if (nr.buffer) ctx.destroyBuffer(nr);
        mShrinkTo = 0;
        mShrinkSince = 0.0;
        logOnce("poolshrink", "page pool shrink to " + std::to_string(((uint64_t)pages * kPageBytes) >> 20)
                                  + " MB failed (no room for the copy); keeping "
                                  + std::to_string(((uint64_t)mPoolPages * kPageBytes) >> 20) + " MB");
        return false;
    }
    std::memset(nr.mapped, 0, (size_t)pages * 4u * kSlots);
    // The prefix (every slot still in use is below `pages`) -- recordUploads copies it before this
    // frame's page copies; the old buffers retire there (frames in flight keep reading them).
    mGrowCopies.push_back({mPool, np, (VkDeviceSize)pages * kPageBytes});
    mGrowCopies.push_back({mUsage, nu, (VkDeviceSize)pages * 4u});
    mOwner.retire(mUsageRead);
    for (Deferred& d : mDef) d.usage_copied = false;   // (the old readback is gone)
    const uint32_t old = mPoolPages;
    mPool = np;
    mUsage = nu;
    mUsageRead = nr;
    mUsageCap = pages;
    mSlotOwner.resize(pages);
    mSlotUsed.resize(pages);
    mFreeSlots.erase(std::remove_if(mFreeSlots.begin(), mFreeSlots.end(), [pages](uint32_t s) { return s >= pages; }),
                     mFreeSlots.end());
    mPoolPages = pages;
    mPoolPeak = std::min(mPoolPeak, pages);
    mOwner.setBuffer(CLB_POOL, mPool);
    mOwner.setBuffer(CLB_PAGE_USAGE, mUsage);
    mLastGrowFrame = mFrame;
    mEffCap = effectiveCap();
    mShrinkTo = 0;
    mShrinkSince = 0.0;
    ++mShrinks;
    LL_INFOS("Vulkan") << "Cluster LOD stream: page pool shrunk " << (((uint64_t)old * kPageBytes) >> 20) << " -> "
                       << (((uint64_t)pages * kPageBytes) >> 20) << " MB (" << pages << " pages; in use " << mUsedSlots
                       << ", demand " << mDemandPages << ")" << LL_ENDL;
    return true;
}

uint64_t LLCleanClusterStreamVK::heldBytes() const
{
    uint64_t b = 0;
    for (const GpuBuffer* x : {&mPool, &mTable, &mMeshes, &mGroups, &mReq, &mUsage, &mUsageRead, &mStaging})
        b += x->buffer ? x->size : 0;
    return b;
}

// ============================================================================================
// registry
// ============================================================================================
LLCleanClusterStreamVK::MeshRef LLCleanClusterStreamVK::acquireMesh(const LLClusterKey& key, uint32_t src_slot,
                                                                     uint64_t frame, float importance, uint32_t group,
                                                                     uint32_t* hint)
{
    uint32_t idx;
    // (a non-Free entry is exactly the one mByKey maps its key to: registration inserts a key
    // only when absent, freeEntry erases it as the entry goes Free)
    if (hint && *hint < mMesh.size() && mMesh[*hint].state != MS::Free && mMesh[*hint].key == key
        && mMesh[*hint].shadow_of == kNone)   // (#798: never a pending upgrade shadow)
        idx = *hint;
    else if (auto it = mByKey.find(key); it != mByKey.end())
        idx = it->second;
    else
    {
        if (mPending >= kMaxPending) { ++mRegRefused; return MeshRef{}; }
        if (!mFreeDirs.empty())
        {
            idx = mFreeDirs.back();
            mFreeDirs.pop_back();
        }
        else
        {
            if (mMesh.size() >= kMaxMeshes) return MeshRef{};
            try { mMesh.emplace_back(); }
            catch (const std::bad_alloc&) { return MeshRef{}; }
            idx = (uint32_t)mMesh.size() - 1u;
        }
        Mesh& m = mMesh[idx];
        const uint32_t gen = m.generation ? m.generation : 1u;
        m = Mesh{};
        m.generation = gen;
        m.key = key;
        m.state = MS::Meta;
        m.state_time = mNow;
        m.first_time = mNow > 0.0 ? mNow : 1e-9;
        try { mByKey.emplace(key, idx); }
        catch (const std::bad_alloc&) { m.state = MS::Free; mFreeDirs.push_back(idx); return MeshRef{}; }
        ++mPending;
    }
    if (hint) *hint = idx;
    Mesh& m = mMesh[idx];
    m.live_frame = frame;
    m.live_time = mNow;
    m.src_slot = src_slot;
    const float imp = std::isfinite(importance) ? std::max(importance, 0.f) : 0.f;
    if (m.prio_frame != frame || imp > m.prio)
    {
        m.prio = imp;
        m.prio_group = group;
        m.prio_frame = frame;
    }
    // (#798 upgrade swap: the shadow is never acquired itself -- it lives and streams as its key does)
    if (m.shadow < mMesh.size() && mMesh[m.shadow].shadow_of == idx)
    {
        Mesh& s = mMesh[m.shadow];
        s.live_frame = m.live_frame;
        s.live_time = m.live_time;
        s.src_slot = src_slot;
        s.prio = m.prio;
        s.prio_group = m.prio_group;
        s.prio_frame = m.prio_frame;
    }
    if (m.state == MS::Live) return MeshRef{idx, m.generation};
    return MeshRef{};
}

uint32_t LLCleanClusterStreamVK::meshOf(uint32_t vpage, uint32_t& local) const
{
    // mVRanges: (page_base, mesh) sorted by base.
    auto it = std::upper_bound(mVRanges.begin(), mVRanges.end(), std::make_pair(vpage, kNone));
    if (it == mVRanges.begin()) return kNone;
    --it;
    const uint32_t idx = it->second;
    if (idx >= mMesh.size()) return kNone;
    const Mesh& m = mMesh[idx];
    if (!m.registered || vpage < m.page_base || vpage >= m.page_base + (uint32_t)m.pages.size()) return kNone;
    local = vpage - m.page_base;
    return idx;
}

void LLCleanClusterStreamVK::writeMeshRecord(uint32_t idx, bool live)
{
    const Mesh& m = mMesh[idx];
    MeshWrite w;
    std::memset(&w.rec, 0, sizeof(w.rec));
    w.dir = idx;
    if (live && m.data)
    {
        const ClusterFileHeader& h = m.data->header;
        w.rec = h.mesh;   // qheader + sphere
        setMeshLod0Tris(w.rec, h.tri_count_lod0);   // the stats' full-detail baseline (kFormatVersion 3)
        w.rec.page_base = m.page_base;
        w.rec.page_count = h.page_count;
        w.rec.root_pages = h.root_pages;
        w.rec.group_base = m.group_base;
        w.rec.group_count = h.group_count;
        w.rec.cluster_count = h.cluster_count;
        w.rec.flags = h.flags;
    }
    w.rec.generation = m.generation;   // dead record: counts 0, generation bumped
    mMeshWrites.push_back(w);
    mUpdPlanned += sizeof(ClusterMesh);
}

// Metadata checks beyond the builder's validate(): what this module indexes with.
static bool metaSane(const LLClusterMeshData& d, std::string& why)
{
    const ClusterFileHeader& h = d.header;
    if (h.page_count == 0 || h.page_count != d.pages.size()) { why = "page count"; return false; }
    if (h.root_pages == 0 || h.root_pages > h.page_count) { why = "root pages"; return false; }
    if (h.group_count < kFirstRealGroup || h.group_count != d.groups.size()) { why = "group count"; return false; }
    if (h.page_count > (kMaxVirtualPages >> 4)) { why = "too many pages"; return false; }
    if ((uint64_t)h.group_count * sizeof(ClusterGroup) > kUpdateBytes / 2) { why = "group table too large"; return false; }
    for (uint32_t p = 0; p < h.page_count; ++p)
    {
        const ClusterPageDir& pd = d.pages[p];
        if (pd.used_bytes > kPageBytes || pd.used_bytes < kPageHeaderBytes) { why = "page size"; return false; }
        if ((uint64_t)pd.dep_first + pd.dep_count > d.deps.size()) { why = "dep range"; return false; }
        for (uint32_t k = 0; k < pd.dep_count; ++k)
        {
            const uint32_t par = d.deps[pd.dep_first + k];
            // G5: parents are coarser = earlier pages; roots have no parents outside the roots.
            if (par >= p) { why = "dep order"; return false; }
        }
    }
    for (uint32_t g = kFirstRealGroup; g < h.group_count; ++g)
        if (d.groups[g].page != kNone && d.groups[g].page >= h.page_count) { why = "group page"; return false; }
    return true;
}

bool LLCleanClusterStreamVK::tryRegister(uint32_t idx)
{
    Mesh& m = mMesh[idx];
    const ClusterFileHeader& h = m.data->header;
    if (!mPool.buffer) return false;
    // Room: directory, virtual pages, groups, the update area, and pool slots for the roots
    // within the effective cap (LRU makes room for the next try when it can).
    if (mMeshWrites.size() + 1 > kMaxMeshWrites) return false;
    const uint32_t gbytes = h.group_count * (uint32_t)sizeof(ClusterGroup);
    if (updRoom() < gbytes + sizeof(ClusterMesh)) return false;
    const uint32_t avail = mEffCap > mUsedSlots ? std::min<uint32_t>(mEffCap - mUsedSlots, (uint32_t)mFreeSlots.size()) : 0;
    if (avail < h.root_pages + mRootsPendingSlots)
    {
        // (2026-10-14: cold pages go before the pool grows; the step follows the need)
        const uint32_t need = h.root_pages + mRootsPendingSlots - avail;
        uint32_t cold = 0;
        if (mRegEvictFrame != mFrame && need > pendingFree())
        {
            mRegEvictFrame = mFrame;
            cold = evictLRU(need - pendingFree(), mVramPressure ? kColdAge / 8u : kColdAge, false);
        }
        if (cold + pendingFree() >= need) {}
        else if (mPoolPages < mPoolTarget && !mVramPressure && mFrame >= mLastGrowFrame + kSlots)
            growPool(growStep(need + 64u));
        else if (mRegEvictFrame != mFrame && h.root_pages + mRootsPendingSlots > avail + pendingFree()
                 && (mRegEvictFrame = mFrame, true))
            evictLRU(h.root_pages + mRootsPendingSlots - avail - pendingFree(), kEvictMinAge, false);
        logOnce("roomroots", "no pool room for root pages yet (pool " + std::to_string(mPoolPages) + " pages, used "
                                 + std::to_string(mUsedSlots) + ", cap " + std::to_string(mEffCap) + ")");
        return false;
    }
    if (!ensureDirCap(idx + 1)) return false;
    const uint32_t vbase = mVAlloc.alloc(h.page_count);
    if (vbase == kNone) { logOnce("vpages", "virtual page table full"); return false; }
    const uint32_t gbase = mGAlloc.alloc(h.group_count);
    if (gbase == kNone || !ensureGroupCap(gbase + h.group_count))
    {
        mVAlloc.release(vbase, h.page_count);
        if (gbase != kNone) mGAlloc.release(gbase, h.group_count);
        logOnce("groups", "group table full");
        return false;
    }
    try
    {
        m.pages.assign(h.page_count, Page{});
        auto it = std::lower_bound(mVRanges.begin(), mVRanges.end(), std::make_pair(vbase, 0u));
        mVRanges.insert(it, {vbase, idx});
    }
    catch (const std::bad_alloc&)
    {
        m.pages.clear();
        mVAlloc.release(vbase, h.page_count);
        mGAlloc.release(gbase, h.group_count);
        return false;
    }
    for (uint32_t p = 0; p < h.root_pages; ++p) m.pages[p].root = true;
    m.page_base = vbase;
    m.group_base = gbase;
    m.registered = true;
    m.roots_resident = 0;
    // The range's table entries: kNone (a previous owner's values are unreachable but stale).
    mRangeFills.push_back({vbase, h.page_count});
    mGroupWrites.push_back({gbase, m.data});
    mUpdPlanned += gbytes;
    writeMeshRecord(idx, true);
    m.state = MS::Rooting;
    m.state_time = mNow;
    mRootsPendingSlots += h.root_pages;
    return true;
}

// The mesh leaves the GPU: its record goes dead (generation bumped, counts 0) this frame,
// its pages' slots / ranges / directory index are freed kFrameSlots frames later.
void LLCleanClusterStreamVK::unregister(uint32_t idx, bool keep_entry)
{
    Mesh& m = mMesh[idx];
    Deferred& d = mDef[mSlot];
    if (m.registered)
    {
        for (uint32_t p = 0; p < (uint32_t)m.pages.size(); ++p)
        {
            Page& pg = m.pages[p];
            switch (pg.st)
            {
            case PG_LOADED:
                if (pg.loaded != kNone && pg.loaded < mLoaded.size())
                {
                    mLoaded[pg.loaded].bytes = std::vector<uint8_t>();
                    mLoaded[pg.loaded].ref.mesh = kNone;
                    mLoadedFree.push_back(pg.loaded);
                    --mLoadedCount;
                }
                break;
            case PG_STAGED:
            case PG_RESIDENT:
                if (pg.slot != kNone) d.slots.push_back(pg.slot);
                if (pg.st == PG_RESIDENT) { --mResidentPages; if (pg.root) --mResidentRoots; }
                break;
            default:
                break;   // LOADING: the read's ticket no longer matches; COOLING: its slot is already deferred
            }
        }
        if (m.state == MS::Rooting && m.data)
        {
            const uint32_t left = m.data->header.root_pages > m.roots_resident ? m.data->header.root_pages - m.roots_resident : 0;
            mRootsPendingSlots = mRootsPendingSlots > left ? mRootsPendingSlots - left : 0;
        }
        auto it = std::lower_bound(mVRanges.begin(), mVRanges.end(), std::make_pair(m.page_base, 0u));
        if (it != mVRanges.end() && it->first == m.page_base) mVRanges.erase(it);
        d.vranges.push_back({m.page_base, (uint32_t)m.pages.size()});
        d.granges.push_back({m.group_base, m.data ? m.data->header.group_count : 0u});
        m.registered = false;
        m.generation = m.generation + 1 ? m.generation + 1 : 1;
        writeMeshRecord(idx, false);
    }
    if (m.state == MS::Live && mLiveMeshes) --mLiveMeshes;
    m.pages.clear();
    m.pages.shrink_to_fit();
    m.roots_resident = 0;
    if (!keep_entry) freeEntry(idx);
}

void LLCleanClusterStreamVK::freeEntry(uint32_t idx)
{
    Mesh& m = mMesh[idx];
    if (m.payload && m.data) mPayloadBytes -= std::min<uint64_t>(mPayloadBytes, m.data->payload.size());
    // (only the entry the key maps to: a #798 shadow is not in mByKey, and a freed live entry's
    // key may already map to its shadow)
    if (auto it = mByKey.find(m.key); it != mByKey.end() && it->second == idx) mByKey.erase(it);
    const uint32_t gen = m.generation;
    const uint32_t sh = m.shadow, so = m.shadow_of;
    m = Mesh{};
    m.generation = gen;
    m.state = MS::Free;
    mDef[mSlot].dirs.push_back(idx);   // reusable once no frame in flight saw the old record
    // #798 upgrade swap links: a freed shadow leaves its live entry as it was (it may rebuild again);
    // a freed live entry (liveness) takes its shadow with it.
    if (so < mMesh.size() && mMesh[so].shadow == idx) mMesh[so].shadow = kNone;
    if (sh < mMesh.size() && mMesh[sh].shadow_of == idx)
    {
        mMesh[sh].shadow_of = kNone;
        unregister(sh, false);
    }
}

uint32_t LLCleanClusterStreamVK::allocDir()
{
    if (!mFreeDirs.empty())
    {
        const uint32_t idx = mFreeDirs.back();
        mFreeDirs.pop_back();
        return idx;
    }
    if (mMesh.size() >= kMaxMeshes) return kNone;
    try { mMesh.emplace_back(); }
    catch (const std::bad_alloc&) { return kNone; }
    return (uint32_t)mMesh.size() - 1u;
}

// #798 CLEAN UPGRADE SWAP, step 1 (after advanceMeshes' loop: allocDir may grow mMesh): each queued
// live entry's rebuilt DAG (a higher creator LOD) becomes a SHADOW entry in Ready -- registered,
// its roots uploaded like any mesh -- while the live entry keeps drawing. No entry free: the old
// way (unregister + re-register in place: the mesh is not drawn while its new roots load).
void LLCleanClusterStreamVK::startShadows()
{
    for (const uint32_t idx : mShadowQ)
    {
        if (idx >= mMesh.size() || mMesh[idx].shadow != kShadowQueued) continue;
        std::shared_ptr<const LLClusterMeshData> data;
        data.swap(mMesh[idx].rebuilt);
        mMesh[idx].shadow = kNone;
        if (!data || mMesh[idx].state != MS::Live) continue;
        const uint32_t sh = allocDir();
        if (sh == kNone)
        {
            // (no entry: rebuildDone's in-place swap, next frame)
            Mesh& o = mMesh[idx];
            o.rebuilding = true;
            o.rebuilt = data;
            o.rebuilt_ready = true;
            o.shadow = kShadowInPlace;   // (rebuildDone: swap in place, do not queue again)
            continue;
        }
        Mesh& s = mMesh[sh];
        const Mesh& o = mMesh[idx];   // (after allocDir: mMesh may have grown)
        const uint32_t gen = s.generation ? s.generation : 1u;
        s = Mesh{};
        s.generation = gen;
        s.key = o.key;
        s.src_slot = o.src_slot;
        s.lod_slot = o.src_slot;
        s.live_frame = o.live_frame;
        s.live_time = o.live_time;
        s.prio = o.prio;
        s.prio_group = o.prio_group;
        s.prio_frame = o.prio_frame;
        s.first_time = o.first_time;
        s.ttfd_done = true;   // (not a first draw)
        const int8_t lod = o.rebuild_lod;
        takeData(s, data);
        s.rebuild_lod = std::max(lod, s.rebuild_lod);
        s.validated = false;
        s.from_builder = true;   // validate() already ran on the worker (runBuild)
        s.state = MS::Ready;
        s.state_time = mNow;
        s.retry_frame = 0;
        s.shadow_of = idx;
        mMesh[idx].shadow = sh;
    }
    mShadowQ.clear();
}

// #798 CLEAN UPGRADE SWAP, step 2: shadow `idx` has its roots resident -- it takes its key over and
// the entry it replaces is released (dead record, deferred reuse), in the same frame, before the
// gather acquires: the key draws from the old DAG until this frame and from the new one from it.
// False: not this frame (no room for the dead record) or the shadow was dropped.
bool LLCleanClusterStreamVK::shadowSwap(uint32_t idx)
{
    Mesh& s = mMesh[idx];
    const uint32_t o = s.shadow_of;
    const bool linked = o < mMesh.size() && mMesh[o].shadow == idx && mMesh[o].state != MS::Free && mMesh[o].key == s.key;
    if (linked && (mMeshWrites.size() + 1 > kMaxMeshWrites || updRoom() < sizeof(ClusterMesh))) return false;
    s.shadow_of = kNone;
    if (linked)
    {
        mMesh[o].shadow = kNone;
        unregister(o, false);   // (frees the entry and its key mapping)
    }
    auto it = mByKey.find(mMesh[idx].key);
    if (it != mByKey.end() && it->second != idx)
    {
        unregister(idx, false);   // (an orphan: the key has another entry meanwhile)
        return false;
    }
    if (it == mByKey.end())
    {
        try { mByKey.emplace(mMesh[idx].key, idx); }
        catch (const std::bad_alloc&) { unregister(idx, false); return false; }
    }
    ++mRebuildSwaps;
    return true;
}

void LLCleanClusterStreamVK::failMesh(uint32_t idx, const char* why, bool drop_cache)
{
    Mesh& m = mMesh[idx];
    logOnce(std::string("fail:") + why, std::string("mesh ") + m.key.mesh_id.asString() + " face "
                                            + std::to_string(m.key.face) + " dropped: " + why);
    if (drop_cache) LLCleanClusterCache::instance().drop(m.key);
    if (m.shadow_of != kNone)
    {
        // (#798: a failed upgrade shadow is dropped; the live entry keeps drawing what it has)
        unregister(idx, false);
        return;
    }
    unregister(idx, true);
    if (m.payload && m.data) mPayloadBytes -= std::min<uint64_t>(mPayloadBytes, m.data->payload.size());
    m.data.reset();
    m.payload = false;
    m.state = MS::Failed;
    m.state_time = mNow;
    m.retry_frame = mFrame + 3600;   // ~1 min: a rebuild (the cache file was dropped)
}

void LLCleanClusterStreamVK::livenessSweep()
{
    // Free on liveness: no acquire for RenderVKClusterMeshGraceSec -> everything is released.
    uint32_t pending = 0, done = 0;
    for (uint32_t i = 0; i < (uint32_t)mMesh.size(); ++i)
    {
        Mesh& m = mMesh[i];
        if (m.state == MS::Free) continue;
        if (m.state != MS::Live && m.state != MS::Failed) ++pending;
        if (mNow - m.live_time <= mGraceSec) continue;
        // (two dead records: a #798 upgrade shadow goes with its entry)
        if (done >= 64 || mMeshWrites.size() + 2 > kMaxMeshWrites || updRoom() < 2 * sizeof(ClusterMesh)) continue;
        if (m.state != MS::Live && m.state != MS::Failed && pending) --pending;
        unregister(i, false);
        ++done;
    }
    mPending = pending;
}

// ============================================================================================
// per frame
// ============================================================================================
void LLCleanClusterStreamVK::beginFrame(const LLClusterFrameIn& in)
{
    mFrameOpen = false;
    mSlot = in.slot % kSlots;
    mFrame = in.frame;
    mNow = nowSec();
    mTableWrites.clear();
    mMeshWrites.clear();
    mGroupWrites.clear();
    mCopies.clear();
    mRangeFills.clear();
    mUpdPlanned = 0;
    mCopyBytes = 0;
    mReqScratch.clear();
    mRegRefusedLast = mRegRefused;
    mRegRefused = 0;

    readSettings(in);
    if (!ensureBuffers(in))
    {
        // Not every buffer exists yet (first frames, or an allocation backing off): nothing
        // can be registered, so nothing is written; builds and reads keep going.
        pollSources(in);
        return;
    }
    mEffCap = effectiveCap();
    harvest(mSlot);
    pollSources(in);
    advanceMeshes(in);
    if ((mFrame % 15u) == 0u) livenessSweep();
    // Pressure: evict down to the effective cap (soft; restored when calm).
    const uint32_t held = mUsedSlots - std::min(mUsedSlots, pendingFree());
    if (held > mEffCap) evictLRU(held - mEffCap, 1, true);
    if ((mFrame % 30u) == 0u) demandTick();
    planUploads();
    // Stall: pages wanted (not started) and nothing copied this frame, since mStallSince.
    if (mWantNoSource + mWantBusy + mWantCap == 0 || !mCopies.empty()) mStallSince = 0.0;
    else if (mStallSince <= 0.0) mStallSince = mNow;
    // (every frame, so the rates fall to 0 when nothing moves instead of holding the last value)
    mUploadsTotal += mCopies.size();
    mUploadBytesTotal += mCopyBytes;
    // staging demand: the bytes staged per frame, peak over two windows (readSettings)
    if (mFrame >= mStageWinFrame + kStagingWinFrames)
    {
        mStagePeakPrev = mStagePeakCur;
        mStagePeakCur = 0;
        mStageWinFrame = mFrame;
    }
    mStagePeakCur = std::max<uint64_t>(mStagePeakCur, mCopyBytes);
    mUpRate.tick(mNow, mCopyBytes);
    mUpPageRate.tick(mNow, mCopies.size());
    mFrameOpen = true;
}

// This slot's frame finished (its fence was waited): its copies are complete, its readbacks
// valid, its deferred frees safe.
void LLCleanClusterStreamVK::harvest(uint32_t s)
{
    Deferred& d = mDef[s];
    VkDeviceContext& ctx = mOwner.ctx();

    // Upload GPU time.
    if (d.ts_written && mTsOk)
    {
        uint64_t r[4] = {};
        const VkResult res = vkGetQueryPoolResults(ctx.device(), mTsPool, s * 2u, 2, sizeof(r), r, 2 * sizeof(uint64_t),
                                                   VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        if ((res == VK_SUCCESS || res == VK_NOT_READY) && r[1] && r[3])
        {
            const uint64_t dt = (r[2] - r[0]) & (d.ts_async ? mAsyncTsMask : mTsMask);
            mUploadGpuMs = (float)((double)dt * mTsPeriodNs * 1e-6);
            mUploadGpuMsMax = std::max(mUploadGpuMsMax * 0.995f, mUploadGpuMs);
        }
        else ++mStalls;   // not complete when the slot came round: the fence logic would be broken
    }
    d.ts_written = false;

    // Page usage stamps (LRU).
    if (d.usage_copied && mUsageRead.mapped && d.usage_pages <= mPoolPages && d.usage_pages <= mUsageCap)
    {
        const uint32_t* u = (const uint32_t*)((const uint8_t*)mUsageRead.mapped + (size_t)s * mUsageCap * 4u);
        for (uint32_t i = 0; i < d.usage_pages; ++i)
        {
            if (u[i] <= mSlotUsed[i]) continue;
            mSlotUsed[i] = u[i];
            const PageRef& o = mSlotOwner[i];
            if (o.mesh < mMesh.size() && mMesh[o.mesh].generation == o.gen && o.page < mMesh[o.mesh].pages.size())
            {
                Page& pg = mMesh[o.mesh].pages[o.page];
                if (pg.slot == i) pg.last_used = std::max(pg.last_used, u[i]);
            }
        }
    }
    d.usage_copied = false;

    // Requests the slot's frame appended (host-visible), then reset for its next frame.
    if (mReq.mapped)
    {
        ClusterRequestHeader* h = (ClusterRequestHeader*)((uint8_t*)mReq.mapped + kReqStride * s);
        const uint32_t n = std::min(h->count, kRequestCap);
        if (h->count > kRequestCap) mReqOverflow += h->count - kRequestCap;
        const ClusterRequest* r = (const ClusterRequest*)(h + 1);
        try { mReqScratch.assign(r, r + n); }
        catch (const std::bad_alloc&) { mReqScratch.clear(); }
        mReqRate.tick(mNow, n);
        h->count = 0;
        h->cap = kRequestCap;
        h->frame = (uint32_t)mFrame;
    }

    // Take the lists out: anything deferred WHILE harvesting (a mesh failing below) belongs
    // to this frame and waits a full round.
    Deferred cur;
    cur.staged.swap(d.staged);
    cur.cooling.swap(d.cooling);
    cur.slots.swap(d.slots);
    cur.vranges.swap(d.vranges);
    cur.granges.swap(d.granges);
    cur.dirs.swap(d.dirs);

    // Copies recorded kFrameSlots frames ago are complete: flip (parents first: per mesh the
    // pages run coarse -> fine, G5).
    std::sort(cur.staged.begin(), cur.staged.end(), [](const PageRef& a, const PageRef& b)
              { return a.mesh != b.mesh ? a.mesh < b.mesh : a.page < b.page; });
    for (const PageRef& r : cur.staged)
    {
        if (r.mesh >= mMesh.size()) continue;
        Mesh& m = mMesh[r.mesh];
        if (m.generation != r.gen || !m.registered || r.page >= m.pages.size()) continue;
        Page& pg = m.pages[r.page];
        if (pg.st != PG_STAGED) continue;
        if (mTableWrites.size() >= kMaxTableWrites || updRoom() < 4)
        {
            d.staged.push_back(r);   // no room for the flip this frame: next round (still complete)
            continue;
        }
        if (parentsAll(r.mesh, r.page, PG_RESIDENT))
        {
            pg.st = PG_RESIDENT;
            pg.last_used = (uint32_t)mFrame;
            mTableWrites.push_back({m.page_base + r.page, pg.slot});
            mUpdPlanned += 4;
            ++mResidentPages;
            if (pg.root)
            {
                ++m.roots_resident;
                ++mResidentRoots;
                if (mRootsPendingSlots) --mRootsPendingSlots;
            }
        }
        else
        {
            // A parent is gone (bad page / mesh trouble): the slot was never published.
            if (pg.root) { failMesh(r.mesh, "root page could not be published", false); continue; }
            mSlotOwner[pg.slot] = PageRef{kNone, 0, 0};
            mFreeSlots.push_back(pg.slot);
            --mUsedSlots;
            pageGone(r.mesh, r.page);
        }
    }
    // Evictions kFrameSlots frames ago: no frame in flight can select those pages any more.
    for (const PageRef& r : cur.cooling)
    {
        if (r.mesh >= mMesh.size()) continue;
        Mesh& m = mMesh[r.mesh];
        if (m.generation != r.gen || r.page >= m.pages.size()) continue;
        if (m.pages[r.page].st == PG_COOLING) pageGone(r.mesh, r.page);
    }
    for (uint32_t slot : cur.slots)
    {
        if (slot >= mPoolPages) continue;
        mSlotOwner[slot] = PageRef{kNone, 0, 0};
        mFreeSlots.push_back(slot);
        if (mUsedSlots) --mUsedSlots;
    }
    for (auto& r : cur.vranges) mVAlloc.release(r.first, r.second);
    for (auto& r : cur.granges) mGAlloc.release(r.first, r.second);
    for (uint32_t dir : cur.dirs) mFreeDirs.push_back(dir);
}

void LLCleanClusterStreamVK::pollSources(const LLClusterFrameIn& in)
{
    LLCleanClusterCache& cache = LLCleanClusterCache::instance();
    LLCleanClusterBuilder& builder = LLCleanClusterBuilder::instance();
    cache.poll();
    builder.pump(mBuildMs);

    // Finished builds: store to disk, keep the payload for a while (it serves the pages).
    mBuildResults.clear();
    builder.takeFinished(mBuildResults);
    for (LLCleanClusterBuilder::Result& r : mBuildResults)
    {
        if (r.mesh) cache.store(r.mesh);
        auto it = mByKey.find(r.key);
        if (it == mByKey.end()) continue;
        Mesh& m = mMesh[it->second];
        if (m.state != MS::Building)
        {
            // A Live mesh's rebuild: applied by advanceMeshes (maybeRebuild) -- this may run on
            // a frame that records nothing (buffers backing off), so no registry change here.
            if (m.rebuilding && !m.rebuilt_ready)
            {
                m.rebuilt = r.mesh;
                m.rebuilt_ready = true;
            }
            continue;
        }
        m.rebuilding = false;
        m.rebuilt_ready = false;
        m.rebuilt.reset();
        if (!r.mesh)
        {
            logOnce("build:" + r.why, "build failed for " + r.key.mesh_id.asString() + ": " + r.why);
            m.state = MS::Failed;
            m.state_time = mNow;
            m.retry_frame = mFrame + 3600;
            continue;
        }
        takeData(m, r.mesh);
        m.validated = false;
        m.from_builder = true;   // validate() already ran on the worker (runBuild)
        m.state = MS::Ready;
        m.state_time = mNow;
        m.retry_frame = 0;
    }
    mBuildResults.clear();

    // Page reads.
    mPageReads.clear();
    cache.takePages(mPageReads);
    for (LLCleanClusterCache::PageRead& pr : mPageReads)
    {
        auto it = mTickets.find(pr.ticket);
        if (it == mTickets.end()) continue;
        const PageRef ref = it->second.ref;
        mTickets.erase(it);
        if (ref.mesh >= mMesh.size()) continue;
        Mesh& m = mMesh[ref.mesh];
        if (m.generation != ref.gen || !m.registered || ref.page >= m.pages.size()) continue;
        Page& pg = m.pages[ref.page];
        if (pg.st != PG_LOADING) continue;
        uint32_t used = 0;
        if (!pr.ok || pr.bytes.size() < kPageBytes || !validPage(m, ref.page, pr.bytes.data(), used))
        {
            ++mLoadFails;
            if (pg.root) { failMesh(ref.mesh, "root page unreadable", true); continue; }
            ++mBadPages;
            addParents(ref.mesh, ref.page, -1);
            pg.st = PG_BAD;   // its parent draws instead (never a hole); rebuilt next session
            LLCleanClusterCache::instance().drop(m.key);
            continue;
        }
        uint32_t li;
        if (!mLoadedFree.empty()) { li = mLoadedFree.back(); mLoadedFree.pop_back(); }
        else
        {
            try { mLoaded.emplace_back(); }
            catch (const std::bad_alloc&) { pageGone(ref.mesh, ref.page); continue; }
            li = (uint32_t)mLoaded.size() - 1u;
        }
        Loaded& L = mLoaded[li];
        L.ref = ref;
        L.bytes = std::move(pr.bytes);
        L.used = used;
        L.prio = pg.want_prio;
        L.frame = mFrame;
        pg.st = PG_LOADED;
        pg.loaded = li;
        ++mLoadedCount;
    }
    mPageReads.clear();

    // Abandon reads that never came back (bounded ticket table).
    if ((mFrame & 63u) == 0u)
    {
        for (auto it = mTickets.begin(); it != mTickets.end();)
        {
            if (mFrame < it->second.frame + kTicketTimeout) { ++it; continue; }
            const PageRef ref = it->second.ref;
            it = mTickets.erase(it);
            if (ref.mesh < mMesh.size() && mMesh[ref.mesh].generation == ref.gen && ref.page < mMesh[ref.mesh].pages.size()
                && mMesh[ref.mesh].pages[ref.page].st == PG_LOADING)
                pageGone(ref.mesh, ref.page);
        }
    }
    (void)in;
}

void LLCleanClusterStreamVK::advanceMeshes(const LLClusterFrameIn& in)
{
    LLCleanClusterCache& cache = LLCleanClusterCache::instance();
    LLCleanClusterBuilder& builder = LLCleanClusterBuilder::instance();
    uint32_t meta_asks = 0, regs = 0, rebuild_asks = 0, wait_source = 0;
    uint64_t no_disk = 0;
    // The builder's priority of a mesh (LLCleanClusterBuilder PRIORITY): what acquireMesh saw.
    const auto prioOf = [](const Mesh& m, bool rebuild)
    {
        LLCleanClusterBuilder::Prio p;
        p.importance = m.prio;
        p.group = m.prio_group;
        p.rebuild = rebuild;
        return p;
    };
    for (uint32_t i = 0; i < (uint32_t)mMesh.size(); ++i)
    {
        Mesh& m = mMesh[i];
        switch (m.state)
        {
        case MS::Meta:
        {
            if (m.wait_source) ++wait_source;
            if (mFrame < m.retry_frame || meta_asks >= kMetaPerFrame) break;
            ++meta_asks;
            std::shared_ptr<const LLClusterMeshData> out;
            const LLCleanClusterCache::State st = cache.requestMeta(m.key, out);
            if (st == LLCleanClusterCache::State::Ready && out)
            {
                takeData(m, out);
                m.wait_source = false;
                m.validated = false;
                m.from_builder = false;
                m.state = MS::Ready;
                m.state_time = mNow;
            }
            else if (st == LLCleanClusterCache::State::Loading) m.retry_frame = mFrame + 1;
            else
            {
                bool no_src = false;
                if (builder.request(m.key, m.src_slot, (U32)mFrame, prioOf(m, false), &no_src))
                {
                    m.state = MS::Building;
                    m.state_time = mNow;
                    m.wait_source = false;
                    m.req_prio = m.prio;
                    m.req_slot = m.src_slot;
                }
                else
                {
                    // refused (queue full / no source yet): ask again
                    m.retry_frame = mFrame + (no_src ? 15 : 60);
                    m.wait_source = no_src;
                }
            }
            break;
        }
        case MS::Building:
            if (mNow - m.state_time > kBuildTimeoutSec)
            {
                m.state = MS::Failed;
                m.state_time = mNow;
                m.retry_frame = mFrame + 3600;
            }
            else if (m.src_slot != m.req_slot || m.prio != m.req_prio)
            {
                // Queued: the builder re-sorts by the new priority (the avatar came closer / went
                // away) and takes a higher LOD the face publishes now (one build, no upgrade
                // rebuild after it). A repeat request is O(1).
                builder.request(m.key, m.src_slot, (U32)mFrame, prioOf(m, false));
                m.req_prio = m.prio;
                m.req_slot = m.src_slot;
            }
            break;
        case MS::Ready:
        {
            if (mFrame < m.retry_frame || !m.data) break;
            if (!m.validated)
            {
                std::string why;
                // A fresh build was validated on its worker (every page hashed: too much for the
                // render thread at hundreds of builds per second); a cache load (metadata only,
                // cheap) is validated here.
                if (!metaSane(*m.data, why) || (!m.from_builder && !LLCleanClusterBuilder::validate(*m.data, &why)))
                {
                    failMesh(i, why.empty() ? "invalid cluster mesh" : why.c_str(), true);
                    break;
                }
                m.validated = true;
            }
            // (the per-frame count limit retries next frame: a crowd's cache hits register in a
            // few frames instead of 15-frame steps; no room retries after 15)
            if (regs >= kRegsPerFrame) m.retry_frame = mFrame + 1;
            else if (!tryRegister(i)) m.retry_frame = mFrame + 15;
            else ++regs;
            break;
        }
        case MS::Rooting:
            if (m.data && m.roots_resident >= m.data->header.root_pages)
            {
                // (#798: a shadow takes its key over now, or waits a frame for room)
                if (m.shadow_of != kNone && !shadowSwap(i)) break;
                if (m.state == MS::Free) break;
                m.state = MS::Live;
                m.state_time = mNow;
                ++mLiveMeshes;
                // TIME TO DRAWABLE (2026-10-14): first acquire -> Live (roots resident), once per
                // directory entry (a re-registration after a rebuild is not a first draw).
                if (!m.ttfd_done && m.first_time > 0.0)
                {
                    m.ttfd_done = true;
                    if (mTtfd.size() < kTtfdSamples)
                    {
                        try { mTtfd.push_back({(float)((mNow - m.first_time) * 1000.0), m.from_builder}); }
                        catch (const std::bad_alloc&) {}
                    }
                    else ++mTtfdDropped;
                }
            }
            break;
        case MS::Live:
            maybeRebuild(i, rebuild_asks);
            // CLUSTER-OWNED MIRROR RELEASE (header, takeMirrorCandidates): a durable DAG of the
            // highest creator LOD no longer needs the store's CPU mirror as its source.
            if (in.owns_rigged && m.state == MS::Live && mFrame >= m.mirror_check && m.data && !m.rebuilding && m.shadow == kNone
                && m.shadow_of == kNone)
            {
                m.mirror_check = mFrame + kMirrorRecheckFrames;
                const LLCleanClusterCache::PageSource src = m.data->header.source_lod == 3
                    ? cache.pageSource(m.key, m.data->header.header_hash) : LLCleanClusterCache::PageSource::None;
                if (src == LLCleanClusterCache::PageSource::Ready || src == LLCleanClusterCache::PageSource::Full)
                {
                    if (mMirrorCand.size() < kMirrorCandMax)
                    {
                        try { mMirrorCand.push_back(m.key); }
                        catch (const std::bad_alloc&) { m.mirror_check = mFrame + 60; }
                    }
                    else m.mirror_check = mFrame + 60;   // (offered again soon)
                }
                else if (src == LLCleanClusterCache::PageSource::Pending)
                    m.mirror_check = mFrame + 60;       // (the write is in flight / the scan runs)
            }
            break;
        case MS::Failed:
            if (mFrame >= m.retry_frame)
            {
                m.state = MS::Meta;
                m.state_time = mNow;
                m.retry_frame = 0;
            }
            break;
        case MS::Free:
            break;
        }
        // Retained build payloads: released after the hold time / over the budget, once the
        // roots are up AND the disk cache can serve the later pages (its write landed). A
        // payload the cache cannot serve (a build from a lower creator LOD is never persisted;
        // a write refused / failed) is the mesh's only page source: kept while the mesh lives
        // (freed on liveness; "no disk" in the stats), released only under RAM pressure -- its
        // pages then stay ABSENT (the parents draw) and the mesh is rebuilt once calm.
        bool release = false;
        if (m.payload && m.data && m.state == MS::Live
            && (mNow > m.payload_until || mPayloadBytes > mPayloadBudget))
        {
            const LLCleanClusterCache::PageSource src = cache.pageSource(m.key, m.data->header.header_hash);
            const bool on_disk = src == LLCleanClusterCache::PageSource::Ready || src == LLCleanClusterCache::PageSource::Full;
            release = on_disk || (src == LLCleanClusterCache::PageSource::None && mRamPressure && mPayloadBytes > mPayloadBudget);
            if (!release && m.data->hasPayload())
            {
                no_disk += m.data->payload.size();
                // A full-LOD build whose write was refused (writes in flight / bytes): offered
                // again once per hold period, so it does not stay in RAM for good.
                if (src == LLCleanClusterCache::PageSource::None && m.data->header.source_lod == 3 && mNow > m.payload_until)
                {
                    cache.store(m.data);
                    m.payload_until = mNow + kPayloadHoldSec;
                }
            }
        }
        if (release)
        {
            try
            {
                auto meta = std::make_shared<LLClusterMeshData>();
                meta->key = m.data->key;
                meta->header = m.data->header;
                meta->groups = m.data->groups;
                meta->pages = m.data->pages;
                meta->deps = m.data->deps;
                mPayloadBytes -= std::min<uint64_t>(mPayloadBytes, m.data->payload.size());
                m.data = meta;
                m.payload = false;
            }
            catch (const std::bad_alloc&) {}   // keep it; retried next frame
        }
    }
    mPayloadNoDisk = no_disk;
    mWaitSource = wait_source;
    // #798: the upgrade swaps queued by this loop's rebuilds (allocDir may grow mMesh: not inside it)
    if (!mShadowQ.empty()) startShadows();
    // #798: the live meshes by the creator LOD their DAG was built from (the CLOD stream line)
    std::memset(mSrcLod, 0, sizeof(mSrcLod));
    mShadows = 0;
    for (const Mesh& m : mMesh)
    {
        if (m.shadow_of != kNone) ++mShadows;
        if (m.state != MS::Live || !m.data) continue;
        const int32_t l = m.data->header.source_lod;
        ++mSrcLod[(l >= 0 && l <= 3) ? l : 4];
    }
    (void)in;
}

// The mesh's data (+ its payload) from a build or the cache's metadata.
void LLCleanClusterStreamVK::takeData(Mesh& m, const std::shared_ptr<const LLClusterMeshData>& data)
{
    if (m.payload && m.data) mPayloadBytes -= std::min<uint64_t>(mPayloadBytes, m.data->payload.size());
    m.data = data;
    m.payload = data && data->hasPayload();
    if (m.payload)
    {
        mPayloadBytes += data->payload.size();
        m.payload_until = mNow + kPayloadHoldSec;
    }
    m.rebuild_lod = data ? (int8_t)std::clamp<int32_t>(data->header.source_lod, -1, 3) : (int8_t)-1;
    m.no_source = false;
}

// A Live mesh is rebuilt (LLCleanClusterBuilder; the mesh stays Live meanwhile) when
//   - the face now publishes a HIGHER creator LOD than the DAG was built from (a face first
//     seen far away is built from the LOD that was resident then: without this it would stay
//     at that detail however close the camera comes), once per LOD step;
//   - a wanted page has no source (payload released, not on disk: evicted / dropped / never
//     persisted) and RAM is calm: the rebuild brings the payload back.
void LLCleanClusterStreamVK::maybeRebuild(uint32_t idx, uint32_t& asks)
{
    Mesh& m = mMesh[idx];
    if (!m.data) return;
    if (m.rebuilding)
    {
        if (m.rebuilt_ready)
        {
            std::shared_ptr<const LLClusterMeshData> d;
            d.swap(m.rebuilt);
            m.rebuilt_ready = false;
            rebuildDone(idx, d);
        }
        else if (mNow - m.rebuild_time > kBuildTimeoutSec) { m.rebuilding = false; m.retry_frame = mFrame + 3600; }
        return;
    }
    if (m.shadow != kNone) return;   // (#798: an upgrade swap is in flight)
    if (asks >= kRebuildPerFrame || mFrame < m.retry_frame) return;
    bool upgrade = false;
    int32_t lod = -1;
    if (m.data->header.source_lod < 3 && m.src_slot != m.lod_slot)
    {
        m.lod_slot = m.src_slot;   // (the LOD lookup runs when the face's source slot changes)
        lod = slotCreatorLod(m.key, m.src_slot);
        upgrade = lod > m.data->header.source_lod && lod > m.rebuild_lod;
    }
    const bool resource = m.no_source && !m.payload && !mRamPressure;
    if (!upgrade && !resource) return;
    ++asks;
    m.no_source = false;
    LLCleanClusterBuilder::Prio prio;   // behind every first build (it already draws)
    prio.importance = m.prio;
    prio.group = m.prio_group;
    prio.rebuild = true;
    if (LLCleanClusterBuilder::instance().request(m.key, m.src_slot, (U32)mFrame, prio))
    {
        m.rebuilding = true;
        m.rebuilt_ready = false;
        m.rebuilt.reset();
        m.rebuild_time = mNow;
        if (upgrade) m.rebuild_lod = (int8_t)lod;
        ++mRebuilds;
    }
    else
    {
        m.retry_frame = mFrame + 60;   // refused (queue full / slot not ready): ask again
        if (upgrade) m.lod_slot = kNone;
    }
}

// A Live mesh's rebuild finished. The same DAG (header hash): only the payload comes back.
// A different one (higher creator LOD): a SHADOW entry registers it beside the live one and takes
// the key over once its roots are resident (#798: startShadows / shadowSwap -- no frame without the
// mesh); only without a free directory entry the old in-place swap (unregister + Ready: not drawn
// for the few frames its roots take).
void LLCleanClusterStreamVK::rebuildDone(uint32_t idx, const std::shared_ptr<const LLClusterMeshData>& data)
{
    Mesh& m = mMesh[idx];
    m.rebuilding = false;
    if (!data || !m.data)
    {
        m.retry_frame = mFrame + 3600;   // keeps what it has
        return;
    }
    if (data->header.header_hash == m.data->header.header_hash)
    {
        if (!m.payload)
        {
            const int8_t lod = m.rebuild_lod;
            takeData(m, data);
            m.rebuild_lod = std::max(lod, m.rebuild_lod);
        }
        return;
    }
    if (data->header.source_lod < m.data->header.source_lod) return;   // (the slot went down meanwhile)
    // #798 CLEAN UPGRADE SWAP: the new DAG registers beside this one (a shadow entry, startShadows
    // after the loop) and takes over once its roots are resident (shadowSwap) -- the mesh never
    // leaves the draw. Only when no directory entry is free: the in-place swap below.
    if (m.shadow == kNone && m.state == MS::Live)
    {
        m.rebuilt = data;
        m.shadow = kShadowQueued;
        try { mShadowQ.push_back(idx); }
        catch (const std::bad_alloc&) { m.rebuilt.reset(); m.shadow = kNone; }
        if (m.shadow == kShadowQueued) return;
    }
    if (m.shadow == kShadowInPlace) m.shadow = kNone;
    // The dead directory record must fit this frame's update area (a dropped one would leave
    // a live record over a page range that is reused): else applied on a later frame.
    if (mMeshWrites.size() + 1 > kMaxMeshWrites || updRoom() < sizeof(ClusterMesh))
    {
        m.rebuilding = true;
        m.rebuilt = data;
        m.rebuilt_ready = true;
        return;
    }
    const int8_t lod = m.rebuild_lod;
    unregister(idx, true);
    takeData(m, data);
    m.rebuild_lod = std::max(lod, m.rebuild_lod);
    m.validated = false;
    m.from_builder = true;   // validate() already ran on the worker (runBuild)
    m.state = MS::Ready;
    m.state_time = mNow;
    m.retry_frame = 0;
    ++mRebuildSwaps;
}

// ---- pages ----------------------------------------------------------------------------------
bool LLCleanClusterStreamVK::validPage(const Mesh& m, uint32_t page, const uint8_t* bytes, uint32_t& used) const
{
    if (!m.data || page >= m.data->pages.size()) return false;
    ClusterPageHeader h;
    std::memcpy(&h, bytes, sizeof(h));
    if (h.magic != kPageMagic || (h.version_count >> 16) != kFormatVersion) return false;
    if (h.page_index != page || h.mesh_key != (uint32_t)m.key.hash()) return false;
    if (h.used_bytes < kPageHeaderBytes || h.used_bytes > kPageBytes || h.used_bytes != m.data->pages[page].used_bytes)
        return false;
    if ((h.version_count & 0xFFu) == 0 || (h.version_count & 0xFFu) > kMaxClustersPerPage) return false;
    used = h.used_bytes;
    return true;
}

void LLCleanClusterStreamVK::addParents(uint32_t mesh, uint32_t page, int delta)
{
    Mesh& m = mMesh[mesh];
    if (!m.data || page >= m.data->pages.size()) return;
    const ClusterPageDir& pd = m.data->pages[page];
    for (uint32_t k = 0; k < pd.dep_count; ++k)
    {
        const uint32_t par = m.data->deps[pd.dep_first + k];
        if (par >= m.pages.size()) continue;
        uint16_t& c = m.pages[par].dependents;
        if (delta > 0) c = (uint16_t)std::min<uint32_t>(0xFFFFu, c + 1u);
        else if (c) --c;
    }
}

bool LLCleanClusterStreamVK::parentsAll(uint32_t mesh, uint32_t page, uint8_t min_state) const
{
    const Mesh& m = mMesh[mesh];
    if (!m.data || page >= m.data->pages.size()) return false;
    const ClusterPageDir& pd = m.data->pages[page];
    for (uint32_t k = 0; k < pd.dep_count; ++k)
    {
        const uint32_t par = m.data->deps[pd.dep_first + k];
        if (par >= m.pages.size()) return false;
        const uint8_t st = m.pages[par].st;
        if (min_state == PG_RESIDENT) { if (st != PG_RESIDENT) return false; }
        else if (min_state == PG_STAGED) { if (st != PG_STAGED && st != PG_RESIDENT) return false; }
        else if (st == PG_ABSENT || st == PG_COOLING || st == PG_BAD) return false;   // "present"
    }
    return true;
}

void LLCleanClusterStreamVK::pageGone(uint32_t mesh, uint32_t page)
{
    Page& pg = mMesh[mesh].pages[page];
    if (pg.st == PG_LOADED && pg.loaded != kNone && pg.loaded < mLoaded.size())
    {
        mLoaded[pg.loaded].bytes = std::vector<uint8_t>();
        mLoaded[pg.loaded].ref.mesh = kNone;
        mLoadedFree.push_back(pg.loaded);
        if (mLoadedCount) --mLoadedCount;
    }
    const bool held = pg.st != PG_ABSENT && pg.st != PG_BAD;
    pg.st = PG_ABSENT;
    pg.slot = kNone;
    pg.loaded = kNone;
    if (held) addParents(mesh, page, -1);
}

bool LLCleanClusterStreamVK::startLoad(uint32_t mesh, uint32_t page)
{
    Mesh& m = mMesh[mesh];
    Page& pg = m.pages[page];
    if (m.payload && m.data && m.data->hasPayload())
    {
        uint32_t li;
        if (!mLoadedFree.empty()) { li = mLoadedFree.back(); mLoadedFree.pop_back(); }
        else
        {
            try { mLoaded.emplace_back(); }
            catch (const std::bad_alloc&) { return false; }
            li = (uint32_t)mLoaded.size() - 1u;
        }
        Loaded& L = mLoaded[li];
        try { L.bytes.resize(kPageBytes); }
        catch (const std::bad_alloc&) { mLoadedFree.push_back(li); return false; }   // retry later
        uint32_t used = 0;
        if (!m.data->copyPage(page, L.bytes.data()) || !validPage(m, page, L.bytes.data(), used))
        {
            L.bytes = std::vector<uint8_t>();
            mLoadedFree.push_back(li);
            ++mLoadFails;
            if (pg.root) failMesh(mesh, "root page invalid", true);
            else { ++mBadPages; pg.st = PG_BAD; }
            return false;
        }
        L.ref = PageRef{mesh, m.generation, page};
        L.used = used;
        L.prio = pg.want_prio;
        L.frame = mFrame;
        pg.st = PG_LOADED;
        pg.loaded = li;
        ++mLoadedCount;
        addParents(mesh, page, +1);
        return true;
    }
    const uint64_t ticket = mNextTicket++;
    if (!LLCleanClusterCache::instance().requestPages(m.key, &page, 1, ticket)) return false;   // refused: retry
    try { mTickets.emplace(ticket, Ticket{PageRef{mesh, m.generation, page}, mFrame}); }
    catch (const std::bad_alloc&) { return false; }   // (its result is ignored when it comes)
    pg.st = PG_LOADING;
    addParents(mesh, page, +1);
    return true;
}

void LLCleanClusterStreamVK::wantPage(uint32_t mesh, uint32_t page, float prio, uint32_t depth)
{
    Mesh& m = mMesh[mesh];
    if (page >= m.pages.size() || depth > kMaxLevels) return;
    Page& pg = m.pages[page];
    if (pg.want_frame == (uint32_t)mFrame && pg.want_prio >= prio) return;
    const bool first = pg.want_frame != (uint32_t)mFrame;
    pg.want_frame = (uint32_t)mFrame;
    pg.want_prio = first ? prio : std::max(pg.want_prio, prio);
    if (pg.st == PG_LOADED && pg.loaded < mLoaded.size()) mLoaded[pg.loaded].prio = pg.want_prio;
    if (pg.st != PG_ABSENT) return;
    // Closure: missing parents first (a little more urgent than the child).
    const ClusterPageDir& pd = m.data->pages[page];
    for (uint32_t k = 0; k < pd.dep_count; ++k)
    {
        const uint32_t par = m.data->deps[pd.dep_first + k];
        if (par < m.pages.size() && m.pages[par].st == PG_ABSENT) wantPage(mesh, par, prio * 1.001f + 1e-3f, depth + 1);
    }
    if (first) mWant.push_back({prio, PageRef{mesh, m.generation, page}});
}

bool LLCleanClusterStreamVK::evictPage(uint32_t mesh, uint32_t page)
{
    Mesh& m = mMesh[mesh];
    Page& pg = m.pages[page];
    if (pg.st != PG_RESIDENT || pg.root || pg.dependents || pg.slot == kNone) return false;
    if (mTableWrites.size() >= kMaxTableWrites || updRoom() < 4) return false;
    mTableWrites.push_back({m.page_base + page, kNone});
    mUpdPlanned += 4;
    pg.st = PG_COOLING;
    Deferred& d = mDef[mSlot];
    d.slots.push_back(pg.slot);
    d.cooling.push_back(PageRef{mesh, m.generation, page});
    --mResidentPages;
    ++mEvictions;
    return true;
}

// Least-recently-used non-root pages nothing depends on. Their slots come free kFrameSlots
// frames later. force: pressure eviction (pages used up to `min_age` frames ago qualify).
uint32_t LLCleanClusterStreamVK::evictLRU(uint32_t want, uint32_t min_age, bool force)
{
    if (!want) return 0;
    std::vector<std::pair<uint32_t, uint32_t>>& cand = mEvictScratch;   // (last_used, pool slot)
    cand.clear();
    const uint32_t now = (uint32_t)mFrame;
    for (uint32_t s = 0; s < mPoolPages; ++s)
    {
        const PageRef& o = mSlotOwner[s];
        if (o.mesh >= mMesh.size()) continue;
        const Mesh& m = mMesh[o.mesh];
        if (m.generation != o.gen || o.page >= m.pages.size()) continue;
        const Page& pg = m.pages[o.page];
        if (pg.st != PG_RESIDENT || pg.root || pg.dependents || pg.slot != s) continue;
        if (pg.want_frame == now && !force) continue;
        if (pg.last_used + min_age > now) continue;
        cand.push_back({pg.last_used, s});
    }
    if (cand.empty()) return 0;
    const size_t k = std::min<size_t>(want, cand.size());
    std::partial_sort(cand.begin(), cand.begin() + (ptrdiff_t)k, cand.end());
    uint32_t n = 0;
    for (size_t i = 0; i < k; ++i)
    {
        const PageRef o = mSlotOwner[cand[i].second];
        if (evictPage(o.mesh, o.page)) ++n;
        else if (mTableWrites.size() >= kMaxTableWrites) break;
    }
    return n;
}

uint64_t LLCleanClusterStreamVK::trim()
{
    // Everything but the pinned roots (children before parents: a parent whose children
    // leave this frame goes on a later call, when their slots are freed).
    const uint32_t before = mResidentPages;
    if (mPoolPages) evictLRU(mPoolPages, 0, true);
    return (uint64_t)(before - std::min(before, mResidentPages)) * kPageBytes;
}

void LLCleanClusterStreamVK::planUploads()
{
    mWantNoSource = mWantBusy = mWantCap = 0;
    if (!mPool.buffer || !mStaging.mapped) return;
    // 1. What is wanted: the roots of registering meshes, then the GPU's requests.
    mWant.clear();
    for (uint32_t i = 0; i < (uint32_t)mMesh.size(); ++i)
    {
        Mesh& m = mMesh[i];
        if (m.state != MS::Rooting || !m.data) continue;
        for (uint32_t p = 0; p < m.data->header.root_pages && p < m.pages.size(); ++p)
            if (m.pages[p].st == PG_ABSENT) wantPage(i, p, kRootPrio, 0);
            else m.pages[p].want_frame = (uint32_t)mFrame;
        // the warm pages (kWarmPages): after the roots, before any GPU request
        for (uint32_t p = m.data->header.root_pages; p < m.data->header.root_pages + kWarmPages && p < m.pages.size(); ++p)
            if (m.pages[p].st == PG_ABSENT) wantPage(i, p, kRootPrio * 0.75f, 0);
            else m.pages[p].want_frame = (uint32_t)mFrame;
    }
    uint32_t uniq = 0;
    for (const ClusterRequest& r : mReqScratch)
    {
        uint32_t local = 0;
        const uint32_t mi = meshOf(r.vpage, local);
        if (mi == kNone) continue;
        Mesh& m = mMesh[mi];
        if (m.state != MS::Live && m.state != MS::Rooting) continue;
        Page& pg = m.pages[local];
        if (pg.want_frame != (uint32_t)mFrame) ++uniq;
        const float prio = std::isfinite(r.priority) ? std::max(r.priority, 0.f) : 0.f;
        wantPage(mi, local, std::min(prio, kRootPrio * 0.5f), 0);
    }
    mReqUniqRate.tick(mNow, uniq);

    // 2. Start loads, most wanted first (ties: coarse pages first).
    std::sort(mWant.begin(), mWant.end(), [](const std::pair<float, PageRef>& a, const std::pair<float, PageRef>& b)
              {
                  if (a.first != b.first) return a.first > b.first;
                  if (a.second.mesh != b.second.mesh) return a.second.mesh < b.second.mesh;
                  return a.second.page < b.second.page;
              });
    //    A page of a mesh without a payload comes from the disk cache. The cache saying it can
    //    NEVER serve it (not persisted / evicted / another build) must not block the others:
    //    only a full read queue ends the cache loads of this frame. Such a page stays ABSENT
    //    (its parent draws, never a hole) and the mesh is re-sourced (maybeRebuild).
    LLCleanClusterCache& cache = LLCleanClusterCache::instance();
    uint32_t inflight = mLoadedCount + (uint32_t)mTickets.size();
    bool cache_full = false;
    for (const auto& w : mWant)
    {
        const PageRef& r = w.second;
        Mesh& m = mMesh[r.mesh];
        if (m.generation != r.gen || r.page >= m.pages.size()) continue;
        Page& pg = m.pages[r.page];
        if (pg.st != PG_ABSENT || !parentsAll(r.mesh, r.page, PG_LOADING)) continue;
        if (inflight >= mLoadCap) { ++mWantCap; continue; }
        const bool from_cache = !(m.payload && m.data && m.data->hasPayload());
        if (from_cache)
        {
            if (cache_full || !m.data) { ++mWantBusy; continue; }
            const LLCleanClusterCache::PageSource src = cache.pageSource(m.key, m.data->header.header_hash);
            if (src == LLCleanClusterCache::PageSource::None)
            {
                ++mWantNoSource;
                ++mNoSourceTotal;
                if (pg.root && mMeshWrites.size() + 1 <= kMaxMeshWrites && updRoom() >= sizeof(ClusterMesh))
                {
                    // Roots of a registering mesh with no source (its file went away between
                    // the metadata and the roots): back to Meta -- it is rebuilt (legacy path
                    // meanwhile). unregister bumps the generation: its other wants skip.
                    logOnce("rootsrc", "root pages of " + m.key.mesh_id.asString() + " have no source; rebuilding");
                    unregister(r.mesh, true);
                    takeData(m, nullptr);
                    m.rebuilding = m.rebuilt_ready = false;
                    m.rebuilt.reset();
                    m.validated = false;
                    m.state = MS::Meta;
                    m.state_time = mNow;
                    m.retry_frame = 0;
                }
                else if (!pg.root) m.no_source = true;   // (a root without room: next frame)
                continue;
            }
            if (src != LLCleanClusterCache::PageSource::Ready)
            {
                ++mWantBusy;
                cache_full = cache_full || src == LLCleanClusterCache::PageSource::Full;
                continue;
            }
        }
        if (startLoad(r.mesh, r.page)) ++inflight;
        else if (from_cache) { cache_full = true; ++mWantBusy; }
    }

    // 3. Room for what is loaded: free slots under the effective cap; grow the pool in a step
    //    (no VRAM pressure); else LRU (those slots free up kFrameSlots frames from now).
    const auto freeNow = [&]() -> uint32_t
    {
        const uint32_t under = mEffCap > mUsedSlots ? mEffCap - mUsedSlots : 0;
        return std::min<uint32_t>(under, (uint32_t)mFreeSlots.size());
    };
    mGrowWaitAsync = false;
    if (mLoadedCount > freeNow())
    {
        // 2026-10-14 VRAM: COLD pages (unused kColdAge frames) make the room first -- the pool
        // grows only for a working set that does not fit (their slots come back kFrameSlots
        // frames from now; the loaded pages wait that long).
        const uint32_t short_by = mLoadedCount - freeNow();
        const uint32_t cold = short_by > pendingFree()
                            ? evictLRU(short_by - pendingFree(), mVramPressure ? kColdAge / 8u : kColdAge, false) : 0u;
        if (cold + pendingFree() >= short_by) {}
        else if (mPoolPages < mPoolTarget && !mVramPressure && !mPressureCap && mFrame >= mLastGrowFrame + kSlots
            && (mVramAvail == ~0ull || mVramAvail > (uint64_t)mPoolPages * kPageBytes + kGrowMarginBytes))
        {
            // #749: the prefix copy reads the old pool on graphics; copies a forked async
            // segment recorded in the last kFrameSlots frames must have aged out first
            // (meanwhile this frame's copies stay on graphics).
            if (mAsyncCopyFrame && mFrame < mAsyncCopyFrame + kSlots)
                mGrowWaitAsync = true;
            else
            {
                growPool(growStep(short_by));
                mEffCap = effectiveCap();
            }
        }
        const uint32_t coming = freeNow() + pendingFree();
        if (mLoadedCount > coming) evictLRU(mLoadedCount - coming, kEvictMinAge, false);
    }

    // 4. Stage the loaded pages (priority order) within the byte budget.
    if (!mLoadedCount) return;
    mStageScratch.clear();
    for (uint32_t li = 0; li < (uint32_t)mLoaded.size(); ++li)
        if (mLoaded[li].ref.mesh != kNone && !mLoaded[li].bytes.empty()) mStageScratch.push_back(li);
    std::sort(mStageScratch.begin(), mStageScratch.end(), [this](uint32_t a, uint32_t b)
              {
                  const Loaded& A = mLoaded[a];
                  const Loaded& B = mLoaded[b];
                  if (A.prio != B.prio) return A.prio > B.prio;
                  if (A.ref.mesh != B.ref.mesh) return A.ref.mesh < B.ref.mesh;
                  return A.ref.page < B.ref.page;
              });
    uint8_t* const base = (uint8_t*)mStaging.mapped + mStagingSlotBytes * mSlot;
    const uint64_t budget = std::min<uint64_t>(mUploadBytesBudget, mStagingPageBytes);   // (staging may have been resized)
    for (uint32_t li : mStageScratch)
    {
        Loaded& L = mLoaded[li];
        const PageRef r = L.ref;
        if (r.mesh >= mMesh.size()) continue;
        Mesh& m = mMesh[r.mesh];
        if (m.generation != r.gen || r.page >= m.pages.size() || m.pages[r.page].st != PG_LOADED) continue;
        Page& pg = m.pages[r.page];
        // Parents left meanwhile (or a stale load nobody wants): give the bytes back.
        if (!parentsAll(r.mesh, r.page, PG_LOADING) || (mFrame > L.frame + kLoadedTimeout && !pg.root))
        {
            pageGone(r.mesh, r.page);
            continue;
        }
        // Closure: a page is copied only after its parents' copies (same frame earlier in this
        // loop -- they sort first -- or an earlier frame), so it flips with or after them.
        if (!parentsAll(r.mesh, r.page, PG_STAGED)) continue;
        const uint32_t bytes = alignUp32(L.used, 16u);
        if (mCopies.size() >= kMaxCopiesPerFrame) break;
        if (mCopyBytes + bytes > budget)
        {
            // (the RING cut this frame short, not the upload budget: grow it to the setting)
            if (budget < mUploadBytesBudget) mStagingStarved = true;
            break;
        }
        if (!freeNow() || mTableWrites.size() >= kMaxTableWrites) break;
        const uint32_t slot = mFreeSlots.back();
        mFreeSlots.pop_back();
        ++mUsedSlots;
        mPoolPeak = std::max(mPoolPeak, mUsedSlots);
        std::memcpy(base + mCopyBytes, L.bytes.data(), bytes);
        mCopies.push_back({(uint32_t)mCopyBytes, slot, bytes});
        mCopyBytes += alignUp32(bytes, 256u);
        mSlotOwner[slot] = r;
        mSlotUsed[slot] = (uint32_t)mFrame;
        L.bytes = std::vector<uint8_t>();
        L.ref.mesh = kNone;
        mLoadedFree.push_back(li);
        if (mLoadedCount) --mLoadedCount;
        pg.loaded = kNone;
        pg.st = PG_STAGED;
        pg.slot = slot;
        pg.used = L.used;
        pg.last_used = (uint32_t)mFrame;
        mDef[mSlot].staged.push_back(r);
    }
}

uint32_t LLCleanClusterStreamVK::updRoom() const
{
    // 64 KiB held back for the 16-byte alignment padding the recording adds per record (the
    // plan counts payload bytes only), so a planned write is never dropped there.
    constexpr uint32_t kUsable = kUpdateBytes - (64u << 10);
    return mUpdPlanned >= kUsable ? 0u : kUsable - mUpdPlanned;
}

// ============================================================================================
// recording
// ============================================================================================
void LLCleanClusterStreamVK::recordUploads(VkCommandBuffer cmd)
{
    const VkCommandBuffer up_cmd = mUploadCmd;
    mUploadCmd = VK_NULL_HANDLE;
    if (!mFrameOpen || !cmd) return;
    mFrameOpen = false;
    const VkClusterFeatures& feat = mOwner.caps().feat;
    const VkPipelineStageFlags2 shaders = shaderStages(feat);
    Deferred& d = mDef[mSlot];

    // Entry: the previous frames' shader / transfer accesses of everything written below
    // (requests and usage are written by selection; the table / directory / groups / pool are
    // read by it) -> this frame's transfers. HOST: the usage readback the CPU reads later.
    memBarrier(cmd, shaders | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
               VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_HOST_BIT,
               VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_HOST_READ_BIT);

    const auto transferBarrier = [&]()
    {
        memBarrier(cmd, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
    };
    // 1. Fresh buffers: zero (the directory: generation 0 never matches; usage stamps 0).
    for (const BufFill& f : mBufFills) vkCmdFillBuffer(cmd, f.buffer, f.offset, f.size, f.value);
    if (!mBufFills.empty()) transferBarrier();
    mBufFills.clear();

    // 2. Growth copies (old -> new prefix, in order; a chain within one frame is ordered by
    //    the barrier after each), then the old buffers retire.
    for (GrowCopy& g : mGrowCopies)
    {
        if (g.bytes && g.from.buffer && g.to.buffer)
        {
            VkBufferCopy c{0, 0, std::min({g.bytes, g.from.size, g.to.size})};
            vkCmdCopyBuffer(cmd, g.from.buffer, g.to.buffer, 1, &c);
            transferBarrier();
        }
        mOwner.retire(g.from);
    }
    mGrowCopies.clear();

    // 3. Refills AFTER the growth copies (a reset must not be overwritten by a stale prefix):
    //    the whole table / directory after a reset, the table range of each new registration.
    bool wrote = false;
    if (mTableFill && mTable.buffer)
    {
        vkCmdFillBuffer(cmd, mTable.buffer, 0, VK_WHOLE_SIZE, 0xFFFFFFFFu);
        mTableFill = false;
        wrote = true;
    }
    if (mDirFill && mMeshes.buffer)
    {
        vkCmdFillBuffer(cmd, mMeshes.buffer, 0, VK_WHOLE_SIZE, 0u);
        mDirFill = false;
        wrote = true;
    }
    if (mTable.buffer)
        for (const auto& rf : mRangeFills)
        {
            vkCmdFillBuffer(cmd, mTable.buffer, (VkDeviceSize)rf.first * 4u, (VkDeviceSize)rf.second * 4u, 0xFFFFFFFFu);
            wrote = true;
        }
    mRangeFills.clear();
    if (wrote) transferBarrier();

    // 4. Table / directory / group updates from this slot's update area.
    uint8_t* const sbase = (uint8_t*)mStaging.mapped + mStagingSlotBytes * mSlot;
    const VkDeviceSize ubase = mStagingSlotBytes * mSlot + mStagingPageBytes;
    uint32_t uoff = 0;
    mRegions.clear();
    if (!mTableWrites.empty() && mTable.buffer)
    {
        // Last write wins per entry (stable sort keeps the queue order among equal pages),
        // runs of consecutive entries become one region.
        std::stable_sort(mTableWrites.begin(), mTableWrites.end(),
                         [](const TableWrite& a, const TableWrite& b) { return a.vpage < b.vpage; });
        size_t n = 0;
        for (size_t i = 0; i < mTableWrites.size(); ++i)
        {
            if (n && mTableWrites[n - 1].vpage == mTableWrites[i].vpage) mTableWrites[n - 1] = mTableWrites[i];
            else mTableWrites[n++] = mTableWrites[i];
        }
        mTableWrites.resize(n);
        for (size_t i = 0; i < n && uoff + 4u <= kUpdateBytes; ++i)
        {
            std::memcpy(sbase + mStagingPageBytes + uoff, &mTableWrites[i].value, 4);
            if (i && mTableWrites[i].vpage == mTableWrites[i - 1].vpage + 1u && !mRegions.empty())
                mRegions.back().size += 4u;
            else
                mRegions.push_back({ubase + uoff, (VkDeviceSize)mTableWrites[i].vpage * 4u, 4u});
            uoff += 4u;
        }
        if (!mRegions.empty()) vkCmdCopyBuffer(cmd, mStaging.buffer, mTable.buffer, (uint32_t)mRegions.size(), mRegions.data());
    }
    mTableWrites.clear();
    mRegions.clear();
    for (const MeshWrite& w : mMeshWrites)
    {
        if (w.dir >= mDirCap || uoff + sizeof(ClusterMesh) > kUpdateBytes) continue;
        uoff = alignUp32(uoff, 16u);
        std::memcpy(sbase + mStagingPageBytes + uoff, &w.rec, sizeof(ClusterMesh));
        mRegions.push_back({ubase + uoff, (VkDeviceSize)w.dir * sizeof(ClusterMesh), sizeof(ClusterMesh)});
        uoff += sizeof(ClusterMesh);
    }
    if (!mRegions.empty() && mMeshes.buffer)
        vkCmdCopyBuffer(cmd, mStaging.buffer, mMeshes.buffer, (uint32_t)mRegions.size(), mRegions.data());
    mMeshWrites.clear();
    mRegions.clear();
    for (const GroupWrite& g : mGroupWrites)
    {
        if (!g.data) continue;
        const uint32_t bytes = (uint32_t)(g.data->groups.size() * sizeof(ClusterGroup));
        uoff = alignUp32(uoff, 16u);
        if (!bytes || uoff + bytes > kUpdateBytes || g.base + g.data->groups.size() > mGroupCap) continue;
        std::memcpy(sbase + mStagingPageBytes + uoff, g.data->groups.data(), bytes);
        mRegions.push_back({ubase + uoff, (VkDeviceSize)g.base * sizeof(ClusterGroup), bytes});
        uoff += bytes;
    }
    if (!mRegions.empty() && mGroups.buffer)
        vkCmdCopyBuffer(cmd, mStaging.buffer, mGroups.buffer, (uint32_t)mRegions.size(), mRegions.data());
    mGroupWrites.clear();
    mRegions.clear();

    // 5. Usage stamps -> this slot's readback (harvested when the slot comes round).
    if (mUsage.buffer && mUsageRead.buffer && mPoolPages)
    {
        VkBufferCopy c{0, (VkDeviceSize)mSlot * mUsageCap * 4u, (VkDeviceSize)mPoolPages * 4u};
        vkCmdCopyBuffer(cmd, mUsage.buffer, mUsageRead.buffer, 1, &c);
        d.usage_copied = true;
        d.usage_pages = mPoolPages;
    }

    // 6. Page copies (into slots no frame references until their fence-gated flip).
    //    A pool grown this frame orders its prefix copy on graphics: copies stay there.
    if (!mCopies.empty() && mPool.buffer)
    {
        const bool grew = mLastGrowFrame == mFrame;
        const VkCommandBuffer pc = (up_cmd && !grew && !mGrowWaitAsync) ? up_cmd : cmd;
        // #749: the facts of the family that records the copies (graphics, or the async
        // compute family: a forked segment, or the front end itself under frame pipelining).
        const bool pc_async = vkAsyncIsAsyncCmd(pc);
        const bool ts_ok = mTsOk && (pc_async ? mAsyncTsOk : true);
        const bool indirect_ok = pc_async ? mIndirectAsyncOk : mIndirectQueueOk;
        mAsyncUsed = pc_async;
        if (pc != cmd) mAsyncCopyFrame = mFrame;
        if (ts_ok)
        {
            vkCmdResetQueryPool(pc, mTsPool, mSlot * 2u, 2);
            vkCmdWriteTimestamp2(pc, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, mTsPool, mSlot * 2u);
        }
        mUploadTier = std::min<int32_t>(mOwner.caps().get(CLCAP_UPLOAD), CLT_UPLOAD_INDIRECT);   // DECOMPRESS: TODO above
        bool done = false;
        if (mUploadTier >= CLT_UPLOAD_INDIRECT && mCopyIndirect && indirect_ok && mStaging.address && mPool.address)
        {
            // The copy list lives in this slot's staging region (host-written before submit).
            const VkDeviceSize list_off = mStagingSlotBytes * mSlot + mStagingPageBytes + kUpdateBytes;
            uint64_t* list = (uint64_t*)((uint8_t*)mStaging.mapped + list_off);
            const VkDeviceAddress sb = mStaging.address + mStagingSlotBytes * mSlot;
            for (size_t i = 0; i < mCopies.size(); ++i)
            {
                list[i * 3 + 0] = sb + mCopies[i].staging_off;
                list[i * 3 + 1] = mPool.address + (VkDeviceAddress)mCopies[i].slot * kPageBytes;
                list[i * 3 + 2] = mCopies[i].bytes;
            }
#ifdef VK_KHR_copy_memory_indirect
            if (mCopyIndirectKHR)
            {
                VkCopyMemoryIndirectInfoKHR ci{VK_STRUCTURE_TYPE_COPY_MEMORY_INDIRECT_INFO_KHR};
                ci.srcCopyFlags = mStagingDeviceLocal ? VK_ADDRESS_COPY_DEVICE_LOCAL_BIT_KHR : 0;
                ci.dstCopyFlags = VK_ADDRESS_COPY_DEVICE_LOCAL_BIT_KHR;
                ci.copyCount = (uint32_t)mCopies.size();
                ci.copyAddressRange.address = mStaging.address + list_off;
                ci.copyAddressRange.size = (VkDeviceSize)mCopies.size() * 24u;
                ci.copyAddressRange.stride = 24u;
                ((PFN_vkCmdCopyMemoryIndirectKHR)mCopyIndirect)(pc, &ci);
                done = true;
            }
#endif
#ifdef VK_NV_copy_memory_indirect
            if (!done && !mCopyIndirectKHR)
            {
                ((PFN_vkCmdCopyMemoryIndirectNV)mCopyIndirect)(pc, mStaging.address + list_off, (uint32_t)mCopies.size(), 24u);
                done = true;
            }
#endif
        }
        if (!done)
        {
            mUploadTier = CLT_UPLOAD_COPY;
            mRegions.clear();
            const VkDeviceSize pbase = mStagingSlotBytes * mSlot;
            for (const PageCopy& c : mCopies)
                mRegions.push_back({pbase + c.staging_off, (VkDeviceSize)c.slot * kPageBytes, c.bytes});
            vkCmdCopyBuffer(pc, mStaging.buffer, mPool.buffer, (uint32_t)mRegions.size(), mRegions.data());
            mRegions.clear();
        }
        if (ts_ok)
        {
            vkCmdWriteTimestamp2(pc, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, mTsPool, mSlot * 2u + 1u);
            d.ts_written = true;
            d.ts_async = pc_async;
        }
    }
    mCopies.clear();

    // Exit: every transfer write above -> the shader stages that read the cluster buffers,
    // and the usage readback -> host.
    VkPipelineStageFlags2 src = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
#ifdef VK_KHR_copy_memory_indirect
    if (feat.copyIndirectKHR) src |= VK_PIPELINE_STAGE_2_COPY_INDIRECT_BIT_KHR;
#endif
    memBarrier(cmd, src, VK_ACCESS_2_TRANSFER_WRITE_BIT, shaders | VK_PIPELINE_STAGE_2_HOST_BIT,
               VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_INDEX_READ_BIT
                   | VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT
                   | VK_ACCESS_2_HOST_READ_BIT);
}

// ============================================================================================
// stats
// ============================================================================================
// #749 (#697): the recording of the last beginFrame / recordUploads was DROPPED (out of
// memory at End / submit; a partial async submission may have run some of it). Its page
// copies, table / directory / group writes, range and buffer fills and growth prefix copies
// may or may not have run, so no CPU residency state derived from them can be trusted:
// residency restarts (table + directory refilled, meshes re-register, every page re-streams;
// the legacy path draws meanwhile) and the usage stamps are zeroed again. The bytes of the
// lost copies are gone (planUploads hands them to staging), so this is the re-stage.
void LLCleanClusterStreamVK::frameDropped()
{
    resetResidency("frame dropped (#697)");
    mFrameOpen = false;
    mUploadCmd = VK_NULL_HANDLE;
    mGrowCopies.clear();   // (their sources were retired when recorded; residency restarted)
    if (mUsage.buffer) mBufFills.push_back({mUsage.buffer, 0, mUsage.size, 0u});
    mAsyncCopyFrame = 0;
    mGrowWaitAsync = false;
}

void LLCleanClusterStreamVK::summary(std::string& out) const
{
    char buf[256];
    std::snprintf(buf, sizeof(buf), "pages res %u/%u (roots %u) req %.0f/s up %.1f MB/s q %s",
                  mResidentPages, mPoolPages, mResidentRoots, mReqUniqRate.per_sec, mUpRate.per_sec / 1048576.0,
                  mAsyncUsed ? "async" : "gfx");
    out += buf;
    // PAGE FILL (fixed-cost audit): the resident pages' payload vs their 80 KiB slots, all / roots
    // only, and the pool MB (allocated / want)
    if (mFillPages)
    {
        const double all = 100.0 * (double)mFillBytes / ((double)mFillPages * kPageBytes);
        const double rts = mFillRoots ? 100.0 * (double)mFillRootBytes / ((double)mFillRoots * kPageBytes) : 0.0;
        std::snprintf(buf, sizeof(buf), " fill %.0f%% (roots %.0f%%, %.0f/%.0f MB payload/slots) pool %llu/%llu MB", all, rts,
                      (double)mFillBytes / 1048576.0, (double)mFillPages * kPageBytes / 1048576.0,
                      (unsigned long long)(((uint64_t)mPoolPages * kPageBytes) >> 20),
                      (unsigned long long)(((uint64_t)mPoolWant * kPageBytes) >> 20));
        out += buf;
    }
    // Stall diagnostics, only when there is something to say: reads in flight / loaded pages
    // waiting for a slot, wanted pages not started (no source / cache busy / load cap), Live
    // meshes rebuilding, and how long nothing was copied while pages were wanted.
    const uint32_t fly = (uint32_t)mTickets.size() + mLoadedCount;
    uint32_t rebuilding = 0;
    for (const Mesh& m : mMesh) rebuilding += m.rebuilding ? 1u : 0u;
    if (fly || mWantNoSource || mWantBusy || mWantCap || rebuilding)
    {
        std::snprintf(buf, sizeof(buf), " fly %u wait %u (nosrc %u busy %u cap %u) rebuild %u", fly,
                      mWantNoSource + mWantBusy + mWantCap, mWantNoSource, mWantBusy, mWantCap, rebuilding);
        out += buf;
    }
    if (mStallSince > 0.0 && mNow - mStallSince >= kStallLogSec)
    {
        std::snprintf(buf, sizeof(buf), " STALL %.0fs", mNow - mStallSince);
        out += buf;
    }
}

// TIME TO DRAWABLE (2026-10-14): per mesh, first acquire (its instance appeared in the gather)
// -> roots resident (Live: the instance is drawn from the next gather on). p50 / p95 / max over
// the meshes that became drawable since the last summary, split by source (built now vs the
// .clod cache). Resets the window.
void LLCleanClusterStreamVK::ttfdSummary(std::string& out)
{
    const auto pct = [](std::vector<float>& v, float q) -> float
    {
        if (v.empty()) return 0.f;
        const size_t k = std::min(v.size() - 1, (size_t)(q * (float)(v.size() - 1) + 0.5f));
        std::nth_element(v.begin(), v.begin() + (std::ptrdiff_t)k, v.end());
        return v[k];
    };
    std::vector<float> all, built, cached;
    all.reserve(mTtfd.size());
    for (const Ttfd& t : mTtfd)
    {
        all.push_back(t.ms);
        (t.built ? built : cached).push_back(t.ms);
    }
    char b[200];
    if (all.empty()) { out += " | ttfd -"; mTtfdDropped = 0; return; }
    const float mx = *std::max_element(all.begin(), all.end());
    std::snprintf(b, sizeof(b), " | ttfd n %zu p50 %.0f p95 %.0f max %.0f ms (cache %zu p50 %.0f p95 %.0f, built %zu p50 %.0f p95 %.0f)%s",
                  all.size(), pct(all, 0.5f), pct(all, 0.95f), mx, cached.size(), pct(cached, 0.5f), pct(cached, 0.95f),
                  built.size(), pct(built, 0.5f), pct(built, 0.95f), mTtfdDropped ? " +more" : "");
    out += b;
    mTtfd.clear();
    if (mTtfd.capacity() > kTtfdSamples) mTtfd.shrink_to_fit();
    mTtfdDropped = 0;
}


namespace
{
// "name live/cap" in MB with one decimal ("name cap" when live is not tracked: live < 0)
void memItem(std::string& out, const char* name, double live, uint64_t cap)
{
    char b[96];
    if (live < 0.0) std::snprintf(b, sizeof(b), " %s %.1f", name, (double)cap / 1048576.0);
    else std::snprintf(b, sizeof(b), " %s %.1f/%.1f", name, live / 1048576.0, (double)cap / 1048576.0);
    out += b;
}
} // namespace

void LLCleanClusterStreamVK::memSummary(std::string& out) const
{
    out += "stream";
    // pool: pages in use (resident + staged + cooling) / allocated; peak in use
    memItem(out, "pool", (double)mUsedSlots * kPageBytes, (uint64_t)mPoolPages * kPageBytes);
    char b[64];
    std::snprintf(b, sizeof(b), " (peak %.0f, want %.0f, fill %.0f%%)", (double)mPoolPeak * kPageBytes / 1048576.0,
                  (double)mPoolWant * kPageBytes / 1048576.0,
                  mFillPages ? 100.0 * (double)mFillBytes / ((double)mFillPages * kPageBytes) : 0.0);
    out += b;
    // staging: the peak bytes staged per frame x 3 slots / the ring (pages + update areas)
    memItem(out, "staging", (double)std::max(mStagePeakCur, mStagePeakPrev) * kSlots, mStaging.buffer ? mStaging.size : 0);
    memItem(out, "table", (double)mVAlloc.highWater() * 4.0, mTable.buffer ? mTable.size : 0);   // fixed kMaxVirtualPages
    memItem(out, "dir", (double)mMesh.size() * sizeof(ClusterMesh), mMeshes.buffer ? mMeshes.size : 0);
    memItem(out, "groups", (double)mGAlloc.used * sizeof(ClusterGroup), mGroups.buffer ? mGroups.size : 0);
    memItem(out, "req", -1.0, mReq.buffer ? mReq.size : 0);
    memItem(out, "usage", -1.0, (mUsage.buffer ? mUsage.size : 0) + (mUsageRead.buffer ? mUsageRead.size : 0));
}

void LLCleanClusterStreamVK::statsLine(std::string& out) const
{
    const auto mb = [](uint64_t pages) { return std::to_string((pages * kPageBytes) >> 20); };
    static const char* kTier[] = {"copy", "indirect", "decompress"};
    const int32_t t = std::clamp<int32_t>(mUploadTier, 0, 2);
    out += "stream pool " + mb(mUsedSlots) + "/" + mb(mEffCap) + "/" + mb(mPoolPages) + " MB live/cap/alloc (peak "
         + mb(mPoolPeak) + ", target " + mb(mPoolTarget) + "; pages " + std::to_string(mUsedSlots) + "/"
         + std::to_string(mEffCap) + "/" + std::to_string(mPoolPages) + " peak " + std::to_string(mPoolPeak) + ")"
         + (mPressureCap ? " PRESSURE" : "") + " resident " + std::to_string(mResidentPages) + " (roots "
         + std::to_string(mResidentRoots) + "; demand " + std::to_string(mDemandPages) + " = working set " + std::to_string(mWsPages)
         + " + roots " + std::to_string(mWsRoots) + " + slack, pool want " + std::to_string(mPoolWant)
         + (mShrinkTo ? ", SHRINKING to " + std::to_string(mShrinkTo) : std::string()) + ", shrinks " + std::to_string(mShrinks)
         + (mShrinkBlocked ? ", root-capped by " + std::to_string(mShrinkBlocked) : std::string())
         + ", cold trimmed " + std::to_string(mDemandEvicts) + ") meshes " + std::to_string(mLiveMeshes) + " live / "
         + std::to_string(mByKey.size()) + " known (" + std::to_string(mPending) + " pending)"
         // #798: the live meshes by the creator LOD their DAG was built from (3 = high; a rigged
         // mesh on a lower one has its creator seams unwelded -- RenderVKClusterRiggedSourceLOD),
         // and the upgrade swaps waiting for their roots
         + " src lod 3/2/1/0 " + std::to_string(mSrcLod[3]) + "/" + std::to_string(mSrcLod[2]) + "/"
         + std::to_string(mSrcLod[1]) + "/" + std::to_string(mSrcLod[0])
         + (mSrcLod[4] ? " ?" + std::to_string(mSrcLod[4]) : std::string()) + " upgrading " + std::to_string(mShadows);
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "; req %.0f/s (uniq %.0f/s, overflow %llu) up %.1f MB/s %.0f pg/s (total %llu) evict %llu pending %u "
                  "(loading %u, loaded %u) upload gpu %.3f ms (max %.3f) stalls %llu tier %s q %s",
                  mReqRate.per_sec, mReqUniqRate.per_sec, (unsigned long long)mReqOverflow,
                  mUpRate.per_sec / 1048576.0, mUpPageRate.per_sec, (unsigned long long)mUploadsTotal,
                  (unsigned long long)mEvictions, (unsigned)(mTickets.size() + mLoadedCount), (unsigned)mTickets.size(),
                  (unsigned)mLoadedCount, mUploadGpuMs, mUploadGpuMsMax, (unsigned long long)mStalls, kTier[t],
                  mAsyncUsed ? "async" : "gfx");
    out += buf;
    out += "; payload " + std::to_string(mPayloadBytes >> 20) + "/" + std::to_string(mPayloadBudget >> 20)
         + " MB (no disk " + std::to_string(mPayloadNoDisk >> 20) + " MB), wanted w/o source " + std::to_string(mWantNoSource)
         + " (total " + std::to_string(mNoSourceTotal) + "), rebuilds " + std::to_string(mRebuilds) + " (swapped "
         + std::to_string(mRebuildSwaps) + "), bad pages " + std::to_string(mBadPages) + ", load fails " + std::to_string(mLoadFails)
         + ", held " + std::to_string(heldBytes() >> 20) + " MB (staging " + std::to_string(mStaging.size >> 20)
         + " MB = 3 x " + std::to_string(mStagingMB) + " MB pages + updates, peak staged "
         + std::to_string(std::max(mStagePeakCur, mStagePeakPrev) >> 10) + " KiB / frame)";
}
