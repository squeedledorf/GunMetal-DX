// vkclean -- PARTICLE FOG (#1003; vkgfx/llcleanpartfogvk.h, RenderVKParticleFog): what the
// dense-fog passes share. Byte layouts = LLCleanPartFogVK (Params, SysGPU, RecGPU, the
// results / accumulator offsets); keep the two in step.
//
// THE IDEA. Creators build fog machines, smoke, steam and mist out of large, faint, slow,
// soft particle systems. The CPU classifies each SYSTEM (LLCleanPartFogVK::classify) and the
// converted ones keep being simulated exactly as SL specifies (burst pattern, speed, accel,
// wind, age: the GPU particle pool, untouched) -- but their particles become DENSITY CARRIERS
// instead of billboards: every frame each one is splatted into a LOCAL WORLD-SPACE BRICK
// around its system (a 3D density grid, ~5-10 cm voxels near the camera, coarser far away),
// which is ray-marched in its own pass, lit (sun cascades, sky, the clustered local lights),
// self-shadowed (bright tops, dark bellies) and composited with the froxel fog by depth.
//
// PASSES (compute, layout {draw set 0, draw set 1 (bindless), own set = PF_SET 2}; 16 B push):
//   texstats  clean_pfog_texstats.comp  per-texture stats (mean alpha / colour, edge energy) once
//   bin       clean_pfog_bin.comp       per particle: its system, the system's bounds (readback),
//                                       the sprite cross-fade, the splat record
//   splat     clean_pfog_splat.comp     per record (indirect): mass-conserving kernel -> accumulator
//   resolve   clean_pfog_resolve.comp   accumulator -> the atlases (coarse first, then fine)
//   light     clean_pfog_light.comp     per coarse voxel: in-brick sun optical depth, ambient
//                                       occlusion, local lights SELF-SHADOWED by the medium
//   march     clean_pfog_march.comp     reduced-resolution ray march over the visible bricks
//                                       (EROSION noise model, HG phase, beer-powder); writes
//                                       the next frame's floor map from the scene depth
//   temporal  clean_pfog_temporal.comp  jitter accumulation (short history, variance clamped)
//   noise     clean_pfog_noise.comp     the tiling noise volumes (once): 64^3 curl + base
//                                       billow, 32^3 Worley detail
// and the COMPOSITE (clean_pfog_composite.frag; {draw set 0, composite set = PF_SET 1}) in
// render pass 2 right after the froxel fog's apply.
//
// THE ATLAS. One brick = one atlas SLOT of PF_N^3 fine voxels (+ PF_NC^3 coarse ones), slots
// side by side along x then z. A brick uses dims (<= PF_N per axis) of its slot; every read
// clamps to its own voxel centres (no bleeding between slots).
//   fine    rgba16f  rgb scattering tint (linear), a extinction sigma (/m, before the blend)
//   coarse  rgba16f  the big kernels' density (same encoding), resolved first
//   vel     rgba16f  xyz mass-weighted particle velocity (m/s), w AMBIENT OCCLUSION (0..1: the
//                    transmittance out of the medium over a few cone directions, sky-weighted)
//   light   rgba16f  rgb local-light in-scatter per unit scattering (isotropic; the strongest
//                    lights shadowed through the medium), a sun optical depth
//   ldir    rgba16f  xyz the local light's mean direction toward the lights (luminance
//                    weighted, length = how concentrated: 1 one light, ~0 surrounded), w the
//                    mean optical depth toward the shadowed lights (the beer-powder term)
// The ACCUMULATOR (own binding 5) holds per slot PF_N^3 x 4 fine channels (sigma, sigma r,
// sigma g, sigma b; fixed point PF_FIX per 1/m) then PF_NC^3 x 8 coarse ones (the same four
// + mass-weighted velocity x / y / z and the mass weight; PF_FIXV), cleared per frame for the
// slots in use.
// The FLOOR buffer (own binding 28) holds per atlas slot PF_FLOOR_WORDS: a header (x / y the
// map's world corner, z its cell size (m), w 1 = written; floats as bits), the density stats
// of this frame's fine resolve (word 4 sum sigma x PF_STAT1, word 5 sum sigma^2 x PF_STAT2:
// the density-weighted mean density the erosion normalises by), then PF_NC^2 columns: the
// lowest UPWARD-FACING scene surface (order-preserving uint, all ones = none seen) below the
// system's source -- what the march saw through the depth buffer (kept a few seconds; cleared
// when the brick moves), what the bin's HEAVY settling lands the fog on.
#ifndef CLEAN_PFOG_COMMON_GLSL
#define CLEAN_PFOG_COMMON_GLSL

#ifndef PF_SET
#define PF_SET 2
#endif

