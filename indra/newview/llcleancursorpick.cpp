/**
 * @file llcleancursorpick.cpp
 * @brief The GPU raster cursor pick under Vulkan (#702 / #704). See llcleancursorpick.h.
 */
#include "llviewerprecompiledheaders.h"

#include "llcleancursorpick.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llappviewer.h"           // gFrameCount
#include "llcleangltfingest.h"
#include "llcleaninstancestore.h"
#include "llcontrolavatar.h"
#include "lldrawable.h"
#include "llfloater.h"
#include "llfloaterreg.h"
#include "llfloatertools.h"
#include "lltextureentry.h"
#include "lltimer.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerobjectlist.h"
#include "llviewerwindow.h"
#include "llvkhitch.h"             // #757 crash breadcrumb / hitch sections
#include "llvoavatar.h"
#include "llvoavatarself.h"
#include "llvovolume.h"
#include "pipeline.h"
#include "raytrace.h"
#include "vkgfx/llvulkancontext.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace
{
// Request ray length: far past any pick segment (LLPickInfo uses 512 / 1024 m), so an
// answer covers every segment along its ray.
// A pick's ray may be longer (the camera tool's focus pick reaches the draw distance,
// LLPickInfo::getPickDepth, #560): its request then spans the ray (makeRequest) and only an
// answer at least that long covers it.
constexpr F32 kRayLength = 1024.f;
// HUD ray: LL's HUD pick segment runs `depth` either side of the cursor's HUD point.
constexpr F32 kHudDepth = 512.f;
// A pick keeps the per-frame request alive this many frames; nothing picking (cursor
// over the UI, a still mouse past the hover refresh) = no GPU pick work.
constexpr U32 kArmFrames = 30;
// A trailing (hover) answer is used while its request is at most this old.
constexpr U32 kMaxTrailFrames = 6;
constexpr F64 kMaxTrailSeconds = 0.25;   // ... and never across a pause
// An exact answer: cursor within this many pixels, same camera (1 cm, 2 px of direction).
constexpr F32 kExactPx = 2.f;
// The single-object pick's trailing surface: within this many pixels.
constexpr F32 kNearPx = 6.f;
constexpr U32 kSerialRing = 32;
// A click asks again at most this many times (narrower / next depth windows).
constexpr U32 kMaxPages = 12;
// The empty window (a page that only needs the other layer).
constexpr F32 kNoWin[2] = {0.f, 0.f};

S32 sClickDepth = 0;
U32 sSerial = 0;
U32 sSerialFrame[kSerialRing] = {};   // the frame each recent serial was issued in
F64 sSerialTime[kSerialRing] = {};    // ... and when
U32 sResetSerial = 0;                 // answers to requests up to this one are void (teleport)
U32 sArmedUntil = 0;
bool sArmed = false;

// The last frame's projections (fillRequest): a ray asked for between frames (pickNow) is
// drawn through the same projection, zoomed on that ray.
struct FrameViews
{
    bool valid = false;
    F32 view_proj_rel[16] = {};   // the main view's camera-relative projection (VK clip)
    F32 apex[3] = {};             // the eye it is relative to
    int rect[4] = {};             // the world view (pixels)
    bool hud = false;             // the HUD layer is drawn
    F32 hud_vp[16] = {};
    int hud_viewport[4] = {};
    U32 mode = 0;
    F32 cam_at[3] = {}, cam_left[3] = {}, cam_up[3] = {};   // the camera's axes (the VR ray's projection)
};
FrameViews sViews;

// #1076 VR world pointer (setVRRay): the agent-space ray that replaces the cursor ray.
struct VRRay
{
    bool on = false;
    LLVector3 s, d;   // origin, unit direction
};
VRRay sVRRay;

// The surface the last pick reported (getSurfaceInfo's single-object pick right after it,
// surfaceTexture).
struct Surface
{
    bool valid = false;
    LLUUID id;
    S32 face = -1;
    S32 node = -1, prim = -1;   // glTF scene objects
    LLVector4a pos, normal, tangent;
    LLVector2 st, uv;
    F32 duv[4] = {};
    bool has_uv = false;        // st / uv / duv from a fragment (not the avatar volumes)
    U32 serial = 0;
    // #837: only the all-objects pick's surface is handed to the single-object pick that
    // follows it, and only for the pixel it was picked at. A surface the single-object pick
    // itself produced is never re-served: a held grab / drag re-picks its object every frame,
    // and re-serving its own answer for kSerialRing requests (~0.5 s) froze the touch data
    // (UV / ST / position) and so the ObjectGrabUpdate rate at ~2 Hz.
    bool from_all = false;
    S32 x = -1, y = -1;
};
Surface sSurface;

// Statistics (RenderVKPickDebug's per-second line).
struct Stats
{
    U32 exact = 0, trail = 0, none = 0, now = 0, pages = 0;
};
Stats sStats;
F64 sStatsNext = 0.0;

// ASYNC CLICK PICKS (llcleancursorpick.h holdClick): the held click (its pixel; the first
// per-frame request made for it; the ray of the answer that came for it, which its fetch takes).
constexpr U32 kMaxClickWaitFrames = 8;
struct HeldClick
{
    bool active = false, ready = false;
    S32 x = 0, y = 0;
    U32 start = 0;        // gFrameCount when held
    U32 serial = 0;       // the first request made for it (0 = none yet)
    LLVector3 s, d;       // the answer's ray (ready)
};
HeldClick sHeld;
// Pick latency (always on, one VKPick line per 10 s when anything waited): clicks held (frames
// until their answer), held clicks that ran out (answered now), picks answered now (ms).
struct Latency
{
    U32 held = 0, heldFrames = 0, heldMax = 0, expired = 0, now = 0;
    F64 nowMs = 0.0, nowMax = 0.0;
};
Latency sLat;
F64 sLatNext = 0.0;

bool asyncClickOn()
{
    static LLCachedControl<bool> s_on(gSavedSettings, "RenderVKPickAsyncClick", true);
    return s_on;
}

bool debugOn()
{
    static LLCachedControl<bool> s_dbg(gSavedSettings, "RenderVKPickDebug", false);
    return s_dbg;
}

U32 nextSerial()
{
    if (++sSerial == 0) sSerial = 1;   // 0 = an unanswered entry
    sSerialFrame[sSerial % kSerialRing] = (U32)gFrameCount;
    sSerialTime[sSerial % kSerialRing] = LLTimer::getTotalSeconds();
    return sSerial;
}

// Answer r's request is recent: one of the last kSerialRing serials, after the last
// reset, issued at most kMaxTrailFrames frames / kMaxTrailSeconds ago.
bool recent(const LLVKPickResult& r)
{
    const U32 sn = r.req.serial;
    if (!r.valid || (U32)(sSerial - sn) >= kSerialRing || (S32)(sn - sResetSerial) <= 0) return false;
    if ((U32)gFrameCount - sSerialFrame[sn % kSerialRing] > kMaxTrailFrames) return false;
    return LLTimer::getTotalSeconds() - sSerialTime[sn % kSerialRing] <= kMaxTrailSeconds;
}

F32 pixelAngle()
{
    const LLViewerCamera* cam = LLViewerCamera::getInstance();
    const S32 h = llmax(cam->getViewHeightInPixels(), 1);
    return cam->getView() / (F32)h;
}

bool hudShown()
{
    return LLPipeline::sShowHUDAttachments && isAgentAvatarValid() && gAgentAvatarp->hasHUDAttachment();
}

// A pick ray: the world segment (and the HUD one), as cursorIntersect builds them.
struct RayQ
{
    S32 x = 0, y = 0;
    LLVector3 s, d;     // world segment start, unit direction
    F32 len = 0.f;
    bool hud = false;   // the HUD layer is wanted
    LLVector3 hs, hd;   // HUD segment
    F32 hlen = 0.f;
};

// Does answer r belong to this very ray: the cursor within kExactPx, the camera the same
// (ray start within 1 cm, direction within kExactPx pixels), the HUD layer drawn if wanted?
bool covers(const LLVKPickResult& r, const RayQ& q, F32 pix)
{
    if (std::abs(r.req.mouse_x - q.x) > (S32)kExactPx || std::abs(r.req.mouse_y - q.y) > (S32)kExactPx) return false;
    if ((LLVector3(r.req.origin) - q.s).length() > 0.01f) return false;
    if (q.hud && !r.req.hud) return false;
    // (a longer ray than the answer's request reaches past what it drew: not covered)
    if (q.len > r.req.length + 0.01f) return false;
    return LLVector3(r.req.dir) * q.d >= std::cos(kExactPx * pix);
}

// A layer's 1x1 projection: the view's own projection (vp, VK clip; world layer: the main
// view's camera-relative one, HUD layer: LL's HUD camera) with point p of the ray moved to
// the centre and one viewport pixel across the target. A positive 2D scale + shift of the
// view's clip x / y: triangle winding (gl_FrontFacing) and clipping stay the view's.
void pixelZoom(const F32 vp[16], const LLVector3& p, const int vp_rect[4], F32 out[16])
{
    F32 c[4];
    for (int r = 0; r < 4; ++r)
        c[r] = vp[r] * p.mV[0] + vp[4 + r] * p.mV[1] + vp[8 + r] * p.mV[2] + vp[12 + r];
    const F32 w = std::fabs(c[3]) > 1e-12f ? c[3] : 1.f;
    const F32 nx = c[0] / w, ny = c[1] / w;
    const F32 sx = vp_rect[2] > 0 ? (F32)vp_rect[2] : (F32)llmax(gViewerWindow->getWindowWidthRaw(), 1);
    const F32 sy = vp_rect[3] > 0 ? (F32)vp_rect[3] : (F32)llmax(gViewerWindow->getWindowHeightRaw(), 1);
    std::memcpy(out, vp, 16 * sizeof(F32));
    for (int col = 0; col < 4; ++col)
    {
        out[col * 4 + 0] = sx * (vp[col * 4 + 0] - nx * vp[col * 4 + 3]);
        out[col * 4 + 1] = sy * (vp[col * 4 + 1] - ny * vp[col * 4 + 3]);
    }
}

// #1076 VR world pointer: the world layer's 1x1 projection for a ray that does not start at
// the camera. The camera's own projection (view_proj_rel: apex-relative -> VK clip) behind a
// rigid move of the scene: translate the ray's origin onto the apex, then the PROPER rotation
// taking the ray's frame (at = the ray, left, up) onto the camera's (at, left, up). The ray then
// is the camera's centre line, and the camera projection pixel-zoomed on its at axis puts it
// in the target's centre. A proper rotation and the camera's own projection keep the view's
// handedness (gl_FrontFacing, clean_pick.frag) and clip conventions. Depth along the ray is
// measured in the fragment shader from the request's origin / dir, not from this matrix.
void vrRayProjection(const RayQ& q, F32 out[16])
{
    const LLVector3 at(sViews.cam_at), left(sViews.cam_left), up(sViews.cam_up);
    const LLVector3 at_c = q.d;
    LLVector3 left_c = LLVector3::z_axis % at_c;
    if (left_c.normVec() < 1e-3f) left_c = left;   // (straight up / down: any left will do)
    LLVector3 up_c = at_c % left_c;
    up_c.normVec();
    // R v = at (v . at_c) + left (v . left_c) + up (v . up_c); column-major rigid move A.
    F32 a[16] = {};
    for (int k = 0; k < 3; ++k)
        for (int r = 0; r < 3; ++r)
            a[k * 4 + r] = at.mV[r] * at_c.mV[k] + left.mV[r] * left_c.mV[k] + up.mV[r] * up_c.mV[k];
    const LLVector3 c = q.s - LLVector3(sViews.apex);   // the origin, apex-relative
    for (int r = 0; r < 3; ++r) a[12 + r] = -(a[r] * c.mV[0] + a[4 + r] * c.mV[1] + a[8 + r] * c.mV[2]);
    a[15] = 1.f;
    F32 z[16];
    pixelZoom(sViews.view_proj_rel, at * 16.f, sViews.rect, z);
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row)
        {
            F32 s = 0.f;
            for (int k = 0; k < 4; ++k) s += z[k * 4 + row] * a[col * 4 + k];
            out[col * 4 + row] = s;
        }
}

