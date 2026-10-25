/**
 * @file llvosurfacepatch.cpp
 * @brief Viewer-object derived "surface patch", which is a piece of terrain
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

#include "llvosurfacepatch.h"
#include "llcleaningest.h"

#include "lldrawable.h"
#include "llface.h"
#include "llsurfacepatch.h"
#include "llsurface.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llvlcomposition.h"
#include "pipeline.h"

F32 LLVOSurfacePatch::sLODFactor = 1.f;

LLVOSurfacePatch::LLVOSurfacePatch(const LLUUID &id, const LLPCode pcode, LLViewerRegion *regionp)
    :   LLStaticViewerObject(id, pcode, regionp),
        mDirtiedPatch(false),
        mPool(NULL),
        mBaseComp(0),
        mPatchp(NULL),
        mDirtyTexture(false),
        mDirtyTerrain(false)
{
    // Terrain must draw during selection passes so it can block objects behind it.
    mbCanSelect = true;
    setScale(LLVector3(16.f, 16.f, 16.f)); // Hack for setting scale for bounding boxes/visibility.
}


LLVOSurfacePatch::~LLVOSurfacePatch()
{
    mPatchp = NULL;
}


void LLVOSurfacePatch::markDead()
{
    if (mPatchp)
    {
        mPatchp->clearVObj();
        mPatchp = NULL;
    }
    LLViewerObject::markDead();
}


bool LLVOSurfacePatch::isActive() const
{
    return false;
}


void LLVOSurfacePatch::setPixelAreaAndAngle(LLAgent &agent)
{
    mAppAngle = 50;
    mPixelArea = 500*500;
}


void LLVOSurfacePatch::updateTextures()
{
}


LLFacePool *LLVOSurfacePatch::getPool()
{
    mPool = (LLFacePool *) gPipeline.getPool(LLDrawPool::POOL_TERRAIN, mPatchp->getSurface()->getSTexture());

    return mPool;
}


LLDrawable *LLVOSurfacePatch::createDrawable(LLPipeline *pipeline)
{
    pipeline->allocDrawable(this);

    mDrawable->setRenderType(LLPipeline::RENDER_TYPE_TERRAIN);

    mBaseComp = llfloor(mPatchp->getMinComposition());
    S32 min_comp, max_comp, range;
    min_comp = llfloor(mPatchp->getMinComposition());
    max_comp = llceil(mPatchp->getMaxComposition());
    range = (max_comp - min_comp);
    range++;
    if (range > 3)
    {
        if ((mPatchp->getMinComposition() - min_comp) > (max_comp - mPatchp->getMaxComposition()))
        {
            // The top side runs over more
            mBaseComp++;
        }
        range = 3;
    }

    LLFacePool *poolp = getPool();

    mDrawable->addFace(poolp, NULL);

    return mDrawable;
}


void LLVOSurfacePatch::updateGL()
{
    if (mPatchp)
    {
        LL_PROFILE_ZONE_SCOPED;
        mPatchp->updateGL();
    }
}

void LLVOSurfacePatch::setPatch(LLSurfacePatch *patchp)
{
    mPatchp = patchp;

    dirtyPatch();
};


void LLVOSurfacePatch::dirtyPatch()
{
    mDirtiedPatch = true;
    dirtyGeom();
    mDirtyTerrain = true;
    LLVector3 center = mPatchp->getCenterRegion();
    LLSurface *surfacep = mPatchp->getSurface();

    setPositionRegion(center);

    F32 scale_factor = surfacep->getGridsPerPatchEdge() * surfacep->getMetersPerGrid();
    setScale(LLVector3(scale_factor, scale_factor, mPatchp->getMaxZ() - mPatchp->getMinZ()));
}

void LLVOSurfacePatch::dirtyGeom()
{
    // Re-ingest this patch next frame (#235: edits waited for the sweep).
    LLCleanIngest::instance().noteTerrainDirty(this);
    if (mDrawable)
    {
        gPipeline.markRebuild(mDrawable, LLDrawable::REBUILD_ALL);
        mDrawable->movePartition();
    }
}

void LLVOSurfacePatch::updateSpatialExtents(LLVector4a& newMin, LLVector4a &newMax)
{
    LLVector3 posAgent = getPositionAgent();
    LLVector3 scale = getScale();
    //make z-axis scale at least 1 to avoid shadow artifacts on totally flat land
    scale.mV[VZ] = llmax(scale.mV[VZ], 1.f);
    newMin.load3( (posAgent-scale*0.5f).mV); // Changing to 2.f makes the culling a -little- better, but still wrong
    newMax.load3( (posAgent+scale*0.5f).mV);
    LLVector4a pos;
    pos.setAdd(newMin,newMax);
    pos.mul(0.5f);
    mDrawable->setPositionGroup(pos);
}

U32 LLVOSurfacePatch::getPartitionType() const
{
    return LLViewerRegion::PARTITION_TERRAIN;
}
