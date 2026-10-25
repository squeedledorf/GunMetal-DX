#!/usr/bin/env bash
# One-command build + install of the optional DLSS shim for Slipstream (Linux, x86_64).
# SPDX-License-Identifier: MIT -- see LICENSE.
#
#   contrib/ngxshim/build.sh [--flatpak] /path/to/DLSS [OUTPUT_DIR]
#
# Runs ONLY on your machine, against YOUR OWN clone of https://github.com/NVIDIA/DLSS (you
# accept NVIDIA's licence by downloading it). It is never run by CI and never packaged into a
# viewer build: nothing from the SDK may become part of the (LGPL) viewer. It:
#   1. checks the prerequisites (cmake, a C++ compiler, the Vulkan headers, the SDK files);
#   2. builds the shim (Release) in a temporary folder;
#   3. copies the shim and your SDK's runtime libraries (lib/Linux_x86_64/rel) into OUTPUT_DIR
#      (default: the "dlss" folder in the viewer's data dir), each via temp file + rename, so a
#      running viewer that has the old files mapped is never modified in place;
#   4. prints the RenderVKDlssShimPath value (only needed for a non-default OUTPUT_DIR).
set -euo pipefail

die()  { echo "error: $*" >&2; exit 1; }
note() { echo "==> $*"; }

usage() {
    sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'
    echo
    echo "  --flatpak   install into the Flatpak viewer's data folder (~/.var/app/org.slipstreamviewer.Slipstream/.slipstreamviewer/dlss)"
    exit "${1:-0}"
}

FLATPAK=0
ARGS=()
for a in "$@"; do
    case "$a" in
        -h|--help) usage 0 ;;
        --flatpak) FLATPAK=1 ;;
        -*)        echo "unknown option: $a" >&2; usage 2 ;;
        *)         ARGS+=("$a") ;;
    esac
done
[ "${#ARGS[@]}" -ge 1 ] && [ "${#ARGS[@]}" -le 2 ] || usage 2

SDK="${ARGS[0]}"
SHIM_SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

[ "$(uname -s)" = "Linux" ] || die "this script is for Linux (use build.ps1 on Windows)"
[ "$(uname -m)" = "x86_64" ] || die "only x86_64 Linux is supported (the shim links the SDK's lib/Linux_x86_64)"

# ---- default output dir: the viewer's data dir (same rule as the viewer) ----
FLATPAK_ID=org.slipstreamviewer.Slipstream
FLATPAK_DATA="$HOME/.var/app/$FLATPAK_ID/.slipstreamviewer"
if [ "${#ARGS[@]}" -eq 2 ]; then
    OUT="${ARGS[1]}"
elif [ "$FLATPAK" = 1 ]; then
    OUT="$FLATPAK_DATA/dlss"
else
    OUT="${SLIPSTREAMVIEWER_USER_DIR:-$HOME/.slipstreamviewer}/dlss"
fi
DEFAULT_OUT="${SLIPSTREAMVIEWER_USER_DIR:-$HOME/.slipstreamviewer}/dlss"

# ---- prerequisites (nothing is written until all of these pass) ----
[ -d "$SDK" ] || die "SDK folder not found: $SDK
  Clone it first:  git clone https://github.com/NVIDIA/DLSS"
SDK="$(cd "$SDK" && pwd)"
[ -f "$SDK/include/nvsdk_ngx_vk.h" ] || die "$SDK is not the NVIDIA DLSS SDK (missing include/nvsdk_ngx_vk.h)
  Point this at your clone of https://github.com/NVIDIA/DLSS"
LIBDIR="$SDK/lib/Linux_x86_64"
[ -f "$LIBDIR/libnvsdk_ngx.a" ] || die "NGX loader library missing: $LIBDIR/libnvsdk_ngx.a
  (the SDK clone is incomplete; large files may need git lfs: 'git lfs pull')"

shopt -s nullglob
SR_LIBS=("$LIBDIR"/rel/libnvidia-ngx-dlss.so.*)
RR_LIBS=("$LIBDIR"/rel/libnvidia-ngx-dlssd.so.*)
shopt -u nullglob
[ "${#SR_LIBS[@]}" -gt 0 ] || [ "${#RR_LIBS[@]}" -gt 0 ] || die "no DLSS runtime libraries in $LIBDIR/rel
  (expected libnvidia-ngx-dlss.so.* and libnvidia-ngx-dlssd.so.*; try 'git lfs pull' in the SDK)"
