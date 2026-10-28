"""Fail if a packaged binary declares that it needs x86-64-v4 (AVX-512)."""
import re

import framework

NAME = "isa-v4"
STAGE = "package"
DESCRIPTION = "no packaged binary declares 'ISA needed' x86-64-v4 (AVX-512)"
GUARDS = ("a binary built for the build host's CPU (CachyOS x86-64-v4) will not start on other "
          "people's machines; fix_isa_note.py strips the note from our own binaries")

NEEDED = re.compile(r"ISA needed:.*x86-64-v4")


def run(ctx, result):
    files = framework.package_files(ctx, framework.BIN, framework.LIB, *framework.LIBDECOR)
    for path in files:
        if any(NEEDED.search(line) for line in framework.elf_notes(path).splitlines()):
            result.fail("%s declares it needs x86-64-v4 (AVX-512)" % ctx.relative(path))
    result.summary = "%d binaries, none need v4" % len(files)
