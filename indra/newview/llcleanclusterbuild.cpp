/**
 * @file llcleanclusterbuild.cpp
 * @brief Cluster LOD ingest (#744, OWNER: 1a): cluster DAG builder + page packer. See
 *        llcleanclusterbuild.h and CLUSTER_LOD_CONTRACT.md.
 *
 * Credits: meshoptimizer (c) Arseny Kapoulkine, MIT licence. The DAG loop (clusterize ->
 * partition into groups -> lock group borders -> simplify each group to half -> re-split; a
 * stuck group's clusters retry at the next level; group bounds = sphere merge of the members'
 * LOD bounds, group error = max of the members' errors + the simplification error) follows its
 * demo/nanite.cpp (MIT). Method otherwise from published material (SIGGRAPH 2021 "A Deep
 * Dive into Nanite Virtualized Geometry"); no engine source.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
 * $/LicenseInfo$
 */
#include "llviewerprecompiledheaders.h"

#include "llcleanclusterbuild.h"

#include "llcleanmeshasset.h"
#include "llcleanqvertex.h"
#include "llviewercontrol.h"
#include "threadpool.h"

#include "meshoptimizer.h"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iterator>
#include <mutex>
#include <queue>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>

using namespace LLClusterFmt;
using LLCleanQV::QHeader;
using LLCleanQV::QVertex;
using LLCleanQV::QWeight;

