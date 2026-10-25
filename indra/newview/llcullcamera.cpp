/**
 * @file llcullcamera.cpp
 * @brief The camera that culling and streaming use (#479), see llcullcamera.h.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
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
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"
#include "llcullcamera.h"

#include "llagent.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"

namespace
{
    bool        sFrozen = false;
    LLVector3d  sPosGlobal;
    LLVector3   sAt(1.f, 0.f, 0.f), sLeft(0.f, 1.f, 0.f), sUp(0.f, 0.f, 1.f);
    F32         sFovY = 1.f, sAspect = 1.f, sNear = 0.25f, sPixelMeterRatio = 0.f;

    LLCamera& frozenCamera()
    {
        static LLCamera s_cam;   // LLCamera is 16-byte aligned; function static, no heap
        return s_cam;
    }

    // The same sanitising buildVulkanWorldFrame applies (the VK path never runs
    // LLViewerCamera::setPerspective, so a field can sit at a constructor default).
    F32 saneFov(F32 v)    { return (v > 0.05f && v < 3.13f) ? v : 60.f * DEG_TO_RAD; }
    F32 saneAspect(F32 a) { return (a > 0.05f) ? a : 1.7778f; }
    F32 saneNear(F32 n)   { return (n > 0.01f) ? n : 0.25f; }

    void capture()
    {
        LLViewerCamera* live = LLViewerCamera::getInstance();
        sPosGlobal       = gAgent.getPosGlobalFromAgent(live->getOrigin());
        sAt              = live->getAtAxis();
        sLeft            = live->getLeftAxis();
        sUp              = live->getUpAxis();
        sFovY            = saneFov(live->getView());
        sAspect          = saneAspect(live->getAspect());
        sNear            = saneNear(live->getNear());
        sPixelMeterRatio = live->getPixelMeterRatio();
        sFrozen          = true;
        LL_INFOS("VOCacheMetrics") << "Culling camera FROZEN at " << sPosGlobal << " (agent "
                                   << live->getOrigin() << "), looking " << sAt << LL_ENDL;
    }
}

namespace LLCullCamera
{

void calcAgentFrustum(LLCamera& cam, F32 far_dist)
{
    const LLVector3 origin = cam.getOrigin();
    const LLVector3 at     = cam.getAtAxis();
    const LLVector3 up     = cam.getUpAxis();
    const LLVector3 right  = -cam.getLeftAxis();

    const F32 fov    = saneFov(cam.getView());
    const F32 aspect = saneAspect(cam.getAspect());
    const F32 nearp  = saneNear(cam.getNear());
    const F32 farp   = llclamp(far_dist, 64.f, 8192.f);

    const F32 nhh = nearp * std::tan(fov * 0.5f);   // near half-height
    const F32 nhw = nhh * aspect;                   // near half-width
    const LLVector3 nc = origin + at * nearp;       // near-plane centre

    LLVector3 frust[8];
    frust[0] = nc - right * nhw - up * nhh;   // near bottom-left
    frust[1] = nc + right * nhw - up * nhh;   // near bottom-right
    frust[2] = nc + right * nhw + up * nhh;   // near top-right
    frust[3] = nc - right * nhw + up * nhh;   // near top-left
    for (U32 i = 0; i < 4; ++i)
    {
        LLVector3 v = frust[i] - origin;
        v.normVec();
        frust[i + 4] = origin + v * farp;     // far corner along the same ray
    }
    cam.calcAgentFrustumPlanes(frust);
}

void update(F32 far_dist)
{
    static LLCachedControl<bool> s_freeze(gSavedSettings, "RenderVKFreezeCullCamera", false);
    bool want = s_freeze;
    // A teleport leaves the frozen pose's regions behind: drop the freeze (the setting
    // goes off so the menu check follows).
    if (want && gAgent.getTeleportState() != LLAgent::TELEPORT_NONE)
    {
        gSavedSettings.setBOOL("RenderVKFreezeCullCamera", false);
        want = false;
    }
    if (want && !sFrozen)
    {
        capture();
    }
    else if (!want && sFrozen)
    {
        sFrozen = false;
        LL_INFOS("VOCacheMetrics") << "Culling camera released (follows the view again)" << LL_ENDL;
    }
    if (!sFrozen)
    {
        return;
    }

    // Rebuild in agent space every frame: the agent origin moves on region crossings.
    LLCamera& cam = frozenCamera();
    cam.setView(sFovY);
    cam.setAspect(sAspect);
    cam.setNear(sNear);
    cam.setFar(llclamp(far_dist, 64.f, 8192.f));
    cam.setOrigin(gAgent.getPosAgentFromGlobal(sPosGlobal));
    cam.setAxes(sAt, sLeft, sUp);
    calcAgentFrustum(cam, far_dist);
}

bool isFrozen()
{
    return sFrozen;
}

LLCamera& get()
{
    if (sFrozen)
    {
        return frozenCamera();
    }
    return *LLViewerCamera::getInstance();
}

LLVector3 getOrigin()
{
    return get().getOrigin();
}

F32 getPixelMeterRatio()
{
    return sFrozen ? sPixelMeterRatio : LLViewerCamera::getInstance()->getPixelMeterRatio();
}

bool getFrozenPose(LLVector3& eye, LLVector3& at, LLVector3& left, LLVector3& up,
                   F32& fov_y, F32& aspect, F32& near_dist)
{
    if (!sFrozen)
    {
        return false;
    }
    const LLCamera& cam = frozenCamera();
    eye = cam.getOrigin();
    at = sAt;
    left = sLeft;
    up = sUp;
    fov_y = sFovY;
    aspect = sAspect;
    near_dist = sNear;
    return true;
}

bool isChanged()
{
    return get().isChanged();
}

} // namespace LLCullCamera
