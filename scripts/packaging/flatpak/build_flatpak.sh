#!/bin/bash
# Build the Slipstream Flatpak from an official Linux release tarball into an
# OSTree repo, sign it, refresh the repo metadata and write a single-file
# bundle (#286).
#
# usage: build_flatpak.sh TARBALL TAG REPO OUTDIR
#          TARBALL  Slipstream_<label>_x86_64.tar.xz
#          TAG      vX.Y.Z (-> branch "stable", also copied to "beta" so beta
#                   users get the final), vX.Y.Z-beta.N (-> branch "beta");
#                   anything else (ci-*, manual) -> branch "test", never
#                   published
#          REPO     the OSTree repo (archive mode) to commit into; created if
#                   missing. Keep it between runs (publish_flatpak_repo.sh
#                   pulls it from the host when the local copy is gone): the
#                   static deltas are computed against the previous commit.
#          OUTDIR   receives Slipstream_<label>.flatpak, slipstream.gpg and the
#                   .flatpakrepo / .flatpakref files (repo_files.sh)
#        build_flatpak.sh --branch TAG    print the branch for TAG
#
# env: SIGNING_KEY          armored OpenPGP secret key (the project release key,
#                           CI secret LINUX_SIGNING_KEY). Imported into a
#                           throwaway GNUPGHOME that is deleted on exit.
#      SIGNING_KEY_ID       key ID / fingerprint to sign with (CI secret
#                           LINUX_SIGNING_KEY_ID)
#      SIGNING_KEY_PASSPHRASE  optional (CI secret LINUX_SIGNING_KEY_PASSPHRASE);
#                           preset into the throwaway agent. Nothing ever
#                           prompts: the agent's pinentry is /bin/false.
#      UNSIGNED=1           allow building without SIGNING_KEY (local tests;
#                           the repo and bundle are then unsigned)
#      BRANCH               override the branch derived from TAG
#      STATE_DIR            flatpak-builder state/cache (default REPO/../builder-state)
#      PRUNE_DEPTH          commits kept per branch (default 3)
#      REPO_URL             public repo URL baked into the bundle and the
#                           .flatpakref files (default https://flatpak.slipstreamviewer.org/repo/)
#      SKIP_LIBCHECK=1      skip the ldd check inside the Platform
#      TAGS_REPO            git checkout whose v* tags feed the metainfo
#                           <releases> (default: this checkout)
#      GNUPG_TMP_PARENT     where the throwaway GNUPGHOME is made (default /tmp;
#                           keep it short, gpg-agent socket paths are limited)
#
# Needs: flatpak, flatpak-builder, and the flathub remote for the build user
# (`flatpak --user remote-add --if-not-exists flathub ...`): the runtime and SDK
# named in the manifest are installed from it on first use. See INFRA.md.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
APP_ID=org.slipstreamviewer.Slipstream
MANIFEST="$HERE/$APP_ID.yml"
REPO_URL=${REPO_URL:-https://flatpak.slipstreamviewer.org/repo/}
FLATHUB_REPO=https://dl.flathub.org/repo/flathub.flatpakrepo

die()  { echo "build_flatpak.sh: $*" >&2; exit 1; }
warn() { echo "build_flatpak.sh: WARNING: $*" >&2; }

branch_of() {
  if [[ "$1" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]]; then echo stable
  elif [[ "$1" =~ ^v[0-9]+\.[0-9]+\.[0-9]+-beta\.[0-9]+$ ]]; then echo beta
  else echo test
  fi
}

if [ "${1:-}" = --branch ]; then branch_of "${2:?usage: build_flatpak.sh --branch TAG}"; exit 0; fi

TARBALL=${1:?usage: build_flatpak.sh TARBALL TAG REPO OUTDIR}
TAG=${2:?usage: build_flatpak.sh TARBALL TAG REPO OUTDIR}
REPO=${3:?usage: build_flatpak.sh TARBALL TAG REPO OUTDIR}
OUT=${4:?usage: build_flatpak.sh TARBALL TAG REPO OUTDIR}
[ -f "$TARBALL" ] || die "no such tarball: $TARBALL"
BRANCH=${BRANCH:-$(branch_of "$TAG")}
LABEL=${TAG#v}
PRUNE_DEPTH=${PRUNE_DEPTH:-3}
command -v flatpak >/dev/null || die "flatpak not installed (apt install flatpak)"
command -v flatpak-builder >/dev/null || die "flatpak-builder not installed (apt install flatpak-builder)"

mkdir -p "$OUT" "$(dirname "$REPO")"
REPO=$(cd "$(dirname "$REPO")" && pwd)/$(basename "$REPO")
OUT=$(cd "$OUT" && pwd)
STATE_DIR=${STATE_DIR:-$(dirname "$REPO")/builder-state}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/slipstream-flatpak.XXXXXX")
GNUPGHOME_TMP=""
cleanup() {
  if [ -n "$GNUPGHOME_TMP" ]; then
    gpgconf --homedir "$GNUPGHOME_TMP" --kill all >/dev/null 2>&1 || true
    rm -rf "$GNUPGHOME_TMP"
  fi
  rm -rf "$WORK"
}
trap cleanup EXIT

# --- signing ------------------------------------------------------------------
SIGN=()
if [ -n "${SIGNING_KEY:-}" ]; then
  : "${SIGNING_KEY_ID:?SIGNING_KEY_ID (CI secret LINUX_SIGNING_KEY_ID) is empty}"
  command -v gpg >/dev/null || die "gpg not installed"
  # Short path: gpg-agent's socket path must fit in sun_path (108 bytes).
  GNUPGHOME_TMP=$(mktemp -d "${GNUPG_TMP_PARENT:-/tmp}/fpgpg.XXXXXX")
  chmod 700 "$GNUPGHOME_TMP"
  # Never prompt: any passphrase request the preset doesn't satisfy fails.
  printf 'allow-preset-passphrase\nallow-loopback-pinentry\npinentry-program /bin/false\n' \
    > "$GNUPGHOME_TMP/gpg-agent.conf"
  printf '%s\n' "$SIGNING_KEY" | gpg --homedir "$GNUPGHOME_TMP" --batch --pinentry-mode loopback \
    --passphrase "${SIGNING_KEY_PASSPHRASE:-}" --import 2>&1 | sed 's/^/  gpg: /'
  if [ -n "${SIGNING_KEY_PASSPHRASE:-}" ]; then
    PRESET=$(gpgconf --list-dirs libexecdir)/gpg-preset-passphrase
    [ -x "$PRESET" ] || die "gpg-preset-passphrase not found ($PRESET)"
    gpg-connect-agent --homedir "$GNUPGHOME_TMP" /bye >/dev/null 2>&1
    gpg --homedir "$GNUPGHOME_TMP" --batch --with-colons --with-keygrip --list-secret-keys \
      | awk -F: '$1=="grp" {print $10}' | while read -r grip; do
          printf '%s' "$SIGNING_KEY_PASSPHRASE" | "$PRESET" --homedir "$GNUPGHOME_TMP" --preset "$grip"
        done
  fi
  # Prove the key signs before spending time on the build.
  echo test | gpg --homedir "$GNUPGHOME_TMP" --batch --no-tty --local-user "$SIGNING_KEY_ID" \
    --detach-sign -o /dev/null || die "cannot sign with $SIGNING_KEY_ID (wrong ID, or the passphrase is missing/wrong)"
  gpg --homedir "$GNUPGHOME_TMP" --batch --export --export-options export-minimal "$SIGNING_KEY_ID" > "$OUT/slipstream.gpg"
  [ -s "$OUT/slipstream.gpg" ] || die "could not export the public key of $SIGNING_KEY_ID"
  SIGN=(--gpg-sign="$SIGNING_KEY_ID" --gpg-homedir="$GNUPGHOME_TMP")
  echo "build_flatpak.sh: signing with $SIGNING_KEY_ID"
elif [ "${UNSIGNED:-0}" = 1 ] || [ "$BRANCH" = test ]; then
  warn "no SIGNING_KEY: the repo and bundle are UNSIGNED (fine for a test build, never publish them)"
else
  die "SIGNING_KEY is empty (CI secret LINUX_SIGNING_KEY); set UNSIGNED=1 for a local test build"
fi

# --- stage the manifest -------------------------------------------------------
cp "$MANIFEST" "$HERE/slipstream-flatpak.sh" "$HERE/$APP_ID.desktop" "$WORK/"
ln "$TARBALL" "$WORK/slipstream.tar.xz" 2>/dev/null || cp "$TARBALL" "$WORK/slipstream.tar.xz"
# The metainfo's newest <release> is TAG; a test build (ci-*, manual) is listed
# as beta 0 of the version in the tarball's build_data.json.
META_TAG=$TAG
if [ "$BRANCH" = test ] && ! [[ "$TAG" =~ ^v[0-9]+\.[0-9]+\.[0-9]+(-beta\.[0-9]+)?$ ]]; then
  V=$(tar -xJOf "$TARBALL" --wildcards '*/build_data.json' | sed -nE 's/.*"Version"[[:space:]]*:[[:space:]]*"([0-9]+\.[0-9]+\.[0-9]+).*/\1/p')
  [ -n "$V" ] || die "no Version in the tarball's build_data.json"
  META_TAG="v$V-beta.0"
fi
"$HERE/gen_metainfo.sh" "$META_TAG" "$WORK/$APP_ID.metainfo.xml" "${TAGS_REPO:-$HERE/../../..}"
RUNTIME_VERSION=$(sed -nE "s/^runtime-version:[[:space:]]*'?([^' ]+)'?.*/\1/p" "$MANIFEST")
[ -n "$RUNTIME_VERSION" ] || die "no runtime-version in $MANIFEST"

# --- build + commit -------------------------------------------------------------
# --disable-rofiles-fuse: no FUSE in the CI container. --install-deps-from: the
# runtime/SDK come from flathub (user installation of the build user).
flatpak-builder "${FLATPAK_INSTALLATION_FLAG:---user}" --install-deps-from=flathub --disable-rofiles-fuse --force-clean \
  --state-dir="$STATE_DIR" --repo="$REPO" --default-branch="$BRANCH" \
  --subject="Slipstream $LABEL ($TAG)" "${SIGN[@]}" \
  "$WORK/build" "$WORK/$APP_ID.yml"

# --- every library resolves inside the Platform (not the SDK) ------------------
if [ "${SKIP_LIBCHECK:-0}" != 1 ]; then
  cat > "$WORK/libcheck.sh" <<'EOF'
#!/bin/sh
# Runs inside org.freedesktop.Platform with the built /app at $1.
app=$1; missing=0
export LD_LIBRARY_PATH="$app/slipstream/lib:$app/slipstream/bin/llplugin"
find "$app/slipstream" -type f > /tmp/elf-candidates
while IFS= read -r f; do
  [ "$(head -c 5 "$f" | od -An -tx1 | tr -d ' \n')" = 7f454c4602 ] || continue
  out=$(ldd "$f" 2>&1 | grep -E 'not found') || true
  if [ -n "$out" ]; then echo "MISSING in ${f#$app/}:"; echo "$out"; missing=1; fi
done < /tmp/elf-candidates
# glibc / libstdc++ of the runtime vs the tarball floor (2.38 / 3.4.32).
echo "runtime: $(ldd --version | head -1)"
exit $missing
EOF
  flatpak run "${FLATPAK_INSTALLATION_FLAG:---user}" --die-with-parent --filesystem="$WORK:ro" --command=sh \
      "org.freedesktop.Platform//$RUNTIME_VERSION" "$WORK/libcheck.sh" "$WORK/build/files" \
    || flatpak run --system --die-with-parent --filesystem="$WORK:ro" --command=sh \
      "org.freedesktop.Platform//$RUNTIME_VERSION" "$WORK/libcheck.sh" "$WORK/build/files" \
    || die "libraries missing in org.freedesktop.Platform//$RUNTIME_VERSION (see above): bundle them in the manifest"
fi

# --- final releases also go to beta -------------------------------------------
# Only when the final is newer than what beta already has: a hotfix on an older
# stable line (1.3.2 while beta is on 1.4.0-beta.N) must not replace the beta.
# Versions come from the "(vX.Y.Z[-beta.N])" tag in the beta head's subject.
semver_key() {   # vX.Y.Z[-beta.N] -> "X Y Z N" (a final gets N = 999999: above its betas)
  local t=${1#v}
  [[ "$t" =~ ^([0-9]+)\.([0-9]+)\.([0-9]+)(-beta\.([0-9]+))?$ ]] || return 1
  echo "${BASH_REMATCH[1]} ${BASH_REMATCH[2]} ${BASH_REMATCH[3]} ${BASH_REMATCH[5]:-999999}"
}
semver_gt() {    # semver_gt KEY_A KEY_B: true when A is newer than B
  local -a a=($1) b=($2); local i
  for i in 0 1 2 3; do
    ((10#${a[i]} > 10#${b[i]})) && return 0
    ((10#${a[i]} < 10#${b[i]})) && return 1
  done
  return 1
}
if [ "$BRANCH" = stable ]; then
  BETA_REF="app/$APP_ID/x86_64/beta"
  BETA_TAG=""
  if ostree --repo="$REPO" rev-parse "$BETA_REF" >/dev/null 2>&1; then
    BETA_TAG=$(ostree --repo="$REPO" log "$BETA_REF" 2>/dev/null \
      | grep -m1 -oE '\(v[0-9]+\.[0-9]+\.[0-9]+(-beta\.[0-9]+)?\)' | tr -d '()')
  fi
  NEW_KEY=$(semver_key "$TAG") || die "cannot parse $TAG as a release tag"
  OLD_KEY=$([ -n "$BETA_TAG" ] && semver_key "$BETA_TAG" || true)
  if [ -z "$OLD_KEY" ] || semver_gt "$NEW_KEY" "$OLD_KEY"; then
    flatpak build-commit-from "${SIGN[@]}" --src-ref="app/$APP_ID/x86_64/stable" \
      --subject="Slipstream $LABEL ($TAG)" "$REPO" "$BETA_REF"
  else
    echo "build_flatpak.sh: beta stays on $BETA_TAG ($TAG is not newer); not copying the final to beta"
  fi
fi

# The repo's default branch, and the branch slipstream.flatpakref installs:
# stable once a final release exists, beta until then.
STABLE_BRANCH=stable
REFS=$(ostree --repo="$REPO" refs)
grep -qx "app/$APP_ID/x86_64/stable" <<<"$REFS" || STABLE_BRANCH=beta

# --- repo metadata: summary, static deltas, pruning ----------------------------
if [ "$BRANCH" != test ]; then
  flatpak build-update-repo "${SIGN[@]}" --generate-static-deltas \
    --prune --prune-depth="$PRUNE_DEPTH" \
    --title="Slipstream Viewer" --default-branch="$STABLE_BRANCH" \
    --homepage=https://slipstreamviewer.org/ \
    --comment="Vulkan viewer for Second Life" \
    "$REPO"
else
  flatpak build-update-repo "$REPO"
fi
ostree --repo="$REPO" refs | sed 's/^/  ref: /'

# --- single-file bundle (offline install; updates then come from REPO_URL) -----
BUNDLE="$OUT/Slipstream_${LABEL}.flatpak"
BUNDLE_ARGS=(--runtime-repo="$FLATHUB_REPO")
if [ "$BRANCH" != test ]; then
  BUNDLE_ARGS+=(--repo-url="$REPO_URL")
  [ -s "$OUT/slipstream.gpg" ] && BUNDLE_ARGS+=(--gpg-keys="$OUT/slipstream.gpg")
fi
# (--gpg-sign is for OCI bundles only; the commit inside is the signed one.)
flatpak build-bundle "${BUNDLE_ARGS[@]}" "$REPO" "$BUNDLE" "$APP_ID" "$BRANCH"
ls -l "$BUNDLE"

# --- .flatpakrepo / .flatpakref + the icon they reference ----------------------
cp "$WORK/build/files/share/icons/hicolor/128x128/apps/$APP_ID.png" "$OUT/slipstream.png"
if [ -s "$OUT/slipstream.gpg" ]; then
  REPO_URL="$REPO_URL" STABLE_BRANCH="$STABLE_BRANCH" "$HERE/repo_files.sh" "$OUT/slipstream.gpg" "$OUT"
fi
echo "build_flatpak.sh: $APP_ID//$BRANCH ($TAG) committed to $REPO; bundle $BUNDLE"
