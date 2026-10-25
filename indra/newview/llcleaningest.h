/**
 * @file llcleaningest.h
 * @brief Clean-pipeline ingest (Vulkan): LL object list -> clean stores. GL-free.
 *        Split out of the old GL clean renderer (GL-B3/B4).
 */

#pragma once

#include "stdtypes.h"

#include <deque>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include "lluuid.h"
#include "v3math.h"
class LLViewerObject;
class LLFace;

// TERRAIN BAKE INPUTS (#811, vkgfx/llcleanterrainbakevk.cpp): per region, what its terrain
// patches were last ingested from -- the composition coord at every grid point (the
// per-vertex value the terrain meshes carry) and a content hash over every ingested patch's
// vertices (heights) + composition. A terrain edit, a neighbour region arriving (edge
// samples) or a composition change moves the hash; the bake re-runs on it. Written by the
// terrain patch ingest, dropped with the region (onRegionRemoved). Main thread.
struct LLCleanTerrainInputs
{
    S32 gpe = 0;                            // grid points per edge (LLSurface::getGridsPerEdge)
    S32 ppe = 0;                            // patches per edge
    F32 mpg = 0.f;                          // metres per grid
    std::vector<F32> comp;                  // gpe * gpe composition coords (row-major, y outer)
    std::unordered_map<U32, U64> patch;     // patch (j0 * gpe + i0) -> its content hash
    U64 hash = 0;                           // sum over patches of a mix of (patch, its hash)
    bool complete() const { return ppe > 0 && patch.size() >= (size_t)ppe * (size_t)ppe; }
};

class LLCleanIngest
{
public:
    static LLCleanIngest& instance();

    // LLWorld::removeRegion: forget per-region ingest state.
    static void onRegionRemoved(U64 region_handle);
    // The terrain bake's inputs for a region (#811), or null (no terrain patch ingested).
    static const LLCleanTerrainInputs* terrainInputs(U64 region_handle);

    // Agent-origin shift (region crossing): re-base instance transforms + particles.
    void shiftOrigin(const LLVector3& offset);

    // VK login ingest tick (step 3): in Vulkan mode the GL renderFrame() that
    // normally drove selfRegisterSweep() is gone, so displayVulkanUI() pumps
    // one sweep slice per frame through this public entry. GL default path unaffected.
    // Section timers (RenderVKSectionLog): ing_priority, sweep_total, ing_gltf, ing_gc, ing_diag.
    void vkIngestTick();
    // <GMDX> The last full sweep cycle changed nothing (the region map capture waits for it).
    bool isSweepIdle() const { return mSweepIdle; }
    // glTF scene assets (#39): prune dead records, re-pose animated nodes, VKGLTF summary.
    void vkGLTFTick();
    // RenderVKVehicleDiag (#96): while seated on an object, one VKVehicleDiag line per
    // frame (seat root pose vs uploaded instance pose, moved-queue slice, avatar offset
    // in the seat frame, camera) to locate vehicle stutter.
    void vkVehicleDiagTick();
    // SELECT-TO-DIAGNOSE (RenderVKIngestDiag, #88). On an edit-selection change, log one
    // "VKIngestDiag" INFO block per selected object (every linkset member) describing its
    // ingest state face by face, then a one-line summary of what is NOT in the GPU scene.
    void vkIngestDiagTick();
    // roots: dump these objects' linksets instead of the selection (avatar dump).
    void vkIngestDiagDump(const std::vector<LLViewerObject*>* roots = nullptr, const std::string& label = std::string());
    void vkAvatarDumpTick();   // RenderVKDumpAvatar
    std::string mAvDumpDone;
    F64 mAvDumpRetry = 0.0;
    // VK: reclaim mesh geometry that no live instance has referenced for a long
    // time, gated on resident-geometry pressure (RenderVKGeometryGC).
    void vkGeometryGC();

    // Per-face ingest (implemented in llvovolume.cpp, next to the LLVOVolume state it
    // reads; was LLVolumeGeometryManager::registerCleanFace). Builds + upserts the
    // GPU-driven clean instance for ONE face directly from the face's object/volume/TE
    // state, with no dependency on LL's spatial group or vertex buffers.
    // force_reingest bypasses the static content-signature dedup so the sweep can
    // rebuild a stale/corrupt instance.
    static void registerCleanFace(LLFace* facep, bool force_reingest);
    // VK ingest worker (RenderVKIngestWorker): commit finished async captures and
    // collect the objects waiting on them (render thread, ingest sync point).
    // Implemented in llvovolume.cpp.
    static void vkPumpAsyncCaptures(std::vector<LLUUID>& wake);
private:
    LLCleanIngest() = default;
    LLCleanIngest(const LLCleanIngest&) = delete;
    LLCleanIngest& operator=(const LLCleanIngest&) = delete;

