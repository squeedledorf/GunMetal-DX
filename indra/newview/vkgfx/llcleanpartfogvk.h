/**
 * @file llcleanpartfogvk.h
 * @brief PARTICLE FOG (#1003): fog-like particle systems become DENSE local volumetric fog
 *        for the Vulkan clean renderer.
 *
 * Creators build fog machines, smoke, steam, mist and dust out of particle systems: many
 * large, faint, slow, soft billboards. Drawn as billboards they show hard intersections with
 * geometry, sorting pops, flat camera-facing sheets, no shadowing or light scattering. The
 * froxel fog (#718 / #721 volumetric particles) is a lit, shadowed medium, but at froxel
 * resolution it is far too thin close up. This module gives such systems REAL density:
 *
 * 1. CLASSIFICATION, per system, on the CPU (classify(); LLCleanParticleStore's
 *    CleanPartFogDesc, copied from LLPartSysData every frame the source feeds its slot).
 *    Deterministic, re-evaluated every frame (cheap) so a creator's change takes effect at
 *    once; HYSTERESIS: converted above RenderVKParticleFogThreshold (0.7), reverted below it
 *    minus 0.2, in between the volume takes a share = the score's position in the band.
 *    Hard exclusions (stay particles): additive / emissive / glow; ribbon, targeted,
 *    follow-velocity; small (< 0.5 m), fast (> 1 m/s burst speed) or short-lived (< 1.5 s)
 *    particles; shaped / opaque textures (per-texture stats); unknown texture stats; worn
 *    attachments (RenderVKParticleFogAttachments 0) and HUD; beyond the distance.
 *    Soft score = product of: big (>= 1 m, full at 2.5 m), faint (alpha x texture coverage
 *    <= 0.35), slow & long-lived (<= 0.5 m/s with accel, >= 3 s), normal alpha blend, soft
 *    texture (edge sharpness / high-frequency residual), enough alive (count x age / rate >= 20).
 *    The blend moves toward its target at 1 / 0.6 s: nothing pops.
 * 2. DENSITY CARRIERS. Converted systems' particles keep being simulated exactly as SL
 *    specifies (the GPU particle pool is untouched) but their billboards cross-fade out by the
 *    system's blend (the bin pass scales the lit particle VS's per-particle fade). Each
 *    particle is splatted into its system's BRICK: mass = the sprite's optical depth over its
 *    quad (-ln(1 - alpha x texture mean alpha) x sx x sy, x RenderVKParticleFogDensity), a
 *    smooth kernel whose radius grows with age from the start scale past the end scale with
 *    the MASS CONSERVED (discrete normalisation: the peak falls as it widens), the particle's
 *    own alpha fade as the dissipation, tint = particle colour x texture mean colour.
 * 3. BRICKS. A world-space grid per system sized to the system's real extent (the particles'
 *    bounds, read back from the GPU two frames late; a CPU estimate from emitter range + speed
 *    x age until then), voxels ~5 cm near the camera growing with distance and system size
 *    (<= kN per axis), quantised + snapped so they do not shimmer. One pooled 3D ATLAS of
 *    kN^3 slots (+ kNC^3 coarse ones), budget RenderVKParticleFogBudgetMB (soft VRAM pressure
 *    halves the slot cap until it eases), slots handed out by priority (blend x screen size),
 *    a holder fades out before its slot is taken (eviction is a fade, never a pop), stats
 *    live / cap / peak. Off-screen bricks are neither splatted nor marched.
 * 4. RENDER. clean_pfog_march.comp marches the visible bricks at reduced resolution
 *    (RenderVKParticleFogResolution; RenderVKParticleFogNearResolution while a brick is
 *    within ~4 m: near fog is sharper) with jittered steps, sun cascades + cloud shadow per
 *    step, the clustered local lights + shadow-mapped projectors (the light grid), in-brick
 *    self-occlusion with a multiple-scattering approximation (bright tops, dark bellies),
 *    then a SHORT temporal accumulation (RenderVKParticleFogHistory frames, variance clamped
 *    and cut at density edges / with motion: it never thins the core or smears a billow). The
 *    composite (second draw of pass 2, right after the froxel apply) upsamples depth-aware
 *    and orders the medium against the froxel fog and the haze by its mean depth;
 *    transparents / particles drawn afterwards lay over it (one known ordering limit: a
 *    transparent BEHIND the cloud also draws over it).
 * 4b. BILLOWING SHAPE (what makes it read as clouds, not haze):
 *    * EROSION noise model (cloud rendering): the smooth splatted field is a coverage that a
 *      low-frequency Perlin-Worley BASE thresholds into lumps with defined edges and a high-
 *      frequency Worley DETAIL erodes into cauliflower, stronger where it is thin (cores stay
 *      solid). Scale = the system's sprite size (0.3 - 1 m) x ...BillowScale, its own phase
 *      per system, advected with the particles' flow, slowly churning. Two small tiling 3D
 *      noise textures built once on the GPU (64^3 + 32^3 RGBA8, 1.1 MB).
 *    * INDIVIDUAL PUFFS: every particle's kernel is its own lumpy, noise-displaced sphere
 *      (its seed: pool slot + spawn offset), so its lumps persist as it grows and spreads.
 *    * LIGHT: the strongest ...ShadowLights local lights per coarse voxel are SELF-SHADOWED by
 *      a short ray through the brick (lit sides glow, far sides fall dark); ambient occlusion
 *      over six cone directions (folds, cores, bellies); beer-powder; dual-lobe
 *      Henyey-Greenstein toward the sun and the local lights' mean direction (halos around
 *      lights seen through the fog).
 *    * HEAVY: fog-machine fog settles (...Heavy x 0.2 m/s after a short delay; rising smoke /
 *      steam -- an upward acceleration -- does not) onto the floor the march saw in the depth
 *      buffer (the lowest upward-facing surface under the brick below the source; else the
 *      land / the source minus 1.5 m) and rolls outward from the source along it in a
 *      flattened layer -- on the density carriers only (the bin's offset on the simulated
 *      position, wind kept); the billboards and the particle pool are untouched.
 * 5. SETTINGS (live): RenderVKParticleFog (master, default 0; needs RenderVKVolumetricFog and
 *    RenderVKParticleLighting), ...Density, ...Billow (base threshold), ...Erosion (detail
 *    erosion + puff lumpiness), ...BillowScale, ...ShadowLights (0-4, 0 off), ...Heavy (0-1),
 *    ...Threshold, ...BudgetMB, ...Debug (1: medium tinted by blend red -> green + brick
 *    boxes; 2 erosion; 3 ambient occlusion; 4 local light), ...Steps, ...Resolution,
 *    ...NearResolution, ...Distance, ...History, ...Attachments. Log VKPartFog: systems seen / converted /
 *    blended / excluded by reason, bricks live / cap / peak, atlas MB. GPU sub-marks
 *    c_pfog_tex, c_pfog_bin, c_pfog_splat, c_pfog_resolve, c_pfog_light, c_pfog_march,
 *    c_pfog_temporal, g_pfog_comp (VKGpu log line).
 *
 * LIFETIME: off -> the renderer never creates this object; turned off -> destroy() + delete.
 * Resources are (re)created only in prepare() (the renderer's target-ensure step, before
 * anything is recorded that could reference them; device idle when replacing). #697: a
 * refused allocation never crashes -- the effect skips, retries ~5 s later, logs.
 *
 * VRAM: per atlas slot ~8.1 MB (fine RGBA16F 2 MB, four coarse RGBA16F 1 MB, the
 * accumulator 5 MB); the march targets (4 RGBA16F + 2 R32F history lengths, at the march
 * resolution -- the near resolution once near fog was seen: ~13 MB at a 1920x1080 march);
 * the noise volumes 1.1 MB; the floor maps 132 KB; small host-visible rings.
 */
