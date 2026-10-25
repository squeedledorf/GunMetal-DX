/**
 * @file llviewerpartsource.cpp
 * @brief LLViewerPartSource class implementation
 *
 * $LicenseInfo:firstyear=2003&license=viewerlgpl$
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
#include "llviewerpartsource.h"

#include "llviewercontrol.h"
#include "llrender.h"
#include "llframetimer.h"

#include "llcleanparticlestore.h"
#include "llagent.h"
#include "lldrawable.h"
#include "llviewercamera.h"
#include "llviewertexturelist.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llviewerjointattachment.h"
#include "llvoavatar.h"
#include "llworld.h"
#include "pipeline.h"


static LLVOAvatar* find_avatar(const LLUUID& id)
{
    LLViewerObject *obj = gObjectList.findObject(id);
    while (obj && obj->isAttachment())
    {
        obj = (LLViewerObject *)obj->getParent();
    }

    if (obj && obj->isAvatar())
    {
        return (LLVOAvatar*)obj;
    }
    else
    {
        return NULL;
    }
}

// Resolve a source's particle texture into the emitter's bindless handle,
// boosting it so it loads immediately (the GPU path bypasses the legacy
// face-setup fetch request). Hands the UUID back so the store keeps the
// handle fresh after the source dies. Shared by all GPU emitter builders.
static void fillGPUEmitterTexture(LLViewerTexture* imagep, CleanEmitter& out, LLUUID& out_img)
{
    LLUUID img = imagep ? imagep->getID() : LLUUID::null;
    if (imagep)
    {
        imagep->setBoostLevel(LLGLTexture::BOOST_HIGH);
        imagep->addTextureStats(2048.f * 2048.f);
    }
    out_img = img;
    // Vulkan resolves the texture from out_img (the store overwrites tex_handle).
    out.tex_handle[0] = out.tex_handle[1] = 0u;
}

// #693: the render transform of a worn (non-HUD, unrigged) attachment prim on a
// GPU-animated avatar, composed from its attachment point's LIVE world matrix. LL's
// getRenderPosition / getRenderRotation read the attachment drawable's world xform,
// which LLXformMatrix::update derives from the attachment point joint's CACHED world
// transform -- and on a GPU-animated avatar (LLCleanAnimStore: no per-frame
// updateWorldMatrixChildren) that cache is only kept current by the dirty-flag driven
// refreshWorld, so whenever the flags do not reach the point (a base joint the readback
// flagged stops the root's touch; a GPU_EXT_DIRTY write falls back to a parent chain
// nobody composed) it holds a pose from frames -- possibly metres -- back. The worn mesh
// is drawn from the GPU joint table and never shows it; the particle source (and every
// FOLLOW_SRC particle riding it) jumped between the stale and the current hand.
// Same math as LLXformMatrix::update (drawable local xform x the point's world) and
// LLDrawable::getPositionAgent / LLViewerObject::getRenderRotation for a child prim.
// False: not such an attachment -- the caller keeps LL's getters.
static bool vk_live_attachment_xform(LLViewerObject* obj, LLVector3& pos, LLQuaternion& rot)
{
    if (!obj || obj->isDead() || !obj->isAttachment() || obj->isHUDAttachment() || obj->mDrawable.isNull())
        return false;
    // Rigged: LL's getters return the avatar position / identity (kept as is).
    if (obj->mDrawable->isState(LLDrawable::RIGGED)) return false;
    LLViewerObject* root = obj->getRootEdit();
    LLVOAvatar* av = obj->getAvatar();
    if (!root || root->mDrawable.isNull() || !av || av->isDead() || av->mGpuAnimMode != 2) return false;
    const S32 att_id = ATTACHMENT_ID_FROM_STATE(root->getAttachmentState()) & ~ATTACHMENT_ADD;
    LLViewerJointAttachment* att = get_if_there(av->mAttachmentPoints, att_id, (LLViewerJointAttachment*)NULL);
    const LLXformMatrix& rx = root->mDrawable->mXform;
    if (!att || rx.getParent() != att->getXform()) return false;
    att->getWorldMatrixLive(LLJoint::nextLiveStamp());   // refreshes the point's world PRS
    const LLXformMatrix* jx = att->getXform();
    LLVector3 rp = rx.getPosition();
    if (jx->getScaleChildOffset()) rp.scaleVec(jx->getScale());
    const LLQuaternion jrot = jx->getWorldRotation();
    const LLVector3 root_pos = rp * jrot + jx->getWorldPosition();
    const LLQuaternion root_rot = rx.getRotation() * jrot;
    if (obj == root || obj->mDrawable->isRoot())
    {
        pos = root_pos;
        rot = root_rot;
    }
    else
    {
        pos = obj->getPosition() * root_rot + root_pos;
        rot = obj->getRotation() * root_rot;
    }
    return true;
}

LLViewerPartSource::LLViewerPartSource(const U32 type) :
    mType(type),
    mOwnerUUID(LLUUID::null),
    mPartFlags(0)
{
    mLastUpdateTime = 0.f;
    mLastPartTime = 0.f;
    mIsDead = false;
    mIsSuspended = false;
    static U32 id_seed = 0;
    mID = ++id_seed;

    mLastPart = NULL;

    mDelay = 0 ;
}

void LLViewerPartSource::setDead()
{
    mIsDead = true;
}


void LLViewerPartSource::updatePart(LLViewerPart &part, const F32 dt)
{
}

void LLViewerPartSource::update(const F32 dt)
{
    LL_ERRS() << "Creating default part source!" << LL_ENDL;
}

LLUUID LLViewerPartSource::getImageUUID() const
{
    LLViewerTexture* imagep = mImagep;
    if(imagep)
    {
        return imagep->getID();
    }
    return LLUUID::null;
}
void LLViewerPartSource::setStart()
{
    //cancel delaying to start a new added particle source, because some particle source just emits for a short time.
    //however, canceling this might cause overall particle emmitting fluctuate for a while because the new added source jumps to
    //the current particle emmitting settings instantly. -->bao
    mDelay = 0 ; //99
}

// RenderVKParticleDiag (#693): script particle sources constructed per source object.
// More than one per object means LL replaced the object's source (PSYS_SRC_MAX_AGE
// expiry + a re-sent block, llParticleSystem([]) toggles, the object re-rezzing from the
// cache): each new source fires its first burst at once, which reads as "the burst
// count is not respected".
static std::map<LLUUID, U32>& diagObjectSources()
{
    static std::map<LLUUID, U32> s_counts;
    return s_counts;
}

LLViewerPartSourceScript::LLViewerPartSourceScript(LLViewerObject *source_objp) :
    LLViewerPartSource(LL_PART_SOURCE_SCRIPT)
{
    llassert(source_objp);
    static LLCachedControl<bool> s_pdiag(gSavedSettings, "RenderVKParticleDiag", false);
    if (s_pdiag && source_objp) ++diagObjectSources()[source_objp->getID()];
    mSourceObjectp = source_objp;
    mPosAgent = mSourceObjectp->getPositionAgent();
    mImagep = LLViewerFetchedTexture::sDefaultParticleImagep;

    mImagep->setAddressMode(LLTexUnit::TAM_CLAMP);
}


void LLViewerPartSourceScript::setDead()
{
    mIsDead = true;
    mSourceObjectp = NULL;
    mTargetObjectp = NULL;
}

void LLViewerPartSourceScript::update(const F32 dt)
{
    if( mIsSuspended )
        return;

    if (mOwnerAvatarp.isNull() && mOwnerUUID != LLUUID::null)
    {
        mOwnerAvatarp = find_avatar(mOwnerUUID);
    }
    if (mOwnerAvatarp.notNull() && LLVOAvatar::AOA_NORMAL != mOwnerAvatarp->getOverallAppearance())
    {
        return;
    }

    F32 old_update_time = mLastUpdateTime;
    mLastUpdateTime += dt;

    F32 ref_rate_travelspeed = llmin(LLViewerPartSim::getInstance()->getRefRate(), 1.f);

    F32 dt_update = mLastUpdateTime - mLastPartTime;

    // Update this for objects which have the follow flag set...
    if (!mSourceObjectp.isNull())
    {
        if (mSourceObjectp->isDead())
        {
            mSourceObjectp = NULL;
        }
        else if (mSourceObjectp->mDrawable.notNull())
        {
            mPosAgent = mSourceObjectp->getRenderPosition();
        }
    }

    if (mTargetObjectp.isNull()
        && mPartSysData.mTargetUUID.notNull())
    {
        //
        // Hmm, missing object, let's see if we can find it...
        //

        LLViewerObject *target_objp = gObjectList.findObject(mPartSysData.mTargetUUID);
        setTargetObject(target_objp);
    }

    if (!mTargetObjectp.isNull())
    {
        if (mTargetObjectp->isDead())
        {
            mTargetObjectp = NULL;
        }
        else if (mTargetObjectp->mDrawable.notNull())
        {
            mTargetPosAgent = mTargetObjectp->getRenderPosition();
        }
    }

    if (!mTargetObjectp)
    {
        mTargetPosAgent = mPosAgent;
    }

    if (mPartSysData.mMaxAge && ((mPartSysData.mStartAge + mLastUpdateTime + dt_update) > mPartSysData.mMaxAge))
    {
        // Kill particle source because it has outlived its max age...
        setDead();
        return;
    }


    if (gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_PARTICLES))
    {
        if (mSourceObjectp.notNull())
        {
            std::ostringstream ostr;
            ostr << mPartSysData;
            mSourceObjectp->setDebugText(ostr.str());
        }
    }

    bool first_run = false;
    if (old_update_time <= 0.f)
    {
        first_run = true;
    }

    F32 max_time = llmax(1.f, 10.f*mPartSysData.mBurstRate);
    dt_update = llmin(max_time, dt_update);
    while ((dt_update > mPartSysData.mBurstRate) || first_run)
    {
        first_run = false;

        // Update the rotation of the particle source by the angular velocity
        // First check to see if there is still an angular velocity.
        F32 angular_velocity_mag = mPartSysData.mAngularVelocity.magVec();
        if (angular_velocity_mag != 0.0f)
        {
            F32 av_angle = dt * angular_velocity_mag;
            LLQuaternion dquat(av_angle, mPartSysData.mAngularVelocity);
            mRotation *= dquat;
        }
        else
        {
            // No angular velocity.  Reset our rotation.
            mRotation.setQuat(0, 0, 0);
        }

        if (LLViewerPartSim::getInstance()->aboveParticleLimit())
        {
            // Don't bother doing any more updates if we're above the particle limit,
            // just give up.
            mLastPartTime = mLastUpdateTime;
            break;

        }

        // find the greatest length that the shortest side of a system
        // particle is expected to have
        F32 max_short_side =
            llmax(
                  llmax(llmin(mPartSysData.mPartData.mStartScale[0],
                      mPartSysData.mPartData.mStartScale[1]),
                    llmin(mPartSysData.mPartData.mEndScale[0],
                      mPartSysData.mPartData.mEndScale[1])),
                  llmin((mPartSysData.mPartData.mStartScale[0]
                     + mPartSysData.mPartData.mEndScale[0])/2,
                    (mPartSysData.mPartData.mStartScale[1]
                     + mPartSysData.mPartData.mEndScale[1])/2));

        F32 pixel_meter_ratio = LLViewerCamera::getInstance()->getPixelMeterRatio();

        // Maximum distance at which spawned particles will be viewable
        F32 max_dist = max_short_side * pixel_meter_ratio;

        if (max_dist < 0.25f)
        {
            // < 1 pixel wide at a distance of >=25cm.  Particles
            // this tiny are useless and mostly spawned by buggy
            // sources
            mLastPartTime = mLastUpdateTime;
            break;
        }

        // Distance from camera
        F32 dist = (mPosAgent - LLViewerCamera::getInstance()->getOrigin()).magVec();

        // Particle size vs distance vs maxage throttling

        F32 limited_rate=0.f;
        if (dist - max_dist > 0.f)
        {
            if((dist - max_dist) * ref_rate_travelspeed > mPartSysData.mPartData.mMaxAge - 0.2f )
            {
                // You need to travel faster than 1 divided by reference rate m/s directly towards these particles to see them at least 0.2s
                mLastPartTime = mLastUpdateTime;
                break;
            }
            limited_rate = ((dist - max_dist) * ref_rate_travelspeed) / mPartSysData.mPartData.mMaxAge;
        }

        if(mDelay)
        {
            limited_rate = llmax(limited_rate, 0.01f * mDelay--) ;
        }

        S32 i;
        for (i = 0; i < mPartSysData.mBurstPartCount; i++)
        {
            if (ll_frand() < llmax(1.0f - LLViewerPartSim::getInstance()->getBurstRate(), limited_rate))
            {
                // Limit particle generation
                continue;
            }

            LLViewerPart* part = new LLViewerPart();

            part->init(this, mImagep, NULL);
            part->mFlags = mPartSysData.mPartData.mFlags;
            if (!mSourceObjectp.isNull() && mSourceObjectp->isHUDAttachment())
            {
                part->mFlags |= LLPartData::LL_PART_HUD;
            }

            if (part->mFlags & LLPartData::LL_PART_RIBBON_MASK && mLastPart)
            { //set previous particle's parent to this particle to chain ribbon together
                mLastPart->mParent = part;
                part->mChild = mLastPart;
                part->mAxis = LLVector3(0,0,1);

                if (mSourceObjectp.notNull())
                {
                    LLQuaternion rot = mSourceObjectp->getRenderRotation();
                    part->mAxis *= rot;
                }
            }

            mLastPart = part;

            part->mMaxAge = mPartSysData.mPartData.mMaxAge;
            part->mStartColor = mPartSysData.mPartData.mStartColor;
            part->mEndColor = mPartSysData.mPartData.mEndColor;
            part->mColor = part->mStartColor;

            part->mStartScale = mPartSysData.mPartData.mStartScale;
            part->mEndScale = mPartSysData.mPartData.mEndScale;
            part->mScale = part->mStartScale;

            part->mAccel = mPartSysData.mPartAccel;

            part->mBlendFuncDest = mPartSysData.mPartData.mBlendFuncDest;
            part->mBlendFuncSource = mPartSysData.mPartData.mBlendFuncSource;

            part->mStartGlow = mPartSysData.mPartData.mStartGlow;
            part->mEndGlow = mPartSysData.mPartData.mEndGlow;
            part->mGlow = LLColor4U(0, 0, 0, (U8) ll_round(part->mStartGlow*255.f));

            if (mPartSysData.mPattern & LLPartSysData::LL_PART_SRC_PATTERN_DROP)
            {
                part->mPosAgent = mPosAgent;
                part->mVelocity.setVec(0.f, 0.f, 0.f);
            }
            else if (mPartSysData.mPattern & LLPartSysData::LL_PART_SRC_PATTERN_EXPLODE)
            {
                part->mPosAgent = mPosAgent;
                LLVector3 part_dir_vector;

                F32 mvs;
                do
                {
                    part_dir_vector.mV[VX] = ll_frand(2.f) - 1.f;
                    part_dir_vector.mV[VY] = ll_frand(2.f) - 1.f;
                    part_dir_vector.mV[VZ] = ll_frand(2.f) - 1.f;
                    mvs = part_dir_vector.magVecSquared();
                }
                while ((mvs > 1.f) || (mvs < 0.01f));

                part_dir_vector.normVec();
                part->mPosAgent += mPartSysData.mBurstRadius*part_dir_vector;
                part->mVelocity = part_dir_vector;
                F32 speed = mPartSysData.mBurstSpeedMin + ll_frand(mPartSysData.mBurstSpeedMax - mPartSysData.mBurstSpeedMin);
                part->mVelocity *= speed;
            }
            else if (mPartSysData.mPattern & LLPartSysData::LL_PART_SRC_PATTERN_ANGLE
                || mPartSysData.mPattern & LLPartSysData::LL_PART_SRC_PATTERN_ANGLE_CONE)
            {
                part->mPosAgent = mPosAgent;

                // original implemenetation for part_dir_vector was just:
                LLVector3 part_dir_vector(0.0, 0.0, 1.0);
                // params from the script...
                // outer = outer cone angle
                // inner = inner cone angle
                //      between outer and inner there will be particles
                F32 innerAngle = mPartSysData.mInnerAngle;
                F32 outerAngle = mPartSysData.mOuterAngle;

                // generate a random angle within the given space...
                F32 angle = innerAngle + ll_frand(outerAngle - innerAngle);
                // split which side it will go on randomly...
                if (ll_frand() < 0.5)
                {
                    angle = -angle;
                }
                // Both patterns rotate around the x-axis first:
                part_dir_vector.rotVec(angle, 1.0, 0.0, 0.0);

                // If this is a cone pattern, rotate again to create the cone.
                if (mPartSysData.mPattern & LLPartSysData::LL_PART_SRC_PATTERN_ANGLE_CONE)
                {
                    part_dir_vector.rotVec(ll_frand(4*F_PI), 0.0, 0.0, 1.0);
                }

                // Only apply this rotation if using the deprecated angles.
                if (! (mPartSysData.mFlags & LLPartSysData::LL_PART_USE_NEW_ANGLE))
                {
                    // Deprecated...
                    part_dir_vector.rotVec(outerAngle, 1.0, 0.0, 0.0);
                }

                if (mSourceObjectp)
                {
                    part_dir_vector = part_dir_vector * mSourceObjectp->getRenderRotation();
                }

                part_dir_vector = part_dir_vector * mRotation;

                part->mPosAgent += mPartSysData.mBurstRadius*part_dir_vector;

                part->mVelocity = part_dir_vector;

                F32 speed = mPartSysData.mBurstSpeedMin + ll_frand(mPartSysData.mBurstSpeedMax - mPartSysData.mBurstSpeedMin);
                part->mVelocity *= speed;
            }
            else
            {
                part->mPosAgent = mPosAgent;
                part->mVelocity.setVec(0.f, 0.f, 0.f);
                //LL_WARNS() << "Unknown source pattern " << (S32)mPartSysData.mPattern << LL_ENDL;
            }

            if (part->mFlags & LLPartData::LL_PART_FOLLOW_SRC_MASK ||   // SVC-193, VWR-717
                part->mFlags & LLPartData::LL_PART_TARGET_LINEAR_MASK)
            {
                mPartSysData.mBurstRadius = 0;
            }

            LLViewerPartSim::getInstance()->addPart(part);
        }

        mLastPartTime = mLastUpdateTime;
        dt_update -= mPartSysData.mBurstRate;
    }
}

// PARTICLE FOG (#1003): the classification inputs (CleanPartFogDesc) straight from the
// source's LLPartSysData. The hash covers everything the classifier reads except the
// position, so the renderer reclassifies exactly when the creator's data (or the texture,
// or the attachment state) changes -- llSetLinkPrimitiveParams updates included.
bool LLViewerPartSourceScript::buildGPUFogDesc(CleanPartFogDesc& out) const
{
    out = CleanPartFogDesc{};
    const LLPartData& pd = mPartSysData.mPartData;
    out.source_id = mID;
    out.flags = pd.mFlags;
    // (as the billboard path below: the blend funcs mean something only with the blend flag;
    //  the 0,0 default maps to ONE,ONE and read every ordinary system as additive)
    out.blend_pair = pd.hasBlendFunc() ? LLCleanParticleStore::blendPair(pd.mBlendFuncSource, pd.mBlendFuncDest)
                                       : LLCleanParticleStore::BLEND_PAIR_ALPHA;
    out.burst_count = mPartSysData.mBurstPartCount;
    out.burst_rate = mPartSysData.mBurstRate;
    out.part_age = pd.mMaxAge;
    out.speed_min = mPartSysData.mBurstSpeedMin;
    out.speed_max = mPartSysData.mBurstSpeedMax;
    out.burst_radius = mPartSysData.mBurstRadius;
    out.start_scale[0] = pd.mStartScale.mV[0]; out.start_scale[1] = pd.mStartScale.mV[1];
    out.end_scale[0] = pd.mEndScale.mV[0];     out.end_scale[1] = pd.mEndScale.mV[1];
    for (S32 k = 0; k < 4; ++k)
    {
        out.start_color[k] = pd.mStartColor.mV[k];
        out.end_color[k] = pd.mEndColor.mV[k];
    }
    out.glow[0] = pd.mStartGlow; out.glow[1] = pd.mEndGlow;
    out.accel[0] = mPartSysData.mPartAccel.mV[0];
    out.accel[1] = mPartSysData.mPartAccel.mV[1];
    out.accel[2] = mPartSysData.mPartAccel.mV[2];
    out.pos[0] = mPosAgent.mV[0]; out.pos[1] = mPosAgent.mV[1]; out.pos[2] = mPosAgent.mV[2];
    out.hud = mSourceObjectp.notNull() && mSourceObjectp->isHUDAttachment();
    out.attachment = !out.hud && mSourceObjectp.notNull() && mSourceObjectp->isAttachment();
    out.texture = mImagep.notNull() ? mImagep->getID() : LLUUID::null;
    // FNV-1a over the inputs (every field but pos).
    U32 h = 2166136261u;
    auto mix = [&h](const void* p, size_t n)
    {
        const U8* b = static_cast<const U8*>(p);
        for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 16777619u; }
    };
    mix(&out.flags, sizeof(out.flags)); mix(&out.blend_pair, sizeof(out.blend_pair));
    mix(&out.burst_count, sizeof(out.burst_count)); mix(&out.burst_rate, sizeof(out.burst_rate));
    mix(&out.part_age, sizeof(out.part_age)); mix(&out.speed_min, sizeof(out.speed_min));
    mix(&out.speed_max, sizeof(out.speed_max)); mix(&out.burst_radius, sizeof(out.burst_radius));
    mix(out.start_scale, sizeof(out.start_scale)); mix(out.end_scale, sizeof(out.end_scale));
    mix(out.start_color, sizeof(out.start_color)); mix(out.end_color, sizeof(out.end_color));
    mix(out.glow, sizeof(out.glow)); mix(out.accel, sizeof(out.accel));
    const U8 att = (out.attachment ? 1u : 0u) | (out.hud ? 2u : 0u);
    mix(&att, 1);
    mix(out.texture.mData, sizeof(out.texture.mData));
    out.hash = h;
    out.valid = true;
    return true;
}

bool LLViewerPartSourceScript::buildGPUEmitter(F32 dt, CleanEmitter& out, LLUUID& out_img)
{
    // Same source bookkeeping as update() -- position/target refresh,
    // source-age death, emission-axis rotation, burst timing -- but
    // instead of creating CPU LLViewerParts we count how many particles
    // would spawn this frame and hand the GPU a descriptor. The GPU emit
    // compute does all per-particle placement + physics.
    if (mIsSuspended) return false;

    // Owner not in view (sim-owned / absent owner): retry the lookup ~once a second,
    // not every frame per emitter.
    if (mOwnerAvatarp.isNull() && mOwnerUUID != LLUUID::null && LLFrameTimer::getFrameCount() >= mOwnerRetryFrame)
    {
        mOwnerAvatarp = find_avatar(mOwnerUUID);
        if (mOwnerAvatarp.isNull()) mOwnerRetryFrame = LLFrameTimer::getFrameCount() + 60u + (mID & 31u);
    }
    if (mOwnerAvatarp.notNull() && LLVOAvatar::AOA_NORMAL != mOwnerAvatarp->getOverallAppearance())
    {
        return false;
    }

    const F32 old_update_time = mLastUpdateTime;
    mLastUpdateTime += dt;
    F32 dt_update = mLastUpdateTime - mLastPartTime;

    // Follow the source object. A worn attachment on a GPU-animated avatar takes its
    // attachment point's live transform (vk_live_attachment_xform, #693); everything else
    // LL's render position / rotation.
    static LLCachedControl<bool> s_audit(gSavedSettings, "RenderVKParticleAudit", false);
    LLQuaternion src_rot;           // source render rotation this frame (identity: none)
    bool live_src = false;
    F32 ll_lag = 0.f;               // (audit) |LL render position - live position|
    if (!mSourceObjectp.isNull())
    {
        if (mSourceObjectp->isDead())
        {
            mSourceObjectp = NULL;
        }
        else if (mSourceObjectp->mDrawable.notNull())
        {
            live_src = vk_live_attachment_xform(mSourceObjectp, mPosAgent, src_rot);
            if (!live_src)
            {
                mPosAgent = mSourceObjectp->getRenderPosition();
            }
            else if (s_audit)
            {
                ll_lag = (mSourceObjectp->getRenderPosition() - mPosAgent).magVec();
            }
        }
        if (!live_src && mSourceObjectp.notNull())
        {
            src_rot = mSourceObjectp->getRenderRotation();
        }
    }

    // Resolve / follow the target object.
    if (mTargetObjectp.isNull() && mPartSysData.mTargetUUID.notNull())
    {
        setTargetObject(gObjectList.findObject(mPartSysData.mTargetUUID));
    }
    if (!mTargetObjectp.isNull())
    {
        if (mTargetObjectp->isDead())
        {
            mTargetObjectp = NULL;
        }
        else if (mTargetObjectp->mDrawable.notNull())
        {
            LLQuaternion tgt_rot;
            if (!vk_live_attachment_xform(mTargetObjectp, mTargetPosAgent, tgt_rot))
            {
                mTargetPosAgent = mTargetObjectp->getRenderPosition();
            }
        }
    }
    if (!mTargetObjectp)
    {
        mTargetPosAgent = mPosAgent;
    }

    // Source outlived its max age -> retire it.
    if (mPartSysData.mMaxAge &&
        ((mPartSysData.mStartAge + mLastUpdateTime + dt_update) > mPartSysData.mMaxAge))
    {
        setDead();
        return false;
    }

    // Count bursts this frame, advancing the emission-axis rotation by
    // angular velocity exactly as update() does (so PATTERN_ANGLE aims
    // the same way the legacy path did).
    bool first_run = (old_update_time <= 0.f);
    const F32 max_time = llmax(1.f, 10.f * mPartSysData.mBurstRate);
    dt_update = llmin(max_time, dt_update);

    // LL's spawn throttles (update(): particle limit, the tiny-or-far size / distance /
    // max-age throttle, the start delay, the adaptive burst rate), applied to the
    // burst's COUNT at once instead of one coin flip per particle.
    const F32 ref_rate_travelspeed = llmin(LLViewerPartSim::getInstance()->getRefRate(), 1.f);
    const F32 burst_keep = LLViewerPartSim::getInstance()->getBurstRate();

    U32 spawn = 0;
    U32 bursts = 0;   // (RenderVKParticleDiag)
    while ((dt_update > mPartSysData.mBurstRate) || first_run)
    {
        first_run = false;

        const F32 av_mag = mPartSysData.mAngularVelocity.magVec();
        if (av_mag != 0.f)
        {
            LLQuaternion dquat(dt * av_mag, mPartSysData.mAngularVelocity);
            mRotation *= dquat;
        }
        else
        {
            mRotation.setQuat(0, 0, 0);
        }

        if (LLViewerPartSim::gpuAboveParticleLimit())
        {
            // Above the particle limit: give up on this frame's bursts (LL update()).
            mLastPartTime = mLastUpdateTime;
            break;
        }

        // Greatest length the shortest side of a particle is expected to have.
        const LLPartData& lpd = mPartSysData.mPartData;
        const F32 max_short_side =
            llmax(llmax(llmin(lpd.mStartScale[0], lpd.mStartScale[1]),
                        llmin(lpd.mEndScale[0], lpd.mEndScale[1])),
                  llmin((lpd.mStartScale[0] + lpd.mEndScale[0]) / 2,
                        (lpd.mStartScale[1] + lpd.mEndScale[1]) / 2));
        // Maximum distance at which spawned particles will be viewable.
        const F32 max_dist = max_short_side * LLViewerCamera::getInstance()->getPixelMeterRatio();
        if (max_dist < 0.25f)
        {
            // < 1 pixel wide at >= 25 cm: useless, mostly spawned by buggy sources.
            mLastPartTime = mLastUpdateTime;
            break;
        }
        const F32 dist = (mPosAgent - LLViewerCamera::getInstance()->getOrigin()).magVec();
        F32 limited_rate = 0.f;
        if (dist - max_dist > 0.f)
        {
            if ((dist - max_dist) * ref_rate_travelspeed > lpd.mMaxAge - 0.2f)
            {
                // Would need to travel faster than 1 / ref rate m/s toward them to see
                // them for at least 0.2 s.
                mLastPartTime = mLastUpdateTime;
                break;
            }
            limited_rate = ((dist - max_dist) * ref_rate_travelspeed) / lpd.mMaxAge;
        }
        if (mDelay)
        {
            limited_rate = llmax(limited_rate, 0.01f * mDelay--);
        }
        // LL skips a particle when frand < max(1 - burst_rate, limited_rate).
        const F32 keep = 1.f - llmax(1.f - burst_keep, limited_rate);
        spawn += LLViewerPartSim::thinSpawn(mPartSysData.mBurstPartCount, keep);
        ++bursts;
        mLastPartTime = mLastUpdateTime;

        if (mPartSysData.mBurstRate <= 0.f) break; // 0 rate would loop forever
        dt_update -= mPartSysData.mBurstRate;
    }

    // HUD attachments: LL draws their particles in HUD space (LL_PART_HUD). There is no
    // GPU HUD particle pass, and the world pass spawned them at the HUD coordinates taken
    // as agent space (near the region corner): emit none (#693).
    const bool hud_src = mSourceObjectp.notNull() && mSourceObjectp->isHUDAttachment();
    if (hud_src) spawn = 0;

    if (s_audit)
    {
        LLCleanParticleStore::AuditSrc& a = LLCleanParticleStore::instance().auditSource(mID);
        a.object = mSourceObjectp.notNull() ? mSourceObjectp->getID() : LLUUID::null;
        a.pattern = mPartSysData.mPattern;
        a.flags = mPartSysData.mPartData.mFlags;
        a.count = mPartSysData.mBurstPartCount;
        a.rate = mPartSysData.mBurstRate;
        a.part_age = mPartSysData.mPartData.mMaxAge;
        a.src_max_age = mPartSysData.mMaxAge;
        a.attachment = mSourceObjectp.notNull() && mSourceObjectp->isAttachment();
        a.hud = hud_src;
        a.live_xform = live_src;
        a.bursts += bursts;
        a.requested += (U64)bursts * mPartSysData.mBurstPartCount;
        a.kept += spawn;
        a.dt_sum += dt;
        ++a.frames;
        if (a.have_last) a.jump_max = llmax(a.jump_max, (mPosAgent - a.last_pos).magVec());
        a.last_pos = mPosAgent;
        a.have_last = true;
        a.lag_max = llmax(a.lag_max, ll_lag);
    }

    // Always fill the descriptor (even when spawn == 0) so the stable
    // emitter slot keeps a live source/target position for following
    // particles; the emit pass simply skips slots whose spawn is 0.

    // ---- Fill the GPU descriptor ----
    out = CleanEmitter{};
    const LLPartData& pd = mPartSysData.mPartData;

    out.pos[0] = mPosAgent.mV[0];
    out.pos[1] = mPosAgent.mV[1];
    out.pos[2] = mPosAgent.mV[2];
    // LL zeroes the burst radius for following / target-linear particles (SVC-193,
    // VWR-717: see update()), so they spawn exactly at the source. The GPU emitter
    // must do the same or they scatter around the prim at the burst radius.
    out.pos[3] = (mPartSysData.mPartData.mFlags & (LLPartData::LL_PART_FOLLOW_SRC_MASK | LLPartData::LL_PART_TARGET_LINEAR_MASK))
               ? 0.f : mPartSysData.mBurstRadius;

    // Emission rotation = source render rotation applied first, then the
    // angular-velocity rotation (matches update(): dir*srcRot*mRotation).
    LLQuaternion q = mRotation;
    if (mSourceObjectp.notNull())
    {
        q = src_rot * mRotation;
    }
    out.rot[0] = q.mQ[0]; out.rot[1] = q.mQ[1];
    out.rot[2] = q.mQ[2]; out.rot[3] = q.mQ[3];
    // Ribbon width axis: LL uses the source's render rotation ONLY (update() /
    // LLVOPartGroup::getGeometry) -- never the PSYS_SRC_OMEGA emission spin, which
    // would tumble a flat ribbon (tire tracks) upright in bands.
    const LLQuaternion src_q = mSourceObjectp.notNull() ? src_rot : LLQuaternion();
    out.src_rot[0] = src_q.mQ[0]; out.src_rot[1] = src_q.mQ[1];
    out.src_rot[2] = src_q.mQ[2]; out.src_rot[3] = src_q.mQ[3];

    out.accel[0] = mPartSysData.mPartAccel.mV[0];
    out.accel[1] = mPartSysData.mPartAccel.mV[1];
    out.accel[2] = mPartSysData.mPartAccel.mV[2];
    out.accel[3] = pd.mMaxAge; // per-particle max age

    for (S32 k = 0; k < 4; ++k)
    {
        out.start_color[k] = pd.mStartColor.mV[k];
        out.end_color[k]   = pd.mEndColor.mV[k];
    }
    out.scale[0] = pd.mStartScale.mV[0];
    out.scale[1] = pd.mStartScale.mV[1];
    out.scale[2] = pd.mEndScale.mV[0];
    out.scale[3] = pd.mEndScale.mV[1];

    out.glow[0] = pd.mStartGlow;
    out.glow[1] = pd.mEndGlow;

    out.speed_angle[0] = mPartSysData.mBurstSpeedMin;
    out.speed_angle[1] = mPartSysData.mBurstSpeedMax;
    out.speed_angle[2] = mPartSysData.mInnerAngle;
    out.speed_angle[3] = mPartSysData.mOuterAngle;

    out.target[0] = mTargetPosAgent.mV[0];
    out.target[1] = mTargetPosAgent.mV[1];
    out.target[2] = mTargetPosAgent.mV[2];
    out.target[3] = mPosAgent.mV[2]; // source Z -> particle bounce plane

    out.misc[0] = mPartSysData.mPattern;
    // bit 30 (free in LLPartData flags): the source uses the DEPRECATED angle
    // parameters (no LL_PART_USE_NEW_ANGLE), which LL rotates once more about X.
    out.misc[1] = pd.mFlags | ((mPartSysData.mFlags & LLPartSysData::LL_PART_USE_NEW_ANGLE) ? 0u : (1u << 30));
    out.misc[2] = spawn;
    out.misc[3] = mID; // rng seed base (per-source, stable); shader mixes in slot + frame

    // RenderVKParticleDiag (#190 leashes): targeted sources, every ~2 s each -- whether
    // the target resolved, where source / target are, and how far the source sits from
    // its owner avatar (a stale attachment position would show there).
    // Ribbon sources (tire tracks lifting in bands vs FS): every ~0.5 s -- the source
    // prim's link role, its render vs object rotation, the width axis LL derives from
    // it, angular velocity and the emission spin.
    {
        static LLCachedControl<bool> s_pdiag(gSavedSettings, "RenderVKParticleDiag", false);
        // Every script source, every ~2 s each (#693, "burst count not respected"): the
        // bursts and particles it asked for per second (before the global shouldAddPart
        // thinning in LLViewerPartSim) against LL's nominal count / rate, its LSL
        // parameters, and how many sources its object has had (re-created sources burst
        // at once). spawned/s ~ count / rate means the emission is LL's; a source count
        // climbing means the object keeps getting new sources.
        if (s_pdiag)
        {
            struct RateDiag { F64 since = 0.0, next = 0.0; U32 bursts = 0, spawned = 0; };
            static std::map<U32, RateDiag> s_rate;
            const F64 now = LLFrameTimer::getTotalSeconds();
            RateDiag& rd = s_rate[mID];
            if (rd.since == 0.0) { rd.since = now; rd.next = now + 2.0; }
            rd.bursts += bursts;
            rd.spawned += spawn;
            if (now >= rd.next)
            {
                const F64 el = llmax(now - rd.since, 1e-3);
                LLViewerObject* src = mSourceObjectp.get();
                const auto& objs = diagObjectSources();
                const auto oit = src ? objs.find(src->getID()) : objs.end();
                F32 own_d = -1.f;
                if (mOwnerAvatarp.notNull()) own_d = (mOwnerAvatarp->getRenderPosition() - mPosAgent).magVec();
                LL_INFOS("VKParticleDiag") << "rate src#" << mID << " obj=" << (src ? src->getID() : LLUUID::null)
                    << (src && src->isAttachment() ? " (attachment)" : "")
                    << " sources_for_obj=" << (oit != objs.end() ? oit->second : 0u)
                    << " count=" << (U32)mPartSysData.mBurstPartCount << " burst_rate=" << mPartSysData.mBurstRate
                    << " nominal_parts/s=" << (mPartSysData.mBurstRate > 0.f ? mPartSysData.mBurstPartCount / mPartSysData.mBurstRate : 0.f)
                    << " bursts/s=" << rd.bursts / el << " parts/s=" << rd.spawned / el
                    << " burst_keep=" << burst_keep
                    << " part_max_age=" << pd.mMaxAge << " src_max_age=" << mPartSysData.mMaxAge
                    << " src_start_age=" << mPartSysData.mStartAge << " src_age=" << mLastUpdateTime
                    << " flags=0x" << std::hex << pd.mFlags << " sysflags=0x" << mPartSysData.mFlags << std::dec
                    << " pattern=" << (U32)mPartSysData.mPattern << " radius=" << mPartSysData.mBurstRadius
                    << " speed=" << mPartSysData.mBurstSpeedMin << ".." << mPartSysData.mBurstSpeedMax
                    << " src_pos=" << mPosAgent << " src_owner_dist=" << own_d
                    << " emitter_slot=" << LLCleanParticleStore::instance().getOrCreateEmitter(mID) << LL_ENDL;
                rd.since = now; rd.next = now + 2.0; rd.bursts = 0; rd.spawned = 0;
            }
        }
        if (s_pdiag && (pd.mFlags & LLPartData::LL_PART_RIBBON_MASK))
        {
            static std::map<U32, F64> s_rnext;
            const F64 now = LLFrameTimer::getTotalSeconds();
            F64& nx = s_rnext[mID];
            if (now >= nx)
            {
                nx = now + 0.5;
                LLViewerObject* src = mSourceObjectp.get();
                LLVector3 axis(0.f, 0.f, 1.f);
                if (src) axis *= src->getRenderRotation();
                LLViewerObject* root = src ? src->getRootEdit() : nullptr;
                LL_INFOS("VKParticleDiag") << "ribbon src#" << mID << " obj=" << (src ? src->getID() : LLUUID::null)
                    << " role=" << (!src ? "none" : (src->isAttachment() ? "attachment" : (src == root ? "root" : "child")))
                    << " root_phys=" << (root && root->flagUsePhysics() ? 1 : 0)
                    << " flags=0x" << std::hex << pd.mFlags << " sysflags=0x" << mPartSysData.mFlags << std::dec
                    << " pattern=" << (U32)mPartSysData.mPattern
                    << " src_pos=" << mPosAgent
                    << " render_rot=" << (src ? src->getRenderRotation() : LLQuaternion())
                    << " obj_rot=" << (src ? src->getRotationRegion() : LLQuaternion())
                    << " width_axis=" << axis
                    << " ang_vel=" << (src ? src->getAngularVelocity() : LLVector3::zero)
                    << " root_ang_vel=" << (root ? root->getAngularVelocity() : LLVector3::zero)
                    << " src_omega=" << mPartSysData.mAngularVelocity
                    << " accel=" << mPartSysData.mPartAccel << " max_age=" << pd.mMaxAge
                    << " burst_rate=" << mPartSysData.mBurstRate << " count=" << (U32)mPartSysData.mBurstPartCount
                    << " speed=" << mPartSysData.mBurstSpeedMin << ".." << mPartSysData.mBurstSpeedMax
                    << " radius=" << mPartSysData.mBurstRadius << " spawn_now=" << spawn
                    << " scale=" << pd.mStartScale << "->" << pd.mEndScale
                    << " image=" << mPartSysData.mPartImageID
                    << " emitter_slot=" << LLCleanParticleStore::instance().getOrCreateEmitter(mID) << LL_ENDL;
            }
        }
        if (s_pdiag && (mPartSysData.mTargetUUID.notNull()
                        || (pd.mFlags & (LLPartData::LL_PART_TARGET_POS_MASK | LLPartData::LL_PART_TARGET_LINEAR_MASK))))
        {
            static std::map<U32, F64> s_next;
            const F64 now = LLFrameTimer::getTotalSeconds();
            F64& nx = s_next[mID];
            if (now >= nx)
            {
                nx = now + 2.0;
                LLViewerObject* tgt = mTargetObjectp.get();
                LLViewerObject* src = mSourceObjectp.get();
                F32 own_d = -1.f;
                if (mOwnerAvatarp.notNull()) own_d = (mOwnerAvatarp->getRenderPosition() - mPosAgent).magVec();
                LL_INFOS("VKParticleDiag") << "src#" << mID << " obj=" << (src ? src->getID() : LLUUID::null)
                    << (src && src->isAttachment() ? " (attachment)" : "")
                    << " flags=0x" << std::hex << pd.mFlags << " sysflags=0x" << mPartSysData.mFlags << std::dec
                    << " pattern=" << (U32)mPartSysData.mPattern
                    << " target_key=" << mPartSysData.mTargetUUID
                    << " resolved=" << (tgt ? (tgt->isAvatar() ? "avatar" : (tgt->isAttachment() ? "attachment" : "object")) : "NO")
                    << " tgt_drawable=" << (tgt && tgt->mDrawable.notNull() ? 1 : 0)
                    << " src_pos=" << mPosAgent << " tgt_pos=" << mTargetPosAgent
                    << " src_tgt_dist=" << (mTargetPosAgent - mPosAgent).magVec()
                    << " src_owner_dist=" << own_d
                    << " accel=" << mPartSysData.mPartAccel << " max_age=" << pd.mMaxAge
                    << " burst_rate=" << mPartSysData.mBurstRate << " count=" << (U32)mPartSysData.mBurstPartCount
                    << " speed=" << mPartSysData.mBurstSpeedMin << ".." << mPartSysData.mBurstSpeedMax
                    << " radius=" << mPartSysData.mBurstRadius << " spawn_now=" << spawn
                    << " scale=" << pd.mStartScale << "->" << pd.mEndScale
                    << " color=" << pd.mStartColor << "->" << pd.mEndColor
                    << " image=" << mPartSysData.mPartImageID
                    << " img_loaded=" << (mImagep.notNull() ? mImagep->getDiscardLevel() : -2)
                    << " blend=" << (U32)pd.mBlendFuncSource << "/" << (U32)pd.mBlendFuncDest
                    << " emitter_slot=" << LLCleanParticleStore::instance().getOrCreateEmitter(mID) << LL_ENDL;
            }
        }
    }

    // The particle texture (the renderer resolves it to its bindless index).
    LLUUID img = mPartSysData.mPartImageID;
    if (img.isNull() && mImagep.notNull()) img = mImagep->getID();
    // The GPU path bypasses the legacy face setup (facep->setTexture)
    // that requests the particle texture, so without an explicit request
    // the texture never fetches -- it stays a magenta placeholder. Boost
    // hard so it loads immediately (particles are short-lived and a
    // placeholder ruins the whole effect). The draw also samples the
    // texture from the emitter's live handle, so once it loads, every
    // already-alive particle updates instead of keeping the placeholder.
    if (mImagep.notNull())
    {
        mImagep->setBoostLevel(LLGLTexture::BOOST_HIGH);
        mImagep->addTextureStats(2048.f * 2048.f);
    }
    // Vulkan resolves the texture from out_img (the store overwrites tex_handle).
    out.tex_handle[0] = out.tex_handle[1] = 0u;
    // Hand the texture id back so the store keeps the handle fresh past
    // this source's death (its particles outlive it and read the live slot).
    out_img = img;

    // mBlendFuncSource/Dest are only meaningful when the blend flag is
    // set; otherwise the particle uses normal alpha blend. Reading them
    // unconditionally routed default particles to an additive bucket
    // (the 0,0 default maps to ONE,ONE) -- making them glow and look
    // like their alpha was broken.
    out.blend[0] = pd.hasBlendFunc()
                       ? LLCleanParticleStore::blendPair(pd.mBlendFuncSource, pd.mBlendFuncDest)
                       : LLCleanParticleStore::BLEND_PAIR_ALPHA; // SRC_ALPHA, ONE_MINUS_SRC_ALPHA
    out.blend[1] = LLCleanParticleStore::MOTION_NORMAL;

    // Region wind for WIND-flagged particles: one sample at the source
    // (the wind grid is coarse, so per-source is representative). The GPU
    // sim drags WIND particles' velocity toward this each frame.
    out.wind[0] = out.wind[1] = out.wind[2] = out.wind[3] = 0.f;
    if (pd.mFlags & LLPartData::LL_PART_WIND_MASK)
    {
        LLViewerRegion* regionp = mSourceObjectp.notNull() ? mSourceObjectp->getRegion()
                                                           : gAgent.getRegion();
        if (regionp)
        {
            LLVector3 w = regionp->mWind.getVelocity(regionp->getPosRegionFromAgent(mPosAgent));
            out.wind[0] = w.mV[0];
            out.wind[1] = w.mV[1];
            out.wind[2] = w.mV[2];
        }
    }

    return true;
}

// static
LLPointer<LLViewerPartSourceScript> LLViewerPartSourceScript::unpackPSS(LLViewerObject *source_objp, LLPointer<LLViewerPartSourceScript> pssp, const S32 block_num)
{
    if (!pssp)
    {
        if (LLPartSysData::isNullPS(block_num))
        {
            return NULL;
        }
        LLPointer<LLViewerPartSourceScript> new_pssp = new LLViewerPartSourceScript(source_objp);
        if (!new_pssp->mPartSysData.unpackBlock(block_num))
        {
            return NULL;
        }
        if (new_pssp->mPartSysData.mTargetUUID.notNull())
        {
            LLViewerObject *target_objp = gObjectList.findObject(new_pssp->mPartSysData.mTargetUUID);
            new_pssp->setTargetObject(target_objp);
        }
        return new_pssp;
    }
    else
    {
        if (LLPartSysData::isNullPS(block_num))
        {
            return NULL;
        }

        F32 prev_max_age = pssp->mPartSysData.mMaxAge;
        F32 prev_start_age = pssp->mPartSysData.mStartAge;
        if (!pssp->mPartSysData.unpackBlock(block_num))
        {
            return NULL;
        }
        else if (pssp->mPartSysData.mMaxAge
                 && (prev_max_age != pssp->mPartSysData.mMaxAge || prev_start_age != pssp->mPartSysData.mStartAge))
        {
            // reusing existing pss, so reset time to allow particles to start again
            pssp->mLastUpdateTime = 0.f;
            pssp->mLastPartTime = 0.f;
        }

        if (pssp->mPartSysData.mTargetUUID.notNull())
        {
            LLViewerObject *target_objp = gObjectList.findObject(pssp->mPartSysData.mTargetUUID);
            pssp->setTargetObject(target_objp);
        }
        return pssp;
    }
}


LLPointer<LLViewerPartSourceScript> LLViewerPartSourceScript::unpackPSS(LLViewerObject *source_objp, LLPointer<LLViewerPartSourceScript> pssp, LLDataPacker &dp, bool legacy)
{
    if (!pssp)
    {
        LLPointer<LLViewerPartSourceScript> new_pssp = new LLViewerPartSourceScript(source_objp);
        if (legacy)
        {
            if (!new_pssp->mPartSysData.unpackLegacy(dp))
            {
                return NULL;
            }
        }
        else
        {
            if (!new_pssp->mPartSysData.unpack(dp))
            {
                return NULL;
            }
        }

        if (new_pssp->mPartSysData.mTargetUUID.notNull())
        {
            LLViewerObject *target_objp = gObjectList.findObject(new_pssp->mPartSysData.mTargetUUID);
            new_pssp->setTargetObject(target_objp);
        }

        return new_pssp;
    }
    else
    {
        if (legacy)
        {
            if (!pssp->mPartSysData.unpackLegacy(dp))
            {
                return NULL;
            }
        }
        else
        {
            if (!pssp->mPartSysData.unpack(dp))
            {
                return NULL;
            }
        }

        if (pssp->mPartSysData.mTargetUUID.notNull())
        {
            LLViewerObject *target_objp = gObjectList.findObject(pssp->mPartSysData.mTargetUUID);
            pssp->setTargetObject(target_objp);
        }
        return pssp;
    }
}


/* static */
LLPointer<LLViewerPartSourceScript> LLViewerPartSourceScript::createPSS(LLViewerObject *source_objp, const LLPartSysData& particle_parameters)
{
    LLPointer<LLViewerPartSourceScript> new_pssp = new LLViewerPartSourceScript(source_objp);

    new_pssp->mPartSysData = particle_parameters;

    if (new_pssp->mPartSysData.mTargetUUID.notNull())
    {
        LLViewerObject *target_objp = gObjectList.findObject(new_pssp->mPartSysData.mTargetUUID);
        new_pssp->setTargetObject(target_objp);
    }
    return new_pssp;
}


