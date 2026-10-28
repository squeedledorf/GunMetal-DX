#!/usr/bin/env python3
# Compile clean_deferred_light.comp variants and print NVIDIA register counts.
# usage: measure.py [VARIANT... -DFLAG...] [:: VARIANT... -DFLAG...] ...   (each '::' group is one experiment)
import subprocess, sys, os, re, concurrent.futures as cf
SRC = os.path.join(os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "indra", "vkclean")), "shaders", "clean_deferred_light.comp")
OUT = os.environ.get("REGPROBE_OUT", os.path.join(os.environ.get("TMPDIR", "/tmp"), "regprobe"))
os.makedirs(OUT, exist_ok=True)
PS = os.environ.get("REGPROBE_BIN", os.path.join(OUT, "pipestats"))   # build: g++ -std=c++17 -O2 pipestats.cpp -lvulkan -o $REGPROBE_OUT/pipestats
B = ["-DCLEAN_DEFER_PASS=11", "-DCLEAN_PROJ_SHADOW_RAYQUERY", "-DRT_SET=4"]
RT = B + ["-DCLEAN_RT_LIST"]
GI = B + ["-DCLEAN_GI_TRACE"]
VQ = ["-DCLEAN_VIS_QUEUE"]
M2 = VQ + ["-DCLEAN_VIS_QUEUE_M2"]
P15 = ["-DCLEAN_DEFER_PASS=15", "-DCLEAN_PROJ_SHADOW_RAYQUERY", "-DRT_SET=4", "-DCLEAN_SHADOW_CORE"]
MRG = ["-DCLEAN_SHADOW_MERGE"]
V = {
    "RtSplitMatVq2":     RT + ["-DCLEAN_RT_STAGE=2"] + M2,
    "RtSplitLayersVq2":  RT + ["-DCLEAN_RT_STAGE=4"] + M2,
    "RtSplitLightVq2":   RT + ["-DCLEAN_RT_STAGE=3"] + M2,
    "GiSplitHitMatVq2":  GI + ["-DCLEAN_GI_STAGE=6"] + M2,
    "GiSplitHitLightVq2": GI + ["-DCLEAN_GI_STAGE=7"] + M2,
    "RtSplitReqVq2":     RT + ["-DCLEAN_RT_STAGE=5"] + M2,
    "GiSplitHitReqVq2":  GI + ["-DCLEAN_GI_STAGE=8"] + M2,
    "VisWalk":           RT + ["-DCLEAN_VIS_WALK"],
    "RtSplitMat":        RT + ["-DCLEAN_RT_STAGE=2"],
    "RtSplitMatVq":      RT + ["-DCLEAN_RT_STAGE=2"] + VQ,
    "RtSplitLight":      RT + ["-DCLEAN_RT_STAGE=3"],
    "RtSplitLightVq":    RT + ["-DCLEAN_RT_STAGE=3"] + VQ,
    "RtSplitTrace":      RT + ["-DCLEAN_RT_STAGE=1"],
    "GiSplitHitMat":     GI + ["-DCLEAN_GI_STAGE=6"],
    "GiSplitHitMatVq":   GI + ["-DCLEAN_GI_STAGE=6"] + VQ,
    "GiSplitHitLight":   GI + ["-DCLEAN_GI_STAGE=7"],
    "GiSplitHitLightVq": GI + ["-DCLEAN_GI_STAGE=7"] + VQ,
    "GiTrace":           GI,
    "RtList":            RT,
    "RtTile":            B,
    "RtFwd":             ["-DCLEAN_DEFER_PASS=14", "-DCLEAN_PROJ_SHADOW_RAYQUERY", "-DRT_SET=4"],
    "RtPreTrace":        ["-DCLEAN_DEFER_PASS=12", "-DCLEAN_PROJ_SHADOW_RAYQUERY", "-DRT_SET=3"],
    "RtMirror":          ["-DCLEAN_DEFER_PASS=12", "-DCLEAN_PROJ_SHADOW_RAYQUERY", "-DRT_SET=3", "-DCLEAN_RT_MIRROR"],
    "GiSplitShade":      GI + ["-DCLEAN_GI_STAGE=2"],
    "GiSplitReshade":    GI + ["-DCLEAN_GI_STAGE=4"],
    "ProbeRt":           ("clean_probe_rt.comp", ["-DRT_SET=2"]),
    # (#1031) pass 15's split shadow core (llcleanprojstochvk.cpp; the viewer specializes kDebugViews off:
    # PS_SPEC="1=0")
    "ShadowMergeSelect":    P15 + MRG + ["-DCLEAN_SHADOW_STAGE=1"],
    "ProjStochSelect":      P15 + ["-DCLEAN_SHADOW_STAGE=1"],
    "ShadowMergeSunVis":    P15 + MRG + ["-DCLEAN_SHADOW_STAGE=4"],
    "ShadowMergeSunVisIso": P15 + MRG + ["-DCLEAN_SHADOW_STAGE=4", "-DCLEAN_SHADOW_ISO_LOD"],
    "ShadowSplitVis":       P15 + ["-DCLEAN_SHADOW_STAGE=3"],
    "ShadowSplitVisIso":    P15 + ["-DCLEAN_SHADOW_STAGE=3", "-DCLEAN_SHADOW_ISO_LOD"],
    "ShadowSplitShade":     P15 + ["-DCLEAN_SHADOW_STAGE=2"],
    # (#1039, RenderVKVisQueue bit 8) the shadow core through the visibility queue: GEN (no walk), WALK (the
    # sample's rays from the request), the VIS TAIL (VIS over the jobs past the region)
    "ShadowQSunGen":        P15 + MRG + ["-DCLEAN_SHADOW_STAGE=4", "-DCLEAN_VQ_SHADOW=1"],
    "ShadowQSunWalk":       P15 + MRG + ["-DCLEAN_SHADOW_STAGE=4", "-DCLEAN_VQ_SHADOW=2"],
    "ShadowQSunWalkIso":    P15 + MRG + ["-DCLEAN_SHADOW_STAGE=4", "-DCLEAN_VQ_SHADOW=2", "-DCLEAN_SHADOW_ISO_LOD"],
    "ShadowQVisGen":        P15 + ["-DCLEAN_SHADOW_STAGE=3", "-DCLEAN_VQ_SHADOW=1"],
    "ShadowQVisWalk":       P15 + ["-DCLEAN_SHADOW_STAGE=3", "-DCLEAN_VQ_SHADOW=2"],
    "ShadowQVisWalkIso":    P15 + ["-DCLEAN_SHADOW_STAGE=3", "-DCLEAN_VQ_SHADOW=2", "-DCLEAN_SHADOW_ISO_LOD"],
    "ShadowQVisTail":       P15 + ["-DCLEAN_SHADOW_STAGE=3", "-DCLEAN_VQ_SHADOW=3"],
    "ShadowQVisTailIso":    P15 + ["-DCLEAN_SHADOW_STAGE=3", "-DCLEAN_VQ_SHADOW=3", "-DCLEAN_SHADOW_ISO_LOD"],
}
SHADOW = ["ShadowMergeSelect", "ShadowMergeSunVis", "ShadowMergeSunVisIso", "ShadowSplitVis", "ShadowSplitVisIso",
          "ShadowSplitShade", "ShadowQSunGen", "ShadowQSunWalk", "ShadowQSunWalkIso", "ShadowQVisGen", "ShadowQVisWalk",
          "ShadowQVisWalkIso", "ShadowQVisTail", "ShadowQVisTailIso"]
