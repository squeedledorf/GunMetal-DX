/**
 * @file gmdxnightsky.cpp
 * @brief GunMetal-DX night sky (see gmdxnightsky.h).
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

// <GMDX> Data: app_settings/gmdx_bsc5.bin, the Yale Bright Star Catalogue 5th Revised Ed.
// (Hoffleit & Warren 1991, CDS V/50; 9096 stars to V 7.96), and app_settings/gmdx_milkyway.jpg,
// ESO/S. Brunier's Milky Way panorama (eso0932a, CC BY 4.0; 2048 x 1024, its stars taken out),
// both made by scripts/nightsky/gmdx_night_sky_assets.py.
//
// Where the stars stand. Second Life has no date, place or Earth: an EEP sky moves the sun and the
// moon along whatever paths its creator keyed. The stars are tied to the sun, as the real sky's
// are over a night: the celestial pole stands GMDXStarLatitude degrees above the north (+Y)
// horizon, and the sky turns about it with the sun -- the sun's hour angle about that pole puts
// it where the catalogue sky has the sun on the date GMDXNightSkySunLongitude names (the sun's
// ecliptic longitude: 90 = June solstice, a northern summer night with the Milky Way's core in
// the south). A day cycle's moving sun turns the stars at its pace, a fixed sky holds them still.
// The moon keeps EEP's place (it is not tied to the stars).
//
// How bright. A star's light follows its V magnitude (x 2.512 a magnitude), its colour its B-V
// (blackbody, Ballesteros 2012), dimmed by the air's extinction toward the horizon (in the
// shader). EEP's star_brightness stays the artist's scalar (250, the default, = 1). Twilight and
// moonlight take the contrast away as they brighten the real sky: the stars' share is
// sqrt(dark / (dark + twilight + moonlit sky)) -- a full moon high up (a sky ~40x a moonless
// one) leaves about the stars to V 4, the end of civil twilight only the brightest few.
//
// The diffuse glow of a moonless night is measured in its zenith airglow (kAirglow, a guess in
// the sky's linear units to tune by eye): the Milky Way's core ~3x it; the sky itself a dark navy
// (kNsNightBlue), the airglow a faint tint low over the horizon, the zodiacal light a faint cone
// on the sun's side after dusk (gmdx_night_sky.glsl; game look, not olive). They take the
// same twilight / moonlight share, without the square root: a full moon high up all but hides
// them, a quarter moon leaves ~1/5.
//
// PLANETS (GMDXPlanets). Mercury, Venus, Mars, Jupiter and Saturn on their mean Keplerian orbits
// (E. M. Standish, "Approximate Positions of the Planets", JPL Solar System Dynamics: the J2000
// elements and their rates per century, valid 1800-2050), seen from an Earth placed where the
// season puts it: the sun at ecliptic longitude GMDXNightSkySunLongitude, as the stars have it.
// The planets themselves move on the real calendar (today's date, UTC), so they creep along the
// ecliptic from night to night while the season stays where it is set; Mercury and Venus never
// stray far from the sun (dusk / dawn). Brightness: V = H + 5 log10(r delta) + a linear phase
// term (game-good, not Mallama 2018), then the stars' units; colour by eye.
//
// METEORS (GMDXMeteors, GMDXMeteorShower). The Lightbox meteors (llcleanrenderervk.cpp, the photo
// studio's Sky tab: on / most at once / spawn interval) stay the streaks; physical night only
// sets how often: kNightMeteors of the stock rate (a few a minute) x GMDXMeteors, by darkness as
// the stars (none by day or in bright twilight, fewer under a full moon). A shower adds
// kShowerMeteors x the radiant's height of meteors flying away from the radiant of the season's
// shower (the nearest peak to GMDXNightSkySunLongitude in kShowers, IMO dates and radiants).
//
// SATELLITES (GMDXSatellites). A few faint points (V 3.5-5, now and then a bright one) gliding
// along great circles at ~0.5-0.9 deg/s from dusk on, each a satellite kSatelliteKm up: lit while
// it is out of the Earth's shadow (a cylinder behind the Earth from EEP's sun), so they shine in
// the hour or two after dusk (the sun down to ~20 deg below the horizon overhead, lower toward
// the sun's side) and fade out as they cross into the shadow. World directions (not the stars'
// frame: they are not tied to the sky's turn); world clock time, so Freeze World holds them.
//
// DEEP SKY (GMDXDeepSky): the Andromeda galaxy, the Orion nebula, the Magellanic Clouds and the
// Pleiades' haze as soft procedural blobs at their real places (gmdx_night_sky.glsl nsDeepSky),
// in the diffuse glow's units, washed out by twilight and moonlight as the Milky Way is.
//
// LOOK. GMDXStarLimitMagnitude (fewer, brighter stars), GMDXStarSize, GMDXStarSaturation (stars,
// planets, Milky Way, deep sky), GMDXMilkyWayRotation / Tilt (the band moved off the stars, for
// a sky that is not Earth's). GMDXFantasySky: a game look on top of all of them, the kFantasy*
// multipliers: brighter, bigger, more colourful stars, a much stronger Milky Way with its faint
// outskirts lifted, more shooting stars and deep-sky glow.

#include "llviewerprecompiledheaders.h"

#include "gmdxnightsky.h"
#include "gmdxnightskygrid.h"
#include "gmdxspacealt.h"   // <GMDX> space at altitude

#include "lldate.h"
#include "lldir.h"
#include "llenvironment.h"
#include "llfile.h"
#include "llimage.h"
#include "llimagejpeg.h"
#include "llrand.h"
#include "llsettingssky.h"
#include "llviewercontrol.h"
#include "llviewertexture.h"
#include "llworldclock.h"

#include <algorithm>
#include <iterator>

namespace
{
// The textures made here; never released (ponytail: they would outlive LL's texture list at exit).
std::vector<LLPointer<LLViewerFetchedTexture>>& held()
{
    static auto* v = new std::vector<LLPointer<LLViewerFetchedTexture>>();
    return *v;
}

LLUUID sStarGrid, sMilkyWay;
bool   sStarTried = false, sMilkyWayTried = false;
constexpr F32 kAirglow = 0.004f;   // a moonless night's zenith airglow (linear sky units)
// GMDXFantasySky multipliers.
constexpr F32 kFantasyStars = 1.6f, kFantasyStarSize = 1.3f, kFantasySaturation = 2.2f, kFantasyMilkyWay = 2.5f,
              kFantasyMilkyWayExp = 0.75f, kFantasyDeepSky = 2.5f, kFantasyMeteors = 3.f, kFantasyPlanets = 1.3f,
              kFantasyZodiacal = 1.5f;
constexpr F32 kNightMeteors = 0.25f;   // of the Lightbox rate (4 s: 15 a minute) on a dark night
constexpr F32 kShowerMeteors = 8.f;    // a shower's extra meteors (x the sporadic rate) with the radiant overhead
F32 sMeteorRate = -1.f;               // meteorRateScale (from fillFrame)
F32 sShowerShare = 0.f;               // of the spawns, the shower's
LLVector3 sRadiant;                   // world direction of the shower's radiant

// Major showers: the sun's longitude at the peak, the radiant's J2000 RA / Dec (deg).
const F32 kShowers[][3] = {
    { 283.2f, 230.f,  49.f },   // Quadrantids
    {  32.3f, 271.f,  34.f },   // Lyrids
    {  45.5f, 338.f,  -1.f },   // Eta Aquariids
    { 140.0f,  48.f,  58.f },   // Perseids
    { 208.0f,  95.f,  16.f },   // Orionids
    { 235.3f, 152.f,  22.f },   // Leonids
    { 262.2f, 112.f,  33.f },   // Geminids
};

// World (x east, y north, z up) -> J2000 equatorial and back: gmdx_night_sky.glsl nsWorldToSky,
// e = z (d.pole) + b2 (d.a2) + (z x b2) (d.(pole x a2)).
struct SkyFrame
{
    LLVector3 pole, a2, c2;   // world
    LLVector3 b2, w2;         // sky (z is the pole)
    SkyFrame(const LLVector3& sun, F32 lat, F32 lam)
    {
        const F32 obliquity = 0.409093f;
        pole.set(0.f, cosf(lat), sinf(lat));
        a2 = sun - pole * (sun * pole);
        if (a2.lengthSquared() > 1e-8f) a2.normalize(); else a2.set(1.f, 0.f, 0.f);
        c2 = pole % a2;
        b2.set(cosf(lam), sinf(lam) * cosf(obliquity), 0.f);
        b2.normalize();
        w2.set(-b2.mV[1], b2.mV[0], 0.f);
    }
    LLVector3 toWorld(const LLVector3& e) const { return pole * e.mV[2] + a2 * (e * b2) + c2 * (e * w2); }
};

// A V magnitude as the sky pass's star light (gmdx_night_sky.glsl nsStars: px^2 x linear).
F32 starLight(F32 scale, F32 mag)
{
    return scale * 13.3f * powf(10.f, -0.4f * mag);
}

// ---- satellites ----
struct Satellite
{
    F64 t0;              // world clock at spawn (s)
    LLVector3 s0, u;     // d(a) = s0 cos a + u sin a: s0 on the horizon, u its top (u.z > 0)
    F32 a0, rate, mag;   // start angle along the path, rad / s, V magnitude
};
constexpr int kMaxSatellites = 6;          // space[10..15]
constexpr F32 kSatelliteEvery = 40.f;      // s between new ones, on average (GMDXSatellites 1)
constexpr F32 kSatelliteKm = 550.f, kEarthKm = 6371.f;
std::vector<Satellite> sSats;
F64 sSatLast = -1.0;

// How much of the sun reaches a satellite seen in world direction d: 0 inside the Earth's
// shadow (a cylinder, no penumbra worth drawing at this size), faded over ~40 km at its edge.
F32 satelliteLit(const LLVector3& d, const LLVector3& sun)
{
    const F32 R = kEarthKm, b = d.mV[VZ] * R;
    const F32 t = -b + sqrtf(b * b + (R + kSatelliteKm) * (R + kSatelliteKm) - R * R);
    const LLVector3 p = d * t + LLVector3(0.f, 0.f, R);   // from the Earth's centre
    const F32 along = p * sun;
    if (along >= 0.f) return 1.f;
    return llclamp(((p - sun * along).length() - (R - 20.f)) / 40.f, 0.f, 1.f);
}

LLVector3 randomUnit()
{
    const F32 z = 2.f * ll_frand() - 1.f, phi = F_TWO_PI * ll_frand();
    const F32 r = sqrtf(llmax(0.f, 1.f - z * z));
    return LLVector3(r * cosf(phi), r * sinf(phi), z);
}

// space[10..15] = world direction + light; spawns and retires them.
void fillSatellites(float space[][4], F32 scale, F32 how_many, const LLVector3& sun)
{
    const F64 now = LLWorldClock::getTime();
    const F32 dt = sSatLast < 0.0 ? 0.f : (F32)llclamp(now - sSatLast, 0.0, 1.0);
    sSatLast = now;
    sSats.erase(std::remove_if(sSats.begin(), sSats.end(), [&](const Satellite& s)
                { return now < s.t0 || s.a0 + s.rate * (F32)(now - s.t0) > F_PI; }), sSats.end());
    // New ones while the sun is below the horizon but not so far down that all of them are dark.
    if (how_many > 0.f && sun.mV[VZ] < 0.f && sun.mV[VZ] > -0.57f && (int)sSats.size() < kMaxSatellites
        && ll_frand() < dt * how_many / kSatelliteEvery)
    {
        const LLVector3 k = randomUnit();
        LLVector3 s0 = k % LLVector3::z_axis;
        if (s0.lengthSquared() > 1e-4f)
        {
            s0.normalize();
            LLVector3 u = k % s0;
            if (u.mV[VZ] < 0.f) { s0 = -s0; u = -u; }
            if (u.mV[VZ] > 0.25f)   // a pass that climbs at least ~15 deg
            {
                const F32 bright = ll_frand() < 0.15f ? 1.f + 1.5f * ll_frand() : 3.5f + 1.5f * ll_frand();
                sSats.push_back({ now, s0, u, 0.6f * F_PI * ll_frand(), (0.5f + 0.4f * ll_frand()) * DEG_TO_RAD, bright });
            }
        }
    }
    for (size_t i = 0; i < sSats.size(); ++i)
    {
        const Satellite& s = sSats[i];
        const F32 age = (F32)(now - s.t0), a = s.a0 + s.rate * age;
        LLVector3 d = s.s0 * cosf(a) + s.u * sinf(a);
        d.normalize();
        const F32 fade = llmin(age / 3.f, 1.f);   // (one that starts mid-sky fades in)
        space[10 + i][0] = d.mV[0]; space[10 + i][1] = d.mV[1]; space[10 + i][2] = d.mV[2];
        space[10 + i][3] = starLight(scale, s.mag) * fade * satelliteLit(d, sun);
    }
}

LLVector3 raDec(F32 ra_deg, F32 dec_deg)
{
    const F32 ra = ra_deg * DEG_TO_RAD, dec = dec_deg * DEG_TO_RAD;
    return LLVector3(cosf(dec) * cosf(ra), cosf(dec) * sinf(ra), sinf(dec));
}

// A texture from pixels made here (no asset behind it), as vkSynthInjectCube makes its checker:
// setFromRaw keeps the pixels as the Vulkan upload's source for good (#707 local lease).
LLUUID makeTexture(LLImageRaw* raw)
{
    LLViewerFetchedTexture* tex = LLViewerTextureManager::getFetchedTexture(raw, FTT_LOCAL_FILE, /*usemipmaps*/false);
    if (!tex) return LLUUID::null;
    tex->setFromRaw(0, raw);
    held().push_back(tex);
    return tex->getID();
}

