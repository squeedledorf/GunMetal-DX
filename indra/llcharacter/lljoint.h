/**
 * @file lljoint.h
 * @brief Implementation of LLJoint class.
 *
 * $LicenseInfo:firstyear=2001&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#ifndef LL_LLJOINT_H
#define LL_LLJOINT_H

//-----------------------------------------------------------------------------
// Header Files
//-----------------------------------------------------------------------------
#include <string>
#include <list>

#include "v3math.h"
#include "v4math.h"
#include "m4math.h"
#include "llquaternion.h"
#include "xform.h"
#include "llmatrix4a.h"

constexpr S32 LL_CHARACTER_MAX_JOINTS_PER_MESH = 15;
// Need to set this to count of animate-able joints,
// currently = #bones + #collision_volumes + #attachments + 2,
// rounded to next multiple of 4.
constexpr U32 LL_CHARACTER_MAX_ANIMATED_JOINTS = 216; // must be divisible by 4!
constexpr U32 LL_MAX_JOINTS_PER_MESH_OBJECT = 110;

// These should be higher than the joint_num of any
// other joint, to avoid conflicts in updateMotionsByType()
constexpr U32 LL_HAND_JOINT_NUM = (LL_CHARACTER_MAX_ANIMATED_JOINTS-1);
constexpr U32 LL_FACE_JOINT_NUM = (LL_CHARACTER_MAX_ANIMATED_JOINTS-2);
constexpr S32 LL_CHARACTER_MAX_PRIORITY = 7;
constexpr F32 LL_MAX_PELVIS_OFFSET = 5.f;

constexpr F32 LL_JOINT_TRESHOLD_POS_OFFSET = 0.0001f; //0.1 mm

class LLVector3OverrideMap
{
public:
    LLVector3OverrideMap() {}
    bool findActiveOverride(LLUUID& mesh_id, LLVector3& pos) const;
    void showJointVector3Overrides(std::ostringstream& os) const;
    U32 count() const;
    void add(const LLUUID& mesh_id, const LLVector3& pos);
    bool remove(const LLUUID& mesh_id);
    void clear();

    typedef std::map<LLUUID,LLVector3> map_type;
    const map_type& getMap() const { return m_map; }
private:
    map_type m_map;
};

inline bool operator==(const LLVector3OverrideMap& a, const LLVector3OverrideMap& b)
{
    return a.getMap() == b.getMap();
}

inline bool operator!=(const LLVector3OverrideMap& a, const LLVector3OverrideMap& b)
{
    return !(a == b);
}

//-----------------------------------------------------------------------------
// class LLJoint
//-----------------------------------------------------------------------------
LL_ALIGN_PREFIX(16)
class LLJoint
{
    LL_ALIGN_NEW
public:
    // priority levels, from highest to lowest
    enum JointPriority
    {
        USE_MOTION_PRIORITY = -1,
        LOW_PRIORITY = 0,
        MEDIUM_PRIORITY,
        HIGH_PRIORITY,
        HIGHER_PRIORITY,
        HIGHEST_PRIORITY,
        ADDITIVE_PRIORITY = LL_CHARACTER_MAX_PRIORITY
    };

    enum DirtyFlags
    {
        MATRIX_DIRTY = 0x1 << 0,
        ROTATION_DIRTY = 0x1 << 1,
        POSITION_DIRTY = 0x1 << 2,
        ALL_DIRTY = 0x7,
        // VK GPU animation: a non-animation local write on this joint or an ancestor
        // since the last GPU readback covered it -- the GPU-supplied root-relative
        // world transform is stale, recompute through the parent chain instead.
        GPU_EXT_DIRTY = 0x1 << 3
    };
public:
    enum SupportCategory
    {
        SUPPORT_BASE,
        SUPPORT_EXTENDED
    };
protected:
    // explicit transformation members
    LL_ALIGN_16(LLMatrix4a          mWorldMatrix);
    LLXformMatrix       mXform;

    std::string mName;

    SupportCategory mSupport;

    // parent joint
    LLJoint *mParent;

    LLVector3       mDefaultPosition;
    LLVector3       mDefaultScale;

public:
    U32             mDirtyFlags;
    bool            mUpdateXform;

    // describes the skin binding pose
    LLVector3       mSkinOffset;

    // Endpoint of the bone, if applicable. This is only relevant for
    // external programs like Blender, and for diagnostic display.
    LLVector3       mEnd;

    S32             mJointNum;

    // child joints
    typedef std::vector<LLJoint*> joints_t;
    joints_t mChildren;

    // debug statics
    static S32      sNumTouches;
    static S32      sNumUpdates;
    typedef std::set<std::string> debug_joint_name_t;
    static debug_joint_name_t s_debugJointNames;
    static void setDebugJointNames(const debug_joint_name_t& names);
    static void setDebugJointNames(const std::string& names_string);

    // Position overrides
    LLVector3OverrideMap m_attachmentPosOverrides;
    LLVector3 m_posBeforeOverrides;

    // Scale overrides
    LLVector3OverrideMap m_attachmentScaleOverrides;
    LLVector3 m_scaleBeforeOverrides;

    void updatePos(const std::string& av_info);
    void updateScale(const std::string& av_info);

    // VK GPU animation. A GPU-animated avatar keeps its animated local pose in a GPU
    // buffer (LL's blend starts from the joint's current local transform, so the pose
    // persists there frame to frame). Any local write that does NOT come from the pose
    // blender -- visual params, joint overrides, skeleton resets, avatar physics -- must
    // reach that buffer: it is recorded here (1 = pos, 2 = rot, 4 = scale, the
    // LLJointState usage bits) with the frame it happened on, and the uploader clears it.
    U8              mGpuExtWrite = 0;

    // ---- ANIMATION HAND-BACK (#207) ----------------------------------------------
    // Shape and avatar-physics volume morphs move a joint INCREMENTALLY
    // (setPosition(getPosition() + delta)), and an animation that drives the joint's
    // position overwrites it. As in every viewer the joint stays where the animation
    // left it (sticky); but under VK the CPU copy of an animated joint was stale while
    // physics kept writing it, and was uploaded as the new pose on release (the bone
    // drifted to an unrelated spot). Now morph deltas are held while an animation owns
    // the position (no stale writes), the CPU copy tracks the GPU pose, and on release
    // the held deltas are applied on top of the animated position. Unanimated joints
    // behave exactly as LL.
    void morphPosDelta(const LLVector3& delta);
    // The blend / GPU export, BEFORE this evaluation's result is written: a
    // non-additive animation drives this joint's position.
    void notePosAnimated();
    // After an evaluation: hand the joint back if the animation let go.
    void endPosEvaluation();
    bool            mPosAnimated = false;       // this evaluation
    bool            mPosAnimatedPrev = false;   // the previous evaluation
    LLVector3       mAnimMorphHeld;             // morph deltas held while animated
    U32             mGpuExtFrame = 0;
    static bool     sAnimApply;     // set while the pose blender applies (not external)
    static U32      sFrameCounter;  // the viewer's frame number, set once per frame
    // Write an animated local pose (GPU readback) without flagging it external. Marks
    // this joint's world transform dirty WITHOUT the recursive touch -- the caller
    // writes every joint of the skeleton, so each child gets its own flags.
    void setLocalFromAnim(const LLVector3& pos, const LLQuaternion& rot, const LLVector3& scale);

    // SPARSE GPU READBACK (LLCleanAnimStore). While the avatar is GPU-animated, each of
    // its joint-table joints is BOUND: mGpuRoot = the avatar's root joint, and the want /
    // ext masks point at per-avatar bit arrays (bit mGpuBit). Reading the joint (local or
    // world getters) sets its want bit, so only joints something actually reads come
    // back from the GPU; a non-animation write sets its ext bit (base-write upload
    // without scanning the skeleton). A joint that came back carries its world transform
    // RELATIVE TO THE ROOT as the GPU computed it; world getters compose it with the LIVE
    // root (no root-motion lag, no parent-chain walk). GPU_EXT_DIRTY (propagated by
    // touch from non-animation writes) falls back to LL's chain.
    LLJoint*        mGpuRoot = nullptr;
    U32*            mGpuWantMask = nullptr;
    U32*            mGpuExtMask = nullptr;
    U16             mGpuBit = 0;
    bool            mGpuRelValid = false;
    U32             mGpuExtDirtyFrame = 0;
    LLVector3       mGpuRelPos;
    LLQuaternion    mGpuRelRot;
    U32             mGpuRelFrame = 0;   // anim frame (LLCleanAnimStore) the readback came from
    static S32      sGpuWantSuppress;   // > 0: reads don't request joints (bulk estimates)
    void gpuBind(LLJoint* root, U32* want_mask, U32* ext_mask, U16 bit)
    { mGpuRoot = root; mGpuWantMask = want_mask; mGpuExtMask = ext_mask; mGpuBit = bit; mGpuRelValid = false; }
    void gpuUnbind()
    { mGpuRoot = nullptr; mGpuWantMask = nullptr; mGpuExtMask = nullptr; mGpuRelValid = false;
      mDirtyFlags = (mDirtyFlags & ~(U32)GPU_EXT_DIRTY) | ALL_DIRTY; }
    // Readback of GPU frame `frame`: root-relative world transform, plus the local pose
    // when `write_local` (not superseded by a newer non-animation write).
    void setFromGpu(const F32* local12, const F32* rel8, bool write_local, U32 frame);
    inline void gpuWant()
    {
        if (mGpuWantMask && sGpuWantSuppress <= 0) mGpuWantMask[mGpuBit >> 5] |= 1u << (mGpuBit & 31u);
    }
    inline bool gpuRelUsable() const
    {
        return mGpuRoot && mGpuRelValid && !(mDirtyFlags & GPU_EXT_DIRTY);
    }
    void gpuComposeWorld();
    // LIVE WORLD MATRIX (#610). LL's getters trust the dirty flags, and on a GPU-animated
    // avatar those don't track root motion: the root's touch stops at a base joint the
    // readback already flagged, and nothing re-dirties a joint outside the readback set
    // once it has been composed -- its cached world transform stays wherever it was last
    // composed (where the avatar stood then). This composes against the LIVE root
    // regardless of the flags: the GPU root-relative transform when this joint has a
    // usable one, else its parent's live matrix * its local pose (LL's chain). Each
    // joint is composed once per stamp (nextLiveStamp: one per bulk read). No readback
    // request (callers that want the exact pose next time call gpuWant()).
    U32             mLiveStamp = 0;
    static U32      sLiveStamp;
    static U32 nextLiveStamp() { if (++sLiveStamp == 0) sLiveStamp = 1; return sLiveStamp; }
    const LLMatrix4& getWorldMatrixLive(U32 stamp);

public:
    LLJoint();

    // Note: these joint_num constructors are a bad idea because there
    // are only a couple of places in the code where it is useful to
    // have a joint num for a joint (for joints that are used in
    // animations), and including them as part of the constructor then
    // forces us to maintain an alternate path through the entire
    // large-ish class hierarchy of joint types. The only reason they
    // are still here now is to avoid breaking the baking service
    // (appearanceutility) builds; these constructors are not used in
    // the viewer.  Once the appearance utility is updated to remove
    // these joint num references, which it shouldn't ever need, from
    // its own classes, we can also remove all the joint_num
    // constructors from LLJoint, LLViewerJoint, LLAvatarJoint, and
    // createAvatarJoint.
    LLJoint(S32 joint_num);

    // *TODO: Only used for LLVOAvatarSelf::mScreenp.  *DOES NOT INITIALIZE mResetAfterRestoreOldXform*
    LLJoint( const std::string &name, LLJoint *parent=NULL );
    virtual ~LLJoint();

private:
    void init();

public:
    // set name and parent
    void setup( const std::string &name, LLJoint *parent=NULL );

    void touch(U32 flags = ALL_DIRTY);

    // get/set name
    const std::string& getName() const { return mName; }
    void setName( const std::string &name ) { mName = name; }

    // joint num
    S32 getJointNum() const { return mJointNum; }
    void setJointNum(S32 joint_num);

    // get/set support
    SupportCategory getSupport() const { return mSupport; }
    void setSupport( const SupportCategory& support) { mSupport = support; }
    void setSupport( const std::string& support_string);

    // get/set end point
    void setEnd( const LLVector3& end) { mEnd = end; }
    const LLVector3& getEnd() const { return mEnd; }

    // getParent
    LLJoint *getParent() { return mParent; }

    // getRoot
    LLJoint *getRoot();

    // search for child joints by name
    LLJoint* findJoint(std::string_view name);

    // add/remove children
    void addChild( LLJoint *joint );
    void removeChild( LLJoint *joint );
    void removeAllChildren();

    // get/set local position
    const LLVector3& getPosition();
    void setPosition( const LLVector3& pos, bool apply_attachment_overrides = false );

    // Tracks the default position defined by the skeleton
    void setDefaultPosition( const LLVector3& pos );
    const LLVector3& getDefaultPosition() const;

    // Tracks the default scale defined by the skeleton
    void setDefaultScale( const LLVector3& scale );
    const LLVector3& getDefaultScale() const;

    // get/set world position
    LLVector3 getWorldPosition();
    LLVector3 getLastWorldPosition();
    void setWorldPosition( const LLVector3& pos );

    // get/set local rotation
    const LLQuaternion& getRotation();
    void setRotation( const LLQuaternion& rot );

    // get/set world rotation
    LLQuaternion getWorldRotation();
    LLQuaternion getLastWorldRotation();
    void setWorldRotation( const LLQuaternion& rot );

    // get/set local scale
    const LLVector3& getScale();
    void setScale( const LLVector3& scale, bool apply_attachment_overrides = false );

    // get/set world matrix
    const LLMatrix4 &getWorldMatrix();
    void setWorldMatrix( const LLMatrix4& mat );

    const LLMatrix4a& getWorldMatrix4a();

    void updateWorldMatrixChildren();
    void updateWorldMatrixParent();

    void updateWorldPRSParent();

    void updateWorldMatrix();

    // get/set skin offset
    const LLVector3 &getSkinOffset();
    void setSkinOffset( const LLVector3 &offset);

    LLXformMatrix   *getXform() { return &mXform; }

    void clampRotation(LLQuaternion old_rot, LLQuaternion new_rot);

    virtual bool isAnimatable() const { return true; }

    void addAttachmentPosOverride( const LLVector3& pos, const LLUUID& mesh_id, const std::string& av_info, bool& active_override_changed );
    void removeAttachmentPosOverride( const LLUUID& mesh_id, const std::string& av_info, bool& active_override_changed );
    bool hasAttachmentPosOverride( LLVector3& pos, LLUUID& mesh_id ) const;
    void clearAttachmentPosOverrides();
    void showAttachmentPosOverrides(const std::string& av_info) const;

    void addAttachmentScaleOverride( const LLVector3& scale, const LLUUID& mesh_id, const std::string& av_info );
    void removeAttachmentScaleOverride( const LLUUID& mesh_id, const std::string& av_info );
    bool hasAttachmentScaleOverride( LLVector3& scale, LLUUID& mesh_id ) const;
    void clearAttachmentScaleOverrides();
    void showAttachmentScaleOverrides(const std::string& av_info) const;

    void getAllAttachmentPosOverrides(S32& num_pos_overrides,
                                      std::set<LLVector3>& distinct_pos_overrides) const;
    void getAllAttachmentScaleOverrides(S32& num_scale_overrides,
                                        std::set<LLVector3>& distinct_scale_overrides) const;

    // These are used in checks of whether a pos/scale override is considered significant.
    bool aboveJointPosThreshold(const LLVector3& pos) const;
    bool aboveJointScaleThreshold(const LLVector3& scale) const;
} LL_ALIGN_POSTFIX(16);
#endif // LL_LLJOINT_H

