/**
 * @file llvovolume.cpp
 * @brief LLVOVolume class implementation
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

// A "volume" is a box, cylinder, sphere, or other primitive shape.

#include "llviewerprecompiledheaders.h"


#include "llvovolume.h"
#include "llcleancursorpick.h"   // GPU raster cursor pick (#255 / #702)
#include "llworldclock.h"
#include "vkgfx/llvulkancontext.h"
#include "lllocalmesh.h"   // Local Assets: local mesh previews
#include "llvkhitch.h"
#include <tuple>

#include "llcleaninstancestore.h"
#include "llcleaningest.h"
#include "slbridge.h"
#include "llcleangltfingest.h"
#include "llcleandebug.h"
#include "llcleanmaterialstore.h"
#include "llcleanmeshasset.h"
#include "llcleantexture.h"
#include "llcleanskinstore.h"
#include <cstring>

#include <sstream>

#include "llviewercontrol.h"
#include "lldir.h"
#include "llflexibleobject.h"
#include "llfloatertools.h"
#include "llmaterialid.h"
#include "llmaterialtable.h"
#include "llprimitive.h"
#include "llvolume.h"
#include "llvolumemgr.h"
#include "llvolumemessage.h"
#include "material_codes.h"
#include "message.h"
#include "llpluginclassmedia.h" // for code in the mediaEvent handler
#include "object_flags.h"
#include "lldrawable.h"
#include "llface.h"
#include "llhudmanager.h"
#include "llskinningutil.h"
#include "llsky.h"
#include "lltexturefetch.h"
#include "llvector4a.h"
#include "llviewercamera.h"
#include "llviewertexturelist.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llviewertextureanim.h"
#include "llworld.h"
#include "llselectmgr.h"
#include "pipeline.h"
#include "llsdutil.h"
#include "llmatrix4a.h"
#include "llmediaentry.h"
#include "llmediadataclient.h"
#include "llmeshrepository.h"
#include "llnotifications.h"
#include "llnotificationsutil.h"
#include "llagent.h"
#include "llviewermediafocus.h"
#include "lldatapacker.h"
#include "llrender.h"
#include "llvoavatar.h"
#include "llcontrolavatar.h"
#include "llviewerjointattachment.h"
#include "llvoavatarself.h"
#include "llvocache.h"
#include "llmaterialmgr.h"
#include "llanimationstates.h"
#include "llinventorytype.h"
#include "llviewerinventory.h"
#include "llsculptidsize.h"
#include "llavatarappearancedefines.h"
#include "llgltfmateriallist.h"
#include "gltfscenemanager.h"
// [RLVa:KB] - Checked: RLVa-2.0.0
#include "rlvactions.h"
#include "rlvlocks.h"
#include "gmdxregioncapture.h"   // <GMDX> region map capture: max LOD in the region being mapped
// [/RLVa:KB]

#include <cmath>
#include <utility>

U32 JOINT_COUNT_REQUIRED_FOR_FULLRIG = 1;


F32 LLVOVolume::sLODFactor = 1.f;
F32 LLVOVolume::sLODSlopDistanceFactor = 0.5f; //Changing this to zero, effectively disables the LOD transition slop
F32 LLVOVolume::sDistanceFactor = 1.0f;
S32 LLVOVolume::sNumLODChanges = 0;
S32 LLVOVolume::mRenderComplexity_last = 0;
S32 LLVOVolume::mRenderComplexity_current = 0;
LLPointer<LLObjectMediaDataClient> LLVOVolume::sObjectMediaClient = NULL;
LLPointer<LLObjectMediaNavigateClient> LLVOVolume::sObjectMediaNavigateClient = NULL;

// Implementation class of LLMediaDataClientObject.  See llmediadataclient.h
class LLMediaDataClientObjectImpl : public LLMediaDataClientObject
{
public:
    LLMediaDataClientObjectImpl(LLVOVolume *obj, bool isNew) : mObject(obj), mNew(isNew)
    {
        mObject->addMDCImpl();
    }
    ~LLMediaDataClientObjectImpl()
    {
        mObject->removeMDCImpl();
    }

    virtual U8 getMediaDataCount() const
        { return mObject->getNumTEs(); }

    virtual LLSD getMediaDataLLSD(U8 index) const
        {
            LLSD result;
            LLTextureEntry *te = mObject->getTE(index);
            if (NULL != te)
            {
                llassert((te->getMediaData() != NULL) == te->hasMedia());
                if (te->getMediaData() != NULL)
                {
                    result = te->getMediaData()->asLLSD();
                    // XXX HACK: workaround bug in asLLSD() where whitelist is not set properly
                    // See DEV-41949
                    if (!result.has(LLMediaEntry::WHITELIST_KEY))
                    {
                        result[LLMediaEntry::WHITELIST_KEY] = LLSD::emptyArray();
                    }
                }
            }
            return result;
        }
    virtual bool isCurrentMediaUrl(U8 index, const std::string &url) const
        {
            LLTextureEntry *te = mObject->getTE(index);
            if (te)
            {
                if (te->getMediaData())
                {
                    return (te->getMediaData()->getCurrentURL() == url);
                }
            }
            return url.empty();
        }

    virtual LLUUID getID() const
        { return mObject->getID(); }

    virtual void mediaNavigateBounceBack(U8 index)
        { mObject->mediaNavigateBounceBack(index); }

    virtual bool hasMedia() const
        { return mObject->hasMedia(); }

    virtual void updateObjectMediaData(LLSD const &data, const std::string &version_string)
        { mObject->updateObjectMediaData(data, version_string); }

    virtual F64 getMediaInterest() const
        {
            F64 interest = mObject->getTotalMediaInterest();
            if (interest < (F64)0.0)
            {
                // media interest not valid yet, try pixel area
                interest = mObject->getPixelArea();
                // HACK: force recalculation of pixel area if interest is the "magic default" of 1024.
                if (interest == 1024.f)
                {
                    const_cast<LLVOVolume*>(static_cast<LLVOVolume*>(mObject))->setPixelAreaAndAngle(gAgent);
                    interest = mObject->getPixelArea();
                }
            }
            return interest;
        }

    virtual bool isInterestingEnough() const
        {
            return LLViewerMedia::getInstance()->isInterestingEnough(mObject, getMediaInterest());
        }

    virtual std::string getCapabilityUrl(const std::string &name) const
        { return mObject->getRegion()->getCapability(name); }

    virtual bool isDead() const
        { return mObject->isDead(); }

    virtual U32 getMediaVersion() const
        { return LLTextureEntry::getVersionFromMediaVersionString(mObject->getMediaURL()); }

    virtual bool isNew() const
        { return mNew; }

private:
    LLPointer<LLVOVolume> mObject;
    bool mNew;
};


LLVOVolume::LLVOVolume(const LLUUID &id, const LLPCode pcode, LLViewerRegion *regionp)
    : LLViewerObject(id, pcode, regionp),
      mVolumeImpl(NULL)
{
    mTexAnimMode = 0;
    mRelativeXform.setIdentity();
    mRelativeXformInvTrans.setIdentity();

    mFaceMappingChanged = false;
    mLOD = MIN_LOD;
    mLODDistance = 0.0f;
    mLODAdjustedDistance = 0.0f;
    mLODRadius = 0.0f;
    mTextureAnimp = NULL;
    mVolumeChanged = false;
    mVObjRadius = LLVector3(1,1,0.5f).length();
    mNumFaces = 0;
    mLODChanged = false;
    mSculptChanged = false;
    mColorChanged = false;
    mSpotLightPriority = 0.f;

    mSkinInfoUnavaliable = false;
    mSkinInfo = NULL;

    mMediaImplList.resize(getNumTEs());
    mLastFetchedMediaVersion = -1;
    mServerDrawableUpdateCount = 0;
    memset(&mIndexInTex, 0, sizeof(S32) * LLRender::NUM_VOLUME_TEXTURE_CHANNELS);
    mMDCImplCount = 0;
    mLastRiggingInfoLOD = -1;
    mResetDebugText = false;
}

LLVOVolume::~LLVOVolume()
{
    LL_PROFILE_ZONE_SCOPED;
    delete mTextureAnimp;
    mTextureAnimp = NULL;
    delete mVolumeImpl;
    mVolumeImpl = NULL;

    gMeshRepo.unregisterMesh(this);

    if(!mMediaImplList.empty())
    {
        for(U32 i = 0 ; i < mMediaImplList.size() ; i++)
        {
            if(mMediaImplList[i].notNull())
            {
                mMediaImplList[i]->removeObject(this) ;
            }
        }
    }
}

void LLVOVolume::markDead()
{
    if (!mDead)
    {
        LL_PROFILE_ZONE_SCOPED;
        if (getVolume())
        {
            LLSculptIDSize::instance().rem(getVolume()->getParams().getSculptID());
        }

        if(getMDCImplCount() > 0)
        {
            LLMediaDataClientObject::ptr_t obj = new LLMediaDataClientObjectImpl(const_cast<LLVOVolume*>(this), false);
            if (sObjectMediaClient) sObjectMediaClient->removeFromQueue(obj);
            if (sObjectMediaNavigateClient) sObjectMediaNavigateClient->removeFromQueue(obj);
        }

        // Detach all media impls from this object
        for(U32 i = 0 ; i < mMediaImplList.size() ; i++)
        {
            removeMediaImpl(i);
        }

        if (mSculptTexture.notNull())
        {
            mSculptTexture->removeVolume(LLRender::SCULPT_TEX, this);
        }

        if (mLightTexture.notNull())
        {
            mLightTexture->removeVolume(LLRender::LIGHT_TEX, this);
        }

        if (mIsHeroProbe)
        {
            gPipeline.mHeroProbeManager.unregisterViewerObject(this);
        }
    }

    LLViewerObject::markDead();
}


// static
void LLVOVolume::initClass()
{
    // gSavedSettings better be around
    if (gSavedSettings.getBOOL("PrimMediaMasterEnabled"))
    {
        const F32 queue_timer_delay = gSavedSettings.getF32("PrimMediaRequestQueueDelay");
        const F32 retry_timer_delay = gSavedSettings.getF32("PrimMediaRetryTimerDelay");
        const U32 max_retries = gSavedSettings.getU32("PrimMediaMaxRetries");
        const U32 max_sorted_queue_size = gSavedSettings.getU32("PrimMediaMaxSortedQueueSize");
        const U32 max_round_robin_queue_size = gSavedSettings.getU32("PrimMediaMaxRoundRobinQueueSize");
        sObjectMediaClient = new LLObjectMediaDataClient(queue_timer_delay, retry_timer_delay, max_retries,
                                                         max_sorted_queue_size, max_round_robin_queue_size);
        sObjectMediaNavigateClient = new LLObjectMediaNavigateClient(queue_timer_delay, retry_timer_delay,
                                                                     max_retries, max_sorted_queue_size, max_round_robin_queue_size);
    }
}

static void vk_clear_pending_sculpt_maps(); // defined with s_vk_pending_sculpt_maps

// static
void LLVOVolume::cleanupClass()
{
    sObjectMediaClient = NULL;
    sObjectMediaNavigateClient = NULL;
    vk_clear_pending_sculpt_maps(); // #275: no texture refs left for static destruction
}

U32 LLVOVolume::processUpdateMessage(LLMessageSystem *mesgsys,
                                          void **user_data,
                                          U32 block_num, EObjectUpdateType update_type,
                                          LLDataPacker *dp)
{

    LLColor4U color;
    const S32 teDirtyBits = (TEM_CHANGE_TEXTURE|TEM_CHANGE_COLOR|TEM_CHANGE_MEDIA);
    const bool previously_volume_changed = mVolumeChanged;
    const bool previously_face_mapping_changed = mFaceMappingChanged;
    const bool previously_color_changed = mColorChanged;

    // Do base class updates...
    const LLQuaternion vk_rot_before = getRotation();
    U32 retval = LLViewerObject::processUpdateMessage(mesgsys, user_data, block_num, update_type, dp);
    {
        // VKIngestDiag: omega vs scripted rotation vs neither.
        mVKUpdAngVel = getAngularVelocity();
        mVKUpdFrame = (U32)gFrameCount;
        ++mVKUpdCount;
        if (!mVKUpdAngVel.isExactlyZero())
        {
            ++mVKOmegaUpdates;
            mVKLastNonzeroAngVel = mVKUpdAngVel;
            mVKLastNonzeroFrame = (U32)gFrameCount;
        }
        const LLQuaternion r = getRotation();
        if (fabsf(r.mQ[0] - vk_rot_before.mQ[0]) + fabsf(r.mQ[1] - vk_rot_before.mQ[1])
            + fabsf(r.mQ[2] - vk_rot_before.mQ[2]) + fabsf(r.mQ[3] - vk_rot_before.mQ[3]) > 1e-4f)
            ++mVKRotUpdates;
    }

    LLUUID sculpt_id;
    U8 sculpt_type = 0;
    if (isSculpted())
    {
        LLSculptParams *sculpt_params = (LLSculptParams *)getParameterEntry(LLNetworkData::PARAMS_SCULPT);
        if (sculpt_params)
        {
            sculpt_id = sculpt_params->getSculptTexture();
            sculpt_type = sculpt_params->getSculptType();
        }

        LL_DEBUGS("ObjectUpdate") << "uuid " << mID << " set sculpt_id " << sculpt_id << LL_ENDL;
    }

    if (!dp)
    {
        if (update_type == OUT_FULL)
        {
            ////////////////////////////////
            //
            // Unpack texture animation data
            //
            //

            if (mesgsys->getSizeFast(_PREHASH_ObjectData, block_num, _PREHASH_TextureAnim))
            {
                if (!mTextureAnimp)
                {
                    mTextureAnimp = new LLViewerTextureAnim(this);
                }
                else
                {
                    if (!(mTextureAnimp->mMode & LLTextureAnim::SMOOTH))
                    {
                        mTextureAnimp->reset();
                    }
                }
                mTexAnimMode = 0;

                mTextureAnimp->unpackTAMessage(mesgsys, block_num);
            }
            else
            {
                if (mTextureAnimp)
                {
                    delete mTextureAnimp;
                    mTextureAnimp = NULL;

                    for (S32 i = 0; i < getNumTEs(); i++)
                    {
                        LLFace* facep = mDrawable->getFace(i);
                        if (facep && facep->mTextureMatrix)
                        {
                            // delete or reset
                            delete facep->mTextureMatrix;
                            facep->mTextureMatrix = NULL;
                        }
                    }

                    gPipeline.markTextured(mDrawable);
                    mFaceMappingChanged = true;
                    mTexAnimMode = 0;
                }
            }

            // Unpack volume data
            LLVolumeParams volume_params;
            LLVolumeMessage::unpackVolumeParams(&volume_params, mesgsys, _PREHASH_ObjectData, block_num);
            volume_params.setSculptID(sculpt_id, sculpt_type);

            if (setVolume(volume_params, 0))
            {
                markForUpdate();
            }
        }

        // Sigh, this needs to be done AFTER the volume is set as well, otherwise bad stuff happens...
        ////////////////////////////
        //
        // Unpack texture entry data
        //

        S32 result = unpackTEMessage(mesgsys, _PREHASH_ObjectData, (S32) block_num);

        if (result & TEM_CHANGE_MEDIA)
        {
            retval |= MEDIA_FLAGS_CHANGED;
        }
    }
    else
    {
        if (update_type != OUT_TERSE_IMPROVED)
        {
            LLVolumeParams volume_params;
            bool res = LLVolumeMessage::unpackVolumeParams(&volume_params, *dp);
            if (!res)
            {
                LL_WARNS() << "Bogus volume parameters in object " << getID() << LL_ENDL;
                LL_WARNS() << getRegion()->getOriginGlobal() << LL_ENDL;
            }

            volume_params.setSculptID(sculpt_id, sculpt_type);

            if (setVolume(volume_params, 0))
            {
                markForUpdate();
            }
            S32 res2 = unpackTEMessage(*dp);
            if (TEM_INVALID == res2)
            {
                // There's something bogus in the data that we're unpacking.
                dp->dumpBufferToLog();
                LL_WARNS() << "Flushing cache files" << LL_ENDL;

                if(LLVOCache::instanceExists() && getRegion())
                {
                    LLVOCache::getInstance()->removeEntry(getRegion()->getHandle()) ;
                }

                LL_WARNS() << "Bogus TE data in " << getID() << LL_ENDL;
            }
            else
            {
                if (res2 & TEM_CHANGE_MEDIA)
                {
                    retval |= MEDIA_FLAGS_CHANGED;
                }
            }

            U32 value = dp->getPassFlags();

            if (value & 0x40)
            {
                if (!mTextureAnimp)
                {
                    mTextureAnimp = new LLViewerTextureAnim(this);
                }
                else
                {
                    if (!(mTextureAnimp->mMode & LLTextureAnim::SMOOTH))
                    {
                        mTextureAnimp->reset();
                    }
                }
                mTexAnimMode = 0;
                mTextureAnimp->unpackTAMessage(*dp);
            }
            else if (mTextureAnimp)
            {
                delete mTextureAnimp;
                mTextureAnimp = NULL;

                for (S32 i = 0; i < getNumTEs(); i++)
                {
                    LLFace* facep = mDrawable->getFace(i);
                    if (facep && facep->mTextureMatrix)
                    {
                        // delete or reset
                        delete facep->mTextureMatrix;
                        facep->mTextureMatrix = NULL;
                    }
                }

                gPipeline.markTextured(mDrawable);
                mFaceMappingChanged = true;
                mTexAnimMode = 0;
            }

            if (value & 0x400)
            { //particle system (new)
                unpackParticleSource(*dp, mOwnerID, false);
            }
        }
        else
        {
            S32 texture_length = mesgsys->getSizeFast(_PREHASH_ObjectData, block_num, _PREHASH_TextureEntry);
            if (texture_length)
            {
                U8                          tdpbuffer[1024];
                LLDataPackerBinaryBuffer    tdp(tdpbuffer, 1024);
                mesgsys->getBinaryDataFast(_PREHASH_ObjectData, _PREHASH_TextureEntry, tdpbuffer, 0, block_num, 1024);
                S32 result = unpackTEMessage(tdp);
                if (result & teDirtyBits)
                {
                    if (mDrawable)
                    { //on the fly TE updates break batches, isolate in octree
                        shrinkWrap();
                    }
                }
                if (result & TEM_CHANGE_MEDIA)
                {
                    retval |= MEDIA_FLAGS_CHANGED;
                }
            }
        }
    }
    if (retval & (MEDIA_URL_REMOVED | MEDIA_URL_ADDED | MEDIA_URL_UPDATED | MEDIA_FLAGS_CHANGED))
    {
        // If only the media URL changed, and it isn't a media version URL,
        // ignore it
        if ( ! ( retval & (MEDIA_URL_ADDED | MEDIA_URL_UPDATED) &&
                 mMedia && ! mMedia->mMediaURL.empty() &&
                 ! LLTextureEntry::isMediaVersionString(mMedia->mMediaURL) ) )
        {
            // If the media changed at all, request new media data
            LL_DEBUGS("MediaOnAPrim") << "Media update: " << getID() << ": retval=" << retval << " Media URL: " <<
                ((mMedia) ?  mMedia->mMediaURL : std::string("")) << LL_ENDL;
            requestMediaDataUpdate(retval & MEDIA_FLAGS_CHANGED);
        }
        else {
            LL_INFOS("MediaOnAPrim") << "Ignoring media update for: " << getID() << " Media URL: " <<
                ((mMedia) ?  mMedia->mMediaURL : std::string("")) << LL_ENDL;
        }
    }
    // ...and clean up any media impls
    cleanUpMediaImpls();

    if ((
            (mVolumeChanged && !previously_volume_changed) ||
            (mFaceMappingChanged && !previously_face_mapping_changed) ||
            (mColorChanged && !previously_color_changed)
        )
        && !mLODChanged) {
        onDrawableUpdateFromServer();
    }

    return retval;
}

// Called when a volume, material, etc is updated by the server, possibly by a
// script. If this occurs too often for this object, mark it as active so that
// it doesn't disrupt the octree/render batches, thereby potentially causing a
// big performance penalty.
void LLVOVolume::onDrawableUpdateFromServer()
{
    constexpr U32 UPDATES_UNTIL_ACTIVE = 8;
    ++mServerDrawableUpdateCount;
    if (mDrawable && !mDrawable->isActive() && mServerDrawableUpdateCount > UPDATES_UNTIL_ACTIVE)
    {
        mDrawable->makeActive();
    }
}

void LLVOVolume::updateTextures()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    updateTextureVirtualSize();
}

bool LLVOVolume::isVisible() const
{
    if(mDrawable.notNull() && mDrawable->isVisible())
    {
        return true ;
    }

    if(isAttachment())
    {
        LLViewerObject* objp = (LLViewerObject*)getParent() ;
        while(objp && !objp->isAvatar())
        {
            objp = (LLViewerObject*)objp->getParent() ;
        }

        return objp && objp->mDrawable.notNull() && objp->mDrawable->isVisible() ;
    }

    return false ;
}

// VK texture-streaming visibility. LL gates texture loading on LLDrawable::
// isVisible(), which LL's octree cull maintains every frame -- a cull the Vulkan
// frame never runs, so "visible" was whatever stale octree-group state a drawable
// happened to sit in: nearby objects (and some HUDs) were treated as invisible and
// their textures never loaded. Decide it directly: attachments (worn + HUD) always,
// everything else by the object's spatial bounds against the live camera frustum
// (refreshed each VK frame). Unknown bounds -> visible, so nothing is starved.
static bool vkTextureStreamVisible(const LLVOVolume* vobj, LLDrawable* drawable)
{
    if (vobj->isHUDAttachment() || vobj->isAttachment())
        return true;
    const LLVector4a* ext = drawable->getSpatialExtents();
    LLVector4a center, radius;
    center.setAdd(ext[0], ext[1]);
    center.mul(0.5f);
    radius.setSub(ext[1], ext[0]);
    radius.mul(0.5f);
    if (!(radius.getLength3().getF32() > 0.f))
        return true;
    return LLViewerCamera::getInstance()->AABBInFrustum(center, radius) != 0;
}

void LLVOVolume::updateTextureVirtualSize(bool forced)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_VOLUME;
    // Update the pixel area of all faces

    if (mDrawable.isNull())
    {
        return;
    }

    if(!forced)
    {
        const bool visible = vkTextureStreamVisible(this, mDrawable);
        if(!visible)
        { //don't load textures for non-visible faces
            const S32 num_faces = mDrawable->getNumFaces();
            for (S32 i = 0; i < num_faces; i++)
            {
                LLFace* face = mDrawable->getFace(i);
                if (face)
                {
                    face->setPixelArea(0.f);
                    face->setVirtualSize(0.f);
                }
            }

            return ;
        }

        if (!gPipeline.hasRenderType(LLPipeline::RENDER_TYPE_SIMPLE))
        {
            return;
        }
    }

    static LLCachedControl<bool> dont_load_textures(gSavedSettings,"TextureDisable", false);

    if (dont_load_textures || LLAppViewer::getTextureFetch()->mDebugPause) // || !mDrawable->isVisible())
    {
        return;
    }

    mTextureUpdateTimer.reset();

    F32 old_area = mPixelArea;
    mPixelArea = 0.f;

    const S32 num_faces = mDrawable->getNumFaces();
    F32 min_vsize=999999999.f, max_vsize=0.f;
    LLViewerCamera* camera = LLViewerCamera::getInstance();
    std::stringstream debug_text;
    for (S32 i = 0; i < num_faces; i++)
    {
        LLFace* face = mDrawable->getFace(i);
        if (!face || face->mExtents[0].equals3(face->mExtents[1])) continue;
        const LLTextureEntry *te = face->getTextureEntry();
        if (!te) continue;

        LLViewerTexture *imagep = nullptr;
        U32 ch_min;
        U32 ch_max;
        if (!te->getGLTFRenderMaterial())
        {
            ch_min = LLRender::DIFFUSE_MAP;
            ch_max = LLRender::SPECULAR_MAP;
        }
        else
        {
            ch_min = LLRender::BASECOLOR_MAP;
            ch_max = LLRender::EMISSIVE_MAP;
        }
        for (U32 ch = ch_min; (!imagep && ch <= ch_max); ++ch)
        {
            // Get _a_ non-null texture if possible (usually diffuse/basecolor, but could be something else)
            imagep = face->getTexture(ch);
        }
        if (!imagep)
        {
            continue;
        }

        F32 vsize;
        F32 old_size = face->getVirtualSize();

        if (isHUDAttachment())
        {
            F32 area = (F32) camera->getScreenPixelArea();
            vsize = area;
            imagep->setBoostLevel(LLGLTexture::BOOST_HUD);
            face->setPixelArea(area); // treat as full screen
            face->setVirtualSize(vsize);
        }
        else
        {
            vsize = face->getTextureVirtualSize();
        }

        mPixelArea = llmax(mPixelArea, face->getPixelArea());

        // if the face has gotten small enough to turn off texture animation and texture
        // animation is running, rebuild the render batch for this face to turn off
        // texture animation
        // Do the opposite when the face gets big enough.
        if (gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_TEXTURE_PRIORITY))
        {
            LLViewerFetchedTexture* img = LLViewerTextureManager::staticCastToFetchedTexture(imagep) ;
            if(img)
            {
                debug_text << img->getDiscardLevel() << ":" << img->getDesiredDiscardLevel() << ":" << img->getWidth() << ":" << (S32) sqrtf(vsize) << ":" << (S32) sqrtf(img->getMaxVirtualSize());
                // #684: the Vulkan side -- resident size / mip below full / requested / why / format.
                const std::string vk_res = LLVulkanContext::instance().texResidencyBrief(img->getID());
                if (!vk_res.empty()) debug_text << " | " << vk_res;
                debug_text << "\n";
                /*F32 pri = img->getDecodePriority();
                pri = llmax(pri, 0.0f);
                if (pri < min_vsize) min_vsize = pri;
                if (pri > max_vsize) max_vsize = pri;*/
            }
        }
        else if (gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_FACE_AREA))
        {
            F32 pri = mPixelArea;
            if (pri < min_vsize) min_vsize = pri;
            if (pri > max_vsize) max_vsize = pri;
        }
    }

    if (isSculpted())
    {
        updateSculptTexture();



        if (mSculptTexture.notNull())
        {
            mSculptTexture->setForSculpt() ;

            S32 texture_discard = mSculptTexture->getRawImageLevel(); //try to match the texture
            S32 current_discard = getVolume() ? getVolume()->getSculptLevel() : -2 ;

            if (texture_discard >= 0 && //texture has some data available
                (texture_discard < current_discard || //texture has more data than last rebuild
                current_discard < 0)) //no previous rebuild
            {
                gPipeline.markRebuild(mDrawable, LLDrawable::REBUILD_VOLUME);
                mSculptChanged = true;
            }

            if (gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_SCULPTED))
            {
                setDebugText(llformat("T%d C%d V%d\n%dx%d",
                                          texture_discard, current_discard, getVolume()->getSculptLevel(),
                                          mSculptTexture->getHeight(), mSculptTexture->getWidth()));
            }
        }

    }

    if (getLightTextureID().notNull())
    {
        LLLightImageParams* params = (LLLightImageParams*) getParameterEntry(LLNetworkData::PARAMS_LIGHT_IMAGE);
        LLUUID id = params->getLightTexture();
        mLightTexture = LLViewerTextureManager::getFetchedTexture(id, FTT_DEFAULT, true, LLGLTexture::BOOST_NONE);
        if (mLightTexture.notNull())
        {
            F32 rad = getLightRadius();
            mLightTexture->addTextureStats(gPipeline.calcPixelArea(getPositionAgent(),
                                                                    LLVector3(rad,rad,rad),
                                                                    *camera));
        }
    }

    if (gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_TEXTURE_AREA))
    {
        setDebugText(llformat("%.0f:%.0f", (F32) sqrt(min_vsize),(F32) sqrt(max_vsize)));
    }
    else if (gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_TEXTURE_PRIORITY))
    {
        //setDebugText(llformat("%.0f:%.0f", (F32) sqrt(min_vsize),(F32) sqrt(max_vsize)));
        setDebugText(debug_text.str());
    }
    else if (gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_FACE_AREA))
    {
        setDebugText(llformat("%.0f:%.0f", (F32) sqrt(min_vsize),(F32) sqrt(max_vsize)));
    }

    if (mPixelArea == 0)
    { //flexi phasing issues make this happen
        mPixelArea = old_area;
    }
}

bool LLVOVolume::isActive() const
{
    return !mStatic;
}

bool LLVOVolume::setMaterial(const U8 material)
{
    bool res = LLViewerObject::setMaterial(material);

    return res;
}

void LLVOVolume::setTexture(const S32 face)
{
    llassert(face < getNumTEs());
    gGL.getTexUnit(0)->bind(getTEImage(face));
}

void LLVOVolume::setScale(const LLVector3 &scale, bool damped)
{
    if (scale != getScale())
    {
        // store local radius
        LLViewerObject::setScale(scale);

        if (mVolumeImpl)
        {
            mVolumeImpl->onSetScale(scale, damped);
        }

        updateRadius();

        //since drawable transforms do not include scale, changing volume scale
        //requires an immediate rebuild of volume verts.
        gPipeline.markRebuild(mDrawable, LLDrawable::REBUILD_POSITION);

        if (mDrawable)
        {
            shrinkWrap();
        }
    }
}

LLFace* LLVOVolume::addFace(S32 f)
{
    const LLTextureEntry* te = getTE(f);
    LLViewerTexture* imagep = getTEImage(f);
    if (te->getMaterialParams().notNull())
    {
        LLViewerTexture* normalp = getTENormalMap(f);
        LLViewerTexture* specularp = getTESpecularMap(f);
        return mDrawable->addFace(te, imagep, normalp, specularp);
    }
    return mDrawable->addFace(te, imagep);
}

LLDrawable *LLVOVolume::createDrawable(LLPipeline *pipeline)
{
    pipeline->allocDrawable(this);

    mDrawable->setRenderType(LLPipeline::RENDER_TYPE_VOLUME);

    S32 max_tes_to_set = getNumTEs();
    for (S32 i = 0; i < max_tes_to_set; i++)
    {
        addFace(i);
    }
    mNumFaces = max_tes_to_set;

    if (isAttachment())
    {
        mDrawable->makeActive();
    }

    if (getIsLight())
    {
        // Add it to the pipeline mLightSet
        gPipeline.setLight(mDrawable, true);
    }

    if (isReflectionProbe())
    {
        updateReflectionProbePtr();
    }

    updateRadius();
    bool force_update = true; // avoid non-alpha mDistance update being optimized away
    mDrawable->updateDistance(*LLViewerCamera::getInstance(), force_update);

    return mDrawable;
}

bool LLVOVolume::setVolume(const LLVolumeParams &params_in, const S32 detail, bool unique_volume)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_VOLUME;
    LLVolumeParams volume_params = params_in;

    S32 last_lod = mVolumep.notNull() ? LLVolumeLODGroup::getVolumeDetailFromScale(mVolumep->getDetail()) : -1;
    S32 lod = mLOD;

    bool is404 = false;

    if (isSculpted())
    {
        // if it's a mesh
        if ((volume_params.getSculptType() & LL_SCULPT_TYPE_MASK) == LL_SCULPT_TYPE_MESH)
        { //meshes might not have all LODs, get the force detail to best existing LOD
            if (NO_LOD != lod)
            {
                lod = gMeshRepo.getActualMeshLOD(volume_params, lod);
                if (lod == -1)
                {
                    is404 = true;
                    lod = 0;
                }
            }
        }
    }

    // Check if we need to change implementations
    bool is_flexible = (volume_params.getPathParams().getCurveType() == LL_PCODE_PATH_FLEXIBLE);
    if (is_flexible)
    {
        setParameterEntryInUse(LLNetworkData::PARAMS_FLEXIBLE, true, false);
        if (!mVolumeImpl)
        {
            LLFlexibleObjectData* data = (LLFlexibleObjectData*)getParameterEntry(LLNetworkData::PARAMS_FLEXIBLE);
            mVolumeImpl = new LLVolumeImplFlexible(this, data);
        }
    }
    else
    {
        // Mark the parameter not in use
        setParameterEntryInUse(LLNetworkData::PARAMS_FLEXIBLE, false, false);
        if (mVolumeImpl)
        {
            delete mVolumeImpl;
            mVolumeImpl = NULL;
            if (mDrawable.notNull())
            {
                // Undo the damage we did to this matrix
                mDrawable->updateXform(false);
            }
        }
    }

    if (is404)
    {
        setIcon(LLViewerTextureManager::getFetchedTextureFromFile("icons/Inv_Mesh.png", FTT_LOCAL_FILE, true, LLGLTexture::BOOST_UI));
        //render prim proxy when mesh loading attempts give up
        volume_params.setSculptID(LLUUID::null, LL_SCULPT_TYPE_NONE);

    }

    if ((LLPrimitive::setVolume(volume_params, lod, (mVolumeImpl && mVolumeImpl->isVolumeUnique()))) || mSculptChanged)
    {
        mFaceMappingChanged = true;

        if (mVolumeImpl)
        {
            mVolumeImpl->onSetVolume(volume_params, mLOD);
        }

        updateSculptTexture();

        if (isSculpted())
        {
            updateSculptTexture();
            // if it's a mesh
            if ((volume_params.getSculptType() & LL_SCULPT_TYPE_MASK) == LL_SCULPT_TYPE_MESH)
            {
                if (mSkinInfo && mSkinInfo->mMeshID != volume_params.getSculptID())
                {
                    mSkinInfo = NULL;
                    mSkinInfoUnavaliable = false;
                }

                if (!getVolume()->isMeshAssetLoaded())
                {
                    //load request not yet issued, request pipeline load this mesh
                    S32 available_lod = gMeshRepo.loadMesh(this, volume_params, lod, last_lod);
                    if (available_lod != lod)
                    {
                        LLPrimitive::setVolume(volume_params, available_lod);
                    }
                }

                if (!mSkinInfo && !mSkinInfoUnavaliable)
                {
                    LLUUID mesh_id = volume_params.getSculptID();
                    if (gMeshRepo.hasHeader(mesh_id) && !gMeshRepo.hasSkinInfo(mesh_id))
                    {
                        // If header is present but has no data about skin,
                        // no point fetching
                        mSkinInfoUnavaliable = true;
                    }

                    if (!mSkinInfoUnavaliable)
                    {
                        const LLMeshSkinInfo* skin_info = gMeshRepo.getSkinInfo(mesh_id, this);
                        if (skin_info)
                        {
                            notifySkinInfoLoaded(skin_info);
                        }
                    }
                }
            }
            else // otherwise is sculptie
            {
                if (mSculptTexture.notNull())
                {
                    sculpt();
                }
            }
        }

        if ((volume_params.getSculptType() & LL_SCULPT_TYPE_MASK) == LL_SCULPT_TYPE_GLTF)
        { // notify GLTFSceneManager about new GLTF object
            LL::GLTFSceneManager::instance().addGLTFObject(this, volume_params.getSculptID());
        }

        return true;
    }
    else if (NO_LOD == lod)
    {
        LLSculptIDSize::instance().resetSizeSum(volume_params.getSculptID());
    }

    return false;
}

void LLVOVolume::updateSculptTexture()
{
    LLPointer<LLViewerFetchedTexture> old_sculpt = mSculptTexture;

    if (isSculpted() && !isMesh())
    {
        LLSculptParams *sculpt_params = (LLSculptParams *)getParameterEntry(LLNetworkData::PARAMS_SCULPT);
        if (sculpt_params)
        {
            LLUUID id = sculpt_params->getSculptTexture();
            if (id.notNull())
            {
                mSculptTexture = LLViewerTextureManager::getFetchedTexture(id, FTT_DEFAULT, true, LLGLTexture::BOOST_SCULPTED, LLViewerTexture::LOD_TEXTURE);
                mSculptTexture->forceToSaveRawImage(0, F32_MAX);
                mSculptTexture->setKnownDrawSize(256, 256);
            }
        }

        mSkinInfoUnavaliable = false;
        mSkinInfo = NULL;
    }
    else
    {
        mSculptTexture = NULL;
    }

    if (mSculptTexture != old_sculpt)
    {
        if (old_sculpt.notNull())
        {
            old_sculpt->removeVolume(LLRender::SCULPT_TEX, this);
        }
        if (mSculptTexture.notNull())
        {
            mSculptTexture->addVolume(LLRender::SCULPT_TEX, this);
        }
    }

}

