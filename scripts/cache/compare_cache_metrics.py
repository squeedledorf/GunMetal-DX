#!/usr/bin/env python3
"""Compare Slipstream cache-metrics session files (#715).

The viewer writes <logs>/cache_metrics_<YYYYmmdd_HHMMSS>.json at shutdown (see
indra/newview/slcachemetrics.h and research/CACHE_METRICS_PROTOCOL.md). This script
puts sessions side by side, per cache (j2c, gputex, asset, mesh, cluster):

    compare_cache_metrics.py a.json b.json            # one column per file and backend
    compare_cache_metrics.py --legacy l1.json l2.json --store s1.json s2.json
    compare_cache_metrics.py --group warm-legacy l*.json --group warm-store s*.json

A group sums the counters and latency histograms of its sessions (so p95 is over every
read of every session), takes the footprint from its latest session and averages the
startup times. --legacy / --store groups keep only that backend's rows; plain files
and --group keep every backend (one column each). Deltas are against the first column;
"better"/"worse" follows each metric's direction. Missing rows / fields print "-".

Schema version 2 (additive) adds, per row, the read-use fields (use_*: what a whole-record
GPU-ready read was needed for) and the top-level "stages" (j2c_decode, bc_encode: the J2C
path's decode / encode cost). Stages are session-wide, so every column of a session shows
the same stage numbers. Version 1 files simply have none of them.

Standard library only.
"""

import argparse
import json
import math
import os
import sys

CACHES = ["j2c", "gputex", "asset", "mesh", "cluster"]
SUM_FIELDS = ["reads", "hits", "read_bytes", "read_usec", "hit_usec", "writes", "write_fail",
              "write_payload", "write_disk", "write_usec", "evict", "evict_bytes", "corrupt",
              "use_count", "use_full", "use_levels", "use_bytes_read", "use_bytes_needed",
              "use_unused", "use_unused_bytes"]
USE_SKIP_BUCKETS = 4   # levels skipped below the top: 0, 1, 2, 3+
STAGES = ["j2c_decode", "bc_encode"]
STAGE_SUM = ["count", "fail", "bytes_in", "bytes_out", "lat_usec", "work_usec", "batches", "timed"]
STAGE_MAX = ["lat_usec_max", "work_usec_max"]
MAX_FIELDS = ["read_usec_max", "hit_usec_max"]
STATE_FIELDS = ["disk_bytes", "live_bytes", "files"]
STORE_SUM = ["bytes_written_total", "written_session_new", "written_session_compaction", "written_session_meta",
             "bytes_read", "hits", "misses", "corrupt_dropped", "write_fail", "writes_refused",
             "evicted_records", "evicted_bytes", "segments_reclaimed", "rescued_records", "superseded", "skipped",
             "encode_count", "encode_usec", "encode_in_bytes", "encode_out_bytes",
             "decode_count", "decode_usec", "decode_in_bytes", "decode_out_bytes",
             "append_count", "append_usec", "append_bytes"]
STORE_STATE = ["segments", "index_files", "files", "records", "disk_bytes", "live_bytes", "dead_bytes",
               "peak_disk_bytes", "effective_limit"]
MB = 1024.0 * 1024.0


def num(v, default=0):
    """A number from a JSON value; anything else is the default."""
    if isinstance(v, bool):
        return int(v)
    if isinstance(v, (int, float)):
        return v
    return default


def load(path):
    try:
        with open(path, "r", encoding="utf-8") as f:
            d = json.load(f)
    except (OSError, ValueError) as e:
        print("warning: skipping %s: %s" % (path, e), file=sys.stderr)
        return None
    if not isinstance(d, dict) or not isinstance(d.get("rows", []), list):
        print("warning: skipping %s: not a cache metrics session file" % path, file=sys.stderr)
        return None
    d["_path"] = path
    return d


