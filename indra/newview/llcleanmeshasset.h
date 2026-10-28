/**
 * @file llcleanmeshasset.h
 * @brief Clean-room mesh-asset store. Owns geometry data keyed by
 *        (mesh_id, LOD, face_idx). Captures ONCE at LL's mesh-decode
 *        completion event (LLMeshRepository::notifyMeshLoaded). Geometry
 *        no live instance references is reclaimed by garbageCollect under
 *        the RenderVKGeometryBudgetMB budget; LOD groups by gcLodGroups.
 *
 *        Storage:
 *          - One mega-VBO (POS + UV blocked sections) and one mega-IBO.
 *          - One Slot per face per LOD per asset. Indices stored
 *            face-local (0..numVerts-1); attribute pointers bind
 *            directly to the slot's vert range so glDrawElements
 *            with no baseVertex is correct.
 *          - Slot ids / LOD-group ids are recycled through free lists
 *            after a short frame quarantine (kIdQuarantineFrames).
 */

#pragma once

#include "stdtypes.h"
#include "lluuid.h"
#include "llpointer.h"
#include "llvolume.h"   // LLVolume complete type (LLPointer<LLVolume> map member)
#include "threadpool.h" // LL::ThreadPool (async mesh decode)
#include "llcleanqvertex.h" // quantized mirror / VK vertex (#573)

#include <atomic>
#include <deque>
#include <iosfwd>
#include <list>
#include <set>
#include <tuple>
#include <map>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>
class LLVector2;
class LLVector3;
class LLVector4a;

class LLCleanMeshAsset
{
public:
    struct Key
    {
        LLUUID mesh_id;
        S32    lod;
        S32    face_idx;

        bool operator<(const Key& r) const
        {
            if (mesh_id != r.mesh_id) return mesh_id < r.mesh_id;
            if (lod     != r.lod)     return lod     < r.lod;
            return face_idx < r.face_idx;
        }
    };

    struct Slot
    {
        U32  pos_offset_bytes  = 0;
        U32  uv_offset_bytes   = 0;
        U32  norm_offset_bytes = 0;
        U32  tan_offset_bytes  = 0;
        U32  wt_offset_bytes   = 0;   // weight4 attribute, 0 = no weights
        U32  ibase_bytes       = 0;
        U32  num_verts         = 0;
        U32  num_indices       = 0;
        U32  slot_id           = 0xFFFFFFFFu;  // index into mSlotInfo / SSBO
        bool valid             = false;
        bool has_weights       = false;
        bool has_tangents      = false;
        // Reclaim bookkeeping (garbageCollect). reclaimable: regular volume meshes
        // (capture()), glTF primitives, and captureTerrain / captureWater callers that
        // pass reclaimable=true (terrain patches, water, grass -- they die with their
        // region). Tree species and smooth-terrain levels stay false: their owners
        // cache the slot / group and free it with freeAsset(). last_ref_frame is the
        // last frame a live instance referenced this slot.
        bool reclaimable       = false;
        U32  last_ref_frame    = 0;
        // STABLE-RANGE capacity (terrain/water in-place updates). These patches
        // rebuild every frame; freeing + reallocating their ranges each rebuild
        // cycled the same storage through different owners ~150x/frame -- the
        // racing surface for the #29 stomp class. Instead the range is allocated
        // once with headroom and re-uploaded IN PLACE; only growth beyond the
        // capacity reallocates (rare).
        U32  vert_cap          = 0;
        U32  idx_cap           = 0;
        // Frame this slot's geometry was (re)captured/uploaded. The bounds
        // auditor skips freshly-captured slots (in-flight upload window).
        U32  captured_frame    = 0;
        // Sculpt decode level (LLVolume::getSculptLevel(), i.e. the sculpt
        // texture's discard) this slot's geometry was captured at. Sculpts refine
        // as their texture decodes: LL rebuilds the volume at a finer level, but
        // our (sculptID,lod,face) key is latched, so without re-capture the slot
        // keeps the coarse/incomplete first decode (e.g. a horse whose body never
        // formed). registerFace re-captures when the live level beats this. -1 =
        // not a sculpt / not tracked.
        S32  sculpt_level      = -1;
        // #29 corruption detector: bounds of the positions WE UPLOADED at capture
        // time. If a later read-back of the VBO range disagrees, this slot's data
        // was stomped by a stray write (finite-but-wrong "stretched" geometry that
        // the degenerate/garbage thresholds miss).
        F32  cap_lo[3]         = {0.f, 0.f, 0.f};
        F32  cap_hi[3]         = {0.f, 0.f, 0.f};
        // Texture-streaming UV density of the captured geometry (computeUVDensity),
        // copied into the instance's spin_pivot.w for the cull's texel demand. 0 = none.
        U32  uv_density        = 0;
    };

    // GPU-side per-slot info. Compute cull reads this (resolved via the LOD group)
    // to build the indirect-draw command.
    struct SlotInfo
    {
        U32 first_index;   // = ibase_bytes / 4
        U32 index_count;   // = num_indices   (0 = NOT RESIDENT -> cull emits nothing)
        S32 base_vertex;   // = pos_offset_bytes / 16
        U32 has_weights;   // 0 or 1
        U32 vert_count;    // = num_verts; used by cull to size skin-output range
        U32 _pad0;         // captured volume-local bounds as 6 halfs (outward-rounded):
        U32 _pad1;         //   _pad0 = lo.x|lo.y, _pad1 = lo.z|hi.x, _pad2 = hi.y|hi.z
        U32 _pad2;         //   all zero = none (packSlotBounds)
    };
    static void packSlotBounds(SlotInfo& si, const F32 lo[3], const F32 hi[3]);

    // TEXTURE-STREAMING UV DENSITY (clean_cull.comp uvDensityWorld): object-space metres
    // per raw UV unit over the triangles (area-weighted 75th percentile of the
    // per-triangle sqrt(world area / UV area)), the area-weighted squared geometric
    // normal (the cull turns the instance's axis scales into this face's area scale) and
    // the mean UV stretch sigma_max / sigma_min. Positions / texcoords are strided in
    // floats; indices U16 or U32. Returns the tagged spin_pivot.w bits, 0 = no usable
    // triangle. O(triangles), any thread.
    static U32  computeUVDensity(const F32* pos, U32 pos_stride, const F32* uv, U32 uv_stride,
                                 const void* indices, bool idx16, U32 ni, U32 nv);
    // Decodes those bits (false = not a density word): d in object metres per UV unit,
    // w = normal weights (sum 1), stretch >= 1.
    static bool decodeUVDensity(U32 bits, F32& d, F32 w[3], F32& stretch);

    // UPLOAD-ONCE / GPU-LOD-SELECT. Each object-face is a LOD GROUP: the slot_ids
    // of its up-to-NUM_LOD levels (SENTINEL = that LOD not resident yet). The
    // instance references a lod_group_id (NOT a fixed LOD slot), so an object is
    // registered ONCE and never re-registered on LOD change -- the cull compute
    // picks the level from camera distance and reads slots[group.slot[level]].
    // Residency falls out of the existing index_count==0 skip: an unresident LOD
    // is either SENTINEL in the group or points at a slot whose index_count is 0,
    // and the cull emits nothing for it (invisible until the level's data lands).
    static constexpr S32 NUM_LOD = 4;             // LL LOD levels 0(lowest)..3(highest)
    static constexpr U32 LOD_SENTINEL = 0xFFFFFFFFu;
    struct LodGroup
    {
        U32 slot[NUM_LOD];   // slot_id per LOD level, LOD_SENTINEL if not resident
    };

    static LLCleanMeshAsset& instance();

    bool init();
    // init() has run (capture() is a silent no-op before it).
    bool ready() const { return mReady; }
    void stopDecodes();   // join the async decode pool (app cleanup)
    void shutdown();

    // Capture every face of `volume` into our store, keyed by
    // (mesh_id, lod, face_idx). Idempotent: re-capture of an existing
    // (mesh_id, lod) is a no-op -- the first capture wins and the
    // geometry stays for the session.
    // force=true re-captures an already-captured asset ATOMICALLY: new geometry is
    // uploaded to fresh ranges and each face's SlotInfo is overwritten in place
    // (reusing its slot_id), then the OLD ranges are freed. The slot is never zeroed,
    // so a live instance never sees index_count==0 -- fixing the #29 re-capture strand
    // where invalidateAsset zeroed a slot that a skipped/degenerate re-capture never
    // re-filled (emitted_empty stuck permanently). A face the new volume can't supply
    // keeps its prior valid geometry.
    void capture(const LLUUID& mesh_id, S32 lod, const LLVolume* volume, bool force = false);

