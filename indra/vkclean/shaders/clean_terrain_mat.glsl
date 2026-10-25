// vkclean -- TERRAIN MATERIAL CODE (instance flag bit5): the blended terrain surface of a
// region-local XY, shared by every consumer that must produce the SAME surface:
//   * clean_draw.frag (forward lit FS, G-buffer FS and its terrain-only split variant);
//   * clean_terrain_bake.comp (RenderVKTerrainBake, #811: the per-region coarse bake, which
//     runs these functions at bake resolution so the far terrain matches the live path).
// Needs (declared by clean_draw_common.glsl): Material / materials (set 0 binding 1),
// uTextures (set 1), TerrainRecord / terrain_records (set 0 binding 22), cleanNormalTex
// (clean_normal_tex.glsl), ssr (clean_frame_info.glsl: render_opts.y, the hex setting).
// The TAA mip bias is the CALLER's (gs = exp2(bias) scales the explicit gradients): the
// bake samples unbiased.
// Hex tiling: terrainHex() below -- a constant in the split / probe builds, the run-time
// setting elsewhere, CLEAN_TERRAIN_HEX_OVERRIDE (an expression) in the bake compute.
#ifndef CLEAN_TERRAIN_MAT_GLSL
#define CLEAN_TERRAIN_MAT_GLSL

// ---- TERRAIN (instance flag bit5) ----
// Composition weights for the 4 detail slots from the per-vertex composition coord
// (v_uv.x, ~0..3), on the same alpha-ramp terms the legacy splat blends with: LL
// pbrterrainUtilF get_terrain_mix_weights -- drop weights under the 0.1 mix
// threshold, renormalize (HEIGHTMAP_WITH_NOISE paint type).
vec4 terrainWeights(float comp)
{
    const float alpha1     = clamp(1.0 - comp, 0.0, 1.0);
    const float alpha2     = clamp(3.0 - comp, 0.0, 1.0);
    const float alphaFinal = clamp(2.0 - comp, 0.0, 1.0);
    vec4 w = mix(mix(vec4(0.0, 0.0, 0.0, 1.0), vec4(0.0, 0.0, 1.0, 0.0), alpha2),
                 mix(vec4(0.0, 1.0, 0.0, 0.0), vec4(1.0, 0.0, 0.0, 0.0), alpha1), alphaFinal);
    w = max(w - 0.1, vec4(0.0));
    return w / max(w.x + w.y + w.z + w.w, 1e-6);
}

// perturbNormal (cotangent frame, Schueler) on EXPLICIT gradients: terrain has no
// tangents, and each PBR material has its own UV transform sampled in divergent
// control flow (weight-gated), so the screen gradients are taken up front.
vec3 perturbNormalGrad(vec3 N, vec3 dp1, vec3 dp2, vec2 duv1, vec2 duv2, vec3 tn)
{
    vec3 dp2perp = cross(dp2, N);
    vec3 dp1perp = cross(N, dp1);
    vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    float mm = max(dot(T, T), dot(B, B));
    if (!(mm > 1e-20)) return N;
    float inv = inversesqrt(mm);
    return normalize(mat3(T * inv, B * inv, N) * tn);
}

