/**
 * @file llcontrolavatar.h
 * @brief Special dummy avatar used to drive rigged meshes.
 *
 * $LicenseInfo:firstyear=2017&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2017, Linden Research, Inc.
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

#ifndef LL_CONTROLAVATAR_H
#define LL_CONTROLAVATAR_H

#include "llvoavatar.h"
#include "llvovolume.h"

class LLControlAvatar:
    public LLVOAvatar
{
    LOG_CLASS(LLControlAvatar);

    using super = LLVOAvatar;

public:
    LLControlAvatar(const LLUUID &id, const LLPCode pcode, LLViewerRegion *regionp);
    virtual void initInstance(); // Called after construction to initialize the class.
    virtual void markDead();
    virtual ~LLControlAvatar();

    // If this is an attachment, return the avatar it is attached to. Otherwise NULL.
    virtual const LLVOAvatar *getAttachedAvatar() const;
    virtual LLVOAvatar *getAttachedAvatar();

    void getNewConstraintFixups(LLVector3& new_pos_constraint, F32& new_scale_constraint) const;
    void matchVolumeTransform();
    void updateVolumeGeom();

    void setGlobalScale(F32 scale);
    void recursiveScaleJoint(LLJoint *joint, F32 factor);
    static LLControlAvatar *createControlAvatar(LLVOVolume *obj);

    // Delayed kill so we don't make graphics pipeline unhappy calling
    // markDead() inside other graphics pipeline operations.
    void markForDeath();

    virtual void idleUpdate(LLAgent &agent, const F64 &time);
    virtual bool computeNeedsUpdate();
    virtual bool updateCharacter(LLAgent &agent);

    void getAnimatedVolumes(std::vector<LLVOVolume*>& volumes);
    void updateAnimations();


    virtual void    updateDebugText();

    virtual std::string getFullname() const;

    virtual bool shouldRenderRigged() const;

    virtual bool isImpostor();
    virtual bool isBuddy() const { return false; }

    bool mPlaying;

    F32 mGlobalScale;

    LLVOVolume *mRootVolp;

    bool mMarkedForDeath;

    LLVector3 mPositionConstraintFixup;
    F32 mScaleConstraintFixup;

    static const F32 MAX_LEGAL_OFFSET;
    static const F32 MAX_LEGAL_SIZE;

    static void onRegionChanged();
    bool mRegionChanged;
    static boost::signals2::connection sRegionChangedSlot;

    // ATTACHED ANIMESH frame alignment (debug readout: DebugAnimatedObjects hover text,
    // RenderVKAnimeshAttachDebug log). Written by matchVolumeTransform / LLCleanAnimStore.
    //   mVKAttachOrderLag: frames the wearer's skeleton was behind when this control
    //     avatar placed itself (0 = wearer already posed this frame; -1 = not attached).
    //   mVKAttachGpuFollow: this frame's root was derived ON THE GPU from the wearer's
    //     evaluated attachment joint (clean_anim.comp follow entry) -- same frame as
    //     the body, whatever the CPU's readback age.
    //   mVKAttachRbAge: GPU-animated wearer: age in frames of the CPU's copy of the
    //     attachment point (sparse readback) the CPU root was built from (-1 = CPU pose).
    S32  mVKAttachOrderLag = -1;
    bool mVKAttachGpuFollow = false;
    S32  mVKAttachRbAge = -1;
};

typedef std::map<LLUUID, S32> signaled_animation_map_t;
typedef std::map<LLUUID, signaled_animation_map_t> object_signaled_animation_map_t;

// Stores information about previously requested animations, by object id.
// Entries are recorded even for objects not yet known (the animation message can
// arrive before the object), and they must survive the viewer's own cache-culling
// kills (the sim does not resend animation state for an object it thinks we still
// have). They are dropped when the sim kills the object, or when its region goes.
class LLObjectSignaledAnimationMap: public LLSingleton<LLObjectSignaledAnimationMap>
{
    LLSINGLETON_EMPTY_CTOR(LLObjectSignaledAnimationMap);

public:
    object_signaled_animation_map_t mMap;

    object_signaled_animation_map_t& getMap() { return mMap; }
    void erase(const LLUUID& object_id) { mMap.erase(object_id); }
};

#endif //LL_CONTROLAVATAR_H
