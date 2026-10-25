#!/usr/bin/env python3
"""After a Jetstream build is published: announce it on Discord and prune old Jetstream releases.

Jetstream = the build CI makes every 6 hours from main when it has new commits
(.forgejo/workflows/jetstream.yml tags it, release.yml builds and publishes it). Its notes are
the commit list release_notes.sh drafted into the release text; nothing is written by hand.

env: TOKEN (Forgejo token), RELEASE_API (.../api/v1/repos/<owner>/<repo>), TAG (jetstream-X.Y.Z.N)
     DISCORD_JETSTREAM_WEBHOOK (optional; unset = no post), DISCORD_JETSTREAM_ROLE (optional role id
     pinged with each post: the opt-in "Jetstream Pings" role), KEEP (Jetstream releases kept, default 10)
     PUBLIC_REPO (default https://git.slipstreamviewer.org/sky_line/Slipstream)
"""
import json
import os
import re
import sys
import time
import urllib.error
import urllib.request

TOKEN = os.environ["TOKEN"]
API = os.environ["RELEASE_API"]
TAG = os.environ["TAG"]
HOOK = os.environ.get("DISCORD_JETSTREAM_WEBHOOK", "")
ROLE = os.environ.get("DISCORD_JETSTREAM_ROLE", "").strip()
KEEP = int(os.environ.get("KEEP", "10"))
PUBLIC = os.environ.get("PUBLIC_REPO", "https://git.slipstreamviewer.org/sky_line/Slipstream")
JTAG = re.compile(r"^jetstream-(\d+)\.(\d+)\.(\d+)\.(\d+)$")


def forgejo(method, path, body=None):
    req = urllib.request.Request(API + path, method=method,
                                 data=json.dumps(body).encode() if body is not None else None,
                                 headers={"Authorization": "token " + TOKEN, "Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=60) as r:
        raw = r.read()
        return json.loads(raw) if raw else None


def notes_from_body(body):
    """The drafted notes inside the release text (publish_release.sh layout): after the
    download-instructions paragraph, before the checksum lines."""
    text = body.split("SHA-256 of each file", 1)[0]
    parts = text.split("\n\n", 2)
    notes = parts[2] if len(parts) == 3 else text
    out = []
    for line in notes.strip().splitlines():
        line = line.rstrip()
        if re.match(r"^[0-9a-f]{64}\s", line):
            continue
        m = re.match(r"^##\s+(.*)", line)
        if m:
            out.append(f"**{m.group(1)}**")
        elif line.startswith("- "):
            out.append("• " + re.sub(r" \(([0-9a-f]{10})\)$", r" (`\1`)", line[2:]))
        else:
            out.append(line)
    return "\n".join(out).strip()


def post_discord(rel):
    title = rel.get("name") or TAG
    page = f"{PUBLIC}/releases/tag/{TAG}"
    assets = {a["name"]: a["browser_download_url"] for a in rel.get("assets") or []}
    links = []
    for label, suffix in (("Windows installer", "_Setup.exe"), ("Windows zip", "_win64.zip"),
                          ("Linux tarball", "_x86_64.tar.xz")):
        url = next((u for n, u in assets.items() if n.endswith(suffix)), None)
        if url:
            links.append(f"[{label}]({url})")
    notes = notes_from_body(rel.get("body") or "") or "No code changes listed."
    tail = "\n\n" + " · ".join(links + [f"[All files and full notes]({page})"])
    limit = 4000 - len(tail)
    if len(notes) > limit:
        notes = notes[:limit].rsplit("\n", 1)[0] + "\n…"
    msg = {
        "content": (f"<@&{ROLE}> " if ROLE.isdigit() else "") + f"**{title}** is up.",
        "allowed_mentions": {"roles": [ROLE]} if ROLE.isdigit() else {"parse": []},
        "embeds": [{
            "title": title, "url": page, "description": notes + tail, "color": 0xF5B041,
            "footer": {"text": "Jetstream: built automatically from the development branch every 6 hours "
                               "when it changes. Expect breakage; report bugs with the build number."},
        }],
    }
    req = urllib.request.Request(HOOK + "?wait=true", data=json.dumps(msg).encode(), method="POST",
                                 headers={"Content-Type": "application/json",
                                          "User-Agent": "DiscordBot (https://slipstreamviewer.org, 1)"})
    for _ in range(5):
        try:
            with urllib.request.urlopen(req, timeout=30):
                print(f"announced {TAG} on Discord")
                return
        except urllib.error.HTTPError as e:
            if e.code == 429:
                time.sleep(float(json.loads(e.read() or b"{}").get("retry_after", 2)) + 0.25)
                continue
            # Never print the webhook URL (it carries a token).
            print(f"::warning::Discord post failed: HTTP {e.code} {e.read()[:300]!r}")
            return
    print("::warning::Discord post still rate-limited; skipped")


def prune():
    rels, page = [], 1
    while True:
        batch = forgejo("GET", f"/releases?limit=50&page={page}") or []
        rels += batch
        if len(batch) < 50:
            break
        page += 1
    jet = []
    for r in rels:
        m = JTAG.match(r.get("tag_name") or "")
        if m:
            jet.append((tuple(int(x) for x in m.groups()), r))
    jet.sort(key=lambda t: t[0], reverse=True)
    for _, r in jet[KEEP:]:
        tag = r["tag_name"]
        try:
            forgejo("DELETE", f"/releases/{r['id']}")
            forgejo("DELETE", f"/tags/{tag}")
            print(f"pruned {tag}")
        except urllib.error.HTTPError as e:
            print(f"::warning::could not prune {tag}: HTTP {e.code}")


def main():
    rel = forgejo("GET", f"/releases/tags/{TAG}")
    if HOOK:
        post_discord(rel)
    else:
        print("::warning::no DISCORD_JETSTREAM_WEBHOOK: not announcing")
    prune()


if __name__ == "__main__":
    sys.exit(main())
