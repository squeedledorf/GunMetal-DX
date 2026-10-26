// <GMDX> VOLUMETRIC CLOUDS: shape, light and march (replaces clean_vol_cloud_common.glsl for the
// march, panorama and shadow map; upstream's noise, EEP weather map, temporal pass and composite
// are used as they are -- same textures, same outputs, same ssr.vol_clouds contract).
//
// The cloud model and its lighting are ported from Wicked Engine
// (https://github.com/turanszkij/WickedEngine, volumetricCloudsHF.hlsli / volumetricCloud_renderCS.hlsl),
// MIT License, Copyright (c) 2026 Turanszki Janos:
//   Permission is hereby granted, free of charge, to any person obtaining a copy of this
//   software and associated documentation files (the "Software"), to deal in the Software
//   without restriction, including without limitation the rights to use, copy, modify, merge,
//   publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons
//   to whom the Software is furnished to do so, subject to the following conditions: The above
//   copyright notice and this permission notice shall be included in all copies or substantial
//   portions of the Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
//   EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
//   FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
//   HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
//   CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
//   USE OR OTHER DEALINGS IN THE SOFTWARE.
// (Wicked's noise and weather generators are NOT used: they come from an unlicensed project.
// The noises here are upstream's own, clean_vol_cloud_noise.comp, in the same channel layout.)
//
// Cloud bodies: CorvaeOboro's hand-sculpted cumulus volumes (CC0) placed among the procedural
// clouds, see gcBody.
//
// Model (Schneider, "Real-Time Volumetric Cloudscapes", GPU Pro 7 / Nubis 2017): Perlin-Worley
// base shape eroded by a Worley fBm, three cloud types (small / medium / large) blended by the
// weather map's type channel, each with its own height gradient and anvil deformation.
// Light (Hillaire, "Physically Based Sky, Atmosphere and Cloud Rendering in Frostbite", 2016):
// energy-conserving integration, two multiple-scattering octaves (Wrenninge), a dual-lobe
// Cornette-Shanks phase, a 5-sample self shadow, sky ambient, light bounced off the ground,
// beer-powder albedo. With the physical sky on (RenderVKPhysicalSky) the sun reaching each
// cloud sample is the physical sky's light through its transmittance LUT at that sample's own
// height and position, so a cloud at sunset is lit by the same orange the sky shows.
//
// Geometry: the shell between base and top follows the planet's curve (altitude = z +
// d^2 / 2R around the camera), so distant clouds sink toward the horizon. Positions are
// offset by the agent frame's global origin (vc.gmdx.xy, wrapped by 21 weather tiles: every
// noise and the anti-tiling term stay periodic) so the clouds stay put across region crossings.
//
// Requires clean_frame_info.glsl (ssr) + clean_atmos_common.glsl, and the module set at VC_SET.

#ifndef VC_SET
#define VC_SET 1
#endif

// The module's parameter block: upstream's VolCloudParams (clean_vol_cloud_ubo.glsl) plus the
// GunMetal-DX vec4s after it (LLCleanVolCloudsVK::Params::gmdx).
layout(std140, set = VC_SET, binding = 0) uniform VolCloudParams {
    vec4 shape0;   // x weather tile (m), y 1 / tile, z 1 / base noise period (m), w 1 / detail period (m)
    vec4 shape1;   // xy wind offset of the 3D noises (m), z extinction per m at density 1, w detail erosion
    vec4 shape2;   // x ambient scale, y max distance (m), z multi-scatter strength, w powder strength
    vec4 phase;    // (upstream's lobes; the Wicked phase below is fixed)
    vec4 march;    // x max steps (main view), y light steps, z frame index, w 1 = offscreen (snapshot)
    vec4 cam;      // xyz camera world position, w time (s)
    vec4 shadow;   // shadow map: xy centre (m), z extent (m), w steps
    vec4 pano;     // x first row, y rows this dispatch, z steps, w history blend (0 = overwrite)
    mat4 inv_vp;
    mat4 reproj;
    vec4 dims;
    vec4 pat;
    vec4 temporal;
    vec4 flash;
    vec4 gmdx;     // xy agent frame origin (global, m) wrapped by 21 weather tiles, zw the EEP scroll (uv)
    vec4 gmdx2;    // x sculpted cloud bodies loaded (0 = none), y their amount (GMDXCloudBodies)
} vc;

