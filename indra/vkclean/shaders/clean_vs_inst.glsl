// vkclean -- #1033 slim per-instance record for the world vertex shaders (112 B vs the 224 B
// Instance). The VS reads only these fields of the instance (clean_draw.vert,
// clean_draw_rigged.vert); loading the full row stalled the vertex stage on cache misses.
// Derived from the Instance SSBO (and the previous-instance SSBO) every frame by
// clean_vs_inst.comp, so no writer of Instance needs to know about it.
//
//   m0..m2  model_matrix rows of the affine 3x4 (row i = M[0][i], M[1][i], M[2][i], M[3][i])
//   uvlin   uv_matrix [0].xy, [1].xy      uvoff   uv_matrix [3].xy
//           (cleanFaceUV / cleanFaceTangent read nothing else, except the GPU texture
//           animation (flag 1024), whose parameter block is read from the full row)
//   aux     bits 0-7 the attachment joint index (aabb_max.w), bit 31 = spin_axis non-zero
//           (spin axis / pivot are then read from the full row)
//   bound_r object-space radius about the origin enclosing the AABB (the VS only opens
//           the full row's AABB when the camera is within it)
// std430: 7 x 16 B.
#ifndef CLEAN_VS_INST_GLSL
#define CLEAN_VS_INST_GLSL

struct VsInst {
    vec4  m0;
    vec4  m1;
    vec4  m2;
    vec4  tint;
    vec4  uvlin;
    vec2  uvoff;
    float bound_r;
    uint  aux;
    uint  material_id;
    uint  flags;
    uint  skin_id;
    uint  pad0;
};

const uint VSI_SPIN = 0x80000000u;

mat4 vsiModel(const VsInst s)
{
    return mat4(vec4(s.m0.x, s.m1.x, s.m2.x, 0.0), vec4(s.m0.y, s.m1.y, s.m2.y, 0.0),
                vec4(s.m0.z, s.m1.z, s.m2.z, 0.0), vec4(s.m0.w, s.m1.w, s.m2.w, 1.0));
}

// (the z column / w row never reach a result: cleanFaceUV multiplies z by 0)
mat4 vsiUv(const VsInst s)
{
    return mat4(vec4(s.uvlin.xy, 0.0, 0.0), vec4(s.uvlin.zw, 0.0, 0.0),
                vec4(0.0, 0.0, 1.0, 0.0), vec4(s.uvoff, 0.0, 1.0));
}

uint vsiBone(const VsInst s) { return s.aux & 0xFFu; }

#endif