void LLVOVolume::updateVisualComplexity()
{
    LLVOAvatar* avatar = getAvatarAncestor();
    if (avatar)
    {
        avatar->updateVisualComplexity();
    }
    LLVOAvatar* rigged_avatar = getAvatar();
    if(rigged_avatar && (rigged_avatar != avatar))
    {
        rigged_avatar->updateVisualComplexity();
    }
}

void LLVOVolume::notifyMeshLoaded()
{
    mSculptChanged = true;
    gPipeline.markRebuild(mDrawable, LLDrawable::REBUILD_GEOMETRY);

    if (!mSkinInfo && !mSkinInfoUnavaliable)
    {
        // Header was loaded, update skin info state from header
        LLUUID mesh_id = getVolume()->getParams().getSculptID();
        if (!gMeshRepo.hasSkinInfo(mesh_id))
        {
            mSkinInfoUnavaliable = true;
        }
    }

    LLVOAvatar *av = getAvatar();
    if (av && !isAnimatedObject())
    {
        av->addAttachmentOverridesForObject(this);
        av->notifyAttachmentMeshLoaded();
    }
    LLControlAvatar *cav = getControlAvatar();
    if (cav && isAnimatedObject())
    {
        cav->addAttachmentOverridesForObject(this);
        cav->notifyAttachmentMeshLoaded();
    }
    updateVisualComplexity();
}

void LLVOVolume::notifySkinInfoLoaded(const LLMeshSkinInfo* skin)
{
    mSkinInfoUnavaliable = false;
    mSkinInfo = skin;

    notifyMeshLoaded();
}

void LLVOVolume::notifySkinInfoUnavailable()
{
    mSkinInfoUnavaliable = true;
    mSkinInfo = nullptr;
}

// VK: sculpt maps pending full-res decode. Driven EVERY frame (vkDriveSculptFetch)
// rather than only when the amortized sweep visits the object, so their fetch priority
// stays high continuously and the decode actually completes to discard 0. Without this
// a sculpt map's virtual size (reset each frame) is only re-asserted ~once per full
// sweep cycle (~tens of frames), so the fetcher deprioritizes it and it stalls coarse
// (the tree-silhouette strips stuck at saved discard 2 = the remaining surround fan).
static std::set<LLPointer<LLViewerFetchedTexture> > s_vk_pending_sculpt_maps;

static void vk_clear_pending_sculpt_maps()
{
    s_vk_pending_sculpt_maps.clear();
}

void LLVOVolume::vkDriveSculptFetch()
{
    for (auto it = s_vk_pending_sculpt_maps.begin(); it != s_vk_pending_sculpt_maps.end(); )
    {
        LLViewerFetchedTexture* t = it->get();
        if (!t || t->getSavedRawImageLevel() == 0)
        {
            it = s_vk_pending_sculpt_maps.erase(it);   // done (full res) or dead
            continue;
        }
        // needs_gltexture=false: a sculpt map is a displacement source, not a drawn
        // texture -- matches LL's own sculpt stat path (keeps isForSculptOnly()).
        t->addTextureStats(1024.f * 1024.f, false);    // sustain fetch priority, sculpt-only
        ++it;
    }
}

bool LLVOVolume::vkEnsureSculptDetail(S32 target_lod)
{
    (void)target_lod;
    if (!isSculpted() || isMesh()) return false;
    LLVolume* v = getVolume();
    if (!v) return false;
    const U8 stype = (U8)(v->getParams().getSculptType() & LL_SCULPT_TYPE_MASK);
    if (stype == LL_SCULPT_TYPE_MESH || stype == LL_SCULPT_TYPE_GLTF) return false; // image sculpts only
    if (mSculptTexture.isNull()) return false;

    // SCULPT-MAP FETCH DRIVE. A sculpt map is a DISPLACEMENT source with no face in the
    // render, so the face-area virtual-size driver (skipped for BOOST>=BOOST_HIGH in
    // updateImageDecodePriority) never raises its fetch priority, and mMaxVirtualSize is
    // reset every frame -> the fetcher deprioritizes it and the decode STALLS coarse
    // (savedlvl stuck at 2 for the large surround panels -> coarse/collapsed plane
    // displacement = the sim-surround fan). Re-assert a large virtual size each sweep
    // visit so the fetcher drives the forceToSaveRawImage(0) decode to full res (0).
    mSculptTexture->addTextureStats(1024.f * 1024.f, false); // sculpt-only: keep isForSculptOnly() true
    // Also register it for the EVERY-FRAME drive so its priority stays high between
    // sweep visits (the per-visit boost above is only re-asserted ~1/cycle, too rarely
    // to complete the decode -- why 2/3 of the surround stayed at saved discard 2).
    if (mSculptTexture->getSavedRawImageLevel() != 0)
    {
        s_vk_pending_sculpt_maps.insert(mSculptTexture);
    }

    // Replicate stock's sculpt re-displacement TRIGGER, which lives in the diverted
    // updateGeometry (llvovolume.cpp ~928-936): re-run sculpt() ONLY when the sculpt
    // map has decoded to a finer level than the current volume displacement (or the
    // volume has no real displacement yet). This is gated -- never an unconditional
    // sculpt(), which would displace with no data and produce the degenerate
    // collapse-to-point (the sim-surround fan). The texture BOOST/knownDrawSize/
    // forceToSaveRawImage are set by updateSculptTexture at creation (sticky); the
    // restored stock texture subsystem in doFrame() now actually drives the fetch to
    // full res, so the map improves over time and this trigger ingests it. Under VK
    // the live raw is a placeholder, so gate on the SAVED raw level (what sculpt()'s
    // VK branch actually displaces from).
    // GRID-DETAIL rebuild (the surround-fan ROOT): under VK the diverted updateGeometry
    // never calls setVolume for sculpts, so the sculpt GRID stays at the coarse detail it
    // was created with at arrival (~50 verts) even though getLOD() is now 3. A fine
    // (512-column) tree-silhouette map then subsamples onto ~7 grid columns and stretches
    // into long slivers = the sim-surround fan. setVolume(params, getLOD()) rebuilds the
    // LLVolume at mLOD detail (LLPrimitive::setVolume uses mLOD, line 1081) and re-runs
    // sculpt() on the NEW grid internally for image sculpties (line 1194). Only fire when
    // the grid is actually below the LOD (setVolume no-ops when detail already matches).
    const S32 grid_lod = LLVolumeLODGroup::getVolumeDetailFromScale(v->getDetail());
    if (grid_lod < getLOD())
    {
        setVolume(v->getParams(), getLOD());
    }
    else
    {
        // Grid already at LOD; just re-displace if the map decoded finer.
        const S32 texture_discard = mSculptTexture->getSavedRawImageLevel();
        const S32 current_discard = v->getSculptLevel();
        if (texture_discard >= 0 && (current_discard < 0 || texture_discard < current_discard))
        {
            sculpt();
        }
    }

    return false;
}

// sculpt replaces generate() for sculpted surfaces
void LLVOVolume::sculpt()
{
    if (mSculptTexture.notNull())
    {
        U16 sculpt_height = 0;
        U16 sculpt_width = 0;
        S8 sculpt_components = 0;
        const U8* sculpt_data = NULL;

        S32 discard_level = mSculptTexture->getRawImageLevel() ;
        LLImageRaw* raw_image = mSculptTexture->getRawImage() ;

        if (!raw_image)
        {
            raw_image = mSculptTexture->getSavedRawImage();
            discard_level = mSculptTexture->getSavedRawImageLevel();
        }

        {
            // There is no GL texture, so the LIVE raw is a 1x1 placeholder
            // (getRawImageLevel() == 32767). The stock path let that 32767 poison
            // discard_level, then bails at the "data not ready" return below -> sculpt()
            // never displaces, the volume stays at placeholder (sculpt_lvl == -1), and a
            // plane sculpt displaced from a placeholder collapses its stitch seam into a
            // fan (the sim-surround corruption). Use the SAVED raw (kept resident by
            // forceToSaveRawImage) at its real level directly.
            const S32   slvl = mSculptTexture->getSavedRawImageLevel();
            LLImageRaw* sraw = mSculptTexture->getSavedRawImage();
            if (sraw && sraw->getData() && slvl >= 0)
            {
                raw_image     = sraw;
                discard_level = slvl;
            }
        }

        S32 max_discard = mSculptTexture->getMaxDiscardLevel();
        if (discard_level > max_discard)
        {
            discard_level = max_discard;    // clamp to the best we can do
        }
        if(discard_level > MAX_DISCARD_LEVEL)
        {
            return; //we think data is not ready yet.
        }

        S32 current_discard = getVolume()->getSculptLevel() ;
        if(current_discard < -2)
        {
            static S32 low_sculpty_discard_warning_count = 1;
            S32 exponent = llmax(1, llfloor((F32)log10((F64) low_sculpty_discard_warning_count)));
            S32 interval = (S32)pow(10.0, exponent);
            if ( low_sculpty_discard_warning_count < 10 ||
                (low_sculpty_discard_warning_count % interval) == 0)
            {   // Log first 10 time, then decreasing intervals afterwards otherwise this can flood the logs
                LL_WARNS() << "WARNING!!: Current discard for sculpty " << mSculptTexture->getID()
                    << " at " << current_discard
                    << " is less than -2."
                    << " Hit this " << low_sculpty_discard_warning_count << " times"
                    << LL_ENDL;
            }
            low_sculpty_discard_warning_count++;

            // corrupted volume... don't update the sculpty
            return;
        }
        else if (current_discard > MAX_DISCARD_LEVEL)
        {
            static S32 high_sculpty_discard_warning_count = 1;
            S32 exponent = llmax(1, llfloor((F32)log10((F64) high_sculpty_discard_warning_count)));
            S32 interval = (S32)pow(10.0, exponent);
            if ( high_sculpty_discard_warning_count < 10 ||
                (high_sculpty_discard_warning_count % interval) == 0)
            {   // Log first 10 time, then decreasing intervals afterwards otherwise this can flood the logs
                LL_WARNS() << "WARNING!!: Current discard for sculpty " << mSculptTexture->getID()
                    << " at " << current_discard
                    << " is more than than allowed max of " << MAX_DISCARD_LEVEL
                    << ".  Hit this " << high_sculpty_discard_warning_count << " times"
                    << LL_ENDL;
            }
            high_sculpty_discard_warning_count++;

            // corrupted volume... don't update the sculpty
            return;
        }

        if (current_discard == discard_level)  // no work to do here
            return;

        if(!raw_image)
        {
            // #29 sim-surround fan ROOT: under VK the sculpt map often hasn't arrived
            // yet (no live raw -- GL isn't loaded -- and no saved raw). Displacing with
            // sculpt_width=0 / null data makes LLVolume::sculpt generate a DEGENERATE
            // collapse-to-point grid, which our sweep captures and latches -> the fan.
            // Stock avoids this by only requesting a rebuild once the map has data
            // (updateGeometry: texture_discard >= 0). Defer: leave the volume as-is
            // until the map lands; a real displacement + the level-improvement recapture
            // then heals it. isMissingAsset() still falls through to the intended blob.
            if (!mSculptTexture->isMissingAsset())
            {
                LL_INFOS_ONCE("VKSculpt") << "sculpt(): no map data yet -- deferring displacement (id="
                    << mSculptTexture->getID().asString().substr(0,8) << ")" << LL_ENDL;
                return;
            }

            sculpt_width = 0;
            sculpt_height = 0;
            sculpt_data = NULL ;

            if(LLViewerTextureManager::sTesterp)
            {
                LLViewerTextureManager::sTesterp->updateGrayTextureBinding();
            }
        }
        else
        {
            LLImageDataSharedLock lock(raw_image);

            sculpt_height = raw_image->getHeight();
            sculpt_width = raw_image->getWidth();
            sculpt_components = raw_image->getComponents();

            sculpt_data = raw_image->getData();

            if(LLViewerTextureManager::sTesterp)
            {
                mSculptTexture->updateBindStatsForTester() ;
            }
        }

        getVolume()->sculpt(sculpt_width, sculpt_height, sculpt_components, sculpt_data, discard_level, mSculptTexture->isMissingAsset());
    }
}

S32 LLVOVolume::computeLODDetail(F32 distance, F32 radius, F32 lod_factor)
{
    S32 cur_detail;
    if (LLPipeline::sDynamicLOD)
    {
        // We've got LOD in the profile, and in the twist.  Use radius.
        F32 tan_angle = (lod_factor*radius)/distance;
        cur_detail = LLVolumeLODGroup::getDetailFromTan(ll_round(tan_angle, 0.01f));
    }
    else
    {
        cur_detail = llclamp((S32) (sqrtf(radius)*lod_factor*4.f), 0, 3);
    }
    return cur_detail;
}

std::string get_debug_object_lod_text(LLVOVolume *rootp)
{
    std::string cam_dist_string = "";
    cam_dist_string += LLStringOps::getReadableNumber(rootp->mLODDistance) +  " ";
    std::string lod_string = llformat("%d",rootp->getLOD());
    F32 lod_radius = rootp->mLODRadius;
    S32 cam_dist_count = 0;
    LLViewerObject::const_child_list_t& child_list = rootp->getChildren();
    for (LLViewerObject::const_child_list_t::const_iterator iter = child_list.begin();
         iter != child_list.end(); ++iter)
    {
        LLViewerObject *childp = *iter;
        LLVOVolume *volp = dynamic_cast<LLVOVolume*>(childp);
        if (volp)
        {
            lod_string += llformat("%d",volp->getLOD());
            if (volp->isRiggedMesh())
            {
                // Rigged/animatable mesh. This is computed from the
                // avatar dynamic box, so value from any vol will be
                // the same.
                lod_radius = volp->mLODRadius;
            }
            if (volp->mDrawable)
            {
                if (cam_dist_count < 4)
                {
                    cam_dist_string += LLStringOps::getReadableNumber(volp->mLODDistance) +  " ";
                    cam_dist_count++;
                }
            }
        }
    }
    std::string result = llformat("lod_radius %s dists %s lods %s",
                                  LLStringOps::getReadableNumber(lod_radius).c_str(),
                                  cam_dist_string.c_str(),
                                  lod_string.c_str());
    return result;
}

bool LLVOVolume::calcLOD()
{
    if (mDrawable.isNull())
    {
        return false;
    }

    if (mGLTFAsset != nullptr)
    {
        // do not calculate LOD for GLTF objects
        return false;
    }

    S32 cur_detail = 0;

    F32 radius;
    F32 distance;
    F32 lod_factor = LLVOVolume::sLODFactor;

    if (mDrawable->isState(LLDrawable::RIGGED))
    {
        LLVOAvatar* avatar = getAvatar();

        // Not sure how this can really happen, but alas it does. Better exit here than crashing.
        if( !avatar || !avatar->mDrawable )
        {
            return false;
        }

        distance = avatar->mDrawable->mDistanceWRTCamera;


        if (avatar->isControlAvatar())
        {
            // MAINT-7926 Handle volumes in an animated object as a special case
            const LLVector3* box = avatar->getLastAnimExtents();
            LLVector3 diag = box[1] - box[0];
            radius = diag.magVec() * 0.5f;
            LL_DEBUGS("DynamicBox") << avatar->getDebugName() << " diag " << diag << " radius " << radius << LL_ENDL;
        }
        else
        {
            // Volume in a rigged mesh attached to a regular avatar.
            // Note this isn't really a radius, so distance calcs are off by factor of 2
            //radius = avatar->getBinRadius();
            // SL-937: add dynamic box handling for rigged mesh on regular avatars.
            const LLVector3* box = avatar->getLastAnimExtents();
            LLVector3 diag = box[1] - box[0];
            radius = diag.magVec(); // preserve old BinRadius behavior - 2x off
            LL_DEBUGS("DynamicBox") << avatar->getDebugName() << " diag " << diag << " radius " << radius << LL_ENDL;
        }
        if (distance <= 0.f || radius <= 0.f)
        {
            return false;
        }
    }
    else
    {
        distance = mDrawable->mDistanceWRTCamera;
        radius = getVolume() ? getVolume()->mLODScaleBias.scaledVec(getScale()).length() : getScale().length();
        if (distance <= 0.f || radius <= 0.f)
        {
            return false;
        }
    }

    // VK reduced-detail avatar tier (#31): their rigged mesh and worn attachments LOD
    // out sooner. (The cull applies the same bias to what is resident; this makes LL
    // fetch and the ingest capture the coarser level.)
    if (LLVOAvatar* tier_av = getAvatar(); tier_av && tier_av->isVKReduced() && !isHUDAttachment())
    {
        lod_factor *= LLVOAvatar::vkReducedLODScale();
    }

    //hold onto unmodified distance for debugging
    //F32 debug_distance = distance;

    mLODDistance = distance;
    mLODRadius = radius;

    static LLCachedControl<bool> debug_lods(gSavedSettings, "DebugObjectLODs", false);
    if (debug_lods)
    {
        if (getAvatar() && isRootEdit())
        {
            std::string debug_object_text = get_debug_object_lod_text(this);
            setDebugText(debug_object_text);
            mResetDebugText = true;
        }
    }
    else
    {
        if (mResetDebugText)
        {
            restoreHudText();
            mResetDebugText = false;
        }
    }

    distance *= sDistanceFactor;

    F32 rampDist = LLVOVolume::sLODFactor * 2;

    if (distance < rampDist)
    {
        // Boost LOD when you're REALLY close
        distance *= 1.0f/rampDist;
        distance *= distance;
        distance *= rampDist;
    }


    distance *= F_PI/3.f;

    static LLCachedControl<bool> ignore_fov_zoom(gSavedSettings,"IgnoreFOVZoomForLODs");
    if(!ignore_fov_zoom)
    {
        lod_factor *= DEFAULT_FIELD_OF_VIEW / LLViewerCamera::getInstance()->getDefaultFOV();
    }

    mLODAdjustedDistance = distance;

    static LLCachedControl<S32> debug_selection_lods(gSavedSettings, "DebugSelectionLODs", 0);
    if (isHUDAttachment())
    {
        // HUDs always show at highest detail
        cur_detail = 3;
    }
    else if (isSelected() && debug_selection_lods() >= 0)
    {
        cur_detail = llmin(debug_selection_lods(), 3);
    }
    else
    {
        cur_detail = computeLODDetail(ll_round(distance, 0.01f), ll_round(radius, 0.01f), lod_factor);
        // #559: the VK renderer's rigged-capacity pressure (far avatars first) caps the
        // wearer's rigged mesh, so the coarser level gets loaded and drawn.
        if (mDrawable->isState(LLDrawable::RIGGED))
        {
            // #798: on the VK cluster path a rigged mesh is drawn from a cluster DAG built from its
            // loaded creator LOD -- the DAG's cut does the screen-size reduction, so load the highest
            // one (where the creators' seams between meshes coincide and weld) at every distance.
            const S32 cl_lod = LLVOAvatar::vkClusterRiggedLOD();
            const LLVOAvatar* pav = getAvatar();
            if (cl_lod >= 0)
            {
                cur_detail = llclamp(cl_lod, 0, 3);
            }
            else if (pav && pav->vkPressureLOD() >= 0)
            {
                cur_detail = llmin(cur_detail, pav->vkPressureLOD());
            }
        }
    }

    if (gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_TRIANGLE_COUNT) && mDrawable->getFace(0))
    {
        if (isRootEdit())
        {
            S32 total_tris = recursiveGetTriangleCount();
            S32 est_max_tris = (S32)recursiveGetEstTrianglesMax();
            setDebugText(llformat("TRIS SHOWN %d EST %d", total_tris, est_max_tris));
        }
    }
    if (gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_LOD_INFO) &&
        mDrawable->getFace(0))
    {
        // This is a debug display for LODs. Please don't put the texture index here.
        setDebugText(llformat("%d", cur_detail));
    }

    // <GMDX> REGION MAP CAPTURE (gmdxregioncapture.h): everything standing in the region being mapped
    // loads its highest LOD while the capture runs (not avatars, attachments or HUDs).
    if (cur_detail < 3 && !isAttachment() && !mDrawable->isState(LLDrawable::RIGGED)
        && GMDXRegionCapture::forceMaxLOD(mDrawable->getPositionAgent(), mDrawable->getRadius()))
    {
        cur_detail = 3;
    }

    if (cur_detail != mLOD)
    {
        mAppAngle = ll_round((F32) atan2( mDrawable->getRadius(), mDrawable->mDistanceWRTCamera) * RAD_TO_DEG, 0.01f);
        mLOD = cur_detail;

        return true;
    }

    return false;
}

bool LLVOVolume::updateLOD()
{
    if (mDrawable.isNull())
    {
        return false;
    }

    LL_PROFILE_ZONE_SCOPED_CATEGORY_VOLUME;

    bool lod_changed = false;

    if (!LLSculptIDSize::instance().isUnloaded(getVolume()->getParams().getSculptID()))
    {
        lod_changed = calcLOD();
    }
    else
    {
        return false;
    }

    if (lod_changed)
    {
        gPipeline.markRebuild(mDrawable, LLDrawable::REBUILD_VOLUME);
        mLODChanged = true;
    }
    else
    {
        F32 new_radius = getBinRadius();
        F32 old_radius = mDrawable->getBinRadius();
        if (new_radius < old_radius * 0.9f || new_radius > old_radius*1.1f)
        {
            gPipeline.markPartitionMove(mDrawable);
        }
    }

    lod_changed = lod_changed || LLViewerObject::updateLOD();

    return lod_changed;
}

bool LLVOVolume::setDrawableParent(LLDrawable* parentp)
{
    if (!LLViewerObject::setDrawableParent(parentp))
    {
        // no change in drawable parent
        return false;
    }

    if (!mDrawable->isRoot())
    {
        // rebuild vertices in parent relative space
        gPipeline.markRebuild(mDrawable, LLDrawable::REBUILD_VOLUME);

        if (mDrawable->isActive() && !parentp->isActive())
        {
            parentp->makeActive();
        }
        else if (mDrawable->isStatic() && parentp->isActive())
        {
            mDrawable->makeActive();
        }
    }

    return true;
}

void LLVOVolume::updateFaceFlags()
{
    // There's no guarantee that getVolume()->getNumFaces() == mDrawable->getNumFaces()
    for (S32 i = 0; i < getVolume()->getNumFaces() && i < mDrawable->getNumFaces(); i++)
    {
        LLFace *face = mDrawable->getFace(i);
        if (face)
        {
            bool fullbright = getTE(i)->getFullbright();
            face->clearState(LLFace::FULLBRIGHT | LLFace::HUD_RENDER | LLFace::LIGHT);

            if (fullbright || (mMaterial == LL_MCODE_LIGHT))
            {
                face->setState(LLFace::FULLBRIGHT);
            }
            if (mDrawable->isLight())
            {
                face->setState(LLFace::LIGHT);
            }
            if (isHUDAttachment())
            {
                face->setState(LLFace::HUD_RENDER);
            }
        }
    }
}

bool LLVOVolume::setParent(LLViewerObject* parent)
{
    bool ret = false ;
    LLViewerObject *old_parent = (LLViewerObject*) getParent();
    if (parent != old_parent)
    {
        ret = LLViewerObject::setParent(parent);
        if (ret && mDrawable)
        {
            gPipeline.markMoved(mDrawable);
            gPipeline.markRebuild(mDrawable, LLDrawable::REBUILD_VOLUME);
        }
        onReparent(old_parent, parent);
    }

    return ret ;
}

// NOTE: regenFaces() MUST be followed by genTriangles()!
void LLVOVolume::regenFaces()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_VOLUME;
    // remove existing faces
    bool count_changed = mNumFaces != getNumTEs();

    if (count_changed)
    {
        deleteFaces();
        // add new faces
        mNumFaces = getNumTEs();
    }

    for (S32 i = 0; i < mNumFaces; i++)
    {
        LLFace* facep = count_changed ? addFace(i) : mDrawable->getFace(i);
        if (!facep) continue;

        facep->setTEOffset(i);
        facep->setTexture(getTEImage(i));
        if (facep->getTextureEntry()->getMaterialParams().notNull())
        {
            facep->setNormalMap(getTENormalMap(i));
            facep->setSpecularMap(getTESpecularMap(i));
        }
        facep->setViewerObject(this);

        // If the face had media on it, this will have broken the link between the LLViewerMediaTexture and the face.
        // Re-establish the link.
        if((int)mMediaImplList.size() > i)
        {
            if(mMediaImplList[i])
            {
                LLViewerMediaTexture* media_tex = LLViewerTextureManager::findMediaTexture(mMediaImplList[i]->getMediaTextureID()) ;
                if(media_tex)
                {
                    media_tex->addMediaToFace(facep) ;
                }
            }
        }
    }

    if (!count_changed)
    {
        updateFaceFlags();
    }
}

bool LLVOVolume::genBBoxes(bool force_global, bool should_update_octree_bounds)
{
    LL_PROFILE_ZONE_SCOPED;
    bool res = true;

    LLVector4a min, max;

    min.clear();
    max.clear();

    bool rebuild = mDrawable->isState(LLDrawable::REBUILD_VOLUME | LLDrawable::REBUILD_POSITION | LLDrawable::REBUILD_RIGGED);

    LLVolume* volume = getVolume();

    bool any_valid_boxes = false;

    // There's no guarantee that getVolume()->getNumFaces() == mDrawable->getNumFaces()
    for (S32 i = 0;
        i < getVolume()->getNumVolumeFaces() && i < mDrawable->getNumFaces() && i < getNumTEs();
        i++)
    {
        LLFace* face = mDrawable->getFace(i);
        if (!face)
        {
            continue;
        }

        bool face_res = face->genVolumeBBoxes(*volume, i,
            mRelativeXform,
            (mVolumeImpl && mVolumeImpl->isVolumeGlobal()) || force_global);
        res &= face_res; // note that this result is never used

        // MAINT-8264 - ignore bboxes of ill-formed faces.
        if (!face_res)
        {
            continue;
        }
        if (rebuild)
        {
            if (!any_valid_boxes)
            {
                min = face->mExtents[0];
                max = face->mExtents[1];
                any_valid_boxes = true;
            }
            else
            {
                min.setMin(min, face->mExtents[0]);
                max.setMax(max, face->mExtents[1]);
            }
        }
    }

    if (any_valid_boxes)
    {
        if (rebuild && should_update_octree_bounds)
        {
            //get the Avatar associated with this object if it's rigged
            LLVOAvatar* avatar = nullptr;
            if (isRiggedMesh())
            {
                if (!isAnimatedObject())
                {
                    if (isAttachment())
                    {
                        avatar = getAvatar();
                    }
                }
                else
                {
                    LLControlAvatar* controlAvatar = getControlAvatar();
                    if (controlAvatar && controlAvatar->mPlaying)
                    {
                        avatar = controlAvatar;
                    }
                }
            }

            mDrawable->setSpatialExtents(min, max);

            if (avatar)
            {
                // put all rigged drawables in the same octree node for better batching
                mDrawable->setPositionGroup(LLVector4a(0, 0, 0));
            }
            else
            {
                min.add(max);
                min.mul(0.5f);
                mDrawable->setPositionGroup(min);
            }
        }

        updateRadius();
        mDrawable->movePartition();
    }
    else
    {
        LL_DEBUGS("RiggedBox") << "genBBoxes failed to find any valid face boxes" << LL_ENDL;
    }

    return res;
}

void LLVOVolume::preRebuild()
{
    if (mVolumeImpl != NULL)
    {
        mVolumeImpl->preRebuild();
    }
}

void LLVOVolume::updateRelativeXform(bool force_identity)
{
    if (mVolumeImpl)
    {
        mVolumeImpl->updateRelativeXform(force_identity);
        return;
    }

    LLDrawable* drawable = mDrawable;

    if (drawable->isActive() || force_identity)
    {
        // setup relative transforms
        LLQuaternion delta_rot;
        LLVector3 delta_pos, delta_scale;

        //matrix from local space to parent relative/global space
        bool use_identity = force_identity || drawable->isSpatialRoot();
        delta_rot = use_identity ? LLQuaternion() : mDrawable->getRotation();
        delta_pos = use_identity ? LLVector3(0,0,0) : mDrawable->getPosition();
        delta_scale = mDrawable->getScale();

        // Vertex transform (4x4)
        LLVector3 x_axis = LLVector3(delta_scale.mV[VX], 0.f, 0.f) * delta_rot;
        LLVector3 y_axis = LLVector3(0.f, delta_scale.mV[VY], 0.f) * delta_rot;
        LLVector3 z_axis = LLVector3(0.f, 0.f, delta_scale.mV[VZ]) * delta_rot;

        mRelativeXform.initRows(LLVector4(x_axis, 0.f),
                                LLVector4(y_axis, 0.f),
                                LLVector4(z_axis, 0.f),
                                LLVector4(delta_pos, 1.f));


        // compute inverse transpose for normals
        // mRelativeXformInvTrans.setRows(x_axis, y_axis, z_axis);
        // mRelativeXformInvTrans.invert();
        // mRelativeXformInvTrans.setRows(x_axis, y_axis, z_axis);
        // grumble - invert is NOT a matrix invert, so we do it by hand:

        LLMatrix3 rot_inverse = LLMatrix3(~delta_rot);

        LLMatrix3 scale_inverse;
        scale_inverse.setRows(LLVector3(1.0, 0.0, 0.0) / delta_scale.mV[VX],
                              LLVector3(0.0, 1.0, 0.0) / delta_scale.mV[VY],
                              LLVector3(0.0, 0.0, 1.0) / delta_scale.mV[VZ]);


        mRelativeXformInvTrans = rot_inverse * scale_inverse;

        mRelativeXformInvTrans.transpose();
    }
    else
    {
        LLVector3 pos = getPosition();
        LLVector3 scale = getScale();
        LLQuaternion rot = getRotation();

        if (mParent)
        {
            pos *= mParent->getRotation();
            pos += mParent->getPosition();
            rot *= mParent->getRotation();
        }

        //LLViewerRegion* region = getRegion();
        //pos += region->getOriginAgent();

        LLVector3 x_axis = LLVector3(scale.mV[VX], 0.f, 0.f) * rot;
        LLVector3 y_axis = LLVector3(0.f, scale.mV[VY], 0.f) * rot;
        LLVector3 z_axis = LLVector3(0.f, 0.f, scale.mV[VZ]) * rot;

        mRelativeXform.initRows(LLVector4(x_axis, 0.f),
                                LLVector4(y_axis, 0.f),
                                LLVector4(z_axis, 0.f),
                                LLVector4(pos, 1.f));

        // compute inverse transpose for normals
        LLMatrix3 rot_inverse = LLMatrix3(~rot);

        LLMatrix3 scale_inverse;
        scale_inverse.setRows(LLVector3(1.0, 0.0, 0.0) / scale.mV[VX],
                              LLVector3(0.0, 1.0, 0.0) / scale.mV[VY],
                              LLVector3(0.0, 0.0, 1.0) / scale.mV[VZ]);


        mRelativeXformInvTrans = rot_inverse * scale_inverse;

        mRelativeXformInvTrans.transpose();
    }
}

void LLVOVolume::updateFaceSize(S32 idx)
{
    if( mDrawable->getNumFaces() <= idx )
    {
        return;
    }

    LLFace* facep = mDrawable->getFace(idx);
    if (facep)
    {
        if (idx >= getVolume()->getNumVolumeFaces())
        {
            facep->setSize(0,0, true);
        }
        else
        {
            const LLVolumeFace& vol_face = getVolume()->getVolumeFace(idx);
            facep->setSize(vol_face.mNumVertices, vol_face.mNumIndices,
                            true); // <--- volume faces should be padded for 16-byte alignment

        }
    }
}

bool LLVOVolume::isRootEdit() const
{
    if (mParent && !((LLViewerObject*)mParent)->isAvatar())
    {
        return false;
    }
    return true;
}

// TE COUNT FOLLOWS THE MESH LOD WITHOUT LOSING TEXTURE ENTRIES. Every volume swap
// (LLPrimitive::setVolume: a LOD change, or the mesh data replacing the placeholder) and the
// ingest's face-list resync set the TE count to the volume's face count. A mesh whose coarser
// LODs have fewer faces than its finest (creators drop materials from low LODs; the VK
// resync log shows objects going "tes 3 <-> 4") passes through such a LOD -- the arrival
// LOD-0 load, loadMesh's lower-LOD fallback while the cluster-forced LOD 3 streams, distance
// LOD -- so the count SHRINKS (LLPrimTextureList deletes the server's entries of the dropped
// faces) and GROWS again at the finer LOD, where LL fills every regained face with a copy of
// the LAST surviving entry (texture, colour/alpha, material). Those faces then draw with
// another face's texture/alpha -- a hair mane "mostly missing" -- for the object's whole
// life, the same again after a region return (re-created through the same LOD sequence),
// until a full ObjectUpdate re-sends the TE block (the owner selecting it to edit). The same
// loss hits a TE block unpacked while the count was short (parseTEMessage keeps
// min(TE count, sent) entries).
// So, for meshes: a shrink stashes the dropped entries (+ render-material ids), a grow
// restores them, and regained faces with no stashed entry ask the simulator for a full update
// of the object (RequestMultipleObjects -- what the owner's edit produced), at most once per
// object per ~30 s. A genuine shape change still gets the server's TE block right after
// setVolume (processUpdateMessage), which overwrites whatever was restored.
static U64 s_vk_te_stashed = 0, s_vk_te_restored = 0, s_vk_te_refetches = 0;
void llvkTEStashStats(std::ostream& os)
{
    os << "mesh TE entries stashed " << s_vk_te_stashed << " restored " << s_vk_te_restored
       << " full-update asks " << s_vk_te_refetches;
}

//virtual
void LLVOVolume::setNumTEs(const U8 num_tes)
{
    const U8 old_num_tes = getNumTEs() ;
    const bool keep_tes = old_num_tes != num_tes && isMesh();

    if (keep_tes && num_tes < old_num_tes)
    {
        for (U8 i = num_tes; i < old_num_tes; ++i)
        {
            if (const LLTextureEntry* te = getTE(i))
            {
                mVKStashedTEs[i] = std::make_pair(*te, getRenderMaterialID(i));
                ++s_vk_te_stashed;
            }
        }
    }

    setNumTEsLL(num_tes);

    if (keep_tes && old_num_tes && num_tes > old_num_tes && getNumTEs() == num_tes)
    {
        bool unknown = false;
        for (U8 i = old_num_tes; i < num_tes; ++i)
        {
            auto it = mVKStashedTEs.find(i);
            if (it == mVKStashedTEs.end())
            {
                unknown = true;
                continue;
            }
            // Material id first (as LLViewerObject::setNumTEs does for the copies), then the
            // whole entry (texture, colour, material params, glTF pointers + texture fetch).
            setRenderMaterialID(i, it->second.second, false);
            setTE(i, it->second.first);
            mVKStashedTEs.erase(it);
            ++s_vk_te_restored;
        }
        LLViewerRegion* region = getRegion();
        if (unknown && region && getLocalID()
            && (mVKTERefetchFrame == 0 || (U32)gFrameCount - mVKTERefetchFrame >= 1800u))
        {
            mVKTERefetchFrame = llmax((U32)gFrameCount, 1u);
            region->addCacheMissFull(getLocalID());
            ++s_vk_te_refetches;
        }
    }
}

void LLVOVolume::setNumTEsLL(const U8 num_tes)
{
    const U8 old_num_tes = getNumTEs() ;

    if(old_num_tes && old_num_tes < num_tes) //new faces added
    {
        LLViewerObject::setNumTEs(num_tes) ;

        if(mMediaImplList.size() >= old_num_tes && mMediaImplList[old_num_tes -1].notNull())//duplicate the last media textures if exists.
        {
            mMediaImplList.resize(num_tes) ;
            const LLTextureEntry* te = getTE(old_num_tes - 1) ;
            for(U8 i = old_num_tes; i < num_tes ; i++)
            {
                setTE(i, *te) ;
                mMediaImplList[i] = mMediaImplList[old_num_tes -1] ;
            }
            mMediaImplList[old_num_tes -1]->setUpdated(true) ;
        }
    }
    else if(old_num_tes > num_tes && mMediaImplList.size() > num_tes) //old faces removed
    {
        U8 end = (U8)(mMediaImplList.size()) ;
        for(U8 i = num_tes; i < end ; i++)
        {
            removeMediaImpl(i) ;
        }
        mMediaImplList.resize(num_tes) ;

        LLViewerObject::setNumTEs(num_tes) ;
    }
    else
    {
        LLViewerObject::setNumTEs(num_tes) ;
    }

    return ;
}


