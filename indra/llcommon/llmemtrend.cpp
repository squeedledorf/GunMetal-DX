/**
 * @file llmemtrend.cpp
 * @brief Periodic leak report (#883). See llmemtrend.h.
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

#include "llmemtrend.h"

#include "llerror.h"
#include "llmemory.h"
#include "llmemorygovernor.h"
#include "workqueue.h"

#if LL_LINUX
#include <malloc.h>
#endif

#include <algorithm>
#include <cstdio>
#include <map>
#include <mutex>
#include <vector>

namespace LLMemTrend
{
namespace
{
constexpr U64 MB = 1024ull * 1024ull;

struct Entry
{
    Probe fn;
    Sample first, last;
    bool have_first = false;
};

struct State
{
    std::mutex mutex;                      // the probe table
    std::map<std::string, Entry> probes;   // bounded: one per registered name
    F64 next_report = -1.0;
    F64 first_t = -1.0, last_t = -1.0;
    U64 first_rss = 0, last_rss = 0;
    U32 reports = 0;
};

State& st()
{
    static State s;   // constructed on first use (a probe may register during static init)
    return s;
}

std::string mbText(U64 bytes)
{
    char buf[32];
    if (bytes >= 10 * MB) snprintf(buf, sizeof(buf), "%llu MB", (unsigned long long)(bytes / MB));
    else snprintf(buf, sizeof(buf), "%.1f MB", (double)bytes / (double)MB);
    return buf;
}

std::string signedMB(S64 bytes)
{
    char buf[32];
    const double mb = (double)bytes / (double)MB;
    if (mb >= 10.0 || mb <= -10.0) snprintf(buf, sizeof(buf), "%+lld MB", (long long)(bytes / (S64)MB));
    else snprintf(buf, sizeof(buf), "%+.1f MB", mb);
    return buf;
}

std::string build(F64 now, U32 top_n, bool advance, U64 trim_free_bytes = 0)
{
    State& s = st();
    const U64 rss = LLMemory::getCurrentRSS();

    struct Row { const std::string* name; Sample now; S64 d_bytes; S64 d_count; bool over; };
    std::vector<Row> rows;
    std::vector<std::pair<std::string, Probe>> fns;
    {
        std::lock_guard<std::mutex> lk(s.mutex);
        fns.reserve(s.probes.size());
        for (auto& kv : s.probes) fns.emplace_back(kv.first, kv.second.fn);
    }
    // Probes run outside the table lock (a probe may take its own subsystem's lock).
    std::vector<std::pair<std::string, Sample>> samples;
    samples.reserve(fns.size());
    for (auto& f : fns)
    {
        Sample smp;
        if (f.second) smp = f.second();
        samples.emplace_back(f.first, smp);
    }

    U64 tracked = 0;
    std::lock_guard<std::mutex> lk(s.mutex);
    rows.reserve(samples.size());
    for (auto& smp : samples)
    {
        auto it = s.probes.find(smp.first);
        if (it == s.probes.end()) continue;   // unregistered meanwhile
        Entry& e = it->second;
        if (!e.have_first && advance) { e.first = smp.second; e.last = smp.second; e.have_first = true; }
        const Sample& base = e.have_first ? e.first : smp.second;
        Row r{ &it->first, smp.second,
               (S64)smp.second.bytes - (S64)base.bytes, (S64)smp.second.count - (S64)base.count,
               smp.second.cap && smp.second.bytes > smp.second.cap };
        rows.push_back(r);
        tracked += smp.second.bytes;
        if (advance) e.last = smp.second;
    }

    std::string o;
    char buf[256];
    const bool baseline = s.first_t < 0.0;
    if (baseline && advance) { s.first_t = now; s.first_rss = rss; }
    const F64 span = s.first_t >= 0.0 ? now - s.first_t : 0.0;
    snprintf(buf, sizeof(buf), "MemTrend #%u: rss %s (peak %s)", s.reports + (advance ? 1 : 0), mbText(rss).c_str(),
             mbText(LLMemoryGovernor::getPeakRSS()).c_str());
    o += buf;
    if (s.last_t >= 0.0)
    {
        snprintf(buf, sizeof(buf), ", %s in %.0f min", signedMB((S64)rss - (S64)s.last_rss).c_str(), (now - s.last_t) / 60.0);
        o += buf;
    }
    if (!baseline && span > 60.0)
    {
        const double per_h = ((double)rss - (double)s.first_rss) / (double)MB / (span / 3600.0);
        snprintf(buf, sizeof(buf), ", %s since baseline %.1f h ago (%+.0f MB/h)",
                 signedMB((S64)rss - (S64)s.first_rss).c_str(), span / 3600.0, per_h);
        o += buf;
    }
    else if (baseline)
    {
        o += advance ? " [baseline]" : " [no baseline yet]";
    }
    snprintf(buf, sizeof(buf), " | tracked %s in %u probes", mbText(tracked).c_str(), (U32)rows.size());
    o += buf;
#if LL_LINUX && defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
    {
        // Heap in use vs free-but-retained (fragmentation / arena retention) vs mmapped
        // chunks: RSS growth with flat probes and a rising "free" is allocator retention,
        // rising "in-use" with flat probes is an untracked container, neither is memory
        // outside malloc (driver, file maps).
        const struct mallinfo2 mi = mallinfo2();
        snprintf(buf, sizeof(buf), " | heap arena %s in-use %s free %s mmap %s",
                 mbText(mi.arena).c_str(), mbText(mi.uordblks).c_str(), mbText(mi.fordblks).c_str(),
                 mbText(mi.hblkhd).c_str());
        o += buf;
        // Many subsystems free a little at a time (objects / avatars leaving, caches
        // evicting): glibc keeps it in its arenas and only the texture-drop trim hands any
        // back. Trim here when enough has piled up -- on a worker (it locks the arenas).
        if (trim_free_bytes && (U64)mi.fordblks > trim_free_bytes)
        {
            if (LL::WorkQueue::ptr_t q = LL::WorkQueue::getInstance("General"))
            {
                q->post([]() { malloc_trim(0); });
                o += " (trim posted)";
            }
        }
    }
#endif

    auto fmtRow = [&](const Row& r, bool growth)
    {
        std::string t = " " + *r.name + " ";
        if (r.now.bytes) t += mbText(r.now.bytes);
        else t += "-";
        snprintf(buf, sizeof(buf), " n=%llu", (unsigned long long)r.now.count);
        t += buf;
        if (r.now.cap) t += " cap " + mbText(r.now.cap) + (r.over ? " OVER" : "");
        if (r.now.peak) t += " peak " + mbText(r.now.peak);
        if (growth)
        {
            snprintf(buf, sizeof(buf), " (%s, n%+lld since baseline)", signedMB(r.d_bytes).c_str(), (long long)r.d_count);
            t += buf;
        }
        return t;
    };

    if (top_n == 0) top_n = 8;
    std::vector<const Row*> by_bytes;
    for (const Row& r : rows) by_bytes.push_back(&r);
    std::sort(by_bytes.begin(), by_bytes.end(), [](const Row* a, const Row* b)
    {
        if (a->now.bytes != b->now.bytes) return a->now.bytes > b->now.bytes;
        return a->now.count > b->now.count;
    });
    o += " | top:";
    for (size_t i = 0; i < by_bytes.size() && i < top_n; ++i) o += (i ? ";" : "") + fmtRow(*by_bytes[i], false);

    if (!baseline)
    {
        // Growers since the baseline: by bytes; probes without bytes rank by count growth
        // after the byte growers (a count-only container that keeps rising is the classic
        // insert-only map keyed by UUID / time).
        std::vector<const Row*> grow;
        for (const Row& r : rows)
            if (r.d_bytes > 0 || (r.now.bytes == 0 && r.d_count > 0)) grow.push_back(&r);
        std::sort(grow.begin(), grow.end(), [](const Row* a, const Row* b)
        {
            if (a->d_bytes != b->d_bytes) return a->d_bytes > b->d_bytes;
            return a->d_count > b->d_count;
        });
        o += " | growers:";
        if (grow.empty()) o += " none";
        for (size_t i = 0; i < grow.size() && i < top_n; ++i) o += (i ? ";" : "") + fmtRow(*grow[i], true);
    }
    // Over-cap probes are always named, wherever they rank.
    std::string over;
    for (const Row& r : rows)
        if (r.over) over += " " + *r.name;
    if (!over.empty()) o += " | OVER CAP:" + over;

    if (advance)
    {
        s.last_t = now;
        s.last_rss = rss;
        ++s.reports;
    }
    return o;
}
}   // namespace

std::string compactLine(U32 top_n)
{
    State& s = st();
    std::lock_guard<std::mutex> lk(s.mutex);
    if (s.reports == 0) return std::string();
    std::vector<std::pair<U64, const std::string*>> v;
    v.reserve(s.probes.size());
    for (const auto& kv : s.probes)
        if (kv.second.have_first && kv.second.last.bytes) v.emplace_back(kv.second.last.bytes, &kv.first);
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::string o = "memtrend";
    for (size_t i = 0; i < v.size() && i < top_n; ++i) o += (i ? ", " : " ") + *v[i].second + " " + mbText(v[i].first);
    return o;
}

void registerProbe(const std::string& name, Probe fn)
{
    State& s = st();
    std::lock_guard<std::mutex> lk(s.mutex);
    Entry& e = s.probes[name];
    e.fn = std::move(fn);
}

void unregisterProbe(const std::string& name)
{
    State& s = st();
    std::lock_guard<std::mutex> lk(s.mutex);
    s.probes.erase(name);
}

void tick(F64 now, F64 interval_sec, U32 top_n, U64 trim_free_bytes)
{
    State& s = st();
    if (interval_sec <= 0.0) { s.next_report = -1.0; return; }
    if (s.next_report < 0.0) { s.next_report = now + interval_sec; return; }   // first report one interval in
    if (now < s.next_report) return;
    s.next_report = now + interval_sec;
    LL_INFOS("MemTrend") << build(now, top_n, true, trim_free_bytes) << LL_ENDL;
}

std::string reportNow(F64 now, U32 top_n)
{
    const std::string line = build(now, top_n, false);
    LL_INFOS("MemTrend") << line << LL_ENDL;
    return line;
}

}   // namespace LLMemTrend
