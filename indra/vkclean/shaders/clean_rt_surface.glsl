// vkclean -- RAY-TRACED SURFACE EVALUATION (research P1): the material at a committed
// TLAS hit (instance, triangle, barycentrics), as the raster's lit FS sees it.
//
// Generalizes the alpha test's UV path (clean_rt_common.glsl rtVertexFaceUV /
// rtCandidateAlphaCol / rtTriPositions) into the whole surface: base colour (material x
// tint x texel), shading normal (vertex normal + normal map), ORM or the legacy specular
// map / colour / glossiness / env intensity, emissive, fullbright, terrain (legacy splat
// and PBR records) and water. Everything branches on the SAME instance / material bits
// as clean_draw.frag::shade, so a hit shades like the raster fragment it stands for.
//
// What differs from the raster fragment (documented, by design):
//   * TEXTURE LOD: ray cones (Akenine-Moller et al. 2021) instead of screen derivatives.
//     Footprint width at the hit = rt_cone_width0 + rt_cone_spread * t; each texture's
//     LOD = 0.5 log2(texel area / world area) + log2(footprint / |N.D|) -- the alpha
//     test's rule, applied per map with its own UV-area scale (per-map transforms,
//     terrain detail scale, PBR terrain transforms).
//   * NORMAL MAPS without a vertex tangent (planar texgen, texture animation, terrain,
//     RIGGED hits): the tangent frame comes from the hit TRIANGLE's world edges and UV
//     deltas (the per-triangle form of the FS's derivative cotangent frame).
//   * RIGGED hits: the shading normal interpolates the SKINNED, WELDED vertex normals the RT
//     rig rows carry (rtRigCornerNormal, #859: the normals the rigged raster interpolates;
//     a collapsed entry falls back to the facet), UVs come from the bind-pose vertex stream
//     (rt_geom.y), exactly as the any-hit alpha test reads them. No skinned tangents: normal
//     maps use the triangle's UV frame orthogonalized against that smooth normal.
//   * Terrain hex tiling (SlipstreamRenderTerrainHexTiling) is not applied (plain tiling).
//
// Requires before inclusion: clean_frame_info.glsl + clean_atmos_common.glsl
// (atmosSrgbToLinear, atmosHdrScale), clean_brdf.glsl (srgbEncode), the bindless table
// `uTextures` (set 1, binding 0) and clean_rt_common.glsl (which brings clean_uv_common /
// clean_mat_uv: set 0 binding 16). Declares the terrain records (set 0 binding 22) unless
// RT_SURF_HAVE_TERRAIN_RECORDS is defined.
#ifndef CLEAN_RT_SURFACE_GLSL
#define CLEAN_RT_SURFACE_GLSL
#include "clean_normal_tex.glsl"   // BC5 normal maps: z rebuilt (#263)

#ifndef RT_SURF_HAVE_TERRAIN_RECORDS
// Per-region terrain record (LLCleanMaterialStore::TerrainRecord, 176 B; layout and
// meaning: clean_draw_common.glsl). Index = the terrain instance's skin_id.
struct TerrainRecord {
    uvec4 mat;
    uvec4 misc;
    vec4  legacy;
    vec4  xf[8];
};
layout(std430, set = 0, binding = 22) readonly buffer TerrainRecords { TerrainRecord terrain_records[]; };
#endif

// Shading models (which branch of clean_draw.frag::shade the hit takes).
#define RT_SURF_PBR        0u   // glTF metallic-roughness (material flag 64) / PBR terrain
#define RT_SURF_LEGACY     1u   // LL legacy diffuse + Blinn-Phong (incl. the legacy terrain splat)
#define RT_SURF_FULLBRIGHT 2u   // instance flag 4096 (not terrain): unlit, haze only
#define RT_SURF_WATER      3u   // instance flag 2048: the water surface (opaque capture water)

