#!/usr/bin/env python3
# Push-constant budget check for the embedded VK shaders (#238). Every push block must
# fit the Vulkan spec minimum maxPushConstantsSize, 128 B (AMD Polaris exposes exactly
# that). Used by regen_world_spv.py and llrendervk/regen_ui_spv.py: each shader variant
# is preprocessed by glslc -E (resolves #include) and reflected by glslangValidator -q;
# a push block over the budget fails the build. glslangValidator is required.
import json, os, re, shutil, subprocess, sys, tempfile
from concurrent.futures import ThreadPoolExecutor
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import spvbuild

BUDGET = 128

def push_blocks(path, stage, defines=(), cache_dir=None):
    """[(block name, size)] of the push-constant blocks the variant declares + uses. Cached
    (cache_dir) by the preprocessed source, like spvbuild's compiled variants."""
    text = spvbuild.preprocess(path, stage, list(defines))
    hit = None
    if cache_dir:
        key = spvbuild.variant_key("push:" + spvbuild.tool_version("glslangValidator"), path, stage,
                                   list(defines), text)
        hit = os.path.join(cache_dir, key + ".push.json")
        spvbuild.mark_used(key + ".push.json")
        if os.path.isfile(hit):
            with open(hit) as f:
                return [tuple(x) for x in json.load(f)]
    with tempfile.TemporaryDirectory() as td:
        pre = os.path.join(td, "pre." + stage)
        with open(pre, "w") as f:
            f.write(text)
        r = subprocess.run([spvbuild.tool("glslangValidator"), "-V", "--target-env", "vulkan1.3", "-q", "-S", stage,
                            pre, "-o", os.path.join(td, "o.spv")], capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(f"{path} ({stage} {' '.join(defines)}): glslangValidator failed:\n{r.stdout}{r.stderr}")
    out, blk = [], False
    for line in r.stdout.splitlines():
        if line.startswith("Uniform block reflection"):
            blk = True; continue
        if blk and not line.strip():
            break
        m = blk and re.match(r"(\S+): offset .* size (\d+), index \d+, binding (-?\d+)", line)
        if m and m.group(3) == "-1":   # push constants carry no binding
            out.append((m.group(1), int(m.group(2))))
    if hit:
        with open(hit + f".{os.getpid()}.tmp", "w") as f:
            json.dump(out, f)
        os.replace(hit + f".{os.getpid()}.tmp", hit)
    return out

def check(entries, cache_dir=None):
    """entries: [(path, stage, defines, label)]. Raises on a block over BUDGET; returns the max."""
    spvbuild.tool_version("glslangValidator")   # required: exits with install hints if missing
    if cache_dir:
        os.makedirs(cache_dir, exist_ok=True)
    with ThreadPoolExecutor(max_workers=max(1, os.cpu_count() or 1)) as ex:
        results = list(ex.map(lambda e: push_blocks(e[0], e[1], e[2], cache_dir), entries))
    worst, bad = 0, []
    for (path, stage, defines, label), blocks in zip(entries, results):
        for name, size in blocks:
            worst = max(worst, size)
            if size > BUDGET:
                bad.append(f"{label}: push block {name} is {size} B > {BUDGET} B")
    if bad:
        raise SystemExit("push_budget: FAILED (#238)\n  " + "\n  ".join(bad))
    print(f"push_budget: {len(entries)} shader variants, largest push block {worst} B (<= {BUDGET})")
    return worst

# ---- Workgroup shared memory (#238 claim 2) ----
# The Vulkan minimum maxComputeSharedMemorySize is 16 KiB; AMD GCN / Polaris on the
# Windows driver exposes 32 KiB (NVIDIA / RDNA: 48-64 KiB). A compute shader over the
# device limit is invalid usage (pipeline creation fails or the driver crashes), so
# every variant must fit SMEM_BUDGET unless it is listed with a capability-gated
# fallback variant that does (the renderer picks the fallback by the device limit).
SMEM_BUDGET = 32768

def shared_bytes(words):
    """Bytes of Workgroup-storage variables in a SPIR-V module (std430-like layout:
    vec3 aligned to 16 -- conservative for drivers that pad it). Array lengths from
    OpConstant / OpSpecConstant defaults; a length computed by OpSpecConstantOp counts
    as 1 (clean_vrs_rate.comp's T-sized tile: 4.3 KiB at its largest T = 32)."""
    ids, i, n = {}, 5, len(words)
    consts, total = {}, 0
    def layout(t):
        k = ids.get(t)
        if k is None: return (4, 4)
        if k[0] in ("int", "float"): s = k[1] // 8; return (s, s)
        if k[0] == "bool": return (4, 4)
        if k[0] == "vec":
            s, a = layout(k[1]); c = k[2]
            return (s * c, a * (4 if c == 3 else c))
        if k[0] == "mat":
            s, a = layout(k[1]); st = (s + a - 1) // a * a
            return (st * k[2], a)
        if k[0] == "arr":
            s, a = layout(k[1]); st = (s + a - 1) // a * a
            return (st * consts.get(k[2], 1), a)
        if k[0] == "struct":
            off, al = 0, 1
            for m in k[1]:
                s, a = layout(m); off = (off + a - 1) // a * a + s; al = max(al, a)
            return ((off + al - 1) // al * al, al)
        return (4, 4)
    ptr = {}
    while i < n:
        op, wc = words[i] & 0xFFFF, words[i] >> 16
        a = words[i + 1:i + wc]
        if   op == 20: ids[a[0]] = ("bool",)
        elif op == 21: ids[a[0]] = ("int", a[1])
        elif op == 22: ids[a[0]] = ("float", a[1])
        elif op == 23: ids[a[0]] = ("vec", a[1], a[2])
        elif op == 24: ids[a[0]] = ("mat", a[1], a[2])
        elif op == 28: ids[a[0]] = ("arr", a[1], a[2])
        elif op == 30: ids[a[0]] = ("struct", list(a[1:]))
        elif op == 32: ptr[a[0]] = (a[1], a[2])
        elif op in (43, 50): consts[a[1]] = a[2]          # OpConstant / OpSpecConstant (default)
        elif op == 59 and a[2] == 4 and a[0] in ptr:      # OpVariable, StorageClass Workgroup
            total += layout(ptr[a[0]][1])[0]
        i += max(wc, 1)
    return total

# Task / mesh shaders (VK_EXT_mesh_shader, #743): the spec minimums of
# maxTaskSharedMemorySize (32 KiB) and maxMeshSharedMemorySize (28 KiB; RTX 5090: 28 KiB).
TASK_SMEM_BUDGET = 32768
MESH_SMEM_BUDGET = 28672

def check_shared(entries, fallbacks=None, budget=None, what="compute"):
    """entries: [(label, spirv words)]. fallbacks: {label over budget: label of its gated
    fallback variant}. Raises on a variant over the budget (default SMEM_BUDGET) without a
    fitting fallback. what: the stage family, for the messages."""
    SMEM_BUDGET = budget if budget is not None else globals()["SMEM_BUDGET"]
    fallbacks = fallbacks or {}
    size = {lab: shared_bytes(w) for lab, w in entries}
    bad, worst = [], 0
    for lab, s in size.items():
        if s <= SMEM_BUDGET:
            worst = max(worst, s); continue
        fb = fallbacks.get(lab)
        if fb is None or size.get(fb, SMEM_BUDGET + 1) > SMEM_BUDGET:
            bad.append(f"{lab}: {s} B shared > {SMEM_BUDGET} B and no gated fallback <= {SMEM_BUDGET} B")
        else:
            print(f"shared_mem: {lab} {s} B > {SMEM_BUDGET} B -> gated fallback {fb} ({size[fb]} B)")
    if bad:
        raise SystemExit("shared_mem: FAILED (#238)\n  " + "\n  ".join(bad))
    print(f"shared_mem ({what}): {len(entries)} variants, largest unguarded {worst} B (<= {SMEM_BUDGET})")
