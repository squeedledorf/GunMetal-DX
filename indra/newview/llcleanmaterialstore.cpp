/**
 * @file llcleanmaterialstore.cpp
 * @brief Implementation of LLCleanMaterialStore.
 */

#include "llviewerprecompiledheaders.h"

#include "llcleanmaterialstore.h"

#include "llappviewer.h" // gFrameCount
#include "llcleanassetlife.h"   // #737 release counters
#include "llcleangpuretire.h"
#include "llgl.h"
#include "llprofiler.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <sstream>

LLCleanMaterialStore& LLCleanMaterialStore::instance()
{
    static LLCleanMaterialStore s;
    return s;
}

bool LLCleanMaterialStore::init(U32 initial_capacity)
{
    // CPU-only store (the VK renderer mirrors cpuMaterialBytes()). Reserve
    // material_id 0 as the default so zero-init instance fields point at a known-safe
    // entry. Idempotent.
    if (mCapacity >= initial_capacity) return true;
    {
        std::lock_guard<std::mutex> lk(mMutex);
        if (!grow(initial_capacity)) return false;
    }
    MaterialDesc d;
    pin(getOrCreate(d));   // id 0 = default white, never swept
    return true;
}

bool LLCleanMaterialStore::grow(U32 new_cap)
{
    if (new_cap <= mCapacity) return true;
    mMaterials.resize(new_cap);
    mMaterialUV.resize(new_cap);
    mResolveState.resize(new_cap);
    mDescs.resize(new_cap);
    mHashOf.resize(new_cap, 0);
    mRefs.resize(new_cap, 0);
    mTouch.resize(new_cap, 0);
    mState.resize(new_cap, 0);
    mInCand.resize(new_cap, 0);

    mCapacity = new_cap;
    ++mGeneration;
    return true;
}

void LLCleanMaterialStore::shutdown()
{
    std::lock_guard<std::mutex> lk(mMutex);
    mCapacity = mLiveCount = mHighWater = 0;
    mMaterials.clear();
    mMaterialUV.clear();
    ++mGeneration;
    mResolveState.clear();
    mDescs.clear();
    mHashToIds.clear();
    mHashOf.clear(); mRefs.clear(); mTouch.clear(); mState.clear(); mInCand.clear();
    mCandidates.clear(); mPending.clear(); mFree.clear();
    mRecycled.clear(); mRecycledAll = true;
    mTerrain.clear();
    mTerrainIndex.clear();
    mTerrainKey.clear(); mTerrainRefs.clear(); mTerrainTouch.clear(); mTerrainState.clear();
    mTerrainFree.clear(); mTerrainPending.clear();
}

// Stable hash over the descriptor's identifying fields. UUIDs +
// quantised factor bytes -- two descs that produce identical GPU
// entries collapse to one material_id.
U64 LLCleanMaterialStore::hashDesc(const MaterialDesc& d)
{
    auto mix = [](U64 a, U64 b) -> U64
    {
        // splitmix64-ish
        a ^= b + 0x9e3779b97f4a7c15ull + (a << 6) + (a >> 2);
        return a;
    };
    // Full 16-byte UUIDs through a splitmix64 finalizer (getCRC32 is a plain sum of
    // the four words -- weak, and collisions were never detected).
    auto fin = [](U64 x) -> U64
    {
        x += 0x9e3779b97f4a7c15ull;
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
        x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
        return x ^ (x >> 31);
    };
    auto uuid = [&](U64 h0, const LLUUID& id) -> U64
    {
        U64 lo, hi;
        std::memcpy(&lo, id.mData, 8);
        std::memcpy(&hi, id.mData + 8, 8);
        return fin(h0 ^ fin(lo) ^ (fin(hi) * 0x9e3779b97f4a7c15ull));
    };
    U64 h = 0;
    h = uuid(h, d.base_color_id);
    h = uuid(h, d.normal_id);
    h = uuid(h, d.mr_id);
    h = uuid(h, d.emissive_id);
    // Factor floats: bit-reinterpret to U32 then fold.
    auto fbits = [](F32 f) -> U64 { U32 u; std::memcpy(&u, &f, 4); return u; };
    for (int i = 0; i < 4; ++i) h = mix(h, fbits(d.base_color_factor[i]));
    for (int i = 0; i < 4; ++i) h = mix(h, fbits(d.emissive_factor[i]));
    h = mix(h, fbits(d.metallic_factor));
    h = mix(h, fbits(d.roughness_factor));
    h = mix(h, fbits(d.alpha_cutoff));
    for (int i = 0; i < 4; ++i) h = mix(h, fbits(d.spec_color_gloss[i]));
    h = mix(h, fbits(d.env_intensity));
    h = mix(h, d.legacy ? 0x4C454741ull : 0ull);
    h = mix(h, d.color_all_slots ? 0x434F4C52ull : 0ull);
    h = mix(h, d.gltf ? 0x474C5446ull : 0ull);
    h = mix(h, d.emissive_mask ? 0x454D4D53ull : 0ull);
    h = mix(h, d.hud ? 0x48554421ull : 0ull);
    h = mix(h, d.alpha_blend ? 0x424C454Eull : 0ull);
    h = mix(h, 0x42554D50ull ^ (U64)(d.bump & 31u));
    h = mix(h, (U64)(U32)(llclamp(d.glow, 0.f, 1.f) * 255.f + 0.5f));
    for (int k = 0; k < 3; ++k)
    {
        h = mix(h, d.uv_own[k] ? 0x55564F57ull : 0ull);
        if (d.uv_own[k])
            for (int i = 0; i < 6; ++i) h = mix(h, fbits(d.uv_xform[k][i]));
    }
    return h;
}

