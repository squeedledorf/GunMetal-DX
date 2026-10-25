#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require
// vkclean -- GPU RASTER CURSOR PICK (#702 / #704), stage 2: the fragment list.
//
// The candidates (clean_pick_select.comp) are drawn into a 1x1 target whose pixel centre
// IS the pick ray (LLCleanCursorPick builds the projection around it), with the world's
// own vertex shaders, cull none and NO depth test: every fragment along the ray lands
// here once. Each one inside the request's depth window is counted in the window's depth
// histogram and appended -- not discarded -- with the raster's verdict, so one pass
// serves every pick flavour (the CPU applies LL's exemptions: hit-everything picks,
// invisible faces, special-hover-cursor click zones, media, beacons; llcleancursorpick.cpp):
//
//   facing  single-sided back faces are never drawn (world: the FS / pipeline cull;
//           HUD: its back-culled buckets) -> skipped; water is the water pass's -> skipped.
//   alpha   the draw's rule (clean_prepass_mask.frag): base alpha = material factor.a *
//           tint.a * base texel; MASK cutoff on mask and blend instances (and on opaque
//           ones without the depth pre-pass); alpha BLEND at PickAlphaThreshold; opaque,
//           terrain and invisiprims never test.
//
// SURFACE (#704: what touch, media, face select and the build tools read -- there is no
// CPU narrowphase behind it): the interpolated normal, the vertex stream's tangent
// (v_pick_tangent; screen derivatives of the raw texcoords only where the mesh has none),
// LL's surface ST (v_pick_st: texcoord0 before texgen), the UV the raster samples (planar
// texgen + TE / glTF transform / texture animation) and that UV's per-pixel derivatives
// (one target pixel is one viewport pixel).
//
// Push (the draw family's 16 B block, clean_draw_view.glsl): view = this layer's draw
// view, pad0 = the result entry, pad1 = PICK_LAYER_HUD | the request's mode bits.
#include "clean_frame_info.glsl"   // taaLodBias / maskTestAlpha
#include "clean_draw_view.glsl"    // g_draw_push
#include "clean_pick_common.glsl"
// <GMDX> rez effects: a part a build effect has not built yet is not there to click (the world VS
// modules carry v_rez / v_rez_box; the cluster VS gives none)
#define GMDX_REZFX_FS
#include "gmdx_rezfx.glsl"

layout(location = 0) flat in uint v_material_id;
layout(location = 2)      in vec3 v_world_normal;
layout(location = 3) flat in vec4 v_tint;
layout(location = 4) flat in uint v_flags;
layout(location = 5)      in vec3 v_world_pos;
layout(location = 1)      in vec2 v_uv;
layout(location = 8)      in vec2 v_uv_raw;
layout(location = 12) flat in uint v_inst;
layout(location = 13)      in vec2 v_pick_st;        // raw texcoord0 (before texgen)
layout(location = 14)      in vec4 v_pick_tangent;   // vertex-stream tangent (world / HUD), w sign; 0 = none

// 96 bytes -- LLCleanMaterialStore::Material (clean_draw_common.glsl).
struct Material {
    uvec2 base_color_handle;
    uvec2 normal_handle;
    uvec2 mr_handle;
    uvec2 emissive_handle;
    vec4  base_color_factor;
    vec4  emissive_factor;
    vec4  spec_color_gloss;
    float metallic_factor;
    float roughness_factor;
    float alpha_cutoff;
    uint  flags;
};
layout(std430, set = 0, binding = 1) readonly buffer Materials { Material materials[]; };
layout(set = 1, binding = 0) uniform sampler2D uTextures[];
// Set 2 for the world / rigged / HUD draws; 3 for the cluster draws (-DPICK_OUT_SET=3: the
// cluster set takes 2 in their layout {draw0, draw1, cluster, pick out}).
#ifndef PICK_OUT_SET
#define PICK_OUT_SET 2
#endif
layout(std430, set = PICK_OUT_SET, binding = 0) coherent buffer PickOut { PickEntry entries[]; };

#define PICK_LAYER_HUD    1u   // pad1: this draw is the HUD layer
#define PICK_MODE_PREPASS 2u   // pad1 (request mode): RenderVKDepthPrepass (opaque buckets never cut)

