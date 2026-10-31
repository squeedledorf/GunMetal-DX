/**
 * @file slupdater.h
 * @brief Auto-updater (#421): download + verification of a signed update, applied when the
 *        person says so. <GMDX> Nothing is downloaded or installed without a click.
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

class LLFloater;
class LLSD;

// Flow (docs: scripts/updater/README.md). <GMDX> Every step that downloads or installs waits
// for the person:
//  1. SLUpdateChecker reads the signed feed (at the login screen); when it carries an "update"
//     object it hands the manifest + signature to offerUpdate().
//  2. The signature is verified with the embedded public key BEFORE anything in the
//     manifest is used. Only the verified manifest text decides URLs, hashes and versions.
//  3. The person is asked: Download and install / Remind me later / Skip this version.
//  4. Download and install: the installer (Windows setup / Linux tarball) is downloaded in
//     small ranged requests (resumable across restarts) with a progress window, hashed, and
//     checked against the signed sha256 and size (state file downloaded-update).
//  5. The person is asked again: Install now (restarts) / Install at next start. Either
//     writes approved-update; only that file is acted on at startup.
//  6. Linux: the launcher applies it on the next start (slupdateapply.cpp). Windows: the
//     next start verifies again and runs the installer silently (applyPendingAtStartup).
//  7. The updated viewer calls confirmStartup() when it reaches the login screen; a Linux
//     build that dies before that is rolled back by the launcher.
namespace SLUpdater
{
    // True when this build/install can auto-update at all: a real signing key is compiled
    // in and this is a Windows installer or Linux tarball install (not Flatpak/deb/AUR/zip).
    bool supported();

    // The feed key of the installer for this install ("linux_tarball" / "windows_setup"),
    // or nullptr when auto-install does not apply.
    const char* artifactKey();

    // The update check found a newer build. Returns true when this install can update itself
    // with the feed's signed manifest; the person has then been asked (or, for something
    // already asked about this run, is left alone unless `manual`). False: the caller shows
    // the plain "download it from the website" notice.
    // `feed_build` and `release_feed` come from the (unsigned) feed and are only used to
    // cross-check the signed manifest; `current_build` is the running build (or the
    // UpdateCheckFakeVersion test override) the manifest must be newer than. `label` and
    // `notes_url` (feed, https) are only shown. `modal`: a dialog (login screen, Help > Check
    // for updates) instead of a toast. `manual`: Help > Check for updates.
    bool offerUpdate(const std::string& manifest, const std::string& signature,
                     const std::string& feed_build, const std::string& current_build, bool release_feed,
                     const std::string& label, const std::string& notes_url, bool modal, bool manual);

    // At the login screen, once per run: an update downloaded earlier but not yet approved
    // (downloaded-update, or the pending-update 0.3.0 and older wrote on their own) is asked
    // about: Install now / Later / Discard. One that no longer applies is dropped quietly.
    void askAboutDownloaded(const std::string& current_build);

    // Called once the login screen is reached: the running build is healthy, so the
    // "unconfirmed" marker (Linux rollback guard) is removed.
    void confirmStartup();

    // Windows only, called first thing in WinMain: if an approved, verified installer is
    // staged, run it silently and return true (the caller must exit at once). False: carry on.
    bool applyPendingAtStartup();

    // LLFloaterReg build function of "gmdx_update_download" (download progress, Cancel).
    LLFloater* buildDownloadFloater(const LLSD& key);
}

#endif // SL_SLUPDATER_H