#ifndef LL_LLCLEANPARTFOGVK_H
#define LL_LLCLEANPARTFOGVK_H

#include "vk_device.h"
#include "vk_descpool.h"
#include "vk_pipeline.h"   // PipelineJobSlot

#include "lluuid.h"

#include <cstdint>
#include <functional>
#include <map>
#include <unordered_map>
#include <vector>

struct CleanPartFogDesc;
struct CleanEmitter;

class LLCleanPartFogVK
{
public:
    // Layout constants (= clean_pfog_common.glsl PF_*).
    static constexpr uint32_t kMaxSys = 64;
    static constexpr uint32_t kMaxBricks = 32;
    static constexpr uint32_t kMaxTexJobs = 8;
    static constexpr uint32_t kN = 64;
    static constexpr uint32_t kNC = 32;
    static constexpr uint64_t kSlotWords = (uint64_t)kN * kN * kN * 4u + (uint64_t)kNC * kNC * kNC * 8u;
    static constexpr uint32_t kFloorWords = 8u + kNC * kNC;   // PF_FLOOR_WORDS

    // What the module needs from the renderer this recording.
    struct Frame
    {
        VkDeviceContext*      ctx = nullptr;
        VkDescriptorSetLayout dslDraw0 = VK_NULL_HANDLE;   // draw set 0 (frame info, lights, cascades, 45, 46)
        VkDescriptorSetLayout dslDraw1 = VK_NULL_HANDLE;   // draw set 1 (bindless)
        VkFormat hdrFormat = VK_FORMAT_UNDEFINED;          // render pass 2's attachments
        VkFormat glowFormat = VK_FORMAT_UNDEFINED;
        VkFormat reactFormat = VK_FORMAT_UNDEFINED;
        VkFormat depthFormat = VK_FORMAT_UNDEFINED;
        uint32_t rw = 0, rh = 0;                           // render extent
        uint32_t frameSlot = 0;                            // frame-in-flight slot (host rings)
        const float* viewProjRel = nullptr;                // UNJITTERED P * rotation-only V (16)
        const float* camTime = nullptr;                    // camera world pos xyz, time (s)
        VkImageView depthView = VK_NULL_HANDLE;            // the main depth (DEPTH_READ_ONLY in pass 2)
        uint64_t depthGen = 0;
        VkBuffer partPool = VK_NULL_HANDLE;                // the renderer's GPU particle pool (CleanParticle x partMax)
        VkBuffer partLight = VK_NULL_HANDLE;               // its per-particle lighting records (3 vec4 per slot)
        uint32_t partMax = 0;
        bool volFogNow = false;                            // the froxel fog runs this recording
        bool particlesOn = false;                          // particles simulated + shown this recording
        bool partLighting = false;                         // RenderVKParticleLighting != 0 (the VS that fades)
        bool underwater = false;
        bool offscreen = false;                            // snapshot: no history, no classification step
        bool cameraCut = false;
        bool pressured = false;                            // VRAM ladder engaged / pressure mode
        float tanHalfFovY = 0.6f;
        uint32_t maxImageDim3D = 2048;
        // The bindless index of a particle texture (-1: not resolved yet).
        std::function<int32_t(const LLUUID&)> texIndex;
    };

