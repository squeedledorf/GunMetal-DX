/**
 * @file llcleanclusterskinvk.cpp
 * @brief Cluster LOD skinning and avatar deformation (#747, OWNER: 2a). Design, buffers and the
 *        per-avatar switch: llcleanclusterskinvk.h. Shaders:
 *        indra/vkclean/shaders/clean_cluster_preskin*.comp, clean_cluster_skin_common.glsl.
 *
 * Method: published material only (SIGGRAPH 2021 Nanite talk: per-cluster work over the
 * selected cut; meshoptimizer's MIT demo for the cluster bounds). The skinning itself is this
 * viewer's own legacy pre-skin (clean_preskin.comp), reproduced statement for statement.
 */
#include "linden_common.h"

#include "llcleanclusterskinvk.h"
#include "llvrambudgetvk.h"
#include "llcleanclustervk.h"
#include "llcleanclusterselectvk.h"
#include "llcleanclusterstreamvk.h"
#include "vk_pipeline.h"
#include "vk_world_shaders_spv.h"

#include "llerror.h"

#if defined(LL_HAS_CLEAN_PIPELINE)
#  include "llviewercontrol.h"
#  include "llcleanrigrestvk.h"   // #798: the rest joint rows (legacy mode 1's pairing space)
#  include "llviewercamera.h"     // #798 weld probe
#  include "llvoavatar.h"
#  include "../llcleanmeshasset.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
#include <unordered_map>

namespace
{
using namespace LLClusterFmt;
constexpr uint32_t kSlots = LLCleanClusterVK::kFrameSlots;
constexpr uint32_t kSetBindings = 6;          // palette, meta, joints, legacy work, legacy allocator, #798 rest table
constexpr uint32_t kGridX = 32768;            // dispatch rows (= CL_TASK_GRID_X)
constexpr uint32_t kMinSkinClusters = 256;    // smallest skinned arena (320 KiB)
constexpr uint64_t kShrinkFrames = 300;       // ~5 s under a quarter before an arena halves
// #1010 ARENA PRESSURE (llcleanclusterskinvk.h): the auto limit (a fraction of the VRAM budget and
// its clamp), the controller's pacing and thresholds, the episode re-arm time.
constexpr uint64_t kAutoArenaDiv = 16;
constexpr uint64_t kAutoArenaMin = 128ull << 20, kAutoArenaMax = 4096ull << 20;
constexpr uint64_t kAutoArenaNoBudget = 512ull << 20;   // no device-local budget reported
constexpr float    kArenaPMax = 17.f;         // every avatar (yours too: L 9) at its +8 ceiling
constexpr double   kArenaRaiseSec = 0.15;     // between raises (read-back lag + slot grace)
constexpr double   kArenaDwellSec = 2.0;      // under the release fill before a release (x2 per flip)
constexpr double   kArenaGrowRetrySec = 2.0;  // a failed growth holds the current size this long
constexpr float    kArenaHiFill = 0.92f, kArenaLoFill = 0.65f;
constexpr double   kEpisodeQuiet = 5.0;
constexpr uint32_t kRestRowWords = 216u * 8u;  // #798 rest table row (= LLRigRest::kRowWords)

// Push blocks (= the shaders' push_constant blocks).
struct PrepPush
{
    uint32_t mode, count, skin_count, avatar_count;
    float    layer_scale;
    uint32_t meta_words, owned_base, owned_words;
    uint32_t layer_base;             // scratch word of the AV_SLOT_WORDS layered-avatar bits
    uint32_t clr_av_base, clr_av_words;       // mode 3: per-avatar skinned boxes to zero (SKIN_OUT words)
    uint32_t clr_weld_heads, clr_weld_cells;  // mode 3: weld cell heads to CL_NONE (0 cells: no weld)
    uint32_t grace;                  // mode 4: frames a stable slot outlives its last selection
    uint32_t prev_joints_lo, prev_joints_hi;   // mode 6: previous joint table (device address, 0 = none)
    uint32_t view, map_base, frame, cap;
    float    eye[4];
};
static_assert(sizeof(PrepPush) == 96, "clean_cluster_preskin_prep.comp PrepPush");
struct SkinPush
{
    uint32_t skin_count, weld_on, weld_cap, weld_base, cell_mask;
    float    cell;
    uint32_t meta_words;
    uint32_t prev_mode;              // 0 none, 1 previous palettes (prev_lo/hi), 2 previous = current
    uint32_t prev_lo, prev_hi;       // device address of the previous-frame palettes (mode 1)
    uint32_t prev_cap;               // SkinBlocks they hold
    float    box_pad;                // skinned bounds padding (m): the weld's max gap (0: no weld)
    uint32_t stats_on;               // 1: count CLK_SKIN_VERTS (sampled frames)
    uint32_t reuse_on;               // 1: reuse slots skinned at their row's pose generation (step 3)
    uint32_t rest_on;                // #798: weld pairing at the avatar's rest pose (1) or in bind space (0)
    uint32_t pair_on;                // #826: WELD PAIR CACHE (rows whose pairs stand re-apply them)
};
static_assert(sizeof(SkinPush) == 64, "clean_cluster_preskin.comp PreskinPush");
// Arena bytes per skinned cluster: the current SkinVertex region + the previous-position one.
// (+ #748 perf pass: its skinned box; the per-avatar boxes follow the cap's clusters.)
// (+ 2026-10-14 step 1, STABLE SLOTS: its persistent meta + its free-list word; the key hash,
// 2 x nextPow2(cap) words, follows -- clean_cluster_skin_common.glsl STABLE SLOTS.)
inline uint64_t nextPow2Arena(uint64_t v) { uint64_t p = 1; while (p < v && p < (1ull << 40)) p <<= 1; return p; }
constexpr uint64_t kArenaBytesPerCluster =
    (uint64_t)LLCleanClusterSkinVK::kVertsPerCluster
    * (LLCleanClusterSkinVK::kSkinVertexBytes + LLCleanClusterSkinVK::kSkinPrevBytes)
    + LLCleanClusterSkinVK::kSkinBoxBytes + LLCleanClusterSkinVK::kSlotMetaWords * 4u + 4u
    // (+ #826 WELD PAIR CACHE: one PAIR word per vertex + the slot's ALLOC frame word)
    + (uint64_t)LLCleanClusterSkinVK::kVertsPerCluster * 4u + 4u;
constexpr uint64_t kAvBoxBytes = (uint64_t)LLCleanClusterSkinVK::kSkinAvRows * LLCleanClusterSkinVK::kSkinBoxBytes;
constexpr uint32_t kSlotGrace = 4;   // frames a stable slot outlives its last selection (frustum flicker, RT reads)
// (= clean_cluster_skin_common.glsl clSkin*BaseC / clSkinHashWords, in words of SKIN_OUT)
uint64_t avBoxWord(uint64_t cap) { return cap * LLCleanClusterSkinVK::kVertsPerCluster * 8u + cap * 8u; }
uint64_t slotMetaWord(uint64_t cap) { return avBoxWord(cap) + kAvBoxBytes / 4u; }
uint64_t slotHashWords(uint64_t cap) { return cap <= 1u ? 2u : 2u * nextPow2Arena(cap); }
// (+ step 2: the per-character frames after the hash, CL_SKIN_CHAR_WORDS words per row)
constexpr uint64_t kCharBytes = (uint64_t)LLCleanClusterSkinVK::kSkinAvRows * LLCleanClusterSkinVK::kCharWords * 4u;
// (+ step 3: the per-row pose generations and the rows' new-cluster bits after the frames)
constexpr uint64_t kGenBytes = (uint64_t)LLCleanClusterSkinVK::kSkinAvRows * 4u
                             + (LLCleanClusterSkinVK::kSkinAvRows + 31u) / 32u * 4u;
uint64_t slotGenWord(uint64_t cap) { return slotMetaWord(cap) + cap * LLCleanClusterSkinVK::kSlotMetaWords + cap
                                            + slotHashWords(cap) + kCharBytes / 4u; }
// (+ #798: the per-slot WELD STATUS words after the new bits, RenderVKClusterDebug 6)
// (+ #826 WELD PAIR CACHE after them: PAIR (cap x 64) and ALLOC (cap) words -- in
// kArenaBytesPerCluster -- then 2 words per row and the host's repair bits;
// = clean_cluster_skin_common.glsl clSkinPairBaseC .. clSkinRepairBaseC)
constexpr uint64_t kRepairWords = (LLCleanClusterSkinVK::kSkinAvRows + 31u) / 32u;
constexpr uint64_t kRowPairBytes = (uint64_t)LLCleanClusterSkinVK::kSkinAvRows * 2u * 4u + kRepairWords * 4u;
uint64_t weldStatWord(uint64_t cap) { return slotGenWord(cap) + kGenBytes / 4u; }
uint64_t rowPairWord(uint64_t cap) { return weldStatWord(cap) + cap + cap * LLCleanClusterSkinVK::kVertsPerCluster + cap; }
uint64_t repairWord(uint64_t cap) { return rowPairWord(cap) + (uint64_t)LLCleanClusterSkinVK::kSkinAvRows * 2u; }
uint64_t arenaBufferBytes(uint64_t cap) { return cap * kArenaBytesPerCluster + kAvBoxBytes + slotHashWords(cap) * 4u + kCharBytes + kGenBytes + cap * 4u + kRowPairBytes; }
struct WeldPush
{
    uint32_t mode, cap, weld_base, cell_mask;
    float    cell, eps, cos_n, max_gap, wmatch;
    uint32_t stats_on, slot_stats, pad2;   // stats_on: #798 unwelded-reason counters (sampled frames); slot_stats: RenderVKClusterDebug 6
};
static_assert(sizeof(WeldPush) == 48, "clean_cluster_preskin_weld.comp WeldPush");
static_assert(LLCleanClusterSkinVK::kHdrWords * 4 <= 256, "header readback");
static_assert(LLCleanClusterSkinVK::kInstBase >= LLCleanClusterSkinVK::kHdrWords, "CLK_INST_BASE past the header");

VkShaderModule makeModule(VkDevice dev, const uint32_t* code, size_t bytes, const char* name)
{
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &ci, nullptr, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    vkNoteShaderModule(m, code, bytes, name);
    return m;
}

uint64_t nextPow2(uint64_t v)
{
    uint64_t p = 1;
    while (p < v && p < (1ull << 40)) p <<= 1;
    return p;
}

#if defined(LL_HAS_CLEAN_PIPELINE)
static_assert(kRestRowWords == LLRigRest::kRowWords, "rest table row (llcleanrigrestvk.h)");
U32 settingRigged()
{
    static LLCachedControl<U32> s(gSavedSettings, "RenderVKClusterRigged", 1);
    return s;
}
// #1010: 0 (default) = auto, from the device's VRAM budget (llcleanclusterskinvk.h ARENA PRESSURE).
U32 settingArenaMB()
{
    static LLCachedControl<U32> s(gSavedSettings, "RenderVKClusterSkinArenaMB", 0);
    return s;
}
F32 settingLayerScale()
{
    static LLCachedControl<F32> s(gSavedSettings, "RenderVKClusterLayerErrorScale", 2.f);
    return s;
}
// The cluster weld follows the legacy weld's settings (RenderVKRigWeld*): the same seams are
// closed whichever path draws the avatar.
bool settingWeld()
{
    static LLCachedControl<bool> s(gSavedSettings, "RenderVKRigWeld", false);
    return s;
}
// ... including its PAIRING SPACE (#798): 1 (default) = the avatar's rest pose (legacy #641 mode 1),
// 0 = bind space (legacy mode 0; A/B for both paths).
bool settingWeldRest()
{
    static LLCachedControl<U32> s(gSavedSettings, "RenderVKRigWeldMode", 1U);
    return (U32)s != 0u;
}
// The cluster weld's entry CEILING -- its own setting, not RenderVKRigWeldMaxVerts (that one
// sizes the legacy weld, one entry per vertex of LOD-stepped work items). The cluster cut
// inserts one entry per cluster copy of every boundary vertex of every switched avatar at full
// detail: 550k entries for 56 avatars (2026-10-14), against the legacy setting's fixed 131072
// -- 76% of the candidates were dropped every frame, a different random subset each frame
// (atomic order), so one side of a mesh boundary welded and the other did not: lighting lines
// along the joins that TAA smeared soft. The capacity now follows the read-back demand below
// this ceiling (beginFrame), and a residual overflow skips the whole avatar (symmetric).
// HELD POSES (2026-10-14 step 3): reuse skinned clusters of held characters (0 = A/B: re-skin all).
bool settingReuse()
{
    static LLCachedControl<bool> s(gSavedSettings, "RenderVKClusterSkinReuse", true);
    return s;
}
// #826 WELD PAIR CACHE (default on, live): rows whose pairs stand re-apply them (no rest-pose
// evaluation, grid insert or search); 0 = every re-skinned row searches every frame (A/B).
bool settingWeldPairCache()
{
    static LLCachedControl<bool> s(gSavedSettings, "RenderVKClusterWeldPairCache", true);
    return s;
}
U32 settingClusterWeldMax()
{
    static LLCachedControl<U32> s(gSavedSettings, "RenderVKClusterWeldMaxVerts", 1048576U);
    return s;
}
#endif
// #798 RenderVKClusterDebug 6 (weld view): the weld writes each candidate's outcome to its slot.
bool settingDebugWeld()
{
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<U32> s(gSavedSettings, "RenderVKClusterDebug", 0);
    return (U32)s == 6u;
#else
    return false;
#endif
}
} // namespace

