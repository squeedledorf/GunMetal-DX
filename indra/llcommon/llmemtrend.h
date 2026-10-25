/**
 * @file llmemtrend.h
 * @brief Periodic leak report (#883): process RSS trend plus the sizes of the viewer's
 *        long-lived containers, so an overnight session names what grew.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

#ifndef LL_LLMEMTREND_H
#define LL_LLMEMTREND_H

#include "stdtypes.h"
#include "llpreprocessor.h"

#include <functional>
#include <string>

// HOW IT WORKS
//  - Subsystems register a named PROBE: a cheap function returning the container's entry
//    count and (estimated) bytes, plus its cap / peak when it has them. Probes run on the
//    main thread, only when a report is due (every MemTrendReportMinutes, default 10), so
//    a probe may walk a container of a few thousand entries.
//  - tick() (main thread, every frame; returns at once when not due) logs ONE line
//    (tag MemTrend): RSS now, its change since the last report and its rate per hour
//    since the first report, the tracked total, the top N probes by bytes and the top N
//    growers since the first report (bytes, else count). A probe with a cap that is over
//    it is flagged. The first report is the baseline (taken after login has settled).
//  - Linux / glibc: each report also carries mallinfo2() (heap arena, in use, free but
//    retained, mmapped) -- tracked containers vs heap fragmentation vs memory outside the
//    heap (driver, mmaps) is what tells a container leak from the rest. It walks the
//    arenas under their locks (can take 100+ ms on a big heap), so only once per report.
//  - Nothing here allocates per frame; a report costs the probes plus one string.
namespace LLMemTrend
{
    struct Sample
    {
        U64 count = 0;   // entries
        U64 bytes = 0;   // bytes held (estimate is fine; 0 = unknown, count only)
        U64 cap = 0;     // byte budget, 0 = none / not applicable
        U64 peak = 0;    // peak bytes the owner saw, 0 = not tracked
    };
    using Probe = std::function<Sample()>;

    // Register or replace a named probe. Any thread; the probe itself is called on the
    // main thread from tick(). The name is the key and what the log prints.
    LL_COMMON_API void registerProbe(const std::string& name, Probe fn);
    LL_COMMON_API void unregisterProbe(const std::string& name);

    // Main thread. now = seconds (any monotonic clock); interval_sec <= 0 disables.
    // trim_free_bytes (Linux / glibc): a report that finds more than this free but retained
    // in the malloc heap posts malloc_trim(0) to the General work queue (0 = never).
    LL_COMMON_API void tick(F64 now, F64 interval_sec, U32 top_n, U64 trim_free_bytes = 0);

    // One report line now (also logged); for a Develop menu entry or a crash report.
    LL_COMMON_API std::string reportNow(F64 now, U32 top_n);

    // The top_n probes by bytes from the LAST report's samples ("name MB, ..."): no probe
    // is called, so it is cheap enough for the 4 Hz MemTimeline / 60 s memgov lines.
    // Empty before the first report.
    LL_COMMON_API std::string compactLine(U32 top_n);
}

#endif // LL_LLMEMTREND_H
