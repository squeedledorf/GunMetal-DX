/**
 * @file slposelookat.cpp
 * @brief Pose Studio look-at sync (#902). See slposelookat.h for the wire format.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
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

#include "slposelookat.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llcharacter.h"
#include "lldir.h"
#include "lleventtimer.h"
#include "llhudeffectlookat.h"
#include "llinventorymodel.h"
#include "lljointstate.h"
#include "llmutelist.h"
#include "llsdserialize.h"
#include "llskinningutil.h"
#include "llviewercontrol.h"
#include "llviewerinventory.h"
#include "llviewerjointattachment.h"
#include "llviewerobjectlist.h"
#include "llviewercamera.h"
#include "llviewerregion.h"
#include "llvoavatarself.h"
#include "llvolume.h"
#include "llvovolume.h"
#include "llvrcamera.h"
#include "rlvactions.h"
#include "slaoengine.h"
#include "slbridge.h"
#include "slposemath.h"
#include "slposestudio.h"
#include "slposesync.h"

#include <algorithm>
#include <cctype>
#include <cmath>

// A fixed id for the layer's motion on every avatar (never sent to the sim: the motion is
// started and stopped locally only).
const LLUUID SLPoseLookAt::ANIM_ID("f0722b41-30d8-4bf7-86f7-e88aa1d8d5b1");

namespace
{
constexpr char BRIDGE_USER[]        = "lookat";
constexpr F32  TICK_PERIOD          = 0.1f;
constexpr F32  MIN_SEND_INTERVAL    = 0.5f;     // at most two lines a second
constexpr F32  KEEPALIVE_INTERVAL   = 5.f;      // while looking at something
constexpr F32  SEND_ANGLE           = 2.f * DEG_TO_RAD;
constexpr F32  SETTLE_ANGLE         = 0.75f * DEG_TO_RAD;   // a smaller miss is re-sent once the
constexpr F32  SETTLE_INTERVAL      = 1.5f;                 // look has been still for this long
constexpr F32  FAST_PERIOD          = 1.f / 60.f;   // own-avatar sampling (local only, no traffic)
constexpr F32  MAX_EASE             = 5.f;
constexpr F32  EASE_OMEGA           = 5.f;      // spring rate = this / ease: ~96% done after one ease
constexpr F32  KNEE                 = 0.75f;    // the head starts handing over at this share of its limit
constexpr F32  SEND_WEIGHT          = 0.05f;
constexpr size_t MAX_QUEUED_BEFORE_SEND = 1;    // bridge lines still waiting: hold the look-at
constexpr F32  STALE_AFTER          = 15.f;     // three missed keep-alives: fade out
constexpr F32  FADE_TIME            = 1.f;
constexpr F32  FORGET_AFTER         = STALE_AFTER + FADE_TIME + 5.f;
constexpr F32  MIN_TARGET_DISTANCE  = 0.3f;     // LL's MIN_HEAD_LOOKAT_DISTANCE
constexpr F32  BEHIND_START         = 120.f * DEG_TO_RAD;  // a target behind the head fades the turn
constexpr F32  BEHIND_END           = 165.f * DEG_TO_RAD;  // ... out, so it never flips side to side
constexpr F32  RATE_BURST           = 6.f;      // lines per sender, then
constexpr F32  RATE_PER_SECOND      = 4.f;      // this many a second
constexpr size_t MAX_ENTRIES        = 64;
constexpr size_t MAX_MUTED          = 500;
constexpr size_t MAX_DATA_CHARS     = 64;       // base64url of the largest valid record (32 bytes)
constexpr F32  DETECT_INTERVAL      = 5.f;
constexpr U8   WIRE_VERSION         = 1;
constexpr S32  HEADER_BYTES         = 8;
// Motion smoothing (time constant, seconds).
constexpr F32  OWN_TAU              = 0.15f;
constexpr F32  VR_EASE              = 0.1f;     // #1121: our own head on the headset (seconds)
constexpr F32  MIN_PIXEL_AREA       = 500.f;    // as LL's head_rot: far avatars skip it
constexpr F32  STATUS_INTERVAL      = 10.f;     // #PoseLookAt status line while in use
constexpr size_t MAX_STATUS_ENTRIES = 8;        // remote entries described per status line
constexpr size_t MAX_REJECT_SENDERS = 32;       // senders whose dropped lines are counted
constexpr F32  STUCK_WEIGHT         = 0.2f;     // a drive at least this strong ...
constexpr F32  STUCK_APPLIED        = 0.05f;    // ... that applies less than this ...
constexpr F32  STUCK_AFTER          = 3.f;      // ... for this long is a stuck motion
constexpr U32  MAX_RESTARTS         = 5;        // watchdog restarts per entry, then only logged
constexpr F32  NOT_SENDING_LOG_AFTER = 30.f;    // a reason not to send, held this long, is logged
constexpr F32  NOT_SENDING_RELOG    = 300.f;    // ... and again at most this often

// LL's look-at types, in ELookAtType order (the names in llhudeffectlookat.cpp are private).
const char* lookAtTypeName(S32 type)
{
    static const char* const names[] = { "none", "idle", "auto_listen", "freelook", "respond", "hover",
                                         "conversation", "select", "focus", "mouselook", "clear" };
    if (type < 0)
        return "no_effect";
    return (type < (S32)LL_ARRAY_SIZE(names)) ? names[type] : "unknown";
}

std::string lowerCase(std::string text)
{
    LLStringUtil::toLower(text);
    return text;
}

// Joints the worn rigged meshes weight (by joint number), read from each face's rigging
// table. The face tables are filled from the vertex weights before the mesh data is
// released, so this does not depend on the avatar's cached rigging key (which stays the
// same when a mesh's skin arrives after the first look, leaving the avatar table empty).
S32 collectRiggedJoints(LLVOAvatar* avatar, std::vector<bool>& rigged)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;
    rigged.assign(LL_CHARACTER_MAX_ANIMATED_JOINTS, false);
    if (!avatar)
        return 0;
    std::vector<LLVOVolume*> volumes;
    avatar->getAssociatedVolumes(volumes);
    for (LLVOVolume* vol : volumes)
    {
        // Animesh attachments rig to their own skeleton, not this one.
        if (!vol || vol->isDead() || vol->isAnimatedObject() || !vol->isRiggedMesh())
            continue;
        LLVolume* volume = vol->getVolume();
        if (!volume)
            continue;
        const LLMeshSkinInfo* skin = vol->getSkinInfo();
        for (S32 f = 0; f < volume->getNumVolumeFaces(); ++f)
        {
            LLVolumeFace& face = volume->getVolumeFace(f);
            if (skin && face.mJointRiggingInfoTab.needsUpdate())
                LLSkinningUtil::updateRiggingInfo(skin, avatar, face);     // no-op without the arrays
            const LLJointRiggingInfoTab& tab = face.mJointRiggingInfoTab;
            const S32 n = llmin(tab.size(), (S32)rigged.size());
            for (S32 j = 0; j < n; ++j)
            {
                if (tab[j].isRiggedTo())
                    rigged[j] = true;
            }
        }
    }
    S32 count = 0;
    for (bool r : rigged)
        count += r ? 1 : 0;
    return count;
}

// The names of the items worn on this avatar (attachments and HUDs), lower case.
void collectWornNames(LLVOAvatar* avatar, std::vector<std::string>& names)
{
    names.clear();
    if (!avatar)
        return;
    for (const auto& point : avatar->mAttachmentPoints)
    {
        const LLViewerJointAttachment* attachment = point.second;
        if (!attachment)
            continue;
        for (const LLPointer<LLViewerObject>& object : attachment->mAttachedObjects)
        {
            if (object.isNull() || object->isDead())
                continue;
            const LLViewerInventoryItem* item = gInventory.getItem(object->getAttachmentItemID());
            if (item)
                names.push_back(lowerCase(item->getName()));
        }
    }
}

class SLPoseLookAtTicker : public LLEventTimer
{
public:
    SLPoseLookAtTicker(F32 period, std::function<void()> cb) : LLEventTimer(period), mCallback(std::move(cb)) {}
    bool tick() override
    {
        if (mCallback)
            mCallback();
        return false;
    }

private:
    std::function<void()> mCallback;
};

bool validId(const std::string& id)
{
    if (id.empty() || id.size() > SLPoseLookAt::MAX_MAP_ID)
        return false;
    for (char c : id)
    {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
            return false;
    }
    return true;
}

bool validJointName(const std::string& name)
{
    if (name.size() < 2 || name.size() > 64 || name[0] != 'm')
        return false;
    for (char c : name)
    {
        if (!isalnum((unsigned char)c) && c != '_')
            return false;
    }
    return true;
}

F32 limitDegrees(const LLSD& value, F32 fallback)
{
    const F32 deg = value.isDefined() ? (F32)value.asReal() : fallback;
    return std::isfinite(deg) ? llclamp(deg, 0.f, 180.f) * DEG_TO_RAD : 0.f;
}

F32 share(const LLSD& value)
{
    const F32 v = (F32)value.asReal();
    return std::isfinite(v) ? llclamp(v, 0.f, 1.f) : 0.f;
}

F32 smoothing(F32 dt, F32 tau)
{
    return (dt > 0.f) ? 1.f - expf(-dt / tau) : 0.f;
}

F32 localEase()
{
    static LLCachedControl<F32> ease(gSavedSettings, "SLLookAtSyncEase", 1.f);
    const F32 v = ease;
    return std::isfinite(v) ? llclamp(v, 0.f, MAX_EASE) : 1.f;
}

// Wire form of an ease: 0 = none sent, else 1 + 50 ms steps of 20 ms.
U8 easeCode(F32 seconds)
{
    return (U8)(1 + llclamp(ll_round(seconds / 0.02f), 0, 254));
}

F32 wrapAngle(F32 a)
{
    a = fmodf(a + F_PI, F_TWO_PI);
    if (a < 0.f)
        a += F_TWO_PI;
    return a - F_PI;
}

// Critically damped approach to a target (exact solution over dt, so frame rate does not
// matter); the velocity carries across target changes, so coarse target steps do not show
// as steps. It never passes the target. omega <= 0 snaps.
void smoothDamp(F32& cur, F32& vel, F32 target, F32 omega, F32 dt)
{
    if (dt <= 0.f)
        return;
    if (omega <= 0.f)
    {
        cur = target;
        vel = 0.f;
        return;
    }
    const F32 start = cur;
    const F32 change = cur - target;
    const F32 temp = (vel + omega * change) * dt;
    const F32 decay = expf(-omega * dt);
    vel = (vel - omega * temp) * decay;
    cur = target + (change + temp) * decay;
    if (((target - start) > 0.f) == (cur > target))
    {
        cur = target;
        vel = 0.f;
    }
}

// What a bone takes of the request still left (signed), within +-limit. Hard at the limit for
// the last bone of the chain; for the others a soft knee: all of it up to KNEE * limit, then
// tanh toward the limit, so the next bone starts taking a share before this one saturates
// and nothing snaps (value and slope are continuous at the knee).
F32 takeShare(F32 request, F32 limit, bool last)
{
    const F32 a = llabs(request);
    if (limit <= 0.f || a <= 0.f)
        return 0.f;
    F32 taken;
    if (last)
    {
        taken = llmin(a, limit);
    }
    else
    {
        const F32 knee = KNEE * limit;
        const F32 span = limit - knee;
        taken = (a <= knee) ? a : knee + span * tanhf((a - knee) / span);
    }
    return (request < 0.f) ? -taken : taken;
}

S32 defaultPriority(const std::string& joint, size_t index)
{
    // Head, then neck, then down the spine; any other bone after those, tip-ward first.
    static const char* const names[] = { "mHead", "mNeck", "mChest", "mSpine4", "mSpine3", "mSpine2", "mSpine1" };
    for (size_t i = 0; i < LL_ARRAY_SIZE(names); ++i)
    {
        if (joint == names[i])
            return (S32)i * 10;
    }
    return 100 + (S32)(SLLookAtMap::MAX_BONES - llmin(index, SLLookAtMap::MAX_BONES));
}

void putS16(std::string& out, S32 v)
{
    const U16 u = (U16)(S16)llclamp(v, -32767, 32767);
    out += (char)(u >> 8);
    out += (char)(u & 0xFF);
}

S32 getS16(const U8* p)
{
    return (S16)(((U16)p[0] << 8) | (U16)p[1]);
}

bool isAncestor(const LLJoint* ancestor, LLJoint* joint)
{
    for (LLJoint* p = joint ? joint->getParent() : nullptr; p; p = p->getParent())
    {
        if (p == ancestor)
            return true;
    }
    return false;
}

// #1121: a chain bone's tilt limit when the map gives none (radians).
F32 defaultRollLimit(const SLLookAtBone& bone)
{
    return 0.75f * llmin(bone.mPitchUpLimit, bone.mPitchDownLimit);
}

// The built-in human map, used when lookat_maps.xml is missing or has no "human".
SLLookAtMap builtinHuman()
{
    SLLookAtMap map;
    map.mId = "human";
    map.mLabel = "Human";
    auto bone = [](const char* joint, F32 yaw, F32 pitch, F32 yaw_lim, F32 up, F32 down)
    {
        SLLookAtBone b;
        b.mJoint = joint;
        b.mYawShare = yaw;
        b.mPitchShare = pitch;
        b.mYawLimit = yaw_lim * DEG_TO_RAD;
        b.mPitchUpLimit = up * DEG_TO_RAD;
        b.mPitchDownLimit = down * DEG_TO_RAD;
        b.mRollLimit = defaultRollLimit(b);
        b.mPriority = defaultPriority(joint, 0);
        return b;
    };
    map.mBones.push_back(bone("mChest", 0.08f, 0.05f, 10.f, 5.f, 5.f));
    map.mBones.push_back(bone("mNeck", 0.32f, 0.35f, 35.f, 25.f, 25.f));
    map.mBones.push_back(bone("mHead", 0.60f, 0.60f, 50.f, 35.f, 35.f));
    map.buildOrder();
    return map;
}
}

// ---------------------------------------------------------------------------
// SLLookAtMap
// ---------------------------------------------------------------------------

void SLLookAtMap::buildOrder()
{
    mOrder.clear();
    for (size_t k = 0; k < mBones.size(); ++k)
    {
        if (!mBones[k].mEye)
            mOrder.push_back(k);
    }
    // Head first, then the neck, then down the chain; ties keep the file's tip-first order.
    std::stable_sort(mOrder.begin(), mOrder.end(), [this](size_t a, size_t b)
    {
        if (mBones[a].mPriority != mBones[b].mPriority)
            return mBones[a].mPriority < mBones[b].mPriority;
        return a > b;
    });
}

bool SLLookAtMap::drives(const std::string& joint) const
{
    for (const SLLookAtBone& bone : mBones)
    {
        if (bone.mJoint == joint)
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// SLPoseLookAtMotion
// ---------------------------------------------------------------------------

SLPoseLookAtMotion::SLPoseLookAtMotion(const LLUUID& id) : LLMotion(id)
{
    mName = "sl_lookat";
}

SLPoseLookAtMotion::~SLPoseLookAtMotion()
{
}

F32 SLPoseLookAtMotion::getMinPixelArea()
{
    return MIN_PIXEL_AREA;
}

LLMotion::LLMotionInitStatus SLPoseLookAtMotion::onInitialize(LLCharacter* character)
{
    // Never STATUS_FAILURE: the motion registry is shared by every character, and a failed
    // init marks the id bad for all of them for the rest of the session. An avatar this
    // cannot drive gets a motion that does nothing (onUpdate checks).
    if (!character)
        return STATUS_SUCCESS;
    mCharacter = character;
    mAvatarID = character->getID();
    mRootJoint = character->getJoint("mRoot");
    if (!mRootJoint || !SLPoseLookAt::instanceExists())
        return STATUS_SUCCESS;

    // One joint state per bone any map can name, on this skeleton. The set never changes
    // while the motion lives: bones outside the active map hold an identity rotation, which
    // leaves them exactly as the animations put them.
    for (const std::string& name : SLPoseLookAt::instance().getAllBones())
    {
        LLJoint* joint = character->getJoint(name);
        if (!joint || joint == mRootJoint)
            continue;
        Slot slot;
        slot.mName = name;
        slot.mJoint = joint;
        slot.mState = new LLJointState;
        slot.mState->setJoint(joint);
        slot.mState->setUsage(LLJointState::ROT);
        slot.mState->setRotation(LLQuaternion::DEFAULT);
        addJointState(slot.mState);
        mSlots.push_back(slot);
    }
    return STATUS_SUCCESS;
}

bool SLPoseLookAtMotion::onActivate()
{
    mLastTime = -1.f;
    mWeight = 0.f;
    mYawVel = mPitchVel = mRollVel = mWeightVel = 0.f;
    mIdle = true;
    for (Slot& slot : mSlots)
    {
        slot.mState->setRotation(LLQuaternion::DEFAULT);
        slot.mCumLast = LLQuaternion::DEFAULT;
        slot.mOwn = 0.f;
    }
    return true;
}

void SLPoseLookAtMotion::onDeactivate()
{
    for (Slot& slot : mSlots)
    {
        slot.mState->setRotation(LLQuaternion::DEFAULT);
        slot.mCumLast = LLQuaternion::DEFAULT;
    }
    mWeight = 0.f;
    mYawVel = mPitchVel = mRollVel = mWeightVel = 0.f;
    mAppliedAngle = 0.f;
    mIdle = true;
}

std::string SLPoseLookAtMotion::describeBones() const
{
    std::string out;
    if (!mBoundMap)
        return "-";
    for (size_t k = 0; k < mBoundMap->mBones.size() && k < mAppliedYaw.size(); ++k)
    {
        out += (out.empty() ? "" : ",") + mBoundMap->mBones[k].mJoint;
        out += llformat("=%.1f/%.1f", mAppliedYaw[k] * RAD_TO_DEG, mAppliedPitch[k] * RAD_TO_DEG);
    }
    return out.empty() ? std::string("-") : out;
}

S32 SLPoseLookAtMotion::getBoundBones() const
{
    S32 n = 0;
    for (S32 slot : mMapSlot)
        n += (slot >= 0) ? 1 : 0;
    return n;
}

void SLPoseLookAtMotion::bindMap(const SLLookAtMap* map)
{
    mBoundMap = map;
    const size_t n = map ? map->mBones.size() : 0;
    mMapSlot.assign(n, -1);
    mMapParent.assign(n, -1);
    mMapHeadNeck.assign(n, false);
    for (size_t k = 0; k < n; ++k)
        mMapHeadNeck[k] = map->mBones[k].mJoint == "mHead" || map->mBones[k].mJoint == "mNeck";
    mAppliedYaw.assign(n, 0.f);
    mAppliedPitch.assign(n, 0.f);
    for (size_t k = 0; k < n; ++k)
    {
        for (size_t s = 0; s < mSlots.size(); ++s)
        {
            if (mSlots[s].mName == map->mBones[k].mJoint)
            {
                mMapSlot[k] = (S32)s;
                break;
            }
        }
    }
    // Each bone inherits the change of the nearest map bone above it in the skeleton (the
    // chain is listed root to tip; eyes hang off the head).
    for (size_t k = 0; k < n; ++k)
    {
        if (mMapSlot[k] < 0)
            continue;
        for (S32 j = (S32)k - 1; j >= 0; --j)
        {
            if (mMapSlot[j] >= 0 && isAncestor(mSlots[mMapSlot[j]].mJoint, mSlots[mMapSlot[k]].mJoint))
            {
                mMapParent[k] = j;
                break;
            }
        }
    }
    for (Slot& slot : mSlots)
        slot.mCumLast = LLQuaternion::DEFAULT;
}

bool SLPoseLookAtMotion::onUpdate(F32 time, U8* joint_mask)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;
    ++mUpdates;
    mSinceUpdate.reset();
    if (!mRootJoint || mSlots.empty())
    {
        mIdle = true;
        return true;
    }
    const F32 dt = (mLastTime < 0.f) ? 0.f : llclamp(time - mLastTime, 0.f, 0.25f);
    mLastTime = time;
    mAppliedAngle = 0.f;
    std::fill(mAppliedYaw.begin(), mAppliedYaw.end(), 0.f);
    std::fill(mAppliedPitch.begin(), mAppliedPitch.end(), 0.f);

    SLPoseLookAt::Drive drive;
    const bool have = SLPoseLookAt::instanceExists() && SLPoseLookAt::instance().getDrive(mAvatarID, drive) && drive.mMap;
    // Without a drive, keep the last map while fading out.
    if (have && drive.mMap != mBoundMap)
        bindMap(drive.mMap);

    const F32 target_weight = have ? drive.mWeight : 0.f;
    mTargetWeight = target_weight;
    // Easing (#902): a critically damped approach, in yaw / pitch space, to the latest target;
    // the weight eases the same way, so starting, stopping and strength changes fade instead
    // of popping. Runs every frame, so a target that moves every frame (our own) is followed
    // smoothly, and a coarse one (a remote line every half second) glides between updates.
    if (have)
        mEase = llclamp(drive.mEase, 0.f, MAX_EASE);
    const F32 omega = (mEase > 0.01f) ? EASE_OMEGA / mEase : 0.f;     // 0: snap
    // #1121: the head tilt -- our own VR head's, else a Slipstream VR sender's from LL's
    // look-at (zero for everyone else).
    F32 roll_to = 0.f;
    if (have)
    {
        mHeadNeckOnly = drive.mHeadNeckOnly;
        if (drive.mHasRoll)
        {
            roll_to = drive.mRoll;
        }
        else if (const F32* roll = (const F32*)mCharacter->getAnimationData(LLCharacter::ANIM_DATA_LOOK_AT_ROLL))
        {
            roll_to = *roll;
        }
        roll_to = std::isfinite(roll_to) ? llclamp(roll_to, -F_PI_BY_TWO, F_PI_BY_TWO) : 0.f;
    }
    if (have)
    {
        if (mWeight < 0.001f || dt <= 0.f)
        {
            // Fading in from nothing: start from where the target is, not from straight ahead.
            mYaw = drive.mYaw;
            mPitch = drive.mPitch;
            mRoll = roll_to;
            mYawVel = mPitchVel = mRollVel = 0.f;
        }
        else
        {
            // The short way round: +-180 degrees is one direction, not a full turn.
            const F32 yaw_to = mYaw + wrapAngle(drive.mYaw - mYaw);
            smoothDamp(mYaw, mYawVel, yaw_to, omega, dt);
            mYaw = wrapAngle(mYaw);
            smoothDamp(mPitch, mPitchVel, drive.mPitch, omega, dt);
            smoothDamp(mRoll, mRollVel, roll_to, omega, dt);
        }
    }
    smoothDamp(mWeight, mWeightVel, target_weight, omega, dt);
    if (llabs(target_weight - mWeight) < 0.001f)
    {
        mWeight = target_weight;
        mWeightVel = 0.f;
    }
    mIdle = (mWeight < 0.001f && target_weight <= 0.f);

    // Per bone: ours unless it is outside the map or a Pose Studio pose owns it.
    std::vector<F32> own_target(mSlots.size(), 0.f);
    if (mBoundMap)
    {
        for (size_t k = 0; k < mMapSlot.size(); ++k)
        {
            if (mMapSlot[k] >= 0)
                own_target[mMapSlot[k]] = (have && k < drive.mPosed.size() && drive.mPosed[k]) ? 0.f : 1.f;
        }
    }
    const F32 own_a = smoothing(dt, OWN_TAU);
    for (size_t s = 0; s < mSlots.size(); ++s)
    {
        Slot& slot = mSlots[s];
        slot.mOwn = (dt > 0.f) ? slot.mOwn + (own_target[s] - slot.mOwn) * own_a : own_target[s];
        slot.mState->setRotation(LLQuaternion::DEFAULT);
    }

    if (mWeight < 0.0005f || !mBoundMap || mMapSlot.empty())
    {
        for (Slot& slot : mSlots)
            slot.mCumLast = LLQuaternion::DEFAULT;
        return true;
    }

    const F32 limit_scale = have ? llclamp(drive.mLimitScale, 0.f, 2.f) : 1.f;
    const F32 yaw = mYaw * mWeight;
    const F32 pitch = mPitch * mWeight;
    const F32 roll = mRoll * mWeight;
    const LLQuaternion root = mRootJoint->getWorldRotation();
    const LLQuaternion inv_root = ~root;
    const std::vector<SLLookAtBone>& bones = mBoundMap->mBones;
    std::vector<LLQuaternion> cum(bones.size(), LLQuaternion::DEFAULT);

    auto apply = [&](size_t k, F32 bone_yaw, F32 bone_pitch, F32 bone_roll)
    {
        const S32 s = mMapSlot[k];
        const LLQuaternion prev = (mMapParent[k] >= 0) ? cum[mMapParent[k]] : LLQuaternion::DEFAULT;
        if (s < 0)
        {
            cum[k] = prev;
            return;
        }
        Slot& slot = mSlots[s];
        if (k < mAppliedYaw.size())
        {
            mAppliedYaw[k] = bone_yaw;
            mAppliedPitch[k] = bone_pitch;
        }
        // The bone's change in the avatar's frame: pitch about its left axis, then yaw about
        // its up axis; then the same change expressed in the world.
        // #1121: a tilt first, about the bone's forward axis.
        LLQuaternion change_avatar = LLQuaternion(-bone_pitch, LLVector3::y_axis) * LLQuaternion(bone_yaw, LLVector3::z_axis);
        if (bone_roll != 0.f)
            change_avatar = LLQuaternion(bone_roll, LLVector3::x_axis) * change_avatar;
        const LLQuaternion change_world = inv_root * change_avatar * root;
        cum[k] = prev * change_world;

        // Additive joint states apply in the bone's own frame (final local = added * animated).
        // The bone's world rotation without our layer: last frame's, less what we added then
        // (the animations below us rarely move much in one frame, and it never accumulates).
        const LLQuaternion world_without = slot.mJoint->getWorldRotation() * ~slot.mCumLast;
        const LLQuaternion before = world_without * prev;     // with the chain above us turned
        LLQuaternion added = before * change_world * ~before;
        added.normalize();
        if (!added.isFinite())
        {
            added = LLQuaternion::DEFAULT;
            cum[k] = prev;
        }
        slot.mState->setRotation(added);
        slot.mCumLast = cum[k];
        mAppliedAngle += 2.f * acosf(llclamp(llabs(added.mQ[VW]), 0.f, 1.f));
    };

    // Priority fill: the whole request goes to the head first (up to its limit, per axis);
    // only what is left over goes to the neck, then on down the chain (map order: head, neck,
    // chest, spine). Each bone but the last hands over smoothly, starting at KNEE of its limit
    // (see takeShare). A bone a pose owns, or one this skeleton lacks, has no capacity.
    // #1121: our own VR head turns only the head and neck (see Drive::mHeadNeckOnly).
    const std::vector<size_t>& order = mBoundMap->mOrder;
    auto usable = [&](size_t k)
    {
        return mMapSlot[k] >= 0 && (!mHeadNeckOnly || (k < mMapHeadNeck.size() && mMapHeadNeck[k]));
    };
    std::vector<F32> got_yaw(bones.size(), 0.f), got_pitch(bones.size(), 0.f), got_roll(bones.size(), 0.f);
    size_t last_n = 0;
    for (size_t n = 0; n < order.size(); ++n)
    {
        if (usable(order[n]))
            last_n = n;
    }
    F32 rem_yaw = yaw, rem_pitch = pitch, rem_roll = roll;
    for (size_t n = 0; n < order.size(); ++n)
    {
        const size_t k = order[n];
        if (!usable(k))
            continue;
        const SLLookAtBone& bone = bones[k];
        const F32 own = mSlots[mMapSlot[k]].mOwn;
        const bool last = (n == last_n);
        const F32 pitch_limit = (rem_pitch >= 0.f ? bone.mPitchUpLimit : bone.mPitchDownLimit) * limit_scale * own;
        got_yaw[k] = takeShare(rem_yaw, bone.mYawLimit * limit_scale * own, last);
        got_pitch[k] = takeShare(rem_pitch, pitch_limit, last);
        got_roll[k] = takeShare(rem_roll, bone.mRollLimit * limit_scale * own, last);
        rem_yaw -= got_yaw[k];
        rem_pitch -= got_pitch[k];
        rem_roll -= got_roll[k];
    }
    // Apply root to tip (a bone inherits the change of the map bone above it).
    for (size_t k = 0; k < bones.size(); ++k)
    {
        if (!bones[k].mEye)
            apply(k, got_yaw[k], got_pitch[k], got_roll[k]);
    }
    // Eyes aim with what the chain could not reach, plus their own share.
    for (size_t k = 0; k < bones.size(); ++k)
    {
        const SLLookAtBone& bone = bones[k];
        if (!bone.mEye)
            continue;
        const F32 yaw_limit = bone.mYawLimit * limit_scale;
        apply(k, llclamp(bone.mYawShare * yaw + rem_yaw, -yaw_limit, yaw_limit),
              llclamp(bone.mPitchShare * pitch + rem_pitch, -bone.mPitchDownLimit * limit_scale, bone.mPitchUpLimit * limit_scale), 0.f);
    }
    return true;
}

// ---------------------------------------------------------------------------
// SLPoseLookAt
// ---------------------------------------------------------------------------

SLPoseLookAt::SLPoseLookAt()
{
}

SLPoseLookAt::~SLPoseLookAt()
{
    delete mTicker;
    mTicker = nullptr;
    delete mFastTicker;
    mFastTicker = nullptr;
}

void SLPoseLookAt::init()
{
    if (mInitialized)
        return;
    mInitialized = true;

    loadMaps();

    mMuted.clear();
    std::vector<std::string> ids;
    LLStringUtil::getTokens(gSavedSettings.getString("SLLookAtSyncIgnored"), ids, ",");
    for (const std::string& id : ids)
    {
        LLUUID uuid;
        if (LLUUID::parseUUID(id, &uuid) && uuid.notNull() && mMuted.size() < MAX_MUTED)
            mMuted.insert(uuid);
    }

    if (!mTicker)
        mTicker = new SLPoseLookAtTicker(TICK_PERIOD, [this]() { tick(); });
    if (!mFastTicker)
        mFastTicker = new SLPoseLookAtTicker(FAST_PERIOD, [this]() { tickFast(); });
}

void SLPoseLookAt::loadMaps()
{
    mMaps.clear();
    mAllBones.clear();
    const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "posestudio", "lookat_maps.xml");
    llifstream infile(path.c_str());
    LLSD data;
    if (!infile.is_open() || LLSDSerialize::fromXML(data, infile) == LLSDParser::PARSE_FAILURE || !data.isMap() || !data["maps"].isArray())
    {
        LL_WARNS("PoseLookAt") << "Cannot read " << path << "; only the built-in human map is available" << LL_ENDL;
    }
    else
    {
        for (const LLSD& entry : llsd::inArray(data["maps"]))
        {
            SLLookAtMap map;
            map.mId = entry["id"].asString();
            if (!validId(map.mId) || findMap(map.mId))
            {
                LL_WARNS("PoseLookAt") << "Skipping look-at map with a bad or repeated id '" << map.mId << "'" << LL_ENDL;
                continue;
            }
            map.mLabel = entry.has("label") ? entry["label"].asString() : map.mId;
            map.mFigure = entry["figure"].asString();
            for (const LLSD& bone : llsd::inArray(entry["detect"]))
            {
                if (validJointName(bone.asString()))
                    map.mDetect.push_back(bone.asString());
            }
            for (const LLSD& bone : llsd::inArray(entry["detect_any"]))
            {
                if (validJointName(bone.asString()))
                    map.mDetectAny.push_back(bone.asString());
            }
            for (const LLSD& name : llsd::inArray(entry["detect_names"]))
            {
                const std::string term = lowerCase(name.asString());
                if (!term.empty() && term.size() <= 64)
                    map.mDetectNames.push_back(term);
            }
            for (const LLSD& name : llsd::inArray(entry["detect_names_exclude"]))
            {
                const std::string term = lowerCase(name.asString());
                if (!term.empty() && term.size() <= 64)
                    map.mDetectNamesExclude.push_back(term);
            }
            for (const LLSD& b : llsd::inArray(entry["bones"]))
            {
                SLLookAtBone bone;
                bone.mJoint = b["joint"].asString();
                if (!validJointName(bone.mJoint) || bone.mJoint == "mRoot" || map.drives(bone.mJoint) ||
                    map.mBones.size() >= SLLookAtMap::MAX_BONES)
                {
                    LL_WARNS("PoseLookAt") << "Skipping bone '" << bone.mJoint << "' in look-at map " << map.mId << LL_ENDL;
                    continue;
                }
                bone.mYawShare = share(b["yaw"]);
                bone.mPitchShare = share(b["pitch"]);
                bone.mYawLimit = limitDegrees(b["yaw_limit"], 0.f);
                bone.mPitchUpLimit = limitDegrees(b["pitch_up"], 0.f);
                bone.mPitchDownLimit = limitDegrees(b["pitch_down"], bone.mPitchUpLimit * RAD_TO_DEG);
                bone.mEye = b["eye"].asBoolean();
                // #1121: tilt (VR); by default three quarters of the smaller pitch limit.
                bone.mRollLimit = bone.mEye ? 0.f : limitDegrees(b["roll_limit"], defaultRollLimit(bone) * RAD_TO_DEG);
                bone.mPriority = b.has("priority") ? llclamp(b["priority"].asInteger(), 0, 1000)
                                                   : defaultPriority(bone.mJoint, map.mBones.size());
                map.mBones.push_back(bone);
            }
            if (map.mBones.empty())
            {
                LL_WARNS("PoseLookAt") << "Look-at map " << map.mId << " has no usable bones" << LL_ENDL;
                continue;
            }
            map.buildOrder();
            mMaps.push_back(map);
        }
    }
    if (!findMap("human"))
        mMaps.insert(mMaps.begin(), builtinHuman());

    std::set<std::string> seen;
    for (const SLLookAtMap& map : mMaps)
    {
        for (const SLLookAtBone& bone : map.mBones)
        {
            if (seen.insert(bone.mJoint).second)
                mAllBones.push_back(bone.mJoint);
        }
    }
    LL_INFOS("PoseLookAt") << "Loaded " << mMaps.size() << " look-at maps over " << mAllBones.size() << " bones" << LL_ENDL;
}

const SLLookAtMap* SLPoseLookAt::findMap(const std::string& id) const
{
    for (const SLLookAtMap& map : mMaps)
    {
        if (map.mId == id)
            return &map;
    }
    return nullptr;
}

std::string SLPoseLookAt::getOwnMapId() const
{
    static LLCachedControl<std::string> setting(gSavedSettings, "SLLookAtSyncMap", "auto");
    const std::string id = setting;
    if (id != "auto" && findMap(id))
        return id;
    return (!mDetectedMap.empty() && findMap(mDetectedMap)) ? mDetectedMap : std::string("human");
}

bool SLPoseLookAt::isRigged(const std::string& joint_name) const
{
    if (!isAgentAvatarValid())
        return false;
    LLVOAvatarSelf* self = gAgentAvatarp.get();
    LLJoint* joint = self->getJoint(joint_name);
    if (!joint)
        return false;
    const S32 num = joint->getJointNum();
    if (num >= 0 && num < (S32)mRigged.size() && mRigged[num])
        return true;
    return self->jointIsRiggedTo(joint);
}

void SLPoseLookAt::detectMap()
{
    // Auto: the Pose Studio figure someone picked for themselves says the most (a pony and a
    // human use the same neck and head bones, so the rigging alone cannot tell them apart);
    // then a map whose distinctive bones the worn meshes are weighted to; then a map whose
    // name terms a worn item's name carries (a pony body rigged to the classic skeleton,
    // forelegs on the arms, cannot be told from a human by its rigging); else human.
    std::string found = "human";
    std::string how = "default";
    const std::string figure = gSavedSettings.getString("SLPoseStudioFigure");
    for (const SLLookAtMap& map : mMaps)
    {
        if (!map.mFigure.empty() && map.mFigure == figure)
        {
            found = map.mId;
            how = "figure";
            break;
        }
    }
    std::string detail;
    if (isAgentAvatarValid())
    {
        LLVOAvatarSelf* self = gAgentAvatarp.get();
        self->updateRiggingInfo();     // keyed by the worn meshes and LODs: cheap when unchanged
        mRiggedCount = collectRiggedJoints(self, mRigged);
        detail = llformat("rigged_bones=%d", mRiggedCount);
        for (const SLLookAtMap& map : mMaps)
        {
            if (map.mDetect.empty() && map.mDetectAny.empty())
                continue;
            bool all = true;
            for (const std::string& name : map.mDetect)
            {
                const bool r = isRigged(name);
                detail += " " + name + (r ? "=rigged" : "=no");
                all = all && r;
            }
            bool any = map.mDetectAny.empty();
            std::string any_hit;
            for (const std::string& name : map.mDetectAny)
            {
                if (isRigged(name))
                {
                    any = true;
                    any_hit = name;
                    break;
                }
            }
            if (!map.mDetectAny.empty())
                detail += " " + map.mId + "_any=" + (any ? any_hit : std::string("none"));
            if (how == "default" && all && any)
            {
                found = map.mId;
                how = "rigging";
            }
        }
        if (how == "default")
        {
            std::vector<std::string> worn;
            collectWornNames(self, worn);
            for (const SLLookAtMap& map : mMaps)
            {
                if (map.mDetectNames.empty())
                    continue;
                for (const std::string& item : worn)
                {
                    bool excluded = false;
                    for (const std::string& term : map.mDetectNamesExclude)
                    {
                        if (item.find(term) != std::string::npos)
                        {
                            excluded = true;
                            break;
                        }
                    }
                    if (excluded)
                        continue;
                    for (const std::string& term : map.mDetectNames)
                    {
                        if (item.find(term) != std::string::npos)
                        {
                            found = map.mId;
                            how = "worn item '" + item + "'";
                            break;
                        }
                    }
                    if (how != "default")
                        break;
                }
                if (how != "default")
                    break;
            }
        }
    }
    if (found != mDetectedMap || how != mDetectedHow)
    {
        LL_INFOS("PoseLookAt") << "Look-at map for this body: " << found << " (by " << how << "; " << detail << ")" << LL_ENDL;
        const bool map_changed = (found != mDetectedMap);
        mDetectedMap = found;
        mDetectedHow = how;
        if (map_changed)
            mChangedSignal();
    }
}

std::vector<LLUUID> SLPoseLookAt::getSenders() const
{
    std::vector<LLUUID> ids;
    for (const auto& [id, entry] : mEntries)
    {
        if (id != gAgentID)
            ids.push_back(id);
    }
    return ids;
}

void SLPoseLookAt::setMuted(const LLUUID& avatar_id, bool muted)
{
    if (avatar_id.isNull() || avatar_id == gAgentID)
        return;
    if (muted)
    {
        if (mMuted.size() >= MAX_MUTED && !mMuted.count(avatar_id))
        {
            LL_WARNS("PoseLookAt") << "Look-at mute list is full (" << MAX_MUTED << ")" << LL_ENDL;
            return;
        }
        mMuted.insert(avatar_id);
        dropEntry(avatar_id);
    }
    else
    {
        mMuted.erase(avatar_id);
    }
    saveMuted();
    mChangedSignal();
}

void SLPoseLookAt::saveMuted() const
{
    std::string list;
    for (const LLUUID& id : mMuted)
        list += (list.empty() ? "" : ",") + id.asString();
    gSavedSettings.setString("SLLookAtSyncIgnored", list);
}

LLVOAvatar* SLPoseLookAt::findAvatar(const LLUUID& id) const
{
    if (id == gAgentID)
        return isAgentAvatarValid() ? gAgentAvatarp.get() : nullptr;
    LLViewerObject* object = gObjectList.findObject(id);
    if (!object || object->isDead() || !object->isAvatar())
        return nullptr;
    LLVOAvatar* avatar = static_cast<LLVOAvatar*>(object);
    if (avatar->isControlAvatar())
        return nullptr;
    return (avatar->getRegion() == gAgent.getRegion()) ? avatar : nullptr;
}

// ---------------------------------------------------------------------------
// Receiving
// ---------------------------------------------------------------------------

void SLPoseLookAt::onRemoteLine(const LLUUID& sender, const std::string& parts, const std::string& target, const std::string& data)
{
    if (!mInitialized || sender.isNull() || sender == gAgentID)
    {
        ++mRejects[REJ_NOT_INIT];
        return;
    }
    ++mRxLines;
    // Others' look-at is shown with "Show poses" (the bridge listening) and "Show look-at".
    static LLCachedControl<bool> show(gSavedSettings, "SLLookAtSyncShow", true);
    if (!show || !SLPoseSync::instance().isShowingOthers())
        return noteReject(sender, REJ_SHOW_OFF);
    // Only ever about the sender's own avatar (sim-verified identity), in one part.
    if (parts != "1/1" || target != "-" || data.size() > MAX_DATA_CHARS)
        return noteReject(sender, REJ_FORMAT);
    if (isMuted(sender) || LLMuteList::getInstance()->isMuted(sender))
        return noteReject(sender, REJ_MUTED);

    std::string bytes;
    if (!SLPoseQuant::decode64(data, bytes) || (S32)bytes.size() < HEADER_BYTES)
        return noteReject(sender, REJ_FORMAT);
    const U8* p = (const U8*)bytes.data();
    if (p[0] != WIRE_VERSION)
        return noteReject(sender, REJ_VERSION);     // a later format: ignore rather than guess
    const S32 id_len = p[7];
    if (id_len > (S32)MAX_MAP_ID || (S32)bytes.size() != HEADER_BYTES + id_len)
        return noteReject(sender, REJ_FORMAT);
    const std::string map_id = bytes.substr(HEADER_BYTES, id_len);
    if (!map_id.empty() && !validId(map_id))
        return noteReject(sender, REJ_FORMAT);

    LLVOAvatar* avatar = findAvatar(sender);
    if (!avatar)
        return noteReject(sender, REJ_NO_AVATAR);

    auto it = mEntries.find(sender);
    const bool is_new = (it == mEntries.end());
    if (is_new)
    {
        if (mEntries.size() >= MAX_ENTRIES)
            return noteReject(sender, REJ_FULL);     // enough people shown; newcomers wait until someone goes quiet
        it = mEntries.emplace(sender, Entry()).first;
        it->second.mTokens = RATE_BURST;
        it->second.mTokenTimer.reset();
    }
    Entry& entry = it->second;

    // Per-sender rate limit, on top of the helper's own budget.
    const F32 since = entry.mTokenTimer.getElapsedTimeF32();
    entry.mTokens = llmin(RATE_BURST, entry.mTokens + since * RATE_PER_SECOND);
    entry.mTokenTimer.reset();
    if (entry.mTokens < 1.f)
        return noteReject(sender, REJ_RATE);
    entry.mTokens -= 1.f;
    ++entry.mLines;

    // Every value is clamped to its range; the map is one of ours or our human map, so a
    // line can never move a bone outside a known neck / head / eye chain.
    entry.mWeight = (F32)p[6] / 255.f;
    // A "not looking" line keeps the last direction, so the head fades out where it was
    // rather than swinging to straight ahead on the way.
    if (entry.mWeight > 0.f || is_new)
    {
        entry.mYaw = llclamp((F32)getS16(p + 2) / 100.f, -180.f, 180.f) * DEG_TO_RAD;
        entry.mPitch = llclamp((F32)getS16(p + 4) / 100.f, -90.f, 90.f) * DEG_TO_RAD;
    }
    // The sender's ease, when it said (flags byte; 0 = it did not, e.g. an older viewer).
    entry.mEase = (p[1] > 0) ? llclamp((F32)(p[1] - 1) * 0.02f, 0.f, MAX_EASE) : -1.f;
    const SLLookAtMap* map = findMap(map_id);
    entry.mMap = map ? map : findMap("human");
    entry.mDeclaredMap = map_id.empty() ? std::string("-") : map_id;
    entry.mLastHeard.reset();
    updatePosed(avatar, entry);
    if (entry.mWeight > 0.f)
        ensureMotion(avatar);
    if (is_new)
        mChangedSignal();
}

F32 SLPoseLookAt::effectiveWeight(const Entry& entry) const
{
    F32 weight = entry.mWeight;
    const F32 heard = entry.mLastHeard.getElapsedTimeF32();
    if (heard > STALE_AFTER)
        weight *= llclamp(1.f - (heard - STALE_AFTER) / FADE_TIME, 0.f, 1.f);
    const F32 yaw = llabs(entry.mYaw);
    if (yaw > BEHIND_START)
        weight *= llclamp((BEHIND_END - yaw) / (BEHIND_END - BEHIND_START), 0.f, 1.f);
    return weight;
}

bool SLPoseLookAt::getDrive(const LLUUID& avatar_id, Drive& out) const
{
    auto it = mEntries.find(avatar_id);
    if (it == mEntries.end() || !it->second.mMap)
        return false;
    static LLCachedControl<F32> limit_scale(gSavedSettings, "SLLookAtSyncLimitScale", 1.f);
    const Entry& entry = it->second;
    out.mYaw = entry.mYaw;
    out.mPitch = entry.mPitch;
    out.mRoll = 0.f;
    out.mHasRoll = false;
    out.mHeadNeckOnly = false;
    if (avatar_id == gAgentID && mVRDriving)
    {
        // #1121: our own head on the headset, read live (the motion runs every frame; tickVR
        // keeps the entry) against the body as it is now, so a body turn never lags.
        out.mRoll = entry.mRoll;
        LLJoint* root = isAgentAvatarValid() ? gAgentAvatarp->getRootJoint() : nullptr;
        F32 yaw, pitch, roll;
        if (root && LLVRCamera::headAngles(root->getWorldRotation(), yaw, pitch, roll))
        {
            out.mYaw = yaw;
            out.mPitch = pitch;
            out.mRoll = roll;
        }
        out.mHasRoll = true;
        out.mHeadNeckOnly = true;
    }
    out.mWeight = effectiveWeight(entry);
    out.mEase = (entry.mEase >= 0.f) ? entry.mEase : localEase();
    out.mLimitScale = std::isfinite((F32)limit_scale) ? llclamp((F32)limit_scale, 0.f, 2.f) : 1.f;
    out.mMap = entry.mMap;
    out.mPosed = entry.mPosed;
    return true;
}

bool SLPoseLookAt::isDriving(const LLUUID& avatar_id) const
{
    auto it = mEntries.find(avatar_id);
    if (it == mEntries.end() || !it->second.mMap || effectiveWeight(it->second) <= 0.01f)
        return false;
    // Only when this map turns a bone LL's head_rot drives; a map of other bones leaves it be.
    return it->second.mMap->drives("mHead") || it->second.mMap->drives("mNeck");
}

void SLPoseLookAt::updatePosed(LLVOAvatar* avatar, Entry& entry)
{
    // A Pose Studio pose (ours, or one synced from its owner) owns its bones: the look-at
    // stays off them until the bone is released.
    entry.mPosed.fill(false);
    if (!avatar || !entry.mMap || !SLPoseStudio::instanceExists())
        return;
    SLPoseStudio& studio = SLPoseStudio::instance();
    FSPoserAnimator& animator = studio.getAnimator();
    if (!animator.isPosingAvatar(avatar))
        return;
    const std::vector<SLLookAtBone>& bones = entry.mMap->mBones;
    for (size_t k = 0; k < bones.size() && k < entry.mPosed.size(); ++k)
    {
        const SLPoseStudio::joint_t* joint = studio.findJoint(bones[k].mJoint);
        entry.mPosed[k] = joint && animator.isPosingAvatarJoint(avatar, *joint);
    }
}

void SLPoseLookAt::dropEntry(const LLUUID& avatar_id)
{
    // The motion fades out by itself (no drive); tickMotions stops it once it is idle.
    if (mEntries.erase(avatar_id))
        mChangedSignal();
}

void SLPoseLookAt::ensureMotion(LLVOAvatar* avatar)
{
    if (!avatar || avatar->isDead())
        return;
    static bool registered = false;
    if (!registered)
    {
        // The motion registry is shared by every character.
        avatar->registerMotion(ANIM_ID, SLPoseLookAtMotion::create);
        registered = true;
    }
    if (!avatar->isMotionActive(ANIM_ID))
        avatar->startMotion(ANIM_ID);
    mMotionsRunning.insert(avatar->getID());
}

// ---------------------------------------------------------------------------
// Tick: sending, pruning, motions
// ---------------------------------------------------------------------------

void SLPoseLookAt::tick()
{
    if (!isAgentAvatarValid() || !gAgent.getRegion())
        return;

    if (!mDetected || mDetectTimer.getElapsedTimeF32() > DETECT_INTERVAL)
    {
        mDetected = true;
        mDetectTimer.reset();
        detectMap();
    }

    tickSend();

    static LLCachedControl<bool> show(gSavedSettings, "SLLookAtSyncShow", true);
    const bool showing = show && SLPoseSync::instance().isShowingOthers();
    std::vector<LLUUID> gone;
    for (auto& [id, entry] : mEntries)
    {
        if (id == gAgentID)
            continue;
        if (!showing || entry.mLastHeard.getElapsedTimeF32() > FORGET_AFTER || !findAvatar(id) || isMuted(id) ||
            LLMuteList::getInstance()->isMuted(id))
        {
            gone.push_back(id);
        }
    }
    for (const LLUUID& id : gone)
        dropEntry(id);

    tickMotions();
    tickStatus();
}

void SLPoseLookAt::tickFast()
{
    // Every frame, locally only (nothing here sends): sample where we look and move our own
    // avatar's displayed target, so its head follows at the frame rate, not at the network's.
    tickVR();
    if (!mSendingNow || !isAgentAvatarValid() || !gAgent.getRegion())
        return;
    sampleSelf();

    static LLCachedControl<bool> show_self(gSavedSettings, "SLLookAtSyncShowSelf", true);
    if (show_self && mEverSent && !mVRDriving)
    {
        const bool is_new = !mEntries.count(gAgentID);
        Entry& entry = mEntries[gAgentID];
        entry.mYaw = llclamp(mLiveYaw, -F_PI, F_PI);
        entry.mPitch = llclamp(mLivePitch, -F_PI_BY_TWO, F_PI_BY_TWO);
        entry.mWeight = mLiveWeight;
        entry.mEase = -1.f;     // our own setting, live
        entry.mMap = findMap(getOwnMapId());
        if (!entry.mMap)
            entry.mMap = findMap("human");
        entry.mLastHeard.reset();
        if (is_new)
            mChangedSignal();
    }
}

void SLPoseLookAt::tickVR()
{
    // #1121: in VR (first person) our own neck and head follow the headset, relative to the
    // body, layered over the animations like the look-at (ADDITIVE_PRIORITY), whatever the
    // sending options: the entry is kept here and getDrive reads the headset live. What
    // others see goes out through LL's look-at (LLAgentCamera::updateLookAt) and, with
    // "Send where I look", the pose channel as before. Desktop: headAngles is false at once.
    F32 yaw = 0.f, pitch = 0.f, roll = 0.f;
    LLJoint* root = (isAgentAvatarValid() && gAgent.getRegion()) ? gAgentAvatarp->getRootJoint() : nullptr;
    if (root && LLVRCamera::headAngles(root->getWorldRotation(), yaw, pitch, roll))
    {
        const bool is_new = !mEntries.count(gAgentID);
        Entry& entry = mEntries[gAgentID];
        entry.mYaw = yaw;
        entry.mPitch = pitch;
        entry.mRoll = roll;
        entry.mWeight = 1.f;
        entry.mEase = VR_EASE;
        entry.mMap = findMap(getOwnMapId());
        if (!entry.mMap)
            entry.mMap = findMap("human");
        entry.mLastHeard.reset();
        if (!mVRDriving)
        {
            mVRDriving = true;
            LL_INFOS("PoseLookAt") << "VR: the avatar's head follows the headset (map " << (entry.mMap ? entry.mMap->mId : std::string("-"))
                                   << ")" << LL_ENDL;
        }
        if (is_new)
            mChangedSignal();
        return;
    }
    if (!mVRDriving)
        return;
    mVRDriving = false;
    LL_INFOS("PoseLookAt") << "VR: head follow off" << LL_ENDL;
    // The own entry goes (the layer fades out), unless "Send where I look" shows our own
    // look-at: tickFast refills it from LL's look-at on its next sample.
    static LLCachedControl<bool> show_self(gSavedSettings, "SLLookAtSyncShowSelf", true);
    if (!(mSendingNow && show_self && mEverSent) && mEntries.erase(gAgentID))
        mChangedSignal();
}

void SLPoseLookAt::sampleSelf()
{
    static LLCachedControl<F32> strength(gSavedSettings, "SLLookAtSyncStrength", 1.f);
    static LLCachedControl<bool> private_lookat(gSavedSettings, "EnableLookAtTarget", false);
    static LLCachedControl<bool> disable_lookat(gSavedSettings, "DisableLookAtAnimation", false);

    // What LL's look-at says right now. Its privacy options act before we see anything:
    // "Don't send my Look At target" (EnableLookAtTarget) and the AO holding the head make
    // LLAgentCamera::setLookAt store no target at all, so there is nothing to send; we check
    // them again anyway. We send a direction only, never the object or point looked at.
    mLLType = -1;
    mLLDistance = -1.f;
    F32 yaw = mLiveYaw, pitch = mLivePitch, weight = 0.f;
    LLHUDEffectLookAt* lookat = gAgentCamera.mLookAt.get();
    const bool held = SLAOEngine::instanceExists() && SLAOEngine::instance().holdsHead();
    if (lookat && !lookat->isDead())
    {
        // For the status line: what LL's look-at holds, whatever we make of it. The position
        // is the target less the head (LLHUDEffectLookAt::calcTargetPosition), in the region
        // frame; it is only current while the type is not NONE.
        mLLType = (S32)lookat->getLookAtType();
        mLLDistance = lookat->getTargetPos().isFinite() ? lookat->getTargetPos().length() : -1.f;
    }
    // #902: camera position and focus, and LL's target as a point, all region-local, so a
    // target that is not where the camera looks shows at a glance.
    {
        const LLViewerCamera* camera = LLViewerCamera::getInstance();
        const LLViewerRegion* region = gAgent.getRegion();
        mFreeCam = gAgentCamera.freeCameraLookAtActive();
        mCamTargetAngle = -1.f;
        if (region)
        {
            mCamPosRegion = region->getPosRegionFromAgent(camera->getOrigin());
            mCamFocusRegion = region->getPosRegionFromGlobal(gAgentCamera.getFocusGlobal());
        }
        else
        {
            mCamPosRegion.clearVec();
            mCamFocusRegion.clearVec();
        }
        mLLTargetRegion.clearVec();
        if (lookat && !lookat->isDead() && gAgentAvatarp->mHeadp)
        {
            const ELookAtType type = lookat->getLookAtType();
            const LLVector3 to_target = lookat->getTargetPos();
            if (type != LOOKAT_TARGET_NONE && type != LOOKAT_TARGET_CLEAR && to_target.isFinite())
            {
                if (region)
                {
                    mLLTargetRegion = region->getPosRegionFromAgent(gAgentAvatarp->mHeadp->getWorldPosition() + to_target);
                }
                LLVector3 dir = to_target;
                LLVector3 cam_at = camera->getAtAxis();
                if (dir.normalize() > 0.001f && cam_at.normalize() > 0.001f)
                {
                    mCamTargetAngle = acosf(llclamp(dir * cam_at, -1.f, 1.f)) * RAD_TO_DEG;
                }
            }
        }
    }
    if (private_lookat)
        mBlock = BLOCK_PRIVACY;     // setLookAt stores no target: nothing to send (by design)
    else if (disable_lookat)
        mBlock = BLOCK_LOOKAT_DISABLED;
    else if (held)
        mBlock = BLOCK_AO;
    else
        mBlock = BLOCK_NO_TARGET;   // until a usable target below
    if (lookat && !lookat->isDead() && !private_lookat && !disable_lookat && !held)
    {
        const ELookAtType type = lookat->getLookAtType();
        if (type != LOOKAT_TARGET_NONE && type != LOOKAT_TARGET_CLEAR)
        {
            const LLVector3 to_target = lookat->getTargetPos();    // from the head, region frame
            LLJoint* root = gAgentAvatarp->getRootJoint();
            if (root && to_target.isFinite() && to_target.length() >= MIN_TARGET_DISTANCE)
            {
                mBlock = BLOCK_NONE;
                const LLVector3 dir = to_target * ~root->getWorldRotation();
                yaw = atan2f(dir.mV[VY], dir.mV[VX]);
                pitch = atan2f(dir.mV[VZ], sqrtf(dir.mV[VX] * dir.mV[VX] + dir.mV[VY] * dir.mV[VY]));
                weight = std::isfinite((F32)strength) ? llclamp((F32)strength, 0.f, 1.f) : 1.f;
            }
        }
    }
    // Not looking at anything: the direction stays where it was (the head fades out there).
    mLiveYaw = yaw;
    mLivePitch = pitch;
    mLiveWeight = weight;
}

void SLPoseLookAt::tickSend()
{
    static LLCachedControl<bool> send(gSavedSettings, "SLLookAtSyncSend", false);
    static LLCachedControl<bool> show_self(gSavedSettings, "SLLookAtSyncShowSelf", true);

    SLBridge& bridge = SLBridge::instance();
    if ((bool)send != mBridgeUser)
    {
        mBridgeUser = send;
        if (mBridgeUser)
            bridge.addUser(BRIDGE_USER);
        else
            bridge.removeUser(BRIDGE_USER);
    }

    // RLV: no look-at goes out while sending on the pose channel is restricted (like pose data).
    const bool rlv_ok = !RlvActions::isRlvEnabled() || RlvActions::canSendChannel(SLBridge::POSE_CHANNEL);
    const bool can_send = send && bridge.isReady() && rlv_ok;
    const bool was_sending = mSendingNow;
    mSendingNow = can_send;     // tickFast samples (every frame) only while this holds
    if (!send)
        mBlock = BLOCK_OFF;
    else if (!rlv_ok)
        mBlock = BLOCK_RLV;
    else if (!bridge.isReady())
        mBlock = BLOCK_BRIDGE;
    if (!can_send)
    {
        if (!send)
            tickSendStatus(std::string());
        else
            tickSendStatus(std::string(blockName(mBlock)) + (bridge.isReady() ? std::string() :
                           llformat(" helper_state=%d helper_reason='%s'", (S32)bridge.getState(), bridge.getReason().c_str())));
        mBacklogWaiting = false;
        mLLType = -1;
        mLLDistance = -1.f;
        mLiveWeight = 0.f;
        // Stopping: one line saying "not looking", so viewers fade us out now rather than
        // after the stale time (a stop always goes out, like pose stops).
        if (mSentActive && bridge.isReady())
            sendLine(mSentYaw, mSentPitch, 0.f, getOwnMapId(), easeCode(localEase()));
        mSentActive = false;
        mEverSent = false;
        if (!mVRDriving && mEntries.erase(gAgentID))   // #1121: tickVR owns it in VR
            mChangedSignal();
        return;
    }
    if (!was_sending)
    {
        // Just became able to send: the next frame's sample is the first fresh one.
        mLiveWeight = 0.f;
        return;
    }

    const F32 yaw = mLiveYaw, pitch = mLivePitch, weight = mLiveWeight;
    const std::string map_id = getOwnMapId();
    const U8 ease_code = easeCode(localEase());
    const bool active = weight > 0.f;

    const F32 since = mLastSend.getElapsedTimeF32();
    F32 yaw_diff = llabs(yaw - mSentYaw);
    if (yaw_diff > F_PI)
        yaw_diff = F_TWO_PI - yaw_diff;
    const F32 pitch_diff = llabs(pitch - mSentPitch);
    bool changed = active != mSentActive || map_id != mSentMap || ease_code != mSentEase || llabs(weight - mSentWeight) > SEND_WEIGHT ||
                   (active && (yaw_diff > SEND_ANGLE || pitch_diff > SEND_ANGLE));
    // A small miss is corrected once the look has been still a while (at most one line per
    // SETTLE_INTERVAL), so a settled head is not left a degree or two off until the keep-alive.
    if (active && !changed && since >= SETTLE_INTERVAL && (yaw_diff > SETTLE_ANGLE || pitch_diff > SETTLE_ANGLE))
        changed = true;
    if (!mEverSent)
        changed = active;   // nothing to say until we first look at something
    const bool keepalive = active && since > KEEPALIVE_INTERVAL;
    // The bridge passes five lines a second, shared with pose data: wait while lines are
    // still queued (the change stays pending and goes out with the newest direction).
    // #902: the wait is bounded (SLLookAtSyncMaxBacklogWait seconds; 0 = wait as long as it
    // takes): with pose data streaming the queue can stay above one line for a long time,
    // and an unbounded wait could hold every look-at line, keep-alives included, while this
    // side showed blocked=none (receivers then fade the head out after STALE_AFTER).
    static LLCachedControl<F32> max_backlog_wait(gSavedSettings, "SLLookAtSyncMaxBacklogWait", 2.f);
    const bool backlog = bridge.getQueuedCommands() > MAX_QUEUED_BEFORE_SEND;
    const bool want = (changed && since >= MIN_SEND_INTERVAL) || keepalive;
    bool hold = want && backlog;
    if (hold)
    {
        if (!mBacklogWaiting)
        {
            mBacklogWaiting = true;
            mBacklogTimer.reset();
        }
        const F32 max_wait = (F32)max_backlog_wait;
        if (std::isfinite(max_wait) && max_wait > 0.f && mBacklogTimer.getElapsedTimeF32() >= max_wait)
            hold = false;
    }
    else if (!want)
    {
        mBacklogWaiting = false;
    }
    // Why nothing goes out (diagnostics): LL's look-at gives no usable target (the block
    // says which: privacy, AO, no target, ...), or the helper queue holds the line.
    if (!active && mBlock != BLOCK_NONE)
        tickSendStatus(blockName(mBlock));
    else if (hold && mBacklogTimer.getElapsedTimeF32() > 1.f)
        tickSendStatus(llformat("backlog queue=%d", (S32)bridge.getQueuedCommands()));
    else
        tickSendStatus(std::string());
    if (hold)
        ++mBacklogWaits;
    if (want && !hold)
    {
        mBacklogWaiting = false;
        sendLine(yaw, pitch, weight, map_id, ease_code);
        mEverSent = true;
        mSentActive = active;
        mSentYaw = yaw;
        mSentPitch = pitch;
        mSentWeight = weight;
        mSentMap = map_id;
        mSentEase = ease_code;
    }

    // Our own avatar's entry is kept by tickFast (every frame); here only its removal.
    if (!(show_self && mEverSent) && !mVRDriving && mEntries.erase(gAgentID))
        mChangedSignal();
}

void SLPoseLookAt::sendLine(F32 yaw, F32 pitch, F32 weight, const std::string& map_id, U8 ease_code)
{
    std::string bytes;
    bytes += (char)WIRE_VERSION;
    bytes += (char)ease_code;
    putS16(bytes, ll_round(llclamp(yaw * RAD_TO_DEG, -180.f, 180.f) * 100.f));
    putS16(bytes, ll_round(llclamp(pitch * RAD_TO_DEG, -90.f, 90.f) * 100.f));
    bytes += (char)(U8)llclamp(ll_round(weight * 255.f), 0, 255);
    const std::string id = validId(map_id) ? map_id : std::string("human");
    bytes += (char)(U8)id.size();
    bytes += id;
    const std::string line = llformat("P1|%u|L|1/1|-|", (U32)++mSeq) + SLPoseQuant::encode64((const U8*)bytes.data(), bytes.size());
    // Data, not control: a newer look-at supersedes one still queued.
    if (SLBridge::instance().sendCommand("say|" + line))
        ++mSendsInWindow;
    else
        LL_WARNS("PoseLookAt") << "The helper did not take a look-at line (state " << (S32)SLBridge::instance().getState() << ")" << LL_ENDL;
    mLastSend.reset();
}

void SLPoseLookAt::tickMotions()
{
    static LLCachedControl<bool> disable_lookat(gSavedSettings, "DisableLookAtAnimation", false);

    const F32 watch_dt = llclamp(mWatchTimer.getElapsedTimeF32(), 0.f, 1.f);
    mWatchTimer.reset();
    for (auto& [id, entry] : mEntries)
    {
        LLVOAvatar* avatar = findAvatar(id);
        if (!avatar)
            continue;
        updatePosed(avatar, entry);
        if (effectiveWeight(entry) > 0.f)
            ensureMotion(avatar);
        if (id != gAgentID)
            watchMotion(avatar, entry, watch_dt);
        // LL's head turn would turn the same head a second time: it stands down while we
        // drive (LLHUDEffectLookAt does not restart it meanwhile either).
        if (isDriving(id) && avatar->isMotionActive(ANIM_AGENT_HEAD_ROT))
        {
            avatar->stopMotion(ANIM_AGENT_HEAD_ROT);
            mHeadRotStopped.insert(id);
        }
    }

    // Hand the head back to LL's head turn once we stop driving it.
    for (auto it = mHeadRotStopped.begin(); it != mHeadRotStopped.end();)
    {
        const LLUUID id = *it;
        if (isDriving(id))
        {
            ++it;
            continue;
        }
        it = mHeadRotStopped.erase(it);
        LLViewerObject* object = gObjectList.findObject(id);
        if (!object || object->isDead() || !object->isAvatar())
            continue;
        LLVOAvatar* avatar = static_cast<LLVOAvatar*>(object);
        const bool held = avatar->isSelf() && SLAOEngine::instanceExists() && SLAOEngine::instance().holdsHead();
        if (!disable_lookat && !held && !avatar->isMotionActive(ANIM_AGENT_HEAD_ROT))
            avatar->startMotion(ANIM_AGENT_HEAD_ROT);
    }

    // Stop the layer on avatars nobody drives once it has faded out.
    for (auto it = mMotionsRunning.begin(); it != mMotionsRunning.end();)
    {
        const LLUUID id = *it;
        LLViewerObject* object = gObjectList.findObject(id);
        if (!object || object->isDead() || !object->isAvatar())
        {
            it = mMotionsRunning.erase(it);
            continue;
        }
        LLVOAvatar* avatar = static_cast<LLVOAvatar*>(object);
        auto entry = mEntries.find(id);
        const bool driven = entry != mEntries.end() && effectiveWeight(entry->second) > 0.f;
        SLPoseLookAtMotion* motion = dynamic_cast<SLPoseLookAtMotion*>(avatar->findMotion(ANIM_ID));
        if (!motion || !avatar->isMotionActive(ANIM_ID))
        {
            it = mMotionsRunning.erase(it);
            continue;
        }
        if (!driven && motion->isIdle())
        {
            // Locally only: this motion is never known to the sim.
            avatar->stopMotion(ANIM_ID, true);
            it = mMotionsRunning.erase(it);
            continue;
        }
        ++it;
    }
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

// static
const char* SLPoseLookAt::blockName(EBlock block)
{
    switch (block)
    {
    case BLOCK_NONE:            return "none";
    case BLOCK_OFF:             return "send_off";
    case BLOCK_RLV:             return "rlv";
    case BLOCK_BRIDGE:          return "helper_not_ready";
    case BLOCK_PRIVACY:         return "privacy";
    case BLOCK_LOOKAT_DISABLED: return "lookat_animation_disabled";
    case BLOCK_AO:              return "ao_holds_head";
    case BLOCK_NO_TARGET:       return "no_target";
    default:                    return "unknown";
    }
}

// static
const char* SLPoseLookAt::rejectName(EReject reason)
{
    switch (reason)
    {
    case REJ_NOT_INIT:  return "not_init";
    case REJ_SHOW_OFF:  return "show_off";
    case REJ_FORMAT:    return "format";
    case REJ_VERSION:   return "version";
    case REJ_MUTED:     return "muted";
    case REJ_NO_AVATAR: return "no_avatar";
    case REJ_FULL:      return "full";
    case REJ_RATE:      return "rate";
    default:            return "unknown";
    }
}

void SLPoseLookAt::noteReject(const LLUUID& sender, EReject reason)
{
    if ((S32)reason < 0 || (S32)reason >= (S32)REJ_COUNT)
        return;
    ++mRejects[reason];
    auto it = mRejectsBy.find(sender);
    if (it == mRejectsBy.end())
    {
        if (mRejectsBy.size() >= MAX_REJECT_SENDERS)
            return;     // the totals still count it
        it = mRejectsBy.emplace(sender, std::array<U32, REJ_COUNT>{}).first;
    }
    ++it->second[reason];
}

std::string SLPoseLookAt::anonId(const LLUUID& id) const
{
    // Never a name or key in the log: four hex digits of a hash, stable for the session.
    return id == gAgentID ? std::string("self") : llformat("%04x", (id.getCRC32() >> 16) & 0xFFFF);
}

// static
bool SLPoseLookAt::isRiggedOn(LLVOAvatar* avatar, const std::vector<bool>& rigged, const std::string& joint_name)
{
    if (!avatar)
        return false;
    LLJoint* joint = avatar->getJoint(joint_name);
    if (!joint)
        return false;
    const S32 num = joint->getJointNum();
    if (num >= 0 && num < (S32)rigged.size() && rigged[num])
        return true;
    return avatar->jointIsRiggedTo(joint);
}

std::string SLPoseLookAt::riggingMapFor(LLVOAvatar* avatar, const std::vector<bool>& rigged, S32 rigged_count) const
{
    // detectMap's rigging step for any avatar (a remote one's figure setting and item names
    // are not known here, so a pony on the classic skeleton reads as human).
    if (!avatar || rigged_count <= 0)
        return "none";
    for (const SLLookAtMap& map : mMaps)
    {
        if (map.mDetect.empty() && map.mDetectAny.empty())
            continue;
        bool all = true;
        for (const std::string& name : map.mDetect)
            all = all && isRiggedOn(avatar, rigged, name);
        bool any = map.mDetectAny.empty();
        for (const std::string& name : map.mDetectAny)
        {
            if (isRiggedOn(avatar, rigged, name))
            {
                any = true;
                break;
            }
        }
        if (all && any)
            return map.mId;
    }
    return "human";
}

std::string SLPoseLookAt::describeEntry(const LLUUID& id, const Entry& entry)
{
    // One remote avatar: what arrived, what we made of its body, and what the layer does.
    std::string out = anonId(id);
    out += llformat(" age=%.1f lines=%u rx(yaw/pitch/w)=%.1f/%.1f/%.2f eff_w=%.2f ease=%.2f map=%s(sent %s)",
                    entry.mLastHeard.getElapsedTimeF32(), entry.mLines, entry.mYaw * RAD_TO_DEG, entry.mPitch * RAD_TO_DEG,
                    entry.mWeight, effectiveWeight(entry), entry.mEase, entry.mMap ? entry.mMap->mId.c_str() : "-",
                    entry.mDeclaredMap.empty() ? "-" : entry.mDeclaredMap.c_str());
    LLVOAvatar* avatar = findAvatar(id);
    if (!avatar)
        return out + " avatar=missing";

    std::vector<bool> rigged;
    const S32 rigged_count = collectRiggedJoints(avatar, rigged);
    out += llformat(" our_map=%s(rigged_bones=%d)", riggingMapFor(avatar, rigged, rigged_count).c_str(), rigged_count);
    std::string bones;
    if (entry.mMap)
    {
        for (const SLLookAtBone& bone : entry.mMap->mBones)
        {
            bones += (bones.empty() ? "" : ",") + bone.mJoint;
            bones += !avatar->getJoint(bone.mJoint) ? ":missing" : (isRiggedOn(avatar, rigged, bone.mJoint) ? ":rigged" : ":unrigged");
        }
    }
    out += " bones=" + (bones.empty() ? std::string("-") : bones);

    SLPoseLookAtMotion* motion = dynamic_cast<SLPoseLookAtMotion*>(avatar->findMotion(ANIM_ID));
    if (!motion)
    {
        out += " motion=none";
    }
    else
    {
        // fade = the motion controller's LOD fade (0: skipped for pixel area); prio = the
        // layer's priority against the avatar's animations (ADDITIVE_PRIORITY sorts first).
        out += llformat(" motion=%s bound=%d slots=%d prio=%d fade=%.2f upd_age=%.1f updates=%u target_w=%.2f w=%.2f "
                        "eased(yaw/pitch)=%.1f/%.1f applied_deg=%.1f applied=%s",
                        avatar->isMotionActive(ANIM_ID) ? "active" : "stopped", motion->isBound() ? 1 : 0, motion->getSlotCount(),
                        (S32)motion->getPriority(), motion->getFadeWeight(), motion->secondsSinceUpdate(), motion->getUpdateCount(),
                        motion->getTargetWeight(), motion->getAppliedWeight(), motion->getYaw() * RAD_TO_DEG,
                        motion->getPitch() * RAD_TO_DEG, motion->getAppliedAngle() * RAD_TO_DEG, motion->describeBones().c_str());
    }
    out += llformat(" head_rot=%d px=%.0f(min %.0f) impostor=%d gpu_anim=%d restarts=%u",
                    avatar->isMotionActive(ANIM_AGENT_HEAD_ROT) ? 1 : 0, avatar->getPixelArea(), MIN_PIXEL_AREA,
                    avatar->isImpostor() ? 1 : 0, (S32)avatar->mGpuAnimMode, entry.mRestarts);
    if (!entry.mLastRestart.empty())
        out += " last_restart=" + entry.mLastRestart;
    return out;
}

void SLPoseLookAt::watchMotion(LLVOAvatar* avatar, Entry& entry, F32 dt)
{
    // #902 watchdog: a remote avatar driven at a real weight whose layer applies nothing.
    // Stopping and restarting the motion (what a sender's off / on did by accident: its
    // weight-0 line let the layer go idle and stop, the next line started a fresh one)
    // brings it back. Avatars the motion controller skips by design (pixel area below the
    // layer's minimum, off screen: no updates) are left alone.
    const F32 want = effectiveWeight(entry);
    SLPoseLookAtMotion* motion = dynamic_cast<SLPoseLookAtMotion*>(avatar->findMotion(ANIM_ID));
    const char* why = nullptr;
    if (want >= STUCK_WEIGHT && motion && avatar->isMotionActive(ANIM_ID) && avatar->getPixelArea() >= MIN_PIXEL_AREA)
    {
        const F32 upd_age = motion->secondsSinceUpdate();
        if (!motion->isBound())
            why = "unbound";        // initialised against an incomplete skeleton: never drives
        else if (upd_age >= 0.f && upd_age < 1.f && motion->getAppliedWeight() < STUCK_APPLIED)
            why = "no_weight";      // updated, yet its weight never ramps up
    }
    if (!why)
    {
        entry.mStuckFor = 0.f;
        return;
    }
    entry.mStuckFor += dt;
    if (entry.mStuckFor < STUCK_AFTER)
        return;
    entry.mStuckFor = 0.f;
    static LLCachedControl<bool> restart_stuck(gSavedSettings, "SLLookAtSyncRestartStuck", true);
    const bool act = restart_stuck && entry.mRestarts < MAX_RESTARTS;
    LL_INFOS("PoseLookAt") << "#PoseLookAt stuck " << anonId(avatar->getID()) << " why=" << why
                           << " want=" << llformat("%.2f", want) << " applied_w=" << llformat("%.2f", motion->getAppliedWeight())
                           << " target_w=" << llformat("%.2f", motion->getTargetWeight())
                           << " upd_age=" << llformat("%.1f", motion->secondsSinceUpdate())
                           << " fade=" << llformat("%.2f", motion->getFadeWeight())
                           << " restarts=" << entry.mRestarts << (act ? " -> restarting" : " (restart off or over budget)") << LL_ENDL;
    if (act)
        restartMotion(avatar, entry, why, true);
}

void SLPoseLookAt::restartMotion(LLVOAvatar* avatar, Entry& entry, const char* why, bool replace)
{
    if (!avatar || avatar->isDead())
        return;
    ++entry.mRestarts;
    entry.mLastRestart = why ? why : "?";
    // Locally only (never known to the sim). Replacing drops the instance, so the next start
    // creates and initialises a fresh one against the skeleton as it is now.
    avatar->stopMotion(ANIM_ID, true);
    if (replace)
        avatar->removeMotion(ANIM_ID);
    mMotionsRunning.erase(avatar->getID());
    ensureMotion(avatar);
}

void SLPoseLookAt::tickSendStatus(const std::string& why)
{
    // Sender diagnostics (#902): "Send where I look" is on, yet nothing goes out. Logged
    // once the reason has held NOT_SENDING_LOG_AFTER seconds (LL's look-at comes and goes
    // on its own timeouts), again at most every NOT_SENDING_RELOG, and once it clears.
    if (why != mNotSendingWhy)
    {
        if (why.empty() && mNotSendingLogged)
        {
            LL_INFOS("PoseLookAt") << "#PoseLookAt sending_resumed after=" << llformat("%.0f", mNotSendingTimer.getElapsedTimeF32())
                                   << "s (was: " << mNotSendingWhy << ")" << LL_ENDL;
        }
        mNotSendingWhy = why;
        mNotSendingTimer.reset();
        mNotSendingLogged = false;
        return;
    }
    if (why.empty() || mNotSendingTimer.getElapsedTimeF32() < NOT_SENDING_LOG_AFTER)
        return;
    if (mNotSendingLogged && mNotSendingLogTimer.getElapsedTimeF32() < NOT_SENDING_RELOG)
        return;
    mNotSendingLogged = true;
    mNotSendingLogTimer.reset();
    SLBridge& bridge = SLBridge::instance();
    LL_INFOS("PoseLookAt") << "#PoseLookAt not_sending reason=" << why
                           << " for=" << llformat("%.0f", mNotSendingTimer.getElapsedTimeF32()) << "s"
                           << " helper_state=" << (S32)bridge.getState()
                           << " helper_reason='" << bridge.getReason() << "'"
                           << " queue=" << bridge.getQueuedCommands()
                           << " ll=" << lookAtTypeName(mLLType) << "(" << mLLType << ")"
                           << " ever_sent=" << (mEverSent ? 1 : 0)
                           << " last_sent_age=" << llformat("%.0f", mEverSent ? mLastSend.getElapsedTimeF32().value() : -1.f) << LL_ENDL;
}

void SLPoseLookAt::tickStatus()
{
    static LLCachedControl<bool> send(gSavedSettings, "SLLookAtSyncSend", false);
    static LLCachedControl<bool> show(gSavedSettings, "SLLookAtSyncShow", true);

    // In use: sending, or showing someone's look-at (the show option is on by default, so it
    // alone does not make a line every ten seconds).
    bool remote = false;
    for (const auto& [id, entry] : mEntries)
    {
        if (id != gAgentID)
        {
            remote = true;
            break;
        }
    }
    const bool in_use = (bool)send || ((bool)show && remote);

    // State changes log at once (the no-target state comes and goes with LL's look-at
    // timeouts, so only the reasons a person has to act on count as a change).
    const EBlock block = (mBlock == BLOCK_NO_TARGET) ? BLOCK_NONE : mBlock;
    const std::string key = llformat("%d|%d|%d|", (S32)(bool)send, (S32)(bool)show, (S32)block) + getOwnMapId();
    if (key != mStatusKey)
    {
        const bool first = mStatusKey.empty();
        mStatusKey = key;
        if (!first || in_use)
            logStatus("changed");
        mStatusTimer.reset();
        return;
    }
    if (in_use && mStatusTimer.getElapsedTimeF32() >= STATUS_INTERVAL)
    {
        logStatus("periodic");
        mStatusTimer.reset();
    }
}

void SLPoseLookAt::logStatus(const char* why)
{
    static LLCachedControl<bool> send(gSavedSettings, "SLLookAtSyncSend", false);
    static LLCachedControl<bool> show(gSavedSettings, "SLLookAtSyncShow", true);
    static LLCachedControl<bool> show_self(gSavedSettings, "SLLookAtSyncShowSelf", true);

    // Lines per minute, over the window since the last status line (reset each minute).
    const F32 window = llmax((F32)mSendWindow.getElapsedTimeF32(), 0.001f);
    const F32 per_minute = (F32)mSendsInWindow * 60.f / window;
    if (window >= 60.f)
    {
        mSendWindow.reset();
        mSendsInWindow = 0;
    }

    S32 driven = 0;
    for (const auto& [id, entry] : mEntries)
    {
        if (effectiveWeight(entry) > 0.f)
            ++driven;
    }

    // Our own avatar: is the layer running, how much it turned, which bones it found.
    std::string self_motion = "none";
    F32 applied_deg = 0.f;
    F32 motion_weight = 0.f;
    S32 bound = 0;
    F32 eased_yaw = 0.f, eased_pitch = 0.f;
    std::string bones;
    std::string applied = "-";
    if (isAgentAvatarValid())
    {
        LLVOAvatarSelf* self = gAgentAvatarp.get();
        SLPoseLookAtMotion* motion = dynamic_cast<SLPoseLookAtMotion*>(self->findMotion(ANIM_ID));
        if (motion)
        {
            self_motion = self->isMotionActive(ANIM_ID) ? "active" : "stopped";
            applied_deg = motion->getAppliedAngle() * RAD_TO_DEG;
            motion_weight = motion->getAppliedWeight();
            bound = motion->getBoundBones();
            eased_yaw = motion->getYaw() * RAD_TO_DEG;
            eased_pitch = motion->getPitch() * RAD_TO_DEG;
            applied = motion->describeBones();
        }
        if (const SLLookAtMap* map = findMap(getOwnMapId()))
        {
            for (const SLLookAtBone& bone : map->mBones)
            {
                bones += (bones.empty() ? "" : ",") + bone.mJoint;
                bones += !self->getJoint(bone.mJoint) ? ":missing" : (isRigged(bone.mJoint) ? ":rigged" : ":unrigged");
            }
        }
    }

    // #902: every remote entry (at most MAX_STATUS_ENTRIES), and the lines dropped before
    // they made one, by stage and by (anonymised) sender.
    std::string remotes;
    size_t described = 0;
    for (const auto& [id, entry] : mEntries)
    {
        if (id == gAgentID)
            continue;
        if (described++ >= MAX_STATUS_ENTRIES)
        {
            remotes += " [more not shown]";
            break;
        }
        remotes += (remotes.empty() ? "[" : " [") + describeEntry(id, entry) + "]";
    }
    if (remotes.empty())
        remotes = "-";
    std::string rejects;
    for (S32 r = 0; r < REJ_COUNT; ++r)
    {
        if (mRejects[r])
            rejects += llformat("%s%s=%u", rejects.empty() ? "" : ",", rejectName((EReject)r), mRejects[r]);
    }
    for (const auto& [sender, counts] : mRejectsBy)
    {
        std::string per;
        for (S32 r = 0; r < REJ_COUNT; ++r)
        {
            if (counts[r])
                per += llformat("%s%s=%u", per.empty() ? "" : ",", rejectName((EReject)r), counts[r]);
        }
        if (!per.empty())
            rejects += " " + anonId(sender) + ":" + per;
    }
    if (rejects.empty())
        rejects = "-";

    SLBridge& bridge = SLBridge::instance();
    LL_INFOS("PoseLookAt") << "#PoseLookAt " << why
                           << " send=" << ((bool)send ? 1 : 0)
                           << " blocked=" << blockName(mBlock)
                           << " helper_state=" << (S32)bridge.getState()
                           << " queue=" << bridge.getQueuedCommands()
                           << " backlog_waits=" << mBacklogWaits
                           << " | ll=" << lookAtTypeName(mLLType) << "(" << mLLType << ")"
                           << " dist=" << llformat("%.2f", mLLDistance)
                           << " target=" << llformat("<%.2f,%.2f,%.2f>", mLLTargetRegion.mV[VX], mLLTargetRegion.mV[VY], mLLTargetRegion.mV[VZ])
                           << " cam=" << llformat("<%.2f,%.2f,%.2f>", mCamPosRegion.mV[VX], mCamPosRegion.mV[VY], mCamPosRegion.mV[VZ])
                           << " cam_focus=" << llformat("<%.2f,%.2f,%.2f>", mCamFocusRegion.mV[VX], mCamFocusRegion.mV[VY], mCamFocusRegion.mV[VZ])
                           << " cam_vs_target_deg=" << llformat("%.1f", mCamTargetAngle)
                           << " freecam=" << (mFreeCam ? 1 : 0)
                           << " | sent yaw=" << llformat("%.1f", mSentYaw * RAD_TO_DEG)
                           << " pitch=" << llformat("%.1f", mSentPitch * RAD_TO_DEG)
                           << " w=" << llformat("%.2f", mSentWeight)
                           << " map=" << (mSentMap.empty() ? std::string("-") : mSentMap)
                           << " ever_sent=" << (mEverSent ? 1 : 0)
                           << " lines_per_min=" << llformat("%.1f", per_minute)
                           << " rx_lines=" << mRxLines
                           << " | own_map=" << getOwnMapId() << " (auto: " << mDetectedMap << " by " << mDetectedHow
                           << ", rigged_bones=" << mRiggedCount << ")"
                           << " bones=" << (bones.empty() ? std::string("-") : bones)
                           << " | ease=" << llformat("%.2f", localEase()) << "s"
                           << " live yaw=" << llformat("%.1f", mLiveYaw * RAD_TO_DEG)
                           << " pitch=" << llformat("%.1f", mLivePitch * RAD_TO_DEG)
                           << " w=" << llformat("%.2f", mLiveWeight)
                           << " eased yaw=" << llformat("%.1f", eased_yaw)
                           << " pitch=" << llformat("%.1f", eased_pitch)
                           << " applied(yaw/pitch deg)=" << applied
                           << " | show=" << ((bool)show ? 1 : 0) << " show_self=" << ((bool)show_self ? 1 : 0)
                           << " vr_head=" << (mVRDriving ? 1 : 0)
                           << " entries=" << mEntries.size() << " driven=" << driven
                           << " self_motion=" << self_motion
                           << " motion_w=" << llformat("%.2f", motion_weight)
                           << " bound_bones=" << bound
                           << " applied_deg=" << llformat("%.1f", applied_deg)
                           << " | not_sending=" << (mNotSendingWhy.empty() ? std::string("-") : mNotSendingWhy)
                           << " last_sent_age=" << llformat("%.1f", mEverSent ? mLastSend.getElapsedTimeF32().value() : -1.f)
                           << " | remotes: " << remotes
                           << " | rejects: " << rejects
                           << LL_ENDL;
}
