#!/bin/bash
# Point a branch of the Slipstream Flatpak repo (#286) at an existing build again.
#
# usage: repoint_branch.sh REPO BRANCH SRC OUT
#   REPO    the persistent OSTree repo (the flatpak job's $FP_ROOT/repo)
#   BRANCH  stable | beta
#   SRC     a commit checksum (or ref) already in REPO whose content BRANCH should serve
#   OUT     scratch dir for the exported public key
#
# Makes a NEW signed commit on BRANCH with SRC's content and subject (so clients
# see a normal update, also those that already pulled the wrong head), then
# regenerates the signed summary, deltas and pruning exactly like build_flatpak.sh.
# Publishing is publish_flatpak_repo.sh push. Used to undo a wrong branch head,
# e.g. v1.3.2 copied over v1.4.0-beta.4 on beta (fixed in build_flatpak.sh).
#
# env: SIGNING_KEY, SIGNING_KEY_ID, SIGNING_KEY_PASSPHRASE   as for build_flatpak.sh (required)
#      PRUNE_DEPTH   commits kept per branch (default 3)
set -euo pipefail
APP_ID=org.slipstreamviewer.Slipstream
REPO=${1:?usage: repoint_branch.sh REPO BRANCH SRC OUT}
BRANCH=${2:?usage: repoint_branch.sh REPO BRANCH SRC OUT}
SRC=${3:?usage: repoint_branch.sh REPO BRANCH SRC OUT}
OUT=${4:?usage: repoint_branch.sh REPO BRANCH SRC OUT}
PRUNE_DEPTH=${PRUNE_DEPTH:-3}

die() { echo "repoint_branch.sh: $*" >&2; exit 1; }
case "$BRANCH" in stable|beta) ;; *) die "BRANCH must be stable or beta" ;; esac
[ -f "$REPO/config" ] || die "$REPO is not an OSTree repo"
: "${SIGNING_KEY:?SIGNING_KEY (CI secret LINUX_SIGNING_KEY) is empty}"
: "${SIGNING_KEY_ID:?SIGNING_KEY_ID (CI secret LINUX_SIGNING_KEY_ID) is empty}"
mkdir -p "$OUT"

REF="app/$APP_ID/x86_64/$BRANCH"
SRC_SUM=$(ostree --repo="$REPO" rev-parse "$SRC") || die "$SRC is not in $REPO (pruned?)"
SUBJECT=$(ostree --repo="$REPO" show "$SRC_SUM" | sed -n '/^$/,$p' | sed -n 's/^    //p' | head -n1)
[ -n "$SUBJECT" ] || die "commit $SRC_SUM has no subject"
echo "repoint_branch.sh: $REF now $(ostree --repo="$REPO" log "$REF" | sed -n 's/^    //p' | head -n1)"
echo "repoint_branch.sh: re-committing $SRC_SUM ($SUBJECT) onto $REF"

# --- signing (as build_flatpak.sh) ----------------------------------------------
GNUPGHOME_TMP=$(mktemp -d "${GNUPG_TMP_PARENT:-/tmp}/fpgpg.XXXXXX")
trap 'gpgconf --homedir "$GNUPGHOME_TMP" --kill gpg-agent >/dev/null 2>&1 || true; rm -rf "$GNUPGHOME_TMP"' EXIT
chmod 700 "$GNUPGHOME_TMP"
printf 'allow-preset-passphrase\nallow-loopback-pinentry\npinentry-program /bin/false\n' \
  > "$GNUPGHOME_TMP/gpg-agent.conf"
printf '%s\n' "$SIGNING_KEY" | gpg --homedir "$GNUPGHOME_TMP" --batch --pinentry-mode loopback \
  --passphrase "${SIGNING_KEY_PASSPHRASE:-}" --import 2>&1 | sed 's/^/  gpg: /'
if [ -n "${SIGNING_KEY_PASSPHRASE:-}" ]; then
  PRESET=$(gpgconf --list-dirs libexecdir)/gpg-preset-passphrase
  [ -x "$PRESET" ] || die "gpg-preset-passphrase not found ($PRESET)"
  gpg-connect-agent --homedir "$GNUPGHOME_TMP" /bye >/dev/null 2>&1
  gpg --homedir "$GNUPGHOME_TMP" --batch --with-colons --with-keygrip --list-secret-keys \
    | awk -F: '$1=="grp" {print $10}' | while read -r grip; do
        printf '%s' "$SIGNING_KEY_PASSPHRASE" | "$PRESET" --homedir "$GNUPGHOME_TMP" --preset "$grip"
      done
fi
echo test | gpg --homedir "$GNUPGHOME_TMP" --batch --no-tty --local-user "$SIGNING_KEY_ID" \
  --detach-sign -o /dev/null || die "cannot sign with $SIGNING_KEY_ID"
gpg --homedir "$GNUPGHOME_TMP" --batch --export --export-options export-minimal "$SIGNING_KEY_ID" > "$OUT/slipstream.gpg"
SIGN=(--gpg-sign="$SIGNING_KEY_ID" --gpg-homedir="$GNUPGHOME_TMP")

# --- new commit, then the repo metadata (as build_flatpak.sh) ---------------------
# --timestamp=NOW: clients refuse an update whose commit is older than the one
# they have (downgrade protection), and SRC is older than the head it replaces.
# --force: commit even when the head already has the same content (a re-run).
flatpak build-commit-from "${SIGN[@]}" --force --timestamp=NOW --src-ref="$SRC_SUM" --subject="$SUBJECT" "$REPO" "$REF"

STABLE_BRANCH=stable
REFS=$(ostree --repo="$REPO" refs)
grep -qx "app/$APP_ID/x86_64/stable" <<<"$REFS" || STABLE_BRANCH=beta
flatpak build-update-repo "${SIGN[@]}" --generate-static-deltas \
  --prune --prune-depth="$PRUNE_DEPTH" \
  --title="Slipstream Viewer" --default-branch="$STABLE_BRANCH" \
  --homepage=https://slipstreamviewer.org/ \
  --comment="Vulkan viewer for Second Life" \
  "$REPO"
echo "repoint_branch.sh: $REF -> $(ostree --repo="$REPO" rev-parse "$REF") ($SUBJECT)"
ostree --repo="$REPO" refs | sed 's/^/  ref: /'