// ---- TERRAIN HEX TILING (SlipstreamRenderTerrainHexTiling, ssr.render_opts.y) ----
// After upstream Alchemy (a75a44658b / 94b2a47efe), which follows Mikkelsen, "Practical
// Real-Time Hex-Tiling", JCGT 11(2), 2022. A lattice of equilateral triangles covers the
// uv plane; a fragment's triangle has the centres of three hexagonal cells as corners.
// Each cell shows the texture at its own offset and rotation; the three blend by the
// barycentrics raised to a power (a narrow band, so contrast survives), with the brighter
// sample taking a little more than its share. Nothing is precomputed from the texture.
// Gradients go through each cell's rotation with the uv (explicit, so branching around
// the fetch is safe); a normal map's xy turn back by the inverse rotation and blend as
// slopes. A cell under the threshold is not fetched.
#define HEX_EXPONENT   7.0
#define HEX_FALLOFF    0.6
#define HEX_GRID_SCALE 3.4641016   // 2 sqrt(3): about three cells per texture repeat
#define HEX_THRESHOLD  0.01
struct HexTile { ivec2 corner[3]; vec3 weight; };
#if defined(CLEAN_GBPROBE_NO_HEX)
bool terrainHex() { return false; }
#elif defined(CLEAN_TERRAIN_HEX_OVERRIDE)
bool terrainHex() { return CLEAN_TERRAIN_HEX_OVERRIDE; }   // bake compute: the job's setting
#elif defined(CLEAN_GBUFFER_TERRAIN)
// The terrain G-buffer pipelines (RenderVKGbufTerrainSplit) are built twice, hex tiling off /
// on (constant 2: 0 / 1), and the renderer binds the one matching the setting, so terrain
// without hex tiling never carries its registers. -1 (default): the run-time test.
layout(constant_id = 2) const int kTerrainHex = -1;
bool terrainHex() { return kTerrainHex < 0 ? ssr.render_opts.y > 0.5 : kTerrainHex != 0; }
#else
bool terrainHex() { return ssr.render_opts.y > 0.5; }
#endif
uint hexHash(ivec2 corner)   // lowbias32 (Wellons): integer, identical on every GPU
{
    uvec2 u = uvec2(corner + ivec2(1 << 20));
    uint h = u.x * 0x8da6b343u + u.y * 0xd8163841u;
    h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
    return h;
}
HexTile hexTile(vec2 uv)
{
    const vec2 skewed = mat2(1.0, 0.0, -0.57735027, 1.15470054) * (uv * HEX_GRID_SCALE);
    const ivec2 base = ivec2(floor(skewed));
    const vec2 f = fract(skewed);
    const int upper = int(step(1.0, f.x + f.y));
    const float flip = float(upper) * 2.0 - 1.0;
    HexTile ht;
    ht.corner[0] = base + ivec2(upper);
    ht.corner[1] = base + ivec2(upper, 1 - upper);
    ht.corner[2] = base + ivec2(1 - upper, upper);
    vec3 w = max(vec3(0.0), vec3((f.x + f.y - 1.0) * flip, float(upper) - f.y * flip, float(upper) - f.x * flip));
    w = pow(w, vec3(HEX_EXPONENT));
    w /= max(w.x + w.y + w.z, 1e-12);
    w = max(w - HEX_THRESHOLD, vec3(0.0));
    ht.weight = w / max(w.x + w.y + w.z, 1e-12);
    return ht;
}
// A cell's sample position (turned about its centre, shifted); returns the rotation.
mat2 hexCell(ivec2 corner, vec2 uv, out vec2 st)
{
    const uint h = hexHash(corner);
    const vec2 offset = vec2(h & 0x7ffu, (h >> 11) & 0x7ffu) * (1.0 / 2048.0);
    const float angle = (float(h >> 22) * (1.0 / 512.0) - 1.0) * 3.14159265;
    const float c = cos(angle), s = sin(angle);
    const mat2 rot = mat2(c, s, -s, c);
    const vec2 centre = mat2(1.0, 0.0, 0.5, 0.8660254) * vec2(corner) * (1.0 / HEX_GRID_SCALE);
    st = rot * (uv - centre) + centre + offset;
    return rot;
}
vec3 hexWeights(vec3 w, vec3 luma)
{
    w *= mix(vec3(1.0), luma, HEX_FALLOFF);
    return w / max(w.x + w.y + w.z, 1e-12);
}
float hexLuma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
// One texture, hex-tiled when enabled (legacy terrain detail slots).
vec3 terrainTex(uint handle, vec2 uv, vec2 g1, vec2 g2)
{
    if (!terrainHex()) return textureGrad(uTextures[nonuniformEXT(handle)], uv, g1, g2).rgb;
    const HexTile ht = hexTile(uv);
    vec3 cell[3], luma = vec3(0.0);
    [[unroll]] for (int k = 0; k < 3; ++k)   // constant cell / corner indices (registers)
    {
        cell[k] = vec3(0.0);
        if (!(ht.weight[k] <= 0.0))
        {
            vec2 st;
            const mat2 rot = hexCell(ht.corner[k], uv, st);
            cell[k] = textureGrad(uTextures[nonuniformEXT(handle)], st, rot * g1, rot * g2).rgb;
            luma[k] = hexLuma(cell[k]);
        }
    }
    const vec3 w = hexWeights(ht.weight, luma);
    return cell[0] * w.x + cell[1] * w.y + cell[2] * w.z;
}

