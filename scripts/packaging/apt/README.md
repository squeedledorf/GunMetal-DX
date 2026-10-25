# Slipstream apt repository (#288)

`https://apt.slipstreamviewer.org` is a signed apt repository for
`slipstream-viewer`, the .deb that `scripts/packaging/deb` builds. Ubuntu
24.04+, Debian 13+ and Mint 22+ users add it once, and then `apt upgrade`
keeps the viewer current.

| File | Purpose |
|---|---|
| `apt_repo.sh` | Adds .debs, prunes, signs and publishes with aptly, verifies, deploys. Used by CI. |
| `slipstream.sources`, `slipstream-beta.sources` | deb822 source files served at the repo root (stable / beta). |
| `index.html` | Install instructions served at the repo root. |
| `INFRA.md` | One-time host setup (runner, web host, edge, DNS, secrets) and the test plan. |

## For users

```sh
sudo wget -qO /usr/share/keyrings/slipstream-archive-keyring.asc https://apt.slipstreamviewer.org/slipstream-archive-keyring.asc
sudo wget -qO /etc/apt/sources.list.d/slipstream.sources https://apt.slipstreamviewer.org/slipstream.sources
sudo apt update && sudo apt install slipstream-viewer slipstream-archive-keyring
```

For betas, download `slipstream-beta.sources` instead (it is saved under the
same name). Pick one suite only.

## Layout

- **Suites:** `stable` (vX.Y.Z) and `beta` (vX.Y.Z-beta.N **and** vX.Y.Z).
  A final release goes into both, because `1.0.0~beta6 < 1.0.0 < 1.0.1~beta1`:
  a beta user should get the final release rather than wait on the last beta
  until the next cycle's first beta.
- **Component** `main`, **architecture** `amd64`, plus the `arch: all` keyring
  package. Package versions follow `build_deb.sh` (`X.Y.Z~betaN`, `X.Y.Z`).
- **History:** the newest `APT_KEEP` (default 3) versions of each package per
  suite, so users can downgrade (`apt install slipstream-viewer=<version>`).
  Older ones are removed from the index, and `aptly db cleanup` deletes their
  pool files. aptly's `by-hash` copies are pruned to the current and previous
  index generation. The pool is therefore bounded at about 2 x `APT_KEEP`
  .debs (~200 MB each).
- **Signing:** `InRelease` and `Release.gpg`, made by the project key
  (`scripts/packaging/keys/README.md`) through `scripts/ci/signing_key.sh`.
  `Acquire-By-Hash: yes`, so a CDN-cached index can never mismatch a fresh
  `InRelease`.
- **Keyring package:** `slipstream-archive-keyring` installs the public key at
  `/usr/share/keyrings/slipstream-archive-keyring.asc`, the path that
  `slipstream.sources` names in `Signed-By`. When the key's expiry is
  extended or a subkey is rotated, users get the new key through `apt upgrade`
  before the old one stops working. A key file downloaded by hand never
  updates, which would break apt for everyone at the first expiry. The install
  snippet puts the same file at the same path for bootstrapping, and dpkg takes
  it over silently. Its version is `scripts/packaging/keys/keyring-version`.
  It is built only once the committed key is the real one.
  `slipstream-viewer` *Recommends* it (never Depends: a direct
  `apt install ./file.deb` must keep working without the repo).

## Why aptly

The task named reprepro or aptly.

- reprepro 5.3.1, the version in Ubuntu 24.04 (the runner), has no `Limit:`
  option. It keeps exactly one version per package per suite, which rules out
  the bounded multi-version history.
- aptly 1.5.0 (noble-updates) keeps several versions natively and signs with
  the gpg CLI, so the ephemeral `GNUPGHOME` just works with no gpgme passphrase
  plumbing. It also publishes `by-hash`.
- Retention is explicit: `apt_repo.sh` orders versions with
  `dpkg --compare-versions` and removes everything older than the Nth newest.

## Where the state lives

The state is aptly's database and pool in `/srv/ci/apt-repo` on the runner
(applejack). It persists between runs like the build trees, and it is the
source of truth. The web host only receives rsync copies of
`/srv/ci/apt-repo/aptly/public`.

Rebuilding from every released .deb on each run was the alternative. It was
rejected because it would download ~200 MB per kept version on every tag, and
a single unavailable old asset would fail the run. The persistent state has a
cheap recovery path: if `/srv/ci/apt-repo` is lost, `apt_repo.sh seed <tags>`
(or the `linux-signing` workflow) rebuilds it from the Forgejo releases. Each
.deb is checked against the sha256 in its release notes. Re-adding identical
files is a no-op, and aptly refuses a different file with the same
name/version.

## CI

- **`release.yml` job `apt-repo`** (after `deb-package`, `v*` tags):
  1. `apt_repo.sh seed $TAG` downloads the release's .deb over the LAN and
     checks its sha256 against the release notes.
  2. `signing_key.sh run apt_repo.sh publish` prunes, adds the keyring
     package, signs, publishes and verifies the signatures and index hashes.
  3. `apt_repo.sh deploy` rsyncs in three passes (new pool files, then the
     indexes with `--delay-updates`, then old pool files are deleted), so a
     client never sees an `InRelease` that names a missing file.

  The job is skipped with a warning while the signing secrets are unset. It is
  not `continue-on-error`.
- **Workflow `linux-signing`** (manual): seeding, rebuilding, re-publishing
  after a keyring bump, and re-signing a release's `SHA256SUMS`.

## Local checks

`apt_repo.sh` needs aptly, dpkg, gnupg/gpgv and rsync, i.e. an Ubuntu or
Debian machine or container. `INFRA.md` ("Test plan") has the container recipe
used to verify publishing and client installs on Ubuntu 24.04 (apt 2.8,
gpgv) and Debian 13 (apt 3.0, sqv).