void LLViewerPartSourceScript::setImage(LLViewerTexture *imagep)
{
    mImagep = imagep;
}

void LLViewerPartSourceScript::setTargetObject(LLViewerObject *objp)
{
    mTargetObjectp = objp;
}





LLViewerPartSourceSpiral::LLViewerPartSourceSpiral(const LLVector3 &pos) :
    LLViewerPartSource(LL_PART_SOURCE_CHAT)
{
    mPosAgent = pos;
}

void LLViewerPartSourceSpiral::setDead()
{
    mIsDead = true;
    mSourceObjectp = NULL;
}


void LLViewerPartSourceSpiral::updatePart(LLViewerPart &part, const F32 dt)
{
    F32 frac = part.mLastUpdateTime/part.mMaxAge;

    LLVector3 center_pos;
    LLPointer<LLViewerPartSource>& ps = part.mPartSourcep;
    LLViewerPartSourceSpiral *pss = (LLViewerPartSourceSpiral *)ps.get();
    if (!pss->mSourceObjectp.isNull() && !pss->mSourceObjectp->mDrawable.isNull())
    {
        part.mPosAgent = pss->mSourceObjectp->getRenderPosition();
    }
    else
    {
        part.mPosAgent = pss->mPosAgent;
    }
    F32 x = sin(F_TWO_PI*frac + part.mParameter);
    F32 y = cos(F_TWO_PI*frac + part.mParameter);

    part.mPosAgent.mV[VX] += x;
    part.mPosAgent.mV[VY] += y;
    part.mPosAgent.mV[VZ] += -0.5f + frac;
}