class Column:
    """One column: a set of sessions, optionally restricted to one backend."""

    def __init__(self, label, sessions, backend):
        self.label = label
        self.sessions = sessions
        self.backend = backend   # "legacy" / "store"
        self.lows = None
        for s in sessions:
            lows = s.get("lat_bucket_low_usec")
            if isinstance(lows, list) and lows:
                self.lows = [num(x) for x in lows]
                break
        self.duration_s = sum(num(s.get("session", {}).get("duration_s")) for s in sessions)
        self.rows = {}
        for cache in CACHES:
            r = self._merge(cache)
            if r is not None:
                self.rows[cache] = r
        self.store = self._merge_store() if backend == "store" else None
        self.stages = {}
        for stage in STAGES:
            st = self._merge_stage(stage)
            if st is not None:
                self.stages[stage] = st

    def _latest(self):
        return sorted(self.sessions, key=lambda s: num(s.get("session", {}).get("end_unix")))

    def _merge(self, cache):
        found = []
        for s in self._latest():
            for r in s.get("rows", []):
                if isinstance(r, dict) and r.get("cache") == cache and r.get("backend") == self.backend:
                    found.append((s, r))
        if not found:
            return None
        m = {f: 0 for f in SUM_FIELDS}
        for f in MAX_FIELDS:
            m[f] = 0
        m["use_skip"] = [0] * USE_SKIP_BUCKETS
        lat, hit = {}, {}
        startups, backend_startups = [], []
        p95s, hp95s = [], []
        for s, r in found:
            for f in SUM_FIELDS:
                m[f] += num(r.get(f))
            for f in MAX_FIELDS:
                m[f] = max(m[f], num(r.get(f)))
            skip = r.get("use_skip_hist")
            if isinstance(skip, list):
                for i, v in enumerate(skip[:USE_SKIP_BUCKETS]):
                    m["use_skip"][i] += num(v)
            for key, acc in (("lat_hist", lat), ("hit_lat_hist", hit)):
                for pair in r.get(key, []) or []:
                    if isinstance(pair, list) and len(pair) == 2:
                        acc[int(num(pair[0]))] = acc.get(int(num(pair[0])), 0) + num(pair[1])
            if num(r.get("startup_usec")):
                startups.append(num(r.get("startup_usec")))
            bs = num(s.get("startup_usec", {}).get(self.backend)) if isinstance(s.get("startup_usec"), dict) else 0
            if bs:
                backend_startups.append(bs)
            p95s.append(num(r.get("p95_usec")))
            hp95s.append(num(r.get("hit_p95_usec")))
        last = found[-1][1]
        for f in STATE_FIELDS:
            m[f] = num(last.get(f))
        m["lat"] = lat
        m["hit_lat"] = hit
        m["sessions"] = len(found)
        # Per-cache startup when recorded; else the backend's (shared by every cache on it).
        if startups:
            m["startup_usec"] = sum(startups) / len(startups)
            m["startup_shared"] = False
        elif backend_startups:
            m["startup_usec"] = sum(backend_startups) / len(backend_startups)
            m["startup_shared"] = True
        m["p95_fallback"] = max(p95s) if p95s else 0
        m["hit_p95_fallback"] = max(hp95s) if hp95s else 0
        return m

    def _merge_stage(self, stage):
        found = []
        for s in self._latest():
            for r in s.get("stages", []) or []:
                if isinstance(r, dict) and r.get("stage") == stage:
                    found.append(r)
        if not found:
            return None
        m = {f: sum(num(r.get(f)) for r in found) for f in STAGE_SUM}
        for f in STAGE_MAX:
            m[f] = max(num(r.get(f)) for r in found)
        for key in ("lat_hist", "work_hist"):
            acc = {}
            for r in found:
                for pair in r.get(key, []) or []:
                    if isinstance(pair, list) and len(pair) == 2:
                        acc[int(num(pair[0]))] = acc.get(int(num(pair[0])), 0) + num(pair[1])
            m[key] = acc
        m["sessions"] = len(found)
        return m

    def _merge_store(self):
        blocks = [s.get("store") for s in self._latest() if isinstance(s.get("store"), dict)]
        if not blocks:
            return None
        m = {f: sum(num(b.get(f)) for b in blocks) for f in STORE_SUM}
        for f in STORE_STATE:
            m[f] = num(blocks[-1].get(f))
        loads = [num(b.get("index_load_usec")) for b in blocks if num(b.get("index_load_usec"))]
        m["index_load_usec"] = sum(loads) / len(loads) if loads else None
        m["drive"] = blocks[-1].get("drive", "?")
        m["sessions"] = len(blocks)
        return m

    def percentile(self, hist, p, mx):
        if not hist or not self.lows:
            return None
        total = sum(hist.values())
        if total <= 0:
            return None
        want = max(1.0, p * total)
        seen = 0.0
        n = len(self.lows)
        for i in sorted(hist):
            c = hist[i]
            if c <= 0:
                continue
            if seen + c >= want:
                lo = self.lows[i] if i < n else self.lows[-1]
                hi = self.lows[i + 1] if i + 1 < n else max(mx, lo + 1)
                v = lo + (hi - lo) * (want - seen) / c
                return min(v, mx) if mx else v
            seen += c
        return mx


