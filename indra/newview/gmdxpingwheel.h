/**
 * @file gmdxpingwheel.h
 * @brief GunMetal-DX: the ping wheel (a held key picks Ping / Enemy / Rally) and its keys in Preferences.
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

// <GMDX> The ping wheel (keyboard action map_ping_wheel, default Shift+B). The spot is taken when the
// key goes down (under the pointer, or the crosshair in mouselook / OTS: GMDXMapShare::pingTarget).
// Let go before GMDXPingWheelHoldMs: a plain Ping there. Held longer: a wheel of Ping / Enemy / Rally
// opens, in the middle of the screen in mouselook (the mouse is freed for it, as Quick Swap does, and
// taken back after) or at the pointer; moving the mouse toward a slice picks it, and letting go sends
// that kind (in the middle: nothing). The key counts as let go when its own key-up comes, or when it
// stops arriving as held (its modifier let go first). Works for a mouse button bound to it too.

#pragma once

#include "llkeyboard.h"

class GMDXPingWheel
{
public:
    static void onKey(const LLKeyPressState& state);   // map_ping_wheel (llviewerinput.cpp)
    static void draw();                                  // over the world (LLHUDView::draw)
};
