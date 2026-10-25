/**
 * @file llcleanparticlestore.cpp
 * @brief Implementation of LLCleanParticleStore.
 */

#include "llviewerprecompiledheaders.h"

#include "llcleanparticlestore.h"

#include "llappviewer.h"  // gFrameCount (emitter reclaim throttle)
#include "llframetimer.h" // emitter reclaim stamps (wall time)
#include "llgl.h"
#include "llpartdata.h"   // LLPartData::LL_PART_BF_*
#include "llcleantexture.h"
#include "llviewercontrol.h" // RenderVKParticleAudit

#include <algorithm>
#include <cmath>
#include <functional>

// Vulkan: the store is CPU-only (emitter table + ribbon state); LLCleanRendererVK
// owns the GPU buffers, streams the emitter table and resolves emitter textures.

LLCleanParticleStore& LLCleanParticleStore::instance()
{
    static LLCleanParticleStore s;
    return s;
}

bool LLCleanParticleStore::init()
{
    if (mReady) return true;

    mMaxParticles = DEFAULT_MAX_PARTICLES;

    // CPU-side store: the VK renderer owns the GPU pool and streams emitterData().
    growEmitters(DEFAULT_MAX_EMITTERS);
    mReady = true;
    LL_INFOS("CleanParticle") << "init (CPU-side store): pool=" << mMaxParticles
        << " emitters=" << mEmitterCapacity << " blend pairs=" << NUM_BLEND_PAIRS << LL_ENDL;
    return true;
}

void LLCleanParticleStore::shutdown()
{
    mEmittersCPU.clear();
    mEmitterSlots.clear();
    mEmitterImg.clear();
    ++mEmitterImgGen;
    mEmitterOwner.clear();
    mFreeEmitterSlots.clear();
    mEmitterLastFed.clear();
    mEmitterFog.clear();
    mEmitterCapacity = 0;
    mEmitterHighWater = 0;
    mReady = false;
}

bool LLCleanParticleStore::growEmitters(U32 new_capacity)
{
    if (new_capacity <= mEmitterCapacity) return true;     // already big enough
    if (new_capacity > HARD_MAX_EMITTERS) new_capacity = HARD_MAX_EMITTERS;
    if (new_capacity <= mEmitterCapacity) return false;    // at the ceiling

    mEmittersCPU.resize(new_capacity);
    mEmitterLastFed.resize(new_capacity, 0.0);
    mEmitterFog.resize(new_capacity);
    mEmitterCapacity = new_capacity;   // the VK renderer sizes its own GPU copy
    return true;
}

void LLCleanParticleStore::beginFrame()
{
    // Zero this frame's spawn counts across the whole live table. Live
    // sources that don't burst this frame still refresh their position
    // (setEmitter with spawn 0) so following particles track them; the
    // emit pass skips any slot whose spawn count stayed 0.
    for (U32 i = 0; i < mEmitterHighWater; ++i)
    {
        mEmittersCPU[i].misc[2] = 0; // spawn_count
    }
    mFrameHasGlow = false;

    // Reclaim emitter slots for sources that derezzed (bounds the table that
    // otherwise grew unbounded as you roam past particle sources).
    reclaimEmitters();
}

void LLCleanParticleStore::consumeSpawns()
{
    for (U32 i = 0; i < mEmitterHighWater; ++i)
    {
        mEmittersCPU[i].misc[2] = 0; // spawn_count
    }
}

void LLCleanParticleStore::holdFrame()
{
    for (U32 i = 0; i < mEmitterHighWater; ++i)
    {
        mEmittersCPU[i].misc[2] = 0; // spawn_count
    }
    // mFrameHasGlow keeps its last value: the held particles still glow.
    const F64 now = LLFrameTimer::getElapsedSeconds();
    for (const auto& kv : mEmitterSlots)
    {
        if (kv.second < mEmitterLastFed.size()) mEmitterLastFed[kv.second] = now;
    }
}

