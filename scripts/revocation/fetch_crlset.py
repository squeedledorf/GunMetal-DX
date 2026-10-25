#!/usr/bin/env python3
"""Fetch Chrome's current CRLSet and write it as a plain `crl-set` file.

The viewer ships one (indra/newview/app_settings/revocation/crl-set) so a fresh install
has revocation data before its first runtime fetch (slrevocationupdater.cpp), and hands
the newest one it has to the embedded browser (CEF has no component updater).

  fetch_crlset.py --out indra/newview/app_settings/revocation/crl-set [--cohort low|full]
  fetch_crlset.py --info <crl-set>

Flow (the same as the viewer's): ask Google's update service (Omaha, update2/crx) for
component hfnkpimlhhgieaddgfemjhofmfblmnib, download the CRX3 it names, check its SHA-256
against the reply, verify the CRX3 signature by the key whose CRX id is that component id
(RSA PKCS#1 v1.5 / ECDSA with SHA-256, via the openssl command line), unpack `crl-set`
from the ZIP, parse its header, and refuse to replace an existing file with an older
sequence. The output is written atomically. Stdlib + the `openssl` binary only.

The committed baseline uses --cohort low (~32 KB): it carries the same BlockedSPKIs as the
full desktop set (~1.2 MB), only fewer per-issuer serials, so refreshing it in git stays
cheap. Release CI fetches --cohort full into the build before packaging.
"""
import argparse
import hashlib
import io
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
import time
import urllib.request
import zipfile

APP_ID = "hfnkpimlhhgieaddgfemjhofmfblmnib"
OMAHA = ("https://clients2.google.com/service/update2/crx?x=id%3D" + APP_ID +
         "%26v%3D%26uc%26acceptformat%3Dcrx3")
UA = "Slipstream-CRLSet-fetch/1"


def http_get(url, limit=16 * 1024 * 1024):
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=60) as resp:
        data = resp.read(limit + 1)
    if len(data) > limit:
        raise SystemExit("response from %s is too large" % url)
    return data


def parse_header(crl):
    """Header of a CRLSet (Chromium net/cert/crl_set.cc: uint16le length + JSON)."""
    if len(crl) < 2:
        raise ValueError("too short")
    (hl,) = struct.unpack("<H", crl[:2])
    if len(crl) < 2 + hl:
        raise ValueError("truncated header")
    hdr = json.loads(crl[2:2 + hl])
    if hdr.get("ContentType") != "CRLSet" or hdr.get("Version") != 0 or not isinstance(hdr.get("Sequence"), int):
        raise ValueError("not a version-0 CRLSet")
    if hdr.get("DeltaFrom", 0):
        raise ValueError("delta CRLSet (DeltaFrom) not supported")
    # walk the body: [32 parent][u32le n][n x (u8 len, serial)]
    p, parents, serials = 2 + hl, 0, 0
    while p < len(crl):
        if p + 36 > len(crl):
            raise ValueError("truncated CRL entry")
        (n,) = struct.unpack("<I", crl[p + 32:p + 36])
        p += 36
        for _ in range(n):
            if p >= len(crl) or p + 1 + crl[p] > len(crl):
                raise ValueError("truncated serial")
            p += 1 + crl[p]
        parents += 1
        serials += n
    hdr["_parents"], hdr["_serials"] = parents, serials
    return hdr


def varint(buf, p):
    v, shift = 0, 0
    while True:
        b = buf[p]
        p += 1
        v |= (b & 0x7F) << shift
        if not b & 0x80:
            return v, p
        shift += 7
        if shift > 63:
            raise ValueError("bad varint")


def proto_fields(buf):
    """(field, bytes) for the length-delimited fields of a protobuf message."""
    p, out = 0, []
    while p < len(buf):
        key, p = varint(buf, p)
        wt = key & 7
        if wt == 0:
            _, p = varint(buf, p)
        elif wt == 1:
            p += 8
        elif wt == 5:
            p += 4
        elif wt == 2:
            n, p = varint(buf, p)
            out.append((key >> 3, buf[p:p + n]))
            p += n
        else:
            raise ValueError("bad wire type")
    return out


def crx_id(spki):
    return "".join(chr(ord("a") + int(c, 16)) for c in hashlib.sha256(spki).hexdigest()[:32])


