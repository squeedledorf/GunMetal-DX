#!/bin/bash
# Generate the Ed25519 key pair that signs auto-update manifests (#421).
#
# usage: gen_update_key.sh PRIVATE_KEY_PATH
#
# Writes the PRIVATE key (PEM, mode 0600) to PRIVATE_KEY_PATH and prints the PUBLIC key as
# 64 hex characters. Refuses to overwrite a file, and refuses a path inside a repository
# working tree: the private key must never be committed.
#
# What to do with the results:
#   1. Public key hex  -> the Forgejo repository VARIABLE  UPDATE_PUBKEY_HEX  (CI passes it
#      to the viewer build as -DSLIPSTREAM_UPDATE_PUBKEY_HEX), or paste it into
#      indra/newview/slupdateverify.h. Keep a copy in a safe place.
#   2. Private key PEM -> the Forgejo repository SECRET  UPDATE_SIGNING_KEY  (the whole PEM
#      text, including the BEGIN/END lines). Then store the file offline (password manager /
#      encrypted backup) and delete the working copy.
# Rotating the key means shipping a viewer with the new public key first; viewers that only
# know the old key cannot verify manifests signed with the new one.
set -euo pipefail
out=${1:?usage: gen_update_key.sh PRIVATE_KEY_PATH}
[ ! -e "$out" ] || { echo "refusing to overwrite $out" >&2; exit 1; }
dir=$(cd "$(dirname "$out")" && pwd)
if (cd "$dir" && command -v git >/dev/null 2>&1 && git rev-parse --is-inside-work-tree >/dev/null 2>&1); then
  echo "refusing: $dir is inside a repository working tree; pick a path outside it" >&2
  exit 1
fi
umask 077
openssl genpkey -algorithm ED25519 -out "$out"
chmod 600 "$out"
pub=$(openssl pkey -in "$out" -pubout -outform DER | tail -c 32 | od -An -v -tx1 | tr -d ' \n')
[ ${#pub} -eq 64 ] || { echo "could not extract the public key" >&2; exit 1; }
echo "private key written to $out (keep it secret, never commit it)"
echo "public key (hex): $pub"
