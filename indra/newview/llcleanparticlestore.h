/**
 * @file llcleanparticlestore.h
 * @brief GPU-driven particle system store. CPU only uploads one
 *        emitter descriptor per active particle source each frame;
 *        the GPU owns everything else -- emission, physics
 *        integration, aging, color/scale/glow interpolation, death,
 *        free-slot recycling, frustum cull, and billboard expansion.
 *
 *        This replaces LL's CPU pipeline (LLViewerPartSim CPU sim +
 *        LLVOPartGroup per-particle CPU billboard VBO assembly +
 *        alpha-pool draw), which the clean GPU-driven renderer left
 *        disabled. Mirrors the clean-pipeline stores (LLCleanSkinStore
 *        et al.): persistent SSBOs, capacity growth, bindless
 *        textures, compute dispatch, multi-draw indirect.
 *
 *        Data flow each frame:
 *          CPU: refresh emitter descriptors (LLPartSysData -> EmitterDesc)
 *          GPU: emit  -> spawn into free slots per emitter pattern
 *               sim   -> integrate, age, recycle dead to free list
 *               compact -> per-blend-bucket draw lists + indirect counts
 *               draw  -> billboard VS expansion, bindless FS, alpha pass
 */

#pragma once

#include "stdtypes.h"
#include "lluuid.h"
#include "v3math.h"

#include <map>
#include <vector>

// Per-particle GPU record (std430). 144 bytes. Lives in the particle
// pool SSBO; written by the emit compute, advanced by the sim compute,
// read by the billboard vertex shader. Must match struct Particle in
// the clean particle shaders exactly.
struct CleanParticle
{
    F32 pos_age[4];      // xyz world position, w = age (seconds)
    F32 vel_maxage[4];   // xyz velocity, w = max age (seconds)
    F32 accel[4];        // xyz acceleration (incl. per-part accel), w unused
    F32 start_color[4];  // rgba at birth
    F32 end_color[4];    // rgba at death
    F32 scale[4];        // start scale .xy, end scale .zw
    F32 glow[4];         // start glow, end glow, z volumetric sprite fade (#721: GPU-written by clean_vol_part.comp; 0 on spawn), 0
    U32 misc[4];         // flags(LL_PART_*), blend_bucket, emitter_idx, alive
    U32 tex_handle[2];   // .x = the renderer's bindless texture index (written by the store)
    U32 pad[2];          // .x ribbon serial; .y source id (LLViewerPartSource mID, #481 pick)
    F32 prev_pos[4];     // ribbon: previous segment point (xyz); else == pos
    F32 axis[4];         // ribbon: width axis at pos end (source +Z in world, xyz)
    F32 prev_axis[4];    // ribbon: width axis at prev_pos end (source +Z last frame, xyz)
    F32 pos_offset[4];   // FOLLOW_SRC: particle position relative to its source (xyz)
};

// Per-emitter GPU record (std430). CPU rewrites every live source each
// frame from its LLPartSysData + current world transform. The GPU emit
// compute reads these. Emission COUNT is decided CPU-side (a trivial
// scalar accumulator per source) and passed in misc.z -- the GPU does
// all per-particle placement (pattern math + RNG). Must match struct
// Emitter in cleanParticleC.glsl exactly.
struct CleanEmitter
{
    F32 pos[4];          // xyz source world position, w = burst_radius
    F32 rot[4];          // emission rotation quaternion (srcRot * angular-vel rot)
    F32 accel[4];        // xyz per-particle acceleration, w = part max age
    F32 start_color[4];
    F32 end_color[4];
    F32 scale[4];        // start .xy, end .zw
    F32 glow[4];         // start glow, end glow, 0, 0
    F32 speed_angle[4];  // burst_speed_min, max, inner_angle, outer_angle
    F32 target[4];       // xyz target world position, w unused
    U32 misc[4];         // pattern, flags, spawn_count (this frame), rng_seed
    U32 tex_handle[2];   // .x = the renderer's bindless texture index (written by the store)
    U32 blend[2];        // blend_bucket, unused
    F32 prev_pos[4];     // ribbon point P-1 (most recent emitted point before this)
    F32 prev_rot[4];     // emission rotation last frame (legacy; unused by mitered ribbon)
    F32 wind[4];         // region wind velocity at the source (xyz); WIND-flagged particles
    F32 src_rot[4];      // source object render rotation alone (no PSYS_SRC_OMEGA spin): the
                         // ribbon width axis, LL LLViewerPart::mAxis; identity without a source
    F32 prev_pos3[4];    // ribbon point P-3 (mitered ribbon needs 4 consecutive points)
};