    // Free every slot for (mesh_id, lod) and drop the capture latch, so the next
    // capture() re-ingests fresh geometry. Used to re-capture a sculpt once its
    // texture decodes to a finer level than the slot was first captured at.
    void invalidateAsset(const LLUUID& mesh_id, S32 lod);






    // LOD-fetch chain telemetry (#29 lodmiss residual): requests we issued to the
    // mesh repo for GPU-desired levels / raw-byte payloads that actually arrived
    // and were stored / desired-level captures that succeeded. If req climbs but
    // got doesn't, the repo isn't delivering; if got climbs but cap doesn't, the
    // sweep-side decode/capture is the broken link.
    static std::atomic<U32> sLodFetchReq;
    static std::atomic<U32> sLodFetchGot;
    static std::atomic<U32> sLodFetchCap;

    // CONVERGE-VISIT TRACE (#29 strand forensics). The sweep's LOD-converge
    // block records what the CPU decided for (mesh_id, face) at its last visit;
    // debugGroupString appends it per offender in the strand alert, so a strand
    // names its own broken link: no trace = the sweep never reaches the object;
    // AGREE while the GPU reports lodmiss = the CPU/GPU formula inputs diverge;
    // FETCH_SENT/WAIT with raw=0 forever = the repo fetch chain never delivers.
    enum ConvergeAction : U8
    {
        CONV_AGREE       = 0,  // desired == LL's live lod: converge block idle
        CONV_SLOT_EXISTS = 1,  // desired level already captured -> published
        CONV_CAPTURED    = 2,  // decoded + captured + published this visit
        CONV_FETCH_SENT  = 3,  // no raw bytes -> gMeshRepo.loadMesh issued
        CONV_FETCH_WAIT  = 4,  // no raw bytes, request throttled (pending)
        CONV_NO_SOURCE   = 5,  // decode failed / nothing fetchable
        CONV_FALLBACK    = 6,  // structural gap -> published best resident level
        CONV_DECODE_WAIT = 7,  // raw bytes present, async decode in flight
    };
    struct ConvergeTrace
    {
        U32 frame     = 0;   // gFrameCount of the last sweep visit
        S16 desired   = -1;  // CPU-computed GPU-select level at that visit
        S16 mesh_lod  = -1;  // LL live-volume level at that visit
        S16 fetch_lod = -1;  // after getActualMeshLOD structural mapping
        U8  have_raw  = 0;   // raw bytes present for fetch_lod at that visit
        U8  action    = CONV_AGREE;
    };
    void noteConverge(const LLUUID& mesh_id, S32 face_idx, const ConvergeTrace& t);

    // Canonical mesh_id for a PURE PRIM (no sculpt/mesh asset): FNV hash of the
    // shape parameters. THE single source of truth -- ingest (registerCleanFace)
    // and every consumer (selection overlay etc.) must use this, never the
    // LLVolume pointer (pointer reuse caused the #29 stale-slot collisions, and
    // a pointer-keyed lookup can never match hash-keyed ingest).
    static LLUUID primShapeKey(const LLVolumeParams& params);
    // THE geometry key for a volume (#44): mesh / glTF -> the asset id; image sculpt ->
    // the sculpt map id salted with the sculpt type byte (sphere / torus / plane /
    // cylinder + invert / mirror all change the geometry, so one map under two types
    // must not share slots); prim -> primShapeKey. Every consumer uses this.
    static LLUUID geometryKey(const LLVolumeParams& params);

    // Group-slot queries for the converge/fetch driver (#29 lodmiss strand).
    // groupSlotAt: the PUBLISHED slot_id at `lod` for (mesh_id, face), or
    // LOD_SENTINEL. groupBestSlot: the highest published level whose slot has
    // index_count > 0 -- the structural-gap fallback source (a level the
    // asset/decode cannot provide for this face publishes best-available;
    // LL parity: LL renders the loaded LOD when the desired one doesn't exist).
    U32 groupSlotAt(const LLUUID& mesh_id, S32 face_idx, S32 lod) const;
    U32 groupBestSlot(const LLUUID& mesh_id, S32 face_idx) const;
    // Bumped (after the write) whenever any groupBestSlot answer may change: a LOD group's
    // slots or the face -> group map (markGroupDirtyLocked) and every SlotInfo write
    // (index_count). Unchanged = every cached groupBestSlot answer still holds (the cluster
    // LOD gather caches its per-mesh build source on it). Lock-free.
    U32 bestSlotGeneration() const { return mBestSlotGen.load(std::memory_order_acquire); }

    // EDIT FLOATER LOD INFO (#272). The slot the cull draws for a static world instance:
    // clean_cull.comp's level (detailFromTan of lod_factor * radius / distance from the
    // camera) when resident, else the highest resident level. LOD_SENTINEL = nothing drawn.
    // (Not for picking: the GPU pick reads the cull's own choice, #704.)
    U32 drawnSlot(U32 lod_group_id, const F32 model_matrix[16], F32 lod_radius,
                  const LLVector3& cam_origin, F32 lod_factor, S32* out_level = nullptr) const;
    // The group's per-level resident geometry (0 = that level isn't resident). false = no
    // such group.
    bool groupLodCounts(U32 lod_group_id, U32 tris[NUM_LOD], U32 verts[NUM_LOD]) const;

    // Capture one terrain face. Distinct API from capture() because
    // terrain geometry comes from LL's per-region heightmap walker,
    // not LLVolumeFace -- positions/normals are tight vec3 (12 B not
    // 16) and indices are U16 group-relative (caller must subtract
    // the face's group geom-base before passing). LOD always 0.
    // Re-capture of the same (mesh_id, face_idx) FREES the previous
    // slot and allocates a new one -- terrain rebuilds on LOD change.
    void captureTerrain(const LLUUID& mesh_id, S32 face_idx,
                        const LLVector3*  positions,
                        const LLVector3*  normals,
                        const LLVector2*  texcoords,
                        const LLVector4a* tangents,  // may be null
                        const U16*        indices,   // face-local 0..nv-1
                        U32 num_verts, U32 num_indices,
                        // Smooth terrain (#211): force = the caller knows the content
                        // changed (bypass never-downgrade + the identical-bounds skip, which
                        // swallow edits that keep min / max); tight = no 1.5x headroom on a
                        // fresh allocation (fixed-size levels, built / freed by residency).
                        // reclaimable = garbageCollect may free the slot once no live
                        // instance has referenced it for keep_age frames (terrain patches,
                        // water, grass: keyed by objects that die with their region).
                        // False for owners that cache the slot / group id themselves (tree
                        // species, smooth-terrain levels): they free it with freeAsset().
                        bool force = false, bool tight = false, bool reclaimable = false);

    // Capture one water face. Identical storage to terrain (tight
    // vec3 + vec2 + U16 face-local) so internally we delegate; the
    // distinct API exists to keep the call sites self-documenting and
    // give us a hook if water grows distinct attribute needs later.
    void captureWater(const LLUUID& vo_id, S32 face_idx,
                      const LLVector3* positions,
                      const LLVector3* normals,
                      const LLVector2* texcoords,
                      const U16*       indices,
                      U32 num_verts, U32 num_indices, bool reclaimable = false);

    // GLTF SCENE ASSET PRIMITIVE (VK, #39). One glTF mesh primitive as one slot keyed
    // (mesh_id, lod, face_idx) -- the scene-asset ingest (llcleangltfingest.cpp) flattens
    // (mesh, primitive) into face_idx and captures at a single level. Distinct from
    // capture() (LLVolumeFace, U16 indices) because glTF primitives carry U32 indices
    // and routinely exceed 65535 vertices. Positions/normals/tangents are LLVector4a
    // (tangent.w = handedness), uv is stored as given (the caller flips V into SL's
    // convention). Triangle LISTS only (the caller converts strips/fans). Slots are
    // RECLAIMABLE: garbageCollect frees them like mesh geometry once no live instance
    // references them. An existing valid slot is kept (returns true) unless force.
    // normals/tangents may be null (unlit primitives). Returns true when a valid slot
    // exists for the key afterwards.
    bool captureGLTFPrimitive(const LLUUID& mesh_id, S32 lod, S32 face_idx,
                              const LLVector4a* positions, const LLVector4a* normals,
                              const LLVector4a* tangents, const LLVector2* texcoords,
                              const U32* indices, U32 num_verts, U32 num_indices,
                              bool force = false);
    // Free EVERY slot of mesh_id (all LODs) now: storage, SlotInfo, LOD-group
    // publication, VK mirror, latch -- the per-asset half of garbageCollect, for an
    // owner that knows nothing references the asset any more (a replaced glTF local
    // preview). Returns bytes freed.
    U64 freeAsset(const LLUUID& mesh_id);

