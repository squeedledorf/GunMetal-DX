#!/bin/bash
# Run a command with the Slipstream Linux release signing key (#288) available
# in an ephemeral GnuPG home, then destroy that home.
#
# usage: signing_key.sh run CMD [ARGS...]
#          Import the key into a fresh GNUPGHOME under /tmp, run CMD with the
#          environment below, then kill the gpg-agent and delete the directory
#          (also on failure or signal). CMD's exit status is returned.
#        signing_key.sh check
#          Exit 0 when LINUX_SIGNING_KEY and LINUX_SIGNING_KEY_ID are set, else
#          print a notice and exit 1. Jobs use it to skip signing until the key
#          exists.
#
# Secrets (Forgejo repository Actions secrets, shared by every Linux signing
# job: tarball/SHA256SUMS, Arch, apt, Flatpak; see scripts/packaging/keys/README.md):
#   LINUX_SIGNING_KEY             ASCII-armored `gpg --export-secret-subkeys`
#                                 output: the signing SUBKEY's secret, the
#                                 primary key is only a stub (sec#).
#   LINUX_SIGNING_KEY_ID          fingerprint of the PRIMARY key (40 hex, no
#                                 spaces). This is the one identifier used
#                                 everywhere: gpg -u, aptly -gpg-key, makepkg
#                                 validpgpkeys, flatpak --gpg-sign. gpg picks
#                                 the signing subkey by itself.
#   LINUX_SIGNING_KEY_PASSPHRASE  optional. Recommended: none (a passphrase
#                                 stored next to the key in the same secret
#                                 store adds nothing). When set, it is preset
#                                 into the ephemeral gpg-agent, so gpg, gpgme
#                                 users (ostree/flatpak) and aptly all sign
#                                 without a prompt.
#
# Environment CMD gets:
#   GNUPGHOME            the ephemeral home (keyring with the key, nothing else)
#   SIGNING_KEY_FPR      = LINUX_SIGNING_KEY_ID, upper case
#   SIGNING_PUBKEY       armored public key exported from that keyring
#   SIGNING_PUBKEY_REPO  scripts/packaging/keys/slipstream-release.asc when it
#                        holds the real key (it must then contain the same
#                        fingerprint, or this script refuses); empty while it is
#                        still the placeholder
# Sign with e.g. `gpg --yes -u "$SIGNING_KEY_FPR" --armor --detach-sign FILE`
# (gpg.conf in the home already sets batch mode and SHA512).
#
# The key never touches the persistent CI trees. The home lives under /tmp
# (short path: gpg-agent's socket path is length-limited) and is removed by an
# EXIT trap after `gpgconf --kill all`.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO_PUBKEY="$HERE/../packaging/keys/slipstream-release.asc"

die() { echo "signing_key.sh: $*" >&2; exit 1; }

is_placeholder() { ! grep -q -- '-----BEGIN PGP PUBLIC KEY BLOCK-----' "$1" 2>/dev/null; }

check() {
  if [ -z "${LINUX_SIGNING_KEY:-}" ] || [ -z "${LINUX_SIGNING_KEY_ID:-}" ]; then
    echo "LINUX_SIGNING_KEY / LINUX_SIGNING_KEY_ID are not set: skipping signing (see scripts/packaging/keys/README.md)"
    return 1
  fi
}

case "${1:-}" in
  check) check; exit ;;
  run) shift ;;
  *) die "usage: signing_key.sh run CMD [ARGS...] | signing_key.sh check" ;;
