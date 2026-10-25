/**
 * @file llcachemetrics_test.cpp
 * @brief Tests for the consumer-level cache metrics (llcachemetrics.h, #715).
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

#include <string>

#include "linden_common.h"

#include "../llcachemetrics.h"

#include "../test/lltut.h"

using LLCacheMetrics::Cache;
using LLCacheMetrics::Backend;

namespace
{
    size_t countLines(const std::string& s, const std::string& needle)
    {
        size_t n = 0, pos = 0;
        while ((pos = s.find(needle, pos)) != std::string::npos) { ++n; pos += needle.size(); }
        return n;
    }
}

namespace tut
{
    struct cachemetrics_data
    {
        cachemetrics_data()
        {
            LLCacheMetrics::reset();
            for (size_t c = 0; c < (size_t)Cache::COUNT; ++c)
                for (size_t b = 0; b < (size_t)Backend::COUNT; ++b)
                    LLCacheMetrics::setFootprint((Cache)c, (Backend)b, 0, 0, 0);
        }
    };
    typedef test_group<cachemetrics_data> cachemetrics_test;
    typedef cachemetrics_test::object cachemetrics_object;
    tut::cachemetrics_test cachemetrics("LLCacheMetrics");

    // Fine latency buckets: contiguous, monotonic, every value lands in the bucket
    // whose range holds it.
    template<> template<>
    void cachemetrics_object::test<1>()
    {
        for (size_t i = 0; i + 1 < LLCacheMetrics::LAT_BUCKETS; ++i)
            ensure("bucket edges increase", LLCacheMetrics::latBucketLow(i) < LLCacheMetrics::latBucketLow(i + 1));
        const U64 probes[] = { 0, 1, 3, 4, 5, 7, 8, 9, 15, 16, 99, 100, 1000, 4999, 5000, 123456, 2000000,
                               (1ull << 31) - 1 };
        for (U64 v : probes)
        {
            const size_t b = LLCacheMetrics::latBucket(v);
            ensure("value >= bucket low", v >= LLCacheMetrics::latBucketLow(b));
            ensure("value < next bucket low", b + 1 >= LLCacheMetrics::LAT_BUCKETS || v < LLCacheMetrics::latBucketLow(b + 1));
        }
        ensure_equals("huge values clamp to the last bucket", LLCacheMetrics::latBucket(~0ull),
                      LLCacheMetrics::LAT_BUCKETS - 1);
    }

    // Hit / miss accounting, hit-only latency, percentiles within the bucket precision.
    template<> template<>
    void cachemetrics_object::test<2>()
    {
        // 90 hits at 1000 us, 10 hits at 10000 us, 100 index misses at 0 us.
        for (int i = 0; i < 90; ++i) LLCacheMetrics::recordRead(Cache::GPUTEX, Backend::STORE, true, 100, 1000);
        for (int i = 0; i < 10; ++i) LLCacheMetrics::recordRead(Cache::GPUTEX, Backend::STORE, true, 100, 10000);
        for (int i = 0; i < 100; ++i) LLCacheMetrics::recordRead(Cache::GPUTEX, Backend::STORE, false, 0, 0);
        const LLCacheMetrics::Row r = LLCacheMetrics::snapshot(Cache::GPUTEX, Backend::STORE);
        ensure_equals("reads", r.reads, (U64)200);
        ensure_equals("hits", r.hits, (U64)100);
        ensure_equals("read bytes count hits only", r.read_bytes, (U64)10000);
        ensure_equals("hit usec", r.hit_usec, (U64)(90 * 1000 + 10 * 10000));
        ensure_equals("hit max", r.hit_usec_max, (U64)10000);
        const U64 hp50 = LLCacheMetrics::latencyPercentile(r, 0.50, true);
        ensure("hit p50 near 1 ms (bucket precision)", hp50 >= 875 && hp50 <= 1125);
        const U64 hp99 = LLCacheMetrics::latencyPercentile(r, 0.99, true);
        ensure("hit p99 near 10 ms", hp99 >= 8192 && hp99 <= 10000);
        // All reads: half are zero-latency misses.
        ensure_equals("all-reads p25 is a miss", LLCacheMetrics::latencyPercentile(r, 0.25, false), (U64)0);
        ensure("all-reads p95 in the slow hits",
               LLCacheMetrics::latencyPercentile(r, 0.97, false) >= 8192);
        const LLCacheMetrics::Row empty = LLCacheMetrics::snapshot(Cache::CLUSTER, Backend::LEGACY);
        ensure_equals("no reads: percentile 0", LLCacheMetrics::latencyPercentile(empty, 0.95, true), (U64)0);
    }

    // reset() zeroes counters but keeps footprint and startup; recordEvictions adds totals.
    template<> template<>
    void cachemetrics_object::test<3>()
    {
        LLCacheMetrics::recordRead(Cache::J2C, Backend::LEGACY, true, 10, 50);
        LLCacheMetrics::recordWrite(Cache::J2C, Backend::LEGACY, 100, 4096, 20, true);
        LLCacheMetrics::recordEvictions(Cache::J2C, Backend::STORE, 3, 300);
        LLCacheMetrics::recordEvict(Cache::J2C, Backend::STORE, 100);
        LLCacheMetrics::setFootprint(Cache::J2C, Backend::LEGACY, 8192, 100, 1);
        LLCacheMetrics::recordStartup(Backend::STORE, 1234);
        LLCacheMetrics::recordStartup(Cache::GPUTEX, Backend::LEGACY, 777);
        LLCacheMetrics::Row s = LLCacheMetrics::snapshot(Cache::J2C, Backend::STORE);
        ensure_equals("evictions add", s.evict, (U64)4);
        ensure_equals("evicted bytes add", s.evict_bytes, (U64)400);
        ensure_equals("per-cache startup", LLCacheMetrics::startupUsec(Cache::GPUTEX, Backend::LEGACY), (U64)777);
        ensure_equals("per-cache startup leaves the backend slot", LLCacheMetrics::startupUsec(Backend::LEGACY), (U64)0);

        LLCacheMetrics::reset();
        const LLCacheMetrics::Row r = LLCacheMetrics::snapshot(Cache::J2C, Backend::LEGACY);
        ensure_equals("reads reset", r.reads, (U64)0);
        ensure_equals("hit usec reset", r.hit_usec, (U64)0);
        ensure_equals("writes reset", r.writes, (U64)0);
        U64 hist = 0;
        for (size_t i = 0; i < LLCacheMetrics::LAT_BUCKETS; ++i) hist += r.lat_hist[i] + r.hit_lat_hist[i];
        ensure_equals("fine histograms reset", hist, (U64)0);
        ensure_equals("footprint kept", r.disk_bytes, (U64)8192);
        ensure_equals("files kept", r.files, (U64)1);
        ensure_equals("startup kept", LLCacheMetrics::startupUsec(Backend::STORE), (U64)1234);
        ensure_equals("per-cache startup kept", LLCacheMetrics::startupUsec(Cache::GPUTEX, Backend::LEGACY), (U64)777);
    }

    // reportKV: one line per non-empty row, the startup lines, integer key=value pairs.
    template<> template<>
    void cachemetrics_object::test<4>()
    {
        LLCacheMetrics::recordStartup(Backend::STORE, 0);
        LLCacheMetrics::recordStartup(Cache::GPUTEX, Backend::LEGACY, 0);
        LLCacheMetrics::recordRead(Cache::GPUTEX, Backend::LEGACY, true, 1000, 2000);
        LLCacheMetrics::recordWrite(Cache::MESH, Backend::STORE, 1000, 600, 300, true);
        LLCacheMetrics::setFootprint(Cache::ASSET, Backend::STORE, 4096, 2048, 2);
        LLCacheMetrics::recordStartup(Backend::STORE, 5000);
        const std::string kv = LLCacheMetrics::reportKV("CM");
        ensure_equals("three row lines", countLines(kv, "CM cache="), (size_t)3);
        ensure_equals("one startup line", countLines(kv, "CM startup "), (size_t)1);
        ensure("gputex legacy row", kv.find("CM cache=gputex backend=legacy reads=1 hits=1 read_bytes=1000 ") != std::string::npos);
        ensure("mesh store write", kv.find("cache=mesh backend=store") != std::string::npos
                                   && kv.find("write_payload=1000 write_disk=600") != std::string::npos);
        ensure("asset footprint", kv.find("disk_bytes=4096 live_bytes=2048 files=2") != std::string::npos);
        ensure("store startup", kv.find("CM startup backend=store usec=5000") != std::string::npos);
        ensure("no empty rows", kv.find("cache=cluster") == std::string::npos);
        ensure("human report mentions hit latency", LLCacheMetrics::report().find("hit lat") != std::string::npos);
    }

    // Read use: skip histogram (3+ bucket), full-chain count, needed <= read, unused reads,
    // its own reportKV line, reset.
    template<> template<>
    void cachemetrics_object::test<5>()
    {
        LLCacheMetrics::recordReadUse(Cache::GPUTEX, Backend::STORE, 11, 0, 1000, 1000);
        LLCacheMetrics::recordReadUse(Cache::GPUTEX, Backend::STORE, 11, 1, 1000, 250);
        LLCacheMetrics::recordReadUse(Cache::GPUTEX, Backend::STORE, 11, 2, 1000, 63);
        LLCacheMetrics::recordReadUse(Cache::GPUTEX, Backend::STORE, 11, 5, 1000, 2000);   // needed clamps to read
        LLCacheMetrics::recordReadUnused(Cache::GPUTEX, Backend::STORE, 700);
        const LLCacheMetrics::Row r = LLCacheMetrics::snapshot(Cache::GPUTEX, Backend::STORE);
        ensure_equals("uses", r.use_count, (U64)4);
        ensure_equals("full chain", r.use_full, (U64)1);
        ensure_equals("levels", r.use_levels, (U64)44);
        ensure_equals("skip 0", r.use_skip_hist[0], (U64)1);
        ensure_equals("skip 1", r.use_skip_hist[1], (U64)1);
        ensure_equals("skip 2", r.use_skip_hist[2], (U64)1);
        ensure_equals("skip 3+", r.use_skip_hist[3], (U64)1);
        ensure_equals("bytes read", r.use_bytes_read, (U64)4000);
        ensure_equals("bytes needed", r.use_bytes_needed, (U64)(1000 + 250 + 63 + 1000));
        ensure_equals("unused", r.use_unused, (U64)1);
        ensure_equals("unused bytes", r.use_unused_bytes, (U64)700);
        ensure("a use-only row is not empty", !LLCacheMetrics::rowEmpty(r));
        const std::string kv = LLCacheMetrics::reportKV("CM");
        ensure("use line", kv.find("CM use cache=gputex backend=store count=4 full=1 levels=44 skip0=1 skip1=1 skip2=1 "
                                   "skip3plus=1 bytes_read=4000 bytes_needed=2313 unused=1 unused_bytes=700")
                               != std::string::npos);
        ensure("human report has read use", LLCacheMetrics::report().find("read use:") != std::string::npos);
        LLCacheMetrics::reset();
        const LLCacheMetrics::Row z = LLCacheMetrics::snapshot(Cache::GPUTEX, Backend::STORE);
        ensure_equals("use reset", z.use_count + z.use_unused + z.use_bytes_read + z.use_skip_hist[3], (U64)0);
    }

    // Stages: per-item latency / work percentiles, failures, untimed and timed batches, reset.
    template<> template<>
    void cachemetrics_object::test<6>()
    {
        for (int i = 0; i < 99; ++i)
            LLCacheMetrics::recordStage(LLCacheMetrics::Stage::J2C_DECODE, true, 100, 400, 4000, 2000);
        LLCacheMetrics::recordStage(LLCacheMetrics::Stage::J2C_DECODE, true, 100, 400, 40000, 20000);
        LLCacheMetrics::recordStage(LLCacheMetrics::Stage::J2C_DECODE, false, 100, 0, 5, 5);
        LLCacheMetrics::StageRow d = LLCacheMetrics::stageSnapshot(LLCacheMetrics::Stage::J2C_DECODE);
        ensure_equals("count includes failures", d.count, (U64)101);
        ensure_equals("fail", d.fail, (U64)1);
        ensure_equals("bytes in (ok only)", d.bytes_in, (U64)10000);
        ensure_equals("bytes out", d.bytes_out, (U64)40000);
        ensure_equals("lat max", d.lat_usec_max, (U64)40000);
        const U64 p50 = LLCacheMetrics::histPercentile(d.lat_hist, d.lat_usec_max, 0.50);
        ensure("lat p50 near 4 ms", p50 >= 3500 && p50 <= 4500);
        const U64 w50 = LLCacheMetrics::histPercentile(d.work_hist, d.work_usec_max, 0.50);
        ensure("work p50 near 2 ms", w50 >= 1750 && w50 <= 2250);

        LLCacheMetrics::recordStageBatch(LLCacheMetrics::Stage::BC_ENCODE, 8, 8000, 1000, 0);    // untimed
        LLCacheMetrics::recordStageBatch(LLCacheMetrics::Stage::BC_ENCODE, 4, 4000, 500, 300);
        LLCacheMetrics::recordStageBatch(LLCacheMetrics::Stage::BC_ENCODE, 0, 1, 1, 1);          // ignored
        const LLCacheMetrics::StageRow e = LLCacheMetrics::stageSnapshot(LLCacheMetrics::Stage::BC_ENCODE);
        ensure_equals("batch items", e.count, (U64)12);
        ensure_equals("batches", e.batches, (U64)2);
        ensure_equals("timed batches", e.timed, (U64)1);
        ensure_equals("batch work", e.work_usec, (U64)300);
        ensure_equals("batch bytes in", e.bytes_in, (U64)12000);
        ensure_equals("no batch latency", e.lat_usec + e.lat_usec_max, (U64)0);

        const std::string kv = LLCacheMetrics::reportKV("CM");
        ensure_equals("two stage lines", countLines(kv, "CM stage stage="), (size_t)2);
        ensure("decode stage line", kv.find("CM stage stage=j2c_decode count=101 fail=1 bytes_in=10000 bytes_out=40000 ")
                                    != std::string::npos);
        ensure("encode stage line", kv.find("stage=bc_encode count=12 ") != std::string::npos);
        ensure("human report has stages", LLCacheMetrics::report().find("stage j2c_decode") != std::string::npos);

        LLCacheMetrics::reset();
        d = LLCacheMetrics::stageSnapshot(LLCacheMetrics::Stage::J2C_DECODE);
        ensure("stage reset", LLCacheMetrics::stageEmpty(d) && d.lat_usec_max == 0 && d.work_hist[LLCacheMetrics::latBucket(2000)] == 0);
        ensure_equals("no stage lines after reset", countLines(LLCacheMetrics::reportKV("CM"), "CM stage "), (size_t)0);
    }
}
