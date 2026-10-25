/**
 * @file llface.cpp
 * @brief LLFace class implementation
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


#include "lldrawable.h" // lldrawable needs to be included before llface
#include "llface.h"
#include "llviewertextureanim.h"

#include "llviewercontrol.h"
#include "llvolume.h"
#include "m3math.h"
#include "llmatrix4a.h"
#include "v3color.h"

#include "lldefs.h"

#include "llgl.h"
#include "llrender.h"
#include "lllightconstants.h"
#include "llsky.h"
#include "llviewercamera.h"
#include "llviewertexturelist.h"
#include "llvopartgroup.h"
#include "llvovolume.h"
#include "llvolumemgr.h"        // LLVolumeLODGroup (clean-slot LOD key, Phase 2b)
#include "llcleanmeshasset.h"   // selection re-home onto the clean mega-VBO (Phase 2b)
#include "llcleaninstancestore.h" // DIAG: dump selected object's clean instance
#include "llcleancursorpick.h"    // #585 / #704 drawn face frames (GPU query; released LL arrays)
#include "pipeline.h"
#include "llviewerregion.h"
#include "llviewerwindow.h"
#include "llrender.h"
#include "llviewertexture.h"
#include "llvoavatar.h"
#include "llmeshrepository.h"
#include "llskinningutil.h"
// [RLVa:KB] - Checked: RLVa-2.0.0
#include "rlvhandler.h"
// [/RLVa:KB]

#if LL_LINUX
// Work-around spurious used before init warning on Vector4a
//
#pragma GCC diagnostic ignored "-Wuninitialized"
#endif

#define LL_MAX_INDICES_COUNT 1000000

bool LLFace::sSafeRenderSelect = true; // false


#define DOTVEC(a,b) (a.mV[0]*b.mV[0] + a.mV[1]*b.mV[1] + a.mV[2]*b.mV[2])

/*
For each vertex, given:
    B - binormal
    T - tangent
    N - normal
    P - position

The resulting texture coordinate <u,v> is:

    u = 2(B dot P)
    v = 2(T dot P)
*/
void planarProjection(LLVector2 &tc, const LLVector4a& normal,
                      const LLVector4a &center, const LLVector4a& vec)
{
    LLVector4a binormal;
    F32 d = normal[0];

    if (d >= 0.5f || d <= -0.5f)
    {
        if (d < 0)
        {
            binormal.set(0,-1,0);
        }
        else
        {
            binormal.set(0, 1, 0);
        }
    }
    else
    {
        if (normal[1] > 0)
        {
            binormal.set(-1,0,0);
        }
        else
        {
            binormal.set(1,0,0);
        }
    }
    LLVector4a tangent;
    tangent.setCross3(binormal,normal);

    tc.mV[1] = -((tangent.dot3(vec).getF32())*2 - 0.5f);
    tc.mV[0] = 1.0f+((binormal.dot3(vec).getF32())*2 - 0.5f);
}

////////////////////
//
// LLFace implementation
//

void LLFace::init(LLDrawable* drawablep, LLViewerObject* objp)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_FACE;
    mLastUpdateTime = gFrameTimeSeconds;
    mLastMoveTime = 0.f;
    mLastSkinTime = gFrameTimeSeconds;
    mVSize = 0.f;
    mPixelArea = 16.f;
    mState      = GLOBAL;
    mDrawPoolp  = NULL;
    mPoolType = 0;
    mCenterLocal = objp->getPosition();
    mCenterAgent = drawablep->getPositionAgent();
    mDistance   = 0.f;

    mGeomCount      = 0;
    mGeomIndex      = 0;
    mIndicesCount   = 0;

    //special value to indicate uninitialized position
    mIndicesIndex   = 0xFFFFFFFF;

    for (U32 i = 0; i < LLRender::NUM_TEXTURE_CHANNELS; ++i)
    {
        mIndexInTex[i] = 0;
        mTexture[i] = NULL;
    }

    mTEOffset       = -1;
    mTextureIndex = FACE_DO_NOT_BATCH_TEXTURES;

    setDrawable(drawablep);
    mVObjp = objp;

    mReferenceIndex = -1;

    mTextureMatrix = NULL;

    mFaceColor = LLColor4(1,0,0,1);

    mImportanceToCamera = 0.f ;
    mBoundingSphereRadius = 0.0f ;

    mTexExtents[0].set(0, 0);
    mTexExtents[1].set(1, 1);
    mHasMedia = false ;
    mIsMediaAllowed = true;

// [SL:KB] - Patch: Render-TextureToggle (Catznip-4.0)
    mShowDiffTexture = true;
// [/SL:KB]
}

void LLFace::destroy()
{
    for (U32 i = 0; i < LLRender::NUM_TEXTURE_CHANNELS; ++i)
    {
        if(mTexture[i].notNull())
        {
            mTexture[i]->removeFace(i, this) ;
            mTexture[i] = NULL;
        }
    }

    if (isState(LLFace::PARTICLE))
    {
        clearState(LLFace::PARTICLE);
    }

    if (mDrawPoolp)
    {
        mDrawPoolp->removeFace(this);
        mDrawPoolp = NULL;
    }

    if (mTextureMatrix)
    {
        delete mTextureMatrix;
        mTextureMatrix = NULL;
    }

    mDrawablep = NULL;
    mVObjp = NULL;
}

void LLFace::setWorldMatrix(const LLMatrix4 &mat)
{
    LL_ERRS() << "Faces on this drawable are not independently modifiable\n" << LL_ENDL;
}

void LLFace::setPool(LLFacePool* pool)
{
    mDrawPoolp = pool;
}

void LLFace::setPool(LLFacePool* new_pool, LLViewerTexture *texturep)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_FACE;

    if (!new_pool)
    {
        LL_ERRS() << "Setting pool to null!" << LL_ENDL;
    }

    if (new_pool != mDrawPoolp)
    {
        // Remove from old pool
        if (mDrawPoolp)
        {
            mDrawPoolp->removeFace(this);

            if (mDrawablep)
            {
                gPipeline.markRebuild(mDrawablep, LLDrawable::REBUILD_ALL);
            }
        }
        mGeomIndex = 0;

        // Add to new pool
        if (new_pool)
        {
            new_pool->addFace(this);
        }
        mDrawPoolp = new_pool;
    }

    setTexture(texturep) ;
}

