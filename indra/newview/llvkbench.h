/**
 * @file llvkbench.h
 * @brief Automated A/B bench for render settings (RenderVKBench*).
 *
 * Steps one setting through a list of values, RenderVKBenchSeconds per value
 * (RenderVKBenchCycles rounds), discarding the first RenderVKBenchSettle seconds
 * after each switch, and logs per value: frame time avg / p50 / p99, FPS, the GPU
 * wait (vk_acquireWait + vk_paceWait, the frame-top fence wait of RenderVKFramePacing),
 * the GPU frame total and the named GPU sub-marks (RenderVKBenchSubs; GPU numbers need
 * RenderVKGpuTimers on). The setting is
 * restored at the end. Log tag VKBench.
 *
 * Header-only state shared by the frame loop (llappviewer.cpp: slipstreamBenchTick)
 * and the GPU timer harvest (llcleanrenderervk.cpp: subSample / gpuFrame).
 */
#pragma once

#include <map>
#include <string>
#include <vector>

#include "llvkautotune.h"   // #422: the auto-tuner's per-pass breakdown shares this harvest
#include "llvkperfcapture.h"   // #966: the Performance window's timings / captures too

namespace LLVKBench
{
    struct Acc { double sum = 0.0; unsigned n = 0; void add(double v) { sum += v; ++n; } double avg() const { return n ? sum / n : 0.0; } };

    struct ValAcc
    {
        std::vector<float> frames;
        Acc acquire, gpu;
        std::map<std::string, Acc> subs;
    };

    struct State
    {
        bool active = false;      // a run is in progress
        bool collecting = false;  // past the settle time of the current step
        std::string setting;
        std::vector<std::string> values;     // distinct values, in order
        std::vector<std::string> schedule;   // values x cycles
        size_t step = 0;
        double step_start = 0.0;
        double settle = 1.5, seconds = 10.0;
        std::vector<std::string> sub_names;
        std::map<std::string, ValAcc> acc;   // keyed by value
        std::map<std::string, double> pending;   // this harvested frame's sub-mark sums
        std::string original;                // value to restore
    };
    inline State& state() { static State s; return s; }

    inline bool wantSub(const char* name)
    {
        const State& st = state();
        if (!st.collecting || !name) return false;
        for (const std::string& n : st.sub_names) if (n == name) return true;
        return false;
    }

    // GPU timer harvest: one sub-mark interval (a name may repeat within a frame). async:
    // measured on the async compute queue (its own clock, overlaps graphics work).
    inline void subSample(const char* name, double ms, bool async = false)
    {
        LLVKAutoTune::gpuSubSample(name, ms);   // #422
        LLVKPerfCapture::gpuSubSample(name, ms, async);   // #966 (no-op unless the window / a capture runs)
        if (wantSub(name)) state().pending[name] += ms;
    }

    // GPU timer harvest: the frame's total; closes that frame's sub-mark sums.
    inline void gpuFrame(double total_ms)
    {
        LLVKAutoTune::gpuFrameDone(total_ms);   // #422
        LLVKPerfCapture::gpuFrameDone(total_ms);   // #966
        State& st = state();
        if (!st.collecting || st.step >= st.schedule.size()) { st.pending.clear(); return; }
        ValAcc& va = st.acc[st.schedule[st.step]];
        va.gpu.add(total_ms);
        for (const auto& kv : st.pending) va.subs[kv.first].add(kv.second);
        st.pending.clear();
    }
}
