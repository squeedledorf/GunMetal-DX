#!/bin/bash
# Windows is case-insensitive and LL's CMake spells SDK libraries in its own case
# (Crypt32.lib, Ws2_32.lib, ...); xwin's splat only has some variants. Add a
# Capitalised symlink for every lowercase .lib. Idempotent; run after `xwin splat`.
for X in ${XWIN_ROOT:-$HOME/.xwin}/sdk/lib/um/x86_64 ${XWIN_ROOT:-$HOME/.xwin}/sdk/lib/ucrt/x86_64 ${XWIN_ROOT:-$HOME/.xwin}/crt/lib/x86_64; do
  cd "$X" || continue
  for f in *.lib; do
    [[ "$f" =~ ^[a-z0-9_]+\.lib$ ]] || continue
    cap="${f^}"
    [ -e "$cap" ] || ln -s "$f" "$cap"
  done
done

# Same for headers (#include <Shlobj.h> etc.).
for X in ${XWIN_ROOT:-$HOME/.xwin}/sdk/include/um ${XWIN_ROOT:-$HOME/.xwin}/sdk/include/shared ${XWIN_ROOT:-$HOME/.xwin}/sdk/include/ucrt; do
  cd "$X" || continue
  for f in *.h; do
    [[ "$f" =~ ^[a-z0-9_]+\.h$ ]] || continue
    cap="${f^}"
    [ -e "$cap" ] || ln -s "$f" "$cap"
  done
done

# All-caps includes used by LL code.
X=${XWIN_ROOT:-$HOME/.xwin}/sdk/include/um
[ -e "$X/WERAPI.H" ] || ln -s werapi.h "$X/WERAPI.H"