void LLFace::setTexture(U32 ch, LLViewerTexture* tex)
{
    llassert(ch < LLRender::NUM_TEXTURE_CHANNELS);

    if(mTexture[ch] == tex)
    {
        return ;
    }

// [SL:KB] - Patch: Render-TextureToggle (Catznip-4.0)
    if ( (LLRender::DIFFUSE_MAP == ch) && (!mShowDiffTexture) )
    {
        mOrigDiffTexture = tex;
        if (LLViewerFetchedTexture::sDefaultDiffuseImagep.get() == mTexture[ch].get())
            return;
    }
// [/SL:KB]

    if(mTexture[ch].notNull())
    {
        mTexture[ch]->removeFace(ch, this) ;
    }

    if(tex)
    {
        tex->addFace(ch, this) ;
    }

    mTexture[ch] = tex ;
}

void LLFace::setTexture(LLViewerTexture* tex)
{
    setDiffuseMap(tex);
}

void LLFace::setDiffuseMap(LLViewerTexture* tex)
{
    setTexture(LLRender::DIFFUSE_MAP, tex);
}

void LLFace::setAlternateDiffuseMap(LLViewerTexture* tex)
{
    setTexture(LLRender::ALTERNATE_DIFFUSE_MAP, tex);
}

void LLFace::setNormalMap(LLViewerTexture* tex)
{
    setTexture(LLRender::NORMAL_MAP, tex);
}

void LLFace::setSpecularMap(LLViewerTexture* tex)
{
    setTexture(LLRender::SPECULAR_MAP, tex);
}

void LLFace::dirtyTexture()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_FACE;

    LLDrawable* drawablep = getDrawable();

    if (mVObjp.notNull() && mVObjp->getVolume())
    {
        for (U32 ch = 0; ch < LLRender::NUM_TEXTURE_CHANNELS; ++ch)
        {
            if (mTexture[ch].notNull() && mTexture[ch]->getComponents() == 4)
            { //dirty texture on an alpha object should be treated as an LoD update
                LLVOVolume* vobj = drawablep->getVOVolume();
                if (vobj)
                {
                    vobj->mLODChanged = true;

                    vobj->updateVisualComplexity();
                }
                gPipeline.markRebuild(drawablep, LLDrawable::REBUILD_VOLUME);
            }
        }
    }

    gPipeline.markTextured(drawablep);
}

void LLFace::switchTexture(U32 ch, LLViewerTexture* new_texture)
{
    llassert(ch < LLRender::NUM_TEXTURE_CHANNELS);

    if(mTexture[ch] == new_texture)
    {
        return ;
    }

    if(!new_texture)
    {
        LL_ERRS() << "Can not switch to a null texture." << LL_ENDL;
        return;
    }

    if (ch == LLRender::DIFFUSE_MAP)
    {
        getViewerObject()->changeTEImage(mTEOffset, new_texture) ;
    }

    setTexture(ch, new_texture) ;
    dirtyTexture();
}

void LLFace::setTEOffset(const S32 te_offset)
{
    mTEOffset = te_offset;
}


void LLFace::setFaceColor(const LLColor4& color)
{
    mFaceColor = color;
    setState(USE_FACE_COLOR);
}

void LLFace::unsetFaceColor()
{
    clearState(USE_FACE_COLOR);
}

void LLFace::setDrawable(LLDrawable *drawable)
{
    mDrawablep  = drawable;
    mXform      = &drawable->mXform;
}

void LLFace::setSize(S32 num_vertices, S32 num_indices, bool align)
{
    if (align)
    {
        //allocate vertices in blocks of 4 for alignment
        num_vertices = (num_vertices + 0x3) & ~0x3;
    }

    if (mGeomCount != num_vertices ||
        mIndicesCount != num_indices)
    {
        mGeomCount    = num_vertices;
        mIndicesCount = num_indices;
    }
}

void LLFace::setGeomIndex(U32 idx)
{
    if (mGeomIndex != idx)
    {
        mGeomIndex = idx;
    }
}

void LLFace::setTextureIndex(U8 index)
{
    if (index != mTextureIndex)
    {
        mTextureIndex = index;

        if (mTextureIndex != FACE_DO_NOT_BATCH_TEXTURES)
        {
            mDrawablep->setState(LLDrawable::REBUILD_POSITION);
        }
    }
}

void LLFace::setIndicesIndex(S32 idx)
{
    if (mIndicesIndex != idx)
    {
        mIndicesIndex = idx;
    }
}

//============================================================================

void LLFace::updateCenterAgent()
{
    if (mDrawablep->isActive())
    {
        mCenterAgent = mCenterLocal * getRenderMatrix();
    }
    else
    {
        mCenterAgent = mCenterLocal;
    }
}

void LLFace::printDebugInfo() const
{
    LLFacePool *poolp = getPool();
    LL_INFOS() << "Object: " << getViewerObject()->mID << LL_ENDL;
    if (getDrawable())
    {
        LL_INFOS() << "Type: " << LLPrimitive::pCodeToString(getDrawable()->getVObj()->getPCode()) << LL_ENDL;
    }
    if (getTexture())
    {
        LL_INFOS() << "Texture: " << getTexture() << " Comps: " << (U32)getTexture()->getComponents() << LL_ENDL;
    }
    else
    {
        LL_INFOS() << "No texture: " << LL_ENDL;
    }

    LL_INFOS() << "Face: " << this << LL_ENDL;
    LL_INFOS() << "State: " << getState() << LL_ENDL;
    LL_INFOS() << "Geom Index Data:" << LL_ENDL;
    LL_INFOS() << "--------------------" << LL_ENDL;
    LL_INFOS() << "GI: " << mGeomIndex << " Count:" << mGeomCount << LL_ENDL;
    LL_INFOS() << "Face Index Data:" << LL_ENDL;
    LL_INFOS() << "--------------------" << LL_ENDL;
    LL_INFOS() << "II: " << mIndicesIndex << " Count:" << mIndicesCount << LL_ENDL;
    LL_INFOS() << LL_ENDL;

    if (poolp)
    {
        poolp->printDebugInfo();

        S32 pool_references = 0;
        for (std::vector<LLFace*>::iterator iter = poolp->mReferences.begin();
             iter != poolp->mReferences.end(); iter++)
        {
            LLFace *facep = *iter;
            if (facep == this)
            {
                LL_INFOS() << "Pool reference: " << pool_references << LL_ENDL;
                pool_references++;
            }
        }

        if (pool_references != 1)
        {
            LL_INFOS() << "Incorrect number of pool references!" << LL_ENDL;
        }
    }

#if 0
    LL_INFOS() << "Indices:" << LL_ENDL;
    LL_INFOS() << "--------------------" << LL_ENDL;

    const U32 *indicesp = getRawIndices();
    S32 indices_count = getIndicesCount();
    S32 geom_start = getGeomStart();

    for (S32 i = 0; i < indices_count; i++)
    {
        LL_INFOS() << i << ":" << indicesp[i] << ":" << (S32)(indicesp[i] - geom_start) << LL_ENDL;
    }
    LL_INFOS() << LL_ENDL;

    LL_INFOS() << "Vertices:" << LL_ENDL;
    LL_INFOS() << "--------------------" << LL_ENDL;
    for (S32 i = 0; i < mGeomCount; i++)
    {
        LL_INFOS() << mGeomIndex + i << ":" << poolp->getVertex(mGeomIndex + i) << LL_ENDL;
    }
    LL_INFOS() << LL_ENDL;
#endif
}

