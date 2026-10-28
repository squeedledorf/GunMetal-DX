/**
 * @file gmdxnighteyevk.h
 * @brief GunMetal-DX night eye: the settings behind the night exposure curve
 *        (clean_exposure.comp params4) and night vision (clean_tonemap.frag p3 / p4),
 *        and the photographic night look (GMDXNightLook: p4.z, the cold grade).
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

#ifndef GMDX_NIGHTEYEVK_H
#define GMDX_NIGHTEYEVK_H

// Header-only; included by llcleanrenderervk.cpp alone (inside LL_HAS_CLEAN_PIPELINE).
#include "llviewercontrol.h"
#include "../gmdxnightlight.h"   // the night band (GMDXNightLight::night)
#include <cmath>

struct GMDXNightEye
{
    // clean_exposure.comp params4: adaptation per stop below the night start (0 = LL's curve),
    // knee (stops), adaptation per stop past the knee, max extra lift (stops).
    float curve[4] = {0.f, 0.f, 0.f, 0.f};
    // clean_tonemap.frag p3: Purkinje strength, cd/m^2 per renderer unit, and the log2 exposure
    // values where the adaptation luminance is 5 cd/m^2 (mesopic starts) and 0.005 (scotopic).
    // All zero: no night vision.
    float vision[4] = {0.f, 0.f, 0.f, 0.f};
    float noise = 0.f;   // p4.x: low-light noise + acuity loss (0 = off)
    float grade = 0.f;   // p4.z: the photographic night's cold grade (0 = none), faded in by the band
};

// GMDXNightLook 1 (photo, the default): night as a photograph shows it instead of as the eye sees
// it -- about one stop under the day for a full moon instead of two (the curve's adaptation at
// least kNightPhotoAdapt), no Purkinje shift, and a cold film grade (clean_tonemap.frag
// gmdxNightGrade) faded in by the night vision band. 0 (eye): night-eye as it was.
constexpr F32 kNightPhotoAdapt = 0.75f;
constexpr F32 kNightPhotoGrade = 1.f;

// The night exposure curve and night vision for this frame. coef / exp_min / exp_max are atmos[14] as
// llappviewer.cpp fills it. Off (all zero, LL's curve exactly) with GMDXPhysicalNight 0, with
// the EEP sky's own exposure (RenderUseExposureSkySettings) and when the sky has no exposure
// range to adapt in (legacy skies, dynamic exposure off).
// NIGHT ONLY. The curve reads the metered scene, not the clock, so by day a view of the ground
// (darker than the night start) was lifted too and the day washed out unless the bright sky was
// in view. Everything here is weighted by the night band (GMDXNightLight::night: 0 with the sun
// above half GMDXDuskSunLow, 1 at GMDXDuskSunLow): the most lift and the grade fade in with it,
// and by day it is all zero, LL's exposure exactly.
// NEVER BRIGHTER THAN THE OLD NIGHT. The lift gives back at most what physical night took from
// Second Life's moon: log2(1 / GMDXNightLight::moon_full) stops (GMDXMoonlightScale x 2 x
// moon_brightness x the phase; 4.3 stops for the default full moon, more for a smaller phase),
// and never more than GMDXNightExposureMaxLift. So a night that was dark with the old moon
// stays at least that dark instead of being pumped up from its own darkness.
// THE CREATOR'S SKY (GMDXRespectRegionSky, default on): on a region's, parcel's or personal EEP
// sky that is not one of Second Life's own (gmdxSkyIsCreators) none of this runs: no lift, no
// grade, no Purkinje shift or noise, the stock exposure as the creator set the sky up for. (The
// light is Second Life's there too: llappviewer.cpp leaves GMDXNightLight::on off.)
inline GMDXNightEye gmdxNightEye(float coef, float exp_min, float exp_max)
{
    static LLCachedControl<bool> s_night(gSavedSettings, "GMDXPhysicalNight", true);
    static LLCachedControl<bool> s_sky_exp(gSavedSettings, "RenderUseExposureSkySettings", false);
    static LLCachedControl<F32>  s_adapt(gSavedSettings, "GMDXNightExposureAdapt", 0.5f);
    static LLCachedControl<F32>  s_knee(gSavedSettings, "GMDXNightExposureKnee", 4.f);
    static LLCachedControl<F32>  s_deep(gSavedSettings, "GMDXNightExposureDeepAdapt", 0.75f);
    static LLCachedControl<F32>  s_max(gSavedSettings, "GMDXNightExposureMaxLift", 6.f);
    static LLCachedControl<F32>  s_purkinje(gSavedSettings, "GMDXNightVisionPurkinje", 1.f);
    static LLCachedControl<F32>  s_cd(gSavedSettings, "GMDXNightVisionLuminance", 10.f);
    static LLCachedControl<F32>  s_noise(gSavedSettings, "GMDXNightVisionNoise", 0.f);
    static LLCachedControl<S32>  s_look(gSavedSettings, "GMDXNightLook", 1);
    const bool photo = s_look == 1;
    GMDXNightEye e;
    if (!s_night || s_sky_exp || !(exp_max > exp_min) || !std::isfinite(exp_min) || !std::isfinite(exp_max))
        return e;
    const GMDXNightLight& nl = gmdxNightLight();
    const F32 w = nl.on ? llclamp(nl.night, 0.f, 1.f) : 0.f;   // the night band
    if (w <= 0.f) return e;   // day: LL's curve exactly
    static LLCachedControl<bool> s_respect(gSavedSettings, "GMDXRespectRegionSky", true);
    if (s_respect && nl.creators_sky) return e;   // a creator's sky: their exposure and colours
    const F32 k1 = llclamp((F32)s_adapt, 0.f, 1.f);
    if (k1 <= 0.f) return e;
    e.curve[0] = photo ? llmax(k1, kNightPhotoAdapt) : k1;
    e.curve[1] = llclamp((F32)s_knee, 0.f, 20.f);
    e.curve[2] = llclamp((F32)s_deep, 0.f, 1.f);
    F32 cap = llclamp((F32)s_max, 0.f, 12.f);
    if (nl.moon_full > 0.f) cap = llmin(cap, llmax(-log2f(nl.moon_full), 0.f));   // (0: the moon gives nothing, the full cap)
    e.curve[3] = cap * w;

    // Night vision band. Adaptation luminance = the metered scene in cd/m^2 (x GMDXNightVisionLuminance).
    // The tonemap pass has only the adapted exposure, so the band goes over as exposure values:
    // the curve's value where the scene is at 5 and at 0.005 cd/m^2 (CIE 191 mesopic range).
    // Above the night start the exposure is LL's and carries no luminance: the band starts
    // there at the earliest (day, dusk and shade get no night vision).
    const F32 purkinje = photo ? 0.f : llclamp((F32)s_purkinje, 0.f, 1.f) * w;
    const F32 noise = llclamp((F32)s_noise, 0.f, 1.f) * w;
    const F32 grade = photo ? kNightPhotoGrade * w : 0.f;
    const F32 cd = (F32)s_cd;
    if ((purkinje > 0.f || noise > 0.f || grade > 0.f) && cd > 0.f && std::isfinite(cd))
    {
        const F32 night_start = llmax(coef, 1e-4f) * 0.25f;   // as clean_exposure.comp
        auto lift = [&](F32 x)   // the curve's extra stops, uncapped
        {
            return x <= e.curve[1] ? e.curve[0] * x : e.curve[0] * e.curve[1] + e.curve[2] * (x - e.curve[1]);
        };
        const F32 xa = llmax(log2f(night_start * cd / 5.f), 0.f);
        const F32 xb = llmax(log2f(night_start * cd / 0.005f), xa + 0.01f);
        const F32 base = log2f(exp_max);
        e.vision[0] = purkinje;
        e.vision[1] = cd;
        e.vision[2] = base + lift(xa);
        e.vision[3] = llmax(base + lift(xb), e.vision[2] + 0.01f);
        e.noise = noise;
        e.grade = grade;
    }
    return e;
}

#endif // GMDX_NIGHTEYEVK_H
