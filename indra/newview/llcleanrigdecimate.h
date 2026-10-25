/**
 * @file llcleanrigdecimate.h
 * @brief Decimated intermediate LODs for rigged mesh (#561): a quality-preserving
 *        "un-subdivide" of the slot the GPU cull draws for far avatars at heavy venues.
 *
 * The cull draws a rigged mesh at its finest resident creator LOD, however small it is
 * on screen. Creator LODs below that are often unusable (crushed to save land impact
 * that worn mesh never pays), so instead of falling to them this module builds up to
 * three DECIMATED STEPS of the drawn slot -- ~1/2, ~1/4, ~1/8 of its triangles -- with
 * meshoptimizer's attribute-aware simplifier (positions + normals + UVs + a skin-weight
 * embedding, UV / normal seams kept, open borders of closed-surface faces locked so
 * faces of one mesh do not crack apart -- on the coarser steps only while locking still
 * lets them simplify: past that the borders move and the step's error is scaled so the
 * cull takes it only where the crack is sub-pixel, kUnlockedErrScale). Vertices are only ever REMOVED (never moved or
 * interpolated), so every kept vertex keeps its exact skin weights / joint indices,
 * tangent and UV; the result is compacted (the VRAM and pre-skin cost go down with the
 * vertex count, not only the raster cost).
 *
 * #612 UN-SUBDIVIDE: a source with one level of Catmull-Clark structure (quad-modelled
 * bodies / heads uploaded subdivided) gets its 1/4 step by topology instead: the cage's own
 * vertices kept, edge / face points removed, the cage faces re-formed (unsubdivide in the
 * .cpp; open-border edge points kept, so borders stay exact). Still only REMOVES vertices --
 * the kept ones sit where the subdivision smoothed them, so the step shrinks slightly inside
 * the surface, which its error measures. The 1/8 step simplifies from it. Anything that does
 * not qualify (Marvelous Designer clothing, sculpted props, already-decimated creator LODs)
 * takes the simplifier's 1/4 step as before. RenderVKRigUnsubdivide (default on) for A/B.
 *
 * Each step's geometric error (metres, bind space) travels with it: the cull
 * (clean_cull.comp rigScreenLod) takes the coarsest step whose error, projected at the
 * wearer's distance, stays under the avatar's pixel tolerance -- chosen by the ONE rigged
 * controller (LLCleanRendererVK::rigPressure) and zero in ordinary scenes, for our own
 * avatar and for the protected ones (the largest on screen, rigAssignTiers). A step
 * coarser than last frame's needs a margin (hysteresis), so detail returns smoothly as
 * the camera approaches.
 *
 * Threading: requests / commits / table / find / selectCpu on the render thread (the
 * renderer's buildRiggedFrame and the RT rigged plan in LLCleanRTVK::recordBuild, both
 * inside recordWorld; noteDirty may come from the store-fill worker and is queued); the simplification runs
 * on the CleanRigDecimate pool from a copy of the slot's CPU mirror (the mirror accessors
 * are render-thread only). The results become ordinary non-reclaimable store slots
 * (LLCleanMeshAsset::captureRawSlot under a synthetic key), owned here: a byte-budgeted
 * LRU (RenderVKRigDecimCacheMB) that also ages out steps nobody drew for ~a minute, so a
 * quiet scene holds none. Source content changes (store mirror-dirty) drop the entry.
 *
 * GPU table (clean_cull.comp binding 29, RigDecim): an open-addressed hash keyed by the
 * SOURCE slot id, kTableCap entries x 2 uvec4: {src, step1, step2, step3},
 * {half2(err1, err2), half2(err3, 0), src index_count, src vert_count}. The cull checks
 * the source signature and each step's residency before using it.
 */

#pragma once

#include "stdtypes.h"
#include "lluuid.h"
#include "llcleanqvertex.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

#include "threadpool_fwd.h"

class LLCleanRigDecimator
{
public:
    static LLCleanRigDecimator& instance();

