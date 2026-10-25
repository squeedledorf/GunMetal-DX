/**
 * @file llvopartgroup.cpp
 * @brief Group of particle systems
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

#include "llvopartgroup.h"


#include "llfasttimer.h"
#include "message.h"
#include "v2math.h"

#include "llagentcamera.h"
#include "lldrawable.h"
#include "llface.h"
#include "llsky.h"
#include "llviewercamera.h"
#include "llviewerpartsim.h"
#include "llviewerregion.h"
#include "pipeline.h"

extern U64MicrosecondsImplicit gFrameTime;

void LLVOPartGroup::initClass()
{
}

//static
void LLVOPartGroup::restoreGL()
{

}

//static
void LLVOPartGroup::destroyGL()
{
}

bool ll_is_part_idx_allocated(S32 idx, S32* start, S32* end)
{
    /*while (start < end)
    {
        if (*start == idx)
        { //not allocated (in free list)
            return false;
        }
        ++start;
    }*/

    //allocated (not in free list)
    return false;
}

LLVOPartGroup::LLVOPartGroup(const LLUUID &id, const LLPCode pcode, LLViewerRegion *regionp)
    :   LLAlphaObject(id, pcode, regionp),
        mViewerPartGroupp(NULL)
{
    setNumTEs(1);
    setTETexture(0, LLUUID::null);
    mbCanSelect = false;            // users can't select particle systems
}


LLVOPartGroup::~LLVOPartGroup()
{
}

bool LLVOPartGroup::isActive() const
{
    return false;
}

F32 LLVOPartGroup::getBinRadius()
{
    return mViewerPartGroupp->getBoxSide();
}

void LLVOPartGroup::updateSpatialExtents(LLVector4a& newMin, LLVector4a& newMax)
{
    const LLVector3& pos_agent = getPositionAgent();

    LLVector4a scale;
    LLVector4a p;

    p.load3(pos_agent.mV);

    scale.splat(mScale.mV[0]+mViewerPartGroupp->getBoxSide()*0.5f);

    newMin.setSub(p, scale);
    newMax.setAdd(p,scale);

    llassert(newMin.isFinite3());
    llassert(newMax.isFinite3());

    llassert(p.isFinite3());
    mDrawable->setPositionGroup(p);
}

void LLVOPartGroup::idleUpdate(LLAgent &agent, const F64 &time)
{
}

void LLVOPartGroup::setPixelAreaAndAngle(LLAgent &agent)
{
    // mPixelArea is calculated during render
    F32 mid_scale = getMidScale();
    F32 range = (getRenderPosition()-LLViewerCamera::getInstance()->getOrigin()).length();

    if (range < 0.001f || isHUDAttachment())        // range == zero
    {
        mAppAngle = 180.f;
    }
    else
    {
        mAppAngle = (F32) atan2( mid_scale, range) * RAD_TO_DEG;
    }
}

void LLVOPartGroup::updateTextures()
{
    // Texture stats for particles need to be updated in a different way...
}


LLDrawable* LLVOPartGroup::createDrawable(LLPipeline *pipeline)
{
    pipeline->allocDrawable(this);
    mDrawable->setLit(false);
    mDrawable->setRenderType(LLPipeline::RENDER_TYPE_PARTICLES);
    return mDrawable;
}

 const F32 MAX_PARTICLE_AREA_SCALE = 0.02f; // some tuned constant, limits on how much particle area to draw

 LLUUID LLVOPartGroup::getPartOwner(S32 idx)
 {
     LLUUID ret = LLUUID::null;

     if (idx < (S32) mViewerPartGroupp->mParticles.size())
     {
         ret = mViewerPartGroupp->mParticles[idx]->mPartSourcep->getOwnerUUID();
     }

     return ret;
 }

 LLUUID LLVOPartGroup::getPartSource(S32 idx)
 {
     LLUUID ret = LLUUID::null;

     if (idx < (S32) mViewerPartGroupp->mParticles.size())
     {
         LLViewerPart* part = mViewerPartGroupp->mParticles[idx];
         if (part && part->mPartSourcep.notNull() &&
             part->mPartSourcep->mSourceObjectp.notNull())
         {
             LLViewerObject* source = part->mPartSourcep->mSourceObjectp;
             ret = source->getID();
         }
     }

     return ret;
 }


F32 LLVOPartGroup::getPartSize(S32 idx)
{
    if (idx < (S32) mViewerPartGroupp->mParticles.size())
    {
        return mViewerPartGroupp->mParticles[idx]->mScale.mV[0];
    }

    return 0.f;
}

void LLVOPartGroup::getBlendFunc(S32 idx, LLRender::eBlendFactor& src, LLRender::eBlendFactor& dst)
{
    if (idx < (S32) mViewerPartGroupp->mParticles.size())
    {
        LLViewerPart* part = mViewerPartGroupp->mParticles[idx];
        src = (LLRender::eBlendFactor) part->mBlendFuncSource;
        dst = (LLRender::eBlendFactor) part->mBlendFuncDest;
    }
}


LLVector3 LLVOPartGroup::getCameraPosition() const
{
    return gAgentCamera.getCameraPositionAgent();
}

