/**
 * @file llcleanmeshasset.cpp
 * @brief Implementation of LLCleanMeshAsset.
 */

#include "llviewerprecompiledheaders.h"

#include "llcleanmeshasset.h"
#include "llcleanrigdecimate.h"   // #561: decimated rigged steps live in this store
#include "llcleanrigshadowlod.h"   // #564: their shadow-caster renumbering
#include <glm/gtc/packing.hpp>
#include "llcleanprovenance.h"   // #29 tripwire: record slot/group mutations
#include "llcleanalert.h"        // #29 alert sink (source-mutation tripwire)

#include "llappviewer.h"   // gFrameCount (slot reclaim bookkeeping)
#include "lltimer.h"       // LLTimer (capture profiler)
#include "llgl.h"
#include "llthread.h"   // on_main_thread() -- capture upload thread check
#include "llprofiler.h"
#include "llrender.h"
#include "llvolume.h"
#include "llvector4a.h"
#include "v2math.h"
#include "llvolumemgr.h"   // LLVolumeLODGroup::getVolumeScaleFromDetail (async decode)
#include "llvkhitch.h"
#include "llcleanassetlife.h"   // #737 release counters
#include "llviewercontrol.h"   // RenderVKRawMeshCacheMB / RenderVKMemStatsLog
#include "llmemorygovernor.h"   // RAM #262: mesh decode pacing + RAM-scaled budgets

#include <algorithm>
#include <cmath>
#include <sstream>
#include <cstring>

namespace
{
// The mega-VBO is updated CONSTANTLY (a glBufferSubData per captured face), so
// it is a DYNAMIC buffer (see glBufferData usage), and it must be a size the
// driver reliably backs. 16M verts * 72 B = 1.15 GB in ONE GL buffer: that
// over-allocation reported success but silently DROPPED writes past scattered
// offsets under GPU load (#29 mass mesh dropout -- geometry read as zeros).
// 8M verts (~576 MB) comfortably covers observed scenes (~3M verts) with
// headroom and allocates reliably.
constexpr U32 INITIAL_VERTS = 1u << 23; // 8M verts
constexpr U32 INITIAL_IDX   = 1u << 25; // 32M indices
} // namespace

// #29 freeze profiler -- running totals for geometry capture/upload, read+reset
// once per frame by the renderer (see takeCaptureStats).
static std::atomic<U64> sCaptureNanos{0};
static std::atomic<U64> sCaptureBytes{0};
static std::atomic<U32> sCaptureCount{0};

namespace
{
// Float -> IEEE half, rounded toward -inf (down=true) or +inf, so packed slot
// bounds always CONTAIN the true extents (conservative for culling).
U16 halfDirected(F32 x, bool down)
{
    U16 h = (U16)glm::packHalf1x16(x);
    auto next = [](U16 b, bool up) -> U16 {
        const bool neg = (b & 0x8000u) != 0;
        if ((b & 0x7fffu) == 0) return up ? (U16)0x0001 : (U16)0x8001;   // +-0
        return (U16)((up != neg) ? b + 1 : b - 1);
    };
    for (int i = 0; i < 4; ++i)
    {
        const F32 v = glm::unpackHalf1x16(h);
        if (down ? (v <= x) : (v >= x)) break;
        h = next(h, !down);
    }
    return h;
}
}

// Pack a slot's captured volume-local bounds into the SlotInfo pad words (6 halfs,
// rounded outward). The GPU cull unions these over an instance's LOD group: the
// instance AABB comes from whichever LOD was captured first, and a later, larger LOD
// (a coarse LOD's face often covers less of the mesh) was frustum-culled while on screen.
void LLCleanMeshAsset::packSlotBounds(SlotInfo& si, const F32 lo[3], const F32 hi[3])
{
    if (!(lo[0] <= hi[0] && lo[1] <= hi[1] && lo[2] <= hi[2]))
    {
        si._pad0 = si._pad1 = si._pad2 = 0u;   // no bounds
        return;
    }
    const U16 l0 = halfDirected(lo[0], true), l1 = halfDirected(lo[1], true), l2 = halfDirected(lo[2], true);
    const U16 h0 = halfDirected(hi[0], false), h1 = halfDirected(hi[1], false), h2 = halfDirected(hi[2], false);
    si._pad0 = (U32)l0 | ((U32)l1 << 16);
    si._pad1 = (U32)l2 | ((U32)h0 << 16);
    si._pad2 = (U32)h1 | ((U32)h2 << 16);
}

// Bits (clean_cull.comp uvDensityWorld): 0-11 log2(d) over [-16, 16]; 12-16 / 17-21 the
// x / y normal weights in 31sts (z = 1 - x - y); 22-27 log2(stretch) over [0, 8]; bit 28
// valid (also keeps the float normal); bit 29 = density word (else the raw UV extent of
// LLCleanInstanceStore::packUVExtent).
// The statistic is a log2 histogram (1/8 octave) of the per-triangle density weighted by
// world area, read at the 75th percentile (bin upper edge): stretched islands and atlas
// sub-regions keep their share of the request instead of being averaged away by large
// low-density regions, and a few degenerate slivers can't dominate it. O(triangles).
U32 LLCleanMeshAsset::computeUVDensity(const F32* pos, U32 ps, const F32* uv, U32 us,
                                       const void* indices, bool idx16, U32 ni, U32 nv)
{
    if (!pos || !uv || !indices || ni < 3 || nv < 3) return 0u;
    constexpr int NBIN = 256;
    F64 hist[NBIN] = {};
    F64 total = 0.0, wn[3] = {0.0, 0.0, 0.0}, lstretch = 0.0;
    const U16* i16 = static_cast<const U16*>(indices);
    const U32* i32 = static_cast<const U32*>(indices);
    for (U32 t = 0; t + 2 < ni; t += 3)
    {
        U32 ix[3];
        for (int k = 0; k < 3; ++k) ix[k] = idx16 ? (U32)i16[t + k] : i32[t + k];
        if (ix[0] >= nv || ix[1] >= nv || ix[2] >= nv) continue;
        const F32* p0 = pos + (size_t)ix[0] * ps;
        const F32* p1 = pos + (size_t)ix[1] * ps;
        const F32* p2 = pos + (size_t)ix[2] * ps;
        const F32* t0 = uv + (size_t)ix[0] * us;
        const F32* t1 = uv + (size_t)ix[1] * us;
        const F32* t2 = uv + (size_t)ix[2] * us;
        const F32 e1[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
        const F32 e2[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
        const F32 du1 = t1[0] - t0[0], dv1 = t1[1] - t0[1];
        const F32 du2 = t2[0] - t0[0], dv2 = t2[1] - t0[1];
        const F32 det = du1 * dv2 - du2 * dv1;               // 2 x UV area (signed)
        const F32 n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                          e1[0] * e2[1] - e1[1] * e2[0]};
        const F32 n2 = n[0] * n[0] + n[1] * n[1] + n[2] * n[2];
        const F32 a2 = sqrtf(n2);                            // 2 x world area
        if (!(a2 > 1e-12f) || !(fabsf(det) > 1e-12f) || !std::isfinite(a2) || !std::isfinite(det)) continue;
        // Jacobian columns dP/du, dP/dv; sigma_max * sigma_min = |Pu x Pv| = a2 / |det|.
        const F32 inv = 1.f / det;
        F32 pu[3], pv[3];
        for (int c = 0; c < 3; ++c)
        {
            pu[c] = (e1[c] * dv2 - e2[c] * dv1) * inv;
            pv[c] = (e2[c] * du1 - e1[c] * du2) * inv;
        }
        const F32 ga = pu[0] * pu[0] + pu[1] * pu[1] + pu[2] * pu[2];
        const F32 gb = pu[0] * pv[0] + pu[1] * pv[1] + pu[2] * pv[2];
        const F32 gc = pv[0] * pv[0] + pv[1] * pv[1] + pv[2] * pv[2];
        const F32 ratio = a2 / fabsf(det);
        const F32 lmax = 0.5f * (ga + gc) + sqrtf(0.25f * (ga - gc) * (ga - gc) + gb * gb);
        const F32 stretch = llclamp(lmax / ratio, 1.f, 256.f);   // sigma_max / sigma_min
        const F32 l = 0.5f * log2f(ratio);                       // log2 sqrt(world / UV area)
        if (!std::isfinite(l) || !std::isfinite(stretch)) continue;
        const int bin = llclamp((int)floorf((l + 16.f) * 8.f), 0, NBIN - 1);
        hist[bin] += a2;
        total += a2;
        for (int c = 0; c < 3; ++c) wn[c] += (F64)n[c] * n[c] / a2;   // a2 * n_c^2 / |n|^2
        lstretch += (F64)a2 * log2f(stretch);
    }
    if (!(total > 0.0)) return 0u;
    const F64 target = 0.75 * total;
    F64 cum = 0.0;
    int bin = NBIN - 1;
    for (int b = 0; b < NBIN; ++b)
    {
        cum += hist[b];
        if (cum >= target) { bin = b; break; }
    }
    const F32 l = (F32)(bin + 1) / 8.f - 16.f;
    const U32 qd = (U32)llclamp((S32)lroundf((l + 16.f) * (4095.f / 32.f)), 0, 4095);
    const U32 qx = (U32)llclamp((S32)lroundf((F32)(wn[0] / total) * 31.f), 0, 31);
    const U32 qy = (U32)llclamp((S32)lroundf((F32)(wn[1] / total) * 31.f), 0, (S32)(31u - qx));
    const U32 qs = (U32)llclamp((S32)lroundf((F32)(lstretch / total) * (63.f / 8.f)), 0, 63);
    return qd | (qx << 12) | (qy << 17) | (qs << 22) | 0x30000000u;
}

bool LLCleanMeshAsset::decodeUVDensity(U32 b, F32& d, F32 w[3], F32& stretch)
{
    if ((b & 0x30000000u) != 0x30000000u) return false;
    d = exp2f((F32)(b & 0xFFFu) * (32.f / 4095.f) - 16.f);
    w[0] = (F32)((b >> 12) & 31u) / 31.f;
    w[1] = (F32)((b >> 17) & 31u) / 31.f;
    w[2] = llmax(1.f - w[0] - w[1], 0.f);
    stretch = exp2f((F32)((b >> 22) & 63u) * (8.f / 63.f));
    return true;
}

void LLCleanMeshAsset::addCaptureStat(F64 seconds, U64 bytes)
{
    sCaptureNanos.fetch_add((U64)(seconds * 1.0e9), std::memory_order_relaxed);
    sCaptureBytes.fetch_add(bytes, std::memory_order_relaxed);
    sCaptureCount.fetch_add(1u, std::memory_order_relaxed);
}

namespace
{
// RAII: time a capture call and fold it into the running totals on scope exit.
struct CaptureScope
{
    LLTimer t;
    U64     bytes;
    explicit CaptureScope(U64 b) : bytes(b) {}
    ~CaptureScope() { LLCleanMeshAsset::addCaptureStat(t.getElapsedTimeF64(), bytes); }
};
} // namespace

std::atomic<U32> LLCleanMeshAsset::sLodFetchReq{0};
std::atomic<U32> LLCleanMeshAsset::sLodFetchGot{0};
std::atomic<U32> LLCleanMeshAsset::sLodFetchCap{0};

const LLCleanMeshAsset::QVertex*
LLCleanMeshAsset::cpuSlotPacked(U32 slot_id, U32& out_verts) const
{
    out_verts = 0;
    if (slot_id >= mMirrorVerts.size() || mMirrorVerts[slot_id].size() <= QV_HEADER)
        return nullptr;
    out_verts = (U32)mMirrorVerts[slot_id].size() - QV_HEADER;
    return mMirrorVerts[slot_id].data();
}

bool LLCleanMeshAsset::cpuSlotHeader(U32 slot_id, QHeader& out) const
{
    U32 n = 0;
    const QVertex* r = cpuSlotPacked(slot_id, n);
    if (!r) return false;
    LLCleanQV::readHeader(r, out);
    return true;
}

// static
void LLCleanMeshAsset::quantizeVerts(const MirrorVertex* v, U32 n, std::vector<QVertex>& out,
                                     const QHeader* domain)
{
    constexpr U32 kMV = (U32)(sizeof(MirrorVertex) / sizeof(F32));
    QHeader h;
    if (domain) h = *domain;
    else LLCleanQV::makeHeader(n ? v[0].pos : nullptr, kMV, n ? v[0].uv : nullptr, kMV, n, h);
    out.resize((size_t)n + QV_HEADER);
    LLCleanQV::writeHeader(out.data(), h);
    QVertex* q = out.data() + QV_HEADER;
    for (U32 i = 0; i < n; ++i)
        LLCleanQV::encode(h, v[i].pos, v[i].uv, v[i].normal, v[i].tangent, q[i]);
}

// static
void LLCleanMeshAsset::decodeVerts(const QVertex* range, U32 n, std::vector<MirrorVertex>& out)
{
    out.resize(n);
    if (!range || n == 0) return;
    QHeader h;
    LLCleanQV::readHeader(range, h);
    const QVertex* q = range + QV_HEADER;
    for (U32 i = 0; i < n; ++i)
    {
        MirrorVertex& m = out[i];
        LLCleanQV::decodePos(h, q[i], m.pos);
        LLCleanQV::decodeUV(h, q[i], m.uv);
        LLCleanQV::decodeNormal(q[i], m.normal);
        LLCleanQV::decodeTangent(q[i], m.tangent);
    }
}

const U16* LLCleanMeshAsset::cpuSlotIndices(U32 slot_id, U32& out_count) const
{
    out_count = 0;
    if (slot_id >= mMirrorIdx.size() || mMirrorIdx[slot_id].empty())
        return nullptr;
    out_count = (U32)mMirrorIdx[slot_id].size();
    return mMirrorIdx[slot_id].data();
}

const LLCleanMeshAsset::MirrorWeight*
LLCleanMeshAsset::cpuSlotWeights(U32 slot_id, U32& out_count) const
{
    out_count = 0;
    if (slot_id >= mMirrorWeights.size() || mMirrorWeights[slot_id].empty())
        return nullptr;
    out_count = (U32)mMirrorWeights[slot_id].size();
    return mMirrorWeights[slot_id].data();
}

LLCleanMeshAsset& LLCleanMeshAsset::instance()
{
    static LLCleanMeshAsset s;
    return s;
}

bool LLCleanMeshAsset::init()
{
    if (mReady) return true;

    // CPU-only store: the mega-VBO/IBO byte offsets capture() computes are
    // bookkeeping only (LLCleanRendererVK::fillFromCleanStores builds compact VK
    // offsets from the interleaved CPU mirror). Keep the SlotInfo table and the
    // used counts alive.
    mSlotInfo.reserve(65536);
    mSlotInfoGPUCap = 65536;
    mVertCap = INITIAL_VERTS;
    mIdxCap  = INITIAL_IDX;
    mReady = true;
    LL_INFOS("CleanMeshAsset") << "init: CPU mirror mode"
        << " vert_cap=" << mVertCap << " idx_cap=" << mIdxCap << LL_ENDL;
    return true;
}

// ---- LOD GROUP (upload-once / GPU LOD select) ----

U32 LLCleanMeshAsset::allocGroupIdLocked()
{
    const U32 now = (U32)gFrameCount;
    while (!mGroupQuarantine.empty() && (now - mGroupQuarantine.front().second) >= kIdQuarantineFrames)
    {
        mFreeGroupIds.push_back(mGroupQuarantine.front().first);
        mGroupQuarantine.pop_front();
    }
    if (!mFreeGroupIds.empty())
    {
        const U32 gid = mFreeGroupIds.back();
        mFreeGroupIds.pop_back();
        return gid;
    }
    LodGroup g;
    for (S32 i = 0; i < NUM_LOD; ++i) g.slot[i] = LOD_SENTINEL;
    mLodGroups.push_back(g);
    mGroupMeta.emplace_back();
    return (U32)mLodGroups.size() - 1u;
}

void LLCleanMeshAsset::markGroupDirtyLocked(U32 gid)
{
    ++mLodGroupGen;
    noteBestSlotChange();   // (callers wrote the group / face map before)
    mLodGroupDirtyMin = llmin(mLodGroupDirtyMin, gid);
    mLodGroupDirtyMax = llmax(mLodGroupDirtyMax, gid);
    if (mGroupDirtyOverflow) return;              // the next drain recopies everything
    if (mGroupDirty.size() >= 65536u)
    {
        mGroupDirtyOverflow = true;               // nobody drained for a while: bounded
        std::vector<U32>().swap(mGroupDirty);
        return;
    }
    mGroupDirty.push_back(gid);
}

bool LLCleanMeshAsset::drainLodGroupDirty(std::vector<U32>& out)
{
    std::lock_guard<std::mutex> lk(mMutex);
    out.swap(mGroupDirty);
    mGroupDirty.clear();
    const bool full = mGroupDirtyOverflow;
    mGroupDirtyOverflow = false;
    return full;
}

void LLCleanMeshAsset::pubAddLocked(U32 slot_id, U32 gid)
{
    if (slot_id == LOD_SENTINEL) return;
    if (slot_id >= (U32)mSlotPubs.size()) mSlotPubs.resize((size_t)slot_id + 1);
    std::vector<U32>& v = mSlotPubs[slot_id];
    if (std::find(v.begin(), v.end(), gid) == v.end()) v.push_back(gid);
}

void LLCleanMeshAsset::pubRemoveLocked(U32 slot_id, U32 gid)
{
    if (slot_id == LOD_SENTINEL || slot_id >= (U32)mSlotPubs.size() || gid >= (U32)mLodGroups.size()) return;
    for (S32 L = 0; L < NUM_LOD; ++L)
        if (mLodGroups[gid].slot[L] == slot_id) return;   // another level still names it
    std::vector<U32>& v = mSlotPubs[slot_id];
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i] == gid) { v[i] = v.back(); v.pop_back(); break; }
}

void LLCleanMeshAsset::unpublishSlotLocked(U32 slot_id)
{
    if (slot_id >= (U32)mSlotPubs.size()) return;
    std::vector<U32> groups;
    groups.swap(mSlotPubs[slot_id]);   // releases the per-slot list's capacity too
    for (U32 gid : groups)
    {
        if (gid >= (U32)mLodGroups.size()) continue;
        bool changed = false;
        for (S32 L = 0; L < NUM_LOD; ++L)
            if (mLodGroups[gid].slot[L] == slot_id) { mLodGroups[gid].slot[L] = LOD_SENTINEL; changed = true; }
        if (changed) markGroupDirtyLocked(gid);
    }
}

U32 LLCleanMeshAsset::allocSlotIdLocked()
{
    const U32 now = (U32)gFrameCount;
    while (!mSlotQuarantine.empty() && (now - mSlotQuarantine.front().second) >= kIdQuarantineFrames)
    {
        mFreeSlotIds.push_back(mSlotQuarantine.front().first);
        mSlotQuarantine.pop_front();
    }
    if (!mFreeSlotIds.empty())
    {
        const U32 id = mFreeSlotIds.back();
        mFreeSlotIds.pop_back();
        return id;
    }
    mSlotInfo.emplace_back();
    return (U32)mSlotInfo.size() - 1u;
}

void LLCleanMeshAsset::releaseSlotIdLocked(U32 slot_id)
{
    if (slot_id == 0xFFFFFFFFu) return;
    mSlotQuarantine.emplace_back(slot_id, (U32)gFrameCount);
}

U32 LLCleanMeshAsset::registerLodSlot(const LLUUID& mesh_id, S32 face_idx, S32 lod, U32 slot_id)
{
    if (lod < 0 || lod >= NUM_LOD) return LOD_SENTINEL;
    std::lock_guard<std::mutex> lk(mMutex);
    const auto key = std::make_pair(mesh_id, face_idx);
    auto it = mFaceLodGroup.find(key);
    U32 gid;
    if (it == mFaceLodGroup.end())
    {
        gid = allocGroupIdLocked();
        LodGroup& g = mLodGroups[gid];
        for (S32 i = 0; i < NUM_LOD; ++i) g.slot[i] = LOD_SENTINEL;
        GroupMeta& m = mGroupMeta[gid];
        m.mesh = mesh_id;
        m.face = face_idx;
        m.live = true;
        ++mLiveGroups;
        mFaceLodGroup.emplace(key, gid);
        markGroupDirtyLocked(gid);
    }
    else
    {
        gid = it->second;
    }
    mGroupMeta[gid].last_pub = (U32)gFrameCount;
    const U32 old_slot = mLodGroups[gid].slot[lod];
    if (old_slot != slot_id)
    {
        mLodGroups[gid].slot[lod] = slot_id;   // PUBLISH: this LOD is now drawable
        pubRemoveLocked(old_slot, gid);
        pubAddLocked(slot_id, gid);
        markGroupDirtyLocked(gid);
        CleanProv::record(CleanProv::REG_LODSLOT, slot_id, gid, (U32)lod, mesh_id);
    }
    return gid;
}

