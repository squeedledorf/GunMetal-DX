/**
 * @file slposecreator.h
 * @brief Pose Creator (#117): a keyframe editor on top of Pose Studio -- capture
 *        the posed avatar at points in time, then write a Second Life internal
 *        animation (.anim, the LLKeyframeMotion serialized form) for upload, local
 *        preview or sharing.
 *
 * Black Dragon's Pose Creator (bdfloaterposecreator / bdanimator) inspired the
 * feature; this is our own implementation on the Pose Studio engine (no BD code).
 *
 * KEYFRAMES hold every (non collision volume) bone's LOCAL rotation -- the joint's
 * parent-relative rotation, exactly what an .anim rotation key is -- as the avatar
 * shows it (a bone mid ease-in contributes the blend's end rotation), the bones Pose
 * Studio counts as edited at that moment, and the pelvis's local position.
 *
 * EXPORT (buildAnim) mirrors LLKeyframeMotion::serialize field for field (version 1.0,
 * no constraints), so the bytes deserialize through the same path as an uploaded
 * animation: the upload floater ("upload_anim_anim"), LLLocalAnimMgr (preview) and
 * the asset server. One keyframe = a static pose: two identical keys over one second,
 * looped. Times are quantized to 1 / 65535 of the duration, rotations to 16 bits per
 * packed component, positions to 16 bits over +-5 m (LL_MAX_PELVIS_OFFSET).
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

#ifndef SL_POSECREATOR_H
#define SL_POSECREATOR_H

#include <map>
#include <set>
#include <string>
#include <vector>

#include "llquaternion.h"
#include "llsingleton.h"
#include "v3math.h"

class LLVOAvatar;

class SLPoseCreator : public LLSingleton<SLPoseCreator>
{
    LLSINGLETON_EMPTY_CTOR(SLPoseCreator);
    LOG_CLASS(SLPoseCreator);

public:
    // Upload limits (llbvhconsts.h MAX_ANIM_DURATION; the uploader's priority range).
    static constexpr F32 MAX_DURATION = 60.f;
    static constexpr S32 MAX_PRIORITY = 4;

    struct Keyframe
    {
        F32 mTime = 0.f;                                  // seconds from the start
        std::map<std::string, LLQuaternion> mRotations;   // joint name -> local rotation
        std::set<std::string> mEdited;                    // Pose Studio's edited bones at capture
        LLVector3 mPelvisPosition;                        // mPelvis local position
    };

    struct Options
    {
        F32  mDuration = 0.f;        // 0 = the last keyframe's time (1 s for a single pose)
        bool mLoop = true;
        F32  mLoopIn = 0.f;          // seconds; loop out < 0 = the duration
        F32  mLoopOut = -1.f;
        S32  mPriority = 4;          // 0..MAX_PRIORITY (every joint uses it)
        F32  mEaseIn = 0.5f;
        F32  mEaseOut = 0.5f;
        U32  mHandPose = 1;          // LLHandMotion::eHandPose (1 = relaxed)
        bool mEditedOnly = true;     // only bones edited in some keyframe (else every bone)
        bool mPelvisPosition = true; // pelvis position keys (when it moves or was edited)
        std::string mEmote;          // "" = none (LL's facial emote names)
    };

    const std::vector<Keyframe>& getKeyframes() const { return mKeys; }
    // Captures the avatar's current pose at `time` (replacing a keyframe within 1 ms of
    // it); returns its index after sorting, -1 if nothing could be captured.
    S32  addKeyframe(LLVOAvatar* avatar, F32 time);
    bool recapture(S32 index, LLVOAvatar* avatar);
    void removeKeyframe(S32 index);
    // Moves a keyframe in time (re-sorted; returns the new index).
    S32  setKeyframeTime(S32 index, F32 time);
    void clear() { mKeys.clear(); }
    // Poses the avatar (through Pose Studio; it must be posing) to a keyframe's bones.
    bool applyKeyframe(S32 index, LLVOAvatar* avatar) const;

    // The .anim bytes for the current keyframes; false with a reason on error.
    bool buildAnim(const Options& opt, std::vector<U8>& out, std::string& error) const;
    bool exportAnim(const std::string& path, const Options& opt, std::string& error) const;

    // Bones written for these options (sorted by name).
    std::vector<std::string> exportedBones(const Options& opt) const;

private:
    bool capture(LLVOAvatar* avatar, Keyframe& key) const;
    std::vector<Keyframe> mKeys;
};

#endif // SL_POSECREATOR_H