// The world ray exactly as LLViewerWindow::cursorIntersect builds it for pixel (x, y):
// camera origin + mouse direction, started on the near plane. False = degenerate.
bool cursorRay(S32 x, S32 y, RayQ& q)
{
    if (sVRRay.on)
    {
        // #1076: the VR controller's ray (the pixel still keys the answer).
        q.x = x;
        q.y = y;
        q.s = sVRRay.s;
        q.d = sVRRay.d;
        q.len = kRayLength;
        return true;
    }
    LLViewerCamera* cam = LLViewerCamera::getInstance();
    const LLVector3 dir = gViewerWindow->mouseDirectionGlobal(x, y);
    const LLVector3 eye = cam->getOrigin();
    const LLVector3 at = cam->getAtAxis();
    q.x = x;
    q.y = y;
    q.s = eye;
    line_plane(eye, dir, eye + at * cam->getNear(), at, q.s);
    q.d = dir;
    cam->orthoMouseRay(x, y, q.s, q.d);   // <GMDX> orthographic view: parallel rays
    q.len = kRayLength;
    return q.d.normVec() >= 1e-6f;
}

// A request for ray q through the last frame's projections (pickNow). False = no frame yet.
bool makeRequest(const RayQ& q, LLVKPickRequest& r)
{
    r = LLVKPickRequest{};
    if (!sViews.valid) return false;
    r.valid = true;
    std::memcpy(r.origin, q.s.mV, sizeof(r.origin));
    std::memcpy(r.dir, q.d.mV, sizeof(r.dir));
    r.length = llmax(kRayLength, q.len);   // (the ray's own length: the camera tool's reaches the draw distance)
    r.pixel_angle = pixelAngle();
    r.alpha_threshold = LLCleanCursorPick::alphaBlendThreshold();
    r.mouse_x = q.x;
    r.mouse_y = q.y;
    r.serial = nextSerial();
    std::memcpy(r.apex, sViews.apex, sizeof(r.apex));
    if (sVRRay.on)
    {
        // #1076: the ray does not start at the camera (a VR controller's): the 1x1 raster's
        // centre of projection must be the ray's origin, or its pixel would be the camera ray
        // through a point of it.
        vrRayProjection(q, r.world_vp);
    }
    else
    {
        // (A point on the ray 16 m out, relative to the projection's eye.)
        const LLVector3 p = q.s + q.d * 16.f - LLVector3(sViews.apex);
        pixelZoom(sViews.view_proj_rel, p, sViews.rect, r.world_vp);
    }
    if (q.hud && sViews.hud)
    {
        r.hud = true;
        std::memcpy(r.hud_origin, q.hs.mV, sizeof(r.hud_origin));
        std::memcpy(r.hud_dir, q.hd.mV, sizeof(r.hud_dir));
        r.hud_length = q.hlen;
        pixelZoom(sViews.hud_vp, q.hs + q.hd * (0.5f * q.hlen), sViews.hud_viewport, r.hud_vp);
    }
    // Depth windows: the rays' own lengths (the histogram bins span what the ray can hold).
    r.win[0] = -1.f; r.win[1] = r.length;
    r.win[2] = -1.f; r.win[3] = r.hud ? r.hud_length : 0.f;
    r.mode = sViews.mode;
    return true;
}

// PICK HEALTH (always on, independent of RenderVKPickDebug): when an object pick can't
// produce a hit for a reason the code can tell (no answer and why, a candidate overflow
// paging could not resolve, a partial rigged layer, fragments whose instances no longer
// resolve), one VKPick line -- on every change of reason, a repeat at most every 30 s,
// never more than one line per 2 s -- and one line when picks produce hits again. A field
// report of "nothing can be clicked" is then diagnosable without the debug setting.
std::string sDiagLast;
F64 sDiagRepeatAt = 0.0;   // same reason again not before
F64 sDiagAnyAt = 0.0;      // any line not before
bool sDiagBad = false;
U32 sClicks = 0, sClickNone = 0, sClickMiss = 0;   // object clicks since start: asked / no GPU answer / answered, hit nothing

void diagNote(const std::string& why)
{
    sDiagBad = true;
    const F64 now = LLTimer::getTotalSeconds();
    if (now < sDiagAnyAt || (why == sDiagLast && now < sDiagRepeatAt)) return;
    sDiagAnyAt = now + 2.0;
    sDiagRepeatAt = now + 30.0;
    sDiagLast = why;
    LL_INFOS("VKPick") << "pick health: " << why << LL_ENDL;
}

void diagOk()
{
    if (!sDiagBad) return;
    sDiagBad = false;
    if (!sDiagLast.empty())
        LL_INFOS("VKPick") << "pick health: picks producing hits again (last problem: " << sDiagLast << ")" << LL_ENDL;
    sDiagLast.clear();
    sDiagRepeatAt = sDiagAnyAt = 0.0;
}

