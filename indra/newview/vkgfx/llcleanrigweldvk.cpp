/**
 * @file llcleanrigweldvk.cpp
 * @brief Rigged vertex welding (#574, #641): host side of clean_rig_weld.comp.
 *
 * DESIGN (see the shader header for the per-pass detail):
 *  - Ingest (llcleanmeshasset.cpp, LLCleanQV::markWeldBoundary): every rigged slot
 *    flags its open-boundary vertices (by position) in bit 7 of its 10 B skin weight
 *    records (#582 format unchanged; joints are < 128). Every LOD and decimated step
 *    (#561) is a slot of its own and is flagged on its own triangles; the quantized
 *    vertex (#573) is untouched.
 *  - Welding is only ever BETWEEN meshes: a vertex pairs only with vertices of other
 *    work items (work item = one face instance; two worn copies of one asset are two),
 *    never with its own face's (slits, hem / lining shells, UV splits), and never across
 *    avatars.
 *
 * RenderVKRigWeldMode 0 -- PER-FRAME SEARCH (#574, kept for A/B). Right after the
 *    pre-skin (same command buffer, before the frame pipelining join / RT build fork /
 *    every consumer): INSERT the flagged vertices of this frame's work list into a
 *    bind-space hash grid keyed by avatar, WELD (average positions / normals within
 *    epsilon and the angle threshold), APPLY to the skinned stream. Stateless (rebuilt
 *    every frame). Bounded: entries capped (RenderVKRigWeldMaxVerts); vertices past the
 *    cap are not welded and counted as dropped.
 *
 * RenderVKRigWeldMode 1 -- REST-POSE PAIRS (#641, default). The pairs are decided ONCE
 *    per avatar and only re-decided when what they depend on changes:
 *    DISCOVERY (async, <= kDiscPerFrame avatars a frame, never a stall): the CPU builds
 *    the avatar's REST joint rows (identity joint rotations; the shape's joint positions /
 *    scales and joint overrides, composed as LLXformMatrix::update + LLMatrix4::initAll)
 *    into the upload ring and allocates a PENDING region in the pair pool; the GPU
 *    rest-skins the avatar's boundary vertices (INSERT_REST), pairs vertices of different
 *    meshes within RenderVKRigWeldDistance at rest whose skin weights overlap by at least
 *    RenderVKRigWeldWeightMatch (#626: cavity rims / onion-skin layers weighted to other
 *    joints stay apart) (PAIR), and PUBLISHes the list as the avatar's active one only
 *    when it is complete (else the old list stays; the CPU reads the status back, frees
 *    the region it replaced, or grows the region and retries).
 *    The avatar is discovered ONE FRAME AFTER it is chosen: that frame forces its
 *    skin-cache items to re-skin (avatarPoseDirtyBit), so every mesh of the avatar is in
 *    the work list.
 *    PER FRAME: VALIDATE each listed mesh against this frame's draw (the main cull's
 *    candidate: skinned range, and rig_cache RC_SRC == the source slot it was discovered
 *    with, same skin -- a LOD / decimated-step switch or a slot re-allocation never gets a
 *    stale list), GATHER (average each record's own + partner vertices, no hash, no
 *    search; records whose meshes were all HELD this frame -- skin-cache ranges not
 *    re-skinned, #632 -- are skipped), APPLY.
 *    TRIGGERS (re-discovery): a new avatar in a joint-table slot (owner UUID change: the
 *    old lists are deactivated on the GPU first), the avatar's rigged TOPOLOGY signature
 *    (every rigged instance, its skin and the slot the cull picks -- attach / detach,
 *    outfit, LOD, pressure; from the rigged demand rescan), its SKELETON serial (shape
 *    sliders) and a bone-only REST hash (joint overrides) -- both rate-limited (avatar
 *    physics bumps the serial), the GPU's STALE count (a drawn mesh skinned from another
 *    slot: decimated steps, re-allocations), and the weld settings (distance, weight
 *    match). Until a new list is published the old one keeps welding (or nothing does).
 *    BOUNDED: one pool of RenderVKRigWeldMaxPairs records; per-avatar regions on a
 *    first-fit free list (coalescing; freed regions wait a few frames before reuse);
 *    live / cap / peak words, lists, recomputes, stale triggers on the VKRigWeld line.
 *
 * Not welded: F6 / temporal previous positions (motion vectors keep the unwelded
 * previous position: a sub-centimetre motion error at a seam).
 *
 * VRAM: grid 16 B + 4 B x cells + 64 B x cap (cells = 2 x cap rounded to a power of
 * two): 9 MB at the default 131072 entries; mode 1 pool 72 B x RenderVKRigWeldMaxPairs
 * + 20 KB (9.4 MB at 131072) + a 210 KB host upload ring. Freed when the setting is off
 * (and the pool in mode 0).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
 * $/LicenseInfo$
 */
#include "linden_common.h"

#include "../llvkhitch.h"

#include "llcleanrenderervk.h"
#include "llcleanrigrestvk.h"
#include "vk_pipeline.h"
#include "vk_world_shaders_spv.h"

#include "llerror.h"
#include "llframetimer.h"
#include "llviewercontrol.h"

#if defined(LL_HAS_CLEAN_PIPELINE)
#  include "../llcleanskinstore.h"
#  include "../llviewerobjectlist.h"
#  include "../llvoavatar.h"
#  include "llavatarjoint.h"        // LLAvatarJointCollisionVolume (rest hash: bones only)
extern U32 gFrameCount;             // llappviewer.cpp
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace
{
constexpr uint32_t kSkinMetaWordsWeld = 7568u / 4u;   // LLCleanSkinStore::SkinMeta stride (words)

struct RigWeldPush
{
    uint32_t mode, work_cap, cap, cell_mask;
    float    cell, eps, cos_n, max_gap;
    uint32_t skin_count, meta_words, inst_count;
    float    wmatch;
    uint32_t skip_pad[8];   // (2026-10-14: the skip bits moved into the weld buffer, kAvatarSlotWords words after the results)
    uint32_t disc[4];   // modes 6..8: discovered avatars, 16 bits each (0xFFFF none)
    uint32_t up_base, seg_count, pad2, pad3;
};
static_assert(sizeof(RigWeldPush) == 112, "clean_rig_weld.comp Push");
VK_PUSH_BUDGET_CHECK(RigWeldPush);

// ---- #641 pair pool / upload ring layout (= clean_rig_weld.comp POOL_* / UP_*) ----
constexpr uint32_t kAvSlots       = kAvatarSlotCap;   // (llavatarslots.h; = AV_SLOT_CAP)
constexpr uint32_t kHdrWords      = 16;      // POOL_H_WORDS
constexpr uint32_t kHdrPendBase   = 4;       // POOL_H_PEND_BASE (host writes 4..10)
constexpr uint32_t kHdrActBase    = 0, kHdrActMesh = 1, kHdrActRec = 2, kHdrActMcap = 3;
constexpr uint32_t kHdrPendMesh   = 5, kHdrPendRec = 6;
constexpr uint32_t kHdrStatus     = 9, kHdrDSerial = 10, kHdrASerial = 11;
constexpr uint32_t kCntBase       = kAvSlots * kHdrWords;   // POOL_CNT_BASE: 4 counters per avatar (16384)
constexpr uint32_t kRegionStart   = kCntBase + kAvSlots * 4;   // first region word (20480)
constexpr uint32_t kMeshWords     = 8;       // POOL_MESH_WORDS
constexpr uint32_t kRecWords      = 16;      // POOL_REC_WORDS
constexpr uint32_t kMaxMeshCap    = 4096;    // per avatar (mesh index is 16 bits)
constexpr uint32_t kSegWords      = 8;       // UP_SEG_WORDS
constexpr uint32_t kSegMax        = 2 * kAvSlots;   // every avatar's active + pending region (1024 slots: 2048)
constexpr uint32_t kUpRest        = kSegMax * 8;   // UP_REST (= the segment table: 16384 words)
constexpr uint32_t kRestRowWords  = 216 * 8; // UP_REST_ROW (= LLRigRest::kRowWords)
constexpr uint32_t kDiscPerFrame  = 4;       // avatars discovered per frame (<= 8 rest rows)
constexpr uint32_t kUpSlotWords   = kUpRest + 8 * kRestRowWords;
constexpr uint32_t kRbSlotBytes   = 32 + kRegionStart * 4;   // weld header (8 B) + pad, pool headers + counters
constexpr uint32_t kRbPoolOff     = 32;
static_assert(kSegMax * kSegWords <= kUpRest, "segment table overlaps the rest rows");
static_assert(kRestRowWords == LLRigRest::kRowWords, "rest row layout (llcleanrigrestvk.h)");

uint32_t nextPow2(uint32_t v)
{
    uint32_t p = 1u;
    while (p < v && p < (1u << 30)) p <<= 1u;
    return p;
}

VkShaderModule weldModule(VkDevice dev, const uint32_t* code, size_t bytes)
{
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(dev, &ci, nullptr, &m));
    return m;
}

void memBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da)
{
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = ss; mb.srcAccessMask = sa; mb.dstStageMask = ds; mb.dstAccessMask = da;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1; d.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &d);
}

// compute / copy / clear writes -> compute reads + writes (and copies / clears / updates)
void computeBarrier(VkCommandBuffer cmd)
{
    memBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
               VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
               VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
               | VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
}
} // namespace

// ---------------------------------------------------------------------------------------
// #641 CPU bookkeeping.
// ---------------------------------------------------------------------------------------
struct LLCleanRendererVK::RigWeldPairs
{
    struct Region { uint32_t base = 0, words = 0, mcap = 0, rcap = 0; };
    struct Av
    {
        LLUUID   owner;
        bool     live = false;          // a joint-table slot owner we track
        Region   active, pending;       // active = what the GPU header names (as far as we know)
        uint32_t pend_serial = 0, pend_frame = 0;
        bool     want = false;          // needs a (re)discovery
        uint32_t next_try = 0;          // not before this frame (rate limit / retry back-off)
        uint32_t last_disc = 0;         // frame of the last discovery recorded
        uint64_t topo_used = 0;         // topology signature the active list was asked for
        bool     topo_used_set = false;
        uint32_t skel_used = 0;         // skeleton serial at the last discovery
        uint64_t rest_used = 0;         // bone rest hash at the last discovery
        bool     skel_set = false;
        uint32_t gen_used = 0;          // settings generation
        uint32_t mesh_hint = 64, rec_hint = 1024;
        uint32_t meshes = 0, recs = 0;  // the active list (readback)
        bool     deactivate = false;    // record: zero the GPU header (owner left)
    };
    Av       av[kAvSlots];
    // topology signature (rigged demand rescan)
    uint64_t topo_scan[kAvSlots] = {};
    uint64_t topo_cur[kAvSlots] = {};
    bool     topo_valid = false;
    // free list over [kRegionStart, pool_words): (base, words), ascending by base
    std::vector<std::pair<uint32_t, uint32_t>> free_list;
    struct Retire { uint32_t base, words, frame; };
    std::vector<Retire> retire;
    uint32_t pool_words = 0;
    uint64_t live_words = 0, peak_words = 0;
    uint32_t serial = 0, gen = 1;
    float    gen_mm = -1.f, gen_wm = -1.f;
    uint32_t rr = 0;                    // round-robin cursor (shape checks)
    uint32_t slots_frame = 0;           // last owner refresh
    // stats since the last log
    uint32_t n_disc = 0, n_ok = 0, n_fail = 0, n_empty = 0, n_stale = 0, n_shape = 0, n_topo = 0, n_alloc_fail = 0;
    uint32_t applied = 0, held = 0, stale_now = 0, noop = 0;   // (noop: #798 already-seamless records left alone)
    uint32_t disc_entries_peak = 0, disc_dropped = 0;
    uint32_t disc_now[kDiscPerFrame] = {};   // this frame's discovered avatar slots
    uint32_t n_disc_now = 0;

    void reset(uint32_t words)
    {
        for (Av& a : av) a = Av{};
        std::memset(topo_scan, 0, sizeof(topo_scan));
        std::memset(topo_cur, 0, sizeof(topo_cur));
        topo_valid = false;
        free_list.clear();
        retire.clear();
        pool_words = words;
        if (words > kRegionStart) free_list.push_back({kRegionStart, words - kRegionStart});
        live_words = 0;
        n_disc_now = 0;
    }

    bool alloc(uint32_t words, uint32_t& base)
    {
        for (size_t i = 0; i < free_list.size(); ++i)
        {
            if (free_list[i].second < words) continue;
            base = free_list[i].first;
            free_list[i].first += words;
            free_list[i].second -= words;
            if (free_list[i].second == 0) free_list.erase(free_list.begin() + (std::ptrdiff_t)i);
            live_words += words;
            peak_words = std::max(peak_words, live_words);
            return true;
        }
        return false;
    }
    void release(uint32_t base, uint32_t words)
    {
        if (!words) return;
        live_words -= std::min<uint64_t>(live_words, words);
        auto it = std::lower_bound(free_list.begin(), free_list.end(), std::make_pair(base, 0u));
        it = free_list.insert(it, {base, words});
        // coalesce with the next, then the previous
        if (it + 1 != free_list.end() && it->first + it->second == (it + 1)->first)
        {
            it->second += (it + 1)->second;
            free_list.erase(it + 1);
        }
        if (it != free_list.begin() && (it - 1)->first + (it - 1)->second == it->first)
        {
            (it - 1)->second += it->second;
            free_list.erase(it);
        }
    }
    // A region the GPU may still read (frames in flight): reusable a few frames later.
    void retireRegion(Region& r, uint32_t now)
    {
        if (r.words) retire.push_back({r.base, r.words, now + 4u});
        r = Region{};
    }
    void drainRetire(uint32_t now)
    {
        for (size_t i = 0; i < retire.size();)
        {
            if ((int32_t)(now - retire[i].frame) >= 0)
            {
                release(retire[i].base, retire[i].words);
                retire[i] = retire.back();
                retire.pop_back();
            }
            else ++i;
        }
    }
};