U32 LLCleanParticleStore::getOrCreateEmitter(U32 source_id)
{
    const F64 now = LLFrameTimer::getElapsedSeconds();
    auto it = mEmitterSlots.find(source_id);
    if (it != mEmitterSlots.end())
    {
        if (it->second < mEmitterLastFed.size())
            mEmitterLastFed[it->second] = now;
        return it->second;
    }

    U32 slot;
    if (!mFreeEmitterSlots.empty())
    {
        // Reuse a slot reclaimed from a derezzed source (bounds the table). The list is
        // kept sorted descending, so back() is the LOWEST free slot: reuse packs the live
        // set low and lets reclaimEmitters trim the dispatched range from the top.
        slot = mFreeEmitterSlots.back();
        mFreeEmitterSlots.pop_back();
        ++mEmitterReused;
    }
    else
    {
        if (mEmitterHighWater >= mEmitterCapacity)
        {
            if (!growEmitters(mEmitterCapacity * 2)) return INVALID;
        }
        slot = mEmitterHighWater++;
    }
    mEmitterSlots[source_id] = slot;
    mEmittersCPU[slot] = CleanEmitter{};
    if (slot < mEmitterFog.size()) mEmitterFog[slot] = CleanPartFogDesc{};
    if (slot < mEmitterLastFed.size()) mEmitterLastFed[slot] = now;
    return slot;
}

void LLCleanParticleStore::reclaimEmitters()
{
    // Throttled -- the leak is slow (one slot per derezzed source).
    static U32 s_last_reclaim = 0xFFFFFFFFu;
    const U32 frame = (U32)gFrameCount;
    if (s_last_reclaim != 0xFFFFFFFFu && (frame - s_last_reclaim) < 120) return;
    s_last_reclaim = frame;
    const F64 now = LLFrameTimer::getElapsedSeconds();

    // Grace in WALL TIME, above the longest particle life LSL allows (PSYS_PART_MAX_AGE
    // clamps to 30 s), so a lingering particle still has its own source's row (texture,
    // frozen position) for its whole life. It was 2700 FRAMES -- 45 s at 60 fps, but
    // 7.5 s at 360 fps: on a fast machine slots were recycled under live particles (#693).
    // The GPU sim also stops following a slot that no longer carries the particle's
    // source id, so a recycle can never move or re-texture a lingering particle.
    constexpr F64 GRACE = 45.0;
    for (auto it = mEmitterSlots.begin(); it != mEmitterSlots.end(); )
    {
        const U32 slot = it->second;
        const F64 fed  = (slot < mEmitterLastFed.size()) ? mEmitterLastFed[slot] : now;
        if (now >= fed && (now - fed) >= GRACE)
        {
            // Mark the emitter dead (no spawn) and recycle the slot. It stays
            // within mEmitterHighWater (the emit pass skips spawn-0 slots), so
            // the table is bounded to the concurrent working set, not every
            // source ever seen.
            if (slot < mEmittersCPU.size())
            {
                mEmittersCPU[slot] = CleanEmitter{};
                mEmittersCPU[slot].misc[2] = 0; // spawn_count
            }
            mFreeEmitterSlots.push_back(slot);
            ++mEmitterReclaimed;
            if (mEmitterImg.erase(slot)) ++mEmitterImgGen;
            mEmitterOwner.erase(slot);
            if (slot < mEmitterFog.size()) mEmitterFog[slot] = CleanPartFogDesc{};
            it = mEmitterSlots.erase(it);
        }
        else
        {
            ++it;
        }
    }
    // Lowest free slot at the back (getOrCreateEmitter pops it).
    std::sort(mFreeEmitterSlots.begin(), mFreeEmitterSlots.end(), std::greater<U32>());

    // TRIM: the emit dispatch and the emitter-table stream cover [0, mEmitterHighWater).
    // Free slots at the top leave that range once they have been free a SECOND grace
    // (freed at fed + GRACE, trimmed at fed + 2*GRACE): their zeroed row was streamed
    // long ago and no lingering particle still reads their texture. The GPU buffer keeps
    // its capacity (high-water); only the live range shrinks.
    while (mEmitterHighWater > 0 && !mFreeEmitterSlots.empty()
           && mFreeEmitterSlots.front() == mEmitterHighWater - 1)
    {
        const U32 top = mEmitterHighWater - 1;
        const F64 fed = (top < mEmitterLastFed.size()) ? mEmitterLastFed[top] : now;
        if (!(now >= fed && (now - fed) >= 2.0 * GRACE)) break;
        mFreeEmitterSlots.erase(mFreeEmitterSlots.begin());   // the front = the highest
        --mEmitterHighWater;
        ++mEmitterTrimmed;
    }
}

