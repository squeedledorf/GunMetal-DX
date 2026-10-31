/**
 * @file slspacealt.h
 * @brief Space at altitude: the sky thins out as the camera climbs.
 *
 * Ported from GunMetal-DX (squeedledorf, LGPL 2.1), v0.3.0 cf5f1f4 (gmdxspacealt.h).
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

// SPACE AT ALTITUDE. Second Life builds go up to 4096 m and higher: skyboxes, space stations.
// Game scale, not physics: from kSpaceStartM (1500 m) to kSpaceFullM (4000 m) of camera height
// the air "thins" as if the camera rose into near space. The amount (0 below kSpaceStartM,
// smoothstep to 1 at kSpaceFullM, held above) drives:
//   - the physical sky (vkgfx/llcleanphysskyvk.cpp): its viewer height is lifted by up to
//     kSpaceKm, so the sky goes from blue to deep navy / black, haze and aerial perspective thin
//     out and the sun's light whitens; the planet's limb is drawn at the horizon (a thin bright
//     band of air, clean_atmos_common.glsl spaceLimb). Cloud lighting keeps the real height.
//   - the classic WindLight sky: darkened toward the zenith, a band kept at the horizon.
//   - the sun disc: sharper and whiter; the 2D cloud dome (always overhead) fades out.
//   - the stars, Milky Way (slnightsky.cpp) and LL's stars show by day.
//   - the night-eye exposure lift is held back by day (a black sky is not night).
// Frame info night[1].w = the amount (clean_atmos_common.glsl spaceAmount; GunMetal-DX used the
// spare cluster_rt.w). 0 (below kSpaceStartM): every hook is a no-op, the image is the old one.
// Header-only: physSkyFill computes it once per frame (slSpaceUpdate), the rest read it.
// (GunMetal-DX's GMDXSpaceAltitude / Start / Full / Strength settings are constants here.)

#ifndef SL_SPACEALT_H
#define SL_SPACEALT_H

#include <algorithm>
#include <cmath>

// How far the physical sky's viewer is lifted at the full amount (km): ~45 km leaves the zenith
// near black and the limb a thin band. Game look, tune by eye.
constexpr float kSpaceKm = 45.f;
constexpr float kSpaceStartM = 1500.f;   // camera height where the air starts to thin (m)
constexpr float kSpaceFullM = 4000.f;    // ... and where it is gone (m)

inline float& slSpaceAmountRef()
{
    static float s = 0.f;
    return s;
}

// This frame's amount, 0..1 (slSpaceUpdate's last value).
inline float slSpaceAmount() { return slSpaceAmountRef(); }

// The amount for a camera at cam_z metres (agent frame z = altitude); remembered for the frame.
inline float slSpaceUpdate(float cam_z)
{
    float s = 0.f;
    // <GMDX> Off in GunMetal-DX: Second Life's builds high up (skyboxes, platforms, sky arenas)
    // didn't suit a sky that thins with height (Orion, 2026-10-31). The sky stays as on the ground.
    constexpr bool kGmdxSpaceAltitude = false;
    if (kGmdxSpaceAltitude && std::isfinite(cam_z))
    // </GMDX>
    {
        const float t = std::clamp((cam_z - kSpaceStartM) / std::max(kSpaceFullM - kSpaceStartM, 1.f), 0.f, 1.f);
        s = t * t * (3.f - 2.f * t);
    }
    return slSpaceAmountRef() = s;
}

#endif // SL_SPACEALT_H