    // Clean-pipeline-owned registration sweep. Each main frame we force a slice
    // of the world's volume objects to re-register (independent of LL's
    // GEOM_DIRTY), so registration timing is OURS and the pipeline self-heals --
    // anything dropped/corrupted re-registers within one full cycle instead of
    // depending on LL deciding to re-dirty the group.
    U32  mSweepCursor = 0;
    bool mSweepIdle = false;          // last full cycle changed nothing (VK adaptive pace)
    // SETTLED-OBJECT SKIP (VK). Per object: a cheap signature of everything the per-face
    // ingest reads (volume + faces' vert/index counts, LOD, local transform, TE data,
    // bound textures + their alpha class, skin presence). A sweep visit whose signature
    // matches the last FULL visit skips the bounds regen + per-face registerCleanFace;
    // every kSigFullLaps laps an object gets a full visit anyway (self-healing net).
    // Changed/moved-queue processing drops the entry so the next visit is full.
    // Diagnostics (VKIngestDiag): frame of the last full visit / settled skip, number of
    // skips, and whether the last full visit left faces without an instance.
    // C1 PRE-SIGNATURE (RenderVKSweepPreSig): `pre` is a cheap hash of what the HEAVY
    // pre-steps (updateDistance/calcLOD, mesh loadMesh + setVolume, face-list resync)
    // read -- volume identity/detail/load state, LL LOD, TE/face counts, the GPU LOD band
    // and a 1/8-octave LOD metric -- recorded after a heavy visit. A matching visit skips
    // the heavy steps (the full signature + registration below still run). Reset to 0
    // by noteObjectChanged (setTE / setVolume / markRebuild hooks) and every
    // kPreFullLaps laps per object (pre_lap), so a missed update self-heals.
    // band = last GPU LOD band seen (idle detection: "no LOD band changed this lap").
    struct ObjSig { U64 sig = 0; U32 lap = 0; U32 full_frame = 0; U32 skip_frame = 0;
                    U32 skips = 0; U16 missing = 0; U16 resyncs = 0;
                    U32 fulls = 0; U8 why = 0;     // full visits since the last churn log; last reason
                    U64 pre = 0; U32 pre_lap = 0; U8 band = 0xFF; };
    std::unordered_map<LLUUID, ObjSig> mObjSig;
    std::vector<LLUUID> mChurnIds;   // objects with fulls > 0 (churn log walks only these)
    U32 mSweepLap = 0;
    bool mSweepLoading = true;   // last cycle added many instances: full load budget
    static constexpr U32 kSigFullLaps = 8;
    static constexpr U32 kPreFullLaps = 4;   // C1: heavy re-verification every N laps per object
    U32  mSweepCycleBandChanges = 0;         // C1: GPU LOD band changes seen this lap
    U32  mSweepCycleSigChanges = 0;          // sweep visits that found a changed signature this lap
    U32  mSweepCycleUpserts = 0;
    U32  mSweepCycleNew = 0;
    LLVector3 mSweepCycleCam;
    void selfRegisterSweep();
    // CLUSTER-OWNED MIRROR RELEASE: objects whose LOD group names a slot whose freed mirror is
    // wanted again (LLCleanMeshAsset::takeRematWake) are re-registered (noteObjectChanged), so
    // registerCleanFace re-decodes it. A sliced walk of the instance rows (kRematRowsPerFrame),
    // plus a refresh of every still-wanted slot's groups every kRematRefreshSec.
    void vkRematWakeTick();
    std::vector<U32> mRematGroups;   // sorted
    U32 mRematCursor = 0, mRematRowsLeft = 0;
    F64 mRematRefreshAt = 0.0;
    static constexpr U32 kRematRowsPerFrame = 32768;
    static constexpr F64 kRematRefreshSec = 5.0;
    // Per-object body of the ingest sweep, shared with the self/HUD priority pass.
    struct SweepStats { U32 objs = 0, faces = 0, mesh_seen = 0, mesh_novol = 0,
                        prim_novol = 0, mesh_reissue = 0, mesh_req = 0, skipped = 0,
                        heavy_skipped = 0, band_changes = 0, setvol_skipped = 0,
                        sig_changes = 0; };   // visits whose signature changed / first visit
    void sweepIngestObject(LLViewerObject* obj, SweepStats& st);
    // VKIngestDiag selection tracking (set by LLSelectMgr::mUpdateSignal).
    bool mDiagSelDirty = false;
    bool mDiagSelHooked = false;
    U64  mDiagSelSig = 0;
    // VK: self-avatar attachments (incl. HUD points + attached animesh) are ingested
    // ahead of the world sweep each frame, under a small budget with their own cursor,
    // and their textures are boosted so LL fetches them first.
    U32  mPriorityCursor = 0;
    bool   mPrioSettled = false;      // outfit fully ingested: re-walk every 30 frames
    bool   mPrioLapClean = true;      // nothing pending seen in the current lap
    U32    mPrioLapCount = 0;         // objects visited in the current lap
    U32    mPrioSettledFrame = 0;
    size_t mPrioLastObjCount = 0;
    U32    mPrioGen = 0;              // LLViewerJointAttachment::sOccupancyGen at the last lap
    bool   mPrioVerifyLap = false;    // current lap re-verifies a settled outfit (low budget)
    // RenderVKPrioPendingSet: objects the last lap left pending, revisited on their own
    // (next = frame of the next visit, streak = consecutive pending visits -> backoff).
    struct PrioPend { LLUUID id; U32 next = 0; U16 streak = 0; };
    std::vector<PrioPend> mPrioPending;
    std::vector<LLUUID>   mPrioLapPending;   // pending objects seen in the current lap
    void vkPriorityIngest();
    // PUSH-ON-MOVE. LL reports every transform change through LLPipeline::markMoved;
    // those objects (and their linkset children) are queued here and re-registered
    // at the head of the next selfRegisterSweep, so a moved STATIC instance picks up
    // its new model_matrix next frame instead of waiting for the rolling sweep to lap.
    std::vector<LLUUID>          mMovedQueue;
    std::unordered_set<LLUUID>   mMovedSet;
    void drainMovedQueue();
    // #567 DYNAMIC SHADOW CASTERS (RenderVKShadowMoverSettleSecs > 0): objects whose pose
    // changed in the moved queue, and pure GPU target-omega spinners (noteSpinner), carry
    // LLCleanInstanceStore::DYNAMIC_CASTER_FLAG until they have been still for the settle
    // time. Cached projector tiles keep them out of their static layer and redraw them in
    // the per-frame overlay (gVKDynSpheres says which tiles hold one), so a moving object
    // costs an overlay draw instead of a full static re-render of every tile around it.
    // Static tiles are invalidated only on the class TRANSITIONS (start / settle).
    struct DynCaster { F64 last = 0.0; U32 frame = 0; bool spinner = false; bool texanim = false; };   // texanim: <GMDX>   // frame: gFrameCount of the last move (#771)
    std::unordered_map<LLUUID, DynCaster> mDynCasters;
    void noteDynCaster(LLViewerObject* obj, bool spinner);
    void tickDynCasters();
public:
    // updateDrawable: a pure GPU spinner (vkUsesGpuSpin) still on LL's move list, per frame.
    void noteSpinner(LLViewerObject* obj);
    // glTF rigid node animation re-posed this object this frame: true if it went to the
    // dynamic class (else the caller invalidates the static tiles around it as before).
    bool noteAnimated(LLViewerObject* obj);
    // <GMDX> registerCleanFace: a mask / blend face with GPU texture animation. Its shadow
    // changes every frame without a re-registration, so tickDynCasters keeps the object in
    // the dynamic class while the animation runs (a cached static layer would freeze it).
    void noteTexAnimCaster(LLViewerObject* obj);
private:
    std::unordered_set<LLUUID>   mTexAnimCasters;   // <GMDX>
    struct MovedDiag   // RenderVKVehicleDiag: what this frame's drain did
    {
        size_t at_drain = 0, drained = 0;
        U32 seat_prims = 0, seat_changed = 0;
        U32 root = 0, child = 0, attach = 0, moving = 0, changed = 0;
        bool seat_drained = false;
    } mDiag;
    // PUSH-ON-CHANGE. LL's markTextured / markRebuild (TE texture, colour, offsets,
    // glow, fullbright, material; shape/volume) queue the object for a full
    // re-registration next frame -- content changes otherwise waited for the ingest
    // sweep to revisit (seconds when settled: "HUD buttons take a while"). HUD
    // objects go to the front. Capped per frame; the content-signature dedup makes
    // unchanged faces cheap.
    std::deque<LLUUID>         mChangedQueue;
    std::unordered_set<LLUUID> mChangedSet;
    void drainChangedQueue();
public:
    void noteObjectMoved(LLViewerObject* obj, bool easing = false, bool via_parent = false);
    void noteObjectChanged(LLViewerObject* obj);
    // #919: the HUD layout changed (HUD rect aspect -> the mScreen joint's scale ->
    // every HUD attach point's HUD-space matrix). HUD instance poses bake that matrix
    // (vkComposeAttachmentMatrix), and an attachment's moved notice is dropped while
    // its local pose holds -- re-pose every HUD attachment of ours once.
    void noteHudLayoutChanged();
    // Terrain (#235): a surface patch whose heights / normals changed (LL's dirtyGeom:
    // sim layer data, a terrain edit -- the patch and its neighbours). Re-ingested next
    // frame instead of when the sweep cursor gets round to it.
    void noteTerrainDirty(LLViewerObject* patch);
    std::vector<LLUUID> mTerrainDirty;
    std::unordered_set<LLUUID> mTerrainDirtySet;
    void drainTerrainDirty();
};
