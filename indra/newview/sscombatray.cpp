/**
 * @file sscombatray.cpp
 * @brief World-geometry rays for the OTS camera and the combat aim, on the GPU pick.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
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
 * $/LicenseInfo$
 */

// <SS:Skoomastream> See sscombatray.h.

#include "llviewerprecompiledheaders.h"
#include "sscombatray.h"

#include "llagent.h"
#include "llappviewer.h"           // gFrameCount
#include "llcleancursorpick.h"
#include "llcleaninstancestore.h"
#include "llviewercontrol.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llvovolume.h"
#include "llworld.h"
#include "vkgfx/llvulkancontext.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace
{
constexpr U32 kMaxAgeFrames = 10;      // an older answer is no answer
constexpr U32 kForgetFrames = 600;     // keys not asked for this long are dropped
constexpr F32 kPixelAngle   = 0.001f;  // the 1x1 target's field (only its centre matters)
constexpr F32 kNear         = 0.01f;   // projection near plane (m)
constexpr F32 kMinWindow    = 2.f;     // narrowest depth window after candidate overflows (m)

struct Answer
{
    bool valid = false;
    U32  frame = 0;          // the frame the ray was asked in
    bool complete = false;   // the whole segment was tested
    bool hit = false;
    F32  t = 0.f;            // metres along the answered segment
};

struct KeyState
{
    LLVector3 s, e;          // the segment asked for this frame
    LLUUID    skip_root;
    U32       asked_frame = U32_MAX;
    F32       win_hi = 0.f;  // depth window for the next request (0 = the whole segment)
    Answer    ans;
};

struct Pending
{
    U32    key, serial, frame;
    LLUUID skip_root;
};

std::unordered_map<U32, KeyState> sKeys;
std::vector<U32>     sQueue;     // keys asked since the last fillRequests
std::vector<Pending> sPending;   // requests sent, not answered yet
U32 sSerial = 0;

// A fragment that blocks: drawn solid (the raster's alpha verdict), a static unworn
// non-phantom prim outside skip_root's linkset, under LL's per-object pick filters
// (LLVOVolume::lineSegmentIntersect's preamble, as the CPU raycast applied them).
bool blocks(const LLVKPickFrag& f, const LLUUID& skip_root)
{
    if (!(f.flags & LLVKPickFrag::ALPHA_PASS)) return false;
    LLUUID id;
    S32 face = -1;
    if (!LLCleanInstanceStore::instance().keyForId(f.instance, id, face)) return false;
    LLViewerObject* obj = gObjectList.findObject(id);
    if (!obj || obj->isDead() || obj->isAvatar() || obj->isAttachment() || obj->getPCode() != LL_PCODE_VOLUME)
        return false;
    if (obj->flagPhantom()) return false;
    const LLViewerObject* root = obj->getRootEdit();
    if (root && (root->isAnimatedObject() || (skip_root.notNull() && root->getID() == skip_root))) return false;
    return ((LLVOVolume*)obj)->vkPickFiltersPass(false);
}

// First crossing from above the land to below it along the segment, refined by bisection.
bool landHit(const LLVector3& s, const LLVector3& d, F32 len, F32& t_out)
{
    LLWorld* world = LLWorld::getInstance();
    auto above = [&](F32 t, bool& valid)
    {
        const LLVector3 p = s + d * t;
        valid = world->getRegionFromPosAgent(p) != nullptr;
        return !valid || p.mV[VZ] >= world->resolveLandHeightAgent(p);
    };
    const F32 step = llclamp(len / 16.f, 0.05f, 1.f);
    bool valid = false;
    bool prev = above(0.f, valid);
    F32 t0 = 0.f;
    for (F32 t = step; ; t += step)
    {
        const F32 tc = llmin(t, len);
        const bool now = above(tc, valid);
        if (prev && !now)
        {
            F32 lo = t0, hi = tc;
            for (int i = 0; i < 10; ++i)
            {
                const F32 mid = 0.5f * (lo + hi);
                bool v = false;
                (above(mid, v) ? lo : hi) = mid;
            }
            t_out = hi;
            return true;
        }
        prev = now;
        t0 = tc;
        if (tc >= len) return false;
    }
}

// The 1x1 projection about the ray's origin (VK clip, reversed Z, Y flipped, the main
// view's handedness so gl_FrontFacing means facing the origin): maps (world - origin)
// to clip with the ray through the pixel centre.
void rayViewProj(const LLVector3& d, F32 far_m, F32 out[16])
{
    const LLVector3 up_ref = std::fabs(d.mV[VZ]) < 0.99f ? LLVector3::z_axis : LLVector3::x_axis;
    LLVector3 right = d % up_ref;
    right.normVec();
    const LLVector3 up = right % d;
    const F32 R[16] = { right.mV[0], up.mV[0], -d.mV[0], 0.f,
                        right.mV[1], up.mV[1], -d.mV[1], 0.f,
                        right.mV[2], up.mV[2], -d.mV[2], 0.f,
                        0.f,         0.f,      0.f,      1.f };
    const F32 f = 1.f / std::tan(0.5f * kPixelAngle);
    const F32 zn = kNear, zf = llmax(far_m, zn + 1.f);
    const F32 P[16] = { f,   0.f, 0.f,                0.f,
                        0.f, -f,  0.f,                0.f,
                        0.f, 0.f, zn / (zf - zn),    -1.f,
                        0.f, 0.f, zn * zf / (zf - zn), 0.f };
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
        {
            F32 v = 0.f;
            for (int k = 0; k < 4; ++k) v += P[k * 4 + r] * R[c * 4 + k];
            out[c * 4 + r] = v;
        }
}

void makeRequest(const KeyState& ks, U32 serial, LLVKPickRequest& r)
{
    LLVector3 d = ks.e - ks.s;
    const F32 len = d.normVec();
    r = LLVKPickRequest{};
    r.valid = true;
    std::memcpy(r.origin, ks.s.mV, sizeof(r.origin));
    std::memcpy(r.dir, d.mV, sizeof(r.dir));
    r.length = len;
    r.pixel_angle = kPixelAngle;
    r.alpha_threshold = LLCleanCursorPick::alphaBlendThreshold();
    r.serial = serial;
    std::memcpy(r.apex, ks.s.mV, sizeof(r.apex));
    rayViewProj(d, 2.f * len + 1.f, r.world_vp);
    r.win[0] = -1.f;
    r.win[1] = ks.win_hi > 0.f ? llmin(ks.win_hi, len) : len;
    r.win[2] = r.win[3] = 0.f;
    static LLCachedControl<bool> s_prepass(gSavedSettings, "RenderVKDepthPrepass", true);
    r.mode = LLVKPickRequest::MODE_ALL_STATIC | (s_prepass ? LLVKPickRequest::MODE_PREPASS : 0u);
}

void takeAnswer(LLVKPickResult& r)
{
    auto it = std::find_if(sPending.begin(), sPending.end(), [&](const Pending& p) { return p.serial == r.req.serial; });
    if (it == sPending.end()) return;
    const Pending p = *it;
    sPending.erase(it);
    auto ki = sKeys.find(p.key);
    if (ki == sKeys.end()) return;
    KeyState& ks = ki->second;
    if (ks.ans.valid && (S32)(p.frame - ks.ans.frame) < 0) return;   // older than what we have
    Answer a;
    a.valid = true;
    a.frame = p.frame;
    const F32 len = r.req.length;
    // A candidate list past its cap left instances undrawn: nothing is known (narrow the
    // window next time). A fragment overflow already narrowed the window on the GPU.
    if (!r.cand_overflow_world && !r.overflow_world)
    {
        std::sort(r.world.begin(), r.world.end(), [](const LLVKPickFrag& x, const LLVKPickFrag& y) { return x.t < y.t; });
        for (const LLVKPickFrag& f : r.world)
        {
            if (f.t < r.win[0] || f.t >= r.win[1] || f.t < 0.f || f.t > len) continue;
            if (blocks(f, p.skip_root))
            {
                a.hit = true;
                a.t = f.t;
                break;
            }
        }
        a.complete = r.win[1] >= len;
    }
    ks.ans = a;
    // Depth window for the next requests of this key: halve after a candidate overflow,
    // grow back while the narrowed windows find nothing.
    if (r.cand_overflow_world) ks.win_hi = llmax(kMinWindow, 0.5f * llmin(r.req.win[1], len));
    else if (!a.hit && ks.win_hi > 0.f) ks.win_hi = (2.f * ks.win_hi >= len) ? 0.f : 2.f * ks.win_hi;
}
} // namespace

