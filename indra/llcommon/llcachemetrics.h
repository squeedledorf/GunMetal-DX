/**
 * @file llcachemetrics.h
 * @brief Consumer-level disk cache metrics (#715): the same counters on the legacy
 *        caches (lltexturecache, LLDiskCache/LLFileSystem, the .gtx files, the cluster
 *        page files) and on the segment store, so the two can be compared on equal
 *        terms. Counted at the consumer boundary (did the request get its bytes from
 *        disk, how long did it take, what did it cost to write), not inside a backend.
 *
 *        Thread-safe (relaxed atomics); callable from any thread, including the
 *        llfilesystem library and the store's callbacks.
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

#ifndef LL_LLCACHEMETRICS_H
#define LL_LLCACHEMETRICS_H

#include "stdtypes.h"

#include <string>

namespace LLCacheMetrics
{
    // What is being cached (not where): one row each, whichever backend serves it.
    enum class Cache : U8
    {
        J2C     = 0,  // texture codestreams
        GPUTEX  = 1,  // GPU-ready mip chains
        ASSET   = 2,  // non-mesh assets (sounds, animations, notecards, ...)
        MESH    = 3,  // mesh asset parts
        CLUSTER = 4,  // cluster LOD pages
        COUNT
    };
    const char* cacheName(Cache c);

    // Which implementation served the operation.
    enum class Backend : U8
    {
        LEGACY = 0,   // the old file-per-item / per-cache-file implementation
        STORE  = 1,   // the #708 segment store
        COUNT
    };
    const char* backendName(Backend b);

    // A lookup finished. hit = the bytes came from disk (complete enough for the
    // request); bytes = bytes returned to the consumer (decoded); usec = wall time from
    // the consumer's request to the bytes being available (queue wait included).
    void recordRead(Cache c, Backend b, bool hit, U64 bytes, U64 usec);

    // A write finished. payload = bytes the consumer asked to cache; disk = bytes that
    // actually hit the disk for it (compressed record + header, or file bytes; 0 if not
    // known at the call site — the backend's own totals then cover it); usec = wall
    // time from submit to durable/complete; ok = false on any failure or refusal.
    void recordWrite(Cache c, Backend b, U64 payload, U64 disk, U64 usec, bool ok);

    // An entry was evicted to stay under budget (not superseded, not corrupt).
    void recordEvict(Cache c, Backend b, U64 bytes);

    // A cached entry was found bad (failed checksum / truncated / undecodable) and dropped.
    void recordCorrupt(Cache c, Backend b);

    // Footprint, sampled by whoever owns the backend (absolute values, not deltas):
    // bytes on disk, live payload bytes, and the number of filesystem files it uses.
    void setFootprint(Cache c, Backend b, U64 disk_bytes, U64 live_bytes, U64 files);

    // Startup cost of a backend (index load / directory scan), in microseconds.
    void recordStartup(Backend b, U64 usec);

    // ---- additive (#715 measurement) -------------------------------------------------

    // Startup cost of one cache on one backend (e.g. the .gtx folder scan, the J2C header
    // cache read). The per-backend slot above is shared by every legacy cache (the last
    // caller wins); use this one for a per-cache number. The store's index is shared by
    // every kind: it records recordStartup(STORE, ...) only.
    void recordStartup(Cache c, Backend b, U64 usec);
    U64 startupUsec(Cache c, Backend b);

    // `count` evictions totalling `bytes` at once (a backend that only exposes running
    // totals: the store's per-kind eviction counters, sampled as deltas).
    void recordEvictions(Cache c, Backend b, U64 count, U64 bytes);

    // Fine read-latency histogram: quarter-octave buckets (4 per doubling of usec,
    // +-12.5%), bucket i covers [latBucketLow(i), latBucketLow(i + 1)). Values >= 2^31 us
    // land in the last bucket. Kept for every read and, separately, for hits only (a miss
    // is usually an index miss with ~0 latency, which drags the all-reads mean down when
    // hit rates differ: compare backends on hit latency).
    constexpr size_t LAT_BUCKETS = 124;
    U64 latBucketLow(size_t i);
    size_t latBucket(U64 usec);

    // ---- additive: read use (is a whole-record read what the consumer needed?) --------
    // A read result (a GPU-ready mip chain, level 0 first) was consumed: the record held
    // `levels` levels, the consumer uploaded all but the top `skip` of them (0 = the full
    // chain was needed); bytes_read = the record's payload, bytes_needed = the part used.
    // Answers whether splitting a record into per-level records would pay. skip buckets:
    // 0, 1, 2, 3+ levels below the top.
    constexpr size_t USE_SKIP_BUCKETS = 4;
    void recordReadUse(Cache c, Backend b, U32 levels, U32 skip, U64 bytes_read, U64 bytes_needed);
    // A read result was taken but not used at all (rejected: other format / coverage).
    void recordReadUnused(Cache c, Backend b, U64 bytes_read);

    // ---- additive: decode / transcode stages ------------------------------------------
    // What turning a cached item into GPU-ready data costs outside the cache read itself,
    // so the J2C path (J2C read + decode + BC encode) compares with the GPU-ready path
    // (one read, request -> decoded chain).
    enum class Stage : U8
    {
        J2C_DECODE = 0,   // J2C codestream -> raw pixels (image decode thread, OpenJPEG)
        BC_ENCODE  = 1,   // raw pixels -> BC blocks (GPU compute, timed per frame batch)
        COUNT
    };
    const char* stageName(Stage s);
    // One item through a stage. latency_usec = request -> done (queue wait included, as
    // the cache rows' read latency); work_usec = the stage's own time (decode CPU time);
    // bytes_in / bytes_out = input / output bytes; ok = false on failure (bytes not added).
    void recordStage(Stage s, bool ok, U64 bytes_in, U64 bytes_out, U64 latency_usec, U64 work_usec);
    // A batch whose items can't be timed one by one (the GPU encodes of one frame): n items,
    // their bytes, the batch's work time (0 = untimed). The work histogram then holds one
    // entry per timed batch; no latency is recorded.
    void recordStageBatch(Stage s, U64 n, U64 bytes_in, U64 bytes_out, U64 work_usec);
    struct StageRow
    {
        U64 count = 0, fail = 0, bytes_in = 0, bytes_out = 0;
        U64 lat_usec = 0, lat_usec_max = 0, work_usec = 0, work_usec_max = 0;
        U64 batches = 0, timed = 0;              // recordStageBatch calls; entries in work_hist
        U64 lat_hist[LAT_BUCKETS] = {};
        U64 work_hist[LAT_BUCKETS] = {};
    };
    StageRow stageSnapshot(Stage s);
    bool stageEmpty(const StageRow& r);

    // One snapshot row; read with snapshot().
    struct Row
    {
        U64 reads = 0, hits = 0, read_bytes = 0, read_usec = 0, read_usec_max = 0;
        U64 read_usec_hist[8] = {};  // <100us, <1ms, <5ms, <20ms, <100ms, <500ms, <2s, >=2s
        U64 writes = 0, write_fail = 0, write_payload = 0, write_disk = 0, write_usec = 0;
        U64 evict = 0, evict_bytes = 0, corrupt = 0;
        U64 disk_bytes = 0, live_bytes = 0, files = 0;
        // additive (#715 measurement)
        U64 hit_usec = 0, hit_usec_max = 0;      // wall time of the hits only
        U64 lat_hist[LAT_BUCKETS] = {};          // every read (fine buckets, see latBucket)
        U64 hit_lat_hist[LAT_BUCKETS] = {};      // hits only
        // additive (read use, see recordReadUse)
        U64 use_count = 0, use_full = 0, use_levels = 0, use_bytes_read = 0, use_bytes_needed = 0;
        U64 use_skip_hist[USE_SKIP_BUCKETS] = {};
        U64 use_unused = 0, use_unused_bytes = 0;
    };
    Row snapshot(Cache c, Backend b);
    U64 startupUsec(Backend b);

    // True when the row has nothing to show (no reads, writes, evictions or footprint).
    bool rowEmpty(const Row& r);
    // Read-latency percentile (p in [0, 1]) in usec from the fine histogram, linearly
    // interpolated inside the bucket and clamped to the observed max. hits_only = from
    // the hit histogram. 0 when there is no read.
    U64 latencyPercentile(const Row& r, F64 p, bool hits_only);
    // The same over any fine histogram (LAT_BUCKETS entries) with its observed max.
    U64 histPercentile(const U64* hist, U64 max_usec, F64 p);

    // Machine-parseable block: one line per non-empty row,
    //   "<tag> cache=<c> backend=<b> reads=.. hits=.. ... p95_usec=.. hit_p95_usec=.."
    // then one "<tag> startup backend=<b> [cache=<c>] usec=.." line per known startup,
    // (additive) one "<tag> use cache=<c> backend=<b> count=.. full=.. skip0=.. ..." line per
    // row with read-use data and one "<tag> stage stage=<s> count=.. ..." line per stage.
    // Every value is an integer (bytes / usec / counts), keys never contain spaces.
    std::string reportKV(const char* tag = "CacheMetrics");

    // Multi-line human-readable table of every non-empty row (hit rate, mean/max/p95
    // read latency, write amplification = disk/payload, footprint, compression ratio).
    std::string report();
    // Zero every counter (footprint and startup kept).
    void reset();
}

#endif // LL_LLCACHEMETRICS_H
