/**
 * @file slposecreator.cpp
 * @brief Pose Creator (#117): keyframes + .anim writer. See slposecreator.h.
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

#include "slposecreator.h"

#include <algorithm>
#include <cmath>

#include "lldatapacker.h"
#include "llfile.h"
#include "llhandmotion.h"
#include "lljoint.h"
#include "llkeyframemotion.h"   // KEYFRAME_MOTION_VERSION / SUBVERSION
#include "llquantize.h"
#include "llvoavatar.h"
#include "slposestudio.h"

namespace
{
constexpr F32 kSameTime = 0.001f;            // keyframes closer than this are one keyframe
constexpr F32 kStaticPoseDuration = 1.f;     // a single keyframe: a looped 1 s static pose
constexpr F32 kMinDuration = 1.f / 30.f;
// A bone counts as animated when its rotation changes by more than this between keys
// (radians, ~0.25 degrees: below the 16-bit quantization's visible step).
constexpr F32 kRotEpsilon = 0.0045f;
constexpr F32 kPosEpsilon = 0.0005f;         // metres

F32 rotAngle(const LLQuaternion& a, const LLQuaternion& b)
{
    F32 d = std::fabs(a.mQ[VX] * b.mQ[VX] + a.mQ[VY] * b.mQ[VY] + a.mQ[VZ] * b.mQ[VZ] + a.mQ[VW] * b.mQ[VW]);
    d = llclamp(d, 0.f, 1.f);
    return 2.f * std::acos(d);
}
} // namespace

bool SLPoseCreator::capture(LLVOAvatar* avatar, Keyframe& key) const
{
    if (!avatar || avatar->isDead())
        return false;
    SLPoseStudio& studio = SLPoseStudio::instance();
    const LLUUID& avatar_id = avatar->getID();
    key.mRotations.clear();
    key.mEdited.clear();
    for (const SLPoseStudio::joint_t& pj : studio.getAnimator().PoserJoints)
    {
        if (pj.boneType() == COL_VOLUMES)
            continue;
        LLJoint* joint = avatar->getJoint(pj.jointName());
        if (!joint)
            continue;
        // What the avatar shows: the joint's blended local rotation, except a bone that
        // is mid ease-in, whose keyframe is where it is going.
        LLQuaternion rot = joint->getRotation();
        LLQuaternion blend_rot;
        LLVector3 blend_pos, blend_scale;
        if (studio.getBlendTarget(avatar, pj, blend_rot, blend_pos, blend_scale))
            rot = blend_rot;
        rot.normalize();
        key.mRotations[pj.jointName()] = rot;
        if (studio.isBoneEdited(avatar_id, pj.jointName()))
            key.mEdited.insert(pj.jointName());
    }
    if (LLJoint* pelvis = avatar->getJoint("mPelvis"))
        key.mPelvisPosition = pelvis->getPosition();
    return !key.mRotations.empty();
}

S32 SLPoseCreator::addKeyframe(LLVOAvatar* avatar, F32 time)
{
    Keyframe key;
    key.mTime = llclamp(time, 0.f, MAX_DURATION);
    if (!capture(avatar, key))
        return -1;
    for (Keyframe& k : mKeys)
    {
        if (std::fabs(k.mTime - key.mTime) < kSameTime)
        {
            const F32 t = k.mTime;
            k = key;
            k.mTime = t;
            return (S32)(&k - mKeys.data());
        }
    }
    mKeys.push_back(key);
    std::stable_sort(mKeys.begin(), mKeys.end(), [](const Keyframe& a, const Keyframe& b) { return a.mTime < b.mTime; });
    for (size_t i = 0; i < mKeys.size(); ++i)
        if (std::fabs(mKeys[i].mTime - key.mTime) < kSameTime)
            return (S32)i;
    return -1;
}

bool SLPoseCreator::recapture(S32 index, LLVOAvatar* avatar)
{
    if (index < 0 || index >= (S32)mKeys.size())
        return false;
    Keyframe key;
    if (!capture(avatar, key))
        return false;
    key.mTime = mKeys[index].mTime;
    mKeys[index] = key;
    return true;
}

void SLPoseCreator::removeKeyframe(S32 index)
{
    if (index >= 0 && index < (S32)mKeys.size())
        mKeys.erase(mKeys.begin() + index);
}

S32 SLPoseCreator::setKeyframeTime(S32 index, F32 time)
{
    if (index < 0 || index >= (S32)mKeys.size())
        return -1;
    time = llclamp(time, 0.f, MAX_DURATION);
    // Never two keyframes at one time: nudge past an occupied slot.
    for (size_t i = 0; i < mKeys.size(); ++i)
        if ((S32)i != index && std::fabs(mKeys[i].mTime - time) < kSameTime)
            time = llmin(time + 2.f * kSameTime, MAX_DURATION);
    Keyframe moved = mKeys[index];
    moved.mTime = time;
    mKeys.erase(mKeys.begin() + index);
    auto it = std::upper_bound(mKeys.begin(), mKeys.end(), moved,
                               [](const Keyframe& a, const Keyframe& b) { return a.mTime < b.mTime; });
    it = mKeys.insert(it, moved);
    return (S32)(it - mKeys.begin());
}

bool SLPoseCreator::applyKeyframe(S32 index, LLVOAvatar* avatar) const
{
    if (index < 0 || index >= (S32)mKeys.size() || !avatar)
        return false;
    SLPoseStudio& studio = SLPoseStudio::instance();
    if (!studio.isPosing(avatar))
        return false;
    // Mirror mode would reflect every bone onto its partner (overwriting the partner's
    // own captured rotation): off while the keyframe is applied.
    const bool mirror = studio.getMirror();
    if (mirror)
        studio.setMirror(false);
    const Keyframe& key = mKeys[index];
    bool any = false;
    for (const auto& [name, rot] : key.mRotations)
    {
        // Only the bones that keyframe owns: edited ones (or every bone when nothing was
        // edited -- a capture of a plain animation).
        if (!key.mEdited.empty() && !key.mEdited.count(name))
            continue;
        const SLPoseStudio::joint_t* joint = studio.findJoint(name);
        if (joint && studio.setBoneRotation(avatar, *joint, rot, false))
            any = true;
    }
    if (mirror)
        studio.setMirror(true);
    if (any)
        studio.notifyEdited(avatar, true);
    return any;
}

std::vector<std::string> SLPoseCreator::exportedBones(const Options& opt) const
{
    std::set<std::string> bones;
    if (mKeys.empty())
        return {};
    const Keyframe& first = mKeys.front();
    for (const auto& [name, rot] : first.mRotations)
    {
        bool want = !opt.mEditedOnly;
        for (const Keyframe& k : mKeys)
        {
            if (want)
                break;
            if (k.mEdited.count(name))
                want = true;
            else
            {
                // Not edited but moving between keyframes (an animation captured
                // under the poser): still part of the motion.
                auto it = k.mRotations.find(name);
                if (it != k.mRotations.end() && rotAngle(it->second, rot) > kRotEpsilon)
                    want = true;
            }
        }
        if (want)
            bones.insert(name);
    }
    return std::vector<std::string>(bones.begin(), bones.end());
}

bool SLPoseCreator::buildAnim(const Options& opt_in, std::vector<U8>& out, std::string& error) const
{
    out.clear();
    if (mKeys.empty())
    {
        error = "no_keyframes";
        return false;
    }
    Options opt = opt_in;
    opt.mPriority = llclamp(opt.mPriority, 0, MAX_PRIORITY);
    opt.mHandPose = llmin(opt.mHandPose, (U32)LLHandMotion::NUM_HAND_POSES - 1u);
    opt.mEaseIn = llclamp(opt.mEaseIn, 0.f, 10.f);
    opt.mEaseOut = llclamp(opt.mEaseOut, 0.f, 10.f);

    // Keys: one keyframe = a static pose (two equal keys); else the keyframes as they
    // are, the first moved to t = 0 (an animation starts at its first key).
    std::vector<const Keyframe*> keys;
    std::vector<F32> times;
    const bool single = mKeys.size() == 1;
    const F32 t0 = mKeys.front().mTime;
    for (const Keyframe& k : mKeys)
    {
        keys.push_back(&k);
        times.push_back(k.mTime - t0);
    }
    F32 duration = opt.mDuration > 0.f ? opt.mDuration : (single ? kStaticPoseDuration : times.back());
    duration = llclamp(duration, kMinDuration, MAX_DURATION);
    if (single)
    {
        keys.push_back(keys.front());
        times.push_back(duration);
    }
    // Keys past an explicit duration are dropped; the timeline then ends at the duration.
    while (keys.size() > 2 && times.back() > duration + kSameTime)
    {
        keys.pop_back();
        times.pop_back();
    }
    const F32 loop_in = llclamp(opt.mLoopIn, 0.f, duration);
    const F32 loop_out = opt.mLoopOut < 0.f ? duration : llclamp(opt.mLoopOut, loop_in, duration);

    const std::vector<std::string> bones = exportedBones(opt);
    if (bones.empty())
    {
        error = "no_bones";
        return false;
    }
    // Pelvis position keys: wanted and either moving or edited (the whole-avatar /
    // pelvis position spinners) -- a pure rotation pose leaves the avatar's own
    // position to whatever plays under it.
    bool pelvis_pos = false;
    if (opt.mPelvisPosition)
    {
        for (const Keyframe* k : keys)
            if ((k->mPelvisPosition - keys.front()->mPelvisPosition).length() > kPosEpsilon || k->mEdited.count("mPelvis"))
                pelvis_pos = true;
    }

    // Two passes (LLKeyframeMotion::getFileSize): measure, then write.
    auto pack = [&](LLDataPacker& dp) -> bool
    {
        bool ok = true;
        // == LLKeyframeMotion::serialize, field for field.
        ok &= dp.packU16(KEYFRAME_MOTION_VERSION, "version");
        ok &= dp.packU16(KEYFRAME_MOTION_SUBVERSION, "sub_version");
        ok &= dp.packS32(opt.mPriority, "base_priority");
        ok &= dp.packF32(duration, "duration");
        ok &= dp.packString(opt.mEmote, "emote_name");
        ok &= dp.packF32(loop_in, "loop_in_point");
        ok &= dp.packF32(loop_out, "loop_out_point");
        ok &= dp.packS32(opt.mLoop ? 1 : 0, "loop");
        ok &= dp.packF32(opt.mEaseIn, "ease_in_duration");
        ok &= dp.packF32(opt.mEaseOut, "ease_out_duration");
        ok &= dp.packU32(opt.mHandPose, "hand_pose");
        ok &= dp.packU32((U32)bones.size(), "num_joints");
        for (const std::string& name : bones)
        {
            ok &= dp.packString(name, "joint_name");
            ok &= dp.packS32(opt.mPriority, "joint_priority");
            ok &= dp.packS32((S32)keys.size(), "num_rot_keys");
            for (size_t i = 0; i < keys.size(); ++i)
            {
                ok &= dp.packU16(F32_to_U16(times[i], 0.f, duration), "time");
                auto it = keys[i]->mRotations.find(name);
                LLQuaternion q = it != keys[i]->mRotations.end() ? it->second : LLQuaternion::DEFAULT;
                q.normalize();
                LLVector3 v = q.packToVector3();
                v.quantize16(-1.f, 1.f, -1.f, 1.f);
                ok &= dp.packU16(F32_to_U16(v.mV[VX], -1.f, 1.f), "rot_angle_x");
                ok &= dp.packU16(F32_to_U16(v.mV[VY], -1.f, 1.f), "rot_angle_y");
                ok &= dp.packU16(F32_to_U16(v.mV[VZ], -1.f, 1.f), "rot_angle_z");
            }
            const bool pos = pelvis_pos && name == "mPelvis";
            ok &= dp.packS32(pos ? (S32)keys.size() : 0, "num_pos_keys");
            if (pos)
            {
                for (size_t i = 0; i < keys.size(); ++i)
                {
                    ok &= dp.packU16(F32_to_U16(times[i], 0.f, duration), "time");
                    LLVector3 p = keys[i]->mPelvisPosition;
                    p.clamp(-LL_MAX_PELVIS_OFFSET, LL_MAX_PELVIS_OFFSET);
                    p.quantize16(-LL_MAX_PELVIS_OFFSET, LL_MAX_PELVIS_OFFSET, -LL_MAX_PELVIS_OFFSET, LL_MAX_PELVIS_OFFSET);
                    ok &= dp.packU16(F32_to_U16(p.mV[VX], -LL_MAX_PELVIS_OFFSET, LL_MAX_PELVIS_OFFSET), "pos_x");
                    ok &= dp.packU16(F32_to_U16(p.mV[VY], -LL_MAX_PELVIS_OFFSET, LL_MAX_PELVIS_OFFSET), "pos_y");
                    ok &= dp.packU16(F32_to_U16(p.mV[VZ], -LL_MAX_PELVIS_OFFSET, LL_MAX_PELVIS_OFFSET), "pos_z");
                }
            }
        }
        ok &= dp.packS32(0, "num_constraints");
        return ok;
    };

    LLDataPackerBinaryBuffer measure;
    pack(measure);
    const S32 size = measure.getCurrentSize();
    if (size <= 0)
    {
        error = "pack_failed";
        return false;
    }
    out.assign((size_t)size, 0);
    LLDataPackerBinaryBuffer dp(out.data(), size);
    if (!pack(dp))
    {
        out.clear();
        error = "pack_failed";
        return false;
    }
    return true;
}

bool SLPoseCreator::exportAnim(const std::string& path, const Options& opt, std::string& error) const
{
    std::vector<U8> bytes;
    if (!buildAnim(opt, bytes, error))
        return false;
    llofstream file(path.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
    if (!file.is_open())
    {
        error = "write_failed";
        return false;
    }
    file.write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
    file.close();
    if (!file)
    {
        error = "write_failed";
        return false;
    }
    LL_INFOS("PoseCreator") << "Wrote " << bytes.size() << " B animation (" << mKeys.size() << " keyframes) to "
                            << path << LL_ENDL;
    return true;
}
