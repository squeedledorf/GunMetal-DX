/**
 * @file llcleanrigshadowlod.h
 * @brief Shadow-caster LOD for rigged mesh (#564): the #561 decimated steps of a drawn
 *        rigged slot, re-indexed onto THAT slot's vertices, so a shadow view can draw a
 *        coarser step over the very skinned vertices the main view pre-skinned.
 *
 * Shadow maps draw rigged casters from the main cull's pre-skinned output (one skinned
 * range per draw, shared by every view). A #561 step is a separate, COMPACTED slot -- its
 * index buffer does not address the source's skinned vertices. But a step only REMOVES
 * vertices (never moves or blends them), so every step vertex is a copy of a source
 * vertex, quantized against the SOURCE's lattice (#573, LLCleanRigDecimator::commit):
 * matching their quantized position + UV lattice indices and skin weights (the normal is
 * irrelevant to a shadow caster) gives the step's triangles in source numbering. Those
 * 16-bit index lists (#573; ranges even-aligned for vkCmdUpdateBuffer) live in this
 * module's own GPU index
 * buffer (byte budget RenderVKShadowRigLodMB, LRU, aged out ~30 s after the slot stops
 * casting), found by the shadow rig cull (clean_shadow_rigcull.comp) through an
 * open-addressed table keyed by the drawn slot's FIRST INDEX (the draw command carries
 * it): 3 uvec4 per entry
 *   {first_index (~0 empty, ~0-1 deleted), index_count, vert_count, steps | half(err3) << 16},
 *   {step1 first, step2 first, step3 first, 0}  (in this module's index buffer),
 *   {step1 count, step2 count, step3 count, half2(err1, err2)}
 * err = the step's geometric error in metres, bind space (= LLCleanRigDecimator's). The
 * cull draws the coarsest step whose error, scaled by the skin's bind shape, stays under
 * the view's shadow-texel footprint x RenderVKShadowRigLodTexels -- invisible by
 * construction: a near / fine cascade keeps the full mesh.
 *
 * Threading: everything on the render thread except the vertex matching, which runs on
 * one worker from copies of the mirrors (the mirror accessors are render-thread only).
 */

#pragma once

#include "stdtypes.h"

#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class LLCleanRigShadowLod
{
public:
    static LLCleanRigShadowLod& instance();

    static constexpr U32 kSteps      = 3;
    static constexpr U32 kTableCap   = 8192;   // = clean_shadow_rigcull.comp (power of two)
    static constexpr U32 kEntryWords = 12;
    static constexpr U32 kProbes     = 16;     // = clean_shadow_rigcull.comp SLOD_PROBES
    static constexpr U32 kEmpty      = 0xFFFFFFFFu;
    static constexpr U32 kTomb       = 0xFFFFFFFEu;

    // The renderer's CPU slot rows (= its GPU SlotInfo, 32 B).
    struct SlotRow { U32 first_index, index_count; S32 base_vertex; U32 has_weights, vert_count, p0, p1, p2; };
    static_assert(sizeof(SlotRow) == 32, "SlotRow must match the renderer's SlotInfo (32 B)");

    // ---- render thread ----
    // Drawn slot `slot` casts into a shadow view this frame (from the rigged scan). Builds
    // its entry when the decimator has its steps; stamps the LRU otherwise. Returns true
    // when the slot has (or is getting) an entry.
    bool want(U32 slot, const SlotRow& row, U32 frame);
    // Slots whose mirror was rewritten / freed: their entries go (any order vs. want).
    void noteDirty(const std::vector<U32>& slots);
    // Once per frame: commit finished matches (allocate + queue index uploads + table),
    // drop entries whose slot moved (repack) or went away, age out / evict to the budget,
    // launch queued jobs. `capacity_words` = the GPU index buffer's size (0 = none: off).
    void pump(U32 frame, const SlotRow* rows, size_t nrows, U32 capacity_words);
    // GPU table: words + the entries written since the last take (full = all).
    const U32* tableWords() const { return mTable.data(); }
    bool takeTableDirty(std::vector<U32>& entries, bool& full);
    // Index uploads queued by pump (word offset into the index buffer + data). The
    // renderer records them (before the table upload), then clears the list.
    // first = index offset (even); idx padded to an even length (the pad is never drawn).
    struct Upload { U32 first; std::vector<U16> idx; };
    std::vector<Upload>& uploads() { return mUploads; }
    // Drop everything (store reset: slot ids are meaningless); stop the worker (app cleanup).
    void reset();
    void stop();
    U32  liveEntries() const { return (U32)mLive.size(); }
    std::string statsLine() const;

private:
    LLCleanRigShadowLod();
    ~LLCleanRigShadowLod();

    struct Job
    {
        U32 slot = 0, id = 0;
        U32 vc = 0;
        std::vector<U32> src;                 // per source vertex: match key (kKeyWords words)
        std::vector<U32> step_v[kSteps];      // per step vertex, same key
        std::vector<U32> step_i[kSteps];      // step-local indices
        F32 err[kSteps] = {};
        std::vector<U32> out[kSteps];         // source-numbered indices (empty = none)
        bool ok = false;
    };
    struct Entry
    {
        U32 slot = 0, first_index = 0, ic = 0, vc = 0;
        U32 range_first[kSteps] = {}, range_count[kSteps] = {};
        F32 err[kSteps] = {};
        U32 nsteps = 0;
        U32 table = kEmpty;
        U32 last_use = 0;
        U64 words = 0;
    };
    enum State : U8 { ST_QUEUED, ST_RUNNING, ST_FAILED };
    struct Pending { State state; U32 job; U32 frame; U32 ic, vc; };

    static void match(Job& job);
    void workerMain();
    bool alloc(U32 words, U32& first);
    void freeRange(U32 first, U32 words);
    void freeEntry(Entry& e);
    U32  hashKey(U32 key) const;
    U32  tableInsert(const Entry& e);
    void tableRemove(U32 t);
    void tableRebuild();
    void writeEntryWords(U32 t, const Entry* e);

    std::unordered_map<U32, Entry>   mLive;      // slot -> entry
    std::unordered_map<U32, Pending> mPending;   // slot -> queued / running / failed
    std::deque<U32>                  mQueue;     // slots to launch
    std::vector<U32>                 mTable;
    std::vector<U32>                 mDirty;
    bool                             mTableFull = true;
    U32                              mTombs = 0;
    std::vector<Upload>              mUploads;
    std::map<U32, U32>               mFree;      // first word -> words
    U32                              mCapWords = 0;
    U64                              mUsedWords = 0, mPeakWords = 0;
    U32                              mJobSeq = 0;
    U64                              mStatBuilt = 0, mStatFailed = 0, mStatEvicted = 0, mStatSrcTris = 0, mStatStepTris = 0;

    std::mutex                       mMutex;
    std::condition_variable          mCv;
    std::vector<std::unique_ptr<Job>> mJobsIn, mJobsOut;
    U32                              mInFlight = 0;
    std::thread                      mThread;
    bool                             mStopped = false;
};
