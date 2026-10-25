/**
 * @file   llfetchedgltfmaterial.cpp
 *
 * $LicenseInfo:firstyear=2022&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2022, Linden Research, Inc.
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

#include "llfetchedgltfmaterial.h"

#include "llviewertexturelist.h"
#include "llavatarappearancedefines.h"
#include "llviewerobject.h"
#include "llselectmgr.h"
#include "pipeline.h"

//static
LLFetchedGLTFMaterial LLFetchedGLTFMaterial::sDefault;

LLFetchedGLTFMaterial::LLFetchedGLTFMaterial()
    : LLGLTFMaterial()
    , mExpectedFlusTime(0.f)
{

}

LLFetchedGLTFMaterial::~LLFetchedGLTFMaterial()
{

}

LLFetchedGLTFMaterial& LLFetchedGLTFMaterial::operator=(const LLFetchedGLTFMaterial& rhs)
{
    LLGLTFMaterial::operator =(rhs);

    mBaseColorTexture = rhs.mBaseColorTexture;
    mNormalTexture = rhs.mNormalTexture;
    mMetallicRoughnessTexture = rhs.mMetallicRoughnessTexture;
    mEmissiveTexture = rhs.mEmissiveTexture;

    return *this;
}

LLViewerFetchedTexture* fetch_texture(const LLUUID& id)
{
    LLViewerFetchedTexture* img = nullptr;
    if (id.notNull())
    {
        img = LLViewerTextureManager::getFetchedTexture(id, FTT_DEFAULT, true, LLGLTexture::BOOST_NONE, LLViewerTexture::LOD_TEXTURE);
        img->addTextureStats(64.f * 64.f, true);
    }
    return img;
};

bool LLFetchedGLTFMaterial::replaceLocalTexture(const LLUUID& tracking_id, const LLUUID& old_id, const LLUUID& new_id)
{
    bool res = false;
    if (mTextureId[LLGLTFMaterial::GLTF_TEXTURE_INFO_BASE_COLOR] == old_id)
    {
        mTextureId[LLGLTFMaterial::GLTF_TEXTURE_INFO_BASE_COLOR] = new_id;
        mBaseColorTexture = fetch_texture(new_id);
        res = true;
    }
    if (mTextureId[LLGLTFMaterial::GLTF_TEXTURE_INFO_NORMAL] == old_id)
    {
        mTextureId[LLGLTFMaterial::GLTF_TEXTURE_INFO_NORMAL] = new_id;
        mNormalTexture = fetch_texture(new_id);
        res = true;
    }
    if (mTextureId[LLGLTFMaterial::GLTF_TEXTURE_INFO_METALLIC_ROUGHNESS] == old_id)
    {
        mTextureId[LLGLTFMaterial::GLTF_TEXTURE_INFO_METALLIC_ROUGHNESS] = new_id;
        mMetallicRoughnessTexture = fetch_texture(new_id);
        res = true;
    }
    if (mTextureId[LLGLTFMaterial::GLTF_TEXTURE_INFO_EMISSIVE] == old_id)
    {
        mTextureId[LLGLTFMaterial::GLTF_TEXTURE_INFO_EMISSIVE] = new_id;
        mEmissiveTexture = fetch_texture(new_id);
        res = true;
    }

    for (int i = 0; i < GLTF_TEXTURE_INFO_COUNT; ++i)
    {
        if (mTextureId[i] == new_id)
        {
            res = true;
        }
    }

    if (res)
    {
        mTrackingIdToLocalTexture[tracking_id] = new_id;
    }
    else
    {
        mTrackingIdToLocalTexture.erase(tracking_id);
    }
    updateLocalTexDataDigest();

    return res;
}

void LLFetchedGLTFMaterial::addTextureEntry(LLTextureEntry* te)
{
    mTextureEntires.insert(te);
}

void LLFetchedGLTFMaterial::removeTextureEntry(LLTextureEntry* te)
{
    mTextureEntires.erase(te);
}

void LLFetchedGLTFMaterial::updateTextureTracking()
{
    for (local_tex_map_t::value_type &val : mTrackingIdToLocalTexture)
    {
        LLLocalBitmapMgr::getInstance()->associateGLTFMaterial(val.first, this);
    }
}

void LLFetchedGLTFMaterial::materialBegin()
{
    llassert(!mFetching);
    mFetching = true;
}

void LLFetchedGLTFMaterial::onMaterialComplete(std::function<void()> material_complete)
{
    if (!material_complete) { return; }

    if (!mFetching)
    {
        material_complete();
        return;
    }

    materialCompleteCallbacks.push_back(material_complete);
}

void LLFetchedGLTFMaterial::materialComplete(bool success)
{
    llassert(mFetching);
    mFetching = false;
    mFetchSuccess = success;

    for (std::function<void()> material_complete : materialCompleteCallbacks)
    {
        material_complete();
    }
    materialCompleteCallbacks.clear();
    materialCompleteCallbacks.shrink_to_fit();
}
