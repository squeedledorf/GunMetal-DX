/**
 * @file llvograss.h
 * @brief Description of LLVOGrass class
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

#ifndef LL_LLVOGRASS_H
#define LL_LLVOGRASS_H

#include "llviewerobject.h"
#include <map>

class LLSurfacePatch;
class LLViewerTexture;


class LLVOGrass : public LLAlphaObject
{
public:
    LLVOGrass(const LLUUID &id, const LLPCode pcode, LLViewerRegion *regionp);

    // Initialize data that's only inited once per class.
    static void initClass();
    static void cleanupClass();

    virtual U32 getPartitionType() const;

    /*virtual*/ U32 processUpdateMessage(LLMessageSystem *mesgsys,
                                            void **user_data,
                                            U32 block_num,
                                            const EObjectUpdateType update_type,
                                            LLDataPacker *dp);
    static void import(LLFILE *file, LLMessageSystem *mesgsys, const LLVector3 &pos);
    /*virtual*/ void exportFile(LLFILE *file, const LLVector3 &position);

    void updateDrawable(bool force_damped);

    /*virtual*/ LLDrawable* createDrawable(LLPipeline *pipeline);

    void updateFaceSize(S32 idx) { }
    /*virtual*/ void updateTextures();
    /*virtual*/ bool updateLOD();
    /*virtual*/ void setPixelAreaAndAngle(LLAgent &agent); // generate accurate apparent angle and area

    // VULKAN (#20): LL's getGeometry blades (all GRASS_MAX_BLADES; LL's blade-count LOD
    // is not applied) relative to getPositionAgent(), bases on the terrain. Also
    // latches mPatch / mLastPatchUpdateTime like getGeometry. False if the species or
    // region is unknown.
    bool buildLocalBlades(std::vector<LLVector3>& pos, std::vector<LLVector3>& nrm,
                          std::vector<LLVector2>& uv, std::vector<U16>& idx);
    const LLUUID& getSpeciesTextureID() const;

    /*virtual*/ bool    isActive() const; // always false: clean-pipeline ingested


    static S32 sMaxGrassSpecies;

    struct GrassSpeciesData
    {
        LLUUID  mTextureID;

        F32     mBladeSizeX;
        F32     mBladeSizeY;
    };

    typedef std::map<U32, GrassSpeciesData*> SpeciesMap;

    U8              mSpecies;       // Species of grass
    F32             mBladeSizeX;
    F32             mBladeSizeY;

    LLSurfacePatch           *mPatch;           //  Stores the land patch where the grass is centered

    U64 mLastPatchUpdateTime;

    LLVector3                 mGrassBend;       // Accumulated wind (used for blowing trees)
    LLVector3                 mGrassVel;
    LLVector3                 mWind;
    F32                       mBladeWindAngle;
    F32                       mBWAOverlap;

protected:
    ~LLVOGrass();

private:
    void updateSpecies();
    F32 mLastHeight;        // For cheap update hack
    S32 mNumBlades;

    static SpeciesMap sSpeciesTable;
};
#endif // LL_VO_GRASS_