//virtual
void LLVOVolume::changeTEImage(S32 index, LLViewerTexture* imagep)
{
    bool changed = (mTEImages[index] != imagep);
    LLViewerObject::changeTEImage(index, imagep);
    if (changed)
    {
        gPipeline.markTextured(mDrawable);
        mFaceMappingChanged = true;
    }
}

void LLVOVolume::setTEImage(const U8 te, LLViewerTexture *imagep)
{
    bool changed = (mTEImages[te] != imagep);
    LLViewerObject::setTEImage(te, imagep);
    if (changed)
    {
        gPipeline.markTextured(mDrawable);
        mFaceMappingChanged = true;
    }
}

S32 LLVOVolume::setTETexture(const U8 te, const LLUUID &uuid)
{
    S32 res = LLViewerObject::setTETexture(te, uuid);
    if (res)
    {
        if (mDrawable)
        {
            // dynamic texture changes break batches, isolate in octree
            shrinkWrap();
            gPipeline.markTextured(mDrawable);
        }
        mFaceMappingChanged = true;
    }
    return res;
}

S32 LLVOVolume::setTEColor(const U8 te, const LLColor3& color)
{
    return setTEColor(te, LLColor4(color));
}

S32 LLVOVolume::setTEColor(const U8 te, const LLColor4& color)
{
    S32 retval = 0;
    const LLTextureEntry *tep = getTE(te);
    if (!tep)
    {
        LL_WARNS("MaterialTEs") << "No texture entry for te " << (S32)te << ", object " << mID << LL_ENDL;
    }
    else if (color != tep->getColor())
    {
        F32 old_alpha = tep->getColor().mV[3];
        if (color.mV[3] != old_alpha)
        {
            gPipeline.markTextured(mDrawable);
            //treat this alpha change as an LoD update since render batches may need to get rebuilt
            mLODChanged = true;
            gPipeline.markRebuild(mDrawable, LLDrawable::REBUILD_VOLUME);
        }
        retval = LLPrimitive::setTEColor(te, color);
        if (mDrawable.notNull() && retval)
        {
            // These should only happen on updates which are not the initial update.
            mColorChanged = true;
            mDrawable->setState(LLDrawable::REBUILD_COLOR);
            shrinkWrap();
            dirtyMesh();
        }
    }

    return  retval;
}

S32 LLVOVolume::setTEBumpmap(const U8 te, const U8 bumpmap)
{
    S32 res = LLViewerObject::setTEBumpmap(te, bumpmap);
    if (res)
    {
        gPipeline.markTextured(mDrawable);
        mFaceMappingChanged = true;
    }
    return  res;
}

S32 LLVOVolume::setTETexGen(const U8 te, const U8 texgen)
{
    S32 res = LLViewerObject::setTETexGen(te, texgen);
    if (res)
    {
        gPipeline.markTextured(mDrawable);
        mFaceMappingChanged = true;
    }
    return  res;
}

S32 LLVOVolume::setTEMediaTexGen(const U8 te, const U8 media)
{
    S32 res = LLViewerObject::setTEMediaTexGen(te, media);
    if (res)
    {
        gPipeline.markTextured(mDrawable);
        mFaceMappingChanged = true;
    }
    return  res;
}

S32 LLVOVolume::setTEShiny(const U8 te, const U8 shiny)
{
    S32 res = LLViewerObject::setTEShiny(te, shiny);
    if (res)
    {
        gPipeline.markTextured(mDrawable);
        mFaceMappingChanged = true;
    }
    return  res;
}

S32 LLVOVolume::setTEFullbright(const U8 te, const U8 fullbright)
{
    S32 res = LLViewerObject::setTEFullbright(te, fullbright);
    if (res)
    {
        gPipeline.markTextured(mDrawable);
        mFaceMappingChanged = true;
    }
    return  res;
}

S32 LLVOVolume::setTEBumpShinyFullbright(const U8 te, const U8 bump)
{
    S32 res = LLViewerObject::setTEBumpShinyFullbright(te, bump);
    if (res)
    {
        gPipeline.markTextured(mDrawable);
        mFaceMappingChanged = true;
    }
    return res;
}

S32 LLVOVolume::setTEMediaFlags(const U8 te, const U8 media_flags)
{
    S32 res = LLViewerObject::setTEMediaFlags(te, media_flags);
    if (res)
    {
        gPipeline.markTextured(mDrawable);
        mFaceMappingChanged = true;
    }
    return  res;
}

S32 LLVOVolume::setTEGlow(const U8 te, const F32 glow)
{
    S32 res = LLViewerObject::setTEGlow(te, glow);
    if (res)
    {
        if (mDrawable)
        {
            gPipeline.markTextured(mDrawable);
            shrinkWrap();
        }
        mFaceMappingChanged = true;
    }
    return  res;
}

void LLVOVolume::setTEMaterialParamsCallbackTE(const LLUUID& objectID, const LLMaterialID &pMaterialID, const LLMaterialPtr pMaterialParams, U32 te)
{
    LLVOVolume* pVol = (LLVOVolume*)gObjectList.findObject(objectID);
    if (pVol)
    {
        LL_DEBUGS("MaterialTEs") << "materialid " << pMaterialID.asString() << " to TE " << te << LL_ENDL;
        if (te >= pVol->getNumTEs())
            return;

        LLTextureEntry* texture_entry = pVol->getTE(te);
        if (texture_entry && (texture_entry->getMaterialID() == pMaterialID))
        {
            pVol->setTEMaterialParams(te, pMaterialParams);
        }
    }
}

S32 LLVOVolume::setTEMaterialID(const U8 te, const LLMaterialID& pMaterialID)
{
    S32 res = LLViewerObject::setTEMaterialID(te, pMaterialID);
    LL_DEBUGS("MaterialTEs") << "te "<< (S32)te << " materialid " << pMaterialID.asString() << " res " << res
                                << ( LLSelectMgr::getInstance()->getSelection()->contains(const_cast<LLVOVolume*>(this), te) ? " selected" : " not selected" )
                                << LL_ENDL;

    LL_DEBUGS("MaterialTEs") << " " << pMaterialID.asString() << LL_ENDL;
    if (res)
    {
        LLMaterialMgr::instance().getTE(getRegion()->getRegionID(), pMaterialID, te, boost::bind(&LLVOVolume::setTEMaterialParamsCallbackTE, getID(), _1, _2, _3));

        setChanged(ALL_CHANGED);
        if (!mDrawable.isNull())
        {
            gPipeline.markTextured(mDrawable);
            gPipeline.markRebuild(mDrawable,LLDrawable::REBUILD_ALL);
        }
        mFaceMappingChanged = true;
    }
    return res;
}

S32 LLVOVolume::setTEMaterialParams(const U8 te, const LLMaterialPtr pMaterialParams)
{
    S32 res = LLViewerObject::setTEMaterialParams(te, pMaterialParams);

    LL_DEBUGS("MaterialTEs") << "te " << (S32)te << " material " << ((pMaterialParams) ? pMaterialParams->asLLSD() : LLSD("null")) << " res " << res
                             << ( LLSelectMgr::getInstance()->getSelection()->contains(const_cast<LLVOVolume*>(this), te) ? " selected" : " not selected" )
                             << LL_ENDL;
    setChanged(ALL_CHANGED);
    if (!mDrawable.isNull())
    {
        gPipeline.markTextured(mDrawable);
        gPipeline.markRebuild(mDrawable,LLDrawable::REBUILD_ALL);
    }
    mFaceMappingChanged = true;
    return TEM_CHANGE_TEXTURE;
}

S32 LLVOVolume::setTEGLTFMaterialOverride(U8 te, LLGLTFMaterial* mat)
{
    S32 retval = LLViewerObject::setTEGLTFMaterialOverride(te, mat);

    if (retval == TEM_CHANGE_TEXTURE)
    {
        if (!mDrawable.isNull())
        {
            gPipeline.markTextured(mDrawable);
            gPipeline.markRebuild(mDrawable, LLDrawable::REBUILD_ALL);
        }
        mFaceMappingChanged = true;
    }

    return retval;
}


S32 LLVOVolume::setTEScale(const U8 te, const F32 s, const F32 t)
{
    S32 res = LLViewerObject::setTEScale(te, s, t);
    if (res)
    {
        gPipeline.markTextured(mDrawable);
        mFaceMappingChanged = true;
    }
    return res;
}

S32 LLVOVolume::setTEScaleS(const U8 te, const F32 s)
{
    S32 res = LLViewerObject::setTEScaleS(te, s);
    if (res)
    {
        gPipeline.markTextured(mDrawable);
        mFaceMappingChanged = true;
    }
    return res;
}

S32 LLVOVolume::setTEScaleT(const U8 te, const F32 t)
{
    S32 res = LLViewerObject::setTEScaleT(te, t);
    if (res)
    {
        gPipeline.markTextured(mDrawable);
        mFaceMappingChanged = true;
    }
    return res;
}

bool LLVOVolume::hasMedia() const
{
    bool result = false;
    const U8 numTEs = getNumTEs();
    for (U8 i = 0; i < numTEs; i++)
    {
        const LLTextureEntry* te = getTE(i);
        if(te->hasMedia())
        {
            result = true;
            break;
        }
    }
    return result;
}

LLVector3 LLVOVolume::getApproximateFaceNormal(U8 face_id)
{
    LLVolume* volume = getVolume();
    LLVector4a result;
    result.clear();

    LLVector3 ret;

    if (volume && face_id < volume->getNumVolumeFaces())
    {
        // The face as the renderer draws it first (#704: a GPU query of its drawn slot --
        // #585: LL's arrays may be released), then LL's volume, then -- a released face
        // with nothing resident -- LL's arrays decoded again from the retained raw bytes
        // (rare: a media-focus zoom on an undrawn face).
        if (!LLCleanCursorPick::drawnFaceFrame(this, face_id, &result, nullptr, nullptr))
        {
            if (volume->getVolumeFace(face_id).isGeometryReleased() && !gMeshRepo.rehydrateReleasedVolume(volume))
            {
                LLMeshRepository::noteReleasedRead(LLMeshRepository::RRS_FACE_NORMAL);
            }
            const LLVolumeFace& face = volume->getVolumeFace(face_id);
            for (S32 i = 0; face.mNormals && i < (S32)face.mNumVertices; ++i)
            {
                result.add(face.mNormals[i]);
            }
        }

        LLVector3 ret(result.getF32ptr());
        ret = volumeDirectionToAgent(ret);
        ret.normVec();
    }

    return ret;
}

void LLVOVolume::requestMediaDataUpdate(bool isNew)
{
    if (sObjectMediaClient && !isLocalOnly()) // client-only object: no sim media data
        sObjectMediaClient->fetchMedia(new LLMediaDataClientObjectImpl(this, isNew));
}

bool LLVOVolume::isMediaDataBeingFetched() const
{
    // I know what I'm doing by const_casting this away: this is just
    // a wrapper class that is only going to do a lookup.
    return (sObjectMediaClient) ? sObjectMediaClient->isInQueue(new LLMediaDataClientObjectImpl(const_cast<LLVOVolume*>(this), false)) : false;
}

void LLVOVolume::cleanUpMediaImpls()
{
    // Iterate through our TEs and remove any Impls that are no longer used
    const U8 numTEs = getNumTEs();
    for (U8 i = 0; i < numTEs; i++)
    {
        const LLTextureEntry* te = getTE(i);
        if( ! te->hasMedia())
        {
            // Delete the media IMPL!
            removeMediaImpl(i) ;
        }
    }
}

void LLVOVolume::updateObjectMediaData(const LLSD &media_data_array, const std::string &media_version)
{
    // media_data_array is an array of media entry maps
    // media_version is the version string in the response.
    U32 fetched_version = LLTextureEntry::getVersionFromMediaVersionString(media_version);

    // Only update it if it is newer!
    if ( (S32)fetched_version > mLastFetchedMediaVersion)
    {
        mLastFetchedMediaVersion = fetched_version;
        //LL_INFOS() << "updating:" << this->getID() << " " << ll_pretty_print_sd(media_data_array) << LL_ENDL;

        LLSD::array_const_iterator iter = media_data_array.beginArray();
        LLSD::array_const_iterator end = media_data_array.endArray();
        U8 texture_index = 0;
        for (; iter != end; ++iter, ++texture_index)
        {
            syncMediaData(texture_index, *iter, false/*merge*/, false/*ignore_agent*/);
        }
    }
}

void LLVOVolume::syncMediaData(S32 texture_index, const LLSD &media_data, bool merge, bool ignore_agent)
{
    if(mDead)
    {
        // If the object has been marked dead, don't process media updates.
        return;
    }

    LLTextureEntry *te = getTE(texture_index);
    if(!te)
    {
        return ;
    }

    LL_DEBUGS("MediaOnAPrim") << "BEFORE: texture_index = " << texture_index
        << " hasMedia = " << te->hasMedia() << " : "
        << ((NULL == te->getMediaData()) ? "NULL MEDIA DATA" : ll_pretty_print_sd(te->getMediaData()->asLLSD())) << LL_ENDL;

    std::string previous_url;
    LLMediaEntry* mep = te->getMediaData();
    if(mep)
    {
        // Save the "current url" from before the update so we can tell if
        // it changes.
        previous_url = mep->getCurrentURL();
    }

    if (merge)
    {
        te->mergeIntoMediaData(media_data);
    }
    else {
        // XXX Question: what if the media data is undefined LLSD, but the
        // update we got above said that we have media flags??  Here we clobber
        // that, assuming the data from the service is more up-to-date.
        te->updateMediaData(media_data);
    }

    mep = te->getMediaData();
    if(mep)
    {
        bool update_from_self = false;
        if (!ignore_agent)
        {
            LLUUID updating_agent = LLTextureEntry::getAgentIDFromMediaVersionString(getMediaURL());
            update_from_self = (updating_agent == gAgent.getID());
        }
        viewer_media_t media_impl = LLViewerMedia::getInstance()->updateMediaImpl(mep, previous_url, update_from_self, isHUDAttachment());

        addMediaImpl(media_impl, texture_index) ;
    }
    else
    {
        removeMediaImpl(texture_index);
    }

    LL_DEBUGS("MediaOnAPrim") << "AFTER: texture_index = " << texture_index
        << " hasMedia = " << te->hasMedia() << " : "
        << ((NULL == te->getMediaData()) ? "NULL MEDIA DATA" : ll_pretty_print_sd(te->getMediaData()->asLLSD())) << LL_ENDL;
}

void LLVOVolume::mediaNavigateBounceBack(U8 texture_index)
{
    // Find the media entry for this navigate
    const LLMediaEntry* mep = NULL;
    viewer_media_t impl = getMediaImpl(texture_index);
    LLTextureEntry *te = getTE(texture_index);
    if(te)
    {
        mep = te->getMediaData();
    }

    if (mep && impl)
    {
        std::string url = mep->getCurrentURL();
        // Look for a ":", if not there, assume "http://"
        if (!url.empty() && std::string::npos == url.find(':'))
        {
            url = "http://" + url;
        }
        // If the url we're trying to "bounce back" to is either empty or not
        // allowed by the whitelist, try the home url.  If *that* doesn't work,
        // set the media as failed and unload it
        if (url.empty() || !mep->checkCandidateUrl(url))
        {
            url = mep->getHomeURL();
            // Look for a ":", if not there, assume "http://"
            if (!url.empty() && std::string::npos == url.find(':'))
            {
                url = "http://" + url;
            }
        }
        if (url.empty() || !mep->checkCandidateUrl(url))
        {
            // The url to navigate back to is not good, and we have nowhere else
            // to go.
            LL_WARNS("MediaOnAPrim") << "FAILED to bounce back URL \"" << url << "\" -- unloading impl" << LL_ENDL;
            impl->setMediaFailed(true);
        }
        // Make sure we are not bouncing to url we came from
        else if (impl->getCurrentMediaURL() != url)
        {
            // Okay, navigate now
            LL_INFOS("MediaOnAPrim") << "bouncing back to URL: " << url << LL_ENDL;
            impl->navigateTo(url, "", false, true);
        }
    }
}

bool LLVOVolume::hasMediaPermission(const LLMediaEntry* media_entry, MediaPermType perm_type)
{
    // NOTE: This logic ALMOST duplicates the logic in the server (in particular, in llmediaservice.cpp).
    if (NULL == media_entry ) return false; // XXX should we assert here?

    // The agent has permissions if:
    // - world permissions are on, or
    // - group permissions are on, and agent_id is in the group, or
    // - agent permissions are on, and agent_id is the owner

    // *NOTE: We *used* to check for modify permissions here (i.e. permissions were
    // granted if permModify() was true).  However, this doesn't make sense in the
    // viewer: we don't want to show controls or allow interaction if the author
    // has deemed it so.  See DEV-42115.

    U8 media_perms = (perm_type == MEDIA_PERM_INTERACT) ? media_entry->getPermsInteract() : media_entry->getPermsControl();

    // World permissions
    if (0 != (media_perms & LLMediaEntry::PERM_ANYONE))
    {
        return true;
    }

    // Group permissions
    else if (0 != (media_perms & LLMediaEntry::PERM_GROUP))
    {
        LLPermissions* obj_perm = LLSelectMgr::getInstance()->findObjectPermissions(this);
        if (obj_perm && gAgent.isInGroup(obj_perm->getGroup()))
        {
            return true;
        }
    }

    // Owner permissions
    else if (0 != (media_perms & LLMediaEntry::PERM_OWNER) && permYouOwner())
    {
        return true;
    }

    return false;

}

void LLVOVolume::mediaNavigated(LLViewerMediaImpl *impl, LLPluginClassMedia* plugin, std::string new_location)
{
    bool block_navigation = false;
    // FIXME: if/when we allow the same media impl to be used by multiple faces, the logic here will need to be fixed
    // to deal with multiple face indices.
    int face_index = getFaceIndexWithMediaImpl(impl, -1);

    // Find the media entry for this navigate
    LLMediaEntry* mep = NULL;
    LLTextureEntry *te = getTE(face_index);
    if(te)
    {
        mep = te->getMediaData();
    }

    if(mep)
    {
        if(!mep->checkCandidateUrl(new_location))
        {
            block_navigation = true;
        }
        if (!block_navigation && !hasMediaPermission(mep, MEDIA_PERM_INTERACT))
        {
            block_navigation = true;
        }
    }
    else
    {
        LL_WARNS("MediaOnAPrim") << "Couldn't find media entry!" << LL_ENDL;
    }

    if(block_navigation)
    {
        LL_INFOS("MediaOnAPrim") << "blocking navigate to URI " << new_location << LL_ENDL;

        // "bounce back" to the current URL from the media entry
        mediaNavigateBounceBack(face_index);
    }
    else if (sObjectMediaNavigateClient && !isLocalOnly()) // client-only object: no sim to notify
    {

        LL_DEBUGS("MediaOnAPrim") << "broadcasting navigate with URI " << new_location << LL_ENDL;

        sObjectMediaNavigateClient->navigate(new LLMediaDataClientObjectImpl(this, false), face_index, new_location);
    }
}

void LLVOVolume::mediaEvent(LLViewerMediaImpl *impl, LLPluginClassMedia* plugin, LLViewerMediaObserver::EMediaEvent event)
{
    switch(event)
    {

        case LLViewerMediaObserver::MEDIA_EVENT_LOCATION_CHANGED:
        {
            switch(impl->getNavState())
            {
                case LLViewerMediaImpl::MEDIANAVSTATE_FIRST_LOCATION_CHANGED:
                {
                    // This is the first location changed event after the start of a non-server-directed nav.  It may need to be broadcast or bounced back.
                    mediaNavigated(impl, plugin, plugin->getLocation());
                }
                break;

                case LLViewerMediaImpl::MEDIANAVSTATE_FIRST_LOCATION_CHANGED_SPURIOUS:
                    // This navigate didn't change the current URL.
                    LL_DEBUGS("MediaOnAPrim") << "  NOT broadcasting navigate (spurious)" << LL_ENDL;
                break;

                case LLViewerMediaImpl::MEDIANAVSTATE_SERVER_FIRST_LOCATION_CHANGED:
                    // This is the first location changed event after the start of a server-directed nav.  Don't broadcast it.
                    LL_INFOS("MediaOnAPrim") << "   NOT broadcasting navigate (server-directed)" << LL_ENDL;
                break;

                default:
                    // This is a subsequent location-changed due to a redirect.  Don't broadcast.
                    LL_INFOS("MediaOnAPrim") << "   NOT broadcasting navigate (redirect)" << LL_ENDL;
                break;
            }
        }
        break;

        case LLViewerMediaObserver::MEDIA_EVENT_NAVIGATE_COMPLETE:
        {
            switch(impl->getNavState())
            {
                case LLViewerMediaImpl::MEDIANAVSTATE_COMPLETE_BEFORE_LOCATION_CHANGED:
                {
                    // This is the first location changed event after the start of a non-server-directed nav.  It may need to be broadcast or bounced back.
                    mediaNavigated(impl, plugin, plugin->getNavigateURI());
                }
                break;

                case LLViewerMediaImpl::MEDIANAVSTATE_COMPLETE_BEFORE_LOCATION_CHANGED_SPURIOUS:
                    // This navigate didn't change the current URL.
                    LL_DEBUGS("MediaOnAPrim") << "  NOT broadcasting navigate (spurious)" << LL_ENDL;
                break;

                case LLViewerMediaImpl::MEDIANAVSTATE_SERVER_COMPLETE_BEFORE_LOCATION_CHANGED:
                    // This is the the navigate complete event from a server-directed nav.  Don't broadcast it.
                    LL_INFOS("MediaOnAPrim") << "   NOT broadcasting navigate (server-directed)" << LL_ENDL;
                break;

                default:
                    // For all other states, the navigate should have been handled by LOCATION_CHANGED events already.
                break;
            }
        }
        break;

        case LLViewerMediaObserver::MEDIA_EVENT_FILE_DOWNLOAD:
        {
            // Media might be blocked, waiting for a file,
            // send an empty response to unblock it
            const std::vector<std::string> empty_response;
            plugin->sendPickFileResponse(empty_response);

            LLNotificationsUtil::add("MediaFileDownloadUnsupported");
        }
        break;

        default:
        break;
    }

}

void LLVOVolume::sendMediaDataUpdate()
{
    if (sObjectMediaClient && !isLocalOnly()) // client-only object: no sim media data
        sObjectMediaClient->updateMedia(new LLMediaDataClientObjectImpl(this, false));
}

void LLVOVolume::removeMediaImpl(S32 texture_index)
{
    if(mMediaImplList.size() <= (U32)texture_index || mMediaImplList[texture_index].isNull())
    {
        return ;
    }

    //make the face referencing to mMediaImplList[texture_index] to point back to the old texture.
    if(mDrawable && texture_index < mDrawable->getNumFaces())
    {
        LLFace* facep = mDrawable->getFace(texture_index) ;
        if(facep)
        {
            LLViewerMediaTexture* media_tex = LLViewerTextureManager::findMediaTexture(mMediaImplList[texture_index]->getMediaTextureID()) ;
            if(media_tex)
            {
                media_tex->removeMediaFromFace(facep) ;
            }
        }
    }

    //check if some other face(s) of this object reference(s)to this media impl.
    S32 i ;
    S32 end = (S32)mMediaImplList.size() ;
    for(i = 0; i < end ; i++)
    {
        if( i != texture_index && mMediaImplList[i] == mMediaImplList[texture_index])
        {
            break ;
        }
    }

    if(i == end) //this object does not need this media impl.
    {
        mMediaImplList[texture_index]->removeObject(this) ;
    }

    mMediaImplList[texture_index] = NULL ;
    return ;
}

void LLVOVolume::addMediaImpl(LLViewerMediaImpl* media_impl, S32 texture_index)
{
    if((S32)mMediaImplList.size() < texture_index + 1)
    {
        mMediaImplList.resize(texture_index + 1) ;
    }

    if(mMediaImplList[texture_index].notNull())
    {
        if(mMediaImplList[texture_index] == media_impl)
        {
            return ;
        }

        removeMediaImpl(texture_index) ;
    }

    mMediaImplList[texture_index] = media_impl;
    media_impl->addObject(this) ;

    //add the face to show the media if it is in playing
    if(mDrawable)
    {
        LLFace* facep(NULL);
        if( texture_index < mDrawable->getNumFaces() )
        {
            facep = mDrawable->getFace(texture_index) ;
        }

        if(facep)
        {
            LLViewerMediaTexture* media_tex = LLViewerTextureManager::findMediaTexture(mMediaImplList[texture_index]->getMediaTextureID()) ;
            if(media_tex)
            {
                media_tex->addMediaToFace(facep) ;
            }
        }
        else //the face is not available now, start media on this face later.
        {
            media_impl->setUpdated(true) ;
        }
    }
    return ;
}

viewer_media_t LLVOVolume::getMediaImpl(U8 face_id) const
{
    if(mMediaImplList.size() > face_id)
    {
        return mMediaImplList[face_id];
    }
    return NULL;
}

F64 LLVOVolume::getTotalMediaInterest() const
{
    // If this object is currently focused, this object has "high" interest
    if (LLViewerMediaFocus::getInstance()->getFocusedObjectID() == getID())
        return F64_MAX;

    F64 interest = (F64)-1.0;  // means not interested;

    // If this object is selected, this object has "high" interest, but since
    // there can be more than one, we still add in calculated impl interest
    // XXX Sadly, 'contains()' doesn't take a const :(
    if (LLSelectMgr::getInstance()->getSelection()->contains(const_cast<LLVOVolume*>(this)))
        interest = F64_MAX / 2.0;

    int i = 0;
    const int end = getNumTEs();
    for ( ; i < end; ++i)
    {
        const viewer_media_t &impl = getMediaImpl(i);
        if (!impl.isNull())
        {
            if (interest == (F64)-1.0) interest = (F64)0.0;
            interest += impl->getInterest();
        }
    }
    return interest;
}

S32 LLVOVolume::getFaceIndexWithMediaImpl(const LLViewerMediaImpl* media_impl, S32 start_face_id)
{
    S32 end = (S32)mMediaImplList.size() ;
    for(S32 face_id = start_face_id + 1; face_id < end; face_id++)
    {
        if(mMediaImplList[face_id] == media_impl)
        {
            return face_id ;
        }
    }
    return -1 ;
}

//----------------------------------------------------------------------------

void LLVOVolume::setLightTextureID(LLUUID id)
{
    LLViewerTexture* old_texturep = getLightTexture(); // same as mLightTexture, but inits if nessesary
    if (id.notNull())
    {
        if (!hasLightTexture())
        {
            setParameterEntryInUse(LLNetworkData::PARAMS_LIGHT_IMAGE, true, true);
        }
        else if (old_texturep)
        {
            old_texturep->removeVolume(LLRender::LIGHT_TEX, this);
        }
        LLLightImageParams* param_block = (LLLightImageParams*) getParameterEntry(LLNetworkData::PARAMS_LIGHT_IMAGE);
        if (param_block && param_block->getLightTexture() != id)
        {
            param_block->setLightTexture(id);
            parameterChanged(LLNetworkData::PARAMS_LIGHT_IMAGE, true);
        }
        LLViewerTexture* tex = getLightTexture();
        if (tex)
        {
            tex->addVolume(LLRender::LIGHT_TEX, this); // new texture
        }
        else
        {
            LL_WARNS() << "Can't get light texture for ID " << id.asString() << LL_ENDL;
        }
    }
    else if (hasLightTexture())
    {
        if (old_texturep)
        {
            old_texturep->removeVolume(LLRender::LIGHT_TEX, this);
        }
        setParameterEntryInUse(LLNetworkData::PARAMS_LIGHT_IMAGE, false, true);
        parameterChanged(LLNetworkData::PARAMS_LIGHT_IMAGE, true);
        mLightTexture = NULL;
    }
}

void LLVOVolume::setSpotLightParams(LLVector3 params)
{
    LLLightImageParams* param_block = (LLLightImageParams*) getParameterEntry(LLNetworkData::PARAMS_LIGHT_IMAGE);
    if (param_block && param_block->getParams() != params)
    {
        param_block->setParams(params);
        parameterChanged(LLNetworkData::PARAMS_LIGHT_IMAGE, true);
    }
}

void LLVOVolume::setIsLight(bool is_light)
{
    bool was_light = getIsLight();
    if (is_light != was_light)
    {
        if (is_light)
        {
            setParameterEntryInUse(LLNetworkData::PARAMS_LIGHT, true, true);
        }
        else
        {
            setParameterEntryInUse(LLNetworkData::PARAMS_LIGHT, false, true);
        }

        if (is_light)
        {
            // Add it to the pipeline mLightSet
            gPipeline.setLight(mDrawable, true);
        }
        else
        {
            // Not a light.  Remove it from the pipeline's light set.
            gPipeline.setLight(mDrawable, false);
        }
    }
}

void LLVOVolume::setLightSRGBColor(const LLColor3& color)
{
    setLightLinearColor(linearColor3(color));
}

void LLVOVolume::setLightLinearColor(const LLColor3& color)
{
    LLLightParams *param_block = (LLLightParams *)getParameterEntry(LLNetworkData::PARAMS_LIGHT);
    if (param_block)
    {
        if (param_block->getLinearColor() != color)
        {
            param_block->setLinearColor(LLColor4(color, param_block->getLinearColor().mV[3]));
            parameterChanged(LLNetworkData::PARAMS_LIGHT, true);
            gPipeline.markTextured(mDrawable);
            mFaceMappingChanged = true;
        }
    }
}

void LLVOVolume::setLightIntensity(F32 intensity)
{
    LLLightParams *param_block = (LLLightParams *)getParameterEntry(LLNetworkData::PARAMS_LIGHT);
    if (param_block)
    {
        if (param_block->getLinearColor().mV[3] != intensity)
        {
            param_block->setLinearColor(LLColor4(LLColor3(param_block->getLinearColor()), intensity));
            parameterChanged(LLNetworkData::PARAMS_LIGHT, true);
        }
    }
}

void LLVOVolume::setLightRadius(F32 radius)
{
    LLLightParams *param_block = (LLLightParams *)getParameterEntry(LLNetworkData::PARAMS_LIGHT);
    if (param_block)
    {
        if (param_block->getRadius() != radius)
        {
            param_block->setRadius(radius);
            parameterChanged(LLNetworkData::PARAMS_LIGHT, true);
        }
    }
}

void LLVOVolume::setLightFalloff(F32 falloff)
{
    LLLightParams *param_block = (LLLightParams *)getParameterEntry(LLNetworkData::PARAMS_LIGHT);
    if (param_block)
    {
        if (param_block->getFalloff() != falloff)
        {
            param_block->setFalloff(falloff);
            parameterChanged(LLNetworkData::PARAMS_LIGHT, true);
        }
    }
}

void LLVOVolume::setLightCutoff(F32 cutoff)
{
    LLLightParams *param_block = (LLLightParams *)getParameterEntry(LLNetworkData::PARAMS_LIGHT);
    if (param_block)
    {
        if (param_block->getCutoff() != cutoff)
        {
            param_block->setCutoff(cutoff);
            parameterChanged(LLNetworkData::PARAMS_LIGHT, true);
        }
    }
}

//----------------------------------------------------------------------------

bool LLVOVolume::getIsLight() const
{
    mIsLight = getParameterEntryInUse(LLNetworkData::PARAMS_LIGHT);
    return mIsLight;
}

bool LLVOVolume::getIsLightFast() const
{
    return mIsLight;
}

LLColor3 LLVOVolume::getLightSRGBBaseColor() const
{
    return srgbColor3(getLightLinearBaseColor());
}

LLColor3 LLVOVolume::getLightLinearBaseColor() const
{
    const LLLightParams *param_block = (const LLLightParams *)getParameterEntry(LLNetworkData::PARAMS_LIGHT);
    if (param_block)
    {
        return LLColor3(param_block->getLinearColor());
    }
    else
    {
        return LLColor3(1,1,1);
    }
}

LLColor3 LLVOVolume::getLightLinearColor() const
{
    const LLLightParams *param_block = (const LLLightParams *)getParameterEntry(LLNetworkData::PARAMS_LIGHT);
    if (param_block)
    {
        return LLColor3(param_block->getLinearColor()) * param_block->getLinearColor().mV[3];
    }
    else
    {
        return LLColor3(1, 1, 1);
    }
}

LLColor3 LLVOVolume::getLightSRGBColor() const
{
    LLColor3 ret = getLightLinearColor();
    ret = srgbColor3(ret);
    return ret;
}

LLUUID LLVOVolume::getLightTextureID() const
{
    if (getParameterEntryInUse(LLNetworkData::PARAMS_LIGHT_IMAGE))
    {
        const LLLightImageParams *param_block = (const LLLightImageParams *)getParameterEntry(LLNetworkData::PARAMS_LIGHT_IMAGE);
        if (param_block)
        {
            return param_block->getLightTexture();
        }
    }

    return LLUUID::null;
}


LLVector3 LLVOVolume::getSpotLightParams() const
{
    if (getParameterEntryInUse(LLNetworkData::PARAMS_LIGHT_IMAGE))
    {
        const LLLightImageParams *param_block = (const LLLightImageParams *)getParameterEntry(LLNetworkData::PARAMS_LIGHT_IMAGE);
        if (param_block)
        {
            return param_block->getParams();
        }
    }

    return LLVector3();
}

F32 LLVOVolume::getSpotLightPriority() const
{
    return mSpotLightPriority;
}

void LLVOVolume::updateSpotLightPriority()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_VOLUME;

    F32 r = getLightRadius();
    LLVector3 pos = mDrawable->getPositionAgent();

    LLVector3 at(0,0,-1);
    at *= getRenderRotation();
    pos += at * r;

    at = LLViewerCamera::getInstance()->getAtAxis();
    pos -= at * r;

    mSpotLightPriority = gPipeline.calcPixelArea(pos, LLVector3(r,r,r), *LLViewerCamera::getInstance());

    if (mLightTexture.notNull())
    {
        mLightTexture->addTextureStats(mSpotLightPriority);
    }
}


bool LLVOVolume::isLightSpotlight() const
{
    LLLightImageParams* params = (LLLightImageParams*) getParameterEntry(LLNetworkData::PARAMS_LIGHT_IMAGE);
    if (params && getParameterEntryInUse(LLNetworkData::PARAMS_LIGHT_IMAGE))
    {
        return params->isLightSpotlight();
    }
    return false;
}


LLViewerTexture* LLVOVolume::getLightTexture()
{
    LLUUID id = getLightTextureID();

    if (id.notNull())
    {
        if (mLightTexture.isNull() || id != mLightTexture->getID())
        {
            mLightTexture = LLViewerTextureManager::getFetchedTexture(id, FTT_DEFAULT, true, LLGLTexture::BOOST_NONE);
        }
    }
    else
    {
        mLightTexture = NULL;
    }

    return mLightTexture;
}

F32 LLVOVolume::getLightIntensity() const
{
    const LLLightParams *param_block = (const LLLightParams *)getParameterEntry(LLNetworkData::PARAMS_LIGHT);
    if (param_block)
    {
        return param_block->getLinearColor().mV[3];
    }
    else
    {
        return 1.f;
    }
}

F32 LLVOVolume::getLightRadius() const
{
    const LLLightParams *param_block = (const LLLightParams *)getParameterEntry(LLNetworkData::PARAMS_LIGHT);
    if (param_block)
    {
        return param_block->getRadius();
    }
    else
    {
        return 0.f;
    }
}

F32 LLVOVolume::getLightFalloff(const F32 fudge_factor) const
{
    const LLLightParams *param_block = (const LLLightParams *)getParameterEntry(LLNetworkData::PARAMS_LIGHT);
    if (param_block)
    {
        return param_block->getFalloff() * fudge_factor;
    }
    else
    {
        return 0.f;
    }
}

