/**
 * @file llvkautotune.h
 * @brief "Improve graphics speed" under Vulkan (#422): our own FPS auto-tuner.
 *
 * Measures OUR renderer -- the main thread's frame time minus its GPU waits (CPU), the
 * world recording's GPU span (the rigged controller's always-on sampled timestamps) and,
 * while the Performance window is open or a tune is measuring, the clean renderer's
 * per-pass GPU timers grouped into categories (shadows, reflections, RT, lighting, post,
 * upscaling, ...) -- decides GPU-bound vs CPU-bound and steps the tunable with the best
 * cost / visual-quality ratio first, re-measures, and repeats, with hysteresis.
 *
 * Two modes:
 *  - TUNE ONCE (explicit user action): writes real settings, one step per measurement
 *    window, until the target is met with a little headroom (or nothing useful is left);
 *    keeps a summary of what changed and an exact Undo (the values it overwrote).
 *  - KEEP MY FPS (continuous, AutoTuneLock): behaves like the VRAM soft ladder --
 *    temporary EFFECTIVE caps only (LLVKPerfCaps, merged into the renderer's ladder caps,
 *    most restrictive wins; LL-side effective avatar limits), NEVER a setting write,
 *    restored in reverse order when there is headroom (project rule: pressure degrades
 *    are soft). It hands its target to the rigged-detail controller
 *    (RenderVKRiggedLodTargetFPS) so both agree on "slow", gives that controller time
 *    before stepping avatar levers, skips levers the VRAM ladder already holds, and never
 *    restores while the VRAM ladder is under pressure.
 *
 * Tune once measures every change (#422 round 2): paired A/B tests (A B B A ..., drift
 * cancels within a pair; one-way for levers whose switch-back re-streams), sequential (more
 * pairs while a gain is promising but undecided). Per lever it probes the cheapest allowed
 * value first; too small a gain ends the lever ("even X saves only Y ms", a small positive
 * one joins a bundle tried together later), a real one is bisected for the mildest stage
 * keeping most of it. A lever kept at a milder stage waits until every other lever had its
 * turn (the upscaler cannot take every step). Keep my FPS judges each engaged cap on its
 * next settled window and releases one that saved less than the floor. Levers own the GPU
 * passes they can remove (mirrors -> the mirror passes only, ...) for the "measured cheap"
 * gate and the order; no-gain verdicts are remembered per GPU + driver
 * (AutoTuneLeverHistory) as a prior that only deprioritises.
 *
 * Hardware rule: nothing is ever stepped down while the frame meets the target, so a
 * machine already hitting its target (newest devices) runs at its full settings.
 *
 * Main thread only, except gpuSubSample / gpuFrameDone (the GPU timer harvest; locked).
 * Log tag "AutoTune" (decisions, rate-limited).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * $/LicenseInfo$
 */
#pragma once

#include "stdtypes.h"
#include "llsd.h"
#include "lluuid.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// The continuous mode's effective caps: the subset of LLVramBudgetVK::Caps that costs
// GPU / CPU time (same meaning, same clamp points in the renderer). Defaults = no cap.
struct LLVKPerfCaps
{
    float    renderScaleMax = 1.f;    // internal render scale cap
    uint32_t probeResShift = 0;       // reflection probe resolution >> shift (floor 64 px)
    uint32_t probeCountShift = 0;     // reflection probe count >> shift (floor 32)
    bool     farShadowsOff = false;   // sun cascades end nearer, RT shadow range limited
    bool     mirrorsOff = false;
    bool     projShadowsOff = false;
    float    meshLodScale = 1.f;      // x effective RenderVolumeLODFactor
    float    avatarLodScale = 1.f;    // x effective RenderAvatarLODFactor
    float    drawDistScale = 1.f;     // x effective draw distance (LLVramBudgetVK floor / ceiling)

    bool operator==(const LLVKPerfCaps& o) const
    {
        return renderScaleMax == o.renderScaleMax && probeResShift == o.probeResShift
            && probeCountShift == o.probeCountShift && farShadowsOff == o.farShadowsOff
            && mirrorsOff == o.mirrorsOff && projShadowsOff == o.projShadowsOff
            && meshLodScale == o.meshLodScale && avatarLodScale == o.avatarLodScale
            && drawDistScale == o.drawDistScale;
    }
    bool operator!=(const LLVKPerfCaps& o) const { return !(*this == o); }
    // Most restrictive of both (the merge the renderer applies with the VRAM ladder).
    static LLVKPerfCaps merged(const LLVKPerfCaps& a, const LLVKPerfCaps& b);
};

