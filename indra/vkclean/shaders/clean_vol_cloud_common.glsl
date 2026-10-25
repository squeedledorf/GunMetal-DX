// vkclean -- VOLUMETRIC SKY CLOUD LAYER (#719): shared shape + lighting + march.
//
// Our implementation of the published real-time cloud techniques: the shape model of
// Schneider & Vos ("The real-time volumetric cloudscapes of Horizon Zero Dawn", SIGGRAPH
// 2015; "Nubis", 2017) -- a tiling 3D Perlin-Worley base noise eroded by a 3D Worley detail
// noise, coverage / cloud type from a 2D weather map, a per-type height profile -- with
// Hillaire's energy-conserving integration and Beer-Lambert transmittance ("Physically based
// sky, atmosphere and cloud rendering in Frostbite", 2016), the powder term (Schneider), a
// dual-lobe Henyey-Greenstein phase (forward lobe = silver lining) and Wrenninge's
// octave multiple-scattering approximation ("Oz: the great and volumetric", 2013).
//
// Geometry: a flat SLAB between the base and top altitudes (ssr.vol_clouds[0].yz; region Z,
// the viewer's settings -- EEP has no altitude). SL is flat; the far end of the slab fades
// into the sky (RenderVKVolCloudMaxDist) and into the WindLight aerial perspective.
//
// Weather map (clean_vol_cloud_weather.comp, every frame, periodic over vc.shape0.x metres):
// r = coverage, g = cloud type (0 stratus .. 1 towering cumulus), b = local density, all
// derived from the region's EEP cloud parameters and its 2D cloud noise texture, with the EEP
// scroll baked in (wind). The 3D noises move with the same wind (vc.shape1.xy).
//
// Requires clean_frame_info.glsl (ssr) + clean_atmos_common.glsl, and the module set at VC_SET.

#ifndef VC_SET
#define VC_SET 1
#endif

#include "clean_vol_cloud_ubo.glsl"

// The texels the march updates this frame (clean_vol_cloud_march.comp's thread mapping).
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
layout(set = VC_SET, binding = 3) uniform sampler2D uVcWeather;   // coverage, type, density

const float VC_FAR = 65000.0;   // fp16-safe "no cloud" distance key

float vcBase()  { return ssr.vol_clouds[0].y; }
float vcTop()   { return ssr.vol_clouds[0].z; }

float vcRemap(float v, float lo, float hi, float nlo, float nhi)
{
    return nlo + (v - lo) / max(hi - lo, 1e-5) * (nhi - nlo);
}

// Height profile per cloud type (h = 0 base .. 1 top of the shell): flat-ish bases, rounded
// tops; stratus stays in the lower quarter, cumulus reaches the top.
float vcHeightProfile(float h, float type)
{
    const float top = mix(0.22, 1.0, type);
    const float bottom = smoothstep(0.0, mix(0.04, 0.12, type), h);
    const float upper = 1.0 - smoothstep(top * mix(0.55, 0.7, type), top, h);
    return bottom * upper;
}

// Density at world point p (m, agent frame). detail = erode with the 32^3 noise (the full
// sample); off = the cheap shape (light march, shadow map, empty-space search).
float vcDensity(vec3 p, bool detail)
{
    const float base = vcBase(), top = vcTop();
    const float h = (p.z - base) / max(top - base, 1.0);
    if (h <= 0.0 || h >= 1.0) return 0.0;
    const vec4 w = textureLod(uVcWeather, p.xy * vc.shape0.y, 0.0);
    const float cov = w.r;
    if (cov <= 0.004) return 0.0;
    const float prof = vcHeightProfile(h, w.g);
    if (prof <= 0.0) return 0.0;
    const vec3 q = vec3(p.xy + vc.shape1.xy, p.z - base) * vc.shape0.z;
    const vec4 n = textureLod(uVcBase, q, 0.0);
    const float fbm = n.g * 0.625 + n.b * 0.25 + n.a * 0.125;
    float c = vcRemap(n.r, fbm - 1.0, 1.0, 0.0, 1.0) * prof;
    c = vcRemap(c, 1.0 - cov, 1.0, 0.0, 1.0) * cov;
    if (c <= 0.0) return 0.0;
    if (detail)
    {
        // Detail erosion: billowy (inverted Worley) higher up, wispy at the base; sheared a
        // little with height so the towers lean downwind.
        const vec3 dq = vec3(p.xy + vc.shape1.xy * 1.5 + vc.shape1.xy * (0.15 * h), p.z - base) * vc.shape0.w;
        const vec3 d = textureLod(uVcDetail, dq, 0.0).rgb;
        const float dfbm = d.r * 0.625 + d.g * 0.25 + d.b * 0.125;
        const float dmod = mix(dfbm, 1.0 - dfbm, clamp(h * 8.0, 0.0, 1.0));
        c = vcRemap(c, dmod * vc.shape1.w, 1.0, 0.0, 1.0);
    }
    return clamp(c, 0.0, 1.0) * w.b;
}

