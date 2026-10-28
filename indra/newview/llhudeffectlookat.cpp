/**
 * @file llhudeffectlookat.cpp
 * @brief LLHUDEffectLookAt class implementation
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

#include "llviewerprecompiledheaders.h"

#include "llhudeffectlookat.h"
#include <utility>

#include "llrender.h"

#include "message.h"
#include "llagent.h"
#include "llagentcamera.h"
#include "llvoavatar.h"
#include "llvoavatarself.h" // for gAgentAvatarp
#include "lldrawable.h"
#include "llviewerobjectlist.h"
#include "llviewercontrol.h"
#include "llselectmgr.h"
#include "llwin32headers.h"
#include "llxmltree.h"

#include "llavatarnamecache.h"
#include "llhudrender.h"
#include "llviewercontrol.h"
#include "slposelookat.h"

#include <cmath>

// packet layout
const S32 SOURCE_AVATAR = 0;
const S32 TARGET_OBJECT = 16;
const S32 TARGET_POS = 32;
const S32 LOOKAT_TYPE = 56;
const S32 PKT_SIZE = 57;

// throttle
const F32 MAX_SENDS_PER_SEC = 4.f;

const F32 MIN_DELTAPOS_FOR_UPDATE_SQUARED = 0.05f * 0.05f;
const F32 MIN_TARGET_OFFSET_SQUARED = 0.0001f;

// Slipstream (#1121): head tilt (roll) in the effect's 4-byte Color field, which every
// ViewerEffect carries and no viewer reads for a look-at (the beacon colours come from the
// attention table). Bytes: ROLL_TAG, ROLL_VERSION, roll in whole degrees (S8, -90..90), and
// a check byte (the XOR of the other three with 0xFF), so a stock viewer's default colour
// (0, 0, 0, 255) or any other colour never decodes as a roll.
const U8 ROLL_TAG = 0x53;
const U8 ROLL_VERSION = 1;
const S32 ROLL_MAX_DEG = 90;
const S32 ROLL_SEND_DEG = 2;    // a tilt change this large is worth a send (the target's 5 cm is ~3 deg)

static U8 rollCheck(U8 a, U8 b, U8 c)
{
    return (U8)(a ^ b ^ c ^ 0xFF);
}


// can't use actual F32_MAX, because we add this to the current frametime
const F32 MAX_TIMEOUT = F32_MAX / 2.f;

/**
 * Simple data class holding values for a particular type of attention.
 */
class LLAttention
{
public:
    LLAttention()
        : mTimeout(0.f),
          mPriority(0.f)
    {}
    LLAttention(F32 timeout, F32 priority, const std::string& name, LLColor3 color) :
      mTimeout(timeout), mPriority(priority), mName(name), mColor(color)
    {
    }
    F32 mTimeout, mPriority;
    std::string mName;
    LLColor3 mColor;
};

/**
 * Simple data class holding a list of attentions, one for every type.
 */
class LLAttentionSet
{
public:
    LLAttentionSet(const LLAttention attentions[])
    {
        for(int i=0; i<LOOKAT_NUM_TARGETS; i++)
        {
            mAttentions[i] = attentions[i];
        }
    }
    LLAttention mAttentions[LOOKAT_NUM_TARGETS];
    LLAttention& operator[](int idx) { return mAttentions[idx]; }
};