    LLCleanPartFogVK() = default;
    ~LLCleanPartFogVK() = default;   // destroy() is explicit (needs the device)
    LLCleanPartFogVK(const LLCleanPartFogVK&) = delete;
    LLCleanPartFogVK& operator=(const LLCleanPartFogVK&) = delete;

    // The renderer's target-ensure step (after the froxel fog's): settings, classification,
    // brick allocation, (re)creation of what changed, this recording's host rings. true = the
    // compute + composite run this recording (if the particle lighting runs too).
    bool prepare(VkCommandBuffer cmd, const Frame& f);
    bool running() const { return mRunning; }
    // After the per-particle lighting compute (its records final), pass 1's depth finished,
    // outside any rendering scope. partLitNow: the lighting ran (the fade has a record to
    // scale) -- else nothing is recorded (the billboards draw whole). shadowMode: the final
    // shadow_params.w; lightCount: this frame's local lights. markFn(cmd, name): sub-marks.
    template <class MarkFn>
    void recordCompute(VkCommandBuffer cmd, const VkDescriptorSet draw01[2], bool partLitNow, float shadowMode,
                       uint32_t lightCount, MarkFn&& markFn)
    {
        recordComputeImpl(cmd, draw01, partLitNow, shadowMode, lightCount,
                          [&](const char* n) { markFn(cmd, n); });
    }
    // Inside render pass 2's first scope, right after the froxel fog's apply (viewport /
    // scissor set). No-op unless recordCompute ran this recording.
    void recordComposite(VkCommandBuffer cmd, VkDescriptorSet dsetDraw0);
    bool compositeNow() const { return mRanNow; }
    // The renderer's emitter table copy for this frame (before it is streamed): flags the
    // converted systems' emitters (kEmitterFogFlag = clean_particle_common.glsl F_PFOG), so the
    // froxel injection (#721) leaves their particles to the bricks.
    static constexpr uint32_t kEmitterFogFlag = 0x20000000u;
    void markConverted(CleanEmitter* em, uint32_t n) const;
    // Nothing allocated (the renderer deletes the object then).
    bool idle() const;
    // Frees everything (waits for the device). Safe to call repeatedly.
    void destroy(VkDevice dev);

