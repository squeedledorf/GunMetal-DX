/**
 * @file llcleanterrainsmooth.cpp
 * @brief See llcleanterrainsmooth.h (#211).
 */
#include "llviewerprecompiledheaders.h"

#include "llcleanterrainsmooth.h"

#include "llterrainsmooth.h"
#include "llvkhitch.h"
#include "llcleaningest.h"
#include "llcleanmeshasset.h"
#include "llsurface.h"
#include "llsurfacepatch.h"
#include "llvosurfacepatch.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llworld.h"
#include "vkgfx/llvulkancontext.h"   // VRAM ladder terrain detail (#405)

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace
{
using LLCleanTerrainSmooth::kLevelDist;
using LLCleanTerrainSmooth::kMaxLevel;

constexpr U32 kSentinel    = 0xFFFFFFFFu;
constexpr F32 kBuildMargin = 16.f;    // level g is built within kLevelDist[g] + this ...
constexpr F32 kFreeMargin  = 40.f;    // ... and freed past kLevelDist[g] + this
constexpr U32 kVertBudget  = 16000;   // fine-level vertices built per frame (>= 1 build; ~0.5 ms)
constexpr F32 kSkirtMin    = 0.05f;   // skirt depth past the steepest edge cell's rise
constexpr F32 kBoxPad      = 0.25f;   // instance box pad (the faceted ingest's)
constexpr S32 kNumLevels   = kMaxLevel + 1;

struct PatchState
{
    U64 hash = 0;                    // samples + apron + composition
    U64 built[kNumLevels] = {};      // hash each level was built from (0 = not built)
    S32 i0 = 0, j0 = 0, gppe = 0;    // lower grid corner, grids per patch edge
    F32 skirt = kSkirtMin;
    F32 lo[3] = {}, hi[3] = {};      // instance box (region-local, every level + skirts)
    std::vector<F32> comp;           // composition at the (gppe + 1)^2 grid points
    U32 seen = 0;                    // last residency scan (or publish) that saw the patch
};
std::unordered_map<LLUUID, PatchState> s_state;
U32  s_scan = 1;
bool s_pending = true;   // the next tick scans (builds left over, an edit, a new patch)

// The mesh-store key of a patch's smooth level (one asset per level: freed on its own).
LLUUID levelId(const LLUUID& patch, S32 g)
{
    LLUUID id = patch;
    id.mData[15] ^= (U8)(0xA0 + g);
    id.mData[7]  ^= 0x5E;
    return id;
}

inline U64 fnvMix(U64 h, F32 f)
{
    U32 u;
    std::memcpy(&u, &f, 4);
    h ^= u;
    return h * 0x100000001b3ull;
}

// Everything a patch's smooth levels read: its samples, the one-sample apron the
// bicubic stencil reaches into (the neighbour patches / regions) and its composition.
U64 patchHash(const LLSurface& surf, const PatchState& st)
{
    U64 h = 0xcbf29ce484222325ull;
    for (S32 gy = st.j0 - 1; gy <= st.j0 + st.gppe + 1; ++gy)
        for (S32 gx = st.i0 - 1; gx <= st.i0 + st.gppe + 1; ++gx)
            h = fnvMix(h, surf.sampleZ(gx, gy));
    for (F32 c : st.comp) h = fnvMix(h, c);
    return h ? h : 1ull;
}

// Skirt depth and the instance box, from the patch's samples. Every level's surface
// lies within each cell's corner range (monotone bicubic; coarser levels and the morph
// interpolate points of it), so the box of the samples holds them, and a crack along an
// edge is at most that edge cell's rise.
void patchBounds(const LLSurface& surf, PatchState& st)
{
    const S32 n = st.gppe;
    F32 rise = 0.f;
    for (S32 k = 0; k < n; ++k)
    {
        rise = llmax(rise, fabsf(surf.getZ(st.i0 + k + 1, st.j0)     - surf.getZ(st.i0 + k, st.j0)));
        rise = llmax(rise, fabsf(surf.getZ(st.i0 + k + 1, st.j0 + n) - surf.getZ(st.i0 + k, st.j0 + n)));
        rise = llmax(rise, fabsf(surf.getZ(st.i0, st.j0 + k + 1)     - surf.getZ(st.i0, st.j0 + k)));
        rise = llmax(rise, fabsf(surf.getZ(st.i0 + n, st.j0 + k + 1) - surf.getZ(st.i0 + n, st.j0 + k)));
    }
    st.skirt = rise + kSkirtMin;
    F32 zlo = 1e30f, zhi = -1e30f;
    for (S32 j = 0; j <= n; ++j)
        for (S32 i = 0; i <= n; ++i)
        {
            const F32 z = surf.getZ(st.i0 + i, st.j0 + j);
            zlo = llmin(zlo, z);
            zhi = llmax(zhi, z);
        }
    const F32 mpg = surf.getMetersPerGrid();
    st.lo[0] = (F32)st.i0 * mpg - kBoxPad;
    st.lo[1] = (F32)st.j0 * mpg - kBoxPad;
    st.lo[2] = zlo - st.skirt - kBoxPad;
    st.hi[0] = (F32)(st.i0 + n) * mpg + kBoxPad;
    st.hi[1] = (F32)(st.j0 + n) * mpg + kBoxPad;
    st.hi[2] = zhi + kBoxPad;
}

// Build (or rebuild in place) level g of the patch and capture it. Vertices: the
// (gppe * 2^g + 1)^2 grid, then four skirt rows. texcoord0 = (composition, morph step):
// x is the legacy splat coord (clean_draw.frag terrain branch; bilinear between the grid
// points), y the height step that takes a vertex not on level g - 1 to level g - 1's
// surface (the triangle diagonal the index pattern below uses: (+1, 0) - (0, +1)).
bool buildLevel(const LLSurface& surf, const LLUUID& patch_id, PatchState& st, S32 g)
{
    const S32 S = 1 << g;
    const S32 N = st.gppe * S;
    const S32 W = N + 1;
    const U32 ngrid = (U32)(W * W);
    const U32 nv = ngrid + 4u * (U32)W;
    const U32 ni = (U32)(N * N * 6) + 4u * (U32)N * 12u;
    if (nv > 65535u || st.comp.size() != (size_t)((st.gppe + 1) * (st.gppe + 1))) return false;

    std::vector<LLVector3> pos(nv), nrm(nv);
    std::vector<LLVector2> uv(nv);
    std::vector<U16> idx(ni);
    const F32 mpg = surf.getMetersPerGrid();
    const F32 inv = 1.f / (F32)S;
    const S32 C = st.gppe + 1;
    // The samples the patch's cells read, resolved once (neighbour regions included):
    // [i0 - 1, i0 + gppe + 2]^2 -- the last vertex row / column sits in cell gppe, whose
    // stencil reaches one further (with zero weight).
    const S32 R = st.gppe + 4;
    std::vector<F32> cache((size_t)(R * R));
    for (S32 y = 0; y < R; ++y)
        for (S32 x = 0; x < R; ++x)
            cache[(size_t)(y * R + x)] = surf.sampleZ(st.i0 - 1 + x, st.j0 - 1 + y);
    auto sample = [&](S32 gx, S32 gy)
    {
        const S32 x = llclamp(gx - st.i0 + 1, 0, R - 1), y = llclamp(gy - st.j0 + 1, 0, R - 1);
        return cache[(size_t)(y * R + x)];
    };
    for (S32 b = 0; b <= N; ++b)
        for (S32 a = 0; a <= N; ++a)
        {
            // Dyadic, exact in float: the VS classifies a vertex's level from it.
            const F32 x = (F32)(st.i0 * S + a) * inv;
            const F32 y = (F32)(st.j0 * S + b) * inv;
            F32 dzdx = 0.f, dzdy = 0.f;
            const F32 z = LLTerrainSmooth::smoothHeight(x, y, sample, &dzdx, &dzdy);
            const U32 v = (U32)(b * W + a);
            pos[v].set(x * mpg, y * mpg, z);
            LLVector3 n(-dzdx / mpg, -dzdy / mpg, 1.f);
            n.normVec();
            nrm[v] = n;
            const S32 ca = llmin(a / S, st.gppe - 1), cb = llmin(b / S, st.gppe - 1);
            const F32 fa = (F32)(a - ca * S) * inv, fb = (F32)(b - cb * S) * inv;
            const F32 c00 = st.comp[cb * C + ca],       c10 = st.comp[cb * C + ca + 1];
            const F32 c01 = st.comp[(cb + 1) * C + ca], c11 = st.comp[(cb + 1) * C + ca + 1];
            const F32 comp = (c00 * (1.f - fa) + c10 * fa) * (1.f - fb) + (c01 * (1.f - fa) + c11 * fa) * fb;
            uv[v].set(comp, 0.f);
        }
    if (g > 0)
    {
        for (S32 b = 0; b <= N; ++b)
            for (S32 a = 0; a <= N; ++a)
            {
                const bool ao = (a & 1) != 0, bo = (b & 1) != 0;
                if (!ao && !bo) continue;   // a vertex of level g - 1: no step
                auto zat = [&](S32 aa, S32 bb) { return pos[(U32)(bb * W + aa)].mV[VZ]; };
                F32 parent;
                if (ao && !bo)      parent = 0.5f * (zat(a - 1, b) + zat(a + 1, b));
                else if (!ao && bo) parent = 0.5f * (zat(a, b - 1) + zat(a, b + 1));
                else                parent = 0.5f * (zat(a + 1, b - 1) + zat(a - 1, b + 1));
                const U32 v = (U32)(b * W + a);
                uv[v].mV[VY] = parent - pos[v].mV[VZ];
            }
    }

    U32 k = 0;
    for (S32 b = 0; b < N; ++b)
        for (S32 a = 0; a < N; ++a)
        {
            // Same pattern as the faceted ingest: diagonal (a + 1, b) - (a, b + 1).
            const U16 i00 = (U16)(b * W + a),       i10 = (U16)(b * W + a + 1);
            const U16 i01 = (U16)((b + 1) * W + a), i11 = (U16)((b + 1) * W + a + 1);
            idx[k++] = i00; idx[k++] = i10; idx[k++] = i01;
            idx[k++] = i10; idx[k++] = i11; idx[k++] = i01;
        }
    // Skirts: edge e's top vertices, a copy st.skirt lower (same normal, composition and
    // morph step), both windings (seen from either patch).
    for (S32 e = 0; e < 4; ++e)
    {
        const U32 base = ngrid + (U32)(e * W);
        for (S32 t = 0; t <= N; ++t)
        {
            const S32 a = e == 0 ? t : e == 1 ? t : e == 2 ? 0 : N;
            const S32 b = e == 0 ? 0 : e == 1 ? N : t;
            const U32 top = (U32)(b * W + a);
            pos[base + t] = pos[top];
            pos[base + t].mV[VZ] -= st.skirt;
            nrm[base + t] = nrm[top];
            uv[base + t]  = uv[top];
        }
        for (S32 t = 0; t < N; ++t)
        {
            const S32 a0 = e == 0 ? t : e == 1 ? t : e == 2 ? 0 : N;
            const S32 b0 = e == 0 ? 0 : e == 1 ? N : t;
            const S32 a1 = e < 2 ? a0 + 1 : a0;
            const S32 b1 = e < 2 ? b0 : b0 + 1;
            const U16 t0 = (U16)(b0 * W + a0), t1 = (U16)(b1 * W + a1);
            const U16 s0 = (U16)(base + t), s1 = (U16)(base + t + 1);
            idx[k++] = t0; idx[k++] = t1; idx[k++] = s0;
            idx[k++] = t1; idx[k++] = s1; idx[k++] = s0;
            idx[k++] = t0; idx[k++] = s0; idx[k++] = t1;
            idx[k++] = t1; idx[k++] = s0; idx[k++] = s1;
        }
    }
    llassert(k == ni);

    LLCleanMeshAsset& ma = LLCleanMeshAsset::instance();
    const LLUUID lid = levelId(patch_id, g);
    ma.captureTerrain(lid, 0, pos.data(), nrm.data(), uv.data(), nullptr, idx.data(), nv, ni,
                      /*force=*/true, /*tight=*/true);
    const LLCleanMeshAsset::Slot ms = ma.getSlot(lid, 0, 0);
    if (!ms.valid) return false;
    st.built[g] = st.hash;
    ma.registerLodSlot(patch_id, 0, g, ms.slot_id);
    return true;
}

void freeLevel(const LLUUID& patch_id, PatchState& st, S32 g, U32 replacement)
{
    if (!st.built[g]) return;
    LLCleanMeshAsset& ma = LLCleanMeshAsset::instance();
    // Unpublish first: freeAsset only clears groups keyed by the freed asset.
    ma.registerLodSlot(patch_id, 0, g, replacement);
    ma.freeAsset(levelId(patch_id, g));
    st.built[g] = 0;
}

// Back to the faceted capture at every level, every smooth level freed.
void releasePatch(const LLUUID& patch_id, PatchState& st)
{
    LLCleanMeshAsset& ma = LLCleanMeshAsset::instance();
    const LLCleanMeshAsset::Slot fs = ma.getSlot(patch_id, 0, 0);
    const U32 faceted = fs.valid ? fs.slot_id : kSentinel;
    for (S32 g = 0; g < kNumLevels; ++g)
    {
        freeLevel(patch_id, st, g, faceted);
        if (faceted != kSentinel) ma.registerLodSlot(patch_id, 0, g, faceted);
    }
}

void dirtyAllPatches()
{
    for (LLViewerRegion* region : LLWorld::getInstance()->getRegionList())
    {
        if (!region) continue;
        LLSurface& surf = region->getLand();
        const S32 ppe = surf.getPatchesPerEdge();
        for (S32 pj = 0; pj < ppe; ++pj)
            for (S32 pi = 0; pi < ppe; ++pi)
            {
                LLSurfacePatch* sp = surf.getPatch(pi, pj);
                LLVOSurfacePatch* vo = sp ? sp->getVObj() : nullptr;
                if (vo && !vo->isDead()) LLCleanIngest::instance().noteTerrainDirty(vo);
            }
    }
}

bool smoothPossible(const LLSurface& surf)
{
    // The VS classifies a vertex's level from its region-local position (exact dyadic
    // metres), and the finest level must fit U16 indices.
    return fabsf(surf.getMetersPerGrid() - 1.f) < 1e-6f && surf.getGridsPerPatchEdge() >= 1
        && surf.getGridsPerPatchEdge() * (1 << kMaxLevel) + 1 <= 253;
}
} // namespace