// Transform the texture coordinates for this face.
static void xform(LLVector2 &tex_coord, F32 cosAng, F32 sinAng, F32 offS, F32 offT, F32 magS, F32 magT)
{
    // New, good way
    F32 s = tex_coord.mV[0];
    F32 t = tex_coord.mV[1];

    // Texture transforms are done about the center of the face.
    s -= 0.5;
    t -= 0.5;

    // Handle rotation
    F32 temp = s;
    s  = s     * cosAng + t * sinAng;
    t  = -temp * sinAng + t * cosAng;

    // Then scale
    s *= magS;
    t *= magT;

    // Then offset
    s += offS + 0.5f;
    t += offT + 0.5f;

    tex_coord.mV[0] = s;
    tex_coord.mV[1] = t;
}


bool less_than_max_mag(const LLVector4a& vec)
{
    LLVector4a MAX_MAG;
    MAX_MAG.splat(1024.f*1024.f);

    LLVector4a val;
    val.setAbs(vec);

    S32 lt = val.lessThan(MAX_MAG).getGatheredBits() & 0x7;

    return lt == 0x7;
}

bool LLFace::genVolumeBBoxes(const LLVolume &volume, S32 f,
                             const LLMatrix4& mat_vert_in, bool global_volume)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_FACE;

    //get bounding box
    if (mDrawablep->isState(LLDrawable::REBUILD_VOLUME | LLDrawable::REBUILD_POSITION | LLDrawable::REBUILD_RIGGED))
    {
        if (f >= volume.getNumVolumeFaces())
        {
            LL_WARNS() << "Generating bounding box for invalid face index!" << LL_ENDL;
            f = 0;
        }

        const LLVolumeFace &face = volume.getVolumeFace(f);

        // MAINT-8264 - stray vertices, especially in low LODs, cause bounding box errors.
        if (face.mNumVertices < 3)
        {
            LL_DEBUGS("RiggedBox") << "skipping face " << f << ", bad num vertices "
                                   << face.mNumVertices << " " << face.mNumIndices << " " << face.mWeights << LL_ENDL;
            return false;
        }

        //VECTORIZE THIS
        LLMatrix4a mat_vert;
        mat_vert.loadu(mat_vert_in);

        llassert(less_than_max_mag(face.mExtents[0]));
        llassert(less_than_max_mag(face.mExtents[1]));

        matMulBoundBox(mat_vert, face.mExtents, mExtents);

        if (!mDrawablep->isActive())
        {   // Shift position for region
            LLVector4a offset;
            offset.load3(mDrawablep->getRegion()->getOriginAgent().mV);
            mExtents[0].add(offset);
            mExtents[1].add(offset);
        }

        LLVector4a t;
        t.setAdd(mExtents[0],mExtents[1]);
        t.mul(0.5f);

        mCenterLocal.set(t.getF32ptr());

        t.setSub(mExtents[1],mExtents[0]);
        mBoundingSphereRadius = t.getLength3().getF32()*0.5f;

        updateCenterAgent();
    }

    return true;
}



// convert surface coordinates to texture coordinates, based on
// the values in the texture entry.  probably should be
// integrated with getGeometryVolume() for its texture coordinate
// generation - but i'll leave that to someone more familiar
// with the implications.
LLVector2 LLFace::surfaceToTexture(LLVector2 surface_coord, const LLVector4a& position, const LLVector4a& normal)
{
    LLVector2 tc = surface_coord;

    const LLTextureEntry *tep = getTextureEntry();

    if (tep == NULL)
    {
        // can't do much without the texture entry
        return surface_coord;
    }

    //VECTORIZE THIS
    // see if we have a non-default mapping
    U8 texgen = tep->getTexGen();
    if (texgen != LLTextureEntry::TEX_GEN_DEFAULT)
    {
        LLVector4a& center = *(mDrawablep->getVOVolume()->getVolume()->getVolumeFace(mTEOffset).mCenter);

        LLVector4a volume_position;
        LLVector3 v_position(position.getF32ptr());
        LLVector3 v_normal(normal.getF32ptr());
        LLVector3 vol_pos, vol_nrm;
        const LLVOVolume* vobj = mDrawablep->getVOVolume();
        if (!(vobj->isAttachment()
              && vkAttachmentAgentToVolume(vobj, v_position, v_normal, vol_pos, vol_nrm)))
        {
            vol_pos = vobj->agentPositionToVolume(v_position);
            vol_nrm = vobj->agentDirectionToVolume(v_normal);
        }

        volume_position.load3(vol_pos.mV);

        if (!mDrawablep->getVOVolume()->isVolumeGlobal())
        {
            LLVector4a scale;
            scale.load3(mVObjp->getScale().mV);
            volume_position.mul(scale);
        }

        LLVector4a volume_normal;
        volume_normal.load3(vol_nrm.mV);
        volume_normal.normalize3fast();

        if (texgen == LLTextureEntry::TEX_GEN_PLANAR)
        {
            planarProjection(tc, volume_normal, center, volume_position);
        }
    }

    if (mTextureMatrix) // if we have a texture matrix, use it
    {
        LLVector3 tc3(tc);
        tc3 = tc3 * *mTextureMatrix;
        tc = LLVector2(tc3);
    }

    else // otherwise use the texture entry parameters
    {
        xform(tc, cos(tep->getRotation()), sin(tep->getRotation()),
              tep->mOffsetS, tep->mOffsetT, tep->mScaleS, tep->mScaleT);
    }


    return tc;
}