// Henyey-Greenstein, normalised so the isotropic phase is 1 (radiance ~ irradiance for a
// thick white cloud, the brightness of a white surface in the same light).
float vcHG(float cos_t, float g)
{
    const float g2 = g * g;
    return (1.0 - g2) / pow(max(1.0 + g2 - 2.0 * g * cos_t, 1e-4), 1.5);
}
// Dual lobe: forward (silver lining toward the sun) + a weaker backward lobe.
float vcPhase(float cos_t, float scale)
{
    return mix(vcHG(cos_t, vc.phase.x * scale), vcHG(cos_t, -vc.phase.y * scale), vc.phase.z);
}

// Light this frame: sun / moon beam as surfaces receive it (atmos_k[5]: the same sunlit the
// lit passes use), sky ambient from the WindLight sky itself (zenith / horizon colour).
struct VcLight { vec3 L; vec3 sun; vec3 ambTop; vec3 ambBottom; };
VcLight vcLightSetup()
{
    VcLight l;
    l.L = atmosLightDir();
    // Below the horizon the beam has to pass the ground: fade it out through twilight.
    l.sun = max(ssr.atmos_k[5].rgb, vec3(0.0)) * smoothstep(-0.06, 0.04, l.L.z);
    const vec3 zen = skyColor(vec3(0.0, 0.0, 1.0));
    // (the horizon away from the light: the sky that fills the clouds' shadowed sides)
    const vec3 hor = skyColor(normalize(vec3(-l.L.x, -l.L.y, 0.15)));
    l.ambTop = mix(zen, hor, 0.3) * vc.shape2.x;
    l.ambBottom = l.ambTop * 0.35;
    return l;
}

// Optical depth toward the light from p (cheap shape): 6 samples with growing steps through
// the upper shell, the last one far (shadowing from distant towers).
float vcLightOD(vec3 p, vec3 L, int n)
{
    const float sig = vc.shape1.z;
    float od = 0.0, t = 0.0;
    float ds = 40.0;
    for (int i = 0; i < n; ++i)
    {
        const float step_i = (i == n - 1) ? ds * 4.0 : ds;
        t += step_i * 0.5;
        od += vcDensity(p + L * t, i < 2) * step_i;
        t += step_i * 0.5;
        ds *= 1.6;
    }
    return od * sig;
}

// Scattered radiance per unit sigma_s at a sample: multi-octave (Wrenninge: a = b = c = 0.5)
// sun term with Beer-Lambert + powder, plus the height-graded ambient.
vec3 vcInScatter(VcLight l, float cos_t, float od, float dens, float h)
{
    const float ms = vc.shape2.z;
    float sunTerm = 0.0, a = 1.0, b = 1.0, c = 1.0;
    for (int i = 0; i < 3; ++i)
    {
        sunTerm += a * vcPhase(cos_t, c) * exp(-od * b);
        a *= 0.5 * ms; b *= 0.5; c *= 0.5;
    }
    // Powder (dark edges facing away from the sun): strongest looking away from the light.
    const float powder = 1.0 - exp(-2.0 * dens * vc.shape1.z * 60.0);
    sunTerm *= mix(1.0, powder, vc.shape2.w * clamp(0.5 - 0.5 * cos_t, 0.0, 1.0));
    const vec3 amb = mix(l.ambBottom, l.ambTop, h) * (0.6 + 0.4 * exp(-od * 0.25));
    return l.sun * sunTerm + amb;
}

