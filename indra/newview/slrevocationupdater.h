/**
 * @file slrevocationupdater.h
 * @brief Keeps the viewer's certificate revocation data current: loads the static distrust
 *        list and the newest Chrome CRLSet at startup, refreshes the CRLSet from Google's
 *        component update service every few hours, and hands it to the embedded browser.
 *
 * The check itself is in slrevocation.cpp, applied by LLBasicCertificateStore to every
 * chain the viewer's HTTPS stack verifies.
 *
 * Files:
 *   app_settings/revocation/distrusted_spki.txt   static distrust list (shipped data)
 *   app_settings/revocation/crl-set               baseline CRLSet (refreshed at release)
 *   <cache>/revocation/crl-set                     last good fetched CRLSet
 * The newer (by sequence) of the two CRLSets is used. Fetching is on unless the setting
 * CRLSetUpdateEnabled is off; the shipped data still applies then.
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

#ifndef SL_SLREVOCATIONUPDATER_H
#define SL_SLREVOCATIONUPDATER_H

#include <string>

namespace SLRevocationUpdater
{
    // Startup, after the cache directory is set up and before the first HTTPS request:
    // loads the distrust list and the newest CRLSet on disk and logs what is in force.
    void init();

    // Main loop: starts a CRLSet fetch shortly after startup and then every few hours.
    void idle();

    // The newest valid CRLSet file on disk (cache or shipped), for the browser plugin;
    // empty when there is none.
    std::string crlsetPathForBrowser();
}

#endif // SL_SLREVOCATIONUPDATER_H
