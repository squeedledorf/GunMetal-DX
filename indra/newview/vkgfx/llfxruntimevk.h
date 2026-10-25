/**
 * @file llfxruntimevk.h
 * @brief Slipstream's built-in ReShade FX effects runtime on the VK post chain (#544),
 *        with the safety rules of #546, snapshots (#547) and the Slipstream extensions
 *        of #548. The user-facing state (which effects, order, values, presets) is
 *        SLFxStack (newview/slfxstack.h); this is the GPU side.
 *
 * INJECTION POINTS (LLCleanRendererVK::recordWorld, small hooks):
 *  - DISPLAY point (default): after recordAA (FXAA / SMAA) -- tonemapped, display
 *    resolution, display-referred, before the RLVa @setsphere passes (so RLVa vision
 *    restrictions stay unbypassable), the Lightbox final pass, the snapshot capture,
 *    selection outlines, 3D overlays, name tags, HUD and UI. The renderer forces its
 *    final pass (prepareFinal(force)) while FX runs; the chain writes OUR back buffer
 *    (displayChainTarget()), recordDisplay() runs the techniques and copies the result
 *    into the chain's own image (mFinalColor, or the sphere ping-pong target), leaving
 *    it COLOR_ATTACHMENT_OPTIMAL exactly as the chain would have.
 *  - HDR point (#548, per effect: technique annotation slipstream_point = "hdr" or the
 *    effect's point setting): after bloom, before recordTonemap, on the linear scene
 *    (mHdr, RGBA16F) exposure-scaled; BUFFER_COLOR_SPACE 2, BUFFER_COLOR_BIT_DEPTH 16.
 *    The result is written back (exposure divided out) so the tonemapper still applies.
 * Nothing is allocated or recorded while no technique is enabled.
 *
 * INPUTS (texture semantics; all display resolution, unjittered, FX-owned copies made by
 * fx_resolve.comp each frame they are referenced):
 *   COLOR                   the back buffer (BackBuffer; UNORM + SRGB views)
 *   DEPTH                   ReShade convention: reversed, encoded so ReShade.fxh's
 *                           GetLinearizedDepth() = view depth / RESHADE_DEPTH_LINEARIZATION_FAR_PLANE
 *                           (the camera far clip, rounded up to 64 m: a recompile trigger)
 *   SLIPSTREAM_NORMAL       RGBA16F view-space normal (w 1 = pre-pass normal, 0.5 = from depth)
 *   SLIPSTREAM_MOTION       RG16F motion, UV units, current - previous, unjittered
 *   SLIPSTREAM_AVATAR_MASK  R32F avatar id (slot + 1; 0 = no avatar) from the provider
 *   SLIPSTREAM_SCENE_HDR    RGBA16F linear scene x exposure (at either point)
 * Unknown semantics bind a 1x1 black texture (with a warning in the effect's errors).
 *
 * SAFETY (#546): user content never goes through VK_CHECK. Every create call for an
 * effect is checked; a failure disables that effect with a readable error. Formats /
 * sizes are validated against the device before any image is created. Caps: texture
 * edge <= device max (and <= 16384), mips <= 16, passes per technique <= 64, dispatch
 * <= device max, effect VRAM <= SLFxVramBudgetMB and the live VRAM budget (soft
 * pressure parks the most expensive effects until it clears). FX pipelines request
 * robustBufferAccess2 / robustImageAccess2 (VK_EXT_pipeline_robustness) when the
 * device enabled it.
 *
 * THREADING: main thread, except the compile worker (preprocess + parse + SPIR-V +
 * shader modules + layouts + pipelines + texture file decode). Images, descriptor sets
 * and uploads are made on the main thread when an effect first goes live.
 * Metadata requests (requestMetadata: the UI's reflection of every installed effect)
 * run ONLY the reshadefx front end -- no shader module, layout or pipeline is created;
 * pipelines are built only for permutations of planned (active) techniques.
 *
 * COMPILE WATCHDOG / SHUTDOWN: a vkCreate*Pipelines call cannot be cancelled, and a user
 * shader can make the driver's compiler run for minutes (NVIDIA: a driver thread pinned in
 * libnvidia-gpucomp). So nothing ever waits on the worker without a bound:
 *  - a job running longer than kCompileTimeoutSec is abandoned: its effect fails with
 *    "compile took too long" and is quarantined (SLFxStack::takeQuarantineRequests), the
 *    stuck worker is retired (its thread keeps running inside the driver; whatever it
 *    produces is destroyed when -- if -- it returns) and a fresh worker takes the queue;
 *  - snapshot (offscreen) compiles wait for the worker at most kSyncWaitSec;
 *  - detachDevice / the destructor wait at most kShutdownWaitMs, then leave a worker that
 *    is still inside the driver alone (detached, never joined) and report it through
 *    compileInDriver(): the device must then NOT be destroyed (vkDestroyDevice while
 *    another thread is inside vkCreate*Pipelines on it is undefined behaviour); the
 *    process exit reclaims it.
 * User-effect pipelines use a private, never-persisted VkPipelineCache (not the viewer's
 * vkSharedPipelineCache()): user shaders do not bloat vk_pipeline_cache.bin, and a stuck
 * compile never holds a cache the viewer's own save path reads.
 *
 * TARGET GENERATION RULE (llcleanrenderervk.h): every descriptor set naming a renderer
 * view (depth, normals, motion, HDR scene, exposure, avatar mask) is per frame-ring slot
 * and rewritten on every use; sets naming only FX-owned images are written once per
 * size entry. No set of ours keys on a view handle.
 */
