/**
 * @file llcleangpuretire.h
 * @brief GPU retirement epochs shared by the clean stores (material / terrain
 *        record / skin slot recycling).
 *
 *        A store that frees a table slot must not hand it to new content while a
 *        frame that may still read the old content is in flight. The VK renderer
 *        calls frameBegun() once per recorded frame, right after the fence of the
 *        frame slot it is about to reuse was waited: every frame begun more than
 *        `frames_in_flight` frames earlier has completed on the GPU.
 *
 *        A store stamps a free with stamp() and may reuse the slot once
 *        retired(stamp) is true. stamp() covers the frame being recorded now and
 *        the next one (the CPU mirrors are copied at the start of each frame's
 *        fill, so a free that lands mid-frame is first visible to the frame after).
 *
 *        No renderer ever begun (GL / pre-login) or a renderer idle for over a
 *        second (minimised, torn down): nothing can be in flight, so every stamp
 *        counts as retired -- otherwise frees would pile up unreusable forever.
 */

#pragma once

#include "stdtypes.h"

#include <atomic>
#include <chrono>

namespace LLCleanGpuRetire
{
    inline std::atomic<U64>& begunRef()  { static std::atomic<U64> s{0}; return s; }
    inline std::atomic<U64>& doneRef()   { static std::atomic<U64> s{0}; return s; }
    inline std::atomic<S64>& lastBeginNsRef() { static std::atomic<S64> s{0}; return s; }

    inline S64 nowNs()
    {
        return (S64)std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // Renderer only: a new frame begins recording (after its slot's fence wait).
    inline void frameBegun(U32 frames_in_flight)
    {
        const U64 b = begunRef().fetch_add(1) + 1;
        doneRef().store(b > (U64)frames_in_flight ? b - (U64)frames_in_flight : 0);
        lastBeginNsRef().store(nowNs());
    }

    // Stamp for a slot freed now.
    inline U64 stamp() { return begunRef().load() + 1; }

    // True once no in-flight frame can still read content freed at `s`.
    inline bool retired(U64 s)
    {
        const U64 b = begunRef().load();
        if (b == 0) return true;                       // no GPU consumer ever ran
        if (doneRef().load() >= s) return true;
        return nowNs() - lastBeginNsRef().load() > 1000000000LL;   // idle > 1 s
    }

    inline U64 framesBegun() { return begunRef().load(); }
    inline U64 framesDone()  { return doneRef().load(); }
}
