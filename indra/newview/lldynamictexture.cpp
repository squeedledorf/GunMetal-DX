/**
 * @file lldynamictexture.cpp
 * @brief Implementation of LLViewerDynamicTexture class
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

#include "lldynamictexture.h"

// Linden library includes
#include "llwindow.h"           // getPosition()

// Viewer includes
#include "llviewerwindow.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewertexture.h"
#include "llviewerdisplay.h"
#include "llrender.h"
#include "llrenderbackend.h"
#include "vkgfx/llrendervkbackend.h"   // #83: GPU render targets under Vulkan

// static
LLViewerDynamicTexture::instance_list_t LLViewerDynamicTexture::sInstances[ LLViewerDynamicTexture::ORDER_COUNT ];
S32 LLViewerDynamicTexture::sNumRenders = 0;
bool LLViewerDynamicTexture::sVKPassActive = false;

//-----------------------------------------------------------------------------
// LLViewerDynamicTexture()
//-----------------------------------------------------------------------------
LLViewerDynamicTexture::LLViewerDynamicTexture(S32 width, S32 height, S32 components, EOrder order, bool clamp) :
    LLViewerTexture(width, height, components, false),
    mClamp(clamp)
{
    llassert((1 <= components) && (components <= 4));

    generateTextureData();

    // Vulkan (#83): publish the UUID now, before any render. Consumers can resolve
    // it the moment it is handed out (the world resolves the appearance composite as
    // soon as BOM faces switch to it) and must never fetch it as an asset; until the
    // first recorded render it samples black (world) / white (UI).
    if (mTextureData.notNull())
    {
        LLRenderVKBackend::attachTarget(mTextureData.get(), getID(), 0, -1);
    }

    llassert( 0 <= order && order < ORDER_COUNT );
    LLViewerDynamicTexture::sInstances[ order ].insert(this);
}

//-----------------------------------------------------------------------------
// LLViewerDynamicTexture()
//-----------------------------------------------------------------------------
LLViewerDynamicTexture::~LLViewerDynamicTexture()
{
    for( S32 order = 0; order < ORDER_COUNT; order++ )
    {
        LLViewerDynamicTexture::sInstances[order].erase(this);  // will fail in all but one case.
    }
    // Unpublish (UI binds / world lookup), then free the image past the frames
    // still in flight.
    LLRenderVKBackend::detachTarget(mTextureData.get(), getID());
    LLRenderVKBackend::releaseTarget(mVKTarget);
    mVKTarget = 0;
}

//virtual
S8 LLViewerDynamicTexture::getType() const
{
    return LLViewerTexture::DYNAMIC_TEXTURE ;
}

//-----------------------------------------------------------------------------
// generateTextureData()
//-----------------------------------------------------------------------------
void LLViewerDynamicTexture::generateTextureData()
{
    LLViewerTexture::generateTextureData() ;
    if (mComponents < 1 || mComponents > 4)
    {
        LL_ERRS() << "Bad number of components in dynamic texture: " << mComponents << LL_ENDL;
    }

    LLPointer<LLImageRaw> raw_image = new LLImageRaw(mFullWidth, mFullHeight, mComponents);
    setFromRaw(0, raw_image);
    setAddressMode((mClamp) ? LLTexUnit::TAM_CLAMP : LLTexUnit::TAM_WRAP);
    mTextureData->setContentReady(false);
}

//-----------------------------------------------------------------------------
// render()
//-----------------------------------------------------------------------------
bool LLViewerDynamicTexture::render()
{
    return false;
}

//-----------------------------------------------------------------------------
// preRender()
//-----------------------------------------------------------------------------
void LLViewerDynamicTexture::preRender(bool clear_depth)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_UI;

     //use the bottom left corner
    mOrigin.set(0, 0);

    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    // Set up camera
    LLViewerCamera* camera = LLViewerCamera::getInstance();
    mCamera.setOrigin(*camera);
    mCamera.setAxes(*camera);
    mCamera.setAspect(camera->getAspect());
    mCamera.setView(camera->getView());
    mCamera.setNear(camera->getNear());

    // The render target is already bound (renderVK) and sized to the texture;
    // depth was cleared by the segment.
}

//-----------------------------------------------------------------------------
// postRender()
//-----------------------------------------------------------------------------
void LLViewerDynamicTexture::postRender(bool success)
{
    // The pixels are in the render target already (no framebuffer copy).
    // Publish it: binds of this texture sample the target, the world finds it
    // by UUID, and the generation tells the world to refresh its copy.
    if (success && mVKTarget && vkNeedsTarget())
    {
        if (mTextureData.isNull() || mTextureData->getWidth() <= 0)
        {
            generateTextureData();   // the size the world's slot is made at
        }
        if (mTextureData.notNull())
        {
            mTextureData->setContentReady(true);
            ++mVKRenderGen;
            LLRenderVKBackend::attachTarget(mTextureData.get(), getID(), mVKTarget, mVKRenderGen);
            // (#707) The pixels live in the render target; the blank full-size raw from
            // generateTextureData only carried the size (own avatar composites: ten
            // 2048^2 RGBA placeholders, ~160 MB). The world makes its slot from a
            // transient blank of the same size.
            mTextureData->releaseVKPlaceholder();
        }
    }
    LLViewerCamera* camera = LLViewerCamera::getInstance();
    camera->setOrigin(mCamera);
    camera->setAxes(mCamera);
    camera->setAspect(mCamera.getAspect());
    camera->setViewNoBroadcast(mCamera.getView());
    camera->setNear(mCamera.getNear());
}

//-----------------------------------------------------------------------------
// Vulkan (#83)
//-----------------------------------------------------------------------------
bool LLViewerDynamicTexture::renderVK(bool clear_depth)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_UI;
    if (!vkNeedsTarget())
    {
        preRender(clear_depth);
        const bool result = render();
        gGL.flush();
        postRender(result);
        return result;
    }

    const bool want_depth = vkWantsDepth();
    if (mVKTarget && (mVKTargetW != mFullWidth || mVKTargetH != mFullHeight || mVKTargetDepth != want_depth))
    {
        LLRenderVKBackend::releaseTarget(mVKTarget);
        mVKTarget = 0;
    }
    if (!mVKTarget)
    {
        mVKTarget = LLRenderVKBackend::createTarget(mFullWidth, mFullHeight, want_depth);
        mVKTargetW = mFullWidth;
        mVKTargetH = mFullHeight;
        mVKTargetDepth = want_depth;
    }
    if (!mVKTarget) return false;

    // Open the segment BEFORE preRender: the composite's preRender pushes its ortho
    // projection, and the hints' preRender re-renders the composites (nested
    // segments, recorded ahead of the hint's own draws).
    const F32 clear[4] = { 0.f, 0.f, 0.f, 0.f };
    if (!LLRenderVKBackend::beginTarget(mVKTarget, clear, clear_depth)) return false;
    gGL.color4f(1.f, 1.f, 1.f, 1.f);
    preRender(clear_depth);
    const bool result = render();
    gGL.flush();
    postRender(result);
    LLRenderVKBackend::endTarget();
    return result;
}

// static
void LLViewerDynamicTexture::updateAllInstancesVK()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_UI;
    sNumRenders = 0;
    if (!LLRender::hasBackend() || sVKPassActive || LLRenderVKBackend::inTarget()) return;
    bool any = false;
    for (S32 order = 0; order < ORDER_COUNT && !any; ++order) any = !sInstances[order].empty();
    if (!any) return;

    sVKPassActive = true;
    // Preview renders move the camera (restored by postRender); the "current"
    // matrices feed the HUD text projection later this frame -- keep them.
    const glm::mat4 saved_proj = get_current_projection();
    const glm::mat4 saved_mv = get_current_modelview();
    // ORDER_MIDDLE previews / hints first, then ORDER_LAST composites, then
    // ORDER_RESET (restores the avatar's visual params after the hints), as GL did.
    std::vector<LLViewerDynamicTexture*> snapshot;
    for (S32 order = 0; order < ORDER_COUNT; ++order)
    {
        // render() can create / destroy instances (composites): walk a copy and
        // skip anything no longer registered.
        snapshot.assign(sInstances[order].begin(), sInstances[order].end());
        for (LLViewerDynamicTexture* dt : snapshot)
        {
            if (!sInstances[order].count(dt)) continue;
            if (!dt->needsRender()) continue;
            if (dt->renderVK(true)) ++sNumRenders;
        }
    }
    set_current_projection(saved_proj);
    set_current_modelview(saved_mv);
    sVKPassActive = false;
}
