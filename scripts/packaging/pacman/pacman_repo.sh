#!/bin/bash
# Maintain the signed Slipstream pacman repositories (#419), served as
# https://arch.slipstreamviewer.org. See README.md (design, user install) and INFRA.md (setup).
#
# usage: pacman_repo.sh add TAG PKG    add a slipstream-bin .pkg.tar.zst built for TAG
#        pacman_repo.sh seed TAG...    get the package of each release TAG (the one the
#                                      arch-package job attached, checked against the sha256
#                                      in the release notes; built here from the release
#                                      tarball when none is attached) and add it
#        pacman_repo.sh publish        prune to the newest $PACMAN_KEEP versions per repo,
#                                      sign every package, (re)build + sign both databases,
#                                      write the static files, verify
#        pacman_repo.sh deploy         rsync the published tree to the web host
#        pacman_repo.sh verify         check the published tree's signatures
#        pacman_repo.sh list           print what each repo holds
#
# `publish` signs, so run it under the signing helper:
#   scripts/ci/signing_key.sh run scripts/packaging/pacman/pacman_repo.sh publish
# (it needs GNUPGHOME and SIGNING_KEY_FPR from there; the key itself comes from CI secrets
# and is never in git).
#
# Repos (pacman section names; Server = https://arch.slipstreamviewer.org/$repo/os/$arch):
#   slipstream        vX.Y.Z releases
#   slipstream-beta   vX.Y.Z-beta.N betas AND vX.Y.Z releases, so a beta user always has
#                     the newest build (pkgver X.Y.ZbetaN sorts below X.Y.Z in vercmp)
# Enable one of them, not both: both ship the package `slipstream-bin`.
#
# Package: `slipstream-bin`, built by scripts/packaging/aur (gen.sh + makepkg). The same
# package is what the AUR gets; it carries /opt/slipstream/.packaged-by, so the in-viewer
# auto-updater never touches a pacman install: pacman -Syu is the update path.
#
# State: $PACMAN_REPO_ROOT on the CI host (persistent) is the source of truth; the web host
# only gets copies. If it is lost, `seed` rebuilds it from the Forgejo releases.
#
# env: PACMAN_REPO_ROOT  state dir (default /srv/ci/pacman-repo)
#      PACMAN_KEEP       package files kept per repo (default 3)
#      PACMAN_URL        public URL (default https://arch.slipstreamviewer.org)
#      PACMAN_DEPLOY_TARGET  rsync target (default archdeploy@192.168.96.20:)
#      PACMAN_DEPLOY_KEY ssh key forced to rrsync on the web host
#                        (default /home/ci/.ssh/slipstream_arch_deploy)
#      RELEASE_API / DOWNLOAD_BASE  Forgejo API + release download base (seed)
#      SIGNING_KEY_FPR, GNUPGHOME   from scripts/ci/signing_key.sh (publish)
#      SIGNING_PUBKEY   exported public key (signing_key.sh), used while keys/ is a placeholder
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
KEYS="$HERE/../keys"
ROOT=${PACMAN_REPO_ROOT:-/srv/ci/pacman-repo}
KEEP=${PACMAN_KEEP:-3}
URL=${PACMAN_URL:-https://arch.slipstreamviewer.org}
TARGET=${PACMAN_DEPLOY_TARGET:-archdeploy@192.168.96.20:}
DEPLOY_KEY=${PACMAN_DEPLOY_KEY:-/home/ci/.ssh/slipstream_arch_deploy}
RELEASE_API=${RELEASE_API:-https://git.slipstreamviewer.org/api/v1/repos/sky_line/Slipstream}
DOWNLOAD_BASE=${DOWNLOAD_BASE:-https://git.slipstreamviewer.org/sky_line/Slipstream/releases/download}
REPOS=(slipstream slipstream-beta)
ARCH=x86_64
PKG=slipstream-bin
PUB="$ROOT/public"

die()  { echo "pacman_repo.sh: $*" >&2; exit 1; }
note() { echo "pacman_repo.sh: $*" >&2; }

[[ "$KEEP" =~ ^[1-9][0-9]*$ ]] || die "PACMAN_KEEP must be a positive number"
need() { command -v "$1" >/dev/null 2>&1 || die "$1 not installed ($2; see INFRA.md)"; }

# Where a repo's files live, relative to the docroot and under $ROOT/state / $PUB.
rel() { echo "$1/os/$ARCH"; }

repos_for_tag() {
  if [[ "$1" =~ ^v[0-9]+\.[0-9]+\.[0-9]+-beta\.[0-9]+$ ]]; then echo slipstream-beta
  elif [[ "$1" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]]; then echo slipstream slipstream-beta
  else die "tag '$1' is not vX.Y.Z or vX.Y.Z-beta.N"
  fi
}

# pkgver of TAG: the same mapping as scripts/packaging/aur/gen.sh.
pkgver_of() {
  [[ "$1" =~ ^v([0-9]+\.[0-9]+\.[0-9]+)(-beta\.([0-9]+))?$ ]] || die "bad tag $1"
  echo "${BASH_REMATCH[1]}${BASH_REMATCH[3]:+beta${BASH_REMATCH[3]}}"
}

# "slipstream-bin-1.2.0beta1-1-x86_64.pkg.tar.zst" -> "1.2.0beta1-1" (empty if not ours)
version_of_file() {
  local f; f=$(basename "$1")
  [[ "$f" =~ ^${PKG}-([0-9][^/]*-[0-9]+)-${ARCH}\.pkg\.tar\.zst$ ]] && echo "${BASH_REMATCH[1]}" || true
}

# Version-release strings on stdin, newest first (pacman's vercmp ordering).
sort_vercmp_desc() {
  need vercmp "apt install pacman-package-manager"
  local -a v=()
  local x i j t
  mapfile -t v
  for ((i = 1; i < ${#v[@]}; i++)); do
    x=${v[i]}; j=$((i - 1))
    while ((j >= 0)) && [ "$(vercmp "${v[j]}" "$x")" -lt 0 ]; do
      v[j + 1]=${v[j]}; j=$((j - 1))
    done
    v[j + 1]=$x
  done
  for t in "${v[@]}"; do echo "$t"; done
}

state_dir() { echo "$ROOT/state/$(rel "$1")"; }

add_pkg() {
  local tag=$1 pkg=$2 r want got d
  [ -f "$pkg" ] || die "no such file: $pkg"
  need bsdtar "apt install libarchive-tools"
  got=$(bsdtar -xOf "$pkg" .PKGINFO | sed -nE 's/^pkgname = (.*)$/\1/p' | head -1)
  [ "$got" = "$PKG" ] || die "$pkg is package '$got', not $PKG"
  want=$(pkgver_of "$tag")
  got=$(version_of_file "$pkg")
  [ "${got%-*}" = "$want" ] || die "$(basename "$pkg") has version '$got', but $tag means $want"
  for r in $(repos_for_tag "$tag"); do
    d=$(state_dir "$r"); mkdir -p "$d"
    if [ -f "$d/$(basename "$pkg")" ] && ! cmp -s "$pkg" "$d/$(basename "$pkg")"; then
      die "$r already holds a different $(basename "$pkg"); a package file never changes under the same name (bump pkgrel)"
    fi
    install -m644 "$pkg" "$d/"
    rm -f "$d/$(basename "$pkg").sig"      # signed again by publish
    note "added $(basename "$pkg") to $r"
  done
}

# sha256 that publish_release.sh appended to the release notes for ASSET.
notes_sha() {
  python3 -I "$HERE/../../ci/release_lookup.py" --api "$RELEASE_API" sha "$1" "$2"
}

# Build the package from the release tarball (what the arch-package job does), for a release
# that has no package attached.
build_pkg() {
  local tag=$1 out=$2 w
  need makepkg "apt install pacman-package-manager"
  w=$(mktemp -d "$ROOT/build.XXXXXX")
  mkdir -p "$w/out" "$w/src" "$w/pkg"
  API_BASE="$RELEASE_API" DOWNLOAD_BASE="$DOWNLOAD_BASE" \
    bash "$HERE/../aur/gen.sh" -o "$w/out" "$tag" >&2
  ( cd "$w/out" && SRCDEST="$w/src" PKGDEST="$w/pkg" BUILDDIR="$w/build" PKGEXT=.pkg.tar.zst \
      PACKAGER='Slipstream CI <noreply@slipstreamviewer.org>' makepkg -f -d --noconfirm >&2 )
  cp "$w"/pkg/${PKG}-*.pkg.tar.zst "$out/"
  rm -rf "$w"
}

seed() {
  local tag ver names name f want got
  mkdir -p "$ROOT/incoming"
  for tag in "$@"; do
    ver=$(pkgver_of "$tag")
    name=$(python3 -I "$HERE/../../ci/release_lookup.py" --api "$RELEASE_API" json "$tag" | python3 -c '
import json, sys
ver = sys.argv[1]
for a in json.load(sys.stdin).get("assets", []):
    n = a["name"]
    if n.startswith("slipstream-bin-" + ver + "-") and n.endswith("-x86_64.pkg.tar.zst"):
        print(n)
        break' "$ver")
    if [ -n "$name" ]; then
      f="$ROOT/incoming/$name"
      python3 -I "$HERE/../../ci/release_lookup.py" --api "$RELEASE_API" fetch "$tag" "$name" "$f" 2>/dev/null \
        || curl -fsSL -o "$f" "$DOWNLOAD_BASE/$tag/$name" || die "could not download $name for $tag"
      want=$(notes_sha "$tag" "$name"); got=$(sha256sum "$f" | cut -d' ' -f1)
      [ -n "$want" ] || die "$tag release notes list no sha256 for $name"
      [ "$want" = "$got" ] || die "$name sha256 $got does not match the release notes ($want)"
    else
      note "$tag has no attached package; building it from the release tarball"
      rm -f "$ROOT"/incoming/${PKG}-*.pkg.tar.zst
      build_pkg "$tag" "$ROOT/incoming"
      f=$(ls "$ROOT"/incoming/${PKG}-*.pkg.tar.zst)
    fi
    add_pkg "$tag" "$f"
    rm -f "$f"
  done
}

# Keep the newest $KEEP package files of every repo.
prune() {
  local r d f v keep cutoff
  for r in "${REPOS[@]}"; do
    d=$(state_dir "$r"); [ -d "$d" ] || continue
    keep=$(for f in "$d"/${PKG}-*.pkg.tar.zst; do [ -f "$f" ] && version_of_file "$f"; done | sort -u | sort_vercmp_desc | head -n "$KEEP")
    [ -n "$keep" ] || continue
    for f in "$d"/${PKG}-*.pkg.tar.zst; do
      [ -f "$f" ] || continue
      v=$(version_of_file "$f")
      grep -qxF "$v" <<<"$keep" || { rm -f "$f" "$f.sig"; note "$r: removed $(basename "$f")"; }
    done
  done
}

# The public key file served at the repo root.
pubkey_file() {
  local asc="$KEYS/slipstream-release.asc"
  # The key that signs right now (exported by signing_key.sh, which refuses a mismatch with
  # the committed one) wins; `verify`/`deploy` outside a signing run use the committed key.
  if [ -n "${SIGNING_PUBKEY:-}" ] && [ -f "$SIGNING_PUBKEY" ]; then echo "$SIGNING_PUBKEY"
  elif grep -q -- '-----BEGIN PGP PUBLIC KEY BLOCK-----' "$asc"; then echo "$asc"
  else die "no public key: keys/slipstream-release.asc is the placeholder and SIGNING_PUBKEY is unset"
  fi
}

publish() {
  : "${SIGNING_KEY_FPR:?run under scripts/ci/signing_key.sh run}"
  : "${GNUPGHOME:?run under scripts/ci/signing_key.sh run}"
  need repo-add "apt install pacman-package-manager"
  need gpg "apt install gnupg"
  prune
  rm -rf "$PUB.new"
  local r d out f
  for r in "${REPOS[@]}"; do
    d=$(state_dir "$r"); out="$PUB.new/$(rel "$r")"
    mkdir -p "$out"
    # Packages: copy, detached binary signature (pacman SigLevel = Required checks .sig).
    for f in "$d"/${PKG}-*.pkg.tar.zst; do
      [ -f "$f" ] || continue
      install -m644 "$f" "$out/"
      gpg --batch --yes -u "$SIGNING_KEY_FPR" --detach-sign --no-armor -o "$out/$(basename "$f").sig" "$out/$(basename "$f")"
    done
    # Databases: repo-add lists the newest package of each name (here: the newest of
    # slipstream-bin), signs the .db and .files archives (--sign) and checks nothing else.
    # An empty repo still gets an (empty) database so `pacman -Sy` works for its users.
    if ls "$out"/${PKG}-*.pkg.tar.zst >/dev/null 2>&1; then
      local newest
      newest=$(for f in "$out"/${PKG}-*.pkg.tar.zst; do echo "$(version_of_file "$f") $f"; done \
               | while read -r v p; do echo "$v"; done | sort_vercmp_desc | head -1)
      ( cd "$out" && repo-add --quiet --sign --key "$SIGNING_KEY_FPR" "$r.db.tar.zst" ${PKG}-"$newest"-$ARCH.pkg.tar.zst )
    else
      # repo-add refuses to create a database without a package: write the empty archive
      # (two zero blocks = a tar with no entries) ourselves.
      need zstd "apt install zstd"
      local kind
      for kind in db files; do
        head -c 10240 /dev/zero | zstd -q -o "$out/$r.$kind.tar.zst"
        cp "$out/$r.$kind.tar.zst" "$out/$r.$kind"
        gpg --batch --yes -u "$SIGNING_KEY_FPR" --detach-sign --no-armor -o "$out/$r.$kind.tar.zst.sig" "$out/$r.$kind.tar.zst"
        cp "$out/$r.$kind.tar.zst.sig" "$out/$r.$kind.sig"
      done
    fi
    # repo-add leaves "name.db -> name.db.tar.zst" symlinks (and for .files / .sig): publish
    # real files, so neither rsync nor the web server depends on symlinks.
    for f in "$out"/*; do
      [ -L "$f" ] || continue
      cp -L --remove-destination "$f" "$f.copy" && mv -f "$f.copy" "$f"
    done
    rm -f "$out"/*.old "$out"/*.old.sig
    note "published $r"
  done
  install -m644 "$(pubkey_file)" "$PUB.new/slipstream.asc"
  local pacconf
  for pacconf in index.html slipstream.conf slipstream-beta.conf; do
    sed "s|@PACMAN_URL@|$URL|g; s|@FPR@|$SIGNING_KEY_FPR|g" "$HERE/$pacconf" > "$PUB.new/$pacconf"
    chmod 644 "$PUB.new/$pacconf"
  done
  verify_tree "$PUB.new"
  rm -rf "$PUB.old"; [ -d "$PUB" ] && mv "$PUB" "$PUB.old"
  mv "$PUB.new" "$PUB"; rm -rf "$PUB.old"
}

verify_tree() {
  local tree=$1 key r out f kr gh
  key=$(pubkey_file)
  need gpgv "apt install gpgv"
  kr=$(mktemp); gh=$(mktemp -d); chmod 700 "$gh"
  gpg --homedir "$gh" --batch --quiet --dearmor < "$key" > "$kr" 2>/dev/null || cp "$key" "$kr"
  gpgconf --homedir "$gh" --kill all >/dev/null 2>&1 || true; rm -rf "$gh"
  for r in "${REPOS[@]}"; do
    out="$tree/$(rel "$r")"
    [ -f "$out/$r.db" ] || { note "$r: not published"; continue; }
    for f in "$out/$r.db" "$out/$r.files" "$out"/${PKG}-*.pkg.tar.zst; do
      [ -f "$f" ] || continue
      gpgv --keyring "$kr" "$f.sig" "$f" >/dev/null 2>&1 || { rm -f "$kr"; die "$r: signature of $(basename "$f") does not verify"; }
    done
    # Every package in the database must exist as a file next to it.
    local entry
    for entry in $(bsdtar -tf "$out/$r.db" | sed -nE 's#^([^/]+)/$#\1#p'); do
      [ -f "$out/${entry}-$ARCH.pkg.tar.zst" ] || { rm -f "$kr"; die "$r: database entry $entry has no package file"; }
    done
    note "$r: database + $(ls "$out"/${PKG}-*.pkg.tar.zst 2>/dev/null | wc -l) package signature(s) verified"
  done
  rm -f "$kr"
}

verify() { [ -d "$PUB" ] || die "nothing published in $PUB"; verify_tree "$PUB"; }

deploy() {
  [ -d "$PUB" ] || die "nothing published in $PUB"
  [ -f "$DEPLOY_KEY" ] || die "deploy key $DEPLOY_KEY missing (INFRA.md)"
  local ssh="ssh -i $DEPLOY_KEY -o BatchMode=yes -o IdentitiesOnly=yes -o StrictHostKeyChecking=accept-new"
  local opts=(-rt --chmod=D755,F644 -e "$ssh")
  local r
  # Order matters: a client must never see a database that names a package the server
  # does not have yet. Packages (+ signatures) first, then the databases (atomically per
  # run with --delay-updates), then drop the old package files.
  for r in "${REPOS[@]}"; do
    rsync "${opts[@]}" --include='*.pkg.tar.zst' --include='*.pkg.tar.zst.sig' --exclude='*' \
      "$PUB/$(rel "$r")/" "${TARGET}$(rel "$r")/"
    rsync "${opts[@]}" --delay-updates --exclude='*.pkg.tar.zst' --exclude='*.pkg.tar.zst.sig' \
      "$PUB/$(rel "$r")/" "${TARGET}$(rel "$r")/"
  done
  rsync "${opts[@]}" "$PUB/slipstream.asc" "$PUB/slipstream.conf" "$PUB/slipstream-beta.conf" "$PUB/index.html" "$TARGET"
  for r in "${REPOS[@]}"; do
    rsync "${opts[@]}" --delete --include='*.pkg.tar.zst' --include='*.pkg.tar.zst.sig' --exclude='*' \
      "$PUB/$(rel "$r")/" "${TARGET}$(rel "$r")/"
  done
  note "deployed to $TARGET"
}

list() {
  local r d f
  for r in "${REPOS[@]}"; do
    echo "== $r"
    d=$(state_dir "$r")
    for f in "$d"/${PKG}-*.pkg.tar.zst; do [ -f "$f" ] && basename "$f"; done || true
  done
}

cmd=${1:-}; shift || true
mkdir -p "$ROOT/state" "$ROOT/incoming"
case "$cmd" in
  add)     add_pkg "${1:?usage: pacman_repo.sh add TAG PKG}" "${2:?usage: pacman_repo.sh add TAG PKG}" ;;
  seed)    [ $# -gt 0 ] || die "usage: pacman_repo.sh seed TAG..."; seed "$@" ;;
  publish) publish ;;
  deploy)  deploy ;;
  verify)  verify ;;
  list)    list ;;
  *) die "usage: pacman_repo.sh add TAG PKG | seed TAG... | publish | deploy | verify | list" ;;
esac