// What the world renderer reports to the tuner (LLVulkanContext::autoTuneSample).
struct LLVKAutoTuneSample
{
    bool     valid = false;
    float    gpuFrameMs = 0.f;        // smoothed GPU span of the world recording (sampled every 8th frame)
    float    rigGpuMs = 0.f;          // smoothed GPU time of the rigged work (pre-skin, weld, raster)
    double   gpuAgeSec = 1.0e9;       // since that sample (stale > 3 s: no GPU numbers)
    uint32_t rigSlod = 0, rigSlodMax = 0;     // rigged controller: perf tolerance level / its top
    uint32_t rigPerfP = 0, rigPerfPMax = 0;   // ... creator-LOD perf pressure / its top
    LLVKPerfCaps vram;                // what the VRAM ladder holds (the LLVKPerfCaps fields of its caps)
    uint32_t vramMode = 0;            // ladder mode: 0 calm, 1 soft, 2 inside the reserve
    uint32_t vramRungs = 0;           // ladder rungs engaged
    uint64_t vramUsage = 0, vramBudget = 0;
    // #966 (Performance window / captures): the main view's internal render extent (0 = no
    // frame yet; below the window size = upscaled) and the upscaler the last main-view frame
    // actually ran (it can differ from the setting: unsupported device, DLSS shim missing).
    uint32_t renderW = 0, renderH = 0;
    enum Upscaler : uint8_t { UPS_NONE = 0, UPS_FSR = 1, UPS_DLSS_SR = 2, UPS_DLSS_RR = 3 };
    uint8_t  upscalerRan = UPS_NONE;
    // Per avatar: visible full-LOD skinned vertices of the last rigged scan (the share of
    // rigGpuMs that avatar's worn mesh costs).
    std::vector<std::pair<LLUUID, uint64_t>> avVerts;
};

class LLVKAutoTune
{
public:
    static LLVKAutoTune& instance();

    // ---- settings ----
    static constexpr U32 kTargetFpsMin = 10, kTargetFpsMax = 500, kTargetFpsDefault = 60;
    static U32 clampTargetFPS(U32 v);
    // Startup: TargetFPS repaired (0 / out of range -> 60); AutoTuneLock cleared unless
    // KeepAutoTuneLock says it persists across sessions.
    static void validateSettings();
    // The target the tuner works to: TargetFPS, capped by the refresh rate (vsync on) and
    // RenderFPSLimit (> 0). Never 0.
    static U32 effectiveTargetFPS();
    static S32 refreshRate();         // the window's display (0 = unknown)

    // ---- per-frame (LLAppViewer::doFrame, before LLVKHitch::frameBoundary) ----
    void frameTick();

    // ---- GPU timer harvest (LLVKBench hooks in the renderer; any thread) ----
    static void gpuSubSample(const char* name, double ms);
    static void gpuFrameDone(double total_ms);
    // The renderer ORs this into its pass timers (RenderVKGpuTimers) -- set while the
    // Performance window shows the breakdown or a tune is measuring. Never a setting write.
    static bool wantPassTimers() { return sWantTimers.load(std::memory_order_relaxed); }
    void requestPassTimers();         // keep them on for the next ~2 s (UI draw)
    // RenderVKRiggedLodTargetFPS as the rigged controller should use it: the tuner's
    // effective target while "Keep my FPS" runs (one definition of "slow"), else the setting.
    static F32 rigTargetFps(F32 setting)
    {
        const F32 t = sRigTargetFps.load(std::memory_order_relaxed);
        return t > 0.f ? t : setting;
    }

    // ---- user actions ----
    void startTuneOnce();
    void cancelTuneOnce();
    bool canUndo() const { return !mUndo.empty() && !tuningOnce(); }
    void undo();
    void setContinuous(bool on);      // AutoTuneLock handler

