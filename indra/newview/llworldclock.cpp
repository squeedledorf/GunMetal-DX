/**
 * @file llworldclock.cpp
 * @brief World pause / freeze: the world clock (see llworldclock.h).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Slipstream contributors.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "llworldclock.h"

#include "llappviewer.h"        // gFrameTime / gStartTime
#include "llcharacter.h"
#include "llviewercontrol.h"

bool LLWorldClock::sFrozen = false;
F64  LLWorldClock::sTime = 0.0;
F32  LLWorldClock::sDelta = 0.f;
bool LLWorldClock::sCaptureFreeze = false;

namespace
{
F64 sFrozenSecs = 0.0;      // total seconds spent frozen (subtracted from session time)
F64 sFreezeStart = 0.0;     // session time the current freeze began
// One pause request per character paused by the freeze (Black Dragon's toggle_freeze_world
// pattern). Handles are refcounts on the character's own request object, so a character
// deleted while frozen leaves nothing dangling; clearing them on thaw lets each motion
// controller resume (LLCharacter::updateMotions) from the held pose, no catch-up.
std::vector<LLAnimPauseRequest> sPauseHandles;
}

bool LLWorldClock::userFrozen()
{
    static LLCachedControl<bool> s_frozen(gSavedSettings, "SlipstreamWorldFrozen", false);
    return s_frozen;
}

void LLWorldClock::tick()
{
    static LLCachedControl<bool> s_freeze_time(gSavedSettings, "FreezeTime", false);
    const F64 now = F64Seconds(gFrameTime - gStartTime).value();
    const bool frozen = userFrozen() || s_freeze_time || sCaptureFreeze;

    if (frozen && !sFrozen)
    {
        sFreezeStart = now;
        LL_INFOS("WorldClock") << "world frozen" << LL_ENDL;
    }
    else if (!frozen && sFrozen)
    {
        sFrozenSecs += llmax(0.0, now - sFreezeStart);
        sPauseHandles.clear();
        LL_INFOS("WorldClock") << "world thawed" << LL_ENDL;
    }
    sFrozen = frozen;

    if (frozen)
    {
        // Every character, including ones that arrive while frozen: a fresh avatar or
        // animesh would otherwise animate inside a paused scene.
        for (LLCharacter* c : LLCharacter::sInstances)
        {
            if (c && !c->areAnimationsPaused())
                sPauseHandles.push_back(c->requestPause());
        }
    }

    static bool s_first = true;
    const F64 prev = sTime;
    sTime = (frozen ? sFreezeStart : now) - sFrozenSecs;
    sDelta = s_first ? 0.f : (F32)llclamp(sTime - prev, 0.0, 1.0);
    s_first = false;
}
