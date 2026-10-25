/**
 * @file llcrossingtrace.cpp
 * @brief Always-on per-crossing timeline ("Crossing568"). See header. #568
 */

#include "llviewerprecompiledheaders.h"

#include "llcrossingtrace.h"
#include "llcrossingkeep.h"
#include "llmemdiag.h"

#include <atomic>
#include <cstdio>
#include <algorithm>
#include <deque>
#include <map>
#include <mutex>
#include <set>

#include "llagent.h"
#include "llagentcamera.h"
#include "llhost.h"
#include "message.h"
#include "llworld.h"
#include "llnotificationsutil.h"
#include "llviewercontrol.h"
#include "llviewerjointattachment.h"
#include "llframetimer.h"
#include "llregionhandle.h"
#include "llviewerobject.h"
#include "llviewerregion.h"
#include "llvoavatarself.h"

static std::atomic<U32> sLastSentControlFlags{0};
void LLCrossingTrace::noteSentControlFlags(U32 flags) { sLastSentControlFlags.store(flags, std::memory_order_relaxed); }

namespace
{
    constexpr double kWindowPreS   = 1.0;   // frame window: -1 s ...
    constexpr double kWindowPostS  = 5.0;   // ... to +5 s around CrossedRegion
    constexpr double kFlushAfterS  = 10.0;  // block is emitted this long after CrossedRegion
    constexpr double kRegionEvtS   = 10.0;  // region events included within +-10 s
    constexpr size_t kMaxRegionEvt = 96;
    constexpr double kHandoffMaxS  = 20.0;  // block waits this long if worn attachments are still missing
    constexpr size_t kSlowHistoryK = 3;     // last K crossings considered per attachment
    constexpr size_t kSlowHitsM    = 2;     // flagged when slow on >= M of them
    constexpr size_t kMinForMedian = 3;     // need this many arrivals for a meaningful median
    constexpr double kFullQuietS   = 1.0;   // #850 first_full_frame: no new-region creates this long

    struct FrameSample { F64 t; float ms; };
    struct RegionEvt   { F64 t; std::string text; };

    struct Attach
    {
        LLUUID id;          // object (root) id; stable across the region change
        LLUUID item;        // inventory item id (null if unknown)
        std::string name;
        F64 tArr = -1.0;    // first update from the new region
    };
    struct SlowState
    {
        std::deque<std::pair<bool, float>> hist;   // (slow, arrival seconds rel CrossedRegion), newest last
    };

    void onSlowAttachResponse(const LLSD& notification, const LLSD& response)
    {
        if (LLNotificationsUtil::getSelectedOption(notification, response) == 1)
        {
            gSavedSettings.setBOOL("SLCrossingSlowAttachWarn", false);
        }
    }

    struct Chunks { double ms = 0.0; U32 n = 0; U64 a = 0; U64 b = 0; };

    struct State
    {
        std::mutex mtx;
        std::atomic<bool> active{false};
        std::atomic<bool> copyArmed{false};
        std::atomic<bool> uploadArmed{false};

        std::deque<FrameSample> frames;      // last ~kWindowPreS seconds, always maintained
        std::deque<RegionEvt>   regionEvts;  // last ~60 s, always maintained

