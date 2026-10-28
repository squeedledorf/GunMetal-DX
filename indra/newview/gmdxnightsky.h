/**
 * @file gmdxnightsky.h
 * @brief GunMetal-DX night sky: the real stars and what else glows in a dark sky.
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

// <GMDX> NIGHT SKY (GMDXPhysicalNight): the stars of the Yale Bright Star Catalogue at their
// real places, brightness and colours in place of Second Life's 1000 random stars, and the Milky
// Way, airglow and zodiacal light of a dark sky. Drawn by the Vulkan sky pass
// (vkclean/shaders/gmdx_night_sky.glsl); this side loads the data and fills the pass's
// parameters. GMDXPhysicalNight off: nothing here runs and the sky is the old one.

#ifndef GMDX_NIGHTSKY_H
#define GMDX_NIGHTSKY_H

#include "lluuid.h"

class LLVector3;

namespace GMDXNightSky
{
// The night sky's textures for the sky pass's bindless slots: 0 = the star grid
// (gmdxnightskygrid.h), 1 = the Milky Way. Null while GMDXPhysicalNight is off or the data is
// missing. Made on the first call that wants them (main thread).
LLUUID textureId(int which);

// The star grid's id if it has been made (never makes it): its pixels are data, so the texture
// upload must leave them uncompressed.
const LLUUID& starGridId();

// The sky pass's parameters (frame info `bodies` / `sky_fx2`, clean_sky_bodies.glsl layout), on
// top of what LLAppViewer / the renderer put there, and `space`: the planets, satellites and
// star / deep-sky options in the top kSpaceSlots of the Lightbox meteors' block
// (gmdx_night_sky.glsl NS_SPACE). Leaves them alone (zeros: the old sky) while
// GMDXPhysicalNight is off.
constexpr int kSpaceSlots = 24;   // vec4s: ssr.meteors[192 - 24 ..]; the meteors keep 56 x 3
void fillFrame(float bodies[10][4], float sky_fx2[4], float space[kSpaceSlots][4]);

// The Lightbox meteors (llcleanrenderervk.cpp vkTickMeteors) under physical night: the spawn
// rate's multiplier (darkness x GMDXMeteors x a shower; < 0 = GMDXPhysicalNight off, the stock
// rate), and a shower meteor's path (travel turned away from the radiant; false, untouched, for a
// sporadic one). From the last fillFrame.
float meteorRateScale();
bool  meteorShowerPath(const LLVector3& start, LLVector3& travel);

}

#endif // GMDX_NIGHTSKY_H