[ "${#SR_LIBS[@]}" -gt 0 ] || echo "warning: no libnvidia-ngx-dlss.so.* (Super Resolution) in the SDK; only Ray Reconstruction will work" >&2
[ "${#RR_LIBS[@]}" -gt 0 ] || echo "warning: no libnvidia-ngx-dlssd.so.* (Ray Reconstruction) in the SDK; only Super Resolution will work" >&2

command -v cmake >/dev/null 2>&1 || die "cmake not found (need 3.16 or newer; e.g. 'pacman -S cmake' / 'apt install cmake')"
CXX_BIN=""
for c in "${CXX:-}" c++ g++ clang++; do
    [ -n "$c" ] && command -v "$c" >/dev/null 2>&1 && { CXX_BIN="$c"; break; }
done
[ -n "$CXX_BIN" ] || die "no C++ compiler found (install g++ or clang++; e.g. 'pacman -S base-devel' / 'apt install g++')"
if ! printf '#include <vulkan/vulkan.h>\nint main(){return 0;}\n' | "$CXX_BIN" -x c++ -fsyntax-only - >/dev/null 2>&1; then
    die "Vulkan headers not found (vulkan/vulkan.h); install 'vulkan-headers' (Arch) or 'libvulkan-dev' (Debian/Ubuntu)"
fi

# ---- build in a temp dir ----
WORK="$(mktemp -d "${TMPDIR:-/tmp}/slipstream-ngxshim.XXXXXX")"
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

GEN=()
command -v ninja >/dev/null 2>&1 && GEN=(-G Ninja)
note "Configuring (Release) with $CXX_BIN"
cmake -S "$SHIM_SRC" -B "$WORK/build" "${GEN[@]}" -DDLSS_SDK_DIR="$SDK" -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_COMPILER="$(command -v "$CXX_BIN")" >"$WORK/configure.log" 2>&1 \
    || { tail -n 30 "$WORK/configure.log" >&2; die "cmake configure failed (log above)"; }
note "Building"
cmake --build "$WORK/build" --parallel >"$WORK/build.log" 2>&1 \
    || { tail -n 40 "$WORK/build.log" >&2; die "build failed (log above)"; }
SHIM="$WORK/build/libslipstream_ngx.so"
[ -f "$SHIM" ] || die "build finished but $SHIM is missing"

# ---- install: temp file in the target folder, then rename ----
mkdir -p "$OUT" || die "cannot create $OUT"
install_file() {   # src dest-name mode
    local src="$1" name="$2" mode="$3" tmp
    tmp="$(mktemp "$OUT/.${name}.XXXXXX")" || die "cannot write to $OUT"
    if ! { cp -f "$src" "$tmp" && chmod "$mode" "$tmp" && mv -f "$tmp" "$OUT/$name"; }; then
        rm -f "$tmp"; die "installing $name into $OUT failed"
    fi
    echo "    $OUT/$name"
}
note "Installing into $OUT"
install_file "$SHIM" libslipstream_ngx.so 755
for f in "${SR_LIBS[@]}" "${RR_LIBS[@]}"; do
    install_file "$f" "$(basename "$f")" 755
done
# Drop older versions of the NVIDIA libraries left by a previous install (an unlink never
# disturbs a running viewer that has them mapped).
for f in "$OUT"/libnvidia-ngx-dlss.so.* "$OUT"/libnvidia-ngx-dlssd.so.*; do
    [ -e "$f" ] || continue
    keep=0
    for g in "${SR_LIBS[@]}" "${RR_LIBS[@]}"; do [ "$(basename "$g")" = "$(basename "$f")" ] && keep=1; done
    [ "$keep" = 1 ] || { rm -f "$f"; echo "    removed stale $(basename "$f")"; }
done

echo
note "Done. Restart the viewer; Slipstream.log will have a 'DLSS:' line."
if [ "$OUT" = "$DEFAULT_OUT" ] || { [ "$FLATPAK" = 1 ] && [ "$OUT" = "$FLATPAK_DATA/dlss" ]; }; then
    echo "The viewer looks in this folder by default: no setting is needed (RenderVKDlssShimPath stays empty)."
else
    echo "Not the default folder. In the viewer, set the debug setting:"
    echo "    RenderVKDlssShimPath = $OUT"
fi

# ---- Flatpak guidance ----
if [ "$FLATPAK" = 0 ] && [ -d "$HOME/.var/app/$FLATPAK_ID" ]; then
    echo
    echo "Flatpak viewer detected ($FLATPAK_ID). The sandbox cannot see $DEFAULT_OUT; it sees"
    echo "    $FLATPAK_DATA/dlss"
    echo "(as ~/.slipstreamviewer/dlss inside the sandbox). To install for it, run:"
    echo "    $0 --flatpak $SDK"
fi
