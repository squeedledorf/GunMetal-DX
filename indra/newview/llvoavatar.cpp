/**
 * @File llvoavatar.cpp
 * @brief Implementation of LLVOAvatar class which is a derivation of LLViewerObject
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

#include "llviewerprecompiledheaders.h"


#include "llvoavatar.h"
#include "llgroupcolormap.h" // <GMDX>
#include "slaoengine.h"
#include "slsoundlog.h"
#include "llpolymorph.h"
#include "llcleaningest.h"
#include "llcleananimstore.h"
#include "llcleanskinstore.h"
#include "llcrossingkeep.h"           // #607 time-to-rezzed after a crossing re-create

#include <stdio.h>
#include <ctype.h>
#include <sstream>

#include "llaudioengine.h"
#include "noise.h"
#include "sound_ids.h"
#include "raytrace.h"

#include "lggcontactsets.h"
#include "llagent.h" //  Get state values from here
#include "llagentbenefits.h"
#include "llagentcamera.h"
#include "llvrcamera.h"   // #1074 VR camera: own avatar hidden as in mouselook
#include "llagentwearables.h"
#include "llanimationstates.h"
#include "llavatarnamecache.h"
#include "llavatarpropertiesprocessor.h"
#include "llavatarrendernotifier.h"
#include "llcontrolavatar.h"
#include "llexperiencecache.h"
#include "llphysicsmotion.h"
#include "llviewercontrol.h"
#include "llcallingcard.h"      // IDEVO for LLAvatarTracker
#include "lldriverparam.h"
#include "llpolyskeletaldistortion.h"
#include "lleditingmotion.h"
#include "llemote.h"
#include "llfilepicker.h"
#include "llfloatertools.h"
#include "llheadrotmotion.h"
#include "llhudeffecttrail.h"
#include "llhudmanager.h"
#include "llhudnametag.h"
#include "llhudtext.h"              // for mText/mDebugText
#include "llimview.h"
#include "llinitparam.h"
#include "llkeyframefallmotion.h"
#include "llkeyframestandmotion.h"
#include "llkeyframewalkmotion.h"
#include "llcallbacklist.h"
#include "llfilesystem.h"
#include "llmanipscale.h"  // for get_default_max_prim_scale()
#include "llmeshrepository.h"
#include "llmutelist.h"
#include "llmoveview.h"
#include "llnotificationsutil.h"
#include "llphysicsshapebuilderutil.h"
#include "llquantize.h"
#include "llrand.h"
#include "llregionhandle.h"
#include "llresmgr.h"
#include "llselectmgr.h"
#include "llslurl.h"
#include "lltargetingmotion.h"
#include "lltoolmgr.h"
#include "lltoolmorph.h"
#include "llviewercamera.h"
#include "llvkhitch.h"
#include "llviewertexlayer.h"
#include "llviewertexturelist.h"
#include "llviewermenu.h"
#include "llviewermenufile.h"
#include "llviewernetwork.h"
#include "llviewerobjectlist.h"
#include "llviewerparcelmgr.h"
#include "llviewerregion.h"
#include "llrender.h"
#include "llviewerstats.h"
#include "llviewerwearable.h"
#include "llvoavatarself.h"
#include "llderenderlist.h"     // #202
#include "gmdxattachprobe.h" // <GMDX>
#include "llvovolume.h"
#include "llvolumemgr.h"
#include "llworld.h"
#include "pipeline.h"
#include "llsky.h"
#include "llanimstatelabels.h"
#include "lltrans.h"
#include "llappearancemgr.h"
// [RLVa:KB] - Checked: RLVa-2.0.1
#include "rlvactions.h"
#include "rlvhandler.h"
#include "rlvmodifiers.h"
// [/RLVa:KB]

#include "llgesturemgr.h" //needed to trigger the voice gesticulations
#include "llvoiceclient.h"
#include "llvoicevisualizer.h" // Ventrella

#include "lldebugmessagebox.h"
#include "llsdutil.h"
#include "llsdserialize.h"
#include "llrender2dutils.h"
#include "llskinningutil.h"

#include "llperfstats.h"

#include <boost/lexical_cast.hpp>

extern F32 SPEED_ADJUST_MAX;
extern F32 SPEED_ADJUST_MAX_SEC;
extern F32 ANIM_SPEED_MAX;
extern F32 ANIM_SPEED_MIN;
extern U32 JOINT_COUNT_REQUIRED_FOR_FULLRIG;

const F32 MAX_HOVER_Z = 3.0;
const F32 MIN_HOVER_Z = -3.0;

const F32 MIN_ATTACHMENT_COMPLEXITY = 0.f;
const F32 DEFAULT_MAX_ATTACHMENT_COMPLEXITY = 1.0e6f;

// Unlike with 'self' avatar, server doesn't inform viewer about
// expected attachments so viewer has to wait to see if anything
// else will arrive
const F32 FIRST_APPEARANCE_CLOUD_MIN_DELAY = 3.f; // seconds
const F32 FIRST_APPEARANCE_CLOUD_MAX_DELAY = 15.f;
const F32 FIRST_APPEARANCE_CLOUD_IMPOSTOR_MODIFIER = 1.25f;

using namespace LLAvatarAppearanceDefines;

//-----------------------------------------------------------------------------
// Global constants
//-----------------------------------------------------------------------------
const LLUUID ANIM_AGENT_BODY_NOISE = LLUUID("9aa8b0a6-0c6f-9518-c7c3-4f41f2c001ad"); //"body_noise"
const LLUUID ANIM_AGENT_BREATHE_ROT = LLUUID("4c5a103e-b830-2f1c-16bc-224aa0ad5bc8");  //"breathe_rot"
const LLUUID ANIM_AGENT_EDITING = LLUUID("2a8eba1d-a7f8-5596-d44a-b4977bf8c8bb");  //"editing"
const LLUUID ANIM_AGENT_EYE = LLUUID("5c780ea8-1cd1-c463-a128-48c023f6fbea");  //"eye"
const LLUUID ANIM_AGENT_FLY_ADJUST = LLUUID("db95561f-f1b0-9f9a-7224-b12f71af126e");  //"fly_adjust"
const LLUUID ANIM_AGENT_HAND_MOTION = LLUUID("ce986325-0ba7-6e6e-cc24-b17c4b795578");  //"hand_motion"
const LLUUID ANIM_AGENT_HEAD_ROT = LLUUID("e6e8d1dd-e643-fff7-b238-c6b4b056a68d");  //"head_rot"
const LLUUID ANIM_AGENT_PELVIS_FIX = LLUUID("0c5dd2a2-514d-8893-d44d-05beffad208b");  //"pelvis_fix"
const LLUUID ANIM_AGENT_TARGET = LLUUID("0e4896cb-fba4-926c-f355-8720189d5b55");  //"target"
const LLUUID ANIM_AGENT_WALK_ADJUST = LLUUID("829bc85b-02fc-ec41-be2e-74cc6dd7215d");  //"walk_adjust"
const LLUUID ANIM_AGENT_PHYSICS_MOTION = LLUUID("7360e029-3cb8-ebc4-863e-212df440d987");  //"physics_motion"


//-----------------------------------------------------------------------------
// Constants
//-----------------------------------------------------------------------------
const F32 DELTA_TIME_MIN = 0.01f;   // we clamp measured delta_time to this
const F32 DELTA_TIME_MAX = 0.2f;    // range to insure stability of computations.

const F32 PELVIS_LAG_FLYING     = 0.22f;// pelvis follow half life while flying
const F32 PELVIS_LAG_WALKING    = 0.4f; // ...while walking
const F32 PELVIS_LAG_MOUSELOOK = 0.15f;
const F32 MOUSELOOK_PELVIS_FOLLOW_FACTOR = 0.5f;
const F32 TORSO_NOISE_AMOUNT = 1.0f;    // Amount of deviation from up-axis, in degrees
const F32 TORSO_NOISE_SPEED = 0.2f; // Time scale factor on torso noise.

const F32 BREATHE_ROT_MOTION_STRENGTH = 0.05f;

const S32 MIN_REQUIRED_PIXEL_AREA_BODY_NOISE = 10000;
const S32 MIN_REQUIRED_PIXEL_AREA_BREATHE = 10000;
const S32 MIN_REQUIRED_PIXEL_AREA_PELVIS_FIX = 40;

const S32 TEX_IMAGE_SIZE_OTHER = 512 / 4;  // The size of local textures for other (!isSelf()) avatars

const F32 HEAD_MOVEMENT_AVG_TIME = 0.9f;

const S32 MORPH_MASK_REQUESTED_DISCARD = 0;

const F32 MAX_STANDOFF_FROM_ORIGIN = 3;
const F32 MAX_STANDOFF_DISTANCE_CHANGE = 32;

// Discard level at which to switch to baked textures
// Should probably be 4 or 3, but didn't want to change it while change other logic - SJB
const S32 SWITCH_TO_BAKED_DISCARD = 5;

const F32 APPEARANCE_MORPH_TIME = 0.65f;
const F32 TIME_BEFORE_MESH_CLEANUP = 5.f; // seconds
const S32 AVATAR_RELEASE_THRESHOLD = 10; // number of avatar instances before releasing memory
const F32 FOOT_GROUND_COLLISION_TOLERANCE = 0.25f;
const F32 AVATAR_LOD_TWEAK_RANGE = 0.7f;
const S32 MAX_BUBBLE_CHAT_LENGTH = DB_CHAT_MSG_STR_LEN;
const S32 MAX_BUBBLE_CHAT_UTTERANCES = 12;
const F32 CHAT_FADE_TIME = 8.0;
const F32 BUBBLE_CHAT_TIME = CHAT_FADE_TIME * 3.f;
const F32 NAMETAG_UPDATE_THRESHOLD = 0.3f;
const F32 NAMETAG_VERTICAL_SCREEN_OFFSET = 25.f;
const F32 NAMETAG_VERT_OFFSET_WEIGHT = 0.17f;

const U32 LLVOAvatar::VISUAL_COMPLEXITY_UNKNOWN = 0;
const F64 HUD_OVERSIZED_TEXTURE_DATA_SIZE = 1024 * 1024;

const F32 MAX_TEXTURE_WAIT_TIME_SEC = 60;
const F32 MAX_ATTACHMENT_WAIT_TIME_SEC = 60;

enum ERenderName
{
    RENDER_NAME_NEVER,
    RENDER_NAME_ALWAYS,
    RENDER_NAME_FADE
};

#define JELLYDOLLS_SHOULD_IMPOSTOR

//-----------------------------------------------------------------------------
// Callback data
//-----------------------------------------------------------------------------

struct LLTextureMaskData
{
    LLTextureMaskData( const LLUUID& id ) :
        mAvatarID(id),
        mLastDiscardLevel(S32_MAX)
    {}
    LLUUID              mAvatarID;
    S32                 mLastDiscardLevel;
};

/*********************************************************************************
 **                                                                             **
 ** Begin private LLVOAvatar Support classes
 **
 **/


struct LLAppearanceMessageContents: public LLRefCount
{
    LLAppearanceMessageContents():
        mAppearanceVersion(-1),
        mParamAppearanceVersion(-1),
        mCOFVersion(LLViewerInventoryCategory::VERSION_UNKNOWN)
    {
    }
    LLTEContents mTEContents;
    S32 mAppearanceVersion;
    S32 mParamAppearanceVersion;
    S32 mCOFVersion;
    // For future use:
    //U32 appearance_flags = 0;
    std::vector<F32> mParamWeights;
    std::vector<LLVisualParam*> mParams;
    LLVector3 mHoverOffset;
    bool mHoverOffsetWasSet;
};


//-----------------------------------------------------------------------------
// class LLBodyNoiseMotion
//-----------------------------------------------------------------------------
class LLBodyNoiseMotion :
    public LLMotion
{
public:
    // Constructor
    LLBodyNoiseMotion(const LLUUID &id)
        : LLMotion(id)
    {
        mName = "body_noise";
        mTorsoState = new LLJointState;
    }

    // Destructor
    virtual ~LLBodyNoiseMotion() { }

public:
    //-------------------------------------------------------------------------
    // functions to support MotionController and MotionRegistry
    //-------------------------------------------------------------------------
    // static constructor
    // all subclasses must implement such a function and register it
    static LLMotion *create(const LLUUID &id) { return new LLBodyNoiseMotion(id); }

public:
    //-------------------------------------------------------------------------
    // animation callbacks to be implemented by subclasses
    //-------------------------------------------------------------------------

    // motions must specify whether or not they loop
    virtual bool getLoop() { return true; }

    // motions must report their total duration
    virtual F32 getDuration() { return 0.0; }

    // motions must report their "ease in" duration
    virtual F32 getEaseInDuration() { return 0.0; }

    // motions must report their "ease out" duration.
    virtual F32 getEaseOutDuration() { return 0.0; }

    // motions must report their priority
    virtual LLJoint::JointPriority getPriority() { return LLJoint::HIGH_PRIORITY; }

    virtual LLMotionBlendType getBlendType() { return ADDITIVE_BLEND; }

    // called to determine when a motion should be activated/deactivated based on avatar pixel coverage
    virtual F32 getMinPixelArea() { return MIN_REQUIRED_PIXEL_AREA_BODY_NOISE; }

    // run-time (post constructor) initialization,
    // called after parameters have been set
    // must return true to indicate success and be available for activation
    virtual LLMotionInitStatus onInitialize(LLCharacter *character)
    {
        if( !mTorsoState->setJoint( character->getJoint("mTorso") ))
        {
            return STATUS_FAILURE;
        }

        mTorsoState->setUsage(LLJointState::ROT);

        addJointState( mTorsoState );
        return STATUS_SUCCESS;
    }

    // called when a motion is activated
    // must return true to indicate success, or else
    // it will be deactivated
    virtual bool onActivate() { return true; }

    // called per time step
    // must return true while it is active, and
    // must return false when the motion is completed.
    virtual bool onUpdate(F32 time, U8* joint_mask)
    {
        LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;
        F32 nx[2];
        nx[0]=time*TORSO_NOISE_SPEED;
        nx[1]=0.0f;
        F32 ny[2];
        ny[0]=0.0f;
        ny[1]=time*TORSO_NOISE_SPEED;
        F32 noiseX = noise2(nx);
        F32 noiseY = noise2(ny);

        F32 rx = TORSO_NOISE_AMOUNT * DEG_TO_RAD * noiseX / 0.42f;
        F32 ry = TORSO_NOISE_AMOUNT * DEG_TO_RAD * noiseY / 0.42f;
        LLQuaternion tQn;
        tQn.setQuat( rx, ry, 0.0f );
        mTorsoState->setRotation( tQn );

        return true;
    }

    // called when a motion is deactivated
    virtual void onDeactivate() {}

private:
    //-------------------------------------------------------------------------
    // joint states to be animated
    //-------------------------------------------------------------------------
    LLPointer<LLJointState> mTorsoState;
};

//-----------------------------------------------------------------------------
// class LLBreatheMotionRot
//-----------------------------------------------------------------------------
class LLBreatheMotionRot :
    public LLMotion
{
public:
    // Constructor
    LLBreatheMotionRot(const LLUUID &id) :
        LLMotion(id),
        mBreatheRate(1.f),
        mCharacter(NULL)
    {
        mName = "breathe_rot";
        mChestState = new LLJointState;
    }

    // Destructor
    virtual ~LLBreatheMotionRot() {}

public:
    //-------------------------------------------------------------------------
    // functions to support MotionController and MotionRegistry
    //-------------------------------------------------------------------------
    // static constructor
    // all subclasses must implement such a function and register it
    static LLMotion *create(const LLUUID &id) { return new LLBreatheMotionRot(id); }

public:
    //-------------------------------------------------------------------------
    // animation callbacks to be implemented by subclasses
    //-------------------------------------------------------------------------

    // motions must specify whether or not they loop
    virtual bool getLoop() { return true; }

    // motions must report their total duration
    virtual F32 getDuration() { return 0.0; }

    // motions must report their "ease in" duration
    virtual F32 getEaseInDuration() { return 0.0; }

    // motions must report their "ease out" duration.
    virtual F32 getEaseOutDuration() { return 0.0; }

    // motions must report their priority
    virtual LLJoint::JointPriority getPriority() { return LLJoint::MEDIUM_PRIORITY; }

    virtual LLMotionBlendType getBlendType() { return NORMAL_BLEND; }

    // called to determine when a motion should be activated/deactivated based on avatar pixel coverage
    virtual F32 getMinPixelArea() { return MIN_REQUIRED_PIXEL_AREA_BREATHE; }

    // run-time (post constructor) initialization,
    // called after parameters have been set
    // must return true to indicate success and be available for activation
    virtual LLMotionInitStatus onInitialize(LLCharacter *character)
    {
        mCharacter = character;
        bool success = true;

        if ( !mChestState->setJoint( character->getJoint( "mChest" ) ) )
        {
            success = false;
        }

        if ( success )
        {
            mChestState->setUsage(LLJointState::ROT);
            addJointState( mChestState );
        }

        if ( success )
        {
            return STATUS_SUCCESS;
        }
        else
        {
            return STATUS_FAILURE;
        }
    }

    // called when a motion is activated
    // must return true to indicate success, or else
    // it will be deactivated
    virtual bool onActivate() { return true; }

    // called per time step
    // must return true while it is active, and
    // must return false when the motion is completed.
    virtual bool onUpdate(F32 time, U8* joint_mask)
    {
        LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;
        mBreatheRate = 1.f;

        F32 breathe_amt = (sinf(mBreatheRate * time) * BREATHE_ROT_MOTION_STRENGTH);

        mChestState->setRotation(LLQuaternion(breathe_amt, LLVector3(0.f, 1.f, 0.f)));

        return true;
    }

    // called when a motion is deactivated
    virtual void onDeactivate() {}

private:
    //-------------------------------------------------------------------------
    // joint states to be animated
    //-------------------------------------------------------------------------
    LLPointer<LLJointState> mChestState;
    F32                 mBreatheRate;
    LLCharacter*        mCharacter;
};

//-----------------------------------------------------------------------------
// class LLPelvisFixMotion
//-----------------------------------------------------------------------------
class LLPelvisFixMotion :
    public LLMotion
{
public:
    // Constructor
    LLPelvisFixMotion(const LLUUID &id)
        : LLMotion(id), mCharacter(NULL)
    {
        mName = "pelvis_fix";

        mPelvisState = new LLJointState;
    }

    // Destructor
    virtual ~LLPelvisFixMotion() { }

public:
    //-------------------------------------------------------------------------
    // functions to support MotionController and MotionRegistry
    //-------------------------------------------------------------------------
    // static constructor
    // all subclasses must implement such a function and register it
    static LLMotion *create(const LLUUID& id) { return new LLPelvisFixMotion(id); }

public:
    //-------------------------------------------------------------------------
    // animation callbacks to be implemented by subclasses
    //-------------------------------------------------------------------------

    // motions must specify whether or not they loop
    virtual bool getLoop() { return true; }

    // motions must report their total duration
    virtual F32 getDuration() { return 0.0; }

    // motions must report their "ease in" duration
    virtual F32 getEaseInDuration() { return 0.5f; }

    // motions must report their "ease out" duration.
    virtual F32 getEaseOutDuration() { return 0.5f; }

    // motions must report their priority
    virtual LLJoint::JointPriority getPriority() { return LLJoint::LOW_PRIORITY; }

    virtual LLMotionBlendType getBlendType() { return NORMAL_BLEND; }

    // called to determine when a motion should be activated/deactivated based on avatar pixel coverage
    virtual F32 getMinPixelArea() { return MIN_REQUIRED_PIXEL_AREA_PELVIS_FIX; }

    // run-time (post constructor) initialization,
    // called after parameters have been set
    // must return true to indicate success and be available for activation
    virtual LLMotionInitStatus onInitialize(LLCharacter *character)
    {
        mCharacter = character;

        if (!mPelvisState->setJoint( character->getJoint("mPelvis")))
        {
            return STATUS_FAILURE;
        }

        mPelvisState->setUsage(LLJointState::POS);

        addJointState( mPelvisState );
        return STATUS_SUCCESS;
    }

    // called when a motion is activated
    // must return true to indicate success, or else
    // it will be deactivated
    virtual bool onActivate() { return true; }

    // called per time step
    // must return true while it is active, and
    // must return false when the motion is completed.
    virtual bool onUpdate(F32 time, U8* joint_mask)
    {
        LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;
        mPelvisState->setPosition(LLVector3::zero);

        return true;
    }

    // called when a motion is deactivated
    virtual void onDeactivate() {}

private:
    //-------------------------------------------------------------------------
    // joint states to be animated
    //-------------------------------------------------------------------------
    LLPointer<LLJointState> mPelvisState;
    LLCharacter* mCharacter;
};

/**
 **
 ** End LLVOAvatar Support classes
 **                                                                             **
 *********************************************************************************/


//-----------------------------------------------------------------------------
// Static Data
//-----------------------------------------------------------------------------
U32 LLVOAvatar::sMaxNonImpostors = 12; // Set from RenderAvatarMaxNonImpostors
bool LLVOAvatar::sLimitNonImpostors = false; // True unless RenderAvatarMaxNonImpostors is 0 (unlimited)
F32 LLVOAvatar::sRenderDistance = 256.f;
S32 LLVOAvatar::sNumVisibleAvatars = 0;
S32 LLVOAvatar::sNumLODChangesThisFrame = 0;
std::atomic<S32> LLVOAvatar::sVKClusterRiggedLOD{-1};   // #798 (LLCleanRendererVK::clusterTick)

const LLUUID LLVOAvatar::sStepSoundOnLand("e8af4a28-aa83-4310-a7c4-c047e15ea0df");
const LLUUID LLVOAvatar::sStepSounds[LL_MCODE_END] =
{
    SND_STONE_RUBBER,
    SND_METAL_RUBBER,
    SND_GLASS_RUBBER,
    SND_WOOD_RUBBER,
    SND_FLESH_RUBBER,
    SND_RUBBER_PLASTIC,
    SND_RUBBER_RUBBER
};

S32 LLVOAvatar::sRenderName = RENDER_NAME_ALWAYS;
bool LLVOAvatar::sRenderGroupTitles = true;
S32 LLVOAvatar::sNumVisibleChatBubbles = 0;
bool LLVOAvatar::sDebugInvisible = false;
bool LLVOAvatar::sShowAttachmentPoints = false;
bool LLVOAvatar::sShowAnimationDebug = false;
bool LLVOAvatar::sVisibleInFirstPerson = false;

// static
bool LLVOAvatar::selfHiddenInMainView()
{
    // <SS:Skoomastream> OTS reports cameraMouselook() too, but is a third-person view.
    // RLV @showself hides the whole own avatar, worn attachments included (HUDs stay), as
    // LL's pipeline did; the system body alone is handled by needsRenderAvatar.
    // Game control: a flycam started in mouselook stops hiding the avatar once it is
    // out of the flycam's view (LLAgentCamera::isHidingAvatarForFirstPerson()).
    // #1074: the VR camera (eye at the head, like mouselook) follows "Show me in Mouselook" too:
    // from inside, a mesh head / hair would block the view. False on the desktop.
    return (!sVisibleInFirstPerson && ((gAgentCamera.isHidingAvatarForFirstPerson() && !gAgentCamera.cameraOTS()) || LLVRCamera::firstPerson())) ||
           (RlvActions::isRlvEnabled() && RlvActions::hasBehaviour(RLV_BHVR_SHOWSELF));
}

// static
bool LLVOAvatar::isHiddenOwnAttachment(const LLViewerObject* obj)
{
    if (!obj || !selfHiddenInMainView() || !isAgentAvatarValid()) return false;
    // Worn by us (root or child prim: the avatar is an ancestor, as cleanIsSelfPriority
    // tags the instances the renderer hides), and not on a HUD point (judged by the root).
    LLViewerObject* o = const_cast<LLViewerObject*>(obj);
    if (o->getAvatarAncestor() != gAgentAvatarp.get()) return false;
    const LLViewerObject* root = o->getRootEdit();
    return !(root ? root : o)->isHUDAttachment();
}
F32 LLVOAvatar::sLODFactor = 1.f;
F32 LLVOAvatar::sPhysicsLODFactor = 1.f;
bool LLVOAvatar::sJointDebug            = false;
F32 LLVOAvatar::sUnbakedTime = 0.f;
F32 LLVOAvatar::sUnbakedUpdateTime = 0.f;
F32 LLVOAvatar::sGreyTime = 0.f;
F32 LLVOAvatar::sGreyUpdateTime = 0.f;
LLPointer<LLViewerTexture> LLVOAvatar::sCloudTexture = NULL;

//-----------------------------------------------------------------------------
// Helper functions
//-----------------------------------------------------------------------------
static F32 calc_bouncy_animation(F32 x);

//-----------------------------------------------------------------------------
// LLVOAvatar()
//-----------------------------------------------------------------------------
LLVOAvatar::LLVOAvatar(const LLUUID& id,
    const LLPCode pcode,
    LLViewerRegion* regionp) :
    LLAvatarAppearance(&gAgentWearables),
    LLViewerObject(id, pcode, regionp),
    mSpecialRenderMode(0),
    mAttachmentSurfaceArea(0.f),
    mAttachmentVisibleTriangleCount(0),
    mAttachmentEstTriangleCount(0.f),
    mReportedVisualComplexity(VISUAL_COMPLEXITY_UNKNOWN),
    mTurning(false),
    mLastSkeletonSerialNum(0),
    mIsSitting(false),
    mTimeVisible(),
    mTyping(false),
    mMeshValid(false),
    mVisible(false),
    mLastImpostorUpdateFrameTime(0.f),
    mLastImpostorUpdateReason(0),
    mBelowWater(false),
    mLastAppearanceBlendTime(0.f),
    mAppearanceAnimating(false),
    mNameIsSet(false),
    mTitle(),
    mNameAway(false),
    mNameDoNotDisturb(false),
    mNameMute(false),
    mNameAppearance(false),
    mNameFriend(false),
    mNameAlpha(0.f),
    mRenderGroupTitles(sRenderGroupTitles),
    mNameCloud(false),
    mFirstTEMessageReceived(false),
    mFirstAppearanceMessageReceived(false),
    mCulled(false),
    mVisibilityRank(0),
    mUpdatePeriod(1),
    mOverallAppearance(AOA_INVISIBLE),
    mVisualComplexityStale(true),
    mVisuallyMuteSetting(AV_RENDER_NORMALLY),
    mMutedAVColor(LLColor4::white /* used for "uninitialize" */),
    mFirstFullyVisible(true),
    mWaitingForMeshes(false),
    mFirstDecloudTime(-1.f),
    mFullyLoaded(false),
    mPreviousFullyLoaded(false),
    mFullyLoadedInitialized(false),
    mLastCloudAttachmentCount(-1),
    mFullyLoadedFrameCounter(0),
    mVisualComplexity(VISUAL_COMPLEXITY_UNKNOWN),
    mLoadedCallbacksPaused(false),
    mLoadedCallbackTextures(0),
    mLastRezzedStatus(-1),
    mIsEditingAppearance(false),
    mUseLocalAppearance(false),
    mLastUpdateRequestCOFVersion(-1),
    mLastUpdateReceivedCOFVersion(-1),
    mCachedMuteListUpdateTime(0),
    mCachedInMuteList(false),
    mIsControlAvatar(false),
    mIsUIAvatar(false),
    mEnableDefaultMotions(true)
{
    LL_DEBUGS("AvatarRender") << "LLVOAvatar Constructor (0x" << this << ") id:" << mID << LL_ENDL;

    //VTResume();  // VTune
    setHoverOffset(LLVector3(0.0, 0.0, 0.0));

    // mVoiceVisualizer is created by the hud effects manager and uses the HUD Effects pipeline
    const bool needsSendToSim = false; // currently, this HUD effect doesn't need to pack and unpack data to do its job
    mVoiceVisualizer = ( LLVoiceVisualizer *)LLHUDManager::getInstance()->createViewerEffect( LLHUDObject::LL_HUD_EFFECT_VOICE_VISUALIZER, needsSendToSim );

    LL_DEBUGS("Avatar","Message") << "LLVOAvatar Constructor (0x" << this << ") id:" << mID << LL_ENDL;
    mPelvisp = NULL;

    mDirtyMesh = 2; // Dirty geometry, need to regenerate.
    mMeshTexturesDirty = false;
    mHeadp = NULL;


    // set up animation variables
    mSpeed = 0.f;
    setAnimationData(LLCharacter::ANIM_DATA_SPEED, &mSpeed);

    mNeedsImpostorUpdate = true;
    mLastImpostorUpdateReason = 0;
    mNeedsAnimUpdate = true;

    mNeedsExtentUpdate = true;

    mImpostorDistance = 0;
    mImpostorPixelArea = 0;

    setNumTEs(TEX_NUM_INDICES);

    mbCanSelect = true;

    mSignaledAnimations.clear();
    mPlayingAnimations.clear();

    mWasOnGroundLeft = false;
    mWasOnGroundRight = false;

    mTimeLast = 0.0f;
    mSpeedAccum = 0.0f;

    mInAir = false;

    mStepOnLand = true;
    mStepMaterial = 0;

    mLipSyncActive = false;
    mOohMorph      = NULL;
    mAahMorph      = NULL;

    mCurrentGesticulationLevel = 0;

    mFirstAppearanceMessageTimer.reset();
    mRuthTimer.reset();
    mRuthDebugTimer.reset();
    mDebugExistenceTimer.reset();
    mLastAppearanceMessageTimer.reset();

    mVisuallyMuteSetting = LLVOAvatar::VisualMuteSettings(LLRenderMuteList::getInstance()->getSavedVisualMuteSetting(getID()));

    sInstances.push_back(this);
}

std::string LLVOAvatar::avString() const
{
    if (isControlAvatar())
    {
        return " " + getFullname() + " ";
    }

    std::string status = LLVOAvatar::rezStatusToString(getRezzedStatus());
    return " Avatar '" + getDebugName() + "' " + status + " ";
}

void LLVOAvatar::debugAvatarRezTime(std::string notification_name, std::string comment)
{
    if (gDisconnected)
    {
        // If we disconected, these values are likely to be invalid and
        // avString() might crash due to a dead sAvatarDictionary
        return;
    }

    LL_INFOS("Avatar") << "REZTIME: [ " << (U32)mDebugExistenceTimer.getElapsedTimeF32()
                       << "sec ]"
                       << avString()
                       << "RuthTimer " << (U32)mRuthDebugTimer.getElapsedTimeF32()
                       << " Notification " << notification_name
                       << " : " << comment
                       << LL_ENDL;

    if (gSavedSettings.getBOOL("DebugAvatarRezTime"))
    {
        LLSD args;
        args["EXISTENCE"] = llformat("%d", (U32)mDebugExistenceTimer.getElapsedTimeF32());
        args["TIME"] = llformat("%d", (U32)mRuthDebugTimer.getElapsedTimeF32());
        args["NAME"] = getFullname();
        LLNotificationsUtil::add(notification_name, args);
    }
}

//------------------------------------------------------------------------
// LLVOAvatar::~LLVOAvatar()
//------------------------------------------------------------------------
LLVOAvatar::~LLVOAvatar()
{
    // The destructor body only (members and the LLAvatarAppearance / LLCharacter /
    // LLViewerObject chain come after it): splits the deferred destruction's cost
    // (LL_destroyObjects, ObjDestroy's slowest destructor) between this and the rest.
    VK_HITCH_SCOPE("av_dtorBody");
    sInstances.remove(this);
    if (LLCleanAnimStore::instanceExists()) LLCleanAnimStore::instance().forgetAvatar(this);

    if (!mFullyLoaded)
    {
        debugAvatarRezTime("AvatarRezLeftCloudNotification", "left after ruth seconds as cloud");
    }
    else
    {
        debugAvatarRezTime("AvatarRezLeftNotification", "left sometime after declouding");
    }

    if (mTuned)
    {
        LLPerfStats::tunedAvatars--;
        mTuned = false;
    }


    logPendingPhases();

    LL_DEBUGS("Avatar") << "LLVOAvatar Destructor (0x" << this << ") id:" << mID << LL_ENDL;

    std::for_each(mAttachmentPoints.begin(), mAttachmentPoints.end(), DeletePairedPointer());
    mAttachmentPoints.clear();
    mOccupiedAttachCache.clear();
    mOccupiedAttachGen = 0;

    mDead = true;

    mAnimationSources.clear();
    LLLoadedCallbackEntry::cleanUpCallbackList(&mCallbackTextureList) ;

    getPhases().clearPhases();

    LL_DEBUGS() << "LLVOAvatar Destructor end" << LL_ENDL;
}

void LLVOAvatar::markDead()
{
    if (mNameText)
    {
        mNameText->markDead();
        mNameText = NULL;
        sNumVisibleChatBubbles--;
    }
    mVoiceVisualizer->markDead();
    LLLoadedCallbackEntry::cleanUpCallbackList(&mCallbackTextureList) ;
    LLCleanAnimStore::instance().forgetAvatar(this);
    LLViewerObject::markDead();
}


bool LLVOAvatar::isFullyBaked()
{
    if (mIsDummy) return true;
    if (getNumTEs() == 0) return false;

    for (U32 i = 0; i < mBakedTextureDatas.size(); i++)
    {
        if (!isTextureDefined(mBakedTextureDatas[i].mTextureIndex)
            && ((i != BAKED_SKIRT) || isWearingWearableType(LLWearableType::WT_SKIRT))
            && (i != BAKED_LEFT_ARM) && (i != BAKED_LEFT_LEG) && (i != BAKED_AUX1) && (i != BAKED_AUX2) && (i != BAKED_AUX3))
        {
            return false;
        }
    }
    return true;
}

bool LLVOAvatar::isFullyTextured() const
{
    for (S32 i = 0; i < mMeshLOD.size(); i++)
    {
        LLAvatarJoint* joint = mMeshLOD[i];
        if (i==MESH_ID_SKIRT && !isWearingWearableType(LLWearableType::WT_SKIRT))
        {
            continue; // don't care about skirt textures if we're not wearing one.
        }
        if (!joint)
        {
            continue; // nonexistent LOD OK.
        }
        avatar_joint_mesh_list_t::iterator meshIter = joint->mMeshParts.begin();
        if (meshIter != joint->mMeshParts.end())
        {
            LLAvatarJointMesh *mesh = (*meshIter);
            if (!mesh)
            {
                continue; // nonexistent mesh OK
            }
            if (mesh->hasTextureData())
            {
                continue; // Mesh exists and has a baked texture.
            }
            if (mesh->hasComposite())
            {
                continue; // Mesh exists and has a composite texture.
            }
            // Fail
            return false;
        }
    }
    return true;
}

bool LLVOAvatar::hasGray() const
{
    return !getHasMissingParts() && !isFullyTextured();
}

S32 LLVOAvatar::getRezzedStatus() const
{
    if (getHasMissingParts()) return 0;
    bool textured = isFullyTextured();
    bool all_baked_loaded = allBakedTexturesCompletelyDownloaded();
    if (textured && all_baked_loaded && getAttachmentCount() == mSimAttachments.size()) return 4;
    if (textured && all_baked_loaded) return 3;
    if (textured) return 2;
    llassert(hasGray());
    return 1; // gray
}

void LLVOAvatar::deleteLayerSetCaches(bool clearAll)
{
    for (U32 i = 0; i < mBakedTextureDatas.size(); i++)
    {
        if (mBakedTextureDatas[i].mTexLayerSet)
        {
            // ! BACKWARDS COMPATIBILITY !
            // Can be removed after hair baking is mandatory on the grid
            if ((i != BAKED_HAIR || isSelf()) && !clearAll)
            {
                mBakedTextureDatas[i].mTexLayerSet->deleteCaches();
            }
        }
    }
}

// static
bool LLVOAvatar::areAllNearbyInstancesBaked(S32& grey_avatars)
{
    grey_avatars = 0;
    for (LLCharacter* character : LLCharacter::sInstances)
    {
        LLVOAvatar* inst = (LLVOAvatar*)character;
        if (!inst->isDead() && inst->mHasGrey && !inst->isFullyBaked())
        {
            ++grey_avatars;
        }
    }
    return grey_avatars == 0;
}

// static
void LLVOAvatar::getNearbyRezzedStats(std::vector<S32>& counts, F32& avg_cloud_time, S32& cloud_avatars, S32& pending_meshes, S32& control_avatars)
{
    counts.clear();
    counts.resize(5);
    avg_cloud_time = 0;
    cloud_avatars = 0;
    pending_meshes = 0;
    control_avatars = 0;
    S32 count_avg = 0;

    for (LLCharacter* character : LLCharacter::sInstances)
    {
        LLVOAvatar* inst = (LLVOAvatar*)character;
        if (inst && !inst->isUIAvatar() && !inst->isSelf())
        {
            if (inst->isControlAvatar())
            {
                control_avatars++;
            }
            else
            {
                S32 rez_status = inst->getRezzedStatus();
                counts[rez_status]++;
                F32 time = inst->getFirstDecloudTime();
                if (time >= 0)
                {
                    avg_cloud_time += time;
                    count_avg++;
                }
                if (!inst->isFullyLoaded() || time < 0)
                {
                    // still renders as cloud
                    cloud_avatars++;
                    if (rez_status >= 4
                        && inst->mWaitingForMeshes)
                    {
                        pending_meshes++;
                    }
                }
            }
        }
    }

    if (count_avg > 0)
    {
        avg_cloud_time /= count_avg;
    }
}

// static
std::string LLVOAvatar::rezStatusToString(S32 rez_status)
{
    switch (rez_status)
    {
    case 0:
        return "missing parts";
    case 1:
        return "gray";
    case 2:
        return "downloading baked";
    case 3:
        return "loading attachments";
    case 4:
        return "full";
    }
    return "unknown";
}

// static
void LLVOAvatar::dumpBakedStatus()
{
    LLVector3d camera_pos_global = gAgentCamera.getCameraPositionGlobal();

    for (LLCharacter* character : LLCharacter::sInstances)
    {
        LLVOAvatar* inst = (LLVOAvatar*)character;
        LL_INFOS() << "Avatar ";

        LLNameValue* firstname = inst->getNVPair("FirstName");
        LLNameValue* lastname = inst->getNVPair("LastName");

        if( firstname )
        {
            LL_CONT << firstname->getString();
        }
        if( lastname )
        {
            LL_CONT << " " << lastname->getString();
        }

        LL_CONT << " " << inst->mID;

        if( inst->isDead() )
        {
            LL_CONT << " DEAD ("<< inst->getNumRefs() << " refs)";
        }

        if( inst->isSelf() )
        {
            LL_CONT << " (self)";
        }


        F64 dist_to_camera = (inst->getPositionGlobal() - camera_pos_global).length();
        LL_CONT << " " << dist_to_camera << "m ";

        LL_CONT << " " << inst->mPixelArea << " pixels";

        if( inst->isVisible() )
        {
            LL_CONT << " (visible)";
        }
        else
        {
            LL_CONT << " (not visible)";
        }

        if( inst->isFullyBaked() )
        {
            LL_CONT << " Baked";
        }
        else
        {
            LL_CONT << " Unbaked (";

            for (LLAvatarAppearanceDictionary::BakedTextures::const_iterator iter = LLAvatarAppearance::getDictionary()->getBakedTextures().begin();
                 iter != LLAvatarAppearance::getDictionary()->getBakedTextures().end();
                 ++iter)
            {
                const LLAvatarAppearanceDictionary::BakedEntry *baked_dict = iter->second;
                const ETextureIndex index = baked_dict->mTextureIndex;
                if (!inst->isTextureDefined(index))
                {
                    LL_CONT << " " << (LLAvatarAppearance::getDictionary()->getTexture(index) ? LLAvatarAppearance::getDictionary()->getTexture(index)->mName : "");
                }
            }
            LL_CONT << " ) " << inst->getUnbakedPixelAreaRank();
            if( inst->isCulled() )
            {
                LL_CONT << " culled";
            }
        }
        LL_CONT << LL_ENDL;
    }
}

//static
void LLVOAvatar::restoreGL()
{
    if (!isAgentAvatarValid()) return;

    gAgentAvatarp->setCompositeUpdatesEnabled(true);
    for (U32 i = 0; i < gAgentAvatarp->mBakedTextureDatas.size(); i++)
    {
        gAgentAvatarp->invalidateComposite(gAgentAvatarp->getTexLayerSet(i));
    }
    gAgentAvatarp->updateMeshTextures();
}

//static
void LLVOAvatar::destroyGL()
{
    deleteCachedImages();
}

// static
void LLVOAvatar::deleteCachedImages(bool clearAll)
{
    if (LLViewerTexLayerSet::sHasCaches)
    {
        for (LLCharacter* character : LLCharacter::sInstances)
        {
            ((LLVOAvatar*)character)->deleteLayerSetCaches(clearAll);
        }
        LLViewerTexLayerSet::sHasCaches = false;
    }
    LLTexLayerStaticImageList::getInstance()->deleteCachedImages();
}


//------------------------------------------------------------------------
// static
// LLVOAvatar::initClass()
//------------------------------------------------------------------------
void LLVOAvatar::initClass()
{
    gAnimLibrary.animStateSetString(ANIM_AGENT_BODY_NOISE,"body_noise");
    gAnimLibrary.animStateSetString(ANIM_AGENT_BREATHE_ROT,"breathe_rot");
    gAnimLibrary.animStateSetString(ANIM_AGENT_PHYSICS_MOTION,"physics_motion");
    gAnimLibrary.animStateSetString(ANIM_AGENT_EDITING,"editing");
    gAnimLibrary.animStateSetString(ANIM_AGENT_EYE,"eye");
    gAnimLibrary.animStateSetString(ANIM_AGENT_FLY_ADJUST,"fly_adjust");
    gAnimLibrary.animStateSetString(ANIM_AGENT_HAND_MOTION,"hand_motion");
    gAnimLibrary.animStateSetString(ANIM_AGENT_HEAD_ROT,"head_rot");
    gAnimLibrary.animStateSetString(ANIM_AGENT_PELVIS_FIX,"pelvis_fix");
    gAnimLibrary.animStateSetString(ANIM_AGENT_TARGET,"target");
    gAnimLibrary.animStateSetString(ANIM_AGENT_WALK_ADJUST,"walk_adjust");

    // Where should this be set initially?
    LLJoint::setDebugJointNames(gSavedSettings.getString("DebugAvatarJoints"));

    LLControlAvatar::sRegionChangedSlot = gAgent.addRegionChangedCallback(&LLControlAvatar::onRegionChanged);

    sCloudTexture = LLViewerTextureManager::getFetchedTextureFromFile("SoftDotNoBack.png");
}


void LLVOAvatar::cleanupClass()
{
    // Release before exit: a static holding a texture past LLAppViewer::cleanup() is
    // destroyed in static-destructor order (#275).
    sCloudTexture = NULL;
}

// virtual
void LLVOAvatar::initInstance()
{
    //-------------------------------------------------------------------------
    // register motions
    //-------------------------------------------------------------------------
    if (LLCharacter::sInstances.size() == 1)
    {
        registerMotion( ANIM_AGENT_DO_NOT_DISTURB,          LLNullMotion::create );
        registerMotion( ANIM_AGENT_CROUCH,                  LLKeyframeStandMotion::create );
        registerMotion( ANIM_AGENT_CROUCHWALK,              LLKeyframeWalkMotion::create );
        registerMotion( ANIM_AGENT_EXPRESS_AFRAID,          LLEmote::create );
        registerMotion( ANIM_AGENT_EXPRESS_ANGER,           LLEmote::create );
        registerMotion( ANIM_AGENT_EXPRESS_BORED,           LLEmote::create );
        registerMotion( ANIM_AGENT_EXPRESS_CRY,             LLEmote::create );
        registerMotion( ANIM_AGENT_EXPRESS_DISDAIN,         LLEmote::create );
        registerMotion( ANIM_AGENT_EXPRESS_EMBARRASSED,     LLEmote::create );
        registerMotion( ANIM_AGENT_EXPRESS_FROWN,           LLEmote::create );
        registerMotion( ANIM_AGENT_EXPRESS_KISS,            LLEmote::create );
        registerMotion( ANIM_AGENT_EXPRESS_LAUGH,           LLEmote::create );
        registerMotion( ANIM_AGENT_EXPRESS_OPEN_MOUTH,      LLEmote::create );
        registerMotion( ANIM_AGENT_EXPRESS_REPULSED,        LLEmote::create );
        registerMotion( ANIM_AGENT_EXPRESS_SAD,             LLEmote::create );
        registerMotion( ANIM_AGENT_EXPRESS_SHRUG,           LLEmote::create );
        registerMotion( ANIM_AGENT_EXPRESS_SMILE,           LLEmote::create );
        registerMotion( ANIM_AGENT_EXPRESS_SURPRISE,        LLEmote::create );
        registerMotion( ANIM_AGENT_EXPRESS_TONGUE_OUT,      LLEmote::create );
        registerMotion( ANIM_AGENT_EXPRESS_TOOTHSMILE,      LLEmote::create );
        registerMotion( ANIM_AGENT_EXPRESS_WINK,            LLEmote::create );
        registerMotion( ANIM_AGENT_EXPRESS_WORRY,           LLEmote::create );
        registerMotion( ANIM_AGENT_FEMALE_RUN_NEW,          LLKeyframeWalkMotion::create );
        registerMotion( ANIM_AGENT_FEMALE_WALK,             LLKeyframeWalkMotion::create );
        registerMotion( ANIM_AGENT_FEMALE_WALK_NEW,         LLKeyframeWalkMotion::create );
        registerMotion( ANIM_AGENT_RUN,                     LLKeyframeWalkMotion::create );
        registerMotion( ANIM_AGENT_RUN_NEW,                 LLKeyframeWalkMotion::create );
        registerMotion( ANIM_AGENT_STAND,                   LLKeyframeStandMotion::create );
        registerMotion( ANIM_AGENT_STAND_1,                 LLKeyframeStandMotion::create );
        registerMotion( ANIM_AGENT_STAND_2,                 LLKeyframeStandMotion::create );
        registerMotion( ANIM_AGENT_STAND_3,                 LLKeyframeStandMotion::create );
        registerMotion( ANIM_AGENT_STAND_4,                 LLKeyframeStandMotion::create );
        registerMotion( ANIM_AGENT_STANDUP,                 LLKeyframeFallMotion::create );
        registerMotion( ANIM_AGENT_TURNLEFT,                LLKeyframeWalkMotion::create );
        registerMotion( ANIM_AGENT_TURNRIGHT,               LLKeyframeWalkMotion::create );
        registerMotion( ANIM_AGENT_WALK,                    LLKeyframeWalkMotion::create );
        registerMotion( ANIM_AGENT_WALK_NEW,                LLKeyframeWalkMotion::create );

        // motions without a start/stop bit
        registerMotion( ANIM_AGENT_BODY_NOISE,              LLBodyNoiseMotion::create );
        registerMotion( ANIM_AGENT_BREATHE_ROT,             LLBreatheMotionRot::create );
        registerMotion( ANIM_AGENT_PHYSICS_MOTION,          LLPhysicsMotionController::create );
        registerMotion( ANIM_AGENT_EDITING,                 LLEditingMotion::create );
        registerMotion( ANIM_AGENT_EYE,                     LLEyeMotion::create );
        registerMotion( ANIM_AGENT_FEMALE_WALK,             LLKeyframeWalkMotion::create );
        registerMotion( ANIM_AGENT_FLY_ADJUST,              LLFlyAdjustMotion::create );
        registerMotion( ANIM_AGENT_HAND_MOTION,             LLHandMotion::create );
        registerMotion( ANIM_AGENT_HEAD_ROT,                LLHeadRotMotion::create );
        registerMotion( ANIM_AGENT_PELVIS_FIX,              LLPelvisFixMotion::create );
        registerMotion( ANIM_AGENT_SIT_FEMALE,              LLKeyframeMotion::create );
        registerMotion( ANIM_AGENT_TARGET,                  LLTargetingMotion::create );
        registerMotion( ANIM_AGENT_WALK_ADJUST,             LLWalkAdjustMotion::create );
    }

    LLAvatarAppearance::initInstance();

    // preload specific motions here
    createMotion( ANIM_AGENT_CUSTOMIZE);
    createMotion( ANIM_AGENT_CUSTOMIZE_DONE);

    //VTPause();  // VTune

    mVoiceVisualizer->setVoiceEnabled( LLVoiceClient::getInstance()->getVoiceEnabled( mID ) );

    mInitFlags |= 1<<1;
}

// virtual
LLAvatarJoint* LLVOAvatar::createAvatarJoint()
{
    return new LLViewerJoint();
}

// virtual
LLAvatarJoint* LLVOAvatar::createAvatarJoint(S32 joint_num)
{
    return new LLViewerJoint(joint_num);
}

// virtual
LLAvatarJointMesh* LLVOAvatar::createAvatarJointMesh()
{
    return new LLViewerJointMesh();
}

// virtual
LLTexLayerSet* LLVOAvatar::createTexLayerSet()
{
    return new LLViewerTexLayerSet(this);
}

const LLVector3 LLVOAvatar::getRenderPosition() const
{

    if (mDrawable.isNull() || mDrawable->getGeneration() < 0)
    {
        return getPositionAgent();
    }
    else if (isRoot())
    {
        F32 fixup;
        if ( hasPelvisFixup( fixup) )
        {
            //Apply a pelvis fixup (as defined by the avs skin)
            LLVector3 pos = mDrawable->getPositionAgent();
            pos[VZ] += fixup;
            return pos;
        }
        else
        {
            return mDrawable->getPositionAgent();
        }
    }
    else
    {
        return getPosition() * mDrawable->getParent()->getRenderMatrix();
    }
}

void LLVOAvatar::updateDrawable(bool force_damped)
{
    clearChanged(SHIFTED);
}

void LLVOAvatar::onShift(const LLVector4a& shift_vector)
{
    const LLVector3& shift = reinterpret_cast<const LLVector3&>(shift_vector);
    mLastAnimExtents[0] += shift;
    mLastAnimExtents[1] += shift;
}

void LLVOAvatar::updateSpatialExtents(LLVector4a& newMin, LLVector4a &newMax)
{
    if (mDrawable.isNull())
    {
        return;
    }

    if (mNeedsExtentUpdate)
    {
        calculateSpatialExtents(newMin,newMax);
        mLastAnimExtents[0].set(newMin.getF32ptr());
        mLastAnimExtents[1].set(newMax.getF32ptr());
        mLastAnimBasePos = mPelvisp->getWorldPosition();
        mNeedsExtentUpdate = false;
    }
    else
    {
        LLVector3 new_base_pos = mPelvisp->getWorldPosition();
        LLVector3 shift = new_base_pos-mLastAnimBasePos;
        mLastAnimExtents[0] += shift;
        mLastAnimExtents[1] += shift;
        mLastAnimBasePos = new_base_pos;

    }

    if (isImpostor() && !needsImpostorUpdate())
    {
        LLVector3 delta = getRenderPosition() -
            ((LLVector3(mDrawable->getPositionGroup().getF32ptr())-mImpostorOffset));

        newMin.load3( (mLastAnimExtents[0] + delta).mV);
        newMax.load3( (mLastAnimExtents[1] + delta).mV);
    }
    else
    {
        newMin.load3(mLastAnimExtents[0].mV);
        newMax.load3(mLastAnimExtents[1].mV);
        LLVector4a pos_group;
        pos_group.setAdd(newMin,newMax);
        pos_group.mul(0.5f);
        mImpostorOffset = LLVector3(pos_group.getF32ptr())-getRenderPosition();
        mDrawable->setPositionGroup(pos_group);
    }
}

// World box of a worn rigid attachment linkset -- the box its spatial bridge kept current
// before attachments lost their bridges (#477). An active linkset's member extents are
// root-relative, so union them and carry the result through the root's world matrix, as
// LLSpatialBridge::updateSpatialExtents did with its octree's bounds. Members without
// extents yet (never bounded) are skipped, as the bridge's octree did not hold them.
// False while the linkset has no root-relative bounds (not active yet / nothing bounded).
static bool attachment_world_extents(const LLViewerObject* root, LLVector4a out[2])
{
    LLDrawable* root_drawable = root ? root->mDrawable.get() : nullptr;
    if (!root_drawable || root_drawable->isDead() || !root_drawable->isActive())
    {
        return false;
    }

    bool any = false;
    LLVector4a local[2];
    auto add_member = [&](LLDrawable* drawable)
    {
        if (!drawable || drawable->isDead())
        {
            return;
        }
        const LLVector4a* ext = drawable->getSpatialExtents();
        if (ext[0].equals3(LLVector4a::getZero()) && ext[1].equals3(LLVector4a::getZero()))
        {
            return;
        }
        if (!any)
        {
            local[0] = ext[0];
            local[1] = ext[1];
            any = true;
        }
        else
        {
            local[0].setMin(local[0], ext[0]);
            local[1].setMax(local[1], ext[1]);
        }
    };
    add_member(root_drawable);
    for (const LLViewerObject* child : root->getChildren())
    {
        if (child && !child->isAvatar())
        {
            add_member(child->mDrawable.get());
        }
    }
    if (!any)
    {
        return false;
    }

    LLMatrix4a mat;
    mat.loadu(root_drawable->getXform()->getWorldMatrix());
    matMulBoundBox(mat, local, out);
    return true;
}

void LLVOAvatar::calculateSpatialExtents(LLVector4a& newMin, LLVector4a& newMax)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;

    // A bulk estimate over every rigged-to / polymesh joint: on a GPU-animated avatar it
    // must not pull the whole skeleton into the sparse readback -- unrequested joints
    // resolve through LL's chain (their last read-back locals; a coarse box is fine).
    struct WantSuppress
    {
        bool on;
        explicit WantSuppress(bool b) : on(b) { if (on) ++LLJoint::sGpuWantSuppress; }
        ~WantSuppress() { if (on) --LLJoint::sGpuWantSuppress; }
    } want_suppress(mGpuAnimMode == 2);
    // ... and it must not read the joints' raw cached world matrices either (#610: those
    // of unread joints are not kept current; LLJoint::getWorldMatrixLive).
    const U32 live_stamp = mGpuAnimMode == 2 ? LLJoint::nextLiveStamp() : 0;

    const S32 BOX_DETAIL_DEFAULT = 3;
    S32 box_detail = BOX_DETAIL_DEFAULT;
    if (getOverallAppearance() != AOA_NORMAL)
    {
        if (isControlAvatar())
        {
            // Animated objects don't show system avatar but do need to include rigged meshes in their bounding box.
            box_detail = 3;
        }
        else
        {
            // Jellydolled avatars ignore attachments, etc, use only system avatar.
            box_detail = 1;
        }
    }

    // FIXME the update_min_max function used below assumes there is a
    // known starting point, but in general there isn't. Ideally the
    // box update logic should be modified to handle the no-point-yet
    // case. For most models, starting with the pelvis is safe though.
    LLVector3 zero_pos;
    LLVector4a pos;
    if (dist_vec(zero_pos, mPelvisp->getWorldPosition())<0.001)
    {
        // Don't use pelvis until av initialized
        pos.load3(getRenderPosition().mV);
    }
    else
    {
        pos.load3(mPelvisp->getWorldPosition().mV);
    }
    newMin = pos;
    newMax = pos;

    if (box_detail>=1 && !isControlAvatar())
    {
        //stretch bounding box by joint positions. Doing this for
        //control avs, where the polymeshes aren't maintained or
        //displayed, can give inaccurate boxes due to joints stuck at (0,0,0).
        for (polymesh_map_t::iterator i = mPolyMeshes.begin(); i != mPolyMeshes.end(); ++i)
        {
            LLPolyMesh* mesh = i->second;
            for (S32 joint_num = 0; joint_num < mesh->mJointRenderData.size(); joint_num++)
            {
                // #610: a GPU-animated avatar skips updateWorldMatrixChildren, so the raw
                // cache of a limb joint nothing reads (collar / shoulder / elbow / hip /
                // knee) kept the pose of the last CPU-path frame -- possibly many metres
                // back -- and stretched this box from there to the avatar (a hit box over
                // a spot it left). Composed against the live root instead, once per joint
                // per call (no readback request).
                const LLJointRenderData* jrd = mesh->mJointRenderData[joint_num];
                const LLMatrix4& wm = (live_stamp && jrd->mJoint) ? jrd->mJoint->getWorldMatrixLive(live_stamp)
                                                                  : *jrd->mWorldMatrix;
                LLVector4a trans;
                trans.load3(wm.getTranslation().mV);
                update_min_max(newMin, newMax, trans);
            }
        }
    }

    // Pad bounding box for starting joint, plus polymesh if
    // applicable. Subsequent calcs should be accurate enough to not
    // need padding.
    LLVector4a padding(0.25);
    newMin.sub(padding);
    newMax.add(padding);


    //stretch bounding box by static attachments
    if (box_detail >= 2)
    {
        float max_attachment_span = get_default_max_prim_scale() * 5.0f;

        for (attachment_map_t::iterator iter = mAttachmentPoints.begin();
             iter != mAttachmentPoints.end();
             ++iter)
        {
            LLViewerJointAttachment* attachment = iter->second;

            if (attachment->getValid())
            {
                for (LLViewerJointAttachment::attachedobjs_vec_t::iterator attachment_iter = attachment->mAttachedObjects.begin();
                     attachment_iter != attachment->mAttachedObjects.end();
                     ++attachment_iter)
                {
                    // Don't we need to look at children of attached_object as well?
                    const LLViewerObject* attached_object = attachment_iter->get();
                    if (attached_object && !attached_object->isHUDAttachment())
                    {
                        const LLVOVolume *vol = dynamic_cast<const LLVOVolume*>(attached_object);
                        if (vol && vol->isAnimatedObject())
                        {
                            // Animated objects already have a bounding box in their control av, use that.
                            // Could lag by a frame if there's no guarantee on order of processing for avatars.
                            LLControlAvatar *cav = vol->getControlAvatar();
                            if (cav)
                            {
                                LLVector4a cav_min;
                                cav_min.load3(cav->mLastAnimExtents[0].mV);
                                LLVector4a cav_max;
                                cav_max.load3(cav->mLastAnimExtents[1].mV);
                                update_min_max(newMin,newMax,cav_min);
                                update_min_max(newMin,newMax,cav_max);
                                continue;
                            }
                        }
                        if (vol && vol->isRiggedMeshFast())
                        {
                            continue;
                        }
                        LLDrawable* drawable = attached_object->mDrawable;
                        if (drawable && !drawable->isState(LLDrawable::RIGGED | LLDrawable::RIGGED_CHILD)) // <-- don't extend bounding box if any rigged objects are present
                        {
                            LLVector4a ext[2];
                            if (attachment_world_extents(attached_object, ext))
                            {
                                LLVector4a distance;
                                distance.setSub(ext[1], ext[0]);
                                LLVector4a max_span(max_attachment_span);

                                S32 lt = distance.lessThan(max_span).getGatheredBits() & 0x7;

                                // Only add the prim to spatial extents calculations if it isn't a megaprim.
                                // max_attachment_span calculated at the start of the function
                                // (currently 5 times our max prim size)
                                if (lt == 0x7)
                                {
                                    update_min_max(newMin,newMax,ext[0]);
                                    update_min_max(newMin,newMax,ext[1]);
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // Stretch bounding box by rigged mesh joint boxes
    if (box_detail>=3)
    {
        updateRiggingInfo();
        // #338: the rigging test first -- getJoint(S32) is an attachment-map find for
        // the attachment-point range, and only joints something is rigged to matter.
        const S32 first_attachment_num = mNumBones + mNumCollisionVolumes;
        for (S32 joint_num = 0; joint_num < LL_CHARACTER_MAX_ANIMATED_JOINTS; joint_num++)
        {
            LLJointRiggingInfo *rig_info = NULL;
            if (joint_num < mJointRiggingInfoTab.size())
            {
                rig_info = &mJointRiggingInfoTab[joint_num];
            }
            if (!rig_info || !rig_info->isRiggedTo())
            {
                continue;
            }
            LLJoint *joint = getJoint(joint_num);

            if (joint)
            {
                // only attachment points (numbered after the bones and collision
                // volumes) can be an LLViewerJointAttachment
                LLViewerJointAttachment *as_joint_attach = (joint_num >= first_attachment_num)
                    ? dynamic_cast<LLViewerJointAttachment*>(joint) : NULL;
                if (as_joint_attach && as_joint_attach->getIsHUDAttachment())
                {
                    // Ignore bounding box of HUD joints
                    continue;
                }
                LLMatrix4a mat;
                LLVector4a new_extents[2];
                mat.loadu(joint->getWorldMatrix());
                matMulBoundBox(mat, rig_info->getRiggedExtents(), new_extents);
                update_min_max(newMin, newMax, new_extents[0]);
                update_min_max(newMin, newMax, new_extents[1]);
                //if (isSelf())
                //{
                //    LL_INFOS() << joint->getName() << " extents " << new_extents[0] << "," << new_extents[1] << LL_ENDL;
                //    LL_INFOS() << joint->getName() << " av box is " << newMin << "," << newMax << LL_ENDL;
                //}
            }
        }
    }

    // Update pixel area
    LLVector4a center, size;
    center.setAdd(newMin, newMax);
    center.mul(0.5f);

    size.setSub(newMax,newMin);
    size.mul(0.5f);

    F32 pixel_area = LLPipeline::calcPixelArea(center, size, *LLViewerCamera::getInstance());
    setCorrectedPixelArea(pixel_area);
}

void render_sphere_and_line(const LLVector3& begin_pos, const LLVector3& end_pos, F32 sphere_scale, const LLVector3& occ_color, const LLVector3& visible_color)
{
    // Unoccluded bone portions
    LLRender::DepthScope normal_depth(true);

    // Draw line segment for unoccluded joint

    gGL.begin(LLRender::LINES);
    gGL.vertex3fv(begin_pos.mV);
    gGL.vertex3fv(end_pos.mV);
    gGL.end();


    // Draw sphere representing joint pos
    gGL.pushMatrix();
    gGL.scalef(sphere_scale, sphere_scale, sphere_scale);
    render_unit_sphere(false);
    gGL.popMatrix();

    LLRender::DepthScope depth_under(true, false, LLDepthFunc::GREATER);

    // Occluded bone portions

    gGL.begin(LLRender::LINES);
    gGL.vertex3fv(begin_pos.mV);
    gGL.vertex3fv(end_pos.mV);
    gGL.end();

    // Draw sphere representing joint pos
    gGL.pushMatrix();
    gGL.scalef(sphere_scale, sphere_scale, sphere_scale);
    render_unit_sphere(false);
    gGL.popMatrix();
}

//-----------------------------------------------------------------------------
// renderCollisionVolumes()
//-----------------------------------------------------------------------------
void LLVOAvatar::renderCollisionVolumes()
{
    std::ostringstream ostr;

    for (S32 i = 0; i < mNumCollisionVolumes; i++)
    {
        ostr << mCollisionVolumes[i].getName() << ", ";

        LLAvatarJointCollisionVolume& collision_volume = mCollisionVolumes[i];

        collision_volume.updateWorldMatrix();

        gGL.pushMatrix();
        gGL.multMatrix( &collision_volume.getXform()->getWorldMatrix().mMatrix[0][0] );

        LLVector3 begin_pos(0,0,0);
        LLVector3 end_pos(collision_volume.getEnd());
        static F32 sphere_scale = 1.0f;
        static F32 center_dot_scale = 0.05f;

        static LLVector3 BLUE(0.0f, 0.0f, 1.0f);
        static LLVector3 PASTEL_BLUE(0.5f, 0.5f, 1.0f);
        static LLVector3 RED(1.0f, 0.0f, 0.0f);
        static LLVector3 PASTEL_RED(1.0f, 0.5f, 0.5f);
        static LLVector3 WHITE(1.0f, 1.0f, 1.0f);


        LLVector3 cv_color_occluded;
        LLVector3 cv_color_visible;
        LLVector3 dot_color_occluded(WHITE);
        LLVector3 dot_color_visible(WHITE);
        if (isControlAvatar())
        {
            cv_color_occluded = RED;
            cv_color_visible = PASTEL_RED;
        }
        else
        {
            cv_color_occluded = BLUE;
            cv_color_visible = PASTEL_BLUE;
        }
        render_sphere_and_line(begin_pos, end_pos, sphere_scale, cv_color_occluded, cv_color_visible);
        render_sphere_and_line(begin_pos, end_pos, center_dot_scale, dot_color_occluded, dot_color_visible);

        gGL.popMatrix();
    }


    if (mNameText.notNull())
    {
        LLVector4a unused;

        mNameText->lineSegmentIntersect(unused, unused, unused, true);
    }
}

// Draw a box outline (LLRender immediate mode) from given center and half-size (bone
// debug display; moved here from the deleted llspatialpartition.cpp, #480).
static void drawBoxOutline(const LLVector3& pos, const LLVector3& size)
{
    if (!pos.isFinite() || !size.isFinite())
        return;

    LLVector3 v1 = size.scaledVec(LLVector3( 1, 1,1));
    LLVector3 v2 = size.scaledVec(LLVector3(-1, 1,1));
    LLVector3 v3 = size.scaledVec(LLVector3(-1,-1,1));
    LLVector3 v4 = size.scaledVec(LLVector3( 1,-1,1));

    gGL.begin(LLRender::LINES);

    //top
    gGL.vertex3fv((pos+v1).mV);
    gGL.vertex3fv((pos+v2).mV);
    gGL.vertex3fv((pos+v2).mV);
    gGL.vertex3fv((pos+v3).mV);
    gGL.vertex3fv((pos+v3).mV);
    gGL.vertex3fv((pos+v4).mV);
    gGL.vertex3fv((pos+v4).mV);
    gGL.vertex3fv((pos+v1).mV);

    //bottom
    gGL.vertex3fv((pos-v1).mV);
    gGL.vertex3fv((pos-v2).mV);
    gGL.vertex3fv((pos-v2).mV);
    gGL.vertex3fv((pos-v3).mV);
    gGL.vertex3fv((pos-v3).mV);
    gGL.vertex3fv((pos-v4).mV);
    gGL.vertex3fv((pos-v4).mV);
    gGL.vertex3fv((pos-v1).mV);

    //right
    gGL.vertex3fv((pos+v1).mV);
    gGL.vertex3fv((pos-v3).mV);

    gGL.vertex3fv((pos+v4).mV);
    gGL.vertex3fv((pos-v2).mV);

    //left
    gGL.vertex3fv((pos+v2).mV);
    gGL.vertex3fv((pos-v4).mV);

    gGL.vertex3fv((pos+v3).mV);
    gGL.vertex3fv((pos-v1).mV);

    gGL.end();
}

static void drawBoxOutline(const LLVector4a& pos, const LLVector4a& size)
{
    drawBoxOutline(reinterpret_cast<const LLVector3&>(pos), reinterpret_cast<const LLVector3&>(size));
}

void LLVOAvatar::renderBones(const std::string &selected_joint)
{
    LLRender::BlendScope blend;

    avatar_joint_list_t::iterator iter = mSkeleton.begin();
    avatar_joint_list_t::iterator end = mSkeleton.end();

    // For selected joints
    static LLVector3 SELECTED_COLOR_OCCLUDED(1.0f, 1.0f, 0.0f);
    static LLVector3 SELECTED_COLOR_VISIBLE(0.5f, 0.5f, 0.5f);
    // For bones with position overrides defined
    static LLVector3 OVERRIDE_COLOR_OCCLUDED(1.0f, 0.0f, 0.0f);
    static LLVector3 OVERRIDE_COLOR_VISIBLE(0.5f, 0.5f, 0.5f);
    // For bones which are rigged to by at least one attachment
    static LLVector3 RIGGED_COLOR_OCCLUDED(0.0f, 1.0f, 1.0f);
    static LLVector3 RIGGED_COLOR_VISIBLE(0.5f, 0.5f, 0.5f);
    // For bones not otherwise colored
    static LLVector3 OTHER_COLOR_OCCLUDED(0.0f, 1.0f, 0.0f);
    static LLVector3 OTHER_COLOR_VISIBLE(0.5f, 0.5f, 0.5f);

    static F32 SPHERE_SCALEF = 0.001f;

    for (; iter != end; ++iter)
    {
        LLJoint* jointp = *iter;
        if (!jointp)
        {
            continue;
        }

        jointp->updateWorldMatrix();

        LLVector3 occ_color, visible_color;

        LLVector3 pos;
        LLUUID mesh_id;
        F32 sphere_scale = SPHERE_SCALEF;

        // We are in render, so it is preferable to implement selection
        // in a different way, but since this is for debug/preview, this
        // is low priority
        if (jointp->getName() == selected_joint)
        {
            sphere_scale *= 16;
            occ_color = SELECTED_COLOR_OCCLUDED;
            visible_color = SELECTED_COLOR_VISIBLE;
        }
        else if (jointp->hasAttachmentPosOverride(pos,mesh_id))
        {
            occ_color = OVERRIDE_COLOR_OCCLUDED;
            visible_color = OVERRIDE_COLOR_VISIBLE;
        }
        else
        {
            if (jointIsRiggedTo(jointp))
            {
                occ_color = RIGGED_COLOR_OCCLUDED;
                visible_color = RIGGED_COLOR_VISIBLE;
            }
            else
            {
                occ_color = OTHER_COLOR_OCCLUDED;
                visible_color = OTHER_COLOR_VISIBLE;
            }
        }
        LLVector3 begin_pos(0,0,0);
        LLVector3 end_pos(jointp->getEnd());


        gGL.pushMatrix();
        gGL.multMatrix( &jointp->getXform()->getWorldMatrix().mMatrix[0][0] );

        render_sphere_and_line(begin_pos, end_pos, sphere_scale, occ_color, visible_color);

        gGL.popMatrix();
    }


    // draw joint space bounding boxes of rigged attachments in yellow
    gGL.color3f(1.f, 1.f, 0.f);
    for (S32 joint_num = 0; joint_num < LL_CHARACTER_MAX_ANIMATED_JOINTS; joint_num++)
    {
        LLJoint* joint = getJoint(joint_num);
        LLJointRiggingInfo* rig_info = NULL;
        if (joint_num < mJointRiggingInfoTab.size())
        {
            rig_info = &mJointRiggingInfoTab[joint_num];
        }

        if (joint && rig_info && rig_info->isRiggedTo())
        {
            LLViewerJointAttachment* as_joint_attach = dynamic_cast<LLViewerJointAttachment*>(joint);
            if (as_joint_attach && as_joint_attach->getIsHUDAttachment())
            {
                // Ignore bounding box of HUD joints
                continue;
            }
            gGL.pushMatrix();
            gGL.multMatrix(&joint->getXform()->getWorldMatrix().mMatrix[0][0]);

            LLVector4a pos;
            LLVector4a size;

            const LLVector4a* extents = rig_info->getRiggedExtents();

            pos.setAdd(extents[0], extents[1]);
            pos.mul(0.5f);
            size.setSub(extents[1], extents[0]);
            size.mul(0.5f);

            drawBoxOutline(pos, size);

            gGL.popMatrix();
        }
    }

    // draw world space attachment rigged bounding boxes in cyan
    gGL.color3f(0.f, 1.f, 1.f);
    for (attachment_map_t::iterator iter = mAttachmentPoints.begin();
         iter != mAttachmentPoints.end();
         ++iter)
    {
        LLViewerJointAttachment* attachment = iter->second;

        if (attachment->getValid())
        {
            for (LLViewerJointAttachment::attachedobjs_vec_t::iterator attachment_iter = attachment->mAttachedObjects.begin();
                 attachment_iter != attachment->mAttachedObjects.end();
                 ++attachment_iter)
            {
                LLViewerObject* attached_object = attachment_iter->get();
                if (attached_object && !attached_object->isHUDAttachment())
                {
                    LLDrawable* drawable = attached_object->mDrawable;
                    if (drawable && drawable->isState(LLDrawable::RIGGED | LLDrawable::RIGGED_CHILD))
                    {
                        // get face rigged extents
                        for (S32 i = 0; i < drawable->getNumFaces(); ++i)
                        {
                            LLFace* facep = drawable->getFace(i);
                            if (facep && facep->isState(LLFace::RIGGED))
                            {
                                LLVector4a center, size;

                                LLVector4a* extents = facep->mRiggedExtents;

                                center.setAdd(extents[0], extents[1]);
                                center.mul(0.5f);
                                size.setSub(extents[1], extents[0]);
                                size.mul(0.5f);
                                drawBoxOutline(center, size);
                            }
                        }
                    }
                }
            }
        }
    }
}


void LLVOAvatar::renderJoints()
{
    std::ostringstream ostr;
    std::ostringstream nullstr;

    for (joint_map_t::iterator iter = mJointMap.begin(); iter != mJointMap.end(); ++iter)
    {
        LLJoint* jointp = iter->second;
        if (!jointp)
        {
            nullstr << iter->first << " is NULL" << std::endl;
            continue;
        }

        ostr << jointp->getName() << ", ";

        jointp->updateWorldMatrix();

        gGL.pushMatrix();
        gGL.multMatrix( &jointp->getXform()->getWorldMatrix().mMatrix[0][0] );


        gGL.begin(LLRender::LINES);

        LLVector3 v[] =
        {
            LLVector3(1,0,0),
            LLVector3(-1,0,0),
            LLVector3(0,1,0),
            LLVector3(0,-1,0),

            LLVector3(0,0,-1),
            LLVector3(0,0,1),
        };

        //sides
        gGL.vertex3fv(v[0].mV);
        gGL.vertex3fv(v[2].mV);

        gGL.vertex3fv(v[0].mV);
        gGL.vertex3fv(v[3].mV);

        gGL.vertex3fv(v[1].mV);
        gGL.vertex3fv(v[2].mV);

        gGL.vertex3fv(v[1].mV);
        gGL.vertex3fv(v[3].mV);


        //top
        gGL.vertex3fv(v[0].mV);
        gGL.vertex3fv(v[4].mV);

        gGL.vertex3fv(v[1].mV);
        gGL.vertex3fv(v[4].mV);

        gGL.vertex3fv(v[2].mV);
        gGL.vertex3fv(v[4].mV);

        gGL.vertex3fv(v[3].mV);
        gGL.vertex3fv(v[4].mV);


        //bottom
        gGL.vertex3fv(v[0].mV);
        gGL.vertex3fv(v[5].mV);

        gGL.vertex3fv(v[1].mV);
        gGL.vertex3fv(v[5].mV);

        gGL.vertex3fv(v[2].mV);
        gGL.vertex3fv(v[5].mV);

        gGL.vertex3fv(v[3].mV);
        gGL.vertex3fv(v[5].mV);

        gGL.end();

        gGL.popMatrix();
    }

    mDebugText.clear();
    addDebugText(ostr.str());
    addDebugText(nullstr.str());
}

bool LLVOAvatar::lineSegmentIntersect(const LLVector4a& start, const LLVector4a& end,
                                      S32 face,
                                      bool pick_transparent,
                                      bool pick_rigged,
                                      bool pick_unselectable,
                                      S32* face_hit,
                                      LLVector4a* intersection,
                                      LLVector2* tex_coord,
                                      LLVector4a* normal,
                                      LLVector4a* tangent)
{
    if ((isSelf() && !gAgent.needsRenderAvatar()) || !LLPipeline::sPickAvatar)
    {
        return false;
    }

    if (isControlAvatar())
    {
        return false;
    }

    if (lineSegmentBoundingBox(start, end))
    {
        // #610: LL's updateWorldMatrix composes a volume from its PARENT's cached
        // transform, trusting the per-frame updateWorldMatrixChildren -- which a Vulkan
        // GPU-animated avatar skips (only the base joints are refreshed:
        // LLCleanAnimStore::refreshWorld), and its dirty flags don't follow root motion.
        // The limb volumes' parents (collars, shoulders, elbows, hips, knees) kept the
        // pose of the avatar's last CPU-path frame, so its arms and legs stayed pickable
        // where it had stood -- hover tips, clicks and camera focus on an empty spot,
        // until it left draw range. Compose them against the live root instead
        // (getWorldMatrixLive), and request them in the GPU readback so the next picks
        // get the exact pose.
        const bool gpu_live = mGpuAnimMode == 2;
        const U32 live_stamp = gpu_live ? LLJoint::nextLiveStamp() : 0;
        for (S32 i = 0; i < mNumCollisionVolumes; ++i)
        {
            const LLMatrix4* wm;
            if (gpu_live)
            {
                mCollisionVolumes[i].gpuWant();
                wm = &mCollisionVolumes[i].getWorldMatrixLive(live_stamp);
            }
            else
            {
                mCollisionVolumes[i].updateWorldMatrix();
                wm = &mCollisionVolumes[i].getXform()->getWorldMatrix();
            }

            glm::mat4 mat(glm::make_mat4((F32*) wm->mMatrix));
            glm::mat4 inverse = glm::inverse(mat);
            glm::mat4 norm_mat = glm::transpose(inverse);

            glm::vec3 p1(start);
            glm::vec3 p2(end);

            p1 = mul_mat4_vec3(inverse, p1);
            p2 = mul_mat4_vec3(inverse, p2);

            LLVector3 position;
            LLVector3 norm;

            if (linesegment_sphere(LLVector3(p1), LLVector3(p2), LLVector3(0,0,0), 1.f, position, norm))
            {
                glm::vec3 res_pos(position);
                res_pos = mul_mat4_vec3(mat, res_pos);

                 glm::vec3 res_norm(norm);
                res_norm = glm::normalize(res_norm);
                res_norm = glm::mat3(norm_mat) * res_norm;

                if (intersection)
                {
                    intersection->load3(glm::value_ptr(res_pos));
                }

                if (normal)
                {
                    normal->load3(glm::value_ptr(res_norm));
                }

                return true;
            }
        }
        // (LL regenerated its rigged attachments' CPU pick octrees here; rigged mesh is
        // picked on the GPU as posed, #704.)
    }



    LLVector4a position;
    if (mNameText.notNull() && mNameText->lineSegmentIntersect(start, end, position))
    {
        if (intersection)
        {
            *intersection = position;
        }

        return true;
    }

    return false;
}

// virtual
LLVOAvatar* LLVOAvatar::asAvatar()
{
    return this;
}

//-----------------------------------------------------------------------------
// LLVOAvatar::startDefaultMotions()
//-----------------------------------------------------------------------------
void LLVOAvatar::startDefaultMotions()
{
    //-------------------------------------------------------------------------
    // start default motions
    //-------------------------------------------------------------------------
    startMotion( ANIM_AGENT_HEAD_ROT );
    startMotion( ANIM_AGENT_EYE );
    startMotion( ANIM_AGENT_BODY_NOISE );
    startMotion( ANIM_AGENT_BREATHE_ROT );
    startMotion( ANIM_AGENT_PHYSICS_MOTION );
    startMotion( ANIM_AGENT_HAND_MOTION );
    startMotion( ANIM_AGENT_PELVIS_FIX );

    //-------------------------------------------------------------------------
    // restart any currently active motions
    //-------------------------------------------------------------------------
    processAnimationStateChanges();
}

//-----------------------------------------------------------------------------
// LLVOAvatar::buildCharacter()
// Deferred initialization and rebuild of the avatar.
//-----------------------------------------------------------------------------
// virtual
void LLVOAvatar::buildCharacter()
{
    LLAvatarAppearance::buildCharacter();

    // Not done building yet; more to do.
    mIsBuilt = false;

    //-------------------------------------------------------------------------
    // set head offset from pelvis
    //-------------------------------------------------------------------------
    updateHeadOffset();

    //-------------------------------------------------------------------------
    // initialize lip sync morph pointers
    //-------------------------------------------------------------------------
    mOohMorph     = getVisualParam( "Lipsync_Ooh" );
    mAahMorph     = getVisualParam( "Lipsync_Aah" );

    // If we don't have the Ooh morph, use the Kiss morph
    if (!mOohMorph)
    {
        LL_WARNS() << "Missing 'Ooh' morph for lipsync, using fallback." << LL_ENDL;
        mOohMorph = getVisualParam( "Express_Kiss" );
    }

    // If we don't have the Aah morph, use the Open Mouth morph
    if (!mAahMorph)
    {
        LL_WARNS() << "Missing 'Aah' morph for lipsync, using fallback." << LL_ENDL;
        mAahMorph = getVisualParam( "Express_Open_Mouth" );
    }

    // Currently disabled for control avatars (animated objects), enabled for all others.
    if (mEnableDefaultMotions)
    {
    startDefaultMotions();
    }

    //-------------------------------------------------------------------------
    // restart any currently active motions
    //-------------------------------------------------------------------------
    processAnimationStateChanges();

    mIsBuilt = true;

    mMeshValid = true;
}

//-----------------------------------------------------------------------------
// resetVisualParams()
//-----------------------------------------------------------------------------
void LLVOAvatar::resetVisualParams()
{
    // Skeletal params
    {
        LLAvatarXmlInfo::skeletal_distortion_info_list_t::iterator iter;
        for (iter = sAvatarXmlInfo->mSkeletalDistortionInfoList.begin();
             iter != sAvatarXmlInfo->mSkeletalDistortionInfoList.end();
             ++iter)
        {
            LLPolySkeletalDistortionInfo *info = (LLPolySkeletalDistortionInfo*)*iter;
            LLPolySkeletalDistortion *param = dynamic_cast<LLPolySkeletalDistortion*>(getVisualParam(info->getID()));
            *param = LLPolySkeletalDistortion(this);
            llassert(param);
            if (!param->setInfo(info))
            {
                llassert(false);
            }
        }
    }

    // Driver parameters
    for (LLAvatarXmlInfo::driver_info_list_t::iterator iter = sAvatarXmlInfo->mDriverInfoList.begin();
         iter != sAvatarXmlInfo->mDriverInfoList.end();
         ++iter)
    {
        LLDriverParamInfo *info = *iter;
        LLDriverParam *param = dynamic_cast<LLDriverParam*>(getVisualParam(info->getID()));
        LLDriverParam::entry_list_t driven_list = param->getDrivenList();
        *param = LLDriverParam(this);
        llassert(param);
        if (!param->setInfo(info))
        {
            llassert(false);
        }
        param->setDrivenList(driven_list);
    }
}

void LLVOAvatar::applyDefaultParams()
{
    // These are params from avs with newly created copies of shape,
    // skin, hair, eyes, plus gender set as noted. Run arche_tool.py
    // to get params from some other xml appearance dump.
    std::map<S32, U8> male_params = {
        {1,33}, {2,61}, {4,85}, {5,23}, {6,58}, {7,127}, {8,63}, {10,85}, {11,63}, {12,42}, {13,0}, {14,85}, {15,63}, {16,36}, {17,85}, {18,95}, {19,153}, {20,63}, {21,34}, {22,0}, {23,63}, {24,109}, {25,88}, {27,132}, {31,63}, {33,136}, {34,81}, {35,85}, {36,103}, {37,136}, {38,127}, {80,255}, {93,203}, {98,0}, {99,0}, {105,127}, {108,0}, {110,0}, {111,127}, {112,0}, {113,0}, {114,127}, {115,0}, {116,0}, {117,0}, {119,127}, {130,114}, {131,127}, {132,99}, {133,63}, {134,127}, {135,140}, {136,127}, {137,127}, {140,0}, {141,0}, {142,0}, {143,191}, {150,0}, {155,104}, {157,0}, {162,0}, {163,0}, {165,0}, {166,0}, {167,0}, {168,0}, {169,0}, {177,0}, {181,145}, {182,216}, {183,133}, {184,0}, {185,127}, {192,0}, {193,127}, {196,170}, {198,0}, {503,0}, {505,127}, {506,127}, {507,109}, {508,85}, {513,127}, {514,127}, {515,63}, {517,85}, {518,42}, {603,100}, {604,216}, {605,214}, {606,204}, {607,204}, {608,204}, {609,51}, {616,25}, {617,89}, {619,76}, {624,204}, {625,0}, {629,127}, {637,0}, {638,0}, {646,144}, {647,85}, {649,127}, {650,132}, {652,127}, {653,85}, {654,0}, {656,127}, {659,127}, {662,127}, {663,127}, {664,127}, {665,127}, {674,59}, {675,127}, {676,85}, {678,127}, {682,127}, {683,106}, {684,47}, {685,79}, {690,127}, {692,127}, {693,204}, {700,63}, {701,0}, {702,0}, {703,0}, {704,0}, {705,127}, {706,127}, {707,0}, {708,0}, {709,0}, {710,0}, {711,127}, {712,0}, {713,159}, {714,0}, {715,0}, {750,178}, {752,127}, {753,36}, {754,85}, {755,131}, {756,127}, {757,127}, {758,127}, {759,153}, {760,95}, {762,0}, {763,140}, {764,74}, {765,27}, {769,127}, {773,127}, {775,0}, {779,214}, {780,204}, {781,198}, {785,0}, {789,0}, {795,63}, {796,30}, {799,127}, {800,226}, {801,255}, {802,198}, {803,255}, {804,255}, {805,255}, {806,255}, {807,255}, {808,255}, {812,255}, {813,255}, {814,255}, {815,204}, {816,0}, {817,255}, {818,255}, {819,255}, {820,255}, {821,255}, {822,255}, {823,255}, {824,255}, {825,255}, {826,255}, {827,255}, {828,0}, {829,255}, {830,255}, {834,255}, {835,255}, {836,255}, {840,0}, {841,127}, {842,127}, {844,255}, {848,25}, {858,100}, {859,255}, {860,255}, {861,255}, {862,255}, {863,84}, {868,0}, {869,0}, {877,0}, {879,51}, {880,132}, {921,255}, {922,255}, {923,255}, {10000,0}, {10001,0}, {10002,25}, {10003,0}, {10004,25}, {10005,23}, {10006,51}, {10007,0}, {10008,25}, {10009,23}, {10010,51}, {10011,0}, {10012,0}, {10013,25}, {10014,0}, {10015,25}, {10016,23}, {10017,51}, {10018,0}, {10019,0}, {10020,25}, {10021,0}, {10022,25}, {10023,23}, {10024,51}, {10025,0}, {10026,25}, {10027,23}, {10028,51}, {10029,0}, {10030,25}, {10031,23}, {10032,51}, {11000,1}, {11001,127}
    };
    std::map<S32, U8> female_params = {
        {1,33}, {2,61}, {4,85}, {5,23}, {6,58}, {7,127}, {8,63}, {10,85}, {11,63}, {12,42}, {13,0}, {14,85}, {15,63}, {16,36}, {17,85}, {18,95}, {19,153}, {20,63}, {21,34}, {22,0}, {23,63}, {24,109}, {25,88}, {27,132}, {31,63}, {33,136}, {34,81}, {35,85}, {36,103}, {37,136}, {38,127}, {80,0}, {93,203}, {98,0}, {99,0}, {105,127}, {108,0}, {110,0}, {111,127}, {112,0}, {113,0}, {114,127}, {115,0}, {116,0}, {117,0}, {119,127}, {130,114}, {131,127}, {132,99}, {133,63}, {134,127}, {135,140}, {136,127}, {137,127}, {140,0}, {141,0}, {142,0}, {143,191}, {150,0}, {155,104}, {157,0}, {162,0}, {163,0}, {165,0}, {166,0}, {167,0}, {168,0}, {169,0}, {177,0}, {181,145}, {182,216}, {183,133}, {184,0}, {185,127}, {192,0}, {193,127}, {196,170}, {198,0}, {503,0}, {505,127}, {506,127}, {507,109}, {508,85}, {513,127}, {514,127}, {515,63}, {517,85}, {518,42}, {603,100}, {604,216}, {605,214}, {606,204}, {607,204}, {608,204}, {609,51}, {616,25}, {617,89}, {619,76}, {624,204}, {625,0}, {629,127}, {637,0}, {638,0}, {646,144}, {647,85}, {649,127}, {650,132}, {652,127}, {653,85}, {654,0}, {656,127}, {659,127}, {662,127}, {663,127}, {664,127}, {665,127}, {674,59}, {675,127}, {676,85}, {678,127}, {682,127}, {683,106}, {684,47}, {685,79}, {690,127}, {692,127}, {693,204}, {700,63}, {701,0}, {702,0}, {703,0}, {704,0}, {705,127}, {706,127}, {707,0}, {708,0}, {709,0}, {710,0}, {711,127}, {712,0}, {713,159}, {714,0}, {715,0}, {750,178}, {752,127}, {753,36}, {754,85}, {755,131}, {756,127}, {757,127}, {758,127}, {759,153}, {760,95}, {762,0}, {763,140}, {764,74}, {765,27}, {769,127}, {773,127}, {775,0}, {779,214}, {780,204}, {781,198}, {785,0}, {789,0}, {795,63}, {796,30}, {799,127}, {800,226}, {801,255}, {802,198}, {803,255}, {804,255}, {805,255}, {806,255}, {807,255}, {808,255}, {812,255}, {813,255}, {814,255}, {815,204}, {816,0}, {817,255}, {818,255}, {819,255}, {820,255}, {821,255}, {822,255}, {823,255}, {824,255}, {825,255}, {826,255}, {827,255}, {828,0}, {829,255}, {830,255}, {834,255}, {835,255}, {836,255}, {840,0}, {841,127}, {842,127}, {844,255}, {848,25}, {858,100}, {859,255}, {860,255}, {861,255}, {862,255}, {863,84}, {868,0}, {869,0}, {877,0}, {879,51}, {880,132}, {921,255}, {922,255}, {923,255}, {10000,0}, {10001,0}, {10002,25}, {10003,0}, {10004,25}, {10005,23}, {10006,51}, {10007,0}, {10008,25}, {10009,23}, {10010,51}, {10011,0}, {10012,0}, {10013,25}, {10014,0}, {10015,25}, {10016,23}, {10017,51}, {10018,0}, {10019,0}, {10020,25}, {10021,0}, {10022,25}, {10023,23}, {10024,51}, {10025,0}, {10026,25}, {10027,23}, {10028,51}, {10029,0}, {10030,25}, {10031,23}, {10032,51}, {11000,1}, {11001,127}
    };
    std::map<S32, U8> *params = NULL;
    if (getSex() == SEX_MALE)
        params = &male_params;
    else
        params = &female_params;

    for( auto it = params->begin(); it != params->end(); ++it)
    {
        LLVisualParam* param = getVisualParam(it->first);
        if( !param )
        {
            // invalid id
            break;
        }

        U8 value = it->second;
        F32 newWeight = U8_to_F32(value, param->getMinWeight(), param->getMaxWeight());
        param->setWeight(newWeight);
    }
}

//-----------------------------------------------------------------------------
// resetSkeleton()
//-----------------------------------------------------------------------------
// #207: after an animation evaluation, hand collision volumes an animation stopped
// driving back to their pre-animation position (+ morph deltas held meanwhile).
void LLVOAvatar::endMorphEvaluation()
{
    for (S32 i = 0; i < mNumCollisionVolumes; ++i) mCollisionVolumes[i].endPosEvaluation();

    // RenderVKPhysicsDiag (#207): where the physics bones sit and why. For self, or the
    // avatar named in RenderVKDumpAvatar; every 2 s.
    static LLCachedControl<bool> s_diag(gSavedSettings, "RenderVKPhysicsDiag", false);
    if (!s_diag) return;
    static LLCachedControl<std::string> s_who(gSavedSettings, "RenderVKDumpAvatar", "");
    const std::string who = s_who;
    bool match = isSelf() && who.empty();
    if (!match && !who.empty())
    {
        LLAvatarName an;
        if (getID().asString() == who) match = true;
        else if (LLAvatarNameCache::get(getID(), &an))
            match = LLStringUtil::compareInsensitive(an.getUserName(), who) == 0
                 || LLStringUtil::compareInsensitive(an.getDisplayName(), who) == 0;
    }
    if (!match || mPhysicsDiagTimer.getElapsedTimeF32() < 2.f) return;
    mPhysicsDiagTimer.reset();
    static const char* names[] = {"LEFT_PEC", "RIGHT_PEC", "BELLY", "BUTT"};
    for (const char* nm : names)
    {
        LLJoint* cv = getJoint(nm);
        if (!cv) continue;
        // Sum of every volume morph's contribution on this joint: (applied weight -
        // default) * offset -- what rest + morphs predicts, before overrides.
        LLVector3 morph_sum;
        std::string physics;
        for (LLVisualParam* p = getFirstVisualParam(); p; p = getNextVisualParam())
        {
            LLPolyMorphTarget* pm = dynamic_cast<LLPolyMorphTarget*>(p);
            if (!pm) continue;
            for (const LLPolyVolumeMorph& vm : pm->getVolumeMorphs())
            {
                if (vm.mVolume != cv) continue;
                const F32 w = pm->getLastWeight() - pm->getDefaultWeight();
                morph_sum += vm.mPos * w;
                if (p->getName().find("Physics") != std::string::npos && fabsf(w) > 0.0001f)
                    physics += llformat(" %s=%.3f", p->getName().c_str(), w);
            }
        }
        LLUUID mesh; LLVector3 ov;
        const bool has_ov = cv->m_attachmentPosOverrides.findActiveOverride(mesh, ov);
        LL_INFOS("VKPhysicsDiag") << getFullname() << " " << nm << " pos " << cv->getPosition()
            << " default " << cv->getDefaultPosition() << " default+morphs " << (cv->getDefaultPosition() + morph_sum)
            << (has_ov ? llformat(" override (%.3f,%.3f,%.3f)", ov.mV[0], ov.mV[1], ov.mV[2]) : std::string(" no-override"))
            << " before-overrides " << cv->m_posBeforeOverrides
            << " animated " << (cv->mPosAnimatedPrev ? 1 : 0) << " held " << cv->mAnimMorphHeld
            << (isSelf() ? " self(cpu anim)" : " other") << physics << LL_ENDL;
    }
}

void LLVOAvatar::resetSkeleton(bool reset_animations)
{
    VK_HITCH_SCOPE("av_resetSkeleton");   // #521: an un-mute burst (e.g. a preset raising the complexity cap)
    LL_DEBUGS("Avatar") << avString() << " reset starts" << LL_ENDL;
    if (!isControlAvatar() && !mLastProcessedAppearance)
    {
        LL_WARNS() << "Can't reset avatar " << getID() << "; no appearance message has been received yet." << LL_ENDL;
        return;
    }

    // Save mPelvis state
    //LLVector3 pelvis_pos = getJoint("mPelvis")->getPosition();
    //LLQuaternion pelvis_rot = getJoint("mPelvis")->getRotation();

    // Clear all attachment pos and scale overrides
    clearAttachmentOverrides();

    // Note that we call buildSkeleton twice in this function. The first time is
    // just to get the right scale for the collision volumes, because
    // this will be used in setting the mJointScales for the
    // LLPolySkeletalDistortions of which the CVs are children.
    if( !buildSkeleton(sAvatarSkeletonInfo) )
    {
        LL_ERRS() << "Error resetting skeleton" << LL_ENDL;
    }

    // Reset some params to default state, without propagating changes downstream.
    resetVisualParams();

    // Now we have to reset the skeleton again, because its state
    // got clobbered by the resetVisualParams() calls
    // above.
    if( !buildSkeleton(sAvatarSkeletonInfo) )
    {
        LL_ERRS() << "Error resetting skeleton" << LL_ENDL;
    }

    // Reset attachment points
    // BuildSkeleton only does bones and CVs but we still need to reinit huds
    // since huds can be animated.
    bool ignore_hud_joints = !isSelf();
    initAttachmentPoints(ignore_hud_joints);

    // Fix up collision volumes (a reset during an animation starts the hand-back over, #207)
    for (S32 i = 0; i < mNumCollisionVolumes; ++i)
    {
        mCollisionVolumes[i].mPosAnimated = mCollisionVolumes[i].mPosAnimatedPrev = false;
        mCollisionVolumes[i].mAnimMorphHeld.clearVec();
    }
    for (LLVisualParam *param = getFirstVisualParam();
         param;
         param = getNextVisualParam())
    {
        LLPolyMorphTarget *poly_morph = dynamic_cast<LLPolyMorphTarget*>(param);
        if (poly_morph)
        {
            // This is a kludgy way to correct for the fact that the
            // collision volumes have been reset out from under the
            // poly morph sliders.
            F32 delta_weight = poly_morph->getLastWeight() - poly_morph->getDefaultWeight();
            poly_morph->applyVolumeChanges(delta_weight);
        }
    }

    // Reset tweakable params to preserved state
    if (getOverallAppearance() == AOA_NORMAL)
    {
        if (mLastProcessedAppearance)
        {
            bool slam_params = true;
            applyParsedAppearanceMessage(*mLastProcessedAppearance, slam_params);
        }
    }
    else
    {
        // Stripped down approximation of
        // applyParsedAppearanceMessage, but with alternative default
        // (jellydoll) params
        setCompositeUpdatesEnabled( false );
        gPipeline.markGLRebuild(this);
        applyDefaultParams();
        setCompositeUpdatesEnabled( true );
        updateMeshTextures();
        updateMeshVisibility();
    }
    updateVisualParams();

    // Restore attachment pos overrides
    updateAttachmentOverrides();

    // Animations
    if (reset_animations)
    {
        if (isSelf())
        {
            // This is equivalent to "Stop Animating Me". Will reset
            // all animations and propagate the changes to other
            // viewers.
            gAgent.stopCurrentAnimations();
        }
        else
        {
            // Local viewer-side reset for non-self avatars.
            resetAnimations();
        }
    }

    // #631: the reset wrote the rest pose into every joint (the GPU animation path takes
    // it as base writes); pose the avatar on its next frames even if it is not stamped
    // visible, so the reset never leaves a drawn avatar in that rest pose.
    mVKForceAnimEval = 10;

    LL_DEBUGS("Avatar") << avString() << " reset ends" << LL_ENDL;
}

//-----------------------------------------------------------------------------
// releaseMeshData()
//-----------------------------------------------------------------------------
void LLVOAvatar::releaseMeshData()
{
    if (sInstances.size() < AVATAR_RELEASE_THRESHOLD || isUIAvatar())
    {
        return;
    }

    // cleanup mesh data
    for (avatar_joint_list_t::iterator iter = mMeshLOD.begin();
         iter != mMeshLOD.end();
         ++iter)
    {
        LLAvatarJoint* joint = (*iter);
        joint->setValid(false, true);
    }

    //cleanup data
    if (mDrawable.notNull())
    {
        LLFace* facep = mDrawable->getFace(0);
        if (facep)
        {
        facep->setSize(0, 0);
        for(S32 i = mNumInitFaces ; i < mDrawable->getNumFaces(); i++)
        {
            facep = mDrawable->getFace(i);
                if (facep)
                {
            facep->setSize(0, 0);
        }
    }
        }
    }

    mMeshValid = false;
}

//-----------------------------------------------------------------------------
// restoreMeshData()
//-----------------------------------------------------------------------------
// virtual
void LLVOAvatar::restoreMeshData()
{
    llassert(!isSelf());
    if (mDrawable.isNull())
    {
        return;
    }

    //LL_INFOS() << "Restoring" << LL_ENDL;
    mMeshValid = true;
    updateJointLODs();

    // force mesh update as LOD might not have changed to trigger this
    gPipeline.markRebuild(mDrawable, LLDrawable::REBUILD_GEOMETRY);
}

//------------------------------------------------------------------------

//------------------------------------------------------------------------
// LLVOAvatar::processUpdateMessage()
//------------------------------------------------------------------------
U32 LLVOAvatar::processUpdateMessage(LLMessageSystem *mesgsys,
                                     void **user_data,
                                     U32 block_num, const EObjectUpdateType update_type,
                                     LLDataPacker *dp)
{
    const bool had_no_name = !getNVPair("FirstName");

    // Do base class updates...
    U32 retval = LLViewerObject::processUpdateMessage(mesgsys, user_data, block_num, update_type, dp);

    // Print out arrival information once we have name of avatar.
    const bool has_name = getNVPair("FirstName");
    if (had_no_name && has_name)
    {
        mDebugExistenceTimer.reset();
        debugAvatarRezTime("AvatarRezArrivedNotification", "avatar arrived");
    }

    if (retval & LLViewerObject::INVALID_UPDATE)
    {
        if (isSelf())
        {
            //tell sim to cancel this update
            gAgent.teleportViaLocation(gAgent.getPositionGlobal());
        }
    }

    return retval;
}

LLViewerFetchedTexture *LLVOAvatar::getBakedTextureImage(const U8 te, const LLUUID& uuid)
{
    LLViewerFetchedTexture *result = NULL;

    if (uuid == IMG_DEFAULT_AVATAR ||
        uuid == IMG_DEFAULT ||
        uuid == IMG_INVISIBLE)
    {
        // Should already exist, don't need to find it on sim or baked-texture host.
        result = gTextureList.findImage(uuid, TEX_LIST_STANDARD);
    }
    if (!result)
    {
        const std::string url = getImageURL(te,uuid);

        if (url.empty())
        {
            LL_WARNS() << "unable to determine URL for te " << te << " uuid " << uuid << LL_ENDL;
            return NULL;
        }
        LL_DEBUGS("Avatar") << avString() << "get server-bake image from URL " << url << LL_ENDL;
        result = LLViewerTextureManager::getFetchedTextureFromUrl(
            url, FTT_SERVER_BAKE, true, LLGLTexture::BOOST_NONE, LLViewerTexture::LOD_TEXTURE, LLPixelFormat::NONE, LLPixelFormat::NONE, uuid);
        if (result->isMissingAsset())
        {
            result->setIsMissingAsset(false);
        }

    }
    return result;
}

// virtual
S32 LLVOAvatar::setTETexture(const U8 te, const LLUUID& uuid)
{
    if (!isIndexBakedTexture((ETextureIndex)te))
    {
        // Sim still sends some uuids for non-baked slots sometimes - ignore.
        return LLViewerObject::setTETexture(te, LLUUID::null);
    }

    LLViewerFetchedTexture *image = getBakedTextureImage(te,uuid);
    llassert(image);
    return setTETextureCore(te, image);
}

//------------------------------------------------------------------------
// LLVOAvatar::dumpAnimationState()
//------------------------------------------------------------------------
void LLVOAvatar::dumpAnimationState()
{
    LL_INFOS() << "==============================================" << LL_ENDL;
    for (LLVOAvatar::AnimIterator it = mSignaledAnimations.begin(); it != mSignaledAnimations.end(); ++it)
    {
        LLUUID id = it->first;
        std::string playtag = "";
        if (mPlayingAnimations.find(id) != mPlayingAnimations.end())
        {
            playtag = "*";
        }
        LL_INFOS() << gAnimLibrary.animationName(id) << playtag << LL_ENDL;
    }
    for (LLVOAvatar::AnimIterator it = mPlayingAnimations.begin(); it != mPlayingAnimations.end(); ++it)
    {
        LLUUID id = it->first;
        bool is_signaled = mSignaledAnimations.find(id) != mSignaledAnimations.end();
        if (!is_signaled)
        {
            LL_INFOS() << gAnimLibrary.animationName(id) << "!S" << LL_ENDL;
        }
    }
}

//------------------------------------------------------------------------
// idleUpdate()
//------------------------------------------------------------------------
void LLVOAvatar::idleUpdate(LLAgent &agent, const F64 &time)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;

    if (LLApp::isExiting())
        return;

    if (isDead())
    {
        LL_INFOS() << "Warning!  Idle on dead avatar" << LL_ENDL;
        return;
    }

    static LLCachedControl<bool> friends_only(gSavedSettings, "RenderAvatarFriendsOnly", false);
    if (friends_only()
        && !isUIAvatar()
        && !isControlAvatar()
        && !isSelf()
        && !isBuddy())
    {
        if (mNameText)
        {
            mNameIsSet = false;
            mNameText->markDead();
            mNameText = NULL;
            sNumVisibleChatBubbles--;
        }
        deleteParticleSource();
        mVoiceVisualizer->setVoiceEnabled(false);

        return;
    }

    // record time and refresh "tooSlow" status
    updateTooSlow();

    static LLCachedControl<bool> disable_all_render_types(gSavedSettings, "DisableAllRenderTypes");
    if (!(gPipeline.hasRenderType(mIsControlAvatar ? LLPipeline::RENDER_TYPE_CONTROL_AV : LLPipeline::RENDER_TYPE_AVATAR))
        && !disable_all_render_types && !isSelf())
    {
        if (!mIsControlAvatar)
        {
            idleUpdateNameTag(idleCalcNameTagPosition(mLastRootPos));
        }
        return;
    }

    // Update should be happening max once per frame.
    static LLCachedControl<S32> refreshPeriod(gSavedSettings, "AvatarExtentRefreshPeriodBatch");
    static LLCachedControl<S32> refreshMaxPerPeriod(gSavedSettings, "AvatarExtentRefreshMaxPerBatch");
    static S32 upd_freq = refreshPeriod; // initialise to a reasonable default of 1 batch
    static S32 lastRecalibrationFrame{ 0 };

    const S32 thisFrame = LLDrawable::getCurrentFrame();
    if (thisFrame - lastRecalibrationFrame >= upd_freq)
    {
        // Only update at the start of a cycle. .
        // update frequency = ((Num_Avatars -1 / NumberPerPeriod) + 1 ) * Periodicity
        // Given NumberPerPeriod = 5 and Periodicity = 4
        // | NumAvatars  | frequency |
        // +-------------+-----------+
        // |      1      |     4     |
        // |      2      |     4     |
        // |      5      |     4     |
        // |     10      |     8     |
        // |     25      |     20    |

        upd_freq = (((gObjectList.getAvatarCount() - 1) / refreshMaxPerPeriod) + 1)*refreshPeriod;
        lastRecalibrationFrame = thisFrame;
    }

    if ((mLastAnimExtents[0]==LLVector3())||
        (mLastAnimExtents[1])==LLVector3())
    {
        mNeedsExtentUpdate = true;
    }
    else
    {
        // Update extent if necessary.
        // if the frame counnter + the first byte of the UUID % upd_freq = 0 then update the extent.
        mNeedsExtentUpdate = ((thisFrame + mID.mData[0]) % upd_freq == 0);
    }

    checkTextureLoading() ;

    // force immediate pixel area update on avatars using last frames data (before drawable or camera updates)
    setPixelAreaAndAngle(gAgent);

    // force asynchronous drawable update
    if(mDrawable.notNull())
    {
        if (isSitting() && getParent())
        {
            LLViewerObject *root_object = (LLViewerObject*)getRoot();
            LLDrawable* drawablep = root_object->mDrawable;
            // if this object hasn't already been updated by another avatar...
            if (drawablep) // && !drawablep->isState(LLDrawable::EARLY_MOVE))
            {
                if (root_object->isSelected())
                {
                    gPipeline.updateMoveNormalAsync(drawablep);
                }
                else
                {
                    gPipeline.updateMoveDampedAsync(drawablep);
                }
            }
        }
        else
        {
            gPipeline.updateMoveDampedAsync(mDrawable);
        }
    }

    //--------------------------------------------------------------------
    // set alpha flag depending on state
    //--------------------------------------------------------------------

    if (isSelf())
    {
        LLViewerObject::idleUpdate(agent, time);

        // trigger fidget anims
        if (isAnyAnimationSignaled(AGENT_STAND_ANIMS, NUM_AGENT_STAND_ANIMS))
        {
            agent.fidget();
        }
    }
    else
    {
        // Should override the idleUpdate stuff and leave out the angular update part.
        LLQuaternion rotation = getRotation();
        LLViewerObject::idleUpdate(agent, time);
        setRotation(rotation);
    }

    // attach objects that were waiting for a drawable
    if (!mPendingAttachment.empty())
    {
        lazyAttach();
    }

    // animate the character
    // store off last frame's root position to be consistent with camera position
    mLastRootPos = mRoot->getWorldPosition();
    bool detailed_update;
    {
        VK_HITCH_SCOPE("avatarCharacter");
        detailed_update = updateCharacter(agent);
        mVKCharUpdateFrame = LLFrameTimer::getFrameCount();
    }

    static LLUICachedControl<bool> visualizers_in_calls("ShowVoiceVisualizersInCalls", false);
    bool voice_enabled = (visualizers_in_calls || LLVoiceClient::getInstance()->inProximalChannel()) &&
                         LLVoiceClient::getInstance()->getVoiceEnabled(mID);

    LLVector3 hud_name_pos = idleCalcNameTagPosition(mLastRootPos);

    { VK_HITCH_SCOPE("av_voiceVis"); idleUpdateVoiceVisualizer(voice_enabled, hud_name_pos); }
    { VK_HITCH_SCOPE("av_misc"); idleUpdateMisc( detailed_update ); }
    { VK_HITCH_SCOPE("av_appearAnim"); idleUpdateAppearanceAnimation(); }
    if (detailed_update)
    {
        VK_HITCH_SCOPE("av_detailFx");
        idleUpdateLipSync( voice_enabled );
        idleUpdateLoadingEffect();
        idleUpdateBelowWater();
        // (#338: idleUpdateWindEffect -- LL's cloth ripple for the GL avatar shader -- is
        // gone; nothing read its mWindVec / mRipplePhase / mWindFreq)
    }

    { VK_HITCH_SCOPE("av_nameTag"); idleUpdateNameTag(hud_name_pos); }

    // Complexity has stale mechanics, but updates still can be very rapid
    // so spread avatar complexity calculations over frames to lesen load from
    // rapid updates and to make sure all avatars are not calculated at once.
    S32 compl_upd_freq = 20;
    if (isControlAvatar())
    {
        // animeshes do not (or won't) have impostors nor change outfis,
        // no need for high frequency
        compl_upd_freq = 100;
    }
    else if (mLastRezzedStatus <= 0) //cloud or  init
    {
        compl_upd_freq = 60;
    }
    else if (isSelf())
    {
        compl_upd_freq = 5;
    }
    else if (mLastRezzedStatus == 1) //'grey', not fully loaded
    {
        compl_upd_freq = 40;
    }
    else if (isInMuteList()) //cheap, buffers value from search
    {
        compl_upd_freq = 100;
    }

    if ((LLFrameTimer::getFrameCount() + mID.mData[0]) % compl_upd_freq == 0)
    {
        // DEPRECATED
        // replace with LLPipeline::profileAvatar?
        // Avatar profile takes ~ 0.5ms while idleUpdateRenderComplexity takes ~5ms
        // (both are unacceptably costly)
        VK_HITCH_SCOPE("av_complexity");
        idleUpdateRenderComplexity();
    }
    { VK_HITCH_SCOPE("av_debugInfo"); idleUpdateDebugInfo(); }
}

void LLVOAvatar::idleUpdateVoiceVisualizer(bool voice_enabled, const LLVector3 &position)
{
    bool render_visualizer = voice_enabled;

    // Don't render the user's own voice visualizer when in mouselook, or when opening the mic is disabled.
    if(isSelf())
    {
        static LLCachedControl<bool> voice_disable_mic(gSavedSettings, "VoiceDisableMic");
        if(gAgentCamera.cameraMouselook() || voice_disable_mic)
        {
            render_visualizer = false;
        }
    }

    mVoiceVisualizer->setVoiceEnabled(render_visualizer);

    if ( voice_enabled )
    {
        //----------------------------------------------------------------
        // Only do gesture triggering for your own avatar, and only when you're in a proximal channel.
        //----------------------------------------------------------------
        if( isSelf() )
        {
            //----------------------------------------------------------------------------------------
            // The following takes the voice signal and uses that to trigger gesticulations.
            //----------------------------------------------------------------------------------------
            int lastGesticulationLevel = mCurrentGesticulationLevel;
            mCurrentGesticulationLevel = mVoiceVisualizer->getCurrentGesticulationLevel();

            //---------------------------------------------------------------------------------------------------
            // If "current gesticulation level" changes, we catch this, and trigger the new gesture
            //---------------------------------------------------------------------------------------------------
            if ( lastGesticulationLevel != mCurrentGesticulationLevel )
            {
                if ( mCurrentGesticulationLevel != VOICE_GESTICULATION_LEVEL_OFF )
                {
                    std::string gestureString = "unInitialized";
                    if ( mCurrentGesticulationLevel == 0 )  { gestureString = "/voicelevel1";   }
                    else    if ( mCurrentGesticulationLevel == 1 )  { gestureString = "/voicelevel2";   }
                    else    if ( mCurrentGesticulationLevel == 2 )  { gestureString = "/voicelevel3";   }
                    else    { LL_INFOS() << "oops - CurrentGesticulationLevel can be only 0, 1, or 2"  << LL_ENDL; }

                    // this is the call that Karl S. created for triggering gestures from within the code.
                    LLGestureMgr::instance().triggerAndReviseString( gestureString );
                }
            }

        } //if( isSelf() )

        //-----------------------------------------------------------------------------------------------------------------
        // If the avatar is speaking, then the voice amplitude signal is passed to the voice visualizer.
        // Also, here we trigger voice visualizer start and stop speaking, so it can animate the voice symbol.
        //
        // Notice the calls to "gAwayTimer.reset()". This resets the timer that determines how long the avatar has been
        // "away", so that the avatar doesn't lapse into away-mode (and slump over) while the user is still talking.
        //-----------------------------------------------------------------------------------------------------------------
        if (LLVoiceClient::getInstance()->getIsSpeaking( mID ) && (!isInMuteList() || isSelf()))
        {
            if (!mVoiceVisualizer->getCurrentlySpeaking())
            {
                mVoiceVisualizer->setStartSpeaking();

                //printf( "gAwayTimer.reset();\n" );
            }

            mVoiceVisualizer->setSpeakingAmplitude( LLVoiceClient::getInstance()->getCurrentPower( mID ) );

            if( isSelf() )
            {
                gAgent.clearAFK();
            }
        }
        else
        {
            if ( mVoiceVisualizer->getCurrentlySpeaking() )
            {
                mVoiceVisualizer->setStopSpeaking();

                if ( mLipSyncActive )
                {
                    if( mOohMorph ) mOohMorph->setWeight(mOohMorph->getMinWeight());
                    if( mAahMorph ) mAahMorph->setWeight(mAahMorph->getMinWeight());

                    mLipSyncActive = false;
                    LLCharacter::updateVisualParams();
                    dirtyMesh();
                }
            }
        }
        mVoiceVisualizer->setPositionAgent(position);
    }//if ( voiceEnabled )
}

void LLVOAvatar::idleUpdateMisc(bool detailed_update)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;
    if (LLVOAvatar::sJointDebug)
    {
        LL_INFOS() << getDebugName() << ": joint touches: " << LLJoint::sNumTouches << " updates: " << LLJoint::sNumUpdates << LL_ENDL;
    }

    LLJoint::sNumUpdates = 0;
    LLJoint::sNumTouches = 0;

    bool visible = isVisible() || mNeedsAnimUpdate;

    // update attachments positions
    if (detailed_update)
    {
        // #338: once per frame, not per avatar (two selection-handle copies and a
        // getObjectCount node cleanup walk each); the selection changes with input and
        // messages, never inside the avatars' idle updates.
        static U32 s_sel_frame = ~0u;
        static bool s_attachment_selected = false;
        if (s_sel_frame != LLFrameTimer::getFrameCount())
        {
            s_sel_frame = LLFrameTimer::getFrameCount();
            s_attachment_selected = LLSelectMgr::getInstance()->getSelection()->getObjectCount() > 0 && LLSelectMgr::getInstance()->getSelection()->isAttachment();
        }
        const bool attachment_selected = s_attachment_selected;
        // #257: walk only the occupied points (a cached list, rebuilt when any
        // attachment's object list or this avatar's point map changes) instead of
        // every map node of every avatar every frame. Same order as the map.
        if (mOccupiedAttachGen != LLViewerJointAttachment::sOccupancyGen)
        {
            mOccupiedAttachCache.clear();
            for (const auto& [attachment_point_id, attachment] : mAttachmentPoints)
                if (attachment && !attachment->mAttachedObjects.empty()) mOccupiedAttachCache.push_back(attachment);
            mOccupiedAttachGen = LLViewerJointAttachment::sOccupancyGen;
        }
        for (LLViewerJointAttachment* attachment : mOccupiedAttachCache)
        {
            for (auto& attached_object : attachment->mAttachedObjects)
            {
                if (attached_object.isNull()
                    || attached_object->isDead()
                    || !attachment->getValid()
                    || attached_object->mDrawable.isNull())
                {
                    continue;
                }

                // An off-screen avatar moves only its large attachments: LL's 2 m test on
                // the radius of the attachment's spatial bridge, now on the linkset's own
                // world box (attachments have no bridge, #477).
                if (!visible)
                {
                    LLVector4a ext[2];
                    if (attachment_world_extents(attached_object, ext))
                    {
                        LLVector4a diagonal;
                        diagonal.setSub(ext[1], ext[0]);
                        if (diagonal.getLength3().getF32() * 0.5f < 2.f)
                        {
                            continue;
                        }
                    }
                }

                // VK: LL's octree (spatial bridges, partitions, group render order) is not
                // used for culling, picking or drawing -- the GPU cull, the instance store's
                // pick and the joint table own those. Only the attachment drawable's own
                // move (CPU consumers: attached lights, particles, hover text, sounds) is
                // kept; it reports the move to the clean pipeline itself (updateXform).
                if (attachment_selected)
                {
                    // if selecting any attachments, update all of them as non-damped
                    gPipeline.updateMoveNormalAsync(attached_object->mDrawable);
                }
                else
                {
                    // Note: SL-17415; While most objects follow joints,
                    // some objects get position updates from server
                    gPipeline.updateMoveDampedAsync(attached_object->mDrawable);
                }
                // No updateText() here: LLHUDText::updateVisibility() re-derives the text
                // position from its source object every frame anyway.
            }
        }
    }

    mNeedsAnimUpdate = false;

    if (isImpostor() && !mNeedsImpostorUpdate)
    {
        LL_ALIGN_16(LLVector4a ext[2]);
        F32 distance;
        LLVector3 angle;

        getImpostorValues(ext, angle, distance);

        for (U32 i = 0; i < 3 && !mNeedsImpostorUpdate; i++)
        {
            F32 cur_angle = angle.mV[i];
            F32 old_angle = mImpostorAngle.mV[i];
            F32 angle_diff = fabsf(cur_angle-old_angle);

            if (angle_diff > F_PI/512.f*distance*mUpdatePeriod)
            {
                mNeedsImpostorUpdate = true;
                mLastImpostorUpdateReason = 2;
            }
        }

        if (detailed_update && !mNeedsImpostorUpdate)
        {   //update impostor if view angle, distance, or bounding box change
            //significantly

            F32 dist_diff = fabsf(distance-mImpostorDistance);
            if (dist_diff/mImpostorDistance > 0.1f)
            {
                mNeedsImpostorUpdate = true;
                mLastImpostorUpdateReason = 3;
            }
            else
            {
                ext[0].load3(mLastAnimExtents[0].mV);
                ext[1].load3(mLastAnimExtents[1].mV);
                // Expensive. Just call this once per frame, in updateSpatialExtents();
                //calculateSpatialExtents(ext[0], ext[1]);
                LLVector4a diff;
                diff.setSub(ext[1], mImpostorExtents[1]);
                if (diff.getLength3().getF32() > 0.05f)
                {
                    mNeedsImpostorUpdate = true;
                    mLastImpostorUpdateReason = 4;
                }
                else
                {
                    diff.setSub(ext[0], mImpostorExtents[0]);
                    if (diff.getLength3().getF32() > 0.05f)
                    {
                        mNeedsImpostorUpdate = true;
                        mLastImpostorUpdateReason = 5;
                    }
                }
            }
        }
    }

    if (mDrawable.notNull())
    {
        // Avatars have no region octree (#476): refresh the drawable's entry data (box,
        // centre) that the per-frame partition move used to refresh.
        mDrawable->updateSpatialExtents();

        //force a move if sitting on an active object
        if (getParent() && ((LLViewerObject*) getParent())->mDrawable->isActive())
        {
            gPipeline.markMoved(mDrawable, true);
        }
    }
}

void LLVOAvatar::idleUpdateAppearanceAnimation()
{
    // update morphing params
    if (mAppearanceAnimating)
    {
        ESex avatar_sex = getSex();
        F32 appearance_anim_time = mAppearanceMorphTimer.getElapsedTimeF32();
        if (appearance_anim_time >= APPEARANCE_MORPH_TIME)
        {
            mAppearanceAnimating = false;
            for (LLVisualParam *param = getFirstVisualParam();
                 param;
                 param = getNextVisualParam())
            {
                if (param->isTweakable())
                {
                    param->stopAnimating();
                }
            }
            updateVisualParams();
        }
        else
        {
            if (!isSelf())
            {
                F32 morph_amt = calcMorphAmount();
                // animate only top level params for non-self avatars
                animateTweakableVisualParams(morph_amt);
            }

            // apply all params
            applyAllVisualParams(avatar_sex);

            mLastAppearanceBlendTime = appearance_anim_time;
        }
        dirtyMesh();
    }
}

F32 LLVOAvatar::calcMorphAmount() const
{
    F32 appearance_anim_time = mAppearanceMorphTimer.getElapsedTimeF32();
    F32 blend_frac = calc_bouncy_animation(appearance_anim_time / APPEARANCE_MORPH_TIME);
    F32 last_blend_frac = calc_bouncy_animation(mLastAppearanceBlendTime / APPEARANCE_MORPH_TIME);

    F32 morph_amt;
    if (last_blend_frac == 1.f)
    {
        morph_amt = 1.f;
    }
    else
    {
        morph_amt = (blend_frac - last_blend_frac) / (1.f - last_blend_frac);
    }

    return morph_amt;
}

void LLVOAvatar::idleUpdateLipSync(bool voice_enabled)
{
    // Use the Lipsync_Ooh and Lipsync_Aah morphs for lip sync
    if (voice_enabled
        && mLastRezzedStatus > 0 // no point updating lip-sync for clouds
        && LLVoiceVisualizer::getLipSyncEnabled()
        && LLVoiceClient::getInstance()->getIsSpeaking(mID))
    {
        F32 ooh_morph_amount = 0.0f;
        F32 aah_morph_amount = 0.0f;

        mVoiceVisualizer->lipSyncOohAah( ooh_morph_amount, aah_morph_amount );

        if (mOohMorph)
        {
            F32 ooh_weight = mOohMorph->getMinWeight()
                + ooh_morph_amount * (mOohMorph->getMaxWeight() - mOohMorph->getMinWeight());

            mOohMorph->setWeight( ooh_weight);
        }

        if (mAahMorph)
        {
            F32 aah_weight = mAahMorph->getMinWeight()
                + aah_morph_amount * (mAahMorph->getMaxWeight() - mAahMorph->getMinWeight());

            mAahMorph->setWeight( aah_weight);
        }

        mLipSyncActive = true;
        LLCharacter::updateVisualParams();
        dirtyMesh();
    }
}

void LLVOAvatar::idleUpdateLoadingEffect()
{
    static LLCachedControl<bool> s_draw_loading(gSavedSettings, "GMDXDrawLoadingAvatars", true);   // <GMDX>
    // update visibility when avatar is partially loaded
    if (!mFullyLoaded && updateIsFullyLoaded()) // Avoid repeat calculations by checking if mFullyLoaded is true first.
    {
        if (isFullyLoaded())
        {
            if (mFirstFullyVisible)
            {
                mFirstFullyVisible = false;
                mLastCloudAttachmentCount = (S32)mSimAttachments.size();
                mFirstDecloudTime = mFirstAppearanceMessageTimer.getElapsedTimeF32();
                if (isSelf())
                {
                    LL_INFOS("Avatar") << avString() << "self isFullyLoaded, mFirstFullyVisible after " << mFirstDecloudTime << LL_ENDL;
                    LLAppearanceMgr::instance().onFirstFullyVisible();
                }
                else
                {
                    LL_INFOS("Avatar") << avString() << "other isFullyLoaded, mFirstFullyVisible after " << mFirstDecloudTime << LL_ENDL;
                }
            }

            deleteParticleSource();
            updateLOD();
        }
        // <GMDX> GMDXDrawLoadingAvatars: the avatar is drawn while it loads
        // (llcleansysavatar.cpp), so no cloud -- and one made before the switch goes.
        else if (s_draw_loading)
        {
            deleteParticleSource();
        }
        // </GMDX>
        else
        {
            LLPartSysData particle_parameters;

            // fancy particle cloud designed by Brent
            particle_parameters.mPartData.mMaxAge            = 5.f;
            particle_parameters.mPartData.mStartScale        = LLVector2(0.100250f, 0.100250f);
            particle_parameters.mPartData.mEndScale          = LLVector2(1.000250f, 1.000250f);
            particle_parameters.mPartData.mStartGlow         = 0;
            particle_parameters.mPartData.mEndGlow           = 0;
            particle_parameters.mPartData.mStartColor        = LLColor4(0.501773f, 0.743102f, 1.000000f, 1.f);
            particle_parameters.mPartData.mEndColor          = LLColor4(0.000000f, 0.000000f, 0.000000f, 1.f);
            particle_parameters.mPartData.mBlendFuncSource   = LLPartData::LL_PART_BF_SOURCE_COLOR;
            particle_parameters.mPartData.mBlendFuncDest     = LLPartData::LL_PART_BF_SOURCE_ALPHA;

            particle_parameters.mPartImageID                 = sCloudTexture->getID();
            particle_parameters.mMaxAge                      = 0.f;
            particle_parameters.mPattern                     = LLPartSysData::LL_PART_SRC_PATTERN_EXPLODE;
            particle_parameters.mInnerAngle                  = 0.f;
            particle_parameters.mOuterAngle                  = 0.f;
            particle_parameters.mBurstRate                   = 0.02f;
            particle_parameters.mBurstRadius                 = 0.0f;
            particle_parameters.mBurstPartCount              = 1;
            particle_parameters.mBurstSpeedMin               = 0.01f;
            particle_parameters.mBurstSpeedMax               = 0.6f;
            particle_parameters.mPartData.mFlags             = ( LLPartData::LL_PART_INTERP_COLOR_MASK | LLPartData::LL_PART_INTERP_SCALE_MASK |
                                                                 LLPartData::LL_PART_EMISSIVE_MASK | LLPartData::LL_PART_FOLLOW_SRC_MASK |
                                                                 LLPartData::LL_PART_TARGET_POS_MASK );

            // do not generate particles for dummy or overly-complex avatars
            if (!mIsDummy && !isTooComplex() && !isTooSlow())
            {
                setParticleSource(particle_parameters, getID());
            }
        }
    }
}

void LLVOAvatar::idleUpdateNameTag(const LLVector3& root_pos_last)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;

    // update chat bubble
    //--------------------------------------------------------------------
    // draw text label over character's head
    //--------------------------------------------------------------------
    if (mChatTimer.getElapsedTimeF32() > BUBBLE_CHAT_TIME)
    {
        mChats.clear();
    }

    const F32 time_visible = mTimeVisible.getElapsedTimeF32();

    static LLCachedControl<F32> NAME_SHOW_TIME(gSavedSettings, "RenderNameShowTime"); // seconds
    static LLCachedControl<F32> FADE_DURATION(gSavedSettings, "RenderNameFadeDuration"); // seconds
    static LLCachedControl<bool> use_chat_bubbles(gSavedSettings, "UseChatBubbles");

// [RLVa:KB] - Checked: RLVa-2.0.1
    bool fRlvShowAvTag = true, fRlvShowAvName = true;
    if (RlvActions::isRlvEnabled())
    {
        fRlvShowAvTag = RlvActions::canShowNameTag(this);
        fRlvShowAvName = (fRlvShowAvTag) && (RlvActions::canShowName(RlvActions::SNC_DEFAULT, getID()));
    }
// [/RLVa:KB]
    bool visible_chat = use_chat_bubbles && (mChats.size() || mTyping);
    bool render_name =  visible_chat ||
// [RLVa:KB] - Checked: RLVa-2.0.1
        ((fRlvShowAvTag) &&
         ((sRenderName == RENDER_NAME_ALWAYS) ||
          (sRenderName == RENDER_NAME_FADE && time_visible < NAME_SHOW_TIME)));
// [/RLVa:KB]
//      (((sRenderName == RENDER_NAME_ALWAYS) ||
//        (sRenderName == RENDER_NAME_FADE && time_visible < NAME_SHOW_TIME)));
    // If it's your own avatar, don't draw in mouselook, and don't
    // draw if we're specifically hiding our own name.
    if (isSelf())
    {
        static LLCachedControl<bool> render_name_show_self(gSavedSettings, "RenderNameShowSelf");
        static LLCachedControl<S32> name_tag_mode(gSavedSettings, "AvatarNameTagMode");
        render_name = render_name
            && !gAgentCamera.isHidingAvatarForFirstPerson()
            && (visible_chat || (render_name_show_self && name_tag_mode));
    }

    if ( !render_name )
    {
        if (mNameText)
        {
            // ...clean up old name tag
            mNameText->markDead();
            mNameText = NULL;
            sNumVisibleChatBubbles--;
        }
        return;
    }

    bool new_name = false;
    if (visible_chat != mVisibleChat)
    {
        mVisibleChat = visible_chat;
        new_name = true;
    }

// [RLVa:KB] - Checked: RLVa-0.2.0
    if (!fRlvShowAvName)
    {
        if (mRenderGroupTitles)
        {
            mRenderGroupTitles = false;
            new_name = true;
        }
    }
    else if (sRenderGroupTitles != mRenderGroupTitles)
// [/RLVa]
//  if (sRenderGroupTitles != mRenderGroupTitles)
    {
        mRenderGroupTitles = sRenderGroupTitles;
        new_name = true;
    }

    // First Calculate Alpha
    // If alpha > 0, create mNameText if necessary, otherwise delete it
    F32 alpha = 0.f;
    if (mAppAngle > 5.f)
    {
        const F32 START_FADE_TIME = NAME_SHOW_TIME - FADE_DURATION;
        if (!visible_chat && sRenderName == RENDER_NAME_FADE && time_visible > START_FADE_TIME)
        {
            alpha = 1.f - (time_visible - START_FADE_TIME) / FADE_DURATION;
        }
        else
        {
            // ...not fading, full alpha
            alpha = 1.f;
        }
    }
    else if (mAppAngle > 2.f)
    {
        // far away is faded out also
        alpha = (mAppAngle-2.f)/3.f;
    }

    if (alpha <= 0.f)
    {
        if (mNameText)
        {
            mNameText->markDead();
            mNameText = NULL;
            sNumVisibleChatBubbles--;
        }
        return;
    }

    if (!mNameText)
    {
        mNameText = static_cast<LLHUDNameTag*>( LLHUDObject::addHUDObject(
            LLHUDObject::LL_HUD_NAME_TAG) );
        //mNameText->setMass(10.f);
        mNameText->setSourceObject(this);
        mNameText->setVertAlignment(LLHUDNameTag::ALIGN_VERT_TOP);
        mNameText->setVisibleOffScreen(true);
        mNameText->setMaxLines(11);
        mNameText->setFadeDistance(CHAT_NORMAL_RADIUS, 5.f);
        sNumVisibleChatBubbles++;
        new_name = true;
        mBubbleValid = false;   // #335: a fresh tag has no bubble lines yet
    }

    mNameText->setPositionAgent(root_pos_last);

    idleUpdateNameTagText(new_name);
    idleUpdateNameTagAlpha(new_name, alpha);
}

void LLVOAvatar::idleUpdateNameTagText(bool new_name)
{
    // #335: the NV pairs only change with an object update (setNameValueList).
    if (!mNVCacheValid || mNVCacheGen != getNVPairsGeneration())
    {
        mNVTitle = getNVPair("Title");
        mNVFirstName = getNVPair("FirstName");
        mNVLastName = getNVPair("LastName");
        mNVCacheGen = getNVPairsGeneration();
        mNVCacheValid = true;
    }
    LLNameValue *title = mNVTitle;
    LLNameValue* firstname = mNVFirstName;
    LLNameValue* lastname = mNVLastName;

    // Avatars must have a first and last name
    if (!firstname || !lastname) return;

// [RLVa:KB] - Checked: RLVa-2.0.1
    bool fRlvShowAvName = RlvActions::canShowName(RlvActions::SNC_DEFAULT, getID());
// [/RLVa:KB]
    // <FS:Ansariel> Show auto-response in nametag
    static LLCachedControl<bool> autorespond_mode(gSavedPerAccountSettings, "SlipstreamAutoresponseEnable", false);
    static LLCachedControl<bool> autorespond_nonfriends_mode(gSavedPerAccountSettings, "SlipstreamAutoresponseNotFriendEnable", false);
    static LLCachedControl<bool> show_autorespond_in_nametag(gSavedSettings, "FSShowAutorespondInNametag", false);
    bool is_autoresponse = isSelf() && show_autorespond_in_nametag && (autorespond_mode || autorespond_nonfriends_mode);
    // </FS:Ansariel>
    // <FS:Ansariel> FIRE-3475: Show typing in nametag
    static LLCachedControl<bool> show_typing_in_nametag(gSavedSettings, "FSShowTypingStateInNameTag", false);
    bool is_typing = !isSelf() && mTyping && show_typing_in_nametag;
    // </FS:Ansariel>
    bool is_away = false, is_do_not_disturb = false, is_appearance = false;
    for (const auto& sig_anim : mSignaledAnimations)
    {
        const LLUUID& sig_id = sig_anim.first;
        is_away |= (sig_id == ANIM_AGENT_AWAY);
        is_do_not_disturb |= (sig_id == ANIM_AGENT_DO_NOT_DISTURB);
        is_appearance |= (sig_id == ANIM_AGENT_CUSTOMIZE);
    }
    bool is_muted;
    if (isSelf())
    {
        is_muted = false;
    }
    else
    {
        is_muted = isInMuteList();
    }
//    bool is_friend = isBuddy();
// [RLVa:KB] - Checked: RLVa-1.2.2
    bool is_friend = (fRlvShowAvName) && (isBuddy());
// [/RLVa:KB]
    bool is_cloud = getHasMissingParts();

    if (is_appearance != mNameAppearance)
    {
        if (is_appearance)
        {
            debugAvatarRezTime("AvatarRezEnteredAppearanceNotification","entered appearance mode");
        }
        else
        {
            debugAvatarRezTime("AvatarRezLeftAppearanceNotification","left appearance mode");
        }
    }

    // Rebuild name tag if state change detected
    if (!mNameIsSet
        || new_name
        || (!title && !mTitle.empty())
        || (title && mTitle != title->getString())
        || is_away != mNameAway
        || is_do_not_disturb != mNameDoNotDisturb
        || is_autoresponse != mNameAutoResponse // <FS:Ansariel>
        || is_typing != mNameIsTyping // <FS:Ansariel> FIRE-3475
        || is_muted != mNameMute
        || is_appearance != mNameAppearance
        || is_friend != mNameFriend
        || is_cloud != mNameCloud)
    {
        LLColor4 name_tag_color = getNameTagColor(is_friend);

        clearNameTag();

        if (is_away || is_muted || is_do_not_disturb || is_autoresponse || is_appearance || is_typing)
        {
            std::string line;
            if (is_away)
            {
                line += LLTrans::getString("AvatarAway");
                line += ", ";
            }
            if (is_do_not_disturb)
            {
                line += LLTrans::getString("AvatarDoNotDisturb");
                line += ", ";
            }
            // <FS:Ansariel> Show auto-response in nametag
            if (is_autoresponse)
            {
                line += LLTrans::getString("AvatarAutoResponse");
                line += ", ";
            }
            // </FS:Ansariel>
            if (is_muted)
            {
                line += LLTrans::getString("AvatarMuted");
                line += ", ";
            }
            if (is_appearance)
            {
                line += LLTrans::getString("AvatarEditingAppearance");
                line += ", ";
            }
            if (is_cloud)
            {
                line += LLTrans::getString("LoadingData");
                line += ", ";
            }
            // <FS:Ansariel> FIRE-3475: Show typing in nametag
            if (is_typing)
            {
                line += LLTrans::getString("AvatarTyping");
                line += ", ";
            }
            // </FS:Ansariel>
            // trim last ", "
            line.resize( line.length() - 2 );
            addNameTagLine(line, name_tag_color, LLFontGL::NORMAL,
                LLFontGL::getFontSansSerifSmall());
        }

//      if (sRenderGroupTitles
// [RLVa:KB] - Checked: RLVa-1.2.2
        if (sRenderGroupTitles && fRlvShowAvName
// [/RLVa:KB]
            && title && title->getString() && title->getString()[0] != '\0')
        {
            std::string title_str = title->getString();
            LLStringFn::replace_ascii_controlchars(title_str,LL_UNKNOWN_CHAR);
            addNameTagLine(title_str, name_tag_color, LLFontGL::NORMAL,
                LLFontGL::getFontSansSerifSmall(), true);
        }

        static LLUICachedControl<bool> show_display_names("NameTagShowDisplayNames", true);
        static LLUICachedControl<bool> show_usernames("NameTagShowUsernames", true);
        static LLUICachedControl<bool> show_rez_status("NameTagDebugAVRezState", false);

        if (LLAvatarName::useDisplayNames())
        {
            LLAvatarName av_name;
            if (!LLAvatarNameCache::get(getID(), &av_name))
            {
                // Force a rebuild at next idle
                // Note: do not connect a callback on idle().
                clearNameTag();
            }

// [RLVa:KB] - Checked: RLVa-1.2.2
            if ( (fRlvShowAvName) || (isSelf()) )
            {
// [/RLVa:KB]
                // Might be blank if name not available yet, that's OK
                if (show_display_names)
                {
                    addNameTagLine(av_name.getDisplayName(), name_tag_color, LLFontGL::NORMAL,
                        LLFontGL::getFontSansSerif(), true);
                }
                // Suppress SLID display if display name matches exactly (ugh)
                if (show_usernames && !av_name.isDisplayNameDefault())
                {
                    // *HACK: Desaturate the color
                    LLColor4 username_color = name_tag_color * 0.83f;
                    addNameTagLine(av_name.getUserName(), username_color, LLFontGL::NORMAL,
                        LLFontGL::getFontSansSerifSmall(), true);
                }
// [RLVa:KB] - Checked: RLVa-1.2.2
            }
            else
            {
                addNameTagLine(RlvStrings::getAnonym(av_name), name_tag_color, LLFontGL::NORMAL, LLFontGL::getFontSansSerif());
            }
// [/RLVa:KB]
        }
        else
        {
            const LLFontGL* font = LLFontGL::getFontSansSerif();
            std::string full_name = LLCacheName::buildFullName( firstname->getString(), lastname->getString() );
// [RLVa:KB] - Checked: RLVa-1.2.2
            if ( (!fRlvShowAvName) && (!isSelf()) )
            {
                full_name = RlvStrings::getAnonym(full_name);
            }
// [/RLVa:KB]
            addNameTagLine(full_name, name_tag_color, LLFontGL::NORMAL, font, true);
        }

        if (show_rez_status)
        {
            std::string av_string = LLVOAvatar::rezStatusToString(mLastRezzedStatus);
            addNameTagLine(av_string, name_tag_color, LLFontGL::NORMAL, LLFontGL::getFontSansSerifSmall(), true);
        }

        mNameAway = is_away;
        mNameDoNotDisturb = is_do_not_disturb;
        mNameAutoResponse = is_autoresponse; // <FS:Ansariel>
        mNameIsTyping = is_typing; // <FS:Ansariel> FIRE-3475
        mNameMute = is_muted;
        mNameAppearance = is_appearance;
        mNameFriend = is_friend;
        mNameCloud = is_cloud;
        mTitle = title ? title->getString() : "";
        LLStringFn::replace_ascii_controlchars(mTitle,LL_UNKNOWN_CHAR);
        new_name = true;
    }

    if (mVisibleChat)
    {
        // <GMDX> bubbles in the chat log's font (GMDXChatBubbleStyle, llhudnametag.cpp)
        static LLCachedControl<bool> gmdx_bubbles(gSavedSettings, "GMDXChatBubbleStyle", true);
        const LLFontGL* bubble_font = gmdx_bubbles ? LLFontGL::getFont(LLFontDescriptor("Barlow", "Large", 0)) : nullptr;
        mNameText->setFont(bubble_font ? bubble_font : LLFontGL::getFontSansSerif());
        // </GMDX>
        mNameText->setTextAlignment(LLHUDNameTag::ALIGN_TEXT_LEFT);
        mNameText->setFadeDistance(CHAT_NORMAL_RADIUS * 2.f, 5.f);

        std::deque<LLChat>::iterator chat_iter = mChats.begin();

        static const LLUIColor user_chat_color = LLUIColorTable::instance().getColor("UserChatColor");
        static const LLUIColor agent_chat_color = LLUIColorTable::instance().getColor("AgentChatColor");
        LLColor4 new_chat = LGGContactSets::getInstance()->getAvatarColor(getID(), isSelf() ? user_chat_color : agent_chat_color, ContactSetType::CHAT);
        LLColor4 normal_chat = lerp(new_chat, LLColor4(0.8f, 0.8f, 0.8f, 1.f), 0.7f);
        LLColor4 old_chat = lerp(normal_chat, LLColor4(0.6f, 0.6f, 0.6f, 1.f), 0.7f);
        if (gmdx_bubbles)   // <GMDX> chat lines stay near-white at full alpha until they expire (no greying with age)
        {
            new_chat = lerp(new_chat, LLColor4::white, 0.75f);
            new_chat.mV[VALPHA] = 1.f;
            normal_chat = old_chat = new_chat;
        }
        if (mTyping && mChats.size() >= MAX_BUBBLE_CHAT_UTTERANCES)
        {
            ++chat_iter;
        }

        // The lines LL's per-frame rebuild would add (same order, colours, styles)...
        mBubbleScratch.clear();
        for(; chat_iter != mChats.end(); ++chat_iter)
        {
            F32 chat_fade_amt = llclamp((F32)((LLFrameTimer::getElapsedSeconds() - chat_iter->mTime) / CHAT_FADE_TIME), 0.f, 4.f);
            LLFontGL::StyleFlags style;
            switch(chat_iter->mChatType)
            {
            case CHAT_TYPE_WHISPER:
                style = LLFontGL::ITALIC;
                break;
            case CHAT_TYPE_SHOUT:
                style = LLFontGL::BOLD;
                break;
            default:
                style = LLFontGL::NORMAL;
                break;
            }
            if (chat_fade_amt < 1.f)
            {
                F32 u = clamp_rescale(chat_fade_amt, 0.9f, 1.f, 0.f, 1.f);
                mBubbleScratch.push_back({ &chat_iter->mText, lerp(new_chat, normal_chat, u), (U8)style });
            }
            else if (chat_fade_amt < 2.f)
            {
                F32 u = clamp_rescale(chat_fade_amt, 1.9f, 2.f, 0.f, 1.f);
                mBubbleScratch.push_back({ &chat_iter->mText, lerp(normal_chat, old_chat, u), (U8)style });
            }
            else if (chat_fade_amt < 3.f)
            {
                // *NOTE: only remove lines down to minimum number
                mBubbleScratch.push_back({ &chat_iter->mText, old_chat, (U8)style });
            }
        }

        if (mTyping)
        {
            static const std::string s_dots[3] = { ".", "..", "..." };
            S32 dot_count = (llfloor(mTypingTimer.getElapsedTimeF32() * 3.f) + 2) % 3 + 1;
            switch(dot_count)
            {
            case 1:
            case 2:
            case 3:
                mBubbleScratch.push_back({ &s_dots[dot_count - 1], new_chat, (U8)LLFontGL::NORMAL });
                break;
            }
        }

        // ...rebuilt only when they differ from the tag's current text (#335).
        bool same = mBubbleValid
            && mBubbleTag == mNameText.get()
            && mBubbleTextGen == mNameText->getTextGeneration()
            && mBubbleLines.size() == mBubbleScratch.size();
        for (size_t i = 0; same && i < mBubbleScratch.size(); ++i)
        {
            const BubbleLine& have = mBubbleLines[i];
            const BubbleLineRef& want = mBubbleScratch[i];
            same = have.style == want.style && have.color == want.color && have.text == *want.text;
        }
        if (same)
        {
            LLVKHitch::count("avBubbleKeep");
        }
        else
        {
            LLVKHitch::count("avBubbleRebuild");
            mNameText->clearString();
            mBubbleLines.resize(mBubbleScratch.size());
            for (size_t i = 0; i < mBubbleScratch.size(); ++i)
            {
                const BubbleLineRef& want = mBubbleScratch[i];
                mNameText->addLine(*want.text, want.color, (LLFontGL::StyleFlags)want.style);
                BubbleLine& have = mBubbleLines[i];
                have.text = *want.text;
                have.color = want.color;
                have.style = want.style;
            }
            mBubbleTag = mNameText.get();
            mBubbleTextGen = mNameText->getTextGeneration();
            mBubbleValid = true;
        }
        mBubbleScratch.clear();
        mNameText->setVisibleOffScreen(true);
    }
    else
    {
        // ...not using chat bubbles, just names
        mNameText->setTextAlignment(LLHUDNameTag::ALIGN_TEXT_CENTER);
        mNameText->setFadeDistance(CHAT_NORMAL_RADIUS, 5.f);
        mNameText->setVisibleOffScreen(false);
    }
}

void LLVOAvatar::addNameTagLine(const std::string& line, const LLColor4& color, S32 style, const LLFontGL* font, const bool use_ellipses)
{
    // extra width (NAMETAG_MAX_WIDTH) is for names only, not for chat
    llassert(mNameText);
    if (mVisibleChat)
    {
        // <GMDX> in the chat log's bubble style the name keeps its colour (group / contact set)
        static LLCachedControl<bool> gmdx_bubbles(gSavedSettings, "GMDXChatBubbleStyle", true);
        mNameText->addLabel(line, LLHUDNameTag::NAMETAG_MAX_WIDTH, gmdx_bubbles ? &color : nullptr);
    }
    else
    {
        mNameText->addLine(line, color, (LLFontGL::StyleFlags)style, font, use_ellipses, LLHUDNameTag::NAMETAG_MAX_WIDTH);
    }
    mNameIsSet |= !line.empty();
}

void LLVOAvatar::clearNameTag()
{
    mNameIsSet = false;
    if (mNameText)
    {
        mNameText->setLabel("");
        mNameText->setString("");
    }
    mTimeVisible.reset();
}

// static
void LLVOAvatar::invalidateNameTag(const LLUUID& agent_id)
{
    if (LLViewerObject* obj = gObjectList.findObject(agent_id))
    {
        if (LLVOAvatar* avatar = dynamic_cast<LLVOAvatar*>(obj))
        {
            avatar->clearNameTag();
        }
    }
}

// static
void LLVOAvatar::invalidateNameTags()
{
    for (LLCharacter* character : LLCharacter::sInstances)
    {
        LLVOAvatar* avatar = (LLVOAvatar*)character;
        if (!avatar->isDead())
        {
            avatar->clearNameTag();
        }
    }
}

// Compute name tag position during idle update
LLVector3 LLVOAvatar::idleCalcNameTagPosition(const LLVector3 &root_pos_last)
{
    LLQuaternion root_rot = mRoot->getWorldRotation();
    LLQuaternion inv_root_rot = ~root_rot;
    LLVector3 pixel_right_vec;
    LLVector3 pixel_up_vec;
    LLViewerCamera::getInstance()->getPixelVectors(root_pos_last, pixel_up_vec, pixel_right_vec);
    LLVector3 camera_to_av = root_pos_last - LLViewerCamera::getInstance()->getOrigin();
    camera_to_av.normalize();
    LLVector3 local_camera_at = camera_to_av * inv_root_rot;
    LLVector3 local_camera_up = camera_to_av % LLViewerCamera::getInstance()->getLeftAxis();
    local_camera_up.normalize();
    local_camera_up = local_camera_up * inv_root_rot;

    // position is based on head position, does not require mAvatarOffset here. - Nyx
    LLVector3 avatar_ellipsoid(mBodySize.mV[VX] * 0.4f,
                                mBodySize.mV[VY] * 0.4f,
                                mBodySize.mV[VZ] * NAMETAG_VERT_OFFSET_WEIGHT);

    local_camera_up.scaleVec(avatar_ellipsoid);
    local_camera_at.scaleVec(avatar_ellipsoid);

    LLVector3 head_offset = (mHeadp->getLastWorldPosition() - mRoot->getLastWorldPosition()) * inv_root_rot;

    if (dist_vec(head_offset, mTargetRootToHeadOffset) > NAMETAG_UPDATE_THRESHOLD)
    {
        mTargetRootToHeadOffset = head_offset;
    }

    mCurRootToHeadOffset = lerp(mCurRootToHeadOffset, mTargetRootToHeadOffset, LLSmoothInterpolation::getInterpolant(0.2f));

    LLVector3 name_position = mRoot->getLastWorldPosition() + (mCurRootToHeadOffset * root_rot);
    name_position += (local_camera_up * root_rot) - (projected_vec(local_camera_at * root_rot, camera_to_av));
    name_position += pixel_up_vec * NAMETAG_VERTICAL_SCREEN_OFFSET;

    // Avoid of crossing the name tag by the water surface
    if (mNameText)
    {
        F32 water_height = getRegion()->getWaterHeight();
        static const F32 WATER_HEIGHT_ABOVE_DELTA = 0.25;
        if (name_position[VZ] < water_height + WATER_HEIGHT_ABOVE_DELTA)
        {
            F32 camera_height = LLViewerCamera::getInstance()->getOrigin()[VZ];
            if (camera_height >= water_height)
            {
                F32 name_world_height = mNameText->getWorldHeight();
                static const F32 WATER_HEIGHT_BELOW_DELTA = 0.5;
                if (name_position[VZ] + name_world_height > water_height - WATER_HEIGHT_BELOW_DELTA)
                {
                    name_position[VZ] = water_height + WATER_HEIGHT_ABOVE_DELTA;
                }
            }
        }
    }

    return name_position;
}

void LLVOAvatar::idleUpdateNameTagAlpha(bool new_name, F32 alpha)
{
    llassert(mNameText);

    if (new_name
        || alpha != mNameAlpha)
    {
        mNameText->setAlpha(alpha);
        mNameAlpha = alpha;
    }
}

LLColor4 LLVOAvatar::getNameTagColor(bool is_friend)
{
#if 0
    static LLUICachedControl<bool> show_friends("NameTagShowFriends", false);
    const char* color_name;
    if (show_friends && is_friend)
    {
        color_name = "NameTagFriend";
    }
    else if (LLAvatarName::useDisplayNames())
    {
        // ...color based on whether username "matches" a computed display name
        LLAvatarName av_name;
        if (LLAvatarNameCache::get(getID(), &av_name) && av_name.isDisplayNameDefault())
        {
            color_name = "NameTagMatch";
        }
        else
        {
            color_name = "NameTagMismatch";
        }
    }
    else
    {
        // ...not using display names
        color_name = "NameTagLegacy";
    }
    return LLUIColorTable::getInstance()->getColor( color_name );
#endif
    static LLUIColor name_tag_match = LLUIColorTable::instance().getColor("NameTagMatch");
    LLColor4 color_name = name_tag_match;
    color_name = LGGContactSets::getInstance()->getAvatarColor(getID(), color_name, ContactSetType::TAG);
    color_name = LLGroupColorMap::instance().tint(getID(), color_name); // <GMDX> group colour wins over contact sets

    return color_name;
}

void LLVOAvatar::idleUpdateBelowWater()
{
    F32 avatar_height = (F32)(getPositionGlobal().mdV[VZ]);

    F32 water_height;
    water_height = getRegion()->getWaterHeight();

    mBelowWater =  avatar_height < water_height;
}

void LLVOAvatar::slamPosition()
{
    gAgent.setPositionAgent(getPositionAgent());
    // SL-315
    mRoot->setWorldPosition(getPositionAgent()); // teleport
    setChanged(TRANSLATED);
    if (mDrawable.notNull())
    {
        gPipeline.updateMoveNormalAsync(mDrawable);
    }
    mRoot->updateWorldMatrixChildren();
}

bool LLVOAvatar::isVisuallyMuted()
{
    bool muted = false;

    // Priority order (highest priority first)
    // * own avatar is never visually muted
    // * if on the "always draw normally" list, draw them normally
    // * if on the "always visually mute" list, mute them
    // * check against the render cost and attachment limits
    if (!isSelf())
    {
// [RLVa:KB] - Checked: RLVa-2.2 (@setcam_avdist)
        if (isRlvSilhouette())
        {
            muted = true;
        }
        else if (mVisuallyMuteSetting == AV_ALWAYS_RENDER)
// [/RLVa:KB]
//      if (mVisuallyMuteSetting == AV_ALWAYS_RENDER)
        {
            muted = false;
        }
        else if (mVisuallyMuteSetting == AV_DO_NOT_RENDER)
        {
#ifdef JELLYDOLLS_SHOULD_IMPOSTOR
            muted = true;
            // Always want to see this AV as an impostor
#else
            muted = false;
#endif
        }
        else if (isInMuteList())
        {
            muted = true;
        }
        else if (mIsControlAvatar)
        {
            muted = isTooSlow();
        }
        else
        {
            muted = isTooComplex() || isTooSlow();
        }
    }

    return muted;
}

bool LLVOAvatar::isInMuteList() const
{
    bool muted = false;
    F64 now = LLFrameTimer::getTotalSeconds();
    if (now < mCachedMuteListUpdateTime)
    {
        muted = mCachedInMuteList;
    }
    else
    {
        muted = LLMuteList::getInstance()->isMuted(getID());

        const F64 SECONDS_BETWEEN_MUTE_UPDATES = 1;
        mCachedMuteListUpdateTime = now + SECONDS_BETWEEN_MUTE_UPDATES;
        mCachedInMuteList = muted;
    }
    return muted;
}

// [RLVa:KB] - Checked: RLVa-2.2 (@setcam_avdist)
bool LLVOAvatar::isRlvSilhouette() const
{
    if (!RlvActions::hasBehaviour(RLV_BHVR_SETCAM_AVDIST))
        return false;

    static RlvCachedBehaviourModifier<float> s_nSetCamAvDist(RLV_MODIFIER_SETCAM_AVDIST);

    const F64 now = LLFrameTimer::getTotalSeconds();
    if (now >= mCachedRlvSilhouetteUpdateTime)
    {
        const F64 SECONDS_BETWEEN_SILHOUETTE_UPDATES = .5f;
        bool fIsRlvSilhouette = dist_vec_squared(gAgent.getPositionGlobal(), getPositionGlobal()) > s_nSetCamAvDist() * s_nSetCamAvDist();
        if (fIsRlvSilhouette != mCachedIsRlvSilhouette)
        {
            mCachedIsRlvSilhouette = fIsRlvSilhouette;
            mNeedsImpostorUpdate = true;
        }
        mCachedRlvSilhouetteUpdateTime = now + SECONDS_BETWEEN_SILHOUETTE_UPDATES;
    }
    return mCachedIsRlvSilhouette;
}
// [/RLVa:KB]

void LLVOAvatar::updateAppearanceMessageDebugText()
{
        S32 central_bake_version = -1;
        if (getRegion())
        {
            central_bake_version = getRegion()->getCentralBakeVersion();
        }
        bool all_baked_downloaded = allBakedTexturesCompletelyDownloaded();
        bool all_local_downloaded = allLocalTexturesCompletelyDownloaded();
        std::string debug_line = llformat("%s%s - mLocal: %d, mEdit: %d, mUSB: %d, CBV: %d",
                                          isSelf() ? (all_local_downloaded ? "L" : "l") : "-",
                                          all_baked_downloaded ? "B" : "b",
                                          mUseLocalAppearance, mIsEditingAppearance,
                                          1, central_bake_version);
        std::string origin_string = bakedTextureOriginInfo();
        debug_line += " [" + origin_string + "]";
        S32 curr_cof_version = LLAppearanceMgr::instance().getCOFVersion();
        S32 last_request_cof_version = mLastUpdateRequestCOFVersion;
        S32 last_received_cof_version = mLastUpdateReceivedCOFVersion;
        if (isSelf())
        {
            debug_line += llformat(" - cof: %d req: %d rcv:%d",
                                   curr_cof_version, last_request_cof_version, last_received_cof_version);
            static LLCachedControl<bool> debug_force_failure(gSavedSettings, "DebugForceAppearanceRequestFailure");
            if (debug_force_failure)
            {
                debug_line += " FORCING ERRS";
            }
        }
        else
        {
            debug_line += llformat(" - cof rcv:%d", last_received_cof_version);
        }
        debug_line += llformat(" bsz-z: %.3f", mBodySize[2]);
        if (mAvatarOffset[2] != 0.0f)
        {
            debug_line += llformat("avofs-z: %.3f", mAvatarOffset[2]);
        }
        bool hover_enabled = getRegion() && getRegion()->avatarHoverHeightEnabled();
        debug_line += hover_enabled ? " H" : " h";
        const LLVector3& hover_offset = getHoverOffset();
        if (hover_offset[2] != 0.0)
        {
            debug_line += llformat(" hov_z: %.3f", hover_offset[2]);
            debug_line += llformat(" %s", (isSitting() ? "S" : "T"));
            debug_line += llformat("%s", (isMotionActive(ANIM_AGENT_SIT_GROUND_CONSTRAINED) ? "G" : "-"));
        }
        if (mInAir)
        {
            debug_line += " A";

        }

        LLVector3 ankle_right_pos_agent = mFootRightp->getWorldPosition();
        LLVector3 normal;
        LLVector3 ankle_right_ground_agent = ankle_right_pos_agent;
        resolveHeightAgent(ankle_right_pos_agent, ankle_right_ground_agent, normal);
        F32 rightElev = llmax(-0.2f, ankle_right_pos_agent.mV[VZ] - ankle_right_ground_agent.mV[VZ]);
        debug_line += llformat(" relev %.3f", rightElev);

        LLVector3 root_pos = mRoot->getPosition();
        LLVector3 pelvis_pos = mPelvisp->getPosition();
        debug_line += llformat(" rp %.3f pp %.3f", root_pos[2], pelvis_pos[2]);

        const LLVector3& scale = getScale();
        debug_line += llformat(" scale-z %.3f", scale[2]);
        S32 is_visible = (S32) isVisible();
        S32 is_m_visible = (S32) mVisible;
        debug_line += llformat(" v %d/%d", is_visible, is_m_visible);

        AvatarOverallAppearance aoa = getOverallAppearance();
        if (aoa == AOA_NORMAL)
        {
            debug_line += " N";
        }
        else if (aoa == AOA_JELLYDOLL)
        {
            debug_line += " J";
        }
        else
        {
            debug_line += " I";
        }

        if (mMeshValid)
        {
            debug_line += "m";
        }
        else
        {
            debug_line += "-";
        }
        if (isImpostor())
        {
            debug_line += " Imp" + llformat("%d[%d]:%.1f", mUpdatePeriod, mLastImpostorUpdateReason, ((F32)(gFrameTimeSeconds-mLastImpostorUpdateFrameTime)));
        }

        addDebugText(debug_line);
}

LLViewerInventoryItem* getObjectInventoryItem(LLViewerObject *vobj, LLUUID asset_id)
{
    LLViewerInventoryItem *item = NULL;

    if (vobj)
    {
        if (vobj->getInventorySerial()<=0)
        {
            vobj->requestInventory();
    }
        item = vobj->getInventoryItemByAsset(asset_id);
    }
    return item;
}

LLViewerInventoryItem* recursiveGetObjectInventoryItem(LLViewerObject *vobj, LLUUID asset_id)
{
    LLViewerInventoryItem *item = getObjectInventoryItem(vobj, asset_id);
    if (!item)
    {
        LLViewerObject::const_child_list_t& children = vobj->getChildren();
        for (LLViewerObject::const_child_list_t::const_iterator it = children.begin();
             it != children.end(); ++it)
        {
            LLViewerObject *childp = *it;
            item = getObjectInventoryItem(childp, asset_id);
            if (item)
    {
                break;
            }
        }
    }
    return item;
}

void LLVOAvatar::updateAnimationDebugText()
{
    for (LLMotionController::motion_list_t::iterator iter = mMotionController.getActiveMotions().begin();
         iter != mMotionController.getActiveMotions().end(); ++iter)
    {
        LLMotion* motionp = *iter;
        if (motionp->getMinPixelArea() < getPixelArea())
        {
            std::string output;
            std::string motion_name = motionp->getName();
            if (motion_name.empty())
            {
                if (isControlAvatar())
                {
                    LLControlAvatar *control_av = dynamic_cast<LLControlAvatar*>(this);
                    // Try to get name from inventory of associated object
                    LLVOVolume *volp = control_av->mRootVolp;
                    LLViewerInventoryItem *item = recursiveGetObjectInventoryItem(volp,motionp->getID());
                    if (item)
                    {
                        motion_name = item->getName();
                    }
                }
            }
            if (motion_name.empty())
            {
                std::string name;
                if (gAgent.isGodlikeWithoutAdminMenuFakery() || isSelf())
                {
                    name = motionp->getID().asString();
                    LLVOAvatar::AnimSourceIterator anim_it = mAnimationSources.begin();
                    for (; anim_it != mAnimationSources.end(); ++anim_it)
                    {
                        if (anim_it->second == motionp->getID())
                        {
                            LLViewerObject* object = gObjectList.findObject(anim_it->first);
                            if (!object)
                            {
                                break;
                            }
                            if (object->isAvatar())
                            {
                                if (mMotionController.mIsSelf)
                                {
                                    // Searching inventory by asset id is really long
                                    // so just mark as inventory
                                    // Also item is likely to be named by LLPreviewAnim
                                    name += "(inventory)";
                                }
                            }
                            else
                            {
                                LLViewerInventoryItem* item = NULL;
                                if (!object->isInventoryDirty())
                                {
                                    item = object->getInventoryItemByAsset(motionp->getID());
                                }
                                if (item)
                                {
                                    name = item->getName();
                                }
                                else if (object->isAttachment())
                                {
                                    name += "(att:" + getAttachmentItemName() + ")";
                                }
                                else
                                {
                                    // in-world object, name or content unknown
                                    name += "(in-world)";
                                }
                            }
                            break;
                        }
                    }
                }
                else
                {
                    name = LLUUID::null.asString();
                }
                motion_name = name;
            }
            std::string motion_tag = "";
            if (mPlayingAnimations.find(motionp->getID()) != mPlayingAnimations.end())
            {
                motion_tag = "*";
            }
            output = llformat("%s%s - %d",
                              motion_name.c_str(),
                              motion_tag.c_str(),
                              (U32)motionp->getPriority());
            addDebugText(output);
        }
    }
}

void LLVOAvatar::updateDebugText()
{
    // Leave mDebugText uncleared here, in case a derived class has added some state first

    static LLCachedControl<bool> debug_av_appearance_message(gSavedSettings, "DebugAvatarAppearanceMessage");
    if (debug_av_appearance_message)
    {
        updateAppearanceMessageDebugText();
    }

    static LLCachedControl<bool> debug_av_composite_baked(gSavedSettings, "DebugAvatarCompositeBaked");
    if (debug_av_composite_baked)
    {
        if (!mBakedTextureDebugText.empty())
            addDebugText(mBakedTextureDebugText);
    }

    // Develop -> Avatar -> Animation Info
    if (LLVOAvatar::sShowAnimationDebug)
    {
        updateAnimationDebugText();
    }

    if (!mDebugText.size() && mText.notNull())
    {
        mText->markDead();
        mText = NULL;
    }
    else if (mDebugText.size())
    {
        setDebugText(mDebugText);
    }
    mDebugText.clear();
}

//------------------------------------------------------------------------
// updateFootstepSounds
// Factored out from updateCharacter()
// Generate footstep sounds when feet hit the ground
//------------------------------------------------------------------------
void LLVOAvatar::updateFootstepSounds()
{
    if (mIsDummy)
    {
        return;
    }

    const LLUUID AGENT_FOOTSTEP_ANIMS[] = {ANIM_AGENT_WALK, ANIM_AGENT_RUN, ANIM_AGENT_LAND};
    const S32 NUM_AGENT_FOOTSTEP_ANIMS = LL_ARRAY_SIZE(AGENT_FOOTSTEP_ANIMS);

    // The ground probes below only feed the footstep sound; skip them when not walking.
    if (!gAudiop || !isAnyAnimationSignaled(AGENT_FOOTSTEP_ANIMS, NUM_AGENT_FOOTSTEP_ANIMS))
    {
        return;
    }

    //-------------------------------------------------------------------------
    // Find the ground under each foot, these are used for a variety
    // of things that follow
    //-------------------------------------------------------------------------
    LLVector3 ankle_left_pos_agent = mFootLeftp->getWorldPosition();
    LLVector3 ankle_right_pos_agent = mFootRightp->getWorldPosition();

    LLVector3 ankle_left_ground_agent = ankle_left_pos_agent;
    LLVector3 ankle_right_ground_agent = ankle_right_pos_agent;
    LLVector3 normal;
    resolveHeightAgent(ankle_left_pos_agent, ankle_left_ground_agent, normal);
    resolveHeightAgent(ankle_right_pos_agent, ankle_right_ground_agent, normal);

    F32 leftElev = llmax(-0.2f, ankle_left_pos_agent.mV[VZ] - ankle_left_ground_agent.mV[VZ]);
    F32 rightElev = llmax(-0.2f, ankle_right_pos_agent.mV[VZ] - ankle_right_ground_agent.mV[VZ]);

    if (!isSitting())
    {
        //-------------------------------------------------------------------------
        // Figure out which foot is on ground
        //-------------------------------------------------------------------------
        if (!mInAir)
        {
            if ((leftElev < 0.0f) || (rightElev < 0.0f))
    {
                ankle_left_pos_agent = mFootLeftp->getWorldPosition();
                ankle_right_pos_agent = mFootRightp->getWorldPosition();
                leftElev = ankle_left_pos_agent.mV[VZ] - ankle_left_ground_agent.mV[VZ];
                rightElev = ankle_right_pos_agent.mV[VZ] - ankle_right_ground_agent.mV[VZ];
            }
        }
    }

    bool playSound = false;
    LLVector3 foot_pos_agent;

    bool onGroundLeft = (leftElev <= 0.05f);
    bool onGroundRight = (rightElev <= 0.05f);

    // did left foot hit the ground?
    if ( onGroundLeft && !mWasOnGroundLeft )
    {
        foot_pos_agent = ankle_left_pos_agent;
        playSound = true;
    }

    // did right foot hit the ground?
    if ( onGroundRight && !mWasOnGroundRight )
    {
        foot_pos_agent = ankle_right_pos_agent;
        playSound = true;
    }

    mWasOnGroundLeft = onGroundLeft;
    mWasOnGroundRight = onGroundRight;

    if ( playSound )
    {
        const F32 STEP_VOLUME = 0.1f;
        const LLUUID& step_sound_id = getStepSound();

        LLVector3d foot_pos_global = gAgent.getPosGlobalFromAgent(foot_pos_agent);

        if (LLViewerParcelMgr::getInstance()->canHearSound(foot_pos_global)
            && !LLMuteList::getInstance()->isMuted(getID(), LLMute::flagObjectSounds))
        {
            LLUUID step_src = gAudiop->triggerSound(step_sound_id, getID(), STEP_VOLUME, LLAudioEngine::AUDIO_TYPE_AMBIENT, foot_pos_global);
            if (step_src.notNull())
            {
                SLSoundLog::getInstance()->recordTriggered(step_sound_id, getID(), getID(), foot_pos_global, step_src, true);
            }
        }
    }
}

//------------------------------------------------------------------------
// computeUpdatePeriod()
// Factored out from updateCharacter()
// ------------------------------------------------------------------------
void LLVOAvatar::computeUpdatePeriod()
{
    // VK draws no impostors: the slow impostor update periods (32-64 frames) would
    // only make far avatars' animation choppy. Every visible avatar updates each frame.
    mUpdatePeriod = 1;
}

//------------------------------------------------------------------------
// updateOrientation()
// Factored out from updateCharacter()
// This is used by updateCharacter() to update the avatar's orientation:
// - updates mTurning state
// - updates rotation of the mRoot joint in the skeleton
// - for self, calls setControlFlags() to notify the simulator about any turns
//------------------------------------------------------------------------
void LLVOAvatar::updateOrientation(LLAgent& agent, F32 speed, F32 delta_time)
{
            LLQuaternion iQ;
            LLVector3 upDir( 0.0f, 0.0f, 1.0f );

            // Compute a forward direction vector derived from the primitive rotation
            // and the velocity vector.  When walking or jumping, don't let body deviate
            // more than 90 from the view, if necessary, flip the velocity vector.

            LLVector3 primDir;
            if (isSelf())
            {
                primDir = agent.getAtAxis() - projected_vec(agent.getAtAxis(), agent.getReferenceUpVector());
                primDir.normalize();
            }
            else
            {
                primDir = getRotation().getMatrix3().getFwdRow();
            }
            LLVector3 velDir = getVelocity();
            velDir.normalize();
            // Slipstream (#496): with SLWalkBackwardsFacingForward, moving backwards on
            // the ground keeps the body facing forward for self even without the walk
            // built-in (running backwards). Other viewers follow the walk built-in, so
            // they turn a backwards runner round as usual; the walk built-in is not
            // requested on top of "run" (#520: both locomotion cycles then blend).
            static LLCachedControl<bool> s_back_facing_forward(gSavedSettings, "SLWalkBackwardsFacingForward", true);
            const bool keep_forward = s_back_facing_forward && isSelf() && !gAgent.getFlying() &&
                                      !gAgent.getAutoPilot() && !isSitting();
            // #922: with the option off and the AO supplying the walk, a backwards
            // walker turns round to face its travel exactly like a backwards runner
            // (the built-in walk stays listed for everybody, so it must not flip us).
            const bool ao_turns_round = isSelf() && !s_back_facing_forward && SLAOEngine::instance().overridesWalk();
            if ( keep_forward || (!ao_turns_round && mSignaledAnimations.find(ANIM_AGENT_WALK) != mSignaledAnimations.end()))
            {
                F32 vpD = velDir * primDir;
                if (vpD < -0.5f)
                {
                    velDir *= -1.0f;
                }
            }
            LLVector3 fwdDir = lerp(primDir, velDir, clamp_rescale(speed, 0.5f, 2.0f, 0.0f, 1.0f));
            if (isSelf() && gAgentCamera.cameraMouselook())
            {
                // make sure fwdDir stays in same general direction as primdir
                if (gAgent.getFlying())
                {
                    fwdDir = LLViewerCamera::getInstance()->getAtAxis();
                }
                else
                {
                    LLVector3 at_axis = LLViewerCamera::getInstance()->getAtAxis();
                    LLVector3 up_vector = gAgent.getReferenceUpVector();
                    at_axis -= up_vector * (at_axis * up_vector);
                    at_axis.normalize();

                    F32 dot = fwdDir * at_axis;
                    if (dot < 0.f)
                    {
                        fwdDir -= 2.f * at_axis * dot;
                        fwdDir.normalize();
                    }
                }
            }

            LLQuaternion root_rotation = mRoot->getWorldMatrix().quaternion();
            F32 root_roll, root_pitch, root_yaw;
            root_rotation.getEulerAngles(&root_roll, &root_pitch, &root_yaw);

            // When moving very slow, the pelvis is allowed to deviate from the
    // forward direction to allow it to hold its position while the torso
            // and head turn.  Once in motion, it must conform however.
            bool self_in_mouselook = isSelf() && gAgentCamera.cameraMouselook();

            LLVector3 pelvisDir( mRoot->getWorldMatrix().getFwdRow4().mV );

            // Firestorm's AvatarRotateThresholdSlow/Fast (degrees; LL's constants are the
            // defaults): how far the body may lag the facing direction before it turns,
            // when moving slowly / fast.
            static LLCachedControl<F32> s_pelvis_rot_threshold_slow(gSavedSettings, "AvatarRotateThresholdSlow", 60.0f);
            static LLCachedControl<F32> s_pelvis_rot_threshold_fast(gSavedSettings, "AvatarRotateThresholdFast", 2.0f);

            F32 pelvis_rot_threshold = clamp_rescale(speed, 0.1f, 1.0f, s_pelvis_rot_threshold_slow, s_pelvis_rot_threshold_fast);

            if (self_in_mouselook)
            {
                pelvis_rot_threshold *= MOUSELOOK_PELVIS_FOLLOW_FACTOR;
            }
            if (areAnimationsPaused())
            {
                pelvis_rot_threshold = 360.f;   // world frozen (BD): the body never turns to follow
            }
            pelvis_rot_threshold *= DEG_TO_RAD;

            F32 angle = angle_between( pelvisDir, fwdDir );

            // The avatar's root is allowed to have a yaw that deviates widely
            // from the forward direction, but if roll or pitch are off even
            // a little bit we need to correct the rotation.
            if(root_roll < 1.f * DEG_TO_RAD
               && root_pitch < 5.f * DEG_TO_RAD)
            {
                // smaller correction vector means pelvis follows prim direction more closely
                if (!mTurning && angle > pelvis_rot_threshold*0.75f)
                {
                    mTurning = true;
                }

                // use tighter threshold when turning
                if (mTurning)
                {
                    pelvis_rot_threshold *= 0.4f;
                    // account for fps, assume that above value is for ~60fps
                    constexpr F32 default_frame_sec = 0.016f;
                    F32 prev_frame_sec = LLFrameTimer::getFrameDeltaTimeF32();
                    if (default_frame_sec > prev_frame_sec)
                    {
                        // reduce threshold since turn rate per second is constant,
                        // shorter frame means shorter turn.
                        pelvis_rot_threshold *= prev_frame_sec/default_frame_sec;
                    }
                }

                // am I done turning?
                if (angle < pelvis_rot_threshold)
                {
                    mTurning = false;
                }

                LLVector3 correction_vector = (pelvisDir - fwdDir) * clamp_rescale(angle, pelvis_rot_threshold*0.75f, pelvis_rot_threshold, 1.0f, 0.0f);
                fwdDir += correction_vector;
            }
            else
            {
                mTurning = false;
            }

            // Now compute the full world space rotation for the whole body (wQv)
            LLVector3 leftDir = upDir % fwdDir;
            leftDir.normalize();
            fwdDir = leftDir % upDir;
            LLQuaternion wQv( fwdDir, leftDir, upDir );

            if (isSelf() && mTurning)
            {
                if ((fwdDir % pelvisDir) * upDir > 0.f)
                {
                    gAgent.setControlFlags(AGENT_CONTROL_TURN_RIGHT);
                }
                else
                {
                    gAgent.setControlFlags(AGENT_CONTROL_TURN_LEFT);
                }
            }

            // Set the root rotation, but do so incrementally so that it
            // lags in time by some fixed amount.
            //F32 u = LLSmoothInterpolation::getInterpolant(PELVIS_LAG);
            F32 pelvis_lag_time = 0.f;
            if (self_in_mouselook)
            {
                pelvis_lag_time = PELVIS_LAG_MOUSELOOK;
            }
            else if (mInAir)
            {
                pelvis_lag_time = PELVIS_LAG_FLYING;
                // increase pelvis lag time when moving slowly
                pelvis_lag_time *= clamp_rescale(mSpeedAccum, 0.f, 15.f, 3.f, 1.f);
            }
            else
            {
                pelvis_lag_time = PELVIS_LAG_WALKING;
            }

    F32 u = llclamp((delta_time / pelvis_lag_time), 0.0f, 1.0f);

            mRoot->setWorldRotation( slerp(u, mRoot->getWorldRotation(), wQv) );
}

void LLVOAvatar::updateRootPositionAndRotation(LLAgent& agent, F32 speed, bool was_sit_ground_constrained)
{
    if (!(isSitting() && getParent()))
    {
        // This case includes all configurations except sitting on an
        // object, so does include ground sit.

        //--------------------------------------------------------------------
        // get timing info
        // handle initial condition case
        //--------------------------------------------------------------------
        F32 animation_time = mAnimTimer.getElapsedTimeF32();
        if (mTimeLast == 0.0f)
        {
            mTimeLast = animation_time;

            // Initially put the pelvis at slaved position/mRotation
            // SL-315
            mRoot->setWorldPosition( getPositionAgent() ); // first frame
            mRoot->setWorldRotation( getRotation() );
        }

        //--------------------------------------------------------------------
        // dont' let dT get larger than 1/5th of a second
        //--------------------------------------------------------------------
        F32 delta_time = animation_time - mTimeLast;

        delta_time = llclamp( delta_time, DELTA_TIME_MIN, DELTA_TIME_MAX );
        mTimeLast = animation_time;

        mSpeedAccum = (mSpeedAccum * 0.95f) + (speed * 0.05f);

        //--------------------------------------------------------------------
        // compute the position of the avatar's root
        //--------------------------------------------------------------------
        LLVector3d root_pos;
        LLVector3d ground_under_pelvis;

        if (isSelf())
        {
            gAgent.setPositionAgent(getRenderPosition());
        }

        root_pos = gAgent.getPosGlobalFromAgent(getRenderPosition());
        // AVATAR_HOVER by cached pointer (as #329's male param): getVisualParamWeight(id)
        // is a map lookup per avatar per frame (perf: 0.3% of the main thread).
        if (mHoverParamGen != getVisualParamGeneration())
        {
            mHoverParam = getVisualParam(AVATAR_HOVER);
            mHoverParamGen = getVisualParamGeneration();
        }
        root_pos.mdV[VZ] += mHoverParam ? mHoverParam->getWeight() : getVisualParamWeight(AVATAR_HOVER);

        // Ground under the pelvis (an object ray cast): only feeds mInAir / the step
        // surface, which drive the motions (fly / land) and the pelvis lag. A held frame
        // (#31) runs no motions: keep the last evaluation's values; the root position
        // itself never depends on it.
        if (!mVKHeldFrame)
        {
            LLVector3 normal;
            resolveHeightGlobal(root_pos, ground_under_pelvis, normal);
            F32 foot_to_ground = (F32) (root_pos.mdV[VZ] - mPelvisToFoot - ground_under_pelvis.mdV[VZ]);
            bool in_air = ((!LLWorld::getInstance()->getRegionFromPosGlobal(ground_under_pelvis)) ||
                            foot_to_ground > FOOT_GROUND_COLLISION_TOLERANCE);

            if (in_air && !mInAir)
            {
                mTimeInAir.reset();
            }
            mInAir = in_air;
        }

        // SL-402: with the ability to animate the position of joints
        // that affect the body size calculation, computed body size
        // can get stale much more easily. Simplest fix is to update
        // it frequently.
        // SL-427: this appears to be too frequent, moving to only do on animation state change.
        //computeBodySize();

        // correct for the fact that the pelvis is not necessarily the center
        // of the agent's physical representation
        root_pos.mdV[VZ] -= (0.5f * mBodySize.mV[VZ]) - mPelvisToFoot;
        if (!isSitting() && !was_sit_ground_constrained)
        {
            root_pos += LLVector3d(getHoverOffset());
            if (getOverallAppearance() == AOA_JELLYDOLL)
            {
                F32 offz = -0.5f * (getScale()[VZ] - mBodySize.mV[VZ]);
                root_pos[2] += offz;
                // if (!isSelf() && !isControlAvatar())
                // {
                //  LL_DEBUGS("Avatar") << "av " << getFullname()
                //                      << " frame " << LLFrameTimer::getFrameCount()
                //                      << " root adjust offz " << offz
                //                      << " scalez " << getScale()[VZ]
                //                      << " bsz " << mBodySize.mV[VZ]
                //                      << LL_ENDL;
                // }
            }
        }
        // if (!isSelf() && !isControlAvatar())
        // {
        //  LL_DEBUGS("Avatar") << "av " << getFullname() << " aoa " << (S32) getOverallAppearance()
        //                      << " frame " << LLFrameTimer::getFrameCount()
        //                      << " scalez " << getScale()[VZ]
        //                      << " bsz " << mBodySize.mV[VZ]
        //                      << " root pos " << root_pos[2]
        //                      << " curr rootz " << mRoot->getPosition()[2]
        //                      << " pp-z " << mPelvisp->getPosition()[2]
        //                      << " renderpos " << getRenderPosition()
        //                      << LL_ENDL;
        // }

        LLControlAvatar *cav = dynamic_cast<LLControlAvatar*>(this);
        if (cav)
        {
            // SL-1350: Moved to LLDrawable::updateXform()
            cav->matchVolumeTransform();
        }
        else
        {
            LLVector3 newPosition = gAgent.getPosAgentFromGlobal(root_pos);
            // if (!isSelf() && !isControlAvatar())
            // {
            //  LL_DEBUGS("Avatar") << "av " << getFullname()
            //                      << " frame " << LLFrameTimer::getFrameCount()
            //                      << " newPosition " << newPosition
            //                      << " renderpos " << getRenderPosition()
            //                      << LL_ENDL;
            // }
            if (newPosition != mRoot->getXform()->getWorldPosition())
            {
                mRoot->touch();
                // SL-315
                mRoot->setWorldPosition( newPosition ); // regular update
            }
        }

        //--------------------------------------------------------------------
        // Propagate viewer object rotation to root of avatar
        //--------------------------------------------------------------------
        if (!isControlAvatar() && !isAnyAnimationSignaled(AGENT_NO_ROTATE_ANIMS, NUM_AGENT_NO_ROTATE_ANIMS))
        {
            // Rotation fixups for avatars in motion.
            // Skip for animated objects.
            updateOrientation(agent, speed, delta_time);
        }
    }
    else if (mDrawable.notNull())
    {
        // Sitting on an object - mRoot is slaved to mDrawable orientation.
        LLVector3 pos = mDrawable->getPosition();
        pos += getHoverOffset() * mDrawable->getRotation();
        // SL-315
        mRoot->setPosition(pos);
        mRoot->setRotation(mDrawable->getRotation());
    }
}

//------------------------------------------------------------------------
// LLVOAvatar::computeNeedsUpdate()
//
// Most of the logic here is to figure out when to periodically update impostors.
// Non-impostors have mUpdatePeriod == 1 and will need update every frame.
//------------------------------------------------------------------------
bool LLVOAvatar::computeNeedsUpdate()
{
    const F32 MAX_IMPOSTOR_INTERVAL = 4.0f;
    computeUpdatePeriod();

    bool needs_update_by_frame_count = ((LLDrawable::getCurrentFrame()+mID.mData[0])%mUpdatePeriod == 0);

    bool needs_update_by_max_time = ((gFrameTimeSeconds-mLastImpostorUpdateFrameTime)> MAX_IMPOSTOR_INTERVAL);
    bool needs_update = needs_update_by_frame_count || needs_update_by_max_time;

    if (needs_update && !isSelf())
    {
        if (needs_update_by_max_time)
        {
            mNeedsImpostorUpdate = true;
            mLastImpostorUpdateReason = 11;
        }
        else
        {
            //mNeedsImpostorUpdate = true;
            //mLastImpostorUpdateReason = 10;
        }
    }
    return needs_update;
}

// updateCharacter()
//
// This is called for all avatars, so there are 4 possible situations:
//
// 1) Avatar is your own. In this case the class is LLVOAvatarSelf,
// isSelf() is true, and agent specifies the corresponding agent
// information for you. In all the other cases, agent is irrelevant
// and it would be less confusing if it were null or something.
//
// 2) Avatar is controlled by another resident. Class is LLVOAvatar,
// and isSelf() is false.
//
// 3) Avatar is the controller for an animated object. Class is
// LLControlAvatar and mIsDummy is true. Avatar is a purely
// viewer-side entity with no representation on the simulator.
//
// 4) Avatar is a UI avatar used in some areas of the UI, such as when
// previewing uploaded animations. Class is LLUIAvatar, and mIsDummy
// is true. Avatar is purely viewer-side with no representation on the
// simulator.
//
//------------------------------------------------------------------------
bool LLVOAvatar::updateCharacter(LLAgent &agent)
{
    mVKHeldFrame = false;
    updateDebugText();

    // #631 diagnostic (RenderVKAnimStateDebug): every avatar's animation state every 15 s,
    // staggered by id so a crowd spreads its lines. Before the early returns: an avatar
    // leaving here early is exactly one whose state must be seen (the line shows the
    // previous frame's evaluation).
    static LLCachedControl<bool> s_state_dbg(gSavedSettings, "RenderVKAnimStateDebug", false);
    if (s_state_dbg && !isSelf() && !isUIAvatar())
    {
        constexpr F64 kStatePeriod = 15.0;
        if (mAnimStateDbgNext <= 0.0)
        {
            mAnimStateDbgNext = gFrameTimeSeconds + kStatePeriod * ((F64)mID.mData[0] / 256.0);
        }
        else if (gFrameTimeSeconds >= mAnimStateDbgNext)
        {
            mAnimStateDbgNext = gFrameTimeSeconds + kStatePeriod;
            logAnimState("periodic");
        }
    }

    if (!mIsBuilt)
    {
        return false;
    }

    // VK GPU animation: decide whether this frame's pose is GPU-evaluated (the motion
    // controller then exports its blend instead of sampling + applying it).
    LLCleanAnimStore::instance().beginAvatar(this);

    bool visible = isVisible();

    // For fading out the names above heads, only let the timer
    // run if we're visible.
    if (mDrawable.notNull() && !visible)
    {
        mTimeVisible.reset();
    }
    if (visible)
    {
        mVKLastVisibleTime = gFrameTimeSeconds;
    }
    // #631: Refresh Animations evaluates the pose on its next few frames whatever the
    // visibility stamp or the animation rate say -- an avatar drawn but stamped hidden
    // (HIDDEN_UPDATE never poses) would otherwise keep its rest pose through a refresh.
    const bool force_eval = mVKForceAnimEval > 0 && !isSelf() && !isUIAvatar();
    if (mVKForceAnimEval > 0)
    {
        --mVKForceAnimEval;
    }

    //--------------------------------------------------------------------
    // The rest should only be done occasionally for far away avatars.
    // Set mUpdatePeriod and visible based on distance and other criteria,
    // and flag for impostor update if needed.
    //--------------------------------------------------------------------
    bool needs_update = computeNeedsUpdate();

    //--------------------------------------------------------------------
    // Early out if does not need update and not self
    // don't early out for your own avatar, as we rely on your animations playing reliably
    // for example, the "turn around" animation when entering customize avatar needs to trigger
    // even when your avatar is offscreen
    //--------------------------------------------------------------------
    if (!needs_update && !isSelf())
    {
        updateMotions(LLCharacter::HIDDEN_UPDATE);
        return false;
    }

    //--------------------------------------------------------------------
    // Handle transitions between regular rendering, jellydoll, or invisible.
    // Can trigger skeleton reset or animation changes
    //--------------------------------------------------------------------
    updateOverallAppearance();

    //--------------------------------------------------------------------
    // PLAYING-WITHOUT-MOTION REPAIR. mPlayingAnimations only records an animation
    // once startMotion succeeds, but the motion controller can later delete that
    // motion (loading races, excess-motion purges) without telling the avatar. The
    // animation then stays "playing" with nothing behind it, and since signaled ==
    // playing nothing ever restarts it -- a deformer silently lost at login until
    // re-attached. Enforce the invariant: a playing animation with no motion object,
    // or a looping one whose motion sits inactive, gets restarted.
    //--------------------------------------------------------------------
    if (gFrameTimeSeconds >= mNextAnimHealTime && getOverallAppearance() == AOA_NORMAL)
    {
        mNextAnimHealTime = gFrameTimeSeconds + 1.0;
        // Backoff per animation: retry after 2, 4, 8, 16, 32 s, then give up until the
        // animation is signaled again (a motion that never loads -- asset forbidden or
        // missing -- is deleted right after every start, so it would otherwise re-fetch
        // from the asset CDN every second).
        constexpr U8 kMaxHealAttempts = 5;
        auto may_retry = [this](const LLUUID& id) -> AnimHealRetry*
        {
            AnimHealRetry& r = mAnimHealRetry[id];
            if (r.attempts >= kMaxHealAttempts || gFrameTimeSeconds < r.next) return nullptr;
            ++r.attempts;
            r.next = gFrameTimeSeconds + F64(1u << r.attempts);
            return &r;
        };
        // Client AO (#668): built-ins its replacement covers are kept off our
        // avatar on purpose; they are playing with no motion behind them.
        const bool ao_self = isSelf() && SLAOEngine::instanceExists();
        for (const auto& kv : mPlayingAnimations)
        {
            if (ao_self && SLAOEngine::instance().suppressesStockMotion(kv.first))
            {
                continue;
            }
            const EPlayingAnimState state = getPlayingAnimState(kv.first);
            if (state == PLAYING_ANIM_NO_MOTION)
            {
                if (AnimHealRetry* r = may_retry(kv.first))
                {
                    LL_INFOS("Motion") << avString() << "restarting playing animation " << kv.first
                                       << " whose motion was deleted (attempt " << (S32)r->attempts << "/"
                                       << (S32)kMaxHealAttempts << ")" << LL_ENDL;
                    restartAnimation(kv.first, false);
                }
            }
            // Second form: the motion exists and loaded, but was stopped (or never
            // activated) while still signaled. Only LOOPING motions are restarted --
            // an inactive looping motion that is still playing is always a desync,
            // while a finished one-shot legitimately sits inactive.
            else if (state == PLAYING_ANIM_INACTIVE_LOOP)
            {
                if (AnimHealRetry* r = may_retry(kv.first))
                {
                    LL_INFOS("Motion") << avString() << "restarting playing looped animation " << kv.first
                                       << " whose motion was inactive (attempt " << (S32)r->attempts << "/"
                                       << (S32)kMaxHealAttempts << ")" << LL_ENDL;
                    restartAnimation(kv.first, false);
                }
            }
            else if (state == PLAYING_ANIM_ACTIVE)
            {
                mAnimHealRetry.erase(kv.first);   // healthy again: a later loss gets fresh attempts
            }
        }
        // Third form (#631): SIGNALED BUT NOT PLAYING. An animation enters
        // mPlayingAnimations only when its start succeeds. A start that failed outright
        // (createMotion -> LLKeyframeMotion::onInitialize STATUS_FAILURE: a truncated or
        // undecodable file in the local asset cache fails every start the same way, for
        // every avatar playing that animation) was retried only by the next
        // AvatarAnimation message -- which an AO stand that loops for hours never sends --
        // and neither this repair nor Refresh Animations looked outside the playing set:
        // the avatar stayed in its rest pose for good. Same gap after an animesh leaves
        // the jellydoll state: processAnimationStateChanges shelved its animations, and
        // setOverallAppearanceNormal returns early for control avatars, so nothing
        // started them again. Start it again with the same backoff; with no motion
        // behind it the locally cached asset is dropped and the data re-fetched. (Not
        // self: our own animation state also goes through the agent and the client AO.)
        for (const auto& kv : mSignaledAnimations)
        {
            if (isSelf() || mPlayingAnimations.count(kv.first))
            {
                continue;
            }
            if (AnimHealRetry* r = may_retry(kv.first))
            {
                const bool refetch = mMotionController.findMotion(remapMotionID(kv.first)) == nullptr;
                const bool started = restartAnimation(kv.first, refetch);
                LL_INFOS("Motion") << avString() << "starting signaled animation " << kv.first
                                   << " that never started playing" << (refetch ? " (re-fetching its data)" : "")
                                   << (started ? "" : ": start failed again") << " (attempt " << (S32)r->attempts
                                   << "/" << (S32)kMaxHealAttempts << ")" << LL_ENDL;
                if (started)
                {
                    mPlayingAnimations[kv.first] = kv.second;
                }
            }
        }
        // Animations neither playing nor signaled drop their backoff, so a fresh start
        // retries again.
        for (auto it = mAnimHealRetry.begin(); it != mAnimHealRetry.end(); )
            it = (mPlayingAnimations.count(it->first) || mSignaledAnimations.count(it->first))
                ? std::next(it) : mAnimHealRetry.erase(it);
    }

    //--------------------------------------------------------------------
    // Update sitting state based on parent and active animation info.
    //--------------------------------------------------------------------
    if (getParent() && !isSitting())
    {
        sitOnObject((LLViewerObject*)getParent());
    }
    else if (!getParent() && isSitting() && !isMotionActive(ANIM_AGENT_SIT_GROUND_CONSTRAINED))
    {
        // If we are starting up, motion might be loading
        LLMotion *motionp = mMotionController.findMotion(ANIM_AGENT_SIT_GROUND_CONSTRAINED);
        if (!motionp || !mMotionController.isMotionLoading(motionp))
        {
            getOffObject();
        }
    }

    //--------------------------------------------------------------------
    // create local variables in world coords for region position values
    //--------------------------------------------------------------------
    LLVector3 xyVel = getVelocity();
    xyVel.mV[VZ] = 0.0f;
    F32 speed = xyVel.length();
    // remembering the value here prevents a display glitch if the
    // animation gets toggled during this update.
    bool was_sit_ground_constrained = isMotionActive(ANIM_AGENT_SIT_GROUND_CONSTRAINED);

    // Animation-rate decision for this frame, made ONCE (vkSkipAnimEval -> vkRateSkipAnim
    // advances the avatar's time bucket). A held frame of a reduced GPU-animated avatar (#31) keeps
    // last evaluation's pose relative to the root: the pose-derived CPU work below is
    // skipped with the motion update. Never self (HIDDEN/FORCE paths unchanged).
    //
    // HIDDEN (off-screen) avatars never pose (HIDDEN_UPDATE only does the motion
    // controller's bookkeeping), so their skeleton relative to the root cannot change:
    // the pose-derived work (pelvis ground probe, head offset, footstep ray casts) is
    // pure waste on them. This was the held-frame gap (#31 follow-up): the skip used to
    // require `visible`, so every off-screen reduced avatar -- most of the reduced tier
    // in a crowd -- paid a full non-held frame each frame (avHeldFrame ~1/3 of the
    // reduced count). Under VK a hidden non-self avatar now holds its frame and runs the
    // HIDDEN_UPDATE bookkeeping (purge, loading motions, stopped-motion deactivation) at
    // RenderVKHiddenAvatarFPS; the root still moves every frame (shadows, RT, attached
    // lights / particles / sounds follow it).
    static LLCachedControl<U32> s_hidden_fps(gSavedSettings, "RenderVKHiddenAvatarFPS", 5);
    const bool hidden = !visible && !isSelf() && !force_eval;
    const bool vk_hidden = hidden && mSpecialRenderMode != 1 && !isUIAvatar();
    bool hidden_skip = false;
    if (vk_hidden)
    {
        if (!mVKWasHidden)
        {
            mVKWasHidden = true;
            mVKAnimBucket = -1;   // the bucket now runs at the hidden rate
            mVKAnimRate = 0;
        }
        hidden_skip = (U32)s_hidden_fps != 0u && vkRateSkipAnim((U32)s_hidden_fps);
        mVKHeldFrame = true;
        LLVKHitch::count(hidden_skip ? "avHiddenSkip" : "avHiddenEval");
    }
    else if (mVKWasHidden)
    {
        // Back on screen: evaluate this frame (the reduced / distance rate starts a
        // fresh bucket), never show the stale pose held while off screen.
        mVKWasHidden = false;
        mVKAnimBucket = -1;
        mVKAnimRate = 0;
    }
    const bool anim_skip = !hidden && !force_eval && mSpecialRenderMode != 1 && vkSkipAnimEval();
    if (!vk_hidden)
    {
        mVKHeldFrame = anim_skip && (mVKReduced || mVKDistThrottled) && !isSelf() && LLCleanAnimStore::instance().gpuDriven(this);
    }
    if (mVKHeldFrame) LLVKHitch::count("avHeldFrame");
    // #725 debug aid (RenderVKAnimRateDebug): rate transitions + hover text
    static LLCachedControl<bool> s_rate_dbg(gSavedSettings, "RenderVKAnimRateDebug", false);
    if (s_rate_dbg && !isSelf() && !isUIAvatar() && mSpecialRenderMode != 1)
    {
        if (vk_hidden) mVKAnimReason = VKAR_HIDDEN;
        U32 fps = 0u;
        if (vk_hidden) fps = (U32)s_hidden_fps;
        else if (mVKAnimReason == VKAR_PX || mVKAnimReason == VKAR_DIST || mVKAnimReason == VKAR_REDUCED) fps = mVKAnimRate;
        vkAnimRateDebug(fps);
    }

    //--------------------------------------------------------------------
    // This does a bunch of state updating, including figuring out
    // whether av is in the air, setting mRoot position and rotation
    // In some cases, calls updateOrientation() for a lot of the
    // work
    // --------------------------------------------------------------------
    updateRootPositionAndRotation(agent, speed, was_sit_ground_constrained);

    //-------------------------------------------------------------------------
    // Update character motions
    //-------------------------------------------------------------------------
    // store data relevant to motions
    mSpeed = speed;

    // update animations
    if (hidden) // NOTE: never do a "hidden update" for self avatar as it interrupts controller processing
    {
        if (!hidden_skip)
        {
            updateMotions(LLCharacter::HIDDEN_UPDATE);
        }
    }
    else if (mSpecialRenderMode == 1) // Animation Preview
    {
        mVKLastAnimEvalTime = gFrameTimeSeconds;
        updateMotions(LLCharacter::FORCE_UPDATE);
        endMorphEvaluation();
    }
    else if (anim_skip)
    {
        // Reduced-rate avatar: keep last frame's joint pose. No motion-controller call
        // at all (a HIDDEN_UPDATE would freeze animation time); the next evaluation
        // advances by the full elapsed time. Root motion + world matrices below still
        // update every frame, so the avatar itself moves smoothly.
        LLVKHitch::count("avAnimSkip");
    }
    else
    {
        // Might be better to do HIDDEN_UPDATE if cloud
        LLVKHitch::count("avAnimEval");
        if (force_eval)
        {
            LLVKHitch::count("avAnimForcedEval");   // #631
        }
        mVKLastAnimEvalTime = gFrameTimeSeconds;
        // #257 A/B: whole motion update of GPU-evaluated avatars (onUpdate + export),
        // microseconds per frame -- compare RenderVKAnimIncremental on vs off.
        const bool time_motion = mGpuAnimMode == 2;
        const auto motion_t0 = time_motion ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
        updateMotions(LLCharacter::NORMAL_UPDATE);
        if (time_motion)
        {
            LLVKHitch::count("animMotionUs", (unsigned)std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - motion_t0).count());
        }
        endMorphEvaluation();
    }

    // Special handling for sitting on ground.
    if (!getParent() && (isSitting() || was_sit_ground_constrained))
    {

        F32 off_z = (F32)LLVector3d(getHoverOffset()).mdV[VZ];
        if (off_z != 0.0f)
        {
            LLVector3 pos = mRoot->getWorldPosition();
            pos.mV[VZ] += off_z;
            mRoot->touch();
            // SL-315
            mRoot->setWorldPosition(pos);
        }
    }

    // Held frame (#31): the head offset (eye joints relative to the root, smoothed) and
    // the feet relative to the ground under them only change with the pose -- both run
    // on the evaluated frames (RenderVKReducedAvatarAnimFPS). Footstep sounds trigger on
    // those frames too (their 2 object ray casts were most of a held frame's cost).
    if (!mVKHeldFrame)
    {
        // update head position
        updateHeadOffset();

        // Generate footstep sounds when feet hit the ground
        updateFootstepSounds();
    }

    // Update child joints as needed. A GPU-animated avatar's world matrices come from
    // clean_anim.comp; the CPU's joints (fed back from the GPU pose) are recomputed
    // lazily, only where something reads them.
    if (!LLCleanAnimStore::instance().gpuDriven(this))
    {
        mRoot->updateWorldMatrixChildren();
    }
    else
    {
        // the always-read joints (attachment points' drawables read their cached world
        // transform directly; nametag reads the head's) against this frame's root
        LLCleanAnimStore::instance().refreshWorld(this);
    }

    return visible;
}

//-----------------------------------------------------------------------------
// vkOwnAnimesh() -- an animesh (control avatar) worn by our own avatar. Like self it
// never rate-limits. (No animesh joins the reduced tier or takes a rank: vkUpdateReducedTier.)
//-----------------------------------------------------------------------------
bool LLVOAvatar::vkOwnAnimesh() const
{
    if (!isControlAvatar())
    {
        return false;
    }
    const LLVOAvatar* wearer = static_cast<const LLControlAvatar*>(this)->getAttachedAvatar();
    return wearer && wearer->isSelf();
}

//-----------------------------------------------------------------------------
// vkAnimRateDebug() -- RenderVKAnimRateDebug (#725): log this avatar's animation-rate
// transitions (rate, reason, the on-screen height / camera distance / pixel area / rank
// the decision used, reduced tier) at most once a second per avatar -- a change inside
// that second is logged when it expires, if it still holds -- and show the current
// rate in the avatar's hover text. fps 0 = every frame.
//-----------------------------------------------------------------------------
void LLVOAvatar::vkAnimRateDebug(U32 fps)
{
    static const char* const kReason[] = { "full", "near", "large", "px", "dist", "reduced", "hidden", "exempt" };
    const char* reason = mVKAnimReason < LL_ARRAY_SIZE(kReason) ? kReason[mVKAnimReason] : "?";
    const std::string rate = fps ? llformat("%u fps", fps) : std::string("every frame");
    const F64 now = LLFrameTimer::getElapsedSeconds();
    if ((fps != mVKAnimDbgFps || mVKAnimReason != mVKAnimDbgReason || mVKReduced != mVKAnimDbgReduced)
        && now - mVKAnimDbgTime >= 1.0)
    {
        LL_INFOS("VKAnimRate") << avString() << (isControlAvatar() ? "(animesh) " : "") << "anim rate "
                               << (mVKAnimDbgReason == 0xFF ? std::string("-") : (mVKAnimDbgFps ? llformat("%u fps", mVKAnimDbgFps) : std::string("every frame")))
                               << " -> " << rate << " (" << reason << ") px " << llformat("%.0f", mVKAnimPx)
                               << " dist " << llformat("%.1f", mVKAnimDist) << " m pxarea "
                               << llformat("%.0f", getPixelArea()) << " rank " << mVisibilityRank
                               << (mVKReduced ? " REDUCED tier" : " full tier") << " (max fully rendered "
                               << sMaxNonImpostors << ")" << LL_ENDL;
        mVKAnimDbgFps = fps;
        mVKAnimDbgReason = mVKAnimReason;
        mVKAnimDbgReduced = mVKReduced;
        mVKAnimDbgTime = now;
    }
    addDebugText(llformat("anim %s (%s) %.0fpx %.1fm rank %u%s", rate.c_str(), reason, mVKAnimPx, mVKAnimDist,
                          mVisibilityRank, mVKReduced ? " R" : ""));
}

//-----------------------------------------------------------------------------
// vkLadderAnimRate() -- SCREEN-SIZE / DISTANCE RATE LADDER (GPU animation; #725).
// This frame's animation rate (fps, 0 = every frame) from live inputs only: the
// avatar's render position, the camera origin and its CURRENT field of view. Nothing
// is cached on a camera change -- an avatar that walks closer, sits, loads its shape
// (mBodySize) or is re-rooted is re-rated on its next frame.
//  - within RenderVKAvatarAnimScreenMinM of the camera: every frame (VKAR_NEAR);
//  - on-screen height (mBodySize Z, pixels): every frame at >= RenderVKAvatarAnimFullPx
//    (VKAR_LARGE), 30 FPS below it, 20 FPS below half, 15 FPS below a quarter
//    (RenderVKAvatarAnimScreenLOD; 10% hysteresis so an avatar on a boundary doesn't
//    flip rates -- each flip restarts its bucket);
//  - distance (RenderVKAvatarDistanceAnimRate): 30 FPS beyond RenderVKAvatarAnimNearM,
//    15 FPS beyond RenderVKAvatarAnimFarM.
// The slower of the screen and distance rates wins. The size rung (and its hysteresis
// state) is tracked even with the screen ladder off: the reduced tier reads it
// (mVKAnimLarge) to keep a near / large avatar at full rate.
//-----------------------------------------------------------------------------
U32 LLVOAvatar::vkLadderAnimRate()
{
    static LLCachedControl<F32>  s_full_px(gSavedSettings, "RenderVKAvatarAnimFullPx", 160.f);
    static LLCachedControl<bool> s_dist(gSavedSettings, "RenderVKAvatarDistanceAnimRate", true);
    static LLCachedControl<F32>  s_near(gSavedSettings, "RenderVKAvatarAnimNearM", 50.f);
    static LLCachedControl<F32>  s_far(gSavedSettings, "RenderVKAvatarAnimFarM", 100.f);
    static LLCachedControl<bool> s_screen(gSavedSettings, "RenderVKAvatarAnimScreenLOD", true);
    static LLCachedControl<F32>  s_screen_min(gSavedSettings, "RenderVKAvatarAnimScreenMinM", 10.f);

    const LLViewerCamera* cam = LLViewerCamera::getInstance();
    const F32 d2 = dist_vec_squared(getRenderPosition(), cam->getOrigin());
    const F32 dist = sqrtf(d2);
    // pixels per metre at 1 m with the CURRENT field of view (getPixelMeterRatio uses
    // the default one: a zoomed-in view would rate a large avatar as small)
    const F32 half_tan = tanf(llclamp(cam->getView(), 0.01f, F_PI - 0.01f) * 0.5f);
    const F32 ppm = half_tan > 0.f ? (F32)cam->getViewHeightInPixels() / (2.f * half_tan) : 0.f;
    const F32 px = ppm <= 0.f ? 0.f : (dist > 0.01f ? llmax(mBodySize.mV[VZ], 1.f) * ppm / dist : 1e6f);
    mVKAnimPx = px;
    mVKAnimDist = dist;

    const F32 minm = llmax((F32)s_screen_min, 0.f);
    if (d2 < minm * minm || ppm <= 0.f)
    {
        // near the camera (or no view to measure against): every frame
        mVKAnimPxRate = 0u;
        mVKAnimLarge = true;
        mVKAnimReason = VKAR_NEAR;
        return 0u;
    }

    const F32 full = llmax((F32)s_full_px, 1.f);
    auto ladder = [full](F32 p) -> U32
    {
        if (p >= full) return 0u;
        if (p >= full * 0.5f) return 30u;
        if (p >= full * 0.25f) return 20u;
        return 15u;
    };
    // rank: 0 (every frame) is the fastest
    auto faster = [](U32 a, U32 b) { return a == 0u ? b != 0u : (b != 0u && a > b); };
    U32 pfps = ladder(px);
    // hysteresis: keep the current screen rate while it lies between the rates 10%
    // above and below this frame's size
    if (mVKAnimPxRate != pfps)
    {
        const U32 hi = ladder(px * 1.1f), lo = ladder(px / 1.1f);
        const U32 cur = mVKAnimPxRate;
        if (cur != 0xFFFFFFFFu && !faster(cur, hi) && !faster(lo, cur)) pfps = cur;
    }
    mVKAnimPxRate = pfps;
    mVKAnimLarge = pfps == 0u;

    U32 fps = 0u;
    mVKAnimReason = mVKAnimLarge ? VKAR_LARGE : VKAR_FULL;
    if (s_dist)
    {
        const F32 nearm = llmax((F32)s_near, 1.f), farm = llmax((F32)s_far, nearm);
        if (d2 >= farm * farm) fps = 15u;
        else if (d2 >= nearm * nearm) fps = 30u;
        if (fps) mVKAnimReason = VKAR_DIST;
    }
    if (s_screen && pfps != 0u && (fps == 0u || pfps < fps))
    {
        fps = pfps;
        mVKAnimReason = VKAR_PX;
    }
    return fps;
}

//-----------------------------------------------------------------------------
// vkSkipAnimEval()
// ANIMATION UPDATE-RATE LOD (Vulkan; LL's impostor update periods are off there
// because VK draws no impostors). Motion evaluation -- keyframe curves, pose
// blending, joint-state application -- is the dominant per-avatar CPU cost in a
// crowd, and an avatar a few dozen pixels tall doesn't need it at full frame rate.
// Evaluate every frame at >= RenderVKAvatarAnimFullPx on-screen height, every 2nd
// frame at half that, every 3rd at a quarter, every 4th below. Staggered per avatar
// so the work spreads evenly across frames. Never for self, anything near the
// camera, our own animesh, or avatars mid appearance change.
//-----------------------------------------------------------------------------
bool LLVOAvatar::vkSkipAnimEval()
{
    static LLCachedControl<bool> s_on(gSavedSettings, "RenderVKAvatarAnimLOD", true);
    static LLCachedControl<F32>  s_full_px(gSavedSettings, "RenderVKAvatarAnimFullPx", 160.f);
    mVKDistThrottled = false;
    const U8 prev_reason = mVKAnimReason;
    mVKAnimReason = VKAR_EXEMPT;
    // Reduced tier (#31): reduced-rate animation on the GPU path too (never on a
    // GPU-vs-CPU compare frame, mode 1, whose CPU pose must be LL's own).
    //
    // #725: the tier is a RANK cut (pixel area vs RenderAvatarMaxNonImpostors), so in a
    // crowd an avatar 10-15 m away can rank past it -- and the tier's fixed
    // RenderVKReducedAvatarAnimFPS (5) then made it visibly choppy next to full-rate
    // neighbours. The tier's rate is now one more input to the screen-size ladder:
    // near / large-on-screen avatars (the ladder's full rung) animate every frame, the
    // mid band runs the ladder's rate, and the fixed reduced rate applies only below
    // RenderVKReducedAvatarAnimMaxPx on-screen height. (Shadows and LOD stay reduced.)
    if (mVKReduced && !mNeedsAnimUpdate && mGpuAnimMode != 1)
    {
        static LLCachedControl<U32> s_fps(gSavedSettings, "RenderVKReducedAvatarAnimFPS", 5);
        static LLCachedControl<F32> s_max_px(gSavedSettings, "RenderVKReducedAvatarAnimMaxPx", 80.f);
        U32 fps = vkLadderAnimRate();
        // 10% hysteresis on the size cut, as on the ladder's rungs
        const F32 cut = llmax((F32)s_max_px, 0.f) * (prev_reason == VKAR_REDUCED ? 1.1f : 1.f);
        const U32 rfps = (U32)s_fps;
        if (!mVKAnimLarge && rfps != 0u && mVKAnimPx < cut && (fps == 0u || rfps < fps))
        {
            fps = rfps;
            mVKAnimReason = VKAR_REDUCED;
        }
        if (fps == 0u)
        {
            mVKAnimRate = 0;
            LLVKHitch::count("avReducedAnimFull");
            return false;
        }
        if (fps != mVKAnimRate) { mVKAnimRate = fps; mVKAnimBucket = -1; }
        if (vkRateSkipAnim(fps))
        {
            LLVKHitch::count("avReducedAnimSkip");
            return true;
        }
        LLVKHitch::count("avReducedAnimEval");
        return false;
    }
    if (!s_on || isSelf() || isUIAvatar() || mNeedsAnimUpdate)
    {
        return false;
    }
    // GPU animation: full-detail avatars evaluate every frame, except by DISTANCE from
    // the camera (RenderVKAvatarDistanceAnimRate) and on-screen SIZE
    // (RenderVKAvatarAnimScreenLOD) -- vkLadderAnimRate --, staggered per avatar like
    // the reduced tier. A held frame keeps the pose relative to the root (the root still
    // moves every frame) and lets the RT refit gate skip the avatar when it stands
    // still. Never on a GPU-vs-CPU compare frame, never your own animesh.
    if (LLCleanAnimStore::enabled())
    {
        static LLCachedControl<bool> s_dist(gSavedSettings, "RenderVKAvatarDistanceAnimRate", true);
        static LLCachedControl<bool> s_screen(gSavedSettings, "RenderVKAvatarAnimScreenLOD", true);
        if (mGpuAnimMode == 1 || (!s_dist && !s_screen) || vkOwnAnimesh()) return false;
        const U32 fps = vkLadderAnimRate();
        if (fps == 0) { mVKAnimRate = 0; return false; }
        if (fps != mVKAnimRate) { mVKAnimRate = fps; mVKAnimBucket = -1; }
        mVKDistThrottled = true;
        const bool px_rate = mVKAnimReason == VKAR_PX;
        if (vkRateSkipAnim(fps))
        {
            LLVKHitch::count(px_rate ? "avPxAnimSkip" : "avDistAnimSkip");
            return true;
        }
        LLVKHitch::count(px_rate ? "avPxAnimEval" : "avDistAnimEval");
        return false;
    }
    if (isControlAvatar())
    {
        const LLVOAvatar* wearer = static_cast<LLControlAvatar*>(this)->getAttachedAvatar();
        if (wearer && wearer->isSelf())
        {
            return false;
        }
    }
    const LLViewerCamera* cam = LLViewerCamera::getInstance();
    const F32 dist = dist_vec(getRenderPosition(), cam->getOrigin());
    if (dist < 10.f)
    {
        return false;
    }
    F32 px_per_m = cam->getPixelMeterRatio();
    if (px_per_m <= 0.f)
    {
        return false;
    }
    const F32 height = llmax(mBodySize.mV[VZ], 1.f);
    const F32 px = height * px_per_m / dist;
    const F32 full = llmax((F32)s_full_px, 1.f);
    U32 period = 1;
    if (px < full * 0.25f)     period = 4;
    else if (px < full * 0.5f) period = 3;
    else if (px < full)        period = 2;
    if (period == 1)
    {
        return false;
    }
    return ((gFrameCount + (U32)mID.mData[0]) % period) != 0;
}

// Fixed-rate animation evaluation: true on frames inside the current 1/fps time bucket
// (already evaluated), phase-shifted per avatar UUID so avatars spread across frames
// instead of all evaluating on the same one. Between updates nothing is evaluated: no
// motion update, no blend export (the GPU pose blend then has no states -- the local
// pose is held and only re-rooted), so the joint table only moves with the root. The
// prev-joint copy (recordPrevJoints) makes held frames' motion vectors the root motion
// alone, and an update frame's the real pose delta.
bool LLVOAvatar::vkRateSkipAnim(U32 fps)
{
    const F64 period = 1.0 / (F64)fps;
    const F64 phase = ((F64)mID.mData[0] + (F64)mID.mData[1] / 256.0) / 256.0 * period;
    const S64 bucket = (S64)floor((gFrameTimeSeconds + phase) / period);
    if (bucket == mVKAnimBucket)
    {
        return true;
    }
    mVKAnimBucket = bucket;
    return false;
}

//-----------------------------------------------------------------------------
// updateHeadOffset()
//-----------------------------------------------------------------------------
void LLVOAvatar::updateHeadOffset()
{
    // since we only care about Z, just grab one of the eyes
    LLVector3 midEyePt = mEyeLeftp->getWorldPosition();
    midEyePt -= mDrawable.notNull() ? mDrawable->getWorldPosition() : mRoot->getWorldPosition();
    midEyePt.mV[VZ] = llmax(-mPelvisToFoot + LLViewerCamera::getInstance()->getNear(), midEyePt.mV[VZ]);

    if (mDrawable.notNull())
    {
        midEyePt = midEyePt * ~mDrawable->getWorldRotation();
    }
    if (isSitting())
    {
        mHeadOffset = midEyePt;
    }
    else
    {
        F32 u = llmax(0.f, HEAD_MOVEMENT_AVG_TIME - (1.f / gFPSClamped));
        mHeadOffset = lerp(midEyePt, mHeadOffset,  u);
    }
}

void LLVOAvatar::debugBodySize() const
{
    LLVector3 pelvis_scale = mPelvisp->getScale();

    // some of the joints have not been cached
    LLVector3 skull = mSkullp->getPosition();
    LL_DEBUGS("Avatar") << "skull pos " << skull << LL_ENDL;
    //LLVector3 skull_scale = mSkullp->getScale();

    LLVector3 neck = mNeckp->getPosition();
    LLVector3 neck_scale = mNeckp->getScale();
    LL_DEBUGS("Avatar") << "neck pos " << neck << " neck_scale " << neck_scale << LL_ENDL;

    LLVector3 chest = mChestp->getPosition();
    LLVector3 chest_scale = mChestp->getScale();
    LL_DEBUGS("Avatar") << "chest pos " << chest << " chest_scale " << chest_scale << LL_ENDL;

    // the rest of the joints have been cached
    LLVector3 head = mHeadp->getPosition();
    LLVector3 head_scale = mHeadp->getScale();
    LL_DEBUGS("Avatar") << "head pos " << head << " head_scale " << head_scale << LL_ENDL;

    LLVector3 torso = mTorsop->getPosition();
    LLVector3 torso_scale = mTorsop->getScale();
    LL_DEBUGS("Avatar") << "torso pos " << torso << " torso_scale " << torso_scale << LL_ENDL;

    LLVector3 hip = mHipLeftp->getPosition();
    LLVector3 hip_scale = mHipLeftp->getScale();
    LL_DEBUGS("Avatar") << "hip pos " << hip << " hip_scale " << hip_scale << LL_ENDL;

    LLVector3 knee = mKneeLeftp->getPosition();
    LLVector3 knee_scale = mKneeLeftp->getScale();
    LL_DEBUGS("Avatar") << "knee pos " << knee << " knee_scale " << knee_scale << LL_ENDL;

    LLVector3 ankle = mAnkleLeftp->getPosition();
    LLVector3 ankle_scale = mAnkleLeftp->getScale();
    LL_DEBUGS("Avatar") << "ankle pos " << ankle << " ankle_scale " << ankle_scale << LL_ENDL;

    LLVector3 foot  = mFootLeftp->getPosition();
    LL_DEBUGS("Avatar") << "foot pos " << foot << LL_ENDL;

    F32 new_offset = (const_cast<LLVOAvatar*>(this))->getVisualParamWeight(AVATAR_HOVER);
    LL_DEBUGS("Avatar") << "new_offset " << new_offset << LL_ENDL;

    F32 new_pelvis_to_foot = hip.mV[VZ] * pelvis_scale.mV[VZ] -
        knee.mV[VZ] * hip_scale.mV[VZ] -
        ankle.mV[VZ] * knee_scale.mV[VZ] -
        foot.mV[VZ] * ankle_scale.mV[VZ];
    LL_DEBUGS("Avatar") << "new_pelvis_to_foot " << new_pelvis_to_foot << LL_ENDL;

    LLVector3 new_body_size;
    new_body_size.mV[VZ] = new_pelvis_to_foot +
                       // the sqrt(2) correction below is an approximate
                       // correction to get to the top of the head
                       F_SQRT2 * (skull.mV[VZ] * head_scale.mV[VZ]) +
                       head.mV[VZ] * neck_scale.mV[VZ] +
                       neck.mV[VZ] * chest_scale.mV[VZ] +
                       chest.mV[VZ] * torso_scale.mV[VZ] +
                       torso.mV[VZ] * pelvis_scale.mV[VZ];

    // TODO -- measure the real depth and width
    new_body_size.mV[VX] = DEFAULT_AGENT_DEPTH;
    new_body_size.mV[VY] = DEFAULT_AGENT_WIDTH;

    LL_DEBUGS("Avatar") << "new_body_size " << new_body_size << LL_ENDL;
}

//------------------------------------------------------------------------
// postPelvisSetRecalc
//------------------------------------------------------------------------
void LLVOAvatar::postPelvisSetRecalc()
{
    mRoot->updateWorldMatrixChildren();
    computeBodySize();
    dirtyMesh(2);
}
//------------------------------------------------------------------------
// updateVisibility()
//------------------------------------------------------------------------
void LLVOAvatar::updateVisibility()
{
    bool visible = false;

    if (mIsDummy)
    {
        visible = false;
    }
    else if (mDrawable.isNull())
    {
        visible = false;
    }
    else
    {
        // The drawable's own visibility stamp (vk_avatarVis), in place of LL's culled
        // spatial group: avatars have no octree (#476).
        if (mDrawable->isVisible())
        {
            visible = true;
        }

        if (isSelf())
        {
            if (!gAgentWearables.areWearablesLoaded())
            {
                visible = false;
            }
        }
        else if (!mFirstAppearanceMessageReceived)
        {
            visible = false;
        }

        if (sDebugInvisible)
        {
            if (getNVPair("FirstName"))
            {
                LL_DEBUGS("Avatar") << avString() << " updating visibility" << LL_ENDL;
            }
            else
            {
                LL_INFOS() << "Avatar " << this << " updating visiblity" << LL_ENDL;
            }

            if (visible)
            {
                LL_INFOS() << "Visible" << LL_ENDL;
            }
            else
            {
                LL_INFOS() << "Not visible" << LL_ENDL;
            }

            /*if (avatar_in_frustum)
            {
                LL_INFOS() << "Avatar in frustum" << LL_ENDL;
            }
            else
            {
                LL_INFOS() << "Avatar not in frustum" << LL_ENDL;
            }*/

            /*if (LLViewerCamera::getInstance()->sphereInFrustum(sel_pos_agent, 2.0f))
            {
                LL_INFOS() << "Sel pos visible" << LL_ENDL;
            }
            if (LLViewerCamera::getInstance()->sphereInFrustum(wrist_right_pos_agent, 0.2f))
            {
                LL_INFOS() << "Wrist pos visible" << LL_ENDL;
            }
            if (LLViewerCamera::getInstance()->sphereInFrustum(getPositionAgent(), getMaxScale()*2.f))
            {
                LL_INFOS() << "Agent visible" << LL_ENDL;
            }*/
            LL_INFOS() << "PA: " << getPositionAgent() << LL_ENDL;
            /*LL_INFOS() << "SPA: " << sel_pos_agent << LL_ENDL;
            LL_INFOS() << "WPA: " << wrist_right_pos_agent << LL_ENDL;*/
            for (attachment_map_t::iterator iter = mAttachmentPoints.begin();
                 iter != mAttachmentPoints.end();
                 ++iter)
            {
                LLViewerJointAttachment* attachment = iter->second;

                for (LLViewerJointAttachment::attachedobjs_vec_t::iterator attachment_iter = attachment->mAttachedObjects.begin();
                     attachment_iter != attachment->mAttachedObjects.end();
                     ++attachment_iter)
                {
                    if (LLViewerObject *attached_object = attachment_iter->get())
                    {
                        if(attached_object->mDrawable->isVisible())
                        {
                            LL_INFOS() << attachment->getName() << " visible" << LL_ENDL;
                        }
                        else
                        {
                            LL_INFOS() << attachment->getName() << " not visible at " << mDrawable->getWorldPosition() << " and radius " << mDrawable->getRadius() << LL_ENDL;
                        }
                    }
                }
            }
        }
    }

    if (!visible && mVisible)
    {
        mMeshInvisibleTime.reset();
    }

    if (visible)
    {
        if (!mMeshValid)
        {
            restoreMeshData();
        }
    }
    else
    {
        if (mMeshValid &&
            (isControlAvatar() || mMeshInvisibleTime.getElapsedTimeF32() > TIME_BEFORE_MESH_CLEANUP))
        {
            releaseMeshData();
        }
    }

    if (visible != mVisible)
    {
        LL_DEBUGS("AvatarRender") << "visible was " << mVisible << " now " << visible << LL_ENDL;
    }

    mVisible = visible;

    mVisibilityPreference = visible ? getPixelArea() : 0;
}

// private
bool LLVOAvatar::shouldAlphaMask()
{
    const bool should_alpha_mask = !LLPipeline::sShowDebugAlpha; // Don't alpha mask if "Highlight Transparent" checked

    return should_alpha_mask;

}

bool LLVOAvatar::allTexturesCompletelyDownloaded(std::set<LLUUID>& ids) const
{
    for (std::set<LLUUID>::const_iterator it = ids.begin(); it != ids.end(); ++it)
    {
        LLViewerFetchedTexture *imagep = gTextureList.findImage(*it, TEX_LIST_STANDARD);
        if (imagep && imagep->getDiscardLevel()!=0)
        {
            return false;
        }
    }
    return true;
}

bool LLVOAvatar::allLocalTexturesCompletelyDownloaded() const
{
    std::set<LLUUID> local_ids;
    collectLocalTextureUUIDs(local_ids);
    return allTexturesCompletelyDownloaded(local_ids);
}

bool LLVOAvatar::allBakedTexturesCompletelyDownloaded() const
{
    std::set<LLUUID> baked_ids;
    collectBakedTextureUUIDs(baked_ids);
    return allTexturesCompletelyDownloaded(baked_ids);
}

std::string LLVOAvatar::bakedTextureOriginInfo()
{
    std::string result;

    std::set<LLUUID> baked_ids;
    collectBakedTextureUUIDs(baked_ids);
    for (U32 i = 0; i < mBakedTextureDatas.size(); i++)
    {
        ETextureIndex texture_index = mBakedTextureDatas[i].mTextureIndex;
        LLViewerFetchedTexture *imagep =
            LLViewerTextureManager::staticCastToFetchedTexture(getImage(texture_index,0), true);
        if (!imagep ||
            imagep->getID() == IMG_DEFAULT ||
            imagep->getID() == IMG_DEFAULT_AVATAR)

        {
            result += "-";
        }
        else
        {
            bool has_url = false, has_host = false;
            if (!imagep->getUrl().empty())
            {
                has_url = true;
            }
            if (imagep->getTargetHost().isOk())
            {
                has_host = true;
            }
            S32 discard = imagep->getDiscardLevel();
            if (has_url && !has_host) result += discard ? "u" : "U"; // server-bake texture with url
            else if (has_host && !has_url) result += discard ? "h" : "H"; // old-style texture on sim
            else if (has_host && has_url) result += discard ? "x" : "X"; // both origins?
            else if (!has_host && !has_url) result += discard ? "n" : "N"; // no origin?
            if (discard != 0)
            {
                result += llformat("(%d/%d)",discard,imagep->getDesiredDiscardLevel());
            }
        }

    }
    return result;
}

void LLVOAvatar::collectLocalTextureUUIDs(std::set<LLUUID>& ids) const
{
    for (U32 texture_index = 0; texture_index < getNumTEs(); texture_index++)
    {
        LLWearableType::EType wearable_type = LLAvatarAppearance::getDictionary()->getTEWearableType((ETextureIndex)texture_index);
        U32 num_wearables = gAgentWearables.getWearableCount(wearable_type);

        LLViewerFetchedTexture *imagep = NULL;
        for (U32 wearable_index = 0; wearable_index < num_wearables; wearable_index++)
        {
            imagep = LLViewerTextureManager::staticCastToFetchedTexture(getImage(texture_index, wearable_index), true);
            if (imagep)
            {
                const LLAvatarAppearanceDictionary::TextureEntry *texture_dict = LLAvatarAppearance::getDictionary()->getTexture((ETextureIndex)texture_index);
                if (texture_dict && texture_dict->mIsLocalTexture)
                {
                    ids.insert(imagep->getID());
                }
            }
        }
    }
    ids.erase(IMG_DEFAULT);
    ids.erase(IMG_DEFAULT_AVATAR);
    ids.erase(IMG_INVISIBLE);
}

void LLVOAvatar::collectBakedTextureUUIDs(std::set<LLUUID>& ids) const
{
    for (U32 texture_index = 0; texture_index < getNumTEs(); texture_index++)
    {
        LLViewerFetchedTexture *imagep = NULL;
        if (isIndexBakedTexture((ETextureIndex) texture_index))
        {
            imagep = LLViewerTextureManager::staticCastToFetchedTexture(getImage(texture_index,0), true);
            if (imagep)
            {
                ids.insert(imagep->getID());
            }
        }
    }
    ids.erase(IMG_DEFAULT);
    ids.erase(IMG_DEFAULT_AVATAR);
    ids.erase(IMG_INVISIBLE);
}

void LLVOAvatar::collectTextureUUIDs(std::set<LLUUID>& ids)
{
    collectLocalTextureUUIDs(ids);
    collectBakedTextureUUIDs(ids);
}

void LLVOAvatar::releaseOldTextures()
{
    // Any textures that we used to be using but are no longer using should no longer be flagged as "NO_DELETE"
    std::set<LLUUID> baked_texture_ids;
    collectBakedTextureUUIDs(baked_texture_ids);

    std::set<LLUUID> local_texture_ids;
    collectLocalTextureUUIDs(local_texture_ids);

    std::set<LLUUID> new_texture_ids;
    new_texture_ids.insert(baked_texture_ids.begin(),baked_texture_ids.end());
    new_texture_ids.insert(local_texture_ids.begin(),local_texture_ids.end());

    for (std::set<LLUUID>::iterator it = mTextureIDs.begin(); it != mTextureIDs.end(); ++it)
    {
        if (new_texture_ids.find(*it) == new_texture_ids.end())
        {
            LLViewerFetchedTexture *imagep = gTextureList.findImage(*it, TEX_LIST_STANDARD);
            if (imagep)
            {
                if (imagep->getTextureState() == LLGLTexture::NO_DELETE)
                {
                    // This will allow the texture to be deleted if not in use.
                    imagep->forceActive();

                    // This resets the clock to texture being flagged
                    // as unused, preventing the texture from being
                    // deleted immediately. If other avatars or
                    // objects are using it, it can still be flagged
                    // no-delete by them.
                    imagep->forceUpdateBindStats();
                }
            }
        }
    }
    mTextureIDs = new_texture_ids;
}

void LLVOAvatar::updateTextures()
{
    releaseOldTextures();

    bool render_avatar = true;

    if (mIsDummy)
    {
        return;
    }

    if( isSelf() )
    {
        render_avatar = true;
    }
    else
    {
        if(!isVisible())
        {
            return ;//do not update for invisible avatar.
        }

        render_avatar = !mCulled; //visible and not culled.
    }

    std::vector<bool> layer_baked;
    // GL NOT ACTIVE HERE - *TODO
    for (U32 i = 0; i < mBakedTextureDatas.size(); i++)
    {
        layer_baked.push_back(isTextureDefined(mBakedTextureDatas[i].mTextureIndex));
        // bind the texture so that they'll be decoded slightly
        // inefficient, we can short-circuit this if we have to
        if (render_avatar)
        {
            if (layer_baked[i] && !mBakedTextureDatas[i].mIsLoaded)
            {
                gGL.getTexUnit(0)->bind(getImage( mBakedTextureDatas[i].mTextureIndex, 0 ));
            }
        }
    }

    mMaxPixelArea = 0.f;
    mMinPixelArea = 99999999.f;
    mHasGrey = false; // debug
    for (U32 texture_index = 0; texture_index < getNumTEs(); texture_index++)
    {
        LLWearableType::EType wearable_type = LLAvatarAppearance::getDictionary()->getTEWearableType((ETextureIndex)texture_index);
        U32 num_wearables = gAgentWearables.getWearableCount(wearable_type);
        const LLTextureEntry *te = getTE(texture_index);

        // getTE can return 0.
        // Not sure yet why it does, but of course it crashes when te->mScale? gets used.
        // Put safeguard in place so this corner case get better handling and does not result in a crash.
        F32 texel_area_ratio = 1.0f;
        if( te )
        {
            texel_area_ratio = fabs(te->mScaleS * te->mScaleT);
        }
        else
        {
            LL_WARNS() << "getTE( " << texture_index << " ) returned 0" <<LL_ENDL;
        }

        LLViewerFetchedTexture *imagep = NULL;
        for (U32 wearable_index = 0; wearable_index < num_wearables; wearable_index++)
        {
            imagep = LLViewerTextureManager::staticCastToFetchedTexture(getImage(texture_index, wearable_index), true);
            if (imagep)
            {
                const LLAvatarAppearanceDictionary::TextureEntry *texture_dict = LLAvatarAppearance::getDictionary()->getTexture((ETextureIndex)texture_index);
                const EBakedTextureIndex baked_index = texture_dict ? texture_dict->mBakedTextureIndex : EBakedTextureIndex::BAKED_NUM_INDICES;
                if (texture_dict && texture_dict->mIsLocalTexture)
                {
                    addLocalTextureStats((ETextureIndex)texture_index, imagep, texel_area_ratio, render_avatar, mBakedTextureDatas[baked_index].mIsUsed);
                }
            }
        }
        if (isIndexBakedTexture((ETextureIndex) texture_index) && render_avatar)
        {
            const S32 boost_level = getAvatarBakedBoostLevel();
            imagep = LLViewerTextureManager::staticCastToFetchedTexture(getImage(texture_index,0), true);
            addBakedTextureStats( imagep, mPixelArea, texel_area_ratio, boost_level );
        }
    }

    if (gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_TEXTURE_AREA))
    {
        setDebugText(llformat("%4.0f:%4.0f", (F32) sqrt(mMinPixelArea),(F32) sqrt(mMaxPixelArea)));
    }
}


void LLVOAvatar::addLocalTextureStats( ETextureIndex idx, LLViewerFetchedTexture* imagep,
                                       F32 texel_area_ratio, bool render_avatar, bool covered_by_baked)
{
    // No local texture stats for non-self avatars
    return;
}

const S32 MAX_TEXTURE_UPDATE_INTERVAL = 64 ; //need to call updateTextures() at least every 32 frames.
const S32 MAX_TEXTURE_VIRTUAL_SIZE_RESET_INTERVAL = S32_MAX ; //frames
void LLVOAvatar::checkTextureLoading()
{
    static const F32 MAX_INVISIBLE_WAITING_TIME = 15.f ; //seconds

    bool pause = !isVisible() ;
    if(!pause)
    {
        mInvisibleTimer.reset() ;
    }
    if(mLoadedCallbacksPaused == pause)
    {
        if (!pause && mFirstFullyVisible && mLoadedCallbackTextures < mCallbackTextureList.size())
        {
            // We still need to update 'loaded' textures count to decide on 'cloud' visibility
            // Alternatively this can be done on TextureLoaded callbacks, but is harder to properly track
            mLoadedCallbackTextures = 0;
            for (LLLoadedCallbackEntry::source_callback_list_t::iterator iter = mCallbackTextureList.begin();
                iter != mCallbackTextureList.end(); ++iter)
            {
                LLViewerFetchedTexture* tex = gTextureList.findImage(*iter);
                if (tex && (tex->getDiscardLevel() >= 0 || tex->isMissingAsset()))
                {
                    mLoadedCallbackTextures++;
                }
            }
        }
        return ;
    }

    if(mCallbackTextureList.empty()) //when is self or no callbacks. Note: this list for self is always empty.
    {
        mLoadedCallbacksPaused = pause ;
        mLoadedCallbackTextures = 0;
        return ; //nothing to check.
    }

    if(pause && mInvisibleTimer.getElapsedTimeF32() < MAX_INVISIBLE_WAITING_TIME)
    {
        return ; //have not been invisible for enough time.
    }

    mLoadedCallbackTextures = pause ? static_cast<S32>(mCallbackTextureList.size()) : 0;

    for(LLLoadedCallbackEntry::source_callback_list_t::iterator iter = mCallbackTextureList.begin();
        iter != mCallbackTextureList.end(); ++iter)
    {
        LLViewerFetchedTexture* tex = gTextureList.findImage(*iter) ;
        if(tex)
        {
            if(pause)//pause texture fetching.
            {
                tex->pauseLoadedCallbacks(&mCallbackTextureList) ;

                //set to terminate texture fetching after MAX_TEXTURE_UPDATE_INTERVAL frames.
                tex->setMaxVirtualSizeResetInterval(MAX_TEXTURE_UPDATE_INTERVAL);
                tex->resetMaxVirtualSizeResetCounter() ;
            }
            else//unpause
            {
                static const F32 START_AREA = 100.f ;

                tex->unpauseLoadedCallbacks(&mCallbackTextureList) ;
                tex->addTextureStats(START_AREA); //jump start the fetching again

                // technically shouldn't need to account for missing, but callback might not have happened yet
                if (tex->getDiscardLevel() >= 0 || tex->isMissingAsset())
                {
                    mLoadedCallbackTextures++; // consider it loaded (we have at least some data)
                }
            }
        }
    }

    if(!pause)
    {
        updateTextures() ; //refresh texture stats.
    }
    mLoadedCallbacksPaused = pause ;
    return ;
}

const F32  SELF_ADDITIONAL_PRI = 0.75f ;
// static -- RenderVKBakeDiag: every 5 s, log the bake textures of every non-self avatar
// that is visible but not fully baked-and-loaded (BOM "stuck fuzzy" investigation, #105).
void LLVOAvatar::logBakeDiag()
{
    // Also automatic, setting off: an avatar that has been visible for 60 s with a bake
    // still not loaded is logged once, so a stuck avatar leaves evidence without anyone
    // having known to turn the diagnostic on first.
    static LLCachedControl<bool> s_diag(gSavedSettings, "RenderVKBakeDiag", false);
    static F64 s_next = 0.0;
    static std::map<LLUUID, F64> s_stuck_since;
    static std::set<LLUUID> s_auto_logged;
    const F64 now = LLTimer::getTotalSeconds();
    if (now < s_next) return;
    s_next = now + 5.0;
    // Avatars that leave while stuck are never erased: keep both sets bounded.
    if (s_stuck_since.size() > 1024) s_stuck_since.clear();
    if (s_auto_logged.size() > 4096) s_auto_logged.clear();
    for (LLCharacter* ch : LLCharacter::sInstances)
    {
        LLVOAvatar* av = (LLVOAvatar*)ch;
        if (!av || av->isDead() || av->isSelf() || av->isUIAvatar() || av->isControlAvatar()) continue;
        bool log_it = s_diag;
        if (!log_it)
        {
            bool stuck = false;
            if (av->isVisible() && av->isFullyBaked())
            {
                for (U32 i = 0; i < av->mBakedTextureDatas.size(); ++i)
                {
                    if (av->mBakedTextureDatas[i].mIsUsed && !av->mBakedTextureDatas[i].mIsLoaded) { stuck = true; break; }
                }
            }
            if (!stuck) { s_stuck_since.erase(av->getID()); continue; }
            auto it = s_stuck_since.emplace(av->getID(), now).first;
            if (now - it->second < 60.0 || !s_auto_logged.insert(av->getID()).second) continue;
            LL_WARNS("BakeDiag") << "Avatar " << av->getID() << " visible for 60 s with bakes not loaded; logging bake state once" << LL_ENDL;
        }
        std::ostringstream hdr;
        hdr << "BakeDiag av=" << av->getID() << " visible=" << av->isVisible() << " culled=" << av->mCulled
            << " fully_baked=" << av->isFullyBaked() << " fully_loaded=" << av->isFullyLoaded()
            << " pixel_area=" << av->mPixelArea << " appearance_anim=" << av->getIsAppearanceAnimating()
            << " name=\"" << av->getFullname() << "\"";
        LL_INFOS("BakeDiag") << hdr.str() << LL_ENDL;
        for (U32 i = 0; i < av->mBakedTextureDatas.size(); ++i)
        {
            const ETextureIndex te = av->mBakedTextureDatas[i].mTextureIndex;
            LLViewerFetchedTexture* img = LLViewerTextureManager::staticCastToFetchedTexture(av->getImage(te, 0), true);
            LL_INFOS("BakeDiag") << "  bake " << i << " te=" << (S32)te << " defined=" << av->isTextureDefined(te)
                << " is_loaded=" << av->mBakedTextureDatas[i].mIsLoaded << " is_used=" << av->mBakedTextureDatas[i].mIsUsed
                << " " << (img ? img->fetchDiagString() : std::string("no image")) << LL_ENDL;
        }
        // BOM attachment faces: the texture the face (and so the VK material) actually
        // uses vs. the avatar's current bake for that sentinel.
        for (const auto& ap : av->mAttachmentPoints)
        {
            LLViewerJointAttachment* att = ap.second;
            if (!att) continue;
            for (LLViewerObject* root : att->mAttachedObjects)
            {
                if (!root || root->isDead()) continue;
                std::vector<LLViewerObject*> objs{root};
                for (LLViewerObject* c : root->getChildren()) objs.push_back(c);
                for (LLViewerObject* o : objs)
                {
                    if (!o || !o->mDrawable) continue;
                    for (S32 f = 0; f < o->mDrawable->getNumFaces(); ++f)
                    {
                        LLFace* face = o->mDrawable->getFace(f);
                        const LLTextureEntry* te = face ? face->getTextureEntry() : nullptr;
                        if (!te || !LLAvatarAppearanceDefines::LLAvatarAppearanceDictionary::isBakedImageId(te->getID())) continue;
                        LLViewerTexture* want = o->getBakedTextureForMagicId(te->getID());
                        LLViewerTexture* have = face->getTexture();
                        const LLGLTFMaterial* gmat = te->getGLTFRenderMaterial();
                        const bool pbr = gmat != nullptr;
                        LL_INFOS("BakeDiag") << "  att obj=" << o->getID() << " face=" << f << " sentinel=" << te->getID()
                            << " pbr=" << pbr
                            << " mat_base=" << (gmat ? gmat->mTextureId[LLGLTFMaterial::GLTF_TEXTURE_INFO_BASE_COLOR].asString() : std::string("-"))
                            << " face_tex=" << (have ? have->getID().asString() : std::string("null"))
                            << " face_disc=" << (have ? have->getDiscardLevel() : -2)
                            << " bake=" << (want ? want->getID().asString() : std::string("null"))
                            << " match=" << (have == want) << LL_ENDL;
                    }
                }
            }
        }
    }
}

void LLVOAvatar::addBakedTextureStats( LLViewerFetchedTexture* imagep, F32 pixel_area, F32 texel_area_ratio, S32 boost_level)
{
    //Note:
    //if this function is not called for the last MAX_TEXTURE_VIRTUAL_SIZE_RESET_INTERVAL frames,
    //the texture pipeline will stop fetching this texture.

    imagep->resetTextureStats();
    imagep->setMaxVirtualSizeResetInterval(MAX_TEXTURE_VIRTUAL_SIZE_RESET_INTERVAL);
    imagep->resetMaxVirtualSizeResetCounter() ;

    mMaxPixelArea = llmax(pixel_area, mMaxPixelArea);
    mMinPixelArea = llmin(pixel_area, mMinPixelArea);
    imagep->addTextureStats(pixel_area / texel_area_ratio);
    imagep->setBoostLevel(boost_level);
}

//virtual
void LLVOAvatar::setImage(const U8 te, LLViewerTexture *imagep, const U32 index)
{
    setTEImage(te, imagep);
}

//virtual
LLViewerTexture* LLVOAvatar::getImage(const U8 te, const U32 index) const
{
    return getTEImage(te);
}
//virtual
const LLTextureEntry* LLVOAvatar::getTexEntry(const U8 te_num) const
{
    return getTE(te_num);
}

//virtual
void LLVOAvatar::setTexEntry(const U8 index, const LLTextureEntry &te)
{
    setTE(index, te);
}

const std::string LLVOAvatar::getImageURL(const U8 te, const LLUUID &uuid)
{
    llassert(isIndexBakedTexture(ETextureIndex(te)));
    std::string url = "";
    const std::string& appearance_service_url = LLAppearanceMgr::instance().getAppearanceServiceURL();
    if (appearance_service_url.empty())
    {
        // Probably a server-side issue if we get here:
        LL_WARNS() << "AgentAppearanceServiceURL not set - Baked texture requests will fail" << LL_ENDL;
        return url;
    }

    const LLAvatarAppearanceDictionary::TextureEntry* texture_entry = LLAvatarAppearance::getDictionary()->getTexture((ETextureIndex)te);
    if (texture_entry != NULL)
    {
        url = appearance_service_url + "texture/" + getID().asString() + "/" + texture_entry->mDefaultImageName + "/" + uuid.asString();
        //LL_INFOS() << "baked texture url: " << url << LL_ENDL;
    }
    return url;
}

//-----------------------------------------------------------------------------
// resolveHeight()
//-----------------------------------------------------------------------------

void LLVOAvatar::resolveHeightAgent(const LLVector3 &in_pos_agent, LLVector3 &out_pos_agent, LLVector3 &out_norm)
{
    LLVector3d in_pos_global, out_pos_global;

    in_pos_global = gAgent.getPosGlobalFromAgent(in_pos_agent);
    resolveHeightGlobal(in_pos_global, out_pos_global, out_norm);
    out_pos_agent = gAgent.getPosAgentFromGlobal(out_pos_global);
}


void LLVOAvatar::resolveRayCollisionAgent(const LLVector3d start_pt, const LLVector3d end_pt, LLVector3d &out_pos, LLVector3 &out_norm)
{
    LLViewerObject *obj;
    LLWorld::getInstance()->resolveStepHeightGlobal(this, start_pt, end_pt, out_pos, out_norm, &obj);
}

void LLVOAvatar::resolveHeightGlobal(const LLVector3d &inPos, LLVector3d &outPos, LLVector3 &outNorm)
{
    LLVector3d zVec(0.0f, 0.0f, 0.5f);
    LLVector3d p0 = inPos + zVec;
    LLVector3d p1 = inPos - zVec;
    LLViewerObject *obj;
    LLWorld::getInstance()->resolveStepHeightGlobal(this, p0, p1, outPos, outNorm, &obj);
    if (!obj)
    {
        mStepOnLand = true;
        mStepMaterial = 0;
        mStepObjectVelocity.setVec(0.0f, 0.0f, 0.0f);
    }
    else
    {
        mStepOnLand = false;
        mStepMaterial = obj->getMaterial();

        // We want the primitive velocity, not our velocity... (which actually subtracts the
        // step object velocity)
        LLVector3 angularVelocity = obj->getAngularVelocity();
        LLVector3 relativePos = gAgent.getPosAgentFromGlobal(outPos) - obj->getPositionAgent();

        LLVector3 linearComponent = angularVelocity % relativePos;
//      LL_INFOS() << "Linear Component of Rotation Velocity " << linearComponent << LL_ENDL;
        mStepObjectVelocity = obj->getVelocity() + linearComponent;
    }
}


//-----------------------------------------------------------------------------
// getStepSound()
//-----------------------------------------------------------------------------
const LLUUID& LLVOAvatar::getStepSound() const
{
    if ( mStepOnLand )
    {
        return sStepSoundOnLand;
    }

    return sStepSounds[mStepMaterial];
}


//-----------------------------------------------------------------------------
// processAnimationStateChanges()
//-----------------------------------------------------------------------------
void LLVOAvatar::processAnimationStateChanges()
{
    if ( isAnyAnimationSignaled(AGENT_WALK_ANIMS, NUM_AGENT_WALK_ANIMS) )
    {
        startMotion(ANIM_AGENT_WALK_ADJUST);
        stopMotion(ANIM_AGENT_FLY_ADJUST);
    }
    else if (mInAir && !isSitting())
    {
        stopMotion(ANIM_AGENT_WALK_ADJUST);
        if (mEnableDefaultMotions)
        {
        startMotion(ANIM_AGENT_FLY_ADJUST);
    }
    }
    else
    {
        stopMotion(ANIM_AGENT_WALK_ADJUST);
        stopMotion(ANIM_AGENT_FLY_ADJUST);
    }

    if ( isAnyAnimationSignaled(AGENT_GUN_AIM_ANIMS, NUM_AGENT_GUN_AIM_ANIMS) )
    {
        if (mEnableDefaultMotions)
        {
        startMotion(ANIM_AGENT_TARGET);
        }
        stopMotion(ANIM_AGENT_BODY_NOISE);
    }
    else
    {
        stopMotion(ANIM_AGENT_TARGET);
        if (mEnableDefaultMotions)
        {
            startMotion(ANIM_AGENT_BODY_NOISE);
        }
    }

    // clear all current animations
    AnimIterator anim_it;
    for (anim_it = mPlayingAnimations.begin(); anim_it != mPlayingAnimations.end();)
    {
        AnimIterator found_anim = mSignaledAnimations.find(anim_it->first);

        // playing, but not signaled, so stop
        if (found_anim == mSignaledAnimations.end())
        {
            processSingleAnimationStateChange(anim_it->first, false);
            mPlayingAnimations.erase(anim_it++);
            continue;
        }

        ++anim_it;
    }

    // if jellydolled, shelve all playing animations
    if (getOverallAppearance() != AOA_NORMAL)
    {
        mPlayingAnimations.clear();
    }

    // start up all new anims
    if (getOverallAppearance() == AOA_NORMAL)
    {
        for (anim_it = mSignaledAnimations.begin(); anim_it != mSignaledAnimations.end();)
        {
            AnimIterator found_anim = mPlayingAnimations.find(anim_it->first);

            // signaled but not playing, or different sequence id, start motion
            if (found_anim == mPlayingAnimations.end() || found_anim->second != anim_it->second)
            {
                if (processSingleAnimationStateChange(anim_it->first, true))
                {
                    mPlayingAnimations[anim_it->first] = anim_it->second;
                    ++anim_it;
                    continue;
                }
            }

            ++anim_it;
        }
    }

    // clear source information for animations which have been stopped
    if (isSelf())
    {
        AnimSourceIterator source_it = mAnimationSources.begin();

        for (source_it = mAnimationSources.begin(); source_it != mAnimationSources.end();)
        {
            if (mSignaledAnimations.find(source_it->second) == mSignaledAnimations.end())
            {
                mAnimationSources.erase(source_it++);
            }
            else
            {
                ++source_it;
            }
        }
    }

}


//-----------------------------------------------------------------------------
// processSingleAnimationStateChange();
//-----------------------------------------------------------------------------
bool LLVOAvatar::processSingleAnimationStateChange( const LLUUID& anim_id, bool start )
{
    // SL-402, SL-427 - we need to update body size often enough to
    // keep appearances in sync, but not so often that animations
    // cause constant jiggling of the body or camera. Possible
    // compromise is to do it on animation changes:
    computeBodySize();

    bool result = false;

    if ( start ) // start animation
    {
        if (anim_id == ANIM_AGENT_TYPE)
        {
            static LLCachedControl<bool> play_typing_sound(gSavedSettings, "PlayTypingSound", true);
            if (gAudiop && play_typing_sound)
            {
                LLVector3d char_pos_global = gAgent.getPosGlobalFromAgent(getCharacterPosition());
                if (LLViewerParcelMgr::getInstance()->canHearSound(char_pos_global)
                    && !LLMuteList::getInstance()->isMuted(getID(), LLMute::flagObjectSounds))
                {
                    // RN: uncomment this to play on typing sound at fixed volume once sound engine is fixed
                    // to support both spatialized and non-spatialized instances of the same sound
                    //if (isSelf())
                    //{
                    //  gAudiop->triggerSound(LLUUID(gSavedSettings.getString("UISndTyping")), 1.0f, LLAudioEngine::AUDIO_TYPE_UI);
                    //}
                    //else
                    {
                        static LLCachedControl<std::string> ui_snd_string(gSavedSettings, "UISndTyping");
                        LLUUID sound_id = LLUUID(ui_snd_string);
                        LLUUID typing_src = gAudiop->triggerSound(sound_id, getID(), 1.0f, LLAudioEngine::AUDIO_TYPE_SFX, char_pos_global);
                        if (typing_src.notNull())
                        {
                            SLSoundLog::getInstance()->recordTriggered(sound_id, getID(), getID(), char_pos_global, typing_src, false);
                        }
                    }
                }
            }
        }
        else if (anim_id == ANIM_AGENT_SIT_GROUND_CONSTRAINED)
        {
            sitDown(true);
        }
        else if (anim_id == ANIM_AGENT_SNAPSHOT)
        {
            static LLCachedControl<bool> announce_snapshot(gSavedSettings, "SnapshotDetection");
            if (announce_snapshot)
            {
                LLNotificationsUtil::add("SnapshotDetected", LLSD().with("NAME", LLSLURL("agent", mID, "about").getSLURLString()));
            }
        }

        if (startMotion(anim_id))
        {
            result = true;
        }
        else
        {
            LL_WARNS("Motion") << "Failed to start motion!" << LL_ENDL;
        }
    }
    else //stop animation
    {
        if (anim_id == ANIM_AGENT_SIT_GROUND_CONSTRAINED)
        {
            sitDown(false);
        }
        if ((anim_id == ANIM_AGENT_DO_NOT_DISTURB) && gAgent.isDoNotDisturb())
        {
            // re-assert DND tag animation
            gAgent.sendAnimationRequest(ANIM_AGENT_DO_NOT_DISTURB, ANIM_REQUEST_START);
            return result;
        }
        stopMotion(anim_id);
        result = true;
    }

    return result;
}

//-----------------------------------------------------------------------------
// isAnyAnimationSignaled()
//-----------------------------------------------------------------------------
bool LLVOAvatar::isAnyAnimationSignaled(const LLUUID *anim_array, const S32 num_anims) const
{
    for (S32 i = 0; i < num_anims; i++)
    {
        if(mSignaledAnimations.find(anim_array[i]) != mSignaledAnimations.end())
        {
            return true;
        }
    }
    return false;
}

//-----------------------------------------------------------------------------
// resetAnimations()
//-----------------------------------------------------------------------------
void LLVOAvatar::resetAnimations()
{
    LLKeyframeMotion::flushKeyframeCache();
    flushAllMotions();
}

//-----------------------------------------------------------------------------
// getPlayingAnimState()
//-----------------------------------------------------------------------------
LLVOAvatar::EPlayingAnimState LLVOAvatar::getPlayingAnimState(const LLUUID& anim_id)
{
    // The motion controller holds the REMAPPED id (sex-specific walk / run / sit).
    LLMotion* motionp = mMotionController.findMotion(remapMotionID(anim_id));
    if (!motionp)
    {
        return PLAYING_ANIM_NO_MOTION;
    }
    // Checked first: a motion still waiting on its data has no loop flag yet.
    if (mMotionController.isMotionLoading(motionp))
    {
        return PLAYING_ANIM_LOADING;
    }
    if (mMotionController.isMotionActive(motionp))
    {
        return PLAYING_ANIM_ACTIVE;
    }
    return motionp->getLoop() ? PLAYING_ANIM_INACTIVE_LOOP : PLAYING_ANIM_INACTIVE;
}

//-----------------------------------------------------------------------------
// restartAnimation()
//-----------------------------------------------------------------------------
bool LLVOAvatar::restartAnimation(const LLUUID& anim_id, bool refetch, bool* refetched)
{
    const LLUUID remap_id = remapMotionID(anim_id);
    bool needs_data = false;
    if (refetch)
    {
        // Drop every instance (the canonical one and any deprecated duplicate still
        // easing out). A failed fetch deletes its motion, but one whose fetch stalled
        // sits in the loading set forever; a fresh instance requests its data again.
        mMotionController.purgeMotionInstances(remap_id);

        // Keyframe data shared through LLKeyframeDataCache came from a successful decode
        // and is referenced by the motions of every avatar playing it: keep it (the new
        // instance picks it up at once). Without it, the data must be loaded; drop the
        // locally cached asset first so a truncated / undecodable file cannot fail the
        // same way again and the asset really comes from the network.
        if (!LLKeyframeDataCache::getKeyframeData(remap_id))
        {
            LLFileSystem::removeFile(remap_id, LLAssetType::AT_ANIMATION);
            needs_data = true;
        }
    }
    // LLCharacter's start, not LLVOAvatar's: the animation is already in the playing set,
    // so the self-only side effects of a new start (AFK on ANIM_AGENT_AWAY) must not
    // fire again; only the motion behind it is rebuilt.
    const bool started = LLCharacter::startMotion(remap_id);
    // Re-fetched: a keyframe motion now loading its data (procedural motions have none).
    if (refetched && needs_data && started
        && dynamic_cast<LLKeyframeMotion*>(mMotionController.findMotion(remap_id)))
    {
        *refetched = true;
    }
    return started;
}

//-----------------------------------------------------------------------------
// refreshAnimations()
//-----------------------------------------------------------------------------
void LLVOAvatar::refreshAnimations()
{
    if (isDead())
    {
        return;
    }

    logAnimState("refresh (before)");   // #631: the state the refresh found

    // Collect what to restart, oldest first: activateMotionInstance() pushes each started
    // motion to the FRONT of the active list, so restarting in the original start order
    // reproduces the original order (it breaks blend ties between equal priorities).
    // 1. the playing set from the last AvatarAnimation message, by sequence id, plus
    //    (#631) every SIGNALED animation that never made it into the playing set -- its
    //    start failed outright (e.g. an undecodable locally cached asset) and nothing
    //    retried it; these join the playing set once they start;
    std::vector<std::pair<S32, LLUUID>> by_seq;
    by_seq.reserve(mPlayingAnimations.size() + mSignaledAnimations.size());
    for (const auto& kv : mPlayingAnimations)
    {
        by_seq.emplace_back(kv.second, kv.first);
    }
    std::map<LLUUID, S32> signaled_only;    // signaled, not playing -> sequence id
    // (shelved on purpose while jellydolled; self: the agent / client AO own that state)
    if (!isSelf() && getOverallAppearance() == AOA_NORMAL)
    {
        for (const auto& kv : mSignaledAnimations)
        {
            if (!mPlayingAnimations.count(kv.first))
            {
                signaled_only.emplace(kv.first, kv.second);
                by_seq.emplace_back(kv.second, kv.first);
            }
        }
    }
    std::stable_sort(by_seq.begin(), by_seq.end(),
                     [](const std::pair<S32, LLUUID>& a, const std::pair<S32, LLUUID>& b) { return a.first < b.first; });
    std::vector<LLUUID> anims;          // as signaled (not remapped)
    std::set<LLUUID> remapped;          // motion ids already covered
    anims.reserve(by_seq.size());
    for (const auto& seq_anim : by_seq)
    {
        if (remapped.insert(remapMotionID(seq_anim.second)).second)
        {
            anims.push_back(seq_anim.second);
        }
    }
    const size_t num_playing = anims.size();
    // 2. keyframe motions the motion controller has active outside the playing set
    //    (locally started default motions), back to front = oldest first. Procedural
    //    motions (head rotation, breathing, editing...) compute their pose every frame
    //    and are left alone, as are motions already easing out.
    LLMotionController::motion_list_t& active = mMotionController.getActiveMotions();
    for (auto it = active.rbegin(); it != active.rend(); ++it)
    {
        LLMotion* motionp = *it;
        if (!motionp || !dynamic_cast<LLKeyframeMotion*>(motionp))
        {
            continue;
        }
        if (motionp->isStopped() || motionp->getStopTime() != 0.f)
        {
            continue;
        }
        if (remapped.insert(motionp->getID()).second)
        {
            anims.push_back(motionp->getID());
        }
    }

    // A refresh gives the playing-without-motion repair fresh attempts as well.
    mAnimHealRetry.clear();

    std::vector<LLUUID> fetching;
    S32 num_failed = 0;
    const bool ao_self = isSelf() && SLAOEngine::instanceExists();
    for (const LLUUID& anim_id : anims)
    {
        // Client AO (#668): a built-in kept off our avatar under a replacement
        if (ao_self && SLAOEngine::instance().suppressesStockMotion(anim_id))
        {
            continue;
        }
        // Only keyframe animations: a playing id whose motion is procedural (or an emote)
        // has no asset to fetch.
        if (LLMotion* motionp = mMotionController.findMotion(remapMotionID(anim_id)))
        {
            if (!dynamic_cast<LLKeyframeMotion*>(motionp))
            {
                continue;
            }
        }
        bool refetched = false;
        if (!restartAnimation(anim_id, true, &refetched))
        {
            ++num_failed;   // could not even be created (its data failed to decode at once)
        }
        else
        {
            auto so = signaled_only.find(anim_id);
            if (so != signaled_only.end())
            {
                mPlayingAnimations[anim_id] = so->second;   // #631: started at last
            }
            if (refetched)
            {
                fetching.push_back(anim_id);
            }
        }
    }

    // The purged motion and joint state objects may be reallocated at the same addresses;
    // never let the GPU animation path validate its cached export against them.
    if (LLCleanAnimStore::instanceExists())
    {
        LLCleanAnimStore::instance().refreshAvatar(this);
    }

    // #631: pose the avatar on its next frames whatever its visibility stamp / animation
    // rate (an avatar drawn but stamped hidden is never evaluated -- the refresh would
    // rebuild its motions and nothing would change on screen).
    mVKForceAnimEval = 10;

    // One log line per refresh, written once the re-fetches had time to land: an
    // animation still without a motion, or still loading, failed again.
    const LLUUID av_id = getID();
    const std::string av_name = avString();
    const S32 num_restarted = (S32)anims.size();
    LLVOAvatar* avp = this;
    doAfterInterval([avp, av_id, av_name, num_playing, num_restarted, fetching, num_failed]()
        {
            const bool alive = std::find(LLCharacter::sInstances.begin(), LLCharacter::sInstances.end(),
                                         static_cast<LLCharacter*>(avp)) != LLCharacter::sInstances.end()
                               && avp->getID() == av_id && !avp->isDead();
            if (!alive)
            {
                LL_INFOS("Motion") << av_name << "refresh animations: " << num_restarted << " restarted ("
                                   << num_playing << " playing), " << fetching.size()
                                   << " re-fetched; avatar gone before the result" << LL_ENDL;
                return;
            }
            // (Read-only: avp may be a re-rezzed avatar at the same address.)
            S32 failed_again = num_failed;
            S32 still_loading = 0;
            for (const LLUUID& anim_id : fetching)
            {
                const LLVOAvatar::EPlayingAnimState state = avp->getPlayingAnimState(anim_id);
                if (state == LLVOAvatar::PLAYING_ANIM_NO_MOTION)
                {
                    ++failed_again;
                }
                else if (state == LLVOAvatar::PLAYING_ANIM_LOADING)
                {
                    ++still_loading;
                }
            }
            LL_INFOS("Motion") << av_name << "refresh animations: " << num_restarted << " restarted ("
                               << num_playing << " playing), " << fetching.size() << " re-fetched, "
                               << failed_again << " failed again, " << still_loading << " still loading" << LL_ENDL;
            avp->logAnimState("refresh (+10 s)");   // #631: what the refresh left
        }, 10.f);
}

//-----------------------------------------------------------------------------
// logAnimState() -- #631 (avatars stuck in their rest pose). One VKAnimState line with
// every input that decides whether this avatar's pose is animated:
//  vis{}    isVisible() and its parts (visibility stamp, orphaned), seconds since it was
//           last visible, and whether the render position / the drawable position (what
//           the stamp tests) are in the camera frustum -- a drawn but unstamped avatar
//           runs HIDDEN_UPDATE, which never poses it;
//  aoa      overall appearance (jellydoll / invisible shelve the animations);
//  eval{}   seconds since the last posing motion update, this frame's rate decision;
//  anims{}  signaled vs playing counts, signaled-but-not-playing ids, the state of the
//           motion behind each playing animation (ACTIVE / LOADING / INACTIVE /
//           INACTIVE_LOOP / NO_MOTION), repair backoffs, active motions;
//  gpu{}    the GPU animation store's view (LLCleanAnimStore::describeAvatar);
//  skins    rigged skin entries / unresolved ones (drawn in their bind pose).
// Logged by Refresh Animations (before, and 10 s after) and every 15 s per avatar with
// RenderVKAnimStateDebug. Lines whose state looks stuck carry SUSPECT.
//-----------------------------------------------------------------------------
bool LLVOAvatar::logAnimState(const char* why)
{
    if (isDead())
    {
        return false;
    }
    const F64 now = gFrameTimeSeconds;
    auto ago = [now](F64 t) -> std::string { return t < 0.0 ? std::string("never") : llformat("%.1f", now - t); };
    std::ostringstream os;
    std::vector<std::string> suspect;

    // visibility
    const LLViewerCamera* cam = LLViewerCamera::getInstance();
    const bool has_drawable = mDrawable.notNull();
    const bool stamp = has_drawable && mDrawable->isVisible();
    const bool render_in_view = cam->sphereInFrustum(getRenderPosition(), 3.f) != 0;
    const bool stamp_pos_in_view = has_drawable
        && cam->sphereInFrustum(mDrawable->getPositionAgent(), llmax(mDrawable->getRadius(), 3.f)) != 0;
    const bool vis = isVisible();
    os << "vis{built=" << mIsBuilt << " mesh_valid=" << mMeshValid << " visible=" << vis << " stamp=" << stamp << " drawable=" << has_drawable << " orphaned=" << isOrphaned()
       << " visible_ago=" << ago(mVKLastVisibleTime) << " render_pos_in_view=" << render_in_view
       << " stamp_pos_in_view=" << stamp_pos_in_view << " sitting=" << isSitting() << " parent=" << (getParent() != nullptr)
       << " dist=" << llformat("%.1f", dist_vec(getRenderPosition(), cam->getOrigin())) << "}";

    // appearance
    const AvatarOverallAppearance aoa = getOverallAppearance();
    static const char* const kAoa[] = { "normal", "jellydoll", "invisible" };
    os << " aoa=" << ((U32)aoa < LL_ARRAY_SIZE(kAoa) ? kAoa[aoa] : "?") << " muted=" << isVisuallyMuted()
       << " render_setting=" << (S32)mVisuallyMuteSetting;

    // evaluation
    static const char* const kReason[] = { "full", "near", "large", "px", "dist", "reduced", "hidden", "exempt" };
    os << " eval{ago=" << ago(mVKLastAnimEvalTime) << " reason="
       << (mVKAnimReason < LL_ARRAY_SIZE(kReason) ? kReason[mVKAnimReason] : "?") << " rate=" << mVKAnimRate
       << " reduced=" << mVKReduced << " was_hidden=" << mVKWasHidden << " paused=" << mMotionController.isPaused()
       << " forced_left=" << (U32)mVKForceAnimEval << "}";
    if (render_in_view && !vis && aoa == AOA_NORMAL)
    {
        suspect.push_back("in view but not stamped visible (pose never evaluated)");
    }
    if (render_in_view && (mVKLastAnimEvalTime < 0.0 || now - mVKLastAnimEvalTime > 3.0) && aoa == AOA_NORMAL)
    {
        suspect.push_back("in view, no pose evaluation for >3 s");
    }

    // animations
    static const char* const kState[] = { "NO_MOTION", "LOADING", "INACTIVE_LOOP", "INACTIVE", "ACTIVE" };
    U32 counts[5] = { 0, 0, 0, 0, 0 };
    std::string not_active;
    for (const auto& kv : mPlayingAnimations)
    {
        const EPlayingAnimState st = getPlayingAnimState(kv.first);
        if ((U32)st < 5) ++counts[st];
        if (st != PLAYING_ANIM_ACTIVE)
        {
            const auto r = mAnimHealRetry.find(kv.first);
            not_active += llformat(" %s=%s", kv.first.asString().substr(0, 8).c_str(), kState[(U32)st < 5 ? st : 0]);
            if (r != mAnimHealRetry.end()) not_active += llformat("(heal %d)", (S32)r->second.attempts);
        }
    }
    std::string sig_only;
    U32 n_sig_only = 0;
    for (const auto& kv : mSignaledAnimations)
    {
        if (mPlayingAnimations.count(kv.first)) continue;
        ++n_sig_only;
        sig_only += " " + kv.first.asString().substr(0, 8);
        const auto r = mAnimHealRetry.find(kv.first);
        if (r != mAnimHealRetry.end()) sig_only += llformat("(heal %d)", (S32)r->second.attempts);
    }
    os << " anims{signaled=" << mSignaledAnimations.size() << " playing=" << mPlayingAnimations.size()
       << " active=" << counts[PLAYING_ANIM_ACTIVE] << " loading=" << counts[PLAYING_ANIM_LOADING]
       << " inactive=" << counts[PLAYING_ANIM_INACTIVE] << " inactive_loop=" << counts[PLAYING_ANIM_INACTIVE_LOOP]
       << " no_motion=" << counts[PLAYING_ANIM_NO_MOTION] << " active_motions="
       << mMotionController.getActiveMotions().size();
    if (!not_active.empty()) os << " not_active:" << not_active;
    if (n_sig_only) os << " signaled_not_playing:" << sig_only;
    os << "}";
    if (aoa == AOA_NORMAL)
    {
        if (n_sig_only) suspect.push_back("signaled animations not playing");
        if (counts[PLAYING_ANIM_NO_MOTION] || counts[PLAYING_ANIM_INACTIVE_LOOP])
            suspect.push_back("playing animations without a running motion");
        if (counts[PLAYING_ANIM_LOADING]) suspect.push_back("playing animations still loading");
    }

    // GPU animation + skins
    if (LLCleanAnimStore::instanceExists())
    {
        bool gpu_stale = false;
        os << " " << LLCleanAnimStore::instance().describeAvatar(this, &gpu_stale);
        if (gpu_stale) suspect.push_back("GPU-animated but no motion update for 600+ frames (its joint table row copies the CPU joints)");
    }
    if (!mIsBuilt) suspect.push_back("not built (updateCharacter returns before any animation work)");
    U32 skins = 0, unresolved = 0;
    LLCleanSkinStore::instance().countAvatarSkins(getID(), skins, unresolved);
    os << " skins=" << skins << " unresolved=" << unresolved;
    if (unresolved) suspect.push_back("unresolved rigged skins (drawn in bind pose)");

    std::string tag;
    if (!suspect.empty())
    {
        tag = " SUSPECT:";
        for (const std::string& s : suspect) tag += " [" + s + "]";
    }
    LL_INFOS("VKAnimState") << avString() << (isControlAvatar() ? "(animesh) " : "") << why << ": " << os.str()
                            << tag << LL_ENDL;
    return !suspect.empty();
}

//-----------------------------------------------------------------------------
// remapMotionID()
// Override selectively based on avatar sex and whether we're using new animations.
//-----------------------------------------------------------------------------
// virtual
LLUUID LLVOAvatar::remapMotionID(const LLUUID& id)
{
    static LLCachedControl<bool> use_new_walk_run(gSavedSettings, "UseNewWalkRun");
    LLUUID result = id;

    // start special case female walk for female avatars
    if (getSex() == SEX_FEMALE)
    {
        if (id == ANIM_AGENT_WALK)
        {
            if (use_new_walk_run)
                result = ANIM_AGENT_FEMALE_WALK_NEW;
            else
                result = ANIM_AGENT_FEMALE_WALK;
        }
        else if (id == ANIM_AGENT_RUN)
        {
            // There is no old female run animation, so only override
            // in one case.
            if (use_new_walk_run)
                result = ANIM_AGENT_FEMALE_RUN_NEW;
        }
        else if (id == ANIM_AGENT_SIT)
        {
            result = ANIM_AGENT_SIT_FEMALE;
        }
    }
    else
    {
        // Male avatar.
        if (id == ANIM_AGENT_WALK)
        {
            if (use_new_walk_run)
                result = ANIM_AGENT_WALK_NEW;
        }
        else if (id == ANIM_AGENT_RUN)
        {
            if (use_new_walk_run)
                result = ANIM_AGENT_RUN_NEW;
        }
        // keeps in sync with setSex() related code (viewer controls sit's sex)
        else if (id == ANIM_AGENT_SIT_FEMALE)
        {
            result = ANIM_AGENT_SIT;
        }

    }

    return result;
}

//-----------------------------------------------------------------------------
// startMotion()
// id is the asset if of the animation to start
// time_offset is the offset into the animation at which to start playing
//-----------------------------------------------------------------------------
// virtual
bool LLVOAvatar::startMotion(const LLUUID& id, F32 time_offset)
{
    LL_DEBUGS("Motion") << "motion requested " << id.asString() << " " << gAnimLibrary.animationName(id) << LL_ENDL;

    LLUUID remap_id = remapMotionID(id);

    if (remap_id != id)
    {
        LL_DEBUGS("Motion") << "motion resultant " << remap_id.asString() << " " << gAnimLibrary.animationName(remap_id) << LL_ENDL;
    }

    if (isSelf() && remap_id == ANIM_AGENT_AWAY)
    {
        gAgent.setAFK();
    }

    // Client AO (#668): a built-in state motion its replacement already covers
    // is not played on our avatar at all. Reported as started, so the playing
    // set stays truthful and the matching stop still arrives.
    if (isSelf() && SLAOEngine::instanceExists() && SLAOEngine::instance().interceptStockStart(id))
    {
        return true;
    }

    return LLCharacter::startMotion(remap_id, time_offset);
}

//-----------------------------------------------------------------------------
// stopMotion()
//-----------------------------------------------------------------------------
// virtual
bool LLVOAvatar::stopMotion(const LLUUID& id, bool stop_immediate)
{
    LL_DEBUGS("Motion") << "Motion requested " << id.asString() << " " << gAnimLibrary.animationName(id) << LL_ENDL;

    LLUUID remap_id = remapMotionID(id);

    if (remap_id != id)
    {
        LL_DEBUGS("Motion") << "motion resultant " << remap_id.asString() << " " << gAnimLibrary.animationName(remap_id) << LL_ENDL;
    }

    if (isSelf())
    {
        gAgent.onAnimStop(remap_id);
        if (SLAOEngine::instanceExists())
        {
            SLAOEngine::instance().onStockStop(id);
        }
    }

    return LLCharacter::stopMotion(remap_id, stop_immediate);
}

//-----------------------------------------------------------------------------
// hasMotionFromSource()
//-----------------------------------------------------------------------------
// virtual
bool LLVOAvatar::hasMotionFromSource(const LLUUID& source_id)
{
    return false;
}

//-----------------------------------------------------------------------------
// stopMotionFromSource()
//-----------------------------------------------------------------------------
// virtual
void LLVOAvatar::stopMotionFromSource(const LLUUID& source_id)
{
}

//-----------------------------------------------------------------------------
// addDebugText()
//-----------------------------------------------------------------------------
// virtual
void LLVOAvatar::addDebugText(const std::string& text)
{
    mDebugText.append(1, '\n');
    mDebugText.append(text);
}

//-----------------------------------------------------------------------------
// getDebugName()
//-----------------------------------------------------------------------------
// virtual
std::string LLVOAvatar::getDebugName() const
{
#if LL_RELEASE_WITH_DEBUG_INFO
    return getFullname();
#else
    return getID().asString();
#endif // LL_RELEASE_WITH_DEBUG_INFO
}

//-----------------------------------------------------------------------------
// getID()
//-----------------------------------------------------------------------------
// virtual
const LLUUID& LLVOAvatar::getID() const
{
    return mID;
}

//-----------------------------------------------------------------------------
// getJoint()
//-----------------------------------------------------------------------------
// RN: avatar joints are multi-rooted to include screen-based attachments
// virtual
LLJoint* LLVOAvatar::getJoint(std::string_view name)
{
    joint_map_t::iterator iter = mJointMap.find(name);

    LLJoint* jointp = nullptr;

    // Once the skeleton is built a cached nullptr is a known miss; before that, search again.
    if (iter == mJointMap.end() || (iter->second == nullptr && !mIsBuilt))
    {   //search for joint and cache found joint in lookup table
        if (mJointAliasMap.empty())
        {
            getJointAliases();
        }
        joint_alias_map_t::const_iterator alias_iter = mJointAliasMap.find(name);
        std::string canonical_name;
        if (alias_iter != mJointAliasMap.end())
        {
            canonical_name = alias_iter->second;
        }
        else
        {
            canonical_name = name;
        }
        jointp = mRoot->findJoint(canonical_name);
        mJointMap[std::string(name)] = jointp;
    }
    else
    {   //return cached pointer
        jointp = iter->second;
    }

#ifndef LL_RELEASE_FOR_DOWNLOAD
    if (jointp && jointp->getName()!="mScreen" && jointp->getName()!="mRoot")
    {
        llassert(getJoint(jointp->getJointNum())==jointp);
    }
#endif
    return jointp;
}

LLJoint *LLVOAvatar::getJoint( S32 joint_num )
{
    LLJoint *pJoint = NULL;
    if (joint_num >= 0)
    {
        if (joint_num < mNumBones)
        {
            pJoint = mSkeleton[joint_num];
        }
        else if (joint_num < mNumBones + mNumCollisionVolumes)
        {
            S32 collision_id = joint_num - mNumBones;
            pJoint = &mCollisionVolumes[collision_id];
        }
        else
        {
            // Attachment IDs start at 1
            S32 attachment_id = joint_num - (mNumBones + mNumCollisionVolumes) + 1;
            attachment_map_t::iterator iter = mAttachmentPoints.find(attachment_id);
            if (iter != mAttachmentPoints.end())
            {
                pJoint = iter->second;
            }
        }
    }

    llassert(!pJoint || pJoint->getJointNum() == joint_num);
    return pJoint;
}

void LLVOAvatar::initAllJoints()
{
    getJointAliases();
    for (auto& alias : mJointAliasMap)
    {
        mJointMap[alias.first] = mRoot->findJoint(alias.second);
    }
    // ignore mScreen and mRoot
}

//-----------------------------------------------------------------------------
// getRiggedMeshID
//
// If viewer object is a rigged mesh, set the mesh id and return true.
// Otherwise, null out the id and return false.
//-----------------------------------------------------------------------------
// static
bool LLVOAvatar::getRiggedMeshID(LLViewerObject* pVO, LLUUID& mesh_id)
{
    mesh_id.setNull();

    //If a VO has a skin that we'll reset the joint positions to their default
    if ( pVO && pVO->mDrawable )
    {
        LLVOVolume* pVObj = pVO->mDrawable->getVOVolume();
        if ( pVObj )
        {
            const LLMeshSkinInfo* pSkinData = pVObj->getSkinInfo();
            if (pSkinData
                && pSkinData->mJointNames.size() > JOINT_COUNT_REQUIRED_FOR_FULLRIG // full rig
                && pSkinData->mAlternateBindMatrix.size() > 0 )
                    {
                        mesh_id = pSkinData->mMeshID;
                        return true;
                    }
        }
    }
    return false;
}

bool LLVOAvatar::jointIsRiggedTo(const LLJoint *joint) const
{
    if (joint)
    {
        const LLJointRiggingInfoTab& tab = mJointRiggingInfoTab;
        S32 joint_num = joint->getJointNum();
        if (joint_num < tab.size() && tab[joint_num].isRiggedTo())
            {
                return true;
            }
        }
    return false;
}

void LLVOAvatar::clearAttachmentOverrides()
{

    for (S32 i=0; i<LL_CHARACTER_MAX_ANIMATED_JOINTS; i++)
    {
        LLJoint *pJoint = getJoint(i);
        if (pJoint)
        {
            pJoint->clearAttachmentPosOverrides();
            pJoint->clearAttachmentScaleOverrides();
        }
    }

    if (mPelvisFixups.count()>0)
    {
        mPelvisFixups.clear();
        LLJoint* pJointPelvis = getJoint("mPelvis");
        if (pJointPelvis)
    {
            pJointPelvis->setPosition( LLVector3( 0.0f, 0.0f, 0.0f) );
        }
        postPelvisSetRecalc();
    }

    mActiveOverrideMeshes.clear();
    onActiveOverrideMeshesChanged();
}

//-----------------------------------------------------------------------------
// rebuildAttachmentOverrides
//-----------------------------------------------------------------------------
void LLVOAvatar::rebuildAttachmentOverrides()
{

    LL_DEBUGS("AnimatedObjects") << "rebuilding" << LL_ENDL;

    clearAttachmentOverrides();

    // Handle the case that we're resetting the skeleton of an animated object.
    LLControlAvatar *control_av = dynamic_cast<LLControlAvatar*>(this);
    if (control_av)
    {
        LLVOVolume *volp = control_av->mRootVolp;
        if (volp)
        {
            LL_DEBUGS("Avatar") << volp->getID() << " adding attachment overrides for root vol, prim count "
                                << (S32) (1+volp->numChildren()) << LL_ENDL;
            addAttachmentOverridesForObject(volp);
        }
    }

    // Attached objects
    for (attachment_map_t::iterator iter = mAttachmentPoints.begin();
         iter != mAttachmentPoints.end();
         ++iter)
    {
        LLViewerJointAttachment *attachment_pt = (*iter).second;
        if (attachment_pt)
        {
            for (LLViewerJointAttachment::attachedobjs_vec_t::iterator at_it = attachment_pt->mAttachedObjects.begin();
                 at_it != attachment_pt->mAttachedObjects.end(); ++at_it)
            {
                LLViewerObject *vo = at_it->get();
                // Attached animated objects affect joints in their control
                // avs, not the avs to which they are attached.
                if (vo && !vo->isAnimatedObject())
                {
                    addAttachmentOverridesForObject(vo);
                }
            }
        }
    }
}

//-----------------------------------------------------------------------------
// updateAttachmentOverrides
//
// This is intended to give the same results as
// rebuildAttachmentOverrides(), while avoiding redundant work.
// -----------------------------------------------------------------------------
void LLVOAvatar::updateAttachmentOverrides()
{

    LL_DEBUGS("AnimatedObjects") << "updating" << LL_ENDL;

    std::set<LLUUID> meshes_seen;

    // Handle the case that we're updating the skeleton of an animated object.
    LLControlAvatar *control_av = dynamic_cast<LLControlAvatar*>(this);
    if (control_av)
    {
        LLVOVolume *volp = control_av->mRootVolp;
        if (volp)
        {
            LL_DEBUGS("Avatar") << volp->getID() << " adding attachment overrides for root vol, prim count "
                                << (S32) (1+volp->numChildren()) << LL_ENDL;
            addAttachmentOverridesForObject(volp, &meshes_seen);
        }
    }

    // Attached objects
    for (attachment_map_t::iterator iter = mAttachmentPoints.begin();
         iter != mAttachmentPoints.end();
         ++iter)
    {
        LLViewerJointAttachment *attachment_pt = (*iter).second;
        if (attachment_pt)
        {
            for (LLViewerJointAttachment::attachedobjs_vec_t::iterator at_it = attachment_pt->mAttachedObjects.begin();
                 at_it != attachment_pt->mAttachedObjects.end(); ++at_it)
            {
                LLViewerObject *vo = at_it->get();
                // Attached animated objects affect joints in their control
                // avs, not the avs to which they are attached.
                if (vo && !vo->isAnimatedObject())
                {
                    addAttachmentOverridesForObject(vo, &meshes_seen);
                }
            }
        }
    }
    // Remove meshes that are no longer present on the skeleton

    // have to work with a copy because removeAttachmentOverrides() will change mActiveOverrideMeshes.
    std::set<LLUUID> active_override_meshes = mActiveOverrideMeshes;
    for (std::set<LLUUID>::iterator it = active_override_meshes.begin(); it != active_override_meshes.end(); ++it)
    {
        if (meshes_seen.find(*it) == meshes_seen.end())
        {
            removeAttachmentOverridesForObject(*it);
        }
    }


#ifdef ATTACHMENT_OVERRIDE_VALIDATION
    {
        std::vector<LLVector3OverrideMap> pos_overrides_by_joint;
        std::vector<LLVector3OverrideMap> scale_overrides_by_joint;
        LLVector3OverrideMap pelvis_fixups;

        // Capture snapshot of override state after update
        for (S32 joint_num = 0; joint_num < LL_CHARACTER_MAX_ANIMATED_JOINTS; joint_num++)
        {
            LLVector3OverrideMap pos_overrides;
            LLJoint *joint = getJoint(joint_num);
            if (joint)
            {
                pos_overrides_by_joint.push_back(joint->m_attachmentPosOverrides);
                scale_overrides_by_joint.push_back(joint->m_attachmentScaleOverrides);
            }
            else
            {
                // No joint, use default constructed empty maps
                pos_overrides_by_joint.push_back(LLVector3OverrideMap());
                scale_overrides_by_joint.push_back(LLVector3OverrideMap());
            }
        }
        pelvis_fixups = mPelvisFixups;
        //dumpArchetypeXML(getDebugName() + "_paranoid_updated");

        // Rebuild and compare
        rebuildAttachmentOverrides();
        //dumpArchetypeXML(getDebugName() + "_paranoid_rebuilt");
        bool mismatched = false;
        for (S32 joint_num = 0; joint_num < LL_CHARACTER_MAX_ANIMATED_JOINTS; joint_num++)
        {
            LLJoint *joint = getJoint(joint_num);
            if (joint)
            {
                if (pos_overrides_by_joint[joint_num] != joint->m_attachmentPosOverrides)
                {
                    mismatched = true;
                }
                if (scale_overrides_by_joint[joint_num] != joint->m_attachmentScaleOverrides)
                {
                    mismatched = true;
            }
        }
    }
        if (pelvis_fixups != mPelvisFixups)
        {
            mismatched = true;
        }
        if (mismatched)
        {
            LL_WARNS() << "MISMATCHED ATTACHMENT OVERRIDES" << LL_ENDL;
        }
    }
#endif
}

void LLVOAvatar::notifyAttachmentMeshLoaded()
{
    if (!isFullyLoaded())
    {
        // We just received mesh or skin info
        // Reset timer to wait for more potential meshes or changes
        mFullyLoadedTimer.reset();
    }
}

//-----------------------------------------------------------------------------
// addAttachmentOverridesForObject
//-----------------------------------------------------------------------------
void LLVOAvatar::addAttachmentOverridesForObject(LLViewerObject *vo, std::set<LLUUID>* meshes_seen, bool recursive)
{
    if (vo->getAvatar() != this && vo->getAvatarAncestor() != this)
    {
        LL_WARNS("Avatar") << "called with invalid avatar" << LL_ENDL;
        return;
    }

    if (getOverallAppearance() != AOA_NORMAL)
    {
        return;
    }

    LL_DEBUGS("AnimatedObjects") << "adding" << LL_ENDL;

    // Process all children
    if (recursive)
    {
    LLViewerObject::const_child_list_t& children = vo->getChildren();
    for (LLViewerObject::const_child_list_t::const_iterator it = children.begin();
         it != children.end(); ++it)
    {
        LLViewerObject *childp = *it;
            addAttachmentOverridesForObject(childp, meshes_seen, true);
        }
    }

    LLVOVolume *vobj = dynamic_cast<LLVOVolume*>(vo);
    bool pelvisGotSet = false;

    if (!vobj)
    {
        return;
    }

    LLViewerObject *root_object = (LLViewerObject*)vobj->getRoot();
    LL_DEBUGS("AnimatedObjects") << "trying to add attachment overrides for root object " << root_object->getID() << " prim is " << vobj << LL_ENDL;
    if (vobj->isMesh() &&
        ((vobj->getVolume() && !vobj->getVolume()->isMeshAssetLoaded()) || !gMeshRepo.meshRezEnabled()))
    {
        LL_DEBUGS("AnimatedObjects") << "failed to add attachment overrides for root object " << root_object->getID() << " mesh asset not loaded" << LL_ENDL;
        return;
    }
    const LLMeshSkinInfo*  pSkinData = vobj->getSkinInfo();

    if ( vobj && vobj->isMesh() && pSkinData )
    {
        const unsigned int bindCnt = static_cast<unsigned int>(pSkinData->mAlternateBindMatrix.size());
        const unsigned int jointCnt = static_cast<unsigned int>(pSkinData->mJointNames.size());
        if ((bindCnt > 0) && (bindCnt != jointCnt))
        {
            LL_WARNS_ONCE() << "invalid mesh, bindCnt " << bindCnt << "!= jointCnt " << jointCnt << ", joint overrides will be ignored." << LL_ENDL;
        }
        if ((bindCnt > 0) && (bindCnt == jointCnt))
        {
            const F32 pelvisZOffset = pSkinData->mPelvisOffset;
            const LLUUID& mesh_id = pSkinData->mMeshID;

            if (meshes_seen)
            {
                meshes_seen->insert(mesh_id);
            }
            bool mesh_overrides_loaded = (mActiveOverrideMeshes.find(mesh_id) != mActiveOverrideMeshes.end());
            if (mesh_overrides_loaded)
            {
                LL_DEBUGS("AnimatedObjects") << "skipping add attachment overrides for " << mesh_id
                                             << " to root object " << root_object->getID()
                                             << ", already loaded"
                                             << LL_ENDL;
            }
            else
            {
                LL_DEBUGS("AnimatedObjects") << "adding attachment overrides for " << mesh_id
                                             << " to root object " << root_object->getID() << LL_ENDL;
            }
            bool fullRig = jointCnt >= JOINT_COUNT_REQUIRED_FOR_FULLRIG;
            if ( fullRig && !mesh_overrides_loaded )
            {
                // The override calls use this string only in LL_DEBUGS output.
#ifdef ENABLE_DEBUG_MACRO
                const std::string av_str = avString();
#else
                const std::string av_str;
#endif
                for (unsigned int i = 0; i < jointCnt; ++i)
                {
                    std::string lookingForJoint = pSkinData->mJointNames[i].c_str();
                    LLJoint* pJoint = getJoint( lookingForJoint );
                    if (pJoint)
                    {
                        const LLVector3& jointPos = LLVector3(pSkinData->mAlternateBindMatrix[i].getTranslation());
                        if (pJoint->aboveJointPosThreshold(jointPos))
                        {
                            bool override_changed;
                            pJoint->addAttachmentPosOverride( jointPos, mesh_id, av_str, override_changed );

                            if (override_changed)
                            {
                                //If joint is a pelvis then handle old/new pelvis to foot values
                                if ( lookingForJoint == "mPelvis" )
                                {
                                    pelvisGotSet = true;
                                }
                            }
                            if (pSkinData->mLockScaleIfJointPosition)
                            {
                                // Note that unlike positions, there's no threshold check here,
                                // just a lock at the default value.
                                pJoint->addAttachmentScaleOverride(pJoint->getDefaultScale(), mesh_id, av_str);
                            }
                        }
                    }
                }

                if (pelvisZOffset != 0.0F)
                {
                    F32 pelvis_fixup_before;
                    bool has_fixup_before =  hasPelvisFixup(pelvis_fixup_before);
                    addPelvisFixup( pelvisZOffset, mesh_id );
                    F32 pelvis_fixup_after;
                    hasPelvisFixup(pelvis_fixup_after); // Don't have to check bool here because we just added it...
                    if (!has_fixup_before || (pelvis_fixup_before != pelvis_fixup_after))
                    {
                        pelvisGotSet = true;
                    }

                }
                mActiveOverrideMeshes.insert(mesh_id);
                onActiveOverrideMeshesChanged();
            }
        }
    }
    else
    {
        LL_DEBUGS("AnimatedObjects") << "failed to add attachment overrides for root object " << root_object->getID() << " not mesh or no pSkinData" << LL_ENDL;
    }

    //Rebuild body data if we altered joints/pelvis
    if ( pelvisGotSet )
    {
        postPelvisSetRecalc();
    }
}

//-----------------------------------------------------------------------------
// getAttachmentOverrideNames
//-----------------------------------------------------------------------------
void LLVOAvatar::getAttachmentOverrideNames(std::set<std::string>& pos_names, std::set<std::string>& scale_names) const
{
    LLVector3 pos;
    LLVector3 scale;
    LLUUID mesh_id;

    // Bones
    for (avatar_joint_list_t::const_iterator iter = mSkeleton.begin();
         iter != mSkeleton.end(); ++iter)
    {
        const LLJoint* pJoint = (*iter);
        if (pJoint && pJoint->hasAttachmentPosOverride(pos,mesh_id))
        {
            pos_names.insert(pJoint->getName());
        }
        if (pJoint && pJoint->hasAttachmentScaleOverride(scale,mesh_id))
        {
            scale_names.insert(pJoint->getName());
        }
    }

    // Attachment points
    for (attachment_map_t::const_iterator iter = mAttachmentPoints.begin();
         iter != mAttachmentPoints.end();
         ++iter)
    {
        const LLViewerJointAttachment *attachment_pt = (*iter).second;
        if (attachment_pt && attachment_pt->hasAttachmentPosOverride(pos,mesh_id))
        {
            pos_names.insert(attachment_pt->getName());
        }
        // Attachment points don't have scales.
    }
}

//-----------------------------------------------------------------------------
// showAttachmentOverrides
//-----------------------------------------------------------------------------
void LLVOAvatar::showAttachmentOverrides(bool verbose) const
{
    std::set<std::string> pos_names, scale_names;
    getAttachmentOverrideNames(pos_names, scale_names);
    if (pos_names.size())
    {
        std::stringstream ss;
        std::copy(pos_names.begin(), pos_names.end(), std::ostream_iterator<std::string>(ss, ","));
        LL_INFOS() << avString() << " attachment positions defined for joints: " << ss.str() << "\n" << LL_ENDL;
    }
    else
    {
        LL_DEBUGS("Avatar") << avString() << " no attachment positions defined for any joints" << "\n" << LL_ENDL;
    }

    if (scale_names.size())
    {
        std::stringstream ss;
        std::copy(scale_names.begin(), scale_names.end(), std::ostream_iterator<std::string>(ss, ","));
        LL_INFOS() << getDebugName() << " attachment scales defined for joints: " << ss.str() << "\n" << LL_ENDL;
    }
    else
    {
        LL_INFOS() << getDebugName() << " no attachment scales defined for any joints" << "\n" << LL_ENDL;
    }

    if (!verbose)
    {
        return;
    }

    LLVector3 pos, scale;
    LLUUID mesh_id;
    S32 count = 0;

    // Bones
    for (avatar_joint_list_t::const_iterator iter = mSkeleton.begin();
         iter != mSkeleton.end(); ++iter)
    {
        const LLJoint* pJoint = (*iter);
        if (pJoint && pJoint->hasAttachmentPosOverride(pos,mesh_id))
        {
            pJoint->showAttachmentPosOverrides(getFullname());
            count++;
        }
        if (pJoint && pJoint->hasAttachmentScaleOverride(scale,mesh_id))
        {
            pJoint->showAttachmentScaleOverrides(getFullname());
            count++;
        }
    }

    // Attachment points
    for (attachment_map_t::const_iterator iter = mAttachmentPoints.begin();
         iter != mAttachmentPoints.end();
         ++iter)
    {
        const LLViewerJointAttachment *attachment_pt = (*iter).second;
        if (attachment_pt && attachment_pt->hasAttachmentPosOverride(pos,mesh_id))
        {
            attachment_pt->showAttachmentPosOverrides(getFullname());
            count++;
        }
    }

    if (count)
    {
        LL_DEBUGS("Avatar") << avString() << " end of pos, scale overrides" << LL_ENDL;
        LL_DEBUGS("Avatar") << "=================================" << LL_ENDL;
    }
}

//-----------------------------------------------------------------------------
// removeAttachmentOverridesForObject
//-----------------------------------------------------------------------------
void LLVOAvatar::removeAttachmentOverridesForObject(LLViewerObject *vo)
{
    if (vo->getAvatar() != this && vo->getAvatarAncestor() != this)
    {
        LL_WARNS("Avatar") << "called with invalid avatar" << LL_ENDL;
        return;
    }

    // Process all children
    LLViewerObject::const_child_list_t& children = vo->getChildren();
    for (LLViewerObject::const_child_list_t::const_iterator it = children.begin();
         it != children.end(); ++it)
    {
        LLViewerObject *childp = *it;
        removeAttachmentOverridesForObject(childp);
    }

    // Process self.
    LLUUID mesh_id;
    if (getRiggedMeshID(vo,mesh_id))
    {
        removeAttachmentOverridesForObject(mesh_id);
    }
}

//-----------------------------------------------------------------------------
// removeAttachmentOverridesForObject
//-----------------------------------------------------------------------------
void LLVOAvatar::removeAttachmentOverridesForObject(const LLUUID& mesh_id)
{
    LLJoint* pJointPelvis = getJoint("mPelvis");
    const std::string av_string = avString();
    for (S32 joint_num = 0; joint_num < LL_CHARACTER_MAX_ANIMATED_JOINTS; joint_num++)
    {
        LLJoint *pJoint = getJoint(joint_num);
        if (pJoint)
        {
            bool dummy; // unused
            pJoint->removeAttachmentPosOverride(mesh_id, av_string, dummy);
            pJoint->removeAttachmentScaleOverride(mesh_id, av_string);
        }
        if (pJoint && pJoint == pJointPelvis)
        {
            removePelvisFixup(mesh_id);
            // SL-315
            pJoint->setPosition(LLVector3( 0.0f, 0.0f, 0.0f));
        }
    }

    postPelvisSetRecalc();

    mActiveOverrideMeshes.erase(mesh_id);
    onActiveOverrideMeshesChanged();
}

//-----------------------------------------------------------------------------
// getCharacterPosition()
//-----------------------------------------------------------------------------
LLVector3 LLVOAvatar::getCharacterPosition()
{
    if (mDrawable.notNull())
    {
        return mDrawable->getPositionAgent();
    }
    else
    {
        return getPositionAgent();
    }
}

//-----------------------------------------------------------------------------
// LLVOAvatar::getCharacterRotation()
//-----------------------------------------------------------------------------
LLQuaternion LLVOAvatar::getCharacterRotation()
{
    return getRotation();
}

//-----------------------------------------------------------------------------
// LLVOAvatar::getCharacterVelocity()
//-----------------------------------------------------------------------------
LLVector3 LLVOAvatar::getCharacterVelocity()
{
    return getVelocity() - mStepObjectVelocity;
}

//-----------------------------------------------------------------------------
// LLVOAvatar::getCharacterAngularVelocity()
//-----------------------------------------------------------------------------
LLVector3 LLVOAvatar::getCharacterAngularVelocity()
{
    return getAngularVelocity();
}

//-----------------------------------------------------------------------------
// LLVOAvatar::getGround()
//-----------------------------------------------------------------------------
void LLVOAvatar::getGround(const LLVector3 &in_pos_agent, LLVector3 &out_pos_agent, LLVector3 &outNorm)
{
    LLVector3d z_vec(0.0f, 0.0f, 1.0f);
    LLVector3d p0_global, p1_global;

    if (isUIAvatar())
    {
        outNorm.setVec(z_vec);
        out_pos_agent = in_pos_agent;
        return;
    }

    p0_global = gAgent.getPosGlobalFromAgent(in_pos_agent) + z_vec;
    p1_global = gAgent.getPosGlobalFromAgent(in_pos_agent) - z_vec;
    LLViewerObject *obj;
    LLVector3d out_pos_global;
    LLWorld::getInstance()->resolveStepHeightGlobal(this, p0_global, p1_global, out_pos_global, outNorm, &obj);
    out_pos_agent = gAgent.getPosAgentFromGlobal(out_pos_global);
}

//-----------------------------------------------------------------------------
// LLVOAvatar::getTimeDilation()
//-----------------------------------------------------------------------------
F32 LLVOAvatar::getTimeDilation()
{
    return mRegionp ? mRegionp->getTimeDilation() : 1.f;
}

//-----------------------------------------------------------------------------
// LLVOAvatar::getPixelArea()
//-----------------------------------------------------------------------------
F32 LLVOAvatar::getPixelArea() const
{
    if (isUIAvatar())
    {
        return 100000.f;
    }
    return mPixelArea;
}

//-----------------------------------------------------------------------------
// LLVOAvatar::getPosGlobalFromAgent()
//-----------------------------------------------------------------------------
LLVector3d  LLVOAvatar::getPosGlobalFromAgent(const LLVector3 &position)
{
    return gAgent.getPosGlobalFromAgent(position);
}

//-----------------------------------------------------------------------------
// getPosAgentFromGlobal()
//-----------------------------------------------------------------------------
LLVector3   LLVOAvatar::getPosAgentFromGlobal(const LLVector3d &position)
{
    return gAgent.getPosAgentFromGlobal(position);
}

//-----------------------------------------------------------------------------
// requestStopMotion()
//-----------------------------------------------------------------------------
// virtual
void LLVOAvatar::requestStopMotion( LLMotion* motion )
{
    // Only agent avatars should handle the stop motion notifications.
}

//-----------------------------------------------------------------------------
// loadSkeletonNode(): loads <skeleton> node from XML tree
//-----------------------------------------------------------------------------
//virtual
bool LLVOAvatar::loadSkeletonNode ()
{
    if (!LLAvatarAppearance::loadSkeletonNode())
    {
        return false;
    }

    bool ignore_hud_joints = false;
    initAttachmentPoints(ignore_hud_joints);

    return true;
}

//-----------------------------------------------------------------------------
// initAttachmentPoints(): creates attachment points if needed, sets state based on avatar_lad.xml.
//-----------------------------------------------------------------------------
void LLVOAvatar::initAttachmentPoints(bool ignore_hud_joints)
{
    LLAvatarXmlInfo::attachment_info_list_t::iterator iter;
    for (iter = sAvatarXmlInfo->mAttachmentInfoList.begin();
         iter != sAvatarXmlInfo->mAttachmentInfoList.end();
         ++iter)
    {
        LLAvatarXmlInfo::LLAvatarAttachmentInfo *info = *iter;
        if (info->mIsHUDAttachment && (!isSelf() || ignore_hud_joints))
        {
            //don't process hud joint for other avatars.
            continue;
        }

        S32 attachmentID = info->mAttachmentID;
        if (attachmentID < 1 || attachmentID > 255)
        {
            LL_WARNS() << "Attachment point out of range [1-255]: " << attachmentID << " on attachment point " << info->mName << LL_ENDL;
            continue;
        }

        LLViewerJointAttachment* attachment = NULL;
        bool newly_created = false;
        if (mAttachmentPoints.find(attachmentID) == mAttachmentPoints.end())
        {
            attachment = new LLViewerJointAttachment();
            newly_created = true;
        }
        else
        {
            attachment = mAttachmentPoints[attachmentID];
        }

        attachment->setName(info->mName);
        LLJoint *parent_joint = getJoint(info->mJointName);
        if (!parent_joint)
        {
            // If the intended parent for attachment point is unavailable, avatar_lad.xml is corrupt.
            LL_WARNS() << "No parent joint by name " << info->mJointName << " found for attachment point " << info->mName << LL_ENDL;
            LL_ERRS() << "Invalid avatar_lad.xml file" << LL_ENDL;
        }

        if (info->mHasPosition)
        {
            attachment->setOriginalPosition(info->mPosition);
            attachment->setDefaultPosition(info->mPosition);
        }

        if (info->mHasRotation)
        {
            LLQuaternion rotation;
            rotation.setQuat(info->mRotationEuler.mV[VX] * DEG_TO_RAD,
                             info->mRotationEuler.mV[VY] * DEG_TO_RAD,
                             info->mRotationEuler.mV[VZ] * DEG_TO_RAD);
            attachment->setRotation(rotation);
        }

        int group = info->mGroup;
        if (group >= 0)
        {
            if (group < 0 || group > 9)
            {
                LL_WARNS() << "Invalid group number (" << group << ") for attachment point " << info->mName << LL_ENDL;
            }
            else
            {
                attachment->setGroup(group);
            }
        }

        attachment->setPieSlice(info->mPieMenuSlice);
        attachment->setVisibleInFirstPerson(info->mVisibleFirstPerson);
        attachment->setIsHUDAttachment(info->mIsHUDAttachment);
        // attachment can potentially be animated, needs a number.
        attachment->setJointNum(mNumBones + mNumCollisionVolumes + attachmentID - 1);

        if (newly_created)
        {
            mAttachmentPoints[attachmentID] = attachment;
            mOccupiedAttachGen = 0;   // occupied-point cache: map changed

            // now add attachment joint
            parent_joint->addChild(attachment);
        }
    }
}

//-----------------------------------------------------------------------------
// updateVisualParams()
//-----------------------------------------------------------------------------
void LLVOAvatar::updateVisualParams()
{
    VK_HITCH_SCOPE("av_visualParams");   // nested in avatarCharacter when a motion calls it
    // #329: the "male" param by pointer (getVisualParamWeight(name) lower-cases and
    // hashes the name every call); re-resolved if the parameter set changes, and read
    // by name (with LL's warning) if it does not exist.
    if (mMaleParamGen != getVisualParamGeneration())
    {
        mMaleParam = getVisualParam("male");
        mMaleParamGen = getVisualParamGeneration();
    }
    const F32 male_weight = mMaleParam ? mMaleParam->getWeight() : getVisualParamWeight("male");
    ESex avatar_sex = (male_weight > 0.5f) ? SEX_MALE : SEX_FEMALE;
    if (getSex() != avatar_sex)
    {
        if (mIsSitting && findMotion(avatar_sex == SEX_MALE ? ANIM_AGENT_SIT_FEMALE : ANIM_AGENT_SIT) != NULL)
        {
            // In some cases of gender change server changes sit motion with motion message,
            // but in case of some avatars (legacy?) there is no update from server side,
            // likely because server doesn't know about difference between motions
            // (female and male sit ids are same server side, so it is likely unaware that it
            // need to send update)
            // Make sure motion is up to date
            stopMotion(ANIM_AGENT_SIT);
            setSex(avatar_sex);
            startMotion(ANIM_AGENT_SIT);
        }
        else
        {
            setSex(avatar_sex);
        }
    }

    LLCharacter::updateVisualParams();

    if (mLastSkeletonSerialNum != mSkeletonSerialNum)
    {
        computeBodySize();
        mLastSkeletonSerialNum = mSkeletonSerialNum;
        mRoot->updateWorldMatrixChildren();
    }

    dirtyMesh();
    updateHeadOffset();
}

void LLVOAvatar::setCorrectedPixelArea(F32 area)
{
    // We always want to look good to ourselves
    if (isSelf())
    {
        area = llmax(area, F32(getTexImageSize() / 16));
    }

    setPixelArea(area);
}

//-----------------------------------------------------------------------------
// isActive()
//-----------------------------------------------------------------------------
bool LLVOAvatar::isActive() const
{
    return true;
}

//-----------------------------------------------------------------------------
// setPixelAreaAndAngle()
//-----------------------------------------------------------------------------
void LLVOAvatar::setPixelAreaAndAngle(LLAgent &agent)
{
    if (mDrawable.isNull())
    {
        return;
    }

    const LLVector4a* ext = mDrawable->getSpatialExtents();
    LLVector4a center;
    center.setAdd(ext[1], ext[0]);
    center.mul(0.5f);
    LLVector4a size;
    size.setSub(ext[1], ext[0]);
    size.mul(0.5f);

    mImpostorPixelArea = LLPipeline::calcPixelArea(center, size, *LLViewerCamera::getInstance());
    setCorrectedPixelArea(mImpostorPixelArea);

    F32 range = mDrawable->mDistanceWRTCamera;

    if (range < 0.001f)     // range == zero
    {
        mAppAngle = 180.f;
    }
    else
    {
        F32 radius = size.getLength3().getF32();
        mAppAngle = (F32) atan2( radius, range) * RAD_TO_DEG;
    }
}

//-----------------------------------------------------------------------------
// updateJointLODs()
//-----------------------------------------------------------------------------
bool LLVOAvatar::updateJointLODs()
{
    const F32 MAX_PIXEL_AREA = 100000000.f;
    F32 lod_factor = (sLODFactor * AVATAR_LOD_TWEAK_RANGE + (1.f - AVATAR_LOD_TWEAK_RANGE));
    F32 avatar_num_min_factor = clamp_rescale(sLODFactor, 0.f, 1.f, 0.25f, 0.6f);
    F32 avatar_num_factor = clamp_rescale((F32)sNumVisibleAvatars, 8, 25, 1.f, avatar_num_min_factor);
    F32 area_scale = 0.16f;

        if (isSelf())
        {
            if(gAgentCamera.cameraCustomizeAvatar() || gAgentCamera.cameraMouselook())
            {
                mAdjustedPixelArea = MAX_PIXEL_AREA;
            }
            else
            {
                mAdjustedPixelArea = mPixelArea*area_scale;
            }
        }
        else if (mIsDummy)
        {
            mAdjustedPixelArea = MAX_PIXEL_AREA;
        }
        else
        {
            // reported avatar pixel area is dependent on avatar render load, based on number of visible avatars
            mAdjustedPixelArea = (F32)mPixelArea * area_scale * lod_factor * lod_factor * avatar_num_factor * avatar_num_factor;
        }

        // now select meshes to render based on adjusted pixel area
        LLViewerJoint* root = dynamic_cast<LLViewerJoint*>(mRoot);
        bool res = false;
        if (root)
        {
            res = root->updateLOD(mAdjustedPixelArea, true);
        }
        if (res)
        {
            sNumLODChangesThisFrame++;
            dirtyMesh(2);
            return true;
        }

    return false;
}

//-----------------------------------------------------------------------------
// createDrawable()
//-----------------------------------------------------------------------------
LLDrawable *LLVOAvatar::createDrawable(LLPipeline *pipeline)
{
    pipeline->allocDrawable(this);
    mDrawable->setLit(false);

    LLFacePool *poolp = (LLFacePool *)gPipeline.getPool(mIsControlAvatar ? LLDrawPool::POOL_CONTROL_AV : LLDrawPool::POOL_AVATAR);

    // Only a single face (one per avatar)
    //this face will be splitted into several if its vertex buffer is too long.
    mDrawable->setState(LLDrawable::ACTIVE);
    mDrawable->addFace(poolp, NULL);
    mDrawable->setRenderType(mIsControlAvatar ? LLPipeline::RENDER_TYPE_CONTROL_AV : LLPipeline::RENDER_TYPE_AVATAR);

    mNumInitFaces = mDrawable->getNumFaces() ;

    dirtyMesh(2);
    return mDrawable;
}


void LLVOAvatar::updateGL()
{
    if (mMeshTexturesDirty)
    {
        LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;
        updateMeshTextures();
        mMeshTexturesDirty = false;
    }
}

//-----------------------------------------------------------------------------
// updateSexDependentLayerSets()
//-----------------------------------------------------------------------------
void LLVOAvatar::updateSexDependentLayerSets()
{
    invalidateComposite( mBakedTextureDatas[BAKED_HEAD].mTexLayerSet);
    invalidateComposite( mBakedTextureDatas[BAKED_UPPER].mTexLayerSet);
    invalidateComposite( mBakedTextureDatas[BAKED_LOWER].mTexLayerSet);
}

//-----------------------------------------------------------------------------
// dirtyMesh()
//-----------------------------------------------------------------------------
void LLVOAvatar::dirtyMesh()
{
    dirtyMesh(1);
}
void LLVOAvatar::dirtyMesh(S32 priority)
{
    mDirtyMesh = llmax(mDirtyMesh, priority);
}

//-----------------------------------------------------------------------------
// getViewerJoint()
//-----------------------------------------------------------------------------
LLViewerJoint*  LLVOAvatar::getViewerJoint(S32 idx)
{
    return dynamic_cast<LLViewerJoint*>(mMeshLOD[idx]);
}

//-----------------------------------------------------------------------------
// hideHair()
//-----------------------------------------------------------------------------
void LLVOAvatar::hideHair()
{
    mMeshLOD[MESH_ID_HAIR]->setVisible(false, true);
}

//-----------------------------------------------------------------------------
// hideSkirt()
//-----------------------------------------------------------------------------
void LLVOAvatar::hideSkirt()
{
    mMeshLOD[MESH_ID_SKIRT]->setVisible(false, true);
}

bool LLVOAvatar::setParent(LLViewerObject* parent)
{
    bool ret ;
    if (parent == NULL)
    {
        getOffObject();
        ret = LLViewerObject::setParent(parent);
        if (isSelf())
        {
            gAgentCamera.resetCamera();
            LL_INFOS("Breadcrumb") << "self stood: unparented, camera reset, frame " << gFrameCount << LL_ENDL;   // #690
        }
    }
    else
    {
        ret = LLViewerObject::setParent(parent);
        if(ret)
        {
            sitOnObject(parent);
        }
    }
    return ret ;
}

void LLVOAvatar::addChild(LLViewerObject *childp)
{
    childp->extractAttachmentItemID(); // find the inventory item this object is associated with.
    if (isSelf())
    {
        const LLUUID& item_id = childp->getAttachmentItemID();
        LLViewerInventoryItem *item = gInventory.getItem(item_id);
        LL_DEBUGS("Avatar") << "ATT attachment child added " << (item ? item->getName() : "UNKNOWN") << " id " << item_id << LL_ENDL;

    }

    LLViewerObject::addChild(childp);
    // #202: a re-worn attachment of a derendered (item-keyed) entry; killed on the next idle
    if ( (!isSelf()) && (LLDerenderList::instanceExists()) )
        LLDerenderList::instance().onAttachmentAdded(this, childp);
    // <GMDX> Attachments get a new UUID on every re-rez and relog: a permanent derender or sound
    // emitter blacklist entry that only wearer + object name can find again asks the sim for the
    // new object's properties (throttled), and the reply re-points the entry.
    GMDXAttachProbe::onAttachmentAdded(this, childp);
    if (childp->mDrawable)
    {
        if (!attachObject(childp))
        {
            LL_WARNS() << "ATT addChild() failed for "
                    << childp->getID()
                    << " item " << childp->getAttachmentItemID()
                    << LL_ENDL;
            // MAINT-3312 backout
            // mPendingAttachment.push_back(childp);
        }
    }
    else
    {
        mPendingAttachment.push_back(childp);
    }
}

void LLVOAvatar::removeChild(LLViewerObject *childp)
{
    LLViewerObject::removeChild(childp);
    if (!detachObject(childp))
    {
        LL_WARNS() << "Calling detach on non-attached object " << LL_ENDL;
    }
}

LLViewerJointAttachment* LLVOAvatar::getTargetAttachmentPoint(LLViewerObject* viewer_object)
{
    S32 attachmentID = ATTACHMENT_ID_FROM_STATE(viewer_object->getAttachmentState());

    // This should never happen unless the server didn't process the attachment point
    // correctly, but putting this check in here to be safe.
    if (attachmentID & ATTACHMENT_ADD)
    {
        LL_WARNS() << "Got an attachment with ATTACHMENT_ADD mask, removing ( attach pt:" << attachmentID << " )" << LL_ENDL;
        attachmentID &= ~ATTACHMENT_ADD;
    }

    LLViewerJointAttachment* attachment = get_if_there(mAttachmentPoints, attachmentID, (LLViewerJointAttachment*)NULL);

    if (!attachment)
    {
        LL_WARNS() << "Object attachment point invalid: " << attachmentID
            << " trying to use 1 (chest)"
            << LL_ENDL;

        attachment = get_if_there(mAttachmentPoints, 1, (LLViewerJointAttachment*)NULL); // Arbitrary using 1 (chest)
        if (attachment)
        {
            LL_WARNS() << "Object attachment point invalid: " << attachmentID
                << " on object " << viewer_object->getID()
                << " attachment item " << viewer_object->getAttachmentItemID()
                << " falling back to 1 (chest)"
                << LL_ENDL;
        }
        else
        {
            LL_WARNS() << "Object attachment point invalid: " << attachmentID
                << " on object " << viewer_object->getID()
                << " attachment item " << viewer_object->getAttachmentItemID()
                << "Unable to use fallback attachment point 1 (chest)"
                << LL_ENDL;
        }
    }

    return attachment;
}

//-----------------------------------------------------------------------------
// attachObject()
//-----------------------------------------------------------------------------
const LLViewerJointAttachment *LLVOAvatar::attachObject(LLViewerObject *viewer_object)
{
    if (isSelf())
    {
        const LLUUID& item_id = viewer_object->getAttachmentItemID();
        LLViewerInventoryItem *item = gInventory.getItem(item_id);
        LL_DEBUGS("Avatar") << "ATT attaching object "
                            << (item ? item->getName() : "UNKNOWN") << " id " << item_id << LL_ENDL;
    }
    LLViewerJointAttachment* attachment = getTargetAttachmentPoint(viewer_object);

    if (!attachment || !attachment->addObject(viewer_object))
    {
        const LLUUID& item_id = viewer_object->getAttachmentItemID();
        LLViewerInventoryItem *item = gInventory.getItem(item_id);
        LL_WARNS("Avatar") << "ATT attach failed "
                           << (item ? item->getName() : "UNKNOWN") << " id " << item_id << LL_ENDL;
        return 0;
    }

    if (!viewer_object->isAnimatedObject())
    {
        updateAttachmentOverrides();
    }

    updateVisualComplexity();

    if (viewer_object->isSelected())
    {
        LLSelectMgr::getInstance()->updateSelectionCenter();
        LLSelectMgr::getInstance()->updatePointAt();
    }

    viewer_object->refreshBakeTexture();


    LLViewerObject::const_child_list_t& child_list = viewer_object->getChildren();
    for (LLViewerObject::child_list_t::const_iterator iter = child_list.begin();
        iter != child_list.end(); ++iter)
    {
        LLViewerObject* objectp = *iter;
        if (objectp)
        {
            objectp->refreshBakeTexture();
        }
    }

    updateMeshVisibility();

    return attachment;
}

//-----------------------------------------------------------------------------
// getNumAttachments()
//-----------------------------------------------------------------------------
U32 LLVOAvatar::getNumAttachments() const
{
    U32 num_attachments = 0;
    for (attachment_map_t::const_iterator iter = mAttachmentPoints.begin();
         iter != mAttachmentPoints.end();
         ++iter)
    {
        const LLViewerJointAttachment *attachment_pt = (*iter).second;
        num_attachments += attachment_pt->getNumObjects();
    }
    return num_attachments;
}

//-----------------------------------------------------------------------------
// getMaxAttachments()
//-----------------------------------------------------------------------------
S32 LLVOAvatar::getMaxAttachments() const
{
    return LLAgentBenefitsMgr::current().getAttachmentLimit();
}

//-----------------------------------------------------------------------------
// canAttachMoreObjects()
// Returns true if we can attach <n> more objects.
//-----------------------------------------------------------------------------
bool LLVOAvatar::canAttachMoreObjects(U32 n) const
{
    return (getNumAttachments() + n) <= (U32)getMaxAttachments();
}

//-----------------------------------------------------------------------------
// getNumAnimatedObjectAttachments()
//-----------------------------------------------------------------------------
U32 LLVOAvatar::getNumAnimatedObjectAttachments() const
{
    U32 num_attachments = 0;
    for (attachment_map_t::const_iterator iter = mAttachmentPoints.begin();
         iter != mAttachmentPoints.end();
         ++iter)
    {
        const LLViewerJointAttachment *attachment_pt = (*iter).second;
        num_attachments += attachment_pt->getNumAnimatedObjects();
    }
    return num_attachments;
}

//-----------------------------------------------------------------------------
// getMaxAnimatedObjectAttachments()
// Gets from simulator feature if available, otherwise 0.
//-----------------------------------------------------------------------------
S32 LLVOAvatar::getMaxAnimatedObjectAttachments() const
{
    return LLAgentBenefitsMgr::current().getAnimatedObjectLimit();
}

//-----------------------------------------------------------------------------
// canAttachMoreAnimatedObjects()
// Returns true if we can attach <n> more animated objects.
//-----------------------------------------------------------------------------
bool LLVOAvatar::canAttachMoreAnimatedObjects(U32 n) const
{
    return (getNumAnimatedObjectAttachments() + n) <= (U32)getMaxAnimatedObjectAttachments();
}

//-----------------------------------------------------------------------------
// lazyAttach()
//-----------------------------------------------------------------------------
void LLVOAvatar::lazyAttach()
{
    std::vector<LLPointer<LLViewerObject> > still_pending;

    for (U32 i = 0; i < mPendingAttachment.size(); i++)
    {
        LLPointer<LLViewerObject> cur_attachment = mPendingAttachment[i];
        // Object might have died while we were waiting for drawable
        if (!cur_attachment->isDead())
        {
            if (cur_attachment->mDrawable)
            {
                if (isSelf())
                {
                    const LLUUID& item_id = cur_attachment->getAttachmentItemID();
                    LLViewerInventoryItem *item = gInventory.getItem(item_id);
                    LL_DEBUGS("Avatar") << "ATT attaching object "
                        << (item ? item->getName() : "UNKNOWN") << " id " << item_id << LL_ENDL;
                }
                if (!attachObject(cur_attachment))
                {   // Drop it
                    LL_WARNS() << "attachObject() failed for "
                        << cur_attachment->getID()
                        << " item " << cur_attachment->getAttachmentItemID()
                        << LL_ENDL;
                    // MAINT-3312 backout
                    //still_pending.push_back(cur_attachment);
                }
            }
            else
            {
                still_pending.push_back(cur_attachment);
            }
        }
    }

    mPendingAttachment = still_pending;
}

void LLVOAvatar::resetHUDAttachments()
{

    for (attachment_map_t::iterator iter = mAttachmentPoints.begin();
         iter != mAttachmentPoints.end();
         ++iter)
    {
        LLViewerJointAttachment* attachment = iter->second;
        if (attachment->getIsHUDAttachment())
        {
            for (LLViewerJointAttachment::attachedobjs_vec_t::iterator attachment_iter = attachment->mAttachedObjects.begin();
                 attachment_iter != attachment->mAttachedObjects.end();
                 ++attachment_iter)
            {
                const LLViewerObject* attached_object = attachment_iter->get();
                if (attached_object && attached_object->mDrawable.notNull())
                {
                    gPipeline.markMoved(attached_object->mDrawable);
                }
            }
        }
    }
}

void LLVOAvatar::rebuildRiggedAttachments( void )
{
    for ( attachment_map_t::iterator iter = mAttachmentPoints.begin(); iter != mAttachmentPoints.end(); ++iter )
    {
        LLViewerJointAttachment* pAttachment = iter->second;
        LLViewerJointAttachment::attachedobjs_vec_t::iterator attachmentIterEnd = pAttachment->mAttachedObjects.end();

        for ( LLViewerJointAttachment::attachedobjs_vec_t::iterator attachmentIter = pAttachment->mAttachedObjects.begin();
             attachmentIter != attachmentIterEnd; ++attachmentIter)
        {
            const LLViewerObject* pAttachedObject =  *attachmentIter;
            if ( pAttachment && pAttachedObject->mDrawable.notNull() )
            {
                gPipeline.markRebuild(pAttachedObject->mDrawable);
            }
        }
    }
}
//-----------------------------------------------------------------------------
// cleanupAttachedMesh()
//-----------------------------------------------------------------------------
void LLVOAvatar::cleanupAttachedMesh( LLViewerObject* pVO )
{
    LLUUID mesh_id;
    if (getRiggedMeshID(pVO, mesh_id))
    {
        // FIXME this seems like an odd place for this code.
        if ( gAgentCamera.cameraCustomizeAvatar() )
        {
            gAgent.unpauseAnimation();
            //Still want to refocus on head bone
            gAgentCamera.changeCameraToCustomizeAvatar();
        }
    }
}

bool check_object_for_mesh_loading(LLViewerObject* objectp)
{
    if (!objectp || !objectp->getVolume())
    {
        return false;
    }
    LLVolume* volp = objectp->getVolume();
    const LLUUID& mesh_id = volp->getParams().getSculptID();
    if (mesh_id.isNull())
    {
        // No mesh nor skin info needed
        return false;
    }

    if (volp->isMeshAssetUnavaliable())
    {
        // Mesh failed to load, do not expect it
        return false;
    }

    if (!objectp->mDrawable)
    {
        return false;
    }

    LLVOVolume* pvobj = objectp->mDrawable->getVOVolume();
    if (pvobj)
    {
        if (!pvobj->isMesh())
        {
            // Not a mesh
            return false;
        }

        if (!volp->isMeshAssetLoaded())
        {
            // Waiting for mesh
            return true;
        }

        const LLMeshSkinInfo* skin_data = pvobj->getSkinInfo();
        if (skin_data)
        {
            // Skin info present, done
            return false;
        }

        if (pvobj->isSkinInfoUnavaliable())
        {
            // Load failed or info not present, don't expect it
            return false;
        }
    }

    // object is not ready
    return true;
}

bool LLVOAvatar::hasPendingAttachedMeshes()
{
    for (attachment_map_t::iterator iter = mAttachmentPoints.begin();
         iter != mAttachmentPoints.end();
         ++iter)
    {
        LLViewerJointAttachment* attachment = iter->second;
        if (attachment)
        {
            for (LLViewerJointAttachment::attachedobjs_vec_t::iterator attachment_iter = attachment->mAttachedObjects.begin();
                 attachment_iter != attachment->mAttachedObjects.end();
                 ++attachment_iter)
            {
                LLViewerObject* objectp = attachment_iter->get();
                if (objectp && !objectp->isDead())
                {
                    if (check_object_for_mesh_loading(objectp))
                    {
                        return true;
                    }
                    LLViewerObject::const_child_list_t& child_list = objectp->getChildren();
                    for (LLViewerObject::child_list_t::const_iterator iter1 = child_list.begin();
                         iter1 != child_list.end(); ++iter1)
                    {
                        LLViewerObject* objectchild = *iter1;
                        if (check_object_for_mesh_loading(objectchild))
                        {
                            return true;
                        }
                    }
                }
            }
        }
    }
    return false;
}

//-----------------------------------------------------------------------------
// detachObject()
//-----------------------------------------------------------------------------
bool LLVOAvatar::detachObject(LLViewerObject *viewer_object)
{
    for (attachment_map_t::iterator iter = mAttachmentPoints.begin();
         iter != mAttachmentPoints.end();
         ++iter)
    {
        LLViewerJointAttachment* attachment = iter->second;

        if (attachment->isObjectAttached(viewer_object))
        {
            updateVisualComplexity();
            bool is_animated_object = viewer_object->isAnimatedObject();
            cleanupAttachedMesh(viewer_object);

            attachment->removeObject(viewer_object);
            if (!is_animated_object)
            {
                updateAttachmentOverrides();
            }
            viewer_object->refreshBakeTexture();

            LLViewerObject::const_child_list_t& child_list = viewer_object->getChildren();
            for (LLViewerObject::child_list_t::const_iterator iter1 = child_list.begin();
                iter1 != child_list.end(); ++iter1)
            {
                LLViewerObject* objectp = *iter1;
                if (objectp)
            {
                    objectp->refreshBakeTexture();
                }
            }

            updateMeshVisibility();

            LL_DEBUGS() << "Detaching object " << viewer_object->mID << " from " << attachment->getName() << LL_ENDL;
            return true;
        }
    }

    std::vector<LLPointer<LLViewerObject> >::iterator iter = std::find(mPendingAttachment.begin(), mPendingAttachment.end(), viewer_object);
    if (iter != mPendingAttachment.end())
    {
        mPendingAttachment.erase(iter);
        return true;
    }

    return false;
}

//-----------------------------------------------------------------------------
// sitDown()
//-----------------------------------------------------------------------------
void LLVOAvatar::sitDown(bool bSitting)
{
    mIsSitting = bSitting;
    if (isSelf())
    {
        // Update Movement Controls according to own Sitting mode
        LLFloaterMove::setSittingMode(bSitting);

// [RLVa:KB] - Checked: 2010-08-29 (RLVa-1.2.1c) | Modified: RLVa-1.2.1c
        if (rlv_handler_t::isEnabled())
        {
            gRlvHandler.onSitOrStand(bSitting);
        }
// [/RLVa:KB]
    }
}

//-----------------------------------------------------------------------------
// sitOnObject()
//-----------------------------------------------------------------------------
void LLVOAvatar::sitOnObject(LLViewerObject *sit_object)
{
    if (isSelf())
    {
        // Might be first sit
        //LLFirstUse::useSit();

        gAgent.setFlying(false);
        gAgentCamera.setThirdPersonHeadOffset(LLVector3::zero);
        //interpolate to new camera position
        gAgentCamera.startCameraAnimation();
        // make sure we are not trying to autopilot
        gAgent.stopAutoPilot();
        gAgentCamera.setupSitCamera();
        if (gAgentCamera.getForceMouselook())
        {
            gAgentCamera.changeCameraToMouselook();
        }

        if (gAgentCamera.getFocusOnAvatar() && LLToolMgr::getInstance()->inEdit())
        {
            LLSelectNode* node = LLSelectMgr::getInstance()->getSelection()->getFirstRootNode();
            if (node && node->mValid)
            {
                LLViewerObject* root_object = node->getObject();
                if (root_object == sit_object)
                {
                    LLFloaterTools::sPreviousFocusOnAvatar = true;
                }
            }
        }
    }

    if (isSelf())
    {
        // #690: what the agent sat on, for a crash report's log tail (no names / ids)
        const LLViewerObject* seat_root = sit_object->getRootEdit();
        LL_INFOS("Breadcrumb") << "self sits: seat prim " << (seat_root == sit_object ? "root" : "child")
                               << ", linkset " << (seat_root ? seat_root->numChildren() + 1 : 0) << " prims"
                               << (sit_object->isAttachment() ? ", attachment" : "")
                               << (seat_root && seat_root->isAnimatedObject() ? ", animesh" : "")
                               << (mDrawable.isNull() ? ", no drawable" : "")
                               << ", frame " << gFrameCount << LL_ENDL;
    }

    if (mDrawable.isNull())
    {
        return;
    }
    LLQuaternion inv_obj_rot = ~sit_object->getRenderRotation();
    LLVector3 obj_pos = sit_object->getRenderPosition();

    LLVector3 rel_pos = getRenderPosition() - obj_pos;
    rel_pos.rotVec(inv_obj_rot);

    mDrawable->mXform.setPosition(rel_pos);
    mDrawable->mXform.setRotation(mDrawable->getWorldRotation() * inv_obj_rot);

    gPipeline.markMoved(mDrawable, true);
    // Notice that removing sitDown() from here causes avatars sitting on
    // objects to be not rendered for new arrivals. See EXT-6835 and EXT-1655.
    sitDown(true);
    mRoot->getXform()->setParent(&sit_object->mDrawable->mXform); // LLVOAvatar::sitOnObject
    // SL-315
    mRoot->setPosition(getPosition());
    mRoot->updateWorldMatrixChildren();

    stopMotion(ANIM_AGENT_BODY_NOISE);

    gAgentCamera.setInitSitRot(gAgent.getFrameAgent().getQuaternion());
}

//-----------------------------------------------------------------------------
// getOffObject()
//-----------------------------------------------------------------------------
void LLVOAvatar::getOffObject()
{
    if (isSelf())
    {
        // #690: standing up, for a crash report's log tail (no names / ids)
        const LLViewerObject* seat = (const LLViewerObject*)getParent();
        LL_INFOS("Breadcrumb") << "self stands: " << (seat ? (seat->isDead() ? "seat dead" : "seat present") : "no seat")
                               << ", sitting " << (isSitting() ? "yes" : "no")
                               << (mDrawable.isNull() ? ", no drawable" : "")
                               << ", frame " << gFrameCount << LL_ENDL;
    }

    if (mDrawable.isNull())
    {
        return;
    }

    LLViewerObject* sit_object = (LLViewerObject*)getParent();

    if (sit_object)
    {
        stopMotionFromSource(sit_object->getID());
        LLFollowCamMgr::getInstance()->setCameraActive(sit_object->getID(), false);

        LLViewerObject::const_child_list_t& child_list = sit_object->getChildren();
        for (LLViewerObject::child_list_t::const_iterator iter = child_list.begin();
             iter != child_list.end(); ++iter)
        {
            LLViewerObject* child_objectp = *iter;

            stopMotionFromSource(child_objectp->getID());
            LLFollowCamMgr::getInstance()->setCameraActive(child_objectp->getID(), false);
        }
    }

    // assumes that transform will not be updated with drawable still having a parent
    // or that drawable had no parent from the start
    LLVector3 cur_position_world = mDrawable->getWorldPosition();
    LLQuaternion cur_rotation_world = mDrawable->getWorldRotation();

    if (mLastRootPos.length() >= MAX_STANDOFF_FROM_ORIGIN
        && (cur_position_world.length() < MAX_STANDOFF_FROM_ORIGIN
            || dist_vec(cur_position_world, mLastRootPos) > MAX_STANDOFF_DISTANCE_CHANGE))
    {
        // Most likely drawable got updated too early or some updates were missed - we got relative position to non-existing parent
        // restore coordinates from cache
        cur_position_world = mLastRootPos;
    }

    // set *local* position based on last *world* position, since we're unparenting the avatar
    mDrawable->mXform.setPosition(cur_position_world);
    mDrawable->mXform.setRotation(cur_rotation_world);

    gPipeline.markMoved(mDrawable, true);

    sitDown(false);

    mRoot->getXform()->setParent(NULL); // LLVOAvatar::getOffObject
    // SL-315
    mRoot->setPosition(cur_position_world);
    mRoot->setRotation(cur_rotation_world);
    mRoot->getXform()->update();

    if (mEnableDefaultMotions)
    {
    startMotion(ANIM_AGENT_BODY_NOISE);
    }

    if (isSelf())
    {
        LLQuaternion av_rot = gAgent.getFrameAgent().getQuaternion();
        LLQuaternion obj_rot = sit_object ? sit_object->getRenderRotation() : LLQuaternion::DEFAULT;
        av_rot = av_rot * obj_rot;
        LLVector3 at_axis = LLVector3::x_axis;
        at_axis = at_axis * av_rot;
        at_axis.mV[VZ] = 0.f;
        at_axis.normalize();
        gAgent.resetAxes(at_axis);
        gAgentCamera.setThirdPersonHeadOffset(LLVector3(0.f, 0.f, 1.f));
        gAgentCamera.setSitCamera(LLUUID::null);
    }
}

//-----------------------------------------------------------------------------
// findAvatarFromAttachment()
//-----------------------------------------------------------------------------
// static
LLVOAvatar* LLVOAvatar::findAvatarFromAttachment( LLViewerObject* obj )
{
    if( obj->isAttachment() )
    {
        do
        {
            obj = (LLViewerObject*) obj->getParent();
        }
        while( obj && !obj->isAvatar() );

        if( obj && !obj->isDead() )
        {
            return (LLVOAvatar*)obj;
        }
    }
    return NULL;
}

S32 LLVOAvatar::getAttachmentCount() const
{
    size_t count = 0;

    for (attachment_map_t::const_iterator iter = mAttachmentPoints.begin(); iter != mAttachmentPoints.end(); ++iter)
    {
        LLViewerJointAttachment* pAttachment = iter->second;
        count += pAttachment->mAttachedObjects.size();
    }

    return static_cast<S32>(count);
}

bool LLVOAvatar::isWearingWearableType(LLWearableType::EType type) const
{
    if (mIsDummy) return true;

    if (isSelf())
    {
        return LLAvatarAppearance::isWearingWearableType(type);
    }

    switch(type)
    {
        case LLWearableType::WT_SHAPE:
        case LLWearableType::WT_SKIN:
        case LLWearableType::WT_HAIR:
        case LLWearableType::WT_EYES:
            return true;  // everyone has all bodyparts
        default:
            break; // Do nothing
    }

    for (LLAvatarAppearanceDictionary::Textures::const_iterator tex_iter = LLAvatarAppearance::getDictionary()->getTextures().begin();
         tex_iter != LLAvatarAppearance::getDictionary()->getTextures().end();
         ++tex_iter)
    {
        const LLAvatarAppearanceDictionary::TextureEntry *texture_dict = tex_iter->second;
        if (texture_dict->mWearableType == type)
        {
            // Thus, you must check to see if the corresponding baked texture is defined.
            // NOTE: this is a poor substitute if you actually want to know about individual pieces of clothing
            // this works for detecting a skirt (most important), but is ineffective at any piece of clothing that
            // gets baked into a texture that always exists (upper or lower).
            if (texture_dict->mIsUsedByBakedTexture)
            {
                const EBakedTextureIndex baked_index = texture_dict->mBakedTextureIndex;
                return isTextureDefined(LLAvatarAppearance::getDictionary()->getBakedTexture(baked_index)->mTextureIndex);
            }
            return false;
        }
    }
    return false;
}

LLViewerObject *    LLVOAvatar::findAttachmentByID( const LLUUID & target_id ) const
{
    for(attachment_map_t::const_iterator attachment_points_iter = mAttachmentPoints.begin();
        attachment_points_iter != mAttachmentPoints.end();
        ++attachment_points_iter)
    {
        LLViewerJointAttachment* attachment = attachment_points_iter->second;
        for (LLViewerJointAttachment::attachedobjs_vec_t::iterator attachment_iter = attachment->mAttachedObjects.begin();
             attachment_iter != attachment->mAttachedObjects.end();
             ++attachment_iter)
        {
            LLViewerObject *attached_object = attachment_iter->get();
            if (attached_object &&
                attached_object->getID() == target_id)
            {
                return attached_object;
            }
        }
    }

    return NULL;
}

// [SL:KB] - Patch: Appearance-RefreshAttachments | Checked: Catznip-5.3
void LLVOAvatar::rebuildAttachments()
{
    for (const auto& kvpAttachPt : mAttachmentPoints)
    {
        for (LLViewerObject* pAttachObj : kvpAttachPt.second->mAttachedObjects)
        {
            if (LLVOVolume* pAttachVol = (pAttachObj->isMesh()) ? dynamic_cast<LLVOVolume*>(pAttachObj) : nullptr)
            {
                pAttachVol->forceLOD(3);
                for (LLViewerObject* pChildObj : pAttachObj->getChildren())
                {
                    if (pChildObj->isMesh() && dynamic_cast<LLVOVolume*>(pChildObj))
                        pAttachVol->forceLOD(3);
                }
            }
        }
    }
}
// [/SL:KB]

// virtual
void LLVOAvatar::invalidateComposite( LLTexLayerSet* layerset)
{
}

void LLVOAvatar::invalidateAll()
{
}

// virtual
void LLVOAvatar::onGlobalColorChanged(const LLTexGlobalColor* global_color)
{
    if (global_color == mTexSkinColor)
    {
        invalidateComposite( mBakedTextureDatas[BAKED_HEAD].mTexLayerSet);
        invalidateComposite( mBakedTextureDatas[BAKED_UPPER].mTexLayerSet);
        invalidateComposite( mBakedTextureDatas[BAKED_LOWER].mTexLayerSet);
    }
    else if (global_color == mTexHairColor)
    {
        invalidateComposite( mBakedTextureDatas[BAKED_HEAD].mTexLayerSet);
        invalidateComposite( mBakedTextureDatas[BAKED_HAIR].mTexLayerSet);

        // ! BACKWARDS COMPATIBILITY !
        // Fix for dealing with avatars from viewers that don't bake hair.
        if (!isTextureDefined(mBakedTextureDatas[BAKED_HAIR].mTextureIndex))
        {
            LLColor4 color = mTexHairColor->getColor();
            avatar_joint_mesh_list_t::iterator iter = mBakedTextureDatas[BAKED_HAIR].mJointMeshes.begin();
            avatar_joint_mesh_list_t::iterator end  = mBakedTextureDatas[BAKED_HAIR].mJointMeshes.end();
            for (; iter != end; ++iter)
            {
                LLAvatarJointMesh* mesh = (*iter);
                if (mesh)
            {
                    mesh->setColor( color );
                }
            }
        }
    }
    else if (global_color == mTexEyeColor)
    {
        // LL_INFOS() << "invalidateComposite cause: onGlobalColorChanged( eyecolor )" << LL_ENDL;
        invalidateComposite( mBakedTextureDatas[BAKED_EYES].mTexLayerSet);
    }
    updateMeshTextures();
}

// virtual
// Do rigged mesh attachments display with this av?
bool LLVOAvatar::shouldRenderRigged() const
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;

    if (getOverallAppearance() == AOA_NORMAL)
    {
        return true;
    }
    // TBD - render for AOA_JELLYDOLL?
    return false;
}

// FIXME: We have an mVisible member, set in updateVisibility(), but this
// function doesn't return it! isVisible() and mVisible are used
// different places for different purposes. mVisible seems to be more
// related to whether the actual avatar mesh is shown, and isVisible()
// to whether anything about the avatar is displayed in the scene.
// Maybe better naming could make this clearer?
bool LLVOAvatar::isVisible() const
{
    static LLCachedControl<bool> friends_only(gSavedSettings, "RenderAvatarFriendsOnly", false);
    return mDrawable.notNull()
        && (!mOrphaned || isSelf())
        && (mDrawable->isVisible() || mIsDummy)
        && (!friends_only() || isUIAvatar() || isSelf() || isControlAvatar() || isBuddy());
}

// Determine if we have enough avatar data to render
bool LLVOAvatar::getHasMissingParts() const
{
    if (mIsDummy)
    {
        return false;
    }

    return (   ((const_cast<LLVOAvatar*>(this))->visualParamWeightsAreDefault())// Do we have a shape?
            || (   !isTextureDefined(TEX_LOWER_BAKED)
                || !isTextureDefined(TEX_UPPER_BAKED)
                || !isTextureDefined(TEX_HEAD_BAKED)
                )
            );
}

void LLVOAvatar::updateRezzedStatusTimers(S32 rez_status)
{
    // State machine for rezzed status. Statuses are -1 on startup, 0
    // Statuses are -1 on startup, 0 = cloud, 1 = gray, 2 = downloading, 3 = waiting for attachments, 4 = full.
    // Purpose is to collect time data for each it takes avatar to reach
    // various loading landmarks: gray, textured (partial), textured fully.

    if (rez_status != mLastRezzedStatus)
    {
        LL_DEBUGS("Avatar") << avString() << "rez state change: " << mLastRezzedStatus << " -> " << rez_status << LL_ENDL;

        if (mLastRezzedStatus == -1 && rez_status != -1)
        {
            // First time initialization, start all timers.
            for (S32 i = 1; i < 4; i++)
            {
                startPhase("load_" + LLVOAvatar::rezStatusToString(i));
                startPhase("first_load_" + LLVOAvatar::rezStatusToString(i));
            }
        }
        if (rez_status < mLastRezzedStatus)
        {
            // load level has decreased. start phase timers for higher load levels.
            for (S32 i = rez_status + 1; i <= mLastRezzedStatus; i++)
            {
                startPhase("load_" + LLVOAvatar::rezStatusToString(i));
            }
        }
        else if (rez_status > mLastRezzedStatus)
        {
            // load level has increased. stop phase timers for lower and equal load levels.
            for (S32 i = llmax(mLastRezzedStatus + 1, 1); i <= rez_status; i++)
            {
                stopPhase("load_" + LLVOAvatar::rezStatusToString(i));
                stopPhase("first_load_" + LLVOAvatar::rezStatusToString(i), false);
            }
            if (rez_status == 4)
            {
                // "fully loaded", mark any pending appearance change complete.
                selfStopPhase("update_appearance_from_cof");
                selfStopPhase("wear_inventory_category", false);
                selfStopPhase("process_initial_wearables_update", false);

                updateVisualComplexity();
            }
        }
        mLastRezzedStatus = rez_status;

        static LLUICachedControl<bool> show_rez_status("NameTagDebugAVRezState", false);
        if (show_rez_status)
        {
            mNameIsSet = false;
        }
    }
}

void LLVOAvatar::clearPhases()
{
    getPhases().clearPhases();
}

void LLVOAvatar::startPhase(const std::string& phase_name)
{
    F32 elapsed = 0.0;
    bool completed = false;
    bool found = getPhases().getPhaseValues(phase_name, elapsed, completed);
    //LL_DEBUGS("Avatar") << avString() << " phase state " << phase_name
    //                  << " found " << found << " elapsed " << elapsed << " completed " << completed << LL_ENDL;
    if (found)
    {
        if (!completed)
        {
            LL_DEBUGS("Avatar") << avString() << "no-op, start when started already for " << phase_name << LL_ENDL;
            return;
        }
    }
    LL_DEBUGS("Avatar") << "started phase " << phase_name << LL_ENDL;
    getPhases().startPhase(phase_name);
}

void LLVOAvatar::stopPhase(const std::string& phase_name, bool err_check)
{
    F32 elapsed = 0.0;
    bool completed = false;
    if (getPhases().getPhaseValues(phase_name, elapsed, completed))
    {
        if (!completed)
        {
            getPhases().stopPhase(phase_name);
            completed = true;
            logMetricsTimerRecord(phase_name, elapsed, completed);
            LL_DEBUGS("Avatar") << avString() << "stopped phase " << phase_name << " elapsed " << elapsed << LL_ENDL;
        }
        else
        {
            if (err_check)
            {
                LL_DEBUGS("Avatar") << "no-op, stop when stopped already for " << phase_name << LL_ENDL;
            }
        }
    }
    else
    {
        if (err_check)
        {
            LL_DEBUGS("Avatar") << "no-op, stop when not started for " << phase_name << LL_ENDL;
        }
    }
}

void LLVOAvatar::logPendingPhases()
{
    if (!isAgentAvatarValid())
    {
        return;
    }

    for (LLViewerStats::phase_map_t::iterator it = getPhases().begin();
         it != getPhases().end();
         ++it)
    {
        const std::string& phase_name = it->first;
        F32 elapsed;
        bool completed;
        if (getPhases().getPhaseValues(phase_name, elapsed, completed))
        {
            if (!completed)
            {
                logMetricsTimerRecord(phase_name, elapsed, completed);
            }
        }
    }
}

//static
void LLVOAvatar::logPendingPhasesAllAvatars()
{
    for (LLCharacter* character : LLCharacter::sInstances)
    {
        LLVOAvatar* avatar = (LLVOAvatar*)character;
        if (!avatar->isDead())
        {
            avatar->logPendingPhases();
        }
    }
}

void LLVOAvatar::logMetricsTimerRecord(const std::string& phase_name, F32 elapsed, bool completed)
{
    if (!isAgentAvatarValid())
    {
        return;
    }

    LLSD record;
    record["timer_name"] = phase_name;
    record["avatar_id"] = getID();
    record["elapsed"] = elapsed;
    record["completed"] = completed;
    U32 grid_x(0), grid_y(0);
    if (getRegion() && LLWorld::instance().isRegionListed(getRegion()))
    {
        record["central_bake_version"] = LLSD::Integer(getRegion()->getCentralBakeVersion());
        grid_from_region_handle(getRegion()->getHandle(), &grid_x, &grid_y);
    }
    record["grid_x"] = LLSD::Integer(grid_x);
    record["grid_y"] = LLSD::Integer(grid_y);
    record["is_using_server_bakes"] = true;
    record["is_self"] = isSelf();

    if (isAgentAvatarValid())
    {
        gAgentAvatarp->addMetricsTimerRecord(record);
    }
}

// call periodically to keep isFullyLoaded up to date.
// returns true if the value has changed.
bool LLVOAvatar::updateIsFullyLoaded()
{
    S32 rez_status = getRezzedStatus();
    bool loading = rez_status == 0;
    if (mFirstFullyVisible && !mIsControlAvatar)
    {
        loading = ((rez_status < 2)
                   // Wait at least 60s for unfinished textures to finish on first load,
                   // don't wait forever, it might fail. Even if it will eventually load by
                   // itself and update mLoadedCallbackTextures (or fail and clean the list),
                   // avatars are more time-sensitive than textures and can't wait that long.
                   || (mLoadedCallbackTextures < mCallbackTextureList.size() && mLastTexCallbackAddedTime.getElapsedTimeF32() < MAX_TEXTURE_WAIT_TIME_SEC)
                   || !mPendingAttachment.empty()
                   || (rez_status < 3 && !isFullyBaked())
                  );
        if (!loading)
        {
            mWaitingForMeshes = hasPendingAttachedMeshes();
            loading = mWaitingForMeshes;
        }

        // compare amount of attachments to one reported by simulator
        if (!isSelf() && mLastCloudAttachmentCount < mSimAttachments.size() && mSimAttachments.size() > 0)
        {
            S32 attachment_count = getAttachmentCount();
            if (mLastCloudAttachmentCount != attachment_count)
            {
                mLastCloudAttachmentCount = attachment_count;
                if (attachment_count != mSimAttachments.size())
                {
                    // attachment count changed, but still below desired, wait for more updates
                    mLastCloudAttachmentChangeTime.reset();
                    loading = true;
                }
            }
            else if (mLastCloudAttachmentChangeTime.getElapsedTimeF32() < MAX_ATTACHMENT_WAIT_TIME_SEC)
            {
                // waiting
                loading = true;
            }
            else if (!loading)
            {
                // for hasFirstFullAttachmentData
                mLastCloudAttachmentCount = (S32)mSimAttachments.size();
            }
        }
    }
    updateRezzedStatusTimers(rez_status);
    updateRuthTimer(loading);
    return processFullyLoadedChange(loading);
}

void LLVOAvatar::updateRuthTimer(bool loading)
{
    if (isSelf() || !loading)
    {
        return;
    }

    if (mPreviousFullyLoaded)
    {
        mRuthTimer.reset();
        debugAvatarRezTime("AvatarRezCloudNotification","became cloud");
    }

    const F32 LOADING_TIMEOUT__SECONDS = 120.f;
    if (mRuthTimer.getElapsedTimeF32() > LOADING_TIMEOUT__SECONDS)
    {
        LL_DEBUGS("Avatar") << avString()
                << "Ruth Timer timeout: Missing texture data for '" << getFullname() << "' "
                << "( Params loaded : " << !visualParamWeightsAreDefault() << " ) "
                << "( Lower : " << isTextureDefined(TEX_LOWER_BAKED) << " ) "
                << "( Upper : " << isTextureDefined(TEX_UPPER_BAKED) << " ) "
                << "( Head : " << isTextureDefined(TEX_HEAD_BAKED) << " )."
                << LL_ENDL;

        LLAvatarPropertiesProcessor::getInstance()->sendAvatarTexturesRequest(getID());
        mRuthTimer.reset();
    }
}

bool LLVOAvatar::processFullyLoadedChange(bool loading)
{
    // We wait a little bit before giving the 'all clear', to let things to
    // settle down: models to snap into place, textures to get first packets,
    // LODs to load.
    const F32 LOADED_DELAY = 1.f;

    if (loading)
    {
        mFullyLoadedTimer.reset();
    }

    if (mFirstFullyVisible)
    {
        F32 first_use_delay = FIRST_APPEARANCE_CLOUD_MIN_DELAY;
        if (!isSelf() && loading)
        {
            // Note that textures can causes 60s delay on thier own
            // so this delay might end up on top of textures' delay
            first_use_delay = llclamp(
                mFirstAppearanceMessageTimer.getElapsedTimeF32(),
                FIRST_APPEARANCE_CLOUD_MIN_DELAY,
                FIRST_APPEARANCE_CLOUD_MAX_DELAY);

            if (shouldImpostor())
            {
                // Impostors are less of a priority,
                // let them stay cloud longer
                first_use_delay *= FIRST_APPEARANCE_CLOUD_IMPOSTOR_MODIFIER;
            }
        }
        mFullyLoaded = (mFullyLoadedTimer.getElapsedTimeF32() > first_use_delay);
    }
    else
    {
        mFullyLoaded = (mFullyLoadedTimer.getElapsedTimeF32() > LOADED_DELAY);
    }

    if (!mPreviousFullyLoaded && !loading && mFullyLoaded)
    {
        debugAvatarRezTime("AvatarRezNotification", "fully loaded");
        LLCrossingKeep::onAvatarFullyLoaded(this);   // #607: time-to-rezzed after a re-create
    }

    // did our loading state "change" from last call?
    // FIXME runway - why are we updating every 30 calls even if nothing has changed?
    // This causes updateLOD() to run every 30 frames, among other things.
    bool fully_loaded_changed = (mFullyLoaded != mPreviousFullyLoaded);
    const S32 UPDATE_RATE = 30;
    bool changed =
        ((mFullyLoaded != mPreviousFullyLoaded) ||         // if the value is different from the previous call
            (!mFullyLoadedInitialized) ||                     // if we've never been called before
            (mFullyLoadedFrameCounter % UPDATE_RATE == 0));   // every now and then issue a change

    mPreviousFullyLoaded = mFullyLoaded;
    mFullyLoadedInitialized = true;
    mFullyLoadedFrameCounter++;

    if (changed && isSelf())
    {
        // to know about outfit switching
        LLAvatarRenderNotifier::getInstance()->updateNotificationState();
    }

    if (fully_loaded_changed && !isSelf() && mFullyLoaded && isImpostor())
    {
        // Fix for jellydoll initially invisible
        mNeedsImpostorUpdate = true;
        mLastImpostorUpdateReason = 6;
    }

    return changed;
}

bool LLVOAvatar::isFullyLoaded() const
{
// [SL:KB] - Patch: Appearance-SyncAttach | Checked: Catznip-2.2
    // Changes to LLAppearanceMgr::updateAppearanceFromCOF() expect this function to actually return mFullyLoaded for gAgentAvatarp
    static LLCachedControl<bool> RenderUnloadedAvatar(gSavedSettings, "RenderUnloadedAvatar", false);
    return (RenderUnloadedAvatar && !isSelf()) ||(mFullyLoaded);
// [/SL:KB]
//  return (RenderUnloadedAvatar || mFullyLoaded);
}

bool LLVOAvatar::hasFirstFullAttachmentData() const
{
    return !mFirstFullyVisible // Avatar is fully visible, have all data
        || mLastCloudAttachmentCount >= (S32)mSimAttachments.size();
}

bool LLVOAvatar::isTooComplex() const
{
    bool too_complex;
    static LLCachedControl<S32> complexity_render_mode(gSavedSettings, "RenderAvatarComplexityMode");
    bool render_friend =  (isBuddy() && complexity_render_mode > AV_RENDER_LIMIT_BY_COMPLEXITY);

    if (isSelf() || render_friend || mVisuallyMuteSetting == AV_ALWAYS_RENDER)
    {
        too_complex = false;
    }
    else if (complexity_render_mode == AV_RENDER_ONLY_SHOW_FRIENDS && !mIsControlAvatar)
    {
        too_complex = true;
    }
    else
    {
        // Determine if visually muted or not
        static LLCachedControl<U32> max_render_cost(gSavedSettings, "RenderAvatarMaxComplexity", 0U);
        static LLCachedControl<F32> max_attachment_area(gSavedSettings, "RenderAutoMuteSurfaceAreaLimit", 1000.0f);
        // If the user has chosen unlimited max complexity, we also disregard max attachment area
        // so that unlimited will completely disable the overly complex impostor rendering
        // yes, this leaves them vulnerable to griefing objects... their choice
        too_complex = (   max_render_cost > 0
                          && (mVisualComplexity > max_render_cost
                           || (max_attachment_area > 0.0f && mAttachmentSurfaceArea > max_attachment_area)
                           ));
    }

    return too_complex;
}

bool LLVOAvatar::isTooSlow() const
{
    if (mIsControlAvatar)
    {
        return mTooSlow;
    }

    static LLCachedControl<S32> complexity_render_mode(gSavedSettings, "RenderAvatarComplexityMode");
    static LLCachedControl<bool> friends_only(gSavedSettings, "RenderAvatarFriendsOnly", false);
    bool is_friend = isBuddy();
    bool render_friend = is_friend && complexity_render_mode > AV_RENDER_LIMIT_BY_COMPLEXITY;

    if (render_friend || mVisuallyMuteSetting == AV_ALWAYS_RENDER)
    {
        return false;
    }
    else if (complexity_render_mode == AV_RENDER_ONLY_SHOW_FRIENDS)
    {
        return true;
    }
    else if (!is_friend && friends_only())
    {
        return true;
    }

    return mTooSlow;
}

// Udpate Avatar state based on render time
void LLVOAvatar::updateTooSlow()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;
    static LLCachedControl<S32> complexity_render_mode(gSavedSettings, "RenderAvatarComplexityMode");
    static LLCachedControl<bool> allowSelfImpostor(gSavedSettings, "AllowSelfImpostor");

    // mTooSlow - Is the avatar flagged as being slow (includes shadow time)
    // mTooSlowWithoutShadows - Is the avatar flagged as being slow even with shadows removed.

    // get max render time in ms
    F32 max_art_ms = (F32) (LLPerfStats::renderAvatarMaxART_ns / 1000000.0);

    // (#422: LL's auto-tune exempted the nearest avatars from a limit IT had lowered; the
    // Vulkan auto-tuner never lowers RenderAvatarMaxART, so the exemption is gone with it.)
    bool exceeds_max_ART =
        ((LLPerfStats::renderAvatarMaxART_ns > 0) &&
            (mGPURenderTime >= max_art_ms)); // NOTE: don't use getGPURenderTime accessor here to avoid "isTooSlow" feedback loop

    if (exceeds_max_ART)
    {
        mTooSlow = true;

        if(!mTooSlowWithoutShadows) // if we were not previously above the full impostor cap
        {
            bool always_render_friends = complexity_render_mode > AV_RENDER_LIMIT_BY_COMPLEXITY;
            bool render_friend_or_exception =   (always_render_friends && isBuddy()) ||
                ( getVisualMuteSettings() == LLVOAvatar::AV_ALWAYS_RENDER );
            if( (!isSelf() || allowSelfImpostor) && !render_friend_or_exception)
            {
                // Note: slow rendering Friends still get their shadows zapped.
                mTooSlowWithoutShadows = (getGPURenderTime()*2.f >= max_art_ms)  // NOTE: assumes shadow rendering doubles render time
                    || (complexity_render_mode == AV_RENDER_ONLY_SHOW_FRIENDS && !mIsControlAvatar);
            }
        }
    }
    else
    {
        mTooSlow = false;
        mTooSlowWithoutShadows = false;
    }
    if(mTooSlow && !mTuned)
    {
        LLPerfStats::tunedAvatars++; // increment the number of avatars that have been tweaked.
        mTuned = true;
    }
    else if(!mTooSlow && mTuned)
    {
        LLPerfStats::tunedAvatars--;
        mTuned = false;
    }
}

//-----------------------------------------------------------------------------
// findMotion()
//-----------------------------------------------------------------------------
LLMotion* LLVOAvatar::findMotion(const LLUUID& id) const
{
    return mMotionController.findMotion(id);
}

// This is a semi-deprecated debugging tool - meshes will not show as
// colorized if using deferred rendering.
void LLVOAvatar::debugColorizeSubMeshes(U32 i, const LLColor4& color)
{
    static LLCachedControl<bool> debug_av_composite_baked(gSavedSettings, "DebugAvatarCompositeBaked");
    if (debug_av_composite_baked)
    {
        avatar_joint_mesh_list_t::iterator iter = mBakedTextureDatas[i].mJointMeshes.begin();
        avatar_joint_mesh_list_t::iterator end  = mBakedTextureDatas[i].mJointMeshes.end();
        for (; iter != end; ++iter)
        {
            LLAvatarJointMesh* mesh = (*iter);
            if (mesh)
            {
                mesh->setColor(color);
            }
        }
    }
}


//-----------------------------------------------------------------------------
// updateMeshVisibility()
// Hide the mesh joints if attachments are using baked textures
//-----------------------------------------------------------------------------
void LLVOAvatar::updateMeshVisibility()
{
    bool bake_flag[BAKED_NUM_INDICES];
    memset(bake_flag, 0, BAKED_NUM_INDICES*sizeof(bool));

    if (getOverallAppearance() == AOA_NORMAL)
    {
        for (const auto& [attachment_point_id, attachment] : mAttachmentPoints)
        {
            if (!attachment)
                continue;

            for (const auto& objectp : attachment->mAttachedObjects)
            {
                if (objectp.isNull())
                    continue;

                for (int face_index = 0; face_index < objectp->getNumTEs(); face_index++)
                {
                    LLTextureEntry* tex_entry = objectp->getTE(face_index);
                    const auto& tex_id = tex_entry->getID();
                    bake_flag[BAKED_HEAD] |= (tex_id == IMG_USE_BAKED_HEAD);
                    bake_flag[BAKED_EYES] |= (tex_id == IMG_USE_BAKED_EYES);
                    bake_flag[BAKED_HAIR] |= (tex_id == IMG_USE_BAKED_HAIR);
                    bake_flag[BAKED_LOWER] |= (tex_id == IMG_USE_BAKED_LOWER);
                    bake_flag[BAKED_UPPER] |= (tex_id == IMG_USE_BAKED_UPPER);
                    bake_flag[BAKED_SKIRT] |= (tex_id == IMG_USE_BAKED_SKIRT);
                    bake_flag[BAKED_LEFT_ARM] |= (tex_id == IMG_USE_BAKED_LEFTARM);
                    bake_flag[BAKED_LEFT_LEG] |= (tex_id == IMG_USE_BAKED_LEFTLEG);
                    bake_flag[BAKED_AUX1] |= (tex_id == IMG_USE_BAKED_AUX1);
                    bake_flag[BAKED_AUX2] |= (tex_id == IMG_USE_BAKED_AUX2);
                    bake_flag[BAKED_AUX3] |= (tex_id == IMG_USE_BAKED_AUX3);
                }

                for (const auto& objectchild : objectp->getChildren())
                {
                    if (objectchild.isNull())
                        continue;

                    for (int face_index = 0; face_index < objectchild->getNumTEs(); face_index++)
                    {
                        LLTextureEntry* tex_entry = objectchild->getTE(face_index);
                        const auto& tex_id = tex_entry->getID();
                        bake_flag[BAKED_HEAD] |= (tex_id == IMG_USE_BAKED_HEAD);
                        bake_flag[BAKED_EYES] |= (tex_id == IMG_USE_BAKED_EYES);
                        bake_flag[BAKED_HAIR] |= (tex_id == IMG_USE_BAKED_HAIR);
                        bake_flag[BAKED_LOWER] |= (tex_id == IMG_USE_BAKED_LOWER);
                        bake_flag[BAKED_UPPER] |= (tex_id == IMG_USE_BAKED_UPPER);
                        bake_flag[BAKED_SKIRT] |= (tex_id == IMG_USE_BAKED_SKIRT);
                        bake_flag[BAKED_LEFT_ARM] |= (tex_id == IMG_USE_BAKED_LEFTARM);
                        bake_flag[BAKED_LEFT_LEG] |= (tex_id == IMG_USE_BAKED_LEFTLEG);
                        bake_flag[BAKED_AUX1] |= (tex_id == IMG_USE_BAKED_AUX1);
                        bake_flag[BAKED_AUX2] |= (tex_id == IMG_USE_BAKED_AUX2);
                        bake_flag[BAKED_AUX3] |= (tex_id == IMG_USE_BAKED_AUX3);
                    }
                }
            }
        }
    }

    //LL_INFOS() << "head " << bake_flag[BAKED_HEAD] << "eyes " << bake_flag[BAKED_EYES] << "hair " << bake_flag[BAKED_HAIR] << "lower " << bake_flag[BAKED_LOWER] << "upper " << bake_flag[BAKED_UPPER] << "skirt " << bake_flag[BAKED_SKIRT] << LL_ENDL;

    for (S32 i = 0; i < mMeshLOD.size(); i++)
    {
        LLAvatarJoint* joint = mMeshLOD[i];
        if (i == MESH_ID_HAIR)
        {
            joint->setVisible(!bake_flag[BAKED_HAIR], true);
        }
        else if (i == MESH_ID_HEAD)
        {
            joint->setVisible(!bake_flag[BAKED_HEAD], true);
        }
        else if (i == MESH_ID_SKIRT)
        {
            joint->setVisible(!bake_flag[BAKED_SKIRT], true);
        }
        else if (i == MESH_ID_UPPER_BODY)
        {
            joint->setVisible(!bake_flag[BAKED_UPPER], true);
        }
        else if (i == MESH_ID_LOWER_BODY)
        {
            joint->setVisible(!bake_flag[BAKED_LOWER], true);
        }
        else if (i == MESH_ID_EYEBALL_LEFT)
        {
            joint->setVisible(!bake_flag[BAKED_EYES], true);
        }
        else if (i == MESH_ID_EYEBALL_RIGHT)
        {
            joint->setVisible(!bake_flag[BAKED_EYES], true);
        }
        else if (i == MESH_ID_EYELASH)
        {
            joint->setVisible(!bake_flag[BAKED_HEAD], true);
        }
    }
}

//-----------------------------------------------------------------------------
// updateMeshTextures()
// Uses the current TE values to set the meshes' and layersets' textures.
//-----------------------------------------------------------------------------
// virtual
void LLVOAvatar::updateMeshTextures()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;
    static S32 update_counter = 0;
    mBakedTextureDebugText.clear();

    // if user has never specified a texture, assign the default
    for (U32 i=0; i < getNumTEs(); i++)
    {
        const LLViewerTexture* te_image = getImage(i, 0);
        if(!te_image || te_image->getID().isNull() || (te_image->getID() == IMG_DEFAULT))
        {
            // IMG_DEFAULT_AVATAR = a special texture that's never rendered.
            const LLUUID& image_id = (i == TEX_HAIR ? IMG_DEFAULT : IMG_DEFAULT_AVATAR);
            setImage(i, LLViewerTextureManager::getFetchedTexture(image_id), 0);
        }
    }

    const bool other_culled = !isSelf() && mCulled;
    LLLoadedCallbackEntry::source_callback_list_t* src_callback_list = NULL ;
    bool paused = false;
    if(!isSelf())
    {
        src_callback_list = &mCallbackTextureList ;
        paused = !isVisible();
    }

    std::vector<bool> is_layer_baked;
    is_layer_baked.resize(mBakedTextureDatas.size(), false);

    std::vector<bool> use_lkg_baked_layer; // lkg = "last known good"
    use_lkg_baked_layer.resize(mBakedTextureDatas.size(), false);

    mBakedTextureDebugText += llformat("%06d\n",update_counter++);
    mBakedTextureDebugText += "indx layerset linvld ltda ilb ulkg ltid\n";
    for (U32 i=0; i < mBakedTextureDatas.size(); i++)
    {
        is_layer_baked[i] = isTextureDefined(mBakedTextureDatas[i].mTextureIndex);
        LLViewerTexLayerSet* layerset = NULL;
        bool layerset_invalid = false;
        if (!other_culled)
        {
            // When an avatar is changing clothes and not in Appearance mode,
            // use the last-known good baked texture until it finishes the first
            // render of the new layerset.
            layerset = getTexLayerSet(i);
            layerset_invalid = layerset && ( !layerset->getViewerComposite()->isInitialized()
                                             || !layerset->isLocalTextureDataAvailable() );
            use_lkg_baked_layer[i] = (!is_layer_baked[i]
                                      && (mBakedTextureDatas[i].mLastTextureID != IMG_DEFAULT_AVATAR)
                                      && layerset_invalid);
            if (use_lkg_baked_layer[i])
            {
                layerset->setUpdatesEnabled(true);
            }
        }
        else
        {
            use_lkg_baked_layer[i] = (!is_layer_baked[i]
                                      && mBakedTextureDatas[i].mLastTextureID != IMG_DEFAULT_AVATAR);
        }

        std::string last_id_string;
        if (mBakedTextureDatas[i].mLastTextureID == IMG_DEFAULT_AVATAR)
            last_id_string = "A";
        else if (mBakedTextureDatas[i].mLastTextureID == IMG_DEFAULT)
            last_id_string = "D";
        else if (mBakedTextureDatas[i].mLastTextureID == IMG_INVISIBLE)
            last_id_string = "I";
        else
            last_id_string = "*";
        bool is_ltda = layerset
            && layerset->getViewerComposite()->isInitialized()
            && layerset->isLocalTextureDataAvailable();
        mBakedTextureDebugText += llformat("%4d   %4s     %4d %4d %4d %4d %4s\n",
                                           i,
                                           (layerset?"*":"0"),
                                           layerset_invalid,
                                           is_ltda,
                                           is_layer_baked[i],
                                           use_lkg_baked_layer[i],
                                           last_id_string.c_str());
    }

    for (U32 i=0; i < mBakedTextureDatas.size(); i++)
    {
        debugColorizeSubMeshes(i, LLColor4::white);

        LLViewerTexLayerSet* layerset = getTexLayerSet(i);
        if (use_lkg_baked_layer[i] && !isUsingLocalAppearance() )
        {
            // use last known good layer (no new one)
            LLViewerFetchedTexture* baked_img = LLViewerTextureManager::getFetchedTexture(mBakedTextureDatas[i].mLastTextureID);
            mBakedTextureDatas[i].mIsUsed = true;

            debugColorizeSubMeshes(i,LLColor4::red);

            avatar_joint_mesh_list_t::iterator iter = mBakedTextureDatas[i].mJointMeshes.begin();
            avatar_joint_mesh_list_t::iterator end  = mBakedTextureDatas[i].mJointMeshes.end();
            for (; iter != end; ++iter)
            {
                LLAvatarJointMesh* mesh = (*iter);
                if (mesh)
                {
                    mesh->setTexture( baked_img );
                }
            }
        }
        else if (!isUsingLocalAppearance() && is_layer_baked[i])
        {
            // use new layer
            LLViewerFetchedTexture* baked_img =
                LLViewerTextureManager::staticCastToFetchedTexture(
                    getImage( mBakedTextureDatas[i].mTextureIndex, 0 ), true) ;
            if( baked_img->getID() == mBakedTextureDatas[i].mLastTextureID )
            {
                // Even though the file may not be finished loading,
                // we'll consider it loaded and use it (rather than
                // doing compositing).
                useBakedTexture( baked_img->getID() );
                                mLoadedCallbacksPaused |= !isVisible();
                                checkTextureLoading();
            }
            else
            {
                mBakedTextureDatas[i].mIsLoaded = false;
                if ( (baked_img->getID() != IMG_INVISIBLE) &&
                     ((i == BAKED_HEAD) || (i == BAKED_UPPER) || (i == BAKED_LOWER)) )
                {
                    baked_img->setLoadedCallback(onBakedTextureMasksLoaded, MORPH_MASK_REQUESTED_DISCARD, true, true, new LLTextureMaskData( mID ),
                        src_callback_list, paused);
                }
                baked_img->setLoadedCallback(onBakedTextureLoaded, SWITCH_TO_BAKED_DISCARD, false, false, new LLUUID( mID ),
                    src_callback_list, paused );
                if (baked_img->getDiscardLevel() < 0 && !paused)
                {
                    // mLoadedCallbackTextures will be updated by checkTextureLoading() below
                    mLastTexCallbackAddedTime.reset();
                }

                // this could add paused texture callbacks
                mLoadedCallbacksPaused |= paused;
                checkTextureLoading();
            }
        }
        else if (layerset && isUsingLocalAppearance())
        {
            debugColorizeSubMeshes(i,LLColor4::yellow );

            layerset->createComposite();
            layerset->setUpdatesEnabled( true );
            mBakedTextureDatas[i].mIsUsed = false;

            avatar_joint_mesh_list_t::iterator iter = mBakedTextureDatas[i].mJointMeshes.begin();
            avatar_joint_mesh_list_t::iterator end  = mBakedTextureDatas[i].mJointMeshes.end();
            for (; iter != end; ++iter)
            {
                LLAvatarJointMesh* mesh = (*iter);
                if (mesh)
                {
                    mesh->setLayerSet( layerset );
                }
            }
        }
        else
        {
            debugColorizeSubMeshes(i,LLColor4::blue);
        }
    }

    // set texture and color of hair manually if we are not using a baked image.
    // This can happen while loading hair for yourself, or for clients that did not
    // bake a hair texture. Still needed for yourself after 1.22 is depricated.
    if (!is_layer_baked[BAKED_HAIR])
    {
        const LLColor4 color = mTexHairColor ? mTexHairColor->getColor() : LLColor4(1,1,1,1);
        LLViewerTexture* hair_img = getImage( TEX_HAIR, 0 );
        avatar_joint_mesh_list_t::iterator iter = mBakedTextureDatas[BAKED_HAIR].mJointMeshes.begin();
        avatar_joint_mesh_list_t::iterator end  = mBakedTextureDatas[BAKED_HAIR].mJointMeshes.end();
        for (; iter != end; ++iter)
        {
            LLAvatarJointMesh* mesh = (*iter);
            if (mesh)
            {
                mesh->setColor( color );
                mesh->setTexture( hair_img );
            }
        }
    }


    for (LLAvatarAppearanceDictionary::BakedTextures::const_iterator baked_iter =
             LLAvatarAppearance::getDictionary()->getBakedTextures().begin();
         baked_iter != LLAvatarAppearance::getDictionary()->getBakedTextures().end();
         ++baked_iter)
    {
        const EBakedTextureIndex baked_index = baked_iter->first;
        const LLAvatarAppearanceDictionary::BakedEntry *baked_dict = baked_iter->second;

        for (texture_vec_t::const_iterator local_tex_iter = baked_dict->mLocalTextures.begin();
             local_tex_iter != baked_dict->mLocalTextures.end();
             ++local_tex_iter)
        {
            const ETextureIndex texture_index = *local_tex_iter;
            const bool is_baked_ready = (is_layer_baked[baked_index] && mBakedTextureDatas[baked_index].mIsLoaded) || other_culled;
            if (isSelf())
            {
                setBakedReady(texture_index, is_baked_ready);
            }
        }
    }

    // removeMissingBakedTextures() will call back into this rountine if something is removed, and can blow up the stack
    static bool call_remove_missing = true;
    if (call_remove_missing)
    {
        call_remove_missing = false;
        removeMissingBakedTextures();   // May call back into this function if anything is removed
        call_remove_missing = true;
    }

    //refresh bakes on any attached objects
    for (attachment_map_t::iterator iter = mAttachmentPoints.begin();
        iter != mAttachmentPoints.end();
        ++iter)
    {
        LLViewerJointAttachment* attachment = iter->second;

        for (LLViewerJointAttachment::attachedobjs_vec_t::iterator attachment_iter = attachment->mAttachedObjects.begin();
            attachment_iter != attachment->mAttachedObjects.end();
            ++attachment_iter)
        {
            LLViewerObject* attached_object = attachment_iter->get();
            if (attached_object && !attached_object->isDead())
            {
                attached_object->refreshBakeTexture();

                LLViewerObject::const_child_list_t& child_list = attached_object->getChildren();
                for (LLViewerObject::child_list_t::const_iterator iter = child_list.begin();
                    iter != child_list.end(); ++iter)
                {
                    LLViewerObject* objectp = *iter;
                    if (objectp && !objectp->isDead())
                    {
                        objectp->refreshBakeTexture();
                    }
                }
            }
        }
    }




    // VK: BOM attachments (rigged bodies/heads) bind the avatar's bakes through the
    // IMG_USE_BAKED_* sentinels, resolved into each face's material when the object is
    // ingested. A settled object isn't re-ingested when the bakes change (new outfit /
    // rebake), so its material kept the OLD bake -- which, no longer wanted by anything,
    // sat at a blurry discard: "fuzzy BOM" until relog (#105). Re-ingest them now.
    {
        for (const auto& ap : mAttachmentPoints)
        {
            LLViewerJointAttachment* att = ap.second;
            if (!att) continue;
            for (LLViewerObject* ao : att->mAttachedObjects)
            {
                if (!ao || ao->isDead()) continue;
                LLCleanIngest::instance().noteObjectChanged(ao);
                for (LLViewerObject* child : ao->getChildren())
                    LLCleanIngest::instance().noteObjectChanged(child);
            }
        }
    }
}

// virtual
//-----------------------------------------------------------------------------
// setLocalTexture()
//-----------------------------------------------------------------------------
void LLVOAvatar::setLocalTexture( ETextureIndex type, LLViewerTexture* in_tex, bool baked_version_ready, U32 index )
{
    // invalid for anyone but self
    llassert(0);
}

//virtual
void LLVOAvatar::setBakedReady(LLAvatarAppearanceDefines::ETextureIndex type, bool baked_version_exists, U32 index)
{
    // invalid for anyone but self
    llassert(0);
}

void LLVOAvatar::addChat(const LLChat& chat)
{
    std::deque<LLChat>::iterator chat_iter;

    mChats.push_back(chat);

    size_t chat_length = 0;
    for( chat_iter = mChats.begin(); chat_iter != mChats.end(); ++chat_iter)
    {
        chat_length += chat_iter->mText.size();
    }

    // remove any excess chat
    chat_iter = mChats.begin();
    while ((chat_length > MAX_BUBBLE_CHAT_LENGTH || mChats.size() > MAX_BUBBLE_CHAT_UTTERANCES) && chat_iter != mChats.end())
    {
        chat_length -= chat_iter->mText.size();
        mChats.pop_front();
        chat_iter = mChats.begin();
    }

    mChatTimer.reset();
}

void LLVOAvatar::clearChat()
{
    mChats.clear();
}


void LLVOAvatar::applyMorphMask(const U8* tex_data, S32 width, S32 height, S32 num_components, LLAvatarAppearanceDefines::EBakedTextureIndex index)
{
    if (index >= BAKED_NUM_INDICES)
    {
        LL_WARNS() << "invalid baked texture index passed to applyMorphMask" << LL_ENDL;
        return;
    }

    for (morph_list_t::const_iterator iter = mBakedTextureDatas[index].mMaskedMorphs.begin();
         iter != mBakedTextureDatas[index].mMaskedMorphs.end(); ++iter)
    {
        const LLMaskedMorph* maskedMorph = (*iter);
        LLPolyMorphTarget* morph_target = dynamic_cast<LLPolyMorphTarget*>(maskedMorph->mMorphTarget);
        if (morph_target)
        {
            morph_target->applyMask(tex_data, width, height, num_components, maskedMorph->mInvert);
        }
    }
}

// returns true if morph masks are present and not valid for a given baked texture, false otherwise
bool LLVOAvatar::morphMaskNeedsUpdate(LLAvatarAppearanceDefines::EBakedTextureIndex index)
{
    if (index >= BAKED_NUM_INDICES)
    {
        return false;
    }

    if (!mBakedTextureDatas[index].mMaskedMorphs.empty())
    {
        if (isSelf())
        {
            LLViewerTexLayerSet *layer_set = getTexLayerSet(index);
            if (layer_set)
            {
                return !layer_set->isMorphValid();
            }
        }
        else
        {
            return false;
        }
    }

    return false;
}

//-----------------------------------------------------------------------------
// releaseComponentTextures()
// release any component texture UUIDs for which we have a baked texture
// ! BACKWARDS COMPATIBILITY !
// This is only called for non-self avatars, it can be taken out once component
// textures aren't communicated by non-self avatars.
//-----------------------------------------------------------------------------
void LLVOAvatar::releaseComponentTextures()
{
    // ! BACKWARDS COMPATIBILITY !
    // Detect if the baked hair texture actually wasn't sent, and if so set to default
    if (isTextureDefined(TEX_HAIR_BAKED) && getImage(TEX_HAIR_BAKED,0)->getID() == getImage(TEX_SKIRT_BAKED,0)->getID())
    {
        if (getImage(TEX_HAIR_BAKED,0)->getID() != IMG_INVISIBLE)
        {
            // Regression case of messaging system. Expected 21 textures, received 20. last texture is not valid so set to default
            setTETexture(TEX_HAIR_BAKED, IMG_DEFAULT_AVATAR);
        }
    }

    for (U8 baked_index = 0; baked_index < BAKED_NUM_INDICES; baked_index++)
    {
        const LLAvatarAppearanceDictionary::BakedEntry * bakedDicEntry = LLAvatarAppearance::getDictionary()->getBakedTexture((EBakedTextureIndex)baked_index);
        // skip if this is a skirt and av is not wearing one, or if we don't have a baked texture UUID
        if (!isTextureDefined(bakedDicEntry->mTextureIndex)
            && ( (baked_index != BAKED_SKIRT) || isWearingWearableType(LLWearableType::WT_SKIRT) ))
        {
            continue;
        }

        for (U8 texture = 0; texture < bakedDicEntry->mLocalTextures.size(); texture++)
        {
            const U8 te = (ETextureIndex)bakedDicEntry->mLocalTextures[texture];
            setTETexture(te, IMG_DEFAULT_AVATAR);
        }
    }
}

void LLVOAvatar::dumpAvatarTEs( const std::string& context ) const
{
    LL_DEBUGS("Avatar") << avString() << (isSelf() ? "Self: " : "Other: ") << context << LL_ENDL;
    for (LLAvatarAppearanceDictionary::Textures::const_iterator iter = LLAvatarAppearance::getDictionary()->getTextures().begin();
         iter != LLAvatarAppearance::getDictionary()->getTextures().end();
         ++iter)
    {
        const LLAvatarAppearanceDictionary::TextureEntry *texture_dict = iter->second;
        // TODO: MULTI-WEARABLE: handle multiple textures for self
        const LLViewerTexture* te_image = getImage(iter->first,0);
        if( !te_image )
        {
            LL_DEBUGS("Avatar") << avString() << "       " << texture_dict->mName << ": null ptr" << LL_ENDL;
        }
        else if( te_image->getID().isNull() )
        {
            LL_DEBUGS("Avatar") << avString() << "       " << texture_dict->mName << ": null UUID" << LL_ENDL;
        }
        else if( te_image->getID() == IMG_DEFAULT )
        {
            LL_DEBUGS("Avatar") << avString() << "       " << texture_dict->mName << ": IMG_DEFAULT" << LL_ENDL;
        }
        else if( te_image->getID() == IMG_DEFAULT_AVATAR )
        {
            LL_DEBUGS("Avatar") << avString() << "       " << texture_dict->mName << ": IMG_DEFAULT_AVATAR" << LL_ENDL;
        }
        else
        {
            LL_DEBUGS("Avatar") << avString() << "       " << texture_dict->mName << ": " << te_image->getID() << LL_ENDL;
        }
    }
}

//-----------------------------------------------------------------------------
// clampAttachmentPositions()
//-----------------------------------------------------------------------------
void LLVOAvatar::clampAttachmentPositions()
{
    if (isDead())
    {
        return;
    }
    for (attachment_map_t::iterator iter = mAttachmentPoints.begin();
         iter != mAttachmentPoints.end();
         ++iter)
    {
        LLViewerJointAttachment* attachment = iter->second;
        if (attachment)
        {
            attachment->clampObjectPosition();
        }
    }
}

bool LLVOAvatar::hasHUDAttachment() const
{
    for (attachment_map_t::const_iterator iter = mAttachmentPoints.begin();
         iter != mAttachmentPoints.end();
         ++iter)
    {
        LLViewerJointAttachment* attachment = iter->second;
        if (attachment->getIsHUDAttachment() && attachment->getNumObjects() > 0)
        {
            return true;
        }
    }
    return false;
}

LLBBox LLVOAvatar::getHUDBBox() const
{
    LLBBox bbox;
    for (attachment_map_t::const_iterator iter = mAttachmentPoints.begin();
         iter != mAttachmentPoints.end();
         ++iter)
    {
        LLViewerJointAttachment* attachment = iter->second;
        if (attachment->getIsHUDAttachment())
        {
            for (LLViewerJointAttachment::attachedobjs_vec_t::iterator attachment_iter = attachment->mAttachedObjects.begin();
                 attachment_iter != attachment->mAttachedObjects.end();
                 ++attachment_iter)
            {
                const LLViewerObject* attached_object = attachment_iter->get();
                if (attached_object == NULL)
                {
                    LL_WARNS() << "HUD attached object is NULL!" << LL_ENDL;
                    continue;
                }
                // initialize bounding box to contain identity orientation and center point for attached object
                bbox.addPointLocal(attached_object->getPosition());
                // add rotated bounding box for attached object
                bbox.addBBoxAgent(attached_object->getBoundingBoxAgent());
                LLViewerObject::const_child_list_t& child_list = attached_object->getChildren();
                for (LLViewerObject::child_list_t::const_iterator iter = child_list.begin();
                     iter != child_list.end();
                     ++iter)
                {
                    const LLViewerObject* child_objectp = *iter;
                    bbox.addBBoxAgent(child_objectp->getBoundingBoxAgent());
                }
            }
        }
    }

    return bbox;
}

//-----------------------------------------------------------------------------
// onFirstTEMessageReceived()
//-----------------------------------------------------------------------------
void LLVOAvatar::onFirstTEMessageReceived()
{
    LL_DEBUGS("Avatar") << avString() << LL_ENDL;
    if( !mFirstTEMessageReceived )
    {
        mFirstTEMessageReceived = true;

        LLLoadedCallbackEntry::source_callback_list_t* src_callback_list = NULL ;
        bool paused = false ;
        if(!isSelf())
        {
            src_callback_list = &mCallbackTextureList ;
            paused = !isVisible();
        }

        for (U32 i = 0; i < mBakedTextureDatas.size(); i++)
        {
            const bool layer_baked = isTextureDefined(mBakedTextureDatas[i].mTextureIndex);

            // Use any baked textures that we have even if they haven't downloaded yet.
            // (That is, don't do a transition from unbaked to baked.)
            if (layer_baked)
            {
                LLViewerFetchedTexture* image = LLViewerTextureManager::staticCastToFetchedTexture(getImage( mBakedTextureDatas[i].mTextureIndex, 0 ), true) ;
                mBakedTextureDatas[i].mLastTextureID = image->getID();
                // If we have more than one texture for the other baked layers, we'll want to call this for them too.
                if ( (image->getID() != IMG_INVISIBLE) && ((i == BAKED_HEAD) || (i == BAKED_UPPER) || (i == BAKED_LOWER)) )
                {
                    image->setLoadedCallback( onBakedTextureMasksLoaded, MORPH_MASK_REQUESTED_DISCARD, true, true, new LLTextureMaskData( mID ),
                        src_callback_list, paused);
                }
                LL_DEBUGS("Avatar") << avString() << "layer_baked, setting onInitialBakedTextureLoaded as callback" << LL_ENDL;
                image->setLoadedCallback( onInitialBakedTextureLoaded, MAX_DISCARD_LEVEL, false, false, new LLUUID( mID ),
                    src_callback_list, paused );
                if (image->getDiscardLevel() < 0 && !paused)
                {
                    mLastTexCallbackAddedTime.reset();
                }
                               // this could add paused texture callbacks
                               mLoadedCallbacksPaused |= paused;
            }
        }

        mMeshTexturesDirty = true;
        gPipeline.markGLRebuild(this);

        mFirstAppearanceMessageTimer.reset();
        mFullyLoadedTimer.reset();
    }
}

//-----------------------------------------------------------------------------
// bool visualParamWeightsAreDefault()
//-----------------------------------------------------------------------------
bool LLVOAvatar::visualParamWeightsAreDefault()
{
    bool rtn = true;

    bool is_wearing_skirt = isWearingWearableType(LLWearableType::WT_SKIRT);
    for (LLVisualParam *param = getFirstVisualParam();
         param;
         param = getNextVisualParam())
    {
        if (param->isTweakable())
        {
            LLViewerVisualParam* vparam = dynamic_cast<LLViewerVisualParam*>(param);
            llassert(vparam);
            bool is_skirt_param = vparam &&
                LLWearableType::WT_SKIRT == vparam->getWearableType();
            if (param->getWeight() != param->getDefaultWeight() &&
                // we have to not care whether skirt weights are default, if we're not actually wearing a skirt
                (is_wearing_skirt || !is_skirt_param))
            {
                //LL_INFOS() << "param '" << param->getName() << "'=" << param->getWeight() << " which differs from default=" << param->getDefaultWeight() << LL_ENDL;
                rtn = false;
                break;
            }
        }
    }

    //LL_INFOS() << "params are default ? " << int(rtn) << LL_ENDL;

    return rtn;
}

void dump_visual_param(apr_file_t* file, LLVisualParam* viewer_param, F32 value)
{
    std::string type_string = "unknown";
    if (dynamic_cast<LLTexLayerParamAlpha*>(viewer_param))
        type_string = "param_alpha";
    if (dynamic_cast<LLTexLayerParamColor*>(viewer_param))
        type_string = "param_color";
    if (dynamic_cast<LLDriverParam*>(viewer_param))
        type_string = "param_driver";
    if (dynamic_cast<LLPolyMorphTarget*>(viewer_param))
        type_string = "param_morph";
    if (dynamic_cast<LLPolySkeletalDistortion*>(viewer_param))
        type_string = "param_skeleton";
    S32 wtype = -1;
    LLViewerVisualParam *vparam = dynamic_cast<LLViewerVisualParam*>(viewer_param);
    if (vparam)
    {
        wtype = vparam->getWearableType();
    }
    S32 u8_value = F32_to_U8(value,viewer_param->getMinWeight(),viewer_param->getMaxWeight());
    apr_file_printf(file, "\t\t<param id=\"%d\" name=\"%s\" display=\"%s\" value=\"%.3f\" u8=\"%d\" type=\"%s\" wearable=\"%s\" group=\"%d\"/>\n",
                    viewer_param->getID(), viewer_param->getName().c_str(), viewer_param->getDisplayName().c_str(), value, u8_value, type_string.c_str(),
                    LLWearableType::getInstance()->getTypeName(LLWearableType::EType(wtype)).c_str(),
                    viewer_param->getGroup());
    }


void LLVOAvatar::dumpAppearanceMsgParams( const std::string& dump_prefix,
    const LLAppearanceMessageContents& contents)
{
    std::string outfilename = get_sequential_numbered_file_name(dump_prefix, ".xml");
    const std::vector<F32>& params_for_dump = contents.mParamWeights;
    const LLTEContents& tec = contents.mTEContents;

    LLAPRFile outfile;
    std::string fullpath = gDirUtilp->getExpandedFilename(LL_PATH_LOGS, outfilename);
    outfile.open(fullpath, LL_APR_WB );
    apr_file_t* file = outfile.getFileHandle();
    if (!file)
    {
        return;
    }
    else
    {
        LL_DEBUGS("Avatar") << "dumping appearance message to " << fullpath << LL_ENDL;
    }

    apr_file_printf(file, "<header>\n");
    apr_file_printf(file, "\t\t<cof_version %i />\n", contents.mCOFVersion);
    apr_file_printf(file, "\t\t<appearance_version %i />\n", contents.mAppearanceVersion);
    apr_file_printf(file, "</header>\n");

    apr_file_printf(file, "\n<params>\n");
    LLVisualParam* param = getFirstVisualParam();
    for (S32 i = 0; i < params_for_dump.size(); i++)
    {
        while( param && ((param->getGroup() != VISUAL_PARAM_GROUP_TWEAKABLE) &&
                         (param->getGroup() != VISUAL_PARAM_GROUP_TRANSMIT_NOT_TWEAKABLE)) ) // should not be any of group VISUAL_PARAM_GROUP_TWEAKABLE_NO_TRANSMIT
        {
            param = getNextVisualParam();
        }
        LLViewerVisualParam* viewer_param = (LLViewerVisualParam*)param;
        F32 value = params_for_dump[i];
        dump_visual_param(file, viewer_param, value);
        param = getNextVisualParam();
    }
    apr_file_printf(file, "</params>\n");

    apr_file_printf(file, "\n<textures>\n");
    for (U32 i = 0; i < tec.face_count; i++)
    {
        std::string uuid_str;
        ((LLUUID*)tec.image_data)[i].toString(uuid_str);
        apr_file_printf( file, "\t\t<texture te=\"%i\" uuid=\"%s\"/>\n", i, uuid_str.c_str());
    }
    apr_file_printf(file, "</textures>\n");
}

void LLVOAvatar::parseAppearanceMessage(LLMessageSystem* mesgsys, LLAppearanceMessageContents& contents)
{
    parseTEMessage(mesgsys, _PREHASH_ObjectData, -1, contents.mTEContents);

    // Parse the AppearanceData field, if any.
    if (mesgsys->has(_PREHASH_AppearanceData))
    {
        U8 av_u8;
        mesgsys->getU8Fast(_PREHASH_AppearanceData, _PREHASH_AppearanceVersion, av_u8, 0);
        contents.mAppearanceVersion = av_u8;
        //LL_DEBUGS("Avatar") << "appversion set by AppearanceData field: " << contents.mAppearanceVersion << LL_ENDL;
        mesgsys->getS32Fast(_PREHASH_AppearanceData, _PREHASH_CofVersion, contents.mCOFVersion, 0);
        // For future use:
        //mesgsys->getU32Fast(_PREHASH_AppearanceData, _PREHASH_Flags, appearance_flags, 0);
    }

    // Parse the AppearanceHover field, if any.
    contents.mHoverOffsetWasSet = false;
    if (mesgsys->has(_PREHASH_AppearanceHover))
    {
        LLVector3 hover;
        mesgsys->getVector3Fast(_PREHASH_AppearanceHover, _PREHASH_HoverHeight, hover);
        //LL_DEBUGS("Avatar") << avString() << " hover received " << hover.mV[ VX ] << "," << hover.mV[ VY ] << "," << hover.mV[ VZ ] << LL_ENDL;
        contents.mHoverOffset = hover;
        contents.mHoverOffsetWasSet = true;
    }

    // Get attachment info, if sent
    LLUUID attachment_id;
    U8     attach_point;
    S32    attach_count = mesgsys->getNumberOfBlocksFast(_PREHASH_AttachmentBlock);
    LL_DEBUGS("AVAppearanceAttachments") << "Agent " << getID() << " has "
                                         << attach_count << " attachments" << LL_ENDL;
    size_t old_size = mSimAttachments.size();
    mSimAttachments.clear();
    for (S32 attach_i = 0; attach_i < attach_count; attach_i++)
    {
        mesgsys->getUUIDFast(_PREHASH_AttachmentBlock, _PREHASH_ID, attachment_id, attach_i);
        mesgsys->getU8Fast(_PREHASH_AttachmentBlock, _PREHASH_AttachmentPoint, attach_point, attach_i);
        LL_DEBUGS("AVAppearanceAttachments") << "AV " << getID() << " has attachment " << attach_i << " "
            << (attachment_id.isNull() ? "pending" : attachment_id.asString())
            << " on point " << (S32)attach_point << LL_ENDL;

        if (attachment_id.notNull())
        {
            mSimAttachments[attachment_id] = attach_point;
        }
        else
        {
            // at the moment viewer is only interested in non-null attachments
            LL_DEBUGS("AVAppearanceAttachments") << "AV " << getID()
                << " has null attachment on point " << (S32)attach_point
                << ", discarding" << LL_ENDL;
        }
    }

    // todo? Doesn't detect if attachments were switched
    if (old_size != mSimAttachments.size())
    {
        mLastCloudAttachmentCount = 0;
        mLastCloudAttachmentChangeTime.reset();
        if (!isFullyLoaded())
        {
            mFullyLoadedTimer.reset();
        }
    }

    // Parse visual params, if any.
    S32 num_blocks = mesgsys->getNumberOfBlocksFast(_PREHASH_VisualParam);
    if( num_blocks > 1)
    {
        //LL_DEBUGS("Avatar") << avString() << " handle visual params, num_blocks " << num_blocks << LL_ENDL;

        LLVisualParam* param = getFirstVisualParam();
        llassert(param); // if this ever fires, we should do the same as when num_blocks<=1
        if (!param)
        {
            LL_WARNS() << "No visual params!" << LL_ENDL;
        }
        else
        {
            for( S32 i = 0; i < num_blocks; i++ )
            {
                while( param && ((param->getGroup() != VISUAL_PARAM_GROUP_TWEAKABLE) &&
                                 (param->getGroup() != VISUAL_PARAM_GROUP_TRANSMIT_NOT_TWEAKABLE)) ) // should not be any of group VISUAL_PARAM_GROUP_TWEAKABLE_NO_TRANSMIT
                {
                    param = getNextVisualParam();
                }

                if( !param )
                {
                    // more visual params supplied than expected - just process what we know about
                    break;
                }

                U8 value;
                mesgsys->getU8Fast(_PREHASH_VisualParam, _PREHASH_ParamValue, value, i);
                F32 newWeight = U8_to_F32(value, param->getMinWeight(), param->getMaxWeight());
                contents.mParamWeights.push_back(newWeight);
                contents.mParams.push_back(param);

                param = getNextVisualParam();
            }
        }

        const S32 expected_tweakable_count = getVisualParamCountInGroup(VISUAL_PARAM_GROUP_TWEAKABLE) +
                                             getVisualParamCountInGroup(VISUAL_PARAM_GROUP_TRANSMIT_NOT_TWEAKABLE); // don't worry about VISUAL_PARAM_GROUP_TWEAKABLE_NO_TRANSMIT
        if (num_blocks != expected_tweakable_count)
        {
            LL_DEBUGS("Avatar") << "Number of params in AvatarAppearance msg (" << num_blocks << ") does not match number of tweakable params in avatar xml file (" << expected_tweakable_count << ").  Processing what we can.  object: " << getID() << LL_ENDL;
        }
    }
    else
    {
            LL_DEBUGS("Avatar") << "AvatarAppearance msg received without any parameters, object: " << getID() << LL_ENDL;
        }

    LLVisualParam* appearance_version_param = getVisualParam(11000);
    if (appearance_version_param)
    {
        std::vector<LLVisualParam*>::iterator it = std::find(contents.mParams.begin(), contents.mParams.end(),appearance_version_param);
        if (it != contents.mParams.end())
        {
            S32 index = (S32)(it - contents.mParams.begin());
            contents.mParamAppearanceVersion = ll_round(contents.mParamWeights[index]);
            //LL_DEBUGS("Avatar") << "appversion req by appearance_version param: " << contents.mParamAppearanceVersion << LL_ENDL;
        }
    }
}

bool resolve_appearance_version(const LLAppearanceMessageContents& contents, S32& appearance_version)
{
    appearance_version = -1;

    if ((contents.mAppearanceVersion) >= 0 &&
        (contents.mParamAppearanceVersion >= 0) &&
        (contents.mAppearanceVersion != contents.mParamAppearanceVersion))
    {
        LL_WARNS() << "inconsistent appearance_version settings - field: " <<
            contents.mAppearanceVersion << ", param: " <<  contents.mParamAppearanceVersion << LL_ENDL;
        return false;
    }
    if (contents.mParamAppearanceVersion >= 0) // use visual param if available.
    {
        appearance_version = contents.mParamAppearanceVersion;
    }
    else if (contents.mAppearanceVersion > 0)
    {
        appearance_version = contents.mAppearanceVersion;
    }
    else // still not set, go with 1.
    {
        appearance_version = 1;
    }
    //LL_DEBUGS("Avatar") << "appearance version info - field " << contents.mAppearanceVersion
    //                  << " param: " << contents.mParamAppearanceVersion
    //                  << " final: " << appearance_version << LL_ENDL;
    return true;
}

//-----------------------------------------------------------------------------
// processAvatarAppearance()
//-----------------------------------------------------------------------------
void LLVOAvatar::processAvatarAppearance( LLMessageSystem* mesgsys )
{
    LL_DEBUGS("Avatar") << "starts" << LL_ENDL;

    static LLCachedControl<bool> enable_verbose_dumps(gSavedSettings, "DebugAvatarAppearanceMessage");
    static LLCachedControl<bool> block_avatar_appearance_messages(gSavedSettings, "BlockAvatarAppearanceMessages");

    std::string dump_prefix = getDebugName() + (isSelf() ? "_s_" : "_o_");
    if (block_avatar_appearance_messages)
    {
        LL_WARNS() << "Blocking AvatarAppearance message" << LL_ENDL;
        return;
    }

    mLastAppearanceMessageTimer.reset();

    LLPointer<LLAppearanceMessageContents> contents(new LLAppearanceMessageContents);
    parseAppearanceMessage(mesgsys, *contents);
    if (enable_verbose_dumps)
    {
        dumpAppearanceMsgParams(dump_prefix + "appearance_msg", *contents);
    }

    S32 appearance_version;
    if (!resolve_appearance_version(*contents, appearance_version))
    {
        LL_WARNS() << "bad appearance version info, discarding" << LL_ENDL;
        return;
    }
    llassert(appearance_version > 0);
    if (appearance_version > 1)
    {
        LL_WARNS() << "unsupported appearance version " << appearance_version << ", discarding appearance message" << LL_ENDL;
        return;
    }

    S32 thisAppearanceVersion(contents->mCOFVersion);
    if (isSelf())
    {   // In the past this was considered to be the canonical COF version,
        // that is no longer the case.  The canonical version is maintained
        // by the AIS code and should match the COF version there. Even so,
        // we must prevent rolling this one backwards backwards or processing
        // stale versions.

        S32 aisCOFVersion(LLAppearanceMgr::instance().getCOFVersion());

        LL_DEBUGS("Avatar") << "handling self appearance message #" << thisAppearanceVersion <<
            " (highest seen #" << mLastUpdateReceivedCOFVersion <<
            ") (AISCOF=#" << aisCOFVersion << ")" << LL_ENDL;

        if (mLastUpdateReceivedCOFVersion >= thisAppearanceVersion)
        {
            LL_WARNS("Avatar") << "Stale appearance received #" << thisAppearanceVersion <<
                " attempt to roll back from #" << mLastUpdateReceivedCOFVersion <<
                "... dropping." << LL_ENDL;
            return;
        }
        if (isEditingAppearance())
        {
            LL_DEBUGS("Avatar") << "Editing appearance.  Dropping appearance update." << LL_ENDL;
            return;
        }

    }

    // SUNSHINE CLEANUP - is this case OK now?
    auto num_params = contents->mParamWeights.size();
    if (num_params <= 1)
    {
        // In this case, we have no reliable basis for knowing
        // appearance version, which may cause us to look for baked
        // textures in the wrong place and flag them as missing
        // assets.
        LL_DEBUGS("Avatar") << "ignoring appearance message due to lack of params" << LL_ENDL;
        return;
    }

    // No backsies zone - if we get here, the message should be valid and usable, will be processed.
    // Note:
    // RequestAgentUpdateAppearanceResponder::onRequestRequested()
    // assumes that cof version is only updated with server-bake
    // appearance messages.
    if (isSelf())
    {
        LL_INFOS("Avatar") << "Processing appearance message version " << thisAppearanceVersion << LL_ENDL;
    }
    else
    {
        LL_INFOS("Avatar") << "Processing appearance message for " << getID() << ", version " << thisAppearanceVersion << LL_ENDL;
    }

    // Note:
    // locally the COF is maintained via LLInventoryModel::accountForUpdate
    // which is called from various places.  This should match the simhost's
    // idea of what the COF version is.  AIS however maintains its own version
    // of the COF that should be considered canonical.
    mLastUpdateReceivedCOFVersion = thisAppearanceVersion;

    mLastProcessedAppearance = contents;

    bool slam_params = false;
    applyParsedAppearanceMessage(*contents, slam_params);
    if (getOverallAppearance() != AOA_NORMAL)
    {
        resetSkeleton(false);
    }
}

void LLVOAvatar::applyParsedAppearanceMessage(LLAppearanceMessageContents& contents, bool slam_params)
{
    auto num_params = contents.mParamWeights.size();
    ESex old_sex = getSex();

    if (applyParsedTEMessage(contents.mTEContents) > 0 && isChanged(TEXTURE))
    {
        updateVisualComplexity();
    }

    // prevent the overwriting of valid baked textures with invalid baked textures
    for (U8 baked_index = 0; baked_index < mBakedTextureDatas.size(); baked_index++)
    {
        if (!isTextureDefined(mBakedTextureDatas[baked_index].mTextureIndex)
            && mBakedTextureDatas[baked_index].mLastTextureID != IMG_DEFAULT
            && baked_index != BAKED_SKIRT && baked_index != BAKED_LEFT_ARM && baked_index != BAKED_LEFT_LEG && baked_index != BAKED_AUX1 && baked_index != BAKED_AUX2 && baked_index != BAKED_AUX3)
        {
            LL_DEBUGS("Avatar") << avString() << " baked_index " << (S32) baked_index << " using mLastTextureID " << mBakedTextureDatas[baked_index].mLastTextureID << LL_ENDL;
            setTEImage(mBakedTextureDatas[baked_index].mTextureIndex,
                LLViewerTextureManager::getFetchedTexture(mBakedTextureDatas[baked_index].mLastTextureID, FTT_DEFAULT, true, LLGLTexture::BOOST_NONE, LLViewerTexture::LOD_TEXTURE));
        }
        else
        {
            LL_DEBUGS("Avatar") << avString() << " baked_index " << (S32) baked_index << " using texture id "
                                << getTE(mBakedTextureDatas[baked_index].mTextureIndex)->getID() << LL_ENDL;
        }
    }

    // runway - was
    // if (!is_first_appearance_message )
    // which means it would be called on second appearance message - probably wrong.
    bool is_first_appearance_message = !mFirstAppearanceMessageReceived;
    mFirstAppearanceMessageReceived = true;

    //LL_DEBUGS("Avatar") << avString() << "processAvatarAppearance start " << mID
    //                    << " first? " << is_first_appearance_message << " self? " << isSelf() << LL_ENDL;

    if (is_first_appearance_message )
    {
        onFirstTEMessageReceived();
    }

    setCompositeUpdatesEnabled( false );
    gPipeline.markGLRebuild(this);

    // Apply visual params
    if( num_params > 1)
    {
        //LL_DEBUGS("Avatar") << avString() << " handle visual params, num_params " << num_params << LL_ENDL;
        bool params_changed = false;
        bool interp_params = false;
        S32 params_changed_count = 0;

        for( size_t i = 0; i < num_params; i++ )
        {
            LLVisualParam* param = contents.mParams[i];
            F32 newWeight = contents.mParamWeights[i];

            if (slam_params || is_first_appearance_message || (param->getWeight() != newWeight))
            {
                params_changed = true;
                params_changed_count++;

                if(is_first_appearance_message || slam_params)
                {
                    //LL_DEBUGS("Avatar") << "param slam " << i << " " << newWeight << LL_ENDL;
                    param->setWeight(newWeight);
                }
                else
                {
                    interp_params = true;
                    param->setAnimationTarget(newWeight);
                }
            }
        }
        const S32 expected_tweakable_count = getVisualParamCountInGroup(VISUAL_PARAM_GROUP_TWEAKABLE) +
                                             getVisualParamCountInGroup(VISUAL_PARAM_GROUP_TRANSMIT_NOT_TWEAKABLE); // don't worry about VISUAL_PARAM_GROUP_TWEAKABLE_NO_TRANSMIT
        if (num_params != expected_tweakable_count)
        {
            LL_DEBUGS("Avatar") << "Number of params in AvatarAppearance msg (" << num_params << ") does not match number of tweakable params in avatar xml file (" << expected_tweakable_count << ").  Processing what we can.  object: " << getID() << LL_ENDL;
        }

        LL_DEBUGS("Avatar") << "Changed " << params_changed_count << " params" << LL_ENDL;
        if (params_changed)
        {
            if (interp_params)
            {
                startAppearanceAnimation();
            }
            updateVisualParams();

            ESex new_sex = getSex();
            if( old_sex != new_sex )
            {
                updateSexDependentLayerSets();
            }
        }

        llassert( getSex() == ((getVisualParamWeight( "male" ) > 0.5f) ? SEX_MALE : SEX_FEMALE) );
    }
    else
    {
        // AvatarAppearance message arrived without visual params
        LL_DEBUGS("Avatar") << avString() << "no visual params" << LL_ENDL;

        const F32 LOADING_TIMEOUT_SECONDS = 60.f;
        // this isn't really a problem if we already have a non-default shape
        if (visualParamWeightsAreDefault() && mRuthTimer.getElapsedTimeF32() > LOADING_TIMEOUT_SECONDS)
        {
            // re-request appearance, hoping that it comes back with a shape next time
            LL_INFOS() << "Re-requesting AvatarAppearance for object: "  << getID() << LL_ENDL;
            LLAvatarPropertiesProcessor::getInstance()->sendAvatarTexturesRequest(getID());
            mRuthTimer.reset();
        }
        else
        {
            LL_INFOS() << "That's okay, we already have a non-default shape for object: "  << getID() << LL_ENDL;
            // we don't really care.
        }
    }

    if (contents.mHoverOffsetWasSet && !isSelf())
    {
        // Got an update for some other avatar
        // Ignore updates for self, because we have a more authoritative value in the preferences.
        setHoverOffset(contents.mHoverOffset);
        LL_DEBUGS("Avatar") << avString() << "setting hover to " << contents.mHoverOffset[2] << LL_ENDL;
    }

    if (!contents.mHoverOffsetWasSet && !isSelf())
    {
        // If we don't get a value at all, we are presumably in a
        // region that does not support hover height.
        LL_WARNS() << avString() << "zeroing hover because not defined in appearance message" << LL_ENDL;
        setHoverOffset(LLVector3(0.0, 0.0, 0.0));
    }

    setCompositeUpdatesEnabled( true );

    // If all of the avatars are completely baked, release the global image caches to conserve memory.
    cullAvatarsByPixelArea();

    if (isSelf())
    {
        mUseLocalAppearance = false;
    }

    updateMeshTextures();
    updateMeshVisibility();

}

LLViewerTexture* LLVOAvatar::getBakedTexture(const U8 te)
{
    if (te < 0 || te >= BAKED_NUM_INDICES)
    {
        return NULL;
    }

    bool is_layer_baked = isTextureDefined(mBakedTextureDatas[te].mTextureIndex);

    LLViewerTexLayerSet* layerset = NULL;
    layerset = getTexLayerSet(te);


    if (!isEditingAppearance() && is_layer_baked)
    {
        LLViewerFetchedTexture* baked_img = LLViewerTextureManager::staticCastToFetchedTexture(getImage(mBakedTextureDatas[te].mTextureIndex, 0), true);
        return baked_img;
    }
    else if (layerset && isEditingAppearance())
    {
        layerset->createComposite();
        layerset->setUpdatesEnabled(true);

        return layerset->getViewerComposite();
    }

    return NULL;


}


// static
void LLVOAvatar::getAnimLabels( std::vector<std::string>* labels )
{
    S32 i;
    labels->reserve(gUserAnimStatesCount);
    for( i = 0; i < gUserAnimStatesCount; i++ )
    {
        labels->push_back( LLAnimStateLabels::getStateLabel( gUserAnimStates[i].mName ) );
    }

    // Special case to trigger away (AFK) state
    labels->push_back( "Away From Keyboard" );
}

// static
void LLVOAvatar::getAnimNames( std::vector<std::string>* names )
{
    S32 i;

    names->reserve(gUserAnimStatesCount);
    for( i = 0; i < gUserAnimStatesCount; i++ )
    {
        names->push_back( std::string(gUserAnimStates[i].mName) );
    }

    // Special case to trigger away (AFK) state
    names->push_back( "enter_away_from_keyboard_state" );
}

// static
void LLVOAvatar::onBakedTextureMasksLoaded( bool success, LLViewerFetchedTexture *src_vi, LLImageRaw* src, LLImageRaw* aux_src, S32 discard_level, bool final, void* userdata )
{
    if (!userdata) return;

    //LL_INFOS() << "onBakedTextureMasksLoaded: " << src_vi->getID() << LL_ENDL;
    const LLUUID id = src_vi->getID();

    LLTextureMaskData* maskData = (LLTextureMaskData*) userdata;
    LLVOAvatar* self = (LLVOAvatar*) gObjectList.findObject( maskData->mAvatarID );

    // if discard level is 2 less than last discard level we processed, or we hit 0,
    // then generate morph masks
    if(self && success && (discard_level < maskData->mLastDiscardLevel - 2 || discard_level == 0))
    {
        if(aux_src && aux_src->getComponents() == 1)
        {
            LLImageDataSharedLock lock(aux_src);

            if (!aux_src->getData())
            {
                LL_ERRS() << "No auxiliary source (morph mask) data for image id " << id << LL_ENDL;
                return;
            }

            /* if( id == head_baked->getID() )
                 if (self->mBakedTextureDatas[BAKED_HEAD].mTexLayerSet)
                     //LL_INFOS() << "onBakedTextureMasksLoaded for head " << id << " discard = " << discard_level << LL_ENDL;
                     self->mBakedTextureDatas[BAKED_HEAD].mTexLayerSet->applyMorphMask(aux_src->getData(), aux_src->getWidth(), aux_src->getHeight(), 1);
                     maskData->mLastDiscardLevel = discard_level; */
            bool found_texture_id = false;
            for (LLAvatarAppearanceDictionary::Textures::const_iterator iter = LLAvatarAppearance::getDictionary()->getTextures().begin();
                 iter != LLAvatarAppearance::getDictionary()->getTextures().end();
                 ++iter)
            {

                const LLAvatarAppearanceDictionary::TextureEntry *texture_dict = iter->second;
                if (texture_dict->mIsUsedByBakedTexture)
                {
                    const ETextureIndex texture_index = iter->first;
                    const LLViewerTexture *baked_img = self->getImage(texture_index, 0);
                    if (baked_img && id == baked_img->getID())
                    {
                        const EBakedTextureIndex baked_index = texture_dict->mBakedTextureIndex;
                        self->applyMorphMask(aux_src->getData(), aux_src->getWidth(), aux_src->getHeight(), 1, baked_index);
                        maskData->mLastDiscardLevel = discard_level;
                        found_texture_id = true;
                        break;
                    }
                }
            }
            if (!found_texture_id)
            {
                LL_INFOS() << "unexpected image id: " << id << LL_ENDL;
            }
            self->dirtyMesh();
        }
        else
        {
            // this can happen when someone uses an old baked texture possibly provided by
            // viewer-side baked texture caching
            LL_WARNS() << "Masks loaded callback but NO aux source, id " << id << LL_ENDL;
        }
    }

    if (final || !success)
    {
        delete maskData;
    }
}

// static
void LLVOAvatar::onInitialBakedTextureLoaded( bool success, LLViewerFetchedTexture *src_vi, LLImageRaw* src, LLImageRaw* aux_src, S32 discard_level, bool final, void* userdata )
{
    LLUUID *avatar_idp = (LLUUID *)userdata;
    LLVOAvatar *selfp = (LLVOAvatar *)gObjectList.findObject(*avatar_idp);

    if (selfp)
    {
        //LL_DEBUGS("Avatar") << selfp->avString() << "discard_level " << discard_level << " success " << success << " final " << final << LL_ENDL;
    }

    if (!success && selfp)
    {
        selfp->removeMissingBakedTextures();
    }
    if (final || !success )
    {
        delete avatar_idp;
    }
}

// Static
void LLVOAvatar::onBakedTextureLoaded(bool success,
                                      LLViewerFetchedTexture *src_vi, LLImageRaw* src, LLImageRaw* aux_src,
                                      S32 discard_level, bool final, void* userdata)
{
    //LL_DEBUGS("Avatar") << "onBakedTextureLoaded: " << src_vi->getID() << LL_ENDL;

    LLUUID id = src_vi->getID();
    LLUUID *avatar_idp = (LLUUID *)userdata;
    LLVOAvatar *selfp = (LLVOAvatar *)gObjectList.findObject(*avatar_idp);
    if (selfp)
    {
        //LL_DEBUGS("Avatar") << selfp->avString() << "discard_level " << discard_level << " success " << success << " final " << final << " id " << src_vi->getID() << LL_ENDL;
    }

    if (selfp && !success)
    {
        selfp->removeMissingBakedTextures();
    }

    if( final || !success )
    {
        delete avatar_idp;
    }

    if( selfp && success && final )
    {
        selfp->useBakedTexture( id );
    }
}


// Called when baked texture is loaded and also when we start up with a baked texture
void LLVOAvatar::useBakedTexture( const LLUUID& id )
{
    for (U32 i = 0; i < mBakedTextureDatas.size(); i++)
    {
        LLViewerTexture* image_baked = getImage( mBakedTextureDatas[i].mTextureIndex, 0 );
        if (id == image_baked->getID())
        {
            //LL_DEBUGS("Avatar") << avString() << " i " << i << " id " << id << LL_ENDL;
            mBakedTextureDatas[i].mIsLoaded = true;
            mBakedTextureDatas[i].mLastTextureID = id;
            mBakedTextureDatas[i].mIsUsed = true;

            if (isUsingLocalAppearance())
            {
                LL_INFOS() << "not changing to baked texture while isUsingLocalAppearance" << LL_ENDL;
            }
            else
            {
                debugColorizeSubMeshes(i,LLColor4::green);

                avatar_joint_mesh_list_t::iterator iter = mBakedTextureDatas[i].mJointMeshes.begin();
                avatar_joint_mesh_list_t::iterator end  = mBakedTextureDatas[i].mJointMeshes.end();
                for (; iter != end; ++iter)
                {
                    LLAvatarJointMesh* mesh = (*iter);
                    if (mesh)
                    {
                        mesh->setTexture( image_baked );
                    }
                }
            }

            const LLAvatarAppearanceDictionary::BakedEntry *baked_dict =
                LLAvatarAppearance::getDictionary()->getBakedTexture((EBakedTextureIndex)i);
            for (texture_vec_t::const_iterator local_tex_iter = baked_dict->mLocalTextures.begin();
                 local_tex_iter != baked_dict->mLocalTextures.end();
                 ++local_tex_iter)
            {
                if (isSelf()) setBakedReady(*local_tex_iter, true);
            }

            // ! BACKWARDS COMPATIBILITY !
            // Workaround for viewing avatars from old viewers that haven't baked hair textures.
            // This is paired with similar code in updateMeshTextures that sets hair mesh color.
            if (i == BAKED_HAIR)
            {
                avatar_joint_mesh_list_t::iterator iter = mBakedTextureDatas[i].mJointMeshes.begin();
                avatar_joint_mesh_list_t::iterator end  = mBakedTextureDatas[i].mJointMeshes.end();
                for (; iter != end; ++iter)
                {
                    LLAvatarJointMesh* mesh = (*iter);
                    if (mesh)
                    {
                        mesh->setColor( LLColor4::white );
                    }
                }
            }
        }
    }

    dirtyMesh();
}

std::string get_sequential_numbered_file_name(const std::string& prefix,
                                              const std::string& suffix)
{
    typedef std::map<std::string,S32> file_num_type;
    static  file_num_type file_nums;
    file_num_type::iterator it = file_nums.find(prefix);
    S32 num = 0;
    if (it != file_nums.end())
    {
        num = it->second;
    }
    file_nums[prefix] = num+1;
    std::string outfilename = prefix + " " + llformat("%04d",num) + ".xml";
    std::replace(outfilename.begin(),outfilename.end(),' ','_');
    return outfilename;
}

void dump_sequential_xml(const std::string outprefix, const LLSD& content)
{
    std::string outfilename = get_sequential_numbered_file_name(outprefix,".xml");
    std::string fullpath = gDirUtilp->getExpandedFilename(LL_PATH_LOGS,outfilename);
    llofstream ofs(fullpath.c_str(), std::ios_base::out);
    ofs << LLSDOStreamer<LLSDXMLFormatter>(content, LLSDFormatter::OPTIONS_PRETTY);
    LL_DEBUGS("Avatar") << "results saved to: " << fullpath << LL_ENDL;
}

void LLVOAvatar::getSortedJointNames(S32 joint_type, std::vector<std::string>& result) const
{
    result.clear();
    if (joint_type==0)
    {
        avatar_joint_list_t::const_iterator iter = mSkeleton.begin();
        avatar_joint_list_t::const_iterator end  = mSkeleton.end();
        for (; iter != end; ++iter)
        {
            LLJoint* pJoint = (*iter);
            result.push_back(pJoint->getName());
        }
    }
    else if (joint_type==1)
    {
        for (S32 i = 0; i < mNumCollisionVolumes; i++)
        {
            LLAvatarJointCollisionVolume* pJoint = &mCollisionVolumes[i];
            result.push_back(pJoint->getName());
        }
    }
    else if (joint_type==2)
    {
        for (LLVOAvatar::attachment_map_t::const_iterator iter = mAttachmentPoints.begin();
             iter != mAttachmentPoints.end(); ++iter)
        {
            LLViewerJointAttachment* pJoint = iter->second;
            if (!pJoint) continue;
            result.push_back(pJoint->getName());
        }
    }
    std::sort(result.begin(), result.end());
}

void LLVOAvatar::dumpArchetypeXML(const std::string& prefix, bool group_by_wearables )
{
    std::string outprefix(prefix);
    if (outprefix.empty())
    {
        outprefix = getFullname() + (isSelf() ? "_s" : "_o");
    }
    if (outprefix.empty())
    {
        outprefix = std::string("new_archetype");
    }
    std::string outfilename = get_sequential_numbered_file_name(outprefix, ".xml");
    LLFilePickerReplyThread::startPicker(boost::bind(&LLVOAvatar::dumpArchetypeXMLCallback, this, _1, group_by_wearables), LLFilePicker::FFSAVE_XML, outfilename);
}

void LLVOAvatar::dumpArchetypeXMLCallback(const std::vector<std::string>& filenames, bool group_by_wearables)
{
    LLAPRFile outfile;
    LLWearableType *wr_inst = LLWearableType::getInstance();
    std::string fullpath = filenames[0];
    if (APR_SUCCESS == outfile.open(fullpath, LL_APR_WB ))
    {
        apr_file_t* file = outfile.getFileHandle();
        LL_INFOS() << "xmlfile write handle obtained : " << fullpath << LL_ENDL;

        apr_file_printf( file, "<?xml version=\"1.0\" encoding=\"US-ASCII\" standalone=\"yes\"?>\n" );
        apr_file_printf( file, "<linden_genepool version=\"1.0\">\n" );
        apr_file_printf( file, "\n\t<archetype name=\"???\">\n" );

        bool agent_is_godlike = gAgent.isGodlikeWithoutAdminMenuFakery();

        if (group_by_wearables)
        {
            for (S32 type = LLWearableType::WT_SHAPE; type < LLWearableType::WT_COUNT; type++)
            {
                const std::string& wearable_name = wr_inst->getTypeName((LLWearableType::EType)type);
                apr_file_printf( file, "\n\t\t<!-- wearable: %s -->\n", wearable_name.c_str() );

                for (LLVisualParam* param = getFirstVisualParam(); param; param = getNextVisualParam())
                {
                    LLViewerVisualParam* viewer_param = (LLViewerVisualParam*)param;
                    if( (viewer_param->getWearableType() == type) &&
                       (viewer_param->isTweakable() ) )
                    {
                        dump_visual_param(file, viewer_param, viewer_param->getWeight());
                    }
                }

                for (U8 te = 0; te < TEX_NUM_INDICES; te++)
                {
                    if (LLAvatarAppearance::getDictionary()->getTEWearableType((ETextureIndex)te) == type)
                    {
                        // MULTIPLE_WEARABLES: extend to multiple wearables?
                        LLViewerTexture* te_image = getImage((ETextureIndex)te, 0);
                        if( te_image )
                        {
                            std::string uuid_str = LLUUID().asString();
                            if (agent_is_godlike)
                            {
                                te_image->getID().toString(uuid_str);
                            }
                            apr_file_printf( file, "\t\t<texture te=\"%i\" uuid=\"%s\"/>\n", te, uuid_str.c_str());
                        }
                    }
                }
            }
        }
        else
        {
            // Just dump all params sequentially.
            for (LLVisualParam* param = getFirstVisualParam(); param; param = getNextVisualParam())
            {
                LLViewerVisualParam* viewer_param = (LLViewerVisualParam*)param;
                dump_visual_param(file, viewer_param, viewer_param->getWeight());
            }

            for (U8 te = 0; te < TEX_NUM_INDICES; te++)
            {
                // MULTIPLE_WEARABLES: extend to multiple wearables?
                LLViewerTexture* te_image = getImage((ETextureIndex)te, 0);
                if( te_image )
                {
                    std::string uuid_str = LLUUID().asString();
                    if (agent_is_godlike)
                    {
                        te_image->getID().toString(uuid_str);
                    }
                    apr_file_printf( file, "\t\t<texture te=\"%i\" uuid=\"%s\"/>\n", te, uuid_str.c_str());
                }
            }
        }

        // Root joint
        const LLVector3& pos = mRoot->getPosition();
        const LLVector3& scale = mRoot->getScale();
        apr_file_printf( file, "\t\t<root name=\"%s\" position=\"%f %f %f\" scale=\"%f %f %f\"/>\n",
                         mRoot->getName().c_str(), pos[0], pos[1], pos[2], scale[0], scale[1], scale[2]);

        // Bones
        std::vector<std::string> bone_names, cv_names, attach_names, all_names;
        getSortedJointNames(0, bone_names);
        getSortedJointNames(1, cv_names);
        getSortedJointNames(2, attach_names);
        all_names.insert(all_names.end(), bone_names.begin(), bone_names.end());
        all_names.insert(all_names.end(), cv_names.begin(), cv_names.end());
        all_names.insert(all_names.end(), attach_names.begin(), attach_names.end());

        for (std::vector<std::string>::iterator name_iter = bone_names.begin();
             name_iter != bone_names.end(); ++name_iter)
        {
            LLJoint *pJoint = getJoint(*name_iter);
            const LLVector3& pos = pJoint->getPosition();
            const LLVector3& scale = pJoint->getScale();
            apr_file_printf( file, "\t\t<bone name=\"%s\" position=\"%f %f %f\" scale=\"%f %f %f\"/>\n",
                             pJoint->getName().c_str(), pos[0], pos[1], pos[2], scale[0], scale[1], scale[2]);
        }

        // Collision volumes
        for (std::vector<std::string>::iterator name_iter = cv_names.begin();
             name_iter != cv_names.end(); ++name_iter)
        {
            LLJoint *pJoint = getJoint(*name_iter);
            const LLVector3& pos = pJoint->getPosition();
            const LLVector3& scale = pJoint->getScale();
            apr_file_printf( file, "\t\t<collision_volume name=\"%s\" position=\"%f %f %f\" scale=\"%f %f %f\"/>\n",
                             pJoint->getName().c_str(), pos[0], pos[1], pos[2], scale[0], scale[1], scale[2]);
        }

        // Attachment joints
        for (std::vector<std::string>::iterator name_iter = attach_names.begin();
             name_iter != attach_names.end(); ++name_iter)
        {
            LLJoint *pJoint = getJoint(*name_iter);
            if (!pJoint) continue;
            const LLVector3& pos = pJoint->getPosition();
            const LLVector3& scale = pJoint->getScale();
            apr_file_printf( file, "\t\t<attachment_point name=\"%s\" position=\"%f %f %f\" scale=\"%f %f %f\"/>\n",
                             pJoint->getName().c_str(), pos[0], pos[1], pos[2], scale[0], scale[1], scale[2]);
        }

        // Joint pos overrides
        for (std::vector<std::string>::iterator name_iter = all_names.begin();
             name_iter != all_names.end(); ++name_iter)
        {
            LLJoint *pJoint = getJoint(*name_iter);

            LLVector3 pos;
            LLUUID mesh_id;

            if (pJoint && pJoint->hasAttachmentPosOverride(pos,mesh_id))
            {
                S32 num_pos_overrides;
                std::set<LLVector3> distinct_pos_overrides;
                pJoint->getAllAttachmentPosOverrides(num_pos_overrides, distinct_pos_overrides);
                apr_file_printf( file, "\t\t<joint_offset name=\"%s\" position=\"%f %f %f\" mesh_id=\"%s\" count=\"%d\" distinct=\"%d\"/>\n",
                                 pJoint->getName().c_str(), pos[0], pos[1], pos[2], mesh_id.asString().c_str(),
                                 num_pos_overrides, (S32) distinct_pos_overrides.size());
            }
        }
        // Joint scale overrides
        for (std::vector<std::string>::iterator name_iter = all_names.begin();
             name_iter != all_names.end(); ++name_iter)
        {
            LLJoint *pJoint = getJoint(*name_iter);

            LLVector3 scale;
            LLUUID mesh_id;

            if (pJoint && pJoint->hasAttachmentScaleOverride(scale,mesh_id))
            {
                S32 num_scale_overrides;
                std::set<LLVector3> distinct_scale_overrides;
                pJoint->getAllAttachmentPosOverrides(num_scale_overrides, distinct_scale_overrides);
                apr_file_printf( file, "\t\t<joint_scale name=\"%s\" scale=\"%f %f %f\" mesh_id=\"%s\" count=\"%d\" distinct=\"%d\"/>\n",
                                 pJoint->getName().c_str(), scale[0], scale[1], scale[2], mesh_id.asString().c_str(),
                                 num_scale_overrides, (S32) distinct_scale_overrides.size());
            }
        }
        F32 pelvis_fixup;
        LLUUID mesh_id;
        if (hasPelvisFixup(pelvis_fixup, mesh_id))
        {
            apr_file_printf( file, "\t\t<pelvis_fixup z=\"%f\" mesh_id=\"%s\"/>\n",
                             pelvis_fixup, mesh_id.asString().c_str());
        }

        LLVector3 rp = getRootJoint()->getWorldPosition();
        LLVector4a rpv;
        rpv.load3(rp.mV);

        for (S32 joint_num = 0; joint_num < LL_CHARACTER_MAX_ANIMATED_JOINTS; joint_num++)
        {
            LLJoint *joint = getJoint(joint_num);
            if (joint_num < mJointRiggingInfoTab.size())
            {
                LLJointRiggingInfo& rig_info = mJointRiggingInfoTab[joint_num];
                if (rig_info.isRiggedTo())
                {
                    LLMatrix4a mat;
                    LLVector4a new_extents[2];
                    mat.loadu(joint->getWorldMatrix());
                    matMulBoundBox(mat, rig_info.getRiggedExtents(), new_extents);
                    LLVector4a rrp[2];
                    rrp[0].setSub(new_extents[0],rpv);
                    rrp[1].setSub(new_extents[1],rpv);
                    apr_file_printf( file, "\t\t<joint_rig_info num=\"%d\" name=\"%s\" min=\"%f %f %f\" max=\"%f %f %f\" tmin=\"%f %f %f\" tmax=\"%f %f %f\"/>\n",
                                     joint_num,
                                     joint->getName().c_str(),
                                     rig_info.getRiggedExtents()[0][0],
                                     rig_info.getRiggedExtents()[0][1],
                                     rig_info.getRiggedExtents()[0][2],
                                     rig_info.getRiggedExtents()[1][0],
                                     rig_info.getRiggedExtents()[1][1],
                                     rig_info.getRiggedExtents()[1][2],
                                     rrp[0][0],
                                     rrp[0][1],
                                     rrp[0][2],
                                     rrp[1][0],
                                     rrp[1][1],
                                     rrp[1][2] );
                }
            }
        }

        bool ultra_verbose = false;
        if (isSelf() && ultra_verbose)
        {
            // show the cloned params inside the wearables as well.
            gAgentAvatarp->dumpWearableInfo(outfile);
        }

        apr_file_printf( file, "\t</archetype>\n" );
        apr_file_printf( file, "\n</linden_genepool>\n" );

        LLSD args;
        args["PATH"] = fullpath;
        LLNotificationsUtil::add("AppearanceToXMLSaved", args);
    }
    else
    {
        LLNotificationsUtil::add("AppearanceToXMLFailed");
    }
    // File will close when handle goes out of scope
    LL_INFOS("DumpArchetypeXML") << "Archetype xml written successfully!" << LL_ENDL;
    LLNotificationsUtil::add("DumpArchetypeSuccess", LLSD().with("FILE_PATH", fullpath));
}


void LLVOAvatar::setVisibilityRank(U32 rank)
{
    if (mDrawable.notNull() && !mDrawable->isDead())
    {
        mVisibilityRank = rank;
    }
}

// Assumes LLVOAvatar::sInstances has already been sorted.
S32 LLVOAvatar::getUnbakedPixelAreaRank()
{
    S32 rank = 1;

    for (LLCharacter* character : LLCharacter::sInstances)
    {
        if (character == this)
        {
            return rank;
        }

        LLVOAvatar* avatar = (LLVOAvatar*)character;
        if (!avatar->isDead() && !avatar->isFullyBaked())
        {
            rank++;
        }
    }

    llassert(0);
    return 0;
}

// static
void LLVOAvatar::cullAvatarsByPixelArea()
{
    // VK (#31): LL's octree visibility (mDrawable->isVisible(), updateVisibility) is not
    // maintained under VK -- the GL cull it comes from doesn't run. Rank on live inputs
    // instead: in the main camera frustum (spatial extents, refreshed every frame by the
    // VK avatar pass) and the on-screen pixel area (setPixelAreaAndAngle, every idle).
    {
        LLViewerCamera* cam = LLViewerCamera::getInstance();
        for (LLCharacter* character : LLCharacter::sInstances)
        {
            LLVOAvatar* inst = (LLVOAvatar*)character;
            bool vis = !inst->isDead() && inst->mDrawable.notNull() && !inst->isUIAvatar();
            if (vis)
            {
                const LLVector4a* ext = inst->mDrawable->getSpatialExtents();
                LLVector4a center, size;
                center.setAdd(ext[1], ext[0]);
                center.mul(0.5f);
                size.setSub(ext[1], ext[0]);
                size.mul(0.5f);
                vis = center.isFinite3() && size.isFinite3() && cam->AABBInFrustumNoFarClip(center, size) > 0;
            }
            inst->mVisibilityPreference = vis ? llmax(inst->getPixelArea(), 1e-6f) : 0.f;
        }
    }

    LLCharacter::sInstances.sort([](LLCharacter* lhs, LLCharacter* rhs)
        {
            return ((LLVOAvatar*)lhs)->mVisibilityPreference > ((LLVOAvatar*)rhs)->mVisibilityPreference;
        });

    // Update the avatars that have changed status
    U32 rank = 2; // Rank 1 is reserved for self.
    for (LLCharacter* character : LLCharacter::sInstances)
    {
        LLVOAvatar* inst = (LLVOAvatar*)character;
        bool culled = !inst->isSelf() && !inst->isFullyBaked();

        if (inst->mCulled != culled)
        {
            inst->mCulled = culled;
            LL_DEBUGS() << "avatar " << inst->getID() << (culled ? " start culled" : " start not culled" ) << LL_ENDL;
            inst->updateMeshTextures();
        }

        // #725: only avatars that can actually use a full-detail slot take a rank. Animesh
        // (control avatars, anyone's) never joins the reduced tier (vkUpdateReducedTier: it
        // is a scene object, not a person), so it ranks with self; a visually muted avatar
        // (blocked, "do not render", over the complexity limit) is reduced regardless
        // (shouldImpostor), so it ranks last. Either one used to push a real avatar near the
        // camera past RenderAvatarMaxNonImpostors (a crowd of 60 animesh took every slot).
        if (inst->isSelf() || inst->isControlAvatar())
        {
            inst->setVisibilityRank(1);
        }
        else if (inst->mDrawable.notNull()
                 && inst->mVisibilityPreference > 0.f
                 && !inst->isVisuallyMuted())
        {
            inst->setVisibilityRank(rank++);
        }
        else
        {
            inst->setVisibilityRank(sMaxNonImpostors * 5);
        }
    }

    {
        const F64 now = LLFrameTimer::getElapsedSeconds();
        U32 n_reduced = 0, n_full = 0, n_animesh = 0;
        for (LLCharacter* character : LLCharacter::sInstances)
        {
            LLVOAvatar* inst = (LLVOAvatar*)character;
            inst->vkUpdateReducedTier(now);
            if (inst->isDead() || inst->isUIAvatar()) continue;
            if (inst->isControlAvatar()) { ++n_animesh; continue; }   // (never reduced, no rank)
            if (inst->mVKReduced) ++n_reduced; else ++n_full;
        }
        static F64 s_next_log = 0.0;
        if (now >= s_next_log)
        {
            s_next_log = now + 10.0;
            LL_INFOS("VKAvLOD") << "reduced tier: " << n_reduced << " reduced, " << n_full << " full (max fully rendered "
                                << sMaxNonImpostors << (sLimitNonImpostors ? "" : " = unlimited") << "); animesh "
                                << n_animesh << " (exempt: scene objects, full tier, no rank)" << LL_ENDL;
        }
    }

    // runway - this doesn't really detect gray/grey state.
    S32 grey_avatars = 0;
    if (!LLVOAvatar::areAllNearbyInstancesBaked(grey_avatars))
    {
        if (gFrameTimeSeconds != sUnbakedUpdateTime) // only update once per frame
        {
            sUnbakedUpdateTime = gFrameTimeSeconds;
            sUnbakedTime += gFrameIntervalSeconds.value();
        }
        if (grey_avatars > 0)
        {
            if (gFrameTimeSeconds != sGreyUpdateTime) // only update once per frame
            {
                sGreyUpdateTime = gFrameTimeSeconds;
                sGreyTime += gFrameIntervalSeconds.value();
            }
        }
    }
}

void LLVOAvatar::startAppearanceAnimation()
{
    if(!mAppearanceAnimating)
    {
        mAppearanceAnimating = true;
        mAppearanceMorphTimer.reset();
        mLastAppearanceBlendTime = 0.f;
    }
}

// virtual
void LLVOAvatar::removeMissingBakedTextures()
{
}

//virtual
void LLVOAvatar::updateRegion(LLViewerRegion *regionp)
{
    LLViewerObject::updateRegion(regionp);
}

// virtual
std::string LLVOAvatar::getFullname() const
{
    std::string name;

    LLNameValue* first = getNVPair("FirstName");
    LLNameValue* last  = getNVPair("LastName");
    if (first && last)
    {
        name = LLCacheName::buildFullName( first->getString(), last->getString() );
    }

    return name;
}

LLHost LLVOAvatar::getObjectHost() const
{
    LLViewerRegion* region = getRegion();
    if (region && !isDead())
    {
        return region->getHost();
    }
    else
    {
        return LLHost();
    }
}

bool LLVOAvatar::updateLOD()
{
    if (mDrawable.isNull())
    {
        return false;
    }

    if (!LLPipeline::sImpostorRender && isImpostor() && 0 != mDrawable->getNumFaces() && mDrawable->getFace(0)->hasGeometry())
    {
        return true;
    }

    bool res = updateJointLODs();

    // VK: the system body's polymesh LODs are not copied into a per-avatar vertex buffer
    // any more (LL's updateMeshData / LLViewerJointMesh::updateFaceData, removed). Nothing
    // read that CPU copy: the world renderer does not draw the system body, and
    // LLVKPreviewRender skins LLPolyMesh directly. Joint LODs above stay live (previews
    // pick the valid LOD through mMeshParts).
    mDirtyMesh = 0;
    mDrawable->clearState(LLDrawable::REBUILD_GEOMETRY);
    updateVisibility();

    return res;
}

void LLVOAvatar::updateLODRiggedAttachments()
{
    updateLOD();
    rebuildRiggedAttachments();
}

void showRigInfoTabExtents(LLVOAvatar *avatar, LLJointRiggingInfoTab& tab, S32& count_rigged, S32& count_box)
{
    count_rigged = count_box = 0;
    LLVector4a zero_vec;
    zero_vec.clear();
    for (S32 i=0; i<tab.size(); i++)
    {
        if (tab[i].isRiggedTo())
        {
            count_rigged++;
            LLJoint *joint = avatar->getJoint(i);
            LL_DEBUGS("RigSpam") << "joint " << i << " name " << joint->getName() << " box "
                                 << tab[i].getRiggedExtents()[0] << ", " << tab[i].getRiggedExtents()[1] << LL_ENDL;
            if ((!tab[i].getRiggedExtents()[0].equals3(zero_vec)) ||
                (!tab[i].getRiggedExtents()[1].equals3(zero_vec)))
            {
                count_box++;
            }
       }
    }
}

void LLVOAvatar::getAssociatedVolumes(std::vector<LLVOVolume*>& volumes)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;
    for (const auto& iter : mAttachmentPoints)
    {
        LLViewerJointAttachment* attachment = iter.second;
        for (LLViewerObject* attached_object : attachment->mAttachedObjects)
        {
            if (attached_object->isDead())
                continue;

            if (attached_object->getPCode() == LL_PCODE_VOLUME)
            {
                LLVOVolume* volume = (LLVOVolume*)attached_object;
                volumes.push_back(volume);
                if (volume->isAnimatedObject())
                {
                    // For animated object attachment, don't need
                    // the children. Will just get bounding box
                    // from the control avatar.
                    continue;
                }
            }

            for (LLViewerObject* childp : attached_object->getChildren())
            {
                if (!childp->isDead() &&  childp->getPCode() == LL_PCODE_VOLUME)
                {
                    volumes.push_back((LLVOVolume*)childp);
                }
            }
        }
    }

    LLControlAvatar *control_av = dynamic_cast<LLControlAvatar*>(this);
    if (control_av)
    {
        LLVOVolume *volp = control_av->mRootVolp;
        if (volp)
        {
            volumes.push_back(volp);
            LLViewerObject::const_child_list_t& children = volp->getChildren();
            for (LLViewerObject::const_child_list_t::const_iterator it = children.begin();
                 it != children.end(); ++it)
            {
                LLViewerObject *childp = *it;
                LLVOVolume *volume = dynamic_cast<LLVOVolume*>(childp);
                if (volume)
                {
                    volumes.push_back(volume);
                }
            }
        }
    }
}

// virtual
void LLVOAvatar::updateRiggingInfo()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;

    LL_DEBUGS("RigSpammish") << getDebugName() << " updating rig tab" << LL_ENDL;

    // use a local static for scratch space to avoid reallocation here
    static std::vector<LLVOVolume*> volumes;
    volumes.resize(0);

    getAssociatedVolumes(volumes);

    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_AVATAR("update rig info - get key");
        size_t hash = 0;
        // Get current rigging info key
        for (LLVOVolume* vol : volumes)
        {
            if (vol->isRiggedMesh())
            {
                const LLUUID& mesh_id = vol->getVolume()->getParams().getSculptID();
                S32 max_lod = llmax(vol->getLOD(), vol->mLastRiggingInfoLOD);

                boost::hash_combine(hash, mesh_id);
                boost::hash_combine(hash, max_lod);
            }
        }

        // Check for key change, which indicates some change in volume composition or LOD.
        if (hash == mLastRiggingInfoKey)
        {
            return;
        }


        // Something changed. Update.
        mLastRiggingInfoKey = hash;
    }

    mJointRiggingInfoTab.clear();
    for (LLVOVolume* vol : volumes)
    {
        vol->updateRiggingInfo();
        mJointRiggingInfoTab.merge(vol->mJointRiggingInfoTab);
    }

    //LL_INFOS() << "done update rig count is " << countRigInfoTab(mJointRiggingInfoTab) << LL_ENDL;
    // Remove debug only stuff on hot path
    // LL_DEBUGS("RigSpammish") << getFullname() << " after update rig tab:" << LL_ENDL;
    // S32 joint_count, box_count;
    // showRigInfoTabExtents(this, mJointRiggingInfoTab, joint_count, box_count);
    // LL_DEBUGS("RigSpammish") << "uses " << joint_count << " joints " << " nonzero boxes: " << box_count << LL_ENDL;
}

// virtual
void LLVOAvatar::onActiveOverrideMeshesChanged()
{
    mJointRiggingInfoTab.setNeedsUpdate(true);
}

U32 LLVOAvatar::getPartitionType() const
{
    // Avatars merely exist as drawables in the bridge partition
    return mIsControlAvatar ? LLViewerRegion::PARTITION_CONTROL_AV : LLViewerRegion::PARTITION_AVATAR;
}

//static
void LLVOAvatar::updateImpostors()
{
    LLViewerCamera::sCurCameraID = LLViewerCamera::CAMERA_WORLD;

    for (LLCharacter* character : LLCharacter::sInstances)
    {
        LLVOAvatar* avatar = (LLVOAvatar*)character;
        if (!avatar->isDead()
            && avatar->isVisible()
            && avatar->isImpostor()
            && avatar->needsImpostorUpdate())
        {
            avatar->calcMutedAVColor();
        }
    }

    LLCharacter::sAllowInstancesChange = true;
}

// virtual
bool LLVOAvatar::isImpostor()
{
    return isVisuallyMuted() || (sLimitNonImpostors && (mUpdatePeriod > 1));
}

bool LLVOAvatar::shouldImpostor(const F32 rank_factor)
{
    if (isSelf())
    {
        return false;
    }
    if (isVisuallyMuted())
    {
        return true;
    }
    return sLimitNonImpostors && (mVisibilityRank > sMaxNonImpostors * rank_factor);
}

//-----------------------------------------------------------------------------
// VK REDUCED-DETAIL TIER (#31). LL's impostor tier drew far avatars as billboards to
// save GL draw calls; the GPU-driven VK pipeline has no per-avatar draw-call cost, so
// its "impostor" tier is the same avatar at reduced cost instead: no shadow casting
// (cascades, projector tiles, RT), animation evaluated at RenderVKReducedAvatarAnimFPS,
// and an extra LOD bias (RenderVKReducedAvatarLODScale). The tier is LL's ranking:
// cullAvatarsByPixelArea sorts by on-screen pixel area (visible avatars only; no
// distance term) and shouldImpostor() is rank > RenderAvatarMaxNonImpostors (0 =
// unlimited), never self, always for visually muted avatars.
// RenderVKReducedAvatarDistance (> 0) additionally reduces any avatar farther away.
// ANIMESH NEVER REDUCES (2026-10-14; LL ranked it like any avatar, and so did we): a
// control avatar is a rigged SCENE OBJECT, not a person in the crowd the "max fully
// rendered avatars" knob is for. Ranked with the people, ~60 copies of one animesh at
// Cloudsdale took the slots, 61 of 65 landed in the reduced tier and -- through its
// no-shadow bit (avatarTierBits) -- cast no shadow at all, raster or ray-traced, on both RT
// paths. It is bounded like world geometry instead: its LOD (LLVOVolume::calcLOD / cluster
// LOD), animation rate (the distance / screen-size ladder, vkLadderAnimRate) and the RT
// budget (legacy avatar BLAS drop, cluster RT bytes). Muted / "do not render" animesh is
// hidden outright (avatarHiddenByPolicy), so nothing the tier did for it is lost; it takes
// no rank either (cullAvatarsByPixelArea).
//-----------------------------------------------------------------------------
// static
F32 LLVOAvatar::vkReducedLODScale()
{
    static LLCachedControl<F32> s_scale(gSavedSettings, "RenderVKReducedAvatarLODScale", 0.5f);
    return llclamp((F32)s_scale, 0.05f, 1.f);
}

void LLVOAvatar::vkUpdateReducedTier(F64 now)
{
    static LLCachedControl<bool> s_on(gSavedSettings, "RenderVKReducedAvatars", true);
    static LLCachedControl<F32>  s_dist(gSavedSettings, "RenderVKReducedAvatarDistance", 0.f);
    // (animesh never reduces: see above)
    const bool eligible = s_on && !isDead() && !isSelf() && !isUIAvatar() && !isControlAvatar() && mDrawable.notNull();
    const F32 dt = mVKReducedLastT >= 0.0 ? (F32)llclamp(now - mVKReducedLastT, 0.0, 0.25) : 0.f;
    mVKReducedLastT = now;
    if (!eligible)
    {
        mVKReduced = false;
        mVKReducedPending = 0.f;
        return;
    }
    // #725 rank band: a full-detail avatar only wants reduced past rank max + 1, a reduced
    // one wants full at rank <= max. Two avatars swapping ranks at the cut then settle
    // (both full: one slot of overshoot) instead of trading tiers every few seconds.
    const F32 band = (!mVKReduced && sMaxNonImpostors > 0) ? 1.f + 1.5f / (F32)sMaxNonImpostors : 1.f;   // rank > max + 1.5
    bool want = shouldImpostor(band);
    if (!want && (F32)s_dist > 0.f && mDrawable->mDistanceWRTCamera > (F32)s_dist)
    {
        want = true;
    }
    // Hysteresis: the pixel-area ranking reshuffles every frame near the cut, and a
    // flip toggles shadows and LOD. Back to full detail quickly, down to reduced slowly.
    // #725: the disagreement is INTEGRATED (agreeing frames drain it at the same rate: a
    // majority vote), not timed from its start. A timer that restarted on any agreeing frame was a latch:
    // two avatars of near-equal pixel area swap ranks across the cut frame to frame
    // (the area moves with every pelvis shift; inside 16 m it is very sensitive), so
    // neither ever disagreed for a continuous 0.25 s and both stayed in whatever tier
    // they had -- until a camera move changed every area decisively.
    if (want == mVKReduced)
    {
        mVKReducedPending = llmax(mVKReducedPending - dt, 0.f);
        return;
    }
    mVKReducedPending += dt;
    if (mVKReducedPending >= (want ? 1.f : 0.25f))
    {
        mVKReduced = want;
        mVKReducedPending = 0.f;
        mVKAnimBucket = -1;
    }
}

S32 LLVOAvatar::vkReducedRiggedLOD() const
{
    return vkRiggedLODAtScale(vkReducedLODScale());
}

S32 LLVOAvatar::vkRiggedLODAtScale(F32 scale) const
{
    // LLVOVolume::calcLOD's rigged branch, verbatim but for the scaled lod factor (every
    // rigged volume of one avatar gets the same LOD there: avatar distance + box).
    if (mDrawable.isNull())
    {
        return 3;
    }
    F32 distance = mDrawable->mDistanceWRTCamera;
    const LLVector3* box = getLastAnimExtents();
    F32 radius = (box[1] - box[0]).magVec();
    if (isControlAvatar())
    {
        radius *= 0.5f;
    }
    if (distance <= 0.f || radius <= 0.f)
    {
        return 3;
    }
    distance *= LLVOVolume::sDistanceFactor;
    const F32 ramp_dist = LLVOVolume::sLODFactor * 2;
    if (distance < ramp_dist)
    {
        distance *= 1.0f / ramp_dist;
        distance *= distance;
        distance *= ramp_dist;
    }
    distance *= F_PI / 3.f;
    F32 lod_factor = LLVOVolume::sLODFactor * llclamp(scale, 0.05f, 1.f);
    static LLCachedControl<bool> ignore_fov_zoom(gSavedSettings, "IgnoreFOVZoomForLODs");
    if (!ignore_fov_zoom)
    {
        lod_factor *= DEFAULT_FIELD_OF_VIEW / LLViewerCamera::getInstance()->getDefaultFOV();
    }
    distance = ll_round(distance, 0.01f);
    radius = ll_round(radius, 0.01f);
    if (LLPipeline::sDynamicLOD)
    {
        return LLVolumeLODGroup::getDetailFromTan(ll_round((lod_factor * radius) / distance, 0.01f));
    }
    return llclamp((S32)(sqrtf(radius) * lod_factor * 4.f), 0, 3);
}

bool LLVOAvatar::needsImpostorUpdate() const
{
    return mNeedsImpostorUpdate;
}

const LLVector3& LLVOAvatar::getImpostorOffset() const
{
    return mImpostorOffset;
}

const LLVector2& LLVOAvatar::getImpostorDim() const
{
    return mImpostorDim;
}

void LLVOAvatar::setImpostorDim(const LLVector2& dim)
{
    mImpostorDim = dim;
}

void LLVOAvatar::cacheImpostorValues()
{
    getImpostorValues(mImpostorExtents, mImpostorAngle, mImpostorDistance);
}

void LLVOAvatar::getImpostorValues(LLVector4a* extents, LLVector3& angle, F32& distance) const
{
    const LLVector4a* ext = mDrawable->getSpatialExtents();
    extents[0] = ext[0];
    extents[1] = ext[1];

    LLVector3 at = LLViewerCamera::getInstance()->getOrigin()-(getRenderPosition()+mImpostorOffset);
    distance = at.normalize();
    F32 da = 1.f - (at*LLViewerCamera::getInstance()->getAtAxis());
    angle.mV[0] = LLViewerCamera::getInstance()->getYaw()*da;
    angle.mV[1] = LLViewerCamera::getInstance()->getPitch()*da;
    angle.mV[2] = da;
}

// static
const U32 LLVOAvatar::NON_IMPOSTORS_MAX_SLIDER = 66; /* Must equal the maximum allowed the RenderAvatarMaxNonImpostors
                                           * slider in panel_preferences_graphics1.xml */

// static
void LLVOAvatar::updateImpostorRendering(U32 newMaxNonImpostorsValue)
{
    U32  oldmax = sMaxNonImpostors;
    bool oldflg = sLimitNonImpostors;

    if (NON_IMPOSTORS_MAX_SLIDER <= newMaxNonImpostorsValue)
    {
        sMaxNonImpostors = 0;
    }
    else
    {
        sMaxNonImpostors = newMaxNonImpostorsValue;
    }
    // the sLimitNonImpostors flag depends on whether or not sMaxNonImpostors is set to the no-limit value (0)
    sLimitNonImpostors = (0 != sMaxNonImpostors);
    if ( oldflg != sLimitNonImpostors )
    {
        LL_DEBUGS("AvatarRender")
            << "was " << (oldflg ? "use" : "don't use" ) << " impostors (max " << oldmax << "); "
            << "now " << (sLimitNonImpostors ? "use" : "don't use" ) << " impostors (max " << sMaxNonImpostors << "); "
            << LL_ENDL;
    }
}


void LLVOAvatar::idleUpdateRenderComplexity()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;
    if (isControlAvatar())
    {
        LLControlAvatar *cav = dynamic_cast<LLControlAvatar*>(this);
        bool is_attachment = cav && cav->mRootVolp && cav->mRootVolp->isAttachment(); // For attached animated objects
        if (is_attachment)
        {
            // ARC for animated object attachments is accounted with the avatar they're attached to.
            return;
        }
    }

    // Render Complexity
    calculateUpdateRenderComplexity(); // Update mVisualComplexity if needed
}

void LLVOAvatar::idleUpdateDebugInfo()
{
    if (gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_AVATAR_DRAW_INFO))
    {
        std::string info_line;
        F32 red_level;
        F32 green_level;
        LLColor4 info_color;
        LLFontGL::StyleFlags info_style;

        if ( !mText )
        {
            initHudText();
            mText->setFadeDistance(20.0, 5.0); // limit clutter in large crowds
        }
        else
        {
            mText->clearString(); // clear debug text
        }

        /*
         * NOTE: the logic for whether or not each of the values below
         * controls muting MUST match that in the isVisuallyMuted and isTooComplex methods.
         */

        static LLCachedControl<U32> max_render_cost(gSavedSettings, "RenderAvatarMaxComplexity", 0);
        info_line = llformat("%d Complexity", mVisualComplexity);

        if (max_render_cost != 0) // zero means don't care, so don't bother coloring based on this
        {
            green_level = 1.f-llclamp(((F32) mVisualComplexity-(F32)max_render_cost)/(F32)max_render_cost, 0.f, 1.f);
            red_level   = llmin((F32) mVisualComplexity/(F32)max_render_cost, 1.f);
            info_color.set(red_level, green_level, 0.0, 1.0);
            info_style = (  mVisualComplexity > max_render_cost
                          ? LLFontGL::BOLD : LLFontGL::NORMAL );
        }
        else
        {
            info_color.set(LLColor4::grey);
            info_style = LLFontGL::NORMAL;
        }
        mText->addLine(info_line, info_color, info_style);

        // Visual rank
        info_line = llformat("%d rank", mVisibilityRank);
        // Use grey for imposters, white for normal rendering or no impostors
        info_color.set(isImpostor() ? LLColor4::grey : (isControlAvatar() ? LLColor4::yellow : LLColor4::white));
        info_style = LLFontGL::NORMAL;
        mText->addLine(info_line, info_color, info_style);

        // Triangle count
        mText->addLine(std::string("VisTris ") + LLStringOps::getReadableNumber(mAttachmentVisibleTriangleCount),
                       info_color, info_style);
        mText->addLine(std::string("EstMaxTris ") + LLStringOps::getReadableNumber(mAttachmentEstTriangleCount),
                       info_color, info_style);

        // Attachment Surface Area
        static LLCachedControl<F32> max_attachment_area(gSavedSettings, "RenderAutoMuteSurfaceAreaLimit", 1000.0f);
        info_line = llformat("%.0f m^2", mAttachmentSurfaceArea);

        if (max_render_cost != 0 && max_attachment_area != 0) // zero means don't care, so don't bother coloring based on this
        {
            green_level = 1.f-llclamp((mAttachmentSurfaceArea-max_attachment_area)/max_attachment_area, 0.f, 1.f);
            red_level   = llmin(mAttachmentSurfaceArea/max_attachment_area, 1.f);
            info_color.set(red_level, green_level, 0.0, 1.0);
            info_style = (  mAttachmentSurfaceArea > max_attachment_area
                          ? LLFontGL::BOLD : LLFontGL::NORMAL );

        }
        else
        {
            info_color.set(LLColor4::grey);
            info_style = LLFontGL::NORMAL;
        }

        mText->addLine(info_line, info_color, info_style);

        updateText(); // corrects position
    }
}

void LLVOAvatar::updateVisualComplexity()
{
    LL_DEBUGS("AvatarRender") << "avatar " << getID() << " appearance changed" << LL_ENDL;
    LLVKHitch::count("avComplexityStale");   // #337: invalidations (recomputes: avComplexityRecompute)
    // Set the cache time to in the past so it's updated ASAP
    mVisualComplexityStale = true;
}


// Account for the complexity of a single top-level object associated
// with an avatar. This will be either an attached object or an animated
// object.
void LLVOAvatar::accountRenderComplexityForObject(
    LLViewerObject *attached_object,
    const F32 max_attachment_complexity,
    LLVOVolume::texture_cost_t& textures,
    U32& cost,
    hud_complexity_list_t& hud_complexity_list,
    object_complexity_list_t& object_complexity_list,
    // <FS:Ansariel> Show per-item complexity in COF
    std::map<LLUUID, U32>& item_complexity,
    std::map<LLUUID, U32>& temp_item_complexity)
    // </FS:Ansariel>
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;
    if (attached_object && !attached_object->isHUDAttachment())
    {
        mAttachmentVisibleTriangleCount += attached_object->recursiveGetTriangleCount();
        mAttachmentEstTriangleCount += attached_object->recursiveGetEstTrianglesMax();
        mAttachmentSurfaceArea += attached_object->recursiveGetScaledSurfaceArea();

        textures.clear();
        const LLDrawable* drawable = attached_object->mDrawable;
        if (drawable)
        {
            const LLVOVolume* volume = drawable->getVOVolume();
            if (volume)
            {
                F32 attachment_total_cost = 0;
                F32 attachment_volume_cost = 0;
                F32 attachment_texture_cost = 0;
                F32 attachment_children_cost = 0;
                const F32 animated_object_attachment_surcharge = 1000;

                if (volume->isAnimatedObjectFast())
                {
                    attachment_volume_cost += animated_object_attachment_surcharge;
                }
                attachment_volume_cost += volume->getRenderCost(textures);

                const_child_list_t children = volume->getChildren();
                for (const_child_list_t::const_iterator child_iter = children.begin();
                    child_iter != children.end();
                    ++child_iter)
                {
                    LLViewerObject* child_obj = *child_iter;
                    LLVOVolume* child = dynamic_cast<LLVOVolume*>(child_obj);
                    if (child)
                    {
                        attachment_children_cost += child->getRenderCost(textures);
                    }
                }

                for (LLVOVolume::texture_cost_t::iterator volume_texture = textures.begin();
                    volume_texture != textures.end();
                    ++volume_texture)
                {
                    // add the cost of each individual texture in the linkset
                    attachment_texture_cost += LLVOVolume::getTextureCost(*volume_texture);
                }
                attachment_total_cost = attachment_volume_cost + attachment_texture_cost + attachment_children_cost;
                LL_DEBUGS("ARCdetail") << "Attachment costs " << attached_object->getAttachmentItemID()
                    << " total: " << attachment_total_cost
                    << ", volume: " << attachment_volume_cost
                    << ", " << textures.size()
                    << " textures: " << attachment_texture_cost
                    << ", " << volume->numChildren()
                    << " children: " << attachment_children_cost
                    << LL_ENDL;
                // Limit attachment complexity to avoid signed integer flipping of the wearer's ACI
                cost += (U32)llclamp(attachment_total_cost, MIN_ATTACHMENT_COMPLEXITY, max_attachment_complexity);

                if (isSelf())
                {
                    LLObjectComplexity object_complexity;
                    object_complexity.objectName = attached_object->getAttachmentItemName();
                    object_complexity.objectId = attached_object->getAttachmentItemID();
                    object_complexity.objectCost = (U32)attachment_total_cost;
                    object_complexity_list.push_back(object_complexity);
                }

                // <FS:Ansariel> Show per-item complexity in COF
                if (isSelf())
                {
                    if (!attached_object->isTempAttachment())
                    {
                        item_complexity.insert(std::make_pair(attached_object->getAttachmentItemID(), (U32)attachment_total_cost));
                    }
                    else
                    {
                        temp_item_complexity.insert(std::make_pair(attached_object->getID(), (U32)attachment_total_cost));
                    }
                }
                // </FS:Ansariel>
            }
        }
    }
    if (isSelf()
        && attached_object
        && attached_object->isHUDAttachment()
        && !attached_object->isTempAttachment()
        && attached_object->mDrawable)
    {
        textures.clear();
        mAttachmentSurfaceArea += attached_object->recursiveGetScaledSurfaceArea();

        const LLVOVolume* volume = attached_object->mDrawable->getVOVolume();
        if (volume)
        {
            bool is_rigged_mesh = volume->isRiggedMeshFast();
            LLHUDComplexity hud_object_complexity;
            hud_object_complexity.objectName = attached_object->getAttachmentItemName();
            hud_object_complexity.objectId = attached_object->getAttachmentItemID();
            std::string joint_name;
            gAgentAvatarp->getAttachedPointName(attached_object->getAttachmentItemID(), joint_name);
            hud_object_complexity.jointName = joint_name;
            // get cost and individual textures
            hud_object_complexity.objectsCost += volume->getRenderCost(textures);
            hud_object_complexity.objectsCount++;

            LLViewerObject::const_child_list_t& child_list = attached_object->getChildren();
            for (LLViewerObject::child_list_t::const_iterator iter = child_list.begin();
                iter != child_list.end(); ++iter)
            {
                LLViewerObject* childp = *iter;
                const LLVOVolume* chld_volume = dynamic_cast<LLVOVolume*>(childp);
                if (chld_volume)
                {
                    is_rigged_mesh = is_rigged_mesh || chld_volume->isRiggedMeshFast();
                    // get cost and individual textures
                    hud_object_complexity.objectsCost += chld_volume->getRenderCost(textures);
                    hud_object_complexity.objectsCount++;
                }
            }
            if (is_rigged_mesh && !attached_object->mRiggedAttachedWarned)
            {
                LLSD args;
                LLViewerInventoryItem* itemp = gInventory.getItem(attached_object->getAttachmentItemID());
                args["NAME"] = itemp ? itemp->getName() : LLTrans::getString("Unknown");
                args["POINT"] = LLTrans::getString(getTargetAttachmentPoint(attached_object)->getName());
                LLNotificationsUtil::add("RiggedMeshAttachedToHUD", args);

                attached_object->mRiggedAttachedWarned = true;
            }

            hud_object_complexity.texturesCount += static_cast<U32>(textures.size());

            for (LLVOVolume::texture_cost_t::iterator volume_texture = textures.begin();
                volume_texture != textures.end();
                ++volume_texture)
            {
                // add the cost of each individual texture (ignores duplicates)
                hud_object_complexity.texturesCost += LLVOVolume::getTextureCost(*volume_texture);
                const LLViewerTexture* img = *volume_texture;
                if (img->getType() == LLViewerTexture::FETCHED_TEXTURE)
                {
                    LLViewerFetchedTexture* tex = (LLViewerFetchedTexture*)img;
                    if (tex->getOriginalHeight() * tex->getOriginalWidth() >= HUD_OVERSIZED_TEXTURE_DATA_SIZE)
                    {
                        hud_object_complexity.largeTexturesCount++;
                    }
                }
            }
            hud_complexity_list.push_back(hud_object_complexity);
        }
    }
}

// Calculations for mVisualComplexity value
void LLVOAvatar::calculateUpdateRenderComplexity()
{
    /*****************************************************************
     * This calculation should not be modified by third party viewers,
     * since it is used to limit rendering and should be uniform for
     * everyone. If you have suggested improvements, submit them to
     * the official viewer for consideration.
     *****************************************************************/
    if (mVisualComplexityStale)
    {
        LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;
        LLVKHitch::count("avComplexityRecompute");   // #337

        static const U32 COMPLEXITY_BODY_PART_COST = 200;
        static LLCachedControl<F32> max_complexity_setting(gSavedSettings, "MaxAttachmentComplexity");
        F32 max_attachment_complexity = max_complexity_setting;
        max_attachment_complexity = llmax(max_attachment_complexity, DEFAULT_MAX_ATTACHMENT_COMPLEXITY);

        // Diagnostic list of all textures on our avatar
        static boost::unordered_set<const LLViewerTexture*> all_textures;

        U32 cost = VISUAL_COMPLEXITY_UNKNOWN;
        LLVOVolume::texture_cost_t textures;
        hud_complexity_list_t hud_complexity_list;
        object_complexity_list_t object_complexity_list;
        // <FS:Ansariel> Show per-item complexity in COF
        std::map<LLUUID, U32> item_complexity;
        std::map<LLUUID, U32> temp_item_complexity;
        // </FS:Ansariel>

        for (U8 baked_index = 0; baked_index < BAKED_NUM_INDICES; baked_index++)
        {
            const LLAvatarAppearanceDictionary::BakedEntry *baked_dict
                = LLAvatarAppearance::getDictionary()->getBakedTexture((EBakedTextureIndex)baked_index);
            ETextureIndex tex_index = baked_dict->mTextureIndex;
            if ((tex_index != TEX_SKIRT_BAKED) || (isWearingWearableType(LLWearableType::WT_SKIRT)))
            {
                // Same as isTextureVisible(), but doesn't account for isSelf to ensure identical numbers for all avatars
                if (isIndexLocalTexture(tex_index))
                {
                    if (isTextureDefined(tex_index, 0))
                    {
                        cost += COMPLEXITY_BODY_PART_COST;
                    }
                }
                else
                {
                    // baked textures can use TE images directly
                    if (isTextureDefined(tex_index)
                        && (getTEImage(tex_index)->getID() != IMG_INVISIBLE || LLPipeline::sShowDebugAlpha))
                    {
                        cost += COMPLEXITY_BODY_PART_COST;
                    }
                }
            }
        }
        LL_DEBUGS("ARCdetail") << "Avatar body parts complexity: " << cost << LL_ENDL;
        const U32 body_parts_complexity = cost; // <FS:Ansariel> Show per-item complexity in COF

        mAttachmentVisibleTriangleCount = 0;
        mAttachmentEstTriangleCount = 0.f;
        mAttachmentSurfaceArea = 0.f;

        // A standalone animated object needs to be accounted for
        // using its associated volume. Attached animated objects
        // will be covered by the subsequent loop over attachments.
        LLControlAvatar *control_av = dynamic_cast<LLControlAvatar*>(this);
        if (control_av)
        {
            LLVOVolume *volp = control_av->mRootVolp;
            if (volp && !volp->isAttachment())
            {
                accountRenderComplexityForObject(volp, max_attachment_complexity,
                                                 textures, cost, hud_complexity_list, object_complexity_list,
                                                 item_complexity, temp_item_complexity);
            }
        }

        // Account for complexity of all attachments.
        for (attachment_map_t::const_iterator attachment_point = mAttachmentPoints.begin();
             attachment_point != mAttachmentPoints.end();
             ++attachment_point)
        {
            LLViewerJointAttachment* attachment = attachment_point->second;
            for (LLViewerJointAttachment::attachedobjs_vec_t::iterator attachment_iter = attachment->mAttachedObjects.begin();
                 attachment_iter != attachment->mAttachedObjects.end();
                 ++attachment_iter)
            {
                LLViewerObject* attached_object = attachment_iter->get();
                accountRenderComplexityForObject(attached_object, max_attachment_complexity,
                                                 textures, cost, hud_complexity_list, object_complexity_list,
                                                 item_complexity, temp_item_complexity);
            }
        }

        if ( cost != mVisualComplexity )
        {
            LL_DEBUGS("AvatarRender") << "Avatar "<< getID()
                                      << " complexity updated was " << mVisualComplexity << " now " << cost
                                      << " reported " << mReportedVisualComplexity
                                      << LL_ENDL;
        }
        else
        {
            LL_DEBUGS("AvatarRender") << "Avatar "<< getID()
                                      << " complexity updated no change " << mVisualComplexity
                                      << " reported " << mReportedVisualComplexity
                                      << LL_ENDL;
        }
        mVisualComplexity = cost;
        mVisualComplexityStale = false;

        // #627: keep our own per-item numbers for the Appearance window, which polls
        // getSelfComplexitySerial() instead of being called from here.
        if (isSelf())
        {
            mSelfItemComplexity.swap(item_complexity);
            mSelfTempItemComplexity.swap(temp_item_complexity);
            mSelfBodyPartsComplexity = body_parts_complexity;
            ++mSelfComplexitySerial;
        }

        static LLCachedControl<U32> show_my_complexity_changes(gSavedSettings, "ShowMyComplexityChanges", 20);

        if (isSelf() && show_my_complexity_changes)
        {
            // Avatar complexity
            LLAvatarRenderNotifier::getInstance()->updateNotificationAgent(mVisualComplexity);
            LLAvatarRenderNotifier::getInstance()->setObjectComplexityList(object_complexity_list);
            // HUD complexity
            LLHUDRenderNotifier::getInstance()->updateNotificationHUD(hud_complexity_list);
        }

        // (#337: LL posted an empty lambda to the mainloop work queue here -- "schedule
        // an update to ART next frame" with the update itself gone; removed.)
    }
}

void LLVOAvatar::setVisualMuteSettings(VisualMuteSettings set)
{
    mVisuallyMuteSetting = set;
    mNeedsImpostorUpdate = true;
    mLastImpostorUpdateReason = 7;

    LLRenderMuteList::getInstance()->saveVisualMuteSetting(getID(), S32(set));
}


void LLVOAvatar::setOverallAppearanceNormal()
{
    LLVKHitch::count("av_unJellydoll");   // #521
    if (isControlAvatar())
        return;

    LLVector3 pelvis_pos = getJoint("mPelvis")->getPosition();
    if (isControlAvatar() || mLastProcessedAppearance)
    {
        resetSkeleton(false);
    }
    getJoint("mPelvis")->setPosition(pelvis_pos);

    for (auto it = mJellyAnims.begin(); it !=  mJellyAnims.end(); ++it)
    {
        bool is_playing = (mPlayingAnimations.find(*it) != mPlayingAnimations.end());
        LL_DEBUGS("Avatar") << "jelly anim " << *it << " " << is_playing << LL_ENDL;
        if (!is_playing)
        {
            // Anim was not requested for this av by sim, but may be playing locally
            stopMotion(*it);
        }
    }
    mJellyAnims.clear();

    processAnimationStateChanges();
}

void LLVOAvatar::setOverallAppearanceJellyDoll()
{
    if (isControlAvatar())
        return;

    // stop current animations
    {
        for ( LLVOAvatar::AnimIterator anim_it= mPlayingAnimations.begin();
              anim_it != mPlayingAnimations.end();
              ++anim_it)
        {
            {
                stopMotion(anim_it->first, true);
            }
        }
    }
    processAnimationStateChanges();

    // Start any needed anims for jellydoll
    updateOverallAppearanceAnimations();

    LLVector3 pelvis_pos = getJoint("mPelvis")->getPosition();
    resetSkeleton(false);
    getJoint("mPelvis")->setPosition(pelvis_pos);

}

void LLVOAvatar::setOverallAppearanceInvisible()
{
}

void LLVOAvatar::updateOverallAppearance()
{
    AvatarOverallAppearance new_overall = getOverallAppearance();
    if (new_overall != mOverallAppearance)
    {
        switch (new_overall)
        {
            case AOA_NORMAL:
                setOverallAppearanceNormal();
                break;
            case AOA_JELLYDOLL:
                setOverallAppearanceJellyDoll();
                break;
            case AOA_INVISIBLE:
                setOverallAppearanceInvisible();
                break;
        }
        mOverallAppearance = new_overall;
        if (!isSelf())
        {
            mNeedsImpostorUpdate = true;
            mLastImpostorUpdateReason = 8;
        }
        updateMeshVisibility();
    }

    // This needs to be done even if overall appearance has not
    // changed, since sit/stand status can be different.
    updateOverallAppearanceAnimations();
}

void LLVOAvatar::updateOverallAppearanceAnimations()
{
    if (isControlAvatar())
        return;

    if (getOverallAppearance() == AOA_JELLYDOLL)
    {
        LLUUID motion_id;
        if (isSitting() && getParent()) // sitting on object
        {
            motion_id = ANIM_AGENT_SIT_FEMALE;
        }
        else if (isSitting()) // sitting on ground
        {
            motion_id = ANIM_AGENT_SIT_GROUND_CONSTRAINED;
        }
        else // standing
        {
            motion_id = ANIM_AGENT_STAND;
        }
        if (mJellyAnims.find(motion_id) == mJellyAnims.end())
        {
            for (auto it = mJellyAnims.begin(); it !=  mJellyAnims.end(); ++it)
            {
                bool is_playing = (mPlayingAnimations.find(*it) != mPlayingAnimations.end());
                LL_DEBUGS("Avatar") << "jelly anim " << *it << " " << is_playing << LL_ENDL;
                if (!is_playing)
                {
                    // Anim was not requested for this av by sim, but may be playing locally
                    stopMotion(*it, true);
                }
            }
            mJellyAnims.clear();

            startMotion(motion_id);
            mJellyAnims.insert(motion_id);

            processAnimationStateChanges();
        }
    }
}

// Based on isVisuallyMuted(), but has 3 possible results.
LLVOAvatar::AvatarOverallAppearance LLVOAvatar::getOverallAppearance() const
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;
    AvatarOverallAppearance result = AOA_NORMAL;

    // Priority order (highest priority first)
    // * own avatar is always drawn normally
    // * if on the "always draw normally" list, draw them normally
    // * if on the "always visually mute" list, show as jellydoll
    // * if explicitly muted (blocked), show as invisible
    // * check against the render cost and attachment limits - if too complex, show as jellydoll
    if (isSelf())
    {
        result = AOA_NORMAL;
    }
    else // !isSelf()
    {
        if (isInMuteList())
        {
            result = AOA_INVISIBLE;
        }
        else if (mVisuallyMuteSetting == AV_ALWAYS_RENDER)
        {
            result = AOA_NORMAL;
        }
        else if (mVisuallyMuteSetting == AV_DO_NOT_RENDER)
        {   // Always want to see this AV as an impostor
            result = AOA_JELLYDOLL;
        }
        else if (isTooComplex() || isTooSlow())
        {
            result = AOA_JELLYDOLL;
        }
    }

    return result;
}

void LLVOAvatar::calcMutedAVColor()
{
    LLColor4 new_color(mMutedAVColor);
    std::string change_msg;
    LLUUID av_id(getID());

// [RLVa:KB] - Checked: RLVa-2.2 (@setcam_avdist)
    if (isRlvSilhouette())
    {
        new_color = LLColor4::silhouette;
        change_msg = " not rendered: color is silhouette";
    }
    else if (getVisualMuteSettings() == AV_DO_NOT_RENDER)
// [/RLVa:KB]
//    if (getVisualMuteSettings() == AV_DO_NOT_RENDER)
    {
        // explicitly not-rendered avatars are light grey
        new_color = LLColor4::grey4;
        change_msg = " not rendered: color is grey4";
    }
    else if (isInMuteList()) // the user blocked them
    {
        // blocked avatars are dark grey
        new_color = LLColor4::grey4;
        change_msg = " blocked: color is grey4";
    }
    else if (!isTooComplex() && !isTooSlow())
    {
        new_color = LLColor4::white;
        change_msg = " simple imposter ";
    }
#ifdef COLORIZE_JELLYDOLLS
    else if ( mMutedAVColor == LLColor4::white || mMutedAVColor == LLColor4::grey3 || mMutedAVColor == LLColor4::grey4 )
    {
        // select a color based on the first byte of the agents uuid so any muted agent is always the same color
        F32 color_value = (F32) (av_id.mData[0]);
        F32 spectrum = (color_value / 256.0);          // spectrum is between 0 and 1.f

        // Array of colors.  These are arranged so only one RGB color changes between each step,
        // and it loops back to red so there is an even distribution.  It is not a heat map
        const S32 NUM_SPECTRUM_COLORS = 7;
        static LLColor4 * spectrum_color[NUM_SPECTRUM_COLORS] = { &LLColor4::red, &LLColor4::magenta, &LLColor4::blue, &LLColor4::cyan, &LLColor4::green, &LLColor4::yellow, &LLColor4::red };

        spectrum = spectrum * (NUM_SPECTRUM_COLORS - 1);               // Scale to range of number of colors
        S32 spectrum_index_1  = floor(spectrum);                               // Desired color will be after this index
        S32 spectrum_index_2  = spectrum_index_1 + 1;                  //    and before this index (inclusive)
        F32 fractBetween = spectrum - (F32)(spectrum_index_1);  // distance between the two indexes (0-1)

        new_color = lerp(*spectrum_color[spectrum_index_1], *spectrum_color[spectrum_index_2], fractBetween);
        new_color.normalize();
        new_color *= 0.28f;            // Tone it down
    }
#endif
    else
    {
        new_color = LLColor4::grey4;
        change_msg = " over limit color ";
    }

    if (mMutedAVColor != new_color)
    {
        LL_DEBUGS("AvatarRender") << "avatar "<< av_id << change_msg << std::setprecision(3) << new_color << LL_ENDL;
        mMutedAVColor = new_color;
    }
}

// static
bool LLVOAvatar::isIndexLocalTexture(ETextureIndex index)
{
    return (index < 0 || index >= TEX_NUM_INDICES)
        ? false
        : LLAvatarAppearance::getDictionary()->getTexture(index)->mIsLocalTexture;
}

// static
bool LLVOAvatar::isIndexBakedTexture(ETextureIndex index)
{
    return (index < 0 || index >= TEX_NUM_INDICES)
        ? false
        : LLAvatarAppearance::getDictionary()->getTexture(index)->mIsBakedTexture;
}

const std::string LLVOAvatar::getBakedStatusForPrintout() const
{
    std::string line;

    for (LLAvatarAppearanceDictionary::Textures::const_iterator iter = LLAvatarAppearance::getDictionary()->getTextures().begin();
         iter != LLAvatarAppearance::getDictionary()->getTextures().end();
         ++iter)
    {
        const ETextureIndex index = iter->first;
        const LLAvatarAppearanceDictionary::TextureEntry *texture_dict = iter->second;
        if (texture_dict->mIsBakedTexture)
        {
            line += texture_dict->mName;
            if (isTextureDefined(index))
            {
                line += "_baked";
            }
            line += " ";
        }
    }
    return line;
}



//virtual
S32 LLVOAvatar::getTexImageSize() const
{
    return TEX_IMAGE_SIZE_OTHER;
}

//-----------------------------------------------------------------------------
// Utility functions
//-----------------------------------------------------------------------------

F32 calc_bouncy_animation(F32 x)
{
    return -(cosf(x * F_PI * 2.5f - F_PI_BY_TWO))*(0.4f + x * -0.1f) + x * 1.3f;
}

//virtual
bool LLVOAvatar::isTextureDefined(LLAvatarAppearanceDefines::ETextureIndex te, U32 index ) const
{
    if (isIndexLocalTexture(te))
    {
        return false;
    }

    LLViewerTexture* tex = getImage(te, index);
    if (!tex)
    {
        LL_WARNS() << "getImage( " << te << ", " << index << " ) returned 0" << LL_ENDL;
        return false;
    }

    return (tex->getID() != IMG_DEFAULT_AVATAR &&
            tex->getID() != IMG_DEFAULT);
}

//virtual
bool LLVOAvatar::isTextureVisible(LLAvatarAppearanceDefines::ETextureIndex type, U32 index) const
{
    if (isIndexLocalTexture(type))
    {
        return isTextureDefined(type, index);
    }

    // baked textures can use TE images directly
    return ((isTextureDefined(type) || isSelf()) &&
            (getTEImage(type)->getID() != IMG_INVISIBLE || LLPipeline::sShowDebugAlpha));
}

//virtual
bool LLVOAvatar::isTextureVisible(LLAvatarAppearanceDefines::ETextureIndex type, LLViewerWearable *wearable) const
{
    // non-self avatars don't have wearables
    return false;
}

F32 LLVOAvatar::getGPURenderTime()
{
    return isVisuallyMuted() ? 0.f : mGPURenderTime;
}

// static
F32 LLVOAvatar::getTotalGPURenderTime()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;

    F32 ret = 0.f;

    for (LLCharacter* character : LLCharacter::sInstances)
    {
        ret += ((LLVOAvatar*)character)->getGPURenderTime();
    }

    return ret;
}

F32 LLVOAvatar::getMaxGPURenderTime()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;

    F32 ret = 0.f;

    for (LLCharacter* character : LLCharacter::sInstances)
    {
        ret = llmax(((LLVOAvatar*)character)->getGPURenderTime(), ret);
    }

    return ret;
}

F32 LLVOAvatar::getAverageGPURenderTime()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;

    F32 ret = 0.f;

    S32 count = 0;

    for (LLCharacter* character : LLCharacter::sInstances)
    {
        LLVOAvatar* avatar = (LLVOAvatar*)character;
        if (!avatar->isTooSlow())
        {
            ret += avatar->getGPURenderTime();
            ++count;
        }
    }

    if (count > 0)
    {
        ret /= count;
    }

    return ret;
}

bool LLVOAvatar::isBuddy() const
{
    bool is_friend = false;
    F64 now = LLFrameTimer::getTotalSeconds();
    if (now < mCachedBuddyListUpdateTime)
    {
        is_friend = mCachedInBuddyList;
    }
    else
    {
        is_friend = LLAvatarTracker::instance().isBuddy(getID());

        const F64 SECONDS_BETWEEN_BUDDY_UPDATES = 1;
        mCachedBuddyListUpdateTime = now + SECONDS_BETWEEN_BUDDY_UPDATES;
        mCachedInBuddyList = is_friend;
    }
    return is_friend;
}

