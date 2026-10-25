/**
 * @file llcleancursorpick.h
 * @brief The GPU raster cursor pick under Vulkan (#702 / #704): requests and LL's pick rules.
 *
 * The renderer answers the cursor ray every frame (vkgfx/llcleanpickvk.h): every
 * fragment of everything the main view and the HUD draw along it -- the drawn LOD,
 * GPU-skinned rigged meshes as posed, worn attachments on this frame's joints -- with
 * the raster's alpha verdict and the surface LL's pick reports, no depth test. It is the
 * viewer's only object pick (#704: no CPU narrowphase). This module asks for it and turns
 * the fragment list into LL's pick result:
 *
 *  - fillRequest: the cursor ray (cursorIntersect's construction) and its 1x1
 *    projections (the main view's camera-relative projection and LL's HUD camera, each
 *    zoomed so one pixel about the ray fills the target), while something keeps picking
 *    (arm) or the mouse is over the window. The frame's projections are kept, so any ray
 *    can be asked for later between frames (LLVulkanContext::pickNow).
 *  - intersectAll: cursorIntersect's "every object" pick. HUD fragments first (nearest
 *    that passes LL's filters wins, as LL's HUD pick), else the world list by distance:
 *      static / worn rigid / HUD fragments: LL's object filters (vkPickFiltersPass:
 *        selectable, click action ignore, render type, the hidden own attachments in
 *        mouselook #554, the helper object #537), reflection probes only when asked,
 *        LL's invisible-face rule (colour alpha 0 needs pick_transparent or a special
 *        hover cursor), and the alpha verdict -- except where the texel test does not
 *        apply (testsAlpha: hit-everything picks, transparent click zones, HUD
 *        transparent picks, beacons; media faces); glTF scene objects report the node /
 *        primitive of the fragment's instance;
 *      GPU-skinned rigged fragments (worn rigged items, playing animesh, system bodies):
 *        the #255 / 42e0d982c0 claims -- the item itself for rigged picks (right click),
 *        own items in build mode and animesh (ITEM), a touchable item on a left click /
 *        hover (TOUCH), else the WEARER; a WEARER claim never hides a rigid attachment
 *        behind it, a TOUCH claim only a non-touchable one;
 *      then the avatars' collision volumes (LL's ATTACHMENT_OVERRIDE_DIST precedence) --
 *        kept on the CPU on purpose: ~20 analytic joint spheres per avatar (posed live for
 *        GPU-animated avatars, #610) plus LL's name-tag box, not mesh narrowphase data. A
 *        volume of the avatar a WEARER claim already resolved to keeps the drawn mesh
 *        surface (the volumes stick out of the body).
 *    Answer policy: inside a mouse-down (ClickScope) only an answer whose request ray is
 *    this ray (same camera, cursor within 2 px); when the newest does not cover it, the
 *    ray is answered NOW, between frames (one GPU wait). A click's answer is exact
 *    beyond the fragment list's size: when a depth window overflowed (after the GPU's own
 *    count-then-fill round) or LL's rules need fragments past it, the click asks again
 *    for the narrower / next window (paging). Other picks (hover, tooltips, DoF focus,
 *    the debug raycast) take the newest answer while it is a few frames old (its ray
 *    trails the cursor by ~2 frames, like every GPU pick), never wait, and treat what
 *    lies past an undecided window as nothing.
 *  - intersectObject: the single-object surface pick (cursorIntersect's this_object path:
 *    getSurfaceInfo, face select, touch / media UVs, the texture inspector): the surface
 *    the all-objects pick just reported on it, else the object's nearest accepted fragment
 *    on an answer for that ray (exact; answered now when needed).
 *  - surfaceTexture: the UV the raster samples at that surface (planar texgen + TE / glTF
 *    transform / texture animation, LL's mUVCoords) and its per-pixel derivatives.
 *
 * RenderVKPickDebug logs every click pick (hover throttled) and a per-second summary.
 *
 * Where the GPU and the old CPU pick disagreed on layered geometry (RenderVKPickVerify,
 * #704: one object, different faces, points cm apart) the GPU is what is on screen: it
 * tests the drawn texel at the drawn mip with the mask's mip compensation (maskTestAlpha),
 * where the CPU read a 128^2 max-filtered 4-bit map that kept near-transparent outer
 * layers solid; and it draws the cull's own LOD, where the CPU re-derived one (drawnSlot,
 * camera origin, LL's LOD factor) that could differ under the VRAM-scaled factor.
 */