LLCleanParticleStore::Stats LLCleanParticleStore::stats() const
{
    Stats s;
    s.live = (U32)mEmitterSlots.size();
    s.high_water = mEmitterHighWater;
    s.capacity = mEmitterCapacity;
    s.free = (U32)mFreeEmitterSlots.size();
    s.trimmed = mEmitterTrimmed;
    s.reclaimed = mEmitterReclaimed;
    s.reused = mEmitterReused;
    s.cpu_bytes = (U64)mEmitterCapacity * (sizeof(CleanEmitter) + sizeof(F64))
                + (U64)(mEmitterSlots.size() + mEmitterImg.size()) * 48;
    return s;
}

void LLCleanParticleStore::setEmitterFog(U32 slot, const CleanPartFogDesc& d)
{
    if (slot >= mEmitterHighWater || slot >= mEmitterFog.size()) return;
    mEmitterFog[slot] = d;
    mEmitterFog[slot].valid = true;
}

void LLCleanParticleStore::clearEmitterFog(U32 slot)
{
    if (slot < mEmitterFog.size()) mEmitterFog[slot].valid = false;
}

U32 LLCleanParticleStore::readLiveParticleCount()
{
    return mVKLiveCount;   // the VK renderer's async free-count readback
}

void LLCleanParticleStore::setEmitter(U32 slot, const CleanEmitter& e)
{
    if (slot >= mEmitterHighWater) return;
    CleanEmitter ne = e;

    // Ribbons emit like every other particle (LL: time-based bursts from the live source
    // position); the GPU chains them through the live particles (clean_particle_emit.comp).
    // (A distance gate here -- a new point only once the source moved 10 cm, one per
    // frame, from a low-passed position -- starved a still source: a leash stopped
    // rendering whenever its wearer stood still and went chunky while walking, #190.)
    {
        // Carry last frame's source position/rotation into
        // prev_pos so following particles track a moving source. First
        // frame (stored pos == 0) seeds prev at the current pos.
        const CleanEmitter& prev = mEmittersCPU[slot];
        const bool have_prev = (prev.pos[0] != 0.f || prev.pos[1] != 0.f || prev.pos[2] != 0.f);
        ne.prev_pos[0] = have_prev ? prev.pos[0] : e.pos[0];
        ne.prev_pos[1] = have_prev ? prev.pos[1] : e.pos[1];
        ne.prev_pos[2] = have_prev ? prev.pos[2] : e.pos[2];
        ne.prev_pos[3] = 0.f;
        for (S32 k = 0; k < 4; ++k)
            ne.prev_rot[k] = have_prev ? prev.rot[k] : e.rot[k];
    }

    // Per-particle max age (accel.w): finite, at least one 8.8 fixed-point step and at
    // most the wire format's ceiling, so the GPU sim always retires the particle
    // (clean_particle_sim.comp). NaN fails the >= test and takes the floor.
    {
        constexpr F32 kMinAge = 1.f / 256.f;
        constexpr F32 kMaxAge = 256.f;
        F32& a = ne.accel[3];
        if (!(a >= kMinAge)) a = kMinAge;
        else if (a > kMaxAge) a = kMaxAge;
    }

    mEmittersCPU[slot] = ne;
    if (e.glow[0] > 0.f || e.glow[1] > 0.f) mFrameHasGlow = true;
}