    static constexpr U32 kSteps      = 3;        // decimated steps per source (1/2, 1/4, 1/8)
    static constexpr U32 kTableCap   = 16384;    // GPU hash entries (power of two) = clean_cull.comp
    static constexpr U32 kEntryWords = 8;        // 2 x uvec4 per entry
    static constexpr U32 kProbes     = 16;       // = clean_cull.comp RDEC_PROBES
    static constexpr U32 kEmpty      = 0xFFFFFFFFu;
    static constexpr U32 kTomb       = 0xFFFFFFFEu;
    // A step whose cascade moved open borders (locking them stalled the simplification)
    // stores its error x this: the crack against the mesh's other faces is bounded by both
    // sides' errors, so at a main-view tolerance <= 8 px (LLCleanRendererVK::kRigSlodMainMax)
    // the cull takes it only where that crack is under ~1 px.
    static constexpr F32 kUnlockedErrScale = 16.f;

    // Pixel tolerance of av_vis screen-LOD level lvl (1..7): 0.25 * 2^lvl px
    // (0.5, 1, 2, 4, ...). Shared with clean_cull.comp rigScreenLod.
    static F32 tolerancePx(U32 lvl) { return lvl ? 0.25f * (F32)(1u << (lvl > 7u ? 7u : lvl)) : 0.f; }

    // ---- render thread ----
    // Queue a decimation of source slot `src` (its CPU mirror holds `verts` vertices).
    // Cheap when already known (touches its LRU stamp). Ignored when stopped, for slots
    // with no weights / too few triangles (decided at copy time), or while the
    // queue is full.
    void request(U32 src, U32 verts, U32 frame);
    // The cull may draw this source decimated this frame (LRU stamp).
    void touch(U32 src, U32 frame);
    // Once per frame: launch queued jobs (bounded copy volume), commit finished ones
    // (captureRawSlot + table), age out / evict to `budget_bytes`.
    void pump(U32 frame, U64 budget_bytes);
    // Store slots whose mirror was (re)written or freed: a source among them is dropped
    // at the next pump (its steps were built from other content; a pending job for it is
    // discarded). Any thread (the store fill may run on its worker).
    void noteDirty(const std::vector<U32>& slots);
    // Full geometry repack: our step slots are not reachable from a LOD group, keep them.
    void markLive(std::vector<U8>& live) const;
    // Drop everything (store shutdown: no frees, the store is going away).
    void reset();
    // Stop the workers (app cleanup, before static destructors).
    void stop();

    struct Info
    {
        U32 dec[kSteps];        // step slot ids (kEmpty = none)
        F32 err[kSteps];        // geometric error (m, bind space)
        U32 dec_verts[kSteps];  // vertices of each step
        U32 src_ic = 0, src_vc = 0;
    };
    // A ready source whose steps the GPU table holds (nullptr = none): exactly the sources
    // the cull can decimate, so the CPU replays (predictors, RT rigged plan, shadow LOD)
    // never pick a step the GPU cannot see. Render thread.
    const Info* find(U32 src) const;
    // = clean_cull.comp rigScreenLod on the CPU (the rigged predictors, the RT rigged plan):
    // the coarsest RESIDENT step of `src` (index / vertex counts = its signature) whose
    // error * bind_scale <= limit (limit = tolerancePx(level) * the wearer's metres per
    // pixel, the av_vis AV_LOD_BASE word), else src. out_verts = the chosen slot's vertex
    // count (in: src's). The errors are the half-rounded values the GPU reads, and the
    // arithmetic is the shader's, so both make the same choice.
    template <class Resident>
    U32 selectCpu(U32 src, U32 src_ic, U32 src_vc, F32 limit, F32 bind_scale, const Resident& resident,
                  U32& out_verts) const
    {
        out_verts = src_vc;
        const Info* in = find(src);
        if (!in || in->src_ic != src_ic || in->src_vc != src_vc) return src;
        for (int k = (int)kSteps - 1; k >= 0; --k)
        {
            const U32 d = in->dec[k];
            if (d != kEmpty && resident(d) && in->err[k] * bind_scale <= limit)
            {
                out_verts = in->dec_verts[k];
                return d;
            }
        }
        return src;
    }
    // = clean_cull.comp rigStepOk: may the packed slot row of a step (ic / vc / base /
    // first / has_weights) stand in for its source (src_ic / src_vc)? Resident, weighted
    // (pre-skin needs its weight range), its decode header in range (base >= the 2 header
    // pseudo-vertices, #573), no larger than the source, whole triangles, and its index /
    // vertex ranges inside the packed buffers (idx_valid = the cull's u_idx_valid,
    // vtx_valid = u_vtx_valid; 0 = unchecked). Anything else draws the SOURCE (never
    // nothing): the cull's OOB guard would drop the whole draw.
    static bool stepRowOk(U32 ic, U32 vc, S32 base_vertex, U32 first_index, U32 has_weights,
                          U32 src_ic, U32 src_vc, U64 idx_valid, U64 vtx_valid)
    {
        return ic >= 3u && ic % 3u == 0u && vc >= 3u && has_weights != 0u
            && base_vertex >= (S32)LLCleanQV::kHeaderVerts && ic <= src_ic && vc <= src_vc
            && (U64)first_index + ic <= idx_valid
            && (vtx_valid == 0u || (U64)base_vertex + vc <= vtx_valid);
    }
    // = clean_cull.comp rigBindScale: the largest column of the skin's bind shape (the
    // SkinMeta record's last 16 words), 1 when unknown / absurd.
    static F32 bindScale(const U32* meta, size_t meta_words, U32 skin_id, size_t rec_words);

