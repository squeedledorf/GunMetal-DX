# Slipstream Flatpak: files, design and infrastructure runbook (#286)

The official Flatpak repackages the Linux release tarball (same binary, nothing
rebuilt) as `org.slipstreamviewer.Slipstream` on the freedesktop 25.08 runtime.
It is published in our own GPG-signed Flatpak repo at
`https://flatpak.slipstreamviewer.org/repo/`, with two branches:

| Tag | Branch | Who gets it |
|---|---|---|
| `vX.Y.Z` | `stable`, **and** copied to `beta` | everyone (beta users get each final too) |
| `vX.Y.Z-beta.N` | `beta` | beta users |
| `ci-*` | `test` in a throwaway repo, unsigned | nobody (CI dry run of the pipeline) |

Users install with
`flatpak install https://flatpak.slipstreamviewer.org/slipstream.flatpakref`
(beta: `slipstream-beta.flatpakref`), then get updates through `flatpak update`,
GNOME Software or Discover. Each release also carries `Slipstream_<ver>.flatpak`,
a single-file bundle for offline installs that still updates from the repo.

Until the first final release exists, `slipstream.flatpakref` installs the `beta`
branch, because there is no `stable` ref to install yet (`build_flatpak.sh`
checks the repo). The website link therefore always works. The repo's
default branch (used by `flatpak install slipstream org.slipstreamviewer.Slipstream`
without a branch) follows the same rule. Those early installs stay on `beta`,
which also receives every final. To move to stable later:
`flatpak install https://flatpak.slipstreamviewer.org/slipstream.flatpakref`,
then `flatpak uninstall org.slipstreamviewer.Slipstream//beta`. The data in
`~/.var/app` is shared by both branches.

## Files

| File | Purpose |
|---|---|
| `org.slipstreamviewer.Slipstream.yml` | The manifest: runtime, finish-args (sandbox permissions), install steps. |
| `slipstream-flatpak.sh` | `/app/bin/slipstream`: runs the tarball's own wrapper (`/app/slipstream/slipstream`). |
| `org.slipstreamviewer.Slipstream.desktop` | Menu entry + `secondlife://` handler. |
| `org.slipstreamviewer.Slipstream.metainfo.xml.in` | AppStream template; `gen_metainfo.sh` adds `<releases>` from the git tags. |
| `gen_metainfo.sh` | Writes the metainfo for a tag and validates it with `appstreamcli`. |
| `build_flatpak.sh` | Tarball -> flatpak-builder -> signed commit -> library check -> repo update (static deltas, prune) -> bundle -> repo files. CI calls it; it runs by hand too. |
| `repo_files.sh`, `repo/*.in` | `slipstream.flatpakrepo`, `slipstream.flatpakref`, `slipstream-beta.flatpakref` with the public key embedded (`GPGKey=`), filled at build time from the signing key. |
| `publish_flatpak_repo.sh` | `pull` the published repo when the runner has none; `push` it (objects first, then refs + signed summary, then deletes) and the repo files to the host. |
| `.forgejo/workflows/release.yml`, job `flatpak` | Runs all of the above on a tag, after `build`. Every step is continue-on-error. |

## Design notes

### Runtime and libraries

The tarball needs glibc >= 2.38 and GLIBCXX_3.4.32. freedesktop 25.08 has glibc
2.42 and GCC 15's libstdc++. Checked with `ldd` inside
`org.freedesktop.Platform//25.08` against the v1.0.0-beta.6 tarball: every
64-bit ELF resolves except the VLC media plugin (libvlc), which is unused: in-world
media and music go through the GStreamer plugin (`mime_types_linux.xml`). The
GStreamer plugins come from the runtime, and H.264/AAC from the `codecs-extra`
extension, which the runtime declares and flatpak installs automatically. The
dlopen'd libraries are all in the runtime too: libdecor + its GTK and cairo
plugins, PulseAudio, PipeWire, Wayland, X11, xkbcommon, NSS/CUPS/ATK for CEF,
libnotify, gamemode and libsecret. Nothing extra is bundled.
`build_flatpak.sh` repeats the check after every build, against the Platform
(not the SDK, which has more libraries), and fails on a missing library. So
moving `runtime-version` to 26.08 (on Flathub since September 2026) is a
one-line change that the next build verifies.