namespace
{
constexpr U32 kQueueMax          = 4096;     // queued faces (request refuses beyond)
constexpr U32 kCopyVertsPerFrame = 262144;   // render-thread mirror copy per pump (~5 MB)
constexpr U32 kMaxResults        = 1024;     // finished, not yet taken
constexpr F32 kStuckRatio        = 0.85f;    // a group that keeps more than this is stuck (demo)
constexpr U32 kMeshletMinTris    = 32;       // meshopt min_triangles (% 4; >= kMinClusterTris)
constexpr U32 kNoGroup           = 0xFFFFFFFFu;
constexpr U32 kDeepMaxTris       = 300000;   // debug cut sampling bound
constexpr U32 kAttr              = 8;        // normal 3, uv 2, skin embedding 3
constexpr F32 kAttrW[kAttr]      = {0.5f, 0.5f, 0.5f, 1.f, 1.f, 0.6f, 0.6f, 0.6f};   // = the #561 decimator's
constexpr F32 kInitMsPerTri      = 0.003f;   // ms / triangle average before any build finished (stats)
constexpr U64 kInflightBytes     = 256ull << 20;   // source copies of jobs in flight (admission stops above)
constexpr F32 kPrioStep          = 1.25f;    // a repeat request re-sorts the queue only past this factor
constexpr F64 kRateWindowMs      = 5000.0;   // throughput window of the stats
// The DAG floor (builder version 2). A frontier cluster group at or below kFloorTris is not
// simplified further; the whole-face tail level aims at kTailTris (a handful of triangles).
constexpr U32 kFloorTris         = 8;
constexpr U32 kTailTris          = 4;

// Frames since `then` (0 when `then` is in the future: frame numbers from different callers).
U32 framesSince(U32 now, U32 then) { return now >= then ? now - then : 0u; }

F64 nowMs()
{
    return std::chrono::duration<F64, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Pseudo-random unit vector per joint index: the skin-weight embedding's basis (the #561
// decimator's, llcleanrigdecimate.cpp). Same influences -> same point; moving weight to
// another joint moves it by ~sqrt(2) x the weight, which the simplifier's attribute metric
// penalizes -- collapses stay inside one skinning region.
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

U64 fnv1a64(const void* data, size_t n, U64 h = 1469598103934665603ull)
{
    const U8* p = (const U8*)data;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

struct Sphere
{
    F32 c[3];
    F32 r;
};

// Sphere enclosing `n` spheres (meshopt_computeSphereBounds, then made exactly conservative
// and inflated by a relative epsilon: nesting (G2) must survive float rounding at a cut).
Sphere mergeSpheres(const Sphere* s, size_t n, F32 abs_eps)
{
    Sphere out{{0.f, 0.f, 0.f}, 0.f};
    if (n == 0) return out;
    const meshopt_Bounds b = meshopt_computeSphereBounds(&s[0].c[0], n, sizeof(Sphere), &s[0].r, sizeof(Sphere));
    out.c[0] = b.center[0]; out.c[1] = b.center[1]; out.c[2] = b.center[2];
    F32 r = b.radius;
    for (size_t i = 0; i < n; ++i)
    {
        const F32 dx = s[i].c[0] - out.c[0], dy = s[i].c[1] - out.c[1], dz = s[i].c[2] - out.c[2];
        r = std::max(r, std::sqrt(dx * dx + dy * dy + dz * dz) + s[i].r);
    }
    out.r = r * (1.f + 1e-5f) + abs_eps;
    return out;
}

bool sphereContains(const F32 outer[4], const F32 inner[4])
{
    const F32 dx = inner[0] - outer[0], dy = inner[1] - outer[1], dz = inner[2] - outer[2];
    const F32 d = std::sqrt(dx * dx + dy * dy + dz * dz) + inner[3];
    return d <= outer[3] * (1.f + 2e-6f) + 1e-12f;
}

// ---------------------------------------------------------------------------------------
// The build (worker)
// ---------------------------------------------------------------------------------------
struct BuildInput
{
    LLClusterKey key;
    S32 lod = -1;
    bool rigged = false;
    bool deep = false;
    U32 seed = 1;
    std::vector<QVertex> packed;   // header-prefixed (kQHeaderVerts pseudo-vertices first)
    std::vector<QWeight> wts;      // rigged only, one per real vertex
    std::vector<U32> idx;
};

struct BuildOutput
{
    std::shared_ptr<LLClusterMeshData> mesh;
    std::string why;
    U32 tris = 0, levels = 0;
    U32 root_tris = 0;             // triangles of the root level (the DAG floor)
    U32 released = 0;              // groups that moved the face's open border (builder v2)
    bool no_converge = false, validate_fail = false, deep_done = false, deep_fail = false;
};

struct BCluster
{
    std::vector<U32> verts;        // global (source) vertex ids, local order
    std::vector<U8>  tri8;         // local indices, 3 per triangle
    Sphere tight{};                // cull bounds
    U32 cone = 0x7F000000u;        // no cone
    Sphere self{};                 // LOD bounds: tight (LOD0) or the generating group's sphere
    F32 self_err = 0.f;
    U32 gen = kNoGroup;            // generating group (kNoGroup = LOD0 / leaf)
    U32 cont = kNoGroup;           // containing group (kNoGroup = root)
    U32 level = 0;
    U32 mask[4] = {0, 0, 0, 0};
    U32 anchor = 0;
    bool has_tangent = false, locked = false;
};

struct BGroup
{
    std::vector<U32> members;
    std::vector<U32> outputs;      // clusters its simplification generated
    Sphere sphere{};
    F32 error = 0.f;
    U32 mask[4] = {0, 0, 0, 0};
    U32 anchor = 0;
    U32 level = 0;                 // max member level
    bool released = false;         // the face's open border was free (error >= the sphere's diameter)
};

struct Builder
{
    const BuildInput& in;
    U32 nv = 0;
    QHeader qh{};
    const QVertex* qv = nullptr;
    std::vector<F32> pos;          // 3 per vertex (decoded lattice positions)
    std::vector<F32> attr;         // kAttr per vertex
    std::vector<U32> remap;        // position remap (canonical vertex per position)
    std::vector<QWeight> wfinal;   // rigged: source weights with the LOD0 weld bit
    std::vector<U8> perm_lock;     // LOD0 open boundary (by position)
    std::vector<U32> vjoint;       // rigged: 4 joints per vertex (decoded)
    std::vector<F32> vweight;      // rigged: 4 weights per vertex
    std::vector<U32> local;        // scratch: global -> local (kNone when unused)
    std::vector<BCluster> cl;
    std::vector<BGroup> gr;
    F32 abs_eps = 0.f;

    explicit Builder(const BuildInput& i) : in(i) {}

    void clusterMask(BCluster& c) const
    {
        c.mask[0] = c.mask[1] = c.mask[2] = c.mask[3] = 0;
        c.anchor = 0;
        c.has_tangent = false;
        c.locked = false;
        F32 sum[128] = {};
        for (U32 v : c.verts)
        {
            if (qv[v].pos[3] & LLCleanQV::kFlagTangent) c.has_tangent = true;
            if (perm_lock[v]) c.locked = true;
            if (!in.rigged) continue;
            for (U32 k = 0; k < 4; ++k)
            {
                const F32 w = vweight[(size_t)v * 4 + k];
                if (!(w > 0.f)) continue;
                const U32 j = vjoint[(size_t)v * 4 + k] & 127u;
                c.mask[j >> 5] |= 1u << (j & 31u);
                sum[j] += w;
            }
        }
        if (in.rigged)
            for (U32 j = 1; j < 128; ++j)
                if (sum[j] > sum[c.anchor]) c.anchor = j;
    }

    // Split a triangle list (global ids) into clusters (<= kMaxClusterVerts / kMaxClusterTris).
    bool clusterize(const std::vector<U32>& tri, U32 gen, const Sphere* self, F32 self_err, U32 level,
                    std::vector<U32>* out_ids)
    {
        if (tri.size() < 3) return true;
        std::vector<U32> lidx(tri.size()), g_of_l;
        std::vector<F32> lpos;
        g_of_l.reserve(tri.size() / 2 + 8);
        lpos.reserve(tri.size() * 2 + 24);
        for (size_t i = 0; i < tri.size(); ++i)
        {
            const U32 v = tri[i];
            if (local[v] == kNone)
            {
                local[v] = (U32)g_of_l.size();
                g_of_l.push_back(v);
                lpos.insert(lpos.end(), &pos[(size_t)v * 3], &pos[(size_t)v * 3] + 3);
            }
            lidx[i] = local[v];
        }
        for (U32 v : g_of_l) local[v] = kNone;
        const size_t nl = g_of_l.size();
        const size_t maxm = meshopt_buildMeshletsBound(lidx.size(), kMaxClusterVerts, kMeshletMinTris);
        std::vector<meshopt_Meshlet> ml(maxm);
        std::vector<U32> mv(maxm * kMaxClusterVerts);
        std::vector<U8> mt(maxm * kMaxClusterTris * 3);
        const size_t n = meshopt_buildMeshletsFlex(ml.data(), mv.data(), mt.data(), lidx.data(), lidx.size(), lpos.data(),
                                                   nl, sizeof(F32) * 3, kMaxClusterVerts, kMeshletMinTris,
                                                   kMaxClusterTris, 0.f, 2.0f);
        for (size_t m = 0; m < n; ++m)
        {
            const meshopt_Meshlet& M = ml[m];
            if (M.triangle_count == 0 || M.vertex_count == 0) continue;
            if (M.vertex_count > kMaxClusterVerts || M.triangle_count > kMaxClusterTris) return false;
            meshopt_optimizeMeshlet(&mv[M.vertex_offset], &mt[M.triangle_offset], M.triangle_count, M.vertex_count);
            const meshopt_Bounds b = meshopt_computeMeshletBounds(&mv[M.vertex_offset], &mt[M.triangle_offset],
                                                                  M.triangle_count, lpos.data(), nl, sizeof(F32) * 3);
            BCluster c;
            c.verts.resize(M.vertex_count);
            for (U32 k = 0; k < M.vertex_count; ++k) c.verts[k] = g_of_l[mv[M.vertex_offset + k]];
            c.tri8.assign(&mt[M.triangle_offset], &mt[M.triangle_offset] + (size_t)M.triangle_count * 3);
            c.tight.c[0] = b.center[0]; c.tight.c[1] = b.center[1]; c.tight.c[2] = b.center[2];
            c.tight.r = b.radius * (1.f + 1e-5f) + abs_eps;
            // Backface cone in mesh space -- not for rigged clusters (the pose moves the
            // triangles away from the rest-pose cone): cutoff 127 = no cone.
            if (!in.rigged && b.cone_cutoff_s8 < 127)
                c.cone = (U32)(U8)b.cone_axis_s8[0] | ((U32)(U8)b.cone_axis_s8[1] << 8)
                       | ((U32)(U8)b.cone_axis_s8[2] << 16) | ((U32)(U8)b.cone_cutoff_s8 << 24);
            c.gen = gen;
            c.level = level;
            if (self) { c.self = *self; c.self_err = self_err; }
            else      { c.self = c.tight; c.self_err = 0.f; }
            clusterMask(c);
            if (out_ids) out_ids->push_back((U32)cl.size());
            cl.push_back(std::move(c));
        }
        return true;
    }

    void appendTris(const BCluster& c, std::vector<U32>& out) const
    {
        for (U8 t : c.tri8) out.push_back(c.verts[t]);
    }

    // PRUNE (released stage, see run()): remove whole connected parts (by position) of `tri`
    // that touch no locked vertex, smallest extent first, until at most `target` indices
    // remain. Parts holding a locked vertex (a group border) are always kept, so no group
    // border moves and no crack opens inside the face. Result in `out`; false when nothing
    // could be removed.
    bool pruneParts(const std::vector<U32>& tri, const std::vector<U8>& lock, size_t target, std::vector<U32>& out) const
    {
        const size_t nt = tri.size() / 3;
        if (nt < 2) return false;
        // union-find over the canonical (position) vertices of the triangles
        std::unordered_map<U32, U32> slot;
        slot.reserve(tri.size());
        std::vector<U32> parent;
        parent.reserve(tri.size());
        auto idOf = [&](U32 v) -> U32
        {
            const U32 c = remap[v];
            auto it = slot.find(c);
            if (it != slot.end()) return it->second;
            const U32 id = (U32)parent.size();
            parent.push_back(id);
            slot.emplace(c, id);
            return id;
        };
        auto root = [&](U32 x) -> U32
        {
            while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
            return x;
        };
        std::vector<U32> tv(tri.size());
        for (size_t i = 0; i < tri.size(); ++i) tv[i] = idOf(tri[i]);
        for (size_t t = 0; t < nt; ++t)
        {
            const U32 a = root(tv[t * 3]), b = root(tv[t * 3 + 1]), c = root(tv[t * 3 + 2]);
            parent[b] = a;
            parent[root(c)] = a;
        }
        struct Part { F32 lo[3], hi[3]; U32 tris = 0; bool locked = false; };
        std::unordered_map<U32, Part> parts;
        for (size_t i = 0; i < tri.size(); ++i)
        {
            const U32 r = root(tv[i]);
            auto ins = parts.emplace(r, Part{});
            Part& P = ins.first->second;
            const F32* p = &pos[(size_t)tri[i] * 3];
            if (ins.second)
                for (int k = 0; k < 3; ++k) { P.lo[k] = p[k]; P.hi[k] = p[k]; }
            for (int k = 0; k < 3; ++k) { P.lo[k] = std::min(P.lo[k], p[k]); P.hi[k] = std::max(P.hi[k], p[k]); }
            if (lock[tri[i]] || lock[remap[tri[i]]]) P.locked = true;
            if (i % 3 == 0) ++P.tris;
        }
        if (parts.size() < 2) return false;
        std::vector<std::pair<F32, U32>> order;   // (extent, part root) of the removable parts
        for (const auto& kv : parts)
        {
            if (kv.second.locked) continue;
            const Part& P = kv.second;
            const F32 dx = P.hi[0] - P.lo[0], dy = P.hi[1] - P.lo[1], dz = P.hi[2] - P.lo[2];
            order.emplace_back(dx * dx + dy * dy + dz * dz, kv.first);
        }
        if (order.empty()) return false;
        std::sort(order.begin(), order.end());
        size_t keep_tris = nt;
        const size_t goal = target / 3;
        std::unordered_set<U32> drop;
        for (const auto& o : order)
        {
            if (keep_tris <= goal) break;
            const U32 pt = parts[o.second].tris;
            if (keep_tris <= pt) break;   // never the last triangles
            keep_tris -= pt;
            drop.insert(o.second);
        }
        if (drop.empty()) return false;
        out.clear();
        out.reserve(keep_tris * 3);
        for (size_t t = 0; t < nt; ++t)
            if (!drop.count(root(tv[t * 3])))
                out.insert(out.end(), tri.begin() + t * 3, tri.begin() + t * 3 + 3);
        return out.size() >= 3;
    }

    bool run(BuildOutput& out);
    bool pack(BuildOutput& out, U32 root_cap);
};

bool Builder::run(BuildOutput& out)
{
    if (in.packed.size() <= kQHeaderVerts) { out.why = "empty source"; return false; }
    nv = (U32)(in.packed.size() - kQHeaderVerts);
    if (nv < 3 || nv > LLCleanQV::kMaxSlotVerts) { out.why = "vertex count out of range"; return false; }
    if (in.rigged && in.wts.size() != nv) { out.why = "weights / vertices mismatch"; return false; }
    LLCleanQV::readHeader(in.packed.data(), qh);
    qv = in.packed.data() + kQHeaderVerts;

    // Decode (positions exactly as the GPU decodes them), attributes.
    pos.resize((size_t)nv * 3);
    attr.assign((size_t)nv * kAttr, 0.f);
    F32 lo[3] = {FLT_MAX, FLT_MAX, FLT_MAX}, hi[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
    if (in.rigged) { vjoint.resize((size_t)nv * 4); vweight.resize((size_t)nv * 4); }
    for (U32 v = 0; v < nv; ++v)
    {
        F32* p = &pos[(size_t)v * 3];
        LLCleanQV::decodePos(qh, qv[v], p);
        for (int k = 0; k < 3; ++k)
        {
            if (!std::isfinite(p[k])) { out.why = "non-finite position"; return false; }
            lo[k] = std::min(lo[k], p[k]);
            hi[k] = std::max(hi[k], p[k]);
        }
        F32* a = &attr[(size_t)v * kAttr];
        LLCleanQV::decodeNormal(qv[v], a);
        LLCleanQV::decodeUV(qh, qv[v], a + 3);
        if (in.rigged)
        {
            U32 jn[4];
            F32 wt[4];
            LLCleanQV::decodeWeight(in.wts[v], jn, wt);
            for (int k = 0; k < 4; ++k)
            {
                vjoint[(size_t)v * 4 + k] = jn[k];
                vweight[(size_t)v * 4 + k] = wt[k];
                if (!(wt[k] > 0.f)) continue;
                F32 b[3];
                jointBasis(jn[k], b);
                a[5] += wt[k] * b[0]; a[6] += wt[k] * b[1]; a[7] += wt[k] * b[2];
            }
        }
    }
    const F32 ext = std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2], 0.f});
    abs_eps = std::max(ext * 1e-6f, 1e-9f);

    // Valid triangles only (in range, not index-degenerate).
    std::vector<U32> tris;
    tris.reserve(in.idx.size());
    for (size_t t = 0; t + 2 < in.idx.size(); t += 3)
    {
        const U32 a = in.idx[t], b = in.idx[t + 1], c = in.idx[t + 2];
        if (a >= nv || b >= nv || c >= nv || a == b || b == c || a == c) continue;
        tris.push_back(a); tris.push_back(b); tris.push_back(c);
    }
    if (tris.size() < 3) { out.why = "no triangles"; return false; }
    out.tris = (U32)(tris.size() / 3);

    // LOD0 open boundary by position = the weld set (#574 / #641) = permanent locks.
    remap.resize(nv);
    meshopt_generatePositionRemap(remap.data(), pos.data(), nv, sizeof(F32) * 3);
    wfinal.resize(nv);
    if (in.rigged) std::memcpy(wfinal.data(), in.wts.data(), (size_t)nv * sizeof(QWeight));
    else std::memset(wfinal.data(), 0, (size_t)nv * sizeof(QWeight));
    LLCleanQV::markWeldBoundary(wfinal.data(), nv, tris.data(), (U32)tris.size(), pos.data(), 3);
    perm_lock.resize(nv);
    for (U32 v = 0; v < nv; ++v) perm_lock[v] = LLCleanQV::isWeldBoundary(wfinal[v]) ? 1 : 0;
    // (positions shared by copies: lock every copy of a locked position)
    for (U32 v = 0; v < nv; ++v) if (perm_lock[v]) perm_lock[remap[v]] = 1;
    for (U32 v = 0; v < nv; ++v) if (perm_lock[remap[v]]) perm_lock[v] = 1;
    if (!in.rigged) wfinal.clear();

    local.assign(nv, kNone);

    // LOD0 clusters.
    std::vector<U32> pending;
    if (!clusterize(tris, kNoGroup, nullptr, 0.f, 0, &pending)) { out.why = "meshlet limits"; return false; }
    if (pending.empty()) { out.why = "clusterize produced nothing"; return false; }

    // THE DAG LOOP. Three stages per face (builder version 2), each only after the previous
    // one stalls (a level that makes almost no progress):
    //   STRICT    group borders + the face's open border locked; meshopt's error (absolute,
    //             attribute-weighted) is the level's deviation. A stuck group retries with
    //             meshopt_SimplifyPermissive (collapses across attribute seams; the attribute
    //             metric still prices them). A lone frontier cluster keeps simplifying too.
    //   RELEASED  group borders locked (no crack INSIDE the face at any cut), the face's open
    //             border free: the seam to the mesh's other faces may move, so the error is
    //             raised to at least the group sphere's DIAMETER -- every output vertex is a
    //             LOD0 vertex inside that sphere and so is all the original geometry below the
    //             group, which bounds the deviation (Hausdorff) and with it any crack to the
    //             neighbouring faces. Such a level is only cut in where the whole patch
    //             projects within the tolerance.
    //   FINAL     the whole remaining frontier (<= kGroupTarget clusters, or a stuck RELEASED
    //             frontier <= kMaxGroupClusters) as ONE group, nothing locked (there are no
    //             group borders), straight to kTailTris (meshopt_SimplifyPrune; the
    //             topology-free meshopt_simplifySloppy when the regular simplifier stops short).
    //             Error >= the face sphere's diameter: the whole face projects within the
    //             tolerance. One group: a cut holding its outputs holds nothing else.
    enum Stage { kStrict, kReleased };
    Stage stage = kStrict;
    bool released_stuck = false;
    // RELEASED + PRUNE: the released stage stalled with more than kMaxGroupClusters clusters
    // (too many for the FINAL group): many small disconnected parts (hair strands, lashes,
    // cards) the topology-preserving simplifier cannot shrink. From then on a stuck released
    // group drops whole unlocked parts (pruneParts: no group border moves). Before builder v2.1
    // this face ended here ("no DAG convergence" past kMaxRootPages, or a root of hundreds of
    // clusters).
    bool prune = false;
    F32 sloppy_scale = -1.f;          // meshopt_simplifyScale of the face (sloppy errors are relative)
    std::vector<U8> lock(nv, 0);
    std::vector<S32> groupmap(nv, -1);
    std::vector<U32> merged, simplified, alt;
    U32 depth = 0;
    while (depth + 1 < kMaxLevels)
    {
        size_t front_tris = 0;
        for (U32 c : pending) front_tris += cl[c].tri8.size() / 3;
        if (front_tris <= kFloorTris) break;             // already a handful of triangles
        const bool final_level = stage == kReleased && (pending.size() <= kGroupTarget || released_stuck);
        std::vector<std::vector<U32>> groups;
        if (final_level)
        {
            if (pending.size() > kMaxGroupClusters) break;
            groups.push_back(pending);
        }
        else
        {
            // 1. partition the frontier into groups of neighbouring clusters
            std::vector<U32> ci, counts(pending.size());
            for (size_t i = 0; i < pending.size(); ++i)
            {
                const BCluster& c = cl[pending[i]];
                counts[i] = (U32)c.tri8.size();
                for (U8 t : c.tri8) ci.push_back(remap[c.verts[t]]);
            }
            std::vector<U32> part(pending.size());
            const size_t np = meshopt_partitionClusters(part.data(), ci.data(), ci.size(), counts.data(), pending.size(),
                                                        pos.data(), nv, sizeof(F32) * 3, kGroupTarget);
            groups.resize(np);
            for (size_t i = 0; i < pending.size(); ++i)
                if (part[i] < np) groups[part[i]].push_back(pending[i]);
            // hard cap (a partition may exceed the target): split, keeping the partitioner's order
            for (size_t g = 0, gn = groups.size(); g < gn; ++g)
                while (groups[g].size() > kMaxGroupClusters)
                {
                    std::vector<U32> tail(groups[g].begin() + kMaxGroupClusters, groups[g].end());
                    groups[g].resize(kMaxGroupClusters);
                    groups.push_back(std::move(tail));
                }

            // 2. locks: group borders (a position used by two groups) + (STRICT) the face's
            //    open border
            std::fill(groupmap.begin(), groupmap.end(), -1);
            for (size_t g = 0; g < groups.size(); ++g)
                for (U32 m : groups[g])
                    for (U32 v : cl[m].verts)
                    {
                        S32& gm = groupmap[remap[v]];
                        gm = (gm == -1 || gm == (S32)g) ? (S32)g : -2;
                    }
            const bool face_locked = stage == kStrict;
            for (U32 v = 0; v < nv; ++v)
                lock[v] = ((face_locked && perm_lock[v]) || groupmap[remap[v]] == -2) ? (U8)meshopt_SimplifyVertex_Lock
                                                                                      : (U8)0;
        }

        // 3. simplify each group (to half; FINAL: to kTailTris), re-split
        std::vector<U32> next, retry;
        size_t made_tris = 0, stuck_tris = 0;
        U32 progressed = 0;
        for (const std::vector<U32>& members : groups)
        {
            if (members.empty()) continue;
            merged.clear();
            for (U32 m : members) appendTris(cl[m], merged);
            const size_t mtris = merged.size() / 3;
            if (!final_level && mtris <= kFloorTris)
            {
                retry.insert(retry.end(), members.begin(), members.end());   // small enough: kept
                continue;
            }
            const size_t target = final_level ? (size_t)kTailTris * 3 : mtris / 2 * 3;
            const size_t stuck_at = (size_t)((F32)merged.size() * kStuckRatio);
            const unsigned base = meshopt_SimplifySparse | meshopt_SimplifyErrorAbsolute;
            auto simplify = [&](std::vector<U32>& dst, unsigned opts, F32& e) -> size_t
            {
                dst.resize(merged.size());
                e = 0.f;
                return meshopt_simplifyWithAttributes(dst.data(), merged.data(), merged.size(), pos.data(), nv,
                                                      sizeof(F32) * 3, attr.data(), sizeof(F32) * kAttr, kAttrW, kAttr,
                                                      final_level ? nullptr : lock.data(), target, FLT_MAX, opts, &e);
            };
            // a retry result replaces the current one when it is valid and smaller
            auto better = [&](size_t n2, F32 e2, size_t n) { return n2 >= 3 && std::isfinite(e2) && n2 < (n < 3 ? merged.size() : n); };
            F32 err = 0.f;
            size_t n = simplify(simplified, stage == kStrict ? base : base | meshopt_SimplifyPermissive, err);
            if (stage == kStrict && (n < 3 || n > stuck_at || !std::isfinite(err)))
            {
                // stuck on attribute seams: collapse across them, still priced by the metric
                F32 e2 = 0.f;
                const size_t n2 = simplify(alt, base | meshopt_SimplifyPermissive, e2);
                if (better(n2, e2, n)) { simplified.swap(alt); n = n2; err = e2; }
            }
            if (prune && !final_level && (n < 3 || n > stuck_at || !std::isfinite(err)) && pruneParts(merged, lock, target, alt))
            {
                // many small disconnected parts: drop the smallest unlocked ones (the regular
                // simplifier then shrinks what is left, still with the group borders locked)
                F32 e2 = 0.f;
                std::vector<U32> rest;
                rest.resize(alt.size());
                const size_t want = std::min(target, alt.size());
                const size_t n2 = meshopt_simplifyWithAttributes(rest.data(), alt.data(), alt.size(), pos.data(), nv,
                                                                 sizeof(F32) * 3, attr.data(), sizeof(F32) * kAttr, kAttrW,
                                                                 kAttr, lock.data(), want, FLT_MAX,
                                                                 base | meshopt_SimplifyPermissive, &e2);
                if (n2 >= 3 && std::isfinite(e2) && n2 < alt.size()) { rest.resize(n2); alt.swap(rest); }
                else e2 = 0.f;
                if (alt.size() >= 3 && (n < 3 || alt.size() < n)) { n = alt.size(); simplified.swap(alt); err = e2; }
            }
            if (final_level && (n < 3 || n > target * 2 || !std::isfinite(err)))
            {
                // the regular simplifier keeps topology (tubes, many small parts): prune small
                // parts, then the topology-free simplifier -- both bounded by the diameter below
                F32 e2 = 0.f;
                const size_t n2 = simplify(alt, base | meshopt_SimplifyPermissive | meshopt_SimplifyPrune, e2);
                if (better(n2, e2, n)) { simplified.swap(alt); n = n2; err = e2; }
                if (n < 3 || n > target * 2)
                {
                    if (sloppy_scale < 0.f) sloppy_scale = meshopt_simplifyScale(pos.data(), nv, sizeof(F32) * 3);
                    alt.resize(merged.size());
                    e2 = 0.f;
                    const size_t n3 = meshopt_simplifySloppy(alt.data(), merged.data(), merged.size(), pos.data(), nv,
                                                             sizeof(F32) * 3, nullptr, target, FLT_MAX, &e2);
                    if (better(n3, e2, n)) { simplified.swap(alt); n = n3; err = e2 * sloppy_scale; }
                }
            }
            if (n < 3 || n > stuck_at || !std::isfinite(err))
            {
                stuck_tris += mtris;
                retry.insert(retry.end(), members.begin(), members.end());
                continue;
            }
            simplified.resize(n);
            const U32 gid = (U32)gr.size();
            gr.emplace_back();
            {
                BGroup& G = gr.back();
                G.members = members;
                std::vector<Sphere> selfs;
                selfs.reserve(members.size());
                F32 emax = 0.f;
                F32 jsum[128] = {};
                for (U32 m : members)
                {
                    const BCluster& c = cl[m];
                    selfs.push_back(c.self);
                    emax = std::max(emax, c.self_err);
                    G.level = std::max(G.level, c.level);
                    for (int k = 0; k < 4; ++k) G.mask[k] |= c.mask[k];
                    if (c.gen != kNoGroup)
                        for (int k = 0; k < 4; ++k) G.mask[k] |= gr[c.gen].mask[k];
                    if (in.rigged)
                        for (U32 v : c.verts)
                            for (U32 k = 0; k < 4; ++k)
                                jsum[vjoint[(size_t)v * 4 + k] & 127u] += vweight[(size_t)v * 4 + k];
                }
                for (U32 j = 1; j < 128; ++j) if (jsum[j] > jsum[G.anchor]) G.anchor = j;
                G.sphere = mergeSpheres(selfs.data(), selfs.size(), abs_eps);
                // G1: >= every generating group's error (demo: max + this step's error)
                G.error = emax + std::max(err, 0.f);
                // RELEASED / FINAL: the face's open border moved -- at least the diameter (the
                // bound of the stage comment above)
                G.released = stage == kReleased;
                if (G.released) G.error = std::max(G.error, 2.f * G.sphere.r);
            }
            for (U32 m : members) cl[m].cont = gid;
            const Sphere gs = gr[gid].sphere;
            const F32 ge = gr[gid].error;
            const U32 glevel = gr[gid].level + 1;
            std::vector<U32> outs;
            if (!clusterize(simplified, gid, &gs, ge, glevel, &outs)) { out.why = "meshlet limits"; return false; }
            gr[gid].outputs = outs;
            if (gr[gid].released) ++out.released;
            next.insert(next.end(), outs.begin(), outs.end());
            made_tris += n / 3;
            ++progressed;
        }
        ++depth;
        pending.swap(next);
        pending.insert(pending.end(), retry.begin(), retry.end());
        if (final_level) break;                          // the tail: nothing coarser
        if (!progressed || made_tris < stuck_tris / 3)   // stalled / mostly stuck (demo)
        {
            if (stage == kStrict) stage = kReleased;     // free the face's border from the next level
            else if (pending.size() <= kMaxGroupClusters) released_stuck = true;   // FINAL next
            else if (!prune) prune = true;               // many small parts: drop whole ones next
            else break;                                  // no convergence (the root page cap decides)
        }
    }
    for (U32 c : pending) out.root_tris += (U32)(cl[c].tri8.size() / 3);
    return true;
}

bool Builder::pack(BuildOutput& out, U32 root_cap)
{
    const bool rigged = in.rigged;
    // ---- pages: roots first, then whole groups, coarse (high member level) -> fine
    struct Page
    {
        std::vector<U32> cls;        // cluster ids in record order
        std::vector<U32> grs;        // internal group ids (contiguous final ids)
        std::vector<U32> vc, tc;     // per cluster, for packPageFits
        U32 level = 0;
    };
    std::vector<Page> pages;
    std::vector<U32> roots;
    for (U32 i = 0; i < (U32)cl.size(); ++i)
        if (cl[i].cont == kNoGroup) roots.push_back(i);
    if (roots.empty()) { out.why = "no root clusters"; return false; }
    auto fits = [&](const Page& p, const std::vector<U32>& add) -> bool
    {
        std::vector<U32> vc = p.vc, tc = p.tc;
        for (U32 c : add) { vc.push_back((U32)cl[c].verts.size()); tc.push_back((U32)(cl[c].tri8.size() / 3)); }
        return packPageFits(vc.data(), tc.data(), (U32)vc.size(), rigged);
    };
    auto addTo = [&](Page& p, U32 c)
    {
        p.cls.push_back(c);
        p.vc.push_back((U32)cl[c].verts.size());
        p.tc.push_back((U32)(cl[c].tri8.size() / 3));
        p.level = std::max(p.level, cl[c].level);
    };
    pages.emplace_back();
    for (U32 c : roots)
    {
        if (!pages.back().cls.empty() && !fits(pages.back(), {c})) pages.emplace_back();
        addTo(pages.back(), c);
    }
    const U32 root_pages = (U32)pages.size();
    if (root_pages > root_cap)
    {
        out.no_converge = true;
        out.why = "no DAG convergence (" + std::to_string(root_pages) + " root pages, " + std::to_string(roots.size())
                + " root clusters)";
        return false;
    }
    std::vector<U32> order(gr.size());
    for (U32 g = 0; g < (U32)gr.size(); ++g) order[g] = g;
    std::stable_sort(order.begin(), order.end(), [&](U32 a, U32 b) { return gr[a].level > gr[b].level; });
    // ROOT-PAGE PACKING (builder v3, 2026-10-14): the coarsest groups go into the LAST root page
    // while they fit, then into the following pages as before (whole groups, coarse -> fine). A
    // root page held only the DAG floor -- a handful of triangles, ~0.3 KB of its 80 KB slot,
    // pinned for the mesh's lifetime: 706 root pages at Cloudsdale were ~55 MB of slots for
    // ~0.2 MB of payload ("roots 0%"), and a small mesh took 2+ slots for a few KB. Now the root
    // page carries the coarse levels too, and a small mesh's whole DAG fits one slot. Groups in
    // the last root page are pinned with it (resident from registration: the cut rule finds them
    // resident; their dependencies are earlier root pages, also pinned). Validator: groups only in
    // the LAST root page, deps of a root page only to root pages.
    for (U32 g : order)
    {
        if (!fits(pages.back(), gr[g].members)) pages.emplace_back();
        if (!fits(pages.back(), gr[g].members)) { out.why = "group does not fit a page"; return false; }
        pages.back().grs.push_back(g);
        for (U32 c : gr[g].members) addTo(pages.back(), c);
    }
    if (pages.size() > LLCleanClusterBuilder::kMaxPages) { out.why = "too many pages"; return false; }

    // ---- final ids
    std::vector<U32> final_group(gr.size(), kNone), group_page(gr.size(), kNone);
    std::vector<U32> cl_page(cl.size(), kNone);
    U32 next_gid = kFirstRealGroup;
    for (U32 p = 0; p < (U32)pages.size(); ++p)
    {
        for (U32 g : pages[p].grs) { final_group[g] = next_gid++; group_page[g] = p; }
        for (U32 c : pages[p].cls) cl_page[c] = p;
    }
    for (U32 c = 0; c < (U32)cl.size(); ++c)
        if (cl_page[c] == kNone) { out.why = "unpacked cluster"; return false; }

    std::shared_ptr<LLClusterMeshData> m = std::make_shared<LLClusterMeshData>();
    m->key = in.key;
    const U32 group_count = next_gid;
    m->groups.resize(group_count);

    // mesh sphere: all LOD0 geometry (every level's vertices are LOD0 vertices)
    std::vector<Sphere> lod0;
    for (const BCluster& c : cl) if (c.gen == kNoGroup) lod0.push_back(c.tight);
    const Sphere ms = mergeSpheres(lod0.data(), lod0.size(), abs_eps);
    U32 all_mask[4] = {0, 0, 0, 0};
    for (const BCluster& c : cl) for (int k = 0; k < 4; ++k) all_mask[k] |= c.mask[k];
    for (U32 s = 0; s < kFirstRealGroup; ++s)
    {
        ClusterGroup& G = m->groups[s];
        std::memcpy(G.sphere, &ms, sizeof(G.sphere));
        G.error = s == kGroupLeaf ? 0.f : kErrorInf;
        G.page = kNone;
        G.counts = 0;
        G.flags = 0;
        for (int k = 0; k < 4; ++k) G.joint_mask[k] = s == kGroupRoot ? all_mask[k] : 0u;
    }
    for (U32 g = 0; g < (U32)gr.size(); ++g)
    {
        const BGroup& B = gr[g];
        ClusterGroup& G = m->groups[final_group[g]];
        std::memcpy(G.sphere, &B.sphere, sizeof(G.sphere));
        for (int k = 0; k < 4; ++k) G.joint_mask[k] = B.mask[k];
        G.error = B.error;
        G.page = group_page[g];
        G.counts = ((U32)B.members.size() & 0xFFu) | ((B.level & 0xFFu) << 8) | ((B.anchor & 0x7Fu) << 16);
        G.flags = B.released ? CLG_FACE_BORDER_FREE : 0u;
    }

    // ---- dependencies (G6): pages of the clusters generated by the groups in each page
    m->pages.resize(pages.size());
    std::vector<U32> dl;
    for (U32 p = 0; p < (U32)pages.size(); ++p)
    {
        dl.clear();
        for (U32 g : pages[p].grs)
            for (U32 o : gr[g].outputs)
                if (cl_page[o] != p) dl.push_back(cl_page[o]);
        std::sort(dl.begin(), dl.end());
        dl.erase(std::unique(dl.begin(), dl.end()), dl.end());
        if (dl.size() > 0xFFFFu) { out.why = "too many page dependencies"; return false; }
        ClusterPageDir& D = m->pages[p];
        D.dep_first = (U32)m->deps.size();
        D.dep_count = (U16)dl.size();
        m->deps.insert(m->deps.end(), dl.begin(), dl.end());
    }

    // ---- page bytes
    const U32 mesh_key = (U32)in.key.hash();
    std::vector<U8> page(kPageBytes);
    U32 total_clusters = 0, max_level = 0;
    for (U32 p = 0; p < (U32)pages.size(); ++p)
    {
        const Page& P = pages[p];
        const U32 n = (U32)P.cls.size();
        std::fill(page.begin(), page.end(), 0);
        std::vector<ClusterRecord> recs(n);
        U32 off = alignUp(kPageHeaderBytes + n * kClusterRecordBytes, kQVertexBytes);
        for (U32 i = 0; i < n; ++i)
        {
            const BCluster& c = cl[P.cls[i]];
            const U32 vc = (U32)c.verts.size();
            LLCleanQV::writeHeader(reinterpret_cast<QVertex*>(&page[off]), qh);
            off += kQHeaderVerts * kQVertexBytes;
            recs[i].vert_off20 = off / kQVertexBytes;
            for (U32 k = 0; k < vc; ++k) std::memcpy(&page[off + k * kQVertexBytes], &qv[c.verts[k]], kQVertexBytes);
            off += vc * kQVertexBytes;
        }
        for (U32 i = 0; i < n; ++i)
        {
            const BCluster& c = cl[P.cls[i]];
            if (!rigged) { recs[i].weight_off10 = kNone; continue; }
            recs[i].weight_off10 = off / kQWeightBytes;
            for (U32 k = 0; k < (U32)c.verts.size(); ++k)
                std::memcpy(&page[off + k * kQWeightBytes], &wfinal[c.verts[k]], kQWeightBytes);
            off += (U32)c.verts.size() * kQWeightBytes;
        }
        off = alignUp(off, 4u);
        for (U32 i = 0; i < n; ++i)
        {
            const BCluster& c = cl[P.cls[i]];
            recs[i].index_off4 = off / 4u;
            std::memcpy(&page[off], c.tri8.data(), c.tri8.size());
            off += clusterIndexBlockBytes((U32)(c.tri8.size() / 3));
        }
        const U32 used = off;
        if (used > kPageBytes || used != pageBytesFor(P.vc.data(), P.tc.data(), n, rigged))
        {
            out.why = "page layout mismatch";
            return false;
        }
        for (U32 i = 0; i < n; ++i)
        {
            const BCluster& c = cl[P.cls[i]];
            ClusterRecord& r = recs[i];
            std::memcpy(r.sphere, &c.tight, sizeof(r.sphere));
            for (int k = 0; k < 4; ++k) r.joint_mask[k] = c.mask[k];
            r.counts = packRecCounts((U32)c.verts.size(), (U32)(c.tri8.size() / 3), c.level, c.anchor);
            r.group = c.cont == kNoGroup ? kGroupRoot : final_group[c.cont];
            r.child_group = c.gen == kNoGroup ? kGroupLeaf : final_group[c.gen];
            r.cone = c.cone;
            r.flags = (rigged ? CLF_RIGGED : 0u) | (c.has_tangent ? CLF_HAS_TANGENT : 0u) | (c.locked ? CLF_BORDER_LOCKED : 0u);
            max_level = std::max(max_level, c.level);
        }
        std::memcpy(&page[kPageHeaderBytes], recs.data(), (size_t)n * kClusterRecordBytes);
        ClusterPageHeader ph{};
        ph.magic = kPageMagic;
        ph.version_count = (n & 0xFFu) | (((U32)P.grs.size() & 0xFFu) << 8) | (kFormatVersion << 16);
        ph.mesh_key = mesh_key;
        ph.page_index = p;
        ph.used_bytes = used;
        ph.group_first = P.grs.empty() ? kGroupRoot : final_group[P.grs.front()];
        ph.flags = 0;
        ph.hash = fnv1a32(&page[kPageHeaderBytes], used - kPageHeaderBytes);
        std::memcpy(&page[0], &ph, sizeof(ph));

        ClusterPageDir& D = m->pages[p];
        D.offset = (U32)m->payload.size();
        D.stored_bytes = used;
        D.used_bytes = used;
        D.hash = fnv1a32(page.data(), used);
        D.group_count = (U16)P.grs.size();
        D.group_first = ph.group_first;
        D.level = P.level;
        if ((U64)m->payload.size() + used > 0xFFFFFFFFull) { out.why = "payload too large"; return false; }
        m->payload.insert(m->payload.end(), page.begin(), page.begin() + used);
        total_clusters += n;
    }

    // ---- header
    ClusterFileHeader& H = m->header;
    H = ClusterFileHeader{};
    H.source_lod = in.lod;
    H.flags = rigged ? CLM_RIGGED : 0u;
    {
        U64 h = fnv1a64(in.packed.data(), in.packed.size() * sizeof(QVertex));
        h = fnv1a64(in.idx.data(), in.idx.size() * sizeof(U32), h);
        if (rigged) h = fnv1a64(in.wts.data(), in.wts.size() * sizeof(QWeight), h);
        H.source_hash = h;
    }
    H.root_pages = root_pages;
    H.cluster_count = total_clusters;
    H.level_count = max_level + 1;
    H.tri_count_lod0 = out.tris;
    H.compression = kCompNone;
    ClusterMesh& M = H.mesh;
    std::memset(&M, 0, sizeof(M));
    std::memcpy(M.qheader, &qh, sizeof(QHeader));
    setMeshLod0Tris(M, out.tris);   // the full-detail baseline of the stats (kFormatVersion 3)
    std::memcpy(M.sphere, &ms, sizeof(M.sphere));
    M.page_count = (U32)pages.size();
    M.root_pages = root_pages;
    M.group_count = group_count;
    M.cluster_count = total_clusters;
    M.flags = H.flags;
    m->finalizeLayout();   // page_count, group_count, offsets, header hash
    out.levels = H.level_count;
    out.mesh = m;
    return true;
}

void runBuild(const BuildInput& in, BuildOutput& out)
{
    Builder b(in);
    if (!b.run(out) || !b.pack(out, LLCleanClusterBuilder::kMaxRootPages))
    {
        out.mesh.reset();
        if (out.why.empty()) out.why = "build failed";
        return;
    }
    std::string why;
    if (!LLCleanClusterBuilder::validate(*out.mesh, &why))
    {
        out.validate_fail = true;
        out.why = "validation: " + why;
        out.mesh.reset();
        return;
    }
    if (in.deep && out.tris <= kDeepMaxTris)
    {
        out.deep_done = true;
        std::string dwhy;
        if (!LLCleanClusterBuilder::validateCuts(*out.mesh, in.seed, &dwhy))
        {
            out.deep_fail = true;
            // Logged on the worker (debug only); the mesh is kept: a cut-check failure is a
            // builder bug to fix, the structural checks above already passed.
            LL_WARNS("VKClusterBuild") << "cut check FAILED " << in.key.fileName() << ": " << dwhy << LL_ENDL;
        }
    }
}
} // namespace

