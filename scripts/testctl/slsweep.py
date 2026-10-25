#!/usr/bin/env python3
"""Settings cost sweep over the test-control channel (see slctl.py).

Holds the camera still, applies one variant at a time, waits for the GPU timer windows
(RenderVKGpuTimers, one "GPU ms/frame" log line per ~5 s) and reports the GPU frame time
against the baseline, with a no-UI snapshot per variant for a visual comparison.
Every setting is restored to its original value afterwards.

  slsweep.py VARIANTS.json [OUTDIR]

VARIANTS.json: [["name", {"Setting": value, ...}], ...]  (the first entry should be
["baseline", {}]; a final baseline re-check is added automatically).
"""

import json
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from slctl import Channel  # noqa: E402

LOG = os.path.expanduser("~/.slipstreamviewer/logs/Slipstream.log")
GPU_RE = re.compile(r"GPU ms/frame over (\d+) frames: total=([\d.]+) compute=([\d.]+) pass1=([\d.]+) pass2=([\d.]+)")
SETTLE_SEC = 8.0
WINDOWS = 2


def gpu_windows_after(offset, count, timeout=40.0):
    """Wait for `count` GPU timer lines written after byte `offset` of the log."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        with open(LOG, "rb") as f:
            f.seek(offset)
            text = f.read().decode("utf-8", "replace")
        hits = GPU_RE.findall(text)
        if len(hits) >= count:
            return [tuple(float(x) for x in h) for h in hits[:count]]
        time.sleep(1.0)
    return []


def measure(ch, name, outdir):
    time.sleep(SETTLE_SEC)
    offset = os.path.getsize(LOG)
    fps = []
    t_end = time.time() + 5.0 * WINDOWS + 1.0
    while time.time() < t_end:
        fps.append(ch.call({"cmd": "status"}).get("fps", 0.0))
        time.sleep(0.5)
    wins = gpu_windows_after(offset, WINDOWS)
    snap = ch.call({"cmd": "snapshot", "path": os.path.join(outdir, name + ".bmp"),
                    "width": 1920, "height": 1080, "showui": False, "showhud": False})
    if not wins:
        return {"name": name, "error": "no GPU timer lines"}
    frames = sum(w[0] for w in wins)
    avg = lambda i: sum(w[i] * w[0] for w in wins) / frames
    return {"name": name, "gpu_total": round(avg(1), 2), "compute": round(avg(2), 2),
            "pass1": round(avg(3), 2), "pass2": round(avg(4), 2),
            "fps_status": round(sorted(fps)[len(fps) // 2], 1), "snapshot": snap.get("ok", False)}


def main():
    variants = json.load(open(sys.argv[1]))
    outdir = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else os.path.expanduser("~/slipstream/sweep"))
    os.makedirs(outdir, exist_ok=True)
    ch = Channel()
    if not ch.call({"cmd": "status"}).get("logged_in"):
        raise SystemExit("not logged in")

    keys = sorted({k for _, s in variants for k in s} | {"RenderVKGpuTimers"})
    original = {k: ch.call({"cmd": "get", "name": k}).get("value") for k in keys}
    # Written first: if the viewer dies mid-sweep (it saves settings on the way out), a later
    # `slctl.py restore OUTDIR/original.json` puts every value back.
    json.dump(original, open(os.path.join(outdir, "original.json"), "w"), indent=1)
    ch.call({"cmd": "set", "name": "RenderVKGpuTimers", "value": True})
    results = []
    try:
        for name, settings in variants + [["baseline_end", {}]]:
            ch.call({"cmd": "log", "text": "SWEEP variant %s %s" % (name, json.dumps(settings))})
            for k, v in settings.items():
                ch.call({"cmd": "set", "name": k, "value": v})
            r = measure(ch, name, outdir)
            results.append(r)
            print(json.dumps(r), flush=True)
            for k in settings:
                ch.call({"cmd": "set", "name": k, "value": original[k]})
    finally:
        for k, v in original.items():
            ch.call({"cmd": "set", "name": k, "value": v})
        ch.call({"cmd": "log", "text": "SWEEP done, settings restored"})
    json.dump(results, open(os.path.join(outdir, "results.json"), "w"), indent=1)


if __name__ == "__main__":
    main()