def verify_crx3(crx):
    """Returns the ZIP archive of a CRX3 signed by the APP_ID key (components/crx_file)."""
    if crx[:4] != b"Cr24" or struct.unpack("<I", crx[4:8])[0] != 3:
        raise SystemExit("not a CRX3 file")
    (hl,) = struct.unpack("<I", crx[8:12])
    header, archive = crx[12:12 + hl], crx[12 + hl:]
    proofs, shd = [], None
    for field, data in proto_fields(header):
        if field in (2, 3):
            d = dict(proto_fields(data))
            proofs.append((d.get(1, b""), d.get(2, b"")))
        elif field == 10000:
            shd = data
    if shd is None:
        raise SystemExit("CRX3 has no signed header")
    declared = dict(proto_fields(shd)).get(1, b"")
    if len(declared) != 16 or "".join(chr(ord("a") + int(c, 16)) for c in declared.hex()) != APP_ID:
        raise SystemExit("CRX3 is not for " + APP_ID)
    signed = b"CRX3 SignedData\x00" + struct.pack("<I", len(shd)) + shd + archive
    with tempfile.TemporaryDirectory() as tmp:
        msg = os.path.join(tmp, "msg")
        with open(msg, "wb") as f:
            f.write(signed)
        for key, sig in proofs:
            if not key or not sig or crx_id(key) != APP_ID:
                continue
            kf, sf = os.path.join(tmp, "key.der"), os.path.join(tmp, "sig")
            with open(kf, "wb") as f:
                f.write(key)
            with open(sf, "wb") as f:
                f.write(sig)
            r = subprocess.run(["openssl", "dgst", "-sha256", "-keyform", "DER", "-verify", kf,
                                "-signature", sf, msg], capture_output=True)
            if r.returncode == 0:
                return archive
    raise SystemExit("no valid CRX3 signature by the %s key" % APP_ID)


def fetch(cohort):
    url = OMAHA + ("&tag=force_full" if cohort == "full" else "")
    xml = http_get(url, 1024 * 1024).decode("utf-8", "replace")
    m = re.search(r'<app[^>]*appid="%s"[^>]*>(.*?)</app>' % APP_ID, xml, re.S)
    uc = re.search(r"<updatecheck([^>]*)>", m.group(1)) if m else None
    if not uc:
        raise SystemExit("unexpected Omaha reply: " + xml[:300])
    attrs = dict(re.findall(r'(\w+)="([^"]*)"', uc.group(1)))
    if attrs.get("status") != "ok" or not attrs.get("codebase"):
        raise SystemExit("Omaha: no update offered: " + uc.group(0))
    codebase = attrs["codebase"].replace("&amp;", "&")
    if codebase.startswith("http://"):
        codebase = "https://" + codebase[len("http://"):]
    crx = http_get(codebase)
    want = attrs.get("hash_sha256", "").lower()
    if want and hashlib.sha256(crx).hexdigest() != want:
        raise SystemExit("CRX SHA-256 does not match the Omaha reply")
    archive = verify_crx3(crx)
    crl = zipfile.ZipFile(io.BytesIO(archive)).read("crl-set")
    return crl, attrs.get("version", "?")


def describe(hdr):
    na = hdr.get("NotAfter") or 0
    return ("sequence %d, %d blocked SPKIs (+%d interception), %d issuers, %d serials, NotAfter %s"
            % (hdr["Sequence"], len(hdr.get("BlockedSPKIs") or []), len(hdr.get("BlockedInterceptionSPKIs") or []),
               hdr["_parents"], hdr["_serials"],
               time.strftime("%Y-%m-%d %H:%M UTC", time.gmtime(na)) if na else "none"))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", help="write the crl-set here")
    ap.add_argument("--cohort", choices=("full", "low"), default="full",
                    help="full = desktop set (~1.2 MB, default); low = small default cohort (~32 KB)")
    ap.add_argument("--info", metavar="CRLSET", help="only describe an existing crl-set file")
    ap.add_argument("--retries", type=int, default=3)
    args = ap.parse_args()

    if args.info:
        with open(args.info, "rb") as f:
            print(describe(parse_header(f.read())))
        return 0
    if not args.out:
        ap.error("--out or --info is required")

    last = None
    for attempt in range(max(1, args.retries)):
        try:
            crl, version = fetch(args.cohort)
            break
        except SystemExit:
            raise
        except Exception as e:  # network trouble: retry
            last = e
            time.sleep(5 * (attempt + 1))
    else:
        raise SystemExit("CRLSet fetch failed: %s" % last)

    hdr = parse_header(crl)
    if os.path.exists(args.out):
        try:
            with open(args.out, "rb") as f:
                old = parse_header(f.read())
            if old["Sequence"] > hdr["Sequence"]:
                raise SystemExit("refusing to replace sequence %d with older %d" % (old["Sequence"], hdr["Sequence"]))
        except ValueError:
            pass  # the existing file is unreadable: replace it
    tmp = args.out + ".tmp"
    with open(tmp, "wb") as f:
        f.write(crl)
    os.replace(tmp, args.out)
    print("%s: %s (%s cohort, component version %s)" % (args.out, describe(hdr), args.cohort, version))
    return 0


if __name__ == "__main__":
    sys.exit(main())
