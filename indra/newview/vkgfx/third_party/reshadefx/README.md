# reshadefx (vendored) -- ReShade FX effect compiler

Slipstream's built-in effects stack (#542) loads unmodified ReShade FX (`.fx` / `.fxh`)
effects. This directory vendors the **effect compiler** of ReShade -- and nothing else
(no injector, no runtime, no add-on API, no D3D / GL back ends).

| What | Upstream | Pinned at | Licence |
|---|---|---|---|
| `src/effect_*` (lexer, preprocessor, parser, symbol table + intrinsics, expression, codegen interface, SPIR-V codegen, module / token types) | https://github.com/crosire/reshade `source/` | release tag **v6.8.0**, commit `18deaa52de0c425a78b329e9cb3c497281cd00ec` | BSD-3-Clause (`LICENSE.md`; SPDX header in every file) |
| `include/spirv/spirv.hpp`, `include/spirv/GLSL.std.450.h` | https://github.com/KhronosGroup/SPIRV-Headers `include/spirv/unified1/` | commit `7845730cab6ebbdeb621e7349b7dc1a59c3377be` (ReShade v6.8.0's `deps/spirv` submodule) | MIT-style Khronos licence (`include/spirv/LICENSE.SPIRV-Headers`, and each file's header) |

Not vendored: `effect_codegen_hlsl.cpp`, `effect_codegen_glsl.cpp`,
`effect_codegen_dxbc.cpp`, `effect_codegen_dxil.cpp` (Slipstream only targets SPIR-V).

## Local modifications

- `src/effect_preprocessor.cpp`: `#include <share.h>` on `_WIN32` (`SH_DENYWR` for
  `_wfsopen`; explicit for clang-cl / windows-cross). Nothing else is changed.

## Build

`CMakeLists.txt` builds the static library `slreshadefx` (C++17, warnings off, no
viewer precompiled header). The viewer's compile front end is
`indra/newview/vkgfx/llfxcompile.{h,cpp}` (Slipstream code); the effects runtime is
`indra/newview/vkgfx/llfxruntimevk.{h,cpp}`.

`tools/slfxc.cpp` (Slipstream code) is a developer smoke test, `ninja slfxc`:
compiles `.fx` files through the same front end, dumps the module, writes one `.spv`
per entry point, runs `spirv-val --target-env vulkan1.3` and (with `--vk`) creates the
shader modules on a real device with the validation layer on.

## Updating

1. Pick a released ReShade tag; copy the files listed above from `source/` and the two
   SPIR-V headers from the SPIRV-Headers commit that tag's `deps/spirv` points at.
2. Re-apply the local modifications, update the table above and `NOTICE`.
3. Re-run the P0 sweep (`slfxc --vk` over SweetFX, prod80 and reshade-shaders) and
   compare with the numbers recorded on #543.

## Related shipped files

- `indra/newview/app_settings/fx/Shaders/ReShade.fxh` -- upstream reshade-shaders
  (`slim` branch, commit `fd0022170615ce0d8162d219bff07232fa6dd84f`), CC0-1.0 (SPDX
  header in the file), unmodified.
- `indra/newview/app_settings/fx/Shaders/ReShadeUI.fxh` -- Slipstream's own clean-room
  implementation of the public `__UNIFORM_*` macro names (the upstream file carries no
  licence and is not used), CC0-1.0.
