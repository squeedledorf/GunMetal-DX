#!/usr/bin/env bash
# GunMetal-DX release, step 1 of 2: builds Linux and Windows, uploads symbols to BugSplat, signs
# the update manifest and writes the feed, all into out/release-<tag>/. Nothing goes public:
# scripts/release/publish.sh does that.
#
#   scripts/release/release.sh vX.Y.Z[-beta.N] [--skip-build] [--notes FILE]
#
# The tag is created on HEAD (local, private history) if it does not exist yet; HEAD must be clean.
#
# Needs: ~/.config/gunmetal-dx/bugsplat (scripts/release/bugsplat-setup.sh),
#        ~/.config/gunmetal-dx/update-signing.pem (the update signing key),
#        docker (makensis), 7z, xwin (scripts/wincross).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
TAG=${1:?usage: release.sh vX.Y.Z[-beta.N] [--skip-build] [--notes FILE]}
shift
SKIP_BUILD=0 NOTES=""
while [ $# -gt 0 ]; do
    case "$1" in
        --skip-build) SKIP_BUILD=1 ;;
        --notes) NOTES=$(realpath "$2"); shift ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
    shift
done
REPO=squeedledorf/GunMetal-DX   # where the files will be downloaded from (publish.sh)
CONF="${XDG_CONFIG_HOME:-$HOME/.config}/gunmetal-dx"
KEY="$CONF/update-signing.pem"
JOBS=${JOBS:-10}

die() { echo "release: $*" >&2; exit 1; }
[[ "$TAG" =~ ^v[0-9]+\.[0-9]+\.[0-9]+(-beta\.[0-9]+)?$ ]] || die "tag must be vX.Y.Z or vX.Y.Z-beta.N"
[ -r "$CONF/bugsplat" ] || die "no BugSplat credentials: scripts/release/bugsplat-setup.sh"
[ -r "$KEY" ] || die "no update signing key at $KEY"
for t in docker 7z python3 openssl; do command -v "$t" >/dev/null || die "$t is missing"; done
git diff --quiet && git diff --cached --quiet || die "the working tree has changes; commit them first"

if git rev-parse -q --verify "refs/tags/$TAG" >/dev/null; then
    [ "$(git rev-list -n1 "$TAG")" = "$(git rev-parse HEAD)" ] || die "$TAG exists on another commit"
else
    git tag -a "$TAG" -m "GunMetal-DX $TAG"
    echo "tagged $TAG on $(git rev-parse --short HEAD)"
fi
vars=$(bash scripts/ci/version.sh "$TAG") || die "version.sh refused $TAG"
eval "$(sed 's/^/V_/; s/=\(.*\)$/="\1"/' <<< "$vars")"
echo "release $V_FULL_VERSION ($V_CHANNEL), files ${V_ARTIFACT_BASE}_*"
OUT="$ROOT/out/release-$TAG"
mkdir -p "$OUT"
[ -z "$NOTES" ] || cp "$NOTES" "$OUT/notes.md"   # the release text (publish.sh)

# ---- build ----
if [ "$SKIP_BUILD" = 0 ]; then
    [ -d build-crashpad-linux/lib ] || NINJA_JOBS=$JOBS bash scripts/crashpad/build.sh linux
    [ -d build-crashpad-windows/lib ] || NINJA_JOBS=$JOBS bash scripts/crashpad/build.sh windows
    rm -f build-linux-x86_64/newview/*.tar.xz
    # EXTRA_CMAKE_FLAGS from the environment: the Makefile appends the channel, build number and
    # update key to it (on make's command line it would replace them)
    EXTRA_CMAKE_FLAGS='-DGCC_DISABLE_FATAL_WARNINGS=ON -DLIBVLCPLUGIN=ON' \
        CMAKE_BUILD_PARALLEL_LEVEL=$JOBS flock ~/.cache/heavy-job.lock nice -n 10 \
        make build TARBALL=1 VIEWER_CHANNEL="$V_CHANNEL" BUILD_ID="$V_BUILD"
    # wincross/build.sh names its files from version.sh, which reads the tag the CI way
    GITHUB_REF_TYPE=tag GITHUB_REF_NAME="$TAG" PATH="$ROOT/scripts/release:$PATH" NINJA_JOBS=$JOBS flock ~/.cache/heavy-job.lock nice -n 10 \
        make windows VIEWER_CHANNEL="$V_CHANNEL" BUILD_ID="$V_BUILD"
fi
tarball=$(ls -t build-linux-x86_64/newview/*.tar.xz 2>/dev/null | head -1)
setup="build-windows-x86_64/package/${V_ARTIFACT_BASE}_Setup.exe"
zip="build-windows-x86_64/package/${V_ARTIFACT_BASE}_win64.zip"
[ -f "$tarball" ] || die "no Linux tarball"
[ -f "$setup" ] || die "no $setup"
[ -f "$zip" ] || die "no $zip"
# the build must be the version the tag says (build_data.json, what the updater checks)
tar -xJOf "$tarball" --wildcards '*/build_data.json' | grep -q "\"$V_FULL_VERSION\"" \
    || die "$tarball is not build $V_FULL_VERSION"
