#!/usr/bin/env python3
"""Build the test plan for a Jetstream build or a beta from commit trailers, and post it to Discord.

Every commit that changes something a tester can check carries trailers in its message body:

    Test-Plan: Stand in a sealed room with no lights; it should go dark. Open a window: the
      room lights up with soft bounce light.
    Test-Settings: RenderVKGI = 3, RenderVKGICache = 1

(continuation lines start with whitespace, as git trailers do; several Test-Plan trailers per commit
are fine). This script collects them from the commits since the previous build of the same kind
(jetstream-* -> the previous Jetstream tag; vX.Y.Z-beta.N -> the previous v* tag), groups them by
commit, and posts the plan to #test-plans. Commits without trailers are listed under "Also changed"
(subject only, at most 15) so testers know what else moved.

usage: test_plan.py TAG [--dry-run]
env:   DISCORD_TESTPLANS_WEBHOOK (unset or --dry-run: print the messages instead of posting)
       PUBLIC_REPO (default https://git.slipstreamviewer.org/sky_line/Slipstream)
"""
import json
import os
import re
import subprocess
import sys
import time
import urllib.error
import urllib.request

LIMIT = 1900          # Discord's message limit is 2000 characters
PUBLIC = os.environ.get("PUBLIC_REPO", "https://git.slipstreamviewer.org/sky_line/Slipstream")
TRAILER = re.compile(r"^(Test-Plan|Test-Settings):[ \t]*(.*)$", re.I)
SKIP_SUBJECT = re.compile(r"^(Regenerate (world )?SPIR-V|Merge )")


def git(*args):
    return subprocess.run(["git", *args], capture_output=True, text=True, check=True).stdout


def previous_tag(tag):
    pattern = "jetstream-*" if tag.startswith("jetstream-") else "v[0-9]*"
    try:
        return git("describe", "--tags", "--abbrev=0", "--match", pattern, f"{tag}^").strip()
    except subprocess.CalledProcessError:
        return ""


def trailers(body):
    """[(kind, text)] from a commit body; continuation lines (leading whitespace) join the previous trailer."""
    out = []
    for line in body.splitlines():
        m = TRAILER.match(line)
        if m:
            out.append([m.group(1).lower(), m.group(2).strip()])
        elif out and line[:1] in (" ", "\t") and line.strip():
            out[-1][1] += " " + line.strip()
        elif out and not line.strip():
            continue
        elif out:
            # a non-trailer line after trailers ends the block
            pass
    return [(k, t) for k, t in out if t]


def collect(tag, prev):
    rng = f"{prev}..{tag}" if prev else tag
    log = git("log", "--no-merges", "--reverse", "--format=%H%x1f%s%x1f%b%x1e", rng)
    planned, other = [], []
    for rec in log.split("\x1e"):
        rec = rec.strip()
        if not rec:
            continue
        h, subj, body = (rec.split("\x1f") + ["", ""])[:3]
        if SKIP_SUBJECT.match(subj):
            continue
        if len(subj) > 160:
            cut = re.search(r"^(.{40,160}?[.;:])\s", subj)
            subj = cut.group(1) if cut else subj[:160].rsplit(" ", 1)[0] + "..."
        tr = trailers(body)
        if any(k == "test-plan" for k, _ in tr):
            planned.append((h[:10], subj, tr))
        else:
            other.append((h[:10], subj))
    return planned, other


def title_for(tag):
    m = re.match(r"^jetstream-(\d+\.\d+\.\d+)\.(\d+)$", tag)
    if m:
        return f"Jetstream {m.group(1)}.{m.group(2)}"
    m = re.match(r"^v(\d+\.\d+\.\d+)-beta\.(\d+)$", tag)
    if m:
        return f"{m.group(1)} Beta {m.group(2)}"
    return tag


def messages(tag, prev, planned, other):
    head = (f"## Test plan: {title_for(tag)}\n"
            f"Changes since {title_for(prev) if prev else 'the start'}. Downloads and full notes: "
            f"<{PUBLIC}/releases/tag/{tag}>. Settings go in **Advanced -> Debug Settings**.")
    blocks = []
    for i, (h, subj, tr) in enumerate(planned, 1):
        lines = [f"**{i}. {subj}** (`{h}`)"]
        settings = [t for k, t in tr if k == "test-settings"]
        if settings:
            lines.append("```\n" + "\n".join(s.strip() for s in ", ".join(settings).split(",") if s.strip()) + "\n```")
        lines += [f"- {t}" for k, t in tr if k == "test-plan"]
        blocks.append("\n".join(lines))
    if not planned:
        blocks.append("No commit in this build asked for specific testing; just use it as usual and report anything odd.")
    if other:
        shown = other[:15]
        lines = ["**Also changed** (no specific test steps):"] + [f"- {s} (`{h}`)" for h, s in shown]
        if len(other) > len(shown):
            lines.append(f"- ...and {len(other) - len(shown)} more (see the release notes)")
        blocks.append("\n".join(lines))
    blocks.append("**How to report:** in the viewer, **Help -> Report Bug** (it attaches your system info and log). "
                  "Say which build and setting, and what you expected. Screenshots help.")
    msgs, cur = [], head
    for b in blocks:
        while len(b) > LIMIT:                       # a single oversized block: hard split on lines
            part = b[:LIMIT].rsplit("\n", 1)[0] or b[:LIMIT]
            if len(cur) + len(part) + 2 > LIMIT:
                msgs.append(cur)
                cur = ""
            cur = (cur + "\n\n" + part).strip("\n")
            b = b[len(part):].lstrip("\n")
        if len(cur) + len(b) + 2 > LIMIT:
            msgs.append(cur)
            cur = b
        else:
            cur = (cur + "\n\n" + b).strip("\n")
    if cur:
        msgs.append(cur)
    return msgs


def post(hook, msgs):
    for m in msgs:
        body = json.dumps({"content": m, "allowed_mentions": {"parse": []}}).encode()
        for _ in range(5):
            req = urllib.request.Request(hook + "?wait=true", data=body, method="POST",
                                         headers={"Content-Type": "application/json",
                                                  "User-Agent": "DiscordBot (https://slipstreamviewer.org, 1)"})
            try:
                with urllib.request.urlopen(req, timeout=30):
                    break
            except urllib.error.HTTPError as e:
                if e.code == 429:
                    time.sleep(float(json.loads(e.read() or b"{}").get("retry_after", 2)) + 0.25)
                    continue
                # never print the webhook URL (it carries a token)
                print(f"::warning::test plan post failed: HTTP {e.code} {e.read()[:300]!r}")
                return
        time.sleep(1)
    print(f"posted the test plan ({len(msgs)} message(s))")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(args) != 1:
        sys.exit(__doc__)
    tag = args[0]
    prev = previous_tag(tag)
    planned, other = collect(tag, prev)
    msgs = messages(tag, prev, planned, other)
    hook = os.environ.get("DISCORD_TESTPLANS_WEBHOOK", "")
    if "--dry-run" in sys.argv or not hook:
        if not hook and "--dry-run" not in sys.argv:
            print("::warning::no DISCORD_TESTPLANS_WEBHOOK: printing the test plan instead of posting")
        print("\n\n----- message -----\n\n".join(msgs))
        return
    post(hook, msgs)


if __name__ == "__main__":
    main()
