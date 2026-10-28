/**
 * @file llvulkancontext.cpp
 * @brief See llvulkancontext.h. Concrete Vulkan bring-up + clear/present frame.
 */
#include "linden_common.h"
#include "../llvkhitch.h"
#include "../gmdxreplay.h"           // <GMDX> instant replay

#include "llvulkancontext.h"

#include "vk_device.h"
#include "vk_requirements.h"                 // #526 device requirements + user message
#include "vk_swapchain.h"
#include "llrendervk/llrendervk.h"       // the facade: gVK
#include "llrendervkbackend.h"           // concrete LLRenderBackend
#include "vk_ui_shaders_spv.h"           // embedded UI SPIR-V
#include "llcleanrenderervk.h"           // GPU-driven static-opaque world pass
#include "llcleansnapshotvk.h"           // #873 live snapshots
#include "llcleanclustercaps.h"          // #743 cluster LOD tier log line
#include "llvkneuralrecon.h"             // #870 / #962 optional DLSS shim (slip_nr: SR + RR)
#include "llxrsession.h"                 // #1073 VR (OpenXR); inert unless VR was chosen at startup
#include "llfxruntimevk.h"               // FX compile watchdog (shutdown)
#include "llrendervk/vk_offscreen.h"     // OffscreenTarget: milestone PNG readback

#include "llrender.h"                    // LLRender::setBackend
#include "llerror.h"
#include "llformat.h"                    // #458 hostMemText
#include "llmemorygovernor.h"            // #458 UI upload ring cap scaled by RAM
#include "llmemory.h"                    // #471 LLMemory::getCurrentRSS (compile memory probe)
#include "vk_pipeline.h"                 // #462 vkSetPipelineCompileThreads / progress hook
#include "vk_pipecache.h"                // #471 learned compile costs (load / save)
#include "workqueue.h"                   // post-batch malloc_trim (General queue)
#include "llthread.h"                    // #1103 on_main_thread (where the post-batch trim runs)

#if LL_WINDOWS
#   include "llwin32headers.h"
#   include <psapi.h>                    // #471 GetProcessMemoryInfo (private bytes)
// #919 VK_EXT_full_screen_exclusive: its structs / entry point types live in the Win32
// platform header, which vulkan.h only includes with VK_USE_PLATFORM_WIN32_KHR (not defined
// for the build). It only needs the Windows types (above) and vulkan_core.h.
#   include <vulkan/vulkan_win32.h>
#endif

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

// The vkgfx layer uses VK_CHECK/std::abort on hard errors, which is acceptable
// for a de-risking bring-up: a failed swapchain create is not recoverable and we
// want the failure loud. init() below guards the recoverable/expected failures
// (no surface support, minimized) and returns false instead.

struct LLVulkanContext::Impl
{
    SDL_Window*     mWindow  = nullptr;
    VkSurfaceKHR    mSurface = VK_NULL_HANDLE;
    VkDeviceContext mDevice;
    VkSwapchain     mSwapchain;

    // One primary command buffer PER frame-in-flight slot. A single shared command
    // buffer was the flashing bug: with 2 frames in flight and pre-signaled fences,
    // frame N+1's vkBeginCommandBuffer implicitly reset the buffer while frame N's
    // submission was still pending, so the image frame N presented never reliably
    // got its clear+UI (black / stale-VRAM static, rotating through the images).
    // Per-slot buffers are re-recorded only after acquire() waits that slot's fence.
    std::vector<VkCommandBuffer> mCmds;

    uint32_t mWidth  = 0;
    uint32_t mHeight = 0;
    bool     mNeedsRecreate = false;
    uint32_t mFrameCounter  = 0;

    // EXPERIMENTAL Vulkan UI: the facade-backed LLRender backend (installed into
    // LLRender::setBackend). Owned here; null until initRenderBackend().
    LLRenderVKBackend* mRenderBackend = nullptr;
    bool mFacadeReady = false;

    // GPU-driven world renderer (static opaque). Created lazily on the first
    // frame that requests world rendering; null until then. Behind the VK flag.
    LLCleanRendererVK* mWorld = nullptr;
    bool mWorldInitTried = false;

    // #1074 VR: while an OpenXR session runs, the world renders into this eye-size target
    // (window format, so the final pipelines are reused) instead of the window image; it is
    // copied into both eye images and blitted aspect-fit onto the window (the mirror). Only
    // ever created in VR; released when the session stops.
    GpuImage mVrTarget;
    bool     mVrBlitChecked = false;
    bool     mVrBlitOk = false;
    VkFilter mVrBlitFilter = VK_FILTER_LINEAR;
    bool     mVrWorldLast = false;   // the previous frame was a VR world frame
    // #1076 VR: the UI wall's own image (window size and format): the UI replayed over a
    // transparent clear, so the wall shows the world between the floaters. VR world frames
    // with the wall shown only; released with the eye target.
    GpuImage mVrUiTarget;
    GpuImage mVrHudDepth;   // #1076 the HUD pass's depth when the HUDs go onto the wall (same size)
    // #1076: the two soft-cap sources the renderer gets merged: "Keep my FPS" and the VR
    // safety governor (default-constructed = no cap).
    LLVKPerfCaps mAutoTuneCaps;
    LLVKPerfCaps mVrSafetyCaps;
    bool vrEnsureTarget(uint32_t w, uint32_t h);   // (defined with renderFrame)
    bool vrEnsureUiTarget(VkExtent2D ext);
    bool vrEnsureHudDepth(VkExtent2D ext, VkFormat fmt);
    void vrReleaseTarget(bool waitIdle);
    void vrRecordMirror(VkCommandBuffer cmd, VkImage windowImage, VkExtent2D windowExtent);

    // #919 full-screen exclusive (Windows). mFseExt: the device has the extension and every
    // entry point resolved. The swapchain carries the exclusive chain (mFseChained) while
    // armed AND focused AND not minimised AND not suspended; the mode is acquired on such a
    // swapchain (mFseHeld) -- retried for kFseAcquireWindow after a (re)creation or a loss,
    // then suspended (composited presentation) until the next focus gain.
    bool     mFseExt = false;
    bool     mFseWanted = false;
    // Not focused until SDL says so (FOCUS_GAINED follows show()): a startup fullscreen arms
    // exclusive mode while the window is still hidden, and acquiring it then only fails.
    bool     mFseFocused = false;
    bool     mFseMinimized = false;
    bool     mFseSuspended = false;
    bool     mFseChained = false;
    bool     mFseHeld = false;
    bool     mFseTrying = false;
    std::chrono::steady_clock::time_point mFseTryStart;
#if LL_WINDOWS
    VkSurfaceFullScreenExclusiveInfoEXT      mFseInfo{VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT};
    VkSurfaceFullScreenExclusiveWin32InfoEXT mFseWin32{VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_WIN32_INFO_EXT};
    PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR  mFseCaps2 = nullptr;
    PFN_vkGetPhysicalDeviceSurfacePresentModes2EXT  mFseModes2 = nullptr;
    PFN_vkAcquireFullScreenExclusiveModeEXT         mFseAcquire = nullptr;
    PFN_vkReleaseFullScreenExclusiveModeEXT         mFseRelease = nullptr;

    // The monitor the window is on (exclusive mode is per monitor), into the Win32 info
    // struct chained behind the exclusive info. False without a window handle.
    bool fseRefreshMonitor()
    {
        HWND hwnd = mWindow ? (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(mWindow),
                                                           SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr)
                            : nullptr;
        HMONITOR mon = hwnd ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) : nullptr;
        mFseInfo.fullScreenExclusive = VK_FULL_SCREEN_EXCLUSIVE_APPLICATION_CONTROLLED_EXT;
        mFseInfo.pNext = &mFseWin32;
        mFseWin32.pNext = nullptr;
        mFseWin32.hmonitor = mon;
        return mon != nullptr;
    }

    // Does the surface support exclusive mode on that monitor
    // (VkSurfaceCapabilitiesFullScreenExclusiveEXT, queried WITH the exclusive chain).
    bool fseSurfaceSupported()
    {
        if (!mFseExt || !mFseCaps2 || !mSurface || !fseRefreshMonitor()) return false;
        VkPhysicalDeviceSurfaceInfo2KHR si{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR};
        si.pNext = &mFseInfo;
        si.surface = mSurface;
        VkSurfaceCapabilitiesFullScreenExclusiveEXT fse{VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_FULL_SCREEN_EXCLUSIVE_EXT};
        VkSurfaceCapabilities2KHR caps{VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR};
        caps.pNext = &fse;
        const VkResult r = mFseCaps2(mDevice.physicalDevice(), &si, &caps);
        if (r != VK_SUCCESS)
        {
            LL_WARNS("Vulkan") << "Fullscreen: vkGetPhysicalDeviceSurfaceCapabilities2KHR = " << vkResultString(r) << LL_ENDL;
            return false;
        }
        return fse.fullScreenExclusiveSupported == VK_TRUE;
    }

    // The present modes valid with the exclusive chain (vkGetPhysicalDeviceSurfacePresentModes2EXT).
    bool fsePresentModes(std::vector<VkPresentModeKHR>& out)
    {
        if (!mFseModes2 || !mSurface) return false;
        VkPhysicalDeviceSurfaceInfo2KHR si{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR};
        si.pNext = &mFseInfo;
        si.surface = mSurface;
        uint32_t n = 0;
        if (mFseModes2(mDevice.physicalDevice(), &si, &n, nullptr) != VK_SUCCESS || n == 0) return false;
        out.resize(n);
        if (mFseModes2(mDevice.physicalDevice(), &si, &n, out.data()) != VK_SUCCESS) return false;
        out.resize(n);
        return n > 0;
    }

    void fseRelease(const char* why)
    {
        if (!mFseHeld) return;
        mFseHeld = false;
        if (mFseRelease && mSwapchain.handle() && !vkDeviceIsLost())
        {
            const VkResult r = mFseRelease(mDevice.device(), mSwapchain.handle());
            LL_INFOS("Vulkan") << "Fullscreen: exclusive mode released (" << why << "): " << vkResultString(r) << LL_ENDL;
        }
    }
#endif
};

LLVulkanContext& LLVulkanContext::instance()
{
    static LLVulkanContext sInstance;
    return sInstance;
}

std::string LLVulkanContext::specStatusText(bool* fallback)
{
    return LLCleanRendererVK::specStatusText(fallback);
}

// #462: persists the pipeline cache from a compile worker every few compiles, so a
// crash (or OOM) during the long cold compile at the login screen keeps its progress.
static VkDeviceContext* s_pipeSaveDev = nullptr;
static std::string s_pipeCostPath;   // #471 learned compile costs ("" = memory only)
static unsigned s_pipeProgressEvery = 8;   // #672: 4 on a cold cache
static void pipelineProgressSave()
{
    if (!s_pipeSaveDev) return;
    s_pipeSaveDev->savePipelineCache("compile progress");
    // #471: with the costs learned so far (+ the compiles in flight), so an OOM crash
    // mid-batch still teaches the next launch which pipelines are heavy. At most once a
    // second (a warm start runs ~100 cache hits in ~30 ms; the batch end saves anyway).
    // Serialized by vk_pipeline's progress mutex.
    static std::chrono::steady_clock::time_point sLast;
    const auto now = std::chrono::steady_clock::now();
    if (now - sLast < std::chrono::seconds(1)) return;
    sLast = now;
    vkpipecache::saveCosts(s_pipeSaveDev->physicalDevice(), s_pipeCostPath, "compile progress");
}

// #471: bytes pipeline compiles may use right now: the live headroom above a reserve for
// the rest of the system. Windows: min(available RAM, commit still available) -- the
// commit charge is what the driver's compiler exhausted (#458). 0 while the memory
// governor is critical (the scheduler then runs one compile at a time).
// RenderVKPipelineCompileBudgetMB > 0 replaces it with a fixed budget (testing).
// #672: AVAILABLE memory (never total), re-read at most every 50 ms -- each worker asks
// before admitting a compile -- less a reserve for the system and, per driver, a margin
// for what its compiler allocates outside our measurement (AMD proprietary: 2 GB; its
// compiler failed with VK_ERROR_OUT_OF_HOST_MEMORY well below the system's limits).
static std::atomic<U64> s_pipeBudgetForced{0};
static std::atomic<U64> s_pipeDriverMargin{0};
static uint64_t pipelineMemProbe();
static uint64_t pipelineMemBudget()
{
    // #1103: the compile concurrency follows the memory governor while batches run (every
    // worker asks here before each admission): 2 at once while tight, 1 while critical
    // (incl. the #698 shed window), no cap when comfortable. Restored as soon as the
    // governor reports comfortable again (it holds an improvement 2 s before reporting it).
    {
        const LLMemoryGovernor::EState st = LLMemoryGovernor::getState();
        const unsigned pcap = st == LLMemoryGovernor::CRITICAL ? 1u : (st == LLMemoryGovernor::TIGHT ? 2u : 0u);
        vkSetPipelinePressureCap(pcap, LLMemoryGovernor::stateName(st));
    }
    const U64 forced = s_pipeBudgetForced.load(std::memory_order_relaxed);
    if (forced)
    {
        // Simulate a machine with `forced` bytes of headroom: like the live budget, it
        // shrinks as committed memory grows (above the lowest level seen), so in-flight
        // compiles' growth uses it up. A constant here over-admitted: the scheduler
        // discounts in-flight estimates by their measured growth, which is only right when
        // the budget itself has already shrunk by that growth.
        static std::atomic<U64> sLow{UINT64_MAX};
        const U64 now = pipelineMemProbe();
        U64 low = sLow.load(std::memory_order_relaxed);
        while (now < low && !sLow.compare_exchange_weak(low, now)) {}
        low = std::min(low, now);
        const U64 grown = now > low ? now - low : 0;
        return forced > grown ? forced - grown : 0;
    }
    if (LLMemoryGovernor::getState() == LLMemoryGovernor::CRITICAL) return 0;
    // Sampled at most every 50 ms (every waiting worker asks every 50 ms).
    static std::mutex sMutex;
    static std::chrono::steady_clock::time_point sAt;
    static uint64_t sBudget = 0;
    static bool sHave = false;
    std::lock_guard<std::mutex> lk(sMutex);
    const auto now = std::chrono::steady_clock::now();
    if (sHave && now - sAt < std::chrono::milliseconds(50)) return sBudget;
    LLMemoryGovernor::SystemMemory sm;
    if (!LLMemoryGovernor::readSystemMemory(sm) || !sm.avail)
    {
        sBudget = UINT64_MAX;   // can't tell: only the thread count limits
    }
    else
    {
        U64 avail = sm.avail;
        if (sm.have_commit) avail = std::min(avail, sm.commit_avail);
        const U64 reserve = std::max<U64>(2048ull << 20, sm.total / 10)
                          + s_pipeDriverMargin.load(std::memory_order_relaxed);
        sBudget = avail > reserve ? avail - reserve : 0;
    }
    sAt = now;
    sHave = true;
    return sBudget;
}

// static
void LLVulkanContext::setPipelineCompileBudgetMB(uint32_t mb)
{
    s_pipeBudgetForced.store((U64)mb << 20, std::memory_order_relaxed);
}

// #471: the process's committed memory for the per-pipeline compile measurement. On
// Windows the private bytes (the commit charge the driver's compiler exhausted in #458);
// elsewhere the resident set.
static uint64_t pipelineMemProbe()
{
#if LL_WINDOWS
    PROCESS_MEMORY_COUNTERS_EX pmem{};
    pmem.cb = sizeof(pmem);
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmem, sizeof(pmem)))
        return (uint64_t)pmem.PrivateUsage;
    return 0;
#else
    return LLMemory::getCurrentRSS();
#endif
}

// #672: the pipeline-creation recovery path (out-of-memory step-downs, retries, give-ups).
static void pipelineEvent(bool warn, const char* line)
{
    if (warn)
        LL_WARNS("VKPipe") << line << LL_ENDL;
    else
        LL_INFOS("VKPipe") << line << LL_ENDL;
}

// #471: batch reports -> the log (VKPipe) + a one-line snapshot of the heaviest batch.
static std::mutex s_pipeReportMutex;
static std::string s_pipeReportText;
static double s_pipeReportSumMs = -1.0;

// #1103: the driver compilers' memory is freed on the compile threads, into glibc's
// per-thread arenas, which keep it resident (#1059: Arc B580 / Mesa, 16 GB: 8.6-9.9 GB
// resident after a cold compile, then the OOM killer). After a batch -- its workers have
// all been joined when the report hook runs -- and once no other batch is compiling, the
// freed memory goes back to the OS with malloc_trim(0), measured (RSS before / after, the
// arenas' free bytes before it). Small batches (single admitted creates) add up until they
// are worth an arena walk. Off the main thread it runs right here (the batch's own thread,
// before its job reports done), on the main thread (a synchronous flush) on a General worker.
#if LL_LINUX && defined(__GLIBC__)
static std::atomic<bool> s_trimRunning{false};
static std::atomic<unsigned> s_trimDebtCompiles{0};
static std::atomic<U64> s_trimDebtBytes{0};
constexpr unsigned kTrimDebtCompiles = 4;      // real compiles since the last trim ...
constexpr U64 kTrimDebtBytes = 64ull << 20;    // ... or this much batch growth

static std::string heapTrimText(const LLMemoryGovernor::HeapTrim& t)
{
    constexpr U64 MB = 1ull << 20;
    return llformat("rss %llu MB -> %llu MB after malloc_trim (%lld MB, %.0f ms; %llu MB free in %u arenas before it)",
                    (unsigned long long)(t.rss_before / MB), (unsigned long long)(t.rss_after / MB),
                    (long long)(t.rss_after / MB) - (long long)(t.rss_before / MB), t.ms,
                    (unsigned long long)(t.free_before / MB), t.arenas);
}
#endif

// Text for the batch report line ("" when nothing was compiled / not supported).
static std::string pipelineHeapTrim(const VkPipelineBatchReport& r)
{
#if LL_LINUX && defined(__GLIBC__)
    unsigned real = 0;
    for (const VkPipelineCompileStat& s : r.items)
        if (!s.hit && !s.rest && !s.failed) ++real;
    if (real)
    {
        s_trimDebtCompiles.fetch_add(real);
        s_trimDebtBytes.fetch_add(r.peakBytes > r.baseBytes ? r.peakBytes - r.baseBytes : 0);
    }
    const unsigned debtN = s_trimDebtCompiles.load();
    const U64 debtB = s_trimDebtBytes.load();
    if (!debtN) return std::string();   // nothing compiled since the last trim
    if (debtN < kTrimDebtCompiles && debtB < kTrimDebtBytes)
        return llformat("; heap trim deferred (%u compile(s), +%llu MB since the last)", debtN,
                        (unsigned long long)(debtB >> 20));
    if (const unsigned inflight = vkPipelineCompilesInFlight())
        return llformat("; heap trim deferred (%u compiling in another batch)", inflight);
    if (s_trimRunning.exchange(true)) return "; heap trim already running";
    s_trimDebtCompiles.store(0);
    s_trimDebtBytes.store(0);
    if (on_main_thread())
    {
        const char* what = r.what;   // (a static string)
        try
        {
            if (LL::WorkQueue::ptr_t q = LL::WorkQueue::getInstance("General"))
            {
                q->post([what]()
                        {
                            const LLMemoryGovernor::HeapTrim t = LLMemoryGovernor::trimMallocHeap();
                            s_trimRunning.store(false);
                            LL_INFOS("VKPipe") << "pipeline batch (" << what << "): " << heapTrimText(t) << LL_ENDL;
                        });
                return "; heap trim queued (General worker)";
            }
        }
        catch (...)
        {
            // queue closed (shutdown): no trim
        }
        s_trimRunning.store(false);
        return "; heap trim skipped (no General queue)";
    }
    const LLMemoryGovernor::HeapTrim t = LLMemoryGovernor::trimMallocHeap();
    s_trimRunning.store(false);
    return "; " + heapTrimText(t);
#else
    (void)r;
    return std::string();
#endif
}

