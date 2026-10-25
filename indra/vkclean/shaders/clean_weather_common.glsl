// vkclean — WEATHER PRECIPITATION (#783; vkgfx/llcleanweathervk.cpp): the shared block of the
// simulation compute (clean_weather_sim.comp) and the draw (clean_weather.vert / .frag).
//
// The particles live in a box around the camera (box.xy horizontal, box.z vertical half
// extents) and WRAP: a drop that leaves the box re-enters on the other side (one that fell
// out of the bottom re-enters at the top in a fresh column), so the volume never runs out
// and the camera can never outrun it. No per-particle state but the position: the type
// (rain / snow), the activity (density) and the speed variation are hashed from the
// particle index, so the CPU never touches a particle and an intensity change is a
// threshold move, not a re-allocation.
//
// Own descriptor set 0 (LLCleanWeatherVK): 0 the parameters (UBO, one per frame slot),
// 1 the particles, 2 the visible list, 3 the draw's indirect arguments, 4 the occlusion maps
// ([0] / [1] the coarse cascade, [2] / [3] the near one -- the coarse ones again where it
// does not exist; double-buffered, clean_weather_surface.glsl), 5 the listener-cover readback
// (host-visible, one per frame slot; clean_weather_sim.comp), M2: 6 the splash ring, 7 its
// head (clean_weather_sim.comp spawns, clean_weather.vert -DWEATHER_SPLASH draws), M4: 8 the
// run-off emitters (clean_weather_runoff.comp builds them; the spawn kernel and the stream draw
// read them). #901: 9 the per-avatar shelter atlas (two images; the coarse maps stand in while
// there is none), 10 its record (the splash ring stands in) -- wp.shelter.x > 0 only with them.
//
// M4 RUN-OFF DRIPS share the particle pool: while run-off runs, the last 2 x runoff2.x slots
// (from runoff2.y) are drips, two slots each -- [a] (origin xyz, birth on the eye.w clock; 0 =
// free), [b] (landing z, seed, 0, 0) -- and the rain takes the slots below them (box.z). A drip
// leaves an eave at rest and falls under gravity (to a ~7 m/s terminal speed), picking up the
// wind; it dies (and splashes) on its landing height. No per-drip CPU work: the spawn kernel
// (clean_weather_sim.comp -DWX_RUNOFF_SPAWN) writes them, the simulation moves / culls them.

// #954 PRECIPITATION RATE (slweather.h): the rates (fall), the drop / flake size spread and the
// snow's motion blur / swirl (shape2), BLOWING SNOW (fall.zw: a share of the slots, independent of
// the precipitation's, streams along the ground in a strong wind over lying snow -- wxIsDrift) and
// the precipitation's own extinction (clean_weather_veil.glsl: a medium in the volumetric fog, or
// the fog-off veil), so the particles only carry the near field -- each particle takes the
// analytic veil of its own distance (wxVeilT).

#ifndef CLEAN_WEATHER_COMMON_GLSL
#define CLEAN_WEATHER_COMMON_GLSL

// The weather's own set (0; #954 the veil binds draw set 0 first and this at 1).
#ifndef WX_OWN_SET
#define WX_OWN_SET 0
#endif

