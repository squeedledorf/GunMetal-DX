/**
 * @file llcleanskinstore.h
 * @brief GPU-driven skin palette store. SSBO holds a fixed-size
 *        matrix palette + bind shape per registered (avatar, skin)
 *        pair, indexed by skin_id. The vertex shader looks up the
 *        skin block via Instance.skin_id when the instance's rigged
 *        flag is set, blends 4 bones via the weight4 attribute, and
 *        produces a skinned world position. One per-skin upload per
 *        frame replaces N per-LLDrawInfo uniform uploads from LL's
 *        legacy renderRigged.
 *
 *        Sized for SL's MAX_JOINTS_PER_MESH_OBJECT (typically 52),
 *        ~2.5 KB per skin. 200 visible skins -> ~500 KB SSBO.
 */

#pragma once

#include "stdtypes.h"
#include "lluuid.h"
#include "llpointer.h"
#include "llavatarslots.h"

#include <map>
#include <set>
#include <mutex>
#include <vector>

class LLVOAvatar;
class LLMeshSkinInfo;

class LLCleanSkinStore
{
public:
    static LLCleanSkinStore& instance();

    // Match LL_MAX_JOINTS_PER_MESH_OBJECT in lljoint.h. Per-mesh
    // joint count cap; controls palette size in SkinBlock.
    static constexpr U32 MAX_JOINTS  = 110;
    // Per-avatar skeleton joint count. Mesh joint_nums index into
    // the AVATAR skeleton (not the mesh) -- the skeleton can have
    // up to LL_CHARACTER_MAX_ANIMATED_JOINTS = 216. If we size
    // AvatarJoints.world to 110, a mesh that references avatar
    // joint 150 reads OOB garbage and vertices explode.
    static constexpr U32 MAX_AVATAR_JOINTS = 216;
    // Per-session cap on distinct avatars whose joints we cache on
    // GPU. Crowded events can exceed default; oversize fails the
    // skin compute path for that avatar (falls back to static).
    static constexpr U32 MAX_AVATARS = kAvatarSlotCap;   // 1024 (llavatarslots.h; was 256)

    // GPU layout. std430. Note: GLSL mat3x4 is 3 columns of 4 floats
    // = 48 bytes each, matching LL's uniformMatrix3x4fv upload.
    struct SkinBlock
    {
        F32 matrices[MAX_JOINTS * 12]; // MAX_JOINTS * mat3x4 (3 cols x 4 floats)
        F32 bind_shape[16];            // mat4
    };

    // Per-avatar joint world matrices. Refreshed CPU-side per frame
    // by reading LL joint hierarchy world matrices. Sized for full
    // avatar skeleton, NOT per-mesh joint cap (see MAX_AVATAR_JOINTS).
    // Joints are AFFINE (the 4x4's last row is always (0,0,0,1)), so each stores 12
    // floats (48 B, was 64 B): row r of the upper 3x4 at [r * 4 .. r * 4 + 3], i.e.
    // world[r * 4 + c] = M[c][r] for the column-major mat4 M (std430 mat3x4 on the GPU,
    // clean_joint_table.glsl). Use packJointWorld / unpackJointWorld.
    static constexpr U32 JOINT_WORLD_FLOATS = 12;
    struct AvatarJoints
    {
        F32 world[MAX_AVATAR_JOINTS * JOINT_WORLD_FLOATS];
    };
    static void packJointWorld(F32* dst12, const F32* m16)
    {
        for (U32 r = 0; r < 3; ++r)
            for (U32 c = 0; c < 4; ++c) dst12[r * 4 + c] = m16[c * 4 + r];
    }
    static void unpackJointWorld(F32* m16, const F32* src12)
    {
        for (U32 c = 0; c < 4; ++c)
        {
            for (U32 r = 0; r < 3; ++r) m16[c * 4 + r] = src12[r * 4 + c];
            m16[c * 4 + 3] = (c == 3) ? 1.f : 0.f;
        }
    }