// static
bool LLCleanMaterialStore::descEqual(const MaterialDesc& a, const MaterialDesc& b)
{
    auto feq = [](const F32* x, const F32* y, int n) { return std::memcmp(x, y, sizeof(F32) * n) == 0; };
    if (a.base_color_id != b.base_color_id || a.normal_id != b.normal_id ||
        a.mr_id != b.mr_id || a.emissive_id != b.emissive_id) return false;
    if (!feq(a.base_color_factor, b.base_color_factor, 4) || !feq(a.emissive_factor, b.emissive_factor, 4) ||
        !feq(&a.metallic_factor, &b.metallic_factor, 1) || !feq(&a.roughness_factor, &b.roughness_factor, 1) ||
        !feq(&a.alpha_cutoff, &b.alpha_cutoff, 1) || !feq(a.spec_color_gloss, b.spec_color_gloss, 4) ||
        !feq(&a.env_intensity, &b.env_intensity, 1)) return false;
    if (a.legacy != b.legacy || a.color_all_slots != b.color_all_slots || a.gltf != b.gltf
        || a.emissive_mask != b.emissive_mask || a.hud != b.hud || a.alpha_blend != b.alpha_blend
        || (a.bump & 31u) != (b.bump & 31u)) return false;
    // Glow is stored quantized (8 bits), as in the hash.
    if ((U32)(llclamp(a.glow, 0.f, 1.f) * 255.f + 0.5f) != (U32)(llclamp(b.glow, 0.f, 1.f) * 255.f + 0.5f)) return false;
    for (int k = 0; k < 3; ++k)
    {
        if (a.uv_own[k] != b.uv_own[k]) return false;
        if (a.uv_own[k] && !feq(a.uv_xform[k], b.uv_xform[k], 6)) return false;
    }
    return true;
}