// =============================================================================================
// init / destroy
// =============================================================================================
bool LLCleanClusterSkinVK::init()
{
    VkDeviceContext& ctx = mOwner.ctx();
    const VkDevice dev = ctx.device();
#ifdef VK_EXT_mesh_shader
    mMesh = (mOwner.setStages() & VK_SHADER_STAGE_MESH_BIT_EXT) != 0;
#endif
    // Set 1: the renderer's rigged buffers (any may be absent: PARTIALLY_BOUND; the shaders
    // only touch what their counts say exists).
    {
        VkDescriptorSetLayoutBinding b[kSetBindings]{};
        VkDescriptorBindingFlags fl[kSetBindings]{};
        for (uint32_t i = 0; i < kSetBindings; ++i)
        {
            b[i].binding = i;
            b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            b[i].descriptorCount = 1;
            b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            fl[i] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
        }
        VkDescriptorSetLayoutBindingFlagsCreateInfo bf{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
        bf.bindingCount = kSetBindings;
        bf.pBindingFlags = fl;
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.pNext = &bf;
        li.bindingCount = kSetBindings;
        li.pBindings = b;
        if (vkdCreateSetLayout(dev, &li, &mDsl, "mDslClusterSkin") != VK_SUCCESS)
        {
            mDsl = VK_NULL_HANDLE;
            LL_WARNS("Vulkan") << "Cluster LOD skin: set layout failed" << LL_ENDL;
            return false;
        }
        if (mPool.create(dev, "clusterSkin", {{mDsl, kSlots}}) != VK_SUCCESS)
        {
            LL_WARNS("Vulkan") << "Cluster LOD skin: descriptor pool failed" << LL_ENDL;
            return false;
        }
        for (uint32_t s = 0; s < kSlots; ++s)
            if (mPool.allocate(mDsl, &mSets[s]) != VK_SUCCESS)
            {
                LL_WARNS("Vulkan") << "Cluster LOD skin: descriptor set allocation failed" << LL_ENDL;
                return false;
            }
    }
    // Pipelines: prep + skin = {cluster set, set 1}, weld = {cluster set}.
    {
        const VkDescriptorSetLayout sets[2] = {mOwner.setLayout(), mDsl};
        auto layout = [&](uint32_t nsets, uint32_t push, VkPipelineLayout& out, const char* name) -> bool
        {
            VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, push};
            VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            pli.setLayoutCount = nsets;
            pli.pSetLayouts = sets;
            pli.pushConstantRangeCount = 1;
            pli.pPushConstantRanges = &pr;
            if (vkdCreatePipelineLayout(dev, &pli, &out, name) != VK_SUCCESS) { out = VK_NULL_HANDLE; return false; }
            return true;
        };
        if (!layout(2, sizeof(PrepPush), mPrepLayout, "mClusterSkinPrepLayout")
            || !layout(2, sizeof(SkinPush), mSkinLayout, "mClusterSkinLayout")
            || !layout(1, sizeof(WeldPush), mWeldLayout, "mClusterSkinWeldLayout"))
        {
            LL_WARNS("Vulkan") << "Cluster LOD skin: pipeline layouts failed" << LL_ENDL;
            return false;
        }
        VkShaderModule mp = makeModule(dev, kWorldClusterPreskinPrepSpv, kWorldClusterPreskinPrepSpv_bytes, "kWorldClusterPreskinPrepSpv");
        VkShaderModule ms = makeModule(dev, kWorldClusterPreskinSpv, kWorldClusterPreskinSpv_bytes, "kWorldClusterPreskinSpv");
        VkShaderModule mw = makeModule(dev, kWorldClusterPreskinWeldSpv, kWorldClusterPreskinWeldSpv_bytes, "kWorldClusterPreskinWeldSpv");
        if (mp) mPrepPipe = createComputePipeline(dev, mp, mPrepLayout);
        if (ms) mSkinPipe = createComputePipeline(dev, ms, mSkinLayout);
        if (mw) mWeldPipe = createComputePipeline(dev, mw, mWeldLayout);
        for (VkShaderModule m : {mp, ms, mw})
            if (m) vkDestroyShaderModule(dev, m, nullptr);
        if (!mPrepPipe || !mSkinPipe || !mWeldPipe)
        {
            LL_WARNS("Vulkan") << "Cluster LOD skin: compute pipelines failed" << LL_ENDL;
            return false;
        }
    }
    // The bindings the cluster draw / selection read always exist (a minimal arena + scratch),
    // so a rigged entry never reads an unwritten descriptor.
    if (!ensureSkinOut(kMinSkinClusters) || !ensureScratch(1024, 1u << 15, 256, 0) || !ensureRest(0))
    {
        LL_WARNS("Vulkan") << "Cluster LOD skin: buffers unavailable (VRAM)" << LL_ENDL;
        return false;
    }
    for (uint32_t s = 0; s < kSlots; ++s)
    {
        VkMemScope mem(VKMEM_READBACK);
        VkDeviceContext::SoftAllocScope soft(&ctx);
        mReadback[s] = ctx.createBuffer(kHdrWords * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                            | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        if (mReadback[s].buffer && !mReadback[s].mapped) ctx.destroyBuffer(mReadback[s]);
        if (mReadback[s].mapped) std::memset(mReadback[s].mapped, 0, kHdrWords * 4);
    }
    mReady = true;
    LL_INFOS("Vulkan") << "Cluster LOD skin: ready (cluster pre-skin, posed bounds, joint stretch, weld on the cut)"
                       << LL_ENDL;
    return true;
}

void LLCleanClusterSkinVK::destroy()
{
    mReady = false;
    VkDeviceContext& ctx = mOwner.ctx();
    const VkDevice dev = ctx.device();
    for (VkPipeline* p : {&mPrepPipe, &mSkinPipe, &mWeldPipe})
        if (*p) { vkDestroyPipeline(dev, *p, nullptr); *p = VK_NULL_HANDLE; }
    for (VkPipelineLayout* l : {&mPrepLayout, &mSkinLayout, &mWeldLayout})
        if (*l) { vkDestroyPipelineLayout(dev, *l, nullptr); *l = VK_NULL_HANDLE; }
    mPool.destroy();
    for (VkDescriptorSet& s : mSets) s = VK_NULL_HANDLE;
    for (SetHave& h : mSetHave) h = SetHave{};
    if (mDsl) { vkdDestroySetLayout(dev, mDsl); mDsl = VK_NULL_HANDLE; }
    for (GpuBuffer* b : {&mStretch, &mSkinOut, &mScratch, &mRest, &mReadback[0], &mReadback[1], &mReadback[2]})
        if (b->buffer) ctx.destroyBuffer(*b);
    mRestCap = 0;
    mRestCpu.clear();
    mRestCpu.shrink_to_fit();
    mRestFree.clear();
    mRestDirtyRows.clear();
    for (RestSlot& r : mRestSlot) r = RestSlot{};
    mStretchRows = 0;
    mSkinOutClusters = 0;
    mScrInstCap = mScrMapCap = mScrOwnedWords = mScrWeldCap = mScrWeldCells = 0;
    mAvInst.clear();
    mAvInst.shrink_to_fit();
}

// =============================================================================================
// buffers
// =============================================================================================
bool LLCleanClusterSkinVK::ensureStretch(uint32_t rows)
{
    rows = (uint32_t)std::max<uint64_t>(64, nextPow2(std::max(rows, 1u)));
    if (mStretch.buffer && rows <= mStretchRows) return true;
    VkDeviceContext& ctx = mOwner.ctx();
    const VkDeviceSize bytes = 16 + (VkDeviceSize)rows * kStretchLanes * 4;
    GpuBuffer b;
    {
        // Read per cluster per view by the selection: device-local + host-visible (ReBAR) first,
        // plain host-visible as the fallback (the host only zero-fills it once).
        VkMemScope mem(VKMEM_RIG);
        VkDeviceContext::SoftAllocScope soft(&ctx);
        b = ctx.createBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                 | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (b.buffer && !b.mapped) ctx.destroyBuffer(b);
        if (!b.buffer)
            b = ctx.createBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
    if (b.buffer && !b.mapped) ctx.destroyBuffer(b);
    if (!b.buffer)
    {   // (soft: keep the old table -- rows past it read stretch 1 -- or the core placeholder)
        LL_WARNS_ONCE("Vulkan") << "Cluster LOD skin: joint-stretch table of " << rows << " rows unavailable" << LL_ENDL;
        return mStretch.buffer != VK_NULL_HANDLE;
    }
    std::memset(b.mapped, 0, (size_t)bytes);   // rows = 0: every stretch 1 until the GPU writes it
    mOwner.retire(mStretch);
    mStretch = b;
    mStretchRows = rows;
    mOwner.setBuffer(CLB_JOINT_STRETCH, mStretch);
    mPeakBytes = std::max(mPeakBytes, heldBytes());
    return true;
}

bool LLCleanClusterSkinVK::ensureSkinOut(uint64_t clusters)
{
    // 2026-10-14 VRAM: 4096-cluster steps (~9 MB), not powers of two, and a shrink once the arena
    // holds more than 1.5x the target + a step for ~5 s (the pow2 + quarter rule kept 131072
    // clusters, 276 MB, for a 47k demand after a crowd's peak).
    constexpr uint64_t kStep = 4096;
    const uint64_t want = std::max<uint64_t>(kMinSkinClusters, (clusters + kStep - 1) / kStep * kStep);
    if (mSkinOut.buffer && want <= mSkinOutClusters)
    {
        // Shrink with hysteresis (no unbounded growth: the arena follows the admitted avatars).
        if (mSkinOutClusters > want + want / 2 + kStep)
        {
            if (!mSkinShrinkSince) mSkinShrinkSince = mFrame ? mFrame : 1;
            if (mFrame < mSkinShrinkSince + kShrinkFrames) return true;
        }
        else
        {
            mSkinShrinkSince = 0;
            return true;
        }
    }
    VkDeviceContext& ctx = mOwner.ctx();
    GpuBuffer b;
    {
        VkMemScope mem(VKMEM_RIG);
        VkDeviceContext::SoftAllocScope soft(&ctx);
        // (#748 rigged cluster RT: with ray tracing on, the rigged CLAS build reads the shared
        // skinned cut straight from here -- an AS build input, by device address)
        const bool rt = ctx.rayTracingEnabled();
        b = ctx.createBuffer((VkDeviceSize)arenaBufferBytes(want),
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT   // (fill / update targets)
                                 | (rt ? VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR : 0u),
                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, rt);
    }
    if (!b.buffer)
    {
        // (#1010: the limit holds the current size for kArenaGrowRetrySec -- the arena pressure fits
        // the cut to it meanwhile, avatars coarsen whole -- then the growth is retried)
        if (mNow - mGrowFailAt >= 60.0)
            LL_WARNS("Vulkan") << "Cluster LOD skin: skinned arena of " << want << " clusters unavailable (VRAM); keeping "
                               << mSkinOutClusters << ", the cut is coarsened to fit; retried" << LL_ENDL;
        mGrowFailAt = mNow;
        return mSkinOut.buffer && clusters <= mSkinOutClusters;
    }
    mOwner.retire(mSkinOut);
    mSkinOut = b;
    mSkinOutClusters = want;
    mArenaReset = true;   // step 1: a new buffer holds no slot -- its meta is filled free before use
    mSkinShrinkSince = 0;
    mOwner.setBuffer(CLB_SKIN_OUT, mSkinOut);
    mPeakBytes = std::max(mPeakBytes, heldBytes());
    mArenaPeakBytes = std::max<uint64_t>(mArenaPeakBytes, mSkinOut.size);
    return true;
}

// Scratch layout: header | margins [inst_cap] | map [map_cap] | owned [owned_words] | layer bits
// [kAvatarSlotWords] | #1010 priority region [kPrioWords] | #798 border-free avatar bits [kWeldAvWords] | weld avatar-drop bits
// [kWeldAvWords] | weld (heads [cells], entries [cap x 12], results [cap x 8]).
// (mWeldBase = the heads; the drop bits sit at mWeldBase - kWeldAvWords = CLW_AV_WORDS, the
// diagnostic bits at mWeldBase - 2 x kWeldAvWords.)
bool LLCleanClusterSkinVK::ensureScratch(uint32_t inst_cap, uint32_t map_cap, uint32_t owned_words, uint32_t weld_cap)
{
    inst_cap = (uint32_t)std::max<uint64_t>(1024, nextPow2(inst_cap));
    map_cap = (uint32_t)std::max<uint64_t>(1u << 15, nextPow2(map_cap));
    owned_words = (uint32_t)std::max<uint64_t>(256, nextPow2(owned_words));
    // (entries in steps of kWeldCapStep -- only the cell count must be a power of two: its mask)
    weld_cap = weld_cap ? (uint32_t)std::min<uint64_t>(((uint64_t)weld_cap + kWeldCapStep - 1) / kWeldCapStep * kWeldCapStep,
                                                       0x7FFFFFFFull)
                        : 0u;
    const bool fits = mScratch.buffer && inst_cap <= mScrInstCap && map_cap <= mScrMapCap && owned_words <= mScrOwnedWords
                   && weld_cap <= mScrWeldCap;
    // (a shrink: when everything wanted is a quarter of what is held, after the hysteresis)
    const bool small = fits && inst_cap * 4 <= mScrInstCap && map_cap * 4 <= mScrMapCap && (weld_cap * 4 <= mScrWeldCap || mScrWeldCap == 0);
    if (fits && !small) { mScrShrinkSince = 0; return true; }
    if (fits && small)
    {
        if (!mScrShrinkSince) mScrShrinkSince = mFrame ? mFrame : 1;
        if (mFrame < mScrShrinkSince + kShrinkFrames) return true;
    }
    mScrShrinkSince = 0;
    const uint32_t cells = weld_cap ? (uint32_t)nextPow2((uint64_t)weld_cap * 2) : 0u;
    // (+ the layered-avatar bits, kAvatarSlotWords words after the owned bits: PrepPush layer_base;
    // + #1010 the PRIORITY region right after them: layer_base + AV_SLOT_WORDS)
    const uint64_t words = (uint64_t)kHdrWords + inst_cap + map_cap + owned_words + kAvatarSlotWords + kPrioWords
                         + 2u * kWeldAvWords + cells + (uint64_t)weld_cap * (kWeldEntWords + kWeldResWords);
    VkDeviceContext& ctx = mOwner.ctx();
    GpuBuffer b;
    {
        VkMemScope mem(VKMEM_RIG);
        VkDeviceContext::SoftAllocScope soft(&ctx);
        b = ctx.createBuffer(words * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT
                                            | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    }
    if (!b.buffer)
    {
        LL_WARNS_ONCE("Vulkan") << "Cluster LOD skin: scratch of " << (words >> 8) << " KiB unavailable (VRAM)" << LL_ENDL;
        return false;
    }
    mOwner.retire(mScratch);
    mScratch = b;
    mScrInstCap = inst_cap;
    mScrMapCap = map_cap;
    mScrOwnedWords = owned_words;
    mScrWeldCap = weld_cap;
    mScrWeldCells = cells;
    mMapBase = kHdrWords + inst_cap;
    mOwnedBase = mMapBase + map_cap;
    mLayerBase = mOwnedBase + owned_words;
    mPrioBase = mLayerBase + kAvatarSlotWords;
    mWeldBase = mPrioBase + kPrioWords + 2u * kWeldAvWords;
    mOwner.setBuffer(CLB_SKIN_SCRATCH, mScratch);
    mPeakBytes = std::max(mPeakBytes, heldBytes());
    return true;
}

uint32_t LLCleanClusterSkinVK::weldCapWanted() const
{
#if defined(LL_HAS_CLEAN_PIPELINE)
    if (!settingWeld()) return 0;
    // THE DEMAND: the candidates the pre-skin tried to insert (CLK_WELD_N counts past the
    // capacity, so a dropping frame already reports its whole need), peak over two ~5 s
    // windows, + a quarter + one step. Never past the ceiling.
    const uint64_t ceiling = std::clamp<uint64_t>(settingClusterWeldMax(), kWeldCapStep, kWeldCapMax);
    const uint64_t peak = std::max(mWeldPeakCur, mWeldPeakPrev);
    const uint64_t want = peak + peak / 4u + kWeldCapStep;
    return (uint32_t)std::clamp<uint64_t>(want, kWeldCapStep, ceiling);
#else
    return 0;
#endif
}

// =============================================================================================
// #798 REST TABLE: the weld's rest-pose pairing space (llcleanclusterskinvk.h REST TABLE)
// =============================================================================================
// (Re)creates the table for `rows` rows -- 0 = the header only (every slot "no row"), the binding
// always exists. Live rows are COMPACTED into the new buffer's first rows (the mirror is the
// source: the whole mirror is uploaded at the next recordPreskin); rows past it lose theirs (rebuilt
// when there is room). A failed allocation keeps the old table.
bool LLCleanClusterSkinVK::ensureRest(uint32_t rows)
{
    rows = std::min(rows, kMaxAvatars);
    if (mRest.buffer && rows == mRestCap) return true;
    constexpr uint32_t kRow = kRestRowWords;
    const uint64_t words = (uint64_t)kMaxAvatars + (uint64_t)rows * kRow;
    VkDeviceContext& ctx = mOwner.ctx();
    GpuBuffer b;
    {
        VkMemScope mem(VKMEM_RIG);
        VkDeviceContext::SoftAllocScope soft(&ctx);
        b = ctx.createBuffer(words * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    }
    if (!b.buffer)
    {
        LL_WARNS_ONCE("Vulkan") << "Cluster LOD skin: weld rest table of " << rows << " avatars unavailable (VRAM); keeping "
                                << mRestCap << LL_ENDL;
        return mRest.buffer != VK_NULL_HANDLE;
    }
    std::vector<uint32_t> cpu;
    try { cpu.assign((size_t)words, 0u); }
    catch (const std::bad_alloc&)
    {
        mOwner.ctx().destroyBuffer(b);
        return mRest.buffer != VK_NULL_HANDLE;
    }
    std::fill(cpu.begin(), cpu.begin() + kMaxAvatars, kNone);
    uint32_t next = 0;
    for (uint32_t a = 0; a < kMaxAvatars; ++a)
    {
        RestSlot& S = mRestSlot[a];
        if (S.row == kNone) continue;
        if (next < rows && (uint64_t)kMaxAvatars + (uint64_t)(S.row + 1u) * kRow <= mRestCpu.size())
        {
            std::memcpy(cpu.data() + kMaxAvatars + (size_t)next * kRow, mRestCpu.data() + kMaxAvatars + (size_t)S.row * kRow,
                        (size_t)kRow * 4u);
            S.row = next++;
            cpu[a] = S.row;
        }
        else
        {
            S.row = kNone;
            S.want = true;
            mRestTouched[a >> 5] |= 1u << (a & 31u);
        }
    }
    mRestFree.clear();
    for (uint32_t r = rows; r-- > next;) mRestFree.push_back(r);   // (pop_back hands out the lowest)
    mRestCpu.swap(cpu);
    mOwner.retire(mRest);
    mRest = b;
    mRestCap = rows;
    mRestLive = next;
    mRestAll = true;
    mRestHdrDirty = true;
    mRestDirtyRows.clear();
    mRestShrinkSince = 0;
    mPeakBytes = std::max(mPeakBytes, heldBytes());
    return true;
}

// The CPU plan of a frame (beginFrame, after admit(): this frame's admitted avatars). on = the weld
// pairs at the rest pose this frame (RenderVKRigWeld + RenderVKRigWeldMode 1).
void LLCleanClusterSkinVK::restPlan(bool on)
{
    mRestOn = false;
#if defined(LL_HAS_CLEAN_PIPELINE)
    constexpr uint32_t kRow = kRestRowWords;
    const uint32_t now = mFrame32;
    auto touch = [this](uint32_t a) { mRestTouched[a >> 5] |= 1u << (a & 31u); };
    auto setHdr = [this](uint32_t a, uint32_t row)
    {
        if (a < mRestCpu.size() && mRestCpu[a] != row) { mRestCpu[a] = row; mRestHdrDirty = true; }
    };
    auto freeRow = [&](uint32_t a)
    {
        RestSlot& S = mRestSlot[a];
        if (S.row == kNone) return;
        mRestFree.push_back(S.row);
        S.row = kNone;
        setHdr(a, kNone);
        touch(a);
    };
    if (!on)
    {
        // Weld off / bind-space pairing (mode 0): no rows; the table shrinks to its header.
        for (uint32_t a = 0; a < kMaxAvatars; ++a)
        {
            freeRow(a);
            mRestSlot[a] = RestSlot{};
        }
        mRestLive = 0;
        if (mRestCap) ensureRest(0);
        return;
    }
    mRestOn = true;

    // OWNERS (every 8 frames, or at once when an admitted slot has none yet): a slot taken by
    // another avatar loses its row (rebuilt for the new owner).
    bool owners = mRestOwnersFrame == 0u || now - mRestOwnersFrame >= 8u;
    for (uint32_t a = 0; a < kMaxAvatars && !owners; ++a)
        owners = avatarSwitched(a) && mRestSlot[a].owner.isNull();
    if (owners)
    {
        mRestOwnersFrame = now ? now : 1u;
        static std::vector<std::pair<LLUUID, uint32_t>> s_slots;
        LLRigRest::slotOwners(s_slots);
        static bool s_present[kMaxAvatars];
        std::memset(s_present, 0, sizeof(s_present));
        for (const auto& kv : s_slots)
        {
            const uint32_t a = kv.second;
            if (a >= kMaxAvatars) continue;
            s_present[a] = true;
            RestSlot& S = mRestSlot[a];
            if (S.owner != kv.first)
            {
                if (S.row != kNone) ++mRestBuilds[1];
                freeRow(a);
                S = RestSlot{};
                S.owner = kv.first;
            }
        }
        for (uint32_t a = 0; a < kMaxAvatars; ++a)
            if (!s_present[a] && (mRestSlot[a].owner.notNull() || mRestSlot[a].row != kNone))
            {
                freeRow(a);
                mRestSlot[a] = RestSlot{};
            }
    }

    // LIVENESS: an admitted avatar wants a row; a slot not admitted for kRestGrace frames frees its.
    uint32_t live = 0, want_new = 0;
    for (uint32_t a = 0; a < kMaxAvatars; ++a)
    {
        RestSlot& S = mRestSlot[a];
        if (avatarSwitched(a))
        {
            S.seen = now;
            if (S.row == kNone) S.want = true;
        }
        else if (S.row != kNone && now - S.seen > kRestGrace)
        {
            freeRow(a);
            S.want = false;
        }
        else if (S.row == kNone) S.want = false;
        if (S.row != kNone) ++live;
        else if (S.want && (int32_t)(now - S.next_try) >= 0) ++want_new;
    }

    // SHAPE TRIGGERS (legacy mode 1's, round robin over the slots holding a row, a few a frame):
    // the skeleton serial (shape sliders; avatar physics bumps it too: >= 300 frames apart) and
    // the bones' rest hash (joint overrides; >= 120 frames apart).
    for (uint32_t probes = 0, checked = 0; probes < 64u && checked < 4u; ++probes)
    {
        const uint32_t a = mRestRR++ % kMaxAvatars;
        RestSlot& S = mRestSlot[a];
        if (S.row == kNone || S.want) continue;
        ++checked;
        LLVOAvatar* avp = LLRigRest::findAvatar(S.owner);
        if (!avp) continue;
        if (LLRigRest::skeletonSerial(avp) != S.skel && now - S.built > 300u) S.want = true;
        else if (now - S.built > 120u && LLRigRest::boneHash(avp) != S.bones) S.want = true;
    }

    // CAPACITY FOLLOWS DEMAND: the live rows + the ones wanted now, + a quarter + 8, in steps;
    // grows at once, shrinks (compacting) after ~5 s at under half.
    {
        const uint32_t need = live + want_new;
        const uint32_t target = std::min<uint32_t>((need + need / 4u + 8u + kRestRowStep - 1u) / kRestRowStep * kRestRowStep,
                                                   kMaxAvatars);
        if (need > mRestCap) ensureRest(target);
        else if (mRestCap > 2u * need + kRestRowStep)
        {
            if (!mRestShrinkSince) mRestShrinkSince = mFrame ? mFrame : 1;
            if (mFrame >= mRestShrinkSince + kShrinkFrames) ensureRest(target);
        }
        else mRestShrinkSince = 0;
    }

    // BUILDS (kRestBuildsPerFrame a frame): new rows first, then the triggered rebuilds.
    uint32_t builds = 0;
    for (uint32_t pass = 0; pass < 2u && builds < kRestBuildsPerFrame; ++pass)
        for (uint32_t a = 0; a < kMaxAvatars && builds < kRestBuildsPerFrame; ++a)
        {
            RestSlot& S = mRestSlot[a];
            if (!S.want || (pass == 0u) != (S.row == kNone) || (int32_t)(now - S.next_try) < 0) continue;
            LLVOAvatar* avp = LLRigRest::findAvatar(S.owner);
            if (!avp)
            {
                // no avatar behind the slot (not in the object list yet, a pseudo slot): no rest
                // pose, so no weld for it -- as the legacy discovery; retry later
                S.next_try = now + 60u;
                ++mRestNoAv;
                continue;
            }
            const bool fresh = S.row == kNone;
            if (fresh)
            {
                if (mRestFree.empty()) continue;   // (the table could not grow: next frames)
                S.row = mRestFree.back();
                mRestFree.pop_back();
            }
            if ((uint64_t)kMaxAvatars + (uint64_t)(S.row + 1u) * kRow > mRestCpu.size())
            {
                S.row = kNone;   // (never expected: the free list holds rows of the mirror only)
                continue;
            }
            const uint32_t skel = LLRigRest::skeletonSerial(avp);
            const uint64_t bones = LLRigRest::boneHash(avp);
            ++mRestBuilds[fresh ? 0 : (skel != S.skel ? 2 : 3)];
            LLRigRest::buildRow(avp, reinterpret_cast<float*>(mRestCpu.data() + kMaxAvatars + (size_t)S.row * kRow));
            S.skel = skel;
            S.bones = bones;
            S.built = now;
            S.want = false;
            setHdr(a, S.row);
            mRestDirtyRows.push_back(S.row);
            touch(a);
            ++builds;
        }
    mRestLive = 0;
    for (const RestSlot& S : mRestSlot) mRestLive += S.row != kNone ? 1u : 0u;
    mRestPeak = std::max(mRestPeak, mRestLive);
    // (the stats line shows the last ~30 s of builds)
    if (mFrame >= mRestStatsFrame + 1800u)
    {
        mRestStatsFrame = mFrame;
        std::memcpy(mRestBuildsLast, mRestBuilds, sizeof(mRestBuilds));
        std::memset(mRestBuilds, 0, sizeof(mRestBuilds));
        mRestNoAvLast = mRestNoAv;
        mRestNoAv = 0;
        mRestPeak = mRestLive;
    }
#else
    (void)on;
#endif
}

// The table's changes of this frame -> the GPU (recordPreskin, after its WAR barrier): the whole
// mirror after a re-creation (or a dropped recording), else the header and the rebuilt rows.
void LLCleanClusterSkinVK::restUpload(VkCommandBuffer cmd, uint32_t& xfers)
{
    if (!mRest.buffer || mRestCpu.empty()) return;
    constexpr VkDeviceSize kChunk = 65536;   // vkCmdUpdateBuffer's limit
    const auto upload = [&](VkDeviceSize word, VkDeviceSize words)
    {
        const VkDeviceSize end = std::min<VkDeviceSize>((word + words) * 4, (VkDeviceSize)mRestCpu.size() * 4);
        for (VkDeviceSize off = word * 4; off < end; off += kChunk)
        {
            const VkDeviceSize n = std::min(kChunk, end - off);
            vkCmdUpdateBuffer(cmd, mRest.buffer, off, n, reinterpret_cast<const uint8_t*>(mRestCpu.data()) + off);
            ++xfers;
        }
    };
    constexpr uint32_t kRow = kRestRowWords;
    if (mRestAll)
    {
        // (the header + the rows up to the last live one)
        uint32_t top = 0;
        for (const RestSlot& S : mRestSlot)
            if (S.row != kNone) top = std::max(top, S.row + 1u);
        upload(0, (VkDeviceSize)kMaxAvatars + (VkDeviceSize)top * kRow);
    }
    else
    {
        if (mRestHdrDirty) upload(0, kMaxAvatars);
        for (uint32_t r : mRestDirtyRows)
            if (r < mRestCap) upload((VkDeviceSize)kMaxAvatars + (VkDeviceSize)r * kRow, kRow);
    }
    mRestAll = false;
    mRestHdrDirty = false;
    mRestDirtyRows.clear();
}

// =============================================================================================
// #798 WELD PROBE (RenderVKClusterWeldProbe, one-shot)
// =============================================================================================
// A CPU replica of the cluster weld's inputs and pairing for ONE admitted avatar (1 = the one
// nearest the view centre, not ours; 2 = ours), from the same data the GPU reads: each cluster
// mesh's build-source store slot (quantised positions, indices, weights), the skin meta, the
// avatar's rest row. Per mesh: the boundary flags the LEGACY slot carries (its weld candidates)
// against the flags the cluster BUILDER recomputes (LLCleanQV::markWeldBoundary on the decoded
// quantised positions: the cluster candidates), and for the builder's candidates the pairing of
// clean_rig_weld_common.glsl -- another mesh's candidate within eps with weight overlap >= wmatch
// -- at the REST pose and in BIND space, with the nearest other-mesh candidate's distance of the
// unpaired ones; then the pairs per mesh pair and a few near misses. Logged as "ClusterWeldProbe".
#if defined(LL_HAS_CLEAN_PIPELINE)
namespace
{
struct ProbeV
{
    uint32_t mesh = 0, v = 0;
    float    rest[3] = {}, bind[3] = {};
    uint32_t ja = 0, qw[2] = {};
    bool     legacy = false, builder = false;
};
float probeOverlap(const ProbeV& a, const ProbeV& b)
{
    float wa[4], wb[4];
    for (int i = 0; i < 2; ++i)
    {
        wa[i * 2] = (float)(a.qw[i] & 0xFFFFu) / 65535.f; wa[i * 2 + 1] = (float)(a.qw[i] >> 16) / 65535.f;
        wb[i * 2] = (float)(b.qw[i] & 0xFFFFu) / 65535.f; wb[i * 2 + 1] = (float)(b.qw[i] >> 16) / 65535.f;
    }
    float s = 0.f;
    for (uint32_t i = 0; i < 4; ++i)
    {
        if (wa[i] <= 0.f) continue;
        const uint32_t j = (a.ja >> (i * 8u)) & 0xFFu;
        bool seen = false;
        float sa = 0.f;
        for (uint32_t k = 0; k < 4; ++k)
        {
            if (((a.ja >> (k * 8u)) & 0xFFu) != j) continue;
            if (k < i && wa[k] > 0.f) seen = true;
            sa += wa[k];
        }
        if (seen) continue;
        float sb = 0.f;
        for (uint32_t k = 0; k < 4; ++k) if (((b.ja >> (k * 8u)) & 0xFFu) == j) sb += wb[k];
        s += std::min(sa, sb);
    }
    return s;
}
// col-major mat4 x (p, 1)
void probeXform(const float* m, const float p[3], float out[3])
{
    for (int r = 0; r < 3; ++r) out[r] = m[r] * p[0] + m[4 + r] * p[1] + m[8 + r] * p[2] + m[12 + r];
}
struct ProbeCell
{
    int64_t x, y, z;
    bool operator==(const ProbeCell& o) const { return x == o.x && y == o.y && z == o.z; }
};
struct ProbeCellHash
{
    size_t operator()(const ProbeCell& c) const
    {
        return (size_t)((uint64_t)c.x * 73856093ull ^ (uint64_t)c.y * 19349663ull ^ (uint64_t)c.z * 83492791ull);
    }
};
} // namespace
#endif

void LLCleanClusterSkinVK::weldProbe(uint32_t mode)
{
#if defined(LL_HAS_CLEAN_PIPELINE)
    float mm = 0.2f, wm = 0.8f;
    {
        static LLCachedControl<F32> s_mm(gSavedSettings, "RenderVKRigWeldDistance", 0.2f);
        static LLCachedControl<F32> s_wmatch(gSavedSettings, "RenderVKRigWeldWeightMatch", 0.8f);
        mm = std::clamp((float)s_mm, 0.05f, 10.f);
        wm = std::clamp((float)s_wmatch, 0.f, 1.f);
    }
    const float eps = mm * 0.001f;
    // ---- the avatar ----
    uint32_t target = kNone;
    float best = -2.f;
    LLViewerCamera* cam = LLViewerCamera::getInstance();
    const LLVector3 co = cam->getOrigin(), at = cam->getAtAxis();
    for (uint32_t a = 0; a < kMaxAvatars; ++a)
    {
        if (!avatarSwitched(a)) continue;
        LLVOAvatar* avp = LLRigRest::findAvatar(mRestSlot[a].owner);
        if (!avp || (mode == 2u) != avp->isSelf()) continue;
        LLVector3 d = avp->getPositionAgent() - co;
        const float len = d.normVec();
        const float score = len > 0.f ? d * at : 1.f;
        if (score > best) { best = score; target = a; }
    }
    if (target == kNone)
    {
        LL_INFOS("ClusterWeldProbe") << "no " << (mode == 2u ? "own" : "other") << " avatar on the cluster path" << LL_ENDL;
        return;
    }
    LLVOAvatar* avp = LLRigRest::findAvatar(mRestSlot[target].owner);
    std::vector<float> rest(kRestRowWords, 0.f);
    const RestSlot& RS = mRestSlot[target];
    const bool table_row = RS.row != kNone && (size_t)kMaxAvatars + (size_t)(RS.row + 1u) * kRestRowWords <= mRestCpu.size();
    if (table_row) std::memcpy(rest.data(), mRestCpu.data() + kMaxAvatars + (size_t)RS.row * kRestRowWords, kRestRowWords * 4u);
    else if (avp) LLRigRest::buildRow(avp, rest.data());
    // (a fresh row too: a stale table row shows as a difference)
    std::vector<float> fresh(kRestRowWords, 0.f);
    if (avp) LLRigRest::buildRow(avp, fresh.data());
    float row_diff = 0.f;
    for (uint32_t k = 0; k < kRestRowWords; ++k) row_diff = std::max(row_diff, std::fabs(rest[k] - fresh[k]));

    // ---- the meshes and their candidates ----
    LLCleanMeshAsset& assets = LLCleanMeshAsset::instance();
    struct PM { const InstNote* n; uint32_t vc = 0, legacy = 0, builder = 0, both = 0, lod = 99; bool ok = false; const char* why = ""; };
    std::vector<PM> meshes;
    std::vector<ProbeV> pv;
    for (const InstNote& n : mAvInst)
    {
        if (n.av != target) continue;
        PM m;
        m.n = &n;
        const uint32_t mi = (uint32_t)meshes.size();
        U32 vc = 0, ic = 0, wc = 0;
        const LLCleanQV::QVertex* pk = n.src_slot != kNone ? assets.cpuSlotPacked(n.src_slot, vc) : nullptr;
        const U16* idx = n.src_slot != kNone ? assets.cpuSlotIndices(n.src_slot, ic) : nullptr;
        const LLCleanMeshAsset::MirrorWeight* w = n.src_slot != kNone ? assets.cpuSlotWeights(n.src_slot, wc) : nullptr;
        for (S32 l = 0; l < LLCleanMeshAsset::NUM_LOD; ++l)
            if (assets.groupSlotAt(n.mesh_id, n.face, l) == n.src_slot) m.lod = (uint32_t)l;
        const bool meta_ok = mIn.meta_cpu && n.skin_id != kNone && (uint64_t)(n.skin_id + 1u) * kSkinMetaBytes <= mIn.meta_cpu_bytes;
        if (!pk || !idx || !w || wc != vc || vc < 3 || !meta_ok)
        {
            // (a cluster-owned mesh whose CPU mirror was freed once its DAG was on disk: skipped,
            // not fetched back for a diagnostic)
            m.why = !meta_ok ? "no skin meta"
                  : (!pk && n.src_slot != kNone && assets.mirrorReleased(n.src_slot)) ? "mirror freed (cluster-owned, DAG on disk; RenderVKClusterFreeMirrors 0 keeps new ones): skipped"
                  : "no slot mirror";
            meshes.push_back(m);
            continue;
        }
        m.ok = true;
        m.vc = vc;
        LLCleanQV::QHeader qh;
        LLCleanQV::readHeader(pk, qh);
        const LLCleanQV::QVertex* qv = pk + LLCleanQV::kHeaderVerts;
        std::vector<float> pos((size_t)vc * 3);
        for (U32 v = 0; v < vc; ++v) LLCleanQV::decodePos(qh, qv[v], &pos[(size_t)v * 3]);
        std::vector<LLCleanQV::QWeight> bw(w, w + vc);
        LLCleanQV::markWeldBoundary(bw.data(), vc, idx, ic, pos.data(), 3);
        uint32_t mw[kSkinMetaBytes / 4];
        std::memcpy(mw, mIn.meta_cpu + (size_t)n.skin_id * kSkinMetaBytes, sizeof(mw));
        const uint32_t jc = mw[1];
        auto mf = [&mw](uint32_t word) { float f; std::memcpy(&f, &mw[word], 4); return f; };
        float bs[16];
        for (int k = 0; k < 16; ++k) bs[k] = mf(1876u + (uint32_t)k);
        for (U32 v = 0; v < vc; ++v)
        {
            const bool lf = LLCleanQV::isWeldBoundary(w[v]), bf = LLCleanQV::isWeldBoundary(bw[v]);
            m.legacy += lf; m.builder += bf; m.both += lf && bf;
            if (!lf && !bf) continue;
            ProbeV p;
            p.mesh = mi;
            p.v = v;
            p.legacy = lf;
            p.builder = bf;
            probeXform(bs, &pos[(size_t)v * 3], p.bind);
            uint32_t jn[4];
            float wt[4];
            LLCleanQV::decodeWeight(w[v], jn, wt);
            const float ws = wt[0] + wt[1] + wt[2] + wt[3];
            if (ws > 0.f) for (float& x : wt) x /= ws;
            else { wt[0] = 1.f; wt[1] = wt[2] = wt[3] = 0.f; }
            for (uint32_t k = 0; k < 4; ++k)
            {
                const uint32_t lane = std::min(jn[k], 109u);
                const uint32_t src_j = jc > 0u ? std::min(lane, jc - 1u) : 0u;
                const uint32_t a = (uint32_t)std::clamp((int32_t)mw[4u + src_j], 0, 215);
                p.ja |= a << (k * 8u);
                if (wt[k] == 0.f) continue;
                float ib[16];
                for (int e = 0; e < 16; ++e) ib[e] = mf(116u + src_j * 16u + (uint32_t)e);
                float q[3];
                probeXform(ib, p.bind, q);
                const float* r = &rest[(size_t)a * 8u];
                for (int c = 0; c < 3; ++c) p.rest[c] += wt[k] * (r[4 + c] * q[c] + r[c]);
            }
            uint32_t qq[4];
            for (int k = 0; k < 4; ++k) qq[k] = (uint32_t)std::lround(std::clamp(wt[k], 0.f, 1.f) * 65535.f);
            p.qw[0] = qq[0] | (qq[1] << 16);
            p.qw[1] = qq[2] | (qq[3] << 16);
            pv.push_back(p);
        }
        meshes.push_back(m);
    }

    // ---- pairing (builder candidates = the cluster weld's; legacy set for comparison) ----
    const float cell = 2.f * eps, far_cell = 0.005f;
    auto pair = [&](bool use_rest, bool use_legacy, std::vector<int>& partner, std::vector<float>& nearest,
                    std::vector<uint32_t>& matrix)
    {
        const size_t n = pv.size(), nm = meshes.size();
        partner.assign(n, -1);
        nearest.assign(n, -1.f);
        matrix.assign(nm * nm, 0u);
        std::unordered_map<ProbeCell, std::vector<uint32_t>, ProbeCellHash> g, gf;
        auto P = [&](uint32_t i) { return use_rest ? pv[i].rest : pv[i].bind; };
        auto in = [&](uint32_t i) { return use_legacy ? pv[i].legacy : pv[i].builder; };
        auto cellOf = [](const float* p, float c) { return ProbeCell{(int64_t)std::floor(p[0] / c), (int64_t)std::floor(p[1] / c), (int64_t)std::floor(p[2] / c)}; };
        for (uint32_t i = 0; i < n; ++i)
            if (in(i)) { g[cellOf(P(i), cell)].push_back(i); gf[cellOf(P(i), far_cell)].push_back(i); }
        for (uint32_t i = 0; i < n; ++i)
        {
            if (!in(i)) continue;
            const float* p = P(i);
            const ProbeCell c0 = cellOf(p, cell);
            for (int dx = -1; dx <= 1 && partner[i] < 0; ++dx)
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dz = -1; dz <= 1; ++dz)
                    {
                        auto it = g.find(ProbeCell{c0.x + dx, c0.y + dy, c0.z + dz});
                        if (it == g.end()) continue;
                        for (uint32_t k : it->second)
                        {
                            if (pv[k].mesh == pv[i].mesh) continue;
                            const float* q = P(k);
                            const float d = std::sqrt((p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]) + (p[2] - q[2]) * (p[2] - q[2]));
                            if (d <= eps && probeOverlap(pv[i], pv[k]) >= wm)
                            {
                                if (partner[i] < 0) ++matrix[(size_t)pv[i].mesh * nm + pv[k].mesh];
                                partner[i] = (int)k;
                            }
                        }
                    }
            if (partner[i] >= 0) continue;
            const ProbeCell f0 = cellOf(p, far_cell);
            float bd = -1.f;
            for (int dx = -1; dx <= 1; ++dx)
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dz = -1; dz <= 1; ++dz)
                    {
                        auto it = gf.find(ProbeCell{f0.x + dx, f0.y + dy, f0.z + dz});
                        if (it == gf.end()) continue;
                        for (uint32_t k : it->second)
                        {
                            if (pv[k].mesh == pv[i].mesh) continue;
                            const float* q = P(k);
                            const float d = std::sqrt((p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]) + (p[2] - q[2]) * (p[2] - q[2]));
                            if (d <= far_cell && (bd < 0.f || d < bd)) { bd = d; partner[i] = -2 - (int)k; }
                        }
                    }
            nearest[i] = bd;
        }
    };
    std::vector<int> pr, pb, pl;
    std::vector<float> nr, nb, nl;
    std::vector<uint32_t> mr, mb, ml;
    pair(true, false, pr, nr, mr);    // the cluster weld: builder candidates at rest
    pair(false, false, pb, nb, mb);   // ... in bind space (mode 0)
    pair(true, true, pl, nl, ml);     // legacy mode 1: the legacy slots' candidates at rest

    LL_INFOS("ClusterWeldProbe") << "avatar slot " << target << " " << RS.owner << (avp && avp->isSelf() ? " (self)" : "")
                                 << ": " << meshes.size() << " meshes, " << pv.size() << " flagged vertices; eps " << mm
                                 << " mm, wmatch " << wm << "; rest row " << (table_row ? "from the table" : "built now")
                                 << ", max |table - fresh| " << row_diff << " m" << LL_ENDL;
    const size_t nm = meshes.size();
    for (size_t i = 0; i < nm; ++i)
    {
        const PM& m = meshes[i];
        if (!m.ok)
        {
            LL_INFOS("ClusterWeldProbe") << "m" << i << " " << m.n->mesh_id << ":f" << m.n->face << " slot " << m.n->src_slot
                                         << ": " << m.why << LL_ENDL;
            continue;
        }
        uint32_t cand = 0, rp = 0, bp = 0, lc = 0, lp = 0, near1 = 0, near2 = 0, near3 = 0, none = 0;
        for (size_t k = 0; k < pv.size(); ++k)
        {
            if (pv[k].mesh != i) continue;
            if (pv[k].legacy) { ++lc; lp += pl[k] >= 0; }
            if (!pv[k].builder) continue;
            ++cand;
            rp += pr[k] >= 0;
            bp += pb[k] >= 0;
            if (pr[k] >= 0) continue;
            const float d = nr[k];
            if (d < 0.f) ++none;
            else if (d <= eps) ++near1;   // (within eps: rejected by the weight overlap)
            else if (d <= 0.001f) ++near2;
            else ++near3;
        }
        LL_INFOS("ClusterWeldProbe") << "m" << i << " " << m.n->mesh_id << ":f" << m.n->face << " slot " << m.n->src_slot
                                     << " lod " << m.lod << " skin " << m.n->skin_id << " verts " << m.vc << " | flags legacy "
                                     << m.legacy << " builder " << m.builder << " (both " << m.both << ") | REST paired " << rp
                                     << "/" << cand << ", unpaired: within eps but weights " << near1 << ", <1mm " << near2
                                     << ", <5mm " << near3 << ", none " << none << " | BIND paired " << bp << "/" << cand
                                     << " | legacy candidates at rest paired " << lp << "/" << lc << LL_ENDL;
    }
    for (size_t i = 0; i < nm; ++i)
        for (size_t j = 0; j < nm; ++j)
        {
            const uint32_t r = mr[i * nm + j], b = mb[i * nm + j], l = ml[i * nm + j];
            if (r || b || l)
                LL_INFOS("ClusterWeldProbe") << "m" << i << " -> m" << j << ": vertices paired REST " << r << " BIND " << b
                                             << " LEGACY-flags REST " << l << LL_ENDL;
        }
    // a few near misses at rest (builder candidates whose nearest other-mesh candidate is past eps)
    uint32_t shown = 0;
    for (size_t k = 0; k < pv.size() && shown < 12u; ++k)
    {
        if (!pv[k].builder || pr[k] >= 0 || pr[k] == -1 || nr[k] < 0.f) continue;
        const ProbeV& a = pv[k];
        const ProbeV& b = pv[(size_t)(-2 - pr[k])];
        float db = 0.f;
        for (int c = 0; c < 3; ++c) db += (a.bind[c] - b.bind[c]) * (a.bind[c] - b.bind[c]);
        LL_INFOS("ClusterWeldProbe") << "near miss m" << a.mesh << " v" << a.v << " -> m" << b.mesh << " v" << b.v << ": rest "
                                     << nr[k] * 1000.f << " mm (" << a.rest[0] << ", " << a.rest[1] << ", " << a.rest[2]
                                     << ") vs (" << b.rest[0] << ", " << b.rest[1] << ", " << b.rest[2] << "), bind "
                                     << std::sqrt(db) * 1000.f << " mm, overlap " << probeOverlap(a, b) << ", joints "
                                     << std::hex << a.ja << " / " << b.ja << std::dec << ", flags legacy " << a.legacy << "/"
                                     << b.legacy << LL_ENDL;
        ++shown;
    }
#else
    (void)mode;
#endif
}