void makeStarGrid()
{
    sStarTried = true;
    const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "gmdx_bsc5.bin");
    llifstream in(path.c_str(), std::ios::binary);
    const std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<GMDXStarGrid::Star> stars;
    if (!GMDXStarGrid::parse(file, stars))
    {
        LL_WARNS("GMDXNightSky") << path << " missing or not a star catalogue; Second Life's stars instead" << LL_ENDL;
        return;
    }
    uint32_t dropped = 0;
    const std::vector<uint8_t> grid = GMDXStarGrid::build(stars, &dropped);
    LLPointer<LLImageRaw> raw = new LLImageRaw(GMDXStarGrid::W, GMDXStarGrid::H, 4);
    if (!raw->getData()) return;
    memcpy(raw->getData(), grid.data(), grid.size());
    sStarGrid = makeTexture(raw);
    LL_INFOS("GMDXNightSky") << stars.size() << " catalogue stars (" << dropped
                             << " faint listings left out of crowded cells)" << LL_ENDL;
}

void makeMilkyWay()
{
    sMilkyWayTried = true;
    const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "gmdx_milkyway.jpg");
    LLPointer<LLImageJPEG> jpeg = new LLImageJPEG();
    LLPointer<LLImageRaw> raw = new LLImageRaw();
    if (!jpeg->load(path) || !jpeg->decode(raw, 0.f) || raw->getWidth() != 2048 || raw->getHeight() != 1024)
    {
        LL_WARNS("GMDXNightSky") << path << " missing or not the 2048 x 1024 panorama; no Milky Way" << LL_ENDL;
        return;
    }
    sMilkyWay = makeTexture(raw);
}

