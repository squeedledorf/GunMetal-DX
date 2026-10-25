/**
 * @file llcleanrigshadowlod.cpp
 * @brief Shadow-caster LOD for rigged mesh (#564). See the header.
 */

#include "llviewerprecompiledheaders.h"

#include "llcleanrigshadowlod.h"

#include "llcleanmeshasset.h"
#include "llcleanrigdecimate.h"
#include "llthread.h"

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <sstream>

namespace
{
constexpr U32 kFloats     = 6;         // match key words: quantized pos3 + uv2 (3 words) + 10 B weights (#573, #582)
constexpr U32 kMaxLive    = LLCleanRigShadowLod::kTableCap / 2;   // load factor <= 0.5
constexpr U32 kAgeFrames  = 1800;      // unused this long (~30 s): freed
constexpr U32 kRetryFrames = 3600;     // a failed match is not retried for ~1 min (same signature)
constexpr U64 kLaunchVerts = 262144;   // mirror vertices copied per frame (render thread)
constexpr U32 kQueueMax   = 4096;
constexpr U64 kUploadWords = 1u << 20;  // indices committed per frame (2 MiB of vkCmdUpdateBuffer)

// 16-bit index ranges are allocated in even counts: vkCmdUpdateBuffer needs 4-byte
// aligned offsets and sizes (#573).
inline U32 evenUp(U32 n) { return (n + 1u) & ~1u; }

// The matched attributes of one vertex: the QUANTIZED position and UV lattice indices
// (#573: a decimated step is quantized against its source's lattice, so a copied vertex
// has identical indices) and the 10 B joint / weight record (#582; a step's weights are
// byte copies of its source's, so they match exactly). The normal is left out: the
// octahedral re-encode of a decoded normal may differ by an LSB, and a shadow caster
// never reads it (duplicates differing only by normal: the first wins, harmless here).
void packVertex(const LLCleanMeshAsset::QVertex& v, const LLCleanMeshAsset::MirrorWeight& w, U32* out)
{
    out[0] = (U32)(U16)v.pos[0] | ((U32)(U16)v.pos[1] << 16);
    out[1] = (U32)(U16)v.pos[2];
    out[2] = (U32)(U16)v.uv[0] | ((U32)(U16)v.uv[1] << 16);
    out[3] = out[4] = out[5] = 0u;   // (10 B in 3 words: the pad half must be deterministic)
    static_assert(sizeof(LLCleanMeshAsset::MirrorWeight) <= sizeof(U32) * 3, "weight record must fit the key's 3 words");
    std::memcpy(out + 3, &w, sizeof(LLCleanMeshAsset::MirrorWeight));
}

struct VKey
{
    std::array<U32, kFloats> b;
    bool operator==(const VKey& o) const { return b == o.b; }
};
struct VKeyHash
{
    size_t operator()(const VKey& k) const
    {
        U64 h = 1469598103934665603ull;
        for (U32 x : k.b) { h ^= x; h *= 1099511628211ull; }
        return (size_t)(h ^ (h >> 29));
    }
};
VKey keyOf(const U32* f)
{
    VKey k;
    std::memcpy(k.b.data(), f, sizeof(U32) * kFloats);
    return k;
}
} // namespace

LLCleanRigShadowLod& LLCleanRigShadowLod::instance()
{
    static LLCleanRigShadowLod s;
    return s;
}

LLCleanRigShadowLod::LLCleanRigShadowLod()
{
    mTable.assign((size_t)kTableCap * kEntryWords, 0u);
    for (U32 e = 0; e < kTableCap; ++e) mTable[(size_t)e * kEntryWords] = kEmpty;
}

LLCleanRigShadowLod::~LLCleanRigShadowLod()
{
    stop();
}

void LLCleanRigShadowLod::stop()
{
    {
        std::lock_guard<std::mutex> lk(mMutex);
        mStopped = true;
    }
    mCv.notify_all();
    if (mThread.joinable()) mThread.join();
}

void LLCleanRigShadowLod::reset()
{
    {
        std::lock_guard<std::mutex> lk(mMutex);
        mJobsIn.clear();
        mJobsOut.clear();   // (a running job's result is discarded: no pending entry)
    }
    mInFlight = 0;
    mLive.clear();
    mPending.clear();
    mQueue.clear();
    mUploads.clear();
    for (U32 e = 0; e < kTableCap; ++e)
    {
        std::fill_n(mTable.begin() + (size_t)e * kEntryWords, kEntryWords, 0u);
        mTable[(size_t)e * kEntryWords] = kEmpty;
    }
    mTombs = 0;
    mTableFull = true;
    mFree.clear();
    if (mCapWords) mFree[0] = mCapWords;
    mUsedWords = 0;
}