// Default attribute set data.
// Used to initialize the global attribute set objects, one of which will be
// refered to by the hud object at any given time.
// Note that the values below are only the default values and that any or all of them
// can be overwritten with customizing data from the XML file. The actual values below
// are those that will give exactly the same look-at behavior as before the ability
// to customize was added. - MG
static const
    LLAttention
        BOY_ATTS[] = { // default set of masculine attentions
            LLAttention(MAX_TIMEOUT, 0, "None",          LLColor3(0.3f, 0.3f, 0.3f)), // LOOKAT_TARGET_NONE
            LLAttention(3.f,         1, "Idle",          LLColor3(0.5f, 0.5f, 0.5f)), // LOOKAT_TARGET_IDLE
            LLAttention(4.f,         3, "AutoListen",    LLColor3(0.5f, 0.5f, 0.5f)), // LOOKAT_TARGET_AUTO_LISTEN
            LLAttention(2.f,         2, "FreeLook",      LLColor3(0.5f, 0.5f, 0.9f)), // LOOKAT_TARGET_FREELOOK
            LLAttention(4.f,         3, "Respond",       LLColor3(0.0f, 0.0f, 0.0f)), // LOOKAT_TARGET_RESPOND
            LLAttention(1.f,         4, "Hover",         LLColor3(0.5f, 0.9f, 0.5f)), // LOOKAT_TARGET_HOVER
            LLAttention(MAX_TIMEOUT, 0, "Conversation",  LLColor3(0.1f, 0.1f, 0.5f)), // LOOKAT_TARGET_CONVERSATION
            LLAttention(MAX_TIMEOUT, 6, "Select",        LLColor3(0.9f, 0.5f, 0.5f)), // LOOKAT_TARGET_SELECT
            LLAttention(MAX_TIMEOUT, 6, "Focus",         LLColor3(0.9f, 0.5f, 0.9f)), // LOOKAT_TARGET_FOCUS
            LLAttention(MAX_TIMEOUT, 7, "Mouselook",     LLColor3(0.9f, 0.9f, 0.5f)), // LOOKAT_TARGET_MOUSELOOK
            LLAttention(0.f,         8, "Clear",         LLColor3(1.0f, 1.0f, 1.0f)), // LOOKAT_TARGET_CLEAR
        },
        GIRL_ATTS[] = { // default set of feminine attentions
            LLAttention(MAX_TIMEOUT, 0, "None",          LLColor3(0.3f, 0.3f, 0.3f)), // LOOKAT_TARGET_NONE
            LLAttention(3.f,         1, "Idle",          LLColor3(0.5f, 0.5f, 0.5f)), // LOOKAT_TARGET_IDLE
            LLAttention(4.f,         3, "AutoListen",    LLColor3(0.5f, 0.5f, 0.5f)), // LOOKAT_TARGET_AUTO_LISTEN
            LLAttention(2.f,         2, "FreeLook",      LLColor3(0.5f, 0.5f, 0.9f)), // LOOKAT_TARGET_FREELOOK
            LLAttention(4.f,         3, "Respond",       LLColor3(0.0f, 0.0f, 0.0f)), // LOOKAT_TARGET_RESPOND
            LLAttention(1.f,         4, "Hover",         LLColor3(0.5f, 0.9f, 0.5f)), // LOOKAT_TARGET_HOVER
            LLAttention(MAX_TIMEOUT, 0, "Conversation",  LLColor3(0.1f, 0.1f, 0.5f)), // LOOKAT_TARGET_CONVERSATION
            LLAttention(MAX_TIMEOUT, 6, "Select",        LLColor3(0.9f, 0.5f, 0.5f)), // LOOKAT_TARGET_SELECT
            LLAttention(MAX_TIMEOUT, 6, "Focus",         LLColor3(0.9f, 0.5f, 0.9f)), // LOOKAT_TARGET_FOCUS
            LLAttention(MAX_TIMEOUT, 7, "Mouselook",     LLColor3(0.9f, 0.9f, 0.5f)), // LOOKAT_TARGET_MOUSELOOK
            LLAttention(0.f,         8, "Clear",         LLColor3(1.0f, 1.0f, 1.0f)), // LOOKAT_TARGET_CLEAR
        };

static LLAttentionSet
    gBoyAttentions(BOY_ATTS),
    gGirlAttentions(GIRL_ATTS);


static bool loadGender(LLXmlTreeNode* gender)
{
    if( !gender)
    {
        return false;
    }
    std::string str;
    gender->getAttributeString("name", str);
    LLAttentionSet& attentions = (str.compare("Masculine") == 0) ? gBoyAttentions : gGirlAttentions;
    for (LLXmlTreeNode* attention_node = gender->getChildByName( "param" );
         attention_node;
         attention_node = gender->getNextNamedChild())
    {
        attention_node->getAttributeString("attention", str);
        LLAttention* attention;
        if     (str == "idle")         attention = &attentions[LOOKAT_TARGET_IDLE];
        else if(str == "auto_listen")  attention = &attentions[LOOKAT_TARGET_AUTO_LISTEN];
        else if(str == "freelook")     attention = &attentions[LOOKAT_TARGET_FREELOOK];
        else if(str == "respond")      attention = &attentions[LOOKAT_TARGET_RESPOND];
        else if(str == "hover")        attention = &attentions[LOOKAT_TARGET_HOVER];
        else if(str == "conversation") attention = &attentions[LOOKAT_TARGET_CONVERSATION];
        else if(str == "select")       attention = &attentions[LOOKAT_TARGET_SELECT];
        else if(str == "focus")        attention = &attentions[LOOKAT_TARGET_FOCUS];
        else if(str == "mouselook")    attention = &attentions[LOOKAT_TARGET_MOUSELOOK];
        else return false;

        F32 priority, timeout;
        attention_node->getAttributeF32("priority", priority);
        attention_node->getAttributeF32("timeout", timeout);
        if(timeout < 0) timeout = MAX_TIMEOUT;
        attention->mPriority = priority;
        attention->mTimeout = timeout;
    }
    return true;
}

