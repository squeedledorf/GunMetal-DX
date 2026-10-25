/**
 * @file llpose.h
 * @brief Implementation of LLPose class.
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

#ifndef LL_LLPOSE_H
#define LL_LLPOSE_H

//-----------------------------------------------------------------------------
// Header Files
//-----------------------------------------------------------------------------

#include "lljointstate.h"
#include "lljoint.h"
#include "llpointer.h"

#include <map>
#include <string>
#include <unordered_map>
#include <vector>


//-----------------------------------------------------------------------------
// class LLPose
//-----------------------------------------------------------------------------
class LLPose
{
    friend class LLPoseBlender;
protected:
    typedef std::map<std::string, LLPointer<LLJointState> > joint_map;
    typedef joint_map::iterator joint_map_iterator;
    typedef joint_map::value_type joint_map_value_type;

    joint_map                   mJointMap;
    F32                         mWeight;
    joint_map_iterator          mListIter;
    // Flat copy of mJointMap's values in map (name) order, rebuilt lazily after a
    // change: the pose blender walks every active motion's joint states for every
    // avatar every frame, and std::map iteration was a measurable crowd cost.
    std::vector<LLJointState*>  mJointVec;
    bool                        mJointVecDirty = true;
public:
    // Joint states in map order (same order as getFirst/getNextJointState).
    const std::vector<LLJointState*>& getJointStates();
    // Iterate through jointStates
    LLJointState* getFirstJointState();
    LLJointState* getNextJointState();
    LLJointState* findJointState(LLJoint *joint);
    LLJointState* findJointState(const std::string &name);
public:
    // Constructor
    LLPose() : mWeight(0.f) {}
    // Destructor
    ~LLPose();
    // add a joint state in this pose
    bool addJointState(const LLPointer<LLJointState>& jointState);
    // remove a joint state from this pose
    bool removeJointState(const LLPointer<LLJointState>& jointState);
    // removes all joint states from this pose
    bool removeAllJointStates();
    // set weight for all joint states in this pose
    void setWeight(F32 weight);
    // get weight for this pose
    F32 getWeight() const;
    // returns number of joint states stored in this pose
    S32 getNumJointStates() const;
};

const S32 JSB_NUM_JOINT_STATES = 6;

// VK GPU animation: receives each active joint blender's ordered joint states (the exact
// slot order LLJointStateBlender::blendJointStates walks -- priority descending, newer
// motions first within a priority) instead of, or in addition to, the CPU blend.
class LLMotion;
class LLPoseExportSink
{
public:
    virtual ~LLPoseExportSink() {}
    // Right before the export (motions have all updated; nothing blended yet).
    virtual void beginExport() {}
    virtual void exportJoint(LLJoint* joint, LLJointState* const* states, const bool* additive, S32 count) = 0;
    // After the export (incremental or full).
    virtual void endExport() {}

    // INCREMENTAL EXPORT (#257, LLCharacter::mGpuAnimIncremental). LLMotionController
    // defers LLPoseBlender::addMotion and hands the sink the exact ordered motion list
    // addMotion would have received this update. exportCached returns true when the sink
    // reproduced the whole export from its cached structure (the motion set, every pose
    // state's joint / priority / usage / GPU source unchanged): LLPoseBlender is not
    // touched at all. Otherwise (and when verifyCached() asks for it) the controller runs
    // addMotion for every motion and exportAndClear between beginCapture / endCapture,
    // which the sink records as its new structure (or, verify=true, as the reference the
    // cached export is compared against).
    virtual bool exportCached(LLMotion* const* motions, size_t count) { return false; }
    virtual bool verifyCached() { return false; }
    virtual void beginCapture(LLMotion* const* motions, size_t count, bool verify) {}
    virtual void endCapture() {}
};

class LLJointStateBlender
{
protected:
    LLPointer<LLJointState> mJointStates[JSB_NUM_JOINT_STATES];
    S32             mPriorities[JSB_NUM_JOINT_STATES];
    bool            mAdditiveBlends[JSB_NUM_JOINT_STATES];
public:
    // On LLPoseBlender's active list this frame (replaces a linear list search).
    bool            mActive = false;
    LLJointStateBlender();
    ~LLJointStateBlender();
    // Blend the slots, apply the result to the joint, clear the slots.
    void blendJointStates();
    bool addJointState(const LLPointer<LLJointState>& joint_state, S32 priority, bool additive_blend);
    void clear();
    // Hand this blender's ordered slots to sink (no blending, no clearing).
    void exportJointStates(LLPoseExportSink* sink);
    void notePosAnimated(LLJoint* joint);   // #207
    // (#330: the ~1 KB LLJoint mJointCache per blender is gone with the time-quantum
    // blendAndCache / interpolate path it served -- see LLMotionController::updateMotions.)
};

class LLMotion;

class LLPoseBlender
{
protected:
    // #330: the active list was a std::list (a node malloc / free per animated joint per
    // update). LL pushed to the front and walked front to back; the vector is appended to
    // and walked BACK TO FRONT -- the same order (the GPU export sink records it).
    typedef std::vector<LLJointStateBlender*> blender_list_t;
    typedef std::unordered_map<LLJoint*,LLJointStateBlender*> blender_map_t;
    // Blender per joint: a flat table indexed by LLJoint::getJointNum() (the joint
    // pointer is kept and compared, so a number collision or a re-created joint is never
    // aliased), with the map for joints without a usable number.
    struct FlatBlender
    {
        LLJoint*             joint = nullptr;
        LLJointStateBlender* blender = nullptr;
    };
    static constexpr S32 MAX_FLAT_BLENDERS = 512;
    std::vector<FlatBlender> mFlatBlenderPool;
    blender_map_t mJointStateBlenderPool;
    blender_list_t mActiveBlenders;

    LLJointStateBlender* getJointStateBlender(LLJoint* jointp);

    S32         mNextPoseSlot;
    LLPose      mBlendedPose;
public:
    // Constructor
    LLPoseBlender();
    // Destructor
    ~LLPoseBlender();

    // request motion joint states to be added to pose blender joint state records
    bool addMotion(LLMotion* motion);

    // blend all joint states and apply to skeleton
    void blendAndApply();

    // VK GPU animation: export every active blender to sink, then either blend + apply
    // on the CPU too (shadow/compare mode) or just clear (the GPU blends).
    void exportAndClear(LLPoseExportSink* sink, bool also_apply);

    // removes all joint state blenders from last time
    void clearBlenders();

    LLPose* getBlendedPose() { return &mBlendedPose; }

    // Joint states added by addMotion since the viewer last read it (main thread; the
    // viewer reports and resets it per frame as "animPoseAdds", #330).
    static U32 sJointStatesAdded;
};

#endif // LL_LLPOSE_H