// Returns scale compared to default texgen, and face orientation as calculated
// by planarProjection(). This is needed to match planar texgen parameters.
void LLFace::getPlanarProjectedParams(LLQuaternion* face_rot, LLVector3* face_pos, F32* scale) const
{
    const LLMatrix4& vol_mat = getWorldMatrix();
    const LLVolumeFace& vf = getViewerObject()->getVolume()->getVolumeFace(mTEOffset);
    // The face's first vertex frame: LL's arrays, else (#585: released) the face as the
    // renderer draws it (#704: a GPU query of its drawn slot -- the same asset vertex
    // order, at the drawn level).
    LLVector4a normal4a, tangent;
    if (vf.mNormals && vf.mTangents)
    {
        normal4a = *vf.mNormals;
        tangent  = *vf.mTangents;
    }
    else if (!LLCleanCursorPick::drawnFaceFrame(getViewerObject(), mTEOffset, nullptr, &normal4a, &tangent))
    {
        if (vf.isGeometryReleased())
        {
            LLMeshRepository::noteReleasedRead(LLMeshRepository::RRS_PLANAR);
        }
        return;
    }

    LLVector4a binormal4a;
    binormal4a.setCross3(normal4a, tangent);
    binormal4a.mul(tangent.getF32ptr()[3]);

    LLVector2 projected_binormal;
    planarProjection(projected_binormal, normal4a, *vf.mCenter, binormal4a);
    projected_binormal -= LLVector2(0.5f, 0.5f); // this normally happens in xform()
    *scale = projected_binormal.length();
    // rotate binormal to match what planarProjection() thinks it is,
    // then find rotation from that:
    projected_binormal.normalize();
    F32 ang = acos(projected_binormal.mV[VY]);
    ang = (projected_binormal.mV[VX] < 0.f) ? -ang : ang;

    //VECTORIZE THIS
    LLVector3 binormal(binormal4a.getF32ptr());
    LLVector3 normal(normal4a.getF32ptr());
    binormal.rotVec(ang, normal);
    LLQuaternion local_rot( binormal % normal, binormal, normal );
    *face_rot = local_rot * vol_mat.quaternion();
    *face_pos = vol_mat.getTranslation();
}

bool LLFace::calcAlignedPlanarGLTF(
    const LLFace* align_to,
    LLVector2* res_st_offset,
    LLVector2* res_st_scale,
    F32* res_st_rot,
    S32 gltf_info_index) const
{
    if (!align_to)
    {
        return false;
    }

    const LLTextureEntry* orig_tep = align_to->getTextureEntry();
    const LLTextureEntry* tep = getTextureEntry();
    if (!orig_tep || !tep)
    {
        return false;
    }

    if (orig_tep->getTexGen() != LLTextureEntry::TEX_GEN_PLANAR ||
        tep->getTexGen() != LLTextureEntry::TEX_GEN_PLANAR)
    {
        return false;
    }

    LLGLTFMaterial* orig_mat = orig_tep->getGLTFRenderMaterial();
    LLGLTFMaterial* this_mat = tep->getGLTFRenderMaterial();
    if (!orig_mat || !this_mat)
    {
        return false;
    }

    const auto& orig_tt = orig_mat->mTextureTransform[gltf_info_index];

    F32 map_scaleS, map_scaleT, map_offsS, map_offsT, map_rot;
    LLGLTFMaterial::convertPBRTransformToTexture(
        orig_tt.mScale,
        orig_tt.mOffset,
        orig_tt.mRotation,
        map_scaleS, map_scaleT, map_offsS, map_offsT, map_rot);

    LLVector3 orig_pos, this_pos;
    LLQuaternion orig_face_rot, this_face_rot;
    F32 orig_proj_scale, this_proj_scale;
    align_to->getPlanarProjectedParams(&orig_face_rot, &orig_pos, &orig_proj_scale);
    getPlanarProjectedParams(&this_face_rot, &this_pos, &this_proj_scale);

    LLQuaternion orig_st_rot = LLQuaternion(map_rot, LLVector3::z_axis) * orig_face_rot;
    LLQuaternion this_st_rot = orig_st_rot * ~this_face_rot;
    F32 x_ang, y_ang, z_ang;
    this_st_rot.getEulerAngles(&x_ang, &y_ang, &z_ang);

    LLVector3 centers_dist = (this_pos - orig_pos) * ~orig_st_rot;
    LLVector3 st_scale(map_scaleS, map_scaleT, 1.f);
    st_scale *= orig_proj_scale;
    centers_dist.scaleVec(st_scale);
    LLVector2 orig_st_offset(map_offsS, map_offsT);

    LLVector2 tex_res_st_offset = orig_st_offset + (LLVector2)centers_dist;
    tex_res_st_offset.mV[VX] -= (S32)tex_res_st_offset.mV[VX];
    tex_res_st_offset.mV[VY] -= (S32)tex_res_st_offset.mV[VY];

    st_scale /= this_proj_scale;

    LLGLTFMaterial::convertTextureTransformToPBR(
        st_scale.mV[0], st_scale.mV[1],
        tex_res_st_offset.mV[0], tex_res_st_offset.mV[1],
        z_ang,
        *res_st_scale, *res_st_offset, *res_st_rot);

    return true;
}