#ifndef LL_LLCLEANCURSORPICK_H
#define LL_LLCLEANCURSORPICK_H

#include "stdtypes.h"
#include "llvector4a.h"
#include <string>
#include "v2math.h"
#include "v3math.h"

#include <vector>

struct LLVKWorldFrame;
struct LLVKRectQuery;
class LLViewerObject;

namespace LLCleanCursorPick
{
    // The renderer runs (Vulkan up).
    bool gpuEnabled();

    // This frame's request (main frame only, after buildVulkanWorldFrame).
    void fillRequest(LLVKWorldFrame& wf);
    // A pick wants per-frame answers (keeps the request alive a few frames).
    void arm();

    // Picks issued while one is alive are clicks (LLViewerWindow::handleAnyMouseClick):
    // exact answers only, answered now if needed.
    struct ClickScope
    {
        explicit ClickScope(bool active = true);
        ~ClickScope();
        ClickScope(const ClickScope&) = delete;
        ClickScope& operator=(const ClickScope&) = delete;
    private:
        bool mActive;
    };
    bool inClick();

    // ASYNC CLICK PICKS (RenderVKPickAsyncClick, default on, live). A click's async pick
    // (pickAsync from a mouse button: LLViewerWindow::performPick, a frame later) whose exact
    // answer is not in yet used to be answered NOW -- one blocking submission behind the
    // frames in flight (~30 ms at an 18 ms GPU frame, the vk_performPick hitches). Now
    // performPick asks holdClick first: true = keep the pick (and, through
    // LLWindow::delayInputProcessing, the input after it) for this frame: the per-frame request
    // asks for the click's pixel and the pick is fetched when that answer is harvested (~3-4
    // frames, nothing waits); false = fetch now (an answer covers it already, the answer came,
    // the wait passed kMaxClickWaitFrames -- then answered now as before -- or the setting is
    // off). Synchronous picks (pickImmediate inside a click: LLToolPie's) still answer now
    // when no answer covers their ray.
    bool holdClick(S32 mouse_x, S32 mouse_y);
    // performPick fetched the held click: the per-frame request follows the cursor again.
    void clickFetched();

    // Pick health, for other layers' logs (the right-click health line): whether the GPU pick
    // can run (and why not), the outstanding "pick health:" problem if any, click counters.
    std::string healthSummary();
    // File a pick problem on the "pick health:" line (same rate limits as the pick's own).
    void noteProblem(const std::string& why);

    // How intersectAll answered.
    enum Source
    {
        SRC_NONE = 0,    // no answer (renderer not up; a hover with no recent answer)
        SRC_EXACT = 1,   // an answer to this very ray
        SRC_TRAIL = 2,   // the newest answer, a frame or two behind the cursor (hover)
    };
    // cursorIntersect's "check ALL objects" pick. hud_blocked: the RLVa camera-tool rule
    // (no HUD hits). mouse_x / mouse_y: cursorIntersect's x / y. gltf_node / gltf_prim:
    // a glTF scene object's node / primitive (else -1).
    Source intersectAll(S32 mouse_x, S32 mouse_y,
                        const LLVector4a& mw_start, const LLVector4a& mw_end,
                        const LLVector4a& mh_start, const LLVector4a& mh_end, bool hud_blocked,
                        bool pick_transparent, bool pick_rigged, bool pick_unselectable,
                        bool pick_reflection_probe, LLViewerObject*& found,
                        S32* face_hit, S32* gltf_node, S32* gltf_prim,
                        LLVector4a* intersection, LLVector2* uv,
                        LLVector4a* normal, LLVector4a* tangent);
    // Single-object surface pick (cursorIntersect's this_object path): the nearest surface
    // of obj (face >= 0: of that face) on the ray [start, end] -- the HUD segment for a HUD
    // attachment -- under obj's pick rules. tex_coord = LL's surface ST.
    bool intersectObject(LLViewerObject* obj, S32 mouse_x, S32 mouse_y,
                         const LLVector4a& start, const LLVector4a& end, S32 face,
                         bool pick_transparent, bool pick_unselectable,
                         S32* face_hit, S32* gltf_node, S32* gltf_prim, LLVector4a* intersection,
                         LLVector2* tex_coord, LLVector4a* normal, LLVector4a* tangent);
    // Depth of field "focus follows pointer" (#724): the nearest surface the cursor ray
    // shows on the newest per-frame answer (its ray trails the cursor by ~2 frames, like
    // hover) -- the world layer only (HUDs are not in the scene DoF blurs), anything drawn
    // and kept by the raster's alpha rule (no click / select filters: DoF focuses on what
    // is seen). Never waits, never asks for a request of its own, never touches the
    // surface the last pick reported.
    enum Focus
    {
        FOCUS_UNKNOWN = 0,   // no recent answer, or its window ended undecided: keep the last focus
        FOCUS_NONE = 1,      // the whole ray is answered and holds nothing (sky)
        FOCUS_HIT = 2,       // pos = agent-space point under the cursor
    };
    Focus focusPoint(LLVector3& pos);