bool vcMarchedNow(ivec2 px)
{
    const int mode = int(vc.pat.x + 0.5);
    const uint phase = uint(vc.pat.y + 0.5);
    if (mode == 0) return true;
    if (mode == 1) return ((uint(px.x) + uint(px.y) + phase) & 1u) == 0u;
    const ivec2 bayer[4] = ivec2[4](ivec2(0, 0), ivec2(1, 1), ivec2(1, 0), ivec2(0, 1));
    return (px & 1) == bayer[phase & 3u];
}
layout(set = VC_SET, binding = 1) uniform sampler3D uVcBase;      // 128^3 Perlin-Worley (r) + Worley fBm (gba)
layout(set = VC_SET, binding = 2) uniform sampler3D uVcDetail;    // 32^3 Worley fBm (rgb)
layout(set = VC_SET, binding = 3) uniform sampler2D uVcWeather;   // coverage, type, local density
layout(set = VC_SET, binding = 13) uniform sampler3D uVcShapes;   // sculpted bodies (GMDXCloudShapesVK), 2 x 2 x 2 of 64^3

const float VC_FAR = 65000.0;   // fp16-safe "no cloud" distance key
const float GC_PI = 3.14159265;
const float GC_HALF_INV_R = 0.5 / 6360000.0;   // altitude = z + d^2 / 2R

// The physical sky lights the clouds (the panorama is the probes' WindLight sky: never).
bool gcPhys()
{
#ifdef PHYSKY_NOT_IN_PROBES
    return false;
#else
    return physkyOn();
#endif
}

float vcBase()  { return ssr.vol_clouds[0].y; }
float vcTop()   { return ssr.vol_clouds[0].z; }
float gcThick() { return max(vcTop() - vcBase(), 1.0); }

float gcRemap(float v, float lo, float hi, float nlo, float nhi)
{
    return nlo + (v - lo) / max(hi - lo, 1e-5) * (nhi - nlo);
}
float gcStretch(float v, float lo, float hi) { return clamp((v - lo) / (hi - lo), 0.0, 1.0); }

// Altitude of a world point over the curved ground (camera-centred).
float gcAlt(vec3 p)
{
    const vec2 d = p.xy - vc.cam.xy;
    return p.z + dot(d, d) * GC_HALF_INV_R;
}
float gcHeight(vec3 p) { return (gcAlt(p) - vcBase()) / gcThick(); }

// ---- cloud types (Wicked's first layer, as fractions of the shell) ----
// Gradients: 4 positions of a black, white, white, black ramp. Anvil: amount top, offset top,
// amount bottom, offset bottom.
const vec4 GC_GRAD_S = vec4(0.01, 0.10, 0.11, 0.20);
const vec4 GC_GRAD_M = vec4(0.01, 0.08, 0.30, 0.40);
const vec4 GC_GRAD_L = vec4(0.01, 0.06, 0.75, 0.95);
const vec4 GC_ANVIL_M = vec4(15.0, 0.10, 15.0, 0.10);
const vec4 GC_ANVIL_L = vec4(5.0, 0.25, 5.0, 0.15);

struct GcWeather { float cov; vec3 mask; vec4 grad; vec4 anvil; float dens; };

GcWeather gcWeather(vec3 p)
{
    const vec4 w = textureLod(uVcWeather, (p.xy + vc.gmdx.xy) * vc.shape0.y, 0.0);
    GcWeather g;
    g.cov = w.r;
    const float type = w.g;
    g.mask = vec3(1.0 - clamp(type * 2.0, 0.0, 1.0), 1.0 - abs(type - 0.5) * 2.0, clamp(type - 0.5, 0.0, 1.0) * 2.0);
    g.grad = GC_GRAD_S * g.mask.r + GC_GRAD_M * g.mask.g + GC_GRAD_L * g.mask.b;
    g.anvil = GC_ANVIL_M * g.mask.g + GC_ANVIL_L * g.mask.b;
    g.dens = w.b;
    return g;
}

float gcAnvil(float h, GcWeather w)
{
    // (amount 0 -- the small type -- gives pow(x, 0) = 1 twice: saturated to 1, no change)
    return clamp(pow(clamp(h + w.anvil.y + (1.0 - w.grad.a), 0.0, 1.0), w.anvil.x)
               + pow(clamp((1.0 - h) + w.anvil.w + w.grad.r, 0.0, 1.0), w.anvil.z), 0.0, 1.0);
}

// Wicked's bounding test: outside it no cloud can be, so the march takes big steps.
bool gcValid(float h, GcWeather w)
{
    return w.cov * gcAnvil(h, w) > 0.05 && w.grad.r < h && w.grad.a > h;
}

