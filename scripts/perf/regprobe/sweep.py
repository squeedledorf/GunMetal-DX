#!/usr/bin/env python3
# Compile every clean_deferred_light.comp / clean_probe_rt.comp variant listed in regen_world_spv.py (compile only).
import ast, os, re, subprocess, concurrent.futures as cf
ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "indra", "vkclean"))
OUT = os.path.join(os.environ.get("REGPROBE_OUT", os.path.join(os.environ.get("TMPDIR", "/tmp"), "regprobe")), "sweep")
os.makedirs(OUT, exist_ok=True)
src = open(os.path.join(ROOT, "regen_world_spv.py")).read()
jobs = []
for m in re.finditer(r'\(\s*"(clean_deferred_light\.comp|clean_probe_rt\.comp)"\s*,\s*"comp"\s*,\s*"(\w+)"\s*(?:,\s*(\[[^\]]*\]))?\s*\)', src):
    flags = ast.literal_eval(m.group(3)) if m.group(3) else []
    jobs.append((m.group(1), m.group(2), flags))
def comp(j):
    f, name, flags = j
    r = subprocess.run(["glslc", "--target-env=vulkan1.3", "-O", "-fshader-stage=comp", *flags,
                        os.path.join(ROOT, "shaders", f), "-o", os.path.join(OUT, name + ".spv")], capture_output=True, text=True)
    return name, r.returncode, r.stderr[-1500:]
with cf.ThreadPoolExecutor(16) as ex:
    res = list(ex.map(comp, jobs))
bad = [r for r in res if r[1]]
print(f"{len(res)} variants, {len(bad)} failed")
for n, _, e in bad: print(n, e)