Changes to the tarball tree inside the Flatpak:

- `.packaged-by` = `flatpak`: the wrapper then skips the per-user desktop entry,
  the `secondlife://` registration and the missing-library check, as for the .deb
  and the AUR package.
- `lib/libvulkan.so.1` is removed, so the runtime's Vulkan loader is used. It is
  maintained together with the GL extension that carries the ICDs. The runtime's
  `/usr/share/vulkan/icd.d` is a symlink to `/usr/lib/x86_64-linux-gnu/GL/vulkan/icd.d`,
  so the bundled loader would also have found the drivers. Every `vk*` symbol
  the viewer links resolves against the runtime loader (`ldd -r`).
- The VLC and example media plugins are removed (Vivox/SLVoice is no longer
  shipped at all; voice is WebRTC).
- The wrapper's `SDL_VIDEO_{WAYLAND,X11}_WMCLASS` now honours a preset value
  (`wrapper.sh`). The launcher sets it to the app ID, so GNOME and KDE match the
  window to the desktop file's icon. For tarballs from before that change, the
  manifest patches the wrapper copy with `sed`.
- CEF already runs without its setuid sandbox (as in the tarball and the .deb).
  `chrome-sandbox` stays, unused.
- The binaries are not stripped and no debuginfo is extracted
  (`no-debuginfo`, `strip: false`).

### Data directory: `--persist=.slipstreamviewer`

`lldir_linux.cpp` takes the home directory from `getpwuid` and uses
`~/.slipstreamviewer`. Inside a Flatpak, HOME is the real home, so the choices were:

1. `--persist=.slipstreamviewer` (**chosen**): the sandbox sees
   `~/.slipstreamviewer`, stored on the host in
   `~/.var/app/org.slipstreamviewer.Slipstream/.slipstreamviewer`. There are no
   code changes. The paths in logs, settings and support answers are the same as
   for the tarball. Nothing outside `~/.var/app` is writable. `flatpak uninstall
   --delete-data` removes it. This is also the pattern Flathub accepts.
2. `--filesystem=~/.slipstreamviewer:create` would share the data with a tarball
   or .deb install. That is write access outside the sandbox. It also lets two
   different builds fight over one cache and settings file, and it gets
   challenged on Flathub.
3. XDG (`$XDG_DATA_HOME` via `SLIPSTREAM_USER_DIR`) would separate the data
   like option 1, but with different paths from every other install. That
   breaks the wiki's "where your settings live" and support answers for no
   gain.

A user moving from the tarball copies `~/.slipstreamviewer` to
`~/.var/app/org.slipstreamviewer.Slipstream/` once (wiki, "Flatpak").

### Permissions (finish-args) and what was checked

| Permission | Why |
|---|---|
| `--socket=wayland`, `--socket=fallback-x11`, `--share=ipc` | SDL3 window; X11 only without Wayland. |
| `--device=dri` | GPU (Vulkan). |
| `--device=input` | Game controllers / 3D mice (SDL). Needs flatpak >= 1.15.6 on the **build** host (see applejack below); older clients ignore it. |
| `--share=network` | Second Life. |
| `--socket=pulseaudio`, `--filesystem=xdg-run/pipewire-0` | OpenAL, WebRTC voice, SDL audio. |
| `--persist=.slipstreamviewer` | User data, see above. |
| `--filesystem=~/.firestorm_x64:ro` (and `~/.firestorm`, `~/.secondlife`, `~/.alchemyviewer`, `~/.alchemynext`, `~/.blackdragon`) | Viewer import (`findSourceViewers`). Import only writes into our own data dir: the `.pre-import` backups go next to the destination. |
| `--own-name=com.secondlife.ViewerAppAPIService` | A second launch with a `secondlife://` URL hands it to the running viewer over D-Bus (`llappviewerlinux.cpp`). |
| `--filesystem=xdg-run/discord-ipc-0`, `--filesystem=xdg-run/app/com.discordapp.Discord:create` | Discord status (`sldiscordpresence.cpp`): the local Discord IPC socket of a native Discord (bound only if it exists at launch) and of Flatpak Discord (directory created so it works whichever starts first). |

