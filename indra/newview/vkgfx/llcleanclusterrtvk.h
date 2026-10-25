/**
 * @file llcleanclusterrtvk.h
 * @brief Cluster LOD (#642 / #748, OWNER: 2b): ray tracing on the cluster cut.
 *
 * Owned by LLCleanRTVK (created with it, RT on); does nothing unless RenderVKClusterLOD is on,
 * the cluster module is ready and RenderVKClusterRT != 0. Everything it builds is a per-instance
 * OVERRIDE of the legacy BLAS: TLAS pass A (clean_rt_instances.comp, binding 22) swaps the
 * override row's BLAS in for an instance the cluster raster draws for real, with the instance's
 * own transform, mask class and flags -- every other instance (and every instance whose override
 * is missing this frame for any reason) keeps the legacy per-slot BLAS. That makes "raster on
 * clusters, RT on the legacy BLAS" the plain fallback (RenderVKClusterRT 0, first-generation RT
 * cards, any failure).
 *
 * RT CUT (clean_cluster_rt.comp mode 0): its own selection pass over 1b's ClusterInstance list,
 * the shared cut rule at 2c's CLROLE_OTHER tolerance (control().roleTolPx: RenderVKClusterRT-
 * TolerancePx, never scaled by pressure), frustum / occlusion / cone independent (off-screen
 * casters), from a per-instance REFERENCE CAMERA that moves only when the live camera is
 * RenderVKClusterRTHysteresis x distance away from it (see the shader; it is also the
 * ClusterView::lod_cam of the projection, margin 0). Only static instances the cluster raster
 * draws for real (1b's kInstDraw); rigged: see below.
 * WHY NOT A 1b VIEW (integration with #749, which added roles and per-role arena slices): 1b's
 * recordSelect is a per-VIEW pass -- frustum / cone / Hi-Z culling into a view slice of
 * CL_B_SELECTED and draw emission -- while the RT cut needs every eligible instance's whole cut,
 * per instance (contiguous ranges for its BLAS, a per-instance reference camera and cut hash for
 * the rebuild / refit decision), with nothing culled. A CLROLE_OTHER | CLV_RT view would cull
 * off-screen casters and back-facing clusters and give no per-instance ranges, so the RT cut
 * keeps its own pass and its own cut arena (RtClCut). It shares 1b's PAGE_USAGE stamps with the
 * same value (LLCleanClusterVK::frame(), 9f4792d8ad) and records on graphics after the front-end
 * join (frame pipelining), before the frame's P, so 1c's next usage readback sees its stamps.
 *
 * TIERS (LLCleanClusterCaps RT, RenderVKClusterForceTier live):
 *   2 CLAS + PTLAS  as 1, and the TLAS is a partitioned TLAS (VK_NV_partitioned_acceleration_
 *                   structure): only changed records are written, only their partitions rebuild.
 *                   Its descriptor type differs from a KHR TLAS, so it is decided ONCE at RT
 *                   init (RenderVKClusterRT 2, the extension enabled): that session's TLAS is
 *                   always partitioned (also for legacy-only frames).
 *   1 CLAS          VK_NV_cluster_acceleration_structure: one CLAS per cluster of every page an RT
 *                   cut uses, built on the GPU when first wanted (fp32 positions decoded from the
 *                   pool, the pool's uint8 indices, geometry index = the cluster ref), in a fixed
 *                   arena of CLAS pages (64 explicit destinations of the per-CLAS worst case each;
 *                   freed when the pool slot changes content or no cut used it for kClasIdle
 *                   frames). Rigged instances' cluster BLAS are built every frame from their
 *                   cut's CLAS (implicit destinations, indirect: no CPU readback, no lag); static
 *                   ones are PERSISTENT (STATIC below).
 *   0 BLAS          per-cut KHR BLAS: the cut is read back (kFrameSlots frames), and an instance
 *                   whose cut hash changed is rebuilt under RenderVKClusterRTBuildsPerFrame and
 *                   the scratch budget (one geometry per cluster: fp32 positions + uint16 indices
 *                   expanded on the GPU, validated against the page table). The previous BLAS
 *                   stays until the new one is confirmed valid; each is used only while every
 *                   page of its cut is still resident (hits shade from the pool).
 *
 * HIT SHADING: a cluster hit resolves its cluster through rt_geom.z bit 30 (clean_rt_common.glsl
 * rtHitPrim / rtTriIndices): geometry index = the cluster ref (CLAS) or the entry of the
 * instance's hit-cut range (tier 0); vertices / UVs / normals / tangents are read from the page
 * pool (RT set binding 11) with the same QVertex decode -- the same material, UV and normal rules
 * as a legacy hit, on the RT cut's geometry.
 *
 * MEMORY: CLAS arena chunks + cluster BLAS arena + the static BLAS heap (VKMEM_RT_AS) and the
 * working buffers (VKMEM_RT_AUX) are fixed-size, grown in steps, under RenderVKClusterRTMemoryMB (0 = a third of the RT budget,
 * 64 MB .. 3 GB; LLCleanRTVK keeps that share out of its avatar-drop rule); the AS arenas are
 * allocated exactly. The rigged arena + its CLAS are sized from their demand FIRST; the static
 * CLAS arena from the static demand (pages the admitted cuts used + wanted) in what is left, and
 * the static ADMISSION threshold keeps the admitted demand inside it (clean_cluster_rt.comp
 * header). Tier-0 BLAS live in the RT arena (LLCleanRTVK::arenaAlloc) and count against
 * the RT budget. Live / cap / peak in statsLine. Failed allocations: the tier steps down for 600
 * frames (never a crash); instances keep the legacy BLAS meanwhile.
 *
 * STATIC (#748 static pass). What static instances cost was paid every frame or in bursts:
 *   - CLAS ARENA IN CHUNKS (kClasChunkPages pages each, chunk table in binding 33): the arena
 *     grows by adding chunks (kClasChunksPerFrame per frame at most), so growth never drops a
 *     CLAS and never allocates hundreds of MB in one frame (the single re-created arena cost
 *     6-12 ms of CPU per step and every CLAS with it). A pool growth keeps the CLAS (the per-slot
 *     tables are copied on the GPU). It is reset only when the statics left the cluster path
 *     (~5 s) or the room stayed under it (~2 s).
 *   - PERSISTENT STATIC CLUSTER BLAS (RenderVKClusterRTStaticPersist, default on): every
 *     static instance keeps its cluster BLAS in a heap block (explicit destinations, size
 *     classes of 1..2048 clusters, per-class free stacks + a bump pointer on the GPU, binding
 *     34). Mode 4 KEEPS it while the instance's cut hash, mesh and every CLAS it references
 *     (per-CLAS-page build stamps) are unchanged; otherwise rebuilds it into a new block (the
 *     old one freed after the frame's allocations, mode 21). While a new cut is not ready (a
 *     CLAS missing, no block) the previous BLAS keeps tracing as long as no CLAS was freed since
 *     it was built -- no flip to the legacy BLAS. A still camera builds no static BLAS. The heap
 *     is sized from the read-back live units (x2, within the cap), grown / re-packed (a new
 *     epoch: one frame of full builds) on missing blocks, shrunk after ~10 s at under a quarter.
 *     PREFER_FAST_TRACE (rarely rebuilt). Not done: compaction (the classes waste <= 1.5x; the
 *     build writes the real sizes, a MOVE_OBJECTS pass could pack them).
 *   - ADMISSION in one step: mode 0 counts the CLAS pages used + wanted per importance bucket;
 *     a raise picks the threshold whose buckets fit 4/5 of the arena (raiseStaticMin).
 *
 * RIGGED (#747 + #748 integration; RenderVKClusterRTRigged, default 1). An avatar 2a switched to
 * the cluster raster (its rigged instances carry CLI1B_DRAW) is traced on ITS CLUSTER CUT:
 *   1. RT CUT (mode 0): its rigged instances take part with the MAIN VIEW's projection --
 *      cl_views[main view] (LLCleanClusterVK::mainView): the 2c LOD camera + margin, held
 *      px_scale and post-controller tolerance -- and 1b's posed-bound margin (clRigMargin;
 *      LOD0 when the margins are missing, as 1b), but no frustum / cone / Hi-Z test. Over the
 *      clusters the raster draws it is the SAME cut (the cut rule is per group; culling never
 *      changes it), off-screen limbs get the cut the same rule gives them. Not the RT
 *      tolerance on purpose: any other projection puts the traced surface up to the larger
 *      cut's error from the rasterized receiver -- the self-shadow mismatch again, only smaller.
 *      (#1008, step 6: the near band keeps exactly this; mid / far avatars may trace a coarser cut.)
 *   2. RT SKINNED ARENA (binding 26; also the RT consumer set's binding 13): mode 9 skins every
 *      cluster of those cuts with 2a's math (clean_cluster_preskin.comp, statement for
 *      statement) from this frame's palettes (the renderer's SkinBlock buffer, set 1 binding
 *      28), eye-relative to the palettes' eye: position (fp32, the AS build input, stride 16)
 *      + the skinned normal per vertex, a header {ref, vpage, k | tris << 16, instance} per slot.
 *      SHARED SKINNED CUT FIRST (CLAS tiers; #702 integration): 1b's main view selects the whole
 *      un-culled cut of every switched avatar a secondary pass draws (the rig lists) and 2a
 *      skins + welds it once into SKIN_OUT. Mode 11 counts each rigged instance's skinned
 *      entries of the main slice and hashes their refs; when count and hash equal its RT
 *      cut's (the same set, whatever the inputs did), mode 0 marks the instance SHARED and mode 12
 *      scatters those entries into its slot headers ({ref, SKIN_OUT slot, k | tris << 16,
 *      instance | bit 31}); mode 9 COPIES those vertices into the slot (relative to the RT
 *      origin, #748 perf pass 2: the CLAS and the hits read the arena, binding 14 is unused) --
 *      one skinning, welded like the raster. Only the instances the
 *      shared cut does not cover completely (an avatar in no rig list: RenderVKClusterViewDraw
 *      bits off, its passes' pipelines missing, an off-screen RT-only caster; a slice or arena
 *      overflow) and tier 0 (its BLAS keep the harvested cut's topology across frames; the
 *      shared cut is this frame's) are skinned here, unwelded (seams within
 *      RenderVKRigWeldDistance of the raster's). Kept separate rather than RT-only: RT then
 *      never depends on the secondary-pass settings.
 *   3. ACCELERATION STRUCTURES, every RT tier:
 *      CLAS (tiers 1 / 2): the arena is handed out PER AVATAR, whole or not at all (mode 0 sums
 *        each avatar's cuts, mode 13 admits avatars while they fit, mode 14 gives each instance
 *        its contiguous range inside its avatar's): an avatar that does not fit keeps the legacy
 *        BLAS and gets no CLAS, every other one is covered completely (the per-avatar
 *        legacy-skin skip); mode 9 writes one CLAS build argument per slot
 *        (vertex stride 16, the pool's uint8 indices, geometry index = the entry k); ONE
 *        explicit-destination CLAS build per frame over the slots written this frame (#748 perf
 *        pass 2: every slot has its own CLAS destination; an avatar whose skinned WORLD positions
 *        and cut are unchanged -- FrameIn::rigStill + its signature -- KEEPS its slots, vertices
 *        and CLAS: no copy, no build; positions are relative to the RT ORIGIN, mRigOrigin, so a
 *        camera move alone rebuilds nothing); mode 4 takes those CLAS addresses into the
 *        instance's cluster BLAS, built with the static ones. Same-frame cut, same-frame pose
 *        (or a provably identical one): no lag at all. The RT cut of an instance the shared rig
 *        cut covers whole is DERIVED from it (mode 11's gather; no page walk).
 *      per-cut BLAS (tier 0, no NV extension): the KHR BLAS (one geometry per cluster: the
 *        geometry index is k, as above) is BUILT from the harvested cut (kFrameSlots frames
 *        old: the LOD lags, never the pose) into a persistent arena range (CPU allocator),
 *        and REFIT every frame from this frame's skinning of that cut (mode 9 jobs). Rebuilt
 *        when the harvested cut changes (RenderVKClusterRTBuildsPerFrame) and every
 *        kT0RigRebuild frames (refit quality). A slot whose page was evicted since collapses
 *        to one point and the row is not used that frame (legacy fallback).
 *   4. OVERRIDE ROWS (mode 5 / mode 10): {BLAS, first arena slot, OVR_RIG | a pool vertex}. TLAS
 *      pass B (clean_rt_instances.comp) gives such an instance its OWN record -- a translation
 *      by the palettes' eye, rigged mask class, never facing-culled -- and rt_geom
 *      RT_GEOM_CLUSTER_RIG: hits read positions / skinned normals from the arena (UVs from the
 *      pool), every rigged rule of the consumers applies (rt_geom.z bit 31).
 *   5. NEVER BOTH, NEVER NEITHER: the rigged plan of LLCleanRTVK keys the switched faces into
 *      their own groups (owner, mask class, switched). Pass B gives a switched group its
 *      members' cluster records only when EVERY member has one this frame (mode 4 of
 *      clean_rt_instances.comp; refit frames: every record may become active), else the legacy
 *      group record as before -- per group, never half. The legacy rigged BLAS is still built /
 *      refit for switched avatars (it is the fallback). tlasRigMap: per-entry group heads +
 *      the "legacy excluded / fallback" counters of the Cluster RT line.
 *   6. #1008 RIGGED RT CADENCE (clean_cluster_rt.comp header; RenderVKClusterRTRigCadence, 0 =
 *      1..5 exactly as above, the A/B and the default). ONLY for avatars that never self-shadow
 *      in RT (FrameIn::rigNoSelf = classifyNoSelf's tag: their receivers skip their own records
 *      on every frame), so aging / coarse geometry never shows as self-shadow, and the self-shadow
 *      state does not toggle with the period (the first cut let untagged avatars and slotless
 *      animesh go stale / coarse: flicker). Every other avatar is near / exact every frame; with
 *      no tagged avatar traced the cadence is off for the frame. For a tagged avatar the RT cut
 *      and the rigged CLAS no longer follow the raster every frame: it has a distance BAND (near / mid / far: RenderVKClusterRTRigNearM /
 *      FarM) with a selection PERIOD (RenderVKClusterRTRigPeriodNear / Mid / Far, staggered by
 *      joint-table slot, capped by RenderVKClusterRTRigMaxStale) and an RT tolerance MULTIPLIER
 *      over the main view's (RenderVKClusterRTRigTolNear / Mid / Far). Near = period 1, x1 by
 *      default: step 1 (caster = receiver) and the shared welded skinning hold there unchanged.
 *      Mid / far may trace a coarser cut, own-skinned (unwelded), capped at 0.1 m past the raster
 *      error (inside the receiver-identity probe window, so its receivers still skip it). Between its
 *      selections an avatar's instances copy their cuts forward (st[ri][5]) and the avatar is
 *      KEPT (RP_KEEP bit 1): no skinning, no CLAS -- its RT pose lags by at most the period. A due
 *      mid / far avatar whose cut set did not change is REFRESHED: same slots and headers, mode 9
 *      re-skins each slot and rebuilds its CLAS only when a vertex moved more than
 *      RenderVKClusterRTRigPoseEps x the cut's error there (or the CLAS is MaxStale frames old).
 *      Arrival, a teleport / origin move, a frame gap, an arena re-created: every avatar due.
 *      Hit consistency: a slot's CLAS and its arena words (the vertices + header ref hits read)
 *      are only written together, by the same mode-9 record -- a skipped / kept slot changes
 *      neither, a re-placed one rewrites both -- so no double buffer is needed.
 *   The #747 interim receiver lift (clean_rt_shadow_mask.comp maskClusterRigLift) only lifts
 *   where a face flagged RT_GEOM_W_CLUSTER_RIG is near -- pass B sets that flag only on a
 *   switched face traced on its LEGACY geometry this frame -- so it applies exactly to the
 *   receivers whose occluder is still legacy, and is off where the cluster cut casts.
 *   STILL READING LEGACY SKINNED VERTICES (for the later "drop the legacy pre-skin of switched
 *   avatars" step): the legacy rigged BLAS of switched avatars (rig copy -> build / refit, the
 *   fallback above), the RT terminator-triangle raster pass (#664, drops switched avatars), and
 *   every non-RT legacy pass 2a lists (shadow maps / probes / mirror / pick).
 *
 * Threading: render thread (LLCleanRTVK::recordBuild).
 */
