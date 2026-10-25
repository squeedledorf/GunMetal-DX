
/**
 * @file llviewertexture.cpp
 * @brief Object which handles a received image (and associated texture(s))
 *
 * $LicenseInfo:firstyear=2000&license=viewerlgpl$
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

#include "llviewertexture.h"

#include "llcleantexture.h"

// Library includes
#include "llmath.h"
#include "llerror.h"
#include "llgl.h"
#include "llwin32headers.h"
#include "llhost.h"
#include "llimage.h"
#include "llimagebmp.h"
#include "llimagej2c.h"
#include "llimagetga.h"
#include "llstl.h"
#include "message.h"
#include "lltimer.h"
#include "v4coloru.h"
#include "llnotificationsutil.h"

// viewer includes
#include "lltexturedata.h"
#include "lldrawpool.h"
#include "lltexturefetch.h"
#include "llviewertexturelist.h"
#include "llviewercontrol.h"
#include "pipeline.h"
#include "llappviewer.h"
#include "llface.h"
#include "llviewercamera.h"
#include "lltextureentry.h"
#include "lltexturemanagerbridge.h"
#include "llmediaentry.h"
#include "llvovolume.h"
#include "llviewermedia.h"
#include "lltexturecache.h"
#include "llviewerwindow.h"
#include "llwindow.h"
#include <algorithm>
#include <unordered_set>
#include "llmemorygovernor.h"   // RAM #262: trim CPU copies sooner under memory pressure
#include "llagent.h"            // #485 network draw distance fetch gate
#include "llagentcamera.h"
#include "llvoavatarself.h"
///////////////////////////////////////////////////////////////////////////////

// statics
LLPointer<LLViewerTexture>        LLViewerTexture::sNullImagep = nullptr;
LLPointer<LLViewerTexture>        LLViewerTexture::sBlackImagep = nullptr;
LLPointer<LLViewerTexture>        LLViewerTexture::sCheckerBoardImagep = nullptr;
LLPointer<LLViewerFetchedTexture> LLViewerFetchedTexture::sMissingAssetImagep = nullptr;
LLPointer<LLViewerFetchedTexture> LLViewerFetchedTexture::sWhiteImagep = nullptr;
LLPointer<LLViewerFetchedTexture> LLViewerFetchedTexture::sDefaultParticleImagep = nullptr;
LLPointer<LLViewerFetchedTexture> LLViewerFetchedTexture::sDefaultImagep = nullptr;
LLPointer<LLViewerFetchedTexture> LLViewerFetchedTexture::sSmokeImagep = nullptr;
LLPointer<LLViewerFetchedTexture> LLViewerFetchedTexture::sFlatNormalImagep = nullptr;
LLPointer<LLViewerFetchedTexture> LLViewerFetchedTexture::sDefaultIrradiancePBRp;
// [SL:KB] - Patch: Render-TextureToggle (Catznip-4.0)
LLPointer<LLViewerFetchedTexture> LLViewerFetchedTexture::sDefaultDiffuseImagep = NULL;
// [/SL:KB]
LLViewerMediaTexture::media_map_t LLViewerMediaTexture::sMediaMap;
LLTexturePipelineTester* LLViewerTextureManager::sTesterp = nullptr;
F32 LLViewerFetchedTexture::sMaxVirtualSize = 8192.f*8192.f;

const std::string sTesterName("TextureTester");

S32 LLViewerTexture::sImageCount = 0;
S32 LLViewerTexture::sRawCount = 0;
S32 LLViewerTexture::sAuxCount = 0;
F32 LLViewerTexture::sDesiredDiscardBias = 1.f;

S32 LLViewerTexture::sMaxSculptRez = 128; //max sculpt image size
constexpr S32 MAX_CACHED_RAW_IMAGE_AREA = 64 * 64;
constexpr S32 DEFAULT_ICON_DIMENSIONS = 32;
constexpr S32 DEFAULT_THUMBNAIL_DIMENSIONS = 256;
U32 LLViewerTexture::sMinLargeImageSize = 65536; //256 * 256.
U32 LLViewerTexture::sMaxSmallImageSize = MAX_CACHED_RAW_IMAGE_AREA;
bool LLViewerTexture::sFreezeImageUpdates = false;
F32 LLViewerTexture::sCurrentTime = 0.0f;

constexpr F32 MEMORY_CHECK_WAIT_TIME = 1.0f;

LLViewerTexture::EDebugTexels LLViewerTexture::sDebugTexelsMode = LLViewerTexture::DEBUG_TEXELS_OFF;

const F64 log_2 = log(2.0);

//----------------------------------------------------------------------------------------------
//namespace: LLViewerTextureAccess
//----------------------------------------------------------------------------------------------

LLLoadedCallbackEntry::LLLoadedCallbackEntry(loaded_callback_func cb,
                      S32 discard_level,
                      bool need_imageraw, // Needs image raw for the callback
                      void* userdata,
                      LLLoadedCallbackEntry::source_callback_list_t* src_callback_list,
                      LLViewerFetchedTexture* target,
                      bool pause)
    : mCallback(cb),
      mLastUsedDiscard(MAX_DISCARD_LEVEL+1),
      mDesiredDiscard(discard_level),
      mNeedsImageRaw(need_imageraw),
      mUserData(userdata),
      mSourceCallbackList(src_callback_list),
      mPaused(pause)
{
    if(mSourceCallbackList)
    {
        mSourceCallbackList->insert(LLTextureKey(target->getID(), (ETexListType)target->getTextureListType()));
    }
}

LLLoadedCallbackEntry::~LLLoadedCallbackEntry()
{
}

void LLLoadedCallbackEntry::removeTexture(LLViewerFetchedTexture* tex)
{
    if (mSourceCallbackList && tex)
    {
        mSourceCallbackList->erase(LLTextureKey(tex->getID(), (ETexListType)tex->getTextureListType()));
    }
}

//static
void LLLoadedCallbackEntry::cleanUpCallbackList(LLLoadedCallbackEntry::source_callback_list_t* callback_list)
{
    //clear texture callbacks.
    if(callback_list && !callback_list->empty())
    {
        for(LLLoadedCallbackEntry::source_callback_list_t::iterator iter = callback_list->begin();
                iter != callback_list->end(); ++iter)
        {
            LLViewerFetchedTexture* tex = gTextureList.findImage(*iter);
            if(tex)
            {
                tex->deleteCallbackEntry(callback_list);
            }
        }
        callback_list->clear();
    }
}

LLViewerMediaTexture* LLViewerTextureManager::createMediaTexture(const LLUUID &media_id, bool usemipmaps, LLTextureData* gl_image)
{
    return new LLViewerMediaTexture(media_id, usemipmaps, gl_image);
}

void LLViewerTextureManager::findFetchedTextures(const LLUUID& id, std::vector<LLViewerFetchedTexture*> &output)
{
    return gTextureList.findTexturesByID(id, output);
}

void  LLViewerTextureManager::findTextures(const LLUUID& id, std::vector<LLViewerTexture*> &output)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    std::vector<LLViewerFetchedTexture*> fetched_output;
    gTextureList.findTexturesByID(id, fetched_output);
    std::vector<LLViewerFetchedTexture*>::iterator iter = fetched_output.begin();
    while (iter != fetched_output.end())
    {
        output.push_back(*iter);
        iter++;
    }

    //search media texture list
    if (output.empty())
    {
        LLViewerTexture* tex;
        tex = LLViewerTextureManager::findMediaTexture(id);
        if (tex)
        {
            output.push_back(tex);
        }
    }

}

LLViewerFetchedTexture* LLViewerTextureManager::findFetchedTexture(const LLUUID& id, S32 tex_type)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    return gTextureList.findImage(id, (ETexListType)tex_type);
}

LLViewerMediaTexture* LLViewerTextureManager::findMediaTexture(const LLUUID &media_id)
{
    return LLViewerMediaTexture::findMediaTexture(media_id);
}

LLViewerMediaTexture*  LLViewerTextureManager::getMediaTexture(const LLUUID& id, bool usemipmaps, LLTextureData* gl_image)
{
    LLViewerMediaTexture* tex = LLViewerMediaTexture::findMediaTexture(id);
    if(!tex)
    {
        tex = LLViewerTextureManager::createMediaTexture(id, usemipmaps, gl_image);
    }

    tex->initVirtualSize();

    return tex;
}

LLViewerFetchedTexture* LLViewerTextureManager::staticCastToFetchedTexture(LLTexture* tex, bool report_error)
{
    if(!tex)
    {
        return NULL;
    }

    S8 type = tex->getType();
    if(type == LLViewerTexture::FETCHED_TEXTURE || type == LLViewerTexture::LOD_TEXTURE)
    {
        return static_cast<LLViewerFetchedTexture*>(tex);
    }

    if(report_error)
    {
        LL_ERRS() << "not a fetched texture type: " << type << LL_ENDL;
    }

    return NULL;
}

LLPointer<LLViewerTexture> LLViewerTextureManager::getLocalTexture(bool usemipmaps, bool generate_gl_tex)
{
    LLPointer<LLViewerTexture> tex = new LLViewerTexture(usemipmaps);
    if(generate_gl_tex)
    {
        tex->generateTextureData();
        tex->setCategory(LLGLTexture::LOCAL);
    }
    return tex;
}
LLPointer<LLViewerTexture> LLViewerTextureManager::getLocalTexture(const LLUUID& id, bool usemipmaps, bool generate_gl_tex)
{
    LLPointer<LLViewerTexture> tex = new LLViewerTexture(id, usemipmaps);
    if(generate_gl_tex)
    {
        tex->generateTextureData();
        tex->setCategory(LLGLTexture::LOCAL);
    }
    return tex;
}
LLPointer<LLViewerTexture> LLViewerTextureManager::getLocalTexture(const LLImageRaw* raw, bool usemipmaps)
{
    LLPointer<LLViewerTexture> tex = new LLViewerTexture(raw, usemipmaps);
    tex->setCategory(LLGLTexture::LOCAL);
    return tex;
}
LLPointer<LLViewerTexture> LLViewerTextureManager::getLocalTexture(const U32 width, const U32 height, const U8 components, bool usemipmaps, bool generate_gl_tex)
{
    LLPointer<LLViewerTexture> tex = new LLViewerTexture(width, height, components, usemipmaps);
    if(generate_gl_tex)
    {
        tex->generateTextureData();
        tex->setCategory(LLGLTexture::LOCAL);
    }
    return tex;
}

LLViewerFetchedTexture* LLViewerTextureManager::getFetchedTexture(const LLImageRaw* raw, FTType type, bool usemipmaps)
{
    LLImageDataSharedLock lock(raw);
    LLViewerFetchedTexture* ret = new LLViewerFetchedTexture(raw, type, usemipmaps);
    gTextureList.addImage(ret, TEX_LIST_STANDARD);
    return ret;
}

LLViewerFetchedTexture* LLViewerTextureManager::getFetchedTexture(
                                                   const LLUUID &image_id,
                                                   FTType f_type,
                                                   bool usemipmaps,
                                                   LLViewerTexture::EBoostLevel boost_priority,
                                                   S8 texture_type,
                                                   LLPixelFormat internal_format,
                                                   LLPixelFormat primary_format,
                                                   LLHost request_from_host)
{
    return gTextureList.getImage(image_id, f_type, usemipmaps, boost_priority, texture_type, internal_format, primary_format, request_from_host);
}

LLViewerFetchedTexture* LLViewerTextureManager::getFetchedTextureFromFile(
                                                   const std::string& filename,
                                                   FTType f_type,
                                                   bool usemipmaps,
                                                   LLViewerTexture::EBoostLevel boost_priority,
                                                   S8 texture_type,
                                                   LLPixelFormat internal_format,
                                                   LLPixelFormat primary_format,
                                                   const LLUUID& force_id)
{
    return gTextureList.getImageFromFile(filename, f_type, usemipmaps, boost_priority, texture_type, internal_format, primary_format, force_id);
}

//static
LLViewerFetchedTexture* LLViewerTextureManager::getFetchedTextureFromUrl(const std::string& url,
                                     FTType f_type,
                                     bool usemipmaps,
                                     LLViewerTexture::EBoostLevel boost_priority,
                                     S8 texture_type,
                                     LLPixelFormat internal_format,
                                     LLPixelFormat primary_format,
                                     const LLUUID& force_id
                                     )
{
    return gTextureList.getImageFromUrl(url, f_type, usemipmaps, boost_priority, texture_type, internal_format, primary_format, force_id);
}

//static
LLImageRaw* LLViewerTextureManager::getRawImageFromMemory(const U8* data, U32 size, std::string_view mimetype)
{
    return gTextureList.getRawImageFromMemory(data, size, mimetype);
}

//static
LLViewerFetchedTexture* LLViewerTextureManager::getFetchedTextureFromMemory(const U8* data, U32 size, std::string_view mimetype)
{
    return gTextureList.getImageFromMemory(data, size, mimetype);
}

LLViewerFetchedTexture* LLViewerTextureManager::getFetchedTextureFromHost(const LLUUID& image_id, FTType f_type, LLHost host)
{
    return gTextureList.getImageFromHost(image_id, f_type, host);
}

// Create a bridge to the viewer texture manager.
class LLViewerTextureManagerBridge : public LLTextureManagerBridge
{
    /*virtual*/ LLPointer<LLGLTexture> getLocalTexture(bool usemipmaps = true, bool generate_gl_tex = true)
    {
        return LLViewerTextureManager::getLocalTexture(usemipmaps, generate_gl_tex);
    }

    /*virtual*/ LLPointer<LLGLTexture> getLocalTexture(const U32 width, const U32 height, const U8 components, bool usemipmaps, bool generate_gl_tex = true)
    {
        return LLViewerTextureManager::getLocalTexture(width, height, components, usemipmaps, generate_gl_tex);
    }

    /*virtual*/ LLGLTexture* getFetchedTexture(const LLUUID &image_id)
    {
        return LLViewerTextureManager::getFetchedTexture(image_id);
    }
};


void LLViewerTextureManager::init()
{
    {
        LLPointer<LLImageRaw> raw = new LLImageRaw(1,1,3);
        raw->clear(0x77, 0x77, 0x77, 0xFF);
        LLViewerTexture::sNullImagep = LLViewerTextureManager::getLocalTexture(raw.get(), true);
    }

    const S32 dim = 128;
    LLPointer<LLImageRaw> image_raw = new LLImageRaw(dim,dim,3);
    U8* data = image_raw->getData();

    memset(data, 0, dim * dim * 3);
    LLViewerTexture::sBlackImagep = LLViewerTextureManager::getLocalTexture(image_raw.get(), true);

#if 1
    LLPointer<LLViewerFetchedTexture> imagep = LLViewerTextureManager::getFetchedTexture(IMG_DEFAULT);
    LLViewerFetchedTexture::sDefaultImagep = imagep;

    for (S32 i = 0; i<dim; i++)
    {
        for (S32 j = 0; j<dim; j++)
        {
#if 0
            const S32 border = 2;
            if (i<border || j<border || i>=(dim-border) || j>=(dim-border))
            {
                *data++ = 0xff;
                *data++ = 0xff;
                *data++ = 0xff;
            }
            else
#endif
            {
                *data++ = 0x7f;
                *data++ = 0x7f;
                *data++ = 0x7f;
            }
        }
    }
    imagep->setFromRaw(0, image_raw);
    image_raw = NULL;
#else
    LLViewerFetchedTexture::sDefaultImagep = LLViewerTextureManager::getFetchedTexture(IMG_DEFAULT, true, LLGLTexture::BOOST_UI);
#endif
    LLViewerFetchedTexture::sDefaultImagep->dontDiscard();
    LLViewerFetchedTexture::sDefaultImagep->setCategory(LLGLTexture::OTHER);

    image_raw = new LLImageRaw(32,32,3);
    data = image_raw->getData();

    for (S32 i = 0; i < (32*32*3); i+=3)
    {
        S32 x = (i % (32*3)) / (3*16);
        S32 y = i / (32*3*16);
        U8 color = ((x + y) % 2) * 255;
        data[i] = color;
        data[i+1] = color;
        data[i+2] = color;
    }

    LLViewerTexture::sCheckerBoardImagep = LLViewerTextureManager::getLocalTexture(image_raw.get(), true);

    LLViewerTexture::initClass();

    // Create a texture manager bridge.
    gTextureManagerBridgep = new LLViewerTextureManagerBridge;

    if (LLMetricPerformanceTesterBasic::isMetricLogRequested(sTesterName) && !LLMetricPerformanceTesterBasic::getTester(sTesterName))
    {
        sTesterp = new LLTexturePipelineTester();
        if (!sTesterp->isValid())
        {
            delete sTesterp;
            sTesterp = NULL;
        }
    }
}

void LLViewerTextureManager::cleanup()
{

    delete gTextureManagerBridgep;
    gTextureManagerBridgep = NULL;
    LLViewerTexture::sNullImagep = NULL;
    LLViewerTexture::sBlackImagep = NULL;
    LLViewerTexture::sCheckerBoardImagep = NULL;
    LLViewerFetchedTexture::sDefaultImagep = NULL;
    LLViewerFetchedTexture::sSmokeImagep = NULL;
    LLViewerFetchedTexture::sMissingAssetImagep = NULL;
    LLViewerFetchedTexture::sWhiteImagep = NULL;

    LLViewerFetchedTexture::sFlatNormalImagep = NULL;
    LLViewerFetchedTexture::sDefaultIrradiancePBRp = NULL;
    LLViewerFetchedTexture::sDefaultParticleImagep = NULL;
    LLViewerFetchedTexture::sDefaultDiffuseImagep = NULL;

    LLViewerMediaTexture::cleanUpClass();
}

//----------------------------------------------------------------------------------------------
//----------------------------------------------------------------------------------------------
//start of LLViewerTexture
//----------------------------------------------------------------------------------------------
// static
void LLViewerTexture::initClass()
{
}

//static
void LLViewerTexture::updateClass()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    sCurrentTime = gFrameTimeSeconds;

    LLTexturePipelineTester* tester = (LLTexturePipelineTester*)LLMetricPerformanceTesterBasic::getTester(sTesterName);
    if (tester)
    {
        tester->update();
    }

    LLViewerMediaTexture::updateClass();

    // sDesiredDiscardBias stays pinned at 1: LL's VRAM-pressure ramp (fed by the GL
    // texture / vertex-buffer byte counters) added a resolution penalty to every
    // texture and capped asset decode at coarse discard. Only the system-memory
    // emergency purge survives: on entering low memory, re-prioritise every texture
    // so off-screen ones drop their data.
    static bool was_low = false;
    const bool is_low = isSystemMemoryLow();
    if (is_low && !was_low)
    {
        LL_WARNS() << "Low system memory detected, emergency downrezzing off screen textures" << LL_ENDL;
        for (auto& image : gTextureList)
        {
            gTextureList.updateImageDecodePriority(image, false /*will modify gTextureList otherwise!*/);
        }
    }
    was_low = is_low;

    LLViewerTexture::sFreezeImageUpdates = false;
}

//static
U32Megabytes LLViewerTexture::getFreeSystemMemory()
{
    static LLFrameTimer timer;
    static U32Megabytes physical_res = U32Megabytes(U32_MAX);

    if (timer.getElapsedTimeF32() < MEMORY_CHECK_WAIT_TIME) //call this once per second.
    {
        return physical_res;
    }

    timer.reset();

    LLMemory::updateMemoryInfo();
    physical_res = LLMemory::getAvailableMemKB();
    return physical_res;
}

//static
bool LLViewerTexture::isSystemMemoryLow()
{
    static LLCachedControl<U32> min_free_main_memory(gSavedSettings, "RenderMinFreeMainMemoryThreshold", 512);
    const U32Megabytes MIN_FREE_MAIN_MEMORY(min_free_main_memory);
    return getFreeSystemMemory() < MIN_FREE_MAIN_MEMORY;
}

//end of static functions
//-------------------------------------------------------------------------------------------
const U32 LLViewerTexture::sCurrentFileVersion = 1;

LLViewerTexture::LLViewerTexture(bool usemipmaps) :
    LLGLTexture(usemipmaps)
{
    init(true);

    mID.generate();
    sImageCount++;
}

LLViewerTexture::LLViewerTexture(const LLUUID& id, bool usemipmaps) :
    LLGLTexture(usemipmaps),
    mID(id)
{
    init(true);

    sImageCount++;
}

LLViewerTexture::LLViewerTexture(const U32 width, const U32 height, const U8 components, bool usemipmaps)  :
    LLGLTexture(width, height, components, usemipmaps)
{
    init(true);

    mID.generate();
    sImageCount++;
}

LLViewerTexture::LLViewerTexture(const LLImageRaw* raw, bool usemipmaps) :
    LLGLTexture(raw, usemipmaps)
{
    init(true);

    mID.generate();
    sImageCount++;
}

LLViewerTexture::~LLViewerTexture()
{
    // LL_DEBUGS("Avatar") << mID << LL_ENDL;
    cleanup();
    sImageCount--;
}

// virtual
void LLViewerTexture::init(bool firstinit)
{
    mMaxVirtualSize = 0.f;
    mMaxVirtualSizeResetInterval = 1;
    mMaxVirtualSizeResetCounter = mMaxVirtualSizeResetInterval;
    mParcelMedia = NULL;

    memset(&mNumVolumes, 0, sizeof(U32)* LLRender::NUM_VOLUME_TEXTURE_CHANNELS);
    mVolumeList[LLRender::LIGHT_TEX].clear();
    mVolumeList[LLRender::SCULPT_TEX].clear();

    for (U32 i = 0; i < LLRender::NUM_TEXTURE_CHANNELS; i++)
    {
        mNumFaces[i] = 0;
        mFaceList[i].clear();
    }

}

//virtual
S8 LLViewerTexture::getType() const
{
    return LLViewerTexture::LOCAL_TEXTURE;
}

void LLViewerTexture::cleanup()
{
    if (LLAppViewer::getTextureFetch())
    {
        LLAppViewer::getTextureFetch()->updateRequestPriority(mID, 0.f);
    }

    mFaceList[LLRender::DIFFUSE_MAP].clear();
    mFaceList[LLRender::NORMAL_MAP].clear();
    mFaceList[LLRender::SPECULAR_MAP].clear();
    mVolumeList[LLRender::LIGHT_TEX].clear();
    mVolumeList[LLRender::SCULPT_TEX].clear();
}

// virtual
void LLViewerTexture::dump()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    LLGLTexture::dump();

    LL_INFOS() << "LLViewerTexture"
            << " mID " << mID
            << LL_ENDL;
}

void LLViewerTexture::setBoostLevel(S32 level)
{
    if(mBoostLevel != level)
    {
        mBoostLevel = level;
        if(mBoostLevel != LLViewerTexture::BOOST_NONE &&
            mBoostLevel != LLViewerTexture::BOOST_SELECTED &&
            mBoostLevel != LLViewerTexture::BOOST_ICON &&
            mBoostLevel != LLViewerTexture::BOOST_THUMBNAIL)
        {
            setNoDelete();
        }
    }

    // strongly encourage anything boosted to load at full res
    if (mBoostLevel >= LLViewerTexture::BOOST_HIGH)
    {
        mMaxVirtualSize = 2048.f * 2048.f;
    }
}

bool LLViewerTexture::isActiveFetching()
{
    return false;
}

bool LLViewerTexture::bindDebugImage(const S32 stage)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    if (stage < 0) return false;

    bool res = true;
    if (LLViewerTexture::sCheckerBoardImagep.notNull() && (this != LLViewerTexture::sCheckerBoardImagep.get()))
    {
        res = gGL.getTexUnit(stage)->bind(LLViewerTexture::sCheckerBoardImagep);
    }

    if(!res)
    {
        return bindDefaultImage(stage);
    }

    return res;
}

