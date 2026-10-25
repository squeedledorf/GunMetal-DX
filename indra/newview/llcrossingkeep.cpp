/**
 * @file llcrossingkeep.cpp
 * @brief Deferred kills across region crossings (avatars keep their assets). See header. #607
 */

#include "llviewerprecompiledheaders.h"

#include "llcrossingkeep.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <unordered_map>
#include <vector>

#include "llagent.h"
#include "llagentcamera.h"
#include "llcontrolavatar.h"
#include "llcrossingtrace.h"
#include "llframetimer.h"
#include "llmemorygovernor.h"
#include "llselectmgr.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llvoavatarself.h"
#include "llworld.h"
#include "vkgfx/llvulkancontext.h"

namespace
{
    // Hard cap on pending (deferred) kills. A crossing moves one avatar + its attachments
    // (dozens of prims at most) per crossing avatar; 512 covers a busy crowd. Overflow
    // applies the kill at once (counted), so nothing is ever held unbounded.
    constexpr size_t kMaxPending   = 512;
    // Kill -> re-create churn window and the time-to-rezzed watch. Bounded maps.
    constexpr F64    kChurnWindowS = 60.0;
    constexpr size_t kMaxRecent    = 4096;
    constexpr F64    kRezWatchS    = 180.0;
    constexpr size_t kMaxRezWatch  = 512;
    constexpr F64    kLookAheadS   = 1.0;    // velocity look-ahead for the edge test
    constexpr F64    kProbeAcrossM = 2.0;    // probe this far across an edge
    // #850 adaptive near-edge grace: observed handoff durations (re-key latency of edge
    // deferrals; kill -> re-create time of edge deferrals that expired) over the last
    // kGraceObsMax handoffs / kGraceObsAgeS seconds; grace = headroom x the longest.
    constexpr size_t kGraceObsMax   = 32;
    constexpr F64    kGraceObsAgeS  = 600.0;
    constexpr F64    kGraceHeadroom = 1.25;

    enum Kind : U8 { K_SELF_ATTACH = 0, K_SELF_VEHICLE, K_AVATAR, K_ATTACH, K_ANIMESH, K_VEHICLE, K_COUNT };
    const char* kKindName[K_COUNT] = { "self_attach", "self_vehicle", "avatar", "attach", "animesh", "vehicle" };
    // C_SELF_EDGE (#850): our own vehicle killed by the agent region near a connected edge
    // (the vehicle is handed over before its riders): short grace, promoted to C_SELF when
    // CrossedRegion arrives.
    enum Cls : U8 { C_NONE = 0, C_SELF, C_CERTAIN, C_EDGE, C_SELF_EDGE };

    // Snapshot slots (LLCrossingKeep::Snapshot::v)
    enum Ctr
    {
        S_DEF_SELF = 0, S_DEF_CERTAIN, S_DEF_EDGE, S_REKEYED, S_EXPIRED, S_GONE, S_OVERFLOW, S_PRESSURE,
        S_IMM_AVATAR, S_IMM_ATTACH, S_CHURN_AVATAR, S_CHURN_OTHER, S_REKEY_MS, S_REZ_N, S_REZ_MS, S_IMM_SELF,
        S_IMM_SELF_VEHICLE,     // #850 own vehicle (any prim) killed by the agent region, applied at once
        S_DEF_SELF_VEH_EDGE,    // #850 ... deferred on the edge rule
        S_COUNT
    };
    static_assert(S_COUNT <= 24, "LLCrossingKeep::Snapshot holds 24 counters");

    struct Pending
    {
        U32  local_id = 0, ip = 0, port = 0;
        F64  t0 = 0.0, deadline = 0.0;
        LLUUID anchor;          // the avatar / linkset root this kill rides on
        Cls  cls = C_NONE;
        Kind kind = K_AVATAR;
    };
    // t = kill applied; t_first = the original kill (deferred t0, = t if immediate);
    // edge = an expired near-edge deferral (its re-create times a handoff, #850).
    struct Recent { F64 t; bool avatar; F64 t_first; bool edge; };
    struct RezWatch { F64 t_kill, t_create; };

