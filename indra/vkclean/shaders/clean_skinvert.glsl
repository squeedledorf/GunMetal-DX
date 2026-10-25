// vkclean — COMPACT SKINNED VERTEX (#616): the pre-skin output in mSkinVbo
// (llcleanrenderervk.cpp SkinVertex). 20 B = 5 words per vertex:
//   w0..w2 = position xyz, fp32 (exact: eye-relative world space, or the reduced-tier
//            skin cache's pelvis-local space; no lattice -- the stream is transient)
//   w3     = normal, octahedral snorm16 x | y << 16 (unit; zero / non-finite -> +Z)
//   w4     = tangent, octahedral: lo16 = x snorm16 (-32767..32767), hi16 = y snorm15 << 1
//            | bitangent sign (bit 16: 1 = handedness -1). NO TANGENT = lo16 0x8000
//            (-32768, which the encoder never emits): decodes to a zero tangent, the
//            frame the FS then derives from screen-space derivatives.
// Normal and tangent decode to UNIT length (the old float stream carried the skinned,
// unnormalized lengths; every consumer normalizes or only uses the direction).
// No UV: a skinned vertex's texcoord is its SOURCE quantized vertex's (clean_qvertex.glsl),
// at  src = draw source base (clean_cull.comp rig_cache word RC_SRC) + (vertex - vertexOffset)
// -- bit-identical to the copy the 48 B stream carried, which was decoded from it.
// Precision: normal <= ~0.005 deg (as the static QVertex), tangent <= ~0.01 deg.
#ifndef CLEAN_SKINVERT_GLSL
#define CLEAN_SKINVERT_GLSL
#include "clean_av_slots.glsl"

#include "clean_qvertex.glsl"

#define SV_WORDS      5u
#define SV_NO_TANGENT 0x00008000u

// Octahedral encode of a direction (any length; zero / non-finite -> (0, 0) = +Z).
// Mirrors llcleanqvertex.h octEncode; qvOctDecode is its inverse.
vec2 qvOctEncode(vec3 d)
{
    const float l1 = abs(d.x) + abs(d.y) + abs(d.z);
    if (!(l1 > 1e-30) || isinf(l1)) return vec2(0.0);
    vec2 p = d.xy / l1;
    if (d.z < 0.0)
        p = (1.0 - abs(p.yx)) * vec2(p.x >= 0.0 ? 1.0 : -1.0, p.y >= 0.0 ? 1.0 : -1.0);
    return p;
}

uint svPackNormal(vec3 n)
{
    const ivec2 q = ivec2(round(clamp(qvOctEncode(n), vec2(-1.0), vec2(1.0)) * 32767.0));
    return (uint(q.x) & 0xFFFFu) | (uint(q.y) << 16u);
}
vec3 svUnpackNormal(uint w) { return qvNormal(w); }

// t: tangent direction (any length; zero / non-finite = none), w: handedness (< 0 = -1).
uint svPackTangent(vec3 t, float w)
{
    const float l1 = abs(t.x) + abs(t.y) + abs(t.z);
    if (!(l1 > 1e-30) || isinf(l1)) return SV_NO_TANGENT;
    const vec2 e = clamp(qvOctEncode(t), vec2(-1.0), vec2(1.0));
    const int qx = int(round(e.x * 32767.0));
    const int qy = int(round(e.y * 16383.0)) * 2 + (w < 0.0 ? 1 : 0);
    return (uint(qx) & 0xFFFFu) | (uint(qy) << 16u);
}
vec4 svUnpackTangent(uint w)
{
    if ((w & 0xFFFFu) == SV_NO_TANGENT) return vec4(0.0);
    const int qy = qvHi(w);
    const vec2 e = vec2(float(qvLo(w)) / 32767.0, float(qy >> 1) / 16383.0);
    return vec4(qvOctDecode(clamp(e, vec2(-1.0), vec2(1.0))), (qy & 1) != 0 ? -1.0 : 1.0);
}

// A skinned vertex's words from a uint[] view (V = vertex index).
#define SV_POS(ARR, V) vec3(uintBitsToFloat(ARR[(V) * SV_WORDS]), uintBitsToFloat(ARR[(V) * SV_WORDS + 1u]), \
                            uintBitsToFloat(ARR[(V) * SV_WORDS + 2u]))
#define SV_NRM(ARR, V) svUnpackNormal(ARR[(V) * SV_WORDS + 3u])
#define SV_TAN(ARR, V) svUnpackTangent(ARR[(V) * SV_WORDS + 4u])

// REDUCED-TIER SKIN CACHE state (clean_cull.comp rig_cache, per instance RC_WORDS words):
// word RC_STATE = the main cull's state for this frame's draw (read by the rigged VS, the
// alpha sort, the GPU pick), word RC_SRC = the draw's SOURCE vertex base (the static slot,
// decimated step or morphed system-body slot its skinned range was skinned from).
// Words RC_CTR..+2 = the CENTRE OF THE DRAW'S POSED (skinned) BOUNDING BOX, fp32 bits, in the
// space of its skinned range (eye-relative, or pelvis-local for a skin-cache range), written
// by clean_preskin.comp for the range starting at vertex RC_CTR_KEY - 1 (0 = none): the
// rigged alpha sort's position (clean_alpha_sort*.comp; = the cluster path's posed-box key,
// clean_cluster_alpha.comp). The first skinned vertex it used before is an arbitrary point
// of the face, so layered faces of one mesh (a bang over hair) swapped order as the camera
// orbited. Keyed by the range, not a frame: a held skin-cache range keeps its centre (and
// its key, the arena base) until it is re-skinned; a per-frame range is re-skinned -- and
// its centre rewritten -- every frame it is drawn.
// Words RC_EXT..+2 = that box's EXTENT (max - min, same space, same key; #835): the sort pulls
// the centre toward the camera by a quarter of it along the view direction (LL
// LLDrawable::updateDistance), so of two nested layers -- fur / onion-skin shells, whose
// centres coincide -- the enclosing one sorts nearer every frame instead of by noise.
#define RC_WORDS   16u
#define RC_STATE   6u
#define RC_SRC     8u
#define RC_CTR     9u
#define RC_CTR_KEY 12u
#define RC_EXT     13u

// The source vertex's texcoord (exact lattice decode, as clean_preskin.comp's old copy).
// ARR = a uint[] view of the static mega-VBO; src_base = the slot's first real vertex.
#define SV_SRC_UV(ARR, SRC_BASE, LOCAL) svSrcUV_impl(QV_LOAD_HEADER(ARR, max((SRC_BASE), QV_HEADER_VERTS)), \
                                                      ARR[((SRC_BASE) + (LOCAL)) * QV_WORDS + 2u])
vec2 svSrcUV_impl(QvHeader h, uint w2) { return qvUV(h, w2); }

#endif
