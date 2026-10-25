// vkclean -- smooth terrain LOD (#211, RenderVKTerrainSmooth; llcleanterrainsmooth.h).
//
// A terrain patch's LOD group holds the smooth surface at group level g = cells of 2^-g m
// (0: 1 m ... 3: 0.125 m; a level may be absent -- not resident / above the detail
// setting -- and the pick falls back coarser first). The level comes from the MAIN
// camera's distance to the patch box, identically in the cull (clean_cull.comp) and the
// TLAS pass (clean_rt_instances.comp), so shadows / RT see what the main view draws.
// Faceted terrain (smooth off) publishes one slot at every level: the choice is moot.
//
// GEOMORPH (clean_draw.vert): a vertex of level g that level g - 1 does not have carries
// in texcoord0.y the height step onto level g - 1's triangle. Which level a vertex was
// introduced at follows from its region-local position (exact dyadic metres), and the
// step is blended in over that level's band, which ENDS at the distance the cull leaves
// the level: the patch is already in its coarser shape when it switches (no pop), and a
// finer patch's edge reaches its coarser neighbour's edge (every vertex of the coarser
// neighbour's box, the shared edge included, is at least its switch distance away).
// Bands start past the previous switch + a patch diagonal, so a coarser level is never
// still morphing where the finer one hands over (patches up to ~15 m tall; skirts cover
// taller ones). Constants mirror llcleanterrainsmooth.h kLevelDist and llcleanrtvk.cpp
// kTerrSwitch / kTerrBandLo (the RT morph BLAS plan; mode 3 of clean_rt_instances.comp
// replays terrainMorph for the BLAS).
#ifndef CLEAN_TERRAIN_LOD_GLSL
#define CLEAN_TERRAIN_LOD_GLSL

// Distance from cam to the world box of local box [lo, hi] under M.
float terrainBoxDist(mat4 M, vec3 lo, vec3 hi, vec3 cam)
{
    vec3 wmin = vec3(1e30), wmax = vec3(-1e30);
    for (int i = 0; i < 8; ++i)
    {
        vec3 c = vec3((i & 1) != 0 ? hi.x : lo.x, (i & 2) != 0 ? hi.y : lo.y, (i & 4) != 0 ? hi.z : lo.z);
        vec3 w = (M * vec4(c, 1.0)).xyz;
        wmin = min(wmin, w);
        wmax = max(wmax, w);
    }
    return length(max(max(wmin - cam, cam - wmax), vec3(0.0)));
}

// The wanted group level at that distance (LLCleanTerrainSmooth::kLevelDist).
uint terrainLevel(float d)
{
    return d < 12.0 ? 3u : (d < 56.0 ? 2u : (d < 120.0 ? 1u : 0u));
}

// k-th candidate level for a wanted one: wanted, coarser down to 0, then finer.
uint terrainLevelOrder(uint want, uint k)
{
    return k <= want ? want - k : k;
}

// How far (0..1) a vertex at region-local p, d metres from the main camera, has moved
// onto the next-coarser level's surface. Morph bands per introduction level:
// 0.125 m [6, 12], 0.25 m [40, 56], 0.5 m [90, 120].
float terrainMorph(vec2 p, float d)
{
    vec2 band;
    if (any(notEqual(fract(p * 4.0), vec2(0.0))))      band = vec2(6.0, 12.0);
    else if (any(notEqual(fract(p * 2.0), vec2(0.0)))) band = vec2(40.0, 56.0);
    else                                               band = vec2(90.0, 120.0);
    return clamp((d - band.x) / (band.y - band.x), 0.0, 1.0);
}

#endif