// ---------------------------------------------------------------------------
// Worker: source-numbered step indices by exact vertex matching.
// ---------------------------------------------------------------------------
void LLCleanRigShadowLod::match(Job& job)
{
    const size_t nv = job.src.size() / kFloats;
    if (nv == 0) { job.ok = false; return; }
    std::unordered_map<VKey, U32, VKeyHash> idx;
    idx.reserve(nv * 2);
    for (size_t v = 0; v < nv; ++v) idx.emplace(keyOf(&job.src[v * kFloats]), (U32)v);   // (duplicates: the first; identical data skins identically)
    for (U32 s = 0; s < kSteps; ++s)
    {
        const size_t sv = job.step_v[s].size() / kFloats;
        if (sv == 0 || job.step_i[s].empty()) break;
        std::vector<U32> remap(sv);
        bool ok = true;
        for (size_t v = 0; v < sv && ok; ++v)
        {
            auto it = idx.find(keyOf(&job.step_v[s][v * kFloats]));
            if (it == idx.end()) ok = false;
            else remap[v] = it->second;
        }
        if (!ok) break;   // not a vertex subset (should not happen): this and coarser steps out
        std::vector<U32>& out = job.out[s];
        out.resize(job.step_i[s].size());
        for (size_t i = 0; i < out.size() && ok; ++i)
        {
            const U32 k = job.step_i[s][i];
            if (k >= sv) ok = false;
            else out[i] = remap[k];
        }
        if (!ok) { out.clear(); break; }
    }
    job.ok = !job.out[0].empty();
    // (inputs are not needed any more)
    job.src.clear(); job.src.shrink_to_fit();
    for (U32 s = 0; s < kSteps; ++s)
    {
        job.step_v[s].clear(); job.step_v[s].shrink_to_fit();
        job.step_i[s].clear(); job.step_i[s].shrink_to_fit();
    }
}

void LLCleanRigShadowLod::workerMain()
{
    ll_set_current_thread_name("CleanRigShadowLod");   // #736
    for (;;)
    {
        std::unique_ptr<Job> job;
        {
            std::unique_lock<std::mutex> lk(mMutex);
            mCv.wait(lk, [&] { return mStopped || !mJobsIn.empty(); });
            if (mStopped) return;
            job = std::move(mJobsIn.back());
            mJobsIn.pop_back();
        }
        match(*job);
        std::lock_guard<std::mutex> lk(mMutex);
        mJobsOut.push_back(std::move(job));
    }
}

// ---------------------------------------------------------------------------
// Render thread
// ---------------------------------------------------------------------------
bool LLCleanRigShadowLod::want(U32 slot, const SlotRow& row, U32 frame)
{
    if (mStopped || mCapWords == 0 || row.index_count == 0) return false;
    auto lit = mLive.find(slot);
    if (lit != mLive.end())
    {
        Entry& e = lit->second;
        if (e.first_index == row.first_index && e.ic == row.index_count && e.vc == row.vert_count)
        {
            e.last_use = frame;
            return true;
        }
        freeEntry(e);
        mLive.erase(lit);
    }
    auto pit = mPending.find(slot);
    if (pit != mPending.end())
    {
        Pending& p = pit->second;
        const bool same = p.ic == row.index_count && p.vc == row.vert_count;
        if (p.state != ST_FAILED && same) return true;
        if (p.state == ST_FAILED && same && frame - p.frame < kRetryFrames) return false;
        mPending.erase(pit);   // other content (recycled slot) or retry
    }
    const LLCleanRigDecimator::Info* in = LLCleanRigDecimator::instance().find(slot);
    if (!in || in->src_ic != row.index_count || in->src_vc != row.vert_count
        || in->dec[0] == LLCleanRigDecimator::kEmpty || mQueue.size() >= kQueueMax)
        return false;
    mPending[slot] = Pending{ST_QUEUED, 0, frame, row.index_count, row.vert_count};
    mQueue.push_back(slot);
    return true;
}

void LLCleanRigShadowLod::noteDirty(const std::vector<U32>& slots)
{
    for (U32 sl : slots)
    {
        auto lit = mLive.find(sl);
        if (lit != mLive.end()) { freeEntry(lit->second); mLive.erase(lit); }
        mPending.erase(sl);   // (a running job's result is discarded on return)
    }
}

