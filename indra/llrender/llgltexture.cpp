/**
 * @file llgltexture.cpp
 * @brief Opengl texture implementation
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
#include "linden_common.h"
#include "llgltexture.h"


LLGLTexture::LLGLTexture(bool usemipmaps)
{
    init();
    mUseMipMaps = usemipmaps;
}

LLGLTexture::LLGLTexture(const U32 width, const U32 height, const U8 components, bool usemipmaps)
{
    init();
    mFullWidth = width ;
    mFullHeight = height ;
    mUseMipMaps = usemipmaps;
    mComponents = components ;
    setTexelsPerImage();
}

LLGLTexture::LLGLTexture(const LLImageRaw* raw, bool usemipmaps)
{
    init();
    mUseMipMaps = usemipmaps ;
    // Create an empty image of the specified size and width
    mTextureData = new LLTextureData(raw, usemipmaps) ;
    mFullWidth = mTextureData->getWidth();
    mFullHeight = mTextureData->getHeight();
    mComponents = mTextureData->getComponents();
    setTexelsPerImage();
}

LLGLTexture::~LLGLTexture()
{
    cleanup();
}

void LLGLTexture::init()
{
    mBoostLevel = LLGLTexture::BOOST_NONE;

    mFullWidth = 0;
    mFullHeight = 0;
    mTexelsPerImage = 0 ;
    mUseMipMaps = false ;
    mComponents = 0 ;

    mTextureState = NO_DELETE ;
    mDontDiscard = false;
    mNeedsGLTexture = false ;
}

void LLGLTexture::cleanup()
{
    if(mTextureData)
    {
        mTextureData->cleanup();
    }
}

// virtual
void LLGLTexture::dump()
{
    if(mTextureData)
    {
        mTextureData->dump();
    }
}

void LLGLTexture::setBoostLevel(S32 level)
{
    if(mBoostLevel != level)
    {
        mBoostLevel = level ;
        if(mBoostLevel != LLGLTexture::BOOST_NONE
           && mBoostLevel != LLGLTexture::BOOST_ICON
           && mBoostLevel != LLGLTexture::BOOST_THUMBNAIL
           && mBoostLevel != LLGLTexture::BOOST_TERRAIN)
        {
            setNoDelete() ;
        }
    }
}

// virtual
U8 LLGLTexture::getVKUiKind() const
{
    switch (mBoostLevel)
    {
        case BOOST_UI:              return LLTextureData::VKUI_SPRITE;
        case BOOST_ICON:            return LLTextureData::VKUI_ICON;
        case BOOST_THUMBNAIL:
        case BOOST_PREVIEW:         return LLTextureData::VKUI_THUMB;
        case BOOST_MAP:
        case BOOST_MAP_VISIBLE:
        case BOOST_TERRAIN:         return LLTextureData::VKUI_MAP;
        case BOOST_AVATAR:
        case BOOST_AVATAR_BAKED:
        case BOOST_AVATAR_BAKED_SELF:
        case BOOST_AVATAR_SELF:     return LLTextureData::VKUI_AVATAR;
        default:                    return LLTextureData::VKUI_WORLD;
    }
}

void LLGLTexture::forceActive()
{
    mTextureState = ACTIVE ;
}

void LLGLTexture::setActive()
{
    if(mTextureState != NO_DELETE)
    {
        mTextureState = ACTIVE ;
    }
}

//set the texture to stay in memory
void LLGLTexture::setNoDelete()
{
    mTextureState = NO_DELETE ;
}

void LLGLTexture::generateTextureData()
{
    if(mTextureData.isNull())
    {
        mTextureData = new LLTextureData(mFullWidth, mFullHeight, mComponents, mUseMipMaps) ;
    }
}

LLTextureData* LLGLTexture::getTextureData() const
{
    llassert(mTextureData.notNull()) ;

    return mTextureData ;
}

bool LLGLTexture::setFromRaw(S32 discard_level, const LLImageRaw* imageraw, bool defer_copy)
{
    llassert(mTextureData.notNull());

    bool ret = mTextureData->setFromRaw(discard_level, imageraw, defer_copy) ;
    // #707: pixels injected from outside the fetch pipeline (local bitmaps, generated
    // images): a re-fetch would bring back other pixels -- never drop this copy.
    if (ret && !(mTextureData->getVKLeaseMask() & (1u << LLTextureData::VKLEASE_LOCAL)))
        mTextureData->acquireVKLease(LLTextureData::VKLEASE_LOCAL);

    if(ret)
    {
        mFullWidth = mTextureData->getCurrentWidth() ;
        mFullHeight = mTextureData->getCurrentHeight() ;
        mComponents = mTextureData->getComponents() ;
        setTexelsPerImage();
    }

    return ret ;
}

void LLGLTexture::setExplicitFormat(LLPixelFormat internal_format, LLPixelFormat primary_format, LLPixelType type_format, bool swap_bytes)
{
    llassert(mTextureData.notNull()) ;

    mTextureData->setExplicitFormat(internal_format, primary_format, type_format, swap_bytes) ;
}
void LLGLTexture::setAddressMode(LLTexUnit::eTextureAddressMode mode)
{
    llassert(mTextureData.notNull()) ;
    mTextureData->setAddressMode(mode) ;
}
//virtual
S32 LLGLTexture::getWidth(S32 discard_level) const
{
    llassert(mTextureData.notNull()) ;
    return mTextureData->getWidth(discard_level) ;
}

//virtual
S32 LLGLTexture::getHeight(S32 discard_level) const
{
    llassert(mTextureData.notNull()) ;
    return mTextureData->getHeight(discard_level) ;
}

S32 LLGLTexture::getMaxDiscardLevel() const
{
    llassert(mTextureData.notNull()) ;
    return mTextureData->getMaxDiscardLevel() ;
}
S32 LLGLTexture::getDiscardLevel() const
{
    llassert(mTextureData.notNull()) ;
    return mTextureData->getDiscardLevel() ;
}
S8  LLGLTexture::getComponents() const
{
    llassert(mTextureData.notNull()) ;

    return mTextureData->getComponents() ;
}

bool LLGLTexture::hasTextureData() const
{
    if (mTextureData.isNull())
    {
        return false;
    }
    // (#707: a dropped copy still has pixels -- on the GPU, re-sourced when read)
    return mTextureData->getVKCpuData() != nullptr || mTextureData->isVKCpuDropped();
}

bool LLGLTexture::getBoundRecently() const
{
    if(mTextureData.notNull())
    {
        return mTextureData->getBoundRecently() ;
    }
    return false ;
}

LLTexUnit::eTextureType LLGLTexture::getTarget(void) const
{
    llassert(mTextureData.notNull()) ;
    return mTextureData->getTarget() ;
}

bool LLGLTexture::setSubImage(const LLImageRaw* imageraw, S32 x_pos, S32 y_pos, S32 width, S32 height)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    llassert(mTextureData.notNull()) ;

    return mTextureData->setSubImage(imageraw, x_pos, y_pos, width, height) ;
}

bool LLGLTexture::setSubImage(const U8* datap, S32 data_width, S32 data_height, S32 x_pos, S32 y_pos, S32 width, S32 height)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    llassert(mTextureData.notNull()) ;

    return mTextureData->setSubImage(datap, data_width, data_height, x_pos, y_pos, width, height) ;
}

void LLGLTexture::setContentReady (bool initialized)
{
    llassert(mTextureData.notNull()) ;

    mTextureData->setContentReady (initialized) ;
}

void  LLGLTexture::setCategory(S32 category)
{
    llassert(mTextureData.notNull()) ;

    mTextureData->setCategory(category) ;
}

LLTexUnit::eTextureAddressMode LLGLTexture::getAddressMode(void) const
{
    llassert(mTextureData.notNull()) ;

    return mTextureData->getAddressMode() ;
}

LLPixelFormat LLGLTexture::getPrimaryFormat() const
{
    llassert(mTextureData.notNull()) ;

    return mTextureData->getPrimaryFormat() ;
}

bool LLGLTexture::getIsAlphaMask() const
{
    llassert(mTextureData.notNull()) ;

    return mTextureData->getIsAlphaMask() ;
}

F32 LLGLTexture::getTimePassedSinceLastBound()
{
    llassert(mTextureData.notNull()) ;

    return mTextureData->getTimePassedSinceLastBound() ;
}
bool LLGLTexture::isJustBound() const
{
    llassert(mTextureData.notNull()) ;

    return mTextureData->isJustBound() ;
}

void LLGLTexture::forceUpdateBindStats(void) const
{
    llassert(mTextureData.notNull()) ;

    return mTextureData->forceUpdateBindStats() ;
}

bool LLGLTexture::isContentReady() const
{
    llassert(mTextureData.notNull()) ;

    return mTextureData->isContentReady() ;
}

void LLGLTexture::setTexelsPerImage()
{
    U32 fullwidth = llmin(mFullWidth,U32(MAX_IMAGE_SIZE_DEFAULT));
    U32 fullheight = llmin(mFullHeight,U32(MAX_IMAGE_SIZE_DEFAULT));
    mTexelsPerImage = (U32)fullwidth * fullheight;
}

static LLUUID sStubUUID;

const LLUUID& LLGLTexture::getID() const { return sStubUUID; }
