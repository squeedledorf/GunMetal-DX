// vkclean — PER-AVATAR REST-POSE ACCUMULATION (#901 follow-up; vkgfx/llcleanweathervk_accum.cpp,
// clean_weather_accum.comp): an avatar's snow and wetness live in a small 3D grid over its REST
// POSE, so a surface gathers them only while it is actually exposed, over time -- a moment of
// exposure (an umbrella swaying with a dance) adds almost nothing, and snow already lying is not
// removed because the cover swings back over it.
//
// REST-POSE SPACE (the skeleton's bind space: z up, feet near 0, the pelvis ~1.07 m, x forward,
// y left; stable under animation):
//   rigged      the vertex's BIND-POSE position, bind_shape x the source (quantized) position --
//               by construction (joint world x inverse bind x bind shape x position) the point in
//               the skeleton's rest space every mesh rigged to the skeleton shares. Its normal: the
//               bind shape's cofactor x the source normal.
//   worn rigid  the vertex in its attachment joint's frame (the instance's constant joint offset x
//               the object-local position) placed with that joint's REST transform (ac_rest: the
//               default skeleton, identity bone rotations; attachment points keep their own
//               rotation), the CPU's table (built once from the own avatar's skeleton).
// A vertex outside its grid's box adds nothing (the box grows to it on the next update).
//
// THE GRIDS: one 3D image (rgba8 unorm, GENERAL; draw set 0 binding 51 sampled linear / clamp, the
// shelter set binding 14 as storage), every grid side by side: width res.x x BUCKETS (the buckets'
// blocks along x), height res.y, depth res.z x grids (the grids' blocks along z). A texel holds
// PREMULTIPLIED (r snow x occ, g wetness x occ, b occ, a #909 near openness x occ): snow normalised to kAcSnowFull
// metres, wetness 0..1, occ = 1 once a vertex sampled the cell (0: empty space -- the shading
// divides by the filtered occupancy, so empty neighbours never dilute a surface's value). Three
// BUCKETS by the REST normal's z (0 up > 0.4, 1 side, 2 down < -0.4): the top and the underside of
// a thin part (a hat brim, a sleeve) sharing a cell keep separate values.
//
// A texel's a: (1 - the cell's last sampled NEAR self-cover) x occ (#909: a cell whose draw is
// culled for an update integrates with the avatar's joint exposure x that -- a mouth's interior
// stays covered; the far cover, an umbrella, stays the joint exposure's, live).
//
// #909 NEAR SELF-COVER: the avatar's OWN geometry close above a surface (a snout over the tongue,
// a chin over the throat, an arm over the armpit, a mane over the neck) is static relative to it,
// so it is taken LIVE, per fragment / per sampled vertex, from the avatar's shelter map (the top
// over the column, against the receiver's own plane): within [threshold, range] above it the
// surface is self-covered -- no snow shows on it, it gathers neither snow nor wetness. Cover
// farther above (an umbrella's canopy) stays FAR cover: it only goes through the grid (exposure
// integrated over time: its sway never flashes). The grid's cells are ~4 cm: a tongue and the
// snout's top share one; the per-fragment near test is what keeps the tongue clean on display.
//
// THE RECORD (std430; draw set 0 binding 50 read by the VS / FS, the shelter set binding 12 for
// the compute; the includer sets WX_AC_SET / WX_AC_BINDING / WX_AC_ACCESS):
//   head     x grids in the pool, y res x | y << 8 | z << 16, z #909 the NEAR SELF-COVER
//            parameters (range cm | threshold mm << 8 | softness mm << 16; range 0: off -- the
//            buckets are WX_AC_BUCKETS), w flags: 1 the rest skeleton is valid
//   kgrid    the scratch slots: [k] the grid updated through slot k this recording (~0 none)
//   ctl[g]   (CPU) x the owner's joint-table slot (~0: free), y flags: 1 due (accumulated this
//            recording), 2 reset (a new owner: cleared, box re-measured), 4 seeded (the CPU
//            restored it -- own avatar after a teleport), z its scratch slot, w dt (s, float bits)
//   box[2g]  (PREP) xyz the rest box's min, w 1 = valid; [2g + 1] xyz its max
//   obox[2g] (PREP) the box before this update's growth, w 1 = it grew this update (resampled)
//   meas[2g] (ACCUM) the rest positions sampled: order-encoded min xyz; [2g + 1] max xyz
//   stat[g]  (RESOLVE, this update) x samples accumulated, y the max snow (x 65535 / full), z the
//            occupied cells' wetness sum (x 255), w the occupied cells
//   slot[s]  (PREP) the grid of joint-table slot s + 1 (0: none)
//   rest[j]  (CPU, once) joint j's rest transform (clean_joint_table.glsl rows)
#ifndef CLEAN_WEATHER_ACCUM_GLSL
#define CLEAN_WEATHER_ACCUM_GLSL