// Answer `req` now (between frames).
bool askNow(const LLVKPickRequest& req, LLVKPickResult& r)
{
    ++sStats.now;
    const F64 t0 = LLTimer::getTotalSeconds();
    const bool ok = LLVulkanContext::instance().pickNow(req, r) && r.valid;
    const F64 ms = (LLTimer::getTotalSeconds() - t0) * 1000.0;
    ++sLat.now;
    sLat.nowMs += ms;
    sLat.nowMax = llmax(sLat.nowMax, ms);
    return ok;
}

// An answer for ray q. exact: its request is q's ray. Exact answers are required for
// clicks (sync: answered now when the newest does not cover q); hover takes a trailing one.
// why: the reason when there is none (static text).
bool answerFor(const RayQ& q, bool sync, LLVKPickResult& r, bool& exact, std::string* why = nullptr)
{
    exact = false;
    const F32 pix = pixelAngle();
    LLVulkanContext& vk = LLVulkanContext::instance();
    const bool have = vk.latestPick(r) && recent(r);
    if (have && covers(r, q, pix))
    {
        exact = true;
        return true;
    }
    if (!sync)
    {
        if (why) *why = have ? "the newest answer lacks the HUD layer" : "no recent per-frame answer";
        return have && (!q.hud || r.req.hud);
    }
    LLVKPickRequest req;
    if (!vk.pickGpuAvailable())
    {
        if (why) *why = std::string("GPU pick unavailable: ") + vk.pickFailure();
        return false;
    }
    if (!makeRequest(q, req))
    {
        if (why)
            *why = gAgent.getTeleportState() != LLAgent::TELEPORT_NONE
                       ? "no frame projection: teleport in progress (state " + std::to_string((S32)gAgent.getTeleportState()) + ")"
                       : std::string("no frame projection yet (no world frame since the last reset)");
        return false;
    }
    if (!askNow(req, r))
    {
        if (why) *why = std::string("answer-now failed: ") + vk.pickFailure();
        return false;
    }
    exact = true;
    return true;
}

// A fragment's (object, face): its clean instance's key, live.
bool fragObject(const LLVKPickFrag& f, LLViewerObject*& obj, S32& face)
{
    LLUUID id;
    if (!LLCleanInstanceStore::instance().keyForId(f.instance, id, face)) return false;
    obj = gObjectList.findObject(id);
    return obj && !obj->isDead();
}

// LL's per-face pick rules for a non-skinned fragment (LLVOVolume::lineSegmentIntersect's
// filters + the raster's alpha verdict): object filters, the invisible-face rule, the alpha
// verdict and its exemptions.
bool staticAccept(LLViewerObject* obj, S32 face, const LLVKPickFrag& f, bool pick_transparent,
                  bool pick_unselectable)
{
    const bool pass = (f.flags & LLVKPickFrag::ALPHA_PASS) != 0u;
    if (obj->getPCode() != LL_PCODE_VOLUME)
    {
        // Trees / grass / terrain: their own filters (LLVOGrass: selectable, render type),
        // the drawn alpha (LL picked trees by their box).
        if (obj->getPCode() == LL_PCODE_LEGACY_GRASS
            && (!obj->mbCanSelect || obj->mDrawable.isNull() || obj->mDrawable->isDead()
                || !gPipeline.hasRenderType(obj->mDrawable->getRenderType())))
            return false;
        return pass || LLCleanCursorPick::alphaBypass();
    }
    LLVOVolume* vo = (LLVOVolume*)obj;
    if (!vo->vkPickFiltersPass(pick_unselectable)) return false;
    bool pt = pick_transparent;
    bool special = obj->specialHoverCursor();
    if (!obj->isAttachment())
    {
        pt = pt || vo->isHiglightedOrBeacon();
        special = special && !vo->isReflectionProbe();
    }
    // glTF scene objects: their faces are (node, primitive) instances, not TEs.
    const LLTextureEntry* te = (!obj->mGLTFAsset && face >= 0 && face < obj->getNumTEs()) ? obj->getTE((U8)face) : nullptr;
    const bool invisible = te && te->getColor().mV[3] == 0.f;
    if (invisible && !special && !pt) return false;   // LL: completely transparent face
    if (pass) return true;
    // The texel verdict is not LL's rule here: hit-everything picks, click zones in
    // transparent picks, HUDs in transparent picks, beacons; invisible faces LL's rule
    // picked; media faces.
    if (!LLCleanCursorPick::testsAlpha(obj, pt)) return true;
    return invisible || (te && te->hasMedia());
}

// The object (and face) LL's rules report for a GPU-skinned (or system body) hit, and the
// claim (#255 / 42e0d982c0).
bool resolveRig(const LLVKPickFrag& f, LLViewerObject* obj, S32 face, bool pick_rigged, bool pick_unselectable,
                LLCleanCursorPick::Hit& out)
{
    LLVOAvatar* report_avatar = nullptr;
    LLCleanCursorPick::Claim claim = LLCleanCursorPick::CLAIM_ITEM;
    if (obj->isAvatar())
    {
        // System body part (non-mesh avatar body): the avatar itself.
        report_avatar = (LLVOAvatar*)obj;
        claim = LLCleanCursorPick::CLAIM_WEARER;
    }
    else
    {
        LLVOVolume* vo = (LLVOVolume*)obj;
        if (obj->isAttachment())
        {
            LLVOAvatar* wearer = obj->getAvatarAncestor();
            if (!wearer || wearer->isDead()) return false;
            // LL: rigged attachments are picked as themselves only when rigged picking was
            // asked for, or they are our own and the build floater is open. Beyond LL (its
            // left click / hover never sees rigged mesh): a TOUCHABLE one is the item too,
            // so the hand cursor shows and the touch reaches it. Otherwise the WEARER.
            const bool as_item = pick_rigged || (wearer->isSelf() && LLFloater::isVisible(gFloaterTools));
            if (as_item || LLCleanCursorPick::touchable(obj))
            {
                if (!vo->vkPickFiltersPass(pick_unselectable)) return false;
                claim = as_item ? LLCleanCursorPick::CLAIM_ITEM : LLCleanCursorPick::CLAIM_TOUCH;
            }
            else
            {
                report_avatar = wearer;
                claim = LLCleanCursorPick::CLAIM_WEARER;
            }
        }
        else if (!vo->vkPickFiltersPass(pick_unselectable))
        {
            return false;   // animesh: the object itself, under LL's object filters
        }
    }
    if (report_avatar)
    {
        if (report_avatar->isControlAvatar()) return false;
        if (report_avatar->isSelf() && !gAgent.needsRenderAvatar()) return false;
        out.obj = report_avatar;
        out.face = -1;
    }
    else
    {
        LLVOAvatar* av = obj->getAvatar();
        if (av && av->isSelf() && !gAgent.needsRenderAvatar()) return false;
        out.obj = obj;
        out.face = face;
    }
    out.claim = claim;
    out.pos.load3(f.pos);
    out.normal.load3(f.normal);
    out.normal.normalize3fast();
    out.tangent.loadua(f.tangent);
    out.st.set(f.st[0], f.st[1]);
    return true;
}

// Fragments sorted by distance along their ray.
std::vector<const LLVKPickFrag*> sorted(const std::vector<LLVKPickFrag>& v)
{
    std::vector<const LLVKPickFrag*> o;
    o.reserve(v.size());
    for (const LLVKPickFrag& f : v) o.push_back(&f);
    std::sort(o.begin(), o.end(), [](const LLVKPickFrag* a, const LLVKPickFrag* b) { return a->t < b->t; });
    return o;
}

// Distance t of point p along the ray (s, unit d), when it lies within `px` pixels of it
// (1 cm at least); < 0 otherwise.
F32 onRay(const F32* p, const LLVector3& s, const LLVector3& d, F32 pix, F32 px)
{
    const LLVector3 v = LLVector3(p) - s;
    const F32 t = v * d;
    if (t < 0.f) return -1.f;
    return (v - d * t).length() <= llmax(0.01f, px * pix * t) ? t : -1.f;
}

// The deepest histogram bin edge whose cumulative count fits the layer's list (at least one
// bin): clean_pick_window.comp's rule, for a layer that still overflowed.
F32 narrowedHi(const LLVKPickResult& r, bool hud)
{
    const U32 nb = hud ? LLVKPickResult::kHistHud : LLVKPickResult::kHistWorld;
    const U32 cap = hud ? 128u : 1024u;   // LLCleanPickVK::kHudFrags / kWorldFrags
    const U32* h = hud ? r.hist_hud : r.hist_world;
    U32 cum = 0, b = 0;
    for (; b < nb; ++b)
    {
        if (cum + h[b] > cap) break;
        cum += h[b];
    }
    b = llmax(b, 1u);
    const F32 lo = r.win[hud ? 2 : 0], hi = r.win[hud ? 3 : 1];
    // (LLCleanPickVK::binEdge)
    const F32 span = llmax(llmin(hi, 1.0e6f) - lo, 1.0e-3f);
    if (b >= nb) return hi;
    return lo + std::exp2((F32)b / (F32)nb * std::log2(1.f + span)) - 1.f;
}