// The cloud colour (EEP cloud_color, LL composites clamp(colour * 2)): scattering albedo.
vec3 vcAlbedo() { return clamp(ssr.clouds[0].rgb * 2.0, vec3(0.05), vec3(1.0)); }

// The view ray's segment inside the slab, from the eye at height cz along V (unit), up to
// tmax. false = no segment.
bool vcSegment(float cz, vec3 V, float tmax, out float t0, out float t1)
{
    const float base = vcBase(), top = vcTop();
    t0 = 0.0; t1 = tmax;
    if (abs(V.z) < 1e-5)
        return cz > base && cz < top && tmax > 0.0;
    const float tb = (base - cz) / V.z, tt = (top - cz) / V.z;
    if (cz >= base && cz <= top)
        t1 = min(tmax, V.z > 0.0 ? tt : tb);         // inside: to the exit plane
    else
    {
        t0 = max(min(tb, tt), 0.0);
        t1 = min(max(tb, tt), tmax);
        if (max(tb, tt) <= 0.0) return false;        // the slab is behind
    }
    return t1 > t0;
}

struct VcResult { vec3 scat; float T; float entry; float dmean; };

// March the layer along V from the camera (vc.cam) up to tmax (scene distance / far). steps
// = the budget for a full-length segment; jitter in [0, 1) (temporal / spatial dither).
VcResult vcMarch(VcLight l, vec3 V, float tmax, int steps, int lightSteps, float jitter)
{
    VcResult r;
    r.scat = vec3(0.0); r.T = 1.0; r.entry = VC_FAR; r.dmean = VC_FAR;
    float t0, t1;
    const vec3 cam = vc.cam.xyz;
    if (!vcSegment(cam.z, V, min(tmax, vc.shape2.y), t0, t1)) return r;
    // Steps scale with the segment (a grazing ray crosses many km): the budget covers about
    // 4 x the shell thickness at full count, never below a quarter of it.
    const float thick = max(vcTop() - vcBase(), 1.0);
    const float frac = clamp((t1 - t0) / (thick * 4.0), 0.25, 1.0);
    const int n = max(int(float(steps) * frac), 8);
    const float dt = (t1 - t0) / float(n);
    const float sig = vc.shape1.z;
    const vec3 alb = vcAlbedo();
    const float cos_t = dot(V, l.L);
    float wsum = 0.0, dsum = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const float t = t0 + (float(i) + jitter) * dt;
        const vec3 p = cam + V * t;
        // Cheap shape first: empty space costs one weather + one base fetch.
        if (vcDensity(p, false) <= 0.0) continue;
        const float dens = vcDensity(p, true);
        if (dens <= 0.0) continue;
        if (r.entry >= VC_FAR) r.entry = max(t - dt, 0.0);
        const float h = clamp((p.z - vcBase()) / thick, 0.0, 1.0);
        const float sigt = dens * sig;
        const float od = (lightSteps > 0 && dot(l.sun, l.sun) > 0.0) ? vcLightOD(p, l.L, lightSteps) : 0.0;
        const vec3 S = sigt * alb * vcInScatter(l, cos_t, od, dens, h);
        const float Tr = exp(-sigt * dt);
        r.scat += r.T * (S - S * Tr) / max(sigt, 1e-7);   // Hillaire's energy-conserving step
        const float wgt = r.T * (1.0 - Tr);
        wsum += wgt; dsum += wgt * t;
        r.T *= Tr;
        if (r.T < 0.01) { r.T = 0.0; break; }
    }
    if (wsum > 0.0) r.dmean = dsum / wsum;
    return r;
}

// Aerial perspective + far fade: the layer dissolves into the WindLight haze and the sky.
// V = view direction (world), returns the composited (premultiplied) result.
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
