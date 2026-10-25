#!/bin/bash
# Checks the viewer's certificate revocation code (indra/newview/slrevocation.cpp, built
# standalone against OpenSSL, zlib and the Boost.JSON headers) and its data files.
#
# Offline: the shipped distrust list and baseline crl-set parse.
# Online (default; --offline skips): fetches a fresh CRLSet CRX3 with fetch_crlset.py and
# checks the C++ CRX3 verifier/unpacker on it (including tampered / wrong-id copies), then
# fetches the chains of the twelve emudhra.we-have-a-problem.com test hosts plus a control
# host, verifies each like LLBasicCertificateStore does (the viewer's ca-bundle.crt) and
# requires every chain that verifies to be REVOKED by: the CRLSet, the static distrust list
# alone, and the CRLSet's per-issuer serials alone (BlockedSPKIs stripped). The control host
# must stay OK.
#
# usage: scripts/revocation/test_revocation.sh [--offline] [--ca-bundle FILE] [--boost-include DIR]
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OFFLINE=0
CA="$ROOT/build-linux-x86_64/newview/packaged/bin/ca-bundle.crt"
BOOST_INC="$ROOT/build-linux-x86_64/packages/include"
while [ $# -gt 0 ]; do
  case "$1" in
    --offline) OFFLINE=1 ;;
    --ca-bundle) CA="$2"; shift ;;
    --boost-include) BOOST_INC="$2"; shift ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
  shift
done
[ -f "$CA" ] || CA=/etc/ssl/certs/ca-certificates.crt
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

# Boost headers come after the system include path so the system OpenSSL headers win.
c++ -std=c++20 -O1 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-maybe-uninitialized -DSL_REVOCATION_STANDALONE \
    -I"$ROOT/indra/newview" -idirafter "$BOOST_INC" -o "$T/test" \
    "$HERE/test_revocation.cpp" "$ROOT/indra/newview/slrevocation.cpp" -lcrypto -lz

DATA="$ROOT/indra/newview/app_settings/revocation"
"$T/test" distrust "$DATA/distrusted_spki.txt"
"$T/test" crlset "$DATA/crl-set"
python3 -I "$HERE/fetch_crlset.py" --info "$DATA/crl-set"
[ "$OFFLINE" = 1 ] && { echo "offline checks OK"; exit 0; }

# --- CRX3 path on a fresh download -------------------------------------------------------
python3 -I - "$T/omaha.xml" "$T/fresh.crx3" <<'PY'
import sys, re, urllib.request
omaha = ("https://clients2.google.com/service/update2/crx?x=id%3Dhfnkpimlhhgieaddgfemjhofmfblmnib"
         "%26v%3D%26uc%26acceptformat%3Dcrx3&tag=force_full")
xml = urllib.request.urlopen(omaha, timeout=60).read()
open(sys.argv[1], "wb").write(xml)
url = re.search(rb'codebase="([^"]+)"', xml).group(1).decode().replace("&amp;", "&")
open(sys.argv[2], "wb").write(urllib.request.urlopen(url, timeout=120).read())
PY
"$T/test" omaha "$T/omaha.xml"
"$T/test" crx "$T/fresh.crx3"
python3 -I "$HERE/fetch_crlset.py" --cohort full --out "$T/full-crl-set"
"$T/test" crlset "$T/full-crl-set"

# Same CRLSet with BlockedSPKIs removed: only the per-issuer serial lists can revoke.
python3 -I - "$T/full-crl-set" "$T/serials-only" <<'PY'
import sys, struct, json
d = open(sys.argv[1], "rb").read()
(hl,) = struct.unpack("<H", d[:2])
h = json.loads(d[2:2 + hl])
for k in ("BlockedSPKIs", "BlockedInterceptionSPKIs", "KnownInterceptionSPKIs"):
    h.pop(k, None)
nh = json.dumps(h, separators=(",", ":")).encode()
open(sys.argv[2], "wb").write(struct.pack("<H", len(nh)) + nh + d[2 + hl:])
PY

# --- the twelve test hosts ---------------------------------------------------------------
fetch_chain() {  # host out.pem
  timeout 20 openssl s_client -connect "$1:443" -servername "$1" -showcerts </dev/null 2>/dev/null |
    awk '/BEGIN CERT/{p=1} p{print} /END CERT/{p=0}' > "$2"
}
fail=0
for p in dv-g3b dv-g3c ov-g3b ov-g3c ev-g3b ev-g3c dv-g3b-aaa dv-g3c-aaa ov-g3b-aaa ov-g3c-aaa ev-g3b-aaa ev-g3c-aaa; do
  host="$p-emudhra.we-have-a-problem.com"
  fetch_chain "$host" "$T/$p.pem"
  if [ ! -s "$T/$p.pem" ]; then echo "$host: could not fetch the chain"; fail=1; continue; fi
  base=$("$T/test" verify "$CA" - - "$T/$p.pem" "$host" | tail -1 || true)
  case "$base" in
    UNTRUSTED*) echo "$host: not trusted by $(basename "$CA") at all ($base) - nothing to revoke"; continue ;;
  esac
  for mode in crlset distrust serials; do
    case $mode in
      crlset)   args=("$T/full-crl-set" -) ;;
      distrust) args=(- "$DATA/distrusted_spki.txt") ;;
      serials)  args=("$T/serials-only" -) ;;
    esac
    out=$("$T/test" verify "$CA" "${args[@]}" "$T/$p.pem" "$host" | tail -1 || true)
    case "$out" in
      REVOKED*) echo "$host [$mode]: $out" ;;
      *) echo "$host [$mode]: NOT REVOKED: $out"; fail=1 ;;
    esac
  done
done
# A 'serial' distrust entry (issuer = emSign Root TLS CA - G3, serial as openssl prints it).
if [ -s "$T/dv-g3b.pem" ]; then
  printf 'serial c619e4780ed8e0abeda24ad732567de4383a7ea941338f6d1a7d1b49ac792fe9 A4AEE678F23362B4C0E460B772F214  # EM DV TLS CA - G3B\n' > "$T/serial-list.txt"
  out=$("$T/test" verify "$CA" - "$T/serial-list.txt" "$T/dv-g3b.pem" | tail -1 || true)
  echo "dv-g3b [distrust serial entry]: $out"
  case "$out" in *"serial is on the static distrust list"*) ;; *) fail=1 ;; esac
fi
fetch_chain secondlife.com "$T/control.pem"
out=$("$T/test" verify "$CA" "$T/full-crl-set" "$DATA/distrusted_spki.txt" "$T/control.pem" secondlife.com | tail -1 || true)
echo "secondlife.com (control): $out"
[ "$out" = OK ] || fail=1
[ "$fail" = 0 ] && echo "revocation checks OK" || { echo "revocation checks FAILED"; exit 1; }