    // Look up a single face. valid=false when nothing was captured.
    Slot getSlot(const LLUUID& mesh_id, S32 lod, S32 face_idx) const;


    // OWN THE MESH DECODE. LL downloads the mesh asset; we grab the raw zlib'd LOD
    // bytes at LLMeshRepoThread::lodReceived and keep OUR OWN copy here, keyed by
    // (mesh_id, lod). registerCleanFace decodes them into the mega-VBO via a
    // transient LLVolume we invoke + immediately discard -- so the geometry lives
    // ONLY in our buffer + these compressed bytes, never in an LL object whose
    // lifetime LL controls. retainRawMesh runs on the mesh worker thread (copies
    // under its own lock); getRawMesh copies the bytes out for the render thread.
    void retainRawMesh(const LLUUID& mesh_id, S32 lod, const U8* data, S32 size);
    bool getRawMesh(const LLUUID& mesh_id, S32 lod, std::vector<U8>& out) const;
    bool hasRawMesh(const LLUUID& mesh_id, S32 lod) const;
    // The raw bytes are a BYTE-BUDGETED LRU (RenderVKRawMeshCacheMB): they are only
    // needed to (re)decode a (mesh, lod) that is not captured -- first sight, and again
    // after the geometry GC freed it. An evicted key is remembered (bounded) so the
    // ingest can re-request it from LL's mesh repository (disk cache): LL may still hold
    // that LOD loaded and would never re-deliver it on its own. Main thread.
    bool wasRawMeshEvicted(const LLUUID& mesh_id, S32 lod) const;
    void setRawMeshBudgetMB(U32 mb);
    // The (mesh, lod) is fully captured: its bytes are not needed again unless the GC
    // frees it, so move them to the eviction end of the LRU (evicted before bytes still
    // waiting for their first decode).
    void demoteRawMesh(const LLUUID& mesh_id, S32 lod);

    // ASYNC MESH DECODE (off the render thread). Decoding a mesh LOD (zlib inflate +
    // LLSD parse + MikkTSpace tangents + meshopt vertex-cache optimize) was ~23% of the
    // main thread in a crowd. The decode now runs on the CleanMeshDecode pool into an
    // OWNED LLVolume (built from our retained raw bytes -- never an LL-lifetime
    // object); capture() itself (slot alloc, SlotInfo publish, CPU mirror, all the
    // #29 guards) stays on the render thread, unchanged.
    //  takeDecoded(): READY hands back the decoded volume (one-shot; the caller
    //   captures it now); PENDING = queued/in flight (waiter recorded -- it is woken
    //   via pumpDecodes when the result lands); NO_BYTES = raw bytes not retained yet
    //   (caller fetches as before); FAILED = decode failed (caller negative-caches).
    //   `params` is the requester's volume params (first requester wins, exactly as
    //   the synchronous decode's first capture did). priority = self/HUD: jumps the
    //   queue.
    //  adoptDecodedFromLL(): LLMeshRepoThread::lodReceived has JUST decoded the same
    //   bytes for LL's own volume (same params, same detail) -- deep-copy its faces
    //   into an owned volume there (mesh thread) so the common first capture needs no
    //   second decode at all. Skipped if the (mesh, lod) is already captured.
    //  pumpDecodes(): render thread, once per frame: collects the objects waiting on
    //   decodes that completed since the last call (the caller re-ingests them), and
    //   expires results nobody took (object left / captured by another path).
    enum DecodeState : U8 { DECODE_NO_BYTES = 0, DECODE_PENDING, DECODE_READY, DECODE_FAILED };
    //   face_idx: the face the requester needs -- a (mesh, lod) whose decode is known
    //   to lack it, or that is already captured (a re-decode could not add a slot:
    //   capture() is latched per asset and the bytes are immutable), fails at once
    //   without decoding. site: 1 = capture path, 2 = LOD-converge path (diagnostics).
    DecodeState takeDecoded(const LLUUID& mesh_id, S32 lod, const LLVolumeParams& params,
                            S32 face_idx, U8 site, bool priority, const LLUUID& waiter,
                            LLPointer<LLVolume>& out);
    void adoptDecodedFromLL(const LLUUID& mesh_id, S32 lod, const LLVolume* src);
    void pumpDecodes(std::vector<LLUUID>& wake);

    // ---- INGEST DIAGNOSTICS (VKIngestDiag, #88) ----
    // Why capture() left a face without a slot. EMPTY_FACE is legitimate (the LOD has
    // no triangles for that face, e.g. an uploader's "NoGeometry" placeholder); the
    // others are NOT-READY/REFUSED skips, which no longer latch the asset (retried).
    enum CaptureFail : U8 { CAPF_NONE = 0, CAPF_EMPTY_FACE, CAPF_DEGEN_IDX, CAPF_IDX_OOR,
                            CAPF_VERT_ALLOC, CAPF_IDX_ALLOC, CAPF_COUNT };
    // Who cleared a (mesh, lod) capture latch.
    enum UnlatchSite : U8 { UNL_NONE = 0, UNL_NOTHING_CAPTURED, UNL_PARTIAL_RETRY, UNL_GC,
                            UNL_INVALIDATE, UNL_HEAL, UNL_SRC_MUTATION, UNL_COUNT };
    static const char* captureFailName(U8 r);
    static const char* unlatchSiteName(U8 s);
    static const char* decodeEndName(U8 e);
    static const char* decodeStateName(S32 s);   // -1 = no table entry
    struct IngestDiag
    {
        bool latched = false;         // mCapturedAssets[(mesh,lod)] set
        bool latch_known = false;     // an entry exists (true or false)
        bool raw = false;             // retained raw LOD bytes present
        S32  decode_state = -1;       // DecodeState of the table entry, -1 = none
        bool decode_started = false;
        U32  decode_waiters = 0;
        U32  hist_decodes = 0;        // decodes run for this key (session)
        S32  hist_nfaces = -1;        // face count of the decoded volume
        U8   hist_last_end = 0;       // DecodeEnd of the previous result
        U8   fail_reason = 0;         // CaptureFail of the last skip of this face
        U32  fail_frame = 0, fail_count = 0, fail_nv = 0, fail_ni = 0;
        U8   unlatch_site = 0;        // UnlatchSite of the last latch clear
        U32  unlatch_frame = 0, unlatch_count = 0;
    };
    // Snapshot everything the store knows about (mesh, lod, face) for the diag. Takes
    // mMutex, then mDecodeMutex (never nested in the reverse order), then the raw lock.
    void diagQuery(const LLUUID& mesh_id, S32 lod, U8 sculpt_type, S32 face_idx, IngestDiag& out);
    bool isLatched(const LLUUID& mesh_id, S32 lod) const;



    // Reclaim mega-VBO slots for regular meshes that no live instance
    // references anymore (objects derezzed). in_use is the set of slot_ids the
    // live instances currently point at (LLCleanInstanceStore::collectInUseMeshSlots);
    // a mesh is freed only when ALL its faces have been unreferenced for >=
    // keep_age frames (conservative -- avoids freeing across a rebuild-window
    // blip; re-capture re-fires on re-rez). Terrain/water (reclaimable=false)
    // is never touched. Frees the vert/idx ranges and recycles the slot_id.
    // Returns bytes freed. This is the geometry-side twin of the texture GC --
    // without it, every unique mesh ever seen stays in the mega-VBO forever.
    U64 garbageCollect(const std::unordered_set<U32>& in_use, U32 current_frame, U32 keep_age);
    // SLICED twin of garbageCollect for the budget GC (no VRAM trim): visits assets from
    // where the previous slice stopped for at most budget_s (at least one asset) and adds
    // the bytes it frees to freed_bytes; true when the walk reached the end (a full pass).
    // A face is in use when any group publishing its slot (mSlotPubs) has a live instance
    // (mGroupRefs) -- the instance store's refcounts, so no instance walk / in-use set,
    // and the answer is as of the visit, not of a snapshot frames old.
    // #737 liveness: keep_age also counts from the moment the last instance of a group
    // publishing the slot let go (mGroupUnrefFrame), so a slot released 2 s ago is kept
    // the full keep_age even when its last GC visit was long before.
    bool garbageCollectSlice(U32 current_frame, U32 keep_age, F64 budget_s, U64& freed_bytes);
    // VK: approximate resident geometry bytes (allocator in-use counts at the #573
    // quantized sizes: 20 B/vert + a 40 B header per slot, 2 B/index; weights are not
    // counted here) -- the VK geometry-GC pressure gauge. The stats line reports the exact
    // mirror bytes instead (mirrorBytesLocked).
    // Plus a per-slot metadata estimate (Slot map node + hash index + SlotInfo + mirror
    // vector headers + LOD-group share): without it a stream of tiny prims (a script
    // cycling shape params) parks millions of slots of bookkeeping under the budget.
    static constexpr U64 kSlotOverheadBytes = 384;
    U64 residentGeometryBytes() const
    { return (U64)mVertUsed * LLCleanQV::kStride + (U64)mSlots.size() * (LLCleanQV::kHeaderVerts * LLCleanQV::kStride)
           + (U64)mIdxUsed * 2u + (U64)mSlots.size() * kSlotOverheadBytes; }

