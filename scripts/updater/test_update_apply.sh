#!/bin/bash
# Tests the Linux apply step (indra/newview/slupdateapply.cpp) and the launcher hooks
# (indra/newview/linux_tools/update_hooks.sh) on a fake install tree, with throwaway keys.
# Needs Boost headers (src.hpp): set BOOST_INCLUDE=<dir containing boost/> if they are not
# in the default include path. Skips (exit 0) when none are found.
# usage: scripts/updater/test_update_apply.sh
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
NV="$ROOT/indra/newview"
INC=()
if [ -n "${BOOST_INCLUDE:-}" ]; then INC=(-I"$BOOST_INCLUDE"); fi
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

pub=$("$HERE/gen_update_key.sh" "$T/key.pem" | sed -n 's/^public key (hex): //p')
other=$("$HERE/gen_update_key.sh" "$T/key2.pem" | sed -n 's/^public key (hex): //p')

if ! c++ -std=c++17 -DSLIPSTREAM_UPDATE_PUBKEY_HEX="\"$pub\"" -Wall -Werror -I"$NV" "${INC[@]}" -o "$T/apply" "$HERE/test_update_apply.cpp" \
      "$NV/slupdateapply.cpp" "$NV/slupdateverify.cpp" -lcrypto 2>"$T/cc.log"; then
  if grep -q "boost/json" "$T/cc.log"; then echo "SKIP: Boost.JSON headers not found (set BOOST_INCLUDE)"; exit 0; fi
  cat "$T/cc.log" >&2; exit 1
fi

fail() { echo "FAIL: $*" >&2; exit 1; }

make_tree() {   # dir build
  mkdir -p "$1/bin"
  printf '{"Version":"%s","Channel":"Slipstream"}\n' "$2" > "$1/build_data.json"
  printf '#!/bin/sh\necho launcher %s\n' "$2" > "$1/gunmetal-dx"; chmod +x "$1/gunmetal-dx"
  printf '#!/bin/sh\necho viewer %s\n' "$2" > "$1/bin/do-not-directly-run-slipstream-bin"
  chmod +x "$1/bin/do-not-directly-run-slipstream-bin"
}

# Fixtures: installed 1.0.0.1; update 1.0.0.2 packed as Slipstream_x/ like the real tarball.
mkdir -p "$T/pk/Slipstream_1_0_0_2"; make_tree "$T/pk/Slipstream_1_0_0_2" 1.0.0.2
NAME=Slipstream_1.0.0.2_x86_64.tar.xz
tar -C "$T/pk" -cJf "$T/$NAME" Slipstream_1_0_0_2
cp "$T/$NAME" "$T/orig.tar.xz"
sha=$(sha256sum "$T/$NAME" | cut -d' ' -f1); size=$(stat -c %s "$T/$NAME")
cat > "$T/artifacts.json" <<JSON
{"linux_tarball": {"name": "$NAME",
 "url": "https://github.com/squeedledorf/GunMetal-DX/releases/download/v1.0.0/$NAME",
 "sha256": "$sha", "size": $size}}
JSON
UPDATE_SIGNING_KEY_FILE="$T/key.pem" python3 "$HERE/sign_update_manifest.py" sign "$T/artifacts.json" \
  --version 1.0.0 --build 1.0.0.2 --channel release --out "$T/signed.json" >/dev/null

stage() {   # staged-json-path file-path [signed-json]
  python3 - "$1" "$2" "${3:-$T/signed.json}" <<'PY'
import json, sys
s = json.load(open(sys.argv[3]))
json.dump({"manifest": s["manifest"], "signature": s["signature"], "artifact": "linux_tarball",
           "file": sys.argv[2]}, open(sys.argv[1], "w"))
PY
}

fresh() {   # new install at $T/inst/app (1.0.0.1) and empty state dir
  rm -rf "$T/inst" "$T/state"; mkdir -p "$T/inst" "$T/state"
  make_tree "$T/inst/app" 1.0.0.1
  echo "$T/staged.json" > "$T/state/pending-update"
  cp "$T/orig.tar.xz" "$T/$NAME"
}
run() { "$T/apply" "$T/inst/app" "$T/state" "$T/staged.json" "${1-$pub}"; }

# 1. happy path
fresh; stage "$T/staged.json" "$T/$NAME"
out=$(run); echo "$out"
[ "${out%% *}" = 0 ] || fail "apply: $out"
grep -q 1.0.0.2 "$T/inst/app/build_data.json" || fail "new version not in place"
grep -q 1.0.0.1 "$T/inst/app.prev/build_data.json" || fail "previous install not kept"
[ -f "$T/state/update-unconfirmed" ] && [ ! -e "$T/state/pending-update" ] || fail "state files"
[ "$(head -1 "$T/state/update-unconfirmed")" = 1.0.0.2 ] || fail "unconfirmed marker"
[ ! -e "$T/$NAME" ] || fail "artifact not removed"
ls -A "$T/inst" | grep -q slipstream-update && fail "work dir left behind"

