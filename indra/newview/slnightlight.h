/**
 * @file slnightlight.h
 * @brief Physical night: the moon's light from its real brightness and phase.
 *
 * Ported from GunMetal-DX (squeedledorf, LGPL 2.1), v0.3.0 cf5f1f4 (gmdxnightlight.h).
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

#ifndef SL_NIGHTLIGHT_H
#define SL_NIGHTLIGHT_H

// PHYSICAL NIGHT (RenderVKPhysicalNight, default on; off = the old Second Life night).
//
// Second Life lights the night with the moon at the sun's full strength: EEP has one light
// colour (LLSettingsSky::getMoonlightColor returns the sunlight colour) and moon_brightness only
// dims the moon's disc and halo. Here the moon's light is the sun's colour times
//   kMoonlightScale (a full moon high in the sky: this fraction of the sun's light)
//   x 2 x moon_brightness (EEP's default 0.5 = 1)
//   x the phase: Krisciunas & Schaefer 1991, I(a) / I(0) = 10^(-0.4 (0.026 |a| + 4e-9 a^4)),
//     a = the phase angle in degrees (0 full, 180 new) = 180 - the sun-moon angle (EEP moves
//     the two independently, so the phase comes from their directions).
// The renderer keeps its units (the sun ~1); the scale is linear light, applied before the
// WindLight light-path extinction like the sun's own colour.
//
// DUSK. Second Life swaps its one scene light from the sun to the moon the moment the sun's
// centre crosses the horizon: the light jumps from the horizon to the moon's height and the
// shadows with it. Here the scene light is the sum of both:
//   - each light fades by the WindLight extinction at its OWN height (the sun's direct light
//     is gone at the horizon, as before);
//   - the moon's light fades in over a twilight band of the sun's height (kDuskSunHigh, +4
//     degrees, to kDuskSunLow, -10): above it the moon adds nothing and the day is exactly as
//     before; below it the moon is at its full strength;
//   - the light's direction turns from the sun to the moon by the sun's height, from +3 to -3
//     degrees, along the sky at an even pace (over the top when they stand opposite). A moon
//     below the horizon leaves the light on the sun and takes it over as it rises.
// Its colour is given before the extinction AT THAT DIRECTION, so everything that applies
// WindLight's extinction to atmos[2] (fillAtmosK: surfaces, fog; god rays) gets exactly the sum.
//
// THE WINDLIGHT SKY'S OWN LIGHT. The dome (the Classic sky, its #819 table, the reflection probes'
// sky) and the classic 2D clouds apply their own extinction per view direction, so the ground's
// back-derived colour would starve them. They get a light of their own instead, one body at a
// time: the sun, its EEP colour in full while it is up and fading to nothing at half kDuskSunLow
// (-5 degrees), then the moon at its real strength, growing from there to full at kDuskSunLow.
// The switch falls where both are zero. Frame info night[1].xyz its clamped direction, night[0].y
// its strength sky_q (>= 0 the sun: atmos[8] x sky_q; < 0 the moon: the sun's colour x -sky_q in
// linear light). (GunMetal-DX put these in atmos[4] / vol_clouds[3].w; atmos[4] stays the scene's.)
//
// THE NIGHT'S AMBIENT. EEP's ambient (the sky's fill light on surfaces, the haze, the WindLight
// sky the reflection probes capture) is set for Second Life's full-strength moon. At night it
// follows the moon too: x kNightAmbient x the moon's light fraction, at least
// kNightAmbientFloor (starlight and airglow on a moonless night), faded in with the night band in
// stops. The night exposure gives the whole scene back, up to what the moon's dimming took. A
// sky without night exposure (no reflection-probe ambiance, dynamic exposure off, the EEP sky's
// own exposure, vintage mode) has nothing to give it back: there the lift the night exposure
// would give (log2 of 1 / moon_full, at most kNightMaxLift) is baked into the moon's light and
// the ambient instead. Frame info night[0].w = amb (clean_atmos_common.glsl nightAmbient).
//
// THE CLOUDS' MOONLIGHT: clouds take the moon's light x kNightCloudLight, so they read at night
// while the disc, the sky and the ground keep kMoonlightScale. Frame info night[0].z =
// 1 + (kNightCloudLight - 1) x moon_share.
//
// THE CREATOR'S SKY: on a sky that is not one of Second Life's own (slSkyIsCreators) none of this
// runs -- the moon at full EEP strength, the old sky, clouds, water glint, moon disc and the switch
// at the horizon, as its creator lit it. (GunMetal-DX's GMDXRespectRegionSky, here always on.)
//
// llappviewer.cpp (the world frame's atmosphere) computes this once per frame and stores it in
// slNightLight(); the Vulkan renderer reads it there for the frame info and the physical sky.

constexpr float kNightAmbientFloor = 1.f / 256.f;   // (THE NIGHT'S AMBIENT above)
// GunMetal-DX's setting defaults, kept as constants (minimal config space).
constexpr float kMoonlightScale = 0.05f;   // GMDXMoonlightScale: a full moon high up, of the sun's light <GMDX> (a setting again: gmdxskysettings.h)
constexpr float kDuskSunHigh = 4.f;        // GMDXDuskSunHigh (degrees)
constexpr float kDuskSunLow = -10.f;       // GMDXDuskSunLow (degrees)
constexpr float kNightAmbient = 0.5f;      // GMDXNightAmbient
constexpr float kNightMaxLift = 6.f;       // GMDXNightExposureMaxLift (stops)
constexpr float kNightCloudLight = 4.9f;   // GMDXNightCloudLight <GMDX> (Orion's tuned value; GunMetal-DX's default 4.5)
constexpr float kMoonDiscLight = 1.1f;     // GMDXMoonDisc (the moon's disc as a light; 0 = LL's disc) <GMDX> (Orion's 1.1)
constexpr float kMoonGlow = 1.f;           // GMDXMoonGlow (the night bloom; 0 = none)

struct SLNightLight
{
    bool  on = false;      // RenderVKPhysicalNight, and not a creator's sky
    float moon = 0.f;      // the moon's light as a fraction of the sun's (linear), dusk band (and a baked lift) applied
    float moon_full = 0.f; // ... before the dusk band (and any baked lift): what physical night keeps of Second Life's
                           // full-strength moon (the night exposure's lift gives back at most that)
    bool  blended = false; // the sun is below kDuskSunHigh: dir / colour are the blend
    bool  sun_lead = true; // the sun gives more light than the moon (the haze glow is its)
    float dir[3] = {0.f, 0.f, 1.f};   // the scene light's direction (world, z up, unit)
    float color[3] = {};   // its colour before the WindLight extinction (sRGB space, as EEP's)
    float sky_dir[3] = {0.f, 0.f, 1.f};   // the WindLight sky's light: direction, z clamped at -0.1
    float sky_q = 1.f;     // ... its strength (see above)
    float night = 0.f;     // 0 by day .. 1 at night: the moon's share of the sky's light, from half
                           // kDuskSunLow (0) to kDuskSunLow (1), whatever the moon's phase
    float moon_share = 0.f; // the moon's share of the scene light (luminance, each after its own
                            // extinction): 0 by day, 1 once the sun's direct light is gone
    float amb = 1.f;       // the night's ambient: the factor on EEP's ambient light (linear), 1 by day
    bool  creators_sky = false; // the sky is a creator's, not one of Second Life's own (slSkyIsCreators)
    bool  custom_moon = false;  // ... and its moon texture is its own, not Second Life's default moon:
                                // drawn as Second Life draws it (no HDR disc, no phase shading)
};

// The scene light at dusk / night (see above): n.moon (from slMoonLight) gets the band's fade;
// fills n.dir / color / sun_lead / blended, the sky's light n.sky_dir / sky_q and the night's
// ambient n.amb. sun / moon unit directions, sunc EEP's sunlight colour, light_atten WindLight's
// light-path extinction per channel ((blue_density + haze_density / 4) x density_multiplier x
// max_y), the band in degrees of the sun's height, night_ambient kNightAmbient, baked_lift the
// most stops of night exposure to bake into the light on a sky that has no night exposure (0:
// the night exposure gives them back).
void slBlendNightLight(SLNightLight& n, const float sun[3], const float moon[3], const float sunc[3],
                       const float light_atten[3], float dusk_high, float dusk_low, float night_ambient,
                       float baked_lift);

// The moon's light as a fraction of the sun's (linear), from the sun and moon directions (unit),
// EEP moon_brightness and kMoonlightScale.
float slMoonLight(const float sun[3], const float moon[3], float moon_brightness, float scale);

// sRGB-space colour c (EEP's light colours) scaled by k in linear light, per channel.
void slScaleLinear(const float c[3], float k, float out[3]);

// This frame's (llappviewer.cpp writes it, the renderer reads it).
SLNightLight& slNightLight();

// THE CREATOR'S SKY. True when the sky in use is not one of Second Life's own: not the built-in
// default environment, not one of LLEnvironment's known skies (Sunrise, Midday, Sunset, Midnight)
// or LL's default sky / day cycle asset, and, for the region's or the parcel's environment, not
// one the simulator reports as its default. A region's or parcel's own EEP environment, a
// script's (llReplaceAgentEnvironment) and a personal sky from inventory are creators' skies.
// Such a sky gets Second Life's own light (SLNightLight::on stays off); the night sky's catalogue
// stars, Milky Way and planets stay.
bool slSkyIsCreators();
// llenvironment.cpp recordEnvironment: the simulator's is_default for the agent's region (parcel
// false) or parcel environment.
void slNoteEnvironmentDefault(bool parcel, bool is_default);

#endif // SL_NIGHTLIGHT_H
