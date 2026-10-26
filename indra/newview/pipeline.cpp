/**
 * @file pipeline.cpp
 * @brief Rendering pipeline.
 *
 * $LicenseInfo:firstyear=2005&license=viewerlgpl$
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
#include "llcrossingtrace.h"

#include "pipeline.h"
#include "llworldclock.h"
#include "llcleanverify.h"   // LL-draw unreachability tripwires
#include "llcleaninstancestore.h"
#include "llcleanmaterialstore.h"
#include "llcleanmeshasset.h"
#include "llcleaningest.h"
#include "llcleanskinstore.h"

// library includes
#include "llimagepng.h"
#include "llaudioengine.h" // For debugging.
#include "llerror.h"
#include "llviewercontrol.h"
#include "llfasttimer.h"
#include "llfontgl.h"
#include "llfontvertexbuffer.h"
#include "llnamevalue.h"
#include "llpointer.h"
#include "llprimitive.h"
#include "llvolume.h"
#include "material_codes.h"
#include "v3color.h"
#include "llui.h"
#include "llrender.h"
#include "llstartup.h"
#include "llwindow.h"

// newview includes
#include "llagent.h"
#include "llagentcamera.h"
#include "llappviewer.h"
#include "lltexturecache.h"
#include "lltexturefetch.h"
#include "llimageworker.h"
#include "lldrawable.h"
#include "llface.h"
#include "llfeaturemanager.h"
#include "llfloatertelehub.h"
#include "llfloaterreg.h"
#include "llhudmanager.h"
#include "llhudnametag.h"
#include "llhudtext.h"
#include "lllightconstants.h"
#include "llmeshrepository.h"
#include "llphysicsshapebuilderutil.h"   // Show Physics Shapes (VK overlay, #29)
#include "llvolumemgr.h"
#include "llconvexdecomposition.h"
#include "llpipelinelistener.h"
#include "llresmgr.h"
#include "llselectmgr.h"
#include "llsky.h"
#include "lltracker.h"
#include "lltool.h"
#include "lltoolmgr.h"
#include "llviewercamera.h"
#include "llviewermediafocus.h"
#include "llviewertexturelist.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerparcelmgr.h"
#include "llviewerregion.h" // for audio debugging.
#include "llviewerwindow.h" // For getSpinAxis
#include "llvoavatarself.h"
#include "llvocache.h"
#include "llvosky.h"
#include "llvotree.h"
#include "llvovolume.h"
#include "llvosurfacepatch.h"
#include "llvowater.h"
#include "llvotree.h"
#include "llvopartgroup.h"
#include "llcleanpartpick.h"   // GPU particle pick (#481)
#include "llworld.h"
#include "llrender.h"
#include "llviewerstats.h"
#include "llviewerjoystick.h"
#include "llviewerdisplay.h"
#include "llmutelist.h"
#include "lltoolpie.h"
#include "llnotifications.h"
#include "llpathinglib.h"
#include "llfloaterpathfindingconsole.h"
#include "llfloaterpathfindingcharacters.h"
#include "llfloatertools.h"
#include "llpanelface.h"
#include "llpathfindingpathtool.h"
#include "llprogressview.h"
#include "llcleanup.h"
#include "gltfscenemanager.h"
// [RLVa:KB] - Checked: RLVa-2.0.0
#include "llvisualeffect.h"
#include "rlvactions.h"
#include "rlvlocks.h"
// [/RLVa:KB]

#include "llenvironment.h"
#include "llsettingsvo.h"

#include "llerror.h"

extern bool gSnapshot;
bool gShiftFrame = false;

//cached settings
U32 LLPipeline::RenderFSAAType;
bool LLPipeline::RenderUIBuffer;
S32 LLPipeline::RenderShadowDetail;
bool LLPipeline::RenderDeferredSSAO;
bool LLPipeline::RenderAnimateRes;
bool LLPipeline::FreezeTime;
S32 LLPipeline::DebugBeaconLineWidth;
bool LLPipeline::RenderDepthOfField;
F32 LLPipeline::RenderFarClip;
bool LLPipeline::RenderScreenSpaceReflections;
bool LLPipeline::RenderMirrors;
LLTrace::EventStatHandle<S64> LLPipeline::sStatBatchSize("renderbatchsize");

const U32 LLPipeline::MAX_PREVIEW_WIDTH = 512;

LLPipeline gPipeline;
const LLMatrix4* gGLLastMatrix = NULL;





void display_update_camera();

S32     LLPipeline::sCompiles = 0;

bool    LLPipeline::sPickAvatar = true;
bool    LLPipeline::sDynamicLOD = true;
bool    LLPipeline::sShowHUDAttachments = true;
bool    LLPipeline::sRenderMOAPBeacons = false;
bool    LLPipeline::sRenderPhysicalBeacons = true;
bool    LLPipeline::sRenderScriptedBeacons = false;
bool    LLPipeline::sRenderScriptedTouchBeacons = true;
bool    LLPipeline::sRenderParticleBeacons = false;
bool    LLPipeline::sRenderSoundBeacons = false;
bool    LLPipeline::sRenderBeacons = false;
bool    LLPipeline::sRenderHighlight = true;
LLRender::eTexIndex LLPipeline::sRenderHighlightTextureChannel = LLRender::DIFFUSE_MAP;
bool    LLPipeline::sForceOldBakedUpload = false;
S32     LLPipeline::sUseOcclusion = 0;
bool    LLPipeline::sAutoMaskAlphaDeferred = true;
bool    LLPipeline::sAutoMaskAlphaNonDeferred = false;
bool    LLPipeline::sRenderTransparentWater = true;
bool    LLPipeline::sBakeSunlight = false;
bool    LLPipeline::sNoAlpha = false;
bool    LLPipeline::sShadowRender = false;
bool    LLPipeline::sRenderGlow = false;
bool    LLPipeline::sReflectionRender = false;
bool    LLPipeline::sImpostorRender = false;
bool    LLPipeline::sTextureBindTest = false;
bool    LLPipeline::sRenderAttachedLights = true;
bool    LLPipeline::sRenderAttachedParticles = true;
bool    LLPipeline::sRenderDeferred = false;
bool    LLPipeline::sScopeCapture = false;
bool    LLPipeline::sGradeBypass = false;
U32     LLPipeline::sGradeBypassMask = 0;

void LLPipeline::requestScenePixel(S32 x, S32 y, scene_pixel_cb_t callback)
{
    if (!callback)
    {
        return;
    }
    mScenePixelX = x;
    mScenePixelY = y;
    mScenePixelCallback = std::move(callback);
    mScenePixelPending = true;
}

// static
bool LLPipeline::worldViewUV(S32 scaled_x, S32 scaled_y, F32& u, F32& v)
{
    // Stay in scaled window space throughout (upstream: mixing the window's
    // raw/scaled ratio with the world view's raw rect drifts across the frame).
    const LLRect world = gViewerWindow->getWorldViewRectScaled();
    if (world.getWidth() <= 0 || world.getHeight() <= 0)
    {
        return false;
    }
    u = (F32)(scaled_x - world.mLeft)   / (F32)world.getWidth();
    v = (F32)(scaled_y - world.mBottom) / (F32)world.getHeight();
    return (u >= 0.f && u <= 1.f && v >= 0.f && v <= 1.f);
}

bool LLPipeline::getScopePixel(S32 scaled_x, S32 scaled_y, LLColor4U& out) const
{
    const S32 width = mScopeW, height = mScopeH;
    if (width <= 0 || height <= 0 || mScopeReadback.size() < (size_t)width * height * 4)
    {
        return false;
    }
    F32 u = 0.f, v = 0.f;
    if (!worldViewUV(scaled_x, scaled_y, u, v))
    {
        return false;
    }
    // Rows are stored bottom-up, so v == 0 is row 0 on both sides.
    const S32 sx = llclamp((S32)(u * (F32)width),  0, width  - 1);
    const S32 sy = llclamp((S32)(v * (F32)height), 0, height - 1);
    const size_t at = ((size_t)sy * width + sx) * 4;
    out.set(mScopeReadback[at], mScopeReadback[at + 1], mScopeReadback[at + 2], 255);
    return true;
}
S32     LLPipeline::sVisibleLightCount = 0;
bool    LLPipeline::sShowDebugAlpha = false;
// [SL:KB] - Patch: Render-TextureToggle (Catznip-4.0)
bool    LLPipeline::sRenderTextures = true;
// [/SL:KB]

// EventHost API LLPipeline listener.
static LLPipelineListener sPipelineListener;

LLPipeline::LLPipeline() :
    mMatrixOpCount(0),
    mTextureMatrixOps(0),
    mRenderDebugFeatureMask(0),
    mRenderDebugMask(0)
{
}

LLPipeline::~LLPipeline()
{
}

void LLPipeline::getGLResidueSizes(size_t& group_q1, size_t& partition_q, size_t& probes) const
{
    group_q1     = 0;   // LL group rebuild queue is gone
    partition_q  = mPartitionQ.size();
    probes       = 0;   // LL probe list is gone (Vulkan owns probes)
}

//static
void LLPipeline::updateRenderTransparentWater()
{
    sRenderTransparentWater = gSavedSettings.getBOOL("RenderTransparentWater");
}

// static
void LLPipeline::refreshCachedSettings()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DISPLAY;
    LLPipeline::sAutoMaskAlphaDeferred = gSavedSettings.getBOOL("RenderAutoMaskAlphaDeferred");
    LLPipeline::sAutoMaskAlphaNonDeferred = gSavedSettings.getBOOL("RenderAutoMaskAlphaNonDeferred");
    // CLEAN: impostors OFF permanently -- the impostor path is an LL scene
    // render (avatars drawn to offscreen FBOs through LL pools). All avatars
    // render direct through the clean rigged path.
    LLVOAvatar::sMaxNonImpostors = 0;
    LLVOAvatar::updateImpostorRendering(0);

    // CLEAN: LL occlusion queries OFF -- the clean pipeline has no LL
    // spatial-group occlusion consumer (GPU depth-based occlusion is a
    // planned clean feature, task #50); the queries were pure overhead.
    LLPipeline::sUseOcclusion = 0;

    RenderFSAAType = gSavedSettings.getU32("RenderFSAAType");
    RenderUIBuffer = gSavedSettings.getBOOL("RenderUIBuffer");
    RenderShadowDetail = gSavedSettings.getS32("RenderShadowDetail");
    RenderDeferredSSAO = gSavedSettings.getBOOL("RenderDeferredSSAO");
    RenderAnimateRes = gSavedSettings.getBOOL("RenderAnimateRes");
    FreezeTime = gSavedSettings.getBOOL("FreezeTime");
    DebugBeaconLineWidth = gSavedSettings.getS32("DebugBeaconLineWidth");
    RenderDepthOfField = gSavedSettings.getBOOL("RenderDepthOfField");
    RenderFarClip = gSavedSettings.getF32("RenderFarClip");
    RenderScreenSpaceReflections = gSavedSettings.getBOOL("RenderScreenSpaceReflections");
    RenderMirrors = gSavedSettings.getBOOL("RenderMirrors");

    if (gNonInteractive)
    {
        LLVOAvatar::sMaxNonImpostors = 1;
        LLVOAvatar::updateImpostorRendering(LLVOAvatar::sMaxNonImpostors);
    }

    LLFontVertexBuffer::enableBufferCollection(gSavedSettings.getBOOL("CollectFontVertexBuffers"));
}

LLDrawPool *LLPipeline::findPool(const U32 type, LLViewerTexture *tex0)
{
    LLDrawPool *poolp = NULL;
    switch( type )
    {
    case LLDrawPool::POOL_SIMPLE:
        poolp = mSimplePool;
        break;

    case LLDrawPool::POOL_GRASS:
        poolp = mGrassPool;
        break;

    case LLDrawPool::POOL_ALPHA_MASK:
        poolp = mAlphaMaskPool;
        break;

    case LLDrawPool::POOL_FULLBRIGHT_ALPHA_MASK:
        poolp = mFullbrightAlphaMaskPool;
        break;

    case LLDrawPool::POOL_FULLBRIGHT:
        poolp = mFullbrightPool;
        break;

    case LLDrawPool::POOL_GLOW:
        poolp = mGlowPool;
        break;

    case LLDrawPool::POOL_TREE:
        poolp = get_if_there(mTreePools, (uintptr_t)tex0, (LLDrawPool*)0 );
        break;

    case LLDrawPool::POOL_TERRAIN:
        poolp = get_if_there(mTerrainPools, (uintptr_t)tex0, (LLDrawPool*)0 );
        break;

    case LLDrawPool::POOL_BUMP:
        poolp = mBumpPool;
        break;
    case LLDrawPool::POOL_MATERIALS:
        poolp = mMaterialsPool;
        break;
    case LLDrawPool::POOL_ALPHA_PRE_WATER:
        poolp = mAlphaPoolPreWater;
        break;
    case LLDrawPool::POOL_ALPHA_POST_WATER:
        poolp = mAlphaPoolPostWater;
        break;

    case LLDrawPool::POOL_AVATAR:
    case LLDrawPool::POOL_CONTROL_AV:
        break; // Do nothing

    case LLDrawPool::POOL_SKY:
        poolp = mSkyPool;
        break;

    case LLDrawPool::POOL_WATER:
        poolp = mWaterPool;
        break;

    case LLDrawPool::POOL_WL_SKY:
        poolp = mWLSkyPool;
        break;

    case LLDrawPool::POOL_GLTF_PBR:
        poolp = mPBROpaquePool;
        break;
    case LLDrawPool::POOL_GLTF_PBR_ALPHA_MASK:
        poolp = mPBRAlphaMaskPool;
        break;

    case LLDrawPool::POOL_WATEREXCLUSION:
        poolp = mWaterExclusionPool;
        break;

    default:
        llassert(0);
        LL_ERRS() << "Invalid Pool Type in  LLPipeline::findPool() type=" << type << LL_ENDL;
        break;
    }

    return poolp;
}


LLDrawPool *LLPipeline::getPool(const U32 type, LLViewerTexture *tex0)
{
    LLDrawPool *poolp = findPool(type, tex0);
    if (poolp)
    {
        return poolp;
    }

    LLDrawPool *new_poolp = LLDrawPool::createPool(type, tex0);
    addPool( new_poolp );

    return new_poolp;
}

//static
U32 LLPipeline::getPoolTypeFromTE(const LLTextureEntry* te, LLViewerTexture* imagep)
{
    if (!te || !imagep)
    {
        return 0;
    }

    LLMaterial* mat = te->getMaterialParams().get();
    LLGLTFMaterial* gltf_mat = te->getGLTFRenderMaterial();

    bool color_alpha = te->getColor().mV[3] < 0.999f;
    bool alpha = color_alpha;
    if (imagep)
    {
        alpha = alpha || (imagep->getComponents() == 4 && imagep->getType() != LLViewerTexture::MEDIA_TEXTURE) || (imagep->getComponents() == 2);
    }

    if (alpha && mat)
    {
        switch (mat->getDiffuseAlphaMode())
        {
            case 1:
                alpha = true; // Material's alpha mode is set to blend.  Toss it into the alpha draw pool.
                break;
            case 0: //alpha mode set to none, never go to alpha pool
            case 3: //alpha mode set to emissive, never go to alpha pool
                alpha = color_alpha;
                break;
            default: //alpha mode set to "mask", go to alpha pool if fullbright
                alpha = color_alpha; // Material's alpha mode is set to none, mask, or emissive.  Toss it into the opaque material draw pool.
                break;
        }
    }

    if (alpha || (gltf_mat && gltf_mat->mAlphaMode == LLGLTFMaterial::ALPHA_MODE_BLEND))
    {
        return LLDrawPool::POOL_ALPHA;
    }
    else if ((te->getBumpmap() || te->getShiny()) && (!mat || mat->getNormalID().isNull()))
    {
        return LLDrawPool::POOL_BUMP;
    }
    else if (gltf_mat)
    {
        return LLDrawPool::POOL_GLTF_PBR;
    }
    else if (mat && !alpha)
    {
        return LLDrawPool::POOL_MATERIALS;
    }
    else
    {
        return LLDrawPool::POOL_SIMPLE;
    }
}


void LLPipeline::addPool(LLDrawPool *new_poolp)
{
    mPools.insert(new_poolp);
    addToQuickLookup( new_poolp );
}


void LLPipeline::allocDrawable(LLViewerObject *vobj)
{
    LLDrawable *drawable = new LLDrawable(vobj);
    vobj->mDrawable = drawable;

    //encompass completely sheared objects by taking
    //the most extreme point possible (<1,1,0.5>)
    drawable->setRadius(LLVector3(1,1,0.5f).scaleVec(vobj->getScale()).length());
    if (vobj->isOrphaned())
    {
        drawable->setState(LLDrawable::FORCE_INVISIBLE);
    }
    drawable->updateXform(true);
}

U32 LLPipeline::addObject(LLViewerObject *vobj)
{
    createObject(vobj);
    return 1;
}

void LLPipeline::createObject(LLViewerObject* vobj)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    LLDrawable* drawablep = vobj->mDrawable;

    if (!drawablep)
    {
        drawablep = vobj->createDrawable(this);
    }
    else
    {
        LL_ERRS() << "Redundant drawable creation!" << LL_ENDL;
    }

    llassert(drawablep);

    if (vobj->getParent())
    {
        vobj->setDrawableParent(((LLViewerObject*)vobj->getParent())->mDrawable); // LLPipeline::addObject 1
    }
    else
    {
        vobj->setDrawableParent(NULL); // LLPipeline::addObject 2
    }

    markRebuild(drawablep, LLDrawable::REBUILD_ALL);

    if (drawablep->getVOVolume() && RenderAnimateRes)
    {
        // fun animated res
        drawablep->updateXform(true);
        drawablep->clearState(LLDrawable::MOVE_UNDAMPED);
        drawablep->setScale(LLVector3(0,0,0));
        drawablep->makeActive();
    }
}

// VISIBILITY FOR EASING (VK). LLDrawable::updateXform only interpolates a damped
// move while the drawable isVisible() -- a flag LL's cull stamps, which Vulkan
// diverts -- otherwise it SNAPS to the target (scripted llSetRot/llSetPos stopped
// gliding; HUD parts jumped). Supply the cull's answer for moving drawables:
// HUD parts always, world objects when their bounds meet the camera frustum.
//
// Both inputs come from OBJECT state: a child drawable's getPositionAgent() goes
// through its render matrix, which VK doesn't maintain, so linked children
// failed the frustum test and every scripted move of a child prim SNAPPED
// (llSetLinkPrimitiveParams motion hit distinct points). updateXform also only
// interpolates while the step exceeds MIN_INTERPOLATE_DISTANCE * the camera
// distance, and mDistanceWRTCamera is set by LL's cull (updateDistance) --
// stale under VK, so normal steps fell under the threshold and snapped. Set it
// here from the same position.
static void vkStampMoveVisibility(LLDrawable* drawablep)
{
    LLViewerObject* vo = (drawablep && !drawablep->isDead()) ? drawablep->getVObj().get() : nullptr;
    if (!vo) return;
    // WORN ATTACHMENTS (RenderVKAttachStampFromAvatar): every attachment of every avatar
    // is stamped every frame (idleUpdateMisc -> updateMoveDampedAsync, and again from the
    // move list), and its object position is a parent-chain + region->agent recompute
    // (perf: ~1.3% of the main thread). The stamp only feeds updateXform's easing of the
    // attachment's LOCAL move, so derive it from the wearer: one position per avatar per
    // frame (a single-entry cache -- an avatar's attachments are visited together), the
    // frustum test on a sphere covering the avatar and the attachment.
    static LLCachedControl<bool> s_from_av(gSavedSettings, "RenderVKAttachStampFromAvatar", true);
    if (s_from_av && vo->isAttachment() && !vo->isHUDAttachment())
    {
        LLVOAvatar* av = vo->getAvatar();
        if (av && !av->isDead() && av->mDrawable.notNull())
        {
            static const LLVOAvatar* s_av = nullptr;
            static U32 s_frame = ~0u;
            static LLVector3 s_pos;
            static F32 s_cam_dist = 0.f, s_av_rad = 0.f;
            const U32 frame = (U32)LLFrameTimer::getFrameCount();
            if (av != s_av || frame != s_frame)
            {
                s_av = av; s_frame = frame;
                s_pos = av->getRenderPosition();
                s_av_rad = llmax(av->mDrawable->getRadius(), 1.f);
                s_cam_dist = (s_pos - LLViewerCamera::getInstance()->getOrigin()).magVec();
            }
            const F32 r = drawablep->getRadius();
            drawablep->mDistanceWRTCamera = llmax(0.01f, s_cam_dist - r);
            if (LLViewerCamera::getInstance()->sphereInFrustum(s_pos, s_av_rad + r))
                drawablep->setVisible();
            return;
        }
    }
    const LLVector3 pos = vo->isHUDAttachment() ? drawablep->getPositionAgent() : vo->getPositionAgent();
    if (!vo->isHUDAttachment())
        drawablep->mDistanceWRTCamera = llmax(0.01f, (pos - LLViewerCamera::getInstance()->getOrigin()).magVec()
                                                     - drawablep->getRadius());
    if (vo->isHUDAttachment() ||
        LLViewerCamera::getInstance()->sphereInFrustum(pos, drawablep->getRadius()))
    {
        drawablep->setVisible();
    }
}

//external functions for asynchronous updating
void LLPipeline::updateMoveDampedAsync(LLDrawable* drawablep)
{
    LL_PROFILE_ZONE_SCOPED;
    // A drawable that has never moved (generation < 0) still takes its first move while
    // frozen (#880): until then getRenderPosition() falls back to the object's LIVE
    // position, so an avatar that arrived after the freeze kept walking around. That one
    // move pins it where it first appeared.
    if (LLWorldClock::isFrozen() && !(drawablep && drawablep->getGeneration() < 0))   // FreezeTime or Freeze World
    {
        return;
    }
    if (!drawablep)
    {
        LL_ERRS() << "updateMove called with NULL drawablep" << LL_ENDL;
        return;
    }
    if (drawablep->isState(LLDrawable::EARLY_MOVE))
    {
        return;
    }

    // update drawable now
    drawablep->clearState(LLDrawable::MOVE_UNDAMPED); // force to DAMPED
    // EARLY damped moves (a seated avatar easing its seat's root, every avatar's own
    // drawable, the tracked camera focus object) run from idleUpdate, BEFORE
    // updateMovedList stamps this frame's visibility -- so under VK the drawable was
    // never isVisible() here and updateXform skipped LL's damping: a driven vehicle
    // drew (and carried its driver and the camera) at the raw interpolated network
    // pose, every server correction a visible step; FS/LL smooth it over
    // OBJECT_DAMPING_TIME_CONSTANT (#180). Stamp it like the move-list walk does.
    static LLCachedControl<bool> s_early_vis(gSavedSettings, "RenderVKEarlyMoveVisibility", true);
    if (s_early_vis)
        vkStampMoveVisibility(drawablep);
    // Clean pipeline: report the move only if the drawable xform changed (attachments
    // riding their avatar keep a constant local xform; a seat or an easing HUD part don't).
    const LLVector3    old_pos = drawablep->mXform.getPosition();
    const LLQuaternion old_rot = drawablep->mXform.getRotation();
    const LLVector3    old_scale = drawablep->getScale();   // eased scale (#180)
    drawablep->updateMove(); // returns done
    if (drawablep->getVObj() && !drawablep->isDead()
        && (drawablep->mXform.getPosition() != old_pos || drawablep->mXform.getRotation() != old_rot
            || drawablep->getScale() != old_scale))
    {
        LLCleanIngest::instance().noteObjectMoved(drawablep->getVObj(), /*easing=*/true);
    }
    drawablep->setState(LLDrawable::EARLY_MOVE); // flag says we already did an undamped move this frame
    // Put on move list so that EARLY_MOVE gets cleared
    if (!drawablep->isState(LLDrawable::ON_MOVE_LIST))
    {
        mMovedList.push_back(drawablep);
        drawablep->setState(LLDrawable::ON_MOVE_LIST);
    }
}

