/**
 * @file llvkperfcapture.h
 * @brief #966 user-visible performance timings: the Performance window's "Frame timings"
 *        page (live) and the timed capture for performance bug reports.
 *
 * Reuses the existing instruments only:
 *  - the frame split from the hitch recorder's slots of the frame that just ended
 *    (LLVKHitch: vk_paceWait / vk_acquireWait / vk_asyncSlotWait / vk_texStageWait = waiting
 *    for the GPU / frame pacing, vk_present = the present call, fpsLimitSleep = the frame
 *    rate cap; the rest of the wall frame time is main-thread work),
 *  - the CPU sections (every VK_HITCH_SCOPE, as a tree: a section's time includes the
 *    sections inside it, minus the waits above),
 *  - the clean renderer's GPU pass timers (RenderVKGpuTimers sub-marks), harvested through
 *    LLVKBench::subSample / gpuFrame (the non-blocking ring readback, results TS_RING-1
 *    frames old: a GPU timer read never waits).
 *
 * COST: nothing runs unless the timings page is on screen (requestLive, ~2 s hold) or a
 * capture runs. Only then are the GPU pass timers requested (LLVKAutoTune::requestPassTimers
 * -- the same switch the Performance window already uses for its breakdown) and the slots
 * read. Closed: frameTick and the harvest hooks return on one flag test.
 *
 * CAPTURE: startCapture(seconds): 1 s warm-up (the timers start, their first results arrive
 * a few frames late), then the window; averages and p50 / p95 / p99 of the frame, its split
 * and every GPU pass / CPU section, as a plain-text block -- hardware, driver and render
 * settings but no avatar / region names, positions or UUIDs (and the text goes through the
 * bug report's scrubber, LLFloaterReportBug::scrubText). Saved to the logs folder
 * (slipstream-perf-capture-<UTC time>.txt, the newest kMaxFiles kept, plus
 * slipstream-perf-capture-last.txt) and optionally copied to the clipboard.
 *
 * Main thread, except gpuSubSample / gpuFrameDone (the GPU timer harvest; locked).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * $/LicenseInfo$
 */
#pragma once

#include "stdtypes.h"

#include <boost/signals2.hpp>

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

class LLVKPerfCapture
{
public:
    static LLVKPerfCapture& instance();

    static constexpr U32 kMinSeconds = 3, kMaxSeconds = 60, kDefaultSeconds = 10;

    // ---- UI ----
    // The timings page is on screen: keep sampling (and the GPU pass timers) for ~2 s.
    void requestLive();
    // Live readout (multi-line, monospace columns). "" before anything was measured.
    std::string liveText() const;
    // Starts a capture of `seconds` (clamped to kMinSeconds..kMaxSeconds). No-op while one runs.
    // to_clipboard: copy the result when done (the Performance window; not Report Bug).
    void startCapture(U32 seconds, bool to_clipboard);
    void cancelCapture();
    bool capturing() const { return mPhase != PH_IDLE; }
    // Capturing: seconds left (warm-up included); else 0.
    F32  captureRemaining() const;
    U32  captureSeconds() const { return mCapSeconds; }
    // The last finished capture (this session): text, file, when (wall s, LLTimer::getTotalSeconds
    // clock), one-line summary ("10 s, avg 58 fps, 1% low 41 fps"). Empty text = none yet.
    const std::string& lastText() const { return mLastText; }
    const std::string& lastFile() const { return mLastFile; }
    F64  lastAt() const { return mLastAt; }
    const std::string& lastSummary() const { return mLastSummary; }
    void copyLastToClipboard() const;
    // Fired on the main thread when a capture finishes (or is cancelled: lastAt unchanged).
    typedef boost::signals2::signal<void()> done_signal_t;
    boost::signals2::connection setDoneCallback(const done_signal_t::slot_type& cb) { return mDone.connect(cb); }