// PARTICLE FOG (#1003; vkgfx/llcleanpartfogvk.h): what the dense-fog classifier needs to know
// about one SCRIPT particle source, copied from its LLPartSysData every frame it feeds its
// emitter slot (LLViewerPartSourceScript::buildGPUFogDesc). CPU only -- the renderer's fog
// module classifies per source (deterministic, reclassified when `hash` changes) and never
// looks at single particles on the CPU. Procedural sources (spiral / beam / chat) give none.
struct CleanPartFogDesc
{
    bool  valid = false;         // a script source fed this slot (false: never / cleared)
    U32   source_id = 0;         // LLViewerPartSource mID (= the particles' pad.y)
    U32   hash = 0;              // hash of the classification inputs (every field but pos)
    U32   flags = 0;             // LLPartData::LL_PART_* (mFlags)
    U32   blend_pair = 0;        // LLCleanParticleStore::blendPair(src, dst)
    U32   burst_count = 0;       // PSYS_SRC_BURST_PART_COUNT
    F32   burst_rate = 0.f;      // PSYS_SRC_BURST_RATE (s)
    F32   part_age = 0.f;        // PSYS_PART_MAX_AGE (s)
    F32   speed_min = 0.f, speed_max = 0.f;   // PSYS_SRC_BURST_SPEED_MIN / MAX (m/s)
    F32   burst_radius = 0.f;    // PSYS_SRC_BURST_RADIUS (m)
    F32   start_scale[2] = {}, end_scale[2] = {};   // m
    F32   start_color[4] = {}, end_color[4] = {};   // rgb (sRGB) + alpha
    F32   glow[2] = {};          // start / end glow
    F32   accel[3] = {};         // PSYS_SRC_ACCEL (m/s^2)
    F32   pos[3] = {};           // source position (agent frame) this frame
    bool  attachment = false;    // worn (non-HUD) attachment
    bool  hud = false;           // HUD attachment
    LLUUID texture;              // particle texture (null: none)
};

class LLCleanParticleStore
{
public:
    static LLCleanParticleStore& instance();

    // Pool of particle slots = LL's hard particle cap (LL_MAX_PARTICLE_COUNT, the
    // RenderMaxPartCount slider's maximum). The live count is capped exactly on the
    // GPU at RenderMaxPartCount (setLiveCap), so a bigger pool was only sim / compact
    // work and memory nobody could use (it was 262144: 54 MB, 32x the dispatch).
    // Must stay a power of two >= 1024 (the depth sort sorts the whole pool).
    static constexpr U32 DEFAULT_MAX_PARTICLES = 8192;
    // Active emitter descriptors per frame. Grows if exceeded.
    static constexpr U32 DEFAULT_MAX_EMITTERS  = 4096;
    // Hard ceiling on the emitter table: the emit pass is one workgroup per emitter
    // slot, and 65535 is the smallest maxComputeWorkGroupCount[0] Vulkan allows. Past
    // it new sources get no GPU emitter (INVALID) until slots are reclaimed.
    static constexpr U32 HARD_MAX_EMITTERS     = 65535;
    // SL particle blend functions (PSYS_PART_BF_*, LLPartData::LL_PART_BF_*): the 8
    // supported factors (ONE, ZERO, DEST_COLOR, SOURCE_COLOR, ONE_MINUS_DEST_COLOR,
    // ONE_MINUS_SOURCE_COLOR, SOURCE_ALPHA, ONE_MINUS_SOURCE_ALPHA) as compact indices
    // 0..7, a (src, dst) pair as src * 8 + dst: every pair LL supports is its own draw
    // (the renderer keys its blend pipelines by pair). Stored in CleanEmitter.blend[0]
    // and the particle's misc.y.
    static constexpr U32 NUM_BLEND_FACTORS = 8;
    static constexpr U32 NUM_BLEND_PAIRS = NUM_BLEND_FACTORS * NUM_BLEND_FACTORS;
    static constexpr U32 BLEND_FACTOR_ONE = 0, BLEND_FACTOR_ZERO = 1, BLEND_FACTOR_DEST_COLOR = 2,
                         BLEND_FACTOR_SOURCE_COLOR = 3, BLEND_FACTOR_ONE_MINUS_DEST_COLOR = 4,
                         BLEND_FACTOR_ONE_MINUS_SOURCE_COLOR = 5, BLEND_FACTOR_SOURCE_ALPHA = 6,
                         BLEND_FACTOR_ONE_MINUS_SOURCE_ALPHA = 7;
    // Default particle blend (no PSYS_PART_BLEND flag): SOURCE_ALPHA, ONE_MINUS_SOURCE_ALPHA.
    static constexpr U32 BLEND_PAIR_ALPHA = BLEND_FACTOR_SOURCE_ALPHA * NUM_BLEND_FACTORS
                                          + BLEND_FACTOR_ONE_MINUS_SOURCE_ALPHA;