    // ---- GPU layouts (clean_pfog_common.glsl) ----
    struct BrickGPU { float org_vox[4]; float dims_slot[4]; float atlas[4]; float look[4]; float shape[4]; };
    static_assert(sizeof(BrickGPU) == 80, "BrickGPU = PfBrick");
    struct Params
    {
        float inv_vp[16], reproj[16];
        float dims[4], cam[4], march[4], look[4], noise[4], counts[4], atlas[4], atlasC[4], light[4], misc[4];
        float erode[4], settle[4];
        BrickGPU bricks[kMaxBricks];
    };
    static_assert(sizeof(Params) == 128 + 192 + 80 * kMaxBricks, "Params = PFogParams (std140)");
    struct SysGPU { uint32_t id[4]; float tex[4]; float col[4]; float heavy[4]; };
    static_assert(sizeof(SysGPU) == 64, "SysGPU = PfSys");
    struct TableGPU { uint32_t hdr[4]; uint32_t job[kMaxTexJobs][4]; SysGPU sys[kMaxSys]; };
    static constexpr VkDeviceSize kRecBytes = 64;   // PfRec
    static constexpr VkDeviceSize kResultsBytes = kMaxSys * 8u * 4u + kMaxTexJobs * 2u * 16u;

    // Exclusion reasons (log order).
    enum Reason : uint8_t
    {
        R_NONE = 0, R_HUD, R_ATTACH, R_BLEND, R_FLAGS, R_SMALL, R_FAST, R_SHORT, R_TEXTURE, R_TEXPEND,
        R_SCORE, R_DIST, R_BUDGET, R_COUNT
    };

private:
    void recordComputeImpl(VkCommandBuffer cmd, const VkDescriptorSet draw01[2], bool partLitNow, float shadowMode,
                           uint32_t lightCount, const std::function<void(const char*)>& mark);
    bool ensurePipelines(const Frame& f);
    bool ensureFixed(VkCommandBuffer cmd);                    // samplers' partners: rings, list, results, noise
    void destroyFixed();
    bool ensureAtlas(VkCommandBuffer cmd, uint32_t slots);    // atlas + accumulator for `slots`
    void destroyAtlas();
    bool ensureTargets(VkCommandBuffer cmd, uint32_t w, uint32_t h);
    void destroyTargets();
    void writeSets();
    void readBack();
    void classifyAll(const Frame& f, double now, float dt);
    void allocateBricks(const Frame& f, double now);
    void abortJobs();
    void logStats(double now);

    // ---- CPU classification state ----
    struct TexStats
    {
        bool     known = false;      // measured (the values below are valid)
        bool     inflight = false;   // a job was handed to a recording (result 2 frames later)
        float    meanA = 0.f, rgb[3] = {1.f, 1.f, 1.f}, sharp = 1.f, hf = 1.f;
        uint32_t w = 0, h = 0, tries = 0, passes = 0;
        int32_t  idx = -1;           // the bindless index it was measured at
        double   retryAt = 0.0, recheckAt = 0.0, lastUse = 0.0, jobAt = 0.0;
    };
    struct Sys
    {
        uint32_t source = 0;
        uint8_t  reason = R_NONE;
        bool     converted = false;   // hysteresis state
        bool     visible = false, evict = false, inTable = false;
        float    score = 0.f, target = 0.f, want = 0.f, cur = 0.f;
        float    prio = 0.f, dist = 0.f;
        float    pos[3] = {};
        float    texA = 0.f, texRGB[3] = {1.f, 1.f, 1.f};
        float    puff = 1.f;              // sprite size (m: the billows' base scale)
        float    settleV = 0.f;           // heavy settling speed (m/s, 0: none)
        float    floorFb = 0.f;           // fallback floor z (no floor map column)
        // the floor map's stamp (its slot cleared when the brick moves / every few seconds)
        float    floorOrg[3] = {}, floorVox = 0.f;
        double   floorAt = 0.0;
        LLUUID   texture;
        int32_t  slot = -1;           // atlas slot (-1: none)
        double   lastSeen = 0.0, lastVisible = 0.0;
        // bounds: GPU readback (gpu*) / CPU estimate (est*), the box in use
        float    gmin[3] = {}, gmax[3] = {};
        double   gAt = -1e9;
        uint32_t alive = 0;
        float    emin[3] = {}, emax[3] = {};
        float    bmin[3] = {}, bmax[3] = {};
        // brick geometry (quantised)
        bool     geom = false;
        float    org[3] = {}, vox = 0.f;
        uint32_t dims[3] = {};
    };
    std::map<LLUUID, TexStats>          mTex;
    std::unordered_map<uint32_t, Sys>   mSys;   // keyed by emitter slot
    struct RbMeta { bool valid = false; uint32_t nSys = 0, nTex = 0; uint32_t em[kMaxSys] = {}, src[kMaxSys] = {}; LLUUID tex[kMaxTexJobs]; int32_t texIdx[kMaxTexJobs] = {}; };
    RbMeta    mRbMeta[2];
    std::vector<LLUUID> mJobsNow;        // this recording's texture jobs
    std::vector<int32_t> mJobIdx;
    uint32_t  mReasonCount[R_COUNT] = {};
    uint32_t  mSeen = 0, mConverted = 0, mBlended = 0;

