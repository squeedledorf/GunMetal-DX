# Auto-updater (#421)

Windows (NSIS installer) and Linux tarball installs can update themselves, **only when the
person asks**: nothing is downloaded before they click "Download and install", and nothing is
installed before they click "Install now" or "Install at next start" (GunMetal-DX; the old
`UpdateAutoInstall` setting no longer does anything). The updater stays inert in any build that
does not have the update signing public key compiled in.

## How it works

1. Release CI (job `update-manifest`) builds a **manifest** (version, build, channel, and for
   `linux_tarball` / `windows_setup`: name, URL, sha256, size) and signs it with Ed25519
   (`sign_update_manifest.py`). The signature covers `"slipstream-update-v1\n" + manifest`.
2. The website feed builder (`scripts/build_update.py`, website repo) embeds
   `{"manifest": "<json string>", "signature": "<base64>", "key_id": "1"}` as `"update"` in
   `/update/release.json` and `/update/beta.json`.
3. The viewer's update check (`slupdatechecker.cpp`, at the login screen and once a day,
   if the person allowed checking) passes it to `SLUpdater` (`slupdater.cpp`). The signature is verified with the embedded key (`slupdateverify.h`)
   **before** anything in the manifest is used; URLs, hashes and versions come only from the
   verified text, URLs must be https on slipstreamviewer.org / git.slipstreamviewer.org, and
   the build must be newer than the running one (no downgrades).
4. The person is asked: *Download and install* / *Remind me later* (offered again at the next
   start) / *Skip this version* (`UpdateCheckSkippedBuild`). On the click the installer is
   downloaded with a progress window in 2 MiB ranged requests (Cancel keeps the partial file,
   the next click resumes it; one installer at a time is kept in `<cache>/updates`), hashed
   on a worker thread and compared with the signed sha256 and size. The state dir then holds
   `downloaded-update` (pointer to the staged record) and the person is asked *Install now*
   (quits and starts again) / *Install at next start*; either renames it to
   `approved-update`, the only file the startup step acts on. An update downloaded but not
   approved (or a `pending-update` left by GunMetal-DX 0.3.0, which downloaded on its own) is
   asked about at the next login screen: *Install now* / *Later* / *Discard*.
5. **Linux tarball**: the next start goes through the launcher (`slipstream`,
   `linux_tools/wrapper.sh` + `etc/update_hooks.sh`). If `~/.local/state/gunmetal-dx-update/approved-update`
   exists it runs `bin/do-not-directly-run-slipstream-bin --slipstream-apply-update <root>`
   (`slupdateapply.cpp`), which re-verifies signature and hash, unpacks beside the install,
   checks the unpacked tree (single top directory, launcher + binary, `build_data.json` version
   equals the manifest build) and swaps the two directories atomically (`renameat2`
   `RENAME_EXCHANGE`, with a two-rename fallback). Files of a running install are never edited
   in place; the old install stays as `<root>.prev`. Installs that are not writable, or that
   carry `.packaged-by` (deb/AUR/Flatpak), are never touched.
   **Rollback**: the apply step writes `update-unconfirmed`; the new viewer deletes it when it
   reaches the login screen. If the viewer exits non-zero before that, the launcher swaps
   `<root>.prev` back, records the build in `bad-build` (never installed again) and restarts.
6. **Windows**: the next start verifies again (`SLUpdater::applyPendingAtStartup`, called at
   the top of `wWinMain`), starts the installer silently (`/S`, it reuses the existing install
   directory) from a detached `cmd.exe` and exits; the cmd relaunches the viewer afterwards.
   One attempt per build: an unconfirmed previous attempt marks the build bad. There is **no
   automatic rollback on Windows** (the NSIS installer replaces files in place).
   *This path is written but has not been run: test it on a Windows machine before enabling.*

## Owner steps

```
scripts/updater/gen_update_key.sh ~/keys/slipstream-update.pem     # outside the repository
```
* Forgejo repo **variable** `UPDATE_PUBKEY_HEX` = the printed public key (64 hex).
* Forgejo repo **secret** `UPDATE_SIGNING_KEY` = the PEM file's full text; keep an offline copy
  and delete the working file.
* Configure with `-DUPDATE_PUBKEY_HEX=<hex>` (the release workflow passes it when the
  variable is set) so viewers can verify. Builds without it never auto-update.
* Until the first signed feed is published (`update-manifest` job ran and the website was
  rebuilt) the feed has no `"update"` object and viewers just show the usual notice.
* Key rotation: ship a viewer with the new public key first, then switch the secret.

## Tests

```
scripts/updater/test_update_verify.sh    # key gen -> sign -> viewer verifier, tamper / wrong key cases
BOOST_INCLUDE=<dir with boost/> scripts/updater/test_update_apply.sh
                                         # apply step on a fake install + launcher hooks (rollback)
```
Both use throw-away keys in a temp directory and need only `openssl`, `tar`, `xz`, `c++`.
