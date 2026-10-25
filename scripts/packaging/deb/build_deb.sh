#!/bin/bash
# Build the slipstream-viewer .deb (Debian 13+, Ubuntu 24.04+, Mint 22+) from an
# official Linux release tarball (#278).
#
# usage: build_deb.sh TARBALL TAG OUTDIR
#          Write OUTDIR/Slipstream_<label>_amd64.deb, where <label> is TAG
#          without the leading v (v1.0.0-beta.5 -> 1.0.0-beta.5). TAG must be
#          vX.Y.Z or vX.Y.Z-beta.N. The tarball isn't required to match TAG, so
#          a v1.0.0 .deb can be built from a beta tarball to test upgrades.
#        build_deb.sh --version TAG     print the Debian version for TAG
#        build_deb.sh --selftest        check the version scheme with dpkg
#        build_deb.sh --shlibs TARBALL  print fresh dpkg-shlibdeps output for
#                                       TARBALL (use it to refresh depends.shlibs)
#
# Version scheme: vX.Y.Z-beta.N -> X.Y.Z~betaN, vX.Y.Z -> X.Y.Z. '~' sorts
# before anything, even the end of the string, so every final release sorts
# above its betas and apt upgrades beta -> final. DEBREV=N appends a Debian
# revision (-N) to fix the packaging without a new release.
#
# Dependencies (see README.md):
#   * dpkg-shlibdeps over every 64-bit ELF in the tree computes the libraries
#     the binaries link against. The tree is staged under debian/<pkg>/ so the
#     libraries the package bundles in /opt/slipstream/lib are recognised and
#     left out. dpkg-shlibdeps needs every linked library installed; when it is
#     missing or fails, the committed snapshot depends.shlibs (made the same way
#     in a clean ubuntu:24.04 with the runtime libraries installed) is used.
#   * depends.extra adds what dpkg-shlibdeps can't see: libraries the bundled
#     SDL3 / OpenAL / WebRTC / CEF load with dlopen, plus Recommends/Suggests.
#
# env: DEBREV       Debian revision (default: none)
#      SHLIBDEPS    auto (default: fresh, else the snapshot) | fresh (fail
#                   without a fresh result) | snapshot (always the snapshot)
#      LINTIAN=0    skip the lintian report (it never fails the build)
#      SOURCE_DATE_EPOCH  changelog date (default: the tarball's mtime)
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
PKG=slipstream-viewer
MAINTAINER='Slipstream Viewer <noreply@slipstreamviewer.org>'
HOMEPAGE=https://slipstreamviewer.org

die()  { echo "build_deb.sh: $*" >&2; exit 1; }
warn() { echo "build_deb.sh: WARNING: $*" >&2; }

debver_of() {
  if [[ "$1" =~ ^v([0-9]+\.[0-9]+\.[0-9]+)(-beta\.([0-9]+))?$ ]]; then
    echo "${BASH_REMATCH[1]}${BASH_REMATCH[3]:+~beta${BASH_REMATCH[3]}}${DEBREV:+-$DEBREV}"
  else
    die "tag '$1' is not vX.Y.Z or vX.Y.Z-beta.N"
  fi
}

# 64-bit x86-64 ELF files under DIR (relative paths). Older tarballs also carry
# legacy 32-bit Vivox files (bin/SLVoice, ...) that would make dpkg-shlibdeps
# look for i386 libraries.
elf64_files() (
  cd "$1"
  find . -type f ! -name '*.debug' -print0 | while IFS= read -r -d '' f; do
    # e_ident: "\177ELF", EI_CLASS=2 (64-bit); e_machine (offset 18) = 0x3e (x86-64)
    [ "$(head -c 5 "$f" | od -An -tx1 | tr -d ' \n')" = 7f454c4602 ] || continue
    [ "$(od -An -j18 -N2 -tx1 "$f" | tr -d ' \n')" = 3e00 ] || continue
    echo "${f#./}"
  done | sort
)

