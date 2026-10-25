/**
 * @file llcleananimstore.h
 * @brief VK GPU avatar animation: CPU half. Decides per avatar whether its pose is
 *        evaluated on the GPU (clean_anim.comp), collects the motion controller's
 *        exported blend (per joint: the ordered joint states LL's pose blender would
 *        blend -- keyframe states by reference into an uploaded keyframe arena,
 *        procedural/constraint states by value), the local writes made by non-animation
 *        code, and the avatar root, and packs them into one per-frame word buffer. The
 *        GPU keeps each avatar's local pose persistent (LL's blend starts from the
 *        joint's current local transform) and writes the joint world matrix table the
 *        skin palette pass and the rigid-attachment draw read. The evaluated local pose
 *        is read back (a few frames late) into LLJoint for the CPU's own consumers.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * $/LicenseInfo$
 */

#pragma once

#include "llpose.h"
#include "lluuid.h"
#include "llkeyframemotion.h"

#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class LLVOAvatar;
class LLJoint;
class LLJointState;

class LLCleanAnimStore
{
public:
    static LLCleanAnimStore& instance();
    // False before first use and after static destruction (avatars destroyed during
    // process exit must not resurrect or touch a dead store).
    static bool instanceExists() { return sAlive; }
    static inline bool sAlive = false;
    ~LLCleanAnimStore();

    static constexpr U32 MAX_JOINTS = 216;      // == LLCleanSkinStore::MAX_AVATAR_JOINTS
    // Entry words 22..31: ATTACHED-ANIMESH FOLLOW (#attach-lag). [22] wearer joint-table
    // slot (0xFFFFFFFF = none: root from words 8..18), [23] wearer joint index (the
    // attachment point), [24..26] root position and [28..31] rotation LOCAL to that
    // joint (LLControlAvatar::matchVolumeTransform's obj_pos / obj_rot). Follow entries
    // are packed LAST (from header word 6) and run in a second dispatch after the
    // wearers': clean_anim.comp reads the wearer's evaluated joint from JointsOut.
    static constexpr U32 ENTRY_WORDS = 32;      // == clean_anim.comp ENTRY_WORDS
    static constexpr U32 RB_RECORD_FLOATS = 20; // == clean_anim.comp readback record (5 vec4)
    static constexpr U32 HEADER_WORDS = 8;      // == clean_anim.comp HDR_ENTRY_BASE
    static constexpr U32 RING = 3;              // readback ring depth

    // ---- main thread, LLVOAvatar::updateCharacter ------------------------------------
    // Decide this avatar's mode for this frame (0 CPU, 1 shadow/compare, 2 GPU) and route
    // its motion controller's pose export here. Call before the motion update.
    void beginAvatar(LLVOAvatar* av);
    // The avatar object is dying (markDead / destructor): drop its state so a new
    // avatar allocated at the same address -- often the SAME agent re-rezzing after
    // leaving and re-entering view -- never inherits cached LLJoint pointers into the
    // freed skeleton (crash in refreshWorld -> updateWorldMatrixParent).
    void forgetAvatar(LLVOAvatar* av);
    // "Refresh Animations" (#297) rebuilt this avatar's motions: the purged motion and
    // joint-state objects may be reallocated at the same addresses, so the cached
    // incremental export (#257, validated by those pointers) is dropped and re-captured
    // from the next full export. The keyframe arena needs nothing: it is keyed by
    // JointMotionList, which is immutable; re-fetched data is a new list (mGpuBase = -1)
    // and uploads on first use. The GPU local pose is NOT re-seeded (the seed reads CPU
    // joint transforms, stale under the sparse readback for joints nobody requested).
    void refreshAvatar(LLVOAvatar* av);
    // #631 diagnostic: this avatar's GPU animation state in one short string (mode,
    // joint-table slot, seeded / bound, frames since its last motion update and last
    // exported blend, cached incremental export).
    // *stale: GPU mode but not seen by a motion update for 600+ frames (buildFrame then
    // stops evaluating it and its joint table row copies the CPU joints through).
    std::string describeAvatar(const LLVOAvatar* av, bool* stale = nullptr) const;
    // True when this frame's pose is GPU-evaluated (the CPU skips its hierarchy walk).
    bool gpuDriven(const LLVOAvatar* av) const;
    // GPU animation is enabled (RenderVKGpuAnim, Vulkan): the frame-skip LOD is off.
    static bool enabled();

