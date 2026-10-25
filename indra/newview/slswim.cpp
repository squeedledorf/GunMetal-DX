/**
 * @file slswim.cpp
 * @brief Simulated swimming (#496).
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

#include "slswim.h"

#include "llagent.h"
#include "llnotificationsutil.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llvoavatarself.h"
#include "llworld.h"
#include "slaoengine.h"

namespace
{
    // Heights are of the agent position (about the hips) and of the ground
    // under it, relative to the water surface of the agent's region.
    constexpr F32 ENTER_SPOT_DEPTH = 1.3f;  // water this deep under us: swim
    constexpr F32 LEAVE_SPOT_DEPTH = 1.0f;  // shallower: walk out (hysteresis)
    constexpr F32 ENTER_HIP_DEPTH = 0.2f;   // hips this far under the surface
    constexpr F32 FLOAT_HIP_DEPTH = 0.6f;   // where floating holds the hips (shoulders at the surface)
    constexpr F32 DEAD_BAND = 0.12f;        // no correction within this
    constexpr F32 FLEW_OUT_HEIGHT = 1.5f;   // hips this far above the water: the user flew out

    F64 now()
    {
        return LLTimer::getElapsedSeconds();
    }
}

void SLSwimController::start(LLViewerRegion* region)
{
    if (!gAgent.canFly())
    {
        // No-fly parcel or region, or RLV @fly=n: no floating here
        if (region && region->getRegionID() != mNoFlyWarnedRegion)
        {
            mNoFlyWarnedRegion = region->getRegionID();
            LLNotificationsUtil::add("SLSwimNoFly");
        }
        return;
    }
    gAgent.setFlying(true, false);
    if (gAgent.getFlying())
    {
        mManaged = true;
        mStartedAt = now();
        if (SLAOEngine::instanceExists())
        {
            SLAOEngine::instance().noteOwnTransition(); // the AO switches to the water state now
        }
    }
}

void SLSwimController::stop(bool land)
{
    if (mManaged && land && gAgent.getFlying())
    {
        gAgent.setFlying(false);
        if (SLAOEngine::instanceExists())
        {
            SLAOEngine::instance().noteOwnTransition();
        }
    }
    mManaged = false;
}

void SLSwimController::update()
{
    static LLCachedControl<bool> sEnabled(gSavedSettings, "SLSimulatedSwimming", false);
    if (!sEnabled)
    {
        if (mManaged)
        {
            stop(false); // leave the user flying where they are
        }
        return;
    }
    if (!isAgentAvatarValid() || gAgent.getTeleportState() != LLAgent::TELEPORT_NONE)
    {
        return; // paused while teleporting / crossing
    }
    LLViewerRegion* region = gAgent.getRegion();
    if (!region)
    {
        return;
    }
    if (gAgentAvatarp->isSitting())
    {
        mManaged = false;   // sitting ends flight on its own
        return;
    }

    const LLVector3 pos = gAgent.getPositionAgent();
    const F32 water = region->getWaterHeight();
    const F32 ground = LLWorld::getInstance()->resolveLandHeightAgent(pos);
    const F32 spot_depth = water - ground;      // how deep the water is here
    const F32 hip_depth = water - pos.mV[VZ];   // how far under the surface we are
    const U32 keys = gAgent.getControlFlags();
    const bool user_up = (keys & (AGENT_CONTROL_UP_POS | AGENT_CONTROL_NUDGE_UP_POS)) != 0;
    const bool user_down = (keys & (AGENT_CONTROL_UP_NEG | AGENT_CONTROL_NUDGE_UP_NEG)) != 0;
    const bool flying = gAgent.getFlying();

    // Out of the water (or onto a beach): forget opt-outs; land what we flew
    if (spot_depth < LEAVE_SPOT_DEPTH || hip_depth < -FLEW_OUT_HEIGHT)
    {
        mUserOptedOut = false;
        if (mManaged)
        {
            // Wading out lands; flying up out of the water keeps flying
            stop(spot_depth < LEAVE_SPOT_DEPTH);
        }
        return;
    }

    if (!mManaged)
    {
        // Only take over walking avatars that reach deep water: never the
        // user's own flight, autopilot (click-to-walk, sit targets), or after
        // the user switched our flight off. On the sea floor (dived down and
        // landed) the avatar walks until it jumps.
        if (flying || mUserOptedOut || gAgent.getAutoPilot())
        {
            return;
        }
        const bool deep = spot_depth > ENTER_SPOT_DEPTH && hip_depth > ENTER_HIP_DEPTH;
        const bool near_surface = hip_depth < FLOAT_HIP_DEPTH + 0.5f;
        if (deep && (near_surface || user_up))
        {
            start(region);
        }
        return;
    }

    // Managed from here on
    if (!flying)
    {
        // Flight ended without us: the user turned it off, or diving landed
        // on the sea floor. Either way, walk until the user jumps or leaves.
        mManaged = false;
        mUserOptedOut = !user_down && hip_depth < FLOAT_HIP_DEPTH + 0.5f;
        return;
    }
    if (gAgent.getAutoPilot() || user_up || user_down)
    {
        return; // the user (or autopilot) steers vertically: up swims up, down dives
    }

    // Buoyancy: gently back to floating depth, with a dead band
    const F32 target = water - FLOAT_HIP_DEPTH;
    const F32 error = target - pos.mV[VZ];
    if (error > DEAD_BAND)
    {
        gAgent.setControlFlags(AGENT_CONTROL_NUDGE_UP_POS);
    }
    else if (error < -DEAD_BAND && hip_depth > -FLEW_OUT_HEIGHT)
    {
        gAgent.setControlFlags(AGENT_CONTROL_NUDGE_UP_NEG);
    }
}