void LLCleanMeshAsset::groupRefSwap(U32 old_gid, U32 new_gid)
{
    if (old_gid == new_gid) return;
    std::lock_guard<std::mutex> lk(mGroupRefMutex);
    if (new_gid != LOD_SENTINEL && new_gid < (1u << 26))   // (sanity: never size by a garbage id)
    {
        if (new_gid >= (U32)mGroupRefs.size())
        {
            mGroupRefs.resize((size_t)new_gid + 1, 0u);
            mGroupUnrefFrame.resize((size_t)new_gid + 1, 0u);
        }
        ++mGroupRefs[new_gid];
    }
    if (old_gid != LOD_SENTINEL && old_gid < (1u << 26))
    {
        if (old_gid < (U32)mGroupRefs.size() && mGroupRefs[old_gid] > 0u)
        {
            if (--mGroupRefs[old_gid] == 0u && old_gid < (U32)mGroupUnrefFrame.size())
                mGroupUnrefFrame[old_gid] = (U32)gFrameCount;   // #737: release grace starts
        }
        else LL_WARNS_ONCE("CleanMeshAsset") << "LOD group " << old_gid << " refcount underflow" << LL_ENDL;
    }
}

void LLCleanMeshAsset::resetGroupRefs()
{
    std::lock_guard<std::mutex> lk(mGroupRefMutex);
    std::vector<U32>().swap(mGroupRefs);
    std::vector<U32>().swap(mGroupUnrefFrame);
}

U32 LLCleanMeshAsset::gcLodGroups(U32 current_frame)
{
    std::lock_guard<std::mutex> lk(mMutex);
    std::vector<U32> refs;
    {
        std::lock_guard<std::mutex> rl(mGroupRefMutex);   // leaf, under mMutex
        refs = mGroupRefs;
    }
    U32 freed = 0;
    const U32 n = (U32)mLodGroups.size();
    for (U32 gid = 0; gid < n; ++gid)
    {
        if (gcGroupFreeableLocked(gid, refs, current_frame)) ++freed;
    }
    return freed;
}

// Frees group gid when nothing uses it (see gcLodGroups); true when it did.
bool LLCleanMeshAsset::gcGroupFreeableLocked(U32 gid, const std::vector<U32>& refs, U32 current_frame)
{
    GroupMeta& m = mGroupMeta[gid];
    if (!m.live) return false;
    if (gid < (U32)refs.size() && refs[gid] != 0u) return false;          // an instance uses it
    if ((current_frame - m.last_pub) < kGroupGraceFrames) return false;   // just (re)published
    const LodGroup& g = mLodGroups[gid];
    for (S32 L = 0; L < NUM_LOD; ++L)
        if (g.slot[L] != LOD_SENTINEL) return false;                       // still owns geometry
    const auto key = std::make_pair(m.mesh, m.face);
    auto fit = mFaceLodGroup.find(key);
    if (fit != mFaceLodGroup.end() && fit->second == gid) mFaceLodGroup.erase(fit);
    mConvergeTrace.erase(key);
    m = GroupMeta{};
    --mLiveGroups;
    mGroupQuarantine.emplace_back(gid, current_frame);
    markGroupDirtyLocked(gid);
    LLCleanAssetLife::add(LLCleanAssetLife::MESH_GROUPS, 1);
    return true;
}

bool LLCleanMeshAsset::gcLodGroupsSlice(U32 current_frame, U32 max_groups, U32& freed)
{
    std::lock_guard<std::mutex> lk(mMutex);
    std::lock_guard<std::mutex> rl(mGroupRefMutex);   // leaf, under mMutex (as reapRetiredAssets)
    const U32 n = (U32)mLodGroups.size();
    U32 gid = mGroupGcCursor < n ? mGroupGcCursor : 0u;
    const U32 end = (U32)std::min<U64>((U64)gid + std::max(max_groups, 1u), n);
    for (; gid < end; ++gid)
    {
        if (gcGroupFreeableLocked(gid, mGroupRefs, current_frame)) ++freed;
    }
    mGroupGcCursor = gid < n ? gid : 0u;
    return gid >= n;
}

void LLCleanMeshAsset::retireAssetWhenUnused(const LLUUID& mesh_id)
{
    if (mesh_id.isNull()) return;
    std::lock_guard<std::mutex> lk(mMutex);
    if (mRetired.size() >= 8192u) mRetired.pop_front();   // the budgeted GC still covers it
    mRetired.emplace_back(mesh_id, (U32)gFrameCount);
}

U32 LLCleanMeshAsset::reapRetiredAssets(U32 current_frame)
{
    std::lock_guard<std::mutex> lk(mMutex);
    if (mRetired.empty()) return 0;
    U32 freed = 0;
    std::lock_guard<std::mutex> rl(mGroupRefMutex);   // leaf, under mMutex
    while (!mRetired.empty() && (current_frame - mRetired.front().second) >= kRetireDelayFrames)
    {
        const LLUUID mesh = mRetired.front().first;
        mRetired.pop_front();
        bool any = false, used = false, all_reclaim = true;
        for (S32 lod = 0; lod < NUM_LOD && !used; ++lod)
        {
            for (auto it = mSlots.lower_bound(Key{mesh, lod, INT32_MIN});
                 it != mSlots.end() && it->first.mesh_id == mesh && it->first.lod == lod; ++it)
            {
                any = true;
                const Slot& sl = it->second;
                if (!sl.reclaimable) all_reclaim = false;
                if (sl.slot_id == 0xFFFFFFFFu || sl.slot_id >= (U32)mSlotPubs.size()) continue;
                for (U32 gid : mSlotPubs[sl.slot_id])
                    if (gid < (U32)mGroupRefs.size() && mGroupRefs[gid] != 0u) { used = true; break; }
                if (used) break;
            }
        }
        if (!any || used || !all_reclaim) continue;
        U64 bytes = 0;
        for (S32 lod = 0; lod < NUM_LOD; ++lod)
        {
            auto it = mSlots.lower_bound(Key{mesh, lod, INT32_MIN});
            auto end_it = it;
            while (end_it != mSlots.end() && end_it->first.mesh_id == mesh && end_it->first.lod == lod) ++end_it;
            freeAssetRangeLocked(it, end_it, bytes);
        }
        ++freed;
    }
    mRetiredFreed += freed;
    return freed;
}

U32 LLCleanMeshAsset::getLodGroupId(const LLUUID& mesh_id, S32 face_idx) const
{
    std::lock_guard<std::mutex> lk(mMutex);
    auto it = mFaceLodGroup.find(std::make_pair(mesh_id, face_idx));
    return it == mFaceLodGroup.end() ? LOD_SENTINEL : it->second;
}

void LLCleanMeshAsset::collectGroupSlots(U32 group_id, std::unordered_set<U32>& out) const
{
    if (group_id == 0xFFFFFFFFu) return;
    std::lock_guard<std::mutex> lk(mMutex);
    if (group_id >= (U32)mLodGroups.size()) return;
    const LodGroup& g = mLodGroups[group_id];
    for (S32 L = 0; L < NUM_LOD; ++L)
        if (g.slot[L] != LOD_SENTINEL) out.insert(g.slot[L]);
}

void LLCleanMeshAsset::collectGroupSlotsUpTo(U32 group_id, S32 max_level, std::unordered_set<U32>& out) const
{
    if (group_id == 0xFFFFFFFFu) return;
    std::lock_guard<std::mutex> lk(mMutex);
    if (group_id >= (U32)mLodGroups.size()) return;
    const LodGroup& g = mLodGroups[group_id];
    auto resident = [&](U32 s) { return s != LOD_SENTINEL && s < (U32)mSlotInfo.size() && mSlotInfo[s].index_count > 0; };
    max_level = std::clamp<S32>(max_level, 0, NUM_LOD - 1);
    bool any = false;
    for (S32 L = 0; L <= max_level; ++L)
    {
        if (g.slot[L] == LOD_SENTINEL) continue;
        out.insert(g.slot[L]);   // (an unresident one too: a capture may be landing in it)
        any = any || resident(g.slot[L]);
    }
    if (any) return;
    for (S32 L = max_level + 1; L < NUM_LOD; ++L)
        if (resident(g.slot[L])) { out.insert(g.slot[L]); return; }
}

bool LLCleanMeshAsset::groupLocalBounds(U32 group_id, F32 lo[3], F32 hi[3]) const
{
    std::lock_guard<std::mutex> lk(mMutex);
    return groupLocalBoundsLocked(group_id, lo, hi);
}

// Half -> float by bit manipulation (normals in two shifts + an add): glm's generic
// unpackHalf1x16 was ~30% of the per-frame instance sync -- every pick-box refresh
// unpacks 6 halves per resident LOD of every touched instance.
static inline F32 halfBitsToF32(U16 h)
{
    const U32 sign = (U32)(h & 0x8000u) << 16;
    U32 e = (h >> 10) & 0x1Fu, m = h & 0x3FFu, bits;
    if (e == 0u)
    {
        if (m == 0u) bits = sign;
        else
        {
            e = 113u;
            while (!(m & 0x400u)) { m <<= 1; --e; }
            bits = sign | (e << 23) | ((m & 0x3FFu) << 13);
        }
    }
    else if (e == 31u) bits = sign | 0x7F800000u | (m << 13);
    else bits = sign | ((e + 112u) << 23) | (m << 13);
    F32 f;
    std::memcpy(&f, &bits, 4);
    return f;
}

bool LLCleanMeshAsset::groupLocalBoundsLocked(U32 group_id, F32 lo[3], F32 hi[3]) const
{
    if (group_id == 0xFFFFFFFFu || group_id >= (U32)mLodGroups.size()) return false;
    bool any = false;
    for (S32 L = 0; L < NUM_LOD; ++L)
    {
        const U32 sl = mLodGroups[group_id].slot[L];
        if (sl == LOD_SENTINEL || sl >= (U32)mSlotInfo.size()) continue;
        const SlotInfo& si = mSlotInfo[sl];
        if (si.index_count == 0 || (si._pad0 | si._pad1 | si._pad2) == 0u) continue;
        const F32 b[6] = { halfBitsToF32((U16)(si._pad0 & 0xFFFF)), halfBitsToF32((U16)(si._pad0 >> 16)),
                           halfBitsToF32((U16)(si._pad1 & 0xFFFF)), halfBitsToF32((U16)(si._pad1 >> 16)),
                           halfBitsToF32((U16)(si._pad2 & 0xFFFF)), halfBitsToF32((U16)(si._pad2 >> 16)) };
        for (int k = 0; k < 3; ++k)
        {
            lo[k] = std::min(lo[k], b[k]);
            hi[k] = std::max(hi[k], b[3 + k]);
        }
        any = true;
    }
    return any;
}

void LLCleanMeshAsset::stopDecodes()
{
    // Async decode: stop the workers first (close() joins), then drop results.
    // Called from LLAppViewer::cleanup (both backends): the pool must NOT be left
    // to the static destructor -- by then the fiber/coro machinery is gone and
    // ThreadPool::close() throws inside a noexcept dtor -> std::terminate at exit.
    mDecodesStopped = true;
    LLCleanRigDecimator::instance().stop();   // #561: its workers too
    LLCleanRigShadowLod::instance().stop();   // #564: its matcher thread too
    if (mDecodePool) { mDecodePool->close(); mDecodePool.reset(); }
    // Async capture workers likewise; then drop their jobs (and volume references)
    // here, on the render thread.
    if (mCapPool) { mCapPool->close(); mCapPool.reset(); }
    {
        std::lock_guard<std::mutex> lk(mCapMutex);
        mCapDone.clear();
    }
    mCapJobs.clear();
    {
        std::lock_guard<std::mutex> dl(mDecodeMutex);
        for (auto& kv : mDecodes) if (kv.second.vol) kv.second.vol->unref();
        mDecodes.clear(); mDecodeQPrio.clear(); mDecodeQ.clear(); mDecodeDone.clear(); mDecodeBytes = 0;
    }
}

void LLCleanMeshAsset::shutdown()
{
    stopDecodes();
    LLCleanRigDecimator::instance().reset();   // #561: its step slots go with the store
    LLCleanRigShadowLod::instance().reset();   // #564: slot ids are meaningless now
    std::lock_guard<std::mutex> lk(mMutex);
    mLodGroups.clear();
    ++mLodGroupGen;
    noteBestSlotChange();
    mFaceLodGroup.clear();
    mGroupMeta.clear();
    mLiveGroups = 0;
    mFreeGroupIds.clear();
    mGroupQuarantine.clear();
    mSlotPubs.clear();
    mGroupDirty.clear();
    mGroupDirtyOverflow = true;
    mConvergeTrace.clear();
    mRetired.clear();
    mLodGroupGPUCap = 0;
    mSlotInfo.clear();
    mFreeSlotIds.clear();
    mSlotQuarantine.clear();
    mVertCap = mIdxCap = mVertUsed = mIdxUsed = 0;
    mSlots.clear();
    mSlotIdx.clear();
    mCapturedAssets.clear();
    mCaptureFail.clear();
    mUnlatch.clear();
    mMirrorRel.clear();   // (cluster-owned mirror releases: the slots are gone)
    mMirrorRelQueue.clear();
    mRematWake.clear();
    mRematLods.clear();
    mRematFaces.clear();
    mRematWanted.store(0, std::memory_order_relaxed);
    mMirrorRelBytes = 0;
    mReady = false;
    {
        std::lock_guard<std::mutex> rl(mGroupRefMutex);
        mGroupRefs.clear();
        mGroupUnrefFrame.clear();
    }
    {
        std::lock_guard<std::mutex> rm(mRawMeshMutex);
        mRawMesh.clear(); mRawLru.clear(); mRawEvicted.clear(); mRawEvictedOrder.clear();
        mRawBytes = 0;
    }
}

// The vertex/index ranges are pure bookkeeping (GL-A10/B6): the VK packer builds
// its own compact GPU offsets from the per-slot CPU mirrors and never reads these.
// Hand out a wrapping bump offset (unique per live allocation in practice, for
// diagnostics) and keep only the used counts that feed residentGeometryBytes and
// the geometry GC. Allocation never fails.
static U32 bumpOffset(U32& cursor, U32 count, U64 limit)
{
    if ((U64)cursor + count > limit) cursor = 0;
    const U32 off = cursor;
    cursor += count;
    return off;
}

U32 LLCleanMeshAsset::allocVerts(U32 count)
{
    if (count == 0) return 0;
    mVertUsed += count;
    return bumpOffset(mVKVertCursor, count, 0xFFFFFFFFull / (U64)VERT_STRIDE_TOTAL);
}

U32 LLCleanMeshAsset::allocIdx(U32 count)
{
    if (count == 0) return 0;
    mIdxUsed += count;
    return bumpOffset(mVKIdxCursor, count, 0xFFFFFFFFull / (U64)IDX_STRIDE);
}

void LLCleanMeshAsset::freeVerts(U32 /*offset*/, U32 count)
{
    mVertUsed -= llmin(count, mVertUsed);
}

void LLCleanMeshAsset::freeIdx(U32 /*offset*/, U32 count)
{
    mIdxUsed -= llmin(count, mIdxUsed);
}

// capture() = prepareCapture + commitCapture (see the header). The split lets the
// ingest worker (RenderVKIngestWorker) run the pure-data half off the render thread;
// the synchronous path below behaves exactly as the old single function.
void LLCleanMeshAsset::capture(const LLUUID& mesh_id, S32 lod, const LLVolume* volume, bool force)
{
    if (!mReady || !volume) return;
    if (volume->getNumVolumeFaces() <= 0) return;
    if (!force)
    {
        // Already FULLY captured (>=1 face ingested): nothing to prepare.
        std::lock_guard<std::mutex> lk(mMutex);
        auto it = mCapturedAssets.find(std::make_pair(mesh_id, lod));
        if (it != mCapturedAssets.end() && it->second) return;
    }
    CaptureScope _cap(0); // profiler: time the whole mesh/prim capture
    CapturePrep prep;
    prepareCapture(volume, prep);
    commitCapture(mesh_id, lod, prep, force);
}

// PREPARE (any thread): per-face validation, bounds, index widening, the interleaved
// VK mirror (with generated tangents where the volume has none) and weights. Reads
// only `volume`; never mutates it (tangents go to a local buffer).
void LLCleanMeshAsset::prepareCapture(const LLVolume* volume, CapturePrep& out)
{
    out.faces.clear();
    out.sculpt_level = -1;
    if (!volume) return;
    const S32 nfaces = volume->getNumVolumeFaces();
    if (nfaces <= 0) return;
    // Record the sculpt decode level this geometry came from (-1 for a non-sculpt
    // volume). registerFace re-captures when the live volume decodes finer than this.
    out.sculpt_level = volume->getSculptLevel();
    out.faces.resize((size_t)nfaces);
    for (S32 fi = 0; fi < nfaces; ++fi)
    {
        const LLVolumeFace& vf = volume->getVolumeFace(fi);
        CapturePrep::Face& f = out.faces[(size_t)fi];
        const U32 nv = (U32)vf.mNumVertices;
        const U32 ni = (U32)vf.mNumIndices;
        f.nv = nv; f.ni = ni;
        // Skip DEGENERATE faces: a triangle needs >= 3 verts / 3 indices. A
        // sculpt mid-decode can momentarily present a face with nv==1 (real
        // vertices not regenerated yet) while getSculptLevel() already reads 0;
        // capturing it latched a single invisible point that never re-captured
        // (#29 "geometry gone after early camera move"). Treating nv<3 as
        // not-ready means created stays 0 for a single-face sculpt -> the latch is
        // dropped in commit -> the next rebuild retries once real geometry arrives.
        if (nv < 3 || ni < 3 || !vf.mPositions || !vf.mTexCoords || !vf.mIndices)
        {
            f.skip = CAPF_EMPTY_FACE;
            continue;
        }
        // DEGENERATE-INDEX / OUT-OF-RANGE-INDEX faces (#29): see commitCapture. TRUE
        // max index (full scan, NO early break -- we need the actual maximum).
        U32 idx_max = 0;
        for (U32 k = 0; k < ni; ++k)
        {
            const U32 iv = (U32)vf.mIndices[k];
            if (iv > idx_max) idx_max = iv;
        }
        f.idx_max = idx_max;
        if (idx_max == 0u && nv > 1u) { f.skip = CAPF_DEGEN_IDX; continue; }
        if (idx_max >= nv)            { f.skip = CAPF_IDX_OOR;   continue; }

        f.has_weights  = (vf.mWeights != nullptr);
        f.has_tangents = (vf.mTangents != nullptr);
        // NON-FINITE POSITIONS (a broken decode / sculpt): a NaN / Inf vertex would
        // reach the BLAS build and the cull's bounds. Such components are captured as 0
        // (finitePos below), here and in the mirror.
        auto finitePos = [](F32 x) { return std::isfinite(x) ? x : 0.f; };
        // Bounds of the positions we're about to upload (the instance AABB / the #29
        // stray-write detector's reference).
        {
            F32 lo[3] = {1e30f,1e30f,1e30f}, hi[3] = {-1e30f,-1e30f,-1e30f};
            for (U32 v = 0; v < nv; ++v)
            {
                const F32* p = vf.mPositions[v].getF32ptr();
                for (int c = 0; c < 3; ++c)
                {
                    const F32 q = finitePos(p[c]);
                    if (q < lo[c]) lo[c] = q;
                    if (q > hi[c]) hi[c] = q;
                }
            }
            for (int c = 0; c < 3; ++c) { f.lo[c] = lo[c]; f.hi[c] = hi[c]; }
        }
        f.uv_density = computeUVDensity(vf.mPositions[0].getF32ptr(), 4, vf.mTexCoords[0].mV, 2,
                                        vf.mIndices, true, ni, nv);

        // GEOMETRY MIRROR. Retain an INTERLEAVED CPU copy of this slot's
        // geometry so the Vulkan world renderer (which wants pos3/uv2/norm3/tan4
        // interleaved rather than the sectioned layout) can build its own VBO/IBO.
        // Section->interleave repack happens HERE, at the one moment all source arrays
        // are live.
        // LL's U16 indices, face-local: the VK index buffer is 16-bit too (#573).
        f.idx.assign(vf.mIndices, vf.mIndices + ni);
        // A face can only reference 65536 vertices (U16 indices); trailing vertices past
        // that are never drawn -- drop them so the slot fits the 16-bit index range
        // (LLVolumeFace never exceeds it in practice).
        const U32 nv_keep = std::min<U32>(nv, LLCleanQV::kMaxSlotVerts);
        std::vector<MirrorVertex> fverts(nv_keep, MirrorVertex{});
        // TANGENTS (normal maps). Meshes carry MikkTSpace tangents from
        // cacheOptimize; prims get them lazily in LL's GL geometry path
        // (LLVolumeFace::createTangents), which VK skips. Generate them the
        // same way (LLCalculateTangentArray, w = handedness) into a LOCAL
        // buffer -- never mutate the source volume face from here.
        const LLVector4a* tans = vf.mTangents;
        LLVector4a* gen_tans = nullptr;
        if (!tans && vf.mNormals)
        {
            gen_tans = (LLVector4a*)ll_aligned_malloc_16(sizeof(LLVector4a) * nv);
            if (gen_tans)
            {
                LLCalculateTangentArray(nv, vf.mPositions, vf.mNormals, vf.mTexCoords,
                                        ni / 3, vf.mIndices, gen_tans);
                tans = gen_tans;
            }
        }
        MirrorVertex* mv = fverts.data();
        for (U32 v = 0; v < nv_keep; ++v)
        {
            if (tans)
            {
                const F32* t = tans[v].getF32ptr();
                mv[v].tangent[0] = t[0]; mv[v].tangent[1] = t[1];
                mv[v].tangent[2] = t[2]; mv[v].tangent[3] = (t[3] < 0.f) ? -1.f : 1.f;
            }
            const F32* p = vf.mPositions[v].getF32ptr();
            mv[v].pos[0] = finitePos(p[0]); mv[v].pos[1] = finitePos(p[1]); mv[v].pos[2] = finitePos(p[2]);
            // LLVector2 texcoords (mV[0], mV[1]).
            mv[v].uv[0] = vf.mTexCoords[v].mV[0];
            mv[v].uv[1] = vf.mTexCoords[v].mV[1];
            if (vf.mNormals)
            {
                const F32* n = vf.mNormals[v].getF32ptr();
                mv[v].normal[0] = n[0]; mv[v].normal[1] = n[1]; mv[v].normal[2] = n[2];
            }
            else
            {
                mv[v].normal[0] = 0.f; mv[v].normal[1] = 0.f; mv[v].normal[2] = 1.f;
            }
        }
        if (gen_tans) ll_aligned_free_16(gen_tans);
        // QUANTIZE (#573): the mirror holds the packed VK format (header + 20 B vertices).
        quantizeVerts(fverts.data(), nv_keep, f.verts);
        f.nv = nv_keep;
        // RIGGED pre-skin source: retain the skin weights (#582: LL's float-per-influence
        // weight4 encoded to 10 B, LLCleanQV::encodeWeight) so the VK pre-skin compute can
        // skin this slot. Only rigged faces carry mWeights; a slot without them keeps an
        // empty weight mirror -> the VK renderer marks its SlotInfo.has_weights = 0 and
        // never routes it rigged. A joint index > 255 (not expressible in the asset format)
        // drops the face's weights instead of wrapping.
        if (vf.mWeights)
        {
            f.wts.assign(nv_keep, MirrorWeight{});
            bool ok = true;
            for (U32 v = 0; v < nv_keep && ok; ++v)
                ok = LLCleanQV::encodeWeight(vf.mWeights[v].getF32ptr(), f.wts[v]);
            if (!ok)
            {
                LL_WARNS_ONCE("CleanMeshAsset") << "#582: skin joint index > 255 -- dropping the face's weights" << LL_ENDL;
                std::vector<MirrorWeight>().swap(f.wts);
                f.has_weights = false;
            }
            else
            {
                // #574 rigged welding: flag the face's open-boundary vertices (by position)
                // in their weight records -- the GPU weld pairs only those.
                constexpr size_t kMVStride = sizeof(MirrorVertex) / sizeof(F32);
                LLCleanQV::markWeldBoundary(f.wts.data(), nv_keep, f.idx.data(), (U32)f.idx.size(),
                                            fverts[0].pos, kMVStride);
            }
        }
    }
}