static void pipelineBatchReport(const VkPipelineBatchReport& r)
{
    constexpr int64_t MB = 1 << 20;
    const int64_t growth = ((int64_t)r.peakBytes - (int64_t)r.baseBytes) / MB;
    const U64 endBytes = pipelineMemProbe();             // #1103: at the batch end, before any trim
    const std::string trimText = pipelineHeapTrim(r);
    LL_INFOS("VKPipe") << "pipeline batch (" << r.what << "): " << r.total << " pipelines, "
                       << r.compiled << " compiled + " << r.fromCache << " dedup twins; wall "
                       << (S64)r.wallMs << " ms, sum of compiles " << (S64)r.sumMs << " ms; "
                       << r.threads << " threads, at most " << r.maxConcurrency << " at once; committed "
                       << (S64)(r.baseBytes / MB) << " MB -> peak " << (S64)(r.peakBytes / MB) << " MB (+"
                       << growth << " MB); budget min "
                       << (r.budgetMin != UINT64_MAX ? std::to_string(r.budgetMin / MB) + " MB" : std::string("unlimited"))
                       << ", " << r.overBudget << " started over budget, " << (S64)r.waitMs
                       << " ms waiting for budget; at the end " << (S64)(endBytes / MB) << " MB"
                       << (r.pressureCapMin ? "; memory-pressure cap down to " + std::to_string(r.pressureCapMin)
                                            : std::string())
                       << trimText << LL_ENDL;
    if (r.oomRetries || r.failed)
    {
        LL_WARNS("VKPipe") << "pipeline batch (" << r.what << "): #672 " << r.oomRetries
                           << " out-of-memory result(s) retried, " << r.failed
                           << " pipeline(s) given up; concurrency cap now " << r.capEnd << LL_ENDL;
        for (const VkPipelineCompileStat& s : r.items)
            if (s.failed)
                LL_WARNS("VKPipe") << "  given up after " << s.attempts << " attempts: " << s.name << LL_ENDL;
    }

    std::vector<const VkPipelineCompileStat*> byTime, byMem;
    for (const VkPipelineCompileStat& s : r.items) { byTime.push_back(&s); byMem.push_back(&s); }
    std::sort(byTime.begin(), byTime.end(),
              [](const VkPipelineCompileStat* a, const VkPipelineCompileStat* b) { return a->ms > b->ms; });
    std::sort(byMem.begin(), byMem.end(),
              [](const VkPipelineCompileStat* a, const VkPipelineCompileStat* b) { return a->attribBytes > b->attribBytes; });
    auto line = [&](const char* what, size_t i, const VkPipelineCompileStat& s)
    {
        LL_INFOS("VKPipe") << llformat("  %s %2d: %8.1f ms %6lld MB (raw %lld MB, est %lld MB) c%u %u words%s%s %s",
                                       what, (int)i + 1, s.ms, (long long)(s.attribBytes / MB),
                                       (long long)(s.rawBytes / MB), (long long)(s.estBytes / MB), s.concurrency,
                                       s.spirvWords, s.hit ? " hit" : "", s.rest ? " twin" : "", s.name.c_str())
                           << LL_ENDL;
    };
    for (size_t i = 0; i < byTime.size() && i < 10; ++i) line("time", i, *byTime[i]);
    if (r.peakBytes)
        for (size_t i = 0; i < byMem.size() && i < 10; ++i) line("mem ", i, *byMem[i]);
    // Snapshot for bug reports: the batch that compiled the most (the world batch).
    std::string text = llformat("%s %d/%d %.1fs +%lldMB t%u c%u ob%u:", r.what, (int)r.total, (int)r.compiled,
                                r.wallMs / 1000.0, (long long)growth, r.threads, r.maxConcurrency, r.overBudget);
    for (size_t i = 0; i < byTime.size() && i < 4; ++i)   // fits the 200-char report field
    {
        const VkPipelineCompileStat& s = *byTime[i];
        std::string name = s.name;
        // "LIT_DRAW (Vert/FragRt)" -> "LIT_DRAW/FragRt" (the frag tells the variants apart)
        const size_t open = name.find(" ("), slash = name.rfind('/');
        if (open != std::string::npos && slash != std::string::npos && slash > open)
            name = name.substr(0, open) + name.substr(slash, name.size() - slash - (name.back() == ')' ? 1 : 0));
        text += llformat(" %s %.1fs %lldM c%u;", name.c_str(), s.ms / 1000.0, (long long)(s.attribBytes / MB),
                         s.concurrency);
    }
    {
        std::lock_guard<std::mutex> lk(s_pipeReportMutex);
        if (r.sumMs >= s_pipeReportSumMs)
        {
            s_pipeReportSumMs = r.sumMs;
            s_pipeReportText = text;
        }
    }
    // Persist what this batch taught the scheduler.
    if (s_pipeSaveDev) vkpipecache::saveCosts(s_pipeSaveDev->physicalDevice(), s_pipeCostPath, "batch done");
}

// static
static std::string s_pipeCapNote;               // #876 (guarded by s_pipeReportMutex)
static std::atomic<bool> s_driverIntelWindows{false};

std::string LLVulkanContext::pipelineCompileText()
{
    std::lock_guard<std::mutex> lk(s_pipeReportMutex);
    if (s_pipeCapNote.empty()) return s_pipeReportText;
    return s_pipeReportText.empty() ? s_pipeCapNote : s_pipeCapNote + "; " + s_pipeReportText;
}

bool LLVulkanContext::driverIsIntelWindows() { return s_driverIntelWindows.load(); }

// #1064 (guarded by s_pipeReportMutex): the driver identity the compile-crash quarantine is keyed by.
static std::string s_driverIdToken;
static std::string s_driverIdLabel;

std::string LLVulkanContext::driverIdentityToken()
{
    std::lock_guard<std::mutex> lk(s_pipeReportMutex);
    return s_driverIdToken;
}

std::string LLVulkanContext::driverIdentityLabel()
{
    std::lock_guard<std::mutex> lk(s_pipeReportMutex);
    return s_driverIdLabel;
}

void LLVulkanContext::setPipelineCompileCapNote(const std::string& note)
{
    std::lock_guard<std::mutex> lk(s_pipeReportMutex);
    s_pipeCapNote = note;
}

namespace { const char* vkPresentModeName(VkPresentModeKHR m); }   // (defined with displayInfoText)

// The swapchain's present mode / image count / frames in flight into the frame timeline
// (LLVKHitch, the HITCH timeline line's header) and, on a recreate that changed one of
// them, one log line: frame-pacing reports need the mode the frames were presented with,
// and vkgfx's own messages go to stderr only. Resizes keep the mode and count, so a
// drag-resize does not spam the log.
static void noteSwapchainMode(const VkSwapchain& sc, bool recreated)
{
    static VkPresentModeKHR s_mode = VK_PRESENT_MODE_MAX_ENUM_KHR;
    static uint32_t s_images = 0, s_in_flight = 0;
    const VkPresentModeKHR mode = sc.presentMode();
    char letter = '?';
    switch (mode)
    {
    case VK_PRESENT_MODE_FIFO_KHR: letter = 'F'; break;
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR: letter = 'R'; break;
    case VK_PRESENT_MODE_MAILBOX_KHR: letter = 'M'; break;
    case VK_PRESENT_MODE_IMMEDIATE_KHR: letter = 'I'; break;
    default: break;
    }
    LLVKHitch::timelineSwapchain(vkPresentModeName(mode), letter, sc.imageCount(), sc.framesInFlight());
    if (recreated && (mode != s_mode || sc.imageCount() != s_images || sc.framesInFlight() != s_in_flight))
    {
        LL_INFOS("Vulkan") << "Swapchain recreated: " << sc.extent().width << "x" << sc.extent().height
                           << ", present mode " << vkPresentModeName(mode) << " (vsync " << (VkSwapchain::sVSync ? "on" : "off")
                           << ", RenderVKNoVsyncPresentMode " << VkSwapchain::sNoVsyncMode << "), " << sc.imageCount()
                           << " images, " << sc.framesInFlight() << " frames in flight" << LL_ENDL;
    }
    s_mode = mode; s_images = sc.imageCount(); s_in_flight = sc.framesInFlight();
}

