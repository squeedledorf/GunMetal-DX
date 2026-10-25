// vkclean -- WEATHER AT RAY-TRACED HITS (#970; part of #783). Two separate, self-contained calls
// the RT consumers make on a hit's RtSurface (clean_rt_surface.glsl) BEFORE it is shaded
// (clean_rt_shade.glsl rtShadeHit -- which this file never touches):
//
//   rtWeatherHit(s)              THE COAT: clean_weather_surface.glsl's weatherSurface -- the very
//                                function the G-buffer / forward lit FS runs (wetness, puddles,
//                                ripples, run-off, the snow coating, persistent cover, movers' and
//                                worn attachments' own snow) -- on the hit, from the same weather
//                                maps (draw set 0 binding 49) at the hit's world position.
//   rtSnowField(s, R, tHit)      THE LYING SNOW (the #783 M3 snow mesh is not in the TLAS): a short
//                                height-field march against the published snow DEPTH grid (the
//                                cover grid's .a -- the mesh's own source) backward from the hit
//                                along the ray; where the ray entered the snow slab in front of the
//                                hit, the hit becomes that snow surface (the mesh FS's fresh-snow
//                                PBR and relief). The coat is not applied to it again.
//
// WHY A HEIGHT-FIELD MARCH, NOT A SNOW-MESH BLAS (the ticket's options A / B):
//   * the snow mesh is rebuilt every RenderVKWeatherSnowMeshInterval recordings and LOCALLY
//     PATCHED nearly every recording while anything walks in snow (#946: index ranges rewritten
//     in place, so a BLAS could not even be refit -- an update needs the same indices): a full
//     BLAS build over its triangle capacity every frame, in the AS build stage, plus its VRAM
//     (vertices + indices are there; the BLAS + scratch would not be);
//   * the mesh build culls every block BEHIND THE VIEW CONE (it is drawn for the camera) -- which
//     is exactly where a mirror / calm water reflection looks: the BLAS would be empty there;
//   * the inner / ring hand-over band overlaps two surfaces (the raster cuts the ring in its FS):
//     a BLAS needs an any-hit test there or reflections show a double sheet;
//   * a new RT instance class in rtSurfaceEval and every ray type's mask (shadow, AO, GI ...).
//   The depth grid is the mesh's own source, is all around the camera (~512^2 at ~6 cm, ~30 m),
//   is published every recording (a footprint shows in the reflection the frame its press lands)
//   and is already bound to every RT consumer (draw set 0 binding 49, FRAGMENT | COMPUTE): no AS
//   work, no VRAM, no descriptor change. GAPS (documented): only the INNER grid (the outer ring's
//   images are the mesh compute's own set -- past the inner grid the coat carries the snow, and
//   weatherCoverAt's #906 "depth is snow" rule already makes the coat full white there); no
//   smoothing / spike clamp / lift (the raster mesh's), a few cm of difference no reflection
//   resolves; nearest texels (6 cm steps in the reflected silhouette); the march reaches at most
//   ~4 m back from the hit (a grazing ray that skimmed a long deep slab sees its far end).
//
// GAPS of the coat at hits: rigged hits carry no avatar slot (the raster's comes from the
// pre-skin cache word in the VS): they take the world rules (wet where open), not the avatar's own
// snow / wetness; no #901 rest-pose grid (WEATHER_REST is the VS's). Worn rigid attachments find
// their wearer through the instance row's skin_id, as the raster does.
//
// Toggles (frame info ssr.weather[14].w; llcleanweathervk.cpp): bit 64 the coat at hits
// (RenderVKRTWeatherHits), bit 128 the lying snow field (RenderVKRTSnowField; only while the snow
// mesh is drawn this recording). Both cost nothing while clear (one uniform branch).
//
// Requires before inclusion: clean_frame_info.glsl (ssr), clean_rt_common.glsl (rt_instances),
// clean_rt_surface.glsl (RtSurface, rt_surf_foot). Declares draw set 0 binding 49
// (uWeatherOccl, via clean_weather_surface.glsl).
#ifndef CLEAN_RT_WEATHER_GLSL
#define CLEAN_RT_WEATHER_GLSL

#include "clean_surface_lit.glsl"   // SurfaceLit / SURF_* (no bindings)

