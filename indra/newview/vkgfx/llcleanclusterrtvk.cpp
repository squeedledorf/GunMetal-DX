/**
 * @file llcleanclusterrtvk.cpp
 * @brief Cluster LOD (#642 / #748, OWNER: 2b): ray tracing on the cluster cut. See the header.
 *
 * Per frame, inside LLCleanRTVK::recordBuild (the RT build's command buffer: graphics or the
 * async compute fork -- every barrier here goes through the vkCmdPipelineBarrier2 sanitizer):
 *   plan()      CPU: settings, tier, the readback of kFrameSlots frames ago (counters; tier 0:
 *               the RT cut + the validity of that frame's builds), the tier-0 build plan,
 *               buffers, the scratch this frame needs.
 *   writeSets() the module set of this frame slot (after LLCleanRTVK sized the TLAS records).
 *   record()    after the RT leading barrier: counters / override reset, CLAS scan, RT
 *               selection, rigged skinning + rigged CLAS build (tiers 1/2), CLAS alloc + decode
 *               + build, cluster BLAS args + build (tiers 1/2) or tier-0 expand + per-cut BLAS
 *               builds + rigged per-cut BLAS builds / refits, the override rows, the readback
 *               copies.
 *   ptlasBuild() in place of the KHR TLAS build when the session's TLAS is partitioned.
 *
 * Method: published material only -- the VK_NV_cluster_acceleration_structure and
 * VK_NV_partitioned_acceleration_structure specifications (indirect argument layouts, size
 * semantics: explicit destinations get the size of ONE structure, implicit ones the total),
 * the cut rule of the shared cluster contract. No engine / other viewer code.
 */
#include "linden_common.h"
#include "../llvkhitch.h"

#include "llcleanclusterrtvk.h"
#include "llcleanrtvk.h"
#include "llcleanclustervk.h"
#include "llcleanclusterselectvk.h"
#include "llcleanclusterstreamvk.h"
#include "llcleanclusterskinvk.h"
#include "llcleanclustercaps.h"
#include "../llcleanclusterformat.h"
#include "vk_pipeline.h"
#include "vk_world_shaders_spv.h"

#include "llerror.h"

#if defined(LL_HAS_CLEAN_PIPELINE)
#  include "llviewercontrol.h"
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <sstream>

namespace
{
using namespace LLClusterFmt;

// clean_cluster_rt.comp push constants (128 B, the spec minimum).
struct ClRtPush
{
    float cam[4];
    float px_scale, tol_px, hyst;
    uint32_t rt_flags;                         // bit 0: previous cut arena valid; bits 1..: cut epoch
    uint32_t mode, instance_count, ri_cap, frame;
    uint32_t cut_cap, max_cut, pool_pages, clas_pages_max;
    uint32_t clas_cap, blas_max, tier, job_count;
    uint32_t pool_addr[2], decode_addr[2];
    uint32_t clas_base[2], clas_refs_addr[2];
    uint32_t clas_stride, idle_frames, part_size, ptlas_full;
};
static_assert(sizeof(ClRtPush) == 128, "ClRtPush must stay at the 128 B push minimum");

constexpr uint32_t kBindings = 36;            // clean_cluster_rt.comp set 1 (26..31: rigged, 32: previous cut,
                                              // 33 / 34: static chunk table + heap parameters / heap bookkeeping,
                                              // 35: spatial PTLAS partition state)
constexpr uint32_t kGridX = 32768;            // dispatch rows (as 1b)
// readback: 256 B counters 0..63 + 256 B validity words + (#1008) the counters past word 63 at
// kRbExtra, then (tier 0) the instance rows + the cut
constexpr VkDeviceSize kRbExtra = 512;
constexpr VkDeviceSize kRbHead = 768;
static_assert(kRbExtra + (LLCleanClusterRTVK::kCntWords - 64) * 4 <= kRbHead, "the extra counters fit the readback head");
constexpr VkDeviceSize kPtlasOpBytes = 104;   // VkPartitionedAccelerationStructureWriteInstanceDataNV
constexpr VkDeviceSize kPtlasCmdBytes = 32;   // indirect command (24 B) + srcInfosCount (4) + pad
constexpr VkDeviceSize kT0ScratchBudget = 64ull << 20;
constexpr uint32_t kTierRetryFrames = 600;    // a failed tier is retried this much later
// STATIC (#748 static pass): binding 33 words (= clean_cluster_rt.comp SC_*) and the heap layout
constexpr uint32_t kScChunkPages = 0, kScChunks = 1, kScSbOn = 2, kScSbEpoch = 3, kScSbAddr = 4, kScSbUnits = 6,
                   kScSbUnit = 7, kScSbClasses = 8, kScStackCap = 9, kScPendCap = 10, kScOffBuilt = 11,
                   kScOffStacks = 12, kScClassUnits = 16, kScClassCount = 48, kScChunkBase = 80;
constexpr uint32_t kSbHdrWords = 64;          // = SB_PEND: bump, last free, tops, then the pending frees
constexpr uint32_t kSbOffBuilt = kSbHdrWords + 2 * LLCleanClusterRTVK::kSbPendCap;
constexpr uint32_t kSbOffStacks = kSbOffBuilt + LLCleanClusterRTVK::kMaxClasChunks * LLCleanClusterRTVK::kClasChunkPages;
constexpr uint32_t kSbMetaWords = kSbOffStacks + LLCleanClusterRTVK::kSbClasses * LLCleanClusterRTVK::kSbStackCap;
// Clusters a BLAS of each size class holds at most (steps of 1.33 / 1.5: <= 1.5x the bytes needed).
constexpr uint32_t kSbClassCounts[LLCleanClusterRTVK::kSbClasses] = {
    1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048};
static_assert(LLCleanClusterRTVK::kSbClasses <= 32 && kScClassUnits + LLCleanClusterRTVK::kSbClasses <= kScClassCount
              && kScClassCount + LLCleanClusterRTVK::kSbClasses <= kScChunkBase
              && kScChunkBase + 2 * LLCleanClusterRTVK::kMaxClasChunks <= LLCleanClusterRTVK::kStaticCpuWords,
              "binding 33 layout");
static_assert(kSbClassCounts[LLCleanClusterRTVK::kSbClasses - 1] == LLCleanClusterRTVK::kMaxCutPerInst, "the largest class holds any cut");
static_assert(4 + LLCleanClusterRTVK::kSbClasses <= kSbHdrWords, "= SB_TOPS + classes <= SB_PEND");
constexpr VkDeviceSize kSbMinBytes = 4ull << 20;   // the persistent static BLAS heap's floor

static_assert(LLCleanClusterRTVK::kMaxCutPerInst <= 2048, "= CL_RT_SHARED_CUT");
static_assert(LLCleanClusterSelectVK::kInstDraw == 1024u, "= CLI1B_DRAW_RT in clean_cluster_rt.comp");
static_assert(LLCleanClusterRTVK::kMaxT0Builds * 4 <= 256, "validity words fit the readback head");

inline VkDeviceSize alignUpD(VkDeviceSize v, VkDeviceSize a) { return a ? (v + a - 1) / a * a : v; }
inline uint32_t lo32(VkDeviceAddress a) { return (uint32_t)(a & 0xFFFFFFFFull); }
inline uint32_t hi32(VkDeviceAddress a) { return (uint32_t)(a >> 32); }
inline uint32_t nextPow2(uint32_t v) { uint32_t p = 1; while (p < v && p < 0x80000000u) p <<= 1; return p; }

constexpr VkPipelineStageFlags2 kCS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
constexpr VkPipelineStageFlags2 kAS = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
constexpr VkPipelineStageFlags2 kXfer = VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT;
constexpr VkAccessFlags2 kMR = VK_ACCESS_2_MEMORY_READ_BIT;
constexpr VkAccessFlags2 kMW = VK_ACCESS_2_MEMORY_WRITE_BIT;

// MEMORY_READ / WRITE are legal in every stage: the cluster AS builds write their address /
// size arrays and read their argument arrays through the AS build stage.
void bar(VkCommandBuffer cmd, VkPipelineStageFlags2 ss, VkPipelineStageFlags2 ds)
{
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = ss; b.srcAccessMask = kMW;
    b.dstStageMask = ds; b.dstAccessMask = kMR | kMW;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1; d.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &d);
}

double nowSec()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Settings
{
    uint32_t mode = 2;        // RenderVKClusterRT
    float tol = 2.f;          // RenderVKClusterRTTolerancePx
    float hyst = 0.25f;       // RenderVKClusterRTHysteresis
    uint32_t builds = 8;      // RenderVKClusterRTBuildsPerFrame
    uint32_t memMB = 0;       // RenderVKClusterRTMemoryMB
    uint32_t rigged = 1;      // RenderVKClusterRTRigged
    uint32_t statics = 1;     // RenderVKClusterRTStatic
    bool persist = true;      // RenderVKClusterRTStaticPersist (#748 static pass)
    float rasterTol = 1.f;    // RenderVKClusterRTStaticRasterTol (static cut perf pass)
    uint32_t staticMB = 0;    // RenderVKClusterRTStaticMB (static cut perf pass)
    // #1008 rigged RT cadence (clean_cluster_rt.comp header)
    bool rigCadence = true;   // RenderVKClusterRTRigCadence (false = the A/B: every frame, the raster's cut)
    float rigNearM = 8.f;     // RenderVKClusterRTRigNearM
    float rigFarM = 24.f;     // RenderVKClusterRTRigFarM
    uint32_t rigPeriod[3] = {1, 2, 4};          // RenderVKClusterRTRigPeriodNear / Mid / Far
    float rigTolMul[3] = {1.f, 1.f, 2.f};      // RenderVKClusterRTRigTolNear / Mid / Far
    uint32_t rigMaxStale = 8;                   // RenderVKClusterRTRigMaxStale
    float rigPoseEps = 0.5f;                    // RenderVKClusterRTRigPoseEps
    // #1011 (clean_cluster_rt.comp header)
    bool rigRank = true;                        // RenderVKClusterRTRigRank (false = the A/B: mode 11 gather, atomic order)
    bool rigReuse = true;                       // RenderVKClusterRTRigReuse (false = the A/B: every due slot rebuilt)
    float rigReuseEps = 2.0e-5f;                // RenderVKClusterRTRigReuseEps (m, self-shadowing avatars)
};
// PTLAS PARTITIONING (read when the PTLAS is (re)created): 0 = by instance id (the original: id /
// 512 -- ids come from a lowest-free allocator, i.e. arrival order, so every partition spans the
// scene and a ray visits most of them: 3-35x slower traces, 2026-10-14), 1 = SPATIAL (default): the
// grid cell of the instance's translation (RenderVKClusterPTLASCellM).
uint32_t settingPtlasPartition()
{
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<U32> s(gSavedSettings, "RenderVKClusterPTLASPartition", 1);
    return std::min<U32>(s, 1u);
#else
    return 1;
#endif
}
float settingPtlasCell()
{
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<F32> s(gSavedSettings, "RenderVKClusterPTLASCellM", 64.f);
    const float v = s;
    return std::isfinite(v) ? std::clamp(v, 8.f, 1024.f) : 64.f;
#else
    return 64.f;
#endif
}
Settings readSettings()
{
    Settings s;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<U32> s_mode(gSavedSettings, "RenderVKClusterRT", 1);
    static LLCachedControl<F32> s_tol(gSavedSettings, "RenderVKClusterRTTolerancePx", 2.f);
    static LLCachedControl<F32> s_hyst(gSavedSettings, "RenderVKClusterRTHysteresis", 0.25f);
    static LLCachedControl<U32> s_builds(gSavedSettings, "RenderVKClusterRTBuildsPerFrame", 8);
    static LLCachedControl<U32> s_mem(gSavedSettings, "RenderVKClusterRTMemoryMB", 0);
    static LLCachedControl<U32> s_rig(gSavedSettings, "RenderVKClusterRTRigged", 1);
    static LLCachedControl<U32> s_static(gSavedSettings, "RenderVKClusterRTStatic", 1);
    static LLCachedControl<bool> s_persist(gSavedSettings, "RenderVKClusterRTStaticPersist", true);
    static LLCachedControl<F32> s_rtol(gSavedSettings, "RenderVKClusterRTStaticRasterTol", 1.f);
    static LLCachedControl<U32> s_smb(gSavedSettings, "RenderVKClusterRTStaticMB", 0);
    s.rasterTol = s_rtol;
    s.staticMB = s_smb;
    s.mode = s_mode; s.tol = s_tol; s.hyst = s_hyst; s.builds = s_builds; s.memMB = s_mem; s.rigged = s_rig;
    s.statics = s_static;
    s.persist = s_persist;
    static LLCachedControl<U32> s_cad(gSavedSettings, "RenderVKClusterRTRigCadence", 0);
    static LLCachedControl<F32> s_nm(gSavedSettings, "RenderVKClusterRTRigNearM", 8.f);
    static LLCachedControl<F32> s_fm(gSavedSettings, "RenderVKClusterRTRigFarM", 24.f);
    static LLCachedControl<U32> s_pn(gSavedSettings, "RenderVKClusterRTRigPeriodNear", 1);
    static LLCachedControl<U32> s_pm(gSavedSettings, "RenderVKClusterRTRigPeriodMid", 2);
    static LLCachedControl<U32> s_pf(gSavedSettings, "RenderVKClusterRTRigPeriodFar", 4);
    static LLCachedControl<F32> s_tn(gSavedSettings, "RenderVKClusterRTRigTolNear", 1.f);
    static LLCachedControl<F32> s_tm(gSavedSettings, "RenderVKClusterRTRigTolMid", 1.f);
    static LLCachedControl<F32> s_tf(gSavedSettings, "RenderVKClusterRTRigTolFar", 2.f);
    static LLCachedControl<U32> s_ms(gSavedSettings, "RenderVKClusterRTRigMaxStale", 8);
    static LLCachedControl<F32> s_pe(gSavedSettings, "RenderVKClusterRTRigPoseEps", 0.5f);
    s.rigCadence = (U32)s_cad != 0u;
    s.rigNearM = s_nm; s.rigFarM = s_fm;
    s.rigPeriod[0] = s_pn; s.rigPeriod[1] = s_pm; s.rigPeriod[2] = s_pf;
    s.rigTolMul[0] = s_tn; s.rigTolMul[1] = s_tm; s.rigTolMul[2] = s_tf;
    s.rigMaxStale = s_ms;
    s.rigPoseEps = s_pe;
    static LLCachedControl<U32> s_rank(gSavedSettings, "RenderVKClusterRTRigRank", 1);
    static LLCachedControl<U32> s_reuse(gSavedSettings, "RenderVKClusterRTRigReuse", 1);
    static LLCachedControl<F32> s_reps(gSavedSettings, "RenderVKClusterRTRigReuseEps", 2.0e-5f);
    s.rigRank = (U32)s_rank != 0u;
    s.rigReuse = (U32)s_reuse != 0u;
    s.rigReuseEps = s_reps;
#endif
    s.tol = std::clamp(s.tol, 0.05f, 1024.f);   // = the controller's clamp (llcleanclustercontrolvk.cpp)
    s.hyst = std::clamp(s.hyst, 0.f, 0.9f);
    s.builds = std::clamp<uint32_t>(s.builds, 1u, LLCleanClusterRTVK::kMaxT0Builds);
    s.rasterTol = std::isfinite(s.rasterTol) ? std::clamp(s.rasterTol, 0.f, 16.f) : 1.f;
    // (#1008) the max staleness caps every period; the multipliers never go finer than the raster
    s.rigMaxStale = std::clamp<uint32_t>(s.rigMaxStale, 1u, 120u);
    s.rigNearM = std::isfinite(s.rigNearM) ? std::clamp(s.rigNearM, 0.f, 4096.f) : 8.f;
    s.rigFarM = std::isfinite(s.rigFarM) ? std::clamp(s.rigFarM, s.rigNearM, 4096.f) : std::max(24.f, s.rigNearM);
    for (uint32_t b = 0; b < 3; ++b)
    {
        s.rigPeriod[b] = std::clamp<uint32_t>(s.rigPeriod[b], 1u, s.rigMaxStale);
        s.rigTolMul[b] = std::isfinite(s.rigTolMul[b]) ? std::clamp(s.rigTolMul[b], 1.f, 64.f) : 1.f;
    }
    s.rigPoseEps = std::isfinite(s.rigPoseEps) ? std::clamp(s.rigPoseEps, 0.f, 16.f) : 0.5f;
    // (#1011: never past 1 mm -- a self-shadowing avatar's receivers must not meet a stale hull)
    s.rigReuseEps = std::isfinite(s.rigReuseEps) ? std::clamp(s.rigReuseEps, 0.f, 1.0e-3f) : 2.0e-5f;
    return s;
}
} // namespace

// ============================================================================================
// range allocator (tier-0 hit cut)
// ============================================================================================
bool LLCleanClusterRTVK::RangeAlloc::alloc(uint32_t n, uint32_t& out)
{
    for (size_t i = 0; i < free.size(); ++i)
    {
        if (free[i].second < n) continue;
        out = free[i].first;
        free[i].first += n;
        free[i].second -= n;
        if (!free[i].second) free.erase(free.begin() + (ptrdiff_t)i);
        return true;
    }
    return false;
}

void LLCleanClusterRTVK::RangeAlloc::release(uint32_t start, uint32_t n)
{
    if (!n) return;
    auto it = std::lower_bound(free.begin(), free.end(), std::make_pair(start, 0u));
    it = free.insert(it, {start, n});
    // merge with the next, then the previous
    if (it + 1 != free.end() && it->first + it->second == (it + 1)->first)
    {
        it->second += (it + 1)->second;
        free.erase(it + 1);
    }
    if (it != free.begin() && (it - 1)->first + (it - 1)->second == it->first)
    {
        (it - 1)->second += it->second;
        free.erase(it);
    }
}

// ============================================================================================
// init / shutdown
// ============================================================================================
LLCleanClusterRTVK::LLCleanClusterRTVK(LLCleanRTVK& rt) : mRT(rt), mCtx(rt.mCtx) {}

LLCleanClusterRTVK::~LLCleanClusterRTVK() { shutdown(); }

bool LLCleanClusterRTVK::init(bool ptlas_wanted)
{
    if (!mCtx) return false;
    VkMemScope memcat(VKMEM_RT_AUX);
    const VkDevice dev = mCtx->device();
    {
        VkDescriptorSetLayoutBinding b[kBindings]{};
        for (uint32_t i = 0; i < kBindings; ++i)
        {
            b[i].binding = i;
            b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            b[i].descriptorCount = 1;
            b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = kBindings; ci.pBindings = b;
        if (vkdCreateSetLayout(dev, &ci, &mDsl, "mDslClusterRT") != VK_SUCCESS) { mDsl = VK_NULL_HANDLE; return false; }
        VkDescriptorSetLayoutCreateInfo ei{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        if (vkdCreateSetLayout(dev, &ei, &mEmptyDsl, "mDslClusterRTEmpty") != VK_SUCCESS) { mEmptyDsl = VK_NULL_HANDLE; return false; }
    }
    if (mPool.create(dev, "clusterRT", {{mDsl, kFrameSlots}}) != VK_SUCCESS) return false;
    for (uint32_t s = 0; s < kFrameSlots; ++s)
        if (mPool.allocate(mDsl, &mSets[s]) != VK_SUCCESS) return false;
    mDummy = mCtx->createBuffer(256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!mDummy.buffer || !mDummy.mapped) return false;
    std::memset(mDummy.mapped, 0, 256);
    vkAddrName(mDummy.buffer, "rt.cl.dummy");

    const VkClusterFeatures& f = mCtx->clusterFeatures();
#ifdef VK_NV_cluster_acceleration_structure
    if (f.clusterAS)
    {
        mGetClasSizes = vkGetDeviceProcAddr(dev, "vkGetClusterAccelerationStructureBuildSizesNV");
        mCmdBuildClas = vkGetDeviceProcAddr(dev, "vkCmdBuildClusterAccelerationStructureIndirectNV");
        VkPhysicalDeviceClusterAccelerationStructurePropertiesNV cp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CLUSTER_ACCELERATION_STRUCTURE_PROPERTIES_NV};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &cp;
        vkGetPhysicalDeviceProperties2(mCtx->physicalDevice(), &p2);
        mClasScratchAlign = std::max(cp.clusterScratchByteAlignment, 1u);
        mClasAlign = std::max(cp.clusterByteAlignment, 1u);
        mClasBlasAlign = std::max(cp.clusterBottomLevelByteAlignment, 1u);
        mMaxClusterGeomIndex = cp.maxClusterGeometryIndex;
        mClasOk = mGetClasSizes && mCmdBuildClas && cp.maxVerticesPerCluster >= kMaxClusterVerts
               && cp.maxTrianglesPerCluster >= kMaxClusterTris && mMaxClusterGeomIndex > 0;
    }
#endif
#ifdef VK_NV_partitioned_acceleration_structure
    if (f.partitionedAS)
    {
        mGetPtlasSizes = vkGetDeviceProcAddr(dev, "vkGetPartitionedAccelerationStructuresBuildSizesNV");
        mCmdBuildPtlas = vkGetDeviceProcAddr(dev, "vkCmdBuildPartitionedAccelerationStructuresNV");
        VkPhysicalDevicePartitionedAccelerationStructurePropertiesNV pp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PARTITIONED_ACCELERATION_STRUCTURE_PROPERTIES_NV};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &pp;
        vkGetPhysicalDeviceProperties2(mCtx->physicalDevice(), &p2);
        mMaxPartitions = pp.maxPartitionCount;
    }
#endif
    (void)f;
    // PTLAS needs its own pipeline (no cluster set): create it now, it may run every frame.
    if (ptlas_wanted && mGetPtlasSizes && mCmdBuildPtlas && mMaxPartitions > 0)
    {
        VkDescriptorSetLayout sl[2] = {mEmptyDsl, mDsl};
        VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ClRtPush)};
        VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        li.setLayoutCount = 2; li.pSetLayouts = sl;
        li.pushConstantRangeCount = 1; li.pPushConstantRanges = &pr;
        if (vkdCreatePipelineLayout(dev, &li, &mPtlasLayoutObj, "mClusterRTPtlasLayout") == VK_SUCCESS)
        {
            VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            mi.codeSize = kWorldClusterRtPtlasSpv_bytes; mi.pCode = kWorldClusterRtPtlasSpv;
            VkShaderModule m = VK_NULL_HANDLE;
            if (vkCreateShaderModule(dev, &mi, nullptr, &m) == VK_SUCCESS)
            {
                mPtlasPipe = createComputePipeline(dev, m, mPtlasLayoutObj);
                vkDestroyShaderModule(dev, m, nullptr);
            }
        }
        else mPtlasLayoutObj = VK_NULL_HANDLE;
        mPtlasLayout = mPtlasPipe != VK_NULL_HANDLE;
    }
    LL_INFOS("Vulkan") << "Cluster RT (#748): CLAS " << (mClasOk ? "available" : "unavailable")
                       << " (align " << mClasAlign << " / blas " << mClasBlasAlign << " / scratch " << mClasScratchAlign
                       << ", max geometry index " << mMaxClusterGeomIndex << "); partitioned TLAS "
                       << (mPtlasLayout ? "ON for this session" : (mGetPtlasSizes ? "available, not wanted (RenderVKClusterRT < 2)" : "unavailable"))
                       << (mMaxPartitions ? " (max partitions " + std::to_string(mMaxPartitions) + ")" : std::string()) << LL_ENDL;
    return true;
}

