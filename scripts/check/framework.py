"""Shared pieces of the check framework (see scripts/check/run.py and doc/DEVELOPER_GUIDE.md).

A check is a module in scripts/check/checks/ that declares
    NAME         short unique id, used by --only and in the result table
    STAGE        "source"  runs on the repository tree, needs no build
                 "package" runs on the packaged viewer tree (build-linux-x86_64/newview/packaged)
    DESCRIPTION  one line: what the check does
    GUARDS       what regression it prevents, with the ticket / release references
    run(ctx, result)  inspects ctx (a Context) and records into result (a Result):
                 result.fail(msg) fails the run, result.warn(msg) does not, result.summary
                 is the one-line detail shown in the table
Python 3 standard library only.
"""

import functools
import glob
import os
import re
import subprocess

STAGES = ("source", "package")


class Result:
    """Outcome of one check: failures fail the run, warnings are shown but never fail it."""

    def __init__(self):
        self.failures = []
        self.warnings = []
        self.summary = ""

    def fail(self, message):
        self.failures.append(message)

    def warn(self, message):
        self.warnings.append(message)

    @property
    def passed(self):
        return not self.failures


class Context:
    """What a check may look at. root is the checkout, package the packaged viewer tree."""

    def __init__(self, root, package=None, glibc_max="2.38", glibcxx_max="3.4.32",
                 glibcxx_strict=False, options=None):
        self.root = root
        self.package = package
        self.glibc_max = glibc_max
        self.glibcxx_max = glibcxx_max
        self.glibcxx_strict = glibcxx_strict
        self.options = options or {}

    def path(self, *parts):
        return os.path.join(self.root, *parts)

    def relative(self, path):
        """path relative to the packaged tree when it is inside it, else to the checkout."""
        if self.package and path.startswith(self.package + os.sep):
            return os.path.relpath(path, self.package)
        return os.path.relpath(path, self.root)


# ---- package-stage helpers ---------------------------------------------------------------

def package_files(ctx, *patterns):
    """Regular files (symlinks followed, like the shell's [ -f ]) matching the globs under the
    package tree, in pattern order, each file once."""
    seen = set()
    out = []
    for pattern in patterns:
        for path in sorted(glob.glob(os.path.join(ctx.package, pattern))):
            if os.path.isfile(path) and path not in seen:
                seen.add(path)
                out.append(path)
    return out


# Binaries the portability checks look at. libdecor sits in lib/libdecor so it does not shadow
# a system copy (#197), which puts it outside lib/*.so*.
BIN = "bin/*"
LIB = "lib/*.so*"
PLUGINS = "bin/llplugin/*.so*"
LIBDECOR = ("lib/libdecor/*.so*", "lib/libdecor/plugins-1/*.so")


def _run(tool, args):
    try:
        proc = subprocess.run([tool] + args, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                              text=True, errors="replace", check=False)
    except FileNotFoundError:
        raise ToolMissing(tool)
    return proc.stdout


class ToolMissing(Exception):
    pass


@functools.lru_cache(maxsize=None)
def dynamic_symbols(path):
    """`objdump -T` output (empty for files that are not ELF)."""
    return _run("objdump", ["-T", path])


@functools.lru_cache(maxsize=None)
def elf_notes(path):
    """`readelf -n` output (empty for files that are not ELF)."""
    return _run("readelf", ["-n", path])


def max_version(text, tag_regex):
    """Highest version tag matching tag_regex in text, as (tag, numeric tuple), or None.
    The regex is the same one the old shell check fed to `grep -o`, e.g. r"GLIBC_2\\.[0-9]+"
    (so GLIBC_2.2.5 counts as GLIBC_2.2); tags are compared by their numbers."""
    best = None
    for m in re.finditer(tag_regex, text):
        key = tuple(int(n) for n in re.findall(r"\d+", m.group(0)))
        if best is None or key > best[1]:
            best = (m.group(0), key)
    return best


def version_tuple(text):
    return tuple(int(n) for n in text.split("."))
