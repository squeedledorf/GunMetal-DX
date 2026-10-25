# slipstream-viewer: Debian / Ubuntu / Mint package (#278)

`slipstream-viewer` repackages the official Linux release tarball as a `.deb`.
apt then installs the system libraries the viewer and its built-in browser
(CEF) need, so the "media_plugin_cef failed" class of bug (#245, #264) can't
happen on a package install.

Supported: Ubuntu 24.04+, Debian 13 (trixie)+, Linux Mint 22+, Pop!_OS 24.04+
and other derivatives of those. The release binary needs glibc 2.38 and
GLIBCXX 3.4.32, which rules out Ubuntu 22.04 and Debian 12; `dpkg-shlibdeps`
turns that into `libc6 (>= 2.38)` and `libstdc++6 (>= 14)`, so apt refuses to
install the package there instead of installing a viewer that can't start.

| File | Purpose |
|---|---|
| `build_deb.sh` | Builds `Slipstream_<label>_amd64.deb` from a release tarball. CI uses it. |
| `depends.extra` | Dependencies `dpkg-shlibdeps` can't see (dlopen), plus Recommends / Suggests. |
| `depends.shlibs` | Committed `dpkg-shlibdeps` result, the fallback when it can't run fresh. |
| `lintian-overrides` | Expected lintian findings for a repackaged prebuilt release. |

## Installing

Recommended: the signed apt repository (#288), which also delivers updates
through `apt upgrade`. See `scripts/packaging/apt/README.md`:

```sh
sudo wget -qO /usr/share/keyrings/slipstream-archive-keyring.asc https://apt.slipstreamviewer.org/slipstream-archive-keyring.asc
sudo wget -qO /etc/apt/sources.list.d/slipstream.sources https://apt.slipstreamviewer.org/slipstream.sources
sudo apt update && sudo apt install slipstream-viewer slipstream-archive-keyring
```

Or install a downloaded file directly:

```sh
sudo apt install ./Slipstream_<version>_amd64.deb
```

The `./` matters: it tells apt to install a local file and resolve its
dependencies. A graphical installer such as GDebi does the same (Ubuntu
24.04's App Center has had trouble with local .deb files, so the command is the
reliable route). Upgrading is the same command with the
newer file; `sudo apt remove slipstream-viewer` uninstalls (user data in
`~/.slipstreamviewer` is kept).

## What the package installs

- `/opt/slipstream/`: the release tarball, unchanged except for a
  `.packaged-by` marker (content `deb`). With the marker, the `slipstream`
  wrapper skips the per-user desktop entry, the per-user `secondlife://`
  registration and the #245 missing-library check. Wrappers older than the
  marker get the two registration lines guarded by a `sed`, as in the Arch
  package.
- `/usr/bin/slipstream`: `exec /opt/slipstream/slipstream "$@"` with
  `SLIPSTREAM_SKIP_MEDIA_CHECK=1`. Not a symlink: the wrapper finds
  `build_data.json` from its own unresolved `$0`.
- `/usr/share/applications/slipstream.desktop`: `Exec=slipstream %u`,
  `MimeType=x-scheme-handler/secondlife;x-scheme-handler/x-grid-location-info;`
  (the two schemes the Windows installer registers and `llslurl.cpp` parses;
  the tarball wrapper's per-user entry registers no MimeType at all), and
  `StartupWMClass` set to the channel from `build_data.json`.
- `/usr/share/icons/hicolor/`: the hicolor set from the tarball's
  `share/icons/hicolor`.
- `/usr/share/doc/slipstream-viewer/`: `copyright` and the changelog.
- No maintainer scripts. dpkg triggers from `desktop-file-utils` and
  `gtk-update-icon-cache` refresh the menu, the MIME cache and the icon cache.
- If you ran the tarball before, delete
  `~/.local/share/applications/slipstream-viewer.desktop`, or the menu shows
  two Slipstream entries.

## Version mapping

| Tag | Debian version | File |
|---|---|---|
| `vX.Y.Z-beta.N` | `X.Y.Z~betaN` | `Slipstream_X.Y.Z-beta.N_amd64.deb` |
| `vX.Y.Z` | `X.Y.Z` | `Slipstream_X.Y.Z_amd64.deb` |

`~` sorts before everything, even the end of the string, so
`1.0.0~beta9 < 1.0.0~beta10 < 1.0.0 < 1.0.1~beta1`, and apt upgrades a beta to
the final release. `build_deb.sh --selftest` checks the same pairs as the AUR
`gen.sh` with `dpkg --compare-versions`. The version has no Debian revision
(a native package); set `DEBREV=2` to rebuild a fixed package for the same
release (`1.0.0-2` sorts above `1.0.0`).

## How the dependencies are derived

1. **Linked libraries, by `dpkg-shlibdeps`.** The tree is staged at
   `debian/slipstream-viewer/opt/slipstream`, with `-l/opt/slipstream/lib
   -l/opt/slipstream/bin/llplugin`, so the libraries the tarball bundles
   (SDL3, OpenAL, ALUT, OpenJPEG, the Vulkan loader, SwiftShader, CEF, WebRTC)
   resolve to the staged copies and produce no dependency, while every other
   `NEEDED` library maps to its package with the minimum version from the
   distribution's symbols files. Every 64-bit ELF is scanned (viewer,
   SLPlugin, dullahan_host, chrome-sandbox, libcef.so, the CEF and GStreamer
   plugins) except the VLC plugin (libvlc is optional: Suggests) and the
   example plugin.
2. **dlopen'd libraries, from `depends.extra`.** SDL3 loads X11 / Wayland /
   xkbcommon / libdecor, OpenAL and WebRTC load libpulse, CEF loads more of
   its runtime than it links. Those are merged in; a relation whose package the
   computed list already names is skipped, so a versioned computed relation is
   never weakened.
3. **t64 names.** Ubuntu 24.04 and Debian 13 renamed several libraries with a
   `t64` suffix. Each `*t64` relation gets the old name as an alternative
   (`libasound2t64 (>= 1.0.17) | libasound2 (>= 1.0.17)`).

`dpkg-shlibdeps` needs every linked library installed. If it is missing or
fails, `build_deb.sh` warns and uses `depends.shlibs`, made with
`build_deb.sh --shlibs <tarball>` in a clean `ubuntu:24.04` with `dpkg-dev`
and the `depends.extra` Depends installed. Refresh it when the viewer links a
new system library (`SHLIBDEPS=fresh` makes a stale environment fail instead
of falling back).

Window decorations (#196 / #197): SDL3 draws client-side decorations on GNOME
Wayland through libdecor, and libdecor needs a plugin. Both noble and trixie
ship `libdecor-0-0` with `libdecor-0-plugin-1-gtk` and
`libdecor-0-plugin-1-cairo`; libdecor itself only *recommends* a plugin, so the
package depends on `libdecor-0-0, libdecor-0-plugin-1-gtk |
libdecor-0-plugin-1-cairo` (GTK first for native-looking title bars on GNOME;
desktop systems have GTK 3 already).

Recommends (installed by default): `mesa-vulkan-drivers | vulkan-icd` (a
Vulkan driver; NVIDIA's driver packages provide `vulkan-icd`), the PipeWire
client library, `gstreamer1.0-plugins-base` + `-good` (in-world media through
the GStreamer plugin), `xdg-utils`, and `slipstream-archive-keyring` (the apt
repository's key package, #288). A direct `apt install ./file.deb` skips the
keyring package because it is not in the distribution archives. That is why it
is a Recommends and never a Depends. Suggests: more GStreamer formats,
`libvlc5`, `libnotify-bin`, `gamemode`, `switcheroo-control`.

## Sandbox (chrome-sandbox)

`bin/chrome-sandbox` stays as the tarball ships it: mode 0755, not setuid
root. dullahan runs CEF without the setuid sandbox, exactly as the tarball
and the Arch package do; the wrapper's `chrome_sandboxing_permissions_setup.sh`
step is disabled. A setuid-root helper nothing uses would only add attack
surface. Enabling the CEF sandbox is a separate decision (security audit item
U-4); if it happens, unprivileged user namespaces (with an AppArmor profile on
Ubuntu 24.04, which restricts them) are the better route than setuid.

## CI (`.forgejo/workflows/release.yml`, job `deb-package`)

Runs on `vX.Y.Z` / `vX.Y.Z-beta.N` tags after `build` published the tarball.
Every step is `continue-on-error`, so it can't block or undo the other assets:

1. Extract `scripts/packaging/deb` + `scripts/ci` at the tagged commit,
   download the tarball over the LAN and check it against the sha256 that
   `publish_release.sh` put in the release notes.
2. `build_deb.sh` (staging under `/srv/ci/slipstream-deb`, not `/tmp`).
3. Attach the `.deb` with `publish_release.sh`, which appends its sha256 to
   the release notes.

The runner needs nothing new to build the package: `dpkg-deb` is part of
`dpkg`, and without `dpkg-dev` the snapshot is used. For fresh dependency
computation and the lintian report, run once on applejack as root:

```sh
apt install dpkg-dev lintian \
  libvulkan1 libx11-6 libxext6 libxcursor1 libxi6 libxrandr2 libxfixes3 libxss1 \
  libxkbcommon0 libwayland-client0 libwayland-cursor0 libwayland-egl1 libdrm2 libgbm1 \
  libpulse0 libdecor-0-0 libdecor-0-plugin-1-gtk libnss3 libnspr4 libatk1.0-0t64 \
  libatk-bridge2.0-0t64 libatspi2.0-0t64 libcups2t64 libxcomposite1 libxdamage1 \
  libxshmfence1 libasound2t64 libpango-1.0-0 libcairo2 libdbus-1-3 libexpat1 libudev1 \
  libglib2.0-0t64 libfontconfig1 hicolor-icon-theme
```

(The library list is the `Depends:` lines of `depends.extra`.)

## Local checks

```sh
scripts/packaging/deb/build_deb.sh --selftest          # version ordering (dpkg)
scripts/packaging/deb/build_deb.sh Slipstream_1.0.0-beta.5_x86_64.tar.xz v1.0.0-beta.5 out/
# then, in a clean container:
docker run --rm -v "$PWD/out:/out:ro" ubuntu:24.04 bash -c \
  'apt-get update && apt-get install -y /out/Slipstream_1.0.0-beta.5_amd64.deb &&
   cd /opt/slipstream && LD_LIBRARY_PATH=lib ldd lib/libcef.so bin/llplugin/libmedia_plugin_cef.so | grep "not found"'
```