    // ---- LOD-GROUP LIFETIME ----
    // Live instances (LLCleanInstanceStore) are the users of a group: the store calls
    // groupRefSwap from every write of an instance's lod_group_id (upsert / release /
    // releaseObject), with its own lock held -- the refcounts take only a leaf mutex.
    void groupRefSwap(U32 old_gid, U32 new_gid);
    void resetGroupRefs();   // instance store shutdown
    // Free groups that no instance references, that publish no slot (every level
    // SENTINEL -- an owner caching a gid it never re-registers, e.g. tree species /
    // system-avatar parts, keeps its slots and therefore its group), and that nothing
    // re-published for kGroupGraceFrames. The (mesh, face) mapping and its converge
    // trace go with it; the id is reused after kIdQuarantineFrames. Returns # freed.
    U32 gcLodGroups(U32 current_frame);
    // Sliced gcLodGroups: at most max_groups group ids from the previous slice's cursor;
    // freed counts into `freed`; true when the walk wrapped (a full pass).
    bool gcLodGroupsSlice(U32 current_frame, U32 max_groups, U32& freed);
    // Geometry keyed by mutable per-object state (a flexi's scale-salted key) is
    // orphaned whenever that state changes. retireAssetWhenUnused queues the old key;
    // reapRetiredAssets (each GC tick) frees it, all LODs, kRetireDelayFrames later IF
    // every slot is reclaimable and no group naming any of them has a live instance
    // (refcount > 0) -- otherwise it is left to the budgeted GC. Bounded queue.
    void retireAssetWhenUnused(const LLUUID& mesh_id);
    U32  reapRetiredAssets(U32 current_frame);
    static constexpr U32 kRetireDelayFrames = 120;
    // VK renderer: group rows written since the last drain (deduplicated by caller).
    // Returns true when the caller must recopy ALL rows (list overflowed / first drain).
    bool drainLodGroupDirty(std::vector<U32>& out);

    // ---- MEMORY STATS (VKMem line + RenderVKMemStatsLog dump) ----
    struct MemStats
    {
        U32 slots_live = 0, slot_ids_hw = 0, slot_ids_free = 0, slot_ids_quar = 0;
        U32 groups_live = 0, groups_hw = 0, groups_free = 0, groups_quar = 0, groups_ref = 0;
        U32 assets = 0, face_map = 0, conv_trace = 0, cap_fail = 0, unlatch = 0;
        U64 geom_bytes = 0;      // exact CPU mirror bytes: vertices (+headers) + indices + weights
        U64 geom_wt_bytes = 0;   // of which weights
        // cluster-owned mirrors freed (queueClusterMirrorRelease): slots, their bytes (not in
        // geom_bytes any more), of which re-materializing
        U32 mirror_rel = 0, mirror_wanted = 0;
        U64 mirror_rel_bytes = 0;
        U32 raw_entries = 0, raw_evicted_known = 0;
        U64 raw_bytes = 0, raw_peak = 0, raw_budget = 0, raw_evictions = 0;
        U32 dec_entries = 0, dec_failed = 0, dec_hist = 0, cap_jobs = 0;
        U32 retired_pending = 0, retired_freed = 0;
        U64 dec_bytes = 0;
    };
    // Render/main thread (takes mMutex, then mDecodeMutex, then the raw/ref leaf locks).
    void memStats(MemStats& out);
    std::string memStatsLine();           // one compact segment for the 5 s VKMem line
    void logMemStatsDetailed();           // per-minute dump (walks the slot mirrors)


    U32 assetCount() const;
    U32 slotCount() const;
    U32 slotInfoCount() const;   // # SlotInfo entries (slot_id space) on CPU
    bool debugGetSlotInfo(U32 slot_id, SlotInfo& out) const;

    // #29 freeze profiler: capture()/captureTerrain()/captureWater() upload
    // geometry into the mega-VBO via glBufferSubData and run OUTSIDE renderFrame
    // (mesh decode / registerFace), so the re-capture storm (the prime freeze
    // suspect) won't show up in renderFrame phase timing. Account it here.
    // addCaptureStat is called by each capture path; takeCaptureStats is called
    // once per frame by the renderer to read+reset the running totals.
    static void addCaptureStat(F64 seconds, U64 bytes);


    // MIRROR (llcleanrenderervk.cpp). SlotInfo is byte-identical (32 B)
    // to the VK renderer's; the CPU mirror mSlotInfo is the authoritative slot
    // table (the SSBO is just its GPU copy). Read-only span; no lock (render
    // thread only). NOTE: the actual VERTEX/INDEX geometry has NO persistent CPU
    // mirror -- it is decoded straight into the sectioned mega-VBO and the CPU
    // copy is dropped after flushUploads(). So a VK path that wants real geometry
    // must first grow a CPU geometry mirror here (see llcleanrenderervk.cpp
    // fillFromCleanStores() and its LL_HAS_CLEAN_PIPELINE gate). These two
    // accessors expose only the metadata tables, which ARE CPU-resident.

    const U8* cpuLodGroupBytes() const
    {
        return mLodGroups.empty() ? nullptr
             : reinterpret_cast<const U8*>(mLodGroups.data());
    }
    U32 cpuLodGroupCount() const { return (U32)mLodGroups.size(); }

    // ---- PERSISTENT CPU GEOMETRY MIRROR ------------------------------------
    // The Vulkan world renderer packs every resident slot into one interleaved vertex
    // stream + one flat 16-bit index buffer, so capture() retains a per-slot CPU mirror in
    // EXACTLY that format, keyed by slot_id: the QUANTIZED vertex (#573, llcleanqvertex.h,
    // 20 B: lattice position / UV, octahedral normal + tangent) prefixed by the slot's
    // 40 B decode header (2 pseudo-vertices), and the face-local U16 indices (an SL face
    // has <= 65536 vertices). The packer copies header + vertices verbatim.
    //
    // MirrorVertex is the DECODED / authoring form (48 B floats): what captures are built
    // from and what CPU consumers (decimation, picking) decode back to. tangent.w =
    // handedness (LL's +/-1); a zero tangent means none (terrain/water) and the FS falls
    // back to a derivative frame.
    struct MirrorVertex { F32 pos[3]; F32 uv[2]; F32 normal[3]; F32 tangent[4]; };
    static_assert(sizeof(MirrorVertex) == 48, "MirrorVertex must be 48 B");
    typedef LLCleanQV::QVertex QVertex;
    typedef LLCleanQV::QHeader QHeader;
    static constexpr U32 QV_HEADER = LLCleanQV::kHeaderVerts;

    // Quantize `n` decoded vertices into a header-prefixed range (out.size() = n + 2).
    // `domain` = an existing slot's decode header to encode against (a decimated subset
    // of that slot: identical lattice -> bit-identical positions / UVs); null = fit one.
    static void quantizeVerts(const MirrorVertex* v, U32 n, std::vector<QVertex>& out,
                              const QHeader* domain = nullptr);
    // Decode a header-prefixed range of `n` vertices.
    static void decodeVerts(const QVertex* range, U32 n, std::vector<MirrorVertex>& out);

