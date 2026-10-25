/**
 * @file llcleanclusterbuild.h
 * @brief Cluster LOD (#642 / #744, OWNER: 1a): ingest -- clusterize a mesh face's highest
 *        LOD, build the cluster DAG, pack whole groups into fixed-size pages.
 *
 * Output: an LLClusterMeshData (llcleanclustercache.h) in the format of
 * llcleanclusterformat.h, satisfying the builder guarantees G1-G6 there (monotonic group
 * errors, nested group spheres, nested joint masks, one page per group, coarse -> fine pages
 * with root pages first, per-page parent dependency lists). validate() checks them.
 *
 * Credits: meshoptimizer (c) Arseny Kapoulkine, MIT licence -- the library and the approach of
 * its demo/nanite.cpp (cluster DAG loop, group-border locks, bounds merge, error accumulation).
 *
 * Method (published material only): meshoptimizer 0.25 (MIT; already in the build) --
 * meshopt_buildMeshlets* (<= kMaxClusterVerts / kMaxClusterTris), meshopt_partitionClusters
 * (groups of ~kGroupTarget, split above kMaxGroupClusters), meshopt_simplifyWithAttributes /
 * WithUpdate with meshopt_SimplifyLockBorder + vertex_lock (group border; LOD0 open-boundary
 * weld vertices #574; layer-contact regions #611), meshopt_computeSphereBounds / ClusterBounds;
 * its demo/nanite.cpp DAG loop and bounds merge (MIT). Attribute- and skin-weight-aware
 * simplification as in llcleanrigdecimate.cpp (#561 / #618).
 *
 * Quantization: one lattice per mesh face (LLCleanQV::makeHeader over LOD0), copied into
 * every cluster's QHeader; simplified vertices re-encoded on it (watertight levels).
 * Weld bit (kWeldBoundaryBit): set by matching LOD0's open-boundary positions, never by
 * markWeldBoundary on a cluster's own triangles.
 *
 * THREADING: request / pump / takeFinished on the render thread; builds run on the
 * CleanClusterBuild pool (RenderVKClusterBuildThreads, 0 = a quarter of the CPU threads, at
 * most 16). request() only queues (key, slot, LOD, priority): O(1) and copy-free on a repeat,
 * so the streamer may ask every frame (a repeat updates the priority and, while still queued,
 * moves the job to a higher LOD slot the face now publishes -- one build instead of a build +
 * an upgrade rebuild). pump() copies the source slot's CPU mirror (like LLCleanRigDecimator)
 * at admission, after re-checking that the slot still IS that face's LOD (a recycled slot is
 * never built under the wrong key).
 * BUDGET (the MAIN thread's, never the workers'): RenderVKClusterBuildMsPerFrame bounds the
 * render thread's pump (result intake + admission copies, 0.25 .. 8 ms), plus kCopyVertsPerFrame
 * vertices copied per frame. The WORKERS run flat out: admission keeps every worker busy plus
 * a backlog (jobs in flight <= max(4, 3 x threads)), bounded by the bytes of
 * the source copies in flight (kInflightBytes). (Until 2026-10-13 the budget was worker CPU
 * time per frame -- a token bucket of 4 ms: the whole pool built ~4 ms per frame, ~1 build
 * at a time, and a 52-avatar crowd's backlog of 1,500 faces took minutes.)
 *
 * PRIORITY (Prio, from the streamer; what blocks an avatar's switch goes first -- an avatar
 * switches only when EVERY rigged face of it is on the cluster path): band (first builds of
 * rigged faces, then first builds of statics, then rebuilds of Live meshes) -> importance
 * (higher first: the avatar's size / distance, our own avatar first) -> group (the avatar: its
 * faces build together, not interleaved with every other avatar's) -> arrival. The "first
 * seen" trigger is the streamer's acquireMesh (1c), which only runs with RenderVKClusterLOD
 * on; request() refuses everything while it is off.
 *
 * SOURCE LOD: the DAG is built from the slot the streamer passes (its highest resident creator
 * LOD); header.source_lod records which (3 = high). A face first built from a lower LOD is
 * rebuilt when a request arrives with a higher one; the disk cache persists only LOD 3 builds.
 *
 * LOCKS (never moved or removed by the simplification of their level, so every DAG cut keeps
 * them exactly):
 *   - group borders (vertices whose position is shared by two groups of the level): at EVERY
 *     level -- a cut is always watertight inside the face;
 *   - the ORIGINAL face's open boundary (by position, LLCleanQV::markWeldBoundary on LOD0):
 *     the seam to the mesh's other faces and the vertex set the rigged weld (#574 / #641
 *     rest-pose pairs) pairs across attachments -- weld bit and lock are the same set.
 *     Locked at every level EXCEPT the DAG floor (builder v2, G7): once the locked
 *     simplification stalls, RELEASED levels free it (group borders still locked) and a
 *     FINAL level takes the whole remaining face, unlocked, to kTailTris triangles
 *     (meshopt_SimplifyPrune / meshopt_simplifySloppy when the regular simplifier stops
 *     short). Those groups carry CLG_FACE_BORDER_FREE and an error >= their sphere's
 *     DIAMETER (a bound of any deviation inside the sphere, so of any crack to a
 *     neighbouring face): they are cut in only where the whole patch / face projects within
 *     the tolerance. Their removed border vertices simply have no weld partner.
 *   - TODO(2a) #611 body / clothing layer-contact regions: needs the OTHER meshes of the
 *     avatar (contact is a property of the outfit, not of one face), so it cannot be decided
 *     per face here; the cluster pre-skin / weld on the cut is the place (2a).
 * Simplification is removal-only (meshopt_simplifyWithAttributes / Sloppy, never WithUpdate): every
 * vertex of every level is a byte copy of a LOD0 vertex (position, UV, normal, tangent, skin
 * weights), so the lattice re-encode is the identity and locked vertices coincide bit-for-bit.
 * Attribute metric: normal, UV and a skin-weight embedding (pseudo-random unit vector per
 * joint, the #561 decimator's), so collapses stay inside one skinning region. A group stuck
 * on attribute seams retries with meshopt_SimplifyPermissive (the metric still prices the
 * seam collapse); a lone frontier cluster keeps simplifying (the floor is no longer "one
 * cluster" but kFloorTris / kTailTris triangles).
 *
 * ERRORS (G1): error(G) = max(errors of G's members' generating groups) + G's simplification
 * error (meshopt demo/nanite.cpp, MIT) -- >= the max, as the contract's G1 requires; a
 * CLG_FACE_BORDER_FREE group: max(that, 2 x its sphere radius) (G7).
 * Many small disconnected parts (hair strands, lashes, cards: the topology-preserving
 * simplifier cannot shrink a part below its own few triangles, so a released level stalls with
 * hundreds of tiny clusters) are PRUNED in the released stage: whole connected parts of a
 * group that touch no locked (group-border) vertex are removed, smallest first, down to the
 * level's target -- no crack inside the face (no group border moves), error >= the group
 * sphere's diameter as for any released group. A face whose DAG still does not converge (more
 * than kMaxRootPages root pages) fails softly and stays on the legacy path.
 *
 * FAILURE: never a crash -- any failure (no mirror, empty, allocation, validation) delivers a
 * Result with a null mesh and the reason; the face is not re-tried for kFailRetryFrames
 * unless a higher LOD arrives.
 */