// COMMIT (render thread): latch, per-face slot allocation + SlotInfo publish, CPU
// mirror, unlatch-on-not-ready. Consumes the prepared vectors.
void LLCleanMeshAsset::commitCapture(const LLUUID& mesh_id, S32 lod, CapturePrep& prep, bool force)
{
    if (!mReady) return;
    const S32 nfaces = (S32)prep.faces.size();
    if (nfaces <= 0) return;
    // #29 RACE PROBE: the store / mirror writes below must stay on the render thread
    // (the VK store-fill worker relies on nothing writing the stores during its run).
    if (!on_main_thread())
        LL_WARNS("CleanRace") << "frame=" << gFrameCount
            << " ## capture commit OFF MAIN THREAD (RACE)" << LL_ENDL;

    // INVERT IMAGE-SCULPT NORMAL FIX. For image-based sculpties (SPHERE/PLANE/
    // TORUS/CYLINDER) the INVERT flag is NOT baked into LLVolumeFace::mNormals --
    // LL implements it purely by re-sampling positions (reversed T column), which
    // leaves the cross-product vertex normals pointing INWARD. Stock LL only looks
    // right because it draws sculpt/bump faces TWO-SIDED and flips per-fragment via
    // gl_FrontFacing; our single-pass deferred capture reads mNormals verbatim, so
    // an inverted sculpt (e.g. a sim-surround hill dome) comes out with every normal
    // facing away from the sky -> entirely unlit. Mesh sculpts already bake the
    // invert into mNormals (unpackVolumeFacesInternal), so exclude them. Negating the
    // captured normals here reproduces LL's intended outward orientation, scoped
    // precisely to inverted image sculpts (non-inverted sculpts are untouched, so the
    // old blanket two-sided band-aid's collateral damage doesn't recur).

    {
        std::lock_guard<std::mutex> lk(mMutex);
        // A freed cluster mirror being re-materialized (requestMirror): the asset is latched
        // and its slots valid, so only a forced re-capture rewrites them (in place).
        if (!force && rematKeyLocked(mesh_id, lod)) force = true;
        auto& flag = mCapturedAssets[std::make_pair(mesh_id, lod)];
        // force re-capture (sculpt decode improved): clear the latch and fall
        // through so the per-face loop overwrites each slot's SlotInfo in place and
        // frees the old ranges AFTER -- an atomic swap, no zeroed-slot window.
        if (flag && !force) return; // already FULLY captured (>=1 face ingested)
        // GC THRASH PROBE (#405): the GC freed this (mesh, lod) less than ~20 s ago and
        // something captures it again -- the in-use set and the level selection disagree.
        if (!flag)
        {
            auto u = mUnlatch.find(std::make_pair(mesh_id, lod));
            if (u != mUnlatch.end() && u->second.site == UNL_GC && (U32)gFrameCount - u->second.frame < 2400u)
                mGcRecaptures.fetch_add(1, std::memory_order_relaxed);
        }
        // Set the flag to gate concurrent captures, but we'll UNLATCH it below
        // if zero faces were ingested -- see the created==0 check after the loop.
        flag = true;
    }

    // Count faces actually ingested. A distant/large prim (megaprim sim-surround,
    // weatherdome) often registers BEFORE its LLVolume finishes decoding, so every
    // getVolumeFace() here is empty (nv==0 / null positions) and all faces hit the
    // `continue` below -- zero slots created. If we left mCapturedAssets latched
    // true in that case, the asset would be marked "captured" forever and the real
    // geometry (available on a later rebuild) would NEVER be ingested -> permanent
    // sentinel slot -> the object renders only as a selectable wireframe, never as
    // mesh. So track ingestion and unlatch on zero so the next rebuild retries.
    U32 created = 0;
    // PARTIAL-LATCH FIX (#88). The latch means "every face this LOD HAS is captured";
    // takeDecoded then refuses further decodes of the asset. It used to be kept as soon
    // as ONE face landed, so a face skipped as not-ready (degenerate/out-of-range
    // indices) or refused by the allocator was never retried -> that face (a tire, a
    // door) stayed missing for the session while its siblings rendered. Count those
    // skips separately from legitimately empty faces and unlatch when any happened; a
    // non-forced retry below skips faces that already have a valid slot.
    U32 present  = 0;   // faces skipped because they already hold a valid slot
    U32 notready = 0;   // not-ready / refused faces that should be retried
    U32 gaveup   = 0;   // deterministic not-ready faces past their retry budget
    auto noteSkip = [&](S32 fi, U8 reason, U32 nv, U32 ni)
    {
        std::lock_guard<std::mutex> lk(mMutex);
        const Key k{mesh_id, lod, fi};
        noteCaptureFailLocked(k, reason, nv, ni);
        if (reason == CAPF_EMPTY_FACE) return;
        // Allocator refusals are pressure-dependent: always retry. Bad indices in
        // decoded mesh bytes are deterministic: retry a few times (a live sculpt
        // volume can be mid-regeneration), then accept the face as absent.
        const CaptureFailRec& r = mCaptureFail[k];
        if (reason == CAPF_VERT_ALLOC || reason == CAPF_IDX_ALLOC || r.count <= 3u) ++notready;
        else
        {
            ++gaveup;
            if (r.count == 4u)
                LL_WARNS("VKIngest") << "capture: giving up on face mesh=" << mesh_id << " lod=" << lod
                    << " face=" << fi << " reason=" << captureFailName(reason) << " nv=" << nv
                    << " ni=" << ni << " after " << r.count << " attempts" << LL_ENDL;
        }
    };

    // Per-face publish. All faces of one asset share the (mesh_id, lod)
    // namespace, distinguished by face_idx. Indices are stored
    // face-local (0..numVerts-1) so the draw-time attribute pointer
    // (which we'll set to the slot's POS section base) makes
    // glDrawElements with no baseVertex correct.
    for (S32 fi = 0; fi < nfaces; ++fi)
    {
        CapturePrep::Face& pf = prep.faces[(size_t)fi];
        const U32 nv = pf.nv;
        const U32 ni = pf.ni;
        if (pf.skip == CAPF_EMPTY_FACE)
        {
            noteSkip(fi, CAPF_EMPTY_FACE, nv, ni);
            continue;
        }
        // Retry of a partially captured asset: keep faces that already landed.
        if (!force)
        {
            std::lock_guard<std::mutex> lk(mMutex);
            const Slot* have = slotFind(Key{mesh_id, lod, fi});
            if (have && have->valid && have->slot_id != 0xFFFFFFFFu) { ++present; continue; }
        }
        // Skip DEGENERATE-INDEX faces (#29 ROOT, 2026-08-09). A coarse LOD can be
        // presented mid-decode with a valid position array but an index array that
        // is still ALL ZERO (count set, values not filled). It passes the ni>=3 /
        // non-null guard, so capture uploads a slot whose every triangle is
        // (v0,v0,v0) -> zero screen area -> the cull draws it at distance and the
        // object VANISHES (proven via GPU dump: coarse-LOD idx max==0 while verts
        // valid; the drawn commands point at those ranges; on-screen prims, zero
        // samples, immune to cull-off + depth-ALWAYS). A real >1-vertex mesh MUST
        // reference some vertex past 0, so max index == 0 with nv>1 = not-ready.
        // Treat as not-ready: skip (created stays 0 -> latch drops -> re-capture
        // once the real indices arrive), exactly like the nv<3 sculpt guard.
        if (pf.skip == CAPF_DEGEN_IDX)
        {
            LL_WARNS_ONCE("CleanMeshAsset") << "capture: skipping degenerate-index face"
                << " mesh=" << mesh_id.asString() << " lod=" << lod << " face=" << fi
                << " nv=" << nv << " ni=" << ni
                << " (all indices 0 -> not-ready coarse LOD; will re-capture) [#29]" << LL_ENDL;
            noteSkip(fi, CAPF_DEGEN_IDX, nv, ni);
            continue;
        }
        // OUT-OF-RANGE INDEX (#29 sibling flavor -- the VK sim-surround stretch).
        // capture() can race the async IMAGE-SCULPT regen: the index array is
        // already the full-res grid (indices up to e.g. 4224 for a 64x64 sculpt)
        // while mPositions is still the coarse 1089-vert array -> indices point
        // PAST the vertex array. With the compact VK packing, base_vertex+index
        // then reads into the NEXT slot's verts -> triangles fan to a wrong point
        // ("geometry stretching toward a non-origin point"). Treat as not-ready:
        // skip -> created stays 0 -> latch drops -> re-capture when consistent.
        if (pf.skip == CAPF_IDX_OOR)
        {
            LL_WARNS_ONCE("CleanMeshAsset") << "capture: skipping out-of-range-index face"
                << " mesh=" << mesh_id.asString() << " lod=" << lod << " face=" << fi
                << " nv=" << nv << " ni=" << ni << " idx_max=" << pf.idx_max
                << " (indices reference verts past the position array -> not-ready"
                << " mid-decode sculpt; will re-capture) [#29]" << LL_ENDL;
            noteSkip(fi, CAPF_IDX_OOR, nv, ni);
            continue;
        }

        Slot slot;
        {
            std::lock_guard<std::mutex> lk(mMutex);
            const U32 vbase = allocVerts(nv);
            const U32 ibase = allocIdx(ni);

            // Section bases in the mega-VBO. All five sections are
            // contiguous: POS | UV | NORM | TAN | WT.
            const U64 sec_pos  = 0;
            const U64 sec_uv   = (U64)mVertCap * POS_STRIDE;
            const U64 sec_norm = sec_uv  + (U64)mVertCap * UV_STRIDE;
            const U64 sec_tan  = sec_norm + (U64)mVertCap * NORM_STRIDE;
            const U64 sec_wt   = sec_tan + (U64)mVertCap * TAN_STRIDE;

            slot.pos_offset_bytes  = (U32)(sec_pos  + (U64)vbase * POS_STRIDE);
            slot.uv_offset_bytes   = (U32)(sec_uv   + (U64)vbase * UV_STRIDE);
            slot.norm_offset_bytes = (U32)(sec_norm + (U64)vbase * NORM_STRIDE);
            slot.tan_offset_bytes  = (U32)(sec_tan  + (U64)vbase * TAN_STRIDE);
            slot.wt_offset_bytes   = (U32)(sec_wt   + (U64)vbase * WT_STRIDE);
            slot.ibase_bytes      = ibase * IDX_STRIDE;
            slot.num_verts        = nv;
            slot.num_indices      = ni;
            slot.valid            = true;
            slot.has_weights      = pf.has_weights;
            slot.has_tangents     = pf.has_tangents;
            // Regular volume mesh: eligible for garbageCollect reclaim. Stamp
            // "referenced now" so it isn't reclaimed before an instance has had
            // a chance to point at it.
            slot.reclaimable      = true;
            slot.last_ref_frame   = (U32)gFrameCount;
            slot.captured_frame   = (U32)gFrameCount;
            slot.sculpt_level     = prep.sculpt_level;
            for (int c = 0; c < 3; ++c) { slot.cap_lo[c] = pf.lo[c]; slot.cap_hi[c] = pf.hi[c]; }
            slot.uv_density       = pf.uv_density;

            // Allocate a GPU slot_id. mSlotInfo is the contiguous table indexed
            // by slot_id. Reuse a slot_id recycled by garbageCollect when
            // available (keeps the table bounded), else push a new entry.
            SlotInfo si{};
            si.first_index = slot.ibase_bytes / IDX_STRIDE;
            si.index_count = slot.num_indices;
            si.base_vertex = (S32)(slot.pos_offset_bytes / POS_STRIDE);
            si.has_weights = slot.has_weights ? 1u : 0u;
            si.vert_count  = slot.num_verts;
            packSlotBounds(si, slot.cap_lo, slot.cap_hi);
            // STABLE SLOT ID (#29): if this (mesh_id,lod,face) is being
            // RE-captured (invalidateAsset kept the entry + slot_id when the
            // sculpt decode improved), REUSE that slot_id and rewrite its
            // SlotInfo in place. Every live instance of this shared asset already
            // points at it, so they ALL recover with the improved geometry
            // instead of being stranded on a recycled id (the #29 evaporation).
            // ATOMIC RE-CAPTURE (#29): remember the OLD storage ranges of a slot we
            // are overwriting so we can free them AFTER the SlotInfo swap below --
            // never before. This replaces invalidateAsset's free-then-refill, whose
            // zeroed SlotInfo stranded live instances at index_count==0 when the
            // refill skipped a still-degenerate face (emitted_empty stuck forever).
            U32 old_pos_first = 0, old_pos_cnt = 0, old_idx_first = 0, old_idx_cnt = 0;
            auto exist_it = mSlots.find(Key{mesh_id, lod, fi});
            if (exist_it != mSlots.end() && exist_it->second.slot_id != 0xFFFFFFFFu)
            {
                slot.slot_id = exist_it->second.slot_id;
                const Slot& os = exist_it->second;
                if (os.num_verts)   { old_pos_first = os.pos_offset_bytes / POS_STRIDE; old_pos_cnt = os.num_verts; }
                if (os.num_indices) { old_idx_first = os.ibase_bytes      / IDX_STRIDE; old_idx_cnt = os.num_indices; }
            }
            else
            {
                slot.slot_id = allocSlotIdLocked();   // recycled (post-quarantine) or new
            }
            mSlotInfo[slot.slot_id] = si;
            if (slot.slot_id < mSlotInfoDirtyMin) mSlotInfoDirtyMin = slot.slot_id;
            if (slot.slot_id + 1 > mSlotInfoDirtyMax) mSlotInfoDirtyMax = slot.slot_id + 1;
            noteBestSlotChange();   // (the cluster LOD best-slot cache)

            slotPut(Key{mesh_id, lod, fi}, slot);
            mCaptureFail.erase(Key{mesh_id, lod, fi});

            // SlotInfo now points at the NEW range for every instance on this slot_id;
            // the old range has no readers -> free it (returns storage to the pool,
            // does not touch GPU data). No zeroed-slot window occurred.
            if (old_pos_cnt) freeVerts(old_pos_first, old_pos_cnt);
            if (old_idx_cnt) freeIdx  (old_idx_first, old_idx_cnt);
            ++created;
        }

        // #29 provenance: real position data written to this vbase THIS frame. A
        // later FREE_VERT/ALLOC_VERT at the same aux (=vbase) recycled it away.
        CleanProv::record(CleanProv::CAPTURE, slot.slot_id, 0xFFFFFFFFu,
                          slot.pos_offset_bytes / POS_STRIDE, mesh_id);
        // The VK world renderer sources this face from the interleaved CPU mirror published here
        // (prepared by prepareCapture). Keyed by slot_id so a re-capture overwrites
        // the same entry in place.
        std::lock_guard<std::mutex> lk(mMutex);
        if (slot.slot_id != 0xFFFFFFFFu)
        {
            mGeomGeneration.fetch_add(1, std::memory_order_relaxed); // mirror mutated
            mMirrorDirty.push_back(slot.slot_id);
            mirrorRelForgetLocked(slot.slot_id, true);   // (a freed cluster mirror is back)
            if (slot.slot_id >= mMirrorVerts.size())
            {
                mMirrorVerts.resize(slot.slot_id + 1);
                mMirrorIdx.resize(slot.slot_id + 1);
                mMirrorWeights.resize(slot.slot_id + 1);
            }
            mMirrorVerts[slot.slot_id] = std::move(pf.verts);
            mMirrorIdx[slot.slot_id]   = std::move(pf.idx);
            if (pf.has_weights) mMirrorWeights[slot.slot_id] = std::move(pf.wts);
            else                mMirrorWeights[slot.slot_id].clear();
        }
    }

    // Nothing ingested -> the volume's faces weren't ready. Unlatch so the next
    // registerFace (fired when the geometry finishes decoding -> drawable rebuild)
    // retries the capture instead of treating this asset as permanently captured.
    if (created == 0 && present == 0)
    {
        std::lock_guard<std::mutex> lk(mMutex);
        mCapturedAssets.erase(std::make_pair(mesh_id, lod));
        noteUnlatchLocked(mesh_id, lod, UNL_NOTHING_CAPTURED);
    }
    else if (notready > 0)
    {
        // Some faces landed (or were already resident) but at least one real face was
        // skipped as not-ready / refused: stay UNLATCHED so the next visit's decode +
        // capture retries exactly the missing faces (see the partial-latch note above).
        std::lock_guard<std::mutex> lk(mMutex);
        mCapturedAssets.erase(std::make_pair(mesh_id, lod));
        noteUnlatchLocked(mesh_id, lod, UNL_PARTIAL_RETRY);
        static U64 s_last = 0; static U32 s_n = 0;
        ++s_n;
        const U64 now = LLTimer::getTotalTime();
        if (now - s_last > 5ull * 1000000ull)
        {
            LL_INFOS("VKIngest") << "capture: partial asset kept unlatched for retry (" << s_n
                << " in last period) e.g. mesh=" << mesh_id << " lod=" << lod << " captured=" << created
                << " present=" << present << " not_ready/refused=" << notready << " gave_up=" << gaveup << LL_ENDL;
            s_last = now; s_n = 0;
        }
    }
    if (created > 0)
    {
        // #29 strand detector: streaming is actively healing while captures land.
        mLastCaptureFrame = (U32)gFrameCount;
    }
    if ((created > 0 || present > 0) && notready == 0)
        demoteRawMesh(mesh_id, lod);   // latched: its raw bytes are evicted first
}

