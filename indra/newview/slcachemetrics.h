/**
 * @file slcachemetrics.h
 * @brief Viewer side of the #715 cache metrics (LLCacheMetrics, llcommon): samples the
 *        segment store's per-kind footprint / evictions / index load time into the
 *        LLCacheMetrics rows, logs a "CacheMetrics" block periodically
 *        (CacheMetricsLogIntervalMin) and at shutdown, and writes a per-session summary
 *        <logs>/cache_metrics_<timestamp>.json for scripts/cache/compare_cache_metrics.py.
 *        Main thread only.
 *
 * Row conventions (both backends; see llcachemetrics.h for the counters):
 *   disk_bytes  bytes the cache occupies on disk. Store: the kind's bytes in the segments,
 *               live + dead (exact per-segment attribution). Legacy GPU-ready: file sizes
 *               rounded up to 4 KiB.
 *   live_bytes  decoded payload the consumer would get back (store: raw bytes of the live
 *               records; legacy GPU-ready: file minus its 128-byte header).
 *   files       filesystem files. Store: segment files holding the kind (a segment shared
 *               by kinds counts in each row; the store block has the true total, segments
 *               + index snapshot files).
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

#ifndef SL_SLCACHEMETRICS_H
#define SL_SLCACHEMETRICS_H

namespace SLCacheMetrics
{
    // After the caches are initialised: registers the idle tick (store sampling every
    // 15 s, the periodic log block).
    void init();
    // Sample now: the store's footprint / evictions / startup and the .gtx footprint.
    void sampleNow();
    // Log the human table, the machine-parseable block and the store stats.
    void logReport(const char* when);
    // Zero the counters (footprint and startup kept; LLCacheMetrics::reset()).
    void reset();
    // Right before SLCacheStore::close(): last sample while the store is still open.
    void beforeStoreClose();
    // App shutdown, after the caches stopped: final log block + the session JSON file.
    void shutdown();
}

#endif // SL_SLCACHEMETRICS_H