U32 LLCleanMaterialStore::getOrCreate(const MaterialDesc& desc)
{
    const U64 h = hashDesc(desc);
    const U32 now = (U32)gFrameCount;
    std::lock_guard<std::mutex> lk(mMutex);
    // Hits are confirmed field-by-field (a genuine 64-bit collision gets its own id in
    // the same bucket).
    auto bit = mHashToIds.find(h);
    if (bit != mHashToIds.end())
    {
        for (U32 cand : bit->second)
        {
            if (cand < mHighWater && mState[cand] == 1 && descEqual(mDescs[cand], desc))
            {
                mTouch[cand] = now;
                return cand;
            }
        }
    }

    // A retired id (lowest first), else a fresh one past the high-water mark.
    U32 id;
    bool reused = false;
    if (!mFree.empty())
    {
        id = *mFree.begin();
        mFree.erase(mFree.begin());
        reused = true;
    }
    else
    {
        if (mHighWater >= mCapacity)
        {
            grow(mCapacity == 0 ? 16384 : mCapacity * 2);
        }
        id = mHighWater++;
    }
    ++mLiveCount;
    mHashToIds[h].push_back(id);
    mHashOf[id] = h;
    mState[id] = 1;
    mRefs[id] = 0;
    mTouch[id] = now;
    addCandidateLocked(id);   // no holder yet: swept unless an instance row picks it up
    mDescs[id] = desc;
    if (reused)
    {
        ++mReusedTotal;
        // The VK mirror keys per-id resolve state (texture handles, settled flags) on the
        // id: tell it this one now holds different content.
        if (mRecycled.size() >= 65536u) { mRecycled.clear(); mRecycledAll = true; }
        if (!mRecycledAll) mRecycled.push_back(id);
    }

    // Track the desc UUIDs: the renderer resolves them to its texture indices
    // (materialTextureIds). mResolveState is sized to match mCapacity in grow().
    ResolveState& rs = mResolveState[id];
    rs.base_color_id = desc.base_color_id;
    rs.normal_id     = desc.normal_id;
    rs.mr_id         = desc.mr_id;
    rs.emissive_id   = desc.emissive_id;

    Material& m = mMaterials[id];
    std::memset(&m, 0, sizeof(m));

    MaterialUV& muv = mMaterialUV[id];
    for (int k = 0; k < 3; ++k)
    {
        const F32* x = desc.uv_xform[k];
        muv.rows[k][0][0] = x[0]; muv.rows[k][0][1] = x[1]; muv.rows[k][0][2] = x[2];
        muv.rows[k][0][3] = desc.uv_own[k] ? 1.f : 0.f;
        muv.rows[k][1][0] = x[3]; muv.rows[k][1][1] = x[4]; muv.rows[k][1][2] = x[5];
        muv.rows[k][1][3] = 0.f;
    }

    // The *_tex fields stay 0 (memset above): the renderer writes its texture indices
    // and the has-map flag bits 0-3 when it resolves the UUIDs in mResolveState.

    std::memcpy(m.base_color_factor, desc.base_color_factor, sizeof(m.base_color_factor));
    std::memcpy(m.emissive_factor,   desc.emissive_factor,   sizeof(m.emissive_factor));
    std::memcpy(m.spec_color_gloss,  desc.spec_color_gloss,  sizeof(m.spec_color_gloss));
    m.metallic_factor  = desc.metallic_factor;
    m.roughness_factor = desc.roughness_factor;
    m.alpha_cutoff     = desc.alpha_cutoff;
    if (desc.legacy)
    {
        // Legacy mats never use emissive_factor; its .a slot carries
        // the environment intensity to the FS (struct stays 96 B).
        m.emissive_factor[3] = desc.env_intensity;
    }
    m.flags = (desc.legacy         ? 16u : 0u)
            | (desc.color_all_slots ? 32u : 0u)
            | (desc.gltf ? 64u : 0u)
            | (desc.emissive_mask ? 128u : 0u)
            | (desc.hud ? 65536u : 0u)
            | (desc.alpha_blend ? (1u << 22) : 0u)
            | ((U32)(desc.bump & 31u) << 17)
            | (((U32)(llclamp(desc.glow, 0.f, 1.f) * 255.f + 0.5f) & 0xFFu) << 8);

    ++mGeneration;
    return id;
}

U32 LLCleanMaterialStore::setTerrainRecord(U64 region_key, const TerrainRecord& rec)
{
    std::lock_guard<std::mutex> lk(mMutex);
    const U32 now = (U32)gFrameCount;
    auto it = mTerrainIndex.find(region_key);
    if (it == mTerrainIndex.end())
    {
        U32 idx;
        if (!mTerrainFree.empty())
        {
            idx = *mTerrainFree.begin();
            mTerrainFree.erase(mTerrainFree.begin());
            mTerrain[idx] = rec;
        }
        else
        {
            idx = (U32)mTerrain.size();
            mTerrain.push_back(rec);
            mTerrainKey.push_back(0);
            mTerrainRefs.push_back(0);
            mTerrainTouch.push_back(0);
            mTerrainState.push_back(0);
        }
        mTerrainKey[idx] = region_key;
        mTerrainRefs[idx] = 0;
        mTerrainTouch[idx] = now;
        mTerrainState[idx] = 1;
        for (U32 m : rec.mat) addRefLocked(m);   // the record holds its materials
        mTerrainIndex[region_key] = idx;
        ++mGeneration;   // the VK mirror copies records with the materials
        return idx;
    }
    const U32 idx = it->second;
    mTerrainTouch[idx] = now;
    TerrainRecord& cur = mTerrain[idx];
    if (std::memcmp(&cur, &rec, sizeof(TerrainRecord)) != 0)
    {
        for (U32 m : rec.mat) addRefLocked(m);   // new holds first: a shared id never dips to 0
        for (U32 m : cur.mat) releaseLocked(m);
        cur = rec;
        ++mGeneration;
    }
    return idx;
}

