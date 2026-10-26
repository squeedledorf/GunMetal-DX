// vkclean — WEATHER PRECIPITATION MEDIUM (#954; vkgfx/llcleanweathervk.cpp): the precipitation's
// own extinction (rain / snow / blowing snow: SLWeather::State::precipExt), as a PARTICIPATING
// MEDIUM only where it falls -- never under cover.
//
// VOLUMETRIC FOG ON (ssr.weather[16], RenderVKVolumetricFog): clean_vol_inject.comp adds it per
// froxel (weatherPrecipMedium): density = the rate's extinction x the roof map's openness at the
// froxel's world position (a room's air is clear, wherever the camera stands) x a 3D value-noise
// field ADVECTED with the wind and the fall speed (rain sheets, snow swirls, gusts: moving density,
// world-anchored and temporally filtered by the fog's own pass); the blowing snow's part hugs the
// first surface under it. The fog's lighting (sun / moon shadows, ambient, local lights) lights it;
// every fog consumer (opaque apply, alpha faces, water, particles) gets it through volFogApply.
// Past the grid's far plane clean_vol_apply.glsl adds it analytically (open, no noise).
//
// VOLUMETRIC FOG OFF (ssr.weather[15]): a noise-free analytic veil, C' = C T + L (1 - T),
// T = exp(-sigma d) (d below the precipitation's ceiling), applied ONLY to surfaces open to the
// sky at their own position (weatherPrecipOpen): a surface under cover is never veiled (a room
// seen from a doorway stays clear; the outdoors behind a window is veiled). Opaque + sky:
// clean_weather_veil.frag (one fullscreen draw at the start of pass 2); alpha faces / water /
// "over" particles: weatherVeilApply. (A covered path to an open surface -- an outdoor wall seen
// through a long hall -- is veiled over its whole length: the fallback's simplification.)
//
// ssr.weather[15]: rgb the fallback's in-scatter colour, w its extinction (1/m; 0 = no fallback,
//                  e.g. the fog carries the medium). ssr.weather[16]: x the falling precipitation's
//                  extinction (1/m), y the blowing snow's (near the ground), z the snow's share of
//                  it (shape + albedo), w the motion noise octaves (0 none .. 2) -- all 0 with the
//                  fog off.
// Probe / mirror captures never apply either (main view only).
//
// Requires clean_frame_info.glsl (ssr; draw set 0 binding 49 -- the weather's roof maps).
#ifndef CLEAN_WEATHER_VEIL_GLSL
#define CLEAN_WEATHER_VEIL_GLSL

#ifndef CLEAN_WEATHER_SURFACE_GLSL
// (the same declaration as clean_weather_surface.glsl: draw set 0 binding 49)
layout(set = 0, binding = 49) uniform sampler2D uWeatherOccl[10];
#endif

// The precipitation's ceiling above the camera (m; llcleanweathervk.cpp kVeilCeiling).
const float kWxVeilCeiling = 1500.0;
const float kVeilIndoor = 10.0;   // <GMDX> metres a covered surface keeps clear of the veil (its own room)

// One cascade's openness at P (1 open .. 0 under its top surface; outside the map: -1).
float weatherPrecipOpenMap(uint m, vec4 fr, vec4 fz, vec2 s, vec3 P)
{
    const vec2 uv = (P.xy - (fz.x - P.z) * s - fr.xy) * fr.z;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return -1.0;
    float d;
    if (m == 0u) d = textureLod(uWeatherOccl[0], uv, 0.0).r;
    else if (m == 1u) d = textureLod(uWeatherOccl[1], uv, 0.0).r;
    else if (m == 2u) d = textureLod(uWeatherOccl[2], uv, 0.0).r;
    else d = textureLod(uWeatherOccl[3], uv, 0.0).r;
    // <GMDX> the clear value: nothing drawn there, open sky. Read as a roof at the box bottom, it
    // covered every receiver below the box -- the whole cascade once the camera is high up.
    if (d >= 0.9999) return 1.0;
    const float top = fz.x - d * fz.y;
    return clamp((P.z - (top - 1.5 * fz.z)) / max(fz.z, 1e-3), 0.0, 1.0);
}

// Open sky over world point P: the near roof map where it covers P, else the coarse one; no map /
// outside both: open. One tap (the medium is soft; the surfaces' own wetness test is the PCF one).
float weatherPrecipOpen(vec3 P)
{
    if (ssr.weather[3].w > 0.5)
    {
        const float o = weatherPrecipOpenMap(1u + uint(ssr.weather[3].w + 0.5), ssr.weather[3], ssr.weather[4], ssr.weather[7].xy, P);
        if (o >= 0.0) return o;
    }
    if (ssr.weather[1].w > 0.5)
    {
        const float o = weatherPrecipOpenMap(uint(ssr.weather[1].w + 0.5) - 1u, ssr.weather[1], ssr.weather[2], ssr.weather[6].xy, P);
        if (o >= 0.0) return o;
    }
    return 1.0;
}

