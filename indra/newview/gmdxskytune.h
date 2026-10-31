/**
 * @file gmdxskytune.h
 * @brief GunMetal-DX: the Sky Debugger's live knobs and readout (never saved).
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

#ifndef GMDX_SKYTUNE_H
#define GMDX_SKYTUNE_H

// <GMDX> THE SKY DEBUGGER (Develop > Rendering > Sky Debugger..., gmdxfloaterskydebug.cpp). The sky's
// deep tuning is constants (a setting earns a place on Graphics > Sky only if a player would change
// it); the debugger moves them live, in memory, never saved. Each knob stands in for one constant
// AT ITS USE SITE:
//     const F32 kStarSize = gmdxSkyTune().star_size(::kStarSize);
// returns the constant until the debugger moves the knob, and tells the debugger the constant's
// value (its default and its Reset). Reset All = every knob back to its constant: exactly the
// look without the debugger. Header-only. (Written by the UI, read by the frame code: a float
// each, a debug tool, no locking. Flags, not NaN: Windows builds with /fp:fast.)

struct GMDXKnob
{
    float v = 0.f;       // the debugger's value, while set
    bool  set = false;   // the debugger moved it (else the constant)
    float def = 0.f;     // the constant, as its use site last passed it
    bool  seen = false;  // ... once its use site has run
    float operator()(float constant) { def = constant; seen = true; return set ? v : constant; }
    float value() const { return set ? v : def; }
};

struct GMDXSkyTune
{
    // Night (slnightlight.h constants; llappviewer.cpp, vkgfx/llcleanrenderervk.cpp)
    GMDXKnob night_cloud_light;   // kNightCloudLight: the clouds' moonlight gain at night
    GMDXKnob moon_disc;           // kMoonDiscLight: the moon's disc as a light
    GMDXKnob night_ambient;       // kNightAmbient
    GMDXKnob night_max_lift;      // kNightMaxLift (stops)
    GMDXKnob dusk_high;           // kDuskSunHigh (degrees)
    GMDXKnob dusk_low;            // kDuskSunLow (degrees)
    // Night exposure curve (vkgfx/slnighteyevk.h)
    GMDXKnob night_adapt;         // kNightAdapt
    GMDXKnob night_knee;          // kNightKnee (stops)
    GMDXKnob night_deep_adapt;    // kNightDeepAdapt
    GMDXKnob night_photo_adapt;   // kNightPhotoAdapt
    GMDXKnob night_photo_grade;   // kNightPhotoGrade
    // Cloud corona (vkgfx/llcleanrenderervk.cpp night[2])
    GMDXKnob corona;              // strength
    GMDXKnob corona_droplet;      // droplet diameter (um)
    GMDXKnob halo22;              // 22-degree halo strength
    // Stars (slnightsky.cpp)
    GMDXKnob star_twinkle;        // kScintillation
    GMDXKnob star_size;           // kStarSize
    GMDXKnob star_saturation;     // kStarSaturation
    GMDXKnob airglow;             // kAirglowScale
    GMDXKnob zodiacal;            // kZodiacal
    GMDXKnob deep_sky;            // kDeepSky
    GMDXKnob planets;             // kPlanetsAmount
    GMDXKnob satellites;          // kSatellites
    GMDXKnob meteors;             // kMeteors
    // The physical sky's air (vkgfx/llcleanphysskyvk.cpp, gmdxSkyAerial below)
    GMDXKnob sky_eep_air;         // kGmdxEepAir: how far EEP's density / haze / max_y shape the air
    GMDXKnob sky_aerial;          // kGmdxSkyAerial: aerial perspective stretch (0 = Slipstream's camera medium)
    // Volumetric clouds (vkgfx/gmdxvolcloudsvk.cpp, gmdx_cloud_*.glsl)
    GMDXKnob cloud_albedo;        // single-scattering albedo (in the EEP cloud colour's hue)
    GMDXKnob cloud_powder;        // beer-powder scale
    GMDXKnob cloud_sky_ambient;   // sky ambient factor
    GMDXKnob cloud_ground;        // light bounced off the ground
    GMDXKnob cloud_erosion;       // detail erosion
    GMDXKnob cloud_curl;          // the detail curl near the base (m)
    GMDXKnob cloud_light_len;     // the self-shadow march's length (m)
    GMDXKnob cloud_moon_g;        // the moon's silver-lining lobe: g
    GMDXKnob cloud_moon_lobe;     // ... its weight
    GMDXKnob cloud_min_width;     // the smallest cloud footprint (a share of the thickness)
    GMDXKnob cloud_world;         // Slipstream's world coverage field: strength (0 = off)
    GMDXKnob cloud_body_density;  // sculpted bodies: density
    GMDXKnob cloud_body_share;    // ... the share of cells that hold one
    GMDXKnob cloud_steps;         // march steps x
    GMDXKnob cloud_light_steps;   // self-shadow samples (default: the quality tier's)
    GMDXKnob cloud_layer_margin;  // the camera counts as in the layer this far above / below it (m)
    // Views (gmdx_cloud_common.glsl GC_VIEW_*, gmdx_cloud_composite.frag): 0 none, 1 cloud density,
    // 2 light through the cloud, 3 weather map, 4 its height scale, 5 world coverage, 6 cloud shadow
    // map, 7 cloud panorama, 8 sky transmittance LUT, 9 sky-view LUT.
    int cloud_view = 0;
};

inline GMDXSkyTune& gmdxSkyTune() { static GMDXSkyTune t; return t; }

// THE PHYSICAL SKY'S AERIAL PERSPECTIVE (two readers: vkgfx/llcleanphysskyvk.cpp physSkyFill, and
// vkgfx/llcleanrenderervk.cpp ensureVolFog, which then skips Slipstream's camera medium). GunMetal-DX's
// own: WindLight's amount of haze on the scene and the clouds, coloured by the physical sky behind it,
// the physical air's extinction stretched by this factor on top (SL's draw distances are short for
// real air; clean_atmos_common.glsl atmosScatterHaze). 4.5 = Orion's RenderVKPhysicalSkyAerial before
// Slipstream's port made it a constant. 0 = Slipstream's physical camera medium (#1266) instead.
constexpr float kGmdxSkyAerial = 4.5f;
inline float gmdxSkyAerial() { const float s = gmdxSkyTune().sky_aerial(kGmdxSkyAerial); return s > 0.f ? s : 0.f; }

// What the frame code saw last frame, for the debugger's readout.
struct GMDXSkyReadout
{
    // vkgfx/gmdxvolcloudsvk.cpp
    bool  clouds_live = false;
    bool  clouds_in_layer = false;
    bool  clouds_temporal = false;
    bool  clouds_physical = false;   // the physical camera medium (Slipstream #1266)
    int   clouds_quality = 0, clouds_pattern = 0, clouds_steps = 0, clouds_light_steps = 0;
    float clouds_base = 0.f, clouds_top = 0.f, clouds_tile = 0.f, clouds_base_period = 0.f;
    float clouds_light[3] = {0.f, 0.f, 1.f};   // the light the shadow map is built for
    float clouds_wind[2] = {0.f, 0.f};         // the total wind (m, unwrapped)
    float clouds_wind_extra[2] = {0.f, 0.f};   // ... past the region's own (GMDXCloudWindSpeed)
    float clouds_shadow_age = 0.f;             // frames since the shadow map's refresh
    bool  clouds_pano_valid = false;
    // vkgfx/slnighteyevk.h
    float night_lift_cap = 0.f;                // the night exposure's extra stops at most, this frame
};

inline GMDXSkyReadout& gmdxSkyReadout() { static GMDXSkyReadout r; return r; }

#endif // GMDX_SKYTUNE_H
