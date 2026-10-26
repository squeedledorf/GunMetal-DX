#!/usr/bin/env python3
"""Release gate: publish a draft release only when every needed job succeeded and every
expected asset is attached; otherwise fail with the list of what is missing and leave the
draft unpublished (nothing public happened, so the tag can be moved and CI re-run).

usage: release_gate.py TAG [--signing] [--update-manifest] [--job NAME=RESULT ...] [--dry-run]
  --signing          LINUX_SIGNING_KEY is configured: expect the tarball .sig (v* tags),
                     SHA256SUMS and SHA256SUMS.asc, and that SHA256SUMS lists every asset
  --update-manifest  UPDATE_SIGNING_KEY is configured: expect update-manifest.json
  --job NAME=RESULT  a needed job and its result; anything but "success" blocks the release
  --dry-run          check only, never PATCH (also used to test against real releases)
env: RELEASE_API (.../api/v1/repos/<owner>/<repo>), TOKEN

Expected assets (names follow scripts/ci/version.sh ARTIFACT_BASE):
  v* (release / beta): <base>_x86_64.tar.xz [+ .sig], <base>_win64.zip, <base>_Setup.exe,
      <base>_amd64.deb, <base>.flatpak, slipstream-bin-*-x86_64.pkg.tar.zst, PKGBUILD,
      SHA256SUMS [+ .asc], update-manifest.json when configured
  jetstream-*: tarball, win64.zip, Setup.exe, SHA256SUMS [+ .asc], update-manifest.json when configured
"""
import os
import re
import sys
import tempfile
import urllib.error

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import release_lookup as rl  # noqa: E402


def artifact_base(tag):
    """Slipstream_<LABEL>: version.sh's LABEL is the tag minus "v", or X.Y.Z-jetstream.N."""
    m = re.match(r"^jetstream-(\d+\.\d+\.\d+)\.(\d+)$", tag)
    if m:
        return f"Slipstream_{m.group(1)}-jetstream.{m.group(2)}", "jetstream"
    if re.match(r"^v\d+\.\d+\.\d+(-beta\.\d+)?$", tag):
        return f"Slipstream_{tag[1:]}", "beta" if "-beta." in tag else "release"
    raise SystemExit(f"release_gate: {tag} is not a gated tag (vX.Y.Z[-beta.N] / jetstream-X.Y.Z.N)")


def expected(tag, signing, update_manifest):
    """[(label, predicate(name) -> bool)] one entry per required asset."""
    base, kind = artifact_base(tag)
    exact = lambda n: (n, lambda x, n=n: x == n)  # noqa: E731
    out = [exact(f"{base}_x86_64.tar.xz"), exact(f"{base}_win64.zip"), exact(f"{base}_Setup.exe")]
    if kind != "jetstream":
        out += [exact(f"{base}_amd64.deb"), exact(f"{base}.flatpak"), exact("PKGBUILD"),
                ("slipstream-bin-*-x86_64.pkg.tar.zst",
                 lambda x: x.startswith("slipstream-bin-") and x.endswith("-x86_64.pkg.tar.zst"))]
        if signing:
            out.append(exact(f"{base}_x86_64.tar.xz.sig"))
    if signing:
        out += [exact("SHA256SUMS"), exact("SHA256SUMS.asc")]
    if update_manifest:
        out.append(exact("update-manifest.json"))
    return out


def check(rel, tag, signing, update_manifest, api=None, token=""):
    """List of problems with the release's asset set (empty = complete)."""
    assets = {a["name"]: a for a in rel.get("assets") or []}
    problems = []
    for label, pred in expected(tag, signing, update_manifest):
        hit = [a for n, a in assets.items() if pred(n)]
        if not hit:
            problems.append(f"missing asset: {label}")
        elif all(a.get("size", 1) == 0 for a in hit):
            problems.append(f"empty asset: {label}")
    if signing and "SHA256SUMS" in assets and api:
        # sign-release must have run after the last asset landed.
        with tempfile.NamedTemporaryFile() as f:
            rl.fetch_asset(api, token, assets["SHA256SUMS"], f.name)
            listed = {ln.split(None, 1)[1].strip() for ln in open(f.name) if len(ln.split(None, 1)) == 2}
        for n in sorted(assets):
            if n not in ("SHA256SUMS", "SHA256SUMS.asc") and n not in listed:
                problems.append(f"SHA256SUMS does not list {n} (sign-release ran before it was attached)")
    return problems


def main(argv):
    args = argv[1:]
    if not args or args[0].startswith("-"):
        print(__doc__, file=sys.stderr)
        return 2
    tag, signing, um, dry, jobs = args[0], False, False, False, []
    it = iter(args[1:])
    for a in it:
        if a == "--signing":
            signing = True
        elif a == "--update-manifest":
            um = True
        elif a == "--dry-run":
            dry = True
        elif a == "--job":
            jobs.append(next(it))
        else:
            print(f"release_gate: unknown argument {a}", file=sys.stderr)
            return 2
    api, token = os.environ["RELEASE_API"], os.environ.get("TOKEN", "")

    problems = []
    for j in jobs:
        name, _, result = j.partition("=")
        if result != "success":
            problems.append(f"job {name}: {result or 'unknown'} (needs to be success)")
    rel = rl.find_release(api, token, tag)
    if rel is None:
        problems.append(f"no release (draft or published) exists for {tag}")
    else:
        problems += check(rel, tag, signing, um, api, token)

    if problems:
        print(f"::error::release gate: {tag} is NOT published. " + "; ".join(problems))
        for p in problems:
            print(f"  - {p}")
        if rel is not None:
            print(f"The release stays {'a draft' if rel.get('draft') else 'as it is'}. Recovery: fix the cause on the "
                  f"branch, delete the draft release {tag}, move the tag and push it "
                  "(git tag -f TAG COMMIT; git push -f origin TAG) -- see the header of .forgejo/workflows/release.yml.")
        return 1

    if not rel.get("draft"):
        print(f"release gate: {tag} is already published; all checks pass")
        return 0
    if dry:
        print(f"release gate (dry run): {tag} is complete and would be published (prerelease={rel['prerelease']})")
        return 0
    rl.api_json(api, token, f"/releases/{rel['id']}", "PATCH",
                {"draft": False, "prerelease": bool(rel["prerelease"])})
    print(f"release gate: {tag} complete -- published (prerelease={rel['prerelease']})")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv))
    except urllib.error.HTTPError as e:
        print(f"::error::release gate: HTTP {e.code} {e.url}")
        sys.exit(1)
