#!/bin/bash
# Generate the slipstream-bin PKGBUILD + .SRCINFO for a release tag (#248).
#
# usage: gen.sh [-o OUTDIR] TAG [SHA256]
#          Write OUTDIR/PKGBUILD and OUTDIR/.SRCINFO for TAG (vX.Y.Z or
#          vX.Y.Z-beta.N). OUTDIR defaults to this directory, i.e. it updates the
#          in-repo copies. Without SHA256, the checksum is read from the release
#          notes (publish_release.sh appends "<sha256>  <asset>" for every
#          upload); if it isn't there, the tarball is downloaded and hashed.
#        gen.sh --pkgver TAG        print the pkgver for TAG
#        gen.sh --selftest          check the pkgver scheme with vercmp
#        gen.sh --check-srcinfo     diff the built-in .SRCINFO writer against
#                                   `makepkg --printsrcinfo` (needs makepkg)
#
# The PKGBUILD in this directory is the template: only its pkgver=, pkgrel=,
# _tag= and sha256sums= lines are rewritten. Edit everything else there.
#
# pkgver scheme: vX.Y.Z-beta.N -> X.Y.ZbetaN, vX.Y.Z -> X.Y.Z. pacman's vercmp
# sorts "1.0beta1" below "1.0" and compares the beta number numerically
# (beta10 > beta9), so every final sorts above its betas. pkgver may not
# contain '-', so the tag can't be used as is.
#
# .SRCINFO comes from `makepkg --printsrcinfo` when makepkg is installed, and
# from the equivalent built-in writer below otherwise (the CI builder is Ubuntu
# and has no makepkg). --check-srcinfo proves the two agree.
#
# Signature (#288): with PGP_FPR set (the release key's primary fingerprint),
# the source array also gets the tarball's detached signature
# (Slipstream_<ver>_x86_64.tar.xz.sig, attached to the release by CI) with
# checksum SKIP, and validpgpkeys=(PGP_FPR), so makepkg verifies the tarball
# against the key. When ../keys/slipstream-release.asc holds the real key,
# OUTDIR/keys/pgp/<PGP_FPR>.asc is written too (the Arch convention for
# shipping the key next to a PKGBUILD; aur_push.sh pushes it). Without PGP_FPR
# the PKGBUILD stays unsigned, as for releases from before the key existed.
#
# env: PKGREL (default 1)
#      PGP_FPR        primary key fingerprint: add the .sig + validpgpkeys
#      API_BASE       Forgejo API of the repo (default: the public URL; CI passes the LAN one)
#      DOWNLOAD_BASE  release download base, used for the hash fallback
#      SRCINFO=builtin  force the built-in .SRCINFO writer
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
TEMPLATE="$HERE/PKGBUILD"
API_BASE=${API_BASE:-https://git.slipstreamviewer.org/api/v1/repos/sky_line/Slipstream}
DOWNLOAD_BASE=${DOWNLOAD_BASE:-https://git.slipstreamviewer.org/sky_line/Slipstream/releases/download}

die() { echo "gen.sh: $*" >&2; exit 1; }

pkgver_of() {
  if [[ "$1" =~ ^v([0-9]+\.[0-9]+\.[0-9]+)(-beta\.([0-9]+))?$ ]]; then
    echo "${BASH_REMATCH[1]}${BASH_REMATCH[3]:+beta${BASH_REMATCH[3]}}"
  else
    die "tag '$1' is not vX.Y.Z or vX.Y.Z-beta.N"
  fi
}

asset_of() { echo "Slipstream_${1#v}_x86_64.tar.xz"; }

# The asset's sha256 from the release notes, or by downloading it.
sha_of() {
  local tag=$1 asset sha=""
  asset=$(asset_of "$tag")
  sha=$(curl -fsS "$API_BASE/releases/tags/$tag" 2>/dev/null \
        | python3 -c 'import json,sys; print(json.load(sys.stdin).get("body",""))' 2>/dev/null \
        | sed -nE "s/^([0-9a-f]{64})  ${asset//./\\.}\$/\1/p" | tail -1) || true
  if [ -z "$sha" ]; then
    echo "gen.sh: no checksum for $asset in the $tag release notes; downloading it" >&2
    sha=$(curl -fsSL "$DOWNLOAD_BASE/$tag/$asset" | sha256sum | cut -d' ' -f1)
  fi
  [[ "$sha" =~ ^[0-9a-f]{64}$ ]] || die "could not determine the sha256 of $asset"
  echo "$sha"
}

# Same output as makepkg --printsrcinfo for this PKGBUILD's shape (one package,
# no per-package overrides): see /usr/share/makepkg/srcinfo.sh.
srcinfo_builtin() (
  cd "$1"
  # shellcheck disable=SC1091
  source ./PKGBUILD
  shopt -s extglob
  w() {
    local name=$1 v; shift
    for v in "$@"; do
      v=${v//+([[:space:]])/ }; v=${v#[[:space:]]}; v=${v%[[:space:]]}
      printf '\t%s = %s\n' "$name" "$v"
    done
  }
  local a hash
  local hashes=(ck md5 sha1 sha224 sha256 sha384 sha512 b2)
  printf 'pkgbase = %s\n' "${pkgbase:-$pkgname}"
  for a in pkgdesc pkgver pkgrel epoch url install changelog; do
    [ -n "${!a+x}" ] && w "$a" "${!a}"
  done
  for a in arch groups license checkdepends makedepends depends optdepends \
           provides conflicts replaces noextract options backup source validpgpkeys \
           "${hashes[@]/%/sums}"; do
    declare -n ref=$a
    [ -n "${ref+x}" ] && w "$a" "${ref[@]}"
    unset -n ref
  done
  for a in "${arch[@]}"; do
    [ "$a" = any ] && continue
    for hash in source provides conflicts depends replaces optdepends makedepends \
                checkdepends "${hashes[@]/%/sums}"; do
      declare -n ref="${hash}_$a"
      [ -n "${ref+x}" ] && w "${hash}_$a" "${ref[@]}"
      unset -n ref
    done
  done
  printf '\npkgname = %s\n' "$pkgname"
)

write_srcinfo() {
  local dir=$1
  # Ubuntu's packaged makepkg can die with "unknown error" outside Arch (applejack);
  # the built-in writer produces the same .SRCINFO, so fall back to it.
  if [ "${SRCINFO:-}" != builtin ] && command -v makepkg >/dev/null 2>&1 \
     && ( cd "$dir" && makepkg --printsrcinfo ) > "$dir/.SRCINFO.tmp" 2>/dev/null; then
    mv "$dir/.SRCINFO.tmp" "$dir/.SRCINFO"
  else
    rm -f "$dir/.SRCINFO.tmp"
    srcinfo_builtin "$dir" > "$dir/.SRCINFO"
  fi
}

case "${1:-}" in
  --pkgver) pkgver_of "${2:?usage: gen.sh --pkgver TAG}"; exit 0 ;;
  --selftest)
    command -v vercmp >/dev/null || die "vercmp (pacman) not installed"
    fail=0
    # pairs that must sort strictly ascending
    while read -r lo hi; do
      a=$(pkgver_of "$lo"); b=$(pkgver_of "$hi"); r=$(vercmp "$a" "$b")
      printf '%-18s %-14s < %-14s vercmp=%2s %s\n' "$lo/$hi" "$a" "$b" "$r" "$([ "$r" = -1 ] && echo ok || echo FAIL)"
      [ "$r" = -1 ] || fail=1
    done <<'EOF'
v0.14.0-beta.1 v0.14.0
v0.14.0-beta.9 v0.14.0-beta.10
v0.14.0-beta.1 v0.14.0-beta.2
v0.12.0-beta.10 v0.13.0-beta.1
v0.13.0 v0.14.0-beta.1
v0.14.0 v0.14.1-beta.1
v0.14.0 v0.14.1
v0.99.0 v1.0.0-beta.1
v1.0.0-beta.12 v1.0.0
EOF
    exit $fail ;;
  --check-srcinfo)
    command -v makepkg >/dev/null || die "makepkg not installed"
    diff -u <(cd "$HERE" && makepkg --printsrcinfo) <(srcinfo_builtin "$HERE") \
      && echo "built-in .SRCINFO matches makepkg --printsrcinfo"
    exit ;;
