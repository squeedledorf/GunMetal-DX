#!/bin/bash
# Sync the Slipstream Flatpak repo with the web host (#286).
#
# usage: publish_flatpak_repo.sh pull REPO
#          When REPO doesn't exist locally (new or rebuilt CI container), copy
#          it down from the host so the next commit continues its history (and
#          its static deltas). No-op when REPO exists or the host has none yet.
#        publish_flatpak_repo.sh push REPO FILES_DIR
#          Upload REPO to <host>/repo/ and the files build_flatpak.sh wrote in
#          FILES_DIR (slipstream.flatpakrepo, *.flatpakref, slipstream.gpg,
#          slipstream.png) to the host root.
#
# The host side is an rsync-only SSH key, forced to
# `rrsync /srv/flatpak.slipstreamviewer.org` (INFRA.md, "Deploy user"), so the
# paths here are relative to that directory.
#
# Upload order keeps the published repo consistent for clients updating while
# it runs: first every new object, static delta and summary index part (all
# content-addressed, nothing refers to them yet), then refs/ and the signed
# summary files that point at them, and only then are objects pruned locally
# deleted on the host (--delete-after).
#
# env: DEPLOY_KEY     SSH private key (default /home/ci/.ssh/slipstream_flatpak_deploy)
#      DEPLOY_TARGET  user@host (default sitedeploy@192.168.96.20)
set -euo pipefail
DEPLOY_KEY=${DEPLOY_KEY:-/home/ci/.ssh/slipstream_flatpak_deploy}
DEPLOY_TARGET=${DEPLOY_TARGET:-sitedeploy@192.168.96.20}

die() { echo "publish_flatpak_repo.sh: $*" >&2; exit 1; }
[ -r "$DEPLOY_KEY" ] || die "deploy key $DEPLOY_KEY missing or unreadable (INFRA.md, 'Deploy user')"
SSH="ssh -i $DEPLOY_KEY -o BatchMode=yes -o IdentitiesOnly=yes -o StrictHostKeyChecking=accept-new"
# Local-only parts of an OSTree repo.
LOCAL_ONLY=(--exclude=/tmp/ --exclude=/.lock --exclude=/state/)
SUMMARY=(--exclude=/summary --exclude=/summary.sig --exclude=/summary.idx --exclude=/summary.idx.sig --exclude=/refs/)

case "${1:-}" in
  pull)
    REPO=${2:?usage: publish_flatpak_repo.sh pull REPO}
    if [ -f "$REPO/config" ]; then echo "publish_flatpak_repo.sh: $REPO exists, not pulling"; exit 0; fi
    listing=$(rsync --list-only -e "$SSH" "$DEPLOY_TARGET:") \
      || die "could not list $DEPLOY_TARGET (refusing to start a new repo over an unreachable one)"
    if ! awk '{print $NF}' <<<"$listing" | grep -qx repo; then
      echo "publish_flatpak_repo.sh: the host has no repo yet; build_flatpak.sh starts a new one"
      exit 0
    fi
    mkdir -p "$REPO"
    rsync -rlt -e "$SSH" "${LOCAL_ONLY[@]}" "$DEPLOY_TARGET:repo/" "$REPO/"
    [ -f "$REPO/config" ] || die "pulled repo/ from $DEPLOY_TARGET but it has no config"
    echo "publish_flatpak_repo.sh: pulled the published repo into $REPO"
    ;;
  push)
    REPO=${2:?usage: publish_flatpak_repo.sh push REPO FILES_DIR}
    FILES=${3:?usage: publish_flatpak_repo.sh push REPO FILES_DIR}
    [ -f "$REPO/config" ] && [ -f "$REPO/summary" ] || die "$REPO is not an updated OSTree repo"
    [ -f "$REPO/summary.sig" ] || die "$REPO/summary is not signed; refusing to publish"
    RSYNC=(rsync -rlt --chmod=D755,F644 -e "$SSH")
    # 1. new content (no deletes, no refs/summary yet)
    "${RSYNC[@]}" "${LOCAL_ONLY[@]}" "${SUMMARY[@]}" "$REPO/" "$DEPLOY_TARGET:repo/"
    # 2. refs + signed summary, then delete what the local prune removed
    "${RSYNC[@]}" --delete-after "${LOCAL_ONLY[@]}" "$REPO/" "$DEPLOY_TARGET:repo/"
    # 3. repo description files at the root
    root=()
    for f in slipstream.flatpakrepo slipstream.flatpakref slipstream-beta.flatpakref slipstream.gpg slipstream.png; do
      [ -f "$FILES/$f" ] && root+=("$FILES/$f")
    done
    [ ${#root[@]} -gt 0 ] && "${RSYNC[@]}" "${root[@]}" "$DEPLOY_TARGET:"
    echo "publish_flatpak_repo.sh: published $(ostree --repo="$REPO" refs | tr '\n' ' ')to $DEPLOY_TARGET"
    ;;
  *) die "usage: publish_flatpak_repo.sh pull REPO | push REPO FILES_DIR" ;;
esac