bool LLCleanRigShadowLod::alloc(U32 words, U32& first)
{
    for (auto it = mFree.begin(); it != mFree.end(); ++it)
    {
        if (it->second < words) continue;
        first = it->first;
        const U32 rest = it->second - words;
        mFree.erase(it);
        if (rest) mFree[first + words] = rest;
        mUsedWords += words;
        mPeakWords = std::max(mPeakWords, mUsedWords);
        return true;
    }
    return false;
}

void LLCleanRigShadowLod::freeRange(U32 first, U32 words)
{
    if (!words) return;
    mUsedWords -= std::min<U64>(mUsedWords, words);
    auto next = mFree.lower_bound(first);
    if (next != mFree.end() && first + words == next->first)
    {
        words += next->second;
        next = mFree.erase(next);
    }
    if (next != mFree.begin())
    {
        auto prev = std::prev(next);
        if (prev->first + prev->second == first) { prev->second += words; return; }
    }
    mFree[first] = words;
}

void LLCleanRigShadowLod::freeEntry(Entry& e)
{
    if (e.table != kEmpty) tableRemove(e.table);
    e.table = kEmpty;
    for (U32 s = 0; s < e.nsteps; ++s) freeRange(e.range_first[s], evenUp(e.range_count[s]));
    e.nsteps = 0;
}

U32 LLCleanRigShadowLod::hashKey(U32 key) const
{
    static_assert((kTableCap & (kTableCap - 1u)) == 0u, "table capacity must be a power of two");
    U32 bits = 0;
    while ((1u << bits) < kTableCap) ++bits;
    return (key * 2654435761u) >> (32u - bits);
}

void LLCleanRigShadowLod::writeEntryWords(U32 t, const Entry* e)
{
    U32* w = &mTable[(size_t)t * kEntryWords];
    if (!e)
    {
        std::fill_n(w, kEntryWords, 0u);
        w[0] = kTomb;
    }
    else
    {
        auto h = [](F32 x) { return (U32)glm::packHalf1x16(std::min(std::max(x, 0.f), 65000.f)); };
        w[0] = e->first_index; w[1] = e->ic; w[2] = e->vc;
        w[3] = e->nsteps | (e->nsteps > 2 ? h(e->err[2]) << 16 : 0u);
        for (U32 s = 0; s < kSteps; ++s)
        {
            w[4 + s] = s < e->nsteps ? e->range_first[s] : 0u;
            w[8 + s] = s < e->nsteps ? e->range_count[s] : 0u;
        }
        w[7] = 0u;
        w[11] = (e->nsteps > 0 ? h(e->err[0]) : 0u) | ((e->nsteps > 1 ? h(e->err[1]) : 0u) << 16);
    }
    if (!mTableFull) mDirty.push_back(t);
}

U32 LLCleanRigShadowLod::tableInsert(const Entry& e)
{
    const U32 h = hashKey(e.first_index);
    for (U32 k = 0; k < kProbes; ++k)
    {
        const U32 t = (h + k) & (kTableCap - 1u);
        const U32 key = mTable[(size_t)t * kEntryWords];
        if (key == kEmpty || key == kTomb)
        {
            if (key == kTomb && mTombs) --mTombs;
            writeEntryWords(t, &e);
            return t;
        }
    }
    return kEmpty;   // probe chain full: not drawn coarser (the cull finds nothing)
}

void LLCleanRigShadowLod::tableRemove(U32 t)
{
    if (t >= kTableCap) return;
    writeEntryWords(t, nullptr);
    ++mTombs;
}

void LLCleanRigShadowLod::tableRebuild()
{
    for (U32 e = 0; e < kTableCap; ++e)
    {
        std::fill_n(mTable.begin() + (size_t)e * kEntryWords, kEntryWords, 0u);
        mTable[(size_t)e * kEntryWords] = kEmpty;
    }
    mTombs = 0;
    mTableFull = true;
    mDirty.clear();
    for (auto& kv : mLive) kv.second.table = tableInsert(kv.second);
}

bool LLCleanRigShadowLod::takeTableDirty(std::vector<U32>& entries, bool& full)
{
    full = mTableFull;
    entries.clear();
    if (mTableFull)
    {
        mTableFull = false;
        mDirty.clear();
        return true;
    }
    if (mDirty.empty()) return false;
    std::sort(mDirty.begin(), mDirty.end());
    mDirty.erase(std::unique(mDirty.begin(), mDirty.end()), mDirty.end());
    entries.swap(mDirty);
    mDirty.clear();
    return true;
}

