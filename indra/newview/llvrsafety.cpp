/**
 * @file llvrsafety.cpp
 * @brief VR milestone M3 (#1076): the frame-rate safety governor. See llvrsafety.h.
 */
#include "llviewerprecompiledheaders.h"

#include "llvrsafety.h"

#include "llagent.h"
#include "llformat.h"
#include "llstartup.h"
#include "llviewercontrol.h"
#include "llviewerwindow.h"
#include "llvkautotune.h"
#include "vkgfx/llvulkancontext.h"
#include "vkgfx/llxrsession.h"

#include <chrono>

namespace
{
using Clock = std::chrono::steady_clock;

constexpr U32 kMaxLevel = 5;

// Judging (half-second windows of xrWaitFrame pacing; "missed" = display slots missed against
// the headset's CURRENT display period, so a steady rate the runtime settled on -- SteamVR's
// 45 Hz motion smoothing -- is clean pacing, not a failure):
constexpr F32 kSevere = 0.5f;       // a window below half the headset's rate: act at once, two levels
constexpr F32 kBad = 0.10f;         // a window missing more than this is "bad" ...
constexpr U32 kBadOf3 = 2;          // ... and 2 bad windows of the last 3 step down one level
constexpr F32 kBad2s = 0.08f;       // ... or the last 2 s missing more than this
constexpr F32 kClean = 0.03f;       // a window at or under this is clean (one stray miss is not a failure)
constexpr F64 kSettleSecs = 3.0;    // after any level change only a severe window counts (the change's own
                                    // churn -- LOD, streaming, render-scale targets -- costs a few frames)
// Restoring (one level at a time, after this long of clean windows):
constexpr F64 kDwellBase = 10.0;
constexpr F64 kDwellRenderScale = 30.0;   // a restore that raises the render scale (visible, re-allocates targets)
constexpr F64 kDwellMax = 600.0;
constexpr F64 kUndoneSecs = 30.0;         // a restore stepped back down within this was premature:
                                          // that level's dwell doubles
constexpr F64 kHeldSecs = 180.0;          // a restore that held this long: its level's dwell back to the base

// What each level caps: draw distance first (it costs the most and loses least in a headset),
// then mirrors / probe counts, far and projector shadows, probe resolution, LOD -- the render
// scale changes only twice (85 % at 3, 70 % at the top level 5) so the frequent 3 <-> 4 moves
// never toggle it. Cumulative.
LLVKPerfCaps capsFor(U32 level)
{
    LLVKPerfCaps c;
    if (level >= 1)
    {
        c.drawDistScale = 0.75f;
        c.mirrorsOff = true;
        c.probeCountShift = 1;
    }
    if (level >= 2)
    {
        c.drawDistScale = 0.5f;
        c.farShadowsOff = true;
        c.projShadowsOff = true;
        c.probeResShift = 1;
    }
    if (level >= 3)
    {
        c.drawDistScale = 0.35f;
        c.renderScaleMax = 0.85f;
        c.meshLodScale = 0.75f;
        c.avatarLodScale = 0.75f;
    }
    if (level >= 4)
    {
        c.drawDistScale = 0.25f;
        c.meshLodScale = 0.5f;
        c.avatarLodScale = 0.5f;
        c.probeCountShift = 2;
    }
    if (level >= 5)
    {
        c.renderScaleMax = 0.7f;
    }
    return c;
}

const char* levelText(U32 level)
{
    switch (level)
    {
    case 0: return "none";
    case 1: return "draw distance 75 %, mirrors off, half the reflection probes";
    case 2: return "draw distance 50 %, far + projector shadows off, probe resolution halved";
    case 3: return "draw distance 35 %, render scale 85 %, mesh / avatar detail 75 %";
    case 4: return "draw distance 25 %, render scale 85 %, mesh / avatar detail 50 %, a quarter of the probes";
    default: return "draw distance 25 %, render scale 70 %, mesh / avatar detail 50 %, a quarter of the probes";
    }
}

struct SafetyState
{
    U32 level = 0;
    // The current half-second window.
    Clock::time_point winStart{};
    U32 frames = 0, missed = 0;
    // The last 4 complete windows (2 s), newest first.
    U32 histFrames[4] = {}, histMissed[4] = {};
    U32 histCount = 0;
    Clock::time_point settleUntil{};   // after a level change: only severe windows count
    Clock::time_point cleanSince{};    // clean windows since (restore clock)
    // Per level L: seconds of clean pacing before L is given back (L -> L - 1), and when it
    // last was.
    F64 dwell[kMaxLevel + 1] = {};
    Clock::time_point restoredAt[kMaxLevel + 1] = {};
    bool restoreOpen[kMaxLevel + 1] = {};   // L was restored and has not yet held kHeldSecs / been undone
    bool measuring = false;
};
SafetyState sS;

F64 secs(Clock::duration d)
{
    return std::chrono::duration<F64>(d).count();
}

F64 baseDwell(U32 level)
{
    // Restoring L -> L - 1 raises the render scale for L = 5 (70 -> 85 %) and L = 3 (85 -> 100 %).
    return (level == 5 || level == 3) ? kDwellRenderScale : kDwellBase;
}

// The VR baseline (2026-10-27, owner): while a session runs the effective draw distance is at
// most kVrMaxDrawDist, whatever the governor does (or with it off). A cap like the governor's:
// the setting itself is never changed.
constexpr F32 kVrMaxDrawDist = 256.f;

F32 baselineDrawScale()
{
    static LLCachedControl<F32> s_far(gSavedSettings, "RenderFarClip", 128.f);
    const F32 dd = (F32)s_far;
    return dd > kVrMaxDrawDist ? kVrMaxDrawDist / dd : 1.f;
}

F32 sAppliedBaseline = -1.f;   // the baseline scale last applied (-1: none, no session)

void applyCaps(bool session)
{
    LLVKPerfCaps c = capsFor(sS.level);
    sAppliedBaseline = -1.f;
    if (session)
    {
        LLVKPerfCaps base;
        base.drawDistScale = baselineDrawScale();
        c = LLVKPerfCaps::merged(c, base);
        sAppliedBaseline = base.drawDistScale;
    }
    LLVulkanContext& vk = LLVulkanContext::instance();
    if (vk.isInitialized()) vk.setVrSafetyPerfCaps(c);
}

void setLevel(U32 level, const std::string& why, bool session = true)
{
    if (level == sS.level) return;
    LL_INFOS("VR") << "safety governor: level " << sS.level << " -> " << level << " (" << levelText(level) << "): " << why
                   << LL_ENDL;
    sS.level = level;
    applyCaps(session);
}

void resetWindows(Clock::time_point now)
{
    sS.winStart = now;
    sS.frames = sS.missed = 0;
    sS.histCount = 0;
    sS.cleanSince = now;
}

void initDwell()
{
    for (U32 l = 0; l <= kMaxLevel; ++l)
    {
        if (sS.dwell[l] <= 0.0) sS.dwell[l] = baseDwell(l);
    }
}
} // namespace