layout(std140, set = WX_OWN_SET, binding = 0) uniform WeatherParams
{
    mat4 view_proj;   // the main view (JITTERED, as the scene depth): (world - eye.xyz) -> clip
    vec4 eye;         // xyz camera (world), w time (s, bounded)
    vec4 box;         // x horizontal half extent (m), y vertical half extent (m), z the rain's slots (the pool,
                      // M4: less the drips' tail; 0 while none falls), w unused
    vec4 wind;        // xy wind velocity (m/s, world), z unused, w dt (s)
    vec4 amounts;     // x rain 0..1, y snow 0..1, z snow share snow / (rain + snow), w active share
    vec4 occl;        // xy occlusion map corner (world), z 1 / extent (per m), w map: 0 none, 1 / 2 = [0] / [1]
    vec4 occl2;       // x map top height (m), y top - bottom (m), z cover bias (m), w water height (m)
    vec4 sun;         // rgb the world draws' sun colour, w sun elevation weight 0..1
    vec4 amb;         // rgb the world draws' ambient colour
    vec4 screen;      // render width, height (px), 1 / width, 1 / height
    vec4 shape;       // x rain streak exposure (s), y rain drop width (m), z snow flake size (m), w rain opacity
    vec4 misc;        // x frame counter, y 1 = reset every particle, z snow opacity, w #954 1 = reverse-Z depth (the veil)
    vec4 ear;         // xyz the audio listener (world), w its cover-tap radius (m; 0 = no report)
    vec4 tilt;        // M2: xy the coarse map's shear s (a point P lies on the map column
                      // P.xy - (top - P.z) s, clean_weather_surface.glsl), zw the near map's
    vec4 splash;      // M2 splashes: x 1 = on, y ring size, z lifetime (s), w spawn radius (m)
    vec4 occlN;       // M2 the near map (splash heights): xy corner, z 1 / extent, w which (0 none, 1 / 2 = uOccl[2] / [3])
    vec4 occlN2;      // x its top height (m), y top - bottom (m), z cover bias (m), w unused
    // M4 RUN-OFF (llcleanweathervk_runoff.cpp):
    vec4 runoff;      // x strength S 0..1 (0 = none), y drips / s per m2 of catchment at S 1,
                      // z stream threshold (catchment m2 x S), w the drip budget (drips / s, all emitters)
    vec4 runoff2;     // x drip slots (drips; 0 = none), y the first drip slot (particle index),
                      // z 1 = streams drawn, w the emitter list's capacity
    // #901 PER-AVATAR SHELTER MAPS (clean_weather_shelter.glsl; binding 9 the atlas, 10 the record):
    vec4 shelter;     // x the layers in use (0: none -- nothing reads 9 / 10), y the atlas image
                      // written this recording (0 / 1), zw unused
    // #954 PRECIPITATION RATE (llcleanweathervk.cpp prepare):
    vec4 fall;        // x rain mm/h, y snow cm/h (falling now), z blowing snow's share of the slots
                      // (0 none), w its layer's height scale (m)
    vec4 shape2;      // x rain drop diameter scale (mm: the exponential tail's mean), y snow flake size
                      // factor (x shape.z), z snow motion-blur exposure (s), w snow swirl (x the wind)
    vec4 veil;        // rgb the precipitation's in-scatter colour (the world draws' light), w its
                      // extinction (1/m; 0 = none): the particles' own veil
    vec4 veil2;       // x the precipitation's ceiling above the camera (m), yz unused, w the snow
                      // share of the fall
    mat4 inv_view_proj;   // the inverse of view_proj (the veil's camera-relative points)
} wp;

#ifdef WEATHER_VS
#define WX_BUFFER readonly buffer
#else
#define WX_BUFFER buffer
#endif
layout(std430, set = WX_OWN_SET, binding = 1) WX_BUFFER WxParticles { vec4 particles[]; };   // xyz world position, w unused
layout(std430, set = WX_OWN_SET, binding = 2) WX_BUFFER WxVisible { uint visible[]; };      // this frame's drawn particle indices
// M2 SPLASHES: a fixed ring of recent rain hits (xyz the hit on the surface, w birth time on
// the eye.w clock; 0 = never used), written round-robin by the simulation.
layout(std430, set = WX_OWN_SET, binding = 6) WX_BUFFER WxSplashes { vec4 splashes[]; };
// M4 RUN-OFF EMITTERS (clean_weather_runoff.comp): a 64 B header -- [0..2] the spawn kernel's
// dispatch, [3] the appends (may pass the capacity: clamp), [4..7] the stream draw's
// VkDrawIndirectCommand, [8] the outlet texels counted, [9] the flow texel's area (float bits)
// -- then two vec4 per emitter: [2e] (origin xyz on the edge, catchment m2), [2e + 1] (landing
// z, outward step xy (m, one flow texel), per-texel flow at the edge (m)).
layout(std430, set = WX_OWN_SET, binding = 8) WX_BUFFER WxEmitters { uint em_head[16]; vec4 emitters[]; };