esac

OUT=$HERE
if [ "${1:-}" = -o ]; then OUT=${2:?-o needs a directory}; shift 2; fi
TAG=${1:?usage: gen.sh [-o OUTDIR] TAG [SHA256]}
PKGVER=$(pkgver_of "$TAG")
SHA=${2:-$(sha_of "$TAG")}
[[ "$SHA" =~ ^[0-9a-f]{64}$ ]] || die "bad sha256 '$SHA'"
PKGREL=${PKGREL:-1}
FPR=$(tr -d ' ' <<<"${PGP_FPR:-}" | tr 'a-f' 'A-F')
[ -z "$FPR" ] || [[ "$FPR" =~ ^[0-9A-F]{40}$ ]] || die "PGP_FPR must be a 40-hex fingerprint"
# The template must keep its one-line source=/sha256sums= form.
[ -z "$FPR" ] || [ "$(cd "$OUT" 2>/dev/null && pwd)" != "$HERE" ] || die "PGP_FPR needs -o OUTDIR (the in-repo PKGBUILD is the unsigned template)"

mkdir -p "$OUT"
TMP=$(mktemp)
trap 'rm -f "$TMP"' EXIT
sed -E -e "s/^pkgver=.*/pkgver=$PKGVER/" -e "s/^pkgrel=.*/pkgrel=$PKGREL/" \
       -e "s/^_tag=.*/_tag=$TAG/" -e "s/^sha256sums=.*/sha256sums=('$SHA')/" \
       "$TEMPLATE" > "$TMP"
