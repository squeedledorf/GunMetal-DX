#!/bin/bash
# Local test of pacman_repo.sh (#419) with a throw-away GPG key and tiny fake packages.
# Needs an Arch-like userland (makepkg, repo-add, vercmp, bsdtar), gpg, gpgv, rsync. Touches
# nothing outside a temp dir; "deploys" to a local directory.
# usage: scripts/packaging/pacman/test_pacman_repo.sh
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
S="$HERE/pacman_repo.sh"
for c in makepkg repo-add vercmp bsdtar gpg gpgv rsync; do
  command -v "$c" >/dev/null 2>&1 || { echo "SKIP: $c not installed"; exit 0; }
done
T=$(mktemp -d)
trap 'gpgconf --homedir "$T/gnupg" --kill all >/dev/null 2>&1 || true; [ -n "${KEEP_TMP:-}" ] && echo "kept $T" || rm -rf "$T"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }

# throw-away signing key
export GNUPGHOME="$T/gnupg"; mkdir -m 700 "$GNUPGHOME"
printf 'batch\nno-tty\n' > "$GNUPGHOME/gpg.conf"
gpg --quiet --passphrase '' --quick-gen-key 'Test Signer <test@example.invalid>' ed25519 sign never 2>/dev/null
FPR=$(gpg --list-keys --with-colons | awk -F: '/^fpr/ {print $10; exit}')
gpg --armor --export "$FPR" > "$T/pub.asc"

mkpkg() {   # pkgver -> prints package path
  local d="$T/pkgbuild-$1"
  mkdir -p "$d"
  cat > "$d/PKGBUILD" <<EOF
pkgname=slipstream-bin
pkgver=$1
pkgrel=1
pkgdesc='test'
arch=('x86_64')
license=('custom')
package() { install -d "\$pkgdir/opt/slipstream"; echo test > "\$pkgdir/opt/slipstream/.packaged-by"; }
EOF
  ( cd "$d" && PKGDEST="$T/pkgs" PKGEXT=.pkg.tar.zst SRCDEST="$d" BUILDDIR="$d/b" makepkg -f -d --noconfirm >/dev/null 2>&1 )
  echo "$T/pkgs/slipstream-bin-$1-1-x86_64.pkg.tar.zst"
}
P_B1=$(mkpkg 1.0.0beta1); P_B2=$(mkpkg 1.0.0beta2); P_REL=$(mkpkg 1.0.0)

export PACMAN_REPO_ROOT="$T/repo" PACMAN_KEEP=2 PACMAN_URL=https://arch.example.invalid
export PACMAN_DEPLOY_KEY="$T/deploykey" PACMAN_DEPLOY_TARGET="$T/web/"
# The web host's docroot has the repo directories (INFRA.md step 3); rrsync would not create them.
: > "$PACMAN_DEPLOY_KEY"; mkdir -p "$T/web/slipstream/os/x86_64" "$T/web/slipstream-beta/os/x86_64"
export SIGNING_KEY_FPR="$FPR" SIGNING_PUBKEY="$T/pub.asc"

# add: betas go to the beta repo only, a final to both; wrong version for the tag is refused
"$S" add v1.0.0-beta.1 "$P_B1"
"$S" add v1.0.0-beta.2 "$P_B2"
if "$S" add v1.0.0-beta.2 "$P_B1" 2>/dev/null; then fail "version/tag mismatch accepted"; fi
if "$S" add v1.0.1 "$P_REL" 2>/dev/null; then fail "version/tag mismatch accepted (final)"; fi
if "$S" add vk-abc "$P_REL" 2>/dev/null; then fail "bad tag accepted"; fi
"$S" add v1.0.0 "$P_REL"
out=$("$S" list)
stable_list=$(awk '/^== /{on=($2=="slipstream")} on && !/^==/' <<<"$out")
grep -q 'slipstream-bin-1.0.0-1' <<<"$stable_list" || fail "stable repo lacks the release"
grep -q beta <<<"$stable_list" && fail "a beta leaked into the stable repo"

