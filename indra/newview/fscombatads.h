/**
 * @file fscombatads.h
 * @brief Aim down sights (ADS): double-tap-hold right mouse zoom, the OTS to
 *        first person dive, and the ADS screen-edge vignette.
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

#ifndef FS_COMBATADS_H
#define FS_COMBATADS_H

#include "stdtypes.h"

// SoapStorm's ADS (its LLToolCompGun in lltoolcomp.cpp), on top of this viewer's own
// right-click mouselook zoom: tap, then tap-and-hold the right button (FSDoubleTapADS,
// or every press with FSADSDefaultZoom) to zoom to FSADSZoomFOV; release to ease back.
// Entered from OTS, the camera dives into first person and returns on release.
// LLToolCompGun calls the hooks; the final pass (llcleanrenderervk.cpp) draws the vignette.
class FSCombatADS
{
public:
    // Right button pressed in mouselook / OTS (no Alt). base_fov: the un-zoomed FOV the
    // gun tool restores. True: this press aims down sights, the tool starts no zoom.
    static bool onRightMouseDown(F32 base_fov);
    // Every right button release. True: it ended ADS (the FOV eases back on its own).
    static bool onRightMouseUp();
    // True: the scroll adjusted the held ADS zoom (and FSADSZoomFOV).
    static bool onScrollWheel(S32 clicks);
    // Per frame, from LLToolCompGun::draw: the ADS FOV ease.
    static void update();
    // The gun tool was deselected (left mouselook): drop all ADS state. The tool
    // restores the base FOV itself.
    static void reset();

    static bool isHeld();      // aiming down sights now (FSADSMouseSensitivity applies)
    static bool ownsFOV();     // held, or easing in / out: the tool must not touch the FOV

    // Strength (0-1) of the ADS vignette this frame, eased on its own clock. Called once
    // per frame by the final pass.
    static F32 getVignetteAmount();
};

#endif // FS_COMBATADS_H