bool LLViewerTexture::bindDefaultImage(S32 stage)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    if (stage < 0) return false;

    bool res = true;
    if (LLViewerFetchedTexture::sDefaultImagep.notNull() && (this != LLViewerFetchedTexture::sDefaultImagep.get()))
    {
        // use default if we've got it
        res = gGL.getTexUnit(stage)->bind(LLViewerFetchedTexture::sDefaultImagep);
    }
    if (!res && LLViewerTexture::sNullImagep.notNull() && (this != LLViewerTexture::sNullImagep))
    {
        res = gGL.getTexUnit(stage)->bind(LLViewerTexture::sNullImagep);
    }
    if (!res)
    {
        LL_WARNS() << "LLViewerTexture::bindDefaultImage failed." << LL_ENDL;
    }

    LLTexturePipelineTester* tester = (LLTexturePipelineTester*)LLMetricPerformanceTesterBasic::getTester(sTesterName);
    if (tester)
    {
        tester->updateGrayTextureBinding();
    }
    return res;
}

//virtual
bool LLViewerTexture::isMissingAsset()const
{
    return false;
}

//virtual
void LLViewerTexture::forceImmediateUpdate()
{
}

void LLViewerTexture::addTextureStats(F32 virtual_size, bool needs_gltexture) const
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    if(needs_gltexture)
    {
        mNeedsGLTexture = true;
    }

    virtual_size = llmin(virtual_size, LLViewerFetchedTexture::sMaxVirtualSize);

    if (virtual_size > mMaxVirtualSize)
    {
        mMaxVirtualSize = virtual_size;
    }
}

void LLViewerTexture::resetTextureStats()
{
    mMaxVirtualSize = 0.0f;
    mMaxVirtualSizeResetCounter = 0;
}

//virtual
F32 LLViewerTexture::getMaxVirtualSize()
{
    return mMaxVirtualSize;
}

//virtual
void LLViewerTexture::setKnownDrawSize(S32 width, S32 height)
{
    //nothing here.
}

//virtual
void LLViewerTexture::addFace(U32 ch, LLFace* facep)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    llassert(ch < LLRender::NUM_TEXTURE_CHANNELS);

    if(mNumFaces[ch] >= mFaceList[ch].size())
    {
        mFaceList[ch].resize(2 * mNumFaces[ch] + 1);
    }
    mFaceList[ch][mNumFaces[ch]] = facep;
    facep->setIndexInTex(ch, mNumFaces[ch]);
    mNumFaces[ch]++;
    mLastFaceListUpdateTimer.reset();
}

//virtual
void LLViewerTexture::removeFace(U32 ch, LLFace* facep)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    llassert(ch < LLRender::NUM_TEXTURE_CHANNELS);

    if(mNumFaces[ch] > 1)
    {
        S32 index = facep->getIndexInTex(ch);
        llassert(index < (S32)mFaceList[ch].size());
        llassert(index < (S32)mNumFaces[ch]);
        mFaceList[ch][index] = mFaceList[ch][--mNumFaces[ch]];
        mFaceList[ch][index]->setIndexInTex(ch, index);
    }
    else
    {
        mFaceList[ch].clear();
        mNumFaces[ch] = 0;
    }
    mLastFaceListUpdateTimer.reset();
}

S32 LLViewerTexture::getTotalNumFaces() const
{
    S32 ret = 0;

    for (U32 i = 0; i < LLRender::NUM_TEXTURE_CHANNELS; ++i)
    {
        ret += mNumFaces[i];
    }

    return ret;
}

S32 LLViewerTexture::getNumFaces(U32 ch) const
{
    llassert(ch < LLRender::NUM_TEXTURE_CHANNELS);
    return ch < LLRender::NUM_TEXTURE_CHANNELS ? mNumFaces[ch] : 0;
}


//virtual
void LLViewerTexture::addVolume(U32 ch, LLVOVolume* volumep)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    if (mNumVolumes[ch] >= mVolumeList[ch].size())
    {
        mVolumeList[ch].resize(2 * mNumVolumes[ch] + 1);
    }
    mVolumeList[ch][mNumVolumes[ch]] = volumep;
    volumep->setIndexInTex(ch, mNumVolumes[ch]);
    mNumVolumes[ch]++;
    mLastVolumeListUpdateTimer.reset();
}

//virtual
void LLViewerTexture::removeVolume(U32 ch, LLVOVolume* volumep)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    if (mNumVolumes[ch] > 1)
    {
        S32 index = volumep->getIndexInTex(ch);
        llassert(index < (S32)mVolumeList[ch].size());
        llassert(index < (S32)mNumVolumes[ch]);
        mVolumeList[ch][index] = mVolumeList[ch][--mNumVolumes[ch]];
        mVolumeList[ch][index]->setIndexInTex(ch, index);
    }
    else
    {
        mVolumeList[ch].clear();
        mNumVolumes[ch] = 0;
    }
    mLastVolumeListUpdateTimer.reset();
}

S32 LLViewerTexture::getNumVolumes(U32 ch) const
{
    return mNumVolumes[ch];
}

void LLViewerTexture::reorganizeFaceList()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    static const F32 MAX_WAIT_TIME = 20.f; // seconds
    static const U32 MAX_EXTRA_BUFFER_SIZE = 4;

    if(mLastFaceListUpdateTimer.getElapsedTimeF32() < MAX_WAIT_TIME)
    {
        return;
    }

    for (U32 i = 0; i < LLRender::NUM_TEXTURE_CHANNELS; ++i)
    {
        if(mNumFaces[i] + MAX_EXTRA_BUFFER_SIZE > mFaceList[i].size())
    {
        return;
    }

        mFaceList[i].erase(mFaceList[i].begin() + mNumFaces[i], mFaceList[i].end());
    }

    mLastFaceListUpdateTimer.reset();
}

void LLViewerTexture::reorganizeVolumeList()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    static const F32 MAX_WAIT_TIME = 20.f; // seconds
    static const U32 MAX_EXTRA_BUFFER_SIZE = 4;


    for (U32 i = 0; i < LLRender::NUM_VOLUME_TEXTURE_CHANNELS; ++i)
    {
        if (mNumVolumes[i] + MAX_EXTRA_BUFFER_SIZE > mVolumeList[i].size())
        {
            return;
        }
    }

    if(mLastVolumeListUpdateTimer.getElapsedTimeF32() < MAX_WAIT_TIME)
    {
        return;
    }

    mLastVolumeListUpdateTimer.reset();
    for (U32 i = 0; i < LLRender::NUM_VOLUME_TEXTURE_CHANNELS; ++i)
    {
        mVolumeList[i].erase(mVolumeList[i].begin() + mNumVolumes[i], mVolumeList[i].end());
    }
}

bool LLViewerTexture::isLargeImage()
{
    return  (S32)mTexelsPerImage > LLViewerTexture::sMinLargeImageSize;
}

//virtual
void LLViewerTexture::updateBindStatsForTester()
{
    LLTexturePipelineTester* tester = (LLTexturePipelineTester*)LLMetricPerformanceTesterBasic::getTester(sTesterName);
    if (tester)
    {
        tester->updateTextureBindingStats(this);
    }
}

//----------------------------------------------------------------------------------------------
//end of LLViewerTexture
//----------------------------------------------------------------------------------------------

const std::string& fttype_to_string(const FTType& fttype)
{
    static const std::string ftt_unknown("FTT_UNKNOWN");
    static const std::string ftt_default("FTT_DEFAULT");
    static const std::string ftt_server_bake("FTT_SERVER_BAKE");
    static const std::string ftt_host_bake("FTT_HOST_BAKE");
    static const std::string ftt_map_tile("FTT_MAP_TILE");
    static const std::string ftt_local_file("FTT_LOCAL_FILE");
    static const std::string ftt_error("FTT_ERROR");
    switch(fttype)
    {
        case FTT_UNKNOWN: return ftt_unknown; break;
        case FTT_DEFAULT: return ftt_default; break;
        case FTT_SERVER_BAKE: return ftt_server_bake; break;
        case FTT_HOST_BAKE: return ftt_host_bake; break;
        case FTT_MAP_TILE: return ftt_map_tile; break;
        case FTT_LOCAL_FILE: return ftt_local_file; break;
    }
    return ftt_error;
}

//----------------------------------------------------------------------------------------------
//start of LLViewerFetchedTexture
//----------------------------------------------------------------------------------------------

//static
LLViewerFetchedTexture* LLViewerFetchedTexture::getSmokeImage()
{
    if (sSmokeImagep.isNull())
    {
        sSmokeImagep = LLViewerTextureManager::getFetchedTexture(IMG_SMOKE);
    }

    sSmokeImagep->addTextureStats(1024.f * 1024.f);

    return sSmokeImagep;
}

LLViewerFetchedTexture::LLViewerFetchedTexture(const LLUUID& id, FTType f_type, const LLHost& host, bool usemipmaps)
    : LLViewerTexture(id, usemipmaps),
    mTargetHost(host)
{
    init(true);
    mFTType = f_type;
    if (mFTType == FTT_HOST_BAKE)
    {
        LL_WARNS() << "Unsupported fetch type " << mFTType << LL_ENDL;
    }
    generateTextureData();
}

LLViewerFetchedTexture::LLViewerFetchedTexture(const LLImageRaw* raw, FTType f_type, bool usemipmaps)
    : LLViewerTexture(raw, usemipmaps)
{
    init(true);
    mFTType = f_type;
    mVKFromRaw = true;   // #707: no fetch source -- its copy is never dropped
}

LLViewerFetchedTexture::LLViewerFetchedTexture(const std::string& url, FTType f_type, const LLUUID& id, bool usemipmaps)
    : LLViewerTexture(id, usemipmaps),
    mUrl(url)
{
    init(true);
    mFTType = f_type;
    generateTextureData();
}

void LLViewerFetchedTexture::init(bool firstinit)
{
    mOrigWidth = 0;
    mOrigHeight = 0;
    mHasAux = false;
    mNeedsAux = false;
    mLastWorkerDiscardLevel = -1;
    mRequestedDiscardLevel = -1;
    mRequestedDownloadPriority = 0.f;
    mFullyLoaded = false;
    mCanUseHTTP = true;
    mDesiredDiscardLevel = MAX_DISCARD_LEVEL + 1;
    mMinDesiredDiscardLevel = MAX_DISCARD_LEVEL + 1;

    mDecodingAux = false;

    mKnownDrawWidth = 0;
    mKnownDrawHeight = 0;
    mKnownDrawSizeChanged = false;

    if (firstinit)
    {
        mInImageList = 0;
    }

    // Only set mIsMissingAsset true when we know for certain that the database
    // does not contain this image.
    mIsMissingAsset = false;

    mLoadedCallbackDesiredDiscardLevel = S8_MAX;
    mPauseLoadedCallBacks = false;

    mNeedsCreateTexture = false;

    mIsRawImageValid = false;
    mRawDiscardLevel = INVALID_DISCARD_LEVEL;
    mMinDiscardLevel = 0;

    mHasFetcher = false;
    mIsFetching = false;
    mFetchState = 0;
    mFetchPriority = 0;
    mDownloadProgress = 0.f;
    mFetchDeltaTime = 999999.f;
    mRequestDeltaTime = 0.f;
    mForSculpt = false;
    mIsFetched = false;

    mSavedRawImage = NULL;
    mForceToSaveRawImage  = false;
    mSaveRawImage = false;
    mSavedRawDiscardLevel = -1;
    mDesiredSavedRawDiscardLevel = -1;
    mLastReferencedSavedRawImageTime = 0.0f;
    mKeptSavedRawImageTime = 0.f;
    mLastCallBackActiveTime = 0.f;
    mForceCallbackFetch = false;

    mFTType = FTT_UNKNOWN;
}

LLViewerFetchedTexture::~LLViewerFetchedTexture()
{
    assert_main_thread();
    if (mNetGated && sNetGateGated > 0) --sNetGateGated;   // #485
    //*NOTE getTextureFetch can return NULL when Viewer is shutting down.
    // This is due to LLWearableList is singleton and is destroyed after
    // LLAppViewer::cleanup() was called. (see ticket EXT-177)
    if (mHasFetcher && LLAppViewer::getTextureFetch())
    {
        LLAppViewer::getTextureFetch()->deleteRequest(getID(), true);
    }
    cleanup();
}

//virtual
S8 LLViewerFetchedTexture::getType() const
{
    return LLViewerTexture::FETCHED_TEXTURE;
}

FTType LLViewerFetchedTexture::getFTType() const
{
    return mFTType;
}

void LLViewerFetchedTexture::cleanup()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    for(callback_list_t::iterator iter = mLoadedCallbackList.begin();
        iter != mLoadedCallbackList.end(); )
    {
        LLLoadedCallbackEntry *entryp = *iter++;
        // We never finished loading the image.  Indicate failure.
        // Note: this allows mLoadedCallbackUserData to be cleaned up.
        entryp->mCallback( false, this, NULL, NULL, 0, true, entryp->mUserData );
        entryp->removeTexture(this);
        delete entryp;
    }
    mLoadedCallbackList.clear();
    mNeedsAux = false;

    // Clean up image data
    destroyRawImage();
    mSavedRawImage = NULL;
    mSavedRawDiscardLevel = -1;
}

void LLViewerFetchedTexture::setForSculpt()
{
    static const S32 MAX_INTERVAL = 8; //frames

    forceToSaveRawImage(0, F32_MAX);

    setBoostLevel(llmax((S32)getBoostLevel(),
        (S32)LLGLTexture::BOOST_SCULPTED));

    mForSculpt = true;
    setMaxVirtualSizeResetInterval(MAX_INTERVAL);
}

bool LLViewerFetchedTexture::isForSculptOnly() const
{
    return mForSculpt && !mNeedsGLTexture;
}

bool LLViewerFetchedTexture::isDeleted()
{
    return mTextureState == DELETED;
}


bool LLViewerFetchedTexture::isFullyLoaded() const
{
    // Unfortunately, the boolean "mFullyLoaded" is never updated correctly so we use that logic
    // to check if the texture is there and completely downloaded
    return (mFullWidth != 0) && (mFullHeight != 0) && !mIsFetching && !mHasFetcher;
}


// virtual
void LLViewerFetchedTexture::dump()
{
    LLViewerTexture::dump();

    LL_INFOS() << "Dump : " << mID
            << ", mIsMissingAsset = " << (S32)mIsMissingAsset
            << ", mFullWidth = " << (S32)mFullWidth
            << ", mFullHeight = " << (S32)mFullHeight
            << ", mOrigWidth = " << (S32)mOrigWidth
            << ", mOrigHeight = " << (S32)mOrigHeight
            << LL_ENDL;
    LL_INFOS() << "     : "
            << " mFullyLoaded = " << (S32)mFullyLoaded
            << ", mFetchState = " << (S32)mFetchState
            << ", mFetchPriority = " << (S32)mFetchPriority
            << ", mDownloadProgress = " << (F32)mDownloadProgress
            << LL_ENDL;
    LL_INFOS() << "     : "
            << " mHasFetcher = " << (S32)mHasFetcher
            << ", mIsFetching = " << (S32)mIsFetching
            << ", mIsFetched = " << (S32)mIsFetched
            << ", mBoostLevel = " << (S32)mBoostLevel
            << LL_ENDL;
}

///////////////////////////////////////////////////////////////////////////////
// ONLY called from LLViewerFetchedTextureList
void LLViewerFetchedTexture::destroyTexture()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;

    if (mNeedsCreateTexture)//return if in the process of generating a new texture.
    {
        return;
    }

    //LL_DEBUGS("Avatar") << mID << LL_ENDL;
    mFullyLoaded = false;
}

void LLViewerFetchedTexture::addToCreateTexture()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    bool force_update = false;
    if (getComponents() != mRawImage->getComponents())
    {
        // We've changed the number of components, so we need to move any
        // objects using this pool to a different pool.
        mComponents = mRawImage->getComponents();
        mTextureData->setComponents(mComponents);
        force_update = true;

        for (U32 j = 0; j < LLRender::NUM_TEXTURE_CHANNELS; ++j)
        {
            llassert(mNumFaces[j] <= mFaceList[j].size());

            for(U32 i = 0; i < mNumFaces[j]; i++)
            {
                mFaceList[j][i]->dirtyTexture();
            }
        }

        mSavedRawDiscardLevel = -1;
        mSavedRawImage = NULL;
    }

    if(isForSculptOnly())
    {
        //just update some variables, not to create a real GL texture.
        // (#707: through the image, not LLGLTexture::setFromRaw -- a fetched decode, not
        // injected pixels, so the copy stays droppable)
        if (mTextureData->setFromRaw(mRawDiscardLevel, mRawImage))
        {
            mFullWidth = mTextureData->getCurrentWidth();
            mFullHeight = mTextureData->getCurrentHeight();
            mComponents = mTextureData->getComponents();
            setTexelsPerImage();
        }
        mNeedsCreateTexture = false;
        destroyRawImage();
    }
    // (#707: a dropped CPU copy keeps naming the level the GPU holds; a decode at that
    // level is the re-source and must land)
    else if(!force_update && getDiscardLevel() > -1 && getDiscardLevel() <= mRawDiscardLevel
            && !mTextureData->isVKCpuDropped())
    {
        mNeedsCreateTexture = false;
        destroyRawImage();
    }
    else
    {
        scheduleCreateTexture();
    }
    return;
}

// ONLY called from LLViewerTextureList
bool LLViewerFetchedTexture::preCreateTexture(S32 usename/*= 0*/)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
#if LL_IMAGEGL_THREAD_CHECK
    mTextureData->checkActiveThread();
#endif

    if (!mNeedsCreateTexture)
    {
        destroyRawImage();
        return false;
    }
    mNeedsCreateTexture = false;

    if (mRawImage.isNull())
    {
        LL_ERRS() << "LLViewerTexture trying to create texture with no Raw Image" << LL_ENDL;
    }
    if (mRawImage->isBufferInvalid())
    {
        LL_WARNS() << "Can't create a texture: invalid image data" << LL_ENDL;
        destroyRawImage();
        return false;
    }
    //  LL_INFOS() << llformat("IMAGE Creating (%d) [%d x %d] Bytes: %d ",
    //                      mRawDiscardLevel,
    //                      mRawImage->getWidth(), mRawImage->getHeight(),mRawImage->getDataSize())
    //          << mID.getString() << LL_ENDL;
    bool res = true;

    // store original size only for locally-sourced images
    if (mUrl.compare(0, 7, "file://") == 0)
    {
        mOrigWidth = mRawImage->getWidth();
        mOrigHeight = mRawImage->getHeight();

        // This is only safe because it's a local image and fetcher doesn't use raw data
        // from local images, but this might become unsafe in case of changes to fetcher
        if (mBoostLevel == BOOST_PREVIEW)
        {
            mRawImage->biasedScaleToPowerOfTwo(1024);
        }
        else
        { // leave black border, do not scale image content
            mRawImage->expandToPowerOfTwo(MAX_IMAGE_SIZE, false);
        }

        mFullWidth = mRawImage->getWidth();
        mFullHeight = mRawImage->getHeight();
        setTexelsPerImage();
    }
    else
    {
        mOrigWidth = mFullWidth;
        mOrigHeight = mFullHeight;
    }

    if (!mRawImage->mComment.empty())
    {
        // a is for uploader
        // z is for time
        // K is the whole thing (just coz)
        std::string comment = mRawImage->getComment();
        mComment['K'] = comment;
        size_t position = 0;
        size_t length = comment.length();
        while (position < length)
        {
            std::size_t equals_position = comment.find('=', position);
            if (equals_position != std::string::npos)
            {
                S8 type = comment.at(equals_position - 1);
                position = comment.find('&', position);
                if (position != std::string::npos)
                {
                    mComment[type] = comment.substr(equals_position + 1, position - (equals_position + 1));
                    position++;
                }
                else
                {
                    mComment[type] = comment.substr(equals_position + 1, length - (equals_position + 1));
                }
            }
            else
            {
                position = equals_position;
            }
        }
    }

    bool size_okay = true;

    S32 discard_level = mRawDiscardLevel;
    if (mRawDiscardLevel < 0)
    {
        LL_DEBUGS() << "Negative raw discard level when creating image: " << mRawDiscardLevel << LL_ENDL;
        discard_level = 0;
    }

    U32 raw_width = mRawImage->getWidth() << discard_level;
    U32 raw_height = mRawImage->getHeight() << discard_level;

    if (raw_width > MAX_IMAGE_SIZE || raw_height > MAX_IMAGE_SIZE)
    {
        LL_INFOS() << "Width or height is greater than " << MAX_IMAGE_SIZE << ": (" << raw_width << "," << raw_height << ")" << LL_ENDL;
        size_okay = false;
    }

    if (!LLTextureData::checkSize(mRawImage->getWidth(), mRawImage->getHeight()))
    {
        // A non power-of-two image was uploaded (through a non standard client)
        LL_INFOS() << "Non power of two width or height: (" << mRawImage->getWidth() << "," << mRawImage->getHeight() << ")" << LL_ENDL;
        size_okay = false;
    }

    if (!size_okay)
    {
        // An inappropriately-sized image was uploaded (through a non standard client)
        // We treat these images as missing assets which causes them to
        // be renderd as 'missing image' and to stop requesting data
        LL_WARNS() << "!size_ok, setting as missing" << LL_ENDL;
        setIsMissingAsset();
        destroyRawImage();
        return false;
    }

    if (mTextureData->getHasExplicitFormat())
    {
        LLPixelFormat format = mTextureData->getPrimaryFormat();
        S8 components = mRawImage->getComponents();
        if ((format == LLPixelFormat::RGBA && components < 4)
            || (format == LLPixelFormat::RGB && components < 3))
        {
            LL_WARNS() << "Can't create a texture " << mID << ": invalid image format " << std::hex << static_cast<U32>(format) << " vs components " << (U32)components << LL_ENDL;
            // Was expecting specific format but raw texture has insufficient components for
            // such format, using such texture will result in crash or will display wrongly
            // if we change format. Texture might be corrupted server side, so just set as
            // missing and clear cashed texture (do not cause reload loop, will retry&recover
            // during new session)
            setIsMissingAsset();
            destroyRawImage();
            LLAppViewer::getTextureCache()->removeFromCache(mID);
            return false;
        }
    }

    return res;
}

bool LLViewerFetchedTexture::createTexture(S32 usename/*= 0*/)
{
    if (!mNeedsCreateTexture)
    {
        return false;
    }

    bool res = mTextureData->setFromRaw(mRawDiscardLevel, mRawImage);

    // Clean-room capture. LL is about to destroyRawImage() in
    // postCreateTexture, so this is our last chance to grab the
    // decoded pixels alongside the asset UUID. capture() copies the
    // bytes, so the later destroyRawImage() is safe. The capture's
    // uploadSlot adopts the GL texture into mTextureData when ownership
    // is on, leaving a single shared GL copy.
    if (res && mRawImage.notNull())
    {
        LLCleanTexture::instance().capture(getID(), mRawImage.get(), mRawDiscardLevel);
    }

    return res;
}

void LLViewerFetchedTexture::postCreateTexture()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    if (!mNeedsCreateTexture)
    {
        return;
    }
#if LL_IMAGEGL_THREAD_CHECK
    mTextureData->checkActiveThread();
#endif

    setActive();

    // rebuild any volumes that are using this texture for sculpts in case their LoD has changed
    for (U32 i = 0; i < mNumVolumes[LLRender::SCULPT_TEX]; ++i)
    {
        LLVOVolume* volume = mVolumeList[LLRender::SCULPT_TEX][i];
        if (volume)
        {
            volume->mSculptChanged = true;
            gPipeline.markRebuild(volume->mDrawable);
        }
    }

    if (!needsToSaveRawImage())
    {
        mNeedsAux = false;
    }
    destroyRawImage(); // will save raw image if needed

    mNeedsCreateTexture = false;
}

void LLViewerFetchedTexture::scheduleCreateTexture()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;

    if (!mNeedsCreateTexture)
    {
        mNeedsCreateTexture = true;
        if (preCreateTexture())
        {
            // preCreateTexture() clears mNeedsCreateTexture; createTexture() /
            // postCreateTexture() bail out unless it is set again here.
            mNeedsCreateTexture = true;
            if (!mCreatePending)
            {
                mCreatePending = true;
                gTextureList.mCreateTextureList.push(this);
            }
        }
    }
}