// The share of a moonless night's contrast left by twilight and moonlight (1 = dark night).
F32 darkSkyContrast(const LLVector3& sun, const LLVector3& moon, F32 moon_brightness)
{
    // Twilight sky over the dark night sky (~6e-4 lux), as log10(1 + ratio) by sun elevation:
    // astronomical twilight ends at -18 deg; 0.008 lux at -12, 3.4 at -6, ~400 at sunset, and
    // daylight on up to ~1e4 cd/m^2 (the sun 20 deg up and over).
    static const F32 kEl[] = {-18.f, -15.f, -12.f, -9.f, -6.f, -3.f, 0.f};
    static const F32 kLg[] = {  0.f,  0.4f,  1.1f, 2.1f, 3.6f, 4.6f, 5.7f};
    const F32 el = asinf(llclamp(sun.mV[VZ], -1.f, 1.f)) * RAD_TO_DEG;
    F32 lg = el <= kEl[0] ? 0.f : kLg[6] + llmin(el, 20.f) * 0.1f;
    for (int i = 0; i < 6; ++i)
        if (el > kEl[i] && el <= kEl[i + 1])
            lg = lerp(kLg[i], kLg[i + 1], (el - kEl[i]) / (kEl[i + 1] - kEl[i]));
    const F32 twilight = powf(10.f, lg) - 1.f;

    // Moonlit sky: a full moon high up brightens the sky ~40x (about 18 vs 22 mag/arcsec^2).
    // Phase from the sun-moon angle (EEP moves them independently); Krisciunas & Schaefer 1991:
    // I(a) / I(0) = 10^(-0.4 (0.026 |a| + 4e-9 a^4)), a = phase angle in degrees. EEP's
    // moon_brightness (default 0.5) scales it as it scales the moon disc, 0.5 = 1.
    const F32 a = 180.f - acosf(llclamp(sun * moon, -1.f, 1.f)) * RAD_TO_DEG;
    const F32 phase = powf(10.f, -0.4f * (0.026f * a + 4e-9f * a * a * a * a));
    const F32 t = llclamp((moon.mV[VZ] + 0.02f) / 0.27f, 0.f, 1.f);   // up from just below the horizon
    const F32 up = t * t * (3.f - 2.f * t);
    const F32 moonlit = 40.f * phase * llclamp(2.f * moon_brightness, 0.f, 2.f) * up;
    return 1.f / (1.f + twilight + moonlit);
}