void LLPipeline::updateMoveNormalAsync(LLDrawable* drawablep)
{
    LL_PROFILE_ZONE_SCOPED;
    // A drawable that has never moved (generation < 0) still takes its first move while
    // frozen (#880): until then getRenderPosition() falls back to the object's LIVE
    // position, so an avatar that arrived after the freeze kept walking around. That one
    // move pins it where it first appeared.
    if (LLWorldClock::isFrozen() && !(drawablep && drawablep->getGeneration() < 0))   // FreezeTime or Freeze World
    {
        return;
    }
    if (!drawablep)
    {
        LL_ERRS() << "updateMove called with NULL drawablep" << LL_ENDL;
        return;
    }
    if (drawablep->isState(LLDrawable::EARLY_MOVE))
    {
        return;
    }

    // update drawable now
    drawablep->setState(LLDrawable::MOVE_UNDAMPED); // force to UNDAMPED
    // Clean pipeline: report the move only if the drawable xform changed (attachments
    // riding their avatar keep a constant local xform; a seat or an easing HUD part don't).
    const LLVector3    old_pos = drawablep->mXform.getPosition();
    const LLQuaternion old_rot = drawablep->mXform.getRotation();
    const LLVector3    old_scale = drawablep->getScale();
    drawablep->updateMove();
    if (drawablep->getVObj() && !drawablep->isDead()
        && (drawablep->mXform.getPosition() != old_pos || drawablep->mXform.getRotation() != old_rot
            || drawablep->getScale() != old_scale))
    {
        LLCleanIngest::instance().noteObjectMoved(drawablep->getVObj(), /*easing=*/true);
    }
    drawablep->setState(LLDrawable::EARLY_MOVE); // flag says we already did an undamped move this frame
    // Put on move list so that EARLY_MOVE gets cleared
    if (!drawablep->isState(LLDrawable::ON_MOVE_LIST))
    {
        mMovedList.push_back(drawablep);
        drawablep->setState(LLDrawable::ON_MOVE_LIST);
    }
}