void LLViewerPartSourceSpiral::update(const F32 dt)
{
    if (!mImagep)
    {
        mImagep = LLViewerFetchedTexture::sDefaultParticleImagep;
    }

    const F32 RATE = 0.025f;

    mLastUpdateTime += dt;

    F32 dt_update = mLastUpdateTime - mLastPartTime;
    F32 max_time = llmax(1.f, 10.f*RATE);
    dt_update = llmin(max_time, dt_update);

    if (dt_update > RATE)
    {
        mLastPartTime = mLastUpdateTime;
        if (!LLViewerPartSim::getInstance()->shouldAddPart())
        {
            // Particle simulation says we have too many particles, skip all this
            return;
        }

        if (!mSourceObjectp.isNull() && !mSourceObjectp->mDrawable.isNull())
        {
            mPosAgent = mSourceObjectp->getRenderPosition();
        }
        LLViewerPart* part = new LLViewerPart();
        part->init(this, mImagep, updatePart);
        part->mStartColor = mColor;
        part->mEndColor = mColor;
        part->mEndColor.mV[3] = 0.f;
        part->mPosAgent = mPosAgent;
        part->mMaxAge = 1.f;
        part->mFlags = LLViewerPart::LL_PART_INTERP_COLOR_MASK;
        part->mLastUpdateTime = 0.f;
        part->mScale.mV[0] = 0.25f;
        part->mScale.mV[1] = 0.25f;
        part->mParameter = ll_frand(F_TWO_PI);
        part->mBlendFuncDest = LLRender::BF_ONE_MINUS_SOURCE_ALPHA;
        part->mBlendFuncSource = LLRender::BF_SOURCE_ALPHA;
        part->mStartGlow = 0.f;
        part->mEndGlow = 0.f;
        part->mGlow = LLColor4U(0, 0, 0, 0);

        LLViewerPartSim::getInstance()->addPart(part);
    }
}