// ---- ASYNC CAPTURE (RenderVKIngestWorker) ----
bool LLCleanMeshAsset::captureInFlight(const LLUUID& mesh_id, S32 lod, const CaptureWaiter* w)
{
    auto it = mCapJobs.find(std::make_pair(mesh_id, lod));
    if (it == mCapJobs.end()) return false;
    if (w && w->obj.notNull())
    {
        std::vector<CaptureWaiter>& ws = it->second->waiters;
        const bool known = std::any_of(ws.begin(), ws.end(), [w](const CaptureWaiter& x)
                                       { return x.obj == w->obj && x.face_idx == w->face_idx && x.site == w->site; });
        if (!known && ws.size() < 256) ws.push_back(*w);
    }
    return true;
}

bool LLCleanMeshAsset::postCapture(const LLUUID& mesh_id, S32 lod, const LLPointer<LLVolume>& src,
                                   const LLVolumeParams& params, F32 detail, const CaptureWaiter& w)
{
    if (!mReady || mDecodesStopped) return false;
    if (captureInFlight(mesh_id, lod, &w)) return true;
    if (!mCapPool)
    {
        // Render thread only (postCapture's callers are registerCleanFace).
        mCapPool.reset(new LL::ThreadPool("CleanCapturePrep", 2));
        mCapPool->start();
    }
    std::unique_ptr<CaptureJob> job(new CaptureJob());
    job->mesh_id = mesh_id;
    job->lod = lod;
    job->src = src;            // render thread: the reference is taken here...
    job->params = params;
    job->detail = detail;
    if (w.obj.notNull()) job->waiters.push_back(w);
    CaptureJob* raw = job.get();
    const auto key = std::make_pair(mesh_id, lod);
    mCapJobs.emplace(key, std::move(job));
    if (!mCapPool->getQueue().post([this, raw]() { runCaptureJob(raw); }))
    {
        mCapJobs.erase(key);   // pool closed: ...and released here (render thread)
        return false;
    }
    LLVKHitch::count("capturePosted");
    return true;
}

// Worker. Inputs (src pointer, params, detail) were written before the post and are not
// touched by the render thread until the job is handed back; the source volume is read
// only (no ref / unref here -- LLRefCount is not atomic). A prim's volume is created,
// tessellated and destroyed on this thread, visible to no one else.
void LLCleanMeshAsset::runCaptureJob(CaptureJob* job)
{
    LL_PROFILE_ZONE_NAMED("clean:asyncCapturePrep");
    if (const LLVolume* src = job->src.get())
    {
        prepareCapture(src, job->prep);
    }
    else
    {
        // Prim: tessellate from the shape params + tangents (vkTessellatePrim's
        // non-flexi path), exactly as the render thread did inline.
        LLVolume* vol = new LLVolume(job->params, job->detail);
        vol->ref();
        const S32 nf = vol->getNumVolumeFaces();
        for (S32 f = 0; f < nf; ++f) vol->genTangents(f);
        prepareCapture(vol, job->prep);
        vol->unref();
    }
    std::lock_guard<std::mutex> lk(mCapMutex);
    mCapDone.push_back(job);   // last access: the render thread owns the job from here
}

void LLCleanMeshAsset::pumpCaptures(std::vector<CaptureDone>& out)
{
    std::vector<CaptureJob*> done;
    {
        std::lock_guard<std::mutex> lk(mCapMutex);
        done.swap(mCapDone);
    }
    for (CaptureJob* j : done)
    {
        {
            CaptureScope _cap(0);
            commitCapture(j->mesh_id, j->lod, j->prep, /*force=*/false);
        }
        const auto key = std::make_pair(j->mesh_id, j->lod);
        CaptureDone d;
        d.mesh_id = j->mesh_id;
        d.lod = j->lod;
        d.waiters.swap(j->waiters);
        out.push_back(std::move(d));
        mCapJobs.erase(key);   // frees the job + its volume ref here (render thread)
    }
    if (!done.empty()) LLVKHitch::count("captureCommitted", (unsigned)done.size());
}

void LLCleanMeshAsset::noteUnlatchLocked(const LLUUID& mesh_id, S32 lod, U8 site)
{
    if (mUnlatch.size() > 65536) mUnlatch.clear();   // bounded diagnostics
    UnlatchRec& r = mUnlatch[std::make_pair(mesh_id, lod)];
    r.site = site; r.frame = (U32)gFrameCount; ++r.count;
}

void LLCleanMeshAsset::noteCaptureFailLocked(const Key& k, U8 reason, U32 nv, U32 ni)
{
    if (mCaptureFail.size() > 65536) mCaptureFail.clear();   // bounded diagnostics
    CaptureFailRec& r = mCaptureFail[k];
    if (r.reason != reason) r.count = 0;
    r.reason = reason; r.frame = (U32)gFrameCount; ++r.count; r.nv = nv; r.ni = ni;
}

const char* LLCleanMeshAsset::captureFailName(U8 r)
{
    static const char* k[] = { "none", "empty_face", "degenerate_indices", "index_out_of_range",
                               "vert_alloc_refused", "index_alloc_refused" };
    return r < CAPF_COUNT ? k[r] : "?";
}

const char* LLCleanMeshAsset::unlatchSiteName(U8 s)
{
    static const char* k[] = { "none", "nothing_captured", "partial_retry", "gc", "invalidate",
                               "heal", "source_mutation" };
    return s < UNL_COUNT ? k[s] : "?";
}

const char* LLCleanMeshAsset::decodeStateName(S32 s)
{
    switch (s)
    {
    case DECODE_NO_BYTES: return "no_bytes";
    case DECODE_PENDING:  return "pending";
    case DECODE_READY:    return "ready";
    case DECODE_FAILED:   return "failed";
    default:              return "none";
    }
}

bool LLCleanMeshAsset::isLatched(const LLUUID& mesh_id, S32 lod) const
{
    std::lock_guard<std::mutex> lk(mMutex);
    auto it = mCapturedAssets.find(std::make_pair(mesh_id, lod));
    return it != mCapturedAssets.end() && it->second;
}

void LLCleanMeshAsset::diagQuery(const LLUUID& mesh_id, S32 lod, U8 sculpt_type, S32 face_idx,
                                 IngestDiag& out)
{
    out = IngestDiag{};
    {
        std::lock_guard<std::mutex> lk(mMutex);
        auto it = mCapturedAssets.find(std::make_pair(mesh_id, lod));
        out.latch_known = it != mCapturedAssets.end();
        out.latched = out.latch_known && it->second;
        auto f = mCaptureFail.find(Key{mesh_id, lod, face_idx});
        if (f != mCaptureFail.end())
        {
            out.fail_reason = f->second.reason; out.fail_frame = f->second.frame;
            out.fail_count = f->second.count; out.fail_nv = f->second.nv; out.fail_ni = f->second.ni;
        }
        auto u = mUnlatch.find(std::make_pair(mesh_id, lod));
        if (u != mUnlatch.end())
        {
            out.unlatch_site = u->second.site; out.unlatch_frame = u->second.frame;
            out.unlatch_count = u->second.count;
        }
    }
    {
        std::lock_guard<std::mutex> dl(mDecodeMutex);
        const DecodeKey key(mesh_id, lod, sculpt_type);
        auto it = mDecodes.find(key);
        if (it != mDecodes.end())
        {
            out.decode_state = (S32)it->second.state;
            out.decode_started = it->second.started;
            out.decode_waiters = (U32)it->second.waiters.size();
        }
        auto h = mDecodeHist.find(key);
        if (h != mDecodeHist.end())
        {
            out.hist_decodes = h->second.decodes;
            out.hist_nfaces = h->second.nfaces;
            out.hist_last_end = h->second.last_end;
        }
    }
    out.raw = hasRawMesh(mesh_id, lod);
}

void LLCleanMeshAsset::captureTerrain(const LLUUID& mesh_id, S32 face_idx,
                                      const LLVector3*  positions,
                                      const LLVector3*  normals,
                                      const LLVector2*  texcoords,
                                      const LLVector4a* tangents,
                                      const U16*        indices,
                                      U32 nv, U32 ni, bool force, bool tight, bool reclaimable)
{
    // Skip degenerate terrain patches (< 1 triangle); same nv<3 not-ready guard
    // as capture(). Early-return BEFORE freeing the existing slot, so a good prior
    // capture is preserved and the next terrain rebuild retries with real data.
    if (!mReady || !positions || !indices || nv < 3 || ni < 3) return;
    CaptureScope _cap((U64)nv * VERT_STRIDE_TOTAL + (U64)ni * IDX_STRIDE);
    constexpr S32 LOD = 0;
    const Key k{mesh_id, LOD, face_idx};

    Slot slot;
    {
        std::lock_guard<std::mutex> lk(mMutex);

        // STABLE-RANGE IN-PLACE UPDATE (#29 churn kill). Terrain/water rebuild
        // every frame; the old free-then-realloc cycled the same storage through
        // different owners ~150x/frame -- the racing surface for the stomp class
        // (provenance: 3 owners uploading overlapping ranges within 4 frames).
        // Keep the ranges STABLE: reuse in place while the new data fits the
        // allocated capacity; reallocate (with 1.5x headroom) only on growth.
        U32 reuse_slot_id = 0xFFFFFFFFu;
        U32 vbase = (U32)~0u, ibase = (U32)~0u;
        U32 vcap = nv, icap = ni;
        auto it = mSlots.find(k);
        if (it != mSlots.end())
        {
            // CAPTURE-ONCE CONVERGENCE (terrain/water off the CPU):
            // (a) NEVER DOWNGRADE: a lower-resolution rebuild of an already-
            //     captured patch is ignored outright. Patches converge to their
            //     max resolution on first close approach and then never
            //     re-capture -- distant terrain simply renders at full detail
            //     (strictly better; the GPU does not care). Capture traffic for
            //     settled terrain drops to ZERO.
            // (b) IDENTICAL-REBUILD SKIP: same counts + same bounds = the same
            //     data (terrain regen is deterministic per LOD; water re-emits
            //     its plane) -- skip the upload entirely. Known lag: a terraform
            //     edit that keeps counts+bounds identical waits for the next LOD
            //     transition to appear; acceptable until the Phase-B tessellation
            //     pipeline owns terrain geometry outright.
            if (!force && nv < it->second.num_verts)
            {
                return;   // (a) never downgrade
            }
            if (!force && nv == it->second.num_verts && ni == it->second.num_indices)
            {
                F32 lo[3] = {1e30f,1e30f,1e30f}, hi[3] = {-1e30f,-1e30f,-1e30f};
                for (U32 v = 0; v < nv; ++v)
                    for (int c = 0; c < 3; ++c)
                    {
                        const F32 x = positions[v].mV[c];
                        if (x < lo[c]) lo[c] = x;
                        if (x > hi[c]) hi[c] = x;
                    }
                const Slot& os = it->second;
                bool same = true;
                for (int c = 0; c < 3; ++c)
                    if (fabsf(lo[c] - os.cap_lo[c]) > 1e-3f ||
                        fabsf(hi[c] - os.cap_hi[c]) > 1e-3f) same = false;
                if (same) return;   // (b) identical rebuild, zero uploads
            }
            const Slot& old = it->second;
            reuse_slot_id = old.slot_id;
            if (nv <= old.vert_cap && ni <= old.idx_cap)
            {
                // fits: same ranges, in-place upload, no allocator traffic
                vbase = old.pos_offset_bytes / POS_STRIDE;
                ibase = old.ibase_bytes      / IDX_STRIDE;
                vcap  = old.vert_cap;
                icap  = old.idx_cap;
            }
            else
            {
                freeVerts(old.pos_offset_bytes / POS_STRIDE, old.vert_cap ? old.vert_cap : old.num_verts);
                freeIdx  (old.ibase_bytes      / IDX_STRIDE, old.idx_cap  ? old.idx_cap  : old.num_indices);
            }
            mSlotIdx.erase(it->first);
            mSlots.erase(it);
        }

        if (vbase == (U32)~0u)
        {
            vcap = tight ? nv : nv + nv / 2u;   // headroom so LOD growth rarely reallocates
            icap = tight ? ni : ni + ni / 2u;
            vbase = allocVerts(vcap);
            ibase = allocIdx(icap);
        }

        const U64 sec_pos  = 0;
        const U64 sec_uv   = (U64)mVertCap * POS_STRIDE;
        const U64 sec_norm = sec_uv  + (U64)mVertCap * UV_STRIDE;
        const U64 sec_tan  = sec_norm + (U64)mVertCap * NORM_STRIDE;
        const U64 sec_wt   = sec_tan + (U64)mVertCap * TAN_STRIDE;
        slot.pos_offset_bytes  = (U32)(sec_pos  + (U64)vbase * POS_STRIDE);
        slot.uv_offset_bytes   = (U32)(sec_uv   + (U64)vbase * UV_STRIDE);
        slot.norm_offset_bytes = (U32)(sec_norm + (U64)vbase * NORM_STRIDE);
        slot.tan_offset_bytes  = (U32)(sec_tan  + (U64)vbase * TAN_STRIDE);
        slot.wt_offset_bytes   = (U32)(sec_wt   + (U64)vbase * WT_STRIDE);
        slot.ibase_bytes       = ibase * IDX_STRIDE;
        slot.num_verts         = nv;
        slot.num_indices       = ni;
        slot.vert_cap          = vcap;
        slot.idx_cap           = icap;
        slot.valid             = true;
        slot.has_weights       = false;
        slot.has_tangents      = (tangents != nullptr);
        slot.reclaimable       = reclaimable;
        slot.last_ref_frame    = (U32)gFrameCount;
        slot.captured_frame    = (U32)gFrameCount;
        {
            F32 lo[3] = {1e30f,1e30f,1e30f}, hi[3] = {-1e30f,-1e30f,-1e30f};
            for (U32 v = 0; v < nv; ++v)
                for (int c = 0; c < 3; ++c)
                { const F32 x = positions[v].mV[c]; if (x < lo[c]) lo[c] = x; if (x > hi[c]) hi[c] = x; }
            for (int c = 0; c < 3; ++c) { slot.cap_lo[c] = lo[c]; slot.cap_hi[c] = hi[c]; }
        }

        // Reuse the prior slot_id (update SlotInfo in place) when this terrain
        // patch was already captured; only grow the table for genuinely new
        // ones. Keeps the SlotInfo index stable so the terrain instance keeps
        // pointing at live data.
        if (reuse_slot_id != 0xFFFFFFFFu)
        {
            slot.slot_id = reuse_slot_id;
        }
        else
        {
            // Recycled ids too: smooth-terrain levels are freed and rebuilt as the
            // camera moves, and a push-only table grew by one id per rebuild.
            slot.slot_id = allocSlotIdLocked();
        }
        SlotInfo& si = mSlotInfo[slot.slot_id];
        si.first_index = slot.ibase_bytes / IDX_STRIDE;
        si.index_count = slot.num_indices;
        si.base_vertex = (S32)(slot.pos_offset_bytes / POS_STRIDE);
        si.has_weights = 0u;
        si.vert_count  = slot.num_verts;
        packSlotBounds(si, slot.cap_lo, slot.cap_hi);
        if (slot.slot_id < mSlotInfoDirtyMin) mSlotInfoDirtyMin = slot.slot_id;
        if (slot.slot_id + 1 > mSlotInfoDirtyMax) mSlotInfoDirtyMax = slot.slot_id + 1;
        noteBestSlotChange();   // (the cluster LOD best-slot cache)

        slotPut(k, slot);
    }

    static U32 s_count = 0;
    if ((++s_count & 0x3F) == 1u)
    {
        LL_INFOS_ONCE("CleanMeshAsset") << "captureTerrain #1 mesh=" << mesh_id
            << " face=" << face_idx << " nv=" << nv << " ni=" << ni
            << " slot=" << slot.slot_id << LL_ENDL;
    }

    // Pad vec3 positions and normals to LLVector4a (vec4 with .w=0).
    // Cheap: nv is a few thousand per patch face at most.
    std::vector<LLVector4a> pos4(nv);
    for (U32 i = 0; i < nv; ++i)
    {
        pos4[i].set(positions[i].mV[VX], positions[i].mV[VY],
                    positions[i].mV[VZ], 0.f);
    }
    std::vector<LLVector4a> nrm4;
    if (normals)
    {
        nrm4.resize(nv);
        for (U32 i = 0; i < nv; ++i)
        {
            nrm4[i].set(normals[i].mV[VX], normals[i].mV[VY],
                        normals[i].mV[VZ], 0.f);
        }
    }


    {
        // VK CPU MIRROR (mirrors capture()'s VK block): fillFromCleanStores reads
        // mMirrorVerts/mMirrorIdx per slot to pack its VkBuffers. Terrain has no weights.
        std::lock_guard<std::mutex> lk(mMutex);
        if (slot.slot_id != 0xFFFFFFFFu)
        {
            mGeomGeneration.fetch_add(1, std::memory_order_relaxed); // mirror mutated
                mMirrorDirty.push_back(slot.slot_id);
                mirrorRelForgetLocked(slot.slot_id, true);   // (a freed cluster mirror is back)
            if (slot.slot_id >= mMirrorVerts.size())
            {
                mMirrorVerts.resize(slot.slot_id + 1);
                mMirrorIdx.resize(slot.slot_id + 1);
                mMirrorWeights.resize(slot.slot_id + 1);
            }
            std::vector<MirrorVertex> mv(nv, MirrorVertex{});   // decoded form, quantized below
            for (U32 v = 0; v < nv; ++v)
            {
                mv[v].pos[0] = positions[v].mV[VX];
                mv[v].pos[1] = positions[v].mV[VY];
                mv[v].pos[2] = positions[v].mV[VZ];
                if (texcoords) { mv[v].uv[0] = texcoords[v].mV[0]; mv[v].uv[1] = texcoords[v].mV[1]; }
                if (normals)
                {
                    mv[v].normal[0] = normals[v].mV[VX];
                    mv[v].normal[1] = normals[v].mV[VY];
                    mv[v].normal[2] = normals[v].mV[VZ];
                }
                else { mv[v].normal[0] = 0.f; mv[v].normal[1] = 0.f; mv[v].normal[2] = 1.f; }
                if (tangents)
                {
                    const F32* t = tangents[v].getF32ptr();
                    mv[v].tangent[0] = t[0]; mv[v].tangent[1] = t[1];
                    mv[v].tangent[2] = t[2]; mv[v].tangent[3] = (t[3] < 0.f) ? -1.f : 1.f;
                }
            }
            quantizeVerts(mv.data(), nv, mMirrorVerts[slot.slot_id]);
            mMirrorIdx[slot.slot_id].assign(indices, indices + ni);   // U16, face-local
            mMirrorWeights[slot.slot_id].clear();
        }
    }

#if CLEAN_DEBUG
    // #29 source-mutation tripwire (terrain/water flavor -- the capture() one
    // caught the mesh path; terrain/water sources are LL arrays that can also
    // mutate under us mid-capture).
    {
        F32 lo2[3] = {1e30f,1e30f,1e30f}, hi2[3] = {-1e30f,-1e30f,-1e30f};
        for (U32 v = 0; v < nv; ++v)
            for (int c = 0; c < 3; ++c)
            {
                const F32 x = positions[v].mV[c];
                if (x < lo2[c]) lo2[c] = x;
                if (x > hi2[c]) hi2[c] = x;
            }
        bool mutated = false;
        for (int c = 0; c < 3; ++c)
            if (fabsf(lo2[c] - slot.cap_lo[c]) > 1e-3f ||
                fabsf(hi2[c] - slot.cap_hi[c]) > 1e-3f) mutated = true;
        if (mutated)
        {
            CleanAlert::fire("sourcemutation",
                "TERRAIN/WATER SOURCE MUTATED MID-CAPTURE: id=" + mesh_id.asString()
                + llformat(" face=%d nv=%u", face_idx, nv));
        }
    }
#endif
}