#pragma once

#include "vk_device.h"
#include "vk_descpool.h"
#include "vk_pipeline.h"   // PipelineJobSlot
#include "../llavatarslots.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

class LLCleanRTVK;
class LLCleanClusterVK;

class LLCleanClusterRTVK
{
public:
    // = clean_cluster_rt.comp CNT_*
    enum : uint32_t
    {
        kCntCut = 0, kCntCutChange, kCntWant, kCntClasArgs, kCntBlasInfos, kCntEligible, kCntReady,
        kCntOverflow, kCntClasFreed, kCntClasAlloc, kCntClasFail, kCntTris, kCntNotReady, kCntPtlasOps,
        kCntRefResets, kCntT0Invalid,
        // rigged (= clean_cluster_rt.comp CNT_RIG*)
        kCntRig, kCntRigWant, kCntRigEligible, kCntRigReady, kCntRigOverflow, kCntRigInvalid, kCntRigTris,
        kCntRigShared,
        // (#748 perf pass) static CLAS demand, per-avatar admission, static admission / reuse
        kCntClasUsed, kCntRigAvIn, kCntRigAvOut, kCntStaticSkip, kCntReused,
        // (#748 perf pass 2) rigged: avatars kept (no CLAS build), CLAS built, RT cuts derived
        kCntRigKeptAv, kCntRigArgs, kCntRigDerived,
        // (#748 static pass) persistent static BLAS, flips, the admission histogram (= CNT_SB_* ...)
        kCntSbInfos, kCntSbKept, kCntSbFail, kCntSbOld, kCntSbUnits, kCntSbPend, kCntStaticFlip,
        kCntImpHist,                                   // [kImpBuckets]
        kCntSbLeak = kCntImpHist + 20,
        kCntRigRebuildSlots = 60,                      // (= CNT_RIG_REBUILD_SLOTS) slots of the admitted, not-kept avatars
        kCntPtlasGlobal = 61,                          // (= CNT_PTLAS_GLOBAL) spatial PTLAS writes into the global partition
        kCntRbSbBump = 62,                             // readback only: the heap's bump pointer
        kCntRbClasTop = 63,                            // readback only: the CLAS free-stack top (live pages = cap - top)
        // #1008 rigged RT cadence (= CNT_RIG_SEL ...; past the original 64-word block, read back
        // at kRbExtra)
        kCntRigSel = 64, kCntRigReused, kCntRigDueAv, kCntRigRefreshAv, kCntRigPoseSkip, kCntRigStaleMax,
        kCntRigRaster, kCntRigRefreshSlots,
        // #1011 CLAS reuse / skinning source (= CNT_RIG_REUSE_CMP ...)
        kCntRigReuseCmp = 72, kCntRigReuseSkip, kCntRigOwnSkin, kCntRigCopied,
        kCntWords = 80
    };
    static constexpr uint32_t kImpBuckets = 20;          // = CNT_IMP_BUCKETS (1.5 per octave from 2^-14)
    static_assert(kCntSbInfos == 32 && kCntSbLeak == 59, "= CNT_SB_INFOS / CNT_SB_LEAK");
    // ---- STATIC (#748 static pass; header "STATIC") ----
    static constexpr uint32_t kClasChunkPages = 32;      // CLAS pages per arena chunk (~12.5 MB)
    static constexpr uint32_t kMaxClasChunks = 256;      // (8192 pages, ~3.2 GB: past the 3 GB cluster RT cap)
    static constexpr uint32_t kClasChunksPerFrame = 2;   // chunks allocated per frame at most
    static constexpr uint32_t kSbClasses = 22;           // persistent static BLAS size classes
    static constexpr uint32_t kSbStackCap = 4096;        // free blocks per class
    static constexpr uint32_t kSbPendCap = 16384;        // blocks freed per frame
    static constexpr uint32_t kStaticCpuWords = 1024;    // binding 33 (SC_*)
    static constexpr uint32_t kStateBytes = 96;          // per renderer instance id (ST_STRIDE 6 uvec4; #1008: [5] rigged cut reuse)
    static constexpr uint32_t kMaxCutPerInst = 2048;     // = CL_RT_SHARED_CUT (more: legacy BLAS)
    static constexpr uint32_t kCutCap = 1u << 17;        // cut arena entries
    static constexpr uint32_t kClasPagesPerFrame = 64;   // CLAS pages built per frame
    static constexpr uint32_t kClasIdle = 60;            // frames unused before a CLAS page is freed
    static constexpr uint32_t kClasMinPages = 16;        // static CLAS arena floor (pages)
    static constexpr uint32_t kMaxT0Builds = 64;         // tier-0 BLAS builds per frame (hard cap)
    static constexpr uint32_t kT0TrisPerFrame = 400000;  // tier-0 triangles built per frame
    static constexpr uint32_t kT0KeepFrames = 120;       // tier-0 BLAS of an instance unseen this long go
    static constexpr uint32_t kHitCutCap = 1u << 18;     // tier-0 persistent hit-cut entries
    static constexpr uint32_t kPartSize = 512;           // PTLAS instances per partition (id-order partitioning)
    // SPATIAL partitioning (RenderVKClusterPTLASPartition 1): instances hashed by the grid cell of
    // their translation into kPartSpatial partitions (or fewer: maxPartitionCount), each holding
    // at most mPartMax; a full partition / a large or disabled instance goes to the GLOBAL partition
    // (capacity = every instance, so a write never overflows). See ptlasEnsure.
    static constexpr uint32_t kPartSpatial = 1024;
    static constexpr uint32_t kFrameSlots = 3;
    // rigged (RIGGED above)
    static constexpr uint32_t kRigHdrBase = 16;          // = RIG_HDR_BASE: arena words before the slot headers
    static constexpr uint32_t kRigVtxWords = 64 * 4;     // per slot: 64 vertices x (xyz, normal)
    static constexpr uint32_t kRigIdxWords = 192;        // per slot (tier 0): 128 x 3 uint16
    static constexpr uint32_t kRigMinSlots = 1024;       // arena slots
    static constexpr uint32_t kRigMaxSlots = 1u << 17;
    static constexpr uint32_t kRigSlotStep = 1024;       // arena capacity steps
    static constexpr uint32_t kRigAvRows = kAvatarSlotCap + 1;   // = RIGAV_ROWS: joint-table slots + "none"
    static constexpr uint32_t kRigParamWords = 160;      // RtClRigCpu parameter block (RP_*; RP_STILL 64, RP_RT_AV 96, RP_NOSELF 128)
    static constexpr uint32_t kT0RigRebuild = 600;       // tier 0: rebuild a refit BLAS this often (frames)
    static constexpr uint32_t kTlasRigHdr = 8 + kAvatarSlotWords;   // = clean_rt_instances.comp CLRIG_HDR (40)
    static constexpr uint32_t kRigAvStride = 8;          // = RIGAV_STRIDE (per-frame per-avatar words)
    static constexpr uint32_t kRigAvPersist = 4;         // = RIGAV_PERSIST_WORDS (kept across frames)
    static constexpr uint32_t kRigAvSched = 4;           // = RIGAV_SCHED_WORDS (#1008: band, last selection frame; kept)
    static constexpr uint32_t kRigAddrWords = 5;         // rig_addr words per slot: destination (2), size, stamp, (#1008) build frame
    static constexpr float    kRigOriginMove = 8.f;      // RT origin moves when the eye is this far (m)

