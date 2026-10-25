#version 460
// vkclean — ray-tracing ACCEPTANCE debug views (RenderVKDebugView 13 / 14), drawn
// fullscreen over the final world image (after tonemap / AA, before selection+HUD).
//
// 13  RT vs RASTER DEPTH HEATMAP: one primary ray per pixel from the camera through
//     the pixel, against the raster depth of the same frame (after pass 2):
//       green = RT hit agrees with the raster surface (within tolerance)
//       red   = RT hit NEARER than raster (geometry in the TLAS the raster lacks, or
//               the raster shows sky where RT hits)
//       blue  = RT hit FARTHER or MISSED (raster geometry missing from the TLAS:
//               unbuilt BLAS, radius cut, wrong LOD / transform)
//       black = sky in both
//     Shaded by the RT hit's facing ratio so structure stays readable. Ray mask =
//     RT_MASK_PRIMARY (no water / alpha-blend: neither writes raster depth).
// 14  INSTANCE VIEW: hash colour of the hit instance id (alpha-mask / blend cutouts
//     via the any-hit), mask = everything but water.
// 15  PROJECTOR VISIBILITY (RenderVKRTDebugFlicker's focus projector): for every
//     raster surface point in front of the projector's near plane, the SAME shadow
//     ray the lit FS traces (projShadowRay: toward the apex, stopped at the near
//     plane, receiver offset, no jitter): white = lit, grey = partly (alpha-blend
//     casters), otherwise the BLOCKING instance's hash colour (the nearest opaque /
//     alpha-passing hit). Outside the light's reach: dark blue. Per-frame counts +
//     the blocker ids go to the stats buffer (binding 8) -> the VKRT log.
// 18  SUN BLOCKER VIEW: for every raster surface point, the hard RT sun ray the shadow
//     mask traces (light-facing facet normal from the depth, the same float-error origin
//     + RenderVKRTShadowBias): white = lit, otherwise the BLOCKING instance's hash colour
//     (compare with view 14 to find which TLAS geometry casts a phantom shadow, and where
//     that geometry really is). STRIPED hash colour: lit for the alpha-tested ray but
//     blocked by the FORCE-OPAQUE one transparent receivers trace (opaque-only: alpha-
//     mask casters solid, no alpha test) -- a caster only alpha-blend surfaces would see.
//     Receivers facing away from the light: dark blue. The screen-centre pixel's
//     blockers go to the stats buffer; LLCleanRTVK logs their flags / material (VKRT).
// Both views apply the raster's back-face rule (single-sided faces seen from behind
// are skipped) and sample alpha at the ray-cone LOD (matching the raster's mip).
layout(set = 1, binding = 0) uniform sampler2D uTextures[];
#define RT_SET 2
#include "clean_rt_common.glsl"
layout(set = 2, binding = 7) uniform sampler2D uRasterDepth;
// View 15 statistics (LLCleanRTVK::mDbgStats, zeroed before the draw):
//   [0] rays  [1] blocked (visibility < 0.01)  [2] partial (0.01 .. 0.99)
//   [3] sum of visibility * 1024   [4..] blocker table: 64 x (instance id + 1, count)
layout(std430, set = 2, binding = 8) buffer RtDbgStats { uint dbg_stats[]; };

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform Push {
    mat4 inv_view_proj;   // VK clip -> world
    vec4 cam_time;        // xyz camera, w time
    vec4 params;          // x view (13/14), y relative tolerance, z absolute tolerance (m), w tmax
    vec4 params2;         // x raster sampler max anisotropy (1 = off); view 15: yzw projector axis
    vec4 params3;         // view 15: xyz projector apex, w near-plane distance from the apex
} pc;

uint hashColorKey(uint x)
{
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}

vec3 hashColor(uint x)
{
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return vec3(float(x & 255u), float((x >> 8) & 255u), float((x >> 16) & 255u)) / 255.0 * 0.8 + 0.2;
}

