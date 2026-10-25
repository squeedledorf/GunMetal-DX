/**
 * @file slupdatechecker.h
 * @brief Update check (#293, phase 1): reads the channel's update feed on
 *        slipstreamviewer.org and offers a newer build in a notification.
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

#ifndef SL_SLUPDATECHECKER_H
#define SL_SLUPDATECHECKER_H

// The feed is a small static JSON file (UpdateCheckURLRelease / UpdateCheckURLBeta,
// format in the website repo's README, "Update feeds"), fetched with a plain GET: no
// cookies, IDs or user data. Release builds read the release feed (or the beta feed when
// UpdaterWillingToTest is on), Beta builds the beta feed (which also carries finals);
// Test/local builds never check on their own.
// Nothing is downloaded or run: the notice only opens the release notes or the download
// link in the browser. Signature verification of the feed is phase 2.
namespace SLUpdateChecker
{
    // Seconds after login before the first scheduled check, and between checks.
    constexpr double FIRST_CHECK_DELAY = 20.0;
    constexpr double CHECK_INTERVAL = 24.0 * 60.0 * 60.0;

    // Call every frame from LLAppViewer::idle(); cheap. Once logged in, runs the
    // scheduled check (UpdateCheckEnabled, not on Test builds) every CHECK_INTERVAL.
    void idle();

    // Help > Check for updates / Preferences "Check now": always answers, with the
    // update notice (even for a skipped version), "you're up to date" or "couldn't check".
    void checkNow();

    // First launch on a new version (UpdaterShowReleaseNotes): opens the update feed's
    // notes_url when the feed describes the running build, else the release notes page
    // listing every version. Failures are logged, never silent.
    void showReleaseNotes();

    // Release (not Beta / Test) build: only these choose between the release and beta
    // feeds (UpdaterWillingToTest); everything else always reads the beta feed.
    bool isReleaseBuild();

    // Feed key of the installer that matches how this copy was installed ("windows_setup"
    // for the NSIS installer, "linux_tarball" for the tarball), nullptr for everything the
    // auto-updater must leave alone (zip, Flatpak, deb, AUR).
    const char* installArtifactKey();
}

#endif // SL_SLUPDATECHECKER_H