void LLCleanClusterSkinVK::writeSet(uint32_t s)
{
    const GpuBuffer* src[kSetBindings] = {mIn.palette, mIn.meta, mIn.joints, mIn.work, mIn.rig_alloc, &mRest};
    VkWriteDescriptorSet w[kSetBindings]{};
    VkDescriptorBufferInfo bi[kSetBindings]{};
    uint32_t n = 0;
    SetHave& have = mSetHave[s];
    for (uint32_t i = 0; i < kSetBindings; ++i)
    {
        if (!src[i] || !src[i]->buffer) continue;   // (never used without it: PARTIALLY_BOUND)
        if (have.b[i] == src[i]->buffer && have.s[i] == src[i]->serial) continue;
        bi[n] = {src[i]->buffer, 0, VK_WHOLE_SIZE};
        w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[n].dstSet = mSets[s];
        w[n].dstBinding = i;
        w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[n].pBufferInfo = &bi[n];
        have.b[i] = src[i]->buffer;
        have.s[i] = src[i]->serial;
        ++n;
    }
    if (n) vkUpdateDescriptorSets(mOwner.ctx().device(), n, w, 0, nullptr);
}

// =============================================================================================
// frame inputs + the per-avatar switch (1b's gatherer)
// =============================================================================================
void LLCleanClusterSkinVK::setFrameInputs(const LLClusterSkinIn& in)
{
    mIn = in;
    if (!mIn.palette || !mIn.palette->buffer || !mIn.meta || !mIn.meta->buffer) mIn.skin_count = 0;
    if (!mIn.joints || !mIn.joints->buffer) mIn.avatar_count = 0;
    // (the skin meta buffer must hold every live skin the shaders index)
    if (mIn.meta && mIn.meta->buffer) mIn.skin_count = std::min<uint32_t>(mIn.skin_count, (uint32_t)(mIn.meta->size / kSkinMetaBytes));
}