// ---- planets ----
struct Planet
{
    F64 a, e, i, L, peri, node;          // J2000: AU, -, deg, deg (mean longitude), deg, deg
    F64 da, de, di, dL, dperi, dnode;    // per Julian century
    F32 H, phase_k;                      // V at 1 AU from the sun and the Earth; mag per degree of phase
    F32 rgb[3];                          // colour (sRGB-ish, by eye)
};
const Planet kPlanets[] = {
    { 0.38709927, 0.20563593, 7.00497902, 252.25032350,  77.45779628,  48.33076593,
      0.00000037, 0.00001906, -0.00594749, 149472.67411175, 0.16047689, -0.12534081, -0.6f, 0.030f, {1.00f, 0.93f, 0.86f} },  // Mercury
    { 0.72333566, 0.00677672, 3.39467605, 181.97909950, 131.60246718,  76.67984255,
      0.00000390, -0.00004107, -0.00078890, 58517.81538729, 0.00268329, -0.27769418, -4.4f, 0.013f, {1.00f, 0.98f, 0.90f} },  // Venus
    { 1.52371034, 0.09339410, 1.84969142,  -4.55343205, -23.94362959,  49.55953891,
      0.00001847, 0.00007882, -0.00813131, 19140.30268499, 0.44441088, -0.29257343, -1.5f, 0.016f, {1.00f, 0.62f, 0.42f} },  // Mars
    { 5.20288700, 0.04838624, 1.30439695,  34.39644051,  14.72847983, 100.47390909,
     -0.00011607, -0.00013253, -0.00183714, 3034.74612775, 0.21252668, 0.20469106, -9.4f, 0.005f, {1.00f, 0.94f, 0.83f} },    // Jupiter
    { 9.53667594, 0.05386179, 2.48599187,  49.95424423,  92.59887831, 113.66242448,
     -0.00125060, -0.00050991, 0.00193609, 1222.49362201, -0.41897216, -0.28867794, -9.2f, 0.004f, {1.00f, 0.90f, 0.70f} },   // Saturn (rings: ~0.3 brighter)
};
constexpr int kNumPlanets = 5;

