/**
 * @file llvkhitch.h
 * @brief Frame-hitch recorder (Vulkan path). Named scopes accumulate milliseconds
 *        (and counters) per frame; frames that spike well above the running average
 *        log their per-section breakdown, so camera-move hitches can be attributed
 *        instead of guessed. Header-only; main thread only.
 *
 *        VK_HITCH_SCOPE("name")        time a block into section "name"
 *        LLVKHitch::count("name", n)   add n to counter "name"
 *        LLVKHitch::frameBoundary()    once per frame (LLAppViewer::doFrame)
 *
 *        Hitch lines: RenderVKHitchLog or RenderVKSectionLog (checked at the frame
 *        boundary). RenderVKSectionLog additionally logs every section's STEADY-STATE
 *        average (ms and count per frame) every ~5 s -- the before/after instrument for
 *        CPU optimizations -- plus the window's frame pacing: worst frame, p95 / p99
 *        frame time, p99 frame-to-frame jitter, frametime events (the Statistics
 *        floater's rule: a frame > event_threshold longer than the previous one) and
 *        hitches (frames that produced a HITCH line).
 */
#ifndef LL_LLVKHITCH_H
#define LL_LLVKHITCH_H

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <sstream>
#include <vector>

namespace LLVKHitch
{
    // parent: the section this one was first seen nested in (null = top level; kept across
    // frames, first seen wins) -- the Performance window's section tree (#966).
    struct Slot { const char* name = nullptr; double ms = 0.0; unsigned count = 0; bool timed = false;
                  const char* parent = nullptr; };
    static constexpr int kSlots = 512;   // sections + counters share the table; overflow is dropped

    struct State
    {
        Slot slots[kSlots];
        std::chrono::steady_clock::time_point frame_start = std::chrono::steady_clock::now();
        double avg_ms = 0.0;
        double last_log = 0.0;
        bool   enabled = true;
        // Section averages (RenderVKSectionLog): per-slot sums since the last report.
        double sum_ms[kSlots] = {};
        double sum_count[kSlots] = {};
        double sum_frame_ms = 0.0;
        unsigned sum_frames = 0;
        double last_avg_log = 0.0;
        double last_long_log = -1e9;   // #700 long-frame line rate limit
        unsigned long_suppressed = 0;
        // Frame pacing over the same window.
        std::vector<float> win_ms;
        double prev_ms = 0.0;
        unsigned win_events = 0, win_hitches = 0;
        std::vector<float> win_jitter;
    };
    inline State& state() { static State s; return s; }

    // The slot table is main-thread state (no locking). The first caller -- the main
    // thread, at static init / the first frame -- owns it; calls from worker threads
    // (e.g. the store-fill worker) are dropped instead of racing the table.
    inline bool onOwnerThread()
    {
        static const std::thread::id s_owner = std::this_thread::get_id();
        return std::this_thread::get_id() == s_owner;
    }

    inline Slot* slot(const char* name)
    {
        if (!onOwnerThread()) return nullptr;
        State& st = state();
        for (Slot& s : st.slots)
        {
            if (s.name == name) return &s;
            if (!s.name) { s.name = name; return &s; }
        }
        return nullptr;   // table full: drop
    }

    inline void add(const char* name, double ms, const char* parent = nullptr)
    {
        if (Slot* s = slot(name))
        {
            s->ms += ms;
            s->timed = true;
            if (parent && !s->parent && parent != name) s->parent = parent;
        }
    }

    inline void count(const char* name, unsigned n = 1)
    {
        if (Slot* s = slot(name)) s->count += n;
    }

    // SETTINGS-CHANGE WATCH (#521): for the next `frames` frames, hitch lines are logged
    // whatever RenderVKHitchLog says (a graphics preset change / a live renderer
    // re-allocation arms it), so a report after such a change carries the breakdown.
    inline unsigned& watchFrames() { static unsigned s = 0; return s; }
    inline void armWatch(unsigned frames = 600) { if (onOwnerThread()) watchFrames() = std::max(watchFrames(), frames); }
    // Called once per frame by the frame boundary's owner: true while the watch is armed.
    inline bool watchTick()
    {
        unsigned& w = watchFrames();
        if (!w) return false;
        --w;
        return true;
    }

