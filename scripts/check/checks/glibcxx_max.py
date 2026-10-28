"""Fail (strict) or warn if a packaged binary needs a libstdc++ newer than --glibcxx-max."""
import framework

NAME = "glibcxx-max"
STAGE = "package"
DESCRIPTION = "no packaged binary (incl. llplugin) needs a GLIBCXX newer than GLIBCXX_MAX (default 3.4.32)"
GUARDS = ("#147: the C++ runtime must stay within GCC 13's libstdc++ (what the CI builder "
          "produces); a local build with a newer GCC only warns, CI passes GLIBCXX_STRICT=1")

TAG = r"GLIBCXX_3\.4\.[0-9]+"


def run(ctx, result):
    limit = framework.version_tuple(ctx.glibcxx_max)
    files = framework.package_files(ctx, framework.BIN, framework.LIB, framework.PLUGINS,
                                    *framework.LIBDECOR)
    report = result.fail if ctx.glibcxx_strict else result.warn
    for path in files:
        found = framework.max_version(framework.dynamic_symbols(path), TAG)
        if found is not None and found[1] > limit:
            report("%s needs %s (> %s)%s" % (
                ctx.relative(path), found[0], ctx.glibcxx_max,
                "" if ctx.glibcxx_strict else " (local build; CI enforces this with GLIBCXX_STRICT=1)"))
    result.summary = "%d binaries <= GLIBCXX_%s" % (len(files), ctx.glibcxx_max)
