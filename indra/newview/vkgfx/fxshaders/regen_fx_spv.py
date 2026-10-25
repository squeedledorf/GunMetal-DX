#!/usr/bin/env python3
# Generate vk_fx_shaders_spv.h -- the effects host's own shaders (#544 / #548: input resolves,
# final copy, per-effect scope, HDR point in / out) -- via glslc --target-env=vulkan1.3 -O.
# CMake runs this at build time into the build tree (target vk_shaders); not in git. By hand:
# regen_fx_spv.py --out-dir <dir>. The ReShade FX effects themselves are compiled at run time
# (llfxcompile.cpp).
import os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.normpath(os.path.join(HERE, "..", "..", "..", "vkclean")))
import spvbuild

# (glsl file, stage, array symbol, extra args)
SHADERS = [
    ("fx_fullscreen.vert", "vert", "kFxFullscreenVertSpv", []),
    ("fx_blit.frag",       "frag", "kFxBlitFragSpv", []),
    ("fx_scope.frag",      "frag", "kFxScopeFragSpv", []),
    ("fx_hdr.frag",        "frag", "kFxHdrInFragSpv", []),
    ("fx_hdr.frag",        "frag", "kFxHdrOutFragSpv", ["-DFX_HDR_OUT"]),
    ("fx_resolve.comp",    "comp", "kFxResolveDepthSpv", ["-DFX_RESOLVE_DEPTH"]),
    ("fx_resolve.comp",    "comp", "kFxResolveNormalSpv", ["-DFX_RESOLVE_NORMAL"]),
    ("fx_resolve.comp",    "comp", "kFxResolveMotionSpv", ["-DFX_RESOLVE_MOTION"]),
    ("fx_resolve.comp",    "comp", "kFxResolveMaskSpv", ["-DFX_RESOLVE_MASK"]),
    ("fx_resolve.comp",    "comp", "kFxResolveHdrSpv", ["-DFX_RESOLVE_HDR"]),
]


def main():
    a = spvbuild.args("Generate the FX host's embedded SPIR-V header", "fx")
    words_all = spvbuild.compile_many([(os.path.join(HERE, n), st, ex) for n, st, _, ex in SHADERS], a.cache_dir)
    lines = [
        "// AUTO-GENERATED at build time from indra/newview/vkgfx/fxshaders/*.{vert,frag,comp}",
        "// via glslc --target-env=vulkan1.3 -O (indra/newview/vkgfx/fxshaders/regen_fx_spv.py).",
        "// The FX host's own shaders (#544). Not in git.",
        "#pragma once",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
    ]
    for (name, stage, sym, extra), words in zip(SHADERS, words_all):
        lines.append(spvbuild.emit_array(sym, words))
    spvbuild.write_if_changed(os.path.join(a.out_dir, "vk_fx_shaders_spv.h"), "\n".join(lines) + "\n")
    spvbuild.prune_cache(a.cache_dir)


if __name__ == "__main__":
    sys.exit(main())