bool LLCleanClusterSkinVK::riggedWanted() const
{
#if defined(LL_HAS_CLEAN_PIPELINE)
    return mReady && settingRigged() != 0;
#else
    return false;
#endif
}

void LLCleanClusterSkinVK::gatherBegin(uint32_t render_instances)
{
    for (AvTally& a : mAv) a = AvTally{};
    mAvInst.clear();
    mGatherBroken = false;
    mRenderInstWords = (render_instances + 31u) / 32u;
}

uint32_t LLCleanClusterSkinVK::avatarOfSkin(uint32_t skin_id) const
{
    if (skin_id == kNone || !mIn.meta_cpu || (uint64_t)(skin_id + 1u) * kSkinMetaBytes > mIn.meta_cpu_bytes) return kNone;
    uint32_t w[2] = {0u, 0u};   // SkinMeta::avatar_idx, joint_count
    std::memcpy(w, mIn.meta_cpu + (size_t)skin_id * kSkinMetaBytes, sizeof(w));
    // An unresolved (joint_count 0) or freed (zeroed) entry carries a DEFAULT avatar slot 0 --
    // a real avatar: never tally it under that slot (llcleanskinstore.cpp populateSkinMeta).
    return (w[1] != 0u && llAvatarSlotOk(w[0], "cluster avatarOfSkin")) ? w[0] : kNone;
}