bool LLVulkanContext::init(SDL_Window* window, uint32_t width, uint32_t height,
                           bool enableValidation)
{
    if (mInitialized)
    {
        return true;
    }
    if (!window)
    {
        LL_WARNS("Vulkan") << "init: null SDL window" << LL_ENDL;
        return false;
    }

    mImpl = new Impl();
    mImpl->mWindow = window;
    mImpl->mWidth  = width;
    mImpl->mHeight = height;

    // 1) Instance extensions SDL needs for a surface on this platform.
    Uint32 sdlExtCount = 0;
    const char* const* sdlExts = SDL_Vulkan_GetInstanceExtensions(&sdlExtCount);
    if (!sdlExts)
    {
        LL_WARNS("Vulkan") << "SDL_Vulkan_GetInstanceExtensions failed: "
                           << SDL_GetError() << LL_ENDL;
        delete mImpl; mImpl = nullptr;
        return false;
    }
    std::vector<const char*> instExts(sdlExts, sdlExts + sdlExtCount);
    for (Uint32 i = 0; i < sdlExtCount; ++i)
    {
        LL_INFOS("Vulkan") << "SDL requires instance extension: " << sdlExts[i]
                           << LL_ENDL;
    }

#if LL_WINDOWS
    // #919 full-screen exclusive: VK_EXT_full_screen_exclusive (device) needs
    // VK_KHR_get_surface_capabilities2 (instance). Both optional: requested only where the
    // loader lists the instance extension; the device enables the other where listed.
    {
        uint32_t n = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> props(n);
        if (n) vkEnumerateInstanceExtensionProperties(nullptr, &n, props.data());
        bool caps2 = false;
        for (uint32_t i = 0; i < n; ++i)
            if (std::strcmp(props[i].extensionName, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME) == 0) caps2 = true;
        if (caps2)
        {
            bool dup = false;
            for (const char* e : instExts)
                if (std::strcmp(e, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME) == 0) dup = true;
            if (!dup) instExts.push_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
            mImpl->mDevice.requestOptionalExtension(VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME);
        }
        else
        {
            LL_INFOS("Vulkan") << "Fullscreen: VK_KHR_get_surface_capabilities2 not available: no exclusive fullscreen" << LL_ENDL;
        }
    }
#endif

    // #870: the optional neural reconstruction library (DLSS RR via the user-installed slip_nr
    // shim) is loaded BEFORE the instance: its instance extensions (only those the loader
    // lists; all or nothing) and, after the GPU pick, its device extensions (NVIDIA only, the
    // late provider). Missing / failing = RR unavailable with a reason, never fatal.
    {
        LLVKNeuralRecon& nr = LLVKNeuralRecon::instance();
        LLVKNeuralRecon::Config nc;
        nc.enabled = mNrEnabled;
        nc.configuredPath = mNrShimPath;
        nc.viewerLibDir = mNrLibDir;
        nc.defaultDir = mNrDefaultDir;
        nc.dataDir = mNrDataDir;
        nc.engineVersion = mNrEngineVersion;
        if (nr.load(nc))
        {
            uint32_t n = 0;
            vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
            std::vector<VkExtensionProperties> props(n);
            if (n) vkEnumerateInstanceExtensionProperties(nullptr, &n, props.data());
            std::string missing;
            for (const std::string& want : nr.instanceExtensions())
            {
                bool listed = false;
                for (uint32_t i = 0; i < n; ++i)
                    if (want == props[i].extensionName) listed = true;
                if (!listed) missing += (missing.empty() ? "" : ", ") + want;
            }
            if (!missing.empty())
                nr.instanceExtensionsMissing(missing);
            else
                for (const std::string& want : nr.instanceExtensions())
                {
                    bool dup = false;
                    for (const char* e : instExts)
                        if (want == e) dup = true;
                    if (!dup) instExts.push_back(want.c_str());   // (the strings live in the singleton)
                }
            mImpl->mDevice.setLateExtensionProvider(
                [](VkInstance inst, VkPhysicalDevice pd, std::vector<std::string>& exts, std::string& why)
                { return LLVKNeuralRecon::instance().deviceExtensions(inst, pd, exts, why); });
        }
    }

    // #1073 VR (startup only): the OpenXR probe succeeded, so the runtime creates the instance
    // and the device from the same requests (XR_KHR_vulkan_enable2) on the GPU it requires. On
    // the desktop LLXrSession::active() is false: no hook, creation exactly as without VR.
    if (LLXrSession::active())
    {
        LLXrSession::instance().installDeviceHooks(mImpl->mDevice);
        mXrHooked = true;
    }

    // 2) Instance + device (with VK_KHR_swapchain). RT extensions only when requested.
    mImpl->mDevice.setRayTracingRequested(mRayTracingRequested);
    mImpl->mDevice.setClusterRequested(mClusterRequested);
    mImpl->mDevice.setPipelineStatsRequested(mPipelineStatsRequested);
    mImpl->mDevice.setShaderClockRequested(mShaderClockRequested);
    mImpl->mDevice.setClusterDeviceFeatures(mClusterDeviceFeatures);
    mImpl->mDevice.setAsyncComputeMode(mAsyncComputeMode);
    mImpl->mDevice.setAsyncComputeAuto(mAsyncComputeAuto);
    mImpl->mDevice.setDebugRobustness(mDebugRobustness);
    mImpl->mDevice.setPipelineCacheFile(mPipeCachePath, mPipeCacheTag);
    // #526: the requirements report (every device, what it lacks, the choice) goes to the
    // log; on failure it also becomes the message box text (shutdown() clears it).
    auto failWith = [this](const char* step, const std::string& why)
    {
        VkRequirementsReport r = mImpl->mDevice.requirementsReport();
        if (!why.empty()) r.failure = why;
        LL_WARNS("Vulkan") << step << " failed: " << (r.failure.empty() ? std::string("?") : r.failure) << LL_ENDL;
        mInitFailureMessage = vkRequirementsUserMessage(r);
        shutdown();
        return false;
    };
    const bool devOk = mImpl->mDevice.initForPresent(enableValidation, instExts, /*wantSwapchain=*/true);
    for (const std::string& l : vkRequirementsLogLines(mImpl->mDevice.requirementsReport()))
    {
        if (devOk)
            LL_INFOS("Vulkan") << l << LL_ENDL;
        else
            LL_WARNS("Vulkan") << l << LL_ENDL;
    }
    if (!devOk)
    {
        return failWith("VkDeviceContext::initForPresent", std::string());
    }
    // #870 / #962: NGX init + per-feature capability query (logs the one "DLSS:" status line).
    if (LLVKNeuralRecon::instance().loaded())
        LLVKNeuralRecon::instance().initDevice(mImpl->mDevice.instance(), mImpl->mDevice.physicalDevice(),
                                               mImpl->mDevice.device(), mImpl->mDevice.lateExtensionsEnabled(),
                                               mImpl->mDevice.lateExtensionsWhy());
    // #1073 VR: the session on this device (graphics queue, index 0). A refusal leaves the
    // desktop running on the device (LLXrSession logs why; the viewer shows the notice).
    if (mXrHooked && LLXrSession::active())
    {
        LLXrSession& xr = LLXrSession::instance();
        if (xr.createSession(mImpl->mDevice))
        {
            mXrSession = true;
            xr.setRunningCallback([this](bool running) { onXrRunningChanged(running); });
            LL_INFOS("VR") << "device created by the OpenXR runtime; multiview "
                           << (mImpl->mDevice.multiviewEnabled() ? "enabled" : "not supported") << LL_ENDL;
        }
    }
    // Device limits for every vkdCreatePipelineLayout check (UI, RT, world), logged once
    // incl. maxPushConstantsSize (#238) -- before the first pipeline layout exists.
    vkdInitDeviceLimits(mImpl->mDevice.physicalDevice());
    // #695: the memory layout and the VRAM budget mode chosen (UMA carve-out + shared RAM,
    // or the main device-local heap) -- what a user report needs to tell what happened.
    LL_INFOS("VKMem") << "Memory layout: " << mImpl->mDevice.memLayoutText() << LL_ENDL;

    // #462 / #471: how pipelines compile. AMD's Windows compiler can take GBs per large
    // world-FS variant; (hardware threads - 1) parallel compiles -- 23 on a 12-core Ryzen --
    // ran a 32 GB machine out of commit at the login screen. The batch scheduler
    // (vk_pipeline.h) admits compiles against the LIVE memory headroom (pipelineMemBudget)
    // using each pipeline's learned cost, or a per-driver default for one never measured:
    // 3 GB on AMD's proprietary driver (conservative until learned), 512 MB elsewhere.
    // Threads: hardware threads - 1, hard-capped by RenderVKPipelineCompileThreads.
    {
        VkPhysicalDeviceDriverProperties drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &drv;
        vkGetPhysicalDeviceProperties2(mImpl->mDevice.physicalDevice(), &p2);
        const bool amdProprietary = (drv.driverID == VK_DRIVER_ID_AMD_PROPRIETARY);
        s_driverIntelWindows = (drv.driverID == VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS);   // #876 (the cap is decided in llappviewer)
        {
            // #1064: "<vendorID>-<driverID>-<driverVersion>" (hex, decimal, hex): no spaces, so the
            // crash handler writes it verbatim and the marker / quarantine files compare it as text.
            char tok[64];
            std::snprintf(tok, sizeof(tok), "%04x-%u-%08x", (unsigned)p2.properties.vendorID, (unsigned)drv.driverID,
                          (unsigned)p2.properties.driverVersion);
            std::string label = drv.driverName;
            if (drv.driverInfo[0]) label += std::string(" ") + drv.driverInfo;
            std::lock_guard<std::mutex> lk(s_pipeReportMutex);
            s_driverIdToken = tok;
            s_driverIdLabel = label;
        }
        U64 defaultCost = amdProprietary ? (3072ull << 20) : (512ull << 20);
        vkSetPipelineCompileThreads(0);   // auto; RenderVKPipelineCompileThreads (llappviewer) may cap it
        const bool warm = mImpl->mDevice.pipelineCacheWarm();
        vkSetPipelineCacheWarm(warm);
        s_pipeCostPath = mPipeCostPath;
        const size_t learned = vkpipecache::loadCosts(mImpl->mDevice.physicalDevice(), s_pipeCostPath);
        // #1103: Mesa's compilers (ANV / RADV / NVK) measured heavier than the 512 MB default
        // on the reporter's machine. A pipeline never measured on this device is assumed to
        // cost what 90% of the measured ones stay under, when that is more (at most 2 GB).
        const bool mesa = drv.driverID == VK_DRIVER_ID_INTEL_OPEN_SOURCE_MESA || drv.driverID == VK_DRIVER_ID_MESA_RADV
                       || drv.driverID == VK_DRIVER_ID_MESA_NVK || drv.driverID == VK_DRIVER_ID_MESA_LLVMPIPE;
        const U64 learnedP90 = vkPipelineLearnedColdBytes(90);
        if (mesa && learnedP90 > defaultCost) defaultCost = std::min<U64>(learnedP90, 2048ull << 20);
        vkSetPipelineDefaultCost(defaultCost);
        const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
        // #672: AMD's proprietary compiler ran out of host memory mid cold-compile (#667 /
        // #669: every launch crashed, 11 workers, budget ~25 GB on 32 GB). A margin on top of
        // the system reserve, and on a COLD cache a low starting concurrency (2 with nothing
        // learned, 4 with learned costs) that ramps up as the measured costs show headroom.
        s_pipeDriverMargin.store(amdProprietary ? (2048ull << 20) : 0, std::memory_order_relaxed);
        const unsigned startCap = (amdProprietary && !warm) ? (learned ? 4u : 2u) : 0u;
        vkSetPipelineConcurrencyStart(startCap);
        const U64 budget = pipelineMemBudget();
        // Compiles in flight may hold at most a quarter of physical RAM, whatever the live
        // headroom (vkSetPipelineMemCap): the headroom alone let them take nearly all of it.
        U64 cap = 0;
        {
            LLMemoryGovernor::SystemMemory sm;
            if (LLMemoryGovernor::readSystemMemory(sm) && sm.total) cap = sm.total / 4;
        }
        vkSetPipelineMemCap(cap);
        LL_INFOS("Vulkan") << "Pipeline compiles: memory-budgeted, up to " << std::max(1u, hw > 2 ? hw - 1 : hw)
                           << " at once (driver " << drv.driverName << (amdProprietary ? ", AMD proprietary" : "")
                           << "; " << (defaultCost >> 20) << " MB assumed per unknown pipeline"
                           << (mesa ? " (Mesa: at least the learned 90th percentile)" : "") << ", " << learned
                           << " learned (90th percentile " << (learnedP90 >> 20) << " MB); compile budget now "
                           << (budget == UINT64_MAX ? std::string("unlimited") : std::to_string(budget >> 20) + " MB")
                           << " (available memory, less the reserve"
                           << (amdProprietary ? " + 2048 MB driver margin" : "") << ")"
                           << "; pipeline cache " << (warm ? "warm" : "cold")
                           << "; compiles in flight capped at "
                           << (cap ? std::to_string(cap >> 20) + " MB (25% of RAM)" : std::string("no cap (RAM unknown)"))
                           << "; memory pressure caps concurrency at 2 (tight) / 1 (critical)"
                           << "; malloc arenas " << LLMemoryGovernor::mallocArenaText() << ")" << LL_ENDL;
        if (startCap)
            LL_INFOS("VKPipe") << "#672: cold pipeline cache on AMD's proprietary driver: starting at " << startCap
                               << " compiles at once, ramping up as measured costs show headroom" << LL_ENDL;
        // #672: a cold start saves the cache (+ learned costs) every 4 compiles or 5 s, so a
        // crash in the driver keeps the progress and the next launch is not cold again.
        vkSetPipelineProgressInterval(warm ? 0u : 5000u);
        s_pipeProgressEvery = warm ? 8u : 4u;
    }
    s_pipeSaveDev = &mImpl->mDevice;
    vkSetPipelineProgressHook(&pipelineProgressSave, s_pipeProgressEvery);
    vkSetPipelineMemProbe(&pipelineMemProbe);         // #471 per-pipeline measurement
    vkSetPipelineBatchHook(&pipelineBatchReport);
    vkSetPipelineMemBudget(&pipelineMemBudget);       // #471 memory-aware scheduling
    vkSetPipelineEventHook(&pipelineEvent);           // #672 out-of-memory recovery log
    // RenderVKPipelineStats: every pipeline from here on logs its executables' statistics
    // ("VKPipeStats: <name> (<vert module>/<frag module>) [stage ...]; stat=value; ...").
    if (mPipelineStatsRequested)
    {
        if (mImpl->mDevice.pipelineStatsEnabled())
            LL_INFOS("VKPipe") << "pipeline statistics ON (RenderVKPipelineStats; VK_KHR_pipeline_executable_properties): "
                               << mImpl->mDevice.shaderCoreSummary() << LL_ENDL;
        else
            LL_WARNS("VKPipe") << "RenderVKPipelineStats: VK_KHR_pipeline_executable_properties (pipelineExecutableInfo)"
                                  " not available on this device: no pipeline statistics" << LL_ENDL;
    }

    // 3) Surface from the SDL_WINDOW_VULKAN window.
    if (!SDL_Vulkan_CreateSurface(window, mImpl->mDevice.instance(), nullptr,
                                  &mImpl->mSurface))
    {
        return failWith("SDL_Vulkan_CreateSurface", std::string("window surface: ") + SDL_GetError());
    }

#if LL_WINDOWS
    // #919 full-screen exclusive entry points (all four, or none of it).
    if (mImpl->mDevice.optionalExtensionEnabled(VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME))
    {
        Impl& m = *mImpl;
        m.mFseCaps2 = (PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR)
            vkGetInstanceProcAddr(m.mDevice.instance(), "vkGetPhysicalDeviceSurfaceCapabilities2KHR");
        m.mFseModes2 = (PFN_vkGetPhysicalDeviceSurfacePresentModes2EXT)
            vkGetInstanceProcAddr(m.mDevice.instance(), "vkGetPhysicalDeviceSurfacePresentModes2EXT");
        m.mFseAcquire = (PFN_vkAcquireFullScreenExclusiveModeEXT)
            vkGetDeviceProcAddr(m.mDevice.device(), "vkAcquireFullScreenExclusiveModeEXT");
        m.mFseRelease = (PFN_vkReleaseFullScreenExclusiveModeEXT)
            vkGetDeviceProcAddr(m.mDevice.device(), "vkReleaseFullScreenExclusiveModeEXT");
        m.mFseExt = m.mFseCaps2 && m.mFseAcquire && m.mFseRelease;
        Impl* impl = mImpl;
        m.mSwapchain.setPresentModeQuery([impl](std::vector<VkPresentModeKHR>& out) { return impl->fsePresentModes(out); });
        LL_INFOS("Vulkan") << "Fullscreen: VK_EXT_full_screen_exclusive "
                           << (m.mFseExt ? "enabled" : "enabled but its entry points are missing: not used")
                           << "; exclusive on this monitor: " << (m.mFseExt && m.fseSurfaceSupported() ? "supported" : "not supported")
                           << LL_ENDL;
    }
    else
    {
        LL_INFOS("Vulkan") << "Fullscreen: VK_EXT_full_screen_exclusive not available on this device" << LL_ENDL;
    }
#endif

    // 4) Swapchain.
    if (!mImpl->mSwapchain.init(mImpl->mDevice, mImpl->mSurface, width, height))
    {
        return failWith("swapchain init", "swapchain: the chosen GPU can't present to this window "
                                          "(no present support or zero extent)");
    }

    // 5) One primary command buffer per frame-in-flight slot (see mCmds comment).
    const uint32_t framesInFlight = mImpl->mSwapchain.framesInFlight();
    mImpl->mCmds.resize(framesInFlight, VK_NULL_HANDLE);
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = mImpl->mDevice.commandPool();
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = framesInFlight;
    VK_CHECK(vkAllocateCommandBuffers(mImpl->mDevice.device(), &ai,
                                      mImpl->mCmds.data()));

    mInitialized = true;
    LL_INFOS("Vulkan") << "RT: requested=" << mRayTracingRequested
                       << " supported=" << mImpl->mDevice.rayQuerySupported()
                       << " enabled=" << mImpl->mDevice.rayTracingEnabled()
                       << " (" << mImpl->mDevice.rayTracingStatus() << ")" << LL_ENDL;
    // #743: one line, every cluster LOD capability and the tier it gets on this device.
    LL_INFOS("Vulkan") << LLCleanClusterCaps::detect(mImpl->mDevice, mClusterForceTier).logLine() << LL_ENDL;
    // One line: how the async queue was decided (RenderVKAsyncCompute -1 = per-GPU auto).
    LL_INFOS("Vulkan") << "VKAsync: " << mImpl->mDevice.asyncComputeDecision() << LL_ENDL;
    LL_INFOS("Vulkan") << "Async compute queue (RenderVKAsyncComputeQueue=" << mAsyncComputeMode << "): "
                       << mImpl->mDevice.asyncComputeStatus() << LL_ENDL;
    LL_INFOS("Vulkan") << "Texture pools (RenderVKTextureSuballoc, #411): " << mImpl->mDevice.texturePoolStatus() << LL_ENDL;
    LL_INFOS("Vulkan") << "Vulkan context initialized: swapchain "
                       << mImpl->mSwapchain.extent().width << "x"
                       << mImpl->mSwapchain.extent().height << ", "
                       << mImpl->mSwapchain.imageCount() << " images, format "
                       << (int)mImpl->mSwapchain.format() << ", present mode "
                       << vkPresentModeName(mImpl->mSwapchain.presentMode()) << ", "
                       << mImpl->mSwapchain.framesInFlight() << " frames in flight" << LL_ENDL;
    noteSwapchainMode(mImpl->mSwapchain, false);
    return true;
}

void LLVulkanContext::setVSync(bool on)
{
    mUserVSync = on;
    if (mXrPresentOverride) return;   // #1073: a VR session runs; applied when it stops
    if (VkSwapchain::sVSync == on) return;
    VkSwapchain::sVSync = on;
    if (mImpl) mImpl->mNeedsRecreate = true;
}

// #1073 VR: while the session runs, xrWaitFrame paces the viewer and the window is a mirror: a
// FIFO present would cap the headset at the monitor's rate, so the window goes MAILBOX /
// IMMEDIATE (RenderVKNoVsyncPresentMode). The user's vsync comes back when the session stops.
void LLVulkanContext::onXrRunningChanged(bool running)
{
    mXrPresentOverride = running;
    const bool want = running ? false : mUserVSync;
    LL_INFOS("VR") << (running ? "session running: the window presents without vsync (it must never block the headset)"
                               : "session stopped: the window presents with the user's vsync setting again")
                   << LL_ENDL;
    if (mImpl)
    {
        // #1074: nor may its acquire: a short timeout skips the window's frame instead.
        mImpl->mSwapchain.setAcquireTimeout(running ? 2000000ull : UINT64_MAX);
        // The eye-size world target is only used while the session runs (called between
        // frames: waits for the frames in flight before freeing it).
        if (!running) mImpl->vrReleaseTarget(true);
    }
    if (VkSwapchain::sVSync == want) return;
    VkSwapchain::sVSync = want;
    if (mImpl) mImpl->mNeedsRecreate = true;
}

void LLVulkanContext::setNoVsyncPresentMode(uint32_t mode)
{
    mode = mode == 1 ? 1u : 0u;
    if (VkSwapchain::sNoVsyncMode == mode) return;
    VkSwapchain::sNoVsyncMode = mode;
    if (mImpl) mImpl->mNeedsRecreate = true;
}

// ---------------------------------------------------------------------------
// #919 full-screen exclusive (Windows, VK_EXT_full_screen_exclusive, APPLICATION_CONTROLLED)
// ---------------------------------------------------------------------------
// How long acquiring the mode is retried (each frame) after a swapchain creation or a loss
// before presentation falls back to composited until the next focus gain. The window's
// fullscreen transition is asynchronous, so the first attempts can fail legitimately.
#if LL_WINDOWS
static const double kFseAcquireWindowSec = 2.0;
#endif

bool LLVulkanContext::fullScreenExclusiveAvailable()
{
#if LL_WINDOWS
    return mInitialized && mImpl && mImpl->mFseExt && mImpl->fseSurfaceSupported();
#else
    return false;
#endif
}

void LLVulkanContext::setFullScreenExclusive(bool want)
{
    if (!mImpl) return;
#if LL_WINDOWS
    if (want && !mImpl->mFseExt)
    {
        LL_INFOS("Vulkan") << "Fullscreen: exclusive presentation requested, but VK_EXT_full_screen_exclusive is not available" << LL_ENDL;
        want = false;
    }
    if (mImpl->mFseWanted == want) return;
    mImpl->mFseWanted = want;
    mImpl->mFseSuspended = false;
    LL_INFOS("Vulkan") << "Fullscreen: exclusive presentation " << (want ? "armed" : "disarmed") << LL_ENDL;
#else
    (void)want;
#endif
}

bool LLVulkanContext::fullScreenExclusiveArmed() const
{
    return mImpl && mImpl->mFseWanted;
}

bool LLVulkanContext::fullScreenExclusiveHeld() const
{
    return mImpl && mImpl->mFseHeld;
}

void LLVulkanContext::onWindowFocusChanged(bool focused)
{
    if (!mImpl) return;
    if (focused)
    {
        mImpl->mFseSuspended = false;   // a new chance after a failed acquire / refused chain
    }
    mImpl->mFseFocused = focused;
}

void LLVulkanContext::onWindowMinimized(bool minimized)
{
    if (!mImpl) return;
    mImpl->mFseMinimized = minimized;
}

// Before the swapchain recreate in renderFrame: decides whether the swapchain must carry the
// exclusive chain, and releases the mode before the swapchain it belongs to goes away.
void LLVulkanContext::fseBeforeRecreate()
{
#if LL_WINDOWS
    Impl& m = *mImpl;
    if (!m.mFseExt) return;
    if (m.mSwapchain.takeExclusiveLost())
    {
        const bool was_held = m.mFseHeld;
        m.mFseHeld = false;   // (lost: nothing to release)
        if (was_held)
        {
            LL_INFOS("Vulkan") << "Fullscreen: VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT (the mode was taken away); re-acquiring" << LL_ENDL;
        }
        if (!m.mFseTrying)
        {
            m.mFseTrying = true;
            m.mFseTryStart = std::chrono::steady_clock::now();
        }
    }
    bool active = m.mFseWanted && m.mFseFocused && !m.mFseMinimized && !m.mFseSuspended;
    if (active && !m.mFseChained && !m.fseSurfaceSupported())
    {
        LL_INFOS("Vulkan") << "Fullscreen: exclusive mode not supported on this monitor; composited presentation" << LL_ENDL;
        m.mFseSuspended = true;
        active = false;
    }
    if (active != m.mFseChained)
    {
        m.fseRelease(active ? "rebuild" : (!m.mFseWanted ? "left exclusive fullscreen"
                                          : m.mFseMinimized ? "minimized"
                                          : !m.mFseFocused ? "focus lost" : "fallback"));
        if (active)
        {
            m.fseRefreshMonitor();
        }
        m.mSwapchain.setCreateChain(active ? static_cast<const void*>(&m.mFseInfo) : nullptr);
        m.mFseChained = active;
        m.mFseTrying = active;
        m.mFseTryStart = std::chrono::steady_clock::now();
        m.mNeedsRecreate = true;
        LL_INFOS("Vulkan") << "Fullscreen: swapchain rebuilt " << (active ? "WITH" : "without")
                           << " the exclusive chain" << LL_ENDL;
    }
    else if (m.mNeedsRecreate && m.mFseHeld)
    {
        // A resize / present-mode change rebuilds the exclusive swapchain: release first,
        // acquire again on the new one.
        m.fseRelease("swapchain recreate");
        m.mFseTrying = true;
        m.mFseTryStart = std::chrono::steady_clock::now();
    }
#endif
}

// After the recreate: acquire the mode on a swapchain that carries the chain.
void LLVulkanContext::fseAfterRecreate()
{
#if LL_WINDOWS
    Impl& m = *mImpl;
    if (!m.mFseExt || !m.mFseChained || m.mFseHeld || !m.mSwapchain.valid() || m.mNeedsRecreate) return;
    if (m.mSwapchain.chainRejected())
    {
        LL_WARNS("Vulkan") << "Fullscreen: the driver refused the exclusive swapchain; composited presentation "
                              "until the window regains focus" << LL_ENDL;
        m.mFseSuspended = true;   // fseBeforeRecreate drops the chain next frame
        m.mFseTrying = false;
        return;
    }
    if (!m.mFseTrying)
    {
        m.mFseTrying = true;
        m.mFseTryStart = std::chrono::steady_clock::now();
    }
    const VkResult r = m.mFseAcquire(m.mDevice.device(), m.mSwapchain.handle());
    if (r == VK_SUCCESS)
    {
        m.mFseHeld = true;
        m.mFseTrying = false;
        LL_INFOS("Vulkan") << "Fullscreen: exclusive mode acquired (VK_EXT_full_screen_exclusive, "
                           << m.mSwapchain.extent().width << "x" << m.mSwapchain.extent().height << ", present mode "
                           << vkPresentModeName(m.mSwapchain.presentMode()) << ")" << LL_ENDL;
        return;
    }
    const double tried = std::chrono::duration<double>(std::chrono::steady_clock::now() - m.mFseTryStart).count();
    if (tried >= kFseAcquireWindowSec)
    {
        LL_WARNS("Vulkan") << "Fullscreen: vkAcquireFullScreenExclusiveModeEXT = " << vkResultString(r) << " for "
                           << tried << " s; composited presentation until the window regains focus" << LL_ENDL;
        m.mFseSuspended = true;   // fseBeforeRecreate rebuilds without the chain next frame
        m.mFseTrying = false;
    }
#endif
}

