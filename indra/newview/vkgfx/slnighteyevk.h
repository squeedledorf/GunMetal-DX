/**
 * @file slnighteyevk.h
 * @brief Physical night's night eye: the night exposure curve (clean_exposure.comp params4)
 *        and night vision / the photographic night grade (clean_tonemap.frag p3 / p4).
 *
 * Ported from GunMetal-DX (squeedledorf, LGPL 2.1), v0.3.0 cf5f1f4 (vkgfx/gmdxnighteyevk.h).
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

#ifndef SL_NIGHTEYEVK_H
#define SL_NIGHTEYEVK_H

// Header-only; included by llcleanrenderervk.cpp alone (inside LL_HAS_CLEAN_PIPELINE).
#include "llviewercontrol.h"
#include "slnightlight.h"   // the night band (SLNightLight::night)
#include "gmdxskysettings.h" // <GMDX> GMDXNightLook
#include "gmdxskytune.h"     // <GMDX> the Sky Debugger's knobs and readout
#include <cmath>

struct SLNightEye
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

// GunMetal-DX's setting defaults (GMDXNightExposure*, GMDXNightVision*), kept as constants
// (minimal config space), and its default look (GMDXNightLook 1, PHOTO): night as a photograph
// shows it instead of as the eye sees it -- about one stop under the day for a full moon instead
// of two (the curve's adaptation at least kNightPhotoAdapt), no Purkinje shift, and a cold film
// grade (clean_tonemap.frag nightGrade) faded in by the night vision band. (The eye look, 0, is
// not exposed: kNightPhotoLook false gives it back for an A/B in code.)
constexpr bool  kNightPhotoLook   = true;
constexpr F32   kNightPhotoAdapt  = 0.75f;
constexpr F32   kNightPhotoGrade  = 1.f;
constexpr F32   kNightAdapt       = 0.5f;    // GMDXNightExposureAdapt
constexpr F32   kNightKnee        = 4.f;     // GMDXNightExposureKnee (stops)
constexpr F32   kNightDeepAdapt   = 0.75f;   // GMDXNightExposureDeepAdapt
constexpr F32   kNightPurkinje    = 1.f;     // GMDXNightVisionPurkinje (eye look only)
constexpr F32   kNightLuminance   = 10.f;    // GMDXNightVisionLuminance (cd/m^2 per renderer unit)
constexpr F32   kNightNoise       = 0.f;     // GMDXNightVisionNoise (off)

// The night exposure curve and night vision for this frame. coef / exp_min / exp_max are atmos[14] as
// llappviewer.cpp fills it. Off (all zero, LL's curve exactly) with physical night off, with
// the EEP sky's own exposure (RenderUseExposureSkySettings) and when the sky has no exposure
// range to adapt in (legacy skies, dynamic exposure off).
// NIGHT ONLY. The curve reads the metered scene, not the clock, so by day a view of the ground
// (darker than the night start) was lifted too and the day washed out unless the bright sky was
// in view. Everything here is weighted by the night band (SLNightLight::night: 0 with the sun
// above half kDuskSunLow, 1 at kDuskSunLow): the most lift and the grade fade in with it, and by
// day it is all zero, LL's exposure exactly.
// NEVER BRIGHTER THAN THE OLD NIGHT. The lift gives back at most what physical night took from
// Second Life's moon: log2(1 / SLNightLight::moon_full) stops (kMoonlightScale x 2 x
// moon_brightness x the phase; 4.3 stops for the default full moon, more for a smaller phase),
// and never more than kNightMaxLift. So a night that was dark with the old moon stays at least
// that dark instead of being pumped up from its own darkness.
// THE CREATOR'S SKY: on a sky that is not one of Second Life's own (slSkyIsCreators) none of this
// runs (SLNightLight::on is off there): the stock exposure as the creator set the sky up for.
inline SLNightEye slNightEye(float coef, float exp_min, float exp_max)
{
    static LLCachedControl<bool> s_sky_exp(gSavedSettings, "RenderUseExposureSkySettings", false);
    SLNightEye e;
    gmdxSkyReadout().night_lift_cap = 0.f;   // <GMDX> (the Sky Debugger's readout)
    if (s_sky_exp || !(exp_max > exp_min) || !std::isfinite(exp_min) || !std::isfinite(exp_max))
        return e;
    const SLNightLight& nl = slNightLight();
    const F32 w = nl.on ? llclamp(nl.night, 0.f, 1.f) : 0.f;   // the night band
    if (w <= 0.f) return e;   // day: LL's curve exactly
    const bool kNightPhotoLook = gmdxNightPhotoLook();   // <GMDX> GMDXNightLook (1 photo, 0 eye) in place of the constant
    // <GMDX> the curve's constants with the Sky Debugger's knobs on them (gmdxskytune.h)
    GMDXSkyTune& tune = gmdxSkyTune();
    const F32 kNightAdapt = tune.night_adapt(::kNightAdapt), kNightKnee = tune.night_knee(::kNightKnee);
    const F32 kNightDeepAdapt = tune.night_deep_adapt(::kNightDeepAdapt), kNightPhotoAdapt = tune.night_photo_adapt(::kNightPhotoAdapt);
    const F32 kNightPhotoGrade = tune.night_photo_grade(::kNightPhotoGrade), kNightMaxLift = tune.night_max_lift(::kNightMaxLift);
    // </GMDX>
    const F32 k1 = kNightAdapt;
    e.curve[0] = kNightPhotoLook ? llmax(k1, kNightPhotoAdapt) : k1;
    e.curve[1] = kNightKnee;
    e.curve[2] = kNightDeepAdapt;
    F32 cap = kNightMaxLift;
    if (nl.moon_full > 0.f) cap = llmin(cap, llmax(-log2f(nl.moon_full), 0.f));   // (0: the moon gives nothing, the full cap)
    e.curve[3] = cap * w;
    gmdxSkyReadout().night_lift_cap = e.curve[3];   // <GMDX>

    // Night vision band. Adaptation luminance = the metered scene in cd/m^2 (x kNightLuminance).
    // The tonemap pass has only the adapted exposure, so the band goes over as exposure values:
    // the curve's value where the scene is at 5 and at 0.005 cd/m^2 (CIE 191 mesopic range).
    // Above the night start the exposure is LL's and carries no luminance: the band starts
    // there at the earliest (day, dusk and shade get no night vision).
    const F32 purkinje = kNightPhotoLook ? 0.f : kNightPurkinje * w;
    const F32 noise = kNightNoise * w;
    const F32 grade = kNightPhotoLook ? kNightPhotoGrade * w : 0.f;
    const F32 cd = kNightLuminance;
    if (purkinje > 0.f || noise > 0.f || grade > 0.f)
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

#endif // SL_NIGHTEYEVK_H