void LLViewerPartSourceSpiral::setSourceObject(LLViewerObject *objp)
{
    mSourceObjectp = objp;
}

void LLViewerPartSourceSpiral::setColor(const LLColor4 &color)
{
    mColor = color;
}

// Shared spiral/chat emitter descriptor: DROP spawn at the source, then the
// GPU sim orbits each particle around the (live) source on a unit circle,
// rising over its 1s life, with the colour fading to transparent. The
// motion is driven by MOTION_SPIRAL, not the burst pattern.
static void buildSpiralCleanEmitter(const LLVector3& pos, const LLColor4& color,
                                    U32 spawn, U32 src_id, CleanEmitter& out)
{
    out = CleanEmitter{};
    out.pos[0] = pos.mV[0]; out.pos[1] = pos.mV[1]; out.pos[2] = pos.mV[2]; out.pos[3] = 0.f;
    out.target[0] = pos.mV[0]; out.target[1] = pos.mV[1];
    out.target[2] = pos.mV[2]; out.target[3] = pos.mV[2];
    out.accel[3] = 1.0f; // per-particle max age (seconds)
    for (S32 k = 0; k < 4; ++k) { out.start_color[k] = color.mV[k]; out.end_color[k] = color.mV[k]; }
    out.end_color[3] = 0.f; // fade to transparent
    out.scale[0] = out.scale[1] = out.scale[2] = out.scale[3] = 0.25f;
    out.misc[0] = LLPartSysData::LL_PART_SRC_PATTERN_DROP;
    out.misc[1] = LLPartData::LL_PART_INTERP_COLOR_MASK;
    out.misc[2] = spawn;
    out.misc[3] = src_id;
    out.blend[0] = LLCleanParticleStore::BLEND_PAIR_ALPHA;
    out.blend[1] = LLCleanParticleStore::MOTION_SPIRAL;
}