        // ---- current crossing ----
        U32  seq = 0;
        F64  t0 = 0.0;                       // CrossedRegion received (seconds, LLFrameTimer)
        F64  tSent = -1, tAMC = -1, tShiftDone = -1;
        F64  tVehUpd = -1, tAvUpd = -1;
        std::string mode;                    // seated / flying / walking
        std::string vehicleName;
        std::string oldLabel, newLabel;
        const LLViewerRegion* oldRegion = nullptr;
        U64  newHandle = 0;
        LLUUID vehId;
        U32  vehLocal = 0, vehIp = 0, vehPort = 0;
        std::string vehUpdRegion, avUpdRegion;
        std::vector<std::string> killEvents; // vehicle-root kill events
        U32  childKills = 0, otherKills = 0, unknownKills = 0, deferredKills = 0;
        LLCrossingKeep::Snapshot keep0;      // #607 counters at CrossedRegion
        Chunks chunk[LLCrossingTrace::CH_COUNT];
        std::string renderFrame;             // #568 render-world frame line
        // ---- #850 pre-crossing state ("pre:" line) ----
        std::string destPre;                 // destination state at CrossedRegion
        std::string destAction;              // what process_crossed_region did for it
        F32  farSent = -1.f, speed = 0.f;
        bool fast = false;
        U32  regionsAtCross = 0;
        U32  anchorConnects = 0;
        F64  tLastNewCreate = -1;            // last object created in the new region
        U32  newCreates = 0;                 // objects created in the new region since CrossedRegion
        F64  tFullFrame = -1;                // resolved first_full_frame (abs time), -1 = pending
        bool copyFired = false;
        // frames
        U32  preN = 0;  float preWorst = 0.f;
        U32  postN = 0; float postWorst = 0.f; double postWorstT = 0.0;
        int  shiftFramesLeft = 0;            // 2 -> crossing frame, 1 -> next frame
        float crossFrameMs = -1.f, nextFrameMs = -1.f;
        // ---- handoff (own attachments + control) ----
        std::vector<Attach> attach;
        std::map<LLUUID, size_t> attachIdx;  // written only in onCrossedRegion (main thread)
        LLVector3d posAMC;
        bool haveAMC = false;
        bool inputSeen = false;
        F64  tControl = -1;
        // ---- rolling per-attachment state (bounded: worn attachments only) ----
        std::map<LLUUID, SlowState> slow;
        std::set<LLUUID> warned;
        std::vector<std::pair<std::string, std::string>> pendingWarn;   // (name, seconds)
        std::atomic<bool> hasPending{false};
    };

    State& S() { static State s; return s; }

    std::string fmtRel(F64 t, F64 t0)
    {
        if (t < 0.0) return "none";
        char b[32];
        std::snprintf(b, sizeof(b), "%+.1f", (t - t0) * 1000.0);
        return b;
    }

    // Caller holds mtx. Returns the "handoff:" line and updates the slow-attachment state.
    std::string handoffLocked(State& s, F64 now)
    {
        char b[256];
        const double relNow = now - s.t0;
        std::vector<double> arr;
        for (const Attach& a : s.attach) if (a.tArr >= 0) arr.push_back((a.tArr - s.t0) * 1000.0);
        std::sort(arr.begin(), arr.end());
        double med = -1.0;
        if (!arr.empty())
        {
            const size_t n = arr.size();
            med = (n & 1) ? arr[n / 2] : 0.5 * (arr[n / 2 - 1] + arr[n / 2]);
        }
        std::string o = "\n  handoff: ";
        std::snprintf(b, sizeof(b), "attachments=%u", (unsigned)s.attach.size());
        o += b;
        if (!arr.empty())
        {
            std::snprintf(b, sizeof(b), " first=%+.0f median=%+.0f last=%+.0f", arr.front(), med, arr.back());
            o += b;
        }
        else if (!s.attach.empty())
        {
            o += " first=none median=none last=none";
        }

        // slowest 3 (missing ones rank above everything)
        std::vector<const Attach*> order;
        for (const Attach& a : s.attach) order.push_back(&a);
        std::stable_sort(order.begin(), order.end(), [](const Attach* x, const Attach* y) {
            const double xv = x->tArr < 0 ? 1e18 : x->tArr, yv = y->tArr < 0 ? 1e18 : y->tArr;
            return xv > yv; });
        o += " slow=[";
        for (size_t i = 0; i < order.size() && i < 3; ++i)
        {
            if (i) o += ", ";
            if (order[i]->tArr < 0) o += order[i]->name + " missing";
            else { std::snprintf(b, sizeof(b), " %+.0f", (order[i]->tArr - s.t0) * 1000.0); o += order[i]->name + b; }
        }
        o += "]";
        U32 nmiss = 0;
        std::string missing;
        for (const Attach& a : s.attach)
            if (a.tArr < 0) { if (nmiss++) missing += ", "; missing += a.name; }
        if (nmiss)
        {
            std::snprintf(b, sizeof(b), " missing@+%.0fs=[", relNow);
            o += b + missing + "]";
        }
        if (s.tControl >= 0)
        {
            std::snprintf(b, sizeof(b), " control_restored=%+.0f (input held)", (s.tControl - s.t0) * 1000.0);
            o += b;
        }
        else if (s.tAMC < 0) o += " control=n/a(no AgentMovementComplete)";
        else if (!s.inputSeen) o += " control=n/a(no input)";
        else o += " control_restored=none (input held)";

        // ---- slow-to-transfer detection (observation only; no extra requests) ----
        if (arr.size() >= kMinForMedian)
        {
            const double thr = std::max(0.1F, gSavedSettings.getF32("SLCrossingSlowAttachSeconds"));
            for (const Attach& a : s.attach)
            {
                if (a.item.isNull()) continue;
                const double arrS = (a.tArr >= 0 ? (a.tArr - s.t0) : relNow);   // missing: lower bound
                const bool isSlow = (arrS - med / 1000.0) > thr;
                SlowState& st = s.slow[a.item];
                st.hist.push_back({isSlow, (float)arrS});
                while (st.hist.size() > kSlowHistoryK) st.hist.pop_front();
                size_t hits = 0; double sum = 0.0;
                for (const auto& h : st.hist) if (h.first) { ++hits; sum += h.second; }
                if (hits >= kSlowHitsM && s.warned.insert(a.item).second)
                {
                    std::snprintf(b, sizeof(b), "%.1f", sum / (double)hits);
                    LL_INFOS("Crossing568") << "slow_attachment flagged: " << a.name << " item=" << a.item
                        << " slow_on=" << hits << "/" << st.hist.size() << " mean_arrival_s=" << b
                        << " threshold_s=" << thr << LL_ENDL;
                    if (gSavedSettings.getBOOL("SLCrossingSlowAttachWarn"))
                    {
                        s.pendingWarn.emplace_back(a.name, b);
                        s.hasPending.store(true);
                    }
                }
            }
        }
        return o;
    }