// weatherSurface's per-fragment instance names: set from the hit before each call.
uint rt_wx_flags = 0u;
uint rt_wx_id = 0xFFFFFFFFu;
#define WEATHER_INST_FLAGS rt_wx_flags
#define WEATHER_INST_ID rt_wx_id
// The instance rows: the RT set's (same 224 B LLCleanInstanceStore::Instance; rest[7].w = skin_id).
#define WX_INST_EXTERNAL
#define WX_INST_COUNT() uint(rt_instances.length())
#define WX_INST_MODEL(i) rt_instances[i].model_matrix
#define WX_INST_SKIN(i) rt_instances[i].skin_id
#define WX_NO_JITTER                               // (no pixel: the sky test's fixed PCF disc)
#define WX_FOOTPRINT(est) max((est), rt_surf_foot) // (the ray cone's footprint at the hit)
#include "clean_weather_surface.glsl"

uint rtWxBits() { return uint(ssr.weather[14].w + 0.5); }

// ---- THE COAT ----
void rtWeatherHit(inout RtSurface s)
{
    if ((rtWxBits() & 64u) == 0u) return;
    if (s.model != RT_SURF_PBR && s.model != RT_SURF_LEGACY) return;     // (water / fullbright: as the raster)
    SurfaceLit l = surfaceInit();
    l.model = s.model == RT_SURF_PBR ? SURF_PBR : SURF_LEGACY;
    l.albedo = s.base.rgb;
    l.N = s.N;
    l.Ng = s.Nv;   // (the raster's Ng: the interpolated vertex normal, flipped to the visible side)
    l.rough = s.rough; l.metal = s.metal; l.ao = s.ao; l.emissive = s.emissive;
    l.spec = s.spec_lin; l.gloss = s.gloss; l.env = s.env;
    rt_wx_flags = s.flags;
    rt_wx_id = (s.flags & 1u) != 0u ? 0xFFFFFFFFu : s.id;   // (rigged: no avatar slot at a hit)
    weatherSurface(l, s.P);
    s.base.rgb = l.albedo;
    s.N = l.N;
    s.rough = l.rough; s.metal = l.metal; s.ao = l.ao; s.emissive = l.emissive;
    s.spec_lin = l.spec; s.gloss = l.gloss; s.env = l.env;
}

// ---- THE LYING SNOW FIELD ----
const float kRtSnowMin = 0.02;   // the mesh's visual threshold (llcleanweathervk.cpp kMeshMinDepth)
const int   kRtSnowSteps = 8;    // backward samples from the hit
const int   kRtSnowBisect = 5;   // refinement (a step / 32)

// The published grid's texel at P's column: x the column top (m), y the snow depth there (m); false:
// P's column is outside the grid.
bool rtSnowAt(vec3 P, out vec2 ta, out ivec2 tc)
{
    const vec4 pr = ssr.weather[10], pz = ssr.weather[12];
    const vec2 uv = (P.xy - (pz.x - P.z) * pz.yz - pr.xy) * pr.z;
    ta = vec2(0.0); tc = ivec2(0);
    // (the outer 4 %: the mesh's inner layer geomorphs onto the ring there -- the coat carries it)
    if (any(lessThan(uv, vec2(0.04))) || any(greaterThan(uv, vec2(0.96)))) return false;
    const float res = 1.0 / (pr.z * pz.w);
    tc = clamp(ivec2(uv * res), ivec2(0), ivec2(int(res) - 1));
    const vec4 c = pr.w < 1.5 ? texelFetch(uWeatherOccl[4], tc, 0) : texelFetch(uWeatherOccl[5], tc, 0);
    ta = vec2(pz.x - c.b, c.a);
    return true;
}
vec2 rtSnowTexel(ivec2 t)
{
    const vec4 c = ssr.weather[10].w < 1.5 ? texelFetch(uWeatherOccl[4], t, 0) : texelFetch(uWeatherOccl[5], t, 0);
    return vec2(ssr.weather[12].x - c.b, c.a);
}
// Where q stands against its column: 1 INSIDE the snow slab (top - tol .. top + depth, depth past
// the threshold), 2 ABOVE it (or above a bare column top), 0 UNDER the column top (under a roof /
// in the ground: the slab is not in front of it), 3 outside the grid.
uint rtSnowClass(vec3 q, float tol)
{
    vec2 ta; ivec2 tc;
    if (!rtSnowAt(q, ta, tc)) return 3u;
    const float d = ta.y >= kRtSnowMin ? ta.y : 0.0;
    if (q.z > ta.x + d) return 2u;
    if (d > 0.0 && q.z >= ta.x - tol) return 1u;
    return 0u;
}

