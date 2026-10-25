/**
 * @file llcleanrigdecimate.cpp
 * @brief Decimated intermediate LODs for rigged mesh (#561). See llcleanrigdecimate.h.
 */
#include "llviewerprecompiledheaders.h"

#include "llcleanrigdecimate.h"

#include "llcleanmeshasset.h"
#include "llviewercontrol.h"
#include "llvkhitch.h"
#include "threadpool.h"

#include "meshoptimizer.h"
#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <queue>
#include <sstream>
#include <unordered_map>

namespace
{
constexpr U32 kVertFloats   = 12;      // MirrorVertex {pos3, uv2, normal3, tangent4}
constexpr U32 kMaxInFlight  = 4;       // jobs on the pool at once
constexpr U32 kCopyVertsPerFrame = 196608;   // render-thread mirror copy per frame
constexpr U32 kAgeFrames    = 3600;    // steps nobody drew for ~a minute go
constexpr U32 kFailRetry    = 18000;   // a failed source is not retried for ~5 min
constexpr U32 kQueueMax     = 16384;
constexpr F32 kRatio[LLCleanRigDecimator::kSteps] = {0.5f, 0.25f, 0.125f};
// Relative error bound per step (meshopt target_error, of the mesh extent): the coarser
// steps may deviate more -- the cull only takes a step whose error, projected at the wearer's
// distance, stays under the pixel tolerance, so a larger bound costs nothing up close and lets
// the 1/4 and 1/8 steps actually reach their triangle targets.
constexpr F32 kTargetErr[LLCleanRigDecimator::kSteps] = {0.05f, 0.10f, 0.20f};
constexpr F32 kNoErr        = 65504.f; // half max: a missing step is never selected

// Pseudo-random unit vector per joint index: the skin-weight embedding's basis. Two
// vertices with the same influences embed to the same point; moving weight to another
// joint moves it by ~sqrt(2) x the weight -- what the simplifier's attribute metric
// penalizes, so collapses stay inside one skinning region (a finger never folds into
// the palm, a sleeve into the torso).
void jointBasis(U32 j, F32 out[3])
{
    U32 h = j * 2654435761u + 0x9E3779B9u;
    F32 v[3];
    for (int k = 0; k < 3; ++k)
    {
        h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
        v[k] = (F32)(h & 0xFFFFu) / 32767.5f - 1.f;
    }
    const F32 len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    for (int k = 0; k < 3; ++k) out[k] = len > 1e-4f ? v[k] / len : (k == 0 ? 1.f : 0.f);
}

// ---------------------------------------------------------------------------
// #612 UN-SUBDIVIDE: one level of Catmull-Clark undone by topology.
//
// Quad-modelled meshes (bodies, heads, some hand-modelled clothing) are often uploaded with
// a subdivision level applied: every cage face became n fine quads around a FACE POINT,
// every cage edge got an EDGE POINT. Removing those (keeping the cage's own vertices and
// re-forming the cage faces) is a clean ~4x step with the original edge flow -- better than
// any edge-collapse order a generic simplifier finds. Detection, on the position-welded
// topology (split UV / normal copies are one vertex):
//  1. Quads + labels, grown together: every fine quad of one CC level is (C, E, F, E) in
//     winding order -- C a coarse (cage) vertex, E edge points, F the face point opposite C.
//     A seed quad (the most rectangular / planar triangle pair left) is labelled in each of
//     its two rotations that are not C <-> F mirrors; the quad across each of its edges
//     follows (a quad edge is C-E or E-F, so the shared edge fixes the neighbour's labels;
//     its triangle's third corner is the corner next to one end or the other -- a label it
//     already has decides, else the more rectangular pair). Any contradiction rolls the
//     seed back (up to 16 seeds, then the mesh is rejected).
//  2. A grown component must have every E in 4 quads (2 on an open border); the C <-> F
//     mirror is decided by the borders (a face point is never on one), else by regularity
//     (face points of quad cages have valence 4: fewer irregular F wins).
//  3. Every triangle must end in a quad (else: < 90% -> not quad-dominant); every F
//     interior with >= 3 quads, and each F's quads close into one ring.
//  4. Coarse faces: around each F, its quads' C corners in winding order; open-border edge
//     points are KEPT (so the borders stay exactly where the mesh's other faces meet them --
//     no cracks, as the locked borders of the decimated steps), interior E and every F go.
//     Each corner takes the SOURCE vertex index its own fine quad used, so split UV / normal
//     copies are carried exactly; a face whose face point is split (a seam through the cage
//     face) rejects the mesh.
// Vertices are only REMOVED, never moved (the step invariant: byte-exact source copies, so
// validateSteps, the shadow LOD #564 and the RT replay are unchanged). Reverse subdivision
// (solving for the cage position, outside the smoothed surface) would remove the shrinkage
// but break that invariant; kept as is, the coarse faces chord across the smooth surface,
// which the error below measures: the largest distance of a removed vertex (F: to its
// coarse face; interior E: to the coarse edge it sat on) -- the same units as the
// simplifier's error (mesh space; the cull scales by the bind shape).
// ---------------------------------------------------------------------------
enum UnsubReason : U32
{
    UNSUB_OK = 0,
    UNSUB_PAIRING,     // not quad-dominant (or no consistent winding)
    UNSUB_LABELS,      // no consistent C / E / F labelling
    UNSUB_STRUCTURE,   // non-manifold, face point on a border, wrong valences, open ring
    UNSUB_SEAM,        // a UV / normal seam through a cage face
    UNSUB_ERROR,       // error over the step's bound
    UNSUB_RATIO,       // not enough triangles removed
    UNSUB_REASONS
};

struct V3
{
    F32 x, y, z;
};
inline V3 v3(const F32* p) { return {p[0], p[1], p[2]}; }
inline V3 sub(const V3& a, const V3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 add(const V3& a, const V3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 mul(const V3& a, F32 s) { return {a.x * s, a.y * s, a.z * s}; }
inline F32 dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(const V3& a, const V3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline F32 len(const V3& a) { return std::sqrt(dot(a, a)); }

// cos of the corner angle at `at` (-1 when degenerate)
F32 cosAt(const V3& at, const V3& a, const V3& b)
{
    const V3 u = sub(a, at), w = sub(b, at);
    const F32 d = len(u) * len(w);
    return d > 1e-20f ? std::clamp(dot(u, w) / d, -1.f, 1.f) : -1.f;
}

F32 pointSegDist(const V3& p, const V3& a, const V3& b)
{
    const V3 ab = sub(b, a);
    const F32 l2 = dot(ab, ab);
    const F32 t = l2 > 1e-20f ? std::clamp(dot(sub(p, a), ab) / l2, 0.f, 1.f) : 0.f;
    return len(sub(p, add(a, mul(ab, t))));
}

// Closest point on a triangle (Ericson, Real-Time Collision Detection 5.1.5).
F32 pointTriDist(const V3& p, const V3& a, const V3& b, const V3& c)
{
    const V3 ab = sub(b, a), ac = sub(c, a), ap = sub(p, a);
    const F32 d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0.f && d2 <= 0.f) return len(ap);
    const V3 bp = sub(p, b);
    const F32 d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0.f && d4 <= d3) return len(bp);
    const F32 vc = d1 * d4 - d3 * d2;
    if (vc <= 0.f && d1 >= 0.f && d3 <= 0.f) return pointSegDist(p, a, b);
    const V3 cp = sub(p, c);
    const F32 d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0.f && d5 <= d6) return len(cp);
    const F32 vb = d5 * d2 - d1 * d6;
    if (vb <= 0.f && d2 >= 0.f && d6 <= 0.f) return pointSegDist(p, a, c);
    const F32 va = d3 * d6 - d5 * d4;
    if (va <= 0.f && (d4 - d3) >= 0.f && (d5 - d6) >= 0.f) return pointSegDist(p, b, c);
    const F32 den = va + vb + vc;
    if (!(std::fabs(den) > 1e-30f)) return std::min({len(ap), len(bp), len(cp)});
    const F32 v = vb / den, w = vc / den;
    return len(sub(p, add(a, add(mul(ab, v), mul(ac, w)))));
}

// pos: MirrorVertex stream (kVertFloats per vertex); idx: the source triangles; weld:
// meshopt_generateShadowIndexBuffer by position (per index: the representative vertex).
// Out: the coarse triangles (source vertex indices) and the error. Returns UNSUB_*.
U32 unsubdivide(const F32* pos, size_t nv, const std::vector<U32>& idx, const std::vector<U32>& weld, F32 max_err,
                std::vector<U32>& out, F32& out_err)
{
    out.clear();
    out_err = 0.f;
    const size_t nt = idx.size() / 3;
    auto P = [&](U32 v) { return v3(pos + (size_t)v * kVertFloats); };
    auto tv = [&](size_t t, U32 k) { return weld[t * 3 + k]; };
    std::vector<U8> tri_ok(nt, 1);
    size_t valid = 0;
    for (size_t t = 0; t < nt; ++t)
    {
        if (tv(t, 0) == tv(t, 1) || tv(t, 1) == tv(t, 2) || tv(t, 0) == tv(t, 2)) tri_ok[t] = 0;
        else ++valid;
    }
    if (valid < 16) return UNSUB_PAIRING;

    // ---- topology: directed half-edges (tri * 3 + k: tv(t,k) -> tv(t,k+1)), borders
    // (per vertex: its outgoing half-edges, CSR -- a handful each)
    std::vector<U32> he_off(nv + 1, 0), he_list;
    for (size_t t = 0; t < nt; ++t) if (tri_ok[t]) for (U32 k = 0; k < 3; ++k) ++he_off[tv(t, k) + 1];
    for (size_t v = 0; v < nv; ++v) he_off[v + 1] += he_off[v];
    he_list.assign(he_off[nv], 0);
    {
        std::vector<U32> fill(he_off.begin(), he_off.end() - 1);
        for (size_t t = 0; t < nt; ++t) if (tri_ok[t]) for (U32 k = 0; k < 3; ++k) he_list[fill[tv(t, k)]++] = (U32)(t * 3 + k);
    }
    auto across = [&](U32 a, U32 b) -> S32   // the half-edge a -> b (-1 none)
    {
        for (U32 j = he_off[a]; j < he_off[a + 1]; ++j)
        {
            const U32 h = he_list[j];
            if (tv(h / 3, (h % 3 + 1) % 3) == b) return (S32)h;
        }
        return -1;
    };
    for (size_t v = 0; v < nv; ++v)   // non-manifold or inconsistent winding: one a -> b only
        for (U32 j = he_off[v]; j < he_off[v + 1]; ++j)
            for (U32 j2 = j + 1; j2 < he_off[v + 1]; ++j2)
                if (tv(he_list[j] / 3, (he_list[j] % 3 + 1) % 3) == tv(he_list[j2] / 3, (he_list[j2] % 3 + 1) % 3))
                    return UNSUB_STRUCTURE;
    // split: a position with more than one source copy (UV / normal seam through it)
    std::vector<U8> split(nv, 0);
    {
        std::vector<U32> first(nv, ~0u);
        for (size_t i = 0; i < idx.size(); ++i)
        {
            U32& f = first[weld[i]];
            if (f == ~0u) f = idx[i];
            else if (f != idx[i]) split[weld[i]] = 1;
        }
    }
    std::vector<U8> on_border(nv, 0);
    for (size_t t = 0; t < nt; ++t)
        if (tri_ok[t])
            for (U32 k = 0; k < 3; ++k)
            {
                const U32 a = tv(t, k), b = tv(t, (k + 1) % 3);
                if (across(b, a) < 0) { on_border[a] = 1; on_border[b] = 1; }
            }

    // How rectangular and planar the quad of half-edge he0 (a -> b) and its twin he1 is:
    // the largest corner-angle deviation from 90 degrees + half the fold between its
    // triangles (degrees; lower = better; 1e9 = not convex).
    auto quadScore = [&](U32 he0, U32 he1) -> F32
    {
        const U32 t0 = he0 / 3, k0 = he0 % 3, t1 = he1 / 3, k1 = he1 % 3;
        const U32 a = tv(t0, k0), b = tv(t0, (k0 + 1) % 3), c = tv(t0, (k0 + 2) % 3), d = tv(t1, (k1 + 2) % 3);
        if (c == d) return 1e9f;
        const V3 pa = P(a), pb = P(b), pc = P(c), pd = P(d);
        // quad loop a -> d -> b -> c
        const F32 cs[4] = {cosAt(pa, pd, pc), cosAt(pd, pb, pa), cosAt(pb, pc, pd), cosAt(pc, pa, pb)};
        F32 dev = 0.f;
        for (F32 x : cs)
        {
            if (!(x > -0.996f)) return 1e9f;   // >= ~175 degrees: not convex
            dev = std::max(dev, std::fabs(std::acos(x) * 57.29578f - 90.f));
        }
        const V3 n0 = cross(sub(pb, pa), sub(pc, pa)), n1 = cross(sub(pa, pb), sub(pd, pb));
        const F32 nl = len(n0) * len(n1);
        const F32 cf = nl > 1e-30f ? std::clamp(dot(n0, n1) / nl, -1.f, 1.f) : -1.f;
        return dev + 0.5f * std::acos(cf) * 57.29578f;
    };

    // ---- quads + labels (1 = C coarse, 2 = E edge point, 3 = F face point), grown from seeds
    struct Quad { U32 v[4]; U32 t[2]; };
    std::vector<Quad> quads;
    quads.reserve(valid / 2);
    std::vector<S8> rot;                  // per quad: the corner holding C
    std::vector<S32> mate(nt, -1);
    std::vector<U8> lab(nv, 0);
    std::vector<U32> qval(nv, 0);         // quads per vertex
    static const U8 kPat[4] = {1, 2, 3, 2};
    std::vector<U32> touched_v, queue;
    // Deferred (ambiguous) neighbour edges, woken when a vertex they wait on gets a label.
    struct Deferred { U32 u, v; bool done; };
    std::vector<Deferred> dents;
    std::unordered_map<U32, std::vector<U32>> dwait;   // vertex -> deferred entries
    std::vector<U32> ready;
    // the quad of he0 (a -> b) and its twin he1 (b -> a): (a, d, b, c) in winding order
    auto makeQuad = [&](U32 he0, U32 he1)
    {
        const U32 t0 = he0 / 3, k0 = he0 % 3, t1 = he1 / 3, k1 = he1 % 3;
        Quad q;
        q.v[0] = tv(t0, k0);
        q.v[1] = tv(t1, (k1 + 2) % 3);
        q.v[2] = tv(t0, (k0 + 1) % 3);
        q.v[3] = tv(t0, (k0 + 2) % 3);
        q.t[0] = t0; q.t[1] = t1;
        mate[t0] = (S32)t1; mate[t1] = (S32)t0;
        quads.push_back(q);
        rot.push_back(-1);
    };
    auto rotOf = [&](const Quad& q) -> int
    {
        for (int k = 0; k < 4; ++k)
        {
            if (lab[q.v[k]] == 1) return k;
            if (lab[q.v[k]] == 3) return (k + 2) & 3;
        }
        return -1;
    };
    auto applyRot = [&](U32 qi, int r) -> bool
    {
        const Quad& q = quads[qi];
        for (int k = 0; k < 4; ++k)
        {
            const U8 want = kPat[(k - r + 4) & 3];
            U8& l = lab[q.v[k]];
            if (l == 0)
            {
                l = want;
                touched_v.push_back(q.v[k]);
                if (!dwait.empty())
                {
                    auto it = dwait.find(q.v[k]);
                    if (it != dwait.end())
                    {
                        ready.insert(ready.end(), it->second.begin(), it->second.end());
                        dwait.erase(it);
                    }
                }
            }
            else if (l != want) return false;
        }
        rot[qi] = (S8)r;
        for (int k = 0; k < 4; ++k) ++qval[q.v[k]];
        return true;
    };
    auto rollback = [&](size_t q_from)
    {
        for (U32 v : touched_v) lab[v] = 0;
        for (size_t qi = q_from; qi < quads.size(); ++qi)
        {
            mate[quads[qi].t[0]] = -1; mate[quads[qi].t[1]] = -1;
            if (rot[qi] >= 0) for (int k = 0; k < 4; ++k) --qval[quads[qi].v[k]];
        }
        quads.resize(q_from);
        rot.resize(q_from);
    };
    // The neighbour quad across quad edge u -> v: its triangle t holds v -> u and a third
    // corner w; the quad's labels follow from u and v (a quad edge is C-E or E-F), and w is
    // either the corner next to u (diagonal v - w) or the one next to v (diagonal u - w). A
    // label w (or the other triangle's apex) already has decides. Both still possible: with
    // `guess` the more rectangular pair, else deferred (1) -- growing elsewhere usually
    // labels w first, so geometry only decides where nothing else does (skewed / stretched
    // fine quads fool it). 0 = done (a quad made, or none to make), -1 = contradiction.
    // gap: |score A - score B| of an ambiguous edge (how sure a guess would be).
    auto grow = [&](U32 u, U32 v, bool guess, F32* gap, U32* wait) -> int
    {
        const S32 he = across(v, u);
        if (he < 0 || mate[(U32)he / 3] >= 0) return 0;   // open border / already a quad
        const U32 t = (U32)he / 3, kk = (U32)he % 3;
        const U32 w = tv(t, (kk + 2) % 3);
        // the neighbour quad in winding order v, u, p (next to u), q (next to v)
        int r = -1;
        for (int rr = 0; rr < 4 && r < 0; ++rr)
            if (kPat[(0 - rr + 4) & 3] == lab[v] && kPat[(1 - rr + 4) & 3] == lab[u]) r = rr;
        if (r < 0) return -1;
        const U8 lp = kPat[(2 - r + 4) & 3], lq = kPat[(3 - r + 4) & 3];
        // option A: w = p, diagonal w -> v, twin v -> w (apex q); B: w = q, diagonal
        // u -> w, twin w -> u (apex p)
        U32 he0[2] = {t * 3 + (kk + 2) % 3, t * 3 + (kk + 1) % 3};
        S32 he1[2] = {across(v, w), across(w, u)};
        const U8 lw[2] = {lp, lq}, lapex[2] = {lq, lp};
        F32 sc[2] = {1e30f, 1e30f};
        int n_ok = 0, pick = -1;
        for (int o = 0; o < 2; ++o)
        {
            if (lab[w] != 0 && lab[w] != lw[o]) continue;
            if (he1[o] < 0) continue;
            const U32 m = (U32)he1[o] / 3;
            if (m == t || !tri_ok[m] || mate[m] >= 0) continue;
            const U32 apex = tv(m, ((U32)he1[o] % 3 + 2) % 3);
            if (apex == u || apex == v || apex == w || (lab[apex] != 0 && lab[apex] != lapex[o])) continue;
            sc[o] = quadScore(he0[o], (U32)he1[o]);
            ++n_ok;
            if (pick < 0 || sc[o] < sc[pick]) pick = o;
        }
        if (pick < 0) return -1;
        if (n_ok == 2 && !guess)
        {
            if (gap) *gap = std::fabs(sc[0] - sc[1]);
            if (wait)
            {
                // (w and both candidate apexes: unlabelled, since both options fit)
                wait[0] = w;
                wait[1] = tv((U32)he1[0] / 3, ((U32)he1[0] % 3 + 2) % 3);
                wait[2] = tv((U32)he1[1] / 3, ((U32)he1[1] % 3 + 2) % 3);
            }
            return 1;
        }
        const U32 qi = (U32)quads.size();
        makeQuad(he0[pick], (U32)he1[pick]);
        const int rq = rotOf(quads[qi]);
        if (rq < 0 || !applyRot(qi, rq)) return -1;
        queue.push_back(qi);
        return 0;
    };
    // A grown component is valid when every edge point has its 4 quads (2 on an open
    // border) and face points stay off the borders -- after the C <-> F mirror, which is
    // free: 0 invalid, 1 valid, 2 valid mirrored. (A regular grid also labels consistently
    // "diagonally" -- edge points on the cage's vertices and face points -- which this
    // rejects: a face point lands on a border, or an edge point on a valence-3 vertex.)
    // The mirror: face points are never on an open border, and no UV / normal seam runs
    // through one (seams follow cage edges); else face points of quad cages have valence 4,
    // so fewer irregular F wins (a closed, regular, seamless mesh may take either).
    auto componentCheck = [&]() -> int
    {
        bool f_border = false, c_border = false, f_split = false, c_split = false;
        U32 f_irr = 0, c_irr = 0;
        for (U32 v : touched_v)
        {
            const U32 val = qval[v];
            if (lab[v] == 2 && val != (on_border[v] ? 2u : 4u)) return 0;
            if (lab[v] == 3) { f_border |= on_border[v] != 0; f_split |= split[v] != 0; f_irr += val != 4u; }
            if (lab[v] == 1) { c_border |= on_border[v] != 0; c_split |= split[v] != 0; c_irr += val != 4u; }
        }
        if (f_border && c_border) return 0;
        if (f_border || c_border) return f_border ? 2 : 1;
        if (f_split != c_split) return f_split ? 2 : 1;
        return c_irr < f_irr ? 2 : 1;
    };
    // Seeds: the most rectangular / planar triangle pairs first (a skewed seed that turns out
    // inconsistent is rolled back; the next seed of the component usually grows it).
    struct Cand { F32 score; U32 he0, he1; };
    std::vector<Cand> cands;
    cands.reserve(valid * 3 / 2);
    for (size_t t = 0; t < nt; ++t)
    {
        if (!tri_ok[t]) continue;
        for (U32 k = 0; k < 3; ++k)
        {
        const U32 a = tv(t, k), b = tv(t, (k + 1) % 3);
        if (a > b) continue;   // (each undirected edge once)
        const S32 tw = across(b, a);
        if (tw < 0) continue;
        const F32 sc = quadScore((U32)(t * 3 + k), (U32)tw);
        if (sc < 60.f) cands.push_back({sc, (U32)(t * 3 + k), (U32)tw});
        }
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& x, const Cand& y) { return x.score < y.score; });
    U32 failed_seeds = 0;
    size_t wasted = 0;   // quads grown by failed attempts (work bound)
    for (const Cand& c : cands)
    {
        if (mate[c.he0 / 3] >= 0 || mate[c.he1 / 3] >= 0) continue;
        int valid_c = 0;
        bool contradicted = false;
        for (int r0 = 0; r0 < 2 && !valid_c; ++r0)
        {
            const size_t q_from = quads.size();
            touched_v.clear();
            queue.clear();
            makeQuad(c.he0, c.he1);
            bool ok = applyRot((U32)q_from, r0);
            if (ok) queue.push_back((U32)q_from);
            // grow: the determined neighbours first. An ambiguous edge waits (dwait) until a
            // vertex it depends on gets a label (ready); only when nothing is determined is
            // the surest one guessed (largest score gap; lazily invalidated heap).
            dents.clear(); dwait.clear(); ready.clear();
            std::priority_queue<std::pair<F32, U32>> guesses;
            auto defer = [&](U32 id, F32 gap, const U32* wv)
            {
                for (U32 j = 0; j < 3; ++j)
                {
                    std::vector<U32>& l = dwait[wv[j]];
                    if (l.empty() || l.back() != id) l.push_back(id);
                }
                guesses.emplace(gap, id);
            };
            size_t qi = 0;
            while (ok)
            {
                F32 gap = 0.f;
                U32 wv[3];
                if (qi < queue.size())
                {
                    const Quad q = quads[queue[qi++]];   // (copy: grow appends)
                    for (U32 k = 0; k < 4 && ok; ++k)
                    {
                        const int g = grow(q.v[k], q.v[(k + 1) & 3], false, &gap, wv);
                        if (g < 0) ok = false;
                        else if (g == 1)
                        {
                            dents.push_back({q.v[k], q.v[(k + 1) & 3], false});
                            defer((U32)dents.size() - 1u, gap, wv);
                        }
                    }
                }
                else if (!ready.empty())
                {
                    const U32 id = ready.back();
                    ready.pop_back();
                    if (dents[id].done) continue;
                    const int g = grow(dents[id].u, dents[id].v, false, &gap, wv);
                    if (g < 0) ok = false;
                    else if (g == 0) dents[id].done = true;
                    else defer(id, gap, wv);
                }
                else if (!guesses.empty())
                {
                    const U32 id = guesses.top().second;
                    guesses.pop();
                    if (dents[id].done) continue;
                    dents[id].done = true;
                    ok = grow(dents[id].u, dents[id].v, true, nullptr, nullptr) >= 0;
                }
                else break;
            }
            valid_c = ok ? componentCheck() : 0;
            if (!ok) contradicted = true;
            wasted += quads.size() - q_from;
            if (!valid_c) rollback(q_from);
        }
        if (!valid_c)
        {
            // A contradiction may be a bad seed (a skewed pair): try the next one -- at most
            // 16, within ~4x the mesh's quads (64k at least) of wasted growth. A component
            // that grew without one but has the wrong structure is not one subdivision level.
            if (!contradicted || ++failed_seeds > 16u || wasted > std::max<size_t>(2u * valid, 65536u))
                return UNSUB_LABELS;
            continue;
        }
        if (valid_c == 2)   // the C <-> F mirror
        {
            for (U32 v : touched_v) if (lab[v] != 2) lab[v] = (U8)(4 - lab[v]);
            for (size_t qi = 0; qi < queue.size(); ++qi) rot[queue[qi]] = (S8)((rot[queue[qi]] + 2) & 3);
        }
    }
    size_t unpaired = 0;
    for (size_t t = 0; t < nt; ++t) unpaired += tri_ok[t] && mate[t] < 0;
    if (unpaired * 10u > valid) return UNSUB_PAIRING;   // not quad-dominant
    if (unpaired) return UNSUB_LABELS;                    // quad-dominant, not one subdivision level