    // Caller holds mtx.
    void emitLocked(State& s, bool partial)
    {
        if (!s.active.load()) return;
        s.active.store(false);
        s.copyArmed.store(false);
        s.uploadArmed.store(false);

        std::string o;
        char b[512];
        std::snprintf(b, sizeof(b), "#%u mode=%s%s%s old=%s new=%s%s", s.seq, s.mode.c_str(),
                      s.vehicleName.empty() ? "" : " vehicle=", s.vehicleName.c_str(),
                      s.oldLabel.c_str(), s.newLabel.c_str(), partial ? " PARTIAL(next crossing began)" : "");
        o += b;
        {
            // #850: one line on the handoff's starting conditions and how fast the new
            // region's content settled.
            U32 created10 = 0;
            for (const RegionEvt& e : s.regionEvts)
            {
                const double d = e.t - s.t0;
                if (d >= 0.0 && d <= kRegionEvtS && e.text.compare(0, 7, "create ") == 0) ++created10;
            }
            std::string ff;
            if (s.tAMC < 0) ff = "n/a(no AgentMovementComplete)";
            else if (s.tFullFrame >= 0) { std::snprintf(b, sizeof(b), "%+.0f", (s.tFullFrame - s.tAMC) * 1000.0); ff = b; }
            else { std::snprintf(b, sizeof(b), "pending@%+.0fs", LLFrameTimer::getElapsedSeconds() - s.tAMC); ff = b; }
            std::snprintf(b, sizeof(b), " far_sent=%.0f speed=%.1f fast=%d regions=%u +%u@anchor +%u@10s"
                          " first_full_frame=%s(rel AMC, new_region_creates=%u)",
                          s.farSent, s.speed, s.fast ? 1 : 0, s.regionsAtCross, s.anchorConnects, created10,
                          ff.c_str(), s.newCreates);
            o += "\n  pre: dest_pre=" + s.destPre
               + (s.destAction.empty() ? std::string() : " dest_action=" + s.destAction) + b;
        }
        o += "\n  timeline_ms(rel CrossedRegion): CrossedRegion=0 CompleteAgentMovement_sent=" + fmtRel(s.tSent, s.t0)
           + " AgentMovementComplete_rx=" + fmtRel(s.tAMC, s.t0);
        if (s.tAMC >= 0 && s.tSent >= 0)
        {
            std::snprintf(b, sizeof(b), " (server_rtt=%.1f)", (s.tAMC - s.tSent) * 1000.0);
            o += b;
        }
        o += " shift_done=" + fmtRel(s.tShiftDone, s.t0);
        o += " first_update_vehicle=" + fmtRel(s.tVehUpd, s.t0);
        if (!s.vehUpdRegion.empty()) o += "[" + s.vehUpdRegion + "]";
        o += " first_update_avatar=" + fmtRel(s.tAvUpd, s.t0);
        if (!s.avUpdRegion.empty()) o += "[" + s.avUpdRegion + "]";

        o += "\n  kills(old sim): ";
        if (s.killEvents.empty()) o += "vehicle_root=none ";
        for (const std::string& k : s.killEvents) o += k + " ";
        std::snprintf(b, sizeof(b), "vehicle_children_killed=%u other_known_killed=%u unknown_local=%u deferred=%u",
                      s.childKills, s.otherKills, s.unknownKills, s.deferredKills);
        o += b;
        o += "\n  keep(#607): " + LLCrossingKeep::deltaLine(s.keep0);

        static const char* kNames[LLCrossingTrace::CH_COUNT] = {
            "agent_move_complete", "objlist_shift", "pipeline_shift", "ingest_shift",
            "copyDirtyVK", "upload_record", "updateWaterObjects", "region_changed_signal",
            "reanchor", "gpu_inst_shift", "objlist_loop", "shiftpos_loop", "shift_regions" };
        o += "\n  sync_ms:";
        for (int i = 0; i < LLCrossingTrace::CH_COUNT; ++i)
        {
            const Chunks& c = s.chunk[i];
            if (c.n == 0) { o += std::string(" ") + kNames[i] + "=-"; continue; }
            std::snprintf(b, sizeof(b), " %s=%.2f", kNames[i], c.ms);
            o += b;
            switch (i)
            {
            case LLCrossingTrace::CH_OBJLIST_SHIFT:  std::snprintf(b, sizeof(b), "(objects=%llu)", (unsigned long long)c.a); o += b; break;
            case LLCrossingTrace::CH_INGEST_SHIFT:   std::snprintf(b, sizeof(b), "(instances=%llu)", (unsigned long long)c.a); o += b; break;
            case LLCrossingTrace::CH_COPY_DIRTY:
            case LLCrossingTrace::CH_UPLOAD:         std::snprintf(b, sizeof(b), "(runs=%llu bytes=%llu)", (unsigned long long)c.a, (unsigned long long)c.b); o += b; break;
            case LLCrossingTrace::CH_WATER:          std::snprintf(b, sizeof(b), "(calls=%u rebuilt=%llu)", c.n, (unsigned long long)c.a); o += b; break;
            case LLCrossingTrace::CH_REANCHOR:
            case LLCrossingTrace::CH_GPU_SHIFT:      std::snprintf(b, sizeof(b), "(rows=%llu)", (unsigned long long)c.a); o += b; break;
            case LLCrossingTrace::CH_OBJLIST_LOOP:
            case LLCrossingTrace::CH_SHIFTPOS_LOOP:  std::snprintf(b, sizeof(b), "(drawables=%llu)", (unsigned long long)c.a); o += b; break;
            default: break;
            }
        }

        o += "\n  render_frame: " + (s.renderFrame.empty() ? std::string("-") : s.renderFrame);
        o += "\n  regions(+-10s):";
        bool any = false;
        for (const RegionEvt& e : s.regionEvts)
        {
            const double d = e.t - s.t0;
            if (d < -kRegionEvtS || d > kRegionEvtS) continue;
            std::snprintf(b, sizeof(b), "\n    [%+.1f] ", d * 1000.0);
            o += b + e.text;
            any = true;
        }
        if (!any) o += " none";

        o += handoffLocked(s, LLFrameTimer::getElapsedSeconds());

        std::snprintf(b, sizeof(b),
            "\n  frames: pre_1s n=%u worst=%.1fms | post_5s n=%u worst=%.1fms@%+.0f | crossing_frame=%.1fms next_frame=%.1fms",
            s.preN, s.preWorst, s.postN, s.postWorst, s.postWorstT * 1000.0, s.crossFrameMs, s.nextFrameMs);
        o += b;

        LL_INFOS("Crossing568") << o << LL_ENDL;
    }
}