    explicit LLCleanClusterRTVK(LLCleanRTVK& rt);
    ~LLCleanClusterRTVK();

    // Entry points + limits of the NV extensions (when enabled), the module set layout.
    // ptlas_wanted: RenderVKClusterRT >= 2 at RT init -> the RT consumer set's binding 0 is a
    // PARTITIONED acceleration structure for the whole session (ptlasLayout()).
    bool init(bool ptlas_wanted);
    void shutdown();                     // device idle

    struct FrameIn
    {
        LLCleanClusterVK* cluster = nullptr;   // null / not ready = cluster RT inactive
        bool     clusterOn = false;            // RenderVKClusterLOD and the module ready
        uint32_t frameSlot = 0;                // renderer frame slot % 3 (= the cluster module's)
        float    cam[3] = {};                  // main camera (world)
        float    znear = 0.1f;
        float    px_scale = 0.f;               // 0.5 * height * P[1][1]
        uint32_t riCap = 0;                    // renderer instance ids the TLAS covers
        uint64_t rtBudget = 0;                 // LLCleanRTVK::budgetBytes()
        bool     legacyRigged = true;          // LLCleanRTVK's rigged plan runs (pass B has entries; also
                                               // switched-only, cluster-only groups while the legacy
                                               // avatar class is dropped over budget)
        // #748 perf pass 2, PERSISTENT RIGGED CLAS: per joint-table slot, its skinned WORLD
        // positions provably did not change since the previous viewer frame (the GPU pose held,
        // the root still, skin meta clean: LLCleanAnimStore::slotWorldClean); valid = that proof
        // exists this frame (RT ran last viewer frame, the anim store built this one).
        uint32_t rigStill[kAvatarSlotWords] = {};
        bool     rigStillValid = false;
        // RT AVATAR SET (2026-10-14): per joint-table slot, LLCleanRTVK's rigged plan traces this
        // avatar (the legacy RT rules: drawn by some CPU-decided pass or a ray-traced capture
        // candidate, and not a no-shadow reduced-tier wearer). Only these avatars' switched
        // instances get an RT cut, arena slots and CLAS / per-cut BLAS (RP_RT_AV) -- the plan
        // never asks pass B for the others, so their cluster RT data was built for nobody.
        // Default: every avatar.
        uint32_t rigTraced[kAvatarSlotWords];
        // #1008: per joint-table slot, the avatar never self-shadows in RT this frame
        // (LLCleanRTVK::classifyNoSelf's kAvVisRtNoSelf: its records carry the owner tag, its
        // receivers skip its own geometry). Only these avatars' RT cut / CLAS may be coarse or
        // stale (RP_NOSELF); with none traced the cadence is off for the frame. Default: none.
        uint32_t rigNoSelf[kAvatarSlotWords] = {};
        FrameIn() { for (uint32_t& w : rigTraced) w = ~0u; }
    };
    // CPU plan (after the legacy plans, before the scratch is sized): settings, tier, readback
    // harvest, tier-0 build plan, buffers. Returns the AS build scratch this frame needs
    // (each request aligned to salign; includes the PTLAS build when ptlasActive()).
    VkDeviceSize plan(const FrameIn& in, VkDeviceSize salign);
    // The module set of this frame slot, once per frame BEFORE record() / ptlasBuild() (after
    // LLCleanRTVK sized its TLAS record buffer: the PTLAS diff reads it).
    void writeSets(const GpuBuffer& records);
    // Record, after the RT leading barrier + uploads, before TLAS pass A: selection, CLAS /
    // BLAS builds, the override rows. `mark` = the renderer's GPU sub-timer.
    void record(VkCommandBuffer cmd, const std::function<VkDeviceAddress(VkDeviceSize)>& takeScratch,
                const std::function<void(VkCommandBuffer, const char*)>& mark);
    // pass A binding 22 (null = no override this frame: bind the zero dummy).
    VkBuffer overrideBuffer() const { return mActive ? mOvr.buffer : VK_NULL_HANDLE; }
    // RT consumer set bindings 11 (page pool) / 12 (tier-0 hit cut) (null = dummy).
    VkBuffer poolBuffer() const { return mActive ? mPoolBuf : VK_NULL_HANDLE; }
    VkBuffer hitCutBuffer() const { return mHitCut.buffer; }