U32 LLCleanMaterialStore::findTerrainRecord(U64 region_key) const
{
    std::lock_guard<std::mutex> lk(mMutex);
    auto it = mTerrainIndex.find(region_key);
    return it == mTerrainIndex.end() ? INVALID : it->second;
}

// ---- lifetime -------------------------------------------------------------------------

void LLCleanMaterialStore::addCandidateLocked(U32 id)
{
    if (id < mInCand.size() && !mInCand[id])
    {
        mInCand[id] = 1;
        mCandidates.push_back(id);
    }
}

void LLCleanMaterialStore::addRefLocked(U32 id)
{
    if (id >= mHighWater) return;
    if (mState[id] != 1)
    {
        // A holder naming an id the sweep already freed (a cached id that outlived its
        // grace). Its content is intact until reuse: resurrect it so refcounts stay
        // balanced instead of counting on a free slot.
        if (mState[id] == 2)
        {
            for (size_t i = 0; i < mPending.size(); ++i)
                if (mPending[i].first == id) { mPending[i] = mPending.back(); mPending.pop_back(); break; }
        }
        else
        {
            mFree.erase(id);
        }
        mState[id] = 1;
        mHashToIds[mHashOf[id]].push_back(id);
        ++mLiveCount;
        LL_WARNS_ONCE("CleanMat") << "material " << id << " referenced after it was swept (stale cached id); resurrected" << LL_ENDL;
    }
    ++mRefs[id];
}

void LLCleanMaterialStore::releaseLocked(U32 id)
{
    if (id >= mHighWater) return;
    if (mRefs[id] == 0)
    {
        LL_WARNS_ONCE("CleanMat") << "material " << id << " released with no references (refcount underflow ignored)" << LL_ENDL;
        return;
    }
    if (--mRefs[id] == 0)
    {
        // #737: the grace counts from the last holder letting go, not from the entry's
        // creation -- a material used for an hour is still kept the full grace after its
        // last instance leaves (a quick return reuses it).
        mTouch[id] = (U32)gFrameCount;
        addCandidateLocked(id);
    }
}

void LLCleanMaterialStore::trecAddRefLocked(U32 idx)
{
    if (idx < mTerrainRefs.size() && mTerrainState[idx] == 1) ++mTerrainRefs[idx];
}

void LLCleanMaterialStore::trecReleaseLocked(U32 idx)
{
    if (idx < mTerrainRefs.size() && mTerrainRefs[idx] > 0 && --mTerrainRefs[idx] == 0)
        mTerrainTouch[idx] = (U32)gFrameCount;   // #737: grace from the last holder leaving
}

void LLCleanMaterialStore::unkeyLocked(U32 id)
{
    auto bit = mHashToIds.find(mHashOf[id]);
    if (bit == mHashToIds.end()) return;
    std::vector<U32>& v = bit->second;
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i] == id) { v[i] = v.back(); v.pop_back(); break; }
    if (v.empty()) mHashToIds.erase(bit);
}

void LLCleanMaterialStore::retarget(U32 old_mat, U32 new_mat, U32 old_trec, U32 new_trec)
{
    std::lock_guard<std::mutex> lk(mMutex);
    // New references first: a holder moving between ids never lets a shared one dip to 0.
    if (new_mat != INVALID) addRefLocked(new_mat);
    if (new_trec != INVALID) trecAddRefLocked(new_trec);
    if (old_mat != INVALID) releaseLocked(old_mat);
    if (old_trec != INVALID) trecReleaseLocked(old_trec);
}

void LLCleanMaterialStore::pin(U32 id)
{
    std::lock_guard<std::mutex> lk(mMutex);
    addRefLocked(id);
}