// Call with 0,0 to turn this feature off.
//virtual
void LLViewerFetchedTexture::setKnownDrawSize(S32 width, S32 height)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    // OCOL (Mirror): icons and thumbnails are scaled to this size after decoding, and GL only takes
    // power-of-two sizes, so a size like 80 (64 px at 125% UI) would fail to create the texture
    width = (S32)get_next_power_two((U32)width, MAX_IMAGE_SIZE_DEFAULT);
    height = (S32)get_next_power_two((U32)height, MAX_IMAGE_SIZE_DEFAULT);
    if(mKnownDrawWidth < width || mKnownDrawHeight < height)
    {
        mKnownDrawWidth = llmax(mKnownDrawWidth, width);
        mKnownDrawHeight = llmax(mKnownDrawHeight, height);

        mKnownDrawSizeChanged = true;
        mFullyLoaded = false;
    }
    addTextureStats((F32)(mKnownDrawWidth * mKnownDrawHeight));
}

void LLViewerFetchedTexture::setDebugText(const std::string& text)
{
    for (U32 i = 0; i < LLRender::NUM_TEXTURE_CHANNELS; ++i)
    {
        for (S32 fi = 0; fi < getNumFaces(i); ++fi)
        {
            LLFace* facep = (*(getFaceList(i)))[fi];

            if (facep)
            {
                LLDrawable* drawable = facep->getDrawable();
                if (drawable)
                {
                    drawable->getVObj()->setDebugText(text);
                }
            }
        }
    }
}

//virtual
//----------------------------------------------------------------------------
// VULKAN TEXTURE RESIDENCY (see setVKDemand)

U32 LLViewerFetchedTexture::sVKScaledDown = 0;
U32 LLViewerFetchedTexture::sVKScaleInFlight = 0;
U64 LLViewerFetchedTexture::sVKDropped = 0;
U64 LLViewerFetchedTexture::sVKResourced = 0;
U64 LLViewerFetchedTexture::sVKResourceFailed = 0;

// A demand not refreshed for this long hands the texture back to LL's face-area logic.
constexpr F32 VK_DEMAND_STALE_SEC = 5.f;
// The CPU copy must stay sharper than wanted this long before it is scaled down, so a
// glance away and back doesn't cost a re-decode.
constexpr F32 VK_SCALE_DOWN_DELAY_SEC = 4.f;
constexpr U32 VK_MAX_SCALE_IN_FLIGHT = 32;
// RAM (#159 / #707): once the GPU holds a texture (or nobody has looked at it for a while),
// its CPU copy has no reader: it is dropped (vkCpuCopyStep), and a later need for pixels
// re-decodes from the disk cache like any fetch.
// Off-screen this long (demand stale) before LL's discard is owned by the stale rule.
constexpr F32 VK_STALE_THUMB_SEC = 15.f;

// RAM (#262): when system memory is short, or the retained CPU copies exceed their
// RAM-scaled allowance (the login / teleport burst: 8 GB of copies the GPU already held
// sat on these timers), trim without the comfort delays. The stale-thumb time must stay
// above VK_DEMAND_STALE_SEC (its branch only runs once the demand went stale).
// "Prefer disk cache" (SlipstreamTextureMemoryMode 1, #405) trims as if memory were short,
// always: the GPU-ready disk cache (tier 2) re-sources the pixels when they are needed.
static bool vk_trim_urgent() { return LLViewerFetchedTexture::vkPreferDisk() || LLMemoryGovernor::shouldTrimAggressively(); }

// static
bool LLViewerFetchedTexture::vkPreferDisk()
{
    static LLCachedControl<U32> s_mode(gSavedSettings, "SlipstreamTextureMemoryMode", 0);
    static LLCachedControl<U32> s_below_gb(gSavedSettings, "SlipstreamTextureDiskBelowRAMGB", 24);
    if ((U32)s_mode == 1u) return true;
    if ((U32)s_mode != 0u || !(U32)s_below_gb) return false;
    // Auto on a machine with less RAM than SlipstreamTextureDiskBelowRAMGB behaves as
    // "Prefer disk cache": decoded copies there compete with the rest of the viewer (and,
    // on integrated graphics, with video memory itself). Nominal size: firmware and iGPU
    // carve-outs only ever lower the reported total, so round up to whole GiB.
    const U64 total = LLMemoryGovernor::getTotalRAM();
    if (!total) return false;
    const U64 gib = (total + (1ull << 30) - 1) >> 30;
    return gib < (U64)(U32)s_below_gb;
}

// static
bool LLViewerFetchedTexture::vkKeepTier1()
{
    // TIER 1 (#405): optional CPU copies are kept for the fastest re-upload only while
    // memory is plentiful. The 3/4 band below the governor's allowance keeps this from
    // saw-toothing against shouldTrimAggressively() (which trims above the allowance):
    // between the two, new copies are thumbnailed on the normal timers and kept ones stay.
    // Never a setting change: copies come back on their own as memory frees.
    if (vkPreferDisk() || !LLMemoryGovernor::isEnabled()) return false;
    if (LLMemoryGovernor::getState() != LLMemoryGovernor::COMFORTABLE) return false;
    const U64 retained = (U64)llmax(LLTextureData::getVKCpuBytes(), (S64)0);
    return retained < LLMemoryGovernor::getRetainedAllowance() / 4 * 3;
}

// static
void LLViewerFetchedTexture::getVKTier1Bytes(U64& required, U64& optional, U64* by_reason)
{
    static F32 s_next = -1.f;
    static U64 s_req = 0, s_opt = 0;
    static U64 s_reason[LLTextureData::VKLEASE_COUNT] = {};
    if (sCurrentTime >= s_next)
    {
        s_next = sCurrentTime + 2.f;
        U64 opt = 0;
        U64 reason[LLTextureData::VKLEASE_COUNT] = {};
        for (auto& image : gTextureList)
        {
            const LLViewerFetchedTexture* t = image.get();
            const LLTextureData* gl = t ? t->getTextureData() : nullptr;
            const LLImageRaw* r = gl ? gl->getVKCpuData() : nullptr;
            if (!r || !r->getData()) continue;
            // LEASED (#707): a consumer holds the copy (first reason, in enum order).
            const U32 mask = t->vkCpuLeaseMask();
            const U64 b = (U64)r->getDataSize();
            if (!mask) { opt += b; continue; }
            for (U32 i = 0; i < LLTextureData::VKLEASE_COUNT; ++i)
                if (mask & (1u << i)) { reason[i] += b; break; }
        }
        const U64 total = (U64)llmax(LLTextureData::getVKCpuBytes(), (S64)0);
        s_opt = llmin(opt, total);
        s_req = total - s_opt;
        // (outside the fetched list -- fonts, media, dynamic -- counts as LOCAL)
        U64 listed = 0;
        for (U32 i = 0; i < LLTextureData::VKLEASE_COUNT; ++i) listed += reason[i];
        if (s_req > listed) reason[LLTextureData::VKLEASE_LOCAL] += s_req - listed;
        for (U32 i = 0; i < LLTextureData::VKLEASE_COUNT; ++i) s_reason[i] = reason[i];
    }
    required = s_req;
    optional = s_opt;
    if (by_reason)
        for (U32 i = 0; i < LLTextureData::VKLEASE_COUNT; ++i) by_reason[i] = s_reason[i];
}
static F32 vk_scale_down_delay() { return vk_trim_urgent() ? 0.5f : VK_SCALE_DOWN_DELAY_SEC; }
// #707: an unleased copy waits this long before it is dropped (a glance away and back
// costs nothing), and at most this long for its tier-2 capture (then J2C re-sources it).
static F32 vk_drop_grace() { return vk_trim_urgent() ? 0.5f : 2.f; }
constexpr F32 VK_CAPTURE_WAIT_SEC = 30.f;
// #707: a re-source that has not landed after this long is abandoned (logged; a reader
// asking again starts a new one).
constexpr F32 VK_RESOURCE_GIVE_UP_SEC = 30.f;
static U32 vk_max_scale_in_flight() { return vk_trim_urgent() ? 4 * VK_MAX_SCALE_IN_FLIGHT : VK_MAX_SCALE_IN_FLIGHT; }
static F32 vk_stale_thumb_sec() { return vk_trim_urgent() ? VK_DEMAND_STALE_SEC + 1.f : VK_STALE_THUMB_SEC; }
static F32 vk_idle_thumb_sec() { return vk_trim_urgent() ? 10.f : 30.f; }

std::vector<LLPointer<LLViewerFetchedTexture>> LLViewerFetchedTexture::sVKDirty;

// #485 NETWORK DRAW DISTANCE FETCH GATE (see the header). Owner policy: assets are
// requested only for objects inside the network draw distance HORIZONTALLY; far cached
// objects (render DD 4096 m vs network 512 m / 192 m in fast flight) may render with
// what is already loaded but generate no new fetches.
U64 LLViewerFetchedTexture::sNetGateSkipped = 0;
U64 LLViewerFetchedTexture::sNetGateDropped = 0;
U32 LLViewerFetchedTexture::sNetGateGated = 0;

bool LLViewerFetchedTexture::vkNetGated()
{
    static LLCachedControl<bool> s_on(gSavedSettings, "SLFetchNetDDGate", true);
    static LLCachedControl<F32> s_margin(gSavedSettings, "SLFetchNetDDGateMargin", 16.f);
    auto set_gated = [this](bool g)
    {
        if (g == mNetGated) return;
        mNetGated = g;
        if (g) ++sNetGateGated;
        else if (sNetGateGated > 0) --sNetGateGated;
    };
    // World-object textures only: plain and sculpt maps. Avatar / bake / UI / preview /
    // map / HUD / terrain textures carry other boosts or fetch types.
    if (!s_on || (mBoostLevel != BOOST_NONE && mBoostLevel != BOOST_SCULPTED) || mFTType != FTT_DEFAULT
        || !isAgentAvatarValid() || !gAgent.getRegion())
    {
        set_gated(false);
        return false;
    }
    if (sCurrentTime - mNetGateCheckTime < 1.f)
    {
        return mNetGated;
    }
    mNetGateCheckTime = sCurrentTime;

    // agent X/Y and the network draw distance, once per frame
    static U32 s_frame = ~0u;
    static LLVector3 s_agent;
    static F32 s_dd = 0.f;
    const U32 frame = LLFrameTimer::getFrameCount();
    if (frame != s_frame)
    {
        s_frame = frame;
        s_agent = gAgent.getPositionAgent();
        s_dd = gAgentCamera.getNetworkDrawDistance();
    }
    if (s_dd <= 0.f)
    {
        set_gated(false);
        return false;
    }
    const F32 base = s_dd + llmax((F32)s_margin, 0.f);

    // 1 = a user inside the gate (or exempt), 0 = beyond it, -1 = not a usable user
    auto user_state = [&](LLViewerObject* obj) -> S32
    {
        if (!obj || obj->isDead()) return -1;
        LLVector3 pos;
        F32 radius;
        if (obj->isHUDAttachment()) return 1;   // HUD: exempt
        // avatar, attachment (any prim of it) or animesh: the avatar's position
        LLVOAvatar* av = obj->isAvatar() ? obj->asAvatar() : obj->getAvatar();
        if (!av && obj->isAttachment()) av = obj->getAvatarAncestor();
        if (av)
        {
            if (av->isSelf()) return 1;         // own avatar / attachments: exempt
            pos = av->getPositionAgent();
            radius = 4.f;
        }
        else
        {
            pos = obj->getPositionAgent();
            radius = llmin(obj->getScale().length() * 0.5f, 256.f);
        }
        const F32 dx = pos.mV[VX] - s_agent.mV[VX];
        const F32 dy = pos.mV[VY] - s_agent.mV[VY];
        const F32 lim = base + radius;
        return (dx * dx + dy * dy <= lim * lim) ? 1 : 0;
    };

    constexpr U32 MAX_USERS = 512;   // a texture on more users than this: never gated
    U32 beyond = 0, seen = 0;
    bool in_range = false;
    for (U32 ch = 0; ch < LLRender::NUM_TEXTURE_CHANNELS && !in_range; ++ch)
    {
        for (U32 i = 0; i < mNumFaces[ch] && i < mFaceList[ch].size(); ++i)
        {
            LLFace* face = mFaceList[ch][i];
            const S32 u = face ? user_state(face->getViewerObject()) : -1;
            if (u < 0) continue;
            if (u > 0 || ++seen > MAX_USERS) { in_range = true; break; }
            ++beyond;
        }
    }
    for (U32 ch = 0; ch < LLRender::NUM_VOLUME_TEXTURE_CHANNELS && !in_range; ++ch)
    {
        for (U32 i = 0; i < mNumVolumes[ch] && i < mVolumeList[ch].size(); ++i)
        {
            const S32 u = user_state(mVolumeList[ch][i]);
            if (u < 0) continue;
            if (u > 0 || ++seen > MAX_USERS) { in_range = true; break; }
            ++beyond;
        }
    }
    // gated only when at least one user is known and none is inside the gate
    const bool gated = !in_range && beyond > 0;
    set_gated(gated);
    if (gated) ++sNetGateSkipped;
    return gated;
}

void LLViewerFetchedTexture::setVKDemand(U32 texels, U32 pass, U32 gpu_texels, bool t2_safe, bool needs_decode, bool mask_variant)
{
    if (pass != mVKDemandPass)
    {
        mVKDemand = texels;
        mVKDemandPass = pass;
        mVKGpuTexels = gpu_texels;
        mVKT2Safe = t2_safe;
    }
    else
    {
        mVKDemand = llmax(mVKDemand, texels);
        mVKGpuTexels = llmax(mVKGpuTexels, gpu_texels);
        mVKT2Safe = mVKT2Safe || t2_safe;
    }
    VKDecodeDemand& request = mVKDecodeDemand[mask_variant ? 1 : 0];
    const bool new_decode = needs_decode && (!request.texels || sCurrentTime - request.time > VK_DEMAND_STALE_SEC);
    if (request.pass != pass) request.texels = 0;
    request.pass = pass;
    request.time = sCurrentTime;
    if (needs_decode) request.texels = llmax(request.texels, texels);
    for (const VKDecodeDemand& pending : mVKDecodeDemand)
    {
        if (pending.texels && sCurrentTime - pending.time <= VK_DEMAND_STALE_SEC)
        {
            mVKDemand = llmax(mVKDemand, pending.texels);
            mVKGpuTexels = 0;
        }
    }
    mVKDemandTime = sCurrentTime;
    if ((mVKDemand != mVKDemandSeen || new_decode) && !mVKDirtyQueued)
    {
        mVKDirtyQueued = true;
        sVKDirty.push_back(this);
    }
}

std::string LLViewerFetchedTexture::fetchDiagString()
{
    const LLTextureData* gl = getTextureData();
    const LLImageRaw* cpu = gl ? gl->getVKCpuData() : nullptr;
    return llformat("id=%s boost=%d disc=%d want=%d min=%d cb_want=%d raw=%d full=%dx%d vsize=%.0f fetching=%d fetcher=%d state=%d missing=%d needs_create=%d cbs=%d pkt_age=%.1f vk_cpu=%dx%d vk_gen=%d unsettled=%d dropped=%d lease=0x%x resrc=%d",
        getID().asString().c_str(), (S32)mBoostLevel, getDiscardLevel(), (S32)mDesiredDiscardLevel,
        (S32)mMinDesiredDiscardLevel, (S32)mLoadedCallbackDesiredDiscardLevel, mRawDiscardLevel,
        mFullWidth, mFullHeight, mMaxVirtualSize, (S32)mIsFetching, (S32)mHasFetcher, mFetchState,
        (S32)mIsMissingAsset, (S32)mNeedsCreateTexture, (S32)mLoadedCallbackList.size(),
        mLastPacketTimer.getElapsedTimeF32(),
        cpu ? (S32)cpu->getWidth() : 0, cpu ? (S32)cpu->getHeight() : 0,
        gl ? gl->getVKGeneration() : -1, (S32)vkUnsettledReason(), gl && gl->isVKCpuDropped() ? 1 : 0,
        vkCpuLeaseMask(), (S32)mVKResourceDiscard);
}

U32 LLViewerFetchedTexture::sVKUnsettledHist[VKU_COUNT] = {};
U32 LLViewerFetchedTexture::sVKNotDrivenBoosted[2] = {};

// A pending CPU-copy downscale (mVKOverSince / mVKScalePending) is RAM housekeeping,
// not fetch work: it doesn't keep a texture in the per-frame rotation (it is retried
// on the every-16th maintenance visit). After a camera spin thousands of textures sat
// "over" forever -- tiny copies and busy/impossible scale jobs never cleared it -- and
// LL_texFetch stayed ~10x its settled cost.
U8 LLViewerFetchedTexture::vkUnsettledReason()
{
    const bool busy = mIsFetching || mHasFetcher || mNeedsCreateTexture
                      || !mLoadedCallbackList.empty();
    // #707: a re-source (decode of a dropped copy) is fetch work until it lands.
    if (mVKResourceDiscard >= 0) return busy ? VKU_FETCHING : VKU_SHARPEN;
    // Wanted by the renderer but never decoded (size unknown): applyVKDemand drives its
    // first fetch -- keep it in the per-frame rotation until that lands.
    if (mBoostLevel == BOOST_NONE && sCurrentTime - mVKDemandTime <= VK_DEMAND_STALE_SEC
        && (!mFullWidth || !mFullHeight) && getDiscardLevel() < 0 && !mIsMissingAsset)
        return busy ? VKU_FETCHING : VKU_SHARPEN;
    if (!vkDemandActive())
    {
        // Once VK-driven, now unreferenced by the renderer (demand went stale -- e.g.
        // everything seen during a camera spin) and idle: maintenance cadence only.
        if (mBoostLevel == BOOST_NONE && mVKDemandTime > 0.f && !busy)
            return VKU_SETTLED;
        // Never asked for by the renderer, and processTextureStats already owns its
        // discard (thumbnail rule, mVKIdleOwned): nothing for the per-face scan to change
        // -- maintenance cadence only, unless the VK UI started drawing it.
        if (mBoostLevel == BOOST_NONE && mVKIdleOwned && !busy)
        {
            const LLTextureData* gl = mTextureData.get();
            if (!(gl && gl->isVKUiUsed()))
                return VKU_SETTLED;
        }
        ++sVKNotDrivenBoosted[mBoostLevel == BOOST_NONE ? 0 : 1];
        return VKU_NOT_DRIVEN;
    }
    if (mIsFetching || mHasFetcher) return VKU_FETCHING;
    if (mNeedsCreateTexture) return VKU_CREATE;
    if (!mLoadedCallbackList.empty()) return VKU_CALLBACKS;
    const S32 cur = getDiscardLevel();
    if (cur < 0 || cur > mDesiredDiscardLevel) return VKU_SHARPEN;
    if (mVKDemand != mVKDemandSeen) return VKU_DEMAND_CHANGED;
    return VKU_SETTLED;
}

bool LLViewerFetchedTexture::vkSettled()
{
    return vkUnsettledReason() == VKU_SETTLED;
}

bool LLViewerFetchedTexture::vkDemandActive() const
{
    static LLCachedControl<bool> textures_fullres(gSavedSettings, "TextureLoadFullRes", false);
    return mBoostLevel == BOOST_NONE && !textures_fullres
        && sCurrentTime - mVKDemandTime <= VK_DEMAND_STALE_SEC
        && !mDontDiscard && mUseMipMaps && !mForceToSaveRawImage
        && mFullWidth && mFullHeight;
}

bool LLViewerFetchedTexture::vkTrimIdleCpuCopy()
{
    const LLTextureData* gl = mTextureData.get();
    if (!gl || !gl->getVKCpuData()) return false;
    return vkCpuCopyStep();
}

bool LLViewerFetchedTexture::vkWorldLive() const
{
    return mVKDemandTime > 0.f && sCurrentTime - mVKDemandTime <= VK_DEMAND_STALE_SEC;
}

bool LLViewerFetchedTexture::vkGpuCoversDemand() const
{
    // The residency rule's target (applyVKDemand): the demand, never more than the texture
    // has under LL's resolution cap.
    const S32 full = llmax(mFullWidth, mFullHeight);
    if (full <= 0 || !mVKGpuTexels) return false;
    static LLCachedControl<U32> max_texture_resolution(gSavedSettings, "RenderMaxTextureResolution", 2048);
    const S32 max_tex_res = (S32)llclamp((U32)max_texture_resolution, 512u, (U32)MAX_IMAGE_SIZE_DEFAULT);
    S32 min_discard = 0;
    while ((full >> min_discard) > max_tex_res && min_discard < MAX_DISCARD_LEVEL) ++min_discard;
    const U32 cap = (U32)llmax(full >> min_discard, 1);
    const U32 want = llmin(mVKDemand >= VK_DEMAND_FULL ? cap : llmax(mVKDemand, 1u), cap);
    return mVKGpuTexels >= want;
}

bool LLViewerFetchedTexture::vkCpuResourceable() const
{
    if (mVKFromRaw || mIsMissingAsset || getID().isNull()) return false;
    switch (mFTType)
    {
        case FTT_DEFAULT:
        case FTT_SERVER_BAKE:
        case FTT_MAP_TILE:    return true;
        case FTT_LOCAL_FILE:  return !mUrl.empty();
        default:              return false;
    }
}

U32 LLViewerFetchedTexture::vkCpuLeaseMask() const
{
    const LLTextureData* gl = mTextureData.get();
    if (!gl) return 0;
    U32 mask = gl->getVKLeaseMask();   // explicit leases, MEDIA (writable), ALPHA (job)
    if (!vkCpuResourceable()) mask |= 1u << LLTextureData::VKLEASE_LOCAL;
    // STREAM: the world renderer wants it now and its GPU image doesn't cover the demand
    // yet (first upload, upgrade, re-encode / coverage job: they report no resident size).
    if (vkWorldLive() && !vkGpuCoversDemand()) mask |= 1u << LLTextureData::VKLEASE_STREAM;
    // UI: drawn and not uploaded at this generation; skin sprites always (the UI itself --
    // small, drawn everywhere, re-sourcing them would flash).
    if (gl->vkUiNeedsCpu() || mBoostLevel == BOOST_UI) mask |= 1u << LLTextureData::VKLEASE_UI;
    // CALLBACK: LL load callbacks waiting for raw pixels (#655 readback source).
    for (const LLLoadedCallbackEntry* e : mLoadedCallbackList)
    {
        if (e && e->mNeedsImageRaw) { mask |= 1u << LLTextureData::VKLEASE_CALLBACK; break; }
    }
    // APPEARANCE: own avatar local textures while the avatar composites locally (LLTexLayer
    // draws them through the VK UI backend).
    if (mBoostLevel == BOOST_AVATAR_SELF && isAgentAvatarValid() && gAgentAvatarp->isUsingLocalAppearance())
        mask |= 1u << LLTextureData::VKLEASE_APPEARANCE;
    return mask;
}

void LLViewerFetchedTexture::vkRequestCpuCopy(S32 discard)
{
    LLTextureData* gl = mTextureData.get();
    if (!gl || gl->getVKCpuData() || !gl->isVKCpuDropped() || !vkCpuResourceable()) return;
    if (discard < 0) discard = llmax(getDiscardLevel(), 0);
    discard = llclamp(discard, 0, (S32)MAX_DISCARD_LEVEL);
    if (mVKResourceDiscard < 0) { ++sVKResourced; mVKResourceSince = sCurrentTime; }
    if (mVKResourceDiscard < 0 || discard < mVKResourceDiscard) mVKResourceDiscard = (S8)discard;
    mDesiredDiscardLevel = (S8)llmin((S32)mDesiredDiscardLevel, (S32)mVKResourceDiscard);
    mFullyLoaded = false;
    // processed first by the next list update (it runs processTextureStats + updateFetch)
    if (!mVKDirtyQueued)
    {
        mVKDirtyQueued = true;
        sVKDirty.push_back(this);
    }
}