static bool loadAttentions()
{
    static bool first_time = true;
    if( ! first_time)
    {
        return true; // maybe not ideal but otherwise it can continue to fail forever.
    }
    first_time = false;

    std::string filename;
    filename = gDirUtilp->getExpandedFilename(LL_PATH_CHARACTER,"attentions.xml");
    LLXmlTree xml_tree;
    bool success = xml_tree.parseFile( filename, false );
    if( !success )
    {
        return false;
    }
    LLXmlTreeNode* root = xml_tree.getRoot();
    if( !root )
    {
        return false;
    }

    //-------------------------------------------------------------------------
    // <linden_attentions version="1.0"> (root)
    //-------------------------------------------------------------------------
    if( !root->hasName( "linden_attentions" ) )
    {
        LL_WARNS() << "Invalid linden_attentions file header: " << filename << LL_ENDL;
        return false;
    }

    std::string version;
    static LLStdStringHandle version_string = LLXmlTree::addAttributeString("version");
    if( !root->getFastAttributeString( version_string, version ) || (version != "1.0") )
    {
        LL_WARNS() << "Invalid linden_attentions file version: " << version << LL_ENDL;
        return false;
    }

    //-------------------------------------------------------------------------
    // <gender>
    //-------------------------------------------------------------------------
    for (LLXmlTreeNode* child = root->getChildByName( "gender" );
         child;
         child = root->getNextNamedChild())
    {
        if( !loadGender( child ) )
        {
            return false;
        }
    }

    return true;
}




//-----------------------------------------------------------------------------
// LLHUDEffectLookAt()
//-----------------------------------------------------------------------------
LLHUDEffectLookAt::LLHUDEffectLookAt(const U8 type) :
    LLHUDEffect(type),
    mKillTime(0.f),
    mLastSendTime(0.f)
{
    clearLookAtTarget();
    // parse the default sets
    loadAttentions();
    // initialize current attention set. switches when avatar sex changes.
    mAttentions = &gGirlAttentions;
}

//-----------------------------------------------------------------------------
// ~LLHUDEffectLookAt()
//-----------------------------------------------------------------------------
LLHUDEffectLookAt::~LLHUDEffectLookAt()
{
}

//-----------------------------------------------------------------------------
// packData()
//-----------------------------------------------------------------------------
void LLHUDEffectLookAt::packData(LLMessageSystem *mesgsys)
{
    // #1121: the head tilt rides in the Color field while there is a target; otherwise the
    // default colour goes out as before.
    const bool send_roll = mHasRoll && mTargetType != LOOKAT_TARGET_NONE && mTargetType != LOOKAT_TARGET_CLEAR;
    mSentHasRoll = send_roll;
    mSentRollDeg = send_roll ? llclamp(ll_round(mTargetRoll * RAD_TO_DEG), -ROLL_MAX_DEG, ROLL_MAX_DEG) : 0;
    if (send_roll)
    {
        const U8 roll_byte = (U8)(S8)mSentRollDeg;
        mColor.set(ROLL_TAG, ROLL_VERSION, roll_byte, rollCheck(ROLL_TAG, ROLL_VERSION, roll_byte));
    }
    else
    {
        mColor = LLColor4U();
    }

    // Pack the default data
    LLHUDEffect::packData(mesgsys);

    // Pack the type-specific data.  Uses a fun packed binary format.  Whee!
    U8 packed_data[PKT_SIZE];
    memset(packed_data, 0, PKT_SIZE);

    if (mSourceObject)
    {
        htolememcpy(&(packed_data[SOURCE_AVATAR]), mSourceObject->mID.mData, MVT_LLUUID, 16);
    }
    else
    {
        htolememcpy(&(packed_data[SOURCE_AVATAR]), LLUUID::null.mData, MVT_LLUUID, 16);
    }

    // pack both target object and position
    // position interpreted as offset if target object is non-null
    if (mTargetObject)
    {
        htolememcpy(&(packed_data[TARGET_OBJECT]), mTargetObject->mID.mData, MVT_LLUUID, 16);
    }
    else
    {
        htolememcpy(&(packed_data[TARGET_OBJECT]), LLUUID::null.mData, MVT_LLUUID, 16);
    }

    htolememcpy(&(packed_data[TARGET_POS]), mTargetOffsetGlobal.mdV, MVT_LLVector3d, 24);

    U8 lookAtTypePacked = (U8)mTargetType;

    htolememcpy(&(packed_data[LOOKAT_TYPE]), &lookAtTypePacked, MVT_U8, 1);

    mesgsys->addBinaryDataFast(_PREHASH_TypeData, packed_data, PKT_SIZE);

    mLastSendTime = mTimer.getElapsedTimeF32();
}

