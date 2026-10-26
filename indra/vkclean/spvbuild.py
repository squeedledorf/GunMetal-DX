#!/usr/bin/env python3
# Shared by the SPIR-V header generators (regen_world_spv.py, vkgfx/fxshaders/regen_fx_spv.py,
# vkgfx/llrendervk/regen_ui_spv.py), which CMake runs at build time: the generated headers live
# in the build directory, never in git.
#
# compile_many() compiles shader variants in parallel with glslc and keeps each result in a
# cache keyed by the PREPROCESSED source (glslc -E: every #include resolved) + stage + flags +
# the glslc version, so a rebuild after editing one shader file recompiles only the variants
# whose preprocessed text changed. glslc -E is cheap next to an optimised compile.
import argparse, hashlib, os, re, shutil, struct, subprocess, sys, tempfile
from concurrent.futures import ThreadPoolExecutor

TARGET = "--target-env=vulkan1.3"
# SL_SPV_DEBUG=<regex> at build time: emit source-level debug info (glslc -g) so GPU profilers can
# map samples to shader source lines, for the variants whose "<file> <flags>" matches the regex
# (e.g. "CLEAN_GI_TRACE|CLEAN_SHADOW_CORE"). Each variant embeds its whole preprocessed source, so
# matching everything ("1" or ".") makes the world header too big to link. Profiling builds only.
# Ninja does not track the environment: delete the generated vk_*_spv.h headers after changing it.
_DEBUG_RE = os.environ.get("SL_SPV_DEBUG") or None
if _DEBUG_RE == "1":
    _DEBUG_RE = "."


def debug_flags(path, flags):
    if _DEBUG_RE and re.search(_DEBUG_RE, os.path.basename(path) + " " + " ".join(flags)):
        return ["-g"]
    return []
_tool_versions = {}
_used = set()          # cache entries this run read or wrote (prune_cache drops the rest)


def tool(name):
    """Path of glslc / glslangValidator: $GLSLC / $GLSLANG_VALIDATOR (CMake passes the ones it
    found), else PATH."""
    env = {"glslc": "GLSLC", "glslangValidator": "GLSLANG_VALIDATOR"}.get(name)
    return (os.environ.get(env) if env else None) or shutil.which(name)


def tool_version(name):
    if name not in _tool_versions:
        exe = tool(name)
        if not exe:
            raise SystemExit(f"spvbuild: {name} not found on PATH. Install the Vulkan shader tools "
                             f"(Arch: shaderc glslang; Debian/Ubuntu: glslc glslang-tools).")
        r = subprocess.run([exe, "--version"], capture_output=True, text=True)
        _tool_versions[name] = (r.stdout + r.stderr).strip()
    return _tool_versions[name]


def preprocess(path, stage, flags):
    """glslc -E output (text) of one variant; only -D / -I flags apply to preprocessing."""
    pre_flags = [f for f in flags if f.startswith(("-D", "-I"))]
    r = subprocess.run([tool("glslc"), "-E", f"-fshader-stage={stage}", TARGET, *pre_flags, path],
                       capture_output=True, text=True)
    if r.returncode:
        raise SystemExit(f"spvbuild: preprocessing {path} ({stage} {' '.join(flags)}) failed:\n{r.stderr}")
    return r.stdout


def variant_key(kind, path, stage, flags, text):
    h = hashlib.blake2b(digest_size=16)
    for part in (kind, tool_version("glslc"), " ".join(debug_flags(path, flags)), stage, "\0".join(flags), os.path.basename(path), text):
        h.update(part.encode("utf-8", "surrogateescape"))
        h.update(b"\x01")
    return h.hexdigest()


def _compile_one(path, stage, flags, cache_dir):
    key = None
    if cache_dir:
        key = variant_key("spv", path, stage, flags, preprocess(path, stage, flags))
        hit = os.path.join(cache_dir, key + ".spv")
        _used.add(key + ".spv")
        if os.path.isfile(hit):
            with open(hit, "rb") as f:
                data = f.read()
            return list(struct.unpack(f"<{len(data)//4}I", data))
    with tempfile.TemporaryDirectory() as td:
        out = os.path.join(td, "o.spv")
        r = subprocess.run([tool("glslc"), TARGET, "-O", *debug_flags(path, flags), f"-fshader-stage={stage}", *flags, path, "-o", out],
                           capture_output=True, text=True)
        if r.returncode:
            raise SystemExit(f"spvbuild: compiling {path} ({stage} {' '.join(flags)}) failed:\n{r.stderr}")
        with open(out, "rb") as f:
            data = f.read()
    assert len(data) % 4 == 0, f"{path}: SPIR-V not word-aligned"
    if key:
        tmp = os.path.join(cache_dir, f"{key}.{os.getpid()}.tmp")
        with open(tmp, "wb") as f:
            f.write(data)
        os.replace(tmp, os.path.join(cache_dir, key + ".spv"))
    return list(struct.unpack(f"<{len(data)//4}I", data))


def compile_many(jobs, cache_dir=None):
    """jobs: [(path, stage, flags)] -> [words] in the same order (parallel, cached)."""
    tool_version("glslc")
    if cache_dir:
        os.makedirs(cache_dir, exist_ok=True)
    with ThreadPoolExecutor(max_workers=max(1, os.cpu_count() or 1)) as ex:
        return list(ex.map(lambda j: _compile_one(j[0], j[1], tuple(j[2]), cache_dir), jobs))


def prune_cache(cache_dir):
    """Delete cache entries this run didn't use: variants that no longer exist or whose
    source changed. Keeps the cache at one entry per live variant."""
    if not cache_dir or not os.path.isdir(cache_dir):
        return
    for name in os.listdir(cache_dir):
        if name not in _used:
            try:
                os.remove(os.path.join(cache_dir, name))
            except OSError:
                pass


def emit_array(sym, words, definition=False):
    # definition (<GMDX>): the out-of-line definition of an array a header declares extern (with
    # its bound and _bytes); otherwise a self-contained static array + _bytes for a header.
    lines = [f"const uint32_t {sym}[{len(words)}] = {{" if definition else f"static const uint32_t {sym}[] = {{"]
    for i in range(0, len(words), 8):
        lines.append("    " + " ".join(f"0x{w:08x}u," for w in words[i:i + 8]))
    lines.append("};")
    if not definition:
        lines.append(f"static const size_t {sym}_bytes = sizeof({sym});")
    return "\n".join(lines)


def write_if_changed(path, text):
    """Write only when the content differs, so an unchanged header doesn't rebuild its users."""
    try:
        with open(path, encoding="utf-8") as f:
            if f.read() == text:
                print(f"unchanged {path}")
                return
    except OSError:
        pass
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(text)
    os.replace(tmp, path)
    print(f"wrote {path}")


def mark_used(name):
    _used.add(name)


def args(description, cache_name):
    """--out-dir (required) and --cache-dir (default OUT_DIR/.spvcache/<cache_name>: one cache
    per generator, so pruning one never drops another's entries)."""
    p = argparse.ArgumentParser(description=description)
    p.add_argument("--out-dir", required=True, help="directory for the generated header(s) (the build tree)")
    p.add_argument("--cache-dir", help="compiled-variant cache (default: OUT_DIR/.spvcache/" + cache_name + ")")
    a = p.parse_args()
    os.makedirs(a.out_dir, exist_ok=True)
    a.cache_dir = a.cache_dir or os.path.join(a.out_dir, ".spvcache", cache_name)
    return a