#pragma once

#include "stdtypes.h"
#include "llcleanclustercache.h"

#include <memory>
#include <string>
#include <vector>

// Build priority (llcleanclusterbuild.h PRIORITY). importance: larger first (the streamer passes
// the avatar's / object's size over distance); group: the avatar (kNoGroup = none); rebuild: a
// Live mesh's rebuild (behind every first build).
struct LLClusterBuildPrio
{
    static constexpr U32 kNoGroup = 0xFFFFFFFFu;
    F32  importance = 0.f;
    U32  group = kNoGroup;
    bool rebuild = false;
};

class LLCleanClusterBuilder
{
public:
    static LLCleanClusterBuilder& instance();

    static constexpr U32 kNoGroup = LLClusterBuildPrio::kNoGroup;
    using Prio = LLClusterBuildPrio;
    // Render thread. Queue a build of `key` from store slot `src_slot` (the face's highest
    // resident LOD; its CPU mirror is copied at admission). Cheap when already queued /
    // building: a repeat refreshes the priority and, while queued, takes a higher LOD slot.
    // False = refused (queue full, no CPU mirror, no triangles): ask again later; *no_source
    // (optional) = refused because the slot has no CPU mirror / triangles yet (waiting on data).
    bool request(const LLClusterKey& key, U32 src_slot, U32 frame, const Prio& prio = Prio(), bool* no_source = nullptr);
    // Render thread, once per frame: take finished builds, admit queued jobs while workers are
    // free, within budget_ms of RENDER-thread time.
    void pump(F32 budget_ms);