struct RtSurface {
    vec3  P;          // world position (barycentric on the world triangle)
    vec3  Ng;         // world GEOMETRIC normal, on the side the ray arrived from
    vec3  N;          // shading normal (vertex normal or flat + normal map), raster-oriented
    vec3  Nv;         // interpolated vertex normal, raster-oriented (no normal map)
    vec3  tlift;      // sun shadow-terminator origin lift (#664, Hanika; world, NOT yet
                      // escape-checked -- clean_rt_shade.glsl rtHitSunShadow), 0 = none
    float tw;         // sun grazing-fade width in smooth N.L (#664 round 2,
                      // clean_shadow_rt.glsl rtTerminatorFadeW), 0 = none
    bool  front;      // the raster's gl_FrontFacing for this triangle (rtCandidateFront rule)
    uint  id;         // clean Instance id
    uint  flags;      // instance flags
    uint  mid;        // material id
    uint  model;      // RT_SURF_*
    bool  terrain;
    vec4  base;       // PBR: linear base colour (factor x tint x texel), a = alpha;
                      // legacy: .rgb = base_lin (LL's sRGB texel x colour, linearized)
    vec3  texel;      // the sampled base-colour texel (linear; 1 when untextured)
    // PBR
    float rough, metal, ao;
    vec3  emissive;
    // legacy
    vec3  spec_lin;   // linear specular colour (map x colour)
    float gloss;      // glossiness (specular exponent x normal-map alpha)
    float env;        // env intensity (x specular-map alpha)
    float emask;      // <GMDX> emissive-mask weight (clean_surface_light.glsl SurfaceLit.emask)
    // fullbright
    vec3  fb;         // linear fullbright colour (haze not applied)
    float fb_env;     // fullbright-shiny env intensity (0 = none)
};

// Triangle tangent frame for a normal map (no vertex tangent): T = dP/du, B = dP/dv of
// the hit triangle, scaled together like the FS's cotangent frame (Schueler), applied to
// the tangent-space normal tn around N.
vec3 rtTriPerturbNormal(vec3 N, vec3 e1, vec3 e2, vec2 duv1, vec2 duv2, vec3 tn)
{
    const float det = duv1.x * duv2.y - duv1.y * duv2.x;
    if (abs(det) < 1e-20) return N;
    const float r = 1.0 / det;
    vec3 T = (e1 * duv2.y - e2 * duv1.y) * r;
    vec3 B = (e2 * duv1.x - e1 * duv2.x) * r;
    // Keep the frame in N's tangent plane (the derivative frame is built on N too).
    T -= N * dot(N, T);
    B -= N * dot(N, B);
    const float m = max(dot(T, T), dot(B, B));
    if (!(m > 1e-20)) return N;
    const float inv = inversesqrt(m);
    return normalize(mat3(T * inv, B * inv, N) * tn);
}

// ---- ray-cone texture LOD (per hit; set by rtSurfaceEval before any texture read) ----
float rt_surf_pa   = 0.0;   // 2 x world area of the hit triangle
float rt_surf_foot = 0.0;   // footprint width across the surface (m), 0 = mip 0
// LOD of texture `tex` for a UV mapping whose triangle covers uv_area2 (2 x UV area).
float rtSurfLod(uint tex, float uv_area2)
{
    if (rt_surf_foot <= 0.0 || rt_surf_pa <= 1e-20 || uv_area2 <= 1e-20) return 0.0;
    const vec2 tsz = vec2(textureSize(uTextures[nonuniformEXT(tex)], 0));
    const float lod = 0.5 * log2(uv_area2 * tsz.x * tsz.y / rt_surf_pa) + log2(rt_surf_foot);
    const float levels = float(textureQueryLevels(uTextures[nonuniformEXT(tex)]));
    return clamp(lod, 0.0, max(levels - 1.0, 0.0));
}
vec4 rtSurfTex(uint tex, vec2 uv, float uv_area2)
{
    return textureLod(uTextures[nonuniformEXT(tex)], uv, rtSurfLod(tex, uv_area2));
}

float rtArea2(vec2 a, vec2 b, vec2 c)
{
    const vec2 e1 = b - a, e2 = c - a;
    return abs(e1.x * e2.y - e1.y * e2.x);
}

