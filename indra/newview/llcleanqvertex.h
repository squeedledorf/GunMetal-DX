/**
 * @file llcleanqvertex.h
 * @brief Quantized world-geometry vertex (#573): the 20 B vertex the VK mega-VBO, the CPU
 *        geometry mirror and every consumer (draw VS, shadows, pre-skin, RT, pick) share.
 *
 * LAYOUT (20 B = 5 words; mirrored by indra/vkclean/shaders/clean_qvertex.glsl):
 *   pos[0..2]  int16  position lattice index per axis      p  = c + q * s
 *   pos[3]     int16  flags: bit0 = has tangent, bit1 = negative handedness
 *   uv[0..1]   int16  texcoord lattice index per axis      uv = uc + q * us
 *   nrm[0..1]  snorm16 octahedral normal (zero normal -> +Z)
 *   tan[0..1]  snorm16 octahedral tangent (xyz only; w from the flags; none = zero tangent)
 *
 * SLOT HEADER. Every slot's vertex range is preceded by QV_HEADER_VERTS (2) pseudo-vertices
 * holding its decode record (40 B = 10 floats): pos c.xyz, pos s.xyz, uv c.xy, uv s.xy.
 * The slot's base_vertex points at its first REAL vertex, so the header always sits at
 * base_vertex - 2 (the draw VS finds it through gl_BaseVertex; compute / RT consumers
 * through the base they already carry). The header moves with the slot (the packer copies
 * header + vertices as one range).
 *
 * LATTICE. Per axis, the step s is a power of two and the centre c a multiple of s, so the
 * decode c + q * s is EXACT in fp32 (a multiple of s within 2^24 steps of 0) and identical
 * lattice points across slots that pick the same step coincide bit-for-bit (watertight
 * shared edges: a prim's faces, terrain grid columns at 1/8 m). s is the smallest power
 * of two whose +/-32767 steps cover the axis around c, floored so that every lattice point
 * is a representable float (|c| + 32767 s < 2^(e+24)). Error <= s/2 per axis:
 *   unit-space prim / mesh face (extent <= 1)         s = 2^-15  -> <= 1.5e-5 x object scale
 *     (64 m object: <= 1 mm; a legacy 256 m megaprim: <= 4 mm; grid / corner verts exact)
 *   region-local terrain / water (256 m)              s = 2^-7   -> xy grid (1/8 m) EXACT,
 *     z by its own (smaller) range, e.g. 40 m relief: s = 2^-10 -> <= 0.5 mm
 *   UV in [0,1]                                       s = 2^-15  -> <= 1.5e-5 (SL mesh UVs are
 *     already 16-bit in the asset; a 0..16 tiled range: s = 2^-11 -> <= 2.4e-4)
 * Normal / tangent octahedral 16-bit: <= ~0.005 deg.
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace LLCleanQV
{
struct QVertex
{
    int16_t pos[4];   // xyz lattice index, [3] = flags
    int16_t uv[2];
    int16_t nrm[2];   // octahedral snorm16
    int16_t tan[2];   // octahedral snorm16
};
static_assert(sizeof(QVertex) == 20, "QVertex must be 20 B (clean_qvertex.glsl)");

struct QHeader
{
    float pos_c[3];
    float pos_s[3];
    float uv_c[2];
    float uv_s[2];
};
static_assert(sizeof(QHeader) == 40, "QHeader must be 2 QVertex");

constexpr uint32_t kHeaderVerts = 2;            // pseudo-vertices before every slot's range
constexpr uint32_t kStride      = 20;           // bytes per vertex
constexpr int16_t  kFlagTangent = 1;
constexpr int16_t  kFlagTanNeg  = 2;
// Largest face-local index a 16-bit index buffer can name (one slot = one face).
constexpr uint32_t kMaxSlotVerts = 65536;

// Lattice (c, s) for one axis covering [lo, hi] (see the file comment).
inline void axisLattice(double lo, double hi, float& c, float& s)
{
    if (!std::isfinite(lo) || !std::isfinite(hi) || lo > hi) { lo = 0.0; hi = 0.0; }
    const double half = 0.5 * (hi - lo), mid = 0.5 * (hi + lo);
    const double amax = std::max(std::fabs(lo), std::fabs(hi));
    int e = -24;
    if (half > 0.0) e = std::max(e, (int)std::ceil(std::log2(half / 32766.0)));
    if (amax > 0.0) e = std::max(e, (int)std::ceil(std::log2(amax)) - 23);
    for (int guard = 0; guard < 160; ++guard, ++e)
    {
        const double sd = std::ldexp(1.0, e);
        const double cd = std::nearbyint(mid / sd) * sd;
        if (cd - lo <= 32767.0 * sd && hi - cd <= 32767.0 * sd)
        {
            c = (float)cd;
            s = (float)sd;
            return;
        }
    }
    c = 0.f; s = 1.f;   // (unreachable for finite input)
}

inline int16_t latticeIndex(float v, float c, float s)
{
    if (!std::isfinite(v)) v = c;
    const double q = std::nearbyint(((double)v - (double)c) / (double)s);
    return (int16_t)std::min(32767.0, std::max(-32767.0, q));
}

inline int16_t snorm16(float v)
{
    if (!std::isfinite(v)) v = 0.f;
    return (int16_t)std::lrint(std::min(1.f, std::max(-1.f, v)) * 32767.f);
}

// Octahedral encode of a direction (any length; zero / non-finite -> +Z).
inline void octEncode(const float d[3], int16_t out[2])
{
    float x = d[0], y = d[1], z = d[2];
    const float l1 = std::fabs(x) + std::fabs(y) + std::fabs(z);
    if (!(l1 > 1e-30f) || !std::isfinite(l1)) { out[0] = 0; out[1] = 0; return; }
    x /= l1; y /= l1; z /= l1;
    if (z < 0.f)
    {
        const float ox = (1.f - std::fabs(y)) * (x >= 0.f ? 1.f : -1.f);
        const float oy = (1.f - std::fabs(x)) * (y >= 0.f ? 1.f : -1.f);
        x = ox; y = oy;
    }
    out[0] = snorm16(x);
    out[1] = snorm16(y);
}

// Octahedral decode (unit length), matching clean_qvertex.glsl qvOctDecode.
inline void octDecode(const int16_t in[2], float out[3])
{
    float x = std::max(-1.f, (float)in[0] / 32767.f), y = std::max(-1.f, (float)in[1] / 32767.f);
    const float z = 1.f - std::fabs(x) - std::fabs(y);
    const float t = std::max(-z, 0.f);
    x += (x >= 0.f) ? -t : t;
    y += (y >= 0.f) ? -t : t;
    const float l = std::sqrt(x * x + y * y + z * z);
    if (l > 0.f) { out[0] = x / l; out[1] = y / l; out[2] = z / l; }
    else         { out[0] = 0.f; out[1] = 0.f; out[2] = 1.f; }
}

// Build the decode record for `n` float vertices given as strided arrays (stride in floats).
// pos / uv may be null (uv null = all zero).
inline void makeHeader(const float* pos, uint32_t pos_stride, const float* uv, uint32_t uv_stride,
                       uint32_t n, QHeader& h)
{
    double lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0}, ulo[2] = {0, 0}, uhi[2] = {0, 0};
    bool first = true, ufirst = true;
    for (uint32_t v = 0; v < n; ++v)
    {
        if (pos)
        {
            const float* p = pos + (size_t)v * pos_stride;
            if (std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]))
            {
                for (int k = 0; k < 3; ++k)
                {
                    lo[k] = first ? p[k] : std::min(lo[k], (double)p[k]);
                    hi[k] = first ? p[k] : std::max(hi[k], (double)p[k]);
                }
                first = false;
            }
        }
        if (uv)
        {
            const float* t = uv + (size_t)v * uv_stride;
            if (std::isfinite(t[0]) && std::isfinite(t[1]))
            {
                for (int k = 0; k < 2; ++k)
                {
                    ulo[k] = ufirst ? t[k] : std::min(ulo[k], (double)t[k]);
                    uhi[k] = ufirst ? t[k] : std::max(uhi[k], (double)t[k]);
                }
                ufirst = false;
            }
        }
    }
    for (int k = 0; k < 3; ++k) axisLattice(lo[k], hi[k], h.pos_c[k], h.pos_s[k]);
    for (int k = 0; k < 2; ++k) axisLattice(ulo[k], uhi[k], h.uv_c[k], h.uv_s[k]);
}

// Encode one vertex against `h`. tan may be null (= none); tan[3] < 0 = negative handedness.
inline void encode(const QHeader& h, const float pos[3], const float uv[2], const float nrm[3],
                   const float* tan, QVertex& o)
{
    for (int k = 0; k < 3; ++k) o.pos[k] = latticeIndex(pos[k], h.pos_c[k], h.pos_s[k]);
    o.uv[0] = latticeIndex(uv ? uv[0] : 0.f, h.uv_c[0], h.uv_s[0]);
    o.uv[1] = latticeIndex(uv ? uv[1] : 0.f, h.uv_c[1], h.uv_s[1]);
    octEncode(nrm, o.nrm);
    int16_t flags = 0;
    o.tan[0] = o.tan[1] = 0;
    if (tan)
    {
        const float t2 = tan[0] * tan[0] + tan[1] * tan[1] + tan[2] * tan[2];
        if (t2 > 1e-24f && std::isfinite(t2))
        {
            flags |= kFlagTangent;
            if (tan[3] < 0.f) flags |= kFlagTanNeg;
            octEncode(tan, o.tan);
        }
    }
    o.pos[3] = flags;
}

inline void decodePos(const QHeader& h, const QVertex& v, float out[3])
{
    for (int k = 0; k < 3; ++k) out[k] = h.pos_c[k] + (float)v.pos[k] * h.pos_s[k];
}
inline void decodeUV(const QHeader& h, const QVertex& v, float out[2])
{
    for (int k = 0; k < 2; ++k) out[k] = h.uv_c[k] + (float)v.uv[k] * h.uv_s[k];
}
inline void decodeNormal(const QVertex& v, float out[3]) { octDecode(v.nrm, out); }
// xyz unit + w = +/-1, or all zero when the vertex has no tangent.
inline void decodeTangent(const QVertex& v, float out[4])
{
    if ((v.pos[3] & kFlagTangent) == 0) { out[0] = out[1] = out[2] = out[3] = 0.f; return; }
    octDecode(v.tan, out);
    out[3] = (v.pos[3] & kFlagTanNeg) ? -1.f : 1.f;
}

// The header stored in the first kHeaderVerts entries of a header-prefixed range.
inline void readHeader(const QVertex* range, QHeader& h) { std::memcpy(&h, range, sizeof(QHeader)); }
inline void writeHeader(QVertex* range, const QHeader& h) { std::memcpy(range, &h, sizeof(QHeader)); }
// Same, from raw packed bytes at a slot's base_vertex (a header-prefixed range: pass
// base_vertex = kHeaderVerts and vert_n = its vertex count + kHeaderVerts).
inline bool headerAt(const uint8_t* verts, size_t vert_n, int64_t base_vertex, QHeader& h)
{
    if (base_vertex < (int64_t)kHeaderVerts || (size_t)base_vertex > vert_n) return false;
    std::memcpy(&h, verts + (size_t)(base_vertex - kHeaderVerts) * kStride, sizeof(QHeader));
    return true;
}

// VkTransformMatrixKHR rows (3x4, row-major) mapping the R16G16B16A16_SNORM view of the
// position (q / 32767) to the decoded position: diag(32767 s) + c. RT BLAS builds.
inline void snormTransform(const QHeader& h, float m[12])
{
    std::memset(m, 0, sizeof(float) * 12);
    for (int k = 0; k < 3; ++k)
    {
        m[k * 4 + k] = h.pos_s[k] * 32767.f;
        m[k * 4 + 3] = h.pos_c[k];
    }
}

// ---- SKIN WEIGHTS (#582) -------------------------------------------------------------
// One rigged vertex's 4 influences in 10 B (align 2, no padding: byte copies and memcmp
// are valid; mirrored by clean_preskin.comp's decode):
//   j[0..3]  uint8   joint index per influence (unused influence: 0)
//   w[0..2]  unorm16 weight of influences 0..2; influence 3's weight is IMPLIED:
//            65535 - (w0 + w1 + w2) (never negative for an encoder-produced value)
// The VK weight arena packs these back to back (10 B stride); the pre-skin reads it as a
// uint[] (a weight starts on a 2-byte boundary, so it always spans exactly 3 words).
struct QWeight
{
    uint8_t  j[4];
    uint16_t w[3];
};
static_assert(sizeof(QWeight) == 10, "QWeight must be 10 B (clean_preskin.comp)");
constexpr uint32_t kWeightStride = 10;
constexpr uint32_t kWeightOne    = 65535;
// RIGGED WELDING (#574): bit 7 of j[3] (influence 3's joint byte) marks an OPEN-BOUNDARY
// vertex (markWeldBoundary). Joint indices are < 128 (LL_MAX_JOINTS_PER_MESH_OBJECT is
// 110; the encoder clamps to 127, which the pre-skin's clamp to 109 makes equivalent),
// so the bit is free; the pre-skin masks it and decodeWeight below does too.
constexpr uint8_t  kWeldBoundaryBit = 0x80;
constexpr uint8_t  kJointMask       = 0x7F;

// Encode LL's float-per-influence weight4 (integer part = joint index, fraction = weight;
// llvolume.cpp / llcleansysavatar.cpp) into the effective weights the float pre-skin used:
// fract / floor exactly as GLSL (x - floor(x)), negative joint -> 0 (the shader's clamp),
// normalized by the sum, zero sum -> (1, 0, 0, 0) on influence 0's joint. The normalized
// weights are apportioned to integers summing to EXACTLY 65535 (floor, then the leftover
// units to the largest remainders, ties to the lower influence), so the implied 4th weight
// is exact and >= 0, and the decoded weights match the normalized floats within 1/65535.
// A pure function of the 4 input floats: identical source weights encode identically
// (seams between meshes deform identically). Zero-weight influences get joint 0.
// Returns false (out zeroed) when a joint index exceeds 255: the caller drops the slot's
// weights rather than wrapping. A non-finite influence counts as weight 0 on joint 0.
inline bool encodeWeight(const float lw[4], QWeight& out)
{
    out = QWeight{};
    uint32_t jnt[4] = {0u, 0u, 0u, 0u};
    double frac[4] = {0.0, 0.0, 0.0, 0.0};
    double sum = 0.0;
    for (int k = 0; k < 4; ++k)
    {
        const float x = lw[k];
        if (!std::isfinite(x)) continue;
        const float fl = std::floor(x);
        if (fl > 255.f) return false;
        // Clamped to 127 (kJointMask): the pre-skin clamps every index to 109 anyway, so
        // 128..255 behave the same, and bit 7 stays free for the weld flag (#574).
        jnt[k] = fl > 0.f ? std::min((uint32_t)fl, (uint32_t)kJointMask) : 0u;
        const float f = x - fl;   // == GLSL fract(x), in fp32 like the GPU did
        frac[k] = f > 0.f ? (double)f : 0.0;
        sum += frac[k];
    }
    if (!(sum > 0.0))
    {
        out.j[0] = (uint8_t)jnt[0];
        out.w[0] = (uint16_t)kWeightOne;
        return true;
    }
    uint32_t q[4];
    double rem[4];
    uint32_t total = 0;
    for (int k = 0; k < 4; ++k)
    {
        const double sc = frac[k] / sum * (double)kWeightOne;
        const double fl = std::floor(sc);
        q[k] = fl >= (double)kWeightOne ? kWeightOne : (uint32_t)fl;
        rem[k] = sc - fl;
        total += q[k];
    }
    // sum(sc) == 65535 (to rounding) and each q <= sc, so total <= 65535 and the leftover
    // is at most 4 units -- one per influence by largest remainder.
    while (total < kWeightOne)
    {
        int best = 0;
        for (int k = 1; k < 4; ++k)
            if (rem[k] > rem[best]) best = k;
        if (rem[best] < 0.0)
        {   // (more leftover than influences: rounding pathology) the largest weight absorbs it
            for (int k = 1; k < 4; ++k)
                if (q[k] > q[best]) best = k;
            q[best] += kWeightOne - total;
            total = kWeightOne;
            break;
        }
        ++q[best];
        ++total;
        rem[best] = -1.0;
    }
    while (total > kWeightOne)
    {   // (defensive: cannot happen with the floors above) the largest weight gives it back
        int best = 0;
        for (int k = 1; k < 4; ++k)
            if (q[k] > q[best]) best = k;
        const uint32_t d = std::min(q[best], total - kWeightOne);
        q[best] -= d;
        total -= d;
    }
    for (int k = 0; k < 4; ++k)
        out.j[k] = q[k] ? (uint8_t)jnt[k] : (uint8_t)0;
    out.w[0] = (uint16_t)q[0];
    out.w[1] = (uint16_t)q[1];
    out.w[2] = (uint16_t)q[2];
    return true;
}

// The effective joint indices + weights (sum 1) of an encoded vertex (CPU consumers;
// the same arithmetic as clean_preskin.comp).
inline void decodeWeight(const QWeight& in, uint32_t joint[4], float wt[4])
{
    const uint32_t s = (uint32_t)in.w[0] + in.w[1] + in.w[2];
    const uint32_t q3 = s >= kWeightOne ? 0u : kWeightOne - s;
    const float inv = 1.f / (float)kWeightOne;
    for (int k = 0; k < 3; ++k) wt[k] = (float)in.w[k] * inv;
    wt[3] = (float)q3 * inv;
    for (int k = 0; k < 4; ++k) joint[k] = in.j[k] & kJointMask;   // (j[3] bit 7: weld flag)
}

inline bool isWeldBoundary(const QWeight& w) { return (w.j[3] & kWeldBoundaryBit) != 0; }

// RIGGED WELDING (#574): flag the slot's OPEN-BOUNDARY vertices (an edge used by exactly
// one triangle) in their weight records, from the slot's own topology -- every LOD and
// decimated step is flagged on its own triangles. Edges are taken BY POSITION (vertices
// at bit-identical positions are one vertex), so UV / normal seams inside a mesh are not
// boundaries; a mesh's real open edges are. Clears stale flags first (decimated steps
// inherit their source's records). pos: float xyz at `stride` floats per vertex.
// Indices >= nv and degenerate triangles are skipped. Pure CPU, any thread.
template <typename IndexT>
inline uint32_t markWeldBoundary(QWeight* w, uint32_t nv, const IndexT* idx, uint32_t ni,
                                 const float* pos, size_t stride)
{
    if (!w || !idx || !pos || nv == 0) return 0;
    for (uint32_t v = 0; v < nv; ++v) w[v].j[3] &= kJointMask;
    if (ni < 3) return 0;
    // Canonical vertex per exact position.
    std::vector<uint32_t> canon(nv);
    {
        std::unordered_map<uint64_t, uint32_t> first;
        first.reserve(nv);
        for (uint32_t v = 0; v < nv; ++v)
        {
            const float* p = pos + (size_t)v * stride;
            uint32_t b[3];
            std::memcpy(b, p, sizeof(b));
            // (-0 == +0 as positions)
            for (uint32_t& x : b) if (x == 0x80000000u) x = 0u;
            const uint64_t h = ((uint64_t)b[0] * 0x9E3779B97F4A7C15ull) ^ ((uint64_t)b[1] * 0xC2B2AE3D27D4EB4Full)
                             ^ ((uint64_t)b[2] * 0x165667B19E3779F9ull);
            // Collisions of the 64-bit hash would merge two positions: compare exactly.
            auto it = first.find(h);
            uint32_t c = v;
            if (it != first.end())
            {
                const float* q = pos + (size_t)it->second * stride;
                if (q[0] == p[0] && q[1] == p[1] && q[2] == p[2]) c = it->second;
            }
            else
                first.emplace(h, v);
            canon[v] = c;
        }
    }
    std::unordered_map<uint64_t, uint32_t> edges;
    edges.reserve((size_t)ni);
    auto key = [](uint32_t a, uint32_t b) { return a < b ? ((uint64_t)a << 32) | b : ((uint64_t)b << 32) | a; };
    for (uint32_t t = 0; t + 2 < ni; t += 3)
    {
        const uint32_t i0 = (uint32_t)idx[t], i1 = (uint32_t)idx[t + 1], i2 = (uint32_t)idx[t + 2];
        if (i0 >= nv || i1 >= nv || i2 >= nv) continue;
        const uint32_t a = canon[i0], b = canon[i1], c = canon[i2];
        if (a == b || b == c || a == c) continue;
        ++edges[key(a, b)];
        ++edges[key(b, c)];
        ++edges[key(c, a)];
    }
    std::vector<uint8_t> boundary(nv, 0);
    for (const auto& e : edges)
    {
        if (e.second != 1) continue;
        boundary[(uint32_t)(e.first >> 32)] = 1;
        boundary[(uint32_t)(e.first & 0xFFFFFFFFu)] = 1;
    }
    uint32_t marked = 0;
    for (uint32_t v = 0; v < nv; ++v)
        if (boundary[canon[v]])
        {
            w[v].j[3] |= kWeldBoundaryBit;
            ++marked;
        }
    return marked;
}
} // namespace LLCleanQV
