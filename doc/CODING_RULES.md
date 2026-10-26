# Slipstream coding rules

Rules for anyone changing the viewer source, human or agent. Each one exists because breaking
it shipped a broken build at least once. Where a rule can be checked mechanically, a script
does it and `make` / CI fail on a violation.

## Windows: never name things after Windows SDK macros

The Linux build never sees the Windows headers, so these names compile fine locally and only
break the Windows cross-build (clang-cl) -- in CI, after the change has landed. That has
broken Windows builds several times, including Jetstream 1.5.0.7 / 1.5.0.8 and the v1.4.0
release (`bool far = true;`), which had to be re-released as 1.4.1.

Do **not** use these as variable, parameter, member, function or type names:

| Name | Where Windows defines it |
| --- | --- |
| `far`, `near`, `FAR`, `NEAR` | `minwindef.h` (to nothing) |
| `interface` | `combaseapi.h` (to `struct`) |
| `PASCAL`, `CALLBACK`, `WINAPI`, `CONST` | `minwindef.h` / `windef.h` |
| `min`, `max` | `windows.h` (we build with `NOMINMAX`, but don't rely on it in headers that third-party code includes) |
| `small` | `rpcndr.h` (not pulled in by our lean `windows.h` today; avoid in new code) |
| `ERROR`, `OPAQUE`, `TRANSPARENT`, `DELETE`, `IN`, `OUT`, `OPTIONAL`, `ABSOLUTE`, `RELATIVE`, `DIFFERENCE` | `wingdi.h` / `winnt.h` / `minwindef.h` (avoid; they break as soon as a file includes the full headers) |

Also avoid function and method names that collide with the Win32 `A`/`W` macros
(`CreateWindow`, `LoadImage`, `GetObject`, `SendMessage`, `PostMessage`, `DrawText`,
`GetMessage`, `CreateFont`, ...): on Windows they are renamed to `...W`.

Use descriptive names instead: `nearPlane` / `farPlane`, `nearDist` / `farDist`, `spaced`, `isSmall`.

**Checked by** `scripts/ci/check_win_macros.py` (declarations using the first rows of the
table), run at the start of `make`, `make windows` and the CI build. Add a name to its list
when a Windows build breaks on another one. A line that genuinely needs the name (after an
`#undef`) can opt out with the comment `// win-macro-ok`.

## Windows: the cross-build is the only Windows compiler we test with before CI

Changes to platform code (`#ifdef LL_WINDOWS`, `llwindow*`, `llfilesystem`, audio / media
plugins, anything that includes `<windows.h>`) need a local Windows cross-build before they
land: `bash scripts/wincross/configure.sh; source scripts/wincross/env.sh;
ninja -C build-windows-x86_64 <target>` (or `make windows` for the full package).