// Per-map UV of the hit (clean_draw.frag mapUV): the slot's own affine transform of the
// interpolated RAW uv (exact: the transform is affine), else the base UV; texture-
// animated faces (1024): matMapAnimUV. uv_area2 is scaled by the transform's |det|.
vec2 rtSurfMapUV(uint mid, uint flags, int slot, vec2 uv_raw, vec2 uv, float raw_area2, float base_area2,
                 out float area2)
{
    area2 = base_area2;
    // <GMDX> texture-animated: glTF slots take their own transform of the animated uv
    // (matMapAnimUV); legacy maps keep the animated base uv.
    if ((flags & 1024u) != 0u)
    {
        if ((rt_materials[mid].flags & 64u) == 0u || !matMapOwnUV(mid, slot)) return uv;
        const vec4 a0 = mat_uv[mid].rows[slot * 2], a1 = mat_uv[mid].rows[slot * 2 + 1];
        area2 = base_area2 * abs(a0.x * a1.y - a0.y * a1.x);
        return matMapAnimUV(mid, slot, uv, true);
    }
    // </GMDX>
    if (!matMapOwnUV(mid, slot)) return uv;
    const vec4 r0 = mat_uv[mid].rows[slot * 2], r1 = mat_uv[mid].rows[slot * 2 + 1];
    area2 = raw_area2 * abs(r0.x * r1.y - r0.y * r1.x);
    return matMapUV(mid, slot, uv_raw, uv);
}

// Normal map -> shading normal (clean_draw_common.glsl applyNormalMap): LL's per-vertex
// frame when the hit has a vertex tangent (vB = w * cross(vN, vT)), else the hit
// triangle's frame (n0..n2 = the normal map's UV at the triangle's vertices); flipped
// for back faces like the FS (its derivative frame built on the flipped normal yields
// exactly the negated result).
vec3 rtSurfApplyNormalMap(bool front, vec3 nv_raw, vec4 Tw, vec3 e1, vec3 e2,
                          vec2 n0, vec2 n1, vec2 n2, vec3 tn)
{
    vec3 n;
    if (dot(Tw.xyz, Tw.xyz) > 1e-12)
    {
        const vec3 vB = Tw.w * cross(nv_raw, Tw.xyz);
        n = normalize(tn.x * Tw.xyz + tn.y * vB + tn.z * nv_raw);
    }
    else
        n = rtTriPerturbNormal(nv_raw, e1, e2, n1 - n0, n2 - n0, tn);
    return front ? n : -n;
}

// Legacy terrain composition weights (clean_draw.frag terrainWeights).
vec4 rtTerrainWeights(float comp)
{
    const float alpha1     = clamp(1.0 - comp, 0.0, 1.0);
    const float alpha2     = clamp(3.0 - comp, 0.0, 1.0);
    const float alphaFinal = clamp(2.0 - comp, 0.0, 1.0);
    vec4 w = mix(mix(vec4(0.0, 0.0, 0.0, 1.0), vec4(0.0, 0.0, 1.0, 0.0), alpha2),
                 mix(vec4(0.0, 1.0, 0.0, 0.0), vec4(1.0, 0.0, 0.0, 0.0), alpha1), alphaFinal);
    w = max(w - 0.1, vec4(0.0));
    return w / max(w.x + w.y + w.z + w.w, 1e-6);
}

