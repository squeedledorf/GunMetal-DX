#!/bin/bash
# Write and attach SHA256SUMS + SHA256SUMS.asc for a Forgejo release (#288).
#
# usage: signing_key.sh run sign_release.sh TAG
#
# Downloads every asset of the release over the LAN API, checks each against
# the "<sha256>  <name>" line publish_release.sh appended to the release notes
# when it verified the upload, writes SHA256SUMS (sha256sum format, sorted by
# name), signs it detached + armored with the release key and attaches both.
# Idempotent: existing SHA256SUMS / SHA256SUMS.asc assets are replaced, so
# re-running it after another job attached a late asset (or after a re-run of
# a failed job) refreshes the list.
#
# Users verify with:
#   gpg --verify SHA256SUMS.asc SHA256SUMS
#   sha256sum -c --ignore-missing SHA256SUMS
#
# env: TOKEN RELEASE_API (as publish_release.sh)
#      GNUPGHOME SIGNING_KEY_FPR SIGNING_PUBKEY [SIGNING_PUBKEY_REPO]  (signing_key.sh)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
TAG=${1:?usage: sign_release.sh TAG}
: "${TOKEN:?RELEASE_TOKEN secret is empty/missing}"
: "${RELEASE_API:?RELEASE_API not set}"
: "${SIGNING_KEY_FPR:?run under scripts/ci/signing_key.sh run}"
: "${GNUPGHOME:?run under scripts/ci/signing_key.sh run}"

die() { echo "::error::sign_release.sh: $*" >&2; exit 1; }
SERVER=${RELEASE_API%/api/v1/repos/*}
auth=(-H "Authorization: token $TOKEN")

W=$(mktemp -d "${TMPDIR:-/tmp}/sign-release.XXXXXX")
trap 'rm -rf "$W"' EXIT

curl -fsS "${auth[@]}" "$RELEASE_API/releases/tags/$TAG" > "$W/release.json" || die "no release $TAG"
ID=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["id"])' "$W/release.json")

# Remove the previous signature files first, so a failure below never leaves a
# SHA256SUMS.asc next to a SHA256SUMS that no longer matches.
python3 - "$W/release.json" <<'PY' > "$W/old"
import json, sys
for a in json.load(open(sys.argv[1])).get("assets", []):
    if a["name"] in ("SHA256SUMS.asc", "SHA256SUMS"):
        print(a["id"], a["name"])
PY
while read -r aid name; do
  [ -n "$aid" ] || continue
  curl -fsS -X DELETE "${auth[@]}" "$RELEASE_API/releases/$ID/assets/$aid" >/dev/null || die "could not delete old $name"
  echo "removed old $name"
done < "$W/old"

# name <TAB> uuid <TAB> notes-sha (empty when the notes have no line for it)
python3 - "$W/release.json" <<'PY' > "$W/assets"
import json, re, sys
rel = json.load(open(sys.argv[1]))
notes = {}
for line in (rel.get("body") or "").splitlines():
    m = re.match(r"^([0-9a-f]{64})  (\S+)$", line.strip())
    if m:
        notes[m.group(2)] = m.group(1)          # the last line for a name wins
for a in sorted(rel.get("assets", []), key=lambda a: a["name"]):
    if a["name"] in ("SHA256SUMS", "SHA256SUMS.asc"):
        continue
    print("\t".join((a["name"], a["uuid"], notes.get(a["name"], ""))))
PY
[ -s "$W/assets" ] || die "release $TAG has no assets"

mkdir "$W/files"
: > "$W/SHA256SUMS.unsorted"
while IFS=$'\t' read -r name uuid want; do
  case "$name" in */*|.*|'') die "refusing odd asset name '$name'" ;; esac
  curl -fsS -o "$W/files/$name" "$SERVER/attachments/$uuid" || die "could not download $name"
  got=$(sha256sum "$W/files/$name" | cut -d' ' -f1)
  if [ -z "$want" ]; then
    echo "::warning::$name has no checksum line in the release notes (uploaded outside publish_release.sh?); using the downloaded bytes"
  elif [ "$want" != "$got" ]; then
    die "$name: downloaded sha256 $got does not match the release notes ($want)"
  fi
  printf '%s  %s\n' "$got" "$name" >> "$W/SHA256SUMS.unsorted"
  rm -f "$W/files/$name"
done < "$W/assets"
mkdir "$W/out"
LC_ALL=C sort -k2 "$W/SHA256SUMS.unsorted" > "$W/out/SHA256SUMS"
cat "$W/out/SHA256SUMS"

gpg --yes -u "$SIGNING_KEY_FPR" --armor --detach-sign -o "$W/out/SHA256SUMS.asc" "$W/out/SHA256SUMS"
# Verify the way a user does: against the published public key only.
for key in "$SIGNING_PUBKEY" ${SIGNING_PUBKEY_REPO:+"$SIGNING_PUBKEY_REPO"}; do
  kr="$W/kr.gpg"
  gpg --batch --quiet --dearmor < "$key" > "$kr"
  gpgv --keyring "$kr" "$W/out/SHA256SUMS.asc" "$W/out/SHA256SUMS" 2>/dev/null \
    || die "SHA256SUMS.asc does not verify against $key"
  rm -f "$kr"
done

for f in SHA256SUMS SHA256SUMS.asc; do
  TAG="$TAG" FILE="$W/out/$f" SHA=$(sha256sum "$W/out/$f" | cut -d' ' -f1) COMMIT="${GITHUB_SHA:-}" \
    APPEND_NOTES=false bash "$HERE/publish_release.sh"
done

# One verification hint in the notes (not repeated on re-runs).
HINT="Signed checksums: SHA256SUMS + SHA256SUMS.asc (key ${SIGNING_KEY_FPR}). Verify: https://slipstreamviewer.org/wiki/verify/"
CUR=$(curl -fsS "${auth[@]}" "$RELEASE_API/releases/$ID" | python3 -c 'import json,sys;print(json.load(sys.stdin)["body"])')
if ! grep -qF "Signed checksums: SHA256SUMS" <<<"$CUR"; then
  NEW=$(python3 -c 'import json,sys;print(json.dumps({"body": sys.argv[1] + "\n\n" + sys.argv[2]}))' "$CUR" "$HINT")
  curl -fsS -X PATCH "${auth[@]}" -H "Content-Type: application/json" -d "$NEW" "$RELEASE_API/releases/$ID" >/dev/null
fi
echo "Signed $(wc -l < "$W/out/SHA256SUMS") assets of $TAG with $SIGNING_KEY_FPR"
