/**
 * @file llvotree.cpp
 * @brief LLVOTree class implementation
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

#include "llvotree.h"


#include "llviewercontrol.h"
#include "lldir.h"
#include "llprimitive.h"
#include "lltree_common.h"
#include "llxmltree.h"
#include "material_codes.h"
#include "object_flags.h"

#include "llagentcamera.h"
#include "lldrawable.h"
#include "llface.h"
#include "llviewercamera.h"
#include "llviewertexturelist.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llvolumemgr.h"
#include "llvovolume.h"
#include "llworld.h"
#include "noise.h"
#include "pipeline.h"
#include "llnotificationsutil.h"
#include "raytrace.h"
#include "llrender.h"

extern LLPipeline gPipeline;

const S32 MAX_SLICES = 32;
const F32 LEAF_LEFT = 0.52f;
const F32 LEAF_RIGHT = 0.98f;
const F32 LEAF_TOP = 1.0f;
const F32 LEAF_BOTTOM = 0.52f;
const F32 LEAF_WIDTH = 1.f;

const S32 LLVOTree::sMAX_NUM_TREE_LOD_LEVELS = 4 ;

S32 LLVOTree::sLODVertexOffset[sMAX_NUM_TREE_LOD_LEVELS];
S32 LLVOTree::sLODVertexCount[sMAX_NUM_TREE_LOD_LEVELS];
S32 LLVOTree::sLODIndexOffset[sMAX_NUM_TREE_LOD_LEVELS];
S32 LLVOTree::sLODIndexCount[sMAX_NUM_TREE_LOD_LEVELS];
S32 LLVOTree::sLODSlices[sMAX_NUM_TREE_LOD_LEVELS] = {10, 5, 4, 3};
F32 LLVOTree::sLODAngles[sMAX_NUM_TREE_LOD_LEVELS] = {30.f, 20.f, 15.f, F_ALMOST_ZERO};

F32 LLVOTree::sTreeFactor = 1.f;

LLVOTree::SpeciesMap LLVOTree::sSpeciesTable;
S32 LLVOTree::sMaxTreeSpecies = 0;

// Tree variables and functions

LLVOTree::LLVOTree(const LLUUID &id, const LLPCode pcode, LLViewerRegion *regionp):
                        LLViewerObject(id, pcode, regionp)
{
    mSpecies = 0;
    mFrameCount = 0;
    mWind = mRegionp->mWind.getVelocity(getPositionRegion());
    mTrunkLOD = 0;

    // if assert triggers, idleUpdate() needs to be revised and adjusted to new LOD levels
    llassert(sMAX_NUM_TREE_LOD_LEVELS == LLVolumeLODGroup::NUM_LODS);
}


LLVOTree::~LLVOTree()
{
    if (mData)
    {
        delete[] mData;
        mData = NULL;
    }
}

//static
bool LLVOTree::isTreeRenderingStopped()
{
    return LLVOTree::sTreeFactor < LLVOTree::sLODAngles[sMAX_NUM_TREE_LOD_LEVELS - 1] ;
}

// static
void LLVOTree::initClass()
{
    std::string xml_filename = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS,"trees.xml");

    LLXmlTree tree_def_tree;

    if (!tree_def_tree.parseFile(xml_filename))
    {
        LL_ERRS() << "Failed to parse tree file." << LL_ENDL;
    }

    LLXmlTreeNode* rootp = tree_def_tree.getRoot();

    for (LLXmlTreeNode* tree_def = rootp->getFirstChild();
        tree_def;
        tree_def = rootp->getNextChild())
        {
            if (!tree_def->hasName("tree"))
            {
                LL_WARNS() << "Invalid tree definition node " << tree_def->getName() << LL_ENDL;
                continue;
            }
            F32 F32_val;
            LLUUID id;
            S32 S32_val;

            bool success{ true };
            S32 species;
            static LLStdStringHandle species_id_string = LLXmlTree::addAttributeString("species_id");
            if (!tree_def->getFastAttributeS32(species_id_string, species))
            {
                LL_WARNS() << "No species id defined" << LL_ENDL;
                continue;
            }

            if (species < 0)
            {
                LL_WARNS() << "Invalid species id " << species << LL_ENDL;
                continue;
            }

            if (sSpeciesTable.count(species))
            {
                LL_WARNS() << "Tree species " << species << " already defined! Duplicate discarded." << LL_ENDL;
                continue;
            }

            TreeSpeciesData* newTree = new TreeSpeciesData();

            static LLStdStringHandle texture_id_string = LLXmlTree::addAttributeString("texture_id");
            success &= tree_def->getFastAttributeUUID(texture_id_string, id);
            newTree->mTextureID = id;

            static LLStdStringHandle droop_string = LLXmlTree::addAttributeString("droop");
            success &= tree_def->getFastAttributeF32(droop_string, F32_val);
            newTree->mDroop = F32_val;

            static LLStdStringHandle twist_string = LLXmlTree::addAttributeString("twist");
            success &= tree_def->getFastAttributeF32(twist_string, F32_val);
            newTree->mTwist = F32_val;

            static LLStdStringHandle branches_string = LLXmlTree::addAttributeString("branches");
            success &= tree_def->getFastAttributeF32(branches_string, F32_val);
            newTree->mBranches = F32_val;

            static LLStdStringHandle depth_string = LLXmlTree::addAttributeString("depth");
            success &= tree_def->getFastAttributeS32(depth_string, S32_val);
            newTree->mDepth = S32_val;

            static LLStdStringHandle scale_step_string = LLXmlTree::addAttributeString("scale_step");
            success &= tree_def->getFastAttributeF32(scale_step_string, F32_val);
            newTree->mScaleStep = F32_val;

            static LLStdStringHandle trunk_depth_string = LLXmlTree::addAttributeString("trunk_depth");
            success &= tree_def->getFastAttributeS32(trunk_depth_string, S32_val);
            newTree->mTrunkDepth = S32_val;

            static LLStdStringHandle branch_length_string = LLXmlTree::addAttributeString("branch_length");
            success &= tree_def->getFastAttributeF32(branch_length_string, F32_val);
            newTree->mBranchLength = F32_val;

            static LLStdStringHandle trunk_length_string = LLXmlTree::addAttributeString("trunk_length");
            success &= tree_def->getFastAttributeF32(trunk_length_string, F32_val);
            newTree->mTrunkLength = F32_val;

            static LLStdStringHandle leaf_scale_string = LLXmlTree::addAttributeString("leaf_scale");
            success &= tree_def->getFastAttributeF32(leaf_scale_string, F32_val);
            newTree->mLeafScale = F32_val;

            static LLStdStringHandle billboard_scale_string = LLXmlTree::addAttributeString("billboard_scale");
            success &= tree_def->getFastAttributeF32(billboard_scale_string, F32_val);
            newTree->mBillboardScale = F32_val;

            static LLStdStringHandle billboard_ratio_string = LLXmlTree::addAttributeString("billboard_ratio");
            success &= tree_def->getFastAttributeF32(billboard_ratio_string, F32_val);
            newTree->mBillboardRatio = F32_val;

            static LLStdStringHandle trunk_aspect_string = LLXmlTree::addAttributeString("trunk_aspect");
            success &= tree_def->getFastAttributeF32(trunk_aspect_string, F32_val);
            newTree->mTrunkAspect = F32_val;

            static LLStdStringHandle branch_aspect_string = LLXmlTree::addAttributeString("branch_aspect");
            success &= tree_def->getFastAttributeF32(branch_aspect_string, F32_val);
            newTree->mBranchAspect = F32_val;

            static LLStdStringHandle leaf_rotate_string = LLXmlTree::addAttributeString("leaf_rotate");
            success &= tree_def->getFastAttributeF32(leaf_rotate_string, F32_val);
            newTree->mRandomLeafRotate = F32_val;

            static LLStdStringHandle noise_mag_string = LLXmlTree::addAttributeString("noise_mag");
            success &= tree_def->getFastAttributeF32(noise_mag_string, F32_val);
            newTree->mNoiseMag = F32_val;

            static LLStdStringHandle noise_scale_string = LLXmlTree::addAttributeString("noise_scale");
            success &= tree_def->getFastAttributeF32(noise_scale_string, F32_val);
            newTree->mNoiseScale = F32_val;

            static LLStdStringHandle taper_string = LLXmlTree::addAttributeString("taper");
            success &= tree_def->getFastAttributeF32(taper_string, F32_val);
            newTree->mTaper = F32_val;

            static LLStdStringHandle repeat_z_string = LLXmlTree::addAttributeString("repeat_z");
            success &= tree_def->getFastAttributeF32(repeat_z_string, F32_val);
            newTree->mRepeatTrunkZ = F32_val;

            sSpeciesTable[species] = newTree;

            if (species >= sMaxTreeSpecies) sMaxTreeSpecies = species + 1;

            if (!success)
            {
                std::string name;
                static LLStdStringHandle name_string = LLXmlTree::addAttributeString("name");
                tree_def->getFastAttributeString(name_string, name);
                LL_WARNS() << "Incomplete definition of tree " << name << LL_ENDL;
            }
        }

        bool have_all_trees {true};
        std::string err;

        for (S32 i=0;i<sMaxTreeSpecies;++i)
        {
            if (!sSpeciesTable.count(i))
            {
                err.append(llformat(" %d",i));
                have_all_trees = false;
            }
        }

        if (!have_all_trees)
        {
            LLSD args;
            args["SPECIES"] = err;
            LLNotificationsUtil::add("ErrorUndefinedTrees", args);
        }
};

//static
void LLVOTree::cleanupClass()
{
    std::for_each(sSpeciesTable.begin(), sSpeciesTable.end(), DeletePairedPointer());
    sSpeciesTable.clear();
}

U32 LLVOTree::processUpdateMessage(LLMessageSystem *mesgsys,
                                          void **user_data,
                                          U32 block_num, EObjectUpdateType update_type,
                                          LLDataPacker *dp)
{
    // Do base class updates...
    U32 retval = LLViewerObject::processUpdateMessage(mesgsys, user_data, block_num, update_type, dp);

    if (  (getVelocity().lengthSquared() > 0.f)
        ||(getAcceleration().lengthSquared() > 0.f)
        ||(getAngularVelocity().lengthSquared() > 0.f))
    {
        LL_INFOS() << "ACK! Moving tree!" << LL_ENDL;
        setVelocity(LLVector3::zero);
        setAcceleration(LLVector3::zero);
        setAngularVelocity(LLVector3::zero);
    }

    if (update_type == OUT_TERSE_IMPROVED)
    {
        // Nothing else needs to be done for the terse message.
        return retval;
    }

    //
    //  Load Instance-Specific data
    //
    if (mData)
    {
        mSpecies = ((U8 *)mData)[0];
    }

    if (!sSpeciesTable.count(mSpecies))
    {
        if (sSpeciesTable.size())
        {
            SpeciesMap::const_iterator it = sSpeciesTable.begin();
            mSpecies = (*it).first;
        }
    }

    //
    //  Load Species-Specific data
    //
    static const S32 MAX_TREE_TEXTURE_VIRTURE_SIZE_RESET_INTERVAL = 32 ; //frames.
    mTreeImagep = LLViewerTextureManager::getFetchedTexture(sSpeciesTable[mSpecies]->mTextureID, FTT_DEFAULT, true, LLGLTexture::BOOST_NONE, LLViewerTexture::LOD_TEXTURE);
    mTreeImagep->setMaxVirtualSizeResetInterval(MAX_TREE_TEXTURE_VIRTURE_SIZE_RESET_INTERVAL); //allow to wait for at most 16 frames to reset virtual size.

    mBranchLength = sSpeciesTable[mSpecies]->mBranchLength;
    mTrunkLength = sSpeciesTable[mSpecies]->mTrunkLength;
    mLeafScale = sSpeciesTable[mSpecies]->mLeafScale;
    mDroop = sSpeciesTable[mSpecies]->mDroop;
    mTwist = sSpeciesTable[mSpecies]->mTwist;
    mBranches = sSpeciesTable[mSpecies]->mBranches;
    mDepth = sSpeciesTable[mSpecies]->mDepth;
    mScaleStep = sSpeciesTable[mSpecies]->mScaleStep;
    mTrunkDepth = sSpeciesTable[mSpecies]->mTrunkDepth;
    mBillboardScale = sSpeciesTable[mSpecies]->mBillboardScale;
    mBillboardRatio = sSpeciesTable[mSpecies]->mBillboardRatio;
    mTrunkAspect = sSpeciesTable[mSpecies]->mTrunkAspect;
    mBranchAspect = sSpeciesTable[mSpecies]->mBranchAspect;

    // position change not caused by us, etc.  make sure to rebuild.
    gPipeline.markRebuild(mDrawable, LLDrawable::REBUILD_ALL);

    return retval;
}

bool LLVOTree::isActive() const
{
    // Ingested by the clean pipeline (LLCleanIngest); LL's per-frame idle rebuild
    // queue has nothing to feed. Bounds for picking are set at creation.
    return false;
}

void LLVOTree::render(LLAgent &agent)
{
}


void LLVOTree::setPixelAreaAndAngle(LLAgent &agent)
{
    LLVector3 center = getPositionAgent();//center of tree.
    LLVector3 viewer_pos_agent = gAgentCamera.getCameraPositionAgent();
    LLVector3 lookAt = center - viewer_pos_agent;
    F32 dist = lookAt.normVec() ;
    F32 cos_angle_to_view_dir = lookAt * LLViewerCamera::getInstance()->getXAxis() ;
    F32 radius = getScale().length()*0.5f;
    F32 range = dist - radius;

    if (range < F_ALMOST_ZERO || isHUDAttachment())     // range == zero
    {
        mAppAngle = 180.f;
    }
    else
    {
        mAppAngle = (F32) atan2( getMaxScale(), range) * RAD_TO_DEG;
    }

    F32 max_scale = mBillboardScale * getMaxScale();
    F32 area = max_scale * (max_scale*mBillboardRatio);
    // Compute pixels per meter at the given range
    F32 pixels_per_meter = LLViewerCamera::getInstance()->getViewHeightInPixels() / (tan(LLViewerCamera::getInstance()->getView()) * dist);
    mPixelArea = pixels_per_meter * pixels_per_meter * area ;

    F32 importance = LLFace::calcImportanceToCamera(cos_angle_to_view_dir, dist) ;
    mPixelArea = LLFace::adjustPixelArea(importance, mPixelArea) ;
    if (mPixelArea > LLViewerCamera::getInstance()->getScreenPixelArea())
    {
        mAppAngle = 180.f;
    }

#if 0
    // mAppAngle is a bit of voodoo;
    // use the one calculated LLViewerObject::setPixelAreaAndAngle above
    // to avoid LOD miscalculations
    mAppAngle = (F32) atan2( max_scale, range) * RAD_TO_DEG;
#endif
}

void LLVOTree::updateTextures()
{
    if (mTreeImagep)
    {
        if (gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_TEXTURE_AREA))
        {
            setDebugText(llformat("%4.0f", (F32) sqrt(mPixelArea)));
        }
    }
}


LLDrawable* LLVOTree::createDrawable(LLPipeline *pipeline)
{
    pipeline->allocDrawable(this);
    mDrawable->setLit(false);

    mDrawable->setRenderType(LLPipeline::RENDER_TYPE_TREE);

    LLFacePool *poolp = (LLFacePool *) gPipeline.getPool(LLDrawPool::POOL_TREE, mTreeImagep);

    // Just a placeholder for an actual object...
    LLFace *facep = mDrawable->addFace(poolp, mTreeImagep);
    facep->setSize(1, 3);
    facep->setTexture(LLRender::DIFFUSE_MAP, mTreeImagep);
    updateRadius();

    return mDrawable;
}


// Yes, I know this is bad.  I'll clean this up soon. - djs 04/02/02
const S32 LEAF_INDICES = 24;
const S32 LEAF_VERTICES = 16;

//static
void LLVOTree::initLODTables(S32& max_vertices, S32& max_indices)
{
    max_indices = LEAF_INDICES;
    max_vertices = LEAF_VERTICES;
    for (S32 lod = 0; lod < sMAX_NUM_TREE_LOD_LEVELS; lod++)
    {
        const S32 slices = sLODSlices[lod];
        sLODVertexOffset[lod] = max_vertices;
        sLODVertexCount[lod] = slices*slices;
        sLODIndexOffset[lod] = max_indices;
        sLODIndexCount[lod] = (slices-1)*(slices-1)*6;
        max_indices += sLODIndexCount[lod];
        max_vertices += sLODVertexCount[lod];
    }
}

// Leaf quads + the trunk-LOD cylinders into `out` (initLODTables' layout). Depends
// only on the species (was inline in updateGeometry).
//static
void LLVOTree::genReferenceGeometry(const TreeSpeciesData& sp, TreeRef& out)
{
    const F32 SRR3 = 0.577350269f; // sqrt(1/3)
    const F32 SRR2 = 0.707106781f; // sqrt(1/2)
    U32 i, j;
    U32 slices = MAX_SLICES;
    S32 lod;
#ifdef SHOW_ASSERT
    S32 max_vertices = 0, max_indices = 0;
    initLODTables(max_vertices, max_indices);
#endif

    S32 vertex_count = 0;
    S32 index_count = 0;

    // First leaf
    *(out.n++) =      LLVector3(-SRR2, -SRR2, 0.f);
    *(out.t++) =   LLVector2(LEAF_LEFT, LEAF_BOTTOM);
    *(out.v++) =     LLVector3(-0.5f*LEAF_WIDTH, 0.f, 0.f);
    *(out.c++) =       LLColor4U::white;
    vertex_count++;

    *(out.n++) =      LLVector3(SRR3, -SRR3, SRR3);
    *(out.t++) =   LLVector2(LEAF_RIGHT, LEAF_TOP);
    *(out.v++) =     LLVector3(0.5f*LEAF_WIDTH, 0.f, 1.f);
    *(out.c++) =       LLColor4U::white;
    vertex_count++;

    *(out.n++) =      LLVector3(-SRR3, -SRR3, SRR3);
    *(out.t++) =   LLVector2(LEAF_LEFT, LEAF_TOP);
    *(out.v++) =     LLVector3(-0.5f*LEAF_WIDTH, 0.f, 1.f);
    *(out.c++) =       LLColor4U::white;
    vertex_count++;

    *(out.n++) =      LLVector3(SRR2, -SRR2, 0.f);
    *(out.t++) =   LLVector2(LEAF_RIGHT, LEAF_BOTTOM);
    *(out.v++) =     LLVector3(0.5f*LEAF_WIDTH, 0.f, 0.f);
    *(out.c++) =       LLColor4U::white;
    vertex_count++;

    *(out.idx++) = 0;
    index_count++;
    *(out.idx++) = 1;
    index_count++;
    *(out.idx++) = 2;
    index_count++;

    *(out.idx++) = 0;
    index_count++;
    *(out.idx++) = 3;
    index_count++;
    *(out.idx++) = 1;
    index_count++;

    // Same leaf, inverse winding/normals
    *(out.n++) =      LLVector3(-SRR2, SRR2, 0.f);
    *(out.t++) =   LLVector2(LEAF_LEFT, LEAF_BOTTOM);
    *(out.v++) =     LLVector3(-0.5f*LEAF_WIDTH, 0.f, 0.f);
    *(out.c++) =       LLColor4U::white;
    vertex_count++;

    *(out.n++) =      LLVector3(SRR3, SRR3, SRR3);
    *(out.t++) =   LLVector2(LEAF_RIGHT, LEAF_TOP);
    *(out.v++) =     LLVector3(0.5f*LEAF_WIDTH, 0.f, 1.f);
    *(out.c++) =       LLColor4U::white;
    vertex_count++;

    *(out.n++) =      LLVector3(-SRR3, SRR3, SRR3);
    *(out.t++) =   LLVector2(LEAF_LEFT, LEAF_TOP);
    *(out.v++) =     LLVector3(-0.5f*LEAF_WIDTH, 0.f, 1.f);
    *(out.c++) =       LLColor4U::white;
    vertex_count++;

    *(out.n++) =      LLVector3(SRR2, SRR2, 0.f);
    *(out.t++) =   LLVector2(LEAF_RIGHT, LEAF_BOTTOM);
    *(out.v++) =     LLVector3(0.5f*LEAF_WIDTH, 0.f, 0.f);
    *(out.c++) =       LLColor4U::white;
    vertex_count++;

    *(out.idx++) = 4;
    index_count++;
    *(out.idx++) = 6;
    index_count++;
    *(out.idx++) = 5;
    index_count++;

    *(out.idx++) = 4;
    index_count++;
    *(out.idx++) = 5;
    index_count++;
    *(out.idx++) = 7;
    index_count++;


    // next leaf
    *(out.n++) =      LLVector3(SRR2, -SRR2, 0.f);
    *(out.t++) =   LLVector2(LEAF_LEFT, LEAF_BOTTOM);
    *(out.v++) =     LLVector3(0.f, -0.5f*LEAF_WIDTH, 0.f);
    *(out.c++) =       LLColor4U::white;
    vertex_count++;

    *(out.n++) =      LLVector3(SRR3, SRR3, SRR3);
    *(out.t++) =   LLVector2(LEAF_RIGHT, LEAF_TOP);
    *(out.v++) =     LLVector3(0.f, 0.5f*LEAF_WIDTH, 1.f);
    *(out.c++) =       LLColor4U::white;
    vertex_count++;

    *(out.n++) =      LLVector3(SRR3, -SRR3, SRR3);
    *(out.t++) =   LLVector2(LEAF_LEFT, LEAF_TOP);
    *(out.v++) =     LLVector3(0.f, -0.5f*LEAF_WIDTH, 1.f);
    *(out.c++) =       LLColor4U::white;
    vertex_count++;

    *(out.n++) =      LLVector3(SRR2, SRR2, 0.f);
    *(out.t++) =   LLVector2(LEAF_RIGHT, LEAF_BOTTOM);
    *(out.v++) =     LLVector3(0.f, 0.5f*LEAF_WIDTH, 0.f);
    *(out.c++) =       LLColor4U::white;
    vertex_count++;

    *(out.idx++) = 8;
    index_count++;
    *(out.idx++) = 9;
    index_count++;
    *(out.idx++) = 10;
    index_count++;

    *(out.idx++) = 8;
    index_count++;
    *(out.idx++) = 11;
    index_count++;
    *(out.idx++) = 9;
    index_count++;


    // other side of same leaf
    *(out.n++) =      LLVector3(-SRR2, -SRR2, 0.f);
    *(out.t++) =   LLVector2(LEAF_LEFT, LEAF_BOTTOM);
    *(out.v++) =     LLVector3(0.f, -0.5f*LEAF_WIDTH, 0.f);
    *(out.c++) =       LLColor4U::white;
    vertex_count++;

    *(out.n++) =      LLVector3(-SRR3, SRR3, SRR3);
    *(out.t++) =   LLVector2(LEAF_RIGHT, LEAF_TOP);
    *(out.v++) =     LLVector3(0.f, 0.5f*LEAF_WIDTH, 1.f);
    *(out.c++) =       LLColor4U::white;
    vertex_count++;

    *(out.n++) =      LLVector3(-SRR3, -SRR3, SRR3);
    *(out.t++) =   LLVector2(LEAF_LEFT, LEAF_TOP);
    *(out.v++) =     LLVector3(0.f, -0.5f*LEAF_WIDTH, 1.f);
    *(out.c++) =       LLColor4U::white;
    vertex_count++;

    *(out.n++) =      LLVector3(-SRR2, SRR2, 0.f);
    *(out.t++) =   LLVector2(LEAF_RIGHT, LEAF_BOTTOM);
    *(out.v++) =     LLVector3(0.f, 0.5f*LEAF_WIDTH, 0.f);
    *(out.c++) =       LLColor4U::white;
    vertex_count++;

    *(out.idx++) = 12;
    index_count++;
    *(out.idx++) = 14;
    index_count++;
    *(out.idx++) = 13;
    index_count++;

    *(out.idx++) = 12;
    index_count++;
    *(out.idx++) = 13;
    index_count++;
    *(out.idx++) = 15;
    index_count++;

    // Generate geometry for the cylinders

    // Different LOD's

    // Generate the vertices
    // Generate the indices

    for (lod = 0; lod < sMAX_NUM_TREE_LOD_LEVELS; lod++)
    {
        slices = sLODSlices[lod];
        F32 base_radius = 0.65f;
        F32 top_radius = base_radius * sp.mTaper;
        //LL_INFOS() << "Species " << ((U32) mSpecies) << ", taper = " << sSpeciesTable[mSpecies].mTaper << LL_ENDL;
        //LL_INFOS() << "Droop " << mDroop << ", branchlength: " << mBranchLength << LL_ENDL;
        F32 angle = 0;
        F32 angle_inc = 360.f/(slices-1);
        F32 z = 0.f;
        F32 z_inc = 1.f;
        if (slices > 3)
        {
            z_inc = 1.f/(slices - 3);
        }
        F32 radius = base_radius;

        F32 x1,y1;
        F32 noise_scale = sp.mNoiseMag;
        LLVector3 nvec;

        const F32 cap_nudge = 0.1f;         // Height to 'peak' the caps on top/bottom of branch

        const S32 fractal_depth = 5;
        F32 nvec_scale = 1.f * sp.mNoiseScale;
        F32 nvec_scalez = 4.f * sp.mNoiseScale;

        F32 tex_z_repeat = sp.mRepeatTrunkZ;

        F32 start_radius;
        F32 nangle = 0;
        F32 height = 1.f;
        F32 r0;

        for (i = 0; i < slices; i++)
        {
            if (i == 0)
            {
                z = - cap_nudge;
                r0 = 0.0;
            }
            else if (i == (slices - 1))
            {
                z = 1.f + cap_nudge;//((i - 2) * z_inc) + cap_nudge;
                r0 = 0.0;
            }
            else
            {
                z = (i - 1) * z_inc;
                r0 = base_radius + (top_radius - base_radius)*z;
            }

            for (j = 0; j < slices; j++)
            {
                if (slices - 1 == j)
                {
                    angle = 0.f;
                }
                else
                {
                    angle =  j*angle_inc;
                }

                nangle = angle;

                x1 = cos(angle * DEG_TO_RAD);
                y1 = sin(angle * DEG_TO_RAD);
                LLVector2 tc;
                // This isn't totally accurate.  Should compute based on slope as well.
                start_radius = r0 * (1.f + 1.2f*fabs(z - 0.66f*height)/height);
                nvec.set(   cos(nangle * DEG_TO_RAD)*start_radius*nvec_scale,
                            sin(nangle * DEG_TO_RAD)*start_radius*nvec_scale,
                            z*nvec_scalez);
                // First and last slice at 0 radius (to bring in top/bottom of structure)
                radius = start_radius + turbulence3((F32*)&nvec.mV, (F32)fractal_depth)*noise_scale;

                if (slices - 1 == j)
                {
                    // Not 0.5 for slight slop factor to avoid edges on leaves
                    tc = LLVector2(0.490f, (1.f - z/2.f)*tex_z_repeat);
                }
                else
                {
                    tc = LLVector2((angle/360.f)*0.5f, (1.f - z/2.f)*tex_z_repeat);
                }

                *(out.v++) =     LLVector3(x1*radius, y1*radius, z);
                *(out.n++) =      LLVector3(x1, y1, 0.f);
                *(out.t++) = tc;
                *(out.c++) =       LLColor4U::white;
                vertex_count++;
            }
        }

        for (i = 0; i < (slices - 1); i++)
        {
            for (j = 0; j < (slices - 1); j++)
            {
                S32 x1_offset = j+1;
                if ((j+1) == slices)
                {
                    x1_offset = 0;
                }
                // Generate the matching quads
                *(out.idx) = j + (i*slices) + sLODVertexOffset[lod];
                llassert(*(out.idx) < (U32)max_vertices);
                out.idx++;
                index_count++;
                *(out.idx) = x1_offset + ((i+1)*slices) + sLODVertexOffset[lod];
                llassert(*(out.idx) < (U32)max_vertices);
                out.idx++;
                index_count++;
                *(out.idx) = j + ((i+1)*slices) + sLODVertexOffset[lod];
                llassert(*(out.idx) < (U32)max_vertices);
                out.idx++;
                index_count++;

                *(out.idx) = j + (i*slices) + sLODVertexOffset[lod];
                llassert(*(out.idx) < (U32)max_vertices);
                out.idx++;
                index_count++;
                *(out.idx) = x1_offset + (i*slices) + sLODVertexOffset[lod];
                llassert(*(out.idx) < (U32)max_vertices);
                out.idx++;
                index_count++;
                *(out.idx) = x1_offset + ((i+1)*slices) + sLODVertexOffset[lod];
                llassert(*(out.idx) < (U32)max_vertices);
                out.idx++;
                index_count++;
            }
        }
        slices /= 2;
    }

    llassert(vertex_count == max_vertices);
    llassert(index_count == max_indices);
    (void)vertex_count;
    (void)index_count;
}

//static
void LLVOTree::appendMesh(TreeRef& ref,
                         LLStrider<LLVector3>& vertices,
                         LLStrider<LLVector3>& normals,
                         LLStrider<LLVector2>& tex_coords,
                         LLStrider<LLColor4U>& colors,
                         LLStrider<U16>& indices,
                         U16& cur_idx,
                         LLMatrix4& matrix,
                         LLMatrix4& norm_mat,
                         S32 vert_start,
                         S32 vert_count,
                         S32 index_count,
                         S32 index_offset)
{
    //copy/transform vertices into mesh - check
    for (S32 i = 0; i < vert_count; i++)
    {
        U16 index = vert_start + i;
        *vertices++ = ref.v[index] * matrix;
        LLVector3 norm = ref.n[index] * norm_mat;
        norm.normalize();
        *normals++ = norm;
        *tex_coords++ = ref.t[index];
        *colors++ = ref.c[index];
    }

    //copy offset indices into mesh - check
    for (S32 i = 0; i < index_count; i++)
    {
        U16 index = index_offset + i;
        *indices++ = ref.idx[index]-vert_start+cur_idx;
    }

    //increment index offset - check
    cur_idx += vert_count;
}


//static
void LLVOTree::genBranchPipeline(TreeRef& ref,
                                 const TreeSpeciesData& sp,
                                 LLStrider<LLVector3>& vertices,
                                 LLStrider<LLVector3>& normals,
                                 LLStrider<LLVector2>& tex_coords,
                                 LLStrider<LLColor4U>& colors,
                                 LLStrider<U16>& indices,
                                 U16& index_offset,
                                 LLMatrix4& matrix,
                                 S32 trunk_LOD,
                                 S32 stop_level,
                                 U16 depth,
                                 U16 trunk_depth,
                                 F32 scale,
                                 F32 twist,
                                 F32 droop,
                                 F32 branches,
                                 F32 alpha)
{
    //
    //  Generates a tree mesh by recursing, generating branches and then a 'leaf' texture.
    //  (Species parameters come from `sp`; the per-object copies LL kept -- mTrunkLength
    //  etc. -- are assigned from the same table in processUpdateMessage.)

    const F32 constant_twist = 360.f/branches;

    F32 length = ((trunk_depth || (scale == 1.f))? sp.mTrunkLength:sp.mBranchLength);
    F32 aspect = ((trunk_depth || (scale == 1.f))? sp.mTrunkAspect:sp.mBranchAspect);

    if (stop_level >= 0)
    {
        if (depth > stop_level)
        {
            {
                llassert(sLODIndexCount[trunk_LOD] > 0);
                const F32 width = scale * length * aspect;
                LLMatrix4 scale_mat;
                scale_mat.mMatrix[0][0] = width;
                scale_mat.mMatrix[1][1] = width;
                scale_mat.mMatrix[2][2] = scale*length;
                scale_mat *= matrix;

                glm::mat4 norm(glm::make_mat4((F32*) scale_mat.mMatrix));
                LLMatrix4 norm_mat = LLMatrix4(glm::value_ptr(glm::transpose(glm::inverse(norm))));

                appendMesh(ref, vertices, normals, tex_coords, colors, indices, index_offset, scale_mat, norm_mat,
                            sLODVertexOffset[trunk_LOD], sLODVertexCount[trunk_LOD], sLODIndexCount[trunk_LOD], sLODIndexOffset[trunk_LOD]);
            }

            // Recurse to create more branches
            for (S32 i=0; i < (S32)branches; i++)
            {
                LLMatrix4 trans_mat;
                trans_mat.setTranslation(0,0,scale*length);
                trans_mat *= matrix;

                LLQuaternion rot =
                    LLQuaternion(20.f*DEG_TO_RAD, LLVector4(0.f, 0.f, 1.f)) *
                    LLQuaternion(droop*DEG_TO_RAD, LLVector4(0.f, 1.f, 0.f)) *
                    LLQuaternion(((constant_twist + ((i%2==0)?twist:-twist))*i)*DEG_TO_RAD, LLVector4(0.f, 0.f, 1.f));

                LLMatrix4 rot_mat(rot);
                rot_mat *= trans_mat;

                genBranchPipeline(ref, sp, vertices, normals, tex_coords, colors, indices, index_offset, rot_mat, trunk_LOD, stop_level, depth - 1, 0, scale*sp.mScaleStep, twist, droop, branches, alpha);
            }
            //  Recurse to continue trunk
            if (trunk_depth)
            {
                LLMatrix4 trans_mat;
                trans_mat.setTranslation(0,0,scale*length);
                trans_mat *= matrix;

                LLMatrix4 rot_mat(70.5f*DEG_TO_RAD, LLVector4(0,0,1));
                rot_mat *= trans_mat; // rotate a bit around Z when ascending
                genBranchPipeline(ref, sp, vertices, normals, tex_coords, colors, indices, index_offset, rot_mat, trunk_LOD, stop_level, depth, trunk_depth-1, scale*sp.mScaleStep, twist, droop, branches, alpha);
            }
        }
        else
        {
            //
            //  Append leaves as two 90 deg crossed quads with leaf textures
            //
            {
                LLMatrix4 scale_mat;
                scale_mat.mMatrix[0][0] =
                    scale_mat.mMatrix[1][1] =
                    scale_mat.mMatrix[2][2] = scale*sp.mLeafScale;

                scale_mat *= matrix;

                glm::mat4 norm(glm::make_mat4((F32*)scale_mat.mMatrix));
                LLMatrix4 norm_mat = LLMatrix4(glm::value_ptr(glm::transpose(glm::inverse(norm))));

                appendMesh(ref, vertices, normals, tex_coords, colors, indices, index_offset, scale_mat, norm_mat, 0, LEAF_VERTICES, LEAF_INDICES, 0);
            }
        }
    }
}



//static
void LLVOTree::calcNumVerts(U32& vert_count, U32& index_count, S32 trunk_LOD, S32 stop_level, U16 depth, U16 trunk_depth, F32 branches)
{
    if (stop_level >= 0)
    {
        if (depth > stop_level)
        {
            index_count += sLODIndexCount[trunk_LOD];
            vert_count += sLODVertexCount[trunk_LOD];

            // Recurse to create more branches
            for (S32 i=0; i < (S32)branches; i++)
            {
                calcNumVerts(vert_count, index_count, trunk_LOD, stop_level, depth - 1, 0, branches);
            }

            //  Recurse to continue trunk
            if (trunk_depth)
            {
                calcNumVerts(vert_count, index_count, trunk_LOD, stop_level, depth, trunk_depth-1, branches);
            }
        }
        else
        {
            index_count += LEAF_INDICES;
            vert_count += LEAF_VERTICES;
        }
    }
    else
    {
        index_count += LEAF_INDICES;
        vert_count += LEAF_VERTICES;
    }
}

//static
const LLVOTree::TreeSpeciesData* LLVOTree::getSpeciesData(S32 species)
{
    SpeciesMap::const_iterator it = sSpeciesTable.find((U32)species);
    return it == sSpeciesTable.end() ? nullptr : it->second;
}

//static
bool LLVOTree::buildLocalMesh(S32 species, S32 trunk_LOD,
                              std::vector<LLVector3>& pos, std::vector<LLVector3>& nrm,
                              std::vector<LLVector2>& uv, std::vector<U16>& idx)
{
    const TreeSpeciesData* sp = getSpeciesData(species);
    if (!sp || trunk_LOD < 0 || trunk_LOD >= sMAX_NUM_TREE_LOD_LEVELS) return false;

    // Reference geometry into plain arrays (LLStrider over raw pointers).
    S32 ref_nv = 0, ref_ni = 0;
    initLODTables(ref_nv, ref_ni);
    std::vector<LLVector3> rv(ref_nv), rn(ref_nv);
    std::vector<LLVector2> rt(ref_nv);
    std::vector<LLColor4U> rc(ref_nv);
    std::vector<U16>       ri(ref_ni);
    {
        TreeRef gen;
        gen.v = rv.data(); gen.n = rn.data(); gen.t = rt.data(); gen.c = rc.data(); gen.idx = ri.data();
        genReferenceGeometry(*sp, gen);
    }

    // updateMesh with an identity root (the per-tree matrix goes in the instance) and
    // no trunk bend (mTrunkBend is never written: droop = mDroop + 25).
    U32 nv = 0, ni = 0;
    calcNumVerts(nv, ni, trunk_LOD, 0, sp->mDepth, sp->mTrunkDepth, sp->mBranches);
    if (nv == 0 || ni == 0 || nv > 65535u)
    {
        LL_WARNS_ONCE("VKTree") << "species " << species << " LOD " << trunk_LOD << ": " << nv
                                << " vertices do not fit U16 indices" << LL_ENDL;
        return false;
    }
    pos.resize(nv); nrm.resize(nv); uv.resize(nv); idx.resize(ni);
    std::vector<LLColor4U> col(nv);
    TreeRef ref;
    ref.v = rv.data(); ref.n = rn.data(); ref.t = rt.data(); ref.c = rc.data(); ref.idx = ri.data();
    LLStrider<LLVector3> vs(pos.data()), ns(nrm.data());
    LLStrider<LLVector2> ts(uv.data());
    LLStrider<LLColor4U> cs(col.data());
    LLStrider<U16>       is(idx.data());
    U16 idx_offset = 0;
    LLMatrix4 root;   // identity
    genBranchPipeline(ref, *sp, vs, ns, ts, cs, is, idx_offset, root, trunk_LOD, 0,
                      sp->mDepth, sp->mTrunkDepth, 1.f, sp->mTwist, sp->mDroop + 25.f, sp->mBranches, 1.f);
    return true;
}

LLMatrix4 LLVOTree::treeLocalToAgent() const
{
    // updateMesh's chain (scale -> Z90 * rotation -> base translate), with the base in
    // AGENT space instead of LL's region-space drawable. No trunk bend (see above).
    const LLVector3 base = getPositionAgent();
    LLMatrix4 trans_mat;
    trans_mat.setTranslation(base.mV[VX], base.mV[VY], base.mV[VZ] - 0.1f);
    LLMatrix4 rot_mat(LLQuaternion(90.f*DEG_TO_RAD, LLVector4(0,0,1)) * getRotation());
    rot_mat *= trans_mat;
    LLMatrix4 m;
    m.mMatrix[0][0] = m.mMatrix[1][1] = m.mMatrix[2][2] = getScale().magVec()*0.05f;
    m *= rot_mat;
    return m;
}

void LLVOTree::vkLODParams(F32 range_lt[3], F32& detail_k, F32& radius) const
{
    // idleUpdate: trunk_LOD = first j with app_angle * sTreeFactor > sLODAngles[j], where
    // app_angle = atan2(getMaxScale(), distance - radius) in degrees (setPixelAreaAndAngle)
    // -- i.e. (distance - radius) < getMaxScale() / tan(sLODAngles[j] / sTreeFactor).
    // sLODAngles[3] ~ 0 makes LOD 3 the floor. Then trunk_LOD = max(trunk_LOD,
    // 3 - getDetailFromTan(sTreeFactor * 64 * sz / (distance * sDistanceFactor * PI/3))).
    const F32 f = llmax(sTreeFactor, 0.0001f);
    const F32 max_scale = getMaxScale();
    radius = getScale().length() * 0.5f;
    for (S32 j = 0; j < 3; ++j)
    {
        const F32 a = sLODAngles[j] / f;   // degrees
        range_lt[j] = (a >= 89.9f) ? 0.f : max_scale / tanf(a * DEG_TO_RAD);
    }
    // Billboard params from the species table: the per-object copies are only set by a
    // full (non-terse) update.
    const TreeSpeciesData* sp = getSpeciesData(mSpecies);
    const F32 sz = sp ? sp->mBillboardScale * sp->mBillboardRatio * getScale().length() : 0.f;
    detail_k = (sTreeFactor * 64.f * sz) / llmax(LLVOVolume::sDistanceFactor * (F_PI / 3.f), 0.0001f);
}

void LLVOTree::updateRadius()
{
    if (mDrawable.isNull())
    {
        return;
    }

    mDrawable->setRadius(32.0f);
}

void LLVOTree::updateSpatialExtents(LLVector4a& newMin, LLVector4a& newMax)
{
    F32 radius = getScale().length()*0.05f;
    LLVector3 center = getRenderPosition();

    F32 sz = mBillboardScale*mBillboardRatio*radius*0.5f;
    LLVector3 size(sz,sz,sz);

    center += LLVector3(0, 0, size.mV[2]) * getRotation();

    newMin.load3((center-size).mV);
    newMax.load3((center+size).mV);
    LLVector4a pos;
    pos.load3(center.mV);
    mDrawable->setPositionGroup(pos);

    if (mDrawable->getNumFaces() > 0)
    {
        LLFace* facep = mDrawable->getFace(0);
        if (facep)
        {
            facep->mExtents[0] = newMin;
            facep->mExtents[1] = newMax;
        }
    }
}

U32 LLVOTree::getPartitionType() const
{
    return LLViewerRegion::PARTITION_TREE;
}