// Evaluate the surface at a committed hit. dir = the (normalized) ray direction, t = the
// hit distance (cone footprint). The hit must be a triangle hit (h.hit).
RtSurface rtSurfaceEval(RtHit h, vec3 dir)
{
    RtSurface s;
    const uint id = h.instance;
    s.id = id;
    s.flags = rt_instances[id].flags;
    s.mid = rt_instances[id].material_id;
    const uint mid = s.mid < uint(rt_materials.length()) ? s.mid : 0u;
    s.mid = mid;
    const RtMaterial m = rt_materials[mid];
    const vec4 tint = rt_instances[id].tint;
    const bool rigged = (rt_geom[id].z & 0x80000000u) != 0u;
    const uint f = rigged ? (s.flags & ~64u) : s.flags;   // rigged: no planar texgen (clean_draw_rigged.vert)
    s.terrain = (s.flags & 32u) != 0u;

    // ---- geometry: world triangle, geometric normal, raster facing ----
    const uvec3 tri = rtTriIndices(id, h.prim);
    const uint  vb  = rt_geom[id].y;
    vec3 p0, p1, p2;
    rtTriPositions(id, h.prim, p0, p1, p2);
    const vec3 w0 = h.o2w * vec4(p0, 1.0), w1 = h.o2w * vec4(p1, 1.0), w2 = h.o2w * vec4(p2, 1.0);
    const vec3 e1 = w1 - w0, e2 = w2 - w0;
    const vec3 b = vec3(1.0 - h.bary.x - h.bary.y, h.bary.x, h.bary.y);
    s.P = w0 * b.x + w1 * b.y + w2 * b.z;
    vec3 ng = cross(e1, e2);
    const float pa = length(ng);
    // (Xid 109) exploded skinned vertices (huge but finite: clean_preskin.comp keeps them) overflow
    // the cross product to Inf, and Inf / Inf is a NaN normal -- then a NaN origin / direction for
    // every secondary ray of the hit. Such a facet takes the ray-facing fallback too.
    ng = (pa > 1e-20 && !isinf(pa)) ? ng / pa : -dir;
    // Raster gl_FrontFacing: counter-clockwise seen from the viewer in the right-handed
    // world = the transformed triangle's normal faces the ray (rtCandidateFront).
    s.front = dot(ng, dir) < 0.0;
    s.Ng = s.front ? ng : -ng;

    // Vertex normal (clean_draw.vert: cofactor of the instance transform, sign(det)).
    // Rigged (#859): the skinned vertex normals in the BLAS space (pelvis-local, #632 / the RT
    // origin frame for rigged clusters), rotated by the hit's object-to-world -- as
    // rtTerminatorTri reads them; oriented with the facet (the raster's winding rule).
    const mat3 M3 = mat3(h.o2w);
    const float det = dot(M3[0], cross(M3[1], M3[2]));
    vec3 nv_raw = ng;   // unflipped
    vec3 n_obj = vec3(0.0, 0.0, 1.0);
    vec4 t_obj = vec4(0.0);
    if (rigged)
    {
        const vec3 rn = M3 * (rtRigCornerNormal(id, h.prim, 0u, tri.x) * b.x
                            + rtRigCornerNormal(id, h.prim, 1u, tri.y) * b.y
                            + rtRigCornerNormal(id, h.prim, 2u, tri.z) * b.z);
        const float rl = dot(rn, rn);
        // (an invalid / collapsed row, or one on the hidden side of its facet: the facet)
        if (rl > 1e-20 && !any(isnan(rn)) && dot(rn, ng) * inversesqrt(rl) > 0.05) nv_raw = rn * inversesqrt(rl);
    }
    else
    {
        n_obj = rtVertNormal(vb + tri.x) * b.x + rtVertNormal(vb + tri.y) * b.y + rtVertNormal(vb + tri.z) * b.z;
        const vec4 tg0 = rtVertTangent(vb + tri.x), tg1 = rtVertTangent(vb + tri.y), tg2 = rtVertTangent(vb + tri.z);
        t_obj.xyz = tg0.xyz * b.x + tg1.xyz * b.y + tg2.xyz * b.z;
        t_obj.w = tg0.w;   // handedness (constant across a triangle)
        const mat3 Ncof = mat3(cross(M3[1], M3[2]), cross(M3[2], M3[0]), cross(M3[0], M3[1]));
        const vec3 nw = (det < 0.0 ? -1.0 : 1.0) * (Ncof * n_obj);
        if (dot(nw, nw) > 1e-20) nv_raw = normalize(nw);
    }
    s.Nv = s.front ? nv_raw : -nv_raw;
    s.N = s.Nv;
    // SHADOW TERMINATOR (#664; clean_shadow_rt.glsl rtTerminatorTri, Hanika): the exact
    // lift from this very hit triangle -- no camera probe -- on the side the ray arrived
    // from, only in the sun's band (the facet near grazing; RenderVKRTShadowTerminatorOffset).
    // The grazing fade for rigged hits too since their shading normal is smooth (#859).
    s.tlift = vec3(0.0);
    s.tw = 0.0;
    if (rtTerminatorOn() && specSunOn() && rtTerminatorBand(dot(s.Ng, atmosLightDir())))
    {
        vec3 tns, tng, tph;
        if (!rtTerminatorTri(id, h.prim, b, h.o2w, -dir, s.tlift, tns, tng, tph)) s.tlift = vec3(0.0);
        else s.tw = rtTerminatorFadeW(rtGrazingFadeK(), rt_tri_dev);
    }

    // ---- UVs: base (texgen + TE / glTF transform / animation) and raw (texgen only) ----
    const mat4 model = rt_instances[id].model_matrix;
    const QvHeader qh = rtHdr(vb);   // the static slot's decode header (#573)
    const vec2 u0 = rtVertexFaceUV(id, f, qh, vb + tri.x);
    const vec2 u1 = rtVertexFaceUV(id, f, qh, vb + tri.y);
    const vec2 u2 = rtVertexFaceUV(id, f, qh, vb + tri.z);
    const vec2 uv = u0 * b.x + u1 * b.y + u2 * b.z;
    vec2 r0, r1, r2;
    if (s.terrain)
    {
        // Terrain: region-local XY is the detail / PBR UV basis (clean_draw.vert).
        r0 = rtVertPos(qh, vb + tri.x).xy; r1 = rtVertPos(qh, vb + tri.y).xy; r2 = rtVertPos(qh, vb + tri.z).xy;
    }
    else
    {
        r0 = cleanFaceRawUV(f, rtVertUV(qh, vb + tri.x), rtVertNormal(vb + tri.x), rtVertPos(qh, vb + tri.x), model);
        r1 = cleanFaceRawUV(f, rtVertUV(qh, vb + tri.y), rtVertNormal(vb + tri.y), rtVertPos(qh, vb + tri.y), model);
        r2 = cleanFaceRawUV(f, rtVertUV(qh, vb + tri.z), rtVertNormal(vb + tri.z), rtVertPos(qh, vb + tri.z), model);
    }
    const vec2 uv_raw = r0 * b.x + r1 * b.y + r2 * b.z;
    const float base_a2 = rtArea2(u0, u1, u2);
    const float raw_a2  = rtArea2(r0, r1, r2);

    // ---- ray-cone footprint ----
    rt_surf_pa = pa;
    {
        const float width = rt_cone_width0 + rt_cone_spread * h.dist;
        const float ndotd = max(abs(dot(ng, dir)), 1e-4);
        const float major = width / ndotd;
        rt_surf_foot = (width > 0.0) ? ((rt_cone_aniso > 1.0) ? max(width, major / rt_cone_aniso) : major) : 0.0;
    }

    // ---- defaults ----
    s.model = RT_SURF_LEGACY;
    s.base = m.base_color_factor * tint;
    s.texel = vec3(1.0);
    s.rough = 1.0; s.metal = 0.0; s.ao = 1.0;
    s.emissive = vec3(0.0);
    s.spec_lin = vec3(0.0); s.gloss = 0.0; s.env = 0.0;
    s.emask = 0.0;   // <GMDX>
    s.fb = vec3(0.0); s.fb_env = 0.0;

    // WATER (clean_water.frag WATER_OPAQUE): shaded from the frame's water block alone.
    if ((s.flags & 2048u) != 0u)
    {
        s.model = RT_SURF_WATER;
        s.base = vec4(1.0);
        return s;
    }

    float texel_a = 1.0;   // <GMDX> the base texel's alpha (the emissive mask)
    bool tpbr = false;
    vec3 t_orm = vec3(1.0), t_emis = vec3(0.0), t_N = s.N;
    const uint trec = rt_instances[id].skin_id;
    const bool t_rec_ok = s.terrain && trec < uint(terrain_records.length());
    if (s.terrain && t_rec_ok && terrain_records[trec].misc.y == 1u)
    {
        // PBR TERRAIN (clean_draw.frag terrainPBR, plain tiling): four glTF materials,
        // each at its own transform of the region-local XY, blended by the composition.
        const TerrainRecord tr = terrain_records[trec];
        const vec4 w = (tr.misc.w == 1u) ? vec4(1.0, 0.0, 0.0, 0.0) : rtTerrainWeights(uv.x);
        const int detail = int(tr.misc.z);
        vec3 col = vec3(0.0), orm = vec3(0.0), emis = vec3(0.0), Nmix = vec3(0.0);
        for (int i = 0; i < 4; ++i)
        {
            const float wi = w[i];
            if (wi <= 0.0 || tr.mat[i] >= uint(rt_materials.length())) continue;
            const RtMaterial mt = rt_materials[tr.mat[i]];
            const vec4 x0 = tr.xf[2 * i], x1 = tr.xf[2 * i + 1];
            const vec2 tuv = vec2(dot(x0.xyz, vec3(uv_raw, 1.0)), dot(x1.xyz, vec3(uv_raw, 1.0)));
            const float a2 = raw_a2 * abs(x0.x * x1.y - x0.y * x1.x);
            vec4 c = mt.base_color_factor;
            if ((mt.flags & 1u) != 0u) c *= rtSurfTex(mt.base_color_handle.x, tuv, a2);
            vec3 o = vec3(1.0, 1.0, 0.0);
            if (detail >= -3)
            {
                o = vec3(1.0, mt.roughness_factor, mt.metallic_factor);
                if ((mt.flags & 4u) != 0u)
                {
                    const vec3 tt = rtSurfTex(mt.mr_handle.x, tuv, a2).rgb;
                    o *= (detail >= -1) ? tt : vec3(1.0, tt.g, tt.b);
                }
            }
            vec3 n = s.N;
            if (detail >= -2 && (mt.flags & 2u) != 0u)
            {
                const vec3 tn = cleanNormalTex(rtSurfTex(mt.normal_handle.x, tuv, a2).xyz);
                const vec2 m0 = vec2(dot(x0.xyz, vec3(r0, 1.0)), dot(x1.xyz, vec3(r0, 1.0)));
                const vec2 m1 = vec2(dot(x0.xyz, vec3(r1, 1.0)), dot(x1.xyz, vec3(r1, 1.0)));
                const vec2 m2 = vec2(dot(x0.xyz, vec3(r2, 1.0)), dot(x1.xyz, vec3(r2, 1.0)));
                n = rtSurfApplyNormalMap(s.front, nv_raw, vec4(0.0), e1, e2, m0, m1, m2, tn);
            }
            vec3 e = vec3(0.0);
            if (detail >= 0)
            {
                e = mt.emissive_factor.rgb;
                if ((mt.flags & 8u) != 0u) e *= rtSurfTex(mt.emissive_handle.x, tuv, a2).rgb;
            }
            col  += wi * c.rgb;
            orm  += wi * o;
            emis += wi * e;
            Nmix += wi * n;
        }
        t_N = (dot(Nmix, Nmix) > 1e-12) ? normalize(Nmix) : s.N;
        t_orm = orm; t_emis = emis;
        s.base = vec4(col, 1.0);
        tpbr = true;
    }
    else if (s.terrain)
    {
        // LEGACY TERRAIN SPLAT: the 4 detail textures in the material's handle slots,
        // detail UV = region-local XY * scale + the region's offset, blended by the
        // per-vertex composition (base uv.x).
        const vec4 lg = t_rec_ok ? terrain_records[trec].legacy : vec4(0.0, 0.0, 1.0 / 12.0, 0.0);
        const vec2 d_uv = uv_raw * lg.z + lg.xy;
        const float a2 = raw_a2 * lg.z * lg.z;
        const vec3 c0 = rtSurfTex(m.base_color_handle.x, d_uv, a2).rgb;
        const vec3 c1 = rtSurfTex(m.normal_handle.x,     d_uv, a2).rgb;
        const vec3 c2 = rtSurfTex(m.mr_handle.x,         d_uv, a2).rgb;
        const vec3 c3 = rtSurfTex(m.emissive_handle.x,   d_uv, a2).rgb;
        const float comp = uv.x;
        const float alpha1     = clamp(1.0 - comp, 0.0, 1.0);
        const float alpha2     = clamp(3.0 - comp, 0.0, 1.0);
        const float alphaFinal = clamp(2.0 - comp, 0.0, 1.0);
        s.base = vec4(mix(mix(c3, c2, alpha2), mix(c1, c0, alpha1), alphaFinal), 1.0);
    }
    else if ((m.flags & 1u) != 0u)
    {
        const vec4 tx = rtSurfTex(m.base_color_handle.x, uv, base_a2);
        s.texel = tx.rgb;
        texel_a = tx.a;   // <GMDX>
        s.base *= tx;
    }

    // ---- FULLBRIGHT (flag 4096, not terrain): LL fullbrightF ----
    if ((s.flags & 4096u) != 0u && !s.terrain)
    {
        s.model = RT_SURF_FULLBRIGHT;
        const vec3 c = srgbEncode(s.texel) * m.base_color_factor.rgb * tint.rgb;
        s.fb = atmosSrgbToLinear(c) * atmosHdrScale();
        // Fullbright + shiny (opaque legacy faces only, as the FS).
        s.fb_env = ((m.flags & 16u) != 0u && (s.flags & 6u) == 0u) ? m.emissive_factor.a : 0.0;
        return s;
    }

    // Normal map tangent frame inputs (non-terrain): the vertex tangent in the normal
    // map's own UV frame (clean_draw.vert), else the hit triangle's frame.
    float n_area;
    const vec2 nuv = rtSurfMapUV(mid, s.flags, MAP_NORMAL, uv_raw, uv, raw_a2, base_a2, n_area);
    vec4 Tw = vec4(0.0);
    if (!rigged && !s.terrain)
    {
        const mat4 nuvm = matMapOwnUV(mid, MAP_NORMAL) ? matMapUVMatrix(mid, MAP_NORMAL)
                                                       : rt_instances[id].uv_matrix;
        const vec4 tl = cleanFaceTangent(s.flags, nuvm, n_obj, t_obj);
        const vec3 wt = M3 * tl.xyz;
        Tw = (dot(wt, wt) > 1e-16) ? vec4(normalize(wt), det < 0.0 ? -tl.w : tl.w) : vec4(0.0);
    }
    // The normal map's UV at the triangle's vertices (the triangle frame's UV deltas).
    vec2 nm0 = u0, nm1 = u1, nm2 = u2;
    if ((s.flags & 1024u) == 0u && matMapOwnUV(mid, MAP_NORMAL))
    {
        nm0 = matMapUV(mid, MAP_NORMAL, r0, u0);
        nm1 = matMapUV(mid, MAP_NORMAL, r1, u1);
        nm2 = matMapUV(mid, MAP_NORMAL, r2, u2);
    }
    else if ((s.flags & 1024u) != 0u && (m.flags & 64u) != 0u)   // <GMDX> animated glTF (matMapAnimUV)
    {
        nm0 = matMapAnimUV(mid, MAP_NORMAL, u0, true);
        nm1 = matMapAnimUV(mid, MAP_NORMAL, u1, true);
        nm2 = matMapAnimUV(mid, MAP_NORMAL, u2, true);
    }

    const bool gltf = (m.flags & 64u) != 0u;
    if (tpbr || (!s.terrain && gltf))
    {
        s.model = RT_SURF_PBR;
        if (tpbr) s.N = t_N;
        else if ((m.flags & 2u) != 0u)
        {
            const vec3 tn = cleanNormalTex(rtSurfTex(m.normal_handle.x, nuv, n_area).xyz);
            s.N = rtSurfApplyNormalMap(s.front, nv_raw, Tw, e1, e2, nm0, nm1, nm2, tn);
        }
        s.rough = tpbr ? t_orm.g : m.roughness_factor;
        s.metal = tpbr ? t_orm.b : m.metallic_factor;
        s.ao    = tpbr ? t_orm.r : 1.0;
        if (!tpbr && (m.flags & 4u) != 0u)
        {
            float a2;
            const vec2 muv = rtSurfMapUV(mid, s.flags, MAP_MR, uv_raw, uv, raw_a2, base_a2, a2);
            const vec3 mr = rtSurfTex(m.mr_handle.x, muv, a2).rgb;
            s.ao = mr.r; s.rough *= mr.g; s.metal *= mr.b;
        }
        s.rough = clamp(s.rough, 0.0, 1.0);
        s.metal = clamp(s.metal, 0.0, 1.0);
        s.emissive = tpbr ? t_emis : m.emissive_factor.rgb;
        if (atmosDropEmissive(s.flags)) s.emissive = vec3(0.0);   // vintage mode
        else if (!tpbr && (m.flags & 8u) != 0u)
        {
            float a2;
            const vec2 euv = rtSurfMapUV(mid, s.flags, MAP_EMISSIVE, uv_raw, uv, raw_a2, base_a2, a2);
            s.emissive *= rtSurfTex(m.emissive_handle.x, euv, a2).rgb;
        }
        return s;
    }

    // ---- LEGACY (cleanSoftenF legacy branch inputs) ----
    s.model = RT_SURF_LEGACY;
    const vec4 base_alpha = s.base;
    s.base = vec4(s.terrain ? s.base.rgb
                            : atmosSrgbToLinear(srgbEncode(s.texel) * m.base_color_factor.rgb * tint.rgb),
                  base_alpha.a);
    float nmap_a = 1.0;
    const uint bump = legacyBumpCode(m.flags);
    if (!s.terrain && bump != 0u)
    {
        // Legacy bump preset (#19): clean_draw.frag legacyBumpTN at this hit's LOD, on the
        // base-colour UV and triangle frame.
        const bool lum = bump <= 2u;
        if ((m.flags & (lum ? 1u : 2u)) != 0u)
        {
            const uint th = lum ? m.base_color_handle.x : m.normal_handle.x;
            const vec2 st = 1.0 / vec2(max(textureSize(uTextures[nonuniformEXT(th)], 0), ivec2(1)));
            const vec4 xm = rtSurfTex(th, uv - vec2(st.x, 0.0), base_a2);
            const vec4 xp = rtSurfTex(th, uv + vec2(st.x, 0.0), base_a2);
            const vec4 ym = rtSurfTex(th, uv - vec2(0.0, st.y), base_a2);
            const vec4 yp = rtSurfTex(th, uv + vec2(0.0, st.y), base_a2);
            const vec3 tn = lum
                ? legacyBumpNormal(legacyBumpLum(srgbEncode(xm.rgb), bump), legacyBumpLum(srgbEncode(xp.rgb), bump),
                                   legacyBumpLum(srgbEncode(ym.rgb), bump), legacyBumpLum(srgbEncode(yp.rgb), bump), bump)
                : legacyBumpNormal(xm.a, xp.a, ym.a, yp.a, bump);
            s.N = rtSurfApplyNormalMap(s.front, nv_raw, Tw, e1, e2, u0, u1, u2, tn);
        }
    }
    else if (!s.terrain && (m.flags & 2u) != 0u)
    {
        const vec4 nt = rtSurfTex(m.normal_handle.x, nuv, n_area);
        nmap_a = nt.a;
        const vec3 tn = normalize(cleanNormalTex(nt.xyz));
        s.N = rtSurfApplyNormalMap(s.front, nv_raw, Tw, e1, e2, nm0, nm1, nm2, tn);
    }
    vec4 spec_tex = vec4(1.0);
    if (!s.terrain && (m.flags & 4u) != 0u)
    {
        float a2;
        const vec2 suv = rtSurfMapUV(mid, s.flags, MAP_MR, uv_raw, uv, raw_a2, base_a2, a2);
        spec_tex = rtSurfTex(m.mr_handle.x, suv, a2);
    }
    s.gloss = m.spec_color_gloss.a * nmap_a;
    s.spec_lin = atmosSrgbToLinear(spec_tex.rgb * m.spec_color_gloss.rgb);
    s.env = ((m.flags & 16u) != 0u) ? m.emissive_factor.a * spec_tex.a : 0.0;
    // <GMDX> emissive mask (clean_draw.frag decodeSurface): the texel alpha, not alpha-blended.
    if ((m.flags & 128u) != 0u && (s.flags & 2u) == 0u && !s.terrain) s.emask = texel_a;
    return s;
}

#endif