bool LLViewerFetchedTexture::vkCpuCopyStep()
{
    LLTextureData* gl = mTextureData.get();
    if (!gl) return false;
    const LLImageRaw* raw = gl->getVKCpuData();
    // A PENDING RE-SOURCE (#707) keeps LL asking for its decode until the copy is back:
    // getCurrentDiscardLevelForFetching reports nothing loaded, the desired level covers
    // the request, and it has a decode priority (UI / callback readers have no demand).
    if (mVKResourceDiscard >= 0)
    {
        if (raw || !gl->isVKCpuDropped() || mIsMissingAsset)
        {
            mVKResourceDiscard = -1;
        }
        else if (sCurrentTime - mVKResourceSince > VK_RESOURCE_GIVE_UP_SEC)
        {
            // Never landed (the J2C left the cache and the network fetch failed): stop
            // asking -- the next reader asks again. Logged, never fatal.
            LL_WARNS_ONCE("VKTex") << "re-source of a dropped texture copy did not land in "
                                   << VK_RESOURCE_GIVE_UP_SEC << " s, giving up for now: " << getID() << LL_ENDL;
            mVKResourceDiscard = -1;
            ++sVKResourceFailed;
        }
        else
        {
            mDesiredDiscardLevel = (S8)llmin((S32)mDesiredDiscardLevel, (S32)mVKResourceDiscard);
            mFullyLoaded = false;
            const S32 full = llmax(mFullWidth, mFullHeight);
            const F32 dim = (F32)llclamp(full >> mVKResourceDiscard, 64, (S32)MAX_IMAGE_SIZE_DEFAULT);
            mMaxVirtualSize = llmax(mMaxVirtualSize, dim * dim);
            return false;
        }
    }
    if (!raw)
    {
        // Dropped, and the world renderer lost (or never reached) the image it needs:
        // VRAM ladder downscale / eviction / demand grown past the GPU image with no
        // tier-2 chain to serve it. Re-source the decode at the level LL wants.
        if (gl->isVKCpuDropped() && vkWorldLive() && !vkGpuCoversDemand())
            vkRequestCpuCopy(llmin(llmax(getDiscardLevel(), 0), (S32)mDesiredDiscardLevel));
        return false;
    }
    if (!raw->getData() || gl->isVKCpuWritable() || mVKScalePending || mNeedsCreateTexture)
    {
        mVKDropReadySince = -1.f;
        return false;
    }
    if (vkCpuLeaseMask())
    {
        mVKDropReadySince = -1.f;
        mVKCaptureWaitSince = -1.f;
        return false;
    }
    const bool shedding = LLMemoryGovernor::isShedding() || LLMemoryGovernor::shouldTrimAggressively();
    // A live world texture: drop only once its GPU image can come back without the network
    // -- the tier-2 capture exists or it is never captured (renderer-reported, mVKT2Safe) --
    // waiting at most VK_CAPTURE_WAIT_SEC (and not at all while memory is short).
    if (vkWorldLive() && !mVKT2Safe && !shedding)
    {
        if (mVKCaptureWaitSince < 0.f) mVKCaptureWaitSince = sCurrentTime;
        if (sCurrentTime - mVKCaptureWaitSince < VK_CAPTURE_WAIT_SEC) return false;
    }
    // Tier-1 fast path (Auto, plenty of RAM): world textures keep their copy at the demand
    // size (applyVKDemand scales it); everything else drops.
    if (mVKDemandTime > 0.f && vkKeepTier1()) return false;
    // Never asked for by the world renderer (LL decoded it on its own, a UI image that drew
    // and uploaded, a sculpt-only map): idle a while first -- face-less ones (UI images
    // that may not have drawn yet, terrain detail textures) longer.
    if (mVKDemandTime <= 0.f && !isForSculptOnly())   // (a sculpt-only map: its saved raw is the sculpt's copy)
    {
        const bool faceless = getTotalNumFaces() <= 0;
        if (mIsFetching || mHasFetcher
            || mLastPacketTimer.getElapsedTimeF32() < vk_idle_thumb_sec() * (faceless ? 4.f : 1.f))
            return false;
    }
    if (mVKDropReadySince < 0.f) { mVKDropReadySince = sCurrentTime; return false; }
    if (sCurrentTime - mVKDropReadySince < vk_drop_grace()) return false;
    if (!gl->dropVKCpuData()) return false;
    mVKDropReadySince = -1.f;
    mVKCaptureWaitSince = -1.f;
    mVKOverSince = -1.f;
    ++sVKDropped;
    return true;
}

bool LLViewerFetchedTexture::applyVKDemand()
{
    // FIRST FETCH (#145): the renderer wants this texture but it has never been decoded,
    // so its full size is unknown and vkDemandActive() (which needs it) can't drive it --
    // while LL's face-area fallback gives far faces a virtual size of ~0 and never starts
    // the fetch: textures stayed grey forever (1.2-1.7k in a busy two-sim view). Drive
    // it: fetch priority from the demand and a coarse first decode; once the header lands
    // (full size known) the normal residency rule below takes over.
    {
        static LLCachedControl<bool> textures_fullres(gSavedSettings, "TextureLoadFullRes", false);
        if (mBoostLevel == BOOST_NONE && !textures_fullres
            && sCurrentTime - mVKDemandTime <= VK_DEMAND_STALE_SEC && (!mFullWidth || !mFullHeight)
            && !mDontDiscard && mUseMipMaps && !mForceToSaveRawImage && getDiscardLevel() < 0)
        {
            const F32 dim = (F32)llmin(llmax(mVKDemand, 64u), (U32)MAX_IMAGE_SIZE_DEFAULT);
            mMaxVirtualSize = llmax(mMaxVirtualSize, dim * dim);
            mDesiredDiscardLevel = (S8)llmin((S32)mDesiredDiscardLevel, (S32)MAX_DISCARD_LEVEL);
            mFullyLoaded = false;
            return true;
        }
    }
    if (!vkDemandActive())
    {
        mVKOverSince = -1.f;
        // RAM (#159): the renderer stopped asking for this texture (off screen). Its GPU
        // image (if any) stays as is; the CPU copy -- kept at whatever size it last had,
        // for the rest of the session, 16.7 GB in a busy sim -- drops to a thumbnail, and
        // the Vulkan side keeps owning the discard so LL's face-area logic doesn't
        // re-decode what nobody is looking at. Back on screen, the demand sharpens it.
        static LLCachedControl<bool> textures_fullres(gSavedSettings, "TextureLoadFullRes", false);
        const bool busy = mIsFetching || mHasFetcher || mNeedsCreateTexture
                          || !mLoadedCallbackList.empty();
        // (#707: the copy itself is vkCpuCopyStep's: dropped, not thumbnailed)
        if (mBoostLevel == BOOST_NONE && !textures_fullres && mVKDemandTime > 0.f
            && sCurrentTime - mVKDemandTime > vk_stale_thumb_sec() && !busy
            && !mDontDiscard && mUseMipMaps && !mForceToSaveRawImage && mFullWidth && mFullHeight)
        {
            const S32 cur = getDiscardLevel();
            if (cur >= 0) { mDesiredDiscardLevel = (S8)cur; mFullyLoaded = true; }
            return true;
        }
        // NEVER ASKED FOR BY THE RENDERER, idle: a plain texture LL decoded on its own
        // (e.g. its virtual size was left at 2048^2 by an earlier boost -- setBoostLevel
        // never lowers it -- or a particle source) and the world renderer never
        // requested. Own its discard 30 s after its last fetch so LL doesn't keep decoding
        // it; its copy is vkCpuCopyStep's (#707: dropped, re-sourced if a reader asks).
        // Images the VK UI draws are LL's (their discard follows the draw size).
        const LLTextureData* gl = mTextureData.get();
        // Face-less textures wait 4x longer: a UI image may not have drawn yet. Terrain
        // detail textures are face-less too (#706).
        mVKIdleOwned = false;
        const bool faceless = getTotalNumFaces() <= 0;
        if (mBoostLevel == BOOST_NONE && !textures_fullres && mVKDemandTime <= 0.f
            && !busy && gl && !gl->isVKUiUsed() && !gl->isVKCpuWritable() && mVKResourceDiscard < 0
            && mLastPacketTimer.getElapsedTimeF32() > vk_idle_thumb_sec() * (faceless ? 4.f : 1.f)
            && !mDontDiscard && mUseMipMaps && !mForceToSaveRawImage && mFullWidth && mFullHeight)
        {
            const S32 cur = getDiscardLevel();
            if (cur >= 0) { mDesiredDiscardLevel = (S8)cur; mFullyLoaded = true; mVKIdleOwned = true; }
            return true;
        }
        return false;
    }
    static LLCachedControl<U32> max_texture_resolution(gSavedSettings, "RenderMaxTextureResolution", 2048);
    const S32 max_tex_res = (S32)llclamp((U32)max_texture_resolution, 512u, (U32)MAX_IMAGE_SIZE_DEFAULT);

    // Smallest discard whose top level still covers the demand -- the VK residency
    // pass's rule, so the decode lands at the size its GPU image wants -- never sharper
    // than LL's resolution cap.
    const S32 full = llmax(mFullWidth, mFullHeight);
    S32 min_discard = 0;
    while ((full >> min_discard) > max_tex_res && min_discard < MAX_DISCARD_LEVEL) ++min_discard;
    S32 d = 0;
    if (mVKDemand < VK_DEMAND_FULL)
    {
        const S32 want = (S32)llmax(mVKDemand, 1u);
        while (d < MAX_DISCARD_LEVEL && (full >> (d + 1)) >= want) ++d;
    }
    d = llclamp(d, min_discard, MAX_DISCARD_LEVEL);
    d = llmin(d, getMaxDiscardLevel());
    d = llmin(d, (S32)mMinDesiredDiscardLevel);
    d = llmin(d, (S32)mLoadedCallbackDesiredDiscardLevel);
    mDesiredDiscardLevel = (S8)d;
    // Fetch priority follows the on-screen need too (LL's face-area max never decays
    // under VK, where the discard bias is pinned).
    const F32 dim = (F32)llmin(mVKDemand, (U32)MAX_IMAGE_SIZE_DEFAULT);
    mMaxVirtualSize = llmax(dim * dim, 1.f);

    S32 cur = getDiscardLevel();
    // GPU ALREADY COVERS THE DEMAND (#159): the renderer's image is at least as sharp as
    // wanted, so nothing needs decoding and the CPU copy is only an upgrade source: keep
    // LL from re-decoding (desired = what the CPU copy holds) and let it shrink to a
    // thumbnail. When the demand outgrows the GPU image, the normal rule applies again:
    // the decode sharpens, the residency pass upgrades from it.
    // Capped at what this texture can be (its full size under LL's resolution cap): the
    // floored / mip-rounded demand can exceed a small texture's size, and the GPU can
    // never "cover" more than the texture has.
    const U32 want_texels = llmin((mVKDemand >= VK_DEMAND_FULL) ? (U32)(full >> min_discard) : llmax(mVKDemand, 1u),
                                  (U32)llmax(full >> min_discard, 1));
    const bool gpu_covers = cur >= 0 && mVKGpuTexels >= want_texels;
    S32 cpu_target = d;
    const bool dropped = mTextureData.notNull() && mTextureData->isVKCpuDropped();
    if (gpu_covers)
    {
        // Tier 1 (#405): while memory is plentiful the copy stays at the demand size (the
        // fastest re-upload after a GPU downgrade / eviction); otherwise it is dropped
        // (#707, vkCpuCopyStep: no point shrinking it first), and the GPU-ready disk cache
        // (tier 2) or a re-decode re-sources the pixels.
        if (!vkKeepTier1()) cpu_target = cur;
        if (cur > d) { d = cur; mDesiredDiscardLevel = (S8)cur; }   // no re-decode
    }
    else if (dropped && cur >= 0 && cur <= d)
    {
        // #707: the GPU no longer holds what LL's bookkeeping says is loaded (eviction /
        // VRAM ladder) and no tier-2 chain covers it: decode the level again.
        vkRequestCpuCopy(d);
    }
    mFullyLoaded = cur >= 0 && cur <= d && !(dropped && !gpu_covers);
    // RAM: a CPU copy sharper than wanted (the texture was seen closer before) is
    // scaled down after a grace period. Its discard level then reads coarser, so a later
    // need for detail re-decodes from the disk cache through the normal fetch.
    if (cur >= 0 && cpu_target > cur && !mVKScalePending
        && !(cpu_target == mVKNoScaleDiscard && cur == mVKNoScaleCur))
    {
        if (mVKOverSince < 0.f) mVKOverSince = sCurrentTime;
        else if (sCurrentTime - mVKOverSince > vk_scale_down_delay())
        {
            const S32 r = vkScaleDownCpuCopy(cpu_target);
            if (r != 0) mVKOverSince = -1.f;          // started, or impossible:
            if (r < 0) { mVKNoScaleDiscard = (S8)cpu_target; mVKNoScaleCur = (S8)cur; }   // until either changes
        }
    }
    else if (!mVKScalePending)
    {
        mVKOverSince = -1.f;
    }
    return true;
}

// Box-filter `src` down `steps` mip levels (any component count). Worker thread: reads
// only the immutable source copy and allocates the result.
static LLPointer<LLImageRaw> vk_box_downscale(const LLPointer<LLImageRaw>& src, S32 steps)
{
    LLImageDataSharedLock lock(src.get());
    S32 w = src->getWidth(), h = src->getHeight();
    const S32 c = src->getComponents();
    if (!src->getData() || w <= 0 || h <= 0 || c < 1 || c > 4 || steps <= 0) return nullptr;
    const U8* in = src->getData();
    std::vector<U8> buf[2];
    for (S32 s = 0; s < steps && (w > 1 || h > 1); ++s)
    {
        const S32 nw = llmax(w >> 1, 1), nh = llmax(h >> 1, 1);
        std::vector<U8>& out = buf[s & 1];
        out.resize((size_t)nw * nh * c);
        for (S32 y = 0; y < nh; ++y)
        {
            const U8* r0 = in + (size_t)llmin(2 * y, h - 1) * w * c;
            const U8* r1 = in + (size_t)llmin(2 * y + 1, h - 1) * w * c;
            U8* o = out.data() + (size_t)y * nw * c;
            for (S32 x = 0; x < nw; ++x, o += c)
            {
                const S32 x0 = llmin(2 * x, w - 1) * c, x1 = llmin(2 * x + 1, w - 1) * c;
                for (S32 k = 0; k < c; ++k)
                    o[k] = (U8)((r0[x0 + k] + r0[x1 + k] + r1[x0 + k] + r1[x1 + k] + 2) >> 2);
            }
        }
        in = out.data(); w = nw; h = nh;
    }
    LLPointer<LLImageRaw> result = new LLImageRaw((U16)w, (U16)h, (S8)c);
    if (!result->getData()) return nullptr;
    memcpy(result->getData(), in, (size_t)w * h * c);
    return result;
}

S32 LLViewerFetchedTexture::vkScaleDownCpuCopy(S32 target_discard)
{
    LLTextureData* gl = mTextureData.get();
    LLImageRaw* src = gl ? gl->getVKCpuData() : nullptr;
    const S32 cur = gl ? gl->getDiscardLevel() : -1;
    // Only immutable decode copies (never a media/UI writable buffer); tiny ones aren't
    // worth a job.
    if (!src || !src->getData() || gl->isVKCpuWritable() || cur < 0 || target_discard <= cur
        || src->getWidth() * src->getHeight() <= 64 * 64)
    {
        return -1;
    }
    if (sVKScaleInFlight >= vk_max_scale_in_flight()) return 0;
    LL::WorkQueue::ptr_t main_queue = LL::WorkQueue::getInstance("mainloop");
    LL::WorkQueue::ptr_t general_queue = LL::WorkQueue::getInstance("General");
    if (!main_queue || !general_queue) return 0;
    LLPointer<LLImageRaw> srcp = src;   // thread-safe refcount; keeps the source alive
    const S32 steps = target_discard - cur;
    const LLUUID id = getID();
    const bool posted = main_queue->postTo(
        general_queue,
        // Never let the worker throw back to the main loop: an exception here was
        // transported as an EMPTY exception_ptr and std::rethrow_exception(null)
        // segfaulted in idle (tester crash, RX 5600 XT / Ubuntu 24.04). A failed
        // downscale just keeps the current copy.
        [srcp, steps]() -> LLPointer<LLImageRaw>
        {
            try { return vk_box_downscale(srcp, steps); }
            catch (const std::exception& e)
            {
                LL_WARNS_ONCE("VKTex") << "vk_box_downscale failed: " << e.what() << LL_ENDL;
            }
            catch (...)
            {
                LL_WARNS_ONCE("VKTex") << "vk_box_downscale failed: non-std exception" << LL_ENDL;
            }
            return nullptr;
        },
        [id, srcp, target_discard](LLPointer<LLImageRaw> out)   // main thread
        {
            if (sVKScaleInFlight) --sVKScaleInFlight;
            LLViewerFetchedTexture* tex = gTextureList.findImage(id, TEX_LIST_STANDARD);
            if (!tex) return;
            tex->mVKScalePending = false;
            LLTextureData* img = tex->getTextureData();
            // A fresh decode replaced the copy meanwhile: keep that one.
            if (out.isNull() || !img || img->getVKCpuData() != srcp.get()) return;
            img->replaceVKCpuData(out, target_discard);
            ++sVKScaledDown;
        });
    if (!posted) return 0;
    ++sVKScaleInFlight;
    mVKScalePending = true;
    return 1;
}

// static
// #535: the VKMem texture tallies, sliced across frames. One pass over gTextureList
// (by hash bucket, so a frame can stop and the next resume) computes what used to take
// two to three full walks in one frame (getVKResidencyStats + its 30 s CPU-copy class
// diagnostic + getVKTier1Bytes): 19k+ cold texture objects, a 5-10 ms spike on the
// VKMem log's 5 s tick. A rehash between frames restarts the pass (after a few restarts
// the rest finishes unbudgeted, so a pass always completes). Main thread.
bool LLViewerFetchedTexture::vkTexStatsStep(F64 budget_s, VKTexStats& out)
{
    struct Acc
    {
        U32 driven = 0, reduced = 0, sharpen = 0;
        U32 never = 0, need_sharper = 0, gpu_cov = 0;
        U64 t1_opt = 0;
        // #707: leased bytes by reason (first reason in enum order) + the unleased copies
        // by why they are still here (drop grace / tier-2 capture wait / tier-1 fast path)
        U64 b_lease[LLTextureData::VKLEASE_COUNT] = {};
        U32 n_lease[LLTextureData::VKLEASE_COUNT] = {};
        U64 b_grace = 0, b_capture = 0, b_keep1 = 0;
        U32 n_unleased = 0, n_dropped = 0;
        U32 n_active_nocov = 0;
        std::vector<std::string> samples_never, samples_sharp, undriven_samples;
        std::string nocov_sample;
    };
    static Acc s_acc;
    static size_t s_bucket = 0, s_buckets = 0;
    static U32 s_restarts = 0;
    static bool s_active = false, s_diag = false;
    static F32 s_next_diag = 0.f;

    const size_t nb = gTextureList.imageBucketCount();
    if (!s_active || s_buckets != nb)
    {
        if (s_active) ++s_restarts;   // rehashed mid-pass: the bucket cursor is no longer valid
        else s_restarts = 0;
        s_active = true;
        s_acc = Acc();
        s_bucket = 0;
        s_buckets = nb;
        // Split the pending ones (diagnostic, every ~30 s): never decoded (no full size
        // yet -- still loading) vs decoded but waiting on a sharper decode, with samples;
        // and who holds CPU copies (#159).
        s_diag = sCurrentTime >= s_next_diag;
    }
    const bool budgeted = budget_s > 0.0 && s_restarts < 4;
    const bool diag = s_diag;
    Acc& a = s_acc;
    LLTimer timer;
    U32 since_check = 0;
    while (s_bucket < nb)
    {
        for (auto it = gTextureList.imageBucketBegin(s_bucket), end = gTextureList.imageBucketEnd(s_bucket); it != end; ++it)
        {
            LLViewerFetchedTexture* t = it->get();
            if (!t) continue;
            ++since_check;
            // Residency (was getVKResidencyStats).
            if (t->mBoostLevel == BOOST_NONE && sCurrentTime - t->mVKDemandTime <= VK_DEMAND_STALE_SEC)
            {
                ++a.driven;
                const S32 cur = t->getDiscardLevel();
                if (cur > 0) ++a.reduced;
                if (cur < 0 || cur > t->mDesiredDiscardLevel)
                {
                    ++a.sharpen;
                    if (diag)
                    {
                        const bool nv = cur < 0 || !t->mFullWidth;
                        (nv ? a.never : a.need_sharper)++;
                        if (t->mVKGpuTexels) ++a.gpu_cov;
                        auto& v = nv ? a.samples_never : a.samples_sharp;
                        if (v.size() < 3) v.push_back(t->fetchDiagString() + llformat(" vkdem=%u gpu=%u", t->mVKDemand, t->mVKGpuTexels));
                    }
                }
            }
            // CPU copies (#707): leased (by reason) vs unleased (tier-1 optional bytes) +
            // the diagnostic split.
            const LLTextureData* gl = t->getTextureData();
            const LLImageRaw* r = gl ? gl->getVKCpuData() : nullptr;
            if (!r || !r->getData())
            {
                if (diag && gl && gl->isVKCpuDropped()) ++a.n_dropped;
                continue;
            }
            const U64 b = (U64)r->getDataSize();
            const U32 mask = t->vkCpuLeaseMask();
            if (!mask) a.t1_opt += b;
            if (!diag) continue;
            if (mask)
            {
                U32 i = 0;
                while (i + 1 < LLTextureData::VKLEASE_COUNT && !(mask & (1u << i))) ++i;
                a.b_lease[i] += b; ++a.n_lease[i];
                if (i == LLTextureData::VKLEASE_STREAM)
                {
                    ++a.n_active_nocov;
                    if (a.nocov_sample.empty() && b > (1u << 20))
                        a.nocov_sample = t->fetchDiagString() + llformat(" vkdem=%u gpu=%u", t->mVKDemand, t->mVKGpuTexels);
                }
                else if (b >= (2u << 20) && a.undriven_samples.size() < 4)
                    a.undriven_samples.push_back(llformat("[%s] ", LLTextureData::vkLeaseName((U8)i)) + t->fetchDiagString()
                        + llformat(" type=%d faces=%d", (S32)t->getType(), t->getTotalNumFaces()));
            }
            else
            {
                ++a.n_unleased;
                if (t->mVKDemandTime > 0.f && vkKeepTier1()) a.b_keep1 += b;
                else if (t->vkWorldLive() && !t->mVKT2Safe) a.b_capture += b;
                else a.b_grace += b;
            }
        }
        ++s_bucket;
        if (budgeted && since_check >= 256)
        {
            since_check = 0;
            if (timer.getElapsedTimeF64() > budget_s) return false;   // resume next frame
        }
    }
    s_active = false;

    out.driven = a.driven;
    out.reduced = a.reduced;
    out.sharpen = a.sharpen;
    const U64 total = (U64)llmax(LLTextureData::getVKCpuBytes(), (S64)0);
    out.t1_opt = llmin(a.t1_opt, total);
    out.t1_req = total - out.t1_opt;
    if (diag)
    {
        std::ostringstream o;
        o << "cpu copies MB (#707 leases):";
        for (U32 i = 0; i < LLTextureData::VKLEASE_COUNT; ++i)
            o << " " << LLTextureData::vkLeaseName((U8)i) << " " << (a.b_lease[i] >> 20) << " (" << a.n_lease[i] << ")";
        o << "; unleased " << a.n_unleased << ": drop grace " << (a.b_grace >> 20) << ", capture wait " << (a.b_capture >> 20)
          << ", tier-1 kept " << (a.b_keep1 >> 20) << "; dropped " << a.n_dropped << " (lifetime " << sVKDropped
          << " dropped, " << sVKResourced << " re-sourced, " << sVKResourceFailed << " re-sources abandoned)";
        LL_INFOS("VKTex") << o.str() << LL_ENDL;
        if (!a.nocov_sample.empty()) LL_INFOS("VKTex") << "  stream (not covered): " << a.nocov_sample << LL_ENDL;
        for (const std::string& u : a.undriven_samples) LL_INFOS("VKTex") << "  leased: " << u << LL_ENDL;
        s_next_diag = sCurrentTime + 30.f;
        LL_INFOS("VKTex") << "pending split: never-decoded " << a.never << ", need-sharper " << a.need_sharper
                          << " (with a GPU image " << a.gpu_cov << ")" << LL_ENDL;
        for (const std::string& d : a.samples_never) LL_INFOS("VKTex") << "  never: " << d << LL_ENDL;
        for (const std::string& d : a.samples_sharp) LL_INFOS("VKTex") << "  sharper: " << d << LL_ENDL;
        // (#707) CPU copies held by images outside the fetched texture list (dynamic,
        // media, local / generated images): counted in the total, invisible above.
        {
            struct Outside
            {
                std::unordered_set<const LLTextureData*> listed;
                S64 bytes = 0; U32 n = 0;
                std::vector<std::pair<S64, std::string>> big;
            } out;
            for (auto& image : gTextureList)
                if (const LLViewerFetchedTexture* t = image.get())
                    if (const LLTextureData* g = t->getTextureData()) out.listed.insert(g);
            LLTextureData::vkForEachOwned([](const LLTextureData* img, S64 b, void* ctx)
            {
                Outside& o = *static_cast<Outside*>(ctx);
                if (o.listed.count(img)) return;
                o.bytes += b; ++o.n;
                o.big.emplace_back(b, llformat("%dx%dx%d%s%s ui%d", img->getWidth(), img->getHeight(), (S32)img->getComponents(),
                                               img->getUseMipMaps() ? "" : " nomips", img->isVKCpuWritable() ? " writable" : "",
                                               (S32)img->getVKUiKind()));
            }, &out);
            std::sort(out.big.begin(), out.big.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
            std::ostringstream o2;
            o2 << "cpu copies outside the texture list: " << (out.bytes >> 20) << " MB (" << out.n << ")";
            for (size_t i = 0; i < out.big.size() && i < 8; ++i)
                o2 << (i ? ", " : "; largest: ") << out.big[i].second << " " << (out.big[i].first >> 10) << " KB";
            LL_INFOS("VKTex") << o2.str() << LL_ENDL;
        }
    }
    s_acc = Acc();   // release the sample strings
    return true;
}

void LLViewerFetchedTexture::processTextureStats()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    llassert(!LLPipeline::sShadowRender);

    if (applyVKDemand())
    {
        updateVirtualSize();
        vkCpuCopyStep();   // #707
        return;
    }

    if(mFullyLoaded)
    {
        if(mDesiredDiscardLevel > mMinDesiredDiscardLevel)//need to load more
        {
            mDesiredDiscardLevel = llmin(mDesiredDiscardLevel, mMinDesiredDiscardLevel);
            mDesiredDiscardLevel = llmin(mDesiredDiscardLevel, (S32)mLoadedCallbackDesiredDiscardLevel);
            mFullyLoaded = false;
        }
        //setDebugText("fully loaded");
    }
    else
    {
        updateVirtualSize();

        static LLCachedControl<bool> textures_fullres(gSavedSettings,"TextureLoadFullRes", false);

        U32 max_tex_res = MAX_IMAGE_SIZE_DEFAULT;
        if (mBoostLevel < LLGLTexture::BOOST_HIGH)
        {
            // restrict texture resolution to download based on RenderMaxTextureResolution
            static LLCachedControl<U32> max_texture_resolution(gSavedSettings, "RenderMaxTextureResolution", 2048);
            // sanity clamp debug setting to avoid settings hack shenanigans
            max_tex_res = (U32)llclamp((U32)max_texture_resolution, 512, MAX_IMAGE_SIZE_DEFAULT);
            mMaxVirtualSize = llmin(mMaxVirtualSize, (F32)(max_tex_res * max_tex_res));
        }

        if (textures_fullres)
        {
            mDesiredDiscardLevel = 0;
        }
        else if (mDontDiscard && (mBoostLevel == LLGLTexture::BOOST_ICON || mBoostLevel == LLGLTexture::BOOST_THUMBNAIL))
        {
            if (mFullWidth > MAX_IMAGE_SIZE_DEFAULT || mFullHeight > MAX_IMAGE_SIZE_DEFAULT)
            {
                mDesiredDiscardLevel = 1; // MAX_IMAGE_SIZE_DEFAULT = 2048 and max size ever is 4096
            }
            else
            {
                mDesiredDiscardLevel = 0;
            }
        }
        else if(!mFullWidth || !mFullHeight)
        {
            mDesiredDiscardLevel =  llmin(getMaxDiscardLevel(), (S32)mLoadedCallbackDesiredDiscardLevel);
        }
        else
        {
            if(!mKnownDrawWidth || !mKnownDrawHeight || (S32)mFullWidth <= mKnownDrawWidth || (S32)mFullHeight <= mKnownDrawHeight)
            {
                if (mFullWidth > max_tex_res || mFullHeight > max_tex_res)
                {
                    mDesiredDiscardLevel = 1;
                }
                else
                {
                    mDesiredDiscardLevel = 0;
                }
            }
            else if(mKnownDrawSizeChanged)//known draw size is set
            {
                mDesiredDiscardLevel = (S8)llmin(log((F32)mFullWidth / mKnownDrawWidth) / log_2,
                                                     log((F32)mFullHeight / mKnownDrawHeight) / log_2);
                mDesiredDiscardLevel =  llclamp(mDesiredDiscardLevel, (S8)0, (S8)getMaxDiscardLevel());
                mDesiredDiscardLevel = llmin(mDesiredDiscardLevel, mMinDesiredDiscardLevel);
                mDesiredDiscardLevel = llmin(mDesiredDiscardLevel, (S32)mLoadedCallbackDesiredDiscardLevel);
            }
            mKnownDrawSizeChanged = false;

            if(getDiscardLevel() >= 0 && (getDiscardLevel() <= mDesiredDiscardLevel))
            {
                mFullyLoaded = true;
            }
        }
    }

    if(mForceToSaveRawImage && mDesiredSavedRawDiscardLevel >= 0) //force to refetch the texture.
    {
        mDesiredDiscardLevel = llmin(mDesiredDiscardLevel, (S8)mDesiredSavedRawDiscardLevel);
        if(getDiscardLevel() < 0 || getDiscardLevel() > mDesiredDiscardLevel)
        {
            mFullyLoaded = false;
        }
    }
    vkCpuCopyStep();   // #707
}