// At most one particle per call, exactly as the CPU update() does (it sets
// mLastPartTime = mLastUpdateTime, consuming all accumulated time).
static U32 spawnOnePerRate(F32& last_update, F32& last_part, F32 rate)
{
    F32 dt_update = last_update - last_part;
    dt_update = llmin(llmax(1.f, 10.f * rate), dt_update);
    if (dt_update > rate) { last_part = last_update; return 1u; }
    return 0u;
}

bool LLViewerPartSourceSpiral::buildGPUEmitter(F32 dt, CleanEmitter& out, LLUUID& out_img)
{
    if (mIsSuspended) return false;
    if (!mImagep) mImagep = LLViewerFetchedTexture::sDefaultParticleImagep;

    mLastUpdateTime += dt;
    if (!mSourceObjectp.isNull() && !mSourceObjectp->mDrawable.isNull())
    {
        mPosAgent = mSourceObjectp->getRenderPosition();
    }

    const U32 spawn = spawnOnePerRate(mLastUpdateTime, mLastPartTime, 0.025f);
    buildSpiralCleanEmitter(mPosAgent, mColor, spawn, mID, out);
    fillGPUEmitterTexture(mImagep, out, out_img);
    return true;
}





LLViewerPartSourceBeam::LLViewerPartSourceBeam() :
    LLViewerPartSource(LL_PART_SOURCE_BEAM)
{
}

