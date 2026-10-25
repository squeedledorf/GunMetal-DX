/**
 * @file llcleanpartpick.cpp
 * @brief Picking particles under Vulkan (#481). See llcleanpartpick.h.
 */
#include "llviewerprecompiledheaders.h"

#include "llcleanpartpick.h"

#include "llagentcamera.h"
#include "llappviewer.h"           // gFrameCount
#include "llcleanparticlestore.h"
#include "llcleancursorpick.h"
#include "llviewercamera.h"
#include "llviewerwindow.h"
#include "llvkhitch.h"             // #757 crash breadcrumb / hitch sections
#include "raytrace.h"
#include "vkgfx/llvulkancontext.h"

#include <cmath>
#include <cstring>

namespace
{
// Per-frame request ray length (as LLCleanCursorPick's): past any pick segment, so an
// answer covers every segment along its ray.
constexpr F32 kRayLength = 1024.f;
// A non-click pick keeps the per-frame request alive this many frames (the answer
// trails the request by ~2); nothing asking = no GPU particle pick work.
constexpr U32 kArmFrames = 8;
// A per-frame answer whose request is more than this many frames old is not used (the
// particles moved on; picking paused while none were alive).
constexpr U32 kMaxAgeFrames = 8;
constexpr U32 kSerialRing = 16;

U32 sSerial = 0;
U32 sSerialFrame[kSerialRing] = {};   // the frame each recent serial was issued in
U32 sArmedUntil = 0;
bool sArmed = false;

U32 nextSerial()
{
    if (++sSerial == 0) sSerial = 1;   // 0 = an unwritten result entry
    sSerialFrame[sSerial % kSerialRing] = (U32)gFrameCount;
    return sSerial;
}

// Answer r's request is recent: one of the last kSerialRing serials, issued at most
// kMaxAgeFrames frames ago.
bool recent(const LLVKPartPickResult& r)
{
    if ((U32)(sSerial - r.serial) >= kSerialRing) return false;
    return (U32)gFrameCount - sSerialFrame[r.serial % kSerialRing] <= kMaxAgeFrames;
}

// Any particle alive (the renderer's free-count readback, a frame or two old): with none
// there is nothing to pick and no GPU pass is recorded or waited on.
bool anyAlive()
{
    LLCleanParticleStore& ps = LLCleanParticleStore::instance();
    return ps.ready() && ps.readLiveParticleCount() > 0;
}

F32 pixelAngle()
{
    const LLViewerCamera* cam = LLViewerCamera::getInstance();
    const S32 h = llmax(cam->getViewHeightInPixels(), 1);
    return cam->getView() / (F32)h;
}

// The alpha the pick treats as empty: the blend threshold (#276), or no texture test
// for hit-everything picks (Select Invisible Objects / Highlight Transparent).
F32 alphaThreshold()
{
    return LLCleanCursorPick::alphaBypass() ? -1.f : LLCleanCursorPick::alphaBlendThreshold();
}

// Does per-frame answer r cover the pick ray (s, unit d)? A hit counts when it lies on
// this ray within `hit_px` pixels at its distance (particles drift, the cursor moves a
// little); a miss counts when the ray itself is the same (origin within 5 cm,
// direction within 2 pixels).
bool matchRay(const LLVKPartPickResult& r, const LLVector3& s, const LLVector3& d, F32 pix, F32 hit_px)
{
    if (!r.valid) return false;
    if (r.hit)
    {
        const LLVector3 v = LLVector3(r.pos) - s;
        const F32 t = v * d;
        if (t > 0.f)
        {
            const F32 perp = (v - d * t).length();
            if (perp <= llmax(0.01f, hit_px * pix * t)) return true;
        }
    }
    const LLVector3 rd(r.dir), ro(r.origin);
    if ((ro - s).length() > 0.05f) return false;
    return rd * d >= std::cos(2.f * pix);
}
} // namespace

