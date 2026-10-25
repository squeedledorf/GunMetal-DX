/**
 * @file slcachemetrics.cpp
 * @brief Viewer side of the #715 cache metrics. See slcachemetrics.h.
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

#include "llviewerprecompiledheaders.h"

#include "slcachemetrics.h"

#include "llcachemetrics.h"
#include "llcachestore.h"
#include "llcallbacklist.h"
#include "lldir.h"
#include "llfile.h"
#include "llformat.h"
#include "llframetimer.h"
#include "llgputexcache.h"
#include "llsdjson.h"
#include "llversioninfo.h"
#include "llviewercontrol.h"
#include "slcachestore.h"

#include <boost/json.hpp>

#include <cstdint>
#include <ctime>
#include <sstream>

namespace
{
    using LLCacheMetrics::Cache;
    using LLCacheMetrics::Backend;

    constexpr F64 SAMPLE_INTERVAL_SEC = 15.0;
    constexpr size_t NKIND = (size_t)LLCache::Kind::COUNT;
    const LLCache::Kind STORE_KINDS[] = { LLCache::Kind::J2C, LLCache::Kind::GPUTEX, LLCache::Kind::ASSET,
                                          LLCache::Kind::MESH, LLCache::Kind::CLUSTER };
    // Settings that select a backend (read defensively: some arrive with the other #715
    // consumers), plus the ones that size / enable the caches being compared.
    const char* const BACKEND_SETTINGS[] = { "CacheStoreGpuTex", "CacheStoreJ2C", "CacheStoreAssets", "CacheStoreCluster",
                                             "SlipstreamGpuTexCache", "SlipstreamGpuTexCacheSizeMB", "CacheSize",
                                             "RenderVKClusterLOD", "RenderVKClusterCacheMB",
                                             "CacheStoreSnapshotIntervalSec", "CacheMetricsLogIntervalMin" };

    bool sInited = false;
    bool sDone = false;
    F64 sLastSample = 0.0, sLastLog = 0.0;
    U32 sSessionStart = 0;
    bool sStoreStartupRecorded = false;
    U64 sEvictRec[NKIND] = {}, sEvictBytes[NKIND] = {};
    // Last store sample (kept after the store closes, for the shutdown block / JSON).
    bool sHaveStore = false;
    LLCache::Stats sStore;
    std::string sStoreDir;
    U32 sIndexFiles = 0;
    std::string sGpuTexBackend = "off";

    Cache cacheOf(LLCache::Kind k)
    {
        switch (k)
        {
        case LLCache::Kind::J2C:     return Cache::J2C;
        case LLCache::Kind::GPUTEX:  return Cache::GPUTEX;
        case LLCache::Kind::ASSET:   return Cache::ASSET;
        case LLCache::Kind::MESH:    return Cache::MESH;
        default:                     return Cache::CLUSTER;
        }
    }

    const char* driveName(LLCache::DriveType d)
    {
        return d == LLCache::DriveType::SSD ? "ssd" : d == LLCache::DriveType::HDD ? "hdd" : "unknown";
    }

    unsigned long long u(U64 v) { return (unsigned long long)v; }

    void sampleGpuTexBackend()
    {
        if (LLGpuTexCache::usingStore()) sGpuTexBackend = "store";
        else if (LLGpuTexCache::getStats().scanned) sGpuTexBackend = "legacy";
    }

    void sampleStore()
    {
        LLCache::Store* st = SLCacheStore::instance().store();
        if (!st || !st->ready()) return;
        const LLCache::Stats s = st->stats();
        for (LLCache::Kind k : STORE_KINDS)
        {
            const size_t i = (size_t)k;
            const LLCache::KindStats& ks = s.kinds[i];
            const Cache c = cacheOf(k);
            // Per-kind disk bytes are exact (the store attributes every appended byte to its
            // kind per segment); files = segments holding the kind.
            if (ks.disk_bytes || ks.raw_bytes || ks.segments)
                LLCacheMetrics::setFootprint(c, Backend::STORE, ks.disk_bytes, ks.raw_bytes, ks.segments);
            // Store evictions are internal: their running totals arrive here as deltas.
            if (ks.evicted_records >= sEvictRec[i] && ks.evicted_bytes >= sEvictBytes[i])
                LLCacheMetrics::recordEvictions(c, Backend::STORE, ks.evicted_records - sEvictRec[i],
                                                ks.evicted_bytes - sEvictBytes[i]);
            sEvictRec[i] = ks.evicted_records;
            sEvictBytes[i] = ks.evicted_bytes;
        }
        if (!sStoreStartupRecorded && s.index_load_usec)
        {
            LLCacheMetrics::recordStartup(Backend::STORE, s.index_load_usec);
            sStoreStartupRecorded = true;
        }
        sStore = s;
        sHaveStore = true;
        sStoreDir = st->dir();
        sIndexFiles = (LLFile::isfile(sStoreDir + "/index.snap") ? 1 : 0) + (LLFile::isfile(sStoreDir + "/index.snap.prev") ? 1 : 0);
    }

    // One "CacheMetrics store ..." line with the store totals, then one per kind.
    std::string storeKV(const char* tag)
    {
        if (!sHaveStore) return std::string();
        const LLCache::Stats& s = sStore;
        std::string out = llformat(
            "%s store ready=%d read_only=%d drive=%s segments=%u index_files=%u files=%u records=%u disk_bytes=%llu "
            "live_bytes=%llu dead_bytes=%llu peak_disk_bytes=%llu effective_limit=%llu "
            "bytes_written_total=%llu written_session_new=%llu written_session_compaction=%llu "
            "written_session_meta=%llu written_today_new=%llu written_today_compaction=%llu written_today_meta=%llu "
            "bytes_read=%llu hits=%llu misses=%llu corrupt_dropped=%llu write_fail=%llu writes_refused=%llu "
            "evicted_records=%llu evicted_bytes=%llu segments_reclaimed=%llu rescued_records=%llu superseded=%llu "
            "skipped=%llu prefetch_bytes=%llu prefetch_hits=%llu "
            "encode_count=%llu encode_usec=%llu encode_in_bytes=%llu encode_out_bytes=%llu "
            "decode_count=%llu decode_usec=%llu decode_in_bytes=%llu decode_out_bytes=%llu "
            "append_count=%llu append_usec=%llu append_bytes=%llu index_load_usec=%llu "
            "index_bytes=%llu index_records=%u\n",
            tag, s.ready ? 1 : 0, s.read_only ? 1 : 0, driveName(s.drive), s.segments, sIndexFiles,
            s.segments + sIndexFiles, s.records, u(s.disk_bytes), u(s.live_bytes), u(s.dead_bytes), u(s.peak_disk_bytes),
            u(s.effective_limit), u(s.bytes_written_total), u(s.bytes_written_session_new),
            u(s.bytes_written_session_compaction), u(s.bytes_written_session_meta), u(s.bytes_written_today_new),
            u(s.bytes_written_today_compaction), u(s.bytes_written_today_meta), u(s.bytes_read), u(s.hits), u(s.misses),
            u(s.corrupt_dropped), u(s.write_fail), u(s.writes_refused), u(s.evicted_records), u(s.evicted_bytes),
            u(s.segments_reclaimed), u(s.rescued_records), u(s.superseded), u(s.skipped), u(s.prefetch_bytes),
            u(s.prefetch_hits), u(s.encode_count), u(s.encode_usec), u(s.encode_in_bytes), u(s.encode_out_bytes),
            u(s.decode_count), u(s.decode_usec), u(s.decode_in_bytes), u(s.decode_out_bytes), u(s.append_count),
            u(s.append_usec), u(s.append_bytes), u(s.index_load_usec), u(s.index_bytes), s.index_records);
        for (size_t k = 1; k < NKIND; ++k)
        {
            const LLCache::KindStats& ks = s.kinds[k];
            if (!ks.records && !ks.disk_bytes && !ks.evicted_records) continue;
            out += llformat("%s store_kind kind=%s records=%u live_bytes=%llu raw_bytes=%llu disk_bytes=%llu segments=%u "
                            "evicted_records=%llu evicted_bytes=%llu\n",
                            tag, LLCache::kindName((LLCache::Kind)k), ks.records, u(ks.live_bytes), u(ks.raw_bytes),
                            u(ks.disk_bytes), ks.segments, u(ks.evicted_records), u(ks.evicted_bytes));
        }
        return out;
    }

    void logLines(const std::string& block)
    {
        std::istringstream in(block);
        std::string line;
        while (std::getline(in, line))
            if (!line.empty()) LL_INFOS("CacheMetrics") << line << LL_ENDL;
    }

    void idle(void*)
    {
        if (sDone) return;
        const F64 t = LLFrameTimer::getElapsedSeconds();
        if (t - sLastSample >= SAMPLE_INTERVAL_SEC)
        {
            SLCacheMetrics::sampleNow();
            sLastSample = t;
        }
        static LLCachedControl<U32> interval_min(gSavedSettings, "CacheMetricsLogIntervalMin", 10);
        const U32 iv = interval_min();
        if (iv && t - sLastLog >= (F64)iv * 60.0)
        {
            SLCacheMetrics::logReport("periodic");
            sLastLog = t;
        }
    }

    // ---- session JSON

    boost::json::value histJson(const U64* h, size_t n)
    {
        boost::json::array a;
        for (size_t i = 0; i < n; ++i)
            if (h[i]) a.emplace_back(boost::json::array{ (std::uint64_t)i, (std::uint64_t)h[i] });
        return a;
    }

    boost::json::object rowJson(Cache c, Backend b, const LLCacheMetrics::Row& r)
    {
        boost::json::object o;
        o["cache"] = LLCacheMetrics::cacheName(c);
        o["backend"] = LLCacheMetrics::backendName(b);
        auto put = [&o](const char* k, U64 v) { o[k] = (std::uint64_t)v; };
        put("reads", r.reads); put("hits", r.hits); put("read_bytes", r.read_bytes);
        put("read_usec", r.read_usec); put("read_usec_max", r.read_usec_max);
        put("hit_usec", r.hit_usec); put("hit_usec_max", r.hit_usec_max);
        put("writes", r.writes); put("write_fail", r.write_fail); put("write_payload", r.write_payload);
        put("write_disk", r.write_disk); put("write_usec", r.write_usec);
        put("evict", r.evict); put("evict_bytes", r.evict_bytes); put("corrupt", r.corrupt);
        put("disk_bytes", r.disk_bytes); put("live_bytes", r.live_bytes); put("files", r.files);
        put("p50_usec", LLCacheMetrics::latencyPercentile(r, 0.50, false));
        put("p95_usec", LLCacheMetrics::latencyPercentile(r, 0.95, false));
        put("p99_usec", LLCacheMetrics::latencyPercentile(r, 0.99, false));
        put("hit_p50_usec", LLCacheMetrics::latencyPercentile(r, 0.50, true));
        put("hit_p95_usec", LLCacheMetrics::latencyPercentile(r, 0.95, true));
        put("hit_p99_usec", LLCacheMetrics::latencyPercentile(r, 0.99, true));
        put("startup_usec", LLCacheMetrics::startupUsec(c, b));
        o["lat_hist"] = histJson(r.lat_hist, LLCacheMetrics::LAT_BUCKETS);           // [[bucket, count], ...]
        o["hit_lat_hist"] = histJson(r.hit_lat_hist, LLCacheMetrics::LAT_BUCKETS);
        boost::json::array coarse;
        for (U64 v : r.read_usec_hist) coarse.emplace_back((std::uint64_t)v);
        o["coarse_hist"] = coarse;   // <100us, <1ms, <5ms, <20ms, <100ms, <500ms, <2s, >=2s
        // version 2: read use (what a whole-record read was needed for; GPU-ready chains)
        put("use_count", r.use_count); put("use_full", r.use_full); put("use_levels", r.use_levels);
        put("use_bytes_read", r.use_bytes_read); put("use_bytes_needed", r.use_bytes_needed);
        put("use_unused", r.use_unused); put("use_unused_bytes", r.use_unused_bytes);
        boost::json::array skip;
        for (U64 v : r.use_skip_hist) skip.emplace_back((std::uint64_t)v);
        o["use_skip_hist"] = skip;   // levels skipped below the top: 0, 1, 2, 3+
        return o;
    }

    // version 2: one decode / transcode stage (LLCacheMetrics::Stage)
    boost::json::object stageJson(LLCacheMetrics::Stage s, const LLCacheMetrics::StageRow& r)
    {
        boost::json::object o;
        o["stage"] = LLCacheMetrics::stageName(s);
        auto put = [&o](const char* k, U64 v) { o[k] = (std::uint64_t)v; };
        put("count", r.count); put("fail", r.fail); put("bytes_in", r.bytes_in); put("bytes_out", r.bytes_out);
        put("lat_usec", r.lat_usec); put("lat_usec_max", r.lat_usec_max);
        put("work_usec", r.work_usec); put("work_usec_max", r.work_usec_max);
        put("batches", r.batches); put("timed", r.timed);
        put("p50_usec", LLCacheMetrics::histPercentile(r.lat_hist, r.lat_usec_max, 0.50));
        put("p95_usec", LLCacheMetrics::histPercentile(r.lat_hist, r.lat_usec_max, 0.95));
        put("p99_usec", LLCacheMetrics::histPercentile(r.lat_hist, r.lat_usec_max, 0.99));
        put("work_p50_usec", LLCacheMetrics::histPercentile(r.work_hist, r.work_usec_max, 0.50));
        put("work_p95_usec", LLCacheMetrics::histPercentile(r.work_hist, r.work_usec_max, 0.95));
        put("work_p99_usec", LLCacheMetrics::histPercentile(r.work_hist, r.work_usec_max, 0.99));
        o["lat_hist"] = histJson(r.lat_hist, LLCacheMetrics::LAT_BUCKETS);     // [[bucket, count], ...]
        o["work_hist"] = histJson(r.work_hist, LLCacheMetrics::LAT_BUCKETS);
        return o;
    }

    boost::json::object storeJson()
    {
        const LLCache::Stats& s = sStore;
        boost::json::object o;
        auto put = [&o](const char* k, U64 v) { o[k] = (std::uint64_t)v; };
        o["dir"] = sStoreDir;
        o["ready"] = s.ready;
        o["read_only"] = s.read_only;
        o["drive"] = driveName(s.drive);
        put("segments", s.segments); put("index_files", sIndexFiles); put("files", (U64)s.segments + sIndexFiles);
        put("records", s.records); put("disk_bytes", s.disk_bytes); put("live_bytes", s.live_bytes);
        put("dead_bytes", s.dead_bytes); put("peak_disk_bytes", s.peak_disk_bytes); put("effective_limit", s.effective_limit);
        put("day", s.day);
        put("bytes_written_total", s.bytes_written_total);   // this run
        put("written_session_new", s.bytes_written_session_new);
        put("written_session_compaction", s.bytes_written_session_compaction);
        put("written_session_meta", s.bytes_written_session_meta);
        // (the *_today_* values span every session of that UTC day: state, not this run)
        put("written_today_new", s.bytes_written_today_new);
        put("written_today_compaction", s.bytes_written_today_compaction);
        put("written_today_meta", s.bytes_written_today_meta);
        put("bytes_read", s.bytes_read); put("hits", s.hits); put("misses", s.misses);
        put("corrupt_dropped", s.corrupt_dropped); put("write_fail", s.write_fail); put("writes_refused", s.writes_refused);
        put("evicted_records", s.evicted_records); put("evicted_bytes", s.evicted_bytes);
        put("segments_reclaimed", s.segments_reclaimed); put("rescued_records", s.rescued_records);
        put("superseded", s.superseded); put("skipped", s.skipped); put("ram_fallback_bytes", s.ram_fallback_bytes);
        put("prefetch_bytes", s.prefetch_bytes); put("prefetch_hits", s.prefetch_hits);
        put("encode_count", s.encode_count); put("encode_usec", s.encode_usec);
        put("encode_in_bytes", s.encode_in_bytes); put("encode_out_bytes", s.encode_out_bytes);
        put("decode_count", s.decode_count); put("decode_usec", s.decode_usec);
        put("decode_in_bytes", s.decode_in_bytes); put("decode_out_bytes", s.decode_out_bytes);
        put("append_count", s.append_count); put("append_usec", s.append_usec); put("append_bytes", s.append_bytes);
        put("index_load_usec", s.index_load_usec);
        put("index_bytes", s.index_bytes); put("index_records", s.index_records);   // #866: index RAM
        boost::json::object groups;
        for (size_t g = 0; g < (size_t)LLCache::Group::COUNT; ++g)
        {
            boost::json::object go;
            go["budget_bytes"] = (std::uint64_t)s.groups[g].budget_bytes;
            go["disk_bytes"] = (std::uint64_t)s.groups[g].disk_bytes;
            go["live_bytes"] = (std::uint64_t)s.groups[g].live_bytes;
            go["records"] = (std::uint64_t)s.groups[g].records;
            groups[LLCache::groupName((LLCache::Group)g)] = go;
        }
        o["groups"] = groups;
        boost::json::object kinds;
        for (size_t k = 1; k < NKIND; ++k)
        {
            const LLCache::KindStats& ks = s.kinds[k];
            boost::json::object ko;
            ko["records"] = (std::uint64_t)ks.records;
            ko["live_bytes"] = (std::uint64_t)ks.live_bytes;
            ko["raw_bytes"] = (std::uint64_t)ks.raw_bytes;
            ko["disk_bytes"] = (std::uint64_t)ks.disk_bytes;
            ko["segments"] = (std::uint64_t)ks.segments;
            ko["evicted_records"] = (std::uint64_t)ks.evicted_records;
            ko["evicted_bytes"] = (std::uint64_t)ks.evicted_bytes;
            kinds[LLCache::kindName((LLCache::Kind)k)] = ko;
        }
        o["kinds"] = kinds;
        return o;
    }

    std::string writeSessionJson()
    {
        const U32 end = (U32)std::time(nullptr);
        boost::json::object root;
        root["schema"] = "slipstream-cache-metrics";
        root["version"] = 2;   // 2: rows' use_* fields, "stages" (additive)

        boost::json::object build;
        build["channel_version"] = LLVersionInfo::instance().getChannelAndVersion();
        build["channel"] = LLVersionInfo::instance().getChannel();
        build["version"] = LLVersionInfo::instance().getVersion();
        build["config"] = LLVersionInfo::instance().getBuildConfig();
        root["build"] = build;

        boost::json::object session;
        session["start_unix"] = (std::uint64_t)sSessionStart;
        session["end_unix"] = (std::uint64_t)end;
        session["duration_s"] = (std::uint64_t)(end > sSessionStart ? end - sSessionStart : 0);
        session["store_read_only"] = sHaveStore && sStore.read_only;
        root["session"] = session;

        boost::json::object settings;
        for (const char* name : BACKEND_SETTINGS)
        {
            if (!gSavedSettings.controlExists(name)) { settings[name] = nullptr; continue; }
            LLControlVariablePtr cv = gSavedSettings.getControl(name);
            settings[name] = cv ? LlsdToJson(cv->getValue()) : boost::json::value(nullptr);
        }
        root["settings"] = settings;

        boost::json::object backends;
        backends["gputex"] = sGpuTexBackend;
        root["backends"] = backends;

        LLCache::DriveType drive = sHaveStore ? sStore.drive : LLCache::DriveType::UNKNOWN;
        if (drive == LLCache::DriveType::UNKNOWN) drive = LLCache::detectDriveType(gDirUtilp->getCacheDir());
        root["drive"] = driveName(drive);

        boost::json::object startup;
        startup["legacy"] = (std::uint64_t)LLCacheMetrics::startupUsec(Backend::LEGACY);
        startup["store"] = (std::uint64_t)LLCacheMetrics::startupUsec(Backend::STORE);
        root["startup_usec"] = startup;

        boost::json::array lows;
        for (size_t i = 0; i < LLCacheMetrics::LAT_BUCKETS; ++i) lows.emplace_back((std::uint64_t)LLCacheMetrics::latBucketLow(i));
        root["lat_bucket_low_usec"] = lows;

        boost::json::array rows;
        for (size_t c = 0; c < (size_t)Cache::COUNT; ++c)
        {
            for (size_t b = 0; b < (size_t)Backend::COUNT; ++b)
            {
                const LLCacheMetrics::Row r = LLCacheMetrics::snapshot((Cache)c, (Backend)b);
                if (LLCacheMetrics::rowEmpty(r) && !LLCacheMetrics::startupUsec((Cache)c, (Backend)b)) continue;
                rows.emplace_back(rowJson((Cache)c, (Backend)b, r));
            }
        }
        root["rows"] = rows;

        boost::json::array stages;
        for (size_t s = 0; s < (size_t)LLCacheMetrics::Stage::COUNT; ++s)
        {
            const LLCacheMetrics::StageRow r = LLCacheMetrics::stageSnapshot((LLCacheMetrics::Stage)s);
            if (!LLCacheMetrics::stageEmpty(r)) stages.emplace_back(stageJson((LLCacheMetrics::Stage)s, r));
        }
        root["stages"] = stages;
        root["store"] = sHaveStore ? boost::json::value(storeJson()) : boost::json::value(nullptr);

        char ts[32] = {};
        const std::time_t now = (std::time_t)end;
        std::strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", std::localtime(&now));
        std::string path = gDirUtilp->getExpandedFilename(LL_PATH_LOGS, std::string("cache_metrics_") + ts + ".json");
        for (int n = 2; LLFile::isfile(path) && n < 100; ++n)
            path = gDirUtilp->getExpandedFilename(LL_PATH_LOGS, llformat("cache_metrics_%s_%d.json", ts, n));
        llofstream out(path.c_str(), std::ios::out | std::ios::trunc);
        if (!out.is_open())
        {
            LL_WARNS("CacheMetrics") << "Cannot write " << path << LL_ENDL;
            return std::string();
        }
        out << boost::json::serialize(root) << "\n";
        out.close();
        return path;
    }
}

namespace SLCacheMetrics
{

void init()
{
    if (sInited) return;
    sInited = true;
    sDone = false;
    sSessionStart = (U32)std::time(nullptr);
    sLastSample = sLastLog = LLFrameTimer::getElapsedSeconds();
    sampleGpuTexBackend();
    gIdleCallbacks.addFunction(idle, nullptr);
}

void sampleNow()
{
    sampleGpuTexBackend();
    LLGpuTexCache::sampleMetrics();
    sampleStore();
}

void logReport(const char* when)
{
    if (!when || !*when) when = "menu";
    sampleNow();
    const U32 now = (U32)std::time(nullptr);
    LL_INFOS("CacheMetrics") << "CacheMetrics begin when=" << when << " uptime_s="
                             << (now > sSessionStart ? now - sSessionStart : 0) << " gputex_backend=" << sGpuTexBackend
                             << LL_ENDL;
    logLines(LLCacheMetrics::reportKV("CacheMetrics"));
    logLines(storeKV("CacheMetrics"));
    LL_INFOS("CacheMetrics") << "Cache metrics (" << when << "):\n" << LLCacheMetrics::report() << LL_ENDL;
    if (SLCacheStore::instance().isOpen()) SLCacheStore::instance().logStats(when);
    LL_INFOS("CacheMetrics") << "CacheMetrics end when=" << when << LL_ENDL;
}

void reset()
{
    LLCacheMetrics::reset();
    // Store eviction deltas restart from the current totals.
    if (LLCache::Store* st = SLCacheStore::instance().store())
    {
        const LLCache::Stats s = st->stats();
        for (size_t k = 0; k < NKIND; ++k)
        {
            sEvictRec[k] = s.kinds[k].evicted_records;
            sEvictBytes[k] = s.kinds[k].evicted_bytes;
        }
    }
    LL_INFOS("CacheMetrics") << "CacheMetrics reset (footprint and startup kept)" << LL_ENDL;
}

void beforeStoreClose()
{
    if (!sInited || sDone) return;
    sampleNow();
}

void shutdown()
{
    if (!sInited || sDone) return;
    gIdleCallbacks.deleteFunction(idle, nullptr);
    // (the store is closed by now: its last sample from beforeStoreClose() stands)
    LLGpuTexCache::sampleMetrics();
    logReport("shutdown");
    const std::string path = writeSessionJson();
    if (!path.empty()) LL_INFOS("CacheMetrics") << "CacheMetrics session summary written to " << path << LL_ENDL;
    sDone = true;
}

} // namespace SLCacheMetrics