# publish: keeps the newest 2 files per repo, signs packages + databases
"$S" publish
PUB="$PACMAN_REPO_ROOT/public"
for r in slipstream slipstream-beta; do
  d="$PUB/$r/os/x86_64"
  [ -f "$d/$r.db" ] && [ ! -L "$d/$r.db" ] && [ -f "$d/$r.db.sig" ] || fail "$r: database / signature missing or a symlink"
  [ -f "$d/$r.files" ] || fail "$r: files db missing"
  find "$d" -type l | grep -q . && fail "$r: symlinks in the published tree"
  for f in "$d"/slipstream-bin-*.pkg.tar.zst; do [ -f "$f.sig" ] || fail "no signature for $f"; done
done
[ "$(ls "$PUB"/slipstream/os/x86_64/slipstream-bin-*.pkg.tar.zst | wc -l)" = 1 ] || fail "stable repo should hold one file"
[ "$(ls "$PUB"/slipstream-beta/os/x86_64/slipstream-bin-*.pkg.tar.zst | wc -l)" = 2 ] || fail "beta repo should hold the newest 2 (KEEP=2)"
ls "$PUB"/slipstream-beta/os/x86_64/ | grep -q 'beta1' && fail "oldest beta not pruned"
bsdtar -tf "$PUB/slipstream-beta/os/x86_64/slipstream-beta.db" | grep -q '^slipstream-bin-1.0.0-1/$' \
  || fail "beta database should list the newest version (1.0.0 > 1.0.0beta2)"
[ -f "$PUB/slipstream.asc" ] && grep -q "$FPR" "$PUB/index.html" && grep -q 'Server = https://arch.example.invalid/\$repo/os/\$arch' "$PUB/slipstream.conf" \
  || fail "static files"
grep -q '^\[slipstream-beta\]' "$PUB/slipstream-beta.conf" || fail "beta conf"

# a signature that is checked: gpgv with the exported key accepts the db, a tampered package fails verify
gpg --batch --quiet --dearmor < "$T/pub.asc" > "$T/pub.gpg"
gpgv --keyring "$T/pub.gpg" "$PUB/slipstream/os/x86_64/slipstream.db.sig" "$PUB/slipstream/os/x86_64/slipstream.db" 2>/dev/null \
  || fail "database signature does not verify"
"$S" verify
pkg=$(ls "$PUB"/slipstream/os/x86_64/slipstream-bin-*.pkg.tar.zst)
cp "$pkg" "$T/orig"; echo x >> "$pkg"
if "$S" verify 2>/dev/null; then fail "verify accepted a tampered package"; fi
cp "$T/orig" "$pkg"; "$S" verify

# deploy (local rsync) puts packages, databases and the static files in place, no symlinks
"$S" deploy
for r in slipstream slipstream-beta; do
  [ -f "$T/web/$r/os/x86_64/$r.db" ] && [ -f "$T/web/$r/os/x86_64/$r.db.sig" ] || fail "deploy: $r database"
done
[ -f "$T/web/slipstream.asc" ] && [ -f "$T/web/index.html" ] || fail "deploy: static files"
find "$T/web" -type l | grep -q . && fail "deploy: symlinks"

# a stale package on the web host is removed by the next deploy
echo junk > "$T/web/slipstream/os/x86_64/slipstream-bin-0.0.1-1-x86_64.pkg.tar.zst"
"$S" deploy
[ ! -e "$T/web/slipstream/os/x86_64/slipstream-bin-0.0.1-1-x86_64.pkg.tar.zst" ] || fail "stale package kept"

# publish twice is stable; an empty repo still publishes a database
rm -rf "$PACMAN_REPO_ROOT/state/slipstream"
"$S" publish
[ -f "$PUB/slipstream/os/x86_64/slipstream.db" ] || fail "empty repo has no database"
echo "pacman repo tests OK"