esac
[ $# -gt 0 ] || die "run needs a command"
check >&2 || die "no signing key configured"
command -v gpg >/dev/null 2>&1 || die "gpg not installed (apt install gnupg)"

FPR=$(tr -d ' \t\r\n' <<<"$LINUX_SIGNING_KEY_ID" | tr 'a-f' 'A-F')
FPR=${FPR#0X}
[[ "$FPR" =~ ^[0-9A-F]{40}$ ]] || die "LINUX_SIGNING_KEY_ID must be the 40-hex primary key fingerprint"

GH=$(mktemp -d /tmp/slipstream-gpg.XXXXXX)
cleanup() {
  gpgconf --homedir "$GH" --kill all >/dev/null 2>&1 || true
  rm -rf "$GH"
}
trap cleanup EXIT
trap 'exit 130' INT TERM
chmod 700 "$GH"
export GNUPGHOME="$GH"
# Never prompt: no display for a GUI pinentry, loopback + batch so gpg fails
# instead of asking, and an agent whose pinentry is /bin/false. A passphrase
# only ever comes from LINUX_SIGNING_KEY_PASSPHRASE (preset into the agent).
unset DISPLAY WAYLAND_DISPLAY GPG_TTY PINENTRY_USER_DATA
cat > "$GH/gpg.conf" <<'EOF'
no-greeting
batch
no-tty
pinentry-mode loopback
keyid-format 0xlong
with-fingerprint
personal-digest-preferences SHA512
cert-digest-algo SHA512
EOF
cat > "$GH/gpg-agent.conf" <<'EOF'
allow-preset-passphrase
allow-loopback-pinentry
pinentry-program /bin/false
default-cache-ttl 86400
max-cache-ttl 86400
EOF

printf '%s\n' "$LINUX_SIGNING_KEY" | gpg --quiet --import 2>/dev/null \
  || die "could not import LINUX_SIGNING_KEY (is it an armored secret key export?)"

# The imported key must be the one the ID names, with a secret signing subkey.
gpg --list-secret-keys --with-colons "$FPR" >/dev/null 2>&1 \
  || die "LINUX_SIGNING_KEY does not contain the key $FPR (LINUX_SIGNING_KEY_ID)"
COLONS=$(gpg --list-secret-keys --with-colons --with-keygrip "$FPR")
# ssb records: field 12 = capabilities, field 15 = serial/"#" for stubs; grp follows.
GRIPS=$(awk -F: '
  $1=="ssb" { cand = ($12 ~ /s/ && $15 != "#" && $2 !~ /[eridn]/) ; next }
  $1=="grp" && cand { print $10; cand = 0 }' <<<"$COLONS")
[ -n "$GRIPS" ] || die "key $FPR has no usable secret signing subkey (expired or revoked? export with --export-secret-subkeys)"
if awk -F: '$1=="sec" && $15 != "#" { found=1 } END { exit !found }' <<<"$COLONS"; then
  echo "signing_key.sh: WARNING: LINUX_SIGNING_KEY contains the PRIMARY secret key; the CI secret should hold only the signing subkey (gpg --export-secret-subkeys)" >&2
fi

if [ -n "${LINUX_SIGNING_KEY_PASSPHRASE:-}" ]; then
  PRESET="$(gpgconf --list-dirs libexecdir)/gpg-preset-passphrase"
  [ -x "$PRESET" ] || die "gpg-preset-passphrase not found (apt install gpg-agent)"
  gpg-connect-agent /bye >/dev/null 2>&1
  for g in $GRIPS; do
    printf '%s' "$LINUX_SIGNING_KEY_PASSPHRASE" | "$PRESET" --preset "$g"
  done
fi

# Prove that signing works (this also catches a wrong passphrase) and that the
# signature verifies against the exported public key.
export SIGNING_KEY_FPR="$FPR"
export SIGNING_PUBKEY="$GH/signing-pubkey.asc"
gpg --armor --export "$FPR" > "$SIGNING_PUBKEY"
echo probe > "$GH/probe"
gpg --quiet -u "$FPR" --detach-sign -o "$GH/probe.sig" "$GH/probe" \
  || die "test signature with $FPR failed (wrong LINUX_SIGNING_KEY_PASSPHRASE?)"
gpg --quiet --verify "$GH/probe.sig" "$GH/probe" 2>/dev/null || die "test signature does not verify"
rm -f "$GH/probe" "$GH/probe.sig"

# The committed public key (website, keyring package, AUR) must be the same key.
export SIGNING_PUBKEY_REPO=""
if [ -f "$REPO_PUBKEY" ] && ! is_placeholder "$REPO_PUBKEY"; then
  REPO_FPRS=$(gpg --show-keys --with-colons "$REPO_PUBKEY" 2>/dev/null | awk -F: '$1=="pub"{p=1;next} $1=="fpr"&&p{print $10;p=0}')
  grep -qxF "$FPR" <<<"$REPO_FPRS" \
    || die "scripts/packaging/keys/slipstream-release.asc is not the key $FPR (it holds: ${REPO_FPRS:-nothing})"
  SIGNING_PUBKEY_REPO=$(cd "$(dirname "$REPO_PUBKEY")" && pwd)/$(basename "$REPO_PUBKEY")
else
  echo "signing_key.sh: NOTE: scripts/packaging/keys/slipstream-release.asc is still the placeholder; commit the real public key (keys/README.md)" >&2
fi

echo "signing_key.sh: signing as $FPR (ephemeral GNUPGHOME)" >&2
set +e
"$@"
rc=$?
set -e
exit $rc
