#!/bin/bash

## Here are some configuration options for Linux Client Users.

## - Avoids using any OpenAL audio driver.
#export LL_BAD_OPENAL_DRIVER=x

# Completely prevent gamemode from enabling even if set to true in the settings
# This can be useful if you run Slipstream on a battery-operated device (i.e. laptop)
# export DISABLE_GAMEMODE=1

## Everything below this line is just for advanced troubleshooters.
##-------------------------------------------------------------------

## - For advanced debugging cases, you can run the viewer under the
##   control of another program, such as strace, gdb, or valgrind.  If
##   you're building your own viewer, bear in mind that the executable
##   in the bin directory will be stripped: you should replace it with
##   an unstripped binary before you run.
if [ -n "${AL_GDB}" ]; then
    export LL_WRAPPER='gdb --args'
fi

if [ -n "${AL_VALGRIND}" ]; then
    export LL_WRAPPER='valgrind --smc-check=all --error-limit=no --log-file=secondlife.vg --leak-check=full --suppressions=/usr/lib/valgrind/glibc-2.5.supp --suppressions=secondlife-i686.supp'
fi

if [ -n "${AL_MANGO}" ]; then
    export LL_WRAPPER='mangohud --dlsym'
fi

## CPU placement on multi-CCD CPUs (AMD Ryzen/Threadripper/EPYC): the L3
## cache is split per-CCD and cross-CCD reads pay Infinity-Fabric latency.
## The viewer now places threads itself (setting SlipstreamThreadAffinity,
## default on): the main thread and the GPU driver threads stay on CCD0,
## worker pools (image decode, texture staging, mesh decode, fetch, HTTP)
## run on the other CCDs. Pinning the WHOLE process to CCD0 (the old
## behavior) crammed ~60 threads onto 6 cores and starved the main thread
## under load; it is kept only as an opt-in: SLIPSTREAM_AFFINITY=legacy.
## SLIPSTREAM_NO_AFFINITY=1 disables both. The pre-1.2 names ALCHEMY_AFFINITY /
## ALCHEMY_NO_AFFINITY are still read when the new ones are not set (#126).
SLIPSTREAM_AFFINITY="${SLIPSTREAM_AFFINITY-${ALCHEMY_AFFINITY}}"
SLIPSTREAM_NO_AFFINITY="${SLIPSTREAM_NO_AFFINITY-${ALCHEMY_NO_AFFINITY}}"
if [ -n "${SLIPSTREAM_NO_AFFINITY}" ]; then
    export SLIPSTREAM_NO_AFFINITY   # the viewer's own thread placement reads it too
fi
if [ -z "${SLIPSTREAM_NO_AFFINITY}" ] && [ "${SLIPSTREAM_AFFINITY}" = "legacy" ] && command -v taskset >/dev/null 2>&1 && [ -d /sys/devices/system/cpu/cpu0/cache ]; then
    cpu0_l3=""
    for f in /sys/devices/system/cpu/cpu0/cache/index*/level; do
        [ "$(cat "$f" 2>/dev/null)" = "3" ] || continue
        cpu0_l3=$(cat "$(dirname "$f")/shared_cpu_list" 2>/dev/null)
        break
    done
    if [ -n "$cpu0_l3" ]; then
        l3_count=$(for f in /sys/devices/system/cpu/cpu*/cache/index*/level; do
            if [ "$(cat "$f" 2>/dev/null)" = "3" ]; then
                cat "$(dirname "$f")/shared_cpu_list" 2>/dev/null
            fi
        done | sort -u | wc -l)
        if [ "$l3_count" -gt 1 ]; then
            export LL_WRAPPER="taskset -c ${cpu0_l3} ${LL_WRAPPER}"
            echo "SLIPSTREAM_AFFINITY=legacy: pinning the whole process to CCD0 cores ${cpu0_l3} ($l3_count L3 cache instances)."
        fi
    fi
fi

## For controlling various sanitizer options
#export ASAN_OPTIONS="halt_on_error=0 detect_leaks=1 symbolize=1"
#export UBSAN_OPTIONS="print_stacktrace=1 print_summary=1 halt_on_error=0"