    // CRASH BREADCRUMB (#757 / #754 / #736): the main thread's open scopes, innermost last,
    // and a few numbers of the frame, kept where the Windows crash report (llwindebug.cpp)
    // reads them without allocating: a main-thread fault names the frame section it hit,
    // a worker / driver fault says what the main thread was doing at that moment. String
    // literals only (scope names); written by the owner thread, read racily by the crash
    // handler (a torn read shows a neighbouring section, never a bad pointer: slots above
    // `depth` keep old literals). No PII: section names, counters, memory sizes.
    struct Breadcrumb
    {
        static constexpr int kDepth = 24;
        const char* open[kDepth] = {};
        std::atomic<int> depth{0};        // may exceed kDepth (deeper names not kept); relaxed
        unsigned frame = 0;               // gFrameCount at the last frame boundary
        int teleport = 0;                 // LLAgent::ETeleportState
        unsigned tp_frames = 0;           // frames since the teleport state last changed
        unsigned vram_used_mb = 0, vram_budget_mb = 0;   // the renderer's budget view (every 30 frames)
    };
    inline Breadcrumb& breadcrumb() { static Breadcrumb b; return b; }

    struct Scope
    {
        const char* name;
        const char* parent = nullptr;   // the enclosing scope (#966 section tree; null = top level)
        std::chrono::steady_clock::time_point t0;
        bool owner;
        explicit Scope(const char* n) : name(n), t0(std::chrono::steady_clock::now()), owner(onOwnerThread())
        {
            if (owner)
            {
                Breadcrumb& b = breadcrumb();
                const int d = b.depth.load(std::memory_order_relaxed);
                if (d > 0 && d <= Breadcrumb::kDepth) parent = b.open[d - 1];
                if (d >= 0 && d < Breadcrumb::kDepth) b.open[d] = n;
                b.depth.store(d + 1, std::memory_order_relaxed);
            }
        }
        ~Scope()
        {
            if (owner)
            {
                Breadcrumb& b = breadcrumb();
                const int d = b.depth.load(std::memory_order_relaxed);
                if (d > 0) b.depth.store(d - 1, std::memory_order_relaxed);
            }
            add(name, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), parent);
        }
    };

    // FRAME TIMELINE: the last kFrames frames' pacing, so a HITCH line can be read against
    // the frames around it (a steady GPU with uneven CPU frames is a pacing problem, not a
    // hitch: the CPU runs ahead, then stalls on the frame-in-flight fence). The VK context
    // fills `cur` during the frame; frameBoundary() closes it into the ring.
    //   start   ms since the previous frame start (the frame time)
    //   pace    top-of-frame frame-slot fence wait (vk_paceWait, RenderVKFramePacing)
    //   cpu     frame start -> graphics submit, minus pace and acquire (main-thread work)
    //   acq     acquire wait (vk_acquireWait: the slot fence when pacing is off, + the image)
    //   present vkQueuePresentKHR call
    //   gpu     newest GPU frame time read back this frame (lags a few frames; -1 = none)
    struct FrameTimes
    {
        float start = 0.f, pace = 0.f, cpu = 0.f, acq = 0.f, present = 0.f, gpu = -1.f;
        bool  rendered = false;   // a VK frame was submitted (else: skipped / minimized / recreate)
        char  mode = '-';         // present mode: F fifo, R fifo-relaxed, M mailbox, I immediate
    };
    struct Timeline
    {
        static constexpr int kFrames = 16;
        FrameTimes ring[kFrames];
        unsigned head = 0, n = 0;
        FrameTimes cur;
        double last_log = -1e9;
        // Swapchain as last (re)created (literal from the context).
        const char* mode_name = "?";
        char mode = '-';
        unsigned images = 0, in_flight = 0;
    };
    inline Timeline& timeline() { static Timeline t; return t; }
    inline void timelinePace(double ms)    { if (onOwnerThread()) timeline().cur.pace += (float)ms; }
    inline void timelineAcquire(double ms) { if (onOwnerThread()) timeline().cur.acq += (float)ms; }
    inline void timelinePresent(double ms) { if (onOwnerThread()) timeline().cur.present += (float)ms; }
    inline void timelineGpu(double ms)     { if (onOwnerThread()) timeline().cur.gpu = (float)ms; }
    inline void timelineSwapchain(const char* mode_name, char mode, unsigned images, unsigned in_flight)
    {
        if (!onOwnerThread()) return;
        Timeline& t = timeline();
        t.mode_name = mode_name; t.mode = mode; t.images = images; t.in_flight = in_flight;
    }
    // Called right after the frame's graphics submit (state().frame_start is this frame's start).
    inline void timelineSubmitted()
    {
        if (!onOwnerThread()) return;
        Timeline& t = timeline();
        const double since = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()
                                                                       - state().frame_start).count();
        t.cur.rendered = true;
        t.cur.mode = t.mode;
        t.cur.cpu = (float)std::max(0.0, since - (double)t.cur.pace - (double)t.cur.acq);
    }
    // frameBoundary(): close the frame that just ended.
    inline void timelinePush(double frame_ms)
    {
        Timeline& t = timeline();
        t.cur.start = (float)frame_ms;
        t.ring[t.head] = t.cur;
        t.head = (t.head + 1) % Timeline::kFrames;
        if (t.n < (unsigned)Timeline::kFrames) ++t.n;
        t.cur = FrameTimes{};
    }
    // One compact line, oldest frame first; empty when rate-limited (at most one per
    // `min_interval_s`) or nothing recorded. Logged after a HITCH line (LLAppViewer::doFrame).
    inline std::string timelineLine(double min_interval_s = 1.0)
    {
        if (!onOwnerThread()) return std::string();
        Timeline& t = timeline();
        const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if (!t.n || wall - t.last_log < min_interval_s) return std::string();
        t.last_log = wall;
        std::string o;
        char buf[96];
        std::snprintf(buf, sizeof(buf), "TIMELINE %s x%u images, %u in flight, last %u frames oldest first ",
                      t.mode_name, t.images, t.in_flight, t.n);
        o = buf;
        o += "[start/pace/cpu/acq/present/gpu ms; gpu lags; '-' = no VK frame; mode letter when it differs]:";
        for (unsigned k = 0; k < t.n; ++k)
        {
            const FrameTimes& f = t.ring[(t.head + Timeline::kFrames - t.n + k) % Timeline::kFrames];
            if (!f.rendered)
                std::snprintf(buf, sizeof(buf), " %s%.1f/%.1f/-", k ? "| " : "", f.start, f.pace);
            else if (f.gpu >= 0.f)
                std::snprintf(buf, sizeof(buf), " %s%.1f/%.1f/%.1f/%.1f/%.1f/%.1f", k ? "| " : "", f.start, f.pace,
                              f.cpu, f.acq, f.present, f.gpu);
            else
                std::snprintf(buf, sizeof(buf), " %s%.1f/%.1f/%.1f/%.1f/%.1f/-", k ? "| " : "", f.start, f.pace,
                              f.cpu, f.acq, f.present);
            o += buf;
            if (f.rendered && f.mode != t.mode) o += f.mode;
        }
        return o;
    }

    // Returns a log line when the frame that just ended was a hitch, else empty.
    // With avg_enabled, *avg_line receives a steady-state section report every ~5 s.
    inline float percentile(std::vector<float>& v, double p)   // sorts v
    {
        if (v.empty()) return 0.f;
        std::sort(v.begin(), v.end());
        const size_t i = std::min(v.size() - 1, (size_t)(p * (double)(v.size() - 1) + 0.5));
        return v[i];
    }

    // event_threshold: the Statistics floater's frametime-event rule (StatsFrametimeEventThreshold).
    inline std::string frameBoundary(bool enabled, bool avg_enabled = false, std::string* avg_line = nullptr,
                                     double event_threshold = 0.1, double long_frame_ms = 0.0)
    {
        if (!onOwnerThread()) return std::string();   // (also pins the owner to the main thread)
        State& st = state();
        const auto now = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(now - st.frame_start).count();
        st.frame_start = now;
        std::string line;
        const double wall = std::chrono::duration<double>(now.time_since_epoch()).count();
        // #700: always-on long-frame line (long_frame_ms > 0): any frame over the threshold, whatever
        // `enabled` says, at most one per 2 s (frames suppressed in between are counted).
        const bool long_frame = long_frame_ms > 0.0 && ms > long_frame_ms;
        bool long_line = false;
        if (long_frame)
        {
            if (wall - st.last_long_log >= 2.0) { long_line = true; st.last_long_log = wall; }
            else ++st.long_suppressed;
        }
        if ((enabled && st.avg_ms > 0.0 && ms > 9.0 && ms > st.avg_ms * 1.6 && ms > st.avg_ms + 4.0
            && wall - st.last_log > 0.1) || long_line)
        {
            st.last_log = wall;
            Slot sorted[kSlots];
            int n = 0;
            for (const Slot& s : st.slots) if (s.name && (s.ms > 0.05 || s.count)) sorted[n++] = s;
            std::sort(sorted, sorted + n, [](const Slot& a, const Slot& b) { return a.ms > b.ms; });
            std::ostringstream os;
            os.setf(std::ios::fixed); os.precision(2);
            os << (long_line ? "LONGFRAME " : "HITCH ") << ms << " ms (avg " << st.avg_ms << "):";
            if (long_line && st.long_suppressed)
            {
                os << " [" << st.long_suppressed << " more long frames since the last line]";
                st.long_suppressed = 0;
            }
            for (int i = 0; i < n; ++i)
            {
                os << ' ' << sorted[i].name << '=';
                if (sorted[i].timed) os << sorted[i].ms << "ms";
                if (sorted[i].count) os << (sorted[i].timed ? "/" : "") << '#' << sorted[i].count;
            }
            line = os.str();
        }
        if (avg_enabled && avg_line)
        {
            if (!line.empty()) ++st.win_hitches;
            if (st.prev_ms > 0.0)
            {
                st.win_jitter.push_back((float)std::abs(ms - st.prev_ms));
                if (ms - st.prev_ms > st.prev_ms * event_threshold) ++st.win_events;
            }
            st.win_ms.push_back((float)ms);
            for (int i = 0; i < kSlots; ++i)
            {
                st.sum_ms[i] += st.slots[i].ms;
                st.sum_count[i] += st.slots[i].count;
            }
            st.sum_frame_ms += ms;
            ++st.sum_frames;
            if (st.last_avg_log == 0.0) st.last_avg_log = wall;
            if (wall - st.last_avg_log >= 5.0 && st.sum_frames > 0)
            {
                const double nf = (double)st.sum_frames;
                int order[kSlots];
                int n = 0;
                for (int i = 0; i < kSlots; ++i)
                    if (st.slots[i].name && (st.sum_ms[i] / nf > 0.01 || st.sum_count[i] > 0.0)) order[n++] = i;
                std::sort(order, order + n, [&](int a, int b) { return st.sum_ms[a] > st.sum_ms[b]; });
                std::ostringstream os;
                os.setf(std::ios::fixed); os.precision(3);
                os << "SECTIONS avg over " << st.sum_frames << " frames: frame=" << st.sum_frame_ms / nf << "ms ("
                   << (1000.0 * nf / std::max(st.sum_frame_ms, 1e-3)) << " fps)";
                for (int k = 0; k < n; ++k)
                {
                    const int i = order[k];
                    os << ' ' << st.slots[i].name << '=';
                    if (st.sum_ms[i] > 0.0) os << st.sum_ms[i] / nf << "ms";
                    if (st.sum_count[i] > 0.0) os << (st.sum_ms[i] > 0.0 ? "/" : "") << '#' << st.sum_count[i] / nf;
                }
                {
                    const float worst = st.win_ms.empty() ? 0.f : *std::max_element(st.win_ms.begin(), st.win_ms.end());
                    const float p95 = percentile(st.win_ms, 0.95), p99 = percentile(st.win_ms, 0.99);
                    const float j99 = percentile(st.win_jitter, 0.99);
                    std::ostringstream ps;
                    ps.setf(std::ios::fixed); ps.precision(2);
                    ps << " | PACING worst=" << worst << "ms p95=" << p95 << "ms p99=" << p99
                       << "ms jitter_p99=" << j99 << "ms events=" << st.win_events
                       << " hitches=" << st.win_hitches;
                    *avg_line = os.str();
                    // Pacing goes right after the frame average so it survives any cut.
                    const size_t at = avg_line->find(" fps)");
                    avg_line->insert(at == std::string::npos ? avg_line->size() : at + 5, ps.str());
                }
                st.win_ms.clear(); st.win_jitter.clear(); st.win_events = 0; st.win_hitches = 0;
                for (int i = 0; i < kSlots; ++i) { st.sum_ms[i] = 0.0; st.sum_count[i] = 0.0; }
                st.sum_frame_ms = 0.0; st.sum_frames = 0;
                st.last_avg_log = wall;
            }
        }
        // Running average excludes the spike itself dominating (clamped contribution).
        st.prev_ms = ms;
        const double contrib = (st.avg_ms > 0.0) ? std::min(ms, st.avg_ms * 3.0) : ms;
        st.avg_ms = (st.avg_ms > 0.0) ? st.avg_ms * 0.95 + contrib * 0.05 : ms;
        timelinePush(ms);
        for (Slot& s : st.slots) { s.ms = 0.0; s.count = 0; s.timed = false; }
        return line;
    }
}

#define VK_HITCH_CAT2(a, b) a##b
#define VK_HITCH_CAT(a, b) VK_HITCH_CAT2(a, b)
// Every hitch scope is also a Tracy zone (runtime name), so a capture shows the Vulkan
// frame's own sections, not one opaque "Render" zone.
#ifdef TRACY_ENABLE
#include "tracy/Tracy.hpp"
#define VK_HITCH_SCOPE(name)                                                                   \
    ZoneTransientN(VK_HITCH_CAT(_vk_tracy_, __LINE__), name, true);                           \
    LLVKHitch::Scope VK_HITCH_CAT(_vk_hitch_, __LINE__)(name)
#else
#define VK_HITCH_SCOPE(name) LLVKHitch::Scope VK_HITCH_CAT(_vk_hitch_, __LINE__)(name)
#endif

#endif // LL_LLVKHITCH_H
