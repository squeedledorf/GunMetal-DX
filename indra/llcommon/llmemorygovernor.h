/**
 * @file llmemorygovernor.h
 * @brief System memory governor: paces heavy transient work (decodes, unzip, big
 *        downloads) by the memory the SYSTEM has available, so loading bursts (login,
 *        teleport arrival) don't push other programs into swap (#262).
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

#ifndef LL_LLMEMORYGOVERNOR_H
#define LL_LLMEMORYGOVERNOR_H

#include "stdtypes.h"
#include "llpreprocessor.h"

#include <functional>
#include <string>

// HOW IT WORKS
//  - update() (main thread, self rate-limited to ~4 Hz) samples the system: available
//    physical memory (Linux /proc/meminfo MemAvailable; Windows GlobalMemoryStatusEx),
//    memory pressure stall info (Linux PSI /proc/pressure/memory, when present), the
//    commit charge (Windows), and our own RSS.
//  - It keeps a RESERVE for the rest of the system (MemoryReserveMB, 0 = auto:
//    max(1.5 GB, 12% of RAM)) and derives a state:
//      COMFORTABLE: available >= 1.5 x reserve and no pressure stalls;
//      TIGHT:       available below 1.5 x reserve, or PSI "some" avg10 >= 5%;
//      CRITICAL:    available below reserve / 2, or PSI "full" avg10 >= 5% / "some" >= 30%.
//    Getting worse applies at once; getting better only after it holds for 2 s.
//  - A BYTE BUDGET for in-flight heavy work: producers acquire their estimated peak
//    bytes before starting and release them when the result is handed off.
//      COMFORTABLE: clamp((avail - reserve) / 4, 256 MB, clamp(RAM / 32, 256 MB, 2 GB))
//      TIGHT:       clamp((avail - reserve) / 8, floor, cap / 4)
//      CRITICAL:    floor (96 MB)
//    NEVER ZERO: a subsystem with nothing in flight is always admitted, whatever the
//    size, so loading slows down but never stops. High priority (avatar / large on
//    screen) may use 2x the budget, low priority (tiny / far) half of it.
//  - RETAINED bytes (decoded pixel copies a renderer keeps until it trims them) are
//    reported by the viewer. Above a RAM-scaled allowance -- or whenever the state isn't
//    comfortable -- shouldTrimAggressively() asks the owners to trim sooner. In TIGHT /
//    CRITICAL the retained GROWTH over its recent (30 s) low also counts against the
//    budget for non-high-priority work, so a burst can't outrun the trimming.
class LL_COMMON_API LLMemoryGovernor
{
public:
    enum EState : U32 { COMFORTABLE = 0, TIGHT = 1, CRITICAL = 2 };
    enum ESubsystem : U32
    {
        SUB_TEX_DECODE = 0,   // J2C / other image decodes (input + output + codec scratch)
        SUB_MESH_DECODE,      // mesh LOD unzip + parse + tangents
        SUB_TEX_UPLOAD,       // renderer upload staging outside its fixed ring
        SUB_HTTP,             // HTTP response bodies alive (gauge: llcorehttp BufferArray)
        SUB_COUNT
    };
    enum EPriority : U32 { PRI_LOW = 0, PRI_NORMAL = 1, PRI_HIGH = 2 };

    struct SystemMemory
    {
        U64 total = 0;          // physical RAM
        U64 avail = 0;          // available to new allocations without swapping
        U64 commit_limit = 0;   // Windows: commit limit (RAM + page file)
        U64 commit_avail = 0;   // Windows: commit still available
        F32 psi_some10 = 0.f;   // Linux PSI: % of time some task stalled on memory (10 s avg)
        F32 psi_full10 = 0.f;   // Linux PSI: % of time ALL tasks stalled on memory
        bool have_psi = false;
        bool have_commit = false;
    };
    // Any thread. False when the platform can't tell (the governor then stays comfortable).
    static bool readSystemMemory(SystemMemory& out);
    // Peak resident set of this process (Linux VmHWM, Windows PeakWorkingSetSize); 0 if unknown.
    static U64 getPeakRSS();

    // Main thread. Cheap; call every frame with the current settings.
    //  reserve_mb: memory to leave for other programs (0 = auto).
    //  timeline_sec: length of the RSS timeline logged after login / teleport (0 = off).
    //  mmap_threshold_kb (Linux/glibc, applied once): allocations at least this big are
    //  mmap'd and go back to the OS as soon as they are freed (0 = glibc's dynamic default).
    static void configure(bool enabled, U32 reserve_mb, F32 timeline_sec, U32 mmap_threshold_kb);
    // Main thread, every frame; samples at most every 250 ms.
    static void update();
    // Admit everything and wake blocked acquirers (app shutdown).
    static void shutdown();

    static bool isEnabled();
    static EState getState();
    static const char* stateName(EState s);
    static U64 getBudgetBytes();
    static U64 getTotalRAM();        // 0 until the first sample
    static U64 getAvailBytes();
    static U64 getReserveBytes();
    static U64 getInFlightBytes();
    static U64 getInFlightBytes(ESubsystem sub);

    // Retained (already decoded, held until trimmed) bytes -- main thread, every frame.
    static void setRetainedBytes(U64 bytes);
    static U64 getRetainedAllowance();
    // True when retained copies should be trimmed without the usual grace delays.
    static bool shouldTrimAggressively();

    // Cache budget scaled to this machine: min(requested, max(floor, RAM / ram_divisor)),
    // halved in CRITICAL. Never raises an explicit request.
    static U64 scaleCacheBudget(U64 requested, U32 ram_divisor, U64 floor);
    // Fraction of the normal HTTP concurrency to use (1 / 0.5 / 0.25).
    static F32 httpConcurrencyScale();

    // Byte semaphore. tryAcquire: any thread, never blocks; true = admitted and counted.
    // count_denial: false for a re-try of the same waiting job (#485: the "denied/s"
    // stat counts jobs that had to wait, not polls).
    static bool tryAcquire(ESubsystem sub, U64 bytes, EPriority pri, bool count_denial = true);
    // #485: number of release() calls so far (any thread): a waiter list can wake its
    // waiters only when bytes were actually freed.
    static U64 getReleaseCount();
    // Worker threads only: blocks until admitted (re-checked as bytes are released;
    // admitted anyway after max_wait_ms so nothing can wedge).
    static void acquire(ESubsystem sub, U64 bytes, EPriority pri, U32 max_wait_ms = 4000);
    static void release(ESubsystem sub, U64 bytes);
    // Gauge only (not budgeted): bytes alive in a subsystem, e.g. HTTP bodies.
    static void noteBytes(ESubsystem sub, S64 delta);
    static S64 getGaugeBytes(ESubsystem sub);

    // Work waiting for admission right now (e.g. fetch workers parked before decode).
    static void noteWaiter(S32 delta);
    static S32 getWaiters();

    // RSS timeline: one line every 250 ms for timeline_sec (see configure()).
    static void beginTimeline(const char* reason);
    // Main thread: extra per-subsystem fields appended to timeline / status lines.
    static void setExtraInfo(std::function<std::string()> fn);
    // One-line status (state, available, budget, in flight, waits/s ...).
    static std::string statusString();

    // ---- MALLOC HEAP (#1103; Linux / glibc only, no-ops elsewhere) ----
    // Caps glibc's malloc arenas (M_ARENA_MAX) at clamp(hardware threads / 2, 4, 16) unless
    // MALLOC_ARENA_MAX / GLIBC_TUNABLES already set it. Call FIRST in main(), before any
    // thread exists: glibc fixes its arena limit for good once more than 8 arenas were
    // created, so a later call has no effect. No logging (the log does not exist yet);
    // mallocArenaText() says what was decided, for a later log line.
    static void limitMallocArenas();
    static std::string mallocArenaText();
    // Hands freed heap memory back to the OS (glibc malloc_trim(0)) and measures it. Any
    // thread; it locks each arena in turn while it walks it (tens to hundreds of ms on a
    // large fragmented heap), so call it on a worker. done = false where unsupported.
    struct HeapTrim
    {
        bool done = false;
        U64 rss_before = 0;     // resident set before / after the trim
        U64 rss_after = 0;
        U64 free_before = 0;    // free bytes the arenas held before it (malloc_info fast + rest)
        U32 arenas = 0;         // arenas in the process (malloc_info heaps)
        F64 ms = 0.0;           // time malloc_trim took
    };
    static HeapTrim trimMallocHeap();

    // ---- ALLOCATION FAILURES (#698) ----
    // Owner policy: a failed allocation is NEVER a crash. The job that failed frees what
    // it built, its OWNER requeues the asset with backoff (retryDelaySec) and gives it up
    // after retryLimit() tries (logged); the asset is eligible again later. Every failure
    // reported here:
    //  - releases the EMERGENCY RESERVE (a pre-committed block, SLMemEmergencyReserveMB)
    //    so the failing job's cleanup, the log line and the next frame have headroom. It
    //    is re-acquired once the state has been COMFORTABLE for a while. A std::new_handler
    //    does the same inside operator new, so the first failing `new` anywhere is retried
    //    transparently after the reserve is freed;
    //  - SHEDS: the state is forced to CRITICAL for SLMemOOMShedSec (extended by every
    //    further failure) -- the decode / cache budgets drop to their floors, HTTP
    //    concurrency to 1/4, the retained allowance to 1/4 and its 30 s-low baseline is
    //    ignored, so retained CPU texture copies are thumbnailed at once (isShedding() also
    //    widens the texture list's per-frame trim). Soft: it ends on its own, no setting
    //    is touched;
    //  - is counted (failures / retried / dropped per kind) for diagnostics (#700) and logged
    //    rate-limited with the governor state.
    enum EAllocKind : U32
    {
        ALLOC_TEX_DECODE = 0,   // image decode job (LLImageDecodeThread)
        ALLOC_MESH_DECODE,      // LL mesh LOD / skin / physics parse (LLVolume unpack, mesh pool)
        ALLOC_MESH_FETCH,       // mesh repo cache-read buffers (mesh repo thread)
        ALLOC_CLEAN_DECODE,     // clean pipeline async mesh decode (CleanMeshDecode pool)
        ALLOC_STORE_FILL,       // VK store-fill worker (renderer CPU mirrors)
        ALLOC_WORK_ITEM,        // generic WorkQueue / ThreadPool item (no owner retry)
        ALLOC_THREAD,           // a thread's run() (LLThread) or worker loop iteration
        ALLOC_MAIN_FRAME,       // main thread, caught at the frame boundary
        ALLOC_OTHER,
        ALLOC_KIND_COUNT
    };
    struct AllocFailStats
    {
        U64 failures[ALLOC_KIND_COUNT] = {};   // allocation failures reported
        U64 retried[ALLOC_KIND_COUNT]  = {};   // requeued with backoff by their owner
        U64 dropped[ALLOC_KIND_COUNT]  = {};   // given up after the retry limit (eligible again later)
        U64 reserve_released   = 0;            // emergency reserve frees (first failure / new_handler)
        U64 reserve_reacquired = 0;
        U64 reserve_held       = 0;            // bytes held right now (0 = released / disabled)
        U64 sheds              = 0;            // forced-CRITICAL shed windows started
        U64 injected           = 0;            // SLMemInjectAllocFail test failures thrown
        U64 last_bytes         = 0;            // size of the last reported failure (0 = unknown)
        U32 last_kind          = ALLOC_OTHER;
        F64 last_age_sec       = -1.0;         // seconds since the last failure (-1 = never)
    };
    // Main thread (first call installs the new_handler). reserve_mb 0 = no reserve.
    static void configureAllocFailure(U32 reserve_mb, U32 retry_limit, F32 shed_sec, U32 inject_one_in_n);
    // Any thread, never throws: report a failure (see above). 'what' is a short static
    // description of the site; bytes 0 = unknown.
    static void noteAllocFailure(EAllocKind kind, U64 bytes, const char* what) noexcept;
    // Any thread: the owner requeued the failed job / gave it up.
    static void noteAllocRetry(EAllocKind kind) noexcept;
    static void noteAllocDrop(EAllocKind kind) noexcept;
    // Retry policy for owners: tries before an asset is dropped, and the backoff before
    // try n (1-based: 1 s, 2 s, 4 s ... capped at 30 s).
    static U32 retryLimit();
    static F32 retryDelaySec(U32 attempt);
    // True inside a shed window (a failure happened within SLMemOOMShedSec).
    static bool isShedding();
    // Free the emergency reserve now (any thread; new_handler-safe, no logging).
    static void releaseEmergencyReserve() noexcept;
    // Test switch (SLMemInjectAllocFail = N): true on every Nth call (any thread).
    // maybeInjectAllocFail() throws std::bad_alloc instead; call it inside a job's try.
    static bool shouldInjectAllocFail() noexcept;
    static void maybeInjectAllocFail();
    static AllocFailStats getAllocFailStats();
    static const char* allocKindName(EAllocKind kind);
    // One line for logs / diagnostics (#700): reserve, shed, failures/retried/dropped by kind.
    static std::string allocFailString();
};

// RAII ticket for LLMemoryGovernor bytes.
class LLMemoryTicket
{
public:
    LLMemoryTicket() = default;
    LLMemoryTicket(LLMemoryGovernor::ESubsystem sub, U64 bytes) : mSub(sub), mBytes(bytes) {}
    ~LLMemoryTicket() { reset(); }
    LLMemoryTicket(const LLMemoryTicket&) = delete;
    LLMemoryTicket& operator=(const LLMemoryTicket&) = delete;
    void reset()
    {
        if (mBytes) LLMemoryGovernor::release(mSub, mBytes);
        mBytes = 0;
    }
private:
    LLMemoryGovernor::ESubsystem mSub = LLMemoryGovernor::SUB_TEX_DECODE;
    U64 mBytes = 0;
};

#endif // LL_LLMEMORYGOVERNOR_H