// Heliocentric ecliptic (J2000) position in AU, T Julian centuries from J2000.
void planetPosition(const Planet& p, F64 T, F64 out[3])
{
    const F64 a = p.a + p.da * T, e = p.e + p.de * T;
    const F64 i = (p.i + p.di * T) * DEG_TO_RAD, L = p.L + p.dL * T;
    const F64 peri = p.peri + p.dperi * T, node = (p.node + p.dnode * T) * DEG_TO_RAD;
    const F64 w = peri * DEG_TO_RAD - node;
    const F64 M = fmod(L - peri, 360.0) * DEG_TO_RAD;
    F64 E = M + e * sin(M);
    for (int k = 0; k < 5; ++k) E -= (E - e * sin(E) - M) / (1.0 - e * cos(E));   // Kepler, Newton
    const F64 x = a * (cos(E) - e), y = a * sqrt(1.0 - e * e) * sin(E);
    const F64 cw = cos(w), sw = sin(w), cn = cos(node), sn = sin(node), ci = cos(i), si = sin(i);
    out[0] = (cw * cn - sw * sn * ci) * x + (-sw * cn - cw * sn * ci) * y;
    out[1] = (cw * sn + sw * cn * ci) * x + (-sw * sn + cw * cn * ci) * y;
    out[2] = (sw * si) * x + (cw * si) * y;
}

