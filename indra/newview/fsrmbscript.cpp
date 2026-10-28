/**
 * @file fsrmbscript.cpp
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

#include "llviewerprecompiledheaders.h"

#include "fsrmbscript.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llfocusmgr.h"
#include "lltoolcomp.h"
#include "llviewercontrol.h"

// True between a right press reported to scripts and its release.
static bool sHeld = false;

static bool rmb_reportable()
{
    static LLCachedControl<bool> enabled(gSavedSettings, "FSRMBScriptEnabled", false);
    // <GMDX> Not while the mouselook pointer is freed for the UI: that right click is for a menu.
    return enabled && gAgentCamera.cameraMouselook() && gAgent.getRegion()
        && !LLToolCompGun::getInstance()->isMouseFreed();
}

static void release()
{
    sHeld = false;
    gAgent.clearControlFlags(AGENT_CONTROL_LBUTTON_DOWN);
    gAgent.setControlFlags(AGENT_CONTROL_LBUTTON_UP);
}

// static
void FSRMBScript::onRightMouse(bool down)
{
    if (down)
    {
        if (rmb_reportable())
        {
            sHeld = true;
            // Any change in the control flags goes out in this frame's AgentUpdate.
            gAgent.setControlFlags(AGENT_CONTROL_LBUTTON_DOWN);
        }
    }
    else if (sHeld)
    {
        // <GMDX> Released wherever the release lands (SoapStorm only released in mouselook,
        // so leaving mouselook while holding left the button held for scripts).
        release();
    }
}

// static
void FSRMBScript::afterControlReset()
{
    if (!sHeld)
    {
        return;
    }
    // resetControlFlags() strips all ephemeral bits; re-apply the held button before the
    // next AgentUpdate, or the sim sees it drop and fires a spurious button-up.
    // <GMDX> Let go if mouselook ended, the window lost focus (the release may never
    // arrive) or the setting was switched off while held.
    if (rmb_reportable() && gFocusMgr.getAppHasFocus())
    {
        gAgent.setControlFlags(AGENT_CONTROL_LBUTTON_DOWN);
    }
    else
    {
        release();
    }
}
