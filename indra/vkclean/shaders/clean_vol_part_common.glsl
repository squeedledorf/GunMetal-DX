// vkclean -- VOLUMETRIC PARTICLES (#721, epic #722 phase 4): what the particle selection /
// binning pass (clean_vol_part.comp) and the froxel injection (clean_vol_inject.comp) share.
// Byte layout = LLCleanVolFogVK (kPV* constants, PVolGPU); keep the two in step.
//
// THE PARTICLE VOLUME BUFFER (fog own set binding 6, device local, one per fog instance):
//   pv_hdr    x injected records this frame, y boundary-bucket claims, z tile-list
//             overflows (a record dropped from a full tile), w candidates (qualifying
//             particles before the cap)
//   pv_aux    unused (0)
//   pv_hist   candidates per distance bucket (bucket = 32 sqrt(dist / max distance)):
//             the cap keeps the NEAREST particles -- every bucket that fits whole is taken,
//             only the one straddling the cap is claimed by atomic order (the only place
//             the selection can change from frame to frame without the scene changing)
//   pv_tile_n records listed per SCREEN TILE (VP_TILE x VP_TILE froxel columns, all depths)
//   pv_tile   the tiles' record indices, VP_TILE_CAP each
//   pv_rec    the injected particles (PVol), at most the C++ cap
// Header, histogram and tile counts are cleared (vkCmdFillBuffer) every frame the pass runs.
#ifndef CLEAN_VOL_PART_COMMON_GLSL
#define CLEAN_VOL_PART_COMMON_GLSL

#define VP_TILE       8u      // froxel columns per tile side (the inject's 4x4x4 groups lie in one tile)
#define VP_MAX_TILES  336u    // the high tier: 192 x 108 froxels -> 24 x 14 tiles
#define VP_TILE_CAP   256u    // records per tile
#define VP_HIST       32u     // distance buckets

// One injected particle: a soft sphere, density (1 - r^2 / R^2)^2 x the core values.
struct PVol {
    vec4 pos_r;   // xyz centre (world), w radius R (m; >= about a froxel at its depth)
    vec4 scat;    // rgb scattering sigma_s at the core (/m), a extinction sigma_t at the core (/m)
    vec4 emit;    // rgb emitted radiance per metre at the core (linear HDR), a noise seed
};                // 48 B (LLCleanVolFogVK::PVolGPU)

#ifdef VOL_PART_READONLY
layout(std430, set = 2, binding = 6) readonly buffer PartVol {
#else
layout(std430, set = 2, binding = 6) buffer PartVol {
#endif
    uvec4 pv_hdr;
    uvec4 pv_aux;
    uint  pv_hist[VP_HIST];
    uint  pv_tile_n[VP_MAX_TILES];
    uint  pv_tile[VP_MAX_TILES * VP_TILE_CAP];
    PVol  pv_rec[];
};

// Smooth sphere profile (1 - u^2)^2 at the squared normalized radius u2 (< 1).
float pvProfile(float u2) { const float s = 1.0 - u2; return s * s; }

// Cheap per-particle value noise (one octave; hashed lattice, quintic fade), [0, 1].
float pvHash(vec3 p)
{
    p = fract(p * 0.3183099 + vec3(0.1, 0.2, 0.3));
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}
float pvNoise(vec3 x)
{
    const vec3 i = floor(x);
    const vec3 f = fract(x);
    const vec3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    return mix(mix(mix(pvHash(i + vec3(0, 0, 0)), pvHash(i + vec3(1, 0, 0)), u.x),
                   mix(pvHash(i + vec3(0, 1, 0)), pvHash(i + vec3(1, 1, 0)), u.x), u.y),
               mix(mix(pvHash(i + vec3(0, 0, 1)), pvHash(i + vec3(1, 0, 1)), u.x),
                   mix(pvHash(i + vec3(0, 1, 1)), pvHash(i + vec3(1, 1, 1)), u.x), u.y), u.z);
}

#endif