#if defined(LL_HAS_CLEAN_PIPELINE)
// ---------------------------------------------------------------------------------------
// REST JOINT ROWS (llcleanrigrestvk.h): this file's discovery ring AND the cluster LOD weld's
// rest table (llcleanclusterskinvk.cpp, #798) -- one pairing space for both welds.
// ---------------------------------------------------------------------------------------
namespace LLRigRest
{
void slotOwners(std::vector<std::pair<LLUUID, uint32_t>>& out)
{
    std::vector<std::pair<LLUUID, U32>> slots;
    LLCleanSkinStore::instance().copyAvatarSlots(slots);
    out.clear();
    out.reserve(slots.size());
    for (const auto& kv : slots) out.emplace_back(kv.first, (uint32_t)kv.second);
}

uint32_t skeletonSerial(LLVOAvatar* av)
{
    return av ? (uint32_t)av->getSkeletonSerialNum() : 0u;
}

// REST joint rows of an avatar: per joint-table index j (av->getJoint(j)), the world
// transform with every joint rotation at identity -- position = parent position + the
// joint's local position (scaled by the parent's local scale when the parent scales its
// children's offsets: LLXformMatrix::update), matrix = initAll(local scale, identity,
// position). The local position / scale carry the shape (skeletal distortions) and the
// joint overrides; the root sits at the origin. Read through the xform (no gpuWant: this
// must not grow a GPU-animated avatar's readback).
void buildRow(LLVOAvatar* av, float* out)
{
    std::unordered_map<LLJoint*, std::pair<LLVector3, LLVector3>> memo;   // joint -> (world pos, local scale)
    memo.reserve(256);
    std::vector<LLJoint*> chain;
    auto restOf = [&](LLJoint* j) -> std::pair<LLVector3, LLVector3>
    {
        // iterative walk up to the first memoized ancestor (or the root), then down
        chain.clear();
        LLJoint* c = j;
        while (c && memo.find(c) == memo.end())
        {
            chain.push_back(c);
            c = c->getParent();
            if (chain.size() > 512) break;   // (a broken cycle: never expected)
        }
        for (auto it = chain.rbegin(); it != chain.rend(); ++it)
        {
            LLJoint* x = *it;
            LLXformMatrix* xf = x->getXform();
            const LLVector3 scale = xf->getScale();
            LLVector3 wpos(0.f, 0.f, 0.f);
            LLJoint* p = x->getParent();
            if (p)
            {
                auto pm = memo.find(p);
                const LLVector3 ppos = pm != memo.end() ? pm->second.first : LLVector3(0.f, 0.f, 0.f);
                LLVector3 off = xf->getPosition();
                if (p->getXform()->getScaleChildOffset())
                    off.scaleVec(pm != memo.end() ? pm->second.second : p->getXform()->getScale());
                wpos = ppos + off;
            }
            memo[x] = {wpos, scale};
        }
        auto f = memo.find(j);
        return f != memo.end() ? f->second : std::make_pair(LLVector3(0.f, 0.f, 0.f), LLVector3(1.f, 1.f, 1.f));
    };
    for (uint32_t j = 0; j < kJoints; ++j)
    {
        float* o = out + j * 8u;
        LLJoint* jt = av->getJoint((S32)j);
        LLVector3 p(0.f, 0.f, 0.f), s(1.f, 1.f, 1.f);
        if (jt)
        {
            const auto r = restOf(jt);
            p = r.first;
            s = r.second;
            if (!p.isFinite()) p.clear();
            if (!s.isFinite()) s.set(1.f, 1.f, 1.f);
        }
        o[0] = p.mV[0]; o[1] = p.mV[1]; o[2] = p.mV[2]; o[3] = 0.f;
        o[4] = s.mV[0]; o[5] = s.mV[1]; o[6] = s.mV[2]; o[7] = 0.f;
    }
}

// Change detector for the rest pose: the BONES' local positions / scales (joint overrides,
// skeletal distortions), quantized to 0.1 mm / 1e-4 -- a flat read of the xforms, no
// composition. Collision volumes are left out: avatar physics moves them every frame.
uint64_t boneHash(LLVOAvatar* av)
{
    uint64_t h = 0xcbf29ce484222325ull;
    auto mix = [&h](int64_t v) { h = (h ^ (uint64_t)v) * 0x100000001B3ull; };
    for (uint32_t j = 0; j < 216u; ++j)
    {
        LLJoint* jt = av->getJoint((S32)j);
        if (!jt || dynamic_cast<LLAvatarJointCollisionVolume*>(jt)) continue;
        LLXformMatrix* xf = jt->getXform();
        const LLVector3& p = xf->getPosition();
        const LLVector3& s = xf->getScale();
        for (int k = 0; k < 3; ++k)
        {
            mix(std::isfinite(p.mV[k]) ? (int64_t)std::llround(p.mV[k] * 10000.f) : 0);
            mix(std::isfinite(s.mV[k]) ? (int64_t)std::llround(s.mV[k] * 10000.f) : 0);
        }
    }
    return h;
}

LLVOAvatar* findAvatar(const LLUUID& id)
{
    LLViewerObject* o = gObjectList.findObject(id);
    LLVOAvatar* a = o ? o->asAvatar() : nullptr;
    return (a && !a->isDead()) ? a : nullptr;
}
} // namespace LLRigRest
#endif