void LLCleanClusterSkinVK::noteRow(uint32_t av)
{
    if (av < kMaxAvatars) ++mAv[av].rows;
}

void LLCleanClusterSkinVK::noteInstance(uint32_t av, uint32_t inst, uint32_t mesh_dir, uint32_t skin_id, bool self,
                                        const LLUUID& mesh_id, int32_t face, uint32_t src_slot, float importance,
                                        float arena_scale)
{
    if (av >= kMaxAvatars) return;
    AvTally& t = mAv[av];
    ++t.insts;
    if (std::isfinite(importance)) t.importance = std::max(t.importance, importance);
    t.coarse = t.coarse || arena_scale > 1.001f;
    InstNote n;
    n.av = av;
    n.inst = inst;
    n.clusters = mOwner.stream().meshClusterCount(mesh_dir);
    n.bad = n.clusters == 0 || skin_id >= mIn.skin_count;
    n.self = self;
    n.mesh_id = mesh_id;
    n.face = face;
    n.skin_id = skin_id;
    n.src_slot = src_slot;
    t.clusters += n.clusters;
    t.self = t.self || self;
    try { mAvInst.push_back(n); }
    catch (const std::bad_alloc&) { t.bad = true; }
}

// PER MESH (2026-10-14, owner decision: the cluster path REPLACES the legacy one -- no per-avatar
// "every mesh ready" switch, no legacy copy kept as a fallback). Every rigged ClusterInstance (its
// mesh is registered with its roots resident: a cut always exists) is drawn by the cluster path
// as soon as it exists; its avatar's other meshes simply are not drawn until theirs are (pop-in,
// the loading cloud covers it). An instance is held back only while it cannot be drawn correctly
// at all: the setting / module is off, its palette is missing, the gather broke off, or (RenderVKClusterRigged 1) the cluster pre-pass pipelines
// are still compiling while per-pixel pre-pass readers exist. The skinned arena is no admission
// limit here: it follows the read-back demand (beginFrame) up to its limit, the arena pressure
// coarsens whole avatars to fit it, and a frame whose new clusters still do not fit hides whole
// avatars from the end of the PRIORITY ORDER built here (#1010; never a subset of an avatar).
void LLCleanClusterSkinVK::admit(bool allowed, std::vector<uint32_t>& out)
{
    out.clear();
    std::memset(mWhy, 0, sizeof(mWhy));
    std::memset(mLayerBits, 0, sizeof(mLayerBits));
    std::memset(mAdmittedBits, 0, sizeof(mAdmittedBits));
    mAvCluster = mAvLegacy = 0;
    mInstAdmitted = 0;
    U32 rigged = 0;
#if defined(LL_HAS_CLEAN_PIPELINE)
    rigged = settingRigged();
#endif
    const bool prepassOk = mReady && mOwner.select().prepassReady();
    Why global = WHY_OK;
    if (!mReady || rigged == 0) global = WHY_OFF;
    else if (rigged == 1 && mIn.prepass_readers && !prepassOk) global = WHY_PREPASS;
    else if (!allowed) global = WHY_GATE;
    else if (mGatherBroken) global = WHY_PENDING;
    else if (mIn.skin_count == 0) global = WHY_NOPAL;
    // (the pre-pass pipelines are compiled asynchronously at init: a few frames; if they never
    // come, no rigged mesh is ever drawn -- say so once)
    mPrepassWait = global == WHY_PREPASS ? mPrepassWait + 1u : 0u;
    if (mPrepassWait == 600u)
        LL_WARNS("Vulkan") << "Cluster LOD skin: the cluster pre-pass pipelines are still missing after 600 frames: no "
                              "rigged mesh is drawn (RenderVKClusterRigged 2 draws them without the pre-pass)" << LL_ENDL;
    for (uint32_t a = 0; a < kMaxAvatars; ++a)
    {
        AvTally& t = mAv[a];
        t.admitted = 0;
        if (t.rows >= 2) mLayerBits[a >> 5] |= 1u << (a & 31u);   // #611: a layered outfit
        // rows whose mesh is not drawable yet (loading / building / roots uploading): not drawn
        if (t.rows > t.insts) mWhy[WHY_PENDING] += t.rows - t.insts;
    }
    for (const InstNote& n : mAvInst)
    {
        Why why = global;
        // (Our own avatar hidden in mouselook is ADMITTED, 2026-10-14: hidden in the main view
        // only -- the main cluster view carries CLV_HIDE_SELF and clean_cluster_select.comp /
        // clean_cluster_pick.comp leave its CLI_SELF instances out of the main draws and the
        // pick -- while its shadow / capture rig lists and the cluster RT keep it: it still
        // casts and is reflected, as on the legacy path. WHY_SELF stays a stats slot, 0.)
        if (why == WHY_OK && n.bad) why = WHY_NOPAL;
        ++mWhy[why];
        if (why != WHY_OK) continue;
        out.push_back(n.inst);
        ++mInstAdmitted;
        ++mAv[n.av].admitted;
        mAdmittedBits[n.av >> 5] |= 1u << (n.av & 31u);
    }
    // #1010 PRIORITY ORDER (WHOLE-AVATAR ARENA ADMISSION): the admitted avatar rows, your own first,
    // then by importance (height over distance) -- GRANT refuses from the end of this list when a
    // frame's new clusters do not fit the free list. (Fixed-capacity vector: reserved once.)
    mPrioOrder.clear();
    mAvCoarse = 0;
    bool prio_ok = mPrioOrder.capacity() >= kSkinAvRows;
    if (!prio_ok)
    {
        try { mPrioOrder.reserve(kSkinAvRows); prio_ok = true; }
        catch (const std::bad_alloc&) {}   // (no order: GRANT refuses nothing, the lanes race as before)
    }
    for (uint32_t a = 0; a < kMaxAvatars; ++a)
    {
        const AvTally& t = mAv[a];
        if (!t.rows) continue;
        if (t.admitted) ++mAvCluster;
        else ++mAvLegacy;   // (nothing of it drawn yet)
        if (t.admitted)
        {
            if (prio_ok) mPrioOrder.push_back(a);   // (capacity kSkinAvRows > kMaxAvatars: never reallocates)
            if (t.coarse) ++mAvCoarse;
        }
    }
    std::sort(mPrioOrder.begin(), mPrioOrder.end(), [this](uint32_t x, uint32_t y)
    {
        const AvTally& a = mAv[x];
        const AvTally& b = mAv[y];
        if (a.self != b.self) return a.self;
        if (a.importance != b.importance) return a.importance > b.importance;
        return x < y;
    });
    mPreskinNeeded = mInstAdmitted != 0;
    // (the arena follows the demand: beginFrame; at least the minimum once anything is drawn)
}

bool LLCleanClusterSkinVK::preskinWillRecord(uint32_t main_view) const
{
    // (= recordPreskin's early-outs, in its order)
    if (!mReady || main_view == kNone) return false;
    if (!mScratch.buffer || !mSkinOut.buffer || !mPrepPipe || !mSkinPipe) return false;
    return mOwner.select().viewSlice(main_view).sel_cap != 0;
}

// =============================================================================================
// per frame
// =============================================================================================
void LLCleanClusterSkinVK::harvest(uint32_t s)
{
    if (!mReadbackValid[s] || !mReadback[s].mapped) return;
    mReadbackValid[s] = false;
    std::memcpy(mLastHdr, mReadback[s].mapped, sizeof(mLastHdr));
    // The arena's demand: the clusters the pre-skin handed out (CLK_USED counts past the
    // capacity too), peak over two ~5 s windows (shrink hysteresis).
    {
        // (step 1: the arena's demand = the slots the sweep kept + the ones the cut asked for anew,
        // granted or not -- CLK_OVERFLOW of those found the free list empty)
        const uint64_t used = (uint64_t)mLastHdr[kHdrLive] + mLastHdr[kHdrAllocN];
        mOverflowLast = mLastHdr[kHdrOverflow];
        // #1010: SHORT (the frame's new clusters exceeded the free list), avatars refused whole and
        // their entries hidden; PARTIAL drops (CLK_OVERFLOW, expected 0)
        mShortLast = mLastHdr[kHdrAllocN] > mLastHdr[kHdrFreeN];
        mRefusedLast = mLastHdr[kHdrRefusedAv];
        mHiddenLast = mLastHdr[kHdrHidden];
        mHdrFresh = true;
        mArenaDemandPeakEver = std::max(mArenaDemandPeakEver, used);
        if (mFrame >= mDemandWinFrame + kShrinkFrames)
        {
            mDemandPeakPrev = mDemandPeakCur;
            mDemandPeakCur = 0;
            mWeldPeakPrev = mWeldPeakCur;
            mWeldPeakCur = 0;
            mDemandWinFrame = mFrame;
        }
        mDemandPeakCur = std::max(mDemandPeakCur, used);
        // the weld's demand (every candidate counted, inserted or dropped)
        mWeldPeakCur = std::max<uint64_t>(mWeldPeakCur, mLastHdr[kHdrWeldN]);
        mWeldDropLast = mLastHdr[kHdrWeldDrop];
    }
    // (CLK_SKIN_VERTS and CLK_REUSED are counted on the sampled frames only: one same-address
    // atomic per workgroup otherwise -- the skinned clusters latch with them)
    if (mSkinVertsSampled[s])
    {
        mSkinVertsLast = mLastHdr[kHdrSkinVerts];
        // #798: the weld's outcome on the same frame (its reason counters run on these frames only)
        mWeldDiag[0] = std::min(mLastHdr[kHdrWeldN], mScrWeldCap);
        mWeldDiag[1] = mLastHdr[kHdrWeldApplied];
        mWeldDiag[2] = mLastHdr[kHdrWeldNoPair];
        mWeldDiag[3] = mLastHdr[kHdrWeldWRej];
        mWeldDiag[4] = mLastHdr[kHdrWeldGRej];
        mWeldDiag[5] = mLastHdr[kHdrWeldBFree];
        mWeldDiag[6] = mLastHdr[kHdrWeldBFreeAv];
        mWeldDiag[7] = mLastHdr[kHdrWeldPaired];
        mWeldDiag[8] = mLastHdr[kHdrWeldNoRest];
        mWeldDiag[9] = mLastHdr[kHdrWeldNoop];
        mWeldDiag[10] = mLastHdr[kHdrWeldMulti];
        mWeldDiag[11] = mLastHdr[kHdrWeldCached];
        mWeldDiag[12] = mLastHdr[kHdrWeldInvalid];
        mWeldDiag[13] = mLastHdr[kHdrWeldSearched];
        mSkinClustersLast = mLastHdr[kHdrUsed] - std::min(mLastHdr[kHdrUsed], mLastHdr[kHdrReused]);
        mReusedLast = mLastHdr[kHdrReused];
    }
    // (step 0: the skin dispatch no longer counts its clusters -- one global atomic per workgroup)
    mSlotLiveLast = mLastHdr[kHdrLive];
    // (#1010: the new slots TAKE handed out -- the new entries of refused rows got none)
    mSlotNewLast = std::min(mLastHdr[kHdrAllocTaken], mLastHdr[kHdrFreeN]);
    mSlotKeptLast = mLastHdr[kHdrUsed] - std::min(mLastHdr[kHdrUsed], mSlotNewLast);
    mWinWeldGroups = mLastHdr[kHdrWeldDispatch];
    if (mLegacySampled[s])
    {
        mLegacyVertsLast = mLastHdr[kHdrLegacyVerts];
        mLegacyItemsLast = mLastHdr[kHdrLegacyItems];
    }
}

// #1010 THE ARENA'S LIMIT (clusters) this frame: RenderVKClusterSkinArenaMB, 0 = AUTO (1/16 of the
// device's VRAM budget, kAutoArenaMin .. kAutoArenaMax); then the VRAM ladder's depth (soft, restored
// with the rungs, the setting untouched: any rung 3/4, a third 1/2, two thirds 1/4, as the page
// pool), the binding range, and, under pressure, growth only into the VRAM room (a new buffer coexists with the old
// one until it retires) -- a failed growth holds the current size for kArenaGrowRetrySec.
uint64_t LLCleanClusterSkinVK::arenaLimit(const LLClusterFrameIn& in, uint32_t& setting_mb, bool& autosize)
{
    setting_mb = 0;
#if defined(LL_HAS_CLEAN_PIPELINE)
    setting_mb = settingArenaMB();
#endif
    autosize = setting_mb == 0;
    uint64_t bytes;
    if (autosize)
    {
        // (the budget, not the room: the room shrinks as the arena itself grows -- a feedback loop)
        bytes = in.vram_budget ? std::clamp<uint64_t>(in.vram_budget / kAutoArenaDiv, kAutoArenaMin, kAutoArenaMax)
                               : kAutoArenaNoBudget;
        mArenaAutoBytes = bytes;
    }
    else bytes = (uint64_t)std::min<uint32_t>(setting_mb, 65535u) << 20;
    mArenaSettingMB = setting_mb;
    uint64_t cap = std::max<uint64_t>(kMinSkinClusters, bytes / kArenaBytesPerCluster);
    const uint32_t rungs = in.vram_ladder;
    const uint32_t total = std::max<uint32_t>(LLVramBudgetVK::rungCount(), 3u);
    if (rungs * 3u >= total * 2u) cap /= 4u;
    else if (rungs * 3u >= total) cap /= 2u;
    else if (rungs >= 1u) cap = cap * 3u / 4u;
    // (the whole buffer is one storage binding; its word indices are 32-bit)
    const uint64_t range = std::min<uint64_t>(mOwner.caps().maxStorageBufferRange ? mOwner.caps().maxStorageBufferRange
                                                                                  : 0xFFFFFFFFull,
                                              0xFFFFFFFFull);
    while (cap > kMinSkinClusters && arenaBufferBytes(cap) > range) cap -= std::min<uint64_t>(cap - kMinSkinClusters, 4096u);
    // Under VRAM pressure (a ladder rung engaged) growth only into the room -- never into memory
    // the ladder is freeing; never below what is held (the arena does not shrink for this). Calm:
    // textures may fill the budget up to the ladder's thresholds, so the room is no gate then (a
    // real out-of-memory fails the soft allocation: held, retried).
    if (rungs >= 1u && in.vram_avail != ~0ull)
        cap = std::min<uint64_t>(cap, std::max<uint64_t>(mSkinOutClusters, in.vram_avail / kArenaBytesPerCluster));
    if (mSkinOut.buffer && mNow - mGrowFailAt < kArenaGrowRetrySec) cap = std::min<uint64_t>(cap, mSkinOutClusters);
    cap = std::max<uint64_t>(cap, kMinSkinClusters);
    mArenaLimit = cap;
    return cap;
}

