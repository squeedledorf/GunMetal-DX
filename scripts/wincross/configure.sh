#!/bin/bash
# Configure the Windows cross build (run from the repo root): the `windows-cross` preset
# in indra/CMakePresets.json (toolchain, options, Vulkan import lib) plus the per-host
# bits. CMake fetches the windows64 3p packages itself (indra/cmake/Prebuilt.cmake).
# PCH stays OFF (preset): clang-cl does not carry the MS-ABI "copy constructor for exception object"
# table through a precompiled header, so ThrowInfo/CatchableType records built in PCH TUs
# lose their copy ctor and exceptions get bitwise-copied (std::make_exception_ptr on a
# boost::fibers broken_promise then frees the what() string twice -> 0xc0000374 at login).
set -euo pipefail
source scripts/wincross/env.sh
cmake -S indra --preset windows-cross \
  -Dredist_path=${WINCROSS_CRT:-$HOME/.cache/wincross/crt} -DPython3_EXECUTABLE=$PWD/.venv/bin/python \
  ${SL_3P_CACHE:+"-DSL_3P_CACHE=$SL_3P_CACHE"} \
  "-DUPDATE_PUBKEY_HEX=${UPDATE_PUBKEY_HEX:-ac6f0c3ff4f77bcb016c84b6e889a296f41034f4c2676514b608c712ea1e3ba0}" \
  "-DVIEWER_CHANNEL=${VIEWER_CHANNEL:-GunMetal-DX Test}" "-DVIEWER_BUILD_ID=${BUILD_ID:-0}" "$@"