void LLCleanMaterialStore::tick(U32 grace_frames)
{
    const U32 grace = std::max(grace_frames, kSweepMinGraceFrames);
    std::lock_guard<std::mutex> lk(mMutex);
    // Retire pending frees whose GPU frames have completed (cheap: usually empty).
    for (size_t i = 0; i < mPending.size(); )
    {
        if (LLCleanGpuRetire::retired(mPending[i].second))
        {
            const U32 id = mPending[i].first;
            mState[id] = 0;
            mFree.insert(id);
            mPending[i] = mPending.back();
            mPending.pop_back();
        }
        else ++i;
    }
    for (size_t i = 0; i < mTerrainPending.size(); )
    {
        if (LLCleanGpuRetire::retired(mTerrainPending[i].second))
        {
            const U32 idx = mTerrainPending[i].first;
            mTerrainState[idx] = 0;
            mTerrainFree.insert(idx);
            mTerrainPending[i] = mTerrainPending.back();
            mTerrainPending.pop_back();
        }
        else ++i;
    }

    const U32 now = (U32)gFrameCount;
    if (now - mLastSweep < kSweepEveryFrames) return;
    mLastSweep = now;
    const U64 stamp = LLCleanGpuRetire::stamp();

    // Terrain records first: a freed record drops its material refs, which the material
    // pass below may then free (after their own grace).
    for (U32 idx = 0; idx < (U32)mTerrain.size(); ++idx)
    {
        if (mTerrainState[idx] != 1 || mTerrainRefs[idx] != 0) continue;
        if (now - mTerrainTouch[idx] < grace) continue;
        for (U32 m : mTerrain[idx].mat) releaseLocked(m);
        mTerrainIndex.erase(mTerrainKey[idx]);
        mTerrain[idx] = TerrainRecord{};
        mTerrainState[idx] = 2;
        mTerrainPending.emplace_back(idx, stamp);
        ++mTerrainFreedTotal;
        LLCleanAssetLife::add(LLCleanAssetLife::TERRAIN_RECORDS, 1);
        ++mGeneration;
    }

    // Materials: only ids whose refcount reached 0 are visited.
    size_t keep = 0;
    for (size_t i = 0; i < mCandidates.size(); ++i)
    {
        const U32 id = mCandidates[i];
        if (id >= mHighWater || mState[id] != 1 || mRefs[id] != 0) { mInCand[id] = 0; continue; }
        if (now - mTouch[id] < grace) { mCandidates[keep++] = id; continue; }
        // Free: drop the dedup key (a new request for this desc allocates afresh) and
        // park the id until the frames that may still read it have retired. Its bytes
        // stay as they are (no live row names it; the VK resolver sees it dead).
        mInCand[id] = 0;
        unkeyLocked(id);
        mState[id] = 2;
        mPending.emplace_back(id, stamp);
        --mLiveCount;
        ++mFreedTotal;
        LLCleanAssetLife::add(LLCleanAssetLife::MATERIALS, 1);
    }
    mCandidates.resize(keep);
}

void LLCleanMaterialStore::takeRecycled(std::vector<U32>& out, bool& all_out)
{
    std::lock_guard<std::mutex> lk(mMutex);
    out.swap(mRecycled);
    mRecycled.clear();
    all_out = mRecycledAll;
    mRecycledAll = false;
}

bool LLCleanMaterialStore::hasRecycled() const
{
    std::lock_guard<std::mutex> lk(mMutex);
    return mRecycledAll || !mRecycled.empty();
}

LLCleanMaterialStore::Stats LLCleanMaterialStore::stats() const
{
    std::lock_guard<std::mutex> lk(mMutex);
    Stats s;
    s.live = mLiveCount;
    s.high_water = mHighWater;
    s.capacity = mCapacity;
    s.free = (U32)mFree.size();
    s.pending = (U32)mPending.size();
    s.candidates = (U32)mCandidates.size();
    s.terrain_slots = (U32)mTerrain.size();
    s.terrain_free = (U32)mTerrainFree.size();
    for (U8 st : mTerrainState) if (st == 1) ++s.terrain_live;
    s.freed_total = mFreedTotal;
    s.reused_total = mReusedTotal;
    s.terrain_freed_total = mTerrainFreedTotal;
    const U64 per_id = sizeof(Material) + sizeof(MaterialUV) + sizeof(ResolveState) + sizeof(MaterialDesc)
                     + sizeof(U64) + 2 * sizeof(U32) + 2 * sizeof(U8);
    s.cpu_bytes = (U64)mCapacity * per_id
                + (U64)mTerrain.capacity() * (sizeof(TerrainRecord) + sizeof(U64) + 2 * sizeof(U32) + 1)
                + (U64)mHashToIds.size() * (sizeof(U64) + sizeof(std::vector<U32>) + 32);
    return s;
}

bool LLCleanMaterialStore::materialTextureIds(U32 id, LLUUID out[4], F32* emissive_rgb, F32* base_rgba) const
{
    std::lock_guard<std::mutex> lk(mMutex);
    if (id >= mHighWater || mState[id] != 1) return false;
    const ResolveState& rs = mResolveState[id];
    out[0] = rs.base_color_id;
    out[1] = rs.normal_id;
    out[2] = rs.mr_id;
    out[3] = rs.emissive_id;
    if (emissive_rgb) std::memcpy(emissive_rgb, mMaterials[id].emissive_factor, 3 * sizeof(F32));
    if (base_rgba) std::memcpy(base_rgba, mMaterials[id].base_color_factor, 4 * sizeof(F32));
    return true;
}