// space[0..4] = J2000 equatorial direction + light, [5..9] = colour (luminance 1).
void fillPlanets(float space[][4], F32 scale, F32 season_rad)
{
    const F64 jd = LLDate::now().secondsSinceEpoch() / 86400.0 + 2440587.5;
    const F64 T = (jd - 2451545.0) / 36525.0;
    // The Earth opposite the season's sun, 1 AU out.
    const F64 earth[3] = { -cos((F64)season_rad), -sin((F64)season_rad), 0.0 };
    const F64 obliquity = 23.43928 * DEG_TO_RAD;
    for (int n = 0; n < kNumPlanets; ++n)
    {
        const Planet& p = kPlanets[n];
        F64 h[3];
        planetPosition(p, T, h);
        const F64 g[3] = { h[0] - earth[0], h[1] - earth[1], h[2] - earth[2] };
        const F64 delta = sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
        const F64 r = sqrt(h[0] * h[0] + h[1] * h[1] + h[2] * h[2]);
        // Phase angle (sun - planet - Earth), the Earth 1 AU from the sun.
        const F64 cos_a = llclamp((r * r + delta * delta - 1.0) / (2.0 * r * delta), -1.0, 1.0);
        const F32 phase = (F32)(acos(cos_a) * RAD_TO_DEG);
        const F32 mag = p.H + 5.f * (F32)log10(r * delta) + p.phase_k * llmin(phase, 150.f);
        // Ecliptic -> equatorial (J2000).
        space[n][0] = (F32)(g[0] / delta);
        space[n][1] = (F32)((g[1] * cos(obliquity) - g[2] * sin(obliquity)) / delta);
        space[n][2] = (F32)((g[1] * sin(obliquity) + g[2] * cos(obliquity)) / delta);
        space[n][3] = starLight(scale, mag);
        const F32 lum = 0.2126f * p.rgb[0] + 0.7152f * p.rgb[1] + 0.0722f * p.rgb[2];
        for (int c = 0; c < 3; ++c) space[5 + n][c] = p.rgb[c] / lum;
    }
}
} // namespace

LLUUID GMDXNightSky::textureId(int which)
{
    static LLCachedControl<bool> s_on(gSavedSettings, "GMDXPhysicalNight", true);
    if (!s_on) return LLUUID::null;
    if (which == 0)
    {
        if (!sStarTried) makeStarGrid();
        return sStarGrid;
    }
    if (!sMilkyWayTried) makeMilkyWay();
    return sMilkyWay;
}

