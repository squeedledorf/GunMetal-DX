#!/usr/bin/env python3
"""Desktop A/B gate for the layered-stereo steps (#1075; research VR_LAYERED_STEREO_DESIGN.md §7).

Compares two Slipstream.log files -- A (the parent commit) and B (the step under test) --
recorded at the same fixed camera with TAA off and time / weather frozen, with
RenderVKDebugFrameHash 1 (and, for the timing half, RenderVKGpuTimers 1 + a RenderVKBench run):

  (a) frame hash:    the FRAMEHASH value that held longest in each log (the settled image)
                     must be identical; every distinct value is listed with its frame count.
  (b) timings:       every "bench result" value line: frame p50 / avg and gpu_total (+ the
                     named subs), B relative to A; flagged when p50 or gpu_total moves by more
                     than --tolerance percent (default 1).
  (c) stream:        the last FRAMESIG (sub-mark names in recording order) must be identical;
                     the last "VRAM breakdown" category totals must be identical (no new
                     allocation at N = 1), MB granularity.

Exit status 0 when (a) and (c) pass and (b) is within tolerance, 1 otherwise, 2 on bad input.
Usage: python3 -I scripts/perf/vk_ab_compare.py A.log B.log [--tolerance 1.0]
"""
import argparse
import re
import sys

HASH_RE = re.compile(r"FRAMEHASH frame=(\d+) size=(\d+)x(\d+) layer=(\d+) hash=([0-9a-f]+) \((changed|held), previous held (\d+) frames\)")
SIG_RE = re.compile(r"FRAMESIG frame=(\d+) sig=([0-9a-f]+) marks=(\d+)")
BENCH_LINE_RE = re.compile(r"^\s*(\S+)=(\S+): frames=(\d+) frame avg=([0-9.]+)ms \(([0-9.]+) fps\) p50=([0-9.]+) p99=([0-9.]+) acquireWait=([0-9.]+)(.*)$")
GPU_RE = re.compile(r"gpu_total=([0-9.]+)")
SUB_RE = re.compile(r" (\S+)=([0-9.]+)")
VRAM_RE = re.compile(r"VRAM breakdown: .*?\|(.*?)\|")
CAT_RE = re.compile(r"([A-Za-z_][A-Za-z0-9_-]*) (\d+) \((\d+)\)")


def parse(path):
    hashes = {}       # (layer, size) -> {hash: frames held}
    last_hash = {}    # (layer, size) -> (hash, start counting)
    sigs = []
    bench = {}        # value -> dict
    vram = None
    in_bench = False
    try:
        fh = open(path, "r", encoding="utf-8", errors="replace")
    except OSError as e:
        print("cannot read %s: %s" % (path, e))
        sys.exit(2)
    with fh:
        for line in fh:
            m = HASH_RE.search(line)
            if m:
                # "changed, previous held N": the run of the previous value ended after N frames;
                # "held, previous held N": the current value has held N frames so far.
                key = (int(m.group(4)), m.group(2) + "x" + m.group(3))
                h = m.group(5)
                n = int(m.group(7))
                table = hashes.setdefault(key, {})
                cur = last_hash.get(key)
                if m.group(6) == "changed":
                    if cur is not None:
                        table[cur[0]] = table.get(cur[0], 0) + n
                    last_hash[key] = (h, 1)
                else:
                    last_hash[key] = (h, n)
                continue
            m = SIG_RE.search(line)
            if m:
                sigs.append((m.group(2), int(m.group(3))))
                continue
            if "bench result:" in line:
                in_bench = True
                bench = {}
                continue
            if in_bench:
                m = BENCH_LINE_RE.match(line)
                if m:
                    rest = m.group(9)
                    g = GPU_RE.search(rest)
                    subs = {}
                    tail = rest.split(")", 1)[1] if "gpu_total" in rest and ")" in rest else rest
                    for s in SUB_RE.finditer(tail):
                        subs[s.group(1)] = float(s.group(2))
                    bench[m.group(2)] = {
                        "avg": float(m.group(4)), "p50": float(m.group(6)), "p99": float(m.group(7)),
                        "gpu": float(g.group(1)) if g else None, "subs": subs,
                    }
                    continue
                in_bench = False
            m = VRAM_RE.search(line)
            if m:
                vram = {c.group(1): int(c.group(2)) for c in CAT_RE.finditer(m.group(1))}
    # Close the open run of each key with what the last line said it had held.
    for key, (h, n) in last_hash.items():
        table = hashes.setdefault(key, {})
        table[h] = table.get(h, 0) + n
    return hashes, sigs, bench, vram


