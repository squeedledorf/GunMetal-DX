/**
 * @file llpose.cpp
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

//-----------------------------------------------------------------------------
// Header Files
//-----------------------------------------------------------------------------
#include "linden_common.h"

#include "llpose.h"

#include "llmotion.h"
#include "llmath.h"
#include "llstl.h"

//-----------------------------------------------------------------------------
// Static
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// LLPose
//-----------------------------------------------------------------------------
LLPose::~LLPose()
{
}

//-----------------------------------------------------------------------------
// getFirstJointState()
//-----------------------------------------------------------------------------
LLJointState* LLPose::getFirstJointState()
{
    mListIter = mJointMap.begin();
    if (mListIter == mJointMap.end())
    {
        return NULL;
    }
    else
    {
        return mListIter->second;
    }
}

//-----------------------------------------------------------------------------
// getNextJointState()
//-----------------------------------------------------------------------------
LLJointState *LLPose::getNextJointState()
{
    mListIter++;
    if (mListIter == mJointMap.end())
    {
        return NULL;
    }
    else
    {
        return mListIter->second;
    }
}

//-----------------------------------------------------------------------------
// getJointStates()
//-----------------------------------------------------------------------------
const std::vector<LLJointState*>& LLPose::getJointStates()
{
    if (mJointVecDirty)
    {
        mJointVec.clear();
        mJointVec.reserve(mJointMap.size());
        for (joint_map_value_type& joint_pair : mJointMap)
            mJointVec.push_back(joint_pair.second.get());
        mJointVecDirty = false;
    }
    return mJointVec;
}

//-----------------------------------------------------------------------------
// addJointState()
//-----------------------------------------------------------------------------
bool LLPose::addJointState(const LLPointer<LLJointState>& jointState)
{
    if (mJointMap.find(jointState->getJoint()->getName()) == mJointMap.end())
    {
        mJointMap[jointState->getJoint()->getName()] = jointState;
        mJointVecDirty = true;
    }
    return true;
}

//-----------------------------------------------------------------------------
// removeJointState()
//-----------------------------------------------------------------------------
bool LLPose::removeJointState(const LLPointer<LLJointState>& jointState)
{
    mJointMap.erase(jointState->getJoint()->getName());
    mJointVecDirty = true;
    return true;
}

//-----------------------------------------------------------------------------
// removeAllJointStates()
//-----------------------------------------------------------------------------
bool LLPose::removeAllJointStates()
{
    mJointMap.clear();
    mJointVecDirty = true;
    return true;
}

//-----------------------------------------------------------------------------
// findJointState()
//-----------------------------------------------------------------------------
LLJointState* LLPose::findJointState(LLJoint *joint)
{
    joint_map_iterator iter = mJointMap.find(joint->getName());

    if (iter == mJointMap.end())
    {
        return NULL;
    }
    else
    {
        return iter->second;
    }
}

//-----------------------------------------------------------------------------
// findJointState()
//-----------------------------------------------------------------------------
LLJointState* LLPose::findJointState(const std::string &name)
{
    joint_map_iterator iter = mJointMap.find(name);

    if (iter == mJointMap.end())
    {
        return NULL;
    }
    else
    {
        return iter->second;
    }
}

//-----------------------------------------------------------------------------
// setWeight()
//-----------------------------------------------------------------------------
void LLPose::setWeight(F32 weight)
{
    // Every active motion sets its pose weight every frame, per avatar: walk the flat
    // copy, not the std::map (a measurable crowd cost).
    for (LLJointState* js : getJointStates())
        js->setWeight(weight);
    mWeight = weight;
}

//-----------------------------------------------------------------------------
// getWeight()
//-----------------------------------------------------------------------------
F32 LLPose::getWeight() const
{
    return mWeight;
}

//-----------------------------------------------------------------------------
// getNumJointStates()
//-----------------------------------------------------------------------------
S32 LLPose::getNumJointStates() const
{
    return (S32)mJointMap.size();
}

//-----------------------------------------------------------------------------
// LLJointStateBlender
//-----------------------------------------------------------------------------

LLJointStateBlender::LLJointStateBlender()
{
    for(S32 i = 0; i < JSB_NUM_JOINT_STATES; i++)
    {
        mJointStates[i] = NULL;
        mPriorities[i] = S32_MIN;
        mAdditiveBlends[i] = false;
    }
}

LLJointStateBlender::~LLJointStateBlender()
{

}

//-----------------------------------------------------------------------------
// addJointState()
//-----------------------------------------------------------------------------
bool LLJointStateBlender::addJointState(const LLPointer<LLJointState>& joint_state, S32 priority, bool additive_blend)
{
    llassert(joint_state);

    if (!joint_state->getJoint())
        // this joint state doesn't point to an actual joint, so we don't care about applying it
        return false;

    for(S32 i = 0; i < JSB_NUM_JOINT_STATES; i++)
    {
        if (mJointStates[i].isNull())
        {
            mJointStates[i] = joint_state;
            mPriorities[i] = priority;
            mAdditiveBlends[i] = additive_blend;
            return true;
        }
        else if (priority > mPriorities[i])
        {
            // we're at a higher priority than the current joint state in this slot
            // so shift everyone over
            // previous joint states (newer motions) with same priority should stay in place
            for (S32 j = JSB_NUM_JOINT_STATES - 1; j > i; j--)
            {
                mJointStates[j] = mJointStates[j - 1];
                mPriorities[j] = mPriorities[j - 1];
                mAdditiveBlends[j] = mAdditiveBlends[j - 1];
            }
            // now store ourselves in this slot
            mJointStates[i] = joint_state;
            mPriorities[i] = priority;
            mAdditiveBlends[i] = additive_blend;
            return true;
        }
    }

    return false;
}

//-----------------------------------------------------------------------------
// blendJointStates()
//-----------------------------------------------------------------------------
void LLJointStateBlender::blendJointStates()
{
    // we need at least one joint to blend
    // if there is one, it will be in slot zero according to insertion logic
    // instead of resetting joint state to default, just leave it unchanged from last frame
    if (mJointStates[0].isNull())
    {
        return;
    }

    LLJoint* target_joint = mJointStates[0]->getJoint();
    notePosAnimated(target_joint);   // before the result is written

    const S32 POS_WEIGHT = 0;
    const S32 ROT_WEIGHT = 1;
    const S32 SCALE_WEIGHT = 2;

    F32             sum_weights[3];
    U32             sum_usage = 0;

    LLVector3       blended_pos = target_joint->getPosition();
    LLQuaternion    blended_rot = target_joint->getRotation();
    LLVector3       blended_scale = target_joint->getScale();

    LLVector3       added_pos;
    LLQuaternion    added_rot;
    LLVector3       added_scale;

    //S32               joint_state_index;

    sum_weights[POS_WEIGHT] = 0.f;
    sum_weights[ROT_WEIGHT] = 0.f;
    sum_weights[SCALE_WEIGHT] = 0.f;

    for(S32 joint_state_index = 0;
        joint_state_index < JSB_NUM_JOINT_STATES && mJointStates[joint_state_index].notNull();
        joint_state_index++)
    {
        LLJointState* jsp = mJointStates[joint_state_index];
        U32 current_usage = jsp->getUsage();
        F32 current_weight = jsp->getWeight();

        if (current_weight == 0.f)
        {
            continue;
        }

        if (mAdditiveBlends[joint_state_index])
        {
            if(current_usage & LLJointState::POS)
            {
                F32 new_weight_sum = llmin(1.f, current_weight + sum_weights[POS_WEIGHT]);

                // add in pos for this jointstate modulated by weight
                added_pos += jsp->getPosition() * (new_weight_sum - sum_weights[POS_WEIGHT]);
            }

            if(current_usage & LLJointState::SCALE)
            {
                F32 new_weight_sum = llmin(1.f, current_weight + sum_weights[SCALE_WEIGHT]);

                // add in scale for this jointstate modulated by weight
                added_scale += jsp->getScale() * (new_weight_sum - sum_weights[SCALE_WEIGHT]);
            }

            if (current_usage & LLJointState::ROT)
            {
                F32 new_weight_sum = llmin(1.f, current_weight + sum_weights[ROT_WEIGHT]);

                // add in rotation for this jointstate modulated by weight
                // <GMDX> from identity, not from what the earlier additive states already added: LL's
                // nlerp(w, added_rot, rot) * added_rot doubled the earlier ones whenever this state's share
                // was 0 (a later additive under a full-weight animation, e.g. body noise on the torso)
                // (the three-argument nlerp: LL's one-argument lerp(t, q) takes w from q's z)
                added_rot = nlerp((new_weight_sum - sum_weights[ROT_WEIGHT]), LLQuaternion::DEFAULT, jsp->getRotation()) * added_rot;
            }
        }
        else
        {
            // blend two jointstates together

            // blend position
            if(current_usage & LLJointState::POS)
            {
                if(sum_usage & LLJointState::POS)
                {
                    F32 new_weight_sum = llmin(1.f, current_weight + sum_weights[POS_WEIGHT]);

                    // blend positions from both
                    blended_pos = lerp(jsp->getPosition(), blended_pos, sum_weights[POS_WEIGHT] / new_weight_sum);
                    sum_weights[POS_WEIGHT] = new_weight_sum;
                }
                else
                {
                    // copy position from current
                    blended_pos = jsp->getPosition();
                    sum_weights[POS_WEIGHT] = current_weight;
                }
            }

            // now do scale
            if(current_usage & LLJointState::SCALE)
            {
                if(sum_usage & LLJointState::SCALE)
                {
                    F32 new_weight_sum = llmin(1.f, current_weight + sum_weights[SCALE_WEIGHT]);

                    // blend scales from both
                    blended_scale = lerp(jsp->getScale(), blended_scale, sum_weights[SCALE_WEIGHT] / new_weight_sum);
                    sum_weights[SCALE_WEIGHT] = new_weight_sum;
                }
                else
                {
                    // copy scale from current
                    blended_scale = jsp->getScale();
                    sum_weights[SCALE_WEIGHT] = current_weight;
                }
            }

            // rotation
            if (current_usage & LLJointState::ROT)
            {
                if(sum_usage & LLJointState::ROT)
                {
                    F32 new_weight_sum = llmin(1.f, current_weight + sum_weights[ROT_WEIGHT]);

                    // blend rotations from both
                    blended_rot = nlerp(sum_weights[ROT_WEIGHT] / new_weight_sum, jsp->getRotation(), blended_rot);
                    sum_weights[ROT_WEIGHT] = new_weight_sum;
                }
                else
                {
                    // copy rotation from current
                    blended_rot = jsp->getRotation();
                    sum_weights[ROT_WEIGHT] = current_weight;
                }
            }

            // update resulting usage mask
            sum_usage = sum_usage | current_usage;
        }
    }

    if (!added_scale.isFinite())
    {
        added_scale.clearVec();
    }

    if (!blended_scale.isFinite())
    {
        blended_scale.setVec(1,1,1);
    }

    // apply transforms
    // SL-315
    target_joint->setPosition(blended_pos + added_pos);
    target_joint->setScale(blended_scale + added_scale);
    target_joint->setRotation(added_rot * blended_rot);

    // now clear joint states
    for(S32 i = 0; i < JSB_NUM_JOINT_STATES; i++)
    {
        mJointStates[i] = NULL;
    }
}

//-----------------------------------------------------------------------------
// exportJointStates()
//-----------------------------------------------------------------------------
// #207: a non-additive state with weight drives the joint's position this evaluation
// (see LLJoint::morphPosDelta / endPosEvaluation).
void LLJointStateBlender::notePosAnimated(LLJoint* joint)
{
    if (!joint) return;
    for (S32 i = 0; i < JSB_NUM_JOINT_STATES && mJointStates[i].notNull(); ++i)
    {
        if (!mAdditiveBlends[i] && mJointStates[i]->getWeight() > 0.f
            && (mJointStates[i]->getUsage() & LLJointState::POS))
        {
            joint->notePosAnimated();
            return;
        }
    }
}

void LLJointStateBlender::exportJointStates(LLPoseExportSink* sink)
{
    LLJointState* states[JSB_NUM_JOINT_STATES];
    S32 n = 0;
    while (n < JSB_NUM_JOINT_STATES && mJointStates[n].notNull())
    {
        states[n] = mJointStates[n];
        ++n;
    }
    if (n > 0)
    {
        notePosAnimated(mJointStates[0]->getJoint());
        sink->exportJoint(mJointStates[0]->getJoint(), states, mAdditiveBlends, n);
    }
}

//-----------------------------------------------------------------------------
// clear()
//-----------------------------------------------------------------------------
void LLJointStateBlender::clear()
{
    // now clear joint states
    for(S32 i = 0; i < JSB_NUM_JOINT_STATES; i++)
    {
        mJointStates[i] = NULL;
    }
}

//-----------------------------------------------------------------------------
// LLPoseBlender
//-----------------------------------------------------------------------------

U32 LLPoseBlender::sJointStatesAdded = 0;

LLPoseBlender::LLPoseBlender()
    : mNextPoseSlot(0)
{
}

LLPoseBlender::~LLPoseBlender()
{
    for (FlatBlender& fb : mFlatBlenderPool)
    {
        delete fb.blender;
    }
    mFlatBlenderPool.clear();
    for_each(mJointStateBlenderPool.begin(), mJointStateBlenderPool.end(), DeletePairedPointer());
    mJointStateBlenderPool.clear();
}

//-----------------------------------------------------------------------------
// getJointStateBlender()
//-----------------------------------------------------------------------------
LLJointStateBlender* LLPoseBlender::getJointStateBlender(LLJoint* jointp)
{
    // #330: flat table by joint number; the stored joint pointer must match, so a joint
    // only ever gets the blender LL's pointer-keyed map would have given it.
    const S32 joint_num = jointp ? jointp->getJointNum() : -1;
    if (joint_num >= 0 && joint_num < MAX_FLAT_BLENDERS)
    {
        if ((size_t)joint_num >= mFlatBlenderPool.size())
        {
            mFlatBlenderPool.resize((size_t)joint_num + 1);
        }
        FlatBlender& fb = mFlatBlenderPool[joint_num];
        if (fb.joint == jointp)
        {
            return fb.blender;
        }
        if (!fb.joint)
        {
            // this is the first time we are animating this joint
            fb.joint = jointp;
            fb.blender = new LLJointStateBlender();
            return fb.blender;
        }
        // another joint holds this number: keyed by pointer below
    }
    // find first: unordered_map::emplace allocates (and frees) a node even when the
    // key exists -- a malloc/free per joint state per motion per avatar per frame.
    auto it = mJointStateBlenderPool.find(jointp);
    if (it == mJointStateBlenderPool.end())
    {
        // this is the first time we are animating this joint
        // so create new jointblender and add it to our pool
        it = mJointStateBlenderPool.emplace(jointp, new LLJointStateBlender()).first;
    }
    return it->second;
}

//-----------------------------------------------------------------------------
// addMotion()
//-----------------------------------------------------------------------------
bool LLPoseBlender::addMotion(LLMotion* motion)
{
    LLPose* pose = motion->getPose();
    const S32 motion_priority = motion->getPriority();
    const bool additive = motion->getBlendType() == LLMotion::ADDITIVE_BLEND;

    const std::vector<LLJointState*>& joint_states = pose->getJointStates();
    sJointStatesAdded += (U32)joint_states.size();
    for (LLJointState* jsp : joint_states)
    {
        LLJointStateBlender* joint_blender = getJointStateBlender(jsp->getJoint());

        const S32 priority = (jsp->getPriority() == LLJoint::USE_MOTION_PRIORITY) ? motion_priority : jsp->getPriority();
        joint_blender->addJointState(jsp, priority, additive);

        // add it to our list of active blenders (flag instead of a linear list search:
        // with Bento skeletons this was O(joints^2) per motion per avatar per frame)
        if (!joint_blender->mActive)
        {
            joint_blender->mActive = true;
            mActiveBlenders.push_back(joint_blender);   // walked back to front (LL: push_front)
        }
    }
    return true;
}

//-----------------------------------------------------------------------------
// blendAndApply()
//-----------------------------------------------------------------------------
void LLPoseBlender::blendAndApply()
{
    for (blender_list_t::reverse_iterator iter = mActiveBlenders.rbegin();
         iter != mActiveBlenders.rend(); ++iter)
    {
        LLJointStateBlender* jsbp = *iter;
        jsbp->blendJointStates();
        jsbp->mActive = false;
    }

    // we're done now so there are no more active blenders for this frame
    mActiveBlenders.clear();
}

//-----------------------------------------------------------------------------
// exportAndClear()
//-----------------------------------------------------------------------------
void LLPoseBlender::exportAndClear(LLPoseExportSink* sink, bool also_apply)
{
    for (blender_list_t::reverse_iterator iter = mActiveBlenders.rbegin();
         iter != mActiveBlenders.rend(); ++iter)
    {
        LLJointStateBlender* jsbp = *iter;
        jsbp->exportJointStates(sink);
        if (also_apply)
        {
            jsbp->blendJointStates();   // clears its slots
        }
        else
        {
            jsbp->clear();
        }
        jsbp->mActive = false;
    }
    mActiveBlenders.clear();
}

//-----------------------------------------------------------------------------
// clearBlenders()
//-----------------------------------------------------------------------------
void LLPoseBlender::clearBlenders()
{
    for (blender_list_t::reverse_iterator iter = mActiveBlenders.rbegin();
         iter != mActiveBlenders.rend(); ++iter)
    {
        LLJointStateBlender* jsbp = *iter;
        jsbp->clear();
        jsbp->mActive = false;
    }

    mActiveBlenders.clear();
}