void LLCleanClusterRTVK::shutdown()
{
    if (!mCtx) return;
    const VkDevice dev = mCtx->device();
    // Device idle: everything goes now (the RT retire ring is flushed by LLCleanRTVK).
    for (auto& kv : mT0)
    {
        for (T0Blas* b : {&kv.second.cur, &kv.second.pend})
            if (b->as) mCtx->rt().vkDestroyAccelerationStructureKHR(dev, b->as, nullptr);
    }
    mT0.clear();
    for (auto& kv : mT0Rig)
        if (kv.second.as) mCtx->rt().vkDestroyAccelerationStructureKHR(dev, kv.second.as, nullptr);
    mT0Rig.clear();
    GpuBuffer* bufs[] = {&mState, &mInstRows, &mCut, &mCutPrev, &mOvr, &mCnt, &mDummy, &mClasOfSlot, &mClasFree, &mClasWant,
                         &mClasStamp, &mClasArgs, &mClasDst, &mClasSize, &mDecode, &mClasRefs, &mBlasInfo, &mBlasAddr,
                         &mBlasOf, &mBlasArena, &mT0Idx, &mHitCut, &mT0Valid, &mTlasPrev, &mPtlasOps,
                         &mPtlasCmd, &mPtlas, &mPtlasPart, &mRig, &mRigOf, &mRigArgs, &mRigAddr, &mRigClasArena, &mSbArena, &mSbMeta};
    for (GpuBuffer* b : bufs) if (b->buffer) mCtx->destroyBuffer(*b);
    for (GpuBuffer& b : mClasChunks) if (b.buffer) mCtx->destroyBuffer(b);
    mClasChunks.clear();
    for (uint32_t s = 0; s < kFrameSlots; ++s)
        for (GpuBuffer* b : {&mT0Jobs[s], &mT0Rows[s], &mT0Stage[s], &mReadback[s], &mRigCpu[s], &mRigStage[s], &mTlasRig[s],
                             &mStaticCpu[s]})
            if (b->buffer) mCtx->destroyBuffer(*b);
    for (auto& o : mOldPipes)
    {
        if (o.pipe) vkDestroyPipeline(dev, o.pipe, nullptr);
        if (o.layout) vkDestroyPipelineLayout(dev, o.layout, nullptr);
    }
    mOldPipes.clear();
    // A build in flight references mLayout (#782 audit): wait, free its result.
    mPipeJob.wait();
    if (mPipeStage) { vkDestroyPipeline(dev, mPipeStage, nullptr); mPipeStage = VK_NULL_HANDLE; }
    if (mPipe) vkDestroyPipeline(dev, mPipe, nullptr);
    if (mLayout) vkDestroyPipelineLayout(dev, mLayout, nullptr);
    if (mPtlasPipe) vkDestroyPipeline(dev, mPtlasPipe, nullptr);
    if (mPtlasLayoutObj) vkDestroyPipelineLayout(dev, mPtlasLayoutObj, nullptr);
    mPipe = mPtlasPipe = VK_NULL_HANDLE;
    mLayout = mPtlasLayoutObj = VK_NULL_HANDLE;
    mPool.destroy();
    for (VkDescriptorSet& s : mSets) s = VK_NULL_HANDLE;
    if (mDsl) vkdDestroySetLayout(dev, mDsl);
    if (mEmptyDsl) vkdDestroySetLayout(dev, mEmptyDsl);
    mDsl = mEmptyDsl = VK_NULL_HANDLE;
    mCtx = nullptr;
}

// ============================================================================================
// buffers
// ============================================================================================
bool LLCleanClusterRTVK::ensureBuf(GpuBuffer& b, VkDeviceSize need, VkBufferUsageFlags usage, bool host,
                                   const char* name, bool as_storage)
{
    need = std::max<VkDeviceSize>(need, 256);
    if (b.buffer && b.size >= need) return true;
    retire(b);
    VkMemScope memcat(as_storage ? VKMEM_RT_AS : VKMEM_RT_AUX);
    VkDeviceContext::SoftAllocScope soft(mCtx);   // a failure steps the tier down, never aborts
    // grow in steps (x1.5) so a slowly rising need does not re-create every frame -- not the AS
    // arenas: their callers size them exactly (in steps of their own) and their bytes are the
    // cluster RT memory cap's (x1.5 here made the 358 MB CLAS arena hold 537 MB, #748 perf pass)
    const VkDeviceSize size = (host || as_storage) ? need : need + need / 2;
    b = mCtx->createBuffer(size, usage | (as_storage ? VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR : 0),
                           host ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                   | ((usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT) ? VK_MEMORY_PROPERTY_HOST_CACHED_BIT : 0))
                                : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                           true);
    if (b.buffer && host && !b.mapped) mCtx->destroyBuffer(b);
    if (!b.buffer) { b = GpuBuffer{}; return false; }
    vkAddrName(b.buffer, name);
    return true;
}

void LLCleanClusterRTVK::retire(GpuBuffer& b)
{
    if (b.buffer) mRT.retireBuf(b);   // freed when this frame slot comes round (RT retire ring)
    b = GpuBuffer{};
}

uint64_t LLCleanClusterRTVK::asBytes() const
{
    // (tier-0 BLAS live in the RT arena chunks: LLCleanRTVK counts those itself)
    uint64_t n = mBlasArena.size + mPtlas.size + mRigClasArena.size + mSbArena.size;
    for (const GpuBuffer& b : mClasChunks) n += b.size;
    return n;
}

uint64_t LLCleanClusterRTVK::auxBytes() const
{
    uint64_t n = 0;
    const GpuBuffer* bufs[] = {&mState, &mInstRows, &mCut, &mCutPrev, &mOvr, &mCnt, &mClasOfSlot, &mClasFree, &mClasWant,
                               &mClasStamp, &mClasArgs, &mClasDst, &mClasSize, &mDecode, &mClasRefs, &mBlasInfo,
                               &mBlasAddr, &mBlasOf, &mT0Idx, &mHitCut, &mT0Valid, &mTlasPrev, &mPtlasOps, &mPtlasCmd,
                               &mPtlasPart, &mRig, &mRigOf, &mRigArgs, &mRigAddr, &mSbMeta};
    for (const GpuBuffer* b : bufs) n += b->size;
    for (uint32_t s = 0; s < kFrameSlots; ++s)
        n += mT0Jobs[s].size + mT0Rows[s].size + mT0Stage[s].size + mReadback[s].size + mRigCpu[s].size + mRigStage[s].size
           + mTlasRig[s].size + mStaticCpu[s].size;
    return n;
}

// ============================================================================================
// pipeline (lazy: its layout includes the cluster module's set layout)
// ============================================================================================
// FIRST USE OFF THE RENDER THREAD: the layout here, the compute pipeline (~1.2 s cold on a
// 5090, measured as vk_rtClusterPlan inside a frame) in a background batch -- false ("RT
// keeps the legacy BLAS" for those frames) until it lands.
bool LLCleanClusterRTVK::ensurePipeline(LLCleanClusterVK& cl)
{
    const VkDescriptorSetLayout cdsl = cl.setLayout();
    if (mPipeJob.pending())
    {
        if (!mPipeJob.poll()) return false;   // still compiling
        mPipe = mPipeStage;
        mPipeStage = VK_NULL_HANDLE;
        if (!mPipe)
        {
            LL_WARNS("Vulkan") << "Cluster RT: compute pipeline creation failed; RT keeps the legacy BLAS" << LL_ENDL;
            if (mLayout) vkDestroyPipelineLayout(mCtx->device(), mLayout, nullptr);
            mLayout = VK_NULL_HANDLE;
            mClusterDsl = VK_NULL_HANDLE;
            mPipeFailFrame = std::max(mFrame, 1u);
            return false;
        }
    }
    if (mPipe && mClusterDsl == cdsl) return true;
    if (mPipeFailFrame && mFrame < mPipeFailFrame + kTierRetryFrames) return false;
    // A re-created cluster module: the old pipeline may still be referenced by frames in flight.
    if (mPipe || mLayout) mOldPipes.push_back({mPipe, mLayout, mFrame});
    mPipe = VK_NULL_HANDLE; mLayout = VK_NULL_HANDLE; mClusterDsl = VK_NULL_HANDLE;
    if (!cdsl) return false;
    const VkDevice dev = mCtx->device();
    VkDescriptorSetLayout sl[2] = {cdsl, mDsl};
    VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ClRtPush)};
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = 2; li.pSetLayouts = sl;
    li.pushConstantRangeCount = 1; li.pPushConstantRanges = &pr;
    if (vkdCreatePipelineLayout(dev, &li, &mLayout, "mClusterRTLayout") != VK_SUCCESS)
    {
        mLayout = VK_NULL_HANDLE;
        mPipeFailFrame = std::max(mFrame, 1u);
        return false;
    }
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = kWorldClusterRtSpv_bytes; mi.pCode = kWorldClusterRtSpv;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &mi, nullptr, &m) != VK_SUCCESS)
    {
        LL_WARNS("Vulkan") << "Cluster RT: shader module creation failed; RT keeps the legacy BLAS" << LL_ENDL;
        vkDestroyPipelineLayout(dev, mLayout, nullptr);
        mLayout = VK_NULL_HANDLE;
        mPipeFailFrame = std::max(mFrame, 1u);
        return false;
    }
    vkNoteShaderModule(m, kWorldClusterRtSpv, kWorldClusterRtSpv_bytes, "kWorldClusterRtSpv");
    {
        PipelineBatchScope batchScope(dev);
        createComputePipelineInto(dev, m, mLayout, 0, nullptr, &mPipeStage, "cluster RT plan");
        destroyShaderModuleDeferred(dev, m, nullptr);
        PipelineJobOpts o;
        o.what = "cluster RT plan";
        mPipeJob.start(pipelineBatchFlushAsync(dev, o));
    }
    mClusterDsl = cdsl;
    return false;
}

void LLCleanClusterRTVK::tierFail(const char* why)
{
    if (mTier <= 0) { mTierCap = -1; }
    else mTierCap = mTier - 1;
    mTierRetryFrame = mFrame + kTierRetryFrames;
    if (mWhy != why)
        LL_WARNS("Vulkan") << "Cluster RT: tier " << mTier << " unavailable (" << why << "); tier "
                           << mTierCap << " for " << kTierRetryFrames << " frames" << LL_ENDL;
    mWhy = why;
}

// ============================================================================================
// release
// ============================================================================================
void LLCleanClusterRTVK::releaseT0(T0Blas& b)
{
    if (b.as || b.chunk >= 0)
    {
        LLCleanRTVK::Alloc a;
        a.chunk = b.chunk; a.off = b.off; a.size = b.size;
        mRT.retireAS(b.as, a);
        mT0Bytes -= std::min<uint64_t>(mT0Bytes, b.size);
    }
    if (b.cut.count) mHitFree.push_back({b.cut, mFrame});
    b = T0Blas{};
}

void LLCleanClusterRTVK::releaseClas()
{
    for (GpuBuffer* b : {&mClasOfSlot, &mClasFree, &mClasWant, &mClasStamp, &mClasArgs, &mClasDst, &mClasSize,
                         &mClasRefs, &mBlasInfo, &mBlasAddr, &mBlasOf, &mBlasArena})
        retire(*b);
    for (GpuBuffer& b : mClasChunks) retire(b);
    mClasChunks.clear();
    mClasCap = 0; mClasPoolPages = 0; mClasStride = 0; mClasMaxGeom = 0; mBlasMax = 0;
    mClasGrowFrom = 0; mClasOverSince = 0;
    mSlotCopyOf = mSlotCopyStamp = VK_NULL_HANDLE; mSlotCopyPages = 0;
    mClasReset = true;
    // every persistent static BLAS references CLAS of the old arena: a new heap epoch
    ++mSbEpoch; if (!mSbEpoch) mSbEpoch = 1;
    mSbReset = true;
}

void LLCleanClusterRTVK::releaseStaticBlas()
{
    retire(mSbArena);
    mSbUnits = 0;
    mSbOn = false;
    ++mSbEpoch; if (!mSbEpoch) mSbEpoch = 1;
    mSbReset = true;
}

void LLCleanClusterRTVK::releaseAll()
{
    for (auto& kv : mT0) { releaseT0(kv.second.cur); releaseT0(kv.second.pend); }
    mT0.clear();
    releaseClas();
    releaseStaticBlas();
    for (GpuBuffer* b : {&mState, &mInstRows, &mCut, &mCutPrev, &mOvr, &mDecode, &mT0Idx, &mHitCut, &mT0Valid, &mTlasPrev,
                         &mPtlasOps, &mPtlasCmd, &mPtlas, &mPtlasPart, &mSbMeta})
        retire(*b);
    mCutWritten = 0;   // (static cut reuse: no previous cut)
    for (uint32_t s = 0; s < kFrameSlots; ++s)
    {
        for (GpuBuffer* b : {&mT0Jobs[s], &mT0Rows[s], &mT0Stage[s], &mReadback[s]}) retire(*b);
        mRbValid[s] = false; mRbInst[s] = 0; mRbCut[s] = false; mRbBuilds[s].clear();
    }
    mHitFree.clear();
    mHitAlloc.reset(0);
    mPtlasCap = 0; mPtlasAddr = 0; mPtlasScratch = 0; mPtlasFresh = true;
    releaseRig();
    for (uint32_t s = 0; s < kFrameSlots; ++s)
    {
        retire(mRigCpu[s]); retire(mTlasRig[s]); retire(mStaticCpu[s]);
        mTlasRigValid[s] = false;
    }
    retire(mRigOf);
    mRigAvRows = 0;
    mStateFill = true;
    mSbCooldown = kFrameSlots + 2;
    mActive = false;
    mRigOn = false;
    mTlasRigOn = false;
}

void LLCleanClusterRTVK::abortPlan()
{
    // The RT frame was skipped after plan(): this frame's tier-0 BLAS were never built.
    for (const T0Rec& r : mT0Recs)
    {
        auto it = mT0.find(r.ri);
        if (it != mT0.end()) releaseT0(it->second.pend);
    }
    mT0Recs.clear(); mT0Geoms.clear(); mT0Ranges.clear();
    mRbBuilds[mSlot].clear();
    mT0Jobs_n = mT0Rows_n = 0;
    unwindT0Rig();   // (rigged tier 0: this frame's builds were never recorded)
    if (mSlotCopyOf || mSlotCopyStamp)
    {   // (the grown CLAS slot tables were never filled from the old ones: every CLAS goes)
        mSlotCopyOf = mSlotCopyStamp = VK_NULL_HANDLE;
        mSlotCopyPages = 0;
        mClasReset = true;
        mClasGrowFrom = mClasCap;
        ++mSbEpoch; if (!mSbEpoch) mSbEpoch = 1;
        mSbReset = true;
    }
    mActive = false;
    mRigOn = false;
    mTlasRigOn = false;
}

void LLCleanClusterRTVK::dropAfterLostFrame()
{
    // Builds recorded into a frame that never ran: CLAS / BLAS / PTLAS contents are garbage.
    releaseAll();
    mRigKeepReset = true;   // (and nothing rigged is kept on the next frame)
    mRigRecordedLast = false;
}

// ============================================================================================
// readback harvest (this slot's frame, kFrameSlots ago; its fence was waited)
// ============================================================================================
void LLCleanClusterRTVK::harvest(uint32_t s)
{
    if (!mRbValid[s] || !mReadback[s].mapped)
    {
        // that frame recorded no readback: its tier-0 builds (if any) were never validated
        for (const T0Build& b : mRbBuilds[s])
        {
            auto it = mT0.find(b.ri);
            if (it != mT0.end() && it->second.pend.valid == b.validIdx) releaseT0(it->second.pend);
        }
        mRbBuilds[s].clear();
        return;
    }
    mRbValid[s] = false;
    const uint8_t* rb = static_cast<const uint8_t*>(mReadback[s].mapped);
    std::memcpy(mLast, rb, 64 * 4);
    std::memcpy(mLast + 64, rb + kRbExtra, (kCntWords - 64) * 4);   // (#1008 counters)
    // (10 s window of the static counters: statsLine's per-frame / per-second figures)
    ++mAcc.frames;
    mAcc.built += mLast[kCntSbInfos];
    mAcc.kept += mLast[kCntSbKept];
    mAcc.old += mLast[kCntSbOld];
    mAcc.fail += mLast[kCntSbFail];
    mAcc.flips += mLast[kCntStaticFlip];
    mAcc.notReady += mLast[kCntNotReady];
    mAcc.clasBuilt += mLast[kCntClasArgs];
    // tier-0 builds of that frame: confirmed valid -> current, invalid -> dropped
    const uint32_t* valid = reinterpret_cast<const uint32_t*>(rb + 256);
    for (const T0Build& b : mRbBuilds[s])
    {
        auto it = mT0.find(b.ri);
        if (it == mT0.end() || it->second.pend.valid != b.validIdx) continue;
        T0Inst& t = it->second;
        const uint32_t local = b.validIdx - s * kMaxT0Builds;
        if (local < kMaxT0Builds && valid[local] != 0u)
        {
            releaseT0(t.cur);
            t.cur = t.pend;
            t.cur.valid = kNone;
            t.pend = T0Blas{};
        }
        else
        {
            ++mT0InvalidTotal;
            releaseT0(t.pend);
        }
    }
    mRbBuilds[s].clear();
    // tier 0: that frame's RT cuts
    mT0Cut.clear();
    if (mRbInst[s] && mRbCut[s])
    {
        const uint32_t* rows = reinterpret_cast<const uint32_t*>(rb + kRbHead);
        const uint32_t* cut = reinterpret_cast<const uint32_t*>(rb + kRbHead + (VkDeviceSize)mRbInst[s] * 32);
        const uint32_t used = std::min<uint32_t>(mLast[kCntCut], kCutCap);
        for (uint32_t i = 0; i < mRbInst[s]; ++i)
        {
            const uint32_t* r = rows + i * 8;
            const uint32_t ri = r[0], base = r[1], n = r[2];
            if (ri == kNone || n == kNone || n == 0 || n > kMaxCutPerInst || base + n > used) continue;
            if (r[3] & 0x80000000u)
            {
                // RIGGED (clinst w bit 31): the cut its per-cut BLAS is (re)built from
                T0Rig& g = mT0Rig[ri];
                if (g.mesh != r[6] || g.gen != r[7])
                {
                    releaseT0Rig(g);
                    g.mesh = r[6]; g.gen = r[7];
                }
                g.want = (uint64_t)r[4] | ((uint64_t)r[5] << 32);
                g.wantBase = (uint32_t)(mT0Cut.size() / 3);
                g.wantN = n;
                g.wantFrame = mFrame;
                for (uint32_t k = 0; k < n; ++k)
                {
                    const uint32_t* e = cut + (size_t)(base + k) * 4;
                    mT0Cut.push_back(e[0]); mT0Cut.push_back(e[1]); mT0Cut.push_back(e[2]);
                }
                continue;
            }
            T0Inst& t = mT0[ri];
            if (t.mesh != r[6] || t.gen != r[7])
            {   // another mesh behind the same instance id: its BLAS are someone else's
                releaseT0(t.cur); releaseT0(t.pend);
                t.mesh = r[6]; t.gen = r[7];
            }
            t.lastSeen = mFrame;
            t.want = (uint64_t)r[4] | ((uint64_t)r[5] << 32);
            t.wantBase = (uint32_t)(mT0Cut.size() / 3);
            t.wantN = n;
            for (uint32_t k = 0; k < n; ++k)
            {
                const uint32_t* e = cut + (size_t)(base + k) * 4;
                mT0Cut.push_back(e[0]); mT0Cut.push_back(e[1]); mT0Cut.push_back(e[2]);
            }
        }
    }
    mRbInst[s] = 0; mRbCut[s] = false;
}