//-----------------------------------------------------------------------------
// unpackData()
//-----------------------------------------------------------------------------
void LLHUDEffectLookAt::unpackData(LLMessageSystem *mesgsys, S32 blocknum)
{
    LLVector3d new_target;
    U8 packed_data[PKT_SIZE];

    LLUUID dataId;
    mesgsys->getUUIDFast(_PREHASH_Effect, _PREHASH_ID, dataId, blocknum);

    if (!gAgentCamera.mLookAt.isNull() && dataId == gAgentCamera.mLookAt->getID())
    {
        return;
    }

    LLHUDEffect::unpackData(mesgsys, blocknum);
    // #1121: a Slipstream sender's head tilt, in the Color field (see packData).
    {
        const U8* c = mColor.mV;
        mHasRoll = c[0] == ROLL_TAG && c[1] == ROLL_VERSION && c[3] == rollCheck(c[0], c[1], c[2]);
        mTargetRoll = mHasRoll ? (F32)llclamp((S32)(S8)c[2], -ROLL_MAX_DEG, ROLL_MAX_DEG) * DEG_TO_RAD : 0.f;
    }
    LLUUID source_id;
    LLUUID target_id;
    S32 size = mesgsys->getSizeFast(_PREHASH_Effect, blocknum, _PREHASH_TypeData);
    if (size != PKT_SIZE)
    {
        LL_WARNS() << "LookAt effect with bad size " << size << LL_ENDL;
        return;
    }
    mesgsys->getBinaryDataFast(_PREHASH_Effect, _PREHASH_TypeData, packed_data, PKT_SIZE, blocknum);

    htolememcpy(source_id.mData, &(packed_data[SOURCE_AVATAR]), MVT_LLUUID, 16);

    LLViewerObject *objp = gObjectList.findObject(source_id);
    if (objp && objp->isAvatar())
    {
        setSourceObject(objp);
    }
    else
    {
        //LL_WARNS() << "Could not find source avatar for lookat effect" << LL_ENDL;
        return;
    }

    htolememcpy(target_id.mData, &(packed_data[TARGET_OBJECT]), MVT_LLUUID, 16);

    objp = gObjectList.findObject(target_id);

    htolememcpy(new_target.mdV, &(packed_data[TARGET_POS]), MVT_LLVector3d, 24);

    if (objp)
    {
        setTargetObjectAndOffset(objp, new_target);
    }
    else if (target_id.isNull())
    {
        setTargetPosGlobal(new_target);
    }
    else
    {
        //LL_WARNS() << "Could not find target object for lookat effect" << LL_ENDL;
    }

    U8 lookAtTypeUnpacked = 0;
    htolememcpy(&lookAtTypeUnpacked, &(packed_data[LOOKAT_TYPE]), MVT_U8, 1);
    if ((U8)LOOKAT_NUM_TARGETS > lookAtTypeUnpacked)
    {
        mTargetType = (ELookAtType)lookAtTypeUnpacked;
    }
    else
    {
        mTargetType = LOOKAT_TARGET_NONE;
        LL_DEBUGS("HUDEffect") << "Invalid target type: " << lookAtTypeUnpacked << LL_ENDL;
    }
    if (mTargetType == LOOKAT_TARGET_NONE)
    {
        clearLookAtTarget();
    }
}

//-----------------------------------------------------------------------------
// setTargetObjectAndOffset()
//-----------------------------------------------------------------------------
void LLHUDEffectLookAt::setTargetObjectAndOffset(LLViewerObject *objp, LLVector3d offset)
{
    mTargetObject = objp;
    mTargetOffsetGlobal = offset;
}