    // Procedural motion mode, stored in CleanEmitter.blend[1]. The sim
    // compute drives the particle's position from age for these modes
    // instead of integrating velocity. Must match the MODE_* constants in
    // cleanParticleEmitC.glsl / cleanParticleSimC.glsl.
    enum MotionMode : U32 { MOTION_NORMAL = 0, MOTION_SPIRAL = 1, MOTION_BEAM = 2 };

    bool init();
    void shutdown();
    // Region crossing: re-base the CPU-held agent-space ribbon point history.
    void shiftOrigin(const F32 off[3]);
    bool ready() const { return mReady; }

    // ---- CPU emitter feed (filled by LLViewerPartSim each frame) ----
    //
    // Emitter slots are STABLE per source (keyed by LLViewerPartSource
    // mID, monotonic for the session): a particle stores its emitter
    // slot and the sim shader reads that slot's LIVE source/target
    // position every frame, so follow-source and target-following track
    // a moving source/target. Each frame: beginFrame() zeros every
    // slot's spawn count, then every live source's descriptor is written
    // via getOrCreateEmitter()/setEmitter() (with spawn 0 on frames it
    // doesn't burst, so its position still refreshes), then upload.

    // Zero the per-frame spawn counts across the whole live table.
    void beginFrame();
    // The renderer took this frame's table for the GPU emit (live frame): zero the spawn
    // counts so every fed spawn is emitted exactly once (#693). A frame rendered without a
    // particle tick (idle()'s teleport / disconnect early-outs skip updateSimulation but
    // still render) re-emitted the last fed bursts every frame.
    void consumeSpawns();
    // World frozen (LLWorldClock): zero the spawn counts like beginFrame, but keep
    // every live emitter slot fed and reclaim nothing -- the frozen particles still
    // reference their emitters (texture, follow-source position).
    void holdFrame();
    // Stable slot for a source id (monotonic; grows the table on demand).
    U32  getOrCreateEmitter(U32 source_id);
    // Write a slot's descriptor for this frame.
    void setEmitter(U32 slot, const CleanEmitter& e);
    // Record a slot's particle-texture UUID so its bindless handle can be
    // re-resolved every frame -- including after the source dies. A
    // ribbon/cloud particle outlives its source (up to PART_MAX_AGE), and
    // it reads its texture from the LIVE emitter slot; without this the
    // dead source's slot freezes on whatever handle it last had (often
    // the magenta placeholder, if the texture hadn't finished loading
    // when the source died), so its lingering particles never get a
    // texture. refreshEmitterTextures() re-resolves all recorded slots.
    void setEmitterTexture(U32 slot, const LLUUID& img);
    // Record who feeds a slot (the particle cursor pick, #481): the source id it was fed
    // under (LLViewerPartSource mID, which every particle carries from its emit) and
    // that source's owner / source object. Kept until the slot is reclaimed, so a
    // particle that outlives its source still names its owner (LL's LLViewerPart held
    // its source alive for the same reason).
    void setEmitterOwner(U32 slot, U32 source_id, const LLUUID& owner, const LLUUID& source_object);
    // The owner / source object of a particle emitted from `slot` by `source_id`;
    // false when the slot has been recycled for another source since (or never fed).
    bool emitterOwner(U32 slot, U32 source_id, LLUUID& owner, LLUUID& source_object) const;