// ---- sculpted cloud bodies (CorvaeOboro's CC0 volumes, gmdxcloudshapesvk.h) ----
// The weather tile is cut into cells of about 3 km; a hash picks which cells hold a body, which
// shape, its turn, size and place in the cell. Cells live in the weather map's own uv (the EEP
// scroll included), so a body drifts with the coverage around it and wraps with the tile. A
// body stands on the shell's base, is cut by the shell's top, and only shows where the region's
// coverage is high; the procedural cloud and the body merge (max) before the detail erosion,
// so both wear the same edges and light the same way.
uvec3 gcHash3(uvec3 v)
{
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    return v;
}

float gcBody(vec3 p, float h, GcWeather w)
{
    const uint count = uint(vc.gmdx2.x + 0.5);
    if (count == 0u || vc.gmdx2.y <= 0.0 || w.cov < 0.3) return 0.0;
    const float cells = max(1.0, floor(vc.shape0.x / 3000.0));
    const float cellM = vc.shape0.x / cells;
    const vec2 g = ((p.xy + vc.gmdx.xy) * vc.shape0.y + vc.gmdx.zw) * cells;
    const vec2 gc = floor(g);
    const uvec2 ci = uvec2(mod(gc, cells));
    const uvec3 hh = gcHash3(uvec3(ci, 0x5bd1u));
    const vec3 r = vec3(hh) * (1.0 / 4294967295.0);
    const uvec3 hh2 = gcHash3(uvec3(ci, 0x2c1bu));
    const vec3 r2 = vec3(hh2) * (1.0 / 4294967295.0);
    if (r.x > 0.55 * vc.gmdx2.y) return 0.0;                      // this cell holds no body
    const float R = cellM * mix(0.22, 0.35, r2.x);                 // half the body's width
    const vec2 centre = vec2(0.5) + (r2.yz - 0.5) * 0.3;
    vec2 l = (g - gc - centre) * cellM;
    const float a = r.z * 6.2831853;
    l = mat2(cos(a), sin(a), -sin(a), cos(a)) * l;
    const float zl = (gcAlt(p) - vcBase()) / (2.0 * R);
    vec3 uvw = vec3(l / (2.0 * R) + 0.5, zl);
    if (any(lessThan(uvw, vec3(0.0))) || any(greaterThan(uvw, vec3(1.0)))) return 0.0;
    const uint k = uint(r.y * float(count)) % count;
    const float e = 0.5 / 64.0;
    uvw = clamp(uvw, vec3(e), vec3(1.0 - e));
    const vec3 blk = vec3(float(k & 1u), float((k >> 1) & 1u), float(k >> 2));
    const float d = textureLod(uVcShapes, (uvw + blk) * 0.5, 0.0).r;
    return d * 0.7 * smoothstep(0.3, 0.7, w.cov) * vc.gmdx2.y;
}

// Density at p (m, agent frame), h its height fraction, body = gcBody there. detail = the full
// sample (erosion).
float gcDensity(vec3 p, float h, GcWeather w, float body, bool detail)
{
    const bool valid = gcValid(h, w);
    if (!valid && body <= 0.0) return 0.0;
    vec3 q = p;
    q.xy += vc.gmdx.xy;
    // Anti-tiling for top-down views (Wicked: y += x/3 + z/7, Y up), then the wind, and a slow
    // vertical drift so the clouds churn as they travel.
    q.z += q.x / 3.0 + q.y / 7.0 + mod(vc.cam.w * 4.0, 1.0 / vc.shape0.z);
    q.xy += vc.shape1.xy;
    float c = 0.0;
    vec4 n = vec4(0.5);
    if (valid)
    {
        n = textureLod(uVcBase, q * (vc.shape0.z * 4.0), 0.0);
        // Upstream's noises sit in a narrow band (Perlin-Worley 0.62 - 0.86, the Worley fBm
        // 0.34 - 0.63, 5th to 95th percentile); Wicked's model expects 0 - 1. Unstretched, the
        // base shape is nearly constant and the clouds are the coverage map pushed straight up
        // (tents and spikes).
        const float fbm = gcStretch(n.g * 0.625 + n.b * 0.25 + n.a * 0.125, 0.34, 0.63);
        c = gcRemap(gcStretch(n.r, 0.62, 0.86), -(1.0 - fbm), 1.0, 0.0, 1.0);
        c *= smoothstep(w.grad.x, w.grad.y, h) - smoothstep(w.grad.z, w.grad.w, h);
        c = gcRemap(c, 1.0 - w.cov, 1.0, 0.0, 1.0) * w.cov;
        c *= gcAnvil(h, w);
    }
    c = max(c, body);
    if (c > 0.0 && detail)
    {
        // Wicked bends the detail lookup with a curl noise near the base (up to 500 m); the
        // base noise's three Worley octaves, already fetched, stand in for it.
        const vec3 curl = (n.gba - 0.5) * (1000.0 * pow(1.0 - h, 5.0));
        const vec3 d = textureLod(uVcDetail, (q + curl) * vc.shape0.w, 0.0).rgb;
        const float dfbm = gcStretch(d.r * 0.625 + d.g * 0.25 + d.b * 0.125, 0.34, 0.63);
        // Wispy at the base, billowy above.
        const float mod_ = mix(1.0 - dfbm, dfbm, clamp(h * 10.0, 0.0, 1.0));
        c = gcRemap(c, mod_ * vc.shape1.w, 1.0, 0.0, 1.0);
    }
    return max(c, 0.0) * w.dens;
}
float gcDensity(vec3 p, float h, GcWeather w, bool detail) { return gcDensity(p, h, w, gcBody(p, h, w), detail); }

