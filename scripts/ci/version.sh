#!/bin/bash
# Resolve the version/channel/artifact naming for a build.
#
# Version scheme: MAJOR.MINOR.PATCH.BUILD (four numeric parts, as the LL plumbing
# expects). MAJOR.MINOR.PATCH lives in indra/newview/VIEWER_VERSION.txt (bump it with
# `make version-bump VERSION=x.y.z`):
#   MAJOR  a substantially different program
#   MINOR  features / new systems / small removals
#   PATCH  bug and hot fixes only, no behaviour changes
# BUILD counts the TAGGED builds of that MAJOR.MINOR.PATCH (it resets with every bump)
# and is derived from git tags alone, so it is reproducible and never depends on a CI
# counter:
#   vX.Y.Z-beta.N  -> BUILD N
#   vX.Y.Z         -> BUILD = highest vX.Y.Z-beta.N + 1 (1 when it had no betas)
#   anything else  -> BUILD 0 (local / ci-* / manual: not an official build)
# e.g. v0.13.0-beta.1 -> 0.13.0.1, v0.13.0-beta.2 -> 0.13.0.2, v0.13.0 -> 0.13.0.3,
# v0.13.1 -> 0.13.1.1.
#
# Tags select the channel:
#   v0.11.0            -> "<base> Release", a normal release
#   v0.11.0-beta.N     -> "<base> Beta",    a pre-release
#   jetstream-X.Y.Z.N  -> "<base> Jetstream", a pre-release built from main every 6 hours
#                         (BUILD N; files Slipstream_X.Y.Z-jetstream.N_*)
#   anything else      -> "<base> Test"     (vk-* legacy tags, ci-* dry runs, manual)
# A v* tag must match VIEWER_VERSION.txt, so the version inside the binary can't
# drift from the tag.
#
# Product name: CHANNEL_BASE here and CHANNEL_VENDOR_BASE in
# indra/lib/python/indra/util/llmanifest.py.
#
# usage: version.sh [TAG]    (TAG defaults to $GITHUB_REF_NAME when it is a tag)
# prints KEY=value lines: VERSION BUILD FULL_VERSION CHANNEL KIND PRERELEASE LABEL ARTIFACT_BASE
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
CHANNEL_BASE="GunMetal-DX"   # <GMDX>
ARTIFACT_PREFIX="GunMetal-DX"

TAG="${1:-}"
if [ -z "$TAG" ] && [ "${GITHUB_REF_TYPE:-}" = "tag" ]; then TAG="${GITHUB_REF_NAME:-}"; fi

VERSION=$(grep -oE '^[0-9]+\.[0-9]+\.[0-9]+' "$ROOT/indra/newview/VIEWER_VERSION.txt")
BUILD=0
SHORT=$(git -C "$ROOT" rev-parse --short=10 HEAD 2>/dev/null || echo unknown)

KIND=test
PRERELEASE=true
# Test builds: the CI run number (when there is one) + commit identify the artifact.
RUN="${GITHUB_RUN_NUMBER:-}"
LABEL="${VERSION}.0${RUN:+-r$RUN}-${SHORT}"
if [[ "$TAG" =~ ^v([0-9]+\.[0-9]+\.[0-9]+)(-beta\.([0-9]+))?$ ]]; then
  TAG_VERSION="${BASH_REMATCH[1]}"
  if [ "$TAG_VERSION" != "$VERSION" ]; then
    echo "version.sh: tag $TAG does not match VIEWER_VERSION.txt ($VERSION); run 'make version-bump VERSION=$TAG_VERSION' and commit" >&2
    exit 1
  fi
  if [ -n "${BASH_REMATCH[2]}" ]; then
    KIND=beta
    BUILD="${BASH_REMATCH[3]}"
  else
    KIND=release; PRERELEASE=false
    # One past this version's last beta (tags are fetched with the checkout).
    LAST=$(git -C "$ROOT" tag -l "v${VERSION}-beta.*" | sed -E 's/.*-beta\.//' | grep -E '^[0-9]+$' | sort -n | tail -1 || true)
    BUILD=$(( ${LAST:-0} + 1 ))
  fi
  LABEL="${TAG#v}"
elif [[ "$TAG" =~ ^jetstream-([0-9]+\.[0-9]+\.[0-9]+)\.([0-9]+)$ ]]; then
  # Jetstream: built every 6 hours from main when it has new commits
  # (.forgejo/workflows/jetstream.yml makes the tag). BUILD = the tag's running number.
  TAG_VERSION="${BASH_REMATCH[1]}"
  if [ "$TAG_VERSION" != "$VERSION" ]; then
    echo "version.sh: tag $TAG does not match VIEWER_VERSION.txt ($VERSION)" >&2
    exit 1
  fi
  KIND=jetstream
  BUILD="${BASH_REMATCH[2]}"
  LABEL="${VERSION}-jetstream.${BUILD}"
elif [[ "$TAG" =~ ^v[0-9] ]]; then
  echo "version.sh: tag $TAG is not vX.Y.Z or vX.Y.Z-beta.N" >&2
  exit 1
fi

case "$KIND" in
  release) CHANNEL="$CHANNEL_BASE Release" ;;
  beta)    CHANNEL="$CHANNEL_BASE Beta" ;;
  jetstream) CHANNEL="$CHANNEL_BASE Jetstream" ;;
  *)       CHANNEL="$CHANNEL_BASE Test" ;;
esac

echo "VERSION=$VERSION"
echo "BUILD=$BUILD"
echo "FULL_VERSION=$VERSION.$BUILD"
echo "CHANNEL=$CHANNEL"
echo "KIND=$KIND"
echo "PRERELEASE=$PRERELEASE"
echo "LABEL=$LABEL"
echo "ARTIFACT_BASE=${ARTIFACT_PREFIX}_${LABEL}"