// Returns the necessary texture transform to align this face's TE to align_to's TE
bool LLFace::calcAlignedPlanarTE(const LLFace* align_to,  LLVector2* res_st_offset,
                                 LLVector2* res_st_scale, F32* res_st_rot, LLRender::eTexIndex map) const
{
    if (!align_to)
    {
        return false;
    }
    const LLTextureEntry *orig_tep = align_to->getTextureEntry();
    if (!orig_tep)
    {
        return false;
    }
    const LLTextureEntry* tep = getTextureEntry();
    if (!tep)
    {
        return false;
    }
    if ((orig_tep->getTexGen() != LLTextureEntry::TEX_GEN_PLANAR) ||
        (tep->getTexGen() != LLTextureEntry::TEX_GEN_PLANAR))
    {
        return false;
    }

    F32 map_rot = 0.f, map_scaleS = 0.f, map_scaleT = 0.f, map_offsS = 0.f, map_offsT = 0.f;

    LLMaterial* mat = orig_tep->getMaterialParams();
    if (!mat && map != LLRender::DIFFUSE_MAP)
    {
        LL_WARNS_ONCE("llface") << "Face is set to use specular or normal map but has no material, defaulting to diffuse" << LL_ENDL;
        map = LLRender::DIFFUSE_MAP;
    }

    switch (map)
    {
    case LLRender::DIFFUSE_MAP:
        map_rot = orig_tep->getRotation();
        map_scaleS = orig_tep->mScaleS;
        map_scaleT = orig_tep->mScaleT;
        map_offsS = orig_tep->mOffsetS;
        map_offsT = orig_tep->mOffsetT;
        break;
    case LLRender::NORMAL_MAP:
        if (mat->getNormalID().isNull())
        {
            return false;
        }
        map_rot = mat->getNormalRotation();
        map_scaleS = mat->getNormalRepeatX();
        map_scaleT = mat->getNormalRepeatY();
        map_offsS = mat->getNormalOffsetX();
        map_offsT = mat->getNormalOffsetY();
        break;
    case LLRender::SPECULAR_MAP:
        if (mat->getSpecularID().isNull())
        {
            return false;
        }
        map_rot = mat->getSpecularRotation();
        map_scaleS = mat->getSpecularRepeatX();
        map_scaleT = mat->getSpecularRepeatY();
        map_offsS = mat->getSpecularOffsetX();
        map_offsT = mat->getSpecularOffsetY();
        break;
    default: /*make compiler happy*/
        break;
    }

    LLVector3 orig_pos, this_pos;
    LLQuaternion orig_face_rot, this_face_rot;
    F32 orig_proj_scale, this_proj_scale;
    align_to->getPlanarProjectedParams(&orig_face_rot, &orig_pos, &orig_proj_scale);
    getPlanarProjectedParams(&this_face_rot, &this_pos, &this_proj_scale);

    // The rotation of "this face's" texture:
    LLQuaternion orig_st_rot = LLQuaternion(map_rot, LLVector3::z_axis) * orig_face_rot;
    LLQuaternion this_st_rot = orig_st_rot * ~this_face_rot;
    F32 x_ang, y_ang, z_ang;
    this_st_rot.getEulerAngles(&x_ang, &y_ang, &z_ang);
    *res_st_rot = z_ang;

    // Offset and scale of "this face's" texture:
    LLVector3 centers_dist = (this_pos - orig_pos) * ~orig_st_rot;
    LLVector3 st_scale(map_scaleS, map_scaleT, 1.f);
    st_scale *= orig_proj_scale;
    centers_dist.scaleVec(st_scale);
    LLVector2 orig_st_offset(map_offsS, map_offsT);

    *res_st_offset = orig_st_offset + (LLVector2)centers_dist;
    res_st_offset->mV[VX] -= (S32)res_st_offset->mV[VX];
    res_st_offset->mV[VY] -= (S32)res_st_offset->mV[VY];

    st_scale /= this_proj_scale;
    *res_st_scale = (LLVector2)st_scale;
    return true;
}

bool LLFace::canRenderAsMask()
{
    const LLTextureEntry* te = getTextureEntry();
    if( !te || !getViewerObject() || !getTexture() )
    {
        return false;
    }

    if (te->getGLTFRenderMaterial())
    {
        return false;
    }

    if (LLPipeline::sNoAlpha)
    {
        return true;
    }

    if (isState(LLFace::RIGGED))
    { // never auto alpha-mask rigged faces
        return false;
    }


    LLMaterial* mat = te->getMaterialParams();
    if (mat && mat->getDiffuseAlphaMode() == LLMaterial::DIFFUSE_ALPHA_MODE_BLEND)
    {
        return false;
    }

    if ((te->getColor().mV[3] == 1.0f) && // can't treat as mask if we have face alpha
        (te->getGlow() == 0.f) && // glowing masks are hard to implement - don't mask
        getTexture()->getIsAlphaMask()) // texture actually qualifies for masking (lazily recalculated but expensive)
    {
        if (getViewerObject()->isHUDAttachment() || te->getFullbright())
        { //hud attachments and fullbright objects are NOT subject to the deferred rendering pipe
            return LLPipeline::sAutoMaskAlphaNonDeferred;
        }
        else
        {
            return LLPipeline::sAutoMaskAlphaDeferred;
        }
    }

    return false;
}

//check if the face has a media
bool LLFace::hasMedia() const
{
    if(mHasMedia)
    {
        return true ;
    }
    if(mTexture[LLRender::DIFFUSE_MAP].notNull())
    {
        return mTexture[LLRender::DIFFUSE_MAP]->hasParcelMedia() ;  //if has a parcel media
    }

    return false ; //no media.
}

const F32 LEAST_IMPORTANCE = 0.05f ;
const F32 LEAST_IMPORTANCE_FOR_LARGE_IMAGE = 0.3f ;

void LLFace::resetVirtualSize()
{
    setVirtualSize(0.f);
    mImportanceToCamera = 0.f;
}