#define PF_MAX_SYS     64u     // systems in the table per frame (LLCleanPartFogVK::kMaxSys)
#define PF_MAX_BRICKS  32u     // bricks marched per frame (kMaxBricks)
#define PF_MAX_JOBS    8u      // texture-stat jobs per frame (kMaxTexJobs)
#define PF_N           64u     // fine voxels per slot edge (kN)
#define PF_NC          32u     // coarse voxels per slot edge (kNC)
#define PF_FIX         1024.0  // accumulator fixed point per 1/m of sigma
#define PF_FIXV        256.0   // velocity mass-weight fixed point
#define PF_FAR         65000.0 // "no fog" distance
// Per-slot accumulator words (kSlotWords).
#define PF_SLOT_WORDS  (PF_N * PF_N * PF_N * 4u + PF_NC * PF_NC * PF_NC * 8u)
#define PF_COARSE_BASE (PF_N * PF_N * PF_N * 4u)
#define PF_FLOOR_HDR   8u
#define PF_FLOOR_WORDS (PF_FLOOR_HDR + PF_NC * PF_NC)
#define PF_STAT1       256.0   // density stats fixed point (sigma clamped at 16 for them)
#define PF_STAT2       32.0

// One brick of this frame (LLCleanPartFogVK::BrickGPU).
struct PfBrick {
    vec4 org_vox;     // xyz world origin (min corner, agent frame), w voxel size (m)
    vec4 dims_slot;   // xyz voxel dims (<= PF_N), w atlas slot
    vec4 atlas;       // xyz fine-atlas texel offset of the slot, w BLEND (volume share 0..1)
    vec4 look;        // x table index, y debug hue (the blend, 0..1), zw 0
    vec4 shape;       // x base noise scale (m: the billows' size, from the sprite size), y floor
                      // reference z (surfaces above it are not the fog's floor), z the system's
                      // noise phase (0..1), w 0
};

// The per-recording parameter block (LLCleanPartFogVK::Params, std140: vec4 / mat4 only).
layout(std140, set = PF_SET, binding = 0) uniform PFogParams {
    mat4 inv_vp;      // UNJITTERED VK clip -> CAMERA-RELATIVE world (this frame)
    mat4 reproj;      // camera-relative world (this frame) -> last frame's unjittered clip
    vec4 dims;        // render w, h, march w, h
    vec4 cam;         // xyz eye (world), w time (s, wrapped)
    vec4 march;       // x max steps, y frame index, z history frames (max), w 1 = history reset
    vec4 look;        // x multi-scatter strength, y billow strength, z self-shadow strength, w ambient scale
    vec4 noise;       // x (unused), y warp amplitude (x the base scale), z flow period (s), w debug view
                      //   (1 blend + boxes, 2 erosion, 3 ambient occlusion, 4 local light)
    vec4 counts;      // x bricks, y systems, z texture jobs, w particle pool size
    vec4 atlas;       // fine atlas w, h, d (texels), w the march targets' allocated width
    vec4 atlasC;      // coarse atlas w, h, d (texels), w the march targets' allocated height
    vec4 light;       // x local-light scale, y light count, z 1 = cascades, w HG g
    vec4 misc;        // x kernel spread, y fade-in (s), z min kernel radius (voxels), w sigma clamp (/m)
    vec4 erode;       // x erosion strength, y per-puff lumpiness, z beer-powder strength, w HG back-lobe weight
    vec4 settle;      // x roll-out gain (horizontal m per m of blocked descent), y settle delay (s),
                      // z the settled layer's height squash (min), w local-light shadow strength
    PfBrick bricks[PF_MAX_BRICKS];
} pf;

// ---- table / list / results (compute only) ----
#ifndef PF_NO_BUFFERS
struct PfSys {
    uvec4 id;     // x emitter slot, y source id, z brick index (0xFFFFFFFF: none), w flags (1 bounds, 2 sprite fade)
    vec4  tex;    // x texture mean alpha, y density multiplier, z sprite fade (the blend), w 0
    vec4  col;    // rgb texture mean colour (texel values), w 0
    vec4  heavy;  // xy the source's world xy (the roll-out centre), z the fallback floor z (no
                  // floor map column: the land / the source minus a drop limit), w settling speed (m/s, 0 off)
};
layout(std430, set = PF_SET, binding = 1) readonly buffer PFogTable {
    uvec4 tb_hdr;                 // x systems, y texture jobs, zw 0
    uvec4 tb_job[PF_MAX_JOBS];    // x bindless texture index
    PfSys tb_sys[PF_MAX_SYS];
};