    struct State
    {
        std::unordered_map<LLUUID, Pending>  pending;
        std::unordered_map<LLUUID, Recent>   recent;    // killed (applied) avatar-class objects
        std::unordered_map<LLUUID, RezWatch> rez;       // re-created avatars waiting to rez
        U64 ctr[S_COUNT] = {};
        U64 kindDeferred[K_COUNT] = {};
        size_t peak = 0;
        F64 rekeyMaxMs = 0.0;
        F64 nextPrune = 0.0;
        F64 nextLog = 0.0;
        U64 loggedAt[S_COUNT] = {};
        std::deque<std::pair<F64, F64>> graceObs;   // #850 (observed at, handoff seconds)
        F64  ownVehKillT = -1.0;                     // #850 last own-vehicle root kill from the agent region
        bool ownVehKillDeferred = false;
        F64 graceLogged = -1.0;                      // last edge grace logged
    };
    State& S() { static State s; return s; }

    F64 now() { return LLFrameTimer::getElapsedSeconds(); }

    bool enabled()
    {
        static LLCachedControl<bool> on(gSavedSettings, "SLCrossingKeep", true);
        return on;
    }

    bool linksetCarriesAvatar(LLViewerObject* root, bool& self)
    {
        self = false;
        if (isAgentAvatarValid() && gAgentAvatarp->getParent() && gAgentAvatarp->getRootEdit() == root)
        {
            self = true;
            return true;
        }
        // Another avatar seated on this linkset: avatars are children of the seat prim
        // (root or a child prim). Only moving linksets cross; a parked one never needs this.
        if (root->getVelocity().magVecSquared() < 0.25f) return false;
        for (LLViewerObject* c : root->getChildren())
        {
            if (c->asAvatar()) return true;
            for (LLViewerObject* g : c->getChildren())
                if (g->asAvatar()) return true;
        }
        return false;
    }

    // The object the keep decision is made on (avatar or linkset root) and its kind.
    LLViewerObject* anchorOf(LLViewerObject* o, Kind& kind)
    {
        if (LLVOAvatar* av = o->asAvatar())
        {
            if (av->isControlAvatar() || av->isSelf()) return nullptr;   // ours is never killed
            kind = K_AVATAR;
            return av;
        }
        if (LLVOAvatar* av = o->getAvatarAncestor())
        {
            if (av->isControlAvatar()) return nullptr;
            kind = av->isSelf() ? K_SELF_ATTACH : K_ATTACH;
            return av;
        }
        LLViewerObject* root = o->getRootEdit();
        if (!root) return nullptr;
        bool self = false;
        if (linksetCarriesAvatar(root, self))
        {
            kind = self ? K_SELF_VEHICLE : K_VEHICLE;
            return root;
        }
        if (root->getControlAvatar())
        {
            kind = K_ANIMESH;
            return root;
        }
        return nullptr;
    }

