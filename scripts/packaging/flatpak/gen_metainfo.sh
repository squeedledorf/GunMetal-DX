#!/bin/bash
# Write the AppStream metainfo for the Flatpak, with <releases> generated from
# the git tags (#286).
#
# usage: gen_metainfo.sh TAG OUTFILE [TAGS_REPO]
#   TAG      the release being built (vX.Y.Z or vX.Y.Z-beta.N). It is always
#            the newest entry, dated today when the tag isn't in TAGS_REPO.
#   OUTFILE  where to write org.slipstreamviewer.Slipstream.metainfo.xml
#   TAGS_REPO  repository whose v* tags are listed (default: this checkout)
#
# Every vX.Y.Z / vX.Y.Z-beta.N tag up to TAG becomes a <release>, newest first,
# capped at MAX_RELEASES (default 20). Version mapping as in the .deb:
# vX.Y.Z-beta.N -> X.Y.Z~betaN (type="development"), vX.Y.Z -> X.Y.Z, because
# AppStream (like dpkg) sorts '~' below everything, so a final release sorts
# above its betas. Each entry links to its Forgejo release page; the full notes
# are on https://slipstreamviewer.org/releases.html.
# Validates the result with appstreamcli when it is installed.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
TAG=${1:?usage: gen_metainfo.sh TAG OUTFILE [TAGS_REPO]}
OUT=${2:?usage: gen_metainfo.sh TAG OUTFILE [TAGS_REPO]}
TAGS_REPO=${3:-$HERE/../../..}
# git would read these instead of -C; never inherit them.
unset GIT_DIR GIT_WORK_TREE
MAX=${MAX_RELEASES:-20}
RELEASE_PAGE=https://git.slipstreamviewer.org/sky_line/Slipstream/releases/tag

die() { echo "gen_metainfo.sh: $*" >&2; exit 1; }
re='^v([0-9]+\.[0-9]+\.[0-9]+)(-beta\.([0-9]+))?$'
[[ "$TAG" =~ $re ]] || die "tag '$TAG' is not vX.Y.Z or vX.Y.Z-beta.N"

# "sortkey tag date": sortkey orders betas below their final (X.Y.Z.0.N vs X.Y.Z.1).
list=$( {
  git -C "$TAGS_REPO" for-each-ref --format='%(refname:short) %(creatordate:short)' 'refs/tags/v*' \
    || echo "gen_metainfo.sh: WARNING: no tags from $TAGS_REPO; <releases> lists only $TAG" >&2
  echo "$TAG $(date -u +%F)"
} | while read -r t d; do
  [[ "$t" =~ $re ]] || continue
  if [ -n "${BASH_REMATCH[2]}" ]; then k="${BASH_REMATCH[1]}.0.${BASH_REMATCH[3]}"; else k="${BASH_REMATCH[1]}.1.0"; fi
  echo "$k $t $d"
done | sort -k1,1V -k3,3 | awk '!seen[$2]++' )

# Keep tags up to TAG (by sort key), newest first.
tagkey=$(awk -v t="$TAG" '$2==t {print $1; exit}' <<<"$list")
releases=$(sort -k1,1Vr <<<"$list" | while read -r k t d; do
  [ "$(printf '%s\n%s\n' "$k" "$tagkey" | sort -V | tail -1)" = "$tagkey" ] && echo "$k $t $d"
done | awk -v n="$MAX" 'NR <= n')

xml=""
while read -r k t d; do
  [[ "$t" =~ $re ]]
  if [ -n "${BASH_REMATCH[2]}" ]; then
    v="${BASH_REMATCH[1]}~beta${BASH_REMATCH[3]}"; type=development
  else
    v="${BASH_REMATCH[1]}"; type=stable
  fi
  xml+="    <release version=\"$v\" date=\"$d\" type=\"$type\">"$'\n'
  xml+="      <url type=\"details\">$RELEASE_PAGE/$t</url>"$'\n'
  xml+="    </release>"$'\n'
done <<<"$releases"
grep -q "$RELEASE_PAGE/$TAG<" <<<"$xml" || die "internal error: $TAG missing from <releases>"

python3 - "$HERE/org.slipstreamviewer.Slipstream.metainfo.xml.in" "$OUT" "${xml%$'\n'}" <<'PY'
import sys
src, out, rel = sys.argv[1:4]
text = open(src, encoding="utf-8").read()
assert text.count("@RELEASES@") == 1
open(out, "w", encoding="utf-8").write(text.replace("@RELEASES@", rel))
PY

if command -v appstreamcli >/dev/null 2>&1; then
  # --no-net: don't fetch the URLs; --explain for readable messages.
  appstreamcli validate --no-net --explain "$OUT" || die "appstreamcli rejected $OUT"
else
  echo "gen_metainfo.sh: appstreamcli not installed, metainfo not validated" >&2
fi
echo "gen_metainfo.sh: wrote $OUT ($(grep -c '<release ' "$OUT") releases, newest $TAG)"
