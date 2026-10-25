#!/usr/bin/env bash
# GunMetal-DX crash reporting: build Crashpad (Sentry's CMake fork, pinned) for one platform into
# a small package the viewer build links (indra/newview/CMakeLists.txt GMDX_CRASHPAD_DIR):
#   <out>/include/...   client headers (+ mini_chromium's base/, build/)
#   <out>/lib/...       static libraries
#   <out>/bin/crashpad_handler[.exe]   shipped beside the viewer binary
#
#   scripts/crashpad/build.sh linux   [out]   (default build-crashpad-linux)
#   scripts/crashpad/build.sh windows [out]   (default build-crashpad-windows; clang-cl + xwin, as
#                                              scripts/wincross)
#
# Without the package the viewer builds without crash reporting (nothing else changes).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
PLATFORM=${1:?usage: build.sh linux|windows [out]}
OUT=$(realpath -m "${2:-$ROOT/build-crashpad-$PLATFORM}")
REPO=https://github.com/getsentry/crashpad.git
COMMIT=0bd6ca43a0958a86939a37569b61162f92f9a022   # 2026-10: "treat the <empty> proxy as a direct connection"
SRC=${CRASHPAD_SRC:-$HOME/.cache/gmdx-3p/crashpad-src}
WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT

if [ ! -d "$SRC/.git" ]; then
    git clone --filter=blob:none "$REPO" "$SRC"
fi
git -C "$SRC" fetch --quiet origin "$COMMIT" 2>/dev/null || git -C "$SRC" fetch --quiet origin
git -C "$SRC" checkout --quiet "$COMMIT"
git -C "$SRC" submodule update --init --recursive --quiet

args=(-S "$SRC" -B "$WORK/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCRASHPAD_ZLIB_SYSTEM=OFF
      -DCRASHPAD_ENABLE_INSTALL=OFF -DCRASHPAD_ENABLE_INSTALL_DEV=OFF)
case "$PLATFORM" in
    linux)
        # the viewer is position-independent; the handler uses the system libcurl for its uploads
        args+=(-DCMAKE_POSITION_INDEPENDENT_CODE=ON) ;;
    windows)
        # the viewer's runtime (/MD); MASM through llvm-ml (no ml64 on Linux)
        args+=(-DCMAKE_TOOLCHAIN_FILE="$ROOT/scripts/wincross/toolchain-clang-cl-xwin.cmake"
               -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL
               -DCMAKE_ASM_MASM_COMPILER=llvm-ml -DCMAKE_ASM_MASM_FLAGS=-m64) ;;
    *) echo "unknown platform $PLATFORM (linux|windows)" >&2; exit 2 ;;
esac
cmake "${args[@]}" >/dev/null
ninja -C "$WORK/build" ${NINJA_JOBS:+-j"$NINJA_JOBS"} crashpad_client crashpad_handler

rm -rf "$OUT"; mkdir -p "$OUT/include" "$OUT/lib" "$OUT/bin"
# headers: the client API and what it includes
( cd "$SRC" && find client util base compat package.h build -name '*.h' 2>/dev/null | cpio -pdm --quiet "$OUT/include" ) || true
( cd "$SRC/third_party/mini_chromium/mini_chromium" && find base build -name '*.h' | cpio -pdm --quiet "$OUT/include" )
[ -d "$SRC/compat" ] && ( cd "$SRC" && find compat -name '*.h' | cpio -pdm --quiet "$OUT/include" )
cp -a "$SRC/third_party/mpack" "$OUT/include/third_party_mpack" 2>/dev/null || true
if [ "$PLATFORM" = linux ]; then
    for l in client/libcrashpad_client.a util/libcrashpad_util.a third_party/mini_chromium/libmini_chromium.a \
             third_party/mpack/libcrashpad_mpack.a compat/libcrashpad_compat.a; do
        cp "$WORK/build/$l" "$OUT/lib/"
    done
    cp "$WORK/build/handler/crashpad_handler" "$OUT/bin/"
    strip "$OUT/bin/crashpad_handler"
else
    for l in client/crashpad_client.lib util/crashpad_util.lib third_party/mini_chromium/mini_chromium.lib \
             third_party/mpack/crashpad_mpack.lib compat/crashpad_compat.lib; do
        cp "$WORK/build/$l" "$OUT/lib/"
    done
    cp "$WORK/build/handler/crashpad_handler.exe" "$OUT/bin/"
fi
echo "$COMMIT" > "$OUT/COMMIT"
echo "crashpad ($PLATFORM) -> $OUT"
ls -la "$OUT/lib" "$OUT/bin"
