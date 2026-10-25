/**
 * @file sscombatray.h
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

// <SS:Skoomastream>
// Static world geometry along a segment: prims, linksets and terrain. Avatars,
// attachments, animesh, trees and grass never block; phantom prims and the linkset
// skip_root heads (the vehicle we sit on) pass through; alpha-blend texels under the pick
// threshold pass (the raster's alpha verdict). It replaces the CPU raycast
// LLPipeline::lineSegmentIntersectWorldGeometry, whose spatial partitions upstream
// removed (#480, #704), for the OTS camera collision, the aim convergence, the
// line-of-sight dot and the mouselook IFF check.
//
// Objects come from the GPU raster pick (LLCleanPickVK aux rays, LLVKWorldFrame::aux_pick):
// cast() queues its segment for this frame and reports the newest answer for its key,
// which is about 2 frames old (the GPU answers when the frame slot comes round). The
// pick draws every resident static object, on screen or not (clean_pick_select.comp
// all-static mode), so a wall behind the pulled-in camera still blocks. Terrain is
// marched on the CPU against the current segment, with no delay.
#ifndef SS_COMBATRAY_H
#define SS_COMBATRAY_H

#include "stdtypes.h"
#include "v3math.h"

struct LLVKWorldFrame;
class LLViewerObject;

namespace SSCombatRay
{
    // One key per ray a caller keeps asking for: its answers are matched by key.
    enum Key : U32
    {
        KEY_OTS_COLLISION = 1,
        KEY_AIM_CONVERGE  = 2,
        KEY_AIM_LOS       = 3,
        KEY_FLYCAM_ORBIT  = 4,    // <GMDX> game-control flycam orbit pivot
        KEY_IFF_BASE      = 16,   // + 2 * (target hash) + (0 centre, 1 head height)
    };

    // True = blocked; dist = metres from start to the nearest blocker (terrain now, or the
    // newest GPU answer for key). known (optional): false when no recent GPU answer covers
    // the whole segment, so only terrain was tested. answer_frame (optional, <GMDX>): the
    // frame the used GPU answer's ray was asked in, 0 without one; a new value is a new answer.
    bool cast(U32 key, const LLVector3& start, const LLVector3& end, const LLViewerObject* skip_root,
              F32& dist, bool* known = nullptr, U32* answer_frame = nullptr);

    // Once per main frame, after LLCleanCursorPick::fillRequest: take the GPU answers and
    // queue the rays cast() asked for since the last call.
    void fillRequests(LLVKWorldFrame& wf);
}
// </SS:Skoomastream>

#endif