def settled(hashes):
    # The value that held the most frames: the settled image (the first frames after login /
    # a teleport differ while textures stream in).
    return {key: max(table.items(), key=lambda kv: kv[1]) for key, table in hashes.items() if table}


def pct(a, b):
    return 0.0 if not a else (b - a) / a * 100.0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--tolerance", type=float, default=1.0, help="timing tolerance in percent (default 1)")
    args = ap.parse_args()
    ha, sa, ba, va = parse(args.a)
    hb, sb, bb, vb = parse(args.b)
    ok = True

    print("(a) frame hash")
    if not ha or not hb:
        print("  no FRAMEHASH lines in %s" % (args.a if not ha else args.b))
        ok = False
    else:
        sta, stb = settled(ha), settled(hb)
        for key in sorted(set(sta) | set(stb)):
            x, y = sta.get(key, (None, 0)), stb.get(key, (None, 0))
            same = x[0] is not None and x[0] == y[0]
            ok &= same
            print("  layer %d %s: A %s (held %d)  B %s (held %d)  %s" %
                  (key[0], key[1], x[0], x[1], y[0], y[1], "IDENTICAL" if same else "DIFFERENT"))
            for name, h in (("A", ha.get(key, {})), ("B", hb.get(key, {}))):
                if len(h) > 1:
                    print("    %s saw %d values: %s" % (name, len(h), ", ".join("%s x%d" % kv for kv in sorted(h.items(), key=lambda kv: -kv[1]))))

    print("(b) timings (tolerance %.2f%%)" % args.tolerance)
    if not ba or not bb:
        print("  no 'bench result' block in %s (run RenderVKBench with RenderVKGpuTimers 1)" % (args.a if not ba else args.b))
    for v in sorted(set(ba) & set(bb)):
        x, y = ba[v], bb[v]
        dp = pct(x["p50"], y["p50"])
        line = "  value %s: p50 %.2f -> %.2f ms (%+.2f%%)" % (v, x["p50"], y["p50"], dp)
        bad = abs(dp) > args.tolerance
        if x["gpu"] is not None and y["gpu"] is not None:
            dg = pct(x["gpu"], y["gpu"])
            line += ", gpu_total %.3f -> %.3f ms (%+.2f%%)" % (x["gpu"], y["gpu"], dg)
            bad |= abs(dg) > args.tolerance
        print(line + ("  OUTSIDE TOLERANCE" if bad else ""))
        ok &= not bad
        for s in sorted(set(x["subs"]) & set(y["subs"])):
            print("    %s %.3f -> %.3f (%+.2f%%)" % (s, x["subs"][s], y["subs"][s], pct(x["subs"][s], y["subs"][s])))

    print("(c) recorded stream + allocations")
    if not sa or not sb:
        print("  no FRAMESIG lines in %s" % (args.a if not sa else args.b))
        ok = False
    else:
        same = sa[-1] == sb[-1]
        ok &= same
        print("  last stream signature: A %s (%d marks)  B %s (%d marks)  %s" %
              (sa[-1][0], sa[-1][1], sb[-1][0], sb[-1][1], "IDENTICAL" if same else "DIFFERENT"))
    if va is None or vb is None:
        print("  no 'VRAM breakdown' line in %s (the periodic VKMem log)" % (args.a if va is None else args.b))
    else:
        # Only the screen-target categories gate (the stereo steps change render targets); the
        # streamed ones (textures, geometry, ...) vary run to run and are listed for information.
        gate = ("targets", "rt-mask", "upscale", "denoise", "readback")
        for c in sorted(set(va) | set(vb)):
            x, y = va.get(c, 0), vb.get(c, 0)
            if x == y:
                continue
            bad = c in gate
            ok &= not bad
            print("  category %s: A %d MB  B %d MB  %s" % (c, x, y, "DIFFERENT" if bad else "(streamed: information only)"))
        print("  gated categories (%s): %s" % (", ".join(gate), "identical" if all(va.get(c, 0) == vb.get(c, 0) for c in gate) else "DIFFERENT"))

    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
