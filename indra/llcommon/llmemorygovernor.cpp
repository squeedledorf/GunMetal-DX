/**
 * @file llmemorygovernor.cpp
 * @brief System memory governor (#262). See llmemorygovernor.h.
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

#include "linden_common.h"

#include "llmemorygovernor.h"

#include "llerror.h"
#include "llmemory.h"

#if LL_WINDOWS
# include "llwin32headers.h"
# include <psapi.h>
#elif LL_LINUX
# include <malloc.h>     // mallopt (glibc)
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <sstream>

namespace
{
constexpr U64 MB = 1024ull * 1024ull;
constexpr U64 GB = 1024ull * MB;

constexpr U64 kBudgetFloor      = 96 * MB;     // CRITICAL budget (never zero; see admit())
constexpr F64 kSampleSec        = 0.25;        // system sample period (4 Hz)
constexpr F64 kImproveHoldSec   = 2.0;         // a better state must hold this long
constexpr F64 kStatusLogSec     = 60.0;        // self-logged status cadence
constexpr int kRetainedWindow   = 30;          // seconds of retained-bytes history (1 s buckets)

const char* kSubName[LLMemoryGovernor::SUB_COUNT] = { "texdec", "meshdec", "texup", "http" };

U64 clampU64(U64 v, U64 lo, U64 hi)
{
    if (hi < lo) hi = lo;
    return v < lo ? lo : (v > hi ? hi : v);
}

F64 nowSec()
{
    using namespace std::chrono;
    return duration<F64>(steady_clock::now().time_since_epoch()).count();
}

// ---- shared state (atomics: read from the fetch, decode, mesh and HTTP threads) ----
std::atomic<bool> sEnabled{true};
std::atomic<bool> sShutdown{false};
std::atomic<U32>  sState{LLMemoryGovernor::COMFORTABLE};
std::atomic<U64>  sBudget{2 * GB};
std::atomic<U64>  sTotal{0};
std::atomic<U64>  sAvail{0};
std::atomic<U64>  sReserve{0};
std::atomic<U64>  sInFlightTotal{0};
std::atomic<U64>  sInFlight[LLMemoryGovernor::SUB_COUNT];
std::atomic<S64>  sGauge[LLMemoryGovernor::SUB_COUNT];
std::atomic<U64>  sRetained{0};
std::atomic<U64>  sRetainedLoad{0};      // retained growth over its recent low
std::atomic<U64>  sRetainedAllowance{1 * GB};
std::atomic<U64>  sDenied{0};            // tryAcquire refusals (cumulative)
std::atomic<U64>  sBlockedWaits{0};      // blocking acquires that had to wait
std::atomic<U64>  sForcedAdmits{0};      // blocking acquires admitted after max wait
std::atomic<S32>  sWaiters{0};
std::atomic<U64>  sReleases{0};          // release() calls (#485 waiter wake-ups)

// ---- #698 allocation failures (any thread; the new_handler touches only these) ----
constexpr U32 kAllocK = LLMemoryGovernor::ALLOC_KIND_COUNT;
constexpr U64 kAllocLogGapMs = 2000;         // per-kind failure log rate limit
constexpr F64 kReserveComfortSec = 20.0;     // COMFORTABLE this long before re-acquiring
const char* kAllocKindName[kAllocK] = { "texdecode", "meshdecode", "meshfetch", "cleandecode",
                                        "storefill", "workitem", "thread", "mainframe", "other" };
std::atomic<U64>   sShedUntilMs{0};          // forced-CRITICAL shed window end (steady ms)
std::atomic<U32>   sShedMs{15000};
std::atomic<U32>   sRetryLimit{4};
std::atomic<U32>   sInjectN{0};
std::atomic<U64>   sInjectCounter{0};
std::atomic<U64>   sInjected{0};
std::atomic<char*> sReserveBlock{nullptr};
std::atomic<U64>   sReserveHeld{0};
std::atomic<U64>   sReserveReleased{0};
std::atomic<U64>   sReserveReacquired{0};
std::atomic<bool>  sReserveLogPending{false};
std::atomic<U64>   sSheds{0};
std::atomic<U64>   sAllocFail[kAllocK];
std::atomic<U64>   sAllocRetried[kAllocK];
std::atomic<U64>   sAllocDropped[kAllocK];
std::atomic<U64>   sAllocLogNextMs[kAllocK];
std::atomic<U64>   sAllocLogSuppressed[kAllocK];
std::atomic<U64>   sLastFailMs{0};
std::atomic<U64>   sLastFailBytes{0};
std::atomic<U32>   sLastFailKind{LLMemoryGovernor::ALLOC_OTHER};

U64 nowMs() noexcept
{
    using namespace std::chrono;
    return (U64)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

bool sheddingNow() noexcept { return nowMs() < sShedUntilMs.load(std::memory_order_relaxed); }

// Any thread / new_handler: open (or extend) the forced-CRITICAL shed window. The state
// and budget drop at once (admission sees them immediately); update() keeps them there
// until the window ends and logs the transition back.
void startShed() noexcept
{
    const U64 now = nowMs();
    const U64 prev = sShedUntilMs.exchange(now + sShedMs.load(std::memory_order_relaxed), std::memory_order_relaxed);
    if (prev <= now) sSheds.fetch_add(1, std::memory_order_relaxed);
    sState.store(LLMemoryGovernor::CRITICAL, std::memory_order_relaxed);
    sBudget.store(kBudgetFloor, std::memory_order_relaxed);
}

// std::new_handler (#698): operator new failed. Free the emergency reserve and return --
// operator new then retries the allocation, so the first failure anywhere is absorbed
// transparently. No reserve left: throw (the standard contract). NO LOGGING here (the
// logger allocates); update() logs the release from the main thread.
void parachuteNewHandler()
{
    if (char* p = sReserveBlock.exchange(nullptr, std::memory_order_acq_rel))
    {
        sReserveHeld.store(0, std::memory_order_relaxed);
        delete[] p;
        sReserveReleased.fetch_add(1, std::memory_order_relaxed);
        sReserveLogPending.store(true, std::memory_order_relaxed);
        startShed();
        return;
    }
    throw std::bad_alloc();
}

// Main thread: (re)take the emergency reserve. Touched page by page so it is really
// committed (Linux over-commits untouched pages, which would make it worthless).
bool acquireReserve(U64 bytes)
{
    if (!bytes || sReserveBlock.load(std::memory_order_relaxed)) return false;
    char* p = new (std::nothrow) char[bytes];
    if (!p) return false;
    for (U64 i = 0; i < bytes; i += 4096) p[i] = 1;
    char* expected = nullptr;
    if (!sReserveBlock.compare_exchange_strong(expected, p, std::memory_order_acq_rel))
    {
        delete[] p;
        return false;
    }
    sReserveHeld.store(bytes, std::memory_order_relaxed);
    return true;
}

std::mutex& waitMutex() { static std::mutex m; return m; }
std::condition_variable& waitCv() { static std::condition_variable cv; return cv; }

// ---- main-thread-only state ----
struct MainState
{
    U32 reserve_mb = 0;
    F32 timeline_sec = 60.f;
    bool mallopt_done = false;
    F64 next_sample = 0.0;
    F64 better_since = -1.0;
    U32 better_state = LLMemoryGovernor::COMFORTABLE;
    LLMemoryGovernor::SystemMemory sys;
    bool have_sys = false;
    U64 rss = 0;
    // retained-bytes history: per-second minimum over the last kRetainedWindow seconds
    U64 ret_bucket[kRetainedWindow] = {};
    int ret_head = 0;
    bool ret_init = false;
    F64 ret_bucket_start = 0.0;
    // waits/s
    F64 rate_t = 0.0;
    U64 rate_denied = 0, rate_blocked = 0;
    F32 denied_per_sec = 0.f, blocked_per_sec = 0.f;
    // status self-log
    F64 next_status_log = 0.0;
    // timeline
    std::string tl_reason;
    F64 tl_start = -1.0, tl_end = -1.0, tl_next = 0.0;
    U64 tl_rss_start = 0, tl_rss_peak = 0;
    std::function<std::string()> extra;
    // #698 emergency reserve (main thread)
    U32 alloc_reserve_mb = 0;
    bool new_handler_set = false;
    F64 comfy_since = -1.0;
    bool was_shedding = false;
};
MainState& ms() { static MainState s; return s; }

U64 computeReserve(U64 total, U32 reserve_mb)
{
    if (!total) return (U64)1536 * MB;
    if (reserve_mb > 0)
    {
        return (std::min)((U64)reserve_mb * MB, total / 2);
    }
    return (std::max)((U64)1536 * MB, total * 12 / 100);
}

U32 rawState(const LLMemoryGovernor::SystemMemory& s, U64 reserve)
{
    bool critical = s.avail < reserve / 2;
    bool tight = s.avail < reserve + reserve / 2;
    if (s.have_psi)
    {
        critical = critical || s.psi_full10 >= 5.f || s.psi_some10 >= 30.f;
        tight = tight || s.psi_some10 >= 5.f;
    }
    if (s.have_commit)
    {
        // Windows: running out of commit fails allocations outright, RAM or not.
        critical = critical || s.commit_avail < reserve / 2;
        tight = tight || s.commit_avail < reserve;
    }
    return critical ? LLMemoryGovernor::CRITICAL : (tight ? LLMemoryGovernor::TIGHT : LLMemoryGovernor::COMFORTABLE);
}

U64 computeBudget(U32 state, U64 total, U64 avail, U64 reserve)
{
    const U64 headroom = avail > reserve ? avail - reserve : 0;
    const U64 cap = clampU64(total / 32, 256 * MB, 2 * GB);
    switch (state)
    {
    case LLMemoryGovernor::COMFORTABLE: return clampU64(headroom / 4, 256 * MB, cap);
    case LLMemoryGovernor::TIGHT:       return clampU64(headroom / 8, kBudgetFloor, cap / 4);
    default:                            return kBudgetFloor;
    }
}

U64 computeRetainedAllowance(U32 state, U64 total)
{
    // 8 GB: 512 MB, 16 GB: 1 GB, 32 GB: 2 GB, 64 GB+: 4 GB (a busy sim settles ~2-3.5 GB).
    U64 a = total ? clampU64(total / 16, 512 * MB, 4 * GB) : 1 * GB;
    if (state == LLMemoryGovernor::TIGHT) a /= 2;
    else if (state == LLMemoryGovernor::CRITICAL) a /= 4;
    return (std::max)(a, (U64)256 * MB);
}

bool admit(LLMemoryGovernor::ESubsystem sub, U64 bytes, LLMemoryGovernor::EPriority pri)
{
    // NEVER ZERO: a subsystem with nothing in flight always gets one job, whatever its
    // size -- loading slows down under pressure but never stops.
    if (sInFlight[sub].load(std::memory_order_relaxed) == 0) return true;
    if (!sEnabled.load(std::memory_order_relaxed) || sShutdown.load(std::memory_order_relaxed)) return true;
    const U64 budget = sBudget.load(std::memory_order_relaxed);
    U64 limit = budget;
    if (pri == LLMemoryGovernor::PRI_HIGH) limit = budget * 2;
    else if (pri == LLMemoryGovernor::PRI_LOW) limit = budget / 2;
    U64 load = sInFlightTotal.load(std::memory_order_relaxed);
    // Retained growth (copies piling up faster than they are trimmed) counts against
    // non-high work when memory is short -- and (#485) whenever the retained copies are
    // over their allowance, in any state: on a big machine the state never leaves
    // COMFORTABLE, and a fast flight took the copies to 7.5 GB against a 4 GB allowance.
    // sRetainedLoad is only the growth above max(allowance, 30 s low), so a steady
    // level stops counting and loading never stops (NEVER ZERO above).
    if (pri != LLMemoryGovernor::PRI_HIGH
        && (sState.load(std::memory_order_relaxed) != LLMemoryGovernor::COMFORTABLE
            || sRetained.load(std::memory_order_relaxed) > sRetainedAllowance.load(std::memory_order_relaxed)))
    {
        load += sRetainedLoad.load(std::memory_order_relaxed);
    }
    return load + bytes <= limit;
}

void addInFlight(LLMemoryGovernor::ESubsystem sub, U64 bytes)
{
    sInFlight[sub].fetch_add(bytes, std::memory_order_relaxed);
    sInFlightTotal.fetch_add(bytes, std::memory_order_relaxed);
}

#if LL_LINUX
// Reads "Key:   12345 kB" lines of /proc/meminfo.
bool readMeminfo(U64& total, U64& avail)
{
    FILE* f = fopen("/proc/meminfo", "r");
    if (!f) return false;
    char line[256];
    unsigned long long v = 0;
    bool have_total = false, have_avail = false;
    while (fgets(line, sizeof(line), f) && !(have_total && have_avail))
    {
        if (!strncmp(line, "MemTotal:", 9) && sscanf(line + 9, "%llu", &v) == 1)
        {
            total = (U64)v * 1024ull; have_total = true;
        }
        else if (!strncmp(line, "MemAvailable:", 13) && sscanf(line + 13, "%llu", &v) == 1)
        {
            avail = (U64)v * 1024ull; have_avail = true;
        }
    }
    fclose(f);
    return have_total && have_avail;
}

// /proc/pressure/memory (kernel >= 4.20 with PSI enabled):
//   some avg10=0.00 avg60=0.00 avg300=0.00 total=0
//   full avg10=0.00 avg60=0.00 avg300=0.00 total=0
bool readPsi(F32& some10, F32& full10)
{
    FILE* f = fopen("/proc/pressure/memory", "r");
    if (!f) return false;
    char line[256];
    bool have_some = false, have_full = false;
    while (fgets(line, sizeof(line), f))
    {
        float v = 0.f;
        if (!strncmp(line, "some ", 5) && sscanf(line + 5, "avg10=%f", &v) == 1) { some10 = v; have_some = true; }
        else if (!strncmp(line, "full ", 5) && sscanf(line + 5, "avg10=%f", &v) == 1) { full10 = v; have_full = true; }
    }
    fclose(f);
    return have_some || have_full;
}
#endif
} // namespace

// static
bool LLMemoryGovernor::readSystemMemory(SystemMemory& out)
{
    out = SystemMemory();
#if LL_WINDOWS
    MEMORYSTATUSEX st;
    st.dwLength = sizeof(st);
    if (!GlobalMemoryStatusEx(&st)) return false;
    out.total = (U64)st.ullTotalPhys;
    out.avail = (U64)st.ullAvailPhys;
    out.commit_limit = (U64)st.ullTotalPageFile;
    out.commit_avail = (U64)st.ullAvailPageFile;
    out.have_commit = out.commit_limit > 0;
    return out.total > 0;
#elif LL_LINUX
    if (!readMeminfo(out.total, out.avail)) return false;
    out.have_psi = readPsi(out.psi_some10, out.psi_full10);
    return out.total > 0;
#else
    return false;
#endif
}

// static
U64 LLMemoryGovernor::getPeakRSS()
{
#if LL_WINDOWS
    PROCESS_MEMORY_COUNTERS counters;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
    {
        return (U64)counters.PeakWorkingSetSize;
    }
    return 0;
#elif LL_LINUX
    U64 hwm = 0;
    if (FILE* f = fopen("/proc/self/status", "r"))
    {
        char line[256];
        unsigned long long v = 0;
        while (fgets(line, sizeof(line), f))
        {
            if (!strncmp(line, "VmHWM:", 6) && sscanf(line + 6, "%llu", &v) == 1)
            {
                hwm = (U64)v * 1024ull;
                break;
            }
        }
        fclose(f);
    }
    return hwm;
#else
    return 0;
#endif
}

// static
void LLMemoryGovernor::configure(bool enabled, U32 reserve_mb, F32 timeline_sec, U32 mmap_threshold_kb)
{
    MainState& m = ms();
    const bool was = sEnabled.exchange(enabled, std::memory_order_relaxed);
    if (was != enabled)
    {
        LL_INFOS("MemGov") << "memory governor " << (enabled ? "enabled" : "disabled") << LL_ENDL;
        waitCv().notify_all();
    }
    if (reserve_mb != m.reserve_mb)
    {
        m.reserve_mb = reserve_mb;
        m.next_sample = 0.0;   // re-derive at once
    }
    m.timeline_sec = (std::max)(timeline_sec, 0.f);
    if (!m.mallopt_done)
    {
        m.mallopt_done = true;
#if LL_LINUX && defined(__GLIBC__)
        // glibc's mmap threshold is dynamic: once a large block is freed, the threshold
        // grows to its size (up to 32 MB), and later decode buffers of that size come from
        // the per-thread arenas, which keep freed memory resident. Pinning the threshold
        // makes every buffer >= it a private mapping that goes back to the OS on free.
        if (enabled && mmap_threshold_kb > 0)
        {
            const U32 kb = (std::min)((std::max)(mmap_threshold_kb, 128u), 32u * 1024u);
            if (mallopt(M_MMAP_THRESHOLD, (int)(kb * 1024u)))
            {
                LL_INFOS("MemGov") << "malloc mmap threshold pinned at " << kb << " KB" << LL_ENDL;
            }
        }
#else
        (void)mmap_threshold_kb;
#endif
    }
}

// static
void LLMemoryGovernor::update()
{
    MainState& m = ms();
    const F64 now = nowSec();
    if (now < m.next_sample) return;
    m.next_sample = now + kSampleSec;

    SystemMemory s;
    m.have_sys = readSystemMemory(s);
    m.rss = LLMemory::getCurrentRSS();
    if (m.have_sys)
    {
        m.sys = s;
        const U64 reserve = computeReserve(s.total, m.reserve_mb);
        sTotal.store(s.total, std::memory_order_relaxed);
        sAvail.store(s.avail, std::memory_order_relaxed);
        sReserve.store(reserve, std::memory_order_relaxed);

        // State with hysteresis: worse at once, better after kImproveHoldSec.
        const U32 cur = sState.load(std::memory_order_relaxed);
        // #698: an allocation failed recently -> CRITICAL for the shed window, whatever
        // the system numbers say (Windows commit / ulimit / fragmentation can fail an
        // allocation while "available" looks fine).
        const U32 raw = sheddingNow() ? (U32)CRITICAL : rawState(s, reserve);
        U32 next = cur;
        if (raw > cur)
        {
            next = raw;
            m.better_since = -1.0;
        }
        else if (raw < cur)
        {
            if (m.better_since < 0.0 || raw != m.better_state) { m.better_since = now; m.better_state = raw; }
            else if (now - m.better_since >= kImproveHoldSec) { next = raw; m.better_since = -1.0; }
        }
        else
        {
            m.better_since = -1.0;
        }
        if (next != cur)
        {
            sState.store(next, std::memory_order_relaxed);
            LL_INFOS("MemGov") << "memory state " << stateName((EState)cur) << " -> " << stateName((EState)next)
                               << ": available " << (s.avail / MB) << " MB of " << (s.total / MB)
                               << " MB (reserve " << (reserve / MB) << " MB)"
                               << (s.have_psi ? " psi some/full " : "")
                               << (s.have_psi ? std::to_string(s.psi_some10) + "/" + std::to_string(s.psi_full10) : std::string())
                               << ", rss " << (m.rss / MB) << " MB" << LL_ENDL;
            waitCv().notify_all();
        }
        const U64 old_budget = sBudget.exchange(computeBudget(next, s.total, s.avail, reserve), std::memory_order_relaxed);
        if (sBudget.load(std::memory_order_relaxed) > old_budget) waitCv().notify_all();
        sRetainedAllowance.store(computeRetainedAllowance(next, s.total), std::memory_order_relaxed);
    }

    // Retained growth: bytes over the minimum of the last kRetainedWindow seconds (a
    // persistent level -- e.g. pinned avatar copies -- stops counting after the window).
    {
        const U64 r = sRetained.load(std::memory_order_relaxed);
        if (!m.ret_init)
        {
            for (U64& b : m.ret_bucket) b = r;
            m.ret_init = true;
            m.ret_bucket_start = now;
        }
        if (now - m.ret_bucket_start >= 1.0)
        {
            m.ret_head = (m.ret_head + 1) % kRetainedWindow;
            m.ret_bucket[m.ret_head] = r;
            m.ret_bucket_start = now;
        }
        else
        {
            m.ret_bucket[m.ret_head] = (std::min)(m.ret_bucket[m.ret_head], r);
        }
        U64 low = r;
        for (U64 b : m.ret_bucket) low = (std::min)(low, b);
        const U64 allowance = sRetainedAllowance.load(std::memory_order_relaxed);
        // #698: while shedding, a plateau above the allowance no longer becomes the new
        // baseline -- all of the overshoot counts (and is trimmed), not just fresh growth.
        const U64 base = sheddingNow() ? allowance : (std::max)(low, allowance);
        const U64 prev = sRetainedLoad.exchange(r > base ? r - base : 0, std::memory_order_relaxed);
        if (sRetainedLoad.load(std::memory_order_relaxed) < prev) waitCv().notify_all();
    }

    // #698: emergency reserve bookkeeping (main thread). Log a release the new_handler
    // did; end-of-shed log; re-acquire once COMFORTABLE has held for a while.
    {
        if (sReserveLogPending.exchange(false, std::memory_order_relaxed))
        {
            LL_WARNS("MemGov") << "#698 emergency reserve released (allocation failure); shedding for "
                               << (sShedMs.load(std::memory_order_relaxed) / 1000) << " s | " << allocFailString() << LL_ENDL;
        }
        const bool shedding = sheddingNow();
        if (!m.have_sys && !shedding && sState.load(std::memory_order_relaxed) != COMFORTABLE)
        {
            // No system numbers (the state never left COMFORTABLE on its own): only a
            // shed window put it here -- end it with the window.
            sState.store(COMFORTABLE, std::memory_order_relaxed);
            sBudget.store(2 * GB, std::memory_order_relaxed);   // the unsampled default
            waitCv().notify_all();
        }
        if (m.was_shedding && !shedding)
        {
            LL_INFOS("MemGov") << "#698 shed window ended | " << allocFailString() << LL_ENDL;
        }
        m.was_shedding = shedding;
        const U32 st = sState.load(std::memory_order_relaxed);
        if (shedding || st != COMFORTABLE) m.comfy_since = -1.0;
        else if (m.comfy_since < 0.0) m.comfy_since = now;
        if (m.alloc_reserve_mb && !sReserveBlock.load(std::memory_order_relaxed)
            && m.comfy_since >= 0.0 && now - m.comfy_since >= kReserveComfortSec)
        {
            if (acquireReserve((U64)m.alloc_reserve_mb * MB))
            {
                sReserveReacquired.fetch_add(1, std::memory_order_relaxed);
                LL_INFOS("MemGov") << "#698 emergency reserve re-acquired (" << m.alloc_reserve_mb << " MB)" << LL_ENDL;
            }
            m.comfy_since = now;   // failed: try again after another comfortable stretch
        }
    }

    // waits / s
    if (now - m.rate_t >= 1.0)
    {
        const U64 d = sDenied.load(std::memory_order_relaxed), b = sBlockedWaits.load(std::memory_order_relaxed);
        if (m.rate_t > 0.0)
        {
            const F64 dt = now - m.rate_t;
            m.denied_per_sec = (F32)((F64)(d - m.rate_denied) / dt);
            m.blocked_per_sec = (F32)((F64)(b - m.rate_blocked) / dt);
        }
        m.rate_t = now; m.rate_denied = d; m.rate_blocked = b;
    }

    // Timeline (login / teleport): one line per sample until it ends.
    if (m.tl_start >= 0.0)
    {
        m.tl_rss_peak = (std::max)(m.tl_rss_peak, m.rss);
        if (now >= m.tl_next)
        {
            m.tl_next = now + kSampleSec - 0.01;
            std::ostringstream o;
            o.setf(std::ios::fixed);
            o.precision(2);
            o << "MemTimeline[" << m.tl_reason << "] +" << (now - m.tl_start) << "s rss " << (m.rss / MB)
              << " MB | " << statusString();
            LL_INFOS("MemGov") << o.str() << LL_ENDL;
        }
        if (now >= m.tl_end)
        {
            LL_INFOS("MemGov") << "MemTimeline[" << m.tl_reason << "] end: rss at start " << (m.tl_rss_start / MB)
                               << " MB, peak sampled " << (m.tl_rss_peak / MB) << " MB, now " << (m.rss / MB)
                               << " MB; process peak (VmHWM / PeakWorkingSet) " << (getPeakRSS() / MB) << " MB" << LL_ENDL;
            m.tl_start = m.tl_end = -1.0;
        }
    }
    else if (now >= m.next_status_log)
    {
        if (m.next_status_log > 0.0)
        {
            LL_INFOS("MemGov") << "memgov: rss " << (m.rss / MB) << " MB peak " << (getPeakRSS() / MB) << " MB | "
                               << statusString() << LL_ENDL;
        }
        m.next_status_log = now + kStatusLogSec;
    }
}

// static
void LLMemoryGovernor::shutdown()
{
    sShutdown.store(true, std::memory_order_relaxed);
    waitCv().notify_all();
}

// static
bool LLMemoryGovernor::isEnabled() { return sEnabled.load(std::memory_order_relaxed); }

// static
LLMemoryGovernor::EState LLMemoryGovernor::getState()
{
    if (sheddingNow()) return CRITICAL;   // #698: even with the governor disabled
    if (!sEnabled.load(std::memory_order_relaxed)) return COMFORTABLE;
    return (EState)sState.load(std::memory_order_relaxed);
}

// static
const char* LLMemoryGovernor::stateName(EState s)
{
    switch (s)
    {
    case COMFORTABLE: return "comfortable";
    case TIGHT:       return "tight";
    case CRITICAL:    return "critical";
    default:          return "?";
    }
}

// static
U64 LLMemoryGovernor::getBudgetBytes() { return sBudget.load(std::memory_order_relaxed); }
// static
U64 LLMemoryGovernor::getTotalRAM() { return sTotal.load(std::memory_order_relaxed); }
// static
U64 LLMemoryGovernor::getAvailBytes() { return sAvail.load(std::memory_order_relaxed); }
// static
U64 LLMemoryGovernor::getReserveBytes() { return sReserve.load(std::memory_order_relaxed); }
// static
U64 LLMemoryGovernor::getInFlightBytes() { return sInFlightTotal.load(std::memory_order_relaxed); }
// static
U64 LLMemoryGovernor::getInFlightBytes(ESubsystem sub)
{
    return sub < SUB_COUNT ? sInFlight[sub].load(std::memory_order_relaxed) : 0;
}

// static
void LLMemoryGovernor::setRetainedBytes(U64 bytes) { sRetained.store(bytes, std::memory_order_relaxed); }
// static
U64 LLMemoryGovernor::getRetainedAllowance() { return sRetainedAllowance.load(std::memory_order_relaxed); }

// static
bool LLMemoryGovernor::shouldTrimAggressively()
{
    if (sheddingNow()) return true;   // #698
    if (!sEnabled.load(std::memory_order_relaxed)) return false;
    return sState.load(std::memory_order_relaxed) != COMFORTABLE
        || sRetained.load(std::memory_order_relaxed) > sRetainedAllowance.load(std::memory_order_relaxed);
}

// static
U64 LLMemoryGovernor::scaleCacheBudget(U64 requested, U32 ram_divisor, U64 floor)
{
    const U64 total = sTotal.load(std::memory_order_relaxed);
    const bool shedding = sheddingNow();   // #698: shed even with the governor disabled
    if ((!sEnabled.load(std::memory_order_relaxed) && !shedding) || !total || !ram_divisor) return requested;
    U64 scaled = (std::max)(floor, total / ram_divisor);
    if (shedding || sState.load(std::memory_order_relaxed) == CRITICAL) scaled = (std::max)(floor, scaled / 2);
    return (std::min)(requested, scaled);
}

// static
F32 LLMemoryGovernor::httpConcurrencyScale()
{
    switch (getState())
    {
    case TIGHT:    return 0.5f;
    case CRITICAL: return 0.25f;
    default:       return 1.f;
    }
}

// static
bool LLMemoryGovernor::tryAcquire(ESubsystem sub, U64 bytes, EPriority pri, bool count_denial)
{
    if (sub >= SUB_COUNT) return true;
    if (!admit(sub, bytes, pri))
    {
        if (count_denial) sDenied.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    addInFlight(sub, bytes);
    return true;
}

// static
void LLMemoryGovernor::acquire(ESubsystem sub, U64 bytes, EPriority pri, U32 max_wait_ms)
{
    if (sub >= SUB_COUNT) return;
    if (admit(sub, bytes, pri))
    {
        addInFlight(sub, bytes);
        return;
    }
    sBlockedWaits.fetch_add(1, std::memory_order_relaxed);
    noteWaiter(1);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(max_wait_ms);
    bool admitted = false;
    {
        std::unique_lock<std::mutex> lk(waitMutex());
        while (!admitted)
        {
            // Short timed waits: releases notify, but budget changes come from update().
            waitCv().wait_for(lk, std::chrono::milliseconds(50));
            admitted = admit(sub, bytes, pri);
            if (!admitted && std::chrono::steady_clock::now() >= deadline)
            {
                sForcedAdmits.fetch_add(1, std::memory_order_relaxed);
                break;
            }
        }
    }
    noteWaiter(-1);
    addInFlight(sub, bytes);
}

// static
void LLMemoryGovernor::release(ESubsystem sub, U64 bytes)
{
    if (sub >= SUB_COUNT || !bytes) return;
    sReleases.fetch_add(1, std::memory_order_relaxed);
    // Saturating: a release can never drive the counters below zero.
    U64 cur = sInFlight[sub].load(std::memory_order_relaxed);
    U64 take = 0;
    do { take = (std::min)(cur, bytes); }
    while (!sInFlight[sub].compare_exchange_weak(cur, cur - take, std::memory_order_relaxed));
    cur = sInFlightTotal.load(std::memory_order_relaxed);
    U64 take_total = 0;
    do { take_total = (std::min)(cur, take); }
    while (!sInFlightTotal.compare_exchange_weak(cur, cur - take_total, std::memory_order_relaxed));
    if (sWaiters.load(std::memory_order_relaxed) > 0) waitCv().notify_all();
}

// static
void LLMemoryGovernor::noteBytes(ESubsystem sub, S64 delta)
{
    if (sub < SUB_COUNT) sGauge[sub].fetch_add(delta, std::memory_order_relaxed);
}

// static
S64 LLMemoryGovernor::getGaugeBytes(ESubsystem sub)
{
    return sub < SUB_COUNT ? sGauge[sub].load(std::memory_order_relaxed) : 0;
}

// static
U64 LLMemoryGovernor::getReleaseCount() { return sReleases.load(std::memory_order_relaxed); }

// static
void LLMemoryGovernor::noteWaiter(S32 delta) { sWaiters.fetch_add(delta, std::memory_order_relaxed); }
// static
S32 LLMemoryGovernor::getWaiters() { return sWaiters.load(std::memory_order_relaxed); }

// static
void LLMemoryGovernor::beginTimeline(const char* reason)
{
    MainState& m = ms();
    if (m.timeline_sec <= 0.f) return;
    const F64 now = nowSec();
    const bool running = m.tl_start >= 0.0;
    m.tl_reason = reason ? reason : "?";
    if (!running)
    {
        m.tl_start = now;
        m.tl_rss_start = LLMemory::getCurrentRSS();
        m.tl_rss_peak = m.tl_rss_start;
    }
    m.tl_end = now + m.timeline_sec;   // a teleport during a login window extends it
    m.tl_next = 0.0;
    m.next_sample = 0.0;
    LL_INFOS("MemGov") << "MemTimeline[" << m.tl_reason << "] " << (running ? "extended" : "start") << ": rss "
                       << (LLMemory::getCurrentRSS() / MB) << " MB, process peak " << (getPeakRSS() / MB)
                       << " MB, logging every " << (S32)(kSampleSec * 1000.0) << " ms for " << m.timeline_sec << " s"
                       << LL_ENDL;
}

// static
void LLMemoryGovernor::setExtraInfo(std::function<std::string()> fn) { ms().extra = std::move(fn); }

// static
std::string LLMemoryGovernor::statusString()
{
    MainState& m = ms();
    std::ostringstream o;
    o << "memgov " << (sEnabled.load(std::memory_order_relaxed) ? stateName((EState)sState.load(std::memory_order_relaxed)) : "off");
    if (m.have_sys)
    {
        o << " avail " << (m.sys.avail / MB) << "/" << (m.sys.total / MB) << " MB reserve "
          << (sReserve.load(std::memory_order_relaxed) / MB) << " MB";
        if (m.sys.have_psi)
        {
            o.setf(std::ios::fixed);
            o.precision(1);
            o << " psi " << m.sys.psi_some10 << "/" << m.sys.psi_full10;
        }
        if (m.sys.have_commit)
        {
            o << " commit-avail " << (m.sys.commit_avail / MB) << "/" << (m.sys.commit_limit / MB) << " MB";
        }
    }
    else
    {
        o << " (system memory unknown)";
    }
    o << " | budget " << (sBudget.load(std::memory_order_relaxed) / MB) << " MB in-flight "
      << (sInFlightTotal.load(std::memory_order_relaxed) / MB) << " MB [";
    for (U32 i = 0; i < SUB_COUNT; ++i)
    {
        if (i == SUB_HTTP) continue;   // gauge only
        o << (i ? " " : "") << kSubName[i] << " " << (sInFlight[i].load(std::memory_order_relaxed) / MB);
    }
    o << "] http-bodies " << ((U64)(std::max)(sGauge[SUB_HTTP].load(std::memory_order_relaxed), (S64)0) / MB)
      << " MB | retained " << (sRetained.load(std::memory_order_relaxed) / MB) << " MB (allowance "
      << (sRetainedAllowance.load(std::memory_order_relaxed) / MB) << ", growth "
      << (sRetainedLoad.load(std::memory_order_relaxed) / MB) << ")";
    o.setf(std::ios::fixed);
    o.precision(1);
    o << " | waits/s denied " << m.denied_per_sec << " blocked " << m.blocked_per_sec << " (waiting "
      << sWaiters.load(std::memory_order_relaxed) << ", forced " << sForcedAdmits.load(std::memory_order_relaxed) << ")";
    {
        const AllocFailStats a = getAllocFailStats();
        U64 fails = 0;
        for (U32 i = 0; i < ALLOC_KIND_COUNT; ++i) fails += a.failures[i];
        if (fails || a.reserve_released || sheddingNow()) o << " | " << allocFailString();
    }
    if (m.extra)
    {
        const std::string e = m.extra();
        if (!e.empty()) o << " | " << e;
    }
    return o.str();
}

// ---- #698 allocation failures ----

// static
void LLMemoryGovernor::configureAllocFailure(U32 reserve_mb, U32 retry_limit, F32 shed_sec, U32 inject_one_in_n)
{
    MainState& m = ms();
    sRetryLimit.store((std::max)(retry_limit, 1u), std::memory_order_relaxed);
    sShedMs.store((U32)((std::max)(shed_sec, 1.f) * 1000.f), std::memory_order_relaxed);
    const U32 prev_inject = sInjectN.exchange(inject_one_in_n, std::memory_order_relaxed);
    if (prev_inject != inject_one_in_n)
    {
        LL_WARNS("MemGov") << "#698 allocation-failure injection " << (inject_one_in_n ? "ON: every " : "off")
                           << (inject_one_in_n ? std::to_string(inject_one_in_n) + "th hooked allocation fails" : std::string())
                           << LL_ENDL;
    }
    if (!m.new_handler_set)
    {
        m.new_handler_set = true;
        std::set_new_handler(parachuteNewHandler);
    }
    reserve_mb = (std::min)(reserve_mb, 1024u);
    if (reserve_mb != m.alloc_reserve_mb)
    {
        m.alloc_reserve_mb = reserve_mb;
        // A size change: drop the old block (not a failure) and take the new one now.
        if (char* p = sReserveBlock.exchange(nullptr, std::memory_order_acq_rel))
        {
            sReserveHeld.store(0, std::memory_order_relaxed);
            delete[] p;
        }
        if (reserve_mb && !sheddingNow())
        {
            const bool ok = acquireReserve((U64)reserve_mb * MB);
            LL_INFOS("MemGov") << "#698 emergency reserve " << (ok ? "held: " : "could not be allocated: ")
                               << reserve_mb << " MB" << LL_ENDL;
        }
    }
}

// static
void LLMemoryGovernor::releaseEmergencyReserve() noexcept
{
    if (char* p = sReserveBlock.exchange(nullptr, std::memory_order_acq_rel))
    {
        sReserveHeld.store(0, std::memory_order_relaxed);
        delete[] p;
        sReserveReleased.fetch_add(1, std::memory_order_relaxed);
        sReserveLogPending.store(true, std::memory_order_relaxed);
    }
}

// static
void LLMemoryGovernor::noteAllocFailure(EAllocKind kind, U64 bytes, const char* what) noexcept
{
    // Reserve first: headroom for the caller's cleanup and for this log line.
    releaseEmergencyReserve();
    const U32 k = kind < ALLOC_KIND_COUNT ? (U32)kind : (U32)ALLOC_OTHER;
    const U64 now = nowMs();
    sAllocFail[k].fetch_add(1, std::memory_order_relaxed);
    sLastFailMs.store(now ? now : 1, std::memory_order_relaxed);
    sLastFailBytes.store(bytes, std::memory_order_relaxed);
    sLastFailKind.store(k, std::memory_order_relaxed);
    startShed();
    U64 next = sAllocLogNextMs[k].load(std::memory_order_relaxed);
    if (now < next || !sAllocLogNextMs[k].compare_exchange_strong(next, now + kAllocLogGapMs, std::memory_order_relaxed))
    {
        sAllocLogSuppressed[k].fetch_add(1, std::memory_order_relaxed);
        return;
    }
    try
    {
        const U64 suppressed = sAllocLogSuppressed[k].exchange(0, std::memory_order_relaxed);
        LL_WARNS("MemGov") << "#698 allocation failed: " << kAllocKindName[k] << " (" << (what ? what : "?") << ")"
                           << (bytes ? ", " + std::to_string(bytes >> 10) + " KB" : std::string(", size unknown"))
                           << " | memgov " << stateName((EState)sState.load(std::memory_order_relaxed))
                           << " avail " << (sAvail.load(std::memory_order_relaxed) / MB) << " MB"
                           << " in-flight " << (sInFlightTotal.load(std::memory_order_relaxed) / MB) << " MB"
                           << " retained " << (sRetained.load(std::memory_order_relaxed) / MB) << " MB"
                           << " (allowance " << (sRetainedAllowance.load(std::memory_order_relaxed) / MB) << ")"
                           << " | reserve " << (sReserveHeld.load(std::memory_order_relaxed) / MB) << " MB held, released "
                           << sReserveReleased.load(std::memory_order_relaxed) << "x"
                           << " | " << kAllocKindName[k] << " failures " << sAllocFail[k].load(std::memory_order_relaxed)
                           << " retried " << sAllocRetried[k].load(std::memory_order_relaxed)
                           << " dropped " << sAllocDropped[k].load(std::memory_order_relaxed)
                           << (suppressed ? ", " + std::to_string(suppressed) + " more not logged" : std::string())
                           << LL_ENDL;
    }
    catch (...)
    {
        // out of memory while logging: the counters above still carry it
    }
}

// static
void LLMemoryGovernor::noteAllocRetry(EAllocKind kind) noexcept
{
    sAllocRetried[kind < ALLOC_KIND_COUNT ? kind : ALLOC_OTHER].fetch_add(1, std::memory_order_relaxed);
}

// static
void LLMemoryGovernor::noteAllocDrop(EAllocKind kind) noexcept
{
    sAllocDropped[kind < ALLOC_KIND_COUNT ? kind : ALLOC_OTHER].fetch_add(1, std::memory_order_relaxed);
}

// static
U32 LLMemoryGovernor::retryLimit() { return sRetryLimit.load(std::memory_order_relaxed); }

// static
F32 LLMemoryGovernor::retryDelaySec(U32 attempt)
{
    const U32 n = attempt > 0 ? attempt - 1 : 0;
    return (std::min)((F32)(1u << (std::min)(n, 5u)), 30.f);
}

// static
bool LLMemoryGovernor::isShedding() { return sheddingNow(); }

// static
bool LLMemoryGovernor::shouldInjectAllocFail() noexcept
{
    const U32 n = sInjectN.load(std::memory_order_relaxed);
    if (!n) return false;
    if ((sInjectCounter.fetch_add(1, std::memory_order_relaxed) + 1) % n != 0) return false;
    sInjected.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// static
void LLMemoryGovernor::maybeInjectAllocFail()
{
    if (shouldInjectAllocFail()) throw std::bad_alloc();
}

// static
LLMemoryGovernor::AllocFailStats LLMemoryGovernor::getAllocFailStats()
{
    AllocFailStats a;
    for (U32 i = 0; i < ALLOC_KIND_COUNT; ++i)
    {
        a.failures[i] = sAllocFail[i].load(std::memory_order_relaxed);
        a.retried[i] = sAllocRetried[i].load(std::memory_order_relaxed);
        a.dropped[i] = sAllocDropped[i].load(std::memory_order_relaxed);
    }
    a.reserve_released = sReserveReleased.load(std::memory_order_relaxed);
    a.reserve_reacquired = sReserveReacquired.load(std::memory_order_relaxed);
    a.reserve_held = sReserveHeld.load(std::memory_order_relaxed);
    a.sheds = sSheds.load(std::memory_order_relaxed);
    a.injected = sInjected.load(std::memory_order_relaxed);
    a.last_bytes = sLastFailBytes.load(std::memory_order_relaxed);
    a.last_kind = sLastFailKind.load(std::memory_order_relaxed);
    const U64 last = sLastFailMs.load(std::memory_order_relaxed);
    a.last_age_sec = last ? (F64)(nowMs() - last) / 1000.0 : -1.0;
    return a;
}

// static
const char* LLMemoryGovernor::allocKindName(EAllocKind kind)
{
    return kAllocKindName[kind < ALLOC_KIND_COUNT ? kind : ALLOC_OTHER];
}

// static
std::string LLMemoryGovernor::allocFailString()
{
    const AllocFailStats a = getAllocFailStats();
    std::ostringstream o;
    o << "oom: reserve " << (a.reserve_held / MB) << " MB (released " << a.reserve_released << " reacquired "
      << a.reserve_reacquired << ") shed " << (sheddingNow() ? "ON" : "off") << " (" << a.sheds << "x)";
    bool any = false;
    for (U32 i = 0; i < ALLOC_KIND_COUNT; ++i)
    {
        if (!a.failures[i] && !a.retried[i] && !a.dropped[i]) continue;
        o << (any ? " " : " [") << kAllocKindName[i] << " " << a.failures[i] << "/" << a.retried[i] << "/" << a.dropped[i];
        any = true;
    }
    if (any) o << "] (fail/retry/drop)";
    if (a.injected) o << " injected " << a.injected;
    return o.str();
}