F32 LLVOVolume::getLightCutoff() const
{
    const LLLightParams *param_block = (const LLLightParams *)getParameterEntry(LLNetworkData::PARAMS_LIGHT);
    if (param_block)
    {
        return param_block->getCutoff();
    }
    else
    {
        return 0.f;
    }
}

bool LLVOVolume::isReflectionProbe() const
{
    return getParameterEntryInUse(LLNetworkData::PARAMS_REFLECTION_PROBE);
}

bool LLVOVolume::setIsReflectionProbe(bool is_probe)
{
    bool was_probe = isReflectionProbe();
    if (is_probe != was_probe)
    {
        if (is_probe)
        {
            setParameterEntryInUse(LLNetworkData::PARAMS_REFLECTION_PROBE, true, true);
        }
        else
        {
            setParameterEntryInUse(LLNetworkData::PARAMS_REFLECTION_PROBE, false, true);
        }
    }

    updateReflectionProbePtr();

    return was_probe != is_probe;
}

bool LLVOVolume::setReflectionProbeAmbiance(F32 ambiance)
{
    LLReflectionProbeParams* param_block = (LLReflectionProbeParams*)getParameterEntry(LLNetworkData::PARAMS_REFLECTION_PROBE);
    if (param_block)
    {
        if (param_block->getAmbiance() != ambiance)
        {
            param_block->setAmbiance(ambiance);
            parameterChanged(LLNetworkData::PARAMS_REFLECTION_PROBE, true);
            return true;
        }
    }

    return false;
}

bool LLVOVolume::setReflectionProbeNearClip(F32 near_clip)
{
    LLReflectionProbeParams* param_block = (LLReflectionProbeParams*)getParameterEntry(LLNetworkData::PARAMS_REFLECTION_PROBE);
    if (param_block)
    {
        if (param_block->getClipDistance() != near_clip)
        {
            param_block->setClipDistance(near_clip);
            parameterChanged(LLNetworkData::PARAMS_REFLECTION_PROBE, true);
            return true;
        }
    }

    return false;
}

bool LLVOVolume::setReflectionProbeIsBox(bool is_box)
{
    LLReflectionProbeParams* param_block = (LLReflectionProbeParams*)getParameterEntry(LLNetworkData::PARAMS_REFLECTION_PROBE);
    if (param_block)
    {
        if (param_block->getIsBox() != is_box)
        {
            param_block->setIsBox(is_box);
            parameterChanged(LLNetworkData::PARAMS_REFLECTION_PROBE, true);
            return true;
        }
    }

    return false;
}

bool LLVOVolume::setReflectionProbeIsDynamic(bool is_dynamic)
{
    LLReflectionProbeParams* param_block = (LLReflectionProbeParams*)getParameterEntry(LLNetworkData::PARAMS_REFLECTION_PROBE);
    if (param_block)
    {
        if (param_block->getIsDynamic() != is_dynamic)
        {
            param_block->setIsDynamic(is_dynamic);
            parameterChanged(LLNetworkData::PARAMS_REFLECTION_PROBE, true);
            return true;
        }
    }

    return false;
}

bool LLVOVolume::setReflectionProbeIsMirror(bool is_mirror)
{
    LLReflectionProbeParams *param_block = (LLReflectionProbeParams *) getParameterEntry(LLNetworkData::PARAMS_REFLECTION_PROBE);
    if (param_block)
    {
        if (param_block->getIsMirror() != is_mirror)
        {
            LL_INFOS() << "Setting reflection probe mirror to " << is_mirror << LL_ENDL;
            param_block->setIsMirror(is_mirror);
            parameterChanged(LLNetworkData::PARAMS_REFLECTION_PROBE, true);

            if (!is_mirror)
                gPipeline.mHeroProbeManager.unregisterViewerObject(this);
            else
                gPipeline.mHeroProbeManager.registerViewerObject(this);

            return true;
        }
    }

    return false;
}

F32 LLVOVolume::getReflectionProbeAmbiance() const
{
    const LLReflectionProbeParams* param_block = (const LLReflectionProbeParams*)getParameterEntry(LLNetworkData::PARAMS_REFLECTION_PROBE);
    if (param_block)
    {
        return param_block->getAmbiance();
    }
    else
    {
        return 0.f;
    }
}

F32 LLVOVolume::getReflectionProbeNearClip() const
{
    const LLReflectionProbeParams* param_block = (const LLReflectionProbeParams*)getParameterEntry(LLNetworkData::PARAMS_REFLECTION_PROBE);
    if (param_block)
    {
        return param_block->getClipDistance();
    }
    else
    {
        return 0.f;
    }
}

bool LLVOVolume::getReflectionProbeIsBox() const
{
    const LLReflectionProbeParams* param_block = (const LLReflectionProbeParams*)getParameterEntry(LLNetworkData::PARAMS_REFLECTION_PROBE);
    if (param_block)
    {
        return param_block->getIsBox();
    }

    return false;
}

bool LLVOVolume::getReflectionProbeIsDynamic() const
{
    const LLReflectionProbeParams* param_block = (const LLReflectionProbeParams*)getParameterEntry(LLNetworkData::PARAMS_REFLECTION_PROBE);
    if (param_block)
    {
        return param_block->getIsDynamic();
    }

    return false;
}

bool LLVOVolume::getReflectionProbeIsMirror() const
{
    const LLReflectionProbeParams *param_block =
        (const LLReflectionProbeParams *) getParameterEntry(LLNetworkData::PARAMS_REFLECTION_PROBE);
    if (param_block)
    {
        return param_block->getIsMirror();
    }

    return false;
}

U32 LLVOVolume::getVolumeInterfaceID() const
{
    if (mVolumeImpl)
    {
        return mVolumeImpl->getID();
    }

    return 0;
}

LLVolumeImplFlexible* LLVOVolume::vkFlexiImpl() const
{
    return (mVolumeImpl && mVolumeImpl->getInterfaceType() == INTERFACE_FLEXIBLE && isFlexible())
        ? (LLVolumeImplFlexible*)mVolumeImpl : nullptr;
}

bool LLVOVolume::isFlexible() const
{
    if (getParameterEntryInUse(LLNetworkData::PARAMS_FLEXIBLE))
    {
        return true;
    }
    else
    {
        return false;
    }
}

bool LLVOVolume::isSculpted() const
{
    if (getParameterEntryInUse(LLNetworkData::PARAMS_SCULPT))
    {
        return true;
    }

    return false;
}

bool LLVOVolume::isMesh() const
{
    if (isSculpted())
    {
        LLSculptParams *sculpt_params = (LLSculptParams *)getParameterEntry(LLNetworkData::PARAMS_SCULPT);
        if (sculpt_params)
        {
            U8 sculpt_type = sculpt_params->getSculptType();

            if ((sculpt_type & LL_SCULPT_TYPE_MASK) == LL_SCULPT_TYPE_MESH)
                // mesh is a mesh
            {
                return true;
            }
        }
    }

    return false;
}

bool LLVOVolume::hasLightTexture() const
{
    if (getParameterEntryInUse(LLNetworkData::PARAMS_LIGHT_IMAGE))
    {
        return true;
    }

    return false;
}

bool LLVOVolume::isFlexibleFast() const
{
    return mVolumep && mVolumep->getParams().getPathParams().getCurveType() == LL_PCODE_PATH_FLEXIBLE;
}

bool LLVOVolume::isSculptedFast() const
{
    return mVolumep && mVolumep->getParams().isSculpt();
}

bool LLVOVolume::isMeshFast() const
{
    return mVolumep && mVolumep->getParams().isMeshSculpt();
}

bool LLVOVolume::isRiggedMeshFast() const
{
    return mSkinInfo.notNull();
}

bool LLVOVolume::isAnimatedObjectFast() const
{
    return mIsAnimatedObject;
}

bool LLVOVolume::isVolumeGlobal() const
{
    if (mVolumeImpl)
    {
        return mVolumeImpl->isVolumeGlobal();
    }

    return false;
}

bool LLVOVolume::canBeFlexible() const
{
    U8 path = getVolume()->getParams().getPathParams().getCurveType();
    return (path == LL_PCODE_PATH_FLEXIBLE || path == LL_PCODE_PATH_LINE);
}

bool LLVOVolume::setIsFlexible(bool is_flexible)
{
    bool res = false;
    bool was_flexible = isFlexible();
    LLVolumeParams volume_params;
    if (is_flexible)
    {
        if (!was_flexible)
        {
            volume_params = getVolume()->getParams();
            U8 profile_and_hole = volume_params.getProfileParams().getCurveType();
            volume_params.setType(profile_and_hole, LL_PCODE_PATH_FLEXIBLE);
            res = true;
            setFlags(FLAGS_USE_PHYSICS, false);
            setFlags(FLAGS_PHANTOM, true);
            setParameterEntryInUse(LLNetworkData::PARAMS_FLEXIBLE, true, true);
            if (mDrawable)
            {
                mDrawable->makeActive();
            }
        }
    }
    else
    {
        if (was_flexible)
        {
            volume_params = getVolume()->getParams();
            U8 profile_and_hole = volume_params.getProfileParams().getCurveType();
            volume_params.setType(profile_and_hole, LL_PCODE_PATH_LINE);
            res = true;
            setFlags(FLAGS_PHANTOM, false);
            setParameterEntryInUse(LLNetworkData::PARAMS_FLEXIBLE, false, true);
        }
    }
    if (res)
    {
        res = setVolume(volume_params, 1);
        if (res)
        {
            markForUpdate();
        }
    }
    return res;
}

const LLMeshSkinInfo* LLVOVolume::getSkinInfo() const
{
    if (getVolume())
    {
         return mSkinInfo;
    }
    else
    {
        return NULL;
    }
}

// virtual
bool LLVOVolume::isRiggedMesh() const
{
    return getSkinInfo() != nullptr;
}

//----------------------------------------------------------------------------
U32 LLVOVolume::getExtendedMeshFlags() const
{
    const LLExtendedMeshParams *param_block =
        (const LLExtendedMeshParams *)getParameterEntry(LLNetworkData::PARAMS_EXTENDED_MESH);
    if (param_block)
    {
        return param_block->getFlags();
    }
    else
    {
        return 0;
    }
}

void LLVOVolume::onSetExtendedMeshFlags(U32 flags)
{

    // The isAnySelected() check was needed at one point to prevent
    // graphics problems. These are now believed to be fixed so the
    // check has been disabled.
    if (/*!getRootEdit()->isAnySelected() &&*/ mDrawable.notNull())
    {
        // Need to trigger rebuildGeom(), which is where control avatars get created/removed
        getRootEdit()->recursiveMarkForUpdate();
    }
    if (isAttachment() && getAvatarAncestor())
    {
        updateVisualComplexity();
        if (flags & LLExtendedMeshParams::ANIMATED_MESH_ENABLED_FLAG)
        {
            // Making a rigged mesh into an animated object
            getAvatarAncestor()->updateAttachmentOverrides();
        }
        else
        {
            // Making an animated object into a rigged mesh
            getAvatarAncestor()->updateAttachmentOverrides();
        }
    }
}

void LLVOVolume::setExtendedMeshFlags(U32 flags)
{
    U32 curr_flags = getExtendedMeshFlags();
    if (curr_flags != flags)
    {
        bool in_use = true;
        setParameterEntryInUse(LLNetworkData::PARAMS_EXTENDED_MESH, in_use, true);
        LLExtendedMeshParams *param_block =
            (LLExtendedMeshParams *)getParameterEntry(LLNetworkData::PARAMS_EXTENDED_MESH);
        if (param_block)
        {
            param_block->setFlags(flags);
        }
        parameterChanged(LLNetworkData::PARAMS_EXTENDED_MESH, true);
        LL_DEBUGS("AnimatedObjects") << this
                                     << " new flags " << flags << " curr_flags " << curr_flags
                                     << ", calling onSetExtendedMeshFlags()"
                                     << LL_ENDL;
        onSetExtendedMeshFlags(flags);
    }
}

bool LLVOVolume::canBeAnimatedObject() const
{
    F32 est_tris = recursiveGetEstTrianglesMax();
    if (est_tris < 0 || est_tris > getAnimatedObjectMaxTris())
    {
        return false;
    }
    return true;
}

bool LLVOVolume::isAnimatedObject() const
{
    LLViewerObject *root_obj = getRootEdit();
    if (root_obj->getPCode() != LL_PCODE_VOLUME)
    {
        return false; // at the moment only volumes can be animated
    }
    LLVOVolume* root_vol = (LLVOVolume*)root_obj;
    mIsAnimatedObject = root_vol->getExtendedMeshFlags() & LLExtendedMeshParams::ANIMATED_MESH_ENABLED_FLAG;
    return mIsAnimatedObject;
}

// Called any time parenting changes for a volume. Update flags and
// control av accordingly.  This is called after parent has been
// changed to new_parent, but before new_parent's mChildList has changed.

// virtual
void LLVOVolume::onReparent(LLViewerObject *old_parent, LLViewerObject *new_parent)
{
    LLVOVolume *old_volp = dynamic_cast<LLVOVolume*>(old_parent);

    if (new_parent && !new_parent->isAvatar())
    {
        if (mControlAvatar.notNull())
        {
            // Here an animated object is being made the child of some
            // other prim. Should remove the control av from the child.
            LLControlAvatar *av = mControlAvatar;
            mControlAvatar = NULL;
            av->markForDeath();
        }
    }
    if (old_volp && old_volp->isAnimatedObject())
    {
        if (old_volp->getControlAvatar())
        {
            // We have been removed from an animated object, need to do cleanup.
            old_volp->getControlAvatar()->updateAttachmentOverrides();
            old_volp->getControlAvatar()->updateAnimations();
        }
    }
}

// This needs to be called after onReparent(), because mChildList is
// not updated until the end of LLViewerObject::addChild()

// virtual
void LLVOVolume::afterReparent()
{
    {
        LL_DEBUGS("AnimatedObjects") << "new child added for parent "
            << ((LLViewerObject*)getParent())->getID() << LL_ENDL;
    }

    if (isAnimatedObject() && getControlAvatar())
    {
        LL_DEBUGS("AnimatedObjects") << "adding attachment overrides, parent is animated object "
            << ((LLViewerObject*)getParent())->getID() << LL_ENDL;

        // MAINT-8239 - doing a full rebuild whenever parent is set
        // makes the joint overrides load more robustly. In theory,
        // addAttachmentOverrides should be sufficient, but in
        // practice doing a full rebuild helps compensate for
        // notifyMeshLoaded() not being called reliably enough.

        // was: getControlAvatar()->addAttachmentOverridesForObject(this);
        //getControlAvatar()->rebuildAttachmentOverrides();
        getControlAvatar()->updateAnimations();
    }
    else
    {
        LL_DEBUGS("AnimatedObjects") << "not adding overrides, parent: "
                                     << ((LLViewerObject*)getParent())->getID()
                                     << " isAnimated: "  << isAnimatedObject() << " cav "
                                     << getControlAvatar() << LL_ENDL;
    }
}

//----------------------------------------------------------------------------
void LLVOVolume::updateRiggingInfo()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_VOLUME;
    if (isRiggedMesh())
    {
        const LLMeshSkinInfo* skin = getSkinInfo();
        LLVOAvatar *avatar = getAvatar();
        LLVolume *volume = getVolume();
        if (skin && avatar && volume)
        {
            if (getLOD()>mLastRiggingInfoLOD || getLOD()==3)
            {
                // Rigging info may need update
                mJointRiggingInfoTab.clear();
                for (S32 f = 0; f < volume->getNumVolumeFaces(); ++f)
                {
                    LLVolumeFace& vol_face = volume->getVolumeFace(f);
                    LLSkinningUtil::updateRiggingInfo(skin, avatar, vol_face);
                    if (vol_face.mJointRiggingInfoTab.size()>0)
                    {
                        mJointRiggingInfoTab.merge(vol_face.mJointRiggingInfoTab);
                    }
                }
                // Keep the highest LOD info available.
                mLastRiggingInfoLOD = getLOD();
            }
        }
    }
}

//----------------------------------------------------------------------------

void LLVOVolume::generateSilhouette(LLSelectNode* nodep, const LLVector3& view_point)
{
    LLVolume *volume = getVolume();

    if (volume)
    {
        LLVector3 view_vector;
        view_vector = view_point;

        //transform view vector into volume space
        view_vector -= getRenderPosition();
        //mDrawable->mDistanceWRTCamera = view_vector.length();
        LLQuaternion worldRot = getRenderRotation();
        view_vector = view_vector * ~worldRot;
        if (!isVolumeGlobal())
        {
            LLVector3 objScale = getScale();
            LLVector3 invObjScale(1.f / objScale.mV[VX], 1.f / objScale.mV[VY], 1.f / objScale.mV[VZ]);
            view_vector.scaleVec(invObjScale);
        }

        updateRelativeXform();
        LLMatrix4 trans_mat = mRelativeXform;
        if (mDrawable->isStatic())
        {
            trans_mat.translate(getRegion()->getOriginAgent());
        }

        volume->generateSilhouetteVertices(nodep->mSilhouetteVertices, nodep->mSilhouetteNormals, view_vector, trans_mat, mRelativeXformInvTrans, nodep->getTESelectMask());

        nodep->mSilhouetteExists = true;
    }
}

void LLVOVolume::deleteFaces()
{
    S32 face_count = mNumFaces;
    if (mDrawable.notNull())
    {
        mDrawable->deleteFaces(0, face_count);
    }

    mNumFaces = 0;
}

void LLVOVolume::updateRadius()
{
    if (mDrawable.isNull())
    {
        return;
    }

    mVObjRadius = getScale().length();
    mDrawable->setRadius(mVObjRadius);
}


bool LLVOVolume::isAttachment() const
{
    return mAttachmentState != 0 ;
}

bool LLVOVolume::isHUDAttachment() const
{
    // *NOTE: we assume hud attachment points are in defined range
    // since this range is constant for backwards compatibility
    // reasons this is probably a reasonable assumption to make
    S32 attachment_id = ATTACHMENT_ID_FROM_STATE(mAttachmentState);
    return ( attachment_id >= 31 && attachment_id <= 38 );
}


const LLMatrix4 LLVOVolume::getRenderMatrix() const
{
    if (mDrawable->isActive() && !mDrawable->isRoot())
    {
        return mDrawable->getParent()->getWorldMatrix();
    }
    return mDrawable->getWorldMatrix();
}

//static
S32 LLVOVolume::getTextureCost(const LLViewerTexture* img)
{
    static const U32 ARC_TEXTURE_COST = 16; // multiplier for texture resolution - performance tested

    S32 texture_cost = 0;
    S8 type = img->getType();
    if (type == LLViewerTexture::FETCHED_TEXTURE || type == LLViewerTexture::LOD_TEXTURE)
    {
        const LLViewerFetchedTexture* fetched_texturep = static_cast<const LLViewerFetchedTexture*>(img);
        if (fetched_texturep
            && fetched_texturep->getFTType() == FTT_LOCAL_FILE
            && (img->getID() == IMG_ALPHA_GRAD_2D || img->getID() == IMG_ALPHA_GRAD)
            )
        {
            // These two textures appear to switch between each other, but are of different sizes (4x256 and 256x256).
            // Hardcode cost from larger one to not cause random complexity changes
            texture_cost = 320;
        }
    }
    if (texture_cost == 0)
    {
        texture_cost = 256 + (S32)(ARC_TEXTURE_COST * (img->getFullHeight() / 128.f + img->getFullWidth() / 128.f));
    }

    return texture_cost;
}

// Returns a base cost and adds textures to passed in set.
// total cost is returned value + 5 * size of the resulting set.
// Cannot include cost of textures, as they may be re-used in linked
// children, and cost should only be increased for unique textures  -Nyx
U32 LLVOVolume::getRenderCost(texture_cost_t &textures) const
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_VOLUME;
    /*****************************************************************
     * This calculation should not be modified by third party viewers,
     * since it is used to limit rendering and should be uniform for
     * everyone. If you have suggested improvements, submit them to
     * the official viewer for consideration.
     *****************************************************************/

    // Get access to params we'll need at various points.
    // Skip if this is object doesn't have a volume (e.g. is an avatar).
    if (getVolume() == NULL)
    {
        return 0;
    }

    U32 num_triangles = 0;

    // per-prim costs
    static const U32 ARC_PARTICLE_COST = 1; // determined experimentally
    static const U32 ARC_PARTICLE_MAX = 2048; // default values
    static const U32 ARC_LIGHT_COST = 500; // static cost for light-producing prims
    static const U32 ARC_MEDIA_FACE_COST = 1500; // static cost per media-enabled face


    // per-prim multipliers
    static const F32 ARC_GLOW_MULT = 1.5f; // tested based on performance
    static const F32 ARC_BUMP_MULT = 1.25f; // tested based on performance
    static const F32 ARC_FLEXI_MULT = 5; // tested based on performance
    static const F32 ARC_SHINY_MULT = 1.6f; // tested based on performance
    static const F32 ARC_INVISI_COST = 1.2f; // tested based on performance
    static const F32 ARC_WEIGHTED_MESH = 1.2f; // tested based on performance

    static const F32 ARC_PLANAR_COST = 1.0f; // tested based on performance to have negligible impact
    static const F32 ARC_ANIM_TEX_COST = 4.f; // tested based on performance
    static const F32 ARC_ALPHA_COST = 4.f; // 4x max - based on performance

    F32 shame = 0;

    U32 invisi = 0;
    U32 shiny = 0;
    U32 glow = 0;
    U32 alpha = 0;
    U32 flexi = 0;
    U32 animtex = 0;
    U32 particles = 0;
    U32 bump = 0;
    U32 planar = 0;
    U32 weighted_mesh = 0;
    U32 produces_light = 0;
    U32 media_faces = 0;

    const LLDrawable* drawablep = mDrawable;
    S32 num_faces = drawablep->getNumFaces();

    const LLVolumeParams& volume_params = getVolume()->getParams();

    LLMeshCostData costs;
    if (getCostData(costs))
    {
        if (isAnimatedObjectFast() && isRiggedMeshFast())
        {
            // Scaling here is to make animated object vs
            // non-animated object ARC proportional to the
            // corresponding calculations for streaming cost.
            num_triangles = (U32)((ANIMATED_OBJECT_COST_PER_KTRI * 0.001f * costs.getEstTrisForStreamingCost())/0.06f);
        }
        else
        {
            F32 radius = getScale().length()*0.5f;
            num_triangles = (U32)costs.getRadiusWeightedTris(radius);
        }
    }


    if (num_triangles <= 0)
    {
        num_triangles = 4;
    }

    if (isSculptedFast())
    {
        if (isMeshFast())
        {
            // base cost is dependent on mesh complexity
            // note that 3 is the highest LOD as of the time of this coding.
            S32 size = gMeshRepo.getMeshSize(volume_params.getSculptID(), getLOD());
            if ( size > 0)
            {
                if (isRiggedMeshFast())
                {
                    // weighted attachment - 1 point for every 3 bytes
                    weighted_mesh = 1;
                }
            }
            else
            {
                // something went wrong - user should know their content isn't render-free
                return 0;
            }
        }
        else
        {
            LLViewerFetchedTexture* texture = mSculptTexture;
            if (texture && textures.find(texture) == textures.end())
            {
                textures.insert(texture);
            }
        }
    }

    if (isFlexibleFast())
    {
        flexi = 1;
    }
    if (isParticleSource())
    {
        particles = 1;
    }

    if (getIsLightFast())
    {
        produces_light = 1;
    }

    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_VOLUME("ARC - face list");
        for (S32 i = 0; i < num_faces; ++i)
        {
            const LLFace* face = drawablep->getFace(i);
            if (!face) continue;
            const LLTextureEntry* te = face->getTextureEntry();
            const LLViewerTexture* img = face->getTexture();

            if (img)
            {
                textures.insert(img);
            }

            if (face->isInAlphaPool())
            {
                alpha = 1;
            }
            else if (img && img->getPrimaryFormat() == LLPixelFormat::ALPHA)
            {
                invisi = 1;
            }
            if (face->hasMedia())
            {
                media_faces++;
            }

            if (te)
            {
                if (te->getBumpmap())
                {
                    // bump is a multiplier, don't add per-face
                    bump = 1;
                }
                if (te->getShiny())
                {
                    // shiny is a multiplier, don't add per-face
                    shiny = 1;
                }
                if (te->getGlow() > 0.f)
                {
                    // glow is a multiplier, don't add per-face
                    glow = 1;
                }
                if (face->mTextureMatrix != NULL)
                {
                    animtex = 1;
                }
                if (te->getTexGen())
                {
                    planar = 1;
                }
            }
        }
    }

    // shame currently has the "base" cost of 1 point per 15 triangles, min 2.
    shame = num_triangles  * 5.f;
    shame = shame < 2.f ? 2.f : shame;

    // multiply by per-face modifiers
    if (planar)
    {
        shame *= planar * ARC_PLANAR_COST;
    }

    if (animtex)
    {
        shame *= animtex * ARC_ANIM_TEX_COST;
    }

    if (alpha)
    {
        shame *= alpha * ARC_ALPHA_COST;
    }

    if(invisi)
    {
        shame *= invisi * ARC_INVISI_COST;
    }

    if (glow)
    {
        shame *= glow * ARC_GLOW_MULT;
    }

    if (bump)
    {
        shame *= bump * ARC_BUMP_MULT;
    }

    if (shiny)
    {
        shame *= shiny * ARC_SHINY_MULT;
    }


    // multiply shame by multipliers
    if (weighted_mesh)
    {
        shame *= weighted_mesh * ARC_WEIGHTED_MESH;
    }

    if (flexi)
    {
        shame *= flexi * ARC_FLEXI_MULT;
    }


    // add additional costs
    if (particles)
    {
        const LLPartSysData *part_sys_data = &(mPartSourcep->mPartSysData);
        const LLPartData *part_data = &(part_sys_data->mPartData);
        U32 num_particles = (U32)(part_sys_data->mBurstPartCount * llceil( part_data->mMaxAge / part_sys_data->mBurstRate));
        num_particles = num_particles > ARC_PARTICLE_MAX ? ARC_PARTICLE_MAX : num_particles;
        F32 part_size = (llmax(part_data->mStartScale[0], part_data->mEndScale[0]) + llmax(part_data->mStartScale[1], part_data->mEndScale[1])) / 2.f;
        shame += num_particles * part_size * ARC_PARTICLE_COST;
    }

    if (produces_light)
    {
        shame += ARC_LIGHT_COST;
    }

    if (media_faces)
    {
        shame += media_faces * ARC_MEDIA_FACE_COST;
    }

    // Streaming cost for animated objects includes a fixed cost
    // per linkset. Add a corresponding charge here translated into
    // triangles, but not weighted by any graphics properties.
    if (isAnimatedObjectFast() && isRootEdit())
    {
        shame += (ANIMATED_OBJECT_BASE_COST/0.06) * 5.0f;
    }

    if (shame > mRenderComplexity_current)
    {
        mRenderComplexity_current = (S32)shame;
    }

    return (U32)shame;
}

F32 LLVOVolume::getEstTrianglesMax() const
{
    if (isMeshFast() && getVolume())
    {
        return gMeshRepo.getEstTrianglesMax(getVolume()->getParams().getSculptID());
    }
    return 0.f;
}

F32 LLVOVolume::getEstTrianglesStreamingCost() const
{
    if (isMeshFast() && getVolume())
    {
        return gMeshRepo.getEstTrianglesStreamingCost(getVolume()->getParams().getSculptID());
    }
    return 0.f;
}

F32 LLVOVolume::getStreamingCost() const
{
    F32 radius = getScale().length()*0.5f;
    F32 linkset_base_cost = 0.f;

    LLMeshCostData costs;
    if (getCostData(costs))
    {
        if (isRootEdit() && isAnimatedObject())
        {
            // Root object of an animated object has this to account for skeleton overhead.
            linkset_base_cost = ANIMATED_OBJECT_BASE_COST;
        }
        if (isMesh())
        {
            if (isAnimatedObject() && isRiggedMesh())
            {
                return linkset_base_cost + costs.getTriangleBasedStreamingCost();
            }
            else
            {
                return linkset_base_cost + costs.getRadiusBasedStreamingCost(radius);
            }
        }
        else
        {
            return linkset_base_cost + costs.getRadiusBasedStreamingCost(radius);
        }
    }
    else
    {
        return 0.f;
    }
}

// virtual
bool LLVOVolume::getCostData(LLMeshCostData& costs) const
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_VOLUME;

    if (isMeshFast())
    {
        return gMeshRepo.getCostData(getVolume()->getParams().getSculptID(), costs);
    }
    else
    {
        LLVolume* volume = getVolume();
        S32 counts[4];
        LLVolume::getLoDTriangleCounts(volume->getParams(), counts);

        LLMeshHeader header;
        header.mLodSize[0] = counts[0] * 10;
        header.mLodSize[1] = counts[1] * 10;
        header.mLodSize[2] = counts[2] * 10;
        header.mLodSize[3] = counts[3] * 10;

        return gMeshRepo.getCostData(header, costs);
    }
}

//static
void LLVOVolume::updateRenderComplexity()
{
    mRenderComplexity_last = mRenderComplexity_current;
    mRenderComplexity_current = 0;
}

U32 LLVOVolume::getTriangleCount(S32* vcount) const
{
    U32 count = 0;
    LLVolume* volume = getVolume();
    if (volume)
    {
        count = volume->getNumTriangles(vcount);
    }

    return count;
}

U32 LLVOVolume::getHighLODTriangleCount()
{
    U32 ret = 0;

    LLVolume* volume = getVolume();

    if (!isSculpted())
    {
        LLVolume* ref = LLPrimitive::getVolumeManager()->refVolume(volume->getParams(), 3);
        ret = ref->getNumTriangles();
        LLPrimitive::getVolumeManager()->unrefVolume(ref);
    }
    else if (isMesh())
    {
        LLVolume* ref = LLPrimitive::getVolumeManager()->refVolume(volume->getParams(), 3);
        if (!ref->isMeshAssetLoaded() || ref->getNumVolumeFaces() == 0)
        {
            gMeshRepo.loadMesh(this, volume->getParams(), LLModel::LOD_HIGH);
        }
        ret = ref->getNumTriangles();
        LLPrimitive::getVolumeManager()->unrefVolume(ref);
    }
    else
    { //default sculpts have a constant number of triangles
        ret = 31*2*31;  //31 rows of 31 columns of quads for a 32x32 vertex patch
    }

    return ret;
}

// [FS:Beq] - Patch: Appearance-RebuildAttachments | Checked: Catznip-5.3
void LLVOVolume::forceLOD(S32 lod)
{
// [SL:KB] - Patch: Appearance-RebuildAttachments | Checked: Catznip-5.3
    if (mDrawable.isNull())
        return;
// [/SL:KB]

    mLOD = lod;
    gPipeline.markRebuild(mDrawable, LLDrawable::REBUILD_VOLUME);
    mLODChanged = true;
}
// [/FS]

//static
void LLVOVolume::preUpdateGeom()
{
    sNumLODChanges = 0;
}

void LLVOVolume::parameterChanged(U16 param_type, bool local_origin)
{
    LLViewerObject::parameterChanged(param_type, local_origin);
}

void LLVOVolume::parameterChanged(U16 param_type, LLNetworkData* data, bool in_use, bool local_origin)
{
    LLViewerObject::parameterChanged(param_type, data, in_use, local_origin);
    if (mVolumeImpl)
    {
        mVolumeImpl->onParameterChanged(param_type, data, in_use, local_origin);
    }
    if (!local_origin && param_type == LLNetworkData::PARAMS_EXTENDED_MESH)
    {
        U32 extended_mesh_flags = getExtendedMeshFlags();
        bool enabled =  (extended_mesh_flags & LLExtendedMeshParams::ANIMATED_MESH_ENABLED_FLAG);
        bool was_enabled = (getControlAvatar() != NULL);
        if (enabled != was_enabled)
        {
            LL_DEBUGS("AnimatedObjects") << this
                                         << " calling onSetExtendedMeshFlags, enabled " << (U32) enabled
                                         << " was_enabled " << (U32) was_enabled
                                         << " local_origin " << (U32) local_origin
                                         << LL_ENDL;
            onSetExtendedMeshFlags(extended_mesh_flags);
        }
    }
    if (mDrawable.notNull())
    {
        bool is_light = getIsLight();
        if (is_light != mDrawable->isState(LLDrawable::LIGHT))
        {
            gPipeline.setLight(mDrawable, is_light);
        }
    }

    updateReflectionProbePtr();
}

// VK (C11): objects that became (or were created as) a non-mirror reflection probe, for
// the Vulkan probe discovery in llappviewer.cpp (drained every frame there), so it no
// longer has to scan thousands of objects per frame to find them. Bounded: past the
// cap new entries are dropped -- the discovery's slow safety scan still finds them.
std::vector<LLUUID> gVKNewReflectionProbes;

void LLVOVolume::updateReflectionProbePtr()
{
    if (isReflectionProbe() && !getReflectionProbeIsMirror() && gVKNewReflectionProbes.size() < 4096)
        gVKNewReflectionProbes.push_back(getID());
    if (isReflectionProbe())
    {
        if (!mIsReflectionProbe && !getReflectionProbeIsMirror())
        {
            mIsReflectionProbe = true;
        }
        else if (!mIsReflectionProbe && getReflectionProbeIsMirror())
        {
            // Geenz: This is a special case - what we want here is a hero probe.
            // What we want to do here is instantiate a hero probe from the hero probe manager.

            if (!mIsHeroProbe)
                mIsHeroProbe = gPipeline.mHeroProbeManager.registerViewerObject(this);
        }
    }
    else if (mIsReflectionProbe || getReflectionProbeIsMirror())
    {
        mIsReflectionProbe = false;

        if (getReflectionProbeIsMirror())
        {
            gPipeline.mHeroProbeManager.unregisterViewerObject(this);
        }
    }
}

void LLVOVolume::setSelected(bool sel)
{
    LLViewerObject::setSelected(sel);
    if (isAnimatedObject())
    {
        getRootEdit()->recursiveMarkForUpdate();
    }
    else
    {
        if (mDrawable.notNull())
        {
            markForUpdate();
        }
    }
}

void LLVOVolume::updateSpatialExtents(LLVector4a& newMin, LLVector4a& newMax)
{
}

F32 LLVOVolume::getBinRadius()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_VOLUME;
    F32 radius;

    // The bin radius sizes the object for the object cache (scene contribution, the grid,
    // the VRAM far-geometry kill) and the shrink-wrap / LOD re-bound trigger. These were
    // the defaults of the octree sizing settings (OctreeStaticObjectSizeFactor,
    // OctreeAlphaDistanceFactor[2]); the settings went with the octrees (#480).
    constexpr S32 size_factor = 3;              // minimum radius of a static object
    constexpr F32 ALPHA_WRAP_DISTANCE = 64.f;   // alpha objects nearer than this wrap tight

    bool shrink_wrap = mShouldShrinkWrap || mDrawable->isAnimating();
    bool alpha_wrap = false;

    if (!isHUDAttachment() && mDrawable->mDistanceWRTCamera < ALPHA_WRAP_DISTANCE)
    {
        for (S32 i = 0; i < mDrawable->getNumFaces(); i++)
        {
            LLFace* face = mDrawable->getFace(i);
            if (!face) continue;
            if (face->isInAlphaPool() &&
                !face->canRenderAsMask())
            {
                alpha_wrap = true;
                break;
            }
        }
    }
    else
    {
        shrink_wrap = false;
    }

    if (alpha_wrap)
    {
        LLVector3 bounds = getScale();
        radius = llmin(bounds.mV[1], bounds.mV[2]);
        radius = llmin(radius, bounds.mV[0]);
        radius *= 0.5f;
    }
    else if (shrink_wrap)
    {
        radius = mDrawable->getRadius() * 0.25f;
    }
    else
    {
        F32 szf = (F32)size_factor;
        radius = llmax(mDrawable->getRadius(), szf);
    }

    return llclamp(radius, 0.5f, 256.f);
}

