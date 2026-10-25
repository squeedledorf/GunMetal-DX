# apt repository and Linux signing: one-time infrastructure (#288)

This is the runbook for the main session. Follow the steps in order. Each step
says which host it runs on. Nothing here is automated. Back up every config
file before you edit it (`cp X X.bak-apt-288`), and run `nginx -t` before
every reload.

| Name | Value |
|---|---|
| Public URL | `https://apt.slipstreamviewer.org` |
| Runner (CI) | applejack, 192.168.96.24, user `ci`, state in `/srv/ci/apt-repo` |
| Web host | starswirl, 192.168.96.20, nginx :8080, docroot `/srv/apt.slipstreamviewer.org`, deploy user `aptdeploy` |
| Edge | purplesmart, 192.168.96.21, cloudflared tunnel `049ca87c-3bf3-493e-b247-a3c71a8a91d6`, nginx 127.0.0.1:8443 |
| Deploy key | `/home/ci/.ssh/slipstream_apt_deploy` on applejack, forced to `rrsync /srv/apt.slipstreamviewer.org` |
| Secrets | `LINUX_SIGNING_KEY`, `LINUX_SIGNING_KEY_ID`, optional `LINUX_SIGNING_KEY_PASSPHRASE` |

## 0. The key (maintainer)

The maintainer creates the key and sets the secrets by following
`scripts/packaging/keys/README.md`. Until then every signing job only warns
and skips. You can do steps 1-5 first.

## 1. applejack: packages and the persistent repo directory (root)

```sh
apt update
apt install aptly gnupg gpgv rsync curl python3 dpkg
aptly version            # expect 1.5.0+ds1-2ubuntu0.24.04.x (noble-updates)
install -d -o ci -g ci -m 755 /srv/ci/apt-repo
# Optional, for the arch-package job's makepkg build and signature check
# (scripts/packaging/aur/README.md):
apt install makepkg libarchive-tools
```

`gnupg` brings `gpg-agent` and `gpg-preset-passphrase`, which
`scripts/ci/signing_key.sh` needs.

## 2. applejack: deploy key (as ci)

```sh
sudo -u ci ssh-keygen -t ed25519 -N '' -C slipstream-apt-deploy -f /home/ci/.ssh/slipstream_apt_deploy
cat /home/ci/.ssh/slipstream_apt_deploy.pub     # goes into step 3
```

## 3. starswirl: deploy user, docroot, rrsync (root)

Mirror the existing `sitedeploy` setup. Look at it first:
`getent passwd sitedeploy; cat ~sitedeploy/.ssh/authorized_keys`.

```sh
adduser --system --group --home /home/aptdeploy --shell /bin/sh aptdeploy
install -d -o aptdeploy -g aptdeploy -m 755 /srv/apt.slipstreamviewer.org
install -d -o aptdeploy -g aptdeploy -m 700 /home/aptdeploy/.ssh
command -v rrsync        # Debian 12: /usr/bin/rrsync (package rsync)
cat > /home/aptdeploy/.ssh/authorized_keys <<'EOF'
command="/usr/bin/rrsync /srv/apt.slipstreamviewer.org",restrict ssh-ed25519 AAAA...PASTE... slipstream-apt-deploy
EOF
chown aptdeploy:aptdeploy /home/aptdeploy/.ssh/authorized_keys; chmod 600 /home/aptdeploy/.ssh/authorized_keys
```

If sshd restricts logins (`AllowUsers` / `AllowGroups` in
`/etc/ssh/sshd_config*`), add `aptdeploy` the way `sitedeploy` is added, then
reload sshd.

Test it from applejack as ci. This also records the host key:

```sh
sudo -u ci bash -c 'mkdir -p /tmp/apttest && echo ok > /tmp/apttest/probe &&
  rsync -rlt -e "ssh -i ~/.ssh/slipstream_apt_deploy -o BatchMode=yes -o StrictHostKeyChecking=accept-new" \
    /tmp/apttest/ aptdeploy@192.168.96.20: && rm -rf /tmp/apttest'
ls -l /srv/apt.slipstreamviewer.org/probe && rm /srv/apt.slipstreamviewer.org/probe    # on starswirl
```

`apt_repo.sh deploy` uses `--delete` and `--delay-updates`, which rrsync
allows by default. Do not pass `-no-del`.

## 4. starswirl: nginx server block (root)

Find the block that serves the website (`grep -rn 'listen.*8080' /etc/nginx/`)
and copy its access rules (it is LAN-only: `allow 192.168.96.0/24; deny all;`).
Then add `/etc/nginx/sites-available/apt.slipstreamviewer.org` (link it into
`sites-enabled`), or a `conf.d` file, whichever the existing site uses:

```nginx
server {
    listen 8080;
    server_name apt.slipstreamviewer.org;
    root /srv/apt.slipstreamviewer.org;

    # Same access policy as the website block (only purplesmart / the LAN).
    allow 192.168.96.0/24;
    deny all;

    autoindex on;

    # Indexes must never be cached stale: InRelease and the Packages files it
    # lists change together. Cloudflare honours these headers.
    location /dists/ {
        add_header Cache-Control "no-cache" always;
    }
    # By-hash names are content-addressed, so they are safe to cache.
    location ~ ^/dists/.+/by-hash/ {
        add_header Cache-Control "public, max-age=86400, immutable" always;
    }
    # A pool file never changes under the same name (aptly refuses that).
    location /pool/ {
        add_header Cache-Control "public, max-age=604800, immutable" always;
    }
    # Plain text in a browser. Scoped here: a server-level `types` block would
    # replace the whole MIME table (index.html would download as a binary).
    location ~ \.(sources|asc)$ {
        types { }
        default_type text/plain;
        add_header Cache-Control "public, max-age=3600" always;
    }
}
```

```sh
nginx -t && systemctl reload nginx
curl -sI -H 'Host: apt.slipstreamviewer.org' http://192.168.96.20:8080/ | head -1    # 200 or 403 (autoindex of an empty dir)
```

## 5. purplesmart: vhost, origin certificate, tunnel (root)

1. **Certificate.** Check whether the existing Origin CA certificate already
   covers subdomains:
   ```sh
   openssl x509 -in /etc/nginx/slipstreamviewer.org/slipstreamviewer.org.pem -noout -ext subjectAltName
   ```
   If it lists `*.slipstreamviewer.org`, reuse it. If not, **the user** creates
   a new one: Cloudflare dashboard, zone slipstreamviewer.org, SSL/TLS >
   Origin Server > Create Certificate, hostnames `slipstreamviewer.org,
   *.slipstreamviewer.org`, 15 years, PEM. Install it as
   `/etc/nginx/apt.slipstreamviewer.org/apt.slipstreamviewer.org.{pem,key}`
   (key mode 600) and use those paths below.
2. **vhost.** Add it next to the existing ones, copying their
   `listen`/`ssl`/`http2` lines exactly
   (`/etc/nginx/conf.d/apt.slipstreamviewer.conf`):
   ```nginx
   server {
       listen 127.0.0.1:8443 ssl;
       http2 on;
       server_name apt.slipstreamviewer.org;
       ssl_certificate     /etc/nginx/slipstreamviewer.org/slipstreamviewer.org.pem;
       ssl_certificate_key /etc/nginx/slipstreamviewer.org/slipstreamviewer.org.key;

       location / {
           proxy_pass http://192.168.96.20:8080;
           proxy_set_header Host apt.slipstreamviewer.org;
           proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
           proxy_set_header X-Forwarded-Proto https;
           # .debs are ~200 MB: stream them instead of spooling to disk.
           proxy_buffering off;
           proxy_read_timeout 300s;
       }
   }
   ```
   Then run `nginx -t && systemctl reload nginx`.
3. **Tunnel ingress.** Check `/root/.cloudflared/config.yml`. If its ingress
   is one catch-all `service: https://127.0.0.1:8443`, nothing changes. If it
   lists hostnames, add the following entry before the final catch-all and run
   `systemctl restart cloudflared`:
   ```yaml
     - hostname: apt.slipstreamviewer.org
       service: https://127.0.0.1:8443
       originRequest:
         noTLSVerify: true
   ```
   Do **not** use `cloudflared tunnel route dns`. Its cert is scoped to
   cloudsdale.horse and would create a bogus `apt.slipstreamviewer.org.cloudsdale.horse`
   record. The user adds the DNS record (step 6).

## 6. Cloudflare DNS (the USER, in the dashboard)

Zone **slipstreamviewer.org**, DNS > Records > Add record:

| Type | Name | Target | Proxy |
|---|---|---|---|
| CNAME | `apt` | `049ca87c-3bf3-493e-b247-a3c71a8a91d6.cfargotunnel.com` | Proxied (orange cloud) |

Optional belt and braces for caching: Caching > Cache Rules > "apt indexes",
when hostname equals `apt.slipstreamviewer.org` and URI path starts with
`/dists/`, set **Bypass cache**. The origin's `Cache-Control: no-cache`
already prevents stale indexes, and `.deb` is not a default-cached extension.

The website already serves `/slipstream-release.asc` from its own docroot, so
it needs no DNS change.

## 7. Forgejo secrets (maintainer, see keys/README.md)

Set these in sky_line/Slipstream > Settings > Actions > Secrets:

- `LINUX_SIGNING_KEY`: armored secret **subkey** export.
- `LINUX_SIGNING_KEY_ID`: primary key fingerprint (40 hex).
- `LINUX_SIGNING_KEY_PASSPHRASE`: only if the subkey export has a passphrase.

Commit the real `scripts/packaging/keys/slipstream-release.asc` and set
`keyring-version`. Without them the repo is still signed, but the
`slipstream-archive-keyring` package is not built, and the AUR gets no
`keys/pgp/` file.

