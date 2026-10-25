// vkclean -- TERRAIN BAKE TABLE (#811, RenderVKTerrainBake; llcleanterrainbakevk.cpp).
//
// Each region's blended terrain surface (clean_terrain_mat.glsl: composition weights, the 4
// detail textures / PBR materials, hex tiling) is baked once into per-region textures in
// region-local XY (clean_terrain_bake.comp), so far terrain samples 1-3 textures instead of
// re-blending 4 materials (up to 16 fetches, x3 in hex tiling's blend bands) per pixel.
// One row per TERRAIN RECORD index (the terrain instance's skin_id = its region), draw set 0
// binding 44, rewritten by the CPU every frame it changes (vkCmdUpdateBuffer, kTerrBakeCap
// rows; a record past the table never has a bake):
//   tex.x  albedo slot (bindless; RGBA8 sRGB: rgb albedo, a PBR occlusion)
//   tex.y  PBR slot    (RGBA8 UNORM: xy shading normal in the Ng-relative XY frame (below),
//                       z roughness, w metallic); 0 = legacy terrain (albedo only)
//   tex.z  emissive    (RGBA8 sRGB); 0 = none (no material of the region emits)
//   tex.w  flags: TB_READY (the images hold THIS frame's inputs: a stale bake is never shown,
//                its row is cleared the frame the inputs change), TB_PBR
//   xf.x   1 / region size (uv = region-local XY * xf.x; clamp-to-edge sampler)
//   xf.y   d0: main-camera distance (m) where the crossfade to the bake starts
//   xf.z   1 / band (m^-1): fully baked at d0 + band
//   xf.w   d1 = d0 + band: a draw whose whole patch box lies past d1 is drawn by the slim
//          bake-only G-buffer pipeline (clean_draw.vert CLEAN_VS_TERRAIN_SIDE)
// Distances are to the MAIN camera's LOD position (ssr.terrain_lod.xyz, every view: the
// smooth terrain geomorph uses the same point), so the per-patch split in the VS and the
// per-pixel weight in the FS agree: every pixel of a patch past d1 has weight 1.
//
// NORMAL FRAME: the bake stores the blended shading normal N relative to the geometric
// normal it was baked on, in the frame T = normalize(X - Ng Ng.x), B = Ng x T (region X / Y
// projected onto the surface). At draw time the same frame is built on the DRAWN geometric
// normal, so the detail rides on whatever LOD level / geomorph is drawn -- as the live path
// perturbs the drawn Ng -- and a finer bake normal lands on coarse far geometry.
#ifndef CLEAN_TERRAIN_BAKE_GLSL
#define CLEAN_TERRAIN_BAKE_GLSL

struct TerrBake {
    uvec4 tex;
    vec4  xf;
};
layout(std430, set = 0, binding = 44) readonly buffer TerrBakes { TerrBake terrain_bakes[]; };

const uint TB_READY = 1u;
const uint TB_PBR   = 2u;

bool terrBakeReady(uint rec)
{
    return rec < uint(terrain_bakes.length()) && (terrain_bakes[rec].tex.w & TB_READY) != 0u;
}

// Patch-level split (VS): the draw's whole box is past the bake's full-weight distance.
// box_dist: terrainBoxDist of the instance box from ssr.terrain_lod.xyz.
bool terrBakeFar(uint rec, float box_dist)
{
    return terrBakeReady(rec) && box_dist >= terrain_bakes[rec].xf.w;
}

// The Ng-relative XY frame (see NORMAL FRAME above). Ng unit length.
void terrBakeFrame(vec3 Ng, out vec3 T, out vec3 B)
{
    const vec3 t = vec3(1.0, 0.0, 0.0) - Ng * Ng.x;
    // (a vertical X-facing wall: fall back to region Y)
    T = dot(t, t) > 1e-8 ? normalize(t) : normalize(vec3(0.0, 1.0, 0.0) - Ng * Ng.y);
    B = cross(Ng, T);
}

#ifndef CLEAN_TERRAIN_BAKE_TABLE_ONLY
// Per-pixel weight of the bake (0 = live only, 1 = bake only). wpos: world position.
float terrBakeWeight(uint rec, vec3 wpos)
{
    if (!terrBakeReady(rec)) return 0.0;
    const vec4 xf = terrain_bakes[rec].xf;
    return clamp((distance(wpos, ssr.terrain_lod.xyz) - xf.y) * xf.z, 0.0, 1.0);
}

// The bake at region-local xy: albedo (linear), ORM (occlusion, roughness, metallic),
// emissive (linear) and the shading normal on the drawn geometric normal Ng. dxy1 / dxy2:
// the screen gradients of xy, gs = exp2(the view's mip bias): EXPLICIT gradients, because
// the caller samples under a per-pixel condition (the crossfade weight), where implicit
// derivatives are undefined. Legacy bakes: ORM (1, 1, 0), no emissive, N = Ng.
void terrBakeFetch(uint rec, vec2 xy, vec2 dxy1, vec2 dxy2, vec3 Ng, float gs,
                   out vec3 col, out vec3 orm, out vec3 emis, out vec3 N)
{
    const uvec4 tx = terrain_bakes[rec].tex;
    const float k = terrain_bakes[rec].xf.x;
    const vec2 uv = xy * k, g1 = dxy1 * (k * gs), g2 = dxy2 * (k * gs);
    const vec4 a = textureGrad(uTextures[nonuniformEXT(tx.x)], uv, g1, g2);
    col = a.rgb;
    orm = vec3(1.0, 1.0, 0.0);
    emis = vec3(0.0);
    N = Ng;
    if ((tx.w & TB_PBR) != 0u)
    {
        const vec4 p = textureGrad(uTextures[nonuniformEXT(tx.y)], uv, g1, g2);
        orm = vec3(a.a, p.z, p.w);
        const vec2 t = p.xy * 2.0 - 1.0;
        vec3 T, B;
        terrBakeFrame(Ng, T, B);
        N = normalize(T * t.x + B * t.y + Ng * sqrt(max(1.0 - dot(t, t), 0.0)));
        if (tx.z != 0u) emis = textureGrad(uTextures[nonuniformEXT(tx.z)], uv, g1, g2).rgb;
    }
}
#endif   // !CLEAN_TERRAIN_BAKE_TABLE_ONLY

#endif   // CLEAN_TERRAIN_BAKE_GLSL