// One PBR terrain material at uv (explicit gradients): base colour (factor applied),
// ORM, tangent-space normal as a SLOPE (xy / z; 0 = flat) and emissive.
void terrainMatFetch(Material mt, int detail, vec2 uv, vec2 g1, vec2 g2,
                     out vec4 c, out vec3 o, out vec2 slope, out vec3 e)
{
    c = mt.base_color_factor;
    if ((mt.flags & 1u) != 0u)
        c *= textureGrad(uTextures[nonuniformEXT(mt.base_color_handle.x)], uv, g1, g2);
    o = vec3(1.0, 1.0, 0.0);   // LL below METALLIC_ROUGHNESS: matte, non-metal
    if (detail >= -3)
    {
        o = vec3(1.0, mt.roughness_factor, mt.metallic_factor);
        if ((mt.flags & 4u) != 0u)
        {
            vec3 t = textureGrad(uTextures[nonuniformEXT(mt.mr_handle.x)], uv, g1, g2).rgb;
            o *= (detail >= -1) ? t : vec3(1.0, t.g, t.b);
        }
    }
    slope = vec2(0.0);
    if (detail >= -2 && (mt.flags & 2u) != 0u)
    {
        vec3 tn = cleanNormalTex(textureGrad(uTextures[nonuniformEXT(mt.normal_handle.x)], uv, g1, g2).xyz);
        slope = tn.xy / max(tn.z, 0.015625);
    }
    e = vec3(0.0);
    if (detail >= 0)
    {
        e = mt.emissive_factor.rgb;
        if ((mt.flags & 8u) != 0u)
            e *= textureGrad(uTextures[nonuniformEXT(mt.emissive_handle.x)], uv, g1, g2).rgb;
    }
}