    // ---- GPU ----
    VkDeviceContext* mCtx = nullptr;
    VkSampler mSampLin = VK_NULL_HANDLE, mSampRep = VK_NULL_HANDLE, mSampNear = VK_NULL_HANDLE;
    VkDescriptorSetLayout mDsl = VK_NULL_HANDLE, mCompDsl = VK_NULL_HANDLE;
    VkDescPool mPool;
    VkDescriptorSet mDset[2][2] = {};       // [history parity][frame slot]
    VkDescriptorSet mCompSet[3][2] = {};    // [0 raw march, 1 + p history p][frame slot]
    VkPipelineLayout mLayout = VK_NULL_HANDLE, mCompLayout = VK_NULL_HANDLE;
    enum { P_TEX, P_BIN, P_SPLAT, P_RESOLVE, P_LIGHT, P_NOISE, P_MARCH, P_TEMPORAL, P_COMP, P_COUNT };
    VkPipeline mPipe[P_COUNT] = {};
    VkPipeline mStage[P_COUNT] = {};
    PipelineJobSlot mJob;
    bool mPipeReady = false, mPipeFailed = false;
    VkDescriptorSetLayout mPipeDsl[2] = {};
    VkFormat mPipeFormats[4] = {};

    GpuBuffer mParams[2], mTable[2], mReadback[2];
    GpuBuffer mList, mResults;
    GpuImage  mNoise, mNoiseD;               // base (64^3) + detail (32^3) noise
    GpuBuffer mFloor;                       // floor maps + density stats (kMaxBricks x kFloorWords)
    bool      mNoiseDone = false;
    uint32_t  mListCap = 0;                 // records (= the pool size it was made for)
    GpuImage  mDens, mCoarse, mVel, mLight, mLDir;
    GpuBuffer mAccum;
    uint32_t  mSlots = 0, mCols = 1, mRows = 1;   // atlas capacity (slots) and layout
    GpuImage  mMarchA, mMarchB, mHistA[2], mHistB[2];
    uint32_t  mMW = 0, mMH = 0;             // allocated
    uint32_t  mActW = 0, mActH = 0;         // this recording's march extent (<= allocated)
    uint32_t  mPrevActW = 0, mPrevActH = 0;
    bool      mNear = false, mNearAlloc = false;   // near-fog resolution now / targets sized for it
    double    mNearOffSince = 0.0;
    bool      mHistValid = false;
    uint32_t  mHistCur = 0;
    // What the sets name (rewritten, device idle, when any changes).
    VkBuffer  mSetPool = VK_NULL_HANDLE, mSetLight = VK_NULL_HANDLE;
    VkImageView mSetDepth = VK_NULL_HANDLE;
    uint64_t  mSetDepthGen = 0;
    bool      mSetsDirty = true;
    VkBuffer  mPartPool = VK_NULL_HANDLE, mPartLight = VK_NULL_HANDLE;
    VkImageView mDepthView = VK_NULL_HANDLE;
    uint64_t  mDepthGen = 0;
    double    mRetryAt = 0.0;               // #697: a refused allocation retries after this
    double    mAtlasRetryAt = 0.0;          // (the atlas / march targets)
    uint32_t  mPeakSlots = 0;
    double    mLastBricksAt = 0.0, mShrinkSince = 0.0;

    // ---- this recording ----
    bool      mRunning = false, mRanNow = false, mOffscreen = false, mReset = true;
    uint32_t  mSlot = 0;
    uint32_t  mPartMax = 0;
    uint32_t  mNumBricks = 0, mNumSys = 0, mNumJobs = 0;
    uint32_t  mBrickSlot[kMaxBricks] = {};
    bool      mFloorClear[kMaxBricks] = {};  // this recording clears the brick's floor columns
    uint32_t  mBrickSys[kMaxBricks] = {};    // the brick's system (mSys key)
    uint32_t  mShadowLights = 0;
    uint32_t  mFrame = 0;
    uint32_t  mLiveSlots = 0, mCapSlots = 0;
    float     mPrevVP[16] = {};
    double    mPrevEye[3] = {};
    double    mLastPrepare = 0.0;
    double    mLogAt = 0.0;
    uint64_t  mLogSig = 0;
};

#endif