#ifndef LL_LLFXRUNTIMEVK_H
#define LL_LLFXRUNTIMEVK_H

#include "vk_device.h"
#include "llfxcompile.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class LLSD;

class LLFxRuntimeVK
{
public:
    enum Point : uint8_t { POINT_DISPLAY = 0, POINT_HDR = 1 };
    enum Scope : uint8_t { SCOPE_ALL = 0, SCOPE_AVATARS = 1, SCOPE_NOT_AVATARS = 2 };

    // ---- what the renderer provides each frame (filled at the hook) ----
    struct Frame
    {
        VkDeviceContext* ctx = nullptr;
        uint32_t width = 0, height = 0;                 // display extent (= the chain's)
        VkFormat colorFormat = VK_FORMAT_UNDEFINED;     // the LDR chain format (mColorFormat)
        uint32_t ringSlot = 0, ringCount = 2;           // frame-ring slot (its last use has completed)
        bool offscreen = false;                         // snapshot / offscreen recording
        bool captureFrame = false;                      // this recording ends in a user snapshot
        bool skip = false;                              // offscreen warm-up / non-final supersample frame
        // World depth (render extent, jittered, reversed-Z, DEPTH_ATTACHMENT_OPTIMAL at the hooks).
        VkImage depthImage = VK_NULL_HANDLE;
        VkImageView depthView = VK_NULL_HANDLE;
        uint32_t rw = 0, rh = 0;
        float jitter[2] = {0.f, 0.f};                  // render pixels, x right / y down
        float lin[4] = {0.f, 1.f, 1.f, 0.f};            // view z = (d*lin0 + lin1) / (d*lin2 + lin3)
        float unproj[2] = {1.f, 1.f};                   // 1 / P[0], 1 / P[5] (normal fallback)
        // Optional render-extent inputs, SHADER_READ_ONLY_OPTIMAL when non-null.
        VkImageView normalView = VK_NULL_HANDLE;        // pre-pass view-space oct normals (AO frames)
        VkImageView motionView = VK_NULL_HANDLE;        // motion vectors (temporal frames)
        // The linear HDR scene (display extent, SHADER_READ_ONLY_OPTIMAL at both hooks).
        VkImage hdrImage = VK_NULL_HANDLE;
        VkImageView hdrView = VK_NULL_HANDLE;
        VkFormat hdrFormat = VK_FORMAT_UNDEFINED;
        VkImageView exposureView = VK_NULL_HANDLE;      // 1x1 R32F, GENERAL (storage)
        float exposureConst = 1.f;
        bool exposureDyn = false;
        // VRAM state (llvrambudgetvk): live budget and ladder mode (0 calm, 1 soft, 2 reserve).
        uint64_t vramBudget = 0, vramUsage = 0;
        uint32_t vramMode = 0;
        uint64_t frameNumber = 0;                       // renderer frame counter (breadcrumbs / timers)
    };