// Walk one layer's fragments nearest first -- across depth windows when may_page (an
// exact answer: a click or a single-object pick) -- until visit() decides. On an
// overflowing layer the next request narrows the window; on an undecided one it asks for
// the next window. complete: every fragment of the ray up to its end was offered (false:
// a hover answer whose window ended undecided, or the page budget ran out).
// visit(f, t): t along the CURRENT segment (s, d); fragments off [0, len] are skipped.
bool walkLayer(bool hud, LLVKPickResult& r, bool may_page, const LLVector3& s, const LLVector3& d, F32 len,
               const std::function<bool(const LLVKPickFrag&, F32)>& visit, bool& complete)
{
    complete = false;
    // A full RIGGED list (LLVKPickResult::cand_overflow_rigged) pages like a static one while
    // narrowing helps (a bounded crowd: exact). It is accepted as a partial rigged layer --
    // statics complete, the drawn rigged subset walked -- once narrowing can't help: a hover
    // (never pages), a narrowed page whose rigged count barely fell (wearers with unknown /
    // absurd boxes pass every window), or the last page. Voiding the layer instead left
    // nothing clickable after a teleport until the arriving wearers' boxes settled.
    bool rig_stuck = !may_page;
    bool rig_paged = false;   // the previous page narrowed for the rigged list alone
    U32 rig_prev = 0;
    for (U32 page = 0; page < kMaxPages; ++page)
    {
        // Overflow: more fragments than the list (the histogram says where to cut), or more
        // candidates than the select's lists (never drawn: no histogram -- shrink the window).
        const bool frag_ovf = hud ? r.overflow_hud : r.overflow_world;
        const bool static_ovf = hud ? r.cand_overflow_hud : r.cand_overflow_world;
        const bool rig_ovf = !hud && r.cand_overflow_rigged;
        if (rig_ovf && rig_paged && (U64)r.cand_rigged * 10u >= (U64)rig_prev * 9u) rig_stuck = true;
        if (page + 1 == kMaxPages) rig_stuck = true;
        const bool cand_ovf = static_ovf || (rig_ovf && !rig_stuck);
        const bool ovf = frag_ovf || cand_ovf;
        const F32 lo = r.win[hud ? 2 : 0], hi = r.win[hud ? 3 : 1];
        if (!ovf)
        {
            for (const LLVKPickFrag* f : sorted(hud ? r.hud : r.world))
            {
                if (f->t < lo || f->t >= hi) continue;
                const F32 t = (LLVector3(f->pos) - s) * d;
                if (t < 0.f || t > len) continue;
                if (visit(*f, t)) return true;
            }
            if (hi >= (hud ? r.req.hud_length : r.req.length))
            {
                complete = true;
                return false;
            }
        }
        if (!may_page) return false;
        // The next request on the same ray: a narrower window, or the next one; the
        // other layer is not drawn.
        LLVKPickRequest q = r.req;
        q.serial = nextSerial();
        const F32 ray_len = hud ? r.req.hud_length : r.req.length;
        F32 nlo = lo, nhi = ray_len;
        if (frag_ovf) nhi = narrowedHi(r, hud);
        else if (cand_ovf) nhi = llmax(lo, 0.f) + 0.25f * (llmin(hi, ray_len) - llmax(lo, 0.f));
        else nlo = hi;
        rig_paged = !frag_ovf && !static_ovf && cand_ovf;   // (cand_ovf here: the rigged list)
        rig_prev = r.cand_rigged;
        if (hud)
        {
            q.win[0] = kNoWin[0]; q.win[1] = kNoWin[1];
            q.win[2] = nlo; q.win[3] = nhi;
        }
        else
        {
            q.hud = false;
            q.win[0] = nlo; q.win[1] = nhi;
        }
        ++sStats.pages;
        LLVKPickResult nr;
        if (!askNow(q, nr)) return false;
        r = std::move(nr);
    }
    return false;
}

// A pick's face is the renderer's instance face; LL's pick consumers index the object's
// TEs with it (LLPickInfo::updateXYCoords, media, build tools). A face with no TE -- more
// volume faces than TEs, or an object rebuilt between the frame that drew it and this
// answer -- reports as no face (-1) instead of a null TE (crash in updateXYCoords,
// 2026-10-13). glTF objects report TE 0 and are left alone.
S32 teFace(const LLViewerObject* obj, S32 face)
{
    if (!obj || face < 0 || obj->mGLTFAsset || face < obj->getNumTEs()) return face;
    static U32 s_count = 0;
    if (s_count++ % 256 == 0)
        LL_WARNS("VKPick") << "pick face " << face << " of " << obj->getID() << " has no TE (TEs "
                           << (S32)obj->getNumTEs() << "): reported as no face (" << s_count << " so far)" << LL_ENDL;
    return -1;
}

void setSurface(const LLVKPickResult& r, LLViewerObject* obj, S32 face, S32 node, S32 prim,
                const LLVKPickFrag* f, const LLVector4a& pos, const LLVector4a& nrm, const LLVector4a& tng,
                const LLVector2& st, bool from_all = false, S32 x = -1, S32 y = -1)
{
    sSurface.valid = obj != nullptr;
    if (!obj) return;
    sSurface.from_all = from_all;
    sSurface.x = x;
    sSurface.y = y;
    sSurface.id = obj->getID();
    sSurface.face = face;
    sSurface.node = node;
    sSurface.prim = prim;
    sSurface.pos = pos;
    sSurface.normal = nrm;
    sSurface.tangent = tng;
    sSurface.st = st;
    sSurface.has_uv = f != nullptr;
    if (f)
    {
        sSurface.uv.set(f->uv[0], f->uv[1]);
        std::memcpy(sSurface.duv, f->duv, sizeof(sSurface.duv));
    }
    sSurface.serial = r.req.serial;
}

// A glTF scene object's (node, primitive) for its instance face (else -1, -1).
void gltfIds(LLViewerObject* obj, S32 face, S32& node, S32& prim)
{
    node = prim = -1;
    if (obj && obj->mGLTFAsset) LLCleanGLTFIngest::instance().instanceNodePrim(obj->getID(), face, node, prim);
}

void report()
{
    const F64 now = LLTimer::getTotalSeconds();
    if (now < sStatsNext) return;
    const bool first = sStatsNext == 0.0;
    sStatsNext = now + 1.0;
    if (first || !debugOn())
    {
        sStats = Stats{};
        return;
    }
    LL_INFOS("VKPick") << "pick: exact " << sStats.exact << " / trailing " << sStats.trail << " / none "
                       << sStats.none << " / answered now " << sStats.now << " (pages " << sStats.pages << ") per s"
                       << LL_ENDL;
    sStats = Stats{};
}

// The pick latency line (every 10 s, only when a click was held or a pick answered now).
void reportLatency()
{
    const F64 now = LLTimer::getTotalSeconds();
    if (now < sLatNext) return;
    const bool first = sLatNext == 0.0;
    sLatNext = now + 10.0;
    if (!first && (sLat.held || sLat.now))
    {
        char b[160];
        std::snprintf(b, sizeof(b), "%u clicks held (avg %.1f, max %u frames; %u ran out), %u answered now (avg %.1f, max %.1f ms)",
                      sLat.held, sLat.held ? (double)sLat.heldFrames / sLat.held : 0.0, sLat.heldMax, sLat.expired, sLat.now,
                      sLat.now ? sLat.nowMs / sLat.now : 0.0, sLat.nowMax);
        LL_INFOS("VKPick") << "pick latency (10 s): " << b << (asyncClickOn() ? "" : " [RenderVKPickAsyncClick off]") << LL_ENDL;
    }
    sLat = Latency{};
}
} // namespace