// The cheap shape at p (the shadow map; vcDensity is upstream's name).
float vcDensity(vec3 p, bool detail)
{
    const float h = gcHeight(p);
    if (h <= 0.0 || h >= 1.0) return 0.0;
    return gcDensity(p, h, gcWeather(p), detail);
}

// Extinction per m (Wicked's rgb ratio at RenderVKVolCloudDensity 1: 0.071, 0.086, 0.1).
vec3 gcSigma() { return vc.shape1.z * vec3(1.775, 2.15, 2.5); }

// Dual-lobe Cornette-Shanks (g 0.5 / -0.5, blend 0.2); c = dot(view, light).
float gcCS(float g, float c)
{
    const float k = 3.0 / (8.0 * GC_PI) * (1.0 - g * g) / (2.0 + g * g);
    return k * (1.0 + c * c) / pow(max(1.0 + g * g - 2.0 * g * c, 1e-4), 1.5);
}
float gcPhase(float c) { return mix(gcCS(0.5, c), gcCS(-0.5, c), 0.2); }

// ---- light ----
struct VcLight { vec3 L; vec3 E; vec3 sky; vec3 albedo; float groundAlbedo; };

// The light's illuminance in the units the sky and the lit surfaces use: the physical sky's
// light (radiance = E x phase, as its LUTs), or WindLight's sunlit (a white surface shows
// sunlit x albedo, so E = pi x sunlit).
VcLight vcLightSetup()
{
    VcLight l;
    l.L = atmosLightDir();
    if (gcPhys())
        l.E = max(pkLight(), vec3(0.0));
    else
        l.E = GC_PI * max(ssr.atmos_k[5].rgb, vec3(0.0)) * smoothstep(-0.06, 0.04, l.L.z);
    // Sky ambient: mean sky radiance (the zenith and four 15-degree directions, the upper
    // hemisphere; x 0.6 for the dimmer half below), Wicked's sky-luminance term.
    vec3 s = skyColor(vec3(0.0, 0.0, 1.0)) * 0.4;
    for (int i = 0; i < 4; ++i)
    {
        const float a = float(i) * 1.5707963 + 0.785398;
        s += skyColor(vec3(cos(a) * 0.966, sin(a) * 0.966, 0.259)) * 0.15;
    }
    l.sky = s * 0.6 * vc.shape2.x;
    // Albedo: Wicked's 0.9 in the region's cloud hue (EEP cloud_color; its brightness is the
    // light's business).
    const vec3 cc = max(ssr.clouds[0].rgb, vec3(1e-3));
    l.albedo = 0.9 * cc / max(max(cc.r, cc.g), cc.b);
    l.groundAlbedo = gcPhys() ? pkGroundAlbedo() : 0.3;
    return l;
}

// The light reaching p through the air (the physical sky's transmittance at p's own height
// and place, the planet's shadow included), before the clouds.
vec3 gcSunAt(VcLight l, vec3 p)
{
    if (!gcPhys()) return l.E;
    const vec3 pos = vec3(0.0, 0.0, pkBottom() + max(pkViewHeight(), 0.0)) + (p - vc.cam.xyz) * 0.001;
    const float r = length(pos);
    const vec3 up = pos / r;
    const float cz = dot(up, l.L);
    if (pkRaySphere(pos, l.L, pkBottom()) >= 0.0) return vec3(0.0);
    return l.E * textureLod(uPkTrans, pkTransUv(r, cz), 0.0).rgb;
}

