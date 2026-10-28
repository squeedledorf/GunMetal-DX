/**
 * @file llcharacter.h
 * @brief Implementation of LLCharacter class.
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

#ifndef LL_LLCHARACTER_H
#define LL_LLCHARACTER_H

//-----------------------------------------------------------------------------
// Header Files
//-----------------------------------------------------------------------------
#include <string>
#include <string_view>

#include "lljoint.h"
#include "llmotioncontroller.h"
#include "llvisualparam.h"
#include "llstringtable.h"
#include "llpointer.h"
#include "llrefcount.h"

class LLPolyMesh;

class LLPauseRequestHandle : public LLThreadSafeRefCount
{
public:
    LLPauseRequestHandle() {};
};

typedef LLPointer<LLPauseRequestHandle> LLAnimPauseRequest;

//-----------------------------------------------------------------------------
// class LLCharacter
//-----------------------------------------------------------------------------
class LLCharacter
{
public:
    // Constructor
    LLCharacter();

    // Destructor
    virtual ~LLCharacter();

    //-------------------------------------------------------------------------
    // LLCharacter Interface
    // These functions must be implemented by subclasses.
    //-------------------------------------------------------------------------

    // get the prefix to be used to lookup motion data files
    // from the viewer data directory
    virtual const char *getAnimationPrefix() = 0;

    // get the root joint of the character
    virtual LLJoint *getRootJoint() = 0;

    // get the specified joint
    // default implementation does recursive search,
    // subclasses may optimize/cache results.
    virtual LLJoint* getJoint(std::string_view name);

    // get the position of the character
    virtual LLVector3 getCharacterPosition() = 0;

    // get the rotation of the character
    virtual LLQuaternion getCharacterRotation() = 0;

    // get the velocity of the character
    virtual LLVector3 getCharacterVelocity() = 0;

    // get the angular velocity of the character
    virtual LLVector3 getCharacterAngularVelocity() = 0;

    // get the height & normal of the ground under a point
    virtual void getGround(const LLVector3 &inPos, LLVector3 &outPos, LLVector3 &outNorm) = 0;

    // skeleton joint accessor to support joint subclasses
    virtual LLJoint *getCharacterJoint( U32 i ) = 0;

    // get the physics time dilation for the simulator
    virtual F32 getTimeDilation() = 0;

    // gets current pixel area of this character
    virtual F32 getPixelArea() const = 0;

    // gets the head mesh of the character
    virtual LLPolyMesh* getHeadMesh() = 0;

    // gets the upper body mesh of the character
    virtual LLPolyMesh* getUpperBodyMesh() = 0;

    // gets global coordinates from agent local coordinates
    virtual LLVector3d  getPosGlobalFromAgent(const LLVector3 &position) = 0;

    // gets agent local coordinates from global coordinates
    virtual LLVector3   getPosAgentFromGlobal(const LLVector3d &position) = 0;

    // updates all visual parameters for this character
    virtual void updateVisualParams();

    virtual void addDebugText( const std::string& text ) = 0;

    virtual std::string getDebugName() const { return getID().asString(); }

    virtual const LLUUID&   getID() const = 0;
    //-------------------------------------------------------------------------
    // End Interface
    //-------------------------------------------------------------------------
    // registers a motion with the character
    // returns true if successfull
    bool registerMotion( const LLUUID& id, LLMotionConstructor create );

    void removeMotion( const LLUUID& id );

    // removes ALL instances of a motion -- the canonical one AND any deprecated
    // duplicates still easing out -- for when the motion's backing keyframe
    // data is about to be destroyed (see LLMotionController::purgeMotionInstances)
    void purgeMotionInstances( const LLUUID& id );

    // returns an instance of a registered motion, creating one if necessary
    LLMotion* createMotion( const LLUUID &id );

    // returns an existing instance of a registered motion
    LLMotion* findMotion( const LLUUID &id );

    // start a motion
    // returns true if successful, false if an error occurred
    virtual bool startMotion( const LLUUID& id, F32 start_offset = 0.f);

    // stop a motion
    virtual bool stopMotion( const LLUUID& id, bool stop_immediate = false );

    // is this motion active?
    bool isMotionActive( const LLUUID& id );

    // Event handler for motion deactivation.
    // Called when a motion has completely stopped and has been deactivated.
    // Subclasses may optionally override this.
    // The default implementation does nothing.
    virtual void requestStopMotion( LLMotion* motion );

    // periodic update function, steps the motion controller
    enum e_update_t { NORMAL_UPDATE, HIDDEN_UPDATE, FORCE_UPDATE };
    void updateMotions(e_update_t update_type);

    LLAnimPauseRequest requestPause();
    bool areAnimationsPaused() const { return mMotionController.isPaused(); }
    void setAnimTimeFactor(F32 factor) { mMotionController.setTimeFactor(factor); }

    LLMotionController& getMotionController() { return mMotionController; }

    // Releases all motion instances which should result in
    // no cached references to character joint data.  This is
    // useful if a character wants to rebuild it's skeleton.
    virtual void flushAllMotions();

    // Flush only wipes active animations.
    virtual void deactivateAllMotions();

    // dumps information for debugging
    virtual void dumpCharacter( LLJoint *joint = NULL );

    virtual F32 getPreferredPelvisHeight() { return mPreferredPelvisHeight; }

    virtual LLVector3 getVolumePos(S32 joint_index, LLVector3& volume_offset) { return LLVector3::zero; }

    virtual LLJoint* findCollisionVolume(S32 volume_id) { return NULL; }

    virtual S32 getCollisionVolumeID(std::string &name) { return -1; }

    // Fixed animation-data keys (#327) live in member slots: "Hand Pose" / "Hand Pose
    // Priority" are set by every keyframe motion and cleared by the hand motion of every
    // avatar every frame, which cost a std::map node plus a heap string (past the SSO
    // limit) each time. The string API below routes these names to the same slots, so
    // callers of either form -- and LLKeyframeMotionParam's asset-defined names -- see
    // one value. remove == set to null (getAnimationData returned null for both before).
    enum EAnimData
    {
        ANIM_DATA_HAND_POSE = 0,        // "Hand Pose"
        ANIM_DATA_HAND_POSE_PRIORITY,   // "Hand Pose Priority"
        ANIM_DATA_LOOK_AT_POINT,        // "LookAtPoint"
        ANIM_DATA_POINT_AT_POINT,       // "PointAtPoint"
        ANIM_DATA_WALK_SPEED,           // "Walk Speed"
        ANIM_DATA_SPEED,                // "Speed"
        ANIM_DATA_DOWN_FOOT,            // "Down Foot"
        ANIM_DATA_LOOK_AT_ROLL,         // "LookAtRoll" (#1121): F32 radians, the head's tilt about its look axis
        ANIM_DATA_COUNT
    };
    void setAnimationData(EAnimData slot, void* data) { mAnimDataSlots[slot] = data; }
    void* getAnimationData(EAnimData slot) const { return mAnimDataSlots[slot]; }
    void removeAnimationData(EAnimData slot) { mAnimDataSlots[slot] = nullptr; }

    // string_view + heterogeneous lookup: the by-value std::string signatures heap-
    // allocated on every call for names past the SSO limit. Names of the slots above go
    // to the slots; any other name to the map.
    void setAnimationData(std::string_view name, void *data);

    void *getAnimationData(std::string_view name);

    void removeAnimationData(std::string_view name);

    void addVisualParam(LLVisualParam *param);
    void addSharedVisualParam(LLVisualParam *param);

    virtual bool setVisualParamWeight(const LLVisualParam *which_param, F32 weight);
    virtual bool setVisualParamWeight(const char* param_name, F32 weight);
    virtual bool setVisualParamWeight(S32 index, F32 weight);

    // get visual param weight by param or name
    F32 getVisualParamWeight(LLVisualParam *distortion);
    F32 getVisualParamWeight(const char* param_name);
    F32 getVisualParamWeight(S32 index);

    // set all morph weights to defaults
    void clearVisualParamWeights();

    // visual parameter accessors
    LLVisualParam*  getFirstVisualParam()
    {
        mCurIterator = mVisualParamIndexMap.begin();
        return getNextVisualParam();
    }
    LLVisualParam*  getNextVisualParam()
    {
        if (mCurIterator == mVisualParamIndexMap.end())
            return 0;
        return (mCurIterator++)->second;
    }

    S32 getVisualParamCountInGroup(const EVisualParamGroup group) const
    {
        S32 rtn = 0;
        for (const visual_param_index_map_t::value_type& index_pair : mVisualParamIndexMap)
        {
            if (index_pair.second->getGroup() == group)
            {
                ++rtn;
            }
        }
        return rtn;
    }

    LLVisualParam*  getVisualParam(S32 id) const
    {
        visual_param_index_map_t::const_iterator iter = mVisualParamIndexMap.find(id);
        return (iter == mVisualParamIndexMap.end()) ? 0 : iter->second;
    }
    S32 getVisualParamID(LLVisualParam *id)
    {
        for (visual_param_index_map_t::value_type& index_pair : mVisualParamIndexMap)
        {
            if (index_pair.second == id)
                return index_pair.first;
        }
        return 0;
    }
    S32             getVisualParamCount() const { return (S32)mVisualParamIndexMap.size(); }
    // Bumped by addVisualParam (the only change to the parameter maps): callers may cache
    // getVisualParam() results while it is unchanged (#329).
    U32             getVisualParamGeneration() const { return mVisualParamGen; }
    LLVisualParam*  getVisualParam(const char *name);

    void animateTweakableVisualParams(F32 delta)
    {
        for (auto& it : mVisualParamIndexMap)
        {
            if (it.second->isTweakable())
            {
                it.second->animate(delta);
            }
        }
    }

    void applyAllVisualParams(ESex avatar_sex)
    {
        for (auto& it : mVisualParamIndexMap)
        {
            it.second->apply(avatar_sex);
        }
    }

    ESex getSex() const         { return mSex; }
    void setSex( ESex sex )     { mSex = sex; }

    U32             getAppearanceSerialNum() const      { return mAppearanceSerialNum; }
    void            setAppearanceSerialNum( U32 num )   { mAppearanceSerialNum = num; }

    U32             getSkeletonSerialNum() const        { return mSkeletonSerialNum; }
    void            setSkeletonSerialNum( U32 num ) { mSkeletonSerialNum = num; }

    static std::list< LLCharacter* > sInstances;
    static bool sAllowInstancesChange ; //debug use

    virtual void    setHoverOffset(const LLVector3& hover_offset, bool send_update=true) { mHoverOffset = hover_offset; }
    const LLVector3& getHoverOffset() const { return mHoverOffset; }

    // VK GPU animation, set by the viewer before each motion update:
    //   0 = CPU (LL: sample, blend, apply),
    //   1 = shadow (CPU evaluates as LL AND exports the pose to mGpuAnimSink -- compare),
    //   2 = GPU (keyframe sampling deferred to the GPU, pose exported, NOT applied here).
    U8                  mGpuAnimMode = 0;
    LLPoseExportSink*   mGpuAnimSink = nullptr;
    // Mode 2 only: the motion controller defers LLPoseBlender::addMotion and lets the
    // sink reuse its cached export structure on stable frames (RenderVKAnimIncremental).
    bool                mGpuAnimIncremental = false;
    // Sparse GPU readback bit arrays (216 joint-table bits), owned here so the joints'
    // pointers (LLJoint::gpuBind) live exactly as long as the joints: reads set want
    // bits, non-animation writes set ext bits (LLCleanAnimStore consumes both).
    U32                 mGpuWantBits[7] = {0, 0, 0, 0, 0, 0, 0};
    U32                 mGpuExtBits[7] = {0, 0, 0, 0, 0, 0, 0};

protected:
    LLMotionController  mMotionController;

    typedef std::map<std::string, void *, std::less<>> animation_data_map_t;
    animation_data_map_t mAnimationData;
    void*               mAnimDataSlots[ANIM_DATA_COUNT] = {};

    F32                 mPreferredPelvisHeight;
    ESex                mSex;
    U32                 mAppearanceSerialNum;
    U32                 mSkeletonSerialNum;
    LLAnimPauseRequest  mPauseRequest;

private:
    // visual parameter stuff
    typedef std::map<S32, LLVisualParam *>      visual_param_index_map_t;
    typedef std::map<char *, LLVisualParam *>   visual_param_name_map_t;

    visual_param_index_map_t::iterator          mCurIterator;
    visual_param_index_map_t                    mVisualParamIndexMap;
    visual_param_name_map_t                     mVisualParamNameMap;
    // #329: mVisualParamIndexMap's values in map (id) order for updateVisualParams'
    // per-call walk; rebuilt when mVisualParamGen moves.
    std::vector<LLVisualParam*>                 mVisualParamVec;
    U32                                         mVisualParamGen = 0;
    U32                                         mVisualParamVecGen = ~0u;

    static LLStringTable sVisualParamNames;

    LLVector3 mHoverOffset;
};

#endif // LL_LLCHARACTER_H