    // ---- structure: E in 4 quads (2 on a border) (componentCheck), F interior with >= 3
    //      quads; per vertex: its quads (CSR)
    std::vector<U32> vq_off(nv + 1, 0), vq;
    for (size_t v = 0; v < nv; ++v)
    {
        vq_off[v + 1] = vq_off[v] + qval[v];
        if (lab[v] == 3 && (on_border[v] || qval[v] < 3u)) return UNSUB_STRUCTURE;
    }
    vq.assign(vq_off[nv], 0);
    {
        std::vector<U32> fill(vq_off.begin(), vq_off.end() - 1);
        for (U32 q = 0; q < quads.size(); ++q) for (U32 k = 0; k < 4; ++k) vq[fill[quads[q].v[k]]++] = q;
    }

    // ---- 4. coarse faces around each face point
    auto srcOf = [&](const Quad& q, U32 topo) -> U32   // the source index q's triangles use for topo
    {
        for (U32 h = 0; h < 2; ++h)
            for (U32 k = 0; k < 3; ++k)
                if (weld[q.t[h] * 3 + k] == topo) return idx[q.t[h] * 3 + k];
        return ~0u;
    };
    out.reserve(quads.size() / 2 * 3 + 64);
    std::vector<U32> poly_topo, poly_src;
    std::vector<U8> poly_e;
    F32 err = 0.f;
    std::vector<U8> f_done(nv, 0);
    for (U32 qi = 0; qi < quads.size(); ++qi)
    {
        const U32 f = quads[qi].v[(rot[qi] + 2) & 3];
        if (f_done[f]) continue;
        f_done[f] = 1;
        const U32 val = vq_off[f + 1] - vq_off[f];
        // seam check: every triangle of F's quads names the same source copy of F
        U32 f_src = ~0u;
        for (U32 j = vq_off[f]; j < vq_off[f + 1]; ++j)
        {
            const Quad& q = quads[vq[j]];
            for (U32 h = 0; h < 2; ++h)
                for (U32 k = 0; k < 3; ++k)
                    if (weld[q.t[h] * 3 + k] == f)
                    {
                        const U32 s = idx[q.t[h] * 3 + k];
                        if (f_src == ~0u) f_src = s;
                        else if (s != f_src) return UNSUB_SEAM;
                    }
        }
        // ring: from a quad (C, Ea, F, Eb) to the one whose Eb is this Ea = the next corner
        // in winding order
        poly_topo.clear(); poly_src.clear(); poly_e.clear();
        U32 cur = qi;
        U32 steps = 0;
        do
        {
            const Quad& q = quads[cur];
            const int r = rot[cur];
            const U32 c = q.v[r], ea = q.v[(r + 1) & 3];
            poly_topo.push_back(c); poly_src.push_back(srcOf(q, c)); poly_e.push_back(0);
            if (on_border[ea]) { poly_topo.push_back(ea); poly_src.push_back(srcOf(q, ea)); poly_e.push_back(1); }
            U32 next = ~0u;
            for (U32 j = vq_off[f]; j < vq_off[f + 1]; ++j)
            {
                const U32 o = vq[j];
                if (quads[o].v[(rot[o] + 3) & 3] == ea && quads[o].v[(rot[o] + 2) & 3] == f) { next = o; break; }
            }
            if (next == ~0u) return UNSUB_STRUCTURE;   // the ring does not close
            if (!on_border[ea])
            {
                // interior edge point: it sat on the coarse edge C -> C(next)
                const U32 cn = quads[next].v[rot[next]];
                err = std::max(err, pointSegDist(P(ea), P(c), P(cn)));
            }
            cur = next;
        } while (cur != qi && ++steps <= val);
        if (cur != qi || steps + 1u != val) return UNSUB_STRUCTURE;
        const size_t m = poly_topo.size();
        for (size_t i = 0; i < m; ++i) if (poly_src[i] == ~0u) return UNSUB_STRUCTURE;
        // fan apex: a quad takes its shorter diagonal; with kept border points a corner not
        // next to one (no sliver along the border)
        size_t apex = 0;
        if (m == 4 && !poly_e[0] && !poly_e[1] && !poly_e[2] && !poly_e[3])
            apex = len(sub(P(poly_topo[0]), P(poly_topo[2]))) <= len(sub(P(poly_topo[1]), P(poly_topo[3]))) ? 0 : 1;
        else
            for (size_t i = 0; i < m; ++i)
                if (!poly_e[i] && !poly_e[(i + 1) % m] && !poly_e[(i + m - 1) % m]) { apex = i; break; }
        F32 f_err = 1e30f;
        const V3 pf = P(f);
        for (size_t i = 1; i + 1 < m; ++i)
        {
            const U32 a = (U32)apex, b = (U32)((apex + i) % m), c = (U32)((apex + i + 1) % m);
            out.push_back(poly_src[a]); out.push_back(poly_src[b]); out.push_back(poly_src[c]);
            f_err = std::min(f_err, pointTriDist(pf, P(poly_topo[a]), P(poly_topo[b]), P(poly_topo[c])));
        }
        if (m >= 3) err = std::max(err, f_err);
    }
    out_err = err;
    if (out.size() < 3) return UNSUB_STRUCTURE;
    if (out.size() * 100u > idx.size() * 32u) return UNSUB_RATIO;   // > 32% of the triangles kept
    if (!(err <= max_err)) return UNSUB_ERROR;
    return UNSUB_OK;
}
}