    // ---- state for the UI ----
    bool tuningOnce() const { return mOnce != ONCE_IDLE; }
    bool continuous() const { return mContinuous; }
    // One line: what the tuner is doing ("Measuring...", "Keeping 60 fps: holding ...").
    std::string statusText() const;
    // Last Tune once: one line per change ("Shadow updates: Realtime -> Balanced"), or the
    // reason nothing changed. Empty before the first run.
    const std::string& summaryText() const { return mSummary; }
    // The frame breakdown (multi-line): frame / CPU / GPU, what limits it, top GPU passes,
    // VRAM. Plain language.
    std::string breakdownText(bool detailed) const;
    // Smoothed frame / CPU / GPU ms (0 = unknown).
    float frameMs() const { return mFrameMsSm; }
    float cpuMs() const { return mCpuMsSm; }
    // The averages since the last call (wall frame ms, main-thread CPU ms = wall minus the GPU /
    // present / pacing waits) and their frame count, then a new window: the renderer's
    // "GPU ms/frame" line logs them over its own 5 s window (0 frames: nothing measured).
    uint32_t takeFrameWindow(double& frame_ms, double& cpu_ms)
    {
        const uint32_t n = mWinN;
        frame_ms = n ? mWinFrameMs / n : 0.0;
        cpu_ms = n ? mWinCpuMs / n : 0.0;
        mWinFrameMs = mWinCpuMs = 0.0;
        mWinN = 0;
        return n;
    }
    float gpuMs() const;
    // The renderer's last sample (4 Hz; VRAM ladder, render extent, upscaler run; #966).
    const LLVKAutoTuneSample& sample() const { return mSample; }

    // Effective non-impostor count the tuner / "simplify avatars beyond" wants (0 = no cap).
    U32 nonImpostorCap() const { return mNonImpCapApplied; }

private:
    LLVKAutoTune() = default;

    // ---- measurement ----
    struct FrameRec { float frame_ms, cpu_ms; };
    struct Window
    {
        bool  ok = false;
        float p50 = 0.f, p75 = 0.f, p90 = 0.f;   // wall frame ms
        float mean = 0.f;                       // wall frame ms
        float se = 0.f;                         // standard error of the mean (from ~0.5 s chunk means)
        float cpu50 = 0.f;                      // main thread busy ms (median)
        float gpu = 0.f;                        // GPU frame ms (0 = unknown)
        float wait_share = 0.f;                 // median GPU-wait share of the frame
        bool  gpu_bound = false;
        uint32_t frames = 0;
    };
    Window measure(uint32_t min_frames = 20) const;
    Window windowOf(const std::vector<FrameRec>& frames, uint32_t min_frames) const;
    bool  sampleBlocked(std::string* why) const;   // teleport, background, login, region change
    void  resetWindow();
    void  pollSample();                // renderer sample (4 Hz)
    void  updateAvatarCosts();         // -> LLVOAvatar::mGPURenderTime (est.)
    void  applyLLSideCaps();           // effective max non-impostors
    void  logDecision(const std::string& line, bool force = false);

    std::vector<FrameRec> mFrames;     // since the window start (bounded)
    double mWinStart = 0.0;            // wall s
    double mLastFrameAt = 0.0;
    float  mFrameMsSm = 0.f, mCpuMsSm = 0.f;
    double mWinFrameMs = 0.0, mWinCpuMs = 0.0;   // takeFrameWindow's sums
    uint32_t mWinN = 0;
    LLVKAutoTuneSample mSample;
    double mSampleAt = 0.0;
    double mBlockedUntil = 0.0;        // settle after a block (teleport, focus, region)
    void*  mLastRegion = nullptr;
    std::string mBlockReason;

    // ---- GPU pass categories (from the timer harvest) ----
public:
    enum Cat : uint8_t { CAT_GEOMETRY, CAT_AVATARS, CAT_SHADOWS, CAT_REFLECTIONS, CAT_RT, CAT_LIGHTING,
                         CAT_TRANSPARENT, CAT_PARTICLES, CAT_POST, CAT_UPSCALE, CAT_UI, CAT_OTHER, CAT_COUNT, CAT_NONE };
    static const char* catLabel(uint32_t c);
    // The category's display name (localized, English fallback; #966 Performance window).
    static std::string catName(uint32_t c);
    // GPU sub-mark name -> category; -1 = not a cost (waits, queue-span summaries, begins).
    static int catOf(const char* name);
    // Pass groups a lever owns (its removable cost), finer than the categories.
    enum Grp : uint8_t { GRP_MIRROR, GRP_RTMASK, GRP_AS, GRP_SUNMAP, GRP_PROJMAP, GRP_PROBE, GRP_AO, GRP_DOF,
                         GRP_GLOW, GRP_COUNT };
private:
    static int grpOf(const char* name);   // -1 = no group
    struct GpuCats
    {
        double frame[CAT_COUNT] = {};  // the harvested frame being summed
        float  sm[CAT_COUNT] = {};     // smoothed ms per category
        double gframe[GRP_COUNT] = {}; // ... per pass group
        float  gsm[GRP_COUNT] = {};
        float  total = 0.f;            // smoothed GPU frame total (pass timers)
        double at = 0.0;               // last harvested frame (wall s)
        uint32_t frames = 0;
    };
    static std::mutex sCatMutex;
    static GpuCats sCats;
    static std::atomic<bool> sWantTimers;
    static std::atomic<float> sRigTargetFps;
    double mTimersUntil = 0.0;
    float  catMs(uint32_t c, bool* known) const;
    float  grpMs(uint32_t mask, bool* known) const;   // sum of the groups in mask