//============================================================================

void LLViewerFetchedTexture::updateVirtualSize()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    reorganizeFaceList();
    reorganizeVolumeList();
}

S32 LLViewerFetchedTexture::getCurrentDiscardLevelForFetching()
{
    // #707 RE-SOURCE: the copy was dropped and a reader asked for it: nothing is loaded as
    // far as the fetch is concerned (the GPU-held level must not stop the decode).
    if (mVKResourceDiscard >= 0 && mTextureData.notNull() && !mTextureData->getVKCpuData()
        && mTextureData->isVKCpuDropped())
    {
        return -1;
    }
    S32 current_discard = getDiscardLevel();
    if(mForceToSaveRawImage)
    {
        if(mSavedRawDiscardLevel < 0 || current_discard < 0)
        {
            current_discard = -1;
        }
        else
        {
            current_discard = llmax(current_discard, mSavedRawDiscardLevel);
        }
    }

    // OCOL (Mirror): icons and thumbnails are scaled down to their known draw size when decoded, so "discard 0"
    // can be a 32 px copy made for a list icon. When a bigger control (the 64 px group notice insignia) later
    // raises the known size, count that copy as one level short so the image is fetched again at the new size.
    if (current_discard >= 0
        && (mBoostLevel == LLGLTexture::BOOST_ICON || mBoostLevel == LLGLTexture::BOOST_THUMBNAIL))
    {
        const S32 gl_width = getWidth(current_discard);
        const S32 gl_height = getHeight(current_discard);
        if ((gl_width < mKnownDrawWidth && gl_width < (S32)(mFullWidth >> current_discard))
            || (gl_height < mKnownDrawHeight && gl_height < (S32)(mFullHeight >> current_discard)))
        {
            current_discard++;
        }
    }

    return current_discard;
}

bool LLViewerFetchedTexture::isActiveFetching()
{
    static LLCachedControl<bool> monitor_enabled(gSavedSettings,"DebugShowTextureInfo");

    return mFetchState > 7 && mFetchState < 10 && monitor_enabled; //in state of WAIT_HTTP_REQ or DECODE_IMAGE.
}

void LLViewerFetchedTexture::setBoostLevel(S32 level)
{
    LLViewerTexture::setBoostLevel(level);

    if (level >= LLViewerTexture::BOOST_HIGH)
    {
        mDesiredDiscardLevel = 0;
    }
}

bool LLViewerFetchedTexture::processFetchResults(S32& desired_discard, S32 current_discard, S32 fetch_discard, F32 decode_priority)
{
    // We may have data ready regardless of whether or not we are finished (e.g. waiting on write)
    if (mRawImage.notNull())
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("vftuf - has raw image");
        LLTexturePipelineTester* tester = (LLTexturePipelineTester*)LLMetricPerformanceTesterBasic::getTester(sTesterName);
        if (tester)
        {
            mIsFetched = true;
            tester->updateTextureLoadingStats(this, mRawImage, LLAppViewer::getTextureFetch()->isFromLocalCache(mID));
        }
        mRawDiscardLevel = fetch_discard;
        if ((mRawImage->getDataSize() > 0 && mRawDiscardLevel >= 0) &&
            (current_discard < 0 || mRawDiscardLevel < current_discard))
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("vftuf - data good");

            // This is going to conflict with Develop, just pick from develop
            // where it uses setDimensions instead of setTexelsPerImage
            mFullWidth = mRawImage->getWidth() << mRawDiscardLevel;
            mFullHeight = mRawImage->getHeight() << mRawDiscardLevel;
            setTexelsPerImage();

            if (mFullWidth > MAX_IMAGE_SIZE || mFullHeight > MAX_IMAGE_SIZE)
            {
                //discard all oversized textures.
                destroyRawImage();
                LL_WARNS() << "oversize, setting as missing" << LL_ENDL;
                setIsMissingAsset();
                mRawDiscardLevel = INVALID_DISCARD_LEVEL;
                mIsFetching = false;
                mLastPacketTimer.reset();
            }
            else
            {
                mIsRawImageValid = true;
                addToCreateTexture();
            }

            if (mBoostLevel == LLGLTexture::BOOST_ICON)
            {
                S32 expected_width = mKnownDrawWidth > 0 ? mKnownDrawWidth : DEFAULT_ICON_DIMENSIONS;
                S32 expected_height = mKnownDrawHeight > 0 ? mKnownDrawHeight : DEFAULT_ICON_DIMENSIONS;
                if (mRawImage && (mRawImage->getWidth() > expected_width || mRawImage->getHeight() > expected_height))
                {
                    // scale oversized icon, no need to give more work to gl
                    // since we got mRawImage from thread worker and image may be in use (ex: writing cache), make a copy
                    //
                    // BOOST_ICON gets scaling because profile icons can have a bunch of different formats, not just j2c
                    // Might need another pass to use discard for j2c and scaling for everything else.
                    mRawImage = mRawImage->scaled(expected_width, expected_height);
                }
            }

            if (mBoostLevel == LLGLTexture::BOOST_THUMBNAIL)
            {
                S32 expected_width = mKnownDrawWidth > 0 ? mKnownDrawWidth : DEFAULT_THUMBNAIL_DIMENSIONS;
                S32 expected_height = mKnownDrawHeight > 0 ? mKnownDrawHeight : DEFAULT_THUMBNAIL_DIMENSIONS;
                if (mRawImage && (mRawImage->getWidth() > expected_width || mRawImage->getHeight() > expected_height))
                {
                    // scale oversized icon, no need to give more work to gl
                    // since we got mRawImage from thread worker and image may be in use (ex: writing cache), make a copy
                    //
                    // Todo: probably needs to be remade to use discard, all thumbnails are supposed to be j2c,
                    // so no need to scale, should be posible to use discard to scale image down.
                    mRawImage = mRawImage->scaled(expected_width, expected_height);
                }
            }

            return true;
        }
        else
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("vftuf - data not needed");
            // Data is ready but we don't need it
            // (received it already while fetcher was writing to disk)
            destroyRawImage();
            return false; // done
        }
    }

    if (!mIsFetching)
    {
        if ((decode_priority > 0)
            && (mRawDiscardLevel < 0 || mRawDiscardLevel == INVALID_DISCARD_LEVEL)
            && mFetchState > 1) // 1 - initial, make sure fetcher did at least something
        {
            // We finished but received no data
            if (getDiscardLevel() < 0)
            {
                if (getFTType() != FTT_MAP_TILE)
                {
                    LL_WARNS() << mID
                        << " Fetch failure, setting as missing, decode_priority " << decode_priority
                        << " mRawDiscardLevel " << mRawDiscardLevel
                        << " current_discard " << current_discard
                        << " stats " << mLastHttpGetStatus.toHex()
                        << " worker state " << mFetchState
                        << LL_ENDL;
                }
                setIsMissingAsset();
                desired_discard = -1;
            }
            else
            {
                //LL_WARNS() << mID << ": Setting min discard to " << current_discard << LL_ENDL;
                if (current_discard >= 0)
                {
                    mMinDiscardLevel = current_discard;
                    //desired_discard = current_discard;
                }
                else
                {
                    S32 dis_level = getDiscardLevel();
                    mMinDiscardLevel = dis_level;
                    //desired_discard = dis_level;
                }
            }
            destroyRawImage();
        }
        else if (mRawImage.notNull())
        {
            // We have data, but our fetch failed to return raw data
            // *TODO: FIgure out why this is happening and fix it
            // Potentially can happen when TEX_LIST_SCALE and TEX_LIST_STANDARD
            // get requested for the same texture id at the same time
            // (two textures, one fetcher)
            destroyRawImage();
        }
    }

    return true;
}

bool LLViewerFetchedTexture::updateFetch()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    static LLCachedControl<bool> textures_decode_disabled(gSavedSettings, "TextureDecodeDisabled", false);

    if (textures_decode_disabled) // don't fetch the surface textures in wireframe mode
    {
        return false;
    }

    mFetchState = 0;
    mFetchPriority = 0;
    mFetchDeltaTime = 999999.f;
    mRequestDeltaTime = 999999.f;

#ifndef LL_RELEASE_FOR_DOWNLOAD
    if (mID == LLAppViewer::getTextureFetch()->mDebugID)
    {
        LLAppViewer::getTextureFetch()->mDebugCount++; // for setting breakpoints
    }
#endif

    if (mNeedsCreateTexture)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("vftuf - needs create");
        // We may be fetching still (e.g. waiting on write)
        // but don't check until we've processed the raw data we have
        return false;
    }
    if (mIsMissingAsset)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("vftuf - missing asset");
        llassert(!mHasFetcher);
        return false; // skip
    }
    if (!mLoadedCallbackList.empty() && mRawImage.notNull())
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("vftuf - callback pending");
        return false; // process any raw image data in callbacks before replacing
    }
    if (mTextureData.isNull())
    { // fix for crash inside getCurrentDiscardLevelForFetching (shouldn't happen but appears to be happening)
        llassert(false);
        return false;
    }

    S32 current_discard = getCurrentDiscardLevelForFetching();
    S32 desired_discard = getDesiredDiscardLevel();
    F32 decode_priority = mMaxVirtualSize;

    // #485 CANCEL BEFORE SEND: the renderer stopped asking for this texture (it left the
    // view -- e.g. flying fast past it) SLFetchMaxQueuedAge seconds ago. Don't start a
    // fetch for it, and withdraw a pending one: priority 0 is the fetcher's "drop it
    // unsent" signal (an in-flight download still lands; it isn't worth killing the
    // keep-alive connection for). LL's face-area priority never decays under VK, so
    // without this a fast flight queued everything it ever passed. Boosted textures
    // (avatars, UI, sculpts, terrain), re-sources of a dropped copy (#707: a reader is
    // waiting) and ones with load callbacks waiting are exempt; on screen again, the
    // demand restores the priority at once. (The renderer's fixed sets -- projectors,
    // particles, water, sky -- send demand every few passes.)
    bool vk_deferred = false;
    {
        static LLCachedControl<F32> s_max_age(gSavedSettings, "SLFetchMaxQueuedAge", 8.f);
        if ((F32)s_max_age > 0.f && mBoostLevel == BOOST_NONE && mVKResourceDiscard < 0 && mVKDemandTime > 0.f
            && sCurrentTime - mVKDemandTime > (F32)s_max_age && mLoadedCallbackList.empty())
        {
            decode_priority = 0.f;
            vk_deferred = true;
        }
    }
    // #485 NETWORK DRAW DISTANCE GATE: every user beyond the network draw distance
    // (horizontally): no new request, and a queued one is withdrawn the same way.
    bool net_gated = false;
    if (!vk_deferred && decode_priority > 0.f && mLoadedCallbackList.empty() && mVKResourceDiscard < 0 && vkNetGated())
    {
        decode_priority = 0.f;
        vk_deferred = true;
        net_gated = true;
    }

    if (mIsFetching)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("vftuf - is fetching");
        // Sets mRawDiscardLevel, mRawImage, mAuxRawImage
        S32 fetch_discard = current_discard;

        if (mRawImage.notNull()) sRawCount--;
        if (mAuxRawImage.notNull()) sAuxCount--;
        // keep in mind that fetcher still might need raw image, don't modify original
        // #305: one fetcher lookup for both "finished?" and the fetch state (this used to
        // be getRequestFinished + getFetchState + an updateRequestPriority post, three
        // worker lookups per fetching texture per frame)
        LLTextureFetch::FetchStateInfo fetch_info;
        bool finished = LLAppViewer::getTextureFetch()->getRequestFinished(getID(), fetch_discard, mFetchState, mRawImage, mAuxRawImage,
            mLastHttpGetStatus, &fetch_info);
        if (mRawImage.notNull()) sRawCount++;
        if (mAuxRawImage.notNull())
        {
            mHasAux = true;
            sAuxCount++;
        }
        if (finished)
        {
            mIsFetching = false;
            mLastFetchState = -1;
            mLastPacketTimer.reset();
            mFetchPriorityPosted = -1.f;
        }
        else
        {
            // (getFetchState()'s defaults -- state 0 = INVALID -- when there was no
            // worker with work, as before)
            mFetchState = fetch_info.mState;
            mDownloadProgress = fetch_info.mDataProgress;
            mRequestedDownloadPriority = fetch_info.mRequestedPriority;
            mFetchPriority = fetch_info.mFetchPriority;
            mFetchDeltaTime = fetch_info.mFetchDeltaTime;
            mRequestDeltaTime = fetch_info.mRequestDeltaTime;
            if (fetch_info.mValid)
            {
                mCanUseHTTP = fetch_info.mCanUseHTTP;
            }
        }

        if (!processFetchResults(desired_discard, current_discard, fetch_discard, decode_priority))
        {
            return false;
        }

        if (mIsFetching)
        {
            static const F32 MAX_HOLD_TIME = 5.0f; //seconds to wait before canceling fecthing if decode_priority is 0.f.
            // (#485: a demand-deferred fetch already waited SLFetchMaxQueuedAge -- no hold)
            if (decode_priority > 0.0f || vk_deferred || mStopFetchingTimer.getElapsedTimeF32() > MAX_HOLD_TIME)
            {
                mStopFetchingTimer.reset();
                // #305: only when it moved (1/16 relative step) or crossed zero (zero is
                // the worker's abort signal) -- it was posted every frame, unchanged or not
                const F32 last = mFetchPriorityPosted;
                const bool changed = last < 0.f
                    || ((decode_priority <= 0.f) != (last <= 0.f))
                    || (fabsf(decode_priority - last) > last * (1.f / 16.f));
                if (changed)
                {
                    if (net_gated && last > 0.f) ++sNetGateDropped;   // #485
                    LLAppViewer::getTextureFetch()->updateRequestPriority(mID, decode_priority);
                    mFetchPriorityPosted = decode_priority;
                }
            }
        }
    }

    desired_discard = llmin(desired_discard, getMaxDiscardLevel());

    bool make_request = true;
    if (decode_priority <= 0)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("vftuf - priority <= 0");
        make_request = false;
    }
    else if (mDesiredDiscardLevel > getMaxDiscardLevel())
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("vftuf - desired > max");
        make_request = false;
    }
    else  if (mNeedsCreateTexture || mIsMissingAsset)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("vftuf - create or missing");
        make_request = false;
    }
    else if (current_discard >= 0 && current_discard <= mMinDiscardLevel)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("vftuf - current < min");
        make_request = false;
    }

    if (make_request)
    {
        if (mIsFetching)
        {
            // already requested a higher resolution mip
            if (mRequestedDiscardLevel <= desired_discard)
            {
                LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("vftuf - requested < desired");
                make_request = false;
            }
        }
        else
        {
            // already at a higher resolution mip, don't discard
            if (current_discard >= 0 && current_discard <= desired_discard)
            {
                LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("vftuf - current <= desired");
                make_request = false;
            }
        }
    }

    if (make_request)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("vftuf - make request");
        S32 w = 0, h = 0, c = 0;
        if (getDiscardLevel() >= 0)
        {
            w = mTextureData->getWidth(0);
            h = mTextureData->getHeight(0);
            c = mComponents;
        }

        static LLCachedControl<U32> texture_discard_level(gSavedSettings, "TextureDiscardLevel");
        const U32 override_tex_discard_level = texture_discard_level;
        if (override_tex_discard_level != 0)
        {
            desired_discard = override_tex_discard_level;
        }

        // bypass texturefetch directly by pulling from LLTextureCache
        S32 fetch_request_response = -1;
        S32 worker_discard = -1;
        fetch_request_response = LLAppViewer::getTextureFetch()->createRequest(mFTType, mUrl, getID(), getTargetHost(), decode_priority,
            w, h, c, desired_discard, needsAux(), mCanUseHTTP);

        if (fetch_request_response >= 0) // positive values and 0 are discard values
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("vftuf - request created");
            mHasFetcher = true;
            mIsFetching = true;
            mLastWorkerDiscardLevel = worker_discard;
            // in some cases createRequest can modify discard, as an example
            // bake textures are always at discard 0
            mRequestedDiscardLevel = llmin(desired_discard, fetch_request_response);
            mFetchPriorityPosted = decode_priority;  // #305: createRequest() set it
            mFetchState = LLAppViewer::getTextureFetch()->getFetchState(mID, mDownloadProgress, mRequestedDownloadPriority,
                mFetchPriority, mFetchDeltaTime, mRequestDeltaTime, mCanUseHTTP);
        }
        else if (fetch_request_response == LLTextureFetch::CREATE_REQUEST_ERROR_TRANSITION)
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("vftuf - processing transition error");
            // Request wasn't created because similar one finished or is in a transitional state, check worker state
            // As an example can happen if an image (like a server bake always fetches at dis 0), was scaled down to
            // needed discard after fetching then sudenly needed higher dis and worker wasn't yet deleted. Worker
            // discard will be identical to requested one and worker will have nothing new to do despite GL image
            // not being up to data.
            S32 desired_discard;
            S32 decoded_discard;
            bool decoded;
            S32 fetch_state = LLAppViewer::getTextureFetch()->getLastFetchState(mID, desired_discard, decoded_discard, decoded);
            if (fetch_state > 1 && decoded && decoded_discard >= 0 && decoded_discard <= desired_discard)
            {
                // worker actually has the image
                if (mRawImage.notNull()) sRawCount--;
                if (mAuxRawImage.notNull()) sAuxCount--;
                decoded_discard = LLAppViewer::getTextureFetch()->getLastRawImage(getID(), mRawImage, mAuxRawImage);
                if (mRawImage.notNull()) sRawCount++;
                if (mAuxRawImage.notNull())
                {
                    mHasAux = true;
                    sAuxCount++;
                }
                processFetchResults(desired_discard, current_discard, decoded_discard, decode_priority);
            }
        }

        // If createRequest() failed, that means one of two things:
        // 1. We're finishing up a request for this UUID, so we
        //    should wait for it to complete
        // 2. We've failed a request for this UUID, so there is
        //    no need to create another request
    }
    else if (mHasFetcher && !mIsFetching)
    {
        // Only delete requests that haven't received any network data
        // for a while.  Note - this is the normal mechanism for
        // deleting requests, not just a place to handle timeouts.
        const F32 FETCH_IDLE_TIME = 0.1f;
        if (mLastPacketTimer.getElapsedTimeF32() > FETCH_IDLE_TIME)
        {
            LL_DEBUGS("Texture") << "exceeded idle time " << FETCH_IDLE_TIME << ", deleting request: " << getID() << LL_ENDL;
            LLAppViewer::getTextureFetch()->deleteRequest(getID(), true);
            mHasFetcher = false;
        }
    }

    return mIsFetching;
}