    // ---- render thread (same thread), LLCleanRendererVK::buildRiggedFrame ------------
    // Pack this frame's GPU program for the joint-table slots (id, slot). Marks GPU slots
    // in the skin store (it then skips their CPU joint copy). Returns the entry count.
    U32  buildFrame(const std::vector<std::pair<LLUUID, U32>>& slots);
    // Write the SPARSE readback of the frame that used ring slot `ring` into the avatars'
    // LLJoints: per requested joint a record of RB_RECORD_FLOATS (local pos, rot, scale,
    // root-relative world pos, rot). Call BEFORE buildFrame reuses that ring slot.
    void consumeReadback(U32 ring, const F32* records, size_t floats);
    // Main thread, LLVOAvatar::updateCharacter (in place of the skipped
    // updateWorldMatrixChildren): recompose the always-read joints (occupied attachment
    // points -- their LLXform children read the cached world directly --, head, pelvis,
    // eyes, hands) against the live root.
    void refreshWorld(LLVOAvatar* av);
    // Compare (RenderVKAnimCompare): diff the GPU world matrices read back for the
    // compared slots against the CPU reference captured in buildFrame.
    void checkCompare(const F32* gpu_rows, size_t floats);

    // Outputs of buildFrame (valid until the next buildFrame).
    const std::vector<U32>& frameWords() const { return mWords; }
    U32  readbackCount() const { return mReadbackCount; }
    U32  ringIndex() const { return mRing; }
    U32  nextRing() const { return mFrame % RING; }     // the ring slot buildFrame will use
    // First follow entry (attached animesh rooted on its wearer's GPU-evaluated
    // attachment joint): entries [0, followFirst) run in dispatch 1, [followFirst,
    // entry count) in dispatch 2 after a barrier. == entry count when there are none.
    U32  followFirst() const { return mFollowFirst; }
    // Compare request for this frame: slots whose GPU rows to read back (<= 4).
    const std::vector<U32>& compareSlots() const { return mCmpSlotsThisFrame; }
    // REDUCED-TIER SKIN CACHE (#31): true when the last buildFrame PROVED this slot's
    // skeleton pose relative to its root unchanged -- GPU-evaluated, no blend exported
    // (motion update skipped: a held frame), no base write, no (re)seed / topology
    // change, same root scale. The renderer then reuses the slot's cached pelvis-local
    // skinned vertices. Anything else (CPU-path slots, unknown slots) is dirty.
    bool slotPoseClean(U32 slot) const { return slot < mSlotClean.size() && mSlotClean[slot] != 0; }
    // RT rigged refit gate (LLCleanRTVK, RenderVKRigRefitGate): slotPoseClean AND the
    // root's world position / rotation identical to the previous buildFrame (which must
    // be the immediately preceding one) -> the slot's skinned WORLD positions are
    // unchanged since last frame (skin meta changes are the caller's to check).
    bool slotWorldClean(U32 slot) const { return slot < mSlotWorldClean.size() && mSlotWorldClean[slot] != 0; }

    // Topology ([slot*216 + j] = parent | depth << 16) and the keyframe arena: CPU
    // mirrors + dirty element runs [first, last) since the last take.
    const std::vector<U32>& topology() const { return mTopo; }
    bool takeTopologyDirty() { bool d = mTopoDirty; mTopoDirty = false; return d; }
    const std::vector<U32>& motionDesc() const { return mDesc; }       // 8 words per joint motion
    const std::vector<F32>& keyTimes() const { return mKeyTimes; }
    const std::vector<F32>& keyVals() const { return mKeyVals; }       // 4 floats per key
    // Dirty element ranges [lo, hi) of each arena array (desc in uvec4 units). full=true
    // after a compaction (upload everything).
    struct ArenaDirty { U32 desc_lo = 0, desc_hi = 0, key_lo = 0, key_hi = 0; bool full = false; };
    ArenaDirty takeArenaDirty();


private:
    LLCleanAnimStore();

