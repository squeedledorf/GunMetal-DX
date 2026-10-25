#!/usr/bin/env bash
# Offline round trip of the GPU block encoder (clean_texenc.comp) on the local Vulkan
# device, checked against the bcdec reference decoder (#840). Not part of the viewer
# build; needs glslc, g++ and the Vulkan loader + headers.
#   scripts/texenc_test/run.sh            # exit 0 = every masked encode kept its cutoff
# bcdec (Sergii "iOrange" Kudlai, MIT / Unlicense) is fetched at a pinned commit into the
# cache dir below, never committed.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CACHE="${TEXENC_TEST_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/slipstream-texenc-test}"
BCDEC_REV=80859ed3b7afb1c527a2a99d70c61457bea72d0c
mkdir -p "$CACHE"
if [ ! -s "$CACHE/bcdec-$BCDEC_REV/bcdec.h" ]; then
    mkdir -p "$CACHE/bcdec-$BCDEC_REV"
    curl -fsSL -o "$CACHE/bcdec-$BCDEC_REV/bcdec.h" \
        "https://raw.githubusercontent.com/iOrange/bcdec/$BCDEC_REV/bcdec.h"
fi
glslc --target-env=vulkan1.3 -O -fshader-stage=comp \
    "$ROOT/indra/vkclean/shaders/clean_texenc.comp" -o "$CACHE/clean_texenc.spv"
g++ -O2 -std=c++17 -I"$CACHE/bcdec-$BCDEC_REV" "$HERE/texenc_roundtrip.cpp" \
    -o "$CACHE/texenc_roundtrip" -lvulkan
"$CACHE/texenc_roundtrip" "$CACHE/clean_texenc.spv"