LLViewerPartSourceBeam::~LLViewerPartSourceBeam()
{
}

void LLViewerPartSourceBeam::setDead()
{
    mIsDead = true;
    mSourceObjectp = NULL;
    mTargetObjectp = NULL;
}

void LLViewerPartSourceBeam::setColor(const LLColor4 &color)
{
    mColor = color;
}


void LLViewerPartSourceBeam::updatePart(LLViewerPart &part, const F32 dt)
{
    F32 frac = part.mLastUpdateTime/part.mMaxAge;

    LLViewerPartSource *ps = (LLViewerPartSource*)part.mPartSourcep;
    LLViewerPartSourceBeam *psb = (LLViewerPartSourceBeam *)ps;
    if (psb->mSourceObjectp.isNull())
    {
        part.mFlags = LLPartData::LL_PART_DEAD_MASK;
        return;
    }

    LLVector3 source_pos_agent;
    LLVector3 target_pos_agent;
    if (!psb->mSourceObjectp.isNull() && !psb->mSourceObjectp->mDrawable.isNull())
    {
        if (psb->mSourceObjectp->isAvatar())
        {
            LLViewerObject *objp = psb->mSourceObjectp;
            LLVOAvatar *avp = (LLVOAvatar *)objp;
            source_pos_agent = avp->mWristLeftp->getWorldPosition();
        }
        else
        {
            source_pos_agent = psb->mSourceObjectp->getRenderPosition();
        }
    }
    if (!psb->mTargetObjectp.isNull() && !psb->mTargetObjectp->mDrawable.isNull())
    {
        target_pos_agent = psb->mTargetObjectp->getRenderPosition();
    }

    part.mPosAgent = (1.f - frac) * source_pos_agent;
    if (psb->mTargetObjectp.isNull())
    {
        part.mPosAgent += frac * (gAgent.getPosAgentFromGlobal(psb->mLKGTargetPosGlobal));
    }
    else
    {
        part.mPosAgent += frac * target_pos_agent;
    }
}