LLCleanRigDecimator& LLCleanRigDecimator::instance()
{
    static LLCleanRigDecimator s;
    return s;
}

LLCleanRigDecimator::LLCleanRigDecimator()
{
    mTable.assign((size_t)kTableCap * kEntryWords, 0u);
    for (U32 e = 0; e < kTableCap; ++e) mTable[(size_t)e * kEntryWords] = kEmpty;
}

LLCleanRigDecimator::~LLCleanRigDecimator()
{
    // stop() ran at app cleanup; nothing may touch the pool here.
}

U32 LLCleanRigDecimator::hashSlot(U32 src) const
{
    // = clean_cull.comp rigDecimFind: multiplicative hash, top log2(cap) bits.
    static_assert((kTableCap & (kTableCap - 1u)) == 0u, "table capacity must be a power of two");
    U32 bits = 0;
    while ((1u << bits) < kTableCap) ++bits;
    return (src * 2654435761u) >> (32u - bits);
}

LLCleanRigDecimator::Entry* LLCleanRigDecimator::lookup(U32 src)
{
    return src < mDense.size() ? mDense[src] : nullptr;
}

LLCleanRigDecimator::Entry& LLCleanRigDecimator::create(U32 src)
{
    Entry& e = mEntries[src];
    e = Entry();
    if (src >= mDense.size()) mDense.resize((size_t)src + 1 + src / 4, nullptr);
    mDense[src] = &e;   // (unordered_map nodes are stable)
    return e;
}