    // ---- the step table ----
    struct Lever;
    static const std::vector<Lever>& levers();
    bool  leverAllowed(const Lever& l) const;   // TuningFPSStrategy scope
    // The lever's attributable GPU ms (its owned pass groups, else its category); known =
    // false when it cannot be attributed (whole-frame levers) or the timers are stale.
    float leverMs(const Lever& l, bool* known) const;
    bool  measuredCheap(const Lever& l, const Window& w) const;
    // Keep my FPS verdict: +1 faster, -1 slower, 0 within noise. gain = ms saved (mean / p75
    // average), thr = the bar it had to clear.
    static int judge(const Window& before, const Window& after, float& gain, float& thr);
    // Tune once: the values of lever l's setting cheaper than the current one, from the
    // mildest to the cheapest (empty = none); cur = the current value.
    bool  stagesOf(const Lever& l, std::vector<double>& stages, double& cur) const;
    // One finished window while tuning (a baseline, or an A/B segment). Returns the end
    // reason ("" = continue).
    std::string onceAdvance(const Window& w, bool timed_out, float target_ms);
    std::string oncePlan(bool timed_out, float target_ms);   // the next test, or the end reason
    void  rebuildUndo();
    std::string onceSummary(const std::string& end, float end_ms);
    // Continuous: engage / release one cap level.
    bool  capStep(bool gpu_bound, const Window& w);
    bool  capRelease();
    LLVKPerfCaps capsFor(const std::vector<uint8_t>& levels) const;
    void  pushCaps();

    enum OnceState { ONCE_IDLE, ONCE_SETTLE, ONCE_MEASURE };
    OnceState mOnce = ONCE_IDLE;
    double mOnceStart = 0.0, mPhaseUntil = 0.0;
    uint32_t mOnceSteps = 0;
    struct Change { std::string control, label, from, to; LLSD old_value; };
    std::vector<Change> mUndo;         // the last Tune once's kept writes, in order (rebuilt from mKept)
    std::string mSummary;
    float  mOnceStartMs = 0.f;
    // ---- Tune once: paired A/B tests ----
    // A test compares state A (the current settings) with state B (one or more settings
    // changed) in alternating segments, A B B A A B B A ... (one switch per pair; a linear
    // drift cancels within each pair), each measured after its settle; the paired segment
    // differences give the gain and its standard error. "Stream" levers (LOD, draw distance,
    // avatar limit: switching back re-streams / re-renders) run one-way instead: A A B B ...
    // (a two-sample comparison). More pairs are added while the gain is promising but not
    // yet decided (sequential).
    typedef std::vector<std::pair<std::string, LLSD>> Values;   // control -> value
    enum AbKind : uint8_t { AB_PROBE, AB_BISECT, AB_BUNDLE, AB_SPLIT, AB_CONFIRM };
    struct AbTest
    {
        bool   on = false;
        uint8_t kind = AB_PROBE;
        bool   oneway = false;
        double settle = 0.0;           // after each switch
        Values a, b;
        std::vector<size_t> levers;
        std::string label, from, to;   // log / status text
        float  thr = 0.f;              // the gain the decision is about
        uint32_t minPairs = 2, maxPairs = 8;
        uint32_t seg = 0;              // segments measured
        bool   inB = false;            // the state applied now
        std::vector<float> va, vb;     // per segment: 0.5 x (mean + p75) ms
        std::vector<FrameRec> fa, fb;  // every measured frame per state (level windows)
        double gpuA = 0.0, gpuB = 0.0;
        uint32_t ngA = 0, ngB = 0;
        // AB_SPLIT: the stronger half (levers[0, half)) alone vs the bundle it came from
        size_t half = 0;
        float  bundleGain = 0.f;
        Window bundleWin;
        Values full;
        // result
        int    verdict = 0;            // +1 gain >= thr, 0 no, -1 slower
        float  gain = 0.f, se = 0.f;
        uint32_t pairs = 0;
    };
    AbTest mAb;
    void  abBegin();
    bool  abSegment(const Window& w, bool force);   // true: decided
    void  abStats(float& gain, float& se, uint32_t& n, uint32_t& dof) const;
    bool  abDecide(bool force);
    void  abSettleTo(bool b);          // apply A or B (settle tracked)
    void  abAbandon(const char* why);  // back to A, no verdict
    Window abWindow(bool b) const;     // the pooled frames of one state
    void  applyValues(const Values& v, double settle);
    float onceBar() const;             // the smallest gain worth a quality step (ms)
    bool  startProbe(size_t lever);
    void  startBisectTest();
    void  finishBisect();
    void  onTestDone(bool timed_out, float target_ms);
    void  commitKeep(size_t lever, double value, float gain);
    void  addTried(size_t lever, const std::string& to, float gain, const char* why);
    bool  startConfirm(const Window& w, float target_ms);

