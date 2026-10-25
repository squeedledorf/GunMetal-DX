#!/usr/bin/env python3
"""Build and sign the auto-update manifest for one release (#421).

The manifest is a compact JSON string; the signature is Ed25519 over
b"slipstream-update-v1\\n" + manifest bytes (the same prefix as indra/newview/slupdateverify.h).
The private key comes from the environment or a file OUTSIDE the repository:

    UPDATE_SIGNING_KEY       the PEM text (a Forgejo Actions secret), or
    UPDATE_SIGNING_KEY_FILE  path to a PEM file

Signing uses the `openssl` command line (3.0+), so no Python crypto package is needed.

Commands
  from-release RELEASE_JSON --tag vX.Y.Z[-beta.N]|jetstream-X.Y.Z.N --out update-manifest.json
        RELEASE_JSON is the Forgejo release API object. Artifacts are the release assets
        Slipstream_<ver>_x86_64.tar.xz (linux_tarball) and Slipstream_<ver>_Setup.exe
        (windows_setup); their sha256 comes from the "<sha256>  <name>" lines that
        publish_release.sh writes into the release notes. Missing artifacts are an error.
  sign ARTIFACTS_JSON --version V --build B --channel release|beta|jetstream --out FILE
        Same, from a hand-written {"linux_tarball": {name,url,sha256,size}, ...}.
  verify FILE --pubkey HEX
        Check a produced file (what the viewer does).

Output file: {"manifest": "<json string>", "signature": "<base64>", "key_id": "1"}
which build_update.py (website repo) embeds in the feeds as the "update" object.
"""
import argparse
import base64
import json
import os
import re
import subprocess
import sys
import tempfile
from datetime import datetime, timezone

PREFIX = b"slipstream-update-v1\n"
ARTIFACT_NAMES = {            # feed key -> asset name pattern ({ver} = tag without "v")
    "linux_tarball": "Slipstream_{ver}_x86_64.tar.xz",
    "windows_setup": "Slipstream_{ver}_Setup.exe",
}
TAG = re.compile(r"^v(\d+\.\d+\.\d+)(?:-beta\.(\d+))?$")
JETSTREAM_TAG = re.compile(r"^jetstream-(\d+\.\d+\.\d+)\.(\d+)$")   # scripts/ci/version.sh
BODY_SHA = re.compile(r"^([0-9a-f]{64})\s+(\S+)\s*$", re.M)
BODY_BUILD = re.compile(r"^Slipstream (?:Release|Beta|Jetstream|Test) (\d+\.\d+\.\d+\.\d+)\b", re.M)
HOST_OK = re.compile(r"^https://github\.com/squeedledorf/GunMetal-DX/releases/download/")   # <GMDX> = slupdateapply urlAllowed


def canonical(manifest: dict) -> str:
    return json.dumps(manifest, sort_keys=True, separators=(",", ":"), ensure_ascii=True)