## 8. Seed the repo with v1.0.0-beta.6

Once steps 1-7 are done and the commit with these scripts is on `main`, run
the **linux-signing** workflow (Actions tab > linux-signing > Run workflow) with
`apt_tags = v1.0.0-beta.6` and `sign_tag = v1.0.0-beta.6`, or use the API:

```sh
curl -fsS -X POST -H "Authorization: token $(cat ~/.config/forgejo-token)" -H 'Content-Type: application/json' \
  -d '{"ref":"main","inputs":{"apt_tags":"v1.0.0-beta.6","sign_tag":"v1.0.0-beta.6"}}' \
  http://192.168.96.20:3000/api/v1/repos/sky_line/Slipstream/actions/workflows/linux-signing.yml/dispatches
```

This adds `Slipstream_1.0.0-beta.6_amd64.deb` to the `beta` suite (after
checking it against the sha256 in the release notes). It then publishes and
signs `stable` (empty until the first final release, plus the keyring
package) and `beta`, deploys, and attaches `SHA256SUMS(.asc)` to the
v1.0.0-beta.6 release. Add older tags only if their release has an
`_amd64.deb` (`seed` fails otherwise).

Check:

```sh
curl -fsS https://apt.slipstreamviewer.org/dists/beta/InRelease | head -12
curl -fsSI https://apt.slipstreamviewer.org/dists/beta/InRelease | grep -i cache-control    # no-cache
sudo -u ci aptly -config=/srv/ci/apt-repo/aptly.conf repo show -with-packages beta          # on applejack
```

From then on, every `v*` tag updates the repo by itself (`release.yml`
job `apt-repo`).

## 9. Test plan

Run this on Ubuntu 24.04, Linux Mint 22 and Debian 13: real machines or VMs
for the desktop check, containers for the apt part.

1. Add the repo with the website snippet (beta suite while only betas exist):
   ```sh
   sudo wget -qO /usr/share/keyrings/slipstream-archive-keyring.asc https://apt.slipstreamviewer.org/slipstream-archive-keyring.asc
   sudo wget -qO /etc/apt/sources.list.d/slipstream.sources https://apt.slipstreamviewer.org/slipstream-beta.sources
   sudo apt update                                   # no GPG warnings, no "not signed"
   ```
2. `sudo apt install slipstream-viewer slipstream-archive-keyring`. Then:
   - `dpkg -S /usr/share/keyrings/slipstream-archive-keyring.asc` names the
     keyring package (once it exists).
   - `slipstream` starts from the menu, and a `secondlife://` link opens it.
3. `apt-cache policy slipstream-viewer` lists up to 3 versions. Downgrade one
   (`sudo apt install slipstream-viewer=<older>`). After the next tag (or on a
   machine that installed beta.6 before the next beta landed), run
   `sudo apt update && sudo apt upgrade`. The viewer moves to the newest
   version, and user data in `~/.slipstreamviewer` is untouched.
4. Switch to `Suites: stable` once a final release exists. `apt update`
   works, and `apt install` offers only finals.
5. Negative checks:
   - Change one byte of a downloaded `Packages` file under
     `/var/lib/apt/lists/` and re-run `apt update`: apt refetches it.
   - Point `Signed-By` at another key: apt refuses with "NO_PUBKEY" (24.04) or
     "Missing key" (sqv, Debian 13).
6. Remove: `sudo apt remove slipstream-viewer slipstream-archive-keyring` and
   delete the `.sources` file. `apt update` is clean again.

Before the real key existed, the publishing and the client side were tested
in containers with a throwaway key. That covered aptly publish/prune/re-add/
conflict, gpgv + index verification, install, keyring takeover, downgrade,
upgrade, stable vs beta, and tamper rejection on `ubuntu:24.04` (apt 2.8,
gpgv) and `debian:trixie` (apt 3.0, sqv). To repeat it, run `apt_repo.sh` in
an `ubuntu:24.04` container with `aptly dpkg-dev gnupg gpgv`, a throwaway key
in an ephemeral `GNUPGHOME` and `APT_REPO_ROOT`/`APT_URL=file:///repo`. Then
mount `aptly/public` at `/repo` in client containers and use the `.sources`
file as is.

## Operations

- **State lost** (`/srv/ci/apt-repo` gone): recreate the directory (step 1)
  and run linux-signing with `apt_tags` = the newest 3 beta tags plus the
  newest 3 final tags that have .debs.
- **Keyring bump** (key extended or rotated): commit the new
  `slipstream-release.asc` + `keyring-version`, then run linux-signing with
  `apt_tags = -` to publish it without waiting for a release.
- **A late asset** (e.g. a Flatpak bundle attached after `sign-release`): run
  linux-signing with `sign_tag = <tag>`. It replaces `SHA256SUMS(.asc)`.
- **Retention:** `APT_KEEP` (default 3) in the job environment. Disk: about
  2 x `APT_KEEP` .debs on applejack and on starswirl.