    // Near an edge of `region` with a CONNECTED region across it (the one that would send
    // the re-keying update), looking ahead along the current horizontal velocity.
    bool nearEdgeToConnected(LLViewerObject* anchor, LLViewerRegion* region)
    {
        static LLCachedControl<F32> margin_m(gSavedSettings, "SLCrossingKeepEdgeMargin", 16.f);
        LLWorld* world = LLWorld::getInstance();
        const LLVector3d pos = anchor->getPositionGlobal();
        const LLVector3d org = region->getOriginGlobal();
        const F64 w = (F64)region->getWidth();
        LLViewerObject* mover = anchor->getParent() ? anchor->getRootEdit() : anchor;   // seated: the vehicle moves
        const LLVector3 vel = mover ? mover->getVelocity() : LLVector3::zero;
        const F64 speed = std::sqrt((F64)vel.mV[VX] * vel.mV[VX] + (F64)vel.mV[VY] * vel.mV[VY]);
        const F64 margin = (F64)llmax((F32)margin_m, 0.f) + speed * kLookAheadS;
        const F64 lx = pos.mdV[VX] - org.mdV[VX], ly = pos.mdV[VY] - org.mdV[VY];
        const F64 cx = llclamp(lx, 0.0, w), cy = llclamp(ly, 0.0, w);

        auto probe = [&](F64 x, F64 y) -> bool
        {
            LLViewerRegion* r = world->getRegionFromPosGlobal(LLVector3d(org.mdV[VX] + x, org.mdV[VY] + y, pos.mdV[VZ]));
            return r && r != region;
        };
        if (lx <= margin && probe(-kProbeAcrossM, cy)) return true;
        if (w - lx <= margin && probe(w + kProbeAcrossM, cy)) return true;
        if (ly <= margin && probe(cx, -kProbeAcrossM)) return true;
        if (w - ly <= margin && probe(cx, w + kProbeAcrossM)) return true;
        const F64 ax = lx + vel.mV[VX] * kLookAheadS, ay = ly + vel.mV[VY] * kLookAheadS;
        if ((ax < 0.0 || ax > w || ay < 0.0 || ay > w) && probe(ax, ay)) return true;
        return false;
    }

    bool withinNetworkDrawDistance(LLViewerObject* anchor)
    {
        const LLVector3d d = anchor->getPositionGlobal() - gAgent.getPositionGlobal();
        const F64 dd = (F64)gAgentCamera.getNetworkDrawDistance();
        return d.mdV[VX] * d.mdV[VX] + d.mdV[VY] * d.mdV[VY] <= dd * dd;
    }

    bool underPressure()
    {
        if (LLMemoryGovernor::isEnabled() && LLMemoryGovernor::getState() != LLMemoryGovernor::COMFORTABLE) return true;
        return LLVulkanContext::instance().vramSummary().mode >= 2u;   // VRAM ladder in its reserve
    }

    F64 graceMax()
    {
        static LLCachedControl<F32> max_s(gSavedSettings, "SLCrossingKeepGraceMaxSeconds", 12.f);
        return (F64)llmax((F32)max_s, 0.f);
    }

    // #850: near-edge grace. A region handoff freezes the object 0.5-10 s, ~1 s typical;
    // a fixed short grace killed (and later re-created) everything whose handoff took
    // longer. Base = SLCrossingKeepGraceSeconds; with SLCrossingKeepAdaptiveGrace it rises
    // to headroom x the longest recently observed handoff, up to SLCrossingKeepGraceMaxSeconds.
    F64 edgeGrace(State& s, F64 t)
    {
        static LLCachedControl<F32> edge_s(gSavedSettings, "SLCrossingKeepGraceSeconds", 6.f);
        static LLCachedControl<bool> adaptive(gSavedSettings, "SLCrossingKeepAdaptiveGrace", true);
        const F64 base = (F64)llmax((F32)edge_s, 0.f);
        while (!s.graceObs.empty() && (s.graceObs.size() > kGraceObsMax || t - s.graceObs.front().first > kGraceObsAgeS))
            s.graceObs.pop_front();
        if (!adaptive || s.graceObs.empty()) return base;
        F64 longest = 0.0;
        for (const auto& o : s.graceObs) longest = llmax(longest, o.second);
        return llclamp(longest * kGraceHeadroom, base, llmax(base, graceMax()));
    }

    F64 graceFor(State& s, Cls c, F64 t)
    {
        static LLCachedControl<F32> certain_s(gSavedSettings, "SLCrossingKeepCertainGraceSeconds", 20.f);
        static LLCachedControl<F32> self_edge_s(gSavedSettings, "SLCrossingKeepOwnVehicleEdgeSeconds", 3.f);
        if (c == C_EDGE) return edgeGrace(s, t);
        if (c == C_SELF_EDGE) return (F64)llclamp((F32)self_edge_s, 0.f, 10.f);
        return (F64)llmax((F32)certain_s, 0.f);
    }

