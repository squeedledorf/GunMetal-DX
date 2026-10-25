/**
 * @file slswim.h
 * @brief Simulated swimming (#496): float at the water surface, dive with
 *        crouch, rise back with buoyancy, walk out onto land.
 *
 * SL has no water physics and a viewer cannot set buoyancy like a script.
 * This drives the agent's own movement instead: on reaching deep enough
 * water it turns flying on, then every frame (just before the agent update
 * goes out) adds slow vertical "nudge" controls that hold the avatar at
 * the surface, let it sink while crouch/down is held, and bring it back up
 * when released. Leaving the water (ground near the surface) turns the
 * flying it started off again. The AO's swim states pick the animations.
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

#ifndef SL_SWIM_H
#define SL_SWIM_H

#include "llsingleton.h"
#include "lluuid.h"

class LLViewerRegion;

class SLSwimController : public LLSingleton<SLSwimController>
{
    LLSINGLETON_EMPTY_CTOR(SLSwimController);
    LOG_CLASS(SLSwimController);

public:
    // Once per frame, before the agent update is sent. Reads this frame's
    // movement keys and may add vertical controls or switch flying.
    void update();

    // True while we hold the avatar in the water (flying we turned on).
    bool isSwimming() const { return mManaged; }

private:
    void start(LLViewerRegion* region);
    void stop(bool land);

    bool mManaged = false;          // flying was turned on by us
    bool mUserOptedOut = false;     // the user switched flying off in the water: wait until dry
    LLUUID mNoFlyWarnedRegion;      // told once per region that it can't fly
    F64 mStartedAt = 0.0;
};

#endif // SL_SWIM_H
