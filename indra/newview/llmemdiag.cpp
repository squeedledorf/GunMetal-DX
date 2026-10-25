/**
 * @file llmemdiag.cpp
 * @brief Crash / disconnect / memory-failure evidence (#700). See llmemdiag.h.
 */
#include "llviewerprecompiledheaders.h"

#include "llmemdiag.h"

#include "llframetimer.h"
#include "llmemory.h"
#include "llmemorygovernor.h"
#include "vkgfx/llvulkancontext.h"
#include "vkgfx/vk_common.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <vector>

namespace LLMemDiag
{
namespace
{
constexpr size_t kFrameRing = 120;
constexpr size_t kCrossRing = 8;
constexpr size_t kMaxWaitSites = 24;
constexpr U64 MB = 1024ull * 1024ull;

const char* const kKindNames[AK_COUNT] = {"vk-host", "vk-device", "vk-buffer", "vk-pipeline",
                                          "ram-texture", "ram-mesh", "ram-other"};

std::atomic<U32> sRetried[AK_COUNT];
std::atomic<U32> sDropped[AK_COUNT];
std::atomic<U32> sFrameNumber{0};
std::atomic<bool> sLastWordsDone{false};

std::mutex sMutex;   // providers, wait table, rings
Provider sSoftFail, sExtra;

struct WaitSite { const char* site; U32 count; double ms; };
WaitSite sWaits[kMaxWaitSites];
size_t sNumWaits = 0;
std::string sLastWaits;

struct FrameRec { float ms; U32 frame; };
FrameRec sFrames[kFrameRing];
size_t sFrameHead = 0, sFrameCount = 0;

struct CrossRec { F64 t; U32 frame; const char* what; };
CrossRec sCross[kCrossRing];
size_t sCrossHead = 0, sCrossCount = 0;

F64 sessionSec() { return LLFrameTimer::getElapsedSeconds().value(); }

double steadySec()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string callProvider(Provider& p)
{
    Provider fn;
    { std::lock_guard<std::mutex> lk(sMutex); fn = p; }
    return fn ? fn() : std::string();
}
}   // namespace

void noteAllocFailure(AllocKind kind, bool dropped)
{
    if (kind >= AK_COUNT) kind = AK_RAM_OTHER;
    (dropped ? sDropped : sRetried)[kind].fetch_add(1, std::memory_order_relaxed);
}

void noteGpuWait(const char* site, double ms)
{
    if (!site) site = "?";
    std::lock_guard<std::mutex> lk(sMutex);
    for (size_t i = 0; i < sNumWaits; ++i)
    {
        if (sWaits[i].site == site || !std::strcmp(sWaits[i].site, site))
        {
            ++sWaits[i].count;
            sWaits[i].ms += ms;
            return;
        }
    }
    if (sNumWaits < kMaxWaitSites) sWaits[sNumWaits++] = { site, 1, ms };
}

void setSoftFailProvider(Provider fn) { std::lock_guard<std::mutex> lk(sMutex); sSoftFail = std::move(fn); }
void setExtraProvider(Provider fn)    { std::lock_guard<std::mutex> lk(sMutex); sExtra = std::move(fn); }

std::string allocFailText()
{
    std::string o;
    for (U32 k = 0; k < AK_COUNT; ++k)
    {
        const U32 r = sRetried[k].load(std::memory_order_relaxed);
        const U32 d = sDropped[k].load(std::memory_order_relaxed);
        if (!r && !d) continue;
        o += (o.empty() ? "" : " ") + std::string(kKindNames[k]) + " " + std::to_string(r) + "r/" + std::to_string(d) + "d";
    }
    return o.empty() ? std::string("none") : o;
}

// (#698) the RAM governor keeps its own failure / reserve / shed counts.
static bool ramAllocFailure()
{
    const LLMemoryGovernor::AllocFailStats a = LLMemoryGovernor::getAllocFailStats();
    if (a.reserve_released || a.sheds) return true;
    for (U32 i = 0; i < LLMemoryGovernor::ALLOC_KIND_COUNT; ++i)
        if (a.failures[i] || a.retried[i] || a.dropped[i]) return true;
    return false;
}

static bool anyAllocFailure()
{
    for (U32 k = 0; k < AK_COUNT; ++k)
        if (sRetried[k].load(std::memory_order_relaxed) || sDropped[k].load(std::memory_order_relaxed)) return true;
    return ramAllocFailure();
}

std::string statusLine()
{
    std::string o;
    {
        const LLMemoryGovernor::EState st = LLMemoryGovernor::getState();
        o += std::string("RAM ") + LLMemoryGovernor::stateName(st);
        o += " rss " + std::to_string(LLMemory::getCurrentRSS() / MB) + " MB";
        o += " avail " + std::to_string(LLMemoryGovernor::getAvailBytes() / MB) + "/" +
             std::to_string(LLMemoryGovernor::getTotalRAM() / MB) + " MB reserve " +
             std::to_string(LLMemoryGovernor::getReserveBytes() / MB) + " MB";
        o += " in-flight " + std::to_string(LLMemoryGovernor::getInFlightBytes() / MB) + " MB";
        o += " retained-allowance " + std::to_string(LLMemoryGovernor::getRetainedAllowance() / MB) + " MB";
        LLMemoryGovernor::SystemMemory sm;
        if (LLMemoryGovernor::readSystemMemory(sm) && sm.have_commit)
            o += " commit-avail " + std::to_string(sm.commit_avail / MB) + " MB";
    }
    {
        const LLVKVramSummary v = LLVulkanContext::instance().vramSummary();
        if (v.valid)
        {
            o += " | VRAM mode " + std::to_string(v.mode) + " rung " + std::to_string(v.rung) + "/" +
                 std::to_string(v.rungCount) + " step " + std::to_string(v.step) +
                 " usage " + std::to_string(v.usage / MB) + "/" + std::to_string(v.budget / MB) +
                 " MB (capacity " + std::to_string(v.capacity / MB) + ")";
        }
    }
    const std::string soft = callProvider(sSoftFail);
    if (!soft.empty()) o += " | vk-soft-fails " + soft;
    o += " | alloc-fail " + allocFailText();
    if (ramAllocFailure()) o += " | " + LLMemoryGovernor::allocFailString();   // (#698)
    const std::string extra = callProvider(sExtra);
    if (!extra.empty()) o += " | " + extra;
    return o;
}

void tick()
{
    static U32 s_key = ~0u;           // governor state, VRAM mode / rung / step
    static double s_next_poll = 0.0, s_last_log = 0.0, s_last_periodic = 0.0;
    const double now = steadySec();
    if (now < s_next_poll) return;
    s_next_poll = now + 0.25;

    const LLMemoryGovernor::EState gs = LLMemoryGovernor::getState();
    const LLVKVramSummary v = LLVulkanContext::instance().vramSummary();
    const U32 key = (U32)gs | ((v.mode & 0xF) << 4) | ((v.rung & 0xFF) << 8) | ((v.step & 0xFF) << 16);
    const bool calm = gs == LLMemoryGovernor::COMFORTABLE && (!v.valid || (v.mode == 0 && v.rung == 0));
    const bool changed = key != s_key;
    const bool periodic = (!calm && now - s_last_periodic >= 30.0) ||
                          (anyAllocFailure() && now - s_last_periodic >= 60.0);
    if (!periodic && !(changed && now - s_last_log >= 2.0)) return;   // changed + too soon: retried next poll
    if (s_key == ~0u && calm && !periodic)
    {
        s_key = key;   // first sample, nothing to say
        return;
    }
    s_key = key;
    s_last_log = s_last_periodic = now;
    LL_INFOS("MemDiag") << (changed ? "memory state change: " : "memory status: ") << statusLine() << LL_ENDL;
}

void frameDone(double frame_ms, U32 frame_number, const std::string& vk_waits)
{
    sFrameNumber.store(frame_number, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(sMutex);
    sFrames[sFrameHead] = { (float)frame_ms, frame_number };
    sFrameHead = (sFrameHead + 1) % kFrameRing;
    sFrameCount = std::min(sFrameCount + 1, kFrameRing);

    sLastWaits.clear();
    if (sNumWaits)
    {
        std::vector<WaitSite> w(sWaits, sWaits + sNumWaits);
        std::sort(w.begin(), w.end(), [](const WaitSite& a, const WaitSite& b) { return a.ms > b.ms; });
        char buf[160];
        sLastWaits = " | gpu-waits:";
        for (const WaitSite& s : w)
        {
            snprintf(buf, sizeof(buf), " %s x%u %.1fms", s.site, s.count, s.ms);
            sLastWaits += buf;
        }
        sNumWaits = 0;
    }
    if (!vk_waits.empty()) sLastWaits += (sLastWaits.empty() ? " | gpu-waits:" : ";") + vk_waits;   // (#699)
}

std::string lastFrameGpuWaits()
{
    std::lock_guard<std::mutex> lk(sMutex);
    return sLastWaits;
}

void noteRegionCrossing(const char* what)
{
    std::lock_guard<std::mutex> lk(sMutex);
    sCross[sCrossHead] = { sessionSec(), sFrameNumber.load(std::memory_order_relaxed), what };
    sCrossHead = (sCrossHead + 1) % kCrossRing;
    sCrossCount = std::min(sCrossCount + 1, kCrossRing);
}

void logPreDisconnect(const char* why)
{
    static double s_last = -1e9;   // one block per disconnect (several paths can fire)
    const double now = steadySec();
    if (now - s_last < 5.0) return;
    s_last = now;

    std::string frames, crossings;
    float worst = 0.f, sum = 0.f;
    size_t n = 0;
    {
        std::lock_guard<std::mutex> lk(sMutex);
        n = sFrameCount;
        for (size_t i = 0; i < n; ++i)
        {
            const FrameRec& f = sFrames[(sFrameHead + kFrameRing - n + i) % kFrameRing];
            worst = std::max(worst, f.ms);
            sum += f.ms;
            frames += (i ? " " : "") + std::to_string((int)(f.ms + 0.5f));
        }
        const F64 t = sessionSec();
        for (size_t i = 0; i < sCrossCount; ++i)
        {
            const CrossRec& c = sCross[(sCrossHead + kCrossRing - sCrossCount + i) % kCrossRing];
            char buf[96];
            snprintf(buf, sizeof(buf), "%s%s -%.1fs (frame %u)", i ? "; " : "", c.what, t - c.t, c.frame);
            crossings += buf;
        }
    }
    LL_WARNS("MemDiag") << "==== PRE-DISCONNECT (" << (why ? why : "?") << ") frame " << sFrameNumber.load()
                        << " session " << (int)sessionSec() << " s ====" << LL_ENDL;
    LL_WARNS("MemDiag") << "last " << n << " frames (ms, oldest first; avg "
                        << (n ? sum / (float)n : 0.f) << " worst " << worst << "): " << frames << LL_ENDL;
    LL_WARNS("MemDiag") << "recent region crossings (oldest first): "
                        << (crossings.empty() ? std::string("none this session") : crossings) << LL_ENDL;
    LL_WARNS("MemDiag") << "memory: " << statusLine() << LL_ENDL;
    LL_WARNS("MemDiag") << "==== END PRE-DISCONNECT ====" << LL_ENDL;
}

void lastWords(const char* what)
{
    if (sLastWordsDone.exchange(true)) return;   // the first fatal path wins
    LL_WARNS("MemDiag") << "==== LAST WORDS: " << (what ? what : "?") << " | frame " << sFrameNumber.load()
                        << " session " << (int)sessionSec() << " s ====" << LL_ENDL;
    LL_WARNS("MemDiag") << "memory: " << statusLine() << LL_ENDL;
    const char* marks[16];
    const uint32_t m = vkRecentMarks(marks, 16);
    std::string s;
    for (uint32_t i = 0; i < m; ++i) s += std::string(" ") + marks[i];
    LL_WARNS("MemDiag") << "last GPU passes recorded (oldest first):" << (m ? s : std::string(" none")) << LL_ENDL;
    LL_WARNS("MemDiag") << "==== END LAST WORDS ====" << LL_ENDL;
}

}   // namespace LLMemDiag