bool LLCleanRendererVK::initRigWeld()
{
    if (mRigWeldPipe) return true;
    if (mRigWeldInitFailed || !mCtx) return false;
    mRigWeldInitFailed = true;   // until everything below exists
    VkDevice dev = mCtx->device();
    VkDescriptorSetLayoutBinding b[kRigWeldBindings]{};
    for (uint32_t i = 0; i < kRigWeldBindings; ++i)
    {
        b[i].binding = i; b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.bindingCount = kRigWeldBindings; dl.pBindings = b;
    if (vkdCreateSetLayout(dev, &dl, &mDslRigWeld, "mDslRigWeld") != VK_SUCCESS) return false;
    if (mRigWeldPool.create(dev, "rigweld", {{mDslRigWeld, 1}}) != VK_SUCCESS) return false;
    if (mRigWeldPool.allocate(mDslRigWeld, &mDsetRigWeld) != VK_SUCCESS) return false;
    VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(RigWeldPush)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1; pli.pSetLayouts = &mDslRigWeld;
    pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr;
    if (vkdCreatePipelineLayout(dev, &pli, &mRigWeldLayout, "mRigWeldLayout") != VK_SUCCESS) return false;
    VkShaderModule m = weldModule(dev, kWorldRigWeldSpv, kWorldRigWeldSpv_bytes);
    mRigWeldPipe = createComputePipeline(dev, m, mRigWeldLayout);
    vkDestroyShaderModule(dev, m, nullptr);
    if (!mRigWeldPipe) return false;
    std::memset(mRigWeldBound, 0, sizeof(mRigWeldBound));
    std::memset(mRigWeldBoundSerial, 0, sizeof(mRigWeldBoundSerial));
    mRigWeldInitFailed = false;
    return true;
}

void LLCleanRendererVK::releaseRigWeldPairs(bool wait)
{
    if (!mCtx) return;
    if ((mRigWeldPairBuf.buffer || mRigWeldUp.buffer) && wait) vkDeviceWaitIdle(mCtx->device());
    if (mRigWeldPairBuf.buffer) mCtx->destroyBuffer(mRigWeldPairBuf);
    mRigWeldPairBuf = GpuBuffer{};
    if (mRigWeldUp.buffer) mCtx->destroyBuffer(mRigWeldUp);
    mRigWeldUp = GpuBuffer{};
    if (mRigWeldPairs) mRigWeldPairs->reset(0);
    std::memset(mRigWeldDiscNext, 0, sizeof(mRigWeldDiscNext));
    // the set names the pool / upload ring: rewrite it next time
    mRigWeldBound[11] = mRigWeldBound[12] = VK_NULL_HANDLE;
}

void LLCleanRendererVK::destroyRigWeld()
{
    if (!mCtx) return;
    VkDevice dev = mCtx->device();
    if (mRigWeldBuf.buffer) mCtx->destroyBuffer(mRigWeldBuf);
    mRigWeldBuf = GpuBuffer{};
    if (mRigWeldRb.buffer) mCtx->destroyBuffer(mRigWeldRb);
    mRigWeldRb = GpuBuffer{};
    releaseRigWeldPairs(false);
    delete mRigWeldPairs;
    mRigWeldPairs = nullptr;
    if (mRigWeldPipe) vkDestroyPipeline(dev, mRigWeldPipe, nullptr);
    mRigWeldPipe = VK_NULL_HANDLE;
    if (mRigWeldLayout) vkDestroyPipelineLayout(dev, mRigWeldLayout, nullptr);
    mRigWeldLayout = VK_NULL_HANDLE;
    mRigWeldPool.destroy();
    mDsetRigWeld = VK_NULL_HANDLE;
    if (mDslRigWeld) vkdDestroySetLayout(dev, mDslRigWeld);
    mDslRigWeld = VK_NULL_HANDLE;
    mRigWeldCap = mRigWeldCells = 0;
    std::memset(mRigWeldBound, 0, sizeof(mRigWeldBound));
    std::memset(mRigWeldBoundSerial, 0, sizeof(mRigWeldBoundSerial));
}

// #641 topology signature: the rigged demand rescan (every rigged instance with a resolved
// skin, at the level the cull picks) mixes each avatar's (instance, skin, slot, vertex
// count) in ascending instance order; rigWeldTopoEnd publishes it.
void LLCleanRendererVK::rigWeldTopoBegin()
{
    if (!mRigWeldPairs) return;
    for (uint64_t& h : mRigWeldPairs->topo_scan) h = 0xcbf29ce484222325ull;
}

void LLCleanRendererVK::rigWeldTopoNote(uint32_t av, uint32_t inst, uint32_t skin, uint32_t slot, uint32_t vert_count)
{
    if (!mRigWeldPairs || av >= kAvSlots) return;
    uint64_t& h = mRigWeldPairs->topo_scan[av];
    const uint32_t v[4] = {inst, skin, slot, vert_count};
    for (uint32_t x : v) h = (h ^ x) * 0x100000001B3ull;
}

void LLCleanRendererVK::rigWeldTopoEnd()
{
    if (!mRigWeldPairs) return;
    std::memcpy(mRigWeldPairs->topo_cur, mRigWeldPairs->topo_scan, sizeof(mRigWeldPairs->topo_cur));
    mRigWeldPairs->topo_valid = true;
}

void LLCleanRendererVK::recordRigWeld(VkCommandBuffer cmd)
{
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<bool> s_on(gSavedSettings, "RenderVKRigWeld", false);
    static LLCachedControl<U32>  s_mode(gSavedSettings, "RenderVKRigWeldMode", 1U);
    static LLCachedControl<U32>  s_cap(gSavedSettings, "RenderVKRigWeldMaxVerts", 131072U);
    VkDevice dev = mCtx->device();
    if (!s_on)
    {
        // Off: release the weld buffers (the pipeline / set stay: they hold no memory).
        if (mRigWeldBuf.buffer || mRigWeldRb.buffer || mRigWeldPairBuf.buffer || mRigWeldUp.buffer)
        {
            vkDeviceWaitIdle(dev);
            if (mRigWeldBuf.buffer) mCtx->destroyBuffer(mRigWeldBuf);
            mRigWeldBuf = GpuBuffer{};
            if (mRigWeldRb.buffer) mCtx->destroyBuffer(mRigWeldRb);
            mRigWeldRb = GpuBuffer{};
            releaseRigWeldPairs(false);
            mRigWeldCap = mRigWeldCells = 0;
            mRigWeldBound[6] = VK_NULL_HANDLE;
            mRigWeldLive = mRigWeldPeak = mRigWeldDropped = 0;
            for (bool& v : mRigWeldRbValid) v = false;
        }
        return;
    }
    const bool pairs = (U32)s_mode != 0u;
    if (!pairs && (mRigWeldPairBuf.buffer || mRigWeldUp.buffer)) releaseRigWeldPairs(true);   // A/B back to mode 0
    if (!mVbo.buffer || !mWeightSSBO.buffer || !mSkinVbo.buffer || !mSkinPaletteSSBO.buffer || !mWorkSSBO.buffer
        || !mSkinMetaSSBO.buffer || !mRigAlloc.buffer || mWorkCap == 0)
        return;
    if (pairs && (!mCand.buffer || !mCandMeta.buffer || !mRigCache.buffer || !mInstanceSSBO.buffer)) return;
    if (!initRigWeld()) return;

    // ---- grid capacity (bounded; re-created only when the setting changes) ----
    const uint32_t cap = nextPow2(std::clamp<uint32_t>((U32)s_cap, 4096u, 1u << 20));
    if (!mRigWeldBuf.buffer || cap != mRigWeldCap)
    {
        VkMemScope memcat(VKMEM_RIG);   // #407
        if (mRigWeldBuf.buffer)
        {
            vkDeviceWaitIdle(dev);   // the set / last frames' dispatches name it
            mCtx->destroyBuffer(mRigWeldBuf);
            mRigWeldBuf = GpuBuffer{};
        }
        const uint32_t cells = nextPow2(cap * 2u);
        // (+ the screen-size skip bits after the results: kAvatarSlotWords words, clean_rig_weld.comp weldSkipBase)
        const VkDeviceSize bytes = 16u + (VkDeviceSize)cells * 4u + (VkDeviceSize)cap * 64u + kAvatarSlotWords * 4u;
        {
            VkDeviceContext::SoftAllocScope scope(mCtx, true);   // out of memory: no welding, not a crash
            mRigWeldBuf = mCtx->createBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
                                                        | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        }
        if (!mRigWeldBuf.buffer)
        {
            LL_WARNS_ONCE("Vulkan") << "Rigged welding: could not allocate " << (bytes >> 20) << " MB; welding off" << LL_ENDL;
            mRigWeldCap = mRigWeldCells = 0;
            mRigWeldBound[6] = VK_NULL_HANDLE;
            return;
        }
        mCtx->setObjectName(VK_OBJECT_TYPE_BUFFER, (uint64_t)mRigWeldBuf.buffer, "rigWeld");
        mRigWeldCap = cap;
        mRigWeldCells = cells;
        mRigWeldPeak = mRigWeldDropped = 0;
        LL_INFOS("Vulkan") << "Rigged welding: " << cap << " entries, " << cells << " cells, " << (bytes >> 10) << " KB" << LL_ENDL;
    }
    if (!mRigWeldRb.buffer)
    {
        VkMemScope memcat(VKMEM_READBACK);   // #407
        mRigWeldRb = mCtx->createBuffer(3u * (VkDeviceSize)kRbSlotBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        for (bool& v : mRigWeldRbValid) v = false;
    }

    // ---- #641 pair pool + upload ring (mode 1) ----
    if (pairs)
    {
        static LLCachedControl<U32> s_pairs(gSavedSettings, "RenderVKRigWeldMaxPairs", 131072U);
        const uint32_t recs = std::clamp<uint32_t>((U32)s_pairs, 4096u, 1u << 21);
        // records + a mesh table budget of one mesh per 4 records
        const uint64_t words64 = (uint64_t)kRegionStart + (uint64_t)recs * kRecWords + (uint64_t)recs / 4u * kMeshWords;
        const uint32_t words = (uint32_t)std::min<uint64_t>(words64, 0x3FFFFFFFull);
        if (!mRigWeldPairs) mRigWeldPairs = new RigWeldPairs();
        if (!mRigWeldPairBuf.buffer || mRigWeldPairs->pool_words != words)
        {
            releaseRigWeldPairs(true);
            VkMemScope memcat(VKMEM_RIG);   // #407
            {
                VkDeviceContext::SoftAllocScope scope(mCtx, true);
                mRigWeldPairBuf = mCtx->createBuffer((VkDeviceSize)words * 4u,
                                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
                                                  | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            }
            if (!mRigWeldPairBuf.buffer)
            {
                LL_WARNS_ONCE("Vulkan") << "Rigged welding: could not allocate the pair pool (" << ((uint64_t)words * 4u >> 20)
                                        << " MB); welding off" << LL_ENDL;
                return;
            }
            mCtx->setObjectName(VK_OBJECT_TYPE_BUFFER, (uint64_t)mRigWeldPairBuf.buffer, "rigWeldPairs");
            mRigWeldPairs->reset(words);
            // every header empty (no list), every counter 0
            vkCmdFillBuffer(cmd, mRigWeldPairBuf.buffer, 0, VK_WHOLE_SIZE, 0u);
            computeBarrier(cmd);
            LL_INFOS("Vulkan") << "Rigged welding (rest-pose pairs): pool " << recs << " records, "
                               << ((uint64_t)words * 4u >> 10) << " KB" << LL_ENDL;
        }
        if (!mRigWeldUp.buffer)
        {
            VkMemScope memcat(VKMEM_STAGING);   // #407
            mRigWeldUp = mCtx->createBuffer(3u * (VkDeviceSize)kUpSlotWords * 4u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            if (!mRigWeldUp.buffer || !mRigWeldUp.mapped)
            {
                LL_WARNS_ONCE("Vulkan") << "Rigged welding: no upload ring; welding off" << LL_ENDL;
                return;
            }
        }
    }

    // ---- descriptor set: rewritten (after a device wait) when a buffer was re-created ----
    // (Mode 0 reads none of 7..12: they name whatever exists.)
    const GpuBuffer& fb = mRigWeldBuf;
    auto pick = [&fb](const GpuBuffer& b) -> const GpuBuffer& { return b.buffer ? b : fb; };
    const GpuBuffer* cur_b[kRigWeldBindings] = {&mVbo, &mWeightSSBO, &mSkinVbo, &mSkinPaletteSSBO, &mWorkSSBO,
                                                &mSkinMetaSSBO, &mRigWeldBuf, &pick(mCand), &pick(mCandMeta),
                                                &pick(mRigCache), &pick(mInstanceSSBO), &pick(mRigWeldPairBuf),
                                                &pick(mRigWeldUp)};
    VkBuffer cur[kRigWeldBindings];
    // Serials too: a re-created buffer can get the freed one's handle back, and a set
    // left naming the freed memory faults the GPU (device lost in c_rigweld).
    uint64_t cur_serial[kRigWeldBindings];
    for (uint32_t i = 0; i < kRigWeldBindings; ++i) { cur[i] = cur_b[i]->buffer; cur_serial[i] = cur_b[i]->serial; }
    if (std::memcmp(cur, mRigWeldBound, sizeof(cur)) != 0
        || std::memcmp(cur_serial, mRigWeldBoundSerial, sizeof(cur_serial)) != 0)
    {
        bool used = false;
        for (VkBuffer bnd : mRigWeldBound) used = used || bnd != VK_NULL_HANDLE;
        if (used) vkDeviceWaitIdle(dev);   // frames in flight may still read the set
        VkDescriptorBufferInfo bi[kRigWeldBindings];
        VkWriteDescriptorSet w[kRigWeldBindings]{};
        for (uint32_t i = 0; i < kRigWeldBindings; ++i)
        {
            bi[i] = {cur[i], 0, VK_WHOLE_SIZE};
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet = mDsetRigWeld;
            w[i].dstBinding = i; w[i].descriptorCount = 1;
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[i].pBufferInfo = &bi[i];
        }
        vkUpdateDescriptorSets(dev, kRigWeldBindings, w, 0, nullptr);
        std::memcpy(mRigWeldBound, cur, sizeof(cur));
        std::memcpy(mRigWeldBoundSerial, cur_serial, sizeof(cur_serial));
    }

    if (pairs) recordRigWeldPairs(cmd);
    else       recordRigWeldSearch(cmd);
#else
    (void)cmd;
#endif
}

// ---------------------------------------------------------------------------------------
// MODE 0: per-frame bind-space search (#574).
// ---------------------------------------------------------------------------------------
void LLCleanRendererVK::recordRigWeldSearch(VkCommandBuffer cmd)
{
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<F32>  s_mm(gSavedSettings, "RenderVKRigWeldDistance", 0.2f);
    static LLCachedControl<F32>  s_deg(gSavedSettings, "RenderVKRigWeldNormalAngle", 60.f);
    static LLCachedControl<F32>  s_gap(gSavedSettings, "RenderVKRigWeldMaxGap", 2.f);
    static LLCachedControl<F32>  s_min_px(gSavedSettings, "RenderVKRigWeldMinPx", 80.f);

    // ---- stats: the slot written 3 frames ago ----
    const uint32_t slot = mRigWeldRbFrame % 3u;
    if (mRigWeldRb.mapped && mRigWeldRbValid[slot])
    {
        uint32_t hdr[2];
        std::memcpy(hdr, static_cast<const uint8_t*>(mRigWeldRb.mapped) + (size_t)slot * kRbSlotBytes, sizeof(hdr));
        mRigWeldLive = std::min(hdr[0], mRigWeldCap);
        mRigWeldPeak = std::max(mRigWeldPeak, mRigWeldLive);
        mRigWeldDropped = std::max(mRigWeldDropped, hdr[1]);
        const double now = LLFrameTimer::getElapsedSeconds();
        if (now >= mRigWeldNextLog)
        {
            mRigWeldNextLog = now + 30.0;
            LL_INFOS("VKRigWeld") << "mode 0 (per-frame search): boundary vertices live " << mRigWeldLive << " / cap "
                                  << mRigWeldCap << ", peak " << mRigWeldPeak << ", dropped (over cap, last 30 s max) "
                                  << mRigWeldDropped << LL_ENDL;
            mRigWeldDropped = 0;
        }
    }

    // ---- clear: count / dropped = 0, cell heads = empty ----
    VkBufferMemoryBarrier2 bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    bb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT;
    bb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    bb.dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT; bb.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    bb.buffer = mRigWeldBuf.buffer; bb.size = VK_WHOLE_SIZE;
    VkDependencyInfo bd{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; bd.bufferMemoryBarrierCount = 1; bd.pBufferMemoryBarriers = &bb;
    vkCmdPipelineBarrier2(cmd, &bd);
    vkCmdFillBuffer(cmd, mRigWeldBuf.buffer, 0, 16, 0u);
    vkCmdFillBuffer(cmd, mRigWeldBuf.buffer, 16, (VkDeviceSize)mRigWeldCells * 4u, 0xFFFFFFFFu);
    bb.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT; bb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    bb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    bb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    vkCmdPipelineBarrier2(cmd, &bd);

    // ---- passes ----
    const float eps = std::clamp((float)s_mm, 0.05f, 10.f) * 0.001f;
    RigWeldPush p{};
    p.work_cap = mWorkCap;
    p.cap = mRigWeldCap;
    p.cell_mask = mRigWeldCells - 1u;
    p.cell = 2.f * eps;
    p.eps = eps;
    p.cos_n = std::cos(std::clamp((float)s_deg, 0.f, 180.f) * 0.01745329252f);
    p.max_gap = std::clamp((float)s_gap, 0.1f, 50.f) * 0.01f;
    const uint32_t meta_skins = (uint32_t)(mSkinMetaSSBO.size / 7568u);
    p.skin_count = std::min({mSkinCount, mSkinPaletteCap, meta_skins});
    p.meta_words = kSkinMetaWordsWeld;
    for (uint32_t& d : p.disc) d = 0xFFFFFFFFu;
    // SCREEN-SIZE SKIP (crowd perf): an avatar whose bounds are shorter on screen than
    // RenderVKRigWeldMinPx (this frame's mRigAvPx) is not welded -- its seams / hairline
    // gaps are sub-pixel there, and its boundary vertices were most of the INSERT scan
    // and of the grid in a crowd. Never our own avatar or the #561 protected set. An
    // unknown size (0) welds.
    uint32_t n_skip = 0;
    uint32_t skip_bits[kAvatarSlotWords] = {};
    if ((F32)s_min_px > 0.f)
    {
        const float min_px = (float)s_min_px;
        for (uint32_t a = 0; a < kAvVisSlots; ++a)
        {
            const float px = mRigAvPx[a];
            if (px > 0.f && px < min_px && !mRigSelfSlot[a] && !mRigProtect[a])
            {
                skip_bits[a >> 5] |= 1u << (a & 31u);
                ++n_skip;
            }
        }
    }
    LLVKHitch::count("rigWeldSkipAv", n_skip);
    // (the skip bits -> the weld buffer's tail; ordered like the clears above, before the insert)
    {
        VkBufferMemoryBarrier2 sb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        sb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; sb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        sb.dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT; sb.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        sb.buffer = mRigWeldBuf.buffer; sb.size = VK_WHOLE_SIZE;
        VkDependencyInfo sd{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; sd.bufferMemoryBarrierCount = 1; sd.pBufferMemoryBarriers = &sb;
        vkCmdPipelineBarrier2(cmd, &sd);
        vkCmdUpdateBuffer(cmd, mRigWeldBuf.buffer, 16u + (VkDeviceSize)mRigWeldCells * 4u + (VkDeviceSize)mRigWeldCap * 64u,
                          sizeof(skip_bits), skip_bits);
        sb.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT; sb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        sb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; sb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        vkCmdPipelineBarrier2(cmd, &sd);
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mRigWeldPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mRigWeldLayout, 0, 1, &mDsetRigWeld, 0, nullptr);
    // INSERT: one workgroup per work item (the pre-skin's indirect dispatch).
    p.mode = 0u;
    vkCmdPushConstants(cmd, mRigWeldLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
    vkCmdDispatchIndirect(cmd, mRigAlloc.buffer, 0);
    // The pre-skin's skinned stream + the grid -> WELD reads.
    memBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    const uint32_t groups = (mRigWeldCap + 63u) / 64u;
    p.mode = 1u;
    vkCmdPushConstants(cmd, mRigWeldLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
    vkCmdDispatch(cmd, groups, 1, 1);
    memBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    p.mode = 2u;
    vkCmdPushConstants(cmd, mRigWeldLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
    vkCmdDispatch(cmd, groups, 1, 1);
    // Welded stream -> later compute / copy readers on this queue (RT rig copy, pick,
    // probe / shadow rig culls). Compute + copy stages only: with frame pipelining this
    // is the async compute queue's command buffer (the join semaphore publishes it to
    // graphics, and the frame's own compute -> vertex-fetch barrier covers the draws).
    memBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
               VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
    // Stats: the 2 header words -> the readback ring (read 3 frames later).
    if (mRigWeldRb.buffer)
    {
        VkBufferCopy c{0, (VkDeviceSize)slot * kRbSlotBytes, 8u};
        vkCmdCopyBuffer(cmd, mRigWeldBuf.buffer, mRigWeldRb.buffer, 1, &c);
        memBarrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
        mRigWeldRbValid[slot] = true;
    }
    ++mRigWeldRbFrame;
    gpuSubMark(cmd, "c_rigweld");
#else
    (void)cmd;
#endif
}

// ---------------------------------------------------------------------------------------
// MODE 1: rest-pose pairs (#641).
// ---------------------------------------------------------------------------------------
void LLCleanRendererVK::recordRigWeldPairs(VkCommandBuffer cmd)
{
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<F32>  s_mm(gSavedSettings, "RenderVKRigWeldDistance", 0.2f);
    static LLCachedControl<F32>  s_deg(gSavedSettings, "RenderVKRigWeldNormalAngle", 60.f);
    static LLCachedControl<F32>  s_gap(gSavedSettings, "RenderVKRigWeldMaxGap", 2.f);
    static LLCachedControl<F32>  s_min_px(gSavedSettings, "RenderVKRigWeldMinPx", 80.f);
    static LLCachedControl<F32>  s_wmatch(gSavedSettings, "RenderVKRigWeldWeightMatch", 0.8f);
    RigWeldPairs& P = *mRigWeldPairs;
    const uint32_t now = (uint32_t)gFrameCount;
    P.drainRetire(now);

    // ---- settings generation: the distance / weight match decide the pairs ----
    const float mm = std::clamp((float)s_mm, 0.05f, 10.f);
    const float wm = std::clamp((float)s_wmatch, 0.f, 1.f);
    if (mm != P.gen_mm || wm != P.gen_wm)
    {
        if (P.gen_mm >= 0.f) ++P.gen;
        P.gen_mm = mm; P.gen_wm = wm;
    }

    // ---- readback: the slot written 3 frames ago (headers + counters + grid stats) ----
    const uint32_t slot = mRigWeldRbFrame % 3u;
    if (mRigWeldRb.mapped && mRigWeldRbValid[slot])
    {
        const uint8_t* rb = static_cast<const uint8_t*>(mRigWeldRb.mapped) + (size_t)slot * kRbSlotBytes;
        uint32_t ghdr[2];
        std::memcpy(ghdr, rb, sizeof(ghdr));
        const uint32_t* pw = reinterpret_cast<const uint32_t*>(rb + kRbPoolOff);
        P.disc_entries_peak = std::max(P.disc_entries_peak, std::min(ghdr[0], mRigWeldCap));
        P.disc_dropped = std::max(P.disc_dropped, ghdr[1]);
        uint32_t applied = 0, held = 0, stale = 0, noop = 0;
        for (uint32_t a = 0; a < kAvSlots; ++a)
        {
            RigWeldPairs::Av& A = P.av[a];
            const uint32_t* h = pw + a * kHdrWords;
            const uint32_t* c = pw + kCntBase + a * 4u;
            applied += c[1];
            held += c[2];
            noop += c[3];
            if (A.pending.words && h[kHdrDSerial] == A.pend_serial && h[kHdrStatus] != 0u)
            {
                const uint32_t st = h[kHdrStatus];
                if (st == 1u && h[kHdrActBase] == A.pending.base && h[kHdrASerial] == A.pend_serial)
                {
                    // published: the list it replaced is no longer read by any later frame
                    P.retireRegion(A.active, now);
                    A.active = A.pending;
                    A.pending = RigWeldPairs::Region{};
                    A.meshes = h[kHdrActMesh];
                    A.recs = h[kHdrActRec];
                    A.mesh_hint = std::clamp<uint32_t>(A.meshes + A.meshes / 4u + 8u, 16u, kMaxMeshCap);
                    A.rec_hint = std::max<uint32_t>(A.recs + A.recs / 4u + 64u, 256u);
                    ++P.n_ok;
                }
                else
                {
                    P.retireRegion(A.pending, now);
                    A.want = true;
                    if (st == 2u || st == 3u)
                    {
                        // overflow: grow to what the GPU asked for and retry soon
                        A.mesh_hint = std::clamp<uint32_t>(std::max(A.mesh_hint * 2u, h[kHdrPendMesh] + 16u), 16u, kMaxMeshCap);
                        A.rec_hint = std::max(A.rec_hint * 2u, h[kHdrPendRec] + h[kHdrPendRec] / 4u + 256u);
                        A.next_try = now + 2u;
                        ++P.n_fail;
                    }
                    else
                    {
                        A.next_try = now + 30u;   // nothing of it drawn: try again later
                        ++P.n_empty;
                    }
                }
            }
            else if (A.pending.words && now - A.pend_frame > 60u && h[kHdrActBase] != A.pending.base)
            {
                // never saw the publish (a dropped frame): the GPU does not name it, retry
                P.retireRegion(A.pending, now);
                A.want = true;
                A.next_try = now + 30u;
            }
            if (c[0] != 0u && A.live && A.active.words)
            {
                // the GPU met a listed mesh drawn from another slot / skin (decimated step,
                // LOD flip, slot re-allocation): re-discover (rate-limited)
                ++stale;
                if (!A.want) { A.want = true; ++P.n_stale; }
                A.next_try = std::max(A.next_try, A.last_disc + 15u);
            }
        }
        P.applied = applied;
        P.noop = noop;
        P.held = held;
        P.stale_now = stale;
    }

    // ---- owners: a joint-table slot taken by another avatar loses its lists ----
    if (now - P.slots_frame >= 8u || P.slots_frame == 0u)
    {
        P.slots_frame = now;
        static std::vector<std::pair<LLUUID, U32>> s_slots;
        s_slots.clear();
        LLCleanSkinStore::instance().copyAvatarSlots(s_slots);
        bool present[kAvSlots] = {};
        for (const auto& kv : s_slots)
        {
            const uint32_t a = kv.second;
            if (a >= kAvSlots) continue;
            present[a] = true;
            RigWeldPairs::Av& A = P.av[a];
            if (!A.live || A.owner != kv.first)
            {
                if (A.live || A.active.words || A.pending.words) A.deactivate = true;
                P.retireRegion(A.active, now);
                P.retireRegion(A.pending, now);
                const bool deact = A.deactivate;
                A = RigWeldPairs::Av{};
                A.deactivate = deact;
                A.owner = kv.first;
                A.live = true;
                A.want = true;
            }
        }
        for (uint32_t a = 0; a < kAvSlots; ++a)
        {
            RigWeldPairs::Av& A = P.av[a];
            if (present[a] || !A.live) continue;
            P.retireRegion(A.active, now);
            P.retireRegion(A.pending, now);
            A = RigWeldPairs::Av{};
            A.deactivate = true;
        }
    }

    // ---- triggers: settings, topology, shape (round robin, rate-limited) ----
    for (uint32_t a = 0; a < kAvSlots; ++a)
    {
        RigWeldPairs::Av& A = P.av[a];
        if (!A.live) continue;
        if (A.gen_used != P.gen && (A.active.words || A.gen_used != 0u) && !A.want) A.want = true;
        if (P.topo_valid && A.topo_used_set && P.topo_cur[a] != A.topo_used && !A.want)
        {
            A.want = true;
            ++P.n_topo;
        }
    }
    for (uint32_t a = 0; a < kAvSlots; ++a)
    {
        // (discovered before the first topology scan: re-discover once, which arms it)
        RigWeldPairs::Av& A = P.av[a];
        if (A.live && P.topo_valid && !A.topo_used_set && A.active.words && !A.want) A.want = true;
    }
    for (uint32_t k = 0; k < 4u; ++k)
    {
        const uint32_t a = P.rr++ % kAvSlots;
        RigWeldPairs::Av& A = P.av[a];
        if (!A.live || A.want || !A.skel_set || A.pending.words) continue;
        LLVOAvatar* avp = LLRigRest::findAvatar(A.owner);
        if (!avp) continue;
        // SHAPE: the skeleton serial (skeletal distortions; avatar physics bumps it too, so
        // at most every ~5 s) and the bones' rest hash (joint overrides; every ~2 s).
        bool shape = false;
        if (avp->getSkeletonSerialNum() != A.skel_used && now - A.last_disc > 300u) shape = true;
        if (!shape && now - A.last_disc > 120u) shape = LLRigRest::boneHash(avp) != A.rest_used;
        if (shape)
        {
            A.want = true;
            ++P.n_shape;
        }
    }

    // ---- screen-size skip (as mode 0): small avatars are neither gathered nor discovered ----
    uint32_t skip[kAvatarSlotWords] = {};
    uint32_t n_skip = 0;
    if ((F32)s_min_px > 0.f)
    {
        const float min_px = (float)s_min_px;
        for (uint32_t a = 0; a < kAvVisSlots && a < kAvSlots; ++a)
        {
            const float px = mRigAvPx[a];
            if (px > 0.f && px < min_px && !mRigSelfSlot[a] && !mRigProtect[a])
            {
                skip[a >> 5] |= 1u << (a & 31u);
                ++n_skip;
            }
        }
    }
    LLVKHitch::count("rigWeldSkipAv", n_skip);
    auto skipped = [&skip](uint32_t a) { return (skip[a >> 5] >> (a & 31u)) & 1u; };

    // ---- this frame's discovery: the avatars chosen LAST frame (their cached items were
    //      forced to re-skin this frame) ----
    uint8_t* up = static_cast<uint8_t*>(mRigWeldUp.mapped) + (size_t)slot * kUpSlotWords * 4u;
    uint32_t* upw = reinterpret_cast<uint32_t*>(up);
    RigWeldPush p{};
    for (uint32_t& d : p.disc) d = 0xFFFFFFFFu;
    struct HdrWrite { uint32_t av; uint32_t w[7]; };
    HdrWrite hw[kDiscPerFrame];
    uint32_t n_hw = 0;
    P.n_disc_now = 0;
    for (uint32_t a = 0; a < kAvSlots && P.n_disc_now < kDiscPerFrame; ++a)
    {
        if (!mRigWeldDiscNext[a]) continue;
        RigWeldPairs::Av& A = P.av[a];
        if (!A.live || A.pending.words || A.deactivate) continue;
        LLVOAvatar* avp = LLRigRest::findAvatar(A.owner);
        if (!avp)
        {
            // no avatar behind the slot (a flexi pseudo slot, or not in the object list
            // yet): never let it occupy the discovery list
            A.next_try = now + 300u;
            continue;
        }
        const uint32_t mcap = std::clamp<uint32_t>(A.mesh_hint, 16u, kMaxMeshCap);
        const uint32_t rcap = std::max<uint32_t>(A.rec_hint, 256u);
        const uint64_t words64 = (uint64_t)mcap * kMeshWords + (uint64_t)rcap * kRecWords;
        uint32_t base = 0;
        if (words64 >= P.pool_words || !P.alloc((uint32_t)words64, base))
        {
            ++P.n_alloc_fail;
            A.next_try = now + 60u;   // pool full: the stats line says so; try later
            continue;
        }
        const uint32_t row = P.n_disc_now;
        LLRigRest::buildRow(avp, reinterpret_cast<float*>(upw + kUpRest + row * kRestRowWords));
        const uint64_t bh = LLRigRest::boneHash(avp);
        A.pending = RigWeldPairs::Region{base, (uint32_t)words64, mcap, rcap};
        A.pend_serial = ++P.serial;
        if (A.pend_serial == 0u) A.pend_serial = ++P.serial;
        A.pend_frame = now;
        A.last_disc = now;
        A.want = false;
        A.gen_used = P.gen;
        A.topo_used = P.topo_cur[a];
        A.topo_used_set = P.topo_valid;
        A.skel_used = avp->getSkeletonSerialNum();
        A.rest_used = bh;
        A.skel_set = true;
        p.disc[row >> 1] = (p.disc[row >> 1] & ((row & 1u) ? 0x0000FFFFu : 0xFFFF0000u)) | (a << ((row & 1u) ? 16u : 0u));
        hw[n_hw++] = {a, {base, 0u, 0u, mcap, rcap, 0u, A.pend_serial}};
        P.disc_now[P.n_disc_now++] = a;
        ++P.n_disc;
    }
    // ...and NEXT frame's: wanted, visible (some pass draws it), big enough on screen,
    // largest on screen first.
    std::memset(mRigWeldDiscNext, 0, sizeof(mRigWeldDiscNext));
    {
        uint32_t cand[kAvSlots];
        uint32_t nc = 0;
        const bool avok = mAvVis[kAvVisSlots] != 0u;
        for (uint32_t a = 0; a < kAvSlots; ++a)
        {
            const RigWeldPairs::Av& A = P.av[a];
            if (!A.live || !A.want || A.pending.words || A.deactivate || (int32_t)(now - A.next_try) < 0) continue;
            if (skipped(a)) continue;
            if (avok && !(mAvVis[a] & (kAvVisAny | kAvVisMain))) continue;
            cand[nc++] = a;
        }
        std::sort(cand, cand + nc, [this](uint32_t x, uint32_t y) { return mRigAvPx[x] > mRigAvPx[y]; });
        for (uint32_t i = 0; i < nc && i < kDiscPerFrame; ++i) mRigWeldDiscNext[cand[i]] = 1u;
    }

    // ---- segment table: every region (active + pending) of every avatar ----
    uint32_t seg_n = 0, wg = 0;
    for (uint32_t a = 0; a < kAvSlots && seg_n + 2u <= kSegMax; ++a)
    {
        const RigWeldPairs::Av& A = P.av[a];
        const RigWeldPairs::Region* rs[2] = {&A.active, &A.pending};
        for (const RigWeldPairs::Region* r : rs)
        {
            if (!r->words) continue;
            uint32_t* s = upw + seg_n * kSegWords;
            s[0] = a; s[1] = r->base; s[2] = r->mcap; s[3] = r->rcap; s[4] = wg;
            s[5] = s[6] = s[7] = 0u;
            wg += (r->rcap + 63u) / 64u;
            ++seg_n;
        }
    }

    // ---- record ----
    const float eps = mm * 0.001f;
    p.work_cap = mWorkCap;
    p.cap = mRigWeldCap;
    p.cell_mask = mRigWeldCells - 1u;
    p.cell = 2.f * eps;
    p.eps = eps;
    p.cos_n = std::cos(std::clamp((float)s_deg, 0.f, 180.f) * 0.01745329252f);
    p.max_gap = std::clamp((float)s_gap, 0.1f, 50.f) * 0.01f;
    const uint32_t meta_skins = (uint32_t)(mSkinMetaSSBO.size / 7568u);
    p.skin_count = std::min({mSkinCount, mSkinPaletteCap, meta_skins});
    p.meta_words = kSkinMetaWordsWeld;
    p.inst_count = (uint32_t)std::min<uint64_t>(mCpuInstances.size() / 224u, mInstanceSSBO.size / 224u);
    p.wmatch = wm;
    p.up_base = slot * kUpSlotWords;
    p.seg_count = seg_n;

    // Everything before (the cull's candidates / rig-cache words, the pre-skin's stream,
    // last frame's readback copy) -> the host writes / clears / dispatches below.
    computeBarrier(cmd);
    // Owners that left: their headers name no list any more (before any region reuse).
    for (uint32_t a = 0; a < kAvSlots; ++a)
    {
        RigWeldPairs::Av& A = P.av[a];
        if (!A.deactivate) continue;
        const uint32_t z[12] = {};
        vkCmdUpdateBuffer(cmd, mRigWeldPairBuf.buffer, (VkDeviceSize)a * kHdrWords * 4u, sizeof(z), z);
        A.deactivate = false;
    }
    // This frame's pending headers: {base, mesh 0, rec 0, mesh cap, rec cap, status 0, serial}.
    for (uint32_t i = 0; i < n_hw; ++i)
        vkCmdUpdateBuffer(cmd, mRigWeldPairBuf.buffer, ((VkDeviceSize)hw[i].av * kHdrWords + kHdrPendBase) * 4u,
                          sizeof(hw[i].w), hw[i].w);
    // Per-frame counters = 0.
    vkCmdFillBuffer(cmd, mRigWeldPairBuf.buffer, (VkDeviceSize)kCntBase * 4u, (VkDeviceSize)kAvSlots * 16u, 0u);
    if (n_hw)
    {
        // discovery grid: count / dropped = 0, cell heads = empty
        vkCmdFillBuffer(cmd, mRigWeldBuf.buffer, 0, 16, 0u);
        vkCmdFillBuffer(cmd, mRigWeldBuf.buffer, 16, (VkDeviceSize)mRigWeldCells * 4u, 0xFFFFFFFFu);
    }
    // the screen-size skip bits (modes 4 / 5) -> the weld buffer's tail (clean_rig_weld.comp weldSkipBase)
    vkCmdUpdateBuffer(cmd, mRigWeldBuf.buffer, 16u + (VkDeviceSize)mRigWeldCells * 4u + (VkDeviceSize)mRigWeldCap * 64u,
                      sizeof(skip), skip);
    computeBarrier(cmd);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mRigWeldPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mRigWeldLayout, 0, 1, &mDsetRigWeld, 0, nullptr);
    if (n_hw)
    {
        // DISCOVERY: rest-skin + insert (one workgroup per work item), pair, publish.
        p.mode = 6u;
        vkCmdPushConstants(cmd, mRigWeldLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatchIndirect(cmd, mRigAlloc.buffer, 0);
        computeBarrier(cmd);
        p.mode = 7u;
        vkCmdPushConstants(cmd, mRigWeldLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(cmd, (mRigWeldCap + 63u) / 64u, 1, 1);
        computeBarrier(cmd);
        p.mode = 8u;
        vkCmdPushConstants(cmd, mRigWeldLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(cmd, 1, 1, 1);
        computeBarrier(cmd);
        gpuSubMark(cmd, "c_rigweld_disc");
    }
    if (seg_n && wg)
    {
        // PER FRAME: validate the listed meshes, gather, apply.
        p.mode = 3u;
        vkCmdPushConstants(cmd, mRigWeldLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(cmd, seg_n, 1, 1);
        computeBarrier(cmd);
        p.mode = 4u;
        vkCmdPushConstants(cmd, mRigWeldLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(cmd, wg, 1, 1);
        computeBarrier(cmd);
        p.mode = 5u;
        vkCmdPushConstants(cmd, mRigWeldLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(cmd, wg, 1, 1);
    }
    // Welded stream -> later compute / copy readers on this queue (as mode 0).
    memBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
               VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
    // Readback: grid stats + every header and counter (read 3 frames later).
    if (mRigWeldRb.buffer)
    {
        const VkBufferCopy c0{0, (VkDeviceSize)slot * kRbSlotBytes, 8u};
        vkCmdCopyBuffer(cmd, mRigWeldBuf.buffer, mRigWeldRb.buffer, 1, &c0);
        const VkBufferCopy c1{0, (VkDeviceSize)slot * kRbSlotBytes + kRbPoolOff, (VkDeviceSize)kRegionStart * 4u};
        vkCmdCopyBuffer(cmd, mRigWeldPairBuf.buffer, mRigWeldRb.buffer, 1, &c1);
        memBarrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
        mRigWeldRbValid[slot] = true;
    }
    ++mRigWeldRbFrame;
    gpuSubMark(cmd, "c_rigweld");

    // ---- stats ----
    uint32_t lists = 0, recs = 0;
    for (const RigWeldPairs::Av& A : P.av)
        if (A.active.words) { ++lists; recs += A.recs; }
    LLVKHitch::count("rigWeldDisc", n_hw);
    LLVKHitch::count("rigWeldPairsApplied", P.applied);
    LLVKHitch::count("rigWeldPairsHeld", P.held);
    const double tnow = LLFrameTimer::getElapsedSeconds();
    if (tnow >= mRigWeldNextLog)
    {
        mRigWeldNextLog = tnow + 30.0;
        const uint64_t cap_words = P.pool_words > kRegionStart ? P.pool_words - kRegionStart : 0u;
        LL_INFOS("VKRigWeld") << "mode 1 (rest-pose pairs): lists " << lists << ", records " << recs
                              << " | pool words live " << P.live_words << " / cap " << cap_words << ", peak "
                              << P.peak_words << " (" << P.free_list.size() << " free runs, " << P.retire.size()
                              << " retiring)"
                              << " | last frame: applied " << P.applied << ", held " << P.held << ", noop (seamless) " << P.noop << ", stale avatars "
                              << P.stale_now
                              << " | last 30 s: recomputes " << P.n_disc << " (ok " << P.n_ok << ", grown " << P.n_fail
                              << ", empty " << P.n_empty << ", no pool space " << P.n_alloc_fail << "), triggers: stale "
                              << P.n_stale << ", shape " << P.n_shape << ", topology " << P.n_topo
                              << " | discovery grid peak " << P.disc_entries_peak << " / " << mRigWeldCap
                              << ", dropped " << P.disc_dropped << LL_ENDL;
        P.n_disc = P.n_ok = P.n_fail = P.n_empty = P.n_stale = P.n_shape = P.n_topo = P.n_alloc_fail = 0;
        P.disc_entries_peak = P.disc_dropped = 0;
        P.peak_words = P.live_words;
    }
#else
    (void)cmd;
#endif
}