void LLViewerFetchedTexture::clearFetchedResults()
{
    if(mNeedsCreateTexture || mIsFetching)
    {
        return;
    }

    cleanup();

    if(getDiscardLevel() >= 0) //sculpty texture, force to invalidate
    {
        mTextureData->invalidateDiscard();
    }
}

void LLViewerFetchedTexture::forceToDeleteRequest()
{
    if (mHasFetcher)
    {
        mHasFetcher = false;
        mIsFetching = false;
    }

    resetTextureStats();

    mDesiredDiscardLevel = getMaxDiscardLevel() + 1;
}

void LLViewerFetchedTexture::setIsMissingAsset(bool is_missing)
{
    if (is_missing == mIsMissingAsset)
    {
        return;
    }
    if (is_missing)
    {
        if (mUrl.empty())
        {
            LL_WARNS() << mID << ": Marking image as missing" << LL_ENDL;
        }
        else
        {
            // This may or may not be an error - it is normal to have no
            // map tile on an empty region, but bad if we're failing on a
            // server bake texture.
            if (getFTType() != FTT_MAP_TILE)
            {
                LL_WARNS() << mUrl << ": Marking image as missing" << LL_ENDL;
            }
        }
        if (mHasFetcher)
        {
            LLAppViewer::getTextureFetch()->deleteRequest(getID(), true);
            mHasFetcher = false;
            mIsFetching = false;
            mLastPacketTimer.reset();
            mFetchState = 0;
            mFetchPriority = 0;
        }
    }
    else
    {
        LL_INFOS() << mID << ": un-flagging missing asset" << LL_ENDL;
    }
    mIsMissingAsset = is_missing;
}

void LLViewerFetchedTexture::setLoadedCallback( loaded_callback_func loaded_callback,
                                       S32 discard_level, bool keep_imageraw, bool needs_aux, void* userdata,
                                       LLLoadedCallbackEntry::source_callback_list_t* src_callback_list, bool pause)
{
    //
    // Don't do ANYTHING here, just add it to the global callback list
    //
    if (mLoadedCallbackList.empty())
    {
        // Put in list to call this->doLoadedCallbacks() periodically
        gTextureList.mCallbackList.insert(this);
        mLoadedCallbackDesiredDiscardLevel = (S8)discard_level;
    }
    else
    {
        mLoadedCallbackDesiredDiscardLevel = llmin(mLoadedCallbackDesiredDiscardLevel, (S8)discard_level);
    }

    if(mPauseLoadedCallBacks)
    {
        if(!pause)
        {
            unpauseLoadedCallbacks(src_callback_list);
        }
    }
    else if(pause)
    {
        pauseLoadedCallbacks(src_callback_list);
    }

    LLLoadedCallbackEntry* entryp = new LLLoadedCallbackEntry(loaded_callback, discard_level, keep_imageraw, userdata, src_callback_list, this, pause);
    mLoadedCallbackList.push_back(entryp);
    ++mLoadedCallbackAdds;  // #309: a new callback may run with data already here

    mNeedsAux |= needs_aux;
    if(keep_imageraw)
    {
        mSaveRawImage = true;
    }
    if (mNeedsAux && mAuxRawImage.isNull() && getDiscardLevel() >= 0)
    {
        if(mHasAux)
        {
            //trigger a refetch
            forceToRefetchTexture();
        }
        else
        {
            // We need aux data, but we've already loaded the image, and it didn't have any
            LL_WARNS() << "No aux data available for callback for image:" << getID() << LL_ENDL;
        }
    }
    mLastCallBackActiveTime = sCurrentTime ;
        mLastReferencedSavedRawImageTime = sCurrentTime;
}

void LLViewerFetchedTexture::clearCallbackEntryList()
{
    if(mLoadedCallbackList.empty())
    {
        return;
    }

    for(callback_list_t::iterator iter = mLoadedCallbackList.begin();
            iter != mLoadedCallbackList.end(); )
    {
        LLLoadedCallbackEntry *entryp = *iter;

        // We never finished loading the image.  Indicate failure.
        // Note: this allows mLoadedCallbackUserData to be cleaned up.
        entryp->mCallback(false, this, NULL, NULL, 0, true, entryp->mUserData);
        iter = mLoadedCallbackList.erase(iter);
        delete entryp;
    }
    gTextureList.mCallbackList.erase(this);

    mLoadedCallbackDesiredDiscardLevel = S8_MAX;
    if(needsToSaveRawImage())
    {
        destroySavedRawImage();
    }

    return;
}

void LLViewerFetchedTexture::deleteCallbackEntry(const LLLoadedCallbackEntry::source_callback_list_t* callback_list)
{
    if(mLoadedCallbackList.empty() || !callback_list)
    {
        return;
    }

    S32 desired_discard = S8_MAX;
    S32 desired_raw_discard = INVALID_DISCARD_LEVEL;
    for(callback_list_t::iterator iter = mLoadedCallbackList.begin();
            iter != mLoadedCallbackList.end(); )
    {
        LLLoadedCallbackEntry *entryp = *iter;
        if(entryp->mSourceCallbackList == callback_list)
        {
            // We never finished loading the image.  Indicate failure.
            // Note: this allows mLoadedCallbackUserData to be cleaned up.
            entryp->mCallback(false, this, NULL, NULL, 0, true, entryp->mUserData);
            iter = mLoadedCallbackList.erase(iter);
            delete entryp;
        }
        else
        {
            ++iter;

            desired_discard = llmin(desired_discard, entryp->mDesiredDiscard);
            if(entryp->mNeedsImageRaw)
            {
                desired_raw_discard = llmin(desired_raw_discard, entryp->mDesiredDiscard);
            }
        }
    }

    mLoadedCallbackDesiredDiscardLevel = desired_discard;
    if (mLoadedCallbackList.empty())
    {
        // If we have no callbacks, take us off of the image callback list.
        gTextureList.mCallbackList.erase(this);

        if(needsToSaveRawImage())
        {
            destroySavedRawImage();
        }
    }
    else if(needsToSaveRawImage() && mBoostLevel != LLGLTexture::BOOST_PREVIEW)
    {
        if(desired_raw_discard != INVALID_DISCARD_LEVEL)
        {
            mDesiredSavedRawDiscardLevel = desired_raw_discard;
        }
        else
        {
            destroySavedRawImage();
        }
    }
}

void LLViewerFetchedTexture::unpauseLoadedCallbacks(const LLLoadedCallbackEntry::source_callback_list_t* callback_list)
{
    if(!callback_list)
{
        mPauseLoadedCallBacks = false;
        return;
    }

    bool need_raw = false;
    for(callback_list_t::iterator iter = mLoadedCallbackList.begin();
            iter != mLoadedCallbackList.end(); )
    {
        LLLoadedCallbackEntry *entryp = *iter++;
        if(entryp->mSourceCallbackList == callback_list)
        {
            entryp->mPaused = false;
            if(entryp->mNeedsImageRaw)
            {
                need_raw = true;
            }
        }
    }
    mPauseLoadedCallBacks = false ;
    mLastCallBackActiveTime = sCurrentTime ;
    mForceCallbackFetch = true;
    if(need_raw)
    {
        mSaveRawImage = true;
    }
}

void LLViewerFetchedTexture::pauseLoadedCallbacks(const LLLoadedCallbackEntry::source_callback_list_t* callback_list)
{
    if(!callback_list)
{
        return;
    }

    bool paused = true;

    for(callback_list_t::iterator iter = mLoadedCallbackList.begin();
            iter != mLoadedCallbackList.end(); )
    {
        LLLoadedCallbackEntry *entryp = *iter++;
        if(entryp->mSourceCallbackList == callback_list)
        {
            entryp->mPaused = true;
        }
        else if(!entryp->mPaused)
        {
            paused = false;
        }
    }

    if(paused)
    {
        mPauseLoadedCallBacks = true;//when set, loaded callback is paused.
        resetTextureStats();
        mSaveRawImage = false;
    }
}

// #309
LLViewerFetchedTexture::LoadedCallbackState LLViewerFetchedTexture::loadedCallbackState() const
{
    LoadedCallbackState st;
    st.mDiscard = getDiscardLevel();
    st.mMaxDiscard = getMaxDiscardLevel();
    st.mRawDiscard = mRawDiscardLevel;
    st.mCallbacks = (U32)mLoadedCallbackList.size();
    st.mAdds = mLoadedCallbackAdds;
    st.mActiveTime = mLastCallBackActiveTime;
    st.mNeedsCreate = mNeedsCreateTexture;
    st.mPaused = mPauseLoadedCallBacks;
    st.mMissing = mIsMissingAsset;
    st.mFetching = mIsFetching;
    st.mRawValid = mIsRawImageValid;
    st.mHaveRaw = mRawImage.notNull();
    st.mNeedsAux = mNeedsAux;
    st.mHaveAux = mAuxRawImage.notNull();
    st.mForceFetch = mForceCallbackFetch;
    return st;
}

// #309
bool LLViewerFetchedTexture::loadedCallbacksNeedUpdate() const
{
    if (mLoadedCallbackLastTime < 0.f
        || sCurrentTime - mLoadedCallbackLastTime >= LOADED_CALLBACK_RECHECK_SECS
        || sCurrentTime < mLoadedCallbackLastTime)
    {
        return true;
    }
    return !(loadedCallbackState() == mLoadedCallbackLast);
}

// #309
void LLViewerFetchedTexture::noteLoadedCallbacksRan()
{
    mLoadedCallbackLast = loadedCallbackState();
    mLoadedCallbackLastTime = sCurrentTime;
}

bool LLViewerFetchedTexture::doLoadedCallbacks()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    static const F32 MAX_INACTIVE_TIME = 900.f ; //seconds
    static const F32 MAX_IDLE_WAIT_TIME = 5.f ; //seconds

    if (mNeedsCreateTexture)
    {
        return false;
    }
    if(mPauseLoadedCallBacks)
    {
        destroyRawImage();
        return false; //paused
    }
    if(sCurrentTime - mLastCallBackActiveTime > MAX_INACTIVE_TIME && !mIsFetching)
    {
        if (mFTType == FTT_SERVER_BAKE)
        {
            //output some debug info
            LL_INFOS() << "baked texture: " << mID << "clears all call backs due to inactivity." << LL_ENDL;
            LL_INFOS() << mUrl << LL_ENDL;
            LL_INFOS() << "current discard: " << getDiscardLevel() << " current discard for fetch: " << getCurrentDiscardLevelForFetching() <<
                " Desired discard: " << getDesiredDiscardLevel() << "decode Pri: " << mMaxVirtualSize << LL_ENDL;
        }

        clearCallbackEntryList() ; //remove all callbacks.
        return false ;
    }

    bool res = false;

    if (isMissingAsset())
    {
        if (mFTType == FTT_SERVER_BAKE)
        {
            //output some debug info
            LL_INFOS() << "baked texture: " << mID << "is missing." << LL_ENDL;
            LL_INFOS() << mUrl << LL_ENDL;
        }

        for(callback_list_t::iterator iter = mLoadedCallbackList.begin();
            iter != mLoadedCallbackList.end(); )
        {
            LLLoadedCallbackEntry *entryp = *iter++;
            // We never finished loading the image.  Indicate failure.
            // Note: this allows mLoadedCallbackUserData to be cleaned up.
            entryp->mCallback(false, this, NULL, NULL, 0, true, entryp->mUserData);
            delete entryp;
        }
        mLoadedCallbackList.clear();

        // Remove ourself from the global list of textures with callbacks
        gTextureList.mCallbackList.erase(this);
        return false;
    }

    S32 gl_discard = getDiscardLevel();

    // If we don't have a legit GL image, set it to be lower than the worst discard level
    if (gl_discard == -1)
    {
        gl_discard = MAX_DISCARD_LEVEL + 1;
    }

    //
    // Determine the quality levels of textures that we can provide to callbacks
    // and whether we need to do decompression to get it
    //
    S32 current_raw_discard = MAX_DISCARD_LEVEL + 1;
    S32 current_aux_discard = MAX_DISCARD_LEVEL + 1;
    S32 best_aux_discard = MAX_DISCARD_LEVEL + 1;

    if (mIsRawImageValid)
    {
        // If we have an existing raw image, we have a baseline for the raw and auxiliary quality levels.
        current_raw_discard = mRawDiscardLevel;
        best_aux_discard = llmin(best_aux_discard, mRawDiscardLevel); // We always decode the aux when we decode the base raw
        current_aux_discard = llmin(current_aux_discard, best_aux_discard);
    }
    else
    {
        // We have no data at all, we need to get it
        // Do this by forcing the best aux discard to be 0.
        best_aux_discard = 0;
    }


    //
    // See if any of the callbacks would actually run using the data that we can provide,
    // and also determine if we need to perform any readbacks or decodes.
    //
    bool run_gl_callbacks = false;
    bool run_raw_callbacks = false;
    // #655: LL read the GL texture back when a raw callback wanted more than the current
    // raw image held (need_readback / readbackRawImage). That was dropped with GL, so a
    // callback registered after the texture had already decoded (Save As on a preview
    // that finished loading; the raw is turned into the saved copy right after decode)
    // never got pixels, and destroySavedRawImage() failed it 20 s later ("Unable to
    // download file"). The CPU-side pixels are the readback source now.
    const S32 best_readback_discard = mIsRawImageValid ? (MAX_DISCARD_LEVEL + 1) : vkReadbackDiscard();
    bool need_readback = false;

    for(callback_list_t::iterator iter = mLoadedCallbackList.begin();
        iter != mLoadedCallbackList.end(); )
    {
        LLLoadedCallbackEntry *entryp = *iter++;

        if (entryp->mNeedsImageRaw)
        {
            if (mNeedsAux)
            {
                //
                // Need raw and auxiliary channels
                //
                if (entryp->mLastUsedDiscard > current_aux_discard)
                {
                    // We have useful data, run the callbacks
                    run_raw_callbacks = true;
                }
            }
            else
            {
                if (entryp->mLastUsedDiscard > current_raw_discard)
                {
                    // We have useful data, just run the callbacks
                    run_raw_callbacks = true;
                }
                else if (entryp->mLastUsedDiscard > best_readback_discard)
                {
                    // We can copy the CPU-side pixels, and then run the callbacks
                    need_readback = true;
                    run_raw_callbacks = true;
                }
            }
        }
        else
        {
            // Needs just GL
            if (entryp->mLastUsedDiscard > gl_discard)
            {
                // We have enough data, run this callback requiring GL data
                run_gl_callbacks = true;
            }
        }
    }

    if (need_readback)
    {
        vkReadbackRawImage();
    }
    else if (!mIsRawImageValid && mTextureData.notNull() && mTextureData->isVKCpuDropped())
    {
        // #707: no raw and the CPU copy was dropped: a raw callback that still wants
        // pixels re-sources them now (was: 5 s idle, then forceToRefetchTexture).
        for (const LLLoadedCallbackEntry* entryp : mLoadedCallbackList)
        {
            if (entryp->mNeedsImageRaw && entryp->mLastUsedDiscard > entryp->mDesiredDiscard)
            {
                vkRequestCpuCopy(llclamp((S32)mLoadedCallbackDesiredDiscardLevel, 0, (S32)MAX_DISCARD_LEVEL));
                break;
            }
        }
    }

    //
    // Run raw/auxiliary data callbacks
    //
    if (run_raw_callbacks && mIsRawImageValid && (mRawDiscardLevel <= getMaxDiscardLevel()))
    {
        // Do callbacks which require raw image data.
        //LL_INFOS() << "doLoadedCallbacks raw for " << getID() << LL_ENDL;

        // Call each party interested in the raw data.
        for(callback_list_t::iterator iter = mLoadedCallbackList.begin();
            iter != mLoadedCallbackList.end(); )
        {
            callback_list_t::iterator curiter = iter++;
            LLLoadedCallbackEntry *entryp = *curiter;
            if (entryp->mNeedsImageRaw && (entryp->mLastUsedDiscard > mRawDiscardLevel))
            {
                // If we've loaded all the data there is to load or we've loaded enough
                // to satisfy the interested party, then this is the last time that
                // we're going to call them.

                mLastCallBackActiveTime = sCurrentTime;
                if(mNeedsAux && mAuxRawImage.isNull())
                {
                    LL_WARNS() << "Raw Image with no Aux Data for callback" << LL_ENDL;
                }
                bool final = mRawDiscardLevel <= entryp->mDesiredDiscard;
                //LL_INFOS() << "Running callback for " << getID() << LL_ENDL;
                //LL_INFOS() << mRawImage->getWidth() << "x" << mRawImage->getHeight() << LL_ENDL;
                entryp->mLastUsedDiscard = mRawDiscardLevel;
                entryp->mCallback(true, this, mRawImage, mAuxRawImage, mRawDiscardLevel, final, entryp->mUserData);
                if (final)
                {
                    iter = mLoadedCallbackList.erase(curiter);
                    delete entryp;
                }
                res = true;
            }
        }
    }

    //
    // Run GL callbacks
    //
    if (run_gl_callbacks && (gl_discard <= getMaxDiscardLevel()))
    {
        //LL_INFOS() << "doLoadedCallbacks GL for " << getID() << LL_ENDL;

        // Call the callbacks interested in GL data.
        for(callback_list_t::iterator iter = mLoadedCallbackList.begin();
            iter != mLoadedCallbackList.end(); )
        {
            callback_list_t::iterator curiter = iter++;
            LLLoadedCallbackEntry *entryp = *curiter;
            if (!entryp->mNeedsImageRaw && (entryp->mLastUsedDiscard > gl_discard))
            {
                mLastCallBackActiveTime = sCurrentTime;
                bool final = gl_discard <= entryp->mDesiredDiscard;
                entryp->mLastUsedDiscard = gl_discard;
                entryp->mCallback(true, this, NULL, NULL, gl_discard, final, entryp->mUserData);
                if (final)
                {
                    iter = mLoadedCallbackList.erase(curiter);
                    delete entryp;
                }
                res = true;
            }
        }
    }

    // Done with any raw image data at this point (will be re-created if we still have callbacks)
    destroyRawImage();

    //
    // If we have no callbacks, take us off of the image callback list.
    //
    if (mLoadedCallbackList.empty())
    {
        gTextureList.mCallbackList.erase(this);
    }
    else if(!res && mForceCallbackFetch && sCurrentTime - mLastCallBackActiveTime > MAX_IDLE_WAIT_TIME && !mIsFetching)
    {
        //wait for long enough but no fetching request issued, force one.
        forceToRefetchTexture(mLoadedCallbackDesiredDiscardLevel, 5.f);
        mForceCallbackFetch = false; //fire once.
    }

    return res;
}

//virtual
void LLViewerFetchedTexture::forceImmediateUpdate()
{
    //only immediately update a deleted texture which is now being re-used.
    if(!isDeleted())
    {
        return;
    }
    //if already called forceImmediateUpdate()
    if(mInImageList && mMaxVirtualSize == LLViewerFetchedTexture::sMaxVirtualSize)
    {
        return;
    }

    gTextureList.forceImmediateUpdate(this);
    return;
}

bool LLViewerFetchedTexture::needsToSaveRawImage()
{
    return mForceToSaveRawImage || mSaveRawImage;
}

void LLViewerFetchedTexture::destroyRawImage()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    if (mAuxRawImage.notNull() && !needsToSaveRawImage())
    {
        sAuxCount--;
        mAuxRawImage = nullptr;
    }

    if (mRawImage.notNull())
    {
        sRawCount--;

        if(mIsRawImageValid)
        {
            if(needsToSaveRawImage())
            {
                saveRawImage();
            }
        }

        mRawImage = nullptr;

        mIsRawImageValid = false;
        mRawDiscardLevel = INVALID_DISCARD_LEVEL;
    }
}

void LLViewerFetchedTexture::saveRawImage()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    if(mRawImage.isNull() || mRawImage == mSavedRawImage || (mSavedRawDiscardLevel >= 0 && mSavedRawDiscardLevel <= mRawDiscardLevel))
    {
        return;
    }

    LLImageDataSharedLock lock(mRawImage);

    mSavedRawDiscardLevel = mRawDiscardLevel;
    if (mBoostLevel == LLGLTexture::BOOST_ICON)
    {
        S32 expected_width = mKnownDrawWidth > 0 ? mKnownDrawWidth : DEFAULT_ICON_DIMENSIONS;
        S32 expected_height = mKnownDrawHeight > 0 ? mKnownDrawHeight : DEFAULT_ICON_DIMENSIONS;
        if (mRawImage->getWidth() > expected_width || mRawImage->getHeight() > expected_height)
        {
            mSavedRawImage = new LLImageRaw(expected_width, expected_height, mRawImage->getComponents());
            mSavedRawImage->copyScaled(mRawImage);
        }
        else
        {
            mSavedRawImage = new LLImageRaw(mRawImage->getData(), mRawImage->getWidth(), mRawImage->getHeight(), mRawImage->getComponents());
        }
    }
    else if (mBoostLevel == LLGLTexture::BOOST_THUMBNAIL)
    {
        if (mRawImage->getWidth() > DEFAULT_THUMBNAIL_DIMENSIONS || mRawImage->getHeight() > DEFAULT_THUMBNAIL_DIMENSIONS)
        {
            mSavedRawImage = new LLImageRaw(DEFAULT_THUMBNAIL_DIMENSIONS, DEFAULT_THUMBNAIL_DIMENSIONS, mRawImage->getComponents());
            mSavedRawImage->copyScaled(mRawImage);
        }
        else
        {
            mSavedRawImage = new LLImageRaw(mRawImage->getData(), mRawImage->getWidth(), mRawImage->getHeight(), mRawImage->getComponents());
        }
    }
    // BOOST_SCULPTED: no downscale. The saved raw IS the sculpt displacement source
    // (LLVOVolume::sculpt reads it directly). LL used to squash any map larger than the
    // known draw size (256x256) into a 256x256 square, relying on sculpt() to read the
    // full-resolution map back from the GL texture instead. There is no GL texture, so
    // the squashed copy was displaced as-is: a 512x8 tree-line strip became 256x256, its
    // aspect ratio (which picks the sculpt grid shape) was destroyed, and the strip
    // fanned into long streaks. Sculpt maps are tiny; keep them at full resolution.
    else
    {
        mSavedRawImage = new LLImageRaw(mRawImage->getData(), mRawImage->getWidth(), mRawImage->getHeight(), mRawImage->getComponents());
    }

    if(mForceToSaveRawImage && mSavedRawDiscardLevel <= mDesiredSavedRawDiscardLevel)
    {
        mForceToSaveRawImage = false;
    }

    mLastReferencedSavedRawImageTime = sCurrentTime;
}

//force to refetch the texture to the discard level
void LLViewerFetchedTexture::forceToRefetchTexture(S32 desired_discard, F32 kept_time)
{
    if(mForceToSaveRawImage)
    {
        desired_discard = llmin(desired_discard, mDesiredSavedRawDiscardLevel);
        kept_time = llmax(kept_time, mKeptSavedRawImageTime);
    }

    //trigger a new fetch.
    mForceToSaveRawImage = true ;
    mDesiredSavedRawDiscardLevel = desired_discard ;
    mKeptSavedRawImageTime = kept_time ;
    mLastReferencedSavedRawImageTime = sCurrentTime ;
    mSavedRawImage = NULL ;
    mSavedRawDiscardLevel = -1 ;
}