OTHERS = ["RtList", "RtTile", "RtFwd", "RtPreTrace", "RtMirror", "GiTrace", "GiSplitShade", "GiSplitReshade", "ProbeRt",
          "RtSplitLight", "RtSplitLightVq", "RtSplitLightVq2", "GiSplitHitLight", "GiSplitHitLightVq", "GiSplitHitLightVq2",
          "RtSplitMat", "RtSplitMatVq", "GiSplitHitMat", "GiSplitHitMatVq"]
MAIN3 = ["RtSplitMatVq2", "GiSplitHitMatVq2", "RtSplitLayersVq2"]
groups, cur = [], []
for a in sys.argv[1:]:
    if a == "::": groups.append(cur); cur = []
    else: cur.append(a)
groups.append(cur)
jobs = []
for gi, g in enumerate(groups):
    flags = [a for a in g if a.startswith("-D")]
    names = [a for a in g if not a.startswith("-D")] or MAIN3
    if names == ["all"]: names = list(V)
    if names == ["others"]: names = OTHERS
    if names == ["shadow"]: names = SHADOW
    for nm in names: jobs.append((gi, nm, flags))
def comp(j):
    gi, nm, flags = j
    out = os.path.join(OUT, f"g{gi}_{nm}.spv")
    v = V[nm]
    src, vf = (os.path.join(os.path.dirname(SRC), v[0]), v[1]) if isinstance(v, tuple) else (SRC, v)
    r = subprocess.run(["glslc", "--target-env=vulkan1.3", "-O", "-fshader-stage=comp", *vf, *flags, src, "-o", out],
                       capture_output=True, text=True)
    if r.returncode: return j, None, r.stderr[-3000:]
    return j, out, ""
with cf.ThreadPoolExecutor(12) as ex:
    res = list(ex.map(comp, jobs))
for j, _, err in res:
    if err: print(j[1], " ".join(j[2]), "COMPILE ERROR\n", err)
ok = [r for r in res if r[1]]
stats = {}
if ok:
    r = subprocess.run([PS] + [o for _, o, _ in ok], capture_output=True, text=True)
    for line in r.stdout.splitlines():
        if ".spv:" not in line: continue
        p, rest = line.split(".spv:", 1)
        stats[p + ".spv"] = (re.search(r"Register Count=(\d+)", rest).group(1), re.search(r"Binary Size=(\d+)", rest).group(1))
    if r.returncode: print(r.stderr)
last = None
for j, o, _ in ok:
    if j[0] != last:
        print("--", " ".join(j[2]) or "(baseline)"); last = j[0]
    regs, size = stats.get(o, ("?", "?"))
    print(f"   {j[1]:22s} regs={regs:>4s} bin={size:>8s}")
