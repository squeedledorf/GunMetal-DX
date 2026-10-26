#include "vk_pipeline.h"
#if LL_WINDOWS
#include "llwin32headers.h"
#elif LL_LINUX
#include <sys/syscall.h>
#include <unistd.h>
#endif
#if !LL_WINDOWS
#include <fcntl.h>      // #876 crash marker (open)
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(__linux__)
#  include <sys/resource.h>   // setpriority (prewarm workers below normal priority)
#  include <sys/syscall.h>
#  include <unistd.h>
#endif

// llcommon/llthread.h (#736: OS thread names for crash reports); declared here because
// this TU includes no viewer headers (llcommon is linked statically).
void ll_set_current_thread_name(const char* name);

namespace {
std::atomic<unsigned> s_compileThreads{0};
void (*s_progressFn)() = nullptr;
std::atomic<unsigned> s_progressEvery{0};
std::atomic<unsigned> s_progressCount{0};
std::mutex s_progressMutex;
// #672: also save when this long passed since the last save (0 = count only), and on
// demand (an out-of-memory result: keep what compiled before retrying).
std::atomic<unsigned> s_progressIntervalMs{0};
std::atomic<int64_t> s_progressLastMs{0};
int64_t progressNowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}
void progressSave()
{
    std::lock_guard<std::mutex> lock(s_progressMutex);   // one save at a time
    if (!s_progressFn) return;
    s_progressFn();
    s_progressLastMs = progressNowMs();
}
void progressTick()
{
    const unsigned every = s_progressEvery.load();
    if (!s_progressFn || !every) return;
    const bool byCount = (++s_progressCount % every) == 0;
    const unsigned interval = s_progressIntervalMs.load();
    const bool byTime = interval && progressNowMs() - s_progressLastMs.load() >= (int64_t)interval;
    if (!byCount && !byTime) return;
    progressSave();
}
void progressForce()
{
    if (s_progressFn && s_progressEvery.load()) progressSave();
}
// Startup progress (the login progress screen): pipelines queued to / finished by any
// batch since launch, and when the last one finished (a stalled driver is told apart
// from a slow one by this, not by a fixed timeout).
std::atomic<unsigned> s_queuedTotal{0};
std::atomic<unsigned> s_doneTotal{0};
std::atomic<int64_t> s_lastDoneMs{0};
}

void vkSetPipelineCompileThreads(unsigned n) { s_compileThreads = n; }
unsigned vkPipelineCompileThreads() { return s_compileThreads.load(); }
void vkSetPipelineProgressHook(void (*fn)(), unsigned every)
{
    s_progressFn = fn;
    s_progressEvery = every;
    s_progressLastMs = progressNowMs();
}
void vkSetPipelineProgressInterval(unsigned ms) { s_progressIntervalMs = ms; }
void vkPipelineCompileProgress(unsigned& done, unsigned& queued, double& idle_sec)
{
    done = s_doneTotal.load();
    queued = s_queuedTotal.load();
    const int64_t last = s_lastDoneMs.load();
    idle_sec = last ? (double)(progressNowMs() - last) / 1000.0 : 0.0;
}

// ---- #471: shader module identity + measurement hooks ----
namespace {
std::atomic<uint64_t (*)()> s_memProbe{nullptr};
std::atomic<void (*)(const VkPipelineBatchReport&)> s_batchHook{nullptr};
std::atomic<uint64_t (*)()> s_memBudget{nullptr};
std::atomic<uint64_t> s_memCap{0};   // vkSetPipelineMemCap
std::atomic<uint64_t> s_defaultCost{512ull << 20};
std::atomic<bool> s_cacheWarm{false};

uint64_t fnv1a(const void* p, size_t n, uint64_t h = 1469598103934665603ull)
{
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

struct ModuleInfo
{
    uint64_t hash = 0;    // FNV-1a of the SPIR-V; 0 = never noted
    uint32_t words = 0;
    std::string name;
};
// Handle -> identity. Bounded: cleared wholesale when full (modules are short-lived;
// queued pipelines keep a copy of what they need).
constexpr size_t kMaxNotedModules = 512;
std::mutex& modMutex() { static std::mutex m; return m; }
std::unordered_map<VkShaderModule, ModuleInfo>& modRegistry() { static std::unordered_map<VkShaderModule, ModuleInfo> r; return r; }

ModuleInfo moduleInfo(VkShaderModule m)
{
    if (!m) return {};
    std::lock_guard<std::mutex> lk(modMutex());
    auto it = modRegistry().find(m);
    return it == modRegistry().end() ? ModuleInfo{} : it->second;
}

void forgetModule(VkShaderModule m)
{
    std::lock_guard<std::mutex> lk(modMutex());
    modRegistry().erase(m);
}

std::string moduleLabel(VkShaderModule h, const ModuleInfo& m)
{
    if (!h) return "-";
    if (!m.name.empty()) return m.name;
    if (!m.hash) return "?";
    char b[24];
    std::snprintf(b, sizeof(b), "spv%08x", (unsigned)(m.hash & 0xFFFFFFFFu));
    return b;
}
}

// ---- #672: recoverable pipeline creation (see vk_pipeline.h) ----
namespace {
// Attempts that failed with nothing else compiling before a batch pipeline is given up;
// the hard bound on attempts per batch pipeline; synchronous retries.
constexpr unsigned kPipeAloneRetries = 4;
constexpr unsigned kPipeMaxAttempts = 12;
constexpr unsigned kSyncRetries = 5;
// #699: main-thread synchronous creates (vkSetPipelineMainThread)
constexpr unsigned kMainSyncRetries = 1;
constexpr double kMainOomCooldown = 5.0;
std::atomic<bool> s_haveMainThread{false};
std::thread::id s_mainThread{};
std::atomic<int64_t> s_mainOomUntilMs{0};   // steady-clock ms
std::atomic<unsigned> s_mainFastFails{0};
// Synchronous creates on the render thread slower than this were real compiles (a cache
// hit is well under a millisecond): logged as stragglers (vk_pipeline.h).
constexpr int64_t kSyncStragglerMs = 3;
std::atomic<unsigned> s_syncStragglers{0};
std::atomic<unsigned> s_simOom{0};
std::atomic<unsigned> s_failedCount{0};
std::atomic<unsigned> s_oomCount{0};
std::atomic<void (*)(bool, const char*)> s_eventHook{nullptr};

void pipeEvent(bool warn, const char* fmt, ...)
{
    char buf[640];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (void (*fn)(bool, const char*) = s_eventHook.load()) fn(warn, buf);
    else std::fprintf(stderr, "[vkgfx] %s\n", buf);
}

// Pipeline executable statistics (vk_pipeline.h): the entry points while capture is on.
std::atomic<PFN_vkGetPipelineExecutablePropertiesKHR> s_statsProps{nullptr};
std::atomic<PFN_vkGetPipelineExecutableStatisticsKHR> s_statsStats{nullptr};

// RenderVKDebugSimulatePipelineOOM: consumes one armed simulated failure.
// t_simOomLast: this thread's last create attempt was a simulated failure (its memory
// level says nothing about the driver: no budget ceiling from it).
thread_local bool t_simOomLast = false;
// Armed batch-only (the start-up arm): only the batch compile workers consume it, so the
// sync UI pipelines at the login screen are not the ones that fail.
std::atomic<bool> s_simBatchOnly{false};
thread_local bool t_inBatchWorker = false;
bool simOomTake()
{
    t_simOomLast = false;
    if (s_simBatchOnly.load(std::memory_order_relaxed) && !t_inBatchWorker) return false;
    unsigned k = s_simOom.load(std::memory_order_relaxed);
    while (k && !s_simOom.compare_exchange_weak(k, k - 1)) {}
    t_simOomLast = k != 0;
    return k != 0;
}

// Any error but out-of-memory: what VK_CHECK did (device lost -> the device-lost path,
// which returns when the viewer handles it; else the viewer's fatal handler / abort).
void pipelineFatal(const char* expr, VkResult r, const char* file, int line)
{
    std::fprintf(stderr, "[vktest] VK_CHECK failed: %s = %s (%d)\n  at %s:%d\n", expr, vkResultString(r), (int)r,
                 file, line);
    if (r == VK_ERROR_DEVICE_LOST || vkDeviceIsLost())
    {
        vkDeviceLostRaise(expr, r, nullptr, file, line);
        if (gVkDeviceLostHandler) return;
    }
    else if (gVkFatalHandler)
    {
        gVkFatalHandler(expr, r, file, line);
    }
    std::abort();
}
}

bool vkPipelineResultIsOom(VkResult r)
{
    return r == VK_ERROR_OUT_OF_HOST_MEMORY || r == VK_ERROR_OUT_OF_DEVICE_MEMORY;
}
unsigned vkPipelineFailedCount() { return s_failedCount.load(std::memory_order_relaxed); }
void vkSetPipelineMainThread()
{
    s_mainThread = std::this_thread::get_id();
    s_haveMainThread.store(true, std::memory_order_release);
}
unsigned vkPipelineMainFastFails() { return s_mainFastFails.load(std::memory_order_relaxed); }
unsigned vkPipelineSyncStragglers() { return s_syncStragglers.load(std::memory_order_relaxed); }

// ---- crash breadcrumb (vk_pipeline.h VkPipelineCompileCrumb) ----
namespace {
constexpr int kCrumbSlots = 64;
constexpr size_t kCrumbName = 112;
std::atomic<bool> s_crumbUsed[kCrumbSlots];
std::atomic<int64_t> s_crumbStartMs[kCrumbSlots];
char s_crumbName[kCrumbSlots][kCrumbName];
std::atomic<uint64_t> s_crumbThread[kCrumbSlots];   // the compiling OS thread (crash report: "while compiling")
uint64_t crumbThreadId()
{
#if LL_WINDOWS
    return (uint64_t)GetCurrentThreadId();
#elif LL_LINUX
    return (uint64_t)syscall(SYS_gettid);
#else
    return (uint64_t)std::hash<std::thread::id>()(std::this_thread::get_id());
#endif
}
int64_t crumbNowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}
// The recent-compile ring (vkPipelineRecentCompilesText): written when a crumb ends.
constexpr int kRecentSlots = 24;
std::atomic<uint32_t> s_recentNext{0};
std::atomic<int64_t> s_recentEndMs[kRecentSlots];
char s_recentName[kRecentSlots][kCrumbName];
}

VkPipelineCompileCrumb::VkPipelineCompileCrumb(const char* name)
{
    for (int i = 0; i < kCrumbSlots; ++i)
    {
        bool expect = false;
        if (s_crumbUsed[i].load(std::memory_order_relaxed) || !s_crumbUsed[i].compare_exchange_strong(expect, true))
            continue;
        std::snprintf(s_crumbName[i], kCrumbName, "%s", name ? name : "?");
        s_crumbThread[i].store(crumbThreadId(), std::memory_order_relaxed);
        s_crumbStartMs[i].store(crumbNowMs(), std::memory_order_release);
        slot = i;
        return;
    }
}

VkPipelineCompileCrumb::~VkPipelineCompileCrumb()
{
    if (slot < 0) return;
    {
        // Into the recent ring (a name may be torn if 24 more creates race past it: the
        // reader always terminates it).
        const uint32_t r = s_recentNext.fetch_add(1, std::memory_order_relaxed) % kRecentSlots;
        s_recentEndMs[r].store(0, std::memory_order_relaxed);
        std::memcpy(s_recentName[r], s_crumbName[slot], kCrumbName);
        s_recentName[r][kCrumbName - 1] = 0;
        s_recentEndMs[r].store(crumbNowMs(), std::memory_order_release);
    }
    s_crumbStartMs[slot].store(0, std::memory_order_relaxed);
    s_crumbUsed[slot].store(false, std::memory_order_release);
}

bool vkPipelineCompileOnThread(uint64_t os_thread_id, char* out, size_t size)
{
    if (!out || !size) return false;
    out[0] = 0;
    for (int i = 0; i < kCrumbSlots; ++i)
    {
        if (!s_crumbUsed[i].load(std::memory_order_acquire)) continue;
        if (s_crumbThread[i].load(std::memory_order_relaxed) != os_thread_id) continue;
        char name[kCrumbName];
        std::memcpy(name, s_crumbName[i], kCrumbName);
        name[kCrumbName - 1] = 0;
        std::snprintf(out, size, "%s", name);
        return true;
    }
    return false;
}

// ---- #876 / #890 crash marker (vk_pipeline.h): written from the crash handler ----
namespace {
#if LL_WINDOWS
wchar_t s_crashMarkerPath[1024];
std::atomic<bool> s_crashMarkerReady{false};
#else
char s_crashMarkerPath[1024];
std::atomic<bool> s_crashMarkerReady{false};
#endif
char s_crashDriverToken[64] = "-";   // #1064 (vkPipelineCrashMarkerSetDriver)
}