//-----------------------------------------------------------------------------
// setTargetPosGlobal()
//-----------------------------------------------------------------------------
void LLHUDEffectLookAt::setTargetPosGlobal(const LLVector3d &target_pos_global)
{
    mTargetObject = NULL;
    mTargetOffsetGlobal = target_pos_global;
}

//-----------------------------------------------------------------------------
// setLookAt()
// called by agent logic to set look at behavior locally, and propagate to sim
//-----------------------------------------------------------------------------
bool LLHUDEffectLookAt::setLookAt(ELookAtType target_type, LLViewerObject *object, LLVector3 position)
{
    if (!mSourceObject)
    {
        return false;
    }

    if (target_type >= LOOKAT_NUM_TARGETS)
    {
        LL_WARNS() << "Bad target_type " << (int)target_type << " - ignoring." << LL_ENDL;
        return false;
    }

    // must be same or higher priority than existing effect
    if ((*mAttentions)[target_type].mPriority < (*mAttentions)[mTargetType].mPriority)
    {
        return false;
    }

    F32 current_time  = mTimer.getElapsedTimeF32();

    bool looking_at_self = false;
    if (object && object->isAvatar())
    {
        if (auto avatarp = object->asAvatar())
        {
            if (avatarp->isSelf())
            {
                looking_at_self = true;
            }
        }
    }
    static LLCachedControl<bool> clamp_lookat_enabled(gSavedSettings, "LimitLookAtTarget", false);
    bool clamp_lookat = clamp_lookat_enabled && isAgentAvatarValid() && !looking_at_self &&
                        (*mAttentions)[target_type].mName != "Respond" &&
                        (*mAttentions)[target_type].mName != "Conversation" &&
                        (*mAttentions)[target_type].mName != "AutoListen";

    if (!clamp_lookat)
    {
        // type of lookat behavior or target object has changed
        bool lookAtChanged = (target_type != mTargetType) || (object != mTargetObject.get());

        // lookat position has moved a certain amount and we haven't just sent an update
        lookAtChanged = lookAtChanged || ((dist_vec_squared(position, mLastSentOffsetGlobal) > MIN_DELTAPOS_FOR_UPDATE_SQUARED) &&
            ((current_time - mLastSendTime) > (1.f / MAX_SENDS_PER_SEC)));

        if (lookAtChanged)
        {
            mLastSentOffsetGlobal = position;
            F32 timeout = (*mAttentions)[target_type].mTimeout;
            setDuration(timeout);
            setNeedsSendToSim(true);
        }
    }

    if (target_type == LOOKAT_TARGET_CLEAR)
    {
        clearLookAtTarget();
    }
    else
    {
        mTargetType = target_type;
        mTargetObject = object;
        if (object)
        {
            if(clamp_lookat)
            {
                // Pretend to look at the object
                mTargetOffsetGlobal.setVec(object->getPositionGlobal() + (LLVector3d)(position * object->getRotationRegion()));
                mTargetObject = NULL;
            }
            else
            {
                mTargetOffsetGlobal.setVec(position);
            }
        }
        else
        {
            mTargetOffsetGlobal = gAgent.getPosGlobalFromAgent(position);
        }

        if (clamp_lookat)
        {
            static LLCachedControl<F32> lookat_clamp_distance(gSavedSettings, "LimitLookAtTargetDistance", 1.0f);
            auto head_position = gAgent.getPosGlobalFromAgent(gAgentAvatarp->mHeadp->getWorldPosition());
            auto distance = dist_vec(mTargetOffsetGlobal, head_position);

            if (distance > lookat_clamp_distance)
            {
                auto distance_from_object = (mTargetOffsetGlobal - head_position) * (lookat_clamp_distance / distance);
                mTargetOffsetGlobal.setVec(head_position + distance_from_object);
            }

            bool lookat_changed = target_type != mTargetType;

            // update position
            if (!lookat_changed)
            {
                auto distance_difference = dist_vec_squared(gAgent.getPosAgentFromGlobal(mTargetOffsetGlobal), mLastSentOffsetGlobal);
                auto time_difference = (current_time - mLastSendTime);
                if (distance_difference > MIN_DELTAPOS_FOR_UPDATE_SQUARED && time_difference > (1.f / MAX_SENDS_PER_SEC))
                {
                    lookat_changed = true;
                }
            }
            if (lookat_changed)
            {
                mLastSentOffsetGlobal = gAgent.getPosAgentFromGlobal(mTargetOffsetGlobal);
                F32 timeout = (*mAttentions)[target_type].mTimeout;
                setDuration(timeout);
                setNeedsSendToSim(true);
            }
        }
        mKillTime = mTimer.getElapsedTimeF32() + mDuration;

        update();
    }
    return true;
}