void LLCleanRigDecimator::erase(U32 src)
{
    if (src < mDense.size()) mDense[src] = nullptr;
    mEntries.erase(src);
}

// ---------------------------------------------------------------------------
// Requests (render thread)
// ---------------------------------------------------------------------------
void LLCleanRigDecimator::request(U32 src, U32 verts, U32 frame)
{
    if (mStopped || src == kEmpty) return;
    if (Entry* pe = lookup(src))
    {
        // (a failed source whose vertex count changed -- a recycled slot id -- is retried)
        if (pe->state != ST_FAILED || pe->src_verts == verts)
        {
            if (pe->state != ST_FAILED) pe->last_use = frame;
            return;
        }
        freeEntry(src, *pe);
        erase(src);
    }
    if (mQueue.size() >= kQueueMax) return;
    Entry& e = create(src);
    e.state = ST_QUEUED;
    ++mQueuedLive;
    e.last_use = frame;
    e.src_verts = verts;
    mQueue.emplace(verts, src);
}

void LLCleanRigDecimator::touch(U32 src, U32 frame)
{
    Entry* pe = lookup(src);
    if (pe && pe->state != ST_FAILED) pe->last_use = frame;
}

const LLCleanRigDecimator::Info* LLCleanRigDecimator::find(U32 src) const
{
    const Entry* pe = src < mDense.size() ? mDense[src] : nullptr;
    // (in the GPU table: a pending or table-full entry is invisible to the cull)
    return (pe && pe->state == ST_READY && pe->table != kEmpty) ? &pe->info : nullptr;
}