namespace LLCleanCursorPick
{

std::string healthSummary()
{
    LLVulkanContext& vk = LLVulkanContext::instance();
    std::string s = vk.pickGpuAvailable() ? std::string("GPU pick available")
                                          : std::string("GPU pick UNAVAILABLE: ") + vk.pickFailure();
    s += sViews.valid ? "; frame projection ok" : "; NO frame projection";
    s += gAgent.getTeleportState() != LLAgent::TELEPORT_NONE
             ? "; teleport state " + std::to_string((S32)gAgent.getTeleportState()) : std::string();
    s += sDiagBad ? "; outstanding pick problem: " + sDiagLast : std::string("; no pick problem outstanding");
    s += "; object clicks " + std::to_string(sClicks) + " (no GPU answer " + std::to_string(sClickNone)
         + ", hit nothing " + std::to_string(sClickMiss) + ")";
    return s;
}

void noteProblem(const std::string& why)
{
    diagNote(why);
}

bool gpuEnabled()
{
    // (RenderVKPickGPU is retired: the GPU pick is the only one, #704.)
    return LLVulkanContext::instance().isInitialized();
}

bool alphaBypass()
{
    // LLViewerWindow::pickImmediate / pickAsync's pick_transparent override.
    if (LLPipeline::sShowDebugAlpha) return true;
    static LLCachedControl<bool> select_invisible(gSavedSettings, "SelectInvisibleObjects", true);
    return select_invisible && LLFloaterReg::instanceVisible("build");
}

F32 alphaBlendThreshold()
{
    static LLCachedControl<F32> s_thresh(gSavedSettings, "PickAlphaThreshold", 0.1f);
    return llclamp((F32)s_thresh, 0.f, 1.f);
}

bool testsAlpha(const LLViewerObject* obj, bool pick_transparent)
{
    if (!obj || alphaBypass()) return false;
    // HUDs keep LL's rule exactly: texels tested by the visible pick only.
    if (obj->isHUDAttachment()) return !pick_transparent;
    if (pick_transparent)
    {
        // Invisible click zones: LLToolPie's "transparent actionable" priority looks at
        // the prim AND its root (script in the root, transparent-textured child prims).
        if (obj->specialHoverCursor()) return false;
        const LLViewerObject* root = obj->getRootEdit();
        if (root && root != obj && root->specialHoverCursor()) return false;
    }
    return !obj->isHiglightedOrBeacon();
}

ClickScope::ClickScope(bool active) : mActive(active)
{
    if (mActive) ++sClickDepth;
}

ClickScope::~ClickScope()
{
    if (!mActive) return;
    if (--sClickDepth < 0) sClickDepth = 0;
}

bool inClick()
{
    return sClickDepth > 0;
}

bool holdClick(S32 mouse_x, S32 mouse_y)
{
    reportLatency();
    if (!asyncClickOn() || !gpuEnabled() || gAgent.getTeleportState() != LLAgent::TELEPORT_NONE) return false;
    LLVulkanContext& vk = LLVulkanContext::instance();
    if (!vk.pickGpuAvailable()) return false;
    if (sHeld.active && (sHeld.x != mouse_x || sHeld.y != mouse_y)) sHeld = HeldClick{};   // (another click)
    LLVKPickResult r;
    bool have = vk.latestPick(r) && r.valid;
    if (!sHeld.active)
    {
        // an answer covering this pixel's ray already (the per-frame request had it): fetch now
        RayQ q;
        if (cursorRay(mouse_x, mouse_y, q) && have && recent(r) && covers(r, q, pixelAngle()) && (!hudShown() || r.req.hud))
            return false;
        sHeld = HeldClick{};
        sHeld.active = true;
        sHeld.x = mouse_x;
        sHeld.y = mouse_y;
        sHeld.start = (U32)gFrameCount;
        arm();
        return true;
    }
    if (sHeld.ready) return false;
    // the answer to a request made for it (fillRequest; the newest one of them) -- also from a
    // frame in flight that completed already
    if (sHeld.serial)
    {
        vk.pollPickAnswers();
        have = vk.latestPick(r) && r.valid;
    }
    if (have && sHeld.serial && (S32)(r.req.serial - sHeld.serial) >= 0 && (S32)(r.req.serial - sResetSerial) > 0
        && r.req.mouse_x == mouse_x && r.req.mouse_y == mouse_y && recent(r))
    {
        sHeld.ready = true;
        sHeld.s.set(r.req.origin);
        sHeld.d.set(r.req.dir);
        const U32 frames = (U32)gFrameCount - sHeld.start;
        ++sLat.held;
        sLat.heldFrames += frames;
        sLat.heldMax = llmax(sLat.heldMax, frames);
        return false;
    }
    if ((U32)gFrameCount - sHeld.start >= kMaxClickWaitFrames)
    {
        ++sLat.expired;   // (fetched now: answered between frames, as before)
        sHeld = HeldClick{};
        return false;
    }
    arm();
    return true;
}

void clickFetched()
{
    sHeld = HeldClick{};
}

bool gpuSkinned(const LLViewerObject* obj)
{
    if (!obj || obj->isDead() || obj->getPCode() != LL_PCODE_VOLUME || obj->isHUDAttachment()) return false;
    if (!obj->isRiggedMesh()) return false;
    // llcleaningest.cpp's routing: worn, or animesh whose control avatar is playing.
    if (obj->isAttachment()) return true;
    if (!obj->isAnimatedObject()) return false;
    const LLControlAvatar* cav = obj->getControlAvatar();
    return cav && cav->mPlaying;
}

bool touchable(const LLViewerObject* obj)
{
    if (!obj || obj->isDead() || obj->getClickAction() == CLICK_ACTION_DISABLED) return false;
    if (obj->flagHandleTouch()) return true;
    const LLViewerObject* root = obj->getRootEdit();
    return root && root != obj && root->flagHandleTouch();
}

void arm()
{
    sArmed = true;
    sArmedUntil = (U32)gFrameCount + kArmFrames;
}

void fillRequest(LLVKWorldFrame& wf)
{
    LLVKPickRequest& r = wf.pick;
    r = LLVKPickRequest{};
    // A teleport (or a camera cut the renderer resets history for) voids every answer
    // asked for before it.
    if (gAgent.getTeleportState() != LLAgent::TELEPORT_NONE || wf.temporal_reset)
    {
        sResetSerial = sSerial;
        sSurface.valid = false;
        sViews.valid = false;
        return;
    }
    if (!wf.enabled || !gViewerWindow) return;
    // The frame's projections, for rays asked for between frames (pickNow).
    {
        LLViewerCamera* cam = LLViewerCamera::getInstance();
        std::memcpy(sViews.view_proj_rel, wf.view_proj_rel, sizeof(sViews.view_proj_rel));
        std::memcpy(sViews.apex, cam->getOrigin().mV, sizeof(sViews.apex));
        std::memcpy(sViews.cam_at, cam->getAtAxis().mV, sizeof(sViews.cam_at));
        std::memcpy(sViews.cam_left, cam->getLeftAxis().mV, sizeof(sViews.cam_left));
        std::memcpy(sViews.cam_up, cam->getUpAxis().mV, sizeof(sViews.cam_up));
        sViews.rect[0] = sViews.rect[1] = 0;
        sViews.rect[2] = gViewerWindow->getWorldViewWidthRaw();
        sViews.rect[3] = gViewerWindow->getWorldViewHeightRaw();
        sViews.hud = wf.hud_enabled && hudShown();
        std::memcpy(sViews.hud_vp, wf.hud_view_proj, sizeof(sViews.hud_vp));
        std::memcpy(sViews.hud_viewport, wf.hud_viewport, sizeof(sViews.hud_viewport));
        static LLCachedControl<bool> s_prepass(gSavedSettings, "RenderVKDepthPrepass", true);
        sViews.mode = s_prepass ? LLVKPickRequest::MODE_PREPASS : 0u;
        sViews.valid = true;
    }
    // Armed by recent picks (LLToolPie's hover picks every frame) -- and always while the
    // mouse is over the window: the build / edit tools don't hover-pick, and a click then
    // has an answer in flight (a click no answer covers is answered between frames).
    if (gViewerWindow->getMouseInWindow() || gAgentCamera.cameraMouselook() || sVRRay.on) arm();
    if (!sArmed || (S32)(sArmedUntil - (U32)gFrameCount) < 0)
    {
        sArmed = false;
        return;
    }

    // The cursor (mouselook: the crosshair, as LLToolPie picks there) and its ray -- or, while a
    // click's async pick is held (holdClick), that click's pixel.
    RayQ q;
    const bool ml = gAgentCamera.cameraMouselook();
    const bool held = sHeld.active && !sHeld.ready;
    if (!cursorRay(held ? sHeld.x : ml ? gViewerWindow->getWorldViewRectScaled().getWidth() / 2 : gViewerWindow->getCurrentMouseX(),
                   held ? sHeld.y : ml ? gViewerWindow->getWorldViewRectScaled().getHeight() / 2 : gViewerWindow->getCurrentMouseY(), q))
        return;
    if (sViews.hud && !sVRRay.on)   // (#1076: the VR pointer never picks HUDs: they are on the UI wall)
    {
        const LLVector3 p = gViewerWindow->mousePointHUD(q.x, q.y);
        q.hud = true;
        q.hs = p - LLVector3(kHudDepth, 0.f, 0.f);
        q.hd.set(1.f, 0.f, 0.f);
        q.hlen = 2.f * kHudDepth;
    }
    if (makeRequest(q, r) && held && !sHeld.serial) sHeld.serial = r.serial;
}

Source intersectAll(S32 mouse_x, S32 mouse_y,
                    const LLVector4a& mw_start, const LLVector4a& mw_end,
                    const LLVector4a& mh_start, const LLVector4a& mh_end, bool hud_blocked,
                    bool pick_transparent, bool pick_rigged, bool pick_unselectable,
                    bool pick_reflection_probe, LLViewerObject*& found,
                    S32* face_hit, S32* gltf_node, S32* gltf_prim,
                    LLVector4a* intersection, LLVector2* uv,
                    LLVector4a* normal, LLVector4a* tangent)
{
    VK_HITCH_SCOPE("pick_intersectAll");
    found = nullptr;
    report();
    reportLatency();
    if (!gpuEnabled()) return SRC_NONE;
    arm();   // keep the per-frame request going while picks keep asking
    RayQ q;
    q.x = mouse_x;
    q.y = mouse_y;
    q.s.set(mw_start.getF32ptr());
    q.d = LLVector3(mw_end.getF32ptr()) - q.s;
    q.len = q.d.normVec();
    if (q.len < 1e-4f) return SRC_NONE;
    // A held click's fetch (holdClick): its ray is the one its answer was asked for -- the
    // click's pixel through the camera of the frame after the click, the ray LL's async pick
    // always used (the camera may have moved on since).
    if (inClick() && sHeld.active && sHeld.ready && sHeld.x == mouse_x && sHeld.y == mouse_y)
    {
        q.s = sHeld.s;
        q.d = sHeld.d;
    }
    q.hud = hudShown() && !hud_blocked;
    if (q.hud)
    {
        q.hs.set(mh_start.getF32ptr());
        q.hd = LLVector3(mh_end.getF32ptr()) - q.hs;
        q.hlen = q.hd.normVec();
        q.hud = q.hlen > 1e-4f;
    }
    const bool click = inClick();

    LLVKPickResult r;
    bool exact = false;
    std::string no_answer;
    const bool have = answerFor(q, click, r, exact, &no_answer);
    const bool dbg = debugOn();
    static F64 s_dbg_next = 0.0;
    const bool log_this = dbg && (click || LLTimer::getTotalSeconds() >= s_dbg_next);
    if (log_this && !click) s_dbg_next = LLTimer::getTotalSeconds() + 0.25;
    if (!have)
    {
        ++sStats.none;
        sSurface.valid = false;
        if (click)
        {
            ++sClicks;
            ++sClickNone;
            diagNote("click got no GPU answer -- " + no_answer);
        }
        if (log_this)
            LL_INFOS("VKPick") << "pick " << (click ? "click" : "hover") << " -> no answer" << LL_ENDL;
        return SRC_NONE;
    }
    const Source src = exact ? SRC_EXACT : SRC_TRAIL;
    ++(exact ? sStats.exact : sStats.trail);
    const U32 serial0 = r.req.serial;
    const U32 cand0[3] = {r.cand_static, r.cand_rigged, r.cand_hud};
    if (r.cand_overflow_rigged)
        diagNote("rigged candidate list full (" + std::to_string(r.cand_rigged)
                 + " wearer draws cross the ray; unknown / huge wearer boxes, e.g. attachments still rezzing"
                   " after a teleport, pass every window) -- narrowed while that helps, else a partial rigged"
                   " layer is walked (statics and avatar volumes still picked)");
    const size_t frags0[2] = {r.world.size(), r.hud.size()};

    auto emit = [&](LLViewerObject* obj, S32 face, S32 node, S32 prim, const LLVKPickFrag* f,
                    const LLVector4a& pos, const LLVector2& st, const LLVector4a& nrm, const LLVector4a& tng)
    {
        found = obj;
        face = teFace(obj, face);
        if (face_hit)     *face_hit = face;
        if (gltf_node)    *gltf_node = node;
        if (gltf_prim)    *gltf_prim = prim;
        if (intersection) *intersection = pos;
        if (uv)           *uv = st;
        if (normal)       *normal = nrm;
        if (tangent)      *tangent = tng;
        setSurface(r, obj, face, node, prim, f, pos, nrm, tng, st, /*from_all=*/true, mouse_x, mouse_y);
    };
    auto fragOut = [](const LLVKPickFrag& f, LLVector4a& pos, LLVector4a& nrm, LLVector4a& tng, LLVector2& st)
    {
        pos.load3(f.pos);
        nrm.load3(f.normal);
        nrm.normalize3fast();
        tng.loadua(f.tangent);
        st.set(f.st[0], f.st[1]);
    };

    // HUD layer first: its nearest fragment that passes LL's rules (LL's HUD pick: the
    // object filters with pick_unselectable, LL's HUD alpha rule) wins over the world.
    if (q.hud && r.req.hud)
    {
        LLVKPickResult r0 = r;
        LLVKPickFrag hf;
        LLViewerObject* hobj = nullptr;
        S32 hface = -1;
        bool hud_complete = false;
        const bool hit = walkLayer(true, r, exact, q.hs, q.hd, q.hlen,
            [&](const LLVKPickFrag& f, F32) -> bool
            {
                LLViewerObject* obj = nullptr;
                S32 face = -1;
                if (!fragObject(f, obj, face) || !obj->isHUDAttachment() || obj->getPCode() != LL_PCODE_VOLUME) return false;
                if (!staticAccept(obj, face, f, pick_transparent, true)) return false;
                hf = f; hobj = obj; hface = face;
                return true;
            }, hud_complete);
        if (hit)
        {
            LLVector4a pos, nrm, tng;
            LLVector2 st;
            fragOut(hf, pos, nrm, tng, st);
            S32 node, prim;
            gltfIds(hobj, hface, node, prim);
            emit(hobj, hface, node, prim, &hf, pos, st, nrm, tng);
            if (log_this)
                LL_INFOS("VKPick") << "pick " << (click ? "click" : "hover") << " -> " << (exact ? "exact" : "trailing")
                                   << " HUD obj=" << hobj->getID() << " face=" << hface << " frags=" << frags0[1]
                                   << " cand_hud=" << cand0[2] << LL_ENDL;
            if (click)
            {
                ++sClicks;
                diagOk();
            }
            return src;
        }
        // (A HUD walk that paged replaced r with HUD-only answers: back to the first one.)
        if (r.req.serial != serial0) r = std::move(r0);
    }

    // World layer, nearest first. The first non-skinned fragment LL's rules accept ends the
    // walk; the nearest GPU-skinned one resolved before it competes under the claims.
    LLVKPickFrag sf;
    bool have_static = false;
    LLViewerObject* sobj = nullptr;
    S32 sface = -1;
    F32 st_t = q.len;
    Hit rig;
    LLVKPickFrag rig_frag;
    bool have_rig = false;
    F32 rig_t = q.len;
    bool world_complete = false;
    U32 n_offered = 0, n_unresolved = 0;
    walkLayer(false, r, exact, q.s, q.d, q.len,
        [&](const LLVKPickFrag& f, F32 t) -> bool
        {
            LLViewerObject* obj = nullptr;
            S32 face = -1;
            ++n_offered;
            if (!fragObject(f, obj, face))
            {
                ++n_unresolved;
                return false;
            }
            if (obj->isAvatar() || gpuSkinned(obj))
            {
                if (have_rig) return false;
                // The raster's alpha rule (hit-everything picks: any surface).
                if (!(f.flags & LLVKPickFrag::ALPHA_PASS) && !alphaBypass()) return false;
                Hit h;
                if (resolveRig(f, obj, face, pick_rigged, pick_unselectable, h))
                {
                    rig = h;
                    rig_frag = f;
                    have_rig = true;
                    rig_t = t;
                }
                return false;
            }
            if (!pick_reflection_probe && obj->isReflectionProbe()) return false;
            if (!staticAccept(obj, face, f, pick_transparent, pick_unselectable)) return false;
            sf = f; sobj = obj; sface = face; st_t = t;
            have_static = true;
            return true;
        }, world_complete);

    LLViewerObject* hit_obj = nullptr;
    S32 hit_face = -1, hit_node = -1, hit_prim = -1;
    const LLVKPickFrag* hit_frag = nullptr;
    LLVector4a hit_pos, hit_nrm, hit_tng;
    hit_pos.clear(); hit_nrm.clear(); hit_tng.clear();
    LLVector2 hit_st;
    const char* claim = "static";
    bool wearer_mesh = false;
    if (have_static)
    {
        hit_obj = sobj;
        hit_face = sface;
        hit_frag = &sf;
        fragOut(sf, hit_pos, hit_nrm, hit_tng, hit_st);
        gltfIds(sobj, sface, hit_node, hit_prim);
        if (sobj->mGLTFAsset) hit_face = 0;   // (LL's glTF pick reports TE 0 with the node / primitive)
    }
    if (have_rig && (!have_static || rig_t < st_t))
    {
        // LL PRECEDENCE vs a rigid attachment behind the rigged surface: a WEARER claim
        // never hides it, a TOUCH claim only hides a non-touchable one (42e0d982c0).
        const bool keep_attachment = sobj && have_static && sobj->isAttachment()
            && (rig.claim == CLAIM_WEARER || (rig.claim == CLAIM_TOUCH && touchable(sobj)));
        if (!keep_attachment)
        {
            hit_obj = rig.obj;
            hit_face = rig.face;
            hit_node = hit_prim = -1;
            hit_frag = &rig_frag;
            hit_pos = rig.pos; hit_nrm = rig.normal; hit_tng = rig.tangent; hit_st = rig.st;
            claim = rig.claim == CLAIM_ITEM ? "rigged item" : rig.claim == CLAIM_TOUCH ? "rigged touch" : "rigged wearer";
            wearer_mesh = rig.claim == CLAIM_WEARER;
        }
    }
    // AVATARS: the collision volumes (LL's pickable avatar shape) with LL's precedence --
    // the avatar wins unless the current hit is an attachment within
    // ATTACHMENT_OVERRIDE_DIST of it. Along the current segment, up to the hit.
    LLVector4a local_end = mw_end;
    if (hit_obj)
    {
        // (the hit on the current ray: a trailing answer's point projected onto it)
        const F32 t = llclamp(((LLVector3(hit_pos.getF32ptr()) - q.s) * q.d), 0.f, q.len);
        local_end.load3((q.s + q.d * t).mV);
    }
    S32 av_face = hit_face;
    LLVector4a av_pos = hit_pos, av_nrm = hit_nrm, av_tng = hit_tng;
    LLVector2 av_st = hit_st;
    LLViewerObject* av_obj = hit_obj;
    if (LLCleanInstanceStore::intersectAvatars(mw_start, local_end, av_obj, pick_transparent, pick_rigged,
                                               pick_unselectable, &av_face, &av_pos, &av_st, &av_nrm, &av_tng))
    {
        // A volume of the very avatar the drawn mesh already resolved to (WEARER): keep
        // the drawn surface -- the volumes stick out of the body.
        if (!(wearer_mesh && av_obj == hit_obj))
        {
            hit_obj = av_obj; hit_face = av_face; hit_pos = av_pos; hit_nrm = av_nrm; hit_tng = av_tng; hit_st = av_st;
            hit_node = hit_prim = -1;
            hit_frag = nullptr;
            claim = "avatar volumes";
        }
    }
    if (hit_obj) emit(hit_obj, hit_face, hit_node, hit_prim, hit_frag, hit_pos, hit_st, hit_nrm, hit_tng);
    else sSurface.valid = false;
    if (click)
    {
        ++sClicks;
        if (!hit_obj) ++sClickMiss;
        if (hit_obj)
            diagOk();
        else if (!world_complete && (r.overflow_world || r.cand_overflow_world))
            diagNote("click hit nothing: world candidates / fragments still overflowed after paging (static cand "
                     + std::to_string(r.cand_static) + ", frags " + std::to_string(r.n_world) + ", window "
                     + std::to_string(r.win[0]) + ".." + std::to_string(r.win[1]) + ")");
        else if (n_unresolved > 0)
            diagNote("click hit nothing: " + std::to_string(n_unresolved) + " of " + std::to_string(n_offered)
                     + " fragments on the ray name instances with no live object (released / dead since drawn)");
    }
    if (log_this)
    {
        U32 pass = 0;
        for (const LLVKPickFrag& f : r.world) pass += (f.flags & LLVKPickFrag::ALPHA_PASS) ? 1u : 0u;
        LL_INFOS("VKPick") << "pick " << (click ? "click" : "hover") << " -> " << (exact ? "exact" : "trailing")
                           << " age " << (sSerial - serial0) << " cand " << cand0[0] << "/" << cand0[1]
                           << "/" << cand0[2] << " frags " << frags0[0] << " hud " << frags0[1]
                           << (r.req.serial != serial0 ? " (paged: window " + std::to_string(r.win[0]) + ".."
                                                             + std::to_string(r.win[1]) + ", " + std::to_string(r.world.size())
                                                             + " frags, " + std::to_string(pass) + " alpha-pass)"
                                                       : std::string())
                           << (world_complete ? "" : " [window undecided]") << " => "
                           << (hit_obj ? hit_obj->getID().asString() + " face " + std::to_string(hit_face) + " (" + claim + ")"
                                       : std::string("nothing"))
                           << LL_ENDL;
    }
    return src;
}

bool intersectObject(LLViewerObject* obj, S32 mouse_x, S32 mouse_y,
                     const LLVector4a& start, const LLVector4a& end, S32 face,
                     bool pick_transparent, bool pick_unselectable,
                     S32* face_hit, S32* gltf_node, S32* gltf_prim, LLVector4a* intersection,
                     LLVector2* tex_coord, LLVector4a* normal, LLVector4a* tangent)
{
    VK_HITCH_SCOPE("pick_object");
    if (!obj || obj->isDead() || !gpuEnabled()) return false;
    const bool hud = obj->isHUDAttachment();
    LLVector3 s(start.getF32ptr());
    LLVector3 d = LLVector3(end.getF32ptr()) - s;
    const F32 len = d.normVec();
    if (len < 1e-4f) return false;
    const F32 pix = pixelAngle();
    auto out = [&](S32 f, S32 node, S32 prim, const LLVector4a& p, const LLVector2& st, const LLVector4a& n,
                   const LLVector4a& tn)
    {
        if (face_hit)     *face_hit = f;
        if (gltf_node)    *gltf_node = node;
        if (gltf_prim)    *gltf_prim = prim;
        if (intersection) *intersection = p;
        if (tex_coord)    *tex_coord = st;
        if (normal)       *normal = n;
        if (tangent)      *tangent = tn;
        return true;
    };
    // The surface the pick that chose this object reported (getSurfaceInfo right after it).
    // A click's is on this very ray; a hover's trails the cursor by a frame or two and is
    // taken as is (a hover never waits for the GPU).
    const bool click = inClick();
    if (sSurface.valid && sSurface.from_all && sSurface.x == mouse_x && sSurface.y == mouse_y
        && sSurface.id == obj->getID() && (face < 0 || face == sSurface.face)
        && (U32)(sSerial - sSurface.serial) < kSerialRing
        && (!click || onRay(sSurface.pos.getF32ptr(), s, d, hud ? 0.f : pix, hud ? 0.f : kNearPx) >= 0.f))
        return out(sSurface.face, sSurface.node, sSurface.prim, sSurface.pos, sSurface.st, sSurface.normal,
                   sSurface.tangent);
    if (!LLVulkanContext::instance().pickGpuAvailable()) return false;

    // An answer for this ray: a click's exact (answered now when no recent one covers it),
    // a hover's the newest one.
    RayQ q;
    q.x = mouse_x;
    q.y = mouse_y;
    if (hud)
    {
        // The world ray for the request (the cursor's), the HUD segment the one asked for.
        if (!cursorRay(mouse_x, mouse_y, q)) return false;
        q.hud = true;
        q.hs = s; q.hd = d; q.hlen = len;
    }
    else
    {
        q.s = s; q.d = d; q.len = len;
    }
    LLVKPickResult r;
    bool exact = false;
    if (!answerFor(q, click, r, exact)) return false;

    const LLUUID& id = obj->getID();
    const bool skinned = obj->isAvatar() || gpuSkinned(obj);
    LLVKPickFrag hf;
    S32 hface = -1;
    bool complete = false;
    const bool hit = walkLayer(hud, r, exact, s, d, len,
        [&](const LLVKPickFrag& f, F32) -> bool
        {
            LLUUID fid;
            S32 ff = -1;
            if (!LLCleanInstanceStore::instance().keyForId(f.instance, fid, ff) || fid != id) return false;
            if (face >= 0 && ff != face) return false;
            if (skinned)
            {
                if (!(f.flags & LLVKPickFrag::ALPHA_PASS) && !alphaBypass()) return false;
            }
            else if (!staticAccept(obj, ff, f, pick_transparent, pick_unselectable))
                return false;
            hf = f;
            hface = ff;
            return true;
        }, complete);
    if (!hit) return false;
    LLVector4a p, n, tn;
    p.load3(hf.pos);
    n.load3(hf.normal);
    n.normalize3fast();
    tn.loadua(hf.tangent);
    const LLVector2 st(hf.st[0], hf.st[1]);
    S32 node, prim;
    gltfIds(obj, hface, node, prim);
    const S32 rface = obj->mGLTFAsset ? 0 : teFace(obj, hface);
    setSurface(r, obj, rface, node, prim, &hf, p, n, tn, st);
    return out(rface, node, prim, p, st, n, tn);
}

Focus focusPoint(LLVector3& pos)
{
    if (!gpuEnabled()) return FOCUS_UNKNOWN;
    LLVKPickResult r;
    if (!LLVulkanContext::instance().latestPick(r) || !recent(r)) return FOCUS_UNKNOWN;
    // (The newest is always a per-frame cursor request: answers asked for between frames
    // go to their caller only.) An overflowing layer hands out no fragments; a window the
    // GPU narrowed holds the nearest ones.
    if (r.overflow_world || r.cand_overflow_world) return FOCUS_UNKNOWN;
    const F32 lo = r.win[0], hi = r.win[1];
    for (const LLVKPickFrag* f : sorted(r.world))
    {
        if (f->t < lo || f->t >= hi) continue;
        if (!(f->flags & LLVKPickFrag::ALPHA_PASS)) continue;   // what the raster keeps
        // (The position is all DoF needs: a fragment whose instance no longer resolves --
        // released since the answer was drawn -- still marks drawn geometry.)
        LLViewerObject* obj = nullptr;
        S32 face = -1;
        if (!fragObject(*f, obj, face))
        {
            pos.set(f->pos);
            return FOCUS_HIT;
        }
        if (obj->isReflectionProbe()) continue;
        // Our own avatar / attachments the main view doesn't draw (mouselook, #554).
        const LLVOAvatar* av = obj->isAvatar() ? (const LLVOAvatar*)obj : obj->getAvatar();
        if (av && av->isSelf() && !gAgent.needsRenderAvatar()) continue;
        if (LLVOAvatar::isHiddenOwnAttachment(obj)) continue;
        if (obj->mDrawable.notNull() && !gPipeline.hasRenderType(obj->mDrawable->getRenderType())) continue;
        if (obj->getPCode() == LL_PCODE_VOLUME && !obj->mGLTFAsset && face >= 0 && face < obj->getNumTEs())
        {
            const LLTextureEntry* te = obj->getTE((U8)face);
            if (te && te->getColor().mV[3] == 0.f) continue;   // completely transparent face
        }
        pos.set(f->pos);
        return FOCUS_HIT;
    }
    return hi >= r.req.length ? FOCUS_NONE : FOCUS_UNKNOWN;
}

void setVRRay(const LLVector3& origin, const LLVector3& dir)
{
    LLVector3 d = dir;
    if (d.normVec() < 1e-6f || !origin.isFinite() || !d.isFinite())
    {
        sVRRay.on = false;
        return;
    }
    sVRRay.on = true;
    sVRRay.s = origin;
    sVRRay.d = d;
}

void clearVRRay()
{
    sVRRay.on = false;
}

bool vrRay(LLVector3& origin, LLVector3& dir)
{
    if (!sVRRay.on) return false;
    origin = sVRRay.s;
    dir = sVRRay.d;
    return true;
}

bool vrRayOn()
{
    return sVRRay.on;
}

Focus vrRayHit(LLVector3& pos)
{
    if (!sVRRay.on || !gpuEnabled()) return FOCUS_UNKNOWN;
    LLVKPickResult r;
    if (!LLVulkanContext::instance().latestPick(r) || !recent(r)) return FOCUS_UNKNOWN;
    // The newest per-frame answer trails the controller by a frame or two: accept one asked for
    // a ray within 10 cm / ~3 degrees of this one (the beam's length only), not a camera ray
    // from before the pointer took over.
    if ((LLVector3(r.req.origin) - sVRRay.s).length() > 0.1f || LLVector3(r.req.dir) * sVRRay.d < 0.9986f)
        return FOCUS_UNKNOWN;
    if (r.overflow_world || r.cand_overflow_world) return FOCUS_UNKNOWN;
    const F32 lo = r.win[0], hi = r.win[1];
    for (const LLVKPickFrag* f : sorted(r.world))
    {
        if (f->t < lo || f->t >= hi) continue;
        if (!(f->flags & LLVKPickFrag::ALPHA_PASS)) continue;
        LLViewerObject* obj = nullptr;
        S32 face = -1;
        if (!fragObject(*f, obj, face)) continue;   // (the hands / beam: rows of no object)
        if (obj->isReflectionProbe()) continue;
        const LLVOAvatar* av = obj->isAvatar() ? (const LLVOAvatar*)obj : obj->getAvatar();
        if (av && av->isSelf() && !gAgent.needsRenderAvatar()) continue;
        if (LLVOAvatar::isHiddenOwnAttachment(obj)) continue;
        if (obj->mDrawable.notNull() && !gPipeline.hasRenderType(obj->mDrawable->getRenderType())) continue;
        if (obj->getPCode() == LL_PCODE_VOLUME && !obj->mGLTFAsset && face >= 0 && face < obj->getNumTEs())
        {
            const LLTextureEntry* te = obj->getTE((U8)face);
            if (te && te->getColor().mV[3] == 0.f) continue;
        }
        pos.set(f->pos);
        return FOCUS_HIT;
    }
    return hi >= r.req.length ? FOCUS_NONE : FOCUS_UNKNOWN;
}

bool surfaceTexture(const LLViewerObject* obj, S32 face, LLVector2& uv, F32* duv_px)
{
    if (!obj || !sSurface.valid || !sSurface.has_uv || sSurface.id != obj->getID()
        || (face >= 0 && face != sSurface.face) || (U32)(sSerial - sSurface.serial) >= kSerialRing)
        return false;
    uv = sSurface.uv;
    if (duv_px) std::memcpy(duv_px, sSurface.duv, sizeof(sSurface.duv));
    return true;
}

bool drawnFaceFrame(const LLViewerObject* obj, S32 face, LLVector4a* normal_sum,
                    LLVector4a* normal0, LLVector4a* tangent0)
{
    if (!obj || face < 0 || !gpuEnabled()) return false;
    U32 id = 0;
    if (!LLCleanInstanceStore::instance().findInstanceId(obj->getID(), face, id)) return false;
    std::vector<LLVKFaceQueryResult> res;
    if (!LLVulkanContext::instance().faceQuery(std::vector<uint32_t>{id}, nullptr, res) || res.empty() || !res[0].valid)
        return false;
    const LLVKFaceQueryResult& q = res[0];
    if (normal_sum) normal_sum->set(q.normal_sum[0], q.normal_sum[1], q.normal_sum[2], 0.f);
    if (normal0) normal0->set(q.normal0[0], q.normal0[1], q.normal0[2], 0.f);
    if (tangent0) tangent0->set(q.tangent0[0], q.tangent0[1], q.tangent0[2], q.tangent0[3]);
    return true;
}

bool drawnRectTest(const std::vector<LLViewerObject*>& objs, const LLVKRectQuery& rect,
                   std::vector<U8>& any_in, std::vector<U8>& any_out)
{
    any_in.assign(objs.size(), 0);
    any_out.assign(objs.size(), 0);
    if (objs.empty() || !gpuEnabled()) return false;
    // One job per drawn face instance; owner[k] = the object it belongs to.
    std::vector<uint32_t> ids;
    std::vector<U32> owner;
    LLCleanInstanceStore& is = LLCleanInstanceStore::instance();
    for (size_t i = 0; i < objs.size(); ++i)
    {
        LLViewerObject* o = objs[i];
        if (!o || o->isDead()) continue;
        const S32 nf = llmax((S32)o->getNumTEs(), o->getVolume() ? o->getVolume()->getNumVolumeFaces() : 0);
        for (S32 f = 0; f < nf; ++f)
        {
            U32 id = 0;
            if (!is.findInstanceId(o->getID(), f, id)) continue;
            ids.push_back(id);
            owner.push_back((U32)i);
        }
    }
    std::vector<LLVKFaceQueryResult> res;
    if (!LLVulkanContext::instance().faceQuery(ids, &rect, res)) return false;
    for (size_t k = 0; k < res.size() && k < owner.size(); ++k)
    {
        if (!res[k].valid) continue;
        any_in[owner[k]] |= res[k].any_in ? 1 : 0;
        any_out[owner[k]] |= res[k].any_out ? 1 : 0;
    }
    return true;
}

} // namespace LLCleanCursorPick