    // One slot's packed range: pointer to its HEADER (QV_HEADER pseudo-vertices, then
    // out_verts real vertices), or nullptr + 0 when the slot has no retained geometry.
    // Render-thread only (same as flushSlotInfo); no lock taken -- capture() and the VK
    // sync both run on the render thread.
    const QVertex* cpuSlotPacked(U32 slot_id, U32& out_verts) const;
    // A slot's decode header (false = none). Render thread.
    bool cpuSlotHeader(U32 slot_id, QHeader& out) const;
    const U16* cpuSlotIndices(U32 slot_id, U32& out_count) const;
    // Highest slot_id + 1 that the mirror knows about (== mSlotInfo table size).
    U32 cpuMirrorSlotCount() const { return (U32)mMirrorVerts.size(); }

    // Monotonic counter bumped every time the CPU slot mirror (verts/indices/
    // weights) is mutated by a capture. The VK sync uses it to skip the (expensive)
    // per-frame repack of the whole mega-geometry when nothing changed -- geometry
    // only changes on ingest, not every frame.
    U64 geomGeneration() const { return mGeomGeneration.load(std::memory_order_relaxed); }
    // Slots whose VK mirror was (re)written since the last drain (appended at every
    // mirror write). The VK renderer drains this to place ONLY changed slots instead of
    // repacking all geometry on every generation bump.
    void drainMirrorDirty(std::vector<U32>& out)
    {
        std::lock_guard<std::mutex> lk(mMutex);
        out.swap(mMirrorDirty);
        mMirrorDirty.clear();
    }

    // Per-vertex skin weights (#582, llcleanqvertex.h QWeight: 4 x u8 joint + 3 x unorm16
    // weight, the 4th implied; 10 B), one per mirror vertex, encoded from LL's
    // float-per-influence weight4 by LLCleanQV::encodeWeight (normalized exactly like the
    // pre-skin's old float path). Only retained for RIGGED slots in VK mode; nullptr +
    // count 0 otherwise (the VK pre-skin compute skips slots with no weights). Parallel to
    // the slot's vertices (no header entries).
    typedef LLCleanQV::QWeight MirrorWeight;
    static_assert(sizeof(MirrorWeight) == 10, "MirrorWeight must be 10 B (#582)");
    const MirrorWeight* cpuSlotWeights(U32 slot_id, U32& out_count) const;

    // ---- CLUSTER-OWNED MIRROR RELEASE (2026-10-16, after #798) ----
    // A rigged mesh the cluster path owns (cluster-owns-rigged mode, its LOD group routed-only:
    // no legacy GPU geometry) needs its CPU mirror only as the DAG builder's source. Once its
    // DAG is on disk (the .clod write landed: LLCleanClusterCache::pageSource Ready / Full) the
    // renderer queues the group's slots here and the mirror is FREED at the ingest sync point
    // (pumpMirrorReleases: the fill worker is not running then). Everything else stays: the
    // Slot, SlotInfo (counts, bounds), the LOD-group publication, the capture latch, skin info.
    // cpuSlot* then return nullptr + 0 for the slot (every caller handles that).
    // RE-MATERIALIZE (requestMirror): legacy needs the slot again (cluster LOD off / the mode
    // flipped / a legacy row shares the group: the packer's placeSlot asks) or the builder needs
    // its source (cache miss / corrupt or evicted .clod / builder version / rebuild: request()
    // asks). The slot is marked WANTED; the ingest wakes the objects naming its group
    // (takeRematWake), and registerCleanFace re-decodes that (mesh, lod) -- our retained raw
    // bytes, or LL's mesh repository again (its disk cache) when they are gone -- and
    // re-captures it in place (same slot id; the latch is bypassed for wanted keys). Async,
    // never blocking: until the mirror is back the slot simply has no legacy geometry.
    // Render thread (queue / request / pump); mirrorReleased also from the fill worker.
    void queueClusterMirrorRelease(U32 slot_id, const LLUUID& mesh_id, S32 lod, S32 face_idx);
    void cancelMirrorReleases();          // the cluster path stopped owning rigged meshes
    U32  pumpMirrorReleases();            // ingest sync point: frees the queued mirrors (# freed)
    bool mirrorReleased(U32 slot_id) const;
    // why: a short tag for the log ("legacy", "cluster build", ...). True = newly wanted.
    bool requestMirror(U32 slot_id, const char* why);
    // Groups naming slots that became WANTED (and, with refresh, every still-wanted slot's
    // groups again: the safety net for a wake that found no object). Render thread.
    void takeRematWake(std::vector<U32>& groups, bool refresh);
    // Ingest: the lowest WANTED level of (mesh, face), false = none. Lock-free when nothing is
    // wanted (rematPending() == 0).
    bool rematWantedFace(const LLUUID& mesh_id, S32 face_idx, S32& lod_out) const;
    U32  rematPending() const { return mRematWanted.load(std::memory_order_relaxed); }
    // The groups publishing `slot_id` (any level). Render thread.
    void slotPublishers(U32 slot_id, std::vector<U32>& out) const;
    struct MirrorRelStats
    {
        U32 released = 0;        // slots whose mirror is freed now
        U64 bytes = 0;           // their mirror bytes (vertices + header, indices, weights)
        U32 wanted = 0;          // of which re-materialization is pending
        U32 stale = 0;           // of which wanted for over kRematStaleSec
        U64 freed_total = 0, remat_total = 0, asked_total = 0;   // session counts
        U64 freed_bytes_total = 0;
    };
    MirrorRelStats mirrorRelStats() const;
    static constexpr F64 kRematStaleSec = 60.0;

    // RAW SLOT (system avatar body, llcleansysavatar.cpp). One slot keyed (mesh_id, lod,
    // face_idx) from caller-built interleaved vertices / encoded weights (null = static) /
    // face-local U32 indices. Always (re)writes: an existing slot keeps its slot_id and
    // gets its SlotInfo swapped in place, then the old range is freed (capture()'s atomic
    // re-capture) -- a same-shape rewrite is placed in place by the VK packer (morphs).
    // reclaimable=false keeps garbageCollect off it: the owner frees it with freeAsset().
    // Returns the slot_id, or 0xFFFFFFFF (bad data / store not ready).
    // nv must be <= 65536 (16-bit face-local indices, #573). `domain`: see quantizeVerts.
    U32 captureRawSlot(const LLUUID& mesh_id, S32 lod, S32 face_idx, const MirrorVertex* verts,
                       const MirrorWeight* weights, const U32* indices, U32 nv, U32 ni,
                       bool reclaimable, const QHeader* domain = nullptr);

    // ---- ASYNC CAPTURE (RenderVKIngestWorker) ----
    // capture() = prepareCapture (pure data: validation, bounds, index widening,
    // interleave, tangent generation, weights -- reads only the given volume) +
    // commitCapture (render thread: latch, slot / SlotInfo allocation, CPU mirror, the
    // #29 guards' bookkeeping and logging). The ingest posts the prepare half of prim
    // tessellation and decoded-mesh captures to a worker; the render thread commits the
    // results at the ingest sync point (pumpCaptures, inside vkIngestTick -- before
    // renderFrame, so the store-fill worker's "nothing writes the stores during the UI
    // pass" invariant holds).
    struct CapturePrep
    {
        struct Face
        {
            U8   skip = 0;            // CaptureFail reason for a not-ready / empty face
            U32  nv = 0, ni = 0, idx_max = 0;
            bool has_weights = false, has_tangents = false;
            F32  lo[3] = {0.f, 0.f, 0.f}, hi[3] = {0.f, 0.f, 0.f};
            U32  uv_density = 0;               // computeUVDensity
            std::vector<QVertex>      verts;   // VK mirror (header-prefixed, quantized, #573)
            std::vector<U16>          idx;     // face-local
            std::vector<MirrorWeight> wts;     // only when has_weights
        };
        std::vector<Face> faces;
        S32 sculpt_level = -1;
    };
    // Any thread. Reads only `volume` (must not be mutated meanwhile).
    static void prepareCapture(const LLVolume* volume, CapturePrep& out);
    // Render thread. Consumes prep's vectors (moved into the mirror).
    void commitCapture(const LLUUID& mesh_id, S32 lod, CapturePrep& prep, bool force);

