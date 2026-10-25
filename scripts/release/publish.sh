#!/usr/bin/env bash
# GunMetal-DX release, step 2 of 2: puts a release made by scripts/release/release.sh on GitHub.
#
#   scripts/release/publish.sh vX.Y.Z[-beta.N]          stage only: snapshot + scan + local commit
#   scripts/release/publish.sh vX.Y.Z[-beta.N] --push   then push it, make the release, push the feed
#
# The public repository gets snapshot commits only ("GunMetal-DX <version> (based on Slipstream
# <commit>)"): the private history has upstream blobs over GitHub's 100 MB limit. The snapshot is the
# tag's tracked files; update/ (the feed the viewers read) is the public repository's own.
#
# Order with --push: code, then the release with its files (a draft until every file is up), and the
# feed last, so no viewer is offered an update whose files are not there yet.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
TAG=${1:?usage: publish.sh vX.Y.Z[-beta.N] [--push]}
PUSH=0
[ "${2:-}" = --push ] && PUSH=1
REPO=squeedledorf/GunMetal-DX
PUB=${PUBLIC_DIR:-$HOME/Projects/gunmetal-dx-public}
OUT="$ROOT/out/release-$TAG"
die() { echo "publish: $*" >&2; exit 1; }

[ -f "$OUT/update/beta.json" ] || die "no release in $OUT: scripts/release/release.sh $TAG first"
git rev-parse -q --verify "refs/tags/$TAG" >/dev/null || die "no tag $TAG"
vars=$(bash scripts/ci/version.sh "$TAG") || die "version.sh refused $TAG"
eval "$(sed 's/^/V_/; s/=\(.*\)$/="\1"/' <<< "$vars")"
base=$(git merge-base "$TAG" upstream/main) || die "no merge base with upstream/main (git fetch upstream)"
msg="GunMetal-DX $V_LABEL (based on Slipstream ${base:0:10})"

# ---- scan: nothing private goes public ----
bad=0
scan() {   # <what> <pattern>: matches in the tag that the Slipstream base does not have
    local hits
    hits=$(comm -23 <(git grep -I -E -i "$2" "$TAG" -- . | sed "s/^$TAG://" | sort -u) \
                    <(git grep -I -E -i "$2" "$base" -- . | sed "s/^$base://" | sort -u))
    if [ -n "$hits" ]; then
        echo "publish: $1:" >&2
        echo "$hits" | cut -c1-200 >&2
        bad=1
    fi
}
# the patterns are private (~/.config/gunmetal-dx/publish-scan, <kind>|<label>|<regex>): they name
# what must not be published
SCAN="${XDG_CONFIG_HOME:-$HOME/.config}/gunmetal-dx/publish-scan"
[ -r "$SCAN" ] || die "no scan list at $SCAN"
while IFS='|' read -r kind label re; do
    case "$kind" in
        content) scan "$label" "$re" ;;
        path) git ls-tree -r --name-only "$TAG" | grep -i -E "$re" >&2 && { echo "publish: $label are tracked" >&2; bad=1; } ;;
    esac
done < <(grep -v -e '^#' -e '^$' "$SCAN")
big=$(git ls-tree -r -l "$TAG" | awk '$4 > 95000000 { print $4, $5 }')
[ -z "$big" ] || { echo "publish: files near GitHub's 100 MB limit:" >&2; echo "$big" >&2; bad=1; }
[ "$bad" = 0 ] || die "scan failed; nothing was staged"
echo "scan clean"

# ---- stage the snapshot ----
if [ ! -d "$PUB/.git" ]; then
    mkdir -p "$PUB"
    git -C "$PUB" init -q -b main
    git -C "$PUB" config user.name "$(git config user.name)"     # this repository's identity
    git -C "$PUB" config user.email "$(git config user.email)"
fi
staged() { git -C "$PUB" log --format='%H%x09%s' 2>/dev/null | awk -F'\t' -v m="$msg" '$2 == m { print $1; exit }'; }
if [ -n "$(staged)" ]; then
    echo "already staged: $msg"
