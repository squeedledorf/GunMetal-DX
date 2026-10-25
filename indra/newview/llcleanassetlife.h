/**
 * @file llcleanassetlife.h
 * @brief Asset lifetime helpers shared by the clean stores (#737): a wall-clock grace
 *        expressed in frames, and per-kind release counters.
 *
 *        The stores stamp their entries with gFrameCount (cheap, monotonic), but a
 *        release grace is a TIME ("free 30 s after the last user let go"): 600 frames is
 *        4 s at 144 fps and 20 s at 30 fps. frameTick() records (frame, time) samples
 *        once per frame; framesSpanning(s) answers how many frames the last `s` seconds
 *        took, so `now_frame - stamp >= framesSpanning(s)` means "unused for >= s".
 *
 *        Release counters are monotonically increasing totals by kind (materials,
 *        bindless slots, images and bytes, mesh slots, LOD groups, geometry and mirror
 *        bytes). The renderer logs their per-minute deltas and a teleport summary.
 */

#pragma once

#include "stdtypes.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>

namespace LLCleanAssetLife
{
    enum Kind : U32
    {
        MATERIALS = 0,      // material store entries freed (no holder past the grace)
        TERRAIN_RECORDS,    // terrain records freed (region left behind)
        TEX_SLOTS,          // bindless texture slots evicted (no live material resolves them)
        TEX_IMAGES,         // GPU texture images freed (their last slot went)
        TEX_BYTES,          // ... and their device bytes
        MESH_SLOTS,         // mesh store slot ids freed (geometry of no live instance)
        MESH_GROUPS,        // LOD groups freed
        GEOM_BYTES,         // geometry bytes freed (packed vertex + index)
        MIRROR_BYTES,       // CPU geometry mirror bytes freed
        KIND_COUNT
    };

    inline std::atomic<U64>* counters()
    {
        static std::atomic<U64> s[KIND_COUNT];
        return s;
    }
    inline void add(Kind k, U64 n)
    {
        if (n) counters()[k].fetch_add(n, std::memory_order_relaxed);
    }
    inline U64 total(Kind k) { return counters()[k].load(std::memory_order_relaxed); }

    struct Totals
    {
        U64 v[KIND_COUNT] = {};
        U64 operator[](Kind k) const { return v[k]; }
    };
    inline Totals snapshot()
    {
        Totals t;
        for (U32 k = 0; k < KIND_COUNT; ++k) t.v[k] = total((Kind)k);
        return t;
    }

    // ---- frame <-> wall-clock ----------------------------------------------------------
    struct Clock
    {
        static constexpr U32 N = 512;     // samples at >= 0.25 s: >= 2 minutes of history
        std::mutex mutex;
        U32    frame[N] = {};
        double t[N] = {};
        U32    head = 0;                  // next write
        U32    count = 0;
    };
    inline Clock& clock()
    {
        static Clock c;
        return c;
    }
    inline double nowSeconds()
    {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // Once per frame (main thread). Keeps a sample every quarter second.
    inline void frameTick(U32 current_frame)
    {
        Clock& c = clock();
        const double now = nowSeconds();
        std::lock_guard<std::mutex> lk(c.mutex);
        if (c.count)
        {
            const U32 last = (c.head + Clock::N - 1) % Clock::N;
            if (now - c.t[last] < 0.25) return;
        }
        c.frame[c.head] = current_frame;
        c.t[c.head] = now;
        c.head = (c.head + 1) % Clock::N;
        if (c.count < Clock::N) ++c.count;
    }

    // Frames rendered during the last `seconds` of wall time (>= 1). Beyond the recorded
    // history (session start, or a span over ~2 min) the average rate of the history is
    // extrapolated; with no history at all, 60 fps is assumed.
    inline U32 framesSpanning(F32 seconds, U32 current_frame)
    {
        if (seconds <= 0.f) return 1;
        Clock& c = clock();
        const double now = nowSeconds();
        const double want = now - (double)seconds;
        std::lock_guard<std::mutex> lk(c.mutex);
        if (c.count == 0) return (U32)std::max(1.0, (double)seconds * 60.0);
        // Newest sample at or before `want` (walk back from the newest).
        for (U32 i = 0; i < c.count; ++i)
        {
            const U32 idx = (c.head + Clock::N - 1 - i) % Clock::N;
            if (c.t[idx] <= want)
            {
                const U32 f = current_frame - c.frame[idx];
                return f ? f : 1u;
            }
        }
        // Not far enough back: extrapolate the history's average rate.
        const U32 oldest = (c.head + Clock::N - c.count) % Clock::N;
        const double span = now - c.t[oldest];
        const U32 frames = current_frame - c.frame[oldest];
        const double fps = (span > 0.5 && frames > 0) ? (double)frames / span : 60.0;
        return (U32)std::max(1.0, fps * (double)seconds);
    }
}
