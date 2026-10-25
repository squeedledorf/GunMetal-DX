/**
 * @file llcrossingtrace.h
 * @brief Always-on, cheap per-region-crossing timeline ("Crossing568" log tag). #568
 *
 * One compact INFO block per crossing, emitted ~10 s after CrossedRegion. Everything here
 * is a no-op (one relaxed atomic load) unless a crossing is in progress; the only per-frame
 * cost is pushing one sample into a small ring buffer.
 */
#ifndef LL_LLCROSSINGTRACE_H
#define LL_LLCROSSINGTRACE_H

#include "llfasttimer.h"
#include "llprofiler.h"
#include "stdtypes.h"

#include <chrono>
#include <string>
#include <utility>
#include <vector>

class LLHost;
class LLViewerObject;
class LLViewerRegion;
class LLUUID;

namespace LLCrossingTrace
{
    // Control flags as last sent in AgentUpdate (gAgent's are reset right after sending,
    // before network messages are processed).
    void noteSentControlFlags(U32 flags);
    enum Chunk
    {
        CH_AGENT_MOVE_COMPLETE = 0, // process_agent_movement_complete total
        CH_OBJLIST_SHIFT,           // LLViewerObjectList::shiftObjects total (extraA = objects);
                                    // INCLUDES pipeline_shift, ingest_shift and shift_regions
        CH_PIPELINE_SHIFT,          // LLPipeline::shiftObjects (incl. shiftpos_loop)
        CH_INGEST_SHIFT,            // LLCleanIngest::shiftOrigin (extraA = instances re-based)
        CH_COPY_DIRTY,              // first copyDirtyVK after the shift (extraA = runs, extraB = bytes)
        CH_UPLOAD,                  // instance stream record after the shift (extraA = runs, extraB = bytes)
        CH_WATER,                   // LLWorld::updateWaterObjects (extraA = rebuilds)
        CH_REGION_SIGNAL,           // LLAgent region-changed signal emission
        CH_REANCHOR,                // #568 render-world frame re-anchor (extraA = store rows moved)
        CH_GPU_SHIFT,               // #568 GPU instance shift pass record (extraA = rows)
        CH_OBJLIST_LOOP,            // the object loop alone inside objlist_shift (extraA = drawables marked)
        CH_SHIFTPOS_LOOP,           // the drawable shiftPos loop alone inside pipeline_shift (extraA = drawables)
        CH_SHIFT_REGIONS,           // LLWorld::shiftRegions (region render matrices + particle sim)
        CH_COUNT
    };

    bool active();
    // #607: a crossing is in progress and AgentMovementComplete has not arrived yet (the
    // agent region is still the region being left).
    bool awaitingMovementComplete();

    // Lifecycle
    // CrossedRegion received: starts a crossing. sim_host = the destination's simulator;
    // its pre-crossing state (#850 "dest_pre=": how long it had been connected, handshake,
    // caps, event queue, or NOT_CONNECTED) is captured here, before anything touches it.
    void onCrossedRegion(U64 new_region_handle, const LLHost& sim_host);
    // #850: what process_crossed_region had to do for the destination ("pre-connected",
    // "connected at crossing (no region)", ...) and how many regions the ring anchor move
    // connected on the crossing frame.
    void noteDestinationAction(const std::string& action);
    void noteAnchorConnects(U32 regions_connected);
    // #850 first_full_frame: an object was created in regionp (cache or network).
    void onObjectCreated(const LLViewerRegion* regionp);
    void onCompleteMovementSent();
    void onAgentMovementComplete();                // AgentMovementComplete received (before any work)
    void onShiftDone();                            // origin shifted: arm the copyDirtyVK/upload probes
    void onFrame(F32 frame_interval_seconds);      // once per frame (LLViewerObjectList::update)
    void flush();                                  // emit the pending block now (shutdown)

    // Events
    void onKill(U32 local_id, U32 ip, U32 port, const LLUUID& resolved_id, LLViewerObject* objectp);
    // #607: process_kill_object deferred this kill (LLCrossingKeep) instead of applying it.
    void onKillDeferred(LLViewerObject* objectp);
    void onObjectUpdate(LLViewerObject* objectp);
    void regionEvent(const std::string& text);     // always recorded (ring, pruned), included if within +-10 s
    // #568 render-world frame line of the current crossing ("render_frame: ..."): the mode,
    // the frame offset d and whether this crossing re-anchored (and its cost).
    void noteRenderFrame(const std::string& text);
    std::string regionLabel(const LLViewerRegion* regionp);

    // Timing
    void addChunk(Chunk c, double ms, U32 extra_a = 0, U32 extra_b = 0);
    // copyDirtyVK / upload probes: armed by onShiftDone, consumed once each
    bool takeCopyProbe();   // true once after the shift; arms the upload probe
    bool takeUploadProbe(); // true once after the copy probe fired

    inline double nowMs()
    {
        using namespace std::chrono;
        return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
    }

    struct Scope
    {
        explicit Scope(Chunk c, U32 a = 0) : mChunk(c), mA(a), mOn(active()), mT0(mOn ? nowMs() : 0.0) {}
        ~Scope() { if (mOn) addChunk(mChunk, nowMs() - mT0, mA, 0); }
        void setA(U32 a) { mA = a; }
        Chunk mChunk; U32 mA; bool mOn; double mT0;
    };

    // RAII around LLCleanInstanceStore::copyDirtyVK: times the first call after a shift.
    struct CopyProbe
    {
        CopyProbe(const std::vector<std::pair<U32, U32>>& runs, size_t row_bytes)
            : mOn(takeCopyProbe()), mRuns(runs), mRuns0(runs.size()), mRow(row_bytes), mT0(mOn ? nowMs() : 0.0) {}
        ~CopyProbe()
        {
            if (!mOn) return;
            U64 bytes = 0;
            for (size_t i = mRuns0; i < mRuns.size(); ++i) bytes += (U64)(mRuns[i].second - mRuns[i].first) * mRow;
            addChunk(CH_COPY_DIRTY, nowMs() - mT0, (U32)(mRuns.size() - mRuns0), (U32)(bytes > 0xFFFFFFFFull ? 0xFFFFFFFFull : bytes));
        }
        bool mOn; const std::vector<std::pair<U32, U32>>& mRuns; size_t mRuns0; size_t mRow; double mT0;
    };

    // Timed region lifecycle event (create/handshake/destroy): logs "<kind> <label> total=..ms k=..ms"
    struct RegionTimed
    {
        RegionTimed(const char* kind, const LLViewerRegion* regionp);
        ~RegionTimed();
        void note(const char* what, double ms);
        void noteCount(const char* what, U32 n);
        std::string mKind, mLabel, mNotes;
        double mT0;
    };
}

// Scope + section timer (Tracy-style FTM view) for one synchronous chunk.
// No LLTrace block timer here: a function-local BlockTimerStatHandle registers after the
// per-thread accumulator buffers exist and writes past them (heap corruption at login).
#define CROSSING_SCOPE(chunk, ftm_name) \
    LLCrossingTrace::Scope crossing_scope_(chunk)

#endif // LL_LLCROSSINGTRACE_H