    struct Pending
    {
        std::vector<U32> hdr;        // 3 per joint: table index, first blend, count
        struct Blend { U32 flags; F32 weight; const void* list; U32 index; F32 time; };
        std::vector<Blend> blends;   // keyframe: list+index; value: index into values
        std::vector<F32> values;     // 12 per value state
        void clear() { hdr.clear(); blends.clear(); values.clear(); }
    };

    struct AvState : public LLPoseExportSink
    {
        LLCleanAnimStore*   store = nullptr;
        LLVOAvatar*         av = nullptr;
        LLUUID              id;
        U32                 slot = 0xFFFFFFFFu;     // joint-table slot (skin store)
        U8                  mode = 0;               // this frame
        bool                seeded = false;         // GPU local pose seeded for `slot`
        bool                compare = false;        // mode 1 frame: capture the CPU reference
        U32                 last_seen = 0;
        bool                gpu_missing = false;    // last export had blends with no GPU keyframes (log edge)
        U32                 topo_slot = 0xFFFFFFFFu;
        U32                 max_depth = 0;
        std::vector<LLJoint*> joints;               // [216], table index -> joint
        std::unordered_map<const LLJoint*, U16> index;
        std::vector<F32>    seed;                   // mode 1: pre-blend locals, 12 per joint
        Pending             pend;
        U32                 pend_frame = 0xFFFFFFFFu;
        // sparse readback
        bool                bound = false;          // joints bound (LLJoint::gpuBind)
        bool                cmp_armed = false;      // full readback ahead of a compare frame
        std::vector<U32>    last_want;              // [216] frame a joint was last read (0 = not wanted)
        std::vector<U16>    base;                   // always-read joints (named + occupied attach points)
        std::vector<LLJoint*> refresh;              // base as joints (refreshWorld)
        U32                 base_frame = 0;         // frame the base set was rebuilt
        LLVector3           last_root_scale;        // skin cache: root scale of the last frame
        LLVector3           last_root_pos;          // RT refit gate: root world pose of the
        LLQuaternion        last_root_rot;          //   frame last_root_frame
        U32                 last_root_frame = 0;
        // attached-animesh follow (RT gate): last frame's follow key (wearer slot | joint
        // << 16, 0xFFFFFFFF = none) and local offset
        U32                 follow_key = 0xFFFFFFFFu;
        U32                 follow_frame = 0;
        LLVector3           follow_pos;
        LLQuaternion        follow_rot;

        // ---- incremental export (#257, RenderVKAnimIncremental) ----------------------
        // The cached STRUCTURE of the last full export: every exportJoint call LL's pose
        // blender made (joint, table index, its ordered states + additive flags), and the
        // validation key it is a pure function of -- the ordered motion list handed to
        // addMotion, each motion's priority / blend type, and every pose state's pointer,
        // joint, priority, usage and GPU source (list, index). Weights, keyframe times and
        // procedural values are read live from the (validated, live) states every frame.
        struct IncCall
        {
            LLJoint*      joint;
            U32           ji;                       // joint-table index, 0xFFFFFFFF = none
            S32           count;
            LLJointState* states[JSB_NUM_JOINT_STATES];
            bool          additive[JSB_NUM_JOINT_STATES];
        };
        struct IncMotion { const LLMotion* m; S32 prio; S32 blend; U32 first; U32 n; };
        struct IncState  { const LLJointState* js; const LLJoint* joint; const void* gpu_list; S32 gpu_index; U32 usage; S32 prio; };
        std::vector<IncCall>   inc_calls;
        std::vector<IncMotion> inc_motions;
        std::vector<IncState>  inc_states;
        bool                   inc_valid = false;
        // capture / verify state of the current export
        enum CaptureMode : U8 { CAP_NONE, CAP_STRUCTURE, CAP_VERIFY };
        CaptureMode            cap_mode = CAP_NONE;
        Pending*               out = nullptr;       // where exportJoint emits (pend by default)
        bool                   verifying = false;
        std::vector<IncCall>   v_calls;             // verify: the full path's calls
        Pending                v_fast, v_full;      // verify: both exports, compared
        std::chrono::steady_clock::time_point t_export;
        bool                   export_incr = false;

