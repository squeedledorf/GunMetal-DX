/**
 * @file llcleaninstancestore.h
 * @brief GPU-driven instance store. One persistent SSBO entry per
 *        (object, face_idx). Updated only when the instance's
 *        transform/material/mesh changes -- NOT per-frame iteration
 *        over visible faces.
 *
 *        Compute cull reads this SSBO, frustum-tests each instance,
 *        and writes per-material indirect-draw-command buffers.
 *        Multi-draw indirect dispatches all visible instances of a
 *        material in ONE GL call.
 *
 *        Per-frame CPU work = push deltas for moved/changed objects.
 *        Per-frame GL calls = (compute dispatches + material buckets),
 *        not per-face.
 */

#pragma once

#include "stdtypes.h"
#include "lluuid.h"

#include <array>
#include <atomic>   // <GMDX> rez effects
#include <map>
#include <unordered_map>
#include <mutex>
#include <unordered_set>
#include <utility>
#include <vector>

class LLViewerObject;   // intersectAvatars
class LLVector4a;
class LLVector2;

class LLCleanInstanceStore
{
public:
    static LLCleanInstanceStore& instance();

    bool init(U32 initial_capacity = 65536);
    void shutdown();

    // GPU-side layout. std430. Mat4s instead of mat3x4 to keep
    // CPU composition trivial and avoid transpose dances. Extra
    // bytes don't matter at 64k cap (~12 MB total).
    struct Instance
    {
        F32 model_matrix[16];   //  64 B (mat4: full affine, bottom row [0,0,0,1])
        F32 aabb_min[4];        //  16 B (vec3 + pad)
        F32 aabb_max[4];        //  16 B
        F32 tint[4];            //  16 B (rgba)
        F32 uv_matrix[16];      //  64 B (mat4: 2D UV xform, last row ignored;
                                //        vegetation (flags bit15) keeps its LOD inputs
                                //        in that row -- llcleaningest.cpp)
        U32 lod_group_id;       //   4 B  -> index into mesh LOD-group SSBO (upload-once /
                                //          GPU LOD select). The cull resolves the draw
                                //          slot from group.slot[level]; the instance is
                                //          LOD-agnostic and registered ONCE. Was
                                //          mesh_slot_id (a fixed-LOD slot) pre-redesign.
        U32 material_id;        //   4 B  -> index into material SSBO
        U32 flags;              //   4 B  bit0 rigged, bit1 alpha, bit2 mask, bit4 fullbright,
                                //        bit15 vegetation (Linden trees / grass),
                                //        bit16 water exclusion (invisiprim, #27),
                                //        bit19 flexi chain (with bit0; bounded from model_matrix)
        U32 skin_id;            //   4 B  -> index into skin SSBO (only meaningful when rigged);
                                //          worn rigid attachment (bit9): joint-table slot;
                                //          terrain (bit5): TerrainRecord index (binding 22)
        // GPU TARGET-OMEGA SPIN (llTargetOmega). model_matrix holds the UN-SPUN pose;
        // the vertex shader rotates it about spin_pivot by |axis|*(time - t0) around
        // the world axis. Zero axis == not spinning.
        F32 spin_axis[4] = {};  //  16 B  xyz = world axis * rate (rad/s), w = t0 (gFrameTimeSeconds clock)
        F32 spin_pivot[4] = {}; //  16 B  xyz = agent-space pivot, w = texel demand input: the slot's
                                //        UV density (LLCleanMeshAsset::computeUVDensity) or packUVExtent
    };                          // 224 B total, 16-aligned

    // Owned world pose for a non-attachment, non-HUD volume prim, from OBJECT
    // state. Under Vulkan the target-omega spin (own, or the linkset root's) is
    // factored OUT of the rotation and returned as a spin record for the GPU;
    // otherwise spin_axis/pivot are zeroed and the rotation is the live one.
    static void composeOwnedPose(LLViewerObject* obj, F32 model_out[16],
                                 F32 spin_axis_out[4], F32 spin_pivot_out[4]);
    // The face's raw texcoord range (LLVolumeFace::mTexCoordExtents) for the cull's
    // texel demand (clean_cull.comp uvExtent) when its slot has no UV density, as
    // spin_pivot[3]. Pose writers keep that lane.
    static F32 packUVExtent(F32 du, F32 dv);

