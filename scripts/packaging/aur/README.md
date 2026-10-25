# slipstream-bin: Arch Linux package (#248)

`slipstream-bin` repackages the official Linux release tarball from Forgejo
Releases into an Arch package. pacman then installs the system libraries the
viewer and its built-in browser (CEF) need, so the missing-library failures from
#245 can't happen on Arch, CachyOS, EndeavourOS, Manjaro or SteamOS.

| File | Purpose |
|---|---|
| `PKGBUILD` | The package recipe and the template for every version. Edit it here. |
| `.SRCINFO` | Generated metadata the AUR requires (`makepkg --printsrcinfo`). |
| `gen.sh` | Writes `PKGBUILD` + `.SRCINFO` for a release tag. CI uses it. |
| `aur_push.sh` | Pushes `PKGBUILD` + `.SRCINFO` to the AUR. It does nothing until `AUR_SSH_KEY` is set. |
| `aur_known_hosts` | Pinned AUR SSH host keys. They match the fingerprints published on aur.archlinux.org. |

## Installing today (no AUR needed)

Starting with the next release tag, the release CI attaches the `PKGBUILD` to
each release. It also attaches a built `slipstream-bin-<ver>-x86_64.pkg.tar.zst`
once makepkg is installed on the builder (see "CI" below). For releases from
before that, use the `PKGBUILD` in this directory, which is pinned to the
current release. Users can install either way:

```sh
# Prebuilt package: pacman pulls in every dependency from the official repos.
sudo pacman -U https://git.slipstreamviewer.org/sky_line/Slipstream/releases/download/<tag>/slipstream-bin-<pkgver>-1-x86_64.pkg.tar.zst

# Or build it locally from the PKGBUILD (only repackages, takes seconds):
mkdir slipstream-bin && cd slipstream-bin
curl -LO https://git.slipstreamviewer.org/sky_line/Slipstream/releases/download/<tag>/PKGBUILD
makepkg -si
```

Once the AUR package exists: `yay -S slipstream-bin` or `paru -S slipstream-bin`.

Suggested text for the website download page (not added yet):

> **Arch Linux, CachyOS, EndeavourOS, Manjaro:** install `slipstream-bin` from
> the AUR (`yay -S slipstream-bin`). You can also download the `.pkg.tar.zst`
> from the release page and run `sudo pacman -U <file>`. pacman installs
> everything the viewer needs.

## What the package installs

- `/opt/slipstream/`: the release tarball, unchanged, plus a `.packaged-by`
  marker. When it sees the marker, the `slipstream` wrapper skips three things:
  the per-user desktop entry, the per-user `secondlife://` registration (the
  package installs both system-wide), and the #245 missing-library check (pacman
  guarantees the libraries). Wrappers from before the marker existed get the two
  registration lines guarded by a `sed` in `package()`. The `sed` does nothing on
  newer wrappers.
- `/usr/bin/slipstream`: a small `exec /opt/slipstream/slipstream "$@"` script
  that also sets `SLIPSTREAM_SKIP_MEDIA_CHECK=1`. It is not a symlink because the
  wrapper finds `build_data.json` relative to its own `$0`.
- `/usr/share/applications/slipstream.desktop`: `Exec=slipstream %u`, which
  handles `x-scheme-handler/secondlife` (the viewer accepts a bare SLURL as a
  positional argument). Its `StartupWMClass` is the channel name taken from
  `build_data.json`.
- `/usr/share/icons/hicolor/`: the full hicolor set when the tarball ships
  `share/icons/hicolor`, which `viewer_manifest.py` adds from the next release.
  Older tarballs only provide the 256 px `slipstream_icon.png`.
- `/usr/share/licenses/slipstream-bin/`: `licenses.txt` (bundled third-party
  components) and the font licenses. The viewer itself is LGPL-2.1-only, a
  common license that is not copied.
- User data stays in `~/.slipstreamviewer`. The package never touches it.
- If you ran the tarball before, its wrapper created
  `~/.local/share/applications/slipstream-viewer.desktop`. Delete that file,
  or the menu shows two Slipstream entries.

## Version mapping

| Tag | pkgver |
|---|---|
| `vX.Y.Z-beta.N` | `X.Y.ZbetaN` |
| `vX.Y.Z` | `X.Y.Z` |

pacman's `vercmp` sorts `1.0beta1` below `1.0` and compares the number after
`beta` as a number, so the order is beta.1 < beta.2 < ... < beta.10 < final <
the next version's betas. `pkgver` may not contain `-`, so the raw tag can't be
used. `gen.sh --selftest` checks these pairs with `vercmp`, and all of them
return -1 (strictly ascending):