F32 LLFace::getTextureVirtualSize()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;

    F32 radius;
    F32 cos_angle_to_view_dir;
    bool in_frustum = calcPixelArea(cos_angle_to_view_dir, radius);


    if (mPixelArea < F_ALMOST_ZERO || !in_frustum)
    {
        setVirtualSize(0.f) ;
        return 0.f;
    }

    //get area of circle in texture space
    LLVector2 tdim = getTextureSpanForVSize();
    F32 texel_area = (tdim * 0.5f).lengthSquared()*3.14159f;
    if (texel_area <= 0)
    {
        // Probably animated, use default
        texel_area = 1.f;
    }

    F32 face_area;
    if (mVObjp->isSculpted() && texel_area > 1.f)
    {
        //sculpts can break assumptions about texel area
        face_area = mPixelArea;
    }
    else
    {
        // Screen pixels per unit of UV area: a face showing a small part of its texture
        // (prim-text glyph cells, sprite frames) needs proportionally more texels. No
        // fixed 1/64 floor (8x linear); the request is capped at what the textures have.
        face_area = mPixelArea / llclamp(texel_area, 1.e-6f, 128.f);
        face_area = llmin(face_area, getTexturesFullArea());
    }

    face_area = LLFace::adjustPixelArea(mImportanceToCamera, face_area);
    if(face_area > LLViewerTexture::sMinLargeImageSize) //if is large image, shrink face_area by considering the partial overlapping.
    {
        if(mImportanceToCamera > LEAST_IMPORTANCE_FOR_LARGE_IMAGE && mTexture[LLRender::DIFFUSE_MAP].notNull() && mTexture[LLRender::DIFFUSE_MAP]->isLargeImage())
        {
            face_area *= adjustPartialOverlapPixelArea(cos_angle_to_view_dir, radius );
        }
    }

    setVirtualSize(face_area) ;

    return face_area;
}

// UV extent the face covers, as getGeometryVolume's mTexExtents (TE transform of the
// volume face's raw texcoord range), computed here: under Vulkan getGeometryVolume is
// not maintained (rebuildGeom is diverted) and mTexExtents is usually its 0..1 default.
LLVector2 LLFace::getTextureSpanForVSize() const
{
    const LLTextureEntry* te = getTextureEntry();
    LLVolume* volume = mVObjp.notNull() ? mVObjp->getVolume() : nullptr;
    if (!te || !volume || mTEOffset < 0 || mTEOffset >= volume->getNumVolumeFaces())
    {
        return mTexExtents[1] - mTexExtents[0];
    }
    const LLVolumeFace& vf = volume->getVolumeFace(mTEOffset);
    const F32 es = vf.mTexCoordExtents[1].mV[0] - vf.mTexCoordExtents[0].mV[0];
    const F32 et = vf.mTexCoordExtents[1].mV[1] - vf.mTexCoordExtents[0].mV[1];
    const LLVOVolume* vvo = mDrawablep ? mDrawablep->getVOVolume() : nullptr;
    const LLViewerTextureAnim* ta = vvo ? vvo->mTextureAnimp : nullptr;
    F32 ms, mt, r;
    if (mTextureMatrix || (ta && (ta->mMode & LLTextureAnim::ON) && (ta->mFace < 0 || ta->mFace == mTEOffset)))
    {
        ms = mt = 1.f; r = 0.f;   // texture animation: getGeometryVolume uses identity
    }
    else if (const LLGLTFMaterial* gm = te->getGLTFRenderMaterial())
    {
        const LLGLTFMaterial::TextureTransform& tt = gm->mTextureTransform[LLGLTFMaterial::GLTF_TEXTURE_INFO_BASE_COLOR];
        ms = tt.mScale.mV[VX]; mt = tt.mScale.mV[VY]; r = tt.mRotation;
    }
    else
    {
        te->getScale(&ms, &mt); r = te->getRotation();
    }
    // Bounding box of the raw texcoord rectangle after rotation and scale.
    const F32 c = fabsf(cosf(r)), sn = fabsf(sinf(r));
    return LLVector2((c * es + sn * et) * fabsf(ms), (sn * es + c * et) * fabsf(mt));
}

// Largest full pixel area among the face's textures (unknown size: the largest asset).
F32 LLFace::getTexturesFullArea() const
{
    F32 area = 0.f;
    for (U32 ch = 0; ch < LLRender::NUM_TEXTURE_CHANNELS; ++ch)
    {
        const LLViewerTexture* tex = mTexture[ch].get();
        if (!tex) continue;
        const S32 w = tex->getFullWidth(), h = tex->getFullHeight();
        if (w <= 0 || h <= 0) return (F32)(LLGLTexture::MAX_IMAGE_SIZE_DEFAULT * LLGLTexture::MAX_IMAGE_SIZE_DEFAULT);
        area = llmax(area, (F32)w * (F32)h);
    }
    return area > 0.f ? area : (F32)(LLGLTexture::MAX_IMAGE_SIZE_DEFAULT * LLGLTexture::MAX_IMAGE_SIZE_DEFAULT);
}