No permission is needed for:

- **File pickers.** These are SDL3's `SDL_ShowFileDialogWithProperties`, which
  uses the FileChooser portal first. The picked files and folders arrive
  through the document portal (`/run/user/<uid>/doc/...`). The snapshot folder
  (`SnapshotBaseDir`) is chosen with the folder picker and stays writable across
  sessions.
- **Web links.** `SDL_OpenURL` runs `xdg-open`. `/usr/bin/xdg-open` in the
  runtime is flatpak-xdg-utils, which forwards to the OpenURI portal.
- **gamemode.** libgamemode talks to the GameMode portal inside a Flatpak.

### Follow-ups that need viewer code (not done here)

1. **Importer sandbox hint.** When `/.flatpak-info` exists and a known viewer
   folder is not visible, or the source viewer's `InstantMessageLogPath` points
   outside the granted folders (Firestorm users often keep chat logs in
   `~/Documents/...`), say so and show
   `flatpak override --user --filesystem=<path>:ro org.slipstreamviewer.Slipstream`.
   Today those transcripts are silently skipped. This was the class of the
   unofficial-Flatpak "Firestorm chat import" report.
2. **External script editor** (`llexternaleditor.cpp`) runs `/usr/bin/xdg-open
   <tmpfile>`. The temp file is in the sandbox's private `/tmp`, so a host editor
   may not be able to save into it. Test this; if it fails, put the temp file
   under `$XDG_CACHE_HOME` and open it through the OpenURI portal with write
   access.
3. **"Show in folder"** (`lldir.cpp`) runs `xdg-open <dir>`. flatpak-xdg-open
   passes local paths to OpenURI's OpenFile. Check that directories open in the
   file manager, or switch to the portal's `OpenDirectory`.
4. **Hybrid-GPU hint.** The wrapper's `switcherooctl` check silently does nothing
   in the sandbox (no `switcherooctl`). The viewer picks the Vulkan device
   itself, so this is cosmetic.
5. **Other viewers' Flatpak data** (`~/.var/app/<id>/...`) isn't granted. Add it
   if a popular viewer ships one.
6. `scripts/ci/publish_release.sh`'s release body lists the tarball, .deb and
   Windows files but not the `.flatpak`. Add one line once #288's edits to that
   script have landed (it was left alone to avoid a conflict).

## One-time infrastructure setup (main session)

Order: applejack -> starswirl -> purplesmart -> DNS (user) -> secrets -> manual
test build -> first tag. Back up every file you edit as `*.bak-flatpak-<date>`,
and run `nginx -t` before every reload.

### 1. applejack (CI runner, CT 125, Ubuntu 24.04, user `ci`)

Ubuntu 24.04 ships flatpak 1.14, which rejects `--device=input` in
`build-finish`. Use the Flatpak team's PPA (the route flatpak.org documents for
Ubuntu). To stay on stock apt instead, delete the `--device=input` line from the
manifest.

```sh
# as root on applejack
apt-get install -y software-properties-common
add-apt-repository -y ppa:flatpak/stable
apt-get update
apt-get install -y flatpak flatpak-builder bubblewrap ostree gnupg rsync appstream xz-utils
flatpak --version          # expect >= 1.16
flatpak-builder --version  # expect >= 1.4

# Can user namespaces / bubblewrap run in this LXC (nesting=1)? All three must print ok.
sudo -iu ci unshare -U -r true && echo userns-ok
sudo -iu ci bwrap --unshare-user --ro-bind / / --proc /proc --dev /dev true && echo bwrap-ok

# Runtime + SDK for the ci user (user installation: the runner runs as ci).
sudo -iu ci flatpak --user remote-add --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
sudo -iu ci flatpak --user install -y --noninteractive flathub org.freedesktop.Platform//25.08 org.freedesktop.Sdk//25.08
sudo -iu ci flatpak run --command=true org.freedesktop.Platform//25.08 && echo flatpak-run-ok

# Persistent repo + work area (the repo is ~1 GB after a few releases; the work dir ~2 GB during a build).
install -d -o ci -g ci /srv/ci/slipstream-flatpak
```

