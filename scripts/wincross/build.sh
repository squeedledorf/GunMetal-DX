#!/bin/bash
# Cross-build the Windows x64 viewer on Linux (clang-cl + lld-link + xwin) and
# package it as a zip. Run from the repo root. Prerequisites on the host:
#   clang/lld/llvm (clang-cl, lld-link, llvm-rc, llvm-mt, llvm-lib, llvm-dlltool),
#   7z, the Vulkan loader + headers, and the MSVC CRT/SDK splatted by xwin
#   (`xwin --accept-license splat --output ~/.xwin`; XWIN_ROOT overrides the path).
# Output: build-windows-x86_64/package/<ARTIFACT_BASE>_win64.zip (see scripts/ci/version.sh), plus
# <ARTIFACT_BASE>_Setup.exe when makensis is installed (scripts/wincross/make_installer.sh)
set -euo pipefail
B=build-windows-x86_64
# Version/channel/artifact name (scripts/ci/version.sh; the tag comes from CI).
eval "$(bash scripts/ci/version.sh | sed 's/^/V_/; s/=\(.*\)$/="\1"/')"
export VIEWER_CHANNEL="${VIEWER_CHANNEL:-$V_CHANNEL}"
CRT=${WINCROSS_CRT:-$HOME/.cache/wincross/crt}

bash scripts/wincross/xwin_case_links.sh
[ -f "$CRT/vcruntime140.dll" ] || python3 scripts/wincross/fetch_vcredist.py "$CRT"
[ -f "$B/vk/vulkan-1.lib" ] || bash scripts/wincross/make_vulkan_lib.sh "$B/vk"
bash scripts/wincross/configure.sh
source scripts/wincross/env.sh
ninja -C "$B" ${NINJA_JOBS:+-j"$NINJA_JOBS"}

# Stage exactly what viewer_manifest.py ships, then zip it.
STAGE="$PWD/$B/package/GunMetal-DX"   # <GMDX>
rm -rf "$PWD/$B/package"; mkdir -p "$STAGE"
( cd "$B/newview" && "$OLDPWD/.venv/bin/python" "$OLDPWD/indra/newview/viewer_manifest.py" \
    "--actions=copy package" --arch=x86_64 --platform=windows \
    --artwork="$OLDPWD/indra/newview" --openal=ON --tracy=OFF \
    --build="$OLDPWD/$B/newview" --buildtype=Release "--channel=$VIEWER_CHANNEL" --configuration=. \
    --dest="$STAGE" --grid=agni --source="$OLDPWD/indra/newview" \
    --versionfile="$OLDPWD/$B/newview/viewer_version.txt" )
( cd "$B/package" && 7z a -tzip -mx=7 "${V_ARTIFACT_BASE}_win64.zip" GunMetal-DX >/dev/null )
ls -la "$B/package/${V_ARTIFACT_BASE}_win64.zip"

# Installer (#168), an extra asset beside the zip. Needs makensis (apt install nsis);
# without it the zip is still produced and the installer is skipped.
if command -v makensis >/dev/null; then
    DISPVER="$V_FULL_VERSION"
    [ "$V_KIND" = "release" ] || DISPVER="$V_FULL_VERSION ${V_KIND^}"
    APPNAME="GunMetal-DX"   # <GMDX>
    bash scripts/wincross/make_installer.sh "$STAGE" "$B/package/${V_ARTIFACT_BASE}_Setup.exe" "$V_FULL_VERSION" "$DISPVER" "$APPNAME"
else
    echo "makensis not found: skipping the Windows installer (apt install nsis)"
fi