    // Per-skin immutable metadata. Filled once at getOrCreate;
    // consumed by the palette compute shader.
    // std430 layout. joint_nums is int[110] (stride 4 = 440 bytes);
    // the following mat4 array needs 16-byte alignment, so 8 bytes
    // of pad sit between. Must match struct SkinMeta in
    // cleanSkinPaletteC.glsl exactly.
    //
    // bind_shape lives here so the palette compute can copy it into
    // SkinBlock each dispatch -- a single C++ upload path for bind_shape
    // would race against the SSBO write semantics of the compute path
    // and lose the data (observed: BIND_SHAPE_GPU read back as all
    // zeros while CPU shadow was correct). Driving it through compute
    // makes the GPU side always live.
    struct SkinMeta
    {
        U32 avatar_idx;
        U32 joint_count;
        U32 pad0;
        U32 pad1;
        S32 joint_nums[MAX_JOINTS];   // 440 B
        S32 pad_for_mat4_align[2];    // 8 B -- aligns inv_bind to 16
        F32 inv_bind[MAX_JOINTS * 16]; // mat4 per joint, column-major
        F32 bind_shape[16];            // mat4
    };

    void shutdown();

    // Register an avatar/skin pair and return its skin_id (stable
    // for the session). Idempotent: same (avatar, skinInfo->mHash)
    // returns the same id.
    U32 getOrCreate(LLVOAvatar* avatar, const LLMeshSkinInfo* skinInfo);

    // Read-only: the pair has an entry that is NOT resolved yet (getOrCreate hands
    // back the sentinel). The sweep keeps such objects unsettled so the instance
    // picks the real id up once it resolves. Unknown pairs are not pending.
    bool isPending(LLVOAvatar* avatar, const LLMeshSkinInfo* skinInfo) const;
    // Diagnostics (VKIngestDiag): entry state and, if unresolved, why.
    std::string describe(LLVOAvatar* avatar, const LLMeshSkinInfo* skinInfo) const;
    // #631 diagnostic: this avatar's skin entries, and how many are still unresolved
    // (joint_count 0: drawn on the static path, i.e. in their bind pose).
    void countAvatarSkins(const LLUUID& avatar_id, U32& total, U32& unresolved) const;

    // Ensure an avatar has a joints slot even if it wears no rigged
    // mesh -- used by statically-worn UNRIGGED attachments, which are
    // drawn rigid-skinned to a single attach-point joint (the matrix is
    // derived in the vertex shader from avatar_joints[slot].world[bone]
    // * local_offset). refreshAll() walks mAvatarSlot, so once a slot is
    // assigned the avatar's full skeleton (attach joints included, they
    // sit in the 0..MAX_AVATAR_JOINTS range) is refreshed every frame.
    // Returns the slot (== avatar joints SSBO index) or INVALID on
    // overflow. Thread-safe (locks mMutex).
    U32 ensureAvatarSlot(LLVOAvatar* avatar);
    // Joint-table slot already assigned to this avatar id, or INVALID (no allocation).
    U32 findAvatarSlot(const LLUUID& avatar_id) const;
    // All assigned (avatar id, slot) pairs.
    void copyAvatarSlots(std::vector<std::pair<LLUUID, U32>>& out) const;
    // VK GPU animation: slots whose joint world matrices the GPU evaluates this frame
    // (LLCleanAnimStore). refreshAllVK skips their CPU joint copy. Reset every frame.
    void clearGpuSlots();
    void setGpuSlot(U32 slot);


    // VK-mode CPU-only refresh. Does exactly the CPU half of refreshAll --
    // revalidate skin metadata (populateSkinMeta) and re-copy each live avatar's
    // joint world matrices into mAvatarJointsCPU -- but performs NONE of the GL
    // work (no SSBO upload, no palette compute dispatch, no shader bind). The VK
    // renderer has no GL context; it calls this each frame and then buildVKPalettes
    // to produce the palettes CPU-side. Thread-safe (locks mMutex).
    void refreshAllVK();