const LLVector3 LLVOVolume::getPivotPositionAgent() const
{
    if (mVolumeImpl)
    {
        return mVolumeImpl->getPivotPosition();
    }
    return LLViewerObject::getPivotPositionAgent();
}

void LLVOVolume::onShift(const LLVector4a &shift_vector)
{
    if (mVolumeImpl)
    {
        mVolumeImpl->onShift(shift_vector);
    }

    updateRelativeXform();
}

const LLMatrix4& LLVOVolume::getWorldMatrix(LLXformMatrix* xform) const
{
    if (mVolumeImpl)
    {
        return mVolumeImpl->getWorldMatrix(xform);
    }
    return xform->getWorldMatrix();
}

void LLVOVolume::markForUpdate()
{
    if (mDrawable)
    {
        shrinkWrap();
    }

    LLViewerObject::markForUpdate();
    mVolumeChanged = true;
}

LLVector3 LLVOVolume::agentPositionToVolume(const LLVector3& pos) const
{
    LLVector3 ret = pos - getRenderPosition();
    ret = ret * ~getRenderRotation();
    if (!isVolumeGlobal())
    {
        LLVector3 objScale = getScale();
        LLVector3 invObjScale(1.f / objScale.mV[VX], 1.f / objScale.mV[VY], 1.f / objScale.mV[VZ]);
        ret.scaleVec(invObjScale);
    }

    return ret;
}

LLVector3 LLVOVolume::agentDirectionToVolume(const LLVector3& dir) const
{
    LLVector3 ret = dir * ~getRenderRotation();

    LLVector3 objScale = isVolumeGlobal() ? LLVector3(1,1,1) : getScale();
    ret.scaleVec(objScale);

    return ret;
}

LLVector3 LLVOVolume::volumePositionToAgent(const LLVector3& dir) const
{
    LLVector3 ret = dir;
    if (!isVolumeGlobal())
    {
        LLVector3 objScale = getScale();
        ret.scaleVec(objScale);
    }

    ret = ret * getRenderRotation();
    ret += getRenderPosition();

    return ret;
}

LLVector3 LLVOVolume::volumeDirectionToAgent(const LLVector3& dir) const
{
    LLVector3 ret = dir;
    LLVector3 objScale = isVolumeGlobal() ? LLVector3(1,1,1) : getScale();
    LLVector3 invObjScale(1.f / objScale.mV[VX], 1.f / objScale.mV[VY], 1.f / objScale.mV[VZ]);
    ret.scaleVec(invObjScale);
    ret = ret * getRenderRotation();

    return ret;
}



// General 4x4 inverse (cofactor / glu form) on the raw 16 floats. NOT
// LLMatrix4::invert(), which assumes a rigid transform (transposes the 3x3): for a
// SCALED prim it multiplies by the scale instead of dividing, which maps every
// point on screen inside a small HUD prim's unit box -- one HUD "hit" every click
// everywhere. Layout-agnostic (inverse of the transpose = transpose of the inverse),
// so it yields M^-1 in LLMatrix4's own row-vector layout. False if singular.
static bool invertGeneral(const LLMatrix4& src, LLMatrix4& dst)
{
    const F32* m = &src.mMatrix[0][0];
    F32 inv[16];
    inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8]  =  m[4]*m[9]*m[15]  - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14]  + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9]  = -m[0]*m[9]*m[15]  + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] =  m[0]*m[9]*m[14]  - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2]  =  m[1]*m[6]*m[15]  - m[1]*m[7]*m[14]  - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7]  - m[13]*m[3]*m[6];
    inv[6]  = -m[0]*m[6]*m[15]  + m[0]*m[7]*m[14]  + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7]  + m[12]*m[3]*m[6];
    inv[10] =  m[0]*m[5]*m[15]  - m[0]*m[7]*m[13]  - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7]  - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14]  + m[0]*m[6]*m[13]  + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6]  + m[12]*m[2]*m[5];
    inv[3]  = -m[1]*m[6]*m[11]  + m[1]*m[7]*m[10]  + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7]   + m[9]*m[3]*m[6];
    inv[7]  =  m[0]*m[6]*m[11]  - m[0]*m[7]*m[10]  - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7]   - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11]  + m[0]*m[7]*m[9]   + m[4]*m[1]*m[11] - m[4]*m[3]*m[9]  - m[8]*m[1]*m[7]   + m[8]*m[3]*m[5];
    inv[15] =  m[0]*m[5]*m[10]  - m[0]*m[6]*m[9]   - m[4]*m[1]*m[10] + m[4]*m[2]*m[9]  + m[8]*m[1]*m[6]   - m[8]*m[2]*m[5];
    const F32 det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (!(fabsf(det) > 1e-20f)) return false;
    const F32 r = 1.f / det;
    F32* d = &dst.mMatrix[0][0];
    for (int i = 0; i < 16; ++i) d[i] = inv[i] * r;
    return true;
}

void vkRenderLocalPose(const LLViewerObject* obj, LLVector3& pos, LLQuaternion& rot, LLVector3* scale)
{
    const LLDrawable* dr = obj->mDrawable;
    // ACTIVE drawables (LL is moving them) too, not only while ON_MOVE_LIST: a seat's
    // root gets an EARLY damped move from the seated avatar's idleUpdate
    // (updateMoveDampedAsync), and updateMovedList then drops it from the move list
    // before our pose refresh runs. Reading object state there drew the vehicle at the
    // undamped network target while the seated avatar and the camera followed the
    // eased drawable -> vehicle stutter relative to its driver (#96).
    static LLCachedControl<bool> s_active(gSavedSettings, "RenderVKActivePoseFromDrawable", true);
    // World frozen (LLWorldClock): LLPipeline::updateMove is held, so a drawable that
    // has never been moved (new arrival: markMoved'd at creation, still inactive) has an
    // UNINITIALISED mXform -- it would draw at the agent origin until thaw. Those use
    // object state; already-moving (active) drawables keep their frozen eased pose.
    const bool frozen_unmoved = dr && LLWorldClock::isFrozen() && !dr->isActive();
    if (dr && !dr->isDead() && !frozen_unmoved
        && (dr->isState(LLDrawable::ON_MOVE_LIST) || (s_active && dr->isActive())))
    {
        pos = dr->mXform.getPosition();
        rot = dr->mXform.getRotation();
        if (scale)
        {
            // Eased scale (#180): LLDrawable::updateXform lerps mCurrentScale toward the
            // object scale with the same damping as pos/rot. mCurrentScale is (1,1,1)
            // until the drawable's first updateMove (mGeneration -1): an active child
            // that was never moved itself would draw at unit size -- object scale then.
            *scale = (dr->getGeneration() >= 0) ? dr->getScale() : obj->getScale();
        }
        return;
    }
    // Settled: object state (the #29-safe source -- drawable state can hold garbage
    // after LL's mass drawable resets, but not while LL is actively easing it).
    pos = obj->getParent() ? obj->getPosition() : obj->getPositionAgent();
    rot = obj->getRotation();
    if (scale) *scale = obj->getScale();
}

bool vkUsesGpuSpin(const LLViewerObject* obj)
{
    // LL's applyAngularVelocity threshold. Attachments keep the CPU-applied spin: their
    // pose is joint-relative and the GPU spin is world-space.
    if (!obj || obj->getPCode() != LL_PCODE_VOLUME || obj->isAttachment()
        || obj->getAngularVelocity().magVecSquared() <= 0.00001f)
    {
        return false;
    }
    // RenderVKGpuSpinMovingLinksets restores the old rule (any omega spins on the GPU).
    static LLCachedControl<bool> s_moving(gSavedSettings, "RenderVKGpuSpinMovingLinksets", false);
    if (s_moving) return true;
    auto moving = [](const LLViewerObject* o)
    {
        return o->flagUsePhysics() || o->isSeat() || !o->getVelocity().isExactlyZero();
    };
    if (moving(obj)) return false;
    const LLViewerObject* root = obj->getRootEdit();
    return !(root && root != obj && moving(root));
}

bool vkComposeAttachmentMatrix(const LLViewerObject* pObj, LLMatrix4& out)
{
    // Prim pose relative to its HUD attach point (root pose is attach-point relative,
    // child pose root relative), then the attach point's world matrix (HUD space,
    // posed by the avatar joint update). Row-vector convention: v' = v * M.
    // Eased (LL-rendered) local poses: a scripted HUD part rotating/sliding glides.
    LLVector3    hpos; LLQuaternion hrot;
    LLVector3    hscale;   // eased with the pose (#180)
    vkRenderLocalPose(pObj, hpos, hrot, &hscale);
    const LLViewerObject* hroot = pObj->getRootEdit();
    if (hroot && hroot != pObj)
    {
        LLVector3 rpos; LLQuaternion rrot;
        vkRenderLocalPose(hroot, rpos, rrot);
        hpos = hpos * rrot + rpos;
        hrot = hrot * rrot;
    }
    out.initRows(LLVector4(LLVector3(hscale.mV[VX], 0.f, 0.f) * hrot, 0.f),
                 LLVector4(LLVector3(0.f, hscale.mV[VY], 0.f) * hrot, 0.f),
                 LLVector4(LLVector3(0.f, 0.f, hscale.mV[VZ]) * hrot, 0.f),
                 LLVector4(hpos, 1.f));
    LLVOAvatar* hav = const_cast<LLViewerObject*>(pObj)->getAvatarAncestor();
    LLViewerJointAttachment* hap = hav
        ? hav->getTargetAttachmentPoint(const_cast<LLViewerObject*>(hroot ? hroot : pObj))
        : nullptr;
    if (!hap) return false;
    out *= LLMatrix4(hap->getWorldMatrix4a());
    return true;
}

bool vkAttachmentAgentToVolume(const LLViewerObject* obj, const LLVector3& pos, const LLVector3& dir,
                               LLVector3& out_pos, LLVector3& out_dir)
{
    // Inverse of the attachment's RENDERED pose (vkComposeAttachmentMatrix, which is
    // what picking intersects), matching LLVOVolume::agentPositionToVolume /
    // agentDirectionToVolume semantics: position -> unscaled volume space,
    // direction -> rotation only (no scale). Under VK the drawable transform those
    // use is stale for attachments.
    LLMatrix4 M;
    if (!obj || !vkComposeAttachmentMatrix(obj, M)) return false;
    LLMatrix4 Minv;
    if (!invertGeneral(M, Minv)) return false;
    out_pos = pos * Minv;
    for (int i = 0; i < 3; ++i)
    {
        LLVector3 axis(M.mMatrix[i][0], M.mMatrix[i][1], M.mMatrix[i][2]);
        axis.normVec();
        out_dir.mV[i] = dir * axis;
    }
    return true;
}

bool LLVOVolume::vkPickFiltersPass(bool pick_unselectable)
{
    if (!mbCanSelect || mDrawable.isNull() || mDrawable->isDead()
        || !gPipeline.hasRenderType(mDrawable->getRenderType()))
    {
        return false;
    }
    // #Slipstream Helper (#537): not drawn, so not pickable by any path (HUD and world
    // picks, hover, touch, tooltips) -- whatever pick_unselectable says.
    if (SLBridge::instanceExists() && SLBridge::instance().isHelperObject(this))
    {
        return false;
    }
    // Mouselook with "Show me in Mouselook" off (#554): the main view doesn't draw our
    // own worn attachments (clean_cull.comp / clean_draw_rigged.vert, hide_self), so no
    // pick hits them -- a click aims at what is under the crosshair, not at the head
    // attachment the camera sits inside. HUD attachments are unaffected.
    if (LLVOAvatar::isHiddenOwnAttachment(this))
    {
        return false;
    }
    if (!pick_unselectable && !LLSelectMgr::instance().canSelectObject(this, true))
    {
        return false;
    }
    if (getClickAction() == CLICK_ACTION_IGNORE && !LLFloater::isVisible(gFloaterTools))
    {
        return false;
    }
    return true;
}

U32 LLVOVolume::getPartitionType() const
{
    if (isHUDAttachment())
    {
        return LLViewerRegion::PARTITION_HUD;
    }
    if (isAnimatedObject() && getControlAvatar())
    {
        return LLViewerRegion::PARTITION_CONTROL_AV;
    }
    if (isAttachment())
    {
        return LLViewerRegion::PARTITION_AVATAR;
    }

    return LLViewerRegion::PARTITION_VOLUME;
}

// Phase 1 decoupling -- face-intrinsic alpha classification for the clean
// GPU-driven pipeline.
//
// The clean instance needs three render-relevant booleans: alpha-blend (the
// WBOIT transparency pass), alpha-mask (fragment discard), and fullbright
// (unlit). These used to be inferred from the LLRenderPass 'type' that
// genDrawInfo (LL's Path A batch build) happened to be iterating when it
// called registerFace. That coupled the clean store to Path A two ways:
//   - the clean classification could not be computed without LL having
//     already bucketed the face into a pass, and
//   - a face registered for several passes (e.g. the trailing PASS_GLOW
//     emitted for any glowing opaque face) had its clean classification
//     overwritten by whichever registerFace call came last -- statics were
//     silently clobbered to "opaque" by the glow pass.
//
// Derive the booleans straight from the TextureEntry / material / texture
// instead, using only STATELESS helpers (getPoolTypeFromTE, the GLTF alpha
// mode, LLFace::canRenderAsMask) that carry no Path A batch state. This is
// the prerequisite for Phase 2 gating genDrawInfo: the clean store owns its
// own classification, and multiple pass-type registrations of one face now
// collapse to a single, identical, type-independent instance.
//
// getPoolTypeFromTE is still an LL utility -- it encapsulates the exact
// colour-alpha + texture-component + material-alpha-mode test rebuildGeom
// uses to assign POOL_ALPHA, so calling it guarantees parity with LL's blend
// decision. It can be reimplemented clean-side later; it is not part of the
// genDrawInfo batch build being gated.
// PER-MAP UV TRANSFORMS for the clean material. LL samples the normal / metallic-
// roughness (legacy: specular) / emissive maps with their OWN transform: PBR via
// texture_transform(texcoord0, texture_<slot>_transform, ...) per glTF slot
// (pbropaqueV.glsl:125-128), legacy via llface.cpp's xform() fed LLMaterial's
// normal/specular rotation, offset and repeats (getGeometryVolume channels 1/2).
// Same affine forms as the base-colour uv_matrix built in registerCleanFace.
// Self/HUD priority (instance flag bit13): HUD attachments, and anything whose
// avatar ancestor is the agent (attachments, their linkset children, attached animesh).
static bool cleanIsSelfPriority(LLViewerObject* obj)
{
    if (!obj) return false;
    if (obj->isHUDAttachment()) return true;
    return isAgentAvatarValid() && obj->getAvatarAncestor() == gAgentAvatarp.get();
}

static void cleanFillMapUV(LLCleanMaterialStore::MaterialDesc& d, const LLTextureEntry* te)
{
    if (!te) return;
    auto gltf6 = [](const LLGLTFMaterial::TextureTransform& tt, F32 o[6])
    {
        // uv' = flip(offset + R * (S * flip(uv))), flip(v) = 1 - v (KHR_texture_transform).
        const F32 c = cosf(tt.mRotation), sn = sinf(tt.mRotation);
        const F32 sx = tt.mScale.mV[VX], sy = tt.mScale.mV[VY];
        const F32 ox = tt.mOffset.mV[VX], oy = tt.mOffset.mV[VY];
        o[0] = c * sx;  o[1] = -sn * sy; o[2] = sn * sy + ox;
        o[3] = sn * sx; o[4] = c * sy;   o[5] = 1.f - c * sy - oy;
    };
    auto legacy6 = [](F32 r, F32 os, F32 ot, F32 ms, F32 mt, F32 o[6])
    {
        // llface.cpp xform(): about the face centre, rotate, then scale, then offset.
        const F32 c = cosf(r), sn = sinf(r);
        o[0] = ms * c;   o[1] = ms * sn;  o[2] = os + 0.5f - 0.5f * (ms * c + ms * sn);
        o[3] = -mt * sn; o[4] = mt * c;   o[5] = ot + 0.5f - 0.5f * (-mt * sn + mt * c);
    };
    if (const LLGLTFMaterial* gm = te->getGLTFRenderMaterial())
    {
        const S32 slot[3] = { LLGLTFMaterial::GLTF_TEXTURE_INFO_NORMAL,
                              LLGLTFMaterial::GLTF_TEXTURE_INFO_METALLIC_ROUGHNESS,
                              LLGLTFMaterial::GLTF_TEXTURE_INFO_EMISSIVE };
        for (int k = 0; k < 3; ++k)
        {
            gltf6(gm->mTextureTransform[slot[k]], d.uv_xform[k]);
            d.uv_own[k] = true;
        }
        return;
    }
    if (const LLMaterial* lm = te->getMaterialParams().get())
    {
        F32 os, ot, ms, mt;
        if (lm->getNormalID().notNull())
        {
            lm->getNormalOffset(os, ot);
            lm->getNormalRepeat(ms, mt);
            legacy6(lm->getNormalRotation(), os, ot, ms, mt, d.uv_xform[0]);
            d.uv_own[0] = true;
        }
        if (lm->getSpecularID().notNull())
        {
            lm->getSpecularOffset(os, ot);
            lm->getSpecularRepeat(ms, mt);
            legacy6(lm->getSpecularRotation(), os, ot, ms, mt, d.uv_xform[1]);
            d.uv_own[1] = true;
        }
    }
}

// LL's legacy SHININESS -> spec / env level (registerFace mSpecColor/mEnvIntensity,
// LLFace SHININESS_TO_ALPHA): None/Low/Medium/High = 0 / .25 / .5 / .75.
static F32 cleanShinyLevel(const LLTextureEntry* te)
{
    static const F32 kShiny[4] = { 0.f, 0.25f, 0.5f, 0.75f };
    return te ? kShiny[te->getShiny() & 3] : 0.f;
}

// LEGACY BUMP PRESETS (#19). TE bumpmap codes: 0 none, 1 brightness, 2 darkness (both
// derived from the face's own diffuse texture), 3..17 the standard heightfields listed in
// app_settings/std_bump.ini (woodgrain .. weave; LL LLStandardBumpmap::addstandard, in
// file order from code 3). LL only applies codes below 18 (registerFace use_legacy_bump).
static constexpr U8 kCleanBumpBrightness = 1;
static constexpr U8 kCleanBumpDarkness   = 2;
static constexpr U8 kCleanBumpCount      = 18;
static const LLUUID& cleanStdBumpId(U8 code)
{
    // (magic static: ingest may run on more than one thread)
    static const std::array<LLUUID, kCleanBumpCount> s_ids = []()
    {
        std::array<LLUUID, kCleanBumpCount> ids{};
        const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "std_bump.ini");
        llifstream in(path.c_str());
        std::string word;
        S32 version = 0;
        // Header: "LLStandardBumpmap version 1".
        if (in >> word && word == "LLStandardBumpmap" && in >> word && word == "version" && in >> version
            && version <= 1)
        {
            std::string label, id;
            for (U8 c = 3; c < kCleanBumpCount && (in >> label >> id); ++c)
                ids[c].set(id, false);
        }
        else
        {
            LL_WARNS("Vulkan") << "std_bump.ini missing or unreadable (" << path
                               << "): standard bump presets render flat" << LL_ENDL;
        }
        return ids;
    }();
    return code < kCleanBumpCount ? s_ids[code] : LLUUID::null;
}

// AUTO-MASK TELEMETRY (#82). Why a blend-candidate face (one LL would put in the
// alpha-blend pool) ended up where it did. CAM_NONE = not a blend candidate (opaque,
// or an authored alpha-MASK material) -- not counted.
enum ECleanAutoMask : U8
{
    CAM_NONE = 0,
    // demoted to alpha-MASK
    CAM_LL_MASK,        // LL's canRenderAsMask (strict analyzeAlpha mIsMask)
    CAM_STRICT,         // our content demotion via the strict mIsMask
    CAM_GRADIENT,       // our content demotion via gradient_pct <= RenderCleanAutoMaskMaxGradient
    CAM_NOALPHA,        // texture has no alpha channel (3 components)
    CAM_OPAQUE,         // post-classify reroute to the OPAQUE bucket (registerCleanFace)
    // stayed alpha-BLEND
    CAM_BLEND_TINT,     // tint / base colour alpha < 1 (real translucency)
    CAM_BLEND_PENDING,  // texture not analysed yet (or below the commit resolution)
    CAM_BLEND_GRADIENT, // analysed: too much partial alpha for a mask
    CAM_BLEND_OTHER,    // no texture / 1-component texture (no alpha channel to judge)
    CAM_BLEND_RIGGED,   // rigged (worn / playing animesh): LL never auto-masks these
    CAM_BLEND_GLOW,     // glowing: LL never auto-masks these
    CAM_BLEND_SHINY,    // shiny legacy material with a real alpha channel: LL's glare (#470)
    CAM_COUNT
};

// Rigged as registerCleanFace derives it (LL's rebuildGeom criteria: a skinned mesh
// worn as an attachment, or an animated object whose control avatar is playing).
// LLFace::RIGGED -- what LL's canRenderAsMask tests -- is set only by rebuildGeom,
// which the Vulkan frame never runs, so it is always clear there.
static bool cleanFaceIsRigged(LLFace* facep)
{
    LLDrawable* dr = facep ? facep->getDrawable() : nullptr;
    LLVOVolume* rvol = dr ? dr->getVOVolume() : nullptr;
    if (!rvol || !rvol->isMesh() || !rvol->getSkinInfo()) return false;
    if (rvol->isAnimatedObject())
    {
        LLControlAvatar* cav = rvol->getControlAvatar();
        return rvol->isRiggedMesh() && cav && cav->mPlaying;
    }
    return rvol->isAttachment();
}

// LL never auto-masks a rigged or a glowing alpha-blend face (LLFace::canRenderAsMask):
// glow is blended per-fragment from the alpha, and rigged content is often authored
// around sorted blending. The clean auto-mask demotions follow the same rule for glow;
// rigged faces (#561, RenderVKRiggedAutoMask) only to the pixel-equivalent classes. 0 =
// eligible, else the stay-blend reason.
static U8 cleanAutoMaskBlocker(LLFace* facep, const LLTextureEntry* te)
{
    if (te && te->getGlow() != 0.f) return CAM_BLEND_GLOW;
    // #561: rigged blend faces may be demoted (RenderVKRiggedAutoMask), but only
    // to classes that are pixel-equivalent as a mask (tex_maskable below): a texture
    // without alpha, or a strictly binary one -- never soft alpha (hair, lace, overlays).
    static LLCachedControl<bool> s_rig_mask(gSavedSettings, "RenderVKRiggedAutoMask", true);
    if (!s_rig_mask && cleanFaceIsRigged(facep)) return CAM_BLEND_RIGGED;
    // SHINY LEGACY MATERIAL (#470): LL draws a blend-mode Blinn-Phong material
    // forward with GLARE (materialF: al = max(texel alpha, glare) * vertex alpha) --
    // specular highlights and the shiny env reflection show at full strength even on
    // fully transparent texels (clear glass that shows only its glints). A mask
    // (cutoff discard) or opaque bucket can't: the transparent texels vanish with
    // their highlights. Only for textures with a real alpha channel; an opaque
    // texture (3 components) loses nothing by being demoted. RenderVKShinyBlendKeep
    // off = the old demotion (A/B; ordering of shiny binary-alpha foliage).
    static LLCachedControl<bool> s_keep(gSavedSettings, "RenderVKShinyBlendKeep", true);
    if (s_keep && te && !te->getGLTFRenderMaterial())
    {
        if (const LLMaterial* lm = te->getMaterialParams().get())
        {
            const bool shiny = lm->getSpecularID().notNull()
                ? (lm->getSpecularLightExponent() > 0 || lm->getEnvironmentIntensity() > 0)
                : cleanShinyLevel(te) > 0.f;
            const LLViewerTexture* t = facep ? facep->getTexture() : nullptr;
            const S32 c = t ? (S32)t->getComponents() : 0;
            if (shiny && (c == 2 || c == 4)) return CAM_BLEND_SHINY;
        }
    }
    return CAM_NONE;
}

// Fullbright as the clean renderer sees it. SLIgnoreFullbright (#566) renders world
// fullbright faces normally lit; HUD attachments keep their fullbright, and glow /
// emissive are separate material fields so they are untouched.
static bool cleanEffectiveFullbright(LLFace* facep, const LLTextureEntry* te)
{
    if (!te || !te->getFullbright()) return false;
    static LLCachedControl<bool> ignore_fb(gSavedSettings, "SLIgnoreFullbright", false);
    if (!ignore_fb) return true;
    LLViewerObject* vobj = facep ? facep->getViewerObject() : nullptr;
    return vobj && vobj->isHUDAttachment();
}

static void classifyCleanFace(LLFace* facep,
                              bool& out_alpha_blend,
                              bool& out_alpha_mask,
                              bool& out_fullbright,
                              U8* out_reason = nullptr)
{
    out_alpha_blend = false;
    out_alpha_mask  = false;
    out_fullbright  = false;
    U8 reason_sink = CAM_NONE;
    U8& reason = out_reason ? *out_reason : reason_sink;
    reason = CAM_NONE;

    const LLTextureEntry* te = facep ? facep->getTextureEntry() : nullptr;
    if (!te) return;

    LLGLTFMaterial* gltf_mat = te->getGLTFRenderMaterial();

    // AUTO-MASK DEMOTION. The static blend pass is WBOIT
    // -- correct for genuine translucency, WRONG for high-coverage/near-opaque
    // content (weighted mixing instead of occlusion: "half invisible" solids,
    // foliage rendering through foliage). Much content is set to BLEND that
    // belongs in MASK (opaque textures, binary-alpha leaves/flowers). Route any
    // blend face with ~opaque tint and a mask-like-or-opaque texture into the
    // MASK bucket: depth-write + cutoff discard = correct ordering, and a fully
    // opaque texture renders there identically to opaque -- so this covers both
    // the no-alpha-in-texture misroutes and lazy-creator foliage, with no
    // fragile "truly has no alpha" detector. Textures still loading (comps==0)
    // stay in blend; the dedup signature includes comps+mask so the face
    // re-classifies the moment analysis lands.
    // Returns the demotion reason (CAM_STRICT / CAM_GRADIENT / CAM_NOALPHA) or the
    // stay-blend reason (CAM_BLEND_*); maskable == reason < CAM_BLEND_TINT.
    auto tex_mask_reason = [&](LLViewerTexture* t) -> U8
    {
        if (!t) return CAM_BLEND_OTHER;
        // Content-driven: our texture store scanned the actual alpha channel at
        // upload. Compare the gradient fraction against the LIVE knob so the
        // mask/blend boundary is user-tunable (RenderCleanAutoMaskMaxGradient,
        // 0..1 fraction of pixels allowed to be partial-alpha). Fall back to
        // the component/flag heuristic until pixels have been seen.
        //
        // Either test demotes: LL's strict analysis (mIsMask, <~2% mid-range alpha)
        // or our looser gradient fraction. <SS:Skoomastream> The gradient test is OFF by
        // default (RenderCleanAutoMaskMaxGradient 0, alphaGradientClass never returns 1
        // then): it demoted soft-alpha light shafts / glass that LL keeps blended, and a
        // demoted face draws opaque wherever alpha > the cutoff. </SS:Skoomastream> They are computed from the same scan
        // (under VK, LLTextureData::vkPostAlphaAnalysis) and committed only from a
        // decent-resolution decode, sticky against later lower-resolution ones.
        // Only textures with a real alpha channel (2 / 4 components) are judged:
        // LL "analyses" a 1-component texture's luminance as alpha.
        const S32 c = (S32)t->getComponents();
        if (c == 3) return CAM_NOALPHA;
        if (c != 2 && c != 4) return CAM_BLEND_OTHER;
        if (c == 4 && t->getIsAlphaMask()) return CAM_STRICT;
        const S32 gclass = LLCleanTexture::instance().alphaGradientClass(t);
        if (gclass == 1) return CAM_GRADIENT;
        if (gclass == 0) return CAM_BLEND_GRADIENT;
        return CAM_BLEND_PENDING;
    };
    // Rigged / glowing faces are never demoted (as LL); computed lazily, only for
    // faces that reach a demotion test.
    S32 blocker = -1;
    auto blocked = [&]() -> U8
    {
        if (blocker < 0) blocker = cleanAutoMaskBlocker(facep, te);
        return (U8)blocker;
    };
    auto tex_maskable = [&](LLViewerTexture* t) -> bool
    {
        if (const U8 b = blocked()) { reason = b; return false; }
        reason = tex_mask_reason(t);
        // #561: rigged: binary / no-alpha only (the gradient demotion would alias soft alpha).
        if (reason == CAM_GRADIENT && cleanFaceIsRigged(facep)) reason = CAM_BLEND_RIGGED;
        return reason < CAM_BLEND_TINT;
    };

    // ---- GLTF material: it owns the alpha decision outright. ----
    if (gltf_mat)
    {
        // Never fullbright -- the PBR path is always lit.
        switch (gltf_mat->mAlphaMode)
        {
            case LLGLTFMaterial::ALPHA_MODE_BLEND:
                // <SS:Skoomastream> LL never auto-masks a glTF BLEND material; demoting
                // soft-alpha glTF faces drew them opaque wherever alpha > the cutoff.
                reason = CAM_BLEND_TINT;
                out_alpha_blend = true;
                break;
            case LLGLTFMaterial::ALPHA_MODE_MASK:  out_alpha_mask  = true; break;
            default: break; // ALPHA_MODE_OPAQUE -> opaque PBR
        }
        return;
    }

    // ---- Legacy / Blinn-Phong: reproduce genDrawInfo's alpha-pool split. ----
    // Fullbright follows the TextureEntry flag for legacy faces.
    out_fullbright = cleanEffectiveFullbright(facep, te);

    LLViewerObject* vobj   = facep->getViewerObject();
    const bool      hud    = vobj && vobj->isHUDAttachment();
    LLMaterial*     mat    = te->getMaterialParams().get();
    LLViewerTexture* tex   = facep->getTexture();

    // is_alpha exactly as genDrawInfo derives it: POOL_ALPHA membership
    // (getPoolTypeFromTE encapsulates the colour-alpha + texture-component +
    // material-alpha-mode test) OR-ed with the blinn-phong tint-transparency.
    bool is_alpha = (LLPipeline::getPoolTypeFromTE(te, tex) == LLDrawPool::POOL_ALPHA)
                    || (te->getColor().mV[3] < 0.999f);

    if (mat)
    {
        // Material (specmap/normmap) face. genDrawInfo: a BLEND-mode material
        // is always alpha; an is_alpha material is forced to BLEND mode; a
        // MASK-mode material that is NOT alpha goes to an *_ALPHA_MASK pass;
        // everything else is opaque (material/specmap/normmap/bump/simple).
        U8 mode = mat->getDiffuseAlphaMode();
        is_alpha = is_alpha || (mode == LLMaterial::DIFFUSE_ALPHA_MODE_BLEND);
        if (is_alpha)
        {
            reason = CAM_BLEND_TINT;
            if (te->getColor().mV[3] >= 0.999f
                && tex_maskable(tex))
                out_alpha_mask = true;   // auto-mask demotion (see above)
            else
                out_alpha_blend = true;
        }
        else if (mode == LLMaterial::DIFFUSE_ALPHA_MODE_MASK)
            out_alpha_mask = true;
        // else opaque
    }
    else if (is_alpha)
    {
        // Simple (no material) face in the alpha pool. canRenderAsMask() picks
        // mask vs blend (it already excludes rigged faces, BLEND-mode
        // materials, glowing faces, and non-mask textures).
        //
        // NOTE: LL's genDrawInfo excludes HUD faces from auto-mask (it relies
        // on depth-SORTED alpha-blend for HUDs). The clean HUD path does NOT
        // depth-sort, so a maskable HUD button left as alpha-blend either
        // punches a depth hole in the HUD background (depth-write on) or
        // flickers against its neighbours (depth-write off). Letting it be an
        // alpha-MASK instead -- discard transparent texels, write depth only
        // for the opaque symbol -- gives correct, stable HUD buttons with no
        // sort. So do NOT exclude HUDs here (canRenderAsMask already returns
        // the HUD-specific auto-mask setting for them).
        reason = CAM_BLEND_TINT;
        // (canRenderAsMask: LL's strict mIsMask -- binary alpha, safe for rigged too)
        if (!blocked() && facep->canRenderAsMask())
        {
            out_alpha_mask = true;
            reason = CAM_LL_MASK;
        }
        else if (te->getColor().mV[3] >= 0.999f && tex_maskable(tex))
            out_alpha_mask = true;   // content-driven demotion (plain BP faces --
                                     // the most common lazy-creator blend class)
        else
            out_alpha_blend = true;
    }
    // else: opaque simple/shiny/bump face.

}

// AUTO-MASK COUNTER (#82). Live tally of blend-candidate faces by the reason they were
// demoted to alpha-MASK or kept in the sorted alpha-BLEND pass, keyed by (object, face)
// and updated on each ingest (a face is only re-ingested when its signature changes,
// so the tally tracks what is actually rendered). Verbose-only: the map exists only
// while SlipstreamVerboseLogging is on, and the "VKAutoMask" tag is in the verbose-only
// tag list. Logs at most every 10 s, and only when something changed.
static void cleanAutoMaskNote(const LLUUID& obj_id, S32 face_idx, U8 reason)
{
    static LLCachedControl<bool> s_verbose(gSavedSettings, "SlipstreamVerboseLogging", false);
    static std::unordered_map<LLUUID, std::vector<U8>> s_faces;
    static S32 s_live[CAM_COUNT] = {};
    static U32 s_to_mask = 0, s_to_blend = 0, s_changes = 0;
    static F64 s_next_log = 0.0;
    if (!s_verbose)
    {
        if (!s_faces.empty())
        {
            s_faces.clear();
            for (S32& n : s_live) n = 0;
            s_to_mask = s_to_blend = s_changes = 0;
        }
        return;
    }
    if (face_idx < 0 || face_idx > 255) return;
    auto is_demoted = [](U8 r) { return r >= CAM_LL_MASK && r <= CAM_OPAQUE; };
    {
        std::vector<U8>& v = s_faces[obj_id];
        if ((S32)v.size() <= face_idx) v.resize(face_idx + 1, CAM_NONE);
        const U8 prev = v[face_idx];
        if (prev != reason)
        {
            --s_live[prev];
            ++s_live[reason];
            v[face_idx] = reason;
            ++s_changes;
            if (prev != CAM_NONE && reason != CAM_NONE)
            {
                if (!is_demoted(prev) && is_demoted(reason)) ++s_to_mask;
                if (is_demoted(prev) && !is_demoted(reason)) ++s_to_blend;
            }
        }
    }
    const F64 now = LLTimer::getElapsedSeconds();
    if (now < s_next_log || s_changes == 0) return;
    s_next_log = now + 10.0;
    // Drop faces of objects that are gone (once per log line, verbose only).
    for (auto it = s_faces.begin(); it != s_faces.end(); )
    {
        if (!gObjectList.findObject(it->first))
        {
            for (U8 r : it->second) --s_live[r];
            it = s_faces.erase(it);
        }
        else ++it;
    }
    const S32 demoted = s_live[CAM_LL_MASK] + s_live[CAM_STRICT] + s_live[CAM_GRADIENT]
                      + s_live[CAM_NOALPHA] + s_live[CAM_OPAQUE];
    const S32 blend = s_live[CAM_BLEND_TINT] + s_live[CAM_BLEND_PENDING] + s_live[CAM_BLEND_GRADIENT]
                    + s_live[CAM_BLEND_OTHER] + s_live[CAM_BLEND_RIGGED] + s_live[CAM_BLEND_GLOW]
                    + s_live[CAM_BLEND_SHINY];
    const S32 total = demoted + blend;
    static LLCachedControl<F32> s_max_grad(gSavedSettings, "RenderCleanAutoMaskMaxGradient", 0.f); // <SS:Skoomastream> default 0 = LL
    LL_INFOS("VKAutoMask") << "blend candidates=" << total
        << " demoted=" << demoted << " (" << (total ? (demoted * 100 / total) : 0) << "%)"
        << " [ll_mask=" << s_live[CAM_LL_MASK] << " strict=" << s_live[CAM_STRICT]
        << " gradient=" << s_live[CAM_GRADIENT] << " noalpha=" << s_live[CAM_NOALPHA]
        << " opaque=" << s_live[CAM_OPAQUE] << "]"
        << " stayed_blend=" << blend
        << " [tint=" << s_live[CAM_BLEND_TINT] << " pending=" << s_live[CAM_BLEND_PENDING]
        << " gradient=" << s_live[CAM_BLEND_GRADIENT] << " other=" << s_live[CAM_BLEND_OTHER]
        << " rigged=" << s_live[CAM_BLEND_RIGGED] << " glow=" << s_live[CAM_BLEND_GLOW]
        << " shiny=" << s_live[CAM_BLEND_SHINY] << "]"
        << " | since last: changes=" << s_changes << " blend->mask=" << s_to_mask
        << " mask->blend=" << s_to_blend
        << " | max_gradient=" << (F32)s_max_grad << LL_ENDL;
    s_changes = s_to_mask = s_to_blend = 0;
}