    // The last surface a pick reported on (obj, face): the UV the raster samples there and
    // that UV per screen pixel (du/dx, dv/dx, du/dy, dv/dy; may be null). False = none.
    bool surfaceTexture(const LLViewerObject* obj, S32 face, LLVector2& uv, F32* duv_px);

    // DRAWN-GEOMETRY QUERIES (#704, LLVulkanContext::faceQuery): the face as the renderer
    // draws it (its drawn LOD, else the highest resident one), from the GPU. Blocking (waits
    // for the frames in flight): build-tool actions only, never per frame.
    // The face's vertex frame data, volume space (any out-param may be null): the normals'
    // sum (its direction is the face's approximate normal), the first vertex's normal and
    // tangent (w handedness). False = no resident geometry for it.
    bool drawnFaceFrame(const LLViewerObject* obj, S32 face, LLVector4a* normal_sum,
                        LLVector4a* normal0, LLVector4a* tangent0);
    // Rectangle test of objs' drawn vertices (every face): per object, any vertex inside /
    // any outside or behind the camera. False = it can't run (results all false).
    bool drawnRectTest(const std::vector<LLViewerObject*>& objs, const LLVKRectQuery& rect,
                       std::vector<U8>& any_in, std::vector<U8>& any_out);

    // Why a rigged hit resolved to what it did (see the header comment).
    enum Claim
    {
        CLAIM_ITEM = 0,     // the item itself: rigged picking, own item in build mode, animesh
        CLAIM_TOUCH = 1,    // the item itself only because it is touchable (left click / hover)
        CLAIM_WEARER = 2,   // the avatar: non-touchable worn rigged mesh / system body
    };
    struct Hit
    {
        Claim claim = CLAIM_ITEM;
        LLViewerObject* obj = nullptr;
        S32 face = -1;
        LLVector4a pos, normal, tangent;
        LLVector2 st;
    };

    // LLToolPie's test for "a left click touches this" (hand cursor) on an attachment:
    // click action not "none" and the prim or its root handles touch.
    bool touchable(const LLViewerObject* obj);

    // `obj` is rigged geometry the renderer SKINS (worn rigged attachment, playing
    // animesh): its CPU volume is the bind pose, useless for picking.
    bool gpuSkinned(const LLViewerObject* obj);

    // Every pick hits every surface (Select Invisible Objects in build mode, or Highlight
    // Transparent): LLViewerWindow::pickImmediate / pickAsync's pick_transparent override.
    bool alphaBypass();
    // PickAlphaThreshold: the alpha-blend cut (effective alpha below it = empty, #276).
    F32 alphaBlendThreshold();
    // The raster's alpha verdict applies to hits on obj in this pick: not for hit-everything
    // picks, transparent click zones (special hover cursor on the prim or its root) in
    // transparent picks, HUDs in transparent picks, highlighted / beacon objects.
    bool testsAlpha(const LLViewerObject* obj, bool pick_transparent);
}

#endif