    // ---- per frame (LLAppViewer::doFrame, before LLVKHitch::frameBoundary clears the slots) ----
    void frameTick();

    // ---- GPU timer harvest (LLVKBench hooks; any thread) ----
    static void gpuSubSample(const char* name, double ms, bool async);
    static void gpuFrameDone(double total_ms);

    // Plain-language names (partial maps; fallback: the GPU pass category, or the raw name).
    static std::string gpuPassName(const char* mark);
    static const char* cpuSectionName(const char* section);

private:
    LLVKPerfCapture() = default;

    enum Phase : uint8_t { PH_IDLE, PH_WARMUP, PH_COLLECT };

    // One frame of the main-thread split (ms).
    struct FrameRec { float frame, cpu, wait, present, sleep; };
    // GPU sub-mark accumulator (graphics and async queue kept apart: different clocks, and
    // async work overlaps the graphics queue).
    struct Pass
    {
        std::string name;
        bool   async = false;
        double pending = 0.0;        // this harvested frame
        float  ema = 0.f;            // live
        double capSum = 0.0;         // capture
        std::vector<float> cap;      // capture: per harvested frame where present (absent = 0)
    };
    static constexpr size_t kMaxPasses = 512;
    static constexpr size_t kMaxFrames = 30000;   // per capture (60 s at 500 fps)
    static constexpr int    kSlots = 512;          // = LLVKHitch::kSlots
    static constexpr size_t kMaxFiles = 10;

    bool  active() const { return mLive || mPhase != PH_IDLE; }
    void  activate();             // inactive -> active: fresh live state
    Pass* passFor(const char* name, bool async);   // under mGpuMutex
    void  finishCapture();
    std::string captureText() const;
    std::string headerText() const;
    std::string statusLines() const;   // memory, tier, upscaler, render scale
    std::string limitText(float frame, float cpu, float gpu, float wait) const;
    void  saveCapture(const std::string& text);
    // CPU section tree: the parent slot index of each slot (-1 = top level), resolved lazily.
    int   parentOf(int slot);

    // ---- state ----
    std::atomic<bool> mAny{false};    // active(): the harvest hooks' one test
    bool   mLive = false;
    double mLiveUntil = 0.0;
    double mLastFrameAt = 0.0;
    // live (main thread)
    float  mEmaFrame = 0.f, mEmaCpu = 0.f, mEmaWait = 0.f, mEmaPresent = 0.f, mEmaSleep = 0.f;
    std::deque<std::pair<double, float>> mRecent;   // (wall s, frame ms) of the last 5 s
    float  mCpuEma[kSlots] = {};
    int16_t mParent[kSlots];
    bool   mParentInit = false;
    // GPU (mGpuMutex)
    mutable std::mutex mGpuMutex;
    std::vector<Pass> mPasses;
    size_t mHint = 0;
    float  mGpuEma = 0.f;
    double mGpuAt = 0.0;              // last harvested frame (wall s)
    uint32_t mGpuLiveFrames = 0;
    uint32_t mGpuSubFrames = 0;       // harvested frames that carried sub-marks (lean mode: none)
    std::vector<float> mCapGpu;       // capture: per harvested frame GPU total
    uint32_t mCapGpuSubFrames = 0;
    bool   mGpuCollect = false;       // mirrors mPhase == PH_COLLECT for the harvest
    // capture (main thread)
    Phase  mPhase = PH_IDLE;
    U32    mCapSeconds = kDefaultSeconds;
    bool   mCapClipboard = false;
    double mPhaseEnd = 0.0, mCapStart = 0.0, mCapWallStart = 0.0;
    std::vector<FrameRec> mCapFrames;
    std::vector<float> mCpuCap[kSlots];
    double mCpuCapSum[kSlots] = {};
    uint32_t mCapExcluded = 0, mCapBackground = 0;
    // last result
    std::string mLastText, mLastFile, mLastSummary;
    F64    mLastAt = 0.0;
    done_signal_t mDone;
};
