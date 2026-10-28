/**
 * @file llcullcamera.h
 * @brief The camera that culling and streaming use (#479): the live world camera, or a
 *        pose frozen from it for debugging (Develop > Rendering > Freeze culling camera).
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

#ifndef LL_LLCULLCAMERA_H
#define LL_LLCULLCAMERA_H

#include "v3math.h"

class LLCamera;

// Culling / streaming camera (#479).
//
// Normally this IS the live world camera (LLViewerCamera). With RenderVKFreezeCullCamera
// on, it is a pose captured from the live camera at the moment of the toggle, kept in
// global coordinates (so region crossings / agent origin shifts keep it in place) and
// rebuilt in agent space every frame. Only these consumers read it:
//   - the object-cache selection (LLVOCachePartition::cull, the grid) and
//     LLViewerRegion::updateVisibleEntries (scene contribution, VRAM hold release);
//   - the VRAM far-geometry eviction (vramUnloadFarObjects);
//   - the VK GPU compute cull frustum + LOD / texel-demand camera (LLVKWorldFrame
//     frustum_planes, lod_cam_pos);
//   - CPU mesh LOD (llcleaningest.cpp updateDistance).
// The view, sound, agent updates / the interest list sent to the sim, picking and
// everything else keep reading LLViewerCamera, which is never modified by a freeze.
namespace LLCullCamera
{
    // Once per frame, AFTER the live camera's agent frustum planes were refreshed
    // (driveVulkanWorldVOCacheCull). Handles the setting's edges and rebuilds the frozen
    // camera in agent space. far_dist: the far distance the frustum corners use.
    void update(F32 far_dist);

    bool isFrozen();

    // The camera to cull with: the frozen copy, or LLViewerCamera.
    LLCamera& get();
    LLVector3 getOrigin();          // agent space
    F32 getPixelMeterRatio();       // LLViewerCamera's, captured at freeze time while frozen

    // Frozen pose for the VK frame (agent space). False when not frozen.
    bool getFrozenPose(LLVector3& eye, LLVector3& at, LLVector3& left, LLVector3& up,
                       F32& fov_y, F32& aspect, F32& near_dist);

    // LLWorld::updateRegions' "camera moved this frame" test (the static-camera throttle):
    // the cull camera's frustum planes changed since the last call. Call once per frame.
    bool isChanged();

    // Build agent-space frustum planes for cam from its own origin / axes / view / aspect /
    // near (no GL): the 8 corners as LLViewerCamera::updateFrustumPlanes' non-ortho branch
    // makes them, far corners along the near-corner rays at far_dist.
    // <GMDX> ortho_half_height > 0: an orthographic box of that half height (LLViewerCamera::
    // getOrthoHalfHeight) instead of the perspective frustum.
    void calcAgentFrustum(LLCamera& cam, F32 far_dist, F32 ortho_half_height = 0.f);
}

#endif // LL_LLCULLCAMERA_H
