"""Warn if the packaged libdecor (copied from the build host) was compiled for x86-64-v3/v4."""
import re

import framework

NAME = "libdecor-isa"
STAGE = "package"
DESCRIPTION = "warn if the packaged libdecor was compiled for x86-64-v3/v4 (never fails)"
GUARDS = ("#197: libdecor is copied from the build host, so a tarball for other machines needs a "
          "generic build (SLIPSTREAM_LIBDECOR_LIBDIR, see viewer_manifest.py)")

USED = re.compile(r"ISA used:.*x86-64-v[34]")


def run(ctx, result):
    files = framework.package_files(ctx, *framework.LIBDECOR)
    for path in files:
        if any(USED.search(line) for line in framework.elf_notes(path).splitlines()):
            result.warn("%s (copied from this build host) was compiled for x86-64-v3/v4; a tarball "
                        "for other machines needs a generic build (SLIPSTREAM_LIBDECOR_LIBDIR, "
                        "see viewer_manifest.py)" % ctx.relative(path))
    result.summary = "%d libdecor files" % len(files)