void vkPipelineCrashMarkerSetDriver(const char* token)
{
    if (!token || !token[0]) return;
    std::snprintf(s_crashDriverToken, sizeof(s_crashDriverToken), "%s", token);
    for (char* c = s_crashDriverToken; *c; ++c)
        if (*c == ' ' || *c == '\n' || *c == '\r' || *c == '\t') *c = '_';   // (one token, one line)
}

void vkPipelineCrashMarkerSetPath(const char* utf8_path)
{
    s_crashMarkerReady = false;
    if (!utf8_path || !utf8_path[0]) return;
#if LL_WINDOWS
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8_path, -1, s_crashMarkerPath, (int)(sizeof(s_crashMarkerPath) / sizeof(wchar_t)));
    if (n <= 0) return;
#else
    if (std::strlen(utf8_path) >= sizeof(s_crashMarkerPath)) return;
    std::memcpy(s_crashMarkerPath, utf8_path, std::strlen(utf8_path) + 1);
#endif
    s_crashMarkerReady = true;
}

bool vkPipelineCrashMarkerWrite(uint64_t os_thread_id)
{
    if (!s_crashMarkerReady.load()) return false;
    // "<unix time> <sessions left>\n<driver token>\n<pipeline name>\n" by hand (vk_pipeline.h):
    // no snprintf, no allocation -- static buffers and byte copies only.
    static char buf[48 + sizeof(s_crashDriverToken) + kCrumbName + 4];
    size_t len = 0;
    char digits[24];
    int nd = 0;
    unsigned long long t = (unsigned long long)std::time(nullptr);
    do { digits[nd++] = (char)('0' + t % 10); t /= 10; } while (t && nd < 20);
    while (nd) buf[len++] = digits[--nd];
    buf[len++] = ' ';
    buf[len++] = (char)('0' + (kVkPipelineCrashSafeSessions % 10));
    buf[len++] = '\n';
    // #1064: the driver it crashed on, and the pipeline the crashing thread was compiling (its
    // crumb; none on a driver-owned thread -- the line is then empty: no quarantine candidate).
    for (size_t k = 0; k + 1 < sizeof(s_crashDriverToken) && s_crashDriverToken[k]; ++k) buf[len++] = s_crashDriverToken[k];
    buf[len++] = '\n';
    for (int i = 0; i < kCrumbSlots; ++i)
    {
        if (!s_crumbUsed[i].load(std::memory_order_acquire)) continue;
        if (s_crumbThread[i].load(std::memory_order_relaxed) != os_thread_id) continue;
        for (size_t k = 0; k + 1 < kCrumbName && s_crumbName[i][k]; ++k)
        {
            const char c = s_crumbName[i][k];
            buf[len++] = (c == '\n' || c == '\r') ? ' ' : c;
        }
        break;
    }
    buf[len++] = '\n';
#if LL_WINDOWS
    HANDLE h = CreateFileW(s_crashMarkerPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(h, buf, (DWORD)len, &written, nullptr);
    CloseHandle(h);
    return ok && written == (DWORD)len;
#else
    const int fd = ::open(s_crashMarkerPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    const ssize_t w = ::write(fd, buf, len);
    ::close(fd);
    return w == (ssize_t)len;
#endif
}

bool vkPipelineCrashOnCompileThread(uint64_t os_thread_id)
{
    char name[kCrumbName];
    return vkPipelineCompileOnThread(os_thread_id, name, sizeof(name));
}

bool vkPipelineCrashOnCurrentThread() { return vkPipelineCrashOnCompileThread(crumbThreadId()); }

bool vkPipelineCrashMarkerWriteForCurrentThread() { return vkPipelineCrashMarkerWrite(crumbThreadId()); }

// ---- #1064 per-pipeline quarantine (vk_pipeline.h) ----
namespace {
std::mutex s_quarMutex;
std::vector<std::string> s_quarNames;      // (guarded by s_quarMutex)
std::vector<std::string> s_quarLogged;     // names whose skip was already logged (subset of s_quarNames)
std::string s_quarDriver;
std::atomic<bool> s_quarAny{false};        // fast path: nothing quarantined
std::mutex s_debugCrashMutex;
std::string s_debugCrashName;              // RenderVKDebugCrashCompilingPipeline (guarded)
std::atomic<bool> s_debugCrashAny{false};

// RenderVKDebugCrashCompilingPipeline: fault on purpose inside the crumb (the real crash path).
void debugCrashIfNamed(const char* crumb_name)
{
    if (!s_debugCrashAny.load(std::memory_order_relaxed) || !crumb_name) return;
    {
        std::lock_guard<std::mutex> lk(s_debugCrashMutex);
        if (s_debugCrashName != crumb_name) return;
    }
    std::fprintf(stderr, "[vkgfx] RenderVKDebugCrashCompilingPipeline: crashing on purpose while compiling %s\n", crumb_name);
    int* volatile target = nullptr;
    *target = 1064;
}
}

void vkSetPipelineQuarantine(const std::vector<std::string>& names, const std::string& driver_label)
{
    std::lock_guard<std::mutex> lk(s_quarMutex);
    s_quarNames = names;
    s_quarLogged.clear();
    s_quarDriver = driver_label;
    s_quarAny.store(!s_quarNames.empty(), std::memory_order_release);
}

bool vkPipelineQuarantined(const std::string& crumb_name)
{
    if (!s_quarAny.load(std::memory_order_acquire)) return false;
    std::string driver;
    {
        std::lock_guard<std::mutex> lk(s_quarMutex);
        if (std::find(s_quarNames.begin(), s_quarNames.end(), crumb_name) == s_quarNames.end()) return false;
        if (std::find(s_quarLogged.begin(), s_quarLogged.end(), crumb_name) != s_quarLogged.end()) return true;
        s_quarLogged.push_back(crumb_name);   // (bounded: a subset of s_quarNames)
        driver = s_quarDriver;
    }
    pipeEvent(true, "VKPipe: pipeline %s is QUARANTINED on this driver (%s): it crashed the driver's shader compiler"
              " twice, so it is not compiled (VK_NULL_HANDLE) and the feature using it is off (#1064)",
              crumb_name.c_str(), driver.empty() ? "?" : driver.c_str());
    return true;
}

void vkSetPipelineDebugCrashName(const std::string& crumb_name)
{
    std::lock_guard<std::mutex> lk(s_debugCrashMutex);
    s_debugCrashName = crumb_name;
    s_debugCrashAny.store(!crumb_name.empty(), std::memory_order_relaxed);
}

size_t vkPipelineRecentCompilesText(char* out, size_t size)
{
    if (!out || !size) return 0;
    out[0] = 0;
    const int64_t now = crumbNowMs();
    const uint32_t next = s_recentNext.load(std::memory_order_acquire);
    const uint32_t have = next < (uint32_t)kRecentSlots ? next : (uint32_t)kRecentSlots;
    int n = std::snprintf(out, size, "%s", have ? "" : "none");
    size_t used = n > 0 ? std::min((size_t)n, size - 1) : 0;
    for (uint32_t k = 1; k <= have && used + 1 < size; ++k)
    {
        const uint32_t r = (next - k) % kRecentSlots;   // newest first
        const int64_t end = s_recentEndMs[r].load(std::memory_order_acquire);
        if (!end) continue;   // (being replaced)
        char name[kCrumbName];
        std::memcpy(name, s_recentName[r], kCrumbName);
        name[kCrumbName - 1] = 0;
        n = std::snprintf(out + used, size - used, "%s%s (%.1f s ago)", used ? "; " : "", name,
                          (double)(now - end) / 1000.0);
        used += n > 0 ? std::min((size_t)n, size - used - 1) : 0;
    }
    return used;
}

size_t vkPipelineCompilesInFlightText(char* out, size_t size)
{
    if (!out || !size) return 0;
    out[0] = 0;
    size_t used = 0;
    int count = 0;
    const int64_t now = crumbNowMs();
    for (int i = 0; i < kCrumbSlots; ++i)
        if (s_crumbUsed[i].load(std::memory_order_acquire)) ++count;
    int n = std::snprintf(out, size, count ? "%d compiling:" : "none", count);
    used = n > 0 ? std::min((size_t)n, size - 1) : 0;
    for (int i = 0; i < kCrumbSlots && used + 1 < size; ++i)
    {
        if (!s_crumbUsed[i].load(std::memory_order_acquire)) continue;
        char name[kCrumbName];
        std::memcpy(name, s_crumbName[i], kCrumbName);
        name[kCrumbName - 1] = 0;   // (torn while replaced: still terminated)
        const int64_t st = s_crumbStartMs[i].load(std::memory_order_acquire);
        n = std::snprintf(out + used, size - used, " %s (%.1f s);", name, st ? (double)(now - st) / 1000.0 : 0.0);
        used += n > 0 ? std::min((size_t)n, size - used - 1) : 0;
    }
    return used;
}
unsigned vkPipelineOomCount() { return s_oomCount.load(std::memory_order_relaxed); }
void vkSimulatePipelineOom(unsigned k, bool batchOnly)
{
    s_simBatchOnly = batchOnly;
    s_simOom.store(k);
}
void vkSetPipelineEventHook(void (*fn)(bool, const char*)) { s_eventHook = fn; }

void vkSetPipelineStatsCapture(PFN_vkGetPipelineExecutablePropertiesKHR props,
                               PFN_vkGetPipelineExecutableStatisticsKHR stats)
{
    const bool on = props && stats;
    s_statsProps = on ? props : nullptr;
    s_statsStats = on ? stats : nullptr;
}
bool vkPipelineStatsCapture() { return s_statsProps.load() != nullptr && s_statsStats.load() != nullptr; }

namespace {
std::string statStages(VkShaderStageFlags s)
{
    static const struct { VkShaderStageFlagBits b; const char* n; } kNames[] = {
        {VK_SHADER_STAGE_VERTEX_BIT, "vert"}, {VK_SHADER_STAGE_FRAGMENT_BIT, "frag"},
        {VK_SHADER_STAGE_COMPUTE_BIT, "comp"}, {VK_SHADER_STAGE_TASK_BIT_EXT, "task"},
        {VK_SHADER_STAGE_MESH_BIT_EXT, "mesh"}, {VK_SHADER_STAGE_GEOMETRY_BIT, "geom"},
        {VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, "tesc"}, {VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, "tese"}};
    std::string r;
    for (const auto& k : kNames)
        if (s & k.b) { if (!r.empty()) r += "+"; r += k.n; }
    return r.empty() ? std::string("?") : r;
}
}

void vkLogPipelineStats(VkDevice device, VkPipeline pipeline, const char* name)
{
    const PFN_vkGetPipelineExecutablePropertiesKHR getProps = s_statsProps.load();
    const PFN_vkGetPipelineExecutableStatisticsKHR getStats = s_statsStats.load();
    if (!getProps || !getStats || !pipeline) return;
    void (*hook)(bool, const char*) = s_eventHook.load();
    auto emit = [&](const std::string& line) {
        if (hook) hook(false, line.c_str());
        else std::fprintf(stderr, "[vkgfx] %s\n", line.c_str());
    };
    VkPipelineInfoKHR pi{VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR};
    pi.pipeline = pipeline;
    uint32_t n = 0;
    if (getProps(device, &pi, &n, nullptr) != VK_SUCCESS || n == 0)
    {
        emit(std::string("VKPipeStats: ") + (name ? name : "pipeline") + ": no executables reported");
        return;
    }
    std::vector<VkPipelineExecutablePropertiesKHR> ex(n, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR});
    if (getProps(device, &pi, &n, ex.data()) != VK_SUCCESS) return;
    for (uint32_t e = 0; e < n; ++e)
    {
        VkPipelineExecutableInfoKHR ei{VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR};
        ei.pipeline = pipeline;
        ei.executableIndex = e;
        uint32_t sc = 0;
        std::vector<VkPipelineExecutableStatisticKHR> st;
        if (getStats(device, &ei, &sc, nullptr) == VK_SUCCESS && sc)
        {
            st.assign(sc, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR});
            if (getStats(device, &ei, &sc, st.data()) != VK_SUCCESS) st.clear();
            else st.resize(sc);
        }
        std::string line = std::string("VKPipeStats: ") + (name ? name : "pipeline") + " [" + statStages(ex[e].stages)
                         + " #" + std::to_string(e) + " \"" + ex[e].name + "\" subgroup " + std::to_string(ex[e].subgroupSize)
                         + "]";
        for (const VkPipelineExecutableStatisticKHR& s : st)
        {
            char v[64];
            switch (s.format)
            {
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR: std::snprintf(v, sizeof(v), "%s", s.value.b32 ? "true" : "false"); break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR:  std::snprintf(v, sizeof(v), "%lld", (long long)s.value.i64); break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR: std::snprintf(v, sizeof(v), "%llu", (unsigned long long)s.value.u64); break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR: std::snprintf(v, sizeof(v), "%.3f", s.value.f64); break;
            default: std::snprintf(v, sizeof(v), "?"); break;
            }
            line += std::string("; ") + s.name + "=" + v;
        }
        if (st.empty()) line += "; (no statistics)";
        emit(line);
    }
}