// Clean-pipeline-owned face ingestion. Faithful extraction of the clean-instance
// build that used to live inside registerFace, re-derived entirely from the face's
// object / volume / TextureEntry state -- NO spatial group, NO LL vertex buffer, NO
// LLRenderPass 'type'. A clean-owned sweep drives this on OUR schedule for any
// decoded object, so ingestion no longer depends on LL flagging a group GEOM_DIRTY.
// NEGATIVE CACHE for mesh decodes that yield no capturable face. A (mesh, lod,
// face) whose decode never produces a valid slot (the LOD lacks the face, or
// decodes empty) was re-decoded -- whole mesh, + tangents -- on every sweep visit,
// forever, eating the steady-state sweep budget. Back off ~10 s after a failure;
// cleared the moment a capture yields the slot.
static std::map<std::tuple<LLUUID, S32, S32>, U32>& cleanCaptureFailures()
{
    static std::map<std::tuple<LLUUID, S32, S32>, U32> s_fail;
    return s_fail;
}
static bool cleanCaptureBackoff(const LLUUID& mesh, S32 lod, S32 face)
{
    auto& m = cleanCaptureFailures();
    auto it = m.find(std::make_tuple(mesh, lod, face));
    return it != m.end() && (gFrameCount - it->second) < 600u;
}
// Drop entries past the 600-frame backoff (indistinguishable from absent: every reader
// treats an older entry as no backoff), so the map holds only the failures of the last
// window instead of every (mesh, lod, face) that ever failed. Hard cap as a backstop.
template <typename M>
static void cleanPruneFrameMap(M& m, U32& last_prune, size_t cap)
{
    const U32 now = (U32)gFrameCount;
    if ((now - last_prune) < 600u && m.size() < cap) return;
    last_prune = now;
    for (auto it = m.begin(); it != m.end(); )
    {
        if ((now - it->second) >= 600u) it = m.erase(it);
        else ++it;
    }
    if (m.size() >= cap) m.clear();
}
static void cleanCaptureResult(const LLUUID& mesh, S32 lod, S32 face, bool ok)
{
    auto key = std::make_tuple(mesh, lod, face);
    if (ok) { cleanCaptureFailures().erase(key); return; }
    static U32 s_prune = 0;
    cleanPruneFrameMap(cleanCaptureFailures(), s_prune, 131072u);
    cleanCaptureFailures()[key] = (U32)gFrameCount;
}
// Throttle for our own LOD fetches (gMeshRepo.loadMesh) per (mesh, lod): true = issue
// now (once per 600 frames). Expired entries are pruned (same semantics as absent).
static std::map<std::pair<LLUUID, S32>, U32>& cleanLodFetchTimes()
{
    static std::map<std::pair<LLUUID, S32>, U32> s_req;
    return s_req;
}
static bool cleanLodFetchThrottle(const LLUUID& mesh, S32 lod)
{
    auto& m = cleanLodFetchTimes();
    static U32 s_prune = 0;
    cleanPruneFrameMap(m, s_prune, 65536u);
    U32& last = m[std::make_pair(mesh, lod)];
    if (last == 0u || (gFrameCount - last) > 600u)
    {
        last = (U32)gFrameCount;
        return true;
    }
    return false;
}
// FLEXI KEYS: object -> its current scale-salted flexi geometry key. When the key
// changes (the prim was resized) or the object dies, the old key is handed to the mesh
// store's retire list, which frees that geometry once no live instance's LOD group
// names it (another flexi of identical shape + scale keeps it). Dead objects are reaped
// every 600 frames, so the map is bounded by the live flexi objects.
static std::unordered_map<LLUUID, LLUUID>& cleanFlexiKeys()
{
    static std::unordered_map<LLUUID, LLUUID> s_keys;
    return s_keys;
}
static void cleanNoteFlexiKey(const LLUUID& obj, const LLUUID& key)
{
    auto& m = cleanFlexiKeys();
    static U32 s_prune = 0;
    if (((U32)gFrameCount - s_prune) >= 600u)
    {
        s_prune = (U32)gFrameCount;
        for (auto it = m.begin(); it != m.end(); )
        {
            if (!gObjectList.findObject(it->first))
            {
                LLCleanMeshAsset::instance().retireAssetWhenUnused(it->second);
                it = m.erase(it);
            }
            else ++it;
        }
    }
    LLUUID& cur = m[obj];
    if (cur == key) return;
    if (cur.notNull()) LLCleanMeshAsset::instance().retireAssetWhenUnused(cur);
    cur = key;
}
void llvkCleanIngestCacheStats(std::ostream& os)
{
    os << "ingest caches: capture-backoff " << cleanCaptureFailures().size()
       << " lod-fetch-throttle " << cleanLodFetchTimes().size()
       << " flexi-keys " << cleanFlexiKeys().size();
}
// VKIngestDiag: is (mesh, lod, face) negative-cached, and since how many frames.
bool cleanCaptureBackoffInfo(const LLUUID& mesh, S32 lod, S32 face, U32& age_frames)
{
    auto& m = cleanCaptureFailures();
    auto it = m.find(std::make_tuple(mesh, lod, face));
    if (it == m.end()) return false;
    age_frames = (U32)gFrameCount - it->second;
    return age_frames < 600u;
}

const char* LLFace::vkIngestName(U8 o)
{
    static const char* k[] = { "never_visited", "no_volume", "face_beyond_volume", "decode_backoff",
                               "decode_no_bytes", "decode_pending", "decode_latched_face_absent",
                               "decode_failed", "lod_lacks_face", "capture_no_slot", "no_slot",
                               "ingested", "up_to_date", "waiting_gpu_desired_lod",
                               "not_drawn_invisiprim", "capture_pending_worker" };
    return o < VKI_COUNT ? k[o] : "?";
}

// PBR BOM: any glTF material texture slot may hold an IMG_USE_BAKED_* sentinel (the
// body's base colour = the avatar's bake; normal / ORM maps routed through spare bake
// channels). Unresolved, the sentinel's own asset -- an image of placeholder TEXT --
// was sampled, e.g. as the normal map: text embossed over the skin (#192). The sentinel is not an asset -- streaming it
// leaves the face on the placeholder forever -- and LL resolves it into the SHARED
// material (mBaseColorTexture), so avatars wearing the same material fight over it.
// Resolve it per face against the face's own object's avatar instead.
static LLUUID vkResolveBakeId(LLViewerObject* obj, const LLUUID& id)
{
    if (obj && LLAvatarAppearanceDefines::LLAvatarAppearanceDictionary::isBakedImageId(id))
    {
        if (LLViewerTexture* bake = obj->getBakedTextureForMagicId(id))
            return bake->getID();
    }
    return id;
}

// Prim geometry source for the Vulkan ingest: tessellated from the shape params. A
// flexi (#25) is tessellated in its straight REST pose with one chain joint per
// vertex (see LLVolumeImplFlexible::vkRestPoints); everything else as LL does.
static LLPointer<LLVolume> vkTessellatePrim(LLViewerObject* pObj, S32 face_idx, const LLVolumeParams& params,
                                            F32 detail, bool vk_flexi, const LLVector3& flexi_scale)
{
    LLPointer<LLVolume> src = new LLVolume(params, detail);
    if (vk_flexi)
    {
        // Rest pose: the straight chain (LLDynamicPath keeps points it's given),
        // then one joint per vertex -- every vertex lies on exactly one path row
        // (scale is applied before rotation per point), weight 1.
        LLVolumeImplFlexible::VKRestPoint rest[LLVolumeImplFlexible::VK_FLEXI_POINTS];
        LLVolumeImplFlexible::vkRestPoints(params, flexi_scale, rest);
        LLPath& path = src->getPath();
        path.resizePath(LLVolumeImplFlexible::VK_FLEXI_POINTS);
        for (S32 i = 0; i < LLVolumeImplFlexible::VK_FLEXI_POINTS; ++i)
        {
            LLPath::PathPt& pt = path.mPath[i];
            pt.mPos.load3(rest[i].pos.mV);
            pt.mRot.loadu(LLMatrix3(rest[i].rot));
            pt.mScale.set(rest[i].scale.mV[0], rest[i].scale.mV[1], 0.f, 1.f);
            pt.mTexT = (F32)i / (F32)(LLVolumeImplFlexible::VK_FLEXI_POINTS - 1);
        }
        src->regen();
        const F32 sz = llmax(flexi_scale.mV[VZ], 0.0001f);
        {
            F32 zmin = 1e9f, zmax = -1e9f; S32 nv = 0;
            for (S32 f = 0; f < src->getNumVolumeFaces(); ++f)
            {
                const LLVolumeFace& vf = src->getVolumeFace(f);
                nv += vf.mNumVertices;
                for (S32 v = 0; v < vf.mNumVertices; ++v)
                {
                    const F32 z = vf.mPositions[v].getF32ptr()[2];
                    zmin = llmin(zmin, z); zmax = llmax(zmax, z);
                }
            }
            LL_INFOS("VKFlexi") << "rest capture " << pObj->getID() << " face " << face_idx << ": path pts "
                        << src->getPath().getPathLength() << ", " << src->getNumVolumeFaces()
                        << " faces, " << nv << " verts, z [" << zmin << ", " << zmax << "], scale "
                        << flexi_scale << LL_ENDL;
        }
        const F32 step = sz / (F32)(LLVolumeImplFlexible::VK_FLEXI_POINTS - 1);
        for (S32 f = 0; f < src->getNumVolumeFaces(); ++f)
        {
            LLVolumeFace& vf = src->getVolumeFace(f);
            vf.allocateWeights(vf.mNumVertices);
            for (S32 v = 0; v < vf.mNumVertices; ++v)
            {
                const F32 z = vf.mPositions[v].getF32ptr()[2];
                const S32 j = llclamp((S32)ll_round((z + 0.5f * sz) / step), 0,
                              LLVolumeImplFlexible::VK_FLEXI_POINTS - 1);
                // LL weight4: integer part = joint, fraction = weight.
                vf.mWeights[v].set((F32)j + 0.999f, 0.f, 0.f, 0.f);
            }
        }
    }
    const S32 nf = src->getNumVolumeFaces();
    for (S32 f = 0; f < nf; ++f) src->genTangents(f);
    return src;
}

// INGEST WORKER (RenderVKIngestWorker). Prim tessellation + tangents and the capture
// prepare (interleave / bounds / index checks) of async-decoded meshes run on the
// CleanCapturePrep workers; the render thread commits at the ingest sync point
// (vkPumpAsyncCaptures, start of the sweep) and re-ingests the waiting objects.
// Flexi prims, sculpts (LL's live volume) and local meshes stay synchronous.
static bool vkIngestWorkerOn()
{
    static LLCachedControl<bool> s_on(gSavedSettings, "RenderVKIngestWorker", true);
    return s_on;
}

// static
void LLCleanIngest::vkPumpAsyncCaptures(std::vector<LLUUID>& wake)
{
    static std::vector<LLCleanMeshAsset::CaptureDone> s_done;
    s_done.clear();
    LLCleanMeshAsset& ma = LLCleanMeshAsset::instance();
    ma.pumpCaptures(s_done);
    for (const LLCleanMeshAsset::CaptureDone& d : s_done)
    {
        for (const LLCleanMeshAsset::CaptureWaiter& w : d.waiters)
        {
            if (w.bookkeep)
            {
                // The synchronous path's post-capture bookkeeping: clear / set the
                // mesh face's negative cache from whether it got a slot.
                const bool ok = ma.getSlot(d.mesh_id, d.lod, w.face_idx).valid;
                cleanCaptureResult(d.mesh_id, d.lod, w.face_idx, ok);
                if (ok && w.site == 2) LLCleanMeshAsset::sLodFetchCap.fetch_add(1, std::memory_order_relaxed);
            }
            wake.push_back(w.obj);
        }
    }
    LLVKHitch::count("captureInFlight", ma.capturesInFlight());
}

// static
// MASK CUTOFF CEILING (#840): every SL texture is lossy JPEG2000, and its alpha comes back
// with +-1..8 levels of codec noise. A mask cutoff of 255 (common: "only fully opaque")
// then discards every interior texel decoded as 254 and below -- under magnification each
// one becomes a half-texel hole, and the cutout's curved edges turn ragged (a trading
// card's rounded corners, measured: 93 texels at 254, a few down to 240, all inside the
// shape). LL's GL path renders the same holes. Cutoffs above 247.5 / 255 test at 247.5 / 255:
// the noise passes, while real anti-aliased rims (well below) still cut where they did.
static constexpr F32 kMaskCutoffCeiling = 247.5f / 255.f;

// <GMDX> ...except on a gradient-alpha texture (half its texels or more partly transparent: a
// dissolve map). A cutoff there above the ceiling is a script fading the face out, and the
// ceiling kept its most opaque parts alive (a staff's "dissolve" left bright shards that GL
// hides). Those test the cutoff as LL does; the shaders give cutoffs above the ceiling no mip
// coverage boost either (clean_frame_info.glsl maskTestAlpha). Not analysed yet: the ceiling.
static F32 clean_mask_cutoff(F32 cutoff, const LLViewerTexture* tex)
{
    if (cutoff <= kMaskCutoffCeiling) return cutoff;
    const U32 pct = tex ? LLCleanTexture::instance().alphaGradientPct(tex) : 255u;
    return (pct != 255u && pct >= 50u) ? cutoff : kMaskCutoffCeiling;
}

