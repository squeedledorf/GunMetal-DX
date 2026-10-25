// vkclean -- VOLUMETRIC FOG (#718): what the four froxel COMPUTE passes share -- their
// pipeline layout {draw set 0, draw set 1, own set}, the own set's bindings, the push block
// and the froxel -> world mapping. (Consumers use clean_vol_apply.glsl instead.)
//
// OWN SET (set 2; LLCleanVolFogVK, one set per history parity p):
//   0  uCur      rgba16f storage  the CURRENT frame's cells: the injection writes the medium
//                                 (sigma_s rgb, sigma_t), the light pass overwrites it in
//                                 place with (in-scatter rgb per metre, sigma_t)
//   1  VolList   SSBO             the local volume primitives (clean_vol_inject.comp)
//   2  uHistPrev sampler3D        last frame's history (linear: the reprojection's taps)
//   3  uHistCur  rgba16f storage  this frame's history (temporal writes, integration reads)
//   4  uInteg    rgba16f storage  the integrated volume (in-scatter rgb, transmittance a);
//                                 draw set 0 binding 45 samples the same image
#ifndef CLEAN_VOL_PASS_GLSL
#define CLEAN_VOL_PASS_GLSL

#define VOL_SET 2

layout(push_constant) uniform VolPush {
    mat4 reproj;   // temporal pass: this frame's camera-relative world -> LAST frame's grid
                   // clip (last vol_vp x translate(eye - last eye)); 0 in the other passes
    vec4 jitter;   // xyz this frame's sample offset inside a cell (cell units, [-0.5, 0.5)),
                   // w frame index (noise rotation)
    vec4 fwd;      // xyz the camera's forward axis (world, unit), w time (s; noise scroll)
    vec4 p0;       // per pass (header of each shader)
    vec4 p1;
} vp;

// Camera-relative world position of the continuous froxel coordinate g (cell units; the
// centre of cell i is i + 0.5): screen uv = g.xy / dims.xy on the unjittered projection,
// view depth = depth(g.z / dims.z).
vec3 volRelPos(vec3 g)
{
    const vec3 dims = ssr.vol_grid.xyz;
    const vec2 ndc = g.xy / dims.xy * 2.0 - 1.0;
    const vec4 w = ssr.vol_inv_vp * vec4(ndc, 0.5, 1.0);
    const vec3 dir = w.xyz / w.w;
    const float vz = volDepthOfSlice(g.z / dims.z);
    return dir * (vz / max(dot(dir, vp.fwd.xyz), 1e-6));
}

// Interleaved gradient noise (Jimenez 2014).
float volIgn(vec2 px) { return fract(52.9829189 * fract(dot(px, vec2(0.06711056, 0.00583715)))); }

// This frame's sample position inside cell c (cell units): the frame's Halton (2, 3, 5)
// offset (vp.jitter, rotated every frame by the C++ side, independent of TAA) in x / y; in
// depth, additionally decorrelated per column by IGN, so neighbouring columns sample
// different depths of their slice in the same frame (the temporal pass's 6-neighbour box
// then sees the slice's spread, and the integration averages it spatially). The injection
// and the light pass call this with the same c: the medium and its lighting are sampled at
// one point.
vec3 volSampleCoord(uvec3 c)
{
    const float jz = fract(vp.jitter.z + 0.5 + volIgn(vec2(c.xy) + 5.588238 * mod(vp.jitter.w, 64.0))) - 0.5;
    return vec3(c) + 0.5 + vec3(vp.jitter.xy, jz);
}

#endif
