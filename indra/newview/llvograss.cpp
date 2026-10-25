/**
 * @file llvograss.cpp
 * @brief Not a blade, but a clump of grass
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

#include "llvograss.h"

#include "llviewercontrol.h"

#include "llagentcamera.h"
#include "llnotificationsutil.h"
#include "lldrawable.h"
#include "llface.h"
#include "llsky.h"
#include "llsurface.h"
#include "llsurfacepatch.h"
#include "llviewercamera.h"
#include "llviewertexturelist.h"
#include "llviewerregion.h"
#include "pipeline.h"
#include "llworld.h"
#include "lldir.h"
#include "llxmltree.h"
#include "llvotree.h"

const S32 GRASS_MAX_BLADES =    32;
const F32 GRASS_BLADE_BASE =    0.25f;          //  Width of grass at base
const F32 GRASS_BLADE_HEIGHT =  0.5f;           // meters
const F32 GRASS_DISTRIBUTION_SD = 0.15f;        // empirically defined

F32 exp_x[GRASS_MAX_BLADES];
F32 exp_y[GRASS_MAX_BLADES];
F32 rot_x[GRASS_MAX_BLADES];
F32 rot_y[GRASS_MAX_BLADES];
F32 dz_x [GRASS_MAX_BLADES];
F32 dz_y [GRASS_MAX_BLADES];

F32 w_mod[GRASS_MAX_BLADES];                    //  Factor to modulate wind movement by to randomize appearance

LLVOGrass::SpeciesMap LLVOGrass::sSpeciesTable;
S32 LLVOGrass::sMaxGrassSpecies = 0;


LLVOGrass::LLVOGrass(const LLUUID &id, const LLPCode pcode, LLViewerRegion *regionp)
:   LLAlphaObject(id, pcode, regionp)
{
    mPatch               = NULL;
    mLastPatchUpdateTime = 0;
    mGrassVel.clearVec();
    mGrassBend.clearVec();
    mbCanSelect          = true;

    mBladeWindAngle      = 35.f;
    mBWAOverlap          = 2.f;

    setNumTEs(1);

    setTEColor(0, LLColor4(1.0f, 1.0f, 1.0f, 1.f));
    mNumBlades = GRASS_MAX_BLADES;
}

LLVOGrass::~LLVOGrass()
{
}


void LLVOGrass::updateSpecies()
{
    mSpecies = getAttachmentState();

    if (!sSpeciesTable.count(mSpecies))
    {
        LL_INFOS() << "Unknown grass type, substituting grass type." << LL_ENDL;
        SpeciesMap::const_iterator it = sSpeciesTable.begin();
        mSpecies = (*it).first;
    }
    setTEImage(0, LLViewerTextureManager::getFetchedTexture(sSpeciesTable[mSpecies]->mTextureID, FTT_DEFAULT, true, LLGLTexture::BOOST_NONE, LLViewerTexture::LOD_TEXTURE));
}


void LLVOGrass::initClass()
{
    LLVector3 pos(0.0f, 0.0f, 0.0f);
    //  Create nifty list of exponential distribution 0-1
    F32 x = 0.f;
    F32 y = 0.f;
    F32 rot;

    std::string xml_filename = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS,"grass.xml");

    LLXmlTree grass_def_grass;

    if (!grass_def_grass.parseFile(xml_filename))
    {
        LL_ERRS() << "Failed to parse grass file." << LL_ENDL;
        return;
    }

    LLXmlTreeNode* rootp = grass_def_grass.getRoot();

    for (LLXmlTreeNode* grass_def = rootp->getFirstChild();
        grass_def;
        grass_def = rootp->getNextChild())
    {
        if (!grass_def->hasName("grass"))
        {
            LL_WARNS() << "Invalid grass definition node " << grass_def->getName() << LL_ENDL;
            continue;
        }
        F32 F32_val;
        LLUUID id;

        bool success{ true };

        S32 species;
        static LLStdStringHandle species_id_string = LLXmlTree::addAttributeString("species_id");
        if (!grass_def->getFastAttributeS32(species_id_string, species))
        {
            LL_WARNS() << "No species id defined" << LL_ENDL;
            continue;
        }

        if (species < 0)
        {
            LL_WARNS() << "Invalid species id " << species << LL_ENDL;
            continue;
        }

        GrassSpeciesData* newGrass = new GrassSpeciesData();


        static LLStdStringHandle texture_id_string = LLXmlTree::addAttributeString("texture_id");
        grass_def->getFastAttributeUUID(texture_id_string, id);
        newGrass->mTextureID = id;

        static LLStdStringHandle blade_sizex_string = LLXmlTree::addAttributeString("blade_size_x");
        success &= grass_def->getFastAttributeF32(blade_sizex_string, F32_val);
        newGrass->mBladeSizeX = F32_val;

        static LLStdStringHandle blade_sizey_string = LLXmlTree::addAttributeString("blade_size_y");
        success &= grass_def->getFastAttributeF32(blade_sizey_string, F32_val);
        newGrass->mBladeSizeY = F32_val;

        if (sSpeciesTable.count(species))
        {
            LL_INFOS() << "Grass species " << species << " already defined! Duplicate discarded." << LL_ENDL;
            delete newGrass;
            continue;
        }
        else
        {
            sSpeciesTable[species] = newGrass;
        }

        if (species >= sMaxGrassSpecies) sMaxGrassSpecies = species + 1;

        if (!success)
        {
            std::string name;
            static LLStdStringHandle name_string = LLXmlTree::addAttributeString("name");
            grass_def->getFastAttributeString(name_string, name);
            LL_WARNS() << "Incomplete definition of grass " << name << LL_ENDL;
        }
    }

    bool have_all_grass{ true };
    std::string err;

    for (S32 i=0;i<sMaxGrassSpecies;++i)
    {
        if (!sSpeciesTable.count(i))
        {
            err.append(llformat(" %d",i));
            have_all_grass = false;
        }
    }

    if (!have_all_grass)
    {
        LLSD args;
        args["SPECIES"] = err;
        LLNotificationsUtil::add("ErrorUndefinedGrasses", args);
    }

    for (S32 i = 0; i < GRASS_MAX_BLADES; ++i)
    {
        if (1)   //(i%2 == 0)           Uncomment for X blading
        {
            F32 u = sqrt(-2.0f * log(ll_frand()));
            F32 v = 2.0f * F_PI * ll_frand();

            x = u * sin(v) * GRASS_DISTRIBUTION_SD;
            y = u * cos(v) * GRASS_DISTRIBUTION_SD;

            rot = ll_frand(F_PI);
        }
        else
        {
            rot += (F_PI*0.4f + ll_frand(0.2f*F_PI));
        }

        exp_x[i] = x;
        exp_y[i] = y;
        rot_x[i] = sin(rot);
        rot_y[i] = cos(rot);
        dz_x[i] = ll_frand(GRASS_BLADE_BASE * 0.25f);
        dz_y[i] = ll_frand(GRASS_BLADE_BASE * 0.25f);
        w_mod[i] = 0.5f + ll_frand();                       //  Degree to which blade is moved by wind

    }
}

void LLVOGrass::cleanupClass()
{
    for_each(sSpeciesTable.begin(), sSpeciesTable.end(), DeletePairedPointer());
    sSpeciesTable.clear();
}

U32 LLVOGrass::processUpdateMessage(LLMessageSystem *mesgsys,
                                          void **user_data,
                                          U32 block_num,
                                          const EObjectUpdateType update_type,
                                          LLDataPacker *dp)
{
    // Do base class updates...
    U32 retval = LLViewerObject::processUpdateMessage(mesgsys, user_data, block_num, update_type, dp);

    updateSpecies();

    if (  (getVelocity().lengthSquared() > 0.f)
        ||(getAcceleration().lengthSquared() > 0.f)
        ||(getAngularVelocity().lengthSquared() > 0.f))
    {
        LL_INFOS() << "ACK! Moving grass!" << LL_ENDL;
        setVelocity(LLVector3::zero);
        setAcceleration(LLVector3::zero);
        setAngularVelocity(LLVector3::zero);
    }

    if (mDrawable)
    {
        gPipeline.markRebuild(mDrawable, LLDrawable::REBUILD_VOLUME);
    }

    return retval;
}

bool LLVOGrass::isActive() const
{
    // Ingested by the clean pipeline (LLCleanIngest); LL's per-frame idle rebuild
    // queue has nothing to feed. Bounds for picking are set at creation.
    return false;
}

void LLVOGrass::setPixelAreaAndAngle(LLAgent &agent)
{
    // This should be the camera's center, as soon as we move to all region-local.
    LLVector3 relative_position = getPositionAgent() - gAgentCamera.getCameraPositionAgent();
    F32 range = relative_position.length();

    F32 max_scale = getMaxScale();

    mAppAngle = (F32) atan2( max_scale, range) * RAD_TO_DEG;

    // Compute pixels per meter at the given range
    F32 pixels_per_meter = LLViewerCamera::getInstance()->getViewHeightInPixels() / (tan(LLViewerCamera::getInstance()->getView()) * range);

    // Assume grass texture is a 5 meter by 5 meter sprite at the grass object's center
    mPixelArea = (pixels_per_meter) * (pixels_per_meter) * 25.f;
}


// BUG could speed this up by caching the relative_position and range calculations
void LLVOGrass::updateTextures()
{
    if (getTEImage(0))
    {
        if (gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_TEXTURE_AREA))
        {
            setDebugText(llformat("%4.0f", (F32) sqrt(mPixelArea)));
        }
        getTEImage(0)->addTextureStats(mPixelArea);
    }
}

bool LLVOGrass::updateLOD()
{
    if (mDrawable->getNumFaces() <= 0)
    {
        return false;
    }

    LLFace* face = mDrawable->getFace(0);

    if(LLVOTree::isTreeRenderingStopped())
    {
        if(mNumBlades)
        {
            mNumBlades = 0 ;
            face->setSize(0, 0);
            gPipeline.markRebuild(mDrawable, LLDrawable::REBUILD_ALL);
        }
        return true ;
    }
    if(!mNumBlades)
    {
        mNumBlades = GRASS_MAX_BLADES;
    }

    F32 tan_angle = 0.f;
    S32 num_blades = 0;

    tan_angle = (mScale.mV[0]*mScale.mV[1])/mDrawable->mDistanceWRTCamera;
    num_blades = llmin(GRASS_MAX_BLADES, lltrunc(tan_angle * 5));
    num_blades = llmax(1, num_blades);
    if (num_blades >= (mNumBlades << 1))
    {
        while (mNumBlades < num_blades)
        {
            mNumBlades <<= 1;
        }
        if (face)
        {
            face->setSize(mNumBlades*8, mNumBlades*12);
        }
        gPipeline.markRebuild(mDrawable, LLDrawable::REBUILD_ALL);
    }
    else if (num_blades <= (mNumBlades >> 1))
    {
        while (mNumBlades > num_blades)
        {
            mNumBlades >>=1;
        }

        if (face)
        {
            face->setSize(mNumBlades*8, mNumBlades*12);
        }
        gPipeline.markRebuild(mDrawable, LLDrawable::REBUILD_ALL);
        return true;
    }

    return false;
}

LLDrawable* LLVOGrass::createDrawable(LLPipeline *pipeline)
{
    pipeline->allocDrawable(this);
    mDrawable->setRenderType(LLPipeline::RENDER_TYPE_GRASS);

    return mDrawable;
}

const LLUUID& LLVOGrass::getSpeciesTextureID() const
{
    SpeciesMap::const_iterator it = sSpeciesTable.find(mSpecies);
    return it == sSpeciesTable.end() ? LLUUID::null : it->second->mTextureID;
}

bool LLVOGrass::buildLocalBlades(std::vector<LLVector3>& pos, std::vector<LLVector3>& nrm,
                                 std::vector<LLVector2>& uv, std::vector<U16>& idx)
{
    SpeciesMap::const_iterator it = sSpeciesTable.find(mSpecies);
    if (it == sSpeciesTable.end() || !mRegionp) return false;

    mPatch = mRegionp->getLand().resolvePatchRegion(getPositionRegion());
    if (mPatch)
        mLastPatchUpdateTime = mPatch->getLastUpdateTime();

    // getGeometry in object-local space: LL emits region + origin-agent positions;
    // the instance translates by getPositionAgent() (= mPosition + origin agent for
    // these unparented objects), so subtract mPosition instead.
    const F32 width  = it->second->mBladeSizeX;
    const F32 height = it->second->mBladeSizeY;
    const S32 n = GRASS_MAX_BLADES;
    pos.resize(n * 8); nrm.resize(n * 8); uv.resize(n * 8); idx.resize(n * 12);
    U32 v = 0, k = 0;
    for (S32 i = 0; i < n; i++)
    {
        const F32 x   = exp_x[i] * mScale.mV[VX];
        const F32 y   = exp_y[i] * mScale.mV[VY];
        const F32 xf  = rot_x[i] * GRASS_BLADE_BASE * width * w_mod[i];
        const F32 yf  = rot_y[i] * GRASS_BLADE_BASE * width * w_mod[i];
        const F32 dzx = dz_x [i];
        const F32 dzy = dz_y [i];
        const F32 blade_height = GRASS_BLADE_HEIGHT * height * w_mod[i];

        uv[v + 0].set(0, 0);     uv[v + 1].set(0, 0);
        uv[v + 2].set(0, 0.98f); uv[v + 3].set(0, 0.98f);
        uv[v + 4].set(1, 0);     uv[v + 5].set(1, 0);
        uv[v + 6].set(1, 0.98f); uv[v + 7].set(1, 0.98f);

        LLVector3 position;
        position.mV[0] = mPosition.mV[VX] + x + xf;
        position.mV[1] = mPosition.mV[VY] + y + yf;
        position.mV[2] = mRegionp->getLand().resolveHeightRegion(position);
        LLVector3 v1 = position;
        position.mV[0] += dzx;
        position.mV[1] += dzy;
        position.mV[2] += blade_height;
        const LLVector3 v2 = position;
        position.mV[0] = mPosition.mV[VX] + x - xf;
        // LL's "y - xf" (not yf) -- kept so the blades lay out exactly as LL's.
        position.mV[1] = mPosition.mV[VY] + y - xf;
        position.mV[2] = mRegionp->getLand().resolveHeightRegion(position);
        const LLVector3 v3 = position;

        LLVector3 normal1 = (v1-v2) % (v2-v3);
        normal1.mV[VZ] = 0.75f;
        normal1.normalize();
        LLVector3 normal2 = -normal1;
        normal2.mV[VZ] = -normal2.mV[VZ];

        position.mV[0] += dzx;
        position.mV[1] += dzy;
        position.mV[2] += blade_height;
        const LLVector3 v4 = position;

        const LLVector3 corners[4] = { v1, v2, v3, v4 };
        for (S32 c = 0; c < 4; ++c)
        {
            pos[v + c * 2] = pos[v + c * 2 + 1] = corners[c] - mPosition;
            nrm[v + c * 2]     = normal1;
            nrm[v + c * 2 + 1] = normal2;
        }

        const U16 o = (U16)v;
        idx[k++] = o + 0; idx[k++] = o + 2; idx[k++] = o + 4;
        idx[k++] = o + 2; idx[k++] = o + 6; idx[k++] = o + 4;
        idx[k++] = o + 1; idx[k++] = o + 5; idx[k++] = o + 3;
        idx[k++] = o + 3; idx[k++] = o + 5; idx[k++] = o + 7;
        v += 8;
    }
    return true;
}

U32 LLVOGrass::getPartitionType() const
{
    return LLViewerRegion::PARTITION_GRASS;
}

// virtual
void LLVOGrass::updateDrawable(bool force_damped)
{
    // Force an immediate rebuild on any update
    if (mDrawable.notNull())
    {
        mDrawable->updateXform(true);
        gPipeline.markRebuild(mDrawable, LLDrawable::REBUILD_ALL);
    }
    clearChanged(SHIFTED);
}

// virtual
