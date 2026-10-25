/**
 * @file fscombataim.h
 * @brief Combat aiming aids: the custom composite crosshair (dot + arrows)
 *        with its target tint and line-of-sight (LOS) true-aim dot, and the
 *        over-the-shoulder (OTS) aim convergence shared by the crosshair and
 *        the camera reported to the sim. Split from SkoomaStorm's
 *        fscombathitmarker so the hit marker can be added beside it later.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
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

#ifndef FS_COMBATAIM_H
#define FS_COMBATAIM_H

#include "v3math.h"
#include "v4color.h"

class FSCombatAim
{
public:
    // The custom composite crosshair (dot + arrows). Called from
    // LLToolGun::draw in place of the stock crosshair when
    // FSCustomCrosshair is enabled.
    static void drawCrosshair(S32 view_width, S32 view_height);

    // Crosshair tint, set per-frame by the mouselook IFF target
    // identification in LLToolGun::draw (contact set color of the avatar
    // under the crosshair, line of sight verified). White when nothing is
    // identified. Only applied when FSCrosshairTargetColor is enabled.
    static void setCrosshairTint(const LLColor4& color);

    // OTS convergence target: the world point under the render
    // camera's crosshair that a bullet fired from the avatar's eye should aim
    // at. World geometry comes from a ray cast; nearby avatars are converged via
    // cheap vertical-capsule proxies (so aiming at a target lands on the target,
    // not the wall behind it, without the rigged-mesh pick cost), the shooter's
    // own body excluded. The depth is smoothed along the ray so it eases rather
    // than snaps. Shared by the reported eye camera (send_agent_update) and the
    // line-of-sight (LOS) dot so the two cannot drift apart; smoothing is advanced once per
    // frame so both callers receive the same value. Its tunables are fixed constants
    // (fair fire, see fscombataim.cpp).
    static LLVector3 getOTSConvergenceTarget(const LLVector3& cam_origin,
                                             const LLVector3& cam_at);
};

#endif // FS_COMBATAIM_H