    // ---- VK-MODE CPU PALETTE MIRROR ----------------------------------------
    // The GL path computes the per-skin bone palette (SkinBlock.matrices) on the
    // GPU (cleanSkinPaletteC: joint.world * inv_bind), so no CPU copy of the
    // blended palette exists. The Vulkan renderer has no GL context to read that
    // SSBO back, so in VK mode we compute the palettes CPU-side here (the same
    // world * inv_bind product, packed to the mat3x4 layout cleanSkinPreC reads)
    // and hand them to the VK pre-skin compute as an SSBO.
    //
    // Fills `out` with liveCount() SkinBlocks (mat3x4[MAX_JOINTS] + bind_shape mat4),
    // byte-identical to the SkinBlock the pre-skin compute consumes. Returns the
    // number of skins written. Must be called AFTER refreshAll() (which refreshes
    // the per-avatar joint world matrices this reads). Cheap CPU matmul; the
    // GPU-side palette gen is a later refinement (see project memory). VK only.
    // eye (optional): palettes are built CAMERA-RELATIVE (joint world translation minus
    // eye), so the pre-skin writes small eye-relative positions instead of region-scale
    // ones -- float precision stays in the microns for layered rigged mesh.
    U32 buildVKPalettes(std::vector<F32>& out, const F32* eye = nullptr) const;
    // VK: copy the live avatars' joint WORLD matrices (refreshed by refreshAllVK)
    // into `out` as a packed AvatarJoints[] (index == avatar slot, the skin_id a
    // rigid attachment carries). Returns the avatar count.
    U32 copyVKAvatarJoints(std::vector<U8>& out) const;
    // C7: as copyVKAvatarJoints, but `out` is the caller's MIRROR of the GPU table: only
    // the slots with want[slot] != 0 are compared and copied, and the slot ranges whose
    // row changed are APPENDED to `runs` (the caller streams them, then clears it). Rows
    // not wanted keep whatever the mirror (and the GPU) holds. Returns the avatar count.
    U32 copyVKAvatarJointRows(std::vector<U8>& out, const std::vector<U8>& want,
                              std::vector<std::pair<U32, U32>>& runs) const;
    // VK: per joint-table slot, 1 if its avatar was alive at the last refreshAllVK (its
    // row is current), else 0 (never assigned, dead, or out of the object list: the row
    // holds a stale pose). The temporal history uses it to spot slots coming back.
    void copyVKAvatarAlive(std::vector<U8>& out) const;
    // VK GPU PALETTES: the palette compute (clean_skin_palette.comp) builds the per-skin
    // palettes from the avatar joint table + this STATIC per-skin meta (SkinMeta layout
    // == the shader's). Copies the entries dirtied since the last call (registrations,
    // resolves, re-resolves) into `mirror` (resized to liveCount() * sizeof(SkinMeta)) and
    // returns that range in [dirty_min, dirty_max) (empty when min >= max). Returns the
    // live skin count. VK only.
    U32 copyVKSkinMetaDirty(std::vector<U8>& mirror, U32& dirty_min, U32& dirty_max);
    // Bytes per SkinBlock in the packed VK palette layout (matches cleanSkinPreC's
    // SkinBlock: mat3x4[MAX_JOINTS] (48 B each) + mat4 bind_shape (64 B)).
    static constexpr U32 VK_SKINBLOCK_BYTES = MAX_JOINTS * 48 + 64;

    // ---- Flexi chains (#25) ----------------------------------------------------
    // A flexi prim is skinned as a 9-joint chain. Its joints live in the joint table
    // like an avatar's, packed 24 chains per pseudo slot (216 / 9); the skin meta
    // maps joints base..base+8 with the rest pose's inverse bind, identity bind shape.
    static constexpr U32 FLEXI_POINTS = 9;
    // Register (or update the rest pose of) the object's chain; returns its skin_id, or
    // INVALID when out of joint-table slots.
    U32  flexiChain(const LLUUID& object_id, const F32 inv_bind[FLEXI_POINTS * 16]);
    U32  flexiSkin(const LLUUID& object_id) const;   // INVALID if none
    // This frame's joint WORLD matrices (agent space, column-major) for a chain.
    void flexiSetPose(U32 skin_id, const F32 world[FLEXI_POINTS * 16]);