void LLCleanRigShadowLod::pump(U32 frame, const SlotRow* rows, size_t nrows, U32 capacity_words)
{
    if (mStopped) return;
    if (capacity_words != mCapWords)
    {
        mCapWords = capacity_words;
        reset();   // (the index buffer was re-created: every range is gone)
    }
    if (mCapWords == 0) return;
    auto rowOk = [&](U32 slot, U32 ic, U32 vc) {
        return slot < nrows && rows[slot].index_count == ic && rows[slot].vert_count == vc && ic != 0;
    };

    // 1. Live entries: freed / other content -> drop (a rewrite also comes through
    // noteDirty); unused -> age out; MOVED (the renderer re-placed the unchanged slot, e.g.
    // a full geometry repack) -> re-keyed by its new first index (removed first, then
    // re-inserted, so no two live entries ever share a key).
    std::vector<Entry*> moved;
    for (auto it = mLive.begin(); it != mLive.end();)
    {
        Entry& e = it->second;
        const bool gone = !rowOk(e.slot, e.ic, e.vc);
        if (gone || frame - e.last_use > kAgeFrames)
        {
            freeEntry(e);
            it = mLive.erase(it);
            if (!gone) ++mStatEvicted;
            continue;
        }
        if (rows[e.slot].first_index != e.first_index)
        {
            if (e.table != kEmpty) tableRemove(e.table);
            e.table = kEmpty;
            e.first_index = rows[e.slot].first_index;
            moved.push_back(&e);
        }
        ++it;
    }
    for (Entry* e : moved) e->table = tableInsert(*e);   // (unordered_map nodes are stable)

    // 2. Finished matches -> index ranges + uploads + table.
    std::vector<std::unique_ptr<Job>> done;
    {
        std::lock_guard<std::mutex> lk(mMutex);
        done.swap(mJobsOut);
    }
    U64 committed = 0;   // index words queued for upload this frame (vkCmdUpdateBuffer, bounded)
    for (size_t di = 0; di < done.size(); ++di)
    {
        if (committed >= kUploadWords)
        {
            // the rest next frame
            std::lock_guard<std::mutex> lk(mMutex);
            for (size_t k = di; k < done.size(); ++k) mJobsOut.push_back(std::move(done[k]));
            break;
        }
        std::unique_ptr<Job>& jp = done[di];
        if (mInFlight) --mInFlight;
        Job& job = *jp;
        auto pit = mPending.find(job.slot);
        if (pit == mPending.end() || pit->second.state != ST_RUNNING || pit->second.job != job.id) continue;
        Pending& p = pit->second;
        if (!job.ok || !rowOk(job.slot, p.ic, p.vc))
        {
            p.state = ST_FAILED; p.frame = frame;
            ++mStatFailed;
            continue;
        }
        if (mLive.size() >= kMaxLive)
        {
            // full table: the least recently used entry goes
            auto lru = mLive.end();
            for (auto it = mLive.begin(); it != mLive.end(); ++it)
                if (lru == mLive.end() || it->second.last_use < lru->second.last_use) lru = it;
            if (lru != mLive.end()) { freeEntry(lru->second); mLive.erase(lru); ++mStatEvicted; }
        }
        Entry e;
        e.slot = job.slot; e.first_index = rows[job.slot].first_index; e.ic = p.ic; e.vc = p.vc;
        e.last_use = frame;
        for (U32 s = 0; s < kSteps; ++s)
        {
            const U32 count = (U32)job.out[s].size();
            if (!count) break;
            const U32 words = evenUp(count);   // allocation (16-bit indices, even-aligned)
            U32 first = 0;
            bool ok = alloc(words, first);
            while (!ok)
            {
                // over budget: evict the least recently used entry not used this frame
                auto lru = mLive.end();
                for (auto it = mLive.begin(); it != mLive.end(); ++it)
                    if (it->second.last_use != frame && (lru == mLive.end() || it->second.last_use < lru->second.last_use))
                        lru = it;
                if (lru == mLive.end()) break;
                freeEntry(lru->second);
                mLive.erase(lru);
                ++mStatEvicted;
                ok = alloc(words, first);
            }
            if (!ok) break;   // (coarser steps are smaller, but a gap would break the step order)
            e.range_first[s] = first;
            e.range_count[s] = count;
            e.err[s] = job.err[s];
            e.nsteps = s + 1;
            committed += words;
            {
                Upload up;
                up.first = first;
                up.idx.resize(words, 0);   // (the pad index is never drawn)
                for (U32 i = 0; i < count; ++i) up.idx[i] = (U16)job.out[s][i];   // source-numbered, < vc <= 65536
                mUploads.push_back(std::move(up));
            }
            mStatStepTris += count / 3;
        }
        mPending.erase(pit);
        if (e.nsteps == 0) continue;
        // (a stale live entry with the same first index cannot exist: step 1 dropped it)
        e.table = tableInsert(e);
        mStatSrcTris += e.ic / 3;
        ++mStatBuilt;
        mLive[e.slot] = e;
    }
    if (mTombs > kTableCap / 4) tableRebuild();

    // 3. Launch queued matches (bounded mirror copy per frame).
    LLCleanMeshAsset& ms = LLCleanMeshAsset::instance();
    U64 copied = 0;
    while (!mQueue.empty() && copied < kLaunchVerts)
    {
        const U32 slot = mQueue.front();
        mQueue.pop_front();
        auto pit = mPending.find(slot);
        if (pit == mPending.end() || pit->second.state != ST_QUEUED) continue;
        Pending& p = pit->second;
        const LLCleanRigDecimator::Info* in = LLCleanRigDecimator::instance().find(slot);
        U32 nv = 0, nw = 0;
        const LLCleanMeshAsset::QVertex* sq = ms.cpuSlotPacked(slot, nv);   // header-prefixed (#573)
        const LLCleanMeshAsset::QVertex* sv = sq ? sq + LLCleanMeshAsset::QV_HEADER : nullptr;
        const LLCleanMeshAsset::MirrorWeight* sw = ms.cpuSlotWeights(slot, nw);
        if (!in || in->src_ic != p.ic || in->src_vc != p.vc || !rowOk(slot, p.ic, p.vc)
            || !sv || !sw || nv != p.vc || nw != nv)
        {
            mPending.erase(pit);   // steps gone / content changed: asked again by want()
            continue;
        }
        auto job = std::make_unique<Job>();
        job->slot = slot; job->id = ++mJobSeq; job->vc = nv;
        job->src.resize((size_t)nv * kFloats);
        for (U32 v = 0; v < nv; ++v) packVertex(sv[v], sw[v], &job->src[(size_t)v * kFloats]);
        copied += nv;
        for (U32 s = 0; s < kSteps; ++s)
        {
            const U32 d = in->dec[s];
            if (d == LLCleanRigDecimator::kEmpty) break;
            U32 dv = 0, dw = 0, di = 0;
            const LLCleanMeshAsset::QVertex* vq = ms.cpuSlotPacked(d, dv);
            const LLCleanMeshAsset::QVertex* vv = vq ? vq + LLCleanMeshAsset::QV_HEADER : nullptr;
            const LLCleanMeshAsset::MirrorWeight* ww = ms.cpuSlotWeights(d, dw);
            const U16* ii = ms.cpuSlotIndices(d, di);
            if (!vv || !ww || !ii || dw != dv || dv == 0 || di == 0) break;
            job->step_v[s].resize((size_t)dv * kFloats);
            for (U32 v = 0; v < dv; ++v) packVertex(vv[v], ww[v], &job->step_v[s][(size_t)v * kFloats]);
            job->step_i[s].assign(ii, ii + di);
            job->err[s] = in->err[s];
            copied += dv;
        }
        p.state = ST_RUNNING;
        p.job = job->id;
        p.frame = frame;
        ++mInFlight;
        if (!mThread.joinable()) mThread = std::thread([this] { workerMain(); });
        {
            std::lock_guard<std::mutex> lk(mMutex);
            mJobsIn.push_back(std::move(job));
        }
        mCv.notify_one();
    }
}

std::string LLCleanRigShadowLod::statsLine() const
{
    std::ostringstream o;
    o << "shadow LOD: " << mLive.size() << " slots, " << (mUsedWords * 2u) / 1024u << " / " << ((U64)mCapWords * 2u) / 1024u
      << " KiB (peak " << (mPeakWords * 2u) / 1024u << "), built " << mStatBuilt << ", failed " << mStatFailed
      << ", evicted " << mStatEvicted << ", queued " << mQueue.size() << ", in flight " << mInFlight;
    if (mStatSrcTris) o << ", step/src tris " << mStatStepTris << "/" << mStatSrcTris;
    return o.str();
}