    struct Bisect
    {
        bool   on = false;
        size_t lever = 0;
        std::vector<double> stages;    // mildest -> cheapest (the cheapest = the probe)
        float  full = 0.f;             // the probe's gain (the most this lever can give)
        float  goal = 0.f;             // the gain a stage must reach (share of full, or the need)
        int    lo = -1, hi = 0;        // lo: known short of the goal, hi: known to reach it
        int    mid = 0;               // the stage under test
        float  hiGain = 0.f;
        Window hiWin;                  // the level with stage hi applied
    };
    Bisect mBisect;
    enum LeverState : uint8_t { LS_UNTRIED, LS_DONE, LS_DEFERRED, LS_POOL };
    std::vector<uint8_t> mLeverState;  // per lever, this run
    std::vector<float> mProbeGain;     // per lever: the probe's gain (pool members)
    struct Kept
    {
        size_t lever = 0;
        uint8_t rank = 0;              // static rank when taken (higher = more quality lost per ms)
        std::string control, label, from, to;
        LLSD   before_value, after_value;
        float  gain = 0.f;
        bool   restored = false;       // given back by the confirmation pass
        bool   tried_back = false;     // the confirmation pass tried it (once)
    };
    std::vector<Kept> mKept;
    size_t mConfirmKept = 0;           // AB_CONFIRM: the mKept entry being given back
    std::vector<std::string> mTried;   // summary lines (did not help)
    std::vector<std::string> mRestored;
    Window mBase;                      // the current state's level (the last window / A pool)
    bool   mBaseOk = false;
    double mSettleUntil = 0.0;         // the last setting change has settled by then
    uint32_t mOnceTests = 0, mConfirmTries = 0;
    bool   mOnceGpuBound = true;

    // ---- per-machine lever history (AutoTuneLeverHistory) ----
    void   loadHistory();
    int    histCount(const char* id);
    void   noteVerdict(const char* id, bool helped);
    std::map<std::string, int> mHist;  // lever id -> times it did not help here (bounded)
    std::string mHistKey;              // GPU + driver
    bool   mHistLoaded = false;

    bool   mContinuous = false;
    std::vector<uint8_t> mCapLevel;    // per lever: engaged cap level (continuous)
    std::vector<std::pair<uint16_t, uint8_t>> mCapOrder;   // engaged steps (lever, its level before), released in reverse
    LLVKPerfCaps mCapsPushed;
    double mCapChangedAt = 0.0;        // last engage / release (wall s)
    double mComfySince = 0.0;          // frames comfortably under target since
    double mReleaseDwell = 10.0;       // doubles after a re-engage soon after a release (max 160)
    double mLastReleaseAt = -1.0e9;
    double mSlowSince = 0.0;
    // Keep my FPS experiments: the cap engaged last is judged on the next settled window.
    struct CapTrial { bool on = false; size_t lever = 0; Window before; double measure_from = 0.0; };
    CapTrial mCapTrial;
    std::vector<double> mCapBlockUntil;   // per lever: no gain -> left alone until then
    std::vector<float>  mCapBlockBusy;    // ... unless the busy ms moves this far from it (scene change)

    U32    mNonImpCapTuner = 0;        // continuous: non-impostor cap (0 = none)
    U32    mNonImpCapApplied = 0;      // what applyLLSideCaps last set (0 = the setting)
    double mNextAvCost = 0.0;
    double mNextLog = 0.0;
    std::string mLastLog;
};
