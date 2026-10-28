/**
 * @file gmdxnightlight.h
 * @brief GunMetal-DX physical night: the moon's light from its real brightness and phase.
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

#ifndef GMDX_NIGHTLIGHT_H
#define GMDX_NIGHTLIGHT_H

// <GMDX> PHYSICAL NIGHT (GMDXPhysicalNight, default on; off = the old Second Life night).
//
// Second Life lights the night with the moon at the sun's full strength: EEP has one light
// colour (LLSettingsSky::getMoonlightColor returns the sunlight colour) and moon_brightness only
// dims the moon's disc and halo. Here the moon's light is the sun's colour times
//   GMDXMoonlightScale (a full moon high in the sky: this fraction of the sun's light)
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
//   - the moon's light fades in over a twilight band of the sun's height (GMDXDuskSunHigh,
//     +4 degrees, to GMDXDuskSunLow, -10): above it the moon adds nothing and the day is
//     exactly as before; below it the moon is at its full strength. (Real twilight outshines a
//     full moon until the sun is ~8-10 degrees down; that skylight is EEP's ambient and the
//     physical sky's, so the moon's direct light comes in as it would show.)
//   - the light's direction turns from the sun to the moon by the sun's height, from +3 to -3
//     degrees (inside the band; 10-90 % of the turn within ~3.6 degrees, at most ~45 degrees of
//     turn per degree of sun), centred on the horizon where the sun's own light goes, along the
//     sky at an even pace (over the top when they stand opposite). A moon below the horizon
//     leaves the light on the sun (it has no direct light by then) and takes it over as it
//     rises. (Weighting by the lights' strengths swung it within ~0.5-1 degree: the sun's light
//     falls by orders of magnitude per degree there.)
// Its colour is given before the extinction AT THAT DIRECTION, so everything that applies
// WindLight's extinction to atmos[2] (fillAtmosK: surfaces, fog; god rays) gets exactly the sum.
//
// THE WINDLIGHT SKY'S OWN LIGHT. The dome (the Classic sky, its #819 table, the reflection probes'
// sky) and the classic 2D clouds apply their own extinction per view direction, so the ground's
// back-derived colour would starve them (it went to 0 before the sun had set). They get a light
// of their own instead, one body at a time: the sun, its EEP colour in full while it is up and
// fading to nothing at half GMDXDuskSunLow (-5 degrees; its dome glow below the horizon, as
// Second Life lights the dome then), then the moon at its real strength, growing from there to
// full at GMDXDuskSunLow. The switch falls where both are zero. Frame info: atmos[4] its clamped
// direction (the ground's clamped norm comes from atmos[7] then), vol_clouds[3].w its strength
// sky_q (>= 0 the sun: atmos[8] x sky_q; < 0 the moon: the sun's colour x -sky_q in linear light).
//
// THE NIGHT'S AMBIENT (GMDXNightAmbient). EEP's ambient (the sky's fill light on surfaces, the
// haze, the WindLight sky the reflection probes capture) is set for Second Life's full-strength
// moon. Left as it was, it outshone the dimmed moon some 14 to 1 on SL's Midnight sky: the night's
// brightness was all fill light, the moon lit nothing visibly, and its shadows (and the clouds')
// took nothing away. At night it follows the moon too: x GMDXNightAmbient x the moon's light
// fraction (0.5: moonlight against shadow about twice Second Life's old ratio), at least
// kGmdxNightAmbientFloor (starlight and airglow on a moonless night), faded in with the night band
// in stops. The night exposure gives the whole scene back, up to what the moon's dimming took.
// A sky without night exposure (a legacy sky without reflection-probe ambiance, dynamic exposure
// off, the EEP sky's own exposure, vintage mode) has nothing to give it back: there the lift the
// night exposure would give (log2 of 1 / moon_full, at most GMDXNightExposureMaxLift) is baked
// into the moon's light and the ambient instead, so both kinds of sky end up alike on screen.
// Frame info atmos[6].z = 2 - amb (its "enabled" test is > 0.5; 1 = stock), clean_atmos_common.glsl
// gmdxNightAmbient.
//
// THE CLOUDS' MOONLIGHT (GMDXNightCloudLight): clouds take the moon's light x the setting, so
// they read at night while the disc, the sky and the ground keep GMDXMoonlightScale. Frame info
// vol_clouds[2].w = 1 + (setting - 1) x moon_share (1 by day and with physical night off).
//
// llappviewer.cpp (the world frame's atmosphere) computes this once per frame and stores it in
// gmdxNightLight(); the Vulkan renderer reads it there for the frame info (gmdx_misc.w, see
// clean_atmos_common.glsl gmdxNightOn) and the physical sky (gmdxphysskyvk.cpp).

constexpr float kGmdxNightAmbientFloor = 1.f / 256.f;   // (THE NIGHT'S AMBIENT above)

struct GMDXNightLight
{
    bool  on = false;      // GMDXPhysicalNight
    float moon = 0.f;      // the moon's light as a fraction of the sun's (linear), dusk band (and a baked lift) applied
    float moon_full = 0.f; // ... before the dusk band (and any baked lift): what physical night keeps of Second Life's
                           // full-strength moon (the night exposure's lift gives back at most that)
    bool  blended = false; // the sun is below GMDXDuskSunHigh: dir / colour are the blend
    bool  sun_lead = true; // the sun gives more light than the moon (the haze glow is its)
    float dir[3] = {0.f, 0.f, 1.f};   // the scene light's direction (world, z up, unit)
    float color[3] = {};   // its colour before the WindLight extinction (sRGB space, as EEP's)
    float sky_dir[3] = {0.f, 0.f, 1.f};   // the WindLight sky's light: direction, z clamped at -0.1
    float sky_q = 1.f;     // ... its strength (see above)
    float night = 0.f;     // 0 by day .. 1 at night: the moon's share of the sky's light, from half
                           // GMDXDuskSunLow (0) to GMDXDuskSunLow (1), whatever the moon's phase
    float moon_share = 0.f; // the moon's share of the scene light (luminance, each after its own
                            // extinction): 0 by day, 1 once the sun's direct light is gone
    float amb = 1.f;       // the night's ambient: the factor on EEP's ambient light (linear), 1 by day
    bool  creators_sky = false; // the sky is a creator's, not one of Second Life's own (gmdxSkyIsCreators)
    bool  custom_moon = false;  // ... and its moon texture is its own, not Second Life's default moon:
                                // drawn as Second Life draws it (no HDR disc, no phase shading)
};

// The scene light at dusk / night (see above): n.moon (from gmdxMoonLight) gets the band's fade;
// fills n.dir / color / sun_lead / blended, the sky's light n.sky_dir / sky_q and the night's
// ambient n.amb. sun / moon unit directions, sunc EEP's sunlight
// colour, light_atten WindLight's light-path extinction per channel ((blue_density +
// haze_density / 4) x density_multiplier x max_y), the band in degrees of the sun's height,
// night_ambient GMDXNightAmbient, baked_lift the most stops of night exposure to bake into the
// light on a sky that has no night exposure (0: the night exposure gives them back).
void gmdxBlendNightLight(GMDXNightLight& n, const float sun[3], const float moon[3], const float sunc[3],
                         const float light_atten[3], float dusk_high, float dusk_low, float night_ambient,
                         float baked_lift);

// The moon's light as a fraction of the sun's (linear), from the sun and moon directions (unit),
// EEP moon_brightness and GMDXMoonlightScale.
float gmdxMoonLight(const float sun[3], const float moon[3], float moon_brightness, float scale);

// sRGB-space colour c (EEP's light colours) scaled by k in linear light, per channel.
void gmdxScaleLinear(const float c[3], float k, float out[3]);

// This frame's (llappviewer.cpp writes it, the renderer reads it).
GMDXNightLight& gmdxNightLight();

// THE CREATOR'S SKY (GMDXRespectRegionSky). True when the sky in use is not one of Second Life's
// own: not the built-in default environment, not one of LLEnvironment's known skies (Sunrise,
// Midday, Sunset, Midnight) or LL's default sky / day cycle asset, and, for the region's or the
// parcel's environment, not one the simulator reports as its default. A region's or parcel's own
// EEP environment, a script's (llReplaceAgentEnvironment) and a personal sky from inventory are
// creators' skies. Sky Test (GMDXSkyTest) never is: it is for tuning physical night itself.
// With GMDXRespectRegionSky (default on) such a sky gets Second Life's own light: llappviewer.cpp
// leaves GMDXNightLight::on off (the moon at full EEP strength, the old sky, clouds, water glint,
// moon disc and the switch at the horizon). The night sky's catalogue stars, Milky Way and planets
// (GMDXNightSky, keyed on the setting) and the cloud corona stay.
bool gmdxSkyIsCreators();
// llenvironment.cpp recordEnvironment: the simulator's is_default for the agent's region (parcel
// false) or parcel environment.
void gmdxNoteEnvironmentDefault(bool parcel, bool is_default);

#endif // GMDX_NIGHTLIGHT_H