// ============================================================================================
// LLCleanClusterBuilder
// ============================================================================================
struct LLCleanClusterBuilder::Impl
{
    enum KState : U8 { K_QUEUED = 0, K_RUNNING, K_OK, K_FAILED };
    // Queue bands (llcleanclusterbuild.h PRIORITY): what blocks an avatar's switch first.
    enum Band : U8 { B_RIGGED = 0, B_STATIC = 1, B_REBUILD = 2 };
    struct Known
    {
        KState state = K_QUEUED;
        U32 frame = 0;
        S32 lod = -1;
        U32 slot = kNone;
        U32 seq = 0;
        bool rigged = false;
        U8 band = B_STATIC;
        F32 importance = 0.f;
        U32 group = kNoGroup;
    };
    // A queued job; the sort fields are refreshed from `known` when a priority changed.
    struct QItem
    {
        LLClusterKey key;
        U32 seq = 0;
        U32 frame = 0;
        U32 group = kNoGroup;
        F32 importance = 0.f;
        U8 band = B_STATIC;
    };
    // true = a has LOWER priority than b (sorted ascending, the best job is last: pop_back).
    // Band, then importance (higher first), then the avatar (its faces together), then arrival.
    static bool lowerPrio(const QItem& a, const QItem& b)
    {
        if (a.band != b.band) return a.band > b.band;
        if (a.importance != b.importance) return a.importance < b.importance;
        if (a.group != b.group) return a.group > b.group;
        if (a.frame != b.frame) return a.frame > b.frame;
        return a.seq > b.seq;
    }
    struct Job
    {
        U32 id = 0;
        U32 seq = 0;
        U64 bytes = 0;               // source copy (the in-flight byte bound)
        BuildInput in;
        BuildOutput out;
        F32 ms = 0.f;
    };

