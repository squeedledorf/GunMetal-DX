/**
 * @file fsrmbscript.h
 * @brief Right mouse button script events in mouselook.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
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

#ifndef FS_RMBSCRIPT_H
#define FS_RMBSCRIPT_H

// SoapStorm (FSRMBScriptEnabled): while in mouselook (OTS included), the right
// mouse button is reported to scripts as the normal, non-mouselook left button
// (AGENT_CONTROL_LBUTTON), so a weapon that took CONTROL_LBUTTON sees a second
// trigger next to the mouselook left button (CONTROL_ML_LBUTTON). The click
// still reaches the mouselook tool afterwards, so right-click zoom and ADS work
// as before; this only adds the control flag.
class FSRMBScript
{
public:
    // LLViewerWindow::handleRightMouseDown / Up, before the click goes to the tools.
    static void onRightMouse(bool down);

    // LLAppViewer::idle, right after gAgent.resetControlFlags(): keeps a held button
    // held in the next AgentUpdate, or lets go of it when mouselook ended meanwhile.
    static void afterControlReset();
};

#endif // FS_RMBSCRIPT_H
