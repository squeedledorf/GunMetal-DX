#!/bin/bash
# Write the Flatpak repo description files from the templates in repo/ (#286):
#   slipstream.flatpakrepo     adds the "slipstream" remote (repo URL + key)
#   slipstream.flatpakref      installs org.slipstreamviewer.Slipstream//stable
#                              (//beta while the repo has no stable release yet,
#                              so the link on the website always works; those
#                              installs keep getting every final via beta)
#   slipstream-beta.flatpakref installs org.slipstreamviewer.Slipstream//beta
# They are served from the root of the Flatpak host, next to repo/ (INFRA.md).
#
# usage: repo_files.sh PUBKEY OUTDIR
#   PUBKEY  the project signing key's PUBLIC key, binary (gpg --export) or
#           armored; embedded base64 as GPGKey=
# env: REPO_URL  default https://flatpak.slipstreamviewer.org/repo/
#      SITE_URL  default: REPO_URL without the trailing repo/ (where the icon
#                slipstream.png lives)
#      STABLE_BRANCH  branch of slipstream.flatpakref (default stable;
#                build_flatpak.sh passes beta until a stable ref exists)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
PUBKEY=${1:?usage: repo_files.sh PUBKEY OUTDIR}
OUT=${2:?usage: repo_files.sh PUBKEY OUTDIR}
REPO_URL=${REPO_URL:-https://flatpak.slipstreamviewer.org/repo/}
SITE_URL=${SITE_URL:-${REPO_URL%repo/}}
[ -s "$PUBKEY" ] || { echo "repo_files.sh: no public key at $PUBKEY" >&2; exit 1; }
mkdir -p "$OUT"

# GPGKey= wants the binary key, base64 on one line. Dearmor when needed.
if grep -q -- '-----BEGIN PGP PUBLIC KEY BLOCK-----' "$PUBKEY" 2>/dev/null; then
  KEY=$(gpg --batch --dearmor < "$PUBKEY" | base64 -w0)
else
  KEY=$(base64 -w0 < "$PUBKEY")
fi

fill() { # template out branch title_suffix
  sed -e "s|@REPO_URL@|$REPO_URL|g" -e "s|@SITE_URL@|$SITE_URL|g" \
      -e "s|@BRANCH@|$3|g" -e "s|@TITLE_SUFFIX@|$4|g" -e "s|@GPGKEY@|$KEY|g" "$1" > "$2"
  ! grep -q '@[A-Z_]*@' "$2" || { echo "repo_files.sh: unfilled token in $2" >&2; exit 1; }
}
fill "$HERE/repo/slipstream.flatpakrepo.in" "$OUT/slipstream.flatpakrepo" "" ""
fill "$HERE/repo/slipstream.flatpakref.in"  "$OUT/slipstream.flatpakref" "${STABLE_BRANCH:-stable}" ""
fill "$HERE/repo/slipstream.flatpakref.in"  "$OUT/slipstream-beta.flatpakref" beta " (Beta)"
echo "repo_files.sh: wrote slipstream.flatpakrepo, slipstream.flatpakref, slipstream-beta.flatpakref in $OUT"
