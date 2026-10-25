/**
 * @file llvowater.cpp
 * @brief LLVOWater class implementation
 *
 * $LicenseInfo:firstyear=2005&license=viewerlgpl$
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

#include "llvowater.h"

#include "llviewercontrol.h"

#include "lldrawable.h"
#include "llface.h"
#include "llsky.h"
#include "llsurface.h"
#include "llviewercamera.h"
#include "llviewertexturelist.h"
#include "llviewerregion.h"
#include "llworld.h"
#include "pipeline.h"


LLVOWater::LLVOWater(const LLUUID &id,
                     const LLPCode pcode,
                     LLViewerRegion *regionp) :
    LLStaticViewerObject(id, pcode, regionp),
    mRenderType(LLPipeline::RENDER_TYPE_WATER)
{
    // Terrain must draw during selection passes so it can block objects behind it.
    mbCanSelect = false;
    setScale(LLVector3(256.f, 256.f, 0.f)); // Hack for setting scale for bounding boxes/visibility.

    mIsEdgePatch = false;
}


void LLVOWater::markDead()
{
    LLViewerObject::markDead();
}


bool LLVOWater::isActive() const
{
    return false;
}


void LLVOWater::setPixelAreaAndAngle(LLAgent &agent)
{
    mAppAngle = 50;
    mPixelArea = 500*500;
}


// virtual
void LLVOWater::updateTextures()
{
}

// Never gets called
void  LLVOWater::idleUpdate(LLAgent &agent, const F64 &time)
{
}

LLDrawable *LLVOWater::createDrawable(LLPipeline *pipeline)
{
    pipeline->allocDrawable(this);
    mDrawable->setLit(false);
    mDrawable->setRenderType(mRenderType);

    LLFacePool *pool = (LLFacePool *) gPipeline.getPool(LLDrawPool::POOL_WATER);

    mDrawable->setNumFaces(1, pool, LLWorld::getInstance()->getDefaultWaterTexture());

    return mDrawable;
}

void LLVOWater::initClass()
{
}

void LLVOWater::cleanupClass()
{
}

void setVecZ(LLVector3& v)
{
    v.mV[VX] = 0;
    v.mV[VY] = 0;
    v.mV[VZ] = 1;
}

void LLVOWater::setIsEdgePatch(const bool edge_patch)
{
    mIsEdgePatch = edge_patch;
}

void LLVOWater::updateSpatialExtents(LLVector4a &newMin, LLVector4a& newMax)
{
    LLVector4a pos;
    pos.load3(getPositionAgent().mV);
    LLVector4a scale;
    scale.load3(getScale().mV);
    scale.mul(0.5f);

    newMin.setSub(pos, scale);
    newMax.setAdd(pos, scale);

    pos.setAdd(newMin,newMax);
    pos.mul(0.5f);

    mDrawable->setPositionGroup(pos);
}

U32 LLVOWater::getPartitionType() const
{
    if (mIsEdgePatch)
    {
        return LLViewerRegion::PARTITION_VOIDWATER;
    }

    return LLViewerRegion::PARTITION_WATER;
}

U32 LLVOVoidWater::getPartitionType() const
{
    return LLViewerRegion::PARTITION_VOIDWATER;
}
