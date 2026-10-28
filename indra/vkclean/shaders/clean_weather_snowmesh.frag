#version 450
#extension GL_GOOGLE_include_directive : require
// vkclean — SNOW SURFACE MESH, G-buffer FS (#783 M3; clean_weather_snowmesh.vert): fresh snow
// as a PBR surface into the deferred G-buffer (the layout of clean_gbuffer.glsl gbufferPack,
// SURF_PBR -- kept in step by hand: the world FS is not used, so its register peak is
// untouched). Albedo / roughness match the coating (clean_weather_surface.glsl
// weatherSnowLayer) so the mesh and the coated surfaces around it read as one blanket; a fine
// value-noise relief tilts the shading normal. #906: the relief / sparkle are the coating's
// (clean_weather_snow_relief.glsl: gradient noise faded by the pixel footprint), not a 17 cm
// value-noise lattice that turned into a regular grid of dark dashes under a low sun.
// -DWX_MESH_PRE=0..3 (GI bug "ghost avatar silhouettes on snow", RenderVKWeatherSnowMeshPrepass):
// the DEPTH PRE-PASS twins -- the mesh lays down its depth (and the pass 1a targets in force)
// before the G-buffer, so every pre-pass reader (the motion vectors the GI / projector denoisers
// and TAA reproject by, the pre-pass AO, the RT shadow mask, the Hi-Z, the no-history mask) sees
// the snow surface instead of what it buries. The variants follow the renderer's pre-pass
// templates (LLCleanRendererVK mPreDescCl): 0 depth only (the hand-over discard alone), 1 + the
// AO normal target, 2 + normal, motion and no-history mask, 3 motion + no-history mask only
// (the slim deferred pre-pass). Same discard as the G-buffer FS, so the G-buffer draw (LEQUAL)
// keeps exactly these fragments.
#include "clean_weather_snow_relief.glsl"
#include "clean_frame_info.glsl"   // (#783: ssr.weather -- the depth debug's grids)
// #783 RenderVKWeatherSnowDepthDebug: the texel grids at world point P -- x: 1 on a border line of
// the INNER snow grid's texels (the near map's columns, ~6 cm), y: 1 on a border of the COARSE
// map's texels (~0.6 m) -- so a stair / tooth's size names the map that made it.
vec2 wxDebugGridLines(vec3 P)
{
    vec2 r = vec2(0.0);
    {
        const vec4 pr = ssr.weather[10], pz = ssr.weather[12];
        if (pr.w > 0.5)
        {
            const vec2 uv = (P.xy - (pz.x - P.z) * pz.yz - pr.xy) * pr.z;
            const vec2 f = abs(fract(uv / (pr.z * pz.w)) - 0.5);
            r.x = max(f.x, f.y) > 0.44 ? 1.0 : 0.0;
        }
    }
    {
        const vec4 cr = ssr.weather[1], cz = ssr.weather[2];
        if (cr.w > 0.5 && cz.w > 0.0)
        {
            const vec2 uv = (P.xy - (cz.x - P.z) * ssr.weather[6].xy - cr.xy) * cr.z;
            const vec2 f = abs(fract(uv / (cr.z * cz.w)) - 0.5);
            r.y = max(f.x, f.y) > 0.47 ? 1.0 : 0.0;
        }
    }
    return r;
}

layout(location = 0) in vec3 v_nrm;
layout(location = 1) in vec3 v_wpos;
layout(location = 3) in float v_dbg;   // #783 the depth debug's stride code
layout(location = 2) in vec2 v_band;   // x the inner -> ring hand-over t, y 1 = the outer ring's (#783: the ring's cut)

#if defined(WX_MESH_PRE)
#if WX_MESH_PRE == 1 || WX_MESH_PRE == 2
layout(location = 0) out vec4 outNormal;      // view-space oct(N), oct(smooth N) (clean_prepass_normal.frag)
#endif
#if WX_MESH_PRE == 2
layout(location = 1) out vec2 outMotion;      // UV units, current minus previous, unjittered
layout(location = 2) out float outNoTemporal;
#elif WX_MESH_PRE == 3
layout(location = 0) out vec2 outMotion;
layout(location = 1) out float outNoTemporal;
#endif
#else
layout(location = 0) out vec4 outG0;   // rgb albedo (sRGB storage), a ORM occlusion
layout(location = 1) out vec4 outG1;   // oct(N), oct(Ng)
layout(location = 2) out vec4 outG2;   // roughness, metallic, glow, model byte
layout(location = 3) out vec4 outG3;   // emissive
#endif

vec2 octEncode(vec3 n)
{
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1e-20);
    return n.z >= 0.0 ? n.xy : (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
}

