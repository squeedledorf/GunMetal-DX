// vkclean — QUANTIZED static vertex (#573). Mirrors indra/newview/llcleanqvertex.h.
//
// One vertex = 20 B = 5 words of the static mega-VBO:
//   w0 = pos.x int16 | pos.y int16 << 16      position lattice index  p  = pos_c + q * pos_s
//   w1 = pos.z int16 | flags << 16            flags: 1 = has tangent, 2 = negative handedness
//   w2 = uv.x int16  | uv.y int16 << 16       texcoord lattice index  uv = uv_c + q * uv_s
//   w3 = normal  octahedral snorm16 x | y << 16
//   w4 = tangent octahedral snorm16 x | y << 16   (xyz only; none = zero tangent)
// Every slot's range is preceded by 2 pseudo-vertices (10 words) holding its decode
// header: pos_c.xyz, pos_s.xyz, uv_c.xy, uv_s.xy (floats). The slot's base_vertex is its first REAL
// vertex, so the header is always at word (base_vertex - 2) * 5. The lattice steps are
// powers of two and the centres multiples of them: pos_c + q * pos_s is exact in fp32.
//
// The draw VS gets the components through the vertex input (R16G16B16A16_SINT /
// R16G16_SINT / R16G16_SNORM x2) and only the header through a word view; compute / RT
// consumers read everything through a uint[] view of the same buffer.
#ifndef CLEAN_QVERTEX_GLSL
#define CLEAN_QVERTEX_GLSL

#define QV_WORDS        5u
#define QV_HEADER_VERTS 2u

struct QvHeader { vec3 pos_c; vec3 pos_s; vec2 uv_c; vec2 uv_s; };

int qvLo(uint w) { return int(w << 16) >> 16; }   // sign-extended low int16
int qvHi(uint w) { return int(w) >> 16; }         // sign-extended high int16

QvHeader qvHeaderFromWords(uint a0, uint a1, uint a2, uint a3, uint a4,
                           uint a5, uint a6, uint a7, uint a8, uint a9)
{
    QvHeader h;
    h.pos_c = vec3(uintBitsToFloat(a0), uintBitsToFloat(a1), uintBitsToFloat(a2));
    h.pos_s = vec3(uintBitsToFloat(a3), uintBitsToFloat(a4), uintBitsToFloat(a5));
    h.uv_c = vec2(uintBitsToFloat(a6), uintBitsToFloat(a7));
    h.uv_s = vec2(uintBitsToFloat(a8), uintBitsToFloat(a9));
    return h;
}
// The header of the slot whose first real vertex is BASE, from a uint[] view ARR.
#define QV_HDR_W(ARR, B, K) ARR[(B) + (K)]
#define QV_LOAD_HEADER(ARR, BASE) qvHeaderFromWords( \
    QV_HDR_W(ARR, (uint(BASE) - QV_HEADER_VERTS) * QV_WORDS, 0u), QV_HDR_W(ARR, (uint(BASE) - QV_HEADER_VERTS) * QV_WORDS, 1u), \
    QV_HDR_W(ARR, (uint(BASE) - QV_HEADER_VERTS) * QV_WORDS, 2u), QV_HDR_W(ARR, (uint(BASE) - QV_HEADER_VERTS) * QV_WORDS, 3u), \
    QV_HDR_W(ARR, (uint(BASE) - QV_HEADER_VERTS) * QV_WORDS, 4u), QV_HDR_W(ARR, (uint(BASE) - QV_HEADER_VERTS) * QV_WORDS, 5u), \
    QV_HDR_W(ARR, (uint(BASE) - QV_HEADER_VERTS) * QV_WORDS, 6u), QV_HDR_W(ARR, (uint(BASE) - QV_HEADER_VERTS) * QV_WORDS, 7u), \
    QV_HDR_W(ARR, (uint(BASE) - QV_HEADER_VERTS) * QV_WORDS, 8u), QV_HDR_W(ARR, (uint(BASE) - QV_HEADER_VERTS) * QV_WORDS, 9u))

// Octahedral decode of a snorm pair in [-1, 1] (unit length; (0, 0) = +Z).
vec3 qvOctDecode(vec2 e)
{
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    const float t = max(-n.z, 0.0);
    n.x += (n.x >= 0.0) ? -t : t;
    n.y += (n.y >= 0.0) ? -t : t;
    return normalize(n);
}
vec2 qvSnorm2(uint w) { return max(vec2(float(qvLo(w)), float(qvHi(w))) / 32767.0, vec2(-1.0)); }

// Word-level decoders (compute / RT); w0..w4 = the vertex's words.
vec3 qvPos(QvHeader h, uint w0, uint w1) { return h.pos_c + vec3(float(qvLo(w0)), float(qvHi(w0)), float(qvLo(w1))) * h.pos_s; }
vec2 qvUV(QvHeader h, uint w2)           { return h.uv_c + vec2(float(qvLo(w2)), float(qvHi(w2))) * h.uv_s; }
vec3 qvNormal(uint w3)                   { return qvOctDecode(qvSnorm2(w3)); }
vec4 qvTangent(uint w1, uint w4)
{
    const uint f = uint(qvHi(w1));
    if ((f & 1u) == 0u) return vec4(0.0);
    return vec4(qvOctDecode(qvSnorm2(w4)), (f & 2u) != 0u ? -1.0 : 1.0);
}

// 16-bit index i of a uint[] view of the index buffer (#573).
#define QV_INDEX16(ARR, I) ((((I) & 1u) != 0u) ? (ARR[(I) >> 1u] >> 16u) : (ARR[(I) >> 1u] & 0xFFFFu))

#endif