void LLViewerPartSourceBeam::update(const F32 dt)
{
    const F32 RATE = 0.025f;

    mLastUpdateTime += dt;

    if (!mSourceObjectp.isNull() && !mSourceObjectp->mDrawable.isNull())
    {
        if (mSourceObjectp->isAvatar())
        {
            LLViewerObject *objp = mSourceObjectp;
            LLVOAvatar *avp = (LLVOAvatar *)objp;
            mPosAgent = avp->mWristLeftp->getWorldPosition();
        }
        else
        {
            mPosAgent = mSourceObjectp->getRenderPosition();
        }
    }

    if (!mTargetObjectp.isNull() && !mTargetObjectp->mDrawable.isNull())
    {
        mTargetPosAgent = mTargetObjectp->getRenderPosition();
    }
    else if (!mLKGTargetPosGlobal.isExactlyZero())
    {
        mTargetPosAgent = gAgent.getPosAgentFromGlobal(mLKGTargetPosGlobal);
    }

    F32 dt_update = mLastUpdateTime - mLastPartTime;
    F32 max_time = llmax(1.f, 10.f*RATE);
    dt_update = llmin(max_time, dt_update);

    if (dt_update > RATE)
    {
        mLastPartTime = mLastUpdateTime;
        if (!LLViewerPartSim::getInstance()->shouldAddPart())
        {
            // Particle simulation says we have too many particles, skip all this
            return;
        }

        if (!mImagep)
        {
            mImagep = LLViewerFetchedTexture::sDefaultParticleImagep;
        }

        LLViewerPart* part = new LLViewerPart();
        part->init(this, mImagep, updatePart);

        part->mFlags = LLPartData::LL_PART_INTERP_COLOR_MASK |
                        LLPartData::LL_PART_INTERP_SCALE_MASK |
                        LLPartData::LL_PART_TARGET_POS_MASK |
                        LLPartData::LL_PART_FOLLOW_VELOCITY_MASK;
        part->mMaxAge = 0.5f;
        part->mStartColor = mColor;
        part->mEndColor = part->mStartColor;
        part->mEndColor.mV[3] = 0.4f;
        part->mColor = part->mStartColor;

        part->mStartScale = LLVector2(0.1f, 0.1f);
        part->mEndScale = LLVector2(0.1f, 0.1f);
        part->mScale = part->mStartScale;

        part->mPosAgent = mPosAgent;
        part->mVelocity = mTargetPosAgent - mPosAgent;

        part->mBlendFuncDest = LLRender::BF_ONE_MINUS_SOURCE_ALPHA;
        part->mBlendFuncSource = LLRender::BF_SOURCE_ALPHA;
        part->mStartGlow = 0.f;
        part->mEndGlow = 0.f;
        part->mGlow = LLColor4U(0, 0, 0, 0);

        LLViewerPartSim::getInstance()->addPart(part);
    }
}

