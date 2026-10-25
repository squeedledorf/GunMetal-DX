/**
 * @file llgltfmaterialpreviewmgr.cpp
 *
 * $LicenseInfo:firstyear=2023&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2023, Linden Research, Inc.
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

#include "llgltfmaterialpreviewmgr.h"

#include <memory>
#include <vector>

#include "llavatarappearancedefines.h"
#include "llenvironment.h"
#include "llselectmgr.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewertexturelist.h"
#include "llviewerwindow.h"
#include "llvolumemgr.h"
#include "pipeline.h"
#include "llvkpreviewrender.h"   // #83: Vulkan 3D preview drawing
#include "llprimitive.h"
#include "llrenderbackend.h"

LLGLTFMaterialPreviewMgr gGLTFMaterialPreviewMgr;

namespace
{
    constexpr S32 FULLY_LOADED = 0;
    constexpr S32 NOT_LOADED = 99;
};

LLGLTFPreviewTexture::MaterialLoadLevels::MaterialLoadLevels()
{
    for (U32 i = 0; i < LLGLTFMaterial::GLTF_TEXTURE_INFO_COUNT; ++i)
    {
        levels[i] = NOT_LOADED;
    }
}

bool LLGLTFPreviewTexture::MaterialLoadLevels::isFullyLoaded()
{
    for (U32 i = 0; i < LLGLTFMaterial::GLTF_TEXTURE_INFO_COUNT; ++i)
    {
        if (levels[i] != FULLY_LOADED) { return false; }
    }
    return true;
}

S32& LLGLTFPreviewTexture::MaterialLoadLevels::operator[](size_t i)
{
    llassert(i >= 0 && i < LLGLTFMaterial::GLTF_TEXTURE_INFO_COUNT);
    return levels[i];
}

const S32& LLGLTFPreviewTexture::MaterialLoadLevels::operator[](size_t i) const
{
    llassert(i >= 0 && i < LLGLTFMaterial::GLTF_TEXTURE_INFO_COUNT);
    return levels[i];
}

bool LLGLTFPreviewTexture::MaterialLoadLevels::operator<(const MaterialLoadLevels& other) const
{
    bool less = false;
    for (U32 i = 0; i < LLGLTFMaterial::GLTF_TEXTURE_INFO_COUNT; ++i)
    {
        if (((*this)[i] > other[i])) { return false; }
        less = less || ((*this)[i] < other[i]);
    }
    return less;
}

bool LLGLTFPreviewTexture::MaterialLoadLevels::operator>(const MaterialLoadLevels& other) const
{
    bool great = false;
    for (U32 i = 0; i < LLGLTFMaterial::GLTF_TEXTURE_INFO_COUNT; ++i)
    {
        if (((*this)[i] < other[i])) { return false; }
        great = great || ((*this)[i] > other[i]);
    }
    return great;
}

namespace
{
    void fetch_texture_for_ui(LLPointer<LLViewerFetchedTexture>& img, const LLUUID& id)
    {
        if (!img && id.notNull())
        {
            if (LLAvatarAppearanceDefines::LLAvatarAppearanceDictionary::isBakedImageId(id))
            {
                LLViewerObject* obj = LLSelectMgr::getInstance()->getSelection()->getFirstObject();
                if (obj)
                {
                    LLViewerTexture* viewerTexture = obj->getBakedTextureForMagicId(id);
                    img = viewerTexture ? dynamic_cast<LLViewerFetchedTexture*>(viewerTexture) : NULL;
                }
            }
            else
            {
                img = LLViewerTextureManager::getFetchedTexture(id, FTT_DEFAULT, true, LLGLTexture::BOOST_NONE, LLViewerTexture::LOD_TEXTURE);
            }
        }
        if (img)
        {
            img->setBoostLevel(LLGLTexture::BOOST_PREVIEW);
            img->forceToSaveRawImage(0);
        }
    };

    // *NOTE: Does not use the same conventions as texture discard level. Lower is better.
    S32 get_texture_load_level(const LLPointer<LLViewerFetchedTexture>& texture)
    {
        if (!texture) { return FULLY_LOADED; }
        const S32 raw_level = texture->getDiscardLevel();
        if (raw_level < 0) { return NOT_LOADED; }
        return raw_level;
    }

    LLGLTFPreviewTexture::MaterialLoadLevels get_material_load_levels(LLFetchedGLTFMaterial& material)
    {
        llassert(!material.isFetching());

        using MaterialTextures = LLPointer<LLViewerFetchedTexture>*[LLGLTFMaterial::GLTF_TEXTURE_INFO_COUNT];

        MaterialTextures textures;

        textures[LLGLTFMaterial::GLTF_TEXTURE_INFO_BASE_COLOR] = &material.mBaseColorTexture;
        textures[LLGLTFMaterial::GLTF_TEXTURE_INFO_NORMAL] = &material.mNormalTexture;
        textures[LLGLTFMaterial::GLTF_TEXTURE_INFO_METALLIC_ROUGHNESS] = &material.mMetallicRoughnessTexture;
        textures[LLGLTFMaterial::GLTF_TEXTURE_INFO_EMISSIVE] = &material.mEmissiveTexture;

        LLGLTFPreviewTexture::MaterialLoadLevels levels;

        for (U32 i = 0; i < LLGLTFMaterial::GLTF_TEXTURE_INFO_COUNT; ++i)
        {
            fetch_texture_for_ui(*textures[i], material.mTextureId[i]);
            levels[i] = get_texture_load_level(*textures[i]);
        }

        return levels;
    }

    // Is the material loaded enough to start rendering a preview?
    bool is_material_loaded_enough_for_ui(LLFetchedGLTFMaterial& material)
    {
        if (material.isFetching())
        {
            return false;
        }

        LLGLTFPreviewTexture::MaterialLoadLevels levels = get_material_load_levels(material);

        for (U32 i = 0; i < LLGLTFMaterial::GLTF_TEXTURE_INFO_COUNT; ++i)
        {
            if (levels[i] == NOT_LOADED)
            {
                return false;
            }
        }

        return true;
    }

};  // namespace

LLGLTFPreviewTexture::LLGLTFPreviewTexture(LLPointer<LLFetchedGLTFMaterial> material, S32 width)
    : LLViewerDynamicTexture(width, width, 4, EOrder::ORDER_MIDDLE, false)
    , mGLTFMaterial(material)
{
}

// static
LLPointer<LLGLTFPreviewTexture> LLGLTFPreviewTexture::create(LLPointer<LLFetchedGLTFMaterial> material)
{
    return new LLGLTFPreviewTexture(material, LLPipeline::MAX_PREVIEW_WIDTH);
}

bool LLGLTFPreviewTexture::needsRender()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_UI;

    if (!mShouldRender && mBestLoad.isFullyLoaded()) { return false; }
    MaterialLoadLevels current_load = get_material_load_levels(*mGLTFMaterial.get());
    if (current_load < mBestLoad)
    {
        mShouldRender = true;
        mBestLoad = current_load;
        return true;
    }
    return false;
}

void LLGLTFPreviewTexture::preRender(bool clear_depth)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_UI;

    llassert(mShouldRender);
    if (!mShouldRender) { return; }

    LLViewerDynamicTexture::preRender(clear_depth);
}


// #83: the swatch, into this texture's render target
// (LLViewerDynamicTexture::renderVK). LL rendered the sphere through the deferred
// PBR alpha pass with a fixed sun; the facade has no PBR shading, so this is the
// same sphere, camera and UV framing with base colour texture x base colour factor,
// lit by that sun (Gouraud) over a transparent background. Emissive, normal and
// ORM maps are not shown.
static bool render_gltf_preview_vk(LLFetchedGLTFMaterial* material, S32 width, S32 height)
{
    LLPrimitive prim;
    prim.init_primitive(LL_PCODE_VOLUME);
    LLVolumeParams params;
    params.setType(LL_PCODE_PROFILE_CIRCLE_HALF, LL_PCODE_PATH_CIRCLE);
    params.setBeginAndEndS(0.f, 1.f);
    params.setBeginAndEndT(0.f, 1.f);
    params.setRatio(1, 1);
    params.setShear(0, 0);
    prim.setVolume(params, LLVolumeLODGroup::NUM_LODS - 1);
    LLVolume* volume = prim.getVolume();
    if (!volume) return false;

    LLViewerCamera camera;
    constexpr F32 fill_ratio = 0.8f;
    constexpr F32 object_radius = 0.5f;
    const F32 object_distance = (object_radius / fill_ratio) * tan(camera.getDefaultFOV());
    const LLVector3 object_position(0.0, -object_distance, 0.0);
    LLMatrix4 object_transform;
    object_transform.translate(object_position);
    LLMatrix4a model;
    model.loadu(object_transform);

    const LLVector3 origin(0.0, 0.0, 0.0);
    LLVKPreviewRender preview(width, height, origin, object_position, LLVector3::z_axis,
                              camera.getDefaultFOV(), camera.getNear(), 64.f);
    preview.setSingleLight(LLVector3(1.f, 1.f, 1.f), LLColor4::white, 0.35f);

    const bool mask = material->mAlphaMode == LLGLTFMaterial::ALPHA_MODE_MASK;
    const bool blend = material->mAlphaMode == LLGLTFMaterial::ALPHA_MODE_BLEND;
    gGL.setSceneBlendType(blend ? LLRender::BT_ALPHA : LLRender::BT_REPLACE);
    if (mask) gGL.setMinimumAlpha(material->mAlphaCutoff);

    LLColor4 base = material->mBaseColor;
    if (!blend && !mask) base.mV[VALPHA] = 1.f;
    LLGLTexture* tex = material->mBaseColorTexture.get();

    // UV hacks of create_preview_sphere: more detail, material centred in view.
    const LLVector2 uv_factor(2.0f, 2.0f);
    const LLVector2 uv_offset(-0.5f, -0.5f);
    std::vector<LLVector2> uv;
    for (const LLVolumeFace& face : volume->getVolumeFaces())
    {
        if (!face.mNumVertices || !face.mNumIndices) continue;
        uv.resize(face.mNumVertices);
        for (S32 v = 0; v < face.mNumVertices; ++v)
        {
            LLVector2 t(face.mTexCoords[v]);
            t.scaleVec(uv_factor);
            uv[v] = t + uv_offset;
        }
        LLVKPreviewRender::Mesh m;
        m.pos = face.mPositions; m.norm = face.mNormals; m.uv = uv.data();
        m.vert_count = (U32)face.mNumVertices;
        m.idx16 = face.mIndices; m.index_count = (U32)face.mNumIndices;
        m.model = &model;
        preview.drawTriangles(m, tex, base, !material->mDoubleSided);
    }
    gGL.setMinimumAlpha(0.f);
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    return true;
}

bool LLGLTFPreviewTexture::render()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_UI;

    if (!mShouldRender) { return false; }

    return render_gltf_preview_vk(mGLTFMaterial.get(), mFullWidth, mFullHeight);
}

void LLGLTFPreviewTexture::postRender(bool success)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_UI;

    if (!mShouldRender) { return; }
    mShouldRender = false;

    LLViewerDynamicTexture::postRender(success);
}

LLPointer<LLViewerTexture> LLGLTFMaterialPreviewMgr::getPreview(LLPointer<LLFetchedGLTFMaterial> &material)
{
    if (!material)
    {
        return nullptr;
    }

    static LLCachedControl<bool> sUIPreviewMaterial(gSavedSettings, "UIPreviewMaterial", false);
    if (!sUIPreviewMaterial)
    {
        fetch_texture_for_ui(material->mBaseColorTexture, material->mTextureId[LLGLTFMaterial::GLTF_TEXTURE_INFO_BASE_COLOR]);
        return material->mBaseColorTexture;
    }

    if (!is_material_loaded_enough_for_ui(*material))
    {
        return nullptr;
    }

    return LLGLTFPreviewTexture::create(material);
}