    // ---- partitioned TLAS (tier 2) ----
    bool ptlasLayout() const { return mPtlasLayout; }
    // Sized for `cap` records (call after the TLAS record buffer is sized). False = no PTLAS
    // this frame (the frame's TLAS is invalid: RT consumers fall back).
    bool ptlasEnsure(uint32_t cap);
    VkDeviceAddress ptlasAddress() const { return mPtlasAddr; }
    VkDeviceSize ptlasScratch() const { return mPtlasScratch; }
    // Records (count) -> write ops -> PTLAS build (in place). In place of the KHR TLAS build.
    void ptlasBuild(VkCommandBuffer cmd, const GpuBuffer& records, uint32_t count,
                    const std::function<VkDeviceAddress(VkDeviceSize)>& takeScratch);

    // ---- rigged (RIGGED above) ----
    // RenderVKClusterRT != 0 and RenderVKClusterRTRigged != 0: LLCleanRTVK keys the switched
    // avatars' faces into their own rigged groups (the per-group switch of pass B).
    bool riggedWanted() const;
    // RT consumer set binding 13 (the RT skinned arena; null = dummy: no record references it).
    VkBuffer rigBuffer() const { return mRigOn ? mRig.buffer : VK_NULL_HANDLE; }
    // RT consumer set binding 14: 2a's SKIN_OUT while slots may take the shared skinned cut.
    VkBuffer rigSkinBuffer() const { return mRigOn && mRigShared ? mSkinOutBuf : VK_NULL_HANDLE; }
    // After plan(), before LLCleanRTVK writes its sets: this frame slot's group map for TLAS
    // pass B (clean_rt_instances.comp binding 23) sized for `entries` rig entries, its counters
    // and group flags zeroed. Returns the per-entry head words to fill (head + 1, 0 = a legacy
    // group), null = no map this frame (binding 23 = the zero dummy: every group legacy).
    uint32_t* tlasRigMap(uint32_t entries);
    VkBuffer tlasRigBuffer() const { return mTlasRigOn ? mTlasRig[mSlot].buffer : VK_NULL_HANDLE; }
    // After TLAS pass B (same command buffer, before the TLAS build): pass B's count of switched
    // faces it traced on their LEGACY geometry (group map word 2, bit 31 = valid) -> the RT
    // skinned arena's word 2 (RT consumer binding 13), which clean_rt_shadow_mask.comp reads to
    // skip the #747 receiver-lift probe ray when no such face exists this frame. Only while
    // tlasRigBuffer() is bound; its source writes must be visible to transfer (the caller's
    // barrier); records the barrier to compute after the copy.
    void recordLiftFlag(VkCommandBuffer cmd);
    // Legacy-skin skip (LLCleanRTVK::harvestClusterCoverage): frame slot `slot`'s group map as
    // TLAS pass B left it (heads, then the per-group "fell back" flags) for `entries` rig
    // entries -- call when that slot comes round (its fence was waited), BEFORE plan() harvests
    // and tlasRigMap() rewrites it; null = no map that frame / a different size.
    const uint32_t* tlasRigWords(uint32_t slot, uint32_t entries) const
    {
        if (slot >= kFrameSlots || !mTlasRigValid[slot] || !mTlasRig[slot].mapped
            || mTlasRig[slot].size < ((VkDeviceSize)kTlasRigHdr + 2ull * entries) * 4)
            return nullptr;
        return static_cast<const uint32_t*>(mTlasRig[slot].mapped);
    }
    // The RT tier of the last plan() (-1 = cluster RT inactive / off): 0 per-cut BLAS, 1 / 2 CLAS.
    int32_t tierNow() const { return mActive ? mTier : -1; }
    // Rigged avatars given RT skinned arena slots by the last harvested plan (mode 13,
    // CNT_RIG_AV_IN): the avatars whose cluster RT data is ready (LLCleanRTVK's TLAS
    // consistency check compares it with the avatars pass B emitted cluster records for).
    uint32_t rigAvatarsIn() const { return mLast[kCntRigAvIn]; }