    // Create or update an instance keyed by (object_uuid, face_idx).
    // Returns the instance_id (slot in the SSBO).
    // is_active = drawable is a mover (active or ANIMATED_CHILD);
    // movers participate in per-frame refresh, statics don't.
    // local_xform (optional, 16 floats column-major): a NODE-LOCAL pre-transform this
    // instance keeps across every later pose refresh -- model_matrix = owner pose *
    // local_xform. glTF scene assets (#39) place each node's primitives with it; the
    // caller passes data.model_matrix already composed. nullptr clears any previous one.
    U32 upsert(const LLUUID& object_uuid, S32 face_idx, const Instance& data,
               bool is_active, U64 sig = 0, const F32* local_xform = nullptr);
    // Replace the node-local pre-transform of an existing (object, face) instance (glTF
    // node animation). Returns true if it changed -- the caller then recomposes the pose
    // with refreshObjectPose(obj). No-op (false) for an unknown instance.
    bool setLocalXform(const LLUUID& object_uuid, S32 face_idx, const F32 local_xform[16]);
    // Column-major 4x4 product out = a * b (out may alias a). Shared by the pose
    // writers and the glTF ingest so every writer composes node-local transforms alike.
    static void mulMat4(const F32 a[16], const F32 b[16], F32 out[16]);

    // True if (object, face) is already registered with this exact content
    // signature -- the caller (registerFace) uses it to skip re-resolving an
    // unchanged STATIC instance (material/skin/upsert). sig==0 never matches
    // (reserved "no signature"). Cheap: one map lookup under the lock.
    bool isUpToDate(const LLUUID& object_uuid, S32 face_idx, U64 sig) const;
    // Monotonic count of real instance changes (new, or content changed); the
    // ingest sweep paces itself on it.
    U32 upsertCount() const { return mUpsertCount; }
    U32 newInstanceCount() const { return mNewCount; }
    // Live instances carrying flag bit16 (water exclusion / invisiprim): the VK
    // renderer skips the exclusion-mask draw while there are none.
    U32 waterExclusionCount() const { return mWaterExclCount; }
    static constexpr U32 WATER_EXCLUSION_FLAG = 65536u;
    // Instance id of (object, face), if ingested.
    bool findInstanceId(const LLUUID& object_uuid, S32 face_idx, U32& id_out) const;
    // (object, face) of a live instance id (GPU pick results, #255); false if released.
    bool keyForId(U32 id, LLUUID& object_uuid, S32& face_idx) const;