def ratio(a, b):
    if a is None or b is None or not b:
        return None
    return float(a) / float(b)


# Each metric: (label, function(column, row) -> value or None, format, direction)
# direction: +1 higher is better, -1 lower is better, 0 informational.
def metrics():
    def per_hour(col, v):
        return v / (col.duration_s / 3600.0) if col.duration_s and v is not None else None

    def lat_p(field_hist, field_max, fallback, p):
        def f(col, r):
            v = col.percentile(r[field_hist], p, r[field_max])
            if v is None and r.get(fallback) and p == 0.95:
                v = r[fallback]
            return v / 1000.0 if v is not None else None
        return f

    return [
        ("sessions", lambda c, r: r["sessions"], "%d", 0),
        ("reads", lambda c, r: r["reads"], "%d", 0),
        ("hit rate %", lambda c, r: 100.0 * r["hits"] / r["reads"] if r["reads"] else None, "%.1f", +1),
        ("hit latency mean ms", lambda c, r: r["hit_usec"] / r["hits"] / 1000.0 if r["hits"] else None, "%.2f", -1),
        ("hit latency p50 ms", lat_p("hit_lat", "hit_usec_max", "hit_p95_fallback", 0.50), "%.2f", -1),
        ("hit latency p95 ms", lat_p("hit_lat", "hit_usec_max", "hit_p95_fallback", 0.95), "%.2f", -1),
        ("hit latency p99 ms", lat_p("hit_lat", "hit_usec_max", "hit_p95_fallback", 0.99), "%.2f", -1),
        ("read latency p95 ms (all)", lat_p("lat", "read_usec_max", "p95_fallback", 0.95), "%.2f", -1),
        ("MB read from cache", lambda c, r: r["read_bytes"] / MB, "%.1f", 0),
        ("writes", lambda c, r: r["writes"], "%d", 0),
        ("write failures", lambda c, r: r["write_fail"], "%d", -1),
        ("MB fetched (write payload)", lambda c, r: r["write_payload"] / MB, "%.1f", 0),
        ("MB written (write disk)", lambda c, r: r["write_disk"] / MB, "%.1f", 0),
        ("write amp (written/fetched)", lambda c, r: ratio(r["write_disk"], r["write_payload"]), "%.3f", -1),
        ("MB written per hour", lambda c, r: per_hour(c, r["write_disk"] / MB), "%.1f", -1),
        ("write latency mean ms", lambda c, r: r["write_usec"] / (r["writes"] - r["write_fail"]) / 1000.0
         if r["writes"] - r["write_fail"] > 0 else None, "%.2f", -1),
        ("evictions", lambda c, r: r["evict"], "%d", 0),
        ("MB evicted", lambda c, r: r["evict_bytes"] / MB, "%.1f", 0),
        ("corrupt", lambda c, r: r["corrupt"], "%d", -1),
        ("disk MB (latest)", lambda c, r: r["disk_bytes"] / MB if r["disk_bytes"] else None, "%.1f", 0),
        ("live MB (latest)", lambda c, r: r["live_bytes"] / MB if r["live_bytes"] else None, "%.1f", 0),
        ("disk MB per live MB", lambda c, r: ratio(r["disk_bytes"], r["live_bytes"]), "%.3f", -1),
        ("files (latest)", lambda c, r: r["files"] if r["files"] else None, "%d", -1),
        ("startup ms", lambda c, r: r["startup_usec"] / 1000.0 if r.get("startup_usec") else None,
         "%.1f", -1),
        # read use (schema 2; GPU-ready chains): is the whole-record read what was needed?
        ("read use: results used", lambda c, r: r["use_count"] if r["use_count"] else None, "%d", 0),
        ("  needed the full chain %", lambda c, r: 100.0 * r["use_full"] / r["use_count"] if r["use_count"] else None,
         "%.1f", 0),
        ("  levels per record (mean)", lambda c, r: ratio(r["use_levels"], r["use_count"]), "%.2f", 0),
        ("  skipped 1 level %", lambda c, r: 100.0 * r["use_skip"][1] / r["use_count"] if r["use_count"] else None,
         "%.1f", 0),
        ("  skipped 2 levels %", lambda c, r: 100.0 * r["use_skip"][2] / r["use_count"] if r["use_count"] else None,
         "%.1f", 0),
        ("  skipped 3+ levels %", lambda c, r: 100.0 * r["use_skip"][3] / r["use_count"] if r["use_count"] else None,
         "%.1f", 0),
        ("  MB read (used results)", lambda c, r: r["use_bytes_read"] / MB if r["use_count"] else None, "%.1f", 0),
        ("  MB needed", lambda c, r: r["use_bytes_needed"] / MB if r["use_count"] else None, "%.1f", 0),
        ("  needed / read %", lambda c, r: 100.0 * r["use_bytes_needed"] / r["use_bytes_read"]
         if r["use_bytes_read"] else None, "%.1f", +1),
        ("  unused results", lambda c, r: r["use_unused"] if r["use_count"] or r["use_unused"] else None, "%d", -1),
        ("  MB unused", lambda c, r: r["use_unused_bytes"] / MB if r["use_count"] or r["use_unused"] else None,
         "%.1f", -1),
    ]


