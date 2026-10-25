/**
 * @file llcleanclustercontrolvk.h
 * @brief Cluster LOD (#642 / #749, OWNER: 2c): the tolerance controller -- one pressure
 *        knob (the pixel tolerance), protection by a lower tolerance, per-view tolerance and
 *        error parameters, and the temporal stability of the cut.
 *
 * CPU only (no Vulkan objects): owned by LLCleanClusterVK (control()), updated once per frame
 * in its tick() from what the renderer hands over (LLClusterFrameIn::pressure / main view),
 * read by the core's recordCull (per-view tolerance + LOD camera) and by 1b's gatherer
 * (ClusterInstance::tol_scale).
 *
 * ---- ONE KNOB: THE PIXEL TOLERANCE --------------------------------------------------------
 *   tol = RenderVKClusterTolerancePx x 2^level, level 0..3 (1x / 2x / 4x / 8x), eased in log
 *   space (raised over ~0.5 s, released over ~2 s) so a level change never pops the whole
 *   scene in one frame. Two inputs raise the level, the larger wins:
 *   - GPU TIME (the #613 measurement): the sampled rigged + cluster raster GPU time over its
 *     budget (RenderVKRiggedGpuBudgetMs, 0 = 30 % of the target frame) on a GPU-bound frame
 *     slower than the target ("Keep my FPS" hands its target over through
 *     LLVKAutoTune::rigTargetFps, exactly as for the old controller). Raised one level after
 *     0.5 s over and >= 2 s since the last change -- and only while the cluster raster is a
 *     real part of that time (the tolerance cannot help legacy-drawn work). Released one
 *     level per dwell (10 s, doubled after a release that was re-raised within 30 s) when
 *     comfortably under: fast frame, or the predicted time one level down (cluster time x 4:
 *     triangles ~ 1 / tol^2) fits 70 % of the budget. No timestamps: never raises, releases.
 *   - VRAM: the VRAM ladder's depth (any rung -> 1, a third of the rungs -> 2, two thirds ->
 *     3; < 256 MB room -> >= 2), released 2 s after the ladder calms (its own hysteresis).
 *   Settings are never written (soft degrade). Snapshots (offscreen recordings) use the base
 *   tolerance with no pressure and no lag: a 4K photo gets the detail its resolution asks.
 *
 * ---- PROTECTION BY LOWER TOLERANCE (no exemption list) ------------------------------------
 *   Avatars get ClusterInstance::tol_scale = mix(1, min(1, protect / tol), w):
 *     protect = RenderVKClusterProtectPx x sqrt(2^level)   (pressure reaches protected avatars
 *                                                          at a reduced multiple: 8x -> 2.8x)
 *     w       = 1 for your own avatar; else smoothstep(0.10, 0.60, avatar height on screen /
 *               viewport height), measured from the LOD camera.
 *   Continuous in distance and resolution: no top-N count, no metre or pixel cliff (the old
 *   10 avatars / 3 m / 100 px rules were what made close avatars go boxy).
 *
 * ---- PER-VIEW ------------------------------------------------------------------------------
 *   Each role has its own tolerance scale (x the knob): main / snapshot / mirror 1, sun
 *   cascades 2 (shadow-map texels, filtered), probe faces 4 (pre-filtered, low-res capture).
 *   RT / other (CLROLE_OTHER, or any view with CLV_RT): tolerance and camera exactly as the
 *   caller set them, untouched by pressure -- the RT path (2b) sets its own tolerance
 *   (roleTolPx(CLROLE_OTHER) = RenderVKClusterRTTolerancePx, for it to use). NOTE for 2a / 2b:
 *   LLClusterViewIn::role defaults to CLROLE_MAIN -- an RT view must set CLV_RT or
 *   CLROLE_OTHER, or it inherits the main view's tolerance and LOD-camera lag. Error parameters per role
 *   (llcleanclusterformat.h ERROR UNITS): perspective px_scale = 0.5 x H x P[1][1] (probe
 *   face: face_res / 2; snapshot: its own height), orthographic px per metre for cascades.
 *
 * ---- TEMPORAL STABILITY (RenderVKClusterHysteresis = h) -------------------------------------
 *   Small camera motion under TAA / FSR must not flip cuts every frame, and the exactly-one-
 *   cut rule (clCutRenders) must stay intact, so the band is NOT a pair of thresholds inside
 *   the cut (a parent / child test at different tolerances can draw both or neither). The
 *   inputs of the projection are held instead, conservatively:
 *   - LOD CAMERA (ClusterView::lod_cam): per view role, lags the live camera; it jumps to the
 *     live camera once they are more than D = h x 2 m apart (0.3 m at the default), and the
 *     projection subtracts D from every distance. |live - lod| <= D, so the projected error
 *     is never under-estimated; within the dead band every input is constant and no cut
 *     changes at all (an idle avatar's camera bob, hand jitter). The frustum / cone /
 *     occlusion tests keep the live camera.
 *   - px_scale (zoom, resolution, cascade fit): raised at once (finer), lowered only once
 *     it fell more than h below the held value ("a cut coarsens only after the margin").
 *   - the tolerance itself: eased (above); protection follows the held LOD camera.
 *   h = 0: lod_cam = cam_pos, no margin, px_scale live (the plain projection).
 *
 * Diagnostics: statsLine (the VKMem fragment) and summaryLine (the rate-limited cluster line,
 * compact for bug-report excerpts).
 */
#pragma once

#include <cmath>
#include <cstdint>
#include <string>