void main()
{
    rt_anim_time = pc.cam_time.w;
    const vec2 ndc = v_uv * 2.0 - 1.0;
    const vec3 cam = pc.cam_time.xyz;
    vec4 fp = pc.inv_view_proj * vec4(ndc, 1.0, 1.0);
    vec3 dir = normalize(fp.xyz / fp.w - cam);
    // Ray cone for the alpha test's texture LOD: the camera's per-pixel angle (the
    // raster's screen-space derivatives, as a cone). Uniform control flow here.
    rt_cone_spread = max(length(dFdx(dir)), length(dFdy(dir)));
    rt_cone_width0 = 0.0;
    rt_cone_aniso  = max(pc.params2.x, 1.0);

    const float d = textureLod(uRasterDepth, v_uv, 0.0).r;
    const bool rsky = d <= 0.0;   // reversed-Z: far = 0
    float rdist = 1e30;
    if (!rsky)
    {
        vec4 wp = pc.inv_view_proj * vec4(ndc, d, 1.0);
        vec3 P = wp.xyz / wp.w;
        rdist = length(P - cam);
        dir = (P - cam) / max(rdist, 1e-6);   // exactly through the raster surface point
    }
    const int view = int(pc.params.x + 0.5);
    const float tmin = 0.05;

    if (view == 15)
    {
        // params.y = light reach (m), params.z = receiver normal offset (record texel.y)
        const vec3 O = pc.params3.xyz, A = pc.params2.yzw;
        vec3 P = cam + dir * rdist;
        // Flat receiver normal from the reconstructed positions (the lit FS uses the
        // interpolated geometric normal; the same offset rule on top).
        vec3 Ng = normalize(cross(dFdx(P), dFdy(P)));
        const bool reach = !rsky && dot(P - O, A) > pc.params3.w && length(P - O) < pc.params.y;
        if (!reach || any(isnan(Ng))) { outColor = vec4(0.02, 0.03, 0.12, 1.0); return; }
        vec3 toL = O - P;
        if (dot(Ng, toL) < 0.0) Ng = -Ng;
        const vec3 Po = rtReceiverOrigin(P, Ng, rdist, pc.params.z);   // as the lit FS / mask
        toL = O - Po;
        const float len = length(toL);
        const vec3 ldir = toL / max(len, 1e-6);
        const float da = dot(ldir, A), dp = dot(Po - O, A);
        float tmax = (da < -1e-6) ? (pc.params3.w - dp) / da : len;
        tmax = min(tmax, len) - 0.002;
        if (tmax <= 0.0) { outColor = vec4(0.02, 0.03, 0.12, 1.0); return; }
        rt_cone_spread = 0.0;
        rt_cone_width0 = rdist * 0.001;
        rt_cone_aniso  = 1.0;
        const float vis = traceShadowRay(Po, ldir, 0.0, tmax, RT_MASK_SHADOW);
        uint blocker = 0xFFFFFFFFu;
        if (vis < 0.99)
        {
            RtHit h = traceClosestHit(Po, ldir, 0.0, tmax, RT_MASK_SHADOW, false);
            if (h.hit) blocker = h.instance;
        }
        // Stats on a 2x2-pixel lattice (contention); the image uses every pixel.
        const uvec2 px = uvec2(gl_FragCoord.xy);
        if (((px.x | px.y) & 1u) == 0u)
        {
            atomicAdd(dbg_stats[0], 1u);
            if (vis < 0.01) atomicAdd(dbg_stats[1], 1u);
            else if (vis < 0.99) atomicAdd(dbg_stats[2], 1u);
            atomicAdd(dbg_stats[3], uint(clamp(vis, 0.0, 1.0) * 1024.0 + 0.5));
            if (blocker != 0xFFFFFFFFu)
            {
                const uint key = blocker + 1u;
                uint slot = hashColorKey(blocker) & 63u;
                for (int i = 0; i < 16; ++i)
                {
                    const uint prev = atomicCompSwap(dbg_stats[4u + slot * 2u], 0u, key);
                    if (prev == 0u || prev == key) { atomicAdd(dbg_stats[5u + slot * 2u], 1u); break; }
                    slot = (slot + 1u) & 63u;
                }
            }
        }
        vec3 c = blocker == 0xFFFFFFFFu ? vec3(vis) : mix(hashColor(blocker) * 0.7, vec3(1.0), vis);
        outColor = vec4(c, 1.0);
        return;
    }

    if (view == 18)
    {
        // params2.yzw = toward the light, params.z = extra receiver offset (m)
        const vec3 L = normalize(pc.params2.yzw);
        const vec3 P = cam + dir * min(rdist, 1e6);
        vec3 Ng = normalize(cross(dFdx(P), dFdy(P)));   // (uniform control flow: before any return)
        if (rsky) { outColor = vec4(0.0, 0.0, 0.0, 1.0); return; }
        if (any(isnan(Ng))) Ng = -dir;
        if (dot(Ng, -dir) < 0.0) Ng = -Ng;                 // the visible side
        if (dot(Ng, L) <= -0.25) { outColor = vec4(0.02, 0.03, 0.12, 1.0); return; }   // the mask's gate
        if (dot(Ng, L) < 0.0) Ng = -Ng;                    // light-facing (sunShadowRayCore)
        const vec3 Po = rtReceiverOrigin(P, Ng, rdist, pc.params.z);
        rt_cone_width0 = rdist * 0.001;
        rt_cone_aniso  = 1.0;
        const float vis = traceShadowRay(Po, L, 0.0, pc.params.w, RT_MASK_SHADOW);
        uint blocker = 0xFFFFFFFFu;
        if (vis < 0.99)
        {
            RtHit h = traceClosestHit(Po, L, 0.0, pc.params.w, RT_MASK_SHADOW, false);
            if (h.hit) blocker = h.instance;
        }
        // The force-opaque trace (clean_rt_common.glsl traceShadowRayOpaque), nearest hit.
        uint oblocker = 0xFFFFFFFFu;
        {
            rayQueryEXT rq;
            rayQueryInitializeEXT(rq, uTLAS, gl_RayFlagsOpaqueEXT, (RT_MASK_SHADOW & ~RT_MASK_BLEND) & 0xFFu,
                                  Po, 0.0, L, pc.params.w);
            while (rayQueryProceedEXT(rq)) {}
            if (rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT)
                oblocker = rtHitInstance(uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true)),
                                         uint(rayQueryGetIntersectionGeometryIndexEXT(rq, true)));
        }
        vec3 c;
        if (blocker != 0xFFFFFFFFu) c = mix(hashColor(blocker) * 0.7, vec3(1.0), vis);
        else if (oblocker != 0xFFFFFFFFu)
            c = (int(floor((gl_FragCoord.x + gl_FragCoord.y) / 6.0)) & 1) == 0 ? hashColor(oblocker) : vec3(vis);
        else c = vec3(vis);
        // Screen-centre probe (params3.xy = the pixel): its blockers for the VKRT log.
        if (all(equal(uvec2(gl_FragCoord.xy), uvec2(pc.params3.xy))))
        {
            dbg_stats[0] = 1u;
            dbg_stats[1] = blocker + 1u;    // 0 = none (alpha-tested ray)
            dbg_stats[2] = oblocker + 1u;   // 0 = none (force-opaque ray)
            dbg_stats[3] = uint(clamp(vis, 0.0, 1.0) * 1024.0 + 0.5);
        }
        outColor = vec4(c, 1.0);
        return;
    }

    if (view == 14)
    {
        RtHit h = traceClosestHit(cam, dir, tmin, pc.params.w, RT_MASK_SHADOW, true);
        if (!h.hit) { outColor = vec4(0.03, 0.03, 0.05, 1.0); return; }
        float shade = 0.35 + 0.65 * abs(dot(rtHitWorldNormal(h), dir));
        outColor = vec4(hashColor(h.instance) * shade, 1.0);
        return;
    }

    RtHit h = traceClosestHit(cam, dir, tmin, pc.params.w, RT_MASK_PRIMARY, true);
    vec3 c;
    if (rsky && !h.hit)      c = vec3(0.0);
    else if (!h.hit)         c = vec3(0.1, 0.25, 1.0);          // raster surface, RT miss
    else if (rsky)           c = vec3(1.0, 0.1, 0.1);           // RT hit, raster sky
    else
    {
        float tol = max(pc.params.z, pc.params.y * rdist);
        float diff = h.dist - rdist;
        c = abs(diff) <= tol ? vec3(0.1, 1.0, 0.2)
          : (diff < 0.0 ? vec3(1.0, 0.1, 0.1) : vec3(0.1, 0.25, 1.0));
    }
    if (h.hit) c *= 0.35 + 0.65 * abs(dot(rtHitWorldNormal(h), dir));
    outColor = vec4(c, 1.0);
}