    // ---- System avatar body (llcleansysavatar.cpp) -----------------------------------
    // One skin per avatar for its classic LLPolyMesh body: joint_nums index the avatar's
    // joint table, inv_bind (16 floats per joint, column-major) carries LL's per-entry
    // skin pivot (or a rigid eyeball's mesh-local transform), identity bind shape. Keyed
    // under the avatar in mKeyToId, so the avatar reap frees it like its rigged skins.
    // Creates or updates (dirty only when the content changed). INVALID on overflow.
    U32  sysBodySkin(LLVOAvatar* avatar, U32 joint_count, const S32* joint_nums, const F32* inv_bind);
    U32  findSysBodySkin(const LLUUID& avatar_id) const;   // INVALID if none

    // HOLDER REFERENCES: LLCleanInstanceStore rows (rigged, flag bit0) hold one reference
    // on their skin id. A MESH skin entry no row references and nobody re-asked for
    // (getOrCreate) over kReapFrames is freed even while its avatar lives -- a live
    // avatar cycling mesh UUIDs used to leak an entry (CPU + GPU meta, palette work) per
    // mesh. No freed id is reused while a row still references it.
    void retargetRef(U32 old_id, U32 new_id);

    U32    capacity() const { return mCapacity; }
    // HIGH-WATER id range [0, liveCount()) mirrored / palette-built each frame. Trimmed
    // from the top as the highest ids are freed, so palette work tracks live skins.
    U32    liveCount() const { return mLiveCount; }

    struct Stats
    {
        U32 high_water = 0, capacity = 0, live = 0, free = 0, referenced = 0;
        U32 avatar_slots = 0, avatar_free = 0, flexi = 0;
        U64 reaped_unused = 0, reaped_skins = 0, trimmed = 0;
        U64 cpu_bytes = 0;
    };
    Stats stats() const;

private:
    LLCleanSkinStore() = default;
    LLCleanSkinStore(const LLCleanSkinStore&) = delete;
    LLCleanSkinStore& operator=(const LLCleanSkinStore&) = delete;

    bool grow(U32 new_capacity);
    bool growSkinMeta(U32 new_capacity);
    // Populate (or repopulate) skin meta + bind_shape for entry id.
    // Returns true if entry is now valid; false if the skin info is
    // still partial -- the slot is sentineled so the consumer takes
    // the static fallback. Idempotent: if nothing changed since last
    // call, returns true without dirtying. Caller holds mMutex.
    bool populateSkinMeta(U32 id);

    struct Entry
    {
        LLVOAvatar*                     avatar = nullptr;
        // Stored at registration so we can detect when the LLVOAvatar*
        // address has been recycled by a different avatar (the OLD
        // avatar got destroyed, new avatar got malloc'd at the same
        // address). isDead() can't distinguish that case -- the new
        // avatar is alive. Without this check, the entry's stored
        // mesh skin info pulls joint matrices from the NEW avatar's
        // skeleton, producing "right mesh, wrong bones" deformation.
        LLUUID                          avatar_id;
        LLPointer<const LLMeshSkinInfo> skin;
        U64                             hash = 0;
    };

    U32    mCapacity   = 0;
    U32    mLiveCount  = 0;

    // GPU palette generation: avatar joints + skin metadata SSBOs +
    // compute shader. Replaces the per-frame CPU palette matmul.
    std::vector<AvatarJoints> mAvatarJointsCPU;  // MAX_AVATARS-sized
    std::vector<U8>           mAvatarDirty;      // MAX_AVATARS bytes
    std::vector<U8>           mSlotGpu;          // MAX_AVATARS bytes (VK GPU animation)
    std::vector<U8>           mAvatarAliveVK;    // MAX_AVATARS bytes (refreshAllVK: alive)
    U32                       mAvatarLive = 0;
    U32                       mSkinMetaCapacity = 0;
    // Set of avatar indices we've already uploaded skin meta for.
    // Skin meta is immutable per-skin -- upload once per registration
    // via a dirty range, no per-frame work.
    U32 mSkinMetaDirtyMin = 0xFFFFFFFFu;
    U32 mSkinMetaDirtyMax = 0;
    std::vector<SkinMeta>     mSkinMetaCPU;     // capacity-sized

