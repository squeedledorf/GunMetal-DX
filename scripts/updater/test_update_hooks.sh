#!/bin/bash
# Launcher-hook tests, called by test_update_apply.sh with its temp dir (which already holds
# the compiled "apply" driver, fixtures and a signed manifest for 1.0.0.2).
# usage: test_update_hooks.sh TMPDIR path/to/update_hooks.sh
set -euo pipefail
T=$1
HOOKS=$2
HERE=$(cd "$(dirname "$0")" && pwd)
fail() { echo "FAIL(hooks): $*" >&2; exit 1; }
# shellcheck disable=SC1090
. "$HOOKS"

export XDG_STATE_HOME="$T/xdg"
STATE="$XDG_STATE_HOME/gunmetal-dx-update"
BIN="$T/apply"       # the driver doubles as the viewer binary for --slipstream-apply-update
NAME=Slipstream_1.0.0.2_x86_64.tar.xz

make_tree() {
  mkdir -p "$1/bin"
  printf '{"Version":"%s"}\n' "$2" > "$1/build_data.json"
  printf '#!/bin/sh\n' > "$1/gunmetal-dx"; chmod +x "$1/gunmetal-dx"
  printf '#!/bin/sh\n' > "$1/bin/do-not-directly-run-slipstream-bin"; chmod +x "$1/bin/do-not-directly-run-slipstream-bin"
}
setup() {
  rm -rf "$T/hi" "$XDG_STATE_HOME"; mkdir -p "$T/hi" "$STATE"
  make_tree "$T/hi/app" 1.0.0.1
  cp "$T/orig.tar.xz" "$T/$NAME"
  python3 - "$T/signed.json" "$T/$NAME" "$T/hstaged.json" <<'PY'
import json, sys
s = json.load(open(sys.argv[1]))
json.dump({"manifest": s["manifest"], "signature": s["signature"], "artifact": "linux_tarball",
           "file": sys.argv[2]}, open(sys.argv[3], "w"))
PY
}
version() { sed -n 's/.*"Version":"\([^"]*\)".*/\1/p' "$T/hi/app/build_data.json"; }

# A. nothing pending: no-op, no process started
setup
slipstream_update_apply "$T/hi/app" "$BIN" && fail "applied without a pending update"
[ "$(version)" = 1.0.0.1 ] || fail "A: changed"

# B. pending: applied, then a crash before confirmation rolls back and blocks the build
setup; echo "$T/hstaged.json" > "$STATE/pending-update"
slipstream_update_apply "$T/hi/app" "$BIN" 2>/dev/null || fail "B: apply failed"
[ "$(version)" = 1.0.0.2 ] || fail "B: not updated"
SLIPSTREAM_UPDATE_APPLIED=1 slipstream_update_apply "$T/hi/app" "$BIN" && fail "B: applied twice in a launch"
slipstream_update_rollback_if_needed "$T/hi/app" 0 && fail "B: rolled back on a clean exit"
[ "$(version)" = 1.0.0.2 ] || fail "B: clean exit changed the install"
slipstream_update_rollback_if_needed "$T/hi/app" 139 2>/dev/null || fail "B: no rollback after a crash"
[ "$(version)" = 1.0.0.1 ] || fail "B: not rolled back"
[ ! -e "$T/hi/app.prev" ] && [ ! -e "$STATE/update-unconfirmed" ] || fail "B: leftovers"
[ "$(cat "$STATE/bad-build")" = 1.0.0.2 ] || fail "B: bad-build not recorded"
ls "$T/hi" | grep -q failed && fail "B: failed tree left behind"
# the same build is not offered again, even if staged again
cp "$T/orig.tar.xz" "$T/$NAME"; echo "$T/hstaged.json" > "$STATE/pending-update"
slipstream_update_apply "$T/hi/app" "$BIN" 2>/dev/null && fail "B: bad build re-applied"
[ "$(version)" = 1.0.0.1 ] || fail "B: bad build installed"

# C. confirmed (marker removed by the new viewer) -> a later crash does not roll back
setup; echo "$T/hstaged.json" > "$STATE/pending-update"
slipstream_update_apply "$T/hi/app" "$BIN" 2>/dev/null || fail "C: apply failed"
rm -f "$STATE/update-unconfirmed"
slipstream_update_rollback_if_needed "$T/hi/app" 139 && fail "C: rolled back a confirmed update"
[ "$(version)" = 1.0.0.2 ] || fail "C: changed"

# D. rollback with no .prev just clears the marker
setup; echo "1.0.0.2" > "$STATE/update-unconfirmed"
slipstream_update_rollback_if_needed "$T/hi/app" 1 && fail "D: rolled back without .prev"
[ ! -e "$STATE/update-unconfirmed" ] || fail "D: marker kept"
echo "hooks OK"