    // POSE-ONLY refresh of an object's registered instances after it moved (edit
    // drag, scripted mover, linkset child): recompose model_matrix (+ spin record)
    // from object state and dirty the changed ones. No geometry/material work --
    // registration (capture, mesh decode) stays with the ingest sweep.
    bool refreshObjectPose(LLViewerObject* obj);   // true if any instance pose changed
    // #567 DYNAMIC SHADOW CASTER (instance bit21): set on an object's (non-rigged, non-HUD)
    // instances while the ingest's mover tracker sees it move on the CPU. Cached shadow
    // views (projector tiles) keep such casters out of their static layer and draw them
    // in the per-frame overlay (clean_cull.comp SHADOW_SPLIT_*). Kept across upsert.
    // True if any row changed (dirtied for upload).
    static constexpr U32 DYNAMIC_CASTER_FLAG = 2097152u;
    bool setObjectDynamicCaster(const LLUUID& object_uuid, bool on);
    // WORN FLEXI CHAIN (bit19, no bit9): the cull and the pick bound a flexi from its
    // model_matrix translation, but a worn one is registered attach-point-relative and no
    // pose refresher touches a bit0 row. LLVolumeImplFlexible::vkUpdateAll hands the
    // chain frame's live agent-space origin here each simulated frame. True if a row moved.
    bool setFlexiOrigin(const LLUUID& object_uuid, const F32 origin[3]);
    // <GMDX> REZ EFFECTS (gmdxrezfx.cpp). Flag bits 22..31 belong to the effect manager: bit 22
    // = dying (a killed object drawn only for its de-rez), bits 23..31 = its effect table slot
    // + 1. Kept across re-registration (a dying row re-registered by a live object of the same
    // id drops them), and a new face of an object takes them from its other faces.
    static constexpr U32 REZFX_FLAGS = 0xFFC00000u;
    static constexpr U32 REZFX_DYING_FLAG = 0x00400000u;
    // Clear `clear` then set `set` on every row of the object (dirtied for upload). dying: a
    // killed object's rows -- the opaque ones move to the mask buckets (the depth pre-pass then
    // runs the de-rez cut) and its shadow leaves cached shadow tiles now, as a release would.
    // False if the object has no rows.
    bool setObjectRezFx(const LLUUID& object_uuid, U32 set, U32 clear, bool dying = false);
    // Release the object's rows that still carry the dying bit (the end of its de-rez).
    void releaseDyingRows(const LLUUID& object_uuid);
    // While on, every object whose FIRST row registers is queued for takeRezFxNew.
    void setRezFxCollect(bool on);
    bool rezFxNewPending() const { return mRezFxNewAny.load(std::memory_order_relaxed); }
    void takeRezFxNew(std::vector<LLUUID>& out);
    // </GMDX>
    // #567 SHADOW-CACHE INVALIDATION from registration: while collecting (the ingest turns
    // it on while cached shadow tiles exist), every NEW, RELEASED or shadow-relevantly
    // re-registered world instance (geometry / LOD group / routing flags / mask material /
    // static pose -- not tint or UVs) leaves an agent-space bounding sphere (x, y, z, r;
    // the old row's AND the new row's). Bounded: past the cap only `overflow` is set.
    void setShadowDirtyCollect(bool on);
    void takeShadowDirty(std::vector<F32>& out, bool& overflow);
    // #825 SHADOW REGISTRATION DIAGNOSTIC (RenderVKShadowRegDiag; the ingest turns it on with
    // the collection and logs a take every few seconds, VKShadowReg): WHY each emitting
    // registration emitted. A re-registration counts every field that differed (raw, before
    // the relevance rules): pose / spin (static rows), LOD group, routing flags (and which
    // bits), local bounds (the largest face move), mask / blend material. New keys and
    // releases count once each. Only rows that pass the static-caster filter
    // (noteShadowDirtyLocked) count; `top` = the most frequent emitting objects.
    enum : U32 { SRD_NEW = 0, SRD_RELEASE, SRD_POSE, SRD_SPIN, SRD_GROUP, SRD_FLAGS, SRD_AABB, SRD_MATERIAL,
                 SRD_COUNT };
    struct ShadowRegDiag
    {
        U32 events = 0;                   // emitting registration events
        U32 why[SRD_COUNT] = {};          // events by (raw) reason
        U32 flag_bits[32] = {};           // SRD_FLAGS: how often each bit differed
        F32 aabb_max_delta = 0.f;         // SRD_AABB: largest face move seen (m, volume-local)
        U32 aabb_same_group = 0;          // SRD_AABB with the LOD group unchanged (a representative-level swap)
        // What the #825 rules dropped: re-registrations of static-layer rows that differed only
        // in shadow-irrelevant ways (by raw reason, flag bits) and LOD-representative bounds
        // swaps absorbed by stabilizeBounds -- all of them emitted a sphere before.
        U32 dropped_events = 0;
        U32 dropped[SRD_COUNT] = {};
        U32 dropped_flag_bits[32] = {};
        U32 aabb_absorbed = 0;
        struct Obj
        {
            LLUUID id;
            U32 n = 0;                    // emitting events
            U32 why = 0;                  // OR of (1 << SRD_*)
            U32 row_flags = 0;            // the last row's flags
            U32 flag_xor = 0;             // OR of the differing flag bits
            F32 aabb_delta = 0.f;         // largest face move
        };
        std::vector<Obj> top;             // by n, at most kRegDiagTop
    };
    static constexpr size_t kRegDiagTop = 8;
    void setShadowRegDiag(bool on);
    void takeShadowRegDiag(ShadowRegDiag& out);   // moves the window out, resets it
    // #825: instance flag bits no shadow pass reads (shadowRelevantWhy): bit4 fullbright
    // (opaque routing), bit12 unlit shading, bit13 self / HUD texture priority, bit20 live media.
    static constexpr U32 SHADOW_IGNORED_FLAGS = 16u | 4096u | 8192u | 1048576u;
private:
    static bool shadowCasterRow(const Instance& row);   // a static-layer caster (noteShadowDirtyLocked's filter)
    static U32 shadowRelevantWhy(const Instance& a, const Instance& b, U32 raw, U32 flag_xor);
    static bool stabilizeBounds(const Instance& old, Instance& data);
    // The (raw) reasons a re-registration differs for the shadow caches (1 << SRD_*); fills
    // the differing flag bits and the largest bounds-face move for the diagnostic.
    static U32 shadowChangeWhy(const Instance& a, const Instance& b, bool is_active, U32& flag_xor,
                               F32& aabb_delta);
    // True when the row is a static-layer caster (emitted or coalesced into the last sphere).
    bool noteShadowDirtyLocked(const Instance& row);   // mMutex held
    void regDiagLocked(const LLUUID& id, U32 why, U32 row_flags, U32 flag_xor, F32 aabb_delta);   // mMutex held
    std::vector<F32> mShadowDirty;
    bool mShadowDirtyOn = false, mShadowDirtyOverflow = false;
    bool mRegDiagOn = false;
    ShadowRegDiag mRegDiag;
    std::unordered_map<LLUUID, ShadowRegDiag::Obj> mRegDiagObjs;   // bounded (kRegDiagObjCap)
    static constexpr size_t kRegDiagObjCap = 4096;
private:
    void markDirtyLocked(U32 id);   // mMutex held
public:

    // DIAG: copy out the instance registered for (object, face), if
    // any. Read-only; for the texture-chain debug dump. out_id gets
    // the SSBO slot index for GPU readback comparison.
    bool debugGet(const LLUUID& object_uuid, S32 face_idx, Instance& out,
                  U32* out_id = nullptr) const;


    // Drop an instance (object destroyed or face vanished).
    void release(const LLUUID& object_uuid, S32 face_idx);

    // Push hooks. Direct (uuid, face_idx) lookup; cheap when the
    // entry exists, no-op when it doesn't. Used instead of polling
    // every live face every frame.
    void pushTint(const LLUUID& object_uuid, S32 face_idx,
                  F32 r, F32 g, F32 b, F32 a);
    // Release ALL face entries for an object (fired from markDead).
    // mKeyToId is std::map keyed by (uuid, face_idx); same-uuid
    // entries are contiguous, so this is O(log N + faces).
    void releaseObject(const LLUUID& object_uuid);
    // #737 LIVENESS AUDIT (diagnostic, never frees): rows are released by markDead
    // (releaseObject), so a row whose object is no longer in gObjectList (or is dead) is
    // a leaked holder -- it pins its material, LOD group (geometry) and skin. Main thread:
    // auditOrphansSlice checks up to max_keys rows per call from a rolling cursor and
    // records each object first seen missing; orphanStats counts those missing for at
    // least min_age_frames (objects and rows) and drops the ones released since.
    void auditOrphansSlice(U32 max_keys, U32 now_frame);
    struct OrphanStats { U32 objects = 0; U32 rows = 0; U32 suspects = 0; };
    OrphanStats orphanStats(U32 now_frame, U32 min_age_frames);
    // Region crossing, LEGACY path (RenderVKStableWorldFrame off): LL re-bases AGENT space
    // by `off` (new = old + off). Translate every agent-space instance (not HUD/worn-
    // attachment -- those are local -- and not rigged, whose pose comes from the per-frame
    // skin palette) and mark the whole store dirty so the GPU copy and pick boxes follow.
    // Returns instances shifted. Runs only with the render frame at d = 0 (the caller
    // re-anchors first), so store == agent space on this path.
    U32 shiftOrigin(const F32 off[3]);