    void releaseAll();                   // through the RT retire ring
    void abortPlan();                    // the RT frame was skipped after plan()
    void dropAfterLostFrame();           // the frame that recorded our builds never ran
    uint64_t asBytes() const;            // CLAS + cluster BLAS + PTLAS arenas (tier-0 BLAS: RT arena)
    uint64_t auxBytes() const;           // working buffers
    std::string statsLine() const;
    // 2026-10-14 VRAM by path: capacity vs live of the cluster RT pools (rigged arena slots + their
    // explicit CLAS destinations, static CLAS pages, PTLAS) and the live CLAS bytes per triangle.
    std::string memLine() const;

private:
    struct Range { uint32_t start = 0, count = 0; };
    struct RangeAlloc
    {
        uint32_t cap = 0;
        std::vector<std::pair<uint32_t, uint32_t>> free;   // (start, count), sorted
        void reset(uint32_t c) { cap = c; free.assign(1, {0u, c}); }
        bool alloc(uint32_t n, uint32_t& out);
        void release(uint32_t start, uint32_t n);
    };
    struct T0Blas
    {
        VkAccelerationStructureKHR as = VK_NULL_HANDLE;
        int chunk = -1; VkDeviceSize off = 0, size = 0;   // LLCleanRTVK::Alloc
        VkDeviceAddress addr = 0;
        Range cut;                       // hit-cut entries
        uint64_t hash = 0;
        uint32_t valid = 0xFFFFFFFFu;    // validity word (kNone = confirmed)
        uint32_t frame = 0;
    };
    struct T0Inst
    {
        T0Blas cur, pend;
        uint32_t mesh = 0, gen = 0;
        uint32_t lastSeen = 0;
        uint64_t want = 0;               // cut hash of the latest readback
        uint32_t wantBase = 0, wantN = 0;   // its cut in mT0Cut (this frame's harvest)
    };
    struct T0Build { uint32_t ri; uint32_t validIdx; };

