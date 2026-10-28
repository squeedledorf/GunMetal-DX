"""Fail if a packaged binary needs a glibc newer than --glibc-max."""
import framework

NAME = "glibc-max"
STAGE = "package"
DESCRIPTION = "no packaged binary needs a glibc symbol newer than GLIBC_MAX (default 2.38)"
GUARDS = ("tarballs must run on other people's machines (Fedora 39+ / Ubuntu 24.04+); new "
          "re-versioned libm symbols are pinned in indra/cmake/glibc_compat.h")

TAG = r"GLIBC_2\.[0-9]+"


def run(ctx, result):
    limit = framework.version_tuple(ctx.glibc_max)
    files = framework.package_files(ctx, framework.BIN, framework.LIB, *framework.LIBDECOR)
    for path in files:
        text = framework.dynamic_symbols(path)
        found = framework.max_version(text, TAG)
        if found is None or found[1] <= limit:
            continue
        tag = found[0]
        symbols = [line.split()[-1] for line in text.splitlines() if tag + ")" in line]
        result.fail("%s needs %s (> %s): %s" % (ctx.relative(path), tag, ctx.glibc_max,
                                                 " ".join(symbols)))
    result.summary = "%d binaries <= GLIBC_%s" % (len(files), ctx.glibc_max)