// The height of the first surface under P (the roof maps' top; -1e9: none known).
float weatherPrecipGround(vec3 P)
{
    if (ssr.weather[1].w > 0.5)
    {
        const vec4 fr = ssr.weather[1], fz = ssr.weather[2];
        const vec2 uv = (P.xy - (fz.x - P.z) * ssr.weather[6].xy - fr.xy) * fr.z;
        if (all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0))))
        {
            const float d = fr.w > 1.5 ? textureLod(uWeatherOccl[1], uv, 0.0).r : textureLod(uWeatherOccl[0], uv, 0.0).r;
            return d >= 1.0 ? -1e9 : fz.x - d * fz.y;
        }
    }
    return -1e9;
}

// ---- the FOG-ON medium (clean_vol_inject.comp) ----
float wxmHash(vec3 c)
{
    uvec3 u = uvec3(ivec3(c) + ivec3(1 << 20));
    uint x = u.x * 0x8da6b343u ^ u.y * 0xd8163841u ^ u.z * 0xcb1ab31fu;
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return float(x >> 8) * (1.0 / 16777216.0);
}
float wxmNoise(vec3 p)
{
    const vec3 i = floor(p);
    const vec3 f = p - i;
    const vec3 w = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(wxmHash(i), wxmHash(i + vec3(1, 0, 0)), w.x), mix(wxmHash(i + vec3(0, 1, 0)), wxmHash(i + vec3(1, 1, 0)), w.x), w.y),
               mix(mix(wxmHash(i + vec3(0, 0, 1)), wxmHash(i + vec3(1, 0, 1)), w.x), mix(wxmHash(i + vec3(0, 1, 1)), wxmHash(i + vec3(1, 1, 1)), w.x), w.y),
               w.z);
}

// The precipitation's medium at world point P (fog on): adds its scattering / extinction.
// wind: the gusty wind (m/s, world xy); fall: the precipitation's mean fall speed (m/s); t: time
// (s); motion: the noise's strength 0..1.
void weatherPrecipMedium(vec3 P, vec2 wind, float fall, float t, float motion, inout vec3 sigma_s, inout float sigma_t)
{
    const float sFall = ssr.weather[16].x, sBlow = ssr.weather[16].y;
    if (!(sFall + sBlow > 0.0)) return;
    if (P.z < ssr.vol_misc.z) return;   // (none below the water cut)
    const float open = weatherPrecipOpen(P);
    if (!(open > 0.0)) return;
    // MOTION: a value-noise field advected with the air (and, sheared, with the fall): rain makes
    // tall, narrow sheets falling fast, snow rounder swirls drifting with the wind. Mean ~1.
    float m = 1.0;
    const float oct = ssr.weather[16].w;
    if (oct > 0.5 && motion > 0.0)
    {
        const vec3 adv = vec3(wind, -fall) * t;
        // (rain: 1 / 14 m across, 1 / 40 m tall; snow: 1 / 9 m, slightly flattened)
        const vec3 freq = mix(vec3(1.0 / 14.0, 1.0 / 14.0, 1.0 / 40.0), vec3(1.0 / 9.0, 1.0 / 9.0, 1.0 / 7.0), ssr.weather[16].z);
        const vec3 q = (P - adv) * freq;
        float n = wxmNoise(q);
        if (oct > 1.5) n = 0.65 * n + 0.35 * wxmNoise(q * 2.7 + vec3(5.3, 1.7, 9.1) - vec3(wind * 0.05, 0.0) * t);
        m = max(1.0 + 1.6 * motion * (n - 0.5), 0.1);
    }
    float d = sFall * m;
    if (sBlow > 0.0)
    {
        // Blowing snow: thick near the first surface under the point, thinning over ~2 m.
        const float g = weatherPrecipGround(P);
        const float h = g > -1e8 ? max(P.z - g, 0.0) : 2.0;
        d += sBlow * 3.0 * exp(-h * 0.5) * m;
    }
    d *= open;
    sigma_t += d;
    // (water drops and ice barely absorb: a high albedo -- the fog's lighting sets how bright)
    sigma_s += d * mix(0.85, 0.95, ssr.weather[16].z);
}

// ---- the FOG-OFF fallback (the transparents; clean_weather_veil.frag for opaque + sky) ----
// The veil's transmittance to world point P (1 when P is under cover).
float weatherVeilT(vec3 P)
{
    const float sigma = ssr.weather[15].w;
    if (!(sigma > 0.0)) return 1.0;
    const bool covered = weatherPrecipOpen(P) < 0.5;   // <GMDX> (the opaque veil's rule, clean_weather_veil.frag)
    const vec3 rel = P - ssr.cam_origin.xyz;
    const float len = length(rel);
    float d = len;
    if (rel.z > 0.0) d = min(d, kWxVeilCeiling * len / rel.z);
    if (covered) d = max(d - kVeilIndoor, 0.0);   // <GMDX>
    return exp(-sigma * d);
}

vec3 weatherVeilApply(vec3 C, vec3 P)
{
    if (!(ssr.weather[15].w > 0.0)) return C;
    const float T = weatherVeilT(P);
    return C * T + ssr.weather[15].rgb * (1.0 - T);
}

// The inverse (the water's refraction: the scene behind the surface was veiled over the air
// segment in front of the surface too, up to the refraction's screen offset).
vec3 weatherVeilRemove(vec3 C, vec3 P)
{
    if (!(ssr.weather[15].w > 0.0)) return C;
    const float T = weatherVeilT(P);
    return max(C - ssr.weather[15].rgb * (1.0 - T), vec3(0.0)) / max(T, 1e-3);
}

#endif