uint wxHash(uint x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
// Uniform [0, 1) per (particle, salt).
float wxRand(uint i, uint salt) { return float(wxHash(i * 0x9E3779B9u + salt * 0x85EBCA6Bu) >> 8) * (1.0 / 16777216.0); }

// #954 BLOWING SNOW: the blowing share of the indices (independent of the precipitation's: it
// lifts off lying snow whether or not snow falls); such a slot is never a falling drop / flake.
bool wxIsDrift(uint i) { return wxRand(i, 59u) < wp.fall.z; }
// The particle's type: snow for the snow share of the indices (a rain -> snow transition
// turns the drops into flakes one by one); blowing snow is snow.
bool wxIsSnow(uint i) { return wxIsDrift(i) || wxRand(i, 11u) < wp.amounts.z; }
// Drawn at all: the active share (rain + snow, <= 1) of the indices (not the blowing ones).
bool wxActive(uint i) { return !wxIsDrift(i) && wxRand(i, 23u) < wp.amounts.w; }

// #954 DROP SIZE (mm): an exponential spread over the rate's scale (Marshall-Palmer-like: heavier
// rain, bigger drops), from 0.8 mm, capped at 6 mm (bigger drops break up).
float wxDropDiam(uint i)
{
    const float u = wxRand(i, 31u);
    return min(0.8 + wp.shape2.x * -log(1.0 - 0.995 * u), 6.0);
}
// The drop's fall speed (m/s): the measured terminal velocity of a drop of that size (Atlas et al.).
float wxDropSpeed(uint i) { return max(9.65 - 10.3 * exp(-0.6 * wxDropDiam(i)), 2.0); }
// #954 FLAKE SIZE factor (x shape.z): a spread over the rate's clumping (heavy calm snow: big
// aggregates; a blizzard's wind breaks them up -- the CPU's factor).
float wxFlakeSize(uint i) { return wp.shape2.y * (0.45 + 0.65 * -log(1.0 - 0.97 * wxRand(i, 53u))); }

// World velocity (m/s) at P. Rain: the drop's terminal velocity (#954: by its size), carried by
// the wind. Snow: ~1 m/s, carried by most of the wind, with a slow per-flake sway and (#954) the
// wind's eddies -- a swirl that grows with the wind (blizzard snow streams, curls and lifts).
vec3 wxVelocity(uint i, bool snow, vec3 P)
{
    if (!snow) return vec3(wp.wind.xy, -wxDropSpeed(i));
    const float ph = wxRand(i, 41u) * 6.2831853;
    const float f = 0.6 + 0.6 * wxRand(i, 43u);
    const float t = wp.eye.w;
    const vec2 sway = vec2(sin(t * f + ph), cos(t * f * 0.83 + ph * 1.7)) * 0.45;
    vec3 v = vec3(wp.wind.xy * 0.9 + sway, -(0.7 + 0.6 * wxRand(i, 47u)));
    if (wp.shape2.w > 0.0)
    {
        // Eddies a few metres across drifting with the wind: a smooth field of P and t (flakes
        // near each other move alike), scaled by the wind speed.
        const float w = length(wp.wind.xy) * wp.shape2.w;
        const vec3 q = P * 0.33 - vec3(wp.wind.xy * 0.33, 0.0) * t;
        v.x += w * sin(q.y * 1.3 + q.z * 0.7 + t * 0.9 + ph * 0.3);
        v.y += w * sin(q.x * 1.1 - q.z * 0.9 + t * 1.1 + ph * 0.3);
        v.z += 0.5 * w * sin(q.x * 0.8 + q.y * 0.9 + t * 1.3);
    }
    return v;
}

// #954 VEIL: the transmittance of the precipitation between the camera and a point `rel` (camera
// relative) -- the path below the precipitation's ceiling.
float wxVeilT(vec3 rel)
{
    if (!(wp.veil.w > 0.0)) return 1.0;
    const float len = length(rel);
    float d = len;
    if (rel.z > 0.0) d = min(d, wp.veil2.x * len / rel.z);
    return exp(-wp.veil.w * d);
}

// ---- M4 RUN-OFF DRIPS ----
const float kWxGravity = 9.81;
const float kWxDripTerminal = 7.0;    // m/s (a drip a few mm across)
const float kWxDripDrag = 0.35;       // s: how fast a drip takes up the wind
// Fall (m) and downward speed (m/s) after t seconds from rest, to the terminal speed.
float wxDripFall(float t)
{
    const float tc = kWxDripTerminal / kWxGravity;
    return t < tc ? 0.5 * kWxGravity * t * t : 0.5 * kWxGravity * tc * tc + kWxDripTerminal * (t - tc);
}
// A drip's position and velocity at age t (s): a = its first slot (origin, birth).
void wxDripAt(vec4 a, float t, out vec3 P, out vec3 V)
{
    const float e = exp(-t / kWxDripDrag);
    P = vec3(a.xy + wp.wind.xy * (t - kWxDripDrag * (1.0 - e)), a.z - wxDripFall(t));
    V = vec3(wp.wind.xy * (1.0 - e), -min(kWxGravity * t, kWxDripTerminal));
}
// The drips' landing height: the surface under the edge, or the water above it.
float wxDripLand(vec4 b) { return max(b.x, wp.occl2.w); }

#endif   // CLEAN_WEATHER_COMMON_GLSL