    std::vector<QItem> queue;        // ascending priority (best last) unless `unsorted`
    bool unsorted = false;           // pushed since the last sort
    bool refresh = false;            // a queued job's priority changed: re-read from `known`
    std::unordered_map<LLClusterKey, Known, LLClusterKeyHash> known;
    std::unordered_map<U32, std::unique_ptr<Job>> jobs;
    std::mutex done_mutex;
    std::vector<Job*> done;
    std::unique_ptr<LL::ThreadPool> pool;
    std::vector<Result> results;
    U32 threads = 0, inflight = 0, queued_live = 0, next_job = 1, seq = 0, frame = 0, pumps = 0;
    U64 inflight_bytes = 0;
    F32 ms_per_tri = kInitMsPerTri;
    bool stopped = false;
    F32 hist[kMsHistory] = {};
    U32 hist_n = 0, hist_pos = 0;
    U32 ok_logged = 0, fail_logged = 0;
    // throughput window (stats)
    F64 win_t0 = 0.0, win_ms = 0.0;
    U64 win_n = 0;
    F32 win_pump_max = 0.f;
    Stats st;

    void sortQueue()
    {
        if (refresh)
        {
            // Re-read every queued job's sort fields; drop entries whose job is gone / re-queued.
            size_t w = 0;
            for (size_t i = 0; i < queue.size(); ++i)
            {
                auto kt = known.find(queue[i].key);
                if (kt == known.end() || kt->second.state != K_QUEUED || kt->second.seq != queue[i].seq) continue;
                QItem q = queue[i];
                q.band = kt->second.band;
                q.importance = kt->second.importance;
                q.group = kt->second.group;
                queue[w++] = q;
            }
            queue.resize(w);
            refresh = false;
            unsorted = true;
        }
        if (unsorted)
        {
            std::sort(queue.begin(), queue.end(), lowerPrio);
            unsorted = false;
        }
        if (queue.capacity() > 2 * queue.size() + 1024) queue.shrink_to_fit();
    }
};