    // ---- RENDER-WORLD FRAME (#568, RenderVKStableWorldFrame) ----------------------------
    // The store (mInstances, the renderer's CPU mirror, the pick boxes / grid) keeps the
    // agent-space lanes of every world row in a STABLE frame R = A + d, where A is LL's
    // agent frame (origin = the agent region's SW corner; it moves on every region
    // crossing) and d = agent origin - render anchor, both region-grid points with z = 0.
    // A crossing then changes only d (d -= off): no row is rewritten, dirtied, re-copied,
    // re-boxed or re-uploaded on the CPU. The GPU keeps AGENT space (every shader, the
    // per-frame camera, lights, skins, particles unchanged): the renderer's upload writes
    // R - d into staging (VKFrameSync::d), and one compute pass (clean_inst_shift.comp)
    // moves the GPU-resident rows by the crossing's offset (VKFrameSync::gpu_shift).
    //
    // Agent-space LANES (one predicate for every writer, the upload and the GPU pass --
    // exactly the legacy shiftOrigin rule): a live row (lod_group_id != sentinel) that is
    // not HUD (flag 8) or worn (flag 512) carries agent space in spin_pivot.xyz, and,
    // unless rigged (flag 1: model_matrix is LL's getRelativeXform fallback, left as the
    // legacy path left it), in model_matrix's translation (column 3).
    //
    // FLOAT ERROR BUDGET. d is an exact multiple of 256 m (region grid, z = 0), so A <-> R
    // only rounds the stored lane to the grid of |R|: half an ULP of max(|R|).
    // RE-ANCHOR (d := 0, the anchor moves to the agent region) when the agent region is
    // more than RenderVKReanchorMeters (default 2048 m = 8 regions) from the anchor along
    // x or y. Geometry near the camera then has |R| < 2048 + ~512 < 4096 m, so the stored
    // error is <= 2^-13 m = 0.12 mm (ULP 0.24 mm) -- half of what a Z = 4000 m skybox
    // already has in agent space today, and an order below the 1-2 mm offsets layered
    // builds use. Far geometry at the 4 km draw distance sits at |R| < ~6.5 km: <= 0.24 mm
    // (it is <= ~4.5 km in agent space today: same binade, unchanged). The GPU rows are
    // agent space as before (camera-relative reversed-Z #555, depth unaffected); pick boxes
    // round like the rows. An unbounded R (no re-anchor) would lose 1 mm per ~16 km
    // travelled: the re-anchor is what keeps long mainland trips at the numbers above.
    // A re-anchor moves the store + mirror + pick boxes by -d (O(N) adds, ~1 ms per 70k
    // rows, no dirtying: R - d is unchanged, so no GPU row, cache or upload is touched).
    static bool rowAgentSpace(U32 flags, U32 lod_group_id)
    { return lod_group_id != 0xFFFFFFFFu && (flags & (8u | 512u)) == 0u; }
    // Add t to an instance's agent-space lanes (rowAgentSpace rule).
    static void translateRow(Instance& in, const F32 t[3]);
    // Same on a raw 224 B row (the renderer's byte mirror).
    static void translateRowBytes(U8* row, const F32 t[3]);
    // The current d (store = agent + d). Locks.
    void renderOffset(F32 out[3]) const;
    // Stable-frame crossing: the agent frame moved by `off` (new agent = old agent + off).
    // d -= off; queue `off` for the GPU rows (VKFrameSync::gpu_shift). O(1).
    void noteAgentShift(const F32 off[3]);
    // Largest |d.x|, |d.y| (m): the caller's re-anchor test.
    F32 renderOffsetSpan() const;
    // Re-anchor: d := 0 (store rows, mirror, pick boxes += -d; see above). Returns live
    // world rows moved; delta_out (optional) gets the delta applied.
    U32 reanchor(F32 delta_out[3] = nullptr);
    // Per copyDirtyVK: the frame the copied rows are in, and the agent shift the GPU rows
    // still need (accumulated since the previous call, then cleared).
    struct VKFrameSync
    {
        F32 d[3] = {0.f, 0.f, 0.f};
        F32 gpu_shift[3] = {0.f, 0.f, 0.f};
        bool shifted = false;   // gpu_shift is non-zero
    };



    U32 capacity() const { return mCapacity; }
    U32 liveCount() const { return mLiveCount; }
    // VRAM soft ladder (#405 geometry rungs): give the id range back when the live ids
    // (packed low by the lowest-first free list) leave at least the top half free --
    // the store's arrays shrink here; the renderer's mirror and its id-indexed GPU tables
    // (instance SSBO, previous instances, skin history, cull scratch) follow. No id is
    // remapped: only free ids above the highest one in use go. Returns the new capacity
    // (0 = nothing trimmed).
    U32 trimTail();