void LLPipeline::updateMovedList(LLDrawable::drawable_vector_t& moved_list)
{
    LL_PROFILE_ZONE_SCOPED;
    // Stable in-place compaction (O(n)): the old loop erased finished entries from the
    // middle of the vector one at a time -- O(n^2) element shifts plus LLPointer
    // ref/unref churn, and in a crowd every avatar and moving attachment is on this
    // list every frame. Index-based, so entries appended during the walk (markMoved
    // from inside an update) are kept for next frame instead of invalidating iterators.
    const size_t n0 = moved_list.size();
    size_t keep_n = 0;
    const bool frozen = LLWorldClock::isFrozen();   // FreezeTime or Freeze World
    for (size_t r = 0; r < n0; ++r)
    {
        LLDrawable *drawablep = moved_list[r].get();
        if (!drawablep)
        {
            continue;
        }
        if (frozen && drawablep->getGeneration() >= 0)
        {
            // World frozen: a drawable that has moved before holds its pose (and its
            // EARLY_MOVE / list state) until the thaw.
            if (keep_n != r)
            {
                moved_list[keep_n] = std::move(moved_list[r]);
            }
            ++keep_n;
            continue;
        }
        bool done = true;
        const bool damped_move = !drawablep->isState(LLDrawable::MOVE_UNDAMPED);
        vkStampMoveVisibility(drawablep);
        const bool early = drawablep->isState(LLDrawable::EARLY_MOVE);
        if (!drawablep->isDead() && !early)
        {
            done = drawablep->updateMove();
        }
        // Clean pipeline: a damped move eases the drawable xform every frame (LL's
        // smooth scripted motion) -- refresh the instance pose each frame it eases,
        // including the final snap. Attachments too (a HUD part rotating), which the
        // plain moved hook skips (avatar-motion attachment moves are undamped).
        // EARLY_MOVE entries were moved (and reported, if they changed) by
        // updateMove*Async; nothing was computed here. Reporting them anyway queued
        // every attachment of every avatar every frame -- ~80% of the moved queue (#96).
        if (damped_move && !early && !drawablep->isDead() && drawablep->getVObj())
            LLCleanIngest::instance().noteObjectMoved(drawablep->getVObj(), /*easing=*/true);
        drawablep->clearState(LLDrawable::EARLY_MOVE | LLDrawable::MOVE_UNDAMPED);
        if (done)
        {
            if (drawablep->isRoot() && !drawablep->isState(LLDrawable::ACTIVE))
            {
                drawablep->makeStatic();
            }
            drawablep->clearState(LLDrawable::ON_MOVE_LIST);
            if (drawablep->isState(LLDrawable::ANIMATED_CHILD))
            { //will likely not receive any future world matrix updates
                // -- this keeps attachments from getting stuck in space and falling off your avatar
                drawablep->clearState(LLDrawable::ANIMATED_CHILD);
                markRebuild(drawablep, LLDrawable::REBUILD_VOLUME);
            }
            continue;   // finished: dropped below
        }
        if (keep_n != r)
        {
            moved_list[keep_n] = std::move(moved_list[r]);
        }
        ++keep_n;
    }
    moved_list.erase(moved_list.begin() + keep_n, moved_list.begin() + n0);
}

void LLPipeline::updateMove()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;

    // Frozen: updateMovedList only gives never-moved drawables their first move (#880).
    updateMovedList(mMovedList);
}

/////////////////////////////////////////////////////////////////////////////
// Culling and occlusion testing
/////////////////////////////////////////////////////////////////////////////

//static
F32 LLPipeline::calcPixelArea(LLVector3 center, LLVector3 size, LLCamera &camera)
{
    LLVector3 lookAt = center - camera.getOrigin();
    F32 dist = lookAt.length();

    //ramp down distance for nearby objects
    //shrink dist by dist/16.
    if (dist < 16.f)
    {
        dist /= 16.f;
        dist *= dist;
        dist *= 16.f;
    }

    //get area of circle around node
    F32 app_angle = atanf(size.length()/dist);
    F32 radius = app_angle*LLDrawable::sCurPixelAngle;
    return radius*radius * F_PI;
}

//static
F32 LLPipeline::calcPixelArea(const LLVector4a& center, const LLVector4a& size, LLCamera &camera)
{
    LLVector4a origin;
    origin.load3(camera.getOrigin().mV);

    LLVector4a lookAt;
    lookAt.setSub(center, origin);
    F32 dist = lookAt.getLength3().getF32();

    //ramp down distance for nearby objects
    //shrink dist by dist/16.
    if (dist < 16.f)
    {
        dist /= 16.f;
        dist *= dist;
        dist *= 16.f;
    }

    //get area of circle around node
    F32 app_angle = atanf(size.getLength3().getF32() / dist);
    F32 radius = app_angle * LLDrawable::sCurPixelAngle;
    return radius * radius * F_PI;
}

