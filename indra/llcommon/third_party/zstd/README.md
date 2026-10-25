# Zstandard (vendored)

Byte codec behind the cache compression shim (`indra/llcommon/llcachecodec.h`,
cache rewrite epic #715 / #713).

- Upstream: https://github.com/facebook/zstd
- Release: `v1.5.7` (`ZSTD_VERSION_STRING` "1.5.7")
- Source: https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz
  - tarball SHA-256 `eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3`
    (matches the release's `zstd-1.5.7.tar.gz.sha256`)
  - `zstd-1.5.7.tar.gz.sig` verified: good signature from "Zstandard Release
    Signing Key <signing@zstd.net>", fingerprint
    `4EF4 AC63 455F C9F4 545D 9B7D EF8F E995 28B5 2FFD`
- Files:
  - `zstd.c` -- the official single-file amalgamation, generated from the
    release tarball with upstream's own script, unmodified:
    `cd build/single_file_libs && python3 combine.py -r ../../lib -x legacy/zstd_legacy.h -o zstd.c zstd-in.c`
    (what `create_single_file_library.sh` runs)
  - `zstd.h`, `zstd_errors.h` -- from the tarball's `lib/`, unmodified (`zstd.h`
    includes `zstd_errors.h`; the amalgamation carries its own inlined copies)
  - `LICENSE` -- the tarball's `LICENSE` (BSD), unmodified
- SHA-256:
  - `zstd.c`  `208e110a1f052d007242d4eef6ed20a03ab1dc6e13eb2c4ff8d158112120ba8e`
  - `zstd.h`  `9b4bc8245565c98ccfc61c07749928b57e7c0f6fddb0530c4f6aa1971893d88b`
  - `zstd_errors.h` `66a8c3f71d12ea6e797e4f622f31f3f8f81c41b36f48cad4f5de7d8bfb6aac0a`
  - `LICENSE` `7055266497633c9025b777c78eb7235af13922117480ed5c674677adc381c9d8`
- License: zstd is dual-licensed BSD / GPLv2 at our choice; we use BSD. Listed in
  `indra/newview/licenses-*.txt`.

## What the amalgamation bakes in (upstream `zstd-in.c`, left as is)

- `ZSTD_DISABLE_ASM 1`: no assembly (the x86-64 Huffman decode `.S` file is not
  part of the amalgamation). Portable C only, so it builds the same with GCC,
  Clang and clang-cl.
- `ZSTD_LEGACY_SUPPORT 0`: no pre-v0.8 frame formats.
- `ZSTD_MULTITHREAD`: compiles `threading.c` / `pool.c` (pthreads on Linux,
  Win32 threads on Windows). Unused by us (we never set `ZSTD_c_nbWorkers`);
  harmless.
- dictBuilder (`cover.c`, `divsufsort.c`, `fastcover.c`, `zdict.c`): unused for
  now; may be used later for trained object-cache dictionaries.
- xxHash is compiled with `XXH_NAMESPACE ZSTD_` + `XXH_PRIVATE_API`, so it
  cannot collide with llcommon's own xxHash (`hbxxh.cpp`).

## Build

Compiled as C++ (zstd's sources build as C++; upstream tests that with
`make cxxtest`) in exactly one translation unit, `indra/llcommon/llzstd.cpp`,
which includes `zstd.c`. That file is built without the precompiled header and
with warnings off (CMakeLists.txt). Other code includes
`third_party/zstd/zstd.h` only, and outside llcommon should go through
`llcachecodec.h` rather than calling zstd directly.

## Updating

Download a newer release tarball, verify its `.sha256` and `.sig`, regenerate
`zstd.c` with the command above, copy `lib/zstd.h` and `LICENSE`, and update
this file (tag, hashes) and the version line in `indra/newview/licenses-*.txt`.
The shim uses only the stable API (no `ZSTD_STATIC_LINKING_ONLY`).
