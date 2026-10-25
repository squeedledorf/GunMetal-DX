#!/bin/bash
# End-to-end check of the update signing chain (#421): gen_update_key.sh ->
# sign_update_manifest.py -> the viewer's verifier (indra/newview/slupdateverify.cpp, built
# standalone against OpenSSL). Uses throwaway keys in a temp dir; touches nothing else.
# usage: scripts/updater/test_update_verify.sh
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

c++ -std=c++17 -Wall -Werror -I"$ROOT/indra/newview" -o "$T/test" \
    "$HERE/test_update_verify.cpp" "$ROOT/indra/newview/slupdateverify.cpp" -lcrypto

# gen_update_key.sh refuses repository paths, so keys live in $T (outside the tree).
pub=$("$HERE/gen_update_key.sh" "$T/key.pem" | sed -n 's/^public key (hex): //p')
other=$("$HERE/gen_update_key.sh" "$T/key2.pem" | sed -n 's/^public key (hex): //p')
[ ${#pub} -eq 64 ] && [ ${#other} -eq 64 ] && [ "$pub" != "$other" ]
[ "$(stat -c %a "$T/key.pem")" = 600 ]
if "$HERE/gen_update_key.sh" "$T/key.pem" 2>/dev/null; then echo "overwrote a key" >&2; exit 1; fi
mkdir -p "$ROOT/.updater-test-inside" && trap 'rm -rf "$T" "$ROOT/.updater-test-inside"' EXIT
if "$HERE/gen_update_key.sh" "$ROOT/.updater-test-inside/k.pem" 2>/dev/null; then
  echo "accepted a path inside the repository" >&2; exit 1
fi

head -c 100000 /dev/urandom > "$T/artifact.bin"
sha=$(sha256sum "$T/artifact.bin" | cut -d' ' -f1)
cat > "$T/artifacts.json" <<JSON
{"linux_tarball": {"name": "Slipstream_1.2.0_x86_64.tar.xz",
  "url": "https://github.com/squeedledorf/GunMetal-DX/releases/download/v1.2.0/GunMetal-DX_1.2.0_x86_64.tar.xz",
  "sha256": "$sha", "size": 100000}}
JSON

# no key -> refuses
if env -u UPDATE_SIGNING_KEY -u UPDATE_SIGNING_KEY_FILE python3 "$HERE/sign_update_manifest.py" sign \
     "$T/artifacts.json" --version 1.2.0 --build 1.2.0.3 --channel release --out "$T/m.json" 2>/dev/null; then
  echo "signed without a key" >&2; exit 1
fi
# key from the environment (the CI path)
UPDATE_SIGNING_KEY="$(cat "$T/key.pem")" python3 "$HERE/sign_update_manifest.py" sign \
  "$T/artifacts.json" --version 1.2.0 --build 1.2.0.3 --channel release --out "$T/m.json"
python3 "$HERE/sign_update_manifest.py" verify "$T/m.json" --pubkey "$pub"
if python3 "$HERE/sign_update_manifest.py" verify "$T/m.json" --pubkey "$other" 2>/dev/null; then
  echo "verified with the wrong key" >&2; exit 1
fi
# bad artifact host / hash are refused before signing
sed 's#github.com/squeedledorf#evil.example/squeedledorf#' "$T/artifacts.json" > "$T/evil.json"
if UPDATE_SIGNING_KEY_FILE="$T/key.pem" python3 "$HERE/sign_update_manifest.py" sign "$T/evil.json" \
     --version 1.2.0 --build 1.2.0.3 --channel release --out "$T/e.json" 2>/dev/null; then
  echo "signed a foreign host" >&2; exit 1
fi

"$T/test" "$T/m.json" "$pub" "$other" "$T/artifact.bin" "$sha"
echo "update signing chain OK"