void LLCleanMeshAsset::captureWater(const LLUUID& vo_id, S32 face_idx,
                                    const LLVector3* positions,
                                    const LLVector3* normals,
                                    const LLVector2* texcoords,
                                    const U16*       indices,
                                    U32 num_verts, U32 num_indices, bool reclaimable)
{
    // Delegate -- water uses the same tight-vec3 storage shape as
    // terrain (positions/normals = vec3, uv = vec2, indices = U16).
    // No tangents (the water FS reconstructs the basis from the
    // wave-normal cross product).
    captureTerrain(vo_id, face_idx, positions, normals, texcoords,
                   nullptr, indices, num_verts, num_indices, false, false, reclaimable);
}

bool LLCleanMeshAsset::captureGLTFPrimitive(const LLUUID& mesh_id, S32 lod, S32 face_idx,
                                            const LLVector4a* positions, const LLVector4a* normals,
                                            const LLVector4a* tangents, const LLVector2* texcoords,
                                            const U32* indices, U32 nv, U32 ni, bool force)
{
    if (!mReady || lod < 0 || lod >= NUM_LOD) return false;
    const Key k{mesh_id, lod, face_idx};
    {
        std::lock_guard<std::mutex> lk(mMutex);
        const Slot* have = slotFind(k);
        if (!force && have && have->valid && have->slot_id != 0xFFFFFFFFu) return true;
    }
    // Same not-ready / bad-data guards as capture(): a triangle needs 3 verts + 3
    // indices, and an index past the vertex array would read the NEXT slot's vertices
    // in the compact VK packing (geometry fanning to a wrong point).
    if (!positions || !indices || nv < 3 || ni < 3) return false;
    U32 idx_max = 0;
    for (U32 i = 0; i < ni; ++i) idx_max = llmax(idx_max, indices[i]);
    if (idx_max >= nv)
    {
        LL_WARNS_ONCE("CleanMeshAsset") << "captureGLTFPrimitive: index past vertex array mesh=" << mesh_id
            << " face=" << face_idx << " nv=" << nv << " idx_max=" << idx_max << LL_ENDL;
        return false;
    }
    // 16-bit face-local indices (#573): vertices past the largest index are never drawn
    // (dropped); a primitive that really indexes > 65536 vertices is refused (SL's glTF
    // upload splits primitives at the 16-bit limit, so this is malformed content).
    if (idx_max >= LLCleanQV::kMaxSlotVerts)
    {
        LL_WARNS_ONCE("CleanMeshAsset") << "captureGLTFPrimitive: primitive indexes " << (idx_max + 1)
            << " vertices (> 65536, the 16-bit index limit) mesh=" << mesh_id << " face=" << face_idx
            << " -- not captured" << LL_ENDL;
        return false;
    }
    nv = std::min<U32>(nv, idx_max + 1);
    CaptureScope _cap((U64)nv * VERT_STRIDE_TOTAL + (U64)ni * IDX_STRIDE);
    const U32 uv_density = texcoords
        ? computeUVDensity(positions[0].getF32ptr(), 4, texcoords[0].mV, 2, indices, false, ni, nv) : 0u;

    Slot slot;
    {
        std::lock_guard<std::mutex> lk(mMutex);
        const U32 vbase = allocVerts(nv);
        const U32 ibase = allocIdx(ni);
        const U64 sec_uv   = (U64)mVertCap * POS_STRIDE;
        const U64 sec_norm = sec_uv  + (U64)mVertCap * UV_STRIDE;
        const U64 sec_tan  = sec_norm + (U64)mVertCap * NORM_STRIDE;
        const U64 sec_wt   = sec_tan + (U64)mVertCap * TAN_STRIDE;
        slot.pos_offset_bytes  = (U32)((U64)vbase * POS_STRIDE);
        slot.uv_offset_bytes   = (U32)(sec_uv   + (U64)vbase * UV_STRIDE);
        slot.norm_offset_bytes = (U32)(sec_norm + (U64)vbase * NORM_STRIDE);
        slot.tan_offset_bytes  = (U32)(sec_tan  + (U64)vbase * TAN_STRIDE);
        slot.wt_offset_bytes   = (U32)(sec_wt   + (U64)vbase * WT_STRIDE);
        slot.ibase_bytes       = ibase * IDX_STRIDE;
        slot.num_verts         = nv;
        slot.num_indices       = ni;
        slot.valid             = true;
        slot.has_weights       = false;   // skinned glTF renders static (see llcleangltfingest.cpp)
        slot.uv_density        = uv_density;
        slot.has_tangents      = (tangents != nullptr);
        // Reclaimable like mesh geometry: garbageCollect frees it once no live instance's
        // LOD group has named it for keep_age frames. Stamped "referenced now".
        slot.reclaimable       = true;
        slot.last_ref_frame    = (U32)gFrameCount;
        slot.captured_frame    = (U32)gFrameCount;
        {
            F32 lo[3] = {1e30f,1e30f,1e30f}, hi[3] = {-1e30f,-1e30f,-1e30f};
            for (U32 v = 0; v < nv; ++v)
            {
                const F32* p = positions[v].getF32ptr();
                for (int c = 0; c < 3; ++c) { lo[c] = llmin(lo[c], p[c]); hi[c] = llmax(hi[c], p[c]); }
            }
            for (int c = 0; c < 3; ++c) { slot.cap_lo[c] = lo[c]; slot.cap_hi[c] = hi[c]; }
        }
        SlotInfo si{};
        si.first_index = slot.ibase_bytes / IDX_STRIDE;
        si.index_count = slot.num_indices;
        si.base_vertex = (S32)(slot.pos_offset_bytes / POS_STRIDE);
        si.has_weights = 0u;
        si.vert_count  = slot.num_verts;
        packSlotBounds(si, slot.cap_lo, slot.cap_hi);
        // Re-capture (force) reuses the slot id and swaps the SlotInfo in place, then
        // frees the old ranges (capture()'s atomic re-capture: no zeroed-slot window).
        U32 old_pos_first = 0, old_pos_cnt = 0, old_idx_first = 0, old_idx_cnt = 0;
        auto exist_it = mSlots.find(k);
        if (exist_it != mSlots.end() && exist_it->second.slot_id != 0xFFFFFFFFu)
        {
            slot.slot_id = exist_it->second.slot_id;
            const Slot& os = exist_it->second;
            if (os.num_verts)   { old_pos_first = os.pos_offset_bytes / POS_STRIDE; old_pos_cnt = os.num_verts; }
            if (os.num_indices) { old_idx_first = os.ibase_bytes      / IDX_STRIDE; old_idx_cnt = os.num_indices; }
        }
        else
        {
            slot.slot_id = allocSlotIdLocked();
        }
        mSlotInfo[slot.slot_id] = si;
        if (slot.slot_id < mSlotInfoDirtyMin) mSlotInfoDirtyMin = slot.slot_id;
        if (slot.slot_id + 1 > mSlotInfoDirtyMax) mSlotInfoDirtyMax = slot.slot_id + 1;
        noteBestSlotChange();   // (the cluster LOD best-slot cache)
        slotPut(k, slot);
        mCaptureFail.erase(k);
        mCapturedAssets[std::make_pair(mesh_id, lod)] = true;
        if (old_pos_cnt) freeVerts(old_pos_first, old_pos_cnt);
        if (old_idx_cnt) freeIdx  (old_idx_first, old_idx_cnt);
        mLastCaptureFrame = (U32)gFrameCount;

        // VK CPU MIRROR (the only geometry copy the VK packer reads).
        mGeomGeneration.fetch_add(1, std::memory_order_relaxed);
        mMirrorDirty.push_back(slot.slot_id);
        mirrorRelForgetLocked(slot.slot_id, true);   // (a freed cluster mirror is back)
        if (slot.slot_id >= mMirrorVerts.size())
        {
            mMirrorVerts.resize(slot.slot_id + 1);
            mMirrorIdx.resize(slot.slot_id + 1);
            mMirrorWeights.resize(slot.slot_id + 1);
        }
        std::vector<MirrorVertex> mv(nv, MirrorVertex{});   // decoded form, quantized below
        for (U32 v = 0; v < nv; ++v)
        {
            const F32* p = positions[v].getF32ptr();
            mv[v].pos[0] = p[0]; mv[v].pos[1] = p[1]; mv[v].pos[2] = p[2];
            if (texcoords) { mv[v].uv[0] = texcoords[v].mV[0]; mv[v].uv[1] = texcoords[v].mV[1]; }
            if (normals)
            {
                const F32* n = normals[v].getF32ptr();
                mv[v].normal[0] = n[0]; mv[v].normal[1] = n[1]; mv[v].normal[2] = n[2];
            }
            else { mv[v].normal[2] = 1.f; }
            if (tangents)
            {
                const F32* t = tangents[v].getF32ptr();
                mv[v].tangent[0] = t[0]; mv[v].tangent[1] = t[1];
                mv[v].tangent[2] = t[2]; mv[v].tangent[3] = (t[3] < 0.f) ? -1.f : 1.f;
            }
        }
        quantizeVerts(mv.data(), nv, mMirrorVerts[slot.slot_id]);
        mMirrorIdx[slot.slot_id].assign(indices, indices + ni);   // U32 -> U16 (< 65536: checked above)
        mMirrorWeights[slot.slot_id].clear();
    }
    CleanProv::record(CleanProv::CAPTURE, slot.slot_id, 0xFFFFFFFFu,
                      slot.pos_offset_bytes / POS_STRIDE, mesh_id);
    return true;
}

U32 LLCleanMeshAsset::captureRawSlot(const LLUUID& mesh_id, S32 lod, S32 face_idx, const MirrorVertex* verts,
                                     const MirrorWeight* weights, const U32* indices, U32 nv, U32 ni,
                                     bool reclaimable, const QHeader* domain)
{
    if (!mReady || lod < 0 || lod >= NUM_LOD || !verts || !indices || nv == 0 || ni < 3) return 0xFFFFFFFFu;
    if (nv > LLCleanQV::kMaxSlotVerts)
    {
        // 16-bit face-local indices (#573). Callers (system avatar parts, decimated steps
        // of an SL face) stay far below; refuse rather than wrap.
        LL_WARNS_ONCE("CleanMeshAsset") << "captureRawSlot: " << nv << " vertices > 65536 (16-bit index limit) mesh="
            << mesh_id << " -- not captured" << LL_ENDL;
        return 0xFFFFFFFFu;
    }
    U32 idx_max = 0;
    for (U32 i = 0; i < ni; ++i) idx_max = llmax(idx_max, indices[i]);
    if (idx_max >= nv)
    {
        LL_WARNS_ONCE("CleanMeshAsset") << "captureRawSlot: index past vertex array mesh=" << mesh_id
            << " nv=" << nv << " idx_max=" << idx_max << LL_ENDL;
        return 0xFFFFFFFFu;
    }
    const Key k{mesh_id, lod, face_idx};
    CaptureScope _cap((U64)nv * VERT_STRIDE_TOTAL + (U64)ni * IDX_STRIDE);
    constexpr U32 kMVStride = (U32)(sizeof(MirrorVertex) / sizeof(F32));
    const U32 uv_density = computeUVDensity(verts[0].pos, kMVStride, verts[0].uv, kMVStride, indices, false, ni, nv);
    // #574 rigged welding: this slot's own open-boundary flags (a decimated step's
    // records arrive with its source's flags; they are recomputed on its triangles).
    std::vector<MirrorWeight> wts;
    if (weights)
    {
        wts.assign(weights, weights + nv);
        LLCleanQV::markWeldBoundary(wts.data(), nv, indices, ni, verts[0].pos, kMVStride);
    }
    std::lock_guard<std::mutex> lk(mMutex);
    Slot slot;
    const U32 vbase = allocVerts(nv);
    const U32 ibase = allocIdx(ni);
    const U64 sec_uv   = (U64)mVertCap * POS_STRIDE;
    const U64 sec_norm = sec_uv  + (U64)mVertCap * UV_STRIDE;
    const U64 sec_tan  = sec_norm + (U64)mVertCap * NORM_STRIDE;
    const U64 sec_wt   = sec_tan + (U64)mVertCap * TAN_STRIDE;
    slot.pos_offset_bytes  = (U32)((U64)vbase * POS_STRIDE);
    slot.uv_offset_bytes   = (U32)(sec_uv   + (U64)vbase * UV_STRIDE);
    slot.norm_offset_bytes = (U32)(sec_norm + (U64)vbase * NORM_STRIDE);
    slot.tan_offset_bytes  = (U32)(sec_tan  + (U64)vbase * TAN_STRIDE);
    slot.wt_offset_bytes   = (U32)(sec_wt   + (U64)vbase * WT_STRIDE);
    slot.ibase_bytes       = ibase * IDX_STRIDE;
    slot.num_verts         = nv;
    slot.num_indices       = ni;
    slot.valid             = true;
    slot.has_weights       = weights != nullptr;
    slot.has_tangents      = false;
    slot.uv_density        = uv_density;
    slot.reclaimable       = reclaimable;
    slot.last_ref_frame    = (U32)gFrameCount;
    slot.captured_frame    = (U32)gFrameCount;
    {
        F32 lo[3] = {1e30f,1e30f,1e30f}, hi[3] = {-1e30f,-1e30f,-1e30f};
        for (U32 v = 0; v < nv; ++v)
            for (int c = 0; c < 3; ++c) { lo[c] = llmin(lo[c], verts[v].pos[c]); hi[c] = llmax(hi[c], verts[v].pos[c]); }
        for (int c = 0; c < 3; ++c) { slot.cap_lo[c] = lo[c]; slot.cap_hi[c] = hi[c]; }
    }
    SlotInfo si{};
    si.first_index = slot.ibase_bytes / IDX_STRIDE;
    si.index_count = slot.num_indices;
    si.base_vertex = (S32)(slot.pos_offset_bytes / POS_STRIDE);
    si.has_weights = slot.has_weights ? 1u : 0u;
    si.vert_count  = slot.num_verts;
    packSlotBounds(si, slot.cap_lo, slot.cap_hi);
    // Rewrite reuses the slot id; the old range is freed after the SlotInfo swap.
    U32 old_pos_first = 0, old_pos_cnt = 0, old_idx_first = 0, old_idx_cnt = 0;
    auto exist_it = mSlots.find(k);
    if (exist_it != mSlots.end() && exist_it->second.slot_id != 0xFFFFFFFFu)
    {
        slot.slot_id = exist_it->second.slot_id;
        const Slot& os = exist_it->second;
        if (os.num_verts)   { old_pos_first = os.pos_offset_bytes / POS_STRIDE; old_pos_cnt = os.num_verts; }
        if (os.num_indices) { old_idx_first = os.ibase_bytes      / IDX_STRIDE; old_idx_cnt = os.num_indices; }
    }
    else
    {
        slot.slot_id = allocSlotIdLocked();
    }
    mSlotInfo[slot.slot_id] = si;
    if (slot.slot_id < mSlotInfoDirtyMin) mSlotInfoDirtyMin = slot.slot_id;
    if (slot.slot_id + 1 > mSlotInfoDirtyMax) mSlotInfoDirtyMax = slot.slot_id + 1;
    noteBestSlotChange();   // (the cluster LOD best-slot cache)
    slotPut(k, slot);
    mCaptureFail.erase(k);
    mCapturedAssets[std::make_pair(mesh_id, lod)] = true;
    if (old_pos_cnt) freeVerts(old_pos_first, old_pos_cnt);
    if (old_idx_cnt) freeIdx  (old_idx_first, old_idx_cnt);
    mLastCaptureFrame = (U32)gFrameCount;

    // VK CPU mirror (the only geometry copy the VK packer reads).
    mGeomGeneration.fetch_add(1, std::memory_order_relaxed);
    mMirrorDirty.push_back(slot.slot_id);
    mirrorRelForgetLocked(slot.slot_id, true);   // (a freed cluster mirror is back)
    if (slot.slot_id >= mMirrorVerts.size())
    {
        mMirrorVerts.resize(slot.slot_id + 1);
        mMirrorIdx.resize(slot.slot_id + 1);
        mMirrorWeights.resize(slot.slot_id + 1);
    }
    quantizeVerts(verts, nv, mMirrorVerts[slot.slot_id], domain);
    mMirrorIdx[slot.slot_id].assign(indices, indices + ni);   // U32 -> U16 (idx_max < nv <= 65536)
    if (weights) mMirrorWeights[slot.slot_id] = std::move(wts);
    else         mMirrorWeights[slot.slot_id].clear();
    return slot.slot_id;
}

U64 LLCleanMeshAsset::freeAsset(const LLUUID& mesh_id)
{
    std::lock_guard<std::mutex> lk(mMutex);
    U64 freed = 0;
    for (S32 lod = 0; lod < NUM_LOD; ++lod)
    {
        auto it = mSlots.lower_bound(Key{mesh_id, lod, INT32_MIN});
        auto end_it = it;
        while (end_it != mSlots.end() && end_it->first.mesh_id == mesh_id && end_it->first.lod == lod) ++end_it;
        freeAssetRangeLocked(it, end_it, freed);
    }
    return freed;
}

LLCleanMeshAsset::Slot LLCleanMeshAsset::getSlot(const LLUUID& mesh_id, S32 lod, S32 face_idx) const
{
    std::lock_guard<std::mutex> lk(mMutex);
    const Slot* sp = slotFind(Key{mesh_id, lod, face_idx});
    return sp ? *sp : Slot{};
}

void LLCleanMeshAsset::retainRawMesh(const LLUUID& mesh_id, S32 lod, const U8* data, S32 size)
{
    if (!data || size <= 0 || mesh_id.isNull()) return;
    std::lock_guard<std::mutex> lk(mRawMeshMutex);
    const RawKey key(mesh_id, lod);
    auto it = mRawMesh.find(key);
    if (it != mRawMesh.end())
    {
        // Identical per (mesh, lod): keep the copy we have, just mark it recent.
        mRawLru.splice(mRawLru.begin(), mRawLru, it->second.lru);
        return;
    }
    RawEntry& e = mRawMesh[key];
    e.bytes.assign(data, data + size);
    mRawLru.push_front(key);
    e.lru = mRawLru.begin();
    mRawBytes += (U64)size;
    mRawPeakBytes = llmax(mRawPeakBytes, mRawBytes);
    mRawEvicted.erase(key);   // (a stale FIFO entry for it is harmless)
    sLodFetchGot.fetch_add(1, std::memory_order_relaxed);
    evictRawLocked(&key);     // never the bytes that just arrived
}

void LLCleanMeshAsset::evictRawLocked(const RawKey* keep)
{
    const U64 budget = mRawBudget.load(std::memory_order_relaxed);
    while (mRawBytes > budget && !mRawLru.empty())
    {
        const RawKey victim = mRawLru.back();
        if (keep && victim == *keep) break;   // the only (oversized) entry left
        auto it = mRawMesh.find(victim);
        mRawLru.pop_back();
        if (it == mRawMesh.end()) continue;
        mRawBytes -= llmin(mRawBytes, (U64)it->second.bytes.size());
        mRawMesh.erase(it);
        ++mRawEvictions;
        if (mRawEvicted.insert(victim).second)
        {
            mRawEvictedOrder.push_back(victim);
            while (mRawEvictedOrder.size() > 65536u)
            {
                mRawEvicted.erase(mRawEvictedOrder.front());
                mRawEvictedOrder.pop_front();
            }
        }
    }
}

void LLCleanMeshAsset::demoteRawMesh(const LLUUID& mesh_id, S32 lod)
{
    std::lock_guard<std::mutex> lk(mRawMeshMutex);
    auto it = mRawMesh.find(RawKey(mesh_id, lod));
    if (it != mRawMesh.end()) mRawLru.splice(mRawLru.end(), mRawLru, it->second.lru);
}

bool LLCleanMeshAsset::wasRawMeshEvicted(const LLUUID& mesh_id, S32 lod) const
{
    std::lock_guard<std::mutex> lk(mRawMeshMutex);
    return mRawEvicted.count(RawKey(mesh_id, lod)) != 0;
}

void LLCleanMeshAsset::setRawMeshBudgetMB(U32 mb)
{
    const U64 b = (U64)llmax(mb, 16u) << 20;
    if (mRawBudget.exchange(b, std::memory_order_relaxed) == b) return;
    std::lock_guard<std::mutex> lk(mRawMeshMutex);
    evictRawLocked(nullptr);   // lowered: trim now
}

