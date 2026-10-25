# ufbx (vendored)

FBX import for mesh upload (`indra/llprimitive/llfbxloader.cpp`).

- Upstream: https://github.com/ufbx/ufbx
- Release: `v0.23.1` (`UFBX_HEADER_VERSION` 0.23.1), commit
  `26a482ae66871d7de36eb722aa060bce95bce274`
- Files: `ufbx.c`, `ufbx.h`, `LICENSE`, unmodified
- SHA-256:
  - `ufbx.c`  `4a956c26a708e40d82ecb0aeaee54da709d5ba7c144c9642174970059bda7e1d`
  - `ufbx.h`  `f787be529af577efc04f3e3e735844a08556718dbb613d1211c2b313d72895dc`
  - `LICENSE` `0dd48ebadf52273c736256325c8f078c03c8bb4facee22a4122de0ad3f615391`
- License: MIT or public domain (Unlicense), at our choice; we use MIT. Listed in
  `indra/newview/licenses-*.txt`.

## Build

Compiled as C++ (ufbx's C also compiles as C++, per upstream) in exactly one
translation unit, `indra/llprimitive/llufbx.cpp`, which includes `ufbx.c`.
That file is built without the precompiled header and with warnings off; on
MSVC/clang-cl it also overrides the global `/fp:fast` with `/fp:precise`.
Other code includes `third_party/ufbx/ufbx.h` only.

## Updating

Replace `ufbx.c` / `ufbx.h` / `LICENSE` from a newer release tag, update this
file (tag, commit, SHA-256), and re-check the API used in `llfbxloader.cpp`
(load options, skin cluster matrices, mesh parts, material maps).