    // Avatar slot assignment. Lookup is by avatar UUID (pointer can be recycled).
    // RECYCLED (#222): an avatar gone from the object list for kReapFrames frames
    // gives back its joint-table slot and every skin entry keyed to it; freed ids wait
    // kQuarantineFrames before reuse (instance rows of a dying linkset, frames in
    // flight). Was monotonic: a session's avatars + respawning animesh exhausted the
    // 256 slots and every newcomer's rigged mesh went invisible.
    std::map<LLUUID, U32> mAvatarSlot;
    U32 assignAvatarSlot(LLVOAvatar* avatar); // assumes lock held; returns INVALID on overflow
    static constexpr U32 kReapFrames = 300;
    static constexpr U32 kQuarantineFrames = 300;
    U32 mReapFrame = 0;
    std::map<LLUUID, U32> mAvatarDeadSince;               // avatar id -> first frame seen gone
    std::vector<std::pair<U32, U32>> mFreeAvatarSlots;    // (slot, frame freed)
    std::vector<std::pair<U32, U32>> mFreeSkinIds;        // (skin id, frame freed)
    std::set<U32> mFlexiPseudoSlots;                      // pseudo avatars: never reaped
    std::map<LLUUID, U32> mFlexiDeadSince;                // flexi object id -> first frame seen gone
    U32 takeFreeSkinId();                                 // INVALID if none past quarantine
    void reapLocked();                                    // assumes lock held
    void freeSkinLocked(U32 id);                          // zero + queue for reuse
    // Per skin id (capacity-sized): instance rows referencing it, and the mReapFrame of
    // the last getOrCreate that named it.
    std::vector<U32> mRefs;
    std::vector<U32> mTouch;
    // Stats (VKIngestDiag / section log).
    U32 mReapedAvatars = 0, mReapedSkins = 0, mReapedFlexi = 0;
    U32 mReapedUnused = 0, mTrimmed = 0;

    // Composite key: (avatar*, mesh_id). Two avatars wearing the
    // same mesh need separate palettes -- their joint poses produce
    // different matrices. mesh_id (NOT mHash) is the canonical asset
    // identifier; LLMeshSkinInfo objects with the same mHash but
    // different mesh_ids can have different mBindShapeMatrix values,
    // so keying on hash aliases them and the wrong bind_shape gets
    // used.
    // Keyed by avatar UUID, not LLVOAvatar*: a pointer is recycled when an avatar
    // leaves and another is allocated at the same address, and the newcomer's meshes
    // then matched the departed avatar's entries -- inheriting its avatar_id and joint
    // slot, so those pieces skinned to someone else's (or a frozen) skeleton.
    struct Key
    {
        LLUUID    avatar_id;
        LLUUID    mesh_id;
        bool operator<(const Key& o) const
        {
            if (avatar_id != o.avatar_id) return avatar_id < o.avatar_id;
            return mesh_id < o.mesh_id;
        }
    };

    std::vector<SkinBlock> mBlocks;          // CPU mirror, capacity-sized
    std::vector<Entry>     mEntries;         // capacity-sized, parallel to mBlocks
    std::map<Key, U32>     mKeyToId;         // (avatar, skin hash) -> skin_id

    // Flexi chains: object -> skin_id; skin_id -> (joint-table slot, first joint).
    std::map<LLUUID, U32> mFlexiSkin;
    struct FlexiChain { U32 slot; U32 base; };
    std::map<U32, FlexiChain> mFlexiChains;
    std::vector<std::pair<FlexiChain, U32>> mFreeFlexiChains;   // (chain, frame freed)
    U32 mFlexiSlot = 0xFFFFFFFFu;   // pseudo slot being filled
    U32 mFlexiNext = 0;             // chains used in it

    mutable std::mutex mMutex;
};