    // ---- what SLFxStack provides (main thread) ----
    struct PlanTech
    {
        std::string effect;      // full path of the .fx (UTF-8)
        std::string technique;   // technique name
        uint8_t scope = SCOPE_ALL;
        uint8_t feather = 1;     // scope feather radius, display pixels (0..8)
    };
    struct EffectConfig
    {
        std::vector<std::pair<std::string, std::string>> defines;   // global + per-effect
        uint8_t point = POINT_DISPLAY;   // effect default (a technique annotation can override)
    };
    struct Inputs   // per-frame user input for uniform "source" specials
    {
        float mouseX = 0.f, mouseY = 0.f;    // window pixels, top-left origin
        float windowW = 1.f, windowH = 1.f;  // window size in pixels
        float wheelDelta = 0.f;              // this frame
        bool  mouseDown[5] = {};
        std::function<bool(int vk)> keyDown; // Windows virtual-key code -> held (may be empty)
        float farClip = 256.f;               // camera far clip (m): RESHADE_DEPTH_LINEARIZATION_FAR_PLANE
    };

    // Status of one effect for the UI (the main-size compile).
    struct EffectStatus
    {
        bool compiling = false;
        bool compiled = false;      // last compile succeeded
        bool failed = false;        // last compile / resource creation failed: disabled
        std::string errors;         // compile errors / warnings / runtime errors
        std::shared_ptr<const slfx::CompileResult> meta;   // module for reflection (UI)
        uint64_t vramBytes = 0;     // live GPU memory of its textures (all sizes)
        uint32_t goodFrames = 0;    // frames recorded since it went live (provisional count)
        bool parked = false;        // held back by VRAM pressure right now
    };

    static LLFxRuntimeVK& instance();

    // ---- renderer lifecycle ----
    void attachDevice(VkDeviceContext* ctx);   // idempotent; called from the renderer hook
    void detachDevice();                       // renderer shutdown: frees every GPU object (waits idle)
    bool hasDevice() const { return mCtx != nullptr; }
    // A compile worker is (still) inside a driver call after the bounded shutdown wait:
    // the caller must not destroy the VkDevice (see COMPILE WATCHDOG above).
    bool compileInDriver() const;

    // ---- SLFxStack -> runtime (main thread) ----
    void setEnabled(bool on);                  // master switch (frees GPU resources when off)
    bool enabled() const { return mEnabled; }
    void setPlan(const std::vector<PlanTech>& order, const std::map<std::string, EffectConfig>& configs,
                 const std::vector<std::string>& includeDirs, const std::vector<std::string>& textureDirs);
    void setUniformValues(const std::string& effect, const LLSD& values);   // name -> value
    void setInputs(const Inputs& in) { mInputs = in; }
    // Main-thread idle (SLFxStack): collects finished compiles even while effects are off
    // or no frame is recorded, so the UI's reflection (techniques, uniforms) arrives.
    void tick();
    // Metadata compile for the UI (current display size, display point) without running it.
    void requestMetadata(const std::string& effect);
    // Drop every compiled permutation (hot reload); the next frames recompile.
    void reloadAll();
    void reloadEffect(const std::string& effect);
    // 360 capture / --no-fx: suppress FX for the next recordings while > 0.
    void pushSuppress() { ++mSuppress; }
    void popSuppress() { if (mSuppress > 0) --mSuppress; }
    // Snapshot floater size changed: compile that size ahead of the shot.
    void warmSize(uint32_t w, uint32_t h);

    // ---- runtime -> SLFxStack ----
    EffectStatus status(const std::string& effect) const;
    float techniqueGpuMs(const std::string& effect, const std::string& technique) const;
    uint64_t metaGeneration() const { return mMetaGen.load(); }   // bumps when any status changes
    // Effects whose compile hit the watchdog since the last call: (effect path, reason).
    // SLFxStack quarantines them (persisted) so the next session does not retry them.
    std::vector<std::pair<std::string, std::string>> takeQuarantineRequests();
    // Last snapshot's FX note ("" = FX ran or was not wanted), e.g. "not enough video memory".
    const std::string& lastSnapshotNote() const { return mSnapNote; }
    std::vector<std::string> activeEffects() const;   // effects recorded in the last live frame

