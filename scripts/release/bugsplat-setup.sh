#!/usr/bin/env bash
# GunMetal-DX: store the BugSplat credentials the release scripts use (symbol uploads, crash
# reporting), in ~/.config/gunmetal-dx/bugsplat (mode 600; never in the repo).
#
#   scripts/release/bugsplat-setup.sh          a form (zenity) for the database, client ID and secret
#   scripts/release/bugsplat-setup.sh --check  test the saved credentials against BugSplat
#
# The database name is not secret (it is built into the viewer); the client ID / secret are, and
# only ever live in that file. BugSplat: Settings > Integrations / "Configure symbol uploads"
# creates the OAuth client ID and secret.
set -euo pipefail

CONF_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/gunmetal-dx"
CONF="$CONF_DIR/bugsplat"

say() { if command -v zenity >/dev/null && [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]; then zenity --info --no-wrap --title="BugSplat" --text="$1" 2>/dev/null || true; fi; echo "$1"; }
fail() { if command -v zenity >/dev/null && [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]; then zenity --error --no-wrap --title="BugSplat" --text="$1" 2>/dev/null || true; fi; echo "error: $1" >&2; exit 1; }

# Asks BugSplat for an OAuth token with the saved client credentials: proves the ID / secret work.
check() {
    [ -r "$CONF" ] || fail "No credentials yet: run scripts/release/bugsplat-setup.sh first."
    # shellcheck disable=SC1090
    . "$CONF"
    [ -n "${client_id:-}" ] && [ -n "${client_secret:-}" ] || fail "$CONF is missing client_id or client_secret."
    # The client's scope was picked when it was made in BugSplat: "symbols" (the symbol-upload
    # credentials) or "restricted"; asking for the other one is a 400, so both are tried.
    local code="" body="" scope
    for scope in symbols restricted; do
        body=$(curl -s -w '\n%{http_code}' -X POST "https://app.bugsplat.com/oauth2/authorize" \
            --data-urlencode "grant_type=client_credentials" \
            --data-urlencode "client_id=$client_id" \
            --data-urlencode "client_secret=$client_secret" \
            --data-urlencode "scope=$scope") || body=$'\n000'
        code="${body##*$'\n'}"
        body="${body%$'\n'*}"
        [ "$code" = 200 ] && break
    done
    case "$code" in
        200) say "BugSplat accepted the credentials (scope: $scope, database: ${database:-not set})." ;;
        000) fail "Could not reach BugSplat (no network?). The credentials are saved; run --check again later." ;;
        *)   fail "BugSplat refused the credentials (HTTP $code): $(printf '%s' "$body" | tr -d '\n' | cut -c1-200). Check the client ID and secret, then run the setup again." ;;
    esac
}

if [ "${1:-}" = "--check" ]; then check; exit 0; fi

command -v zenity >/dev/null || fail "zenity is not installed (pacman -S zenity)."

# current values as defaults (the secret is never shown)
database="" client_id="" client_secret=""
if [ -r "$CONF" ]; then
    # shellcheck disable=SC1090
    . "$CONF"
fi

form=$(zenity --forms --title="GunMetal-DX: BugSplat" --width=460 \
    --text="BugSplat credentials for crash reports and symbol uploads.\nSaved to $CONF (only you can read it).\nLeave the secret empty to keep the saved one." \
    --add-entry="Database name${database:+ (now: $database)}" \
    --add-entry="Client ID${client_id:+ (now: ${client_id:0:6}...)}" \
    --add-password="Client secret" \
    --separator=$'\x1f' 2>/dev/null) || { echo "cancelled"; exit 1; }

IFS=$'\x1f' read -r new_db new_id new_secret <<< "$form"
new_db="${new_db:-$database}"
new_id="${new_id:-$client_id}"
new_secret="${new_secret:-$client_secret}"
[ -n "$new_db" ] && [ -n "$new_id" ] && [ -n "$new_secret" ] || fail "All three are needed (database name, client ID, client secret)."
case "$new_db$new_id$new_secret" in *$'\n'*|*\'*) fail "The values can't contain quotes or line breaks." ;; esac

mkdir -p "$CONF_DIR"
umask 077
tmp="$CONF.tmp"
printf "database='%s'\nclient_id='%s'\nclient_secret='%s'\n" "$new_db" "$new_id" "$new_secret" > "$tmp"
chmod 600 "$tmp"
mv "$tmp" "$CONF"
echo "saved $CONF"

check