namespace LLCleanPartPick
{

void fillRequest(LLVKWorldFrame& wf)
{
    LLVKPickRequest& r = wf.part_pick;
    r = LLVKPickRequest{};
    if (!sArmed || (S32)(sArmedUntil - (U32)gFrameCount) < 0)
    {
        sArmed = false;
        return;
    }
    if (!wf.enabled || !gViewerWindow || !anyAlive()) return;

    // The cursor (mouselook: the crosshair) and the ray exactly as
    // LLViewerWindow::cursorIntersect builds it (LLCleanCursorPick::fillRequest).
    S32 x, y;
    if (gAgentCamera.cameraMouselook())
    {
        x = gViewerWindow->getWorldViewRectScaled().getWidth() / 2;
        y = gViewerWindow->getWorldViewRectScaled().getHeight() / 2;
    }
    else
    {
        x = gViewerWindow->getCurrentMouseX();
        y = gViewerWindow->getCurrentMouseY();
    }
    LLViewerCamera* cam = LLViewerCamera::getInstance();
    const LLVector3 dir = gViewerWindow->mouseDirectionGlobal(x, y);
    const LLVector3 eye = cam->getOrigin();
    const LLVector3 at = cam->getAtAxis();
    LLVector3 start = eye;
    line_plane(eye, dir, eye + at * cam->getNear(), at, start);
    LLVector3 d = dir;
    if (d.normVec() < 1e-6f) return;

    r.valid = true;
    std::memcpy(r.origin, start.mV, sizeof(r.origin));
    std::memcpy(r.dir, d.mV, sizeof(r.dir));
    r.length = kRayLength;
    r.pixel_angle = pixelAngle();
    r.alpha_threshold = alphaThreshold();
    r.mouse_x = x;
    r.mouse_y = y;
    r.serial = nextSerial();
}

void arm()
{
    sArmed = true;
    sArmedUntil = (U32)gFrameCount + kArmFrames;
}

bool intersect(const LLVector4a& start, const LLVector4a& end, bool sync, Hit& out)
{
    VK_HITCH_SCOPE("pick_particle");
    if (!anyAlive()) return false;
    const LLVector3 s(start.getF32ptr());
    LLVector3 d = LLVector3(end.getF32ptr()) - s;
    const F32 len = d.normVec();
    if (len < 1e-4f) return false;

    LLVulkanContext& vk = LLVulkanContext::instance();
    LLVKPartPickResult r;
    // The per-frame answer, when it covers this ray (a click keeps a 2 px hit tolerance:
    // a miss falls through to the synchronous answer below; other picks take 6 px).
    if (!sync) arm();   // keep the per-frame request going while picks keep asking
    bool have = vk.latestPartPick(r) && recent(r) && matchRay(r, s, d, pixelAngle(), sync ? 2.f : 6.f);
    if (!have && sync)
    {
        // A click no per-frame answer covers: answer exactly this segment against the
        // pool the last frame drew (waits for the frames in flight).
        LLVKPickRequest q;
        q.valid = true;
        std::memcpy(q.origin, s.mV, sizeof(q.origin));
        std::memcpy(q.dir, d.mV, sizeof(q.dir));
        q.length = len;
        q.pixel_angle = pixelAngle();
        q.alpha_threshold = alphaThreshold();
        q.serial = nextSerial();
        have = vk.partPickNow(q, r);
        if (!have) LLCleanCursorPick::noteProblem("click's particle pick got no GPU answer while particles are alive (particle pool / pipeline not ready, or the submission failed)");
    }
    if (!have || !r.hit) return false;
    // Only a hit before the segment's end (the caller shortened it to the nearest
    // solid hit: a particle behind a wall is not drawn there).
    const F32 t = (LLVector3(r.pos) - s) * d;
    if (t < 0.f || t > len) return false;

    out.pos.load3(r.pos);
    out.owner.setNull();
    out.source.setNull();
    // Owner / source object of the emitting source (unknown once its emitter slot was
    // recycled: still a hit, as LL's pick of an ownerless particle).
    LLCleanParticleStore::instance().emitterOwner(r.emitter, r.source_id, out.owner, out.source);
    return true;
}

} // namespace LLCleanPartPick
