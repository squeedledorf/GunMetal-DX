/**
 * @file gmdxcrashreport.h
 * @brief GunMetal-DX crash reports: Crashpad writes a minidump on a crash; the next start asks
 * whether to send it to BugSplat.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
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

// <GMDX> CRASH REPORTS (BugSplat database GMDX, via Crashpad; scripts/crashpad/build.sh).
//  - start(): after the viewer's own error handling is set up (Crashpad then dumps first and hands
//    the signal / exception back), the crashpad_handler beside the viewer binary is started with
//    the report database in <user dir>/crashpad, the viewer's log as the attachment, and uploads
//    OFF: a report waits until the person says it may go.
//  - offerPending(): once the login screen is up, reports from earlier runs that were never sent:
//    "GunMetal-DX crashed..." Send / Always send / Don't send (GMDXCrashReportSend: -1 ask, 1
//    always). A report that goes has its log cut to the last part first (a session log can run
//    to hundreds of MB); Don't send deletes it.
// Built without the Crashpad package (GMDX_CRASHPAD undefined) both do nothing.

#pragma once

namespace GMDXCrashReport
{
    void start();
    void offerPending();
}