void LLViewerFetchedTexture::forceToSaveRawImage(S32 desired_discard, F32 kept_time)
{
    mKeptSavedRawImageTime = kept_time;
    mLastReferencedSavedRawImageTime = sCurrentTime;

    if(mSavedRawDiscardLevel > -1 && mSavedRawDiscardLevel <= desired_discard)
    {
        return; //raw imge is ready.
    }

    if(!mForceToSaveRawImage || mDesiredSavedRawDiscardLevel < 0 || mDesiredSavedRawDiscardLevel > desired_discard)
    {
        mForceToSaveRawImage = true;
        mDesiredSavedRawDiscardLevel = desired_discard;
    }
}

// #655: see doLoadedCallbacks(). Only exact-size copies qualify: the discard level is
// derived from the copy's size (as processFetchResults does), so a scaled icon/thumbnail
// copy or a CPU copy mid-downscale is never handed out as a sharper level than it is.
static S32 vk_readback_level(const LLImageRaw* raw, S32 full_w, S32 full_h)
{
    if (!raw || raw->isBufferInvalid() || !raw->getData() || full_w <= 0 || full_h <= 0)
    {
        return MAX_DISCARD_LEVEL + 1;
    }
    for (S32 d = 0; d <= MAX_DISCARD_LEVEL; ++d)
    {
        if ((raw->getWidth() << d) == full_w && (raw->getHeight() << d) == full_h)
        {
            return d;
        }
    }
    return MAX_DISCARD_LEVEL + 1;
}

S32 LLViewerFetchedTexture::vkReadbackDiscard() const
{
    S32 best = MAX_DISCARD_LEVEL + 1;
    if (mSavedRawImage.notNull() && mSavedRawDiscardLevel >= 0)
    {
        const S32 d = vk_readback_level(mSavedRawImage, mFullWidth, mFullHeight);
        if (d == mSavedRawDiscardLevel)
        {
            best = d;
        }
    }
    const LLTextureData* tex = mTextureData.get();
    if (tex && !mVKScalePending && !tex->isVKCpuWritable())
    {
        best = llmin(best, vk_readback_level(tex->getVKCpuData(), mFullWidth, mFullHeight));
    }
    return best;
}

bool LLViewerFetchedTexture::vkReadbackRawImage()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    if (mRawImage.notNull() || mIsRawImageValid)
    {
        return false;   // never replace pixels the fetcher handed us
    }

    const LLImageRaw* src = nullptr;
    S32 src_discard = MAX_DISCARD_LEVEL + 1;
    if (mSavedRawImage.notNull() && mSavedRawDiscardLevel >= 0
        && vk_readback_level(mSavedRawImage, mFullWidth, mFullHeight) == mSavedRawDiscardLevel)
    {
        src = mSavedRawImage;
        src_discard = mSavedRawDiscardLevel;
    }
    const LLTextureData* tex = mTextureData.get();
    if (tex && !mVKScalePending && !tex->isVKCpuWritable())
    {
        const LLImageRaw* cpu = tex->getVKCpuData();
        const S32 d = vk_readback_level(cpu, mFullWidth, mFullHeight);
        if (d < src_discard)
        {
            src = cpu;
            src_discard = d;
        }
    }
    if (!src || src_discard > MAX_DISCARD_LEVEL)
    {
        return false;
    }

    LLImageDataSharedLock lock(src);
    // A private copy, as LL's GL readback made one: callbacks receive a non-const raw.
    mRawImage = new LLImageRaw(src->getData(), (U16)src->getWidth(), (U16)src->getHeight(), src->getComponents());
    sRawCount++;
    mIsRawImageValid = true;
    mRawDiscardLevel = src_discard;
    LL_DEBUGS("FileSaveAs") << "Raw image for load callbacks of " << mID << " copied from CPU-side pixels at discard "
                            << src_discard << " (" << src->getWidth() << "x" << src->getHeight() << ")" << LL_ENDL;
    return true;
}

void LLViewerFetchedTexture::destroySavedRawImage()
{
    if(mLastReferencedSavedRawImageTime < mKeptSavedRawImageTime)
    {
        return; //keep the saved raw image.
    }

    mForceToSaveRawImage  = false;
    mSaveRawImage = false;

    clearCallbackEntryList();

    mSavedRawImage = NULL ;
    mForceToSaveRawImage  = false ;
    mSaveRawImage = false ;
    mSavedRawDiscardLevel = -1 ;
    mDesiredSavedRawDiscardLevel = -1 ;
    mLastReferencedSavedRawImageTime = 0.0f ;
    mKeptSavedRawImageTime = 0.f ;

    if(mAuxRawImage.notNull())
    {
        sAuxCount--;
        mAuxRawImage = NULL;
    }
}

LLImageRaw* LLViewerFetchedTexture::getSavedRawImage()
{
    mLastReferencedSavedRawImageTime = sCurrentTime;

    return mSavedRawImage;
}

const LLImageRaw* LLViewerFetchedTexture::getSavedRawImage() const
{
    return mSavedRawImage;
}

bool LLViewerFetchedTexture::hasSavedRawImage() const
{
    return mSavedRawImage.notNull();
}

F32 LLViewerFetchedTexture::getElapsedLastReferencedSavedRawImageTime() const
{
    return sCurrentTime - mLastReferencedSavedRawImageTime;
}

LLUUID LLViewerFetchedTexture::getUploader()
{
    return (mComment.find('a') != mComment.end()) ? LLUUID(mComment['a']) : LLUUID::null;
}

LLDate LLViewerFetchedTexture::getUploadTime()
{
    if (mComment.find('z') != mComment.end())
    {
        struct tm t = {};
        sscanf(mComment['z'].c_str(), "%4d%2d%2d%2d%2d%2d",
               &t.tm_year, &t.tm_mon, &t.tm_mday, &t.tm_hour, &t.tm_min, &t.tm_sec);
        std::string iso_date = llformat("%d-%d-%dT%d:%d:%dZ", t.tm_year, t.tm_mon, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
        return LLDate(iso_date);
    }
    return LLDate();
}

std::string LLViewerFetchedTexture::getComment()
{
    return (mComment.find('K') != mComment.end()) ? mComment['K'] : LLStringUtil::null;
}

//----------------------------------------------------------------------------------------------
//end of LLViewerFetchedTexture
//----------------------------------------------------------------------------------------------

//----------------------------------------------------------------------------------------------
//start of LLViewerLODTexture
//----------------------------------------------------------------------------------------------
LLViewerLODTexture::LLViewerLODTexture(const LLUUID& id, FTType f_type, const LLHost& host, bool usemipmaps)
    : LLViewerFetchedTexture(id, f_type, host, usemipmaps)
{
    init(true);
}

LLViewerLODTexture::LLViewerLODTexture(const std::string& url, FTType f_type, const LLUUID& id, bool usemipmaps)
    : LLViewerFetchedTexture(url, f_type, id, usemipmaps)
{
    init(true);
}

void LLViewerLODTexture::init(bool firstinit)
{
    mTexelsPerImage = 64*64;
}

//virtual
S8 LLViewerLODTexture::getType() const
{
    return LLViewerTexture::LOD_TEXTURE;
}

bool LLViewerLODTexture::isUpdateFrozen()
{
    return LLViewerTexture::sFreezeImageUpdates;
}

// This is gauranteed to get called periodically for every texture
//virtual
void LLViewerLODTexture::processTextureStats()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    updateVirtualSize();

    if (applyVKDemand())
    {
        vkCpuCopyStep();   // #707
        return;   // Vulkan: the on-screen demand sets the discard (see setVKDemand)
    }

    static LLCachedControl<bool> textures_fullres(gSavedSettings,"TextureLoadFullRes", false);

    F32 max_tex_res = MAX_IMAGE_SIZE_DEFAULT;
    if (mBoostLevel < LLGLTexture::BOOST_HIGH)
    {
        // restrict texture resolution to download based on RenderMaxTextureResolution
        static LLCachedControl<U32> max_texture_resolution(gSavedSettings, "RenderMaxTextureResolution", 2048);
        // sanity clamp debug setting to avoid settings hack shenanigans
        max_tex_res = (F32)llclamp((S32)max_texture_resolution, 512, MAX_IMAGE_SIZE_DEFAULT);
        mMaxVirtualSize = llmin(mMaxVirtualSize, max_tex_res * max_tex_res);
    }

    if (textures_fullres)
    {
        mDesiredDiscardLevel = 0;
    }
    // Generate the request priority and render priority
    else if (mDontDiscard || !mUseMipMaps)
    {
        mDesiredDiscardLevel = 0;
        if (mFullWidth > MAX_IMAGE_SIZE_DEFAULT || mFullHeight > MAX_IMAGE_SIZE_DEFAULT)
            mDesiredDiscardLevel = 1; // MAX_IMAGE_SIZE_DEFAULT = 2048 and max size ever is 4096
    }
    else if (mBoostLevel < LLGLTexture::BOOST_HIGH && mMaxVirtualSize <= 10.f)
    {
        // If the image has not been significantly visible in a while, we don't want it
        mDesiredDiscardLevel = llmin(mMinDesiredDiscardLevel, (S8)(MAX_DISCARD_LEVEL + 1));
        mDesiredDiscardLevel = llmin(mDesiredDiscardLevel, (S32)mLoadedCallbackDesiredDiscardLevel);
    }
    else if (!mFullWidth  || !mFullHeight)
    {
        mDesiredDiscardLevel =  getMaxDiscardLevel();
        mDesiredDiscardLevel = llmin(mDesiredDiscardLevel, (S32)mLoadedCallbackDesiredDiscardLevel);
    }
    else
    {
        //static const F64 log_2 = log(2.0);
        static const F64 log_4 = log(4.0);

        F32 discard_level = 0.f;

        // If we know the output width and height, we can force the discard
        // level to the correct value, and thus not decode more texture
        // data than we need to.
        if (mKnownDrawWidth && mKnownDrawHeight)
        {
            S32 draw_texels = mKnownDrawWidth * mKnownDrawHeight;
            draw_texels = llclamp(draw_texels, MIN_IMAGE_AREA, MAX_IMAGE_AREA);

            // Use log_4 because we're in square-pixel space, so an image
            // with twice the width and twice the height will have mTexelsPerImage
            // 4 * draw_size
            discard_level = (F32)(log(mTexelsPerImage / draw_texels) / log_4);
        }
        else
        {
            // Calculate the required scale factor of the image using pixels per texel
            discard_level = (F32)(log(mTexelsPerImage / mMaxVirtualSize) / log_4);
        }

        discard_level = floorf(discard_level);

        F32 min_discard = 0.f;
        if (mFullWidth > max_tex_res || mFullHeight > max_tex_res)
            min_discard = 1.f;

        discard_level = llclamp(discard_level, min_discard, (F32)MAX_DISCARD_LEVEL);

        // Can't go higher than the max discard level
        mDesiredDiscardLevel = llmin(getMaxDiscardLevel() + 1, (S32)discard_level);
        // Clamp to min desired discard
        mDesiredDiscardLevel = llmin(mMinDesiredDiscardLevel, mDesiredDiscardLevel);

        //
        // At this point we've calculated the quality level that we want,
        // if possible.  Now we check to see if we have it, and take the
        // proper action if we don't.
        //

        S32 current_discard = getDiscardLevel();

        if (isUpdateFrozen() // we are out of memory and nearing max allowed bias
            && mBoostLevel < LLGLTexture::BOOST_SCULPTED
            && mDesiredDiscardLevel < current_discard)
        {
            // stop requesting more
            mDesiredDiscardLevel = current_discard;
        }
        mDesiredDiscardLevel = llmin(mDesiredDiscardLevel, (S32)mLoadedCallbackDesiredDiscardLevel);
    }

    if(mForceToSaveRawImage && mDesiredSavedRawDiscardLevel >= 0)
    {
        mDesiredDiscardLevel = llmin(mDesiredDiscardLevel, (S8)mDesiredSavedRawDiscardLevel);
    }

    // selection manager will immediately reset BOOST_SELECTED but never unsets it
    // unset it immediately after we consume it
    if (getBoostLevel() == BOOST_SELECTED)
    {
        setBoostLevel(BOOST_NONE);
    }
    vkCpuCopyStep();   // #707
}

//----------------------------------------------------------------------------------------------
//end of LLViewerLODTexture
//----------------------------------------------------------------------------------------------

//----------------------------------------------------------------------------------------------
//start of LLViewerMediaTexture
//----------------------------------------------------------------------------------------------
//static
void LLViewerMediaTexture::updateClass()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    static const F32 MAX_INACTIVE_TIME = 30.f;

#if 0
    //force to play media.
    gSavedSettings.setBOOL("AudioStreamingMedia", true);
#endif

    for(media_map_t::iterator iter = sMediaMap.begin(); iter != sMediaMap.end(); )
    {
        LLViewerMediaTexture* mediap = iter->second;

        if(mediap->getNumRefs() == 1) //one reference by sMediaMap
        {
            //
            //Note: delay some time to delete the media textures to stop endlessly creating and immediately removing media texture.
            //
            if(mediap->getLastReferencedTimer()->getElapsedTimeF32() > MAX_INACTIVE_TIME)
            {
                media_map_t::iterator cur = iter++;
                sMediaMap.erase(cur);
                continue;
            }
        }
        ++iter;
    }
}

//static
void LLViewerMediaTexture::removeMediaImplFromTexture(const LLUUID& media_id)
{
    LLViewerMediaTexture* media_tex = findMediaTexture(media_id);
    if(media_tex)
    {
        media_tex->invalidateMediaImpl();
    }
}

//static
void LLViewerMediaTexture::cleanUpClass()
{
    sMediaMap.clear();
}

//static
LLViewerMediaTexture* LLViewerMediaTexture::findMediaTexture(const LLUUID& media_id)
{
    media_map_t::iterator iter = sMediaMap.find(media_id);
    if(iter == sMediaMap.end())
    {
        return NULL;
    }

    LLViewerMediaTexture* media_tex = iter->second;
    media_tex->setMediaImpl();
    media_tex->getLastReferencedTimer()->reset();

    return media_tex;
}

// virtual
U8 LLViewerMediaTexture::getVKUiKind() const
{
    return LLTextureData::VKUI_MEDIA;
}

LLViewerMediaTexture::LLViewerMediaTexture(const LLUUID& id, bool usemipmaps, LLTextureData* gl_image)
    : LLViewerTexture(id, usemipmaps),
    mMediaImplp(NULL),
    mUpdateVirtualSizeTime(0)
{
    sMediaMap.insert(std::make_pair(id, this));

    mTextureData = gl_image;

    if(mTextureData.isNull())
    {
        generateTextureData();
    }


    mTextureData->setNeedsAlphaAndPickMask(false);

    mIsPlaying = false;

    setMediaImpl();

    setCategory(LLGLTexture::MEDIA);

    LLViewerTexture* tex = gTextureList.findImage(mID, TEX_LIST_STANDARD);
    if(tex) //this media is a parcel media for tex.
    {
        tex->setParcelMedia(this);
    }
}

//virtual
LLViewerMediaTexture::~LLViewerMediaTexture()
{
    LLViewerTexture* tex = gTextureList.findImage(mID, TEX_LIST_STANDARD);
    if(tex) //this media is a parcel media for tex.
    {
        tex->setParcelMedia(NULL);
    }
}

void LLViewerMediaTexture::reinit(bool usemipmaps /* = true */)
{
    llassert(mTextureData.notNull());

    mUseMipMaps = usemipmaps;
    getLastReferencedTimer()->reset();
    mTextureData->setUseMipMaps(mUseMipMaps);
    mTextureData->setNeedsAlphaAndPickMask(false);
}

void LLViewerMediaTexture::setUseMipMaps(bool mipmap)
{
    mUseMipMaps = mipmap;

    if(mTextureData.notNull())
    {
        mTextureData->setUseMipMaps(mipmap);
    }
}

//virtual
S8 LLViewerMediaTexture::getType() const
{
    return LLViewerTexture::MEDIA_TEXTURE;
}

void LLViewerMediaTexture::invalidateMediaImpl()
{
    mMediaImplp = NULL;
}

void LLViewerMediaTexture::setMediaImpl()
{
    if(!mMediaImplp)
    {
        mMediaImplp = LLViewerMedia::getInstance()->getMediaImplFromTextureID(mID);
    }
}

//return true if all faces to reference to this media texture are found
//Note: mMediaFaceList is valid only for the current instant
//      because it does not check the face validity after the current frame.
bool LLViewerMediaTexture::findFaces()
{
    mMediaFaceList.clear();

    bool ret = true;

    LLViewerTexture* tex = gTextureList.findImage(mID, TEX_LIST_STANDARD);
    if(tex) //this media is a parcel media for tex.
    {
        for (U32 ch = 0; ch < LLRender::NUM_TEXTURE_CHANNELS; ++ch)
        {
            const ll_face_list_t* face_list = tex->getFaceList(ch);
            U32 end = tex->getNumFaces(ch);
        for(U32 i = 0; i < end; i++)
        {
            if ((*face_list)[i]->isMediaAllowed())
            {
                mMediaFaceList.push_back((*face_list)[i]);
            }
        }
    }
    }

    if(!mMediaImplp)
    {
        return true;
    }

    //for media on a face.
    const std::list< LLVOVolume* >* obj_list = mMediaImplp->getObjectList();
    std::list< LLVOVolume* >::const_iterator iter = obj_list->begin();
    for(; iter != obj_list->end(); ++iter)
    {
        LLVOVolume* obj = *iter;
        if (obj->isDead())
        {
            // Isn't supposed to happen, objects are supposed to detach
            // themselves on markDead()
            // If this happens, viewer is likely to crash
            llassert(0);
            LL_WARNS() << "Dead object in mMediaImplp's object list" << LL_ENDL;
            ret = false;
            continue;
        }

        if (obj->mDrawable.isNull() || obj->mDrawable->isDead())
        {
            ret = false;
            continue;
        }

        S32 face_id = -1;
        S32 num_faces = obj->mDrawable->getNumFaces();
        while((face_id = obj->getFaceIndexWithMediaImpl(mMediaImplp, face_id)) > -1 && face_id < num_faces)
        {
            LLFace* facep = obj->mDrawable->getFace(face_id);
            if(facep)
            {
                mMediaFaceList.push_back(facep);
            }
            else
            {
                ret = false;
            }
        }
    }

    return ret;
}

void LLViewerMediaTexture::initVirtualSize()
{
    if(mIsPlaying)
    {
        return;
    }

    findFaces();
    for(std::list< LLFace* >::iterator iter = mMediaFaceList.begin(); iter!= mMediaFaceList.end(); ++iter)
    {
        addTextureStats((*iter)->getVirtualSize());
    }
}

void LLViewerMediaTexture::addMediaToFace(LLFace* facep)
{
    if(facep)
    {
        facep->setHasMedia(true);
    }
    if(!mIsPlaying)
    {
        return; //no need to add the face because the media is not in playing.
    }

    switchTexture(LLRender::DIFFUSE_MAP, facep);
}

void LLViewerMediaTexture::removeMediaFromFace(LLFace* facep)
{
    if(!facep)
    {
        return;
    }
    facep->setHasMedia(false);

    if(!mIsPlaying)
    {
        return; //no need to remove the face because the media is not in playing.
    }

    mIsPlaying = false; //set to remove the media from the face.
    switchTexture(LLRender::DIFFUSE_MAP, facep);
    mIsPlaying = true; //set the flag back.

    if(getTotalNumFaces() < 1) //no face referencing to this media
    {
        stopPlaying();
    }
}

//virtual
void LLViewerMediaTexture::addFace(U32 ch, LLFace* facep)
{
    LLViewerTexture::addFace(ch, facep);

    const LLTextureEntry* te = facep->getTextureEntry();
    if(te && te->getID().notNull())
    {
        LLViewerTexture* tex = gTextureList.findImage(te->getID(), TEX_LIST_STANDARD);
        if(tex)
        {
// [SL:KB] - Patch: Render-TextureToggle (Catznip-5.2)
            // See LLViewerMediaTexture::removeFace()
            if (facep->isDefaultTexture(ch))
            {
                return;
            }
// [/SL:KB]

            mTextureList.push_back(tex);//increase the reference number by one for tex to avoid deleting it.
            return;
        }
    }

    //check if it is a parcel media
    if(facep->getTexture() && facep->getTexture() != this && facep->getTexture()->getID() == mID)
    {
        mTextureList.push_back(facep->getTexture()); //a parcel media.
        return;
    }

    if(te && te->getID().notNull()) //should have a texture
    {
        LL_WARNS_ONCE() << "The face's texture " << te->getID() << " is not valid. Face must have a valid texture before media texture." << LL_ENDL;
        // This might break the object, but it likely isn't a 'recoverable' situation.
        LLViewerFetchedTexture* tex = LLViewerTextureManager::getFetchedTexture(te->getID());
        mTextureList.push_back(tex);
    }
}

//virtual
//void LLViewerMediaTexture::removeFace(U32 ch, LLFace* facep)
// [SL:KB] - Patch: Render-TextureToggle (Catznip-5.2)
void LLViewerMediaTexture::removeFace(U32 channel, LLFace* facep)
// [/SL:KB]
{
// [SL:KB] - Patch: Render-TextureToggle (Catznip-5.2)
    LLViewerTexture::removeFace(channel, facep);
// [/SL:KB]
//  LLViewerTexture::removeFace(ch, facep);

    const LLTextureEntry* te = facep->getTextureEntry();
    if(te && te->getID().notNull())
    {
        LLViewerTexture* tex = gTextureList.findImage(te->getID(), TEX_LIST_STANDARD);
        if(tex)
        {
            for(std::list< LLPointer<LLViewerTexture> >::iterator iter = mTextureList.begin();
                iter != mTextureList.end(); ++iter)
            {
                if(*iter == tex)
                {
// [SL:KB] - Patch: Render-TextureToggle (Catznip-5.2)
                    // Switching to the default texture results in clearing the media textures on all prims;
                    // a side-effect is that we loose out on the reference to the original (non-media)
                    // texture potentially letting it dissapear from memory if this was the only reference to it
                    // (which is harmless, it just means we'll need to grab it from the cache or refetch it but
                    // the LL - debug - code at the bottom of addFace/removeFace disagrees so we'll hang on
                    // to it (and then block readding it a seond time higher up)
                    if (facep->isDefaultTexture(channel))
                    {
                        return;
                    }
// [/SL:KB]
                    mTextureList.erase(iter); //decrease the reference number for tex by one.
                    return;
                }
            }

            std::vector<const LLTextureEntry*> te_list;

            for (U32 ch = 0; ch < 3; ++ch)
            {
            //
            //we have some trouble here: the texture of the face is changed.
            //we need to find the former texture, and remove it from the list to avoid memory leaking.

                llassert(mNumFaces[ch] <= mFaceList[ch].size());

                for(U32 j = 0; j < mNumFaces[ch]; j++)
                {
                    te_list.push_back(mFaceList[ch][j]->getTextureEntry());//all textures are in use.
                }
            }

            if (te_list.empty())
            {
                mTextureList.clear();
                return;
            }

            auto end = te_list.size();

            for(std::list< LLPointer<LLViewerTexture> >::iterator iter = mTextureList.begin();
                iter != mTextureList.end(); ++iter)
            {
                size_t i = 0;

                for(i = 0; i < end; i++)
                {
                    if(te_list[i] && te_list[i]->getID() == (*iter)->getID())//the texture is in use.
                    {
                        te_list[i] = NULL;
                        break;
                    }
                }
                if(i == end) //no hit for this texture, remove it.
                {
// [SL:KB] - Patch: Render-TextureToggle (Catznip-5.2)
                    // See above
                    if (facep->isDefaultTexture(channel))
                    {
                        return;
                    }
// [/SL:KB]
                    mTextureList.erase(iter); //decrease the reference number for tex by one.
                    return;
                }
            }
        }
    }

    //check if it is a parcel media
    for(std::list< LLPointer<LLViewerTexture> >::iterator iter = mTextureList.begin();
                iter != mTextureList.end(); ++iter)
    {
        if((*iter)->getID() == mID)
        {
            mTextureList.erase(iter); //decrease the reference number for tex by one.
            return;
        }
    }

    if(te && te->getID().notNull()) //should have a texture but none found
    {
        LL_ERRS() << "mTextureList texture reference number is corrupted. Texture id: " << te->getID() << " List size: " << (U32)mTextureList.size() << LL_ENDL;
    }
}