#define WX_AC_GRIDS   16u
#define WX_AC_BUCKETS 3u
const float kAcSnowFull = 0.02;       // m of snow the unorm snow channel's 1 stands for (the coat is full at 1 cm)
const float kAcOccMin = 0.04;         // a filtered occupancy below this: the grid knows nothing here

#ifndef WX_AC_ACCESS
#define WX_AC_ACCESS readonly
#endif
layout(std430, set = WX_AC_SET, binding = WX_AC_BINDING) WX_AC_ACCESS buffer WxAccum
{
    uvec4  ac_head;
    uvec4  ac_kgrid;
    uvec4  ac_ctl[WX_AC_GRIDS];
    vec4   ac_box[2u * WX_AC_GRIDS];
    vec4   ac_obox[2u * WX_AC_GRIDS];
    uvec4  ac_meas[2u * WX_AC_GRIDS];
    uvec4  ac_stat[WX_AC_GRIDS];
    uint   ac_slot[1024];
    mat3x4 ac_rest[216];   // (byte 6176: LLCleanWeatherVK::kAcRestOffset; record 16544 B)
};

uvec3 wxAcRes() { return uvec3(ac_head.y & 0xFFu, (ac_head.y >> 8u) & 0xFFu, (ac_head.y >> 16u) & 0xFFu); }

// #909 the near self-cover parameters (ac_head.z): x the range (m), y the threshold (m), z the
// softness (m); x 0 = off.
vec3 wxAcNear()
{
    const uint w = ac_head.z;
    return vec3(float(w & 0xFFu) * 0.01, float((w >> 8u) & 0xFFu) * 0.001, float((w >> 16u) & 0xFFu) * 0.001);
}
// #909: how much of the avatar's own surface `above` m over a receiver counts as NEAR self-cover
// (0..1): from the threshold (+ the softness) up to the range, fading out over kAcNearFade past
// it (an umbrella's canopy dipping toward a shoulder does not snap its snow off). nr: wxAcNear().
// (clean_weather_surface.glsl weatherNearCover is the same, for the FS without the record.)
const float kAcNearFade = 0.08;
float wxAcNearCover(float above, vec3 nr)
{
    if (!(nr.x > 0.0)) return 0.0;
    return smoothstep(nr.y, nr.y + max(nr.z, 1.0e-3), above) * (1.0 - smoothstep(nr.x, nr.x + kAcNearFade, above));
}

// The bucket a rest normal's z accumulates into (its dominant one).
uint wxAcBucket(float nz) { return nz > 0.4 ? 0u : (nz < -0.4 ? 2u : 1u); }
// The shading's blend of the buckets by the rest normal's z (at most two non-zero).
vec3 wxAcBucketW(float nz)
{
    const float up = smoothstep(0.25, 0.55, nz);
    const float dn = 1.0 - smoothstep(-0.55, -0.25, nz);
    return vec3(up, max(1.0 - up - dn, 0.0), dn);
}

// The rest transform of joint j (a mat4 from the stored rows).
mat4 wxAcRestJoint(uint j)
{
    const mat3x4 W = ac_rest[min(j, 215u)];
    return mat4(vec4(W[0].x, W[1].x, W[2].x, 0.0), vec4(W[0].y, W[1].y, W[2].y, 0.0),
                vec4(W[0].z, W[1].z, W[2].z, 0.0), vec4(W[0].w, W[1].w, W[2].w, 1.0));
}

// The cofactor of a 3x3 (the normal transform up to scale).
mat3 wxAcCofactor(mat3 m) { return mat3(cross(m[1], m[2]), cross(m[2], m[0]), cross(m[0], m[1])); }

#endif   // CLEAN_WEATHER_ACCUM_GLSL