void vkSetPipelineMemProbe(uint64_t (*fn)()) { s_memProbe = fn; }
void vkSetPipelineBatchHook(void (*fn)(const VkPipelineBatchReport&)) { s_batchHook = fn; }
void vkSetPipelineMemBudget(uint64_t (*fn)()) { s_memBudget = fn; }
void vkSetPipelineMemCap(uint64_t bytes) { s_memCap = bytes; }
void vkSetPipelineDefaultCost(uint64_t bytes) { s_defaultCost = bytes ? bytes : (512ull << 20); }
void vkSetPipelineCacheWarm(bool warm) { s_cacheWarm = warm; }

void vkNoteShaderModule(VkShaderModule module, const uint32_t* code, size_t bytes, const char* name)
{
    if (!module || !code) return;
    ModuleInfo info;
    info.hash = fnv1a(code, bytes);
    if (!info.hash) info.hash = 1;
    info.words = (uint32_t)(bytes / sizeof(uint32_t));
    if (name) info.name = name;
    std::lock_guard<std::mutex> lk(modMutex());
    auto& r = modRegistry();
    if (r.size() >= kMaxNotedModules && r.find(module) == r.end()) r.clear();
    r[module] = std::move(info);
}

VkShaderModule loadShaderModule(VkDevice device, const std::string& spvPath) {
    FILE* f = std::fopen(spvPath.c_str(), "rb");
    if (!f) {
        std::fprintf(stderr, "[vktest] cannot open shader %s\n", spvPath.c_str());
        std::abort();
    }
    std::fseek(f, 0, SEEK_END);
    long len = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<char> code(len);
    if (std::fread(code.data(), 1, len, f) != (size_t)len) {
        std::fprintf(stderr, "[vktest] short read on %s\n", spvPath.c_str());
        std::abort();
    }
    std::fclose(f);

    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = (size_t)len;
    ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule mod;
    VK_CHECK(vkCreateShaderModule(device, &ci, nullptr, &mod));
    return mod;
}

namespace {
struct DynRaster
{
    VkCullModeFlags cull;
    VkFrontFace front;
    VkBool32 test, write;
    VkCompareOp compare;
};
std::mutex& dynMutex() { static std::mutex m; return m; }
std::unordered_map<VkPipeline, DynRaster>& dynRegistry() { static std::unordered_map<VkPipeline, DynRaster> r; return r; }
}

void vkForgetPipeline(VkPipeline pipeline)
{
    if (!pipeline) return;
    std::lock_guard<std::mutex> lk(dynMutex());
    dynRegistry().erase(pipeline);
}

bool pipelineHasDynamicRaster(VkPipeline pipeline)
{
    std::lock_guard<std::mutex> lk(dynMutex());
    return dynRegistry().count(pipeline) != 0;
}

void bindGraphicsPipeline(VkCommandBuffer cmd, VkPipeline pipeline)
{
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    DynRaster d;
    {
        std::lock_guard<std::mutex> lk(dynMutex());
        auto it = dynRegistry().find(pipeline);
        if (it == dynRegistry().end()) return;
        d = it->second;
    }
    vkCmdSetCullMode(cmd, d.cull);
    vkCmdSetFrontFace(cmd, d.front);
    vkCmdSetDepthTestEnable(cmd, d.test);
    vkCmdSetDepthWriteEnable(cmd, d.write);
    vkCmdSetDepthCompareOp(cmd, d.compare);
}

// One vkCreateGraphicsPipelines call (#672: no retry, no fatal; *out = VK_NULL_HANDLE
// unless it returns VK_SUCCESS).
static VkResult createGraphicsPipelineOnce(VkDevice device, const GraphicsPipelineDesc& desc,
                                           VkPipelineCache cache, VkPipeline* out);
// #672: a synchronous create with out-of-memory recovery (defined with the scheduler).
static VkPipeline createWithRecovery(const char* what, const std::function<VkResult(VkPipeline*)>& once,
                                     const char* expr, const char* file, int line);

VkPipeline createGraphicsPipeline(VkDevice device,
                                  const GraphicsPipelineDesc& desc) {
    const VkShaderModule front = desc.mesh != VK_NULL_HANDLE ? desc.mesh : desc.vert;   // #745 mesh tier
    const ModuleInfo vi = moduleInfo(front), fi = moduleInfo(desc.frag);
    const std::string what = "graphics pipeline (" + moduleLabel(front, vi) + "/" + moduleLabel(desc.frag, fi) + ")";
    VkPipelineCache cache = vkSharedPipelineCache();
    const VkPipeline made = createWithRecovery(
        what.c_str(), [&](VkPipeline* p) { return createGraphicsPipelineOnce(device, desc, cache, p); },
        "vkCreateGraphicsPipelines(device, cache, 1, &pci, nullptr, &pipeline)", __FILE__, __LINE__);
    if (made && vkPipelineStatsCapture()) vkLogPipelineStats(device, made, what.c_str());
    return made;
}