std::string LLCleanMaterialStore::debugDescDiff(U32 a, U32 b) const
{
    std::lock_guard<std::mutex> lk(mMutex);
    auto ok = [&](U32 id) { return id < mHighWater && id < (U32)mDescs.size() && mState[id] == 1; };
    if (!ok(a) || !ok(b)) return std::string("(not live: ") + (ok(a) ? "" : "old ") + (ok(b) ? "" : "new") + ")";
    const MaterialDesc& x = mDescs[a];
    const MaterialDesc& y = mDescs[b];
    std::ostringstream os;
    auto id = [&](const char* n, const LLUUID& p, const LLUUID& q)
    { if (p != q) os << n << ' ' << p.asString().substr(0, 8) << "->" << q.asString().substr(0, 8) << ' '; };
    auto f = [&](const char* n, const F32* p, const F32* q, int c)
    {
        if (std::memcmp(p, q, sizeof(F32) * c) == 0) return;
        os << n << ' ';
        for (int k = 0; k < c; ++k) os << (k ? "," : "") << p[k];
        os << "->";
        for (int k = 0; k < c; ++k) os << (k ? "," : "") << q[k];
        os << ' ';
    };
    id("base", x.base_color_id, y.base_color_id);
    id("normal", x.normal_id, y.normal_id);
    id("mr", x.mr_id, y.mr_id);
    id("emissive", x.emissive_id, y.emissive_id);
    f("base_factor", x.base_color_factor, y.base_color_factor, 4);
    f("emis_factor", x.emissive_factor, y.emissive_factor, 4);
    f("metal", &x.metallic_factor, &y.metallic_factor, 1);
    f("rough", &x.roughness_factor, &y.roughness_factor, 1);
    f("alpha_cutoff", &x.alpha_cutoff, &y.alpha_cutoff, 1);
    f("spec", x.spec_color_gloss, y.spec_color_gloss, 4);
    f("env", &x.env_intensity, &y.env_intensity, 1);
    f("glow", &x.glow, &y.glow, 1);
    if (x.legacy != y.legacy) os << "legacy " << x.legacy << "->" << y.legacy << ' ';
    if (x.alpha_blend != y.alpha_blend) os << "alpha_blend " << x.alpha_blend << "->" << y.alpha_blend << ' ';
    if (x.gltf != y.gltf) os << "gltf " << x.gltf << "->" << y.gltf << ' ';
    if (x.color_all_slots != y.color_all_slots) os << "color_all " << x.color_all_slots << "->" << y.color_all_slots << ' ';
    for (int k = 0; k < 3; ++k)
    {
        if (x.uv_own[k] != y.uv_own[k]) os << "uv_own" << k << ' ' << x.uv_own[k] << "->" << y.uv_own[k] << ' ';
        else if (x.uv_own[k]) f("uv_xform", x.uv_xform[k], y.uv_xform[k], 6);
    }
    const std::string s = os.str();
    return s.empty() ? std::string("(same desc)") : s;
}

bool LLCleanMaterialStore::shadowEquivalent(U32 a, U32 b) const
{
    if (a == b) return true;
    std::lock_guard<std::mutex> lk(mMutex);
    auto ok = [&](U32 id) { return id < mHighWater && id < (U32)mDescs.size() && mState[id] == 1; };
    if (!ok(a) || !ok(b)) return false;
    const MaterialDesc& x = mDescs[a];
    const MaterialDesc& y = mDescs[b];
    return x.base_color_id == y.base_color_id
        && std::memcmp(x.base_color_factor, y.base_color_factor, sizeof(x.base_color_factor)) == 0
        && x.alpha_cutoff == y.alpha_cutoff
        && x.legacy == y.legacy && x.gltf == y.gltf && x.emissive_mask == y.emissive_mask && x.hud == y.hud
        && x.color_all_slots == y.color_all_slots;
}

bool LLCleanMaterialStore::debugGet(U32 material_id, Material& out, LLUUID& base_id_out) const
{
    std::lock_guard<std::mutex> lk(mMutex);
    if (material_id >= mHighWater || mState[material_id] != 1) return false;
    out = mMaterials[material_id];
    base_id_out = mResolveState[material_id].base_color_id;
    return true;
}