void LLViewerPartSourceBeam::setSourceObject(LLViewerObject* objp)
{
    mSourceObjectp = objp;
}

void LLViewerPartSourceBeam::setTargetObject(LLViewerObject* objp)
{
    mTargetObjectp = objp;
}

bool LLViewerPartSourceBeam::buildGPUEmitter(F32 dt, CleanEmitter& out, LLUUID& out_img)
{
    if (mIsSuspended) return false;
    if (mSourceObjectp.isNull()) { setDead(); return false; }
    if (!mImagep) mImagep = LLViewerFetchedTexture::sDefaultParticleImagep;

    mLastUpdateTime += dt;

    // Source: the avatar's left wrist for an avatar (pointing beam), else
    // the object's render position. Target: object or last-known global.
    if (!mSourceObjectp->mDrawable.isNull())
    {
        if (mSourceObjectp->isAvatar())
        {
            LLVOAvatar* avp = (LLVOAvatar*)mSourceObjectp.get();
            mPosAgent = avp->mWristLeftp->getWorldPosition();
        }
        else
        {
            mPosAgent = mSourceObjectp->getRenderPosition();
        }
    }
    if (!mTargetObjectp.isNull() && !mTargetObjectp->mDrawable.isNull())
    {
        mTargetPosAgent = mTargetObjectp->getRenderPosition();
    }
    else if (!mLKGTargetPosGlobal.isExactlyZero())
    {
        mTargetPosAgent = gAgent.getPosAgentFromGlobal(mLKGTargetPosGlobal);
    }

    const U32 spawn = spawnOnePerRate(mLastUpdateTime, mLastPartTime, 0.025f);

    out = CleanEmitter{};
    out.pos[0] = mPosAgent.mV[0]; out.pos[1] = mPosAgent.mV[1];
    out.pos[2] = mPosAgent.mV[2]; out.pos[3] = 0.f;
    out.target[0] = mTargetPosAgent.mV[0]; out.target[1] = mTargetPosAgent.mV[1];
    out.target[2] = mTargetPosAgent.mV[2]; out.target[3] = mPosAgent.mV[2];
    out.accel[3] = 0.5f; // per-particle max age (seconds)
    for (S32 k = 0; k < 4; ++k) { out.start_color[k] = mColor.mV[k]; out.end_color[k] = mColor.mV[k]; }
    out.end_color[3] = 0.4f; // fade toward (not to) transparent
    out.scale[0] = out.scale[1] = out.scale[2] = out.scale[3] = 0.1f;
    out.misc[0] = LLPartSysData::LL_PART_SRC_PATTERN_DROP;
    out.misc[1] = LLPartData::LL_PART_INTERP_COLOR_MASK |
                  LLPartData::LL_PART_INTERP_SCALE_MASK |
                  LLPartData::LL_PART_FOLLOW_VELOCITY_MASK;
    out.misc[2] = spawn;
    out.misc[3] = mID;
    out.blend[0] = LLCleanParticleStore::BLEND_PAIR_ALPHA;
    out.blend[1] = LLCleanParticleStore::MOTION_BEAM;
    fillGPUEmitterTexture(mImagep, out, out_img);
    return true;
}




LLViewerPartSourceChat::LLViewerPartSourceChat(const LLVector3 &pos) :
    LLViewerPartSource(LL_PART_SOURCE_SPIRAL)
{
    mPosAgent = pos;
}

void LLViewerPartSourceChat::setDead()
{
    mIsDead = true;
    mSourceObjectp = NULL;
}


void LLViewerPartSourceChat::updatePart(LLViewerPart &part, const F32 dt)
{
    F32 frac = part.mLastUpdateTime/part.mMaxAge;

    LLVector3 center_pos;
    LLViewerPartSource *ps = (LLViewerPartSource*)part.mPartSourcep;
    LLViewerPartSourceChat *pss = (LLViewerPartSourceChat *)ps;
    if (!pss->mSourceObjectp.isNull() && !pss->mSourceObjectp->mDrawable.isNull())
    {
        part.mPosAgent = pss->mSourceObjectp->getRenderPosition();
    }
    else
    {
        part.mPosAgent = pss->mPosAgent;
    }
    F32 x = sin(F_TWO_PI*frac + part.mParameter);
    F32 y = cos(F_TWO_PI*frac + part.mParameter);

    part.mPosAgent.mV[VX] += x;
    part.mPosAgent.mV[VY] += y;
    part.mPosAgent.mV[VZ] += -0.5f + frac;
}


void LLViewerPartSourceChat::update(const F32 dt)
{
    if (!mImagep)
    {
        mImagep = LLViewerFetchedTexture::sDefaultParticleImagep;
    }


    const F32 RATE = 0.025f;

    mLastUpdateTime += dt;

    if (mLastUpdateTime > 2.f)
    {
        // Kill particle source because it has outlived its max age...
        setDead();
        return;
    }

    F32 dt_update = mLastUpdateTime - mLastPartTime;

    // Clamp us to generating at most one second's worth of particles on a frame.
    F32 max_time = llmax(1.f, 10.f*RATE);
    dt_update = llmin(max_time, dt_update);

    if (dt_update > RATE)
    {
        mLastPartTime = mLastUpdateTime;
        if (!LLViewerPartSim::getInstance()->shouldAddPart())
        {
            // Particle simulation says we have too many particles, skip all this
            return;
        }

        if (!mSourceObjectp.isNull() && !mSourceObjectp->mDrawable.isNull())
        {
            mPosAgent = mSourceObjectp->getRenderPosition();
        }
        LLViewerPart* part = new LLViewerPart();
        part->init(this, mImagep, updatePart);
        part->mStartColor = mColor;
        part->mEndColor = mColor;
        part->mEndColor.mV[3] = 0.f;
        part->mPosAgent = mPosAgent;
        part->mMaxAge = 1.f;
        part->mFlags = LLViewerPart::LL_PART_INTERP_COLOR_MASK;
        part->mLastUpdateTime = 0.f;
        part->mScale.mV[0] = 0.25f;
        part->mScale.mV[1] = 0.25f;
        part->mParameter = ll_frand(F_TWO_PI);
        part->mBlendFuncDest = LLRender::BF_ONE_MINUS_SOURCE_ALPHA;
        part->mBlendFuncSource = LLRender::BF_SOURCE_ALPHA;
        part->mStartGlow = 0.f;
        part->mEndGlow = 0.f;
        part->mGlow = LLColor4U(0, 0, 0, 0);


        LLViewerPartSim::getInstance()->addPart(part);
    }
}

void LLViewerPartSourceChat::setSourceObject(LLViewerObject *objp)
{
    mSourceObjectp = objp;
}

void LLViewerPartSourceChat::setColor(const LLColor4 &color)
{
    mColor = color;
}

bool LLViewerPartSourceChat::buildGPUEmitter(F32 dt, CleanEmitter& out, LLUUID& out_img)
{
    if (mIsSuspended) return false;
    if (!mImagep) mImagep = LLViewerFetchedTexture::sDefaultParticleImagep;

    mLastUpdateTime += dt;
    if (mLastUpdateTime > 2.f) { setDead(); return false; } // source max age

    if (!mSourceObjectp.isNull() && !mSourceObjectp->mDrawable.isNull())
    {
        mPosAgent = mSourceObjectp->getRenderPosition();
    }

    const U32 spawn = spawnOnePerRate(mLastUpdateTime, mLastPartTime, 0.025f);
    buildSpiralCleanEmitter(mPosAgent, mColor, spawn, mID, out);
    fillGPUEmitterTexture(mImagep, out, out_img);
    return true;
}