def stage_metrics():
    def pct(hist_key, max_key, p):
        def f(col, s):
            v = col.percentile(s[hist_key], p, s[max_key])
            return v / 1000.0 if v is not None else None
        return f

    def ok(s):
        return s["count"] - min(s["fail"], s["count"])

    return [
        ("sessions", lambda c, s: s["sessions"], "%d", 0),
        ("items", lambda c, s: s["count"], "%d", 0),
        ("failures", lambda c, s: s["fail"], "%d", -1),
        ("MB in", lambda c, s: s["bytes_in"] / MB, "%.1f", 0),
        ("MB out", lambda c, s: s["bytes_out"] / MB, "%.1f", 0),
        ("latency mean ms (request -> done)", lambda c, s: s["lat_usec"] / ok(s) / 1000.0
         if ok(s) and not s["batches"] else None, "%.2f", -1),
        ("latency p50 ms", pct("lat_hist", "lat_usec_max", 0.50), "%.2f", -1),
        ("latency p95 ms", pct("lat_hist", "lat_usec_max", 0.95), "%.2f", -1),
        ("latency p99 ms", pct("lat_hist", "lat_usec_max", 0.99), "%.2f", -1),
        ("work mean ms (per item / per batch)", lambda c, s: ratio(s["work_usec"] / 1000.0, s["timed"]), "%.2f", -1),
        ("work p50 ms", pct("work_hist", "work_usec_max", 0.50), "%.2f", -1),
        ("work p95 ms", pct("work_hist", "work_usec_max", 0.95), "%.2f", -1),
        ("work p99 ms", pct("work_hist", "work_usec_max", 0.99), "%.2f", -1),
        ("work s total", lambda c, s: s["work_usec"] / 1e6, "%.2f", -1),
        ("work ms per MB out", lambda c, s: ratio(s["work_usec"] / 1000.0, s["bytes_out"] / MB), "%.2f", -1),
        ("batches (timed)", lambda c, s: "%d (%d)" % (s["batches"], s["timed"]) if s["batches"] else None, "%s", 0),
    ]