    bool ensureBuf(GpuBuffer& b, VkDeviceSize need, VkBufferUsageFlags usage, bool host, const char* name,
                   bool as_storage = false);
    void retire(GpuBuffer& b);
    bool ensurePipeline(LLCleanClusterVK& cl);
    void writeSet(uint32_t slot, LLCleanClusterVK& cl);
    void harvest(uint32_t slot);
    void planT0(VkDeviceSize salign, uint64_t mem_cap);
    bool planClas(VkDeviceSize salign, uint64_t mem_cap, uint32_t pool_pages);
    void planStaticBlas(VkDeviceSize salign, uint64_t budget);   // the persistent static BLAS heap
    void writeStaticCpu();               // binding 33 of this frame slot
    void raiseStaticMin();               // the admission threshold from the read-back histogram
    void releaseT0(T0Blas& b);
    void releaseClas();
    void releaseStaticBlas();
    void dispatch(VkCommandBuffer cmd, uint32_t mode, uint32_t groups);
    void dispatchIndirect(VkCommandBuffer cmd, uint32_t mode, VkBuffer args, VkDeviceSize off);
    void tierFail(const char* why);
    // rigged
    struct T0Rig
    {
        VkAccelerationStructureKHR as = VK_NULL_HANDLE;
        int chunk = -1; VkDeviceSize off = 0, size = 0;   // LLCleanRTVK::Alloc
        VkDeviceAddress addr = 0;
        Range slots;                     // arena slots (persistent while the BLAS lives)
        uint64_t hash = 0;               // the harvested cut it was built from
        uint32_t mesh = 0, gen = 0;
        uint32_t lastSeen = 0, builtFrame = 0;
        VkDeviceSize updScratch = 0;
        std::vector<VkAccelerationStructureGeometryKHR> geoms;
        std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
        std::vector<uint32_t> hdr;       // 4 words per slot {ref, vpage, k | tris << 16, 0} (build frame)
        uint64_t want = 0;               // latest harvested cut (hash; base / n in mT0Cut)
        uint32_t wantBase = 0, wantN = 0, wantFrame = 0;
    };
    struct T0RigRec { uint32_t ri, ii; bool build; VkDeviceSize scratch; };
    void planRig(LLCleanClusterVK& cl, int32_t tier, VkDeviceSize salign, uint64_t rig_budget);
    uint32_t rigSlotsWanted() const;     // arena slots the read-back demand asks for
    uint64_t rigBytesFor(uint32_t slots) const;   // device bytes of such an arena (+ its CLAS)
    bool planRigClas(VkDeviceSize salign, uint64_t rig_budget);
    void planT0Rig(LLCleanClusterVK& cl, VkDeviceSize salign, uint64_t rig_budget);
    bool ensureRigArena(uint32_t slots, bool tier0);
    void releaseT0Rig(T0Rig& r);
    void releaseRig();
    void unwindT0Rig();                  // this frame's tier-0 rig plan is not recorded

    LLCleanRTVK& mRT;
    VkDeviceContext* mCtx = nullptr;

    // pipeline (set 0 = the cluster set, set 1 = mDsl)
    VkDescriptorSetLayout mDsl = VK_NULL_HANDLE;
    VkDescriptorSetLayout mEmptyDsl = VK_NULL_HANDLE;    // set 0 of the PTLAS-only pipeline
    VkDescriptorSetLayout mClusterDsl = VK_NULL_HANDLE;   // the cluster module's layout the pipeline was made with
    VkPipelineLayout mLayout = VK_NULL_HANDLE;
    VkPipeline mPipe = VK_NULL_HANDLE;
    PipelineJobSlot mPipeJob;                            // mPipe's background build (ensurePipeline)
    VkPipeline mPipeStage = VK_NULL_HANDLE;              // its output, installed when it lands
    VkPipelineLayout mPtlasLayoutObj = VK_NULL_HANDLE;   // {empty, mDsl}
    VkPipeline mPtlasPipe = VK_NULL_HANDLE;              // kWorldClusterRtPtlasSpv
    struct OldPipe { VkPipeline pipe; VkPipelineLayout layout; uint32_t frame; };
    std::vector<OldPipe> mOldPipes;                      // destroyed kFrameSlots + 1 frames later
    uint32_t mPipeFailFrame = 0;
    VkDescPool mPool;
    VkDescriptorSet mSets[kFrameSlots] = {};
    VkDescriptorSet mClusterSet = VK_NULL_HANDLE;         // this frame's cluster set

    // NV entry points + limits
    PFN_vkVoidFunction mGetClasSizes = nullptr, mCmdBuildClas = nullptr;
    PFN_vkVoidFunction mGetPtlasSizes = nullptr, mCmdBuildPtlas = nullptr;
    uint32_t mClasScratchAlign = 256, mClasAlign = 256, mClasBlasAlign = 256, mMaxClusterGeomIndex = 0;
    uint32_t mMaxPartitions = 0;
    bool mClasOk = false;                // extension + entry points + limits
    bool mPtlasLayout = false;

    // frame state
    bool mActive = false;                // this frame records (override bound)
    int32_t mTier = -1;                  // tier recorded this frame
    int32_t mTierCap = 2;                // stepped down on failures
    uint32_t mTierRetryFrame = 0;
    uint32_t mFrame = 0, mSlot = 0;
    uint32_t mGpuFrame = 0;              // LLCleanClusterVK::frame() (GPU stamps: = 1b's page-usage frame)
    uint32_t mIdleFrames = 0;            // consecutive inactive frames (release after 120)
    bool mStateFill = true;              // zero the hysteresis state this frame
    uint32_t mInstN = 0, mRiCap = 0, mPoolPages = 0;
    float mCam[4] = {}, mPxScale = 0.f, mTol = 2.f, mHyst = 0.25f;
    VkBuffer mPoolBuf = VK_NULL_HANDLE;
    VkDeviceAddress mPoolAddr = 0;
    uint64_t mMemCap = 0;