namespace LLCleanTerrainSmooth
{
bool enabled()
{
    return LLSurface::isSmoothing();
}

S32 detail()
{
    static LLCachedControl<U32> s_detail(gSavedSettings, "RenderVKTerrainSmoothDetail", 2);
    // VRAM soft ladder, mesh / terrain detail (#405 step 4): the EFFECTIVE detail drops
    // 1 / 2 / 3 levels at terrain LOD scale 3/4 / 1/2 / 1/3 (the residency scan frees the
    // finer levels at once); the setting keeps the user's value.
    const F32 vram = LLVulkanContext::instance().vramTerrainLodScale();
    const S32 drop = vram >= 0.99f ? 0 : vram >= 0.6f ? 1 : vram >= 0.4f ? 2 : 3;
    return llclamp((S32)(U32)s_detail - drop, 0, kMaxLevel);
}

bool publishPatch(LLVOSurfacePatch* patchp, U32 faceted_slot, S32 i0, S32 j0, const F32* comp,
                  F32 lo[3], F32 hi[3])
{
    if (!patchp) return false;
    const LLUUID id = patchp->getID();
    LLCleanMeshAsset& ma = LLCleanMeshAsset::instance();
    LLSurfacePatch* sp = patchp->getPatch();
    LLSurface* surf = sp ? sp->getSurface() : nullptr;
    if (!enabled() || !surf || !comp || !smoothPossible(*surf))
    {
        auto it = s_state.find(id);
        if (it != s_state.end())
        {
            releasePatch(id, it->second);
            s_state.erase(it);
        }
        for (S32 g = 0; g < kNumLevels; ++g) ma.registerLodSlot(id, 0, g, faceted_slot);
        return false;
    }

    PatchState& st = s_state[id];
    st.seen = s_scan;
    st.i0 = i0;
    st.j0 = j0;
    st.gppe = surf->getGridsPerPatchEdge();
    st.comp.assign(comp, comp + (size_t)((st.gppe + 1) * (st.gppe + 1)));
    const U64 h = patchHash(*surf, st);
    if (h != st.hash)
    {
        st.hash = h;
        patchBounds(*surf, st);
        s_pending = true;   // resident fine levels are stale: the next tick rebuilds them
    }
    if (st.built[0] != st.hash) buildLevel(*surf, id, st, 0);
    // Publish what is resident. A stale fine level (an edit) stays drawn until the
    // residency tick rebuilds it (first in line: it is near the camera).
    for (S32 g = 0; g < kNumLevels; ++g)
    {
        U32 slot = (g == 0) ? faceted_slot : kSentinel;
        if (st.built[g])
        {
            const LLCleanMeshAsset::Slot ms = ma.getSlot(levelId(id, g), 0, 0);
            if (ms.valid) slot = ms.slot_id;
            else st.built[g] = 0;
        }
        ma.registerLodSlot(id, 0, g, slot);
    }
    for (int c = 0; c < 3; ++c) { lo[c] = st.lo[c]; hi[c] = st.hi[c]; }
    return true;
}

void tick()
{
    VK_HITCH_SCOPE("terrainSmooth");
    static bool s_was_on = false;
    const bool on = enabled();
    if (!on)
    {
        if (!s_state.empty())
        {
            for (auto& kv : s_state) releasePatch(kv.first, kv.second);
            s_state.clear();
            // The faceted capture is reclaimable and no group names it while smoothing is
            // on, so the geometry GC may have freed it (releasePatch then publishes
            // nothing): re-ingest every patch, which re-captures + republishes it.
            dirtyAllPatches();
            LL_INFOS("VKTerrain") << "smooth terrain off: faceted patches republished" << LL_ENDL;
        }
        s_was_on = false;
        return;
    }
    if (!s_was_on)
    {
        // Every patch re-ingests (LLCleanIngest::drainTerrainDirty, bounded per frame)
        // and publishes its smooth levels.
        s_was_on = true;
        dirtyAllPatches();
        LL_INFOS("VKTerrain") << "smooth terrain on: re-ingesting the terrain" << LL_ENDL;
    }

    // RESIDENCY SCAN: when the camera moved, work is pending, the detail changed, or
    // every 16 frames (new / dead patches, stale levels).
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    const S32 det = detail();
    static LLVector3 s_last_cam(1e30f, 1e30f, 1e30f);
    static S32 s_last_det = -1;
    static U32 s_frames = 0;
    if (!s_pending && det == s_last_det && dist_vec_squared(cam, s_last_cam) < 1.f && ++s_frames < 16) return;
    s_frames = 0;
    s_last_cam = cam;
    s_last_det = det;
    ++s_scan;

    struct Cand { F32 d; LLUUID id; LLSurface* surf; S32 g; };
    static std::vector<Cand> cand;
    cand.clear();
    U32 freed = 0;
    for (LLViewerRegion* region : LLWorld::getInstance()->getRegionList())
    {
        if (!region) continue;
        LLSurface& surf = region->getLand();
        const LLVector3 org = region->getOriginAgent();
        const S32 ppe = surf.getPatchesPerEdge();
        for (S32 pj = 0; pj < ppe; ++pj)
            for (S32 pi = 0; pi < ppe; ++pi)
            {
                LLSurfacePatch* sp = surf.getPatch(pi, pj);
                LLVOSurfacePatch* vo = sp ? sp->getVObj() : nullptr;
                if (!vo || vo->isDead()) continue;
                auto it = s_state.find(vo->getID());
                if (it == s_state.end()) continue;   // not ingested (smooth) yet
                PatchState& st = it->second;
                st.seen = s_scan;
                F32 d2 = 0.f;
                for (int c = 0; c < 3; ++c)
                {
                    const F32 p = cam.mV[c];
                    const F32 a = org.mV[c] + st.lo[c], b = org.mV[c] + st.hi[c];
                    const F32 e = p < a ? a - p : (p > b ? p - b : 0.f);
                    d2 += e * e;
                }
                const F32 d = sqrtf(d2);
                for (S32 g = 1; g < kNumLevels; ++g)
                {
                    const bool want = g <= det && d < kLevelDist[g] + kBuildMargin;
                    const bool keep = g <= det && d < kLevelDist[g] + kFreeMargin;
                    if (st.built[g] && !keep)
                    {
                        freeLevel(it->first, st, g, kSentinel);
                        ++freed;
                    }
                    else if ((want && !st.built[g]) || (st.built[g] && st.built[g] != st.hash))
                        cand.push_back({ d, it->first, &surf, g });   // missing, or stale (an edit)
                }
            }
    }
    // Dead patches (region gone, patch replaced): release their storage.
    for (auto it = s_state.begin(); it != s_state.end();)
    {
        if (it->second.seen != s_scan)
        {
            releasePatch(it->first, it->second);
            it = s_state.erase(it);
            ++freed;
        }
        else ++it;
    }

    // Nearest first (coarser first at a tie: it covers more ground), under the budget.
    std::sort(cand.begin(), cand.end(), [](const Cand& x, const Cand& y)
              { return x.d != y.d ? x.d < y.d : x.g < y.g; });
    U32 verts = 0, built = 0;
    size_t i = 0;
    for (; i < cand.size(); ++i)
    {
        const Cand& c = cand[i];
        auto it = s_state.find(c.id);
        if (it == s_state.end()) continue;
        const U32 w = (U32)(it->second.gppe * (1 << c.g) + 1);
        const U32 nv = w * w + 4u * w;
        // (#521: failed builds count against the budget too -- a level that keeps failing
        // must not let the loop walk, and re-try, every candidate each frame.)
        if (verts > 0 && verts + nv > kVertBudget) break;
        verts += nv;
        if (buildLevel(*c.surf, c.id, it->second, c.g)) ++built;
    }
    s_pending = i < cand.size();
    LLVKHitch::count("terrainSmoothBuilds", built);
    LLVKHitch::count("terrainSmoothFreed", freed);
}
} // namespace LLCleanTerrainSmooth
