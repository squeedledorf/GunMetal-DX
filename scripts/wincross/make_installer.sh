#!/bin/bash
# Build the Windows installer (NSIS) from a staged Windows package.
#   make_installer.sh <stage dir> <output .exe> <file version a.b.c.d> <display version> [app name]
# The app name (default "Slipstream") names the install folder, shortcuts and uninstall
# entry; Jetstream builds pass "Slipstream Jetstream" so they install beside a Beta / Release.
# Needs makensis (apt install nsis). The installer's file list and the uninstaller's
# removal list are generated from the stage, so the uninstaller removes exactly what was
# installed. Inno Setup leftovers from viewer_manifest.py (installers/, *.iss) are skipped.
set -euo pipefail
STAGE=$(realpath "$1"); OUT=$(realpath -m "$2"); FILEVER="$3"; DISPVER="$4"; APPNAME="${5:-GunMetal-DX}"
SRC="$(cd "$(dirname "$0")/../.." && pwd)/indra/newview/installers/windows"
command -v makensis >/dev/null || { echo "make_installer: makensis not found (apt install nsis)"; exit 2; }
[ -f "$STAGE/GunMetalDX.exe" ] || { echo "make_installer: $STAGE has no GunMetalDX.exe"; exit 1; }

LISTS=$(mktemp -d); trap 'rm -rf "$LISTS"' EXIT
: > "$LISTS/install_list.nsh"; : > "$LISTS/uninstall_list.txt"
while IFS= read -r -d '' entry; do
    name=$(basename "$entry")
    case "$name" in installers|*.iss) continue ;; esac
    if [ -d "$entry" ]; then
        printf 'File /r "%s"\n' "$entry" >> "$LISTS/install_list.nsh"
        printf 'D %s\r\n' "$name" >> "$LISTS/uninstall_list.txt"
    else
        printf 'File "%s"\n' "$entry" >> "$LISTS/install_list.nsh"
        printf 'F %s\r\n' "$name" >> "$LISTS/uninstall_list.txt"
    fi
done < <(find "$STAGE" -mindepth 1 -maxdepth 1 -print0 | sort -z)

mkdir -p "$(dirname "$OUT")"
makensis -V2 -INPUTCHARSET UTF8 \
    "-DVERSION=$FILEVER" "-DDISPLAY_VERSION=$DISPVER" "-DSRCDIR=$STAGE" "-DOUTFILE=$OUT" \
    "-DASSETS=$SRC" "-DLISTS=$LISTS" "-DAPP_NAME=$APPNAME" "$SRC/slipstream_installer.nsi"
ls -la "$OUT"