bool LLCleanMeshAsset::hasRawMesh(const LLUUID& mesh_id, S32 lod) const
{
    std::lock_guard<std::mutex> lk(mRawMeshMutex);
    auto it = mRawMesh.find(RawKey(mesh_id, lod));
    return it != mRawMesh.end() && !it->second.bytes.empty();
}

// ---- ASYNC MESH DECODE ------------------------------------------------------------
namespace
{
constexpr U64 kDecodeBudgetBytes = 384ull << 20;   // READY results kept for re-use
// RAM #262: that budget scaled to the machine: min(384 MB, max(96 MB, RAM / 48)) -- 8 GB: 170 MB,
// 16 GB: 341 MB, 32 GB+: 384 MB -- and halved while system memory is critical.
U64 decodeBudgetBytes()
{
    return LLMemoryGovernor::scaleCacheBudget(kDecodeBudgetBytes, 48, 96ull << 20);
}
constexpr U64 kDecodeIdleUs      = 20ull * 1000000ull;   // untouched READY result lifetime
constexpr U64 kDecodeFailUs      = 30ull * 1000000ull;   // NO_BYTES negative-result lifetime
                                                         // (FAILED is sticky: the bytes are immutable,
                                                         // re-inflating them would fail again)
constexpr size_t kDecodeHistCap  = 65536;    // diagnostics history: stops adding past this
constexpr size_t kDecodeFailedCap = 8192;   // sticky FAILED results kept (oldest dropped)

U64 volumeBytes(const LLVolume* v)
{
    U64 b = 0;
    for (S32 f = 0; f < v->getNumVolumeFaces(); ++f)
    {
        const LLVolumeFace& vf = v->getVolumeFace(f);
        // positions + normals + tangents (16 B each) + uv (8) + weights (16 if any) + U16 idx
        b += (U64)vf.mNumVertices * (48 + 8 + (vf.mWeights ? 16 : 0)) + (U64)vf.mNumIndices * 2;
    }
    return b;
}
const char* kEndName[] = { "none", "latched", "expired_untaken", "expired_taken", "budget", "failed_aged" };
}

const char* LLCleanMeshAsset::decodeEndName(U8 e)
{
    return e < END_COUNT ? kEndName[e] : "?";
}

// Record a finished decode (pool or adopted) into its entry + the key history.
void LLCleanMeshAsset::noteDecodedLocked(const DecodeKey& key, DecodeEntry& e, LLVolume* vol)
{
    const U64 now = LLTimer::getTotalTime();
    e.vol = vol; e.done = true; e.t_done = e.t_touch = now;
    if (vol) { e.bytes = volumeBytes(vol); mDecodeBytes += e.bytes; }
    if (!e.waiters.empty()) mDecodeDone.push_back(key);
    ++mStatDecoded;
    if (mDecodeHist.size() < kDecodeHistCap || mDecodeHist.count(key))
    {
        DecodeHist& h = mDecodeHist[key];
        if (h.decodes > 0)
        {
            ++mStatRepeat;
            const U8 why = h.last_end < END_COUNT ? h.last_end : END_NONE;
            ++h.why[why];
            ++mStatRepeatWhy[why];
        }
        ++h.decodes;
        h.sites |= e.site;
        if (vol) h.nfaces = vol->getNumVolumeFaces();
        if (h.decodes > 1) noteRepeaterLocked(key, h.decodes);
    }
}

// #534: keep the 8 most-repeated keys current as decode counts rise. Counts only grow
// and history entries are never erased, so this matches a full scan of mDecodeHist.
void LLCleanMeshAsset::noteRepeaterLocked(const DecodeKey& key, U32 decodes)
{
    size_t min_i = 0;
    for (size_t i = 0; i < kTopRepeaters; ++i)
    {
        if (mTopRepeaters[i].first && mTopRepeaters[i].second == key)
        {
            mTopRepeaters[i].first = decodes;
            return;
        }
        if (mTopRepeaters[i].first < mTopRepeaters[min_i].first) min_i = i;
    }
    if (decodes > mTopRepeaters[min_i].first) mTopRepeaters[min_i] = std::make_pair(decodes, key);
}

void LLCleanMeshAsset::dropDecodeLocked(std::map<DecodeKey, DecodeEntry>::iterator it, DecodeEnd why)
{
    DecodeEntry& e = it->second;
    if (e.vol) { e.vol->unref(); e.vol = nullptr; mDecodeBytes -= llmin(mDecodeBytes, e.bytes); }
    ++mStatEnd[why];
    auto h = mDecodeHist.find(it->first);
    if (h != mDecodeHist.end()) h->second.last_end = why;
    mDecodes.erase(it);
}

// Worker: pop one queued key (priority first) and decode our retained raw bytes into
// an OWNED LLVolume -- exactly what registerCleanFace did inline: unpackVolumeFaces
// (inflate + parse + LL's cacheOptimize: MikkTSpace + meshopt) then genTangents per
// face. Runs on the CleanMeshDecode pool; touches only its own volume + the raw-byte
// store (own lock) + the decode table (own lock).
void LLCleanMeshAsset::runOneDecode()
{
    DecodeKey key;
    LLVolumeParams params;
    bool from_prio = false;
    {
        std::lock_guard<std::mutex> dl(mDecodeMutex);
        bool found = false;
        while (!found && (!mDecodeQPrio.empty() || !mDecodeQ.empty()))
        {
            from_prio = !mDecodeQPrio.empty();
            std::deque<DecodeKey>& q = from_prio ? mDecodeQPrio : mDecodeQ;
            key = q.front();
            q.pop_front();
            auto it = mDecodes.find(key);
            if (it != mDecodes.end() && it->second.state == DECODE_PENDING && !it->second.started
                && !it->second.oom_retry_at)   // #698: still in its out-of-memory backoff
            {
                it->second.started = true;
                params = it->second.params;
                found = true;
            }
        }
        if (!found) return;   // a priority duplicate / adoption already served it
    }
    LL_PROFILE_ZONE_NAMED("clean:asyncMeshDecode");
    const LLUUID& mesh_id = std::get<0>(key);
    const S32 lod = std::get<1>(key);
    DecodeState st = DECODE_NO_BYTES;
    LLVolume* vol = nullptr;
    bool oom = false;   // #698
    U64 oom_bytes = 0;
    try
    {
        std::vector<U8> bytes;
        if (getRawMesh(mesh_id, lod, bytes))
        {
            // RAM #262: wait for the memory governor's budget (this pool thread only; never
            // zero -- with no mesh decode in flight one always runs). Estimate: the
            // compressed bytes inflate ~3-4x and unpack into 56-72 B/vertex faces + tangents.
            const U64 gov_bytes = (U64)bytes.size() * 12;
            oom_bytes = gov_bytes;
            LLMemoryGovernor::acquire(LLMemoryGovernor::SUB_MESH_DECODE, gov_bytes,
                                      from_prio ? LLMemoryGovernor::PRI_HIGH : LLMemoryGovernor::PRI_NORMAL);
            LLMemoryTicket gov_ticket(LLMemoryGovernor::SUB_MESH_DECODE, gov_bytes);
            LLMemoryGovernor::maybeInjectAllocFail();   // #698 test switch (SLMemInjectAllocFail)
            vol = new LLVolume(params, LLVolumeLODGroup::getVolumeScaleFromDetail(lod));
            vol->ref();
            if (vol->unpackVolumeFaces(bytes.data(), (S32)bytes.size()) && vol->getNumVolumeFaces() > 0)
            {
                const S32 nf = vol->getNumVolumeFaces();
                for (S32 f = 0; f < nf; ++f) vol->genTangents(f);
                st = DECODE_READY;
            }
            else
            {
                oom = vol->isOutOfMemory();   // #698: not a bad asset -- retry later
                vol->unref();
                vol = nullptr;
                st = DECODE_FAILED;
            }
        }
    }
    catch (const std::bad_alloc&)
    {
        // #698: out of memory anywhere in the decode (tangents, volume, byte copy): free
        // the partial volume, retry later. Was: forwarded to the main loop as fatal.
        if (vol) { vol->unref(); vol = nullptr; }
        oom = true;
        LLMemoryGovernor::noteAllocFailure(LLMemoryGovernor::ALLOC_CLEAN_DECODE, oom_bytes, "clean mesh decode");
    }
    std::lock_guard<std::mutex> dl(mDecodeMutex);
    auto it = mDecodes.find(key);
    if (it == mDecodes.end() || it->second.state != DECODE_PENDING)
    {
        if (vol) vol->unref();   // shut down / served meanwhile
        return;
    }
    if (oom)
    {
        // #698: back off and retry (the next takeDecoded after oom_retry_at re-queues it;
        // waiters keep waiting), or after the retry limit fail it for now: FAILED with
        // oom_drop ages out after kDecodeFailUs (pumpDecodes), so the asset is eligible
        // again later -- unlike a real decode failure, which is sticky.
        DecodeEntry& e = it->second;
        const U32 tries = ++e.oom_tries;
        if (tries <= LLMemoryGovernor::retryLimit())
        {
            e.started = false;
            e.oom_retry_at = LLTimer::getTotalTime() + (U64)(LLMemoryGovernor::retryDelaySec(tries) * 1.0e6f);
            LLMemoryGovernor::noteAllocRetry(LLMemoryGovernor::ALLOC_CLEAN_DECODE);
            return;
        }
        LLMemoryGovernor::noteAllocDrop(LLMemoryGovernor::ALLOC_CLEAN_DECODE);
        LL_WARNS("CleanMesh") << "#698 async mesh decode " << mesh_id << " lod " << lod << ": out of memory "
                              << (tries - 1) << " times, dropped for now" << LL_ENDL;
        e.oom_drop = true;
        e.oom_retry_at = 0;
        e.state = DECODE_FAILED;
        noteDecodedLocked(key, e, nullptr);
        return;
    }
    it->second.state = st;
    noteDecodedLocked(key, it->second, vol);
}

LLCleanMeshAsset::DecodeState LLCleanMeshAsset::takeDecoded(const LLUUID& mesh_id, S32 lod,
                                                            const LLVolumeParams& params, S32 face_idx,
                                                            U8 site, bool priority, const LLUUID& waiter,
                                                            LLPointer<LLVolume>& out)
{
    // Already captured: capture() is latched per (mesh, lod) and the bytes are
    // immutable, so a decode cannot add this face's slot -- the face is absent from or
    // degenerate in this LOD. (GC / heal clear the latch, re-enabling a decode.)
    {
        std::lock_guard<std::mutex> lk(mMutex);
        auto it = mCapturedAssets.find(std::make_pair(mesh_id, lod));
        // (a freed cluster mirror being re-materialized needs exactly this decode)
        if (it != mCapturedAssets.end() && it->second && !rematKeyLocked(mesh_id, lod))
        {
            std::lock_guard<std::mutex> dl(mDecodeMutex);
            ++mStatSkipLatched;
            return DECODE_FAILED;
        }
    }
    const DecodeKey key(mesh_id, lod, (U8)params.getSculptType());
    const U64 now = LLTimer::getTotalTime();
    bool oom_repost = false;   // #698: consume() re-queued an out-of-memory retry
    auto consume = [&](DecodeState& st) -> bool
    {
        auto it = mDecodes.find(key);
        if (it == mDecodes.end())
        {
            // Known face count from an earlier decode of these bytes: a face past it
            // never exists at this LOD.
            auto h = mDecodeHist.find(key);
            if (h != mDecodeHist.end() && h->second.nfaces >= 0 && face_idx >= h->second.nfaces)
            {
                ++mStatSkipNoFace;
                st = DECODE_FAILED;
                return true;
            }
            return false;
        }
        DecodeEntry& e = it->second;
        if (e.state == DECODE_NO_BYTES) { mDecodes.erase(it); return false; }   // bytes may exist now
        e.site |= site;
        if (e.state == DECODE_PENDING)
        {
            if (waiter.notNull() && e.waiters.size() < 256
                && std::find(e.waiters.begin(), e.waiters.end(), waiter) == e.waiters.end())
                e.waiters.push_back(waiter);
            if (e.oom_retry_at && now >= e.oom_retry_at && !e.started)
            {
                // #698: its out-of-memory backoff is over: queue it again (posted below)
                e.oom_retry_at = 0;
                e.prio = e.prio || priority;
                (e.prio ? mDecodeQPrio : mDecodeQ).push_back(key);
                oom_repost = true;
            }
            else if (priority && !e.prio && !e.started && !e.oom_retry_at) { e.prio = true; mDecodeQPrio.push_back(key); }
            st = DECODE_PENDING;
            return true;
        }
        st = e.state;
        if (st == DECODE_READY)
        {
            out = e.vol;            // shared: the entry keeps its reference for re-use
            e.t_touch = now;
            ++e.takes;
            ++mStatTaken;
        }
        return true;
    };
    DecodeState st = DECODE_NO_BYTES;
    {
        std::lock_guard<std::mutex> dl(mDecodeMutex);
        if (consume(st) && !oom_repost) return st;
    }
    if (oom_repost)
    {
        // #698: the entry exists only because the pool ran it, so the pool exists (unless
        // stopped: then the entry just waits, as any pending one would).
        if (mDecodePool && !mDecodesStopped) mDecodePool->getQueue().post([this]() { runOneDecode(); });
        return st;
    }
    if (!hasRawMesh(mesh_id, lod)) return DECODE_NO_BYTES;
    if (mDecodesStopped) return DECODE_NO_BYTES;   // app cleanup: never restart the pool
    if (!mDecodePool)
    {
        // Render thread only (takeDecoded's sole caller is registerCleanFace).
        mDecodePool.reset(new LL::ThreadPool("CleanMeshDecode", 3));
        mDecodePool->start();
    }
    {
        std::lock_guard<std::mutex> dl(mDecodeMutex);
        if (consume(st))   // adopted from LL's decode between the locks
        {
            if (oom_repost) mDecodePool->getQueue().post([this]() { runOneDecode(); });   // #698
            return st;
        }
        DecodeEntry& e = mDecodes[key];
        e.params = params;
        e.prio = priority;
        e.site = site;
        if (waiter.notNull()) e.waiters.push_back(waiter);
        (priority ? mDecodeQPrio : mDecodeQ).push_back(key);
    }
    if (!mDecodePool->getQueue().post([this]() { runOneDecode(); }))
    {
        // Pool closed (shutdown): forget the request; the caller simply retries.
        std::lock_guard<std::mutex> dl(mDecodeMutex);
        auto it = mDecodes.find(key);
        if (it != mDecodes.end() && !it->second.started) mDecodes.erase(it);
        return DECODE_NO_BYTES;
    }
    LLVKHitch::count("meshDecodeQueued");
    return DECODE_PENDING;
}

void LLCleanMeshAsset::adoptDecodedFromLL(const LLUUID& mesh_id, S32 lod, const LLVolume* src)
{
    // LLMeshRepoThread (worker thread). LL just decoded these exact bytes for its own
    // volume: deep-copy the faces into a volume WE own instead of decoding them again.
    if (!src || mesh_id.isNull() || src->getNumVolumeFaces() <= 0) return;
    const DecodeKey key(mesh_id, lod, (U8)src->getParams().getSculptType());
    {
        std::lock_guard<std::mutex> lk(mMutex);
        auto it = mCapturedAssets.find(std::make_pair(mesh_id, lod));
        if (it != mCapturedAssets.end() && it->second && !rematKeyLocked(mesh_id, lod)) return;   // already captured
    }
    {
        std::lock_guard<std::mutex> dl(mDecodeMutex);
        auto it = mDecodes.find(key);
        if (it != mDecodes.end() && (it->second.state != DECODE_PENDING || it->second.started)) return;
        if (mDecodeBytes > decodeBudgetBytes()) { ++mStatAdoptSkipped; return; }
    }
    LLVolume* vol = nullptr;
    try
    {
        vol = new LLVolume(src->getParams(), LLVolumeLODGroup::getVolumeScaleFromDetail(lod));
        vol->ref();
        vol->copyVolumeFaces(src);
        const S32 nf = vol->getNumVolumeFaces();
        for (S32 f = 0; f < nf; ++f) vol->genTangents(f);
    }
    catch (const std::bad_alloc&)
    {
        // #698: adoption is an optimisation -- skip it (the pool decodes the bytes when
        // the asset is wanted). Must not fail LL's own LOD load, which already succeeded.
        if (vol) vol->unref();
        LLMemoryGovernor::noteAllocFailure(LLMemoryGovernor::ALLOC_CLEAN_DECODE, 0, "clean mesh adopt");
        return;
    }
    bool used = false;
    {
        std::lock_guard<std::mutex> dl(mDecodeMutex);
        auto it = mDecodes.find(key);
        if (it == mDecodes.end() || (it->second.state == DECODE_PENDING && !it->second.started))
        {
            DecodeEntry& e = mDecodes[key];   // new, or fulfils a still-queued request
            if (it == mDecodes.end()) e.params = src->getParams();
            e.state = DECODE_READY; e.started = true; e.adopted = true;
            noteDecodedLocked(key, e, vol);
            ++mStatAdopted;
            used = true;
        }
    }
    if (!used) vol->unref();
}