    // PARTICLE FOG (#1003): the slot's classification inputs (script sources only; see
    // CleanPartFogDesc). Cleared when the slot is reclaimed / re-assigned or a non-script
    // source feeds it.
    void setEmitterFog(U32 slot, const CleanPartFogDesc& d);
    void clearEmitterFog(U32 slot);
    // The slot's descriptor, or null (none / out of range).
    const CleanPartFogDesc* emitterFog(U32 slot) const
    {
        return (slot < mEmitterFog.size() && mEmitterFog[slot].valid) ? &mEmitterFog[slot] : nullptr;
    }
    // When the slot's source last fed it (LLFrameTimer::getElapsedSeconds; 0 = never).
    F64  emitterLastFed(U32 slot) const { return slot < mEmitterLastFed.size() ? mEmitterLastFed[slot] : 0.0; }

    // Emit dispatches one workgroup per live slot; slots with spawn 0
    // early-out. (The whole table is small -- one entry per source.)
    U32  emitterCount() const { return mEmitterHighWater; }

    struct Stats
    {
        U32 live = 0, high_water = 0, capacity = 0, free = 0;
        U64 trimmed = 0, cpu_bytes = 0;
        U64 reclaimed = 0, reused = 0;   // slots freed by the reclaim / handed to a new source
    };
    Stats stats() const;
    U32  maxParticles() const { return mMaxParticles; }

    // DIAGNOSTIC: read back the live particle count (mMaxParticles minus the
    // GPU free-list count). GPU sync -- call throttled only. A pool saturating
    // toward mMaxParticles is the overdraw/fill-rate signature (100% GPU).
    U32  readLiveParticleCount();

    // Free emitter slots whose source hasn't fed for >= the reclaim grace
    // (longer than any particle's lifetime, so lingering particles can't alias
    // a recycled slot), recycling them for future sources. Bounds the emitter
    // table to the concurrent working set instead of every source ever seen.
    // Call once per frame (cheap; throttles internally).
    void reclaimEmitters();

    // ---- Vulkan: the renderer owns the GPU side and pulls the CPU table ----
    // Live emitter descriptors [0, emitterCount()) and the slot -> particle
    // texture id map (resolved to bindless indices by LLCleanRendererVK).
    const CleanEmitter* emitterData() const { return mEmittersCPU.data(); }
    const std::map<U32, LLUUID>& emitterTextures() const { return mEmitterImg; }
    // Bumped whenever emitterTextures() changes (the renderer caches its resolution).
    U32 emitterTexturesGen() const { return mEmitterImgGen; }
    // Live count from the renderer's (one-frame-late) free-count readback.
    void setVKLiveCount(U32 n) { mVKLiveCount = n; }
    // RenderMaxPartCount, clamped to the pool: the GPU emit spawns no particle that
    // would take the live count past it. 0 = particles off (no spawn, no draw).
    void setLiveCap(U32 n) { mLiveCap = n < mMaxParticles ? n : mMaxParticles; }
    U32  liveCap() const { return mLiveCap; }


    // Map an LL particle blend-func pair (LLPartData::LL_PART_BF_*) to its blend pair
    // index (see NUM_BLEND_PAIRS). An unsupported factor (the DEST_ALPHA ones, out of
    // range) falls back to the default alpha blend, as LL's validBlendFunc does.
    static U32 blendPair(U8 src_factor, U8 dst_factor);
    // LL_PART_BF_* -> compact factor index (0..7), or INVALID.
    static U32 blendFactorIndex(U8 ll_factor);


    static constexpr U32 INVALID = 0xFFFFFFFFu;