    struct CaptureWaiter { LLUUID obj; S32 face_idx = 0; U8 site = 0; bool bookkeep = false; };
    // Render thread. True when an async capture of (mesh_id, lod) is in flight; the
    // waiter (if any) is added to it and woken when it commits.
    bool captureInFlight(const LLUUID& mesh_id, S32 lod, const CaptureWaiter* w);
    // Render thread. Queue a capture of (mesh_id, lod): `src` = an owned, immutable volume
    // (async-decoded mesh; the job keeps a reference, released on the render thread), or
    // null = tessellate the prim (params, detail) + tangents on the worker. False = not
    // queued (worker stopped / pool closed): the caller captures synchronously.
    bool postCapture(const LLUUID& mesh_id, S32 lod, const LLPointer<LLVolume>& src,
                     const LLVolumeParams& params, F32 detail, const CaptureWaiter& w);
    struct CaptureDone { LLUUID mesh_id; S32 lod = 0; std::vector<CaptureWaiter> waiters; };
    // Render thread, once per frame (ingest sync point): commit every finished job.
    void pumpCaptures(std::vector<CaptureDone>& out);
    U32  capturesInFlight() const { return (U32)mCapJobs.size(); }

    // ---- LOD GROUP (upload-once / GPU LOD select) ----
    // Ensure a LOD group exists for (mesh_id, face_idx) and record `slot_id` as
    // its level-`lod` slot. Returns the stable lod_group_id the instance stores.
    // Publishing the slot into the group is what makes that LOD drawable; until
    // then the level is LOD_SENTINEL and the cull emits nothing for it. Called
    // from the capture/registration path on the render thread.
    U32    registerLodSlot(const LLUUID& mesh_id, S32 face_idx, S32 lod, U32 slot_id);
    // Existing group id for (mesh_id, face_idx), or LOD_SENTINEL if none yet.
    U32    getLodGroupId(const LLUUID& mesh_id, S32 face_idx) const;
    // Insert the (non-sentinel) slot_ids of this LOD group into `out`. The GC frees
    // by slot_id, but an instance now references a GROUP -- ALL its resident LOD
    // slots are in use (the cull may pick any level). Without expanding groups to
    // slots here, collectInUseMeshSlots reported group_ids and the GC freed live
    // slots -> #29 mass vanish (proven by the provenance tripwire).
    void   collectGroupSlots(U32 group_id, std::unordered_set<U32>& out) const;
    // VRAM soft ladder (#405 mesh detail): the group's slots of levels 0..max_level only,
    // plus -- when none of those is resident -- the lowest resident level above, so the
    // instance keeps drawing something until the wanted level is captured. A level a
    // fallback publish points at another level's slot is kept with that level.
    void   collectGroupSlotsUpTo(U32 group_id, S32 max_level, std::unordered_set<U32>& out) const;
    // While set (renderer: a mesh-detail or far-geometry rung is engaged), the VK geometry
    // GC (LLCleanIngest::vkGeometryGC) runs on a short cadence regardless of
    // RenderVKGeometryBudgetMB, counts only the levels instances can select at the
    // EFFECTIVE LOD factor (LLCleanInstanceStore::collectInUseMeshSlotsLod), and the
    // renderer compacts / shrinks the packed geometry buffers sooner.
    void   setVramPressureTrim(bool on) { mVramTrim.store(on, std::memory_order_relaxed); }
    // (mesh, lod) captures re-latched < 2400 frames after the GC freed them (GC thrash
    // probe, logged by vkGeometryGC); read-and-reset.
    U32    takeGcRecaptures() { return mGcRecaptures.exchange(0, std::memory_order_relaxed); }
    bool   vramPressureTrim() const { return mVramTrim.load(std::memory_order_relaxed); }
    // Volume-local bounds of every RESIDENT LOD in the group (their captured slot
    // bounds), unioned into lo/hi. The instance AABB is only the first-captured LOD's;
    // the cull and the pick broadphase must use this union. Returns false if none.
    bool   groupLocalBounds(U32 group_id, F32 lo[3], F32 hi[3]) const;

    // #29 RACE FIX: capture()/captureTerrain() run mid-frame (registerFace can
    // fire during the clean render passes). A glBufferSubData to the mega-VBO
    // while the GPU is reading it for an in-flight draw RACES and the write is
    // silently dropped -> geometry reads as zeros (mass dropout). So capture
    // QUEUES the geometry (CPU copy) instead of uploading inline, and the
    // renderer calls flushUploads() ONCE per frame at a controlled point (start
    // of the first renderFrame pass, before any draw binds the mega-VBO).

private:
    LLCleanMeshAsset() = default;
    std::atomic<bool> mVramTrim{false};   // setVramPressureTrim
    std::atomic<U32>  mGcRecaptures{0};   // takeGcRecaptures
    LLCleanMeshAsset(const LLCleanMeshAsset&) = delete;
    LLCleanMeshAsset& operator=(const LLCleanMeshAsset&) = delete;

    static constexpr U32 POS_STRIDE  = 16; // LLVector4a
    static constexpr U32 UV_STRIDE   = 8;  // LLVector2
    static constexpr U32 NORM_STRIDE = 16; // LLVector4a
    static constexpr U32 TAN_STRIDE  = 16; // LLVector4a (w = sign)
    static constexpr U32 WT_STRIDE   = 16; // weight4 (vec4 packed as float-per-joint)
    static constexpr U32 IDX_STRIDE  = 4;  // GL_UNSIGNED_INT
    static constexpr U32 VERT_STRIDE_TOTAL =
        POS_STRIDE + UV_STRIDE + NORM_STRIDE + TAN_STRIDE + WT_STRIDE;

    // Dedicated VAO for multi-draw paths. Without this our
    // glVertexAttribPointer calls leak into LL's default VAO state;
    // LL's setBuffer skips attribute setup when its cached
    // sGLRenderBuffer == mGLBuffer, so subsequent draws after ours
    // read from our mega-VBO via stale pointers (rogue water plane).
    // VAO encapsulates attribute state; switching back to LL's VAO
    // (0) restores its untouched configuration.
    std::vector<SlotInfo> mSlotInfo;
    bool groupLocalBoundsLocked(U32 group_id, F32 lo[3], F32 hi[3]) const;   // mMutex held
    U32                   mSlotInfoDirtyMin = 0xFFFFFFFFu;
    U32                   mSlotInfoDirtyMax = 0;
    // GPU-side capacity (entries) of mSlotInfoSSBO. mSlotInfo (CPU)
    // grows unbounded as slots are captured; the SSBO must grow to
    // match or the cull reads garbage SlotInfo past the end (transient
    // "vert_count" garbage -> rigged corruption).
    U32                   mSlotInfoGPUCap = 0;

    // LOD groups (upload-once / GPU LOD select). mFaceLodGroup maps (mesh_id,
    // face_idx) -> group_id; mLodGroups[group_id] holds the per-level slot_ids.
    // Mirrored to mLodGroupSSBO (uvec4 per group) which the cull indexes.
    std::vector<LodGroup>         mLodGroups;
    // (mesh_id, face) keyed: hash maps -- looked up per face per sweep visit, never
    // walked in order (UUID-compare tree walks were a crowd cost).
    struct UuidS32Hash
    {
        size_t operator()(const std::pair<LLUUID, S32>& k) const noexcept
        { return std::hash<LLUUID>()(k.first) ^ ((size_t)(U32)k.second * 0x9E3779B97F4A7C15ull); }
    };
    std::unordered_map<std::pair<LLUUID, S32>, U32, UuidS32Hash> mFaceLodGroup;
    // Per-group bookkeeping, parallel to mLodGroups (mMutex). key = the mFaceLodGroup
    // entry it owns; last_pub = last registerLodSlot touch (gcLodGroups grace).
    struct GroupMeta { LLUUID mesh; S32 face = 0; U32 last_pub = 0; bool live = false; };
    std::vector<GroupMeta>        mGroupMeta;
    U32                           mLiveGroups = 0;
    std::vector<U32>              mFreeGroupIds;                    // reusable now
    std::deque<std::pair<U32, U32>> mGroupQuarantine;               // (gid, freed frame)
    // Reverse publication map: slot_id -> groups naming it at some level. A freed slot
    // is unpublished from EVERY level of EVERY group (fallback publishes name another
    // level's slot; smooth terrain / trees publish slots keyed by another asset id).
    std::vector<std::vector<U32>> mSlotPubs;
    // Group rows written since the VK renderer's last drain (see drainLodGroupDirty).
    std::vector<U32>              mGroupDirty;
    bool                          mGroupDirtyOverflow = true;      // first drain: full copy
    // Instance refcounts per group (leaf lock: taken under the instance store's lock).
    std::vector<U32>              mGroupRefs;
    // #737: gFrameCount when each group's refcount last dropped to 0 (parallel to
    // mGroupRefs, same lock) -- the geometry release grace counts from the last instance
    // letting go, not from the last GC visit that happened to see it referenced.
    std::vector<U32>              mGroupUnrefFrame;
    mutable std::mutex            mGroupRefMutex;
    std::deque<std::pair<LLUUID, U32>> mRetired;                    // (mesh_id, queued frame)
    U32                           mRetiredFreed = 0;
    // Sliced GC cursors (garbageCollectSlice / gcLodGroupsSlice; mMutex).
    Key                           mGcCursor{};
    bool                          mGcCursorValid = false;
    U32                           mGroupGcCursor = 0;
    bool gcGroupFreeableLocked(U32 gid, const std::vector<U32>& refs, U32 current_frame);   // mMutex held: frees gid if unused
    U32  allocGroupIdLocked();                        // mMutex held
    void markGroupDirtyLocked(U32 gid);                // mMutex held
    void pubAddLocked(U32 slot_id, U32 gid);           // mMutex held
    void pubRemoveLocked(U32 slot_id, U32 gid);        // mMutex held (if no level still names it)
    void unpublishSlotLocked(U32 slot_id);             // mMutex held
public:
    static constexpr U32 kIdQuarantineFrames = 8;      // > frames in flight + fill lag
    static constexpr U32 kGroupGraceFrames   = 600;
private:

    // Converge-visit trace per (mesh_id, face_idx); see noteConverge. Guarded
    // by mMutex; read inline by debugGroupString (already under the lock).
    std::unordered_map<std::pair<LLUUID, S32>, ConvergeTrace, UuidS32Hash> mConvergeTrace;
    U32                           mLodGroupGPUCap    = 0;
    U32                           mLodGroupDirtyMin  = 0xFFFFFFFFu;
    U32                           mLodGroupGen       = 1;   // bumped on every LOD-group write
public:
    U32 lodGroupGeneration() const { return mLodGroupGen; }
private:
    U32                           mLodGroupDirtyMax  = 0;

    U32    mVertCap  = 0;
    U32    mVertUsed = 0;
    U32    mIdxCap   = 0;
    U32    mIdxUsed  = 0;
    U32    mVKVertCursor = 0, mVKIdxCursor = 0;   // GL-A10: VK bump offsets
    bool   mReady    = false;

    std::map<Key, Slot> mSlots;
    // Hash index into mSlots (std::map nodes are stable): getSlot runs per face per
    // sweep visit, and the ordered find was a UUID-compare tree walk. mSlots stays
    // ordered -- garbageCollect relies on an asset's faces being contiguous.
    struct KeyHash
    {
        size_t operator()(const Key& k) const noexcept
        { return std::hash<LLUUID>()(k.mesh_id) ^ ((size_t)(U32)(k.lod * 1024 + k.face_idx) * 0x9E3779B97F4A7C15ull); }
    };
    struct KeyEq
    {
        bool operator()(const Key& a, const Key& b) const noexcept
        { return a.mesh_id == b.mesh_id && a.lod == b.lod && a.face_idx == b.face_idx; }
    };
    std::unordered_map<Key, Slot*, KeyHash, KeyEq> mSlotIdx;

    // INGEST DIAGNOSTICS (VKIngestDiag, #88). Why a face has no slot: the last reason
    // capture() skipped (mesh, lod, face), erased when the face captures. Bounded.
    struct CaptureFailRec { U8 reason = 0; U32 frame = 0; U32 count = 0; U32 nv = 0; U32 ni = 0; };
    std::unordered_map<Key, CaptureFailRec, KeyHash, KeyEq> mCaptureFail;
    // Who last cleared the (mesh, lod) capture latch, and how often. Bounded.
    struct UnlatchRec { U8 site = 0; U32 frame = 0; U32 count = 0; };
    std::unordered_map<std::pair<LLUUID, S32>, UnlatchRec, UuidS32Hash> mUnlatch;
    void noteUnlatchLocked(const LLUUID& mesh_id, S32 lod, U8 site);   // mMutex held
    void noteCaptureFailLocked(const Key& k, U8 reason, U32 nv, U32 ni); // mMutex held
    Slot& slotPut(const Key& k, const Slot& s) { Slot& r = mSlots[k]; r = s; mSlotIdx[k] = &r; return r; }
    const Slot* slotFind(const Key& k) const { auto it = mSlotIdx.find(k); return it == mSlotIdx.end() ? nullptr : it->second; }

    // Geometry mirror (see cpuSlotPacked/cpuSlotIndices). Indexed by
    // slot_id, parallel to mSlotInfo.
    // Each capture() writes the slot's interleaved verts + face-local indices here
    // when the mirror is enabled; a re-capture overwrites the same slot_id in
    // place (mirrors the SlotInfo stable-slot-id behaviour).
    std::vector<std::vector<QVertex>>      mMirrorVerts; // per slot_id (header-prefixed, #573)
    std::vector<std::vector<U16>>          mMirrorIdx;   // per slot_id (face-local)
    // VK-mode rigged pre-skin: per-slot 10 B skin weights (#582; parallel to mMirrorVerts).
    // Empty for non-rigged slots; the VK pre-skin compute reads this per vertex.
    std::vector<std::vector<MirrorWeight>> mMirrorWeights; // per slot_id
    // Bumped under mMutex on every mirror mutation (capture/terrain/water). Read
    // lock-free by the render-thread VK sync to skip the per-frame geometry repack.
    std::atomic<U64> mGeomGeneration{0};
    std::atomic<U32> mBestSlotGen{1};   // bestSlotGeneration()
    void noteBestSlotChange() { mBestSlotGen.fetch_add(1, std::memory_order_acq_rel); }
    std::vector<U32> mMirrorDirty;   // slot ids written since the last drainMirrorDirty
    void clearMirrorLocked(U32 slot_id);   // free a slot's mirror + mark dirty (mMutex held)
    // CLUSTER-OWNED MIRROR RELEASE (queueClusterMirrorRelease). mMutex held for all of it.
    struct MirrorRel
    {
        LLUUID mesh_id;
        S32  lod = 0, face = 0;
        U64  bytes = 0;            // mirror bytes freed (element sizes, as memStats)
        F64  want_time = 0.0;      // wanted since (0 = not wanted)
        bool warned = false;       // the stale warning was logged
    };
    std::unordered_map<U32, MirrorRel> mMirrorRel;      // slot_id -> released mirror
    struct RelQ { U32 slot; LLUUID mesh_id; S32 lod, face; };
    std::vector<RelQ> mMirrorRelQueue;                  // queued releases (pumpMirrorReleases)
    std::vector<U32>  mRematWake;                       // groups to wake (takeRematWake)
    std::atomic<U32>  mRematWanted{0};                  // wanted entries (lock-free gate)
    U64 mMirrorRelBytes = 0;
    U64 mStatMirrorFreed = 0, mStatMirrorFreedBytes = 0, mStatRemat = 0, mStatRematAsk = 0;
    // Drop a slot's mirror vectors (capacity too); returns the element bytes. mMutex held.
    U64  dropMirrorVectorsLocked(U32 slot_id);
    // The slot's mirror was (re)written or cleared: forget its release (counts a completed
    // re-materialization when `written`). mMutex held.
    void mirrorRelForgetLocked(U32 slot_id, bool written);
    // (mesh, lod) / (mesh, face) of every WANTED slot (the latch bypass, rematWantedFace).
    std::unordered_map<std::pair<LLUUID, S32>, U32, UuidS32Hash> mRematLods;   // -> wanted slots
    std::unordered_map<std::pair<LLUUID, S32>, U8, UuidS32Hash>  mRematFaces;  // -> wanted LOD bits
    bool rematKeyLocked(const LLUUID& mesh_id, S32 lod) const
    { return !mRematLods.empty() && mRematLods.count(std::make_pair(mesh_id, lod)) != 0; }
    void rematUnwantLocked(MirrorRel& r);
    // Free the slots [it, end) of ONE (mesh_id, lod) asset (garbageCollect / freeAsset):
    // ranges, SlotInfo (zeroed), LOD-group publication, recycled slot id, VK mirror,
    // latch. Erases the range; returns the iterator past it. mMutex held.
    std::map<Key, Slot>::iterator freeAssetRangeLocked(std::map<Key, Slot>::iterator it,
                                                       std::map<Key, Slot>::iterator end,
                                                       U64& freed_bytes);

