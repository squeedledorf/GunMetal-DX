/**
 * @file llcleanrtvk.cpp
 * @brief See llcleanrtvk.h. Ray tracing foundation for the Vulkan clean renderer.
 *
 * DESIGN NOTES
 *
 * Per-SLOT BLAS (vs per-asset). The renderer's geometry unit is the packed LOD slot:
 * SlotInfo {first_index, index_count, base_vertex, vert_count} and LodGroup.slot[4].
 * The cull chooses one slot per instance per frame. Keying BLAS by slot gives:
 *   - exact LOD alignment: the TLAS pass runs the cull's own LOD rule and references
 *     the BLAS of the slot the raster drew (or would draw, off screen);
 *   - sharing: every instance of a mesh LOD references one BLAS (the asset is the
 *     group; per-asset would need a 4-geometry BLAS or per-LOD keys anyway -- which
 *     IS per slot);
 *   - a simple invalidation contract with the packer: a BLAS copies the geometry into
 *     its own storage at build time, so repacks / incremental appends that MOVE a slot
 *     (and VBO growth) don't touch it. Only CONTENT changes do: LLCleanRendererVK calls
 *     slotChanged() from placeSlot() (mirror rewritten or freed = the slot GC) and
 *     from the full repack for every slot whose extent changed or that dropped out of
 *     the pack. The BLAS is retired immediately (lifetime tied to the slot) and its
 *     TLAS address zeroed in the same frame's address-table upload.
 *
 * Lazy build + budget. Pass A (clean_rt_instances.comp) ORs each chosen slot into a
 * `used` bitset. The CPU reads the bitset of frame N-2 (same frame slot, its fence
 * already waited) and builds the missing BLAS, at most RenderVKRTBlasBuildsPerFrame
 * and RenderVKRTBlasTrisPerFrame*1000 triangles per frame. lastUsed drives LRU
 * eviction of static BLAS above RenderVKRTBlasBudgetMB (only BLAS unused for >8
 * frames are evicted; when nothing is evictable, builds wait = "starved").
 * Static BLAS: PREFER_FAST_TRACE | ALLOW_COMPACTION; compacted size queried at build
 * and COMPACT-copied two frames later when it saves >= 10%.
 * All BLAS geometry is flagged neither OPAQUE nor NO_DUPLICATE_ANY_HIT; opacity comes
 * from the TLAS instance flags (FORCE_OPAQUE / FORCE_NO_OPAQUE).
 *
 * Rigged. For every rigged instance of an avatar the cull considers visible (av_vis)
 * and whose skin is live this frame: a private range of the RT rig-position buffer
 * (vec3, re-laid-out every frame), filled by the rig-copy compute from the main
 * cull's pre-skin output, in the owner AVATAR'S PELVIS FRAME (#632: eye-relative output
 * -> R^T (p + eye - pelvis), the skin cache's pelvis-local output as is; the group's TLAS
 * record carries the current pelvis transform), or absolute world for an avatar-less
 * group (identity record). Its BLAS is
 * refit (BUILD_MODE_UPDATE, in place) while slot / slot generation / counts are
 * unchanged, rebuilt otherwise and every kRigRebuildFrames (refit quality decays).
 * PREFER_FAST_BUILD | ALLOW_UPDATE. Order: preskin -> barrier -> rig copy -> barrier
 * -> refit/build -> barrier -> TLAS -> barrier -> consumers. A rigged instance whose
 * pre-skin candidate is missing/mismatched this frame is collapsed to a point by the
 * rig copy (zero-area: never hit), never left with stale positions. Records unseen
 * for kRigKeepFrames are retired.
 * REFIT GATE (RenderVKRigRefitGate, B8): a group whose avatar's skinned positions in
 * the group's space provably did not change -- avatar-local groups: the GPU pose held
 * (LLCleanAnimStore::slotPoseClean; the root may move, #632), world groups: also the
 * root unmoved (slotWorldClean); skin meta unchanged -- skips its rig copy AND its refit -- rig_pos and the
 * BLAS still hold the last copy, which consumers read through rt_geom. Gate 2 also
 * refits avatars farther than RenderVKRigRefitFarMeters every 2nd frame, and small
 * on-screen ones every 2nd / 3rd / 4th (RenderVKRigRefitFullPx, by the renderer's
 * per-slot on-screen height; never self or the #561 protected set) -- copy + refit
 * together, so rig_pos and the BLAS always agree. Only avatars classifyNoSelf tagged
 * (kAvVisRtNoSelf, hysteresis) are throttled: they never self-shadow in RT on any frame
 * (their records carry the owner tag, clean_rt_common.glsl rtSelfCandidate). Only while the plan + rig_pos
 * layout are reused; a forced refit every kRigForceRefit frames self-heals.
 * #1007 CADENCE + POSE SKIP. Distance bands on top of gate 2: past RenderVKRigRefitFarMeters
 * every 2nd frame, past RenderVKRigRefitVeryFarMeters every 4th (RenderVKRigRefitCadence 0 =
 * neither band nor the screen-size rate, and no no-self-shadow tags: A/B). POSE SKIP
 * (RenderVKRigRefitPoseThreshold): mode 5 of clean_rt_instances.comp measures, per joint-table
 * slot, the max change of every joint's pelvis-relative transform since the previous frame
 * (GPU: the palettes are GPU-generated) into a per-frame-slot host readback; the CPU sums the
 * readings since a group's last refit (newest frames without a reading yet count as the
 * newest readings) and skips an avatar-local group whose sum is under the threshold (scaled
 * by distance), for at most RenderVKRigRefitPoseMaxStale frames. Every skip -- held proof,
 * cadence, pose, scratch budget -- drops the group's rig copy AND its refit together, so its
 * BLAS and its rig_pos range (what hit shading reads: rtTriPositions / rtRigNormal) hold the
 * same pose; the TLAS record (pass B) still carries this frame's pelvis. Immediate refit (no
 * cadence / pose skip): new group / geometry change (rebuild), skin meta change, a distance
 * jump > kRigDistJump, and for every group a teleport (camera jump) or region crossing.
 * The rig copy cannot be dropped: the BLAS vertex address could legally change between
 * updates (no VUID ties vertexData to the last build), but the skinned range is chosen
 * by THIS frame's GPU cull (cand.vertexOffset), unknown when the CPU records the build,
 * and the indirect build (which would take it as firstVertex) is not exposed by NVIDIA.
 *
 * TLAS. Dense 1:1 with instance ids (instanceCustomIndex = id, so consumers index
 * the instance SSBO directly); inactive entries have reference 0 (spec: inactive).
 * primitiveCount = instance count. Records are GPU-written every frame (no cached
 * transforms). REFIT (RenderVKTLASRefit, F1): built PREFER_FAST_TRACE | ALLOW_UPDATE;
 * MODE_UPDATE in place on most frames, a full BUILD every RenderVKTLASRebuildFrames
 * and whenever an update could not legally or sensibly absorb the change: count /
 * flags / record buffer changed, repack, rigged plan change, RT radius change, camera
 * jump (teleport; a region crossing re-bases mTlasBuildCam with the rows -- noteOriginShift,
 * #568 -- so its uniform translation is refit), idle gap, or
 * too many instances waiting to enter. An update may not flip instances between
 * active and inactive (VUID-vkCmdBuildAccelerationStructuresKHR-pInfos-03663/03664):
 * clean_rt_instances.comp keeps each record's activity from the last build (leaving:
 * mask 0 + a dummy 1-triangle BLAS in place; entering: deferred to the next build).
 * Mask = one class bit per instance (see clean_rt_common.glsl). HUD (flag 8) and
 * sentinel rows are never active.
 * RADIUS POLICY: an instance is in the TLAS when its LOD bounding sphere comes
 * within RenderVKRTRadius of the camera (default 256 m; terrain / water always),
 * frustum-independent so off-screen casters exist for shadows.
 *
 * SMOOTH TERRAIN (#211, llcleanterrainsmooth.h). Terrain patches publish smooth LOD
 * levels as ordinary slots, so the per-slot BLAS above already holds the smooth surface
 * -- but the raster differs from it in two ways the BLAS alone can't follow, and the ULP
 * ray origins (RenderVKRTShadowBias default 0) no longer hide either:
 *   - GEOMORPH: clean_draw.vert slides a level's newer vertices onto the next-coarser
 *     level's triangles (texcoord0.y * terrainMorph(main-camera distance)) over a band
 *     before each level switch ([6,12] / [40,56] / [90,120] m). The static BLAS holds
 *     the unmorphed level: up to the full step (cm on curved ground) off the drawn
 *     surface -- acne rings on convex ground, and along a finer patch's edge, the
 *     unmorphed edge stands above its coarser neighbour (shadow stripes on patch seams).
 *   - STAND-IN: the TLAS pass references the first level whose BLAS is BUILT (coarser
 *     first). A fine level coming into range waited for the used-bit feedback (2 frames)
 *     and the ascending lazy scan under the build budget, and meanwhile RT traced level 0
 *     -- whose vertices are exactly the 1 m faceted grid -- under a smooth raster.
 * So every frame (RenderVKTerrainSmooth on; off = nothing here runs, as before) the CPU
 * resolves each terrain instance's drawn slot with the cull's own rule (box distance ->
 * clean_terrain_lod.glsl terrainLevel -> coarser-first over the present levels; both
 * candidates within 0.5 m of a switch distance) and:
 *   - a drawn slot that MORPHS this frame (a band class with a non-zero step, scanned
 *     once per slot generation from the CPU VBO mirror, and the patch's far corner past
 *     that band's start; 2 m hysteresis) gets a MORPH BLAS: mode 3 of
 *     clean_rt_instances.comp writes the slot's vertices morphed exactly as the VS does
 *     (local space, same expressions) into mTerrPos, and the BLAS is BUILT from that
 *     (PREFER_FAST_TRACE | ALLOW_UPDATE) and REFIT whenever the main camera moved > 1 cm
 *     since its last copy (error <= step * 1 cm / band width: hundredths of a mm). It
 *     replaces the slot's static BLAS (dropped once the morph one exists) and is
 *     published through the same slot address, so pass A needs no change. Static camera:
 *     no copies, no refits.
 *   - any other drawn slot within 136 m without a BLAS is built THIS frame, ahead of the
 *     lazy scan (no stand-in past the first frame barring budget starvation).
 * Skirts stay in both BLAS kinds: they are part of the slot's triangles the raster
 * draws; with the BLAS matching the raster they sit inside the ground everywhere except
 * where the raster shows them too (a crack it covers) -- dropping them would open light
 * leaks exactly there.
 *
 * Lifetime / sync. AS + buffers retire through a ring indexed by the frame slot,
 * freed when that slot's fence has been waited again (same rule as the renderer's
 * texture retire). No vkDeviceWaitIdle anywhere here. Every GPU-read table the CPU
 * produces goes through a per-frame-slot staging copy; the compute-written buffers
 * (TLAS records, rt_geom, used bits, rig positions, rig_valid) are never CPU-written.
 * Scratch is one pooled buffer, bump-allocated per frame (grow = retire + recreate).
 */
#include "linden_common.h"
#include "../llvkhitch.h"

#include "llcleanrtvk.h"
#include "llcleanrtomm.h"
#include "llcleanclusterrtvk.h"   // TODO(2b) cluster LOD ray tracing (#748)
#include "llcleanclustervk.h"     // #747 / #748 rigged cluster RT: the switched avatars (rig plan)
#include "llcleanclusterselectvk.h"
#include "../llcleanclusterformat.h"
#include "llcleanrenderervk.h"
#include "llcleanrigdecimate.h"   // #561: the rigged plan replays the cull's decimated step
#include "../llcleanqvertex.h"    // #573: quantized static vertices (BLAS decode transform)
#include "vk_pipeline.h"
#include "vk_world_shaders_spv.h"

#include "llerror.h"

#if defined(LL_HAS_CLEAN_PIPELINE)
#  include "llviewercontrol.h"
#  include "llcleananimstore.h"     // slotWorldClean (rigged refit gate)
extern U32 gFrameCount;             // llappviewer.cpp (rigged refit gate)
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iterator>
#include <sstream>

namespace
{
// Byte offsets inside the renderer's GPU structs (static_asserted there).
constexpr size_t kInstBytes = 224, kInstGroup = 176, kInstFlags = 184, kInstSkin = 188;
constexpr size_t kSlotBytes = 32;
constexpr uint32_t kSentinel = 0xFFFFFFFFu;
constexpr uint32_t kSkinMetaWords = 1892u;       // LLCleanSkinStore::SkinMeta / 4
constexpr uint32_t kAvVisSlots = kAvatarSlotCap;   // (= LLCleanRendererVK::kAvVisSlots)
constexpr uint32_t kRigRebuildFrames = 90;
// rig_pos row (clean_rt_instances.comp rigCopy, clean_rt_common.glsl RT_RIGPOS_WORDS):
// fp32 xyz -- the rig BLAS's vertex stream -- + the skinned vertex's packed normal word
// (the RT shadow-terminator lift's shading normals, clean_shadow_rt.glsl).
constexpr uint32_t kRigPosStride = 16;
constexpr uint32_t kRigForceRefit = 32;          // refit gate: refit at least this often
constexpr uint32_t kTlasDeferRebuild = 16;       // instances waiting to enter -> early BUILD
constexpr float    kTlasCamJump = 16.f;          // camera moved this far since the BUILD (m)
constexpr uint32_t kRigKeepFrames = 120;
// #692 rigged groups out of the TLAS (LLCleanRTVK::RigWait): frames a group may wait on the
// build-scratch budget before it builds over it; frames out before the exclusion is logged;
// allocation-failure backoff (#697: first retry after kRigAllocRetry frames, doubling up to
// kRigAllocRetryMax -- the group is retried for as long as it wants in, never given up).
constexpr uint32_t kRigStarveFrames = 4;
// Rigged refits share this much update scratch per frame; one deferred longer than
// kRigRefitLag frames runs regardless (2026-10-13 working-buffer trim).
constexpr VkDeviceSize kRigRefitScratch = 128ull << 20;
constexpr uint32_t kRigRefitLag = 3;
// #1007 rigged refit POSE SKIP (recordBuild, harvestPoseMotion): a reading's displacement =
// translation + rotation x kPoseLever (a vertex this far from its joint; skirts / hair tips
// reach further, but their own joints' positions carry most of a parent's turn); the
// threshold holds up to kPoseRefMeters from the camera and grows linearly beyond; the
// staleness cap's ceiling (RenderVKRigRefitPoseMaxStale; < LLCleanRTVK::kPoseHist minus
// the readback's lag); a camera move this far in one frame (teleport) or an avatar distance
// jump this large (sit, in-sim teleport) refits every group / the avatar's groups at once.
constexpr float    kPoseLever = 0.5f;
constexpr float    kPoseRefMeters = 10.f;
constexpr uint32_t kPoseMaxStale = 8;
constexpr float    kRigDistJump = 2.f;
constexpr uint32_t kRigOutLogFrames = 60;
constexpr uint32_t kRigAllocRetry = 30;
constexpr uint32_t kRigAllocRetryMax = 600;
constexpr uint32_t kEvictAge = 8;
constexpr VkDeviceSize kChunkBytes = 64ull << 20;
// SMOOTH TERRAIN (#211). Level switch distances: MUST match clean_terrain_lod.glsl
// terrainLevel (LLCleanTerrainSmooth::kLevelDist). Morph bands per vertex class
// (terrainMorph: 0.125 m, 0.25 m, 0.5 m vertices).
constexpr float    kTerrSwitch[3] = {12.f, 56.f, 120.f};   // level 3 / 2 / 1 below these
constexpr float    kTerrBandLo[3] = {6.f, 40.f, 90.f};
constexpr float    kTerrStepEps = 1.0e-4f;       // m: below this a patch is flat for the morph
constexpr float    kTerrHyst = 2.f;              // m: morph-set hysteresis on the far distance
constexpr float    kTerrRefitMove = 0.01f;       // m: camera move that refits the morph BLAS
constexpr float    kTerrPriorityDist = 136.f;    // m: drawn slots this near get their BLAS now
inline uint32_t terrLevelCpu(float d)
{
    return d < kTerrSwitch[0] ? 3u : (d < kTerrSwitch[1] ? 2u : (d < kTerrSwitch[2] ? 1u : 0u));
}
constexpr VkDeviceSize kAsAlign = 256;
// #973 arena guard (RenderVKRTArenaGuard; llcleanrtvk.h ARENA GUARD).
constexpr uint32_t     kArenaGuardPattern = 0xA5C3E1F7u;
constexpr VkDeviceSize kArenaGuardProbe = 256;    // bytes checked at the start of each band
constexpr uint32_t     kArenaGuardMax = 1024;     // bands per check (256 KB host copy)
constexpr uint32_t     kArenaGuardEvery = 30;     // frames between checks

struct SlotInfoCPU { uint32_t first_index, index_count; int32_t base_vertex; uint32_t has_weights, vert_count, p0, p1, p2; };
static_assert(sizeof(SlotInfoCPU) == kSlotBytes, "SlotInfo mirror");

struct RtCompPush
{
    float lod_cam[4];
    float cam_time[4];
    float eye[4];
    uint32_t instance_count, slot_count, rig_count, mode;
    uint32_t idx_valid, joint_count, counter_word, dbg_on;   // dbg_on: bit 0 flicker diag, bit 1 RenderVKRTBlendCull, bit 2 rigged facing parity, bit 3 leaf facing
    float dbg_sphere[4];  // RenderVKRTDebugFlicker: focus sphere (apex, reach)
    uint32_t rig_first;   // mode 2: first rig entry of the dispatched run
    uint32_t tlas_update; // 1 = TLAS refit frame (records keep their activity)
    uint32_t dummy_lo, dummy_hi;   // placeholder BLAS address (refit frames)
    float small_size;     // pass A: the small static opaque class bound (m; RenderVKRTReflectionsSmallSize, 0 = off)
};
static_assert(sizeof(RtCompPush) == 116, "RtCompPush");
// used_bits after the slot bits: [cw] active counter, [cw + 1] deferred counter (refit
// frames: wanted but inactive at the last build), [cw + 2] rig copy entries that failed
// their check (collapsed: no RT shadow, #692), [cw + 3] the largest such instance id + 1,
// [cw + 4] #973 rig copy entries collapsed by the input check (low 16: beyond the reach,
// high 16: non-finite), [cw + 5] how many were listed, [cw + 6 .. cw + 13] their instance ids,
// then the diagnostic tail (clean_rt_instances.comp: the same layout).
constexpr uint32_t kRigBadList = 8;   // = clean_rt_instances.comp RIG_BAD_LIST
constexpr uint32_t kCounterWords = 6 + kRigBadList;
constexpr uint32_t kDbgFocusCap = 1024;
constexpr uint32_t kDbgTailWords = 19 + 2 * kDbgFocusCap;   // [cw+4 .. cw+22] + (id, hash) pairs
// clean_rt_debug.frag view 15 stats: rays, blocked, partial, vis sum, 64 x (id+1, count).
constexpr uint32_t kDbgStatsWords = 4 + 128;

struct RtDebugPush
{
    float inv_view_proj[16];
    float cam_time[4];
    float params[4];
    float params2[4];     // x raster sampler max anisotropy (ray-cone LOD rule); view 15: yzw axis; view 18: yzw light dir
    float params3[4];     // view 15: xyz projector apex, w near distance
};
static_assert(sizeof(RtDebugPush) == 128, "RtDebugPush");

bool rtDebugFlicker()
{
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<U32> s_dbg(gSavedSettings, "RenderVKRTDebugFlicker", 0);
    return (U32)s_dbg != 0u;
#else
    return false;
#endif
}

inline VkDeviceSize alignUp(VkDeviceSize v, VkDeviceSize a) { return (v + a - 1) / a * a; }

inline uint32_t rd32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }

void memBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 ss, VkAccessFlags2 sa,
                VkPipelineStageFlags2 ds, VkAccessFlags2 da)
{
    // Every access bit must be supported by some stage in its mask
    // (VUID-VkMemoryBarrier2-*AccessMask-03900..). The acceleration-structure build
    // stage supports AS read/write, SHADER_READ (build inputs) and transfer only:
    // drop shader-storage bits when no shader stage is present.
    auto sanitize = [](VkPipelineStageFlags2 st, VkAccessFlags2 ac) {
        const VkPipelineStageFlags2 shader = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
            | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT
            | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        if (!(st & shader))
        {
            ac &= ~(VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                    | VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            if (!(st & VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR))
                ac &= ~VK_ACCESS_2_SHADER_READ_BIT;
        }
        return ac;
    };
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = ss; b.srcAccessMask = sanitize(ss, sa); b.dstStageMask = ds; b.dstAccessMask = sanitize(ds, da);
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1; d.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &d);
}

// #889 SYNC A/B (RenderVKRTFullBarriers): a full memory barrier -- every earlier command's
// writes available + visible to every later command -- recorded before each BLAS build /
// refit, the TLAS build and the RT shadow mask trace. No device loss with it on = a missing
// or too-narrow barrier somewhere in the RT chain.
void fullBarrier(VkCommandBuffer cmd)
{
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1; d.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &d);
}

VkShaderModule rtModule(VkDevice dev, const uint32_t* code, size_t bytes)
{
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes; ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(dev, &ci, nullptr, &m));
    return m;
}

constexpr VkPipelineStageFlags2 kAS = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
constexpr VkPipelineStageFlags2 kCS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
constexpr VkPipelineStageFlags2 kFS = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
constexpr VkPipelineStageFlags2 kXfer = VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT;
constexpr VkAccessFlags2 kASR = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
constexpr VkAccessFlags2 kASW = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
constexpr VkAccessFlags2 kSR = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT;
constexpr VkAccessFlags2 kSW = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
} // namespace

LLCleanRTVK::LLCleanRTVK(LLCleanRendererVK& r) : mR(r), mCtx(r.mCtx) {}

LLCleanRTVK::~LLCleanRTVK() { shutdown(); }

