/**
 * @file llcachemetrics.cpp
 * @brief Consumer-level disk cache metrics (#715). See llcachemetrics.h.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#include "linden_common.h"

#include "llcachemetrics.h"

#include "llformat.h"

#include <atomic>

namespace
{
    using LLCacheMetrics::Cache;
    using LLCacheMetrics::Backend;

    constexpr size_t NC = (size_t)Cache::COUNT;
    constexpr size_t NB = (size_t)Backend::COUNT;
    constexpr size_t NH = 8;
    constexpr size_t NL = LLCacheMetrics::LAT_BUCKETS;
    constexpr size_t NU = LLCacheMetrics::USE_SKIP_BUCKETS;
    constexpr size_t NS = (size_t)LLCacheMetrics::Stage::COUNT;

    struct AtomicRow
    {
        std::atomic<U64> reads{ 0 }, hits{ 0 }, read_bytes{ 0 }, read_usec{ 0 }, read_usec_max{ 0 };
        std::atomic<U64> read_usec_hist[NH] = {};
        std::atomic<U64> writes{ 0 }, write_fail{ 0 }, write_payload{ 0 }, write_disk{ 0 }, write_usec{ 0 };
        std::atomic<U64> evict{ 0 }, evict_bytes{ 0 }, corrupt{ 0 };
        std::atomic<U64> disk_bytes{ 0 }, live_bytes{ 0 }, files{ 0 };
        std::atomic<U64> hit_usec{ 0 }, hit_usec_max{ 0 };
        std::atomic<U64> lat_hist[NL] = {};
        std::atomic<U64> hit_lat_hist[NL] = {};
        std::atomic<U64> use_count{ 0 }, use_full{ 0 }, use_levels{ 0 }, use_bytes_read{ 0 }, use_bytes_needed{ 0 };
        std::atomic<U64> use_skip_hist[NU] = {};
        std::atomic<U64> use_unused{ 0 }, use_unused_bytes{ 0 };
    };

    struct AtomicStage
    {
        std::atomic<U64> count{ 0 }, fail{ 0 }, bytes_in{ 0 }, bytes_out{ 0 };
        std::atomic<U64> lat_usec{ 0 }, lat_usec_max{ 0 }, work_usec{ 0 }, work_usec_max{ 0 };
        std::atomic<U64> batches{ 0 }, timed{ 0 };
        std::atomic<U64> lat_hist[NL] = {};
        std::atomic<U64> work_hist[NL] = {};
    };

    AtomicRow sRows[NC][NB];
    AtomicStage sStages[NS];
    std::atomic<U64> sStartup[NB] = {};
    std::atomic<U64> sCacheStartup[NC][NB] = {};

    AtomicRow& row(Cache c, Backend b) { return sRows[(size_t)c % NC][(size_t)b % NB]; }

    void add(std::atomic<U64>& a, U64 v) { a.fetch_add(v, std::memory_order_relaxed); }

    void storeMax(std::atomic<U64>& a, U64 v)
    {
        U64 cur = a.load(std::memory_order_relaxed);
        while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
    }

    size_t histBucket(U64 usec)
    {
        static const U64 edges[NH - 1] = { 100, 1000, 5000, 20000, 100000, 500000, 2000000 };
        for (size_t i = 0; i < NH - 1; ++i)
            if (usec < edges[i]) return i;
        return NH - 1;
    }

    // Upper edge (usec) of the bucket that holds the p-th fraction of the reads.
    U64 coarsePercentile(const LLCacheMetrics::Row& r, F64 p)
    {
        static const U64 upper[NH] = { 100, 1000, 5000, 20000, 100000, 500000, 2000000, 0 };
        U64 total = 0;
        for (size_t i = 0; i < NH; ++i) total += r.read_usec_hist[i];
        if (!total) return 0;
        const U64 want = (U64)(p * (F64)total + 0.5);
        U64 seen = 0;
        for (size_t i = 0; i < NH; ++i)
        {
            seen += r.read_usec_hist[i];
            if (seen >= want) return upper[i] ? upper[i] : r.read_usec_max;
        }
        return r.read_usec_max;
    }
}

namespace LLCacheMetrics
{
    const char* cacheName(Cache c)
    {
        switch (c)
        {
        case Cache::J2C:     return "j2c";
        case Cache::GPUTEX:  return "gputex";
        case Cache::ASSET:   return "asset";
        case Cache::MESH:    return "mesh";
        case Cache::CLUSTER: return "cluster";
        default:             return "?";
        }
    }

    const char* backendName(Backend b)
    {
        return b == Backend::STORE ? "store" : (b == Backend::LEGACY ? "legacy" : "?");
    }

    void recordRead(Cache c, Backend b, bool hit, U64 bytes, U64 usec)
    {
        AtomicRow& r = row(c, b);
        add(r.reads, 1);
        if (hit)
        {
            add(r.hits, 1);
            add(r.read_bytes, bytes);
        }
        add(r.read_usec, usec);
        storeMax(r.read_usec_max, usec);
        add(r.read_usec_hist[histBucket(usec)], 1);
        const size_t lb = latBucket(usec);
        add(r.lat_hist[lb], 1);
        if (hit)
        {
            add(r.hit_usec, usec);
            storeMax(r.hit_usec_max, usec);
            add(r.hit_lat_hist[lb], 1);
        }
    }

    size_t latBucket(U64 usec)
    {
        if (usec < 4) return (size_t)usec;
        U32 e = 0;
        for (U64 v = usec; v > 1; v >>= 1) ++e;      // floor(log2(usec)) >= 2
        if (e > 31) return NL - 1;
        const size_t m = (size_t)((usec >> (e - 2)) & 3);
        return llmin((size_t)(4 * e + m - 4), NL - 1);
    }

    U64 latBucketLow(size_t i)
    {
        if (i < 4) return (U64)i;
        const U32 e = (U32)(i / 4) + 1;
        const U64 m = (U64)(i % 4);
        return (4 + m) << (e - 2);
    }

    void recordEvictions(Cache c, Backend b, U64 count, U64 bytes)
    {
        if (!count && !bytes) return;
        AtomicRow& r = row(c, b);
        add(r.evict, count);
        add(r.evict_bytes, bytes);
    }

    void recordStartup(Cache c, Backend b, U64 usec)
    {
        sCacheStartup[(size_t)c % NC][(size_t)b % NB].store(usec, std::memory_order_relaxed);
    }

    U64 startupUsec(Cache c, Backend b)
    {
        return sCacheStartup[(size_t)c % NC][(size_t)b % NB].load(std::memory_order_relaxed);
    }

    bool rowEmpty(const Row& r)
    {
        return !r.reads && !r.writes && !r.evict && !r.corrupt && !r.disk_bytes && !r.live_bytes && !r.files
            && !r.use_count && !r.use_unused;
    }

    void recordReadUse(Cache c, Backend b, U32 levels, U32 skip, U64 bytes_read, U64 bytes_needed)
    {
        AtomicRow& r = row(c, b);
        add(r.use_count, 1);
        if (!skip) add(r.use_full, 1);
        add(r.use_levels, levels);
        add(r.use_skip_hist[llmin((size_t)skip, NU - 1)], 1);
        add(r.use_bytes_read, bytes_read);
        add(r.use_bytes_needed, llmin(bytes_needed, bytes_read));
    }

    void recordReadUnused(Cache c, Backend b, U64 bytes_read)
    {
        AtomicRow& r = row(c, b);
        add(r.use_unused, 1);
        add(r.use_unused_bytes, bytes_read);
    }

    const char* stageName(Stage s)
    {
        switch (s)
        {
        case Stage::J2C_DECODE: return "j2c_decode";
        case Stage::BC_ENCODE:  return "bc_encode";
        default:                return "?";
        }
    }

    void recordStage(Stage s, bool ok, U64 bytes_in, U64 bytes_out, U64 latency_usec, U64 work_usec)
    {
        AtomicStage& a = sStages[(size_t)s % NS];
        add(a.count, 1);
        if (!ok)
        {
            add(a.fail, 1);
            return;
        }
        add(a.bytes_in, bytes_in);
        add(a.bytes_out, bytes_out);
        add(a.lat_usec, latency_usec);
        storeMax(a.lat_usec_max, latency_usec);
        add(a.lat_hist[latBucket(latency_usec)], 1);
        add(a.work_usec, work_usec);
        storeMax(a.work_usec_max, work_usec);
        add(a.work_hist[latBucket(work_usec)], 1);
        add(a.timed, 1);
    }

    void recordStageBatch(Stage s, U64 n, U64 bytes_in, U64 bytes_out, U64 work_usec)
    {
        if (!n) return;
        AtomicStage& a = sStages[(size_t)s % NS];
        add(a.count, n);
        add(a.batches, 1);
        add(a.bytes_in, bytes_in);
        add(a.bytes_out, bytes_out);
        if (work_usec)
        {
            add(a.work_usec, work_usec);
            storeMax(a.work_usec_max, work_usec);
            add(a.work_hist[latBucket(work_usec)], 1);
            add(a.timed, 1);
        }
    }

    StageRow stageSnapshot(Stage s)
    {
        const AtomicStage& a = sStages[(size_t)s % NS];
        auto ld = [](const std::atomic<U64>& v) { return v.load(std::memory_order_relaxed); };
        StageRow r;
        r.count = ld(a.count); r.fail = ld(a.fail); r.bytes_in = ld(a.bytes_in); r.bytes_out = ld(a.bytes_out);
        r.lat_usec = ld(a.lat_usec); r.lat_usec_max = ld(a.lat_usec_max);
        r.work_usec = ld(a.work_usec); r.work_usec_max = ld(a.work_usec_max);
        r.batches = ld(a.batches); r.timed = ld(a.timed);
        for (size_t i = 0; i < NL; ++i)
        {
            r.lat_hist[i] = ld(a.lat_hist[i]);
            r.work_hist[i] = ld(a.work_hist[i]);
        }
        return r;
    }

    bool stageEmpty(const StageRow& r) { return !r.count && !r.fail; }

    U64 latencyPercentile(const Row& r, F64 p, bool hits_only)
    {
        return hits_only ? histPercentile(r.hit_lat_hist, r.hit_usec_max, p) : histPercentile(r.lat_hist, r.read_usec_max, p);
    }

    U64 histPercentile(const U64* h, U64 mx, F64 p)
    {
        U64 total = 0;
        for (size_t i = 0; i < NL; ++i) total += h[i];
        if (!total) return 0;
        p = llclamp(p, 0.0, 1.0);
        // Rank of the wanted sample (1-based, nearest-rank), then linear inside its bucket.
        const F64 want = llmax(1.0, p * (F64)total);
        F64 seen = 0.0;
        for (size_t i = 0; i < NL; ++i)
        {
            if (!h[i]) continue;
            if (seen + (F64)h[i] >= want)
            {
                const F64 lo = (F64)latBucketLow(i);
                const F64 hi = i + 1 < NL ? (F64)latBucketLow(i + 1) : (F64)llmax(mx, (U64)lo + 1);
                const F64 frac = (want - seen) / (F64)h[i];
                const U64 v = (U64)(lo + (hi - lo) * frac);
                return mx ? llmin(v, mx) : v;
            }
            seen += (F64)h[i];
        }
        return mx;
    }

    void recordWrite(Cache c, Backend b, U64 payload, U64 disk, U64 usec, bool ok)
    {
        AtomicRow& r = row(c, b);
        add(r.writes, 1);
        if (!ok)
        {
            add(r.write_fail, 1);
            return;
        }
        add(r.write_payload, payload);
        add(r.write_disk, disk);
        add(r.write_usec, usec);
    }

    void recordEvict(Cache c, Backend b, U64 bytes)
    {
        AtomicRow& r = row(c, b);
        add(r.evict, 1);
        add(r.evict_bytes, bytes);
    }

    void recordCorrupt(Cache c, Backend b) { add(row(c, b).corrupt, 1); }

    void setFootprint(Cache c, Backend b, U64 disk_bytes, U64 live_bytes, U64 files)
    {
        AtomicRow& r = row(c, b);
        r.disk_bytes.store(disk_bytes, std::memory_order_relaxed);
        r.live_bytes.store(live_bytes, std::memory_order_relaxed);
        r.files.store(files, std::memory_order_relaxed);
    }

    void recordStartup(Backend b, U64 usec) { sStartup[(size_t)b % NB].store(usec, std::memory_order_relaxed); }

    Row snapshot(Cache c, Backend b)
    {
        const AtomicRow& a = row(c, b);
        auto ld = [](const std::atomic<U64>& v) { return v.load(std::memory_order_relaxed); };
        Row r;
        r.reads = ld(a.reads); r.hits = ld(a.hits); r.read_bytes = ld(a.read_bytes);
        r.read_usec = ld(a.read_usec); r.read_usec_max = ld(a.read_usec_max);
        for (size_t i = 0; i < NH; ++i) r.read_usec_hist[i] = ld(a.read_usec_hist[i]);
        r.writes = ld(a.writes); r.write_fail = ld(a.write_fail); r.write_payload = ld(a.write_payload);
        r.write_disk = ld(a.write_disk); r.write_usec = ld(a.write_usec);
        r.evict = ld(a.evict); r.evict_bytes = ld(a.evict_bytes); r.corrupt = ld(a.corrupt);
        r.disk_bytes = ld(a.disk_bytes); r.live_bytes = ld(a.live_bytes); r.files = ld(a.files);
        r.hit_usec = ld(a.hit_usec); r.hit_usec_max = ld(a.hit_usec_max);
        for (size_t i = 0; i < NL; ++i)
        {
            r.lat_hist[i] = ld(a.lat_hist[i]);
            r.hit_lat_hist[i] = ld(a.hit_lat_hist[i]);
        }
        r.use_count = ld(a.use_count); r.use_full = ld(a.use_full); r.use_levels = ld(a.use_levels);
        r.use_bytes_read = ld(a.use_bytes_read); r.use_bytes_needed = ld(a.use_bytes_needed);
        for (size_t i = 0; i < NU; ++i) r.use_skip_hist[i] = ld(a.use_skip_hist[i]);
        r.use_unused = ld(a.use_unused); r.use_unused_bytes = ld(a.use_unused_bytes);
        return r;
    }

    U64 startupUsec(Backend b) { return sStartup[(size_t)b % NB].load(std::memory_order_relaxed); }

    std::string report()
    {
        constexpr F64 MB = 1024.0 * 1024.0;
        std::string out;
        for (size_t b = 0; b < NB; ++b)
        {
            const U64 st = startupUsec((Backend)b);
            if (st) out += llformat("%s startup: %.1f ms\n", backendName((Backend)b), (F64)st / 1000.0);
        }
        for (size_t c = 0; c < NC; ++c)
        {
            for (size_t b = 0; b < NB; ++b)
            {
                const U64 st = startupUsec((Cache)c, (Backend)b);
                if (st) out += llformat("%s %s startup: %.1f ms\n", cacheName((Cache)c), backendName((Backend)b),
                                        (F64)st / 1000.0);
            }
        }
        for (size_t c = 0; c < NC; ++c)
        {
            for (size_t b = 0; b < NB; ++b)
            {
                const Row r = snapshot((Cache)c, (Backend)b);
                if (rowEmpty(r)) continue;
                const F64 hit_rate = r.reads ? 100.0 * (F64)r.hits / (F64)r.reads : 0.0;
                const F64 mean_ms = r.reads ? (F64)r.read_usec / (F64)r.reads / 1000.0 : 0.0;
                const F64 amp = r.write_payload ? (F64)r.write_disk / (F64)r.write_payload : 0.0;
                const F64 ratio = r.disk_bytes ? (F64)r.live_bytes / (F64)r.disk_bytes : 0.0;
                out += llformat("%-7s %-6s reads %llu hit %.1f%% read %.1f MB lat mean %.2f ms p95 <%.1f ms max %.1f ms | "
                                "writes %llu fail %llu payload %.1f MB disk %.1f MB amp %.2f | evict %llu (%.1f MB) corrupt %llu | "
                                "footprint %.1f MB disk, %.1f MB live (x%.2f), %llu files | "
                                "hit lat mean %.2f ms p50 %.2f p95 %.2f p99 %.2f ms\n",
                                cacheName((Cache)c), backendName((Backend)b),
                                (unsigned long long)r.reads, hit_rate, (F64)r.read_bytes / MB, mean_ms,
                                (F64)coarsePercentile(r, 0.95) / 1000.0, (F64)r.read_usec_max / 1000.0,
                                (unsigned long long)r.writes, (unsigned long long)r.write_fail,
                                (F64)r.write_payload / MB, (F64)r.write_disk / MB, amp,
                                (unsigned long long)r.evict, (F64)r.evict_bytes / MB, (unsigned long long)r.corrupt,
                                (F64)r.disk_bytes / MB, (F64)r.live_bytes / MB, ratio, (unsigned long long)r.files,
                                r.hits ? (F64)r.hit_usec / (F64)r.hits / 1000.0 : 0.0,
                                (F64)latencyPercentile(r, 0.50, true) / 1000.0,
                                (F64)latencyPercentile(r, 0.95, true) / 1000.0,
                                (F64)latencyPercentile(r, 0.99, true) / 1000.0);
                if (r.use_count || r.use_unused)
                    out += llformat("%-7s %-6s read use: %llu used, %.1f%% needed the full chain (mean %.1f levels), "
                                    "skipped 0/1/2/3+ levels %llu/%llu/%llu/%llu, needed %.1f MB of %.1f MB read (%.1f%%), "
                                    "unused %llu (%.1f MB)\n",
                                    cacheName((Cache)c), backendName((Backend)b), (unsigned long long)r.use_count,
                                    r.use_count ? 100.0 * (F64)r.use_full / (F64)r.use_count : 0.0,
                                    r.use_count ? (F64)r.use_levels / (F64)r.use_count : 0.0,
                                    (unsigned long long)r.use_skip_hist[0], (unsigned long long)r.use_skip_hist[1],
                                    (unsigned long long)r.use_skip_hist[2], (unsigned long long)r.use_skip_hist[3],
                                    (F64)r.use_bytes_needed / MB, (F64)r.use_bytes_read / MB,
                                    r.use_bytes_read ? 100.0 * (F64)r.use_bytes_needed / (F64)r.use_bytes_read : 0.0,
                                    (unsigned long long)r.use_unused, (F64)r.use_unused_bytes / MB);
            }
        }
        for (size_t s = 0; s < NS; ++s)
        {
            const StageRow r = stageSnapshot((Stage)s);
            if (stageEmpty(r)) continue;
            const U64 ok = r.count - llmin(r.fail, r.count);
            out += llformat("stage %-10s count %llu fail %llu in %.1f MB out %.1f MB | lat mean %.2f ms p50 %.2f p95 %.2f "
                            "p99 %.2f max %.1f ms | work mean %.2f ms p50 %.2f p95 %.2f p99 %.2f max %.1f ms "
                            "(%llu timed, %llu batches)\n",
                            stageName((Stage)s), (unsigned long long)r.count, (unsigned long long)r.fail,
                            (F64)r.bytes_in / MB, (F64)r.bytes_out / MB,
                            ok && !r.batches ? (F64)r.lat_usec / (F64)ok / 1000.0 : 0.0,
                            (F64)histPercentile(r.lat_hist, r.lat_usec_max, 0.50) / 1000.0,
                            (F64)histPercentile(r.lat_hist, r.lat_usec_max, 0.95) / 1000.0,
                            (F64)histPercentile(r.lat_hist, r.lat_usec_max, 0.99) / 1000.0,
                            (F64)r.lat_usec_max / 1000.0,
                            r.timed ? (F64)r.work_usec / (F64)r.timed / 1000.0 : 0.0,
                            (F64)histPercentile(r.work_hist, r.work_usec_max, 0.50) / 1000.0,
                            (F64)histPercentile(r.work_hist, r.work_usec_max, 0.95) / 1000.0,
                            (F64)histPercentile(r.work_hist, r.work_usec_max, 0.99) / 1000.0,
                            (F64)r.work_usec_max / 1000.0, (unsigned long long)r.timed, (unsigned long long)r.batches);
        }
        return out;
    }

    std::string reportKV(const char* tag)
    {
        if (!tag || !*tag) tag = "CacheMetrics";
        auto u = [](U64 v) { return (unsigned long long)v; };
        std::string out;
        for (size_t c = 0; c < NC; ++c)
        {
            for (size_t b = 0; b < NB; ++b)
            {
                const Row r = snapshot((Cache)c, (Backend)b);
                if (rowEmpty(r)) continue;
                out += llformat("%s cache=%s backend=%s reads=%llu hits=%llu read_bytes=%llu read_usec=%llu "
                                "read_usec_max=%llu hit_usec=%llu hit_usec_max=%llu "
                                "p50_usec=%llu p95_usec=%llu p99_usec=%llu "
                                "hit_p50_usec=%llu hit_p95_usec=%llu hit_p99_usec=%llu "
                                "writes=%llu write_fail=%llu write_payload=%llu write_disk=%llu write_usec=%llu "
                                "evict=%llu evict_bytes=%llu corrupt=%llu disk_bytes=%llu live_bytes=%llu files=%llu\n",
                                tag, cacheName((Cache)c), backendName((Backend)b),
                                u(r.reads), u(r.hits), u(r.read_bytes), u(r.read_usec), u(r.read_usec_max),
                                u(r.hit_usec), u(r.hit_usec_max),
                                u(latencyPercentile(r, 0.50, false)), u(latencyPercentile(r, 0.95, false)),
                                u(latencyPercentile(r, 0.99, false)),
                                u(latencyPercentile(r, 0.50, true)), u(latencyPercentile(r, 0.95, true)),
                                u(latencyPercentile(r, 0.99, true)),
                                u(r.writes), u(r.write_fail), u(r.write_payload), u(r.write_disk), u(r.write_usec),
                                u(r.evict), u(r.evict_bytes), u(r.corrupt), u(r.disk_bytes), u(r.live_bytes), u(r.files));
                if (r.use_count || r.use_unused)
                    out += llformat("%s use cache=%s backend=%s count=%llu full=%llu levels=%llu skip0=%llu skip1=%llu "
                                    "skip2=%llu skip3plus=%llu bytes_read=%llu bytes_needed=%llu unused=%llu "
                                    "unused_bytes=%llu\n",
                                    tag, cacheName((Cache)c), backendName((Backend)b), u(r.use_count), u(r.use_full),
                                    u(r.use_levels), u(r.use_skip_hist[0]), u(r.use_skip_hist[1]), u(r.use_skip_hist[2]),
                                    u(r.use_skip_hist[3]), u(r.use_bytes_read), u(r.use_bytes_needed), u(r.use_unused),
                                    u(r.use_unused_bytes));
            }
        }
        for (size_t s = 0; s < NS; ++s)
        {
            const StageRow r = stageSnapshot((Stage)s);
            if (stageEmpty(r)) continue;
            out += llformat("%s stage stage=%s count=%llu fail=%llu bytes_in=%llu bytes_out=%llu lat_usec=%llu "
                            "lat_usec_max=%llu p50_usec=%llu p95_usec=%llu p99_usec=%llu work_usec=%llu work_usec_max=%llu "
                            "work_p50_usec=%llu work_p95_usec=%llu work_p99_usec=%llu timed=%llu batches=%llu\n",
                            tag, stageName((Stage)s), u(r.count), u(r.fail), u(r.bytes_in), u(r.bytes_out), u(r.lat_usec),
                            u(r.lat_usec_max), u(histPercentile(r.lat_hist, r.lat_usec_max, 0.50)),
                            u(histPercentile(r.lat_hist, r.lat_usec_max, 0.95)),
                            u(histPercentile(r.lat_hist, r.lat_usec_max, 0.99)), u(r.work_usec), u(r.work_usec_max),
                            u(histPercentile(r.work_hist, r.work_usec_max, 0.50)),
                            u(histPercentile(r.work_hist, r.work_usec_max, 0.95)),
                            u(histPercentile(r.work_hist, r.work_usec_max, 0.99)), u(r.timed), u(r.batches));
        }
        for (size_t b = 0; b < NB; ++b)
        {
            const U64 st = startupUsec((Backend)b);
            if (st) out += llformat("%s startup backend=%s usec=%llu\n", tag, backendName((Backend)b), u(st));
            for (size_t c = 0; c < NC; ++c)
            {
                const U64 cs = startupUsec((Cache)c, (Backend)b);
                if (cs) out += llformat("%s startup backend=%s cache=%s usec=%llu\n", tag, backendName((Backend)b),
                                        cacheName((Cache)c), u(cs));
            }
        }
        return out;
    }

    void reset()
    {
        for (size_t c = 0; c < NC; ++c)
        {
            for (size_t b = 0; b < NB; ++b)
            {
                AtomicRow& r = sRows[c][b];
                for (std::atomic<U64>* a : { &r.reads, &r.hits, &r.read_bytes, &r.read_usec, &r.read_usec_max,
                                             &r.writes, &r.write_fail, &r.write_payload, &r.write_disk, &r.write_usec,
                                             &r.evict, &r.evict_bytes, &r.corrupt })
                    a->store(0, std::memory_order_relaxed);
                for (size_t i = 0; i < NH; ++i) r.read_usec_hist[i].store(0, std::memory_order_relaxed);
                r.hit_usec.store(0, std::memory_order_relaxed);
                r.hit_usec_max.store(0, std::memory_order_relaxed);
                for (size_t i = 0; i < NL; ++i)
                {
                    r.lat_hist[i].store(0, std::memory_order_relaxed);
                    r.hit_lat_hist[i].store(0, std::memory_order_relaxed);
                }
                for (std::atomic<U64>* a : { &r.use_count, &r.use_full, &r.use_levels, &r.use_bytes_read,
                                             &r.use_bytes_needed, &r.use_unused, &r.use_unused_bytes })
                    a->store(0, std::memory_order_relaxed);
                for (size_t i = 0; i < NU; ++i) r.use_skip_hist[i].store(0, std::memory_order_relaxed);
            }
        }
        for (size_t s = 0; s < NS; ++s)
        {
            AtomicStage& a = sStages[s];
            for (std::atomic<U64>* v : { &a.count, &a.fail, &a.bytes_in, &a.bytes_out, &a.lat_usec, &a.lat_usec_max,
                                         &a.work_usec, &a.work_usec_max, &a.batches, &a.timed })
                v->store(0, std::memory_order_relaxed);
            for (size_t i = 0; i < NL; ++i)
            {
                a.lat_hist[i].store(0, std::memory_order_relaxed);
                a.work_hist[i].store(0, std::memory_order_relaxed);
            }
        }
    }
}