```
v0.14.0-beta.1  < v0.14.0          0.14.0beta1  < 0.14.0
v0.14.0-beta.9  < v0.14.0-beta.10  0.14.0beta9  < 0.14.0beta10
v0.14.0-beta.1  < v0.14.0-beta.2   0.14.0beta1  < 0.14.0beta2
v0.12.0-beta.10 < v0.13.0-beta.1   0.12.0beta10 < 0.13.0beta1
v0.13.0         < v0.14.0-beta.1   0.13.0       < 0.14.0beta1
v0.14.0         < v0.14.1-beta.1   0.14.0       < 0.14.1beta1
v0.14.0         < v0.14.1          0.14.0       < 0.14.1
v0.99.0         < v1.0.0-beta.1    0.99.0       < 1.0.0beta1
v1.0.0-beta.12  < v1.0.0           1.0.0beta12  < 1.0.0
```

`pkgrel` starts at 1. Set `PKGREL=2` when running `gen.sh` to fix the
packaging without a new release.

## How the dependencies were derived

The dependencies come from the binaries, not from guesswork:

1. Run `readelf -d` and `ldd` (with `LD_LIBRARY_PATH=lib`) over every ELF file
   in the v0.14.0-beta.1 tarball.
2. Drop the libraries the tarball bundles in `lib/` and `bin/llplugin/`. Map
   each remaining direct `NEEDED` entry to its package with `pacman -Qo`.
3. Add the libraries that the bundled SDL3, OpenAL and WebRTC load with
   `dlopen`, found with `strings`: `libX11`, `libXcursor`, `libXi`, `libXrandr`,
   `libXfixes`, `libXext`, `libxkbcommon`, `wayland`, `libpulse`, `libdrm`.
4. Check the result with `namcap` in an `archlinux` container with the depends
   installed. Every 64-bit library resolves. The only unresolved ones are
   bundled libraries and the optional `libvlc`.

