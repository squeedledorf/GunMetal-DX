/**
 * @file gmdxcombatlockout.h
 * @brief Debug views that see through walls stay off while the player aims, and for 5 s after.
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

// <GMDX> From Alchemy: Skooma (Nexii Malthus), LGPL 2.1: the combat render lockout (skcombatlockout.cpp,
// feature-combat/lockout 300484629c, after his SoapStorm 8f635810aa). Adjusted: Skooma switched the views
// off and restored them afterwards; here nothing the user set is ever changed. Each view asks active()
// where it draws, so a crash or quit while aiming leaves every setting as it was. A fair-play rule, not
// an option: there is no setting.
//
// Locked: beacons (all debug beacons: Beacons, area search, pathfinding, ...) and their labels, render
// metadata (physics shapes, bounding boxes, raycast) and object debug text, look-at and point-at,
// wireframe, highlight transparent, hidden selection outlines, and the render types that draw world
// geometry (a hidden one is drawn anyway; the menu keeps showing the user's own choice).

#pragma once

#include "stdtypes.h"

namespace GMDXCombatLockout
{
    // Once per frame, from LLAppViewer::idle after the shooter camera's update.
    void update();
    // Aiming (first-person mouselook, the shooter camera, an ADS dive) now or within the last 5 s.
    bool active();
    // True while active for a render type that holds world geometry: it draws even if hidden.
    bool keepsRenderType(U32 type);
}
