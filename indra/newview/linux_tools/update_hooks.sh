#!/bin/bash
# Launcher side of the tarball auto-updater (#421). Sourced by the `slipstream` launcher
# (wrapper.sh); defines functions only.
#
# State lives in ${XDG_STATE_HOME:-$HOME/.local/state}/gunmetal-dx-update (the viewer,
# SLUpdater / slupdateapply.cpp, uses the same path):
#   pending-update      one line: path of the staged record a running viewer wrote after it
#                       downloaded and verified an update. Its existence is the only thing
#                       the launcher checks on a normal start (one stat, no process).
#   update-unconfirmed  written by the apply step: line 1 = new build, line 2 = old build.
#                       The new viewer deletes it when it reaches the login screen.
#   bad-build           a build that died before confirming; never installed again.
#
# Nothing inside a running install is edited. The apply step unpacks the new tree next to the
# install and swaps the two directories; the old one stays as <root>.prev.

slipstream_update_state_dir() {
    echo "${XDG_STATE_HOME:-$HOME/.local/state}/gunmetal-dx-update"
}

# slipstream_update_apply ROOT BIN
# Runs the apply step when an update is pending. Returns 0 when the install changed (the
# caller must re-exec the launcher: its own directory was just swapped), 1 otherwise.
slipstream_update_apply() {
    local root=$1 bin=$2 state
    [ -z "${SLIPSTREAM_UPDATE_APPLIED:-}" ] || return 1     # at most once per launch
    state=$(slipstream_update_state_dir)
    [ -f "${state}/pending-update" ] || return 1
    [ -x "${bin}" ] || return 1
    "${bin}" --slipstream-apply-update "${root}"
    [ $? -eq 0 ]
}

# slipstream_update_rollback_if_needed ROOT EXITCODE
# Called after the viewer exited. A viewer that died (non-zero exit) before it confirmed
# itself at the login screen is replaced by the previous install; its build is remembered in
# bad-build. Returns 0 when it rolled back (caller re-execs the launcher), 1 otherwise.
slipstream_update_rollback_if_needed() {
    local root=$1 rc=$2 state build
    state=$(slipstream_update_state_dir)
    [ -f "${state}/update-unconfirmed" ] || return 1
    [ "${rc}" -ne 0 ] || return 1                 # a clean exit is not a failed start
    [ -d "${root}.prev" ] || { rm -f "${state}/update-unconfirmed"; return 1; }
    build=$(head -n 1 "${state}/update-unconfirmed")
    echo "*** The updated viewer ${build} stopped before it finished starting; going back to the previous version. ***" >&2
    if ! mv "${root}" "${root}.failed.$$"; then
        return 1
    fi
    if ! mv "${root}.prev" "${root}"; then
        mv "${root}.failed.$$" "${root}"          # put the broken-but-complete one back
        return 1
    fi
    rm -rf "${root}.failed.$$"
    echo "${build}" > "${state}/bad-build"
    rm -f "${state}/update-unconfirmed"
    return 0
}