// PBR TERRAIN (LL pbrterrainF): the record's 4 glTF materials, each sampled at its own
// transform of the region-local XY and multiplied by its factors, blended by `w`.
// rec: the TerrainRecord index -- read in place, not copied (a by-value record's xf[2i]
// indexed by the run-time slot i would sit in local memory).
// ORM = (occlusion, roughness, metallic); missing maps read white (flat normal).
// RenderTerrainPBRDetail (misc.z) drops features like LL: < 0 no emissive, < -1 no
// occlusion, < -2 no normal map, < -3 no metallic-roughness (rough 1, metal 0).
// gs: the caller's mip-bias gradient scale (exp2(taaLodBias): textureGrad takes no bias).
void terrainPBR(uint rec, vec4 w, vec2 xy, vec2 dxy1, vec2 dxy2, vec3 dp1, vec3 dp2,
                vec3 Ng, float gs, out vec3 col, out vec3 orm, out vec3 emis, out vec3 Nmix)
{
    const int detail = int(terrain_records[rec].misc.z);
    col = vec3(0.0); orm = vec3(0.0); emis = vec3(0.0); Nmix = vec3(0.0);
    for (int i = 0; i < 4; ++i)
    {
        const float wi = w[i];
        const uint mi = terrain_records[rec].mat[i];
        if (wi <= 0.0 || mi >= uint(materials.length())) continue;
        const Material mt = materials[mi];
        const vec4 r0 = terrain_records[rec].xf[2 * i], r1 = terrain_records[rec].xf[2 * i + 1];
        const vec2 uv = vec2(dot(r0.xyz, vec3(xy, 1.0)), dot(r1.xyz, vec3(xy, 1.0)));
        const mat2 A  = mat2(r0.x, r1.x, r0.y, r1.y);
        // TAA mip bias: explicit gradients scaled by 2^bias (textureGrad takes no bias).
        const vec2 g1 = A * dxy1 * gs, g2 = A * dxy2 * gs;

        vec4 c; vec3 o, e; vec2 slope;
        if (!terrainHex())
            terrainMatFetch(mt, detail, uv, g1, g2, c, o, slope, e);
        else
        {
            // Every map of the material blends by the one set of cell weights (from
            // the base colour), so they stay one surface.
            const HexTile ht = hexTile(uv);
            vec4 cc[3]; vec3 oc[3], ec[3]; vec2 sc[3]; vec3 luma = vec3(0.0);
            [[unroll]] for (int k = 0; k < 3; ++k)   // constant cell indices (registers)
            {
                cc[k] = vec4(0.0); oc[k] = vec3(0.0); ec[k] = vec3(0.0); sc[k] = vec2(0.0);
                if (!(ht.weight[k] <= 0.0))
                {
                    vec2 st;
                    const mat2 rot = hexCell(ht.corner[k], uv, st);
                    terrainMatFetch(mt, detail, st, rot * g1, rot * g2, cc[k], oc[k], sc[k], ec[k]);
                    sc[k] = sc[k] * rot;   // the cell's normal turns back with its texture
                    luma[k] = hexLuma(cc[k].rgb);
                }
            }
            const vec3 hw = hexWeights(ht.weight, luma);
            c = cc[0] * hw.x + cc[1] * hw.y + cc[2] * hw.z;
            o = oc[0] * hw.x + oc[1] * hw.y + oc[2] * hw.z;
            e = ec[0] * hw.x + ec[1] * hw.y + ec[2] * hw.z;
            slope = sc[0] * hw.x + sc[1] * hw.y + sc[2] * hw.z;
        }
        vec3 n = Ng;
        if (detail >= -2 && (mt.flags & 2u) != 0u)
            n = perturbNormalGrad(Ng, dp1, dp2, g1, g2, vec3(slope, 1.0));
        col  += wi * c.rgb;
        orm  += wi * o;
        emis += wi * e;
        Nmix += wi * n;
    }
    Nmix = (dot(Nmix, Nmix) > 1e-12) ? normalize(Nmix) : Ng;
}

// LEGACY TERRAIN SPLAT: the 4 detail textures (packed in the instance material's base /
// normal / mr / emissive handle slots, detail_0..3, low -> high band) at the detail UV
// d_uv (explicit gradients dg1 / dg2, the caller's mip bias folded in), blended by the
// composition coord comp (~0..3) on LL's alpha_ramp (inverted clamp form). Linear rgb.
vec3 terrainLegacy(Material m, float comp, vec2 d_uv, vec2 dg1, vec2 dg2)
{
    vec3 c0 = terrainTex(m.base_color_handle.x, d_uv, dg1, dg2);
    vec3 c1 = terrainTex(m.normal_handle.x,     d_uv, dg1, dg2);
    vec3 c2 = terrainTex(m.mr_handle.x,         d_uv, dg1, dg2);
    vec3 c3 = terrainTex(m.emissive_handle.x,   d_uv, dg1, dg2);
    float alpha1     = clamp(1.0 - comp, 0.0, 1.0);
    float alpha2     = clamp(3.0 - comp, 0.0, 1.0);
    float alphaFinal = clamp(2.0 - comp, 0.0, 1.0);
    return mix(mix(c3, c2, alpha2), mix(c1, c0, alpha1), alphaFinal);
}

#endif   // CLEAN_TERRAIN_MAT_GLSL