LLCleanClusterBuilder::LLCleanClusterBuilder() : mImpl(new Impl) {}
LLCleanClusterBuilder::~LLCleanClusterBuilder() = default;

LLCleanClusterBuilder& LLCleanClusterBuilder::instance()
{
    static LLCleanClusterBuilder s;
    return s;
}

bool LLCleanClusterBuilder::request(const LLClusterKey& key, U32 src_slot, U32 frame, const Prio& prio, bool* no_source)
{
    Impl& d = *mImpl;
    if (no_source) *no_source = false;
    static LLCachedControl<bool> s_on(gSavedSettings, "RenderVKClusterLOD", false);
    if (!s_on || d.stopped || src_slot == kNone) return false;
    d.frame = std::max(d.frame, frame);
    const F32 imp = std::isfinite(prio.importance) ? std::max(prio.importance, 0.f) : 0.f;
    LLCleanMeshAsset& store = LLCleanMeshAsset::instance();
    auto lodOf = [&](U32 slot) -> S32
    {
        for (S32 L = LLCleanMeshAsset::NUM_LOD - 1; L >= 0; --L)
            if (store.groupSlotAt(key.mesh_id, key.face, L) == slot) return L;
        return -1;
    };
    auto it = d.known.find(key);
    if (it != d.known.end())
    {
        Impl::Known& k = it->second;
        if (k.state == Impl::K_QUEUED)
        {
            // Still queued: a higher LOD the face publishes now replaces the source (pump reads
            // k.slot / k.lod at admission) -- one build, not a build + an upgrade rebuild.
            if (src_slot != k.slot)
            {
                const S32 L = lodOf(src_slot);
                if (L > k.lod) { k.slot = src_slot; k.lod = L; ++d.st.coalesced; }
            }
            // The priority: the queue is re-sorted only past kPrioStep (bounded churn while
            // avatars move), a better band or another avatar.
            const U8 band = prio.rebuild ? (U8)Impl::B_REBUILD : (k.rigged ? (U8)Impl::B_RIGGED : (U8)Impl::B_STATIC);
            const bool moved = imp > k.importance * kPrioStep || imp * kPrioStep < k.importance;
            // (a group change alone -- one mesh worn by two avatars at a similar distance --
            // does not re-sort: it would flip every frame)
            if (band < k.band || moved)
            {
                k.band = std::min(k.band, band);
                k.importance = imp;
                k.group = prio.group;
                d.refresh = true;
            }
        }
        // Queued / running / built but not yet taken (takeFinished forgets a delivered
        // success, so a request after delivery is a new need and rebuilds): cheap repeat.
        if (k.state != Impl::K_FAILED) return true;
        if (src_slot == k.slot && framesSince(frame, k.frame) < kFailRetryFrames) return false;
    }
    const S32 lod = lodOf(src_slot);
    if (lod < 0)   // not a published LOD of this face
    {
        ++d.st.refused;
        if (no_source) *no_source = true;
        return false;
    }
    if (it != d.known.end())
    {
        // A failed face, another slot: retry only for a HIGHER LOD than the one that failed (a
        // lower or equal one is the same content or worse) until the entry ages out.
        const Impl::Known& k = it->second;
        if (lod <= k.lod && framesSince(frame, k.frame) < kFailRetryFrames) return false;
    }
    U32 vc = 0, ic = 0, wc = 0;
    const QVertex* mv = store.cpuSlotPacked(src_slot, vc);
    const U16* mi = store.cpuSlotIndices(src_slot, ic);
    const QWeight* mw = store.cpuSlotWeights(src_slot, wc);
    if (!mv || !mi || vc < 3 || ic < 3)
    {
        // A cluster-owned mirror freed once its DAG was on disk (LLCleanMeshAsset::
        // queueClusterMirrorRelease): a build needs it again (cache miss / corrupt or evicted
        // .clod / builder version / rebuild) -- re-materialize it; the caller asks again.
        if (store.mirrorReleased(src_slot)) store.requestMirror(src_slot, "cluster build source");
        ++d.st.refused;
        if (no_source) *no_source = true;
        return false;
    }
    if (d.queued_live >= kQueueMax) { ++d.st.refused; return false; }
    Impl::QItem q;
    q.key = key;
    q.frame = frame;
    q.seq = d.seq + 1;
    q.group = prio.group;
    q.importance = imp;
    q.band = prio.rebuild ? (U8)Impl::B_REBUILD : ((mw != nullptr && wc == vc) ? (U8)Impl::B_RIGGED : (U8)Impl::B_STATIC);
    try { d.queue.push_back(q); }
    catch (const std::bad_alloc&) { ++d.st.refused; return false; }
    Impl::Known& k = d.known[key];
    k.state = Impl::K_QUEUED;
    k.frame = frame;
    k.lod = lod;
    k.slot = src_slot;
    k.seq = ++d.seq;
    k.rigged = mw != nullptr && wc == vc;
    k.band = q.band;
    k.importance = imp;
    k.group = prio.group;
    d.unsorted = true;
    ++d.queued_live;
    ++d.st.requested;
    return true;
}

