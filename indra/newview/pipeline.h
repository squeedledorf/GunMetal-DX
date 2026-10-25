/**
 * @file pipeline.h
 * @brief Rendering pipeline definitions
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

#ifndef LL_PIPELINE_H
#define LL_PIPELINE_H

#include "llcamera.h"
#include "llerror.h"
#include "lldrawpool.h"
#include "m4math.h"
#include "llpointer.h"
#include "lldrawable.h"
// What llspatialpartition.h (#480: deleted) supplied to every includer of pipeline.h.
#include "llface.h"
#include "llviewercamera.h"
#include "llvoavatar.h"
#include "llfetchedgltfmaterial.h"
#include <queue>
#include <unordered_map>
#include "llgl.h"
#include "llrender.h"
#include "llheroprobemanager.h"

#include <stack>
#include <functional>
#include "slscopedata.h"

class LLViewerTexture;
class LLFace;
class LLViewerObject;
class LLTextureEntry;
class LLCullResult;
class LLVOAvatar;
class LLVOPartGroup;

bool setup_hud_matrices(); // use whole screen to render hud
bool setup_hud_matrices(const LLRect& screen_region); // specify portion of screen (in pixels) to render hud attachments from (for picking)



class LLPipeline
{
public:
    LLPipeline();
    ~LLPipeline();

    // ---- Lightbox hooks (upstream Alchemy API). The Vulkan renderer services the
    // requests (LLCleanRendererVK: recordFinal / serviceScenePixel / scope sample)
    // and writes the results back into the members below.
    // Scopes floater sample of the presented frame; only while sScopeCapture is set.
    const SLScopeData& getScopeData() const { return mScopeData; }
    /// Colour of the last scope sample under a point in scaled window coordinates.
    bool getScopePixel(S32 scaled_x, S32 scaled_y, LLColor4U& out) const;
    void releaseScopeBuffers() { mScopeReadback.clear(); mScopeW = mScopeH = 0; mScopeData.clear(); }
    /// Where a scaled window point falls inside the world view, 0..1 across and up.
    static bool worldViewUV(S32 scaled_x, S32 scaled_y, F32& u, F32& v);
    // Reference still for the wipe comparison: grabbed on the next presented frame.
    void requestReferenceStill() { mReferenceStillWanted = true; }
    void clearReferenceStill() { mReferenceStillWanted = false; mReferenceStillValid = false; }
    bool hasReferenceStill() const { return mReferenceStillValid; }
    // Linear scene colour under a scaled window pixel, delivered on a later frame.
    typedef std::function<void(const LLColor3&)> scene_pixel_cb_t;
    void requestScenePixel(S32 x, S32 y, scene_pixel_cb_t callback);
    static bool             sScopeCapture;
    static bool             sGradeBypass;
    enum EGradeBypass : U32
    {
        GRADE_BYPASS_BASIC     = 1 << 0,
        GRADE_BYPASS_PRIMARIES = 1 << 1,
        GRADE_BYPASS_SPLIT     = 1 << 2,
        GRADE_BYPASS_LUT       = 1 << 3,
        GRADE_BYPASS_CURVE     = 1 << 4,
    };
    static U32              sGradeBypassMask;
    bool                    mLensDirtSliderHeld = false;
    SLScopeData             mScopeData;
    std::vector<U8>         mScopeReadback;          // RGBA8, bottom-up rows (GL convention)
    S32                     mScopeW = 0, mScopeH = 0;
    bool                    mReferenceStillWanted = false;
    bool                    mReferenceStillValid = false;
    scene_pixel_cb_t        mScenePixelCallback;
    S32                     mScenePixelX = 0, mScenePixelY = 0;
    bool                    mScenePixelPending = false;


    // GL-residue counters (RenderVKSectionLog "GLResidue" line): sizes of legacy
    // queues/containers that nothing drains under Vulkan. Read-only snapshot.
    void getGLResidueSizes(size_t& group_q1, size_t& partition_q, size_t& probes) const;


    // Rebuild all terrain
    void rebuildTerrain();

    // GL-REMOVAL-STUB: LLPipeline::init() no longer exists (delete after merge).
    bool isInit() { return false; }
    bool mInitialized = false;

    /// @brief Get a draw pool from pool type (POOL_SIMPLE, POOL_MEDIA) and texture.
    /// @return Draw pool, or NULL if not found.
    LLDrawPool *findPool(const U32 pool_type, LLViewerTexture *tex0 = NULL);

    /// @brief Get a draw pool for faces of the appropriate type and texture.  Create if necessary.
    /// @return Always returns a draw pool.
    LLDrawPool *getPool(const U32 pool_type, LLViewerTexture *tex0 = NULL);

    static U32 getPoolTypeFromTE(const LLTextureEntry* te, LLViewerTexture* imagep);

    void         addPool(LLDrawPool *poolp);    // Only to be used by LLDrawPool classes for splitting pools!
    void         removePool( LLDrawPool* poolp );


    void         allocDrawable(LLViewerObject *obj);

    void        markMoved(LLDrawable *drawablep, bool damped_motion = false);
    void        markShift(LLDrawable *drawablep);
    void        markTextured(LLDrawable *drawablep);
    void        markGLRebuild(LLGLUpdate* glu);
    void        markRebuild(LLDrawable *drawablep, LLDrawable::EDrawableFlags flag = LLDrawable::REBUILD_ALL);
    void        markPartitionMove(LLDrawable* drawablep);

    // Object picking: the GPU raster pick (#702 / #704, llcleancursorpick.h).

    // The closest drawn particle to start between start and end (#481: GPU pick over the
    // GPU particle pool, LLCleanPartPick): its position, owner and source object.
    // sync: a click whose result is read at once (may wait for the GPU); otherwise the
    // per-frame answer (~2 frames old).
    bool lineSegmentIntersectParticle(const LLVector4a& start, const LLVector4a& end, bool sync,
                                      LLVector4a* intersection, LLUUID* owner_id, LLUUID* source_id);





    U32         addObject(LLViewerObject *obj);

    // GL-REMOVAL-STUB: GL shader state (delete after merge).
    bool        shadersLoaded() { return false; }
    bool        canUseWindLightShaders() const { return true; }


    void updateMoveDampedAsync(LLDrawable* drawablep);
    void updateMoveNormalAsync(LLDrawable* drawablep);
    void updateMovedList(LLDrawable::drawable_vector_t& move_list);
    void updateMove();

    void createObject(LLViewerObject* vobj);
    // Refresh the entry data (bin radius, extents) of drawables queued by
    // markPartitionMove (shrink-wrap, LOD bin-radius change) -- LLDrawable::movePartition.
    void processPartitionQ();
    void updateGL();
    // GL-REMOVAL-STUB: LL group rebuild queue (delete after merge).
    void clearRebuildGroups() {}

    //calculate pixel area of given box from vantage point of given camera
    static F32 calcPixelArea(LLVector3 center, LLVector3 size, LLCamera& camera);
    static F32 calcPixelArea(const LLVector4a& center, const LLVector4a& size, LLCamera &camera);

    S32  getLightCount() const { return static_cast<S32>(mLights.size()); }
    // All light-emitting drawables (LL-maintained via setLight). The Vulkan world
    // gathers its local lights from this (the VK frame skips calcNearbyLights).
    const LLDrawable::ordered_drawable_set_t& getLights() const { return mLights; }
    // Bumped whenever mLights gains or loses a member (the VK light gather keeps a flat
    // copy of the set, rebuilt only when this changes).
    U32 getLightsGen() const { return mLightsGen; }

    void shiftObjects(const LLVector3 &offset);

    void setLight(LLDrawable *drawablep, bool is_light);

    // GL-REMOVAL-STUB: GL draw statistics / muted-light culling (delete after merge).
    void addTrianglesDrawn(S32) {}
    static void removeMutedAVsLights(LLVOAvatar*) {}

    bool hasRenderDebugFeatureMask(const U32 mask) const    { return bool(mRenderDebugFeatureMask & mask); }
    bool hasRenderDebugMask(const U64 mask) const           { return bool(mRenderDebugMask & mask); }
    void setAllRenderDebugFeatures() { mRenderDebugFeatureMask = 0xffffffff; }
    void clearAllRenderDebugFeatures() { mRenderDebugFeatureMask = 0x0; }
    void setAllRenderDebugDisplays() { mRenderDebugMask = 0xffffffffffffffff; }
    void clearAllRenderDebugDisplays() { mRenderDebugMask = 0x0; }

    bool hasRenderType(const U32 type) const;

    void setRenderTypeMask(U32 type, ...);
    // This is equivalent to 'setRenderTypeMask'
    //void orRenderTypeMask(U32 type, ...);
    void andRenderTypeMask(U32 type, ...);
    void clearRenderTypeMask(U32 type, ...);
    void setAllRenderTypes();
    void clearAllRenderTypes();

    void pushRenderTypeMask();
    void popRenderTypeMask();

    static void toggleRenderType(U32 type);

    // For UI control of render features
    static bool hasRenderTypeControl(U32 data);
    static void toggleRenderDebug(U64 data);
    static void toggleRenderDebugFeature(U32 data);
    static void toggleRenderTypeControl(U32 data);
    static bool toggleRenderTypeControlNegated(S32 data);
    static bool toggleRenderDebugControl(U64 data);
    static bool toggleRenderDebugFeatureControl(U32 data);
    static void setRenderDebugFeatureControl(U32 bit, bool value);

    // Develop/View > Beacons (scripted, touch, physical, MOAP, particle, sound) and
    // the telehub floater's beacons: queues this frame's debug beacons into
    // gObjectList. Called from the VK world-overlay pass before renderObjectBeacons.
    static void addDebugBeacons();
    // Develop > Render Metadata overlays drawn under Vulkan from the world-overlay gGL
    // segment (#29): Physics Shapes and Bounding Boxes (LL renderPhysicsShapes /
    // renderBoundingBox, deleted with the spatial partitions).
    static void renderDebugOverlaysVK();

    static void setRenderParticleBeacons(bool val);
    static void toggleRenderParticleBeacons();
    static bool getRenderParticleBeacons();

    static void setRenderSoundBeacons(bool val);
    static void toggleRenderSoundBeacons();
    static bool getRenderSoundBeacons();

    static void setRenderMOAPBeacons(bool val);
    static void toggleRenderMOAPBeacons();
    static bool getRenderMOAPBeacons();

    static void setRenderPhysicalBeacons(bool val);
    static void toggleRenderPhysicalBeacons();
    static bool getRenderPhysicalBeacons();

    static void setRenderScriptedBeacons(bool val);
    static void toggleRenderScriptedBeacons();
    static bool getRenderScriptedBeacons();

    static void setRenderScriptedTouchBeacons(bool val);
    static void toggleRenderScriptedTouchBeacons();
    static bool getRenderScriptedTouchBeacons();

    static void setRenderBeacons(bool val);
    static void toggleRenderBeacons();
    static bool getRenderBeacons();

    static void setRenderHighlights(bool val);
    static void toggleRenderHighlights();
    static bool getRenderHighlights();
    static void setRenderHighlightTextureChannel(LLRender::eTexIndex channel); // sets which UV setup to display in highlight overlay

    static void updateRenderTransparentWater();
    static void refreshCachedSettings();

    // GL-REMOVAL-STUB: GL debug-blip / terrain-skip render hooks (delete after merge).
    void addDebugBlip(const LLVector3&, const LLColor4&) {}
    void skipRenderingOfTerrain(bool) {}

    void hidePermanentObjects( std::vector<U32>& restoreList );
    void restorePermanentObjects( const std::vector<U32>& restoreList );
    void hideObject( const LLUUID& id );
    void restoreHiddenObject( const LLUUID& id );

    LLHeroProbeManager mHeroProbeManager;

private:
    void addToQuickLookup( LLDrawPool* new_poolp );
    void removeFromQuickLookup( LLDrawPool* poolp );
    void hideDrawable( LLDrawable *pDrawable );
    void unhideDrawable( LLDrawable *pDrawable );
public:
    enum LLRenderTypeMask
    {
        // Following are pool types (some are also object types)
        RENDER_TYPE_SKY                         = LLDrawPool::POOL_SKY,
        RENDER_TYPE_WL_SKY                      = LLDrawPool::POOL_WL_SKY,
        RENDER_TYPE_TERRAIN                     = LLDrawPool::POOL_TERRAIN,
        RENDER_TYPE_SIMPLE                      = LLDrawPool::POOL_SIMPLE,
        RENDER_TYPE_GRASS                       = LLDrawPool::POOL_GRASS,
        RENDER_TYPE_ALPHA_MASK                  = LLDrawPool::POOL_ALPHA_MASK,
        RENDER_TYPE_FULLBRIGHT_ALPHA_MASK       = LLDrawPool::POOL_FULLBRIGHT_ALPHA_MASK,
        RENDER_TYPE_FULLBRIGHT                  = LLDrawPool::POOL_FULLBRIGHT,
        RENDER_TYPE_BUMP                        = LLDrawPool::POOL_BUMP,
        RENDER_TYPE_MATERIALS                   = LLDrawPool::POOL_MATERIALS,
        RENDER_TYPE_AVATAR                      = LLDrawPool::POOL_AVATAR,
        RENDER_TYPE_CONTROL_AV                  = LLDrawPool::POOL_CONTROL_AV, // Animesh
        RENDER_TYPE_TREE                        = LLDrawPool::POOL_TREE,
        RENDER_TYPE_WATEREXCLUSION              = LLDrawPool::POOL_WATEREXCLUSION,
        RENDER_TYPE_VOIDWATER                   = LLDrawPool::POOL_VOIDWATER,
        RENDER_TYPE_WATER                       = LLDrawPool::POOL_WATER,
        RENDER_TYPE_GLTF_PBR                    = LLDrawPool::POOL_GLTF_PBR,
        RENDER_TYPE_GLTF_PBR_ALPHA_MASK         = LLDrawPool::POOL_GLTF_PBR_ALPHA_MASK,
        RENDER_TYPE_ALPHA                       = LLDrawPool::POOL_ALPHA,
        RENDER_TYPE_ALPHA_PRE_WATER             = LLDrawPool::POOL_ALPHA_PRE_WATER,
        RENDER_TYPE_ALPHA_POST_WATER            = LLDrawPool::POOL_ALPHA_POST_WATER,
        RENDER_TYPE_GLOW                        = LLDrawPool::POOL_GLOW,
        RENDER_TYPE_PASS_SIMPLE                 = LLRenderPass::PASS_SIMPLE,
        RENDER_TYPE_PASS_SIMPLE_RIGGED = LLRenderPass::PASS_SIMPLE_RIGGED,
        RENDER_TYPE_PASS_GRASS                  = LLRenderPass::PASS_GRASS,
        RENDER_TYPE_PASS_FULLBRIGHT             = LLRenderPass::PASS_FULLBRIGHT,
        RENDER_TYPE_PASS_FULLBRIGHT_RIGGED = LLRenderPass::PASS_FULLBRIGHT_RIGGED,
        RENDER_TYPE_PASS_INVISIBLE              = LLRenderPass::PASS_INVISIBLE,
        RENDER_TYPE_PASS_INVISIBLE_RIGGED = LLRenderPass::PASS_INVISIBLE_RIGGED,
        RENDER_TYPE_PASS_INVISI_SHINY           = LLRenderPass::PASS_INVISI_SHINY,
        RENDER_TYPE_PASS_INVISI_SHINY_RIGGED = LLRenderPass::PASS_INVISI_SHINY_RIGGED,
        RENDER_TYPE_PASS_FULLBRIGHT_SHINY       = LLRenderPass::PASS_FULLBRIGHT_SHINY,
        RENDER_TYPE_PASS_FULLBRIGHT_SHINY_RIGGED = LLRenderPass::PASS_FULLBRIGHT_SHINY_RIGGED,
        RENDER_TYPE_PASS_SHINY                  = LLRenderPass::PASS_SHINY,
        RENDER_TYPE_PASS_SHINY_RIGGED = LLRenderPass::PASS_SHINY_RIGGED,
        RENDER_TYPE_PASS_BUMP                   = LLRenderPass::PASS_BUMP,
        RENDER_TYPE_PASS_BUMP_RIGGED = LLRenderPass::PASS_BUMP_RIGGED,
        RENDER_TYPE_PASS_POST_BUMP              = LLRenderPass::PASS_POST_BUMP,
        RENDER_TYPE_PASS_POST_BUMP_RIGGED = LLRenderPass::PASS_POST_BUMP_RIGGED,
        RENDER_TYPE_PASS_GLOW                   = LLRenderPass::PASS_GLOW,
        RENDER_TYPE_PASS_GLOW_RIGGED = LLRenderPass::PASS_GLOW_RIGGED,
        RENDER_TYPE_PASS_GLTF_GLOW = LLRenderPass::PASS_GLTF_GLOW,
        RENDER_TYPE_PASS_GLTF_GLOW_RIGGED = LLRenderPass::PASS_GLTF_GLOW_RIGGED,
        RENDER_TYPE_PASS_ALPHA                  = LLRenderPass::PASS_ALPHA,
        RENDER_TYPE_PASS_ALPHA_MASK             = LLRenderPass::PASS_ALPHA_MASK,
        RENDER_TYPE_PASS_ALPHA_MASK_RIGGED = LLRenderPass::PASS_ALPHA_MASK_RIGGED,
        RENDER_TYPE_PASS_FULLBRIGHT_ALPHA_MASK  = LLRenderPass::PASS_FULLBRIGHT_ALPHA_MASK,
        RENDER_TYPE_PASS_FULLBRIGHT_ALPHA_MASK_RIGGED = LLRenderPass::PASS_FULLBRIGHT_ALPHA_MASK_RIGGED,
        RENDER_TYPE_PASS_MATERIAL               = LLRenderPass::PASS_MATERIAL,
        RENDER_TYPE_PASS_MATERIAL_RIGGED = LLRenderPass::PASS_MATERIAL_RIGGED,
        RENDER_TYPE_PASS_MATERIAL_ALPHA         = LLRenderPass::PASS_MATERIAL_ALPHA,
        RENDER_TYPE_PASS_MATERIAL_ALPHA_RIGGED = LLRenderPass::PASS_MATERIAL_ALPHA_RIGGED,
        RENDER_TYPE_PASS_MATERIAL_ALPHA_MASK    = LLRenderPass::PASS_MATERIAL_ALPHA_MASK,
        RENDER_TYPE_PASS_MATERIAL_ALPHA_MASK_RIGGED = LLRenderPass::PASS_MATERIAL_ALPHA_MASK_RIGGED,
        RENDER_TYPE_PASS_MATERIAL_ALPHA_EMISSIVE= LLRenderPass::PASS_MATERIAL_ALPHA_EMISSIVE,
        RENDER_TYPE_PASS_MATERIAL_ALPHA_EMISSIVE_RIGGED = LLRenderPass::PASS_MATERIAL_ALPHA_EMISSIVE_RIGGED,
        RENDER_TYPE_PASS_SPECMAP                = LLRenderPass::PASS_SPECMAP,
        RENDER_TYPE_PASS_SPECMAP_RIGGED = LLRenderPass::PASS_SPECMAP_RIGGED,
        RENDER_TYPE_PASS_SPECMAP_BLEND          = LLRenderPass::PASS_SPECMAP_BLEND,
        RENDER_TYPE_PASS_SPECMAP_BLEND_RIGGED = LLRenderPass::PASS_SPECMAP_BLEND_RIGGED,
        RENDER_TYPE_PASS_SPECMAP_MASK           = LLRenderPass::PASS_SPECMAP_MASK,
        RENDER_TYPE_PASS_SPECMAP_MASK_RIGGED = LLRenderPass::PASS_SPECMAP_MASK_RIGGED,
        RENDER_TYPE_PASS_SPECMAP_EMISSIVE       = LLRenderPass::PASS_SPECMAP_EMISSIVE,
        RENDER_TYPE_PASS_SPECMAP_EMISSIVE_RIGGED = LLRenderPass::PASS_SPECMAP_EMISSIVE_RIGGED,
        RENDER_TYPE_PASS_NORMMAP                = LLRenderPass::PASS_NORMMAP,
        RENDER_TYPE_PASS_NORMMAP_RIGGED = LLRenderPass::PASS_NORMMAP_RIGGED,
        RENDER_TYPE_PASS_NORMMAP_BLEND          = LLRenderPass::PASS_NORMMAP_BLEND,
        RENDER_TYPE_PASS_NORMMAP_BLEND_RIGGED = LLRenderPass::PASS_NORMMAP_BLEND_RIGGED,
        RENDER_TYPE_PASS_NORMMAP_MASK           = LLRenderPass::PASS_NORMMAP_MASK,
        RENDER_TYPE_PASS_NORMMAP_MASK_RIGGED = LLRenderPass::PASS_NORMMAP_MASK_RIGGED,
        RENDER_TYPE_PASS_NORMMAP_EMISSIVE       = LLRenderPass::PASS_NORMMAP_EMISSIVE,
        RENDER_TYPE_PASS_NORMMAP_EMISSIVE_RIGGED = LLRenderPass::PASS_NORMMAP_EMISSIVE_RIGGED,
        RENDER_TYPE_PASS_NORMSPEC               = LLRenderPass::PASS_NORMSPEC,
        RENDER_TYPE_PASS_NORMSPEC_RIGGED = LLRenderPass::PASS_NORMSPEC_RIGGED,
        RENDER_TYPE_PASS_NORMSPEC_BLEND         = LLRenderPass::PASS_NORMSPEC_BLEND,
        RENDER_TYPE_PASS_NORMSPEC_BLEND_RIGGED = LLRenderPass::PASS_NORMSPEC_BLEND_RIGGED,
        RENDER_TYPE_PASS_NORMSPEC_MASK          = LLRenderPass::PASS_NORMSPEC_MASK,
        RENDER_TYPE_PASS_NORMSPEC_MASK_RIGGED = LLRenderPass::PASS_NORMSPEC_MASK_RIGGED,
        RENDER_TYPE_PASS_NORMSPEC_EMISSIVE      = LLRenderPass::PASS_NORMSPEC_EMISSIVE,
        RENDER_TYPE_PASS_NORMSPEC_EMISSIVE_RIGGED = LLRenderPass::PASS_NORMSPEC_EMISSIVE_RIGGED,
        RENDER_TYPE_PASS_GLTF_PBR                 = LLRenderPass::PASS_GLTF_PBR,
        RENDER_TYPE_PASS_GLTF_PBR_RIGGED         = LLRenderPass::PASS_GLTF_PBR_RIGGED,
        RENDER_TYPE_PASS_GLTF_PBR_ALPHA_MASK        = LLRenderPass::PASS_GLTF_PBR_ALPHA_MASK,
        RENDER_TYPE_PASS_GLTF_PBR_ALPHA_MASK_RIGGED = LLRenderPass::PASS_GLTF_PBR_ALPHA_MASK_RIGGED,
        // Following are object types (only used in drawable mRenderType)
        RENDER_TYPE_HUD = LLRenderPass::NUM_RENDER_TYPES,
        RENDER_TYPE_VOLUME,
        RENDER_TYPE_PARTICLES,
        RENDER_TYPE_CLOUDS,
        RENDER_TYPE_HUD_PARTICLES,
        NUM_RENDER_TYPES,
        END_RENDER_TYPES = NUM_RENDER_TYPES
    };

    enum LLRenderDebugFeatureMask
    {
        RENDER_DEBUG_FEATURE_UI                 = 0x0001,
        RENDER_DEBUG_FEATURE_SELECTED           = 0x0002,
        RENDER_DEBUG_FEATURE_HIGHLIGHTED        = 0x0004,
        RENDER_DEBUG_FEATURE_DYNAMIC_TEXTURES   = 0x0008,
//      RENDER_DEBUG_FEATURE_HW_LIGHTING        = 0x0010,
        RENDER_DEBUG_FEATURE_FLEXIBLE           = 0x0010,
        RENDER_DEBUG_FEATURE_FOG                = 0x0020,
        RENDER_DEBUG_FEATURE_FR_INFO            = 0x0080,
        RENDER_DEBUG_FEATURE_FOOT_SHADOWS       = 0x0100,
    };

    enum LLRenderDebugMask: U64
    {
        RENDER_DEBUG_COMPOSITION        =  0x00000001,
        RENDER_DEBUG_VERIFY             =  0x00000002,
        RENDER_DEBUG_BBOXES             =  0x00000004,
        RENDER_DEBUG_WIND_VECTORS       =  0x00000010,
        RENDER_DEBUG_POINTS             =  0x00000040,
        RENDER_DEBUG_TEXTURE_PRIORITY   =  0x00000080,
        RENDER_DEBUG_TEXTURE_AREA       =  0x00000100,
        RENDER_DEBUG_FACE_AREA          =  0x00000200,
        RENDER_DEBUG_PARTICLES          =  0x00000400,
        RENDER_DEBUG_GLOW               =  0x00000800, // not used
        RENDER_DEBUG_TEXTURE_ANIM       =  0x00001000,
        RENDER_DEBUG_LIGHTS             =  0x00002000,
        RENDER_DEBUG_BATCH_SIZE         =  0x00004000,
        RENDER_DEBUG_ALPHA_BINS         =  0x00008000, // not used
        RENDER_DEBUG_RAYCAST            =  0x00010000,
        RENDER_DEBUG_AVATAR_DRAW_INFO   =  0x00020000,
        RENDER_DEBUG_SHADOW_FRUSTA      =  0x00040000,
        RENDER_DEBUG_SCULPTED           =  0x00080000,
        RENDER_DEBUG_AVATAR_VOLUME      =  0x00100000,
        RENDER_DEBUG_AVATAR_JOINTS      =  0x00200000,
        RENDER_DEBUG_AGENT_TARGET       =  0x00800000,
        RENDER_DEBUG_UPDATE_TYPE        =  0x01000000,
        RENDER_DEBUG_PHYSICS_SHAPES     =  0x02000000,
        RENDER_DEBUG_NORMALS            =  0x04000000,
        RENDER_DEBUG_LOD_INFO           =  0x08000000,
        RENDER_DEBUG_NODES              =  0x20000000,
        RENDER_DEBUG_TEXEL_DENSITY      =  0x40000000,
        RENDER_DEBUG_TRIANGLE_COUNT     =  0x80000000,
        RENDER_DEBUG_IMPOSTORS          = 0x100000000,
        RENDER_DEBUG_REFLECTION_PROBES  = 0x200000000,
        RENDER_DEBUG_PROBE_UPDATES      = 0x400000000,
    };

public:

    // HUD debug-text counters (LLViewerWindow debug overlay).
    S32                      mMatrixOpCount;
    S32                      mTextureMatrixOps;

    static S32              sCompiles;

    static bool             sShowHUDAttachments;
    static bool             sForceOldBakedUpload; // If true will not use capabilities to upload baked textures.
    static S32              sUseOcclusion;  // 0 = no occlusion, 1 = read only, 2 = read/write
    static bool             sAutoMaskAlphaDeferred;
    static bool             sAutoMaskAlphaNonDeferred;
    static bool             sRenderTransparentWater;
    static bool             sBakeSunlight;
    static bool             sNoAlpha;
    static bool             sShadowRender;
    static bool             sDynamicLOD;
    static bool             sPickAvatar;
    static bool             sReflectionRender;
    static bool             sImpostorRender;
    static bool             sRenderGlow;
    static bool             sTextureBindTest;
    static bool             sRenderAttachedLights;
    static bool             sRenderAttachedParticles;
    static bool             sRenderDeferred;
    static S32              sVisibleLightCount;
    static bool             sShowDebugAlpha;   // "Highlight Transparent" (was LLDrawPoolAlpha)
// [SL:KB] - Patch: Render-TextureToggle (Catznip-4.0)
    static bool             sRenderTextures;
// [/SL:KB]

    static LLTrace::EventStatHandle<S64> sStatBatchSize;

    static const U32 MAX_PREVIEW_WIDTH;

protected:
    bool                    mRenderTypeEnabled[NUM_RENDER_TYPES];
    std::stack<std::string> mRenderTypeEnableStack;

    U32                     mRenderDebugFeatureMask;
    U64                     mRenderDebugMask;

    /////////////////////////////////////////////
    //
    //
    LLDrawable::drawable_vector_t   mMovedList;
    LLDrawable::drawable_vector_t   mShiftList;

    LLDrawable::ordered_drawable_set_t  mLights;
    U32                                 mLightsGen = 0;   // getLightsGen

    LLDrawable::drawable_list_t     mPartitionQ; //drawables that need to refresh their bin radius / entry data

    //////////////////////////////////////////////////
    //
    // Draw pools are responsible for storing all rendered data,
    // and performing the actual rendering of objects.
    //
    struct compare_pools
    {
        bool operator()(const LLDrawPool* a, const LLDrawPool* b) const
        {
            if (!a)
                return true;
            else if (!b)
                return false;
            else
            {
                S32 atype = a->getType();
                S32 btype = b->getType();
                if (atype < btype)
                    return true;
                else if (atype > btype)
                    return false;
                else
                    return a->getId() < b->getId();
            }
        }
    };
    typedef std::set<LLDrawPool*, compare_pools > pool_set_t;
    pool_set_t mPools;

    // For quick-lookups into mPools (mapped by texture pointer)
    std::map<uintptr_t, LLDrawPool*>    mTerrainPools;
    std::map<uintptr_t, LLDrawPool*>    mTreePools;
    LLDrawPool*                 mAlphaPoolPreWater = nullptr;
    LLDrawPool*                 mAlphaPoolPostWater = nullptr;
    LLDrawPool*                 mSkyPool = nullptr;
    LLDrawPool*                 mWaterPool = nullptr;
    LLRenderPass*               mSimplePool = nullptr;
    LLRenderPass*               mGrassPool = nullptr;
    LLRenderPass*               mAlphaMaskPool = nullptr;
    LLRenderPass*               mFullbrightAlphaMaskPool = nullptr;
    LLRenderPass*               mFullbrightPool = nullptr;
    LLDrawPool*                 mGlowPool = nullptr;
    LLDrawPool*                 mBumpPool = nullptr;
    LLDrawPool*                 mMaterialsPool = nullptr;
    LLDrawPool*                 mWLSkyPool = nullptr;
    LLDrawPool*                 mPBROpaquePool = nullptr;
    LLDrawPool*                 mPBRAlphaMaskPool = nullptr;
    LLDrawPool*                 mWaterExclusionPool      = nullptr;

    // Note: no need to keep an quick-lookup to avatar pools, since there's only one per avatar

protected:
    static bool             sRenderPhysicalBeacons;
    static bool             sRenderMOAPBeacons;
    static bool             sRenderScriptedTouchBeacons;
    static bool             sRenderScriptedBeacons;
    static bool             sRenderParticleBeacons;
    static bool             sRenderSoundBeacons;
public:
    static bool             sRenderBeacons;
    static bool             sRenderHighlight;

    // Determines which set of UVs to use in highlight display
    //
    static LLRender::eTexIndex sRenderHighlightTextureChannel;

    // GL-REMOVAL-STUB: deprecated GL setting, always on (delete after merge).
    static constexpr bool WindLightUseAtmosShaders = true;

    //cached settings
    static U32 RenderFSAAType;
    static bool RenderUIBuffer;
    static S32 RenderShadowDetail;
    static bool RenderDeferredSSAO;
    static bool RenderAnimateRes;
    static bool FreezeTime;
    static S32 DebugBeaconLineWidth;
    static bool RenderDepthOfField;
    static F32 RenderFarClip;
    static bool RenderScreenSpaceReflections;
    static bool RenderMirrors;
};


extern LLPipeline gPipeline;
extern const LLMatrix4* gGLLastMatrix;

#endif