F32 LLCleanRigDecimator::bindScale(const U32* meta, size_t meta_words, U32 skin_id, size_t rec_words)
{
    if (!meta || rec_words < 16) return 1.f;
    const size_t b = (size_t)skin_id * rec_words + rec_words - 16u;
    if (b + 11u >= meta_words) return 1.f;
    F32 m[12];
    std::memcpy(m, meta + b, sizeof(m));
    F32 sc = 0.f;
    for (int c = 0; c < 3; ++c)
        sc = std::max(sc, std::sqrt(m[4 * c] * m[4 * c] + m[4 * c + 1] * m[4 * c + 1] + m[4 * c + 2] * m[4 * c + 2]));
    return (sc > 1.0e-4f && sc < 1.0e4f) ? sc : 1.f;
}

// ---------------------------------------------------------------------------
// Worker: attribute-aware simplification of one slot.
// ---------------------------------------------------------------------------
void LLCleanRigDecimator::runJob(Job* job)
{
    // Decode the source's quantized mirror range (#573) here, off the render thread.
    if (job->packed.size() > LLCleanMeshAsset::QV_HEADER)
    {
        const U32 pn = (U32)(job->packed.size() - LLCleanMeshAsset::QV_HEADER);
        std::vector<LLCleanMeshAsset::MirrorVertex> dec;
        LLCleanMeshAsset::decodeVerts(job->packed.data(), pn, dec);
        job->verts.resize((size_t)pn * kVertFloats);
        std::memcpy(job->verts.data(), dec.data(), (size_t)pn * sizeof(LLCleanMeshAsset::MirrorVertex));
        std::vector<LLCleanQV::QVertex>().swap(job->packed);
    }
    const size_t nv = job->verts.size() / kVertFloats;
    const size_t ni = job->idx.size();
    if (nv < 3 || ni < 3 || job->wts.size() != nv) { job->ok = false; return; }
    const F32* pos = job->verts.data();
    constexpr size_t stride = kVertFloats * sizeof(F32);

    // Attributes: normal (3), UV (2), skin embedding (3).
    constexpr size_t kAttr = 8;
    std::vector<F32> attr(nv * kAttr);
    for (size_t v = 0; v < nv; ++v)
    {
        const F32* mv = pos + v * kVertFloats;
        F32* a = attr.data() + v * kAttr;
        a[0] = mv[5]; a[1] = mv[6]; a[2] = mv[7];   // normal
        a[3] = mv[3]; a[4] = mv[4];                 // uv
        a[5] = a[6] = a[7] = 0.f;
        // (#582: the effective, normalized weights the pre-skin uses)
        U32 jn[4];
        F32 wt[4];
        LLCleanQV::decodeWeight(job->wts[v], jn, wt);
        for (int k = 0; k < 4; ++k)
        {
            if (!(wt[k] > 0.f)) continue;
            F32 b[3];
            jointBasis(jn[k], b);
            a[5] += wt[k] * b[0]; a[6] += wt[k] * b[1]; a[7] += wt[k] * b[2];
        }
    }
    const F32 attr_w[kAttr] = {0.5f, 0.5f, 0.5f, 1.f, 1.f, 0.6f, 0.6f, 0.6f};

    // Open borders: a closed-surface face's border is a seam to the mesh's other faces
    // (simplified separately) -- lock it or they crack apart. Card-like content (hair
    // cards, fringe, lace strips: most vertices on a border) has no such neighbour and
    // nothing to simplify inside, so its borders may move (meshopt keeps border edges on
    // the border).
    U32 options = 0;
    bool card_like = false;
    // (position-welded topology: split UV / normal copies are one vertex; also #612's input)
    std::vector<U32> weld(ni);
    meshopt_generateShadowIndexBuffer(weld.data(), job->idx.data(), ni, pos, nv, sizeof(F32) * 3, stride);
    {
        std::unordered_map<U64, U32> edges;
        edges.reserve(ni);
        for (size_t t = 0; t + 2 < ni; t += 3)
            for (int k = 0; k < 3; ++k)
            {
                U32 a = weld[t + k], b = weld[t + (k + 1) % 3];
                if (a == b) continue;
                if (a > b) std::swap(a, b);
                ++edges[((U64)a << 32) | b];
            }
        std::vector<U8> on_border(nv, 0), used(nv, 0);
        for (size_t t = 0; t < ni; ++t) used[weld[t]] = 1;
        for (const auto& kv : edges)
            if (kv.second == 1u) { on_border[(U32)(kv.first >> 32)] = 1; on_border[(U32)kv.first] = 1; }
        size_t nu = 0, nb = 0;
        for (size_t v = 0; v < nv; ++v) { nu += used[v]; nb += used[v] && on_border[v]; }
        card_like = nu && nb * 10 > nu * 3;   // > 30% of the vertices on a border
        if (!card_like) options |= meshopt_SimplifyLockBorder;
    }

    const F32 scale = meshopt_simplifyScale(pos, nv, stride);
    std::vector<U32> cur = job->idx;    // cascade input (references the original vertices)
    F32 err_acc = 0.f;
    bool unlocked = false;              // a step of the cascade moved the open borders
    std::vector<U32> out(ni);
    // Compact the vertices the step (cur) still uses (vertex fetch order), then re-order the
    // triangles for the post-transform cache. Every kept vertex is a byte copy of a source one.
    auto emitStep = [&](U32 s, F32 err)
    {
        const size_t n = cur.size();
        std::vector<U32> remap(nv);
        const size_t nu = meshopt_optimizeVertexFetchRemap(remap.data(), cur.data(), n, nv);
        Job::Step& st = job->steps[s];
        st.verts.resize(nu * kVertFloats);
        st.wts.resize(nu);
        for (size_t v = 0; v < nv; ++v)
        {
            const U32 r = remap[v];
            if (r == ~0u) continue;
            std::memcpy(&st.verts[(size_t)r * kVertFloats], pos + v * kVertFloats, kVertFloats * sizeof(F32));
            st.wts[r] = job->wts[v];   // byte copy: bit-identical to the source (#582)
        }
        st.idx.resize(n);
        for (size_t i = 0; i < n; ++i) st.idx[i] = remap[cur[i]];
        meshopt_optimizeVertexCache(st.idx.data(), st.idx.data(), n, nu);
        st.err = err;
    };
    for (U32 s = 0; s < kSteps; ++s)
    {
        // #612: the 1/4 step is the UN-SUBDIVIDED source when it has one level of
        // Catmull-Clark structure (unsubdivide): built from the source, not from step 0, its
        // error is its own; step 2 (1/8) then simplifies from it (cascade, borders as before).
        // Card-like content (mostly border) is never a subdivided closed surface: not tried.
        if (s == 1 && job->unsub && !card_like)
        {
            std::vector<U32> us;
            F32 uerr = 0.f;
            const U32 why = unsubdivide(pos, nv, job->idx, weld, kTargetErr[1] * scale, us, uerr);
            job->unsub_reason = why;
            job->unsub_err_rel = scale > 0.f ? uerr / scale : 0.f;
            if (why == UNSUB_OK)
            {
                job->unsub_tris = (U32)(us.size() / 3);
                cur.swap(us);
                err_acc = uerr;
                emitStep(s, unlocked ? err_acc * kUnlockedErrScale : err_acc);
                continue;
            }
        }
        const size_t target = std::max<size_t>((size_t)((F32)ni * kRatio[s]) / 3 * 3, 3);
        // The last step also drops disconnected bits smaller than its error (sub-pixel
        // trims, studs, lashes at that distance).
        const U32 opt_s = options | (s + 1 == kSteps ? (U32)meshopt_SimplifyPrune : 0u);
        F32 rel = 0.f;
        size_t n = meshopt_simplifyWithAttributes(out.data(), cur.data(), cur.size(), pos, nv, stride,
                                                  attr.data(), kAttr * sizeof(F32), attr_w, kAttr, nullptr,
                                                  target, kTargetErr[s], opt_s, &rel);
        // #561 tuning: the coarser steps (used far / small on screen) may move the open
        // borders when locking them stalls the simplification. The crack against the mesh's
        // other faces is bounded by the two sides' errors, so such a step (and every step
        // built from it) carries its error x kUnlockedErrScale: the cull then takes it only
        // where that crack stays under ~1 px (main-view tolerance <= 8 px).
        if (s > 0 && (opt_s & meshopt_SimplifyLockBorder) && (n < 3 || n * 20 > cur.size() * 17))
        {
            F32 rel2 = 0.f;
            const size_t n2 = meshopt_simplifyWithAttributes(out.data(), cur.data(), cur.size(), pos, nv, stride,
                                                             attr.data(), kAttr * sizeof(F32), attr_w, kAttr, nullptr,
                                                             target, kTargetErr[s], opt_s & ~(U32)meshopt_SimplifyLockBorder,
                                                             &rel2);
            if (n2 >= 3 && n2 * 20 <= cur.size() * 17)
            {
                n = n2;
                rel = rel2;
                unlocked = true;
                options &= ~(U32)meshopt_SimplifyLockBorder;   // (and every coarser step)
                job->unlocked_from = s;
            }
        }
        // No real progress (topology / error bound): this and every coarser step are out.
        if (n < 3 || n * 20 > cur.size() * 17) break;
        err_acc += rel * scale;
        cur.assign(out.begin(), out.begin() + n);
        emitStep(s, unlocked ? err_acc * kUnlockedErrScale : err_acc);
    }
    job->ok = !job->steps[0].idx.empty();
}