void LLCleanClusterBuilder::pump(F32 budget_ms)
{
    Impl& d = *mImpl;
    if (d.stopped) return;
    const F64 t0 = nowMs();
    ++d.pumps;
    // The RENDER thread's budget (llcleanclusterbuild.h BUDGET); the workers are not metered.
    const F64 main_cap = std::clamp((F64)budget_ms, 0.25, 8.0);

    // 1. finished jobs -> results
    {
        std::vector<Impl::Job*> fin;
        {
            std::lock_guard<std::mutex> lk(d.done_mutex);
            fin.swap(d.done);
        }
        for (Impl::Job* j : fin)
        {
            auto jt = d.jobs.find(j->id);
            if (jt == d.jobs.end()) continue;
            if (d.inflight) --d.inflight;
            Impl::Job& job = *jt->second;
            d.inflight_bytes -= std::min(d.inflight_bytes, job.bytes);
            BuildOutput& o = job.out;
            auto kt = d.known.find(job.in.key);
            const bool current = kt != d.known.end() && kt->second.seq == job.seq;
            if (current)
            {
                kt->second.state = o.mesh ? Impl::K_OK : Impl::K_FAILED;
                kt->second.frame = d.frame;
            }
            // stats
            d.hist[d.hist_pos] = job.ms;
            d.hist_pos = (d.hist_pos + 1) % kMsHistory;
            d.hist_n = std::min(d.hist_n + 1, kMsHistory);
            d.st.ms_last = job.ms;
            d.st.ms_max = std::max(d.st.ms_max, job.ms);
            d.st.ms_total += job.ms;
            ++d.win_n;
            d.win_ms += job.ms;
            if (o.tris > 0 && job.ms > 0.f)
                d.ms_per_tri = std::clamp(0.8f * d.ms_per_tri + 0.2f * (job.ms / (F32)o.tris), 1e-5f, 0.1f);
            if (o.no_converge) ++d.st.no_converge;
            if (o.validate_fail) ++d.st.validate_fail;
            if (o.deep_done) ++d.st.deep_checked;
            if (o.deep_fail) ++d.st.deep_failed;
            d.st.tris_in += o.tris;
            if (o.mesh)
            {
                ++d.st.built;
                d.st.clusters_out += o.mesh->header.cluster_count;
                d.st.pages_out += o.mesh->header.page_count;
                d.st.root_pages_out += o.mesh->header.root_pages;
                d.st.bytes_out += o.mesh->payload.size();
                d.st.levels_out += o.levels;
                d.st.root_tris_out += o.root_tris;
                if (o.released) ++d.st.released_meshes;
                if (d.ok_logged < 16)
                {
                    ++d.ok_logged;
                    LL_INFOS("VKClusterBuild") << "built " << job.in.key.fileName() << " (LOD " << job.in.lod
                        << (job.in.rigged ? ", rigged" : "") << "): " << o.tris << " tris -> "
                        << o.mesh->header.cluster_count << " clusters, " << (o.mesh->header.group_count - kFirstRealGroup)
                        << " groups, " << o.levels << " levels (" << o.released << " border-free groups), root "
                        << o.root_tris << " tris, " << o.mesh->header.page_count << " pages ("
                        << o.mesh->header.root_pages << " root), " << (o.mesh->payload.size() >> 10) << " KB in "
                        << job.ms << " ms" << (o.deep_done ? (o.deep_fail ? ", cut check FAILED" : ", cut check ok") : "")
                        << LL_ENDL;
                }
            }
            else
            {
                ++d.st.failed;
                if (d.fail_logged < 32)
                {
                    ++d.fail_logged;
                    LL_INFOS("VKClusterBuild") << "no clusters for " << job.in.key.fileName() << " (LOD " << job.in.lod
                                               << ", " << o.tris << " tris): " << o.why
                                               << " -- stays on the legacy path" << LL_ENDL;
                }
            }
            if (current)
            {
                Result r;
                r.key = job.in.key;
                r.mesh = o.mesh;
                r.why = o.why;
                r.build_ms = job.ms;
                if (d.results.size() >= kMaxResults)
                {
                    // (bounded; ~never) the dropped key is forgotten, so it is asked again
                    auto ot = d.known.find(d.results.front().key);
                    if (ot != d.known.end() && ot->second.state == Impl::K_OK) d.known.erase(ot);
                    d.results.erase(d.results.begin());
                }
                d.results.push_back(std::move(r));
            }
            d.jobs.erase(jt);
        }
    }

    // 2. age the known table (bounded: failures go after their hold time; successes leave at
    //    takeFinished)
    if ((d.pumps & 63u) == 0u && !d.known.empty())
    {
        for (auto it = d.known.begin(); it != d.known.end();)
        {
            const Impl::Known& k = it->second;
            const bool old = k.state == Impl::K_FAILED && framesSince(d.frame, k.frame) > kFailRetryFrames;
            if (old) it = d.known.erase(it);
            else ++it;
        }
    }

    // 3. admit queued jobs, best first, while a worker is free (+ a backlog of two jobs per
    //    worker: short builds finish many times per frame, and the pool would idle between two
    //    pumps), within the render thread's budget,
    //    the per-frame copy and the in-flight source bytes.
    auto endPump = [&]()
    {
        const F32 ms = (F32)(nowMs() - t0);
        d.win_pump_max = std::max(d.win_pump_max, ms);
        if (d.win_t0 <= 0.0) d.win_t0 = t0;
        const F64 dt = t0 - d.win_t0;
        if (dt >= kRateWindowMs)
        {
            d.st.per_sec = (F32)((F64)d.win_n * 1000.0 / dt);
            d.st.avg_ms = d.win_n ? (F32)(d.win_ms / (F64)d.win_n) : 0.f;
            d.st.pump_ms_max = d.win_pump_max;
            d.win_t0 = t0;
            d.win_n = 0;
            d.win_ms = 0.0;
            d.win_pump_max = 0.f;
        }
    };
    if (d.queue.empty()) { endPump(); return; }
    if (!d.pool)
    {
        static LLCachedControl<U32> s_threads(gSavedSettings, "RenderVKClusterBuildThreads", 0);
        U32 n = s_threads;
        if (n == 0) n = std::max(1u, std::thread::hardware_concurrency() / 4u);
        d.threads = std::min(n, 16u);
        try
        {
            d.pool.reset(new LL::ThreadPool("CleanClusterBuild", d.threads));
            d.pool->start();
        }
        catch (...)
        {
            // never a crash: no workers this frame, retried on the next pump
            d.pool.reset();
            endPump();
            return;
        }
        LL_INFOS("VKClusterBuild") << "Cluster LOD builder: " << d.threads << " worker threads, render-thread budget "
                                   << main_cap << " ms/frame, jobs in flight <= "
                                   << (std::max(4u, d.threads * 3u)) << LL_ENDL;
    }
    const U32 max_inflight = std::max(4u, d.threads * 3u);
    if (d.inflight >= max_inflight) { endPump(); return; }
    d.sortQueue();
    static LLCachedControl<U32> s_debug(gSavedSettings, "RenderVKClusterDebug", 0);
#ifndef NDEBUG
    const bool deep = true;
#else
    const bool deep = s_debug != 0;
#endif
    LLCleanMeshAsset& store = LLCleanMeshAsset::instance();
    U32 copied = 0;
    while (!d.queue.empty() && d.inflight < max_inflight && copied < kCopyVertsPerFrame
           && d.inflight_bytes < kInflightBytes && nowMs() - t0 < main_cap)
    {
        const Impl::QItem q = d.queue.back();
        d.queue.pop_back();
        auto kt = d.known.find(q.key);
        if (kt == d.known.end() || kt->second.state != Impl::K_QUEUED || kt->second.seq != q.seq) continue;
        if (d.queued_live) --d.queued_live;
        Impl::Known& k = kt->second;
        // The slot must still be this face's LOD (a freed / recycled slot is never built
        // under this key); the mirror must still be there.
        U32 vc = 0, ic = 0, wc = 0;
        const QVertex* mv = nullptr;
        const U16* mi = nullptr;
        const QWeight* mw = nullptr;
        if (store.groupSlotAt(q.key.mesh_id, q.key.face, k.lod) == k.slot)
        {
            mv = store.cpuSlotPacked(k.slot, vc);
            mi = store.cpuSlotIndices(k.slot, ic);
            mw = store.cpuSlotWeights(k.slot, wc);
        }
        if (!mv || !mi || vc < 3 || ic < 3)
        {
            // (a freed cluster-owned mirror: brought back for the build that asks again)
            if (store.groupSlotAt(q.key.mesh_id, q.key.face, k.lod) == k.slot && store.mirrorReleased(k.slot))
                store.requestMirror(k.slot, "cluster build source");
            d.known.erase(kt);   // asked again once the face is back
            ++d.st.stale;
            continue;
        }
        std::unique_ptr<Impl::Job> job(new Impl::Job());
        job->id = d.next_job++;
        if (d.next_job == 0) d.next_job = 1;
        job->seq = k.seq;
        BuildInput& in = job->in;
        in.key = q.key;
        in.lod = k.lod;
        in.rigged = mw != nullptr && wc == vc;
        in.deep = deep;
        in.seed = (U32)q.key.hash() ^ k.seq;
        try
        {
            in.packed.assign(mv, mv + vc + kQHeaderVerts);
            in.idx.assign(mi, mi + ic);
            if (in.rigged) in.wts.assign(mw, mw + vc);
        }
        catch (const std::bad_alloc&)
        {
            d.known.erase(kt);   // allocation failure: retried on a later request
            ++d.st.stale;
            break;
        }
        job->bytes = (U64)in.packed.size() * sizeof(QVertex) + (U64)in.idx.size() * sizeof(U32)
                   + (U64)in.wts.size() * sizeof(QWeight);
        Impl::Job* raw = job.get();
        d.jobs.emplace(raw->id, std::move(job));
        Impl* self = &d;
        bool posted = false;
        try
        {
            posted = d.pool->getQueue().post([self, raw]()
            {
                const F64 s = nowMs();
                try { runBuild(raw->in, raw->out); }
                catch (const std::bad_alloc&) { raw->out.mesh.reset(); raw->out.why = "out of memory"; }
                catch (...) { raw->out.mesh.reset(); raw->out.why = "exception"; }
                raw->ms = (F32)(nowMs() - s);
                // the source copy is not needed any more (bounded RAM while results wait)
                std::vector<QVertex>().swap(raw->in.packed);
                std::vector<QWeight>().swap(raw->in.wts);
                std::vector<U32>().swap(raw->in.idx);
                std::lock_guard<std::mutex> lk(self->done_mutex);
                self->done.push_back(raw);
            });
        }
        catch (...) { posted = false; }
        if (!posted)
        {
            d.jobs.erase(raw->id);
            d.known.erase(kt);
            break;
        }
        k.state = Impl::K_RUNNING;
        ++d.inflight;
        d.inflight_bytes += raw->bytes;
        copied += vc;
    }
    endPump();
}

void LLCleanClusterBuilder::takeFinished(std::vector<Result>& out)
{
    Impl& d = *mImpl;
    out.clear();
    out.swap(d.results);
    // Delivered successes are forgotten: the caller owns the mesh now, and a later request
    // for the key (its mesh was freed and the cache had no file) builds again at once.
    for (const Result& r : out)
        if (r.mesh)
        {
            auto it = d.known.find(r.key);
            if (it != d.known.end() && it->second.state == Impl::K_OK) d.known.erase(it);
        }
}

void LLCleanClusterBuilder::stop()
{
    Impl& d = *mImpl;
    d.stopped = true;
    if (d.pool)
    {
        d.pool->close();   // joins the workers (a running build finishes first)
        d.pool.reset();
    }
    {
        std::lock_guard<std::mutex> lk(d.done_mutex);
        d.done.clear();
    }
    d.jobs.clear();
    d.known.clear();
    d.results.clear();
    d.queue.clear();
    d.unsorted = d.refresh = false;
    d.inflight = d.queued_live = 0;
    d.inflight_bytes = 0;
}