# 2. tampered download -> nothing changes
fresh; cp "$T/orig.tar.xz" "$T/tampered.tar.xz"
printf 'x' >> "$T/tampered.tar.xz"; stage "$T/staged.json" "$T/tampered.tar.xz"
out=$(run); [ "${out%% *}" = 10 ] || fail "tampered file accepted: $out"
grep -q 1.0.0.1 "$T/inst/app/build_data.json" && [ ! -e "$T/inst/app.prev" ] || fail "tampered: tree changed"
[ ! -e "$T/state/pending-update" ] || fail "tampered: pending not dropped"

# 3. wrong public key (manifest signed by someone else)
fresh; stage "$T/staged.json" "$T/$NAME"
out=$(run "$other"); [ "${out%% *}" = 10 ] || fail "wrong key accepted: $out"
out=$(run ""); [ "${out%% *}" = 10 ] || fail "empty (placeholder) key accepted: $out"
grep -q 1.0.0.1 "$T/inst/app/build_data.json" || fail "wrong key: tree changed"

# 4. manifest edited after signing (URL swapped)
fresh; python3 - "$T/signed.json" "$T/forged.json" <<'PY'
import json, sys
s = json.load(open(sys.argv[1]))
s["manifest"] = s["manifest"].replace("github.com/squeedledorf/GunMetal-DX", "github.com/evil/GunMetal-DX")
json.dump(s, open(sys.argv[2], "w"))
PY
stage "$T/staged.json" "$T/$NAME" "$T/forged.json"
out=$(run); [ "${out%% *}" = 10 ] || fail "forged manifest accepted: $out"

# 5. not newer (downgrade / replay): installed already 1.0.0.2
fresh; make_tree "$T/inst/app" 1.0.0.2; stage "$T/staged.json" "$T/$NAME"
out=$(run); [ "${out%% *}" = 10 ] || fail "downgrade/same version accepted: $out"

# 6. build marked bad by an earlier rollback
fresh; echo 1.0.0.2 > "$T/state/bad-build"; stage "$T/staged.json" "$T/$NAME"
out=$(run); [ "${out%% *}" = 10 ] || fail "bad build re-installed: $out"

# 7. package-managed install is left alone
fresh; echo deb > "$T/inst/app/.packaged-by"; stage "$T/staged.json" "$T/$NAME"
out=$(run); [ "${out%% *}" = 10 ] || fail "package-managed install touched: $out"

# 8. archive whose version does not match the manifest -> FAILED, install untouched, no debris
fresh; mkdir -p "$T/pk2/Slipstream_x"; make_tree "$T/pk2/Slipstream_x" 1.0.0.9
tar -C "$T/pk2" -cJf "$T/mismatch.tar.xz" Slipstream_x
msha=$(sha256sum "$T/mismatch.tar.xz" | cut -d' ' -f1); msize=$(stat -c %s "$T/mismatch.tar.xz")
sed "s/$sha/$msha/; s/\"size\": $size/\"size\": $msize/; s/$NAME/mismatch.tar.xz/" "$T/artifacts.json" > "$T/a2.json"
UPDATE_SIGNING_KEY_FILE="$T/key.pem" python3 "$HERE/sign_update_manifest.py" sign "$T/a2.json" \
  --version 1.0.0 --build 1.0.0.2 --channel release --out "$T/signed2.json" >/dev/null
stage "$T/staged.json" "$T/mismatch.tar.xz" "$T/signed2.json"
out=$(run); [ "${out%% *}" = 11 ] || fail "mismatching archive: $out"
grep -q 1.0.0.1 "$T/inst/app/build_data.json" && [ ! -e "$T/inst/app.prev" ] || fail "mismatch: tree changed"
ls -A "$T/inst" | grep -q slipstream-update && fail "mismatch: work dir left behind"

# 9. read-only parent -> FAILED (skip when running as root, which ignores modes)
if [ "$(id -u)" != 0 ]; then
  fresh; stage "$T/staged.json" "$T/$NAME"; chmod a-w "$T/inst"
  out=$(run); chmod u+w "$T/inst"
  [ "${out%% *}" = 11 ] || fail "read-only parent: $out"
fi

# 10. version compare
for pair in "1.2.0.4:1" "1.2.0.3:0" "1.2.0.2:-1" "1.10.0.0:1" "1.2.0:2" "x:2" "1.2.0.3.1:2"; do
  got=$("$T/apply" version-compare "${pair%%:*}")
  [ "$got" = "${pair##*:}" ] || fail "compareBuild ${pair%%:*} -> $got"
done

# 11. launcher hooks: apply + relaunch contract, rollback on early crash, bad-build
bash "$HERE/test_update_hooks.sh" "$T" "$NV/linux_tools/update_hooks.sh"
echo "update apply tests OK"