# Run dpkg-shlibdeps over the tree staged at WORK/debian/$PKG/opt/slipstream.
# Prints "shlibs:Depends=...".
run_shlibdeps() (
  local work=$1 opt="debian/$PKG/opt/slipstream"
  command -v dpkg-shlibdeps >/dev/null 2>&1 || { warn "dpkg-shlibdeps not installed (apt install dpkg-dev)"; return 1; }
  cd "$work"
  mkdir -p debian "debian/$PKG/DEBIAN"   # DEBIAN/ lets it resolve $ORIGIN RUNPATHs
  printf 'Source: %s\n\nPackage: %s\nArchitecture: amd64\n' "$PKG" "$PKG" > debian/control
  local all=() dep=() f
  mapfile -t all < <(elf64_files "$opt")
  for f in "${all[@]}"; do
    case "$f" in
      # VLC media plugin: links libvlc.so.5, which is optional (Suggests:
      # libvlc5 in depends.extra). Scanning it would make libvlc a hard
      # dependency, or fail where it isn't installed.
      bin/llplugin/libmedia_plugin_libvlc.so) ;;
      # Test plugin, never loaded by a release build.
      bin/llplugin/libmedia_plugin_example.so) ;;
      # Everything else, including the GStreamer plugin (it links only
      # libstdc++/libc and loads GStreamer with dlopen: Recommends in
      # depends.extra).
      *) dep+=("$opt/$f") ;;
    esac
  done
  [ ${#dep[@]} -gt 0 ] || { warn "no 64-bit ELF files found under $opt"; return 1; }
  # -l: the bundled libraries live here (and in bin/llplugin for the CEF
  # plugin's $ORIGIN RUNPATH); being under debian/$PKG they are recognised as
  # part of this package and produce no dependency.
  # --ignore-missing-info: the bundled libraries have no shlibs/symbols file.
  dpkg-shlibdeps -O --ignore-missing-info -l/opt/slipstream/lib -l/opt/slipstream/bin/llplugin -dDepends "${dep[@]}"
)

# Comma-separated dependency list helpers. A relation is "name [(op ver)]",
# alternatives are joined by " | ".
split_list() { tr ',' '\n' <<<"$1" | sed -E 's/^[[:space:]]+//; s/[[:space:]]+$//' | sed '/^$/d'; }
rel_names()  { tr '|' '\n' <<<"$1" | sed -E 's/^[[:space:]]+//; s/[[:space:](].*//' | sed '/^$/d'; }

# On Ubuntu 24.04 / Debian 13 several libraries were renamed with a t64 suffix
# (64-bit time_t). Offer the old name as an alternative: harmless where the
# t64 one exists, and it keeps the relation satisfiable on a derivative that
# kept the old name.
add_t64_alternatives() {
  local rel=$1 out=() alt name rest
  IFS='|' read -ra alts <<<"$rel"
  for alt in "${alts[@]}"; do
    alt=$(sed -E 's/^[[:space:]]+//; s/[[:space:]]+$//' <<<"$alt")
    out+=("$alt")
    name=${alt%% *}; rest=${alt#"$name"}
    if [[ "$name" == *t64 ]] && ! grep -qxF "${name%t64}" <(rel_names "$rel"); then
      out+=("${name%t64}$rest")
    fi
  done
  local IFS='|'; sed 's/|/ | /g' <<<"${out[*]}"
}

# merge FIELD COMPUTED EXTRA_FILE [EXCLUDE_NAMES...]: the computed relations plus
# the depends.extra ones for FIELD whose packages aren't already named (by the
# computed list or in EXCLUDE_NAMES, i.e. a stronger field).
merge_field() {
  local field=$1 computed=$2 extra=$3; shift 3
  local -A seen=()
  local n rel out=()
  for n in "$@"; do seen[$n]=1; done
  while IFS= read -r rel; do
    local dup=1
    while IFS= read -r n; do [ -n "${seen[$n]:-}" ] || dup=0; done < <(rel_names "$rel")
    [ $dup = 1 ] && continue
    out+=("$(add_t64_alternatives "$rel")")
    while IFS= read -r n; do seen[$n]=1; done < <(rel_names "$rel")
  done < <(split_list "$computed"; split_list "$(sed -nE "s/^$field:[[:space:]]*//p" "$extra" | paste -sd, -)")
  local IFS=,; sed 's/,/, /g' <<<"${out[*]}"
}

case "${1:-}" in
  --version) debver_of "${2:?usage: build_deb.sh --version TAG}"; exit 0 ;;
  --selftest)
    command -v dpkg >/dev/null || die "dpkg not installed"
    fail=0
    # pairs that must sort strictly ascending (the same list as aur/gen.sh)
    while read -r lo hi; do
      a=$(debver_of "$lo"); b=$(debver_of "$hi")
      if dpkg --compare-versions "$a" lt "$b"; then r=ok; else r=FAIL; fail=1; fi
      printf '%-32s %-14s < %-14s %s\n' "$lo/$hi" "$a" "$b" "$r"
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
esac

SHLIBS_ONLY=0
if [ "${1:-}" = --shlibs ]; then SHLIBS_ONLY=1; TARBALL=${2:?usage: build_deb.sh --shlibs TARBALL}; TAG=v0.0.0; OUT=.
else
  TARBALL=${1:?usage: build_deb.sh TARBALL TAG OUTDIR}
  TAG=${2:?usage: build_deb.sh TARBALL TAG OUTDIR}
  OUT=${3:?usage: build_deb.sh TARBALL TAG OUTDIR}
fi
[ -f "$TARBALL" ] || die "no such tarball: $TARBALL"
VERSION=$(debver_of "$TAG")
LABEL=${TAG#v}
DEB_NAME="Slipstream_${LABEL}_amd64.deb"

WORK=$(mktemp -d "${TMPDIR:-/tmp}/slipstream-deb.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
ROOT="$WORK/debian/$PKG"
OPT="$ROOT/opt/slipstream"

# --- the tree --------------------------------------------------------------
umask 022
mkdir -p "$OPT"
tar -xJf "$TARBALL" -C "$OPT" --strip-components=1 --no-same-owner --no-same-permissions
[ -x "$OPT/slipstream" ] && [ -f "$OPT/build_data.json" ] || die "$TARBALL doesn't look like a Slipstream Linux tarball"

if [ $SHLIBS_ONLY = 1 ]; then run_shlibdeps "$WORK"; exit; fi

# Marks a package install: the wrapper skips the per-user desktop entry, the
# per-user secondlife:// registration (installed system-wide below) and the
# #245 missing-library check (apt guarantees the libraries).
printf 'deb\n' > "$OPT/.packaged-by"
# Wrappers older than the marker run the registration scripts unconditionally;
# guard them. A no-op on wrappers that already check the marker.
sed -i -E 's@^(\./etc/(register_secondlifeprotocol|refresh_desktop_app_entry)\.sh)$@[ -f .packaged-by ] || \1@' \
  "$OPT/slipstream"
# CEF's sandbox helper stays as shipped: 0755, NOT setuid root (README.md).
[ -f "$OPT/bin/chrome-sandbox" ] && chmod 0755 "$OPT/bin/chrome-sandbox"

# Launcher. Not a symlink: the wrapper finds build_data.json from its own $0.
install -d "$ROOT/usr/bin"
cat > "$ROOT/usr/bin/slipstream" <<'EOF'
#!/bin/sh
# slipstream-viewer launcher: apt provides the system libraries, so the
# wrapper's missing-library check is skipped. User data: ~/.slipstreamviewer
export SLIPSTREAM_SKIP_MEDIA_CHECK=1
exec /opt/slipstream/slipstream "$@"
EOF
chmod 0755 "$ROOT/usr/bin/slipstream"

# The wrapper sets the window class to the channel name ("Slipstream Beta").
CHANNEL=$(sed -n 's/.*"Channel"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$OPT/build_data.json")
install -d "$ROOT/usr/share/applications"
cat > "$ROOT/usr/share/applications/slipstream.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Slipstream Viewer
GenericName=Second Life Viewer
Comment=Vulkan viewer for Second Life
Exec=slipstream %u
Icon=slipstream
Terminal=false
Categories=Game;Simulation;
MimeType=x-scheme-handler/secondlife;x-scheme-handler/x-grid-location-info;
StartupNotify=true
StartupWMClass=${CHANNEL:-Slipstream}
EOF

# Icons: the hicolor set when the tarball ships one, else the 256 px icon.
if [ -d "$OPT/share/icons/hicolor" ]; then
  install -d "$ROOT/usr/share/icons"
  cp -a "$OPT/share/icons/hicolor" "$ROOT/usr/share/icons/"
else
  install -Dm644 "$OPT/slipstream_icon.png" "$ROOT/usr/share/icons/hicolor/256x256/apps/slipstream.png"
fi

# Documentation Debian policy requires: copyright and changelog.
DOC="$ROOT/usr/share/doc/$PKG"
install -d "$DOC"
cat > "$DOC/copyright" <<'EOF'
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: Slipstream Viewer
Upstream-Contact: https://git.slipstreamviewer.org/sky_line/Slipstream/issues
Source: https://git.slipstreamviewer.org/sky_line/Slipstream

Files: *
Copyright: Copyright 2001-2026 Linden Research, Inc.
           Copyright 2013-2026 The Alchemy Viewer Project
           Copyright 2025-2026 The Slipstream Viewer contributors
License: LGPL-2.1
Comment: The package repackages the official Slipstream Linux release tarball.
 Bundled third-party components (CEF, SDL3, OpenAL, WebRTC, fonts, ...) carry
 their own licenses, listed in /opt/slipstream/licenses.txt and next to the
 fonts in /opt/slipstream/fonts.

License: LGPL-2.1
 This library is free software; you can redistribute it and/or modify it
 under the terms of the GNU Lesser General Public License as published by the
 Free Software Foundation; version 2.1 of the License only.
 .
 This library is distributed in the hope that it will be useful, but WITHOUT
 ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for more
 details.
 .
 On Debian systems, the complete text of the GNU Lesser General Public License
 version 2.1 can be found in "/usr/share/common-licenses/LGPL-2.1".
EOF
DATE=$(LC_ALL=C date -u -R -d "@${SOURCE_DATE_EPOCH:-$(stat -c %Y "$TARBALL")}")
# A version without a Debian revision is a native package: changelog.gz.
CHANGELOG=changelog.gz; [ -z "${DEBREV:-}" ] || CHANGELOG=changelog.Debian.gz
printf '%s (%s) unstable; urgency=medium\n\n  * Slipstream %s, repackaged from the official Linux release tarball.\n  * Release notes:\n    https://git.slipstreamviewer.org/sky_line/Slipstream/releases/tag/%s\n\n -- %s  %s\n' \
  "$PKG" "$VERSION" "$LABEL" "$TAG" "$MAINTAINER" "$DATE" | gzip -9n > "$DOC/$CHANGELOG"

# Expected lintian findings for a repackaged prebuilt binary release.
install -Dm644 "$HERE/lintian-overrides" "$ROOT/usr/share/lintian/overrides/$PKG"

# --- dependencies ----------------------------------------------------------
SHLIBS=""
case "${SHLIBDEPS:-auto}" in
  snapshot) ;;
  auto|fresh)
    if SHLIBS=$(run_shlibdeps "$WORK"); then
      echo "build_deb.sh: dependencies computed with dpkg-shlibdeps"
    else
      SHLIBS=""
      [ "${SHLIBDEPS:-auto}" = fresh ] && die "dpkg-shlibdeps failed and SHLIBDEPS=fresh"
    fi ;;
  *) die "SHLIBDEPS must be auto, fresh or snapshot" ;;
esac
rm -f "$WORK/debian/control"
if [ -z "$SHLIBS" ]; then
  warn "using the committed snapshot $HERE/depends.shlibs (install dpkg-dev and the runtime libraries to compute them fresh; see README.md)"
  SHLIBS=$(grep -v '^#' "$HERE/depends.shlibs")
fi
echo "$SHLIBS" | sed 's/^/  /'
S_DEP=$(sed -nE 's/^shlibs:Depends=//p' <<<"$SHLIBS")
S_REC=$(sed -nE 's/^shlibs:Recommends=//p' <<<"$SHLIBS")
S_SUG=$(sed -nE 's/^shlibs:Suggests=//p' <<<"$SHLIBS")
[ -n "$S_DEP" ] || die "no shlibs:Depends computed"

# The bundled libraries must not appear: that would mean dpkg-shlibdeps
# resolved them to a system copy instead of the one in the tree.
# (libvulkan1 is bundled too, but depends.extra names it on purpose.)
S_NAMES=$(split_list "$S_DEP" | while IFS= read -r r; do rel_names "$r"; done)
for bundled in libopenal1 libopenjp2-7 libsdl3-0 libalut0; do
  grep -qxF "$bundled" <<<"$S_NAMES" && warn "dpkg-shlibdeps named the bundled library package $bundled"
done

EXTRA="$HERE/depends.extra"
DEPENDS=$(merge_field Depends "$S_DEP" "$EXTRA")
mapfile -t DEP_NAMES < <(split_list "$DEPENDS" | while IFS= read -r r; do rel_names "$r"; done)
RECOMMENDS=$(merge_field Recommends "$S_REC" "$EXTRA" "${DEP_NAMES[@]}")
mapfile -t REC_NAMES < <(split_list "$RECOMMENDS" | while IFS= read -r r; do rel_names "$r"; done)
SUGGESTS=$(merge_field Suggests "$S_SUG" "$EXTRA" "${DEP_NAMES[@]}" "${REC_NAMES[@]}")

# --- control ----------------------------------------------------------------
INSTALLED_SIZE=$(du -sk --apparent-size "$ROOT" | cut -f1)
install -d "$ROOT/DEBIAN"
{
  echo "Package: $PKG"
  echo "Version: $VERSION"
  echo "Architecture: amd64"
  echo "Maintainer: $MAINTAINER"
  echo "Installed-Size: $INSTALLED_SIZE"
  echo "Depends: $DEPENDS"
  [ -n "$RECOMMENDS" ] && echo "Recommends: $RECOMMENDS"
  [ -n "$SUGGESTS" ] && echo "Suggests: $SUGGESTS"
  echo "Provides: slipstream"
  echo "Section: net"
  echo "Priority: optional"
  echo "Homepage: $HOMEPAGE"
  cat <<'EOF'
Description: Vulkan viewer for Second Life
 Slipstream is a third-party viewer for Second Life with a GPU-driven Vulkan
 renderer. It is not provided or supported by Linden Lab, the makers of
 Second Life.
 .
 This package installs the official Linux release to /opt/slipstream and adds
 the slipstream command, a menu entry and the secondlife:// link handler.
 Settings, chat logs and caches live in ~/.slipstreamviewer.
EOF
} > "$ROOT/DEBIAN/control"
# md5sums, as dpkg-deb leaves them to the packaging helper.
( cd "$ROOT" && find . -path ./DEBIAN -prune -o -type f -printf '%P\0' | LC_ALL=C sort -z | xargs -0 md5sum ) > "$ROOT/DEBIAN/md5sums"
chmod 0644 "$ROOT/DEBIAN/control" "$ROOT/DEBIAN/md5sums"
cat "$ROOT/DEBIAN/control"

# --- build ------------------------------------------------------------------
command -v dpkg-deb >/dev/null 2>&1 || die "dpkg-deb not installed (apt install dpkg)"
mkdir -p "$OUT"
dpkg-deb --root-owner-group -Zxz --build "$ROOT" "$OUT/$DEB_NAME"
echo "build_deb.sh: $OUT/$DEB_NAME ($VERSION)"

if [ "${LINTIAN:-1}" != 0 ]; then
  if command -v lintian >/dev/null 2>&1; then
    echo "--- lintian (informational, never fails the build) ---"
    # The package's lintian-overrides hide the expected findings for a
    # repackaged prebuilt release (files in /opt, bundled libraries, ...).
    lintian --tag-display-limit 0 "$OUT/$DEB_NAME" || true
  else
    warn "lintian not installed: skipping the lintian report (apt install lintian)"
  fi
fi
