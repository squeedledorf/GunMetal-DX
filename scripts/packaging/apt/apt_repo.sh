#!/bin/bash
# Maintain the signed Slipstream apt repository (#288), served as
# https://apt.slipstreamviewer.org. See README.md (design) and INFRA.md (setup).
#
# usage: apt_repo.sh add TAG DEB       add a slipstream-viewer .deb for TAG
#        apt_repo.sh seed TAG...       download the .deb of each release TAG from
#                                      Forgejo (checked against the sha256 in the
#                                      release notes) and add it: first seeding
#                                      and disaster recovery
#        apt_repo.sh publish           prune to the newest $APT_KEEP versions per
#                                      suite, add the keyring package, sign and
#                                      (re)publish both suites, clean the pool,
#                                      write the static files, verify
#        apt_repo.sh deploy            rsync the published tree to the web host
#        apt_repo.sh verify            check the published tree's signatures
#        apt_repo.sh list              print what each suite holds
#
# `publish` signs, so run it under the signing helper:
#   scripts/ci/signing_key.sh run scripts/packaging/apt/apt_repo.sh publish
# (it needs GNUPGHOME and SIGNING_KEY_FPR from there).
#
# Suites (aptly local repos of the same name, published at the root prefix so
# they share one pool), component main, architecture amd64 (+ the arch:all
# keyring package):
#   stable   vX.Y.Z releases
#   beta     vX.Y.Z-beta.N betas AND vX.Y.Z releases: 1.0.0~beta6 < 1.0.0 <
#            1.0.1~beta1, so a beta user always has the newest build
#
# State: aptly's database + pool live in $APT_REPO_ROOT on the CI host
# (persistent, like the build trees) and are the source of truth; the web host
# only gets copies. If the state is ever lost, `seed` rebuilds it from the
# Forgejo releases (the newest $APT_KEEP tags per suite are enough).
#
# env: APT_REPO_ROOT   aptly root (default /srv/ci/apt-repo)
#      APT_KEEP        versions of each package kept per suite (default 3)
#      APT_URL         public URL (default https://apt.slipstreamviewer.org)
#      APT_DEPLOY_TARGET  rsync target (default aptdeploy@192.168.96.20:)
#      APT_DEPLOY_KEY  ssh key forced to rrsync on the web host
#                      (default /home/ci/.ssh/slipstream_apt_deploy)
#      RELEASE_API / DOWNLOAD_BASE  Forgejo API + release download base (seed)
#      SIGNING_KEY_FPR, GNUPGHOME   from scripts/ci/signing_key.sh (publish)
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
KEYS="$HERE/../keys"
ROOT=${APT_REPO_ROOT:-/srv/ci/apt-repo}
KEEP=${APT_KEEP:-3}
URL=${APT_URL:-https://apt.slipstreamviewer.org}
TARGET=${APT_DEPLOY_TARGET:-aptdeploy@192.168.96.20:}
DEPLOY_KEY=${APT_DEPLOY_KEY:-/home/ci/.ssh/slipstream_apt_deploy}
RELEASE_API=${RELEASE_API:-https://git.slipstreamviewer.org/api/v1/repos/sky_line/Slipstream}
DOWNLOAD_BASE=${DOWNLOAD_BASE:-https://git.slipstreamviewer.org/sky_line/Slipstream/releases/download}
SUITES=(stable beta)
PKG=slipstream-viewer
KEYRING_PKG=slipstream-archive-keyring
KEYRING_FILE=/usr/share/keyrings/slipstream-archive-keyring.asc
PUB="$ROOT/aptly/public"

die()  { echo "apt_repo.sh: $*" >&2; exit 1; }
note() { echo "apt_repo.sh: $*" >&2; }

command -v aptly >/dev/null 2>&1 || die "aptly not installed (apt install aptly; see INFRA.md)"
[[ "$KEEP" =~ ^[1-9][0-9]*$ ]] || die "APT_KEEP must be a positive number"

# --- aptly setup -------------------------------------------------------------
CONF="$ROOT/aptly.conf"
init() {
  mkdir -p "$ROOT/aptly" "$ROOT/incoming"
  # Rewritten every run so the committed script stays the only configuration.
  cat > "$CONF" <<EOF
{
  "rootDir": "$ROOT/aptly",
  "architectures": ["amd64"],
  "gpgDisableSign": false,
  "gpgDisableVerify": false,
  "gpgProvider": "gpg2",
  "downloadSourcePackages": false,
  "skipContentsPublishing": true,
  "FileSystemPublishEndpoints": {}
}
EOF
  local s
  for s in "${SUITES[@]}"; do
    if ! a repo show "$s" >/dev/null 2>&1; then
      a repo create -distribution="$s" -component=main \
        -comment="Slipstream Viewer ($s)" "$s" >/dev/null
      note "created aptly repo $s"
    fi
  done
}
a() { aptly -config="$CONF" "$@"; }

suites_for_tag() {
  if [[ "$1" =~ ^v[0-9]+\.[0-9]+\.[0-9]+-beta\.[0-9]+$ ]]; then echo beta
  elif [[ "$1" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]]; then echo stable beta
  else die "tag '$1' is not vX.Y.Z or vX.Y.Z-beta.N"
  fi
}

# Debian version of TAG, the same mapping as scripts/packaging/deb/build_deb.sh.
debver_of() {
  [[ "$1" =~ ^v([0-9]+\.[0-9]+\.[0-9]+)(-beta\.([0-9]+))?$ ]] || die "bad tag $1"
  echo "${BASH_REMATCH[1]}${BASH_REMATCH[3]:+~beta${BASH_REMATCH[3]}}"
}

# Package keys ("name_version_arch") of NAME in SUITE, one per line.
versions_in() {
  a repo search "$1" "Name ($2)" 2>/dev/null | sed -nE "s/^$2_([^_]+)_[a-z0-9]+\$/\1/p" | sort -u || true
}

# Versions on stdin, newest first (dpkg ordering: '~' sorts before everything).
sort_debver_desc() {
  local -a v=()
  local x i j t
  mapfile -t v
  for ((i = 1; i < ${#v[@]}; i++)); do
    x=${v[i]}; j=$((i - 1))
    while ((j >= 0)) && dpkg --compare-versions "${v[j]}" lt "$x"; do
      v[j + 1]=${v[j]}; j=$((j - 1))
    done
    v[j + 1]=$x
  done
  for t in "${v[@]}"; do echo "$t"; done
}

add_deb() {
  local tag=$1 deb=$2 s want got
  [ -f "$deb" ] || die "no such file: $deb"
  command -v dpkg-deb >/dev/null 2>&1 || die "dpkg-deb not installed"
  got=$(dpkg-deb -f "$deb" Package)
  [ "$got" = "$PKG" ] || die "$deb is package '$got', not $PKG"
  want=$(debver_of "$tag"); got=$(dpkg-deb -f "$deb" Version)
  # DEBREV rebuilds (X.Y.Z-N) are allowed: same upstream version.
  [ "${got%-[0-9]*}" = "$want" ] || die "$deb has version $got, but $tag means $want"
  for s in $(suites_for_tag "$tag"); do
    # Adding an identical file again is a no-op; a different file with the same
    # name/version is refused (aptly reports a conflict), which is what we want.
    a repo add "$s" "$deb"
    note "added $(basename "$deb") ($got) to $s"
  done
}

# sha256 that publish_release.sh appended to the release notes for ASSET.
notes_sha() {
  local tag=$1 asset=$2
  python3 -I "$HERE/../../ci/release_lookup.py" --api "$RELEASE_API" sha "$tag" "$asset"
}

seed() {
  local tag asset f want got
  mkdir -p "$ROOT/incoming"
  for tag in "$@"; do
    asset="Slipstream_${tag#v}_amd64.deb"
    f="$ROOT/incoming/$asset"
    # Draft-aware (TOKEN in the environment); falls back to the public URL for old releases.
    python3 -I "$HERE/../../ci/release_lookup.py" --api "$RELEASE_API" fetch "$tag" "$asset" "$f" 2>/dev/null \
      || curl -fsSL -o "$f" "$DOWNLOAD_BASE/$tag/$asset" || die "could not download $asset for $tag"
    want=$(notes_sha "$tag" "$asset"); got=$(sha256sum "$f" | cut -d' ' -f1)
    [ -n "$want" ] || die "$tag release notes list no sha256 for $asset"
    [ "$want" = "$got" ] || die "$asset sha256 $got does not match the release notes ($want)"
    add_deb "$tag" "$f"
    rm -f "$f"
  done
}

# Keep the newest $KEEP versions of every package in every suite.
prune() {
  local s name all keep cutoff
  for s in "${SUITES[@]}"; do
    for name in "$PKG" "$KEYRING_PKG"; do
      all=$(versions_in "$s" "$name")
      [ -n "$all" ] || continue
      keep=$(sort_debver_desc <<<"$all" | head -n "$KEEP")
      cutoff=$(tail -1 <<<"$keep")
      if [ "$(wc -l <<<"$all")" -gt "$KEEP" ]; then
        a repo remove "$s" "$name (<< $cutoff)" >/dev/null
        note "$s: removed $name older than $cutoff (keeping $(paste -sd' ' <<<"$keep"))"
      fi
    done
  done
}

# Tiny arch:all package that installs the public key where slipstream.sources'
# Signed-By points, so key updates (expiry extension, new subkey) reach users
# through apt. Version: keys/keyring-version. Built only when the committed key
# is the real one.
keyring() {
  local asc="$KEYS/slipstream-release.asc" ver work deb s
  if ! grep -q -- '-----BEGIN PGP PUBLIC KEY BLOCK-----' "$asc"; then
    note "keys/slipstream-release.asc is the placeholder: no $KEYRING_PKG package yet"
    return 0
  fi
  ver=$(tr -d ' \n' < "$KEYS/keyring-version")
  [[ "$ver" =~ ^[0-9]{4}\.[0-9]{2}\.[0-9]{2}(\.[0-9]+)?$ ]] || die "keys/keyring-version must be YYYY.MM.DD[.N], got '$ver'"
  local missing=0
  for s in "${SUITES[@]}"; do grep -qxF "$ver" <(versions_in "$s" "$KEYRING_PKG") || missing=1; done
  [ $missing = 1 ] || return 0
  work=$(mktemp -d)
  install -Dm644 "$asc" "$work/root$KEYRING_FILE"
  install -d "$work/root/usr/share/doc/$KEYRING_PKG" "$work/root/DEBIAN"
  cat > "$work/root/usr/share/doc/$KEYRING_PKG/copyright" <<'EOF'
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: Slipstream Viewer archive keyring
Source: https://git.slipstreamviewer.org/sky_line/Slipstream

Files: *
Copyright: 2026 The Slipstream Viewer contributors
License: public-domain
 The OpenPGP public key used to sign the Slipstream releases and apt repository.
EOF
  printf '%s (%s) unstable; urgency=medium\n\n  * Slipstream release signing key, see scripts/packaging/keys/README.md.\n\n -- Slipstream Viewer <noreply@slipstreamviewer.org>  %s\n' \
    "$KEYRING_PKG" "$ver" "$(LC_ALL=C date -u -R -d "${ver:0:4}-${ver:5:2}-${ver:8:2}")" \
    | gzip -9n > "$work/root/usr/share/doc/$KEYRING_PKG/changelog.gz"
  cat > "$work/root/DEBIAN/control" <<EOF
Package: $KEYRING_PKG
Version: $ver
Architecture: all
Maintainer: Slipstream Viewer <noreply@slipstreamviewer.org>
Installed-Size: $(du -sk --apparent-size "$work/root" | cut -f1)
Section: misc
Priority: optional
Multi-Arch: foreign
Homepage: https://slipstreamviewer.org
Description: OpenPGP key of the Slipstream apt repository
 Installs the Slipstream release signing key as
 $KEYRING_FILE, which the repository's
 slipstream.sources file names in Signed-By, so apt keeps the key up to date.
EOF
  ( cd "$work/root" && find . -path ./DEBIAN -prune -o -type f -printf '%P\0' | LC_ALL=C sort -z | xargs -0 md5sum ) > "$work/root/DEBIAN/md5sums"
  deb="$ROOT/incoming/${KEYRING_PKG}_${ver}_all.deb"
  SOURCE_DATE_EPOCH=$(date -u -d "${ver:0:4}-${ver:5:2}-${ver:8:2}" +%s) \
    dpkg-deb --root-owner-group -Zxz --build "$work/root" "$deb" >/dev/null
  rm -rf "$work"
  for s in "${SUITES[@]}"; do
    grep -qxF "$ver" <(versions_in "$s" "$KEYRING_PKG") || a repo add "$s" "$deb"
  done
  rm -f "$deb"
  note "added $KEYRING_PKG $ver"
}

# The public key file served at the repo root: the committed one when real,
# else the one exported from the signing keyring (pre-key test runs).
pubkey_file() {
  local asc="$KEYS/slipstream-release.asc"
  if grep -q -- '-----BEGIN PGP PUBLIC KEY BLOCK-----' "$asc"; then echo "$asc"
  elif [ -n "${SIGNING_PUBKEY:-}" ] && [ -f "$SIGNING_PUBKEY" ]; then echo "$SIGNING_PUBKEY"
  else die "no public key: keys/slipstream-release.asc is the placeholder and SIGNING_PUBKEY is unset"
  fi
}

publish() {
  : "${SIGNING_KEY_FPR:?run under scripts/ci/signing_key.sh run}"
  : "${GNUPGHOME:?run under scripts/ci/signing_key.sh run}"
  prune
  keyring
  local s
  mkdir -p "$ROOT/state"
  for s in "${SUITES[@]}"; do
    # The index generation clients may still be fetching (see prune_by_hash).
    [ -f "$PUB/dists/$s/Release" ] && cp "$PUB/dists/$s/Release" "$ROOT/state/prev-Release-$s"
    # An empty suite is published too, so `apt update` works for its users.
    if a publish list -raw 2>/dev/null | grep -qE "^\. $s\$"; then
      a publish update -batch -gpg-key="$SIGNING_KEY_FPR" "$s" >/dev/null
    else
      a publish repo -batch -gpg-key="$SIGNING_KEY_FPR" -acquire-by-hash \
        -origin=Slipstream -label=Slipstream -architectures=amd64 \
        -distribution="$s" -component=main "$s" >/dev/null
    fi
    note "published $s"
  done
  a db cleanup >/dev/null
  prune_by_hash
  # Static files at the repo root (aptly leaves files it doesn't own alone).
  mkdir -p "$PUB"
  install -m644 "$(pubkey_file)" "$PUB/slipstream-archive-keyring.asc"
  local f
  for f in slipstream.sources slipstream-beta.sources index.html; do
    sed "s|@APT_URL@|$URL|g; s|@KEYRING_FILE@|$KEYRING_FILE|g" "$HERE/$f" > "$PUB/$f"
    chmod 644 "$PUB/$f"
  done
  verify
}

# aptly adds by-hash copies of every index on each publish but never deletes
# old ones. Keep the current and the previous generation (a client that read
# the previous InRelease a moment ago can still fetch its indexes), drop the rest.
prune_by_hash() {
  local s keep n
  for s in "${SUITES[@]}"; do
    [ -d "$PUB/dists/$s" ] || continue
    keep=$( { cat "$PUB/dists/$s/Release"; cat "$ROOT/state/prev-Release-$s" 2>/dev/null || true; } \
           | awk 'NF==3 && $1 ~ /^[0-9a-f]+$/ {print $1}' | sort -u)
    n=0
    while IFS= read -r -d '' f; do
      grep -qxF "$(basename "$f")" <<<"$keep" || { rm -f "$f"; n=$((n + 1)); }
    done < <(find "$PUB/dists/$s" -path '*/by-hash/*' -type f -print0)
    [ $n = 0 ] || note "$s: removed $n stale by-hash files"
  done
}

verify() {
  local key s rel ok=0
  key=$(pubkey_file)
  command -v gpgv >/dev/null 2>&1 || die "gpgv not installed"
  local kr; kr=$(mktemp)
  # gpgv wants a binary keyring.
  local gh; gh=$(mktemp -d); chmod 700 "$gh"
  gpg --homedir "$gh" --batch --quiet --dearmor < "$key" > "$kr" 2>/dev/null || cp "$key" "$kr"
  gpgconf --homedir "$gh" --kill all >/dev/null 2>&1 || true; rm -rf "$gh"
  for s in "${SUITES[@]}"; do
    rel="$PUB/dists/$s"
    [ -f "$rel/InRelease" ] || { note "$s: not published"; continue; }
    gpgv --keyring "$kr" "$rel/InRelease" >/dev/null 2>&1 || { rm -f "$kr"; die "$s: InRelease signature does not verify"; }
    gpgv --keyring "$kr" "$rel/Release.gpg" "$rel/Release" >/dev/null 2>&1 || { rm -f "$kr"; die "$s: Release.gpg does not verify"; }
    # Every index Release lists must match its checksum.
    ( cd "$rel" && awk '/^SHA256:/{f=1;next} /^[A-Za-z]/{f=0} f && NF==3 {print $1"  "$3}' Release \
        | while read -r sum path; do [ -f "$path" ] && echo "$sum  $path"; done | sha256sum -c --quiet ) \
      || { rm -f "$kr"; die "$s: an index does not match Release"; }
    note "$s: InRelease + Release.gpg verified, indexes match"
    ok=1
  done
  rm -f "$kr"
  [ $ok = 1 ] || note "nothing published yet"
}

deploy() {
  [ -d "$PUB/dists" ] || die "nothing published in $PUB"
  [ -f "$DEPLOY_KEY" ] || die "deploy key $DEPLOY_KEY missing (INFRA.md)"
  local ssh="ssh -i $DEPLOY_KEY -o BatchMode=yes -o IdentitiesOnly=yes -o StrictHostKeyChecking=accept-new"
  local opts=(-rlt --chmod=D755,F644 -e "$ssh")
  # Order matters: a client must never see an InRelease that names a file the
  # server doesn't have yet. New pool files first, then the indexes (atomically
  # per run with --delay-updates), then drop the old pool files.
  rsync "${opts[@]}" "$PUB/pool/" "${TARGET}pool/"
  rsync "${opts[@]}" --delete --delay-updates "$PUB/dists/" "${TARGET}dists/"
  rsync "${opts[@]}" "$PUB/slipstream-archive-keyring.asc" "$PUB/slipstream.sources" \
    "$PUB/slipstream-beta.sources" "$PUB/index.html" "$TARGET"
  rsync "${opts[@]}" --delete "$PUB/pool/" "${TARGET}pool/"
  note "deployed to $TARGET"
}

list() {
  local s
  for s in "${SUITES[@]}"; do
    echo "== $s"
    a repo show -with-packages "$s" | sed -n '/^Packages:/,$p'
  done
}

cmd=${1:-}; shift || true
init
case "$cmd" in
  add)     add_deb "${1:?usage: apt_repo.sh add TAG DEB}" "${2:?usage: apt_repo.sh add TAG DEB}" ;;
  seed)    [ $# -gt 0 ] || die "usage: apt_repo.sh seed TAG..."; seed "$@" ;;
  publish) publish ;;
  deploy)  deploy ;;
  verify)  verify ;;
  list)    list ;;
  *) die "usage: apt_repo.sh add TAG DEB | seed TAG... | publish | deploy | verify | list" ;;
esac