//-----------------------------------------------------------------------------
// clearLookAtTarget()
//-----------------------------------------------------------------------------
void LLHUDEffectLookAt::setHeadRoll(F32 radians, bool valid)
{
    valid = valid && std::isfinite(radians);
    mHasRoll = valid;
    mTargetRoll = valid ? radians : 0.f;
    if (mTargetType == LOOKAT_TARGET_NONE || mTargetType == LOOKAT_TARGET_CLEAR)
    {
        return;     // nothing to tilt; packData sends no roll then
    }
    // The target's own send gate: a tilt change worth sending, at most MAX_SENDS_PER_SEC.
    const S32 deg = valid ? llclamp(ll_round(radians * RAD_TO_DEG), -ROLL_MAX_DEG, ROLL_MAX_DEG) : 0;
    const bool changed = valid != mSentHasRoll || llabs(deg - mSentRollDeg) >= ROLL_SEND_DEG;
    if (changed && (mTimer.getElapsedTimeF32() - mLastSendTime) > (1.f / MAX_SENDS_PER_SEC))
    {
        setNeedsSendToSim(true);
    }
}

void LLHUDEffectLookAt::clearLookAtTarget()
{
    mTargetObject = NULL;
    mTargetOffsetGlobal.clearVec();
    mTargetType = LOOKAT_TARGET_NONE;
    mTargetRoll = 0.f;
    mHasRoll = false;
    if (mSourceObject.notNull())
    {
        ((LLVOAvatar*)(LLViewerObject*)mSourceObject)->stopMotion(ANIM_AGENT_HEAD_ROT);
    }
}

//-----------------------------------------------------------------------------
// markDead()
//-----------------------------------------------------------------------------
void LLHUDEffectLookAt::markDead()
{
    if (mSourceObject.notNull())
    {
        ((LLVOAvatar*)(LLViewerObject*)mSourceObject)->removeAnimationData(LLCharacter::ANIM_DATA_LOOK_AT_POINT);
        ((LLVOAvatar*)(LLViewerObject*)mSourceObject)->removeAnimationData(LLCharacter::ANIM_DATA_LOOK_AT_ROLL);
    }

    mSourceObject = NULL;
    clearLookAtTarget();
    LLHUDEffect::markDead();
}

void LLHUDEffectLookAt::setSourceObject(LLViewerObject* objectp)
{
    // restrict source objects to avatars
    if (objectp && objectp->isAvatar())
    {
        LLHUDEffect::setSourceObject(objectp);
    }
}

