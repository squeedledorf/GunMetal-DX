# Wuffs (vendored)

GIF decoding for chat inline image previews (#265).

- Upstream: https://github.com/google/wuffs
- File: `release/c/wuffs-v0.3.c` (the latest *stable* release line; v0.4 is
  still alpha), unmodified
- Version: `0.3.5+3404.20260711` (`WUFFS_VERSION_STRING`)
- Fetched from commit `b2e6da302476d47e22fc3edea618f5867bac3f5e`
  (raw.githubusercontent.com/google/wuffs/<commit>/release/c/wuffs-v0.3.c)
- SHA-256: `82c6741dd751eb962a287882991836498526eb02dbe0c7f19adc01810b8bac96`
- License: the file header states Apache-2.0; the upstream repository's
  `LICENSE` at that commit (copied here) offers MIT or Apache-2.0. Listed in
  `indra/newview/licenses-*.txt`.

## Build

Compiled as C++ (Wuffs' C is also valid C++; the viewer compiles no `.c` files
and its global compile options are C++-oriented) in exactly one translation
unit, `indra/newview/llwuffs.cpp`, which defines `WUFFS_IMPLEMENTATION` and
enables only the modules the GIF decoder needs:

    WUFFS_CONFIG__MODULES
    WUFFS_CONFIG__MODULE__BASE
    WUFFS_CONFIG__MODULE__GIF
    WUFFS_CONFIG__MODULE__LZW   (the GIF decoder embeds the LZW decoder)

Other users include `llwuffs.h`, which sets the same module defines and
includes the file as a header. `llwuffs.cpp` is built without the viewer's
precompiled header and with warnings off (generated third-party code).

## Updating

Replace `wuffs-v0.3.c` with a newer `release/c/wuffs-vX.Y.c`, update this file
(version, commit, SHA-256), rename the include in `llwuffs.h`, and re-check the
GIF API used in `llchatimagegif.cpp`.
