#!/usr/bin/env python3
"""Compile/validate lighting-history shader variants without building the viewer.

Usage: python scripts/check/compile_stochastic_shaders.py ROOT /tmp/OUTPUT
Two compiler workers; SPIR-V and the command/result manifest stay outside the tree.
No embedded headers, viewer build, linker or renderer execution.
"""
import ast
import concurrent.futures
import json
from pathlib import Path
import subprocess
import sys


def registered_variants(root):
    tree = ast.parse((root / 'indra/vkclean/regen_world_spv.py').read_text())
    def literal(name):
        return next(ast.literal_eval(node.value) for node in tree.body
                    if isinstance(node, ast.Assign)
                    and any(isinstance(t, ast.Name) and t.id == name for t in node.targets))
    entries = literal('SHADERS')
    by_name = {e[2]: e for e in entries}
    for kernel, name in enumerate(literal('RT_BC_KERNELS'), 1):
        entry = by_name[name]
        entries.append((entry[0], entry[1], name[:-3] + 'BcSpv',
                        list(entry[3]) + ['-DCLEAN_RT_HANG_BEACON', f'-DCLEAN_RT_BC_KERNEL={kernel}']))
    families = {'clean_dn_temporal.comp', 'clean_dn_atrous.comp',
                'clean_rt_mask_temporal.comp', 'clean_rt_mask_spatial.comp',
                'clean_rt_mask_cache.comp', 'clean_rt_shadow_mask.comp',
                'clean_godrays_dn_temporal.comp', 'clean_godrays_atrous.comp',
                'clean_draw.frag', 'clean_draw.vert'}
    deferred = {'-DCLEAN_GI_TRACE', '-DCLEAN_DEFER_PASS=15', '-DCLEAN_DEFER_PASS=3',
                '-DCLEAN_DEFER_PASS=7', '-DCLEAN_DEFER_PASS=8', '-DCLEAN_DEFER_PASS=16'}
    return [e for e in entries if e[0] in families or e[0] == 'clean_deferred_light.comp'
            and deferred.intersection(e[3] if len(e) > 3 else [])]


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    root, output = (Path(arg).resolve() for arg in sys.argv[1:])
    if output == root or root in output.parents:
        raise SystemExit('Shader check output must be outside the source worktree (use /tmp).')
    output.mkdir(parents=True, exist_ok=True)
    def check(entry):
        filename, stage, name, *extra = entry
        flags = (extra[0] if extra else []) + ['-DCLEAN_STAGE_' + stage.upper()]
        binary = output / (name + '.spv')
        command = ['glslc', '--target-env=vulkan1.3', '-O', '-fshader-stage=' + stage,
                   *flags, str(root / 'indra/vkclean/shaders' / filename), '-o', str(binary)]
        compiled = subprocess.run(command, capture_output=True, text=True)
        validated = None if compiled.returncode else subprocess.run(
            ['spirv-val', '--target-env', 'vulkan1.3', str(binary)], capture_output=True, text=True)
        return dict(name=name, ok=validated is not None and validated.returncode == 0,
                    cmd=command, error=compiled.stderr + (validated.stderr if validated else ''))
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as workers:
        results = list(workers.map(check, registered_variants(root)))
    (output / 'checks.json').write_text(json.dumps(results, indent=2))
    for result in results:
        if not result['ok']:
            print(result['name'], result['error'][:1600])
    print(f'{sum(r["ok"] for r in results)}/{len(results)} compiled and spirv-val validated')
    return int(any(not r['ok'] for r in results))


if __name__ == '__main__':
    sys.exit(main())
