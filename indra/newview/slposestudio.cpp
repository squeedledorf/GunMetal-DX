/**
 * @file slposestudio.cpp
 * @brief Pose Studio (#500): the shared posing engine.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
 * Portions Copyright (c) 2024 Angeldark Raymaker @ Second Life (Firestorm)
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
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "slposestudio.h"

#include "llagent.h"
#include "llcallbacklist.h"
#include "llcharacter.h"
#include "llcontrolavatar.h"
#include "lldir.h"
#include "lldiriterator.h"
#include "lleventtimer.h"
#include "llfile.h"
#include "llmutelist.h"
#include "llsdserialize.h"
#include "llsdutil.h"
#include "llsdutil_math.h"
#include "lluri.h"
#include "llviewercontrol.h"
#include "llvoavatarself.h"
#include "llvovolume.h"
#include "slposesync.h"

#include <algorithm>

namespace
{
constexpr char POSE_INTERNAL_FORMAT_FILE_MASK[] = "*.xml";
constexpr char POSE_INTERNAL_FORMAT_FILE_EXT[]  = ".xml";
constexpr char POSE_SAVE_SUBDIRECTORY[]         = "poses";
constexpr char POSE_PRESETS_HANDS_SUBDIRECTORY[] = "hand_presets";
constexpr char SEQUENCE_FILE_NAME[]             = "pose_studio_sequence.xml";
constexpr S32  POSE_FILE_VERSION                = 8;
constexpr F32  NEARBY_RANGE                     = 50.f;
constexpr F32  LOAD_RETRY_PERIOD                = 0.5f;
constexpr S32  LOAD_MAX_ATTEMPTS                = 5;
constexpr F32  MIN_HOLD                         = 0.25f;
constexpr F32  TICK_PERIOD                      = 0.1f;

// One-shot-capable timer that calls back into the studio until cancelled.
class SLPoseStudioTimer : public LLEventTimer
{
public:
    SLPoseStudioTimer(F32 period, std::function<void()> cb) : LLEventTimer(period), mCallback(std::move(cb)) {}
    bool tick() override
    {
        if (mCallback)
            mCallback();
        return false;
    }

private:
    std::function<void()> mCallback;
};

std::string sequencePath()
{
    return gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, SEQUENCE_FILE_NAME);
}
}

SLPoseStudio::SLPoseStudio()
{
    loadBoneModels();
    loadSequence();
}

SLPoseStudio::~SLPoseStudio()
{
    if (mBlendIdleRegistered)
        gIdleCallbacks.deleteFunction(&SLPoseStudio::onIdleBlend, this);
    delete mTicker;
    mTicker = nullptr;
}

void SLPoseStudio::ensureTicker()
{
    // One persistent ticker for pose-load retries and sequence holds; it is never deleted
    // from inside its own tick.
    if (!mTicker)
        mTicker = new SLPoseStudioTimer(TICK_PERIOD, [this]() { onTick(); });
}

void SLPoseStudio::onTick()
{
    if (!mRetryPose.empty() && mRetryTimer.getElapsedTimeF32() >= LOAD_RETRY_PERIOD)
    {
        mRetryTimer.reset();
        if (loadPose(getTarget(), mRetryPose, mRetryMethod))
        {
            mRetryPose.clear();
        }
        else if (++mRetryAttempts >= LOAD_MAX_ATTEMPTS)
        {
            LL_WARNS("Posing") << "Gave up loading pose " << mRetryPose << LL_ENDL;
            mRetryPose.clear();
        }
    }

    if (mPlaying && mSequenceTimer.getElapsedTimeF32() >= mSequenceHold)
        stepSequence(1);
}

// ---------------------------------------------------------------------------
// Target
// ---------------------------------------------------------------------------

LLVOAvatar* SLPoseStudio::getTarget() const
{
    if (mTargetID.isNull())
        return isAgentAvatarValid() ? gAgentAvatarp.get() : nullptr;

    for (LLCharacter* character : LLCharacter::sInstances)
    {
        if (character && character->getID() == mTargetID)
        {
            LLVOAvatar* avatar = dynamic_cast<LLVOAvatar*>(character);
            if (avatar && !avatar->isDead())
                return avatar;
        }
    }
    return nullptr;
}

void SLPoseStudio::setTarget(const LLUUID& avatar_id)
{
    const LLUUID id = (isAgentAvatarValid() && avatar_id == gAgentID) ? LLUUID::null : avatar_id;
    if (id == mTargetID)
        return;
    mTargetID = id;
    notifyChanged();
}

bool SLPoseStudio::canPose(LLVOAvatar* avatar) const
{
    if (!avatar || avatar->isDead() || avatar->isUIAvatar())
        return false;
    if (avatar->getRegion() != gAgent.getRegion())
        return false;
    if (avatar->isSelf())
        return true;

    if (avatar->isControlAvatar())
    {
        LLControlAvatar* control_av = dynamic_cast<LLControlAvatar*>(avatar);
        const LLVOVolume* root_volume = control_av ? control_av->mRootVolp : nullptr;
        const LLViewerObject* root_edit = root_volume ? root_volume->getRootEdit() : nullptr;
        return root_edit && root_edit->permYouOwner();
    }

    // Other residents only with a consent grant the sync layer verified itself (the
    // target's own answer, never the poser's word).
    return SLPoseSync::instanceExists() && SLPoseSync::instance().hasVerifiedGrant(avatar->getID(), gAgentID);
}

bool SLPoseStudio::isPosing(LLVOAvatar* avatar) const
{
    return avatar && mAnimator.isPosingAvatar(avatar);
}

bool SLPoseStudio::startPosing(LLVOAvatar* avatar)
{
    if (!canPose(avatar))
        return false;
    if (mAnimator.isPosingAvatar(avatar))
        return true;
    if (!mAnimator.tryPosingAvatar(avatar))
        return false;

    mPosedByStudio.insert(avatar->getID());
    applyFreeze(avatar);
    mPosingSignal(avatar->getID(), true);
    notifyChanged();
    return true;
}

void SLPoseStudio::stopPosing(LLVOAvatar* avatar)
{
    if (!avatar)
        return;
    if (mAnimator.isPosingAvatar(avatar))
        mAnimator.stopPosingAvatar(avatar);
    cancelBlend(avatar->getID());
    mEdited.erase(avatar->getID());
    mDialAngles.erase(avatar->getID());
    mEditEase.erase(avatar->getID());
    mPreviewing.erase(avatar->getID());
    mPosedByStudio.erase(avatar->getID());
    mPosingSignal(avatar->getID(), false);
    notifyChanged();
}

void SLPoseStudio::stopAll()
{
    stopPlayback();
    const std::set<LLUUID> posed = mPosedByStudio;
    for (LLCharacter* character : LLCharacter::sInstances)
    {
        LLVOAvatar* avatar = dynamic_cast<LLVOAvatar*>(character);
        if (avatar && posed.count(avatar->getID()))
            stopPosing(avatar);
    }
    mPosedByStudio.clear();
    mDialAngles.clear();    // also avatars that left without stopPosing (bounded per session)
    notifyChanged();
}

std::vector<LLUUID> SLPoseStudio::getPoseableNearby() const
{
    std::vector<LLUUID> ids;
    if (!isAgentAvatarValid())
        return ids;

    ids.push_back(gAgentID);
    for (LLCharacter* character : LLCharacter::sInstances)
    {
        LLVOAvatar* avatar = dynamic_cast<LLVOAvatar*>(character);
        if (!avatar || avatar->isSelf() || avatar->isDead())
            continue;
        const LLVector3 separation = character->getCharacterPosition() - gAgentAvatarp->getCharacterPosition();
        if (separation.magVec() > NEARBY_RANGE)
            continue;
        if (LLMuteList::getInstance()->isMuted(avatar->getID()))
            continue;
        if (canPose(avatar))
            ids.push_back(avatar->getID());
    }
    return ids;
}

// ---------------------------------------------------------------------------
// Bones
// ---------------------------------------------------------------------------

const SLPoseStudio::joint_t* SLPoseStudio::findJoint(const std::string& joint_name) const
{
    return mAnimator.getPoserJointByName(joint_name);
}

bool SLPoseStudio::avatarHasJoint(LLVOAvatar* avatar, const joint_t& joint) const
{
    return avatar && avatar->getJoint(joint.jointName()) != nullptr;
}

void SLPoseStudio::loadBoneModels()
{
    mBoneModels.clear();
    mDefaultModel = SLBoneModel();
    const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "posestudio", "bone_axes.xml");
    llifstream infile(path.c_str());
    LLSD data;
    if (!infile.is_open() || LLSDSerialize::fromXML(data, infile) == LLSDParser::PARSE_FAILURE || !data.isMap())
    {
        LL_WARNS("PoseStudio") << "Cannot read " << path << "; every bone uses the default dial model" << LL_ENDL;
        return;
    }
    if (data.has("default"))
        mDefaultModel = SLBoneModel::fromLLSD(data["default"], SLBoneModel());
    for (const auto& [joint, entry] : llsd::inMap(data))
    {
        if (joint != "default")
            mBoneModels[joint] = SLBoneModel::fromLLSD(entry, mDefaultModel);
    }
    LL_INFOS("PoseStudio") << "Loaded " << mBoneModels.size() << " OP Pose HUD bone models" << LL_ENDL;
}

const SLBoneModel& SLPoseStudio::getBoneModel(const std::string& joint_name) const
{
    auto it = mBoneModels.find(joint_name);
    return (it != mBoneModels.end()) ? it->second : mDefaultModel;
}

bool SLPoseStudio::getBoneAngles(LLVOAvatar* avatar, const joint_t& joint, SLPoseAngles& out)
{
    if (!avatar)
        return false;
    // Mid ease-in (pose load, sequence step, sync receive) the joint's target is an
    // in-between frame -- right after the load it is still the old pose. Show the end.
    LLQuaternion rotation;
    LLVector3 position, scale;
    if (!getBlendTarget(avatar, joint, rotation, position, scale) && !mAnimator.getJointTargetRotation(avatar, joint, rotation))
        return false;

    // The remembered dial values are the reference: fromQuat returns them unchanged while
    // they still produce this rotation (what the user dialled comes back exactly, poles and
    // all), and otherwise the in-range triple nearest to them. Never-seen bones start from
    // dial zero.
    std::map<std::string, SLPoseAngles>& bones = mDialAngles[avatar->getID()];
    auto it = bones.find(joint.jointName());
    const SLPoseAngles reference = (it != bones.end()) ? it->second : SLPoseAngles();
    const SLBoneModel& model = getBoneModel(joint.jointName());
    if (it != bones.end())
    {
        // The remembered values still describe this bone (within what limits, followers or float
        // rounding change): show them as they are. Re-deriving here on every refresh made the other
        // axes jump while a dial was dragged.
        LLQuaternion target(rotation);
        target.normalize();
        const F32 cos_half = llmin(1.f, llabs(dot(model.toQuat(reference), target)));
        if (2.f * acosf(cos_half) * RAD_TO_DEG <= 2.f)
        {
            out = reference;
            return true;
        }
    }
    out = model.fromQuat(rotation, reference);
    bones[joint.jointName()] = out;

    // Self-check (debug log): the shown dials must reproduce the bone's rotation. They
    // cannot for a rotation no dial setting reaches, e.g. one made in the classic poser.
    LLQuaternion target(rotation);
    target.normalize();
    const F32 cos_half = llmin(1.f, llabs(dot(model.toQuat(out), target)));
    const F32 off_degrees = 2.f * acosf(cos_half) * RAD_TO_DEG;
    LL_DEBUGS("PoseStudio") << joint.jointName() << " dials " << out.mX << " " << out.mY << " " << out.mZ << " (reference "
                            << reference.mX << " " << reference.mY << " " << reference.mZ << "), " << off_degrees
                            << " deg from the bone's rotation" << LL_ENDL;
    return true;
}

bool SLPoseStudio::setBoneAngles(LLVOAvatar* avatar, const joint_t& joint, const SLPoseAngles& angles, bool committed)
{
    if (!avatar)
        return false;
    const SLBoneModel& model = getBoneModel(joint.jointName());
    // Remember the values first: setBoneRotation derives the mirror partner's dials from them.
    mDialAngles[avatar->getID()][joint.jointName()] = angles;
    beginEdit(avatar, committed);
    if (!setBoneRotation(avatar, joint, model.toQuat(angles), committed))
    {
        endEdit(avatar);
        return false;
    }

    // Followers move with the bone, exactly as in the HUD's animations.
    for (const SLBoneModel::Follower& follower : model.mFollowers)
    {
        const joint_t* fj = findJoint(follower.mJoint);
        if (!fj)
            continue;
        const LLQuaternion rot = model.toQuat(angles, follower.mInvert);
        cancelBoneBlend(avatar->getID(), *fj);
        mAnimator.setJointTargetRotation(avatar, *fj, rot);
        markEdited(avatar, *fj, false);
        // The follower has its own dial model (e.g. mTorso): decode, do not copy.
        rememberRotation(avatar, *fj, rot, nullptr);
        if (getMirror() && !fj->mirrorJointName().empty())
        {
            if (const joint_t* opposite = findJoint(fj->mirrorJointName()))
            {
                cancelBoneBlend(avatar->getID(), *opposite);
                mAnimator.setJointTargetRotation(avatar, *opposite, SLBoneModel::mirror(rot));
                markEdited(avatar, *opposite, false);
                rememberRotation(avatar, *opposite, SLBoneModel::mirror(rot), nullptr);
            }
        }
    }
    endEdit(avatar);
    return true;
}

bool SLPoseStudio::setBoneRotation(LLVOAvatar* avatar, const joint_t& joint, const LLQuaternion& rotation, bool committed)
{
    if (!canPose(avatar) || !mAnimator.isPosingAvatar(avatar))
        return false;
    // The bones this edit sets stop any ease they were in (other bones carry on); a
    // committed edit that was not previewed live eases in from what is on screen (endEdit).
    beginEdit(avatar, committed);
    cancelBoneBlend(avatar->getID(), joint);
    if (!mAnimator.setJointTargetRotation(avatar, joint, rotation))
    {
        endEdit(avatar);
        return false;
    }
    markEdited(avatar, joint, false);

    // Mirror mode: the opposite limb gets the reflected local rotation. Bones on the
    // spine have no opposite and are unaffected.
    if (getMirror() && !joint.mirrorJointName().empty())
    {
        if (const joint_t* opposite = findJoint(joint.mirrorJointName()))
        {
            cancelBoneBlend(avatar->getID(), *opposite);
            mAnimator.setJointTargetRotation(avatar, *opposite, SLBoneModel::mirror(rotation));
            markEdited(avatar, *opposite, false);
            // The HUD's mirror rule on the dials (X -> -X, Y kept, Z -> -Z) is exact for every
            // HUD bone but the hips; fromQuat checks it and falls back to the nearest triple.
            const SLPoseAngles* source = nullptr;
            auto bones = mDialAngles.find(avatar->getID());
            if (bones != mDialAngles.end())
            {
                auto it = bones->second.find(joint.jointName());
                if (it != bones->second.end())
                    source = &it->second;
            }
            SLPoseAngles hint;
            if (source)
            {
                hint.mX = -source->mX;
                hint.mY = source->mY;
                hint.mZ = -source->mZ;
            }
            rememberRotation(avatar, *opposite, SLBoneModel::mirror(rotation), source ? &hint : nullptr);
        }
    }

    notifyEdited(avatar, committed);
    endEdit(avatar);
    return true;
}

void SLPoseStudio::rememberRotation(LLVOAvatar* avatar, const joint_t& joint, const LLQuaternion& rotation, const SLPoseAngles* hint)
{
    if (!avatar)
        return;
    std::map<std::string, SLPoseAngles>& bones = mDialAngles[avatar->getID()];
    SLPoseAngles reference;
    if (hint)
    {
        reference = *hint;
    }
    else
    {
        auto it = bones.find(joint.jointName());
        if (it != bones.end())
            reference = it->second;
    }
    bones[joint.jointName()] = getBoneModel(joint.jointName()).fromQuat(rotation, reference);
}

bool SLPoseStudio::getRememberedAngles(const LLUUID& avatar_id, const std::string& joint_name, SLPoseAngles& out) const
{
    auto av = mDialAngles.find(avatar_id);
    if (av == mDialAngles.end())
        return false;
    auto it = av->second.find(joint_name);
    if (it == av->second.end())
        return false;
    out = it->second;
    return true;
}

void SLPoseStudio::forgetAngles(const LLUUID& avatar_id, const std::string& joint_name)
{
    auto it = mDialAngles.find(avatar_id);
    if (it != mDialAngles.end())
        it->second.erase(joint_name);
}

LLVector3 SLPoseStudio::getBonePosition(LLVOAvatar* avatar, const joint_t& joint) const
{
    LLQuaternion rotation;
    LLVector3 position, scale;
    if (getBlendTarget(avatar, joint, rotation, position, scale))
        return position;
    return mAnimator.getJointPosition(avatar, joint);
}

void SLPoseStudio::setBonePosition(LLVOAvatar* avatar, const joint_t& joint, const LLVector3& position, bool committed)
{
    if (!canPose(avatar) || !mAnimator.isPosingAvatar(avatar))
        return;
    beginEdit(avatar, committed);
    cancelBoneBlend(avatar->getID(), joint);
    if (const joint_t* opposite = getMirror() ? findJoint(joint.mirrorJointName()) : nullptr)
        cancelBoneBlend(avatar->getID(), *opposite);
    mAnimator.setPosingAvatarJoint(avatar, joint, true);
    mAnimator.setJointPosition(avatar, &joint, position, POSER_FRAME_BONE, getMirror() ? MIRROR : NONE);
    markEdited(avatar, joint, getMirror());
    notifyEdited(avatar, committed);
    endEdit(avatar);
}

LLVector3 SLPoseStudio::getBoneScale(LLVOAvatar* avatar, const joint_t& joint) const
{
    LLQuaternion rotation;
    LLVector3 position, scale;
    if (getBlendTarget(avatar, joint, rotation, position, scale))
        return scale;
    return mAnimator.getJointScale(avatar, joint);
}

void SLPoseStudio::setBoneScale(LLVOAvatar* avatar, const joint_t& joint, const LLVector3& scale)
{
    if (!canPose(avatar) || !mAnimator.isPosingAvatar(avatar))
        return;
    beginEdit(avatar, true);
    cancelBoneBlend(avatar->getID(), joint);
    if (const joint_t* opposite = getMirror() ? findJoint(joint.mirrorJointName()) : nullptr)
        cancelBoneBlend(avatar->getID(), *opposite);
    mAnimator.setPosingAvatarJoint(avatar, joint, true);
    mAnimator.setJointScale(avatar, &joint, scale, POSER_FRAME_BONE, getMirror() ? MIRROR : NONE);
    markEdited(avatar, joint, getMirror());
    notifyEdited(avatar, true);
    endEdit(avatar);
}

bool SLPoseStudio::isBoneEdited(const LLUUID& avatar_id, const std::string& joint_name) const
{
    auto it = mEdited.find(avatar_id);
    return it != mEdited.end() && it->second.count(joint_name) != 0;
}

bool SLPoseStudio::isBoneChanged(LLVOAvatar* avatar, const joint_t& joint) const
{
    return avatar && isBoneEdited(avatar->getID(), joint.jointName());
}

void SLPoseStudio::markEdited(LLVOAvatar* avatar, const joint_t& joint, bool with_mirror)
{
    if (!avatar)
        return;
    std::set<std::string>& edited = mEdited[avatar->getID()];
    edited.insert(joint.jointName());
    if (with_mirror && !joint.mirrorJointName().empty())
        edited.insert(joint.mirrorJointName());
}

void SLPoseStudio::holdOrRelease(LLVOAvatar* avatar, const joint_t& joint)
{
    // An unedited bone: with Freeze it holds the frame it shows right now, without it the
    // animations underneath play on it.
    if (getFreeze())
    {
        if (!mAnimator.isPosingAvatarJoint(avatar, joint))
            mAnimator.recaptureJoint(avatar, joint);
    }
    else
    {
        mAnimator.setPosingAvatarJoint(avatar, joint, false);
    }
}

void SLPoseStudio::resetBone(LLVOAvatar* avatar, const joint_t& joint)
{
    if (!mAnimator.isPosingAvatar(avatar))
        return;
    cancelBlend(avatar->getID());
    mAnimator.resetJoint(avatar, joint, getMirror() ? MIRROR : NONE);

    // No longer part of the pose: not saved, not shared, and back to the animation.
    std::set<std::string>& edited = mEdited[avatar->getID()];
    edited.erase(joint.jointName());
    forgetAngles(avatar->getID(), joint.jointName());
    if (!getFreeze())
        mAnimator.setPosingAvatarJoint(avatar, joint, false);
    if (getMirror() && !joint.mirrorJointName().empty())
    {
        edited.erase(joint.mirrorJointName());
        forgetAngles(avatar->getID(), joint.mirrorJointName());
        if (const joint_t* opposite = findJoint(joint.mirrorJointName()); opposite && !getFreeze())
            mAnimator.setPosingAvatarJoint(avatar, *opposite, false);
    }
    notifyEdited(avatar, true);
}

void SLPoseStudio::resetAll(LLVOAvatar* avatar)
{
    if (!mAnimator.isPosingAvatar(avatar))
        return;
    cancelBlend(avatar->getID());
    for (const joint_t& joint : mAnimator.PoserJoints)
        mAnimator.resetJoint(avatar, joint, NONE);
    mEdited.erase(avatar->getID());
    mDialAngles.erase(avatar->getID());
    applyFreeze(avatar);
    notifyEdited(avatar, true);
}

std::vector<SLPoseStudio::BoneState> SLPoseStudio::captureBoneStates(LLVOAvatar* avatar) const
{
    std::vector<BoneState> states;
    if (!avatar || !mAnimator.isPosingAvatar(avatar))
        return states;
    states.reserve(mAnimator.PoserJoints.size());
    for (const joint_t& joint : mAnimator.PoserJoints)
    {
        BoneState state;
        if (!mAnimator.getJointTargetTransform(avatar, joint, state.mRotation, state.mPosition, state.mScale))
            continue;
        state.mJoint = joint.jointName();
        state.mEdited = isBoneEdited(avatar->getID(), state.mJoint);
        states.push_back(state);
    }
    return states;
}

void SLPoseStudio::restoreBoneStates(LLVOAvatar* avatar, const std::vector<BoneState>& states)
{
    if (states.empty() || !canPose(avatar) || !mAnimator.isPosingAvatar(avatar))
        return;
    // Undo / redo of a whole-pose step eases like any other direct edit.
    beginEdit(avatar, true);
    std::set<std::string>& edited = mEdited[avatar->getID()];
    for (const BoneState& state : states)
    {
        const joint_t* joint = findJoint(state.mJoint);
        if (joint)
            cancelBoneBlend(avatar->getID(), *joint);
        if (!joint || !mAnimator.setJointTargetTransform(avatar, *joint, state.mRotation, state.mPosition, state.mScale))
            continue;
        // The dials re-derive from the restored rotation.
        forgetAngles(avatar->getID(), state.mJoint);
        if (state.mEdited)
        {
            edited.insert(state.mJoint);
        }
        else
        {
            edited.erase(state.mJoint);
            holdOrRelease(avatar, *joint);
        }
    }
    notifyEdited(avatar, true);
    endEdit(avatar);
}

void SLPoseStudio::undoBone(LLVOAvatar* avatar, const joint_t& joint)
{
    if (!mAnimator.isPosingAvatar(avatar))
        return;
    beginEdit(avatar, true);
    cancelBoneBlend(avatar->getID(), joint);
    mAnimator.undoLastJointChange(avatar, joint, getMirror() ? MIRROR : NONE);
    notifyEdited(avatar, true);
    endEdit(avatar);
}

void SLPoseStudio::redoBone(LLVOAvatar* avatar, const joint_t& joint)
{
    if (!mAnimator.isPosingAvatar(avatar))
        return;
    beginEdit(avatar, true);
    cancelBoneBlend(avatar->getID(), joint);
    mAnimator.redoLastJointChange(avatar, joint, getMirror() ? MIRROR : NONE);
    notifyEdited(avatar, true);
    endEdit(avatar);
}

// ---------------------------------------------------------------------------
// Modes
// ---------------------------------------------------------------------------

bool SLPoseStudio::getMirror() const
{
    static LLCachedControl<bool> mirror(gSavedSettings, "SLPoseStudioMirror", false);
    return mirror && !mMirrorSuppressed;
}

void SLPoseStudio::setMirror(bool mirror)
{
    gSavedSettings.setBOOL("SLPoseStudioMirror", mirror);
    notifyChanged();
}

bool SLPoseStudio::getFreeze() const
{
    static LLCachedControl<bool> freeze(gSavedSettings, "SLPoseStudioFreeze", false);
    return freeze;
}

void SLPoseStudio::setFreeze(bool freeze)
{
    gSavedSettings.setBOOL("SLPoseStudioFreeze", freeze);
    for (const LLUUID& id : mPosedByStudio)
    {
        for (LLCharacter* character : LLCharacter::sInstances)
        {
            if (character && character->getID() == id)
                applyFreeze(dynamic_cast<LLVOAvatar*>(character));
        }
    }
    notifyChanged();
}

void SLPoseStudio::applyFreeze(LLVOAvatar* avatar)
{
    if (!avatar || !mAnimator.isPosingAvatar(avatar))
        return;

    // The posing motion runs at the highest priority and owns every joint it has a state
    // for, so holding a joint state freezes that bone and dropping it lets the animations
    // underneath play through. Edited bones are always held.
    for (const joint_t& joint : mAnimator.PoserJoints)
    {
        if (isBoneEdited(avatar->getID(), joint.jointName()))
            mAnimator.setPosingAvatarJoint(avatar, joint, true);
        else
            holdOrRelease(avatar, joint);
    }
}

void SLPoseStudio::notifyEdited(LLVOAvatar* avatar, bool committed, F32 ease)
{
    if (!avatar)
        return;
    // Recorded before the signal, so the pose sync can tag what it sends with the ease.
    if (committed && ease != EASE_KEEP)
        mEditEase[avatar->getID()] = (ease < 0.f) ? EASE_EDIT : llmin(ease, MAX_EASE_IN);
    // A live drag: its edits follow the mouse and its release does not ease again. Only
    // previews made directly by the user count -- an IK pin re-solving inside this signal
    // (committed=false) is not a drag.
    if (!committed && mEditedSignalDepth == 0)
        mPreviewing.insert(avatar->getID());
    else if (committed && ease != EASE_KEEP)
        mPreviewing.erase(avatar->getID());
    ++mEditedSignalDepth;
    mEditedSignal(avatar->getID(), committed);
    --mEditedSignalDepth;
}

F32 SLPoseStudio::getLastEditEase(const LLUUID& avatar_id) const
{
    auto it = mEditEase.find(avatar_id);
    return (it != mEditEase.end()) ? it->second : EASE_EDIT;
}

// ---------------------------------------------------------------------------
// Pose library (file format and logic from FSFloaterPoser, Firestorm)
// ---------------------------------------------------------------------------

// static
std::string SLPoseStudio::getPoseDirectory()
{
    return gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, POSE_SAVE_SUBDIRECTORY);
}

// static
std::string SLPoseStudio::getPosePath(const std::string& pose_name)
{
    return gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, POSE_SAVE_SUBDIRECTORY, pose_name + POSE_INTERNAL_FORMAT_FILE_EXT);
}

void SLPoseStudio::createUserPoseDirectoryIfNeeded()
{
    std::string userPath = getPoseDirectory();
    std::string userHandPresetsPath = userPath + gDirUtilp->getDirDelimiter() + POSE_PRESETS_HANDS_SUBDIRECTORY;

    try
    {
        if (!gDirUtilp->fileExists(userPath))
        {
            LL_WARNS("Poser") << "Couldn't find folder: " << userPath << " - creating one." << LL_ENDL;
            LLFile::mkdir(userPath);
        }
        if (!gDirUtilp->fileExists(userHandPresetsPath))
        {
            LL_WARNS("Poser") << "Couldn't find folder: " << userHandPresetsPath << " - creating one." << LL_ENDL;
            LLFile::mkdir(userHandPresetsPath);
        }

        std::string sourcePresetPath =
            gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, POSE_SAVE_SUBDIRECTORY, POSE_PRESETS_HANDS_SUBDIRECTORY);
        if (!gDirUtilp->fileExists(sourcePresetPath))
        {
            LL_WARNS("Poser") << "Can not copy poser presets because failed to find path: " << sourcePresetPath << LL_ENDL;
            return;
        }

        auto posesToCopy = gDirUtilp->getFilesInDir(sourcePresetPath);
        for (const auto& pose : posesToCopy)
        {
            std::string source      = sourcePresetPath + gDirUtilp->getDirDelimiter() + pose;
            std::string destination = userHandPresetsPath + gDirUtilp->getDirDelimiter() + pose;

            S32 sourceVersion      = tryGetPoseVersion(source);
            S32 destinationVersion = tryGetPoseVersion(destination);
            if (destinationVersion >= sourceVersion)
                continue;

            if (gDirUtilp->fileExists(destination))
            {
                LL_WARNS("Poser") << "Removing pose file " << destination << " to replace with updated version " << LL_ENDL;
                LLFile::remove(destination);
            }

            if (!LLFile::copy(source, destination))
                LL_WARNS("Poser") << "Failed to copy " << source << " to " << destination << LL_ENDL;
        }
    }
    catch (const std::exception& e)
    {
        LL_WARNS("Posing") << "Exception caught trying to create/update poses: " << e.what() << LL_ENDL;
    }
}

std::vector<std::string> SLPoseStudio::listPoses() const
{
    std::vector<std::string> names;
    const std::string dir = getPoseDirectory();
    if (!gDirUtilp->fileExists(dir))
        return names;

    std::string file;
    LLDirIterator dir_iter(dir, POSE_INTERNAL_FORMAT_FILE_MASK);
    while (dir_iter.next(file))
    {
        const std::string path = gDirUtilp->add(dir, file);
        names.push_back(gDirUtilp->getBaseFileName(LLURI::unescape(path), true));
    }
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b)
              { return LLStringUtil::compareDict(a, b) < 0; });
    return names;
}

bool SLPoseStudio::poseExists(const std::string& pose_name) const
{
    return !pose_name.empty() && gDirUtilp->fileExists(getPosePath(pose_name));
}

bool SLPoseStudio::savePose(LLVOAvatar* avatar, const std::string& pose_name, F32 ease_in)
{
    if (pose_name.empty() || !avatar || !mAnimator.isPosingAvatar(avatar))
        return false;

    createUserPoseDirectoryIfNeeded();

    try
    {
        // Same XML as the classic (Firestorm) poser, version 8, always "from T-pose":
        // every saved bone carries its absolute rotation (zero base), so the file means
        // the same in either poser regardless of the animation playing when it was made.
        // Only edited bones are written; the rest are "enabled = false", which both
        // loaders treat as "leave this bone to the animations". Slipstream additions,
        // ignored by the classic poser: top-level "easeIn" and per bone "edited" and an
        // exact "targetRotation" quaternion.
        LLSD record;
        record["version"]["value"]          = POSE_FILE_VERSION;
        record["startFromTeePose"]["value"] = true;
        record["easeIn"]["value"]           = llclamp(ease_in, 0.f, MAX_EASE_IN);

        LLQuaternion target;
        LLVector3    target_pos, target_scale, rotation, position, scale;
        bool         baseRotationIsZero, userSetBaseRotationToZero;
        S32          saved = 0;

        for (const joint_t& pj : mAnimator.PoserJoints)
        {
            const std::string bone_name = pj.jointName();
            const bool edited = isBoneEdited(avatar->getID(), bone_name) && mAnimator.isPosingAvatarJoint(avatar, pj);

            record[bone_name]            = bone_name;
            record[bone_name]["enabled"] = edited;
            if (!edited)
                continue;
            if (!mAnimator.getJointTargetTransform(avatar, pj, target, target_pos, target_scale) ||
                !mAnimator.tryGetJointSaveVectors(avatar, pj, &rotation, &position, &scale, &baseRotationIsZero, &userSetBaseRotationToZero))
            {
                record[bone_name]["enabled"] = false;
                continue;
            }

            target.normalize();
            target.getEulerAngles(&rotation.mV[VX], &rotation.mV[VY], &rotation.mV[VZ]);

            record[bone_name]["edited"]                    = true;
            record[bone_name]["mirrored"]                  = false;
            record[bone_name]["jointBaseRotationIsZero"]   = true;
            record[bone_name]["userSetBaseRotationToZero"] = true;
            record[bone_name]["rotation"]                  = rotation.getValue();
            record[bone_name]["targetRotation"]            = ll_sd_from_quaternion(target);
            record[bone_name]["position"]                  = position.getValue();
            record[bone_name]["scale"]                     = scale.getValue();
            record[bone_name]["worldLocked"]               = mAnimator.getRotationIsWorldLocked(avatar, pj);
            ++saved;
        }

        llofstream file;
        file.open(getPosePath(pose_name).c_str());
        if (!file.is_open())
        {
            LL_WARNS("Poser") << "Unable to save pose!" << LL_ENDL;
            return false;
        }
        LLSDSerialize::toPrettyXML(record, file);
        file.close();
        LL_INFOS("PoseStudio") << "Saved pose " << pose_name << ": " << saved << " edited bones, ease-in " << ease_in << " s" << LL_ENDL;
    }
    catch (const std::exception& e)
    {
        LL_WARNS("Posing") << "Exception caught in saveToXml: " << e.what() << LL_ENDL;
        return false;
    }

    notifyChanged();
    return true;
}

F32 SLPoseStudio::getPoseEaseIn(const std::string& pose_name) const
{
    llifstream infile(getPosePath(pose_name).c_str());
    LLSD pose;
    if (!infile.is_open() || LLSDSerialize::fromXML(pose, infile) == LLSDParser::PARSE_FAILURE)
        return DEFAULT_EASE_IN;
    if (!pose.has("easeIn"))
        return DEFAULT_EASE_IN;
    return llclamp((F32)pose["easeIn"]["value"].asReal(), 0.f, MAX_EASE_IN);
}

bool SLPoseStudio::deletePose(const std::string& pose_name)
{
    if (!poseExists(pose_name))
        return false;
    const bool ok = LLFile::remove(getPosePath(pose_name)) == 0;
    notifyChanged();
    return ok;
}

// static
S32 SLPoseStudio::tryGetPoseVersion(const std::string& pathToPoseFile)
{
    S32 version = -1;
    if (pathToPoseFile.empty() || !gDirUtilp->fileExists(pathToPoseFile))
        return version;

    try
    {
        LLSD       pose;
        llifstream infile;

        infile.open(pathToPoseFile);
        if (!infile.is_open())
            return version;

        while (!infile.eof())
        {
            S32 lineCount = LLSDSerialize::fromXML(pose, infile);
            if (lineCount == LLSDParser::PARSE_FAILURE)
            {
                LL_WARNS("Posing") << "Failed to parse pose file: " << pathToPoseFile << LL_ENDL;
                return version;
            }

            if (pose.has("version"))
                return (S32)pose["version"]["value"].asInteger();
        }
    }
    catch (const std::exception& e)
    {
        LL_WARNS("Posing") << "Threw an exception trying read the pose file: " << pathToPoseFile << " exception: " << e.what() << LL_ENDL;
    }

    return version;
}

bool SLPoseStudio::poseFileStartsFromTeePose(const std::string& pose_name) const
{
    const std::string fullPath = getPosePath(pose_name);
    if (!gDirUtilp->fileExists(fullPath))
        return false;

    try
    {
        LLSD       pose;
        llifstream infile;

        infile.open(fullPath);
        if (!infile.is_open())
            return false;

        if (LLSDSerialize::fromXML(pose, infile) == LLSDParser::PARSE_FAILURE)
            return false;

        return pose.has("startFromTeePose") && pose["startFromTeePose"]["value"].asBoolean();
    }
    catch (const std::exception& e)
    {
        LL_WARNS("Posing") << "Unable to load or parse the pose: " << pose_name << " exception: " << e.what() << LL_ENDL;
    }

    return false;
}

bool SLPoseStudio::loadPose(LLVOAvatar* avatar, const std::string& pose_name, E_LoadPoseMethods method, F32 ease_in)
{
    if (!canPose(avatar))
        return false;
    if (!mAnimator.isPosingAvatar(avatar) && !startPosing(avatar))
        return false;
    if (ease_in < 0.f)
        ease_in = getPoseEaseIn(pose_name);

    // Where every bone is right now (what is on screen), for the ease-in.
    cancelBlend(avatar->getID());
    const blend_start_t from = captureBlendStart(avatar);

    std::set<std::string> loaded;
    const bool ok = loadPoseFromFile(avatar, pose_name, method, loaded);
    if (!ok)
        return false;

    // The pose is exactly the bones in the file; every other bone goes back to the
    // animations (or holds its current frame with Freeze on).
    mEdited[avatar->getID()] = loaded;
    // A loaded pose shows the same dials whatever was dialled before it: decoded afresh.
    mDialAngles.erase(avatar->getID());
    for (const joint_t& joint : mAnimator.PoserJoints)
    {
        if (!loaded.count(joint.jointName()))
            holdOrRelease(avatar, joint);
    }

    ease_in = llclamp(ease_in, 0.f, MAX_EASE_IN);
    startBlend(avatar, from, loaded, ease_in, true);

    // The ease goes with the edit: pose sharing tells viewers that see this pose to ease
    // into it over the same time.
    notifyEdited(avatar, true, ease_in > 0.01f ? ease_in : 0.f);
    notifyChanged();
    return true;
}

SLPoseStudio::blend_start_t SLPoseStudio::captureBlendStart(LLVOAvatar* avatar) const
{
    blend_start_t from;
    if (!avatar)
        return from;
    for (const joint_t& joint : mAnimator.PoserJoints)
    {
        LLJoint* live = avatar->getJoint(joint.jointName());
        if (!live || joint.boneType() == COL_VOLUMES)
            continue;
        BoneBlend& b = from[joint.jointName()];
        b.mJoint = &joint;
        b.mFromRot = live->getRotation();
        b.mFromPos = live->getPosition();
        b.mFromScale = live->getScale();
    }
    return from;
}

void SLPoseStudio::startBlend(LLVOAvatar* avatar, const blend_start_t& from, const std::set<std::string>& bones, F32 duration, bool notify_when_done)
{
    if (!avatar)
        return;
    auto existing = mBlends.find(avatar->getID());
    if (duration <= 0.01f)
    {
        // Instant: the bones stay at their new targets; an older blend must not move them.
        if (existing != mBlends.end())
        {
            for (const std::string& name : bones)
                existing->second.mBones.erase(name);
            if (existing->second.mBones.empty())
                mBlends.erase(existing);
        }
        return;
    }

    Blend& blend = mBlends[avatar->getID()];
    for (const std::string& name : bones)
    {
        blend.mBones.erase(name);
        auto it = from.find(name);
        if (it == from.end())
            continue;
        BoneBlend b = it->second;
        if (!mAnimator.getJointTargetTransform(avatar, *b.mJoint, b.mToRot, b.mToPos, b.mToScale))
            continue;
        // Start where the bone is; stepBlends moves it to the pose.
        mAnimator.setJointTargetTransform(avatar, *b.mJoint, b.mFromRot, b.mFromPos, b.mFromScale);
        b.mDuration = duration;
        b.mTimer.reset();
        blend.mBones[name] = b;
    }
    blend.mNotify |= notify_when_done;
    if (blend.mBones.empty())
    {
        mBlends.erase(avatar->getID());
        return;
    }
    if (!mBlendIdleRegistered)
    {
        mBlendIdleRegistered = true;
        gIdleCallbacks.addFunction(&SLPoseStudio::onIdleBlend, this);
    }
}

void SLPoseStudio::cancelBlend(const LLUUID& avatar_id)
{
    mBlends.erase(avatar_id);
}

void SLPoseStudio::finishBlend(LLVOAvatar* avatar)
{
    if (!avatar)
        return;
    auto it = mBlends.find(avatar->getID());
    if (it == mBlends.end())
        return;
    for (const auto& [name, b] : it->second.mBones)
        mAnimator.setJointTargetTransform(avatar, *b.mJoint, b.mToRot, b.mToPos, b.mToScale);
    mBlends.erase(it);
}

F32 SLPoseStudio::getEditEase() const
{
    static LLCachedControl<F32> ease(gSavedSettings, "SLPoseStudioEditEase", 0.3f);
    return llclamp((F32)ease, 0.f, MAX_EASE_IN);
}

void SLPoseStudio::beginEdit(LLVOAvatar* avatar, bool committed)
{
    if (mEditDepth++ > 0)
        return;     // part of an edit already under way
    mEditEasing = false;
    mEditFrom.clear();
    mEditBefore.clear();
    // Eases: a committed edit, not the release of a live drag, with an ease set.
    if (!avatar || !committed || mPreviewing.count(avatar->getID()) || getEditEase() <= 0.01f)
        return;
    mEditEasing = true;
    mEditFrom = captureBlendStart(avatar);
    mEditBefore = captureBoneStates(avatar);
}

void SLPoseStudio::endEdit(LLVOAvatar* avatar)
{
    if (mEditDepth <= 0 || --mEditDepth > 0)
        return;
    if (!mEditEasing || !avatar)
        return;
    mEditEasing = false;
    // The bones whose targets this edit changed (mirror partners, followers, pins an IK
    // hold re-solved inside the edit included) ease there from what was on screen.
    std::set<std::string> changed;
    const std::vector<BoneState> after = captureBoneStates(avatar);
    for (const BoneState& now : after)
    {
        for (const BoneState& was : mEditBefore)
        {
            if (was.mJoint != now.mJoint)
                continue;
            if (llabs(dot(was.mRotation, now.mRotation)) < 0.99999f || (was.mPosition - now.mPosition).length() > 1.0e-5f
                || (was.mScale - now.mScale).length() > 1.0e-5f)
                changed.insert(now.mJoint);
            break;
        }
    }
    const blend_start_t from = std::move(mEditFrom);
    mEditFrom.clear();
    mEditBefore.clear();
    if (!changed.empty())
        startBlend(avatar, from, changed, getEditEase(), true);
}

void SLPoseStudio::cancelBoneBlend(const LLUUID& avatar_id, const joint_t& joint)
{
    auto it = mBlends.find(avatar_id);
    if (it == mBlends.end())
        return;
    it->second.mBones.erase(joint.jointName());
    if (it->second.mBones.empty())
        mBlends.erase(it);
}

bool SLPoseStudio::getBlendTarget(LLVOAvatar* avatar, const joint_t& joint, LLQuaternion& rotation, LLVector3& position, LLVector3& scale) const
{
    if (!avatar)
        return false;
    auto it = mBlends.find(avatar->getID());
    if (it == mBlends.end())
        return false;
    auto bone = it->second.mBones.find(joint.jointName());
    if (bone == it->second.mBones.end())
        return false;

    // The blend works on target position / scale (public + base); the joint's base does
    // not change during a blend, so the end state's public values follow from the current.
    LLQuaternion cur_rot;
    LLVector3 cur_pos, cur_scale;
    if (!mAnimator.getJointTargetTransform(avatar, joint, cur_rot, cur_pos, cur_scale))
        return false;
    const BoneBlend& b = bone->second;
    rotation = b.mToRot;
    position = b.mToPos - (cur_pos - mAnimator.getJointPosition(avatar, joint));
    scale = b.mToScale - (cur_scale - mAnimator.getJointScale(avatar, joint));
    return true;
}

F32 SLPoseStudio::getBlendRemaining(const LLUUID& avatar_id) const
{
    auto it = mBlends.find(avatar_id);
    if (it == mBlends.end())
        return 0.f;
    F32 remaining = 0.f;
    for (const auto& [name, b] : it->second.mBones)
        remaining = llmax(remaining, b.mDuration - b.mTimer.getElapsedTimeF32().value());
    return remaining;
}

// static
void SLPoseStudio::onIdleBlend(void* data)
{
    static_cast<SLPoseStudio*>(data)->stepBlends();
}

void SLPoseStudio::stepBlends()
{
    for (auto it = mBlends.begin(); it != mBlends.end();)
    {
        Blend& blend = it->second;
        LLVOAvatar* avatar = nullptr;
        for (LLCharacter* character : LLCharacter::sInstances)
        {
            if (character && character->getID() == it->first)
            {
                avatar = dynamic_cast<LLVOAvatar*>(character);
                break;
            }
        }
        if (!avatar || avatar->isDead() || !mAnimator.isPosingAvatar(avatar))
        {
            it = mBlends.erase(it);
            continue;
        }

        // Smoothstep: eases in and out, so a looping sequence flows from pose to pose.
        for (auto bone = blend.mBones.begin(); bone != blend.mBones.end();)
        {
            const BoneBlend& b = bone->second;
            const F32 t = llclamp(b.mTimer.getElapsedTimeF32() / llmax(b.mDuration, 0.01f), 0.f, 1.f);
            const F32 s = t * t * (3.f - 2.f * t);
            const LLQuaternion rot = slerp(s, b.mFromRot, b.mToRot);
            const LLVector3 pos = lerp(b.mFromPos, b.mToPos, s);
            const LLVector3 scale = lerp(b.mFromScale, b.mToScale, s);
            mAnimator.setJointTargetTransform(avatar, *b.mJoint, rot, pos, scale);
            bone = (t >= 1.f) ? blend.mBones.erase(bone) : std::next(bone);
        }
        if (blend.mBones.empty())
        {
            const bool notify = blend.mNotify;
            it = mBlends.erase(it);
            if (notify)
            {
                mBlendSettling = true;
                notifyEdited(avatar, true, EASE_KEEP);  // the pose is reached: refresh dials
                mBlendSettling = false;
            }
        }
        else
        {
            ++it;
        }
    }

    if (mBlends.empty() && mBlendIdleRegistered)
    {
        mBlendIdleRegistered = false;
        gIdleCallbacks.deleteFunction(&SLPoseStudio::onIdleBlend, this);
    }
}

bool SLPoseStudio::loadPoseFromFile(LLVOAvatar* avatar, const std::string& poseFileName, E_LoadPoseMethods loadMethod,
                                    std::set<std::string>& loaded)
{
    bool loadSuccess = false;
    if (!gDirUtilp->fileExists(getPoseDirectory()))
        return loadSuccess;

    if (!mAnimator.isPosingAvatar(avatar))
        return loadSuccess;

    const std::string fullPath = getPosePath(poseFileName);

    bool loadRotations = loadMethod == ROTATIONS || loadMethod == ROTATIONS_AND_POSITIONS || loadMethod == ROTATIONS_AND_SCALES ||
                         loadMethod == ROT_POS_AND_SCALES || loadMethod == SELECTIVE || loadMethod == SELECTIVE_ROT;
    bool loadPositions = loadMethod == POSITIONS || loadMethod == ROTATIONS_AND_POSITIONS || loadMethod == POSITIONS_AND_SCALES ||
                         loadMethod == ROT_POS_AND_SCALES || loadMethod == SELECTIVE;
    bool loadScales    = loadMethod == SCALES || loadMethod == POSITIONS_AND_SCALES || loadMethod == ROTATIONS_AND_SCALES ||
                         loadMethod == ROT_POS_AND_SCALES || loadMethod == SELECTIVE;
    bool loadSelective = loadMethod == SELECTIVE || loadMethod == SELECTIVE_ROT;

    try
    {
        LLSD       pose;
        llifstream infile;
        LLVector3  vec3;
        bool       enabled;
        bool       setJointBaseRotationToZero;
        bool       userSetBaseRotationToZero;
        bool       worldLocked;
        bool       mirroredJoint;
        S32        version          = 0;
        bool       startFromZeroRot = true;

        infile.open(fullPath);
        if (!infile.is_open())
            return loadSuccess;

        loadSuccess = true;
        while (!infile.eof())
        {
            S32 lineCount = LLSDSerialize::fromXML(pose, infile);
            if (lineCount == LLSDParser::PARSE_FAILURE)
            {
                LL_WARNS("Posing") << "Failed to parse file: " << poseFileName << LL_ENDL;
                return loadSuccess;
            }

            for (LLSD::map_const_iterator itr = pose.beginMap(); itr != pose.endMap(); ++itr)
            {
                std::string const& name        = itr->first;
                LLSD const&        control_map = itr->second;

                if (name == "startFromTeePose")
                    startFromZeroRot = control_map["value"].asBoolean();

                if (name == "version")
                    version = (S32)control_map["value"].asInteger();
            }

            if (version > 5 && startFromZeroRot)
                mAnimator.setAllAvatarStartingRotationsToZero(avatar);

            bool loadPositionsAndScalesAsDeltas = version > 3;

            for (LLSD::map_const_iterator itr = pose.beginMap(); itr != pose.endMap(); ++itr)
            {
                std::string const& name        = itr->first;
                LLSD const&        control_map = itr->second;

                const joint_t* poserJoint = mAnimator.getPoserJointByName(name);
                if (!poserJoint)
                    continue;

                if (loadSelective && mAnimator.isPosingAvatarJoint(avatar, *poserJoint))
                    continue;

                if (control_map.has("enabled"))
                {
                    enabled = control_map["enabled"].asBoolean();
                    mAnimator.setPosingAvatarJoint(avatar, *poserJoint, enabled || loadSelective);
                }

                setJointBaseRotationToZero = control_map.has("jointBaseRotationIsZero") ? control_map["jointBaseRotationIsZero"].asBoolean()
                                                                                         : startFromZeroRot;
                userSetBaseRotationToZero = control_map.has("userSetBaseRotationToZero") ? control_map["userSetBaseRotationToZero"].asBoolean()
                                                                                          : startFromZeroRot;

                if (loadPositions && control_map.has("position"))
                    vec3.setValue(control_map["position"]);
                else
                    vec3.clear();
                mAnimator.loadJointPosition(avatar, poserJoint, loadPositionsAndScalesAsDeltas, vec3);

                if (loadRotations && control_map.has("rotation"))
                    vec3.setValue(control_map["rotation"]);
                else
                    vec3.clear();
                mAnimator.loadJointRotation(avatar, poserJoint, setJointBaseRotationToZero, userSetBaseRotationToZero, vec3);

                if (loadScales && control_map.has("scale"))
                    vec3.setValue(control_map["scale"]);
                else
                    vec3.clear();
                mAnimator.loadJointScale(avatar, poserJoint, loadPositionsAndScalesAsDeltas, vec3);

                worldLocked = control_map.has("worldLocked") ? control_map["worldLocked"].asBoolean() : false;
                mAnimator.setRotationIsWorldLocked(avatar, *poserJoint, worldLocked);

                mirroredJoint = control_map.has("mirrored") ? control_map["mirrored"].asBoolean() : false;
                mAnimator.setRotationIsMirrored(avatar, *poserJoint, mirroredJoint);

                // Slipstream files carry the exact rotation as well (no Euler round trip).
                if (loadRotations && control_map.has("targetRotation"))
                {
                    LLQuaternion exact = ll_quaternion_from_sd(control_map["targetRotation"]);
                    exact.normalize();
                    mAnimator.setJointTargetRotation(avatar, *poserJoint, exact);
                }

                // Which bones make up the pose: Slipstream files say so; for older files a
                // bone counts when it is enabled and moved at all (a bone listed at rest is
                // left to the animations).
                bool edited;
                if (control_map.has("edited"))
                {
                    edited = control_map["edited"].asBoolean();
                }
                else
                {
                    const bool enabled_bone = !control_map.has("enabled") || control_map["enabled"].asBoolean();
                    LLVector3 r, p, sc;
                    if (control_map.has("rotation"))
                        r.setValue(control_map["rotation"]);
                    if (control_map.has("position"))
                        p.setValue(control_map["position"]);
                    if (control_map.has("scale"))
                        sc.setValue(control_map["scale"]);
                    edited = enabled_bone && (!r.isExactlyZero() || !p.isExactlyZero() || !sc.isExactlyZero());
                }
                if (edited)
                    loaded.insert(name);
            }

            if (version > 6 && !startFromZeroRot && !loadSelective)
                loadSuccess = mAnimator.loadPosingState(avatar, true, pose);
        }
    }
    catch (const std::exception& e)
    {
        loadSuccess = false;
        LL_WARNS("Posing") << "Everything caught fire trying to load the pose: " << poseFileName << " exception: " << e.what() << LL_ENDL;
    }

    return loadSuccess;
}

void SLPoseStudio::loadPoseRetrying(const std::string& pose_name, E_LoadPoseMethods method)
{
    mRetryPose.clear();
    if (loadPose(getTarget(), pose_name, method))
        return;
    mRetryPose = pose_name;
    mRetryMethod = method;
    mRetryAttempts = 0;
    mRetryTimer.reset();
    ensureTicker();
}

// ---------------------------------------------------------------------------
// Sequence
// ---------------------------------------------------------------------------

void SLPoseStudio::loadSequence()
{
    mSequence.clear();
    llifstream infile(sequencePath().c_str());
    if (!infile.is_open())
        return;

    LLSD data;
    if (LLSDSerialize::fromXML(data, infile) == LLSDParser::PARSE_FAILURE || !data.isArray())
        return;

    for (const LLSD& entry : llsd::inArray(data))
    {
        SequenceEntry e;
        e.mPose = entry["pose"].asString();
        e.mHold = llmax(MIN_HOLD, (F32)entry["hold"].asReal());
        e.mEase = entry.has("ease") ? llmin((F32)entry["ease"].asReal(), MAX_EASE_IN) : -1.f;
        if (!e.mPose.empty())
            mSequence.push_back(e);
    }
}

void SLPoseStudio::saveSequence() const
{
    LLSD data = LLSD::emptyArray();
    for (const SequenceEntry& e : mSequence)
    {
        LLSD entry;
        entry["pose"] = e.mPose;
        entry["hold"] = e.mHold;
        entry["ease"] = e.mEase;
        data.append(entry);
    }
    llofstream file(sequencePath().c_str());
    if (file.is_open())
        LLSDSerialize::toPrettyXML(data, file);
}

void SLPoseStudio::addToSequence(const std::string& pose_name, F32 hold)
{
    if (pose_name.empty())
        return;
    mSequence.push_back({ pose_name, llmax(MIN_HOLD, hold), -1.f });
    saveSequence();
    notifyChanged();
}

void SLPoseStudio::removeFromSequence(S32 index)
{
    if (index < 0 || index >= (S32)mSequence.size())
        return;
    mSequence.erase(mSequence.begin() + index);
    if (mSequenceIndex >= (S32)mSequence.size())
        mSequenceIndex = (S32)mSequence.size() - 1;
    if (mSequence.empty())
        stopPlayback();
    saveSequence();
    notifyChanged();
}

void SLPoseStudio::moveSequenceEntry(S32 index, S32 delta)
{
    const S32 to = index + delta;
    if (index < 0 || index >= (S32)mSequence.size() || to < 0 || to >= (S32)mSequence.size())
        return;
    std::swap(mSequence[index], mSequence[to]);
    saveSequence();
    notifyChanged();
}

void SLPoseStudio::setSequenceHold(S32 index, F32 hold)
{
    if (index < 0 || index >= (S32)mSequence.size())
        return;
    mSequence[index].mHold = llmax(MIN_HOLD, hold);
    saveSequence();
    notifyChanged();
}

void SLPoseStudio::setSequenceEase(S32 index, F32 ease)
{
    if (index < 0 || index >= (S32)mSequence.size())
        return;
    mSequence[index].mEase = (ease < 0.f) ? -1.f : llmin(ease, MAX_EASE_IN);
    saveSequence();
    notifyChanged();
}

void SLPoseStudio::clearSequence()
{
    stopPlayback();
    mSequence.clear();
    mSequenceIndex = -1;
    saveSequence();
    notifyChanged();
}

bool SLPoseStudio::getLoop() const
{
    static LLCachedControl<bool> loop(gSavedSettings, "SLPoseStudioLoop", true);
    return loop;
}

void SLPoseStudio::setLoop(bool loop)
{
    gSavedSettings.setBOOL("SLPoseStudioLoop", loop);
    notifyChanged();
}

void SLPoseStudio::play()
{
    if (mSequence.empty())
        return;
    mPlaying = true;
    if (mSequenceIndex < 0 || mSequenceIndex >= (S32)mSequence.size())
        mSequenceIndex = 0;
    loadPose(getTarget(), mSequence[mSequenceIndex].mPose, ROT_POS_AND_SCALES, mSequence[mSequenceIndex].mEase);
    startSequenceTimer(mSequence[mSequenceIndex].mHold);
    notifyChanged();
}

void SLPoseStudio::stopPlayback()
{
    mPlaying = false;
    notifyChanged();
}

void SLPoseStudio::stepSequence(S32 delta)
{
    if (mSequence.empty())
        return;
    const S32 count = (S32)mSequence.size();
    S32 next = (mSequenceIndex < 0) ? 0 : mSequenceIndex + delta;
    if (next >= count || next < 0)
    {
        if (!getLoop() && mPlaying)
        {
            stopPlayback();
            return;
        }
        next = (next % count + count) % count;
    }
    mSequenceIndex = next;
    loadPose(getTarget(), mSequence[mSequenceIndex].mPose, ROT_POS_AND_SCALES, mSequence[mSequenceIndex].mEase);
    if (mPlaying)
        startSequenceTimer(mSequence[mSequenceIndex].mHold);
    notifyChanged();
}

void SLPoseStudio::startSequenceTimer(F32 seconds)
{
    mSequenceHold = llmax(MIN_HOLD, seconds);
    mSequenceTimer.reset();
    ensureTicker();
}