    // P5: the avatar-id mask provider (R16_UINT, display extent, unjittered, 0 = no avatar,
    // SHADER_READ_ONLY_OPTIMAL whenever recordHdr / recordDisplay run). Null / empty
    // provider or VK_NULL_HANDLE result: a 1x1 zero mask ("avatars only" = nothing).
    static void setAvatarMaskProvider(std::function<VkImageView()> provider);

    // ---- the renderer hooks (main thread, inside recordWorld) ----
    // HDR point: before recordTonemap. true = mHdr was rewritten (left SHADER_READ_ONLY).
    bool recordHdr(VkCommandBuffer cmd, const Frame& f);
    // Display point, step 1 (before prepareFinal): does this frame run display-point FX?
    bool wantsDisplay(const Frame& f);
    // Step 2 (after prepareFinal succeeded): our back buffer, COLOR_ATTACHMENT_OPTIMAL,
    // for the tonemap / glow / AA chain to write instead of the chain's own image.
    // VK_NULL_HANDLE = FX cannot run this frame (the chain then writes its own target).
    VkImageView beginDisplay(VkCommandBuffer cmd, const Frame& f);
    // Step 3 (after recordAA): run the techniques, write the result into dstView
    // (COLOR_ATTACHMENT_OPTIMAL in and out; the chain's format).
    void recordDisplay(VkCommandBuffer cmd, const Frame& f, VkImageView dstView);
    // endOffscreen: drop the snapshot-size permutations and targets.
    void onEndOffscreen();
    // Whether an enabled effect wants SLIPSTREAM_NORMAL (the renderer may force the
    // normal pre-pass; without it the normals are reconstructed from depth).
    bool wantsNormals() const { return mWantNormals; }
    // An active technique samples the avatar mask or runs with an avatar scope (#548/#549);
    // the renderer keeps the cel module's mask produced while this is set.
    bool wantsAvatarMask() const { return mWantMask; }

    // Device-lost forensics (any thread; static storage): the techniques recorded in the
    // last frame, oldest first, "technique@file.fx" -- read by SLFxStack::deviceLostSummary.
    static std::string lastRecordedSummary();

    struct Perm;     // one compiled effect at one (size, point, defines, far plane) key
    struct Shared;   // per (size, point): back buffer pair, inputs, scope / blit targets
    struct Worker;   // the compile thread
    struct Job;      // one compile request

private:
    LLFxRuntimeVK();
    ~LLFxRuntimeVK();
    LLFxRuntimeVK(const LLFxRuntimeVK&) = delete;
    LLFxRuntimeVK& operator=(const LLFxRuntimeVK&) = delete;

    std::string configKey(const std::string& effect) const;
    std::string permKey(const std::string& effect, uint32_t w, uint32_t h, uint8_t point, float farPlane) const;
    std::shared_ptr<Perm> findOrQueue(const std::string& effect, uint32_t w, uint32_t h, uint8_t point,
                                      VkFormat bbFormat, bool sync);
    void pollJobs();
    void checkWatchdog();       // main thread: retire a worker stuck past kCompileTimeoutSec
    void retireWorker();        // move mWorker to mRetired (abandoned) and start a fresh one
    bool ensureLive(Perm& p, Shared& s, const Frame& f, std::string& err);
    Shared* ensureShared(uint32_t w, uint32_t h, uint8_t point, VkFormat bbFormat);
    bool ensureSharedInput(Shared& s, int kind);
    bool ensureHostPipes(VkFormat bbFormat, VkFormat dstFormat, VkFormat hdrFormat);
    bool initHost();
    void destroyHost();
    void destroyPerm(Perm& p);
    void destroyShared(Shared& s);
    void freeAllGpu();
    bool runPoint(VkCommandBuffer cmd, const Frame& f, uint8_t point, Shared& s);
    void resolveInputs(VkCommandBuffer cmd, const Frame& f, Shared& s, uint32_t needMask);
    void recordTechnique(VkCommandBuffer cmd, const Frame& f, Perm& p, Shared& s, size_t techIndex,
                         uint8_t scope, uint8_t feather);
    void writeUniforms(Perm& p, const Frame& f, Shared& s);
    void readTimers(uint32_t slot);
    void frameStart(VkCommandBuffer cmd, const Frame& f);
    uint8_t techPoint(const PlanTech& pt) const;   // effect setting or technique annotation
    void setStatusError(const std::string& effect, const std::string& err);
    void bumpMeta() { ++mMetaGen; }
    void applyPressure(const Frame& f);
    uint64_t budgetBytes(const Frame& f) const;