// ---------------------------------------------------------------------------
// Commit / table / eviction (render thread)
// ---------------------------------------------------------------------------
void LLCleanRigDecimator::commit(Job* job, U32 frame)
{
    Entry* pe = lookup(job->src);
    if (!pe || pe->job != job->id || pe->state != ST_RUNNING)
        return;   // invalidated (source changed / freed) while it ran
    Entry& e = *pe;
    // #612 statistics (every finished job that tried it)
    static_assert(UNSUB_REASONS == kUnsubReasons, "kUnsubReasons = the UNSUB_* count");
    if (job->unsub_reason != ~0u)
    {
        ++mStatUnsubTried;
        if (job->unsub_reason == UNSUB_OK)
        {
            ++mStatUnsubOk;
            mStatUnsubSrcTris += job->idx.size() / 3;
            mStatUnsubTris += job->unsub_tris;
            mStatUnsubErrSum += job->unsub_err_rel;
            mStatUnsubErrMax = std::max(mStatUnsubErrMax, job->unsub_err_rel);
        }
        else if (job->unsub_reason < kUnsubReasons)
            ++mStatUnsubFail[job->unsub_reason];
    }
    if (!job->ok)
    {
        e.state = ST_FAILED;
        e.last_use = frame;
        ++mFailed;
        return;
    }
    LLCleanMeshAsset& ms = LLCleanMeshAsset::instance();
    e.key.generate(llformat("slipstream-rigdecim:%u:%u", job->src, job->id));
    e.bytes = 0;
    bool any = false;
    for (U32 s = 0; s < kSteps; ++s)
    {
        e.info.dec[s] = kEmpty;
        e.info.err[s] = kNoErr;
        e.info.dec_verts[s] = 0;
        const Job::Step& st = job->steps[s];
        if (st.idx.empty()) continue;
        const U32 nv = (U32)(st.verts.size() / kVertFloats);
        // Quantized against the SOURCE's lattice (#573): a step's vertices are copies of
        // decoded source vertices, so they re-encode bit-identically -- the shadow-caster
        // LOD (llcleanrigshadowlod.cpp) matches step vertices to source ones exactly.
        const U32 sl = ms.captureRawSlot(e.key, LLCleanMeshAsset::NUM_LOD - 1, (S32)s,
                                         reinterpret_cast<const LLCleanMeshAsset::MirrorVertex*>(st.verts.data()),
                                         st.wts.data(),
                                         st.idx.data(), nv, (U32)st.idx.size(), false, &job->domain);
        if (sl == kEmpty) break;   // store refused: no coarser step either
        e.info.dec[s] = sl;
        // the value the GPU reads (half), so the CPU replays of its choice agree
        e.info.err[s] = glm::unpackHalf1x16(glm::packHalf1x16(std::min(std::max(st.err, 0.f), kNoErr)));
        e.info.dec_verts[s] = nv;
        e.bytes += (U64)nv * (LLCleanQV::kStride + LLCleanQV::kWeightStride) + (U64)st.idx.size() * 2u;   // (#573: 20 B verts, U16 idx; #582: 10 B weights)
        mStatStepTris += st.idx.size() / 3;
        mStatStepTrisS[s] += st.idx.size() / 3;
        ++mStatReach[s];
        any = true;
    }
    if (!any)
    {
        ms.freeAsset(e.key);
        e.state = ST_FAILED;
        e.last_use = frame;
        ++mFailed;
        return;
    }
    mStatSrcTris += job->idx.size() / 3;
    if (job->unlocked_from < kSteps) ++mStatUnlocked;
    e.info.src_ic = (U32)job->idx.size();
    e.info.src_vc = (U32)(job->verts.size() / kVertFloats);
    e.state = ST_READY;
    e.last_use = frame;
    ++mReady;
    ++mStatBuilt;
    mBytes += e.bytes;
    mPeakBytes = std::max(mPeakBytes, mBytes);
    // #573 regression fix: NOT in the GPU table yet. The step slots were just captured;
    // the store fill places their packed rows next frame. Before that the cull would read
    // the slot rows of these ids as they are on the GPU -- for a brand-new slot id the
    // never-written tail of the slot buffer (VK_WHOLE_SIZE, capacity headroom) -- and a
    // garbage row with a non-zero index_count passes slotOk: the OOB guard then drops the
    // whole draw (part missing) or it draws garbage ranges. flushTablePending inserts the
    // entry from the next pump on, after validating the steps.
    e.table = kEmpty;
    e.ready_frame = frame;
    e.pending_table = true;
    mTablePending.push_back(job->src);
}

bool LLCleanRigDecimator::validateSteps(U32 src, const Entry& e, bool diag)
{
    LLCleanMeshAsset& ms = LLCleanMeshAsset::instance();
    U32 svc = 0, sic = 0, swc = 0;
    const LLCleanMeshAsset::QVertex* sq = ms.cpuSlotPacked(src, svc);   // header-prefixed
    const U16* si = ms.cpuSlotIndices(src, sic);
    const LLCleanMeshAsset::MirrorWeight* sw = ms.cpuSlotWeights(src, swc);
    const char* why = nullptr;
    if (!sq || !si || !sw) why = "source mirror gone";
    else if (svc != e.info.src_vc || sic != e.info.src_ic || swc != svc) why = "source signature changed";
    LLCleanQV::QHeader sh{};
    if (!why) LLCleanQV::readHeader(sq, sh);
    U32 nsteps = 0;
    std::ostringstream detail;
    for (U32 s = 0; s < kSteps && !why; ++s)
    {
        const U32 d = e.info.dec[s];
        if (d == kEmpty) break;
        U32 vc = 0, ic = 0, wc = 0;
        const LLCleanMeshAsset::QVertex* q = ms.cpuSlotPacked(d, vc);
        const U16* ix = ms.cpuSlotIndices(d, ic);
        const LLCleanMeshAsset::MirrorWeight* w = ms.cpuSlotWeights(d, wc);
        if (!q || !ix || !w) { why = "step mirror missing"; break; }
        if (vc != e.info.dec_verts[s] || wc != vc) { why = "step vertex / weight count mismatch"; break; }
        if (ic < 3u || ic % 3u != 0u || ic > sic || vc > svc) { why = "step counts out of range"; break; }
        LLCleanQV::QHeader h;
        LLCleanQV::readHeader(q, h);
        if (std::memcmp(&h, &sh, sizeof(h)) != 0) { why = "step decode header != source header"; break; }
        U32 imax = 0;
        for (U32 i = 0; i < ic; ++i) imax = std::max<U32>(imax, ix[i]);
        if (imax >= vc) { why = "step index past its vertices"; break; }
        ++nsteps;
        if (diag)
        {
            // The first step vertices must be bit-identical copies (position + UV lattice
            // indices, weights) of source vertices (decoded error then exactly 0).
            const U32 nchk = std::min<U32>(vc, 8u);
            const LLCleanMeshAsset::QVertex* qv = q + LLCleanMeshAsset::QV_HEADER;
            const LLCleanMeshAsset::QVertex* sv = sq + LLCleanMeshAsset::QV_HEADER;
            U32 matched = 0;
            F32 max_err = 0.f;
            for (U32 v = 0; v < nchk; ++v)
            {
                F32 p[3];
                LLCleanQV::decodePos(h, qv[v], p);
                F32 best = 1e30f;
                bool exact = false;
                for (U32 k = 0; k < svc && !exact; ++k)
                {
                    F32 sp[3];
                    LLCleanQV::decodePos(sh, sv[k], sp);
                    const F32 dd = std::max(std::fabs(p[0] - sp[0]), std::max(std::fabs(p[1] - sp[1]), std::fabs(p[2] - sp[2])));
                    best = std::min(best, dd);
                    exact = std::memcmp(qv[v].pos, sv[k].pos, sizeof(qv[v].pos[0]) * 3) == 0
                         && std::memcmp(qv[v].uv, sv[k].uv, sizeof(qv[v].uv)) == 0
                         && std::memcmp(&w[v], &sw[k], sizeof(w[v])) == 0;
                }
                if (exact) ++matched;
                max_err = std::max(max_err, best);
            }
            detail << " s" << s << "{slot=" << d << " v=" << vc << " i=" << ic << " idx_max=" << imax
                   << " exact=" << matched << "/" << nchk << " max_pos_err=" << max_err << "}";
        }
    }
    if (why) ++mDiagBad;
    if (diag || (why && mDiagBad <= 32u))
    {
        LL_INFOS("VKRigDecim573") << "check src=" << src << " src_v=" << e.info.src_vc << " src_i=" << e.info.src_ic
            << " steps_ok=" << nsteps << detail.str() << " hdr c=(" << sh.pos_c[0] << "," << sh.pos_c[1] << ","
            << sh.pos_c[2] << ") s=(" << sh.pos_s[0] << "," << sh.pos_s[1] << "," << sh.pos_s[2] << ") -> "
            << (why ? "BAD: " : "OK") << (why ? why : "") << LL_ENDL;
    }
    return why == nullptr && nsteps != 0;
}