void LLPipeline::updateGL()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    {
        while (!LLGLUpdate::sGLQ.empty())
        {
            LLGLUpdate* glu = LLGLUpdate::sGLQ.front();
            glu->updateGL();
            glu->mInQ = false;
            LLGLUpdate::sGLQ.pop_front();
        }
    }
}

void LLPipeline::markMoved(LLDrawable *drawablep, bool damped_motion)
{
    if (!drawablep)
    {
        //LL_ERRS() << "Sending null drawable to moved list!" << LL_ENDL;
        return;
    }

    if (drawablep->isDead())
    {
        LL_WARNS() << "Marking NULL or dead drawable moved!" << LL_ENDL;
        return;
    }

    // Clean pipeline: re-register this object's instances next frame (statics
    // otherwise only pick up a new transform when the rolling sweep laps them).
    if (drawablep->getVObj())
        LLCleanIngest::instance().noteObjectMoved(drawablep->getVObj());

    if (drawablep->getParent())
    {
        //ensure that parent drawables are moved first
        markMoved(drawablep->getParent(), damped_motion);
    }

    if (!drawablep->isState(LLDrawable::ON_MOVE_LIST))
    {
        mMovedList.push_back(drawablep);
        drawablep->setState(LLDrawable::ON_MOVE_LIST);
    }
    if (! damped_motion)
    {
        drawablep->setState(LLDrawable::MOVE_UNDAMPED); // UNDAMPED trumps DAMPED
    }
    else if (drawablep->isState(LLDrawable::MOVE_UNDAMPED))
    {
        drawablep->clearState(LLDrawable::MOVE_UNDAMPED);
    }
}

void LLPipeline::markShift(LLDrawable *drawablep)
{
    if (!drawablep || drawablep->isDead())
    {
        return;
    }

    if (!drawablep->isState(LLDrawable::ON_SHIFT_LIST))
    {
        drawablep->getVObj()->setChanged(LLXform::SHIFTED | LLXform::SILHOUETTE);
        if (drawablep->getParent())
        {
            markShift(drawablep->getParent());
        }
        mShiftList.push_back(drawablep);
        drawablep->setState(LLDrawable::ON_SHIFT_LIST);
    }
}

void LLPipeline::shiftObjects(const LLVector3 &offset)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    CROSSING_SCOPE(LLCrossingTrace::CH_PIPELINE_SHIFT, "Crossing pipeline shift");
    gDepthDirty = true;

    LLVector4a offseta;
    offseta.load3(offset.mV);

    {
    // #568: the per-drawable loop alone (LL's agent-space caches: drawable xforms, face
    // centres / extents for texture priority, spatial-entry extents, relative xforms,
    // flexi sections -- all still read under Vulkan, see LLDrawable::shiftPos).
    LLCrossingTrace::Scope loop_scope(LLCrossingTrace::CH_SHIFTPOS_LOOP, (U32)mShiftList.size());
    for (LLDrawable::drawable_vector_t::iterator iter = mShiftList.begin();
            iter != mShiftList.end(); iter++)
    {
        LLDrawable *drawablep = *iter;
        if (drawablep->isDead())
        {
            continue;
        }
        drawablep->shiftPos(offseta);
        drawablep->clearState(LLDrawable::ON_SHIFT_LIST);
    }
    mShiftList.resize(0);
    }

    LLHUDText::shiftAll(offset);
    LLHUDNameTag::shiftAll(offset);

    display_update_camera();
}

void LLPipeline::markTextured(LLDrawable *drawablep)
{
    // Clean pipeline: TE content changed -> re-register the object's faces next frame.
    if (drawablep && !drawablep->isDead() && drawablep->getVObj())
        LLCleanIngest::instance().noteObjectChanged(drawablep->getVObj());
}

void LLPipeline::markGLRebuild(LLGLUpdate* glu)
{
    if (glu && !glu->mInQ)
    {
        LLGLUpdate::sGLQ.push_back(glu);
        glu->mInQ = true;
    }
}

void LLPipeline::markPartitionMove(LLDrawable* drawable)
{
    if (!drawable->isState(LLDrawable::PARTITION_MOVE) &&
        !drawable->getPositionGroup().equals3(LLVector4a::getZero()))
    {
        drawable->setState(LLDrawable::PARTITION_MOVE);
        mPartitionQ.push_back(drawable);
    }
}

void LLPipeline::processPartitionQ()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    for (LLDrawable::drawable_list_t::iterator iter = mPartitionQ.begin(); iter != mPartitionQ.end(); ++iter)
    {
        LLDrawable* drawable = *iter;
        if (!drawable->isDead())
        {
            drawable->updateBinRadius();
            drawable->movePartition();
        }
        drawable->clearState(LLDrawable::PARTITION_MOVE);
    }

    mPartitionQ.clear();
}

void LLPipeline::markRebuild(LLDrawable *drawablep, LLDrawable::EDrawableFlags flag)
{
    // Clean pipeline: colour / texcoord / volume changes -> re-register next frame.
    if (drawablep && !drawablep->isDead() && drawablep->getVObj())
        LLCleanIngest::instance().noteObjectChanged(drawablep->getVObj());
}

void LLPipeline::addToQuickLookup( LLDrawPool* new_poolp )
{
    switch( new_poolp->getType() )
    {
    case LLDrawPool::POOL_SIMPLE:
        if (mSimplePool)
        {
            llassert(0);
            LL_WARNS() << "Ignoring duplicate simple pool." << LL_ENDL;
        }
        else
        {
            mSimplePool = (LLRenderPass*) new_poolp;
        }
        break;

    case LLDrawPool::POOL_ALPHA_MASK:
        if (mAlphaMaskPool)
        {
            llassert(0);
            LL_WARNS() << "Ignoring duplicate alpha mask pool." << LL_ENDL;
            break;
        }
        else
        {
            mAlphaMaskPool = (LLRenderPass*) new_poolp;
        }
        break;

    case LLDrawPool::POOL_FULLBRIGHT_ALPHA_MASK:
        if (mFullbrightAlphaMaskPool)
        {
            llassert(0);
            LL_WARNS() << "Ignoring duplicate alpha mask pool." << LL_ENDL;
            break;
        }
        else
        {
            mFullbrightAlphaMaskPool = (LLRenderPass*) new_poolp;
        }
        break;

    case LLDrawPool::POOL_GRASS:
        if (mGrassPool)
        {
            llassert(0);
            LL_WARNS() << "Ignoring duplicate grass pool." << LL_ENDL;
        }
        else
        {
            mGrassPool = (LLRenderPass*) new_poolp;
        }
        break;

    case LLDrawPool::POOL_FULLBRIGHT:
        if (mFullbrightPool)
        {
            llassert(0);
            LL_WARNS() << "Ignoring duplicate simple pool." << LL_ENDL;
        }
        else
        {
            mFullbrightPool = (LLRenderPass*) new_poolp;
        }
        break;

    case LLDrawPool::POOL_GLOW:
        if (mGlowPool)
        {
            llassert(0);
            LL_WARNS() << "Ignoring duplicate glow pool." << LL_ENDL;
        }
        else
        {
            mGlowPool = (LLRenderPass*) new_poolp;
        }
        break;

    case LLDrawPool::POOL_TREE:
        mTreePools[ uintptr_t(new_poolp->getTexture()) ] = new_poolp ;
        break;

    case LLDrawPool::POOL_TERRAIN:
        mTerrainPools[ uintptr_t(new_poolp->getTexture()) ] = new_poolp ;
        break;

    case LLDrawPool::POOL_BUMP:
        if (mBumpPool)
        {
            llassert(0);
            LL_WARNS() << "Ignoring duplicate bump pool." << LL_ENDL;
        }
        else
        {
            mBumpPool = new_poolp;
        }
        break;
    case LLDrawPool::POOL_MATERIALS:
        if (mMaterialsPool)
        {
            llassert(0);
            LL_WARNS() << "Ignorning duplicate materials pool." << LL_ENDL;
        }
        else
        {
            mMaterialsPool = new_poolp;
        }
        break;
    case LLDrawPool::POOL_ALPHA_PRE_WATER:
        if( mAlphaPoolPreWater )
        {
            llassert(0);
            LL_WARNS() << "LLPipeline::addPool(): Ignoring duplicate Alpha pre-water pool" << LL_ENDL;
        }
        else
        {
            mAlphaPoolPreWater = new_poolp;
        }
        break;
    case LLDrawPool::POOL_ALPHA_POST_WATER:
        if (mAlphaPoolPostWater)
        {
            llassert(0);
            LL_WARNS() << "LLPipeline::addPool(): Ignoring duplicate Alpha post-water pool" << LL_ENDL;
        }
        else
        {
            mAlphaPoolPostWater = new_poolp;
        }
        break;

    case LLDrawPool::POOL_AVATAR:
    case LLDrawPool::POOL_CONTROL_AV:
        break; // Do nothing

    case LLDrawPool::POOL_SKY:
        if( mSkyPool )
        {
            llassert(0);
            LL_WARNS() << "LLPipeline::addPool(): Ignoring duplicate Sky pool" << LL_ENDL;
        }
        else
        {
            mSkyPool = new_poolp;
        }
        break;

    case LLDrawPool::POOL_WATER:
        if( mWaterPool )
        {
            llassert(0);
            LL_WARNS() << "LLPipeline::addPool(): Ignoring duplicate Water pool" << LL_ENDL;
        }
        else
        {
            mWaterPool = new_poolp;
        }
        break;

    case LLDrawPool::POOL_WL_SKY:
        if( mWLSkyPool )
        {
            llassert(0);
            LL_WARNS() << "LLPipeline::addPool(): Ignoring duplicate WLSky Pool" << LL_ENDL;
        }
        else
        {
            mWLSkyPool = new_poolp;
        }
        break;

    case LLDrawPool::POOL_GLTF_PBR:
        if( mPBROpaquePool )
        {
            llassert(0);
            LL_WARNS() << "LLPipeline::addPool(): Ignoring duplicate PBR Opaque Pool" << LL_ENDL;
        }
        else
        {
            mPBROpaquePool = new_poolp;
        }
        break;

    case LLDrawPool::POOL_GLTF_PBR_ALPHA_MASK:
        if (mPBRAlphaMaskPool)
        {
            llassert(0);
            LL_WARNS() << "LLPipeline::addPool(): Ignoring duplicate PBR Alpha Mask Pool" << LL_ENDL;
        }
        else
        {
            mPBRAlphaMaskPool = new_poolp;
        }
        break;

    case LLDrawPool::POOL_WATEREXCLUSION:
        if (mWaterExclusionPool)
        {
            llassert(0);
            LL_WARNS() << "LLPipeline::addPool(): Ignoring duplicate Water Exclusion Pool" << LL_ENDL;
        }
        else
        {
            mWaterExclusionPool = new_poolp;
        }
        break;

    default:
        llassert(0);
        LL_WARNS() << "Invalid Pool Type in  LLPipeline::addPool()" << LL_ENDL;
        break;
    }
}