    struct Result
    {
        LLClusterKey key;
        std::shared_ptr<const LLClusterMeshData> mesh;   // null = failed (see why)
        std::string why;
        F32 build_ms = 0.f;
    };
    // Finished builds since the last call (success and failure). A delivered success is
    // forgotten (a later request for the key rebuilds); a failure stays negative-cached for
    // kFailRetryFrames (request returns false) unless a higher LOD slot is offered. A build
    // that was in flight when the caller stopped tracking the key is still delivered: ignore it.
    void takeFinished(std::vector<Result>& out);

    // Stop the workers (app cleanup, before static destructors).
    void stop();

    struct Stats
    {
        U32 queued = 0, building = 0;
        U64 built = 0, failed = 0;
        F32 ms_last = 0.f, ms_max = 0.f, ms_total = 0.f;
        U64 tris_in = 0, clusters_out = 0, pages_out = 0, bytes_out = 0;
        // 1a extras
        U64 requested = 0, refused = 0, stale = 0, no_converge = 0, validate_fail = 0;
        U64 deep_checked = 0, deep_failed = 0, levels_out = 0, root_pages_out = 0;
        U64 root_tris_out = 0, released_meshes = 0;   // the DAG floor (builder v2)
        F32 ms_p50 = 0.f, ms_p95 = 0.f;      // over the last kMsHistory builds
        F32 ms_per_ktri = 0.f;               // worker ms per 1000 triangles (running average; stats only)
        U32 threads = 0;
        // Throughput over the last window (~5 s): finished builds per second and their mean
        // worker ms; source bytes copied for jobs in flight; queued jobs moved to a higher LOD.
        F32 per_sec = 0.f, avg_ms = 0.f;
        U64 inflight_bytes = 0, coalesced = 0;
        F32 pump_ms_max = 0.f;               // render-thread pump, worst of the window
    };
    Stats stats() const;
    std::string statsLine() const;

    // Checks G1-G6 and the page layout of a built / loaded mesh. Any thread.
    // Metadata only (a cache load): header, sentinels, group / page / dependency structure
    // (G4-G6, deps point to coarser = lower-index pages). With the payload (a fresh build)
    // also every page (hashes, layout, alignment 20 / 10 / 4, counts, indices < vert_count)
    // and every cluster's G1-G3 relation (containing vs generating group).
    static bool validate(const LLClusterMeshData& mesh, std::string* why);
    // DEBUG (RenderVKClusterDebug != 0 at admission, or a debug build): sample DAG cuts --
    // uniform tolerances, perspective views, and random non-resident page sets (closure kept,
    // the fallback path) -- through the contract's projectGroupError / clusterRenders, and
    // check each cut is WATERTIGHT (its boundary edges, by exact lattice position, equal
    // LOD0's) and has no ancestor / descendant overlap. Cuts holding CLG_FACE_BORDER_FREE
    // clusters (G7; sampled where each such group first appears, and from far views): every
    // open edge runs between LOD0 border positions (no crack inside the face); a cut of one
    // generating group (the FINAL tail) is a single simplified surface.
    // Needs the payload. Any thread. Failures are counted and logged (tag VKClusterBuild).
    static bool validateCuts(const LLClusterMeshData& mesh, U32 seed, std::string* why);

    static constexpr U32 kMaxRootPages   = 8;       // more = "no convergence": legacy path (pinned VRAM per face)
    static constexpr U32 kMaxPages       = 16384;   // per face (1.3 GB): absurd input refused
    static constexpr U32 kFailRetryFrames = 18000;  // ~5 min before a failed face is retried
    static constexpr U32 kMsHistory      = 256;

private:
    LLCleanClusterBuilder();
    ~LLCleanClusterBuilder();
    LLCleanClusterBuilder(const LLCleanClusterBuilder&) = delete;
    LLCleanClusterBuilder& operator=(const LLCleanClusterBuilder&) = delete;
    struct Impl;                 // job queue, worker pool, source copies, budget (.cpp)
    std::unique_ptr<Impl> mImpl;
};
