/**
 * @file gmdxnightlight.cpp
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

// <GMDX> PHYSICAL NIGHT: see gmdxnightlight.h.

#include "llviewerprecompiledheaders.h"

#include "gmdxnightlight.h"

#include "llenvironment.h"
#include "llviewercontrol.h"

#include <algorithm>
#include <cmath>

namespace
{
// The shaders' atmosSrgbToLinear / an unclamped inverse (EEP colours go above 1).
float srgbToLinear(float cs)
{
    return cs <= 0.04045f ? cs / 12.92f : std::pow((std::max(cs, 0.f) + 0.055f) / 1.055f, 2.4f);
}
float linearToSrgb(float c)
{
    c = std::max(c, 0.f);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.f / 2.4f) - 0.055f;
}
} // namespace

float gmdxMoonLight(const float sun[3], const float moon[3], float moon_brightness, float scale)
{
    const float c = std::clamp(sun[0] * moon[0] + sun[1] * moon[1] + sun[2] * moon[2], -1.f, 1.f);
    const float a = 180.f - std::acos(c) * (180.f / 3.14159265f);   // phase angle (degrees)
    const float ks = std::pow(10.f, -0.4f * (0.026f * a + 4e-9f * a * a * a * a));
    return std::max(scale, 0.f) * std::clamp(2.f * moon_brightness, 0.f, 2.f) * ks;
}

void gmdxScaleLinear(const float c[3], float k, float out[3])
{
    for (int i = 0; i < 3; ++i) out[i] = linearToSrgb(srgbToLinear(c[i]) * k);
}

void gmdxBlendNightLight(GMDXNightLight& n, const float sun[3], const float moon[3], const float sunc[3],
                         const float light_atten[3], float dusk_high, float dusk_low, float night_ambient,
                         float baked_lift)
{
    // The band: 1 at and above dusk_high (day: the moon adds nothing), 0 at and below dusk_low.
    const float e = std::asin(std::clamp(sun[2], -1.f, 1.f)) * (180.f / 3.14159265f);

    // The WindLight sky's own light (header): the sun to half the band's low end, then the moon.
    {
        const float mid = 0.5f * std::min(dusk_low, -1.f);
        auto smooth = [](float t) { t = std::clamp(t, 0.f, 1.f); return t * t * (3.f - 2.f * t); };
        const float* d = e >= mid ? sun : moon;
        n.night = e >= mid ? 0.f : smooth((mid - e) / -mid);

        // The night's ambient and, on a sky without night exposure, the lift baked into the light
        // (header). In stops, faded in with the night.
        const float m = n.moon;
        const float lift = (baked_lift > 0.f && m > 0.f) ? std::exp2(std::min(baked_lift, -std::log2(m)) * n.night) : 1.f;
        n.moon *= lift;
        const float amb = std::max(std::max(night_ambient, 0.f) * m, kGmdxNightAmbientFloor);
        n.amb = std::clamp(lift * std::exp2(std::log2(amb) * n.night), 0.f, 1.f);

        n.sky_q = e >= mid ? smooth((e - mid) / -mid) : -n.moon * n.night;
        for (int c = 0; c < 3; ++c) n.sky_dir[c] = d[c];
        n.sky_dir[2] = std::max(n.sky_dir[2], -0.1f);
    }

    float b = std::clamp((e - dusk_low) / std::max(dusk_high - dusk_low, 0.1f), 0.f, 1.f);
    b = b * b * (3.f - 2.f * b);
    n.moon *= 1.f - b;
    if (b >= 1.f)
    {
        // Exactly Second Life's daytime light.
        n.blended = false;
        n.sun_lead = true;
        n.moon_share = 0.f;
        for (int c = 0; c < 3; ++c) { n.dir[c] = sun[c]; n.color[c] = sunc[c]; }
        return;
    }
    n.blended = true;

    // fillAtmosK's extinction for a light at height z (its clamped light norm).
    auto ext = [&](float z, int c) { return std::exp(-light_atten[c] / std::max(1e-6f, std::max(z, -0.1f))); };
    float moonc[3];
    gmdxScaleLinear(sunc, n.moon, moonc);
    const float lum[3] = {0.2126f, 0.7152f, 0.0722f};
    float sum[3], is = 0.f, im = 0.f;
    for (int c = 0; c < 3; ++c)
    {
        const float s = srgbToLinear(sunc[c] * ext(sun[2], c));
        const float m = srgbToLinear(moonc[c] * ext(moon[2], c));
        sum[c] = s + m;
        is += lum[c] * s;
        im += lum[c] * m;
    }
    // The direction turns by the sun's height, not by the two lights' strengths: near the horizon
    // the sun's light falls by orders of magnitude per degree, and any weighting by strength (even
    // its fourth root) swung the light, and the shadows, across the sky within ~0.5-1 degree of the
    // sun's travel. Over 6 degrees of it, from +3 to -3 (inside the band; 10-90 % of the turn
    // within ~3.6), centred on the horizon where the sun's own light goes. A moon below the
    // horizon gives the turn nothing (the light stays on the sun, which has no direct light by
    // then); it takes the direction as it rises (over its own -2 to +2 degrees).
    const float dhi = std::min(3.f, dusk_high), dlo = std::max(-3.f, std::min(dusk_low, dhi - 0.1f));
    float t = std::clamp((e - dlo) / (dhi - dlo), 0.f, 1.f);
    t = t * t * (3.f - 2.f * t);
    const float me = std::asin(std::clamp(moon[2], -1.f, 1.f)) * (180.f / 3.14159265f);
    float mp = std::clamp((me + 2.f) / 4.f, 0.f, 1.f);
    mp = mp * mp * (3.f - 2.f * mp);
    // Along the sky at an even angular pace (a straight blend of two far-apart directions turns
    // nearly all the way at its midpoint): sun -> h -> moon, two great-circle arcs, h above
    // between them (the zenith when they stand opposite on the horizon).
    const float x = (1.f - t) * mp;   // the moon's share of the direction (0: the sun exactly)
    float h[3];
    {
        const float sm = sun[0] * moon[0] + sun[1] * moon[1] + sun[2] * moon[2];
        for (int c = 0; c < 3; ++c) h[c] = sun[c] + moon[c];
        h[2] += 1.f - sm;
        const float hl = std::sqrt(h[0] * h[0] + h[1] * h[1] + h[2] * h[2]);
        for (int c = 0; c < 3; ++c) h[c] /= std::max(hl, 1e-6f);
    }
    auto slerp = [](const float* p, const float* q, float f, float* o)
    {
        const float th = std::acos(std::clamp(p[0] * q[0] + p[1] * q[1] + p[2] * q[2], -1.f, 1.f));
        if (th < 1e-4f) { for (int c = 0; c < 3; ++c) o[c] = p[c]; return; }
        const float a = std::sin((1.f - f) * th) / std::sin(th), b = std::sin(f * th) / std::sin(th);
        for (int c = 0; c < 3; ++c) o[c] = a * p[c] + b * q[c];
    };
    float v[3];
    if (x < 0.5f) slerp(sun, h, 2.f * x, v); else slerp(h, moon, 2.f * x - 1.f, v);
    const float len = std::max(std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]), 1e-6f);
    for (int c = 0; c < 3; ++c) n.dir[c] = v[c] / len;
    n.sun_lead = is > im;   // (neither: the moon's glow factor, 0 with the moon down, as LL)
    n.moon_share = is > 0.f ? im / (is + im) : 1.f;   // (GMDXNightCloudLight's gain)

    // The colour that the extinction at n.dir turns into the sum (0 where that direction's light
    // is gone anyway; capped at the two lights' own colours).
    for (int c = 0; c < 3; ++c)
    {
        const float k = light_atten[c] / std::max(1e-6f, std::max(n.dir[2], -0.1f));
        n.color[c] = k > 30.f ? 0.f : std::min(linearToSrgb(sum[c]) * std::exp(k), std::max(sunc[c], 0.f) + moonc[c]);
    }
}

GMDXNightLight& gmdxNightLight()
{
    static GMDXNightLight s;
    return s;
}

namespace
{
bool sRegionEnvDefault = true;   // the simulator's is_default (LL's reading: absent = default)
bool sParcelEnvDefault = true;
} // namespace

void gmdxNoteEnvironmentDefault(bool parcel, bool is_default)
{
    (parcel ? sParcelEnvDefault : sRegionEnvDefault) = is_default;
}

bool gmdxSkyIsCreators()
{
    static LLCachedControl<bool> s_sky_test(gSavedSettings, "GMDXSkyTest", false);
    if (s_sky_test) return false;
    LLEnvironment& env = LLEnvironment::instance();
    const LLEnvironment::DayInstance::ptr_t inst = env.getSelectedEnvironmentInstance();
    if (!inst) return false;
    const LLEnvironment::EnvSelection_t sel = inst->getEnvironmentSelection();
    if (sel == LLEnvironment::ENV_DEFAULT) return false;
    // A fixed sky (a personal or scripted one) carries its asset; a day cycle its own.
    LLUUID id;
    if (inst->getFlags() & LLEnvironment::DayInstance::NO_ANIMATE_SKY)
    {
        if (inst->getSky()) id = inst->getSky()->getAssetId();
    }
    else if (inst->getDayCycle())
    {
        id = inst->getDayCycle()->getAssetId();
    }
    if (id.notNull() && (id == LLSettingsDay::GetDefaultAssetId() || id == LLSettingsSky::GetDefaultAssetId()
                         || id == LLEnvironment::KNOWN_SKY_SUNRISE || id == LLEnvironment::KNOWN_SKY_MIDDAY
                         || id == LLEnvironment::KNOWN_SKY_LEGACY_MIDDAY || id == LLEnvironment::KNOWN_SKY_SUNSET
                         || id == LLEnvironment::KNOWN_SKY_MIDNIGHT))
        return false;
    if (sel == LLEnvironment::ENV_REGION) return !sRegionEnvDefault;
    // (a parcel that reports the default may be passing on the region's own day cycle)
    if (sel == LLEnvironment::ENV_PARCEL) return !(sParcelEnvDefault && sRegionEnvDefault);
    return true;   // a personal, scripted or edited sky that is not one of LL's
}