// #1010 THE ARENA PRESSURE controller (llcleanclusterskinvk.h ARENA PRESSURE): P rises while the
// arena is SHORT at its limit (the cut's new clusters did not fit and it cannot grow: whole avatars
// were hidden) or over kArenaHiFill of it, and falls once the demand stayed under kArenaLoFill for a
// dwell. Runs on harvested frames only (the demand it reads is the read-back's).
void LLCleanClusterSkinVK::arenaControl(double now, uint64_t limit)
{
    if (!mHdrFresh) return;
    const uint64_t used = (uint64_t)mLastHdr[kHdrLive] + mLastHdr[kHdrAllocN];
    const bool any = limit != 0 && used != 0;
    const float fill = any ? (float)((double)used / (double)limit) : 0.f;
    // short AT THE LIMIT: the arena can no longer grow its way out (below it, it doubles next frame)
    const bool stuck = any && (mShortLast || mOverflowLast) && mSkinOutClusters >= limit;
    const float p0 = mArenaP;
    if (stuck || fill > kArenaHiFill)
    {
        mArenaUnderSince = 0.0;
        if (now - mArenaRaiseAt >= kArenaRaiseSec && mArenaP < kArenaPMax)
        {
            // (a release re-raised within 30 s: the next releases wait twice as long -- hysteresis)
            if (mArenaReleaseAt > 0.0 && mArenaChangeAt == mArenaReleaseAt && now - mArenaReleaseAt < 30.0)
                mArenaFlip = std::min<uint32_t>(mArenaFlip + 1u, 3u);
            mArenaP = std::min(kArenaPMax, mArenaP + (stuck ? 1.f : 0.25f));
            mArenaRaiseAt = now;
            mArenaChangeAt = now;
        }
    }
    else if (mArenaP > 0.f && fill < kArenaLoFill)
    {
        const double dwell = kArenaDwellSec * (double)(1u << mArenaFlip);
        if (mArenaUnderSince <= 0.0) mArenaUnderSince = now;
        else if (now - mArenaUnderSince >= dwell && now - mArenaRaiseAt >= dwell)
        {
            mArenaP = std::max(0.f, mArenaP - 0.5f);
            mArenaReleaseAt = now;
            mArenaChangeAt = now;
            mArenaUnderSince = now;   // (the next step waits a full dwell)
        }
    }
    else mArenaUnderSince = 0.0;
    if (mArenaFlip && mArenaReleaseAt > 0.0 && now - mArenaReleaseAt > 300.0 && mArenaP <= 0.f) mArenaFlip = 0;
    if ((p0 <= 0.f) != (mArenaP <= 0.f))
        LL_INFOS("Vulkan") << "Cluster LOD skin: arena pressure " << (mArenaP > 0.f ? "on" : "off") << " (demand " << used
                           << " of limit " << limit << " clusters, " << (limit * kArenaBytesPerCluster >> 20)
                           << " MB; avatars coarsen whole, least important first)" << LL_ENDL;

    // The 10 s window + the per-episode warnings.
    ++mWinFrames;
    mWinDropped += mOverflowLast;
    mWinHidden += mHiddenLast;
    mWinRefusedMax = std::max(mWinRefusedMax, mRefusedLast);
    if (mShortLast) ++mWinShortFrames;
    mWinCoarseMax = std::max(mWinCoarseMax, mAvCoarse);
    mWinPMax = std::max(mWinPMax, mArenaP);
    // PARTIAL drops (a subset of an avatar's clusters not drawn): must not happen -- warn per episode.
    if (mOverflowLast)
    {
        mDropQuietSince = 0.0;
        mDropEpisodeSum += mOverflowLast;
        if (!mDropEpisode)
        {
            mDropEpisode = true;
            LL_WARNS("Vulkan") << "Cluster LOD skin: " << mOverflowLast << " skinned clusters without an avatar row got no "
                               << "arena slot (partial drop; expected 0). Arena " << mSkinOutClusters << " / limit " << limit
                               << " clusters" << LL_ENDL;
        }
    }
    else if (mDropEpisode)
    {
        if (mDropQuietSince <= 0.0) mDropQuietSince = now;
        else if (now - mDropQuietSince >= kEpisodeQuiet)
        {
            LL_INFOS("Vulkan") << "Cluster LOD skin: partial-drop episode over (" << mDropEpisodeSum << " clusters)" << LL_ENDL;
            mDropEpisode = false;
            mDropEpisodeSum = 0;
        }
    }
    // WHOLE avatars hidden (the free list could not take a frame's new clusters): per episode.
    if (mRefusedLast)
    {
        mRefuseQuietSince = 0.0;
        mRefuseEpisodeHidden += mHiddenLast;
        mRefuseEpisodePeak = std::max(mRefuseEpisodePeak, mRefusedLast);
        if (!mRefuseEpisode && mSkinOutClusters < limit)
        {
            // (still growing towards its limit -- a login / teleport into a crowd: not an episode)
            mRefuseEpisodeHidden = 0;
            mRefuseEpisodePeak = 0;
            if (now - mGrowHideLogAt >= 10.0)
            {
                mGrowHideLogAt = now;
                LL_INFOS("Vulkan") << "Cluster LOD skin: arena growing (" << mSkinOutClusters << " of limit " << limit
                                   << " clusters): " << mRefusedLast << " avatars hidden whole this frame" << LL_ENDL;
            }
        }
        else if (!mRefuseEpisode)
        {
            mRefuseEpisode = true;
            mRefuseEpisodeStart = now;
            LL_WARNS("Vulkan") << "Cluster LOD skin: skinned arena full (" << mSkinOutClusters << " of limit " << limit
                               << " clusters, " << (limit * kArenaBytesPerCluster >> 20) << " MB"
                               << (mArenaSettingMB ? " RenderVKClusterSkinArenaMB" : " auto") << "): " << mRefusedLast
                               << " avatars hidden WHOLE this frame (least important first), arena pressure " << mArenaP
                               << " -- coarsening whole avatars to fit" << LL_ENDL;
        }
    }
    else if (mRefuseEpisode)
    {
        if (mRefuseQuietSince <= 0.0) mRefuseQuietSince = now;
        else if (now - mRefuseQuietSince >= kEpisodeQuiet)
        {
            LL_INFOS("Vulkan") << "Cluster LOD skin: arena-full episode over after "
                               << (float)(mRefuseQuietSince - mRefuseEpisodeStart) << " s (peak " << mRefuseEpisodePeak
                               << " avatars hidden in a frame, " << mRefuseEpisodeHidden << " cluster-frames hidden; "
                               << "arena pressure now " << mArenaP << ")" << LL_ENDL;
            mRefuseEpisode = false;
            mRefuseEpisodeHidden = 0;
            mRefuseEpisodePeak = 0;
        }
    }
}

void LLCleanClusterSkinVK::beginFrame(const LLClusterFrameIn& in)
{
    mSlot = in.slot % kSlots;
    mFrame = in.frame;
    mFrame32 = (uint32_t)in.frame;
    mBoundsRecorded = false;
    if (!mReady) return;
    harvest(mSlot);
    LLCleanClusterSelectVK& sel = mOwner.select();
    mInstCount = sel.instanceCount();
    mMapCap = sel.arenaEntries();
    if (mIn.skin_count) ensureStretch(mIn.skin_count);
    // THE SKINNED ARENA follows the demand (2026-10-14): the peak handed-out clusters + a quarter
    // (+ 1024), doubled at once when SHORT (#1010: the frame's new clusters exceeded the free list;
    // whole avatars were hidden that frame), never past the main slice's map (nothing beyond it is
    // skinned) or the LIMIT (#1010: VRAM-derived, ladder-scaled, room-gated) -- which the ARENA
    // PRESSURE fits the cut to (llcleanclusterskinvk.h ARENA PRESSURE).
    mNow = in.now;
    if (mPreskinNeeded)
    {
        uint32_t arena_mb = 0;
        bool autosize = true;
        const uint64_t limit = arenaLimit(in, arena_mb, autosize);
        const uint64_t peak = std::max(mDemandPeakCur, mDemandPeakPrev);
        uint64_t want = peak + (in.vram_ladder ? peak / 8u : peak / 4u) + 1024u;
        if (mShortLast || mOverflowLast) want = std::max<uint64_t>(want, mSkinOutClusters * 2u);
        if (mMapCap) want = std::min<uint64_t>(want, std::max<uint64_t>(mMapCap, kMinSkinClusters));
        mWantClusters = std::clamp<uint64_t>(want, kMinSkinClusters, limit);
        if (!in.offscreen) arenaControl(in.now, limit);
    }
    else
    {
        mWantClusters = 0;
        if (!in.offscreen) arenaControl(in.now, 0);
    }
    mHdrFresh = false;
    ensureSkinOut(mWantClusters);   // (grows at once, shrinks with hysteresis)
    mOwner.control().setArenaPressure(mArenaP);   // (the next gather's per-avatar arena term)
    const uint32_t weld = mPreskinNeeded ? weldCapWanted() : 0u;
#if defined(LL_HAS_CLEAN_PIPELINE)
    if (weld && mWeldDropLast && mScrWeldCap >= std::clamp<uint64_t>(settingClusterWeldMax(), kWeldCapStep, kWeldCapMax))
        LL_WARNS_ONCE("Vulkan") << "Cluster LOD skin: weld at its ceiling (RenderVKClusterWeldMaxVerts "
                                << settingClusterWeldMax() << ", demand " << std::max(mWeldPeakCur, mWeldPeakPrev)
                                << "): the avatars that overflowed are not welded those frames" << LL_ENDL;
#endif
    if (!ensureScratch(std::max(mInstCount, 1u), std::max(mMapCap, 1u), std::max(mRenderInstWords, 1u), weld))
    {
        // (the scratch that exists stays registered: the selection then skips the margins
        // of instances past its capacity -- clRigMargin bounds-checks -- and the pre-skin the
        // map entries past it; a weld that does not fit is skipped below)
    }
    // #798: the weld's rest-pose pairing space -- rows for this frame's admitted avatars
    bool rest = false;
#if defined(LL_HAS_CLEAN_PIPELINE)
    rest = settingWeld() && settingWeldRest();
#endif
    restPlan(rest);
    writeSet(mSlot);
#if defined(LL_HAS_CLEAN_PIPELINE)
    // #798 WELD PROBE (one-shot): 1 = the avatar nearest the view centre, 2 = ours
    {
        static LLCachedControl<U32> s_probe(gSavedSettings, "RenderVKClusterWeldProbe", 0U);
        if ((U32)s_probe != 0u)
        {
            const uint32_t mode = (U32)s_probe;
            gSavedSettings.setU32("RenderVKClusterWeldProbe", 0u);
            weldProbe(mode);
        }
    }
#endif
}

void LLCleanClusterSkinVK::barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 ss, VkAccessFlags2 sa,
                                   VkPipelineStageFlags2 ds, VkAccessFlags2 da) const
{
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = ss; mb.srcAccessMask = sa; mb.dstStageMask = ds; mb.dstAccessMask = da;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1;
    d.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &d);
    ++mBarrierCount;
}