    // Classes exempt from the RAM / VRAM pressure flush (our own avatar's things).
    bool isSelfClass(Cls c) { return c == C_SELF || c == C_SELF_EDGE; }

    void observeHandoff(State& s, F64 t, F64 seconds, const char* what)
    {
        if (seconds <= 0.0) return;
        s.graceObs.emplace_back(t, llmin(seconds, llmax(graceMax(), 0.1)));
        const F64 g = edgeGrace(s, t);
        if (s.graceLogged < 0.0 || std::fabs(g - s.graceLogged) >= 1.0)
        {
            LL_INFOS("CrossingKeep") << "near-edge grace " << llformat("%.1f", s.graceLogged < 0.0 ? 0.0 : s.graceLogged)
                                     << " -> " << llformat("%.1f", g) << " s (" << what << " " << llformat("%.1f", seconds)
                                     << " s; " << s.graceObs.size() << " handoffs observed)" << LL_ENDL;
            s.graceLogged = g;
        }
    }

    void prune(State& s, F64 t);

    // Applied kill bookkeeping (churn watch). t_first = the original kill time; edge = an
    // expired near-edge deferral.
    void noteKilledAt(LLViewerObject* objectp, F64 t_first, bool edge)
    {
        if (!objectp) return;
        Kind kind = K_AVATAR;
        if (!anchorOf(objectp, kind)) return;
        State& s = S();
        const bool avatar = objectp->asAvatar() != nullptr;
        if (avatar) ++s.ctr[S_IMM_AVATAR];
        else if (kind == K_ATTACH || kind == K_SELF_ATTACH) ++s.ctr[S_IMM_ATTACH];
        if (s.recent.size() >= kMaxRecent) prune(s, now());
        if (s.recent.size() < kMaxRecent) s.recent[objectp->getID()] = Recent{ now(), avatar, t_first, edge };
    }

    // The deferred kill is applied now: the same tail as process_kill_object.
    void applyKill(LLViewerObject* objectp, F64 t_first, bool edge)
    {
        const LLUUID id = objectp->getID();
        noteKilledAt(objectp, t_first, edge);
        gObjectList.killObject(objectp);
        LLObjectSignaledAnimationMap::instance().erase(id);
        LLSelectMgr::getInstance()->removeObjectFromSelections(id);
    }

    void prune(State& s, F64 t)
    {
        for (auto it = s.recent.begin(); it != s.recent.end();)
            it = (t - it->second.t > kChurnWindowS) ? s.recent.erase(it) : std::next(it);
        for (auto it = s.rez.begin(); it != s.rez.end();)
            it = (t - it->second.t_create > kRezWatchS) ? s.rez.erase(it) : std::next(it);
    }

    void maybeLog(State& s, F64 t)
    {
        if (t < s.nextLog) return;
        s.nextLog = t + 30.0;
        if (std::equal(std::begin(s.ctr), std::end(s.ctr), std::begin(s.loggedAt))) return;
        std::copy(std::begin(s.ctr), std::end(s.ctr), std::begin(s.loggedAt));
        LL_INFOS("CrossingKeep") << LLCrossingKeep::statsLine() << LL_ENDL;
    }
}