//-----------------------------------------------------------------------------
// render()
//-----------------------------------------------------------------------------
void LLHUDEffectLookAt::render()
{
    static LLCachedControl<bool> show_lookat(gSavedSettings, "SlipstreamLookAtShow", false);
    if (show_lookat && mSourceObject.notNull())
    {
        static LLCachedControl<bool> isOwnHidden(gSavedSettings, "SlipstreamLookAtHideSelf", true);
        static LLCachedControl<bool> isPrivate(gSavedSettings, "EnableLookAtTarget", false);

        if ((isOwnHidden || isPrivate || gAgentCamera.cameraMouselook()) && static_cast<LLVOAvatar*>(mSourceObject.get())->isSelf())
            return;

        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

        LLRender::DepthScope gls_depth(true, false);

        LLVector3 target = mTargetPos + ((LLVOAvatar*)(LLViewerObject*)mSourceObject)->mHeadp->getWorldPosition();
        gGL.matrixMode(LLRender::MM_MODELVIEW);
        gGL.pushMatrix();
        gGL.translatef(target.mV[VX], target.mV[VY], target.mV[VZ]);
        gGL.scalef(0.1f, 0.1f, 0.1f);
        gGL.begin(LLRender::LINES);
        {
            LLColor3 color = (*mAttentions)[mTargetType].mColor;
            gGL.color3f(color.mV[VRED], color.mV[VGREEN], color.mV[VBLUE]);
            gGL.vertex3f(-1.f, 0.f, 0.f);
            gGL.vertex3f(1.f, 0.f, 0.f);

            gGL.vertex3f(0.f, -1.f, 0.f);
            gGL.vertex3f(0.f, 1.f, 0.f);

            gGL.vertex3f(0.f, 0.f, -1.f);
            gGL.vertex3f(0.f, 0.f, 1.f);

            static LLCachedControl<bool> lookAtLines(gSavedSettings, "SlipstreamLookAtLines", false);
            if(lookAtLines)
            {
                const std::string targname = (*mAttentions)[mTargetType].mName;
                if(targname != "None" && targname != "Idle" && targname != "AutoListen")
                {
                    LLVector3 dist = (mSourceObject->getWorldPosition() - mTargetPos) * 10;
                    gGL.vertex3f(0.f, 0.f, 0.f);
                    gGL.vertex3f(dist.mV[VX], dist.mV[VY], dist.mV[VZ] + 0.5f);
                }
            }
        }
        gGL.end();
        gGL.popMatrix();

        static LLCachedControl<U32> lookAtNames(gSavedSettings, "SlipstreamLookAtNames", 0);
        if(lookAtNames > 0)
        {
            std::string text;
            LLAvatarName av_name;
            LLAvatarNameCache::get(static_cast<LLVOAvatar*>(mSourceObject.get())->getID(), &av_name);
            switch (lookAtNames)
            {
                case 1: // Display Name (user.name)
                    text = av_name.getCompleteName();
                    break;
                case 2: // Display Name
                    text = av_name.getDisplayName();
                    break;
                case 3: // First Last
                    text = av_name.getUserName();
                    break;
                default: //user.name
                    text = av_name.getAccountName();
                    break;
            }

            const LLFontGL* fontp = LLFontGL::getFontSansSerif();
            gGL.pushMatrix();

            LLWString wstr(utf8str_to_wstring(text));

            hud_render_text(
                wstr,
                target + LLVector3(0.f, 0.f, 0.15f),
                *fontp,
                LLFontGL::NORMAL,
                LLFontGL::NO_SHADOW,
                -0.5f * fontp->getWidthF32(wstr.c_str()),
                0.0f,
                (*mAttentions)[mTargetType].mColor,
                FALSE
            );
            gGL.popMatrix();
        }
    }
}

//-----------------------------------------------------------------------------
// update()
//-----------------------------------------------------------------------------
void LLHUDEffectLookAt::update()
{
    // If the target object is dead, set the target object to NULL
    if (!mTargetObject.isNull() && mTargetObject->isDead())
    {
        clearLookAtTarget();
    }

    // if source avatar is null or dead, mark self as dead and return
    if (mSourceObject.isNull() || mSourceObject->isDead())
    {
        markDead();
        return;
    }

    // make sure the proper set of avatar attention are currently being used.
    LLVOAvatar* source_avatar = (LLVOAvatar*)(LLViewerObject*)mSourceObject;
    // for now the first cut will just switch on sex. future development could adjust
    // timeouts according to avatar age and/or other features.
    mAttentions = (source_avatar->getSex() == SEX_MALE) ? &gBoyAttentions : &gGirlAttentions;
    //printf("updated to %s\n", (source_avatar->getSex() == SEX_MALE) ? "male" : "female");

    F32 time = mTimer.getElapsedTimeF32();

    // clear out the effect if time is up
    if (mKillTime != 0.f && time > mKillTime)
    {
        if (mTargetType != LOOKAT_TARGET_NONE)
        {
            clearLookAtTarget();
            // look at timed out (only happens on own avatar), so tell everyone
            setNeedsSendToSim(true);
        }
    }

    if (mTargetType != LOOKAT_TARGET_NONE)
    {
        if (calcTargetPosition())
        {
            static LLCachedControl<bool> disable_look_at(gSavedSettings, "DisableLookAtAnimation", true);
            LLMotion* head_motion = ((LLVOAvatar*)(LLViewerObject*)mSourceObject)->findMotion(ANIM_AGENT_HEAD_ROT);
            // Slipstream (#902): while the look-at sync layer turns this avatar's head along
            // its own bone chain, LL's head turn stands down (it would turn the head twice).
            const bool synced = SLPoseLookAt::instanceExists() && SLPoseLookAt::instance().isDriving(mSourceObject->getID());
            if (disable_look_at() || synced)
            {
                if (head_motion)
                {
                    ((LLVOAvatar*)(LLViewerObject*)mSourceObject)->stopMotion(ANIM_AGENT_HEAD_ROT);
                }
            }
            else if (!head_motion || head_motion->isStopped())
            {
                ((LLVOAvatar*)(LLViewerObject*)mSourceObject)->startMotion(ANIM_AGENT_HEAD_ROT);
            }
        }
    }

    static LLCachedControl<bool> lookAtDebug(gSavedSettings, "SlipstreamLookAtDebug", false);
    if (lookAtDebug)
    {
        ((LLVOAvatar*)(LLViewerObject*)mSourceObject)->addDebugText((*mAttentions)[mTargetType].mName);
    }
}

