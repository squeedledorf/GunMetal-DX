#!/usr/bin/env python3
"""Run the Slipstream regression checks (the one place regression guards live).

  python3 -I scripts/check/run.py --stage source
  python3 -I scripts/check/run.py --stage package --package build-linux-x86_64/newview/packaged
  python3 -I scripts/check/run.py --list
  python3 -I scripts/check/run.py --only glibc-max --package <dir>

`make check` and `make check-package` wrap the first two; CI runs those. A check is one module
in scripts/check/checks/ (see scripts/check/framework.py); to add a regression guard, add a
module there -- never a workflow step or a Makefile loop. Exit status is non-zero when any
check fails; warnings are printed but do not fail the run.
"""

import argparse
import importlib
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)          # -I does not add the script directory
sys.dont_write_bytecode = True

import framework  # noqa: E402

REQUIRED = ("NAME", "STAGE", "DESCRIPTION", "GUARDS", "run")


def discover():
    checks = []
    names = sorted(f[:-3] for f in os.listdir(os.path.join(HERE, "checks"))
                   if f.endswith(".py") and not f.startswith("_"))
    for module_name in names:
        mod = importlib.import_module("checks." + module_name)
        missing = [a for a in REQUIRED if not hasattr(mod, a)]
        if missing:
            sys.exit("check module %s lacks %s" % (module_name, ", ".join(missing)))
        if mod.STAGE not in framework.STAGES:
            sys.exit("check %s has unknown stage %r" % (mod.NAME, mod.STAGE))
        checks.append(mod)
    seen = {}
    for mod in checks:
        if mod.NAME in seen:
            sys.exit("two checks are named %s" % mod.NAME)
        seen[mod.NAME] = mod
    return sorted(checks, key=lambda m: (framework.STAGES.index(m.STAGE), m.NAME))


def env_flag(name):
    return os.environ.get(name, "0").strip().lower() in ("1", "true", "yes", "on")


def parse_args():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--stage", choices=framework.STAGES, help="run every check of this stage")
    p.add_argument("--only", action="append", default=[], metavar="NAME",
                   help="run just this check (repeatable; any stage)")
    p.add_argument("--list", action="store_true", help="list the checks and exit")
    p.add_argument("--root", default=os.path.dirname(os.path.dirname(HERE)), help="repository root")
    p.add_argument("--package", default=os.environ.get("CHECK_PACKAGE"),
                   help="packaged viewer tree for the package stage (env CHECK_PACKAGE)")
    p.add_argument("--glibc-max", default=os.environ.get("GLIBC_MAX", "2.38"))
    p.add_argument("--glibcxx-max", default=os.environ.get("GLIBCXX_MAX", "3.4.32"))
    p.add_argument("--glibcxx-strict", action="store_true", default=env_flag("GLIBCXX_STRICT"),
                   help="a libstdc++ version above --glibcxx-max fails (otherwise it only warns)")
    p.add_argument("--opt", action="append", default=[], metavar="KEY=VALUE",
                   help="check-specific option, e.g. list_raw=1 for key-commands")
    return p.parse_args()


def print_list(checks):
    for mod in checks:
        print("%-8s %-16s %s" % (mod.STAGE, mod.NAME, mod.DESCRIPTION))
        print("%-8s %-16s guards: %s" % ("", "", mod.GUARDS))


def main():
    args = parse_args()
    checks = discover()
    if args.list:
        print_list(checks)
        return 0

    by_name = {m.NAME: m for m in checks}
    unknown = [n for n in args.only if n not in by_name]
    if unknown:
        print("unknown check(s): %s (see --list)" % ", ".join(unknown), file=sys.stderr)
        return 2
    if args.only:
        selected = [by_name[n] for n in args.only]
    elif args.stage:
        selected = [m for m in checks if m.STAGE == args.stage]
    else:
        print("give --stage, --only or --list", file=sys.stderr)
        return 2

    options = dict(o.split("=", 1) if "=" in o else (o, "1") for o in args.opt)
    ctx = framework.Context(os.path.abspath(args.root),
                            os.path.abspath(args.package) if args.package else None,
                            args.glibc_max, args.glibcxx_max, args.glibcxx_strict, options)

    rows = []
    for mod in selected:
        result = framework.Result()
        if mod.STAGE == "package" and not (ctx.package and os.path.isdir(ctx.package)):
            result.fail("packaged tree not found (%s); pass --package DIR or build first"
                        % (ctx.package or "no --package given"))
        else:
            try:
                mod.run(ctx, result)
            except framework.ToolMissing as e:
                result.fail("required tool not installed: %s" % e)
        print("== %s (%s): %s" % (mod.NAME, mod.STAGE, mod.DESCRIPTION))
        for line in result.failures:
            print("  FAIL  " + line)
        for line in result.warnings:
            print("  WARN  " + line)
        status = "FAIL" if result.failures else ("WARN" if result.warnings else "PASS")
        rows.append((status, mod, result))

    print()
    print("%-5s %-8s %-16s %s" % ("", "STAGE", "CHECK", "DETAIL"))
    for status, mod, result in rows:
        detail = result.summary
        if result.failures:
            detail = "%d problem(s)" % len(result.failures)
        elif result.warnings:
            detail = ("%s; " % detail if detail else "") + "%d warning(s)" % len(result.warnings)
        print("%-5s %-8s %-16s %s" % (status, mod.STAGE, mod.NAME, detail))
    failed = [m.NAME for s, m, _ in rows if s == "FAIL"]
    print()
    if failed:
        print("check: FAILED: %s" % ", ".join(failed))
        return 1
    print("check: %d check(s) passed" % len(rows))
    return 0


if __name__ == "__main__":
    sys.exit(main())