    // ---- RenderVKParticleAudit (#693): per script source, LL's expected emission vs the
    // GPU path's, logged (VKParticleAudit) each time the renderer parses its pool snapshot
    // (~2 s). The sources fill AuditSrc while the setting is on; the renderer counts each
    // source's live particles in the snapshot (AuditGpu) and calls auditReport, which logs
    // and starts the next window.
    struct AuditSrc
    {
        LLUUID object;                          // source object
        U32  pattern = 0, flags = 0, count = 0; // PSYS_SRC_PATTERN, PSYS_PART_FLAGS, BURST_PART_COUNT
        F32  rate = 0.f, part_age = 0.f, src_max_age = 0.f;   // BURST_RATE, PART_MAX_AGE, SRC_MAX_AGE
        bool attachment = false, hud = false, live_xform = false;
        // Window totals: bursts LL's timer fired, the particles they asked for (bursts x
        // count), kept by LL's per-burst throttles (the source's spawn), handed to the GPU
        // (after the global thinning); frames and their summed dt (LL's expected rate).
        U64  bursts = 0, requested = 0, kept = 0, fed = 0;
        F64  dt_sum = 0.0;
        U32  frames = 0;
        F32  jump_max = 0.f;    // largest per-frame source move (m)
        F32  lag_max = 0.f;     // live attachment transform vs LL's getRenderPosition (m)
        LLVector3 last_pos;
        bool have_last = false;
        F64  last_seen = 0.0;
    };
    struct AuditGpu
    {
        U32 live = 0;           // the source's live particles in the snapshot
        U32 young = 0;          // of them, younger than young_window
        F32 young_window = 0.f; // min(1 s, half the particle max age): young / it = spawns/s
        F32 spread_max = 0.f;   // farthest live particle from its source (m)
    };
    static bool auditEnabled();
    AuditSrc& auditSource(U32 source_id);
    void auditFed(U32 source_id, U32 fed);
    void auditReport(const std::map<U32, AuditGpu>& gpu);

private:
    LLCleanParticleStore() = default;
    LLCleanParticleStore(const LLCleanParticleStore&) = delete;
    LLCleanParticleStore& operator=(const LLCleanParticleStore&) = delete;

    bool growEmitters(U32 new_capacity);

    bool   mReady          = false;
    U32    mMaxParticles   = 0;
    U32    mVKLiveCount    = 0;   // Vulkan: readLiveParticleCount() value
    U32    mLiveCap        = DEFAULT_MAX_PARTICLES;   // RenderMaxPartCount (GPU-enforced)


    // CPU emitter mirror (persistent table; slot index is stable).
    std::vector<CleanEmitter> mEmittersCPU;
    U32 mEmitterCapacity  = 0;
    U32 mEmitterHighWater = 0; // count of slots ever assigned
    bool mFrameHasGlow = false; // any live emitter this frame has glow > 0
    std::map<U32, U32> mEmitterSlots; // source mID -> stable slot
    // Emitter slot reclaim. Without it, every particle source ever seen leaks
    // its emitter slot (mEmitterHighWater grew unbounded: 60 -> 672+ as sources
    // rez/derez while roaming), inflating the per-frame emit dispatch + upload.
    // mEmitterLastFed[slot] = when (LLFrameTimer::getElapsedSeconds) the source last fed
    // this slot; a slot not fed for >= the reclaim grace (wall time, above the longest
    // LSL particle life; it was 2700 FRAMES, only 7.5 s at 360 fps, #693) is freed and
    // recycled. Lingering particles never follow a recycled slot either way: the GPU sim
    // checks the slot still carries their source id (clean_particle_sim.comp).
    std::vector<F64>   mEmitterLastFed;  // time stamp per slot (seconds)
    std::vector<U32>   mFreeEmitterSlots; // recycled slots (sorted descending), reused before highWater++
    U64                mEmitterTrimmed = 0; // top slots trimmed off the dispatched range
    U64                mEmitterReclaimed = 0; // slots freed by reclaimEmitters
    U64                mEmitterReused = 0;    // free slots handed to a new source
    std::vector<CleanPartFogDesc> mEmitterFog;   // slot -> particle fog inputs (#1003; sized with the table)
    std::map<U32, LLUUID> mEmitterImg; // slot -> particle texture id (for handle refresh)
    U32 mEmitterImgGen = 1;
    struct EmitterOwner { U32 source_id = 0; LLUUID owner, object; };
    std::map<U32, EmitterOwner> mEmitterOwner; // slot -> who feeds it (particle pick, #481)
    std::map<U32, AuditSrc> mAudit;            // source id -> audit window (RenderVKParticleAudit)
    F64 mAuditSince = 0.0;                     // window start (s)

    // Per-ribbon-source state for distance-gated, mitered emission. Keeps
    // the last three emitted points (p[0] newest .. p[2] oldest); a new
    // point is recorded only once the source moves MIN_SEG (frame-rate
    // independent). A particle spawned at the current point carries these
    // three plus the current one (four total) so the draw can miter the
    // segment one behind the tip. count tracks warm-up (need >= 3).
};
