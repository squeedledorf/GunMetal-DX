/**
 * @file llworldclock.h
 * @brief World pause / freeze (Ctrl+Shift+F): the single "world time" that stops.
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

#ifndef LL_LLWORLDCLOCK_H
#define LL_LLWORLDCLOCK_H

#include "stdtypes.h"

// WORLD CLOCK. Session seconds (gFrameTimeSeconds) minus every second the world has
// spent frozen: it stops while frozen and resumes from the held value on thaw (no
// jump). Frozen = SlipstreamWorldFrozen (the Freeze World toggle, not persisted) OR
// LL's FreezeTime (snapshot freeze-frame / 360 capture), so those freeze the VK world
// too. Ticked once per frame by LLViewerObjectList::update, right after the frame
// timer, before any reader below runs.
//
// Readers (everything that animates the world; camera, UI, exposure, network stay on
// real time):
//  - getTime(): LLVKWorldFrame::time = the draw push cam_time.w -> water waves, GPU
//    texture animation (texAnimUV u_anim_time), GPU target omega (with the spin t0 in
//    LLCleanInstanceStore::composeOwnedPose), RT/probe anim time, the TAA previous time
//    (frozen: prev == current, zero motion); star field rotation / twinkle.
//  - getDelta() (0 while frozen): GPU particle sim dt, aurora / meteor ticks.
//  - isFrozen(): object interpolation + drawable moves (LLViewerObjectList::update,
//    LLPipeline::updateMove*), CPU particle sources (spawning), EEP day cycle and cloud
//    scroll (LLEnvironment::update), avatar animation (every LLCharacter holds a pause
//    request while frozen -> motion controller time stops, GPU pose is sticky).
class LLWorldClock
{
public:
    static void tick();                     // once per frame (see above)
    static bool isFrozen()  { return sFrozen; }
    static F64  getTime()   { return sTime; }
    static F32  getDelta()  { return sDelta; }
    // The user toggle alone (the on-screen indicator); isFrozen() also covers FreezeTime.
    static bool userFrozen();
    // #873: an upscaled live snapshot holds the world for its jittered frames (not a setting,
    // not shown). The caller ticks once after setting it (world time holds, delta 0) and
    // clears it when done; the next tick thaws without a jump.
    static void setCaptureFreeze(bool on) { sCaptureFreeze = on; }

private:
    static bool sFrozen;
    static bool sCaptureFreeze;
    static F64  sTime;
    static F32  sDelta;
};

#endif // LL_LLWORLDCLOCK_H