void main()
{
    // #906: the pixel footprint (m) for the relief -- derivatives taken here, in uniform control
    // flow before the dither's discard.
    const float px = max(length(dFdx(v_wpos)), length(dFdy(v_wpos)));
    // THE HAND-OVER (clean_weather_snowmesh.comp): the inner grid geomorphs onto the ring's
    // surface over the band and is drawn whole; the ring only past it (t >= 0.98). #783: no
    // screen-space dither between the two any more -- it interleaved two surfaces of different
    // heights, and the AO read the noisy depth as a dark crease along the hand-over.
    if (v_band.y > 0.5 && v_band.x < 0.98) discard;   // (clean_weather_snowmesh.comp kRingCut)
    vec3 Ng = normalize(v_nrm);
#if defined(WX_MESH_PRE)
#if WX_MESH_PRE == 1 || WX_MESH_PRE == 2
    {
        // the G-buffer's shading normal (the relief) and the smooth one, view space
        float glintP;
        const vec3 Np = wxSnowRelief(Ng, v_wpos.xy, px, glintP);
        outNormal = vec4(octEncode(normalize(mat3(ssr.views[CLEAN_VIEW].cur_view) * Np)), octEncode(normalize(mat3(ssr.views[CLEAN_VIEW].cur_view) * Ng)));
    }
#endif
#if WX_MESH_PRE == 2 || WX_MESH_PRE == 3
    {
        // The snow is static in the world (its rebuilds / geomorph are not motion): camera-only,
        // as the world pre-pass's static geometry (camera-relative, #555).
        const vec4 cur  = ssr.views[CLEAN_VIEW].taa_cur_vp  * vec4(v_wpos - ssr.cam_origin.xyz, 1.0);
        const vec4 prev = ssr.views[CLEAN_VIEW].taa_prev_vp * vec4(v_wpos - ssr.cam_origin.xyz, 1.0);
        outMotion = prev.w > 1e-6 ? (cur.xy / cur.w - prev.xy / prev.w) * 0.5 : vec2(0.0);
        outNoTemporal = 0.0;
    }
#endif
#else
    // (#783: no flip for a back face -- snow is lit from above; a skirt or a fold seen from its
    // back took the down-facing normal and shaded BLACK: the dark slits along cut edges)
    // Relief + sparkle, as the coating's.
    float glint;
    const vec3 N = wxSnowRelief(Ng, v_wpos.xy, px, glint);
    const vec3 kSnow = vec3(0.86, 0.89, 0.93);   // fresh snow (linear albedo; weatherSnowLayer's)
    vec3 albedo = kSnow;
    // #783 RenderVKWeatherSnowDepthDebug (ssr.weather[14].w bit 32): the mesh in light blue with the
    // inner grid's texel borders (dark) and the coarse map's (red) -- a wall / tooth whose size is
    // a coarse texel came from the coarse map, one the size of the dark cells from the inner grid.
    if ((uint(ssr.weather[14].w + 0.5) & 32u) != 0u)
    {
        // the cell's STRIDE: light blue 1, yellow 2, orange 4 (interpolated across stride seams)
        const vec2 gl = wxDebugGridLines(v_wpos);
        // (codes: bits 0-1 stride, 4 an apron / closure vertex -> magenta, 8 a sheet texel whose
        // blocked neighbour got no apron -> cyan; interpolated, so the colours blend over triangles)
        const float fail = clamp(v_dbg - 7.0, 0.0, 1.0);
        const float rest = v_dbg >= 7.5 ? v_dbg - 8.0 : v_dbg;
        const float apw = clamp(rest - 3.0, 0.0, 1.0);
        const float sd = rest >= 3.5 ? rest - 4.0 : rest;
        vec3 sc = sd < 0.5 ? vec3(0.55, 0.75, 1.0) : sd < 1.5 ? vec3(1.0, 0.9, 0.2) : vec3(1.0, 0.45, 0.0);
        sc = mix(sc, vec3(1.0, 0.0, 1.0), apw);
        sc = mix(sc, vec3(0.0, 1.0, 1.0), fail);
        albedo = mix(mix(sc, sc * 0.3, gl.x), vec3(1.0, 0.0, 0.0), gl.y);
    }
    outG0 = vec4(albedo, 1.0);
    outG1 = vec4(octEncode(N), octEncode(Ng));
    outG2 = vec4(mix(0.85, 0.3, glint), 0.0, 0.0, 1.0 / 255.0);   // rough 0.85 (a glint 0.3), not metallic, no glow, SURF_PBR (1)
    outG3 = vec4(0.0);
#endif
}