void LLCleanParticleStore::setEmitterTexture(U32 slot, const LLUUID& img)
{
    if (slot >= mEmitterHighWater) return;
    auto it = mEmitterImg.find(slot);
    if (it == mEmitterImg.end()) { mEmitterImg.emplace(slot, img); ++mEmitterImgGen; }
    else if (it->second != img) { it->second = img; ++mEmitterImgGen; }
}

void LLCleanParticleStore::setEmitterOwner(U32 slot, U32 source_id, const LLUUID& owner, const LLUUID& source_object)
{
    if (slot >= mEmitterHighWater) return;
    EmitterOwner& o = mEmitterOwner[slot];
    o.source_id = source_id;
    o.owner = owner;
    o.object = source_object;
}

bool LLCleanParticleStore::emitterOwner(U32 slot, U32 source_id, LLUUID& owner, LLUUID& source_object) const
{
    auto it = mEmitterOwner.find(slot);
    if (it == mEmitterOwner.end() || it->second.source_id != source_id) return false;
    owner = it->second.owner;
    source_object = it->second.object;
    return true;
}

// SL particle systems can request any (src, dst) pair of the 8 supported blend
// factors; each pair is its own draw with its own fixed-function blend (the
// renderer builds the uncommon pairs' pipelines on first sight).
U32 LLCleanParticleStore::blendFactorIndex(U8 f)
{
    using P = LLPartData;
    switch (f)
    {
    case P::LL_PART_BF_ONE:                    return BLEND_FACTOR_ONE;
    case P::LL_PART_BF_ZERO:                   return BLEND_FACTOR_ZERO;
    case P::LL_PART_BF_DEST_COLOR:             return BLEND_FACTOR_DEST_COLOR;
    case P::LL_PART_BF_SOURCE_COLOR:           return BLEND_FACTOR_SOURCE_COLOR;
    case P::LL_PART_BF_ONE_MINUS_DEST_COLOR:   return BLEND_FACTOR_ONE_MINUS_DEST_COLOR;
    case P::LL_PART_BF_ONE_MINUS_SOURCE_COLOR: return BLEND_FACTOR_ONE_MINUS_SOURCE_COLOR;
    case P::LL_PART_BF_SOURCE_ALPHA:           return BLEND_FACTOR_SOURCE_ALPHA;
    case P::LL_PART_BF_ONE_MINUS_SOURCE_ALPHA: return BLEND_FACTOR_ONE_MINUS_SOURCE_ALPHA;
    default:                                   return INVALID;   // UNSUPPORTED_*_DEST_ALPHA
    }
}

U32 LLCleanParticleStore::blendPair(U8 s, U8 d)
{
    const U32 si = blendFactorIndex(s), di = blendFactorIndex(d);
    if (si == INVALID || di == INVALID) return BLEND_PAIR_ALPHA;
    return si * NUM_BLEND_FACTORS + di;
}

// ---- RenderVKParticleAudit (#693) ----

bool LLCleanParticleStore::auditEnabled()
{
    static LLCachedControl<bool> s_audit(gSavedSettings, "RenderVKParticleAudit", false);
    return s_audit;
}

LLCleanParticleStore::AuditSrc& LLCleanParticleStore::auditSource(U32 source_id)
{
    if (mAuditSince == 0.0) mAuditSince = LLFrameTimer::getTotalSeconds();
    AuditSrc& a = mAudit[source_id];
    a.last_seen = LLFrameTimer::getTotalSeconds();
    return a;
}

void LLCleanParticleStore::auditFed(U32 source_id, U32 fed)
{
    auto it = mAudit.find(source_id);
    if (it != mAudit.end()) it->second.fed += fed;
}

