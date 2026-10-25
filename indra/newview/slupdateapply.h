/**
 * @file slupdateapply.h
 * @brief Auto-updater (#421): signed manifest parsing and the Linux tarball apply step.
 *
 * Free of viewer headers (std, Boost.JSON, OpenSSL via slupdateverify) so it also runs as a
 * tiny command-line mode of the viewer binary and is tested standalone
 * (scripts/updater/test_update_apply.sh).
 *
 * Linux flow: the viewer downloads + verifies the tarball in the background and records it
 * (SLUpdater, slupdater.cpp). The NEXT start goes through the launcher (etc/update_hooks.sh),
 * which runs `<bin> --slipstream-apply-update <install root>` BEFORE the viewer starts. That
 * mode re-verifies signature + hash, unpacks the tarball into a sibling directory of the
 * install, checks the unpacked tree and swaps the two directories atomically
 * (renameat2 RENAME_EXCHANGE; rename fallback). Nothing inside the running install is ever
 * edited: the old install stays whole as <root>.prev, which the launcher swaps back if the
 * new version dies before reaching the login screen.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

#ifndef SL_SLUPDATEAPPLY_H
#define SL_SLUPDATEAPPLY_H

#include "slupdateverify.h"

#include <map>
#include <string>

namespace SLUpdateApply
{
    struct Artifact
    {
        std::string        name;
        std::string        url;
        std::string        sha256;
        unsigned long long size = 0;
    };

    struct Manifest
    {
        std::string                      version;    // 1.2.0
        std::string                      build;      // 1.2.0.3
        std::string                      channel;    // release / beta / jetstream
        std::map<std::string, Artifact>  artifacts;  // linux_tarball, windows_setup
    };

    // Jetstream builds (channel "Slipstream Jetstream": built every 6 hours from main,
    // SLIPSTREAM_JETSTREAM=1 from CMake) only ever take jetstream manifests; every other build
    // never takes one. Keeps an install from being "updated" into the other channel.
    bool isJetstreamBuild();
    bool channelAllowed(const std::string& manifest_channel);

    // Hosts a manifest may point downloads at (https only).
    bool urlAllowed(const std::string& url);

    // "1.2.0.3" vs "1.2.0.4": -1, 0, 1; 2 when either is not a four-part number.
    int compareBuild(const std::string& a, const std::string& b);

    // Verifies the Ed25519 signature FIRST, then parses and validates the manifest text.
    // Everything the updater uses comes from this verified text, never from unsigned feed
    // fields. False (with `error`) on a bad signature, unknown schema or any odd field.
    bool parseManifest(const std::string& manifest, const std::string& signature_b64, Manifest& out,
                       std::string& error, const char* pubkey_hex = nullptr);

    // $XDG_STATE_HOME/slipstream-update or ~/.local/state/slipstream-update (Jetstream builds:
    // slipstream-update-jetstream, so the two channels never apply each other's staged
    // updates). The launcher (etc/update_hooks.sh) computes the same path. Empty if neither is set.
    std::string stateDir();

    // Exit codes of the apply step (also the launcher's contract).
    enum ApplyCode { APPLIED = 0, NOTHING = 10, FAILED = 11 };

    // The apply step described above. `staged_json` is the file written by the viewer
    // ({manifest, signature, artifact, file}); `root` the install directory (the launcher's
    // directory). `state_dir` holds pending-update / update-unconfirmed / bad-build.
    int applyStaged(const std::string& staged_json, const std::string& root, const std::string& state_dir,
                    std::string& message, const char* pubkey_hex = nullptr);

    // Entry for `--slipstream-apply-update ROOT`. Returns an ApplyCode. Linux only.
    int cliMain(int argc, char** argv);
}

#endif // SL_SLUPDATEAPPLY_H
