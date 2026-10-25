/**
 * @file slspeedfov.cpp
 * @brief Speed-based field of view (#663)
 *
 * The speed is the camera ANCHOR's: the agent's global position differenced over real time
 * (your avatar, which is carried by the vehicle you sit on, so a boat or an aircraft counts
 * as is), never the camera's own movement -- orbiting, Alt-cam and manual camera moves
 * cannot raise it. Frames whose implied speed is far above the smoothed one (a teleport,
 * a region-crossing seam, a script move) are dropped, and a teleport in progress resets
 * the state.
 *
 *   speed (m/s) --low-pass (0.2 s)--> s --smoothstep(start, full)--> t
 *   target = t * max extra FoV;  boost chases target with an attack (quick) and a release
 *   (slower) time constant, both as exp(-dt / tau) so frame rate does not matter.
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

#include "llviewerprecompiledheaders.h"

#include "slspeedfov.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llcontrol.h"
#include "llfloaterreg.h"
#include "llstartup.h"
#include "lltimer.h"
#include "lltoolmgr.h"
#include "llviewercontrol.h"
#include "llvoavatarself.h"
#include "sltpscamera.h"

namespace
{
    constexpr F32 kSpeedTau      = 0.20f;   // s, low-pass on the measured speed (animation bob, packet jitter)
    constexpr F32 kAttackTau     = 0.30f;   // s, widening
    constexpr F32 kReleaseTau    = 1.00f;   // s, returning
    constexpr F32 kMaxDt         = 0.25f;   // s, longer gaps (stalls, loading) restart the sampling

    LLTimer    sTimer;
    LLVector3d sPrevPos;
    bool       sHavePrev = false;
    F32        sSpeed    = 0.f;   // smoothed anchor speed, m/s
    F32        sBoost    = 0.f;   // radians

    F32 smoothstep01(F32 e0, F32 e1, F32 x)
    {
        const F32 t = llclamp((x - e0) / llmax(e1 - e0, 0.001f), 0.f, 1.f);
        return t * t * (3.f - 2.f * t);
    }

    // The boost must be exactly zero where the camera is being driven for a precise shot or
    // by a tool: build / edit mode, Pose Studio, Photo Studio, the TPS camera's aim mode,
    // appearance editing.
    bool hardSuppressed()
    {
        if (!isAgentAvatarValid()) return true;
        if (LLToolMgr::getInstance()->inBuildMode()) return true;
        if (gAgentCamera.cameraCustomizeAvatar()) return true;
        if (SLTPSCamera::isEngagedNow()) return true;
        if (LLFloaterReg::instanceVisible("sl_pose_studio") || LLFloaterReg::instanceVisible("fs_poser")
            || LLFloaterReg::instanceVisible("photo_studio"))
        {
            return true;
        }
        return false;
    }
}

void SLSpeedFov::reset()
{
    sHavePrev = false;
    sSpeed = 0.f;
    sBoost = 0.f;
    sTimer.reset();
}

F32 SLSpeedFov::update()
{
    static LLCachedControl<bool> s_on(gSavedSettings, "SlipstreamSpeedFov", false);
    static LLCachedControl<bool> s_ml(gSavedSettings, "SlipstreamSpeedFovMouselook", false);
    static LLCachedControl<F32> s_max_deg(gSavedSettings, "SlipstreamSpeedFovMax", 10.f);
    static LLCachedControl<F32> s_start(gSavedSettings, "SlipstreamSpeedFovStartSpeed", 8.f);
    static LLCachedControl<F32> s_full(gSavedSettings, "SlipstreamSpeedFovFullSpeed", 45.f);

    const F32 dt_real = sTimer.getElapsedTimeAndResetF32();

    const bool mouselook = gAgentCamera.cameraMouselook();
    if (!s_on || (mouselook && !s_ml) || LLStartUp::getStartupState() < STATE_STARTED || hardSuppressed())
    {
        sHavePrev = false;
        sSpeed = 0.f;
        sBoost = 0.f;
        return 0.f;
    }

    // Anchor speed.
    const LLVector3d pos = gAgent.getPositionGlobal();
    const bool teleporting = gAgent.getTeleportState() != LLAgent::TELEPORT_NONE;
    if (teleporting || !sHavePrev || dt_real <= 0.f || dt_real > kMaxDt)
    {
        if (teleporting) { sSpeed = 0.f; sBoost = 0.f; }
        sPrevPos = pos;
        sHavePrev = !teleporting;
        return sBoost;
    }
    const F32 dt = dt_real;
    const F32 inst = (F32)(pos - sPrevPos).magVec() / dt;
    sPrevPos = pos;
    // A jump (teleport inside the region, region-crossing seam, scripted move) is far above
    // anything the speed could have reached smoothly: drop the sample, keep the last speed.
    if (inst <= sSpeed * 3.f + 80.f)
    {
        sSpeed += (inst - sSpeed) * (1.f - expf(-dt / kSpeedTau));
    }

    // Free camera (Alt-cam / orbit / manual): no boost, eased out like any other release.
    const bool free_cam = !mouselook && !gAgentCamera.getFocusOnAvatar();

    F32 target = 0.f;
    if (!free_cam)
    {
        const F32 start = llmax((F32)s_start, 0.f);
        const F32 full  = llmax((F32)s_full, start + 1.f);
        target = smoothstep01(start, full, sSpeed) * llclamp((F32)s_max_deg, 0.f, 30.f) * DEG_TO_RAD;
    }
    const F32 tau = (target > sBoost) ? kAttackTau : kReleaseTau;
    sBoost += (target - sBoost) * (1.f - expf(-dt / tau));
    if (sBoost < 1e-5f && target <= 0.f) sBoost = 0.f;
    return sBoost;
}