static VkResult createGraphicsPipelineOnce(VkDevice device, const GraphicsPipelineDesc& desc,
                                           VkPipelineCache cache, VkPipeline* out) {
    *out = VK_NULL_HANDLE;
    // REVERSED-Z (#188): a main-view pipeline (desc.reverseZ) depth-tests a buffer
    // cleared to 0 with near = 1: flip the ordering compares and the depth-bias sign
    // here, once, so every caller keeps writing its compare in conventional terms.
    GraphicsPipelineDesc rz;
    if (desc.reverseZ)
    {
        rz = desc;
        switch (rz.depthCompare)
        {
        case VK_COMPARE_OP_LESS:             rz.depthCompare = VK_COMPARE_OP_GREATER; break;
        case VK_COMPARE_OP_LESS_OR_EQUAL:    rz.depthCompare = VK_COMPARE_OP_GREATER_OR_EQUAL; break;
        case VK_COMPARE_OP_GREATER:          rz.depthCompare = VK_COMPARE_OP_LESS; break;
        case VK_COMPARE_OP_GREATER_OR_EQUAL: rz.depthCompare = VK_COMPARE_OP_LESS_OR_EQUAL; break;
        default: break;   // EQUAL / NOT_EQUAL / ALWAYS / NEVER are symmetric
        }
        rz.depthBiasConstant = -rz.depthBiasConstant;
        rz.depthBiasSlope = -rz.depthBiasSlope;
        rz.depthBiasClamp = -rz.depthBiasClamp;   // <GMDX>
        rz.reverseZ = false;
        return createGraphicsPipelineOnce(device, rz, cache, out);
    }
    if (simOomTake()) return VK_ERROR_OUT_OF_HOST_MEMORY;   // RenderVKDebugSimulatePipelineOOM
    // Stages: vertex (+ fragment), or for a mesh-shader pipeline (#745) [task] + mesh (+ fragment).
    VkPipelineShaderStageCreateInfo stages[3]{};
    uint32_t stageCount = 0;
    auto addStage = [&](VkShaderStageFlagBits st, VkShaderModule m) {
        VkPipelineShaderStageCreateInfo& s = stages[stageCount++];
        s.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        s.stage = st;
        s.module = m;
        s.pName = "main";
    };
    const bool meshPipe = desc.mesh != VK_NULL_HANDLE;
#ifdef VK_EXT_mesh_shader
    if (meshPipe)
    {
        if (desc.task != VK_NULL_HANDLE) addStage(VK_SHADER_STAGE_TASK_BIT_EXT, desc.task);
        addStage(VK_SHADER_STAGE_MESH_BIT_EXT, desc.mesh);
    }
    else
#else
    if (meshPipe) return VK_ERROR_FEATURE_NOT_PRESENT;   // headers without VK_EXT_mesh_shader
#endif
        addStage(VK_SHADER_STAGE_VERTEX_BIT, desc.vert);
    if (desc.frag != VK_NULL_HANDLE)
    {
        addStage(VK_SHADER_STAGE_FRAGMENT_BIT, desc.frag);
        stages[stageCount - 1].pSpecializationInfo = desc.fragSpec;
    }

    VkPipelineVertexInputStateCreateInfo vin{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vin.vertexBindingDescriptionCount = (uint32_t)desc.bindings.size();
    vin.pVertexBindingDescriptions = desc.bindings.data();
    vin.vertexAttributeDescriptionCount = (uint32_t)desc.attributes.size();
    vin.pVertexAttributeDescriptions = desc.attributes.data();

    VkPipelineInputAssemblyStateCreateInfo ia{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1; // dynamic

    VkPipelineRasterizationStateCreateInfo rs{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = desc.polygonMode;
    rs.cullMode = desc.cullMode;
    rs.frontFace = desc.frontFace;
    rs.lineWidth = desc.lineWidth;
    rs.depthClampEnable = desc.depthClamp ? VK_TRUE : VK_FALSE;
    if (desc.depthBiasConstant != 0.f || desc.depthBiasSlope != 0.f)
    {
        rs.depthBiasEnable = VK_TRUE;
        rs.depthBiasConstantFactor = desc.depthBiasConstant;
        rs.depthBiasSlopeFactor = desc.depthBiasSlope;
        rs.depthBiasClamp = desc.depthBiasClamp;   // <GMDX>
    }

    VkPipelineMultisampleStateCreateInfo ms{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = desc.depthTest ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = desc.depthWrite ? VK_TRUE : VK_FALSE;
    // LEQUAL, as LL (GL_LEQUAL): at equal depth the LATER draw wins, so coincident
    // layers resolve by pass/draw order (mask over opaque, stable within a bucket).
    ds.depthCompareOp = desc.depthCompare;

    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (desc.customBlend) {
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = desc.srcColor;
        cba.dstColorBlendFactor = desc.dstColor;
        cba.colorBlendOp = VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        cba.dstAlphaBlendFactor = desc.partAccum ? desc.dstColor : VK_BLEND_FACTOR_ONE;
        cba.alphaBlendOp = VK_BLEND_OP_ADD;
    } else if (desc.additive) {
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.colorBlendOp = VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.alphaBlendOp = VK_BLEND_OP_ADD;
    } else if (desc.blend) {
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.colorBlendOp = VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.alphaBlendOp = VK_BLEND_OP_ADD;
    } else {
        cba.blendEnable = VK_FALSE;
    }

    VkPipelineColorBlendStateCreateInfo cb{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cbas[GraphicsPipelineDesc::kMaxMrt] = {cba, cba, cba, cba};
    const bool has_glow = desc.glowFormat != VK_FORMAT_UNDEFINED;
    const bool has_react = has_glow && desc.reactFormat != VK_FORMAT_UNDEFINED;
    if (has_react)
    {
        // Reactive / water mask (F5): MAX of what the pass-2 writers output.
        cbas[2].blendEnable = VK_TRUE;
        cbas[2].srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        cbas[2].dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        cbas[2].colorBlendOp = VK_BLEND_OP_MAX;
        cbas[2].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cbas[2].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cbas[2].alphaBlendOp = VK_BLEND_OP_MAX;
        cbas[2].colorWriteMask = desc.reactWriteMask;
    }
    if (has_glow && !desc.glowWrite) cbas[1].colorWriteMask = 0;
    if (has_glow && desc.glowAdditive)
    {
        cbas[1].blendEnable = VK_TRUE;
        cbas[1].srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        cbas[1].dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        cbas[1].colorBlendOp = VK_BLEND_OP_ADD;
        cbas[1].srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        cbas[1].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cbas[1].alphaBlendOp = VK_BLEND_OP_ADD;
    }
    const bool mrt = desc.mrtCount > 0 && !desc.noColor;
    if (mrt)
    {
        // Plain MRT (G-buffer): every attachment written in full, no blending.
        VkPipelineColorBlendAttachmentState plain{};
        plain.blendEnable = VK_FALSE;
        plain.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        for (uint32_t i = 0; i < GraphicsPipelineDesc::kMaxMrt; ++i) cbas[i] = plain;
    }
    if (!desc.colorWrite)
        for (uint32_t i = 0; i < GraphicsPipelineDesc::kMaxMrt; ++i) cbas[i].colorWriteMask = 0;
    const uint32_t nColor = desc.noColor ? 0u
                          : mrt ? std::min<uint32_t>(desc.mrtCount, GraphicsPipelineDesc::kMaxMrt)
                          : (has_react ? 3u : (has_glow ? 2u : 1u));
    cb.attachmentCount = nColor;
    cb.pAttachments = cbas;

    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT,
                            VK_DYNAMIC_STATE_SCISSOR,
                            VK_DYNAMIC_STATE_CULL_MODE,
                            VK_DYNAMIC_STATE_FRONT_FACE,
                            VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
                            VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
                            VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
                            VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR};
    VkDynamicState dynList[8];
    uint32_t dynN = desc.dynamicDepthCull ? 7 : 2;
    for (uint32_t i = 0; i < dynN; ++i) dynList[i] = dyn[i];
    if (desc.dynamicShadingRate) dynList[dynN++] = VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR;
    VkPipelineDynamicStateCreateInfo dynInfo{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynInfo.dynamicStateCount = dynN;
    dynInfo.pDynamicStates = dynList;
    if (desc.dynamicDepthCull)
    {
        // Canonical static values (ignored; the dynamic ones apply) so pipelines that
        // differ only here hash identically in the driver / pipeline cache.
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        ds.depthTestEnable = VK_TRUE;
        ds.depthWriteEnable = VK_TRUE;
        ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    }

    VkPipelineRenderingCreateInfo rendering{
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    const VkFormat colorFormats[GraphicsPipelineDesc::kMaxMrt] = {
        mrt ? desc.mrtFormats[0] : desc.colorFormat, mrt ? desc.mrtFormats[1] : desc.glowFormat,
        mrt ? desc.mrtFormats[2] : desc.reactFormat, mrt ? desc.mrtFormats[3] : VK_FORMAT_UNDEFINED};
    rendering.colorAttachmentCount = nColor;
    rendering.pColorAttachmentFormats = colorFormats;
    if (desc.depthFormat != VK_FORMAT_UNDEFINED)
        rendering.depthAttachmentFormat = desc.depthFormat;

    VkGraphicsPipelineCreateInfo pci{
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pci.pNext = &rendering;
    pci.stageCount = stageCount;
    pci.pStages = stages;
    // (a mesh-shader pipeline has no vertex input / input assembly state)
    pci.pVertexInputState = meshPipe ? nullptr : &vin;
    pci.pInputAssemblyState = meshPipe ? nullptr : &ia;
    pci.pViewportState = &vp;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms;
    pci.pDepthStencilState = &ds;
    pci.pColorBlendState = &cb;
    pci.pDynamicState = &dynInfo;
    pci.layout = desc.layout;
    if (desc.fsrAttachment) pci.flags |= VK_PIPELINE_CREATE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR;
    if (vkPipelineStatsCapture()) pci.flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;   // RenderVKPipelineStats

    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult res = vkCreateGraphicsPipelines(device, cache, 1, &pci, nullptr, &pipeline);
    if (res != VK_SUCCESS) return res;   // (#672: the caller decides: retry / fatal)
    if (desc.dynamicDepthCull)
    {
        const DynRaster d{desc.cullMode, desc.frontFace,
                          (desc.depthFormat != VK_FORMAT_UNDEFINED && desc.depthTest) ? VK_TRUE : VK_FALSE,
                          (desc.depthFormat != VK_FORMAT_UNDEFINED && desc.depthWrite) ? VK_TRUE : VK_FALSE,
                          desc.depthCompare};
        std::lock_guard<std::mutex> lk(dynMutex());
        dynRegistry()[pipeline] = d;
    }
    *out = pipeline;
    return VK_SUCCESS;
}

VkPipeline createComputePipeline(VkDevice device, VkShaderModule comp,
                                 VkPipelineLayout layout) {
    return createComputePipeline(device, comp, layout, 0);
}

VkPipeline createComputePipeline(VkDevice device, VkShaderModule comp,
                                 VkPipelineLayout layout, VkPipelineShaderStageCreateFlags stageFlags) {
    return createComputePipeline(device, comp, layout, stageFlags, nullptr);
}

VkPipeline createComputePipeline(VkDevice device, VkShaderModule comp, VkPipelineLayout layout,
                                 VkPipelineShaderStageCreateFlags stageFlags, const VkSpecializationInfo* spec) {
    VkPipelineShaderStageCreateInfo stage{
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.flags = stageFlags;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = comp;
    stage.pName = "main";
    stage.pSpecializationInfo = spec;

    VkComputePipelineCreateInfo ci{
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = stage;
    ci.layout = layout;
    const bool capture = vkPipelineStatsCapture();
    if (capture) ci.flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;   // RenderVKPipelineStats

    const std::string what = "compute pipeline (" + moduleLabel(comp, moduleInfo(comp)) + ")";
    const VkPipeline made = createWithRecovery(
        what.c_str(),
        [&](VkPipeline* p) {
            *p = VK_NULL_HANDLE;
            if (simOomTake()) return VK_ERROR_OUT_OF_HOST_MEMORY;   // RenderVKDebugSimulatePipelineOOM
            return vkCreateComputePipelines(device, vkSharedPipelineCache(), 1, &ci, nullptr, p);
        },
        "vkCreateComputePipelines(device, vkSharedPipelineCache(), 1, &ci, nullptr, &pipeline)", __FILE__, __LINE__);
    if (made && capture) vkLogPipelineStats(device, made, what.c_str());
    return made;
}

VkPipeline vkCreatePipelineResilient(const char* what, VkPipelineCreateFn call, void* ctx, const char* expr,
                                     const char* file, int line)
{
    return createWithRecovery(
        what ? what : "pipeline",
        [&](VkPipeline* p) {
            *p = VK_NULL_HANDLE;
            if (simOomTake()) return VK_ERROR_OUT_OF_HOST_MEMORY;   // RenderVKDebugSimulatePipelineOOM
            return call(ctx, p);
        },
        expr, file, line);
}

// ---- Parallel pipeline batch (see vk_pipeline.h) ----
namespace {
struct QueuedPipeline
{
    GraphicsPipelineDesc desc;   // (compute: only `layout` is used)
    VkPipeline* out = nullptr;
    // COMPUTE entry (createComputePipelineInto): one compute stage `comp` with these
    // stage flags; its specialization data rides in specEntries / specData like a
    // graphics entry's fragment ones.
    bool compute = false;
    VkShaderModule comp = VK_NULL_HANDLE;
    VkPipelineShaderStageCreateFlags compFlags = 0;
    VkSpecializationInfo spec{};
    std::vector<VkSpecializationMapEntry> specEntries;
    std::vector<uint8_t> specData;
    // #471: report name + the modules' identity, copied at queue time (handles are
    // destroyed / reused before a later batch could look them up).
    std::string name;
    uint64_t vertHash = 0, fragHash = 0;
    uint32_t spirvWords = 0;
    uint64_t stableKey = 0;   // learned-cost key (set at flush)
};
struct PipelineBatch
{
    int depth = 0;
    std::vector<QueuedPipeline> queue;
    std::vector<VkShaderModule> modules;
    // DEFERRED GROUP (PipelineBatchDeferScope): queued apart, taken as its own job
    // (pipelineBatchTakeDeferred). deferredModules: modules its pipelines reference,
    // kept out of the main group's destroys.
    int deferDepth = 0;
    std::vector<QueuedPipeline> deferred;
    std::vector<VkShaderModule> deferredModules;
};
PipelineBatch& batch() { static PipelineBatch b; return b; }

// Moves the modules a pending deferred pipeline references out of `mods` into the
// deferred group's list (destroyed with the deferred job, not with the main group).
void keepDeferredModules(PipelineBatch& b, std::vector<VkShaderModule>& mods)
{
    if (b.deferred.empty()) return;
    auto used = [&](VkShaderModule m) {
        for (const QueuedPipeline& q : b.deferred)
            if (q.desc.vert == m || q.desc.frag == m || q.desc.task == m || q.desc.mesh == m || q.comp == m) return true;
        return false;
    };
    for (size_t i = 0; i < mods.size();)
    {
        if (used(mods[i]))
        {
            b.deferredModules.push_back(mods[i]);
            mods[i] = mods.back();
            mods.pop_back();
        }
        else ++i;
    }
}

void destroyModule(VkDevice device, VkShaderModule m)
{
    forgetModule(m);
    vkDestroyShaderModule(device, m, nullptr);
}
}

// One queue entry (deep copies: the caller's desc / spec info may not outlive the queue).
static void fillGraphicsQueued(QueuedPipeline& q, const GraphicsPipelineDesc& desc, VkPipeline* out, const char* name)
{
    q.desc = desc;
    q.out = out;
    if (desc.fragSpec)   // the caller's spec info may not outlive the queue: copy it
    {
        const VkSpecializationInfo& s = *desc.fragSpec;
        q.specEntries.assign(s.pMapEntries, s.pMapEntries + s.mapEntryCount);
        const uint8_t* d = static_cast<const uint8_t*>(s.pData);
        q.specData.assign(d, d + s.dataSize);
        q.desc.fragSpec = nullptr;   // re-pointed at flush (vector storage is stable then)
    }
    // (#745 mesh tier: the mesh module stands in for the vertex one, the task module's
    // identity is folded into the hash.)
    const VkShaderModule front = desc.mesh != VK_NULL_HANDLE ? desc.mesh : desc.vert;
    const ModuleInfo vi = moduleInfo(front), fi = moduleInfo(desc.frag);
    const ModuleInfo ti = moduleInfo(desc.task);
    q.vertHash = desc.task != VK_NULL_HANDLE ? (vi.hash ^ (ti.hash * 0x9E3779B97F4A7C15ull)) : vi.hash;
    q.fragHash = fi.hash;
    q.spirvWords = vi.words + fi.words + (desc.task != VK_NULL_HANDLE ? ti.words : 0u);
    q.name = std::string(name ? name : "pipeline") + " (" + moduleLabel(front, vi) + "/"
           + moduleLabel(desc.frag, fi) + ")";
}

static void fillComputeQueued(QueuedPipeline& q, VkShaderModule comp, VkPipelineLayout layout,
                              VkPipelineShaderStageCreateFlags stageFlags, const VkSpecializationInfo* spec,
                              VkPipeline* out, const char* name)
{
    q.compute = true;
    q.comp = comp;
    q.compFlags = stageFlags;
    q.desc.layout = layout;
    q.out = out;
    if (spec)   // (copied: the caller's spec info may not outlive the queue)
    {
        q.specEntries.assign(spec->pMapEntries, spec->pMapEntries + spec->mapEntryCount);
        const uint8_t* d = static_cast<const uint8_t*>(spec->pData);
        q.specData.assign(d, d + spec->dataSize);
    }
    const ModuleInfo ci = moduleInfo(comp);
    q.vertHash = ci.hash;
    q.fragHash = 0;
    q.spirvWords = ci.words;
    q.name = std::string(name ? name : "compute") + " (" + moduleLabel(comp, ci) + ")";
}

void createGraphicsPipelineInto(VkDevice device, const GraphicsPipelineDesc& desc, VkPipeline* out,
                                const char* name)
{
    PipelineBatch& b = batch();
    if (b.depth == 0)
    {
        if (s_quarAny.load(std::memory_order_acquire))   // #1064 (by the batch name too, not only the sync one)
        {
            QueuedPipeline q;
            fillGraphicsQueued(q, desc, out, name);
            if (vkPipelineQuarantined(q.name)) { *out = VK_NULL_HANDLE; return; }
        }
        *out = createGraphicsPipeline(device, desc);
        return;
    }
    *out = VK_NULL_HANDLE;
    std::vector<QueuedPipeline>& dst = b.deferDepth > 0 ? b.deferred : b.queue;
    dst.emplace_back();
    fillGraphicsQueued(dst.back(), desc, out, name);
    if (vkPipelineQuarantined(dst.back().name)) dst.pop_back();   // #1064: stays VK_NULL_HANDLE
}

void createComputePipelineInto(VkDevice device, VkShaderModule comp, VkPipelineLayout layout,
                               VkPipelineShaderStageCreateFlags stageFlags, const VkSpecializationInfo* spec,
                               VkPipeline* out, const char* name)
{
    PipelineBatch& b = batch();
    if (b.depth == 0)
    {
        if (s_quarAny.load(std::memory_order_acquire))   // #1064 (by the batch name too, not only the sync one)
        {
            QueuedPipeline q;
            fillComputeQueued(q, comp, layout, stageFlags, spec, out, name);
            if (vkPipelineQuarantined(q.name)) { *out = VK_NULL_HANDLE; return; }
        }
        *out = createComputePipeline(device, comp, layout, stageFlags, spec);
        return;
    }
    *out = VK_NULL_HANDLE;
    if (!comp || !layout) return;   // (what the synchronous create would return for it)
    std::vector<QueuedPipeline>& dst = b.deferDepth > 0 ? b.deferred : b.queue;
    dst.emplace_back();
    fillComputeQueued(dst.back(), comp, layout, stageFlags, spec, out, name);
    if (vkPipelineQuarantined(dst.back().name)) dst.pop_back();   // #1064: stays VK_NULL_HANDLE
}

void destroyShaderModuleDeferred(VkDevice device, VkShaderModule module, const VkAllocationCallbacks* alloc)
{
    PipelineBatch& b = batch();
    if (b.depth == 0 || alloc) { forgetModule(module); vkDestroyShaderModule(device, module, alloc); return; }
    b.modules.push_back(module);
}

namespace {
// Everything that reaches the driver except the shader / layout HANDLES and the
// dynamicDepthCull fields.
void appendPipelineState(std::string& k, const QueuedPipeline& q)
{
    const GraphicsPipelineDesc& d = q.desc;
    auto put = [&](const void* p, size_t n) { k.append(static_cast<const char*>(p), n); };
    auto pv = [&](auto v) { put(&v, sizeof(v)); };
    pv(q.compute); pv(q.compFlags);
    pv(d.colorFormat); pv(d.depthFormat);
    pv(d.blend); pv(d.additive); pv(d.glowFormat); pv(d.glowWrite); pv(d.polygonMode);
    pv(d.depthBiasConstant); pv(d.depthBiasSlope); pv(d.depthBiasClamp); pv(d.lineWidth); pv(d.noColor);   // <GMDX> clamp
    pv(d.depthClamp); pv(d.customBlend); pv(d.srcColor); pv(d.dstColor); pv(d.partAccum); pv(d.glowAdditive);
    pv(d.colorWrite); pv(d.dynamicDepthCull); pv(d.dynamicShadingRate); pv(d.reverseZ);
    pv(d.fsrAttachment); pv(d.reactFormat); pv(d.reactWriteMask);
    pv(d.mrtCount); for (uint32_t i = 0; i < GraphicsPipelineDesc::kMaxMrt; ++i) pv(d.mrtFormats[i]);
    if (!d.dynamicDepthCull)
    {
        pv(d.depthTest); pv(d.depthWrite); pv(d.depthCompare); pv(d.cullMode); pv(d.frontFace);
    }
    for (const auto& b : d.bindings)   { pv(b.binding); pv(b.stride); pv(b.inputRate); }
    for (const auto& a : d.attributes) { pv(a.location); pv(a.binding); pv(a.format); pv(a.offset); }
    for (const auto& e : q.specEntries) { pv(e.constantID); pv(e.offset); pv(e.size); }
    k.append(q.specData.begin(), q.specData.end());
}

// In-process dedup key: everything that reaches the driver except the dynamicDepthCull
// fields -- two queued pipelines with the same key compile to the same binary.
std::string pipelineKey(const QueuedPipeline& q)
{
    std::string k;
    auto pv = [&](auto v) { k.append(reinterpret_cast<const char*>(&v), sizeof(v)); };
    pv(q.desc.vert); pv(q.desc.frag); pv(q.desc.task); pv(q.desc.mesh); pv(q.comp); pv(q.desc.layout);
    appendPipelineState(k, q);
    return k;
}

// ---- #471 measurement: process-wide, shared by concurrent batches ----
// A compile in flight. Its memory is attributed from the probe's samples (sampleLocked).
struct Running
{
    uint64_t key = 0;          // learned-cost key (0 = do not learn: a dedup twin)
    uint64_t est = 0;          // estimated cost it was admitted with
    uint64_t startBytes = 0, maxBytes = 0;
    int64_t attrib = 0, attribPeak = 0;
    unsigned maxConc = 0;
};
struct BatchMem
{
    uint64_t peak = 0;
    unsigned maxConc = 0;
};
struct Sched
{
    std::mutex m;
    std::condition_variable cv;        // a compile finished
    std::vector<Running*> inflight;
    std::vector<BatchMem*> batches;
    uint64_t last = 0;                 // committed bytes at the previous sample
    bool haveLast = false;
    // #672: most compiles in flight at once (0 = no cap: threads + budget decide). Set by
    // an out-of-memory result (halved, floor 1) or a cold start (vkSetPipelineConcurrencyStart),
    // raised again by rampLocked while the learned costs show memory headroom -- never
    // above rampCeil (0 = the thread count), which an out-of-memory result lowers to below
    // the concurrency it failed at.
    unsigned cap = 0;
    unsigned startCap = 0;             // vkSetPipelineConcurrencyStart's value (re-armed per deferred job)
    unsigned rampCeil = 0;
    unsigned rampCredit = 0;           // real compiles completed since the last cap change
    // Pipelines queued and not yet taken by a worker, per priority, across every batch
    // (vk_pipeline.h PRIORITY): a lower-priority worker waits while a higher one is non-zero.
    unsigned pendingPrio[VKPIPE_PRIO_COUNT] = {};
    uint64_t peakCost = 0;             // largest attributed cost of a real compile this session
    uint64_t lastBudget = UINT64_MAX;  // the budget hook's latest value
    uint64_t oomCommitted = 0;         // committed bytes at the last out-of-memory result (0 = none)
};
Sched& sched() { static Sched s; return s; }

// #672, under s.m: an out-of-memory result from a compile that ran with up to `conc`
// compiles in flight (itself included): halve the cap (floor 1), stop ramping above
// what failed, and remember the committed memory it failed at (budget ceiling).
void onOomLocked(Sched& s, unsigned conc, const char* what, VkResult r)
{
    s_oomCount.fetch_add(1, std::memory_order_relaxed);
    if (s.last && !t_simOomLast) s.oomCommitted = s.oomCommitted ? std::min(s.oomCommitted, s.last) : s.last;
    s.rampCredit = 0;
    if (!t_simOomLast)   // (a simulated failure says nothing about the driver's limit)
    {
        const unsigned ceil = std::max(1u, std::max(1u, conc) - 1u);
        s.rampCeil = s.rampCeil ? std::min(s.rampCeil, ceil) : ceil;   // only ever lowered
    }
    const unsigned base = s.cap ? s.cap : std::max(1u, conc);
    const unsigned next = std::max(1u, std::min(s.rampCeil ? s.rampCeil : base, base / 2));
    const unsigned long long committedMB = (unsigned long long)(s.last >> 20);
    const long long budgetMB = s.lastBudget == UINT64_MAX ? -1 : (long long)(s.lastBudget >> 20);
    if (s.cap && next >= s.cap)
    {
        pipeEvent(true, "VKPipe: %s: %s with %u compile(s) in flight (committed %llu MB, budget %lld MB);"
                  " concurrency already at %u", what, vkResultString(r), conc, committedMB, budgetMB, s.cap);
        return;
    }
    pipeEvent(true, "VKPipe: %s: %s with %u compile(s) in flight (committed %llu MB, budget %lld MB); concurrency"
              " cap %u -> %u, ramp ceiling %u (#672)", what, vkResultString(r), conc, committedMB, budgetMB, base,
              next, s.rampCeil);
    s.cap = next;
}

// #672, under s.m: a real (cold, non-twin) compile finished with `attrib` bytes
// attributed. Additive increase: +1 after `cap` such compiles, while the latest budget
// still holds cap + 1 compiles at the session's largest measured cost.
void rampLocked(Sched& s, uint64_t attrib, unsigned threads)
{
    s.peakCost = std::max(s.peakCost, attrib);
    if (!s.cap) return;
    if (++s.rampCredit < s.cap) return;
    const unsigned ceil = s.rampCeil ? s.rampCeil : std::max(1u, threads);
    if (s.cap >= ceil) return;
    const uint64_t per = std::max<uint64_t>(s.peakCost, 16ull << 20);
    if (s.lastBudget != UINT64_MAX && s.lastBudget / per < (uint64_t)s.cap + 1) return;   // no headroom shown
    s.rampCredit = 0;
    const unsigned next = s.cap + 1;
    if (!s.rampCeil && next >= threads)
    {
        pipeEvent(false, "VKPipe: concurrency cap lifted (%u reached the thread count; largest compile %llu MB,"
                  " budget %lld MB)", next, (unsigned long long)(s.peakCost >> 20),
                  s.lastBudget == UINT64_MAX ? -1ll : (long long)(s.lastBudget >> 20));
        s.cap = 0;
        return;
    }
    pipeEvent(false, "VKPipe: concurrency cap %u -> %u (largest compile %llu MB, budget %lld MB)", s.cap, next,
              (unsigned long long)(s.peakCost >> 20),
              s.lastBudget == UINT64_MAX ? -1ll : (long long)(s.lastBudget >> 20));
    s.cap = next;
}

// #672, under s.m: the budget, also kept below the committed memory an out-of-memory
// result happened at (less a tenth): the driver failed there whatever the system said.
uint64_t ceilingBudgetLocked(const Sched& s, uint64_t budget)
{
    if (!s.oomCommitted || !s.last) return budget;
    const uint64_t ceil = s.oomCommitted - s.oomCommitted / 10;
    const uint64_t room = ceil > s.last ? ceil - s.last : 0;
    return std::min(budget, room);
}
}

void vkSetPipelineConcurrencyStart(unsigned cap)
{
    Sched& s = sched();
    std::lock_guard<std::mutex> lk(s.m);
    s.cap = cap;
    s.startCap = cap;
    s.rampCredit = 0;
}

void vkPipelineConcurrencyRearm()
{
    Sched& s = sched();
    std::lock_guard<std::mutex> lk(s.m);
    if (!s.startCap) return;   // no cold-start cap on this driver / cache
    const unsigned next = s.cap ? std::min(s.cap, s.startCap) : s.startCap;   // only ever lowers
    if (next != s.cap)
        pipeEvent(false, "VKPipe: deferred batch on a cold cache: concurrency cap %u -> %u (0 = none; ramps up"
                  " again as before)", s.cap, next);
    s.cap = next;
    s.rampCredit = 0;
}

unsigned vkPipelineConcurrencyCap()
{
    Sched& s = sched();
    std::lock_guard<std::mutex> lk(s.m);
    return s.cap;
}

static VkPipeline createWithRecovery(const char* what, const std::function<VkResult(VkPipeline*)>& once,
                                     const char* expr, const char* file, int line)
{
    // #699: the main thread never runs the multi-second ladder (vk_pipeline.h).
    const bool on_main = s_haveMainThread.load(std::memory_order_acquire)
                         && std::this_thread::get_id() == s_mainThread;
    auto nowMs = []() {
        return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    };
    // #1064: crashed the driver's compiler twice on this driver: never handed to it again.
    if (vkPipelineQuarantined(what ? what : "")) return VK_NULL_HANDLE;
    for (unsigned attempt = 1;; ++attempt)
    {
        VkPipeline p = VK_NULL_HANDLE;
        const int64_t t0 = on_main ? nowMs() : 0;
        VkResult r;
        {
            const VkPipelineCompileCrumb crumb(what);   // (#782 crash breadcrumb)
            debugCrashIfNamed(what);                     // (#1064 RenderVKDebugCrashCompilingPipeline)
            r = once(&p);
        }
        if (on_main)
        {
            // STRAGGLER (vk_pipeline.h): a real compile inside a frame -- make it visible.
            const int64_t ms = nowMs() - t0;
            if (ms >= kSyncStragglerMs)
            {
                s_syncStragglers.fetch_add(1, std::memory_order_relaxed);
                pipeEvent(false, "VKPipe: sync create %s %lld ms", what, (long long)ms);
            }
        }
        if (r == VK_SUCCESS) return p;
        if (!vkPipelineResultIsOom(r))
        {
            pipelineFatal(expr, r, file, line);   // returns only on a handled device loss
            return VK_NULL_HANDLE;
        }
        Sched& s = sched();
        {
            std::lock_guard<std::mutex> lk(s.m);
            onOomLocked(s, (unsigned)s.inflight.size() + 1u, what, r);
        }
        if (on_main && (attempt > kMainSyncRetries || nowMs() < s_mainOomUntilMs.load(std::memory_order_relaxed)))
        {
            // (not counted in vkPipelineFailedCount: nothing is disabled for the session)
            s_mainFastFails.fetch_add(1, std::memory_order_relaxed);
            s_mainOomUntilMs.store(nowMs() + (int64_t)(kMainOomCooldown * 1000.0), std::memory_order_relaxed);
            pipeEvent(true, "VKPipe: %s: %s on the render thread (attempt %u): VK_NULL_HANDLE for now -- not retried"
                      " synchronously (#699); its owner asks again later", what, vkResultString(r), attempt);
            return VK_NULL_HANDLE;
        }
        if (attempt > kSyncRetries)
        {
            s_failedCount.fetch_add(1, std::memory_order_relaxed);
            pipeEvent(true, "VKPipe: %s: still %s after %u attempts: GIVEN UP (VK_NULL_HANDLE); the feature using it is"
                      " disabled this session", what, vkResultString(r), attempt);
            return VK_NULL_HANDLE;
        }
        // Back off: let the compiles in flight (async batches) finish and free their memory.
        const auto backoff = on_main ? std::chrono::milliseconds(100)   // #699: bounded on the render thread
                                     : std::chrono::milliseconds(std::min(2000u, 100u << (attempt - 1)));
        pipeEvent(true, "VKPipe: %s: %s; retry %u/%u after the compiles in flight drain (up to %lld ms)", what,
                  vkResultString(r), attempt, kSyncRetries, (long long)backoff.count());
        {
            std::unique_lock<std::mutex> lk(s.m);
            s.cv.wait_for(lk, backoff, [&]() { return s.inflight.empty(); });
        }
        if (!on_main) std::this_thread::sleep_for(std::chrono::milliseconds(25u * attempt));
    }
}

namespace {

// Under s.m: one probe sample. The change since the previous sample is split equally
// between the compiles in flight (growth while nothing compiles is nobody's).
void sampleLocked(Sched& s)
{
    uint64_t (*probe)() = s_memProbe.load();
    if (!probe) return;
    const uint64_t now = probe();
    if (s.haveLast && !s.inflight.empty())
    {
        const int64_t share = ((int64_t)now - (int64_t)s.last) / (int64_t)s.inflight.size();
        for (Running* r : s.inflight)
        {
            r->attrib += share;
            r->attribPeak = std::max(r->attribPeak, r->attrib);
            r->maxBytes = std::max(r->maxBytes, now);
        }
    }
    s.last = now;
    s.haveLast = true;
    for (BatchMem* b : s.batches) b->peak = std::max(b->peak, now);
}

void beginRunningLocked(Sched& s, Running& r)
{
    sampleLocked(s);
    r.startBytes = r.maxBytes = s.last;
    s.inflight.push_back(&r);
    const unsigned n = (unsigned)s.inflight.size();
    for (Running* o : s.inflight) o->maxConc = std::max(o->maxConc, n);
    for (BatchMem* b : s.batches) b->maxConc = std::max(b->maxConc, n);
}

// Samples BEFORE removing it, so the memory the driver releases at the end of the
// compile lands on the compile that releases it.
void endRunningLocked(Sched& s, Running& r)
{
    sampleLocked(s);
    s.inflight.erase(std::find(s.inflight.begin(), s.inflight.end(), &r));
}

// Faster than this = served from the pipeline cache (no real compile). Also a hit: under
// a tenth of the key's learned cold compile time (a slow driver's cache lookup).
constexpr double kHitMs = 5.0;

// ---- #471 learned compile costs ----
constexpr uint64_t kMB = 1ull << 20;
constexpr uint64_t kHitCost = 16 * kMB;      // estimate of an expected cache hit / twin
constexpr uint64_t kMinCost = 16 * kMB;      // floor of a learned cold cost
constexpr double kUnknownMs = 500.0;         // ordering weight of a never-seen pipeline
constexpr double kDecay = 0.9;               // per new cold observation
constexpr size_t kMaxCostEntries = 1024;
constexpr uint32_t kKeepSessions = 8;

struct CostEntry
{
    uint64_t coldBytes = 0;    // 0 = no cold compile measured yet
    double coldMs = 0.0;       // 0 = no cold compile seen yet
    double lastMs = 0.0;
    bool lastHit = false;
    uint32_t lastSession = 0;
    std::string name;
};
struct CostTable
{
    std::mutex m;
    std::unordered_map<uint64_t, CostEntry> map;
    uint32_t session = 1;
};
CostTable& costs() { static CostTable t; return t; }

bool isHit(double ms, const CostEntry* e)
{
    return ms < kHitMs || (e && e->coldMs > 0.0 && ms < e->coldMs * 0.1);
}

// Estimated bytes + time of a queued pipeline. Twins of an already-compiled pipeline
// (`rest`) come from the cache: tiny.
void estimate(uint64_t key, bool twin, uint64_t& bytes, double& ms)
{
    if (twin) { bytes = kHitCost; ms = 1.0; return; }
    CostTable& t = costs();
    std::lock_guard<std::mutex> lk(t.m);
    auto it = t.map.find(key);
    if (it != t.map.end())
    {
        const CostEntry& e = it->second;
        if (s_cacheWarm.load() && e.lastHit) { bytes = kHitCost; ms = std::max(0.1, e.lastMs); return; }
        if (e.coldMs > 0.0)
        {
            bytes = e.coldBytes ? std::max(e.coldBytes, kMinCost) : s_defaultCost.load();
            ms = e.coldMs;
            return;
        }
    }
    bytes = s_defaultCost.load();
    ms = kUnknownMs;
}

// Drop keys unseen for kKeepSessions sessions; then the least recently seen over the cap.
void pruneLocked(CostTable& t)
{
    for (auto it = t.map.begin(); it != t.map.end();)
    {
        if (t.session - it->second.lastSession > kKeepSessions) it = t.map.erase(it);
        else ++it;
    }
    while (t.map.size() > kMaxCostEntries)
    {
        auto oldest = t.map.begin();
        for (auto it = t.map.begin(); it != t.map.end(); ++it)
            if (it->second.lastSession < oldest->second.lastSession) oldest = it;
        t.map.erase(oldest);
    }
}

// Records one compile of `key`; true if it was a cache hit. `estBytes`: what it was
// admitted with.
bool learn(uint64_t key, const std::string& name, double ms, uint64_t attribBytes, bool measured,
           uint64_t estBytes)
{
    CostTable& t = costs();
    std::lock_guard<std::mutex> lk(t.m);
    CostEntry& e = t.map[key];
    const bool hit = isHit(ms, &e);
    // Only a REAL compile may move the cold cost: a slow cache lookup that missed the hit
    // test (tens of ms on some drivers, ~no growth) must never decay a measured cold cost
    // -- a later release with this shader unchanged would then be admitted too cheaply.
    const bool real = e.coldMs <= 0.0 || ms >= e.coldMs * 0.5 || (measured && attribBytes >= e.coldBytes / 4);
    if (!hit && real)
    {
        e.coldMs = e.coldMs > 0.0 ? std::max(ms, e.coldMs * kDecay) : ms;
        if (measured)
            e.coldBytes = std::max(std::max(attribBytes, kMinCost), (uint64_t)((double)e.coldBytes * kDecay));
    }
    // Expected a warm-cache hit (tiny estimate) but it compiled for real with a large
    // footprint: the "warm" cache is not serving this batch (the driver dropped it), so
    // stop trusting it for the rest of the session -- later admissions use cold costs.
    if (!hit && measured && estBytes <= kHitCost && attribBytes >= 256 * kMB && s_cacheWarm.exchange(false))
        std::fprintf(stderr, "[vkgfx] pipeline batch: %s compiled for real (%.0f ms, %llu MB) despite the warm"
                     " pipeline cache; using cold costs from now on\n", name.c_str(), ms,
                     (unsigned long long)(attribBytes >> 20));
    e.lastMs = ms;
    e.lastHit = hit;
    e.lastSession = t.session;
    e.name = name;
    if (t.map.size() > kMaxCostEntries) pruneLocked(t);
    return hit;
}

// Report / file names: no whitespace (the text format ends a line with the name).
std::string flatName(const std::string& n)
{
    std::string o = n.substr(0, 96);
    for (char& c : o) if (c == ' ' || c == '\t' || c == '\n' || c == '\r') c = '_';
    return o;
}
}

size_t vkPipelineCostImport(const std::string& text)
{
    CostTable& t = costs();
    std::lock_guard<std::mutex> lk(t.m);
    t.map.clear();
    t.session = 1;
    size_t accepted = 0;
    size_t pos = 0;
    while (pos < text.size())
    {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        const std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        unsigned long long key = 0, cold = 0;
        double coldMs = 0.0, lastMs = 0.0;
        unsigned hit = 0, sess = 0;
        int nameAt = 0;
        if (std::sscanf(line.c_str(), "s %u", &sess) == 1 && line.size() > 1 && line[1] == ' ')
        {
            t.session = std::max(1u, std::min(sess, 0x7FFFFFFFu));
            continue;
        }
        if (std::sscanf(line.c_str(), "e %llx %llu %lf %lf %u %u %n", &key, &cold, &coldMs, &lastMs, &hit, &sess,
                        &nameAt) == 6 && nameAt > 0 && key != 0)
        {
            if (!(coldMs >= 0.0 && coldMs < 1e7) || !(lastMs >= 0.0 && lastMs < 1e7)) continue;
            CostEntry e;
            e.coldBytes = std::min<unsigned long long>(cold, 1ull << 40);
            e.coldMs = coldMs;
            e.lastMs = lastMs;
            e.lastHit = hit != 0;
            e.lastSession = sess;
            e.name = flatName(line.substr((size_t)nameAt));
            t.map[(uint64_t)key] = std::move(e);
            ++accepted;
        }
    }
    // A new session; entries "from the future" (a damaged counter) count as just seen.
    for (auto& kv : t.map) kv.second.lastSession = std::min(kv.second.lastSession, t.session);
    ++t.session;
    pruneLocked(t);
    return accepted;
}

std::string vkPipelineCostExport()
{
    // Compiles still in flight: their attributed peak so far is a lower bound of their
    // cold cost (a batch that crashes mid-compile still teaches the next launch).
    std::vector<std::pair<uint64_t, uint64_t>> partial;
    {
        Sched& s = sched();
        std::lock_guard<std::mutex> lk(s.m);
        for (const Running* r : s.inflight)
            if (r->key) partial.emplace_back(r->key, (uint64_t)std::max<int64_t>(0, r->attribPeak));
    }
    CostTable& t = costs();
    std::lock_guard<std::mutex> lk(t.m);
    std::string out;
    char buf[192];
    std::snprintf(buf, sizeof(buf), "s %u\n", t.session);
    out += buf;
    for (const auto& kv : t.map)
    {
        const CostEntry& e = kv.second;
        uint64_t cold = e.coldBytes;
        double coldMs = e.coldMs;
        bool hit = e.lastHit;
        for (const auto& p : partial)
            if (p.first == kv.first && p.second > cold)
            {
                cold = p.second;   // growing now: a real compile, not a cache hit
                coldMs = std::max(coldMs, kUnknownMs);
                hit = false;
            }
        std::snprintf(buf, sizeof(buf), "e %016llx %llu %.1f %.1f %u %u ", (unsigned long long)kv.first,
                      (unsigned long long)cold, coldMs, e.lastMs, hit ? 1u : 0u, e.lastSession);
        out += buf;
        out += e.name.empty() ? std::string("-") : e.name;
        out += '\n';
    }
    // In flight and never seen before: a provisional entry (never a hit).
    for (const auto& p : partial)
        if (!t.map.count(p.first) && p.second)
        {
            std::snprintf(buf, sizeof(buf), "e %016llx %llu %.1f 0.0 0 %u in-flight\n", (unsigned long long)p.first,
                          (unsigned long long)p.second, kUnknownMs, t.session);
            out += buf;
        }
    return out;
}

namespace {

void printReport(const VkPipelineBatchReport& r)
{
    std::fprintf(stderr, "[vkgfx] pipeline batch (%s): %zu pipelines (%zu compiled, %zu from cache in %.0f ms)"
                 " in %.0f ms (sum of compiles %.0f ms), %u threads, peak +%lld MB\n",
                 r.what, r.total, r.compiled, r.fromCache, r.restMs, r.wallMs, r.sumMs, r.threads,
                 (long long)(((int64_t)r.peakBytes - (int64_t)r.baseBytes) / (1 << 20)));
    if (r.oomRetries || r.failed)
        std::fprintf(stderr, "[vkgfx]   #672: %u out-of-memory retries, %u pipelines given up, concurrency cap %u\n",
                     r.oomRetries, r.failed, r.capEnd);
    std::vector<const VkPipelineCompileStat*> top;
    for (const VkPipelineCompileStat& s : r.items) top.push_back(&s);
    std::sort(top.begin(), top.end(), [](const VkPipelineCompileStat* a, const VkPipelineCompileStat* b) { return a->ms > b->ms; });
    for (size_t i = 0; i < top.size() && i < 5; ++i)
        std::fprintf(stderr, "[vkgfx]   %8.0f ms %6llu MB c%u %s\n", top[i]->ms,
                     (unsigned long long)(top[i]->attribBytes >> 20), top[i]->concurrency, top[i]->name.c_str());
}

// One queued compute pipeline (createComputePipelineInto) into `cache`.
VkResult createComputeQueuedOnce(VkDevice device, const QueuedPipeline& q, VkPipelineCache cache, VkPipeline* out)
{
    *out = VK_NULL_HANDLE;
    if (simOomTake()) return VK_ERROR_OUT_OF_HOST_MEMORY;   // RenderVKDebugSimulatePipelineOOM
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.flags = q.compFlags;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = q.comp;
    ci.stage.pName = "main";
    ci.stage.pSpecializationInfo = q.specEntries.empty() ? nullptr : &q.spec;
    ci.layout = q.desc.layout;
    if (vkPipelineStatsCapture()) ci.flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;   // RenderVKPipelineStats
    return vkCreateComputePipelines(device, cache, 1, &ci, nullptr, out);
}

// Linux: this thread (a prewarm compile worker) below normal priority -- nice +10 on its
// own tid, so the render thread and the asset threads win the cores. Elsewhere a no-op
// (the prewarm job is limited to one worker instead).
void lowerThisThreadPriority()
{
#if defined(__linux__)
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);
#endif
}

std::atomic<unsigned> s_gateQueued{0}, s_gateDone{0}, s_gateJobs{0};

// Compile `queue` on all cores, writing each handle to its `out`. Pipelines that differ
// only in dynamic state (see GraphicsPipelineDesc::dynamicDepthCull) compile once: the
// first of each group in parallel, the rest afterwards from a shared pipeline cache.
// opts: priority / worker cap / gate (vk_pipeline.h PipelineJobOpts; its cache field is
// the cacheOverride).
// blocking: a synchronous flush (the caller waits, e.g. on the render thread): it is not
// held back by the process-wide limit or by other batches' priorities.
void compileQueue(VkDevice device, std::vector<QueuedPipeline>& queue, const char* what,
                  VkPipelineCache cacheOverride = VK_NULL_HANDLE, const PipelineJobOpts& opts = PipelineJobOpts(),
                  bool blocking = false)
{
    if (queue.empty()) return;
    const auto t0 = std::chrono::steady_clock::now();
    for (QueuedPipeline& q : queue)
        if (!q.specEntries.empty())
        {
            q.spec.mapEntryCount = (uint32_t)q.specEntries.size();
            q.spec.pMapEntries = q.specEntries.data();
            q.spec.dataSize = q.specData.size();
            q.spec.pData = q.specData.data();
            if (!q.compute) q.desc.fragSpec = &q.spec;
        }
    const size_t n = queue.size();
    const unsigned prio = std::min<unsigned>(opts.prio, VKPIPE_PRIO_COUNT - 1u);
    s_queuedTotal += (unsigned)n;
    if (opts.gate) s_gateQueued += (unsigned)n;
    if (!s_lastDoneMs.load()) s_lastDoneMs = progressNowMs();
    std::atomic<size_t> doneHere{0};
    std::vector<size_t> first, rest;
    std::vector<size_t> firstOf(n);   // #672: the `first` whose compile a twin reuses
    {
        std::unordered_map<std::string, size_t> seen;
        for (size_t i = 0; i < n; ++i)
        {
            auto ins = seen.emplace(pipelineKey(queue[i]), i);
            (ins.second ? first : rest).push_back(i);
            firstOf[i] = ins.first->second;
        }
    }
    // Launch-stable key of the learned costs: SPIR-V content instead of handles.
    for (QueuedPipeline& q : queue)
    {
        std::string k;
        k.append(reinterpret_cast<const char*>(&q.vertHash), sizeof(q.vertHash));
        k.append(reinterpret_cast<const char*>(&q.fragHash), sizeof(q.fragHash));
        appendPipelineState(k, q);
        q.stableKey = fnv1a(k.data(), k.size());
        if (!q.stableKey) q.stableKey = 1;
    }
    // The process-wide (disk-persisted) cache, internally synchronized (no
    // EXTERNALLY_SYNCHRONIZED flag): shared by the workers. A warm disk cache turns
    // every compile below into a lookup. Without one (no device cache), a private
    // cache still lets the `rest` group reuse the `first` group's compiles.
    // (A job flushed with its own cache -- pipelineBatchFlushAsync(device, cache) -- uses it.)
    VkPipelineCache cache = cacheOverride ? cacheOverride : vkSharedPipelineCache();
    VkPipelineCache ownCache = VK_NULL_HANDLE;
    if (!cache)
    {
        VkPipelineCacheCreateInfo pci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        if (vkCreatePipelineCache(device, &pci, nullptr, &ownCache) != VK_SUCCESS) ownCache = VK_NULL_HANDLE;
        cache = ownCache;
    }

    // #471 measurement: register the batch and sample memory every ~15 ms while it runs.
    Sched& s = sched();
    BatchMem bm;
    uint64_t baseBytes = 0;
    {
        std::lock_guard<std::mutex> lk(s.m);
        s.batches.push_back(&bm);
        sampleLocked(s);
        baseBytes = bm.peak = s.last;
    }
    bool samplerStop = false;
    std::condition_variable samplerCv;
    std::thread sampler;
    if (s_memProbe.load())
        sampler = std::thread([&]() {
            ll_set_current_thread_name("VK pipeline mem");
            std::unique_lock<std::mutex> lk(s.m);
            while (!samplerStop)
            {
                sampleLocked(s);
                samplerCv.wait_for(lk, std::chrono::milliseconds(15));
            }
        });

    // #471 scheduler. Thread count: hardware threads - 1, hard-capped by the app's
    // override; the MEMORY BUDGET decides how many of them compile at once.
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    const unsigned limit = s_compileThreads.load();
    const bool measured = s_memProbe.load() != nullptr;
    std::vector<VkPipelineCompileStat> stats(n);
    unsigned threadsUsed = 0, overBudget = 0;
    unsigned oomRetries = 0, failedN = 0;   // #672 (under s.m)
    uint64_t budgetMin = UINT64_MAX;
    double waitMs = 0.0;
    using Clock = std::chrono::steady_clock;
    auto run = [&](const std::vector<size_t>& list, bool isRest) {
        if (list.empty()) return;
        // Pending work, most expensive first (longest-processing-time first, then by
        // memory): the big compiles start early and the cheap ones fill the gaps.
        // #672: `twin` = built from its first's cache entry (false when that first failed:
        // it then compiles cold); attempts / alone / notBefore drive the OOM requeue.
        struct Pending
        {
            size_t i;
            uint64_t est;
            double estMs;
            bool taken;
            bool twin;
            unsigned attempts;
            unsigned alone;
            Clock::time_point notBefore;
        };
        std::vector<Pending> pend;
        pend.reserve(list.size());
        for (size_t i : list)
        {
            Pending p{i, 0, 0.0, false, false, 0, 0, Clock::time_point{}};
            p.twin = isRest && *queue[firstOf[i]].out != VK_NULL_HANDLE;
            estimate(queue[i].stableKey, p.twin, p.est, p.estMs);
            pend.push_back(p);
        }
        std::stable_sort(pend.begin(), pend.end(), [](const Pending& a, const Pending& b) {
            return a.estMs != b.estMs ? a.estMs > b.estMs : a.est > b.est;
        });
        size_t untaken = pend.size();
        const unsigned autoThreads = std::max(1u, hw > 2 ? hw - 1 : hw);
        // The process-wide limit (every batch's compiles in flight together) and this job's own.
        const unsigned globalLimit = limit ? std::min(limit, autoThreads) : autoThreads;
        const unsigned jobLimit = opts.maxThreads ? std::min(opts.maxThreads, globalLimit) : globalLimit;
        const unsigned nthreads = (unsigned)std::min<size_t>(list.size(), jobLimit);
        threadsUsed = std::max(threadsUsed, nthreads);
        {
            std::lock_guard<std::mutex> lk(s.m);
            s.pendingPrio[prio] += (unsigned)pend.size();
        }
        auto worker = [&]() {
            // A batch compile worker (RenderVKDebugSimulatePipelineOOM's start-up arm).
            struct InBatch
            {
                InBatch() { t_inBatchWorker = true; }
                ~InBatch() { t_inBatchWorker = false; }
            } inBatch;
            if (opts.lowThreadPriority) lowerThisThreadPriority();
            for (;;)
            {
                // Outside the lock: the app's hook may sample the system.
                uint64_t (*budgetFn)() = s_memBudget.load();
                uint64_t budget = budgetFn ? budgetFn() : UINT64_MAX;
                const Clock::time_point nowT = Clock::now();
                std::unique_lock<std::mutex> lk(s.m);
                if (!untaken) return;
                // PRIORITY + PROCESS-WIDE LIMIT (vk_pipeline.h): wait while every batch's
                // compiles in flight fill the thread count, or while a higher-priority batch
                // still has pipelines nobody has taken (it gets the next free core).
                {
                    bool yield = !blocking && s.inflight.size() >= globalLimit;
                    for (unsigned p = 0; p < prio && !yield && !blocking; ++p) yield = s.pendingPrio[p] > 0;
                    if (yield)
                    {
                        const auto w0 = Clock::now();
                        s.cv.wait_for(lk, std::chrono::milliseconds(50));
                        waitMs += std::chrono::duration<double, std::milli>(Clock::now() - w0).count();
                        continue;
                    }
                }
                if (budgetFn)
                {
                    budget = ceilingBudgetLocked(s, budget);   // #672
                    s.lastBudget = budget;
                    budgetMin = std::min(budgetMin, budget);
                }
                // #672: after an out-of-memory result, at most s.cap compiles at once.
                if (s.cap && s.inflight.size() >= s.cap)
                {
                    const auto w0 = Clock::now();
                    s.cv.wait_for(lk, std::chrono::milliseconds(50));
                    waitMs += std::chrono::duration<double, std::milli>(Clock::now() - w0).count();
                    continue;
                }
                // What the compiles in flight (every batch) may still allocate, and what
                // they hold or may hold in all (the cap's measure).
                uint64_t owed = 0, held = 0;
                for (const Running* o : s.inflight)
                {
                    const uint64_t grown = (uint64_t)std::max<int64_t>(0, o->attrib);
                    owed += o->est > grown ? o->est - grown : 0;
                    held += std::max(o->est, grown);
                }
                // The compile cap (vkSetPipelineMemCap), as a budget in the same terms as
                // the live one: owed + estimate <= owed + room  <=>  estimate <= room.
                if (const uint64_t cap = s_memCap.load(std::memory_order_relaxed))
                {
                    const uint64_t room = cap > held ? cap - held : 0;
                    budget = std::min(budget, owed + room);
                    s.lastBudget = budget;
                    budgetMin = std::min(budgetMin, budget);
                }
                // The head (most expensive pending) first. Cheaper ones fill the room
                // left after keeping the head's share -- unless the head can never fit
                // the budget (it then runs through the progress guarantee), so fillers
                // cannot starve it until it is the last one left. Requeued pipelines
                // backing off (#672) are skipped until their time.
                Pending* pick = nullptr;
                Pending* head = nullptr;
                Clock::time_point soonest = Clock::time_point::max();
                for (Pending& p : pend)
                {
                    if (p.taken) continue;
                    if (p.notBefore > nowT) { soonest = std::min(soonest, p.notBefore); continue; }
                    if (!head)
                    {
                        head = &p;
                        if (budget == UINT64_MAX || owed + p.est <= budget) { pick = &p; break; }
                        continue;
                    }
                    const uint64_t keep = head->est <= budget ? head->est : 0;
                    if (owed + p.est + keep <= budget) { pick = &p; break; }
                }
                if (!pick)
                {
                    if (!s.inflight.empty())
                    {
                        // Over budget: wait for a compile to finish or the budget to grow.
                        const auto w0 = std::chrono::steady_clock::now();
                        s.cv.wait_for(lk, std::chrono::milliseconds(50));
                        waitMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - w0).count();
                        continue;
                    }
                    if (!head)
                    {
                        // Everything left is backing off after an out-of-memory result.
                        s.cv.wait_until(lk, soonest);
                        continue;
                    }
                    // Nothing in flight anywhere: start the head anyway (progress guarantee).
                    pick = head;
                    if (++overBudget <= 3)
                        std::fprintf(stderr, "[vkgfx] pipeline batch (%s): %s starts over the memory budget"
                                     " (estimate %llu MB, budget %llu MB): nothing else is compiling\n",
                                     what, queue[pick->i].name.c_str(), (unsigned long long)(pick->est >> 20),
                                     (unsigned long long)(budget >> 20));
                }
                pick->taken = true;
                --untaken;
                --s.pendingPrio[prio];
                const size_t i = pick->i;
                const bool twin = pick->twin;
                Running r;
                r.key = twin ? 0 : queue[i].stableKey;
                r.est = pick->est;
                beginRunningLocked(s, r);
                lk.unlock();

                const auto st = std::chrono::steady_clock::now();
                VkPipeline made = VK_NULL_HANDLE;
                VkResult res;
                {
                    const VkPipelineCompileCrumb crumb(queue[i].name.c_str());   // (#782 crash breadcrumb)
                    debugCrashIfNamed(queue[i].name.c_str());                     // (#1064 debug setting)
                    res = queue[i].compute ? createComputeQueuedOnce(device, queue[i], cache, &made)
                                           : createGraphicsPipelineOnce(device, queue[i].desc, cache, &made);
                }
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - st).count();
                lk.lock();
                endRunningLocked(s, r);
                // #672: out of memory is never fatal. Requeue with a backoff after halving the
                // concurrency cap; give up (VK_NULL_HANDLE) once it failed kPipeAloneRetries
                // times with nothing else compiling, or after kPipeMaxAttempts.
                const bool oom = vkPipelineResultIsOom(res);
                const unsigned attempts = ++pick->attempts;
                bool gaveUp = false;
                if (oom)
                {
                    ++oomRetries;
                    onOomLocked(s, r.maxConc, queue[i].name.c_str(), res);
                    if (r.maxConc <= 1) ++pick->alone;
                    if (pick->alone >= kPipeAloneRetries || attempts >= kPipeMaxAttempts)
                    {
                        gaveUp = true;
                        ++failedN;
                    }
                    else
                    {
                        const unsigned backoffMs = std::min(4000u, 250u << std::min(attempts - 1u, 4u));
                        pick->notBefore = Clock::now() + std::chrono::milliseconds(backoffMs);
                        pick->taken = false;
                        ++untaken;
                        ++s.pendingPrio[prio];
                        const unsigned alone = pick->alone;
                        lk.unlock();
                        s.cv.notify_all();
                        pipeEvent(true, "VKPipe: pipeline batch (%s): %s: %s (attempt %u, %u alone); requeued, retry in"
                                  " %u ms", what, queue[i].name.c_str(), vkResultString(res), attempts, alone, backoffMs);
                        progressForce();   // keep what compiled so far before retrying
                        continue;
                    }
                }
                lk.unlock();
                s.cv.notify_all();
                if (res != VK_SUCCESS && !oom)
                {
                    pipelineFatal(queue[i].compute ? "vkCreateComputePipelines(device, cache, 1, &ci, nullptr, &pipeline)"
                                                   : "vkCreateGraphicsPipelines(device, cache, 1, &pci, nullptr, &pipeline)", res,
                                  __FILE__, __LINE__);   // returns only on a handled device loss
                    gaveUp = true;
                }
                if (gaveUp && oom)
                {
                    s_failedCount.fetch_add(1, std::memory_order_relaxed);
                    pipeEvent(true, "VKPipe: pipeline batch (%s): %s: still %s after %u attempts: GIVEN UP"
                              " (VK_NULL_HANDLE); the feature using it is disabled this session", what,
                              queue[i].name.c_str(), vkResultString(res), attempts);
                }
                *queue[i].out = made;   // VK_NULL_HANDLE when given up
                VkPipelineCompileStat& o = stats[i];
                o.attempts = attempts;
                o.failed = gaveUp;
                o.name = queue[i].name;
                o.ms = ms;
                o.rawBytes = r.maxBytes > r.startBytes ? r.maxBytes - r.startBytes : 0;
                o.attribBytes = (uint64_t)std::max<int64_t>(0, r.attribPeak);
                o.estBytes = r.est;
                o.spirvWords = queue[i].spirvWords;
                o.concurrency = r.maxConc;
                o.rest = isRest;
                ++doneHere;
                ++s_doneTotal;
                if (opts.gate) ++s_gateDone;
                s_lastDoneMs = progressNowMs();
                if (gaveUp) continue;   // nothing compiled: nothing to learn or persist
                // RenderVKPipelineStats: this worker's own handle (never the queued `out`).
                const bool capture = vkPipelineStatsCapture();
                if (capture) vkLogPipelineStats(device, made, queue[i].name.c_str());
                // Twins share their first's key and always come from the cache: not learned.
                // (Nor a capture run: the statistics flag changes compile cost / cache hits.)
                o.hit = (twin || capture) ? ms < kHitMs
                             : learn(r.key, flatName(queue[i].name), ms, o.attribBytes, measured, r.est);
                if (!twin && !o.hit)
                {
                    // #672: a real compile: the cold-start / post-OOM cap may step up.
                    std::lock_guard<std::mutex> rl(s.m);
                    rampLocked(s, o.attribBytes, limit ? std::min(limit, autoThreads) : autoThreads);
                }
                progressTick();
            }
        };
        std::vector<std::thread> threads;
        for (unsigned t = 1; t < nthreads; ++t)
            threads.emplace_back([&worker]() { ll_set_current_thread_name("VK pipeline compile"); worker(); });
        worker();
        for (std::thread& t : threads) t.join();
    };
    run(first, false);
    const auto t1 = std::chrono::steady_clock::now();
    run(rest, true);
    if (doneHere.load() < n)   // every queued one is settled now
    {
        s_doneTotal += (unsigned)(n - doneHere.load());
        if (opts.gate) s_gateDone += (unsigned)(n - doneHere.load());
    }
    s_lastDoneMs = progressNowMs();
    if (ownCache) vkDestroyPipelineCache(device, ownCache, nullptr);

    if (sampler.joinable())
    {
        {
            std::lock_guard<std::mutex> lk(s.m);
            samplerStop = true;
        }
        samplerCv.notify_all();
        sampler.join();
    }
    {
        std::lock_guard<std::mutex> lk(s.m);
        sampleLocked(s);
        s.batches.erase(std::find(s.batches.begin(), s.batches.end(), &bm));
    }

    VkPipelineBatchReport rep;
    rep.what = what;
    rep.total = n;
    rep.compiled = first.size();
    rep.fromCache = rest.size();
    rep.wallMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    rep.restMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
    for (const VkPipelineCompileStat& st : stats) rep.sumMs += st.ms;
    rep.threads = threadsUsed;
    rep.maxConcurrency = bm.maxConc;
    rep.baseBytes = baseBytes;
    rep.peakBytes = bm.peak;
    rep.budgetMin = budgetMin;
    rep.overBudget = overBudget;
    rep.waitMs = waitMs;
    {
        std::lock_guard<std::mutex> lk(s.m);
        rep.oomRetries = oomRetries;
        rep.failed = failedN;
        rep.capEnd = s.cap;
    }
    rep.items = std::move(stats);
    printReport(rep);
    if (void (*hook)(const VkPipelineBatchReport&) = s_batchHook.load()) hook(rep);
}
}

void pipelineBatchFlush(VkDevice device)
{
    PipelineBatch& b = batch();
    keepDeferredModules(b, b.modules);
    {
        PipelineJobOpts o;
        o.prio = VKPIPE_PRIO_USER;   // (the caller is waiting)
        compileQueue(device, b.queue, "sync", VK_NULL_HANDLE, o, true);
    }
    b.queue.clear();
    for (VkShaderModule m : b.modules) destroyModule(device, m);
    b.modules.clear();
}

// ADMITTED SYNCHRONOUS CREATES (vk_pipeline.h): one pipeline through compileQueue as a
// one-entry, non-blocking batch -- the process-wide memory budget, the #672 concurrency
// cap / out-of-memory requeue, the priority order and the learned costs all apply, so a
// background std::async compile can never stack on top of a batch that already fills the
// budget (Windows AMD drivers allocate hundreds of MB per large compile). Blocks the
// CALLING thread until it is admitted and compiled: worker threads only.
VkPipeline createComputePipelineAdmitted(VkDevice device, VkShaderModule comp, VkPipelineLayout layout,
                                         VkPipelineShaderStageCreateFlags stageFlags,
                                         const VkSpecializationInfo* spec, const char* name, uint8_t prio)
{
    if (!comp || !layout) return VK_NULL_HANDLE;
    VkPipeline out = VK_NULL_HANDLE;
    std::vector<QueuedPipeline> q(1);
    fillComputeQueued(q[0], comp, layout, stageFlags, spec, &out, name);
    if (vkPipelineQuarantined(q[0].name)) return VK_NULL_HANDLE;   // #1064
    PipelineJobOpts o;
    o.prio = prio;
    o.maxThreads = 1;
    o.gate = false;
    o.what = "admitted";
    compileQueue(device, q, o.what, VK_NULL_HANDLE, o, false);
    return out;
}

VkPipeline createGraphicsPipelineAdmitted(VkDevice device, const GraphicsPipelineDesc& desc, const char* name,
                                          uint8_t prio)
{
    VkPipeline out = VK_NULL_HANDLE;
    std::vector<QueuedPipeline> q(1);
    fillGraphicsQueued(q[0], desc, &out, name);
    if (vkPipelineQuarantined(q[0].name)) return VK_NULL_HANDLE;   // #1064
    PipelineJobOpts o;
    o.prio = prio;
    o.maxThreads = 1;
    o.gate = false;
    o.what = "admitted";
    compileQueue(device, q, o.what, VK_NULL_HANDLE, o, false);
    return out;
}

struct PipelineJob
{
    VkDevice device = VK_NULL_HANDLE;
    std::vector<QueuedPipeline> queue;
    std::vector<VkShaderModule> modules;
    std::thread thread;
    std::atomic<bool> done{false};
    VkPipelineCache cache = VK_NULL_HANDLE;   // VK_NULL_HANDLE = the shared cache
    PipelineJobOpts opts;
    std::atomic<bool> gateCounted{false};     // in s_gateJobs until its compile finished / it was dropped
};

namespace {
// The job's compile finished (or it was dropped unstarted): it leaves the gate count.
void gateRelease(PipelineJob* job)
{
    if (job->gateCounted.exchange(false)) --s_gateJobs;
}
void jobRun(PipelineJob* job)
{
    ll_set_current_thread_name("VK pipeline batch");
    if (job->opts.lowThreadPriority) lowerThisThreadPriority();
    const char* what = job->opts.what ? job->opts.what : (job->cache ? "async, own cache" : "async");
    compileQueue(job->device, job->queue, what, job->cache, job->opts);
    gateRelease(job);   // (with the compile, before the owner polls the job)
    job->done.store(true, std::memory_order_release);
}
}

void vkPipelineGateProgress(unsigned& done, unsigned& queued, unsigned& jobsLive)
{
    done = s_gateDone.load();
    queued = s_gateQueued.load();
    jobsLive = s_gateJobs.load();
}

PipelineJob* pipelineBatchFlushAsync(VkDevice device)
{
    return pipelineBatchFlushAsync(device, PipelineJobOpts());
}

PipelineJob* pipelineBatchFlushAsync(VkDevice device, VkPipelineCache cache)
{
    PipelineJobOpts o;
    o.cache = cache;
    return pipelineBatchFlushAsync(device, o);
}

PipelineJob* pipelineBatchFlushAsync(VkDevice device, const PipelineJobOpts& opts)
{
    PipelineBatch& b = batch();
    if (b.queue.empty())
    {
        pipelineBatchFlush(device);   // just the module destroys
        return nullptr;
    }
    keepDeferredModules(b, b.modules);
    PipelineJob* job = new PipelineJob();
    job->device = device;
    job->queue.swap(b.queue);
    job->modules.swap(b.modules);
    job->cache = opts.cache;
    job->opts = opts;
    if (opts.gate) { job->gateCounted = true; ++s_gateJobs; }
    job->thread = std::thread([job]() { jobRun(job); });
    return job;
}

PipelineJob* pipelineBatchTakeDeferred(VkDevice device)
{
    PipelineBatch& b = batch();
    if (b.deferred.empty()) return nullptr;
    keepDeferredModules(b, b.modules);   // (modules still queued for the main group's destroys)
    PipelineJob* job = new PipelineJob();
    job->device = device;
    job->queue.swap(b.deferred);
    job->modules.swap(b.deferredModules);
    job->opts.what = "async, deferred";
    job->gateCounted = true;   // (the RT lit variants: part of the login gate)
    ++s_gateJobs;
    return job;   // not started: pipelineJobStart
}

void pipelineJobStart(PipelineJob* job)
{
    if (!job || job->thread.joinable() || job->done.load(std::memory_order_acquire)) return;
    // #672: a deferred group starts after the main batch, whose cheap compiles may have
    // ramped the cold-start cap all the way up; its (heavy, possibly never learned)
    // pipelines start from the cold-start cap again.
    vkPipelineConcurrencyRearm();
    job->thread = std::thread([job]() { jobRun(job); });
}

bool pipelineJobDone(const PipelineJob* job)
{
    return !job || job->done.load(std::memory_order_acquire);
}

void pipelineJobFinish(PipelineJob* job)
{
    if (!job) return;
    if (job->thread.joinable()) job->thread.join();
    gateRelease(job);   // (an unstarted job dropped here)
    for (VkShaderModule m : job->modules) destroyModule(job->device, m);
    delete job;
}

bool PipelineJobSlot::poll()
{
    if (!job || !pipelineJobDone(job)) return false;
    pipelineJobFinish(job);
    job = nullptr;
    return true;
}

void PipelineJobSlot::wait()
{
    if (!job) return;
    pipelineJobFinish(job);
    job = nullptr;
}

PipelineBatchScope::PipelineBatchScope(VkDevice device) : mDevice(device) { ++batch().depth; }
PipelineBatchDeferScope::PipelineBatchDeferScope() { ++batch().deferDepth; }
PipelineBatchDeferScope::~PipelineBatchDeferScope() { --batch().deferDepth; }
PipelineBatchScope::~PipelineBatchScope()
{
    pipelineBatchFlush(mDevice);
    --batch().depth;
}