LLVector3 LLVOHUDPartGroup::getCameraPosition() const
{
    return LLVector3(-1,0,0);
}

void LLVOPartGroup::getGeometry(const LLViewerPart& part,
                                LLStrider<LLVector4a>& verticesp)
{
    if (part.mFlags & LLPartData::LL_PART_RIBBON_MASK)
    {
        LLVector4a axis, pos, paxis, ppos;
        F32 scale, pscale;

        pos.load3(part.mPosAgent.mV);
        axis.load3(part.mAxis.mV);
        scale = part.mScale.mV[0];

        if (part.mParent)
        {
            ppos.load3(part.mParent->mPosAgent.mV);
            paxis.load3(part.mParent->mAxis.mV);
            pscale = part.mParent->mScale.mV[0];
        }
        else
        { //use source object as position

            if (part.mPartSourcep->mSourceObjectp.notNull())
            {
                LLVector3 v = LLVector3(0,0,1);
                v *= part.mPartSourcep->mSourceObjectp->getRenderRotation();
                paxis.load3(v.mV);
                ppos.load3(part.mPartSourcep->mPosAgent.mV);
                pscale = part.mStartScale.mV[0];
            }
            else
            { //no source object, no parent, nothing to draw
                ppos = pos;
                pscale = scale;
                paxis = axis;
            }
        }

        LLVector4a p0, p1, p2, p3;

        scale *= 0.5f;
        pscale *= 0.5f;

        axis.mul(scale);
        paxis.mul(pscale);

        p0.setAdd(pos, axis);
        p1.setSub(pos,axis);
        p2.setAdd(ppos, paxis);
        p3.setSub(ppos, paxis);

        (*verticesp++) = p2;
        (*verticesp++) = p3;
        (*verticesp++) = p0;
        (*verticesp++) = p1;
    }
    else
    {
        LLVector4a part_pos_agent;
        part_pos_agent.load3(part.mPosAgent.mV);
        LLVector4a camera_agent;
        camera_agent.load3(getCameraPosition().mV);
        LLVector4a at;
        at.setSub(part_pos_agent, camera_agent);
        LLVector4a up(0, 0, 1);
        LLVector4a right;

        right.setCross3(at, up);
        right.normalize3fast();

        up.setCross3(right, at);
        up.normalize3fast();

        if (part.mFlags & LLPartData::LL_PART_FOLLOW_VELOCITY_MASK && !part.mVelocity.isExactlyZero())
        {
            LLVector4a normvel;
            normvel.load3(part.mVelocity.mV);
            normvel.normalize3fast();
            LLVector2 up_fracs;
            up_fracs.mV[0] = normvel.dot3(right).getF32();
            up_fracs.mV[1] = normvel.dot3(up).getF32();
            up_fracs.normalize();
            LLVector4a new_up;
            LLVector4a new_right;

            //new_up = up_fracs.mV[0] * right + up_fracs.mV[1]*up;
            LLVector4a t = right;
            t.mul(up_fracs.mV[0]);
            new_up = up;
            new_up.mul(up_fracs.mV[1]);
            new_up.add(t);

            //new_right = up_fracs.mV[1] * right - up_fracs.mV[0]*up;
            t = right;
            t.mul(up_fracs.mV[1]);
            new_right = up;
            new_right.mul(up_fracs.mV[0]);
            t.sub(new_right);

            up = new_up;
            right = t;
            up.normalize3fast();
            right.normalize3fast();
        }

        right.mul(0.5f*part.mScale.mV[0]);
        up.mul(0.5f*part.mScale.mV[1]);


        //HACK -- the verticesp->mV[3] = 0.f here are to set the texture index to 0 (particles don't use texture batching, maybe they should)
        // this works because there is actually a 4th float stored after the vertex position which is used as a texture index
        // also, somebody please VECTORIZE THIS

        LLVector4a ppapu;
        LLVector4a ppamu;

        ppapu.setAdd(part_pos_agent, up);
        ppamu.setSub(part_pos_agent, up);

        verticesp->setSub(ppapu, right);
        (*verticesp++).getF32ptr()[3] = 0.f;
        verticesp->setSub(ppamu, right);
        (*verticesp++).getF32ptr()[3] = 0.f;
        verticesp->setAdd(ppapu, right);
        (*verticesp++).getF32ptr()[3] = 0.f;
        verticesp->setAdd(ppamu, right);
        (*verticesp++).getF32ptr()[3] = 0.f;
    }
}



U32 LLVOPartGroup::getPartitionType() const
{
    return LLViewerRegion::PARTITION_PARTICLE;
}

U32 LLVOHUDPartGroup::getPartitionType() const
{
    return LLViewerRegion::PARTITION_HUD_PARTICLE;
}

LLDrawable* LLVOHUDPartGroup::createDrawable(LLPipeline *pipeline)
{
    pipeline->allocDrawable(this);
    mDrawable->setLit(false);
    mDrawable->setRenderType(LLPipeline::RENDER_TYPE_HUD_PARTICLES);
    return mDrawable;
}