// ===========================================================================
// init / shutdown
// ===========================================================================
bool LLCleanRTVK::init()
{
    VkMemScope memcat(VKMEM_RT_AUX);   // #407 (AS storage is RT_AS regardless)
    if (!mCtx || !mCtx->rayTracingEnabled()) return false;
    VkDevice dev = mCtx->device();

    // Compute set (clean_rt_instances.comp): 23 storage buffers (15/16 = OMM tables,
    // 17-19 = smooth terrain morph copy: static VBO, entries, positions, 20/21 = skin meta
    // + rig-cache state: the avatar-local rigged BLAS, #632, 22 = the cluster RT override, #748).
    {
        VkDescriptorSetLayoutBinding b[kCompBindings]{};
        for (uint32_t i = 0; i < kCompBindings; ++i)
        {
            b[i].binding = i; b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = kCompBindings; ci.pBindings = b;
        VK_CHECK(vkdCreateSetLayout(dev, &ci, &mDslComp, "mDslComp"));
    }
    // TODO(2b) cluster LOD RT (#748): created first -- it decides binding 0's descriptor type
    // (a partitioned TLAS for the whole session: RenderVKClusterRT 2 and the extension enabled).
    {
        uint32_t cl_mode = 2;
#if defined(LL_HAS_CLEAN_PIPELINE)
        static LLCachedControl<U32> s_cl_rt(gSavedSettings, "RenderVKClusterRT", 1);
        cl_mode = s_cl_rt;
#endif
        mClusterRT = std::make_unique<LLCleanClusterRTVK>(*this);
        if (!mClusterRT->init(cl_mode >= 2))
        {
            LL_WARNS("VKRT") << "RT: cluster LOD ray tracing unavailable (init failed); legacy BLAS only" << LL_ENDL;
            mClusterRT->shutdown();
            mClusterRT.reset();
        }
    }
    // RT consumer set (clean_rt_common.glsl, RT_SET): TLAS, 6 SSBOs, raster depth,
    // debug-view stats (binding 8, clean_rt_debug.frag view 15 only), 9 the rig map,
    // 10 the RT terminator triangle target (#664, clean_shadow_rt.glsl uTermTri: a
    // sampled R32G32_UINT image, texelFetch only), 11 / 12 the cluster page pool + the
    // tier-0 hit-cut table (#748, cluster hits; PARTIALLY_BOUND like the rest), 13 the RT skinned
    // arena of the switched avatars' cluster cuts (#747 / #748 rigged cluster hits), 14 2a's
    // SKIN_OUT (rigged cluster hits on the shared skinned cut).
    {
        // (#874: 15 the frame slot's read-guard buffer -- host-visible, written by the RT shadow
        // mask only; 16 the per-geometry vertex counts every consumer checks indices against)
        constexpr uint32_t kRtBindings = 17;
        VkDescriptorSetLayoutBinding b[kRtBindings]{};
        for (uint32_t i = 0; i < kRtBindings; ++i)
        {
            b[i].binding = i; b[i].descriptorCount = 1;
            b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
            b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        }
        b[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
#ifdef VK_NV_partitioned_acceleration_structure
        if (mClusterRT && mClusterRT->ptlasLayout())   // TODO(2b) #748
            b[0].descriptorType = VK_DESCRIPTOR_TYPE_PARTITIONED_ACCELERATION_STRUCTURE_NV;
#endif
        b[7].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[10].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        // PARTIALLY_BOUND: the lit world / particle FS (ray-traced projector shadows)
        // bind this set on EVERY draw, including frames whose TLAS build was skipped
        // (set never written, or holding a retired TLAS). Legal as long as the shader
        // doesn't dynamically touch it -- it only traces when this frame's projector
        // record says so, which the CPU sets only when tlasValid().
        VkDescriptorBindingFlags bfl[kRtBindings];
        for (uint32_t i = 0; i < kRtBindings; ++i) bfl[i] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
        VkDescriptorSetLayoutBindingFlagsCreateInfo bfi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
        bfi.bindingCount = kRtBindings; bfi.pBindingFlags = bfl;
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.pNext = &bfi;
        ci.bindingCount = kRtBindings; ci.pBindings = b;
        VK_CHECK(vkdCreateSetLayout(dev, &ci, &mDslRT, "mDslRT"));
        VkDescriptorSetLayoutCreateInfo ei{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        VK_CHECK(vkdCreateSetLayout(dev, &ei, &mDslEmpty, "mDslEmpty"));
    }
    {
        // Per frame slot: one compute set + one consumer set (sized from the layouts).
        VK_CHECK(mPool.create(dev, "rt", {{mDslComp, 3}, {mDslRT, 3}}));
        VkDescriptorSetLayout l[6] = {mDslComp, mDslComp, mDslComp, mDslRT, mDslRT, mDslRT};
        VkDescriptorSet sets[6];
        VK_CHECK(mPool.allocate(6, l, sets));
        for (int i = 0; i < 3; ++i) { mSetComp[i] = sets[i]; mSetRT[i] = sets[3 + i]; }
    }
    {
        VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(RtCompPush)};
        VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        li.setLayoutCount = 1; li.pSetLayouts = &mDslComp;
        li.pushConstantRangeCount = 1; li.pPushConstantRanges = &pr;
        VK_CHECK(vkdCreatePipelineLayout(dev, &li, &mCompLayout, "mCompLayout"));
        VkShaderModule cs = rtModule(dev, kWorldRtInstancesSpv, sizeof(kWorldRtInstancesSpv));
        mCompPipe = createComputePipeline(dev, cs, mCompLayout);
        vkDestroyShaderModule(dev, cs, nullptr);
    }
    {
        // Debug views: {empty, bindless draw set 1 (uTextures), RT set} so the helper's
        // set numbering matches future clean_draw consumers.
        VkDescriptorSetLayout sl[3] = {mDslEmpty, mR.mDslDraw1, mDslRT};
        VkPushConstantRange pr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(RtDebugPush)};
        VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        li.setLayoutCount = 3; li.pSetLayouts = sl;
        li.pushConstantRangeCount = 1; li.pPushConstantRanges = &pr;
        VK_CHECK(vkdCreatePipelineLayout(dev, &li, &mDebugLayout, "mDebugLayout"));
        VkShaderModule vs = rtModule(dev, kWorldGlowCombineVertSpv, sizeof(kWorldGlowCombineVertSpv));
        VkShaderModule fs = rtModule(dev, kWorldRtDebugFragSpv, sizeof(kWorldRtDebugFragSpv));
        GraphicsPipelineDesc d;
        d.vert = vs; d.frag = fs; d.layout = mDebugLayout;
        d.colorFormat = mR.mColorFormat; d.depthFormat = VK_FORMAT_UNDEFINED;
        d.depthTest = false; d.depthWrite = false; d.blend = false; d.cullMode = VK_CULL_MODE_NONE;
        mDebugPipe = createGraphicsPipeline(dev, d);
        vkDestroyShaderModule(dev, vs, nullptr);
        vkDestroyShaderModule(dev, fs, nullptr);
    }
    {
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR;
        qi.queryCount = 3 * kMaxBuildsPerFrame;
        VK_CHECK(vkCreateQueryPool(dev, &qi, nullptr, &mQueryPool));
    }
    mDummy = mCtx->createBuffer(256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    // Debug view 15 statistics (tiny; bound in every RT set, written only by view 15).
    mDbgStats = mCtx->createBuffer(kDbgStatsWords * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                                   | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    // #874 READ GUARD: one tiny host-visible buffer per frame slot (RT set binding 15).
    for (GpuBuffer& g : mGuard)
    {
        g = mCtx->createBuffer(kGuardWords * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (g.buffer && !g.mapped) { mCtx->destroyBuffer(g); g = GpuBuffer{}; }
        if (g.buffer) { vkAddrName(g.buffer, "rt.guard"); std::memset(g.mapped, 0, kGuardWords * 4); }
    }
    mOmmOk = mCtx->opacityMicromapEnabled() && ommSelfTest(*mCtx);
    if (mOmmOk && !ommInit())
    {
        LL_WARNS("VKRT") << "RT: opacity micromap bake pipeline failed -- micromaps disabled" << LL_ENDL;
        mOmmOk = false;
    }
    LL_INFOS("Vulkan") << "RT: foundation ready (per-slot BLAS, GPU TLAS, ray-query debug views 13/14; scratch align "
                       << mCtx->asScratchAlignment() << ")" << LL_ENDL;
    const bool ready = mCompPipe && mDebugPipe && mDummy.buffer;
    if (ready) auditRegister(true);   // #874: the DEVICE_LOST RT scene audit
    return ready;
}

bool LLCleanRTVK::tlasValid() const
{
    // Built THIS frame (recordBuild resets it on entry and ran in this frame slot) and
    // this slot's consumer set was written with it.
    return mTlasBuilt && mSlotIdx == mR.mFrameSlot % 3u && !mSetKeyRT[mSlotIdx].empty();
}

VkDescriptorSet LLCleanRTVK::consumerSet() const
{
    // The RENDERER's frame slot (not mSlotIdx, which only advances inside recordBuild):
    // set k is rewritten only in frames of slot k, after that slot's fence was waited.
    return mSetRT[mR.mFrameSlot % 3u];
}

void LLCleanRTVK::shutdown()
{
    auditRegister(false);   // #874 (before anything it reads goes)
    if (!mCtx) return;
    VkDevice dev = mCtx->device();
    const auto& rt = mCtx->rt();
    if (mClusterRT) { mClusterRT->shutdown(); mClusterRT.reset(); }   // TODO(2b) #748 (before the ring)
    for (Retire& r : mRetire) destroyRetired(r);
    destroyRetired(mPend);
    ommShutdown();
    if (mPoolOmm) { vkDestroyDescriptorPool(dev, mPoolOmm, nullptr); mPoolOmm = VK_NULL_HANDLE; }
    for (VkDescriptorSet& s : mSetOmmBake) s = VK_NULL_HANDLE;
    for (SlotBlas& s : mSlot)
        if (s.as) { rt.vkDestroyAccelerationStructureKHR(dev, s.as, nullptr); s.as = VK_NULL_HANDLE; }
    for (auto& kv : mRig)
        if (kv.second.as) rt.vkDestroyAccelerationStructureKHR(dev, kv.second.as, nullptr);
    for (auto& kv : mTerrMorph)
        if (kv.second.as) rt.vkDestroyAccelerationStructureKHR(dev, kv.second.as, nullptr);
    mTerrMorph.clear(); mTerrOwned.clear(); mTerrBytes = 0;
    mIsTerrInst.clear(); mTerrIds.clear(); mTerrIdsDirty = true;
    mRig.clear(); mSlot.clear();
    mIsRigInst.clear(); mRigIds.clear(); mPendInstRuns.clear(); mRigSetFull = true; mRigHealCursor = 0;
    mRigRowSig.clear(); mRigCands.clear(); mRigPlanValid = false; mRigGeomValid = false;
    if (mTlas) { rt.vkDestroyAccelerationStructureKHR(dev, mTlas, nullptr); mTlas = VK_NULL_HANDLE; }
    if (mDummyBlas) { rt.vkDestroyAccelerationStructureKHR(dev, mDummyBlas, nullptr); mDummyBlas = VK_NULL_HANDLE; }
    mDummyMem = Alloc{}; mDummyAddr = 0; mDummyBuiltFrame = 0; mTlasHasBuild = false;
    for (Chunk& c : mChunks) if (c.buf.buffer) mCtx->destroyBuffer(c.buf);
    mChunks.clear();
    GpuBuffer* bufs[] = {&mScratch, &mTlasInst, &mRtGeom, &mSlotAddr, &mUsedBits, &mRigEntry, &mRigValid,
                         &mRigPos, &mDummy, &mTlasBuf, &mStage[0], &mStage[1], &mStage[2],
                         &mXform[0], &mXform[1], &mXform[2],
                         &mUsedRb[0], &mUsedRb[1], &mUsedRb[2], &mDbgStats,
                         &mDbgStatsRb[0], &mDbgStatsRb[1], &mDbgStatsRb[2], &mDummyVerts,
                         &mTerrEntry, &mTerrPos, &mAuditRb[0], &mAuditRb[1], &mAuditRb[2],
                         &mRtGeomVc, &mGuard[0], &mGuard[1], &mGuard[2],
                         &mArenaGuardRb[0], &mArenaGuardRb[1], &mArenaGuardRb[2],
                         &mPoseSnap, &mPoseRb[0], &mPoseRb[1], &mPoseRb[2]};   // #1007
    for (GpuBuffer* b : bufs) if (b->buffer) mCtx->destroyBuffer(*b);
    for (bool& v : mPoseRbValid) v = false;
    mArenaGuards.clear(); mArenaGuardFills.clear();   // #973
    for (uint32_t i = 0; i < 3; ++i) { mArenaGuardProbes[i].clear(); mArenaGuardToken[i] = 0; }
    for (GuardMeta& g : mGuardMeta) g = GuardMeta{};
    auditInvalidate();
    if (mQueryPool) vkDestroyQueryPool(dev, mQueryPool, nullptr);
    if (mCompPipe) vkDestroyPipeline(dev, mCompPipe, nullptr);
    if (mDebugPipe) vkDestroyPipeline(dev, mDebugPipe, nullptr);
    if (mCompLayout) vkDestroyPipelineLayout(dev, mCompLayout, nullptr);
    if (mDebugLayout) vkDestroyPipelineLayout(dev, mDebugLayout, nullptr);
    mPool.destroy();
    if (mDslComp) vkdDestroySetLayout(dev, mDslComp);
    if (mDslRT) vkdDestroySetLayout(dev, mDslRT);
    if (mDslEmpty) vkdDestroySetLayout(dev, mDslEmpty);
    mQueryPool = VK_NULL_HANDLE; mCompPipe = mDebugPipe = VK_NULL_HANDLE;
    mCompLayout = mDebugLayout = VK_NULL_HANDLE;
    mDslComp = mDslRT = mDslEmpty = VK_NULL_HANDLE;
    mCtx = nullptr;
}

// ===========================================================================
// AS storage arena
// ===========================================================================
void LLCleanRTVK::freeAdd(Chunk& c, VkDeviceSize off, VkDeviceSize size)
{
    c.free.emplace(off, size);
    c.bySize.emplace(size, off);
}

void LLCleanRTVK::freeErase(Chunk& c, std::map<VkDeviceSize, VkDeviceSize>::iterator it)
{
    auto r = c.bySize.equal_range(it->second);
    for (auto s = r.first; s != r.second; ++s)
        if (s->second == it->first) { c.bySize.erase(s); break; }
    c.free.erase(it);
}

bool LLCleanRTVK::arenaAlloc(VkDeviceSize size, Alloc& out)
{
    size = alignUp(size, kAsAlign);
    // #973 arena guard: the band after the AS (guardable chunks only: they can be filled / read)
    const VkDeviceSize guard = mArenaGuardBytes;
    size += guard;
    for (int ci = 0; ci < (int)mChunks.size(); ++ci)
    {
        Chunk& c = mChunks[ci];
        if (!c.buf.buffer || c.dedicated) continue;
        if (guard && !c.guardable) continue;
        auto s = c.bySize.lower_bound(size);   // best fit: smallest block that holds it
        if (s == c.bySize.end()) continue;
        const VkDeviceSize off = s->second, rest = s->first - size;
        freeErase(c, c.free.find(off));
        if (rest) freeAdd(c, off + size, rest);
        c.used += size;
        out = Alloc{ci, off, size, guard};
        return true;
    }
    const bool dedicated = size > kChunkBytes / 2;
    Chunk c;
    c.size = dedicated ? size : kChunkBytes;
    c.dedicated = dedicated;
    // Micromaps (#194) share the arena with the BLAS they belong to.
    c.guardable = guard != 0;
    c.buf = mCtx->createBuffer(c.size, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR
                               | (mOmmOk ? VK_BUFFER_USAGE_MICROMAP_STORAGE_BIT_EXT : 0)
                               | (c.guardable ? VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT : 0),
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true);
    if (!c.buf.buffer) return false;
    vkAddrName(c.buf.buffer, dedicated ? "rt.asDedicated" : "rt.asChunk");
    c.used = size;
    if (c.size > size) freeAdd(c, size, c.size - size);
    int ci = -1;
    for (int i = 0; i < (int)mChunks.size(); ++i) if (!mChunks[i].buf.buffer) { ci = i; break; }
    if (ci < 0) { ci = (int)mChunks.size(); mChunks.push_back(Chunk{}); }
    mChunks[ci] = std::move(c);
    out = Alloc{ci, 0, size, guard};
    return true;
}

void LLCleanRTVK::arenaFree(const Alloc& a)
{
    if (a.chunk < 0 || a.chunk >= (int)mChunks.size()) return;
    // #973: by key, not a.guard (the cluster RT rebuilds its Allocs from chunk / off / size)
    if (!mArenaGuards.empty()) mArenaGuards.erase({a.chunk, a.off});
    Chunk& c = mChunks[a.chunk];
    c.used -= std::min(c.used, a.size);
    // #458: an emptied shared chunk stays as the one spare (rebuilding rigged BLAS freed
    // and re-allocated 64 MB chunks back to back); a second empty one is released.
    bool spare_exists = false;
    for (int ci = 0; ci < (int)mChunks.size() && !spare_exists; ++ci)
        spare_exists = ci != a.chunk && mChunks[ci].buf.buffer && !mChunks[ci].dedicated && mChunks[ci].used == 0;
    if (c.used == 0 && (c.dedicated || spare_exists || mReleased))
    {
        mCtx->destroyBuffer(c.buf);   // GPU done with it: called from the retire ring only
        c = Chunk{};
        return;
    }
    // coalesce with the neighbours, then insert the merged block (both indices)
    VkDeviceSize off = a.off, size = a.size;
    auto nx = c.free.lower_bound(off);
    if (nx != c.free.end() && off + size == nx->first) { size += nx->second; freeErase(c, nx); }
    auto pv = c.free.lower_bound(off);
    if (pv != c.free.begin())
    {
        --pv;
        if (pv->first + pv->second == off) { off = pv->first; size += pv->second; freeErase(c, pv); }
    }
    freeAdd(c, off, size);
}

VkAccelerationStructureKHR LLCleanRTVK::createAS(const Alloc& a, VkAccelerationStructureTypeKHR type,
                                                  VkDeviceAddress& addr, const char* kind, uint32_t id)
{
    VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    ci.buffer = mChunks[a.chunk].buf.buffer;
    ci.offset = a.off;
    ci.size = a.size - a.guard;   // (#973: the guard band stays outside the AS)
    ci.type = type;
    VkAccelerationStructureKHR as = VK_NULL_HANDLE;
    if (mCtx->rt().vkCreateAccelerationStructureKHR(mCtx->device(), &ci, nullptr, &as) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    ai.accelerationStructure = as;
    addr = mCtx->rt().vkGetAccelerationStructureDeviceAddressKHR(mCtx->device(), &ai);
    vkAddrNote(addr, a.size - a.guard, kind, id);
    if (a.guard)
    {
        // #973 arena guard: track the band; its pattern fill is recorded before the builds.
        ArenaGuardRec& g = mArenaGuards[{a.chunk, a.off}];
        g = ArenaGuardRec{};
        g.asSize = a.size - a.guard; g.guard = a.guard; g.id = id; g.serial = ++mArenaGuardSerial; g.frame = mFrame;
        std::strncpy(g.kind, kind ? kind : "?", sizeof(g.kind) - 1);
        mArenaGuardFills.push_back({{a.chunk, a.off}, g.serial});
    }
    return as;
}

// ===========================================================================
// #973 arena guard (llcleanrtvk.h ARENA GUARD)
// ===========================================================================
void LLCleanRTVK::arenaGuardFlushFills(VkCommandBuffer cmd)
{
    for (const auto& f : mArenaGuardFills)
    {
        const auto it = mArenaGuards.find(f.first);
        if (it == mArenaGuards.end() || it->second.serial != f.second) continue;   // freed meanwhile
        const int ci = f.first.first;
        if (ci < 0 || ci >= (int)mChunks.size() || !mChunks[ci].buf.buffer || !mChunks[ci].guardable) continue;
        vkCmdFillBuffer(cmd, mChunks[ci].buf.buffer, f.first.second + it->second.asSize, it->second.guard,
                        kArenaGuardPattern);
        it->second.filled = true;
    }
    mArenaGuardFills.clear();
}

void LLCleanRTVK::arenaGuardRecordCheck(VkCommandBuffer cmd)
{
    const uint32_t s = mSlotIdx;
    mArenaGuardProbes[s].clear();
    mArenaGuardToken[s] = 0;
    if (mArenaGuards.empty() || mFrame - mArenaGuardLastCheck < kArenaGuardEvery) return;
    if (!ensureBuf(mArenaGuardRb[s], kArenaGuardMax * kArenaGuardProbe + 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true)
        || !mArenaGuardRb[s].mapped)
        return;
    mArenaGuardLastCheck = mFrame;
    // Round robin over the bands (resume after the last one checked).
    std::vector<std::pair<int, VkBufferCopy>> cps;
    auto it = mArenaGuards.upper_bound(mArenaGuardCursor);
    for (uint32_t n = 0; n < (uint32_t)mArenaGuards.size() && cps.size() < kArenaGuardMax; ++n)
    {
        if (it == mArenaGuards.end()) it = mArenaGuards.begin();
        const ArenaGuardRec& g = it->second;
        const int ci = it->first.first;
        if (g.filled && ci >= 0 && ci < (int)mChunks.size() && mChunks[ci].buf.buffer && mChunks[ci].guardable)
        {
            VkBufferCopy c{it->first.second + g.asSize, (VkDeviceSize)cps.size() * kArenaGuardProbe,
                           std::min(g.guard, kArenaGuardProbe)};
            cps.emplace_back(ci, c);
            mArenaGuardProbes[s].push_back({ci, it->first.second, g.serial});
        }
        mArenaGuardCursor = it->first;
        ++it;
    }
    if (cps.empty()) return;
    // Every AS build / refit / compaction copy of the frame -> the copy; the stamp after it.
    memBarrier(cmd, kAS | kXfer, kASW | VK_ACCESS_2_TRANSFER_WRITE_BIT, kXfer, VK_ACCESS_2_TRANSFER_READ_BIT);
    for (const auto& c : cps) vkCmdCopyBuffer(cmd, mChunks[c.first].buf.buffer, mArenaGuardRb[s].buffer, 1, &c.second);
    memBarrier(cmd, kXfer, VK_ACCESS_2_TRANSFER_WRITE_BIT, kXfer, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    const uint32_t token = (mFrame & 0x7FFFFFFFu) | 0x80000000u;
    vkCmdFillBuffer(cmd, mArenaGuardRb[s].buffer, (VkDeviceSize)kArenaGuardMax * kArenaGuardProbe, 4, token);
    memBarrier(cmd, kXfer, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    static_cast<uint32_t*>(mArenaGuardRb[s].mapped)[kArenaGuardMax * kArenaGuardProbe / 4] = 0;   // (not stamped yet)
    mArenaGuardToken[s] = token;
    ++mArenaGuardChecks;
}

uint32_t LLCleanRTVK::arenaGuardCompare(uint32_t s, bool lost)
{
    if (!mArenaGuardToken[s] || !mArenaGuardRb[s].mapped) return 0;
    const uint32_t* w = static_cast<const uint32_t*>(mArenaGuardRb[s].mapped);
    const uint32_t token = mArenaGuardToken[s];
    mArenaGuardToken[s] = 0;
    char line[400];
    if (w[kArenaGuardMax * kArenaGuardProbe / 4] != token)
    {
        if (lost)
        {
            std::snprintf(line, sizeof(line), "[vktest] RT arena guard: frame slot %u's check (%zu bands) did not run "
                          "before the loss", s, mArenaGuardProbes[s].size());
            vkFaultLogLine(line);
        }
        return 0;
    }
    uint32_t bad = 0;
    for (size_t i = 0; i < mArenaGuardProbes[s].size(); ++i)
    {
        const ArenaGuardProbe& p = mArenaGuardProbes[s][i];
        const auto it = mArenaGuards.find({p.chunk, p.asOff});
        if (it == mArenaGuards.end() || it->second.serial != p.serial) continue;   // freed since (copy was valid then)
        ArenaGuardRec& g = it->second;
        const uint32_t nw = (uint32_t)(std::min(g.guard, kArenaGuardProbe) / 4);
        const uint32_t* b = w + i * (kArenaGuardProbe / 4);
        uint32_t first = nw;
        for (uint32_t k = 0; k < nw; ++k) if (b[k] != kArenaGuardPattern) { first = k; break; }
        if (first == nw) continue;
        ++bad;
        const bool fresh = !g.reported;
        g.reported = true;
        if (fresh) ++mArenaGuardBad;           // (distinct bands)
        if (!fresh && !lost) continue;         // (logged once per band; the loss report repeats it)
        if (bad > 8) continue;
        // The AS after the band (same chunk): the one that wrote BELOW its start, if not the one before.
        std::string next = "none (free space)";
        const auto nx = std::next(it);
        if (nx != mArenaGuards.end() && nx->first.first == p.chunk)
            next = std::string("'") + nx->second.kind + "' id " + std::to_string(nx->second.id) + " at +"
                 + std::to_string(nx->first.second) + (nx->first.second == p.asOff + g.asSize + g.guard ? " (adjacent)" : "");
        std::snprintf(line, sizeof(line), "[vktest] RT arena guard: band after AS '%s' id %u (chunk %d [+%llu, +%llu), "
                      "created RT frame %u) OVERWRITTEN at band +%u: 0x%08x (pattern 0x%08x) -- that AS wrote past its "
                      "end (build / refit / compaction overrun); next AS: %s",
                      g.kind, g.id, p.chunk, (unsigned long long)p.asOff, (unsigned long long)(p.asOff + g.asSize),
                      g.frame, first * 4, b[first], kArenaGuardPattern, next.c_str());
        if (lost) vkFaultLogLine(line);
        else LL_WARNS("VKRT") << line << LL_ENDL;
    }
    if (lost)
    {
        std::snprintf(line, sizeof(line), "[vktest] RT arena guard: frame slot %u: %zu bands checked, %u overwritten",
                      s, mArenaGuardProbes[s].size(), bad);
        vkFaultLogLine(line);
    }
    return bad;
}

void LLCleanRTVK::arenaGuardLostReport()
{
    if (mArenaGuards.empty() && !mArenaGuardChecks) return;
    char line[200];
    std::snprintf(line, sizeof(line), "[vktest] RT arena guard (RenderVKRTArenaGuard): %zu guarded AS, %u checks so far, "
                  "%u overwritten bands found before the loss", mArenaGuards.size(), mArenaGuardChecks, mArenaGuardBad);
    vkFaultLogLine(line);
    for (uint32_t s = 0; s < 3; ++s) arenaGuardCompare(s, true);
}

// ===========================================================================
// retire ring
// ===========================================================================
void LLCleanRTVK::retireAS(VkAccelerationStructureKHR as, const Alloc& a)
{
    if (as) mPend.as.push_back(as);
    if (a.chunk >= 0) mPend.allocs.push_back(a);
}

void LLCleanRTVK::retireBuf(GpuBuffer& b)
{
    if (b.buffer) mPend.bufs.push_back(b);
    b = GpuBuffer{};
}

void LLCleanRTVK::destroyRetired(Retire& r)
{
    if (!mCtx) return;
    for (VkAccelerationStructureKHR as : r.as)
    {
        VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
        ai.accelerationStructure = as;
        vkAddrForget(mCtx->rt().vkGetAccelerationStructureDeviceAddressKHR(mCtx->device(), &ai));
        mCtx->rt().vkDestroyAccelerationStructureKHR(mCtx->device(), as, nullptr);
    }
    for (VkMicromapEXT m : r.micromaps) mCtx->rt().vkDestroyMicromapEXT(mCtx->device(), m, nullptr);
    for (const Alloc& a : r.allocs) arenaFree(a);
    for (GpuBuffer& b : r.bufs) mCtx->destroyBuffer(b);
    r = Retire{};
}

void LLCleanRTVK::shrinkIfOversized(GpuBuffer& b, VkDeviceSize need, uint32_t& over_frames)
{
    if (b.buffer && b.size > (16ull << 20) && b.size > std::max<VkDeviceSize>(need, 1) * 4)
    {
        if (++over_frames >= 300) { retireBuf(b); over_frames = 0; }
    }
    else over_frames = 0;
}

bool LLCleanRTVK::ensureBuf(GpuBuffer& b, VkDeviceSize need, VkBufferUsageFlags usage, bool host, bool addr)
{
    VkMemScope memcat(VKMEM_RT_AUX);   // #407
    need = std::max<VkDeviceSize>(need, 256);
    if (b.buffer && b.size >= need) return true;
    retireBuf(b);
    // Host buffers: HOST_CACHED is a preference (createBuffer falls back) -- the used-slot
    // readback is scanned by the CPU every frame and uncached reads stall.
    // (scratch: +1/8 -- a 1 GB build frame left 1.5 GB of scratch; the window rule shrinks it)
    b = mCtx->createBuffer(need + (&b == &mScratch ? need / 8 : need / 2), usage,
                           host ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                   | ((usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT) ? VK_MEMORY_PROPERTY_HOST_CACHED_BIT : 0))
                                : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, addr);
    if (b.buffer)
    {
        const char* nm = &b == &mScratch ? "rt.scratch" : &b == &mTlasInst ? "rt.tlasInst"
                       : &b == &mRtGeom ? "rt.geom" : &b == &mRtGeomVc ? "rt.geomVc" : &b == &mSlotAddr ? "rt.slotAddr"
                       : &b == &mUsedBits ? "rt.usedBits" : &b == &mRigEntry ? "rt.rigEntry"
                       : &b == &mRigValid ? "rt.rigValid" : &b == &mRigPos ? "rt.rigPos"
                       : (&b >= &mStage[0] && &b <= &mStage[2]) ? "rt.stage"
                       : (&b >= &mXform[0] && &b <= &mXform[2]) ? "rt.blasXform"
                       : (&b >= &mUsedRb[0] && &b <= &mUsedRb[2]) ? "rt.usedRb"
                       : (&b >= &mAuditRb[0] && &b <= &mAuditRb[2]) ? "rt.auditRb"
                       : &b == &mPoseSnap ? "rt.poseSnap"
                       : (&b >= &mPoseRb[0] && &b <= &mPoseRb[2]) ? "rt.poseRb" : "rt.buf";
        vkAddrName(b.buffer, nm);
    }
    return b.buffer != VK_NULL_HANDLE;
}

// ===========================================================================
// slot bookkeeping
// ===========================================================================
void LLCleanRTVK::setSlotAddr(uint32_t sl, VkDeviceAddress a)
{
    if ((size_t)sl * 2 + 1 >= mSlotAddrCpu.size()) return;
    mSlotAddrCpu[sl * 2] = (uint32_t)(a & 0xFFFFFFFFull);
    mSlotAddrCpu[sl * 2 + 1] = (uint32_t)(a >> 32);
    mAddrDirtyLo = std::min(mAddrDirtyLo, sl);
    mAddrDirtyHi = std::max(mAddrDirtyHi, sl + 1);
}

bool LLCleanRTVK::slotIndicesOk(uint32_t sl, uint32_t first_index, uint32_t index_count, uint32_t vert_count)
{
    if (mIdxCheckGen.size() <= sl) { mIdxCheckGen.resize((size_t)sl + 1, 0); mIdxBad.resize((size_t)sl + 1, 0); }
    const uint32_t gen = (sl < mSlotGen.size() ? mSlotGen[sl] : 0u) + 1u;
    if (mIdxCheckGen[sl] != gen)
    {
        // #584: the slot's indices from the mesh store mirror (no CPU image of the IBO). A
        // row the mirror no longer matches is not OK this frame (not cached: it is re-placed).
        const uint8_t* sv = nullptr; const uint16_t* ix = nullptr; const uint8_t* sw = nullptr;
        if (!mR.geomSlotSource(sl, sv, ix, sw)) return false;
        uint32_t mx = 0;
        for (uint32_t i = 0; i < index_count; ++i) mx = std::max<uint32_t>(mx, ix[i]);
        mIdxBad[sl] = mx >= vert_count ? 1 : 0;
        mIdxCheckGen[sl] = gen;
        if (mIdxBad[sl] && mIdxBadLogged < 32)
        {
            ++mIdxBadLogged;
            LL_WARNS("VKRT") << "RT: slot " << sl << " has index " << mx << " >= vert_count " << vert_count
                             << " (first_index " << first_index << ", " << index_count
                             << " indices) -- no BLAS (would read past its vertices)" << LL_ENDL;
        }
    }
    return mIdxBad[sl] == 0;
}

void LLCleanRTVK::dropSlotBlas(uint32_t sl)
{
    if (sl >= mSlot.size()) return;
    SlotBlas& s = mSlot[sl];
    if (!s.as) return;
    retireAS(s.as, s.mem);
    mStaticBytes -= std::min<uint64_t>(mStaticBytes, s.mem.size);
    --mStaticCount;
    s.as = VK_NULL_HANDLE; s.mem = Alloc{}; s.addr = 0; s.compacted = false;
    ++s.serial;
    setSlotAddr(sl, 0);
    // still inside the build window: the static build may rebuild it this frame (as the
    // full scan it replaced would)
    if (s.lastUsed != 0 && s.lastUsed + 2 >= mFrame) wantSlot(sl);
}

void LLCleanRTVK::wantSlot(uint32_t sl)
{
    if (sl >= mSlot.size() || mSlot[sl].as) return;
    if (mInWant.size() <= sl) mInWant.resize(std::max<size_t>(mSlot.size(), (size_t)sl + 1), 0);
    if (mInWant[sl]) return;
    mInWant[sl] = 1;
    mWant.push_back(sl);
}

void LLCleanRTVK::dropTerrMorph(uint32_t sl)
{
    if (sl >= mTerrOwned.size() || !mTerrOwned[sl]) return;
    mTerrOwned[sl] = 0;
    auto it = mTerrMorph.find(sl);
    if (it == mTerrMorph.end()) return;
    if (it->second.as)
    {
        retireAS(it->second.as, it->second.mem);
        mStaticBytes -= std::min<uint64_t>(mStaticBytes, it->second.mem.size);
        mTerrBytes -= std::min<uint64_t>(mTerrBytes, it->second.mem.size);
    }
    mTerrMorph.erase(it);
    setSlotAddr(sl, sl < mSlot.size() ? mSlot[sl].addr : 0);
}

void LLCleanRTVK::slotChanged(uint32_t sl, bool from_repack)
{
    if (sl >= mSlotGen.size()) mSlotGen.resize((size_t)sl + 1, 0);
    ++mSlotGen[sl];
    // C3: the rigged plan read this slot's contents -> rebuild it. (Only plan slots: flexi
    // and other static slots change every frame and must not defeat the plan reuse.)
    if (sl < mRigPlanSlot.size() && mRigPlanSlot[sl]) ++mSlotGenTotal;
    if (sl < mSlot.size())
    {
        if (mSlot[sl].as && from_repack) { mSlot[sl].repackInvalidated = true; ++mRepackInvalidPending; }
        dropSlotBlas(sl);
    }
    dropTerrMorph(sl);   // smooth terrain: copied the old vertices too (the plan re-adds it)
    if (sl >= mSlotChangedAt.size()) mSlotChangedAt.resize((size_t)sl + 1, 0);
    mSlotChangedAt[sl] = mFrame;
    ommSlotChanged(sl);   // its micromap BLAS copied the old triangles
}

// ===========================================================================
// per frame
// ===========================================================================
void LLCleanRTVK::beginFrame()
{
    ++mFrame;
    mSlotIdx = mR.mFrameSlot % 3u;
    // This slot's fence was waited: what it retired (and everything before) is free.
    destroyRetired(mRetire[mSlotIdx]);
    mRetire[mSlotIdx] = std::move(mPend);
    mPend = Retire{};
    mBuiltThisFrame = mRepackRebuilt = mRefits = mRigBuilds = mCompactions = mStarved = 0;
}

// ---- CLUSTER LOD LEGACY-SKIN SKIP: cluster coverage of the switched avatars in RT ----
// This frame slot's last recorded TLAS pass B (fence waited): per switched group, mode 4's
// "fell back" flag (clean_rt_instances.comp clRigGroupCheck: some member had no cluster record
// that frame -- the group traced its legacy BLAS, or nothing when it was cluster-only). An avatar
// whose groups were all covered extends its streak; any fallback, or no switched group of it in
// that frame, resets it (renderer clusterLegacySkipPlan: kClNeedRt until the streak is back).
void LLCleanRTVK::harvestClusterCoverage()
{
    const uint32_t s = mSlotIdx;
    // (mode 4 not recorded for a plan that had switched groups: nothing new to compare -- keep the
    // last harvest. A plan WITHOUT any switched group covered nothing: every expected avatar of it
    // is a hole -- the regression the consistency check is for.)
    if (!mClCovValid[s] && !mClCov[s].empty()) return;
    const bool valid = mClCovValid[s];
    mClCovValid[s] = false;
    uint32_t seen[kAvVisSlots / 32] = {}, bad[kAvVisSlots / 32] = {};
    if (valid)
    {
        const uint32_t entries = mClCovEntries[s];
        const uint32_t* w = mClusterRT ? mClusterRT->tlasRigWords(s, entries) : nullptr;
        if (!w) return;
        const uint32_t* flags = w + LLCleanClusterRTVK::kTlasRigHdr + entries;
        for (const ClCovGroup& g : mClCov[s])
        {
            if (g.av >= kAvVisSlots || g.head >= entries) continue;
            seen[g.av >> 5] |= 1u << (g.av & 31u);
            if (flags[g.head] != 0u) bad[g.av >> 5] |= 1u << (g.av & 31u);
        }
    }
    const ClExpect& ex = mClExpect[s];
    mClExpLast = ex;
    mClCovIn = mClCovFell = 0;
    mClExpN = mClHoleFell = mClHoles = 0;
    for (uint32_t i = 0; i < kAvVisSlots / 32; ++i)
    {
        mClCovIn += (uint32_t)__builtin_popcount(seen[i] & ~bad[i]);
        mClCovFell += (uint32_t)__builtin_popcount(bad[i]);
        mClExpN += (uint32_t)__builtin_popcount(ex.exp[i]);
        mClHoleFell += (uint32_t)__builtin_popcount(ex.exp[i] & bad[i]);
        mClHoles += (uint32_t)__builtin_popcount(ex.exp[i] & ~(seen[i] & ~bad[i]));
    }
    if (!valid) return;
    for (uint32_t a = 0; a < kAvVisSlots; ++a)
    {
        const uint32_t m = 1u << (a & 31u);
        if ((seen[a >> 5] & m) && !(bad[a >> 5] & m))
        {
            mClCovStreak[a] = std::min<uint32_t>(mClCovStreak[a] + 1u, 1u << 20);
            mClCovFrame[a] = mFrame;
        }
        else mClCovStreak[a] = 0;
    }
}

// CLUSTER RT -> TLAS CONSISTENCY (2026-10-14): every avatar the cluster path draws that the legacy
// RT rules would trace (ClExpect::exp, decided with the plan of that frame) must be traced from its
// cluster cut: pass B covered all its switched groups. Avatars excluded by rule (no-shadow reduced
// tier, seen by no pass) are not compared -- they are counted per reason on the "RT avatars" line.
// Both sides are of the same frame slot; only holes lasting kClConsistFrames harvests are reported.
void LLCleanRTVK::checkClusterConsistency()
{
    const bool on = mClusterRT && clusterTracesSwitched();
    mClConsistReady = on ? mClusterRT->rigAvatarsIn() : 0u;
    if (!on || mClHoles == 0u)
    {
        if (mClConsistWarned)
            LL_INFOS("VKRT") << "RT avatars consistent again: all " << mClExpN
                             << " cluster-path avatars the RT rules trace are traced from their cluster cut" << LL_ENDL;
        mClConsistStreak = 0;
        mClConsistWarned = false;
        return;
    }
    ++mClConsistStreak;
    if (mClConsistStreak == kClConsistFrames
        || (mClConsistStreak > kClConsistFrames && (mClConsistStreak - kClConsistFrames) % kClConsistRelog == 0u))
    {
        mClConsistWarned = true;
        const ClExpect& ex = mClExpLast;
        LL_WARNS("VKRT") << "RT avatars INCONSISTENT for " << mClConsistStreak << " frames: " << mClHoles << " of "
                         << mClExpN << " cluster-path avatars the RT rules trace cast no ray-traced shadow (not in the RT plan "
                         << ex.not_plan << ": skin " << ex.np_skin << ", row " << ex.np_row << ", other " << ex.np_other
                         << "; cluster records missing / fell back " << mClHoleFell << "; cluster RT data ready for "
                         << mClConsistReady << ")" << LL_ENDL;
    }
}

uint64_t LLCleanRTVK::clusterRtBytes() const
{
    return mClusterRT ? mClusterRT->asBytes() + mClusterRT->auxBytes() : 0;
}

std::string LLCleanRTVK::clusterRtMemLine() const
{
    return mClusterRT ? mClusterRT->memLine() : std::string("off");
}

bool LLCleanRTVK::clusterTracesSwitched() const
{
    // (the module's tier of the last plan: -1 = inactive; every tier traces the switched groups'
    // cut -- tier 0 through its per-cut BLAS)
    return mClusterRT && mClusterRT->riggedWanted() && mClusterRT->tierNow() >= 0;
}

bool LLCleanRTVK::clusterRigCovered(uint32_t av) const
{
    if (av >= kAvVisSlots || !mClusterRT || !mClusterRT->riggedWanted()) return false;
    // (tier 0: the per-cut BLAS follow the read-back cut, and a slot evicted on its build frame
    // keeps the instance legacy until a rebuild -- its legacy BLAS stays the fallback)
    if (mClusterRT->tierNow() < 1) return false;
    return mClCovStreak[av] >= kClCovStreak && mFrame - mClCovFrame[av] <= kClCovMaxAge;
}

void LLCleanRTVK::releaseToConsumers(VkCommandBuffer cmd, VkPipelineStageFlags2 stages)
{
    // See recordBuild (the deferred TLAS release) for why src = AS build is enough.
    stages &= mReleaseOwed;
    if (!stages || !cmd) return;
    memBarrier(cmd, kAS, kASW, stages, kASR | kSR);
    mReleaseOwed &= ~stages;
}

void LLCleanRTVK::idleFrame()
{
    mTlasBuilt = false;   // tlasValid() false: no consumer may see a stale TLAS / set
    mReleaseOwed = 0;     // nothing built: nothing to release
    if (!mCtx) return;
    // Retire ring: same rotation as beginFrame (this slot's fence was waited), so
    // slotChanged retirements during a long idle stretch are still freed. mFrame does
    // NOT advance: LRU / rig ages pause while nothing traces.
    mSlotIdx = mR.mFrameSlot % 3u;
    destroyRetired(mRetire[mSlotIdx]);
    mRetire[mSlotIdx] = std::move(mPend);
    mPend = Retire{};
    // Used-slot feedback is KEPT (consumed on this slot's next active frame): a consumer
    // flickering on/off must still get its static BLAS built; stale feedback only marks
    // slots the TLAS pass wanted before the gap (mFrame is paused, so they build promptly).
    mIdleSinceBuild = true;
    // Legacy-skin skip: nothing traced -- every avatar must confirm its cluster cut afresh.
    std::memset(mClCovStreak, 0, sizeof(mClCovStreak));
    for (bool& v : mClCovValid) v = false;
    mClConsistStreak = 0;   // (the consistency check restarts with tracing)
    // Idle long enough (15 s: not a brief gap with no projector in view) -> free it all.
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (mIdleSince == 0.0) mIdleSince = now;
    else if (!mReleased && (mReleaseAsap || now - mIdleSince > 15.0)) releaseAll();
}

void LLCleanRTVK::releaseAll()
{
    const uint64_t before = mStaticBytes + mRigBytes;
    if (mClusterRT) mClusterRT->releaseAll();   // TODO(2b) #748
    for (uint32_t i = 0; i < (uint32_t)mSlot.size(); ++i) dropSlotBlas(i);
    ommReleaseAll();
    for (GpuBuffer* b : {&mOmmAddr, &mOmmInst, &mOmmUp[0], &mOmmUp[1], &mOmmUp[2], &mOmmData[0], &mOmmData[1], &mOmmData[2]})
        retireBuf(*b);
    for (auto& kv : mRig) retireAS(kv.second.as, kv.second.mem);
    mRig.clear();
    mRigBytes = 0;
    mRigWait.clear();   // #692: every group rebuilds from scratch on the next active frame
    mRigInvalidFrames = 0;
    {
        // (dropTerrMorph also zeroes the slot address: a recreated slot-address buffer is
        // filled from mSlotAddrCpu, which must not name a retired morph BLAS.)
        std::vector<uint32_t> ts;
        ts.reserve(mTerrMorph.size());
        for (const auto& kv : mTerrMorph) ts.push_back(kv.first);
        for (const uint32_t sl : ts) dropTerrMorph(sl);
        mTerrMorph.clear(); mTerrOwned.clear(); mTerrBytes = 0;
    }
    retireBuf(mTerrEntry); retireBuf(mTerrPos);
    for (auto& c : mCompact) c.clear();   // their serials no longer match anything
    if (mTlas) { mPend.as.push_back(mTlas); mTlas = VK_NULL_HANDLE; }
    retireBuf(mTlasBuf);
    mTlasCap = 0; mTlasScratch = 0;
    mTlasHasBuild = false;
    if (mDummyBlas) retireAS(mDummyBlas, mDummyMem);
    mDummyBlas = VK_NULL_HANDLE; mDummyMem = Alloc{}; mDummyAddr = 0; mDummyBuiltFrame = 0;
    retireBuf(mDummyVerts);
    for (GpuBuffer* b : {&mScratch, &mTlasInst, &mRtGeom, &mRtGeomVc, &mSlotAddr, &mUsedBits, &mRigEntry, &mRigValid, &mRigPos})
        retireBuf(*b);
    for (int i = 0; i < 3; ++i) { retireBuf(mStage[i]); retireBuf(mXform[i]); retireBuf(mUsedRb[i]); mUsedRbValid[i] = false; }
    for (int i = 0; i < 3; ++i) { retireBuf(mArenaGuardRb[i]); mArenaGuardProbes[i].clear(); mArenaGuardToken[i] = 0; }   // #973
    for (GpuBuffer& b : mAuditRb) retireBuf(b);   // #874
    retireBuf(mPoseSnap);   // #1007
    for (int i = 0; i < 3; ++i) { retireBuf(mPoseRb[i]); mPoseRbValid[i] = false; }
    auditInvalidate();
    mRigPosAddrSeen = 0;
    mRigEntries.clear(); mRigEntries.shrink_to_fit();
    mRigGeoms.clear(); mRigGeoms.shrink_to_fit();
    mRigRanges.clear(); mRigRanges.shrink_to_fit();
    mRigCands.clear(); mRigPlanValid = false; mRigGeomValid = false;   // C3
    mReleased = true;
    // (#268) The pressure ladder's state (mPressureCap, mRigPressureDrop) survives the
    // release: resetting it here brought RT back at full size 15 s after every pressure
    // episode, which re-filled VRAM and re-triggered the pressure (sun shadows flapping
    // with every view change). Only the renderer's calm-period restore lifts it.
    mRigDropped = false; mOverBudget = false;
    mRigDropBytes = 0; mRigDropRows = 0; mRigRoomFrames = 0;
    mReleaseAsap = false;
    markCold("idle release");
    LL_INFOS("VKRT") << "RT: idle for 15 s -- released " << (before >> 20)
                     << " MB of BLAS + TLAS/scratch/aux buffers (rebuilt on next use)" << LL_ENDL;
}

// #1007 POSE MOTION READINGS: this frame slot's last mode-5 output (its fence was waited) ->
// mPoseHist[slot], stamped with the mFrame that measured it. Nothing to read (mode 5 not
// recorded there, a fresh snapshot then) leaves the history as it is: the pose skip treats a
// frame without a reading as moving like the newest readings it has.
void LLCleanRTVK::harvestPoseMotion()
{
    const uint32_t s = mSlotIdx;
    const bool valid = mPoseRbValid[s];
    mPoseRbValid[s] = false;
    if (!valid || !mPoseRb[s].mapped) return;
    const uint32_t* w = static_cast<const uint32_t*>(mPoseRb[s].mapped);
    const uint32_t n = std::min<uint32_t>(mPoseRbSlots[s], (uint32_t)(mPoseRb[s].size / 8));
    if (mPoseHist.size() < n) mPoseHist.resize(n);
    const uint32_t f = mPoseRbFrame[s], k = f % kPoseHist;
    for (uint32_t a = 0; a < n; ++a)
    {
        float t = 0.f, r = 0.f;
        std::memcpy(&t, &w[2 * a], 4);
        std::memcpy(&r, &w[2 * a + 1], 4);
        const float d = t + r * kPoseLever;
        mPoseHist[a].frame[k] = f;
        mPoseHist[a].d[k] = std::isfinite(d) ? d : 1e30f;
    }
}

void LLCleanRTVK::readFeedback()
{
    const uint32_t s = mSlotIdx;
    if (!mUsedRbValid[s] || !mUsedRb[s].mapped) return;
    const uint32_t* w = static_cast<const uint32_t*>(mUsedRb[s].mapped);
    const uint32_t words = mUsedRbWords[s];
    mTlasActive = w[words];
    mTlasDeferred = w[words + 1];
    // #692: rig copy entries collapsed this frame (no RT shadow) + the largest such id + 1
    mRigInvalid = w[words + 2] & 0xFFFFu;        // the cull drew another slot / source (a real mismatch)
    mRigUndrawn = w[words + 2] >> 16;            // the cull drew the face nowhere (nothing to trace)
    mRigInvalidId = w[words + 3];
    noteRigInvalid();
    // #973 rig copy input check: entries collapsed for non-finite / out-of-reach positions.
    // Their groups are rebuilt from scratch on their next copy (RigBlas::forceRebuild): never a
    // refit chain from the collapsed BVH.
    mRigBadFar = w[words + 4] & 0xFFFFu;
    mRigBadNonFinite = w[words + 4] >> 16;
    if (mRigBadFar || mRigBadNonFinite)
    {
        mRigBadTotal += mRigBadFar + mRigBadNonFinite;
        ++mRigBadFrames;
        const uint32_t listed = std::min(w[words + 5], kRigBadList);
        std::string ids;
        for (uint32_t k = 0; k < listed; ++k)
        {
            const uint32_t id = w[words + 6 + k];
            ids += (k ? ", " : "") + std::to_string(id);
            for (size_t i = 0; i < mRigEntries.size() && i < mRigCands.size(); ++i)
            {
                if (mRigEntries[i].inst != id) continue;
                const auto it = mRig.find(mRigCands[i].key);
                if (it != mRig.end() && !it->second.forceRebuild) { it->second.forceRebuild = true; ++mRigBadRebuilds; }
                break;
            }
        }
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if (now >= mRigBadLogNext)
        {
            mRigBadLogNext = now + 10.0;
            LL_WARNS("VKRT") << "RT: rig copy input check (#973): " << mRigBadNonFinite << " rigged faces with non-finite "
                             << "and " << mRigBadFar << " with out-of-reach (> 1 km) skinned positions collapsed this frame "
                             << "(instance ids " << ids << (w[words + 5] > listed ? ", ..." : "") << "); " << mRigBadTotal
                             << " in " << mRigBadFrames << " frames so far, groups marked for rebuild " << mRigBadRebuilds << LL_ENDL;
        }
    }
    // The tail's t[0] is the last counter word (readDebugTail reads t[1..] only).
    if (mUsedRbDbg[s]) readDebugTail(w + words + kCounterWords - 1, mUsedRbRigs[s]);
    else mDbgHavePrev = false;
    const uint32_t nslot = (uint32_t)mSlot.size();
    for (uint32_t i = 0; i < words; ++i)
    {
        uint32_t bits = w[i];
        while (bits)
        {
            const uint32_t b = (uint32_t)__builtin_ctz(bits);
            bits &= bits - 1;
            const uint32_t sl = i * 32 + b;
            if (sl < nslot)
            {
                mSlot[sl].lastUsed = mFrame;
                if (!mSlot[sl].as) wantSlot(sl);
            }
        }
    }
    mUsedRbValid[s] = false;
}

void LLCleanRTVK::evictForBudget(VkDeviceSize need, uint64_t budget, bool force)
{
    if (mStaticBytes + mRigBytes + need <= budget) return;
    std::vector<std::pair<uint32_t, uint32_t>> cand;   // (lastUsed, slot)
    cand.reserve(mStaticCount);
    for (uint32_t i = 0; i < mSlot.size(); ++i)
        if (mSlot[i].as && (force || mSlot[i].lastUsed + kEvictAge < mFrame)) cand.emplace_back(mSlot[i].lastUsed, i);
    std::sort(cand.begin(), cand.end());
    const uint64_t target = budget - std::min<uint64_t>(budget, need) ;
    uint32_t evicted = 0;
    for (const auto& c : cand)
    {
        if (mStaticBytes + mRigBytes <= target * 9 / 10) break;
        dropSlotBlas(c.second);
        ++evicted;
    }
    if (evicted) LLVKHitch::count("rt_blasEvicted", evicted);
}

void LLCleanRTVK::updateSets(uint32_t s)
{
    VkDevice dev = mCtx->device();
    auto B = [&](const GpuBuffer& b) { return b.buffer ? b.buffer : mDummy.buffer; };
    const GpuBuffer& joints = mR.jointTable();
    // ---- compute set ----
    // OMM tables (15 / 16): the 256 B zeroed dummy when micromaps are off = "none".
    VkBuffer cb[kCompBindings] = {B(mR.mInstanceSSBO), B(mR.mSlotSSBO), B(mR.mLodGroupSSBO), B(joints), B(mR.mAvVisSSBO),
                       B(mSlotAddr), B(mUsedBits), B(mTlasInst), B(mRtGeom), B(mRigEntry), B(mRigValid),
                       B(mR.mCand), B(mR.mCandMeta), B(mR.mSkinVbo), B(mRigPos),
                       mOmmTablesOn ? B(mOmmInst) : mDummy.buffer, mOmmTablesOn ? B(mOmmAddr) : mDummy.buffer,
                       B(mR.mVbo), B(mTerrEntry), B(mTerrPos),   // 17-19: smooth terrain morph copy
                       B(mR.mSkinMetaSSBO), B(mR.mRigCache),     // 20-21: avatar-local rigged BLAS (#632)
                       // 22: TODO(2b) cluster RT override rows (#748; the zeroed dummy = none)
                       (mClusterRT && mClusterRT->overrideBuffer()) ? mClusterRT->overrideBuffer() : mDummy.buffer,
                       // 23: the cluster-switched rigged groups of pass B (#747 / #748; dummy = none)
                       (mClusterRT && mClusterRT->tlasRigBuffer()) ? mClusterRT->tlasRigBuffer() : mDummy.buffer,
                       // 24: per-geometry vertex counts (#874 read guard)
                       B(mRtGeomVc),
                       // 25 / 26: #1007 rigged pose motion (mode 5): joint snapshot, this slot's readback
                       B(mPoseSnap), B(mPoseRb[s])};
    // REWRITTEN EVERY FRAME (this slot's fence was waited; ~21 writes). Never skip on a
    // "same handles" key: a destroyed + recreated buffer can come back with the SAME
    // VkBuffer value, and a skipped rewrite then leaves the descriptor on freed memory --
    // the rig copy read the grown pre-skin output's OLD allocation (DEVICE_LOST,
    // READ_INVALID, mapped by the fault registry to a freed ensureRiggedBuffers buffer).
    std::vector<uint64_t> key;
    for (VkBuffer b : cb) key.push_back((uint64_t)(uintptr_t)b);
    {
        VkDescriptorBufferInfo bi[kCompBindings];
        VkWriteDescriptorSet w[kCompBindings];
        for (uint32_t i = 0; i < kCompBindings; ++i)
        {
            bi[i] = {cb[i], 0, VK_WHOLE_SIZE};
            w[i] = VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w[i].dstSet = mSetComp[s]; w[i].dstBinding = i; w[i].descriptorCount = 1;
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[i].pBufferInfo = &bi[i];
        }
        vkUpdateDescriptorSets(dev, kCompBindings, w, 0, nullptr);
        mSetKeyComp[s] = key;
    }
    // ---- RT consumer set ----
    VkBuffer rb[6] = {B(mR.mInstanceSSBO), B(mR.mMaterialSSBO), B(mRtGeom), B(mR.mVbo), B(mR.mIbo), B(mRigPos)};
    VkImageView depthView = mR.mPrevDepth.view;
    std::vector<uint64_t> rkey;
    rkey.push_back((uint64_t)(uintptr_t)mTlas);
    for (VkBuffer b : rb) rkey.push_back((uint64_t)(uintptr_t)b);
    rkey.push_back((uint64_t)(uintptr_t)depthView);
    rkey.push_back(mR.mTargetGen);   // the depth view's handle can be reused (see mTargetGen)
    // Every frame (see the compute set). TLAS + SSBOs whenever the TLAS exists (the
    // lit FS's projector shadow rays depend on "tlasValid() => this slot's set holds
    // this frame's TLAS + buffers"); the raster-depth sampler (debug views only) only
    // when it exists -- the layout is PARTIALLY_BOUND.
    mSetKeyRT[s].clear();
    // TODO(2b) #748: a partitioned-TLAS session binds the PTLAS (by address) at binding 0; none
    // sized yet = the set is not written (tlasValid() false: the consumers fall back).
    const bool ptlas = mClusterRT && mClusterRT->ptlasLayout();
    const VkDeviceAddress ptlas_addr = ptlas ? mClusterRT->ptlasAddress() : 0;
    if (ptlas) rkey.push_back(ptlas_addr);
    if (mTlas && (!ptlas || ptlas_addr))
    {
        VkWriteDescriptorSetAccelerationStructureKHR wa{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
        wa.accelerationStructureCount = 1; wa.pAccelerationStructures = &mTlas;
        VkDescriptorBufferInfo bi[14];
        VkDescriptorImageInfo ii{mR.mDepthSampler, depthView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w[17];
        for (uint32_t i = 0; i < 17; ++i)
        {
            w[i] = VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w[i].dstSet = mSetRT[s]; w[i].descriptorCount = 1;
        }
        w[0].dstBinding = 0;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR; w[0].pNext = &wa;
#ifdef VK_NV_partitioned_acceleration_structure
        VkWriteDescriptorSetPartitionedAccelerationStructureNV wp{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_PARTITIONED_ACCELERATION_STRUCTURE_NV};
        if (ptlas)
        {
            wp.accelerationStructureCount = 1; wp.pAccelerationStructures = &ptlas_addr;
            w[0].descriptorType = VK_DESCRIPTOR_TYPE_PARTITIONED_ACCELERATION_STRUCTURE_NV; w[0].pNext = &wp;
        }
#endif
        for (uint32_t i = 0; i < 6; ++i)
        {
            bi[i] = {rb[i], 0, VK_WHOLE_SIZE};
            w[1 + i].dstBinding = 1 + i;
            w[1 + i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[1 + i].pBufferInfo = &bi[i];
        }
        uint32_t nw = 7;
        // Rig entries: grouped rigged hits map (custom, geometry) -> instance id.
        bi[7] = {B(mRigEntry), 0, VK_WHOLE_SIZE};
        w[nw].dstBinding = 9;
        w[nw].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[nw].pBufferInfo = &bi[7];
        ++nw;
        if (mDbgStats.buffer)
        {
            bi[6] = {mDbgStats.buffer, 0, VK_WHOLE_SIZE};
            w[nw].dstBinding = 8;
            w[nw].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[nw].pBufferInfo = &bi[6];
            ++nw;
        }
        const bool depth_ok = depthView && mR.mDepthSampler;
        if (depth_ok)
        {
            w[nw].dstBinding = 7;
            w[nw].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[nw].pImageInfo = &ii;
            ++nw;
        }
        // TODO(2b) #748: cluster hits read the page pool (11) and the tier-0 hit cut (12);
        // dummies while the cluster RT is inactive (no record references a cluster BLAS then).
        {
            const VkBuffer pool = mClusterRT && mClusterRT->poolBuffer() ? mClusterRT->poolBuffer() : mDummy.buffer;
            const VkBuffer hit = mClusterRT && mClusterRT->hitCutBuffer() ? mClusterRT->hitCutBuffer() : mDummy.buffer;
            bi[8] = {pool, 0, VK_WHOLE_SIZE};
            bi[9] = {hit, 0, VK_WHOLE_SIZE};
            for (uint32_t k = 0; k < 2; ++k)
            {
                w[nw].dstBinding = 11 + k;
                w[nw].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[nw].pBufferInfo = &bi[8 + k];
                ++nw;
            }
            // #747 / #748: rigged cluster hits read the RT skinned arena (13); dummy while no
            // record references a rigged cluster BLAS.
            const VkBuffer rig = mClusterRT && mClusterRT->rigBuffer() ? mClusterRT->rigBuffer() : mDummy.buffer;
            bi[10] = {rig, 0, VK_WHOLE_SIZE};
            w[nw].dstBinding = 13;
            w[nw].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[nw].pBufferInfo = &bi[10];
            ++nw;
            const VkBuffer skin = mClusterRT && mClusterRT->rigSkinBuffer() ? mClusterRT->rigSkinBuffer() : mDummy.buffer;
            bi[11] = {skin, 0, VK_WHOLE_SIZE};
            w[nw].dstBinding = 14;
            w[nw].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[nw].pBufferInfo = &bi[11];
            ++nw;
        }
        // #874 READ GUARD: 15 this slot's guard buffer (harvested + re-armed here: the slot's
        // fence was waited), 16 the per-geometry vertex counts (every consumer reads them).
        guardPrepare(s);
        bi[12] = {mGuard[s].buffer ? mGuard[s].buffer : mDummy.buffer, 0, VK_WHOLE_SIZE};
        w[nw].dstBinding = 15;
        w[nw].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[nw].pBufferInfo = &bi[12];
        ++nw;
        bi[13] = {B(mRtGeomVc), 0, VK_WHOLE_SIZE};
        w[nw].dstBinding = 16;
        w[nw].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[nw].pBufferInfo = &bi[13];
        ++nw;
        // #664: the terminator triangle target (only read when frame info lit_dbg.x bit 17
        // says it is bound and written this frame -- PARTIALLY_BOUND otherwise).
        VkDescriptorImageInfo ti{VK_NULL_HANDLE, mR.mTermTri.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        if (mR.mTermTri.view)
        {
            w[nw].dstBinding = 10;
            w[nw].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; w[nw].pImageInfo = &ti;
            ++nw;
        }
        vkUpdateDescriptorSets(dev, nw, w, 0, nullptr);
        mSetRTDepth[s] = depth_ok;
        mSetKeyRT[s] = rkey;
    }
}

void LLCleanRTVK::noteOriginShift(const float off[3])
{
    for (int k = 0; k < 3; ++k) mTlasBuildCam[k] += off[k];
    // #1007: the cadence's camera follows the re-base (no false teleport); the crossing itself
    // refits every rigged group once (a world-space group's BLAS holds absolute positions).
    for (int k = 0; k < 3; ++k) mRigCadCam[k] += off[k];
    mRigOriginShifted = true;
    for (auto& kv : mTerrMorph)
        for (int k = 0; k < 3; ++k) kv.second.cam[k] += off[k];
}

void LLCleanRTVK::noteInstanceRuns(const std::vector<std::pair<uint32_t, uint32_t>>& runs, bool full)
{
    // Opacity micromaps: changed rows are re-resolved on the next plan (not just by the
    // round-robin slice) -- a reused instance id must not keep another object's row.
    if (full || mOmmDirtyRuns.size() + runs.size() > 65536) { mOmmDirtyAll = true; mOmmDirtyRuns.clear(); }
    else if (!mOmmDirtyAll) mOmmDirtyRuns.insert(mOmmDirtyRuns.end(), runs.begin(), runs.end());
    if (full || mPendInstRuns.size() + runs.size() > 65536)   // idle for long: just rescan
    {
        mRigSetFull = true;
        mPendInstRuns.clear();
        return;
    }
    if (!mRigSetFull) mPendInstRuns.insert(mPendInstRuns.end(), runs.begin(), runs.end());
}

void LLCleanRTVK::refreshRigSet(uint32_t inst_n)
{
    const uint8_t* ib = mR.mCpuInstances.data();
    // Live (flags bit0) AND skinned. Unused rows (the packer's free list) keep stale
    // skin words, so the live bit is what keeps them out of the set.
    // C3: a rigged row's plan fields (the flag bits the plan tests, skin, LOD group),
    // hashed; 0 = not rigged. Pose updates rewrite rigged rows every frame without
    // touching these, so only a real plan-input change bumps mRigRowGen.
    auto rowSig = [&](uint32_t id) -> uint64_t {
        const uint8_t* in = ib + (size_t)id * kInstBytes;
        const uint32_t flags = rd32(in + kInstFlags), skin = rd32(in + kInstSkin);
        if (!(flags & 1u) || skin == kSentinel) return 0;
        uint64_t h = (uint64_t)(flags & 0x292Fu) + 0x9E3779B97F4A7C15ull;   // (+ 256: the group key's double-sided bit)
        h = (h ^ skin) * 0x100000001B3ull;
        h = (h ^ rd32(in + kInstGroup)) * 0x100000001B3ull;
        return h | 1ull;
    };
    // Terrain rows (flag 32; the smooth terrain plan re-checks flags + group per frame).
    auto terrBit = [&](uint32_t id) -> uint8_t {
        return (rd32(ib + (size_t)id * kInstBytes + kInstFlags) & 32u) ? 1 : 0;
    };
    auto check = [&](uint32_t id) {
        const uint64_t sg = rowSig(id);
        const uint8_t v = sg != 0 ? 1 : 0;
        if (v != mIsRigInst[id]) { mIsRigInst[id] = v; mRigIdsDirty = true; }
        if (sg != mRigRowSig[id]) { mRigRowSig[id] = sg; ++mRigRowGen; }
        const uint8_t t = terrBit(id);
        if (t != mIsTerrInst[id]) { mIsTerrInst[id] = t; mTerrIdsDirty = true; }
    };
    const uint32_t old_n = (uint32_t)mIsRigInst.size();
    if (mIsTerrInst.size() != old_n) { mIsTerrInst.assign(old_n, 0); mRigSetFull = true; }
    if (old_n != inst_n)
    {
        mIsRigInst.resize(inst_n, 0);
        mIsTerrInst.resize(inst_n, 0);
        mRigRowSig.resize(inst_n, 0);
        mRigIdsDirty = true;
        mTerrIdsDirty = true;
        for (uint32_t id = old_n; id < inst_n; ++id) check(id);   // grown tail
    }
    if (mRigRowSig.size() != inst_n) mRigRowSig.resize(inst_n, 0);
    if (mRigSetFull)
    {
        for (uint32_t id = 0; id < inst_n; ++id)
        {
            mRigRowSig[id] = rowSig(id);
            mIsRigInst[id] = mRigRowSig[id] != 0 ? 1 : 0;
            mIsTerrInst[id] = terrBit(id);
        }
        mRigSetFull = false;
        mRigIdsDirty = true;
        mTerrIdsDirty = true;
    }
    else
    {
        for (const auto& r : mPendInstRuns)
            for (uint32_t id = r.first; id < r.second && id < inst_n; ++id) check(id);
        // Self-heal: re-check a rolling slice, in case a row ever changes outside the
        // renderer's dirty runs (C8, RenderVKSelfHealRate: 2 = 2048 rows/frame, 1 =
        // max(256, n/256), 0 = none).
        static LLCachedControl<U32> s_heal(gSavedSettings, "RenderVKSelfHealRate", 1);
        const uint32_t kHealSlice = s_heal >= 2 ? 2048u : (s_heal == 1 ? std::max<uint32_t>(256u, inst_n / 256u) : 0u);
        for (uint32_t k = 0; k < kHealSlice && inst_n; ++k)
        {
            if (mRigHealCursor >= inst_n) mRigHealCursor = 0;
            check(mRigHealCursor++);
        }
    }
    mPendInstRuns.clear();
    LLVKHitch::count("rt_rigSetIds", (uint32_t)mRigIds.size());
    if (mRigIdsDirty)
    {
        ++mRigRowGen;   // C3: the rigged set itself changed
        mRigIds.clear();
        for (uint32_t id = 0; id < inst_n; ++id)
            if (mIsRigInst[id]) mRigIds.push_back(id);
        mRigIdsDirty = false;
    }
    if (mTerrIdsDirty)
    {
        mTerrIds.clear();
        for (uint32_t id = 0; id < inst_n; ++id)
            if (mIsTerrInst[id]) mTerrIds.push_back(id);
        mTerrIdsDirty = false;
    }
}

// NO-SELF-SHADOW CLASSIFICATION (rigged RT update throttle, 2026-10-14). The refit gate's rule
// 2 (RenderVKRigRefitGate 2: legacy rigged BLAS refit and the cluster path's rigged CLAS
// rebuild, recordBuild) lets a far or small on-screen avatar's RT geometry lag its raster by up
// to period - 1 frames; on those frames the shadow rays leaving its CURRENT surface met its own
// STALE copy (self-shadow patches jumping every few frames: a swimming fish school). An avatar
// the gate MAY throttle is classified here, once per frame, and only a classified one is ever
// throttled; it never self-shadows in RT on ANY frame (clean_rt_common.glsl rtSelfCandidate:
// its records carry kAvVisRtNoSelf -> its owner tag), so its update frames do not blink a
// self-shadow back in. It still casts onto everything else and receives every other caster.
// Its self-shadow detail is small by construction (under RenderVKRigRefitFullPx tall or past
// RenderVKRigRefitFarMeters). HYSTERESIS (the denoiser's history must not flip with it): enter
// when the gate's rule would give period > 1 (farther than FarMeters, or a known on-screen
// height under FullPx); leave only when nearer than 0.85 x FarMeters AND at least 1.25 x FullPx
// tall (or off screen); either change at most every kNoSelfDwell frames. Never our own avatar
// or the protected set (#561) -- those leave at once, and are never throttled anyway.
void LLCleanRTVK::classifyNoSelf()
{
    constexpr uint32_t kNoSelfDwell = 30;
    constexpr float kNoSelfFarLeave = 0.85f, kNoSelfPxLeave = 1.25f;
    LLCleanRendererVK& R = mR;
    mNoSelfCount = 0;
    uint32_t rig_gate = 0, max_period = 1;
    float far_m = 0.f, full_px = 0.f;
    bool cadence = true;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<U32> s_rig_gate(gSavedSettings, "RenderVKRigRefitGate", 2);
    static LLCachedControl<F32> s_rig_far(gSavedSettings, "RenderVKRigRefitFarMeters", 24.f);
    static LLCachedControl<F32> s_rig_full_px(gSavedSettings, "RenderVKRigRefitFullPx", 200.f);
    static LLCachedControl<U32> s_rig_max_period(gSavedSettings, "RenderVKRigRefitMaxPeriod", 4);
    static LLCachedControl<bool> s_rig_cadence(gSavedSettings, "RenderVKRigRefitCadence", true);
    rig_gate = s_rig_gate; far_m = s_rig_far; full_px = s_rig_full_px;
    max_period = std::clamp<U32>(s_rig_max_period, 1u, kRigForceRefit);
    cadence = s_rig_cadence;
#endif
    // (#1007 RenderVKRigRefitCadence off: nobody is throttled, so nobody loses self-shadowing.
    // The very-far band -- RenderVKRigRefitVeryFarMeters, period 4 -- lies past FarMeters,
    // so the FarMeters rule below already tags every avatar it throttles.)
    const bool on = rig_gate >= 2 && cadence && R.mAvVisSSBO.buffer;
    const uint32_t frame = (uint32_t)gFrameCount;
    for (uint32_t a = 0; a < kAvVisSlots; ++a)
    {
        const bool eligible = on && !R.mRigSelfSlot[a] && !R.mRigProtect[a];
        bool want = false, keep = false;
        if (eligible)
        {
            float d = 0.f;
            std::memcpy(&d, &R.mAvVis[LLCleanRendererVK::kAvDemBase + 2 * a], 4);
            const float px = R.mRigAvPx[a];
            const bool d_ok = far_m > 0.f && std::isfinite(d);
            const bool px_ok = full_px > 0.f && max_period > 1u && px > 0.f && std::isfinite(px);
            want = (d_ok && d > far_m) || (px_ok && px < full_px);
            keep = (d_ok && d > far_m * kNoSelfFarLeave) || (px_ok && px < full_px * kNoSelfPxLeave);
        }
        uint8_t& st = mNoSelf[a];
        if (!eligible)   // (self / protected / gate off: never throttled, leave at once)
        {
            if (st) { st = 0; mNoSelfChanged[a] = frame; }
        }
        else if ((st ? keep : want) != (st != 0) && frame - mNoSelfChanged[a] >= kNoSelfDwell)
        {
            st = st ? 0 : 1;
            mNoSelfChanged[a] = frame;
        }
        // (a zero word is an avatar no pass draws: not in the TLAS, nothing to tag)
        if (st && R.mAvVis[a] != 0u)
        {
            R.mAvVis[a] |= LLCleanRendererVK::kAvVisRtNoSelf;
            ++mNoSelfCount;
        }
    }
}

void LLCleanRTVK::recordBuild(VkCommandBuffer cmd, const VKCullPush& cull, const VKDrawPush& draw, bool do_rigged)
{
    // #889 sync A/B: full barriers around every AS build of this frame (fullBarrier).
    bool full_barriers = false;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<bool> s_full(gSavedSettings, "RenderVKRTFullBarriers", false);
    full_barriers = s_full;
#endif
    auto fullB = [&](VkCommandBuffer c) { if (full_barriers) fullBarrier(c); };
    VkMemScope memcat(VKMEM_RT_AUX);   // #407 (TLAS / BLAS storage is RT_AS regardless)
    // #697: every allocation of the build is SOFT. The fallbacks below were written for
    // failure returns (arenaAlloc false -> ++mStarved / the build skipped, ensureBuf false
    // -> abortFrame, no TLAS buffer -> abortFrame: the consumers fall back to shadow maps
    // this frame) but the scope was never opened, so an out-of-memory aborted inside
    // createBuffer first (64 MB arena chunks, dedicated BLAS, scratch, TLAS). Soft requests
    // >= 4 MB are also refused up front past the OS budget (heapBudgetPreflight).
    VkDeviceContext::SoftAllocScope rt_soft(mCtx);
    VK_HITCH_SCOPE("vk_rtBuild");
    mTlasBuilt = false;
    mReleaseOwed = 0;   // set only once this frame's TLAS build is recorded (below)
    mIdleSince = 0.0;
    mReleased = false;
    mReleaseAsap = false;   // a consumer is back: no early release
    if (!mCtx || !mCompPipe) return;
    LLCleanRendererVK& R = mR;
    const uint32_t inst_n = std::min<uint32_t>(R.mInstanceCount, (uint32_t)(R.mCpuInstances.size() / kInstBytes));
    if (inst_n == 0 || !R.mVbo.address || !R.mIbo.address || !R.mInstanceSSBO.buffer || !R.mSlotSSBO.buffer)
        return;
    VkDevice dev = mCtx->device();
    const auto& rt = mCtx->rt();

    auditBeforeFrame();   // #874: RenderVKRTAuditNow (this slot's fence was waited; before its frees)
    beginFrame();
    mAuditMeta[mSlotIdx].valid = false;   // #874: this slot's readback is rewritten (or not) this frame
    harvestClusterCoverage();   // (legacy-skin skip: this slot's last pass B, before plan() reuses its map)
    checkClusterConsistency();  // (cluster RT data ready vs cluster records in the TLAS)
    // Phase timers (section log, vk_rtP_*): which part of the CPU plan costs what.
    auto phase_t = std::chrono::steady_clock::now();
    auto phase = [&phase_t](const char* name) {
        const auto now = std::chrono::steady_clock::now();
        LLVKHitch::add(name, std::chrono::duration<double, std::milli>(now - phase_t).count());
        phase_t = now;
    };

    // ---- settings ----
    uint32_t builds_max = 64, tris_k = 300;
    float radius = 256.f;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<U32> s_builds(gSavedSettings, "RenderVKRTBlasBuildsPerFrame", 64);
    static LLCachedControl<U32> s_tris(gSavedSettings, "RenderVKRTBlasTrisPerFrame", 300);
    static LLCachedControl<F32> s_radius(gSavedSettings, "RenderVKRTRadius", 256.f);
    builds_max = s_builds; tris_k = s_tris; radius = s_radius;
#endif
    radius = mR.rtRadiusEffective(radius);   // VRAM ladder: far shadows / RT range (#405)
    // F1 / B8 toggles (A/B: RenderVKBench).
    bool tlas_refit = true;
    uint32_t tlas_rebuild_n = 8, rig_gate = 2;
    bool rig_force_rebuild = false, no_compact = false;   // #973 A/Bs
    float rig_far = 24.f, rig_full_px = 0.f;
    uint32_t rig_max_period = 4;
    // #1007: cadence on/off (A/B), the very-far band (period 4), the pose skip's threshold
    // (m at <= kPoseRefMeters; 0 = off) and its staleness cap (frames).
    bool rig_cadence = true;
    float rig_very_far = 60.f, pose_thr = 0.f;
    uint32_t pose_max_stale = 0;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<bool> s_tlas_refit(gSavedSettings, "RenderVKTLASRefit", true);
    static LLCachedControl<U32> s_tlas_n(gSavedSettings, "RenderVKTLASRebuildFrames", 8);
    static LLCachedControl<U32> s_rig_gate(gSavedSettings, "RenderVKRigRefitGate", 2);
    static LLCachedControl<F32> s_rig_far(gSavedSettings, "RenderVKRigRefitFarMeters", 24.f);
    static LLCachedControl<F32> s_rig_full_px(gSavedSettings, "RenderVKRigRefitFullPx", 200.f);
    static LLCachedControl<U32> s_rig_max_period(gSavedSettings, "RenderVKRigRefitMaxPeriod", 4);
    tlas_refit = s_tlas_refit; tlas_rebuild_n = std::max<U32>(s_tlas_n, 1u);
    rig_gate = s_rig_gate; rig_far = s_rig_far;
    // #973 debug A/Bs (llcleanrtvk.h / settings.xml): rigged BLAS rebuilt instead of refit; no
    // static BLAS compaction; a guard band after every arena allocation made while it is on.
    static LLCachedControl<bool> s_rig_force_rebuild(gSavedSettings, "RenderVKRTRigForceRebuild", false);
    static LLCachedControl<bool> s_no_compact(gSavedSettings, "RenderVKRTNoCompaction", false);
    static LLCachedControl<U32> s_arena_guard(gSavedSettings, "RenderVKRTArenaGuard", 0);
    rig_force_rebuild = s_rig_force_rebuild;
    no_compact = s_no_compact;
    mArenaGuardBytes = alignUp((VkDeviceSize)std::min<U32>(s_arena_guard, 4096u) * 1024, kAsAlign);
    rig_full_px = s_rig_full_px;
    rig_max_period = std::clamp<U32>(s_rig_max_period, 1u, kRigForceRefit);
    static LLCachedControl<bool> s_rig_cadence(gSavedSettings, "RenderVKRigRefitCadence", true);
    static LLCachedControl<F32> s_rig_very_far(gSavedSettings, "RenderVKRigRefitVeryFarMeters", 60.f);
    static LLCachedControl<F32> s_pose_thr(gSavedSettings, "RenderVKRigRefitPoseThreshold", 0.002f);
    static LLCachedControl<U32> s_pose_stale(gSavedSettings, "RenderVKRigRefitPoseMaxStale", 4);
    rig_cadence = s_rig_cadence;
    rig_very_far = s_rig_very_far;
    pose_thr = std::isfinite((float)s_pose_thr) ? std::clamp<float>(s_pose_thr, 0.f, 0.05f) : 0.f;
    pose_max_stale = std::min<U32>(s_pose_stale, kPoseMaxStale);
    // The refit gate's proofs describe THIS frame vs the previous one: RT must have run
    // on the previous viewer frame too (no gap where a pose change went unseen).
    const uint32_t gframe = (uint32_t)gFrameCount;
    const bool gframe_seq = mLastBuildGFrame + 1u == gframe;
    mLastBuildGFrame = gframe;
    const bool anim_now = R.mAnimBuiltFrame == gframe;
    LLCleanAnimStore& anim = LLCleanAnimStore::instance();
    // #632: joint-table rows the rig copy / pass B can read (avatar-local groups below it)
    const uint32_t jt_slots = (uint32_t)std::min<VkDeviceSize>(R.jointTable().size / (216u * 48u), kAvVisSlots);
#else
    const bool gframe_seq = false, anim_now = false;
    rig_gate = 0;
#endif
    // TODO(2b) #748: a partitioned TLAS is rewritten per changed record, never refit (pass A
    // then writes plain records: inactive = reference 0, no placeholder BLAS).
    if (mClusterRT && mClusterRT->ptlasLayout()) tlas_refit = false;
    mRigRefitSkipped = 0; mRigCopyEntries = 0; mRigRefitSkippedMoving = 0;
    mRigSkipCadence = mRigSkipPose = mRigMaxStale = 0;
    mRigStarvedBuilt = 0; mRigRefitDeferred = 0;
    builds_max = std::min<uint32_t>(std::max<uint32_t>(builds_max, 1u), kMaxBuildsPerFrame);
    // BUILD SCRATCH BUDGET (#161): scratch was sized by the biggest frame ever (e.g. every
    // rigged group rebuilt at once after idle) and never shrank. Builds past this budget
    // wait a frame; the first build of a frame always goes (no starvation).
    constexpr VkDeviceSize kScratchBudget = 96ull << 20;
    mScratchUsed = 0;
    // ONE budget for all RT memory; BLAS get what the TLAS + working buffers leave.
    updateHeadroomBudget();
    const uint64_t total_budget = budgetBytes();
    // Log the effective budget and why on every change of more than ~3% (the VRAM budget the
    // renderer measures moves a little all the time).
    if (mBudgetLogged == 0 || total_budget > mBudgetLogged + mBudgetLogged / 32 || total_budget + mBudgetLogged / 32 < mBudgetLogged)
    {
        LL_INFOS("VKRT") << "RT: memory budget " << (total_budget >> 20) << " MB (" << budgetWhy() << ")" << LL_ENDL;
        mBudgetLogged = total_budget;
    }
    const uint64_t aux_now = rtAuxBytes();
    const uint64_t budget = total_budget > aux_now ? total_budget - aux_now : 0;
    // The cluster RT module's memory is its own share of the budget (LLCleanClusterRTVK caps all
    // its arenas at a third of it): the avatar-drop and over-budget rules below judge the legacy
    // classes against what that share leaves (#748 perf pass: its CLAS arenas used to count as
    // "working buffers" here and pushed every avatar and worn attachment out of RT).
    const uint64_t cl_bytes = mClusterRT ? mClusterRT->asBytes() + mClusterRT->auxBytes() : 0;
    const uint64_t legacy_aux = aux_now > cl_bytes ? aux_now - cl_bytes : 0;
    const uint64_t legacy_budget = total_budget > cl_bytes ? total_budget - cl_bytes : 0;
    // Degrade ladder. #1015: the legacy rigged class (avatars leave the TLAS; static geometry keeps
    // shadowing) is dropped ONLY by the VRAM pressure ladder (dropRiggedForPressure, a rung that engages
    // under real video memory pressure and is lifted by clearRiggedDrop when the ladder calms) -- it used
    // to also drop when RT crossed 3/4 of a fixed share of VRAM, with VRAM to spare. Working buffers
    // beyond 70% of the budget -> overBudget (renderer: shadow maps).
    // CLUSTER LOD REPLACES LEGACY IN RT (2026-10-13, "never fall back"): avatars the cluster path
    // draws (switched groups, key bit 0) trace ONLY their cluster cut (llcleanclusterrtvk.h RIGGED,
    // its own share of the cluster RT memory) whenever the cluster RT traces switched avatars
    // (clusterTracesSwitched): no legacy fallback BLAS, rig copy or legacy skinning for them --
    // a frame whose cluster data is not ready traces nothing for that group. mRig / mRigBytes
    // therefore hold only avatars NOT on the cluster path, and this drop judges exactly those.
    // Dropped, the plan still runs over the switched groups (cluster-only); their worn
    // attachments stay too (group map words 8..15). Cluster LOD off: no switched group, the old rule.
    if (!mRigDropped && mRigPressureDrop)
    {
        mRigDropped = true;
        mRigRoomFrames = 0;
        const uint64_t rig_was = mRigBytes;
        mRigDropBytes = rig_was;
        mRigDropRows = (uint32_t)std::max<size_t>(mRigIds.size(), 1);
        for (auto& kv : mRig) retireAS(kv.second.as, kv.second.mem);
        mRig.clear();
        mRigBytes = 0;
        mRigWait.clear();   // (#692: the whole class is out; logged below)
        const char* cl = clusterTracesSwitched() ? "; avatars on the cluster path keep tracing their cluster cut" : "";
        LL_WARNS("VKRT") << "RT: VRAM pressure -- legacy avatars (rigged meshes + worn attachments) dropped from RT ("
                         << (rig_was >> 20) << " MB; video memory full)" << cl << LL_ENDL;
    }
    // The working buffers leave < 30% of the budget for BLAS: RT would trace a scene
    // mostly evicted (a few % of casters) -- shadow maps look better than that.
    if (!mOverBudget && legacy_aux > legacy_budget / 10 * 7)
    {
        mOverBudget = true;
        LL_WARNS("VKRT") << "RT: working buffers (" << (aux_now >> 20) << " MB) leave too little of the RT budget ("
                         << (total_budget >> 20) << " MB) for geometry -- projector shadows fall back to shadow maps"
                         << LL_ENDL;
    }
    // (the legacy rigged class off: the plan below keeps only the switched groups, cluster-only)
    const bool rig_cl_forced = mRigDropped || clusterTracesSwitched();
    // Hard cap: a budget that DROPPED (pressure cap, device budget change) shrinks what
    // is already resident -- LRU unused first, then in-use statics.
    if (mStaticBytes + mRigBytes > budget)
    {
        evictForBudget(0, budget);
        if (mStaticBytes + mRigBytes > budget)
        {
            // In-use BLAS go too: the TLAS loses casters it was tracing. A big cut (over a
            // quarter of them: pressure cap, budget collapse) -> re-warm, the renderer keeps
            // the cascades until the builds catch up; the ~10% trims of a budget that
            // fluctuates with the measured heap don't.
            const uint32_t before = mStaticCount;
            evictForBudget(0, budget, true);
            if (mStaticCount < before - before / 4) markCold("budget cut evicted in-use BLAS");
        }
    }
    const uint64_t tris_max = (uint64_t)std::max<uint32_t>(tris_k, 1u) * 1000ull;

    // ---- slot tables follow the packer ----
    const SlotInfoCPU* slots = reinterpret_cast<const SlotInfoCPU*>(R.mCpuSlots.data());
    const uint32_t slot_n = (uint32_t)(R.mCpuSlots.size() / kSlotBytes);
    if (mSlot.size() > slot_n)
        for (uint32_t i = slot_n; i < mSlot.size(); ++i) dropSlotBlas(i);   // table shrank (repack)
    for (uint32_t i = slot_n; i < mTerrOwned.size(); ++i) dropTerrMorph(i);
    mSlot.resize(slot_n);
    mTerrOwned.resize(slot_n, 0);
    if (mSlotGen.size() < slot_n) mSlotGen.resize(slot_n, 0);
    if (mSlotAddrCpu.size() != (size_t)slot_n * 2)
    {
        mSlotAddrCpu.resize((size_t)slot_n * 2, 0);
        mAddrDirtyLo = 0; mAddrDirtyHi = slot_n;
    }
    const uint32_t vbo_verts = R.mGeomVertEnd;   // quantized (#573); the arena's high-water (#584)
    auto slotOk = [&](uint32_t sl) -> bool {
        if (sl >= slot_n) return false;
        const SlotInfoCPU& s = slots[sl];
        return s.index_count >= 3 && s.vert_count > 0 && s.base_vertex >= (int32_t)LLCleanQV::kHeaderVerts
            && (uint64_t)s.first_index + s.index_count <= R.mIdxValid
            && (uint64_t)s.first_index + s.index_count <= R.mGeomIdxEnd
            && (uint64_t)s.base_vertex + s.vert_count <= vbo_verts
            && slotIndicesOk(sl, s.first_index, s.index_count, s.vert_count);
    };

    phase("vk_rtP_setup");
    // ---- feedback (frame N-2) -> lastUsed ----
    readFeedback();
    harvestPoseMotion();   // #1007 (the pose skip's readings, this slot's last mode 5)
    arenaGuardCompare(mSlotIdx, false);   // #973 (this slot's fence was waited)

    // ---- compaction results (built 2 frames ago on this slot index) ----
    struct CompactOp { uint32_t slot; VkAccelerationStructureKHR src, dst; };
    std::vector<CompactOp> compactOps;
    if (no_compact) mCompact[mSlotIdx].clear();   // #973 RenderVKRTNoCompaction: no compaction copies
    if (!mCompact[mSlotIdx].empty())
    {
        std::vector<CompactPending>& pend = mCompact[mSlotIdx];
        std::vector<uint64_t> sizes(pend.size(), 0);
        const VkResult qr = vkGetQueryPoolResults(dev, mQueryPool, mSlotIdx * kMaxBuildsPerFrame,
                                                  (uint32_t)pend.size(), sizes.size() * sizeof(uint64_t),
                                                  sizes.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
        if (qr == VK_SUCCESS)
        {
            for (size_t i = 0; i < pend.size(); ++i)
            {
                const CompactPending& p = pend[i];
                if (p.slot >= mSlot.size()) continue;
                SlotBlas& s = mSlot[p.slot];
                if (!s.as || s.serial != p.serial || s.compacted) continue;
                const VkDeviceSize csz = alignUp(sizes[i], kAsAlign);
                if (csz == 0 || csz * 10 > s.mem.size * 9) { s.compacted = true; continue; }
                Alloc a;
                if (!arenaAlloc(csz, a)) continue;
                VkDeviceAddress addr = 0;
                VkAccelerationStructureKHR nas = createAS(a, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, addr, "blas-compact", p.slot);
                if (!nas) { arenaFree(a); continue; }
                compactOps.push_back({p.slot, s.as, nas});
                retireAS(s.as, s.mem);
                mStaticBytes = mStaticBytes - s.mem.size + a.size;
                s.as = nas; s.mem = a; s.addr = addr; s.compacted = true; ++s.serial;
                setSlotAddr(p.slot, addr);
            }
        }
        pend.clear();
    }
    mCompactions = (uint32_t)compactOps.size();

    // Rigged + terrain instance sets (the dirty runs since the last frame).
    phase("vk_rtP_feedback");
    refreshRigSet(inst_n);
    phase("vk_rtP_rigSet");

    // ---- SMOOTH TERRAIN (#211): each terrain patch's drawn slot (see the notes at the
    //      top: morph BLAS for slots the VS geomorphs, same-frame static builds for the
    //      rest). RenderVKTerrainSmooth off: nothing -- every morph BLAS goes. ----
    bool terr_on = false;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<bool> s_tsmooth(gSavedSettings, "RenderVKTerrainSmooth", true);
    terr_on = s_tsmooth;
#endif
    mTerrPriority.clear();
    if (terr_on && !mTerrIds.empty())
    {
        const float* cam = cull.lod_cam_pos;
        const uint8_t* ib = R.mCpuInstances.data();
        const uint8_t* groups = R.mCpuGroups.data();
        const uint32_t grp_n = (uint32_t)(R.mCpuGroups.size() / 16);
        if (mSlotStep.size() < (size_t)slot_n * 3) mSlotStep.resize((size_t)slot_n * 3, 0.f);
        if (mSlotStepGen.size() < slot_n) mSlotStepGen.resize(slot_n, 0);
        // Max |step| per band class of a slot's vertices, classified as terrainMorph does
        // (fract(p * 4) != 0: 0.125 m; else fract(p * 2) != 0: 0.25 m; else 0.5 m).
        auto slotSteps = [&](uint32_t sl) -> const float* {
            float* st = &mSlotStep[(size_t)sl * 3];
            const uint32_t gen = mSlotGen[sl] + 1u;
            if (mSlotStepGen[sl] != gen)
            {
                st[0] = st[1] = st[2] = 0.f;
                const SlotInfoCPU& si = slots[sl];
                // #584: the slot's header-prefixed range from the mesh store mirror
                const uint8_t* sv = nullptr; const uint16_t* six = nullptr; const uint8_t* sw = nullptr;
                if (!R.geomSlotSource(sl, sv, six, sw)) return st;   // (stale row: retried once re-placed)
                LLCleanQV::QHeader qh;
                const bool qok = LLCleanQV::headerAt(sv, (size_t)si.vert_count + LLCleanQV::kHeaderVerts,
                                                     LLCleanQV::kHeaderVerts, qh);
                for (uint32_t v = 0; qok && v < si.vert_count; ++v)
                {
                    // pos xyz, uv: the exact lattice decode clean_draw.vert does (#573)
                    LLCleanQV::QVertex q;
                    std::memcpy(&q, sv + ((size_t)LLCleanQV::kHeaderVerts + v) * LLCleanQV::kStride, sizeof(q));
                    float f[5];
                    LLCleanQV::decodePos(qh, q, f);
                    LLCleanQV::decodeUV(qh, q, f + 3);
                    if (f[4] == 0.f) continue;
                    const float x4 = f[0] * 4.f, y4 = f[1] * 4.f, x2 = f[0] * 2.f, y2 = f[1] * 2.f;
                    const int c = (x4 != std::floor(x4) || y4 != std::floor(y4)) ? 0
                                : (x2 != std::floor(x2) || y2 != std::floor(y2)) ? 1 : 2;
                    st[c] = std::max(st[c], std::fabs(f[4]));
                }
                mSlotStepGen[sl] = gen;
            }
            return st;
        };
        for (const uint32_t id : mTerrIds)
        {
            if (id >= inst_n) continue;
            const uint8_t* in = ib + (size_t)id * kInstBytes;
            const uint32_t flags = rd32(in + kInstFlags), gid = rd32(in + kInstGroup);
            // Pass A's terrain rows: not HUD / rigged / water-excluded, a real group.
            if (!(flags & 32u) || (flags & (1u | 8u | 65536u)) || gid == kSentinel || gid >= grp_n) continue;
            float M[16], lo[3], hi[3];
            std::memcpy(M, in, sizeof(M));
            std::memcpy(lo, in + 64, sizeof(lo));
            std::memcpy(hi, in + 80, sizeof(hi));
            // #568: the CPU mirror row is in the store's render-world frame (agent +
            // R.mInstD; terrain rows are agent-space lanes, LLCleanInstanceStore::
            // rowAgentSpace); the LOD camera -- and the GPU rows the cull / VS / mode-3
            // copy read -- are AGENT space. Without this, after any region crossing
            // (d = a non-zero multiple of 256 m until the re-anchor) every distance below
            // was off by |d|: the patches the VS geomorphs near the camera were rejected
            // (no morph BLAS: the RT kept the UNMORPHED level, up to the full step off
            // the drawn surface -- grazing-sun self-shadow streaks on terrain, per LOD
            // band) and no drawn slot got its same-frame priority build (coarser stand-in).
            for (int r = 0; r < 3; ++r) M[12 + r] -= R.mInstD[r];
            // O(1) reject first (most of a 5x5-region draw distance): a patch past
            // kTerrPriorityDist draws level 0 -- no morph, no priority build. Terrain
            // instances are translations (region origin), so the box lies within the
            // radius of its local corners about the translation.
            {
                float r2 = 0.f, t2 = 0.f;
                for (int r = 0; r < 3; ++r)
                {
                    r2 += std::max(lo[r] * lo[r], hi[r] * hi[r]);
                    const float d = M[12 + r] - cam[r];
                    t2 += d * d;
                }
                const float sc2 = std::max({M[0] * M[0] + M[1] * M[1] + M[2] * M[2],
                                            M[4] * M[4] + M[5] * M[5] + M[6] * M[6],
                                            M[8] * M[8] + M[9] * M[9] + M[10] * M[10]});
                const float reach = std::sqrt(r2 * std::max(sc2, 1.f)) + kTerrPriorityDist;
                if (std::isfinite(t2) && t2 > reach * reach) continue;
            }
            // World box of the instance box (clean_terrain_lod.glsl terrainBoxDist): near
            // distance picks the level, far distance bounds every vertex's distance.
            float wmin[3] = {1e30f, 1e30f, 1e30f}, wmax[3] = {-1e30f, -1e30f, -1e30f};
            for (int i = 0; i < 8; ++i)
            {
                const float c[3] = {(i & 1) ? hi[0] : lo[0], (i & 2) ? hi[1] : lo[1], (i & 4) ? hi[2] : lo[2]};
                for (int r = 0; r < 3; ++r)
                {
                    const float w = M[r] * c[0] + M[4 + r] * c[1] + M[8 + r] * c[2] + M[12 + r];
                    wmin[r] = std::min(wmin[r], w);
                    wmax[r] = std::max(wmax[r], w);
                }
            }
            float dn2 = 0.f, df2 = 0.f;
            for (int r = 0; r < 3; ++r)
            {
                const float e = std::max(std::max(wmin[r] - cam[r], cam[r] - wmax[r]), 0.f);
                const float f = std::max(std::fabs(cam[r] - wmin[r]), std::fabs(cam[r] - wmax[r]));
                dn2 += e * e;
                df2 += f * f;
            }
            if (!std::isfinite(dn2) || !std::isfinite(df2)) continue;
            const float dnear = std::sqrt(dn2), dfar = std::sqrt(df2);
            if (dnear >= kTerrPriorityDist) continue;   // level 0: nothing to do (see above)
            // The cull's slot (wanted level, then coarser, then finer: terrainLevelOrder);
            // within 0.5 m of a switch distance both candidates (GPU vs CPU rounding).
            uint32_t picked[2] = {kSentinel, kSentinel};
            for (int k = 0; k < 2; ++k)
            {
                const uint32_t want = terrLevelCpu(std::max(dnear + (k ? 0.5f : -0.5f), 0.f));
                for (uint32_t o = 0; o < 4u; ++o)
                {
                    const uint32_t L = o <= want ? want - o : o;
                    const uint32_t v = rd32(groups + (size_t)gid * 16 + (size_t)L * 4);
                    if (v != kSentinel && v < slot_n && slots[v].index_count != 0) { picked[k] = v; break; }
                }
            }
            if (picked[1] == picked[0]) picked[1] = kSentinel;
            for (const uint32_t sl : picked)
            {
                if (sl == kSentinel || !slotOk(sl)) continue;
                const float* st = slotSteps(sl);
                const float hyst = mTerrOwned[sl] ? kTerrHyst : 0.f;
                bool morph = false;
                for (int c = 0; c < 3; ++c)
                    if (st[c] > kTerrStepEps && dfar > kTerrBandLo[c] - hyst) morph = true;
                if (morph)
                {
                    TerrMorph& tm = mTerrMorph[sl];
                    mTerrOwned[sl] = 1;
                    tm.inst = id;
                    tm.seen = mFrame;
                }
                else if (!mSlot[sl].as)
                {
                    mSlot[sl].lastUsed = mFrame;
                    mTerrPriority.push_back(sl);
                    wantSlot(sl);
                }
            }
        }
    }
    // Leaving the morph set (no longer drawn, no longer morphing, smooth off): the slot
    // falls back to its static BLAS (the priority list above builds it now if drawn near).
    if (!mTerrMorph.empty())
    {
        static std::vector<uint32_t> s_tdrop;
        s_tdrop.clear();
        for (const auto& kv : mTerrMorph)
            if (!terr_on || kv.second.seen != mFrame) s_tdrop.push_back(kv.first);
        for (const uint32_t sl : s_tdrop) dropTerrMorph(sl);
    }

    phase("vk_rtP_terrain");
    // ---- static BLAS builds (lazy: slots the TLAS pass selected, not yet built; drawn
    //      terrain slots first) ----
    // xf: the slot's decode transform (#573): the BLAS reads the quantized positions as
    // R16G16B16A16_SNORM and VkTransformMatrixKHR maps them back (written at record time).
    struct StaticBuild { uint32_t slot; VkAccelerationStructureGeometryKHR geom; VkAccelerationStructureBuildRangeInfoKHR range;
                         VkDeviceSize scratch; float xf[12]; };
    // C11: per-frame plan vectors keep their capacity across frames (main thread only).
    static std::vector<StaticBuild> s_sbuilds;
    std::vector<StaticBuild>& sbuilds = s_sbuilds;
    sbuilds.clear();
    bool build_saturated = false;   // a build limit stopped the static builds this frame (warm-up)
    {
        uint64_t tris = 0;
        // 1 = built, 0 = skipped, -1 = stop for this frame (budget / scratch / allocation).
        auto tryStatic = [&](uint32_t sl) -> int
        {
            SlotBlas& s = mSlot[sl];
            if (s.as || s.lastUsed == 0 || s.lastUsed + 2 < mFrame || !slotOk(sl)) return 0;
            if (mTerrOwned[sl]) return 0;   // smooth terrain: its morph BLAS stands in for it
            const SlotInfoCPU& si = slots[sl];
            StaticBuild b{};
            b.slot = sl;
            b.geom = VkAccelerationStructureGeometryKHR{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
            b.geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
            auto& t = b.geom.geometry.triangles;
            t = VkAccelerationStructureGeometryTrianglesDataKHR{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
            // QUANTIZED source (#573): int16 lattice positions read as SNORM (q / 32767; a
            // mandatory AS vertex format, the 4th component -- the flags -- is ignored) and
            // mapped back to object space by the slot's decode transform (transformData,
            // set at record time). 16-bit face-local indices.
            {
                // #584: the decode header from the mesh store mirror (= the packed one)
                const uint8_t* sv = nullptr; const uint16_t* six = nullptr; const uint8_t* sw = nullptr;
                LLCleanQV::QHeader qh;
                if (!R.geomSlotSource(sl, sv, six, sw)
                    || !LLCleanQV::headerAt(sv, (size_t)si.vert_count + LLCleanQV::kHeaderVerts, LLCleanQV::kHeaderVerts, qh))
                    return 0;
                LLCleanQV::snormTransform(qh, b.xf);
            }
            t.vertexFormat = VK_FORMAT_R16G16B16A16_SNORM;
            t.vertexData.deviceAddress = R.mVbo.address + (VkDeviceAddress)si.base_vertex * LLCleanQV::kStride;
            t.vertexStride = LLCleanQV::kStride;
            t.maxVertex = si.vert_count - 1;
            t.indexType = VK_INDEX_TYPE_UINT16;
            t.indexData.deviceAddress = R.mIbo.address + (VkDeviceAddress)si.first_index * 2;
            // The sizes query looks at whether transformData is null: a non-null placeholder
            // here, the real per-frame address at record time (same null-ness for both).
            t.transformData.deviceAddress = 16;
            b.range.primitiveCount = si.index_count / 3;
            VkAccelerationStructureBuildGeometryInfoKHR gi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
            gi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            gi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
            gi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            gi.geometryCount = 1; gi.pGeometries = &b.geom;
            VkAccelerationStructureBuildSizesInfoKHR sz{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
            rt.vkGetAccelerationStructureBuildSizesKHR(dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &gi,
                                                       &b.range.primitiveCount, &sz);
            if (sz.accelerationStructureSize == 0) return 0;
            if (!sbuilds.empty() && mScratchUsed + sz.buildScratchSize > kScratchBudget) return -1;   // next frame
            evictForBudget(sz.accelerationStructureSize, budget);
            if (mStaticBytes + mRigBytes + sz.accelerationStructureSize > budget) { ++mStarved; return -1; }
            Alloc a;
            if (!arenaAlloc(sz.accelerationStructureSize, a)) { ++mStarved; return -1; }
            VkDeviceAddress addr = 0;
            VkAccelerationStructureKHR as = createAS(a, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, addr, "blas", sl);
            if (!as) { arenaFree(a); return -1; }
            s.as = as; s.mem = a; s.addr = addr; s.compacted = false; ++s.serial;
            ++mStaticCount; mStaticBytes += a.size;
            if (s.repackInvalidated)
            {
                s.repackInvalidated = false; ++mRepackRebuilt;
                if (mRepackInvalidPending) --mRepackInvalidPending;
            }
            b.scratch = sz.buildScratchSize;
            mScratchUsed += sz.buildScratchSize;
            setSlotAddr(sl, addr);
            tris += b.range.primitiveCount;
            sbuilds.push_back(b);
            return 1;
        };
        bool stop = false;
        for (const uint32_t sl : mTerrPriority)
        {
            if (sbuilds.size() >= builds_max || tris >= tris_max) break;
            if (tryStatic(sl) < 0) { stop = true; break; }
        }
        // Ascending slot order over the want list (= the order of the full scan it
        // replaced: tryStatic acts only on slots without a BLAS stamped within 2 frames,
        // which is exactly what the list holds). Built / expired / out-of-table entries
        // leave; the rest (skipped, or behind the limits) stay while fresh.
        {
            std::sort(mWant.begin(), mWant.end());
            size_t keep = 0, k = 0;
            for (; k < mWant.size(); ++k)
            {
                const uint32_t sl = mWant[k];
                if (stop || sbuilds.size() >= builds_max || tris >= tris_max) break;
                const bool gone = sl >= slot_n || mSlot[sl].as || mSlot[sl].lastUsed == 0 || mSlot[sl].lastUsed + 2 < mFrame;
                int r = 0;
                if (!gone)
                {
                    r = tryStatic(sl);
                    if (r < 0) stop = true;
                }
                if (gone || r > 0)
                {
                    if (sl < mInWant.size()) mInWant[sl] = 0;
                    continue;
                }
                mWant[keep++] = sl;
            }
            for (; k < mWant.size(); ++k) mWant[keep++] = mWant[k];   // not reached this frame
            mWant.resize(keep);
        }
        mBuiltThisFrame = (uint32_t)sbuilds.size();
        build_saturated = stop || sbuilds.size() >= builds_max || tris >= tris_max || mStarved > 0;
    }
    // WARM-UP (#268, sunReady): settled = the used-slot feedback has had time to come
    // back (3 frame slots; 8 build frames) and 4 consecutive frames built everything the
    // TLAS pass asked for, with few instances still waiting to enter the TLAS. A scene
    // that never settles (budget-starved, constant streaming) goes warm on the timeout.
    if (!mWarm)
    {
        constexpr uint32_t kWarmTimeout = 240;
        ++mWarmFrames;
        mWarmSettled = build_saturated ? 0u : mWarmSettled + 1u;
        const bool settled = mWarmFrames >= 8u && mWarmSettled >= 4u && mTlasDeferred < kTlasDeferRebuild;
        if (settled || mWarmFrames >= kWarmTimeout)
        {
            mWarm = true;
            LL_INFOS("VKRT") << "RT: warm after " << mWarmFrames << " build frames (" << mStaticCount << " static BLAS, "
                             << (mStaticBytes >> 20) << " MB" << (settled ? ")" : "; timed out, builds still limited)")
                             << LL_ENDL;
        }
    }

    // ---- SMOOTH TERRAIN MORPH BLAS: build a new / changed one, refit one whose camera
    //      moved > kTerrRefitMove since its last copy; nothing while the camera is still.
    //      Each listed entry gets this frame's mode-3 copy into mTerrPos (re-laid out
    //      every frame: only the BLAS builds read it). ----
    mTerrEntries.clear(); mTerrBuilds.clear();
    mTerrBuiltN = mTerrRefitN = 0;
    if (!mTerrMorph.empty())
    {
        const float* cam = cull.lod_cam_pos;
        static std::vector<std::pair<uint32_t, bool>> s_tplan;   // (slot, refit)
        s_tplan.clear();
        for (const auto& kv : mTerrMorph)
        {
            const uint32_t sl = kv.first;
            const TerrMorph& tm = kv.second;
            if (!slotOk(sl)) continue;
            const SlotInfoCPU& si = slots[sl];
            const bool same = tm.as && tm.slotGen == mSlotGen[sl] && tm.vert_count == si.vert_count
                              && tm.index_count == si.index_count && tm.first_index == si.first_index
                              && tm.iboAddr == R.mIbo.address;
            float m2 = 0.f;
            for (int k = 0; k < 3; ++k) { const float d = cam[k] - tm.cam[k]; m2 += d * d; }
            if (same && m2 <= kTerrRefitMove * kTerrRefitMove) continue;   // still matches the raster
            s_tplan.emplace_back(sl, same);
        }
        // Refits first (they keep existing BLAS true to the raster), then the builds
        // nearest the start of the list; builds respect the scratch / triangle / memory
        // budgets (a deferred one keeps its static BLAS or the TLAS's coarser stand-in).
        std::stable_sort(s_tplan.begin(), s_tplan.end(),
                         [](const std::pair<uint32_t, bool>& a, const std::pair<uint32_t, bool>& b) { return a.second > b.second; });
        uint64_t ttris = 0;
        uint32_t tverts = 0;
        for (const auto& pl : s_tplan)
        {
            const uint32_t sl = pl.first;
            TerrMorph& tm = mTerrMorph[sl];
            const SlotInfoCPU& si = slots[sl];
            TerrBuild b{sl, pl.second, 0};
            if (b.update)
            {
                b.scratch = tm.updateScratch;
                mScratchUsed += tm.updateScratch;
                ++mTerrRefitN;
            }
            else
            {
                if (ttris >= tris_max) continue;
                VkAccelerationStructureGeometryKHR g{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
                g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
                auto& t = g.geometry.triangles;
                t = VkAccelerationStructureGeometryTrianglesDataKHR{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
                t.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
                t.vertexStride = 12;
                t.maxVertex = si.vert_count - 1;
                t.indexType = VK_INDEX_TYPE_UINT16;   // (#573)
                const uint32_t prims = si.index_count / 3;
                VkAccelerationStructureBuildGeometryInfoKHR gi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
                gi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
                gi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
                gi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
                gi.geometryCount = 1; gi.pGeometries = &g;
                VkAccelerationStructureBuildSizesInfoKHR sz{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
                rt.vkGetAccelerationStructureBuildSizesKHR(dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &gi, &prims, &sz);
                if (sz.accelerationStructureSize == 0) continue;
                if (mScratchUsed > 0 && mScratchUsed + sz.buildScratchSize > kScratchBudget) continue;   // next frame
                // (its own old BLAS, if any, is about to go: don't count it twice)
                const VkDeviceSize old = tm.as ? tm.mem.size : 0;
                evictForBudget(sz.accelerationStructureSize, budget);
                if (mStaticBytes + mRigBytes - std::min<uint64_t>(mStaticBytes + mRigBytes, old)
                    + sz.accelerationStructureSize > budget) { ++mStarved; continue; }
                Alloc a;
                if (!arenaAlloc(sz.accelerationStructureSize, a)) { ++mStarved; continue; }
                VkDeviceAddress addr = 0;
                VkAccelerationStructureKHR as = createAS(a, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, addr, "blas-terrain", sl);
                if (!as) { arenaFree(a); continue; }
                if (tm.as)
                {
                    retireAS(tm.as, tm.mem);
                    mStaticBytes -= std::min<uint64_t>(mStaticBytes, tm.mem.size);
                    mTerrBytes -= std::min<uint64_t>(mTerrBytes, tm.mem.size);
                }
                tm.as = as; tm.mem = a; tm.addr = addr;
                tm.slotGen = mSlotGen[sl]; tm.vert_count = si.vert_count; tm.index_count = si.index_count;
                tm.first_index = si.first_index; tm.iboAddr = R.mIbo.address;
                tm.updateScratch = sz.updateScratchSize;
                mStaticBytes += a.size; mTerrBytes += a.size;
                b.scratch = sz.buildScratchSize;
                mScratchUsed += sz.buildScratchSize;
                ttris += prims;
                ++mTerrBuiltN;
                // The static BLAS gives way (its address is replaced below; retired via the ring).
                dropSlotBlas(sl);
                setSlotAddr(sl, addr);
            }
            for (int k = 0; k < 3; ++k) tm.cam[k] = cam[k];
            mTerrEntries.push_back({tm.inst, (uint32_t)si.base_vertex, tverts, si.vert_count});
            mTerrBuilds.push_back(b);
            tverts += si.vert_count;
        }
        // Morphed position buffer (GPU-written by mode 3, read only by these builds).
        if (!mTerrBuilds.empty()
            && !ensureBuf(mTerrPos, (VkDeviceSize)tverts * 12,
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
                          false, true))
        {
            // Unwind: fresh builds lose their BLAS (address published above), refits retry.
            for (const TerrBuild& b : mTerrBuilds)
            {
                if (b.update) { mTerrMorph[b.slot].cam[0] = 1e30f; continue; }
                dropTerrMorph(b.slot);
            }
            mTerrEntries.clear(); mTerrBuilds.clear();
            mTerrBuiltN = mTerrRefitN = 0;
        }
        mTerrGeoms.resize(mTerrBuilds.size());
        mTerrRanges.resize(mTerrBuilds.size());
        for (size_t i = 0; i < mTerrBuilds.size(); ++i)
        {
            const TerrMorph& tm = mTerrMorph[mTerrBuilds[i].slot];
            VkAccelerationStructureGeometryKHR& g = mTerrGeoms[i];
            g = VkAccelerationStructureGeometryKHR{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
            g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
            auto& t = g.geometry.triangles;
            t = VkAccelerationStructureGeometryTrianglesDataKHR{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
            t.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
            t.vertexData.deviceAddress = mTerrPos.address + (VkDeviceAddress)mTerrEntries[i].dst_base * 12;
            t.vertexStride = 12;
            t.maxVertex = tm.vert_count - 1;
            t.indexType = VK_INDEX_TYPE_UINT16;   // (#573)
            t.indexData.deviceAddress = R.mIbo.address + (VkDeviceAddress)tm.first_index * 2;
            mTerrRanges[i] = VkAccelerationStructureBuildRangeInfoKHR{tm.index_count / 3, 0, 0, 0};
        }
    }

    phase("vk_rtP_static");
    // ---- rigged plan ----
    // Rigged face instances are GROUPED per owner (avatar vis slot, else skin id) and
    // TLAS mask class; each group is ONE multi-geometry BLAS (a geometry per face, in
    // rig-entry order) refit / built with ONE build info. ~40 avatars used to mean
    // ~6.7k per-face BLAS refits per frame, each its own driver-recorded build.
    // The group's TLAS record sits at its first member's instance id with custom index
    // 0x800000 | first entry; consumers map (custom, geometry index) back to the face's
    // instance id through the rig entries (clean_rt_common.glsl rtHitInstance).
    struct RigBuild { uint64_t key; uint32_t e0, n; bool update; VkDeviceSize scratch; };
    using RigCand = RigPlanCand;
    static std::vector<RigBuild> s_rbuilds;
    std::vector<RigBuild>& rbuilds = s_rbuilds;
    rbuilds.clear();
    std::vector<RigCand>& cands = mRigCands;   // C3: kept (with mRigEntries) while the key holds
    uint32_t rig_verts = 0;
    uint32_t rig_noshadow = 0;   // reduced-tier rigged faces kept out of the TLAS (#31)
    const bool rig_bufs = do_rigged && R.mCand.buffer && R.mCandMeta.buffer && R.mSkinVbo.buffer;
    // C3: PLAN REUSE KEY -- everything the candidate walk below reads.
    bool plan_cache = true;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<bool> s_plan_cache(gSavedSettings, "RenderVKRTRigPlanCache", true);
    plan_cache = s_plan_cache;
#endif
    // #747 / #748 RIGGED CLUSTER RT (llcleanclusterrtvk.h RIGGED): the rigged faces the cluster
    // raster draws this frame (2a's switch: rigged ClusterInstances flagged kInstDraw) are keyed
    // into their OWN groups (bit 0 of the mask-class byte: free for the rigged classes), so TLAS
    // pass B can give a whole switched group its members' cluster records -- or keep its legacy
    // record -- without touching the avatar's other faces (flexi, system body, alpha-blend:
    // never on the cluster path). Empty with cluster LOD off: the plan is the old one exactly.
    uint64_t clsw_hash = 0;
    uint32_t clsw_n = 0;
    mClSwitched.clear();
    mClOnlyGroups = 0;
    // LEGACY-SKIN SKIP (renderer clusterLegacySkipPlan, av_vis kAvVisClSkip): a switched avatar
    // whose legacy pre-skin did not run this frame. Its switched groups are CLUSTER-ONLY: entries
    // (pass B's per-member cluster records, mode 4's check) but no rig_pos range, no rig copy, no
    // legacy BLAS build / refit -- pass B writes the cluster records, or nothing for a group whose
    // cluster data is missing (the mask re-arms the legacy BLAS kFrameSlots frames later).
    const bool skip_avok = R.mAvVisSSBO.buffer && R.mAvVis[kAvVisSlots] != 0;
    auto clOnlyOwner = [&](uint64_t owner)
    {
        return skip_avok && owner < kAvVisSlots && (R.mAvVis[owner] & LLCleanRendererVK::kAvVisClSkip) != 0u;
    };
    uint64_t skip_bits[kAvVisSlots / 64] = {};
    if (skip_avok)
        for (uint32_t a = 0; a < kAvVisSlots; ++a)
            if (R.mAvVis[a] & LLCleanRendererVK::kAvVisClSkip) skip_bits[a >> 6] |= 1ull << (a & 63u);
    // RT AVATAR RULE (2026-10-14): the avatar-level test the walk below applies to every rigged
    // row (legacy RT): drawn by some CPU-decided pass or a ray-traced capture candidate (#517), and
    // not a no-shadow reduced-tier wearer (#31: every production ray is a shadow ray). The cluster
    // RT builds RT data only for these avatars (FrameIn::rigTraced) and the consistency check
    // expects exactly these of the cluster-path avatars in pass B. (No av_vis yet: every avatar.)
    const bool rt_avok = R.mAvVisSSBO.buffer && R.mAvVis[kAvVisSlots] != 0;
    auto rtAvUnseen = [&](uint32_t a)
    {
        return rt_avok && a < kAvVisSlots
            && !(R.mAvVis[a] & (LLCleanRendererVK::kAvVisAny | LLCleanRendererVK::kAvVisRtProbe));
    };
    auto rtAvNoShadow = [&](uint32_t a)
    {
        return rt_avok && a < kAvVisSlots && (R.mAvVis[a] & LLCleanRendererVK::kAvVisNoShadow) != 0u;
    };
    // the avatars (joint-table slots) the cluster path draws this frame: the cluster side's own
    // ClusterInstance avatar slot (= the skin meta's avatar_idx the walk reads)
    uint32_t cl_av_bits[kAvVisSlots / 32] = {};
    if (mClusterRT && mClusterRT->riggedWanted() && R.mCluster && R.clusterLodOn() && R.mCluster->ready())
    {
        mClSwitched.assign((inst_n + 31) / 32, 0u);
        for (const LLClusterFmt::ClusterInstance& c : R.mCluster->select().cpuInstances())
        {
            const uint32_t ri = c.render_instance;
            if (!(c.flags & LLClusterFmt::CLI_RIGGED) || !(c.flags & LLCleanClusterSelectVK::kInstDraw) || ri >= inst_n) continue;
            if (c.flags & LLCleanClusterSelectVK::kInstAvatar)
            {
                const uint32_t ca = (c.flags >> LLCleanClusterSelectVK::kInstAvShift) & kAvatarSlotMask;
                cl_av_bits[ca >> 5] |= 1u << (ca & 31u);
            }
            mClSwitched[ri >> 5] |= 1u << (ri & 31u);
            uint64_t h = (uint64_t)ri * 0x9E3779B97F4A7C15ull;
            h ^= h >> 29;
            clsw_hash += h;   // (order-independent)
            ++clsw_n;
        }
    }
    auto clSwitched = [&](uint32_t id) { return (id >> 5) < mClSwitched.size() && ((mClSwitched[id >> 5] >> (id & 31u)) & 1u); };
    // CLUSTER-ONLY RT (2026-10-13, owner decision: with cluster LOD on there is no legacy
    // rendering of cluster-path meshes, not even transitional): while the cluster RT traces
    // switched avatars, a rigged face ON THE CLUSTER PATH (select().rowOnClusterPath) is traced
    // only from its cluster cut -- switched: a cluster-only group (pass B gives it its cluster
    // records); not switched yet (loading / building): not traced at all (pop-in accepted). Only
    // faces OUTSIDE the cluster path (alpha-blend, flexi, system body, non-mesh) keep a legacy
    // BLAS, and only they count for the legacy avatar budget / drop. Dropped over budget, the
    // walk keeps the switched faces alone. Cluster LOD off (or no cluster RT for switched avatars:
    // RenderVKClusterRT 0 / RenderVKClusterRTRigged 0): the legacy plan as before.
    const bool rig_cl_rt = R.clusterLodOn() && clusterTracesSwitched() && R.mCluster;
    const bool rig_sw_only_plan = mRigDropped;   // (walk: switched faces only)
    const bool rig_ok = rig_bufs && (!rig_sw_only_plan || (clsw_n > 0 && clusterTracesSwitched()));
    // 2026-10-14 (the cluster path REPLACES the legacy one): in the renderer's cluster-owns-rigged
    // mode a cluster-path row has no legacy geometry at all -- with RenderVKClusterRT 0 /
    // RenderVKClusterRTRigged 0 (or the cluster RT inactive) it is simply not traced, never from a
    // legacy BLAS.
    const bool rig_owned = R.clusterOwnsRigged() && R.mCluster;
    const LLCleanClusterSelectVK* cl_sel = (rig_cl_rt || rig_owned) ? &R.mCluster->select() : nullptr;
    uint64_t plan_key = 0xcbf29ce484222325ull;
    {
        auto mix = [&plan_key](uint64_t v) { plan_key = (plan_key ^ v) * 0x100000001B3ull; };
        mix(clsw_hash); mix(clsw_n);
        for (const uint64_t w : skip_bits) mix(w);   // (cluster-only groups: no rig_pos ranges)
        mix(mRigRowGen); mix(mSlotGenTotal); mix(slot_n); mix(inst_n);
        mix(R.mLastGrpGen); mix(R.mCpuGroups.size()); mix(R.mGeomPackGen); mix(R.mSkinMetaGen);
        mix(R.mSkinCount); mix(R.mCpuSkinMeta.size()); mix(R.mIdxValid); mix(R.mGeomIdxEnd);
        mix(vbo_verts); mix(rig_ok ? 1u : 0u);
        mix((rig_sw_only_plan ? 2u : 0u) | (rig_cl_forced ? 4u : 0u) | (rig_cl_rt ? 8u : 0u) | (rig_owned ? 16u : 0u));   // (walk / cluster-only groups)
        const bool avok_k = R.mAvVisSSBO.buffer && R.mAvVis[kAvVisSlots] != 0;
        mix(avok_k ? 1u : 0u);
        if (avok_k)
        {
            // RT candidacy (#517: the CPU-decided passes + a ray-traced capture's), no-shadow,
            // reduced and the reduced LOD cap -- not the per-frame pose-dirty bit, nor which
            // pass sees an avatar (only whether one does)
            // + the #559 pressure cap and #561 screen-LOD level / scale (the slot the cull
            // draws: the plan replays its pick, see below)
            const uint32_t keep = LLCleanRendererVK::kAvVisNoShadow | LLCleanRendererVK::kAvVisReduced
                                | (7u << LLCleanRendererVK::kAvVisLodShift)
                                | (7u << LLCleanRendererVK::kAvVisPressShift)
                                | (7u << LLCleanRendererVK::kAvVisSlodShift);
            const uint32_t cand_bits = LLCleanRendererVK::kAvVisAny | LLCleanRendererVK::kAvVisRtProbe;
            for (uint32_t a = 0; a < kAvVisSlots; ++a)
            {
                mix((R.mAvVis[a] & keep) | ((R.mAvVis[a] & cand_bits) ? 1u : 0u));
                if ((R.mAvVis[a] >> LLCleanRendererVK::kAvVisSlodShift) & 7u)
                    mix(R.mAvVis[LLCleanRendererVK::kAvLodBase + a]);
            }
        }
        mix(LLCleanRigDecimator::instance().tableGeneration());
    }
    const bool reuse_plan = plan_cache && rig_ok && mRigPlanValid && plan_key == mRigPlanKey;
    LLVKHitch::count("rt_rigPlanReuse", reuse_plan ? 1u : 0u);
    if (reuse_plan)
    {
        rig_verts = mRigPlanVerts;
        rig_noshadow = mRigPlanNoShadow;
        LLVKHitch::count("rt_rigNoShadow", rig_noshadow);
    }
    else
    {
        mRigPlanValid = false;
        mRigGeomValid = false;
        cands.clear();
        mRigEntries.clear();
        mRigPlanSkips.clear();
        cands.reserve(mRigIds.size());
        std::memset(mRigPlanAvWhy, 0, sizeof(mRigPlanAvWhy));
        mRigPlanLegFaces = mRigPlanLegRoutable = mRigPlanLegNever = 0;
    }
    // #692: faces the walk below leaves out because their slot row fails slotOk (owner, tris)
    static std::vector<std::pair<uint64_t, uint32_t>> s_rig_skips;
    s_rig_skips.clear();
    if (rig_ok && !reuse_plan)
    {
        const uint8_t* ib = R.mCpuInstances.data();
        const uint8_t* groups = R.mCpuGroups.data();
        const uint32_t grp_n = (uint32_t)(R.mCpuGroups.size() / 16);
        const uint32_t* meta = reinterpret_cast<const uint32_t*>(R.mCpuSkinMeta.data());
        const uint32_t meta_words = (uint32_t)(R.mCpuSkinMeta.size() / 4);
        const bool avok = R.mAvVisSSBO.buffer && R.mAvVis[kAvVisSlots] != 0;
        // C3: record every slot whose contents this walk reads (slotChanged on one of them
        // invalidates the plan).
        for (const uint32_t ps : mRigPlanSlotList) if (ps < mRigPlanSlot.size()) mRigPlanSlot[ps] = 0;
        mRigPlanSlotList.clear();
        if (mRigPlanSlot.size() < slot_n) mRigPlanSlot.resize(slot_n, 0);
        auto markSlot = [&](uint32_t v)
        {
            if (v < slot_n && !mRigPlanSlot[v]) { mRigPlanSlot[v] = 1; mRigPlanSlotList.push_back(v); }
        };
        for (const uint32_t id : mRigIds)   // rigged rows only (refreshRigSet)
        {
            if (id >= inst_n) continue;
            const uint8_t* in = ib + (size_t)id * kInstBytes;
            const uint32_t flags = rd32(in + kInstFlags), skin = rd32(in + kInstSkin), gid = rd32(in + kInstGroup);
            const bool row_bad = !(flags & 1u) || skin == kSentinel || (flags & 8u) || gid == kSentinel || gid >= grp_n;
            if (row_bad || skin >= R.mSkinCount)
            {
                // (a cluster-switched row the plan cannot take: its avatar's reason, for the
                // consistency check -- the cluster path draws it, RT would not trace it)
                if (clSwitched(id) && skin != kSentinel && (size_t)skin * kSkinMetaWords < meta_words)
                {
                    const uint32_t wa = meta[(size_t)skin * kSkinMetaWords];
                    if (wa < kAvVisSlots) mRigPlanAvWhy[wa] |= row_bad ? 2u : 1u;
                }
                continue;
            }
            if (rig_sw_only_plan && !clSwitched(id)) continue;   // (legacy class dropped: not traced)
            // (cluster path, not ready -- or no cluster RT for it: not traced)
            if (cl_sel && !(rig_cl_rt && clSwitched(id)) && cl_sel->rowOnClusterPath(id)) continue;
            uint32_t av = kSentinel;
            if ((size_t)skin * kSkinMetaWords < meta_words) av = meta[(size_t)skin * kSkinMetaWords];
            // avatar not drawn by any CPU-decided pass, nor a ray-traced capture candidate
            // (#517: test the bits -- an off-screen avatar keeps its tier bits)
            if (avok && av < kAvVisSlots
                && !(R.mAvVis[av] & (LLCleanRendererVK::kAvVisAny | LLCleanRendererVK::kAvVisRtProbe)))
                continue;
            // Reduced tier (#31): a no-shadow avatar stays out of the TLAS (every
            // production ray is a shadow ray; no free mask bit) -- no BLAS refit either.
            const uint32_t avbits = (avok && av < kAvVisSlots) ? R.mAvVis[av] : 0u;
            if (avbits & LLCleanRendererVK::kAvVisNoShadow) { ++rig_noshadow; continue; }
            // Mask class exactly as clean_rt_instances.comp maskClass(f, rigged = true) (the own
            // avatar shares the rigged bit: its own owner key keeps its groups apart).
            const uint32_t cls = (flags & 2048u) ? 0x20u : (flags & 32u) ? 0x10u : (flags & 2u) ? 0x04u : 0x08u;
            // FACING (clean_rt_instances.comp rigCullFlags): the group's TLAS record culls (or
            // not) every face of its BLAS alike, and the shadow pass culls single-sided faces
            // only -- double-sided (flag 256) faces get groups of their own (key bit 1, a bit
            // no mask class uses), so one group never mixes the two.
            const uint32_t dsb = (flags & 256u) ? 0x02u : 0u;
            const uint64_t owner = av < kAvVisSlots ? (uint64_t)av : (0x100000000ull | skin);
            // Owner distance (closest point of its bounds to the main camera; unknown =
            // last). Every entry of a group shares its owner, so groups stay contiguous.
            float dist = 1e30f;
            if (avok && av < kAvVisSlots)
            {
                float d = 0.f;
                std::memcpy(&d, &R.mAvVis[LLCleanRendererVK::kAvDemBase + 2 * av], 4);
                if (std::isfinite(d) && d > 0.f) dist = d;
                else if (d == 0.f) dist = 0.f;   // camera inside / touching the bounds
            }
            // CLUSTER-ONLY ROWS COME FROM THE CLUSTER SIDE (2026-10-14, RT avatar regression): a
            // row the cluster path draws (mClSwitched = select().cpuInstances() with kInstDraw)
            // whose group traces only its cluster cut needs NO legacy slot -- and in the
            // renderer's cluster-owns-rigged mode it has none (fillFromCleanStores drops the
            // routed-only groups' slots: SlotInfo{}, index_count 0). The slot walk below used to
            // reject every such row at slotOk -- silently (index_count 0 is not a #692 skip) --
            // so its avatar never reached pass B and cast no RT shadow; only avatars whose LOD
            // groups a legacy row still shared kept a slot ("RT avatars: cluster cut 3" with 56
            // avatars ready on the "Cluster RT:" line). The legacy slot table is not consulted
            // for these rows: the entry carries no geometry (no rig copy writes it, no BLAS reads
            // it); pass B gives the group its members' cluster records (nothing while one is
            // missing: mode 4).
            if (clSwitched(id) && (rig_cl_forced || clOnlyOwner(owner)))
            {
                cands.push_back({(owner << 8) | cls | dsb | 1u, id, kSentinel, (flags & 6u) == 0u ? 1u : 0u, dist});
                continue;
            }
            uint32_t sl = kSentinel;
            // The level the cull picks (clean_cull.comp): the #559 pressure cap (that level
            // or coarser first, finer only when nothing coarser is resident), a reduced
            // wearer's biased LOD (desired, then finer, then coarser), else the highest
            // resident one.
            auto lvlSlot = [&](uint32_t L) -> uint32_t
            {
                const uint32_t v = rd32(groups + (size_t)gid * 16 + (size_t)L * 4);
                markSlot(v);
                return (v != kSentinel && v < slot_n && slots[v].index_count != 0) ? v : kSentinel;
            };
            const uint32_t cap = (avbits & LLCleanRendererVK::kAvVisReduced) ? ((avbits >> LLCleanRendererVK::kAvVisLodShift) & 7u) : 0u;
            const uint32_t want = cap != 0u ? cap - 1u : 4u;
            const uint32_t press = av < kAvVisSlots ? (avbits >> LLCleanRendererVK::kAvVisPressShift) & 7u : 0u;
            if (press != 0u)
            {
                const uint32_t pc_ = std::min(want, press - 1u);
                // (#734: the cap, else finer, coarser last -- = the cull)
                for (uint32_t L = pc_; L <= 3u && sl == kSentinel; ++L) sl = lvlSlot(L);
                for (int L = (int)pc_ - 1; L >= 0 && sl == kSentinel; --L) sl = lvlSlot((uint32_t)L);
            }
            if (want < 3u && sl == kSentinel)
            {
                for (uint32_t k = 0; k < 4u && sl == kSentinel; ++k)
                    sl = lvlSlot((want + k <= 3u) ? want + k : 3u - k);
            }
            // The highest level with indices, as the cull (clean_cull.comp): a top slot
            // that exists but is empty / zeroed (#88) used to drop the face from the TLAS
            // entirely while the raster drew the next level down -- a missing caster
            // (lit blotches) right where the avatar is in view.
            for (int L = 3; L >= 0 && sl == kSentinel; --L)
            {
                const uint32_t v = rd32(groups + (size_t)gid * 16 + (size_t)L * 4);
                markSlot(v);
                if (v != kSentinel && v < slot_n && slots[v].index_count != 0) { sl = v; break; }
            }
            // #561: the decimated step the cull draws for a far wearer (rigScreenLod): its
            // pre-skinned range has the step's vertices, and the rig copy only accepts the
            // slot the cull drew. (Not HUD / system-body parts / flexi, as the cull.)
            if (sl != kSentinel && av < kAvVisSlots && (flags & (131072u | 524288u)) == 0u)
            {
                const uint32_t lvl = (avbits >> LLCleanRendererVK::kAvVisSlodShift) & 7u;
                float mq = 0.f;
                std::memcpy(&mq, &R.mAvVis[LLCleanRendererVK::kAvLodBase + av], 4);
                if (lvl && mq > 0.f)
                {
                    // every step: one becoming resident changes the cull's choice
                    if (const LLCleanRigDecimator::Info* di = LLCleanRigDecimator::instance().find(sl))
                        for (uint32_t k = 0; k < LLCleanRigDecimator::kSteps; ++k)
                            if (di->dec[k] != kSentinel) markSlot(di->dec[k]);
                    const float bs = LLCleanRigDecimator::bindScale(meta, meta_words, skin, kSkinMetaWords);
                    uint32_t vout = 0;
                    const uint32_t dsl = LLCleanRigDecimator::instance().selectCpu(
                        sl, slots[sl].index_count, slots[sl].vert_count, LLCleanRigDecimator::tolerancePx(lvl) * mq, bs,
                        [&](uint32_t x)
                        {   // = clean_cull.comp rigStepOk
                            if (x >= slot_n) return false;
                            const SlotInfoCPU& t = slots[x];
                            return LLCleanRigDecimator::stepRowOk(t.index_count, t.vert_count, t.base_vertex, t.first_index,
                                                                  t.has_weights, slots[sl].index_count, slots[sl].vert_count,
                                                                  R.mIdxValid, vbo_verts);
                        }, vout);
                    if (dsl != sl) { markSlot(dsl); sl = dsl; }
                }
            }
            markSlot(sl);
            if (!slotOk(sl))
            {
                // #692: the cull draws this face (the row has indices) but RT cannot use it
                // -- counted per owner and logged when it lasts (rigNoteOut RIG_OUT_SLOT).
                if (sl < slot_n && slots[sl].index_count != 0)
                    s_rig_skips.emplace_back(av < kAvVisSlots ? (uint64_t)av : (0x100000000ull | skin),
                                             slots[sl].index_count / 3);
                continue;
            }
            // (#747 / #748: bit 0 = a cluster-switched face -- its own group, see clsw above)
            // (owner / cls / dsb / dist: computed above, before the cluster-side shortcut)
            cands.push_back({(owner << 8) | cls | dsb | (clSwitched(id) ? 1u : 0u), id, sl, (flags & 6u) == 0u ? 1u : 0u, dist});
            if (!clSwitched(id))
            {
                // (undrawn diagnosis: a legacy face the static cluster-routable rule accepts has no
                // legacy pre-skin reserved in the cluster-owns-rigged mode)
                ++mRigPlanLegFaces;
                if (LLCleanClusterSelectVK::rowClusterRoutable(flags, skin, gid)) ++mRigPlanLegRoutable;
                mRigPlanLegNever |= flags & LLCleanClusterSelectVK::kRowNever;
            }
        }
        LLVKHitch::count("rt_rigNoShadow", rig_noshadow);
        // NEAR GROUPS FIRST: (re)builds run in this order under kScratchBudget, and a group
        // over it is dropped from RT for the frame -- on heavy frames (arrival, teleport,
        // rig_pos growth) a close-up avatar with a high joint-table slot used to queue
        // behind far ones and lose its shadows. Ties (and unknown distances) by key.
        std::stable_sort(cands.begin(), cands.end(),
                         [](const RigCand& a, const RigCand& b) {
                             if (a.dist != b.dist) return a.dist < b.dist;   // (one owner: one distance)
                             return a.key < b.key;
                         });
        mRigEntries.reserve(cands.size());
        for (const RigCand& c : cands)
        {
            // (a cluster-only group's member: no rig copy writes it, no BLAS reads it; a
            // cluster-side row has no legacy slot at all -- kSentinel, an entry without geometry)
            const bool cl_only = (c.key & 1u) && (rig_cl_forced || clOnlyOwner(c.key >> 8));
            if (c.slot >= slot_n)
            {
                mRigEntries.push_back({c.id, 0u, 0u, 0u, 0u, 0u, 0, 0});
                continue;
            }
            const SlotInfoCPU& si = slots[c.slot];
            mRigEntries.push_back({c.id, cl_only ? 0u : rig_verts, si.vert_count, si.first_index, si.index_count,
                                   (uint32_t)si.base_vertex, 0, 0});
            if (!cl_only) rig_verts += si.vert_count;
        }
        mRigPlanKey = plan_key; mRigPlanValid = true;
        mRigPlanVerts = rig_verts; mRigPlanNoShadow = rig_noshadow;
        // #692: per owner, what the slot check left out vs. the owner's whole rigged set.
        if (!s_rig_skips.empty())
        {
            std::sort(s_rig_skips.begin(), s_rig_skips.end());
            for (const auto& sk : s_rig_skips)
            {
                if (mRigPlanSkips.empty() || mRigPlanSkips.back().owner != sk.first)
                    mRigPlanSkips.push_back({sk.first, 0u, 0u, 0u, 0u});
                RigPlanSkip& ps = mRigPlanSkips.back();
                ++ps.faces; ps.tris += sk.second;
                ++ps.all_faces; ps.all_tris += sk.second;
            }
            for (size_t ci = 0; ci < cands.size(); ++ci)
            {
                const uint64_t ow = cands[ci].key >> 8;
                const auto it = std::lower_bound(mRigPlanSkips.begin(), mRigPlanSkips.end(), ow,
                                                 [](const RigPlanSkip& a, uint64_t o) { return a.owner < o; });
                if (it == mRigPlanSkips.end() || it->owner != ow) continue;
                ++it->all_faces; it->all_tris += mRigEntries[ci].index_count / 3;
            }
        }
    }
    if (!rig_ok) mRigPlanSkips.clear();
    for (const RigPlanSkip& ps : mRigPlanSkips)
        rigNoteOut(ps.owner << 8, RIG_OUT_SLOT, ps.faces, ps.tris, false, ps.all_faces, ps.all_tris);
    // Rig position buffer (GPU-written by the rig copy). Grow before planning addresses.
    shrinkIfOversized(mRigPos, (VkDeviceSize)rig_verts * kRigPosStride, mRigPosOverFrames);
    if (!mRigEntries.empty() &&
        !ensureBuf(mRigPos, (VkDeviceSize)rig_verts * kRigPosStride,
                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
                   false, true))
        { mRigEntries.clear(); cands.clear(); mRigPlanValid = false; mRigGeomValid = false; }
    // The rig BLAS inputs moved (rig_pos grown): treat as a topology change -- rebuild
    // every rigged BLAS this frame instead of refitting against a different allocation.
    // Also after idle frames: the skinned positions changed while nothing refit them, so
    // every rigged BLAS is rebuilt from scratch on the first active frame.
    const bool rigInputsMoved = mRigPos.address != mRigPosAddrSeen || mIdleSinceBuild;
    mRigPosAddrSeen = mRigPos.address;
    // Geometries + ranges for every entry (stable storage: build infos point into these).
    // C3: unchanged while the plan is reused and rig_pos / the IBO did not move.
    const bool geoms_ok = reuse_plan && mRigGeomValid && mRigGeomPosAddr == mRigPos.address
                          && mRigGeomIboAddr == R.mIbo.address && mRigGeoms.size() == mRigEntries.size();
    if (!geoms_ok)
    {
    mRigGeoms.resize(mRigEntries.size());
    mRigRanges.resize(mRigEntries.size());
    for (size_t ei = 0; ei < mRigEntries.size(); ++ei)
    {
        const RigEntryCPU& e = mRigEntries[ei];
        VkAccelerationStructureGeometryKHR& g = mRigGeoms[ei];
        g = VkAccelerationStructureGeometryKHR{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        // Per-geometry opacity (the TLAS record no longer forces it): alpha-mask / blend
        // faces stay candidates for the consumers' alpha test.
        g.flags = cands[ei].opaque ? VK_GEOMETRY_OPAQUE_BIT_KHR : 0;
        auto& t = g.geometry.triangles;
        t = VkAccelerationStructureGeometryTrianglesDataKHR{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
        t.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        t.vertexData.deviceAddress = mRigPos.address + (VkDeviceAddress)e.dst_base * kRigPosStride;
        t.vertexStride = kRigPosStride;
        t.maxVertex = e.vert_count ? e.vert_count - 1 : 0;   // (cluster-only entries: no geometry, never built)
        t.indexType = VK_INDEX_TYPE_UINT16;   // (#573)
        t.indexData.deviceAddress = R.mIbo.address + (VkDeviceAddress)e.first_index * 2;
        mRigRanges[ei] = VkAccelerationStructureBuildRangeInfoKHR{e.index_count / 3, 0, 0, 0};
    }
    mRigGeomValid = mRigPlanValid;
    mRigGeomPosAddr = mRigPos.address; mRigGeomIboAddr = R.mIbo.address;
    }
    rbuilds.reserve(64);
    std::vector<uint32_t> sig, prims;
    // #1007 IMMEDIATE REFIT: on a teleport (the camera jumped since the last build) or a
    // region crossing every group copies + refits this frame (no gate skip of any kind); an
    // avatar whose distance jumped (sit, in-sim teleport: its bounds moved) refits its groups.
    bool rig_skip_off = mRigOriginShifted;
    mRigOriginShifted = false;
    {
        float d2 = 0.f;
        for (int k = 0; k < 3; ++k) { const float d = cull.lod_cam_pos[k] - mRigCadCam[k]; d2 += d * d; }
        if (mRigCadCamValid && !(d2 < kTlasCamJump * kTlasCamJump)) rig_skip_off = true;
        for (int k = 0; k < 3; ++k) mRigCadCam[k] = cull.lod_cam_pos[k];
        mRigCadCamValid = true;
    }
    static std::vector<uint8_t> s_dist_jump;
    s_dist_jump.assign(kAvVisSlots, 0u);
#if defined(LL_HAS_CLEAN_PIPELINE)
    if (R.mAvVisSSBO.buffer)
        for (uint32_t a = 0; a < kAvVisSlots; ++a)
        {
            float d = 0.f;
            std::memcpy(&d, &R.mAvVis[LLCleanRendererVK::kAvDemBase + 2 * a], 4);
            if (!std::isfinite(d)) d = 0.f;
            if (std::fabs(d - mRigPrevDist[a]) > kRigDistJump) s_dist_jump[a] = 1u;
            mRigPrevDist[a] = d;
        }
#endif
    // POSE SKIP (#1007): the summed per-frame pose change of avatar slot av since frame
    // `since` (its group's last copy + refit) -- an upper bound on how far any joint of the
    // BLAS's pose sits from this frame's (triangle inequality over mode 5's readings). The
    // readings arrive one frame-slot round late, so the newest frames have none yet: each
    // counts as the largest of the newest three readings (the pose keeps moving the way it
    // just did; onset of a motion is caught that late, bounded by the threshold + the cap).
    // No reading in the last 6 frames -> unknown (never skipped).
    auto poseDrift = [&](uint32_t av, uint32_t since) -> float
    {
        if (av >= mPoseHist.size() || mFrame <= since || mFrame - since >= kPoseHist) return 1e30f;
        const PoseHist& h = mPoseHist[av];
        uint32_t newest = 0;
        for (uint32_t i = 0; i < kPoseHist; ++i)
            if (h.frame[i] <= mFrame && h.frame[i] > newest && mFrame - h.frame[i] < kPoseHist) newest = h.frame[i];
        if (newest == 0 || mFrame - newest > 6u) return 1e30f;
        float recent = 0.f;
        for (uint32_t f = newest > 2u ? newest - 2u : 1u; f <= newest; ++f)
            if (h.frame[f % kPoseHist] == f) recent = std::max(recent, h.d[f % kPoseHist]);
        float sum = 0.f;
        for (uint32_t f = since + 1u; f <= mFrame; ++f)
            sum += h.frame[f % kPoseHist] == f ? h.d[f % kPoseHist] : recent;
        return sum;
    };
    for (size_t g0 = 0; g0 < cands.size();)
    {
        size_t g1 = g0 + 1;
        while (g1 < cands.size() && cands[g1].key == cands[g0].key) ++g1;
        const uint32_t n = (uint32_t)(g1 - g0);
        const uint64_t key = cands[g0].key;
        // C3: entries persist across frames now -- only a group built / refit THIS frame
        // carries a BLAS address (a skipped or dropped group must not keep a retired one).
        mRigEntries[g0].blas_lo = mRigEntries[g0].blas_hi = 0;
        auto buildSig = [&]()
        {
            sig.clear(); prims.clear();
            for (size_t i = g0; i < g1; ++i)
            {
                const RigEntryCPU& e = mRigEntries[i];
                const uint32_t sl = cands[i].slot;
                sig.insert(sig.end(), {sl, sl < mSlotGen.size() ? mSlotGen[sl] : 0u, e.vert_count, e.index_count,
                                       cands[i].opaque});
                prims.push_back(e.index_count / 3);
            }
        };
        if ((key & 1u) && (rig_cl_forced || clOnlyOwner(key >> 8)))
        {
            // CLUSTER-ONLY (legacy-skin skip): no legacy BLAS this frame. One kept from earlier
            // frames is left to age out (kRigKeepFrames, free on liveness) and is rebuilt, not
            // refit, if the group falls back to it later (its positions are long stale).
            const auto it = mRig.find(key);
            if (it != mRig.end()) { it->second.builtFrame = 0; it->second.lastRefit = 0; it->second.stale = true; }
            ++mClOnlyGroups;
            g0 = g1;
            continue;
        }
        RigBlas& rb = mRig[key];
        RigBuild b{key, (uint32_t)g0, n, false, 0};
        // A reused plan has the inputs of the frame that last compared or (re)built this
        // group, so an existing BLAS still matches its signature: skip the compare.
        if (!reuse_plan) buildSig();
        // geom_same: the BLAS matches the group's geometries (a refit is valid); same: and
        // its periodic quality rebuild is not due yet.
        const bool geom_same = !rigInputsMoved && rb.as && (reuse_plan || rb.sig == sig);
        // #973 RenderVKRTRigForceRebuild: never refit (a rebuild over the scratch budget still
        // falls back to a refit of the matching BLAS below, as the periodic quality rebuild does).
        const bool same = geom_same && rb.builtFrame + kRigRebuildFrames > mFrame && !rig_force_rebuild
                          && !rb.forceRebuild;   // (#973: its input was collapsed by the rig copy check)
        // REFIT GATE (B8): skip the copy + refit when the skinned positions IN THE GROUP'S
        // SPACE are provably unchanged (gate >= 1), or every 2nd frame for a far avatar
        // (gate 2). #632: an avatar-owned group's BLAS is in the avatar's pelvis frame (the
        // TLAS record carries the pelvis), so a HELD pose (slotPoseClean) is enough -- the
        // root may move (walking at a throttled animation rate, vehicles); a world-space
        // group needs the root still too (slotWorldClean).
        // Needs the layout unchanged (geoms_ok: same plan, rig_pos + IBO not moved), so
        // this group's rig_pos range still holds what its BLAS was last refit from.
        if (same && rig_gate >= 1 && geoms_ok && gframe_seq && rb.lastRefit != 0
            && mFrame - rb.lastRefit < kRigForceRefit && !rig_skip_off)
        {
            const uint64_t owner = key >> 8;
            const uint32_t av = owner < kAvVisSlots ? (uint32_t)owner : kSentinel;
            bool still = false;
            uint32_t period = 1;   // refit every Nth frame (gate 2: distance / screen size)
            bool pose_ok = false;  // #1007: the pose skip may test this group
            float av_dist = 0.f;
#if defined(LL_HAS_CLEAN_PIPELINE)
            if (av != kSentinel)
            {
                std::memcpy(&av_dist, &R.mAvVis[LLCleanRendererVK::kAvDemBase + 2 * av], 4);
                if (!std::isfinite(av_dist)) av_dist = 0.f;
                // (= clean_rt_instances.comp rigFrame: a joint-table row -> avatar-local)
                const bool local = av < jt_slots;
                still = anim_now && (local ? anim.slotPoseClean(av) : anim.slotWorldClean(av))
                        && !R.mRigCacheSlotDirty[av];
                if (still && local && !anim.slotWorldClean(av)) ++mRigRefitSkippedMoving;
                // #1007 IMMEDIATE REFIT: skin meta changed (outfit / attachment / mesh change
                // that kept the geometry signature) or the avatar's distance jumped -> no
                // cadence or pose skip for it this frame (a held-pose proof still holds).
                const bool changed = R.mRigCacheSlotDirty[av] || s_dist_jump[av];
                // POSE SKIP candidates: avatar-local groups only (a world-space group's BLAS
                // holds absolute positions: root motion alone moves it), never our own avatar.
                pose_ok = local && !changed && pose_thr > 0.f && !R.mRigSelfSlot[av];
                // Never throttled: our own avatar and the protected set (#561: the largest
                // on screen / within RenderVKRiggedProtectMeters) -- near avatars' RT
                // shadows follow them every frame.
                // NO-SELF-SHADOW (classifyNoSelf): only an avatar tagged this frame may lag -- its
                // stale BLAS then never shadows its own surface (rtSelfCandidate).
                if (rig_gate >= 2 && rig_cadence && !changed && !R.mRigSelfSlot[av] && !R.mRigProtect[av]
                    && (R.mAvVis[av] & LLCleanRendererVK::kAvVisRtNoSelf))
                {
                    // DISTANCE BANDS (#1007): every 2nd frame past RenderVKRigRefitFarMeters,
                    // every 4th past RenderVKRigRefitVeryFarMeters (both capped below).
                    if (rig_far > 0.f && av_dist > rig_far) period = 2;
                    if (rig_very_far > 0.f && av_dist > std::max(rig_very_far, rig_far))
                        period = std::max(period, std::min(4u, rig_max_period));
                    // SCREEN-SIZE RATE (crowd perf): every 2nd / 3rd / 4th frame below
                    // RenderVKRigRefitFullPx, half, a quarter of it (on-screen bounds
                    // height, this frame), capped at RenderVKRigRefitMaxPeriod.
                    const float px = R.mRigAvPx[av];
                    if (rig_full_px > 0.f && px > 0.f && std::isfinite(px))
                    {
                        const uint32_t pp = px >= rig_full_px ? 1u : px >= rig_full_px * 0.5f ? 2u
                                          : px >= rig_full_px * 0.25f ? 3u : 4u;
                        period = std::max(period, std::min(pp, rig_max_period));
                    }
                }
            }
#endif
            // Throttled groups are staggered by key (1/N of them refit on each frame, not
            // all on the same one); never more than N-1 skipped dirty frames in a row.
            const uint32_t phase = (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 32);
            const bool far_skip = period > 1u && ((mFrame + phase) % period) != 0u && mFrame - rb.lastRefit < period;
            const bool held_skip = still && !rb.stale;
            // POSE SKIP (#1007, RenderVKRigRefitPoseThreshold): the pose moved, but by less
            // than the threshold (m, scaled by distance past kPoseRefMeters) since the
            // group's last copy + refit, and no more than RenderVKRigRefitPoseMaxStale
            // frames ago. Like every skip here it keeps BOTH the BLAS and the group's rig_pos
            // range (no copy, no refit): hit shading reads the pose the BLAS holds.
            bool pose_skip = false;
            if (!held_skip && !far_skip && pose_ok && mFrame - rb.lastRefit <= pose_max_stale)
            {
                const float thr = pose_thr * std::max(1.f, av_dist / kPoseRefMeters);
                pose_skip = poseDrift(av, rb.lastRefit) < thr;
            }
            const bool skip = held_skip || far_skip || pose_skip;
            if (skip)
            {
                if (!still) rb.stale = true;   // moved: the next refit must not be skipped as "still"
                if (!held_skip)
                {
                    if (far_skip) ++mRigSkipCadence; else ++mRigSkipPose;
                    mRigMaxStale = std::max(mRigMaxStale, mFrame - rb.lastRefit);
                }
                ++mRigRefitSkipped;
                rb.lastSeen = mFrame;
                mRigEntries[g0].blas_lo = (uint32_t)(rb.addr & 0xFFFFFFFFull);
                mRigEntries[g0].blas_hi = (uint32_t)(rb.addr >> 32);
                g0 = g1;
                continue;
            }
        }
        // REFIT SCRATCH BUDGET (2026-10-13): every refit of a frame needs its own update scratch
        // (one build call); a crowd's ~100 groups refit together sized the scratch to ~1.5 GB of
        // "working buffers". Past kRigRefitScratch a refit waits a frame like a far-avatar skip
        // (its BLAS and rig_pos keep the previous pose, consistently) -- at most kRigRefitLag
        // frames: then it runs whatever the budget (near groups come first, so a far one lags).
        if (same && mScratchUsed + rb.updateScratch > kRigRefitScratch && rb.lastRefit != 0
            && mFrame - rb.lastRefit < kRigRefitLag && geoms_ok)
        {
            rb.stale = true;
            ++mRigRefitDeferred;
            rb.lastSeen = mFrame;
            mRigEntries[g0].blas_lo = (uint32_t)(rb.addr & 0xFFFFFFFFull);
            mRigEntries[g0].blas_hi = (uint32_t)(rb.addr >> 32);
            g0 = g1;
            continue;
        }
        if (same)
        {
            b.update = true;
            b.scratch = rb.updateScratch;
            mScratchUsed += rb.updateScratch;
        }
        else
        {
            if (reuse_plan) buildSig();   // (lazy: only a (re)build needs sig + prims)
            VkAccelerationStructureBuildGeometryInfoKHR gi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
            gi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            gi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
            gi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            gi.geometryCount = n; gi.pGeometries = &mRigGeoms[g0];
            VkAccelerationStructureBuildSizesInfoKHR sz{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
            rt.vkGetAccelerationStructureBuildSizesKHR(dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &gi,
                                                       prims.data(), &sz);
            // #692: why this group would go without a BLAS this frame (none = build it).
            const auto wit = mRigWait.empty() ? mRigWait.end() : mRigWait.find(key);
            const RigWait* wt = wit != mRigWait.end() ? &wit->second : nullptr;
            RigOut why = RIG_OUT_NONE;
            bool alloc_failed = false;
            // Over this frame's scratch budget: wait a frame -- but not forever. A group
            // deferred kRigStarveFrames in a row (behind the statics and nearer groups'
            // refits, every frame: a big avatar group used to lose its BLAS at a periodic
            // rebuild and never get it back) builds over the budget; the scratch buffer
            // grows for it and shrinks again later (shrinkIfOversized).
            const uint32_t waited = geom_same ? rb.qualityDefers
                                  : (wt && wt->reason == RIG_OUT_SCRATCH ? wt->frames : 0u);
            // (2026-10-13: ONE starved build per frame goes over the budget -- every group of a
            // re-admitted crowd starved together and then built in the same frame: ~1.5 GB of
            // scratch, which itself tripped the over-budget drop again.)
            if (mScratchUsed > 0 && mScratchUsed + sz.buildScratchSize > kScratchBudget
                && (waited < kRigStarveFrames || mRigStarvedBuilt > 0))
                why = RIG_OUT_SCRATCH;
            // #697: a failed allocation is retried after a backoff, not every frame.
            else if (!geom_same && wt && wt->reason == RIG_OUT_ALLOC && mFrame < wt->retryAt)
                why = RIG_OUT_ALLOC;
            else if (mScratchUsed > 0 && mScratchUsed + sz.buildScratchSize > kScratchBudget)
                ++mRigStarvedBuilt;   // (the frame's one starved build over the budget)
            Alloc a;
            VkDeviceAddress addr = 0;
            VkAccelerationStructureKHR as = VK_NULL_HANDLE;
            if (why == RIG_OUT_NONE)
            {
                if (sz.accelerationStructureSize && arenaAlloc(sz.accelerationStructureSize, a))
                {
                    as = createAS(a, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, addr, "rig", mRigEntries[g0].inst);
                    if (!as) arenaFree(a);
                }
                if (!as) { why = RIG_OUT_ALLOC; alloc_failed = true; }
            }
            if (why != RIG_OUT_NONE)
            {
                if (geom_same)
                {
                    // Only the periodic quality rebuild is due and it cannot run now: the
                    // current BLAS still matches the geometries -- keep it and refit
                    // (it used to be retired here: the group left RT until a later frame
                    // found scratch for a full build). Scratch: retried next frame (over
                    // the budget after kRigStarveFrames); allocation: kRigAllocRetry later.
                    if (why == RIG_OUT_SCRATCH) ++rb.qualityDefers;
                    if (alloc_failed) rb.builtFrame = mFrame + kRigAllocRetry - kRigRebuildFrames;
                    b.update = true;
                    b.scratch = rb.updateScratch;
                    mScratchUsed += rb.updateScratch;
                }
                else
                {
                    // The old BLAS (if any) no longer matches the geometries: the group sits
                    // out of RT until a later frame builds it.
                    uint32_t tris = 0;
                    for (const uint32_t p : prims) tris += p;
                    if (rb.as) { retireAS(rb.as, rb.mem); mRigBytes -= std::min<uint64_t>(mRigBytes, rb.mem.size); }
                    mRig.erase(key);
                    rigNoteOut(key, why, n, tris, alloc_failed);
                    g0 = g1;
                    continue;
                }
            }
            else
            {
                mScratchUsed += sz.buildScratchSize;
                if (rb.as) { retireAS(rb.as, rb.mem); mRigBytes -= std::min<uint64_t>(mRigBytes, rb.mem.size); }
                rb = RigBlas{};
                rb.as = as; rb.mem = a; rb.addr = addr;
                rb.sig = sig;
                // Stagger the periodic quality rebuild (kRigRebuildFrames) by 0..31 frames per
                // group, so groups built together (arrival, teleport) don't all rebuild on
                // the same later frame -- a periodic GPU spike.
                const uint32_t jitter = (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 59);
                rb.builtFrame = mFrame > jitter ? mFrame - jitter : mFrame;
                rb.updateScratch = sz.updateScratchSize;
                mRigBytes += a.size;
                b.scratch = sz.buildScratchSize;
            }
        }
        if (!mRigWait.empty()) rigNoteIn(key);   // #692: in the TLAS this frame
        rb.lastSeen = mFrame;
        rb.lastRefit = mFrame; rb.stale = false;   // copied + refit / built this frame
        // Only the group's first entry carries the BLAS: it writes the group's record.
        mRigEntries[g0].blas_lo = (uint32_t)(rb.addr & 0xFFFFFFFFull);
        mRigEntries[g0].blas_hi = (uint32_t)(rb.addr >> 32);
        rbuilds.push_back(b);
        g0 = g1;
    }
    // #747 / #748: per rig entry, its group's head + 1 when the group is cluster-switched (pass B)
    mRigClHead.assign(mRigEntries.size(), 0u);
    bool clsw_any = false;
    for (size_t g0 = 0; g0 < cands.size() && cands.size() == mRigEntries.size();)
    {
        size_t g1 = g0 + 1;
        while (g1 < cands.size() && cands[g1].key == cands[g0].key) ++g1;
        if (cands[g0].key & 1u)
        {
            for (size_t i = g0; i < g1; ++i) mRigClHead[i] = (uint32_t)g0 + 1u;
            clsw_any = true;
        }
        g0 = g1;
    }
    // Legacy-skin skip: this frame's switched groups by owner avatar (harvestClusterCoverage reads
    // mode 4's flags of them when this frame slot comes round; valid once mode 4 is recorded).
    mClCov[mSlotIdx].clear();
    mClCovEntries[mSlotIdx] = (uint32_t)mRigEntries.size();
    for (size_t g0 = 0; g0 < cands.size() && cands.size() == mRigEntries.size();)
    {
        size_t g1 = g0 + 1;
        while (g1 < cands.size() && cands[g1].key == cands[g0].key) ++g1;
        const uint64_t owner = cands[g0].key >> 8;
        if ((cands[g0].key & 1u) && owner < kAvVisSlots) mClCov[mSlotIdx].push_back({(uint32_t)g0, (uint32_t)owner});
        g0 = g1;
    }
    // ... and the avatars that MUST be among them (consistency check, harvested with them): the
    // cluster path's avatars the RT rule traces; the rest counted by reason.
    {
        ClExpect& ex = mClExpect[mSlotIdx];
        ex = ClExpect{};
        if (clusterTracesSwitched())
        {
            uint32_t owners[kAvVisSlots / 32] = {};
            for (const ClCovGroup& g : mClCov[mSlotIdx]) owners[g.av >> 5] |= 1u << (g.av & 31u);
            for (uint32_t a = 0; a < kAvVisSlots; ++a)
            {
                const uint32_t m = 1u << (a & 31u);
                const bool own = (owners[a >> 5] & m) != 0u;
                if (!(cl_av_bits[a >> 5] & m))
                {
                    if (own) ++ex.extra;
                    continue;
                }
                ++ex.cl_av;
                if (R.mAvAnimesh[a]) ++ex.am_cl_av;
                if (rtAvUnseen(a)) { ++ex.unseen; if (own) ++ex.extra; continue; }
                if (rtAvNoShadow(a)) { ++ex.no_shadow; if (own) ++ex.extra; continue; }
                if (!rig_ok) { ++ex.plan_off; continue; }
                ex.exp[a >> 5] |= m;
                if (own) continue;
                ++ex.not_plan;
                const uint8_t why = mRigPlanAvWhy[a];
                ++((why & 1u) ? ex.np_skin : (why & 2u) ? ex.np_row : ex.np_other);
            }
        }
    }
    phase("vk_rtP_rigPlan");
    // Retire rigged BLAS whose instance hasn't been seen for a while (the age is 120
    // frames: sweeping every 16th frame is plenty and keeps the map walk off most frames).
    if ((mFrame & 15u) == 0)
    for (auto it = mRig.begin(); it != mRig.end();)
    {
        if (it->second.lastSeen + kRigKeepFrames < mFrame)
        {
            retireAS(it->second.as, it->second.mem);
            mRigBytes -= std::min<uint64_t>(mRigBytes, it->second.mem.size);
            it = mRig.erase(it);
        }
        else ++it;
    }
    // #692: exclusions no longer noted (the group / owner left the plan, or its faces passed
    // the slot check again).
    if ((mFrame & 15u) == 0)
    for (auto it = mRigWait.begin(); it != mRigWait.end();)
    {
        if (it->second.lastFrame + kRigKeepFrames < mFrame)
        {
            if (it->second.logged)
                LL_INFOS("VKRT") << "RT: " << rigOwnerText(it->first) << " -- no longer left out of ray-traced shadows ("
                                 << (it->second.reason == RIG_OUT_SLOT ? "its faces pass the slot check again, or are gone"
                                                                       : "the group left the RT plan")
                                 << ")" << LL_ENDL;
            it = mRigWait.erase(it);
        }
        else ++it;
    }

    // Any return from here on, before the builds are recorded, must UNWIND this frame's
    // plan: the BLAS created above (static builds, compaction targets, fresh rig builds)
    // have published addresses (slot_addr / rig entries) but would never be built or
    // copied -- a later TLAS would reference uninitialized AS memory, and a rig entry
    // would REFIT (MODE_UPDATE) an AS that was never built.
    bool dummy_build = false;   // the TLAS-refit placeholder BLAS is created this frame
    auto abortFrame = [&]()
    {
        if (mClusterRT) mClusterRT->abortPlan();   // TODO(2b) #748: this frame's plan is never recorded
        if (dummy_build)
        {
            retireAS(mDummyBlas, mDummyMem);
            mDummyBlas = VK_NULL_HANDLE; mDummyMem = Alloc{}; mDummyAddr = 0; mDummyBuiltFrame = 0;
        }
        for (const StaticBuild& b : sbuilds) dropSlotBlas(b.slot);
        for (const CompactOp& c : compactOps) dropSlotBlas(c.slot);
        // Smooth terrain: fresh morph BLAS were never built (drop them: the address is
        // published); a skipped refit just refits next frame.
        for (const TerrBuild& b : mTerrBuilds)
        {
            if (b.update)
            {
                auto it = mTerrMorph.find(b.slot);
                if (it != mTerrMorph.end()) it->second.cam[0] = 1e30f;
                continue;
            }
            dropTerrMorph(b.slot);
        }
        mTerrEntries.clear(); mTerrBuilds.clear();
        for (const RigBuild& b : rbuilds)
        {
            if (b.update)
            {
                // Built in an earlier frame: skipping one refit is fine, but the gate must
                // not treat it as refit this frame.
                auto it = mRig.find(b.key);
                if (it != mRig.end()) it->second.lastRefit = 0;
                continue;
            }
            auto it = mRig.find(b.key);
            if (it == mRig.end()) continue;
            retireAS(it->second.as, it->second.mem);
            mRigBytes -= std::min<uint64_t>(mRigBytes, it->second.mem.size);
            mRig.erase(it);
        }
        ommAbortPlan();
        LL_WARNS_ONCE("VKRT") << "RT: frame skipped after planning (allocation failure); unwound "
                              << sbuilds.size() << " builds, " << compactOps.size() << " compactions" << LL_ENDL;
    };

    // ---- TLAS sizing (#161: from the live instance count, +25%; was the renderer's
    //      instance-buffer capacity -- 2x need -- +25%). Rebuilt on growth, and when over
    //      2x the need (a sim emptied / teleport). ----
    const uint32_t cap = std::max<uint32_t>(inst_n, 4096u);
    if (!mTlas || mTlasCap < cap || mTlasCap > cap * 2 + 16384)
    {
        if (mTlas) { mPend.as.push_back(mTlas); mTlas = VK_NULL_HANDLE; }
        mTlasHasBuild = false;   // a fresh TLAS: the next build must be a BUILD
        retireBuf(mTlasBuf);
        const uint32_t ncap = cap + cap / 4;
        VkAccelerationStructureGeometryKHR g{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        g.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        g.geometry.instances = VkAccelerationStructureGeometryInstancesDataKHR{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
        VkAccelerationStructureBuildGeometryInfoKHR gi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        gi.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        gi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        gi.geometryCount = 1; gi.pGeometries = &g;
        // Sized for BOTH flag sets (RenderVKTLASRefit may toggle without a recreate) and
        // for the update scratch (VUID-vkCmdBuildAccelerationStructuresKHR-pInfos-12259).
        VkAccelerationStructureBuildSizesInfoKHR sz{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        gi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        rt.vkGetAccelerationStructureBuildSizesKHR(dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &gi, &ncap, &sz);
        {
            VkAccelerationStructureBuildSizesInfoKHR su{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
            gi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
            rt.vkGetAccelerationStructureBuildSizesKHR(dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &gi, &ncap, &su);
            sz.accelerationStructureSize = std::max(sz.accelerationStructureSize, su.accelerationStructureSize);
            sz.buildScratchSize = std::max({sz.buildScratchSize, su.buildScratchSize, su.updateScratchSize});
        }
        mTlasBuf = mCtx->createBuffer(sz.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,
                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true);
        if (!mTlasBuf.buffer) { abortFrame(); return; }
        VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
        ci.buffer = mTlasBuf.buffer; ci.size = sz.accelerationStructureSize;
        ci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        if (rt.vkCreateAccelerationStructureKHR(dev, &ci, nullptr, &mTlas) != VK_SUCCESS) { mTlas = VK_NULL_HANDLE; abortFrame(); return; }
        vkAddrName(mTlasBuf.buffer, "rt.tlasBuf");
        {
            VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
            ai.accelerationStructure = mTlas;
            vkAddrNote(rt.vkGetAccelerationStructureDeviceAddressKHR(dev, &ai), sz.accelerationStructureSize, "tlas", ncap);
        }
        mTlasCap = ncap;
        mTlasScratch = sz.buildScratchSize;
    }

    // ---- TLAS-refit placeholder BLAS (once): one degenerate triangle (zero-filled
    //      vertices, non-indexed). Records leaving the TLAS on a refit frame point at it
    //      with mask 0 (never hit) so they stay ACTIVE (VUID-...-pInfos-03664). ----
    VkAccelerationStructureGeometryKHR dummy_geom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    VkAccelerationStructureBuildRangeInfoKHR dummy_range{1, 0, 0, 0};
    VkDeviceSize dummy_scratch = 0;
    if (tlas_refit && !mDummyBlas
        && ensureBuf(mDummyVerts, 256, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR
                                       | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false, true))
    {
        dummy_geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        dummy_geom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        auto& t = dummy_geom.geometry.triangles;
        t = VkAccelerationStructureGeometryTrianglesDataKHR{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
        t.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        t.vertexData.deviceAddress = mDummyVerts.address;
        t.vertexStride = 12;
        t.maxVertex = 2;
        t.indexType = VK_INDEX_TYPE_NONE_KHR;
        VkAccelerationStructureBuildGeometryInfoKHR gi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        gi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        gi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        gi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        gi.geometryCount = 1; gi.pGeometries = &dummy_geom;
        VkAccelerationStructureBuildSizesInfoKHR sz{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        rt.vkGetAccelerationStructureBuildSizesKHR(dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &gi,
                                                   &dummy_range.primitiveCount, &sz);
        Alloc a;
        if (sz.accelerationStructureSize && arenaAlloc(sz.accelerationStructureSize, a))
        {
            VkDeviceAddress addr = 0;
            VkAccelerationStructureKHR as = createAS(a, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, addr, "blas-dummy", 0);
            if (as)
            {
                mDummyBlas = as; mDummyMem = a; mDummyAddr = addr; mDummyBuiltFrame = 0;
                dummy_build = true;
                dummy_scratch = sz.buildScratchSize;
            }
            else arenaFree(a);
        }
    }

    // ---- opacity micromaps: table + this frame's micromap builds (#194) ----
    const VkDeviceSize omm_scratch = ommPlanFrame(cull.lod_cam_pos, radius,
                                                  std::max<VkDeviceSize>(mCtx->asScratchAlignment(), 16), budget);

    // ---- TODO(2b) cluster LOD RT (#748): the RT cut, CLAS / per-cut BLAS plan (+ the PTLAS) ----
    VkDeviceSize cl_scratch = 0;
    if (mClusterRT)
    {
        const VkDeviceSize cl_salign = std::max<VkDeviceSize>(mCtx->asScratchAlignment(), 16);
        LLCleanClusterRTVK::FrameIn ci;
        ci.cluster = R.mCluster;
        ci.clusterOn = R.clusterLodOn();
        ci.frameSlot = mSlotIdx;
        ci.cam[0] = cull.cam_pos[0]; ci.cam[1] = cull.cam_pos[1]; ci.cam[2] = cull.cam_pos[2];
        ci.znear = R.mCamNear;                // the camera's near clip (#749, = clusterCullMain's)
        ci.px_scale = cull.lod_cam_pos[3];    // 0.5 * height * |P[1][1]| (as the main cluster view)
        ci.riCap = mTlasCap;
        ci.rtBudget = total_budget;
        ci.legacyRigged = rig_ok;   // (pass B -- the rigged override rows' only reader -- walks the rig entries;
                                    //  legacy class dropped: the switched groups alone, cluster-only)
#if defined(LL_HAS_CLEAN_PIPELINE)
        // #748 perf pass 2, PERSISTENT RIGGED CLAS: the avatars whose skinned WORLD positions are
        // provably those of the previous viewer frame (the refit gate's proof: GPU pose held, root
        // still, skin meta clean; RT ran last viewer frame, the anim store built this one) -- their
        // rigged CLAS are kept when their cut did not change either.
        ci.rigStillValid = gframe_seq && anim_now;
        uint32_t cl_throttled = 0;
        if (ci.rigStillValid)
            for (uint32_t a = 0; a < std::min<uint32_t>(jt_slots, kAvatarSlotCap); ++a)
            {
                if (R.mRigCacheSlotDirty[a]) continue;   // (skin meta changed: always rebuilt)
                bool still = anim.slotWorldClean(a);
                // UPDATE RATE (fixed-cost audit 2026-10-14): the legacy refit gate's rule 2 (B8, above:
                // a far avatar every 2nd frame, a small one on screen every 2nd .. 4th by its height in
                // px, never our own / the protected set, staggered by key, at most period - 1 skipped
                // frames in a row) applied to the rigged CLAS: on its off frames a throttled avatar is
                // KEPT (no skin copy, no CLAS build; the cluster RT traces its last build), exactly as
                // the legacy path traced its last refit. Its cut changing still rebuilds it (the keep
                // also needs the signature). Was: every moving switched avatar rebuilt every frame.
                // (only an avatar classifyNoSelf tagged this frame: it never self-shadows in RT)
                if (!still && rig_gate >= 2 && !R.mRigSelfSlot[a] && !R.mRigProtect[a]
                    && (R.mAvVis[a] & LLCleanRendererVK::kAvVisRtNoSelf))
                {
                    uint32_t period = 1;
                    if (rig_far > 0.f)
                    {
                        float d = 0.f;
                        std::memcpy(&d, &R.mAvVis[LLCleanRendererVK::kAvDemBase + 2 * a], 4);
                        if (std::isfinite(d) && d > rig_far) period = 2;
                    }
                    const float px = R.mRigAvPx[a];
                    if (rig_full_px > 0.f && px > 0.f && std::isfinite(px))
                    {
                        const uint32_t pp = px >= rig_full_px ? 1u : px >= rig_full_px * 0.5f ? 2u
                                          : px >= rig_full_px * 0.25f ? 3u : 4u;
                        period = std::max(period, std::min(pp, rig_max_period));
                    }
                    const uint32_t phase = (uint32_t)(((uint64_t)(a + 1u) * 0x9E3779B97F4A7C15ull) >> 32);
                    if (period > 1u && ((mFrame + phase) % period) != 0u) { still = true; ++cl_throttled; }
                }
                if (still) ci.rigStill[a >> 5] |= 1u << (a & 31u);
            }
        LLVKHitch::count("rt_clRigThrottled", cl_throttled);
#endif
        // RT AVATAR SET: RT data only for the avatars the plan traces (the walk's avatar rule)
        for (uint32_t a = 0; a < kAvVisSlots; ++a)
        {
            if (rtAvUnseen(a) || rtAvNoShadow(a)) ci.rigTraced[a >> 5] &= ~(1u << (a & 31u));
            // #1008: the avatars that never self-shadow in RT this frame (classifyNoSelf): the only
            // ones whose cluster RT cut / CLAS may go coarse or stale
            if (R.mAvVis[a] & LLCleanRendererVK::kAvVisRtNoSelf) ci.rigNoSelf[a >> 5] |= 1u << (a & 31u);
        }
        cl_scratch = mClusterRT->plan(ci, cl_salign);
        // #747 / #748: the cluster-switched groups for pass B (null: every group legacy)
        if (clsw_any && rig_ok)
            if (uint32_t* hm = mClusterRT->tlasRigMap((uint32_t)mRigClHead.size()))
            {
                std::memcpy(hm, mRigClHead.data(), mRigClHead.size() * 4);
                // Words 8..15 (clean_rt_instances.comp CLRIG_AV_TRACED): the legacy
                // avatar class dropped (pass A's joint_count 0 drops every worn rigid attachment): the avatars
                // whose switched groups are in this plan keep their worn attachments in RT: they
                // trace their body from the cluster cut.
                if (rig_sw_only_plan)
                {
                    uint32_t* av_words = hm - LLCleanClusterRTVK::kTlasRigHdr + 8;
                    for (size_t g = 0; g < cands.size() && cands.size() == mRigEntries.size(); ++g)
                    {
                        const uint64_t ow = cands[g].key >> 8;
                        if ((cands[g].key & 1u) && ow < kAvVisSlots) av_words[ow >> 5] |= 1u << (ow & 31u);
                    }
                }
            }
        if (mClusterRT->ptlasLayout())
        {
            if (!mClusterRT->ptlasEnsure(mTlasCap)) { abortFrame(); return; }   // consumers fall back
            cl_scratch += alignUp(mClusterRT->ptlasScratch() + 256, cl_salign);
        }
    }

    // ---- buffers ----
    const uint32_t used_words = (slot_n + 31) / 32;
    const VkBufferUsageFlags kSt = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    shrinkIfOversized(mTlasInst, (VkDeviceSize)mTlasCap * 64, mTablesOverFrames);
    if (!mTlasInst.buffer) { retireBuf(mRtGeom); retireBuf(mRtGeomVc); }   // shrink together
    bool ok = ensureBuf(mTlasInst, (VkDeviceSize)mTlasCap * 64,
                        kSt | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR
                        | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false, true);   // (+ SRC: llcleanrtaudit copies it)
    ok = ok && ensureBuf(mRtGeom, (VkDeviceSize)mTlasCap * 16, kSt | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);   // (+ SRC: audit)
    // #874 (bound: the TLAS capacity) -- 4 words per instance (#1009: + a cluster-traced static's legacy row)
    ok = ok && ensureBuf(mRtGeomVc, (VkDeviceSize)mTlasCap * 16, kSt);
    {
        const VkBuffer old = mSlotAddr.buffer;
        ok = ok && ensureBuf(mSlotAddr, (VkDeviceSize)std::max<uint32_t>(slot_n, 1) * 8, kSt | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        if (mSlotAddr.buffer != old && slot_n) { mAddrDirtyLo = 0; mAddrDirtyHi = slot_n; }   // fresh buffer: all
        mAddrDirtyHi = std::min(mAddrDirtyHi, slot_n);
    }
    // + the flicker-diagnostic tail (8 KB; filled / read back only while it's on).
    const bool dbg = rtDebugFlicker() && R.mProjFocus.valid;
    const uint32_t rb_words = used_words + kCounterWords + (dbg ? kDbgTailWords : 0);
    ok = ok && ensureBuf(mUsedBits, (VkDeviceSize)(used_words + kCounterWords + kDbgTailWords) * 4,
                         kSt | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    ok = ok && ensureBuf(mRigEntry, (VkDeviceSize)std::max<size_t>(mRigEntries.size(), 1) * sizeof(RigEntryCPU),
                         kSt | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    ok = ok && ensureBuf(mRigValid, (VkDeviceSize)std::max<size_t>(mRigEntries.size(), 1) * 4, kSt);
    if (!mRigPos.buffer) ok = ok && ensureBuf(mRigPos, 256, kSt | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, false, true);
    // Smooth terrain morph copy: entries (staged) + positions (bound even when unused).
    ok = ok && ensureBuf(mTerrEntry, (VkDeviceSize)std::max<size_t>(mTerrEntries.size(), 1) * sizeof(TerrEntryCPU),
                         kSt | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    if (!mTerrPos.buffer) ok = ok && ensureBuf(mTerrPos, 256, kSt | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, false, true);
    // Staging (this frame slot; its previous user is done): address range + rig entries.
    const bool addr_dirty = mAddrDirtyLo < mAddrDirtyHi;
    const VkDeviceSize addr_bytes = addr_dirty ? (VkDeviceSize)(mAddrDirtyHi - mAddrDirtyLo) * 8 : 0;
    const VkDeviceSize rig_bytes = (VkDeviceSize)mRigEntries.size() * sizeof(RigEntryCPU);
    const VkDeviceSize terr_bytes = (VkDeviceSize)mTerrEntries.size() * sizeof(TerrEntryCPU);
    ok = ok && ensureBuf(mStage[mSlotIdx], addr_bytes + rig_bytes + terr_bytes + 16, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    // Static BLAS decode transforms (#573): one VkTransformMatrixKHR (48 B, 16-aligned) per build.
    if (!sbuilds.empty())
        ok = ok && ensureBuf(mXform[mSlotIdx], (VkDeviceSize)sbuilds.size() * sizeof(VkTransformMatrixKHR),
                             VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true, true);
    ok = ok && ensureBuf(mUsedRb[mSlotIdx], (VkDeviceSize)(used_words + kCounterWords + kDbgTailWords) * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
    // #1007 pose motion (mode 5): the joint snapshot (every joint-table slot the table holds)
    // + this slot's readback, while the pose skip is on and there are rigged groups. A soft
    // failure only turns the pose skip off this frame (mode 5 is not recorded).
    uint32_t pose_slots = 0;
    bool pose_fresh = false;
    if (ok && pose_thr > 0.f && rig_gate >= 1 && !mRigEntries.empty())
    {
        constexpr VkDeviceSize kJointRowBytes = 216u * 48u;   // = AvatarJoints (clean_joint_table.glsl)
        const uint32_t jt_n = (uint32_t)std::min<VkDeviceSize>(R.jointTable().size / kJointRowBytes, kAvVisSlots);
        const uint32_t n = std::min<uint32_t>(jt_n, R.mAvatarJointCount);
        const VkBuffer snap_was = mPoseSnap.buffer;
        if (n && R.jointTable().buffer
            && ensureBuf(mPoseSnap, (VkDeviceSize)jt_n * kJointRowBytes, kSt)
            && ensureBuf(mPoseRb[mSlotIdx], (VkDeviceSize)jt_n * 8, kSt | VK_BUFFER_USAGE_TRANSFER_DST_BIT, true))
        {
            pose_slots = n;
            // (a new snapshot's rows are not a previous pose yet: this frame's readings are
            // dropped, its rows are written by this frame's dispatch)
            if (mPoseSnap.buffer != snap_was) mPoseSnapReady = false;
            pose_fresh = !mPoseSnapReady;
        }
    }
    // Scratch plan: bump-allocated, aligned in DEVICE ADDRESS space.
    const VkDeviceSize salign = std::max<VkDeviceSize>(mCtx->asScratchAlignment(), 16);
    VkDeviceSize scratch_need = 0;
    for (const StaticBuild& b : sbuilds) scratch_need += alignUp(b.scratch, salign);
    for (const RigBuild& b : rbuilds)    scratch_need += alignUp(b.scratch, salign);
    for (const TerrBuild& b : mTerrBuilds) scratch_need += alignUp(b.scratch, salign);
    scratch_need += alignUp(mTlasScratch, salign) + salign;
    scratch_need += alignUp(dummy_scratch, salign);
    scratch_need += omm_scratch;
    scratch_need += cl_scratch;   // TODO(2b) #748
    // Scratch shrinks to the largest need of the last ~300-600 frames once it holds over 2x
    // that (the generic rule waited for 4x of ONE frame's need -- per-frame needs swing with
    // that frame's builds), and grows by 1/8 instead of the generic 1/2.
    mScratchWinMax = std::max(mScratchWinMax, scratch_need);
    if (++mScratchWinFrames >= 300) { mScratchWinPrev = mScratchWinMax; mScratchWinMax = 0; mScratchWinFrames = 0; }
    VkDeviceSize scratch_alloc = scratch_need;
    {
        const VkDeviceSize peak = std::max(mScratchWinMax, mScratchWinPrev);
        if (mScratch.buffer && mScratch.size > (32ull << 20) && mScratch.size > peak * 2 && mScratchWinPrev != 0)
        {
            retireBuf(mScratch);
            scratch_alloc = peak;   // (re-created at the window's peak: no regrow churn)
        }
    }
    ok = ok && ensureBuf(mScratch, scratch_alloc, kSt, false, true);
    if (!ok || !mTlas)
    {
        // Out of memory: skip RT this frame -- and unwind this frame's plan (see abortFrame).
        LLVKHitch::count("rt_skipOOM", 1);
        abortFrame();
        return;
    }
    updateSets(mSlotIdx);
    if (mClusterRT) mClusterRT->writeSets(mTlasInst);   // TODO(2b) #748 (once per frame, before any bind)
    const VkDeviceAddress sbase = alignUp(mScratch.address, salign);
    VkDeviceAddress scur = sbase;
    auto takeScratch = [&](VkDeviceSize n) { VkDeviceAddress a = scur; scur += alignUp(n, salign); return a; };

    // ---- WAR / WAW with the previous frame (same queue) + this frame's geometry
    // stream copies -> BLAS build input reads ----
    memBarrier(cmd, kFS | kCS | kAS | kXfer, kASW | kSW | VK_ACCESS_2_TRANSFER_WRITE_BIT,
               kAS | kCS | kXfer, kASR | kASW | kSR | kSW | VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);

    // ---- uploads ----
    {
        uint8_t* st = static_cast<uint8_t*>(mStage[mSlotIdx].mapped);
        std::vector<VkBufferCopy> addrCopy;
        if (addr_dirty && st)
        {
            std::memcpy(st, mSlotAddrCpu.data() + (size_t)mAddrDirtyLo * 2, addr_bytes);
            VkBufferCopy c{0, (VkDeviceSize)mAddrDirtyLo * 8, addr_bytes};
            vkCmdCopyBuffer(cmd, mStage[mSlotIdx].buffer, mSlotAddr.buffer, 1, &c);
            mAddrDirtyLo = ~0u; mAddrDirtyHi = 0;
        }
        if (rig_bytes && st)
        {
            std::memcpy(st + addr_bytes, mRigEntries.data(), rig_bytes);
            VkBufferCopy c{addr_bytes, 0, rig_bytes};
            vkCmdCopyBuffer(cmd, mStage[mSlotIdx].buffer, mRigEntry.buffer, 1, &c);
        }
        if (terr_bytes && st)
        {
            std::memcpy(st + addr_bytes + rig_bytes, mTerrEntries.data(), terr_bytes);
            VkBufferCopy c{addr_bytes + rig_bytes, 0, terr_bytes};
            vkCmdCopyBuffer(cmd, mStage[mSlotIdx].buffer, mTerrEntry.buffer, 1, &c);
        }
        vkCmdFillBuffer(cmd, mUsedBits.buffer, 0, (VkDeviceSize)rb_words * 4, 0);
        if (dummy_build) vkCmdFillBuffer(cmd, mDummyVerts.buffer, 0, VK_WHOLE_SIZE, 0);
        arenaGuardFlushFills(cmd);   // #973 (before every build of the frame)
        ommRecordUploads(cmd);
        memBarrier(cmd, kXfer, VK_ACCESS_2_TRANSFER_WRITE_BIT, kCS | kAS, kSR | kSW | kASR);
    }

    // ---- static BLAS builds + compaction copies ----
    fullB(cmd);   // (#889 A/B)
    if (!sbuilds.empty() || !compactOps.empty() || dummy_build)
    {
        std::vector<VkAccelerationStructureBuildGeometryInfoKHR> infos;
        std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> ranges;
        std::vector<VkAccelerationStructureKHR> built;
        infos.reserve(sbuilds.size() + 1); ranges.reserve(sbuilds.size() + 1);
        if (dummy_build)
        {
            VkAccelerationStructureBuildGeometryInfoKHR gi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
            gi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            gi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
            gi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            gi.dstAccelerationStructure = mDummyBlas;
            gi.geometryCount = 1; gi.pGeometries = &dummy_geom;
            gi.scratchData.deviceAddress = takeScratch(dummy_scratch);
            infos.push_back(gi);
            ranges.push_back(&dummy_range);
            mDummyBuiltFrame = mFrame;   // referenced from the next frame on
        }
        uint8_t* xmap = static_cast<uint8_t*>(mXform[mSlotIdx].mapped);
        size_t xi = 0;
        for (StaticBuild& b : sbuilds)
        {
            // The slot's decode transform (#573; host-coherent, visible at submit).
            if (xmap)
            {
                std::memcpy(xmap + xi * sizeof(VkTransformMatrixKHR), b.xf, sizeof(VkTransformMatrixKHR));
                b.geom.geometry.triangles.transformData.deviceAddress =
                    mXform[mSlotIdx].address + (VkDeviceAddress)(xi * sizeof(VkTransformMatrixKHR));
            }
            ++xi;
            VkAccelerationStructureBuildGeometryInfoKHR gi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
            gi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            gi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
            gi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            gi.dstAccelerationStructure = mSlot[b.slot].as;
            gi.geometryCount = 1; gi.pGeometries = &b.geom;
            gi.scratchData.deviceAddress = takeScratch(b.scratch);
            infos.push_back(gi);
            ranges.push_back(&b.range);
            built.push_back(mSlot[b.slot].as);
        }
        if (!infos.empty())
            rt.vkCmdBuildAccelerationStructuresKHR(cmd, (uint32_t)infos.size(), infos.data(), ranges.data());
        for (const CompactOp& c : compactOps)
        {
            VkCopyAccelerationStructureInfoKHR ci{VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR};
            ci.src = c.src; ci.dst = c.dst; ci.mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR;
            rt.vkCmdCopyAccelerationStructureKHR(cmd, &ci);
        }
        if (!built.empty() && !no_compact)   // (#973 RenderVKRTNoCompaction: no queries either)
        {
            // Compacted-size queries (read on this frame slot's next turn).
            memBarrier(cmd, kAS, kASW, kAS, kASR);
            const uint32_t q0 = mSlotIdx * kMaxBuildsPerFrame;
            vkCmdResetQueryPool(cmd, mQueryPool, q0, (uint32_t)built.size());
            rt.vkCmdWriteAccelerationStructuresPropertiesKHR(cmd, (uint32_t)built.size(), built.data(),
                VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, mQueryPool, q0);
            for (size_t i = 0; i < sbuilds.size(); ++i)
                mCompact[mSlotIdx].push_back({sbuilds[i].slot, mSlot[sbuilds[i].slot].serial, q0 + (uint32_t)i});
        }
    }
    fullB(cmd);   // (#889 A/B: the micromap BLAS builds)
    ommRecordBuilds(cmd, takeScratch);
    R.gpuSubMark(cmd, "c_blas_build");
    // TODO(2b) cluster LOD RT (#748): RT cut + CLAS / cluster BLAS (or per-cut BLAS) builds +
    // the override rows pass A reads (binding 22). GPU time: c_cl_rt_select / c_cl_clas_build /
    // c_cl_blas_build (tiers 1/2) or c_cl_t0_blas (tier 0) -- vs the legacy c_blas_build /
    // c_blas_refit above (RenderVKSectionLog).
    if (mClusterRT) fullB(cmd);   // (#889 A/B: the cluster CLAS / BLAS builds)
    if (mClusterRT)
        mClusterRT->record(cmd, takeScratch, [&R](VkCommandBuffer c, const char* n) { R.gpuSubMark(c, n); });

    // ---- TLAS pass A (all instances) ----
    RtCompPush pp{};
    std::memcpy(pp.lod_cam, cull.lod_cam_pos, sizeof(float) * 3);
    pp.lod_cam[3] = cull.cam_pos[3];
    std::memcpy(pp.cam_time, draw.cam_time, sizeof(pp.cam_time));
    pp.eye[0] = R.mPalEye[0]; pp.eye[1] = R.mPalEye[1]; pp.eye[2] = R.mPalEye[2];
    pp.eye[3] = std::max(radius, 0.f);
    pp.instance_count = std::min(inst_n, mTlasCap);
    pp.slot_count = slot_n;
    pp.rig_count = (uint32_t)mRigEntries.size();
    pp.idx_valid = R.mIdxValid;
    // joint_count gates worn rigid attachments in pass A (slot >= joint_count ->
    // inactive). With rigged meshes dropped for the budget, drop the worn attachments
    // too: avatars leave RT as a whole (no floating hair / glasses shadow over a body
    // that casts none).
    // (legacy class dropped: only the avatars traced from their cluster cut keep their worn
    // attachments -- group map words 8..15)
    pp.joint_count = rig_sw_only_plan ? 0u : 0xFFFFFFFFu;
    pp.counter_word = used_words;
    pp.dbg_on = dbg ? 1u : 0u;
    {
        // Bit 1: faint alpha-blend faces (face alpha < 0.05) leave the TLAS (pass A).
        bool blend_cull = true;
#if defined(LL_HAS_CLEAN_PIPELINE)
        static LLCachedControl<bool> s_blend_cull(gSavedSettings, "RenderVKRTBlendCull", true);
        blend_cull = s_blend_cull;
#endif
        if (blend_cull) pp.dbg_on |= 2u;
        // FACING CULL GATES (clean_rt_instances.comp FACING_RIG / FACING_LEAF): the sun / moon
        // shadow rays always cull away-facing faces; these say which instances they reach.
        // Bit 2: single-sided rigged meshes + worn attachments (RenderVKRTRiggedFacingParity,
        // the shadow pass's back-face culling of them). Bit 3: single-sided static alpha-mask
        // leaves (#735; RenderVKRTFoliageTransmission 0 = off, its 3 = auto is never 0).
        bool rig_facing = true, leaf_facing = true;
#if defined(LL_HAS_CLEAN_PIPELINE)
        static LLCachedControl<bool> s_rig_facing(gSavedSettings, "RenderVKRTRiggedFacingParity", true);
        static LLCachedControl<U32> s_fol(gSavedSettings, "RenderVKRTFoliageTransmission", 3);
        rig_facing = s_rig_facing;
        leaf_facing = s_fol != 0u;
#endif
        if (rig_facing) pp.dbg_on |= 4u;
        if (leaf_facing) pp.dbg_on |= 8u;
        // SIZE CLASS (clean_rt_common.glsl / clean_rt_instances.comp maskClassSized): static opaque
        // instances below this bound take the RT_MASK_SMALL bit, which only the reflection rays'
        // far segment leaves out (RenderVKRTReflectionsSizeClasses); every other ray takes it.
        float small_size = 0.5f;
#if defined(LL_HAS_CLEAN_PIPELINE)
        static LLCachedControl<F32> s_small(gSavedSettings, "RenderVKRTReflectionsSmallSize", 0.5f);
        small_size = s_small;
#endif
        pp.small_size = (std::isfinite(small_size) && small_size > 0.f) ? std::min(small_size, 64.f) : 0.f;
    }
    if (dbg)
    {
        const LLCleanRendererVK::ProjFocus& f = R.mProjFocus;
        pp.dbg_sphere[0] = f.apex[0]; pp.dbg_sphere[1] = f.apex[1]; pp.dbg_sphere[2] = f.apex[2];
        pp.dbg_sphere[3] = f.reach;
    }
    // ---- TLAS: BUILD or UPDATE (F1). An update must match the last BUILD in count
    //      (VUID-vkCmdBuildAccelerationStructuresKHR-primitiveCount-03769), flags (-03759,
    //      which also need ALLOW_UPDATE: -03667), and per-instance activity (-03663 /
    //      -03664: kept by pass A / B from the record buffer the build read). The rest
    //      are quality / latency triggers: a refit only moves boxes, it never re-sorts. ----
    const VkBuildAccelerationStructureFlagsKHR tlas_flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR
        | (tlas_refit ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR : 0);
    // Rigged group records sit at their first member's id: any plan change moves them.
    const uint64_t tlas_rig_key = rig_ok ? (plan_key ^ ((uint64_t)mRigEntries.size() << 1)) : 1ull;
    float cam_d2 = 0.f;
    for (int k = 0; k < 3; ++k) { const float d = cull.lod_cam_pos[k] - mTlasBuildCam[k]; cam_d2 += d * d; }
    const bool tlas_update = tlas_refit && mTlas && mTlasHasBuild
        && mTlasBuildFlags == tlas_flags
        && mTlasBuildCount == pp.instance_count
        && mTlasBuildInstAddr == mTlasInst.address
        && mTlasBuildPackGen == R.mGeomPackGen
        && mTlasBuildRadius == radius
        && mTlasBuildRigKey == tlas_rig_key
        && !mIdleSinceBuild
        && mDummyBlas && mDummyBuiltFrame != 0 && mDummyBuiltFrame < mFrame
        && mFrame - mTlasBuildFrame < tlas_rebuild_n
        && cam_d2 < kTlasCamJump * kTlasCamJump
        && !(mTlasDeferred >= kTlasDeferRebuild && mFrame - mTlasBuildFrame >= 3u);
    pp.tlas_update = tlas_update ? 1u : 0u;
    pp.dummy_lo = tlas_update ? (uint32_t)(mDummyAddr & 0xFFFFFFFFull) : 0u;
    pp.dummy_hi = tlas_update ? (uint32_t)(mDummyAddr >> 32) : 0u;
    pp.rig_first = 0;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mCompPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mCompLayout, 0, 1, &mSetComp[mSlotIdx], 0, nullptr);
    pp.mode = 0;
    VK_PUSH_BUDGET_CHECK(pp);
    vkCmdPushConstants(cmd, mCompLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pp), &pp);
    vkCmdDispatch(cmd, (pp.instance_count + 63) / 64, 1, 1);

    // ---- smooth terrain: morph copy (mode 3) -> morph BLAS builds / refits ----
    if (!mTerrBuilds.empty())
    {
        // pass A's (declared) writes -> the copy's; the copy's positions -> AS build input
        memBarrier(cmd, kCS, kSW, kCS, kSR | kSW);
        pp.mode = 3;
        pp.rig_first = 0;
        VK_PUSH_BUDGET_CHECK(pp);
        vkCmdPushConstants(cmd, mCompLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pp), &pp);
        vkCmdDispatch(cmd, (uint32_t)mTerrEntries.size(), 1, 1);
        memBarrier(cmd, kCS, kSW, kAS | kCS, kASR | kSR | kSW);
        static std::vector<VkAccelerationStructureBuildGeometryInfoKHR> s_tinfos;
        static std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> s_tranges;
        s_tinfos.clear(); s_tranges.clear();
        for (size_t i = 0; i < mTerrBuilds.size(); ++i)
        {
            const TerrBuild& b = mTerrBuilds[i];
            const TerrMorph& tm = mTerrMorph[b.slot];
            VkAccelerationStructureBuildGeometryInfoKHR gi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
            gi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            gi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
            gi.mode = b.update ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            gi.srcAccelerationStructure = b.update ? tm.as : VK_NULL_HANDLE;
            gi.dstAccelerationStructure = tm.as;
            gi.geometryCount = 1; gi.pGeometries = &mTerrGeoms[i];
            gi.scratchData.deviceAddress = takeScratch(b.scratch);
            s_tinfos.push_back(gi);
            s_tranges.push_back(&mTerrRanges[i]);
        }
        fullB(cmd);   // (#889 A/B)
        rt.vkCmdBuildAccelerationStructuresKHR(cmd, (uint32_t)s_tinfos.size(), s_tinfos.data(), s_tranges.data());
        R.gpuSubMark(cmd, "c_blas_terrain");
        // The builds READ terr_pos; later dispatches of this pipeline declare it written (WAR).
        memBarrier(cmd, kAS, kASW, kCS, kSR | kSW);
        pp.mode = 0;
    }

    // ---- rigged: copy -> refit/build -> pass B ----
    if (!mRigEntries.empty())
    {
        // pre-skin output (compute) -> rig copy reads; pass A's writes -> the rig copy's
        // (same pipeline: its writeonly bindings count as writes on every dispatch)
        memBarrier(cmd, kCS, kSW, kCS, kSR | kSW);
        // #1007 POSE MOTION (mode 5): per joint-table slot, the pelvis-relative joint change
        // since the last mode 5 -> this slot's readback (host, read when this frame slot
        // comes round: harvestPoseMotion), then the snapshot takes this frame's rows. The
        // snapshot's previous-frame writes are ordered by the frame's leading barrier.
        mPoseRbValid[mSlotIdx] = false;
        if (pose_slots)
        {
            pp.mode = 5;
            pp.rig_first = 0;
            VK_PUSH_BUDGET_CHECK(pp);
            vkCmdPushConstants(cmd, mCompLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pp), &pp);
            vkCmdDispatch(cmd, pose_slots, 1, 1);
            memBarrier(cmd, kCS, kSW, kCS | VK_PIPELINE_STAGE_2_HOST_BIT, kSR | kSW | VK_ACCESS_2_HOST_READ_BIT);
            mPoseRbValid[mSlotIdx] = !pose_fresh;
            mPoseSnapReady = true;
            mPoseRbFrame[mSlotIdx] = mFrame;
            mPoseRbSlots[mSlotIdx] = pose_slots;
        }
        // Rig copy: only the entry runs of groups refit / built this frame (B8: a gated
        // group keeps last copy's rig_pos, which its BLAS and the consumers agree on).
        // rbuilds are in entry order; adjacent groups merge into one dispatch.
        pp.mode = 2;
        {
            uint32_t r0 = 0, rn = 0;
            auto flush = [&]()
            {
                if (!rn) return;
                pp.rig_first = r0;
                VK_PUSH_BUDGET_CHECK(pp);
                vkCmdPushConstants(cmd, mCompLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pp), &pp);
                vkCmdDispatch(cmd, rn, 1, 1);
                mRigCopyEntries += rn;
                rn = 0;
            };
            for (const RigBuild& b : rbuilds)
            {
                if (rn && r0 + rn == b.e0) { rn += b.n; continue; }
                flush();
                r0 = b.e0; rn = b.n;
            }
            flush();
            pp.rig_first = 0;
        }
        // rig positions -> AS build input; rig_valid + pass A records -> pass B
        memBarrier(cmd, kCS, kSW, kAS | kCS, kASR | kSR | kSW);
        static std::vector<VkAccelerationStructureBuildGeometryInfoKHR> s_rinfos;
        static std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> s_rranges;
        std::vector<VkAccelerationStructureBuildGeometryInfoKHR>& infos = s_rinfos;
        std::vector<const VkAccelerationStructureBuildRangeInfoKHR*>& ranges = s_rranges;
        infos.clear(); ranges.clear();
        infos.reserve(rbuilds.size()); ranges.reserve(rbuilds.size());
        for (RigBuild& b : rbuilds)
        {
            auto it = mRig.find(b.key);
            if (it == mRig.end() || !it->second.as) continue;
            VkAccelerationStructureBuildGeometryInfoKHR gi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
            gi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            gi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
            gi.mode = b.update ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            gi.srcAccelerationStructure = b.update ? it->second.as : VK_NULL_HANDLE;
            gi.dstAccelerationStructure = it->second.as;
            gi.geometryCount = b.n; gi.pGeometries = &mRigGeoms[b.e0];
            gi.scratchData.deviceAddress = takeScratch(b.scratch);
            infos.push_back(gi);
            ranges.push_back(&mRigRanges[b.e0]);   // one range per geometry, contiguous
            if (b.update) ++mRefits; else ++mRigBuilds;
        }
        fullB(cmd);   // (#889 A/B)
        if (!infos.empty())
            rt.vkCmdBuildAccelerationStructuresKHR(cmd, (uint32_t)infos.size(), infos.data(), ranges.data());
        R.gpuSubMark(cmd, "c_blas_refit");
        // The rig builds just READ rig_pos (build input); pass B uses the same pipeline,
        // whose rig_pos binding is writeonly -> order the build's reads (and its BLAS
        // writes, whose addresses pass B publishes) before the dispatch (WAR).
        memBarrier(cmd, kAS, kASW, kCS, kSR | kSW);
        // #747 / #748: which cluster-switched groups take their members' cluster records (mode 4:
        // every member has one this frame) -> pass B.
        const bool clrig = mClusterRT && mClusterRT->tlasRigBuffer();
        if (clrig)
        {
            pp.mode = 4;
            VK_PUSH_BUDGET_CHECK(pp);
            vkCmdPushConstants(cmd, mCompLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pp), &pp);
            vkCmdDispatch(cmd, (pp.rig_count + 63) / 64, 1, 1);
            memBarrier(cmd, kCS, kSW, kCS, kSR | kSW);
            // (its flags are host-read when this slot comes round: the host barrier below)
            mClCovValid[mSlotIdx] = mClCovEntries[mSlotIdx] == pp.rig_count;
        }
        pp.mode = 1;
        VK_PUSH_BUDGET_CHECK(pp);
        vkCmdPushConstants(cmd, mCompLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pp), &pp);
        vkCmdDispatch(cmd, (pp.rig_count + 63) / 64, 1, 1);
        // (its counters -> the host: read when this frame slot comes round, the Cluster RT line)
        if (clrig)
        {
            // #747 lift gate: pass B's "switched faces traced on legacy geometry" count -> the RT
            // skinned arena's word 2 (clean_rt_shadow_mask.comp skips the probe ray at 0)
            memBarrier(cmd, kCS, kSW, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
            mClusterRT->recordLiftFlag(cmd);
            memBarrier(cmd, kCS, kSW, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
        }
    }

    // ---- TLAS build: instance records (compute) + BLAS (builds/refits/compaction) ----
    memBarrier(cmd, kCS | kAS, kSW | kASW, kAS, kASR | kSR);
    fullB(cmd);   // (#889 A/B)
    if (mClusterRT && mClusterRT->ptlasLayout())
    {
        // TODO(2b) #748: the session's TLAS is partitioned: changed records -> write ops -> the
        // partitions holding them rebuild (in place). Same records, same instance ids.
        mClusterRT->ptlasBuild(cmd, mTlasInst, pp.instance_count, takeScratch);
        mTlasRefitThis = false;
        R.gpuSubMark(cmd, "c_cl_ptlas");
    }
    else
    {
        VkAccelerationStructureGeometryKHR g{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        g.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        g.geometry.instances = VkAccelerationStructureGeometryInstancesDataKHR{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
        g.geometry.instances.arrayOfPointers = VK_FALSE;
        g.geometry.instances.data.deviceAddress = mTlasInst.address;
        VkAccelerationStructureBuildGeometryInfoKHR gi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        gi.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        gi.flags = tlas_flags;
        gi.mode = tlas_update ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        gi.srcAccelerationStructure = tlas_update ? mTlas : VK_NULL_HANDLE;   // in place (-03668)
        gi.dstAccelerationStructure = mTlas;
        gi.geometryCount = 1; gi.pGeometries = &g;
        gi.scratchData.deviceAddress = takeScratch(mTlasScratch);   // max(build, update) (-12259)
        VkAccelerationStructureBuildRangeInfoKHR range{};
        range.primitiveCount = pp.instance_count;
        const VkAccelerationStructureBuildRangeInfoKHR* pr = &range;
        rt.vkCmdBuildAccelerationStructuresKHR(cmd, 1, &gi, &pr);
        mTlasRefitThis = tlas_update;
        if (!tlas_update)
        {
            mTlasHasBuild = true;
            mTlasBuildFrame = mFrame;
            mTlasBuildCount = pp.instance_count;
            mTlasBuildFlags = tlas_flags;
            mTlasBuildInstAddr = mTlasInst.address;
            mTlasBuildPackGen = R.mGeomPackGen;
            mTlasBuildRadius = radius;
            mTlasBuildRigKey = tlas_rig_key;
            for (int k = 0; k < 3; ++k) mTlasBuildCam[k] = cull.lod_cam_pos[k];
        }
    }
    // #874 RT scene audit: this frame's records + rt_geom -> the host ring (before c_tlas, so a
    // loss in a later pass of this frame finds them stamped).
    auditRecordCopy(cmd, pp.instance_count, slot_n, tlas_update, mClusterRT && mClusterRT->ptlasLayout(),
                    addr_bytes, rig_bytes != 0 && mStage[mSlotIdx].mapped != nullptr);
    // TLAS + rt_geom + rig positions -> every consumer (ray queries in FS / CS).
    // Deferred (setDeferRelease, RenderVKOverlapTlas): releaseToConsumers records it
    // at the first consumer with src = AS build only, so compute recorded in between
    // is not waited on. Correct because every compute write the consumers read
    // (pass A / rig copy / pass B: rt_geom, rig positions) is in the first scope of the
    // kCS|kAS -> kAS barrier just before the TLAS build, which made those writes
    // AVAILABLE; the TLAS build chains the two (its AS stage is in that barrier's second
    // scope and in the release's first), and the release's visibility op (kASR|kSR at
    // FS / CS) makes them VISIBLE. If that pre-TLAS barrier ever drops kCS from its
    // source, this deferral becomes a race: keep them in step.
    if (mDeferRelease)
        mReleaseOwed = kFS | kCS;
    else
        memBarrier(cmd, kAS | kCS, kASW | kSW, kFS | kCS, kASR | kSR);
    arenaGuardRecordCheck(cmd);   // #973 (after every AS build / refit / compaction of the frame)
    // used-slot bitset + active counter -> host (read on this frame slot's next turn)
    {
        memBarrier(cmd, kCS, kSW, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferCopy c{0, 0, (VkDeviceSize)rb_words * 4};
        vkCmdCopyBuffer(cmd, mUsedBits.buffer, mUsedRb[mSlotIdx].buffer, 1, &c);
        memBarrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
        mUsedRbWords[mSlotIdx] = used_words;
        mUsedRbValid[mSlotIdx] = true;
        mUsedRbDbg[mSlotIdx] = dbg;
        mUsedRbRigs[mSlotIdx] = (uint32_t)mRigEntries.size();
    }
    R.gpuSubMark(cmd, "c_tlas");
    mTlasBuilt = true;
    mIdleSinceBuild = false;

    phase("vk_rtP_record");
    // ---- counters (per frame; RenderVKSectionLog averages them) ----
    LLVKHitch::count("rt_blasResident", mStaticCount + (uint32_t)mRig.size());
    LLVKHitch::count("rt_blasMB", (unsigned)((mStaticBytes + mRigBytes) >> 20));
    LLVKHitch::count("rt_blasBuilt", mBuiltThisFrame);
    LLVKHitch::count("rt_blasRebuiltAfterRepack", mRepackRebuilt);
    LLVKHitch::count("rt_blasCompacted", mCompactions);
    LLVKHitch::count("rt_blasStarved", mStarved);
    LLVKHitch::count("rt_rigRefits", mRefits);
    LLVKHitch::count("rt_rigBuilds", mRigBuilds);
    LLVKHitch::count("rt_tlasInstances", mTlasActive);   // active, from 2 frames ago
    LLVKHitch::count("rt_tlasRefit", mTlasRefitThis ? 1u : 0u);
    LLVKHitch::count("rt_tlasRebuild", mTlasRefitThis ? 0u : 1u);
    LLVKHitch::count("rt_tlasDeferred", mTlasDeferred);  // waiting to enter, from 2 frames ago
    LLVKHitch::count("rt_rigRefitSkipped", mRigRefitSkipped);
    LLVKHitch::count("rt_rigRefitSkippedMoving", mRigRefitSkippedMoving);   // #632: held pose, root moved
    LLVKHitch::count("rt_rigSkipCadence", mRigSkipCadence);   // #1007: distance / screen-size cadence
    LLVKHitch::count("rt_rigSkipPose", mRigSkipPose);         // #1007: pose change under the threshold
    LLVKHitch::count("rt_rigMaxStale", mRigMaxStale);         // #1007: frames, the stalest skipped group
    LLVKHitch::count("rt_rigCopyEntries", mRigCopyEntries);
    LLVKHitch::count("rt_terrMorph", (uint32_t)mTerrMorph.size());
    LLVKHitch::count("rt_terrMorphBuilt", mTerrBuiltN);
    LLVKHitch::count("rt_terrMorphRefit", mTerrRefitN);
    LLVKHitch::count("rt_terrPriority", (uint32_t)mTerrPriority.size());
    if (mOmmOk)
    {
        LLVKHitch::count("rt_ommLive", mOmmLive);
        LLVKHitch::count("rt_ommBuilt", mOmmBuilt);
        LLVKHitch::count("rt_ommInstances", mOmmUsedInst);
        LLVKHitch::count("rt_ommMB", (unsigned)(mOmmBytes >> 20));
        LLVKHitch::count("rt_ommRetired", mOmmRetired);
        mOmmRetired = 0;
    }
    {
        static double s_next = 0.0;
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if (now >= s_next)
        {
            s_next = now + 5.0;
            LL_INFOS("VKRT") << "RT: blas static " << mStaticCount << " (" << (mStaticBytes >> 20) << " MB) rigged "
                             << mRig.size() << " groups / " << mRigEntries.size() << " faces (" << (mRigBytes >> 20)
                             << " MB, cluster-only " << mClOnlyGroups << ") | this frame built " << mBuiltThisFrame
                             << " after-repack " << mRepackRebuilt << " compacted " << mCompactions << " starved " << mStarved
                             << " rig group refit " << mRefits << " build " << mRigBuilds
                             << " gated " << mRigRefitSkipped << " (held + moving " << mRigRefitSkippedMoving << ")"
                             << " [#1007 refits/frame " << mRefits << " skipped by cadence " << mRigSkipCadence
                             << " by pose " << mRigSkipPose << " max stale " << mRigMaxStale << "]"
                             << " no-self-shadow (throttled) " << mNoSelfCount
                             << " | terrain morph " << mTerrMorph.size() << " (" << (mTerrBytes >> 20) << " MB) built "
                             << mTerrBuiltN << " refit " << mTerrRefitN << " | tlas " << (mTlasRefitThis ? "refit" : "build")
                             << " active " << mTlasActive << "/" << pp.instance_count << " deferred " << mTlasDeferred
                             << " (cap " << mTlasCap << ")"
                             << " | aux MB scratch " << (mCtx->allocBytes(mScratch.memory) >> 20)
                             << " tlas " << (mCtx->allocBytes(mTlasBuf.memory) >> 20)
                             << " tlasInst " << (mCtx->allocBytes(mTlasInst.memory) >> 20)
                             << " geom " << (mCtx->allocBytes(mRtGeom.memory) >> 20)
                             << " rigPos " << (mCtx->allocBytes(mRigPos.memory) >> 20)
                             << " stage " << ((mCtx->allocBytes(mStage[0].memory) + mCtx->allocBytes(mStage[1].memory)
                                               + mCtx->allocBytes(mStage[2].memory)) >> 20)
                             << " rb " << ((mCtx->allocBytes(mUsedRb[0].memory) + mCtx->allocBytes(mUsedRb[1].memory)
                                            + mCtx->allocBytes(mUsedRb[2].memory)) >> 20)
                             << " (tlas cap " << mTlasCap << ", arena " << (asBytes() >> 20) << ")"
                             << " | budget " << (total_budget >> 20) << " MB (BLAS " << (budget >> 20) << ", cluster RT " << (cl_bytes >> 20) << ")" << (mRigDropped ? " rigged-dropped" : "") << (mOverBudget ? " OVER" : "") << LL_ENDL;
            // RT AVATARS (2026-10-13): which path traces how many avatars this frame, the budget
            // split, and why switched avatars still have a legacy BLAS.
            {
                uint32_t leg[kAvVisSlots / 32] = {}, swfb[kAvVisSlots / 32] = {};
                uint32_t leg_world = 0;
                uint64_t sw_bytes = 0;
                for (const auto& kv : mRig)
                {
                    if (kv.second.lastSeen != mFrame) continue;   // (not in this frame's TLAS)
                    const uint64_t ow = kv.first >> 8;
                    if (kv.first & 1u) sw_bytes += kv.second.mem.size;
                    if (ow >= kAvVisSlots) { ++leg_world; continue; }
                    (kv.first & 1u ? swfb : leg)[ow >> 5] |= 1u << (ow & 31u);
                }
                uint32_t n_leg = 0, n_swfb = 0;
                for (uint32_t i = 0; i < kAvVisSlots / 32; ++i)
                {
                    n_leg += (uint32_t)__builtin_popcount(leg[i] & ~swfb[i]);
                    n_swfb += (uint32_t)__builtin_popcount(swfb[i]);
                }
                const char* state = mRigPressureDrop ? "pressure drop (legacy avatars out, cluster ones in)"
                                  : mRigDropped ? "VRAM pressure: legacy avatars out, cluster ones in"
                                  : "all in";
                // (like with like: the cluster-path avatars the legacy RT rules trace vs those pass B
                // traced from their cluster cut; the others by reason -- a HOLE is always real)
                const ClExpect& ex = mClExpLast;
                std::ostringstream why;
                if (mClHoles)
                    why << ", HOLES " << mClHoles << ": not in the RT plan " << ex.not_plan << " (skin " << ex.np_skin
                        << ", row " << ex.np_row << ", other " << ex.np_other << "), fell back " << mClHoleFell;
                std::ostringstream hd;
                hd << "RT avatars: cluster path " << ex.cl_av << " -- traced from the cluster cut "
                   << (mClExpN - std::min(mClExpN, mClHoles)) << " of " << mClExpN << " the RT rules trace"
                   << (mClConsistStreak >= kClConsistFrames ? " MISMATCH" : "") << why.str()
                   << "; not traced by rule: no-shadow (reduced tier) " << ex.no_shadow << ", seen by no pass " << ex.unseen
                   << "; no-self-shadow (throttled) " << mNoSelfCount;
                if (ex.plan_off) hd << ", rig plan off " << ex.plan_off;
                if (ex.extra) hd << " (+ unexpected " << ex.extra << ")";
                // ANIMESH BREAKOUT (2026-10-14): control avatars by outcome, this frame, on both
                // paths. Animesh never joins the reduced tier (LLVOAvatar::vkUpdateReducedTier),
                // so "no-shadow" should stay 0; "other" = eligible but in neither TLAS path this
                // frame (still loading / cluster cut not ready / dropped over the RT budget).
                {
                    const bool avok = R.mAvVisSSBO.buffer && R.mAvVis[kAvVisSlots] != 0;
                    uint32_t am_n = 0, am_leg = 0, am_cl = 0, am_hidden = 0, am_unseen = 0, am_nosh = 0, am_other = 0;
                    for (uint32_t a = 0; a < kAvVisSlots; ++a)
                    {
                        if (!R.mAvAnimesh[a]) continue;
                        ++am_n;
                        const uint32_t m = 1u << (a & 31u);
                        const uint32_t v = R.mAvVis[a];
                        if (R.mAvPolicyHidden[a]) ++am_hidden;
                        else if (avok && !(v & (LLCleanRendererVK::kAvVisAny | LLCleanRendererVK::kAvVisRtProbe))) ++am_unseen;
                        else if (avok && (v & LLCleanRendererVK::kAvVisNoShadow)) ++am_nosh;
                        else if ((leg[a >> 5] | swfb[a >> 5]) & m) ++am_leg;
                        else if (mClCovStreak[a] > 0 && mFrame - mClCovFrame[a] <= kClCovMaxAge) ++am_cl;
                        else ++am_other;
                    }
                    hd << " | animesh " << am_n << " (cluster path " << ex.am_cl_av << "): traced legacy BLAS " << am_leg
                       << " + cluster cut " << am_cl << "; not traced: no-shadow (reduced tier) " << am_nosh
                       << ", seen by no pass " << am_unseen << ", hidden by policy " << am_hidden
                       << ", other (loading / dropped / not ready) " << am_other;
                }
                LL_INFOS("VKRT") << hd.str()
                                 << " | cluster RT data ready " << mClConsistReady << ", covered " << mClCovIn << ", fell back "
                                 << mClCovFell << ", cluster-only groups " << mClOnlyGroups << " | legacy BLAS " << n_leg
                                 << " avatars (+" << leg_world << " world groups), switched on legacy BLAS " << n_swfb
                                 << " (" << (sw_bytes >> 20) << " MB; "
                                 << (clusterTracesSwitched() ? "0 expected: cluster RT traces them"
                                                             : "cluster RT does not trace switched avatars")
                                 << ") | " << state << " | budget " << (total_budget >> 20) << " MB = cluster RT "
                                 << (cl_bytes >> 20) << " + working " << (legacy_aux >> 20) << " + static BLAS "
                                 << (mStaticBytes >> 20) << " + legacy avatar BLAS " << (mRigBytes >> 20) << " (+ free "
                                 << ((total_budget > cl_bytes + legacy_aux + mStaticBytes + mRigBytes)
                                     ? (total_budget - cl_bytes - legacy_aux - mStaticBytes - mRigBytes) >> 20 : 0)
                                 << ") | refits deferred " << mRigRefitDeferred << ", starved builds " << mRigStarvedBuilt
                                 << " | rig copy: undrawn " << mRigUndrawn << " mismatched " << mRigInvalid
                                 << " input-check non-finite " << mRigBadNonFinite << " out-of-reach " << mRigBadFar
                                 << " (total " << mRigBadTotal << ", groups marked for rebuild " << mRigBadRebuilds << ")"
                                 << " (legacy faces " << mRigPlanLegFaces << ", cluster-routable " << mRigPlanLegRoutable
                                 << ", never-bits 0x" << std::hex << mRigPlanLegNever << std::dec << ")" << LL_ENDL;
            }
        }
    }
}

// ===========================================================================
// debug views
// ===========================================================================
void LLCleanRTVK::recordDebug(VkCommandBuffer cmd, VkImageView colorView, uint32_t w, uint32_t h,
                              const VKDrawPush& draw, uint32_t view)
{
    VkMemScope memcat(VKMEM_RT_AUX);   // #407
    if (!mTlasBuilt || !mDebugPipe || !colorView || mSetKeyRT[mSlotIdx].empty() || !mSetRTDepth[mSlotIdx]
        || !mR.mDsetDraw1) return;
    const LLCleanRendererVK::ProjFocus& focus = mR.mProjFocus;
    if (view == 18u && mDbgStats.buffer)
    {
        readSunBlocker(mSlotIdx);
        memBarrier(cmd, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                                                               | VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                   kXfer, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        vkCmdFillBuffer(cmd, mDbgStats.buffer, 0, VK_WHOLE_SIZE, 0);
        memBarrier(cmd, kXfer, VK_ACCESS_2_TRANSFER_WRITE_BIT, kFS, kSR | kSW);
    }
    if (view == 15u)
    {
        if (!focus.valid || !mDbgStats.buffer)
        {
            LL_WARNS_ONCE("VKRT") << "RenderVKDebugView 15 needs RenderVKProjectorShadows 2 with at least one traced "
                                     "projector and RenderVKRTDebugFlicker >= 1 (the focus projector)" << LL_ENDL;
            return;
        }
        // This slot's previous stats (2 frames old; its fence was waited), then zero for this frame.
        readDebugStats(mSlotIdx);
        memBarrier(cmd, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                                                               | VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                   kXfer, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        vkCmdFillBuffer(cmd, mDbgStats.buffer, 0, VK_WHOLE_SIZE, 0);
        memBarrier(cmd, kXfer, VK_ACCESS_2_TRANSFER_WRITE_BIT, kFS, kSR | kSW);
    }
    // Prior colour writes (tonemap / glow / AA) -> our full overwrite.
    memBarrier(cmd, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
    VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ca.imageView = colorView; ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {w, h}}; ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
    vkCmdBeginRendering(cmd, &ri);
    VkViewport vp{0, 0, (float)w, (float)h, 0, 1};
    VkRect2D sc{{0, 0}, {w, h}};
    vkCmdSetViewport(cmd, 0, 1, &vp); vkCmdSetScissor(cmd, 0, 1, &sc);
    bindGraphicsPipeline(cmd, mDebugPipe);
    VkDescriptorSet sets[2] = {mR.mDsetDraw1, mSetRT[mSlotIdx]};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mDebugLayout, 1, 2, sets, 0, nullptr);
    RtDebugPush p{};
    std::memcpy(p.inv_view_proj, draw.inv_view_proj, sizeof(p.inv_view_proj));
    std::memcpy(p.cam_time, draw.cam_time, sizeof(p.cam_time));
    p.params[0] = (float)view;
    p.params[1] = 0.01f;    // 1% of the raster distance ...
    p.params[2] = 0.05f;    // ... or 5 cm, whichever is larger (depth quantization, near plane)
    p.params[3] = 4096.f;   // tmax
    if (view == 15u)
    {
        p.params[1] = focus.reach;
        p.params[2] = focus.bias;
        p.params2[1] = focus.axis[0]; p.params2[2] = focus.axis[1]; p.params2[3] = focus.axis[2];
        p.params3[0] = focus.apex[0]; p.params3[1] = focus.apex[1]; p.params3[2] = focus.apex[2];
        p.params3[3] = focus.near_d;
    }
    else if (view == 18u)
    {
        // SUN BLOCKER VIEW: the light the RT sun traces toward (atmos lane 7, the shaders'
        // atmosLightDir: sun or moon) and the extra receiver offset.
        float bias = 0.f;
#if defined(LL_HAS_CLEAN_PIPELINE)
        static LLCachedControl<F32> s_rbias(gSavedSettings, "RenderVKRTShadowBias", 0.f);
        bias = std::max((F32)s_rbias, 0.f);
#endif
        p.params[2] = bias;
        p.params2[1] = mR.mAtmos[28]; p.params2[2] = mR.mAtmos[29]; p.params2[3] = mR.mAtmos[30];
        p.params3[0] = (float)(w / 2); p.params3[1] = (float)(h / 2);   // the logged probe pixel
    }
    {
        // The world sampler's actual anisotropy (live: LLCleanRendererVK::applyAnisoChange),
        // so the ray-cone LOD picks the mip the raster samples.
        const float level = mR.mSamplerAniso;
        p.params2[0] = std::max(level, 1.f);
    }
    VK_PUSH_BUDGET_CHECK(p);
    vkCmdPushConstants(cmd, mDebugLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(p), &p);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    if (view == 15u || (view == 18u && mDbgStats.buffer))
    {
        GpuBuffer& rb = mDbgStatsRb[mSlotIdx];
        VkDeviceContext::SoftAllocScope dbg_soft(mCtx);   // #697 (null: no readback this frame)
        if (!rb.buffer)
            rb = mCtx->createBuffer(kDbgStatsWords * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                    | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        if (rb.buffer)
        {
            memBarrier(cmd, kFS, kSW, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
            VkBufferCopy c{0, 0, kDbgStatsWords * 4};
            vkCmdCopyBuffer(cmd, mDbgStats.buffer, rb.buffer, 1, &c);
            memBarrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
            mDbgStatsValid[mSlotIdx] = true;
        }
    }
}

// ===========================================================================
// FLICKER DIAGNOSTIC (RenderVKRTDebugFlicker)
// ===========================================================================
namespace
{
// (bit 7: the small static opaque class -- the own avatar shares the rigged bit; clean_rt_common.glsl SIZE CLASS)
const char* kClassName[8] = {"opaque", "mask", "blend", "rigged", "terrain", "water", "attach", "small"};
double nowSec()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
// Per-second line budget shared by the flicker logs.
bool dbgLineOk()
{
    static uint32_t s_lines = 0;
    static double s_reset = 0.0;
    const double now = nowSec();
    if (now >= s_reset) { s_lines = 0; s_reset = now + 1.0; }
    return s_lines++ < 12;
}
}

// TLAS membership near the focus apex (clean_rt_instances.comp tail), one frame per
// call in frame order. Logs whenever the focus set's hash / count changes frame to
// frame, naming up to 8 instances that entered / left (id, flags, class, skin, pos).
void LLCleanRTVK::readDebugTail(const uint32_t* t, uint32_t rig_entries)
{
    // t[0] = deferred counter (unused here); t[1..8] class counts; t[9] focus n; t[10] xor; t[11] sum;
    // t[12..19] focus per class; t[20..] (id, hash) pairs.
    const uint32_t n = t[9];
    std::vector<std::pair<uint32_t, uint32_t>> ids;
    ids.reserve(std::min(n, kDbgFocusCap));
    for (uint32_t i = 0; i < std::min(n, kDbgFocusCap); ++i) ids.emplace_back(t[20 + 2 * i], t[21 + 2 * i]);
    std::sort(ids.begin(), ids.end());
    const bool changed = mDbgHavePrev && (n != mDbgPrevN || t[10] != mDbgPrevXor || t[11] != mDbgPrevSum);
    ++mDbgWinFrames;
    if (changed)
    {
        ++mDbgWinChanged;
        if (dbgLineOk())
        {
            // entered / left by id; `mod` = same id, different hash (BLAS address,
            // transform, flags, material or tint alpha changed).
            std::vector<uint32_t> in, out, mod;
            {
                size_t a = 0, b = 0;
                while (a < ids.size() || b < mDbgPrevIds.size())
                {
                    if (b >= mDbgPrevIds.size() || (a < ids.size() && ids[a].first < mDbgPrevIds[b].first))
                        in.push_back(ids[a++].first);
                    else if (a >= ids.size() || mDbgPrevIds[b].first < ids[a].first)
                        out.push_back(mDbgPrevIds[b++].first);
                    else { if (ids[a].second != mDbgPrevIds[b].second) mod.push_back(ids[a].first); ++a; ++b; }
                }
            }
            std::ostringstream os;
            os << "TLAS focus set changed: " << mDbgPrevN << " -> " << n
               << (n > kDbgFocusCap || mDbgPrevN > kDbgFocusCap ? " (id list truncated)" : "")
               << " | entered " << in.size() << " left " << out.size()
               << " modified " << mod.size() << " (BLAS address / transform / flags / material / tint alpha)"
               << (n > kDbgFocusCap ? " [focus sphere holds more than the id cap: diff incomplete]" : "")
               << " | per class (prev->now):";
            for (int c = 0; c < 8; ++c)
                if (t[12 + c] || mDbgPrevCls[c]) os << " " << kClassName[c] << " " << mDbgPrevCls[c] << "->" << t[12 + c];
            os << " | rig entries " << rig_entries;
            const uint8_t* ib = mR.mCpuInstances.data();
            const uint32_t inst_n = (uint32_t)(mR.mCpuInstances.size() / kInstBytes);
            auto describe = [&](char sign, uint32_t id) {
                os << "\n    " << sign << id;
                if (id >= inst_n) { os << " (row gone)"; return; }
                const uint8_t* in = ib + (size_t)id * kInstBytes;
                float m[16]; std::memcpy(m, in, sizeof(m));
                os << " flags 0x" << std::hex << rd32(in + kInstFlags) << std::dec
                   << " skin " << (int32_t)rd32(in + kInstSkin) << " group " << (int32_t)rd32(in + kInstGroup)
                   << " pos(store frame, -" << mR.mInstD[0] << "," << mR.mInstD[1] << " = agent) " << m[12] << "," << m[13] << "," << m[14];
            };
            for (size_t i = 0; i < in.size() && i < 8; ++i) describe('+', in[i]);
            for (size_t i = 0; i < out.size() && i < 8; ++i) describe('-', out[i]);
            for (size_t i = 0; i < mod.size() && i < 8; ++i) describe('~', mod[i]);
            LL_INFOS("VKRTFLICKER") << os.str() << LL_ENDL;
        }
    }
    mDbgHavePrev = true;
    mDbgPrevN = n; mDbgPrevXor = t[10]; mDbgPrevSum = t[11];
    for (int c = 0; c < 8; ++c) mDbgPrevCls[c] = t[12 + c];
    mDbgPrevIds.swap(ids);
    const double now = nowSec();
    if (now >= mDbgNextSummary)
    {
        mDbgNextSummary = now + 2.0;
        std::ostringstream os;
        os << "TLAS focus set: " << n << " instances near the apex, changed in " << mDbgWinChanged << "/"
           << mDbgWinFrames << " frames | active by class:";
        for (int c = 0; c < 8; ++c) os << " " << kClassName[c] << " " << t[1 + c];
        os << " | rig entries " << rig_entries;
        if (mDbgVisFrames)
            os << " | view15 lit fraction min " << mDbgVisMin << " max " << mDbgVisMax
               << ", blocked-fraction jumps (>5%) " << mDbgVisJumps << "/" << mDbgVisFrames;
        LL_INFOS("VKRTFLICKER") << os.str() << LL_ENDL;
        mDbgWinChanged = mDbgWinFrames = 0;
        mDbgVisFrames = mDbgVisJumps = 0; mDbgVisMin = 1.f; mDbgVisMax = 0.f;
    }
}

// Debug view 15 statistics of frame slot s (written 2 frames ago): visibility of the
// focus projector over the screen, and the instances that blocked it.
void LLCleanRTVK::readDebugStats(uint32_t s)
{
    if (!mDbgStatsValid[s] || !mDbgStatsRb[s].mapped) return;
    mDbgStatsValid[s] = false;
    const uint32_t* w = static_cast<const uint32_t*>(mDbgStatsRb[s].mapped);
    const uint32_t rays = w[0];
    if (rays == 0) return;
    const float blocked = (float)w[1] / (float)rays, partial = (float)w[2] / (float)rays;
    const float lit = (float)w[3] / 1024.f / (float)rays;
    std::vector<std::pair<uint32_t, uint32_t>> top;   // (count, id)
    for (uint32_t i = 0; i < 64; ++i)
        if (w[4 + i * 2]) top.emplace_back(w[5 + i * 2], w[4 + i * 2] - 1u);
    std::sort(top.rbegin(), top.rend());
    ++mDbgVisFrames;
    mDbgVisMin = std::min(mDbgVisMin, lit); mDbgVisMax = std::max(mDbgVisMax, lit);
    const bool jump = mDbgVisHavePrev && std::fabs(blocked - mDbgVisPrevBlocked) > 0.05f;
    if (jump) ++mDbgVisJumps;
    if (jump && dbgLineOk())
    {
        std::ostringstream os;
        os << "view15 visibility jump: blocked " << mDbgVisPrevBlocked << " -> " << blocked << " (partial " << partial
           << ", mean lit " << lit << ", " << rays << " rays) | top blockers:";
        for (size_t i = 0; i < top.size() && i < 4; ++i) os << " " << top[i].second << "x" << top[i].first;
        LL_INFOS("VKRTFLICKER") << os.str() << LL_ENDL;
    }
    mDbgVisHavePrev = true;
    mDbgVisPrevBlocked = blocked;
}

// View 18: the screen-centre pixel's sun blockers (alpha-tested ray, force-opaque ray),
// logged with the instance's flags and material when they change.
void LLCleanRTVK::readSunBlocker(uint32_t s)
{
    if (!mDbgStatsValid[s] || !mDbgStatsRb[s].mapped) return;
    mDbgStatsValid[s] = false;
    const uint32_t* w = static_cast<const uint32_t*>(mDbgStatsRb[s].mapped);
    if (w[0] != 1u) return;
    const uint32_t b = w[1], ob = w[2];
    if (b == mDbgSunBlk[0] && ob == mDbgSunBlk[1]) return;
    mDbgSunBlk[0] = b; mDbgSunBlk[1] = ob;
    const LLCleanRendererVK& R = mR;
    auto describe = [&](std::ostringstream& os, uint32_t id1)
    {
        if (id1 == 0u) { os << "none"; return; }
        const uint32_t id = id1 - 1u;
        os << "inst " << id;
        if ((size_t)(id + 1) * kInstBytes > R.mCpuInstances.size()) { os << " (no CPU row)"; return; }
        const uint8_t* in = R.mCpuInstances.data() + (size_t)id * kInstBytes;
        const uint32_t flags = rd32(in + kInstFlags), mid = rd32(in + 180), skin = rd32(in + kInstSkin);
        float tint_a = 0.f;
        std::memcpy(&tint_a, in + 108, 4);
        os << " flags 0x" << std::hex << flags << std::dec
           << ((flags & 2048u) ? " water" : (flags & 32u) ? " terrain" : (flags & 2u) ? " BLEND" : (flags & 4u) ? " MASK" : " opaque")
           << ((flags & 1u) ? " rigged" : "") << ((flags & 512u) ? " attached" : "") << ((flags & 256u) ? " 2-sided" : "")
           << " skin " << (int32_t)skin << " tintA " << tint_a << " mat " << mid;
        if ((size_t)(mid + 1) * 96 <= R.mCpuMaterials.size())
        {
            const uint8_t* m = R.mCpuMaterials.data() + (size_t)mid * 96;
            float cutoff = 0.f, bca = 0.f;
            std::memcpy(&cutoff, m + 88, 4);
            std::memcpy(&bca, m + 44, 4);   // base_color_factor.a
            os << " (cutoff " << cutoff << " baseA " << bca << " mflags 0x" << std::hex << rd32(m + 92) << std::dec
               << " tex " << rd32(m + 0) << ")";
        }
    };
    std::ostringstream os;
    os << "view18 centre: vis " << (float)w[3] / 1024.f << " | alpha-tested blocker: ";
    describe(os, b);
    os << " | force-opaque blocker (transparent receivers): ";
    describe(os, ob);
    LL_INFOS("VKRT") << os.str() << LL_ENDL;
}

// ===========================================================================
// accounting
// ===========================================================================
uint64_t LLCleanRTVK::asBytes() const
{
    if (!mCtx) return 0;
    uint64_t b = mCtx->allocBytes(mTlasBuf.memory);
    for (const Chunk& c : mChunks) b += mCtx->allocBytes(c.buf.memory);
    if (mClusterRT) b += mClusterRT->asBytes();   // TODO(2b) #748: CLAS / cluster BLAS / PTLAS arenas
    return b;
}

uint64_t LLCleanRTVK::rtAuxBytes() const
{
    // TODO(2b) #748: the cluster RT arenas are fixed working memory of the budget too
    return mCtx ? auxBytes() + mCtx->allocBytes(mTlasBuf.memory) + (mClusterRT ? mClusterRT->asBytes() : 0) : 0;
}

// The automatic RT budget before the headroom rule has a measurement (llcleanrtvk.h ONE RT MEMORY
// BUDGET). #1015: no percentage of VRAM any more -- RT is bounded by the device-local budget
// itself (at least 384 MB); the headroom rule below narrows it to the room that is actually free.
static uint64_t autoRtBudget(uint64_t dev)
{
    return std::max<uint64_t>(dev, 384ull << 20);
}
constexpr uint64_t kRtBudgetFloor = 384ull << 20;   // the headroom rule never goes under this

// HEADROOM RULE (llcleanrtvk.h setDeviceUsage): RT may grow into VRAM nothing else uses:
// target = what RT holds + (budget - usage - reserve), i.e. the
// budget minus the reserve minus everything that is not RT (a fixed point: RT growing shrinks the
// free part by as much). Reserve = max(2 GB, 30% of the budget): RT never pushes the process
// past 70% of the capacity, under every other controller's line on the same measurement -- the
// ladder (LLVramBudgetVK: B = 90% of capacity, soft step past 95% of B = 85.5%, rung recovery
// only under 80% of B = 72%), texture borrowing (93% of B), the rigged floor (90% of the OS
// budget) -- so RT growing into free VRAM can neither trip the ladder nor stall its recovery.
// Growth only while the ladder is calm (no rung engaged); a shrink always applies. At most
// the budget; never under kRtBudgetFloor (#1015: the old fixed 12% share and the 60% / 16 GB
// ceiling are gone, every card gets the room it really has).
static uint64_t headroomReserve(uint64_t dev) { return std::max<uint64_t>(2ull << 30, dev / 100 * 30); }
static uint64_t headroomCeiling(uint64_t dev) { return dev; }

void LLCleanRTVK::updateHeadroomBudget()
{
    const uint64_t dev = mDeviceBudget;
    if (!mCtx || dev == 0 || mDeviceUsage == 0)
    {
        mHeadroomBudget = 0;   // not measured yet: the device budget itself
        return;
    }
    const uint64_t held = asBytes() + auxBytes();
    const uint64_t other = mDeviceUsage > held ? mDeviceUsage - held : 0;   // everything that is not RT
    const uint64_t reserve = headroomReserve(dev);
    const uint64_t room = dev > other + reserve ? dev - other - reserve : 0;
    const uint64_t target = std::clamp<uint64_t>(room, kRtBudgetFloor, std::max(kRtBudgetFloor, headroomCeiling(dev)));
    const uint64_t cur = mHeadroomBudget;
    // Hysteresis: the measured usage moves every frame (texture streaming); grow past +5%,
    // shrink under -10% (a shrink evicts BLAS), else hold.
    if ((cur == 0 || target > cur + cur / 20) && mLadderCalm) mHeadroomBudget = std::max(cur, target);
    else if (cur != 0 && target + cur / 10 < cur) mHeadroomBudget = target;
}

uint64_t LLCleanRTVK::autoBudgetBytes() const
{
    return mHeadroomBudget ? mHeadroomBudget : autoRtBudget(mDeviceBudget ? mDeviceBudget : (8ull << 30));
}

uint64_t LLCleanRTVK::budgetBytes() const
{
    uint64_t b = 0;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<U32> s_budget(gSavedSettings, "RenderVKRTBlasBudgetMB", 0);
    b = (uint64_t)(U32)s_budget << 20;
#endif
    if (b == 0) b = autoBudgetBytes();
    // The pressure cap never squeezes the budget under the working set + a geometry
    // share (pressureFloor, evaluated live: the working buffers can grow after the cap).
    if (mPressureCap) b = std::min(b, std::max(mPressureCap, pressureFloor()));
    return std::max<uint64_t>(b, 64ull << 20);
}

std::string LLCleanRTVK::budgetWhy() const
{
    std::ostringstream os;
    uint32_t setting = 0;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<U32> s_budget(gSavedSettings, "RenderVKRTBlasBudgetMB", 0);
    setting = s_budget;
#endif
    if (setting) os << "RenderVKRTBlasBudgetMB " << setting << " MB";
    else
    {
        const uint64_t dev = mDeviceBudget ? mDeviceBudget : (8ull << 30);
        os << "auto: VRAM budget " << (dev >> 20) << " MB" << (mDeviceBudget ? "" : " (not measured yet: 8 GB assumed)")
           << ", no percentage cap";
        if (mHeadroomBudget)
        {
            const uint64_t held = asBytes() + auxBytes();
            os << "; HEADROOM" << (mLadderCalm ? "" : " (held: ladder engaged)") << ": usage " << (mDeviceUsage >> 20) << " MB (RT " << (held >> 20) << " MB, other "
               << ((mDeviceUsage > held ? mDeviceUsage - held : 0) >> 20) << " MB), reserve "
               << (headroomReserve(dev) >> 20) << " MB -> " << (mHeadroomBudget >> 20) << " MB";
        }
        else os << "; headroom rule: usage not measured yet";
    }
    if (mPressureCap)
        os << "; VRAM pressure cap " << (mPressureCap >> 20) << " MB (floor " << (pressureFloor() >> 20)
           << " MB; temporary, lifted when calm)";
    return os.str();
}

// Working buffers (TLAS, scratch, instance / rig / terrain tables, staging) at <= 60% of
// the floor: recordBuild's overBudget test (working set > 70% of the budget) can never be
// tripped by the pressure cap alone, and BLAS keep >= 40% (+32 MB).
// ---- #692: rigged groups out of the TLAS ----
std::string LLCleanRTVK::rigOwnerText(uint64_t key) const
{
    const uint64_t owner = key >> 8;
    const uint32_t cls = (uint32_t)(key & 0xFCu);   // (bit 0: the cluster-switched faces, #747 / #748; bit 1: double-sided)
    std::ostringstream os;
    if (owner < kAvVisSlots)
    {
        os << "avatar (joint-table slot " << owner;
#if defined(LL_HAS_CLEAN_PIPELINE)
        if (mR.mRigSelfSlot[owner]) os << ", our own";
#endif
        os << ")";
    }
    else os << "rigged mesh (skin " << (owner & 0xFFFFFFFFull) << ", no avatar row)";
    if (cls)
        os << ", " << (cls == 0x08u ? "rigged" : cls == 0x04u ? "alpha-blend"
                       : cls == 0x10u ? "terrain" : cls == 0x20u ? "water" : "?") << " mask group"
           << ((key & 2u) ? " (double-sided faces)" : "")
           << ((key & 1u) ? " (cluster-switched faces)" : "");
    return os.str();
}

bool LLCleanRTVK::rigLogOk()
{
    // At most 8 lines per 10 s (an allocation storm in a crowd must not flood the log); a
    // line that does not fit is written on a later frame (its state keeps logged false).
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now - mRigLogWindow > 10.0) { mRigLogWindow = now; mRigLogLines = 0; }
    if (mRigLogLines >= 8) return false;
    ++mRigLogLines;
    return true;
}

void LLCleanRTVK::rigNoteOut(uint64_t key, RigOut why, uint32_t faces, uint32_t tris, bool alloc_failed,
                             uint32_t all_faces, uint32_t all_tris)
{
    RigWait& w = mRigWait[key];
    if (w.frames == 0 || w.lastFrame + 1u < mFrame)
    {
        // a new stretch out (or the first)
        w.since = mFrame; w.frames = 0; w.logged = false;
    }
    if (w.lastFrame != mFrame || w.frames == 0) ++w.frames;
    w.lastFrame = mFrame;
    w.reason = why;
    w.faces = faces; w.tris = tris;
    w.all_faces = all_faces; w.all_tris = all_tris;
    if (alloc_failed)
    {
        ++w.allocFails;
        // #697 policy: retry some frames later (doubling), never crash, never give up while
        // the group still wants in -- memory may come back.
        const uint32_t sh = std::min<uint32_t>(w.allocFails - 1u, 5u);
        w.retryAt = mFrame + std::min<uint32_t>(kRigAllocRetry << sh, kRigAllocRetryMax);
    }
    if (w.logged || w.frames < kRigOutLogFrames || !rigLogOk()) return;
    w.logged = true;
    std::ostringstream os;
    os << "RT: " << rigOwnerText(key) << " -- ";
    if (why == RIG_OUT_SLOT)
        os << faces << " of " << all_faces << " rigged faces (" << tris << " of " << all_tris
           << " triangles) left out of ray-traced shadows for " << w.frames
           << " frames: their mesh rows fail the RT slot check (index range / CPU mirror)";
    else
    {
        os << "out of ray-traced shadows for " << w.frames << " frames (" << faces << " faces, " << tris << " triangles): ";
        if (why == RIG_OUT_ALLOC)
            os << "its BLAS allocation failed (" << w.allocFails << " attempts; next retry in "
               << (w.retryAt > mFrame ? w.retryAt - mFrame : 0u) << " frames, backing off up to every "
               << kRigAllocRetryMax << ")";
        else
            os << "waiting on the per-frame BLAS build-scratch budget";
    }
    LL_WARNS("VKRT") << os.str() << LL_ENDL;
}

void LLCleanRTVK::rigNoteIn(uint64_t key)
{
    const auto it = mRigWait.find(key);
    if (it == mRigWait.end()) return;
    if (it->second.logged)
        LL_INFOS("VKRT") << "RT: " << rigOwnerText(key) << " -- back in ray-traced shadows after " << it->second.frames
                         << " frames out" << (it->second.allocFails ? " (" + std::to_string(it->second.allocFails)
                                                                      + " failed allocations)" : std::string())
                         << LL_ENDL;
    mRigWait.erase(it);
}

void LLCleanRTVK::noteRigInvalid()
{
    // GPU rig copy check (#692): rigged faces whose skinned positions did not come from the
    // main cull's draw of the same slot this frame were collapsed (no RT shadow from them).
    // Normal for a frame or two (LOD / step switches, skin cache refills); logged once it
    // persists (~2 s of readbacks), at most every 30 s.
    // 2026-10-13: faces the cull drew NOWHERE this frame are counted apart (mRigUndrawn, the
    // `rt_rigUndrawn` hitch counter): the persistent "213 faces for 15085 frames" warning was
    // those -- avatars that are RT candidates only for a traced probe capture whose realtime job
    // did not pull them in (and the phase-2 rigged capacity drop) -- not a slot mismatch.
    LLVKHitch::count("rt_rigUndrawn", mRigUndrawn);
    mRigInvalidFrames = mRigInvalid ? mRigInvalidFrames + 1u : 0u;
    if (mRigInvalidFrames < 120u) return;
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now < mRigInvalidLogNext) return;
    mRigInvalidLogNext = now + 30.0;
    std::ostringstream os;
    os << "RT: " << mRigInvalid << " rigged faces failed the rig copy check for " << mRigInvalidFrames
       << " frames running (collapsed: no ray-traced shadow from them; the main cull drew them from another slot / source)";
    const uint32_t id = mRigInvalidId ? mRigInvalidId - 1u : kSentinel;
    const LLCleanRendererVK& R = mR;
    const size_t inst_n = R.mCpuInstances.size() / kInstBytes;
    if (id != kSentinel && id < inst_n)
    {
        const uint8_t* in = R.mCpuInstances.data() + (size_t)id * kInstBytes;
        const uint32_t skin = rd32(in + kInstSkin);
        const uint32_t* meta = reinterpret_cast<const uint32_t*>(R.mCpuSkinMeta.data());
        const size_t meta_words = R.mCpuSkinMeta.size() / 4;
        uint32_t av = kSentinel;
        if (skin != kSentinel && (size_t)skin * kSkinMetaWords < meta_words) av = meta[(size_t)skin * kSkinMetaWords];
        os << "; e.g. instance " << id << " of "
           << rigOwnerText(((av < kAvVisSlots ? (uint64_t)av : (0x100000000ull | skin))) << 8);
    }
    LL_WARNS("VKRT") << os.str() << LL_ENDL;
}

uint64_t LLCleanRTVK::pressureFloor() const
{
    return std::max<uint64_t>(256ull << 20, rtAuxBytes() / 6 * 10 + (32ull << 20));
}

bool LLCleanRTVK::dropRiggedForPressure()
{
    if (mRigPressureDrop || mRigDropped || mRigBytes == 0) return false;
    mRigPressureDrop = true;   // recordBuild retires them (and logs) on its next frame
    return true;
}

bool LLCleanRTVK::yieldForPressure()
{
    const uint64_t held = totalBytes();
    const uint64_t floor = pressureFloor();
    const uint64_t cap = std::max(floor, held / 2);
    if (held <= floor || budgetBytes() <= cap) return false;   // nothing (more) to give
    mPressureCap = cap;
    LL_WARNS("VKRT") << "RT: VRAM pressure -- budget capped at " << (mPressureCap >> 20) << " MB (held "
                     << (held >> 20) << " MB, working buffers " << (rtAuxBytes() >> 20) << " MB)" << LL_ENDL;
    return true;
}

void LLCleanRTVK::clearPressure()
{
    if (!pressureActive()) return;
    mPressureCap = 0;
    if (mRigPressureDrop) { mRigPressureDrop = false; mRigDropped = false; }   // (the ladder alone drops them)
    LL_WARNS("VKRT") << "RT: VRAM pressure lifted -- budget back to " << (budgetBytes() >> 20) << " MB" << LL_ENDL;
}

void LLCleanRTVK::clearPressureCap()
{
    if (!mPressureCap) return;
    mPressureCap = 0;
    LL_INFOS("VKRT") << "RT: VRAM pressure cap lifted -- budget back to " << (budgetBytes() >> 20) << " MB" << LL_ENDL;
}

void LLCleanRTVK::clearRiggedDrop()
{
    if (!mRigPressureDrop) return;
    mRigPressureDrop = false;
    mRigDropped = false;   // (the ladder alone drops them)
    LL_INFOS("VKRT") << "RT: VRAM pressure lifted -- avatars may enter RT again" << LL_ENDL;
}

void LLCleanRTVK::markCold(const char* why)
{
    if (mWarm) LL_INFOS("VKRT") << "RT: cold (" << why << ") -- sun shadows use the cascades until the BLAS builds catch up" << LL_ENDL;
    mWarm = false;
    mWarmFrames = mWarmSettled = 0;
}

uint64_t LLCleanRTVK::auxBytes() const
{
    if (!mCtx) return 0;
    uint64_t b = 0;
    const GpuBuffer* bufs[] = {&mScratch, &mTlasInst, &mRtGeom, &mSlotAddr, &mUsedBits, &mRigEntry, &mRigValid,
                               &mRigPos, &mStage[0], &mStage[1], &mStage[2], &mUsedRb[0], &mUsedRb[1], &mUsedRb[2],
                               &mTerrEntry, &mTerrPos, &mXform[0], &mXform[1], &mXform[2],
                               &mAuditRb[0], &mAuditRb[1], &mAuditRb[2], &mRtGeomVc,
                               &mGuard[0], &mGuard[1], &mGuard[2],
                               &mArenaGuardRb[0], &mArenaGuardRb[1], &mArenaGuardRb[2],
                               &mPoseSnap, &mPoseRb[0], &mPoseRb[1], &mPoseRb[2]};   // #1007
    for (const GpuBuffer* p : bufs) b += mCtx->allocBytes(p->memory);
    if (mClusterRT) b += mClusterRT->auxBytes();   // TODO(2b) #748
    return b;
}