    // VK-MODE MIRROR. The Vulkan world renderer (llcleanrenderervk.cpp) has no
    // GL context to read the SSBO, so it copies the CPU shadow array directly.
    // The Instance struct is byte-identical (224 B) to the VK renderer's, and
    // released/empty slots carry the sentinel lod_group_id (0xFFFFFFFF) which
    // the cull skips -- so exporting the full [0, cpuInstanceCount) span (holes
    // included) is safe. Read-only, no lock (the VK sync runs on the render
    // thread, same as flush()). Returns the raw byte view + element count.
    const U8* cpuInstanceBytes() const
    {
        return mInstances.empty() ? nullptr
             : reinterpret_cast<const U8*>(mInstances.data());
    }
    U32 cpuInstanceCount() const { return (U32)mInstances.size(); }
    // Per row: bumped every time the row is released, so a cached (object, face) of an id is
    // still the row's key while its generation is unchanged (a release + reuse of the same id
    // between two looks is otherwise invisible). cpuInstanceCount() entries; no lock (as above).
    const U32* rowGenerations() const { return mRowGen.empty() ? nullptr : mRowGen.data(); }

    // VK INCREMENTAL MIRROR. Copy only the instances dirtied since the last call into
    // `mirror` (a byte array of Instance) and append their id runs [first, last) to `runs`. A
    // size change (grow) copies everything as one run. A rolling 1/64 slice is always
    // re-copied as well (self-heal, as flush()'s rolling refresh). Consumes the dirty
    // flags -- only valid while the GL flush() path is not running (VK mode).
    // heal_mode (C8, RenderVKSelfHealRate, latched by the caller on the main thread): the
    // rolling self-heal slice re-copies 0 none, 1 n/256 (min 16), 2 n/64 (min 64) rows/frame.
    // live_out: the live count under the same lock, i.e. an upper bound on the mirror's
    // non-sentinel rows (the cull's output lists are sized from it).
    // sync_out (#568): the render-world frame of the mirror rows (upload: R - d) and the
    // pending GPU agent shift (consumed). Its gpu_shift ACCUMULATES into *sync_out.
    void copyDirtyVK(std::vector<U8>& mirror, std::vector<std::pair<U32, U32>>& runs, U32 heal_mode = 2,
                     U32* live_out = nullptr, VKFrameSync* sync_out = nullptr);

    // ROW CHANGE LOG (cluster LOD gather, llcleanclusterselectvk.cpp). While on, copyDirtyVK
    // also appends every dirtied id run it consumes (NOT the self-heal slice) here, so a
    // consumer can visit only the rows that changed since its last take instead of every row
    // every frame. It sees exactly the rows the renderer's mirror took (copyDirtyVK runs in
    // syncFromStores, before the frame records; nothing writes the store in between).
    // Bounded: past kChangeLogCap runs (a consumer that stopped taking) the log drops to
    // `full` (= "rescan every row"); turning it on, a mirror size change and shutdown set
    // `full` too. takeChangeLog moves the runs out (appended to `out`) and clears `full`.
    void setChangeLog(bool on);
    void takeChangeLog(std::vector<std::pair<U32, U32>>& out, bool& full);

    // PICKING is the GPU raster pick's (#702 / #704, llcleancursorpick.h): it maps its
    // fragments' instance ids back to (object, face) with keyForId.
    // The avatars' collision volumes on [start, local_end] (LL's pickable avatar shape),
    // with LL's precedence: an avatar hit replaces hit_obj unless hit_obj is an
    // attachment within 0.1 m of it. Shortens local_end and fills the out-params on a hit;
    // true when an avatar was taken. (The GPU pick's last step: avatars are not instances.)
    static bool intersectAvatars(const LLVector4a& start, LLVector4a& local_end, LLViewerObject*& hit_obj,
                                 bool pick_transparent, bool pick_rigged, bool pick_unselectable,
                                 S32* face_hit, LLVector4a* intersection, LLVector2* tex_coord,
                                 LLVector4a* normal, LLVector4a* tangent);

    // Insert the mesh_slot_id of every live instance into `out`. Used by the
    // mesh-asset garbageCollect to learn which mega-VBO slots are still
    // referenced. Conservative: empty slots (mesh_slot_id == sentinel) are
    // skipped; a released-but-not-cleared slot at worst keeps its mesh alive a
    // little longer (never frees a live one).
    void collectInUseMeshSlots(std::unordered_set<U32>& out) const;
    // VRAM soft ladder (#405 mesh detail): as collectInUseMeshSlots, but a static world
    // instance keeps only the levels up to one above what the cull selects for it at
    // lod_factor from cam_origin (the clean_cull.comp formula, LLCleanMeshAsset::drawnSlot)
    // -- the higher levels the lower EFFECTIVE LOD factor no longer selects become
    // reclaimable. Rigged, HUD, worn, terrain and vegetation instances (their own LOD
    // rules) keep every level.
    void collectInUseMeshSlotsLod(std::unordered_set<U32>& out, const F32 cam_origin[3], F32 lod_factor) const;