void LLCleanIngest::registerCleanFace(LLFace* facep, bool force_reingest)
{
    if (!facep) return;
    LLViewerObject* pObj = facep->getViewerObject();
    LLDrawable* drawable = facep->getDrawable();
    if (!pObj || !drawable) return;

    // LL's CURRENT volume -- transient: LL may LOD-unload (mesh/sculpt) or reuse
    // (prim) it out from under our async capture. Used only to read the asset
    // identity (params/detail) and the face count; the actual capture reads from a
    // volume WE keep resident (acquired below).
    LLVolume* live = pObj->getVolume();
    facep->mVKIngestFrame = (U32)gFrameCount;
    if (!live) { facep->mVKIngest = LLFace::VKI_NO_VOLUME; return; }
    // GLTF SCENE ASSET (#39): drawn from its asset by LLCleanGLTFIngest, whose instances
    // share this object's face-index namespace -- never register placeholder faces.
    if (LLCleanGLTFIngest::isGLTFScene(pObj))
    {
        facep->mVKIngest = LLFace::VKI_NOT_DRAWN;
        return;
    }
    // #Slipstream Helper (#500): an invisible bridge HUD -- never drawn, whatever state
    // its stored copy is in (a fresh library rock would sit in front of the camera).
    if (pObj->isAttachment() && SLBridge::instanceExists() && SLBridge::instance().isHelperObject(pObj))
    {
        LLCleanInstanceStore::instance().releaseObject(pObj->getID());
        facep->mVKIngest = LLFace::VKI_NOT_DRAWN;
        return;
    }

    const S32 face_idx = facep->getTEOffset();
    if (face_idx < 0 || face_idx >= live->getNumVolumeFaces())
    {
        facep->mVKIngest = LLFace::VKI_FACE_BEYOND_VOLUME;
        return;
    }
    U8 vk_outcome = LLFace::VKI_NO_SLOT;   // refined below; published before any return

    // GLTF MATERIAL FETCH (VK). LL initiates the GLTF material ASSET fetch and applies
    // per-face material overrides from LLVolumeGeometryManager::rebuildGeom (this file,
    // ~7440), which VK never runs -- the clean pipeline owns ingest and display() (hence
    // rebuildGeom) is diverted. Without this, cache-loaded PBR faces never fetch their
    // material asset, te->getGLTFRenderMaterial() stays null, and the material desc below
    // falls back to the legacy Blinn-Phong texture (te->getID()). Drive the same two calls
    // LL does per rebuilt drawable so the fetch initiates here; the material resolves over
    // subsequent selfRegisterSweep re-ingests and the desc then reads the real PBR
    // base-color. Both are idempotent/self-clearing.
    pObj->updateTEMaterialTextures((U8)face_idx);
    gGLTFMaterialList.applyQueuedOverrides(pObj);

    // FACE DIFFUSE TEXTURE SYNC. LL copies an object's TE image onto its LLFace
    // (facep->setTexture(getTEImage)) only in the rebuild path (updateFaceCount via
    // updateGeometry, reached from markTextured -> updateMove -> REBUILD_MATERIAL),
    // which VK never runs. So a face kept whatever texture it had at creation:
    // retextured objects streamed the OLD texture, and BOM faces stayed on the gray
    // IMG_DEFAULT that getBakedTextureForMagicId hands out before the avatar's bake
    // arrives (gray BOM body parts). Sync it here from the authoritative source; for
    // BOM sentinels resolve the avatar's bake LIVE rather than trusting mTEImages
    // (only refreshed when LLVOAvatar::updateMeshTextures runs). Media faces keep
    // their media texture.
    if (!facep->hasMedia())
    {
        const LLTextureEntry* sync_te = facep->getTextureEntry();
        LLViewerTexture* want = nullptr;
        if (sync_te && LLAvatarAppearanceDefines::LLAvatarAppearanceDictionary::isBakedImageId(sync_te->getID()))
            want = pObj->getBakedTextureForMagicId(sync_te->getID());
        if (!want)
            want = pObj->getTEImage((U8)face_idx);
        if (want && facep->getTexture() != want)
            facep->setTexture(want);
    }

    // PBR TEXTURE STREAMING. LL tells the texture streamer about a PBR face's
    // textures by assigning them to the face's PBR channels -- in rebuildGeom
    // (~7925), which VK never runs. LLVOVolume::updateTextures() drives each
    // face's virtual size (fetch/decode priority) from those channels, so with
    // them empty every PBR texture got no priority: never fetched, never
    // sharpened. Mirror rebuildGeom's assignment (and, like it, drop the
    // Blinn-Phong channels so they stop streaming for a PBR face).
    const LLTextureEntry* pbr_te = facep->getTextureEntry();
    LLFetchedGLTFMaterial* pbr_mat =
        pbr_te ? (LLFetchedGLTFMaterial*)pbr_te->getGLTFRenderMaterial() : nullptr;
    if (pbr_mat)
    {
        if (!facep->hasMedia())
        {
            facep->setTexture(LLRender::DIFFUSE_MAP, nullptr);
        }
        facep->setTexture(LLRender::NORMAL_MAP, nullptr);
        facep->setTexture(LLRender::SPECULAR_MAP, nullptr);
        facep->setTexture(LLRender::BASECOLOR_MAP, pbr_mat->mBaseColorTexture);
        facep->setTexture(LLRender::GLTF_NORMAL_MAP, pbr_mat->mNormalTexture);
        facep->setTexture(LLRender::METALLIC_ROUGHNESS_MAP, pbr_mat->mMetallicRoughnessTexture);
        facep->setTexture(LLRender::EMISSIVE_MAP, pbr_mat->mEmissiveTexture);

    }

    const LLTextureEntry* te = facep->getTextureEntry();

    // RIGGED classification, decided HERE from the object -- the same criteria LL's
    // LLVolumeGeometryManager::rebuildGeom uses (skinned mesh that is either worn as an
    // attachment, or an animated object whose control avatar is playing). LL only
    // records the result as face state (LLFace::RIGGED) inside rebuildGeom, which the
    // Vulkan frame never runs, so reading the face flag left every rigged face
    // classified as static: no skin, no rigged flag, zero rigged draws -- and rigged
    // ATTACHMENTS fell into the rigid-attachment path, drawing bind-pose geometry
    // relative to the attach joint.
    bool rigged = false;
    if (LLVOVolume* rvol = drawable->getVOVolume())
    {
        if (rvol->isMesh() && rvol->getSkinInfo())
        {
            if (rvol->isAnimatedObject())
            {
                LLControlAvatar* cav = rvol->getControlAvatar();
                rigged = rvol->isRiggedMesh() && cav && cav->mPlaying;
            }
            else
            {
                rigged = rvol->isAttachment();
            }
        }

        // JOINT OVERRIDES (deformers / mesh bodies with joint positions). LL applies a
        // rigged mesh's joint position/scale overrides to its avatar skeleton from
        // notifyMeshLoaded -- which bails (addAttachmentOverridesForObject returns
        // early) if the mesh asset isn't loaded yet at that moment -- and again from
        // rebuildGeom on every rebuild, the safety net. VK never runs rebuildGeom, so a
        // deformer whose skin info landed before its mesh stayed unapplied: body left
        // undeformed, intermittently. Re-assert it on each sweep visit (once per
        // object); it skips cheaply once the mesh's overrides are active.
        if (rigged && face_idx == 0)
        {
            LLVOAvatar* oav = rvol->isAnimatedObject() ? rvol->getControlAvatar() : rvol->getAvatar();
            if (oav)
            {
                oav->addAttachmentOverridesForObject(rvol, nullptr, false);
            }
        }
    }

    // INVISIPRIM / WATER EXCLUSION SURFACE (#27). LL's rebuildGeom files a face whose
    // texture is one of the GL_ALPHA-format preloads (IMG_ALPHA_GRAD / _2D) -- with no
    // glTF or legacy material and an opaque tint (not in the alpha pool) -- under
    // PASS_INVISIBLE, and only when the object is neither worn nor rigged; otherwise it
    // registers nothing. PASS_INVISIBLE draws nowhere visible: only into the water
    // exclusion mask, which cuts the water surface out where the prim covers it (boat
    // hulls). Here: instance flag bit 16 routes it to the post-water alpha set, where
    // the exclusion-mask pass rasterises it and every visible FS discards it.
    bool water_exclusion = false;
    if (te && !te->getGLTFRenderMaterial() && !te->getMaterialParams().get()
        && te->getColor().mV[3] >= 0.999f
        && (te->getID() == IMG_ALPHA_GRAD || te->getID() == IMG_ALPHA_GRAD_2D))
    {
        if (pObj->isAttachment() || pObj->isRiggedMesh())
        {
            // LL draws nothing: drop any instance left from before a retexture.
            LLCleanInstanceStore::instance().release(pObj->getID(), face_idx);
            facep->mVKIngest = LLFace::VKI_NOT_DRAWN;
            return;
        }
        water_exclusion = true;
    }

    LLVOVolume* cvobj = drawable->getVOVolume();

    // SELECTION DIAGNOSTIC (sim-surround dark-link hunt): when a link is selected
    // in build/edit, dump its per-face material/tint so a known-good (bright) link
    // can be diffed against a known-bad (dark) one. Throttled per object. TEMP.
    if (CLEAN_DEBUG && te && pObj->isSelected())
    {
        static std::map<U32, U32> s_selLog;
        const U32 lid = pObj->getLocalID();
        U32& last = s_selLog[lid];
        if (last == 0u || (gFrameCount - last) > 90u)
        {
            last = gFrameCount;
            const LLColor4 c = te->getColor();
            LLGLTFMaterial* gm = te->getGLTFRenderMaterial();
            const LLMaterial* lm = te->getMaterialParams().get();
            bool ab=false, am=false, fb=false;
            classifyCleanFace(facep, ab, am, fb);
            const S32 pool = (S32)LLPipeline::getPoolTypeFromTE(te, facep->getTexture());
            LL_INFOS("CleanSurround") << "SEL obj=" << lid << " face=" << face_idx
                << " tint=(" << c.mV[0] << "," << c.mV[1] << "," << c.mV[2] << "," << c.mV[3] << ")"
                << " fullbright=" << (te->getFullbright() ? 1 : 0)
                << " glow=" << te->getGlow()
                << " shiny=" << (S32)te->getShiny()
                << " bump=" << (S32)te->getBumpmap()
                << " texgen=" << (S32)te->getTexGen()
                << " gltf=" << (gm ? 1 : 0)
                << " legacy=" << (lm ? 1 : 0)
                << " env=" << (lm ? (S32)lm->getEnvironmentIntensity() : -1)
                << " glossExp=" << (lm ? (S32)lm->getSpecularLightExponent() : -1)
                << " diffMode=" << (lm ? (S32)lm->getDiffuseAlphaMode() : -1)
                << " | CLASSIFY blend=" << (ab?1:0) << " mask=" << (am?1:0)
                << " fullbright=" << (fb?1:0) << " poolType=" << pool
                << " (POOL_ALPHA=" << (S32)LLDrawPool::POOL_ALPHA << ")"
                << LL_ENDL;
        }
    }

    // ---- mesh_id / mesh_lod (mirrors registerFace 5649-5696). ----
    const S32 mesh_lod = LLVolumeLODGroup::getVolumeDetailFromScale(live->getDetail());
    const bool is_prim = live->getParams().getSculptID().isNull();   // pure prim: no asset, shape-param keyed
    // Local mesh preview (Local Assets): no downloaded bytes to decode -- the mesh
    // repository copies the file's geometry into LL's live volume, the same source
    // sculpts use, and a live reload mints fresh mesh ids (so fresh slots). Every
    // detail level is the same geometry.
    const bool is_local_mesh = !is_prim && pObj->isMesh() && LLLocalMeshMgr::instanceExists()
                            && LLLocalMeshMgr::getInstance()->isLocal(live->getParams().getSculptID());
    // Canonical geometry key -- shared with every consumer (selection overlay etc.)
    // via LLCleanMeshAsset::geometryKey so lookups can never diverge from ingest
    // (prims: shape hash; sculpts: map id salted by sculpt type, #44).
    LLUUID mesh_id = LLCleanMeshAsset::geometryKey(live->getParams());
    // FLEXI (#25): drawn as a 9-joint chain through the rigged pre-skin path. Its
    // geometry is tessellated in a straight rest pose with the object scale baked in
    // (LL's flexi path frame is unscaled; the scale lives in the path points), so the
    // slot is keyed by shape AND object scale.
    const bool vk_flexi = is_prim && cvobj && cvobj->vkFlexiImpl() != nullptr;
    if (cvobj && cvobj->isFlexible() && face_idx == 0)
    {
        static std::set<LLUUID> s_logged;
        if (s_logged.size() >= 1024) s_logged.clear();   // log-once set: bounded
        if (s_logged.insert(pObj->getID()).second)
            LL_INFOS("VKFlexi") << "flexi object " << pObj->getID() << " is_prim=" << is_prim
                                << " impl=" << (cvobj->vkFlexiImpl() ? 1 : 0) << " => chain=" << vk_flexi << LL_ENDL;
    }
    LLVector3 flexi_scale;
    if (vk_flexi)
    {
        flexi_scale = cvobj->getScale();
        LLUUID salt;
        salt.generate(llformat("flexi %.4f %.4f %.4f", flexi_scale.mV[0], flexi_scale.mV[1], flexi_scale.mV[2]));
        LLUUID keyed;
        mesh_id.combine(salt, keyed);
        mesh_id = keyed;
        // Resizing a flexi mints a new key (and slots); retire the object's previous one
        // so its geometry goes as soon as nothing references it -- not only once the
        // geometry budget forces a GC.
        cleanNoteFlexiKey(pObj->getID(), mesh_id);
    }

    // ---- OWN THE GEOMETRY. We decode into the mega-VBO from a geometry source WE
    // control, transient -- discarded the instant capture() finishes, so the geometry
    // lives ONLY in our buffer, never in an LL object whose lifetime LL controls
    // (the #29 vanish was LL unloading its LLVolume before our async re-read). Decode
    // only when we actually (re)capture; the LIVE volume is used for the cheap
    // "do we need to capture?" checks (no decode just to decide). ----
    LLCleanMeshAsset::Slot exist =
        LLCleanMeshAsset::instance().getSlot(mesh_id, mesh_lod, face_idx);

    bool need_capture = !exist.valid;
    // VRAM LADDER TRIM (#405 mesh detail / far geometry): LL's live level (mesh_lod) comes
    // from LL's calcLOD, which under VK reads stale drawable state -- typically the level
    // picked before the rung lowered the effective LOD factor. The pressure GC keeps only
    // the levels the cull can select at the EFFECTIVE factor (+1 margin,
    // LLCleanInstanceStore::collectInUseMeshSlotsLod), so capturing LL's level here
    // re-captured exactly what the GC had just freed, every pass (freed ~13 MB, resident
    // back up before the next GC). While trimming, skip LL's level when it is above that
    // margin and the face's group already draws something; the converge block below
    // captures the level the cull selects. Same exclusions as the in-use set (rigged,
    // HUD / worn attachments, flexi chains, sculpts: they keep every level).
    if (need_capture && (is_prim || pObj->isMesh()) && !is_local_mesh && !rigged && !vk_flexi
        && !pObj->isAttachment() && LLCleanMeshAsset::instance().vramPressureTrim())
    {
        const F32 radius = live->mLODScaleBias.scaledVec(cvobj ? cvobj->getScale() : pObj->getScale()).length();
        const F32 dist = llmax((pObj->getRenderPosition() - LLViewerCamera::getInstance()->getOrigin()).length(), 0.001f);
        const F32 tan_angle = ll_round((LLVOVolume::sLODFactor * radius) / dist, 0.01f);
        const S32 sel = (tan_angle <= 0.03f) ? 0 : (tan_angle <= 0.06f) ? 1 : (tan_angle <= 0.24f) ? 2 : 3;
        if (mesh_lod > sel + 1
            && LLCleanMeshAsset::instance().groupBestSlot(mesh_id, face_idx) != 0xFFFFFFFFu)
        {
            need_capture = false;
            LLVKHitch::count("trimSkipLLLevel");
        }
    }
    // Re-capture ONLY for sculpts whose decode level improved. Mesh never re-captures
    // (a (mesh_id, lod) maps to immutable asset bytes; a finer LOD is a different
    // mesh_lod key). Prims are tessellated deterministically from params, and a
    // reshape changes the shape-hash key -> a fresh capture; so a prim's slot is
    // never stale. (An earlier live-vs-captured vertex-count check forced EVERY prim
    // to re-tessellate every sweep cycle -- our generated vert count legitimately
    // differs from LL's live volume -- which collapsed FPS. Removed.)
    if (exist.valid && !pObj->isMesh() && !is_prim)
    {
        if (live->getSculptLevel() >= 0
            && (exist.sculpt_level < 0 || live->getSculptLevel() < exist.sculpt_level))
        {
            need_capture = true; // sculpt decode improved
        }
    }

    // Async-capable source for this face (see vkIngestWorkerOn): non-flexi prims and
    // async-decoded meshes. One job per (mesh_id, lod); later requesters join it.
    const bool async_src = vkIngestWorkerOn() && (is_prim ? !vk_flexi : (pObj->isMesh() && !is_local_mesh));
    if (need_capture && async_src)
    {
        LLCleanMeshAsset::CaptureWaiter w;
        w.obj = pObj->getID(); w.face_idx = face_idx; w.site = 1; w.bookkeep = !is_prim;
        if (LLCleanMeshAsset::instance().captureInFlight(mesh_id, mesh_lod, &w))
        {
            need_capture = false;
            vk_outcome = LLFace::VKI_CAPTURE_PENDING;
        }
    }
    if (need_capture)
    {
        LLPointer<LLVolume> src;   // OUR transient geometry source (auto-freed here)
        LLVolume* cap = nullptr;
        LLCleanMeshAsset::CaptureWaiter aw;
        aw.obj = pObj->getID(); aw.face_idx = face_idx; aw.site = 1; aw.bookkeep = !is_prim;
        if (is_prim && async_src
            && LLCleanMeshAsset::instance().postCapture(mesh_id, mesh_lod, nullptr, live->getParams(),
                                                        live->getDetail(), aw))
        {
            // Prim: tessellated + captured on the ingest worker; committed and this
            // object re-ingested at the next sync point.
            vk_outcome = LLFace::VKI_CAPTURE_PENDING;
        }
        else if (is_prim)
        {
            // Prim: tessellate from the shape params -- core geometry we invoke, our
            // instance, dropped after capture.
            VK_HITCH_SCOPE("sweep_tess");
            src = vkTessellatePrim(pObj, face_idx, live->getParams(), live->getDetail(), vk_flexi, flexi_scale);
            cap = src.get();
        }
        else if (is_local_mesh)
        {
            if (live->getNumVolumeFaces() > 0) cap = live;
        }
        else if (pObj->isMesh())
        {
            // Mesh: decode OUR retained raw LOD bytes ourselves (LLCleanMeshAsset::
            // retainRawMesh captured them at download). unpackVolumeFaces is the core
            // decode we invoke; the resulting volume is ours and discarded. If the
            // bytes haven't arrived yet, skip -- the sweep retries once they land.
            //
            // NEGATIVE CACHE: a (mesh, lod, face) whose decode produced no capturable
            // face (the LOD has fewer faces than the drawable, or decodes empty) never
            // gets a slot -- without this every sweep visit re-decoded the WHOLE mesh
            // (+ tangents) per face, forever, eating the steady-state sweep budget.
            //
            // ASYNC DECODE: the decode itself (inflate + parse + MikkTSpace + meshopt)
            // runs on the CleanMeshDecode pool (or is adopted from LL's own decode of the
            // same bytes); this visit either takes a finished volume or queues the
            // decode and returns -- the face renders nothing until the decode lands,
            // exactly as while its bytes were downloading. The object is re-ingested
            // (noteObjectChanged) as soon as its decode completes.
            if (cleanCaptureBackoff(mesh_id, mesh_lod, face_idx))
            {
                // Backed off because the asset is fully captured without this face
                // (legitimately empty in this LOD) = settled, not waiting.
                vk_outcome = LLCleanMeshAsset::instance().isLatched(mesh_id, mesh_lod)
                           ? LLFace::VKI_DECODE_LATCHED : LLFace::VKI_BACKOFF;
            }
            else
            {
                LLPointer<LLVolume> dec;
                const LLCleanMeshAsset::DecodeState ds = LLCleanMeshAsset::instance().takeDecoded(
                    mesh_id, mesh_lod, live->getParams(), face_idx, /*site*/1, cleanIsSelfPriority(pObj),
                    pObj->getID(), dec);
                if (ds == LLCleanMeshAsset::DECODE_READY && dec.notNull())
                {
                    LLVKHitch::count("meshDecodeTaken");
                    src = dec;
                    const bool has_face = src->getNumVolumeFaces() > face_idx;
                    // Async: the decoded volume is ours and immutable; the worker reads
                    // it (the job holds the reference). A face this LOD lacks is
                    // negative-cached now, as before; the job still captures the others.
                    if (!has_face) aw.bookkeep = false;
                    if (async_src && !exist.valid
                        && LLCleanMeshAsset::instance().postCapture(mesh_id, mesh_lod, src, live->getParams(), 0.f, aw))
                    {
                        if (has_face) vk_outcome = LLFace::VKI_CAPTURE_PENDING;
                        else
                        {
                            cleanCaptureResult(mesh_id, mesh_lod, face_idx, false);
                            vk_outcome = LLFace::VKI_LOD_LACKS_FACE;
                        }
                    }
                    else if (has_face)
                    {
                        cap = src.get();
                    }
                    else
                    {
                        // This LOD lacks our face. The volume is one-shot: still capture
                        // the faces it does have for the object's other faces.
                        { VK_HITCH_SCOPE("meshCapture"); LLCleanMeshAsset::instance().capture(mesh_id, mesh_lod, src.get(), /*force=*/exist.valid); }
                        cleanCaptureResult(mesh_id, mesh_lod, face_idx, false);
                        vk_outcome = LLFace::VKI_LOD_LACKS_FACE;
                    }
                }
                else if (ds == LLCleanMeshAsset::DECODE_FAILED)
                {
                    cleanCaptureResult(mesh_id, mesh_lod, face_idx, false);
                    vk_outcome = LLCleanMeshAsset::instance().isLatched(mesh_id, mesh_lod)
                               ? LLFace::VKI_DECODE_LATCHED : LLFace::VKI_DECODE_FAILED;
                }
                else if (ds == LLCleanMeshAsset::DECODE_PENDING)
                {
                    vk_outcome = LLFace::VKI_DECODE_PENDING;
                }
                else
                {
                    vk_outcome = LLFace::VKI_DECODE_NO_BYTES;
                    // The raw-byte LRU (RenderVKRawMeshCacheMB) evicted this LOD's bytes. LL
                    // may still hold the LOD loaded and would never re-deliver it; ask the
                    // repo again (its receive path re-retains the bytes; usually a disk-cache
                    // hit). Only for evicted keys: a first download is LL's own request.
                    if (cvobj && LLCleanMeshAsset::instance().wasRawMeshEvicted(mesh_id, mesh_lod)
                        && cleanLodFetchThrottle(mesh_id, mesh_lod))
                    {
                        LLCleanMeshAsset::sLodFetchReq.fetch_add(1, std::memory_order_relaxed);
                        gMeshRepo.loadMesh(cvobj, live->getParams(), mesh_lod);
                    }
                }
            }
        }
        else
        {
            cap = live; // sculpt: LL's live volume (refines as the sculpt texture decodes)
        }

        if (cap && face_idx < cap->getNumVolumeFaces())
        {
            // ATOMIC RE-CAPTURE (#29): when a slot already exists (sculpt decode
            // improved), force=true re-captures in place -- new geometry to fresh
            // ranges, SlotInfo overwritten per face, old ranges freed AFTER. The old
            // invalidateAsset() zeroed every slot up front, so any face the refill
            // skipped (still mid-decode/degenerate) left live instances stranded at
            // index_count==0 forever (the emitted_empty stuck-count #29 signature).
            { VK_HITCH_SCOPE("meshCapture"); LLCleanMeshAsset::instance().capture(mesh_id, mesh_lod, cap, /*force=*/exist.valid); }
            exist = LLCleanMeshAsset::instance().getSlot(mesh_id, mesh_lod, face_idx);
            if (pObj->isMesh()) cleanCaptureResult(mesh_id, mesh_lod, face_idx, exist.valid);
            if (!exist.valid) vk_outcome = LLFace::VKI_CAPTURE_NO_SLOT;
        }
    }

    // CLUSTER-OWNED MIRROR RE-MATERIALIZATION (LLCleanMeshAsset::requestMirror). A slot of this
    // face whose CPU mirror was freed (the cluster path owned it, its DAG on disk) is wanted
    // again -- by legacy (the mode flipped off / a legacy row) or by a DAG build. Its level is
    // usually NOT LL's current one (rigged volumes load the cluster source LOD 3; after a flip
    // off calcLOD is back to the screen-size LOD), so it is re-decoded here at the WANTED level:
    // our raw bytes if still retained, else LL's mesh repository again (disk cache; the receive
    // path re-retains + adopts the bytes). The commit re-captures the latched asset in place.
    // Async throughout (decode pool, capture worker); the slot has no mirror until it lands.
    if (pObj->isMesh() && !is_local_mesh && cvobj && LLCleanMeshAsset::instance().rematPending())
    {
        S32 rl = -1;
        LLCleanMeshAsset& rms = LLCleanMeshAsset::instance();
        if (rms.rematWantedFace(mesh_id, face_idx, rl) && rl >= 0)
        {
            LLCleanMeshAsset::CaptureWaiter rw;
            rw.obj = pObj->getID(); rw.face_idx = face_idx; rw.site = 3; rw.bookkeep = false;
            if (!rms.captureInFlight(mesh_id, rl, &rw))   // (in flight: its commit is forced)
            {
                LLPointer<LLVolume> rdec;
                const LLCleanMeshAsset::DecodeState rds = rms.takeDecoded(
                    mesh_id, rl, live->getParams(), face_idx, /*site*/4, cleanIsSelfPriority(pObj), pObj->getID(), rdec);
                if (rds == LLCleanMeshAsset::DECODE_READY && rdec.notNull())
                {
                    LLVKHitch::count("rematDecodeTaken");
                    if (!(vkIngestWorkerOn()
                          && rms.postCapture(mesh_id, rl, rdec, live->getParams(), 0.f, rw)))
                    {
                        VK_HITCH_SCOPE("meshCapture");
                        rms.capture(mesh_id, rl, rdec.get(), /*force=*/true);
                    }
                }
                else if (rds == LLCleanMeshAsset::DECODE_NO_BYTES)
                {
                    // Our raw bytes are gone (the LRU demoted them once captured): ask LL's
                    // repository for that LOD again (throttled per (mesh, lod)).
                    if (cleanLodFetchThrottle(mesh_id, rl))
                    {
                        LLCleanMeshAsset::sLodFetchReq.fetch_add(1, std::memory_order_relaxed);
                        gMeshRepo.loadMesh(cvobj, live->getParams(), rl);
                        LLVKHitch::count("rematFetch");
                    }
                }
                // PENDING: the decode wakes this object. FAILED: the bytes do not decode (or
                // the face is absent at that level) -- it stays wanted and is logged as stale
                // by the store (no legacy geometry for that slot).
            }
        }
    }

    // LOD CONVERGENCE (#29 lodmiss strand). The GPU cull selects the draw level
    // with detailFromTan(lodFactor*radius/dist) -- INDEPENDENTLY of LL's calcLOD
    // (whose close-range ramp/hysteresis/boosts disagree for a subset of objects).
    // Historically we only captured LL's current level, so the GPU-selected level
    // could stay SENTINEL forever and the object was permanently invisible
    // (detector: thousands of lodmiss instances, e.g. a shared fence asset with
    // only L1 captured while distant instances select L0). Fix by OWNING the
    // choice: compute the exact level the cull will pick (same formula, same
    // ll_round) and capture THAT level from sources we control -- prims tessellate
    // at any detail, meshes decode from our retained per-LOD bytes. Sculpts can't
    // be synthesized at other levels without the sculpt image (their re-capture-on-
    // decode path stands). capture()'s per-(mesh,lod) latch makes this once-only.
    // Rigged/HUD skip: the cull draws them at highest-resident, not by distance.
    bool conv_waiting = false;   // the GPU-desired level is being fetched / decoded
    if (!rigged && !pObj->isHUDAttachment())
    {
        const F32 radius = live->mLODScaleBias.scaledVec(cvobj ? cvobj->getScale()
                                                                : pObj->getScale()).length();
        const LLVector3 to_cam = pObj->getRenderPosition()
                               - LLViewerCamera::getInstance()->getOrigin();
        const F32 dist = llmax(to_cam.length(), 0.001f);
        const F32 tan_angle = ll_round((LLVOVolume::sLODFactor * radius) / dist, 0.01f);
        const S32 desired = (tan_angle <= 0.03f) ? 0
                          : (tan_angle <= 0.06f) ? 1
                          : (tan_angle <= 0.24f) ? 2 : 3;
        // Strand forensics: record what this visit decided (see noteConverge);
        // the strand alert prints it per offender via debugGroupString.
        LLCleanMeshAsset::ConvergeTrace conv_tr;
        conv_tr.frame    = (U32)gFrameCount;
        conv_tr.desired  = (S16)desired;
        conv_tr.mesh_lod = (S16)mesh_lod;
        // Heal whenever the GROUP lacks a published slot at the desired level --
        // NOT merely when desired != LL's current lod (strand alert 2293,
        // conv=AGREE pattern): when both metrics agree on a level LL never
        // downloaded (LL happily renders its loaded LOD via its own group
        // fallback), the old gate idled forever and the GPU emitted nothing.
        if (LLCleanMeshAsset::instance().groupSlotAt(mesh_id, face_idx, desired)
            == 0xFFFFFFFFu)
        {
            // STRUCTURAL LOD MAPPING (LL parity). Not every mesh asset HAS all 4
            // LODs -- uploaders commonly provide a subset, and LL resolves a
            // requested level to the nearest level the asset actually contains
            // (getActualMeshLOD, from the mesh header). Without this, a level the
            // asset doesn't have can NEVER be fetched or captured, and every
            // instance whose distance selects it is invisible FOREVER (lodmiss
            // froze at 3423 with fetch requests running -- nothing to fetch).
            // fetch_lod is what we obtain bytes/geometry for; the group still
            // registers it AT the GPU-selected level, so the cull's selection
            // resolves exactly like LL would render it.
            S32 fetch_lod = desired;
            if (pObj->isMesh() && !is_local_mesh)
            {
                const S32 actual = gMeshRepo.getActualMeshLOD(live->getParams(), desired);
                if (actual >= 0) fetch_lod = actual;
            }
            LLCleanMeshAsset::Slot dslot =
                LLCleanMeshAsset::instance().getSlot(mesh_id, fetch_lod, face_idx);
            conv_tr.fetch_lod = (S16)fetch_lod;
            conv_tr.action = dslot.valid ? LLCleanMeshAsset::CONV_SLOT_EXISTS
                                         : LLCleanMeshAsset::CONV_NO_SOURCE;
            const S32 cap_lod = is_prim ? desired : fetch_lod;
            LLCleanMeshAsset::CaptureWaiter cw;
            cw.obj = pObj->getID(); cw.face_idx = face_idx; cw.site = 2; cw.bookkeep = !is_prim;
            if (!dslot.valid && async_src
                && LLCleanMeshAsset::instance().captureInFlight(mesh_id, cap_lod, &cw))
            {
                // The level is being captured on the ingest worker: wait for it (no
                // structural-gap fallback while it is in flight).
                conv_tr.action = LLCleanMeshAsset::CONV_DECODE_WAIT;
            }
            else if (!dslot.valid && is_prim && async_src
                     && LLCleanMeshAsset::instance().postCapture(mesh_id, desired, nullptr, live->getParams(),
                                                                 LLVolumeLODGroup::getVolumeScaleFromDetail(desired), cw))
            {
                conv_tr.action = LLCleanMeshAsset::CONV_DECODE_WAIT;
            }
            else if (!dslot.valid)
            {
                LLPointer<LLVolume> dsrc;
                LLVolume* dcap = nullptr;
                if (is_prim)
                {
                    VK_HITCH_SCOPE("sweep_tess");
                    dsrc = vkTessellatePrim(pObj, face_idx, live->getParams(),
                                            LLVolumeLODGroup::getVolumeScaleFromDetail(desired), vk_flexi, flexi_scale);
                    dcap = dsrc.get();
                }
                else if (is_local_mesh)
                {
                    if (live->getNumVolumeFaces() > face_idx) dcap = live;
                }
                else if (pObj->isMesh())
                {
                    LLCleanMeshAsset::DecodeState dstate = LLCleanMeshAsset::DECODE_NO_BYTES;
                    if (cleanCaptureBackoff(mesh_id, fetch_lod, face_idx))
                    {
                        conv_tr.have_raw = 1;   // bytes known; decode backed off
                    }
                    else if ((dstate = LLCleanMeshAsset::instance().takeDecoded(
                                  mesh_id, fetch_lod, live->getParams(), face_idx, /*site*/2,
                                  cleanIsSelfPriority(pObj), pObj->getID(), dsrc)) != LLCleanMeshAsset::DECODE_NO_BYTES)
                    {
                        // Async decode (see the capture path above): READY -> capture now;
                        // PENDING -> wait for it (no structural-gap fallback while the
                        // real level is in flight); FAILED -> negative-cache.
                        conv_tr.have_raw = 1;
                        if (dstate == LLCleanMeshAsset::DECODE_READY && dsrc.notNull())
                        {
                            LLVKHitch::count("meshDecodeLODTaken");
                            const bool has_face = dsrc->getNumVolumeFaces() > face_idx;
                            if (!has_face) cw.bookkeep = false;
                            if (async_src
                                && LLCleanMeshAsset::instance().postCapture(mesh_id, fetch_lod, dsrc, live->getParams(), 0.f, cw))
                            {
                                if (has_face) conv_tr.action = LLCleanMeshAsset::CONV_DECODE_WAIT;
                                else cleanCaptureResult(mesh_id, fetch_lod, face_idx, false);
                            }
                            else if (has_face)
                            {
                                dcap = dsrc.get();
                            }
                            else
                            {
                                { VK_HITCH_SCOPE("meshCapture"); LLCleanMeshAsset::instance().capture(mesh_id, fetch_lod, dsrc.get()); }
                                cleanCaptureResult(mesh_id, fetch_lod, face_idx, false);
                            }
                        }
                        else if (dstate == LLCleanMeshAsset::DECODE_PENDING)
                        {
                            conv_tr.action = LLCleanMeshAsset::CONV_DECODE_WAIT;
                        }
                        else
                        {
                            cleanCaptureResult(mesh_id, fetch_lod, face_idx, false);
                        }
                    }
                    else if (cvobj)
                    {
                        // OWN THE FETCH (#29 permanent-vanish class). We have NO raw
                        // bytes for the GPU-selected level because LL only downloads
                        // the level ITS calcLOD picks -- when ours differs, the level
                        // never arrives and the mesh is invisible FOREVER (and every
                        // camera move re-rolls which meshes fall into this hole).
                        // Request it ourselves; the repo's receive path already runs
                        // retainRawMesh, so the bytes land in our store and the next
                        // sweep pass decodes + captures + publishes the level.
                        // Throttled per (mesh, lod): the repo dedups pending fetches,
                        // but don't hammer it from every sweep visit.
                        if (cleanLodFetchThrottle(mesh_id, fetch_lod))
                        {
                            LLCleanMeshAsset::sLodFetchReq.fetch_add(1, std::memory_order_relaxed);
                            gMeshRepo.loadMesh(cvobj, live->getParams(), fetch_lod);
                            conv_tr.action = LLCleanMeshAsset::CONV_FETCH_SENT;
                        }
                        else
                        {
                            conv_tr.action = LLCleanMeshAsset::CONV_FETCH_WAIT;
                        }
                    }
                }
                if (dcap && face_idx < dcap->getNumVolumeFaces())
                {
                    { VK_HITCH_SCOPE("meshCapture"); LLCleanMeshAsset::instance().capture(mesh_id, is_prim ? desired : fetch_lod, dcap); }
                    dslot = LLCleanMeshAsset::instance().getSlot(mesh_id, is_prim ? desired : fetch_lod, face_idx);
                    if (!is_prim) cleanCaptureResult(mesh_id, fetch_lod, face_idx, dslot.valid);
                    if (dslot.valid)
                    {
                        LLCleanMeshAsset::sLodFetchCap.fetch_add(1, std::memory_order_relaxed);
                        conv_tr.action = LLCleanMeshAsset::CONV_CAPTURED;
                    }
                }
            }
            // Publish the desired level into the face's LOD group (idempotent).
            if (dslot.valid)
            {
                LLCleanMeshAsset::instance().registerLodSlot(mesh_id, face_idx, desired, dslot.slot_id);
            }
            else if (conv_tr.action != LLCleanMeshAsset::CONV_FETCH_SENT &&
                     conv_tr.action != LLCleanMeshAsset::CONV_FETCH_WAIT &&
                     conv_tr.action != LLCleanMeshAsset::CONV_DECODE_WAIT)
            {
                // STRUCTURAL GAP FALLBACK (strand alert 2293, conv=NO_SOURCE
                // raw=1 pattern): the desired level's bytes exist but its decode
                // lacks this face (uploaders may drop faces in lower LODs), or
                // the source can't be synthesized (sculpt at a level we never
                // saw). Nothing will EVER arrive for this level, so "invisible
                // until it lands" means invisible forever -- publish the best
                // resident level's slot instead (LL parity: LL renders the
                // loaded LOD when the desired one doesn't exist). NOT the
                // rejected streaming clamp: fetch-in-flight cases above keep
                // the invisible-until-lands design, and registerLodSlot
                // overwrites, so a later real capture replaces this fallback.
                const U32 best = LLCleanMeshAsset::instance().groupBestSlot(mesh_id, face_idx);
                if (best != 0xFFFFFFFFu)
                {
                    LLCleanMeshAsset::instance().registerLodSlot(mesh_id, face_idx, desired, best);
                    conv_tr.action = LLCleanMeshAsset::CONV_FALLBACK;
                }
            }
        }
        conv_waiting = conv_tr.action == LLCleanMeshAsset::CONV_FETCH_SENT
                    || conv_tr.action == LLCleanMeshAsset::CONV_FETCH_WAIT
                    || conv_tr.action == LLCleanMeshAsset::CONV_DECODE_WAIT;
        LLCleanMeshAsset::instance().noteConverge(mesh_id, face_idx, conv_tr);
    }

    // INSTANCE IS NOT GATED ON LL's LOD (#88 root). The face's instance used to require
    // a slot at mesh_lod -- LL's live volume level, which under VK is also STALE (LL's
    // calcLOD reads drawable distance state the diverted pipeline no longer maintains).
    // Uploaders routinely zero faces in low LODs (LL's empty-face placeholder, nv=1),
    // so a face empty at LL's level got NO instance even when the GPU-desired level
    // (captured by the converge block above) held its real geometry: the face was never
    // drawn at any distance (tires/doors missing, depending on where the camera was at
    // load). Build the instance from ANY resident level of the face; the LOD group lets
    // the cull pick the level (absent/empty levels fall back to a resident one).
    LLCleanMeshAsset::Slot rep = exist;   // representative slot: bounds + weights
    S32 rep_lod = mesh_lod;
    if (!exist.valid)
    {
        for (S32 L = LLCleanMeshAsset::NUM_LOD - 1; L >= 0; --L)
        {
            if (L == mesh_lod) continue;
            const LLCleanMeshAsset::Slot s = LLCleanMeshAsset::instance().getSlot(mesh_id, L, face_idx);
            if (s.valid && s.num_indices > 0) { rep = s; rep_lod = L; break; }
        }
    }
    // Nothing captured for this face at any level yet (bytes downloading, decode in
    // flight, ...) -- bail; the instance gets built once a level lands.
    if (!rep.valid)
    {
        // Waiting on the GPU-desired level is NOT a settled absence, whatever LL's level said.
        if (conv_waiting && (vk_outcome == LLFace::VKI_DECODE_LATCHED || vk_outcome == LLFace::VKI_LOD_LACKS_FACE
                             || vk_outcome == LLFace::VKI_CAPTURE_NO_SLOT || vk_outcome == LLFace::VKI_NO_SLOT))
            vk_outcome = LLFace::VKI_LOD_WAIT;
        facep->mVKIngest = vk_outcome;
        return;
    }
    facep->mVKIngest = LLFace::VKI_UP_TO_DATE;

    LLCleanInstanceStore::Instance inst;
    std::memset(&inst, 0, sizeof(inst));

    // ATTACHMENT SKINNING. A statically-worn UNRIGGED attachment rides rigidly on
    // one avatar joint (the attach point). Rather than the CPU re-composing its
    // world matrix every frame and re-uploading it (which the sweep throttles, so
    // the geometry lags behind a moving avatar), we treat it as rigid-skinned to
    // that one joint: store the CONSTANT joint-local offset in model_matrix, the
    // attach joint index in aabb_max.w, the avatar's joints-slot in skin_id, set
    // flag bit9, and let cleanGPUV derive the live matrix from the per-frame
    // avatar-joints table. Detected on ANY prim of the linkset via
    // getAvatarAncestor() (isAttachment()/getAvatar() are root-only); the attach
    // point comes from the ROOT's attachment state. HUD + rigged excluded.
    bool      is_attach       = false;
    S32       attach_bone     = -1;
    U32       attach_av_slot  = 0xFFFFFFFFu;
    if (!rigged && !pObj->isHUDAttachment())
    {
        LLVOAvatar* av = pObj->getAvatarAncestor();
        if (av && !av->isDead())
        {
            LLViewerObject* root = pObj->getRootEdit();
            LLViewerJointAttachment* ap =
                av->getTargetAttachmentPoint(root ? root : pObj);
            if (ap)
            {
                const S32 jn = ap->getJointNum();
                if (jn >= 0 && jn < (S32)LLCleanSkinStore::MAX_AVATAR_JOINTS)
                {
                    attach_bone      = jn;
                    attach_av_slot   = LLCleanSkinStore::instance().ensureAvatarSlot(av);
                    is_attach        = (attach_av_slot != 0xFFFFFFFFu);
                }
            }
        }
    }

    // OWNED model_matrix. Volume-local -> AGENT space, built from the object's
    // CURRENT authoritative world pose -- getRenderPosition()/getRenderRotation()
    // (which compose parent linkset + attachment + region origin) and getScale().
    // This replaces the LL composition relativeXform * renderMatrix: those are
    // LL-cached matrices recomputed only when LL calls updateRelativeXform, so for
    // a static drawable they go stale (e.g. on an agent region-cross the region
    // origin shifts but the cached static relativeXform doesn't) -> the geometry is
    // drawn off-screen and frustum-rejected forever. Recomputing from the live pose
    // every ingest (and the sweep re-ingests continuously) keeps it always current
    // and removes the last LL-owned transform dependency for non-rigged geometry.
    {
        LLMatrix4 composite;
        const bool is_hud = pObj->isHUDAttachment();
        if (rigged && cvobj)
        {
            // Rigged: the draw VS uses the pre-skinned AGENT-space verts and the
            // collapse guard is now JOINT-RELATIVE (cleanSkinPreC compares each vert
            // to its own joint, NOT to model_matrix), so model_matrix no longer
            // gates the skinned draw at all. Its ONE remaining consumer is the
            // static FALLBACK: when a rigged face's skin_id hasn't resolved yet the
            // cull draws it through cleanGPUV using THIS matrix at bind pose. That
            // needs the volume's real scale+rotation -- getRelativeXform() carries
            // it; an identity-rotation matrix strips the scale and the bind-pose
            // geometry renders hugely oversized + offset (the "mm alignment tris
            // become 1x1m above the avatar" regression). Keep getRelativeXform().
            // (The proper fix for the fallback's wrong placement is to not draw
            // unresolved-skin rigged as static at all -- task #37 -- handled in the
            // cull, not here.)
            composite = cvobj->getRelativeXform();
        }
        else if (is_hud && cvobj)
        {
            // HUD attachments render in HUD-LOCAL space under the HUD ortho pass,
            // not agent space. Pose from OBJECT STATE via the composer shared with
            // the HUD pick (see vkComposeAttachmentMatrix).
            vkComposeAttachmentMatrix(pObj, composite);
        }
        else
        {
            // OWNED world geometry: agent-space matrix from OBJECT-STATE pose.
            // NOT getRenderPosition/getRenderRotation -- those read the DRAWABLE
            // xform, which decays to garbage after LL's mass drawable resets since
            // our pipeline (not LL's renderer) is the consumer; see the matching
            // note in buildOwnedModelMatrix (the month-old #29 root). Attachments
            // use their attach-point-relative object state (bit9 GPU path).
            LLVector3    wscale = pObj->getScale();
            LLVector3    wpos;
            LLQuaternion wrot;
            if (is_attach)
            {
                // ATTACHMENT: the model_matrix is the prim's pose RELATIVE TO ITS
                // ATTACH POINT; the vertex shader re-applies the live joint matrix
                // every frame. Compose it from OBJECT STATE, which is exactly that
                // relation and constant while worn: a root prim's position/rotation
                // are already relative to the attach point, a child prim's are
                // relative to the root. Do NOT derive it from a world pose (the old
                // ownedWorld * inverse(jointWorld) bake): the render/drawable pose is
                // only refreshed by LL's diverted display() path, so under Vulkan it
                // lags the joints, and each re-bake encoded a stale offset -- worn
                // objects drifted behind the avatar at the sweep's re-ingest rate.
                vkRenderLocalPose(pObj, wpos, wrot, &wscale);   // eased while LL interpolates
                LLViewerObject* root = pObj->getRootEdit();
                if (root && root != pObj)
                {
                    LLVector3 rpos; LLQuaternion rrot;
                    vkRenderLocalPose(root, rpos, rrot);
                    wpos = wpos * rrot + rpos;
                    wrot = wrot * rrot;
                }
            }
            if (!is_attach)
            {
                // Shared composer (store refreshers use the same one): un-spun pose
                // + GPU target-omega spin record under VK.
                LLCleanInstanceStore::composeOwnedPose(pObj, &composite.mMatrix[0][0],
                                                       inst.spin_axis, inst.spin_pivot);
            }
            else
            {
            const LLVector3 ax = LLVector3(wscale.mV[VX], 0.f, 0.f) * wrot;
            const LLVector3 ay = LLVector3(0.f, wscale.mV[VY], 0.f) * wrot;
            const LLVector3 az = LLVector3(0.f, 0.f, wscale.mV[VZ]) * wrot;
            composite.initRows(LLVector4(ax, 0.f), LLVector4(ay, 0.f),
                               LLVector4(az, 0.f), LLVector4(wpos, 1.f));
            }
        }
        std::memcpy(inst.model_matrix, composite.mMatrix, sizeof(inst.model_matrix));
    }

    // AABB in VOLUME-LOCAL space, read from the SLOT we captured (cap_lo/cap_hi are
    // the bounds of the positions we uploaded). This decouples the instance build
    // from any live volume -- once captured, our mega-VBO slot is the sole authority.
    bool bounds_measured = false;
    {
        const F32 lo0 = rep.cap_lo[0], lo1 = rep.cap_lo[1], lo2 = rep.cap_lo[2];
        const F32 hi0 = rep.cap_hi[0], hi1 = rep.cap_hi[1], hi2 = rep.cap_hi[2];
        const F32 ext = (hi0-lo0) + (hi1-lo1) + (hi2-lo2);
        bounds_measured = ext > 1e-4f;
        inst.aabb_min[0]=lo0; inst.aabb_min[1]=lo1; inst.aabb_min[2]=lo2; inst.aabb_min[3]=0.f;
        inst.aabb_max[0]=hi0; inst.aabb_max[1]=hi1; inst.aabb_max[2]=hi2; inst.aabb_max[3]=0.f;
        // ATTACHMENT: pack the attach joint index into the free .w lane; the draw
        // VS reads it to index the per-avatar joints table. (The cull skips the
        // frustum test for attachments, so aabb_max.w is unused there.)
        if (is_attach) inst.aabb_max[3] = (F32)attach_bone;
    }

    // tint + uv_matrix (mirrors registerFace 5831-5873, 5979-5987).
    bool anim_parametric = false;   // GPU texture animation (params in uv_matrix lanes)
    if (te)
    {
        LLColor4 tint = te->getGLTFRenderMaterial()
                        ? te->getGLTFRenderMaterial()->mBaseColor : te->getColor();
        inst.tint[0]=tint.mV[0]; inst.tint[1]=tint.mV[1];
        inst.tint[2]=tint.mV[2]; inst.tint[3]=tint.mV[3];

        // GPU TEXTURE ANIMATION: when the anim is fully parametric (llSetTextureAnim
        // state machine -- everything a pure function of static params + time), pack
        // the params into the uv_matrix lanes and let cleanGPUV::texAnimUV evaluate
        // per frame. Upload-once: zero per-frame CPU work, zero dirty-range churn.
        // Scripted UV manipulation (llOffsetTexture loops) arrives as TE updates and
        // flows through the static path below unchanged.
        LLViewerTextureAnim* ta = cvobj ? cvobj->mTextureAnimp : nullptr;
        if (ta && (ta->mMode & LLTextureAnim::ON)
            && (ta->mFace < 0 || (S32)ta->mFace == face_idx))
        {
            anim_parametric = true;
            // TEXANIM DIAG (INFO "VKTexAnim", per object at most every 5 s, bounded
            // table): the animated face's texture streaming state -- VK on-screen demand
            // (top-mip texels; 16777215 = full), LL's current/desired discard and the
            // decoded vs full size. A texture stuck at a high discard with a demand far
            // below its full size = the demand under-request; demand high but discard
            // not following = the fetch/decode side. Only with SlipstreamVerboseLogging (the
            // tag is muted otherwise), so the bookkeeping is skipped too.
            static LLCachedControl<bool> s_verbose_log(gSavedSettings, "SlipstreamVerboseLogging", false);
            if (s_verbose_log)
            {
                static std::unordered_map<U32, F64> s_ta_log;
                const F64 now = LLTimer::getTotalSeconds();
                auto it = s_ta_log.find(pObj->getLocalID());
                if ((it == s_ta_log.end() && s_ta_log.size() < 4096) || (it != s_ta_log.end() && now - it->second > 5.0))
                {
                    s_ta_log[pObj->getLocalID()] = now;
                    LLViewerTexture* vt = facep->getTexture();
                    if (!vt) vt = facep->getTexture(LLRender::BASECOLOR_MAP);
                    LLViewerFetchedTexture* ft = vt ? LLViewerTextureManager::staticCastToFetchedTexture(vt) : nullptr;
                    const F32 eff_s = (ta->mMode & LLTextureAnim::ROTATE) ? te->getScaleS()
                                    : (ta->mMode & LLTextureAnim::SCALE) ? ta->mStart
                                    : (ta->mSizeX && ta->mSizeY) ? 1.f / (F32)ta->mSizeX : 1.f;
                    const F32 eff_t = (ta->mMode & LLTextureAnim::ROTATE) ? te->getScaleT()
                                    : (ta->mMode & LLTextureAnim::SCALE) ? ta->mStart
                                    : (ta->mSizeX && ta->mSizeY) ? 1.f / (F32)ta->mSizeY : 1.f;
                    // <GMDX> INFO, not DEBUG: the tag's INFO shows with SlipstreamVerboseLogging; DEBUG never did.
                    LL_INFOS("VKTexAnim") << "obj=" << pObj->getID() << " local=" << pObj->getLocalID()
                        << " face=" << face_idx << " mode=0x" << std::hex << (U32)ta->mMode << std::dec
                        << " size=" << (S32)ta->mSizeX << "x" << (S32)ta->mSizeY
                        << " start=" << ta->mStart << " len=" << ta->mLength << " rate=" << ta->mRate
                        << " te_scale=" << te->getScaleS() << "," << te->getScaleT()
                        << " eff_uv_scale=" << eff_s << "," << eff_t
                        << " scale=" << pObj->getScale()
                        << " dist=" << (pObj->getPositionAgent() - LLViewerCamera::getInstance()->getOrigin()).length()
                        << " temp=" << (pObj->flagTemporaryOnRez() ? 1 : 0)
                        << " pbr=" << (te->getGLTFRenderMaterial() ? 1 : 0)
                        << " tex=" << (vt ? vt->getID() : LLUUID::null)
                        << " vk_demand=" << (ft ? (S64)ft->getVKDemand() : -1)
                        << " discard=" << (ft ? ft->getDiscardLevel() : -99)
                        << " desired=" << (ft ? ft->getDesiredDiscardLevel() : -99)
                        << " decoded=" << (vt ? vt->getWidth() : 0) << "x" << (vt ? vt->getHeight() : 0)
                        << " full=" << (ft ? ft->getFullWidth() : 0) << "x" << (ft ? ft->getFullHeight() : 0)
                        << " boost=" << (vt ? vt->getBoostLevel() : -1)
                        << LL_ENDL;
                }
            }
            F32* L = inst.uv_matrix;           // lane map documented in cleanGPUV
            L[0]  = (F32)ta->mMode;
            L[1]  = (F32)ta->mSizeX;
            L[2]  = (F32)ta->mSizeY;
            L[3]  = ta->mStart;
            L[4]  = ta->mLength;
            L[5]  = ta->mRate;
            L[6]  = te->getOffsetS();
            L[7]  = te->getOffsetT();
            L[8]  = te->getScaleS();
            L[9]  = te->getScaleT();
            L[10] = te->getRotation();
            L[11] = 0.f;                       // t0 phase base (session-relative)
        }
        else if (const LLGLTFMaterial* uvm = te->getGLTFRenderMaterial())
        {
            // PBR face: LL ignores the legacy TE repeats/offset/rotation and applies
            // the glTF KHR_texture_transform instead (textureUtilV.glsl
            // texture_transform): uv' = flip(offset + R * (S * flip(uv))), with
            // flip(v) = 1 - v (glTF UVs are left-handed, SL's right-handed). Affine,
            // so it folds into the uv_matrix lanes the VS applies. Using the TE
            // transform here rendered glTF-tiled textures as one stretched (blurry)
            // copy. The base-color transform is used for every map of the face.
            const LLGLTFMaterial::TextureTransform& tt =
                uvm->mTextureTransform[LLGLTFMaterial::GLTF_TEXTURE_INFO_BASE_COLOR];
            const F32 c = cosf(tt.mRotation), sn = sinf(tt.mRotation);
            const F32 sx = tt.mScale.mV[VX], sy = tt.mScale.mV[VY];
            const F32 ox = tt.mOffset.mV[VX], oy = tt.mOffset.mV[VY];
            LLMatrix4 uv;
            uv.setIdentity();
            uv.mMatrix[0][0] =  c * sx;     // u' = c*sx*u - sn*sy*v + (sn*sy + ox)
            uv.mMatrix[1][0] = -sn * sy;
            uv.mMatrix[3][0] =  sn * sy + ox;
            uv.mMatrix[0][1] =  sn * sx;    // v' = sn*sx*u + c*sy*v + (1 - c*sy - oy)
            uv.mMatrix[1][1] =  c * sy;
            uv.mMatrix[3][1] =  1.f - c * sy - oy;
            std::memcpy(inst.uv_matrix, uv.mMatrix, sizeof(inst.uv_matrix));
        }
        else
        {
        const F32 r = te->getRotation();
        const F32 cos_r = cosf(r), sin_r = sinf(r);
        const F32 ms = te->getScaleS(), mt = te->getScaleT();
        const F32 os = te->getOffsetS(), ot = te->getOffsetT();
        LLMatrix4 uv;
        uv.setIdentity();
        uv.mMatrix[0][0] =  ms * cos_r;
        uv.mMatrix[1][0] =  ms * sin_r;
        uv.mMatrix[0][1] = -mt * sin_r;
        uv.mMatrix[1][1] =  mt * cos_r;
        uv.mMatrix[3][0] = os + 0.5f - 0.5f * (ms * cos_r + ms * sin_r);
        uv.mMatrix[3][1] = ot + 0.5f - 0.5f * (-mt * sin_r + mt * cos_r);
        std::memcpy(inst.uv_matrix, uv.mMatrix, sizeof(inst.uv_matrix));
        }
    }
    else
    {
        inst.tint[0]=inst.tint[1]=inst.tint[2]=inst.tint[3]=1.f;
        inst.uv_matrix[0]=inst.uv_matrix[5]=inst.uv_matrix[10]=inst.uv_matrix[15]=1.f;
    }
    // Texel demand input (clean_cull.comp): the captured slot's UV density (metres per
    // UV unit, computed at capture), else the face's raw texcoord range (the cull then
    // divides the bounds' size by uv_matrix x this range, as LL's mTexExtents do).
    if (rep.valid && rep.uv_density)
    {
        std::memcpy(&inst.spin_pivot[3], &rep.uv_density, sizeof(F32));
    }
    else
    {
        const LLVolumeFace& vf = live->getVolumeFace(face_idx);
        inst.spin_pivot[3] = LLCleanInstanceStore::packUVExtent(
            vf.mTexCoordExtents[1].mV[0] - vf.mTexCoordExtents[0].mV[0],
            vf.mTexCoordExtents[1].mV[1] - vf.mTexCoordExtents[0].mV[1]);
    }

    // UPLOAD-ONCE / GPU LOD SELECT. Register this LOD's slot into the object-face
    // LOD group and store the GROUP id (not a fixed slot). The cull picks the level
    // by distance and resolves the draw slot from the group -- so an object is
    // registered ONCE and never re-registered on LOD change; other levels fill the
    // same group as they are captured on later passes.
    bool slot_has_weights = false;
    {
        // Publish the representative level (LL's level when it has the face, else the
        // resident level found above) -- idempotent; other levels were/are published
        // by the converge block.
        inst.lod_group_id = rep.valid
            ? LLCleanMeshAsset::instance().registerLodSlot(mesh_id, face_idx, rep_lod, rep.slot_id)
            : 0xFFFFFFFFu;
        slot_has_weights  = rep.valid && rep.has_weights;
    }
    // LL LOD radius (LLVOVolume::calcLOD): mLODScaleBias-scaled object scale length.
    // Stored in aabb_min.w (a pad lane the cull ignores for the frustum test) so the
    // cull can compute tan_angle = lodFactor*radius/distance per frame.
    inst.aabb_min[3] = live->mLODScaleBias.scaledVec(cvobj ? cvobj->getScale()
                                                           : pObj->getScale()).length();

    // Static-instance dedup signature (mirrors registerFace 6005-6076). The sweep
    // passes force_reingest=true to bypass the skip and rebuild the instance.
    // Attachments count as static here: their model_matrix (joint-local offset) is
    // constant even as the avatar moves, so the sig is stable and the sweep skips
    // re-ingesting them -- the GPU joint table supplies the per-frame motion.
    const bool clean_static = is_attach ||
        (!rigged && !slot_has_weights &&
         !drawable->isActive() && !drawable->isState(LLDrawable::ANIMATED_CHILD));
    U64 clean_sig = 0;
    bool do_ingest = true;
    if (clean_static)
    {
        clean_sig = 0xcbf29ce484222325ULL;
        auto mix = [&clean_sig](const void* p, size_t n)
        {
            const U8* b = (const U8*)p;
            for (size_t i = 0; i < n; ++i) { clean_sig ^= b[i]; clean_sig *= 0x100000001b3ULL; }
        };
        // #568: hash the pose in the instance store's render-world frame (agent + d), which
        // a region crossing does not change -- the agent-space matrix did, so every static
        // face re-upserted identical content on its next visit after each crossing.
        F32 sig_mm[16];
        std::memcpy(sig_mm, inst.model_matrix, sizeof(sig_mm));
        if (!is_attach && !rigged && !pObj->isHUDAttachment())
        {
            F32 rd[3];
            LLCleanInstanceStore::instance().renderOffset(rd);
            sig_mm[12] += rd[0]; sig_mm[13] += rd[1]; sig_mm[14] += rd[2];
        }
        mix(sig_mm, sizeof(sig_mm));
        mix(inst.spin_axis, sizeof(F32) * 3);   // not t0: phase re-derives each call
        mix(&inst.lod_group_id, sizeof(inst.lod_group_id));
        mix(inst.tint, sizeof(inst.tint));
        mix(inst.uv_matrix, sizeof(inst.uv_matrix));
        mix(&inst.spin_pivot[3], sizeof(F32));   // UV density / raw UV extent
        LLGLTFMaterial* gmr = te ? te->getGLTFRenderMaterial() : nullptr;
        if (gmr)
        {
            for (U32 t = 0; t < LLGLTFMaterial::GLTF_TEXTURE_INFO_COUNT; ++t)
                mix(gmr->mTextureId[t].mData, sizeof(LLUUID::mData));
            const bool fetching = ((LLFetchedGLTFMaterial*)gmr)->isFetching();
            mix(&fetching, sizeof(fetching));
            const bool dsided = gmr->mDoubleSided;   // selects the cull-none buckets
            mix(&dsided, sizeof(dsided));
            // Material FACTORS feed the material descriptor below. Scripts animate
            // them through glTF overrides (llSetPrimitiveParams PRIM_GLTF_* -- e.g. a
            // weather dome's lightning flashing the emissive factor); without them in
            // the signature a static face kept its stale material until a forced
            // re-ingest. (Base colour already flows through inst.tint, mixed above.)
            mix(gmr->mEmissiveColor.mV, sizeof(gmr->mEmissiveColor.mV));
            mix(&gmr->mMetallicFactor, sizeof(gmr->mMetallicFactor));
            mix(&gmr->mRoughnessFactor, sizeof(gmr->mRoughnessFactor));
            mix(&gmr->mAlphaCutoff, sizeof(gmr->mAlphaCutoff));
            const S32 amode = (S32)gmr->mAlphaMode;
            mix(&amode, sizeof(amode));
        }
        else
        {
            const void* gmp = nullptr;
            mix(&gmp, sizeof(gmp));
        }
        if (te)
        {
            const LLUUID tid = te->getID();
            mix(tid.mData, sizeof(tid.mData));
            const LLMaterial* mpp = te->getMaterialParams().get();
            mix(&mpp, sizeof(mpp));
            // Scripted lamps toggle these on static prims; without them the
            // dedup kept the stale fullbright bit / glow material.
            const U8  sig_fb   = cleanEffectiveFullbright(facep, te) ? 1 : 0;
            const F32 sig_glow = te->getGlow();
            mix(&sig_fb, sizeof(sig_fb));
            mix(&sig_glow, sizeof(sig_glow));
            // Legacy SHININESS feeds spec / env of non-spec-mapped legacy faces.
            const U8  sig_shiny = te->getShiny();
            mix(&sig_shiny, sizeof(sig_shiny));
            // Legacy bump preset (#19): picks the material's bump code / heightfield.
            const U8  sig_bump = te->getBumpmap();
            mix(&sig_bump, sizeof(sig_bump));
        }
        // TEXTURE-DERIVED CLASSIFICATION STATE. The alpha-blend -> opaque demote
        // (below) depends on the texture's component count + alpha-mask analysis,
        // which are UNKNOWN while the texture is still downloading. Without these
        // in the signature, a face classified pre-load froze as alpha-blend
        // forever -- opaque-textured content stuck in the WBOIT pass rendering
        // "half invisible" and seemingly mis-ordered (weighted average, not
        // occlusion). Mixing them re-ingests the face the moment the texture
        // resolves (or its alpha analysis completes), landing it in the right
        // bucket.
        {
            LLViewerTexture* sig_tex = facep->getTexture();
            // Resolved texture identity: BOM faces keep a constant sentinel te->getID()
            // while the bake behind it changes (IMG_DEFAULT -> real bake -> re-bakes).
            const LLUUID sig_tid = sig_tex ? sig_tex->getID() : LLUUID::null;
            mix(sig_tid.mData, sizeof(sig_tid.mData));
            // PBR BOM: any material slot may hold a sentinel resolving to the avatar's
            // bake (base colour, and creators route normal / ORM / emissive maps
            // through spare bake channels too).
            if (const LLFetchedGLTFMaterial* sgm = (const LLFetchedGLTFMaterial*)te->getGLTFRenderMaterial())
            {
                for (S32 slot = 0; slot < LLGLTFMaterial::GLTF_TEXTURE_INFO_COUNT; ++slot)
                {
                    const LLUUID sig_bc = vkResolveBakeId(pObj, sgm->mTextureId[slot]);
                    mix(sig_bc.mData, sizeof(sig_bc.mData));
                }
            }
            const S32  sig_comps = sig_tex ? (S32)sig_tex->getComponents() : -1;
            const bool sig_mask  = sig_tex && sig_tex->getIsAlphaMask();
            const S32 sig_ac = LLCleanTexture::instance().alphaGradientClass(sig_tex);
            mix(&sig_ac, sizeof(sig_ac));
            mix(&sig_comps, sizeof(sig_comps));
            mix(&sig_mask,  sizeof(sig_mask));
            // #561: toggling the rigged auto-mask re-classifies the RIGGED faces only. (It
            // used to be mixed into every face's signature, so a toggle re-ingested the
            // whole scene and the A/B also refreshed any stale face state -- "turning it
            // off fixes it" could not tell the setting from the re-ingest.)
            static LLCachedControl<bool> s_rig_mask_sig(gSavedSettings, "RenderVKRiggedAutoMask", true);
            const bool sig_rm = s_rig_mask_sig && cleanFaceIsRigged(facep);
            mix(&sig_rm, sizeof(sig_rm));
            // RenderVKAlphaCoverageDebug: selecting an object (or turning the debug on with it
            // selected) re-ingests its faces once, so the "VKCoverage SEL" line below is
            // printed for them -- it is only reached on an ingest.
            static LLCachedControl<bool> s_cov_dbg_sig(gSavedSettings, "RenderVKAlphaCoverageDebug", false);
            const bool sig_sel = s_cov_dbg_sig && pObj->isSelected();
            mix(&sig_sel, sizeof(sig_sel));
        }
        if (clean_sig == 0) clean_sig = 1;
        if (!force_reingest &&
            LLCleanInstanceStore::instance().isUpToDate(pObj->getID(), face_idx, clean_sig))
        {
            do_ingest = false;
        }
    }

    if (do_ingest)
    {
        // ---- material descriptor (mirrors registerFace 6083-6214). ----
        LLCleanMaterialStore::MaterialDesc mdesc;
        bool plain_shiny = false;   // plain (no-material) face carrying TE shininess
        if (te)
        {
            LLFetchedGLTFMaterial* gm = (LLFetchedGLTFMaterial*)te->getGLTFRenderMaterial();
            if (gm)
            {
                mdesc.base_color_id = vkResolveBakeId(pObj, gm->mTextureId[LLGLTFMaterial::GLTF_TEXTURE_INFO_BASE_COLOR]);
                mdesc.gltf = true;
                mdesc.normal_id     = vkResolveBakeId(pObj, gm->mTextureId[LLGLTFMaterial::GLTF_TEXTURE_INFO_NORMAL]);
                mdesc.mr_id         = vkResolveBakeId(pObj, gm->mTextureId[LLGLTFMaterial::GLTF_TEXTURE_INFO_METALLIC_ROUGHNESS]);
                mdesc.emissive_id   = vkResolveBakeId(pObj, gm->mTextureId[LLGLTFMaterial::GLTF_TEXTURE_INFO_EMISSIVE]);
                // base_color_factor stays NEUTRAL {1,1,1,1}: the color flows
                // through the per-instance v_tint (inst.tint = mBaseColor, set
                // above). The FS does base *= m.base_factor * v_tint, so setting
                // BOTH to the color double-applied it (texture * color^2) and
                // darkened every non-white-tinted face -- the sim-surround green
                // hills (0.635 -> 0.40). Apply the color ONCE, via v_tint.
                mdesc.emissive_factor[0]   = gm->mEmissiveColor.mV[0];
                mdesc.emissive_factor[1]   = gm->mEmissiveColor.mV[1];
                mdesc.emissive_factor[2]   = gm->mEmissiveColor.mV[2];
                mdesc.emissive_factor[3]   = 0.f;
                mdesc.metallic_factor      = gm->mMetallicFactor;
                mdesc.roughness_factor     = gm->mRoughnessFactor;
                mdesc.alpha_cutoff = (gm->mAlphaMode == LLGLTFMaterial::ALPHA_MODE_MASK)
                                     ? clean_mask_cutoff(gm->mAlphaCutoff, gm->mBaseColorTexture.get()) : -1.f;   // <GMDX>
            }
            else
            {
                LLUUID tid = te->getID();
                if (LLAvatarAppearanceDefines::LLAvatarAppearanceDictionary::isBakedImageId(tid))
                {
                    if (facep->getTexture() && facep->getTexture()->getID().notNull())
                        tid = facep->getTexture()->getID();
                }
                mdesc.base_color_id = tid;
                // base_color_factor stays NEUTRAL {1,1,1,1}: the te color flows
                // through the per-instance v_tint (inst.tint = te->getColor(), set
                // above). Setting it here too double-applied the color (the FS does
                // base *= m.base_factor * v_tint) -> texture * color^2, darkening
                // every non-white-tinted face (the sim-surround green hills).
                if (const LLMaterial* lm = te->getMaterialParams().get())
                {
                    const bool has_norm = lm->getNormalID().notNull();
                    const bool has_spec = lm->getSpecularID().notNull();
                    mdesc.legacy = true;
                    if (has_norm) mdesc.normal_id = vkResolveBakeId(facep->getViewerObject(), lm->getNormalID());
                    if (has_spec)
                    {
                        mdesc.mr_id = vkResolveBakeId(facep->getViewerObject(), lm->getSpecularID());
                        const LLColor4U sc = lm->getSpecularLightColor();
                        mdesc.spec_color_gloss[0] = sc.mV[0] / 255.f;
                        mdesc.spec_color_gloss[1] = sc.mV[1] / 255.f;
                        mdesc.spec_color_gloss[2] = sc.mV[2] / 255.f;
                        mdesc.spec_color_gloss[3] = lm->getSpecularLightExponent() / 255.f;
                        mdesc.env_intensity = lm->getEnvironmentIntensity() / 255.f;
                    }
                    else
                    {
                        // No specular map: LL's legacy SHININESS drives spec and env
                        // (registerFace: mSpecColor = mEnvIntensity = {0,.25,.5,.75}
                        // [shiny]; materialF getSpecular -> (specular_color.rgb, 1)).
                        const F32 sh = cleanShinyLevel(te);
                        mdesc.spec_color_gloss[0] = sh;
                        mdesc.spec_color_gloss[1] = sh;
                        mdesc.spec_color_gloss[2] = sh;
                        mdesc.spec_color_gloss[3] = sh;
                        mdesc.env_intensity = sh;
                    }
                    // LL's materialF alphaMask() discards below cutoff - 1/512 ("comparing
                    // floats cast from 8-bit values produces acne right at the 8-bit
                    // transition points"): half a level of slack. Without it a cutoff of
                    // 255 kept only texels filtering to exactly 1.0 -- ragged, see-through
                    // cutout edges up close (#840). (Clamped: cutoff 0 stays a mask.)
                    if (lm->getDiffuseAlphaMode() == LLMaterial::DIFFUSE_ALPHA_MODE_MASK)
                        mdesc.alpha_cutoff = clean_mask_cutoff(llmax(lm->getAlphaMaskCutoff() / 255.f - 1.f / 512.f, 0.f),
                                                               facep->getTexture());   // <GMDX>
                    mdesc.emissive_mask = lm->getDiffuseAlphaMode() == LLMaterial::DIFFUSE_ALPHA_MODE_EMISSIVE;
                }
                else
                {
                    // Plain diffuse texture: no GLTF material AND no legacy
                    // material params. In LL this is legacy content -> it gets
                    // ATMOSPHERIC/Lambert lighting (HAS_ATMOS), NOT the PBR
                    // metallic-roughness path. We were routing it through PBR,
                    // which adds reflection-probe IBL ambient + a view-dependent
                    // glossy term; on large, region-edge geometry with sparse
                    // probe coverage (the sim surround) that reads as patchy,
                    // angle-dependent darkness. Mark it legacy with zero spec /
                    // zero env so it renders as clean Lambert + sky ambient like
                    // LL. (Faces WITH a GLTF material still take the PBR path.)
                    // SHININESS: LL stores {0,.25,.5,.75}[shiny] in the vertex colour
                    // alpha (LLFace::getGeometryVolume) and diffuseF/bumpF write it as
                    // spec rgb, glossiness AND env intensity (frag_data[1] =
                    // vertex_color.aaaa; encodeNormal(n, vertex_color.a)). Only for
                    // faces outside the alpha pool -- there the alpha is transparency;
                    // gated below once the face is classified.
                    mdesc.legacy = true;
                    const F32 sh = cleanShinyLevel(te);
                    mdesc.spec_color_gloss[0] = sh;
                    mdesc.spec_color_gloss[1] = sh;
                    mdesc.spec_color_gloss[2] = sh;
                    mdesc.spec_color_gloss[3] = sh;
                    mdesc.env_intensity = sh;
                    plain_shiny = true;
                }
            }
        }
        // LIVE MEDIA: while LL shows media on this face (LLFace::switchTexture swapped a
        // media texture in; its markTextured re-registers us, and the sig above changes),
        // the base colour is the media texture. The VK resolver maps the media id to a
        // live, per-frame-refreshed slot. Switching back restores the TE's own texture.
        bool live_media = false;   // -> instance bit 20 (#605, below)
        if (LLViewerTexture* mt = facep->getTexture();
            mt && mt->getType() == LLViewerTexture::MEDIA_TEXTURE && mt->getID().notNull())
        {
            mdesc.base_color_id = mt->getID();
            live_media = true;
        }
        mdesc.glow = te ? te->getGlow() : 0.f;
        // MASK CUTOFF DEFAULT. Only glTF MASK and legacy-material MASK faces carry a
        // cutoff; every other face that classifies as alpha-MASK (LL's canRenderAsMask,
        // our auto-mask demotion) kept -1 => the FS never discarded, so transparent
        // texels drew their stored colour: SL's transparent texture (white, alpha 0)
        // showed as white, black-backed alpha textures as black. LL gives such faces
        // mAlphaMaskCutoff = 0.33 (rebuildGeom / genDrawInfo); mirror it.
        {
            bool mc_blend, mc_mask, mc_fullbright;
            classifyCleanFace(facep, mc_blend, mc_mask, mc_fullbright);
            if (mc_mask && mdesc.alpha_cutoff < 0.f)
            {
                mdesc.alpha_cutoff = 0.33f;
            }
            // Plain faces in LL's alpha pool (blend, or mask via canRenderAsMask /
            // our demotion) keep transparency in the vertex alpha, not shininess:
            // no legacy spec / env for them (LLFace: shiny_in_alpha needs
            // !isInAlphaPool()).
            if (plain_shiny && (mc_blend || mc_mask))
            {
                for (F32& c : mdesc.spec_color_gloss) c = 0.f;
                mdesc.env_intensity = 0.f;
            }
            // Fullbright faces only consume env_intensity (clean_draw.frag's
            // fullbright branch = LL fullbrightShinyF), and LL routes them to
            // PASS_FULLBRIGHT_SHINY only when the TE is shiny. A spec-mapped
            // material's own env intensity must not reflect on a non-shiny face.
            if (mc_fullbright && te && te->getShiny() == 0)
                mdesc.env_intensity = 0.f;
            // LEGACY BUMP PRESET (#19; LL registerFace use_legacy_bump = bump in 1..17 and
            // no material normal map -> PASS_BUMP, deferred bumpF: the preset's normal map
            // through the face's TBN). LL never bumps glTF faces, alpha-pool faces (blend,
            // or a plain face masked via canRenderAsMask -- a legacy-material MASK face
            // still takes PASS_BUMP), fullbright faces (their PASS_POST_BUMP is the old
            // forward emboss multiply, not reproduced here) or HUD faces.
            if (te && !mdesc.gltf && mdesc.legacy && mdesc.normal_id.isNull() && !mc_blend && !mc_fullbright
                && (!mc_mask || te->getMaterialParams().notNull()) && !pObj->isHUDAttachment())
            {
                const U8 bc = te->getBumpmap();
                if (bc == kCleanBumpBrightness || bc == kCleanBumpDarkness)
                {
                    mdesc.bump = bc;   // derived from the base colour in the FS
                }
                else if (bc > kCleanBumpDarkness && bc < kCleanBumpCount)
                {
                    const LLUUID& hid = cleanStdBumpId(bc);
                    if (hid.notNull())
                    {
                        mdesc.bump = bc;
                        mdesc.normal_id = hid;   // heightfield; the VK resolver loads it as data
                    }
                }
            }
        }
        cleanFillMapUV(mdesc, te);
        // HUD faces get their own material (#532): the VK renderer samples their base
        // colour through the UNORM view (gamma-space filtering, as LL) and keeps the
        // image uncompressed -- see MaterialDesc::hud.
        mdesc.hud = pObj->isHUDAttachment();

        // ---- skin (mirrors registerFace 6221-6238). ----
        inst.skin_id = 0xFFFFFFFFu;
        // Flexi chain (#25): register the rest pose's inverse binds; the per-frame pose
        // comes from LLVolumeImplFlexible::vkUpdateAll.
        U32 flexi_skin = 0xFFFFFFFFu;
        if (vk_flexi && slot_has_weights)
        {
            LLVolumeImplFlexible::VKRestPoint rest[LLVolumeImplFlexible::VK_FLEXI_POINTS];
            LLVolumeImplFlexible::vkRestPoints(live->getParams(), flexi_scale, rest);
            F32 inv_bind[LLVolumeImplFlexible::VK_FLEXI_POINTS * 16];
            for (S32 i = 0; i < LLVolumeImplFlexible::VK_FLEXI_POINTS; ++i)
            {
                // Rest point transform B (columns = rotated basis, translation = pos) is
                // rigid, so inv(B) = [R^T | -R^T t].
                const LLMatrix3 r(rest[i].rot);   // rows = basis rotated by rot
                F32* m = inv_bind + i * 16;
                for (S32 c = 0; c < 3; ++c)
                {
                    for (S32 k = 0; k < 3; ++k) m[c * 4 + k] = r.mMatrix[k][c];
                    m[c * 4 + 3] = 0.f;
                }
                for (S32 k = 0; k < 3; ++k)
                    m[12 + k] = -(r.mMatrix[k][0] * rest[i].pos.mV[0] + r.mMatrix[k][1] * rest[i].pos.mV[1]
                                 + r.mMatrix[k][2] * rest[i].pos.mV[2]);
                m[15] = 1.f;
            }
            flexi_skin = LLCleanSkinStore::instance().flexiChain(pObj->getID(), inv_bind);
            LL_DEBUGS("VKFlexi") << "chain " << pObj->getID() << " face " << face_idx << " skin " << flexi_skin << LL_ENDL;
            if (flexi_skin != 0xFFFFFFFFu) LLVolumeImplFlexible::vkTrack(pObj->getID());
        }
        const bool gpu_rigged = (rigged && slot_has_weights) || flexi_skin != 0xFFFFFFFFu;
        if (flexi_skin != 0xFFFFFFFFu)
        {
            inst.skin_id = flexi_skin;
            // WORN FLEXI: model_matrix above is attach-point-relative, but a chain row
            // (bit19, no bit9) is bounded by the cull and the pick from its translation as a
            // WORLD origin. Use the chain frame's (vkSimulate: getRenderPosition); after the
            // static signature, so the sig stays the constant attach-relative pose, and
            // LLVolumeImplFlexible::vkUpdateAll keeps it current (setFlexiOrigin).
            if (is_attach)
            {
                const LLVector3 fpos = pObj->getRenderPosition();
                inst.model_matrix[12] = fpos.mV[0];
                inst.model_matrix[13] = fpos.mV[1];
                inst.model_matrix[14] = fpos.mV[2];
            }
        }
        else if (gpu_rigged && pObj->getAvatar() && cvobj)
        {
            const LLMeshSkinInfo* sk = cvobj->getSkinInfo();
            if (sk)
                inst.skin_id = LLCleanSkinStore::instance().getOrCreate(pObj->getAvatar(), sk);
        }
        else if (is_attach)
        {
            // Attachment: skin_id is the AVATAR joints-table slot (not a rigged
            // palette id). The draw VS reads avatar_joints[skin_id].world[bone].
            inst.skin_id = attach_av_slot;
        }

        // ---- flags (mirrors registerFace 6240-6336). ----
        inst.flags = 0;
        if (gpu_rigged) inst.flags |= 1u;
        // bit19 = FLEXI CHAIN: skinned like rigged (bit0) but a world object: the cull and
        // the pick bound it from its own transform (LLCleanInstanceStore::kFlexiFlag).
        if (flexi_skin != 0xFFFFFFFFu) inst.flags |= 524288u;
        // (A worn flexi is a chain like any other flexi: skinned, not attach-derived.)
        if (is_attach && flexi_skin == 0xFFFFFFFFu) inst.flags |= 512u;   // bit9 = GPU-derived attachment matrix
        if (anim_parametric) inst.flags |= 1024u; // bit10 = GPU texture animation
        // bit20 = LIVE MEDIA (#605): the base colour changes every frame with no motion, so
        // the TAA / FSR resolve keeps no history on it (clean_frame_info.glsl noTemporalFace;
        // RenderVKMediaNoTemporal gates it live).
        if (live_media) inst.flags |= 1048576u;

        bool clean_alpha_blend, clean_alpha_mask, clean_fullbright;
        U8 automask_reason = CAM_NONE;
        classifyCleanFace(facep, clean_alpha_blend, clean_alpha_mask, clean_fullbright, &automask_reason);
        bool alpha_blend = clean_alpha_blend;
        if (alpha_blend && te)
        {
            const F32 tint_a = te->getColor().mV[3];
            LLViewerTexture* dtex = facep->getTexture();
            const S32 comps = dtex ? (S32)dtex->getComponents() : 0;
            const bool tex_opaque = (comps == 3) ||
                                    (comps == 4 && dtex && dtex->getIsAlphaMask());
            // Not for rigged / glowing faces: classifyCleanFace keeps them blend on
            // purpose (LL never auto-masks them), and this reroute would otherwise
            // send them straight to OPAQUE -- cutouts drawn solid.
            if (tint_a >= 0.999f && tex_opaque && !cleanAutoMaskBlocker(facep, te))
            {
                alpha_blend = false;
                automask_reason = CAM_OPAQUE;
            }
        }
        // Invisiprims are forced into the blend bucket below whatever the analysis said.
        cleanAutoMaskNote(pObj->getID(), face_idx, water_exclusion ? (U8)CAM_NONE : automask_reason);
        // ALPHA COVERAGE DIAG (RenderVKAlphaCoverageDebug): a SELECTED object's faces --
        // the alpha bucket each lands in and the cutoff its material carries. Coverage-
        // preserving mips only apply to alpha-MASK faces (cutoff > 0); a blend face keeps
        // plain mips. Throttled per (object, face).
        {
            static LLCachedControl<bool> s_cov_dbg(gSavedSettings, "RenderVKAlphaCoverageDebug", false);
            if (s_cov_dbg && te && pObj->isSelected())
            {
                static std::map<std::pair<U32, S32>, F64> s_last;
                if (s_last.size() > 4096) s_last.clear();
                F64& last = s_last[std::make_pair(pObj->getLocalID(), face_idx)];
                const F64 now = LLTimer::getElapsedSeconds();
                if (now - last > 5.0)
                {
                    last = now;
                    static const char* kCam[CAM_COUNT] = {"none", "ll_mask", "strict", "gradient", "noalpha", "opaque",
                                                          "blend:tint", "blend:pending", "blend:gradient", "blend:other",
                                                          "blend:rigged", "blend:glow", "blend:shiny"};
                    // Every input of the alpha decision (#561 triage: one line per selected face):
                    //  - the texture the decision judged (legacy diffuse; a PBR face's base
                    //    colour) -- components, LL's strict mask, our gradient class / partial %;
                    //  - legacy material: alpha mode (0 none 1 blend 2 mask 3 emissive; -1 no
                    //    material; mat_pending=1: the TE names a material whose params have not
                    //    arrived yet -- classified as "no material" until they do), its cutoff;
                    //  - PBR: alpha mode (0 opaque 1 blend 2 mask; -1 not PBR), cutoff, base alpha;
                    //  - ll_alpha: LL's own alpha-pool decision (getPoolTypeFromTE), the blend
                    //    candidacy the auto-mask starts from;
                    //  - the auto-mask class (CAM_*) and the final bucket / instance flags.
                    const LLGLTFMaterial* sgm = te->getGLTFRenderMaterial();
                    LLViewerTexture* dtex = facep->getTexture();
                    if (!dtex && sgm) dtex = ((const LLFetchedGLTFMaterial*)sgm)->mBaseColorTexture;
                    const LLMaterial* slm = te->getMaterialParams().get();
                    static LLCachedControl<bool> s_rig_mask_dbg(gSavedSettings, "RenderVKRiggedAutoMask", true);
                    const bool ll_alpha =
                        LLPipeline::getPoolTypeFromTE(te, facep->getTexture()) == LLDrawPool::POOL_ALPHA;
                    LL_INFOS("VKCoverage") << "SEL obj=" << pObj->getLocalID() << " face=" << face_idx
                        << " rigged=" << (cleanFaceIsRigged(facep) ? 1 : 0)
                        << " tex=" << (dtex ? dtex->getID() : te->getID())
                        << " comps=" << (dtex ? (S32)dtex->getComponents() : -1)
                        << " strict_mask=" << (dtex ? (S32)dtex->getIsAlphaMask() : -1)
                        << " grad_class=" << LLCleanTexture::instance().alphaGradientClass(dtex)
                        << " partial_pct=" << (S32)LLCleanTexture::instance().alphaGradientPct(dtex)
                        << " tint_a=" << te->getColor().mV[3]
                        << " glow=" << te->getGlow() << " shiny=" << (S32)te->getShiny()
                        << " legacy_mode=" << (slm ? (S32)slm->getDiffuseAlphaMode() : -1)
                        << " legacy_cutoff=" << (slm ? (S32)slm->getAlphaMaskCutoff() : -1)
                        << " mat_pending=" << ((!slm && !te->getMaterialID().isNull()) ? 1 : 0)
                        << " gltf=" << (sgm ? 1 : 0)
                        << " pbr_mode=" << (sgm ? (S32)sgm->mAlphaMode : -1)
                        << " pbr_cutoff=" << (sgm ? sgm->mAlphaCutoff : -1.f)
                        << " pbr_base_a=" << (sgm ? sgm->mBaseColor.mV[3] : -1.f)
                        << " ll_alpha=" << (ll_alpha ? 1 : 0)
                        << " rigged_automask=" << (s_rig_mask_dbg ? 1 : 0)
                        << " reason=" << (automask_reason < CAM_COUNT ? kCam[automask_reason] : "?")
                        << " bucket=" << (alpha_blend ? "BLEND" : clean_alpha_mask ? "MASK" : "OPAQUE")
                        << " material_cutoff=" << mdesc.alpha_cutoff
                        << " pre_flags=0x" << std::hex << inst.flags << std::dec   // (before the bucket bits)
                        << " -> coverage mips " << (mdesc.alpha_cutoff > 0.f ? "YES" : "no (not an alpha-mask material)")
                        << LL_ENDL;
                }
            }
        }
        // BOMAlpha (#717): the alpha decision of a BOM face (legacy sentinel or a PBR slot
        // holding one), logged whenever any input of it changes -- the bake behind the
        // sentinel, its component count / alpha facts / discard, or the bucket chosen. A
        // face that sits in the wrong bucket after an alpha wearable change shows which
        // fact it was classified on. Bounded: one entry per (object, face), cleared at 2048.
        if (te)
        {
            const LLFetchedGLTFMaterial* bgm = (const LLFetchedGLTFMaterial*)te->getGLTFRenderMaterial();
            const bool bom_face = LLAvatarAppearanceDefines::LLAvatarAppearanceDictionary::isBakedImageId(te->getID())
                || (bgm && LLAvatarAppearanceDefines::LLAvatarAppearanceDictionary::isBakedImageId(
                       bgm->mTextureId[LLGLTFMaterial::GLTF_TEXTURE_INFO_BASE_COLOR]));
            if (bom_face)
            {
                static const char* kCamB[CAM_COUNT] = {"none", "ll_mask", "strict", "gradient", "noalpha", "opaque",
                                                       "blend:tint", "blend:pending", "blend:gradient", "blend:other",
                                                       "blend:rigged", "blend:glow", "blend:shiny"};
                LLViewerTexture* btex = facep->getTexture();
                if (!btex && bgm) btex = bgm->mBaseColorTexture;
                const LLUUID bid = btex ? btex->getID() : LLUUID::null;
                const S32 bcomps = btex ? (S32)btex->getComponents() : -1;
                const S32 bmask = btex ? (S32)btex->getIsAlphaMask() : -1;
                const S32 bgrad = (S32)LLCleanTexture::instance().alphaGradientClass(btex);
                const S32 bdisc = btex ? (S32)btex->getDiscardLevel() : -2;
                const U8 bucket = alpha_blend ? 2 : clean_alpha_mask ? 1 : 0;
                // #786: the cutoff the FS tests (8-bit; -1 = none) -- the material's own
                // (legacy mask / glTF mask) or the 0.33 default of an auto-mask -- and the
                // legacy material's raw cutoff; the bake's partial-alpha percentage (the
                // number grad_class thresholds; 255 = not analysed).
                const S32 bcut = mdesc.alpha_cutoff >= 0.f ? (S32)ll_round(mdesc.alpha_cutoff * 255.f) : -1;
                const LLMaterial* blm = te->getMaterialParams().get();
                const S32 blcut = blm ? (S32)blm->getAlphaMaskCutoff() : -1;
                const S32 bgpct = (S32)LLCleanTexture::instance().alphaGradientPct(btex);
                U64 bsig = 0xcbf29ce484222325ull;
                auto bmix = [&bsig](U64 v) { bsig = (bsig ^ v) * 0x100000001b3ull; };
                bmix(bid.getDigest64()); bmix((U64)(U32)bcomps); bmix((U64)(U32)bmask); bmix((U64)(U32)bgrad);
                bmix((U64)(U32)bdisc); bmix(bucket); bmix(automask_reason); bmix((U64)(U32)bcut);
                static std::map<std::pair<U32, S32>, U64> s_bom_last;
                if (s_bom_last.size() > 2048) s_bom_last.clear();
                U64& last = s_bom_last[std::make_pair(pObj->getLocalID(), face_idx)];
                if (last != bsig)
                {
                    last = bsig;
                    LL_INFOS("BOMAlpha") << "obj=" << pObj->getLocalID() << " face=" << face_idx
                        << " sentinel=" << te->getID() << " bake=" << bid
                        << " comps=" << bcomps << " strict_mask=" << bmask << " grad_class=" << bgrad
                        << " partial_pct=" << bgpct
                        << " discard=" << bdisc << " tint_a=" << te->getColor().mV[3]
                        << " pbr=" << (bgm ? 1 : 0)
                        << " pbr_mode=" << (bgm ? (S32)bgm->mAlphaMode : -1)
                        << " legacy_mode=" << (te->getMaterialParams().get() ? (S32)te->getMaterialParams()->getDiffuseAlphaMode() : -1)
                        << " rigged=" << (cleanFaceIsRigged(facep) ? 1 : 0)
                        << " bucket=" << (bucket == 2 ? "BLEND" : bucket == 1 ? "MASK" : "OPAQUE")
                        << " reason=" << (automask_reason < CAM_COUNT ? kCamB[automask_reason] : "?")
                        << " cutoff=" << bcut << " legacy_cutoff=" << blcut << LL_ENDL;
                }
            }
        }
        if (alpha_blend)                       inst.flags |= 2u;
        if (clean_alpha_mask)                  inst.flags |= 4u;
        if (clean_fullbright && !alpha_blend)  inst.flags |= 16u;
        if (pObj->isHUDAttachment())           inst.flags |= 8u;
        // <GMDX> A texture-animated cut-out / blend face moves its shadow every frame.
        if (anim_parametric && (alpha_blend || clean_alpha_mask))
            LLCleanIngest::instance().noteTexAnimCaster(pObj);
        if (cleanIsSelfPriority(pObj))         inst.flags |= 8192u; // bit13 self/HUD texture priority
        if (clean_fullbright)                  inst.flags |= 4096u; // VK: unlit (any bucket)
        if (te && te->getTexGen() == LLTextureEntry::TEX_GEN_PLANAR) inst.flags |= 64u;
        if (!bounds_measured)                  inst.flags |= 128u;
        // bit8 = DOUBLE-SIDED (glTF mDoubleSided): drawn without back-face culling.
        // Everything else is culled like LL (GL_CULL_FACE on; legacy is never
        // double-sided). The FS still orients N via gl_FrontFacing for these.
        if (te && te->getGLTFRenderMaterial() && te->getGLTFRenderMaterial()->mDoubleSided)
            inst.flags |= 256u;
        // bit16 = WATER EXCLUSION (invisiprim, above): always the alpha-blend bucket
        // (never mask / fullbright), whatever the texture analysis said.
        if (water_exclusion)
            inst.flags = (inst.flags & ~(4u | 16u)) | 2u | 65536u;

        mdesc.alpha_blend = (inst.flags & 2u) != 0;
        inst.material_id = LLCleanMaterialStore::instance().getOrCreate(mdesc);

        // Attachments are GPU-driven: their model_matrix is a CONSTANT joint-local
        // offset, so they must NOT be on the active list (refreshAndReap would
        // overwrite model_matrix with a world-space pose every frame and undo the
        // whole scheme). The avatar-joints table supplies the live motion instead.
        const bool is_active = !is_attach &&
            (drawable->isActive() ||
             drawable->isState(LLDrawable::ANIMATED_CHILD) || gpu_rigged);
        LLCleanInstanceStore::instance().upsert(
            pObj->getID(), face_idx, inst, is_active, clean_sig);
        facep->mVKIngest = LLFace::VKI_INGESTED;
    }
}