| Who needs it | Packages |
|---|---|
| Viewer binary | glibc, gcc-libs, glib2, fontconfig |
| CEF: browser, media on a prim, profiles, search (#245) | nss, nspr, at-spi2-core, libcups, libdrm, mesa (libgbm), libxkbcommon, libxcomposite, libxdamage, libxrandr, libxfixes, libxext, libx11, libxcb, alsa-lib, pango, cairo, dbus, expat, systemd-libs (libudev) |
| Loaded with `dlopen` (windowing and audio) | libxcursor, libxi, wayland, libpulse (PipeWire's pulse server works too) |
| Packaging | hicolor-icon-theme |

`optdepends`:

- A Vulkan driver, which is required in practice: `vulkan-radeon`,
  `vulkan-intel` or `nvidia-utils`. They are listed as optional because the
  tarball bundles the Vulkan loader and SwiftShader, and because
  `vulkan-driver` is a virtual provide that can't be named in `optdepends`
  usefully.
- Media: `gst-plugins-bad` / `gst-libav` for more GStreamer formats, or `libvlc`
  for the VLC plugin. (`gst-plugins-base-libs` and `gst-plugins-good` are
  `depends`: without good, parcel music fails silently -- no HTTP source, MP3
  decoder or PulseAudio sink.)
- Desktop extras: `libdecor`, `xdg-utils`, `libnotify`, `gamemode`,
  `switcheroo-control`.

Voice uses WebRTC (`libllwebrtc.so`, 64-bit). The legacy 32-bit Vivox/SLVoice
files are no longer shipped (#678), so the package needs no lib32 dependencies.

namcap also reports the following, all expected for a `-bin` repackage: ELF
files under `/opt`, unstripped binaries (`options=(!strip)`), missing
RELRO/PIE, and insecure RPATH/RUNPATH in the prebuilt third-party binaries
(dullahan_host, libalut).

## Sandbox (chrome-sandbox)

The package installs `bin/chrome-sandbox` as the tarball ships it: mode 0755,
not setuid root. Reasons:

- The release tarball has never shipped it setuid. The wrapper's
  `chrome_sandboxing_permissions_setup.sh` step (chown root + chmod 4755) is
  commented out. A package install behaves exactly like the tarball.
- Arch kernels enable unprivileged user namespaces, which Chromium prefers
  over the setuid helper. If CEF sandboxing is turned on later, that is the
  route to use, not a setuid binary.
- A setuid-root binary that nothing uses only adds attack surface.

## CI (`.forgejo/workflows/release.yml`, job `arch-package`)

The job runs on `vX.Y.Z` and `vX.Y.Z-beta.N` tags, after the `build` job has
published the Linux tarball. Every step is `continue-on-error`, so the job
can't block or undo the main assets. It does four things:

1. Extracts `scripts/packaging/aur` + `scripts/ci` at the tagged commit.
   `gen.sh` then reads the tarball's sha256 from the release notes and writes
   `PKGBUILD` + `.SRCINFO`.
2. If `makepkg` and `bsdtar` exist on the runner, downloads the tarball over
   the LAN and builds the package with `makepkg -f -d`. `-d` is needed because
   the runner isn't Arch and has no pacman database.
3. Attaches `PKGBUILD` (and the `.pkg.tar.zst` when it was built) to the
   release with `publish_release.sh`, which appends their checksums to the
   notes.
4. Runs `aur_push.sh`, which skips the push while `AUR_SSH_KEY` is unset.

The builder (applejack, Ubuntu 24.04) has no makepkg yet, so until it is
installed only the PKGBUILD gets attached. To enable the package build, run
once on applejack as root:

```sh
apt install makepkg libarchive-tools   # pulls in fakeroot; pacman 6.0.2 from noble/universe
```

On a non-Arch host, `.SRCINFO` comes from `gen.sh`'s built-in writer. On a
machine with makepkg, `gen.sh --check-srcinfo` confirms that it matches
`makepkg --printsrcinfo` byte for byte.

## Signature (#288)

Once the release key exists (`scripts/packaging/keys/README.md`), the
`arch-package` job signs the tarball. It attaches
`Slipstream_<ver>_x86_64.tar.xz.sig` to the release and runs `gen.sh` with
`PGP_FPR`, so the generated PKGBUILD verifies the signature:

```sh
source=(".../${_dir}.tar.xz"
        ".../${_dir}.tar.xz.sig")
sha256sums=('<sha256>'
            'SKIP')
validpgpkeys=('<primary key fingerprint>')
```

makepkg checks the signature against the user's own keyring. Users import the
key once, from a keyserver (`gpg --recv-keys <fingerprint>`; yay and paru
offer to do this) or from the website
(`curl -fsSL https://slipstreamviewer.org/slipstream-release.asc | gpg --import`).
`aur_push.sh` also pushes the key as `keys/pgp/<fingerprint>.asc`, the Arch
convention.

In CI, the makepkg build verifies the signature for real, with a temporary
keyring that holds only the release key. While the signing secrets are unset,
the job skips the signature and the PKGBUILD stays unsigned, as before. The
in-repo `PKGBUILD` is the unsigned template: `gen.sh` refuses `PGP_FPR`
without `-o`.

The built `.pkg.tar.zst` is not signed on purpose. When `pacman -U <url>`
finds a `.sig` it cannot verify, it fails, whereas a missing one is fine
(pacman's default `LocalFileSigLevel`). The package is covered by
`SHA256SUMS.asc`.

## One-time AUR setup

1. Create an account at <https://aur.archlinux.org/register>.
2. Generate a key used only for CI: `ssh-keygen -t ed25519 -N '' -f aur_slipstream -C slipstream-ci`.
   Paste `aur_slipstream.pub` into the AUR account (My Account > SSH Public Key).
3. In Forgejo, go to sky_line/Slipstream > Settings > Actions > Secrets and
   add `AUR_SSH_KEY` with the contents of the private key `aur_slipstream`.
   Then delete the local copy, or keep it somewhere safe.
4. Optional: add the repository variable `AUR_PUBLISH_BETAS=false` to push
   only final releases. The default pushes betas too, because every build so
   far is a beta and the AUR package would otherwise stay empty.
5. Optional: create the package now instead of waiting for the next tag. In a
   checkout:
   ```sh
   scripts/packaging/aur/gen.sh -o /tmp/aur v0.14.0-beta.1
   AUR_SSH_KEY="$(cat aur_slipstream)" scripts/packaging/aur/aur_push.sh /tmp/aur v0.14.0-beta.1
   ```
   The first push to `ssh://aur@aur.archlinux.org/slipstream-bin.git` creates
   the package, and the account becomes its maintainer. If you skip this step,
   the next tag's CI run does the same.

After that, every release tag updates the AUR package automatically.

## Local checks

```sh
scripts/packaging/aur/gen.sh --selftest          # pkgver ordering (vercmp)
scripts/packaging/aur/gen.sh --check-srcinfo     # built-in .SRCINFO == makepkg's
cd "$(mktemp -d)" && cp <repo>/scripts/packaging/aur/PKGBUILD . && makepkg -f   # build, don't install
```