    VkDeviceContext* mCtx = nullptr;
    bool mEnabled = false;
    std::atomic<int> mSuppress{0};
    bool mWantNormals = false;
    bool mWantMask = false;
    std::atomic<uint64_t> mMetaGen{1};
    Inputs mInputs;
    std::string mSnapNote;

    // Plan
    std::vector<PlanTech> mPlan;
    std::map<std::string, EffectConfig> mConfigs;
    std::vector<std::string> mIncludeDirs, mTextureDirs;
    std::unordered_map<std::string, std::shared_ptr<LLSD>> mValues;
    uint64_t mPlanGen = 1;

    // Compiled permutations + their per-(size, point) shared targets.
    std::unordered_map<std::string, std::shared_ptr<Perm>> mPerms;
    std::vector<std::unique_ptr<Shared>> mShared;
    std::unique_ptr<Worker> mWorker;
    // Abandoned workers whose thread may still be inside a driver call. Polled for their
    // finished perms (destroyed as orphans) and joined once they exit; never waited on.
    std::vector<std::unique_ptr<Worker>> mRetired;
    std::unordered_map<std::string, std::string> mTimedOut;   // effect -> reason (no recompiles)
    std::vector<std::pair<std::string, std::string>> mQuarantine;   // for takeQuarantineRequests
    VkPipelineCache mUserCache = VK_NULL_HANDLE;   // private cache of user-effect pipelines
    bool mStuckAtDetach = false;                    // compileInDriver() after detachDevice
    mutable std::mutex mStatusMutex;
    std::unordered_map<std::string, EffectStatus> mStatus;   // by effect path
    std::unordered_map<std::string, uint32_t> mParked;       // effect -> parked by pressure (frame of parking)
    uint32_t mCalmFrames = 0;
    uint32_t mMainW = 0, mMainH = 0;                          // last live display size
    VkFormat mMainFormat = VK_FORMAT_UNDEFINED;
    float mFarPlane = 256.f;                                  // compiled far plane (rounded)
    uint64_t mFrameCounter = 0;
    uint64_t mStartedRecording = ~0ull;   // frameStart: renderer frame number of the last start
    bool mStartedOffscreen = false;
    std::vector<std::string> mActive;

    // Host objects (our own shaders): samplers, layouts, pipelines.
    struct Host;
    std::unique_ptr<Host> mHost;
    bool mHostFailed = false;
    bool mRobustness = false;   // VK_EXT_pipeline_robustness usable

    // GPU timers (per ring slot): two timestamps per technique.
    static constexpr uint32_t kMaxTimedTechs = 64;
    VkQueryPool mQueryPool[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    std::vector<std::pair<std::string, std::string>> mTimedTechs[2];   // effect, technique per query pair
    bool mTimerIssued[2] = {false, false};
    std::unordered_map<std::string, float> mGpuMs;                     // "effect\ntechnique" -> smoothed ms
    float mTimestampPeriod = 0.f;
};

// RAII: no FX for the recordings in this scope (360 capture: seams; see #547).
struct LLFxSuppressScope
{
    LLFxSuppressScope() { LLFxRuntimeVK::instance().pushSuppress(); }
    ~LLFxSuppressScope() { LLFxRuntimeVK::instance().popSuppress(); }
    LLFxSuppressScope(const LLFxSuppressScope&) = delete;
    LLFxSuppressScope& operator=(const LLFxSuppressScope&) = delete;
};

#endif