bool LLFace::calcPixelArea(F32& cos_angle_to_view_dir, F32& radius)
{
    constexpr F32 PIXEL_AREA_UPDATE_PERIOD = 0.1f;
    // this is an expensive operation and the result is valid (enough) for several frames
    // don't update every frame
    if (gFrameTimeSeconds - mLastPixelAreaUpdate < PIXEL_AREA_UPDATE_PERIOD)
    {
        return true;
    }

    LL_PROFILE_ZONE_SCOPED_CATEGORY_FACE;

    //get area of circle around face
    LLVector4a center;
    LLVector4a size;

    if (isState(LLFace::RIGGED))
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_FACE("calcPixelArea - rigged");
        //override with joint volume face joint bounding boxes
        LLVOAvatar* avatar = mVObjp.notNull() ? mVObjp->getAvatar() : nullptr;
        bool hasRiggedExtents = false;

        if (avatar && avatar->mDrawable)
        {
            LLVolume* volume = mVObjp->getVolume();
            if (volume)
            {
                LLVolumeFace& face = volume->getVolumeFace(mTEOffset);

                auto& rigInfo = face.mJointRiggingInfoTab;

                if (rigInfo.needsUpdate())
                {
                    LLVOVolume* vo_volume = (LLVOVolume*)mVObjp.get();
                    const LLMeshSkinInfo* skin = vo_volume->getSkinInfo();
                    if (skin)
                    {
                        LLSkinningUtil::updateRiggingInfo(skin, avatar, face);
                    }
                }

                // calculate the world space bounding box of the face by combining the bounding boxes of all the joints
                LLVector4a& minp = mRiggedExtents[0];
                LLVector4a& maxp = mRiggedExtents[1];
                minp = LLVector4a(FLT_MAX, FLT_MAX, FLT_MAX);
                maxp = LLVector4a(-FLT_MAX, -FLT_MAX, -FLT_MAX);

                for (S32 i = 0; i < rigInfo.size(); i++)
                {
                    auto& jointInfo = rigInfo[i];
                    if (jointInfo.isRiggedTo())
                    {
                        LLJoint* joint = avatar->getJoint(i);

                        if (joint)
                        {
                            LLMatrix4a worldMat;
                            worldMat.loadu((F32*)&joint->getWorldMatrix().mMatrix[0][0]);

                            LLVector4a extents[2];

                            matMulBoundBox(worldMat, jointInfo.getRiggedExtents(), extents);

                            minp.setMin(minp, extents[0]);
                            maxp.setMax(maxp, extents[1]);
                            hasRiggedExtents = true;
                        }
                    }
                }
            }
        }

        if (!hasRiggedExtents)
        {
            // no rigged extents, zero out bounding box and skip update
            mRiggedExtents[0] = mRiggedExtents[1] = LLVector4a(0.f, 0.f, 0.f);

            return false;
        }

        center.setAdd(mRiggedExtents[1], mRiggedExtents[0]);
        center.mul(0.5f);
        size.setSub(mRiggedExtents[1], mRiggedExtents[0]);
    }
    else
    {
        center.load3(getPositionAgent().mV);
        size.setSub(mExtents[1], mExtents[0]);
    }
    size.mul(0.5f);

    LLViewerCamera* camera = LLViewerCamera::getInstance();

    F32 size_squared = size.dot3(size).getF32();
    LLVector4a lookAt;
    LLVector4a t;
    t.load3(camera->getOrigin().mV);
    lookAt.setSub(center, t);

    F32 dist = lookAt.getLength3().getF32();
    dist = llmax(dist-size.getLength3().getF32(), 0.001f);

    lookAt.normalize3fast() ;

    //get area of circle around node
    F32 app_angle = atanf((F32) sqrt(size_squared) / dist);
    radius = app_angle*LLDrawable::sCurPixelAngle;
    mPixelArea = radius*radius * 3.14159f;

    // remember last update time, add 10% noise to avoid all faces updating at the same time
    mLastPixelAreaUpdate = gFrameTimeSeconds + ll_frand() * PIXEL_AREA_UPDATE_PERIOD * 0.1f;

    LLVector4a x_axis;
    x_axis.load3(camera->getXAxis().mV);
    cos_angle_to_view_dir = lookAt.dot3(x_axis).getF32();

    //if has media, check if the face is out of the view frustum.
    if(hasMedia())
    {
        if(!camera->AABBInFrustum(center, size))
        {
            mImportanceToCamera = 0.f ;
            return false ;
        }
        if(cos_angle_to_view_dir > camera->getCosHalfFov()) //the center is within the view frustum
        {
            cos_angle_to_view_dir = 1.0f ;
        }
        else
        {
            LLVector4a d;
            d.setSub(lookAt, x_axis);

            if(dist * dist * d.dot3(d) < size_squared)
            {
                cos_angle_to_view_dir = 1.0f ;
            }
        }
    }

    if(dist < mBoundingSphereRadius) //camera is very close
    {
        cos_angle_to_view_dir = 1.0f ;
        mImportanceToCamera = 1.0f ;
    }
    else
    {
        mImportanceToCamera = LLFace::calcImportanceToCamera(cos_angle_to_view_dir, dist) ;
    }

    return true ;
}

//the projection of the face partially overlaps with the screen
F32 LLFace::adjustPartialOverlapPixelArea(F32 cos_angle_to_view_dir, F32 radius )
{
    F32 screen_radius = (F32)llmax(gViewerWindow->getWindowWidthRaw(), gViewerWindow->getWindowHeightRaw()) ;
    F32 center_angle = acosf(cos_angle_to_view_dir) ;
    F32 d = center_angle * LLDrawable::sCurPixelAngle ;

    if(d + radius > screen_radius + 5.f)
    {
        //----------------------------------------------
        //calculate the intersection area of two circles
        //F32 radius_square = radius * radius ;
        //F32 d_square = d * d ;
        //F32 screen_radius_square = screen_radius * screen_radius ;
        //face_area =
        //  radius_square * acosf((d_square + radius_square - screen_radius_square)/(2 * d * radius)) +
        //  screen_radius_square * acosf((d_square + screen_radius_square - radius_square)/(2 * d * screen_radius)) -
        //  0.5f * sqrtf((-d + radius + screen_radius) * (d + radius - screen_radius) * (d - radius + screen_radius) * (d + radius + screen_radius)) ;
        //----------------------------------------------

        //the above calculation is too expensive
        //the below is a good estimation: bounding box of the bounding sphere:
        F32 alpha = 0.5f * (radius + screen_radius - d) / radius ;
        alpha = llclamp(alpha, 0.f, 1.f) ;
        return alpha * alpha ;
    }
    return 1.0f ;
}

const S8 FACE_IMPORTANCE_LEVEL = 4 ;
const F32 FACE_IMPORTANCE_TO_CAMERA_OVER_DISTANCE[FACE_IMPORTANCE_LEVEL][2] = //{distance, importance_weight}
    {{16.1f, 1.0f}, {32.1f, 0.5f}, {48.1f, 0.2f}, {96.1f, 0.05f} } ;
const F32 FACE_IMPORTANCE_TO_CAMERA_OVER_ANGLE[FACE_IMPORTANCE_LEVEL][2] =    //{cos(angle), importance_weight}
    {{0.985f /*cos(10 degrees)*/, 1.0f}, {0.94f /*cos(20 degrees)*/, 0.8f}, {0.866f /*cos(30 degrees)*/, 0.64f}, {0.0f, 0.36f}} ;

//static
F32 LLFace::calcImportanceToCamera(F32 cos_angle_to_view_dir, F32 dist)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_FACE;
    F32 importance = 0.f ;

    if(cos_angle_to_view_dir > LLViewerCamera::getInstance()->getCosHalfFov() &&
        dist < FACE_IMPORTANCE_TO_CAMERA_OVER_DISTANCE[FACE_IMPORTANCE_LEVEL - 1][0])
    {
        LLViewerCamera* camera = LLViewerCamera::getInstance();
        F32 camera_moving_speed = camera->getAverageSpeed() ;
        F32 camera_angular_speed = camera->getAverageAngularSpeed();

        if(camera_moving_speed > 10.0f || camera_angular_speed > 1.0f)
        {
            //if camera moves or rotates too fast, ignore the importance factor
            return 0.f ;
        }

        S32 i = 0 ;
        for(i = 0; i < FACE_IMPORTANCE_LEVEL && dist > FACE_IMPORTANCE_TO_CAMERA_OVER_DISTANCE[i][0]; ++i);
        i = llmin(i, FACE_IMPORTANCE_LEVEL - 1) ;
        F32 dist_factor = FACE_IMPORTANCE_TO_CAMERA_OVER_DISTANCE[i][1] ;

        for(i = 0; i < FACE_IMPORTANCE_LEVEL && cos_angle_to_view_dir < FACE_IMPORTANCE_TO_CAMERA_OVER_ANGLE[i][0] ; ++i) ;
        i = llmin(i, FACE_IMPORTANCE_LEVEL - 1) ;
        importance = dist_factor * FACE_IMPORTANCE_TO_CAMERA_OVER_ANGLE[i][1] ;
    }

    return importance ;
}