    // DIAGNOSTIC: dump the clean instance for a specific (object, face) -- the
    // model_matrix decomposed to world position + per-axis scale, the AABB, the
    // mesh_slot_id (sentinel?), and flags. Lets us inspect the exact object the
    // user selects (vs the scanner's worst-by-scale outlier). Logs to CleanSel.
    void debugDumpInstanceFor(const LLUUID& obj, S32 face) const;

    // DIAGNOSTIC (#29 alert forensics): reverse-resolve an instance id (as the
    // cull's offender ring reports them) to its owning (object uuid, face) plus
    // the instance's lod_group_id and flags. Linear scan of mKeyToId -- alert
    // path only, never per frame. Returns false if the id isn't live.






private:
    LLCleanInstanceStore() = default;
    LLCleanInstanceStore(const LLCleanInstanceStore&) = delete;
    LLCleanInstanceStore& operator=(const LLCleanInstanceStore&) = delete;

    bool grow(U32 new_capacity);
    void freePush(U32 id);   // mFreeList is a min-heap (lowest id first); mMutex held
    U32  freePop();

    // HOLDER REFERENCES (security audit: grow-only material / skin / terrain stores).
    // A live row holds one reference on its material, its skin entry (rigged, flag
    // bit0, not terrain) and its terrain record (flag bit5: skin_id is the record).
    // Every row writer (upsert / release / releaseObject / shutdown) moves them with
    // retargetRefs. Lock order: mMutex -> material / skin store mutexes (those stores
    // never call back into this one).
    struct RowRefs { U32 mat, skin, trec; };
    static RowRefs rowRefs(const Instance& in);
    static void retargetRefs(const RowRefs& old_refs, const RowRefs& new_refs);

    U32    mCapacity = 0;
    U32    mLiveCount = 0;

    std::vector<Instance> mInstances;          // CPU shadow of SSBO
    std::vector<U32>      mFreeList;           // free instance slots
    std::vector<U8>       mDirty;              // 0/1 per slot
    std::vector<U64>      mSig;                // content signature per slot (0 = none/stale)
    std::vector<U32>      mRowGen;             // release count per slot (rowGenerations)
    U32                   mDirtyMin = 0xFFFFFFFFu;
    U32                   mDirtyMax = 0;

    std::map<std::pair<LLUUID, S32>, U32> mKeyToId;
    // Hash indexes kept in lockstep with mKeyToId (keyAdd/keyErase/keyClear): every
    // point lookup and per-object range was an O(log n) UUID-compare tree walk, run
    // per face per sweep visit and per moved object per frame (a crowd cost).
    // mKeyToId stays for the ordered sweep cursor and whole-store walks.
    struct KeyHash
    {
        size_t operator()(const std::pair<LLUUID, S32>& k) const noexcept
        { return std::hash<LLUUID>()(k.first) ^ ((size_t)(U32)k.second * 0x9E3779B97F4A7C15ull); }
    };
    std::unordered_map<std::pair<LLUUID, S32>, U32, KeyHash> mKeyHash;
    std::unordered_map<LLUUID, std::vector<U32>>             mObjIds;   // object -> its instance ids
    // #737 orphan audit: rolling cursor + objects seen missing (uuid -> first frame).
    std::pair<LLUUID, S32>            mOrphanCursor;
    bool                              mOrphanCursorInit = false;
    std::unordered_map<LLUUID, U32>   mOrphanSuspect;
    void keyAdd(const std::pair<LLUUID, S32>& k, U32 id);
    void keyErase(std::pair<LLUUID, S32> k, U32 id);   // by value: callers pass map-node keys
    void keyClear();
    U32  keyFind(const LLUUID& obj, S32 face) const   // 0xFFFFFFFF if absent
    { auto it = mKeyHash.find(std::make_pair(obj, face)); return it == mKeyHash.end() ? 0xFFFFFFFFu : it->second; }