def sign_bytes(message: bytes) -> bytes:
    pem = os.environ.get("UPDATE_SIGNING_KEY", "")
    keyfile = os.environ.get("UPDATE_SIGNING_KEY_FILE", "")
    if not pem and not keyfile:
        sys.exit("sign_update_manifest: set UPDATE_SIGNING_KEY (PEM text) or UPDATE_SIGNING_KEY_FILE")
    with tempfile.TemporaryDirectory(prefix="slipstream-sign-") as tmp:
        os.chmod(tmp, 0o700)
        if pem:
            keyfile = os.path.join(tmp, "key.pem")
            fd = os.open(keyfile, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            with os.fdopen(fd, "w") as f:
                f.write(pem if pem.endswith("\n") else pem + "\n")
        msg = os.path.join(tmp, "msg")
        sig = os.path.join(tmp, "sig")
        with open(msg, "wb") as f:
            f.write(message)
        r = subprocess.run(["openssl", "pkeyutl", "-sign", "-inkey", keyfile, "-rawin", "-in", msg,
                            "-out", sig], capture_output=True)
        if r.returncode != 0:
            sys.exit("sign_update_manifest: openssl failed: " + r.stderr.decode(errors="replace")[:300])
        with open(sig, "rb") as f:
            return f.read()


def build_manifest(version, build, channel, artifacts) -> dict:
    if channel not in ("release", "beta", "jetstream"):
        sys.exit("channel must be release, beta or jetstream")
    if not re.fullmatch(r"\d+\.\d+\.\d+\.\d+", build):
        sys.exit("build must be MAJOR.MINOR.PATCH.BUILD")
    if not artifacts:
        sys.exit("no artifacts")
    for key, a in artifacts.items():
        if set(a) != {"name", "url", "sha256", "size"}:
            sys.exit(f"artifact {key}: need name, url, sha256, size")
        if not HOST_OK.match(a["url"]):
            sys.exit(f"artifact {key}: url must be a GunMetal-DX GitHub release download (https://github.com/squeedledorf/GunMetal-DX/releases/download/...)")
        if not re.fullmatch(r"[0-9a-f]{64}", a["sha256"]) or not (isinstance(a["size"], int) and a["size"] > 0):
            sys.exit(f"artifact {key}: bad sha256/size")
        if not re.fullmatch(r"[A-Za-z0-9._+-]{1,120}", a["name"]):
            sys.exit(f"artifact {key}: bad name")
    return {
        "schema": 1, "channel": channel, "version": version, "build": build,
        "issued": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "artifacts": artifacts,
    }


def write_signed(manifest: dict, out: str):
    text = canonical(manifest)
    sig = sign_bytes(PREFIX + text.encode())
    doc = {"manifest": text, "signature": base64.b64encode(sig).decode(), "key_id": "1"}
    tmp = out + ".tmp"
    with open(tmp, "w") as f:
        json.dump(doc, f, indent=2)
        f.write("\n")
    os.replace(tmp, out)
    print(f"signed manifest for {manifest['build']} ({manifest['channel']}): {', '.join(sorted(manifest['artifacts']))}")


def cmd_from_release(args):
    with open(args.release_json) as f:
        rel = json.load(f)
    m = TAG.match(args.tag)
    jm = JETSTREAM_TAG.match(args.tag)
    if m:
        ver = args.tag[1:]                                   # files: Slipstream_1.4.0-beta.5_*
        version, channel = m.group(1), ("beta" if m.group(2) else "release")
    elif jm:
        ver = f"{jm.group(1)}-jetstream.{jm.group(2)}"      # files: Slipstream_1.5.0-jetstream.12_*
        version, channel = jm.group(1), "jetstream"
    else:
        sys.exit("bad tag " + args.tag)
    body = rel.get("body") or ""
    sums = {name: digest for digest, name in BODY_SHA.findall(body)}
    bm = BODY_BUILD.search(body)
    if not bm:
        sys.exit("release text has no 'Slipstream <Kind> <build>' first line")
    assets = {a["name"]: a for a in rel.get("assets") or []}
    artifacts = {}
    for key, pat in ARTIFACT_NAMES.items():
        name = pat.format(ver=ver)
        a = assets.get(name)
        if not a:
            sys.exit(f"release {args.tag} has no asset {name}")
        if name not in sums:
            sys.exit(f"release notes have no sha256 line for {name}")
        artifacts[key] = {"name": name, "url": a["browser_download_url"], "sha256": sums[name],
                          "size": int(a["size"])}
    write_signed(build_manifest(version, bm.group(1), channel, artifacts), args.out)


def cmd_sign(args):
    with open(args.artifacts_json) as f:
        artifacts = json.load(f)
    write_signed(build_manifest(args.version, args.build, args.channel, artifacts), args.out)


def cmd_verify(args):
    with open(args.file) as f:
        doc = json.load(f)
    with tempfile.TemporaryDirectory(prefix="slipstream-verify-") as tmp:
        pub = bytes.fromhex(args.pubkey)
        if len(pub) != 32:
            sys.exit("pubkey must be 64 hex chars")
        der = bytes.fromhex("302a300506032b6570032100") + pub      # SubjectPublicKeyInfo, Ed25519
        pk = os.path.join(tmp, "pub.der")
        with open(pk, "wb") as f:
            f.write(der)
        msg, sig = os.path.join(tmp, "m"), os.path.join(tmp, "s")
        with open(msg, "wb") as f:
            f.write(PREFIX + doc["manifest"].encode())
        with open(sig, "wb") as f:
            f.write(base64.b64decode(doc["signature"]))
        r = subprocess.run(["openssl", "pkeyutl", "-verify", "-pubin", "-keyform", "DER", "-inkey", pk,
                            "-rawin", "-in", msg, "-sigfile", sig], capture_output=True)
        if r.returncode != 0:
            sys.exit("signature INVALID")
    print("signature ok")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("from-release")
    p.add_argument("release_json")
    p.add_argument("--tag", required=True)
    p.add_argument("--out", required=True)
    p.set_defaults(fn=cmd_from_release)
    p = sub.add_parser("sign")
    p.add_argument("artifacts_json")
    p.add_argument("--version", required=True)
    p.add_argument("--build", required=True)
    p.add_argument("--channel", required=True)
    p.add_argument("--out", required=True)
    p.set_defaults(fn=cmd_sign)
    p = sub.add_parser("verify")
    p.add_argument("file")
    p.add_argument("--pubkey", required=True)
    p.set_defaults(fn=cmd_verify)
    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