void LLCleanClusterSkinVK::recordBounds(VkCommandBuffer cmd)
{
    if (mBoundsRecorded || !mReady || !mPrepPipe || !mScratch.buffer || !mStretch.buffer || mIn.skin_count == 0
        || !mIn.joints || !mIn.joints->buffer)
        return;
    const uint32_t insts = std::min(mInstCount, mScrInstCap);
    VkPipelineStageFlags2 readers = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT
                                  | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT
                                  | VK_PIPELINE_STAGE_2_COPY_BIT;
#ifdef VK_EXT_mesh_shader
    if (mMesh) readers |= VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT | VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT;
#endif
    // WAR: last frame's readers of the scratch / stretch table -> this frame's writes.
    barrier(cmd, readers, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_CLEAR_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    vkCmdFillBuffer(cmd, mScratch.buffer, (VkDeviceSize)mOwnedBase * 4, (VkDeviceSize)mScrOwnedWords * 4, 0u);
    // #611 layered-avatar bits (1024 slots: no longer in the push block) -> the scratch, read by mode 0
    vkCmdUpdateBuffer(cmd, mScratch.buffer, (VkDeviceSize)mLayerBase * 4, sizeof(mLayerBits), mLayerBits);
    barrier(cmd, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

    const VkDescriptorSet sets[2] = {mOwner.set(mSlot), mSets[mSlot]};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPrepPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPrepLayout, 0, 2, sets, 0, nullptr);
    PrepPush p{};
    p.skin_count = mIn.skin_count;
    p.avatar_count = mIn.avatar_count;
    p.layer_scale = 1.f;
#if defined(LL_HAS_CLEAN_PIPELINE)
    p.layer_scale = std::clamp((float)settingLayerScale(), 1.f, 16.f);
#endif
    p.meta_words = kSkinMetaBytes / 4;
    p.owned_base = mOwnedBase;
    p.owned_words = std::min(mScrOwnedWords, std::max(mRenderInstWords, 1u));
    p.layer_base = mLayerBase;
    // mode 0: the joint-stretch table (one workgroup per skin).
    p.mode = 0;
    p.count = std::min(mIn.skin_count, mStretchRows);
    vkCmdPushConstants(cmd, mPrepLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
    vkCmdDispatch(cmd, std::max(p.count, 1u), 1, 1);
    // mode 1: the posed-bound margins (one workgroup per ClusterInstance).
    if (insts)
    {
        p.mode = 1;
        p.count = insts;
        vkCmdPushConstants(cmd, mPrepLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        const uint32_t gx = std::min(insts, kGridX);
        vkCmdDispatch(cmd, gx, (insts + gx - 1) / gx, 1);
    }
    // -> the selection (margins, stretch) and the legacy-stats pass (owned bits).
    barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    // Valid only when every instance got its margin (a scratch growth that failed leaves some
    // without): otherwise the selection runs without them -- an instance drawn for real is then
    // never culled and takes LOD0 (clean_cluster_select.comp), never a hole.
    mBoundsRecorded = insts == mInstCount;
}

void LLCleanClusterSkinVK::frameDropped()
{
    for (bool& v : mReadbackValid) v = false;
    // (#798: that recording may have carried rest-table uploads -- upload the whole mirror again)
    mRestAll = true;
    mRestHdrDirty = true;
    // (step 1: that recording may have carried the arena's slot-meta fill -- redo it; at worst
    // every slot is re-skinned once more)
    mArenaReset = true;
    mBoundsRecorded = false;
}

void LLCleanClusterSkinVK::recordPreskin(VkCommandBuffer cmd, uint32_t main_view, const LLClusterSkinPrevIn& prev,
                                         const std::function<void(VkCommandBuffer, const char*)>& mark)
{
    // STEP 0 (2026-10-14, measure first): the window c_cl_preskin_begin -> c_cluster_preskin is
    // split by sub-marks (each = the GPU time since the previous mark; the renderer registers them
    // as class-4 links so the rigged-time span stays one chain):
    //   c_cl_sk_prep    WAR barrier + fills + ARGS dispatch            (prep)
    //   c_cl_sk_skin    barrier + the cluster pre-skin (+ weld INSERT, inside the same dispatch)
    //   c_cl_sk_legacy  barrier + the sampled legacy-verts statistic (every 64th frame)
    //   c_cl_sk_weld    barrier + the weld RESOLVE dispatch (mode 0; over the weld CAPACITY)
    //   c_cl_sk_apply   barrier + the weld APPLY dispatch (mode 1)
    //   c_cluster_preskin (renderer) barriers + the header readback copy + the barrier to the draws
    // and the stats line counts the window's dispatches / barriers / transfer commands.
    const auto mk = [&](const char* n) { if (mark) mark(cmd, n); };
    const uint32_t bar0 = mBarrierCount;
    uint32_t disp = 0, xfers = 0;
    struct WinTally
    {
        LLCleanClusterSkinVK& self; const uint32_t& b0; uint32_t& d; uint32_t& x;
        ~WinTally() { self.mWinDispatches = d; self.mWinXfers = x; self.mWinBarriers = self.mBarrierCount - b0; }
    } tally{*this, bar0, disp, xfers};
    VkPipelineStageFlags2 gfx = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
#ifdef VK_EXT_mesh_shader
    if (mMesh) gfx |= VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT | VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT;
#endif
    // No pre-skin this frame while the main view draws: the scratch header and map still hold
    // an older frame's, so a rigged entry could read a stale skinned slot. Zero the map
    // capacity instead (clSkinnedCluster then answers CL_NONE: rest pose, debug overlay only --
    // an admitted avatar always has a pre-skin when the pipes and buffers exist).
    const auto invalidateMap = [&]()
    {
        if (!mScratch.buffer) return;
        barrier(cmd, gfx | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        vkCmdFillBuffer(cmd, mScratch.buffer, (VkDeviceSize)kHdrMapCap * 4, 4, 0u);
        ++xfers;
        barrier(cmd, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, gfx | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    };
    if (!mReady || main_view == kNone) return;
    if (!mScratch.buffer || !mSkinOut.buffer || !mPrepPipe || !mSkinPipe) { invalidateMap(); return; }
    const LLCleanClusterSelectVK::ViewSlice vs = mOwner.select().viewSlice(main_view);
    if (!vs.sel_cap) { invalidateMap(); return; }
    // WAR: last frame's draws / weld reads of the arena + map, its header readback copy.
    // (+ COPY: this frame's pose-generation upload, vkCmdUpdateBuffer)
    barrier(cmd, gfx | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_CLEAR_BIT | VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

    // Weld this frame: on, admitted avatars, and the scratch carries the grid.
    float mm = 0.2f, deg = 60.f, gap = 2.f, wm = 0.8f;
#if defined(LL_HAS_CLEAN_PIPELINE)
    {
        static LLCachedControl<F32> s_mm(gSavedSettings, "RenderVKRigWeldDistance", 0.2f);
        static LLCachedControl<F32> s_deg(gSavedSettings, "RenderVKRigWeldNormalAngle", 60.f);
        static LLCachedControl<F32> s_gap(gSavedSettings, "RenderVKRigWeldMaxGap", 2.f);
        static LLCachedControl<F32> s_wmatch(gSavedSettings, "RenderVKRigWeldWeightMatch", 0.8f);
        mm = s_mm; deg = s_deg; gap = s_gap; wm = s_wmatch;
    }
#endif
    const bool weld = mPreskinNeeded && weldCapWanted() != 0 && mScrWeldCap != 0 && mScrWeldCells != 0 && mWeldPipe;
    // STRUCTURE (2026-10-14 step 0): the frame's clears (per-avatar skinned boxes, weld cell heads,
    // weld drop bits) are done by the ARGS dispatch itself (was: three fills + their own barrier);
    // the sampled legacy statistic runs right after ARGS, unordered with the skin (disjoint words);
    // the skin -> weld barrier exists only with the weld, whose two passes are INDIRECT over the
    // entries written (CLK_WELD_DISPATCH; were over the whole capacity: up to 32768 groups each);
    // the header readback rides on the final barrier. Weld off: 3 barriers (were 6).

    const VkDescriptorSet sets[2] = {mOwner.set(mSlot), mSets[mSlot]};
    // ARGS: the header of this frame + the indirect dispatch (one group per selected entry) + clears.
    PrepPush p{};
    p.mode = 3;
    p.skin_count = mIn.skin_count;
    p.avatar_count = mIn.avatar_count;
    p.meta_words = kSkinMetaBytes / 4;
    p.owned_base = mOwnedBase;
    p.owned_words = std::min(mScrOwnedWords, std::max(mRenderInstWords, 1u));
    p.view = main_view;
    p.count = mScrMapCap;   // mode 3: the map's capacity (entries past it are not skinned)
    p.map_base = mMapBase;
    p.frame = mFrame32;
    p.cap = (uint32_t)std::min<uint64_t>(mSkinOutClusters, 0xFFFFFFFFull);
    p.eye[0] = mIn.eye[0]; p.eye[1] = mIn.eye[1]; p.eye[2] = mIn.eye[2];
    // SKINNED BOUNDS (#748 perf pass): the per-avatar union region starts empty (0 = no cluster).
    p.clr_av_base = (uint32_t)avBoxWord(mSkinOutClusters);
    // HELD POSES (2026-10-14 step 3): every row's pose generation -- unchanged only while the
    // renderer PROVES the row's pose held frame after frame (a frame without a pre-skin, or no
    // proof: a new value) -- uploaded after the WAR barrier; the ARGS -> SWEEP barrier orders it.
    {
        const bool chain = mPoseGenFrame != ~0ull && mPoseGenFrame + 1u == mFrame && prev.pose_clean;
        mCharsHeld = mCharsSkinned = 0;
        for (uint32_t r = 0; r < kSkinAvRows; ++r)
        {
            // (#798: a slot whose rest row changed -- built, rebuilt, freed -- has new weld pairs:
            // its whole row re-skins and re-welds, reused clusters would keep the old welds)
            const bool clean = chain && r < kMaxAvatars && ((prev.pose_clean[r >> 5] >> (r & 31u)) & 1u) != 0u
                            && ((mRestTouched[r >> 5] >> (r & 31u)) & 1u) == 0u;
            if (!clean || mPoseGen[r] == 0u)
            {
                mPoseGenCounter = (mPoseGenCounter + 1u) & 0x7FFFFFFFu;
                if (mPoseGenCounter == 0u) mPoseGenCounter = 1u;
                mPoseGen[r] = mPoseGenCounter;   // (never CL_NONE, never 0)
            }
            if (r < kMaxAvatars && avatarSwitched(r)) ++(clean ? mCharsHeld : mCharsSkinned);
        }
        mPoseGenFrame = mFrame;
        vkCmdUpdateBuffer(cmd, mSkinOut.buffer, (VkDeviceSize)slotGenWord(mSkinOutClusters) * 4, sizeof(mPoseGen), mPoseGen);
        ++xfers;
        // #826 WELD PAIR CACHE: rows to re-pair -- a changed rest row (its pairing space moved),
        // every row when the pairing parameters changed (eps, weight match, space) or the cache was
        // off (its stored pairs were not maintained meanwhile). Zero words are uploaded only to clear
        // the last non-zero upload.
        bool pair_on = true;
#if defined(LL_HAS_CLEAN_PIPELINE)
        pair_on = settingWeldPairCache();
#endif
        pair_on = pair_on && weld;
        uint32_t ph = 0x811C9DC5u;
        for (const uint32_t v : {(uint32_t)std::lround(std::clamp(mm, 0.05f, 10.f) * 1.0e5f),
                                 (uint32_t)std::lround(std::clamp(wm, 0.f, 1.f) * 1.0e5f), mRestOn ? 1u : 0u})
            ph = (ph ^ v) * 0x01000193u;
        const bool all = pair_on && (!mPairOn || ph != mPairParamHash);
        mPairOn = pair_on;
        mPairParamHash = ph;
        bool any = false;
        for (uint32_t w = 0; w < (uint32_t)kRepairWords; ++w)
        {
            mRepair[w] = all ? 0xFFFFFFFFu : (w < kAvatarSlotWords ? mRestTouched[w] : 0u);
            any = any || mRepair[w] != 0u;
        }
        if (any || mRepairUploaded || mArenaReset)   // (a (re)created arena: its words are undefined)
        {
            vkCmdUpdateBuffer(cmd, mSkinOut.buffer, (VkDeviceSize)repairWord(mSkinOutClusters) * 4, sizeof(mRepair), mRepair);
            ++xfers;
        }
        mRepairUploaded = any;
        std::memset(mRestTouched, 0, sizeof(mRestTouched));
    }
    // #798 REST TABLE: this frame's row / header changes (same ordering as the generations above:
    // after the WAR barrier, before the ARGS -> SWEEP barrier the skin's reads come after).
    restUpload(cmd, xfers);
    // #1010 PRIORITY ORDER (admit(): own avatar first, then by importance) -> the scratch's PRIORITY
    // region {n, rows}; ARGS (push layer_base) clears its per-row words beside it and points the
    // header at it. Same ordering as the uploads above.
    p.layer_base = mLayerBase;
    {
        uint32_t buf[1u + kSkinAvRows];
        const uint32_t n = (uint32_t)std::min<size_t>(mPrioOrder.size(), kSkinAvRows);
        buf[0] = n;
        if (n) std::memcpy(buf + 1, mPrioOrder.data(), (size_t)n * 4u);
        vkCmdUpdateBuffer(cmd, mScratch.buffer, (VkDeviceSize)mPrioBase * 4, (VkDeviceSize)(1u + n) * 4u, buf);
        ++xfers;
    }
    p.clr_av_words = (uint32_t)(kAvBoxBytes / 4);
    p.clr_weld_heads = weld ? mWeldBase : 0u;
    p.clr_weld_cells = weld ? mScrWeldCells : 0u;
    const uint32_t clr_words = std::max({p.clr_av_words, p.clr_weld_cells, (uint32_t)slotHashWords(mSkinOutClusters)});
    // STABLE SLOTS (step 1): a (re)created arena holds no slot yet -- every meta word CL_NONE (free).
    // (After the WAR barrier above; the ARGS -> SWEEP barrier below orders it before the sweep.)
    if (mArenaReset)
    {
        vkCmdFillBuffer(cmd, mSkinOut.buffer, (VkDeviceSize)slotMetaWord(mSkinOutClusters) * 4,
                        (VkDeviceSize)mSkinOutClusters * kSlotMetaWords * 4, 0xFFFFFFFFu);
        ++xfers;
        // #826: no row has a pairing (CL_NONE). (The repair bits were uploaded above whatever their
        // value: a (re)created arena's words are undefined. Every row re-pairs anyway: every slot is new.)
        vkCmdFillBuffer(cmd, mSkinOut.buffer, (VkDeviceSize)rowPairWord(mSkinOutClusters) * 4,
                        (VkDeviceSize)kSkinAvRows * 2u * 4u, 0xFFFFFFFFu);
        ++xfers;
        mArenaReset = false;
        ++mArenaResets;
    }
    const uint32_t args_groups = std::clamp<uint32_t>((clr_words + 64u * 16u - 1u) / (64u * 16u), 1u, 4096u);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPrepPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPrepLayout, 0, 2, sets, 0, nullptr);
    vkCmdPushConstants(cmd, mPrepLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
    vkCmdDispatch(cmd, args_groups, 1, 1);
    ++disp;
    mk("c_cl_sk_prep");
    barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);

    // STABLE SLOTS (2026-10-14 step 1): SWEEP (one lane per arena slot: live slots -> the key hash,
    // the rest -> the free list), then ALLOC (one lane per selected entry: its kept slot or a free
    // one -> its map word). The skin below reads the map; every reader addresses the arena by it
    // as before -- only the slot numbers stay put while a cluster stays in the cut.
    {
        p.mode = 4;
        p.grace = kSlotGrace;
        const uint32_t groups = (uint32_t)((mSkinOutClusters + 63u) / 64u);
        const uint32_t gx = std::min(std::max(groups, 1u), kGridX);
        vkCmdPushConstants(cmd, mPrepLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(cmd, gx, (std::max(groups, 1u) + gx - 1u) / gx, 1);
        ++disp;
        // CHARACTER SPACE (step 2): every character row's frame (pelvis, this and the previous
        // frame, eye-relative) -- unordered with the sweep (disjoint words), read by the skin and
        // every arena reader after the barriers below.
        p.mode = 6;
        p.count = prev.joints ? prev.joints_rows : 0u;
        p.prev_joints_lo = (uint32_t)(prev.joints & 0xFFFFFFFFull);
        p.prev_joints_hi = (uint32_t)(prev.joints >> 32);
        vkCmdPushConstants(cmd, mPrepLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(cmd, (kSkinAvRows + 63u) / 64u, 1, 1);
        ++disp;
        mk("c_cl_sk_sweep");
        barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        p.mode = 5;
        vkCmdPushConstants(cmd, mPrepLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatchIndirect(cmd, mScratch.buffer, (VkDeviceSize)kHdrAllocDispatch * 4);
        ++disp;
        // #1010 WHOLE-AVATAR ARENA ADMISSION: GRANT (one workgroup: when the frame's new entries
        // exceed the free list, the rows past it in the priority order are refused), then TAKE (one
        // lane per entry: free slots for the new entries of the rows not refused, every entry of a
        // refused row -> CL_NONE). ALLOC above only kept slots and counted the new ones.
        barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        p.mode = 7;
        vkCmdPushConstants(cmd, mPrepLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(cmd, 1, 1, 1);
        ++disp;
        barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        p.mode = 8;
        vkCmdPushConstants(cmd, mPrepLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatchIndirect(cmd, mScratch.buffer, (VkDeviceSize)kHdrAllocDispatch * 4);
        ++disp;
        mk("c_cl_sk_alloc");
        barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    }

    // STATS: the legacy pre-skin vertices of the instances the cluster path owns (the owned
    // bits were set by the bounds pass; the legacy work list is complete: its pre-skin ran).
    // (#748 perf pass 3: a statistic only -- one dispatch over the whole legacy work list capacity
    // every frame, inside c_cluster_preskin -- sampled every 64th frame, latched at harvest.
    // Step 0: before the skin, no barrier between them: CLK_LEGACY_* are its only writes.)
    mLegacySampled[mSlot] = false;
    if (mPreskinNeeded && mBoundsRecorded && mIn.work && mIn.work->buffer && mIn.rig_alloc && mIn.rig_alloc->buffer
        && mIn.work_cap && (mFrame32 & 63u) == 0u)
    {
        mLegacySampled[mSlot] = true;
        p.mode = 2;
        p.count = mIn.work_cap;
        vkCmdPushConstants(cmd, mPrepLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(cmd, (mIn.work_cap + 63) / 64, 1, 1);
        ++disp;
        mk("c_cl_sk_legacy");
    }

    // THE CLUSTER PRE-SKIN (+ weld insert).
    const float eps = std::clamp(mm, 0.05f, 10.f) * 0.001f;
    SkinPush sp{};
    sp.skin_count = mIn.skin_count;
    sp.weld_on = weld ? 1u : 0u;
    sp.weld_cap = mScrWeldCap;
    sp.weld_base = mWeldBase;
    sp.cell_mask = mScrWeldCells ? mScrWeldCells - 1u : 0u;
    sp.cell = 2.f * eps;
    sp.meta_words = kSkinMetaBytes / 4;
    // Previous positions (integration 2a + 2c: the motion pre-pass draws skinned clusters). The
    // renderer's previous-frame palettes when it built them this frame (LAST frame's joints,
    // relative to THIS frame's eye: the legacy pre-skin's u_prev 1), else previous = current
    // (camera-only motion) on a motion frame. No motion frame: nothing reads them.
    sp.prev_mode = prev.palette ? 1u : (prev.motion ? 2u : 0u);
    sp.prev_lo = (uint32_t)(prev.palette & 0xFFFFFFFFull);
    sp.prev_hi = (uint32_t)(prev.palette >> 32);
    sp.prev_cap = prev.palette ? prev.palette_cap : 0u;
    // (the weld moves a boundary vertex by at most its max gap after the box is taken)
    sp.box_pad = weld ? std::clamp(gap, 0.1f, 50.f) * 0.01f : 0.f;
    // (the skinned-vertex count, one global atomic per workgroup: sampled frames only, latched)
    mSkinVertsSampled[mSlot] = (mFrame32 & 15u) == 0u;
    sp.stats_on = mSkinVertsSampled[mSlot] ? 1u : 0u;
    mReuseOn = true;
#if defined(LL_HAS_CLEAN_PIPELINE)
    mReuseOn = settingReuse();
#endif
    sp.reuse_on = mReuseOn ? 1u : 0u;
    sp.rest_on = mRestOn ? 1u : 0u;
    sp.pair_on = mPairOn ? 1u : 0u;
    mPrevMode = sp.prev_mode;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mSkinPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mSkinLayout, 0, 2, sets, 0, nullptr);
    vkCmdPushConstants(cmd, mSkinLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(sp), &sp);
    vkCmdDispatchIndirect(cmd, mScratch.buffer, (VkDeviceSize)kHdrDispatch * 4);
    ++disp;
    mk("c_cl_sk_skin");

    // WELD on the cut: weld (results), then apply -- indirect over the entries written.
    if (weld)
    {
        barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                    | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
        WeldPush wp{};
        wp.cap = mScrWeldCap;
        wp.weld_base = mWeldBase;
        wp.cell_mask = mScrWeldCells - 1u;
        wp.cell = 2.f * eps;
        wp.eps = eps;
        wp.cos_n = std::cos(std::clamp(deg, 0.f, 180.f) * 0.01745329252f);
        wp.max_gap = std::clamp(gap, 0.1f, 50.f) * 0.01f;
        wp.wmatch = std::clamp(wm, 0.f, 1.f);
        wp.stats_on = sp.stats_on;
        wp.slot_stats = settingDebugWeld() ? 1u : 0u;   // (#798 RenderVKClusterDebug 6: per-slot outcomes)
        const VkDescriptorSet cset = mOwner.set(mSlot);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mWeldPipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mWeldLayout, 0, 1, &cset, 0, nullptr);
        for (uint32_t mode = 0; mode < 2; ++mode)
        {
            wp.mode = mode;
            if (mode == 1)
                barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
            vkCmdPushConstants(cmd, mWeldLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(wp), &wp);
            vkCmdDispatchIndirect(cmd, mScratch.buffer, (VkDeviceSize)kHdrWeldDispatch * 4);
            ++disp;
            mk(mode == 0 ? "c_cl_sk_weld" : "c_cl_sk_apply");
        }
    }

    // -> the cluster draws (vertex / task / mesh) and the header readback (stats, harvested when
    // the slot comes round), one barrier. Under frame pipelining this records in the front-end
    // segment and the graphics join orders it (a compute-only family drops the graphics stages,
    // vk sanitizer); on graphics it is the barrier itself.
    barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            gfx | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
    if (mReadback[mSlot].buffer)
    {
        const VkBufferCopy c{0, 0, (VkDeviceSize)kHdrWords * 4};
        vkCmdCopyBuffer(cmd, mScratch.buffer, mReadback[mSlot].buffer, 1, &c);
        ++xfers;
        mReadbackValid[mSlot] = true;
    }
    mPreskinFrame = mFrame;   // (#702 integration: the shared rig lists / pick may read the slice)
}

// =============================================================================================
// stats
// =============================================================================================

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

void LLCleanClusterSkinVK::memSummary(std::string& out) const
{
    out += "skin";
    // arena: the peak clusters the pre-skin handed out / capacity (2 KiB per cluster)
    memItem(out, "arena", (double)std::max(mDemandPeakCur, mDemandPeakPrev) * kArenaBytesPerCluster,
            mSkinOut.buffer ? mSkinOut.size : 0);
    {   // (#1010: the arena's allocated high-water and its limit)
        char ab[64];
        std::snprintf(ab, sizeof(ab), " (peak %.1f lim %.1f)", (double)mArenaPeakBytes / 1048576.0,
                      (double)(mArenaLimit * kArenaBytesPerCluster) / 1048576.0);
        out += ab;
    }
    // scratch: header + margins + map + owned bits + weld (cells + entries + results); live = the
    // weld's peak demand share
    const double weld_live = (double)std::max(mWeldPeakCur, mWeldPeakPrev) * (kWeldEntWords + kWeldResWords) * 4.0
                           + (double)mScrWeldCells * 4.0;
    char b[96];
    std::snprintf(b, sizeof(b), " scratch %.1f (weld %.1f/%.1f, map %.1f)", (double)(mScratch.buffer ? mScratch.size : 0) / 1048576.0,
                  weld_live / 1048576.0,
                  ((double)mScrWeldCells + (double)mScrWeldCap * (kWeldEntWords + kWeldResWords)) * 4.0 / 1048576.0,
                  (double)mScrMapCap * 4.0 / 1048576.0);
    out += b;
    memItem(out, "stretch", -1.0, mStretch.buffer ? mStretch.size : 0);
    // #798 rest table: header + the live rows / the rows held
    memItem(out, "rest", ((double)kMaxAvatars + (double)mRestLive * kRestRowWords) * 4.0, mRest.buffer ? mRest.size : 0);
    std::snprintf(b, sizeof(b), " (peak %.1f)", (double)mPeakBytes / 1048576.0);
    out += b;
}

void LLCleanClusterSkinVK::statsLine(std::string& out) const
{
    const uint32_t* h = mLastHdr;
    out += "skin avatars drawn " + std::to_string(mAvCluster) + " none-yet " + std::to_string(mAvLegacy)
         + "; meshes drawn " + std::to_string(mInstAdmitted) + " not drawn (loading " + std::to_string(mWhy[WHY_PENDING])
         + ", off " + std::to_string(mWhy[WHY_OFF]) + ", prepass " + std::to_string(mWhy[WHY_PREPASS]) + ", gate "
         + std::to_string(mWhy[WHY_GATE]) + ", nopal " + std::to_string(mWhy[WHY_NOPAL]) + ", self "
         + std::to_string(mWhy[WHY_SELF]) + "); gpu skinned clusters "
         + std::to_string(mSkinClustersLast) + " verts " + std::to_string(mSkinVertsLast) + " (sampled) vs legacy verts "
         + std::to_string(mLegacyVertsLast) + " (" + std::to_string(mLegacyItemsLast) + " items, sampled) overflow "
         + std::to_string(h[kHdrOverflow]) + " weld " + std::to_string(std::min(h[kHdrWeldN], mScrWeldCap)) + "/"
         + std::to_string(h[kHdrWeldApplied]) + " (drop " + std::to_string(h[kHdrWeldDrop]) + ", skipped "
         + std::to_string(h[kHdrWeldSkip]) + " of overflowing avatars; demand " + std::to_string(h[kHdrWeldN])
         + " peak " + std::to_string(std::max(mWeldPeakCur, mWeldPeakPrev)) + " cap " + std::to_string(mScrWeldCap)
         // #798 seam diagnostics (one sampled frame): the pairing space (rest pose / bind space), the
         // candidates, those PAIRED there (a partner within eps with matching weights: what the rest
         // pose decided) and WELDED (applied: the partner within the max gap in this pose) / unwelded
         // because no other mesh had a vertex within eps, its skin weights differed, or it was past
         // the max gap; boundary vertices of avatars without a rest row yet (not inserted);
         // candidates of BORDER-FREE clusters (the face's open border was simplified there: their
         // partners lose vertices -- a sawtooth seam) and the avatars showing any
         // (#798 resolve) NOOP = every partner already seamless (left alone), MULTI = more than one
         // partner (other meshes, or distinct vertices of one within eps)
         + "; seam " + (mRestOn ? "rest" : "bind") + " " + std::to_string(mWeldDiag[0]) + " cand paired "
         + std::to_string(mWeldDiag[7]) + " welded " + std::to_string(mWeldDiag[1]) + " noop " + std::to_string(mWeldDiag[9])
         + " multi " + std::to_string(mWeldDiag[10]) + " unwelded nopair "
         + std::to_string(mWeldDiag[2]) + " weights " + std::to_string(mWeldDiag[3]) + " gap " + std::to_string(mWeldDiag[4])
         + " norest " + std::to_string(mWeldDiag[8])
         + " border-free " + std::to_string(mWeldDiag[5]) + " on " + std::to_string(mWeldDiag[6]) + " av"
         // #826 WELD PAIR CACHE (the same sampled frame): entries re-applying stored pairs (no search),
         // of them with a partner that left its slot / the cut, and entries of rows that searched
         + "; pairs " + (mPairOn ? "cached " + std::to_string(mWeldDiag[11]) + " (invalid " + std::to_string(mWeldDiag[12])
                                    + ") searched " + std::to_string(mWeldDiag[13])
                                  : std::string("off"))
         // the rest table (live / cap / peak rows = avatars) and its builds of the last ~30 s by
         // trigger (new row, new slot owner, shape, joint overrides), slots without an avatar
         + "; rest rows " + std::to_string(mRestLive) + "/" + std::to_string(mRestCap) + " peak "
         + std::to_string(mRestPeak) + " builds new " + std::to_string(mRestBuildsLast[0]) + " owner "
         + std::to_string(mRestBuildsLast[1]) + " shape " + std::to_string(mRestBuildsLast[2]) + " bones "
         + std::to_string(mRestBuildsLast[3]) + " noav " + std::to_string(mRestNoAvLast)
         + "); arena want "
         + std::to_string(mWantClusters) + " of " + std::to_string(mSkinOutClusters) + " clusters (demand peak "
         + std::to_string(std::max(mDemandPeakCur, mDemandPeakPrev)) + "), bounds "
         + (mBoundsRecorded ? "posed" : "rest") + ", prev " + (mPrevMode == 1 ? "pal" : mPrevMode == 2 ? "cur" : "off")
         + ", held " + std::to_string(heldBytes() >> 10) + " KiB (peak "
         + std::to_string(mPeakBytes >> 10) + ")"
         // step 0: the c_cl_preskin_begin -> c_cluster_preskin window's commands (last frame)
         // step 1: stable slots -- live after the sweep, kept in the cut / new this frame, the peak
         // demand, arena (re)creations (each re-skins everything once)
         + "; slots live " + std::to_string(mSlotLiveLast) + " kept " + std::to_string(mSlotKeptLast) + " new "
         + std::to_string(mSlotNewLast) + " demand peak " + std::to_string(std::max(mDemandPeakCur, mDemandPeakPrev)) + " cap " + std::to_string(mSkinOutClusters)
         + " resets " + std::to_string(mArenaResets)
         // #1010: the limit, the pressure, avatars coarsened / hidden whole (last frame), partial drops
         + "; arena limit " + std::to_string(mArenaLimit) + (mArenaSettingMB ? " (setting)" : " (auto)") + " pressure "
         + std::to_string(mArenaP) + " coarse av " + std::to_string(mAvCoarse) + " short " + (mShortLast ? "1" : "0")
         + " hidden av " + std::to_string(mRefusedLast) + " (" + std::to_string(mHiddenLast) + " clusters) partial "
         + std::to_string(mOverflowLast)
         // step 3: clusters skinned this frame vs reused (held pose), admitted characters held / re-skinned
         + "; reuse " + (mReuseOn ? "on" : "off") + " skinned " + std::to_string(mSkinClustersLast) + " reused "
         + std::to_string(mReusedLast) + " chars held " + std::to_string(mCharsHeld) + " skinned "
         + std::to_string(mCharsSkinned)
         + "; window disp " + std::to_string(mWinDispatches) + " barriers " + std::to_string(mWinBarriers)
         + " xfers " + std::to_string(mWinXfers) + " weld groups " + std::to_string(mWinWeldGroups);
}

void LLCleanClusterSkinVK::arenaSummary(std::string& out)
{
    // clusters dropped PARTIALLY (expected 0) | avatars hidden whole (peak in a frame) + their cluster-
    // frames, SHORT frames | avatars coarsened (peak) + the pressure (now / peak) | arena live (the
    // demand peak of ~5-10 s) / cap (allocated) / peak (allocated high-water) MB, demand peak ever,
    // and the limit (auto = 1/16 of the VRAM budget; or the setting) after the ladder / room.
    const double mb = 1.0 / 1048576.0;
    const uint64_t live = std::max(mDemandPeakCur, mDemandPeakPrev) * kArenaBytesPerCluster;
    char b[400];
    std::snprintf(b, sizeof(b),
                  "dropped %llu | hidden av %u (%llu cl-frames, short %u/%u fr) | coarse av %u pressure %.2f (peak %.2f) | "
                  "arena %.1f/%.1f/%.1f MB live/cap/peak (demand peak %llu cl) limit %.1f MB %s",
                  (unsigned long long)mWinDropped, mWinRefusedMax, (unsigned long long)mWinHidden, mWinShortFrames, mWinFrames,
                  mWinCoarseMax, mArenaP, mWinPMax, (double)live * mb, (double)mSkinOut.size * mb, (double)mArenaPeakBytes * mb,
                  (unsigned long long)mArenaDemandPeakEver, (double)(mArenaLimit * kArenaBytesPerCluster) * mb,
                  mArenaSettingMB ? "(RenderVKClusterSkinArenaMB)" : "(auto)");
    out += b;
    mWinDropped = mWinHidden = 0;
    mWinRefusedMax = mWinShortFrames = mWinCoarseMax = mWinFrames = 0;
    mWinPMax = mArenaP;
}

uint64_t LLCleanClusterSkinVK::heldBytes() const
{
    uint64_t b = mStretch.size + mSkinOut.size + mScratch.size + mRest.size;
    for (uint32_t s = 0; s < kSlots; ++s) b += mReadback[s].size;
    return b;
}