cp "$tarball" "$OUT/${V_ARTIFACT_BASE}_x86_64.tar.xz"
cp "$setup" "$OUT/"
cp "$zip" "$OUT/"
( cd "$OUT" && sha256sum "${V_ARTIFACT_BASE}_x86_64.tar.xz" "${V_ARTIFACT_BASE}_Setup.exe" "${V_ARTIFACT_BASE}_win64.zip" > SHA256SUMS.txt )

# ---- symbols (BugSplat; application and version as the viewer reports them) ----
# shellcheck disable=SC1090
. "$CONF/bugsplat"
SU="$HOME/.cache/gmdx-3p/symbol-upload-linux"
[ -x "$SU" ] || { curl -sfL -o "$SU" https://app.bugsplat.com/download/symbol-upload-linux && chmod +x "$SU"; }
export SYMBOL_UPLOAD_CLIENT_ID="$client_id" SYMBOL_UPLOAD_CLIENT_SECRET="$client_secret"
"$SU" -b "$database" -a GunMetal-DX -v "$V_FULL_VERSION" -d build-linux-x86_64/newview -f "slipstream-bin" -m
"$SU" -b "$database" -a GunMetal-DX -v "$V_FULL_VERSION" -d build-windows-x86_64/newview -f "slipstream-bin.{pdb,exe}"
unset SYMBOL_UPLOAD_CLIENT_ID SYMBOL_UPLOAD_CLIENT_SECRET

# ---- signed update manifest + feed ----
dl="https://github.com/$REPO/releases/download/$TAG"
python3 - "$OUT" "$V_ARTIFACT_BASE" "$dl" > "$OUT/artifacts.json" <<'PY'
import json, os, sys, hashlib
out, base, dl = sys.argv[1:4]
def art(name):
    p = os.path.join(out, name)
    h = hashlib.sha256(open(p, 'rb').read()).hexdigest()
    return {"name": name, "url": f"{dl}/{name}", "sha256": h, "size": os.path.getsize(p)}
print(json.dumps({"linux_tarball": art(f"{base}_x86_64.tar.xz"), "windows_setup": art(f"{base}_Setup.exe")}))
PY
UPDATE_SIGNING_KEY_FILE="$KEY" python3 scripts/updater/sign_update_manifest.py sign "$OUT/artifacts.json" \
    --version "$V_VERSION" --build "$V_FULL_VERSION" --channel "$V_KIND" --out "$OUT/update-manifest.json"
pub=$(sed -n 's/^UPDATE_PUBKEY_HEX ?= *//p' Makefile)
python3 scripts/updater/sign_update_manifest.py verify "$OUT/update-manifest.json" --pubkey "$pub"
mkdir -p "$OUT/update"
python3 - "$OUT" "$V_FULL_VERSION" "$V_LABEL" "https://github.com/$REPO/releases/tag/$TAG" "$dl" "$V_ARTIFACT_BASE" <<'PY'
import json, os, sys
out, build, label, notes, dl, base = sys.argv[1:7]
feed = {"build": build, "label": label, "notes_url": notes,
        "downloads": {"windows_setup": f"{dl}/{base}_Setup.exe", "windows_zip": f"{dl}/{base}_win64.zip",
                      "linux_tarball": f"{dl}/{base}_x86_64.tar.xz"},
        "update": json.load(open(os.path.join(out, "update-manifest.json")))}
json.dump(feed, open(os.path.join(out, "update", "feed.json"), "w"), indent=1)
PY
# betas go to the beta feed; a release to both (the beta feed carries finals too)
if [ "$V_KIND" = release ]; then
    cp "$OUT/update/feed.json" "$OUT/update/release.json"
fi
mv "$OUT/update/feed.json" "$OUT/update/beta.json"

echo
echo "built and signed: $OUT (files, SHA256SUMS.txt, update/*.json)."
echo "Publish with scripts/release/publish.sh $TAG"