    // Node-local pre-transforms (see upsert's local_xform), by instance id. Sparse: only
    // glTF scene-asset instances carry one. Erased on release / releaseObject / an upsert
    // without one.
    std::unordered_map<U32, std::array<F32, 16>> mLocalXform;
    const F32* localXformLocked(U32 id) const   // mMutex held; nullptr if none
    { auto it = mLocalXform.find(id); return it == mLocalXform.end() ? nullptr : it->second.data(); }

    std::vector<std::pair<LLUUID, S32>> mIdToKey;   // id -> key (GPU pick results: keyForId)
    U32 mUpsertCount = 0;        // real instance content changes (sweep adaptive pacing)
    U32 mNewCount = 0;           // instances created (sweep adaptive pacing)
    U32 mWaterExclCount = 0;     // live instances with WATER_EXCLUSION_FLAG
    // #568 render-world frame (see rowAgentSpace): d, the GPU agent shift not yet handed to
    // the renderer, and a re-anchor delta its byte mirror still needs (copyDirtyVK).
    F32  mRenderD[3] = {0.f, 0.f, 0.f};
    F32  mGpuShiftPending[3] = {0.f, 0.f, 0.f};
    F32  mMirrorShiftPending[3] = {0.f, 0.f, 0.f};
    bool mMirrorShiftPendingSet = false;
    bool renderOffsetZero() const   // mMutex held
    { return mRenderD[0] == 0.f && mRenderD[1] == 0.f && mRenderD[2] == 0.f; }
    void toStoreFrame(Instance& in) const { if (!renderOffsetZero()) translateRow(in, mRenderD); }   // mMutex held
    void toAgentFrame(Instance& in) const   // mMutex held
    {
        if (renderOffsetZero()) return;
        const F32 n[3] = {-mRenderD[0], -mRenderD[1], -mRenderD[2]};
        translateRow(in, n);
    }

    // Active (mover) tracking. mActiveIds is the dense list the
    // per-frame refresh walks; mActiveKey is the parallel (uuid,
    // face_idx) lookup (so we don't need a reverse map from id to
    // key for the per-mover gObjectList lookup and face fetch).
    // mActiveIndex[id] gives the position within mActiveIds (or
    // INVALID_ACTIVE_INDEX). Swap-pop on remove keeps it compact.
    std::vector<U32> mActiveIds;
    std::vector<std::pair<LLUUID, S32>> mActiveKey;
    std::vector<U32> mActiveIndex;
    static constexpr U32 INVALID_ACTIVE_INDEX = 0xFFFFFFFFu;

    void addActive(U32 id, const LLUUID& uuid, S32 face_idx); // assumes lock held
    void removeActive(U32 id);                                // assumes lock held

    // Rolling all-instance transform self-heal. refreshAndReap walks the active
    // (mover) list every frame, but STATIC instances' model_matrix is composed
    // ONCE at registration (relativeXform * region/render matrix) and never
    // recomposed -- so if the region render matrix shifts (agent region-cross),
    // or a drawable transitions active<->static, or a static re-registers with a
    // momentarily-wrong transform, the static is drawn at the wrong place and gets
    // frustum-rejected forever. That is a prime #29 ("most geometry gone, never
    // recovers") suspect. This sweep recomposes a SLICE of all live instances each
    // frame from the object's CURRENT authoritative transform, on OUR schedule,
    // independent of LL's GEOM_DIRTY -- so any transform staleness self-heals
    // within one full cursor cycle. Pointer-safe: a transient findObject miss
    // SKIPS (never reaps) the instance.
    std::pair<LLUUID, S32> mSweepCursorKey;   // resume point into mKeyToId
    bool mSweepCursorInit = false;
    void rollingTransformSweep();             // assumes lock held

    mutable std::mutex mMutex;
    // Row change log (setChangeLog / takeChangeLog), mMutex held.
    static constexpr size_t kChangeLogCap = 65536;
    std::vector<std::pair<U32, U32>> mChangeLog;
    bool mChangeLogOn = false;
    bool mChangeLogFull = false;
    // <GMDX> rez effects: objects whose first row registered (setRezFxCollect), mMutex held.
    std::vector<LLUUID> mRezFxNew;
    bool mRezFxCollect = false;
    std::atomic<bool> mRezFxNewAny{false};
    // </GMDX>
};