namespace LLCrossingTrace
{

bool active() { return S().active.load(std::memory_order_relaxed); }

bool awaitingMovementComplete()
{
    if (!active()) return false;
    State& s = S();
    std::lock_guard<std::mutex> lk(s.mtx);
    return s.tAMC < 0;
}

std::string regionLabel(const LLViewerRegion* regionp)
{
    if (!regionp) return "(none)";
    U32 x = 0, y = 0;
    from_region_handle(regionp->getHandle(), &x, &y);
    char b[96];
    std::snprintf(b, sizeof(b), "(%u,%u)", x / 256u, y / 256u);
    return regionp->getName() + b;
}

// #850: the destination's state at CrossedRegion, before process_crossed_region touches it.
static std::string destinationState(U64 handle, const LLHost& host, F64 now)
{
    LLViewerRegion* regionp = LLWorld::instanceExists() ? LLWorld::getInstance()->getRegionFromHandle(handle) : nullptr;
    const LLCircuitData* cdp = gMessageSystem ? gMessageSystem->mCircuitInfo.findCircuit(host) : nullptr;
    if (!regionp)
    {
        return cdp ? "NOT_CONNECTED(no region, circuit up)" : "NOT_CONNECTED(no region)";
    }
    if (regionp->getHost() != host)
    {
        return "NOT_CONNECTED(region on another host)";
    }
    if (!cdp)
    {
        return "NOT_CONNECTED(region, no circuit)";
    }
    char b[192];
    auto rel = [&](F64 t) -> std::string
    {
        if (t < 0.0) return "no";
        char r[32];
        std::snprintf(r, sizeof(r), "+%.1fs", t - regionp->getCreatedAt());
        return r;
    };
    std::snprintf(b, sizeof(b), "up%.1fs(handshake=%s caps=%s eq=%s circuit=%s)",
                  regionp->getCreatedAt() >= 0.0 ? now - regionp->getCreatedAt() : -1.0,
                  rel(regionp->getHandshakeReplyAt()).c_str(),
                  regionp->capabilitiesReceived() ? rel(regionp->getCapsReceivedAt()).c_str() : "no",
                  regionp->hasEventPoll() ? "yes" : "no",
                  cdp->isAlive() ? "alive" : "DEAD");
    return b;
}

void onCrossedRegion(U64 new_region_handle, const LLHost& sim_host)
{
    LLMemDiag::noteRegionCrossing("crossed");   // #700 pre-disconnect trace
    State& s = S();
    std::lock_guard<std::mutex> lk(s.mtx);
    emitLocked(s, true);

    const F64 now = LLFrameTimer::getElapsedSeconds();
    const U32 seq = s.seq + 1;
    // reset everything but the always-on rings and the sequence counter
    std::deque<FrameSample> frames;
    frames.swap(s.frames);
    std::deque<RegionEvt> evts;
    evts.swap(s.regionEvts);
    s.killEvents.clear();
    for (Chunks& c : s.chunk) c = Chunks();
    s.renderFrame.clear();
    s.tSent = s.tAMC = s.tShiftDone = s.tVehUpd = s.tAvUpd = -1;
    s.vehUpdRegion.clear(); s.avUpdRegion.clear(); s.vehicleName.clear();
    s.childKills = s.otherKills = s.unknownKills = s.deferredKills = 0;
    s.keep0 = LLCrossingKeep::snapshot();
    s.copyFired = false; s.shiftFramesLeft = 0;
    s.crossFrameMs = s.nextFrameMs = -1.f;
    s.postN = 0; s.postWorst = 0.f; s.postWorstT = 0.0;
    s.vehId.setNull(); s.vehLocal = s.vehIp = s.vehPort = 0;
    s.attach.clear(); s.attachIdx.clear();
    s.haveAMC = false; s.inputSeen = false; s.tControl = -1;
    s.destPre = destinationState(new_region_handle, sim_host, now);
    s.destAction.clear();
    s.farSent = gAgentCamera.getNetFarSent();
    s.speed = gAgentCamera.getNetMoveSpeed();
    s.fast = gAgentCamera.isNetFastMoving();
    s.regionsAtCross = LLWorld::instanceExists() ? (U32)LLWorld::getInstance()->getRegionList().size() : 0;
    s.anchorConnects = 0;
    s.tLastNewCreate = s.tFullFrame = -1;
    s.newCreates = 0;
    {
        // #850: our vehicle is often killed by the region we are leaving shortly BEFORE
        // CrossedRegion; record it in this crossing's kills line.
        const std::string own_veh = LLCrossingKeep::ownVehicleKillBefore(10.0);
        if (!own_veh.empty()) s.killEvents.push_back(own_veh);
    }

    s.seq = seq;
    s.t0 = now;
    s.newHandle = new_region_handle;
    s.oldRegion = gAgent.getRegion();
    s.oldLabel = regionLabel(s.oldRegion);
    U32 x = 0, y = 0;
    from_region_handle(new_region_handle, &x, &y);
    {
        char b[64];
        std::snprintf(b, sizeof(b), "(%u,%u)", x / 256u, y / 256u);
        s.newLabel = b;
    }

    s.preN = 0; s.preWorst = 0.f;
    for (const FrameSample& f : frames)
    {
        if (f.t >= now - kWindowPreS) { ++s.preN; if (f.ms > s.preWorst) s.preWorst = f.ms; }
    }
    s.frames.swap(frames);
    s.regionEvts.swap(evts);

    s.mode = "walking";
    if (gAgentAvatarp)
    {
        LLViewerObject* parent = static_cast<LLViewerObject*>(gAgentAvatarp->getParent());
        if (parent)
        {
            s.mode = "seated";
            LLViewerObject* root = parent->getRootEdit();
            if (!root) root = parent;
            s.vehId = root->getID();
            s.vehLocal = root->getLocalID();
            if (const LLViewerRegion* r = root->getRegion())
            {
                const LLHost& h = r->getHost();
                s.vehIp = h.getAddress();
                s.vehPort = h.getPort();
            }
            s.vehicleName = s.vehId.asString().substr(0, 8);
        }
        else if (gAgent.getFlying())
        {
            s.mode = "flying";
        }
    }
    // Worn attachments (rigid + HUD roots): the list we expect the new region to re-announce.
    if (gAgentAvatarp)
    {
        for (const auto& kv : gAgentAvatarp->mAttachmentPoints)
        {
            LLViewerJointAttachment* ap = kv.second;
            if (!ap) continue;
            for (const LLPointer<LLViewerObject>& objp : ap->mAttachedObjects)
            {
                if (objp.isNull() || s.attachIdx.count(objp->getID())) continue;
                Attach a;
                a.id = objp->getID();
                a.item = objp->getAttachmentItemID();
                a.name = objp->getAttachmentItemName();
                if (a.name.empty()) a.name = ap->getName();
                if (a.name.empty()) a.name = a.id.asString().substr(0, 8);
                s.attachIdx[a.id] = s.attach.size();
                s.attach.push_back(std::move(a));
            }
        }
    }
    // Drop rolling state for anything no longer worn.
    for (auto it = s.slow.begin(); it != s.slow.end();)
    {
        bool worn = false;
        for (const Attach& a : s.attach) if (a.item == it->first) { worn = true; break; }
        if (worn) ++it; else it = s.slow.erase(it);
    }
    s.active.store(true);
}

// Shows queued slow-attachment notifications. Must be called WITHOUT mtx held.
static void drainWarnings()
{
    State& s = S();
    if (!s.hasPending.load(std::memory_order_relaxed)) return;
    std::vector<std::pair<std::string, std::string>> w;
    {
        std::lock_guard<std::mutex> lk(s.mtx);
        w.swap(s.pendingWarn);
        s.hasPending.store(false);
    }
    for (const auto& p : w)
    {
        LLSD args;
        args["NAME"] = p.first;
        args["SECONDS"] = p.second;
        LLNotificationsUtil::add("SlowCrossingAttachment", args, LLSD(), &onSlowAttachResponse);
    }
}

void onCompleteMovementSent()
{
    if (!active()) return;
    State& s = S();
    std::lock_guard<std::mutex> lk(s.mtx);
    if (s.tSent < 0) s.tSent = LLFrameTimer::getElapsedSeconds();
}

void onAgentMovementComplete()
{
    LLMemDiag::noteRegionCrossing("movement-complete");   // #700 pre-disconnect trace
    if (!active()) return;
    State& s = S();
    std::lock_guard<std::mutex> lk(s.mtx);
    if (s.tAMC < 0)
    {
        s.tAMC = LLFrameTimer::getElapsedSeconds();
        if (gAgentAvatarp) { s.posAMC = gAgentAvatarp->getPositionGlobal(); s.haveAMC = true; }
    }
}

void onShiftDone()
{
    if (!active()) return;
    State& s = S();
    std::lock_guard<std::mutex> lk(s.mtx);
    s.tShiftDone = LLFrameTimer::getElapsedSeconds();
    s.shiftFramesLeft = 2;
    s.copyFired = false;
    s.copyArmed.store(true);
}

void onFrame(F32 dt)
{
    State& s = S();
    const F64 now = LLFrameTimer::getElapsedSeconds();
    const float ms = dt * 1000.f;
    {
    std::lock_guard<std::mutex> lk(s.mtx);
    s.frames.push_back({now, ms});
    while (!s.frames.empty() && s.frames.front().t < now - kWindowPreS) s.frames.pop_front();
    while (!s.regionEvts.empty() && s.regionEvts.front().t < now - 60.0) s.regionEvts.pop_front();

    if (s.active.load())
    {
    const double rel = now - s.t0;
    if (rel <= kWindowPostS)
    {
        ++s.postN;
        if (ms > s.postWorst) { s.postWorst = ms; s.postWorstT = rel; }
    }
    if (s.shiftFramesLeft > 0)
    {
        // The interval reported at the first update() after the shift spans the network
        // phase that ran the shift, i.e. it is the crossing frame.
        if (s.shiftFramesLeft == 2) s.crossFrameMs = ms; else s.nextFrameMs = ms;
        --s.shiftFramesLeft;
    }
    // #850 first_full_frame: the new region's content has settled once no object was
    // created in it for kFullQuietS after AgentMovementComplete (a proxy for "the first
    // fully rendered frame": everything the region streams goes through object creation).
    if (s.tAMC >= 0 && s.tFullFrame < 0)
    {
        const F64 last = std::max(s.tAMC, s.tLastNewCreate);
        if (now - last >= kFullQuietS) s.tFullFrame = last;
    }
    bool missing = s.tAMC >= 0 && s.tFullFrame < 0;
    for (const Attach& a : s.attach) if (a.tArr < 0) { missing = true; break; }
    if (rel >= (missing ? kHandoffMaxS : kFlushAfterS)) emitLocked(s, false);
    }
    }
    drainWarnings();
}

void flush()
{
    State& s = S();
    std::lock_guard<std::mutex> lk(s.mtx);
    emitLocked(s, true);
}

void onKill(U32 local_id, U32 ip, U32 port, const LLUUID& resolved_id, LLViewerObject* objectp)
{
    if (!active()) return;
    State& s = S();
    std::lock_guard<std::mutex> lk(s.mtx);
    const F64 now = LLFrameTimer::getElapsedSeconds();
    char b[160];
    if (resolved_id.isNull())
    {
        if (s.vehId.notNull() && local_id == s.vehLocal && ip == s.vehIp && port == s.vehPort)
        {
            std::snprintf(b, sizeof(b), "vehicle_root=IGNORED(already re-keyed)@%+.1f", (now - s.t0) * 1000.0);
            s.killEvents.push_back(b);
        }
        else
        {
            ++s.unknownKills;
        }
        return;
    }
    if (objectp && s.vehId.notNull())
    {
        if (objectp->getID() == s.vehId)
        {
            std::snprintf(b, sizeof(b), "vehicle_root=KILLED@%+.1f", (now - s.t0) * 1000.0);
            s.killEvents.push_back(b);
            return;
        }
        LLViewerObject* root = objectp->getRootEdit();
        if (root && root->getID() == s.vehId) { ++s.childKills; return; }
    }
    ++s.otherKills;
}

void onKillDeferred(LLViewerObject* objectp)
{
    if (!active() || !objectp) return;
    State& s = S();
    std::lock_guard<std::mutex> lk(s.mtx);
    ++s.deferredKills;
    if (s.vehId.notNull() && objectp->getID() == s.vehId)
    {
        char b[96];
        const F64 now = LLFrameTimer::getElapsedSeconds();
        std::snprintf(b, sizeof(b), "vehicle_root=DEFERRED@%+.1f", (now - s.t0) * 1000.0);
        s.killEvents.push_back(b);
    }
}

void onObjectUpdate(LLViewerObject* objectp)
{
    if (!active() || !objectp) return;
    State& s = S();
    const bool is_avatar = (objectp == gAgentAvatarp.get());
    bool is_vehicle = false;
    if (!is_avatar && s.vehId.notNull())
    {
        // unlocked read of vehId is fine: only written in onCrossedRegion on the same (main) thread
        is_vehicle = (objectp->getID() == s.vehId);
    }
    // unlocked read of attachIdx: only written in onCrossedRegion on the same (main) thread
    const bool is_attach = !is_avatar && !is_vehicle && !s.attachIdx.empty() && s.attachIdx.count(objectp->getID()) != 0;
    if (!is_avatar && !is_vehicle && !is_attach) return;
    if (objectp->getRegion() == s.oldRegion) return;   // we want the first update from the NEW sim
    std::lock_guard<std::mutex> lk(s.mtx);
    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (is_attach)
    {
        auto it = s.attachIdx.find(objectp->getID());
        if (it != s.attachIdx.end() && s.attach[it->second].tArr < 0) s.attach[it->second].tArr = now;
        return;
    }
    if (is_avatar && s.tAMC >= 0 && s.tControl < 0)
    {
        // Control restored: server motion agrees with held input, or the server position moved.
        const U32 f = sLastSentControlFlags.load(std::memory_order_relaxed);
        LLVector3 dir;
        if (f & AGENT_CONTROL_AT_POS)   dir.mV[VX] += 1.f;
        if (f & AGENT_CONTROL_AT_NEG)   dir.mV[VX] -= 1.f;
        if (f & AGENT_CONTROL_LEFT_POS) dir.mV[VY] += 1.f;
        if (f & AGENT_CONTROL_LEFT_NEG) dir.mV[VY] -= 1.f;
        if (f & AGENT_CONTROL_UP_POS)   dir.mV[VZ] += 1.f;
        if (f & AGENT_CONTROL_UP_NEG)   dir.mV[VZ] -= 1.f;
        if (dir.magVecSquared() > 0.f)
        {
            s.inputSeen = true;
            dir.normalize();
            dir = dir * objectp->getRotationRegion();      // agent frame -> region frame
            const LLVector3& v = objectp->getVelocity();
            const bool aligned = v.magVec() > 0.3f && (dir * v) > 0.f;
            const bool moved = s.haveAMC && (objectp->getPositionGlobal() - s.posAMC).magVec() > 0.5;
            if (aligned || moved) s.tControl = now;
        }
    }
    if (is_avatar && s.tAvUpd < 0) { s.tAvUpd = now; s.avUpdRegion = regionLabel(objectp->getRegion()); }
    if (is_vehicle && s.tVehUpd < 0) { s.tVehUpd = now; s.vehUpdRegion = regionLabel(objectp->getRegion()); }
}

void noteDestinationAction(const std::string& action)
{
    if (!active()) return;
    State& s = S();
    std::lock_guard<std::mutex> lk(s.mtx);
    s.destAction = action;
}

void noteAnchorConnects(U32 regions_connected)
{
    if (!active()) return;
    State& s = S();
    std::lock_guard<std::mutex> lk(s.mtx);
    s.anchorConnects += regions_connected;
}

void onObjectCreated(const LLViewerRegion* regionp)
{
    if (!active() || !regionp) return;
    State& s = S();
    // unlocked read of newHandle: only written in onCrossedRegion on the same (main) thread
    if (regionp->getHandle() != s.newHandle) return;
    std::lock_guard<std::mutex> lk(s.mtx);
    s.tLastNewCreate = LLFrameTimer::getElapsedSeconds();
    ++s.newCreates;
}

void noteRenderFrame(const std::string& text)
{
    State& s = S();
    if (!s.active.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lk(s.mtx);
    if (!s.renderFrame.empty()) s.renderFrame += " | ";
    s.renderFrame += text;
}

void regionEvent(const std::string& text)
{
    State& s = S();
    std::lock_guard<std::mutex> lk(s.mtx);
    s.regionEvts.push_back({LLFrameTimer::getElapsedSeconds(), text});
    while (s.regionEvts.size() > kMaxRegionEvt) s.regionEvts.pop_front();
}

void addChunk(Chunk c, double ms, U32 a, U32 b)
{
    if (!active() || c < 0 || c >= CH_COUNT) return;
    State& s = S();
    std::lock_guard<std::mutex> lk(s.mtx);
    Chunks& k = s.chunk[c];
    k.ms += ms; ++k.n; k.a += a; k.b += b;
}

bool takeCopyProbe()
{
    State& s = S();
    if (!s.copyArmed.load(std::memory_order_relaxed)) return false;
    if (!s.copyArmed.exchange(false)) return false;
    s.uploadArmed.store(true);
    return true;
}

bool takeUploadProbe()
{
    return S().uploadArmed.exchange(false);
}

RegionTimed::RegionTimed(const char* kind, const LLViewerRegion* regionp)
    : mKind(kind), mLabel(regionLabel(regionp)), mT0(nowMs()) {}

void RegionTimed::note(const char* what, double ms)
{
    char b[64];
    std::snprintf(b, sizeof(b), " %s=%.2fms", what, ms);
    mNotes += b;
}

void RegionTimed::noteCount(const char* what, U32 n)
{
    char b[64];
    std::snprintf(b, sizeof(b), " %s=%u", what, n);
    mNotes += b;
}

RegionTimed::~RegionTimed()
{
    char b[64];
    std::snprintf(b, sizeof(b), " total=%.2fms", nowMs() - mT0);
    regionEvent(mKind + " " + mLabel + b + mNotes);
}

} // namespace LLCrossingTrace