void LLPipeline::removePool( LLDrawPool* poolp )
{
    removeFromQuickLookup(poolp);
    mPools.erase(poolp);
    delete poolp;
}

void LLPipeline::removeFromQuickLookup( LLDrawPool* poolp )
{
    switch( poolp->getType() )
    {
    case LLDrawPool::POOL_SIMPLE:
        llassert(mSimplePool == poolp);
        mSimplePool = NULL;
        break;

    case LLDrawPool::POOL_ALPHA_MASK:
        llassert(mAlphaMaskPool == poolp);
        mAlphaMaskPool = NULL;
        break;

    case LLDrawPool::POOL_FULLBRIGHT_ALPHA_MASK:
        llassert(mFullbrightAlphaMaskPool == poolp);
        mFullbrightAlphaMaskPool = NULL;
        break;

    case LLDrawPool::POOL_GRASS:
        llassert(mGrassPool == poolp);
        mGrassPool = NULL;
        break;

    case LLDrawPool::POOL_FULLBRIGHT:
        llassert(mFullbrightPool == poolp);
        mFullbrightPool = NULL;
        break;

    case LLDrawPool::POOL_WL_SKY:
        llassert(mWLSkyPool == poolp);
        mWLSkyPool = NULL;
        break;

    case LLDrawPool::POOL_GLOW:
        llassert(mGlowPool == poolp);
        mGlowPool = NULL;
        break;

    case LLDrawPool::POOL_TREE:
        #ifdef _DEBUG
            {
                bool found = mTreePools.erase( (uintptr_t)poolp->getTexture() );
                llassert( found );
            }
        #else
            mTreePools.erase( (uintptr_t)poolp->getTexture() );
        #endif
        break;

    case LLDrawPool::POOL_TERRAIN:
        #ifdef _DEBUG
            {
                bool found = mTerrainPools.erase( (uintptr_t)poolp->getTexture() );
                llassert( found );
            }
        #else
            mTerrainPools.erase( (uintptr_t)poolp->getTexture() );
        #endif
        break;

    case LLDrawPool::POOL_BUMP:
        llassert( poolp == mBumpPool );
        mBumpPool = NULL;
        break;

    case LLDrawPool::POOL_MATERIALS:
        llassert(poolp == mMaterialsPool);
        mMaterialsPool = NULL;
        break;

    case LLDrawPool::POOL_ALPHA_PRE_WATER:
        llassert( poolp == mAlphaPoolPreWater );
        mAlphaPoolPreWater = nullptr;
        break;

    case LLDrawPool::POOL_ALPHA_POST_WATER:
        llassert(poolp == mAlphaPoolPostWater);
        mAlphaPoolPostWater = nullptr;
        break;

    case LLDrawPool::POOL_AVATAR:
    case LLDrawPool::POOL_CONTROL_AV:
        break; // Do nothing

    case LLDrawPool::POOL_SKY:
        llassert( poolp == mSkyPool );
        mSkyPool = NULL;
        break;

    case LLDrawPool::POOL_WATER:
        llassert( poolp == mWaterPool );
        mWaterPool = NULL;
        break;

    case LLDrawPool::POOL_GLTF_PBR:
        llassert( poolp == mPBROpaquePool );
        mPBROpaquePool = NULL;
        break;

    case LLDrawPool::POOL_GLTF_PBR_ALPHA_MASK:
        llassert(poolp == mPBRAlphaMaskPool);
        mPBRAlphaMaskPool = NULL;
        break;

    case LLDrawPool::POOL_WATEREXCLUSION:
        llassert(poolp == mWaterExclusionPool);
        mWaterExclusionPool = nullptr;
        break;

    default:
        llassert(0);
        LL_WARNS() << "Invalid Pool Type in  LLPipeline::removeFromQuickLookup() type=" << poolp->getType() << LL_ENDL;
        break;
    }
}



void LLPipeline::setLight(LLDrawable *drawablep, bool is_light)
{
    // mLights is a plain CPU set; the Vulkan world gathers its local lights from it.
    if (drawablep)
    {
        if (is_light)
        {
            if (mLights.insert(drawablep).second) ++mLightsGen;
            drawablep->setState(LLDrawable::LIGHT);
        }
        else
        {
            drawablep->clearState(LLDrawable::LIGHT);
            if (mLights.erase(drawablep)) ++mLightsGen;
        }
    }
}

//static
void LLPipeline::toggleRenderType(U32 type)
{
    gPipeline.mRenderTypeEnabled[type] = !gPipeline.mRenderTypeEnabled[type];
    if (type == LLPipeline::RENDER_TYPE_WATER)
    {
        gPipeline.mRenderTypeEnabled[LLPipeline::RENDER_TYPE_VOIDWATER] = !gPipeline.mRenderTypeEnabled[LLPipeline::RENDER_TYPE_VOIDWATER];
    }
}

//static
void LLPipeline::toggleRenderTypeControl(U32 type)
{
    gPipeline.toggleRenderType(type);
}

//static
bool LLPipeline::hasRenderTypeControl(U32 type)
{
    return gPipeline.hasRenderType(type);
}

// Allows UI items labeled "Hide foo" instead of "Show foo"
//static
bool LLPipeline::toggleRenderTypeControlNegated(S32 type)
{
    return !gPipeline.hasRenderType(type);
}

//static
void LLPipeline::toggleRenderDebug(U64 bit)
{
    if (gPipeline.hasRenderDebugMask(bit))
    {
        LL_INFOS() << "Toggling render debug mask " << std::hex << bit << " off" << std::dec << LL_ENDL;
    }
    else
    {
        LL_INFOS() << "Toggling render debug mask " << std::hex << bit << " on" << std::dec << LL_ENDL;
    }
    gPipeline.mRenderDebugMask ^= bit;
}


//static
bool LLPipeline::toggleRenderDebugControl(U64 bit)
{
    return gPipeline.hasRenderDebugMask(bit);
}

//static
void LLPipeline::toggleRenderDebugFeature(U32 bit)
{
    gPipeline.mRenderDebugFeatureMask ^= bit;
}


//static
bool LLPipeline::toggleRenderDebugFeatureControl(U32 bit)
{
    return gPipeline.hasRenderDebugFeatureMask(bit);
}

void LLPipeline::setRenderDebugFeatureControl(U32 bit, bool value)
{
    if (value)
    {
        gPipeline.mRenderDebugFeatureMask |= bit;
    }
    else
    {
        gPipeline.mRenderDebugFeatureMask &= !bit;
    }
}