void main()
{
    // Derivatives first, in uniform control flow (the 2x2 quad's helper lanes): the
    // fallback tangent from the raw texcoords, and the sampled UV's footprint.
    const vec3 dpx = dFdx(v_world_pos), dpy = dFdy(v_world_pos);
    const vec2 dsx = dFdx(v_pick_st), dsy = dFdy(v_pick_st);
    const vec2 dux = dFdx(v_uv), duy = dFdy(v_uv);

    const uint mode = g_draw_push.pad1;
    const bool hud = (mode & PICK_LAYER_HUD) != 0u;
    if ((v_flags & 2048u) != 0u) return;                                   // water
    const bool back = !gl_FrontFacing;
    if (back && ((v_flags & 8u) != 0u || (v_flags & 256u) == 0u)) return;   // never drawn

    // The layer's ray and depth window: outside it, not counted and not stored.
    const uint e = g_draw_push.pad0;
    const vec4 org = hud ? entries[e].hdr.h_org : entries[e].hdr.w_org;
    const vec3 dir = hud ? entries[e].hdr.h_dir.xyz : entries[e].hdr.w_dir.xyz;
    const vec2 win = hud ? entries[e].hdr.win.zw : entries[e].hdr.win.xy;
    const float t = dot(v_world_pos - org.xyz, dir);
    if (!(t >= win.x && t < win.y)) return;
    if (!hud && rezfxHidden(v_flags, true)) return;   // <GMDX> rez effects

    // The draw's alpha rule.
    float a = 1.0;
    bool pass = true;
    if ((v_flags & (32u | 65536u)) == 0u && v_material_id < uint(materials.length()))
    {
        const Material m = materials[v_material_id];
        float cutoff = m.alpha_cutoff;
        bool test = true;
        if (cutoff >= 0.0 && (mode & PICK_MODE_PREPASS) != 0u && (v_flags & 6u) == 0u) test = false;
        const bool mask = cutoff >= 0.0;
        if (!mask)
        {
            if ((v_flags & 2u) == 0u) test = false;           // opaque
            else cutoff = entries[e].hdr.w_dir.w;            // alpha blend: the pick threshold
        }
        a = m.base_color_factor.a * v_tint.a;
        float lod = 0.0;
        if ((m.flags & 1u) != 0u)
        {
            // The raster's sample: same UV, the lit FS's TAA mip bias (gradients x 2^bias).
            const uint tex = m.base_color_handle.x;
            const float g = exp2(taaLodBias(v_flags));
            a *= textureGrad(uTextures[nonuniformEXT(tex)], v_uv, dux * g, duy * g).a;
            const vec2 ts = vec2(textureSize(uTextures[nonuniformEXT(tex)], 0));
            const float rho = max(length(dux * ts), length(duy * ts)) * g;
            lod = rho > 0.0 ? log2(rho) : 0.0;
        }
        if (test) pass = (mask ? maskTestAlpha(a, lod, cutoff) : a) >= cutoff;
    }

    vec3 n = v_world_normal;
    const float nl = length(n);
    n = nl > 1e-20 ? n / nl : vec3(0.0, 0.0, 1.0);
    // Tangent: the vertex stream's (LL's volume tangent, as the old CPU pick reported it),
    // re-orthogonalized against the interpolated normal.
    uint fl = (pass ? PICK_FRAG_ALPHA_PASS : 0u) | (back ? PICK_FRAG_BACK_FACE : 0u);
    vec4 tg = vec4(1.0, 0.0, 0.0, 1.0);
    vec3 T = v_pick_tangent.xyz - n * dot(n, v_pick_tangent.xyz);
    float tl = length(T);
    if (tl > 1e-6)
        tg = vec4(T / tl, v_pick_tangent.w < 0.0 ? -1.0 : 1.0);
    else
    {
        // No tangent in the stream: d pos / d s of the raw texcoords, bitangent sign
        // from d pos / d t.
        fl |= PICK_FRAG_DERIV_TAN;
        const float det = dsx.x * dsy.y - dsy.x * dsx.y;
        if (abs(det) > 1e-20)
        {
            T = (dpx * dsy.y - dpy * dsx.y) / det;
            const vec3 B = (dpy * dsx.x - dpx * dsy.x) / det;
            T -= n * dot(n, T);
            tl = length(T);
            if (tl > 1e-12) tg = vec4(T / tl, dot(cross(n, T), B) < 0.0 ? -1.0 : 1.0);
        }
    }

    PickFrag f;
    f.ids = uvec4(v_inst, fl, floatBitsToUint(a), 0u);
    f.pos_t = vec4(v_world_pos, t);
    f.nrm = vec4(n, 0.0);
    f.tng = tg;
    f.st = vec4(v_pick_st, v_uv);
    f.duv = vec4(dux, duy);
    if (hud)
    {
        atomicAdd(entries[e].hdr.hist_h[pickBin(t, win.x, win.y, PICK_HIST_HUD)], 1u);
        const uint k = atomicAdd(entries[e].hdr.n_hud, 1u);
        if (k < PICK_HUD_FRAGS) entries[e].hud[k] = f;
    }
    else
    {
        atomicAdd(entries[e].hdr.hist_w[pickBin(t, win.x, win.y, PICK_HIST_WORLD)], 1u);
        const uint k = atomicAdd(entries[e].hdr.n_world, 1u);
        if (k < PICK_WORLD_FRAGS) entries[e].world[k] = f;
    }
}
