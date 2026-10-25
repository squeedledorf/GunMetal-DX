#!/usr/bin/env python3
# Generate vk_ui_shaders_spv.h (the LLRenderVK facade's UI shaders) via
# glslc --target-env=vulkan1.3 -O. CMake runs this at build time into the build tree (target
# vk_shaders); not in git. By hand: regen_ui_spv.py --out-dir <dir>.
import os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC  = os.path.join(HERE, "shaders")
sys.path.insert(0, os.path.normpath(os.path.join(HERE, "..", "..", "..", "vkclean")))
import push_budget, spvbuild

SHADERS = [
    ("ui.vert",       "kUiVertSpv"),
    ("ui.frag",       "kUiFragSpv"),
    ("ui_atest.frag", "kUiFragATestSpv"),   # alpha-tested variant (#83 offscreen targets)
    ("ui_occl.vert",  "kUiVertOcclSpv"),    # in-world label occlusion variant (#487)
]


def main():
    a = spvbuild.args("Generate the UI facade's embedded SPIR-V header", "ui")
    jobs = [(os.path.join(SRC, f), f.rsplit(".", 1)[1], ()) for f, _ in SHADERS]
    words_all = spvbuild.compile_many(jobs, a.cache_dir)
    # #238: every push block <= 128 B (the Vulkan minimum maxPushConstantsSize).
    push_budget.check([(p, st, fl, sym) for (p, st, fl), (_, sym) in zip(jobs, SHADERS)], a.cache_dir)
    lines = [
        "// AUTO-GENERATED at build time from indra/newview/vkgfx/llrendervk/shaders/{ui.vert,ui.frag,ui_atest.frag,ui_occl.vert}",
        "// via glslc --target-env=vulkan1.3 -O (llrendervk/regen_ui_spv.py). Not in git.",
        "// Embedded so the viewer VK path needs no runtime shader files/packaging.",
        "#pragma once",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
    ]
    for (_, sym), words in zip(SHADERS, words_all):
        lines.append(spvbuild.emit_array(sym, words))
        lines.append("")
    spvbuild.write_if_changed(os.path.join(a.out_dir, "vk_ui_shaders_spv.h"), "\n".join(lines))
    spvbuild.prune_cache(a.cache_dir)


if __name__ == "__main__":
    main()