void LLCleanMeshAsset::pumpDecodes(std::vector<LLUUID>& wake)
{
    {
        static LLCachedControl<U32> s_raw_mb(gSavedSettings, "RenderVKRawMeshCacheMB", 256);
        // RAM #262: never above RAM / 64 (8 GB: 128 MB; 16 GB+: the setting), halved
        // while system memory is critical.
        const U64 raw_b = LLMemoryGovernor::scaleCacheBudget((U64)(U32)s_raw_mb << 20, 64, 64ull << 20);
        setRawMeshBudgetMB((U32)(raw_b >> 20));
    }
    const U64 now = LLTimer::getTotalTime();
    std::vector<DecodeKey> ready;
    {
        std::lock_guard<std::mutex> dl(mDecodeMutex);
        for (const DecodeKey& key : mDecodeDone)
        {
            auto it = mDecodes.find(key);
            if (it == mDecodes.end()) continue;
            for (const LLUUID& w : it->second.waiters) wake.push_back(w);
            it->second.waiters.clear();
            it->second.done = false;
        }
        mDecodeDone.clear();
        static U32 s_calls = 0;
        if (((++s_calls) & 15u) != 0u || mDecodes.empty()) return;
        for (const auto& kv : mDecodes)
            if (kv.second.state == DECODE_READY) ready.push_back(kv.first);
    }
    // Which READY results' assets are now captured (their geometry is in our mirror;
    // nothing will need the volume again unless GC/heal unlatches).
    std::vector<U8> latched(ready.size(), 0);
    {
        std::lock_guard<std::mutex> lk(mMutex);
        for (size_t i = 0; i < ready.size(); ++i)
        {
            auto it = mCapturedAssets.find(std::make_pair(std::get<0>(ready[i]), std::get<1>(ready[i])));
            latched[i] = (it != mCapturedAssets.end() && it->second
                          && !rematKeyLocked(std::get<0>(ready[i]), std::get<1>(ready[i]))) ? 1 : 0;
        }
    }
    std::lock_guard<std::mutex> dl(mDecodeMutex);
    for (size_t i = 0; i < ready.size(); ++i)
    {
        if (!latched[i]) continue;
        auto it = mDecodes.find(ready[i]);
        if (it != mDecodes.end() && it->second.state == DECODE_READY) dropDecodeLocked(it, END_LATCHED);
    }
    for (auto it = mDecodes.begin(); it != mDecodes.end(); )
    {
        auto cur = it++;
        const DecodeEntry& e = cur->second;
        if (e.state == DECODE_READY && now - e.t_touch > kDecodeIdleUs)
            dropDecodeLocked(cur, e.takes ? END_EXPIRED_TAKEN : END_EXPIRED_UNTAKEN);
        else if (e.state == DECODE_NO_BYTES && now - e.t_done > kDecodeFailUs)
            dropDecodeLocked(cur, END_FAILED_AGED);
        // #698: an out-of-memory give-up is not sticky (unlike bad bytes): age it out so
        // the asset is eligible again; and an OOM retry nobody came back for is dropped.
        else if (e.state == DECODE_FAILED && e.oom_drop && now - e.t_done > kDecodeFailUs)
            dropDecodeLocked(cur, END_FAILED_AGED);
        else if (e.state == DECODE_PENDING && !e.started && e.oom_retry_at && now > e.oom_retry_at + kDecodeIdleUs)
            dropDecodeLocked(cur, END_FAILED_AGED);
    }
    {
        // FAILED is sticky (the bytes are immutable) but not unbounded: a stream of
        // distinct undecodable assets keeps only the newest kDecodeFailedCap. A dropped
        // one costs at most a repeat decode (which fails again) if it is requested.
        std::vector<std::pair<U64, DecodeKey>> failed;
        for (const auto& kv : mDecodes)
            if (kv.second.state == DECODE_FAILED) failed.emplace_back(kv.second.t_done, kv.first);
        if (failed.size() > kDecodeFailedCap)
        {
            std::sort(failed.begin(), failed.end());
            const size_t drop = failed.size() - kDecodeFailedCap * 3 / 4;
            for (size_t i = 0; i < drop; ++i)
            {
                auto it = mDecodes.find(failed[i].second);
                if (it != mDecodes.end()) dropDecodeLocked(it, END_FAILED_AGED);
            }
        }
    }
    const U64 decode_budget = decodeBudgetBytes();
    if (mDecodeBytes > decode_budget)
    {
        // Over budget: evict least-recently-touched READY results.
        std::vector<std::pair<U64, DecodeKey>> lru;
        for (const auto& kv : mDecodes)
            if (kv.second.state == DECODE_READY) lru.emplace_back(kv.second.t_touch, kv.first);
        std::sort(lru.begin(), lru.end());
        for (const auto& l : lru)
        {
            if (mDecodeBytes <= decode_budget * 3 / 4) break;
            auto it = mDecodes.find(l.second);
            if (it != mDecodes.end()) dropDecodeLocked(it, END_BUDGET);
        }
    }

    static U64 s_last_log = 0;
    if (now - s_last_log > 10ull * 1000000ull && (mStatDecoded || mStatSkipLatched))
    {
        s_last_log = now;
        std::ostringstream os;
        os << "async mesh decode: decoded=" << mStatDecoded << " (adopted_from_LL=" << mStatAdopted
           << ") taken=" << mStatTaken << " repeats=" << mStatRepeat
           << " skipped[latched=" << mStatSkipLatched << " no_face=" << mStatSkipNoFace
           << " adopt_over_budget=" << mStatAdoptSkipped << "] ended[";
        for (int k = 1; k < END_COUNT; ++k) os << (k > 1 ? " " : "") << kEndName[k] << "=" << mStatEnd[k];
        os << "] table=" << mDecodes.size() << " ready_MB=" << (mDecodeBytes >> 20)
           << " queued=" << (mDecodeQPrio.size() + mDecodeQ.size());
        // Top repeated keys and why their previous result was gone (#534: both kept
        // incrementally by noteDecodedLocked -- no walk of mDecodeHist here).
        std::vector<std::pair<U32, const DecodeKey*>> top;
        for (const auto& r : mTopRepeaters)
            if (r.first > 1) top.emplace_back(r.first, &r.second);
        std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        os << " | repeat_after[";
        for (int k = 0; k < END_COUNT; ++k) os << (k ? " " : "") << kEndName[k] << "=" << mStatRepeatWhy[k];
        os << "]";
        for (size_t i = 0; i < top.size() && i < 8; ++i)
        {
            auto hit = mDecodeHist.find(*top[i].second);
            if (hit == mDecodeHist.end()) continue;
            const DecodeHist& h = hit->second;
            os << "\n    " << std::get<0>(*top[i].second).asString() << " lod=" << std::get<1>(*top[i].second)
               << " flags=" << (S32)std::get<2>(*top[i].second) << " decodes=" << h.decodes
               << " nfaces=" << h.nfaces << " sites=" << (S32)h.sites << " after[";
            for (int k = 0; k < END_COUNT; ++k) if (h.why[k]) os << " " << kEndName[k] << "=" << h.why[k];
            os << " ]";
        }
        LL_INFOS("CleanMeshDecode") << os.str() << LL_ENDL;
    }
}

bool LLCleanMeshAsset::getRawMesh(const LLUUID& mesh_id, S32 lod, std::vector<U8>& out) const
{
    std::lock_guard<std::mutex> lk(mRawMeshMutex);
    auto it = mRawMesh.find(RawKey(mesh_id, lod));
    if (it == mRawMesh.end() || it->second.bytes.empty()) return false;
    mRawLru.splice(mRawLru.begin(), mRawLru, it->second.lru);   // decoded = recently used
    out = it->second.bytes;
    return true;
}

void LLCleanMeshAsset::invalidateAsset(const LLUUID& mesh_id, S32 lod)
{
    std::lock_guard<std::mutex> lk(mMutex);
    // Free every face-slot for (mesh_id, lod): release its vert/index ranges,
    // zero + recycle its SlotInfo (index_count=0 -> the cull no-ops on it until
    // re-captured), drop it from mSlots. mSlots is sorted by Key so an asset's
    // faces are contiguous, but a simple full scan is fine here (rare path).
    for (auto it = mSlots.begin(); it != mSlots.end(); ++it)
    {
        if (it->first.mesh_id == mesh_id && it->first.lod == lod)
        {
            Slot& s = it->second;
            if (s.num_verts)   freeVerts(s.pos_offset_bytes / POS_STRIDE, s.num_verts);
            if (s.num_indices) freeIdx  (s.ibase_bytes      / IDX_STRIDE, s.num_indices);
            // STABLE SLOT ID (#29): do NOT recycle the slot_id or erase the
            // mSlots entry. The immediately-following re-capture (same
            // registerFace call) reuses this exact slot_id, so every live
            // instance of this SHARED asset keeps a valid pointer. A sim
            // surround is hundreds of identical sculpt panels all pointing at
            // this one slot; only ONE of them triggers the re-capture and
            // re-registers, so recycling the id stranded all the others on the
            // freed slot. Their SlotInfo (zeroed just below) reads index_count==0
            // -> the cull EMITS them but the indirect draw is a no-op -> "most
            // geometry evaporates and never comes back" with every health
            // counter clean (orphan_slots=0, sentinel_inst=0). Free only the
            // STORAGE here; keep the id + entry, mark the slot empty, and zero
            // its SlotInfo so a draw in the sub-frame window before re-capture
            // is a safe no-op.
            s.num_verts   = 0;
            s.num_indices = 0;
            s.valid       = false;
            if (s.slot_id != 0xFFFFFFFFu)
            {
                // #29: this zeroes SlotInfo[slot_id] -> index_count 0. If a live
                // instance's group still resolves to this slot before re-capture
                // republishes it, the draw no-ops (the mass-vanish). Record who
                // did it + which frame so the detector can correlate.
                CleanProv::record(CleanProv::INVALIDATE, s.slot_id, 0xFFFFFFFFu, (U32)it->first.face_idx, mesh_id);
                mSlotInfo[s.slot_id] = SlotInfo{};
                if (s.slot_id < mSlotInfoDirtyMin) mSlotInfoDirtyMin = s.slot_id;
                if (s.slot_id + 1 > mSlotInfoDirtyMax) mSlotInfoDirtyMax = s.slot_id + 1;
                noteBestSlotChange();   // (the cluster LOD best-slot cache)
                clearMirrorLocked(s.slot_id);
            }
        }
    }
    // Drop the dedup latch so the next capture() re-ingests fresh geometry.
    mCapturedAssets.erase(std::make_pair(mesh_id, lod));
    noteUnlatchLocked(mesh_id, lod, UNL_INVALIDATE);
}

bool LLCleanMeshAsset::debugGetSlotInfo(U32 slot_id, SlotInfo& out) const
{
    std::lock_guard<std::mutex> lk(mMutex);
    if (slot_id >= mSlotInfo.size()) return false;
    out = mSlotInfo[slot_id];
    return true;
}

void LLCleanMeshAsset::noteConverge(const LLUUID& mesh_id, S32 face_idx, const ConvergeTrace& t)
{
    std::lock_guard<std::mutex> lk(mMutex);
    // Diagnostics: entries go with their LOD group (gcLodGroups); the cap is a backstop.
    const auto key = std::make_pair(mesh_id, face_idx);
    if (mConvergeTrace.size() >= 262144u && mConvergeTrace.find(key) == mConvergeTrace.end())
        mConvergeTrace.clear();
    mConvergeTrace[key] = t;
}

LLUUID LLCleanMeshAsset::geometryKey(const LLVolumeParams& params)
{
    LLUUID id = params.getSculptID();
    if (id.isNull()) return primShapeKey(params);
    const U8 st = params.getSculptType();
    const U8 base = st & LL_SCULPT_TYPE_MASK;
    if (base == LL_SCULPT_TYPE_MESH || base == LL_SCULPT_TYPE_GLTF) return id;   // decode keys by asset id
    id.mData[15] ^= st;
    id.mData[14] ^= 0xA5;   // never equal to the bare map id, even for type byte 0
    return id;
}

LLUUID LLCleanMeshAsset::primShapeKey(const LLVolumeParams& params)
{
    const LLProfileParams& pp = params.getProfileParams();
    const LLPathParams&    ph = params.getPathParams();
    struct ShapeKey {
        U8  prof_curve, path_curve, pad0, pad1;
        F32 prof_begin, prof_end, prof_hollow;
        F32 path_begin, path_end, scale_x, scale_y, shear_x, shear_y;
        F32 twist_begin, twist_end, radius_off, taper_x, taper_y,
            revolutions, skew;
    } sk;
    memset(&sk, 0, sizeof(sk));
    sk.prof_curve  = pp.getCurveType();
    sk.path_curve  = ph.getCurveType();
    sk.prof_begin  = pp.getBegin();  sk.prof_end = pp.getEnd();
    sk.prof_hollow = pp.getHollow();
    sk.path_begin  = ph.getBegin();  sk.path_end = ph.getEnd();
    sk.scale_x = ph.getScaleX(); sk.scale_y = ph.getScaleY();
    sk.shear_x = ph.getShearX(); sk.shear_y = ph.getShearY();
    sk.twist_begin = ph.getTwistBegin(); sk.twist_end = ph.getTwistEnd();
    sk.radius_off  = ph.getRadiusOffset();
    sk.taper_x = ph.getTaperX(); sk.taper_y = ph.getTaperY();
    sk.revolutions = ph.getRevolutions(); sk.skew = ph.getSkew();
    auto fnv = [](const void* d, size_t n, U64 seed) -> U64 {
        U64 h = seed; const U8* b = (const U8*)d;
        for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ULL; }
        return h;
    };
    const U64 h1 = fnv(&sk, sizeof(sk), 1469598103934665603ULL);
    const U64 h2 = fnv(&sk, sizeof(sk), 0x9E3779B97F4A7C15ULL);
    LLUUID id;
    memcpy(id.mData,     &h1, 8);
    memcpy(id.mData + 8, &h2, 8);
    return id;
}

U32 LLCleanMeshAsset::groupSlotAt(const LLUUID& mesh_id, S32 face_idx, S32 lod) const
{
    if (lod < 0 || lod >= NUM_LOD) return LOD_SENTINEL;
    std::lock_guard<std::mutex> lk(mMutex);
    auto it = mFaceLodGroup.find(std::make_pair(mesh_id, face_idx));
    if (it == mFaceLodGroup.end() || it->second >= (U32)mLodGroups.size())
        return LOD_SENTINEL;
    return mLodGroups[it->second].slot[lod];
}

U32 LLCleanMeshAsset::groupBestSlot(const LLUUID& mesh_id, S32 face_idx) const
{
    std::lock_guard<std::mutex> lk(mMutex);
    auto it = mFaceLodGroup.find(std::make_pair(mesh_id, face_idx));
    if (it == mFaceLodGroup.end() || it->second >= (U32)mLodGroups.size())
        return LOD_SENTINEL;
    const LodGroup& g = mLodGroups[it->second];
    for (S32 L = NUM_LOD - 1; L >= 0; --L)
    {
        const U32 s = g.slot[L];
        if (s == LOD_SENTINEL || s >= (U32)mSlotInfo.size()) continue;
        if (mSlotInfo[s].index_count > 0) return s;
    }
    return LOD_SENTINEL;
}

U32 LLCleanMeshAsset::drawnSlot(U32 lod_group_id, const F32 model_matrix[16], F32 lod_radius,
                                const LLVector3& cam_origin, F32 lod_factor, S32* out_level) const
{
    if (out_level) *out_level = -1;
    std::lock_guard<std::mutex> lk(mMutex);
    if (lod_group_id >= (U32)mLodGroups.size()) return LOD_SENTINEL;
    const LodGroup& g = mLodGroups[lod_group_id];
    auto resident = [&](U32 s) { return s != LOD_SENTINEL && s < (U32)mSlotInfo.size() && mSlotInfo[s].index_count > 0; };
    // clean_cull.comp: detailFromTan((lod_factor * radius) / dist), ll_round(x, 0.01).
    const F32 dx = model_matrix[12] - cam_origin.mV[0];
    const F32 dy = model_matrix[13] - cam_origin.mV[1];
    const F32 dz = model_matrix[14] - cam_origin.mV[2];
    const F32 dist = std::max(std::sqrt(dx * dx + dy * dy + dz * dz), 0.001f);
    const F32 tan_angle = std::floor((lod_factor * lod_radius) / dist * 100.f + 0.5f) * 0.01f;
    const S32 level = tan_angle <= 0.03f ? 0 : tan_angle <= 0.06f ? 1 : tan_angle <= 0.24f ? 2 : 3;
    if (resident(g.slot[level])) { if (out_level) *out_level = level; return g.slot[level]; }
    for (S32 L = NUM_LOD - 1; L >= 0; --L)
    {
        if (resident(g.slot[L])) { if (out_level) *out_level = L; return g.slot[L]; }
    }
    return LOD_SENTINEL;
}


bool LLCleanMeshAsset::groupLodCounts(U32 lod_group_id, U32 tris[NUM_LOD], U32 verts[NUM_LOD]) const
{
    for (S32 L = 0; L < NUM_LOD; ++L) tris[L] = verts[L] = 0;
    std::lock_guard<std::mutex> lk(mMutex);
    if (lod_group_id >= (U32)mLodGroups.size()) return false;
    const LodGroup& g = mLodGroups[lod_group_id];
    for (S32 L = 0; L < NUM_LOD; ++L)
    {
        const U32 s = g.slot[L];
        if (s == LOD_SENTINEL || s >= (U32)mSlotInfo.size()) continue;
        tris[L] = mSlotInfo[s].index_count / 3;
        verts[L] = tris[L] ? mSlotInfo[s].vert_count : 0;
    }
    return true;
}

void LLCleanMeshAsset::clearMirrorLocked(U32 slot_id)
{
    // Caller holds mMutex.
    mirrorRelForgetLocked(slot_id, false);   // (a freed / invalidated slot: its release goes with it)
    if (slot_id >= mMirrorVerts.size()) return;
    LLCleanAssetLife::add(LLCleanAssetLife::MIRROR_BYTES,
        (U64)mMirrorVerts[slot_id].capacity() * sizeof(QVertex) + (U64)mMirrorIdx[slot_id].capacity() * sizeof(U16)
        + (slot_id < mMirrorWeights.size() ? (U64)mMirrorWeights[slot_id].capacity() * sizeof(MirrorWeight) : 0ull));
    dropMirrorVectorsLocked(slot_id);
    mMirrorDirty.push_back(slot_id);   // VK packer: index_count>0 -> garbage, then zero SlotInfo
    mGeomGeneration.fetch_add(1, std::memory_order_relaxed);
}

U64 LLCleanMeshAsset::dropMirrorVectorsLocked(U32 slot_id)
{
    if (slot_id >= mMirrorVerts.size()) return 0;
    const U64 b = (U64)mMirrorVerts[slot_id].size() * sizeof(QVertex) + (U64)mMirrorIdx[slot_id].size() * sizeof(U16)
                + (slot_id < mMirrorWeights.size() ? (U64)mMirrorWeights[slot_id].size() * sizeof(MirrorWeight) : 0ull);
    std::vector<QVertex>().swap(mMirrorVerts[slot_id]);   // release capacity
    std::vector<U16>().swap(mMirrorIdx[slot_id]);
    if (slot_id < mMirrorWeights.size()) std::vector<MirrorWeight>().swap(mMirrorWeights[slot_id]);
    return b;
}

// ---- CLUSTER-OWNED MIRROR RELEASE (2026-10-16, after #798; see the header) ----------
void LLCleanMeshAsset::rematUnwantLocked(MirrorRel& r)
{
    if (r.want_time <= 0.0) return;
    r.want_time = 0.0;
    r.warned = false;
    const U32 n = mRematWanted.load(std::memory_order_relaxed);
    mRematWanted.store(n ? n - 1u : 0u, std::memory_order_relaxed);
    auto l = mRematLods.find(std::make_pair(r.mesh_id, r.lod));
    if (l != mRematLods.end() && --l->second == 0u) mRematLods.erase(l);
    auto f = mRematFaces.find(std::make_pair(r.mesh_id, r.face));
    if (f != mRematFaces.end())
    {
        f->second &= (U8)~(1u << (r.lod & 7));
        if (!f->second) mRematFaces.erase(f);
    }
}

void LLCleanMeshAsset::mirrorRelForgetLocked(U32 slot_id, bool written)
{
    if (mMirrorRel.empty()) return;
    auto it = mMirrorRel.find(slot_id);
    if (it == mMirrorRel.end()) return;
    if (written && it->second.want_time > 0.0)
    {
        ++mStatRemat;
        LL_DEBUGS("VKMem") << "cluster mirror re-materialized: mesh " << it->second.mesh_id << " lod " << it->second.lod
                           << " face " << it->second.face << " slot " << slot_id << " after "
                           << (LLTimer::getTotalSeconds() - it->second.want_time) << " s" << LL_ENDL;
    }
    rematUnwantLocked(it->second);
    mMirrorRelBytes -= std::min(mMirrorRelBytes, it->second.bytes);
    mMirrorRel.erase(it);
}

void LLCleanMeshAsset::queueClusterMirrorRelease(U32 slot_id, const LLUUID& mesh_id, S32 lod, S32 face_idx)
{
    if (slot_id == LOD_SENTINEL) return;
    std::lock_guard<std::mutex> lk(mMutex);
    // Bounded: the renderer offers a key every few seconds at most; a burst past the cap is
    // simply offered again later.
    if (mMirrorRelQueue.size() >= 65536u) return;
    try { mMirrorRelQueue.push_back(RelQ{slot_id, mesh_id, lod, face_idx}); }
    catch (const std::bad_alloc&) {}   // (offered again later)
}

void LLCleanMeshAsset::cancelMirrorReleases()
{
    std::lock_guard<std::mutex> lk(mMutex);
    std::vector<RelQ>().swap(mMirrorRelQueue);
}

U32 LLCleanMeshAsset::pumpMirrorReleases()
{
    std::lock_guard<std::mutex> lk(mMutex);
    if (mMirrorRelQueue.empty()) return 0;
    U32 freed = 0;
    U64 bytes = 0;
    for (const RelQ& q : mMirrorRelQueue)
    {
        const U32 sl = q.slot;
        // Still that face's slot, still holding a mirror, not released / wanted already.
        if (sl >= mMirrorVerts.size() || mMirrorVerts[sl].size() <= QV_HEADER) continue;
        if (mMirrorRel.count(sl)) continue;
        const Slot* s = slotFind(Key{q.mesh_id, q.lod, q.face});
        if (!s || !s->valid || s->slot_id != sl) continue;
        MirrorRel r;
        r.mesh_id = q.mesh_id;
        r.lod = q.lod;
        r.face = q.face;
        MirrorRel* pr = nullptr;
        try { pr = &mMirrorRel.emplace(sl, r).first->second; }
        catch (const std::bad_alloc&) { continue; }   // (kept; offered again later)
        const U64 b = dropMirrorVectorsLocked(sl);
        pr->bytes = b;
        mMirrorRelBytes += b;
        bytes += b;
        ++freed;
        // The packer and the legacy rigged LOD helpers see the slot change (they drop what
        // they derived from it; a legacy row that needs the slot asks for it back: placeSlot).
        mMirrorDirty.push_back(sl);
        mGeomGeneration.fetch_add(1, std::memory_order_relaxed);
    }
    std::vector<RelQ>().swap(mMirrorRelQueue);
    if (freed)
    {
        mStatMirrorFreed += freed;
        mStatMirrorFreedBytes += bytes;
        static F64 s_last = 0.0;
        static U32 s_n = 0;
        static U64 s_b = 0;
        s_n += freed;
        s_b += bytes;
        const F64 now = LLTimer::getTotalSeconds();
        if (now - s_last > 10.0)
        {
            LL_INFOS("VKMem") << "cluster LOD: freed the CPU mirror of " << s_n << " cluster-owned slots ("
                              << (s_b >> 20) << " MB) since the last note; " << mMirrorRel.size() << " released now ("
                              << (mMirrorRelBytes >> 20) << " MB), " << mRematWanted.load(std::memory_order_relaxed)
                              << " re-materializing" << LL_ENDL;
            s_last = now;
            s_n = 0;
            s_b = 0;
        }
    }
    return freed;
}