// s: the TLAS hit (P, Ng) seen along unit R; tHit its distance. True: the ray entered the lying snow
// in front of the hit -- s is now that snow surface and tHit its distance.
bool rtSnowField(inout RtSurface s, vec3 R, inout float tHit)
{
    if ((rtWxBits() & 128u) == 0u || ssr.weather[10].w < 0.5) return false;
    const vec3 P = s.P;
    if (P.z < ssr.water[3].w - 0.02) return false;   // (no snow under the water line: the grid holds none)
    const float tol = max(ssr.weather[11].w, 0.05);
    vec2 ta; ivec2 tc;
    if (!rtSnowAt(P, ta, tc)) return false;
    const float d0 = ta.y >= kRtSnowMin ? ta.y : 0.0;
    // Cheap reject (one fetch): the hit is ON its column's top (an open surface) with no snow: the
    // ray reached it through open air. A hit UNDER its column's top (a wall's foot, under an eave)
    // marches: snow may be piled in front of it.
    if (d0 <= 0.0 && P.z >= ta.x - tol) return false;
    const float span = max(d0, 0.5);
    const float umax = min(clamp(1.25 * span / max(abs(R.z), 0.1), 0.25, 4.0), tHit - 0.05);
    if (umax < 0.05) return false;
    float uIn = -1.0, uOut = -1.0;
    for (int i = 0; i <= kRtSnowSteps; ++i)
    {
        const float u = umax * float(i) / float(kRtSnowSteps);
        const uint c = rtSnowClass(P - R * u, tol);
        if (c == 1u) { uIn = u; continue; }
        if (uIn >= 0.0) { if (c == 2u) uOut = u; break; }   // (in, then out above it: the entry; else a wall / roof ends it)
        if (c == 2u || c == 3u) return false;              // (open air right in front of the hit: no snow on the way)
    }
    if (uIn < 0.0) return false;
    if (uOut > 0.0)
    {
        for (int k = 0; k < kRtSnowBisect; ++k)
        {
            const float um = 0.5 * (uIn + uOut);
            if (rtSnowClass(P - R * um, tol) == 1u) uIn = um; else uOut = um;
        }
    }
    const float t = tHit - uIn;
    if (t < 0.05) return false;   // (the ray starts in the snow: its receiver is the snow itself)
    const vec3 q = P - R * uIn;
    // The snow surface's normal: the slope of top + depth over the 4-neighbourhood (nearest texels;
    // a neighbour over a different surface -- a wall's roof, a bare kerb -- is left out).
    if (!rtSnowAt(q, ta, tc)) return false;
    const float sc = ta.x + ta.y;
    const float texel = ssr.weather[12].w;
    const ivec2 rs = ivec2(int(1.0 / (ssr.weather[10].z * texel)) - 1);
    float sn[4];
    const ivec2 off[4] = ivec2[4](ivec2(1, 0), ivec2(-1, 0), ivec2(0, 1), ivec2(0, -1));
    for (int k = 0; k < 4; ++k)
    {
        const vec2 n = rtSnowTexel(clamp(tc + off[k], ivec2(0), rs));
        const float sk = n.x + (n.y >= kRtSnowMin ? n.y : 0.0);
        sn[k] = abs(sk - sc) < 0.5 ? sk : sc;
    }
    const vec2 g = clamp(vec2(sn[0] - sn[1], sn[2] - sn[3]) / (2.0 * texel), vec2(-3.0), vec2(3.0));
    vec3 Ng = normalize(vec3(-g, 1.0));
    if (dot(Ng, R) > 0.0) Ng = normalize(Ng - R * (dot(Ng, R) + 0.02));   // (facing the ray)
    // The snow mesh FS's surface (clean_weather_snowmesh.frag / clean_draw.frag -DCLEAN_WEATHER_SNOWMESH).
    float glint;
    s.P = q;
    s.Ng = Ng;
    s.Nv = Ng;
    s.N = wxSnowRelief(Ng, q.xy, max(rt_surf_foot, 1.0e-3), glint);
    s.tlift = vec3(0.0);
    s.tw = 0.0;
    s.front = true;
    s.flags = 0u;
    s.mid = 0u;
    s.model = RT_SURF_PBR;
    s.terrain = false;
    s.base = vec4(0.86, 0.89, 0.93, 1.0);   // fresh snow (linear albedo; weatherSnowLayer's)
    s.texel = vec3(1.0);
    s.rough = mix(0.85, 0.3, glint);
    s.metal = 0.0;
    s.ao = 1.0;
    s.emissive = vec3(0.0);
    s.spec_lin = vec3(0.04); s.gloss = 0.0; s.env = 0.0;
    s.fb = vec3(0.0); s.fb_env = 0.0;
    tHit = t;
    return true;
}

// The two in order (reflection hits): the lying snow in front of the hit, else the coat on it.
void rtWeatherAtHit(inout RtSurface s, vec3 R, inout float tHit)
{
    if (!rtSnowField(s, R, tHit)) rtWeatherHit(s);
}

#endif   // CLEAN_RT_WEATHER_GLSL