/**
 * Initializes the mTargetPos member from the current mSourceObjec and mTargetObject
 * (and possibly mTargetOffsetGlobal).
 * When mTargetObject is another avatar, it sets mTargetPos to be their eyes.
 *
 * Has the side-effect of also calling setAnimationData(LLCharacter::ANIM_DATA_LOOK_AT_POINT) with the new
 * mTargetPos on the source object which is assumed to be an avatar.
 *
 * Returns whether we successfully calculated a finite target position.
 */
bool LLHUDEffectLookAt::calcTargetPosition()
{
    LLViewerObject *target_obj = (LLViewerObject *)mTargetObject;
    LLVector3 local_offset;

    if (target_obj)
    {
        local_offset.setVec(mTargetOffsetGlobal);
    }
    else
    {
        local_offset = gAgent.getPosAgentFromGlobal(mTargetOffsetGlobal);
    }

    LLVOAvatar* source_avatar = (LLVOAvatar*)(LLViewerObject*)mSourceObject;
    if (!source_avatar->isBuilt())
        return false;

    if (target_obj && target_obj->mDrawable.notNull())
    {
        LLQuaternion target_rot;
        if (target_obj->isAvatar())
        {
            LLVOAvatar *target_av = (LLVOAvatar *)target_obj;

            bool looking_at_self = source_avatar->isSelf() && target_av->isSelf();

            // if selecting self, stare forward
            if (looking_at_self && mTargetOffsetGlobal.magVecSquared() < MIN_TARGET_OFFSET_SQUARED)
            {
                //sets the lookat point in front of the avatar
                mTargetOffsetGlobal.setVec(5.0, 0.0, 0.0);
                local_offset.setVec(mTargetOffsetGlobal);
            }

            // look the other avatar in the eye. note: what happens if target is self? -MG
            mTargetPos = target_av->mHeadp->getWorldPosition();
            if (mTargetType == LOOKAT_TARGET_MOUSELOOK || mTargetType == LOOKAT_TARGET_FREELOOK)
            {
                // mouselook and freelook target offsets are absolute
                target_rot = LLQuaternion::DEFAULT;
            }
            else if (looking_at_self && gAgentCamera.cameraCustomizeAvatar())
            {
                // *NOTE: We have to do this because animation
                // overrides do not set lookat behavior.
                // *TODO: animation overrides for lookat behavior.
                target_rot = target_av->mPelvisp->getWorldRotation();
            }
            else
            {
                target_rot = target_av->mRoot->getWorldRotation();
            }
        }
        else // target obj is not an avatar
        {
            if (target_obj->mDrawable->getGeneration() == -1)
            {
                mTargetPos = target_obj->getPositionAgent();
                target_rot = target_obj->getWorldRotation();
            }
            else
            {
                mTargetPos = target_obj->getRenderPosition();
                target_rot = target_obj->getRenderRotation();
            }
        }

        mTargetPos += (local_offset * target_rot);
    }
    else // no target obj or it's not drawable
    {
        mTargetPos = local_offset;
    }

    mTargetPos -= source_avatar->mHeadp->getWorldPosition();

    if (!mTargetPos.isFinite())
        return false;

    static LLCachedControl<bool> disable_look_at(gSavedSettings, "DisableLookAtAnimation", true);
    if (disable_look_at())
    {
        source_avatar->removeAnimationData(LLCharacter::ANIM_DATA_LOOK_AT_POINT);
        source_avatar->removeAnimationData(LLCharacter::ANIM_DATA_LOOK_AT_ROLL);
    }
    else
    {
        source_avatar->setAnimationData(LLCharacter::ANIM_DATA_LOOK_AT_POINT, (void*)&mTargetPos);
        // #1121: the head tilt with it (head_rot and the look-at sync layer read it).
        if (mHasRoll)
        {
            source_avatar->setAnimationData(LLCharacter::ANIM_DATA_LOOK_AT_ROLL, (void*)&mTargetRoll);
        }
        else
        {
            source_avatar->removeAnimationData(LLCharacter::ANIM_DATA_LOOK_AT_ROLL);
        }
    }

    return true;
}