namespace LLCrossingKeep
{

bool deferKill(LLViewerObject* objectp, LLViewerRegion* sender_region, U32 local_id, U32 sender_ip, U32 sender_port)
{
    if (!objectp || objectp->isDead() || !sender_region || !enabled()) return false;
    State& s = S();
    if (s.pending.count(objectp->getID())) return true;   // duplicate kill: already deferred

    Kind kind = K_AVATAR;
    LLViewerObject* anchor = anchorOf(objectp, kind);
    if (!anchor) return false;

    Cls cls = C_NONE;
    if (kind == K_SELF_ATTACH || kind == K_SELF_VEHICLE)
    {
        // A real detach / derez comes from the region we are in. A kill from the region we
        // left (after AgentMovementComplete moved us), from the old region mid-handoff, or
        // during a teleport is the losing side of the re-key race. Temp attachments really
        // do go away on teleport.
        const bool handoff = LLCrossingTrace::awaitingMovementComplete();
        const bool tp = gAgent.getTeleportState() != LLAgent::TELEPORT_NONE;
        const bool temp = objectp->isTempAttachment() && !LLCrossingTrace::active();
        if (!temp && (sender_region != gAgent.getRegion() || handoff || tp)) cls = C_SELF;
        if (cls == C_NONE && kind == K_SELF_VEHICLE)
        {
            // #850: the vehicle is handed to the next region before its riders, so the agent
            // region kills our vehicle a moment BEFORE CrossedRegion: applied at once, that
            // unseats us and re-seats us when the new region re-sends it. Defer it briefly,
            // but only near an edge with a connected region across it (an ejection / derez
            // mid-region must still apply promptly), and only for the vehicle (never our
            // attachments). CrossedRegion or the new region's update ends the wait.
            static LLCachedControl<bool> own_vehicle_edge(gSavedSettings, "SLCrossingKeepOwnVehicleEdge", true);
            const bool root = objectp == anchor;
            const bool near_edge = own_vehicle_edge && nearEdgeToConnected(anchor, sender_region);
            if (near_edge) cls = C_SELF_EDGE;
            if (root)
            {
                s.ownVehKillT = now();
                s.ownVehKillDeferred = near_edge;
                LL_INFOS("CrossingKeep") << "own vehicle " << anchor->getID() << " killed by the agent region "
                                         << LLCrossingTrace::regionLabel(sender_region) << ": "
                                         << (near_edge ? "deferred (near a connected edge, waiting for the crossing)"
                                                       : (own_vehicle_edge ? "applied now (not near a connected edge)"
                                                                           : "applied now (SLCrossingKeepOwnVehicleEdge off)"))
                                         << LL_ENDL;
            }
            if (!near_edge) ++s.ctr[S_IMM_SELF_VEHICLE];
        }
        if (cls == C_NONE) { ++s.ctr[S_IMM_SELF]; return false; }
    }
    else if (anchor->getRegion() && anchor->getRegion() != sender_region)
    {
        cls = C_CERTAIN;   // its avatar / root already re-keyed to the new region
    }
    else if (withinNetworkDrawDistance(anchor) && nearEdgeToConnected(anchor, sender_region))
    {
        cls = C_EDGE;
    }

    if (cls == C_NONE || (!isSelfClass(cls) && underPressure()))
    {
        if (cls != C_NONE) ++s.ctr[S_PRESSURE];
        return false;
    }
    if (s.pending.size() >= kMaxPending)
    {
        ++s.ctr[S_OVERFLOW];
        return false;
    }

    const F64 t = now();
    Pending& p = s.pending[objectp->getID()];
    p.local_id = local_id; p.ip = sender_ip; p.port = sender_port;
    p.t0 = t; p.deadline = t + graceFor(s, cls, t);
    p.anchor = anchor->getID(); p.cls = cls; p.kind = kind;
    s.peak = llmax(s.peak, s.pending.size());
    ++s.ctr[cls == C_SELF ? S_DEF_SELF : (cls == C_SELF_EDGE ? S_DEF_SELF_VEH_EDGE
                                          : (cls == C_CERTAIN ? S_DEF_CERTAIN : S_DEF_EDGE))];
    ++s.kindDeferred[kind];
    LL_DEBUGS("CrossingKeep") << "deferred kill of " << kKindName[kind] << " " << objectp->getID()
                              << " local " << local_id << " from " << LLCrossingTrace::regionLabel(sender_region)
                              << (cls == C_EDGE ? " (edge)" : (cls == C_CERTAIN ? " (anchor re-keyed)"
                                          : (cls == C_SELF_EDGE ? " (own vehicle, edge)" : " (self)"))) << LL_ENDL;
    return true;
}

void noteKilled(LLViewerObject* objectp)
{
    noteKilledAt(objectp, now(), false);
}

void onRekey(LLViewerObject* objectp)
{
    State& s = S();
    if (s.pending.empty() || !objectp) return;
    auto it = s.pending.find(objectp->getID());
    if (it == s.pending.end()) return;
    const F64 t = now();
    const F64 ms = (t - it->second.t0) * 1000.0;
    // #850: one observation per handoff: the anchor itself (avatar / linkset root), not each
    // of its attachments or prims.
    if (it->second.cls == C_EDGE && it->second.anchor == it->first) observeHandoff(s, t, ms / 1000.0, "re-keyed after");
    s.ctr[S_REKEYED]++;
    s.ctr[S_REKEY_MS] += (U64)ms;
    s.rekeyMaxMs = llmax(s.rekeyMaxMs, ms);
    const LLUUID id = it->first;
    s.pending.erase(it);
    // The anchor moved: its attachments / children still pending are now certain to follow.
    const F64 certain = graceFor(s, C_CERTAIN, t);
    for (auto& kv : s.pending)
    {
        if (kv.second.anchor == id && kv.second.cls == C_EDGE)
        {
            kv.second.cls = C_CERTAIN;
            kv.second.deadline = llmax(kv.second.deadline, kv.second.t0 + certain);
        }
    }
}

void onCreated(LLViewerObject* objectp)
{
    State& s = S();
    if (s.recent.empty() || !objectp) return;
    auto it = s.recent.find(objectp->getID());
    if (it == s.recent.end()) return;
    const F64 t = now();
    if (it->second.edge && it->second.avatar)   // one per handoff: the avatar, not its parts
    {
        // #850: an expired near-edge deferral came back: the handoff took this long from
        // the original kill. Implausibly long gaps (logout + relog) are not handoffs.
        const F64 handoff = t - it->second.t_first;
        if (handoff <= 1.5 * graceMax()) observeHandoff(s, t, handoff, "re-created after kill, handoff");
    }
    if (it->second.avatar)
    {
        ++s.ctr[S_CHURN_AVATAR];
        if (s.rez.size() < kMaxRezWatch) s.rez[objectp->getID()] = RezWatch{ it->second.t, t };
        LL_INFOS("CrossingKeep") << "avatar " << objectp->getID() << " re-created "
                                 << llformat("%.0f", (t - it->second.t) * 1000.0) << " ms after its kill" << LL_ENDL;
    }
    else
    {
        ++s.ctr[S_CHURN_OTHER];
    }
    s.recent.erase(it);
}

void onAvatarFullyLoaded(LLVOAvatar* avatarp)
{
    State& s = S();
    if (s.rez.empty() || !avatarp) return;
    auto it = s.rez.find(avatarp->getID());
    if (it == s.rez.end()) return;
    const F64 t = now();
    const F64 ms = (t - it->second.t_create) * 1000.0;
    ++s.ctr[S_REZ_N];
    s.ctr[S_REZ_MS] += (U64)ms;
    LL_INFOS("CrossingKeep") << "re-created avatar " << avatarp->getID() << " fully rezzed "
                             << llformat("%.1f", ms / 1000.0) << " s after re-create ("
                             << llformat("%.1f", (t - it->second.t_kill)) << " s after its kill)" << LL_ENDL;
    s.rez.erase(it);
}

void tick()
{
    State& s = S();
    const F64 t = now();
    if (t >= s.nextPrune)
    {
        s.nextPrune = t + 5.0;
        prune(s, t);
    }
    maybeLog(s, t);
    if (s.pending.empty()) return;

    bool any_other = false;
    for (const auto& kv : s.pending) if (!isSelfClass(kv.second.cls)) { any_other = true; break; }
    const bool pressure = any_other && underPressure();
    const bool off = !enabled();

    // #850: near-edge deadlines follow the grace in force now (it can rise while pending).
    const F64 edge_grace = edgeGrace(s, t);
    std::vector<std::pair<LLUUID, Pending>> due;
    for (auto& kv : s.pending)
    {
        if (kv.second.cls == C_EDGE) kv.second.deadline = llmax(kv.second.deadline, kv.second.t0 + edge_grace);
        if (off || t >= kv.second.deadline || (pressure && !isSelfClass(kv.second.cls))) due.push_back(kv);
    }
    for (const auto& d : due) s.pending.erase(d.first);

    for (const auto& d : due)
    {
        const Pending& p = d.second;
        LLViewerObject* objectp = gObjectList.findObject(d.first);
        if (!objectp || objectp->isDead())
        {
            ++s.ctr[S_GONE];   // its region went, or its avatar / root was killed with it
            continue;
        }
        LLViewerRegion* r = objectp->getRegion();
        const bool same_key = r && objectp->mLocalID == p.local_id
                              && r->getHost().getAddress() == p.ip && r->getHost().getPort() == p.port;
        if (!same_key)
        {
            ++s.ctr[S_REKEYED];   // re-keyed by a path that did not report it
            continue;
        }
        ++s.ctr[(t >= p.deadline) ? S_EXPIRED : S_PRESSURE];
        if (p.cls == C_SELF_EDGE && p.anchor == d.first)
        {
            LL_INFOS("CrossingKeep") << "own vehicle " << d.first << " kill applied after "
                                     << llformat("%.1f", t - p.t0) << " s: no crossing followed" << LL_ENDL;
        }
        LL_DEBUGS("CrossingKeep") << "applying deferred kill of " << kKindName[p.kind] << " " << d.first
                                  << " after " << llformat("%.0f", (t - p.t0) * 1000.0) << " ms" << LL_ENDL;
        applyKill(objectp, p.t0, p.cls == C_EDGE && t >= p.deadline);
    }
}

void onCrossedRegion()
{
    State& s = S();
    const F64 t = now();
    const F64 kill_t = s.ownVehKillT;
    s.ownVehKillT = -1.0;   // reported once (LLCrossingTrace::onCrossedRegion ran first)
    if (s.pending.empty()) return;
    const F64 certain = graceFor(s, C_SELF, t);
    U32 n = 0;
    for (auto& kv : s.pending)
    {
        if (kv.second.cls != C_SELF_EDGE) continue;
        kv.second.cls = C_SELF;
        kv.second.deadline = llmax(kv.second.deadline, kv.second.t0 + certain);
        ++n;
    }
    if (n)
    {
        LL_INFOS("CrossingKeep") << "CrossedRegion: " << n << " deferred own-vehicle kill(s) now wait for the new region"
                                 << " (deferred " << llformat("%.1f", kill_t >= 0.0 ? t - kill_t : 0.0)
                                 << " s before the crossing)" << LL_ENDL;
    }
}

std::string ownVehicleKillBefore(F64 window_s)
{
    const State& s = S();
    if (s.ownVehKillT < 0.0) return std::string();
    const F64 dt = now() - s.ownVehKillT;
    if (dt > window_s) return std::string();
    char b[96];
    std::snprintf(b, sizeof(b), "own_vehicle_kill=%+.0fms(%s)", -dt * 1000.0,
                  s.ownVehKillDeferred ? "deferred, edge" : "applied now");
    return b;
}

void clear()
{
    State& s = S();
    s.pending.clear();
    s.recent.clear();
    s.rez.clear();
}

Snapshot snapshot()
{
    Snapshot sn;
    const State& s = S();
    for (int i = 0; i < S_COUNT && i < 24; ++i) sn.v[i] = s.ctr[i];
    return sn;
}

std::string deltaLine(const Snapshot& since)
{
    State& s = S();
    U64 d[S_COUNT];
    for (int i = 0; i < S_COUNT; ++i) d[i] = s.ctr[i] - since.v[i];
    const F64 grace = edgeGrace(s, now());
    char b[896];
    std::snprintf(b, sizeof(b),
        "deferred self=%llu other_certain=%llu other_edge=%llu -> rekeyed=%llu (avg %.0f ms) expired=%llu gone=%llu"
        " | own_vehicle: edge_deferred=%llu applied_now=%llu"
        " | applied_now: self=%llu pressure=%llu overflow=%llu"
        " | killed avatars=%llu attachments=%llu | re-created avatars=%llu objects=%llu | pending=%zu"
        " | edge_grace=%.1fs(handoffs observed=%zu)",
        (unsigned long long)d[S_DEF_SELF], (unsigned long long)d[S_DEF_CERTAIN], (unsigned long long)d[S_DEF_EDGE],
        (unsigned long long)d[S_REKEYED], d[S_REKEYED] ? (double)d[S_REKEY_MS] / (double)d[S_REKEYED] : 0.0,
        (unsigned long long)d[S_EXPIRED], (unsigned long long)d[S_GONE],
        (unsigned long long)d[S_DEF_SELF_VEH_EDGE], (unsigned long long)d[S_IMM_SELF_VEHICLE],
        (unsigned long long)d[S_IMM_SELF], (unsigned long long)d[S_PRESSURE], (unsigned long long)d[S_OVERFLOW],
        (unsigned long long)d[S_IMM_AVATAR], (unsigned long long)d[S_IMM_ATTACH],
        (unsigned long long)d[S_CHURN_AVATAR], (unsigned long long)d[S_CHURN_OTHER], s.pending.size(),
        grace, s.graceObs.size());
    return b;
}

std::string statsLine()
{
    const State& s = S();
    char b[640];
    std::snprintf(b, sizeof(b),
        "pending live=%zu cap=%zu peak=%zu | session: deferred self=%llu certain=%llu edge=%llu"
        " [self_attach=%llu self_vehicle=%llu avatar=%llu attach=%llu animesh=%llu vehicle=%llu]"
        " rekeyed=%llu (avg %.0f max %.0f ms) expired=%llu gone=%llu pressure=%llu overflow=%llu"
        " | killed avatars=%llu attachments=%llu re-created avatars=%llu objects=%llu"
        " | re-created avatars rezzed=%llu (avg %.1f s) | churn watch=%zu rez watch=%zu",
        s.pending.size(), kMaxPending, s.peak,
        (unsigned long long)s.ctr[S_DEF_SELF], (unsigned long long)s.ctr[S_DEF_CERTAIN], (unsigned long long)s.ctr[S_DEF_EDGE],
        (unsigned long long)s.kindDeferred[K_SELF_ATTACH], (unsigned long long)s.kindDeferred[K_SELF_VEHICLE],
        (unsigned long long)s.kindDeferred[K_AVATAR], (unsigned long long)s.kindDeferred[K_ATTACH],
        (unsigned long long)s.kindDeferred[K_ANIMESH], (unsigned long long)s.kindDeferred[K_VEHICLE],
        (unsigned long long)s.ctr[S_REKEYED],
        s.ctr[S_REKEYED] ? (double)s.ctr[S_REKEY_MS] / (double)s.ctr[S_REKEYED] : 0.0, s.rekeyMaxMs,
        (unsigned long long)s.ctr[S_EXPIRED], (unsigned long long)s.ctr[S_GONE],
        (unsigned long long)s.ctr[S_PRESSURE], (unsigned long long)s.ctr[S_OVERFLOW],
        (unsigned long long)s.ctr[S_IMM_AVATAR], (unsigned long long)s.ctr[S_IMM_ATTACH],
        (unsigned long long)s.ctr[S_CHURN_AVATAR], (unsigned long long)s.ctr[S_CHURN_OTHER],
        (unsigned long long)s.ctr[S_REZ_N], s.ctr[S_REZ_N] ? (double)s.ctr[S_REZ_MS] / 1000.0 / (double)s.ctr[S_REZ_N] : 0.0,
        s.recent.size(), s.rez.size());
    return b;
}

} // namespace LLCrossingKeep