namespace SSCombatRay
{
bool cast(U32 key, const LLVector3& start, const LLVector3& end, const LLViewerObject* skip_root,
          F32& dist, bool* known, U32* answer_frame)
{
    if (known) *known = false;
    if (answer_frame) *answer_frame = 0;   // <GMDX>
    LLVector3 d = end - start;
    const F32 len = d.normVec();
    dist = len;
    if (len < 1e-3f) return false;
    bool hit = false;
    F32 lt = 0.f;
    if (landHit(start, d, len, lt))
    {
        dist = lt;
        hit = true;
    }
    const U32 frame = (U32)gFrameCount;
    KeyState& ks = sKeys[key];
    if (ks.asked_frame != frame)
    {
        ks.asked_frame = frame;
        ks.s = start;
        ks.e = end;
        ks.skip_root = skip_root ? skip_root->getID() : LLUUID::null;
        sQueue.push_back(key);
    }
    const Answer& a = ks.ans;
    if (a.valid && frame - a.frame <= kMaxAgeFrames)
    {
        if (a.hit && a.t <= len && a.t < dist)
        {
            dist = a.t;
            hit = true;
        }
        if (known) *known = a.complete || a.hit;
        if (answer_frame) *answer_frame = a.frame;   // <GMDX>
    }
    return hit;
}

void fillRequests(LLVKWorldFrame& wf)
{
    for (LLVKPickRequest& r : wf.aux_pick) r = LLVKPickRequest{};
    std::vector<LLVKPickResult> answers;
    LLVulkanContext::instance().takeAuxPicks(answers);
    if (gAgent.getTeleportState() != LLAgent::TELEPORT_NONE)
    {
        sKeys.clear();
        sQueue.clear();
        sPending.clear();
        return;
    }
    for (LLVKPickResult& r : answers) takeAnswer(r);
    const U32 frame = (U32)gFrameCount;
    sPending.erase(std::remove_if(sPending.begin(), sPending.end(),
                                  [&](const Pending& p) { return frame - p.frame > kMaxAgeFrames; }),
                   sPending.end());
    if (wf.enabled)
    {
        U32 n = 0;
        for (U32 key : sQueue)
        {
            if (n == LLVKWorldFrame::kAuxPicks) break;   // ponytail: first come first served; later keys wait a frame
            auto ki = sKeys.find(key);
            if (ki == sKeys.end()) continue;
            if (++sSerial == 0) sSerial = 1;
            makeRequest(ki->second, sSerial, wf.aux_pick[n++]);
            sPending.push_back({key, sSerial, ki->second.asked_frame, ki->second.skip_root});
        }
    }
    sQueue.clear();
    for (auto it = sKeys.begin(); it != sKeys.end();)
    {
        if (frame - it->second.asked_frame > kForgetFrames) it = sKeys.erase(it);
        else ++it;
    }
}
}