// What a cluster view is for (arena reservation, tolerance scale, LOD camera state).
enum LLClusterViewRole : uint32_t
{
    CLROLE_MAIN = 0,     // the main view (and a snapshot's recordings: offscreen)
    CLROLE_SHADOW,       // sun cascades (orthographic)
    CLROLE_PROBE,        // reflection probe faces
    CLROLE_MIRROR,       // the planar mirror
    CLROLE_OTHER,        // RT / anything else: tolerance and camera left as given
    CLROLE_COUNT
};

// The #613 GPU-time measurement and the frame target, handed over by the renderer per frame.
struct LLClusterPressureIn
{
    bool     gpu_ok = false;          // a fresh timestamp sample exists (< 3 s)
    float    frame_ms = 0.f;          // smoothed frame time
    float    gpu_frame_ms = 0.f;      // GPU span of the world recording (sampled)
    float    rig_gpu_ms = 0.f;        // legacy rigged spans (pre-skin, weld, rigged raster)
    float    cluster_gpu_ms = 0.f;    // cluster spans the cut drives (pre-pass + G-buffer + the #747 pre-skin)
    float    target_ms = 1000.f / 60.f;   // LLVKAutoTune::rigTargetFps (RenderVKRiggedLodTargetFPS)
    float    budget_ms = 5.f;         // RenderVKRiggedGpuBudgetMs (0 = 30 % of the target)
    bool     perf_on = true;          // RenderVKRiggedScreenLOD (the perf trigger's switch)
    uint32_t vram_ladder = 0;         // engaged VRAM ladder rungs
    uint32_t vram_rungs = 3;          // rungs the ladder has
    uint64_t vram_avail = ~0ull;      // bytes of VRAM room
};

class LLCleanClusterControlVK
{
public:
    static constexpr uint32_t kMaxLevel = 3;          // 2^3 = 8x
    static constexpr uint32_t kMaxSub = 8;            // LOD-camera states per role (cascades, faces)

    // Once per frame (core tick). offscreen: a snapshot recording -- the live state is kept
    // untouched, the views get the base tolerance and the live camera.
    void update(const LLClusterPressureIn& in, double now, bool offscreen);
    // The main view of this frame (before the gatherer runs): the culling camera, its
    // px_scale and viewport height (protection is measured from its LOD camera).
    void setMainView(const float cam[3], float px_scale, float viewport_h, float znear);

    // A view's tolerance (px) and LOD camera: fills lod_cam (xyz + margin) and replaces
    // px_scale by its held value. role CLROLE_OTHER: tolerance / camera left as given.
    void applyView(uint32_t role, uint32_t sub, const float cam[3], bool ortho, float& px_scale, float& tol_px,
                   float lod_cam[4]);
    float roleTolPx(uint32_t role) const;
    // ClusterInstance::tol_scale of an avatar instance (static instances: 1): the protection (<= 1).
    // (#1010: arena_out = the ARENA term below, >= 1 -- the gatherer multiplies it in AFTER its alpha
    // tolerance cap, so a crowd's hair / lashes coarsen with their avatar too.)
    float avatarTolScale(bool self, const float av_pos[3], float av_height_m, float* arena_out = nullptr) const;
    // #1010 ARENA PRESSURE (2a's skinned arena, llcleanclusterskinvk.h): a log2 exponent P >= 0, set
    // once per frame after the skin module read its demand back (applies from the next gather).
    // Every avatar instance's tol_scale gets 2^clamp(P - L, 0, kArenaMaxExtra), L = log2(the
    // avatar's on-screen height / kArenaRefFrac of the viewport) clamped to 0..kArenaSpan, your own
    // avatar kArenaSelfL: the smallest / farthest avatars coarsen first, yours after all others,
    // every mesh of one avatar by the same factor (it stays whole). Snapshots too (the arena is
    // shared). Not part of level / reason: a separate term (summary "arena P").
    void setArenaPressure(float p) { mArenaP = (std::isfinite(p) && p > 0.f) ? p : 0.f; }
    float arenaPressure() const { return mArenaP; }
    static constexpr float kArenaMaxExtra = 8.f;     // 256x tolerance at most
    static constexpr float kArenaSpan = 6.f;         // L of an avatar at 64 % of the viewport height
    static constexpr float kArenaSelfL = 9.f;
    static constexpr float kArenaRefFrac = 0.01f;

    uint32_t level() const { return mLevel; }
    float tolPx() const { return mOffscreen ? mBase : mTol; }
    float basePx() const { return mBase; }
    const char* reason() const;
    void statsLine(std::string& out) const;
    void summary(std::string& out) const;   // compact: "tol 1.0 (x1 none)"

private:
    struct Lag { float cam[3] = {}; float px = 0.f; bool valid = false; };
    Lag& lag(uint32_t role, uint32_t sub) { return mLag[role < CLROLE_COUNT ? role : CLROLE_OTHER][sub % kMaxSub]; }

    // settings (read in update)
    float mBase = 1.f, mProtect = 0.5f, mRtTol = 2.f, mHyst = 0.15f;
    // pressure state
    uint32_t mLevel = 0, mPerf = 0, mVram = 0;
    double mOverSince = 0.0, mUnderSince = 0.0, mChangeAt = 0.0, mReleaseAt = 0.0, mVramCalmSince = 0.0;
    uint32_t mFlip = 0;
    float mTol = 1.f;                 // eased tolerance (px)
    double mLastNow = 0.0;
    bool mOffscreen = false;
    // last inputs (diagnostics)
    LLClusterPressureIn mIn;
    // main view / LOD cameras
    float mMainCam[3] = {};
    float mMainPx = 0.f, mMainH = 0.f, mZnear = 0.1f;
    bool mMainValid = false;
    Lag mLag[CLROLE_COUNT][kMaxSub];
    uint32_t mLagJumps = 0;           // LOD camera re-anchors (main), for the diag line
    float mArenaP = 0.f;              // #1010 arena pressure exponent (setArenaPressure)
};
