#!/bin/sh
# Launch a Slipstream TESTCTL build on a test box's desktop session from SSH.
#
#   slrun.sh [PACKAGE_DIR] [viewer args...]
#
# PACKAGE_DIR defaults to ~/slipstream/current. Logs in with the last remembered account
# (--autologin, else "Log In" over the test channel), so log in by hand once with
# "Remember password" ticked. Stdout/stderr go to
# ~/slipstream/run.log; the viewer's own log is ~/.slipstreamviewer/logs/Slipstream.log.
# Refuses to start a second copy.
set -eu

PKG=${1:-$HOME/slipstream/current}
[ $# -gt 0 ] && shift

if [ "$(ps -eo comm | grep -c '^slipstream$')" != 0 ]; then
    echo "viewer already running" >&2
    exit 1
fi

uid=$(id -u)
export XDG_RUNTIME_DIR=/run/user/$uid
# The desktop's Wayland socket (KDE uses wayland-0 or wayland-1).
if [ -z "${WAYLAND_DISPLAY:-}" ]; then
    for s in "$XDG_RUNTIME_DIR"/wayland-[0-9]; do
        [ -S "$s" ] && WAYLAND_DISPLAY=$(basename "$s")
    done
fi
export WAYLAND_DISPLAY
export DISPLAY=${DISPLAY:-:0}
export DBUS_SESSION_BUS_ADDRESS=unix:path=$XDG_RUNTIME_DIR/bus

cd "$PKG"
nohup setsid ./slipstream --autologin "$@" > "$HOME/slipstream/run.log" 2>&1 < /dev/null &
echo "started pid $! from $(readlink -f "$PKG")"

# --autologin doesn't always get past the login screen; press "Log In" (saved
# credentials) over the test channel if it is still sitting there.
CTL=$(dirname "$0")/slctl.py
if "$CTL" wait STATE_LOGIN_WAIT 180 > /dev/null; then
    sleep 3
    if "$CTL" status | grep -q '"STATE_LOGIN_WAIT"'; then
        "$CTL" login
    fi
fi