If `unshare`/`bwrap` fail:

1. On the Proxmox host (rainbowcrash), `sysctl kernel.unprivileged_userns_clone`
   must be 1 (the Debian default). `pct config 125` must show `features: nesting=1`.
2. If it still fails in an unprivileged container, add `keyctl=1`
   (`pct set 125 --features nesting=1,keyctl=1`) and restart the CT.
3. Last resort: `chmod u+s /usr/bin/bwrap`. This is a setuid helper, a security
   trade-off, so note it if you use it.

flatpak-builder needs no FUSE: the scripts pass `--disable-rofiles-fuse`.

The runner service must give `ci` a HOME. flatpak `--user` lives in
`~ci/.local/share/flatpak`. Check with
`systemctl show forgejo-runner -p Environment`; if HOME is missing, add
`Environment=HOME=/home/ci` to the unit, `systemctl daemon-reload` and restart
it between jobs.

Deploy key, created on applejack:

```sh
sudo -iu ci ssh-keygen -t ed25519 -N '' -C 'applejack flatpak deploy' -f /home/ci/.ssh/slipstream_flatpak_deploy
cat /home/ci/.ssh/slipstream_flatpak_deploy.pub     # goes into sitedeploy's authorized_keys below
```

### 2. starswirl (static origin, 192.168.96.20, nginx :8080)

Directory, owned by the existing deploy user:

```sh
install -d -o sitedeploy -g sitedeploy -m 755 /srv/flatpak.slipstreamviewer.org
```

Deploy user: reuse `sitedeploy` with a **second** key, locked to the new
directory. Keys are restricted per line, so the website key still only reaches
`/srv/slipstreamviewer.org`. In `~sitedeploy/.ssh/authorized_keys`, copy the
options of the existing website line and change the path:

```
command="rrsync /srv/flatpak.slipstreamviewer.org",restrict ssh-ed25519 AAAA...(the .pub above)... applejack flatpak deploy
```

Do **not** add rrsync's `-wo`: CI reads the repo back (`publish_flatpak_repo.sh
pull`) when its local copy is missing. Check from applejack:

```sh
sudo -iu ci rsync -e 'ssh -i /home/ci/.ssh/slipstream_flatpak_deploy -o StrictHostKeyChecking=accept-new' \
  --list-only sitedeploy@192.168.96.20:     # lists the (empty) directory; anything else = wrong key line
