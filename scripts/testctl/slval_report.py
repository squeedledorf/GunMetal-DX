#!/usr/bin/env python3
"""Summarise a validation-layer run (slvalidate.sh / slval.py) from the viewer log.

  slval_report.py Slipstream.log [steps.json]

Attributes every validation message to the "SLVAL step <name>" marker before it (messages
before "SLVAL begin" count as startup / login), and reports device losses and crashes.
Exit status 1 when any validation ERROR, device loss or crash was seen.
"""

import collections
import json
import re
import sys

MSG_RE = re.compile(r"Validation (Error|Warning): \[ ([^\]]+) \]")
STEP_RE = re.compile(r"SLVAL (step (.*)|begin|end)")
FATAL_RE = re.compile(r"DEVICE LOST|VK_ERROR_DEVICE_LOST|fatal signal|Segmentation fault|Crash: unhandled")
# The media plugin (SLPlugin / CEF) segfaulting is reported separately: it does not take the
# viewer down, and on the headless-ish test box it also happens without validation layers.
PLUGIN_RE = re.compile(r"SLPlugin \(\d+\) killed by signal")


def main():
    log = sys.argv[1]
    step = "startup/login"
    per_step = collections.OrderedDict()
    totals = collections.Counter()
    fatal = []
    plugin_crashes = 0
    for line in open(log, encoding="utf-8", errors="replace"):
        m = STEP_RE.search(line)
        if m:
            step = m.group(2) or ("(" + m.group(1) + ")")
            continue
        m = MSG_RE.search(line)
        if m:
            key = "%s %s" % (m.group(1).upper(), m.group(2))
            per_step.setdefault(step, collections.Counter())[key] += 1
            totals[key] += 1
            continue
        if PLUGIN_RE.search(line):
            plugin_crashes += 1
            continue
        if FATAL_RE.search(line):
            fatal.append("%s: %s" % (step, line.strip()[:200]))

    errors = sum(n for k, n in totals.items() if k.startswith("ERROR"))
    print("== validation summary: %d message types, %d errors, %d warnings, %d fatal lines"
          % (len(totals), errors, sum(totals.values()) - errors, len(fatal)))
    for k, n in totals.most_common():
        print("  %5d  %s" % (n, k))
    if per_step:
        print("\n== by step (first step that produced each message type marked *)")
        first = {}
        for s, c in per_step.items():
            for k in c:
                first.setdefault(k, s)
        for s, c in per_step.items():
            print("  [%s]" % s)
            for k, n in c.most_common():
                print("    %s%5d  %s" % ("*" if first[k] == s else " ", n, k))
    if plugin_crashes:
        print("\n== media plugin (SLPlugin) crashes: %d (not fatal to the viewer)" % plugin_crashes)
    if fatal:
        print("\n== fatal")
        for f in fatal[:20]:
            print("  " + f)
    if len(sys.argv) > 2:
        steps = json.load(open(sys.argv[2]))
        dead = [s["step"] for s in steps if s.get("alive") is False or s.get("ok") is False]
        print("\n== steps: %d run%s" % (len(steps), (", failed: " + ", ".join(dead)) if dead else ", all ok"))
    sys.exit(1 if errors or fatal else 0)


if __name__ == "__main__":
    main()