void LLViewerMediaTexture::stopPlaying()
{
    // Don't stop the media impl playing here -- this breaks non-inworld media (login screen, search, and media browser).
//  if(mMediaImplp)
//  {
//      mMediaImplp->stop();
//  }
    mIsPlaying = false;
}

void LLViewerMediaTexture::switchTexture(U32 ch, LLFace* facep)
{
    if(facep)
    {
        //check if another media is playing on this face.
        if(facep->getTexture() && facep->getTexture() != this
            && facep->getTexture()->getType() == LLViewerTexture::MEDIA_TEXTURE)
        {
            if(mID == facep->getTexture()->getID()) //this is a parcel media
            {
                return; //let the prim media win.
            }
        }

        if(mIsPlaying) //old textures switch to the media texture
        {
            facep->switchTexture(ch, this);
        }
        else //switch to old textures.
        {
            const LLTextureEntry* te = facep->getTextureEntry();
            if(te)
            {
                LLViewerTexture* tex = te->getID().notNull() ? gTextureList.findImage(te->getID(), TEX_LIST_STANDARD) : NULL;
                if(!tex && te->getID() != mID)//try parcel media.
                {
                    tex = gTextureList.findImage(mID, TEX_LIST_STANDARD);
                }
                if(!tex)
                {
                    tex = LLViewerFetchedTexture::sDefaultImagep;
                }
                facep->switchTexture(ch, tex);
            }
        }
    }
}

void LLViewerMediaTexture::setPlaying(bool playing)
{
    if(!mMediaImplp)
    {
        return;
    }
    if(!playing && !mIsPlaying)
    {
        return; //media is already off
    }

    if(playing == mIsPlaying && !mMediaImplp->isUpdated())
    {
        return; //nothing has changed since last time.
    }

    mIsPlaying = playing;
    if(mIsPlaying) //is about to play this media
    {
        if(findFaces())
        {
            //about to update all faces.
            mMediaImplp->setUpdated(false);
        }

        if(mMediaFaceList.empty())//no face pointing to this media
        {
            stopPlaying();
            return;
        }

        for(std::list< LLFace* >::iterator iter = mMediaFaceList.begin(); iter!= mMediaFaceList.end(); ++iter)
        {
            LLFace* facep = *iter;
            switchTexture(LLRender::DIFFUSE_MAP, facep);
        }
    }
    else //stop playing this media
    {
        U32 ch = LLRender::DIFFUSE_MAP;

        llassert(mNumFaces[ch] <= mFaceList[ch].size());
        for(U32 i = mNumFaces[ch]; i; i--)
        {
            switchTexture(ch, mFaceList[ch][i - 1]); //current face could be removed in this function.
        }
    }
    return;
}

// VK: LLDrawable::isRecentlyVisible() is stamped by LL's spatial cull, which never
// runs under Vulkan (only moving drawables get stamped), so a still media prim read
// as not visible, its interest dropped to 0 and LLViewerMedia ranked it HIDDEN (the
// plugin then renders ~1 fps; focus forces HIGH, which hid this). Use the camera
// frustum test that LLVOVolume::updateTextureVirtualSize uses instead.
static bool vkMediaFaceVisible(LLFace* facep)
{
    LLDrawable* drawable = facep ? facep->getDrawable() : nullptr;
    if (!drawable || drawable->isDead())
        return false;
    LLViewerObject* vobj = drawable->getVObj().get();
    if (vobj && (vobj->isHUDAttachment() || vobj->isAttachment()))
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

//virtual
F32 LLViewerMediaTexture::getMaxVirtualSize()
{
    if(LLFrameTimer::getFrameCount() == mUpdateVirtualSizeTime)
    {
        return mMaxVirtualSize;
    }
    mUpdateVirtualSizeTime = LLFrameTimer::getFrameCount();

    if(!mMaxVirtualSizeResetCounter)
    {
        addTextureStats(0.f, false);//reset
    }

    if(mIsPlaying) //media is playing
    {
        for (U32 ch = 0; ch < LLRender::NUM_TEXTURE_CHANNELS; ++ch)
        {
            llassert(mNumFaces[ch] <= mFaceList[ch].size());
            for(U32 i = 0; i < mNumFaces[ch]; i++)
            {
                LLFace* facep = mFaceList[ch][i];
            if(vkMediaFaceVisible(facep))
            {
                addTextureStats(facep->getVirtualSize());
            }
        }
    }
    }
    else //media is not in playing
    {
        findFaces();

        if(!mMediaFaceList.empty())
        {
            for(std::list< LLFace* >::iterator iter = mMediaFaceList.begin(); iter!= mMediaFaceList.end(); ++iter)
            {
                LLFace* facep = *iter;
                if(vkMediaFaceVisible(facep))
                {
                    addTextureStats(facep->getVirtualSize());
                }
            }
        }
    }

    if(mMaxVirtualSizeResetCounter > 0)
    {
        mMaxVirtualSizeResetCounter--;
    }
    reorganizeFaceList();
    reorganizeVolumeList();

    return mMaxVirtualSize;
}
//----------------------------------------------------------------------------------------------
//end of LLViewerMediaTexture
//----------------------------------------------------------------------------------------------

//----------------------------------------------------------------------------------------------
//start of LLTexturePipelineTester
//----------------------------------------------------------------------------------------------
LLTexturePipelineTester::LLTexturePipelineTester() : LLMetricPerformanceTesterWithSession(sTesterName)
{
    addMetric("TotalBytesLoaded");
    addMetric("TotalBytesLoadedFromCache");
    addMetric("TotalBytesLoadedForLargeImage");
    addMetric("TotalBytesLoadedForSculpties");
    addMetric("StartFetchingTime");
    addMetric("TotalGrayTime");
    addMetric("TotalStablizingTime");
    addMetric("StartTimeLoadingSculpties");
    addMetric("EndTimeLoadingSculpties");

    addMetric("Time");
    addMetric("TotalBytesBound");
    addMetric("TotalBytesBoundForLargeImage");
    addMetric("PercentageBytesBound");

    mTotalBytesLoaded = (S32Bytes)0;
    mTotalBytesLoadedFromCache = (S32Bytes)0;
    mTotalBytesLoadedForLargeImage = (S32Bytes)0;
    mTotalBytesLoadedForSculpties = (S32Bytes)0;

    reset();
}

LLTexturePipelineTester::~LLTexturePipelineTester()
{
    LLViewerTextureManager::sTesterp = NULL;
}

void LLTexturePipelineTester::update()
{
    mLastTotalBytesUsed = mTotalBytesUsed;
    mLastTotalBytesUsedForLargeImage = mTotalBytesUsedForLargeImage;
    mTotalBytesUsed = (S32Bytes)0;
    mTotalBytesUsedForLargeImage = (S32Bytes)0;

    if(LLAppViewer::getTextureFetch()->getNumRequests() > 0) //fetching list is not empty
    {
        if(mPause)
        {
            //start a new fetching session
            reset();
            mStartFetchingTime = LLTextureData::sLastFrameTime;
            mPause = false;
        }

        //update total gray time
        if(mUsingDefaultTexture)
        {
            mUsingDefaultTexture = false;
            mTotalGrayTime = LLTextureData::sLastFrameTime - mStartFetchingTime;
        }

        //update the stablizing timer.
        updateStablizingTime();

        outputTestResults();
    }
    else if(!mPause)
    {
        //stop the current fetching session
        mPause = true;
        outputTestResults();
        reset();
    }
}

void LLTexturePipelineTester::reset()
{
    mPause = true;

    mUsingDefaultTexture = false;
    mStartStablizingTime = 0.0f;
    mEndStablizingTime = 0.0f;

    mTotalBytesUsed = (S32Bytes)0;
    mTotalBytesUsedForLargeImage = (S32Bytes)0;
    mLastTotalBytesUsed = (S32Bytes)0;
    mLastTotalBytesUsedForLargeImage = (S32Bytes)0;

    mStartFetchingTime = 0.0f;

    mTotalGrayTime = 0.0f;
    mTotalStablizingTime = 0.0f;

    mStartTimeLoadingSculpties = 1.0f;
    mEndTimeLoadingSculpties = 0.0f;
}

//virtual
void LLTexturePipelineTester::outputTestRecord(LLSD *sd)
{
    std::string currentLabel = getCurrentLabelName();
    (*sd)[currentLabel]["TotalBytesLoaded"]              = (LLSD::Integer)mTotalBytesLoaded.value();
    (*sd)[currentLabel]["TotalBytesLoadedFromCache"]     = (LLSD::Integer)mTotalBytesLoadedFromCache.value();
    (*sd)[currentLabel]["TotalBytesLoadedForLargeImage"] = (LLSD::Integer)mTotalBytesLoadedForLargeImage.value();
    (*sd)[currentLabel]["TotalBytesLoadedForSculpties"]  = (LLSD::Integer)mTotalBytesLoadedForSculpties.value();

    (*sd)[currentLabel]["StartFetchingTime"]             = (LLSD::Real)mStartFetchingTime;
    (*sd)[currentLabel]["TotalGrayTime"]                 = (LLSD::Real)mTotalGrayTime;
    (*sd)[currentLabel]["TotalStablizingTime"]           = (LLSD::Real)mTotalStablizingTime;

    (*sd)[currentLabel]["StartTimeLoadingSculpties"]     = (LLSD::Real)mStartTimeLoadingSculpties;
    (*sd)[currentLabel]["EndTimeLoadingSculpties"]       = (LLSD::Real)mEndTimeLoadingSculpties;

    (*sd)[currentLabel]["Time"]                          = LLTextureData::sLastFrameTime;
    (*sd)[currentLabel]["TotalBytesBound"]               = (LLSD::Integer)mLastTotalBytesUsed.value();
    (*sd)[currentLabel]["TotalBytesBoundForLargeImage"]  = (LLSD::Integer)mLastTotalBytesUsedForLargeImage.value();
    (*sd)[currentLabel]["PercentageBytesBound"]          = (LLSD::Real)(100.f * mLastTotalBytesUsed / mTotalBytesLoaded);
}

void LLTexturePipelineTester::updateTextureBindingStats(const LLViewerTexture* imagep)
{
    // No per-texture GPU memory is tracked: nothing to add.
}

void LLTexturePipelineTester::updateTextureLoadingStats(const LLViewerFetchedTexture* imagep, const LLImageRaw* raw_imagep, bool from_cache)
{
    U32Bytes data_size = (U32Bytes)raw_imagep->getDataSize();
    mTotalBytesLoaded += data_size;

    if(from_cache)
    {
        mTotalBytesLoadedFromCache += data_size;
    }

    if(MIN_LARGE_IMAGE_AREA <= (U32)(data_size.value() / (U32)raw_imagep->getComponents()))
    {
        mTotalBytesLoadedForLargeImage += data_size;
    }

    if(imagep->forSculpt())
    {
        mTotalBytesLoadedForSculpties += data_size;

        if(mStartTimeLoadingSculpties > mEndTimeLoadingSculpties)
        {
            mStartTimeLoadingSculpties = LLTextureData::sLastFrameTime;
        }
        mEndTimeLoadingSculpties = LLTextureData::sLastFrameTime;
    }
}

void LLTexturePipelineTester::updateGrayTextureBinding()
{
    mUsingDefaultTexture = true;
}

void LLTexturePipelineTester::setStablizingTime()
{
    if(mStartStablizingTime <= mStartFetchingTime)
    {
        mStartStablizingTime = LLTextureData::sLastFrameTime;
    }
    mEndStablizingTime = LLTextureData::sLastFrameTime;
}

void LLTexturePipelineTester::updateStablizingTime()
{
    if(mStartStablizingTime > mStartFetchingTime)
    {
        F32 t = mEndStablizingTime - mStartStablizingTime;

        if(t > F_ALMOST_ZERO && (t - mTotalStablizingTime) < F_ALMOST_ZERO)
        {
            //already stablized
            mTotalStablizingTime = LLTextureData::sLastFrameTime - mStartStablizingTime;

            //cancel the timer
            mStartStablizingTime = 0.f;
            mEndStablizingTime = 0.f;
        }
        else
        {
            mTotalStablizingTime = t;
        }
    }
    mTotalStablizingTime = 0.f;
}

//virtual
void LLTexturePipelineTester::compareTestSessions(llofstream* os)
{
    LLTexturePipelineTester::LLTextureTestSession* base_sessionp = dynamic_cast<LLTexturePipelineTester::LLTextureTestSession*>(mBaseSessionp);
    LLTexturePipelineTester::LLTextureTestSession* current_sessionp = dynamic_cast<LLTexturePipelineTester::LLTextureTestSession*>(mCurrentSessionp);
    if(!base_sessionp || !current_sessionp)
    {
        LL_ERRS() << "type of test session does not match!" << LL_ENDL;
    }

    //compare and output the comparison
    *os << llformat("%s\n", getTesterName().c_str());
    *os << llformat("AggregateResults\n");

    compareTestResults(os, "TotalGrayTime", base_sessionp->mTotalGrayTime, current_sessionp->mTotalGrayTime);
    compareTestResults(os, "TotalStablizingTime", base_sessionp->mTotalStablizingTime, current_sessionp->mTotalStablizingTime);
    compareTestResults(os, "StartTimeLoadingSculpties", base_sessionp->mStartTimeLoadingSculpties, current_sessionp->mStartTimeLoadingSculpties);
    compareTestResults(os, "TotalTimeLoadingSculpties", base_sessionp->mTotalTimeLoadingSculpties, current_sessionp->mTotalTimeLoadingSculpties);

    compareTestResults(os, "TotalBytesLoaded", base_sessionp->mTotalBytesLoaded, current_sessionp->mTotalBytesLoaded);
    compareTestResults(os, "TotalBytesLoadedFromCache", base_sessionp->mTotalBytesLoadedFromCache, current_sessionp->mTotalBytesLoadedFromCache);
    compareTestResults(os, "TotalBytesLoadedForLargeImage", base_sessionp->mTotalBytesLoadedForLargeImage, current_sessionp->mTotalBytesLoadedForLargeImage);
    compareTestResults(os, "TotalBytesLoadedForSculpties", base_sessionp->mTotalBytesLoadedForSculpties, current_sessionp->mTotalBytesLoadedForSculpties);

    *os << llformat("InstantResults\n");
    S32 size = llmin(base_sessionp->mInstantPerformanceListCounter, current_sessionp->mInstantPerformanceListCounter);
    for(S32 i = 0; i < size; i++)
    {
        *os << llformat("Time(B-T)-%.4f-%.4f\n", base_sessionp->mInstantPerformanceList[i].mTime, current_sessionp->mInstantPerformanceList[i].mTime);

        compareTestResults(os, "AverageBytesUsedPerSecond", base_sessionp->mInstantPerformanceList[i].mAverageBytesUsedPerSecond,
            current_sessionp->mInstantPerformanceList[i].mAverageBytesUsedPerSecond);

        compareTestResults(os, "AverageBytesUsedForLargeImagePerSecond", base_sessionp->mInstantPerformanceList[i].mAverageBytesUsedForLargeImagePerSecond,
            current_sessionp->mInstantPerformanceList[i].mAverageBytesUsedForLargeImagePerSecond);

        compareTestResults(os, "AveragePercentageBytesUsedPerSecond", base_sessionp->mInstantPerformanceList[i].mAveragePercentageBytesUsedPerSecond,
            current_sessionp->mInstantPerformanceList[i].mAveragePercentageBytesUsedPerSecond);
    }

    if(size < base_sessionp->mInstantPerformanceListCounter)
    {
        for(S32 i = size; i < base_sessionp->mInstantPerformanceListCounter; i++)
        {
            *os << llformat("Time(B-T)-%.4f- \n", base_sessionp->mInstantPerformanceList[i].mTime);

            *os << llformat(", AverageBytesUsedPerSecond, %d, N/A \n", base_sessionp->mInstantPerformanceList[i].mAverageBytesUsedPerSecond);
            *os << llformat(", AverageBytesUsedForLargeImagePerSecond, %d, N/A \n", base_sessionp->mInstantPerformanceList[i].mAverageBytesUsedForLargeImagePerSecond);
            *os << llformat(", AveragePercentageBytesUsedPerSecond, %.4f, N/A \n", base_sessionp->mInstantPerformanceList[i].mAveragePercentageBytesUsedPerSecond);
        }
    }
    else if(size < current_sessionp->mInstantPerformanceListCounter)
    {
        for(S32 i = size; i < current_sessionp->mInstantPerformanceListCounter; i++)
        {
            *os << llformat("Time(B-T)- -%.4f\n", current_sessionp->mInstantPerformanceList[i].mTime);

            *os << llformat(", AverageBytesUsedPerSecond, N/A, %d\n", current_sessionp->mInstantPerformanceList[i].mAverageBytesUsedPerSecond);
            *os << llformat(", AverageBytesUsedForLargeImagePerSecond, N/A, %d\n", current_sessionp->mInstantPerformanceList[i].mAverageBytesUsedForLargeImagePerSecond);
            *os << llformat(", AveragePercentageBytesUsedPerSecond, N/A, %.4f\n", current_sessionp->mInstantPerformanceList[i].mAveragePercentageBytesUsedPerSecond);
        }
    }
}

//virtual
LLMetricPerformanceTesterWithSession::LLTestSession* LLTexturePipelineTester::loadTestSession(LLSD* log)
{
    LLTexturePipelineTester::LLTextureTestSession* sessionp = new LLTexturePipelineTester::LLTextureTestSession();
    if(!sessionp)
    {
        return NULL;
    }

    F32 total_gray_time = 0.f;
    F32 total_stablizing_time = 0.f;
    F32 total_loading_sculpties_time = 0.f;

    F32 start_fetching_time = -1.f;
    F32 start_fetching_sculpties_time = 0.f;

    F32 last_time = 0.0f;
    S32 frame_count = 0;

    sessionp->mInstantPerformanceListCounter = 0;
    sessionp->mInstantPerformanceList.resize(128);
    sessionp->mInstantPerformanceList[sessionp->mInstantPerformanceListCounter].mAverageBytesUsedPerSecond = 0;
    sessionp->mInstantPerformanceList[sessionp->mInstantPerformanceListCounter].mAverageBytesUsedForLargeImagePerSecond = 0;
    sessionp->mInstantPerformanceList[sessionp->mInstantPerformanceListCounter].mAveragePercentageBytesUsedPerSecond = 0.f;
    sessionp->mInstantPerformanceList[sessionp->mInstantPerformanceListCounter].mTime = 0.f;

    //load a session
    std::string currentLabel = getCurrentLabelName();
    bool in_log = (*log).has(currentLabel);
    while (in_log)
    {
        LLSD::String label = currentLabel;

        if(sessionp->mInstantPerformanceListCounter >= (S32)sessionp->mInstantPerformanceList.size())
        {
            sessionp->mInstantPerformanceList.resize(sessionp->mInstantPerformanceListCounter + 128);
        }

        //time
        F32 start_time = (F32)(*log)[label]["StartFetchingTime"].asReal();
        F32 cur_time   = (F32)(*log)[label]["Time"].asReal();
        if(start_time - start_fetching_time > F_ALMOST_ZERO) //fetching has paused for a while
        {
            sessionp->mTotalGrayTime += total_gray_time;
            sessionp->mTotalStablizingTime += total_stablizing_time;

            sessionp->mStartTimeLoadingSculpties = start_fetching_sculpties_time;
            sessionp->mTotalTimeLoadingSculpties += total_loading_sculpties_time;

            start_fetching_time = start_time;
            total_gray_time = 0.f;
            total_stablizing_time = 0.f;
            total_loading_sculpties_time = 0.f;
        }
        else
        {
            total_gray_time = (F32)(*log)[label]["TotalGrayTime"].asReal();
            total_stablizing_time = (F32)(*log)[label]["TotalStablizingTime"].asReal();

            total_loading_sculpties_time = (F32)(*log)[label]["EndTimeLoadingSculpties"].asReal() - (F32)(*log)[label]["StartTimeLoadingSculpties"].asReal();
            if(start_fetching_sculpties_time < 0.f && total_loading_sculpties_time > 0.f)
            {
                start_fetching_sculpties_time = (F32)(*log)[label]["StartTimeLoadingSculpties"].asReal();
            }
        }

        //total loaded bytes
        sessionp->mTotalBytesLoaded = (*log)[label]["TotalBytesLoaded"].asInteger();
        sessionp->mTotalBytesLoadedFromCache = (*log)[label]["TotalBytesLoadedFromCache"].asInteger();
        sessionp->mTotalBytesLoadedForLargeImage = (*log)[label]["TotalBytesLoadedForLargeImage"].asInteger();
        sessionp->mTotalBytesLoadedForSculpties = (*log)[label]["TotalBytesLoadedForSculpties"].asInteger();

        //instant metrics
        sessionp->mInstantPerformanceList[sessionp->mInstantPerformanceListCounter].mAverageBytesUsedPerSecond +=
            (*log)[label]["TotalBytesBound"].asInteger();
        sessionp->mInstantPerformanceList[sessionp->mInstantPerformanceListCounter].mAverageBytesUsedForLargeImagePerSecond +=
            (*log)[label]["TotalBytesBoundForLargeImage"].asInteger();
        sessionp->mInstantPerformanceList[sessionp->mInstantPerformanceListCounter].mAveragePercentageBytesUsedPerSecond +=
            (F32)(*log)[label]["PercentageBytesBound"].asReal();
        frame_count++;
        if(cur_time - last_time >= 1.0f)
        {
            sessionp->mInstantPerformanceList[sessionp->mInstantPerformanceListCounter].mAverageBytesUsedPerSecond /= frame_count;
            sessionp->mInstantPerformanceList[sessionp->mInstantPerformanceListCounter].mAverageBytesUsedForLargeImagePerSecond /= frame_count;
            sessionp->mInstantPerformanceList[sessionp->mInstantPerformanceListCounter].mAveragePercentageBytesUsedPerSecond /= frame_count;
            sessionp->mInstantPerformanceList[sessionp->mInstantPerformanceListCounter].mTime = last_time;

            frame_count = 0;
            last_time = cur_time;
            sessionp->mInstantPerformanceListCounter++;
            sessionp->mInstantPerformanceList[sessionp->mInstantPerformanceListCounter].mAverageBytesUsedPerSecond = 0;
            sessionp->mInstantPerformanceList[sessionp->mInstantPerformanceListCounter].mAverageBytesUsedForLargeImagePerSecond = 0;
            sessionp->mInstantPerformanceList[sessionp->mInstantPerformanceListCounter].mAveragePercentageBytesUsedPerSecond = 0.f;
            sessionp->mInstantPerformanceList[sessionp->mInstantPerformanceListCounter].mTime = 0.f;
        }
        // Next label
        incrementCurrentCount();
        currentLabel = getCurrentLabelName();
        in_log = (*log).has(currentLabel);
    }

    sessionp->mTotalGrayTime += total_gray_time;
    sessionp->mTotalStablizingTime += total_stablizing_time;

    if(sessionp->mStartTimeLoadingSculpties < 0.f)
    {
        sessionp->mStartTimeLoadingSculpties = start_fetching_sculpties_time;
    }
    sessionp->mTotalTimeLoadingSculpties += total_loading_sculpties_time;

    return sessionp;
}

LLTexturePipelineTester::LLTextureTestSession::LLTextureTestSession()
{
    reset();
}
LLTexturePipelineTester::LLTextureTestSession::~LLTextureTestSession()
{
}
void LLTexturePipelineTester::LLTextureTestSession::reset()
{
    mTotalGrayTime = 0.0f;
    mTotalStablizingTime = 0.0f;

    mStartTimeLoadingSculpties = 0.0f;
    mTotalTimeLoadingSculpties = 0.0f;

    mTotalBytesLoaded = 0;
    mTotalBytesLoadedFromCache = 0;
    mTotalBytesLoadedForLargeImage = 0;
    mTotalBytesLoadedForSculpties = 0;

    mInstantPerformanceListCounter = 0;
}
//----------------------------------------------------------------------------------------------
//end of LLTexturePipelineTester
//----------------------------------------------------------------------------------------------