LLCleanClusterBuilder::Stats LLCleanClusterBuilder::stats() const
{
    const Impl& d = *mImpl;
    Stats s = d.st;
    s.queued = d.queued_live;
    s.building = d.inflight;
    s.threads = d.threads;
    s.ms_per_ktri = d.ms_per_tri * 1000.f;
    s.inflight_bytes = d.inflight_bytes;
    if (d.hist_n)
    {
        std::vector<F32> h(d.hist, d.hist + d.hist_n);
        std::sort(h.begin(), h.end());
        s.ms_p50 = h[(h.size() - 1) / 2];
        s.ms_p95 = h[std::min(h.size() - 1, (size_t)((F32)(h.size() - 1) * 0.95f + 0.5f))];
    }
    return s;
}

std::string LLCleanClusterBuilder::statsLine() const
{
    const Stats s = stats();
    std::ostringstream o;
    o.setf(std::ios::fixed);
    o.precision(1);
    o << "build q " << s.queued << " run " << s.building << "/" << s.threads << "t (" << (s.inflight_bytes >> 20)
      << " MB src) " << s.per_sec << "/s avg " << s.avg_ms << " ms pump max " << s.pump_ms_max << " ms, built "
      << s.built << " failed " << s.failed
      << " (no-converge " << s.no_converge << ", invalid " << s.validate_fail << "), refused " << s.refused
      << " stale " << s.stale << " coalesced " << s.coalesced << ", ms p50 " << s.ms_p50 << " p95 " << s.ms_p95 << " max " << s.ms_max;
    if (s.built)
        o << ", per mesh: pages " << (F64)s.pages_out / (F64)s.built << " (root " << (F64)s.root_pages_out / (F64)s.built
          << ") levels " << (F64)s.levels_out / (F64)s.built << " clusters " << (F64)s.clusters_out / (F64)s.built
          << " root tris " << (F64)s.root_tris_out / (F64)s.built << " (border-free " << s.released_meshes << ")"
          << ", " << (s.bytes_out >> 20) << " MB built";
    if (s.deep_checked) o << ", cut checks " << s.deep_checked << " failed " << s.deep_failed;
    return o.str();
}

// ============================================================================================
// Validation
// ============================================================================================
bool LLCleanClusterBuilder::validate(const LLClusterMeshData& mesh, std::string* why)
{
    std::string w;
    auto fail = [&](const std::string& s) { if (why) *why = s; return false; };
    if (!mesh.checkHeader(&w)) return fail("header: " + w);
    const ClusterFileHeader& H = mesh.header;
    if (mesh.groups.size() != H.group_count || mesh.pages.size() != H.page_count || mesh.deps.size() != H.deps_count)
        return fail("metadata vectors");
    if (H.mesh.page_count != H.page_count || H.mesh.root_pages != H.root_pages || H.mesh.group_count != H.group_count
        || H.mesh.cluster_count != H.cluster_count)
        return fail("mesh record counts");
    if (H.level_count == 0 || H.level_count > kMaxLevels) return fail("level count");
    // sentinels
    const ClusterGroup& leaf = mesh.groups[kGroupLeaf];
    const ClusterGroup& root = mesh.groups[kGroupRoot];
    if (leaf.error != 0.f || root.error < kErrorInf || leaf.page != kNone || root.page != kNone)
        return fail("sentinel groups");
    // real groups
    for (U32 g = kFirstRealGroup; g < H.group_count; ++g)
    {
        const ClusterGroup& G = mesh.groups[g];
        const U32 members = G.counts & 0xFFu;
        if (!std::isfinite(G.error) || G.error < 0.f || G.error >= kErrorInf) return fail("group error " + std::to_string(g));
        if (!(G.sphere[3] >= 0.f) || !std::isfinite(G.sphere[0]) || !std::isfinite(G.sphere[1])
            || !std::isfinite(G.sphere[2]) || !std::isfinite(G.sphere[3]))
            return fail("group sphere " + std::to_string(g));
        // (v3 root-page packing: a group may live in the LAST root page, pinned with it)
        if (G.page >= H.page_count || G.page + 1 < H.root_pages) return fail("group page " + std::to_string(g));
        if (members == 0 || members > kMaxGroupClusters) return fail("group members " + std::to_string(g));
        // G7: a group that moved the face's open border is never cut in before the whole
        // group sphere projects within the tolerance
        if ((G.flags & CLG_FACE_BORDER_FREE) && G.error < 2.f * G.sphere[3])
            return fail("G7: border-free group error below its diameter " + std::to_string(g));
    }
    // pages: G4 (group ranges), G5 (roots first), G6 (deps to lower-index pages)
    U32 covered = 0, next_first = kFirstRealGroup;
    for (U32 p = 0; p < H.page_count; ++p)
    {
        const ClusterPageDir& D = mesh.pages[p];
        if ((U64)D.dep_first + D.dep_count > mesh.deps.size()) return fail("deps range, page " + std::to_string(p));
        for (U32 i = 0; i < D.dep_count; ++i)
        {
            const U32 q = mesh.deps[D.dep_first + i];
            if (q >= p) return fail("dependency not coarser, page " + std::to_string(p));
        }
        if (p < H.root_pages)
        {
            // (v3: only the LAST root page may hold groups; its dependencies are root pages)
            const bool last_root = p + 1 == H.root_pages;
            if (D.group_count != 0 && !last_root) return fail("root page " + std::to_string(p));
            for (U32 i = 0; i < D.dep_count; ++i)
                if (mesh.deps[D.dep_first + i] >= H.root_pages) return fail("root page dependency " + std::to_string(p));
            if (D.group_count == 0)
            {
                if (D.dep_count != 0) return fail("root page " + std::to_string(p));
                continue;
            }
        }
        else if (D.group_count == 0) return fail("page without groups " + std::to_string(p));
        if (D.group_first != next_first || (U64)D.group_first + D.group_count > H.group_count)
            return fail("group range, page " + std::to_string(p));
        for (U32 g = D.group_first; g < D.group_first + D.group_count; ++g)
            if (mesh.groups[g].page != p) return fail("group / page mismatch " + std::to_string(g));
        next_first += D.group_count;
        covered += D.group_count;
    }
    if (covered + kFirstRealGroup != H.group_count) return fail("groups not covered by pages");
    if (!mesh.hasPayload()) return true;

    // ---- payload: every page and cluster
    if (mesh.payload.size() != H.payload_bytes) return fail("payload size");
    std::vector<U8> buf(kPageBytes);
    std::vector<U32> members_seen(H.group_count, 0);
    U32 clusters = 0;
    for (U32 p = 0; p < H.page_count; ++p)
    {
        const ClusterPageDir& D = mesh.pages[p];
        const std::string ps = " (page " + std::to_string(p) + ")";
        if (!mesh.copyPage(p, buf.data())) return fail("page bytes / hash" + ps);
        ClusterPageHeader ph;
        std::memcpy(&ph, buf.data(), sizeof(ph));
        const U32 n = ph.version_count & 0xFFu;
        if (ph.magic != kPageMagic || (ph.version_count >> 16) != kFormatVersion || ph.page_index != p
            || ph.used_bytes != D.used_bytes || ph.mesh_key != (U32)mesh.key.hash()
            || ((ph.version_count >> 8) & 0xFFu) != D.group_count || ph.group_first != D.group_first)
            return fail("page header" + ps);
        if (fnv1a32(buf.data() + kPageHeaderBytes, ph.used_bytes - kPageHeaderBytes) != ph.hash) return fail("page hash" + ps);
        if (n == 0 || n > kMaxClustersPerPage) return fail("cluster count" + ps);
        const U32 vstart = alignUp(kPageHeaderBytes + n * kClusterRecordBytes, kQVertexBytes);
        if (vstart > ph.used_bytes) return fail("records past used bytes" + ps);
        U32 lvl = 0;
        for (U32 i = 0; i < n; ++i)
        {
            ClusterRecord r;
            std::memcpy(&r, buf.data() + kPageHeaderBytes + (size_t)i * kClusterRecordBytes, sizeof(r));
            const std::string cs = " (page " + std::to_string(p) + " cluster " + std::to_string(i) + ")";
            const U32 vc = recVertCount(r), tc = recTriCount(r);
            if (vc < 3 || vc > kMaxClusterVerts || tc == 0 || tc > kMaxClusterTris) return fail("cluster counts" + cs);
            if (recLevel(r) >= H.level_count) return fail("cluster level" + cs);
            lvl = std::max(lvl, recLevel(r));
            // vertex block: QHeader at vert_off20 - 2, = the mesh lattice
            if (r.vert_off20 < kQHeaderVerts || (r.vert_off20 - kQHeaderVerts) * kQVertexBytes < vstart
                || (U64)(r.vert_off20 + vc) * kQVertexBytes > ph.used_bytes)
                return fail("vertex block" + cs);
            if (std::memcmp(buf.data() + (size_t)(r.vert_off20 - kQHeaderVerts) * kQVertexBytes, H.mesh.qheader,
                            sizeof(QHeader)) != 0)
                return fail("cluster QHeader != mesh lattice" + cs);
            const bool rig = (H.flags & CLM_RIGGED) != 0;
            if (rig != ((r.flags & CLF_RIGGED) != 0)) return fail("rigged flag" + cs);
            if (rig)
            {
                if (r.weight_off10 == kNone || (U64)(r.weight_off10 + vc) * kQWeightBytes > ph.used_bytes
                    || (U64)r.weight_off10 * kQWeightBytes < vstart)
                    return fail("weight block" + cs);
            }
            else if (r.weight_off10 != kNone) return fail("static cluster with weights" + cs);
            if ((U64)r.index_off4 * 4u + (U64)tc * 3u > ph.used_bytes || (U64)r.index_off4 * 4u < vstart)
                return fail("index block" + cs);
            const U8* ib = buf.data() + (size_t)r.index_off4 * 4u;
            for (U32 k = 0; k < tc * 3u; ++k)
                if (ib[k] >= vc) return fail("index past vertices" + cs);
            // DAG relations
            if (r.group >= H.group_count || r.child_group >= H.group_count || r.group == kGroupLeaf
                || r.child_group == kGroupRoot || r.group == r.child_group)
                return fail("group ids" + cs);
            if (r.group == kGroupRoot) { if (p >= H.root_pages) return fail("root cluster outside the root pages" + cs); }
            else if (D.group_count == 0 || r.group < D.group_first || r.group >= D.group_first + D.group_count)
                return fail("G4: cluster outside its group's page" + cs);
            if (r.group != kGroupRoot) ++members_seen[r.group];
            if ((r.child_group == kGroupLeaf) != (recLevel(r) == 0)) return fail("leaf / level" + cs);
            const ClusterGroup& G = mesh.groups[r.group];
            const ClusterGroup& C = mesh.groups[r.child_group];
            if (G.error < C.error) return fail("G1: error not monotonic" + cs);
            if (r.group != kGroupRoot && r.child_group != kGroupLeaf && !sphereContains(G.sphere, C.sphere))
                return fail("G2: group sphere not nested" + cs);
            if (r.group != kGroupRoot)
                for (int k = 0; k < 4; ++k)
                    if ((G.joint_mask[k] & r.joint_mask[k]) != r.joint_mask[k]
                        || (r.child_group != kGroupLeaf && (G.joint_mask[k] & C.joint_mask[k]) != C.joint_mask[k]))
                        return fail("G3: joint mask not nested" + cs);
            ++clusters;
        }
        if (lvl != D.level) return fail("page level" + ps);
    }
    if (clusters != H.cluster_count) return fail("cluster count");
    for (U32 g = kFirstRealGroup; g < H.group_count; ++g)
        if (members_seen[g] != (mesh.groups[g].counts & 0xFFu)) return fail("group member count " + std::to_string(g));
    return true;
}