def store_metrics():
    def per_hour(col, v):
        return v / (col.duration_s / 3600.0) if col.duration_s and v is not None else None

    return [
        ("sessions", lambda c, s: s["sessions"], "%d", 0),
        ("drive", lambda c, s: s["drive"], "%s", 0),
        ("SSD MB written (all classes)", lambda c, s: s["bytes_written_total"] / MB, "%.1f", 0),
        ("SSD MB written per hour", lambda c, s: per_hour(c, s["bytes_written_total"] / MB), "%.1f", -1),
        ("SSD MB per day (extrapolated)", lambda c, s: per_hour(c, s["bytes_written_total"] / MB) * 24
         if per_hour(c, s["bytes_written_total"] / MB) is not None else None, "%.0f", -1),
        ("  of which new MB", lambda c, s: s["written_session_new"] / MB, "%.1f", 0),
        ("  of which compaction MB", lambda c, s: s["written_session_compaction"] / MB, "%.1f", -1),
        ("  of which meta MB", lambda c, s: s["written_session_meta"] / MB, "%.1f", -1),
        ("SSD write amp (all / new)", lambda c, s: ratio(s["bytes_written_total"], s["written_session_new"]), "%.3f", -1),
        ("append MB/s (pwrite)", lambda c, s: ratio(s["append_bytes"] / MB, s["append_usec"] / 1e6), "%.0f", +1),
        ("zstd encode ms per MB in", lambda c, s: ratio(s["encode_usec"] / 1000.0, s["encode_in_bytes"] / MB), "%.2f", -1),
        ("zstd ratio (out/in)", lambda c, s: ratio(s["encode_out_bytes"], s["encode_in_bytes"]), "%.3f", -1),
        ("encode CPU s total", lambda c, s: s["encode_usec"] / 1e6, "%.2f", -1),
        ("decode ms per MB out", lambda c, s: ratio(s["decode_usec"] / 1000.0, s["decode_out_bytes"] / MB), "%.2f", -1),
        ("decode CPU s total", lambda c, s: s["decode_usec"] / 1e6, "%.2f", -1),
        ("index load ms (mean)", lambda c, s: s["index_load_usec"] / 1000.0 if s["index_load_usec"] else None,
         "%.1f", -1),
        ("disk MB (latest)", lambda c, s: s["disk_bytes"] / MB, "%.1f", 0),
        ("live fraction (latest)", lambda c, s: ratio(s["live_bytes"], s["disk_bytes"]), "%.3f", +1),
        ("files: segments + index", lambda c, s: s["files"], "%d", -1),
        ("evicted MB", lambda c, s: s["evicted_bytes"] / MB, "%.1f", 0),
        ("corrupt dropped", lambda c, s: s["corrupt_dropped"], "%d", -1),
        ("writes refused", lambda c, s: s["writes_refused"], "%d", -1),
    ]


def fmt(v, f):
    if v is None:
        return "-"
    if isinstance(v, str):
        return v
    if isinstance(v, float) and (math.isnan(v) or math.isinf(v)):
        return "-"
    try:
        return f % v
    except (TypeError, ValueError):
        return str(v)


def delta(base, v, direction):
    if base is None or v is None or isinstance(base, str) or isinstance(v, str):
        return ""
    if base == v:
        return "="
    if base == 0:
        txt = "new"
    else:
        txt = "%+.1f%%" % (100.0 * (v - base) / abs(base))
    if direction:
        better = (v > base) == (direction > 0)
        txt += " better" if better else " worse"
    return txt


