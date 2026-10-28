/**
 * @file gmdxspacealt.h
 * @brief GunMetal-DX space at altitude: the sky thins out as the camera climbs.
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

// <GMDX> SPACE AT ALTITUDE (GMDXSpaceAltitude, default on). Second Life builds go up to 4096 m and
// higher: skyboxes, space stations. Game scale, not physics: from GMDXSpaceStart (1500 m) to
// GMDXSpaceFull (4000 m) of camera height the air "thins" as if the camera rose into near space.
// The amount (0 below GMDXSpaceStart, smoothstep to GMDXSpaceStrength at GMDXSpaceFull, held
// above) drives:
//   - the physical sky (gmdxphysskyvk.cpp): its viewer height is lifted by up to kGmdxSpaceKm,
//     so the sky goes from blue to deep navy / black, haze and aerial perspective thin out and
//     the sun's light whitens; the planet's limb is drawn at the horizon (a thin bright band of
//     air, clean_atmos_common.glsl gmdxSpaceLimb). Cloud lighting keeps the real height.
//   - the classic WindLight sky: darkened toward the zenith, a band kept at the horizon.
//   - the sun disc: sharper and whiter; the 2D cloud dome (always overhead) fades out.
//   - the stars, Milky Way (gmdxnightsky.cpp) and LL's stars show by day.
//   - the night-eye exposure lift is held back by day (a black sky is not night).
// Frame info: cluster_rt.w (unused upstream) = the amount (clean_atmos_common.glsl gmdxSpace).
// 0 (off, or below GMDXSpaceStart): every hook is a no-op, the image is the old one.
// Header-only: physSkyFill computes it once per frame (gmdxSpaceUpdate), the rest read it.

#ifndef GMDX_SPACEALT_H
#define GMDX_SPACEALT_H

#include "llviewercontrol.h"
#include <algorithm>
#include <cmath>

// How far the physical sky's viewer is lifted at the full amount (km): ~45 km leaves the zenith
// near black and the limb a thin band. Game look, tune by eye.
constexpr float kGmdxSpaceKm = 45.f;

inline float& gmdxSpaceAmountRef()
{
    static float s = 0.f;
    return s;
}

// This frame's amount, 0..1 (gmdxSpaceUpdate's last value).
inline float gmdxSpaceAmount() { return gmdxSpaceAmountRef(); }

// The amount for a camera at cam_z metres (agent frame z = altitude); remembered for the frame.
inline float gmdxSpaceUpdate(float cam_z)
{
    static LLCachedControl<bool> s_on(gSavedSettings, "GMDXSpaceAltitude", true);
    static LLCachedControl<F32>  s_start(gSavedSettings, "GMDXSpaceStart", 1500.f);
    static LLCachedControl<F32>  s_full(gSavedSettings, "GMDXSpaceFull", 4000.f);
    static LLCachedControl<F32>  s_strength(gSavedSettings, "GMDXSpaceStrength", 1.f);
    float s = 0.f;
    if (s_on && std::isfinite(cam_z))
    {
        const float start = (F32)s_start;
        const float t = std::clamp((cam_z - start) / std::max((F32)s_full - start, 1.f), 0.f, 1.f);
        s = t * t * (3.f - 2.f * t) * std::clamp((float)(F32)s_strength, 0.f, 1.f);
    }
    return gmdxSpaceAmountRef() = s;
}

#endif // GMDX_SPACEALT_H