void LLVRSafety::reset()
{
    sS.measuring = false;
    setLevel(0, "the VR session stopped", false);
    if (sAppliedBaseline >= 0.f) applyCaps(false);   // (level already 0: still drop the baseline)
}

void LLVRSafety::tick(bool inWorld)
{
    static LLCachedControl<bool> s_on(gSavedSettings, "SlipstreamVRSafetyGovernor", false);
    const Clock::time_point now = Clock::now();
    // The baseline follows the draw-distance setting live (and is first applied here).
    if (baselineDrawScale() != sAppliedBaseline)
    {
        applyCaps(true);
        LL_INFOS("VR") << "VR baseline: draw distance at most " << kVrMaxDrawDist << " m (x" << sAppliedBaseline
                       << " of the setting)" << LL_ENDL;
    }
    if (!s_on)
    {
        sS.measuring = false;
        setLevel(0, "turned off (Preferences > Safety)");
        return;
    }
    initDwell();
    // Only judge real world frames: not while loading, teleporting or on the progress screen
    // (a load hitch is not a frame rate). The levels hold meanwhile.
    double period = 0.0;
    U32 missed = 0;
    const bool judge = inWorld && LLStartUp::getStartupState() == STATE_STARTED &&
                       gAgent.getTeleportState() == LLAgent::TELEPORT_NONE && gViewerWindow &&
                       !gViewerWindow->getShowProgress() && LLXrSession::instance().frameTiming(period, missed);
    if (!judge)
    {
        sS.measuring = false;
        return;
    }
    if (!sS.measuring)
    {
        sS.measuring = true;
        resetWindows(now);
        sS.settleUntil = now + std::chrono::seconds(1);
    }
    ++sS.frames;
    sS.missed += llmin(missed, 3u);   // one long stall counts as a few slots, not a collapse
    if (now - sS.winStart < std::chrono::milliseconds(500)) return;

    // A half-second window is complete: into the history (newest first).
    for (U32 i = 3; i > 0; --i)
    {
        sS.histFrames[i] = sS.histFrames[i - 1];
        sS.histMissed[i] = sS.histMissed[i - 1];
    }
    sS.histFrames[0] = sS.frames;
    sS.histMissed[0] = sS.missed;
    sS.histCount = llmin(sS.histCount + 1, 4u);
    sS.winStart = now;
    sS.frames = sS.missed = 0;
    auto share = [](U32 i) {
        return (F32)sS.histMissed[i] / (F32)llmax(1u, sS.histFrames[i] + sS.histMissed[i]);
    };
    const F32 half = share(0);
    U32 bad3 = 0;
    for (U32 i = 0; i < llmin(sS.histCount, 3u); ++i) bad3 += share(i) > kBad ? 1u : 0u;
    U32 f2 = 0, m2 = 0;
    for (U32 i = 0; i < sS.histCount; ++i)
    {
        f2 += sS.histFrames[i];
        m2 += sS.histMissed[i];
    }
    const F32 two = (F32)m2 / (F32)llmax(1u, f2 + m2);
    const F32 hz = period > 0.0 ? (F32)(1.0 / period) : 0.f;
    const bool settling = now < sS.settleUntil;

    U32 up = 0;
    if (half > kSevere) up = 2;   // below half the headset's rate: act hard, even while settling
    else if (!settling && sS.histCount >= 2 && (bad3 >= kBadOf3 || (sS.histCount >= 4 && two > kBad2s))) up = 1;
    if (up && sS.level < kMaxLevel)
    {
        const U32 to = llmin(kMaxLevel, sS.level + up);
        // A restore of a level we are stepping back into within kUndoneSecs was premature: that
        // level waits twice as long (up to kDwellMax) before it is given back again.
        for (U32 l = sS.level + 1; l <= to; ++l)
        {
            if (sS.restoreOpen[l] && secs(now - sS.restoredAt[l]) < kUndoneSecs)
            {
                sS.dwell[l] = llmin(sS.dwell[l] * 2.0, kDwellMax);
                LL_INFOS("VR") << "safety governor: giving level " << l << " back was premature ("
                               << secs(now - sS.restoredAt[l]) << " s): it now waits " << sS.dwell[l]
                               << " s of clean pacing" << LL_ENDL;
            }
            sS.restoreOpen[l] = false;
        }
        setLevel(to, llformat("%.0f %% of the headset's %.0f Hz slots missed in 0.5 s (%.0f %% over 2 s, %u of the last "
                              "3 windows over %.0f %%)",
                              half * 100.f, hz, two * 100.f, bad3, kBad * 100.f));
        sS.settleUntil = now + std::chrono::milliseconds((S64)(kSettleSecs * 1000.0));
        sS.cleanSince = now;
        return;
    }
    // A restore that held: its level is back to quick restores next time.
    for (U32 l = 1; l <= kMaxLevel; ++l)
    {
        if (sS.restoreOpen[l] && secs(now - sS.restoredAt[l]) >= kHeldSecs)
        {
            sS.restoreOpen[l] = false;
            sS.dwell[l] = baseDwell(l);
        }
    }
    if (half > kClean)
    {
        sS.cleanSince = now;
        return;
    }
    if (settling) return;
    // Clean pacing: give one level back after that level's dwell of it.
    const U32 l = sS.level;
    if (l > 0 && secs(now - sS.cleanSince) >= sS.dwell[l])
    {
        setLevel(l - 1, llformat("%.0f s of clean pacing at %.0f Hz", sS.dwell[l], hz));
        sS.restoredAt[l] = now;
        sS.restoreOpen[l] = true;
        sS.cleanSince = now;
        sS.settleUntil = now + std::chrono::milliseconds((S64)(kSettleSecs * 1000.0));
    }
}