```

nginx server block. Put it next to the website's block and copy the website
block's `allow`/`deny` lines; the website allows only the LAN (purplesmart
proxies from 192.168.96.21). For example `/etc/nginx/sites-available/flatpak.slipstreamviewer.org`,
enabled with a symlink in `sites-enabled` (or `conf.d`, wherever the website's
block lives):

```nginx
server {
    listen 8080;
    server_name flatpak.slipstreamviewer.org;
    root /srv/flatpak.slipstreamviewer.org;

    allow 192.168.96.0/24;
    deny all;

    autoindex off;
    sendfile on;
    tcp_nopush on;
    default_type application/octet-stream;

    # Files that change on every publish: always revalidate.
    location ~ ^/repo/(summary|summary\.sig|summary\.idx|summary\.idx\.sig|config)$ {
        add_header Cache-Control "no-cache" always;
    }
    location ^~ /repo/refs/          { add_header Cache-Control "no-cache" always; }
    location ^~ /repo/delta-indexes/ { add_header Cache-Control "public, max-age=300" always; }
    # Content-addressed: never change once written.
    location ^~ /repo/objects/   { add_header Cache-Control "public, max-age=31536000, immutable" always; }
    location ^~ /repo/deltas/    { add_header Cache-Control "public, max-age=31536000, immutable" always; }
    location ^~ /repo/summaries/ { add_header Cache-Control "public, max-age=31536000, immutable" always; }

    location ~ \.flatpakref$  { default_type application/vnd.flatpak.ref;  add_header Cache-Control "public, max-age=300" always; }
    location ~ \.flatpakrepo$ { default_type application/vnd.flatpak.repo; add_header Cache-Control "public, max-age=300" always; }
    location = /slipstream.gpg { default_type application/pgp-keys; }

    location = / { return 302 https://slipstreamviewer.org/wiki/install/#flatpak; }
}
```

If the website's 8080 block is the `default_server` and purplesmart sends
`Host: slipstreamviewer.org` (check its `proxy_set_header Host`), give this
block its own port (e.g. `listen 8081`) instead of relying on `server_name`,
and point purplesmart there. Then run
`nginx -t && systemctl reload nginx`, and test from purplesmart:
`curl -sI -H 'Host: flatpak.slipstreamviewer.org' http://192.168.96.20:8080/slipstream.flatpakref`.
This returns 404 until the first publish; check for nginx's 404, not 403.

### 3. purplesmart (edge: cloudflared + nginx 127.0.0.1:8443)

**cloudflared:** the ingress in `/root/.cloudflared/config.yml` sends
everything to `https://127.0.0.1:8443` (noTLSVerify), so it needs no change.
Confirm with `grep -A15 '^ingress' /root/.cloudflared/config.yml`. If it lists
hostnames, add `- hostname: flatpak.slipstreamviewer.org` / `service: https://127.0.0.1:8443`
before the catch-all and `systemctl restart cloudflared`.

**Origin certificate:** check whether the existing slipstreamviewer.org
Cloudflare Origin CA cert covers the new name:

```sh
openssl x509 -in /etc/nginx/slipstreamviewer.org/slipstreamviewer.org.pem -noout -ext subjectAltName
```

- If it shows `DNS:*.slipstreamviewer.org` (Cloudflare's default for origin
  certs), reuse it.
- If not, **the user** creates a new Origin CA certificate in the Cloudflare
  dashboard (slipstreamviewer.org -> SSL/TLS -> Origin Server -> Create
  certificate) with hostnames `slipstreamviewer.org` and `*.slipstreamviewer.org`.
  Install it as `/etc/nginx/flatpak.slipstreamviewer.org/flatpak.slipstreamviewer.org.{pem,key}`
  (key 0600), or replace the existing pair, which the wildcard covers too.

**vhost:** add a server block to `/etc/nginx/conf.d/slipstreamviewer.conf`
(after a `.bak-flatpak-<date>` backup). Copy the apex block's
`listen`/`ssl_*`/`http2` lines verbatim; only the parts below differ:

```nginx
server {
    # listen / ssl_certificate / ssl_certificate_key / http2: as in the apex block
    server_name flatpak.slipstreamviewer.org;

    location / {
        proxy_pass http://192.168.96.20:8080;      # or :8081, see starswirl
        proxy_set_header Host flatpak.slipstreamviewer.org;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_http_version 1.1;
        proxy_read_timeout 300s;
        proxy_max_temp_file_size 0;                 # stream big delta parts, don't spool them
        limit_except GET HEAD { deny all; }
    }
}
```

`nginx -t && systemctl reload nginx`.

### 4. DNS (the USER, Cloudflare dashboard, zone slipstreamviewer.org)

| Type | Name | Target | Proxy |
|---|---|---|---|
| CNAME | `flatpak` | `049ca87c-3bf3-493e-b247-a3c71a8a91d6.cfargotunnel.com` | Proxied (orange) |

That is the only new hostname: `flatpak.slipstreamviewer.org`. Universal SSL
covers first-level subdomains. Don't use `cloudflared tunnel route dns`:
purplesmart's `cert.pem` is scoped to cloudsdale.horse and would create
`flatpak.slipstreamviewer.org.cloudsdale.horse`.

Optional, also the user: a Cloudflare redirect rule so
`https://slipstreamviewer.org/slipstream.flatpakref` (the URL in the ticket)
goes 302 to `https://flatpak.slipstreamviewer.org/slipstream.flatpakref`, and
the same for `slipstream-beta.flatpakref` and `slipstream.flatpakrepo`.
flatpak follows redirects. The website links the flatpak host directly, so
this is cosmetic.

### 5. Cloudflare caching and large files

- **What must never be stale.** `summary`, `summary.sig`, `summary.idx(.sig)`,
  `refs/` and `config` change on every publish. A cached old summary makes
  `flatpak update` see no update. A cached new summary that points at objects
  the edge doesn't have yet is also a problem: publishing uploads objects
  first, so this resolves itself.
- **Defaults are safe.** Cloudflare caches only its list of static extensions
  by default. OSTree's `.filez`, `.dirtree`, `.dirmeta`, `.commit` and the
  extensionless summary and delta files are not on it, so everything passes
  through the tunnel to starswirl (correct, but every install pulls about
  170 MB through the tunnel). Also, `.flatpakref` / `.flatpakrepo` / `.gpg` /
  `.png` are not cached for long (the origin sends max-age=300).
- **To offload bandwidth later,** the user adds a Cache Rule for
  `hostname eq flatpak.slipstreamviewer.org and (starts_with(path, "/repo/objects/")
  or starts_with(path, "/repo/deltas/") or starts_with(path, "/repo/summaries/"))`
  -> Eligible for cache, Edge TTL "use cache-control header". Add a second rule,
  ordered after it, that bypasses the cache for `/repo/summary*`, `/repo/refs/`
  and `/repo/config`.
- **Size limits.** The free plan's maximum cacheable file is 512 MB. Static
  delta parts are about 32 MB at most and the largest single objects (libcef.so,
  the viewer binary) are well under that. Download size through the tunnel is
  not capped (the 100 MB limit is on request bodies, i.e. uploads, which go over
  the LAN anyway).
- **Terms.** Cloudflare's CDN terms restrict serving "a disproportionate
  percentage of large files" without a paid product. A small user base is
  fine. If the Flatpak becomes popular, move `/repo/` to R2 (free egress) or
  a non-proxied host.

### 6. CI secrets (repo sky_line/Slipstream -> Settings -> Actions -> Secrets)

The **same project signing key as the apt repo / SHA256SUMS work in #288**:

| Secret | Content |
|---|---|
| `LINUX_SIGNING_KEY` | ASCII-armored secret key: `gpg --armor --export-secret-subkeys <SIGNING_SUBKEY_FPR>!` (subkey only, recommended) or the full secret key. |
| `LINUX_SIGNING_KEY_ID` | Fingerprint to sign with: the signing subkey's or the primary's. Both were tested with a subkey-only export (`sec#` stub + `ssb [S]`): ostree signs with the subkey either way. |
| `LINUX_SIGNING_KEY_PASSPHRASE` | Optional. Only if the exported key is passphrase-protected; it is preset into a throwaway agent, and nothing ever prompts (`pinentry-program /bin/false`). |
| `RELEASE_TOKEN` | Already exists; attaches the bundle. |

The deploy key is **not** a secret: it is the file
`/home/ci/.ssh/slipstream_flatpak_deploy` on applejack, like the website's
`slipstream_site_deploy`. The `.flatpakrepo` / `.flatpakref` files and
`slipstream.gpg` are generated from the imported key at build time, so there
is no public key to commit. When #288 commits one, compare fingerprints:
`gpg --show-keys slipstream.gpg`.

Re-check the secrets after any Forgejo major upgrade: the 11->15 upgrade
dropped `RELEASE_TOKEN` once.

### 7. Manual test build on applejack (before the first tag)

```sh
# from the workstation: copy the packaging scripts (the merged branch isn't on applejack yet)
ssh root@192.168.96.24 install -d -o ci -g ci /srv/ci/slipstream-flatpak/manual
scp -r scripts/packaging/flatpak root@192.168.96.24:/srv/ci/slipstream-flatpak/manual/
ssh root@192.168.96.24 chown -R ci:ci /srv/ci/slipstream-flatpak/manual

# on applejack
sudo -iu ci bash -c '
  set -e; cd /srv/ci/slipstream-flatpak/manual
  curl -fsSLO http://192.168.96.20:3000/sky_line/Slipstream/releases/download/v1.0.0-beta.6/Slipstream_1.0.0-beta.6_x86_64.tar.xz
  UNSIGNED=1 BRANCH=test TMPDIR=$PWD TAGS_REPO=/srv/ci/slipstream \
    bash flatpak/build_flatpak.sh Slipstream_1.0.0-beta.6_x86_64.tar.xz v1.0.0-beta.6 $PWD/repo $PWD/out
'
```

If the library check step fails without printing any `MISSING` line, the
problem is the sandbox (`flatpak run` under the runner service: no
`XDG_RUNTIME_DIR` or session bus), not the libraries. Confirm that
`sudo -iu ci flatpak run --command=true org.freedesktop.Platform//25.08` works,
then set `SKIP_LIBCHECK: 1` in the job's env until it is sorted.

Expect the commit, `runtime: ldd (GNU libc) 2.42` with no `MISSING` lines,
deltas, and `out/Slipstream_1.0.0-beta.6.flatpak` (about 170 MB). Then delete
`manual/`. After merge, a `ci-*` tag runs the same pipeline in CI (unsigned,
`test` branch, throwaway repo, nothing published).

For a signed dry run with a throwaway key, see "Testing signing" below. Never
use `~/.gnupg`.

### 8. First release and test plan

1. Tag `vX.Y.Z-beta.N` as usual. The `flatpak` job runs after `build` (about
   10 min). On the host it creates `repo/` with the `beta` branch,
   `slipstream.flatpakrepo`, both `.flatpakref`s, `slipstream.gpg` and
   `slipstream.png`, and it attaches `Slipstream_<ver>.flatpak` to the release.
2. From the LAN:
   `curl -s https://flatpak.slipstreamviewer.org/slipstream-beta.flatpakref`
   shows a `GPGKey=` line, and
   `curl -sI https://flatpak.slipstreamviewer.org/repo/summary.sig` returns 200.
3. **Fedora 43** (Workstation; flatpak + Flathub preinstalled), clean VM:
   - `flatpak install https://flatpak.slipstreamviewer.org/slipstream-beta.flatpakref`
     (it offers to install the 25.08 runtime from Flathub).
   - Start it from the menu. Log in.
   - **Rendering:** Vulkan on Mesa (virtio/Intel/AMD) and on an NVIDIA host;
     flatpak pulls `org.freedesktop.Platform.GL.nvidia-<driver>` automatically,
     run `flatpak update` if it says the driver extension is missing. Check
     `Slipstream.log` for the GPU name.
   - **Media and voice:** in-world media / a music stream (GStreamer +
     codecs-extra), voice in a voice-enabled region (WebRTC).
   - **Web:** web profiles and search (CEF).
   - **Snapshots:** save to disk (the folder picker is the portal's), then a
     second save into the same folder without a picker.
   - **Import:** unpack a Firestorm tarball, log in once so
     `~/.firestorm_x64` exists, then run the importer. Settings, contacts and
     chat transcripts arrive.
   - **SLURLs:** click a `secondlife://` link in Firefox. It starts the viewer,
     or hands the link to the running one.
   - `flatpak info --show-permissions org.slipstreamviewer.Slipstream` matches
     the manifest.
   - Data lands in `~/.var/app/org.slipstreamviewer.Slipstream/.slipstreamviewer`.
4. **Steam Deck** (desktop mode, Konsole):
   `flatpak install --user https://flatpak.slipstreamviewer.org/slipstream-beta.flatpakref`
   (or open the file in Discover). Run the same checks with RADV, plus a
   controller (`--device=input`) and the on-screen keyboard.
5. **Updates:** tag the next beta, then `flatpak update` on both machines picks
   it up (a static delta, much smaller than a fresh install). After the next
   final `vX.Y.Z`: `flatpak install .../slipstream.flatpakref` gets `stable`,
   and beta installs update to the final too.
6. **Bundle:** on a third machine,
   `flatpak install --user Slipstream_<ver>.flatpak` adds the
   `slipstream-origin` remote, and `flatpak update` then follows the repo.
7. **Uninstall:**
   `flatpak uninstall --delete-data org.slipstreamviewer.Slipstream`.

## Building by hand

```sh
# needs flatpak, flatpak-builder and flathub as a --user remote
UNSIGNED=1 BRANCH=test scripts/packaging/flatpak/build_flatpak.sh \
  Slipstream_<ver>_x86_64.tar.xz v<ver> /tmp/fp/repo /tmp/fp/out
flatpak --user remote-add --no-gpg-verify slipstream-local /tmp/fp/repo
flatpak --user install slipstream-local org.slipstreamviewer.Slipstream//test
flatpak run org.slipstreamviewer.Slipstream//test
```

## Testing signing

Use a throwaway key in a temporary GNUPGHOME, never `~/.gnupg`:

```sh
G=$(mktemp -d /tmp/gk.XXXXXX); chmod 700 $G
gpg --homedir $G --batch --pinentry-mode loopback --passphrase '' --quick-gen-key 'Slipstream Test <test@example.invalid>' ed25519 sign 1d
FPR=$(gpg --homedir $G --batch --with-colons --list-secret-keys | awk -F: '$1=="fpr"{print $10; exit}')
SIGNING_KEY="$(gpg --homedir $G --batch --pinentry-mode loopback --passphrase '' --armor --export-secret-keys $FPR)" \
SIGNING_KEY_ID=$FPR scripts/packaging/flatpak/build_flatpak.sh <tarball> v<ver> /tmp/fp/repo /tmp/fp/out
gpgconf --homedir $G --kill all; rm -rf $G
```

`build_flatpak.sh` imports the key into its own throwaway GNUPGHOME (deleted on
exit). It checks that the key can sign before it builds, and fails rather than
prompts.

Verified that way on 2026-10-04, on CachyOS with flatpak 1.18, against
v1.0.0-beta.6. The commits were made with `flatpak build-export` instead of
flatpak-builder, which isn't installed there:

- signed commits and summary, plus static deltas;
- install from the repo and from the bundle, with GPG verification, into a
  throwaway `FLATPAK_USER_DIR`;
- a beta-to-final `flatpak update` through the delta;
- pull and push through an emulated rrsync host;
- a passphrase-protected key, with no prompt.

The flatpak-builder run itself, appstream compose included, is first exercised
on applejack (step 7).
## Build VM (as set up)

The `flatpak` job runs on its own VM, **applejack-flatpak** (192.168.96.26,
Ubuntu 24.04, runner label `flatpak`), not on applejack: flatpak-builder needs
unprivileged user namespaces, which the Proxmox host disables for its
containers (`kernel.unprivileged_userns_clone=0`, deliberate hardening). A VM
has its own kernel, so the host stays hardened.

- Packages: flatpak/flatpak-builder from `ppa:flatpak/stable`, plus bubblewrap,
  ostree, gnupg, rsync, appstream, git; apt forced to IPv4.
- `/etc/apparmor.d/bwrap`: Ubuntu 24.04 restricts unprivileged user namespaces
  per AppArmor profile; this profile grants `userns` to `/usr/bin/bwrap` (the
  pattern Ubuntu uses for flatpak/Chrome).
- User `ci` runs `forgejo-runner` (v13.2.0, `/etc/systemd/system/forgejo-runner.service`),
  with the freedesktop 25.08 Platform + Sdk in its user installation.
- `/srv/ci/slipstream`: the VM's own clone; the job fetches tags + the commit first.
- Deploy key `/home/ci/.ssh/slipstream_flatpak_deploy` (comment `flatpak-vm-deploy`),
  locked in starswirl's `sitedeploy` authorized_keys to `rrsync /srv/flatpak.slipstreamviewer.org`.
- SSH: key-only (`/etc/ssh/sshd_config.d/00-hardening.conf`).
- `ci-*` dry runs package the newest published release (the build job's unpublished
  tarball stays on applejack).