void LLCleanRigDecimator::flushTablePending(U32 frame)
{
    if (mTablePending.empty()) return;
    std::vector<U32> keep;
    for (U32 src : mTablePending)
    {
        Entry* pe = lookup(src);
        if (!pe || pe->state != ST_READY || !pe->pending_table) continue;
        if (pe->ready_frame == frame) { keep.push_back(src); continue; }   // committed this frame
        pe->pending_table = false;
        const bool diag = mDiagChecks < 8u;   // one-shot detail for the first few (VKRigDecim573)
        if (diag) ++mDiagChecks;
        if (!validateSteps(src, *pe, diag))
        {
            // Invalid steps: drop them; the source draws itself (negative-cached).
            freeEntry(src, *pe);
            pe->state = ST_FAILED;
            pe->last_use = frame;
            ++mFailed;
            continue;
        }
        pe->table = tableInsert(src, *pe);
    }
    mTablePending.swap(keep);
    if (!mDiagSummary && mDiagChecks >= 8u)
    {
        mDiagSummary = true;
        LL_INFOS("VKRigDecim573") << "summary: " << mDiagChecks << " detailed checks, " << mDiagBad
            << " bad so far (later failures are logged as they happen, up to 32)" << LL_ENDL;
    }
}

void LLCleanRigDecimator::freeEntry(U32 src, Entry& e)
{
    (void)src;
    const State was = e.state;
    const U32 tab = e.table;
    // Dead before the table changes: a rebuild (tableRemove) re-inserts READY entries only.
    e.state = ST_DEAD;
    e.pending_table = false;
    if (was == ST_QUEUED && mQueuedLive) --mQueuedLive;
    e.table = kEmpty;
    if (was == ST_READY)
    {
        LLCleanMeshAsset::instance().freeAsset(e.key);
        mBytes -= std::min(mBytes, e.bytes);
        if (mReady) --mReady;
        if (tab != kEmpty) tableRemove(tab);
    }
    else if (was == ST_FAILED && mFailed)
        --mFailed;
    e.bytes = 0;
}

void LLCleanRigDecimator::tableWrite(U32 idx, U32 src, const Entry& e)
{
    U32* w = mTable.data() + (size_t)idx * kEntryWords;
    w[0] = src;
    for (U32 s = 0; s < kSteps; ++s) w[1 + s] = e.info.dec[s];
    w[4] = (U32)glm::packHalf1x16(std::min(e.info.err[0], kNoErr)) | ((U32)glm::packHalf1x16(std::min(e.info.err[1], kNoErr)) << 16);
    w[5] = (U32)glm::packHalf1x16(std::min(e.info.err[2], kNoErr));
    w[6] = e.info.src_ic;
    w[7] = e.info.src_vc;
    ++mTableGen;
    if (!mTableFull) mTableDirty.push_back(idx);
}

U32 LLCleanRigDecimator::tableInsert(U32 src, const Entry& e)
{
    const U32 h = hashSlot(src);
    for (U32 k = 0; k < kProbes; ++k)
    {
        const U32 i = (h + k) & (kTableCap - 1u);
        const U32 key = mTable[(size_t)i * kEntryWords];
        if (key == kEmpty || key == kTomb)
        {
            if (key == kTomb && mTombs) --mTombs;
            tableWrite(i, src, e);
            return i;
        }
    }
    LLVKHitch::count("rigDecimTableFull");
    return kEmpty;   // probe chain full: the source draws undecimated
}

void LLCleanRigDecimator::tableRemove(U32 idx)
{
    if (idx >= kTableCap) return;
    U32* w = mTable.data() + (size_t)idx * kEntryWords;
    w[0] = kTomb;
    for (U32 s = 0; s < kSteps; ++s) w[1 + s] = kEmpty;
    ++mTombs;
    ++mTableGen;
    if (!mTableFull) mTableDirty.push_back(idx);
    if (mTombs > kTableCap / 8u) tableRebuild();
}

void LLCleanRigDecimator::tableRebuild()
{
    for (U32 e = 0; e < kTableCap; ++e)
    {
        U32* w = mTable.data() + (size_t)e * kEntryWords;
        std::fill(w, w + kEntryWords, 0u);
        w[0] = kEmpty;
        for (U32 s = 0; s < kSteps; ++s) w[1 + s] = kEmpty;
    }
    mTombs = 0;
    mTableFull = true;
    ++mTableGen;
    mTableDirty.clear();
    for (auto& kv : mEntries)
        if (kv.second.state == ST_READY && !kv.second.pending_table) kv.second.table = tableInsert(kv.first, kv.second);
        else kv.second.table = kEmpty;
}

bool LLCleanRigDecimator::takeTableDirty(std::vector<U32>& entries, bool& full)
{
    full = mTableFull;
    entries.clear();
    if (mTableFull)
    {
        mTableFull = false;
        mTableDirty.clear();
        return true;
    }
    if (mTableDirty.empty()) return false;
    entries.swap(mTableDirty);
    std::sort(entries.begin(), entries.end());
    entries.erase(std::unique(entries.begin(), entries.end()), entries.end());
    if (entries.size() > 1024u) { full = true; entries.clear(); }
    return true;
}

void LLCleanRigDecimator::noteDirty(const std::vector<U32>& slots)
{
    // Called from the store fill (possibly its worker thread, while the main thread runs
    // the UI pass): only record the ids; pump() (render thread, before this frame's cull
    // and table upload) drops the entries and frees their steps.
    if (slots.empty()) return;
    std::lock_guard<std::mutex> lk(mDirtyMutex);
    if (mPendingDirty.size() > 1000000u) mPendingDirty.clear();   // (bounded; ~never)
    mPendingDirty.insert(mPendingDirty.end(), slots.begin(), slots.end());
}

void LLCleanRigDecimator::markLive(std::vector<U8>& live) const
{
    for (const auto& kv : mEntries)
    {
        if (kv.second.state != ST_READY) continue;
        for (U32 s = 0; s < kSteps; ++s)
        {
            const U32 d = kv.second.info.dec[s];
            if (d != kEmpty && d < live.size()) live[d] = 1;
        }
    }
}