bool LLCleanMeshAsset::mirrorReleased(U32 slot_id) const
{
    std::lock_guard<std::mutex> lk(mMutex);
    return !mMirrorRel.empty() && mMirrorRel.count(slot_id) != 0;
}

bool LLCleanMeshAsset::requestMirror(U32 slot_id, const char* why)
{
    std::lock_guard<std::mutex> lk(mMutex);
    // A queued (not yet applied) release of it is cancelled: wanting wins.
    if (!mMirrorRelQueue.empty())
        mMirrorRelQueue.erase(std::remove_if(mMirrorRelQueue.begin(), mMirrorRelQueue.end(),
                                             [slot_id](const RelQ& q) { return q.slot == slot_id; }),
                              mMirrorRelQueue.end());
    auto it = mMirrorRel.find(slot_id);
    if (it == mMirrorRel.end() || it->second.want_time > 0.0) return false;
    MirrorRel& r = it->second;
    const auto lk_key = std::make_pair(r.mesh_id, r.lod);
    const auto fk_key = std::make_pair(r.mesh_id, r.face);
    try
    {
        U8& bits = mRematFaces[fk_key];   // (first: the only insert that can throw before the count)
        ++mRematLods[lk_key];
        bits |= (U8)(1u << (r.lod & 7));
    }
    catch (const std::bad_alloc&)
    {
        // #698 policy: not now -- the asker repeats while it still needs the slot.
        auto f = mRematFaces.find(fk_key);
        if (f != mRematFaces.end() && !f->second) mRematFaces.erase(f);
        return false;
    }
    if (slot_id < mSlotPubs.size() && mRematWake.size() < 65536u)
    {
        try { mRematWake.insert(mRematWake.end(), mSlotPubs[slot_id].begin(), mSlotPubs[slot_id].end()); }
        catch (const std::bad_alloc&) {}   // (the periodic refresh wakes it)
    }
    r.want_time = LLTimer::getTotalSeconds();
    r.warned = false;
    mRematWanted.fetch_add(1, std::memory_order_relaxed);
    ++mStatRematAsk;
    static F64 s_last = 0.0;
    static U32 s_n = 0;
    ++s_n;
    if (r.want_time - s_last > 10.0)
    {
        LL_INFOS("VKMem") << "cluster LOD: re-materializing " << s_n << " freed CPU mirror(s) since the last note (e.g. "
                          << (why ? why : "?") << ": mesh " << r.mesh_id << " lod " << r.lod << " face " << r.face
                          << "); until each is back its slot has no legacy geometry / build source" << LL_ENDL;
        s_last = r.want_time;
        s_n = 0;
    }
    return true;
}

void LLCleanMeshAsset::takeRematWake(std::vector<U32>& groups, bool refresh)
{
    groups.clear();
    std::lock_guard<std::mutex> lk(mMutex);
    groups.swap(mRematWake);
    if (refresh && mRematWanted.load(std::memory_order_relaxed))
    {
        const F64 now = LLTimer::getTotalSeconds();
        U32 stale = 0;
        for (auto& kv : mMirrorRel)
        {
            MirrorRel& r = kv.second;
            if (r.want_time <= 0.0) continue;
            if (kv.first < mSlotPubs.size() && groups.size() < 65536u)
                groups.insert(groups.end(), mSlotPubs[kv.first].begin(), mSlotPubs[kv.first].end());
            if (!r.warned && now - r.want_time > kRematStaleSec)
            {
                r.warned = true;
                if (stale++ < 4u)
                    LL_WARNS("VKMem") << "cluster LOD: the freed mirror of mesh " << r.mesh_id << " lod " << r.lod << " face "
                                      << r.face << " is still re-materializing after " << (U32)(now - r.want_time)
                                      << " s (no decode / LL re-delivery yet; still retried)" << LL_ENDL;
            }
        }
    }
    std::sort(groups.begin(), groups.end());
    groups.erase(std::unique(groups.begin(), groups.end()), groups.end());
}

bool LLCleanMeshAsset::rematWantedFace(const LLUUID& mesh_id, S32 face_idx, S32& lod_out) const
{
    if (mRematWanted.load(std::memory_order_relaxed) == 0u) return false;
    std::lock_guard<std::mutex> lk(mMutex);
    auto f = mRematFaces.find(std::make_pair(mesh_id, face_idx));
    if (f == mRematFaces.end() || !f->second) return false;
    for (S32 L = 0; L < NUM_LOD; ++L)
        if (f->second & (1u << L)) { lod_out = L; return true; }
    return false;
}

void LLCleanMeshAsset::slotPublishers(U32 slot_id, std::vector<U32>& out) const
{
    out.clear();
    std::lock_guard<std::mutex> lk(mMutex);
    if (slot_id < mSlotPubs.size()) out = mSlotPubs[slot_id];
}

LLCleanMeshAsset::MirrorRelStats LLCleanMeshAsset::mirrorRelStats() const
{
    MirrorRelStats o;
    std::lock_guard<std::mutex> lk(mMutex);
    o.released = (U32)mMirrorRel.size();
    o.bytes = mMirrorRelBytes;
    o.wanted = mRematWanted.load(std::memory_order_relaxed);
    if (o.wanted)
    {
        const F64 now = LLTimer::getTotalSeconds();
        for (const auto& kv : mMirrorRel)
            if (kv.second.want_time > 0.0 && now - kv.second.want_time > kRematStaleSec) ++o.stale;
    }
    o.freed_total = mStatMirrorFreed;
    o.freed_bytes_total = mStatMirrorFreedBytes;
    o.remat_total = mStatRemat;
    o.asked_total = mStatRematAsk;
    return o;
}

std::map<LLCleanMeshAsset::Key, LLCleanMeshAsset::Slot>::iterator
LLCleanMeshAsset::freeAssetRangeLocked(std::map<Key, Slot>::iterator it,
                                       std::map<Key, Slot>::iterator end_it, U64& freed_bytes)
{
    if (it == end_it) return end_it;
    const LLUUID mesh = it->first.mesh_id;
    const S32    lod  = it->first.lod;
    for (auto k = it; k != end_it; ++k)
    {
        Slot& s = k->second;
        // Terrain-storage slots (captureTerrain) own their headroom capacity too.
        const U32 vfree = s.vert_cap ? s.vert_cap : s.num_verts;
        const U32 ifree = s.idx_cap  ? s.idx_cap  : s.num_indices;
        if (vfree) freeVerts(s.pos_offset_bytes / POS_STRIDE, vfree);
        if (ifree) freeIdx(s.ibase_bytes / IDX_STRIDE, ifree);
        const U64 slot_bytes = (U64)s.num_verts * VERT_STRIDE_TOTAL + (U64)s.num_indices * IDX_STRIDE;
        freed_bytes += slot_bytes;
        LLCleanAssetLife::add(LLCleanAssetLife::GEOM_BYTES, slot_bytes);
        if (s.slot_id != 0xFFFFFFFFu)
        {
            LLCleanAssetLife::add(LLCleanAssetLife::MESH_SLOTS, 1);
            // Recycled after kIdQuarantineFrames (allocSlotIdLocked).
            releaseSlotIdLocked(s.slot_id);
            // Zero the SlotInfo so any stale cull read of a recycled-but-
            // not-yet-reused slot draws nothing (vert/index counts 0).
            mSlotInfo[s.slot_id] = SlotInfo{};
            if (s.slot_id < mSlotInfoDirtyMin) mSlotInfoDirtyMin = s.slot_id;
            if (s.slot_id + 1 > mSlotInfoDirtyMax) mSlotInfoDirtyMax = s.slot_id + 1;
            noteBestSlotChange();   // (the cluster LOD best-slot cache)
            // Unpublish the freed id from EVERY group / level naming it: once the id
            // is recycled for a DIFFERENT asset, a group still naming it would draw
            // the wrong mesh (stale addressing). Not just slot[lod] of this asset's own
            // group: the structural-gap fallback publishes a slot at another level, and
            // smooth terrain / trees publish slots keyed by another asset id. A re-rez
            // re-captures (latch dropped below) and re-publishes.
            unpublishSlotLocked(s.slot_id);
            // VK: drop the CPU geometry mirror and tell the VK packer.
            clearMirrorLocked(s.slot_id);
        }
    }
    // Drop the dedup flag so the asset re-captures if it's seen again.
    mCapturedAssets.erase(std::make_pair(mesh, lod));
    noteUnlatchLocked(mesh, lod, UNL_GC);
    for (auto e = it; e != end_it; ++e) { mSlotIdx.erase(e->first); mCaptureFail.erase(e->first); }
    return mSlots.erase(it, end_it);
}

U64 LLCleanMeshAsset::garbageCollect(const std::unordered_set<U32>& in_use,
                                     U32 current_frame, U32 keep_age)
{
    std::lock_guard<std::mutex> lk(mMutex);

    U64 freed_bytes = 0;
    // mSlots is sorted by (mesh_id, lod, face_idx), so all faces of one
    // (mesh_id, lod) are contiguous. Reclaim a whole asset at once -- the
    // mCapturedAssets dedup is keyed per (mesh_id, lod), so freeing some faces
    // but not others would let a later re-capture re-add the freed ones while
    // orphaning the kept ones.
    auto it = mSlots.begin();
    while (it != mSlots.end())
    {
        const LLUUID mesh = it->first.mesh_id;
        const S32    lod  = it->first.lod;

        // Scan this asset's face range: stamp referenced faces, track whether
        // every face is reclaimable and the newest reference across all faces.
        auto         end_it      = it;
        bool         all_reclaim = true;
        U32          newest_ref  = 0;
        while (end_it != mSlots.end() &&
               end_it->first.mesh_id == mesh && end_it->first.lod == lod)
        {
            Slot& s = end_it->second;
            if (!s.reclaimable) all_reclaim = false;
            if (s.slot_id != 0xFFFFFFFFu && in_use.count(s.slot_id))
                s.last_ref_frame = current_frame;
            if (s.last_ref_frame > newest_ref) newest_ref = s.last_ref_frame;
            ++end_it;
        }

        // Free only when the WHOLE asset is reclaimable and no face has been
        // referenced for >= keep_age frames (conservative: a rebuild-window
        // blip keeps a recent reference, and re-capture re-fires on re-rez).
        const bool aged_out = (current_frame >= newest_ref) &&
                              ((current_frame - newest_ref) >= keep_age);
        if (all_reclaim && aged_out)
        {
            it = freeAssetRangeLocked(it, end_it, freed_bytes);
        }
        else
        {
            it = end_it;
        }
    }
    return freed_bytes;
}

bool LLCleanMeshAsset::garbageCollectSlice(U32 current_frame, U32 keep_age, F64 budget_s, U64& freed_bytes)
{
    LLTimer timer;
    std::lock_guard<std::mutex> lk(mMutex);
    std::lock_guard<std::mutex> rl(mGroupRefMutex);   // leaf, under mMutex (as reapRetiredAssets)
    // Resume at the START of the cursor asset (a face added below the cursor since must
    // not split it: an asset is reclaimed whole).
    auto it = mGcCursorValid ? mSlots.lower_bound(Key{mGcCursor.mesh_id, mGcCursor.lod, INT32_MIN}) : mSlots.begin();
    U32 visited = 0;
    while (it != mSlots.end())
    {
        if (visited > 0 && (visited & 31u) == 0u && timer.getElapsedTimeF64().value() > budget_s)
        {
            break;
        }
        ++visited;
        const LLUUID mesh = it->first.mesh_id;
        const S32    lod  = it->first.lod;
        // As garbageCollect, with "referenced" = published by a group a live instance uses.
        auto end_it      = it;
        bool all_reclaim = true;
        U32  newest_ref  = 0;
        while (end_it != mSlots.end() && end_it->first.mesh_id == mesh && end_it->first.lod == lod)
        {
            Slot& s = end_it->second;
            if (!s.reclaimable) all_reclaim = false;
            if (s.slot_id != 0xFFFFFFFFu && s.slot_id < (U32)mSlotPubs.size())
            {
                for (U32 gid : mSlotPubs[s.slot_id])
                {
                    if (gid < (U32)mGroupRefs.size() && mGroupRefs[gid] != 0u)
                    {
                        s.last_ref_frame = current_frame;
                        break;
                    }
                    // #737: unreferenced publisher -- its last instance left at
                    // mGroupUnrefFrame; the grace counts from then.
                    if (gid < (U32)mGroupUnrefFrame.size() && (S32)(mGroupUnrefFrame[gid] - s.last_ref_frame) > 0
                        && mGroupUnrefFrame[gid] <= current_frame)
                        s.last_ref_frame = mGroupUnrefFrame[gid];
                }
            }
            if (s.last_ref_frame > newest_ref) newest_ref = s.last_ref_frame;
            ++end_it;
        }
        const bool aged_out = (current_frame >= newest_ref) && ((current_frame - newest_ref) >= keep_age);
        it = (all_reclaim && aged_out) ? freeAssetRangeLocked(it, end_it, freed_bytes) : end_it;
    }
    if (it == mSlots.end())
    {
        mGcCursorValid = false;
        return true;
    }
    mGcCursor = it->first;   // the next asset
    mGcCursorValid = true;
    return false;
}

U32 LLCleanMeshAsset::assetCount() const
{
    std::lock_guard<std::mutex> lk(mMutex);
    return (U32)mCapturedAssets.size();
}

U32 LLCleanMeshAsset::slotCount() const
{
    std::lock_guard<std::mutex> lk(mMutex);
    return (U32)mSlots.size();
}

U32 LLCleanMeshAsset::slotInfoCount() const
{
    std::lock_guard<std::mutex> lk(mMutex);
    return (U32)mSlotInfo.size();
}


// ---- MEMORY STATS -----------------------------------------------------------------
void LLCleanMeshAsset::memStats(MemStats& o)
{
    o = MemStats{};
    {
        std::lock_guard<std::mutex> lk(mMutex);
        o.slots_live    = (U32)mSlots.size();
        o.slot_ids_hw   = (U32)mSlotInfo.size();
        o.slot_ids_free = (U32)mFreeSlotIds.size();
        o.slot_ids_quar = (U32)mSlotQuarantine.size();
        o.groups_live   = mLiveGroups;
        o.groups_hw     = (U32)mLodGroups.size();
        o.groups_free   = (U32)mFreeGroupIds.size();
        o.groups_quar   = (U32)mGroupQuarantine.size();
        o.assets        = (U32)mCapturedAssets.size();
        o.face_map      = (U32)mFaceLodGroup.size();
        o.conv_trace    = (U32)mConvergeTrace.size();
        o.cap_fail      = (U32)mCaptureFail.size();
        o.unlatch       = (U32)mUnlatch.size();
        {
            // exact CPU mirror footprint (element bytes; vector slack not counted)
            U64 vb = 0, ib = 0, wb = 0;
            for (const auto& v : mMirrorVerts)   vb += (U64)v.size() * sizeof(QVertex);
            for (const auto& v : mMirrorIdx)     ib += (U64)v.size() * sizeof(U16);
            for (const auto& v : mMirrorWeights) wb += (U64)v.size() * sizeof(MirrorWeight);
            o.geom_bytes    = vb + ib + wb;
            o.geom_wt_bytes = wb;
        }
        o.mirror_rel       = (U32)mMirrorRel.size();
        o.mirror_rel_bytes = mMirrorRelBytes;
        o.mirror_wanted    = mRematWanted.load(std::memory_order_relaxed);
        o.cap_jobs      = (U32)mCapJobs.size();
        o.retired_pending = (U32)mRetired.size();
        o.retired_freed   = mRetiredFreed;
        {
            std::lock_guard<std::mutex> dl(mDecodeMutex);
            o.dec_entries = (U32)mDecodes.size();
            o.dec_bytes   = mDecodeBytes;
            o.dec_hist    = (U32)mDecodeHist.size();
            for (const auto& kv : mDecodes)
                if (kv.second.state == DECODE_FAILED) ++o.dec_failed;
        }
    }
    {
        std::lock_guard<std::mutex> rl(mGroupRefMutex);
        for (U32 r : mGroupRefs) if (r) ++o.groups_ref;
    }
    {
        std::lock_guard<std::mutex> rm(mRawMeshMutex);
        o.raw_entries       = (U32)mRawMesh.size();
        o.raw_bytes         = mRawBytes;
        o.raw_peak          = mRawPeakBytes;
        o.raw_budget        = mRawBudget.load(std::memory_order_relaxed);
        o.raw_evictions     = mRawEvictions;
        o.raw_evicted_known = (U32)mRawEvicted.size();
    }
}

std::string LLCleanMeshAsset::memStatsLine()
{
    MemStats m;
    memStats(m);
    std::ostringstream os;
    os << "meshstore slots " << m.slots_live << "/" << m.slot_ids_hw << " ids (free " << m.slot_ids_free
       << " q " << m.slot_ids_quar << ") groups " << m.groups_live << "/" << m.groups_hw << " (ref "
       << m.groups_ref << " free " << m.groups_free << " q " << m.groups_quar << ") cpu mirror "
       << (m.geom_bytes >> 20) << "MB (weights " << (m.geom_wt_bytes >> 20) << "), mirror freed " << m.mirror_rel
       << " slots (" << (m.mirror_rel_bytes >> 20) << " MB), rematerialize pending " << m.mirror_wanted << " | rawmesh " << m.raw_entries << " " << (m.raw_bytes >> 20) << "/"
       << (m.raw_budget >> 20) << "MB peak " << (m.raw_peak >> 20) << " evict " << m.raw_evictions
       << " | decode " << m.dec_entries << " (" << (m.dec_bytes >> 20) << "MB, failed " << m.dec_failed
       << ") hist " << m.dec_hist << " | retired " << m.retired_pending << " (freed " << m.retired_freed << ")";
    return os.str();
}

void LLCleanMeshAsset::logMemStatsDetailed()
{
    MemStats m;
    memStats(m);
    const MirrorRelStats rel = mirrorRelStats();
    U64 mirror_v = 0, mirror_i = 0, mirror_w = 0, pubs = 0;
    {
        std::lock_guard<std::mutex> lk(mMutex);
        for (const auto& v : mMirrorVerts)   mirror_v += (U64)v.capacity() * sizeof(QVertex);
        for (const auto& v : mMirrorIdx)     mirror_i += (U64)v.capacity() * sizeof(U16);
        for (const auto& v : mMirrorWeights) mirror_w += (U64)v.capacity() * sizeof(MirrorWeight);
        for (const auto& v : mSlotPubs)      pubs += v.size();
    }
    std::ostringstream os;
    os << "MemStats meshstore: slots live " << m.slots_live << " slot-id hw " << m.slot_ids_hw
       << " free " << m.slot_ids_free << " quarantined " << m.slot_ids_quar
       << " | mirror verts " << (mirror_v >> 20) << "MB idx " << (mirror_i >> 20) << "MB weights "
       << (mirror_w >> 20) << "MB (cluster-owned freed " << rel.released << " slots, " << (rel.bytes >> 20)
       << "MB, rematerialize pending " << rel.wanted << " stale " << rel.stale << "; session: freed "
       << rel.freed_total << " (" << (rel.freed_bytes_total >> 20) << "MB), asked back " << rel.asked_total
       << ", re-materialized " << rel.remat_total << ") | groups live " << m.groups_live << " hw " << m.groups_hw << " referenced "
       << m.groups_ref << " free " << m.groups_free << " quarantined " << m.groups_quar << " face-map "
       << m.face_map << " slot-pubs " << pubs << " | assets latched " << m.assets << " converge-trace "
       << m.conv_trace << " capture-fail " << m.cap_fail << " unlatch " << m.unlatch
       << " | geom gauge " << (m.geom_bytes >> 20) << "MB | raw mesh " << m.raw_entries << " entries "
       << (m.raw_bytes >> 20) << "MB (peak " << (m.raw_peak >> 20) << " budget " << (m.raw_budget >> 20)
       << ") evictions " << m.raw_evictions << " evicted-known " << m.raw_evicted_known
       << " | decode table " << m.dec_entries << " (" << (m.dec_bytes >> 20) << "MB ready, " << m.dec_failed
       << " failed) hist " << m.dec_hist << " capture jobs " << m.cap_jobs << " | ";
    llvkCleanIngestCacheStats(os);
    LL_INFOS("VKMem") << os.str() << LL_ENDL;
}