    // OUR copy of the raw zlib'd mesh-asset LOD bytes (see retainRawMesh). Written
    // on the mesh worker thread, read on the render / decode threads -- own mutex.
    // Byte-budgeted LRU (front = most recently used).
    typedef std::pair<LLUUID, S32> RawKey;
    struct RawEntry { std::vector<U8> bytes; std::list<RawKey>::iterator lru; };
    std::map<RawKey, RawEntry> mRawMesh;
    mutable std::list<RawKey>  mRawLru;
    U64 mRawBytes = 0, mRawPeakBytes = 0, mRawEvictions = 0;
    std::atomic<U64> mRawBudget{256ull << 20};
    // Keys the LRU evicted (bounded FIFO): the ingest re-requests them from LL.
    std::set<RawKey>   mRawEvicted;
    std::deque<RawKey> mRawEvictedOrder;
    mutable std::mutex mRawMeshMutex;
    void evictRawLocked(const RawKey* keep);   // mRawMeshMutex held

    // Async decode state (see takeDecoded). Guarded by mDecodeMutex (never held
    // across a decode or together with mMutex/mRawMeshMutex). Keyed by (mesh, lod,
    // sculpt type/flags): mirror/invert change the decode, so each flag set is its own
    // result. A READY result is SHARED (not one-shot): every requester of the key gets
    // the same volume until the asset is captured (then it is dropped -- the geometry
    // lives in our mirror), it goes untouched for a while, or the byte budget evicts it.
    typedef std::tuple<LLUUID, S32, U8> DecodeKey;
    enum DecodeEnd : U8 { END_NONE = 0, END_LATCHED, END_EXPIRED_UNTAKEN, END_EXPIRED_TAKEN,
                          END_BUDGET, END_FAILED_AGED, END_COUNT };
    struct DecodeEntry
    {
        DecodeState          state    = DECODE_PENDING;
        bool                 started  = false;   // a worker picked it up
        bool                 done     = false;   // finished, waiters not yet woken
        bool                 prio     = false;   // already in the priority queue
        bool                 adopted  = false;   // came from LL's decode (lodReceived)
        U8                   site     = 0;       // requester site bits (1 capture, 2 converge)
        LLVolume*            vol      = nullptr; // owned (one ref) when READY
        U64                  bytes    = 0;       // approx heap bytes of vol
        U64                  t_done   = 0;       // LLTimer::getTotalTime() at completion
        U64                  t_touch  = 0;       // last take (or completion)
        U32                  takes    = 0;
        // #698: out of memory. PENDING with oom_retry_at set = waiting out the backoff
        // (re-queued by the next takeDecoded after it); FAILED with oom_drop = given up
        // after the retry limit, aged out like NO_BYTES (eligible again later).
        U64                  oom_retry_at = 0;   // LLTimer::getTotalTime()
        U8                   oom_tries    = 0;
        bool                 oom_drop     = false;
        LLVolumeParams       params;
        std::vector<LLUUID>  waiters;
    };
    // Per-key history (repeat diagnosis + known face count). Bounded.
    struct DecodeHist
    {
        U32 decodes  = 0;             // decodes run (pool or adopted) for this key
        S32 nfaces   = -1;            // face count of the decoded volume (-1 unknown)
        U8  last_end = END_NONE;      // how the previous result left the table
        U8  sites    = 0;             // requester sites seen
        U32 why[END_COUNT] = {};      // re-decodes by the end reason of the prior result
    };
    std::map<DecodeKey, DecodeEntry> mDecodes;
    std::map<DecodeKey, DecodeHist>  mDecodeHist;
    std::deque<DecodeKey> mDecodeQPrio, mDecodeQ;
    std::unique_ptr<LL::ThreadPool> mDecodePool;
    bool mDecodesStopped = false;   // set by stopDecodes(); blocks lazy re-creation
    // Async capture jobs (see postCapture). mCapJobs / the jobs' LLPointer and waiters
    // are render-thread only; a worker reads its job's inputs, writes job->prep, then
    // hands the job back through mCapDone (mCapMutex) and never touches it again.
    struct CaptureJob
    {
        LLUUID mesh_id;
        S32 lod = 0;
        LLPointer<LLVolume> src;     // never ref'd / unref'd off the render thread
        LLVolumeParams params;
        F32 detail = 0.f;
        std::vector<CaptureWaiter> waiters;
        CapturePrep prep;
    };
    std::map<std::pair<LLUUID, S32>, std::unique_ptr<CaptureJob>> mCapJobs;
    std::vector<CaptureJob*> mCapDone;
    std::mutex mCapMutex;
    std::unique_ptr<LL::ThreadPool> mCapPool;
    void runCaptureJob(CaptureJob* job);   // worker
    U64 mDecodeBytes = 0;          // sum of READY entry bytes (budget)
    std::vector<DecodeKey> mDecodeDone;   // completed with waiters to wake
    // Stats (logged by pumpDecodes ~every 10 s while active).
    U32 mStatDecoded = 0, mStatAdopted = 0, mStatTaken = 0, mStatRepeat = 0;
    U32 mStatEnd[END_COUNT] = {};
    // #534: the 10 s log's repeat totals and top repeaters, kept up to date in
    // noteDecodedLocked (O(1) per decode) instead of walking all of mDecodeHist (45-65k
    // map nodes, twice, under mDecodeMutex) at log time -- a 12 ms render-thread hitch.
    U32 mStatRepeatWhy[END_COUNT] = {};   // sum over keys of DecodeHist::why
    static constexpr size_t kTopRepeaters = 8;
    std::pair<U32, DecodeKey> mTopRepeaters[kTopRepeaters] = {};   // (decodes, key); decodes 0 = empty
    void noteRepeaterLocked(const DecodeKey& key, U32 decodes);
    U32 mStatSkipLatched = 0, mStatSkipNoFace = 0, mStatAdoptSkipped = 0;
    std::mutex mDecodeMutex;
    void runOneDecode();           // worker: pop + decode one queued key
    void noteDecodedLocked(const DecodeKey& key, DecodeEntry& e, LLVolume* vol);
    void dropDecodeLocked(std::map<DecodeKey, DecodeEntry>::iterator it, DecodeEnd why);
    // Tracks which (mesh_id, lod) pairs we've already captured so
    // re-decode of the same LOD doesn't allocate duplicate slots.
    std::map<std::pair<LLUUID, S32>, bool> mCapturedAssets;
    mutable std::mutex mMutex;
    // Rotating cursor for healZeroedResidentVerts so each sweep checks a fresh
    // slice of resident slots (bounded readback cost per frame).
    U32 mVtxHealCursor = 0;

    struct Range { U32 offset; U32 count; };
    // Recycled slot_ids (from garbageCollect / freeAsset). Every capture path takes
    // ids through allocSlotIdLocked, which reuses these before pushing a new mSlotInfo
    // entry, so the slot-info table (and its GPU SSBO) stops growing as you explore.
    // A freed id waits kIdQuarantineFrames in mSlotQuarantine (its SlotInfo is zeroed,
    // so a stale read in that window draws nothing) before it can be reused.
    std::vector<U32>   mFreeSlotIds;
    std::deque<std::pair<U32, U32>> mSlotQuarantine;   // (slot_id, freed frame)
    U32  allocSlotIdLocked();                          // mMutex held; mSlotInfo sized
    void releaseSlotIdLocked(U32 slot_id);             // mMutex held

    U32  allocVerts(U32 count);
    U32  allocIdx  (U32 count);
    void freeVerts (U32 offset, U32 count);
    void freeIdx   (U32 offset, U32 count);

    // #29 detector state: frame of the last face-ingesting capture (strand
    // detector: "is anything healing?") + the bounds auditor's rolling cursor
    // through mSlots.
    U32 mLastCaptureFrame = 0;
    U32 mAuditCursor      = 0;
    static constexpr U32 NUM_CANARIES   = 16;
    static constexpr U32 CANARY_INDICES = 1024;   // 4 KB each
    U32 mCanaryBase[NUM_CANARIES] = {0};          // index offsets; 0 = not planted
    bool mCanariesPlanted = false;



};

// Sizes of the clean-ingest negative / throttle caches that live in llvovolume.cpp
// (capture backoff, LOD fetch throttle), for the memory stats dump.
void llvkCleanIngestCacheStats(std::ostream& os);

// #585: LL's shared mesh volumes release their per-vertex arrays once captured here
// (LLMeshRepository, SLFreeCapturedMeshData). One segment for the VKMem RAM line:
// bytes / faces freed, pending, refills, kept, rehydrates, released-face reads.
std::string llMeshDataReleaseStats();