    // GPU table: words (kTableCap * kEntryWords) and the entries written since the last
    // take (full = upload all). Render thread.
    const U32* tableWords() const { return mTable.data(); }
    bool takeTableDirty(std::vector<U32>& entries, bool& full);
    void forceFullUpload() { mTableFull = true; }
    // Bumped on every table change (the RT rigged plan's reuse key).
    U32  tableGeneration() const { return mTableGen; }

    // Steps still being built (queued within the budget, or running): the controller
    // waits for them before reaching for creator LODs.
    bool busy() const { return mInFlight != 0 || (mQueuedLive != 0 && !mBudgetFull); }
    U64  bytes() const { return mBytes; }
    U32  readyCount() const { return mReady; }
    std::string statsLine(U64 budget_bytes) const;   // VKMem segment
    // Cull readback (dbg words): instances drawn decimated / vertices saved last frame.
    void noteGpuUse(U32 inst, U32 saved) { mGpuInst = inst; mGpuSaved = saved; }

private:
    LLCleanRigDecimator();
    ~LLCleanRigDecimator();
    LLCleanRigDecimator(const LLCleanRigDecimator&) = delete;
    LLCleanRigDecimator& operator=(const LLCleanRigDecimator&) = delete;

    enum State : U8 { ST_QUEUED = 0, ST_RUNNING, ST_READY, ST_FAILED, ST_DEAD };
    struct Entry
    {
        State state = ST_QUEUED;
        U32   job = 0;              // id of the job building it (stale results are dropped)
        U32   last_use = 0;         // frame
        U32   src_verts = 0;
        U64   bytes = 0;            // GPU-side bytes of its steps (packed verts + weights + indices)
        LLUUID key;                 // synthetic store key of the steps
        Info  info;
        U32   table = kEmpty;       // its GPU table entry
        // #573 regression fix: committed steps enter the GPU table only from the NEXT pump,
        // after the store fill has placed their slot rows (a brand-new slot id's GPU row is
        // unwritten memory until then), and only once validated (validateSteps).
        U32   ready_frame = 0;
        bool  pending_table = false;
    };
    struct Job
    {
        U32 id = 0, src = 0;
        // input (copied from the store mirror on the render thread: the quantized range,
        // decoded into `verts` on the worker, #573)
        std::vector<LLCleanQV::QVertex> packed;   // header-prefixed source range
        LLCleanQV::QHeader domain{};              // the source's decode header (steps reuse it)
        std::vector<F32> verts;     // MirrorVertex stream (12 floats per vertex)
        std::vector<LLCleanQV::QWeight> wts;   // MirrorWeight stream (10 B per vertex, #582)
        std::vector<U32> idx;
        // output, per step (empty = no step)
        struct Step { std::vector<F32> verts; std::vector<LLCleanQV::QWeight> wts; std::vector<U32> idx; F32 err = 0.f; };
        Step steps[kSteps];
        bool ok = false;
        U32  unlocked_from = kSteps;   // first step built with unlocked borders (kSteps = none)
        // #612 un-subdivide: tried for the 1/4 step (RenderVKRigUnsubdivide at post time)
        bool unsub = false;
        U32  unsub_reason = ~0u;       // UNSUB_* (.cpp; 0 = qualified, ~0 = not tried)
        U32  unsub_tris = 0;           // triangles of the un-subdivided step
        F32  unsub_err_rel = 0.f;      // its error / the mesh extent
    };