// ---------------------------------------------------------------------------------------
// RENDER METADATA OVERLAYS under Vulkan (#29). LL drew these from LLPipeline::renderDebug
// over the spatial partition octree, both deleted (GL removal / #480). Same output, from
// the object list, in agent space through the facade's world-overlay segment (gGL lines
// and triangles against the world depth). Debug-only CPU immediate mode, bounded: objects
// in the camera frustum within the draw distance, nearest first, and a vertex budget per
// frame (RenderVKDebugOverlayMaxVerts).
// ---------------------------------------------------------------------------------------
namespace
{
// Unit-space (volume) point -> agent space: the object's own pose (object state, as the
// clean ingest composes it) -- LL's getRelativeXform plus the region translation.
struct VKObjXform
{
    LLVector3 pos, scale;
    LLQuaternion rot;
    explicit VKObjXform(LLViewerObject* o)
        : pos(o->getPositionAgent()), scale(o->getScale()), rot(o->getRotationRegion()) {}
    LLVector3 operator()(const LLVector3& p) const
    {
        return pos + LLVector3(p.mV[0] * scale.mV[0], p.mV[1] * scale.mV[1], p.mV[2] * scale.mV[2]) * rot;
    }
};

// LL get_physics_detail (llspatialpartition.cpp)
S32 vkPhysicsDetail(const LLVector3& scale)
{
    S32 detail = 1;
    const F32 avg = (scale.mV[0] + scale.mV[1] + scale.mV[2]) / 3.f;
    if (avg > 5.f) { ++detail; if (avg > 25.f) ++detail; }
    return detail;
}

// Triangle sink: filled (translucent) or edges. Counts vertices against the budget.
struct VKTriSink
{
    bool edges = false;
    U32 budget = 0, used = 0;
    bool full() const { return used >= budget; }
    void tri(const LLVector3& a, const LLVector3& b, const LLVector3& c)
    {
        if (edges)
        {
            gGL.vertex3fv(a.mV); gGL.vertex3fv(b.mV);
            gGL.vertex3fv(b.mV); gGL.vertex3fv(c.mV);
            gGL.vertex3fv(c.mV); gGL.vertex3fv(a.mV);
            used += 6;
        }
        else
        {
            gGL.vertex3fv(a.mV); gGL.vertex3fv(b.mV); gGL.vertex3fv(c.mV);
            used += 3;
        }
    }
};

void vkPushVolume(VKTriSink& sink, const VKObjXform& x, const LLVolume* vol)
{
    for (S32 f = 0; vol && f < vol->getNumVolumeFaces() && !sink.full(); ++f)
    {
        const LLVolumeFace& face = vol->getVolumeFace(f);
        for (S32 i = 0; i + 2 < face.mNumIndices && !sink.full(); i += 3)
        {
            sink.tri(x(LLVector3(face.mPositions[face.mIndices[i]].getF32ptr())),
                     x(LLVector3(face.mPositions[face.mIndices[i + 1]].getF32ptr())),
                     x(LLVector3(face.mPositions[face.mIndices[i + 2]].getF32ptr())));
        }
    }
}

void vkPushPositions(VKTriSink& sink, const VKObjXform& x, const std::vector<LLVector3>& pos)
{
    for (size_t i = 0; i + 2 < pos.size() && !sink.full(); i += 3)
        sink.tri(x(pos[i]), x(pos[i + 1]), x(pos[i + 2]));
}

// Axis-aligned box [c - r, c + r] in unit space (LL drawBox / drawBoxOutline).
void vkPushBox(VKTriSink& sink, const VKObjXform& x, const LLVector3& c, const LLVector3& r)
{
    LLVector3 v[8];
    for (S32 i = 0; i < 8; ++i)
        v[i] = x(LLVector3(c.mV[0] + ((i & 1) ? r.mV[0] : -r.mV[0]),
                           c.mV[1] + ((i & 2) ? r.mV[1] : -r.mV[1]),
                           c.mV[2] + ((i & 4) ? r.mV[2] : -r.mV[2])));
    static const U8 q[6][4] = { {0,1,3,2}, {4,6,7,5}, {0,4,5,1}, {2,3,7,6}, {0,2,6,4}, {1,5,7,3} };
    for (const auto& f : q)
    {
        sink.tri(v[f[0]], v[f[1]], v[f[2]]);
        sink.tri(v[f[0]], v[f[2]], v[f[3]]);
    }
}

// LL renderPhysicsShape's inline prim hull (gMeshRepo.buildHull is an empty stub): the
// convex hull of the physics-LOD volume through the convex decomposition library, cached
// on the shared LLVolume (mHullPoints / mHullIndices, freed with the volume). Without a
// functional library nothing is cached and the caller draws LL's magenta box.
void vkBuildPrimHull(LLVolume* phys)
{
    if (!LLConvexDecomposition::isFunctional() || !LLConvexDecomposition::getInstance()) return;
    std::vector<LLVector3> pos;
    std::vector<U16> index;
    S32 index_offset = 0;
    for (S32 i = 0; i < phys->getNumVolumeFaces(); ++i)
    {
        const LLVolumeFace& face = phys->getVolumeFace(i);
        if (index_offset + face.mNumVertices > 65535) continue;
        for (S32 j = 0; j < face.mNumVertices; ++j) pos.emplace_back(face.mPositions[j].getF32ptr());
        for (S32 j = 0; j < face.mNumIndices; ++j) index.push_back((U16)(face.mIndices[j] + index_offset));
        index_offset += face.mNumVertices;
    }
    if (pos.empty() || index.empty()) return;
    LLCDMeshData mesh;
    mesh.mIndexBase = &index[0];
    mesh.mVertexBase = pos[0].mV;
    mesh.mNumVertices = (int)pos.size();
    mesh.mVertexStrideBytes = 12;
    mesh.mIndexStrideBytes = 6;
    mesh.mIndexType = LLCDMeshData::INT_16;
    mesh.mNumTriangles = (int)index.size() / 3;
    LLCDMeshData res;
    if (LLConvexDecomposition::getInstance()->generateSingleHullMeshFromMesh(&mesh, &res) != LLCD_OK
        || res.mNumVertices <= 0 || res.mNumTriangles <= 0)
        return;
    phys->mHullPoints = (LLVector4a*)ll_aligned_malloc_16(sizeof(LLVector4a) * res.mNumVertices);
    phys->mNumHullPoints = res.mNumVertices;
    const S32 idx_size = (res.mNumTriangles * 3 * 2 + 0xF) & ~0xF;
    phys->mHullIndices = (U16*)ll_aligned_malloc_16(idx_size);
    phys->mNumHullIndices = res.mNumTriangles * 3;
    for (S32 i = 0; i < res.mNumVertices; ++i)
        phys->mHullPoints[i].load3((const F32*)((const U8*)res.mVertexBase + i * res.mVertexStrideBytes));
    for (S32 i = 0; i < res.mNumTriangles; ++i)
    {
        const U8* t = (const U8*)res.mIndexBase + i * res.mIndexStrideBytes;
        for (S32 k = 0; k < 3; ++k)
            phys->mHullIndices[i * 3 + k] = res.mIndexType == LLCDMeshData::INT_16 ? ((const U16*)t)[k]
                                                                                   : (U16)((const U32*)t)[k];
    }
}

// LL renderPhysicsShape (llspatialpartition.cpp), one pass (filled or edges).
void vkPhysicsShape(VKTriSink& sink, LLVOVolume* volume, bool wireframe)
{
    const U8 physics_type = volume->getPhysicsShapeType();
    if (physics_type == LLViewerObject::PHYSICS_SHAPE_NONE || volume->isFlexible() || !volume->getVolume())
        return;

    // Colour by object cost (LL's ObjectCost{Low,Mid,High}Color / ObjectCostHighThreshold
    // defaults; those settings went with the GL pipeline).
    static const LLColor4 low(0.f, 0.5f, 1.f, 0.5f), mid(1.f, 0.75f, 0.f, 0.65f), high(1.f, 0.f, 0.f, 0.75f);
    const F32 n = 1.f - expf(-(volume->getObjectCost() / 50.f));
    LLColor4 color = n <= 0.5f ? lerp(low, mid, 2.f * n) : lerp(mid, high, 2.f * (n - 0.5f));
    if (wireframe) color = color * 0.5f;
    gGL.color4fv(color.mV);

    const VKObjXform x(volume);
    const LLVolumeParams volume_params = volume->getVolume()->getParams();
    LLPhysicsVolumeParams physics_params(volume_params, physics_type == LLViewerObject::PHYSICS_SHAPE_CONVEX_HULL);
    LLPhysicsShapeBuilderUtil::PhysicsShapeSpecification spec;
    LLPhysicsShapeBuilderUtil::determinePhysicsShape(physics_params, volume->getScale(), spec);
    using Spec = LLPhysicsShapeBuilderUtil::PhysicsShapeSpecification;
    const U32 type = spec.getType();
    const LLVector3 center(0.f, 0.f, 0.f), size(0.25f, 0.25f, 0.25f);

    auto baseHull = [&](LLModel::Decomposition* decomp)
    {
        if (decomp && !decomp->mBaseHullMesh.empty())
            vkPushPositions(sink, x, decomp->mBaseHullMesh.mPositions);
        else
        {
            if (decomp) gMeshRepo.buildPhysicsMesh(*decomp);
            vkPushBox(sink, x, center, size);   // LL: a placeholder box until it exists
        }
    };

    if (type == Spec::USER_MESH)
    {
        const LLUUID mesh_id = volume_params.getSculptID();
        LLModel::Decomposition* decomp = gMeshRepo.getDecomposition(mesh_id);
        if (!decomp)
            vkPushBox(sink, x, center, size);
        else if (!decomp->mHull.empty())
        {
            if (decomp->mMesh.empty()) gMeshRepo.buildPhysicsMesh(*decomp);
            for (const LLModel::PhysicsMesh& m : decomp->mMesh) vkPushPositions(sink, x, m.mPositions);
        }
        else if (!decomp->mPhysicsShapeMesh.empty())
            vkPushPositions(sink, x, decomp->mPhysicsShapeMesh.mPositions);
        else
        {
            baseHull(decomp);
            gMeshRepo.fetchPhysicsShape(mesh_id);
        }
    }
    else if (type == Spec::USER_CONVEX || type == Spec::PRIM_CONVEX)
    {
        if (volume->isMesh())
            baseHull(gMeshRepo.getDecomposition(volume_params.getSculptID()));
        else
        {
            const S32 detail = vkPhysicsDetail(volume->getScale());
            LLVolume* phys = LLPrimitive::sVolumeManager->refVolume(volume_params, detail);
            if (phys && !phys->mHullPoints) vkBuildPrimHull(phys);
            if (phys && phys->mHullPoints && phys->mHullIndices)
            {
                for (S32 i = 0; i + 2 < phys->mNumHullIndices && !sink.full(); i += 3)
                    sink.tri(x(LLVector3(phys->mHullPoints[phys->mHullIndices[i]].getF32ptr())),
                             x(LLVector3(phys->mHullPoints[phys->mHullIndices[i + 1]].getF32ptr())),
                             x(LLVector3(phys->mHullPoints[phys->mHullIndices[i + 2]].getF32ptr())));
            }
            else
            {
                gGL.color4f(1.f, 0.f, 1.f, wireframe ? 0.5f : 1.f);   // LL: magenta box, no hull
                vkPushBox(sink, x, center, size);
            }
            if (phys) LLPrimitive::sVolumeManager->unrefVolume(phys);
        }
    }
    else if (type == Spec::BOX)
    {
        if (!wireframe)
        {
            const LLVector3 vscale = volume->getScale() * 2.f;
            const LLVector3 sc = spec.getScale();
            vkPushBox(sink, x, spec.getCenter(),
                      LLVector3(sc.mV[0] / vscale.mV[0], sc.mV[1] / vscale.mV[1], sc.mV[2] / vscale.mV[2]));
        }
    }
    else if (type == Spec::SPHERE || type == Spec::CYLINDER)
    {
        if (!wireframe)
        {
            LLVolumeParams vp;
            if (type == Spec::SPHERE) vp.setType(LL_PCODE_PROFILE_CIRCLE_HALF, LL_PCODE_PATH_CIRCLE);
            else                      vp.setType(LL_PCODE_PROFILE_CIRCLE, LL_PCODE_PATH_LINE);
            vp.setBeginAndEndS(0.f, 1.f);
            vp.setBeginAndEndT(0.f, 1.f);
            vp.setRatio(1, 1);
            vp.setShear(0, 0);
            LLVolume* shape = LLPrimitive::sVolumeManager->refVolume(vp, 3);
            vkPushVolume(sink, x, shape);
            if (shape) LLPrimitive::sVolumeManager->unrefVolume(shape);
        }
    }
    else if (type == Spec::PRIM_MESH)
    {
        LLVolume* phys = LLPrimitive::sVolumeManager->refVolume(volume_params, vkPhysicsDetail(volume->getScale()));
        vkPushVolume(sink, x, phys);
        if (phys) LLPrimitive::sVolumeManager->unrefVolume(phys);
    }
    // SCULPT: LL had no display for it either (TODO in renderPhysicsShape).
}

void vkBoxOutline(const LLVector3& mn, const LLVector3& mx)
{
    LLVector3 v[8];
    for (S32 i = 0; i < 8; ++i)
        v[i].set((i & 1) ? mx.mV[0] : mn.mV[0], (i & 2) ? mx.mV[1] : mn.mV[1], (i & 4) ? mx.mV[2] : mn.mV[2]);
    static const U8 e[12][2] = { {0,1},{2,3},{4,5},{6,7}, {0,2},{1,3},{4,6},{5,7}, {0,4},{1,5},{2,6},{3,7} };
    for (const auto& k : e) { gGL.vertex3fv(v[k[0]].mV); gGL.vertex3fv(v[k[1]].mV); }
}
} // namespace