void LLCleanRigDecimator::pump(U32 frame, U64 budget_bytes)
{
    if (mStopped) return;
    // #612 A/B: RenderVKRigUnsubdivide changed -> every source rebuilds its steps (running
    // jobs find no entry at commit and are dropped; queued ids find none at launch).
    {
        static LLCachedControl<bool> s_unsub(gSavedSettings, "RenderVKRigUnsubdivide", true);
        if ((bool)s_unsub != mUnsubOn)
        {
            mUnsubOn = s_unsub;
            if (!mEntries.empty())
            {
                for (auto& kv : mEntries) freeEntry(kv.first, kv.second);
                mEntries.clear();
                std::fill(mDense.begin(), mDense.end(), nullptr);
                mTablePending.clear();
                mQueuedLive = 0;
                LL_INFOS("VKMem") << "rigged decimation: un-subdivide " << (mUnsubOn ? "on" : "off")
                                  << " -- every decimated step is rebuilt" << LL_ENDL;
            }
        }
    }
    // 0. sources whose content was rewritten / freed (noteDirty)
    {
        std::vector<U32> dirty;
        {
            std::lock_guard<std::mutex> lk(mDirtyMutex);
            dirty.swap(mPendingDirty);
        }
        if (!mEntries.empty())
            for (U32 sl : dirty)
            {
                Entry* pe = lookup(sl);
                if (!pe) continue;
                freeEntry(sl, *pe);   // (a RUNNING job's result finds no entry: dropped)
                erase(sl);
                ++mStatInvalidated;
            }
    }
    // 1. finished jobs
    {
        std::vector<U32> done;
        {
            std::lock_guard<std::mutex> lk(mDoneMutex);
            for (Job* j : mDone) done.push_back(j->id);
            mDone.clear();
        }
        for (U32 id : done)
        {
            auto jt = mJobs.find(id);
            if (jt == mJobs.end()) continue;
            if (mInFlight) --mInFlight;
            commit(jt->second.get(), frame);
            mJobs.erase(jt);
        }
    }
    // 1b. last frame's commits -> the GPU table (their slot rows are placed by now)
    flushTablePending(frame);
    // 2. age-out / budget eviction (a bounded number per frame)
    {
        U32 aged = 0;
        for (auto it = mEntries.begin(); it != mEntries.end() && aged < 32u;)
        {
            Entry& e = it->second;
            const bool stale = ((e.state == ST_READY || e.state == ST_QUEUED) && frame - e.last_use > kAgeFrames)
                            || (e.state == ST_FAILED && frame - e.last_use > kFailRetry);
            if (stale)
            {
                freeEntry(it->first, e);
                if (it->first < mDense.size()) mDense[it->first] = nullptr;
                it = mEntries.erase(it);
                ++aged;
                ++mStatAged;
            }
            else ++it;
        }
        for (U32 n = 0; n < 16u && mBytes > budget_bytes; ++n)
        {
            // least recently drawn ready entry, not drawn in the last ~half second
            auto victim = mEntries.end();
            for (auto it = mEntries.begin(); it != mEntries.end(); ++it)
                if (it->second.state == ST_READY && frame - it->second.last_use > 30u
                    && (victim == mEntries.end() || it->second.last_use < victim->second.last_use))
                    victim = it;
            if (victim == mEntries.end()) break;
            freeEntry(victim->first, victim->second);
            erase(victim->first);
            ++mStatEvicted;
        }
    }
    // 3. launch queued jobs: bounded in flight, bounded mirror copy per frame, within
    //    the byte budget and the table's load.
    mBudgetFull = mBytes >= budget_bytes || mReady + mInFlight >= kTableCap / 2u;
    if (mBudgetFull) return;
    LLCleanMeshAsset& ms = LLCleanMeshAsset::instance();
    U32 copied = 0;
    while (!mQueue.empty() && mInFlight < kMaxInFlight && copied < kCopyVertsPerFrame)
    {
        const U32 src = mQueue.top().second;
        mQueue.pop();
        Entry* pe = lookup(src);
        if (!pe || pe->state != ST_QUEUED) continue;
        Entry& e = *pe;
        U32 vc = 0, ic = 0, wc = 0;
        const LLCleanMeshAsset::QVertex* mv = ms.cpuSlotPacked(src, vc);   // header-prefixed (#573)
        const U16* mi = ms.cpuSlotIndices(src, ic);
        const LLCleanMeshAsset::MirrorWeight* mw = ms.cpuSlotWeights(src, wc);
        if (mQueuedLive) --mQueuedLive;
        if (!mv || !mi || !mw || vc < 3 || ic < 3 || wc != vc)
        {
            e.state = ST_FAILED;   // not a weighted mirror (or gone): negative-cache
            e.last_use = frame;
            ++mFailed;
            continue;
        }
        if (!mPool)
        {
            mPool.reset(new LL::ThreadPool("CleanRigDecimate", 2));
            mPool->start();
        }
        std::unique_ptr<Job> job(new Job());
        job->id = mNextJob++;
        if (mNextJob == 0u) mNextJob = 1u;
        job->src = src;
        job->unsub = mUnsubOn;   // #612 (the worker cannot read settings)
        job->packed.assign(mv, mv + vc + LLCleanMeshAsset::QV_HEADER);   // decoded by runJob
        LLCleanQV::readHeader(mv, job->domain);
        job->wts.assign(mw, mw + vc);
        job->idx.assign(mi, mi + ic);
        Job* raw = job.get();
        mJobs.emplace(raw->id, std::move(job));
        if (!mPool->getQueue().post([this, raw]()
            {
                runJob(raw);
                std::lock_guard<std::mutex> lk(mDoneMutex);
                mDone.push_back(raw);
            }))
        {
            mJobs.erase(raw->id);
            e.state = ST_FAILED;   // pool closing
            ++mFailed;
            break;
        }
        e.state = ST_RUNNING;
        e.job = raw->id;
        ++mInFlight;
        copied += vc;
    }
}

void LLCleanRigDecimator::reset()
{
    // The store is shutting down: its slots go with it, no frees.
    if (mPool) { mPool->close(); mPool.reset(); }
    {
        std::lock_guard<std::mutex> lk(mDoneMutex);
        mDone.clear();
    }
    mJobs.clear();
    mInFlight = 0;
    mQueuedLive = 0;
    mEntries.clear();
    mDense.clear();
    mTablePending.clear();
    while (!mQueue.empty()) mQueue.pop();
    mBytes = 0;
    mReady = mFailed = 0;
    for (U32 e = 0; e < kTableCap; ++e)
    {
        U32* w = mTable.data() + (size_t)e * kEntryWords;
        std::fill(w, w + kEntryWords, 0u);
        w[0] = kEmpty;
        for (U32 s = 0; s < kSteps; ++s) w[1 + s] = kEmpty;
    }
    mTombs = 0;
    mTableFull = true;
    mTableDirty.clear();
}

void LLCleanRigDecimator::stop()
{
    mStopped = true;
    if (mPool) { mPool->close(); mPool.reset(); }   // joins the workers
    {
        std::lock_guard<std::mutex> lk(mDoneMutex);
        mDone.clear();
    }
    mJobs.clear();
    mInFlight = 0;
}

std::string LLCleanRigDecimator::statsLine(U64 budget_bytes) const
{
    std::ostringstream o;
    o << "rig-decim " << mReady << " ready / " << mEntries.size() << " known (cap " << kTableCap / 2u << "), "
      << (mBytes >> 20) << "MB of " << (budget_bytes >> 20) << "MB (peak " << (mPeakBytes >> 20)
      << "MB; same again in the CPU mirror), queued " << mQueue.size() << " running " << mInFlight
      << " failed " << mFailed << ", built " << mStatBuilt << " evicted " << mStatEvicted << " aged " << mStatAged
      << " invalidated " << mStatInvalidated;
    if (mStatSrcTris)
    {
        // Per step: the triangles it kept of its sources' (sources that reached it; the
        // targets are 50 / 25 / 12.5%), and how many sources reached it. The old single
        // "all steps" figure summed the steps (87.5% = every target met) and read as if
        // decimation kept 87% of the triangles.
        o << ", tris kept per step";
        for (U32 k = 0; k < kSteps; ++k)
            o << (k ? " / " : " ") << (mStatReach[k] && mStatSrcTris
                                        ? 100.0 * (double)mStatStepTrisS[k] / (double)mStatSrcTris : 0.0)
              << "% (" << mStatReach[k] << ")";
        o << " of " << mStatBuilt << " sources, " << mStatUnlocked << " with unlocked borders";
    }
    // #612: how many sources had one level of subdivision structure (their 1/4 step is the
    // un-subdivided cage), the triangles that kept, its error (of the mesh extent) and why
    // the others did not qualify.
    o << "; unsubdivide " << (mUnsubOn ? "" : "OFF ") << mStatUnsubOk << " of " << mStatUnsubTried << " sources";
    if (mStatUnsubOk)
        o << " (tris kept " << 100.0 * (double)mStatUnsubTris / (double)std::max<U64>(mStatUnsubSrcTris, 1u)
          << "%, error avg " << 100.0 * mStatUnsubErrSum / (double)mStatUnsubOk << "% max " << 100.0 * mStatUnsubErrMax
          << "% of extent)";
    if (mStatUnsubTried > mStatUnsubOk)
        o << ", not: pairing " << mStatUnsubFail[UNSUB_PAIRING] << " labels " << mStatUnsubFail[UNSUB_LABELS]
          << " structure " << mStatUnsubFail[UNSUB_STRUCTURE] << " seams " << mStatUnsubFail[UNSUB_SEAM]
          << " error " << mStatUnsubFail[UNSUB_ERROR] << " ratio " << mStatUnsubFail[UNSUB_RATIO];
    o << "; drawn decimated " << mGpuInst << " instances, " << mGpuSaved << " verts saved";
    return o.str();
}