def table(title, cols, rows_of, metric_list):
    present = [c for c in cols if rows_of(c) is not None]
    if not present:
        return
    print("\n== %s ==" % title)
    w0 = max(len(m[0]) for m in metric_list) + 2
    widths = [max(14, len(c.label) + 2) for c in present]
    head = "%-*s" % (w0, "metric") + "".join("%*s" % (w, c.label) for w, c in zip(widths, present))
    if len(present) > 1:
        head += "   delta vs %s" % present[0].label
    print(head)
    for label, fn, f, direction in metric_list:
        vals = []
        for c in present:
            try:
                vals.append(fn(c, rows_of(c)))
            except (KeyError, TypeError, ZeroDivisionError):
                vals.append(None)
        if all(v is None for v in vals):
            continue
        line = "%-*s" % (w0, label) + "".join("%*s" % (w, fmt(v, f)) for w, v in zip(widths, vals))
        if len(present) > 1:
            line += "   " + "  ".join(d for d in (delta(vals[0], v, direction) for v in vals[1:]))
        print(line.rstrip())


def describe(cols):
    for c in cols:
        print("%s: %d session(s), %.1f h, backend %s" % (c.label, len(c.sessions), c.duration_s / 3600.0, c.backend))
        for s in c.sessions:
            st = s.get("settings", {}) if isinstance(s.get("settings"), dict) else {}
            sel = " ".join("%s=%s" % (k, st[k]) for k in ("CacheStoreGpuTex", "CacheStoreJ2C", "CacheStoreAssets",
                                                          "CacheStoreCluster") if k in st and st[k] is not None)
            print("    %s  %s  drive=%s  %s" % (os.path.basename(s["_path"]),
                                               s.get("build", {}).get("channel_version", "?"), s.get("drive", "?"), sel))


def main(argv):
    ap = argparse.ArgumentParser(description="Compare Slipstream cache metrics session files (#715).")
    ap.add_argument("files", nargs="*", help="session files, one column per file and backend")
    ap.add_argument("--legacy", nargs="+", default=[], metavar="JSON", help="sessions of the legacy group (legacy rows only)")
    ap.add_argument("--store", nargs="+", default=[], metavar="JSON", help="sessions of the store group (store rows only)")
    ap.add_argument("--group", nargs="+", action="append", default=[], metavar=("NAME", "JSON"),
                    help="a named group of sessions (every backend, one column each)")
    ap.add_argument("--cache", action="append", choices=CACHES, help="only these caches (repeatable)")
    a = ap.parse_args(argv)

    cols = []
    if a.legacy:
        ss = [s for s in map(load, a.legacy) if s]
        if ss:
            cols.append(Column("legacy", ss, "legacy"))
    if a.store:
        ss = [s for s in map(load, a.store) if s]
        if ss:
            cols.append(Column("store", ss, "store"))
    for g in a.group:
        name, paths = g[0], g[1:]
        ss = [s for s in map(load, paths) if s]
        for backend in ("legacy", "store"):
            if any(r.get("backend") == backend for s in ss for r in s.get("rows", []) if isinstance(r, dict)):
                cols.append(Column("%s/%s" % (name, backend), ss, backend))
    for p in a.files:
        s = load(p)
        if not s:
            continue
        base = os.path.splitext(os.path.basename(p))[0].replace("cache_metrics_", "")
        for backend in ("legacy", "store"):
            if any(r.get("backend") == backend for r in s.get("rows", []) if isinstance(r, dict)):
                cols.append(Column("%s/%s" % (base, backend), [s], backend))
    if not cols:
        ap.error("no usable session files")

    describe(cols)
    ml = metrics()
    for cache in (a.cache or CACHES):
        table(cache, cols, lambda c, cache=cache: c.rows.get(cache), ml)
    table("store totals (all kinds; SSD wear, codec and append cost)", cols, lambda c: c.store, store_metrics())
    sm = stage_metrics()
    for stage in STAGES:
        table("stage %s (session-wide)" % stage, cols, lambda c, stage=stage: c.stages.get(stage), sm)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