else
    find "$PUB" -mindepth 1 -maxdepth 1 ! -name .git ! -name update -exec rm -rf {} +
    git archive "$TAG" | tar -x -C "$PUB"
    git -C "$PUB" add -A
    git -C "$PUB" commit -q -m "$msg" -m "Slipstream: https://git.slipstreamviewer.org/sky_line/Slipstream/commit/$base"
    echo "staged: $msg ($(git -C "$PUB" rev-parse --short HEAD))"
fi
git -C "$PUB" show --stat --format='%h %s' "$(staged)" | tail -1
[ "$PUSH" = 1 ] || { echo "look it over in $PUB, then: publish.sh $TAG --push"; exit 0; }

# ---- push: code, release, feed ----
gh repo view "$REPO" >/dev/null 2>&1 \
    || die "no $REPO on GitHub; create it: gh repo create $REPO --public --description 'A Second Life combat viewer, forked from Slipstream'"
git -C "$PUB" remote get-url origin >/dev/null 2>&1 || git -C "$PUB" remote add origin "https://github.com/$REPO.git"
git -C "$PUB" push -q origin main
sha=$(staged)

# 3p packages of our own (indra/cmake/3p_manifest.json) live on the "deps" release; upload any that are
# missing from the local 3p cache before a build somewhere else needs them
cache=${SL_3P_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/slipstream-3p}
for u in $(git show "$TAG:indra/cmake/3p_manifest.json" | grep -o "https://github.com/$REPO/releases/download/deps/[^\"]*"); do
    n=$(basename "$u")
    gh release view deps --repo "$REPO" >/dev/null 2>&1 \
        || gh release create deps --repo "$REPO" --target "$sha" --prerelease --title "Build dependencies" \
               --notes "Prebuilt packages GunMetal-DX builds with (indra/cmake/3p_manifest.json). Not a viewer release."
    gh release view deps --repo "$REPO" --json assets -q '.assets[].name' | grep -qxF "$n" \
        || gh release upload deps --repo "$REPO" "$cache/$n"
    curl -sfIL -o /dev/null "$u" || die "$u does not download"
done

files=("$OUT/${V_ARTIFACT_BASE}_Setup.exe" "$OUT/${V_ARTIFACT_BASE}_win64.zip"
       "$OUT/${V_ARTIFACT_BASE}_x86_64.tar.xz" "$OUT/SHA256SUMS.txt")
notes="$OUT/notes.md"
[ -f "$notes" ] || printf 'GunMetal-DX %s, based on Slipstream %s.\n' "$V_LABEL" "${base:0:10}" > "$notes"
pre=(); [ "$V_PRERELEASE" = true ] && pre=(--prerelease)
if ! gh release view "$TAG" --repo "$REPO" >/dev/null 2>&1; then
    gh release create "$TAG" --repo "$REPO" --target "$sha" --draft "${pre[@]}" \
        --title "GunMetal-DX $V_LABEL" --notes-file "$notes" "${files[@]}"
else
    gh release upload "$TAG" --repo "$REPO" --clobber "${files[@]}"
fi
# every file up with the right size, then out of draft
for f in "${files[@]}"; do
    n=$(basename "$f")
    size=$(gh release view "$TAG" --repo "$REPO" --json assets -q ".assets[] | select(.name == \"$n\") | .size")
    [ "$size" = "$(stat -c %s "$f")" ] || die "$n is not on the release (size '$size'); it stays a draft"
done
gh release edit "$TAG" --repo "$REPO" --draft=false
dl="https://github.com/$REPO/releases/download/$TAG/${V_ARTIFACT_BASE}_x86_64.tar.xz"
curl -sfIL -o /dev/null "$dl" || die "$dl does not download; the feed is not pushed"

mkdir -p "$PUB/update"
cp "$OUT/update/"*.json "$PUB/update/"
git -C "$PUB" add update
git -C "$PUB" commit -q -m "Update feed: $V_LABEL ($V_KIND)"
git -C "$PUB" push -q origin main
echo "published $TAG: https://github.com/$REPO/releases/tag/$TAG"