// Optical depth (x the octaves' extinction) toward the light: Wicked's quadratic 5-sample
// spread over 3 km; the first two with detail.
vec3 gcLightOD(vec3 p, vec3 L, int n)
{
    const float len = 3000.0;
    float od = 0.0;
    for (int s = 0; s < n; ++s)
    {
        float t0 = float(s) / float(n), t1 = float(s + 1) / float(n);
        t0 *= t0; t1 *= t1;
        const vec3 q = p + L * (len * (t0 + (t1 - t0) * 0.5));
        const float h = gcHeight(q);
        if (h < 0.0 || h > 1.0) break;
        od += gcDensity(q, h, gcWeather(q), s < 2) * (t1 - t0);
    }
    return gcSigma() * (od * len);
}

// Light off the sunlit ground under the cloud, through the cloud below p (3 samples).
vec3 gcGround(VcLight l, vec3 p, vec3 sunAir)
{
    const float below = gcAlt(p) - vcBase();
    const float len = min(4000.0, max(below, 0.0));
    float od = 0.0;
    for (int s = 0; s < 3; ++s)
    {
        float t0 = float(s) / 3.0, t1 = float(s + 1) / 3.0;
        t0 *= t0; t1 *= t1;
        const vec3 q = p - vec3(0.0, 0.0, len * (t0 + (t1 - t0) * 0.5));
        const float h = gcHeight(q);
        if (h < 0.0 || h > 1.0) continue;
        od += gcDensity(q, h, gcWeather(q), false) * (t1 - t0);
    }
    const float brdf = clamp(l.L.z, 0.0, 1.0) * l.groundAlbedo / GC_PI;
    return sunAir * (brdf * 0.5) * exp(-gcSigma() * (od * len));   // 0.5 = 2 pi x 1 / (4 pi)
}

// The view ray's span inside the curved shell, from the camera along V (unit), up to tmax.
// altitude(t) - level = a t^2 + V.z t + (cam.z - level), a = (1 - V.z^2) / 2R.
bool gcRoots(float a, float b, float c, out float r0, out float r1)
{
    const float disc = b * b - 4.0 * a * c;
    if (disc < 0.0) { r0 = r1 = 0.0; return false; }
    const float s = sqrt(disc);
    const float q = -0.5 * (b + (b >= 0.0 ? s : -s));
    const float x0 = q / max(a, 1e-20);
    const float x1 = abs(q) > 1e-20 ? c / q : 0.0;
    r0 = min(x0, x1); r1 = max(x0, x1);
    return true;
}
bool vcSegment(vec3 V, float tmax, out float t0, out float t1)
{
    const float a = (1.0 - V.z * V.z) * GC_HALF_INV_R;
    const float z = vc.cam.z;
    float tT0, tT1, tB0, tB1;
    t0 = 0.0; t1 = 0.0;
    if (!gcRoots(a, V.z, z - vcTop(), tT0, tT1)) return false;   // always above the top
    const bool hitsBase = gcRoots(a, V.z, z - vcBase(), tB0, tB1);
    t0 = max(tT0, 0.0);
    if (hitsBase && t0 > tB0 && t0 < tB1) t0 = tB1;               // starts under the base
    t1 = tT1;
    if (hitsBase && tB0 > t0) t1 = min(t1, tB0);                  // goes down through the base
    t1 = min(t1, tmax);
    return t1 > t0;
}

struct VcResult { vec3 scat; float T; float entry; float dmean; };

