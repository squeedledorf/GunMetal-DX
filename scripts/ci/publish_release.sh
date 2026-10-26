#!/bin/bash
# Attach a build artifact to the Forgejo release for TAG, creating the release if
# this is the first job to finish (the Linux and Windows jobs both publish to the
# same release). Uploads over the LAN API (Cloudflare caps bodies at 100 MB)
# and verifies the round-trip checksum.
#
# Release gate: v* and jetstream-* releases are created as DRAFTS and stay unpublished
# until the release-gate job (release_gate.py) has checked that every job succeeded and
# every asset is attached. Draft releases are invisible to GET /releases/tags/{tag}, so
# the lookup goes through release_lookup.py. vk-* test builds and workflow_dispatch
# builds are published immediately (DRAFT=false). Uploading a name that already exists
# on the release replaces it (re-runs of a job, a moved tag).
#
# env: TOKEN RELEASE_API TAG FILE SHA COMMIT
#      NOTES_FILE (drafted notes, Markdown) or NOTES (one line), CHANNEL, FULL_VERSION,
#      PRERELEASE (true/false; default true)
#      DRAFT (true/false; default: true for v* and jetstream-* tags, false otherwise)
#      APPEND_NOTES=false  don't append the "<sha256>  <name>" line to the notes
#                   (sign_release.sh: SHA256SUMS lists everything already)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
: "${TOKEN:?RELEASE_TOKEN secret is empty/missing (repo Settings > Actions > Secrets)}"

# api METHOD URL [curl args...]: prints the body; fails loudly with status + body.
api() {
  local m="$1" u="$2"; shift 2
  local out code
  out=$(mktemp)
  code=$(curl -sS -o "$out" -w '%{http_code}' -X "$m" -H "Authorization: token $TOKEN" "$@" "$u")
  if [ "${code:0:1}" != "2" ]; then echo "::error::$m $u -> HTTP $code" >&2; cat "$out" >&2; rm -f "$out"; return 1; fi
  cat "$out"; rm -f "$out"
}

# Release id for TAG, draft or published; empty when there is none.
get_id() { python3 -I "$HERE/release_lookup.py" id "$TAG" 2>/dev/null || true; }

if [ -z "${DRAFT:-}" ]; then
  case "$TAG" in v[0-9]*|jetstream-*) DRAFT=true ;; *) DRAFT=false ;; esac
fi

ID=$(get_id)
if [ -z "$ID" ]; then
  NOTES_TEXT="${NOTES:-}"
  if [ -n "${NOTES_FILE:-}" ] && [ -s "$NOTES_FILE" ]; then NOTES_TEXT=$(cat "$NOTES_FILE"); fi
  BODY=$(python3 - "$TAG" "$NOTES_TEXT" "$COMMIT" "${CHANNEL:-}" "${FULL_VERSION:-}" "${PRERELEASE:-true}" "$DRAFT" <<'PY'
import json, sys
tag, notes, commit, channel, full_version, prerelease, draft = sys.argv[1:8]
title = f"{channel} {full_version}".strip() if channel and full_version else f"VK test build {tag}"
body = (f"{title} -- {tag} ({commit[:10]}).\n\n"
        "Linux x86_64 (glibc >= 2.38, x86-64-v2 CPU): *_x86_64.tar.xz\n"
        "Ubuntu 24.04+ / Debian 13+ / Mint 22+: *_amd64.deb (sudo apt install ./<file>)\n"
        "Windows x64 (unsigned; SmartScreen: More info > Run anyway): *_Setup.exe installer, or *_win64.zip to unpack anywhere\n\n"
        f"{notes}\n\n"
        "SHA-256 of each file is listed in its job log and below as uploads land.")
print(json.dumps({"tag_name": tag, "target_commitish": commit, "name": title,
                  "body": body, "prerelease": prerelease != "false", "draft": draft == "true"}))
PY
)
  # Another job may have created it between our GET and POST: fall back to a re-read.
  ID=$(api POST "$RELEASE_API/releases" -H "Content-Type: application/json" -d "$BODY" \
         | python3 -c 'import json,sys;print(json.load(sys.stdin)["id"])') || ID=""
  if [ -z "$ID" ]; then
    for _ in 1 2 3 4 5; do ID=$(get_id); [ -n "$ID" ] && break; sleep 2; done
  fi
fi
[ -n "$ID" ] || { echo "::error::could not create or find release $TAG"; exit 1; }

NAME=$(basename "$FILE")
# Replace an asset of the same name (re-run of a job, or a moved tag rebuilding a draft).
OLD=$(api GET "$RELEASE_API/releases/$ID/assets" | python3 -c '
import json, sys
for a in json.load(sys.stdin):
    if a["name"] == sys.argv[1]:
        print(a["id"])' "$NAME")
for aid in $OLD; do
  api DELETE "$RELEASE_API/releases/$ID/assets/$aid" >/dev/null
  echo "replaced existing asset $NAME ($aid)"
done
UUID=$(api POST "$RELEASE_API/releases/$ID/assets?name=$NAME" -F "attachment=@$FILE" \
        | python3 -c 'import json,sys;print(json.load(sys.stdin)["uuid"])')
# Authenticated: a draft's attachments are not served to anonymous requests.
GOT=$(curl -sf -H "Authorization: token $TOKEN" "${RELEASE_API%/api/v1/repos/*}/attachments/$UUID" | sha256sum | cut -d' ' -f1)
[ "$GOT" = "$SHA" ] || { echo "::error::uploaded asset checksum mismatch: $GOT"; exit 1; }

if [ "${APPEND_NOTES:-true}" = false ]; then echo "Published $NAME to $TAG ($SHA)"; exit 0; fi

# Append the checksum line to the release notes.
CUR=$(api GET "$RELEASE_API/releases/$ID" | python3 -c 'import json,sys;print(json.load(sys.stdin)["body"])')
NEW=$(python3 -c 'import json,sys;print(json.dumps({"body": sys.argv[1] + "\n\n" + sys.argv[2]}))' "$CUR" "$SHA  $NAME")
api PATCH "$RELEASE_API/releases/$ID" -H "Content-Type: application/json" -d "$NEW" >/dev/null
echo "Published $NAME to $TAG ($SHA)"
