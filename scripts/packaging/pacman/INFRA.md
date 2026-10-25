# pacman repository: one-time infrastructure (#419)

Runbook for the owner. Nothing here is automated, and nothing was done on any server. It mirrors
the apt repository setup (`../apt/INFRA.md`); the CI job only warns and skips until step 2 exists.
Back up every config file before you edit it, and run `nginx -t` before every reload.

| Name | Value |
|---|---|
| Public URL | `https://arch.slipstreamviewer.org` |
| Runner (CI) | applejack, user `ci`, state in `/srv/ci/pacman-repo` |
| Web host | starswirl, nginx :8080, docroot `/srv/arch.slipstreamviewer.org`, deploy user `archdeploy` |
| Edge | purplesmart (cloudflared tunnel + nginx), same pattern as `apt.slipstreamviewer.org` |
| Deploy key | `/home/ci/.ssh/slipstream_arch_deploy` on applejack, forced to `rrsync /srv/arch.slipstreamviewer.org` |
| Secrets | the existing `LINUX_SIGNING_KEY`, `LINUX_SIGNING_KEY_ID` (+ optional passphrase): nothing new |

## 1. applejack: packages and state directory (root)

```sh
apt install pacman-package-manager libarchive-tools zstd gnupg gpgv rsync curl python3
# pacman-package-manager provides makepkg, repo-add and vercmp on Ubuntu 24.04.
install -d -o ci -g ci -m 755 /srv/ci/pacman-repo
```

## 2. applejack: deploy key (as ci)

```sh
sudo -u ci ssh-keygen -t ed25519 -N '' -C slipstream-arch-deploy -f /home/ci/.ssh/slipstream_arch_deploy
cat /home/ci/.ssh/slipstream_arch_deploy.pub        # goes into step 3
```

## 3. starswirl: deploy user, docroot, rrsync (root)

```sh
adduser --system --group --home /home/archdeploy --shell /bin/sh archdeploy
install -d -o archdeploy -g archdeploy -m 755 /srv/arch.slipstreamviewer.org
# rrsync cannot create directories: create the two repo trees up front
install -d -o archdeploy -g archdeploy -m 755 \
  /srv/arch.slipstreamviewer.org/slipstream/os/x86_64 \
  /srv/arch.slipstreamviewer.org/slipstream-beta/os/x86_64
install -d -o archdeploy -g archdeploy -m 700 /home/archdeploy/.ssh
cat > /home/archdeploy/.ssh/authorized_keys <<'EOF'
command="/usr/bin/rrsync /srv/arch.slipstreamviewer.org",restrict ssh-ed25519 AAAA...PASTE... slipstream-arch-deploy
EOF
chown archdeploy:archdeploy /home/archdeploy/.ssh/authorized_keys; chmod 600 /home/archdeploy/.ssh/authorized_keys
```

Add `archdeploy` to sshd's `AllowUsers`/`AllowGroups` the way `sitedeploy`/`aptdeploy` are, then reload
sshd. `pacman_repo.sh deploy` uses `--delete` and `--delay-updates`, which rrsync allows by default.

## 4. starswirl: nginx server block (root)

Copy the access policy of the apt block (LAN only):

```nginx
server {
    listen 8080;
    server_name arch.slipstreamviewer.org;
    root /srv/arch.slipstreamviewer.org;
    allow 192.168.96.0/24;
    deny all;
    autoindex on;

    # Databases change with every release: never serve them stale.
    location ~ \.(db|files)(\.sig)?$ { add_header Cache-Control "no-cache" always; }
    # A package file never changes under the same name.
    location ~ \.pkg\.tar\.zst(\.sig)?$ { add_header Cache-Control "public, max-age=604800, immutable" always; }
    location ~ \.(asc|conf)$ {
        types { }
        default_type text/plain;
        add_header Cache-Control "public, max-age=3600" always;
    }
}
```

## 5. purplesmart and Cloudflare

Add `arch.slipstreamviewer.org` to the cloudflared tunnel ingress and to nginx like
`apt.slipstreamviewer.org` (copy that block, proxy to starswirl:8080), add the DNS record
(CNAME to the tunnel), and make sure Cloudflare does not cache `*.db`/`*.files` longer than the
origin says (a Cache Rule "respect origin" is enough). Mind the cloudsdale-scoped tunnel cert pitfall.

## 6. Seed and enable

1. Push a tag (or `ci-*` dry run): the `pacman-repo` job runs after `arch-package`, warns and exits
   while `/home/ci/.ssh/slipstream_arch_deploy` is missing or `LINUX_SIGNING_KEY` is unset.
2. To seed existing releases by hand on applejack (as ci):
   `scripts/ci/signing_key.sh run scripts/packaging/pacman/pacman_repo.sh seed v1.1.0 v1.2.0 && ... publish && ... deploy`
   (`seed` needs the tags oldest first only for readability; order does not matter).

## Test plan

1. `scripts/packaging/pacman/test_pacman_repo.sh` on any Arch box (local, throw-away key).
2. On a clean Arch container after the first real deploy:
   `curl .../slipstream.asc`, `pacman-key --add`, `--lsign-key`, add `[slipstream]`, `pacman -Sy`,
   `pacman -Si slipstream-bin`, `pacman -S slipstream-bin` (signature check must pass).
3. Tamper check: change one byte of a package on the server copy; `pacman -S` must refuse it.
4. Release a new beta: `pacman -Syu` in the beta-repo container offers it; the stable one does not.