namespace
{
struct EdgeKey
{
    U64 a, b;
    bool operator==(const EdgeKey& o) const { return a == o.a && b == o.b; }
    bool operator<(const EdgeKey& o) const { return a < o.a || (a == o.a && b < o.b); }
};
struct EdgeKeyHash
{
    size_t operator()(const EdgeKey& e) const
    {
        return (size_t)(e.a * 0x9E3779B97F4A7C15ull ^ (e.b + 0x632BE59BD9B4E019ull) * 0xC2B2AE3D27D4EB4Full);
    }
};
struct DCluster
{
    U32 page, group, child;
    std::vector<U64> keys;   // 3 per triangle: exact lattice position (48 bits)
    bool border_free;        // its generating group is CLG_FACE_BORDER_FREE (G7)
};

void boundaryOf(const std::vector<const DCluster*>& sel, std::vector<EdgeKey>& out)
{
    std::unordered_map<EdgeKey, U32, EdgeKeyHash> cnt;
    for (const DCluster* c : sel)
        for (size_t t = 0; t + 2 < c->keys.size(); t += 3)
            for (int k = 0; k < 3; ++k)
            {
                U64 a = c->keys[t + k], b = c->keys[t + (k + 1) % 3];
                if (a == b) continue;
                if (a > b) std::swap(a, b);
                ++cnt[EdgeKey{a, b}];
            }
    out.clear();
    for (const auto& kv : cnt)
        if (kv.second & 1u) out.push_back(kv.first);
    std::sort(out.begin(), out.end());
}
}

bool LLCleanClusterBuilder::validateCuts(const LLClusterMeshData& mesh, U32 seed, std::string* why)
{
    auto fail = [&](const std::string& s) { if (why) *why = s; return false; };
    if (!mesh.hasPayload()) return fail("no payload");
    const ClusterFileHeader& H = mesh.header;
    // decode every cluster's triangles to exact lattice positions
    std::vector<DCluster> cls;
    cls.reserve(H.cluster_count);
    std::vector<U8> buf(kPageBytes);
    for (U32 p = 0; p < H.page_count; ++p)
    {
        if (!mesh.copyPage(p, buf.data())) return fail("page read");
        ClusterPageHeader ph;
        std::memcpy(&ph, buf.data(), sizeof(ph));
        const U32 n = ph.version_count & 0xFFu;
        for (U32 i = 0; i < n; ++i)
        {
            ClusterRecord r;
            std::memcpy(&r, buf.data() + kPageHeaderBytes + (size_t)i * kClusterRecordBytes, sizeof(r));
            DCluster c{p, r.group, r.child_group, {}, (mesh.groups[r.child_group].flags & CLG_FACE_BORDER_FREE) != 0};
            const U32 tc = recTriCount(r);
            const U8* ib = buf.data() + (size_t)r.index_off4 * 4u;
            c.keys.resize((size_t)tc * 3);
            for (U32 k = 0; k < tc * 3u; ++k)
            {
                QVertex q;
                std::memcpy(&q, buf.data() + (size_t)(r.vert_off20 + ib[k]) * kQVertexBytes, sizeof(q));
                c.keys[k] = (U64)(U16)q.pos[0] | ((U64)(U16)q.pos[1] << 16) | ((U64)(U16)q.pos[2] << 32);
            }
            cls.push_back(std::move(c));
        }
    }
    std::vector<const DCluster*> sel;
    std::vector<EdgeKey> lod0, cut;
    for (const DCluster& c : cls) if (c.child == kGroupLeaf) sel.push_back(&c);
    boundaryOf(sel, lod0);
    // LOD0's open-border positions (G7: a border-free level moves the face's border only along
    // it -- the regular simplifier collapses a border vertex onto a border vertex)
    std::unordered_set<U64> border_pos;
    for (const EdgeKey& e : lod0) { border_pos.insert(e.a); border_pos.insert(e.b); }

    // identity instance
    ClusterInstance inst{};
    inst.xform[0] = inst.xform[5] = inst.xform[10] = 1.f;
    inst.lod_scale = 1.f;
    inst.tol_scale = 1.f;
    std::vector<F32> errs;
    for (U32 g = kFirstRealGroup; g < H.group_count; ++g) errs.push_back(mesh.groups[g].error);
    std::sort(errs.begin(), errs.end());
    const F32 r = std::max(H.mesh.sphere[3], 1e-4f);

    U32 rng = seed ? seed : 1u;
    auto rnd = [&]() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return (F32)(rng & 0xFFFFFFu) / 16777216.f; };

    struct Sample { ClusterView v; F32 tol; bool partial; };
    std::vector<Sample> samples;
    {
        const F32 qs[5] = {0.f, 0.25f, 0.5f, 0.75f, 1.f};
        for (F32 q : qs)
        {
            Sample s{};
            s.v.flags = CLV_ORTHO;
            s.v.px_scale = 1.f;
            s.tol = errs.empty() ? 0.f : errs[(size_t)(q * (F32)(errs.size() - 1))];
            s.partial = false;
            samples.push_back(s);
        }
        const F32 dists[4] = {0.3f, 2.f, 8.f, 40.f};
        for (F32 dd : dists)
        {
            Sample s{};
            F32 dir[3] = {rnd() * 2.f - 1.f, rnd() * 2.f - 1.f, rnd() * 2.f - 1.f};
            const F32 l = std::max(std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]), 1e-3f);
            for (int k = 0; k < 3; ++k) s.v.cam_pos[k] = H.mesh.sphere[k] + dir[k] / l * dd * r;
            s.v.cam_pos[3] = 0.01f * r;
            for (int k = 0; k < 3; ++k) s.v.lod_cam[k] = s.v.cam_pos[k];   // (#749: the projection's camera; margin 0)
            s.v.px_scale = 1000.f;
            s.tol = 1.f;
            s.partial = false;
            samples.push_back(s);
        }
        // G7: the cut where each border-free group's outputs first appear (the moved face
        // border next to locked finer clusters: the case the position check below covers)
        for (U32 g = kFirstRealGroup, nfree = 0; g < H.group_count && nfree < 16; ++g)
            if (mesh.groups[g].flags & CLG_FACE_BORDER_FREE)
            {
                Sample s{};
                s.v.flags = CLV_ORTHO;
                s.v.px_scale = 1.f;
                s.tol = mesh.groups[g].error;
                s.partial = false;
                samples.push_back(s);
                ++nfree;
            }
        for (F32 dd : {300.f, 3000.f})
        {   // far perspective views: the coarse (border-free / tail) levels of the DAG floor
            Sample s{};
            for (int k = 0; k < 3; ++k) s.v.cam_pos[k] = H.mesh.sphere[k] + (k == 0 ? dd * r : 0.f);
            s.v.cam_pos[3] = 0.01f * r;
            for (int k = 0; k < 3; ++k) s.v.lod_cam[k] = s.v.cam_pos[k];
            s.v.px_scale = 1000.f;
            s.tol = 1.f;
            s.partial = false;
            samples.push_back(s);
        }
        for (int k = 0; k < 4; ++k)
        {
            Sample s{};
            s.v.flags = CLV_ORTHO;
            s.v.px_scale = 1.f;
            s.tol = errs.empty() ? 0.f : errs[k < 2 ? errs.size() / 4 : (errs.size() - 1) * 9 / 10];
            s.partial = true;
            samples.push_back(s);
        }
    }
    std::vector<U8> resident(H.page_count, 1);
    std::vector<U32> sel_count(H.group_count);
    for (size_t si = 0; si < samples.size(); ++si)
    {
        const Sample& s = samples[si];
        std::fill(resident.begin(), resident.end(), 1);
        if (s.partial)
        {
            // random evictions, closure kept: a page whose parent is gone goes too
            for (U32 p = H.root_pages; p < H.page_count; ++p)
            {
                if (rnd() < 0.3f) resident[p] = 0;
                const ClusterPageDir& D = mesh.pages[p];
                for (U32 i = 0; i < D.dep_count && resident[p]; ++i)
                    if (!resident[mesh.deps[D.dep_first + i]]) resident[p] = 0;
            }
        }
        std::vector<F32> proj(H.group_count);
        for (U32 g = 0; g < H.group_count; ++g) proj[g] = projectGroupError(mesh.groups[g], inst, s.v, 1.f);
        sel.clear();
        std::fill(sel_count.begin(), sel_count.end(), 0);
        for (const DCluster& c : cls)
        {
            if (!resident[c.page]) continue;
            const bool child_res = c.child == kGroupLeaf || resident[mesh.groups[c.child].page];
            if (clusterRenders(proj[c.group], proj[c.child], child_res, s.tol * inst.tol_scale))
            {
                sel.push_back(&c);
                ++sel_count[c.group];
            }
        }
        const std::string ss = " (sample " + std::to_string(si) + ", tol " + std::to_string(s.tol) + ")";
        if (sel.empty()) return fail("empty cut" + ss);
        // no overlap: a selected cluster's generating group has no selected member. (A group
        // is NOT selected all-or-none: its members may come from different generating groups,
        // e.g. retried finer clusters, and each switches with its own generating group.)
        for (const DCluster* c : sel)
            if (c->child != kGroupLeaf && sel_count[c->child] != 0) return fail("ancestor + descendant both selected" + ss);
        boundaryOf(sel, cut);
        bool any_free = false, one_group = true;
        for (const DCluster* c : sel)
        {
            any_free = any_free || c->border_free;
            one_group = one_group && c->child == sel.front()->child;
        }
        if (any_free)
        {
            // G7: the face's border may have moved (bounded by the diameter error), cracks
            // INSIDE the face still may not: every boundary edge of the cut runs between LOD0
            // border positions (a crack along a locked group border would expose interior
            // positions). One generating group (the FINAL tail, possibly topology-free) is a
            // single simplified surface: nothing to match.
            if (one_group) continue;
            for (const EdgeKey& e : cut)
                if (!border_pos.count(e.a) || !border_pos.count(e.b))
                    return fail("not watertight inside the face (border-free cut): an open edge off the LOD0 border, "
                                + std::to_string(sel.size()) + " clusters selected" + ss);
            continue;
        }
        if (cut != lod0)
        {
            std::vector<EdgeKey> extra, missing;
            std::set_difference(cut.begin(), cut.end(), lod0.begin(), lod0.end(), std::back_inserter(extra));
            std::set_difference(lod0.begin(), lod0.end(), cut.begin(), cut.end(), std::back_inserter(missing));
            return fail("not watertight: " + std::to_string(extra.size()) + " boundary edges not in LOD0, "
                        + std::to_string(missing.size()) + " LOD0 boundary edges missing, " + std::to_string(sel.size())
                        + " clusters selected" + ss);
        }
    }
    return true;
}
