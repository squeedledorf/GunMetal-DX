/**
 * @file llvotree.h
 * @brief LLVOTree class header file
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

#ifndef LL_LLVOTREE_H
#define LL_LLVOTREE_H

#include "llviewerobject.h"
#include "xform.h"

class LLFace;
class LLDrawPool;
class LLViewerFetchedTexture;

class LLVOTree : public LLViewerObject
{
protected:
    ~LLVOTree();

public:
    LLVOTree(const LLUUID &id, const LLPCode pcode, LLViewerRegion *regionp);

    // Initialize data that's only inited once per class.
    static void initClass();
    static void cleanupClass();
    static bool isTreeRenderingStopped();

    /*virtual*/ U32 processUpdateMessage(LLMessageSystem *mesgsys,
                                            void **user_data,
                                            U32 block_num, const EObjectUpdateType update_type,
                                            LLDataPacker *dp);
    /*virtual*/ bool isActive() const; // always false: clean-pipeline ingested

    // Graphical stuff for objects - maybe broken out into render class later?
    /*virtual*/ void render(LLAgent &agent);
    /*virtual*/ void setPixelAreaAndAngle(LLAgent &agent);
    /*virtual*/ void updateTextures();

    /*virtual*/ LLDrawable* createDrawable(LLPipeline *pipeline);
    /*virtual*/ void        updateSpatialExtents(LLVector4a &min, LLVector4a &max);

    virtual U32 getPartitionType() const;

    void updateRadius();

    static void calcNumVerts(U32& vert_count, U32& index_count, S32 trunk_LOD, S32 stop_level, U16 depth, U16 trunk_depth, F32 branches);

    // Reference geometry (the two crossed leaf quads, then one unit cylinder per trunk
    // LOD) the recursive generator instances. Depends only on the species.
    struct TreeRef
    {
        LLStrider<LLVector3> v;
        LLStrider<LLVector3> n;
        LLStrider<LLVector2> t;
        LLStrider<LLColor4U> c;
        LLStrider<U16>       idx;
    };
    struct TreeSpeciesData;
    // sLOD* offset/count tables for the reference geometry (species independent).
    static void initLODTables(S32& max_vertices, S32& max_indices);
    static void genReferenceGeometry(const TreeSpeciesData& sp, TreeRef& out);

    static void appendMesh(TreeRef& ref,
                         LLStrider<LLVector3>& vertices,
                         LLStrider<LLVector3>& normals,
                         LLStrider<LLVector2>& tex_coords,
                         LLStrider<LLColor4U>& colors,
                         LLStrider<U16>& indices,
                         U16& idx_offset,
                         LLMatrix4& matrix,
                         LLMatrix4& norm_mat,
                         S32 vertex_offset,
                         S32 vertex_count,
                         S32 index_count,
                         S32 index_offset);

    static void genBranchPipeline(TreeRef& ref,
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
                                 F32 alpha);

    // VULKAN (#20): one trunk LOD (0 = best .. 3) of a species' mesh in TREE-LOCAL
    // space -- LL's updateMesh chain with an identity root, i.e. before the per-tree
    // scale (getScale().magVec()*0.05), Z-90deg*rotation and base translation that
    // treeLocalToAgent() supplies. False if the species is unknown or the mesh would
    // not fit U16 indices.
    static bool buildLocalMesh(S32 species, S32 trunk_LOD,
                               std::vector<LLVector3>& pos, std::vector<LLVector3>& nrm,
                               std::vector<LLVector2>& uv, std::vector<U16>& idx);
    // The per-tree transform LL bakes into its mesh (updateMesh), in AGENT space.
    LLMatrix4 treeLocalToAgent() const;
    // LL's trunk-LOD selection (idleUpdate / setPixelAreaAndAngle) as range thresholds:
    // trunk LOD j applies while (distance - radius) < range_lt[j] (j = 0..2, else 3),
    // capped by detailFromTan(detail_k / distance). radius = getScale().length()*0.5.
    void vkLODParams(F32 range_lt[3], F32& detail_k, F32& radius) const;
    S32 getSpecies() const { return mSpecies; }
    static const TreeSpeciesData* getSpeciesData(S32 species);


    static S32 sMaxTreeSpecies;

    struct TreeSpeciesData
    {
        LLUUID mTextureID;

        F32             mBranchLength;  // Scale (length) of tree branches
        F32             mDroop;         // Droop from vertical (degrees) at each branch recursion
        F32             mTwist;         // Twist
        F32             mBranches;      // Number of branches emitted at each recursion level
        U8              mDepth;         // Number of recursions to tips of branches
        F32             mScaleStep;     // Multiplier for scale at each recursion level
        U8              mTrunkDepth;

        F32             mLeafScale;     // Scales leaf texture when rendering
        F32             mTrunkLength;   // Scales branch diameters when rendering
        F32             mBillboardScale; // Scales the billboard representation
        F32             mBillboardRatio; // Height to width aspect ratio
        F32             mTrunkAspect;
        F32             mBranchAspect;
        F32             mRandomLeafRotate;
        F32             mNoiseScale;    //  Scaling of noise function in perlin space (norm = 1.0)
        F32             mNoiseMag;      //  amount of perlin noise to deform by (0 = none)
        F32             mTaper;         //  amount of perlin noise to deform by (0 = none)
        F32             mRepeatTrunkZ;  //  Times to repeat the trunk texture vertically along trunk
    };

    static F32 sTreeFactor;         // Tree level of detail factor
    static const S32 sMAX_NUM_TREE_LOD_LEVELS ;

protected:
    LLVector3       mTrunkBend;     // Accumulated wind (used for blowing trees)
    LLVector3       mWind;

    LLPointer<LLViewerFetchedTexture> mTreeImagep;  // Pointer to proper tree image

    U8              mSpecies;       // Species of tree
    F32             mBranchLength;  // Scale (length) of tree branches
    F32             mTrunkLength;   // Trunk length (first recursion)
    F32             mDroop;         // Droop from vertical (degrees) at each branch recursion
    F32             mTwist;         // Twist
    F32             mBranches;      // Number of branches emitted at each recursion level
    U8              mDepth;         // Number of recursions to tips of branches
    F32             mScaleStep;     // Multiplier for scale at each recursion level
    U8              mTrunkDepth;
    U32             mTrunkLOD;
    F32             mLeafScale;     // Scales leaf texture when rendering

    F32             mBillboardScale;    //  How big to draw the billboard?
    F32             mBillboardRatio;    //  Height to width ratio of billboard
    F32             mTrunkAspect;       //  Ratio between width/length of trunk
    F32             mBranchAspect;  //  Ratio between width/length of branch
    F32             mRandomLeafRotate;  //  How much to randomly rotate leaves about arbitrary axis

    U32 mFrameCount;

    typedef std::map<U32, TreeSpeciesData*> SpeciesMap;
    static SpeciesMap sSpeciesTable;

    static S32 sLODIndexOffset[4];
    static S32 sLODIndexCount[4];
    static S32 sLODVertexOffset[4];
    static S32 sLODVertexCount[4];
    static S32 sLODSlices[4];
    static F32 sLODAngles[4];
};

#endif
