# Slipstream pacman repository (#419)

`https://arch.slipstreamviewer.org` serves two signed pacman repositories for `slipstream-bin`
(x86_64), so `pacman -Syu` keeps the viewer current on Arch, CachyOS, EndeavourOS, Manjaro...

| File | Purpose |
|---|---|
| `pacman_repo.sh` | Adds packages, prunes, signs packages + databases, verifies, deploys. Used by CI. |
| `slipstream.conf`, `slipstream-beta.conf` | `pacman.conf` sections served at the repo root. |
| `index.html` | Install instructions served at the repo root. |
| `test_pacman_repo.sh` | Local test with a throw-away key and fake packages (needs an Arch userland). |
| `INFRA.md` | One-time host setup (runner, web host, edge, DNS, secrets) and the test plan. |

The package is the one `scripts/packaging/aur` builds (`PKGBUILD`, `gen.sh`; the `arch-package`
job attaches the built `slipstream-bin-<ver>-1-x86_64.pkg.tar.zst` to the release). The
`pacman-repo` job in `.forgejo/workflows/release.yml` reuses that file (building it from the
release tarball with `gen.sh` + `makepkg` when none is attached) and does not change it.

## For users

```sh
curl -fsSLO https://arch.slipstreamviewer.org/slipstream.asc
gpg --show-keys --with-fingerprint slipstream.asc        # compare with the wiki, "Verifying downloads"
sudo pacman-key --add slipstream.asc
sudo pacman-key --lsign-key <fingerprint>
curl -fsS https://arch.slipstreamviewer.org/slipstream.conf | sudo tee -a /etc/pacman.conf   # or slipstream-beta.conf
sudo pacman -Syu slipstream-bin
```

`pacman.conf` section (what `slipstream.conf` contains):

```ini
[slipstream]
SigLevel = Required DatabaseRequired TrustedOnly
Server = https://arch.slipstreamviewer.org/$repo/os/$arch
```

## Layout

- **Repos:** `slipstream` (vX.Y.Z) and `slipstream-beta` (vX.Y.Z-beta.N **and** vX.Y.Z; a final goes
  into both because `1.0.0beta2 < 1.0.0 < 1.0.1beta1` in `vercmp`). Enable one, not both: both ship
  `slipstream-bin`. Files live at `/<repo>/os/x86_64/`.
- **Versions:** `X.Y.ZbetaN-1` / `X.Y.Z-1` (same `pkgver` scheme as the AUR package). The database
  lists the newest version; the newest `PACMAN_KEEP` (default 3) package files stay on the server so
  `pacman -U <url>` can downgrade.
- **Signing:** every package gets a detached `.sig`, and `repo-add --sign` signs `<repo>.db` and
  `<repo>.files`, all with the project key through `scripts/ci/signing_key.sh` (see
  `scripts/packaging/keys/README.md`). The key is in CI secrets only. `slipstream.asc` is the public
  key. Databases are published as real files (no symlinks) and deployed after the packages, so a client
  never sees a database that names a missing file.
- **Auto-updater:** the package carries `/opt/slipstream/.packaged-by`, so the in-viewer updater (#421)
  never touches it.
- **State:** `PACMAN_REPO_ROOT` on the runner (default `/srv/ci/pacman-repo`) is the source of truth;
  `pacman_repo.sh seed TAG...` rebuilds it from the Forgejo releases.
