/**
 * @file gmdxskysettings.h
 * @brief GunMetal-DX's sky, night, star, cloud and space settings (Graphics > Sky).
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

#ifndef GMDX_SKYSETTINGS_H
#define GMDX_SKYSETTINGS_H

// <GMDX> Slipstream's port of the GunMetal-DX sky (slnightlight.h, slnightsky.cpp, slspacealt.h,
// vkgfx/slnighteyevk.h, the volumetric clouds) turned GunMetal-DX's look settings into constants.
// The ones a player would change for taste come back as settings on Graphics > Sky, under their
// original GMDX* names and defaults, so saved values apply as before the merge; the deep tuning
// knobs stay constants (at Orion's tuned values where he had them). Header-only.

#include "llviewercontrol.h"

#define GMDX_SKY_F32(fn, name, def, lo, hi) \
    inline F32 fn() { static LLCachedControl<F32> s(gSavedSettings, name, def); return llclamp((F32)s, (F32)(lo), (F32)(hi)); }
#define GMDX_SKY_BOOL(fn, name, def) \
    inline bool fn() { static LLCachedControl<bool> s(gSavedSettings, name, def); return s; }

// Night (llappviewer.cpp, vkgfx/slnighteyevk.h)
GMDX_SKY_F32(gmdxMoonlightScale,    "GMDXMoonlightScale",   0.05f, 0.f, 1.f)   // kMoonlightScale
GMDX_SKY_BOOL(gmdxRespectRegionSky, "GMDXRespectRegionSky", true)              // (always on in Slipstream)
inline bool gmdxNightPhotoLook() { static LLCachedControl<S32> s(gSavedSettings, "GMDXNightLook", 1); return s == 1; }   // kNightPhotoLook

// Night sky (slnightsky.cpp)
GMDX_SKY_F32(gmdxStarBrightness,    "GMDXStarBrightness",   1.f, 0.f, 10.f)    // kStarBrightness
GMDX_SKY_F32(gmdxMilkyWay,          "GMDXMilkyWay",         1.f, 0.f, 10.f)    // kMilkyWay
GMDX_SKY_BOOL(gmdxMeteorShower,     "GMDXMeteorShower",     false)             // kMeteorShower
GMDX_SKY_BOOL(gmdxFantasySky,       "GMDXFantasySky",       false)             // (not in Slipstream)


// Volumetric clouds: GMDXCloudBodies and GMDXCloudWindSpeed are read by GunMetal-DX's cloud model
// itself (vkgfx/gmdxvolcloudsvk.cpp), which keeps no dependency on this header.

#undef GMDX_SKY_F32
#undef GMDX_SKY_BOOL

#endif // GMDX_SKYSETTINGS_H
