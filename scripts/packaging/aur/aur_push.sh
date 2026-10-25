#!/bin/bash
# Push a generated PKGBUILD + .SRCINFO to the slipstream-bin AUR repository (#248).
#
# usage: aur_push.sh DIR TAG     (DIR holds the PKGBUILD and .SRCINFO from gen.sh,
#                                 and keys/pgp/<fpr>.asc when the PKGBUILD is signed)
#
# env: AUR_SSH_KEY        private key of the AUR account (CI secret). Empty or
#                         unset: print a notice and exit 0, so the CI step stays
#                         green until the account exists.
#      AUR_PUBLISH_BETAS  "false" to push only vX.Y.Z releases (default: betas too,
#                         since every build so far is a beta)
#      AUR_GIT_NAME / AUR_GIT_EMAIL  commit identity in the AUR repo
#      AUR_REPO           default ssh://aur@aur.archlinux.org/slipstream-bin.git
#
# The first push creates the AUR package. The host key is pinned in
# aur_known_hosts (checked against the fingerprints on aur.archlinux.org).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
DIR=${1:?usage: aur_push.sh DIR TAG}
TAG=${2:?usage: aur_push.sh DIR TAG}
AUR_REPO=${AUR_REPO:-ssh://aur@aur.archlinux.org/slipstream-bin.git}

if [ -z "${AUR_SSH_KEY:-}" ]; then
  echo "AUR_SSH_KEY is not set: skipping the AUR push (see scripts/packaging/aur/README.md)"
  exit 0
fi
if [[ "$TAG" == *-beta.* ]] && [ "${AUR_PUBLISH_BETAS:-true}" = false ]; then
  echo "$TAG is a beta and AUR_PUBLISH_BETAS=false: skipping the AUR push"
  exit 0
fi
[ -f "$DIR/PKGBUILD" ] && [ -f "$DIR/.SRCINFO" ] || { echo "aur_push.sh: $DIR needs PKGBUILD and .SRCINFO" >&2; exit 1; }

W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
( umask 077; printf '%s\n' "$AUR_SSH_KEY" > "$W/key" )
export GIT_SSH_COMMAND="ssh -i $W/key -o IdentitiesOnly=yes -o StrictHostKeyChecking=yes -o UserKnownHostsFile=$HERE/aur_known_hosts"

# Cloning a package that doesn't exist yet gives an empty repository; the push
# below creates it.
git clone -q "$AUR_REPO" "$W/repo"
cp "$DIR/PKGBUILD" "$DIR/.SRCINFO" "$W/repo/"
# The release key next to a signed PKGBUILD (keys/pgp/<fingerprint>.asc, #288).
if [ -d "$DIR/keys/pgp" ]; then
  rm -rf "$W/repo/keys"
  cp -r "$DIR/keys" "$W/repo/"
fi
cd "$W/repo"
git add PKGBUILD .SRCINFO
[ ! -d keys ] || git add -A keys
if git diff --cached --quiet 2>/dev/null && git rev-parse -q --verify HEAD >/dev/null; then
  echo "AUR slipstream-bin is already at $TAG: nothing to push"
  exit 0
fi
PKGVER=$(sed -n 's/^\tpkgver = //p' .SRCINFO)
PKGREL=$(sed -n 's/^\tpkgrel = //p' .SRCINFO)
git -c user.name="${AUR_GIT_NAME:-Slipstream CI}" -c user.email="${AUR_GIT_EMAIL:-noreply@slipstreamviewer.org}" \
    commit -q -m "Update to $PKGVER-$PKGREL ($TAG)"
# The AUR only accepts the master branch.
git push -q origin HEAD:master
echo "Pushed slipstream-bin $PKGVER-$PKGREL ($TAG) to the AUR"