for want in "pkgver=$PKGVER" "pkgrel=$PKGREL" "_tag=$TAG" "sha256sums=('$SHA')"; do
  grep -qxF "$want" "$TMP" || die "template PKGBUILD has no line to set $want"
done
if [ -n "$FPR" ]; then
  # source=("URL") -> source=("URL" "URL.sig"); the signature's checksum is SKIP
  # (it is verified cryptographically), and validpgpkeys follows sha256sums.
  sed -i -E -e 's@^source=\("([^"]+)"\)$@source=("\1"\n        "\1.sig")@' \
            -e "s/^sha256sums=\('$SHA'\)\$/sha256sums=('$SHA'\n            'SKIP')\n# makepkg checks the tarball signature against this key, which must be in your keyring first:\n#   gpg --recv-keys $FPR\n#   (or: curl -fsSL https:\/\/slipstreamviewer.org\/slipstream-release.asc | gpg --import)\nvalidpgpkeys=('$FPR')  # Slipstream release key, see scripts\/packaging\/keys/" "$TMP"
  grep -q '\.sig")$' "$TMP" || die "template PKGBUILD's source= line is not a single quoted URL"
  grep -q "^validpgpkeys=('$FPR')" "$TMP" || die "could not add validpgpkeys"
  KEYFILE="$HERE/../keys/slipstream-release.asc"
  if grep -q -- '-----BEGIN PGP PUBLIC KEY BLOCK-----' "$KEYFILE" 2>/dev/null; then
    install -Dm644 "$KEYFILE" "$OUT/keys/pgp/$FPR.asc"
  fi
fi
cp "$TMP" "$OUT/PKGBUILD"
write_srcinfo "$OUT"
echo "gen.sh: $OUT/PKGBUILD + .SRCINFO for $TAG (pkgver $PKGVER-$PKGREL, sha256 $SHA${FPR:+, signed by $FPR})"
