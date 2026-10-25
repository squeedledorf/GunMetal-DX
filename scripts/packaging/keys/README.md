# Slipstream release signing key (#288)

One OpenPGP key signs everything Linux users download from us:

| What | Signature | Made by |
|---|---|---|
| Every release asset (tarball, .deb, PKGBUILD, Windows files, ...) | `SHA256SUMS.asc` over `SHA256SUMS` | `release.yml` job `sign-release`, `scripts/ci/sign_release.sh` |
| Arch source tarball | `Slipstream_<ver>_x86_64.tar.xz.sig`, checked by makepkg via `validpgpkeys` | job `arch-package` |
| apt repository | `InRelease` / `Release.gpg` of each suite | job `apt-repo`, `scripts/packaging/apt/apt_repo.sh` |
| Flatpak repository (#286) | ostree commits + summary | the Flatpak job (#286) |

**Fingerprint:** `C28F DB4A 40AA 8360 D8DF  0470 AD8E 6152 6C06 E929` (also on
<https://slipstreamviewer.org/wiki/verify/>).

## Layout

- **Primary key**: ed25519, capability *certify* only, 5-year expiry. It
  never leaves offline storage. It is needed only to add, extend or revoke
  subkeys.
- **Signing subkey**: ed25519, capability *sign*, 2-year expiry. This is the
  only secret CI has.
- **Revocation certificate** for the primary key. gpg writes it when it creates
  the key. Store it offline, separately from the key backup.

Expiry protects against a key that is lost or stolen without anyone noticing.
Extending the expiry is a routine step (see below), so a short expiry costs
little.

## Files and CI secrets (the convention every job uses)

| Name | Where | Content |
|---|---|---|
| `scripts/packaging/keys/slipstream-release.asc` | this repo | Armored public key. Scripts treat it as a placeholder while it has no `-----BEGIN PGP PUBLIC KEY BLOCK-----` line. |
| `scripts/packaging/keys/keyring-version` | this repo | Version of the `slipstream-archive-keyring` apt package (`YYYY.MM.DD`). **Bump it whenever the .asc changes.** |
| `/slipstream-release.asc` | website (`site/` in slipstream-website) | The same file, at <https://slipstreamviewer.org/slipstream-release.asc>. |
| `LINUX_SIGNING_KEY` | Forgejo secret | `gpg --armor --export-secret-subkeys` output: the signing subkey's secret. The primary key is only a stub in it (`sec#`). |
| `LINUX_SIGNING_KEY_ID` | Forgejo secret | Fingerprint of the **primary** key, 40 hex digits, no spaces. This one identifier is used everywhere: `gpg -u`, aptly `-gpg-key`, makepkg `validpgpkeys`, `flatpak build-update-repo --gpg-sign`. gpg chooses the signing subkey itself. |
| `LINUX_SIGNING_KEY_PASSPHRASE` | Forgejo secret, optional | Leave it unset (recommended): a passphrase stored in the same secret store as the key protects nothing. If you set it, the helper presets it into the ephemeral gpg-agent, so gpg, gpgme (ostree/flatpak) and aptly can sign without prompting. |

A job never imports the key itself. It runs its signing command through the
helper:

```sh
scripts/ci/signing_key.sh check || exit 0     # skip (with a notice) until the secrets exist
scripts/ci/signing_key.sh run <command...>    # e.g. gpg -u "$SIGNING_KEY_FPR" --detach-sign FILE
```

`run` does the following:

- Creates a fresh `GNUPGHOME` under `/tmp` (mode 700), configured with
  `batch`, `pinentry-mode loopback`, and an agent whose pinentry is
  `/bin/false`, so nothing can ever prompt.
- Imports the secret and checks that it contains the key named by
  `LINUX_SIGNING_KEY_ID`, with a usable signing subkey.
- Makes a test signature.
- Checks that `slipstream-release.asc` is the same key, once it is no longer
  the placeholder.
- Runs the command with `GNUPGHOME`, `SIGNING_KEY_FPR`, `SIGNING_PUBKEY` (the
  exported public key) and `SIGNING_PUBKEY_REPO` set.
- Kills the agent and deletes the directory, even when the command fails.

The key is never written to the persistent CI trees.

## Creating the key (maintainer workstation, once)

Work in a RAM-only GnuPG home, so the primary secret never touches a disk you
don't control. An offline or live-USB machine is better still. Every command
below uses that home. Your normal `~/.gnupg` is never involved.

```sh
export GNUPGHOME=$(mktemp -d /dev/shm/slipstream-key.XXXXXX); chmod 700 "$GNUPGHOME"

# Primary: certify only, 5 years. gpg asks for a passphrase: use a strong one,
# since this passphrase protects the offline backup.
gpg --quick-gen-key 'Slipstream Release Signing Key <support@slipstreamviewer.org>' ed25519 cert 5y
FPR=$(gpg --list-keys --with-colons | awk -F: '$1=="fpr"{print $10; exit}')
echo "$FPR"

# Signing subkey: sign only, 2 years.
gpg --quick-add-key "$FPR" ed25519 sign 2y
gpg --list-secret-keys --keyid-format long "$FPR"
#   sec   ed25519/... [C] [expires: +5y]
#   ssb   ed25519/... [S] [expires: +2y]
```

Replace the e-mail address with one that reaches the maintainers, because
keys.openpgp.org sends its verification mail there (see "Publish").

### Back up the primary key and the revocation certificate

```sh
gpg --armor --export-secret-keys "$FPR" > slipstream-primary-secret.asc   # passphrase-protected
cp "$GNUPGHOME/openpgp-revocs.d/$FPR.rev" slipstream-revocation.rev
```

- Copy `slipstream-primary-secret.asc` to two encrypted USB sticks (or a
  password manager's file vault) that are kept in different places.
  Optionally also keep a paper copy (`paperkey`).
- Store `slipstream-revocation.rev` separately from the key backup, for
  example printed. Anyone who has it can revoke the key, but cannot sign with
  it.
- Then delete both local files.

### Export the CI subkey (no passphrase)

Remove the passphrase in a second, throwaway home. The primary key's
passphrase stays on the backups.

```sh
SUB=$(mktemp -d /dev/shm/slipstream-sub.XXXXXX); chmod 700 "$SUB"
gpg --armor --export-secret-subkeys "$FPR" | gpg --homedir "$SUB" --import
gpg --homedir "$SUB" --passwd "$FPR"     # enter the old passphrase, then leave the new one empty and confirm
gpg --homedir "$SUB" --armor --export-secret-subkeys "$FPR" > ci-signing-subkey.asc
gpg --homedir "$SUB" --list-secret-keys  # must show "sec#" (stub) and "ssb" (the subkey)
```

If the key ever has more than one signing subkey, export only the current one
by giving its fingerprint with a `!` suffix: `--export-secret-subkeys <SUBFPR>!`.

### Publish the public key

```sh
gpg --armor --export-options export-minimal --export "$FPR" > slipstream-release.asc
gpg --show-keys slipstream-release.asc
```

1. In this repo, replace `scripts/packaging/keys/slipstream-release.asc` with
   that file and set `keyring-version` to today's date. Commit.
2. In the website repo, replace `site/slipstream-release.asc` with the same
   file and put the fingerprint (`gpg --fingerprint` format, in groups of 4)
   in `wiki/docs/verify.md`. Deploy.
3. Upload the key to keyservers. AUR helpers (`yay`, `paru`) and `makepkg`
   users fetch it from there by fingerprint.
   ```sh
   gpg --keyserver hkps://keys.openpgp.org --send-keys "$FPR"
   gpg --keyserver hkps://keyserver.ubuntu.com --send-keys "$FPR"
   ```
   keys.openpgp.org publishes the user ID only after you click the link in its
   verification e-mail. Without the user ID, `gpg --recv-keys` there fails with
   "no user ID", so do click it.
4. Set the secrets in Forgejo: sky_line/Slipstream > Settings > Actions >
   Secrets. Use `LINUX_SIGNING_KEY` = the content of `ci-signing-subkey.asc`
   and `LINUX_SIGNING_KEY_ID` = `$FPR`. The API works too:
   ```sh
   python3 -c 'import json,sys; print(json.dumps({"data": open(sys.argv[1]).read()}))' ci-signing-subkey.asc \
     | curl -fsS -X PUT -H "Authorization: token $(cat ~/.config/forgejo-token)" -H 'Content-Type: application/json' \
         -d @- http://192.168.96.20:3000/api/v1/repos/sky_line/Slipstream/actions/secrets/LINUX_SIGNING_KEY
   ```
   (Forgejo upgrades have dropped secrets before. Check them after every
   major upgrade.)
5. Clean up: `gpgconf --homedir "$SUB" --kill all; gpgconf --kill all; rm -rf "$SUB" "$GNUPGHOME" ci-signing-subkey.asc; unset GNUPGHOME`.

## Routine: extend the subkey before it expires

Start at least **6 months before** the subkey expires. Users' apt keyrings and
AUR keyrings need the extended key before the old expiry date: apt and makepkg
reject signatures from an expired subkey.

```sh
export GNUPGHOME=$(mktemp -d /dev/shm/slipstream-key.XXXXXX); chmod 700 "$GNUPGHOME"
gpg --import slipstream-primary-secret.asc                 # from the offline backup
SUBFPR=$(gpg --list-keys --with-colons "$FPR" | awk -F: '$1=="sub"{s=1;next} $1=="fpr"&&s{print $10; s=0}' | tail -1)
gpg --quick-set-expire "$FPR" 2y "$SUBFPR"                 # subkey: 2 more years
gpg --quick-set-expire "$FPR" 5y                           # primary too, if it is due
```

Then repeat "Publish the public key", steps 1-4:

- Commit the new `.asc` and bump `keyring-version`. The next tag's `apt-repo`
  job ships it as `slipstream-archive-keyring`, which apt users then get with
  `apt upgrade`.
- Update the website copy.
- Upload to the keyservers again.
- Re-export the CI subkey (the expiry lives in the exported binding signature)
  and update the `LINUX_SIGNING_KEY` secret.

Once the keyring package is released, apt users need nothing else.
Tarball-only users fetch the key again from the website.

## Rotating to a new subkey

Use this when the key is not compromised but is due for replacement.

1. Add the new subkey with the offline primary: `gpg --quick-add-key "$FPR" ed25519 sign 2y`.
2. Publish the public key (repo + `keyring-version` + website + keyservers).
   Keep CI signing with the old subkey for at least one release, so that the
   keyring package carrying the new subkey reaches apt users while the old
   signature still verifies.
3. Then export only the new subkey for CI (`--export-secret-subkeys <NEWSUBFPR>!`)
   and update the secret. Let the old subkey expire. Revoke it only if it was
   exposed.

## Compromise

- **CI subkey leaked** (the primary is safe): with the offline primary, revoke
  the subkey (`gpg --edit-key "$FPR"`, `key N`, `revkey`, `save`), add a new
  one, publish and update the secret. apt users must fetch the new key once by
  hand (the old subkey can no longer be trusted to deliver it). Announce the
  change on the website and in the release notes.
- **Primary key lost or leaked**: `gpg --import slipstream-revocation.rev`,
  then publish the revoked key (repo, website, keyservers). Create a new key
  from scratch, and treat it as a new trust root everywhere: users must replace
  the key by hand.