void LLVulkanContext::waitFrameSlot()
{
    if (!mInitialized || !mImpl || vkDeviceIsLost()) return;   // (#135)
    const auto t0 = std::chrono::steady_clock::now();
    {
        VK_HITCH_SCOPE("vk_paceWait");
        mImpl->mSwapchain.waitFrameSlot(mImpl->mDevice);
    }
    LLVKHitch::timelinePace(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
}

void LLVulkanContext::onResize(uint32_t width, uint32_t height)
{
    if (!mImpl) return;
    mImpl->mWidth = width;
    mImpl->mHeight = height;
    mImpl->mNeedsRecreate = true;
}

bool LLVulkanContext::runWorldSynthTest(const std::string& png_path,
                                        const LLVKWorldFrame& cam,
                                        uint32_t width, uint32_t height)
{
    if (vkDeviceIsLost()) return false;   // #135
    if (!mInitialized || !mImpl)
    {
        LL_WARNS("Vulkan") << "runWorldSynthTest: context not initialized" << LL_ENDL;
        return false;
    }

    // Offscreen RGBA8 color target (TRANSFER_SRC-capable) so we can copy it back
    // to host memory + PNG. Depth is owned by the world renderer itself.
    OffscreenTarget off;
    off.init(mImpl->mDevice, width, height, /*withDepth=*/false);

    // A dedicated world renderer instance: init() calls fillFromCleanStores(),
    // which mirrors the (already-injected) clean-store cube; syncFromStores()
    // rebuilds the VK buffers if anything changed. Isolated from the live
    // swapchain renderer so this proof never disturbs the present path.
    LLCleanRendererVK* w = new LLCleanRendererVK();
    if (!w->init(mImpl->mDevice, off.colorFormat()))
    {
        LL_WARNS("Vulkan") << "runWorldSynthTest: world renderer init failed" << LL_ENDL;
        delete w;
        off.destroy(mImpl->mDevice);
        return false;
    }
    w->pollPipelines(true);
    w->syncFromStores();

    // Camera + lighting push blocks (same packing as renderFrame's world path).
    VKDrawPush draw{};
    std::memcpy(draw.view_proj, cam.view_proj, sizeof(float)*16);
    std::memcpy(draw.sun_dir, cam.sun_dir, sizeof(float)*4);
    std::memcpy(draw.sun_color, cam.sun_color, sizeof(float)*4);
    std::memcpy(draw.ambient_color, cam.ambient_color, sizeof(float)*4);
    std::memcpy(draw.cam_time, cam.cam_pos, sizeof(float)*3);
    draw.cam_time[3] = cam.time;
    std::memcpy(draw.sky_horizon, cam.sky_horizon, sizeof(float)*4);
    std::memcpy(draw.sky_zenith,  cam.sky_zenith,  sizeof(float)*4);
    std::memcpy(draw.inv_view_proj, cam.inv_view_proj, sizeof(float)*16);
    std::memcpy(draw.water_fog, cam.water_fog, sizeof(float)*4);
    VKCullPush cull{};
    std::memcpy(cull.frustum_planes, cam.frustum_planes, sizeof(cull.frustum_planes));
    std::memcpy(cull.cam_pos, cam.cam_pos, sizeof(float)*4);
    std::memcpy(cull.lod_cam_pos, cam.lod_cam_pos, sizeof(float)*4);
    VKSkyPush sky{};
    std::memcpy(sky.horizon, cam.sky_horizon, sizeof(float)*4);
    std::memcpy(sky.zenith, cam.sky_zenith, sizeof(float)*4);
    std::memcpy(sky.inv_view_proj, cam.inv_view_proj, sizeof(float)*16);
    std::memcpy(sky.cam, cam.cam_pos, sizeof(float)*3); sky.cam[3] = 1.f;

    // Record the whole world pass into one immediate submit: transition the
    // offscreen color UNDEFINED -> COLOR_ATTACHMENT_OPTIMAL (recordWorld LOADs
    // it), then draw. recordWorld leaves it in COLOR_ATTACHMENT_OPTIMAL, which is
    // exactly the layout readbackToPng expects for its TRANSFER_SRC transition.
    VkImage colorImg = off.color().image;
    VkImageView colorView = off.color().view;
    mImpl->mDevice.submitImmediate([&](VkCommandBuffer cmd)
    {
        VkImageMemoryBarrier2 toColor{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        toColor.srcStageMask  = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        toColor.dstStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        toColor.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        toColor.oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
        toColor.newLayout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        toColor.image         = colorImg;
        toColor.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1; dep.pImageMemoryBarriers = &toColor;
        vkCmdPipelineBarrier2(cmd, &dep);

        w->setFrameSlot(0);   // offscreen immediate submit is fully serialized
        w->recordWorld(cmd, colorView, width, height, draw, cull, sky);
    });

    const bool ok = off.readbackToPng(mImpl->mDevice, png_path);

    w->shutdown();
    delete w;
    off.destroy(mImpl->mDevice);

    LL_INFOS("Vulkan") << "runWorldSynthTest: png=" << png_path
                       << " ok=" << (ok?1:0) << LL_ENDL;
    return ok;
}

float LLVulkanContext::getMaxSamplerAnisotropy() const
{
    if (!mInitialized || !mImpl) return 0.f;
    VkPhysicalDevice phys = mImpl->mDevice.physicalDevice();
    if (phys == VK_NULL_HANDLE) return 0.f;
    VkPhysicalDeviceFeatures f{};
    vkGetPhysicalDeviceFeatures(phys, &f);
    if (!f.samplerAnisotropy) return 0.f;
    VkPhysicalDeviceProperties p{};
    vkGetPhysicalDeviceProperties(phys, &p);
    return p.limits.maxSamplerAnisotropy;
}

bool LLVulkanContext::isIntegratedGPU() const
{
    if (!mInitialized || !mImpl) return false;
    VkPhysicalDevice phys = mImpl->mDevice.physicalDevice();
    if (phys == VK_NULL_HANDLE) return false;
    VkPhysicalDeviceProperties p{};
    vkGetPhysicalDeviceProperties(phys, &p);
    return p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU
        || p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
}

bool LLVulkanContext::supportsHardwareRayTracing() const
{
    if (!mInitialized || !mImpl) return false;
    VkPhysicalDevice phys = mImpl->mDevice.physicalDevice();
    if (phys == VK_NULL_HANDLE) return false;
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(phys, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> exts(n);
    vkEnumerateDeviceExtensionProperties(phys, nullptr, &n, exts.data());
    bool accel = false, query = false;
    for (const VkExtensionProperties& e : exts)
    {
        if (std::strcmp(e.extensionName, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) == 0) accel = true;
        if (std::strcmp(e.extensionName, VK_KHR_RAY_QUERY_EXTENSION_NAME) == 0) query = true;
    }
    return accel && query;
}

void LLVulkanContext::getDeviceIdentity(std::string& deviceName,
                                        std::string& vendorShort,
                                        std::string& apiVersion,
                                        bool& isNVIDIA, bool& isAMD, bool& isIntel,
                                        uint32_t& vramMB) const
{
    if (!mInitialized || !mImpl)
    {
        return; // leave outputs untouched; caller keeps its placeholders
    }
    VkPhysicalDevice phys = mImpl->mDevice.physicalDevice();
    if (phys == VK_NULL_HANDLE)
    {
        return;
    }

    VkPhysicalDeviceProperties p{};
    vkGetPhysicalDeviceProperties(phys, &p);

    deviceName = p.deviceName; // e.g. "NVIDIA GeForce RTX 5090"

    // PCI vendor IDs (Khronos-registered).
    switch (p.vendorID)
    {
        case 0x10DE: vendorShort = "NVIDIA"; isNVIDIA = true; break;
        case 0x1002:
        case 0x1022: vendorShort = "ATI";    isAMD    = true; break; // is_ati field name is historical
        case 0x8086: vendorShort = "Intel";  isIntel  = true; break;
        default:     vendorShort = "Vulkan"; break;
    }

    char ver[64];
    std::snprintf(ver, sizeof(ver), "Vulkan %u.%u.%u",
                  VK_VERSION_MAJOR(p.apiVersion),
                  VK_VERSION_MINOR(p.apiVersion),
                  VK_VERSION_PATCH(p.apiVersion));
    apiVersion = ver;

    // VRAM: largest DEVICE_LOCAL heap, in MB.
    VkPhysicalDeviceMemoryProperties mem{};
    vkGetPhysicalDeviceMemoryProperties(phys, &mem);
    VkDeviceSize largest = 0;
    for (uint32_t i = 0; i < mem.memoryHeapCount; ++i)
    {
        if ((mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) &&
            mem.memoryHeaps[i].size > largest)
        {
            largest = mem.memoryHeaps[i].size;
        }
    }
    vramMB = (uint32_t)(largest / (1024ull * 1024ull));
}

// driverVersion is vendor-encoded: NVIDIA 10.8.8.6 bits, Intel on Windows 18.14, else the
// Vulkan packing.
static std::string decode_driver_version(const VkPhysicalDeviceProperties& p)
{
    const uint32_t v = p.driverVersion;
    char ver[64];
    if (p.vendorID == 0x10DE)
        std::snprintf(ver, sizeof(ver), "%u.%u.%u.%u", (v >> 22) & 0x3ffu, (v >> 14) & 0xffu, (v >> 6) & 0xffu, v & 0x3fu);
#if LL_WINDOWS
    else if (p.vendorID == 0x8086)
        std::snprintf(ver, sizeof(ver), "%u.%u", v >> 14, v & 0x3fffu);
#endif
    else
        std::snprintf(ver, sizeof(ver), "%u.%u.%u", VK_VERSION_MAJOR(v), VK_VERSION_MINOR(v), VK_VERSION_PATCH(v));
    return ver;
}

std::string LLVulkanContext::driverVersionText() const
{
    if (!mInitialized || !mImpl) return std::string();
    VkPhysicalDevice phys = mImpl->mDevice.physicalDevice();
    if (phys == VK_NULL_HANDLE) return std::string();
    VkPhysicalDeviceDriverProperties drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &drv;
    vkGetPhysicalDeviceProperties2(phys, &p2);
    std::string out = drv.driverName;
    if (drv.driverInfo[0])
    {
        if (!out.empty()) out += " ";
        out += drv.driverInfo;
    }
    else
    {
        if (!out.empty()) out += " ";
        out += decode_driver_version(p2.properties);
    }
    return out;
}

std::string LLVulkanContext::deviceLostDiagnostics() const
{
    // Physical-device queries and our own allocation tracking only: valid on a lost device.
    if (!mImpl || mImpl->mDevice.physicalDevice() == VK_NULL_HANDLE) return "Vulkan device: not initialized\n";
    VkPhysicalDevice phys = mImpl->mDevice.physicalDevice();
    VkPhysicalDeviceDriverProperties drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &drv;
    vkGetPhysicalDeviceProperties2(phys, &p2);
    const VkPhysicalDeviceProperties& p = p2.properties;
    char buf[512];
    std::string out;
    const uint32_t v = p.driverVersion;
    const std::string ver = decode_driver_version(p);
    std::snprintf(buf, sizeof(buf), "GPU: %s (vendor 0x%04x device 0x%04x), Vulkan %u.%u.%u\n", p.deviceName,
                  p.vendorID, p.deviceID, VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion),
                  VK_VERSION_PATCH(p.apiVersion));
    out += buf;
    std::snprintf(buf, sizeof(buf), "Driver: %s %s (driverVersion %s, raw 0x%08x)\n", drv.driverName, drv.driverInfo, ver.c_str(), v);
    out += buf;
    const VkDeviceContext::MemStats ms = mImpl->mDevice.memStats();
    for (uint32_t i = 0; i < ms.heapCount; ++i)
    {
        if (!ms.heapDeviceLocal[i]) continue;
        std::snprintf(buf, sizeof(buf), "VRAM heap %u: ours %llu MB, process %s%llu MB, budget %llu MB%s\n", i,
                      (unsigned long long)(ms.heapUsed[i] >> 20), ms.budgetExt ? "" : "n/a ",
                      (unsigned long long)(ms.heapDriverUsage[i] >> 20), (unsigned long long)(ms.heapBudget[i] >> 20),
                      ms.budgetExt ? "" : " (heap size: no VK_EXT_memory_budget)");
        out += buf;
    }
    if (ms.uma)
    {   // #695
        const VkDeviceContext::BudgetHeaps bh = VkDeviceContext::budgetHeaps(ms);
        std::snprintf(buf, sizeof(buf), "UMA budget: carve-out heap %u %llu MB + shared heap %u %llu MB (ceiling %llu MB); "
                                        "%llu MB spilled to shared memory (%llu allocations)\n",
                      bh.heap, (unsigned long long)(bh.devBudget >> 20), bh.hostHeap, (unsigned long long)(bh.hostBudget >> 20),
                      (unsigned long long)(ms.umaHostCeil >> 20), (unsigned long long)(ms.umaSpillBytes >> 20),
                      (unsigned long long)ms.umaSpills);
        out += buf;
    }
    std::snprintf(buf, sizeof(buf), "Allocations: %u (buffers %llu MB, images %llu MB, host-visible %llu MB)\n",
                  ms.allocCount, (unsigned long long)(ms.bufferBytes >> 20), (unsigned long long)(ms.imageBytes >> 20),
                  (unsigned long long)(ms.hostVisibleBytes >> 20));
    out += buf;
    {
        // #407: our allocations by category (device-local heaps), from the tracker alone.
        std::string cats = "VRAM by category (MB):";
        for (uint32_t c = 0; c < VKMEM_COUNT; ++c)
        {
            if (!ms.catDev[c]) continue;
            std::snprintf(buf, sizeof(buf), " %s %llu", vkMemCatName(c), (unsigned long long)(ms.catDev[c] >> 20));
            cats += buf;
        }
        out += cats + "\n";
    }
    out += "Ray tracing: " + rayTracingStatus() + "\n";
    out += "Async compute: " + mImpl->mDevice.asyncComputeDecision() + "; queue: "
         + mImpl->mDevice.asyncComputeStatus() + "\n";
    out += "Texture pools: " + mImpl->mDevice.texturePoolStatus() + "\n";
    if (ms.texPools)
    {
        // #411: blocks, live vs block bytes (occupancy), and the driver allocations they replaced.
        std::snprintf(buf, sizeof(buf), "Texture pool blocks: %u (%llu MB), %u images %llu MB live (%.0f%% occupied), "
                      "slack %llu MB\n", ms.texPoolBlocks, (unsigned long long)(ms.texPoolBlockBytes >> 20), ms.texPooledImages,
                      (unsigned long long)(ms.texPoolLive >> 20),
                      ms.texPoolBlockBytes ? 100.0 * (double)ms.texPoolLive / (double)ms.texPoolBlockBytes : 0.0,
                      (unsigned long long)(ms.texPoolSlack >> 20));
        out += buf;
        // #614: what compaction could still free, the empty blocks VMA keeps, and its work so far.
        std::snprintf(buf, sizeof(buf), "Texture pool compaction: reclaimable %llu MB, %u empty blocks (%llu MB); "
                      "moved %llu images / %llu MB, %llu blocks evacuated, %llu empty released, %llu busy skips\n",
                      (unsigned long long)(ms.texPoolReclaim >> 20), ms.texPoolEmptyBlocks,
                      (unsigned long long)(ms.texPoolEmptyBytes >> 20), (unsigned long long)ms.texCompactMoves,
                      (unsigned long long)(ms.texCompactMoved >> 20), (unsigned long long)ms.texCompactEvacuated,
                      (unsigned long long)ms.texCompactReleased, (unsigned long long)ms.texCompactSkipped);
        out += buf;
    }
    const char* async = mImpl->mDevice.computeQueue() != VK_NULL_HANDLE ? "yes" : "no";
    std::snprintf(buf, sizeof(buf), "Async compute queue: %s; frames presented: %llu\n", async,
                  (unsigned long long)mImpl->mFrameCounter);
    out += buf;
    return out;
}

LLRenderBackend* LLVulkanContext::initRenderBackend()
{
    if (!mInitialized || !mImpl)
    {
        LL_WARNS("Vulkan") << "initRenderBackend before context init" << LL_ENDL;
        return nullptr;
    }
    if (mImpl->mRenderBackend)
    {
        return mImpl->mRenderBackend;
    }

    if (!gVK.initFromSpv(mImpl->mDevice,
                         kUiVertSpv, kUiVertSpv_bytes,
                         kUiFragSpv, kUiFragSpv_bytes))
    {
        LL_WARNS("Vulkan") << "LLRenderVK facade init failed" << LL_ENDL;
        return nullptr;
    }
    // Alpha-tested fragment variant for offscreen render targets (#83).
    gVK.initAlphaTestFragSpv(kUiFragATestSpv, kUiFragATestSpv_bytes);
    // In-world label occlusion vertex variant (pre-HUD text layer, #487).
    gVK.initOcclVertSpv(kUiVertOcclSpv, kUiVertOcclSpv_bytes);
    mImpl->mFacadeReady = true;

    mImpl->mRenderBackend = new LLRenderVKBackend();
    LLRender::setBackend(mImpl->mRenderBackend);
    LL_INFOS("Vulkan") << "LLRenderVK facade + backend installed; gGL now "
                          "dispatches to Vulkan." << LL_ENDL;
    return mImpl->mRenderBackend;
}

// sync2 image-layout barrier helper.
static void imageBarrier(VkCommandBuffer cmd, VkImage image,
                         VkImageLayout oldLayout, VkImageLayout newLayout,
                         VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                         VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
                         uint32_t layers = 1)   // #1075: every layer of a layered (stereo) target
{
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    b.oldLayout = oldLayout;
    b.newLayout = newLayout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};

    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

void LLVulkanContext::pipelineCompileProgress(U32& done, U32& queued, F64& idle_sec) const
{
    unsigned d = 0, q = 0;
    double idle = 0.0;
    vkPipelineCompileProgress(d, q, idle);
    done = d;
    queued = q;
    idle_sec = idle;
}

bool LLVulkanContext::worldPipelinesPending() const
{
    if (!mInitialized || !mImpl) return false;
    if (!mImpl->mWorld) return !mImpl->mWorldInitTried;   // created on the first renderFrame
    if (mImpl->mWorld->pipelinesPending()) return true;
    unsigned done = 0, queued = 0, live = 0;
    vkPipelineGateProgress(done, queued, live);
    return live > 0;
}

void LLVulkanContext::shaderGateProgress(U32& done, U32& queued) const
{
    unsigned d = 0, q = 0, live = 0;
    vkPipelineGateProgress(d, q, live);
    done = d;
    queued = q;
}

bool LLVulkanContext::rayQuerySupported() const
{
    return mInitialized && mImpl && mImpl->mDevice.rayQuerySupported();
}

bool LLVulkanContext::rayTracingEnabled() const
{
    return mInitialized && mImpl && mImpl->mDevice.rayTracingEnabled();
}

bool LLVulkanContext::textureCompressionBC() const
{
    return mInitialized && mImpl && mImpl->mDevice.textureCompressionBC();
}

std::string LLVulkanContext::rayTracingStatus() const
{
    return (mInitialized && mImpl) ? mImpl->mDevice.rayTracingStatus() : std::string("not initialized");
}

std::string LLVulkanContext::asyncComputeSummary() const
{
    if (!mInitialized || !mImpl) return "not initialized";
    return mImpl->mDevice.asyncComputeDecision() + "; queue: " + mImpl->mDevice.asyncComputeStatus();
}

// Lost device (#135): the readbacks would hand out whatever the dead GPU left in the
// host-visible buffers (the logout snapshot keeps the previous session's instead).
bool LLVulkanContext::readWorldSnapshot(std::vector<uint8_t>& rgb, uint32_t& w, uint32_t& h)
{
    if (vkDeviceIsLost()) return false;
    return mImpl && mImpl->mWorld && mImpl->mWorld->readSnapshotRGB(rgb, w, h);
}

bool LLVulkanContext::readWorldPixel(int32_t x, int32_t y, uint8_t rgba[4])
{
    if (vkDeviceIsLost()) return false;
    return mImpl && mImpl->mWorld && mImpl->mWorld->readWorldPixel(x, y, rgba);
}

bool LLVulkanContext::requestCapture(uint32_t point)
{
    if (vkDeviceIsLost() || !mImpl || !mImpl->mWorld) return false;
    mImpl->mWorld->requestCapture(point);
    return true;
}

bool LLVulkanContext::readCapture(std::vector<uint8_t>& rgb, uint32_t& w, uint32_t& h)
{
    if (vkDeviceIsLost()) return false;
    return mImpl && mImpl->mWorld && mImpl->mWorld->readCaptureRGB(rgb, w, h);
}

bool LLVulkanContext::liveFrameInfo(uint32_t& w, uint32_t& h) const
{
    w = h = 0;
    if (!mInitialized || !mImpl || vkDeviceIsLost() || !mImpl->mWorld || !mImpl->mWorld->ready()) return false;
    if (!mImpl->mSwapchain.valid()) return false;
    const VkExtent2D e = mImpl->mSwapchain.extent();
    w = e.width; h = e.height;
    return w && h && LLCleanSnapshotVK::sourceFormatSupported(mImpl->mSwapchain.format());
}

bool LLVulkanContext::beginLiveSnapshot(const LiveSnapshot& req, std::string& why)
{
    uint32_t sw = 0, sh = 0;
    if (!liveFrameInfo(sw, sh))
    {
        why = "no live frame to capture (renderer not ready or unsupported swapchain format)";
        return false;
    }
    LLCleanSnapshotVK::Request r;
    r.point = req.point;
    r.mode = req.mode == 1 ? LLCleanSnapshotVK::MODE_DEPTH
           : req.mode == 2 ? LLCleanSnapshotVK::MODE_ACCUM : LLCleanSnapshotVK::MODE_RESAMPLE;
    if (r.mode == LLCleanSnapshotVK::MODE_DEPTH) r.point = 1;
    r.outW = req.outW; r.outH = req.outH;
    std::memcpy(r.crop, req.crop, sizeof(r.crop));
    r.frames = req.frames;
    r.motionReject = req.motionReject;
    r.dither = req.dither;
    r.sharpen = req.sharpen; r.adaptiveSigma = req.adaptiveSigma; r.mipBias = req.mipBias;   // (#1093)
    r.depthRange[0] = req.depthNear; r.depthRange[1] = req.depthFar;
    return mImpl->mWorld->liveSnap()->arm(&mImpl->mDevice, r, sw, sh, why);
}

bool LLVulkanContext::liveSnapshotFinished() const
{
    if (!mImpl || !mImpl->mWorld || vkDeviceIsLost()) return true;
    return !mImpl->mWorld->liveSnap()->armed();   // (frames still to render while armed)
}

bool LLVulkanContext::readLiveSnapshot(std::vector<uint8_t>& rgb, uint32_t& w, uint32_t& h, std::string& why)
{
    if (!mImpl || !mImpl->mWorld || vkDeviceIsLost()) { why = "no device"; return false; }
    LLCleanSnapshotVK* s = mImpl->mWorld->liveSnap();
    if (s->failed()) why = s->failReason();
    else if (!s->finished()) why = "the shot's frames were not all rendered";
    const bool ok = s->read(rgb, w, h);
    if (ok) why.clear();
    return ok;
}

void LLVulkanContext::cancelLiveSnapshot()
{
    if (mImpl && mImpl->mWorld && !vkDeviceIsLost()) mImpl->mWorld->liveSnap()->cancel();
}

bool LLVulkanContext::beginWorldOverlay()
{
    return mInitialized && mImpl && mImpl->mFacadeReady && gVK.beginWorldOverlay();
}

void LLVulkanContext::endWorldOverlay()
{
    if (mInitialized && mImpl && mImpl->mFacadeReady) gVK.endWorldOverlay();
}

bool LLVulkanContext::beginPreHudLayer(const float* proj, const float* view)
{
    return mInitialized && mImpl && mImpl->mFacadeReady && gVK.beginPreHudLayer(proj, view);
}

void LLVulkanContext::endPreHudLayer()
{
    if (mInitialized && mImpl && mImpl->mFacadeReady) gVK.endPreHudLayer();
}

// Per-frame push blocks for LLCleanRendererVK::recordWorld from the world frame.
static void fillWorldPushes(const LLVKWorldFrame& wf, VKDrawPush& draw, VKCullPush& cull, VKSkyPush& sky)
{
    const LLVKWorldFrame* world = &wf;
    std::memcpy(draw.view_proj, world->view_proj, sizeof(float)*16);
    std::memcpy(draw.sun_dir, world->sun_dir, sizeof(float)*4);
    std::memcpy(draw.sun_color, world->sun_color, sizeof(float)*4);
    std::memcpy(draw.ambient_color, world->ambient_color, sizeof(float)*4);
    std::memcpy(draw.cam_time, world->cam_pos, sizeof(float)*3);
    draw.cam_time[3] = world->time;
    std::memcpy(draw.sky_horizon, world->sky_horizon, sizeof(float)*4);
    std::memcpy(draw.sky_zenith,  world->sky_zenith,  sizeof(float)*4);
    std::memcpy(draw.inv_view_proj, world->inv_view_proj, sizeof(float)*16);
    std::memcpy(draw.water_fog, world->water_fog, sizeof(float)*4);

    std::memcpy(cull.frustum_planes, world->frustum_planes, sizeof(cull.frustum_planes));
    std::memcpy(cull.cam_pos, world->cam_pos, sizeof(float)*4);
    std::memcpy(cull.lod_cam_pos, world->lod_cam_pos, sizeof(float)*4);

    std::memcpy(sky.horizon, world->sky_horizon, sizeof(float)*4);
    std::memcpy(sky.zenith, world->sky_zenith, sizeof(float)*4);
    // Camera-relative sky rays (#555): the translation-free inverse; cam.w = 2 tells
    // clean_sky.frag not to subtract the eye (cam.xyz stays the eye: underwater test).
    std::memcpy(sky.inv_view_proj, world->inv_view_proj_rel, sizeof(float)*16);
    std::memcpy(sky.cam, world->cam_pos, sizeof(float)*3); sky.cam[3] = 2.f;

}

bool LLVulkanContext::renderOffscreen(const LLVKWorldFrame& world, uint32_t w, uint32_t h,
                                      std::vector<uint8_t>& rgb, std::vector<float>* depth, uint32_t frames,
                                      uint32_t aaFrames)
{
    if (!mInitialized || !mImpl || !world.enabled || !w || !h) return false;
    if (vkDeviceIsLost()) return false;   // #135
    LLCleanRendererVK* W = mImpl->mWorld;
    if (!W || !W->ready()) return false;
    // #1075: a stereo session's recordings are N-view (layered targets, multiview scopes); an
    // offscreen snapshot is one view into a one-layer image -- refused (snapshots are in the
    // stereo-off set, design §8.3).
    if (W->stereoActive())
    {
        LL_WARNS_ONCE("Vulkan") << "offscreen snapshot refused: not available while rendering stereo views (#1075)" << LL_ENDL;
        return false;
    }
    VkDevice dev = mImpl->mDevice.device();
    vkDeviceWaitIdle(dev);   // no frame in flight: the renderer's per-slot state is ours

    GpuImage img;
    {
        VkMemScope memcat(VKMEM_TARGET);   // #407 (snapshot colour target; the renderer scopes its own)
        // #730: soft -- the first snapshot-size allocation; out of memory fails the snapshot
        // here (the !img.image return below) instead of ending in the fatal handler.
        VkDeviceContext::SoftAllocScope soft(&mImpl->mDevice);
        img = mImpl->mDevice.createImage2D(w, h, mImpl->mSwapchain.format(),
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT);
    }
    if (!img.image) return false;
    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VK_CHECK(vkCreateFence(dev, &fi, nullptr, &fence));
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = mImpl->mDevice.commandPool();
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateCommandBuffers(dev, &ai, &cmd));

    { VK_HITCH_SCOPE("vk_syncFromStores"); W->syncFromStores(); }
    VKDrawPush draw{}; VKCullPush cull{}; VKSkyPush sky{};
    fillWorldPushes(world, draw, cull, sky);
    const uint32_t slot = mImpl->mSwapchain.frameSlot();
    // SNAPSHOT SUPERSAMPLING (#417): the renderer saves the live view's temporal state and,
    // with aaFrames > 0 (never for depth: the raw depth must stay unjittered), allocates the
    // accumulation targets -- 0 back when they don't fit, then the old path below runs.
    // Supersampled: recording 0 is the warm-up (SSR's previous frame at this size), 1..N
    // are jittered and averaged; the capture takes the last, converged one. Otherwise
    // `frames` unjittered recordings (warm-up for the history-based passes) as before.
    const uint32_t aa = W->beginOffscreen(w, h, depth ? 0u : aaFrames);
    frames = aa ? aa + 1u : std::max(frames, 1u);
    W->snapMatteBegin(world.avatar_matte && !depth, w, h);   // <GMDX> avatar matte snapshot
    if (aa)
        LL_DEBUGS("Vulkan") << "offscreen snapshot " << w << "x" << h << ": supersampled, " << aa
                           << " jittered frames" << LL_ENDL;
    for (uint32_t f = 0; f < frames; ++f)
    {
        W->mOffscreenFrame = f;
        W->snapMatteFrame(f);   // <GMDX>
        const bool last = (f + 1 == frames);
        if (last) W->requestCapture(world.hud_enabled ? 2u : 1u);
        VK_CHECK(vkResetCommandBuffer(cmd, 0));
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
        imageBarrier(cmd, img.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                     VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkClearColorValue clear{}; clear.float32[3] = 1.f;
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
        imageBarrier(cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

        W->setFrameSlot(slot);
        W->specFrameTick();   // setting specialization: bind / fall back at the frame boundary
        W->setLights(world);
        W->setProbes(world);
        W->setShadows(world);
        W->setMirror(world);
        W->setTemporal(world);
        W->mHideSelf = world.hide_self;
        W->mHideSelfHead = world.hide_self_head;
        W->mOrtho = world.ortho;            // <GMDX> region map capture
        W->mOrthoPpm = world.ortho_ppm;     // <GMDX>
        W->mOrthoView = world.ortho_view;   // <GMDX> snapshot of the orthographic view
        W->setSelection(world);
        W->setAtmosphere(world);
        W->setSwapImage(img.image);
        W->mWorldOverlayHook = [](VkCommandBuffer, VkImageView, VkImageView, VkImage, uint32_t, uint32_t) {};
        W->recordWorld(cmd, img.view, w, h, draw, cull, sky,
                       world.hud_enabled ? world.hud_view_proj : nullptr, world.hud_viewport);
        W->recordCaptureAt(cmd, 2, w, h);
        if (last && depth) W->recordDepthCapture(cmd, w, h);
        W->snapMatteRecord(cmd, w, h);   // <GMDX>
        VK_CHECK(vkEndCommandBuffer(cmd));

        VkCommandBufferSubmitInfo ci{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        ci.commandBuffer = cmd;
        VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        si.commandBufferInfoCount = 1; si.pCommandBufferInfos = &ci;
        VK_CHECK(vkQueueSubmit2(mImpl->mDevice.queue(), 1, &si, fence));
        VK_CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
        if (vkDeviceIsLost()) break;   // #135: the readback below would be garbage
        W->snapMatteCollect();   // <GMDX>
        VK_CHECK(vkResetFences(dev, 1, &fence));
    }
    W->snapMatteEnd();   // <GMDX>
    uint32_t cw = 0, ch = 0;
    bool ok = !vkDeviceIsLost() && W->readCaptureRGB(rgb, cw, ch) && cw == w && ch == h;
    if (ok && depth)
    {
        uint32_t dw = 0, dh = 0;
        ok = W->readDepthCapture(*depth, dw, dh) && dw == w && dh == h;
    }
    W->mOrtho = false;      // <GMDX> (live frames are never a map capture)
    W->mOrthoPpm = 0.f;     // <GMDX>
    W->mOrthoView = false;  // <GMDX> (the next live frame sets its own)
    W->endOffscreen();   // the next on-screen frame resets like a window resize
    W->setSwapImage(VK_NULL_HANDLE);   // the next presented frame sets its own
    vkFreeCommandBuffers(dev, mImpl->mDevice.commandPool(), 1, &cmd);
    vkDestroyFence(dev, fence, nullptr);
    mImpl->mDevice.destroyImage(img);
    return ok;
}

// <GMDX> avatar matte snapshots (gmdxsnapshotavatars.h)
bool LLVulkanContext::takeSnapshotMatte(std::vector<uint8_t>& alpha, uint32_t& w, uint32_t& h)
{
    return mImpl && mImpl->mWorld && mImpl->mWorld->snapMatteTake(alpha, w, h);
}

uint32_t LLVulkanContext::maxImageDimension() const
{
    if (!mImpl) return 0;
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(mImpl->mDevice.physicalDevice(), &props);
    return props.limits.maxImageDimension2D;
}

// Swapchain images are allocated by the driver / WSI, not through VkDeviceContext (#407):
// VkSwapchain measures them (memory requirements of their create info x image count).
// The estimate (count x extent x texel size) is only the fallback when that query gave 0.
static uint64_t swapchainBytes(const VkSwapchain& sc)
{
    if (sc.memoryBytes()) return sc.memoryBytes();
    uint32_t bpp = 4;
    switch (sc.format())
    {
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R16G16B16A16_UNORM: bpp = 8; break;
    case VK_FORMAT_R32G32B32A32_SFLOAT: bpp = 16; break;
    default: bpp = 4; break;   // B8G8R8A8 / R8G8B8A8 / A2B10G10R10 / A2R10G10B10
    }
    const VkExtent2D e = sc.extent();
    return (uint64_t)sc.imageCount() * e.width * e.height * bpp;
}

namespace
{
const char* vkSurfaceFormatName(VkFormat f)
{
    switch (f)
    {
    case VK_FORMAT_B8G8R8A8_UNORM: return "BGRA8";
    case VK_FORMAT_B8G8R8A8_SRGB: return "BGRA8-sRGB";
    case VK_FORMAT_R8G8B8A8_UNORM: return "RGBA8";
    case VK_FORMAT_R8G8B8A8_SRGB: return "RGBA8-sRGB";
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return "A2B10G10R10";
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32: return "A2R10G10B10";
    case VK_FORMAT_R16G16B16A16_SFLOAT: return "RGBA16F";
    default: return "other";
    }
}

const char* vkColorSpaceName(VkColorSpaceKHR c)
{
    switch (c)
    {
    case VK_COLOR_SPACE_SRGB_NONLINEAR_KHR: return "sRGB";
    case VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT: return "scRGB";
    case VK_COLOR_SPACE_HDR10_ST2084_EXT: return "HDR10";
    case VK_COLOR_SPACE_DISPLAY_P3_NONLINEAR_EXT: return "P3";
    default: return "other";
    }
}

const char* vkPresentModeName(VkPresentModeKHR m)
{
    switch (m)
    {
    case VK_PRESENT_MODE_FIFO_KHR: return "fifo";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "fifo-relaxed";
    case VK_PRESENT_MODE_MAILBOX_KHR: return "mailbox";
    case VK_PRESENT_MODE_IMMEDIATE_KHR: return "immediate";
    default: return "other";
    }
}
} // namespace

std::string LLVulkanContext::displayInfoText() const
{
    // One short line (bug reports, < 200 chars): the swapchain as created (it is what the
    // VRAM accounting measures) and the monitors -- count, the window's display mode, HDR.
    std::string o;
    char buf[160];
    if (mImpl && mImpl->mSwapchain.valid())
    {
        const VkSwapchain& sc = mImpl->mSwapchain;
        std::snprintf(buf, sizeof(buf), "swapchain %ux%u %s/%s %s x%u (%llu MB)", sc.extent().width, sc.extent().height,
                      vkSurfaceFormatName(sc.format()), vkColorSpaceName(sc.colorSpace()), vkPresentModeName(sc.presentMode()),
                      sc.imageCount(), (unsigned long long)((swapchainBytes(sc) + (1u << 19)) >> 20));
        o = buf;
    }
    int count = 0;
    SDL_DisplayID* ids = SDL_GetDisplays(&count);
    if (ids) SDL_free(ids);
    std::snprintf(buf, sizeof(buf), "%smonitors %d", o.empty() ? "" : "; ", count);
    o += buf;
    const SDL_DisplayID win_disp = (mImpl && mImpl->mWindow) ? SDL_GetDisplayForWindow(mImpl->mWindow) : 0;
    if (win_disp)
    {
        const SDL_DisplayMode* dm = SDL_GetCurrentDisplayMode(win_disp);
        const bool hdr = SDL_GetBooleanProperty(SDL_GetDisplayProperties(win_disp), SDL_PROP_DISPLAY_HDR_ENABLED_BOOLEAN, false);
        if (dm)
        {
            std::snprintf(buf, sizeof(buf), ", window on %dx%d@%.0fHz scale %.2f", dm->w, dm->h, (double)dm->refresh_rate,
                          (double)dm->pixel_density);
            o += buf;
        }
        o += hdr ? ", HDR on" : ", HDR off";
    }
    return o;
}

LLVKVramSummary LLVulkanContext::vramSummary() const
{
    LLVKVramSummary s;
    if (!mImpl || !mImpl->mWorld) return s;
    const LLVramStatus& v = mImpl->mWorld->vramStatus();
    if (!v.valid) return s;
    s.valid = true;
    s.capacity = v.capacity; s.reserve = v.reserve; s.budget = v.budget; s.usage = v.usage;
    s.fixed = v.fixed; s.overhead = v.overhead;
    s.fixedSmoothed = v.fixedSmoothed; s.scalable = v.scalable; s.swapchain = v.swapchain;
    s.slack = v.slackDev; s.pad = v.pad; s.unattributed = v.unattributed;
    s.rung = v.rung; s.rungCount = v.rungCount; s.step = v.step; s.mode = v.mode;
    return s;
}

std::string LLVulkanContext::vramStatusText(int part) const
{
    if (!mImpl || !mImpl->mWorld) return part == 1 ? std::string() : std::string("n/a (no Vulkan world renderer)");
    const LLVramStatus& v = mImpl->mWorld->vramStatus();
    return part == 0 ? v.summaryText() : part == 1 ? v.categoryText() : part == 3 ? v.overheadText()
         : part == 4 ? v.texPoolText() : v.compactText();
}

// #458: allocation-traffic rates, sampled by hostMemTick (main thread).
namespace
{
struct HostMemRates
{
    bool   have_prev = false, have_rate = false;
    double prev_t = 0.0, next_log = 0.0;
    uint64_t prev_alloc[2] = {}, prev_free[2] = {}, prev_alloc_b[2] = {}, prev_free_b[2] = {};
    uint64_t prev_cat_alloc[VKMEM_COUNT] = {};
    double alloc_s[2] = {}, free_s[2] = {}, alloc_mb_s[2] = {}, free_mb_s[2] = {};
    double cat_alloc_s[VKMEM_COUNT] = {};
};
HostMemRates s_host_rates;
}

std::string LLVulkanContext::hostMemText() const
{
    if (!mInitialized || !mImpl) return std::string();
    const VkDeviceContext::MemStats ms = mImpl->mDevice.memStats();
    auto mb = [](uint64_t b) { return (double)b / (1024.0 * 1024.0); };
    const uint64_t dev_only = ms.bufferBytes + ms.imageBytes > ms.hostVisibleBytes
                            ? ms.bufferBytes + ms.imageBytes - ms.hostVisibleBytes : 0;
    std::string o = llformat("host-visible %.0f MB / %u allocs [", mb(ms.hostVisibleBytes), ms.hostVisibleCount);
    bool first = true;
    for (uint32_t c = 0; c < VKMEM_COUNT; ++c)
    {
        if (!ms.catHVCount[c]) continue;
        o += llformat("%s%s %.0f/%u", first ? "" : " ", vkMemCatName(c), mb(ms.catHV[c]), ms.catHVCount[c]);
        first = false;
    }
    o += llformat("] device-only %.0f MB", mb(dev_only));
    const HostMemRates& r = s_host_rates;
    if (r.have_rate)
    {
        o += llformat(" | /s: alloc hv %.1f (%.0f MB) dev %.1f (%.0f MB), free hv %.1f (%.0f MB) dev %.1f (%.0f MB)",
                      r.alloc_s[1], r.alloc_mb_s[1], r.alloc_s[0], r.alloc_mb_s[0],
                      r.free_s[1], r.free_mb_s[1], r.free_s[0], r.free_mb_s[0]);
        // the three busiest categories by allocation calls
        uint32_t top[3] = {VKMEM_COUNT, VKMEM_COUNT, VKMEM_COUNT};
        for (uint32_t c = 0; c < VKMEM_COUNT; ++c)
        {
            if (r.cat_alloc_s[c] < 0.05) continue;
            for (int k = 0; k < 3; ++k)
            {
                if (top[k] == VKMEM_COUNT || r.cat_alloc_s[c] > r.cat_alloc_s[top[k]])
                {
                    for (int j = 2; j > k; --j) top[j] = top[j - 1];
                    top[k] = c;
                    break;
                }
            }
        }
        if (top[0] != VKMEM_COUNT)
        {
            o += " busiest";
            for (int k = 0; k < 3 && top[k] != VKMEM_COUNT; ++k)
                o += llformat(" %s %.1f", vkMemCatName(top[k]), r.cat_alloc_s[top[k]]);
        }
        o += llformat(" | total allocs %llu frees %llu",
                      (unsigned long long)(ms.allocCalls[0] + ms.allocCalls[1]),
                      (unsigned long long)(ms.freeCalls[0] + ms.freeCalls[1]));
    }
    return o;
}

void LLVulkanContext::hostMemTick(double now, bool memTight)
{
    if (!mInitialized || !mImpl) return;
    HostMemRates& r = s_host_rates;
    if (r.have_prev && now - r.prev_t < 1.0) return;
    // The UI upload staging ring's cap, like the world texture ring: RAM / 64, here within
    // [128 MB, 256 MB] (a 4K media frame, 32 MB, must stay under cap / 4).
    if (mImpl->mFacadeReady)
        gVK.setStageRingMax((uint64_t)LLMemoryGovernor::scaleCacheBudget(256ull << 20, 64, 128ull << 20));
    const VkDeviceContext::MemStats ms = mImpl->mDevice.memStats();
    if (r.have_prev)
    {
        const double dt = std::max(now - r.prev_t, 1.0e-3);
        for (int k = 0; k < 2; ++k)
        {
            r.alloc_s[k] = (double)(ms.allocCalls[k] - r.prev_alloc[k]) / dt;
            r.free_s[k] = (double)(ms.freeCalls[k] - r.prev_free[k]) / dt;
            r.alloc_mb_s[k] = (double)(ms.allocBytesTotal[k] - r.prev_alloc_b[k]) / dt / (1024.0 * 1024.0);
            r.free_mb_s[k] = (double)(ms.freeBytesTotal[k] - r.prev_free_b[k]) / dt / (1024.0 * 1024.0);
        }
        for (uint32_t c = 0; c < VKMEM_COUNT; ++c)
            r.cat_alloc_s[c] = (double)(ms.catAllocCalls[c] - r.prev_cat_alloc[c]) / dt;
        r.have_rate = true;
    }
    for (int k = 0; k < 2; ++k)
    {
        r.prev_alloc[k] = ms.allocCalls[k];
        r.prev_free[k] = ms.freeCalls[k];
        r.prev_alloc_b[k] = ms.allocBytesTotal[k];
        r.prev_free_b[k] = ms.freeBytesTotal[k];
    }
    for (uint32_t c = 0; c < VKMEM_COUNT; ++c) r.prev_cat_alloc[c] = ms.catAllocCalls[c];
    r.prev_t = now;
    r.have_prev = true;
    const bool high = r.have_rate && (r.alloc_s[1] > 20.0 || r.alloc_mb_s[1] > 64.0);
    if ((high || memTight) && now >= r.next_log)
    {
        r.next_log = now + 5.0;
        LL_INFOS("VKMem") << "hostmem" << (high ? " (high traffic)" : "") << (memTight ? " (memory tight)" : "")
                          << ": " << hostMemText() << LL_ENDL;
    }
}

std::string LLVulkanContext::texInspectText(const LLUUID& obj, int face, unsigned map, const float* uv_px) const
{
    if (!mImpl || !mImpl->mWorld) return std::string();
    return mImpl->mWorld->texInspectText(obj, face, map, uv_px);
}

std::string LLVulkanContext::texResidencyBrief(const LLUUID& id) const
{
    if (!mImpl || !mImpl->mWorld) return std::string();
    return mImpl->mWorld->texResidencyBrief(id);
}

// <GMDX>
bool LLVulkanContext::texOnGpu(const LLUUID& id) const
{
    if (!mImpl || !mImpl->mWorld) return true;
    return mImpl->mWorld->texOnGpu(id);
}
// </GMDX>

std::string LLVulkanContext::vramReadoutText() const
{
    if (!mImpl || !mImpl->mWorld) return "Video memory: not measured yet";
    return mImpl->mWorld->vramStatus().readoutText();
}

float LLVulkanContext::vramShadowRangeScale() const
{
    return (mImpl && mImpl->mWorld) ? mImpl->mWorld->vramShadowRangeScale() : 1.f;
}

float LLVulkanContext::vramMeshLodScale() const
{
    return (mImpl && mImpl->mWorld) ? mImpl->mWorld->vramMeshLodScale() : 1.f;
}

float LLVulkanContext::vramAvatarLodScale() const
{
    return (mImpl && mImpl->mWorld) ? mImpl->mWorld->vramAvatarLodScale() : 1.f;
}

float LLVulkanContext::vramTerrainLodScale() const
{
    return (mImpl && mImpl->mWorld) ? mImpl->mWorld->vramTerrainLodScale() : 1.f;
}

float LLVulkanContext::vramDrawDistance(float setting) const
{
    const float scale = (mImpl && mImpl->mWorld) ? mImpl->mWorld->vramDrawDistScale() : 1.f;
    return LLVramBudgetVK::effectiveDrawDistance(setting, scale);
}

bool LLVulkanContext::autoTuneSample(LLVKAutoTuneSample& out) const
{
    if (!mImpl || !mImpl->mWorld) { out.valid = false; return false; }
    mImpl->mWorld->autoTuneSample(out);
    return out.valid;
}

void LLVulkanContext::setAutoTunePerfCaps(const LLVKPerfCaps& caps)
{
    if (!mImpl) return;
    mImpl->mAutoTuneCaps = caps;
    // #1076: with the VR safety governor's caps (default: no cap -- merged() then returns
    // `caps` unchanged, as on the desktop where the governor never runs).
    if (mImpl->mWorld) mImpl->mWorld->setPerfCaps(LLVKPerfCaps::merged(caps, mImpl->mVrSafetyCaps));
}

void LLVulkanContext::setVrSafetyPerfCaps(const LLVKPerfCaps& caps)
{
    if (!mImpl) return;
    mImpl->mVrSafetyCaps = caps;
    if (mImpl->mWorld) mImpl->mWorld->setPerfCaps(LLVKPerfCaps::merged(mImpl->mAutoTuneCaps, caps));
}

uint32_t LLVulkanContext::shadowMapResolution() const
{
    return (mImpl && mImpl->mWorld) ? mImpl->mWorld->shadowMapResolution() : 0u;
}

uint64_t LLVulkanContext::freeDeviceMemory() const
{
    if (!mImpl) return 0;
    const VkDeviceContext::MemStats ms = mImpl->mDevice.memStats();
    if (ms.uma)
    {   // #695: the combined UMA budget (carve-out + shared host heap share) minus both heaps' usage
        const VkDeviceContext::BudgetHeaps bh = VkDeviceContext::budgetHeaps(ms);
        const uint64_t used = (ms.budgetExt ? ms.heapDriverUsage[bh.heap] + ms.heapDriverUsage[bh.hostHeap]
                                            : ms.heapUsed[bh.heap] + ms.heapUsed[bh.hostHeap]);
        return bh.budget > used ? bh.budget - used : 0;
    }
    uint64_t best = 0;
    for (uint32_t i = 0; i < ms.heapCount; ++i)
    {
        if (!ms.heapDeviceLocal[i]) continue;
        const uint64_t used = ms.budgetExt ? ms.heapDriverUsage[i] : ms.heapUsed[i];
        if (ms.heapBudget[i] > used) best = std::max<uint64_t>(best, ms.heapBudget[i] - used);
    }
    return best;
}

LLVulkanContext::SnapshotVram LLVulkanContext::snapshotVram() const
{
    SnapshotVram s;
    if (!mInitialized || !mImpl) return s;
    s.usable = freeDeviceMemory();
    if (mImpl->mWorld)
    {
        // The renderer's own budget (user cap / OS budget less its reserve): a snapshot that
        // fits the OS budget but not this one starts the ladder shedding mid-render.
        const LLVramStatus& v = mImpl->mWorld->vramStatus();
        if (v.valid && v.budget) s.usable = std::min<uint64_t>(s.usable, v.budget > v.usage ? v.budget - v.usage : 0);
    }
    const VkDeviceContext::MemStats ms = mImpl->mDevice.memStats();
    s.liveTargetBytes = ms.catDev[VKMEM_TARGET] + ms.catDev[VKMEM_RT_MASK] + ms.catDev[VKMEM_DENOISE];
    const VkExtent2D e = mImpl->mSwapchain.extent();
    s.livePx = (uint64_t)e.width * e.height;
    return s;
}

bool LLVulkanContext::latestPick(LLVKPickResult& out) const
{
    return mImpl && mImpl->mWorld && mImpl->mWorld->pickLatest(out);
}

// <SS:Skoomastream> aux rays (sscombatray.cpp)
void LLVulkanContext::takeAuxPicks(std::vector<LLVKPickResult>& out)
{
    if (mImpl && mImpl->mWorld) mImpl->mWorld->takeAuxPicks(out);
}

void LLVulkanContext::pollPickAnswers()
{
    // (#748 static pass) the answer of a frame in flight that has completed already, now -- not
    // when its slot comes round (a held click's answer a frame sooner; llcleancursorpick.h)
    if (!mImpl || !mImpl->mWorld || vkDeviceIsLost()) return;
    VK_HITCH_SCOPE("pick_harvest");   // (#757 crash breadcrumb)
    const VkDevice dev = mImpl->mDevice.device();
    for (uint32_t s = 0; s < mImpl->mSwapchain.framesInFlight(); ++s)
        if (mImpl->mSwapchain.slotDone(dev, s)) mImpl->mWorld->pickHarvestDone(s);
}

bool LLVulkanContext::pickNow(const LLVKPickRequest& req, LLVKPickResult& out)
{
    if (vkDeviceIsLost()) return false;   // #135
    return mImpl && mImpl->mWorld && mImpl->mWorld->ready() && mImpl->mWorld->pickNow(req, out);
}

bool LLVulkanContext::faceQuery(const std::vector<uint32_t>& instance_ids, const LLVKRectQuery* rect,
                                std::vector<LLVKFaceQueryResult>& out)
{
    if (vkDeviceIsLost()) return false;   // #135
    return mImpl && mImpl->mWorld && mImpl->mWorld->ready() && mImpl->mWorld->faceQuery(instance_ids, rect, out);
}

const char* LLVulkanContext::pickFailure() const
{
    if (vkDeviceIsLost()) return "device lost";
    if (!mImpl || !mImpl->mWorld) return "no world renderer";
    return mImpl->mWorld->pickFailure();
}

bool LLVulkanContext::pickGpuAvailable() const
{
    return !vkDeviceIsLost() && mImpl && mImpl->mWorld && mImpl->mWorld->ready() && mImpl->mWorld->pickAvailable();
}

bool LLVulkanContext::latestPartPick(LLVKPartPickResult& out) const
{
    return mImpl && mImpl->mWorld && mImpl->mWorld->partPickLatest(out);
}

bool LLVulkanContext::latestAcoustics(LLVKAcousticsResult& out) const
{
    return mImpl && mImpl->mWorld && mImpl->mWorld->acousticsLatest(out);
}

bool LLVulkanContext::latestAcousticsBounce(LLVKAcousticsBounce& out, uint32_t seen) const
{
    return mImpl && mImpl->mWorld && mImpl->mWorld->acousticsBounceLatest(out, seen);
}

uint32_t LLVulkanContext::acousticsBounceDispatched() const
{
    return (mImpl && mImpl->mWorld) ? mImpl->mWorld->acousticsBounceDispatched() : 0u;
}

uint32_t LLVulkanContext::worldViewCount() const
{
    return (mImpl && mImpl->mWorld) ? mImpl->mWorld->viewCount() : 1u;
}

bool LLVulkanContext::partPickNow(const LLVKPickRequest& req, LLVKPartPickResult& out)
{
    if (vkDeviceIsLost()) return false;   // #135
    return mImpl && mImpl->mWorld && mImpl->mWorld->ready() && mImpl->mWorld->partPickNow(req, out);
}

// #1074 VR: the eye-size world target. The window format (the final / UI pipelines are
// compiled for it, and its bytes are the eye images' UNORM sibling); kept while the session
// runs, so the renderer's screen targets keep one size (no target churn).
bool LLVulkanContext::Impl::vrEnsureTarget(uint32_t w, uint32_t h)
{
    const VkFormat fmt = mSwapchain.format();
    if (!w || !h || fmt == VK_FORMAT_UNDEFINED) return false;
    // #1075 LAYERED STEREO: one layer per view the world renders (the eyes; 1 = M1 mono).
    const uint32_t layers = mWorld ? std::max(1u, mWorld->viewCount()) : 1u;
    if (mVrTarget.image && mVrTarget.extent.width == w && mVrTarget.extent.height == h && mVrTarget.format == fmt &&
        mVrTarget.layers == layers)
        return true;
    if (mVrTarget.image) vrReleaseTarget(true);   // (another size / format: rare)
    if (!mVrBlitChecked)
    {
        mVrBlitChecked = true;
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(mDevice.physicalDevice(), fmt, &fp);
        const VkFormatFeatureFlags ff = fp.optimalTilingFeatures;
        mVrBlitOk = (ff & VK_FORMAT_FEATURE_BLIT_SRC_BIT) && (ff & VK_FORMAT_FEATURE_BLIT_DST_BIT);
        mVrBlitFilter = (ff & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        if (!mVrBlitOk)
            LL_WARNS("VR") << "the window format cannot be blitted: the desktop window shows no mirror of the headset" << LL_ENDL;
    }
    {
        VkMemScope memcat(VKMEM_TARGET);
        // Soft: out of memory leaves VR without the world for now (logged), never fatal.
        VkDeviceContext::SoftAllocScope soft(&mDevice);
        // (#1075: layers 1 is exactly the M1 createImage2D target)
        mVrTarget = mDevice.createScreenImage(w, h, layers, fmt,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT);
    }
    if (!mVrTarget.image)
    {
        static uint32_t s_warned = 0;
        if (s_warned++ < 5) LL_WARNS("VR") << "could not allocate the " << w << "x" << h << " world target for the headset" << LL_ENDL;
        mVrTarget = GpuImage{};
        return false;
    }
    LL_INFOS("VR") << "world target for the headset: " << w << "x" << h << " x" << layers
                   << (layers > 1 ? " layers (stereo: one per eye)" : "")
                   << " (the world renders at the eye size while the session runs)" << LL_ENDL;
    return true;
}

void LLVulkanContext::Impl::vrReleaseTarget(bool waitIdle)
{
    if ((!mVrTarget.image && !mVrUiTarget.image && !mVrHudDepth.image) || vkDeviceIsLost()) return;
    if (waitIdle && mDevice.device()) vkDeviceWaitIdle(mDevice.device());
    if (mVrTarget.image) mDevice.destroyImage(mVrTarget);
    mVrTarget = GpuImage{};
    if (mVrUiTarget.image) mDevice.destroyImage(mVrUiTarget);   // #1076
    mVrUiTarget = GpuImage{};
    if (mVrHudDepth.image) mDevice.destroyImage(mVrHudDepth);
    mVrHudDepth = GpuImage{};
}

// #1076 VR: the HUD pass's depth for HUDs drawn onto the UI wall image (its size, the world
// depth format the HUD pipelines are compiled for). Recreated only when the window size changes.
bool LLVulkanContext::Impl::vrEnsureHudDepth(VkExtent2D ext, VkFormat fmt)
{
    if (!ext.width || !ext.height || fmt == VK_FORMAT_UNDEFINED) return false;
    if (mVrHudDepth.image && mVrHudDepth.extent.width == ext.width && mVrHudDepth.extent.height == ext.height &&
        mVrHudDepth.format == fmt)
        return true;
    if (mVrHudDepth.image)
    {
        if (mDevice.device()) vkDeviceWaitIdle(mDevice.device());   // (a window resize: rare)
        mDevice.destroyImage(mVrHudDepth);
        mVrHudDepth = GpuImage{};
    }
    {
        VkMemScope memcat(VKMEM_TARGET);
        VkDeviceContext::SoftAllocScope soft(&mDevice);   // out of memory: no HUDs on the wall for now
        mVrHudDepth = mDevice.createImage2D(ext.width, ext.height, fmt, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                            VK_IMAGE_ASPECT_DEPTH_BIT);
    }
    if (!mVrHudDepth.image)
    {
        mVrHudDepth = GpuImage{};
        return false;
    }
    return true;
}

// #1076 VR: the UI wall image at the window size (the UI is laid out in window pixels), in the
// window format (the UI pipelines are compiled for it; its bytes are the UI swapchain's UNORM
// sibling). Recreated only when the window size changes.
bool LLVulkanContext::Impl::vrEnsureUiTarget(VkExtent2D ext)
{
    const VkFormat fmt = mSwapchain.format();
    if (!ext.width || !ext.height || fmt == VK_FORMAT_UNDEFINED) return false;
    if (mVrUiTarget.image && mVrUiTarget.extent.width == ext.width && mVrUiTarget.extent.height == ext.height &&
        mVrUiTarget.format == fmt)
        return true;
    if (mVrUiTarget.image)
    {
        if (mDevice.device()) vkDeviceWaitIdle(mDevice.device());   // (a window resize: rare)
        mDevice.destroyImage(mVrUiTarget);
        mVrUiTarget = GpuImage{};
    }
    {
        VkMemScope memcat(VKMEM_TARGET);
        VkDeviceContext::SoftAllocScope soft(&mDevice);   // out of memory: no wall content for now, never fatal
        mVrUiTarget = mDevice.createImage2D(ext.width, ext.height, fmt,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT);
    }
    if (!mVrUiTarget.image)
    {
        static uint32_t s_warned = 0;
        if (s_warned++ < 5) LL_WARNS("VR") << "could not allocate the " << ext.width << "x" << ext.height << " UI wall image" << LL_ENDL;
        mVrUiTarget = GpuImage{};
        return false;
    }
    LL_INFOS("VR") << "UI wall image: " << ext.width << "x" << ext.height << LL_ENDL;
    return true;
}

// After the world (and its captures): the target, in COLOR_ATTACHMENT_OPTIMAL, blitted
// aspect-fit (centred, bars in the window's clear colour) onto the window image, which is in
// COLOR_ATTACHMENT_OPTIMAL; both are left there. Same-format UNORM blit: the sRGB-encoded bytes
// are scaled, never re-encoded. A null windowImage (no window image this frame): nothing.
void LLVulkanContext::Impl::vrRecordMirror(VkCommandBuffer cmd, VkImage windowImage, VkExtent2D windowExtent)
{
    if (!windowImage || !mVrTarget.image || !mVrBlitOk || !windowExtent.width || !windowExtent.height) return;
    const uint32_t sw = mVrTarget.extent.width, sh = mVrTarget.extent.height;
    const float fit = std::min((float)windowExtent.width / (float)sw, (float)windowExtent.height / (float)sh);
    const int32_t dw = std::max(1, (int32_t)((float)sw * fit)), dh = std::max(1, (int32_t)((float)sh * fit));
    const int32_t dx = ((int32_t)windowExtent.width - dw) / 2, dy = ((int32_t)windowExtent.height - dh) / 2;
    // (#1075: a stereo target's every layer moves together; the mirror shows layer 0, the left eye)
    imageBarrier(cmd, mVrTarget.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT |
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, mVrTarget.layers);
    imageBarrier(cmd, windowImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageBlit rg{};
    rg.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    rg.srcOffsets[1] = {(int32_t)sw, (int32_t)sh, 1};
    rg.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    rg.dstOffsets[0] = {dx, dy, 0};
    rg.dstOffsets[1] = {dx + dw, dy + dh, 1};
    vkCmdBlitImage(cmd, mVrTarget.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, windowImage,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg, mVrBlitFilter);
    imageBarrier(cmd, windowImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    imageBarrier(cmd, mVrTarget.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_PIPELINE_STAGE_2_BLIT_BIT, 0,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT,
                 mVrTarget.layers);
}

bool LLVulkanContext::renderFrame(const std::function<void()>& uiRender,
                                  const LLVKWorldFrame* world)
{
    if (!mInitialized || !mImpl)
    {
        return false;
    }
    // #135: after a device loss nothing records or submits (the viewer's main loop shows
    // the message and exits; see vkDeviceIsLost). Before the swapchain recreate and the
    // pipeline poll, which would touch the lost device too.
    if (vkDeviceIsLost())
    {
        return false;
    }

    fseBeforeRecreate();   // #919 (Windows): exclusive chain on / off, release before a rebuild
    bool recreateNow = mImpl->mNeedsRecreate;
    if (recreateNow && mXrPresentOverride && mImpl->mFseMinimized)
    {
        // #1074 VR: the mirror window is minimised, so every recreate fails -- and each one
        // waits for the device to idle, which would stall the headset every frame. Retry
        // twice a second until the window is back. (mXrPresentOverride: a session runs.)
        static std::chrono::steady_clock::time_point s_last{};
        const auto now = std::chrono::steady_clock::now();
        if (now - s_last < std::chrono::milliseconds(500)) recreateNow = false;
        else s_last = now;
    }
    if (recreateNow)
    {
        mImpl->mSwapchain.recreate(mImpl->mDevice, mImpl->mWidth, mImpl->mHeight);
        mImpl->mNeedsRecreate = false;
        // (a failed recreate -- minimized -- retries every frame: only a valid one logs)
        if (mImpl->mSwapchain.valid()) noteSwapchainMode(mImpl->mSwapchain, true);
    }
    fseAfterRecreate();    // #919 (Windows): acquire exclusive mode on the exclusive swapchain

    // Bring up the GPU-driven world renderer on the FIRST frame -- the login screen --
    // not the first world frame: its pipelines (dozens of variants of the large world
    // FS, seconds each on a cold driver cache, i.e. every new release) then compile on
    // background threads while the user logs in, instead of stalling after the
    // progress bar (#213). It turns ready() when they are done. Failure is non-fatal
    // (UI-only present continues).
    const bool wantWorld = world && world->enabled;
    if (mImpl->mWorld) { VK_HITCH_SCOPE("vk_pollPipelines"); mImpl->mWorld->pollPipelines(); }
    if (!mImpl->mWorld && !mImpl->mWorldInitTried)
    {
        mImpl->mWorldInitTried = true;
        LLCleanRendererVK* w = new LLCleanRendererVK();
        w->mWantVrHudPipes = mXrSession;   // #1076 VR: the wall's HUD pipelines (desktop: false)
        // #1075: a stereo VR session renders one layer per eye (desktop: false).
        w->mStereoRequested = mXrSession && LLXrSession::instance().stereo();
        if (w->init(mImpl->mDevice, mImpl->mSwapchain.format()))
        {
            mImpl->mWorld = w;
        }
        else
        {
            LL_WARNS("Vulkan") << "World renderer init failed; UI-only present."
                               << LL_ENDL;
            w->shutdown();
            delete w;
        }
    }
    const bool doWorld = wantWorld && mImpl->mWorld && mImpl->mWorld->ready();
    // #1074 VR world frame: the session runs and LLVRCamera put this frame's head pose into the
    // camera. The world then renders into the eye-size target (copied into both eyes, mirrored
    // onto the window) instead of the window image. Desktop: mXrSession is false.
    uint32_t vrW = 0, vrH = 0;
    const bool vrWorld = mXrSession && doWorld && LLXrSession::instance().sessionRunning() &&
                         LLXrSession::instance().worldCameraApplied() && LLXrSession::instance().renderExtent(vrW, vrH) &&
                         mImpl->vrEnsureTarget(vrW, vrH);
    if (vrWorld != mImpl->mVrWorldLast)
    {
        // Into / out of VR world frames (session start / stop, login): the render size and the
        // view count change. Let the frames in flight (and their compute work) finish first;
        // once per transition.
        mImpl->mVrWorldLast = vrWorld;
        if (vrWorld && mImpl->mDevice.device()) vkDeviceWaitIdle(mImpl->mDevice.device());
        LL_INFOS("VR") << (vrWorld ? "the world renders for the headset now (eye size, head-tracked camera)"
                                   : "the world renders for the desktop window again")
                       << LL_ENDL;
    }
    if (doWorld)
    {
        // CPU-side: mirror the latest clean-store arrays into VK buffers (or keep
        // the prototype scene). Must run before command recording. The store fill
        // starts on a worker here and overlaps the UI buffering below; the join (+ the
        // main-thread texture resolve) follows it.
        { VK_HITCH_SCOPE("vk_syncBegin"); mImpl->mWorld->syncFromStoresBegin(); }
    }

    // If a UI render is supplied and the facade is ready, buffer the whole UI pass
    // now (CPU-side: gGL calls dispatch into gVK, filling batches). This must run
    // BEFORE command recording; gVK.recordFrame(cmd) lays the batches down below.
    const bool doUI = uiRender && mImpl->mFacadeReady;
    if (doUI)
    {
        VkExtent2D ext = mImpl->mSwapchain.extent();
        gVK.beginFrameToView(VK_NULL_HANDLE /*view set below*/,
                             mImpl->mSwapchain.format(), ext.width, ext.height);
        // LL's depth-tested 3D overlays get the world depth only when a world pass
        // records this frame (see beginWorldOverlay / mWorldOverlayHook).
        gVK.setWorldOverlayDepthFormat(doWorld ? mImpl->mWorld->worldDepthFormat()
                                               : VK_FORMAT_UNDEFINED);
        gVK.setWorldOverlayReverseZ(doWorld && mImpl->mWorld->worldReverseZ());
        { VK_HITCH_SCOPE("vk_uiBuild"); uiRender(); }
    }
    if (doWorld)
    {
        { VK_HITCH_SCOPE("vk_syncFromStores"); mImpl->mWorld->syncFromStores(); }
    }

    // The frame-in-flight slot for this frame. acquire() has already waited this
    // slot's in-flight fence, so its command buffer + the facade's per-slot VBO
    // are guaranteed retired and safe to re-record/overwrite.
    const uint32_t slot = mImpl->mSwapchain.frameSlot();

    uint32_t imageIndex = 0;
    bool needsRecreate = false;
    bool acquired;
    {
        const auto t0 = std::chrono::steady_clock::now();
        { VK_HITCH_SCOPE("vk_acquireWait"); acquired = mImpl->mSwapchain.acquire(mImpl->mDevice, &imageIndex, &needsRecreate); }
        LLVKHitch::timelineAcquire(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    // #1074 VR: no window image (the mirror window is minimised, resizing, or its acquire timed
    // out): the headset still gets this frame -- recorded and submitted on the slot's fence
    // with no window image, no UI pass and no present. Desktop: skip the frame as before.
    bool headless = false;
    if (!acquired)
    {
        if (needsRecreate)
        {
            mImpl->mNeedsRecreate = true;
        }
        if (!vrWorld || !mImpl->mSwapchain.beginFrameWithoutImage(mImpl->mDevice))
            return true; // benign; skip this frame
        headless = true;
    }
    // acquire() returned true but may still flag SUBOPTIMAL: render + present this
    // frame normally, then recreate the swapchain afterward.
    if (needsRecreate)
    {
        mImpl->mNeedsRecreate = true;
    }

    VkCommandBuffer cmd = mImpl->mCmds[slot];
    VkImage img = headless ? VK_NULL_HANDLE : mImpl->mSwapchain.image(imageIndex);

    // Record: UNDEFINED -> TRANSFER_DST, clear, (UI pass), -> PRESENT_SRC.
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
    // #790 breadcrumbs for the frame's head and tail: a device loss whose last finished
    // crumb is the previous frame's p_probe left everything from there to the next
    // frame's first world sub-mark (UI uploads, offscreen targets, the clear, the world
    // prologue, the UI replay, the present transition) without one.
    vkBreadcrumb(cmd, "f_begin");
    // ASYNC COMPUTE (roadmap phase 3a): world frames opt the renderer in. recordWorld may
    // then end `cmd` at its fork / join points and continue in its own command buffers
    // (asyncCurrentCmd); the swapchain clear goes into a separate command buffer
    // (asyncWsiCmd), the only submission that waits the acquire semaphore, and
    // asyncSubmitFrame submits everything in order on both queues.
    // #1075 VR world frames too: the XR eye copies, the depth layer copy and the mirror blit
    // are recorded into the frame's LAST graphics command buffer (asyncCurrentCmd below), the
    // tail asyncSubmitFrame submits last on the graphics queue -- after the joins of every
    // segment the world's output depends on -- so xrEndFrame (after the submit, main thread)
    // still follows all the eye work on the graphics queue the runtime was given (family /
    // index 0). The async queue is another VkQueue, never handed to the runtime, and every
    // submit stays on the main thread (VR_QUEUE_AUDIT option A: no lock). A window-less frame
    // has no acquire / render-finished semaphores (asyncSubmitFrame and the #697 drop path
    // take null ones) and no WSI command buffer.
    LLCleanRendererVK* asyncW = (doWorld && mImpl->mWorld->asyncAvailable()) ? mImpl->mWorld : nullptr;
    if (asyncW)
    {
        VK_HITCH_SCOPE("vk_asyncBegin");
        asyncW->setFrameSlot(slot);
        asyncW->asyncBeginFrame(cmd);
    }
    // #1074: the facade's per-frame recording (uploads, offscreen segments, world overlays)
    // only in frames whose UI pass is recorded too: gVK.recordFrame closes the facade's frame
    // (upload ring marks, deferred frees). A VR frame can have no UI pass (no window image, or
    // the window minimised); its uploads wait for the next frame that has one. Desktop
    // (mXrSession false, never headless): exactly as before.
    const bool uiRecords = doUI && !headless;
    const bool uiFrame = !mXrSession || uiRecords;
    // #1076 VR: a VR world frame with the UI wall shown also lays the UI into the wall's own
    // image (cleared transparent, so the world shows between the floaters). Desktop: vrWorld
    // is false, so this is false and nothing below that depends on it runs.
    const bool vrUi = vrWorld && uiRecords && LLXrSession::instance().uiImageWanted() &&
                      mImpl->vrEnsureUiTarget(mImpl->mSwapchain.extent());
    // UI texture uploads queued since the last frame (minimap, font atlas, previews):
    // recorded first, before any pass samples them (no per-upload submit-and-wait).
    if (mImpl->mFacadeReady && uiFrame) { VK_HITCH_SCOPE("vk_uiUploads"); gVK.recordUploads(cmd); vkBreadcrumb(cmd, "f_ui_uploads"); }
    // Offscreen render targets buffered this frame (#83: previews, visual-param
    // hints, the avatar appearance composite): before the world (which copies the
    // composite into its BOM slot) and the UI (which samples the previews).
    if (mImpl->mFacadeReady && uiRecords) { VK_HITCH_SCOPE("vk_uiOffscreen"); gVK.recordOffscreen(cmd, slot); vkBreadcrumb(cmd, "f_ui_offscreen"); }

    // Swapchain clear + transitions: `cmd`, or the async frame's WSI command buffer.
    // (#1075: a window-less VR frame has no window image to clear: no WSI command buffer)
    VkCommandBuffer wsiCmd = (asyncW && !headless) ? asyncW->asyncWsiCmd() : VK_NULL_HANDLE;
    VkCommandBuffer clearCmd = wsiCmd ? wsiCmd : cmd;

    // First scope = the acquire semaphore's wait stages (see the submit below), so the
    // layout transition + clear chain after the presentation engine releases the image.
    // (#1074: a window-less VR frame has no window image: every window step is skipped.)
    if (!headless)
    {
        imageBarrier(clearCmd, img,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT, 0,
                     VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    }

    // Background clear. A mild dark slate behind the UI (or the animated teal when
    // no UI is drawn, so a human can confirm the frame is live).
    float pulse = 0.35f + 0.10f * (float)((mImpl->mFrameCounter >> 3) & 1);
    VkClearColorValue clear{};
    clear.float32[0] = doUI ? 0.06f : 0.10f;
    clear.float32[1] = doUI ? 0.06f : pulse;
    clear.float32[2] = doUI ? 0.08f : 0.55f;
    if (wantWorld && !doWorld)
    {
        // The world is wanted but its pipelines are still compiling (normally hidden by
        // the startup progress hold, llstartup.cpp; this is the post-timeout fallback):
        // a neutral daytime-sky backdrop behind the UI instead of a black world.
        clear.float32[0] = 0.42f; clear.float32[1] = 0.58f; clear.float32[2] = 0.78f;
    }
    clear.float32[3] = 1.0f;
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (!headless)
    {
        vkCmdClearColorImage(clearCmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &clear, 1, &range);
    }
    vkBreadcrumb(clearCmd, "f_clear");

    if (doWorld || doUI)
    {
        VkExtent2D ext = mImpl->mSwapchain.extent();
        VkImageView view = headless ? VK_NULL_HANDLE : mImpl->mSwapchain.imageView(imageIndex);

        // TRANSFER_DST -> COLOR_ATTACHMENT so both the world pass and the UI pass
        // render into this image view with the clear preserved (LOAD). (Async frames: the
        // world's later swap-image accesses chain on this barrier's COLOR_ATTACHMENT
        // second scope across the submission boundary -- same queue, submission order.)
        if (!headless)
        {
            imageBarrier(clearCmd, img,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);   // LOAD reads
        }
        if (wsiCmd && asyncW) asyncW->vkEndOrNoteOom(wsiCmd);   // #697 (OOM: the frame is dropped)
        else if (wsiCmd) VK_CHECK(vkEndCommandBuffer(wsiCmd));

        // #1074 VR world frame: the world's colour target is the eye-size VR target (cleared
        // here; the world LOADs it), not the window image. Its previous use (last frame's eye
        // copies and mirror blit, transfer reads) is ordered before by the first scope.
        VkImage worldImg = img;
        VkImageView worldView = view;
        VkExtent2D worldExt = ext;
        uint32_t worldLayers = 1;   // #1075: the VR target's layers (one per eye in stereo)
        if (vrWorld)
        {
            worldImg = mImpl->mVrTarget.image;
            worldLayers = std::max(1u, mImpl->mVrTarget.layers);
            // (stereo: the whole array -- the world's final pass writes one layer per view)
            worldView = worldLayers > 1u && mImpl->mVrTarget.arrayView ? mImpl->mVrTarget.arrayView : mImpl->mVrTarget.view;
            worldExt = mImpl->mVrTarget.extent;
            imageBarrier(cmd, worldImg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                         VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, worldLayers);
            VkClearColorValue black{};
            black.float32[3] = 1.0f;
            const VkImageSubresourceRange worldRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, worldLayers};
            vkCmdClearColorImage(cmd, worldImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &worldRange);
            imageBarrier(cmd, worldImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, worldLayers);
        }
        // #1076 VR: the UI wall image, cleared to transparent black (the UI's premultiplied
        // blend over it gives the coverage in alpha). Its previous use (last frame's copy into
        // the UI swapchain) is ordered before by the first scope.
        if (vrUi)
        {
            const VkImage uiT = mImpl->mVrUiTarget.image;
            imageBarrier(cmd, uiT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                         VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            VkClearColorValue clearUi{};   // (0, 0, 0, 0)
            vkCmdClearColorImage(cmd, uiT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearUi, 1, &range);
            imageBarrier(cmd, uiT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        }

        // WORLD FIRST: GPU-driven static-opaque geometry into the swapchain, with
        // the renderer's own depth buffer. Its dynamic-rendering scope LOADs the
        // color (keeps the clear) and CLEARs depth.
        if (doWorld)
        {
            VKDrawPush draw{}; VKCullPush cull{}; VKSkyPush sky{};
            VK_HITCH_SCOPE("vk_worldSetup");
            { VK_HITCH_SCOPE("ws_fillPushes"); fillWorldPushes(*world, draw, cull, sky); }

            mImpl->mWorld->setFrameSlot(slot);   // ring-index the instance staging copy
            { VK_HITCH_SCOPE("ws_specTick"); mImpl->mWorld->specFrameTick(); }   // setting specialization: bind / fall back at the frame boundary
            { VK_HITCH_SCOPE("ws_lights"); mImpl->mWorld->setLights(*world); }
            { VK_HITCH_SCOPE("ws_probes"); mImpl->mWorld->setProbes(*world); }
            { VK_HITCH_SCOPE("ws_shadows"); mImpl->mWorld->setShadows(*world); }
            { VK_HITCH_SCOPE("ws_mirror"); mImpl->mWorld->setMirror(*world); }
            { VK_HITCH_SCOPE("ws_temporal"); mImpl->mWorld->setTemporal(*world); }
            mImpl->mWorld->mHideSelf = world->hide_self;
            mImpl->mWorld->mHideSelfHead = world->hide_self_head;
            mImpl->mWorld->mOrthoView = world->ortho_view;   // <GMDX> orthographic view
            { VK_HITCH_SCOPE("ws_selection"); mImpl->mWorld->setSelection(*world); }
            { VK_HITCH_SCOPE("ws_pick"); mImpl->mWorld->setPick(*world); }   // raster (#702) + particle (#481) cursor pick rays
            { VK_HITCH_SCOPE("ws_acoustics"); mImpl->mWorld->setAcoustics(*world); }   // acoustics probe (#683)
            { VK_HITCH_SCOPE("ws_atmos"); mImpl->mWorld->setAtmosphere(*world); }
            mImpl->mWorld->setSwapImage(worldImg);   // SSR copies the lit scene out
            mImpl->mWorld->setSwapchainBytes(swapchainBytes(mImpl->mSwapchain));   // #407
            // World-overlay segment (property/ban lines, beacons, manipulators),
            // recorded against the world depth before the HUD pass clears it, then
            // the pre-HUD text layer (name tags, hover text, chat bubbles: #468) so
            // HUD attachments draw over in-world text. A no-UI "with HUD" snapshot
            // (capture point 2, right after the world) must not contain that text:
            // that frame leaves it to the UI replay, after the capture.
            LLCleanRendererVK* W = mImpl->mWorld;
            mImpl->mWorld->mWorldOverlayHook =
                [slot, uiRecords, W](VkCommandBuffer c, VkImageView color, VkImageView depth,
                                     VkImage depthImage, uint32_t w, uint32_t h)
                {
                    if (uiRecords)   // (#1074: = doUI on the desktop)
                        gVK.recordWorldOverlay(c, slot, color, depth, depthImage, w, h,
                                               W->capturePending() != 2u && !W->liveSnapPoint2Pending());
                };
            const float* hudViewProj = world->hud_enabled ? world->hud_view_proj : nullptr;
            if (mXrSession)
            {
                // #1076 VR: HUD attachments go onto the UI wall's image (laid out in window
                // pixels, like the UI), never into the eye-size world; no wall, no HUDs.
                // Desktop: mXrSession is false -- the HUD target is never written.
                LLCleanRendererVK::HudTarget ht;
                if (vrUi && hudViewProj && mImpl->vrEnsureHudDepth(ext, mImpl->mWorld->worldDepthFormat()))
                {
                    ht.color = mImpl->mVrUiTarget.view;
                    ht.depth = mImpl->mVrHudDepth.view;
                    ht.depthImage = mImpl->mVrHudDepth.image;
                    ht.width = ext.width;
                    ht.height = ext.height;
                }
                mImpl->mWorld->mHudTarget = ht;
                if (vrWorld && !ht.color) hudViewProj = nullptr;
            }
            // #1075: the layers worldView spans (the VR target's 2D_ARRAY view: one per eye; else 1)
            mImpl->mWorld->mOutputLayers = (vrWorld && worldLayers > 1u && worldView == mImpl->mVrTarget.arrayView)
                                         ? worldLayers : 1u;
            // #1075 VR: the lens' hidden area per eye (XR_KHR_visibility_mask), drawn into the eyes'
            // depth in a stereo world frame whose views are the located eyes. Desktop: never called.
            if (mXrSession)
            {
                const LLXrSession& xs = LLXrSession::instance();
                mImpl->mWorld->setVisibilityMask(xs.visibilityMaskGeneration(), xs.visibilityMask(0), xs.visibilityMask(1),
                                                 mImpl->mWorld->mOutputLayers > 1u && world->views_are_eyes);
            }
            mImpl->mWorld->recordWorld(cmd, worldView, worldExt.width, worldExt.height,
                                       draw, cull, sky,
                                       hudViewProj,
                                       world->hud_viewport);
            // Async compute: recordWorld may have ended `cmd`; continue in its last one.
            if (asyncW) cmd = asyncW->asyncCurrentCmd(cmd);
            // user snapshot, world + HUD (before the UI replay)
            mImpl->mWorld->recordCaptureAt(cmd, 2, worldExt.width, worldExt.height);
            mImpl->mWorld->recordLiveSnap(cmd, 2, worldExt.width, worldExt.height);   // #873
        }
        // #1074 VR: the eye-size world, aspect-fit onto the window (the mirror), before the UI.
        if (vrWorld) mImpl->vrRecordMirror(cmd, img, ext);

        // UI ON TOP: replay the buffered UI batches over the (cleared + world)
        // color. Its own dynamic-rendering scope also LOADs, so it composites.
        if (uiRecords)
        {
            VK_HITCH_SCOPE("vk_uiRecord");
            vkBreadcrumb(cmd, "f_ui");
            // #1076 VR: the same UI into the wall's image first (the batches stay for the
            // window's replay below), then ready for the copy into the UI swapchain.
            if (vrUi)
            {
                gVK.replayTo(cmd, slot, mImpl->mVrUiTarget.view);
                imageBarrier(cmd, mImpl->mVrUiTarget.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                             VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
                vkBreadcrumb(cmd, "f_ui_wall");
            }
            gVK.setFrameColorView(view);
            gVK.recordFrame(cmd, slot);
            vkBreadcrumb(cmd, "f_ui_done");
        }
        // user snapshot, full frame incl. UI (the swap image was set by the world pass;
        // #1074 in VR world frames: the eye-size world without the UI)
        if (doWorld)
        {
            mImpl->mWorld->recordCaptureAt(cmd, 3, worldExt.width, worldExt.height);
            mImpl->mWorld->recordLiveSnap(cmd, 3, worldExt.width, worldExt.height);   // #873
        }
        // #1073 VR: the headset's images for the XR frame begun at the top of this viewer frame,
        // in the frame's last graphics command buffer. Desktop: mXrSession false.
        // #1074: in VR world frames the world target (-> TRANSFER_SRC) is copied into both eyes;
        // otherwise (login, loading) the eyes are dark and the window shows as the panel.
        if (mXrSession)
        {
            if (vrWorld)
            {
                imageBarrier(cmd, worldImg, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT |
                                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                             VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT |
                                 VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                             VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, worldLayers);
            }
            // #1075: a stereo world (one layer per eye) and its depth for the depth layer.
            LLXrWorldDepth xrDepth;
            const bool haveDepth = vrWorld && worldLayers > 1u && mImpl->mWorld &&
                                   mImpl->mWorld->worldDepthForXr(xrDepth.image, xrDepth.format, xrDepth.layout,
                                                                  xrDepth.extent, xrDepth.layers, xrDepth.distAtZero,
                                                                  xrDepth.distAtOne);
            LLXrSession::instance().recordFrame(cmd, img, mImpl->mSwapchain.format(), ext,
                                                vrWorld ? worldImg : VK_NULL_HANDLE, mImpl->mSwapchain.format(), worldExt,
                                                vrUi ? mImpl->mVrUiTarget.image : VK_NULL_HANDLE, worldLayers,
                                                haveDepth ? &xrDepth : nullptr);
        }
        // <GMDX> instant replay: the finished frame, UI included (in world only)
        if (doWorld && !headless && !vkDeviceIsLost())   // (#1074 headless VR frames have no window image)
            GMDXReplay::captureFrame(cmd, img, mImpl->mSwapchain.format(), ext.width, ext.height, mImpl->mDevice);
        vkBreadcrumb(cmd, "f_present");

        if (!headless)
        {
            imageBarrier(cmd, img,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0);
        }
    }
    else
    {
        imageBarrier(cmd, img,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                     VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0);
    }

    // #697: out of memory ending / submitting the frame DROPS it (LLCleanRendererVK,
    // DROPPED FRAMES): an empty submission keeps the swapchain + fence contract and the
    // renderer re-streams next frame. Other failures stay fatal (VK_CHECK / device lost).
    LLCleanRendererVK::AsyncWsi wsi;
    // (#1074 window-less VR frame: no acquire to wait, no present to signal; the fence only.)
    wsi.acquire = headless ? VK_NULL_HANDLE : mImpl->mSwapchain.imageAvailableSemaphore();
    wsi.acquireStage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT;
    wsi.renderFinished = headless ? VK_NULL_HANDLE : mImpl->mSwapchain.renderFinishedSemaphore(imageIndex);
    wsi.fence = mImpl->mSwapchain.inFlightFence();
    LLCleanRendererVK* dropW = doWorld ? mImpl->mWorld : nullptr;   // (re-streams after a drop)
    const uint32_t dropsBefore = dropW ? dropW->framesDropped() : 0;   // <GMDX> instant replay
    bool endOom = false;
    if (asyncW) endOom = !asyncW->vkEndOrNoteOom(cmd);   // (asyncSubmitFrame drops the frame)
    else
    {
        const VkResult er = vkInjectOomTake() ? VK_ERROR_OUT_OF_HOST_MEMORY : vkEndCommandBuffer(cmd);
        if (vkIsOom(er)) endOom = true;
        else VK_CHECK(er);
    }

    // Async frame: every segment on both queues, in recording order (acquire wait on the
    // WSI command buffer, render-finished + fence on the last graphics submission).
    bool submitted = false;
    if (asyncW)
    {
        VK_HITCH_SCOPE("vk_submit");
        submitted = asyncW->asyncSubmitFrame(mImpl->mDevice.queue(), wsi);
    }
    if (!submitted && endOom)
    {
        // (non-async frame whose command buffer could not be ended)
        LLCleanRendererVK::submitEmptyFrame(mImpl->mDevice.queue(), wsi, true);
        if (dropW) dropW->onFrameDropped();
        else vkLowMemNote(VKLM_FRAME, true);
        submitted = true;
    }
    if (!submitted)
    {
    // Submit: wait on image-available, signal render-finished, fence in-flight.
    VkSemaphoreSubmitInfo waitSem{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    waitSem.semaphore = wsi.acquire;
    // The image's first use is the CLEAR (vkCmdClearColorImage) + its layout transition:
    // the wait must cover that stage too, not only attachment output.
    waitSem.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT;

    VkSemaphoreSubmitInfo signalSem{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    // Per-IMAGE render-finished semaphore: present(imageIndex) waits this exact one.
    signalSem.semaphore = wsi.renderFinished;
    signalSem.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkCommandBufferSubmitInfo cmdInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmdInfo.commandBuffer = cmd;

    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.waitSemaphoreInfoCount = headless ? 0u : 1u;   // (#1074 window-less VR frame: no semaphores)
    submit.pWaitSemaphoreInfos = headless ? nullptr : &waitSem;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmdInfo;
    submit.signalSemaphoreInfoCount = headless ? 0u : 1u;
    submit.pSignalSemaphoreInfos = headless ? nullptr : &signalSem;

    VkResult sr;
    { VK_HITCH_SCOPE("vk_submit");
      sr = vkInjectOomTake() ? VK_ERROR_OUT_OF_HOST_MEMORY
                             : vkQueueSubmit2(mImpl->mDevice.queue(), 1, &submit, mImpl->mSwapchain.inFlightFence()); }
    if (vkIsOom(sr))
    {
        // #697: nothing was submitted (a failed vkQueueSubmit2 has no effect): replace it
        // with the empty submission and drop the frame.
        LLCleanRendererVK::submitEmptyFrame(mImpl->mDevice.queue(), wsi, true);
        if (dropW) dropW->onFrameDropped();
        else vkLowMemNote(VKLM_FRAME, true);
    }
    else VK_CHECK(sr);
    }

    // #873 live snapshot: a shot whose last frame was recorded this frame waits on this
    // frame's fence (never a device idle). (A dropped frame already failed the shot.)
    if (doWorld) mImpl->mWorld->liveSnapSubmitted(wsi.fence);
    // <GMDX> instant replay: queue the encode, or forget the capture of a dropped frame (#697 paths)
    if (!vkDeviceIsLost()) GMDXReplay::afterSubmit(!dropW || dropW->framesDropped() == dropsBefore);
    LLVKHitch::timelineSubmitted();   // (frame timeline: frame start -> submit)
    // #1073 VR: release the XR images and xrEndFrame after the frame's submit (the runtime's
    // reads follow our work on the same queue), before the mirror present. Desktop: skipped.
    if (mXrSession) LLXrSession::instance().endFrame();
    bool presentNeedsRecreate = false;
    if (headless)
    {
        // #1074: nothing to present; the next frame takes the next slot as after a present.
        mImpl->mSwapchain.endFrameWithoutPresent();
    }
    else
    {
        const auto t0 = std::chrono::steady_clock::now();
        { VK_HITCH_SCOPE("vk_present"); mImpl->mSwapchain.present(mImpl->mDevice, imageIndex, &presentNeedsRecreate); }
        LLVKHitch::timelinePresent(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    if (presentNeedsRecreate)
    {
        mImpl->mNeedsRecreate = true;
    }

    ++mImpl->mFrameCounter;
    return true;
}

// #1074 VR: the OpenXR teardown at the viewer's exit. LLAppViewer::cleanup calls this while
// logging still runs (shutdown() below is not reached on a normal exit, which is why M0's
// teardown lines were missing): the session ends (xrRequestExitSession, xrEndSession,
// xrDestroySession, the swapchains and views), then the XrInstance is destroyed. The Vulkan
// device and instance the runtime created stay until process exit, as on the desktop; nothing
// of the runtime uses them after its session is gone. No-op without VR.
void LLVulkanContext::shutdownXr()
{
    if (!mXrHooked && !mXrSession) return;
    if (!mImpl || vkDeviceIsLost())
    {
        // #135: never touch a lost device; the runtime cleans up with the process.
        LL_WARNS("VR") << "VR teardown skipped: the graphics device was lost" << LL_ENDL;
        return;
    }
    LL_INFOS("VR") << "VR teardown at exit" << LL_ENDL;
    if (mImpl->mDevice.device()) vkDeviceWaitIdle(mImpl->mDevice.device());
    if (mXrSession)
    {
        LLXrSession::instance().shutdownSession();
        mXrSession = false;
        mXrPresentOverride = false;
    }
    mImpl->vrReleaseTarget(false);
    if (mXrHooked)
    {
        LLXrSession::instance().shutdownInstance();
        mXrHooked = false;
    }
    LL_INFOS("VR") << "VR teardown done" << LL_ENDL;
}

void LLVulkanContext::shutdown()
{
    vkSetPipelineProgressHook(nullptr, 0);   // #462: no saves through a dying device
    vkSetPipelineBatchHook(nullptr);         // #471
    vkSetPipelineMemProbe(nullptr);
    vkSetPipelineMemBudget(nullptr);
    vkSetPipelineEventHook(nullptr);         // #672
    s_pipeSaveDev = nullptr;
    if (!mImpl)
    {
        mInitialized = false;
        return;
    }
    if (vkDeviceIsLost())
    {
        // #135: never tear down on a lost device (some drivers crash or hang in vkDestroy*
        // after a TDR); everything is released at process exit.
        LL_WARNS("Vulkan") << "shutdown skipped: the graphics device was lost" << LL_ENDL;
        GMDXReplay::shutdown(true);   // <GMDX> join the save thread only
        return;
    }
    if (mImpl->mDevice.device())
    {
        vkDeviceWaitIdle(mImpl->mDevice.device());
    }
    // #1073 VR: the session, its swapchains and image views go before the device.
    if (mXrSession)
    {
        LLXrSession::instance().shutdownSession();
        mXrSession = false;
        mXrPresentOverride = false;
    }
    GMDXReplay::shutdown(false);   // <GMDX> instant replay: its encoder objects go before the device
    mImpl->vrReleaseTarget(false);   // #1074 (device idle above; none outside VR)
    // EXPERIMENTAL Vulkan world: tear down the GPU-driven world renderer first.
    if (mImpl->mWorld)
    {
        mImpl->mWorld->shutdown();
        delete mImpl->mWorld;
        mImpl->mWorld = nullptr;
    }
    // #870: after the world renderer released its RR feature, before the device goes.
    LLVKNeuralRecon::instance().shutdown();
    // EXPERIMENTAL Vulkan UI: uninstall + tear down the facade backend first.
    if (mImpl->mRenderBackend)
    {
        LLRender::setBackend(nullptr);
        delete mImpl->mRenderBackend;
        mImpl->mRenderBackend = nullptr;
    }
    if (mImpl->mFacadeReady)
    {
        gVK.shutdown();
        mImpl->mFacadeReady = false;
    }
#if LL_WINDOWS
    mImpl->fseRelease("shutdown");   // #919: before its swapchain goes
#endif
    mImpl->mSwapchain.destroy(mImpl->mDevice);
    if (mImpl->mSurface && mImpl->mDevice.instance())
    {
        // SDL created the surface; SDL owns the destroy on this Vulkan instance.
        SDL_Vulkan_DestroySurface(mImpl->mDevice.instance(), mImpl->mSurface,
                                  nullptr);
        mImpl->mSurface = VK_NULL_HANDLE;
    }
    if (LLFxRuntimeVK::instance().compileInDriver())
    {
        // An effect compile is still inside the driver on this device (it cannot be
        // interrupted; llfxruntimevk.h, COMPILE WATCHDOG). vkDestroyDevice under it is
        // undefined behaviour and may block on the compile: leave the device to the
        // process exit, as for a lost device.
        LL_WARNS("Vulkan") << "shutdown: an effect compile is still running in the driver; "
                           << "the device is left to the process exit" << LL_ENDL;
        mInitialized = false;
        return;
    }
    mImpl->mDevice.shutdown();
    // #1073 VR: the XrInstance after the VkInstance the runtime created for it.
    if (mXrHooked)
    {
        LLXrSession::instance().shutdownInstance();
        mXrHooked = false;
    }
    delete mImpl;
    mImpl = nullptr;
    mInitialized = false;
    LL_INFOS("Vulkan") << "Vulkan context shut down" << LL_ENDL;
}
