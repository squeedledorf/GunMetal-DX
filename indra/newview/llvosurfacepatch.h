/**
 * @file llvosurfacepatch.h
 * @brief Description of LLVOSurfacePatch class
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

#ifndef LL_VOSURFACEPATCH_H
#define LL_VOSURFACEPATCH_H

#include "llviewerobject.h"
#include "llstrider.h"

class LLSurfacePatch;
class LLDrawPool;
class LLVector2;
class LLFacePool;
class LLFace;

class LLVOSurfacePatch : public LLStaticViewerObject
{
public:
    static F32 sLODFactor;

    LLVOSurfacePatch(const LLUUID &id, const LLPCode pcode, LLViewerRegion *regionp);

    /*virtual*/ void markDead();

    // Initialize data that's only inited once per class.
    static void initClass();

    virtual U32 getPartitionType() const;

    /*virtual*/ LLDrawable* createDrawable(LLPipeline *pipeline);
    /*virtual*/ void        updateGL();
    /*virtual*/ bool        updateLOD() { return true; }
    /*virtual*/ void updateTextures();
    /*virtual*/ void setPixelAreaAndAngle(LLAgent &agent); // generate accurate apparent angle and area

    /*virtual*/ void updateSpatialExtents(LLVector4a& newMin, LLVector4a& newMax);
    /*virtual*/ bool isActive() const; // Whether this object needs to do an idleUpdate.

    void setPatch(LLSurfacePatch *patchp);
    LLSurfacePatch  *getPatch() const       { return mPatchp; }

    void dirtyPatch();
    void dirtyGeom();


    bool            mDirtiedPatch;
protected:
    ~LLVOSurfacePatch();

    LLFacePool      *mPool;
    LLFacePool      *getPool();
    S32             mBaseComp;
    LLSurfacePatch  *mPatchp;
    bool            mDirtyTexture;
    bool            mDirtyTerrain;

};

#endif // LL_VOSURFACEPATCH_H
