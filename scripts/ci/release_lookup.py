#!/usr/bin/env python3
"""Find a Forgejo release by tag, drafts included (the release gate keeps builds in draft).

Forgejo's GET /releases/tags/{tag} never returns a draft, and releases are created as drafts
until the release-gate job publishes them (scripts/ci/release_gate.py). Every CI step that
looks a release up by tag therefore goes through here: published releases are found through
the tags endpoint (works without a token, so old public releases stay readable), drafts by
listing /releases?draft=true with the token and matching tag_name.

usage (env: RELEASE_API = .../api/v1/repos/<owner>/<repo>, TOKEN optional but needed for drafts;
       `--api URL` overrides RELEASE_API):
  release_lookup.py json  TAG              print the release JSON (exit 1 if there is none)
  release_lookup.py id    TAG              print the release id (exit 1 if none)
  release_lookup.py body  TAG              print the release notes text
  release_lookup.py sha   TAG ASSET        the sha256 publish_release.sh put in the notes for ASSET
  release_lookup.py fetch TAG ASSET OUT    download an asset over the API host (works for drafts)
Importable: find_release(api, token, tag), asset_url(api, asset).
"""
import json
import os
import re
import sys
import urllib.error
import urllib.request


def _req(url, token, method="GET", data=None, headers=None):
    h = dict(headers or {})
    if token:
        h["Authorization"] = "token " + token
    return urllib.request.Request(url, method=method, data=data, headers=h)


def api_json(api, token, path, method="GET", body=None):
    data = json.dumps(body).encode() if body is not None else None
    hdr = {"Content-Type": "application/json"} if body is not None else {}
    with urllib.request.urlopen(_req(api + path, token, method, data, hdr), timeout=60) as r:
        raw = r.read()
    return json.loads(raw) if raw else None


def find_release(api, token, tag):
    """The release for `tag` (draft or published), or None."""
    try:
        return api_json(api, token, "/releases/tags/" + urllib.request.quote(tag, safe=""))
    except urllib.error.HTTPError as e:
        if e.code != 404:
            raise
    if not token:
        return None
    page = 1
    while True:
        batch = api_json(api, token, f"/releases?draft=true&limit=50&page={page}") or []
        for r in batch:
            if r.get("tag_name") == tag:
                return r
        if not batch:
            return None
        page += 1


def asset_url(api, asset):
    """Download URL of a release asset through the API host (the public browser_download_url
    sits behind Cloudflare and 404s for drafts)."""
    server = api.split("/api/v1/repos/", 1)[0]
    return f"{server}/attachments/{asset['uuid']}"


def fetch_asset(api, token, asset, out):
    with urllib.request.urlopen(_req(asset_url(api, asset), token), timeout=600) as r, open(out, "wb") as f:
        while True:
            chunk = r.read(1 << 20)
            if not chunk:
                break
            f.write(chunk)


def notes_sha(rel, name):
    sha = ""
    for line in (rel.get("body") or "").splitlines():
        m = re.match(r"^([0-9a-f]{64})  (\S+)$", line.strip())
        if m and m.group(2) == name:
            sha = m.group(1)           # the last line for a name wins
    return sha


def main(argv):
    args = argv[1:]
    api = os.environ.get("RELEASE_API", "")
    if args[:1] == ["--api"]:
        api, args = args[1], args[2:]
    if len(args) < 2 or not api:
        print(__doc__, file=sys.stderr)
        return 2
    cmd, tag, rest = args[0], args[1], args[2:]
    token = os.environ.get("TOKEN", "")
    rel = find_release(api, token, tag)
    if rel is None:
        print(f"release_lookup: no release (draft or published) for tag {tag}", file=sys.stderr)
        return 1
    if cmd == "json":
        json.dump(rel, sys.stdout)
        print()
    elif cmd == "id":
        print(rel["id"])
    elif cmd == "body":
        print(rel.get("body") or "")
    elif cmd == "sha":
        print(notes_sha(rel, rest[0]))
    elif cmd == "fetch":
        asset = next((a for a in rel.get("assets") or [] if a["name"] == rest[0]), None)
        if asset is None:
            print(f"release_lookup: {tag} has no asset {rest[0]}", file=sys.stderr)
            return 1
        fetch_asset(api, token, asset, rest[1])
    else:
        print(__doc__, file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv))
    except urllib.error.HTTPError as e:
        print(f"release_lookup: HTTP {e.code} {e.url}", file=sys.stderr)
        sys.exit(1)