        void beginExport() override;
        void exportJoint(LLJoint* joint, LLJointState* const* states, const bool* additive, S32 count) override;
        void endExport() override;
        bool exportCached(LLMotion* const* motions, size_t count) override;
        bool verifyCached() override { return verifying; }
        void beginCapture(LLMotion* const* motions, size_t count, bool verify) override;
        void endCapture() override;

        U32  jointIndex(const LLJoint* joint) const;
        void emit(Pending& dst, U32 ji, LLJointState* const* states, const bool* additive, S32 count);
        void verifyCompare();
    };

    static void appendPending(Pending& dst, const Pending& src);
    void buildTopology(AvState& st, U32 slot);
    S32  ensureUploaded(LLKeyframeMotion::JointMotionList* list);
    static void onListDestroyed(LLKeyframeMotion::JointMotionList* list);
    void compactArena();
    void captureLocals(AvState& st, std::vector<F32>& out) const;
    void bindJoints(AvState& st);
    void unbindJoints(AvState& st);
    void rebuildBase(AvState& st);

    std::unordered_map<const LLVOAvatar*, std::unique_ptr<AvState>> mAvatars;
    U32 mFrame = 1;

    // frame program
    std::vector<U32> mWords;
    std::vector<U8>  mSlotClean;    // [slot] pose unchanged this frame (slotPoseClean)
    std::vector<U8>  mSlotWorldClean;   // [slot] + root world pose unchanged (slotWorldClean)
    U32 mEntryCount = 0;
    U32 mFollowFirst = 0;
    U32 mReadbackCount = 0;
    U32 mRing = 0;
    struct RbRecord { LLVOAvatar* av; LLUUID id; U32 first; U32 count; U32 req_off; U32 frame; };
    std::vector<RbRecord> mRbRecords[RING];
    std::vector<U16>      mRbReq[RING];     // requested joint indices of each record run

    // topology
    std::vector<U32> mTopo;
    bool mTopoDirty = true;

    // keyframe arena
    // last_used: mFrame of the upload or of the last (64-frame sampled) use. Lists unused
    // for kListEvictFrames leave the arena (garbage until compaction; re-uploaded on
    // demand) -- LL's keyframe cache never evicts in normal play, so without this every
    // animation a GPU-animated avatar ever played stayed in the arena for the session.
    struct ListRange { U32 desc_first; U32 desc_count; U32 key_first; U32 key_count; U32 last_used; };
    static constexpr U32 kListEvictFrames = 3600;
    std::unordered_map<const LLKeyframeMotion::JointMotionList*, ListRange> mLists;
    std::unordered_set<const void*> mDeadLists;     // destroyed since the last buildFrame
    std::vector<U32> mDesc;
    std::vector<F32> mKeyTimes;
    std::vector<F32> mKeyVals;
    U32 mDescGarbage = 0, mKeyGarbage = 0;
    ArenaDirty mArenaDirty;

    // compare
    bool mCmpPending = false;
    U32  mCmpFrame = 0;
    std::vector<U32> mCmpSlotsThisFrame;
    struct CmpRef { LLUUID id; U32 slot; std::vector<F32> cpu; std::vector<const LLJoint*> joints; std::string anims; };
    std::vector<CmpRef> mCmpRefs;

    U32 mStatGpu = 0;
    // incremental export (#257): running totals for the 600-frame log, verify throttle
    U64 mSumExportFull = 0, mSumExportIncr = 0, mSumVerify = 0, mSumVerifyBad = 0;
    U64 mSumExportUs = 0;
    F64 mVerifyLogTime = 0.0;
    static bool incrementalEnabled();
    static bool verifyEnabled();
    // sparse readback stats (per frame + running totals for the throttled log)
    U32 mStatWantAdds = 0, mStatWantExpires = 0;
    U64 mSumRbJoints = 0, mSumRbAvatars = 0, mSumWantAdds = 0, mSumWantExpires = 0;
    U32 mCmpArmFrame = 0, mCmpArmedCount = 0;
};