// static
void LLPipeline::renderDebugOverlaysVK()
{
    const bool phys = gPipeline.hasRenderDebugMask(RENDER_DEBUG_PHYSICS_SHAPES);
    const bool bbox = gPipeline.hasRenderDebugMask(RENDER_DEBUG_BBOXES);
    const bool ray = gPipeline.hasRenderDebugMask(RENDER_DEBUG_RAYCAST);
    if (!phys && !bbox && !ray) return;

    static LLCachedControl<U32> s_budget(gSavedSettings, "RenderVKDebugOverlayMaxVerts", 250000);
    LLViewerCamera* cam = LLViewerCamera::getInstance();
    const LLVector3 eye = cam->getOrigin();
    const F32 far_clip = cam->getFar();
    // Candidates: in-world volumes (LL skipped attachments) in the camera frustum within
    // the draw distance, nearest first so the budget keeps what is close.
    std::vector<std::pair<F32, LLVOVolume*>> objs;
    const S32 count = gObjectList.getNumObjects();
    for (S32 i = 0; i < count; ++i)
    {
        LLViewerObject* o = gObjectList.getObject(i);
        if (!o || o->isDead() || o->getPCode() != LL_PCODE_VOLUME || o->isAttachment() || o->isHUDAttachment())
            continue;
        const LLVector3 p = o->getPositionAgent();
        const F32 r = o->getScale().length() * 0.5f;
        const F32 d = dist_vec(p, eye);
        if (d - r > far_clip || !cam->sphereInFrustum(p, r)) continue;
        objs.emplace_back(d, (LLVOVolume*)o);
    }
    std::sort(objs.begin(), objs.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

    LLRender::UIScope gls_ui;
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

    if (bbox)
    {
        // LL renderBoundingBox colours: root prim yellow (a mover -- LL's spatial bridge --
        // orange), child prim green; the box is the object's world AABB (LL: the
        // drawable's spatial extents).
        LLRender::DepthScope depth(true, false);
        gGL.begin(LLRender::LINES);
        U32 used = 0;
        for (const auto& it : objs)
        {
            if (used >= s_budget) break;
            LLVOVolume* v = it.second;
            if (v->isRoot()) gGL.color4f(1.f, v->isActive() ? 0.5f : 1.f, 0.f, 1.f);
            else             gGL.color4f(0.f, 1.f, 0.f, 1.f);
            const VKObjXform x(v);
            LLVector3 mn(F32_MAX, F32_MAX, F32_MAX), mx(-F32_MAX, -F32_MAX, -F32_MAX);
            for (S32 c = 0; c < 8; ++c)
            {
                const LLVector3 p = x(LLVector3((c & 1) ? 0.5f : -0.5f, (c & 2) ? 0.5f : -0.5f, (c & 4) ? 0.5f : -0.5f));
                mn.set(llmin(mn.mV[0], p.mV[0]), llmin(mn.mV[1], p.mV[1]), llmin(mn.mV[2], p.mV[2]));
                mx.set(llmax(mx.mV[0], p.mV[0]), llmax(mx.mV[1], p.mV[1]), llmax(mx.mV[2], p.mV[2]));
            }
            vkBoxOutline(mn, mx);
            used += 24;
        }
        gGL.end();
    }

    if (phys)
    {
        // LL renderDebug: the shapes filled (translucent), then their edges at half colour.
        for (int pass = 0; pass < 2; ++pass)
        {
            VKTriSink sink;
            sink.edges = pass == 1;
            sink.budget = s_budget;
            LLRender::DepthScope depth(true, false);
            gGL.setSceneBlendType(LLRender::BT_ALPHA);
            gGL.begin(sink.edges ? LLRender::LINES : LLRender::TRIANGLES);
            for (const auto& it : objs)
            {
                if (sink.full()) break;
                vkPhysicsShape(sink, it.second, sink.edges);
            }
            gGL.end();
        }
    }

    if (ray && gDebugRaycastObject)
    {
        // LL's Raycast overlay (#684): the pick ray (grey), the hit point, and the surface
        // normal (blue) / tangent (red) there.
        LLRender::DepthScope depth(false, false);
        const LLVector3 hit(gDebugRaycastIntersection.getF32ptr());
        const LLVector3 start(gDebugRaycastStart.getF32ptr());
        const LLVector3 n(gDebugRaycastNormal.getF32ptr());
        const LLVector3 t(gDebugRaycastTangent.getF32ptr());
        const F32 len = llclamp(dist_vec(hit, eye) * 0.05f, 0.1f, 2.f);
        const F32 c = len * 0.15f;
        gGL.begin(LLRender::LINES);
        gGL.color4f(0.6f, 0.6f, 0.6f, 0.8f);
        gGL.vertex3fv(start.mV); gGL.vertex3fv(hit.mV);
        gGL.color4f(1.f, 1.f, 0.f, 1.f);
        gGL.vertex3f(hit.mV[0] - c, hit.mV[1], hit.mV[2]); gGL.vertex3f(hit.mV[0] + c, hit.mV[1], hit.mV[2]);
        gGL.vertex3f(hit.mV[0], hit.mV[1] - c, hit.mV[2]); gGL.vertex3f(hit.mV[0], hit.mV[1] + c, hit.mV[2]);
        gGL.vertex3f(hit.mV[0], hit.mV[1], hit.mV[2] - c); gGL.vertex3f(hit.mV[0], hit.mV[1], hit.mV[2] + c);
        const LLVector3 nt = hit + n * len;
        gGL.color4f(0.2f, 0.4f, 1.f, 1.f);
        gGL.vertex3fv(hit.mV); gGL.vertex3fv(nt.mV);
        const LLVector3 tt = hit + t * len;
        gGL.color4f(1.f, 0.2f, 0.2f, 1.f);
        gGL.vertex3fv(hit.mV); gGL.vertex3fv(tt.mV);
        gGL.end();
    }
    gGL.flush();
}

// static
void LLPipeline::addDebugBeacons()
{
    // LL gate (was postSort): only while the Beacons floater is open. sRenderBeacons
    // picks beams; sRenderHighlight (face highlights) has no VK path yet.
    if (sRenderBeacons && LLFloaterReg::instanceVisible("beacons"))
    {
        const S32 width = gSavedSettings.getS32("DebugBeaconLineWidth");
        const LLColor4 text_color(1.f, 1.f, 1.f, 0.5f);
        const bool scripted_touch = sRenderScriptedTouchBeacons;
        const bool scripted = !scripted_touch && sRenderScriptedBeacons;
        if (scripted_touch || scripted || sRenderPhysicalBeacons || sRenderMOAPBeacons || sRenderParticleBeacons)
        {
            // LL walked the culled visible drawables; the VK renderer does its own
            // culling, so walk the object list (beams are tall: an off-screen object
            // can still have an on-screen beam).
            const S32 count = gObjectList.getNumObjects();
            for (S32 i = 0; i < count; ++i)
            {
                LLViewerObject* vobj = gObjectList.getObject(i);
                if (!vobj || vobj->isDead() || vobj->mDrawable.isNull())
                {
                    continue;
                }
                const bool avatar = vobj->isAvatar();
                if (!avatar && !vobj->getParent() && vobj->flagScripted()
                    && (scripted || (scripted_touch && vobj->flagHandleTouch())))
                {
                    // Root objects only.
                    gObjectList.addDebugBeacon(vobj, "", LLColor4(1.f, 0.f, 0.f, 0.5f), text_color, width);
                }
                if (sRenderPhysicalBeacons && !avatar && vobj->flagUsePhysics())
                {
                    gObjectList.addDebugBeacon(vobj, "", LLColor4(0.f, 1.f, 0.f, 0.5f), text_color, width);
                }
                if (sRenderMOAPBeacons && !avatar)
                {
                    const U8 te_count = vobj->getNumTEs();
                    for (U8 te = 0; te < te_count; ++te)
                    {
                        const LLTextureEntry* tep = vobj->getTE(te);
                        if (tep && tep->hasMedia())
                        {
                            gObjectList.addDebugBeacon(vobj, "", LLColor4(1.f, 1.f, 1.f, 0.5f), text_color, width);
                            break;
                        }
                    }
                }
                if (sRenderParticleBeacons && vobj->isParticleSource())
                {
                    gObjectList.addDebugBeacon(vobj, "", LLColor4(0.5f, 0.5f, 1.f, 0.5f), text_color, width);
                }
            }
        }

        if (sRenderSoundBeacons && gAudiop)
        {
            // All sound sources, visible or not (as LL). Object sounds track their
            // object; the rest use the source position.
            for (const auto& entry : gAudiop->mAllSources)
            {
                const LLAudioSource* sourcep = entry.second;
                if (!sourcep)
                {
                    continue;
                }
                const LLColor4 sound_color(1.f, 1.f, 0.f, 0.5f);
                if (LLViewerObject* objectp = gObjectList.findObject(sourcep->getID()))
                {
                    gObjectList.addDebugBeacon(objectp, "", sound_color, text_color, width);
                }
                else
                {
                    gObjectList.addDebugBeacon(gAgent.getPosAgentFromGlobal(sourcep->getPositionGlobal()), "",
                                               sound_color, text_color, width);
                }
            }
        }
    }

    // Managing a telehub: beacons at the telehub and the selected spawn point.
    if (LLFloaterTelehub::renderBeacons())
    {
        LLFloaterTelehub::addBeacons();
    }
}

// static
void LLPipeline::setRenderScriptedBeacons(bool val)
{
    sRenderScriptedBeacons = val;
}

// static
void LLPipeline::toggleRenderScriptedBeacons()
{
    sRenderScriptedBeacons = !sRenderScriptedBeacons;
}

// static
bool LLPipeline::getRenderScriptedBeacons()
{
    return sRenderScriptedBeacons;
}

// static
void LLPipeline::setRenderScriptedTouchBeacons(bool val)
{
    sRenderScriptedTouchBeacons = val;
}

// static
void LLPipeline::toggleRenderScriptedTouchBeacons()
{
    sRenderScriptedTouchBeacons = !sRenderScriptedTouchBeacons;
}

// static
bool LLPipeline::getRenderScriptedTouchBeacons()
{
    return sRenderScriptedTouchBeacons;
}

// static
void LLPipeline::setRenderMOAPBeacons(bool val)
{
    sRenderMOAPBeacons = val;
}

// static
void LLPipeline::toggleRenderMOAPBeacons()
{
    sRenderMOAPBeacons = !sRenderMOAPBeacons;
}

// static
bool LLPipeline::getRenderMOAPBeacons()
{
    return sRenderMOAPBeacons;
}

// static
void LLPipeline::setRenderPhysicalBeacons(bool val)
{
    sRenderPhysicalBeacons = val;
}

// static
void LLPipeline::toggleRenderPhysicalBeacons()
{
    sRenderPhysicalBeacons = !sRenderPhysicalBeacons;
}

// static
bool LLPipeline::getRenderPhysicalBeacons()
{
    return sRenderPhysicalBeacons;
}

// static
void LLPipeline::setRenderParticleBeacons(bool val)
{
    sRenderParticleBeacons = val;
}

// static
void LLPipeline::toggleRenderParticleBeacons()
{
    sRenderParticleBeacons = !sRenderParticleBeacons;
}

// static
bool LLPipeline::getRenderParticleBeacons()
{
    return sRenderParticleBeacons;
}

// static
void LLPipeline::setRenderSoundBeacons(bool val)
{
    sRenderSoundBeacons = val;
}

// static
void LLPipeline::toggleRenderSoundBeacons()
{
    sRenderSoundBeacons = !sRenderSoundBeacons;
}

// static
bool LLPipeline::getRenderSoundBeacons()
{
    return sRenderSoundBeacons;
}

// static
void LLPipeline::setRenderBeacons(bool val)
{
    sRenderBeacons = val;
}

// static
void LLPipeline::toggleRenderBeacons()
{
    sRenderBeacons = !sRenderBeacons;
}

// static
bool LLPipeline::getRenderBeacons()
{
    return sRenderBeacons;
}

// static
void LLPipeline::setRenderHighlights(bool val)
{
    sRenderHighlight = val;
}

// static
void LLPipeline::toggleRenderHighlights()
{
    sRenderHighlight = !sRenderHighlight;
}

// static
bool LLPipeline::getRenderHighlights()
{
    return sRenderHighlight;
}

// static
void LLPipeline::setRenderHighlightTextureChannel(LLRender::eTexIndex channel)
{
    if (channel != sRenderHighlightTextureChannel)
    {
        sRenderHighlightTextureChannel = channel;
    }
}

bool LLPipeline::lineSegmentIntersectParticle(const LLVector4a& start, const LLVector4a& end, bool sync,
                                              LLVector4a* intersection, LLUUID* owner_id, LLUUID* source_id)
{
    // The particles live only in the GPU pool (the PARTITION_PARTICLE octree is empty):
    // the renderer picks them against what it drew (#481).
    if (!hasRenderType(RENDER_TYPE_PARTICLES)) return false;
    LLCleanPartPick::Hit h;
    if (!LLCleanPartPick::intersect(start, end, sync, h)) return false;
    if (intersection) *intersection = h.pos;
    if (owner_id) *owner_id = h.owner;
    if (source_id) *source_id = h.source;
    return true;
}

bool LLPipeline::hasRenderType(const U32 type) const
{
    // STORM-365 : a render type of 0 means "do not render"
    // TODO: reintroduce RENDER_TYPE_NONE in LLRenderTypeMask and initialize its mRenderTypeEnabled[RENDER_TYPE_NONE] to false explicitely
    return (type == 0 ? false : mRenderTypeEnabled[type]);
}

void LLPipeline::setRenderTypeMask(U32 type, ...)
{
    va_list args;

    va_start(args, type);
    while (type < END_RENDER_TYPES)
    {
        mRenderTypeEnabled[type] = true;
        type = va_arg(args, U32);
    }
    va_end(args);

    if (type > END_RENDER_TYPES)
    {
        LL_ERRS() << "Invalid render type." << LL_ENDL;
    }
}

void LLPipeline::pushRenderTypeMask()
{
    std::string cur_mask;
    cur_mask.assign((const char*) mRenderTypeEnabled, sizeof(mRenderTypeEnabled));
    mRenderTypeEnableStack.push(cur_mask);
}

void LLPipeline::popRenderTypeMask()
{
    if (mRenderTypeEnableStack.empty())
    {
        LL_ERRS() << "Depleted render type stack." << LL_ENDL;
    }

    memcpy(mRenderTypeEnabled, mRenderTypeEnableStack.top().data(), sizeof(mRenderTypeEnabled));
    mRenderTypeEnableStack.pop();
}

void LLPipeline::andRenderTypeMask(U32 type, ...)
{
    va_list args;

    bool tmp[NUM_RENDER_TYPES];
    for (U32 i = 0; i < NUM_RENDER_TYPES; ++i)
    {
        tmp[i] = false;
    }

    va_start(args, type);
    while (type < END_RENDER_TYPES)
    {
        if (mRenderTypeEnabled[type])
        {
            tmp[type] = true;
        }

        type = va_arg(args, U32);
    }
    va_end(args);

    if (type > END_RENDER_TYPES)
    {
        LL_ERRS() << "Invalid render type." << LL_ENDL;
    }

    for (U32 i = 0; i < LLPipeline::NUM_RENDER_TYPES; ++i)
    {
        mRenderTypeEnabled[i] = tmp[i];
    }

}

void LLPipeline::clearRenderTypeMask(U32 type, ...)
{
    va_list args;

    va_start(args, type);
    while (type < END_RENDER_TYPES)
    {
        mRenderTypeEnabled[type] = false;

        type = va_arg(args, U32);
    }
    va_end(args);

    if (type > END_RENDER_TYPES)
    {
        LL_ERRS() << "Invalid render type." << LL_ENDL;
    }
}

void LLPipeline::setAllRenderTypes()
{
    for (U32 i = 0; i < NUM_RENDER_TYPES; ++i)
    {
        mRenderTypeEnabled[i] = true;
    }
}

void LLPipeline::clearAllRenderTypes()
{
    for (U32 i = 0; i < NUM_RENDER_TYPES; ++i)
    {
        mRenderTypeEnabled[i] = false;
    }
}

void LLPipeline::hidePermanentObjects( std::vector<U32>& restoreList )
{
    //This method is used to hide any vo's from the object list that may have
    //the permanent flag set.

    U32 objCnt = gObjectList.getNumObjects();
    for (U32 i = 0; i < objCnt; ++i)
    {
        LLViewerObject* pObject = gObjectList.getObject(i);
        if ( pObject && pObject->flagObjectPermanent() )
        {
            LLDrawable *pDrawable = pObject->mDrawable;

            if ( pDrawable )
            {
                restoreList.push_back( i );
                hideDrawable( pDrawable );
            }
        }
    }
}

void LLPipeline::restorePermanentObjects( const std::vector<U32>& restoreList )
{
    //This method is used to restore(unhide) any vo's from the object list that may have
    //been hidden because their permanency flag was set.

    std::vector<U32>::const_iterator itCurrent  = restoreList.begin();
    std::vector<U32>::const_iterator itEnd      = restoreList.end();

    U32 objCnt = gObjectList.getNumObjects();

    while ( itCurrent != itEnd )
    {
        U32 index = *itCurrent;
        LLViewerObject* pObject = NULL;
        if ( index < objCnt )
        {
            pObject = gObjectList.getObject( index );
        }
        if ( pObject )
        {
            LLDrawable *pDrawable = pObject->mDrawable;
            if ( pDrawable )
            {
                pDrawable->clearState( LLDrawable::FORCE_INVISIBLE );
                unhideDrawable( pDrawable );
            }
        }
        ++itCurrent;
    }
}

void LLPipeline::hideObject( const LLUUID& id )
{
    LLViewerObject *pVO = gObjectList.findObject( id );

    if ( pVO )
    {
        LLDrawable *pDrawable = pVO->mDrawable;

        if ( pDrawable )
        {
            hideDrawable( pDrawable );
        }
    }
}

void LLPipeline::hideDrawable( LLDrawable *pDrawable )
{
    pDrawable->setState( LLDrawable::FORCE_INVISIBLE );
    markRebuild( pDrawable, LLDrawable::REBUILD_ALL);
    //hide the children
    LLViewerObject::const_child_list_t& child_list = pDrawable->getVObj()->getChildren();
    for ( LLViewerObject::child_list_t::const_iterator iter = child_list.begin();
          iter != child_list.end(); iter++ )
    {
        LLViewerObject* child = *iter;
        LLDrawable* drawable = child->mDrawable;
        if ( drawable )
        {
            drawable->setState( LLDrawable::FORCE_INVISIBLE );
            markRebuild( drawable, LLDrawable::REBUILD_ALL);
        }
    }
}
void LLPipeline::unhideDrawable( LLDrawable *pDrawable )
{
    pDrawable->clearState( LLDrawable::FORCE_INVISIBLE );
    markRebuild( pDrawable, LLDrawable::REBUILD_ALL);
    //restore children
    LLViewerObject::const_child_list_t& child_list = pDrawable->getVObj()->getChildren();
    for ( LLViewerObject::child_list_t::const_iterator iter = child_list.begin();
          iter != child_list.end(); iter++)
    {
        LLViewerObject* child = *iter;
        LLDrawable* drawable = child->mDrawable;
        if ( drawable )
        {
            drawable->clearState( LLDrawable::FORCE_INVISIBLE );
            markRebuild( drawable, LLDrawable::REBUILD_ALL);
        }
    }
}
void LLPipeline::restoreHiddenObject( const LLUUID& id )
{
    LLViewerObject *pVO = gObjectList.findObject( id );

    if ( pVO )
    {
        LLDrawable *pDrawable = pVO->mDrawable;
        if ( pDrawable )
        {
            unhideDrawable( pDrawable );
        }
    }
}

void LLPipeline::rebuildTerrain()
{
    for (LLWorld::region_list_t::const_iterator iter = LLWorld::getInstance()->getRegionList().begin();
        iter != LLWorld::getInstance()->getRegionList().end(); ++iter)
    {
        LLViewerRegion* region = *iter;
        region->dirtyAllPatches();
    }
}