// LL's expected particles per second for a source (LLViewerPartSourceScript::update, before
// its throttles) at a steady frame time f: a burst fires once the time since the last one
// EXCEEDS the rate, and every burst resets that time to now (the remainder is dropped), so
//   f <= rate: one burst every floor(rate / f) + 1 frames;
//   f >  rate: ceil(f / rate - 1) bursts every frame.
static F32 ll_expected_parts_per_sec(U32 count, F32 rate, F32 f)
{
    if (rate <= 0.f || count == 0) return 0.f;
    if (f <= 0.f) return (F32)count / rate;
    if (f <= rate)
    {
        const F32 m = std::floor(rate / f) + 1.f;
        return (F32)count / (m * f);
    }
    const F32 n = llmax(1.f, std::ceil(f / rate - 1.f));
    return (F32)count * n / f;
}

void LLCleanParticleStore::auditReport(const std::map<U32, AuditGpu>& gpu)
{
    const F64 now = LLFrameTimer::getTotalSeconds();
    const F64 el = llmax(now - mAuditSince, 1e-3);
    mAuditSince = now;
    F32 tot_exp = 0.f, tot_fed = 0.f, tot_gpu = 0.f, tot_exp_live = 0.f;
    U32 tot_live = 0, n = 0;
    for (auto it = mAudit.begin(); it != mAudit.end(); )
    {
        AuditSrc& a = it->second;
        if (now - a.last_seen > 60.0)
        {
            it = mAudit.erase(it);   // source gone a minute: drop its record
            continue;
        }
        if (a.frames > 0)
        {
            const auto g = gpu.find(it->first);
            const AuditGpu gs = g != gpu.end() ? g->second : AuditGpu{};
            const F32 f = (F32)(a.dt_sum / a.frames);
            const F32 exp = a.hud ? 0.f : ll_expected_parts_per_sec(a.count, a.rate, f);
            const F32 nominal = a.rate > 0.f ? (F32)a.count / a.rate : 0.f;
            const F32 gpu_rate = gs.young_window > 0.f ? (F32)gs.young / gs.young_window : 0.f;
            const F32 exp_live = exp * a.part_age;
            const std::string obj = a.object.asString().substr(0, 8);
            LL_INFOS("VKParticleAudit") << "src#" << it->first << " obj=" << obj
                << (a.hud ? " hud" : (a.attachment ? " att" : " world")) << (a.live_xform ? "+live" : "")
                << " pat=0x" << std::hex << a.pattern << " flags=0x" << a.flags << std::dec
                << " rate=" << a.rate << " count=" << a.count << " part_age=" << a.part_age
                << " src_max_age=" << a.src_max_age
                << " | ll_exp/s=" << exp << " nominal/s=" << nominal << " frame_ms=" << f * 1000.f
                << " bursts/s=" << a.bursts / el << " req/s=" << a.requested / el
                << " kept/s=" << a.kept / el << " fed/s=" << a.fed / el << " gpu/s=" << gpu_rate
                << " | live=" << gs.live << " exp_live=" << exp_live
                << " | jump_max=" << a.jump_max << " lag_max=" << a.lag_max
                << " spread_max=" << gs.spread_max << LL_ENDL;
            tot_exp += exp; tot_fed += (F32)(a.fed / el); tot_gpu += gpu_rate;
            tot_live += gs.live; tot_exp_live += exp_live; ++n;
        }
        a.bursts = a.requested = a.kept = a.fed = 0;
        a.dt_sum = 0.0;
        a.frames = 0;
        a.jump_max = a.lag_max = 0.f;
        ++it;
    }
    LL_INFOS("VKParticleAudit") << "total sources=" << n << " ll_exp/s=" << tot_exp << " fed/s=" << tot_fed
        << " gpu/s=" << tot_gpu << " live=" << tot_live << " exp_live=" << tot_exp_live
        << " (window " << el << " s)" << LL_ENDL;
}

void LLCleanParticleStore::shiftOrigin(const F32 off[3])
{
    // No CPU-side agent-space state to re-base (the GPU particles shift in the sim pass).
    (void)off;
}