install_dir=$(dirname "$0")
build_data_file="${install_dir}/build_data.json"
if [ -f "${build_data_file}" ]; then
    channel=$(sed -n 's/.*"Channel"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "${build_data_file}")
else
    echo "Error: File ${build_data_file} not found." >&2
    channel="Slipstream" # Fail safely if we're unable to determine the channel
fi

## Allow Gnome 3 to properly display window title in app bar. A package can
## preset these (the Flatpak sets its app ID, so the desktop matches the
## window to the app's icon).
export SDL_VIDEO_WAYLAND_WMCLASS="${SDL_VIDEO_WAYLAND_WMCLASS:-$channel}"
export SDL_VIDEO_X11_WMCLASS="${SDL_VIDEO_X11_WMCLASS:-$channel}"

## - Enable threaded mesa GL impl
export mesa_glthread=true

## - Uncomment to enable nvidia threaded GL optimizations - MAY CAUSE INSTABILITY
#export __GL_THREADED_OPTIMIZATIONS=1

## Nothing worth editing below this line.
##-------------------------------------------------------------------

SCRIPTSRC=$(readlink -f "$0" || echo "$0")
RUN_PATH=$(dirname "${SCRIPTSRC}" || echo .)
echo "Running from ${RUN_PATH}"
cd "${RUN_PATH}" || return

# Installed by a distribution package (e.g. the slipstream-bin Arch package,
# #248): the package provides the desktop entry, the secondlife:// handler and
# the system libraries, so skip the per-user registration and the library check.
packaged_by=""
if [ -f .packaged-by ]; then
    packaged_by=$(cat .packaged-by)
fi

if [ -z "${packaged_by}" ]; then
    # Re-register the secondlife:// protocol handler every launch, for now.
    ./etc/register_secondlifeprotocol.sh

    # Re-register the application with the desktop system every launch, for now.
    ./etc/refresh_desktop_app_entry.sh
fi

## Before we mess with LD_LIBRARY_PATH, save the old one to restore for
##  subprocesses that care.
export SAVED_LD_LIBRARY_PATH="${LD_LIBRARY_PATH}"

# Add our library directory
export LD_LIBRARY_PATH="$PWD/lib:${LD_LIBRARY_PATH}"

## Auto-update (#421, off unless the person enabled it in Preferences): a viewer that
## downloaded and verified an update left a "pending" note; apply it before starting. The
## apply step swaps the install directory, so this launcher (which just moved to
## <dir>.prev) re-runs itself from the new one. Never for package-manager installs.
if [ -z "${packaged_by}" ] && [ -f etc/update_hooks.sh ]; then
    . etc/update_hooks.sh
    if slipstream_update_apply "${RUN_PATH}" "${RUN_PATH}/bin/do-not-directly-run-slipstream-bin"; then
        export SLIPSTREAM_UPDATE_APPLIED=1
        exec "${RUN_PATH}/gunmetal-dx" "$@"
    fi
fi

## Window frame on Wayland (#196/#197). GNOME draws no title bar or border for
## native Wayland windows; SDL3 draws them through libdecor + a libdecor plugin.
## lib/libdecor carries a copy (libdecor + the cairo plugin) for systems that
## lack it. It is deliberately not in lib/ (on LD_LIBRARY_PATH above), so a
## system libdecor wins when it has plugins: it may have the GTK plugin and
## match the desktop theme. The viewer makes the final choice, preloads the
## copy it picked and logs it ("Display backend:" lines in Slipstream.log);
## here we only point libdecor at our plugin when the system has no libdecor
## plugin directory, unless LIBDECOR_PLUGIN_DIR is already set.
if [ -z "${LIBDECOR_PLUGIN_DIR}" ] && [ -d "${RUN_PATH}/lib/libdecor/plugins-1" ]; then
    system_libdecor_plugins=""
    for libdir in /usr/lib/x86_64-linux-gnu /usr/lib64 /usr/lib; do
        if [ -e "${libdir}/libdecor-0.so.0" ] && ls "${libdir}"/libdecor/plugins-1/*.so >/dev/null 2>&1; then
            system_libdecor_plugins="${libdir}/libdecor/plugins-1"
            break
        fi
    done
    if [ -z "${system_libdecor_plugins}" ]; then
        export LIBDECOR_PLUGIN_DIR="${RUN_PATH}/lib/libdecor/plugins-1"
    fi
fi

# The web browser and media-on-a-prim (CEF) need system libraries that the
# viewer itself doesn't (nss, atk, cups, gbm, ...). If one is missing, the
# media plugin fails to load and all web content stays blank (#245). Say so
# up front, naming the libraries; README-linux.txt lists the packages.
# Set SLIPSTREAM_SKIP_MEDIA_CHECK=1 to skip; package installs skip it.
if [ -z "${SLIPSTREAM_SKIP_MEDIA_CHECK}" ] && [ -z "${packaged_by}" ] && [ -f lib/libcef.so ] && command -v ldd >/dev/null 2>&1; then
    cef_missing=$(ldd lib/libcef.so bin/llplugin/libmedia_plugin_cef.so 2>/dev/null \
        | awk '/not found/ {print $1}' | sort -u | tr '\n' ' ')
    if [ -n "${cef_missing}" ]; then
        echo "WARNING: web pages and in-world media will not work. Missing system libraries: ${cef_missing}" >&2
        echo "         See README-linux.txt ('Troubleshooting', 'Web and media') for the packages to install." >&2
        if command -v notify-send >/dev/null 2>&1; then
            notify-send "Slipstream: web and media disabled" \
                "Missing system libraries: ${cef_missing}. See README-linux.txt for the packages to install."
        fi
    fi
fi

# Copy "$@" to ARGS string specifically to delete the --skip-gridargs switch.
# The gridargs.dat file is no more, but we still want to avoid breaking
# scripts that invoke this one with --skip-gridargs.
# Note: In sh, we don't have arrays like in Bash. So, we use a string instead to store the arguments.
# --x11 / --wayland pick the display backend for this run (same as
# SDL_VIDEO_DRIVER=x11 / =wayland, overriding the Display backend preference).
ARGS=""
for ARG in "$@"; do
    case "$ARG" in
        --skip-gridargs) ;;
        --x11) export SDL_VIDEO_DRIVER=x11 ;;
        --wayland) export SDL_VIDEO_DRIVER=wayland ;;
        *) ARGS="$ARGS \"$ARG\"" ;;
    esac
done

# Check chrome-sandbox permissions, and try to set them if they are not already
# SANDBOX_BIN=bin/llplugin/chrome-sandbox
# if set-user-id = false || is writable || executable = false || read is false || is owned by effective uid || is owned by effective gid
# OPTOUT_FILE="bin/llplugin/.user_does_not_want_chrome_sandboxing_and_accepts_the_risks"
# if [ ! -u "$SANDBOX_BIN" ] || [ -w "$SANDBOX_BIN" ] || [ ! -x "$SANDBOX_BIN" ] || [ ! -r "$SANDBOX_BIN" ] || [ -O "$SANDBOX_BIN" ] || [ -G "$SANDBOX_BIN" ]; then
#     echo "$SANDBOX_BIN permissions are not set properly to run under sandboxing."
#     if [ ! -f "$OPTOUT_FILE" ]; then
#         SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
#         pkexec "$SCRIPT_DIR/etc/chrome_sandboxing_permissions_setup.sh"
#     fi
# fi

#setup wine voice
# if command -v wine >/dev/null 2>&1; then
#     export WINEDEBUG=-all # disable all debug output for wine
#     export WINEPREFIX="$HOME/.alchemynext/wine"
#     if [ ! -d "$WINEPREFIX" ]; then
#         DISPLAY="" wine hostname
#     fi
# else
#     export VIEWER_DISABLE_WINE=1
#     echo "Please install wine to enable full voice functionality."
# fi

# Check if switcheroo is needed
if [ -d /sys/class/drm/card1 ] && command -v switcherooctl >/dev/null 2>&1 && [ "$(switcherooctl)" = "" ]; then
    notify-send "Automatic GPU selection is not available" "Please enable switcheroo-control.service"
fi

# Run the program.
# Don't quote $LL_WRAPPER because, if empty, it should simply vanish from the
# command line. But DO quote "$ARGS": preserve separate args as
# individually quoted.
# Note: In sh, we don't have arrays like in Bash. So, we use a string instead to store the arguments.
eval "$LL_WRAPPER bin/do-not-directly-run-slipstream-bin $ARGS"
LL_RUN_ERR=$?

# A freshly installed update that died before it reached the login screen is rolled back
# to the previous install (kept as <dir>.prev) and that build is never installed again.
if type slipstream_update_rollback_if_needed >/dev/null 2>&1 \
   && slipstream_update_rollback_if_needed "${RUN_PATH}" "${LL_RUN_ERR}"; then
    export SLIPSTREAM_UPDATE_APPLIED=1
    exec "${RUN_PATH}/gunmetal-dx" "$@"
fi

# Handle any resulting errors
if [ $LL_RUN_ERR -ne 0 ]; then
    # generic error running the binary
    echo "*** Bad shutdown ($LL_RUN_ERR). ***"
    if [ "$(uname -m)" = "x86_64" ]; then
        echo
        cat << EOFMARKER
You are running GunMetal-DX on a x86_64 platform.
EOFMARKER
    fi
fi