// One splat record (bin -> splat).
struct PfRec {
    vec4 pos_r;    // xyz centre (world), w kernel radius (m)
    vec4 tint_m;   // rgb tint (linear), w mass (optical depth x area, m^2)
    vec4 vel_b;    // xyz velocity (m/s), w brick index
    vec4 aux;      // x vertical squash (the settled layer: kernel height / R), y the particle's
                   // noise seed (an integer < 2^24), zw 0
};
layout(std430, set = PF_SET, binding = 4) buffer PFogList {
    uvec4 ls_hdr;  // x records (= the splat's indirect x), y z 1, w dropped
    PfRec ls_rec[];
};
layout(std430, set = PF_SET, binding = 5) buffer PFogAccum { uint ac[]; };
// Results (copied to the host every frame): per table system 8 words (min xyz, max xyz as
// order-preserving uints, live particles, 0); per texture job 2 vec4 (mean alpha, mean rgb;
// edge sharpness, high-frequency residual, width, height).
layout(std430, set = PF_SET, binding = 6) buffer PFogResults {
    uint rs_bounds[PF_MAX_SYS * 8u];
    vec4 rs_tex[PF_MAX_JOBS * 2u];
};
#endif
#ifdef PF_FLOOR
layout(std430, set = PF_SET, binding = 28) buffer PFogFloor { uint fl[]; };
// The density-weighted mean density of slot s this frame (0: no stats).
float pfRefSigma(uint s)
{
    const float s1 = float(fl[s * PF_FLOOR_WORDS + 4u]) / PF_STAT1;
    const float s2 = float(fl[s * PF_FLOOR_WORDS + 5u]) / PF_STAT2;
    return s1 > 1e-4 ? s2 / s1 : 0.0;
}
#endif

// Order-preserving float <-> uint (atomicMin / atomicMax bounds).
uint  pfOrdered(float f)  { const uint b = floatBitsToUint(f); return (b & 0x80000000u) != 0u ? ~b : (b | 0x80000000u); }
float pfUnordered(uint u) { return uintBitsToFloat((u & 0x80000000u) != 0u ? (u & 0x7FFFFFFFu) : ~u); }

uvec3 pfDims(PfBrick b)  { return uvec3(b.dims_slot.xyz + 0.5); }
uvec3 pfDimsC(PfBrick b) { return (pfDims(b) + 1u) / 2u; }
vec3  pfMax(PfBrick b)   { return b.org_vox.xyz + vec3(pfDims(b)) * b.org_vox.w; }

// Ray (origin O, direction D) against the brick's box: (t enter, t exit); enter > exit = miss.
vec2 pfBoxHit(PfBrick b, vec3 O, vec3 D)
{
    const vec3 inv = vec3(1.0) / mix(D, vec3(1e-20), lessThan(abs(D), vec3(1e-20)));
    const vec3 t0 = (b.org_vox.xyz - O) * inv, t1 = (pfMax(b) - O) * inv;
    const vec3 lo = min(t0, t1), hi = max(t0, t1);
    return vec2(max(max(lo.x, lo.y), lo.z), min(min(hi.x, hi.y), hi.z));
}

#ifdef PF_SAMPLERS
layout(set = PF_SET, binding = 11) uniform sampler3D uDens;     // fine (tint, sigma)
layout(set = PF_SET, binding = 12) uniform sampler3D uCoarse;   // coarse (tint, sigma)
layout(set = PF_SET, binding = 13) uniform sampler3D uVel;      // (velocity, sky OD)
layout(set = PF_SET, binding = 14) uniform sampler3D uLight;    // (local light, sun OD)
layout(set = PF_SET, binding = 16) uniform sampler3D uNoise;    // (curl xyz, billow) -- repeat
layout(set = PF_SET, binding = 25) uniform sampler3D uLDir;     // (light direction, light OD)
layout(set = PF_SET, binding = 27) uniform sampler3D uNoiseD;   // (Worley detail octaves) -- repeat

// World point -> fine / coarse atlas uvw of brick b (clamped to its own voxel centres).
vec3 pfUvw(PfBrick b, vec3 P)
{
    const vec3 lv = (P - b.org_vox.xyz) / b.org_vox.w;
    const vec3 d = vec3(pfDims(b));
    return (b.atlas.xyz + clamp(lv, vec3(0.5), d - 0.5)) / pf.atlas.xyz;
}
vec3 pfUvwC(PfBrick b, vec3 P)
{
    const vec3 lv = (P - b.org_vox.xyz) / (2.0 * b.org_vox.w);
    const vec3 d = vec3(pfDimsC(b));
    return (b.atlas.xyz * 0.5 + clamp(lv, vec3(0.5), d - 0.5)) / pf.atlasC.xyz;
}
bool pfInside(PfBrick b, vec3 P)
{
    return all(greaterThanEqual(P, b.org_vox.xyz)) && all(lessThanEqual(P, pfMax(b)));
}
// Extinction of brick b at P (/m) including its blend (volume share); 0 outside it.
float pfSigma(PfBrick b, vec3 P)
{
    if (!pfInside(b, P)) return 0.0;
    return textureLod(uDens, pfUvw(b, P), 0.0).a * b.atlas.w;
}
#endif

// Interleaved gradient noise (as the froxel passes' volIgn).
float pfIgn(vec2 px) { return fract(52.9829189 * fract(dot(px, vec2(0.06711056, 0.00583715)))); }

// Integer hash (the per-particle / per-system noise phases).
uint pfHash(uint h)
{
    h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
    return h;
}
vec3 pfHashVec(uint h)   // [0, 1)^3
{
    const uint a = pfHash(h), b = pfHash(a ^ 0x9e3779b9u);
    return vec3(float(a & 0xFFFFu), float(a >> 16), float(b & 0xFFFFu)) / 65536.0;
}

#endif