//static
F32 LLFace::adjustPixelArea(F32 importance, F32 pixel_area)
{
    if(pixel_area > LLViewerTexture::sMaxSmallImageSize)
    {
        if(importance < LEAST_IMPORTANCE) //if the face is not important, do not load hi-res.
        {
            static const F32 MAX_LEAST_IMPORTANCE_IMAGE_SIZE = 128.0f * 128.0f ;
            pixel_area = llmin(pixel_area * 0.5f, MAX_LEAST_IMPORTANCE_IMAGE_SIZE) ;
        }
        else if(pixel_area > LLViewerTexture::sMinLargeImageSize) //if is large image, shrink face_area by considering the partial overlapping.
        {
            if(importance < LEAST_IMPORTANCE_FOR_LARGE_IMAGE)//if the face is not important, do not load hi-res.
            {
                pixel_area = (F32)LLViewerTexture::sMinLargeImageSize ;
            }
        }
    }

    return pixel_area ;
}

void LLFace::setViewerObject(LLViewerObject* objp)
{
    mVObjp = objp;
}


const LLMatrix4& LLFace::getRenderMatrix() const
{
    return mDrawablep->getRenderMatrix();
}

//============================================================================
// From llface.inl

LLVector3 LLFace::getPositionAgent() const
{
    if (mDrawablep->isStatic())
    {
        return mCenterAgent;
    }
    else
    {
        return mCenterLocal * getRenderMatrix();
    }
}

LLViewerTexture* LLFace::getTexture(U32 ch) const
{
    llassert(ch < LLRender::NUM_TEXTURE_CHANNELS);

// [SL:KB] - Patch: Render-TextureToggle (Catznip-4.0)
    // Check whether the diffuse texture needs to be obscured or restored
    if (mShowDiffTexture != LLPipeline::sRenderTextures)
        setDefaultTexture(LLRender::DIFFUSE_MAP, !LLPipeline::sRenderTextures);
// [/SL:KB]

    return mTexture[ch] ;
}

// [SL:KB] - Patch: Render-TextureToggle (Catznip-4.0)
bool LLFace::isDefaultTexture(U32 nChannel) const
{
    // NOTE: mShowDiffTexture gets flipped before the clear (good) but also before the restore (bad) and hence can't
    //       be used to tell whether we're usurping a texture channel for our own use
    return (LLRender::DIFFUSE_MAP == nChannel) ? mOrigDiffTexture.notNull() : false;
}

void LLFace::setDefaultTexture(U32 nChannel, bool fShowDefault) const
{
    bool fUpdated = false;
    if ( (LLRender::DIFFUSE_MAP == nChannel) && (mVObjp) && (!mVObjp->isDead()) && ((LL_PCODE_VOLUME == mVObjp->getPCode()) || (LLViewerObject::LL_VO_PART_GROUP == mVObjp->getPCode())) )
    {
        if ( ((mShowDiffTexture) && (fShowDefault)) ||
             ((!mShowDiffTexture) && (fShowDefault) && (mOrigDiffTexture.notNull()) && (mTexture[nChannel]) && (mTexture[nChannel]->getID() != LLViewerFetchedTexture::sDefaultDiffuseImagep->getID())) )
        {
            if (mOrigDiffTexture.notNull())
                mShowDiffTexture = true;                // Swap out the default texture
            else
                mOrigDiffTexture = mTexture[nChannel];  // Cache the original texture

            if ( (!gRlvHandler.hasBehaviour(RLV_BHVR_SETCAM_TEXTURES)) || (!mVObjp->isAttachment()) )
            {
                if (LL_PCODE_VOLUME == mVObjp->getPCode())
                    const_cast<LLFace*>(this)->switchTexture(nChannel, LLViewerFetchedTexture::sDefaultDiffuseImagep);
                else
                    const_cast<LLFace*>(this)->setTexture(nChannel, LLViewerFetchedTexture::sDefaultDiffuseImagep);
            }
            mShowDiffTexture = false; fUpdated = true;
        }
        else if ( (!mShowDiffTexture) && (!fShowDefault) && (mOrigDiffTexture.notNull()) )
        {
            mShowDiffTexture = true;
            if (LL_PCODE_VOLUME == mVObjp->getPCode())
                const_cast<LLFace*>(this)->switchTexture(nChannel, mOrigDiffTexture);
            else
                const_cast<LLFace*>(this)->setTexture(nChannel, mOrigDiffTexture);
            mOrigDiffTexture = nullptr; fUpdated = true;
        }

        if ((fUpdated) && (mDrawablep))
        {
            gPipeline.markTextured(mDrawablep);
            mDrawablep->updateTexture();
        }
    }

    // Always flip the flag even if we didn't obscure so we don't keep wasting cycles with negative checks
    mShowDiffTexture = !fShowDefault;
}
// [/SL:KB]

S32 LLFace::getRiggedIndex(U32 type) const
{
    if (mRiggedIndex.empty())
    {
        return -1;
    }

    llassert(type < mRiggedIndex.size());

    return mRiggedIndex[type];
}

U64 LLFace::getSkinHash()
{
    return mSkinInfo ? mSkinInfo->mHash : 0;
}

bool LLFace::isInAlphaPool() const
{
    return  getPoolType() == LLDrawPool::POOL_ALPHA ||
        getPoolType() == LLDrawPool::POOL_ALPHA_PRE_WATER ||
        getPoolType() == LLDrawPool::POOL_ALPHA_POST_WATER;
}
