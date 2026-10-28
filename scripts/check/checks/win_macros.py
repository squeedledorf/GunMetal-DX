"""Fail when C/C++ code under indra/ names a variable or parameter after a Windows SDK macro.

The Linux build never sees those macros, so the mistake only shows up in the Windows
cross-build (CI), after the change has landed. Rule and the full list: doc/CODING_RULES.md.
A line can opt out with the comment  // win-macro-ok  (only where the name is #undef'd).
"""
import os
import re

NAME = "win-macros"
STAGE = "source"
DESCRIPTION = "no identifier is named after a Windows SDK macro (far, near, interface, ...)"
GUARDS = ("'far' / 'near' (minwindef.h: defined to nothing) broke Jetstream 1.5.0.7 / 1.5.0.8 and "
          "v1.4.0 (re-released as 1.4.1); the Linux build cannot see it. Rule: doc/CODING_RULES.md")

# Macros the Windows headers we build with (WIN32_LEAN_AND_MEAN, NOMINMAX) define and that
# make an ordinary identifier stop parsing. 'small' (rpcndr.h) is not pulled in by our lean
# windows.h, so it is not listed; add a name here when a Windows build breaks on another one.
WIN_MACROS = ("far", "near", "FAR", "NEAR", "interface", "PASCAL", "CALLBACK", "WINAPI", "CONST")

TYPES = (r"bool|char|short|int|long|float|double|unsigned|signed|size_t|auto|"
         r"U8|U16|U32|U64|S8|S16|S32|S64|F32|F64|BOOL|"
         r"LLVector2|LLVector3|LLVector3d|LLVector4|LLVector4a|LLColor3|LLColor4|LLUUID|LLSD|"
         r"std::string|glm::vec[234]|vec[234]")
DECL = re.compile(r"\b(?:%s)\b(?:\s*const)?\s*[&*]*\s+(%s)\s*(?:[=;,)\[{:]|$)"
                  % (TYPES, "|".join(WIN_MACROS)))
EXTS = (".cpp", ".cc", ".c", ".h", ".hpp", ".inl", ".mm")
SKIP_DIRS = ("build-", ".git", "packages", "3p")


def strip(line):
    """Drop // comments and string literals so prose like 'the near plane' never matches."""
    line = re.sub(r'"(?:\\.|[^"\\])*"', '""', line)
    return line.split("//", 1)[0]


def run(ctx, result):
    scanned = 0
    for base, dirs, files in os.walk(ctx.path("indra")):
        dirs[:] = [d for d in dirs if not d.startswith(SKIP_DIRS)]
        for f in files:
            if not f.endswith(EXTS):
                continue
            scanned += 1
            path = os.path.join(base, f)
            with open(path, encoding="utf-8", errors="replace") as fh:
                in_block = False
                for n, raw in enumerate(fh, 1):
                    if "win-macro-ok" in raw:
                        continue
                    line = raw
                    if in_block:
                        if "*/" not in line:
                            continue
                        line = line.split("*/", 1)[1]
                        in_block = False
                    if "/*" in line and "*/" not in line.split("/*", 1)[1]:
                        line = line.split("/*", 1)[0]
                        in_block = True
                    m = DECL.search(strip(line))
                    if m:
                        result.fail("%s:%d: '%s' is a Windows SDK macro and breaks the Windows "
                                    "build (rename it, doc/CODING_RULES.md): %s"
                                    % (ctx.relative(path), n, m.group(1), raw.strip()))
    result.summary = "%d source files" % scanned
