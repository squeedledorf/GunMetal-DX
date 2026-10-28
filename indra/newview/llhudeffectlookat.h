/**
 * @file llhudeffectlookat.h
 * @brief LLHUDEffectLookAt class definition
 *
 * $LicenseInfo:firstyear=2002&license=viewerlgpl$
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

#ifndef LL_LLHUDEFFECTLOOKAT_H
#define LL_LLHUDEFFECTLOOKAT_H

#include "llhudeffect.h"

class LLViewerObject;
class LLVOAvatar;
class LLAttentionSet;

typedef enum e_lookat_type
{
    LOOKAT_TARGET_NONE,
    LOOKAT_TARGET_IDLE,
    LOOKAT_TARGET_AUTO_LISTEN,
    LOOKAT_TARGET_FREELOOK,
    LOOKAT_TARGET_RESPOND,
    LOOKAT_TARGET_HOVER,
    LOOKAT_TARGET_CONVERSATION,
    LOOKAT_TARGET_SELECT,
    LOOKAT_TARGET_FOCUS,
    LOOKAT_TARGET_MOUSELOOK,
    LOOKAT_TARGET_CLEAR,
    LOOKAT_NUM_TARGETS
} ELookAtType;

class LLHUDEffectLookAt : public LLHUDEffect
{
public:
    friend class LLHUDObject;

    /*virtual*/ void markDead();
    /*virtual*/ void setSourceObject(LLViewerObject* objectp);

    bool setLookAt(ELookAtType target_type, LLViewerObject *object, LLVector3 position);
    void clearLookAtTarget();
    // Slipstream (#1121): the head's tilt about its look axis (radians; + = the left ear up),
    // from a VR headset; valid false: none. Sent to the sim in the effect's Color field (see
    // packData), under the same rate limit as the target; other Slipstream viewers tilt the
    // head with it, stock viewers never read that field for a look-at.
    void setHeadRoll(F32 radians, bool valid);

    ELookAtType getLookAtType() { return mTargetType; }
    const LLVector3& getTargetPos() { return mTargetPos; }
    const LLVector3d& getTargetOffset() { return mTargetOffsetGlobal; }
    bool calcTargetPosition();

protected:
    LLHUDEffectLookAt(const U8 type);
    ~LLHUDEffectLookAt();

    /*virtual*/ void update();
    /*virtual*/ void render();
    /*virtual*/ void packData(LLMessageSystem *mesgsys);
    /*virtual*/ void unpackData(LLMessageSystem *mesgsys, S32 blocknum);

    // lookat behavior has either target position or target object with offset
    void setTargetObjectAndOffset(LLViewerObject *objp, LLVector3d offset);
    void setTargetPosGlobal(const LLVector3d &target_pos_global);

public:
    static bool sDebugLookAt;

private:
    ELookAtType                 mTargetType;
    LLVector3d                  mTargetOffsetGlobal;
    LLVector3                   mLastSentOffsetGlobal;
    F32                         mKillTime;
    LLFrameTimer                mTimer;
    LLVector3                   mTargetPos;
    F32                         mLastSendTime;
    LLAttentionSet*             mAttentions;
    F32                         mTargetRoll = 0.f;      // #1121: radians (ANIM_DATA_LOOK_AT_ROLL points here)
    bool                        mHasRoll = false;
    bool                        mSentHasRoll = false;   // what the last packData encoded
    S32                         mSentRollDeg = 0;
};

#endif // LL_LLHUDEFFECTLOOKAT_H
