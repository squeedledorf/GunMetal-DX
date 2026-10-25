/**
 * @file slupdater.h
 * @brief Auto-updater (#421): background download + verification of a signed update, applied
 *        at the next start. Off by default (setting UpdateAutoInstall).
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

#ifndef SL_SLUPDATER_H
#define SL_SLUPDATER_H

#include <string>

// Flow (docs: scripts/updater/README.md):
//  1. SLUpdateChecker reads the signed feed; when it carries an "update" object and
//     UpdateAutoInstall is on, it hands the manifest + signature to onFeedUpdate().
//  2. The signature is verified with the embedded public key BEFORE anything in the
//     manifest is used. Only the verified manifest text decides URLs, hashes and versions.
//  3. The installer (Windows setup / Linux tarball) is downloaded in the background in
//     small ranged requests (resumable across restarts), hashed, and checked against the
//     signed sha256 and size.
//  4. Linux: the launcher applies it on the next start (slupdateapply.cpp). Windows: the
//     next start verifies again and runs the installer silently (applyPendingAtStartup).
//  5. The updated viewer calls confirmStartup() when it reaches the login screen; a Linux
//     build that dies before that is rolled back by the launcher.
namespace SLUpdater
{
    // True when this build/install can auto-update at all: a real signing key is compiled
    // in and this is a Windows installer or Linux tarball install (not Flatpak/deb/AUR/zip).
    bool supported();

    // The feed key of the installer for this install ("linux_tarball" / "windows_setup"),
    // or nullptr when auto-install does not apply.
    const char* artifactKey();

    // Offered by the update check (UpdateAutoInstall on). Returns true when the update is
    // being handled (downloading, already staged or just staged), so the caller skips the
    // "download it yourself" notice; false means: use the normal notice.
    // `feed_build` and `release_feed` come from the (unsigned) feed and are only used to
    // cross-check the signed manifest; `current_build` is the running build (or the
    // UpdateCheckFakeVersion test override) the manifest must be newer than.
    bool onFeedUpdate(const std::string& manifest, const std::string& signature,
                      const std::string& feed_build, const std::string& current_build, bool release_feed);

    // Called once the login screen is reached: the running build is healthy, so the
    // "unconfirmed" marker (Linux rollback guard) is removed.
    void confirmStartup();

    // Windows only, called first thing in WinMain: if a verified installer is staged, run it
    // silently and return true (the caller must exit at once). False: carry on starting.
    bool applyPendingAtStartup();
}

#endif // SL_SLUPDATER_H