    // buffers (VKMEM_RT_AUX unless noted)
    GpuBuffer mState, mInstRows, mCut, mOvr, mCnt, mDummy;
    // STATIC CUT REUSE (clean_cluster_rt.comp header): the previous epoch's cut arena (binding 32;
    // swapped with mCut after each record), the epoch counter, the epoch / tolerance / px_scale
    // the previous arena was written with (0 = none: no reuse next frame).
    GpuBuffer mCutPrev;
    uint32_t mCutEpoch = 0, mCutWritten = 0, mRtFlags = 0;
    float    mCutTol = 0.f, mCutPx = 0.f;
    uint32_t mClasBudgetPages = 0;     // static cut perf pass: the static CLAS budget (RenderVKClusterRTStaticMB) in pages
    // STATIC ADMISSION (RP_STATIC / RP_STATIC_MIN) and the demand-sized static CLAS arena
    bool     mStaticOn = true;           // RenderVKClusterRTStatic
    // Static instances take part in the RT cut THIS frame (CPU-exact: 1b's static instances drawn
    // for real; RenderVKClusterStatic is off by default). False: the static CLAS passes (modes 2 / 3,
    // the static CLAS build), the static BLAS heap passes (modes 22 / 21, the explicit build) and
    // -- once no static CLAS page is live -- the pool scan (mode 1) are not recorded.
    bool     mStaticFrame = false;
    uint32_t mStaticSkips = 0;           // frames the static RT passes were skipped (stats window)
    float    mStaticMin = 0.f;           // importance threshold (0 = every static instance)
    uint32_t mClasRoomCap = 0;           // CLAS pages the cap leaves room for (last plan)
    uint32_t mClasShrinkSince = 0;
    uint32_t mRigAvBase = 0;             // RP_AV_BASE: rig_of word of the per-avatar table
    uint32_t mRigAvRows = 0;             // per-instance rows before it (high-water, 4096 steps)
    bool     mRigBudgetCut = false;      // the rigged arena was cut below its demand by the budget
    bool     mLegacyRigged = true;       // FrameIn::legacyRigged of this frame
    uint32_t mTlasLegacyFlagged = 0;     // last harvested: switched faces traced on legacy geometry
    GpuBuffer mClasOfSlot, mClasFree, mClasWant, mClasStamp, mClasArgs, mClasDst, mClasSize, mDecode;
    GpuBuffer mClasRefs, mBlasInfo, mBlasAddr, mBlasOf;
    GpuBuffer mBlasArena;                // VKMEM_RT_AS (this frame's implicit cluster BLAS)
    // STATIC CLAS ARENA in chunks (VKMEM_RT_AS, kClasChunkPages pages each; chunk table in binding
    // 33): growth adds chunks -- nothing built moves, no CLAS is lost.
    std::vector<GpuBuffer> mClasChunks;
    uint32_t mClasGrowFrom = 0;          // first CLAS page of the chunks added this frame (mode 20)
    uint32_t mClasOverSince = 0;         // the arena has been past the room since (frame)
    // pool growth with the CLAS kept: the old per-slot tables, copied into the grown ones (record)
    VkBuffer mSlotCopyOf = VK_NULL_HANDLE, mSlotCopyStamp = VK_NULL_HANDLE;
    uint32_t mSlotCopyPages = 0;
    // PERSISTENT STATIC BLAS (header STATIC): the heap (VKMEM_RT_AS), its bookkeeping (binding 34,
    // device), the host parameters + chunk table per frame slot (binding 33).
    GpuBuffer mSbArena, mSbMeta, mStaticCpu[kFrameSlots];
    bool     mSbOn = false;              // static BLAS persistent this frame
    bool     mSbReset = true;            // zero the heap header (a new epoch) in record()
    bool     mSbSized = false;           // class sizes queried
    bool     mSbUnsupported = false;     // a size query returned 0: implicit builds as before
    uint32_t mSbEpoch = 1;
    uint32_t mSbUnits = 0;               // heap capacity (units)
    VkDeviceSize mSbUnit = 256, mSbScratch = 0;
    uint32_t mSbScratchMax = 0;          // the blas_max the scratch was queried for
    uint32_t mSbClassUnits[kSbClasses] = {}, mSbClassCount[kSbClasses] = {};
    uint32_t mSbCooldown = 0;            // frames before the read-back heap state is trusted again
    uint32_t mSbSmallSince = 0;          // the heap has been over 4x its need since (frame)
    uint32_t mSbRepacks = 0, mSbGrows = 0;
    uint32_t mSbIdle = 0;                // frames without a static on the cluster path (release at 120)
    uint32_t mSbRetryFrame = 0;          // a heap allocation failed: not before this frame
    uint32_t mSbLastRepack = 0;          // the frame of the last in-place repack
    bool     mSbMetaFresh = false;       // the bookkeeping was (re)created: zero it whole in record()
    // 10 s accumulation of the per-frame counters (statsLine: per-second / per-frame averages)
    struct StaticAcc { uint64_t frames = 0, built = 0, kept = 0, old = 0, fail = 0, flips = 0, notReady = 0, clasBuilt = 0; };
    StaticAcc mAcc, mAccShown;
    GpuBuffer mT0Jobs[kFrameSlots], mT0Rows[kFrameSlots], mT0Stage[kFrameSlots];   // host
    GpuBuffer mT0Idx, mHitCut, mT0Valid;
    GpuBuffer mTlasPrev, mPtlasOps, mPtlasCmd, mPtlas;   // mPtlas: VKMEM_RT_AS
    GpuBuffer mReadback[kFrameSlots];
    uint32_t mRbInst[kFrameSlots] = {};      // ClusterInstance rows copied (tier 0)
    bool     mRbCut[kFrameSlots] = {};       // cut arena copied (tier 0)
    bool     mRbValid[kFrameSlots] = {};
    std::vector<T0Build> mRbBuilds[kFrameSlots];   // tier-0 builds of that frame (validity readback)

    // CLAS state
    uint32_t mClasCap = 0;               // CLAS pages the arena holds
    uint32_t mClasPoolPages = 0;         // pool pages clas_of_slot covers
    VkDeviceSize mClasStride = 0, mClasScratch = 0, mBlasScratch = 0, mBlasArenaBytes = 0;
    uint32_t mClasMaxGeom = 0;           // maxGeometryIndexValue the sizes were queried with
    bool mClasReset = true;              // fill clas_of_slot + init the stack this frame
    uint32_t mBlasMax = 0;               // cluster BLAS per frame (= instance rows cap)

    // tier 0
    std::unordered_map<uint32_t, T0Inst> mT0;
    std::vector<uint32_t> mT0Cut;        // harvested cut entries (ref, vpage, tris) x n
    RangeAlloc mHitAlloc;
    std::vector<std::pair<Range, uint32_t>> mHitFree;   // (range, frame freed): reused after kFrameSlots
    std::vector<VkAccelerationStructureGeometryKHR> mT0Geoms;
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> mT0Ranges;
    struct T0Rec { uint32_t ri, g0, gn; VkDeviceSize scratch; };
    std::vector<T0Rec> mT0Recs;          // this frame's builds
    std::vector<uint32_t> mT0JobCpu;     // 8 words per job
    std::vector<uint32_t> mT0RowCpu;     // 16 words per row
    std::vector<uint32_t> mT0HitCpu;     // 4 words per new hit-cut entry
    std::vector<VkBufferCopy> mT0HitCopies;
    uint32_t mT0Jobs_n = 0, mT0Rows_n = 0;
    uint64_t mT0Bytes = 0;
    VkDeviceSize mT0IdxNeed = 0, mT0VertNeed = 0;

    // PTLAS
    uint32_t mPtlasCap = 0;
    uint32_t mPartSize = kPartSize;      // id order: instances per partition; 0 = spatial (below)
    // spatial partitioning (binding 35 = mPtlasPart: [0, P] instances per partition (P = the global
    // one), then per instance id its partition (CL_NONE = none yet)); its config at creation
    GpuBuffer mPtlasPart;
    uint32_t mPartMode = 0;              // RenderVKClusterPTLASPartition the PTLAS was created with
    uint32_t mPartCount = 0;             // spatial: regular partitions P
    uint32_t mPartMax = 0;               // spatial: maxInstancePerPartitionCount
    float    mPartCell = 64.f;           // spatial: grid cell (m)
    uint64_t mPartIdOrderBytes = 0;      // the id-order PTLAS size for the same capacity (log)
    uint32_t mPtlasBuiltFrame = 0;
#ifdef VK_NV_partitioned_acceleration_structure
    VkPartitionedAccelerationStructureInstancesInputNV mPtlasIn{VK_STRUCTURE_TYPE_PARTITIONED_ACCELERATION_STRUCTURE_INSTANCES_INPUT_NV};
#endif
    VkDeviceAddress mPtlasAddr = 0;
    VkDeviceSize mPtlasScratch = 0;
    bool mPtlasFresh = true;

