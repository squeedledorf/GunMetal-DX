/**
 * @file slnightsky.h
 * @brief Night sky: the real stars and what else glows in a dark sky.
 *
 * Ported from GunMetal-DX (squeedledorf, LGPL 2.1), v0.3.0 cf5f1f4 (gmdxnightsky.h).
 * Copyright (C) 2026, squeedledorf (GunMetal-DX); adapted for Slipstream.
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

// NIGHT SKY (RenderVKPhysicalNight): the stars of the Yale Bright Star Catalogue at their real
// places, brightness and colours in place of Second Life's 1000 random stars, and the Milky Way,
// airglow and zodiacal light of a dark sky, planets and satellites. Drawn by the Vulkan sky pass
// (vkclean/shaders/clean_night_sky.glsl); this side loads the data and fills the pass's
// parameters (frame info night_sky / night_space). Physical night off: nothing here runs and the
// sky is the old one (LL's random stars). One star system: the catalogue replaces LL's stars, it
// is never drawn on top of them.

#ifndef SL_NIGHTSKY_H
#define SL_NIGHTSKY_H

#include "lluuid.h"

class LLVector3;

namespace SLNightSky
{
// The night sky's textures for the sky pass's bindless slots: 0 = the star grid
// (slnightskygrid.h), 1 = the Milky Way. Null while physical night is off or the data is
// missing. Made on the first call that wants them (main thread).
LLUUID textureId(int which);

// The star grid's id if it has been made (never makes it): its pixels are data, so the texture
// upload must leave them uncompressed.
const LLUUID& starGridId();

// The sky pass's parameters (clean_frame_info.glsl night_sky / night_space; clean_night_sky.glsl
// has the layout). Leaves them alone (zeros: the old sky) while physical night is off. The
// bindless indices (night_sky[2].xy) are the renderer's.
constexpr int kSpaceSlots = 24;
void fillFrame(float night_sky[3][4], float space[kSpaceSlots][4]);

// The Lightbox meteors (llcleanrenderervk.cpp vkTickMeteors) under physical night: the spawn
// rate's multiplier (darkness x a shower; < 0 = physical night off, the stock rate), and a shower
// meteor's path (travel turned away from the radiant; false, untouched, for a sporadic one).
// From the last fillFrame.
float meteorRateScale();
bool  meteorShowerPath(const LLVector3& start, LLVector3& travel);
}

#endif // SL_NIGHTSKY_H