    Entry* lookup(U32 src);         // O(1): mDense
    Entry& create(U32 src);
    void   erase(U32 src);
    void runJob(Job* job);          // worker
    void commit(Job* job, U32 frame);
    void freeEntry(U32 src, Entry& e);
    U32  tableInsert(U32 src, const Entry& e);
    void tableRemove(U32 idx);
    void tableWrite(U32 idx, U32 src, const Entry& e);
    void tableRebuild();
    U32  hashSlot(U32 src) const;
    // Checks a committed entry's step slots against the store mirror (header = the
    // source's, counts, index range, weights); `diag` also matches the first step vertices
    // to source vertices and logs the result (tag VKRigDecim573).
    bool validateSteps(U32 src, const Entry& e, bool diag);
    void flushTablePending(U32 frame);

    std::unordered_map<U32, Entry> mEntries;
    std::vector<Entry*> mDense;     // by source slot id (the scan asks per rigged instance)
    std::priority_queue<std::pair<U32, U32>> mQueue;   // (verts, src): biggest first
    std::vector<U32> mTable;
    std::vector<U32> mTableDirty;
    std::vector<U32> mTablePending;   // committed sources waiting for their table insert
    U32  mDiagChecks = 0, mDiagBad = 0;   // VKRigDecim573 one-shot diagnostic
    bool mDiagSummary = false;
    bool mTableFull = true;
    U32  mTableGen = 1;
    U32  mTombs = 0;

    std::unique_ptr<LL::ThreadPool> mPool;
    std::unordered_map<U32, std::unique_ptr<Job>> mJobs;   // render thread: every posted job, by id
    std::mutex mDoneMutex;
    std::mutex mDirtyMutex;
    std::vector<U32> mPendingDirty;   // noteDirty -> pump
    std::vector<Job*> mDone;
    U32  mInFlight = 0;
    U32  mQueuedLive = 0;           // entries in ST_QUEUED
    bool mBudgetFull = false;       // last pump: no room for more steps (bytes / table load)
    U32  mNextJob = 1;
    bool mStopped = false;

    U64 mBytes = 0, mPeakBytes = 0;
    U32 mReady = 0, mFailed = 0;
    U32 mStatBuilt = 0, mStatEvicted = 0, mStatAged = 0, mStatInvalidated = 0;
    U64 mStatSrcTris = 0, mStatStepTris = 0;
    U64 mStatStepTrisS[kSteps] = {};   // per step (#561 tuning: per-step tris kept)
    U32 mStatReach[kSteps] = {};       // sources that reached each step
    U32 mStatUnlocked = 0;             // sources with an unlocked-border step
    // #612 un-subdivide: sources tried / qualified, their triangles, error (/ mesh extent),
    // and why the rest did not qualify (UNSUB_* in the .cpp, index 1..)
    static constexpr U32 kUnsubReasons = 7;
    U32 mStatUnsubTried = 0, mStatUnsubOk = 0;
    U64 mStatUnsubSrcTris = 0, mStatUnsubTris = 0;
    F64 mStatUnsubErrSum = 0.0;
    F32 mStatUnsubErrMax = 0.f;
    U32 mStatUnsubFail[kUnsubReasons] = {};
    bool mUnsubOn = true;              // RenderVKRigUnsubdivide as last seen (a change rebuilds every step)
    U32 mGpuInst = 0, mGpuSaved = 0;
};