const LLUUID& GMDXNightSky::starGridId()
{
    return sStarGrid;
}

void GMDXNightSky::fillFrame(float b[10][4], float fx2[4], float space[kSpaceSlots][4])
{
    static LLCachedControl<bool> s_on(gSavedSettings, "GMDXPhysicalNight", true);
    sMeteorRate = -1.f;
    if (!s_on) return;
    LLSettingsSky::ptr_t sky = LLEnvironment::instance().getCurrentSky();
    if (!sky) return;
    static LLCachedControl<F32> s_stars(gSavedSettings, "GMDXStarBrightness", 1.f);
    static LLCachedControl<F32> s_lat(gSavedSettings, "GMDXStarLatitude", 35.f);
    static LLCachedControl<F32> s_season(gSavedSettings, "GMDXNightSkySunLongitude", 90.f);
    static LLCachedControl<F32> s_twinkle(gSavedSettings, "GMDXStarScintillation", 1.f);
    static LLCachedControl<F32> s_milky_way(gSavedSettings, "GMDXMilkyWay", 1.f);
    static LLCachedControl<F32> s_airglow(gSavedSettings, "GMDXAirglow", 1.f);
    static LLCachedControl<F32> s_zodiacal(gSavedSettings, "GMDXZodiacalLight", 1.f);

    static LLCachedControl<bool> s_fantasy(gSavedSettings, "GMDXFantasySky", false);
    const bool fantasy = s_fantasy;
    F32 artist = llclamp(sky->getStarBrightness() / 250.f, 0.f, 2.f);
    F32 contrast = darkSkyContrast(sky->getSunDirection(), sky->getMoonDirection(), sky->getMoonBrightness());
    // <GMDX> SPACE AT ALTITUDE (gmdxspacealt.h): above the air the stars, Milky Way and glow show by day
    // (a day sky's star_brightness is often 0: at least the default 1 up there).
    if (const F32 space = gmdxSpaceAmount(); space > 0.f)
    {
        artist = lerp(artist, llmax(artist, 1.f), space);
        contrast = lerp(contrast, 1.f, space);
    }
    // bodies[7].w > 0 selects the catalogue (0: LL's random stars, the fallback without the
    // data); 1e-12 keeps it selected with nothing to draw (washed out by day, or turned down to
    // none: below 1e-3 even Sirius peaks under 0.02, unseen in any sky bright enough for that).
    F32 stars = llclamp((F32)s_stars, 0.f, 10.f) * artist * sqrtf(contrast) * (fantasy ? kFantasyStars : 1.f);
    if (stars < 1e-3f) stars = 1e-12f;
    b[7][3] = textureId(0).notNull() ? stars : 0.f;
    b[2][3] = llclamp((F32)s_lat, -90.f, 90.f) * DEG_TO_RAD;
    fx2[1] = (F32)s_season * DEG_TO_RAD;
    fx2[2] = (F32)fmod(LLWorldClock::getTime(), 3600.0);
    fx2[3] = llclamp((F32)s_twinkle, 0.f, 3.f);

    // Airglow, zodiacal light, Milky Way; none at all (no cost) while twilight or day leaves them
    // under 1/1000 of their dark-sky contrast (the sun above about -8 deg).
    const F32 glow = contrast < 1e-3f ? 0.f : kAirglow * artist * contrast;
    b[5][3] = glow * llclamp((F32)s_airglow, 0.f, 10.f);
    b[4][3] = glow * llclamp((F32)s_zodiacal, 0.f, 10.f) * (fantasy ? kFantasyZodiacal : 1.f);
    b[6][3] = textureId(1).notNull() ? glow * 3.f * llclamp((F32)s_milky_way, 0.f, 10.f) * (fantasy ? kFantasyMilkyWay : 1.f) : 0.f;

    // The space objects and options (gmdx_night_sky.glsl NS_SPACE layout).
    static LLCachedControl<F32> s_planets(gSavedSettings, "GMDXPlanets", 1.f);
    static LLCachedControl<F32> s_limit(gSavedSettings, "GMDXStarLimitMagnitude", 8.f);
    static LLCachedControl<F32> s_size(gSavedSettings, "GMDXStarSize", 1.f);
    static LLCachedControl<F32> s_sat(gSavedSettings, "GMDXStarSaturation", 1.f);
    static LLCachedControl<F32> s_deep(gSavedSettings, "GMDXDeepSky", 1.f);
    static LLCachedControl<F32> s_mw_turn(gSavedSettings, "GMDXMilkyWayRotation", 0.f);
    static LLCachedControl<F32> s_mw_tilt(gSavedSettings, "GMDXMilkyWayTilt", 0.f);
    space[16][0] = s_limit >= 7.99f ? 99.f : llclamp((F32)s_limit, 1.f, 8.f);   // (8: the whole catalogue)
    space[16][1] = llclamp((F32)s_size, 0.5f, 4.f) * (fantasy ? kFantasyStarSize : 1.f);
    space[16][2] = llclamp((F32)s_sat, 0.f, 3.f) * (fantasy ? kFantasySaturation : 1.f);
    space[16][3] = glow * llclamp((F32)s_deep, 0.f, 10.f) * (fantasy ? kFantasyDeepSky : 1.f);   // (Andromeda, Orion nebula, ...)
    space[17][0] = llclamp((F32)s_mw_turn, -360.f, 360.f) * DEG_TO_RAD;
    space[17][1] = llclamp((F32)s_mw_tilt, -90.f, 90.f) * DEG_TO_RAD;
    space[17][2] = fantasy ? kFantasyMilkyWayExp : 1.f;
    const F32 planets = llclamp((F32)s_planets, 0.f, 10.f) * (fantasy ? kFantasyPlanets : 1.f);
    if (b[7][3] > 0.f && planets > 0.f) fillPlanets(space, stars * planets, fx2[1]);

    static LLCachedControl<F32> s_satellites(gSavedSettings, "GMDXSatellites", 1.f);
    if (b[7][3] > 0.f) fillSatellites(space, stars, llclamp((F32)s_satellites, 0.f, 10.f), sky->getSunDirection());

    // Meteors: how often, and the season's shower.
    static LLCachedControl<F32> s_meteors(gSavedSettings, "GMDXMeteors", 1.f);
    static LLCachedControl<bool> s_shower(gSavedSettings, "GMDXMeteorShower", false);
    sMeteorRate = kNightMeteors * llclamp((F32)s_meteors, 0.f, 10.f) * sqrtf(contrast) * (fantasy ? kFantasyMeteors : 1.f);
    sShowerShare = 0.f;
    if (s_shower && sMeteorRate > 0.f)
    {
        const F32 season = (F32)fmod(fmod((F64)s_season, 360.0) + 360.0, 360.0);
        int best = 0;
        F32 best_d = 999.f;
        for (int i = 0; i < (int)LL_ARRAY_SIZE(kShowers); ++i)
        {
            const F32 d = 180.f - fabsf(fabsf(season - kShowers[i][0]) - 180.f);
            if (d < best_d) { best_d = d; best = i; }
        }
        const SkyFrame frame(sky->getSunDirection(), b[2][3], fx2[1]);
        sRadiant = frame.toWorld(raDec(kShowers[best][1], kShowers[best][2]));
        const F32 extra = kShowerMeteors * llmax(sRadiant.mV[VZ], 0.1f);
        sShowerShare = extra / (1.f + extra);
        sMeteorRate *= 1.f + extra;
    }
}

float GMDXNightSky::meteorRateScale()
{
    return sMeteorRate;
}

bool GMDXNightSky::meteorShowerPath(const LLVector3& start, LLVector3& travel)
{
    if (sMeteorRate < 0.f || ll_frand() >= sShowerShare) return false;
    // Away from the radiant along the great circle through it and the start.
    LLVector3 t = start * (start * sRadiant) - sRadiant;
    if (t.lengthSquared() < 1e-6f) return false;
    t.normalize();
    travel = t;
    return true;
}