    // ---- rigged (RIGGED above) ----
    bool     mRigOn = false;             // rigged instances take part this frame
    uint64_t mRigReserve = 0;            // cluster RT memory kept for the rigged path (mMemCap / 4)
    uint32_t mRigIdleFrames = 0;         // consecutive frames without rigged cluster RT (release at 120)
    bool     mRigTier0 = false;          // the arena is laid out for tier 0 (persistent, + indices)
    uint32_t mRigCap = 0;                // arena slots
    uint32_t mRigVtxBase = 0, mRigIdxBase = 0;   // arena regions (words)
    uint32_t mRigMainView = 0xFFFFFFFFu;
    bool     mRigBounds = false;
    uint32_t mRigSkinCount = 0;
    float    mRigEye[3] = {};
    VkBuffer mPalBuf = VK_NULL_HANDLE;   // the renderer's skin palettes (this frame)
    // CLAS tiers: the SHARED SKINNED CUT (1b's main slice, 2a's SKIN_OUT) is usable this frame
    bool     mRigShared = false;
    VkBuffer mSkinOutBuf = VK_NULL_HANDLE;
    // #748 perf pass 2: PERSISTENT RIGGED CLAS (clean_cluster_rt.comp header). The arena's positions
    // are relative to mRigOrigin (moved only when the eye gets kRigOriginMove away); every slot has
    // its explicit CLAS destination (mRigClasStride bytes each); an avatar whose skinned world
    // positions and cut did not change keeps its slots, vertices and CLAS. mRigKeepReset: nothing
    // may be kept this frame (an arena / CLAS / table re-created, the origin moved, a frame gap) --
    // record() then also zeroes the slot stamps and the persistent per-avatar words.
    float    mRigOrigin[3] = {};
    bool     mRigOriginOk = false;
    bool     mRigKeepReset = true;
    bool     mRigRecordedLast = false;   // the previous plan()'s rigged CLAS path was recorded
    bool     mRigKeep = false;            // RP_KEEP bit 0 of this frame (the still-keep)
    bool     mRigCadence = false;         // #1008: RP_SCHED bit 0 of this frame (RenderVKClusterRTRigCadence, CLAS tiers)
    uint32_t mRigStamp = 0;               // RP_STAMP (incremented per plan with the rigged CLAS path)
    VkDeviceSize mRigClasStride = 0;      // explicit destination bytes per slot CLAS
    VkBuffer mRigOfSeen = VK_NULL_HANDLE; // the rig_of buffer the persistent words were zeroed in
    uint32_t mInStill[kAvatarSlotWords] = {};   // FrameIn::rigStill of this frame
    uint32_t mInTraced[kAvatarSlotWords] = {};   // FrameIn::rigTraced (RP_RT_AV; plan() copies it)
    uint32_t mInNoSelf[kAvatarSlotWords] = {};   // #1008: FrameIn::rigNoSelf (RP_NOSELF)
    bool     mRigCadenceIdle = false;            // #1008: cadence on but no traced no-self-shadow avatar this frame
    // #1011 (clean_cluster_rt.comp header): RP_RANK bit 0 (the ranked shared cut: the selection's
    // summary + ranks, mode 13 on the workgroup) and RP_REUSE bit 0 (CLAS reuse) of this frame
    bool     mRigRank = false;
    bool     mRigReuse = false;
    // #1011 profile: the CPU plan() time (vk_rtClusterPlan) of the last log window, avg / max ms
    double   mPlanMsAcc = 0.0, mPlanMsMax = 0.0;
    uint32_t mPlanN = 0;
    double   mPlanMsShown = 0.0, mPlanMsMaxShown = 0.0;
    // A switched rigged instance (ClusterInstance flags) whose avatar the RT plan traces (mInTraced;
    // an instance without a joint-table slot always: = clean_cluster_rt.comp rigTraced).
    bool rigTracedInst(uint32_t flags) const;
    bool     mInStillValid = false;
    VkDeviceAddress mSkinOutAddr = 0;
    uint32_t mRigSelCap = 0;             // the main view's slice capacity (modes 11 / 12 fallback dispatch)
    VkBuffer mSelCnt = VK_NULL_HANDLE;   // 1b's CL_B_COUNTERS: modes 11 / 12 dispatch from the main view's args
    VkDeviceSize mSelDispatchOff = 0;
    uint32_t mRigShrinkSince = 0;
    GpuBuffer mRig, mRigOf, mRigArgs, mRigAddr;          // VKMEM_RT_AUX
    GpuBuffer mRigClasArena;                             // VKMEM_RT_AS (implicit CLAS)
    GpuBuffer mRigCpu[kFrameSlots], mRigStage[kFrameSlots];   // host: params + rows + jobs; tier-0 slot headers
    VkDeviceSize mRigClasScratch = 0, mRigClasBytes = 0;
    uint32_t mRigClasCapQueried = 0;
    std::string mRigWhy;                 // why rigged RT is off (stats line)
    std::string mRigLogged;              // the last failure logged
    // tier 0
    std::unordered_map<uint32_t, T0Rig> mT0Rig;          // per renderer instance id
    RangeAlloc mRigAlloc;
    std::vector<std::pair<Range, uint32_t>> mRigFree;    // (slots, frame freed): reused after kFrameSlots
    std::vector<T0RigRec> mT0RigRecs;                    // this frame's builds / refits (row order)
    std::vector<uint32_t> mRigRowCpu, mRigJobCpu, mRigHdrCpu;
    std::vector<VkBufferCopy> mRigHdrCopies;
    uint32_t mT0RigRows = 0, mT0RigJobs = 0, mT0RigBuilt = 0, mT0RigRefit = 0, mT0RigFails = 0;
    uint32_t mT0RigRetryFrame = 0;
    uint32_t mRigRetryFrame = 0;         // an arena / CLAS allocation failed: retried from this frame
    uint64_t mT0RigBytes = 0;
    // TLAS pass B group map (tlasRigMap), per frame slot; counters read when the slot comes round
    GpuBuffer mTlasRig[kFrameSlots];
    bool     mTlasRigOn = false;
    bool     mTlasRigValid[kFrameSlots] = {};
    uint32_t mTlasCovered = 0, mTlasFallback = 0;     // last harvested: entries on cluster / legacy

    // stats
    uint32_t mLast[kCntWords] = {};      // last harvested counters
    uint32_t mT0BuiltFrame = 0, mT0TrisFrame = 0, mT0InvalidTotal = 0;
    uint64_t mPeakAs = 0, mPeakAux = 0;
    VkDeviceSize mScratchFrame = 0, mScratchPeak = 0;
    double mNextLog = 0.0;
    double mAccStart = 0.0;              // when mAcc started (statsLine)
    double mAccSecs = 0.0;               // the length of the window mAccShown covers
    std::string mWhy;                    // last tier step-down reason
};