// ============================================================================================
// plan
// ============================================================================================
VkDeviceSize LLCleanClusterRTVK::plan(const FrameIn& in, VkDeviceSize salign)
{
    const auto t0 = std::chrono::steady_clock::now();
    ++mFrame;
    mSlot = in.frameSlot % kFrameSlots;
    mActive = false;
    mTier = -1;
    mScratchFrame = 0;
    mT0Recs.clear(); mT0Geoms.clear(); mT0Ranges.clear();
    mT0JobCpu.clear(); mT0RowCpu.clear(); mT0HitCpu.clear(); mT0HitCopies.clear();
    mT0Jobs_n = mT0Rows_n = 0;
    mT0BuiltFrame = mT0TrisFrame = 0;
    mClusterSet = VK_NULL_HANDLE;
    mRigOn = false;
    mTlasRigOn = false;
    mT0RigRecs.clear(); mRigRowCpu.clear(); mRigJobCpu.clear(); mRigHdrCpu.clear(); mRigHdrCopies.clear();
    mT0RigRows = mT0RigJobs = mT0RigBuilt = mT0RigRefit = 0;
    // deferred pipeline / hit-cut frees
    if (mCtx)
    {
        size_t keep = 0;
        for (size_t i = 0; i < mOldPipes.size(); ++i)
        {
            if (mFrame >= mOldPipes[i].frame + kFrameSlots + 1)
            {
                if (mOldPipes[i].pipe) vkDestroyPipeline(mCtx->device(), mOldPipes[i].pipe, nullptr);
                if (mOldPipes[i].layout) vkDestroyPipelineLayout(mCtx->device(), mOldPipes[i].layout, nullptr);
            }
            else mOldPipes[keep++] = mOldPipes[i];
        }
        mOldPipes.resize(keep);
    }
    {
        size_t keep = 0;
        for (size_t i = 0; i < mHitFree.size(); ++i)
        {
            if (mFrame >= mHitFree[i].second + kFrameSlots + 1) mHitAlloc.release(mHitFree[i].first.start, mHitFree[i].first.count);
            else mHitFree[keep++] = mHitFree[i];
        }
        mHitFree.resize(keep);
        keep = 0;
        for (size_t i = 0; i < mRigFree.size(); ++i)
        {
            if (mFrame >= mRigFree[i].second + kFrameSlots + 1) mRigAlloc.release(mRigFree[i].first.start, mRigFree[i].first.count);
            else mRigFree[keep++] = mRigFree[i];
        }
        mRigFree.resize(keep);
    }
    harvest(mSlot);
    // TLAS pass B's rigged group counters of this slot's last frame (its fence was waited)
    if (mTlasRigValid[mSlot] && mTlasRig[mSlot].mapped)
    {
        const uint32_t* w = static_cast<const uint32_t*>(mTlasRig[mSlot].mapped);
        mTlasCovered = w[0];
        mTlasFallback = w[1];
        mTlasLegacyFlagged = w[2] & 0x7FFFFFFFu;
    }
    else { mTlasCovered = mTlasFallback = 0; mTlasLegacyFlagged = kNone; }   // (no map: the lift probe runs)
    mTlasRigValid[mSlot] = false;

    const Settings st = readSettings();
    mTol = st.tol; mHyst = st.hyst;
    auto inactive = [&](const char* why) -> VkDeviceSize {
        (void)why;
        // Nothing of ours was used for a while: give the memory back (free on liveness).
        if (++mIdleFrames == 120 && (asBytes() || mClasOfSlot.buffer))
        {
            for (auto& kv : mT0) { releaseT0(kv.second.cur); releaseT0(kv.second.pend); }
            mT0.clear();
            releaseClas();
            releaseStaticBlas();
            releaseRig();
            for (GpuBuffer* b : {&mState, &mInstRows, &mCut, &mCutPrev, &mOvr, &mDecode, &mT0Idx, &mHitCut, &mT0Valid, &mRigOf, &mSbMeta})
                retire(*b);
            mCutWritten = 0;
            mHitAlloc.reset(0);
            mStateFill = true;
        }
        LLVKHitch::add("vk_rtClusterPlan", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        return 0;
    };
    if (!mCtx || st.mode == 0 || !in.clusterOn || !in.cluster) return inactive("off");
    LLCleanClusterVK& cl = *in.cluster;
    if (!cl.ready() || cl.slot() != mSlot) return inactive("cluster frame not set up");
    // (integration with #749) the RT tolerance is 2c's "other" role: RenderVKClusterRTTolerancePx
    // as the controller reads and clamps it, never scaled by the LOD-camera lag.
    // STATIC CUT PERF PASS: a static RT cut is never FINER than the raster draws the same mesh
    // (RenderVKClusterRTStaticRasterTol x the controller's base tolerance x 2^level -- the
    // pressure LEVEL, not its eased value: the tolerance changes only when the level steps, so the
    // cut reuse / persistent BLAS are not churned by the easing). At PEAK the raster ran at 8 px
    // (x8 GPU pressure) while statics were traced at 2 px: ~4x the clusters, CLAS pages and trace
    // work of the surface actually drawn, and a traced caster up to the cut error off the
    // rasterized receiver (the self-shadow mismatch the rigged RT cut avoids by using the main
    // view's projection). Never coarser than the raster rule, never finer than
    // RenderVKClusterRTTolerancePx (0 = the fixed RT tolerance, the old behaviour).
    {
        const LLCleanClusterControlVK& ctl = cl.control();
        const float rt_tol = std::max(ctl.roleTolPx(CLROLE_OTHER), 0.05f);
        const float raster = st.rasterTol > 0.f ? st.rasterTol * ctl.basePx() * (float)(1u << std::min<uint32_t>(ctl.level(), 8u))
                                                : 0.f;
        mTol = std::max(rt_tol, raster);
    }
    mInstN = cl.select().instanceCount();
    if (mInstN == 0 || in.riCap == 0) return inactive("no cluster instances");
    // (record(): no static instance in the RT cut this frame -> none of the static passes)
    mStaticFrame = st.statics != 0 && cl.select().staticDrawn() != 0;
    const GpuBuffer& pool = cl.stream().pool();
    mPoolPages = cl.stream().poolPages();
    if (!pool.buffer || !pool.address || mPoolPages == 0) return inactive("no page pool");
    int32_t tier = cl.caps().get(CLCAP_RT);
    if (tier < 0) return inactive("RT tier none");
    if (mTierCap < 2 && mFrame >= mTierRetryFrame) { mTierCap = 2; mWhy.clear(); }
    if (mTierCap < 0) return inactive("tier failed");
    tier = std::min(tier, mTierCap);
    if (tier >= 1 && !mClasOk) tier = 0;
    if (tier >= 1 && (uint64_t)mPoolPages * kMaxClustersPerPage > (uint64_t)mMaxClusterGeomIndex) tier = 0;
    if (!ensurePipeline(cl)) return inactive("pipeline");
    mIdleFrames = 0;
    mPoolBuf = pool.buffer;
    mPoolAddr = pool.address;
    // GPU-side frame stamps (page usage for 1c's LRU, the selection stamp, CLAS idle ages) use
    // the cluster module's frame -- the one 1b stamps page usage with -- not this module's count.
    {
        // A re-created cluster module (RenderVKClusterLOD toggled) restarts its frame count: every
        // frame stamp held here (hysteresis rows, persistent static BLAS, CLAS build stamps) and
        // every CLAS / mesh index belongs to the old one -- drop them.
        const uint32_t gf = (uint32_t)cl.frame();
        if (mGpuFrame && gf + 1u < mGpuFrame)
        {
            releaseClas();
            mStateFill = true;
            mCutWritten = 0;
        }
        mGpuFrame = gf;
    }
    mRiCap = in.riCap;
    mCam[0] = in.cam[0]; mCam[1] = in.cam[1]; mCam[2] = in.cam[2]; mCam[3] = std::max(in.znear, 1e-3f);
    mPxScale = in.px_scale;
    // The cluster RT memory: its own share of the RT budget (LLCleanRTVK keeps it out of the
    // avatar-drop rule: the legacy classes get the rest), every arena of this module inside it.
    // (#748 static pass: a third of the RT budget up to 3 GB -- the 1 GB ceiling bound on large
    // cards once the RT budget scaled with VRAM; still a share, so small cards keep a small cap)
    mMemCap = st.memMB ? std::clamp<uint64_t>((uint64_t)st.memMB << 20, 64ull << 20, 3072ull << 20)
                       : std::clamp<uint64_t>(in.rtBudget / 3, 64ull << 20, 3072ull << 20);
    // RIGGED FIRST (#748 perf pass): the rigged arena + its per-frame CLAS are sized from the
    // read-back demand (planRigClas) and get what they need of the cap before the static CLAS
    // arena (the legacy-skin skip of a covered avatar saves its whole legacy pre-skin; a static
    // that does not fit just keeps its legacy BLAS). This is the reservation the static arena is
    // sized around: the demand's bytes (or the live arena's, while it has not shrunk yet: the
    // static arena must not follow every step of the demand -- re-creating it drops its CLAS),
    // at most 3/4 of the cap, 0 with rigged cluster RT off.
    // Never past what leaves the static arena its floor (kClasMinPages: below it planClas fails
    // the tier -- e.g. an 8 GB card's 128 MB cap with a big rigged demand); the rigged arena
    // then gets cut to its budget instead (mRigBudgetCut).
    {
        const uint64_t page = (mClasStride ? (uint64_t)mClasStride : 6272ull) * kMaxClustersPerPage;
        const uint64_t keep = mBlasArenaBytes + mPtlas.size + (uint64_t)kClasMinPages * page + mClasAlign;
        const uint64_t most = std::min<uint64_t>(mMemCap / 4 * 3, mMemCap > keep ? mMemCap - keep : 0);
        mRigReserve = st.rigged ? std::min<uint64_t>(rigBytesFor(std::max(rigSlotsWanted(), mRigCap)), most) : 0;
    }
    mStaticOn = st.statics != 0;

    // ---- common buffers ----
    const VkBufferUsageFlags kSt = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    const VkBufferUsageFlags kDst = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    const VkBufferUsageFlags kIn = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    bool ok = true;
    {
        const VkBuffer old = mState.buffer;
        ok = ok && ensureBuf(mState, (VkDeviceSize)mRiCap * kStateBytes, kSt | kDst, false, "rt.cl.state");
        if (mState.buffer != old) mStateFill = true;
        // (st zeroed: every persistent static BLAS forgets its block -- a new heap epoch)
        if (mStateFill) { ++mSbEpoch; if (!mSbEpoch) mSbEpoch = 1; mSbReset = true; }
        // (#1008: ... and every rigged slot record / cut-reuse record: nothing rigged is kept)
        if (mStateFill) mRigKeepReset = true;
    }
    ok = ok && ensureBuf(mStaticCpu[mSlot], (VkDeviceSize)kStaticCpuWords * 4, kSt, true, "rt.cl.staticCpu");
    ok = ok && ensureBuf(mInstRows, (VkDeviceSize)mInstN * 32, kSt | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false, "rt.cl.inst");
    ok = ok && ensureBuf(mCut, (VkDeviceSize)kCutCap * 16, kSt | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false, "rt.cl.cut");
    ok = ok && ensureBuf(mCutPrev, (VkDeviceSize)kCutCap * 16, kSt | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false, "rt.cl.cut");
    ok = ok && ensureBuf(mOvr, (VkDeviceSize)mRiCap * 16, kSt | kDst, false, "rt.cl.ovr");
    ok = ok && ensureBuf(mCnt, kCntWords * 4, kSt | kDst | kIn | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false, "rt.cl.cnt");
    ok = ok && ensureBuf(mT0Valid, (VkDeviceSize)kFrameSlots * kMaxT0Builds * 4,
                         kSt | kDst | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false, "rt.cl.t0valid");
    // (rigged; always real buffers: mode 0 writes rig_of, reads the parameter block)
    // (+ the per-avatar table of modes 0 / 13 / 14 after the per-instance rows: RP_AV_BASE, and
    // mode 13's "admitted last frame" words after it -- kept across frames, so the row region is
    // a high-water mark in 4096-row steps: the table must not move with every instance count)
    if (mInstN > mRigAvRows || !mRigOf.buffer) mRigAvRows = std::max<uint32_t>((mInstN + mInstN / 4 + 4095) / 4096 * 4096, mRigAvRows);
    mRigAvBase = mRigAvRows * 8;
    // (+ #748 perf pass 2: the per-avatar rows are kRigAvStride words, and the persistent
    // {base, want, signature} rows of the kept rigged CLAS follow the "admitted last frame" words)
    // (+ #1008: the per-avatar schedule words after the persistent rows)
    ok = ok && ensureBuf(mRigOf, ((VkDeviceSize)mRigAvBase + kRigAvRows * (kRigAvStride + 1 + kRigAvPersist + kRigAvSched)) * 4,
                         kSt | kDst, false, "rt.cl.rigOf");
    if (mRigOf.buffer != mRigOfSeen) { mRigOfSeen = mRigOf.buffer; mRigKeepReset = true; }
    ok = ok && ensureBuf(mRigCpu[mSlot], (VkDeviceSize)kRigParamWords * 4, kSt, true, "rt.cl.rigCpu");
    if (!ok) { tierFail("working buffers (VRAM)"); return inactive("alloc"); }
    if (mRigCpu[mSlot].mapped) std::memset(mRigCpu[mSlot].mapped, 0, (size_t)kRigParamWords * 4);

    // STATIC ADMISSION threshold (clean_cluster_rt.comp mode 0, RP_STATIC_MIN; CLAS tiers): the
    // static instances whose importance (world radius / distance) passes it take part. Every 8
    // frames, on the read-back demand (pool pages the admitted cuts used + wanted): raised while
    // it does not fit an arena that already has all its room (alloc fails), lowered (to 0: all)
    // while it fits with a quarter to spare -- so the instances that could not become ready
    // stay off the cluster path (legacy BLAS) instead of paying the selection every frame.
    if (!mStaticOn || tier < 1) mStaticMin = 0.f;
    else if ((mFrame & 7u) == 0u && mClasCap)
    {
        const uint32_t demand = mLast[kCntClasUsed] + mLast[kCntWant];
        // (full: the arena has all the room it may take -- chunks: within one chunk of it)
        const bool full = mClasCap + std::max(mClasCap / 8, kClasChunkPages) >= mClasRoomCap;
        if ((mLast[kCntClasFail] != 0 || demand > mClasCap) && full)
            raiseStaticMin();
        else if (mStaticMin > 0.f && demand * 4 < mClasCap * 3 && mLast[kCntClasFail] == 0)
        {
            mStaticMin *= 0.8f;
            if (mStaticMin < 1e-3f) mStaticMin = 0.f;
        }
    }
    if (tier >= 1)
    {
        // tier-0 BLAS of an earlier tier are not used any more
        if (!mT0.empty()) { for (auto& kv : mT0) { releaseT0(kv.second.cur); releaseT0(kv.second.pend); } mT0.clear(); }
        mTier = tier;
        if (!planClas(salign, mMemCap, mPoolPages))
        {
            tierFail(mWhy.empty() ? "CLAS buffers" : mWhy.c_str());
            tier = 0;
        }
    }
    if (tier == 0)
    {
        if (!mClasChunks.empty()) releaseClas();
        if (mSbArena.buffer) releaseStaticBlas();
        mTier = 0;
        planT0(salign, mMemCap);
        (void)st;
    }
    mTier = tier;
    // ---- rigged (RIGGED in the header): the switched avatars' RT cut + skinning + AS ----
    // Its budget: what the cap leaves after the static arenas (which were sized around
    // mRigReserve, the rigged demand). No consumer when LLCleanRTVK's rigged plan does not run
    // (pass B, the only reader of a rigged override row, walks its rig entries): skipped then.
    // That plan runs for the switched faces even while the legacy avatar class is dropped.
    {
        uint64_t other = mBlasArena.size + mPtlas.size + mSbArena.size;
        for (const GpuBuffer& c : mClasChunks) other += c.size;
        const uint64_t rig_budget = st.rigged ? (mMemCap > other ? mMemCap - other : 0) : 0;
        mLegacyRigged = in.legacyRigged;
        std::memcpy(mInStill, in.rigStill, sizeof(mInStill));
        mInStillValid = in.rigStillValid;
        std::memcpy(mInTraced, in.rigTraced, sizeof(mInTraced));
        std::memcpy(mInNoSelf, in.rigNoSelf, sizeof(mInNoSelf));
        planRig(cl, tier, salign, rig_budget);
    }
    // The static words of the parameter block (after planRig: it re-creates / clears the block).
    if (uint32_t* w = static_cast<uint32_t*>(mRigCpu[mSlot].mapped))
    {
        w[20] = mRigAvBase;                                       // RP_AV_BASE
        w[21] = mStaticOn ? 1u : 0u;                              // RP_STATIC
        std::memcpy(&w[22], &mStaticMin, 4);                      // RP_STATIC_MIN
    }
    writeStaticCpu();   // (binding 33: the chunk table + the heap parameters this frame planned)
    // readback for this slot (counters + validity; tier 0: rows + cut)
    {
        const VkDeviceSize need = kRbHead + (tier == 0 ? (VkDeviceSize)mInstN * 32 + (VkDeviceSize)kCutCap * 16 : 0);
        if (!ensureBuf(mReadback[mSlot], need, kDst, true, "rt.cl.readback"))
        {
            tierFail("readback (RAM)");
            unwindT0Rig();
            mRigOn = false;
            return inactive("alloc");
        }
    }
    mClusterSet = cl.set(mSlot);
    mActive = mClusterSet != VK_NULL_HANDLE;
    mScratchPeak = std::max(mScratchPeak, mScratchFrame);
    mPeakAs = std::max<uint64_t>(mPeakAs, asBytes());
    mPeakAux = std::max<uint64_t>(mPeakAux, auxBytes());
    {
        // (#1011 profile: the Cluster RT line's "cpu plan" avg / max over its log window)
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        LLVKHitch::add("vk_rtClusterPlan", ms);
        mPlanMsAcc += ms;
        mPlanMsMax = std::max(mPlanMsMax, ms);
        ++mPlanN;
    }
    return mActive ? mScratchFrame : 0;
}

// ---- tiers 1 / 2: sizes, the CLAS arena, the per-frame argument buffers ----
bool LLCleanClusterRTVK::planClas(VkDeviceSize salign, uint64_t mem_cap, uint32_t pool_pages)
{
#ifdef VK_NV_cluster_acceleration_structure
    const VkDevice dev = mCtx->device();
    auto getSizes = reinterpret_cast<PFN_vkGetClusterAccelerationStructureBuildSizesNV>(mGetClasSizes);
    const uint32_t argsMax = kClasPagesPerFrame * kMaxClustersPerPage;
    const uint32_t maxGeom = pool_pages * kMaxClustersPerPage - 1;
    if (mClasMaxGeom != maxGeom || !mClasStride)
    {
        VkClusterAccelerationStructureTriangleClusterInputNV tri{VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_TRIANGLE_CLUSTER_INPUT_NV};
        tri.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        tri.maxGeometryIndexValue = maxGeom;
        tri.maxClusterUniqueGeometryCount = 1;
        tri.maxClusterTriangleCount = kMaxClusterTris;
        tri.maxClusterVertexCount = kMaxClusterVerts;
        tri.maxTotalTriangleCount = argsMax * kMaxClusterTris;
        tri.maxTotalVertexCount = argsMax * kMaxClusterVerts;
        tri.minPositionTruncateBitCount = 0;
        VkClusterAccelerationStructureInputInfoNV ii{VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_INPUT_INFO_NV};
        ii.maxAccelerationStructureCount = argsMax;
        ii.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        ii.opType = VK_CLUSTER_ACCELERATION_STRUCTURE_OP_TYPE_BUILD_TRIANGLE_CLUSTER_NV;
        ii.opMode = VK_CLUSTER_ACCELERATION_STRUCTURE_OP_MODE_EXPLICIT_DESTINATIONS_NV;
        ii.opInput.pTriangleClusters = &tri;
        VkAccelerationStructureBuildSizesInfoKHR sz{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        getSizes(dev, &ii, &sz);   // explicit mode: the size of ONE CLAS of these maxima
        if (!sz.accelerationStructureSize) { mWhy = "CLAS size query returned 0"; return false; }
        if (mClasStride && alignUpD(sz.accelerationStructureSize, mClasAlign) != mClasStride)
            releaseClas();   // a different stride: every CLAS slot moves
        mClasStride = alignUpD(sz.accelerationStructureSize, mClasAlign);
        mClasScratch = sz.buildScratchSize;
        mClasMaxGeom = maxGeom;
    }
    // cluster BLAS: one per ClusterInstance row at most
    const uint32_t blasMax = std::max<uint32_t>(256u, nextPow2(mInstN));
    if (blasMax != mBlasMax)
    {
        VkClusterAccelerationStructureClustersBottomLevelInputNV bl{VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_CLUSTERS_BOTTOM_LEVEL_INPUT_NV};
        bl.maxTotalClusterCount = kCutCap;
        bl.maxClusterCountPerAccelerationStructure = kMaxCutPerInst;
        VkClusterAccelerationStructureInputInfoNV ii{VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_INPUT_INFO_NV};
        ii.maxAccelerationStructureCount = blasMax;
        ii.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        ii.opType = VK_CLUSTER_ACCELERATION_STRUCTURE_OP_TYPE_BUILD_CLUSTERS_BOTTOM_LEVEL_NV;
        ii.opMode = VK_CLUSTER_ACCELERATION_STRUCTURE_OP_MODE_IMPLICIT_DESTINATIONS_NV;
        ii.opInput.pClustersBottomLevel = &bl;
        VkAccelerationStructureBuildSizesInfoKHR sz{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        getSizes(dev, &ii, &sz);   // implicit mode: the total of all of them
        if (!sz.accelerationStructureSize) { mWhy = "cluster BLAS size query returned 0"; return false; }
        mBlasArenaBytes = alignUpD(sz.accelerationStructureSize, mClasBlasAlign) + mClasBlasAlign;
        mBlasScratch = sz.buildScratchSize;
        mBlasMax = blasMax;
    }
    // STATIC CLAS ARENA (#748 static pass): CLAS pages (64 CLAS each) in CHUNKS of kClasChunkPages,
    // within what the cap leaves after the BLAS arena, the PTLAS, the rigged reservation
    // (mRigReserve: RIGGED first) and the persistent static BLAS heap. SIZED FROM DEMAND: the pool
    // pages the admitted static cuts used + wanted (read back), all the room while the admission
    // threshold limits them (so it can come down), the minimum with RenderVKClusterRTStatic 0 / no
    // static on the cluster path. GROWTH ADDS CHUNKS (at most kClasChunksPerFrame per frame): no
    // CLAS is lost and no 500 MB allocation lands in one frame -- the re-created single arena of
    // the perf pass cost 6-12 ms of CPU (vk_rtClusterPlan) and every CLAS on each step.
    const uint64_t page_bytes = (uint64_t)mClasStride * kMaxClustersPerPage;
    // (the heap's share: its size, or what it wants -- twice the live static BLAS -- in 16 MB steps)
    const uint64_t sb_live2 = (uint64_t)mLast[kCntSbUnits] * mSbUnit * 2;
    const uint64_t sb_want = mStaticOn ? (std::max<uint64_t>({mSbArena.size, kSbMinBytes, sb_live2}) + (16ull << 20) - 1)
                                             / (16ull << 20) * (16ull << 20)
                                       : mSbArena.size;
    const uint64_t fixed = mBlasArenaBytes + mRigReserve + mPtlas.size + sb_want;
    const uint64_t room = mem_cap > fixed ? mem_cap - fixed : 0;
    uint32_t room_cap = (uint32_t)std::min<uint64_t>({(uint64_t)pool_pages, (uint64_t)kMaxClasChunks * kClasChunkPages,
                                                           room / std::max<uint64_t>(page_bytes, 1)});
    if (room_cap < kClasMinPages) { mWhy = "RenderVKClusterRTMemoryMB too small for the CLAS arena"; return false; }
    // STATIC CLAS BUDGET (static cut perf pass, RenderVKClusterRTStaticMB; 0 = a quarter of the
    // cluster RT cap, 64 .. 512 MB): the static arena's ceiling. Before it, the arena took ALL the
    // room the cap left whenever the admission threshold limited the statics -- 1078 MB at PEAK
    // (2357 live pages of 2848), five times the 200 MB of compacted legacy BLAS of every static
    // in range. The admission threshold (raiseStaticMin) keeps the admitted demand inside this
    // budget the same way it kept it inside the room: the most important statics first. Policy
    // only: the pool / chunk plumbing is unchanged (a lower ceiling than the live arena resets it
    // through the "room under the arena for ~2 s" rule below, once).
    {
        const Settings sst = readSettings();
        const uint64_t budget = sst.staticMB ? std::clamp<uint64_t>((uint64_t)sst.staticMB << 20, 16ull << 20, mem_cap)
                                             : std::clamp<uint64_t>(mem_cap / 4, 64ull << 20, 512ull << 20);
        const uint32_t budget_pages = (uint32_t)std::min<uint64_t>(budget / std::max<uint64_t>(page_bytes, 1), 0xFFFFFFFFull);
        mClasBudgetPages = std::max<uint32_t>(budget_pages, kClasMinPages);
        room_cap = std::min(room_cap, mClasBudgetPages);
    }
    mClasRoomCap = room_cap;
    const uint32_t demand = mLast[kCntClasUsed] + std::min(mLast[kCntWant], pool_pages);
    const bool statics_on_path = mStaticOn && mLast[kCntEligible] + mLast[kCntStaticSkip] != 0;
    uint32_t want = kClasMinPages;
    if (statics_on_path)
        want = (mStaticMin > 0.f || mLast[kCntClasFail] != 0) ? room_cap : demand + demand / 4 + kClasMinPages;
    const uint32_t target = std::clamp<uint32_t>(want, kClasMinPages, room_cap);
    const uint32_t want_chunks = (target + kClasChunkPages - 1) / kClasChunkPages;
    const uint32_t room_chunks = std::max<uint32_t>(1u, room_cap / kClasChunkPages);   // (never past the room)
    const uint32_t have = (uint32_t)mClasChunks.size();
    // SHRINK drops every CLAS (the free stack holds pages of every chunk): only when the statics
    // left the cluster path (~5 s under half), or when the room stayed under the arena for ~2 s
    // (the rigged reservation / the BLAS arena grew) -- not on every step of the room.
    bool reset = false;
    if (have && !statics_on_path && want_chunks * 2 <= have)
    {
        if (!mClasShrinkSince) mClasShrinkSince = std::max(mFrame, 1u);
        reset = mFrame - mClasShrinkSince > 300;
    }
    else mClasShrinkSince = 0;
    if (have > room_chunks)
    {
        if (!mClasOverSince) mClasOverSince = std::max(mFrame, 1u);
        reset = reset || mFrame - mClasOverSince > 120;
    }
    else mClasOverSince = 0;
    if (mClasOfSlot.buffer && pool_pages < mClasPoolPages) reset = true;   // (a smaller pool: slots renumbered)
    if (reset)
    {
        const VkDeviceSize stride = mClasStride;
        releaseClas();   // (clears the cached sizes: restored below, they are still valid)
        mClasStride = stride; mClasMaxGeom = maxGeom; mBlasMax = blasMax;
        mClasShrinkSince = mClasOverSince = 0;
    }
    const VkBufferUsageFlags kSt = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    const VkBufferUsageFlags kDst = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    const VkBufferUsageFlags kSrc = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    const VkBufferUsageFlags kIn = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    // grow: whole chunks, a few per frame (a failed allocation stops here, retried next frame)
    const uint32_t old_cap = mClasCap;
    const uint32_t grow_to = std::min(want_chunks, room_chunks);
    uint32_t added = 0;
    while ((uint32_t)mClasChunks.size() < grow_to && added < kClasChunksPerFrame)
    {
        GpuBuffer c;
        if (!ensureBuf(c, (VkDeviceSize)kClasChunkPages * page_bytes + mClasAlign, kSt, false, "rt.cl.clasChunk", true)) break;
        mClasChunks.push_back(c);
        ++added;
    }
    if (mClasChunks.empty()) { mWhy = "static CLAS arena (VRAM)"; return false; }
    mClasCap = (uint32_t)mClasChunks.size() * kClasChunkPages;
    // (mode 20 pushes the new chunks' pages onto the free stack -- also pages of a frame whose
    // record never ran; a reset frame's mode 8 does all)
    if (mClasReset) mClasGrowFrom = mClasCap;
    else if (added) mClasGrowFrom = std::min(mClasGrowFrom, old_cap);
    if (added && (uint32_t)mClasChunks.size() >= grow_to)
        LL_INFOS("Vulkan") << "Cluster RT: static CLAS arena " << mClasCap << " pages in " << mClasChunks.size() << " chunks ("
                           << ((uint64_t)mClasCap * page_bytes >> 20) << " MB; demand " << demand << ", room " << room_cap
                           << (reset ? ", re-created" : "") << ")" << LL_ENDL;
    // the per-pool-slot tables follow the pool: grown with their content (copied on the GPU in
    // record(), the old buffers alive through the retire ring), so a pool growth keeps the CLAS
    bool ok = true;
    if (!mClasReset && !mSlotCopyOf && mClasOfSlot.buffer && mClasStamp.buffer && pool_pages > mClasPoolPages
        && (mClasOfSlot.size < (VkDeviceSize)pool_pages * 16 || mClasStamp.size < (VkDeviceSize)pool_pages * 4))
    {
        GpuBuffer old_of = mClasOfSlot, old_st = mClasStamp;
        mClasOfSlot = GpuBuffer{}; mClasStamp = GpuBuffer{};
        ok = ensureBuf(mClasOfSlot, (VkDeviceSize)pool_pages * 16, kSt | kDst | kSrc, false, "rt.cl.clasOfSlot")
          && ensureBuf(mClasStamp, (VkDeviceSize)pool_pages * 4, kSt | kDst | kSrc, false, "rt.cl.clasStamp");
        if (ok)
        {
            mSlotCopyOf = old_of.buffer; mSlotCopyStamp = old_st.buffer; mSlotCopyPages = mClasPoolPages;
        }
        retire(old_of); retire(old_st);   // (freed when this frame slot comes round: after the copy)
        if (!ok) { mWhy = "CLAS slot tables (VRAM)"; return false; }
    }
    {
        const VkBuffer of0 = mClasOfSlot.buffer, st0 = mClasStamp.buffer, fr0 = mClasFree.buffer;
        ok = ok && ensureBuf(mClasOfSlot, (VkDeviceSize)pool_pages * 16, kSt | kDst | kSrc, false, "rt.cl.clasOfSlot");
        ok = ok && ensureBuf(mClasStamp, (VkDeviceSize)pool_pages * 4, kSt | kDst | kSrc, false, "rt.cl.clasStamp");
        // (the free stack at its largest size once: a re-created stack would lose the free pages)
        ok = ok && ensureBuf(mClasFree, ((VkDeviceSize)kMaxClasChunks * kClasChunkPages + 1) * 4, kSt | kSrc, false,
                             "rt.cl.clasFree");   // (+ SRC: the stats copy reads it)
        const bool lost = (of0 && of0 != mClasOfSlot.buffer && mClasOfSlot.buffer != VK_NULL_HANDLE && !mSlotCopyOf)
                       || (st0 && st0 != mClasStamp.buffer && !mSlotCopyStamp) || (fr0 && fr0 != mClasFree.buffer);
        if (lost && !mClasReset)
        {   // (a table re-created without its content: every CLAS goes -- the full reset)
            mClasReset = true;
            mClasGrowFrom = mClasCap;
            ++mSbEpoch; if (!mSbEpoch) mSbEpoch = 1;
            mSbReset = true;
        }
    }
    mClasPoolPages = pool_pages;
    ok = ok && ensureBuf(mClasWant, (VkDeviceSize)pool_pages * 4, kSt, false, "rt.cl.clasWant");
    ok = ok && ensureBuf(mClasArgs, (VkDeviceSize)argsMax * 64, kSt | kIn, false, "rt.cl.clasArgs");
    ok = ok && ensureBuf(mClasDst, (VkDeviceSize)argsMax * 8, kSt | kIn, false, "rt.cl.clasDst");
    ok = ok && ensureBuf(mClasSize, (VkDeviceSize)argsMax * 4, kSt | kIn, false, "rt.cl.clasSize");
    ok = ok && ensureBuf(mDecode, (VkDeviceSize)argsMax * kMaxClusterVerts * 12, kSt | kIn, false, "rt.cl.decode");
    ok = ok && ensureBuf(mClasRefs, (VkDeviceSize)kCutCap * 8, kSt | kIn, false, "rt.cl.clasRefs");
    // (regions: this frame's implicit build [0, blas_max), the static explicit build after it;
    // blas_addr: + the static build's sizes)
    ok = ok && ensureBuf(mBlasInfo, (VkDeviceSize)mBlasMax * 32 + 16, kSt | kIn | kDst, false, "rt.cl.blasInfo");
    ok = ok && ensureBuf(mBlasAddr, (VkDeviceSize)mBlasMax * 24, kSt | kIn, false, "rt.cl.blasAddr");
    ok = ok && ensureBuf(mBlasOf, (VkDeviceSize)mInstN * 4, kSt, false, "rt.cl.blasOf");
    ok = ok && ensureBuf(mBlasArena, mBlasArenaBytes, kSt, false, "rt.cl.blasArena", true);
    // the persistent static BLAS heap's bookkeeping + the CLAS build stamps (modes 1 / 2 / 4)
    {
        const VkBuffer m0 = mSbMeta.buffer;
        ok = ok && ensureBuf(mSbMeta, (VkDeviceSize)kSbMetaWords * 4, kSt | kDst | kSrc, false, "rt.cl.sbMeta");
        if (ok && mSbMeta.buffer != m0)
        {   // fresh: zeroed whole in record() (build stamps 0 = older than any BLAS), a new epoch
            mSbReset = true;
            mSbMetaFresh = true;
            ++mSbEpoch; if (!mSbEpoch) mSbEpoch = 1;
        }
    }
    if (!ok) { mWhy = "CLAS / cluster BLAS buffers (VRAM)"; return false; }
    // the counters word that is the CLAS build's srcInfosCount / BLAS build's live in mCnt
    // (the static CLAS build's scratch only while a static takes part: record() skips that build)
    if (mStaticFrame) mScratchFrame += alignUpD(mClasScratch + mClasScratchAlign, salign);
    mScratchFrame += alignUpD(mBlasScratch + mClasScratchAlign, salign);
    // PERSISTENT STATIC BLAS: what the cap leaves after the CLAS chunks
    {
        uint64_t chunks = 0;
        for (const GpuBuffer& c : mClasChunks) chunks += c.size;
        const uint64_t used = mBlasArenaBytes + mRigReserve + mPtlas.size + chunks;
        planStaticBlas(salign, mem_cap > used ? mem_cap - used : 0);
    }
    return true;
#else
    (void)salign; (void)mem_cap; (void)pool_pages;
    mWhy = "built without VK_NV_cluster_acceleration_structure headers";
    return false;
#endif
}

// ---- the PERSISTENT STATIC BLAS heap (header STATIC): size classes, scratch, capacity ----
void LLCleanClusterRTVK::planStaticBlas(VkDeviceSize salign, uint64_t budget)
{
    mSbOn = false;
#ifdef VK_NV_cluster_acceleration_structure
    const Settings st = readSettings();
    if (!st.persist || !mStaticOn || mSbUnsupported)
    {
        if (mSbArena.buffer) releaseStaticBlas();
        return;
    }
    if (mLast[kCntEligible] + mLast[kCntStaticSkip] == 0)
    {   // (no static on the cluster path for a while: give the heap back -- free on liveness)
        if (mSbArena.buffer && ++mSbIdle >= 120) { releaseStaticBlas(); mSbIdle = 0; }
        if (!mSbArena.buffer) return;
    }
    else mSbIdle = 0;
    if (!mSbArena.buffer && mFrame < mSbRetryFrame) return;   // (an allocation failed: retried later)
    const VkDevice dev = mCtx->device();
    auto getSizes = reinterpret_cast<PFN_vkGetClusterAccelerationStructureBuildSizesNV>(mGetClasSizes);
    auto query = [&](uint32_t count, uint32_t total, uint32_t per, VkAccelerationStructureBuildSizesInfoKHR& sz)
    {
        VkClusterAccelerationStructureClustersBottomLevelInputNV bl{VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_CLUSTERS_BOTTOM_LEVEL_INPUT_NV};
        bl.maxTotalClusterCount = total;
        bl.maxClusterCountPerAccelerationStructure = per;
        VkClusterAccelerationStructureInputInfoNV ii{VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_INPUT_INFO_NV};
        ii.maxAccelerationStructureCount = count;
        ii.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;   // (rarely rebuilt: trace speed)
        ii.opType = VK_CLUSTER_ACCELERATION_STRUCTURE_OP_TYPE_BUILD_CLUSTERS_BOTTOM_LEVEL_NV;
        ii.opMode = VK_CLUSTER_ACCELERATION_STRUCTURE_OP_MODE_EXPLICIT_DESTINATIONS_NV;
        ii.opInput.pClustersBottomLevel = &bl;
        sz = VkAccelerationStructureBuildSizesInfoKHR{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        getSizes(dev, &ii, &sz);   // explicit mode: the size of ONE structure of these maxima
    };
    if (!mSbSized)
    {
        // the unit: the bottom-level alignment (>= 256 B), a power of two
        VkDeviceSize unit = 256;
        while (unit < mClasBlasAlign) unit <<= 1;
        mSbUnit = unit;
        for (uint32_t c = 0; c < kSbClasses; ++c)
        {
            VkAccelerationStructureBuildSizesInfoKHR sz;
            query(1, kSbClassCounts[c], kSbClassCounts[c], sz);
            if (!sz.accelerationStructureSize)
            {
                mSbUnsupported = true;
                LL_WARNS("Vulkan") << "Cluster RT: explicit cluster BLAS size query returned 0 (" << kSbClassCounts[c]
                                   << " clusters); static cluster BLAS stay per-frame builds" << LL_ENDL;
                return;
            }
            mSbClassCount[c] = kSbClassCounts[c];
            mSbClassUnits[c] = (uint32_t)((sz.accelerationStructureSize + mSbUnit - 1) / mSbUnit);
        }
        mSbSized = true;
        LL_INFOS("Vulkan") << "Cluster RT: persistent static cluster BLAS: " << kSbClasses << " size classes, unit "
                           << mSbUnit << " B, " << mSbClassCount[0] << " cluster = " << mSbClassUnits[0] * mSbUnit << " B, "
                           << mSbClassCount[8] << " = " << mSbClassUnits[8] * mSbUnit << " B, " << mSbClassCount[kSbClasses - 1]
                           << " = " << (mSbClassUnits[kSbClasses - 1] * mSbUnit >> 10) << " KiB" << LL_ENDL;
    }
    if (mSbScratchMax != mBlasMax)
    {
        VkAccelerationStructureBuildSizesInfoKHR sz;
        query(mBlasMax, kCutCap, kMaxCutPerInst, sz);
        if (!sz.buildScratchSize && !sz.accelerationStructureSize) { mSbUnsupported = true; return; }
        mSbScratch = sz.buildScratchSize;
        mSbScratchMax = mBlasMax;
    }
    // CAPACITY from the read-back demand (the units the live static BLAS hold, x2), within the
    // budget: grown when a block could not be had while the live ones fill 2/3 of it; RE-PACKED in
    // place (a new epoch: every static BLAS rebuilt once) when blocks were missing while it is
    // mostly free (fragmentation, blocks of instances gone for good); shrunk after ~10 s at under a
    // quarter. Each of these is one frame of full static builds -- the per-frame cost before.
    const uint64_t live = (uint64_t)mLast[kCntSbUnits] * mSbUnit;
    const uint64_t cap_bytes = mSbArena.buffer ? (uint64_t)mSbUnits * mSbUnit : 0;
    // (before any read-back: seeded from the eligible statics at ~8 clusters each -- the log's
    // average cut is ~7.6 -- so the start does not grow 4 -> 8 -> 16 MB with a full rebuild each)
    const uint64_t seed = live ? 0 : (uint64_t)mLast[kCntEligible] * mSbClassUnits[5] * mSbUnit * 2;
    const uint64_t want = std::max<uint64_t>({kSbMinBytes, live * 2, seed});
    const bool trust = mSbCooldown == 0;
    if (mSbCooldown) --mSbCooldown;
    bool recreate = !mSbArena.buffer;
    bool repack = false;
    uint64_t bytes = want;
    if (trust && mSbArena.buffer)
    {
        if (mLast[kCntSbFail] != 0 || mLast[kCntSbLeak] != 0)
        {
            // (grown only when the budget lets it grow; a repack at most every ~2 s -- a heap the
            // budget holds too small leaves the rest on their previous / legacy BLAS instead)
            if (live * 3 / 2 > cap_bytes && budget > cap_bytes) { recreate = true; bytes = std::max(want, cap_bytes * 2); }
            else if (mFrame - mSbLastRepack > 120) repack = true;
            mSbSmallSince = 0;
        }
        else if (cap_bytes > kSbMinBytes && live * 4 < cap_bytes)
        {
            if (!mSbSmallSince) mSbSmallSince = std::max(mFrame, 1u);
            if (mFrame - mSbSmallSince > 600) { recreate = true; mSbSmallSince = 0; }
        }
        else mSbSmallSince = 0;
    }
    if (recreate)
    {
        bytes = std::min<uint64_t>(bytes, budget);
        const bool had = mSbArena.buffer != VK_NULL_HANDLE;
        retire(mSbArena);
        mSbUnits = 0;
        if (bytes < kSbMinBytes || !ensureBuf(mSbArena, bytes + mSbUnit, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false,
                                              "rt.cl.sbHeap", true))
        {
            // (no room / no memory: the statics take the implicit per-frame builds; retried later)
            mSbArena = GpuBuffer{};
            mSbRetryFrame = mFrame + 120;
            ++mSbEpoch; if (!mSbEpoch) mSbEpoch = 1;
            mSbReset = true;
            return;
        }
        mSbUnits = (uint32_t)std::min<uint64_t>(bytes / mSbUnit, 0x00FFFFFFu);
        ++mSbEpoch; if (!mSbEpoch) mSbEpoch = 1;
        mSbReset = true;
        mSbCooldown = kFrameSlots + 2;
        if (had) ++mSbGrows;
        LL_INFOS("Vulkan") << "Cluster RT: persistent static BLAS heap " << (bytes >> 20) << " MB (live "
                           << (live >> 10) << " KiB" << (had ? ", re-created" : "") << ")" << LL_ENDL;
    }
    else if (repack)
    {
        ++mSbEpoch; if (!mSbEpoch) mSbEpoch = 1;
        mSbReset = true;
        mSbCooldown = kFrameSlots + 2;
        mSbLastRepack = mFrame;
        ++mSbRepacks;
    }
    mSbOn = true;
    if (mStaticFrame) mScratchFrame += alignUpD(mSbScratch + mClasScratchAlign, salign);   // (record: built only then)
#else
    (void)salign; (void)budget;
#endif
}

// Binding 33 of this frame slot: the chunk table and the heap parameters (SC_*).
void LLCleanClusterRTVK::writeStaticCpu()
{
    uint32_t* w = static_cast<uint32_t*>(mStaticCpu[mSlot].mapped);
    if (!w) return;
    std::memset(w, 0, (size_t)kStaticCpuWords * 4);
    w[kScChunkPages] = kClasChunkPages;
    w[kScChunks] = (uint32_t)mClasChunks.size();
    w[kScSbOn] = mSbOn && mSbArena.buffer && mSbUnits ? 1u : 0u;
    w[kScSbEpoch] = mSbEpoch;
    const VkDeviceAddress heap = mSbArena.buffer ? alignUpD(mSbArena.address, mSbUnit) : 0;
    w[kScSbAddr] = lo32(heap); w[kScSbAddr + 1] = hi32(heap);
    w[kScSbUnits] = mSbUnits;
    w[kScSbUnit] = (uint32_t)mSbUnit;
    w[kScSbClasses] = kSbClasses;
    w[kScStackCap] = kSbStackCap;
    w[kScPendCap] = kSbPendCap;
    w[kScOffBuilt] = kSbOffBuilt;
    w[kScOffStacks] = kSbOffStacks;
    for (uint32_t c = 0; c < kSbClasses; ++c)
    {
        w[kScClassUnits + c] = mSbClassUnits[c];
        w[kScClassCount + c] = mSbClassCount[c];
    }
    for (uint32_t i = 0; i < (uint32_t)mClasChunks.size() && i < kMaxClasChunks; ++i)
    {
        const VkDeviceAddress a = alignUpD(mClasChunks[i].address, mClasAlign);
        w[kScChunkBase + i * 2] = lo32(a);
        w[kScChunkBase + i * 2 + 1] = hi32(a);
    }
}

// STATIC ADMISSION, raise (the arena has all its room and the admitted demand does not fit): the
// read-back histogram (CLAS pages used + wanted per importance bucket, 1.5 buckets per octave
// from 2^-14) -> the lowest bucket edge whose buckets above it fit 4/5 of the arena, in ONE step
// (the x1.25-per-8-frames hunt took ~2 s, flipping instances on the way). Pages shared by
// instances count once, at the first one's bucket: dropping a bucket may free less than it
// counted, so the next check may raise again. No histogram: the old step.
void LLCleanClusterRTVK::raiseStaticMin()
{
    const uint32_t* h = &mLast[kCntImpHist];
    uint64_t total = 0;
    for (uint32_t b = 0; b < kImpBuckets; ++b) total += h[b];
    const float step = mStaticMin > 0.f ? std::min(mStaticMin * 1.25f, 4.f) : 1e-3f;
    if (!total) { mStaticMin = step; return; }
    const uint64_t target = (uint64_t)mClasCap * 4 / 5;
    uint64_t acc = 0;
    uint32_t keep = kImpBuckets;   // the lowest bucket kept
    for (int b = (int)kImpBuckets - 1; b >= 0; --b)
    {
        if (acc + h[b] > target) break;
        acc += h[b];
        keep = (uint32_t)b;
    }
    const float edge = std::exp2(((float)keep - 21.f) / 1.5f);   // (= clean_cluster_rt.comp impBucket)
    mStaticMin = std::min(std::max(edge, mStaticMin > 0.f ? mStaticMin * 1.05f : 1e-3f), 4.f);
}

// ---- tier 0: per-cut KHR BLAS builds from the harvested cuts ----
void LLCleanClusterRTVK::planT0(VkDeviceSize salign, uint64_t mem_cap)
{
    const Settings st = readSettings();
    const VkDevice dev = mCtx->device();
    const auto& rt = mCtx->rt();
    if (mHitAlloc.cap == 0) mHitAlloc.reset(kHitCutCap);

    // ---- retire instances unseen for a while ----
    for (auto it = mT0.begin(); it != mT0.end();)
    {
        if (it->second.lastSeen + kT0KeepFrames < mFrame)
        {
            releaseT0(it->second.cur); releaseT0(it->second.pend);
            it = mT0.erase(it);
        }
        else ++it;
    }
    // ---- candidates: seen in this harvest, cut changed (or no BLAS), nothing pending ----
    struct Cand { uint32_t ri; bool has; uint32_t tris; };
    std::vector<Cand> cands;
    for (auto& kv : mT0)
    {
        const T0Inst& t = kv.second;
        if (t.lastSeen != mFrame || !t.wantN || t.pend.as) continue;
        if (t.cur.as && t.cur.hash == t.want) continue;
        uint32_t tris = 0;
        for (uint32_t k = 0; k < t.wantN; ++k) tris += mT0Cut[(size_t)(t.wantBase + k) * 3 + 2];
        cands.push_back({kv.first, t.cur.as != VK_NULL_HANDLE, tris});
    }
    // instances with no cluster BLAS first (they trace the legacy one), then the smallest
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
        return a.has != b.has ? !a.has : a.tris < b.tris;
    });
    // ---- choose builds (offsets first: the buffers are sized from the totals) ----
    struct Pick { uint32_t ri; uint32_t v0, i0; };   // first job vertex / index (uint16 units)
    std::vector<Pick> picks;
    uint32_t verts = 0, idx = 0, tris_total = 0;
    VkDeviceSize scratch = 0;
    uint64_t bytes = mT0Bytes;
    for (const Cand& c : cands)
    {
        if (picks.size() >= st.builds) break;
        if (!picks.empty() && tris_total + c.tris > kT0TrisPerFrame) break;
        const T0Inst& t = mT0[c.ri];
        // sizes
        std::vector<VkAccelerationStructureGeometryKHR> g(t.wantN);
        std::vector<uint32_t> prim(t.wantN);
        for (uint32_t k = 0; k < t.wantN; ++k)
        {
            g[k] = VkAccelerationStructureGeometryKHR{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
            g[k].geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
            auto& tr = g[k].geometry.triangles;
            tr = VkAccelerationStructureGeometryTrianglesDataKHR{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
            tr.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
            tr.vertexStride = 12;
            tr.maxVertex = kMaxClusterVerts - 1;
            tr.indexType = VK_INDEX_TYPE_UINT16;
            prim[k] = mT0Cut[(size_t)(t.wantBase + k) * 3 + 2];
        }
        VkAccelerationStructureBuildGeometryInfoKHR gi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        gi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        gi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        gi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        gi.geometryCount = t.wantN; gi.pGeometries = g.data();
        VkAccelerationStructureBuildSizesInfoKHR sz{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        rt.vkGetAccelerationStructureBuildSizesKHR(dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &gi, prim.data(), &sz);
        if (!sz.accelerationStructureSize) continue;
        if (!picks.empty() && scratch + sz.buildScratchSize > kT0ScratchBudget) break;
        if (bytes + sz.accelerationStructureSize > mem_cap) break;
        uint32_t hit = 0;
        if (!mHitAlloc.alloc(t.wantN, hit)) break;
        LLCleanRTVK::Alloc a;
        if (!mRT.arenaAlloc(sz.accelerationStructureSize, a)) { mHitAlloc.release(hit, t.wantN); tierFail("per-cut BLAS allocation"); break; }
        VkDeviceAddress addr = 0;
        VkAccelerationStructureKHR as = mRT.createAS(a, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, addr, "blas-cluster", c.ri);
        if (!as) { mRT.arenaFree(a); mHitAlloc.release(hit, t.wantN); break; }
        T0Inst& tw = mT0[c.ri];
        T0Blas& p = tw.pend;
        p.as = as; p.chunk = a.chunk; p.off = a.off; p.size = a.size; p.addr = addr;
        p.cut = Range{hit, t.wantN};
        p.hash = t.want;
        p.valid = mSlot * kMaxT0Builds + (uint32_t)picks.size();
        p.frame = mFrame;
        mT0Bytes += a.size;
        bytes += a.size;
        scratch += alignUpD(sz.buildScratchSize, salign);
        tris_total += c.tris;
        mT0Recs.push_back({c.ri, (uint32_t)mT0Geoms.size(), t.wantN, sz.buildScratchSize});
        mRbBuilds[mSlot].push_back({c.ri, p.valid});
        picks.push_back({c.ri, verts, idx});
        for (uint32_t k = 0; k < t.wantN; ++k)
        {
            const uint32_t* e = &mT0Cut[(size_t)(t.wantBase + k) * 3];
            const uint32_t tr = e[2];
            // job: {ref, vpage, vertex base, index base}, {validity word, tris, 0, 0}
            mT0JobCpu.insert(mT0JobCpu.end(), {e[0], e[1], verts, idx, p.valid, tr, 0u, 0u});
            mT0HitCpu.insert(mT0HitCpu.end(), {e[0], e[1], 0u, 0u});
            mT0Geoms.push_back(g[k]);
            VkAccelerationStructureBuildRangeInfoKHR r{};
            r.primitiveCount = tr;
            mT0Ranges.push_back(r);
            // (addresses filled once the buffers are sized; the offsets ride in firstVertex /
            // primitiveOffset scratch fields below)
            mT0Ranges.back().firstVertex = verts;
            mT0Ranges.back().primitiveOffset = idx;
            verts += kMaxClusterVerts;
            idx += (uint32_t)alignUpD(tr * 3, 2);
        }
        mT0HitCopies.push_back({(VkDeviceSize)(mT0HitCpu.size() / 4 - t.wantN) * 16, (VkDeviceSize)hit * 16,
                                (VkDeviceSize)t.wantN * 16});
    }
    mT0BuiltFrame = (uint32_t)picks.size();
    mT0TrisFrame = tris_total;
    mT0Jobs_n = (uint32_t)(mT0JobCpu.size() / 8);
    mScratchFrame += scratch;

    // ---- buffers ----
    const VkBufferUsageFlags kSt = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    const VkBufferUsageFlags kIn = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    bool ok = ensureBuf(mHitCut, (VkDeviceSize)kHitCutCap * 16, kSt | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false, "rt.cl.hitCut");
    if (mT0Jobs_n)
    {
        ok = ok && ensureBuf(mDecode, (VkDeviceSize)verts * 12, kSt | kIn, false, "rt.cl.decode");
        ok = ok && ensureBuf(mT0Idx, (VkDeviceSize)idx * 2 + 4, kSt | kIn, false, "rt.cl.t0idx");
        ok = ok && ensureBuf(mT0Jobs[mSlot], (VkDeviceSize)mT0JobCpu.size() * 4, kSt, true, "rt.cl.t0jobs");
        ok = ok && ensureBuf(mT0Stage[mSlot], (VkDeviceSize)mT0HitCpu.size() * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, "rt.cl.t0stage");
    }
    if (!ok)
    {
        // undo this frame's builds (their BLAS were never recorded)
        for (const T0Rec& r : mT0Recs)
        {
            auto it = mT0.find(r.ri);
            if (it != mT0.end()) releaseT0(it->second.pend);
        }
        mRbBuilds[mSlot].clear();
        mT0Recs.clear(); mT0Geoms.clear(); mT0Ranges.clear(); mT0JobCpu.clear(); mT0HitCpu.clear(); mT0HitCopies.clear();
        mT0Jobs_n = 0; mT0BuiltFrame = 0; mT0TrisFrame = 0;
        mScratchFrame -= std::min(mScratchFrame, scratch);
        tierFail("per-cut BLAS buffers (VRAM)");
    }
    else if (mT0Jobs_n)
    {
        std::memcpy(mT0Jobs[mSlot].mapped, mT0JobCpu.data(), mT0JobCpu.size() * 4);
        std::memcpy(mT0Stage[mSlot].mapped, mT0HitCpu.data(), mT0HitCpu.size() * 4);
        for (size_t i = 0; i < mT0Geoms.size(); ++i)
        {
            auto& tr = mT0Geoms[i].geometry.triangles;
            tr.vertexData.deviceAddress = mDecode.address + (VkDeviceAddress)mT0Ranges[i].firstVertex * 12;
            tr.indexData.deviceAddress = mT0Idx.address + (VkDeviceAddress)mT0Ranges[i].primitiveOffset * 2;
            mT0Ranges[i].firstVertex = 0;
            mT0Ranges[i].primitiveOffset = 0;
        }
    }

    // ---- override rows: every instance with a current or pending BLAS ----
    for (const auto& kv : mT0)
    {
        const T0Inst& t = kv.second;
        if (!t.cur.as && !t.pend.as) continue;
        const T0Blas& n = t.pend.as ? t.pend : t.cur;
        const T0Blas& o = t.pend.as ? t.cur : T0Blas{};
        const uint32_t row[16] = {
            kv.first, t.mesh, t.gen, 0u,
            lo32(n.addr), hi32(n.addr), n.cut.start, n.cut.count,
            n.valid, lo32(o.addr), hi32(o.addr), o.cut.start,
            o.cut.count, 0u, 0u, 0u};
        mT0RowCpu.insert(mT0RowCpu.end(), row, row + 16);
    }
    mT0Rows_n = (uint32_t)(mT0RowCpu.size() / 16);
    if (mT0Rows_n && ensureBuf(mT0Rows[mSlot], (VkDeviceSize)mT0RowCpu.size() * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                               true, "rt.cl.t0rows"))
        std::memcpy(mT0Rows[mSlot].mapped, mT0RowCpu.data(), mT0RowCpu.size() * 4);
    else mT0Rows_n = 0;
}

// ============================================================================================
// rigged (RIGGED in the header): the switched avatars' RT cut, its skinned arena and AS
// ============================================================================================
bool LLCleanClusterRTVK::riggedWanted() const
{
    const Settings st = readSettings();
    return mCtx && st.mode != 0 && st.rigged != 0;
}

void LLCleanClusterRTVK::releaseT0Rig(T0Rig& r)
{
    if (r.as || r.chunk >= 0)
    {
        LLCleanRTVK::Alloc a;
        a.chunk = r.chunk; a.off = r.off; a.size = r.size;
        mRT.retireAS(r.as, a);
        mT0RigBytes -= std::min<uint64_t>(mT0RigBytes, r.size);
    }
    if (r.slots.count) mRigFree.push_back({r.slots, mFrame});
    // (the harvested want, mesh and generation stay: they describe the instance, not the BLAS)
    r.as = VK_NULL_HANDLE; r.chunk = -1; r.off = 0; r.size = 0; r.addr = 0;
    r.slots = Range{}; r.hash = 0; r.builtFrame = 0; r.updScratch = 0;
    r.geoms.clear(); r.ranges.clear(); r.hdr.clear();
}

void LLCleanClusterRTVK::unwindT0Rig()
{
    // BLAS planned as a BUILD this frame were never built: a later refit of one would read an
    // unbuilt structure -- drop them (rebuilt from the next harvest). Skipped refits are harmless.
    for (const T0RigRec& r : mT0RigRecs)
    {
        if (!r.build) continue;
        auto it = mT0Rig.find(r.ri);
        if (it != mT0Rig.end()) releaseT0Rig(it->second);
    }
    mT0RigRecs.clear(); mRigRowCpu.clear(); mRigJobCpu.clear(); mRigHdrCpu.clear(); mRigHdrCopies.clear();
    mT0RigRows = mT0RigJobs = mT0RigBuilt = mT0RigRefit = 0;
}

void LLCleanClusterRTVK::releaseRig()
{
    for (auto& kv : mT0Rig) releaseT0Rig(kv.second);
    mT0Rig.clear();
    for (GpuBuffer* b : {&mRig, &mRigArgs, &mRigAddr, &mRigClasArena}) retire(*b);
    for (uint32_t s = 0; s < kFrameSlots; ++s) retire(mRigStage[s]);
    mRigCap = 0; mRigVtxBase = mRigIdxBase = 0; mRigTier0 = false;
    mRigAlloc.reset(0);
    mRigFree.clear();
    mRigClasCapQueried = 0; mRigClasBytes = mRigClasScratch = 0; mRigClasStride = 0;
    mRigKeepReset = true;
    mRigShrinkSince = 0;
    mT0RigRecs.clear(); mRigRowCpu.clear(); mRigJobCpu.clear(); mRigHdrCpu.clear(); mRigHdrCopies.clear();
    mT0RigRows = mT0RigJobs = 0;
    mRigOn = false;
}

bool LLCleanClusterRTVK::ensureRigArena(uint32_t slots, bool tier0)
{
    if (mRig.buffer && mRigCap == slots && mRigTier0 == tier0) return true;
    mRigKeepReset = true;   // (the kept slots' vertices / headers go)
    // A new layout: the tier-0 BLAS reference the old arena by address -- they go (rebuilt).
    for (auto& kv : mT0Rig) releaseT0Rig(kv.second);
    mRigFree.clear();
    retire(mRig);
    mRigCap = 0; mRigVtxBase = mRigIdxBase = 0;
    const uint64_t vtx = kRigHdrBase + (uint64_t)slots * 4;
    const uint64_t idx = vtx + (uint64_t)slots * kRigVtxWords;
    const uint64_t words = idx + (tier0 ? (uint64_t)slots * kRigIdxWords : 0);
    const VkBufferUsageFlags use = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
                                 | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    if (words > 0xFFFFFFFFull || !ensureBuf(mRig, words * 4, use, false, "rt.cl.rig"))
    {
        mRigAlloc.reset(0);
        mRigTier0 = false;
        return false;
    }
    mRigCap = slots;
    mRigVtxBase = (uint32_t)vtx;
    mRigIdxBase = tier0 ? (uint32_t)idx : 0u;
    mRigTier0 = tier0;
    mRigAlloc.reset(tier0 ? slots : 0u);
    return true;
}

void LLCleanClusterRTVK::planRig(LLCleanClusterVK& cl, int32_t tier, VkDeviceSize salign, uint64_t rig_budget)
{
    mRigOn = false;
    mRigCadence = false;
    mRigCadenceIdle = false;
    mRigRank = mRigReuse = false;
    const LLClusterSkinIn& si = cl.skin().frameInputs();
    const char* why = nullptr;
    if (!readSettings().rigged) why = "off (RenderVKClusterRTRigged 0)";
    else if (cl.skin().avatarsOnCluster() == 0) why = "no avatar on the cluster path";
    // (2026-10-13: the rigged plan of LLCleanRTVK runs for the switched faces alone while the
    // legacy avatar class is dropped over budget -- pass B then still reads the override rows --
    // so this is only "no rigged frame / pre-skin / buffers" now, never the RT budget.)
    else if (!mLegacyRigged) why = "no rigged RT plan this frame (no rigged pre-skin / RT rig buffers): no pass B consumer";
    else if (!rig_budget) why = "no cluster RT memory left (RenderVKClusterRTMemoryMB)";
    else if (cl.mainView() == kNone) why = "no main-view selection this frame";
    else if (!si.palette || !si.palette->buffer || si.skin_count == 0) why = "no skin palettes";
    else if (mFrame < mRigRetryFrame) why = "";   // (a failed allocation: mRigWhy keeps its reason)
    if (why)
    {
        if (*why) mRigWhy = why;
        // Nothing switched for a while: give the memory back (free on liveness).
        if (++mRigIdleFrames == 120 && (mRig.buffer || !mT0Rig.empty())) releaseRig();
        return;
    }
    mRigIdleFrames = 0;
    mRigMainView = cl.mainView();
    mRigBounds = cl.skin().boundsValid();
    mRigSkinCount = si.skin_count;
    mPalBuf = si.palette->buffer;
    mRigEye[0] = si.eye[0]; mRigEye[1] = si.eye[1]; mRigEye[2] = si.eye[2];
    // PERSISTENT RIGGED CLAS: the RT origin (positions are relative to it, not to the per-frame
    // eye: a camera move alone never invalidates a kept avatar). Moved -- everything rebuilds --
    // only when the eye gets kRigOriginMove away (precision: positions stay within that of the
    // eye-relative ones), on a teleport / region crossing (a jump) and on the first use.
    {
        const float dx = mRigEye[0] - mRigOrigin[0], dy = mRigEye[1] - mRigOrigin[1], dz = mRigEye[2] - mRigOrigin[2];
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (!mRigOriginOk || !(d2 <= kRigOriginMove * kRigOriginMove))
        {
            std::memcpy(mRigOrigin, mRigEye, sizeof(mRigOrigin));
            mRigOriginOk = true;
            mRigKeepReset = true;
        }
    }
    // CLAS tiers: the main view's SHARED SKINNED CUT (#702 integration: every switched avatar a
    // secondary pass draws is selected whole, un-culled at the main LOD, and skinned + welded once
    // by 2a's pre-skin THIS frame) -- an instance whose whole RT cut is in it takes those
    // vertices (modes 11 / 12) instead of being skinned again (mode 9 skins the rest).
    {
        const GpuBuffer& so = cl.skin().skinOut();
        const LLCleanClusterSelectVK::ViewSlice vs = cl.select().viewSlice(mRigMainView);
        mRigShared = tier >= 1 && cl.skin().preskinRecorded(cl.frame()) && so.buffer && so.address && vs.sel_cap;
        mSkinOutBuf = mRigShared ? so.buffer : VK_NULL_HANDLE;
        mSkinOutAddr = mRigShared ? so.address : 0;
        mRigSelCap = mRigShared ? vs.sel_cap : 0u;
        // FIXED-COST AUDIT (2026-10-14): modes 11 / 12 run one lane per SELECTED entry of the main
        // slice, dispatched indirectly from the main view's finalize args (CLC_DISPATCH: the
        // selected count / 64 in rows of kGridX, = recordPickFilter's) -- not over the slice
        // CAPACITY (the main view takes what the other roles leave of the arena: up to the whole
        // arena, 32k .. 2M entries, every RT frame, whatever was selected).
        mSelCnt = mRigShared ? cl.registeredBuffer(CLB_COUNTERS) : VK_NULL_HANDLE;
        mSelDispatchOff = ((VkDeviceSize)mRigMainView * LLCleanClusterSelectVK::kViewWords
                           + LLCleanClusterSelectVK::kCntDispatch) * 4;
    }
    const bool ok = tier >= 1 ? planRigClas(salign, rig_budget) : (planT0Rig(cl, salign, rig_budget), mRig.buffer != VK_NULL_HANDLE);
    if (!ok)
    {
        unwindT0Rig();
        if (mRigLogged != mRigWhy)
            LL_WARNS("Vulkan") << "Cluster RT: rigged cluster RT unavailable (" << mRigWhy << "); switched avatars keep "
                               << "their legacy rigged BLAS, retried in " << kTierRetryFrames << " frames" << LL_ENDL;
        mRigLogged = mRigWhy;
        mRigRetryFrame = mFrame + kTierRetryFrames;
        return;
    }
    // The parameter block (+ the tier-0 rows and jobs) of this frame slot.
    const size_t words = kRigParamWords + mRigRowCpu.size() + mRigJobCpu.size();
    if (!ensureBuf(mRigCpu[mSlot], (VkDeviceSize)words * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, "rt.cl.rigCpu")
        || (!mRigHdrCpu.empty()
            && !ensureBuf(mRigStage[mSlot], (VkDeviceSize)mRigHdrCpu.size() * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true,
                          "rt.cl.rigStage")))
    {
        unwindT0Rig();
        mRigWhy = "parameter / staging buffers (RAM)";
        if (mRigCpu[mSlot].mapped) std::memset(mRigCpu[mSlot].mapped, 0, (size_t)std::min<VkDeviceSize>(mRigCpu[mSlot].size, kRigParamWords * 4));
        return;
    }
    uint32_t* w = static_cast<uint32_t*>(mRigCpu[mSlot].mapped);
    std::memset(w, 0, (size_t)kRigParamWords * 4);
    const VkDeviceAddress vaddr = mRig.address + (VkDeviceAddress)mRigVtxBase * 4;
    w[0] = 1u;                                   // RP_RIG_ON
    w[1] = mRigMainView;                         // RP_MAIN_VIEW
    w[2] = mRigBounds ? 1u : 0u;                 // RP_RIG_BOUNDS
    w[3] = mRigCap;                              // RP_RIG_CAP
    w[4] = mRigVtxBase;                          // RP_VTX_BASE
    w[5] = mRigIdxBase;                          // RP_IDX_BASE
    w[6] = lo32(vaddr); w[7] = hi32(vaddr);      // RP_VTX_ADDR
    w[8] = mRigSkinCount;                        // RP_SKIN_COUNT
    w[9] = mT0RigRows;                           // RP_T0_ROWS
    w[10] = mT0RigJobs;                          // RP_T0_JOBS
    w[11] = mRigShared ? 1u : 0u;                // RP_SHARED
    w[18] = lo32(mSkinOutAddr); w[19] = hi32(mSkinOutAddr);   // RP_SKIN_ADDR
    std::memcpy(w + 12, mRigEye, sizeof(mRigEye));   // RP_EYE
    // PERSISTENT RIGGED CLAS (#748 perf pass 2): keep only with the still-proof of this frame, the
    // previous plan's rigged CLAS path recorded (no gap), and nothing re-created / moved since.
    const bool prev_rec = mRigRecordedLast;
    mRigRecordedLast = false;   // (set again when this frame's build is recorded)
    if (tier < 1) mRigKeepReset = true;
    mRigKeep = tier >= 1 && mInStillValid && prev_rec && !mRigKeepReset;
    // #1008 RIGGED RT CADENCE (clean_cluster_rt.comp header): bit 1 = an avatar may be kept because
    // it is not due / refreshed in place -- the same continuity as the still-keep (its slots, CLAS
    // and records are from the frame before, nothing re-created, the origin did not move), but no
    // still-proof. No continuity: every avatar is due (RP_SCHED bit 1: a teleport / region
    // crossing moves the origin, an arena or the state was re-created, a frame was skipped).
    const Settings st = readSettings();
    // (#1008 fix) only avatars that never self-shadow in RT (FrameIn::rigNoSelf) may go coarse or
    // stale; with none of them traced this frame the cadence is OFF for the frame -- the exact
    // path at exactly its cost (no band / schedule work in modes 0 / 13).
    bool any_noself = false;
    for (uint32_t i = 0; i < kAvatarSlotWords; ++i) any_noself = any_noself || (mInNoSelf[i] & mInTraced[i]) != 0u;
    mRigCadence = tier >= 1 && st.rigCadence && any_noself;
    mRigCadenceIdle = tier >= 1 && st.rigCadence && !any_noself;
    const bool cont = tier >= 1 && prev_rec && !mRigKeepReset;
    // #1011 (clean_cluster_rt.comp header): the RANKED SHARED CUT when 1b's main selection writes its
    // summary + ranks this frame (CL_B_INST_CUT); CLAS REUSE on top of it (RP_KEEP bit 2: the same
    // continuity as the keep -- slots, CLAS and the origin are the previous frame's)
    mRigRank = tier >= 1 && st.rigRank && mRigShared && cl.select().rtSummaryOn();
    mRigReuse = mRigRank && st.rigReuse;
    w[23] = (mRigKeep ? 1u : 0u) | (mRigCadence && cont ? 2u : 0u) | (mRigReuse && cont ? 4u : 0u);   // RP_KEEP
    std::memcpy(w + 24, mRigOrigin, sizeof(mRigOrigin));   // RP_ORIGIN
    if (++mRigStamp == 0 || mRigStamp > 0x7FFFFFFFu) mRigStamp = 1;   // (31 bits: slot stamp bit 31 = REFRESH)
    w[27] = mRigStamp;                           // RP_STAMP
    w[32] = (mRigCadence ? 1u : 0u) | (mRigCadence && !cont ? 2u : 0u);   // RP_SCHED
    for (uint32_t b = 0; b < 3; ++b)
    {
        // (the A/B: period 1, the raster's tolerance -- also read when the cadence is off)
        w[33 + b] = mRigCadence ? st.rigPeriod[b] : 1u;                    // RP_PERIOD
        const float m = mRigCadence ? st.rigTolMul[b] : 1.f;
        std::memcpy(&w[36 + b], &m, 4);                                    // RP_TOLMUL
    }
    std::memcpy(&w[39], &st.rigNearM, 4);                                  // RP_BAND_M
    std::memcpy(&w[40], &st.rigFarM, 4);
    w[41] = st.rigMaxStale;                                                // RP_MAX_STALE
    const float eps = mRigCadence ? st.rigPoseEps : 0.f;
    std::memcpy(&w[42], &eps, 4);                                          // RP_POSE_EPS
    w[43] = mRigRank ? 1u : 0u;                                            // RP_RANK (#1011)
    w[44] = mRigReuse ? 1u : 0u;                                           // RP_REUSE
    std::memcpy(&w[45], &st.rigReuseEps, 4);                               // RP_REUSE_EPS
    std::memcpy(&w[46], &st.rigPoseEps, 4);                                // RP_REUSE_LAG
    static_assert(46 < 64, "RP_* words 32..46 sit before RP_STILL (64)");
    {
        const VkDeviceAddress cb = mRigClasArena.address ? alignUpD(mRigClasArena.address, mClasAlign) : 0;
        w[28] = lo32(cb); w[29] = hi32(cb);      // RP_CLAS_BASE
        w[30] = (uint32_t)mRigClasStride;        // RP_CLAS_STRIDE
    }
    static_assert(64 + 2 * kAvatarSlotWords <= 128 && 128 + kAvatarSlotWords <= kRigParamWords,
                  "RP_STILL / RP_RT_AV / RP_NOSELF fit the parameter block");
    std::memcpy(w + 64, mInStill, sizeof(mInStill));   // RP_STILL (clean_cluster_rt.comp: 64)
    std::memcpy(w + 96, mInTraced, sizeof(mInTraced)); // RP_RT_AV (96)
    std::memcpy(w + 128, mInNoSelf, sizeof(mInNoSelf)); // RP_NOSELF (128, #1008)
    w[16] = kRigParamWords;                      // RP_ROWS_BASE
    w[17] = kRigParamWords + (uint32_t)mRigRowCpu.size();   // RP_JOBS_BASE
    if (!mRigRowCpu.empty()) std::memcpy(w + kRigParamWords, mRigRowCpu.data(), mRigRowCpu.size() * 4);
    if (!mRigJobCpu.empty()) std::memcpy(w + kRigParamWords + mRigRowCpu.size(), mRigJobCpu.data(), mRigJobCpu.size() * 4);
    if (!mRigHdrCpu.empty()) std::memcpy(mRigStage[mSlot].mapped, mRigHdrCpu.data(), mRigHdrCpu.size() * 4);
    mRigOn = true;
    mRigWhy.clear();
}

// The rigged arena slots the read-back demand asks for (+ a quarter, in kRigSlotStep steps).
uint32_t LLCleanClusterRTVK::rigSlotsWanted() const
{
    const uint32_t want = mLast[kCntRigWant];
    const uint32_t n = (want + want / 4 + 256 + kRigSlotStep - 1) / kRigSlotStep * kRigSlotStep;
    return std::clamp<uint32_t>(n, kRigMinSlots, kRigMaxSlots);
}

// Device bytes of a rigged arena of `slots` slots: the arena, its CLAS arguments / addresses and
// the implicit CLAS storage (the last size query's per-slot bytes; ~6 KB per CLAS before one).
uint64_t LLCleanClusterRTVK::rigBytesFor(uint32_t slots) const
{
    const uint64_t clas = mRigClasStride ? (uint64_t)mRigClasStride * slots : (uint64_t)slots * 6144;
    return (kRigHdrBase + (uint64_t)slots * (4 + kRigVtxWords)) * 4 + (uint64_t)slots * (64 + kRigAddrWords * 4) + clas + mClasAlign;
}

// ---- tiers 1 / 2: the arena sized from the read-back demand, the rigged CLAS of a frame ----
bool LLCleanClusterRTVK::planRigClas(VkDeviceSize salign, uint64_t budget)
{
    mRigBudgetCut = false;
#ifdef VK_NV_cluster_acceleration_structure
    if (mRigTier0 && mRig.buffer) releaseRig();   // (tier 0's layout: persistent, + indices)
    // Capacity: the slots last asked for (CNT_RIG_WANT, kFrameSlots frames ago; every rigged cut
    // at the main view's projection) + a quarter, in kRigSlotStep steps (#748 perf pass: the
    // power-of-two steps against a flat quarter of the cap pinned it at 16384 slots with ~400
    // clusters over -- every avatar then had a fallback group and none ever skipped its legacy
    // pre-skin); shrunk after ~5 s under half of it. Modes 13 / 14 hand it out per avatar: an
    // avatar that does not fit keeps the legacy BLAS (no CLAS built for it), the others are covered.
    const uint32_t need = rigSlotsWanted();
    uint32_t cap = mRigCap;
    if (!mRig.buffer || need > cap) { cap = need; mRigShrinkSince = 0; }
    // 2026-10-14 VRAM: shrink once the arena holds two steps more than the target for ~5 s (the
    // target already carries +1/4). The old "half" gate kept 86016 slots -- 472 MB of explicit
    // CLAS destinations at ~5.8 KB each -- for 39k live after a crowd's peak. (A re-create drops
    // the kept CLAS: one full rebuild frame, as any moving avatar does every frame.)
    else if (need + 2u * kRigSlotStep <= cap)
    {
        if (!mRigShrinkSince) mRigShrinkSince = std::max(mFrame, 1u);
        if (mFrame - mRigShrinkSince > 300) { cap = need; mRigShrinkSince = 0; }
    }
    else mRigShrinkSince = 0;
    // Fit the budget: arena + arguments + destinations / sizes / stamps + the CLAS storage of `cap`
    // clusters (explicit destinations: cap x the per-CLAS worst case, #748 perf pass 2).
    auto getSizes = reinterpret_cast<PFN_vkGetClusterAccelerationStructureBuildSizesNV>(mGetClasSizes);
    for (;;)
    {
        if (mRigClasCapQueried != cap)
        {
            VkClusterAccelerationStructureTriangleClusterInputNV tri{VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_TRIANGLE_CLUSTER_INPUT_NV};
            tri.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
            tri.maxGeometryIndexValue = kMaxCutPerInst - 1;     // geometry index = the entry k of the cut
            tri.maxClusterUniqueGeometryCount = 1;
            tri.maxClusterTriangleCount = kMaxClusterTris;
            tri.maxClusterVertexCount = kMaxClusterVerts;
            tri.maxTotalTriangleCount = cap * kMaxClusterTris;
            tri.maxTotalVertexCount = cap * kMaxClusterVerts;
            tri.minPositionTruncateBitCount = 0;
            VkClusterAccelerationStructureInputInfoNV ii{VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_INPUT_INFO_NV};
            ii.maxAccelerationStructureCount = cap;
            // (a moving avatar's are rebuilt every frame: build speed; a still one is kept)
            ii.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
            ii.opType = VK_CLUSTER_ACCELERATION_STRUCTURE_OP_TYPE_BUILD_TRIANGLE_CLUSTER_NV;
            ii.opMode = VK_CLUSTER_ACCELERATION_STRUCTURE_OP_MODE_EXPLICIT_DESTINATIONS_NV;
            ii.opInput.pTriangleClusters = &tri;
            VkAccelerationStructureBuildSizesInfoKHR sz{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
            getSizes(mCtx->device(), &ii, &sz);   // explicit mode: the size of ONE CLAS of these maxima
            if (!sz.accelerationStructureSize) { mRigWhy = "rigged CLAS size query returned 0"; return false; }
            mRigClasStride = alignUpD(sz.accelerationStructureSize, mClasAlign);
            mRigClasBytes = (VkDeviceSize)mRigClasStride * cap;
            mRigClasScratch = sz.buildScratchSize;
            mRigClasCapQueried = cap;
        }
        const uint64_t bytes = (kRigHdrBase + (uint64_t)cap * (4 + kRigVtxWords)) * 4 + (uint64_t)cap * (64 + kRigAddrWords * 4)
                             + mRigClasBytes + mClasAlign;
        if (bytes <= budget) break;
        if (cap <= kRigMinSlots) { mRigWhy = "RenderVKClusterRTMemoryMB too small for the rigged arena"; return false; }
        cap = std::max<uint32_t>(kRigMinSlots, cap / 4 * 3 / kRigSlotStep * kRigSlotStep);
        mRigBudgetCut = true;
    }
    if (cap != mRigCap || !mRig.buffer)
    {
        retire(mRigArgs); retire(mRigAddr); retire(mRigClasArena);
        mRigKeepReset = true;   // (every slot's CLAS goes)
    }
    const VkBufferUsageFlags kSt = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    const VkBufferUsageFlags kIn = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    bool ok = ensureRigArena(cap, false);
    ok = ok && ensureBuf(mRigArgs, (VkDeviceSize)cap * 64, kSt | kIn, false, "rt.cl.rigArgs");
    // (per argument: explicit destination + built size; per slot: its header's build stamp and
    // -- #1008 -- the frame its CLAS was built)
    ok = ok && ensureBuf(mRigAddr, (VkDeviceSize)cap * kRigAddrWords * 4, kSt | kIn | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false,
                         "rt.cl.rigAddr");
    ok = ok && ensureBuf(mRigClasArena, mRigClasBytes + mClasAlign, kSt, false, "rt.cl.rigClas", true);
    if (!ok)
    {
        mRigWhy = "rigged arena / CLAS (VRAM)";
        releaseRig();
        return false;
    }
    mScratchFrame += alignUpD(mRigClasScratch + mClasScratchAlign, salign);
    return true;
#else
    (void)salign; (void)budget;
    mRigWhy = "built without VK_NV_cluster_acceleration_structure headers";
    return false;
#endif
}

bool LLCleanClusterRTVK::rigTracedInst(uint32_t flags) const
{
    if (!(flags & LLCleanClusterSelectVK::kInstAvatar)) return true;
    const uint32_t a = (flags >> LLCleanClusterSelectVK::kInstAvShift) & kAvatarSlotMask;
    return ((mInTraced[a >> 5] >> (a & 31u)) & 1u) != 0u;
}

// ---- tier 0: per-cut KHR BLAS per rigged instance, built from the harvested cut, refit every
// frame from this frame's skinning of that cut ----
void LLCleanClusterRTVK::planT0Rig(LLCleanClusterVK& cl, VkDeviceSize salign, uint64_t budget)
{
    const Settings st = readSettings();
    const VkDevice dev = mCtx->device();
    const auto& rt = mCtx->rt();
    if (!mRigTier0 && mRig.buffer) releaseRig();   // (the CLAS tiers' layout)

    // ---- this frame's switched rigged instances (renderer instance id, ClusterInstance) ----
    const std::vector<ClusterInstance>& rows = cl.select().cpuInstances();
    std::vector<std::pair<uint32_t, uint32_t>> live;
    for (uint32_t i = 0; i < (uint32_t)rows.size() && i < mInstN; ++i)
    {
        const ClusterInstance& c = rows[i];
        if ((c.flags & CLI_RIGGED) && (c.flags & LLCleanClusterSelectVK::kInstDraw) && c.render_instance < mRiCap
            && c.skin_id < mRigSkinCount && rigTracedInst(c.flags))
            live.emplace_back(c.render_instance, i);
    }
    // ---- forget instances gone for a while (their BLAS + slots through the retire rings) ----
    for (auto it = mT0Rig.begin(); it != mT0Rig.end();)
    {
        if (std::max(it->second.lastSeen, it->second.wantFrame) + kT0KeepFrames < mFrame)
        {
            releaseT0Rig(it->second);
            it = mT0Rig.erase(it);
        }
        else ++it;
    }
    // ---- (re)build candidates: a fresh harvest whose cut differs from the BLAS's (or none, or
    // the periodic refit-quality rebuild is due) ----
    struct Cand { uint32_t ri, ii; bool has; uint32_t tris; };
    std::vector<Cand> cands;
    uint32_t want_slots = 0;
    for (const auto& li : live)
    {
        T0Rig& t = mT0Rig[li.first];
        const ClusterInstance& c = rows[li.second];
        if (t.as && (t.mesh != c.mesh || t.gen != c.mesh_generation)) releaseT0Rig(t);
        t.lastSeen = mFrame;
        const bool fresh = t.wantFrame == mFrame && t.wantN && t.mesh == c.mesh && t.gen == c.mesh_generation;
        if (!fresh || (t.as && t.hash == t.want && mFrame - t.builtFrame < kT0RigRebuild)) continue;
        uint32_t tris = 0;
        for (uint32_t k = 0; k < t.wantN; ++k) tris += mT0Cut[(size_t)(t.wantBase + k) * 3 + 2];
        cands.push_back({li.first, li.second, t.as != VK_NULL_HANDLE, tris});
        want_slots += t.wantN;
    }
    // ---- the arena: grown (x2 steps, within the budget) when this frame's builds cannot fit next
    // to what is held; nothing held: back to the minimum ----
    {
        uint32_t free_slots = 0;
        for (const auto& f : mRigAlloc.free) free_slots += f.second;
        const bool held = std::any_of(mT0Rig.begin(), mT0Rig.end(), [](const auto& kv) { return kv.second.as != VK_NULL_HANDLE; });
        uint32_t cap = mRig.buffer ? mRigCap : kRigMinSlots;
        if (!held && mRigFree.empty() && cap > kRigMinSlots) cap = kRigMinSlots;
        const uint32_t used = mRig.buffer ? mRigCap - std::min(mRigCap, free_slots) : 0u;
        if (used + want_slots > cap)
            cap = std::min<uint32_t>(kRigMaxSlots, nextPow2(used + want_slots + (used + want_slots) / 4));
        // (bytes: header + vertices + indices per slot)
        while (cap > kRigMinSlots && (kRigHdrBase + (uint64_t)cap * (4 + kRigVtxWords + kRigIdxWords)) * 4 > budget / 2) cap /= 2;
        if (!ensureRigArena(cap, true))
        {
            mRigWhy = "rigged arena (VRAM)";
            return;
        }
    }
    const uint64_t arena_bytes = mRig.size;
    // instances with no BLAS first (they trace the legacy one), then the smallest
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
        return a.has != b.has ? !a.has : a.tris < b.tris;
    });
    VkDeviceSize scratch = 0;
    uint32_t nb = 0, tris_total = 0;
    for (const Cand& c : cands)
    {
        if (nb >= st.builds || mFrame < mT0RigRetryFrame) break;
        if (nb && tris_total + c.tris > kT0TrisPerFrame) break;
        T0Rig& t = mT0Rig[c.ri];
        const uint32_t n = t.wantN;
        uint32_t s0 = 0;
        if (!mRigAlloc.alloc(n, s0)) continue;   // (fragmented: a smaller one may fit; grown next frame)
        std::vector<VkAccelerationStructureGeometryKHR> g(n);
        std::vector<VkAccelerationStructureBuildRangeInfoKHR> rg(n);
        std::vector<uint32_t> prim(n);
        for (uint32_t k = 0; k < n; ++k)
        {
            g[k] = VkAccelerationStructureGeometryKHR{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
            g[k].geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
            auto& tr = g[k].geometry.triangles;
            tr = VkAccelerationStructureGeometryTrianglesDataKHR{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
            tr.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
            tr.vertexStride = 16;
            tr.maxVertex = kMaxClusterVerts - 1;
            tr.vertexData.deviceAddress = mRig.address + ((VkDeviceAddress)mRigVtxBase + (VkDeviceAddress)(s0 + k) * kRigVtxWords) * 4;
            tr.indexType = VK_INDEX_TYPE_UINT16;
            tr.indexData.deviceAddress = mRig.address + ((VkDeviceAddress)mRigIdxBase + (VkDeviceAddress)(s0 + k) * kRigIdxWords) * 4;
            prim[k] = std::min<uint32_t>(mT0Cut[(size_t)(t.wantBase + k) * 3 + 2], kMaxClusterTris);
            rg[k] = VkAccelerationStructureBuildRangeInfoKHR{prim[k], 0, 0, 0};
        }
        VkAccelerationStructureBuildGeometryInfoKHR gi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        gi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        gi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
        gi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        gi.geometryCount = n; gi.pGeometries = g.data();
        VkAccelerationStructureBuildSizesInfoKHR sz{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        rt.vkGetAccelerationStructureBuildSizesKHR(dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &gi, prim.data(), &sz);
        const bool fits = sz.accelerationStructureSize
                       && (!nb || scratch + sz.buildScratchSize <= kT0ScratchBudget)
                       && mT0RigBytes + sz.accelerationStructureSize + arena_bytes <= budget;
        if (!fits) { mRigAlloc.release(s0, n); break; }
        LLCleanRTVK::Alloc a;
        VkDeviceAddress addr = 0;
        VkAccelerationStructureKHR as = VK_NULL_HANDLE;
        if (mRT.arenaAlloc(sz.accelerationStructureSize, a))
        {
            as = mRT.createAS(a, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, addr, "blas-cluster-rig", c.ri);
            if (!as) mRT.arenaFree(a);
        }
        if (!as)
        {
            // #697 / #698: never a crash -- this instance keeps the legacy BLAS; retried later
            mRigAlloc.release(s0, n);
            if (!mT0RigFails++)
                LL_WARNS("Vulkan") << "Cluster RT: rigged per-cut BLAS allocation failed (" << (sz.accelerationStructureSize >> 10)
                                   << " KiB); those avatars keep their legacy BLAS, retried in 60 frames" << LL_ENDL;
            mT0RigRetryFrame = mFrame + 60;
            break;
        }
        releaseT0Rig(t);   // (the previous BLAS + slots, through the retire rings)
        t.as = as; t.chunk = a.chunk; t.off = a.off; t.size = a.size; t.addr = addr;
        t.slots = Range{s0, n};
        t.hash = t.want;
        t.builtFrame = mFrame;
        t.updScratch = sz.updateScratchSize;
        t.geoms = std::move(g);
        t.ranges = std::move(rg);
        mT0RigBytes += a.size;
        // the slots' headers {ref, vpage, k | tris << 16, 0}, staged (copied before the selection)
        const VkDeviceSize src = (VkDeviceSize)mRigHdrCpu.size() * 4;
        for (uint32_t k = 0; k < n; ++k)
        {
            const uint32_t* e = &mT0Cut[(size_t)(t.wantBase + k) * 3];
            mRigHdrCpu.insert(mRigHdrCpu.end(), {e[0], e[1], k | (std::min<uint32_t>(e[2], kMaxClusterTris) << 16), 0u});
        }
        mRigHdrCopies.push_back({src, ((VkDeviceSize)kRigHdrBase + (VkDeviceSize)s0 * 4) * 4, (VkDeviceSize)n * 16});
        mT0RigRecs.push_back({c.ri, c.ii, true, sz.buildScratchSize});
        scratch += alignUpD(sz.buildScratchSize, salign);
        tris_total += c.tris;
        ++nb;
    }
    mT0RigBuilt = nb;
    // ---- every other live BLAS: refit from this frame's pose (a refit the scratch budget cannot
    // take this frame is skipped: that instance is not overridden -- legacy -- never stale) ----
    VkDeviceSize refit_scratch = 0;
    for (const auto& li : live)
    {
        auto it = mT0Rig.find(li.first);
        if (it == mT0Rig.end() || !it->second.as || it->second.builtFrame == mFrame) continue;
        const VkDeviceSize us = alignUpD(std::max<VkDeviceSize>(it->second.updScratch, 1), salign);
        if (refit_scratch + us > kT0ScratchBudget) break;
        refit_scratch += us;
        mT0RigRecs.push_back({li.first, li.second, false, it->second.updScratch});
        ++mT0RigRefit;
    }
    // ---- rows {ri, ClusterInstance, first slot, slots, build, BLAS lo, hi, valid} + jobs {row, k} ----
    for (uint32_t r = 0; r < (uint32_t)mT0RigRecs.size(); ++r)
    {
        const T0RigRec& rc = mT0RigRecs[r];
        const T0Rig& t = mT0Rig[rc.ri];
        mRigRowCpu.insert(mRigRowCpu.end(), {rc.ri, rc.ii, t.slots.start, t.slots.count, rc.build ? 1u : 0u,
                                             lo32(t.addr), hi32(t.addr), 1u});
        for (uint32_t k = 0; k < t.slots.count; ++k) mRigJobCpu.insert(mRigJobCpu.end(), {r, k});
    }
    mT0RigRows = (uint32_t)mT0RigRecs.size();
    mT0RigJobs = (uint32_t)(mRigJobCpu.size() / 2);
    mScratchFrame += scratch + refit_scratch;
}

uint32_t* LLCleanClusterRTVK::tlasRigMap(uint32_t entries)
{
    mTlasRigOn = false;
    if (!mCtx || !mRigOn || entries == 0) return nullptr;
    const VkDeviceSize need = ((VkDeviceSize)kTlasRigHdr + 2ull * entries) * 4;
    if (!ensureBuf(mTlasRig[mSlot], need, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true,
                   "rt.cl.tlasRig"))
        return nullptr;
    uint32_t* w = static_cast<uint32_t*>(mTlasRig[mSlot].mapped);
    std::memset(w, 0, (size_t)need);   // counters, heads (0 = legacy group), group flags
    w[2] = 0x80000000u;                // pass B: + switched faces traced on legacy geometry (bit 31: valid)
    w[3] = 1u;                         // (this is a real map: pass B may count into word 2)
    // #748 perf pass 2: the RT origin of the cluster arena's positions (pass B's translation)
    std::memcpy(w + 4, mRigOrigin, sizeof(mRigOrigin));
    w[7] = mRigOriginOk ? 1u : 0u;
    mTlasRigOn = true;
    mTlasRigValid[mSlot] = true;
    return w + kTlasRigHdr;
}

void LLCleanClusterRTVK::recordLiftFlag(VkCommandBuffer cmd)
{
    if (!mTlasRigOn || !mRigOn || !mRig.buffer || !mTlasRig[mSlot].buffer) return;
    const VkBufferCopy c{2 * 4, 2 * 4, 4};
    vkCmdCopyBuffer(cmd, mTlasRig[mSlot].buffer, mRig.buffer, 1, &c);
    bar(cmd, kXfer, kCS | kAS);
}

// ============================================================================================
// descriptor set (once per frame, before any bind of it)
// ============================================================================================
void LLCleanClusterRTVK::writeSets(const GpuBuffer& records)
{
    if (!mCtx || (!mActive && !mPtlasLayout)) return;
    const uint32_t s = mSlot;
    auto B = [&](const GpuBuffer& b) { return b.buffer ? b.buffer : mDummy.buffer; };
    const VkBuffer bufs[kBindings] = {
        B(mState), B(mInstRows), B(mCut), B(mOvr), B(mCnt), B(mClasOfSlot), B(mClasFree), B(mClasWant),
        B(mClasStamp), B(mClasArgs), B(mClasDst), B(mClasSize), B(mDecode), B(mClasRefs), B(mBlasInfo),
        B(mBlasAddr), B(mBlasOf), B(mT0Jobs[s]), B(mT0Rows[s]), B(mT0Idx), B(mHitCut), B(mT0Valid),
        B(records), B(mTlasPrev), B(mPtlasOps), B(mPtlasCmd),
        // 26..31 rigged: the arena, per-instance slots, the palettes, CLAS args / addresses, params
        B(mRig), B(mRigOf), (mRigOn && mPalBuf) ? mPalBuf : mDummy.buffer, B(mRigArgs), B(mRigAddr), B(mRigCpu[s]),
        // 32: the previous epoch's cut arena (static cut reuse)
        B(mCutPrev),
        // 33 / 34: the static chunk table + heap parameters (this slot), the heap's bookkeeping
        B(mStaticCpu[s]), B(mSbMeta),
        // 35: the spatial PTLAS partition state (counts + per-instance partition)
        B(mPtlasPart)};
    VkDescriptorBufferInfo bi[kBindings];
    VkWriteDescriptorSet w[kBindings];
    for (uint32_t i = 0; i < kBindings; ++i)
    {
        bi[i] = {bufs[i], 0, VK_WHOLE_SIZE};
        w[i] = VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstSet = mSets[s]; w[i].dstBinding = i; w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[i].pBufferInfo = &bi[i];
    }
    // (every frame: a re-created buffer can come back with the same handle)
    vkUpdateDescriptorSets(mCtx->device(), kBindings, w, 0, nullptr);
}

// ============================================================================================
// record
// ============================================================================================
void LLCleanClusterRTVK::dispatch(VkCommandBuffer cmd, uint32_t mode, uint32_t groups)
{
    if (!groups) return;
    ClRtPush p{};
    std::memcpy(p.cam, mCam, sizeof(p.cam));
    p.px_scale = mPxScale; p.tol_px = mTol; p.hyst = mHyst; p.rt_flags = mRtFlags;
    p.mode = mode; p.instance_count = mInstN; p.ri_cap = mRiCap; p.frame = mGpuFrame;
    p.cut_cap = kCutCap; p.max_cut = kMaxCutPerInst; p.pool_pages = mPoolPages; p.clas_pages_max = kClasPagesPerFrame;
    p.clas_cap = mClasCap; p.blas_max = mBlasMax; p.tier = (uint32_t)std::max(mTier, 0);
    p.job_count = mode == 6 ? mT0Jobs_n : mode == 20 ? mClasGrowFrom : mT0Rows_n;
    p.pool_addr[0] = lo32(mPoolAddr); p.pool_addr[1] = hi32(mPoolAddr);
    p.decode_addr[0] = lo32(mDecode.address); p.decode_addr[1] = hi32(mDecode.address);
    const VkDeviceAddress clas_base = mClasChunks.empty() ? 0 : alignUpD(mClasChunks[0].address, mClasAlign);   // (unused)
    p.clas_base[0] = lo32(clas_base); p.clas_base[1] = hi32(clas_base);
    p.clas_refs_addr[0] = lo32(mClasRefs.address); p.clas_refs_addr[1] = hi32(mClasRefs.address);
    p.clas_stride = (uint32_t)mClasStride; p.idle_frames = kClasIdle;
    p.part_size = mPartSize; p.ptlas_full = 0;
    vkCmdPushConstants(cmd, mLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
    const uint32_t gx = std::min(groups, kGridX);
    vkCmdDispatch(cmd, gx, (groups + gx - 1) / gx, 1);
}

// The same push as dispatch(), the grid from a buffer (VkDispatchIndirectCommand at `off`).
void LLCleanClusterRTVK::dispatchIndirect(VkCommandBuffer cmd, uint32_t mode, VkBuffer args, VkDeviceSize off)
{
    ClRtPush p{};
    std::memcpy(p.cam, mCam, sizeof(p.cam));
    p.px_scale = mPxScale; p.tol_px = mTol; p.hyst = mHyst; p.rt_flags = mRtFlags;
    p.mode = mode; p.instance_count = mInstN; p.ri_cap = mRiCap; p.frame = mGpuFrame;
    p.cut_cap = kCutCap; p.max_cut = kMaxCutPerInst; p.pool_pages = mPoolPages; p.clas_pages_max = kClasPagesPerFrame;
    p.clas_cap = mClasCap; p.blas_max = mBlasMax; p.tier = (uint32_t)std::max(mTier, 0);
    p.job_count = mT0Rows_n;
    p.pool_addr[0] = lo32(mPoolAddr); p.pool_addr[1] = hi32(mPoolAddr);
    p.decode_addr[0] = lo32(mDecode.address); p.decode_addr[1] = hi32(mDecode.address);
    p.clas_refs_addr[0] = lo32(mClasRefs.address); p.clas_refs_addr[1] = hi32(mClasRefs.address);
    p.clas_stride = (uint32_t)mClasStride; p.idle_frames = kClasIdle;
    p.part_size = mPartSize; p.ptlas_full = 0;
    vkCmdPushConstants(cmd, mLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
    vkCmdDispatchIndirect(cmd, args, off);
}

void LLCleanClusterRTVK::record(VkCommandBuffer cmd, const std::function<VkDeviceAddress(VkDeviceSize)>& takeScratch,
                                const std::function<void(VkCommandBuffer, const char*)>& mark)
{
    if (!mActive || !mPipe || !mClusterSet) return;
    const uint32_t s = mSlot;
    // STATIC CUT REUSE: the previous arena (binding 32) holds the cuts of the epoch just before
    // this one, at the same tolerance and px_scale (else no instance reuses: they all walk).
    // (#1008 rigged reuse, bit 1: the previous arena is the epoch just before -- the rigged cut
    // follows the main view's projection, a not-due avatar keeps last frame's cut whatever it is.)
    {
        const bool prev_last = mCutWritten != 0 && mCutWritten == mCutEpoch;
        const bool prev_ok = prev_last && mCutTol == mTol && mCutPx == mPxScale;
        mCutEpoch = (mCutEpoch + 1u) & 0x3FFFFFFFu;
        if (mCutEpoch == 0) mCutEpoch = 1;
        mRtFlags = (mCutEpoch << 2) | (prev_ok ? 1u : 0u) | (prev_last ? 2u : 0u);
    }
    // ---- resets (transfer) ----
    vkCmdFillBuffer(cmd, mCnt.buffer, 0, kCntWords * 4, 0);
    vkCmdFillBuffer(cmd, mOvr.buffer, 0, VK_WHOLE_SIZE, 0);
    if (mStateFill) { vkCmdFillBuffer(cmd, mState.buffer, 0, VK_WHOLE_SIZE, 0); mStateFill = false; }
    const bool clas = mTier >= 1;
    if (clas && mClasReset)
    {
        vkCmdFillBuffer(cmd, mClasOfSlot.buffer, 0, VK_WHOLE_SIZE, 0xFFFFFFFFu);
        vkCmdFillBuffer(cmd, mClasStamp.buffer, 0, VK_WHOLE_SIZE, 0);
        mSlotCopyOf = mSlotCopyStamp = VK_NULL_HANDLE;   // (nothing to keep)
    }
    else if (clas && mSlotCopyOf && mSlotCopyStamp)
    {
        // the pool grew: the grown per-slot tables = none, then the old slots' entries over them
        // (the old buffers stay alive through the RT retire ring until this frame completed;
        // earlier frames' scan / alloc writes into them first)
        bar(cmd, kCS | kAS | kXfer, kXfer);
        vkCmdFillBuffer(cmd, mClasOfSlot.buffer, 0, VK_WHOLE_SIZE, 0xFFFFFFFFu);
        vkCmdFillBuffer(cmd, mClasStamp.buffer, 0, VK_WHOLE_SIZE, 0);
        bar(cmd, kXfer, kXfer);
        const VkBufferCopy co{0, 0, (VkDeviceSize)mSlotCopyPages * 16};
        const VkBufferCopy cs{0, 0, (VkDeviceSize)mSlotCopyPages * 4};
        if (mSlotCopyPages)
        {
            vkCmdCopyBuffer(cmd, mSlotCopyOf, mClasOfSlot.buffer, 1, &co);
            vkCmdCopyBuffer(cmd, mSlotCopyStamp, mClasStamp.buffer, 1, &cs);
        }
        mSlotCopyOf = mSlotCopyStamp = VK_NULL_HANDLE;
        mSlotCopyPages = 0;
    }
    if (clas && mSbMeta.buffer && (mSbReset || mSbMetaFresh))
    {
        // a new heap epoch: bump, last free frame, the class stacks (a fresh buffer: whole --
        // its CLAS build stamps = 0, older than any BLAS)
        bar(cmd, kCS | kAS | kXfer, kXfer);   // (after earlier frames' modes 1 / 2 / 4 / 21)
        vkCmdFillBuffer(cmd, mSbMeta.buffer, 0, mSbMetaFresh ? VK_WHOLE_SIZE : (VkDeviceSize)kSbHdrWords * 4, 0u);
        mSbReset = false;
        mSbMetaFresh = false;
    }
    if (!clas)
    {
        vkCmdFillBuffer(cmd, mT0Valid.buffer, (VkDeviceSize)s * kMaxT0Builds * 4, kMaxT0Builds * 4, 1u);
        if (!mT0HitCopies.empty() && mT0Stage[s].buffer)
            vkCmdCopyBuffer(cmd, mT0Stage[s].buffer, mHitCut.buffer, (uint32_t)mT0HitCopies.size(), mT0HitCopies.data());
    }
    const bool rig = mRigOn && mRig.buffer;
    if (rig)
    {
        // the arena's region words (hits read the vertex region base from word 0) + tier 0's
        // newly built slot headers
        // (word 2: the lift flag, recordLiftFlag -- 0 = unknown until pass B's count lands)
        const uint32_t hdr[4] = {mRigVtxBase, mRigIdxBase, 0u, 0u};
        vkCmdUpdateBuffer(cmd, mRig.buffer, 0, sizeof(hdr), hdr);
        // (modes 11 / 0 / 13 / 14 / 12; not the per-avatar "admitted last frame" words after the
        // table: mode 13 keeps those across frames)
        if (clas) vkCmdFillBuffer(cmd, mRigOf.buffer, 0, ((VkDeviceSize)mRigAvBase + kRigAvRows * kRigAvStride) * 4, 0u);
        // PERSISTENT RIGGED CLAS: nothing kept this frame -> the slot stamps and the kept-across-
        // frames per-avatar words start from zero (a re-created buffer holds garbage)
        if (clas && mRigKeepReset)
        {
            vkCmdFillBuffer(cmd, mRigOf.buffer, ((VkDeviceSize)mRigAvBase + kRigAvRows * kRigAvStride) * 4,
                            (VkDeviceSize)kRigAvRows * (1 + kRigAvPersist + kRigAvSched) * 4, 0u);
            if (mRigAddr.buffer) vkCmdFillBuffer(cmd, mRigAddr.buffer, 0, VK_WHOLE_SIZE, 0u);
            mRigKeepReset = false;
        }
        if (!mRigHdrCopies.empty() && mRigStage[s].buffer)
            vkCmdCopyBuffer(cmd, mRigStage[s].buffer, mRig.buffer, (uint32_t)mRigHdrCopies.size(), mRigHdrCopies.data());
    }
    // (+ DRAW_INDIRECT: modes 11 / 12 read the main view's finalize args as their grid)
    bar(cmd, kXfer | kCS | kAS, kCS | kAS | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPipe);
    const VkDescriptorSet sets[2] = {mClusterSet, mSets[s]};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mLayout, 0, 2, sets, 0, nullptr);
    if (clas)
    {
        if (mClasReset)
        {
            dispatch(cmd, 8, (mClasCap + 63) / 64);
            mClasReset = false;
            bar(cmd, kCS, kCS);
        }
        else if (mClasGrowFrom < mClasCap)
        {
            dispatch(cmd, 20, (mClasCap - mClasGrowFrom + 63) / 64);   // the new chunks' pages -> the free stack
            bar(cmd, kCS, kCS);
        }
        mClasGrowFrom = mClasCap;
        // FIXED-COST AUDIT (2026-10-14): the pool scan is one lane per POOL page every frame; it
        // only frees STATIC CLAS pages. Skipped while no static takes part AND the last read-back
        // free-stack top says no static CLAS page is live (a vanished static's pages are still
        // freed: the scan runs until they are all back on the stack).
        const uint32_t live_static_clas = mClasCap - std::min(mClasCap, mLast[kCntRbClasTop]);
        if (mStaticFrame || live_static_clas != 0)
        {
            dispatch(cmd, 1, (mPoolPages + 63) / 64);    // free CLAS pages whose pool slot changed / idle
            bar(cmd, kCS, kCS);
        }
    }
    // #1011 PROFILE: the RT cut's phases as GPU sub-marks (each = the time since the previous mark;
    // their sum is the old c_cl_rt_select, which the renderer's CLOD gpu sub line re-adds as
    // c_cl_rt_select_sum): c_cl_rts_reset (fills, CLAS scan), c_cl_rts_gather (mode 11: nothing
    // with the ranked cut), c_cl_rts_cut (mode 0), c_cl_rts_admit (13), c_cl_rts_place (14),
    // c_cl_rt_select (mode 12, the scatter).
    mark(cmd, "c_cl_rts_reset");
    if (rig && clas && mRigShared)
    {
        // the shared skinned cut: entries per instance (one lane per selected entry; #1011: an
        // early-out while the selection's summary stands for it)
        if (mSelCnt) dispatchIndirect(cmd, 11, mSelCnt, mSelDispatchOff);
        else dispatch(cmd, 11, (mRigSelCap + 63) / 64);
        bar(cmd, kCS, kCS);
        mark(cmd, "c_cl_rts_gather");
    }
    dispatch(cmd, 0, mInstN);                         // the RT cut
    bar(cmd, kCS, kCS | kAS);
    mark(cmd, "c_cl_rts_cut");
    if (rig && clas)
    {
        dispatch(cmd, 13, 1);                         // the arena per avatar, whole or not at all
        bar(cmd, kCS, kCS);
        mark(cmd, "c_cl_rts_admit");
        dispatch(cmd, 14, mInstN);                    // ... its instances' slots + headers
        bar(cmd, kCS, kCS | kAS);
        mark(cmd, "c_cl_rts_place");
    }
    if (rig && clas && mRigShared)
    {
        // ... into the slots of the complete ones
        if (mSelCnt) dispatchIndirect(cmd, 12, mSelCnt, mSelDispatchOff);
        else dispatch(cmd, 12, (mRigSelCap + 63) / 64);
        bar(cmd, kCS, kCS | kAS);
    }
    mark(cmd, "c_cl_rt_select");

#ifdef VK_NV_cluster_acceleration_structure
    if (clas)
    {
        auto buildClas = reinterpret_cast<PFN_vkCmdBuildClusterAccelerationStructureIndirectNV>(mCmdBuildClas);
        // ---- RIGGED: skin the slots mode 0 handed out -> one CLAS per slot (implicit
        // destinations, count = CNT_RIG <= the capacity), taken by mode 4 below ----
        if (rig && mRigArgs.buffer && mRigAddr.buffer && mRigClasArena.buffer && mRigClasStride)
        {
            dispatch(cmd, 9, mRigCap);
            bar(cmd, kCS, kAS | kCS);
            // (#1011 profile: mode 9 -- the shared copies, compares and own skinning -- apart from the
            // CLAS build below; c_cl_rig_clas is the build alone now, the gpu sub line re-adds the two)
            mark(cmd, "c_cl_rig_skin");
            VkClusterAccelerationStructureTriangleClusterInputNV tri{VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_TRIANGLE_CLUSTER_INPUT_NV};
            tri.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
            tri.maxGeometryIndexValue = kMaxCutPerInst - 1;
            tri.maxClusterUniqueGeometryCount = 1;
            tri.maxClusterTriangleCount = kMaxClusterTris;
            tri.maxClusterVertexCount = kMaxClusterVerts;
            tri.maxTotalTriangleCount = mRigCap * kMaxClusterTris;
            tri.maxTotalVertexCount = mRigCap * kMaxClusterVerts;
            VkClusterAccelerationStructureCommandsInfoNV ci{VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_COMMANDS_INFO_NV};
            ci.input = VkClusterAccelerationStructureInputInfoNV{VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_INPUT_INFO_NV};
            ci.input.maxAccelerationStructureCount = mRigCap;
            ci.input.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;   // (= planRigClas)
            ci.input.opType = VK_CLUSTER_ACCELERATION_STRUCTURE_OP_TYPE_BUILD_TRIANGLE_CLUSTER_NV;
            // PERSISTENT RIGGED CLAS: one explicit destination per arena slot (mode 9 writes it per
            // argument); only the slots whose headers were written this frame are built.
            ci.input.opMode = VK_CLUSTER_ACCELERATION_STRUCTURE_OP_MODE_EXPLICIT_DESTINATIONS_NV;
            ci.input.opInput.pTriangleClusters = &tri;
            ci.scratchData = alignUpD(takeScratch(mRigClasScratch + mClasScratchAlign), mClasScratchAlign);
            ci.dstAddressesArray = {mRigAddr.address, 8, (VkDeviceSize)mRigCap * 8};
            ci.dstSizesArray = {mRigAddr.address + (VkDeviceSize)mRigCap * 8, 4, (VkDeviceSize)mRigCap * 4};
            ci.srcInfosArray = {mRigArgs.address, 64, (VkDeviceSize)mRigCap * 64};
            ci.srcInfosCount = mCnt.address + kCntRigArgs * 4;   // <= mRigCap (one per slot written this frame)
            buildClas(cmd, &ci);
            mRigRecordedLast = true;
            mark(cmd, "c_cl_rig_clas");
            // (its addresses -> mode 4: the barrier after the static CLAS build below)
        }
        const uint32_t argsMax = kClasPagesPerFrame * kMaxClustersPerPage;
        // FIXED-COST AUDIT (2026-10-14): the STATIC CLAS build -- mode 2, mode 3 over its argument
        // CAPACITY (4096 workgroups) and an indirect CLAS build sized for 4096 CLAS -- only ever
        // builds CLAS for static cuts (mode 0 asks for static pages only). With no static in the
        // cut (RenderVKClusterStatic 0, the default) it was ~0.5 ms of empty work per frame
        // (c_cl_clas_build 0.54 ms for 2 traced avatars and no static, Cloudsdale 2026-10-14).
        if (mStaticFrame)
        {
        dispatch(cmd, 2, (kClasPagesPerFrame + 63) / 64);   // CLAS pages for the wanted pool slots
        bar(cmd, kCS, kCS);
        dispatch(cmd, 3, argsMax);                           // fp32 positions per CLAS
        bar(cmd, kCS, kAS | kCS);
        {
            VkClusterAccelerationStructureTriangleClusterInputNV tri{VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_TRIANGLE_CLUSTER_INPUT_NV};
            tri.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
            tri.maxGeometryIndexValue = mClasMaxGeom;
            tri.maxClusterUniqueGeometryCount = 1;
            tri.maxClusterTriangleCount = kMaxClusterTris;
            tri.maxClusterVertexCount = kMaxClusterVerts;
            tri.maxTotalTriangleCount = argsMax * kMaxClusterTris;
            tri.maxTotalVertexCount = argsMax * kMaxClusterVerts;
            VkClusterAccelerationStructureCommandsInfoNV ci{VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_COMMANDS_INFO_NV};
            ci.input = VkClusterAccelerationStructureInputInfoNV{VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_INPUT_INFO_NV};
            ci.input.maxAccelerationStructureCount = argsMax;
            ci.input.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
            ci.input.opType = VK_CLUSTER_ACCELERATION_STRUCTURE_OP_TYPE_BUILD_TRIANGLE_CLUSTER_NV;
            ci.input.opMode = VK_CLUSTER_ACCELERATION_STRUCTURE_OP_MODE_EXPLICIT_DESTINATIONS_NV;
            ci.input.opInput.pTriangleClusters = &tri;
            ci.scratchData = alignUpD(takeScratch(mClasScratch + mClasScratchAlign), mClasScratchAlign);
            ci.dstAddressesArray = {mClasDst.address, 8, (VkDeviceSize)argsMax * 8};
            ci.dstSizesArray = {mClasSize.address, 4, (VkDeviceSize)argsMax * 4};
            ci.srcInfosArray = {mClasArgs.address, 64, (VkDeviceSize)argsMax * 64};
            ci.srcInfosCount = mCnt.address + kCntClasArgs * 4;   // clamped by construction (<= argsMax)
            buildClas(cmd, &ci);
        }
        }
        else ++mStaticSkips;
        mark(cmd, "c_cl_clas_build");
        // (also orders the rigged CLAS build above before mode 4's reads of its addresses)
        bar(cmd, kAS | kCS, kCS | kAS);
        if (mStaticFrame && mSbOn && mSbArena.buffer && (mFrame & 15u) == 0u)
        {
            dispatch(cmd, 22, (mRiCap + 63) / 64);           // static BLAS blocks unused 120 frames -> freed
            bar(cmd, kCS, kCS);
        }
        dispatch(cmd, 4, mInstN);                            // per instance: CLAS refs -> BLAS info
        bar(cmd, kCS, kAS | kCS);
        {
            VkClusterAccelerationStructureClustersBottomLevelInputNV bl{VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_CLUSTERS_BOTTOM_LEVEL_INPUT_NV};
            bl.maxTotalClusterCount = kCutCap;
            bl.maxClusterCountPerAccelerationStructure = kMaxCutPerInst;
            VkClusterAccelerationStructureCommandsInfoNV ci{VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_COMMANDS_INFO_NV};
            ci.input = VkClusterAccelerationStructureInputInfoNV{VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_INPUT_INFO_NV};
            ci.input.maxAccelerationStructureCount = mBlasMax;
            ci.input.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
            ci.input.opType = VK_CLUSTER_ACCELERATION_STRUCTURE_OP_TYPE_BUILD_CLUSTERS_BOTTOM_LEVEL_NV;
            ci.input.opMode = VK_CLUSTER_ACCELERATION_STRUCTURE_OP_MODE_IMPLICIT_DESTINATIONS_NV;
            ci.input.opInput.pClustersBottomLevel = &bl;
            ci.dstImplicitData = alignUpD(mBlasArena.address, mClasBlasAlign);
            ci.scratchData = alignUpD(takeScratch(mBlasScratch + mClasScratchAlign), mClasScratchAlign);
            ci.dstAddressesArray = {mBlasAddr.address, 8, (VkDeviceSize)mBlasMax * 8};
            ci.srcInfosArray = {mBlasInfo.address, 16, (VkDeviceSize)mBlasMax * 16};
            // blas_max is >= the ClusterInstance rows (one info per row at most): never past it
            ci.srcInfosCount = mCnt.address + kCntBlasInfos * 4;
            buildClas(cmd, &ci);
        }
        mark(cmd, "c_cl_blas_build");
        // PERSISTENT STATIC BLAS (header STATIC): only the statics whose cut / CLAS changed this
        // frame, each into its own heap block (explicit destinations, written by mode 4); a still
        // camera builds none. Then the blocks they left -> their class stacks (mode 21).
        // (no static this frame: mode 4 wrote no static info and freed no block -- skipped)
        if (mStaticFrame && mSbOn && mSbArena.buffer)
        {
            VkClusterAccelerationStructureClustersBottomLevelInputNV bl{VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_CLUSTERS_BOTTOM_LEVEL_INPUT_NV};
            bl.maxTotalClusterCount = kCutCap;
            bl.maxClusterCountPerAccelerationStructure = kMaxCutPerInst;
            VkClusterAccelerationStructureCommandsInfoNV ci{VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_COMMANDS_INFO_NV};
            ci.input = VkClusterAccelerationStructureInputInfoNV{VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_INPUT_INFO_NV};
            ci.input.maxAccelerationStructureCount = mBlasMax;
            ci.input.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;   // (= planStaticBlas)
            ci.input.opType = VK_CLUSTER_ACCELERATION_STRUCTURE_OP_TYPE_BUILD_CLUSTERS_BOTTOM_LEVEL_NV;
            ci.input.opMode = VK_CLUSTER_ACCELERATION_STRUCTURE_OP_MODE_EXPLICIT_DESTINATIONS_NV;
            ci.input.opInput.pClustersBottomLevel = &bl;
            ci.scratchData = alignUpD(takeScratch(mSbScratch + mClasScratchAlign), mClasScratchAlign);
            ci.dstAddressesArray = {mBlasAddr.address + (VkDeviceAddress)mBlasMax * 8, 8, (VkDeviceSize)mBlasMax * 8};
            ci.dstSizesArray = {mBlasAddr.address + (VkDeviceAddress)mBlasMax * 16, 8, (VkDeviceSize)mBlasMax * 8};
            ci.srcInfosArray = {mBlasInfo.address + (VkDeviceAddress)mBlasMax * 16, 16, (VkDeviceSize)mBlasMax * 16};
            ci.srcInfosCount = mCnt.address + kCntSbInfos * 4;   // <= blas_max (one per instance row at most)
            buildClas(cmd, &ci);
            mark(cmd, "c_cl_sblas_build");
            dispatch(cmd, 21, (kSbPendCap + 63) / 64);
        }
        bar(cmd, kAS | kCS, kCS);
        dispatch(cmd, 5, mInstN);                            // override rows
    }
#endif
    if (!clas)
    {
        if (mT0Jobs_n && !mT0Recs.empty())
        {
            dispatch(cmd, 6, mT0Jobs_n);                     // expand + validate the planned cuts
            bar(cmd, kCS, kAS | kCS);
            std::vector<VkAccelerationStructureBuildGeometryInfoKHR> infos;
            std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> ranges;
            infos.reserve(mT0Recs.size()); ranges.reserve(mT0Recs.size());
            for (const T0Rec& r : mT0Recs)
            {
                auto it = mT0.find(r.ri);
                if (it == mT0.end() || !it->second.pend.as) continue;
                VkAccelerationStructureBuildGeometryInfoKHR gi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
                gi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
                gi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
                gi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
                gi.dstAccelerationStructure = it->second.pend.as;
                gi.geometryCount = r.gn; gi.pGeometries = &mT0Geoms[r.g0];
                gi.scratchData.deviceAddress = takeScratch(r.scratch);
                infos.push_back(gi);
                ranges.push_back(&mT0Ranges[r.g0]);
            }
            if (!infos.empty())
                mCtx->rt().vkCmdBuildAccelerationStructuresKHR(cmd, (uint32_t)infos.size(), infos.data(), ranges.data());
            mark(cmd, "c_cl_t0_blas");
            bar(cmd, kAS | kCS, kCS);
        }
        dispatch(cmd, 5, mT0Rows_n);                         // override rows (newest valid, else previous)
        // ---- RIGGED tier 0: skin every live per-cut BLAS's cut (+ the indices of this frame's
        // builds) -> build / refit -> the override rows of the valid ones ----
        if (rig && mT0RigJobs && !mT0RigRecs.empty())
        {
            bar(cmd, kCS, kCS);
            dispatch(cmd, 9, mT0RigJobs);
            bar(cmd, kCS, kAS | kCS);
            std::vector<VkAccelerationStructureBuildGeometryInfoKHR> infos;
            std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> ranges;
            infos.reserve(mT0RigRecs.size()); ranges.reserve(mT0RigRecs.size());
            for (const T0RigRec& r : mT0RigRecs)
            {
                auto it = mT0Rig.find(r.ri);
                if (it == mT0Rig.end() || !it->second.as || it->second.geoms.empty()) continue;
                const T0Rig& t = it->second;
                VkAccelerationStructureBuildGeometryInfoKHR gi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
                gi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
                gi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
                gi.mode = r.build ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
                gi.srcAccelerationStructure = r.build ? VK_NULL_HANDLE : t.as;   // in place
                gi.dstAccelerationStructure = t.as;
                gi.geometryCount = (uint32_t)t.geoms.size(); gi.pGeometries = t.geoms.data();
                gi.scratchData.deviceAddress = takeScratch(std::max<VkDeviceSize>(r.scratch, 1));
                infos.push_back(gi);
                ranges.push_back(t.ranges.data());
            }
            if (!infos.empty())
                mCtx->rt().vkCmdBuildAccelerationStructuresKHR(cmd, (uint32_t)infos.size(), infos.data(), ranges.data());
            mark(cmd, "c_cl_t0_rig");
            bar(cmd, kAS | kCS, kCS);
            dispatch(cmd, 10, (mT0RigRows + 63) / 64);
        }
    }
    // override rows -> TLAS pass A; counters (+ tier 0 cut) -> readback
    bar(cmd, kCS | kAS, kCS | kAS | kXfer);
    if (mReadback[s].buffer)
    {
        // counters words 0..61; word 62 = the static BLAS heap's bump pointer, word 63 = the CLAS
        // free-stack top (live CLAS pages = cap - top) -- disjoint regions, no write-after-write
        const VkBufferCopy cc0{0, 0, (VkDeviceSize)kCntRbSbBump * 4};
        vkCmdCopyBuffer(cmd, mCnt.buffer, mReadback[s].buffer, 1, &cc0);
        const VkBufferCopy cx{64 * 4, kRbExtra, (VkDeviceSize)(kCntWords - 64) * 4};   // (#1008 counters)
        vkCmdCopyBuffer(cmd, mCnt.buffer, mReadback[s].buffer, 1, &cx);
        if (clas)
        {
            const VkBufferCopy ct{0, (VkDeviceSize)kCntRbClasTop * 4, 4};
            vkCmdCopyBuffer(cmd, mClasFree.buffer, mReadback[s].buffer, 1, &ct);
            if (mSbMeta.buffer)
            {
                const VkBufferCopy cb{0, (VkDeviceSize)kCntRbSbBump * 4, 4};   // (the heap's bump pointer)
                vkCmdCopyBuffer(cmd, mSbMeta.buffer, mReadback[s].buffer, 1, &cb);
            }
        }
        else
        {
            const VkBufferCopy cv{(VkDeviceSize)s * kMaxT0Builds * 4, 256, kMaxT0Builds * 4};
            vkCmdCopyBuffer(cmd, mT0Valid.buffer, mReadback[s].buffer, 1, &cv);
            const VkBufferCopy ci{0, kRbHead, (VkDeviceSize)mInstN * 32};
            vkCmdCopyBuffer(cmd, mInstRows.buffer, mReadback[s].buffer, 1, &ci);
            const VkBufferCopy cc{0, kRbHead + (VkDeviceSize)mInstN * 32, (VkDeviceSize)kCutCap * 16};
            vkCmdCopyBuffer(cmd, mCut.buffer, mReadback[s].buffer, 1, &cc);
            mRbInst[s] = mInstN;
            mRbCut[s] = true;
        }
        VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT; b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT; b.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.memoryBarrierCount = 1; d.pMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &d);
        mRbValid[s] = true;
    }
    mark(cmd, "c_cl_rt");
    // this epoch's cuts become the next one's previous arena
    std::swap(mCut, mCutPrev);
    mCutWritten = mCutEpoch;
    mCutTol = mTol;
    mCutPx = mPxScale;

    // ---- counters (section log) + a periodic line ----
    LLVKHitch::count("rt_clTier", (unsigned)(mTier + 1));
    LLVKHitch::count("rt_clEligible", mLast[kCntEligible]);
    LLVKHitch::count("rt_clReady", mLast[kCntReady]);
    LLVKHitch::count("rt_clCutChanges", mLast[kCntCutChange]);
    LLVKHitch::count("rt_clRefResets", mLast[kCntRefResets]);
    if (clas)
    {
        LLVKHitch::count("rt_clClasPages", mClasCap - std::min(mClasCap, mLast[kCntRbClasTop]));
        LLVKHitch::count("rt_clClasBuilt", mLast[kCntClasArgs]);
        LLVKHitch::count("rt_clBlasBuilt", std::min(mLast[kCntBlasInfos], mBlasMax));
    }
    else
    {
        LLVKHitch::count("rt_clBlasBuilt", mT0BuiltFrame);
        LLVKHitch::count("rt_clBlasTrisK", mT0TrisFrame / 1000);
    }
    LLVKHitch::count("rt_clScratchMB", (unsigned)(mScratchFrame >> 20));
    LLVKHitch::count("rt_clRigEligible", mLast[kCntRigEligible]);
    LLVKHitch::count("rt_clRigReady", mLast[kCntRigReady]);
    LLVKHitch::count("rt_clRigLegacyOut", mTlasCovered);
    // #1008 rigged RT cadence: selections, copied-forward cuts, CLAS skipped by pose, oldest RT pose
    LLVKHitch::count("rt_clRigSel", mLast[kCntRigSel]);
    LLVKHitch::count("rt_clRigReused", mLast[kCntRigReused]);
    LLVKHitch::count("rt_clRigClasBuilt", std::min(mLast[kCntRigArgs], mRigCap));
    LLVKHitch::count("rt_clRigPoseSkip", mLast[kCntRigPoseSkip]);
    LLVKHitch::count("rt_clRigStaleMax", mLast[kCntRigStaleMax]);
    LLVKHitch::count("rt_clRigFallback", mTlasFallback);
    // #1011 CLAS reuse: shared slots compared / kept, own-skinned slots
    LLVKHitch::count("rt_clRigReuseCmp", mLast[kCntRigReuseCmp]);
    LLVKHitch::count("rt_clRigReuseSkip", mLast[kCntRigReuseSkip]);
    LLVKHitch::count("rt_clRigOwnSkin", mLast[kCntRigOwnSkin]);
    const double now = nowSec();
    if (now >= mNextLog)
    {
        mNextLog = now + 10.0;
        mAccShown = mAcc;
        mPlanMsShown = mPlanN ? mPlanMsAcc / mPlanN : 0.0;
        mPlanMsMaxShown = mPlanMsMax;
        mPlanMsAcc = mPlanMsMax = 0.0;
        mPlanN = 0;
        const double secs = mAccStart > 0.0 ? now - mAccStart : 0.0;
        mAccSecs = secs > 0.5 ? secs : 0.0;
        LL_INFOS("Vulkan") << statsLine() << LL_ENDL;
        mAcc = StaticAcc{};
        mAccStart = now;
    }
}

// ============================================================================================
// partitioned TLAS
// ============================================================================================
bool LLCleanClusterRTVK::ptlasEnsure(uint32_t cap)
{
#ifdef VK_NV_partitioned_acceleration_structure
    if (!mPtlasLayout || !mCtx || cap == 0) return false;
    const VkBufferUsageFlags kSt = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    const VkBufferUsageFlags kIn = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    const uint32_t part_mode = settingPtlasPartition();
    const float part_cell = settingPtlasCell();
    if (cap != mPtlasCap || !mPtlas.buffer || part_mode != mPartMode || (part_mode == 1 && part_cell != mPartCell))
    {
        retire(mPtlas); retire(mPtlasOps); retire(mTlasPrev); retire(mPtlasPart);
        mPtlasCap = 0; mPtlasAddr = 0;
        const auto sizeOf = [&](const VkPartitionedAccelerationStructureInstancesInputNV& q)
        {
            VkAccelerationStructureBuildSizesInfoKHR r{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
            reinterpret_cast<PFN_vkGetPartitionedAccelerationStructuresBuildSizesNV>(mGetPtlasSizes)(mCtx->device(), &q, &r);
            return r;
        };
        uint32_t part = kPartSize;
        if (mMaxPartitions && (cap + part - 1) / part > mMaxPartitions) part = (cap + mMaxPartitions - 1) / mMaxPartitions;
        VkPartitionedAccelerationStructureInstancesInputNV in{VK_STRUCTURE_TYPE_PARTITIONED_ACCELERATION_STRUCTURE_INSTANCES_INPUT_NV};
        in.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        in.instanceCount = cap;
        in.maxInstancePerPartitionCount = part;
        in.partitionCount = (cap + part - 1) / part;
        in.maxInstanceInGlobalPartitionCount = 0;
        VkAccelerationStructureBuildSizesInfoKHR sz = sizeOf(in);
        mPartIdOrderBytes = sz.accelerationStructureSize;
        mPartCount = 0; mPartMax = 0;
        if (part_mode == 1 && sz.accelerationStructureSize)
        {
            // SPATIAL: P regular partitions (grid cells hashed into them) + the GLOBAL partition, which
            // can hold every instance (disabled records, instances larger than a cell, a full cell's
            // overflow: a write never exceeds a capacity). The per-partition capacity is the largest of
            // cap / 4 .. 512 whose structure stays within 2x the id-order one (the API reserves by these
            // limits; the size query is the only portable measure of that reservation).
            const uint32_t P = std::max<uint32_t>(1u, std::min<uint32_t>(mMaxPartitions ? mMaxPartitions : kPartSpatial, kPartSpatial));
            const uint32_t cands[] = {std::max(cap / 4u, 1u), std::max(cap / 16u, 1u), std::max(cap / 64u, 1u), 2048u, 512u};
            for (const uint32_t m : cands)
            {
                VkPartitionedAccelerationStructureInstancesInputNV q = in;
                q.partitionCount = P;
                q.maxInstancePerPartitionCount = std::min(m, cap);
                q.maxInstanceInGlobalPartitionCount = cap;
                const VkAccelerationStructureBuildSizesInfoKHR qs = sizeOf(q);
                if (qs.accelerationStructureSize && qs.accelerationStructureSize <= 2u * mPartIdOrderBytes)
                {
                    in = q; sz = qs; mPartCount = P; mPartMax = q.maxInstancePerPartitionCount;
                    break;
                }
            }
            if (!mPartCount)
                LL_WARNS_ONCE("Vulkan") << "Cluster RT: no spatial PTLAS configuration within 2x the id-order size for "
                                        << cap << " instances -- partitioned by instance id" << LL_ENDL;
        }
        if (!sz.accelerationStructureSize)
        {
            LL_WARNS_ONCE("Vulkan") << "Cluster RT: partitioned TLAS size query returned 0 for " << cap
                                    << " instances -- RT consumers fall back (shadow maps) this session" << LL_ENDL;
            return false;
        }
        bool ok = ensureBuf(mPtlas, sz.accelerationStructureSize + 256, kSt, false, "rt.cl.ptlas", true);
        ok = ok && ensureBuf(mPtlasOps, (VkDeviceSize)cap * kPtlasOpBytes, kSt | kIn, false, "rt.cl.ptlasOps");
        ok = ok && ensureBuf(mTlasPrev, (VkDeviceSize)cap * 64, kSt, false, "rt.cl.tlasPrev");
        if (mPartCount)
            ok = ok && ensureBuf(mPtlasPart, ((VkDeviceSize)mPartCount + 1u + cap) * 4u,
                                 kSt | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false, "rt.cl.ptlasPart");
        if (!ok)
        {
            retire(mPtlas); retire(mPtlasOps); retire(mTlasPrev); retire(mPtlasPart);
            LL_WARNS("Vulkan") << "Cluster RT: partitioned TLAS allocation failed (" << (sz.accelerationStructureSize >> 20)
                               << " MB for " << cap << " instances); RT skipped this frame, retried next" << LL_ENDL;
            return false;
        }
        mPtlasCap = cap;
        mPartSize = mPartCount ? 0u : part;
        mPartMode = part_mode;
        mPartCell = part_cell;
        LL_INFOS("Vulkan") << "Cluster RT: partitioned TLAS for " << cap << " instances, "
                           << (mPartCount ? "SPATIAL partitions: " + std::to_string(mPartCount) + " x <= "
                                              + std::to_string(mPartMax) + " instances (cell "
                                              + std::to_string((int)mPartCell) + " m) + global <= " + std::to_string(cap)
                                          : "id-order partitions of " + std::to_string(part))
                           << ", " << (sz.accelerationStructureSize >> 20) << " MB (id order "
                           << (mPartIdOrderBytes >> 20) << " MB)" << LL_ENDL;
        mPtlasAddr = alignUpD(mPtlas.address, 256);
        mPtlasScratch = std::max(sz.buildScratchSize, sz.updateScratchSize);
        mPtlasFresh = true;
        mPtlasIn = in;
    }
    bool ok = ensureBuf(mPtlasCmd, kPtlasCmdBytes, kSt | kIn | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false, "rt.cl.ptlasCmd");
    ok = ok && ensureBuf(mCnt, kCntWords * 4, kSt | kIn | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         false, "rt.cl.cnt");
    return ok;
#else
    (void)cap;
    return false;
#endif
}

void LLCleanClusterRTVK::ptlasBuild(VkCommandBuffer cmd, const GpuBuffer& records, uint32_t count,
                                    const std::function<VkDeviceAddress(VkDeviceSize)>& takeScratch)
{
#ifdef VK_NV_partitioned_acceleration_structure
    if (!mPtlasLayout || !mPtlasPipe || !mPtlas.buffer || !mPtlasCmd.buffer || !records.buffer) return;
    // indirect command {WRITE_INSTANCE, argCount (the diff counts), ops, stride} + srcInfosCount = 1
    uint32_t c[8] = {(uint32_t)VK_PARTITIONED_ACCELERATION_STRUCTURE_OP_TYPE_WRITE_INSTANCE_NV, 0u,
                     lo32(mPtlasOps.address), hi32(mPtlasOps.address), (uint32_t)kPtlasOpBytes, 0u, 1u, 0u};
    static_assert(sizeof(VkBuildPartitionedAccelerationStructureIndirectCommandNV) == 24, "PTLAS indirect command");
    static_assert(sizeof(VkPartitionedAccelerationStructureWriteInstanceDataNV) == kPtlasOpBytes, "PTLAS write op");
    vkCmdUpdateBuffer(cmd, mPtlasCmd.buffer, 0, sizeof(c), c);
    if (!mActive) vkCmdFillBuffer(cmd, mCnt.buffer, 0, kCntWords * 4, 0);   // (diff stats)
    // SPATIAL partitions: a fresh PTLAS (every record written) starts with empty partitions and no
    // instance placed (CL_NONE)
    if (mPartCount && mPtlasPart.buffer && mPtlasFresh)
    {
        vkCmdFillBuffer(cmd, mPtlasPart.buffer, 0, ((VkDeviceSize)mPartCount + 1u) * 4u, 0u);
        vkCmdFillBuffer(cmd, mPtlasPart.buffer, ((VkDeviceSize)mPartCount + 1u) * 4u, (VkDeviceSize)mPtlasCap * 4u, 0xFFFFFFFFu);
    }
    // pass A / B records (compute) + the update -> the diff
    bar(cmd, kCS | kXfer | kAS, kCS);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPtlasPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPtlasLayoutObj, 1, 1, &mSets[mSlot], 0, nullptr);
    ClRtPush p{};
    p.mode = 7;
    p.instance_count = std::min(count, mPtlasCap);
    p.job_count = mPtlasCap;
    p.part_size = mPartSize;
    p.ptlas_full = mPtlasFresh ? 1u : 0u;
    if (mPartCount)
    {
        // (mode 7's spatial parameters ride in fields it does not otherwise read)
        p.cut_cap = mPartCount;       // regular partitions
        p.max_cut = mPartMax;         // instances per regular partition
        p.hyst = mPartCell;           // grid cell (m)
        p.tol_px = mPartCell;         // an instance whose largest axis scale exceeds it -> global
    }
    vkCmdPushConstants(cmd, mPtlasLayoutObj, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
    const uint32_t groups = (mPtlasCap + 63) / 64;
    const uint32_t gx = std::min(groups, kGridX);
    vkCmdDispatch(cmd, gx, (groups + gx - 1) / gx, 1);
    bar(cmd, kCS, kAS);
    VkBuildPartitionedAccelerationStructureInfoNV bi{VK_STRUCTURE_TYPE_BUILD_PARTITIONED_ACCELERATION_STRUCTURE_INFO_NV};
    bi.input = mPtlasIn;
    bi.srcAccelerationStructureData = mPtlasFresh ? 0 : mPtlasAddr;   // in place after the first build
    bi.dstAccelerationStructureData = mPtlasAddr;
    bi.scratchData = alignUpD(takeScratch(mPtlasScratch + 256), 256);
    bi.srcInfos = mPtlasCmd.address;
    bi.srcInfosCount = mPtlasCmd.address + 24;
    reinterpret_cast<PFN_vkCmdBuildPartitionedAccelerationStructuresNV>(mCmdBuildPtlas)(cmd, &bi);
    mPtlasFresh = false;
    mPtlasBuiltFrame = mFrame;
#else
    (void)cmd; (void)records; (void)count; (void)takeScratch;
#endif
}

// ============================================================================================
// stats
// ============================================================================================
std::string LLCleanClusterRTVK::memLine() const
{
    std::ostringstream o;
    const uint64_t rig_slot_b = (uint64_t)(4 + kRigVtxWords) * 4 + 64 + kRigAddrWords * 4 + mRigClasStride;
    const uint32_t live = std::min(mLast[kCntRig], mRigCap);   // (slots the arena holds this frame; want = the demand)
    const uint64_t tris = mLast[kCntRigTris];
    o << "rig arena " << mRigCap << " slots cap / " << live << " live / " << mLast[kCntRigWant] << " want (" << (((uint64_t)mRigCap * rig_slot_b) >> 20) << " / "
      << (((uint64_t)live * rig_slot_b) >> 20) << " MB, CLAS " << mRigClasStride << " B each";
    if (tris && live) o << ", ~" << ((uint64_t)live * mRigClasStride / std::max<uint64_t>(tris, 1)) << " B/tri live";
    o << "), static CLAS " << (mClasCap - std::min(mClasCap, mLast[kCntRbClasTop])) << "/" << mClasCap << " pages, PTLAS "
      << mPtlasCap << " instances (" << ((mPtlas.size + mPtlasOps.size + mTlasPrev.size) >> 20) << " MB)";
    return o.str();
}

std::string LLCleanClusterRTVK::statsLine() const
{
    std::ostringstream o;
    o << "Cluster RT: tier " << mTier << (mTierCap < 2 ? " (capped " + std::to_string(mTierCap) + ": " + mWhy + ")" : std::string())
      << " | eligible " << mLast[kCntEligible] << " ready " << mLast[kCntReady]
      << " overflow " << mLast[kCntOverflow] << " not-ready " << mLast[kCntNotReady]
      << " | cut " << mLast[kCntCut] << " clusters / " << mLast[kCntTris] << " tris, changes " << mLast[kCntCutChange]
      << " (ref resets " << mLast[kCntRefResets] << ")";
    // #748 perf pass: statics (RenderVKClusterRTStatic) -- below the admission threshold (legacy
    // BLAS, no selection), cuts reused from the previous frame (no page walk)
    if (!mStaticOn) o << " | static off (RenderVKClusterRTStatic 0)";
    else
    {
        char thr[24];
        std::snprintf(thr, sizeof(thr), "%.4f", mStaticMin);
        char tol[24];
        std::snprintf(tol, sizeof(tol), "%.1f", mTol);
        o << " | static tol " << tol << " px, admit >= " << thr << " (skipped " << mLast[kCntStaticSkip] << "), reused "
          << mLast[kCntReused] << ", CLAS budget " << mClasBudgetPages << " pages";
    }
    // (#748 static pass) per-frame averages over the last log window (mAccShown, mAccSecs)
    const double nf = (double)std::max<uint64_t>(mAccShown.frames, 1);
    auto avg = [&](uint64_t v) { char b[24]; std::snprintf(b, sizeof(b), "%.1f", (double)v / nf); return std::string(b); };
    if (mTier >= 1)
    {
        o << " | CLAS pages " << (mClasCap - std::min(mClasCap, mLast[kCntRbClasTop])) << " live / " << mClasCap << " cap in "
          << mClasChunks.size() << " chunks (" << ((uint64_t)mClasCap * kMaxClustersPerPage * mClasStride >> 20)
          << " MB, " << mClasStride << " B/CLAS), built " << mLast[kCntClasArgs] << " CLAS / " << mLast[kCntClasAlloc]
          << " pages (avg " << avg(mAccShown.clasBuilt) << " CLAS / frame), freed " << mLast[kCntClasFreed] << ", alloc fails "
          << mLast[kCntClasFail] << ", demand " << (mLast[kCntClasUsed] + mLast[kCntWant]) << " pages (room " << mClasRoomCap << ")"
          << (mStaticFrame ? "" : ", static passes SKIPPED (no static in the cut; " + std::to_string(mStaticSkips) + " frames)")
          << " | cluster BLAS " << std::min(mLast[kCntBlasInfos], mBlasMax) << " / frame implicit (rigged"
          << (mSbOn ? "" : " + static") << ", arena " << (mBlasArena.size >> 20) << " MB)";
        // PERSISTENT STATIC BLAS: built (cut / CLAS changed) / kept / previous-BLAS fallback / no
        // block, per frame (last frame; 10 s average); the heap; static RT flips (an override row
        // back after a frame on the legacy BLAS) per second.
        if (mSbOn)
            o << " | static BLAS built " << mLast[kCntSbInfos] << " kept " << mLast[kCntSbKept] << " previous "
              << mLast[kCntSbOld] << " no-block " << mLast[kCntSbFail] << " / frame (avg built " << avg(mAccShown.built)
              << " kept " << avg(mAccShown.kept) << " previous " << avg(mAccShown.old) << "), heap "
              << (((uint64_t)mLast[kCntSbUnits] * mSbUnit) >> 10) << " KiB live, "
              << (((uint64_t)std::min(mLast[kCntRbSbBump], mSbUnits) * mSbUnit) >> 10) << " KiB touched / "
              << (((uint64_t)mSbUnits * mSbUnit) >> 20) << " MB (epoch " << mSbEpoch << ", repacks " << mSbRepacks
              << ", re-created " << mSbGrows << (mLast[kCntSbLeak] ? ", leaked " + std::to_string(mLast[kCntSbLeak]) : std::string())
              << ")";
        else
            o << " | static BLAS per frame (" << (mSbUnsupported ? "explicit builds unsupported" : !readSettings().persist
                                                 ? "RenderVKClusterRTStaticPersist 0" : "no heap") << ")";
        char fl[32];
        std::snprintf(fl, sizeof(fl), "%.1f", mAccSecs > 0.0 ? (double)mAccShown.flips / mAccSecs : 0.0);
        o << ", static RT flips " << fl << " / s, not ready " << avg(mAccShown.notReady) << " / frame";
    }
    else if (mTier == 0)
        o << " | per-cut BLAS live " << mT0.size() << " (" << (mT0Bytes >> 20) << " MB), built " << mT0BuiltFrame
          << " / frame (" << mT0TrisFrame << " tris), invalid " << mT0InvalidTotal << " total, rows " << mT0Rows_n;
    // RIGGED: switched avatars' instances eligible / ready (override rows) / without slots; the
    // arena; the AS built; TLAS pass B's legacy rigged entries excluded (cluster records) and the
    // switched ones that kept the legacy group (fallback: the receiver lift covers them).
    o << " | rigged ";
    if (!mRigOn && !mRigWhy.empty()) o << "off (" << mRigWhy << "), ";
    if (mTier >= 1)
        o << "avatars " << mLast[kCntRigAvIn] << " in / " << mLast[kCntRigAvOut] << " out (want "
          << mLast[kCntRigWant] << " slots" << (mRigBudgetCut ? ", cut by the budget" : "") << "), ";
    o << "eligible " << mLast[kCntRigEligible] << " ready " << mLast[kCntRigReady] << " overflow "
      << mLast[kCntRigOverflow] << " invalid " << mLast[kCntRigInvalid] << " shared-cut " << mLast[kCntRigShared]
      << ", " << mLast[kCntRigTris] << " tris, arena "
      << (mTier >= 1 ? std::min(mLast[kCntRig], mRigCap) : (mRigCap - std::min<uint32_t>(mRigCap, [&] {
             uint32_t f = 0; for (const auto& r : mRigAlloc.free) f += r.second; return f; }())))
      << "/" << mRigCap << " slots (" << (mRig.size >> 20) << " MB)";
    // #748 perf pass 2: CLAS built this frame, avatars kept (no build: skinned positions + cut
    // unchanged), RT cuts derived from the shared cut (no page walk)
    if (mTier >= 1)
        o << ", CLAS built " << std::min(mLast[kCntRigArgs], mRigCap) << " / frame (" << (mRigClasArena.size >> 20)
          << " MB, " << mRigClasStride << " B/CLAS), avatars kept " << mLast[kCntRigKeptAv] << " rebuilt "
          << (mLast[kCntRigAvIn] - std::min(mLast[kCntRigAvIn], mLast[kCntRigKeptAv])) << " (their slots "
          << mLast[kCntRigRebuildSlots] << " = the CLAS the build may make), derived cuts "
          << mLast[kCntRigDerived];
    else
        o << ", per-cut BLAS live " << std::count_if(mT0Rig.begin(), mT0Rig.end(), [](const auto& kv) { return kv.second.as != VK_NULL_HANDLE; })
          << " (" << (mT0RigBytes >> 20) << " MB), built " << mT0RigBuilt << " refit " << mT0RigRefit << " / frame";
    // #1008 RIGGED RT CADENCE: the RT cut against the raster's (rigged clusters; the raster figure
    // is mode 11's gather, only while the shared cut runs), selections / copied-forward cuts this
    // frame, avatars due / kept / refreshed, the refreshed slots compared and kept by pose, the
    // oldest RT pose traced (frames).
    if (mTier >= 1)
    {
        o << " | rig cadence ";
        if (mRigCadenceIdle) o << "idle (no no-self-shadow avatar traced: every avatar exact, every frame)";
        else if (!mRigCadence) o << "off (RenderVKClusterRTRigCadence 0: every frame, the raster's cut)";
        else
            o << "RT cut " << mLast[kCntRigWant] << " clusters vs raster "
              << (mRigShared ? std::to_string(mLast[kCntRigRaster]) : std::string("n/a")) << ", selected "
              << mLast[kCntRigSel] << " inst / reused " << mLast[kCntRigReused] << ", avatars due " << mLast[kCntRigDueAv]
              << " kept " << mLast[kCntRigKeptAv] << " refreshed " << mLast[kCntRigRefreshAv] << " (slots compared "
              << mLast[kCntRigRefreshSlots] << ", pose-skipped " << mLast[kCntRigPoseSkip] << "), CLAS rebuilt "
              << std::min(mLast[kCntRigArgs], mRigCap) << ", max stale " << mLast[kCntRigStaleMax] << " frames";
    }
    // #1011: the ranked shared cut (mode 11 skipped, slots by rank, mode 13 on the workgroup) and
    // CLAS REUSE -- refreshed avatars (cadence or reuse), shared slots compared / kept (moved less
    // than the avatar's tolerance), the CLAS built; the skinning source per slot (copied from 2a's
    // shared skinned cut vs skinned by mode 9 itself); the CPU plan.
    if (mTier >= 1)
    {
        o << " | rig rank " << (mRigRank ? "on" : "off") << " reuse " << (mRigReuse ? "on" : "off") << ": avatars refreshed "
          << mLast[kCntRigRefreshAv] << ", slots compared " << mLast[kCntRigReuseCmp] << " kept "
          << mLast[kCntRigReuseSkip] << ", CLAS rebuilt " << std::min(mLast[kCntRigArgs], mRigCap)
          << " | skin: shared-copied " << mLast[kCntRigCopied] << " own-skinned " << mLast[kCntRigOwnSkin] << " slots";
    }
    {
        char pb[64];
        std::snprintf(pb, sizeof(pb), " | cpu plan %.3f/%.2f ms", mPlanMsShown, mPlanMsMaxShown);
        o << pb;
    }
    o << "; legacy rigged excluded " << mTlasCovered << " (fallback " << mTlasFallback << "); lift probe ";
    if (mTlasLegacyFlagged == kNone) o << "on (no group map)";
    else o << (mTlasLegacyFlagged ? "on (" : "off (") << mTlasLegacyFlagged << " switched faces traced legacy)";
    if (mPtlasLayout)
    {
        o << " | PTLAS " << mPtlasCap << " instances / ";
        if (mPartCount)
            o << "spatial " << mPartCount << " x <= " << mPartMax << " (cell " << (int)mPartCell << " m), writes to global "
              << mLast[kCntPtlasGlobal];
        else
            o << mPartSize << " per partition (id order)";
        o << ", " << (mPtlas.size >> 20) << " MB (id order " << (mPartIdOrderBytes >> 20) << "), writes " << mLast[kCntPtlasOps];
    }
    o << " | AS " << (asBytes() >> 20) << " MB (peak " << (mPeakAs >> 20) << ", cap " << (mMemCap >> 20) << ") aux "
      << (auxBytes() >> 20) << " MB (peak " << (mPeakAux >> 20) << ") scratch " << (mScratchFrame >> 20) << " MB (peak "
      << (mScratchPeak >> 20) << ")";
    return o.str();
}