// March the shell along V from the camera up to tmax (scene distance / far). steps = the
// budget for a full-length segment; jitter in [0, 1).
VcResult vcMarch(VcLight l, vec3 V, float tmax, int steps, int lightSteps, float jitter)
{
    VcResult r;
    r.scat = vec3(0.0); r.T = 1.0; r.entry = VC_FAR; r.dmean = VC_FAR;
    float t0, t1;
    if (!vcSegment(V, min(tmax, vc.shape2.y), t0, t1)) return r;
    const float frac = clamp((t1 - t0) / (gcThick() * 4.0), 0.25, 1.0);
    const float n = float(max(int(float(steps) * frac), 8));
    const float span = t1 - t0;
    const vec3 cam = vc.cam.xyz;
    const vec3 sig = gcSigma();
    const float cos_t = dot(V, l.L);
    const float ph0 = gcPhase(cos_t);
    const float ph1 = mix(1.0 / (4.0 * GC_PI), ph0, 0.2);   // the second octave: flatter
    const bool lit = dot(l.E, l.E) > 0.0;
    vec3 Tv = vec3(1.0);
    float wsum = 0.0, dsum = 0.0;
    // Quadratic spacing: short steps where the ray enters the layer (or leaves the camera inside
    // it), long ones far away -- even steps over a 40 km grazing ray left stripes in the walls.
    for (float i = 0.0; i < n; i += 1.0)
    {
        const float ua = i / n, ub = (i + 1.0) / n;
        const float ta = t0 + span * ua * ua;
        const float dt = span * (ub * ub - ua * ua);
        const float t = ta + jitter * dt;
        const vec3 p = cam + V * t;
        const float h = gcHeight(p);
        if (h < 0.0 || h > 1.0) continue;
        const GcWeather w = gcWeather(p);
        const float body = gcBody(p, h, w);
        if (!gcValid(h, w) && body <= 0.0)
        {
            // Nothing can be here: skip the next step too.
            i += 1.0;
            continue;
        }
        const float dens = gcDensity(p, h, w, body, true);
        if (dens > 0.001)
        {
            if (r.entry >= VC_FAR) r.entry = max(t - dt, 0.0);
            const vec3 ext0 = min(dens * sig, vec3(1.0));
            const vec3 ext1 = ext0 * 0.1;
            // Beer-powder albedo (Wicked: beer powder 20, power 0.5).
            const vec3 scat0 = l.albedo * sqrt(clamp(dens * 0.9 * 20.0, 0.0, 1.0)) * ext0;
            const vec3 scat1 = scat0;
            const vec3 sunAir = gcSunAt(l, p);
            vec3 Tl0 = vec3(0.0), Tl1 = vec3(0.0);
            vec3 env = l.sky * clamp(0.75 + h, 0.0, 1.0);
            if (lit && dot(sunAir, sunAir) > 0.0)
            {
                const vec3 od = lightSteps > 0 ? gcLightOD(p, l.L, lightSteps) : vec3(0.0);
                Tl0 = exp(-od); Tl1 = exp(-od * 0.1);
                if (max(max(Tv.r, Tv.g), Tv.b) > 0.01) env += gcGround(l, p, sunAir);
            }
            // Second octave first (it uses the view transmittance before this step's).
            const vec3 S1 = Tl1 * sunAir * ph1 * scat1;
            const vec3 Tr1 = exp(-max(ext1, vec3(1e-7)) * dt);
            r.scat += Tv * (S1 - S1 * Tr1) / max(ext1, vec3(1e-7));
            const vec3 S0 = (Tl0 * sunAir * ph0 + env) * scat0;
            const vec3 Tr0 = exp(-max(ext0, vec3(1e-7)) * dt);
            r.scat += Tv * (S0 - S0 * Tr0) / max(ext0, vec3(1e-7));
            const float Tm = dot(Tv, vec3(1.0 / 3.0));
            Tv *= Tr0;
            const float wgt = Tm - dot(Tv, vec3(1.0 / 3.0));
            wsum += wgt; dsum += wgt * t;
            if (all(lessThan(Tv, vec3(0.005)))) { Tv = vec3(0.0); break; }
        }
    }
    r.T = dot(Tv, vec3(1.0 / 3.0));
    if (r.T < 0.005) r.T = 0.0;
    if (wsum > 0.0) r.dmean = dsum / wsum;
    return r;
}

// Aerial perspective + far fade: the layer dissolves into the haze (physical or WindLight,
// atmosScatterHaze -- the ground's own) and the sky.
VcResult vcAtmosphere(VcResult r, vec3 V)
{
    if (r.T >= 1.0) return r;
    if (atmosEnabled())
    {
        vec3 additive, atten;
        atmosScatterHaze(V * r.dmean, additive, atten);
        r.scat = r.scat * atten.r + (1.0 - r.T) * atmosSrgbToLinear(additive * 2.0) * atmosHdrScale();
    }
    const float maxd = vc.shape2.y;
    const float f = smoothstep(maxd * 0.45, maxd, r.dmean);
    r.scat *= 1.0 - f;
    r.T = mix(r.T, 1.0, f);
    r.scat = atmosClampHDR(r.scat);
    return r;
}

// Interleaved gradient noise (Jimenez 2014).
float vcIgn(vec2 px) { return fract(52.9829189 * fract(dot(px, vec2(0.06711056, 0.00583715)))); }
