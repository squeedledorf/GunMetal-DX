/**
 * @file fscombataim.cpp
 * @brief Custom crosshair, line-of-sight (LOS) true-aim dot and OTS aim
 *        convergence.
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

// <SS:Skoomastream>
// Ported from SkoomaStorm's fscombathitmarker.cpp (the crosshair, tint and
// convergence parts; the hit marker and hit report stay there for now). The
// debug lines use gGL.color4f: diffuseColor4f is a no-op under the Vulkan
// backend.
// </SS:Skoomastream>

#include "llviewerprecompiledheaders.h"

#include "fscombataim.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llcriticaldamp.h"
#include "llframetimer.h"
#include "llquaternion.h"
#include "llrender.h"
#include "llui.h"
#include "lluiimage.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerwindow.h"
#include "llvoavatarself.h"
#include "sscombatray.h"   // <SS:Skoomastream> aim convergence / LOS rays

static LLColor4 sCrosshairTint = LLColor4::white;

// static
void FSCombatAim::setCrosshairTint(const LLColor4& color)
{
    sCrosshairTint = color;
}

// Ray (origin O, unit direction D) vs an oriented box (center C, rotation R,
// half-extents H). Returns true and the entry distance t along the ray (>=0) when
// it hits. Used to converge on an avatar's hitbox: transform the ray into the
// box's local frame and run the standard slab test against an axis-aligned box.
static bool rayOBBIntersect(const LLVector3& O, const LLVector3& D,
                            const LLVector3& C, const LLQuaternion& R, const LLVector3& H,
                            F32& t_out)
{
    const LLQuaternion inv = ~R;
    const LLVector3 o = (O - C) * inv; // ray origin in box-local space
    const LLVector3 d = D * inv;       // ray direction in box-local space (unit; rotation preserves length)

    const F32 BIG = 1e30f;
    F32 tmin = -BIG, tmax = BIG;
    for (S32 a = 0; a < 3; ++a)
    {
        const F32 oa = o.mV[a];
        const F32 da = d.mV[a];
        const F32 ha = H.mV[a];
        if (fabsf(da) < 1e-6f)
        {
            if (oa < -ha || oa > ha)
            {
                return false; // parallel to this slab and outside it
            }
        }
        else
        {
            F32 t1 = (-ha - oa) / da;
            F32 t2 = ( ha - oa) / da;
            if (t1 > t2) { const F32 tmp = t1; t1 = t2; t2 = tmp; }
            if (t1 > tmin) tmin = t1;
            if (t2 < tmax) tmax = t2;
            if (tmin > tmax)
            {
                return false;
            }
        }
    }
    if (tmax < 0.f)
    {
        return false; // box entirely behind the camera
    }
    t_out = (tmin >= 0.f) ? tmin : tmax; // near face, or exit face if the camera is inside
    return true;
}

// <GMDX> Fair fire: the convergence tunables are fixed, not settings. They decide where the
// sim is told the shot aims (send_agent_update's eye camera), so no one may tune them.
namespace
{
    // Lean guard: a world hit must lie this far beyond the eye along the camera ray
    // (SoapStorm 557eddcac5's eye-relative guard and its 0.2 m value).
    constexpr F32 OTS_CONVERGE_MIN_DISTANCE = 0.2f;
    // Half-life (s) of the world convergence depth smoothing.
    constexpr F32 OTS_CONVERGE_SMOOTHING_HALF_LIFE = 0.06f;
    // Avatars converge on their exact hitbox: always on, no inflation (a radius
    // would be aim assist).
}
// </GMDX>

// static
LLVector3 FSCombatAim::getOTSConvergenceTarget(const LLVector3& cam_origin,
                                                     const LLVector3& cam_at)
{
    const F32 CONVERGE_RANGE = 512.f; // meters; open-sky fallback distance

    // <SS:Skoomastream>
    // Solved once per frame: both callers (send_agent_update and the LOS dot)
    // get this frame's result, without a second raycast and avatar pass.
    static U32 s_solved_frame = U32_MAX;
    static LLVector3 s_solved_target;
    if (LLFrameTimer::getFrameCount() == s_solved_frame)
    {
        return s_solved_target;
    }
    // </SS:Skoomastream>

    LLVector3 target = cam_origin + cam_at * CONVERGE_RANGE;

    bool avatar_best = false;


    // World/static-geometry depth only (prims, linksets, terrain). Avatars, their
    // attachments and nametags are deliberately NOT tested here -- they are handled
    // exclusively by the hitbox pass below: a pick that tests avatars snapped convergence
    // onto a bystander's mesh (or even their nametag) the crosshair grazed. SSCombatRay
    // never tests avatars/attachments, and so never our own body either -- no
    // step-past-self needed. Its object answer is ~2 frames old (GPU pick); with no recent
    // answer the last world depth stands. Phantom prims pass through
    // (no physics, a bullet passes through). With nothing solid under the crosshair the
    // far point stands. Also skips the seated vehicle linkset so the crosshair aim
    // ray can pass through the vehicle's hood/windshield to reach targets in the world.
    const LLViewerObject* seated_root = (isAgentAvatarValid() && gAgentAvatarp->isSitting())
        ? (const LLViewerObject*)gAgentAvatarp->getRoot() : nullptr;

    static F32 s_last_world_dist = CONVERGE_RANGE;
    F32 hit_dist = CONVERGE_RANGE;
    bool known = false;
    if (SSCombatRay::cast(SSCombatRay::KEY_AIM_CONVERGE, cam_origin, target, seated_root, hit_dist, &known))
    {
        target = cam_origin + cam_at * hit_dist;
    }
    else if (!known)
    {
        target = cam_origin + cam_at * s_last_world_dist;
    }

    // Convergence depth measured along the camera ray (target is colinear with
    // cam_at, so the dot is its distance).
    F32 world_dist = (target - cam_origin) * cam_at;
    if (world_dist < 0.f)
    {
        world_dist = CONVERGE_RANGE;
    }
    s_last_world_dist = world_dist;

    // <GMDX> SoapStorm 557eddcac5: the guard is measured from the eye, not the camera.
    // The shot originates at the player's eye, but the convergence ray starts at
    // the (pulled-back OTS) camera. Someone standing behind the player is still in
    // front of the camera, so the crosshair ray can graze their hitbox or geometry and steal
    // convergence. Require convergence to be at least as far as the eye along the ray
    // so only targets genuinely in front of the shooter qualify. One dot product, computed once.
    F32 eye_dist = 0.1f;
    if (isAgentAvatarValid() && gAgentAvatarp->mHeadp)
    {
        const LLVector3 eye = gAgentAvatarp->mHeadp->getWorldPosition();
        eye_dist = llmax(0.1f, (eye - cam_origin) * cam_at);
    }

    // Lean guard: a convergence point right on top of or behind the player's eye
    // makes the eye->target angle blow up (~atan(shoulder_offset / dist)). Guard
    // against world hits falling behind the eye or into the shoulder gap. Genuine
    // avatar hits below are trusted and exempt, so close targets still converge dead on.
    const F32 min_world_dist = eye_dist + OTS_CONVERGE_MIN_DISTANCE; // fixed: fair fire
    F32 best_dist = llmax(world_dist, min_world_dist);
    // </GMDX>

    // Avatar convergence on the actual hitbox. We converge on the SAME oriented box
    // LL's Avatar Hitboxes debug draws (DebugRenderHitboxes; that GL draw is not in
    // this viewer, the box is the same): centered at getPositionAgent() (the object position, unaffected by viewer-side
    // skeleton animation/extrapolation), sized by getScale(), oriented by
    // getRotationRegion(). That box is what an LSL llCastRay weapon hits, so
    // converging on it lines the crosshair up with what takes damage rather than the
    // smoothly-rendered body, which leads the hitbox during fast movement. Taken as
    // min() against world depth so a wall in front of the target still wins.
    // (Always on, exact hitbox: fixed on purpose, fair fire.)
    {
        for (LLCharacter* character : LLCharacter::sInstances)
        {
            LLVOAvatar* avatar = (LLVOAvatar*)character;
            if (!avatar || avatar->isDead() || avatar->isControlAvatar() || avatar->isSelf())
            {
                continue;
            }

            const LLVector3 center = avatar->getPositionAgent();
            const LLVector3 to_av = center - cam_origin;
            if (to_av * cam_at < 0.f)
            {
                continue; // behind the camera
            }
            if (to_av * to_av > CONVERGE_RANGE * CONVERGE_RANGE)
            {
                continue; // out of range
            }

            const LLQuaternion rot = avatar->getRotationRegion();
            const LLVector3 half = avatar->getScale() * 0.5f;

            F32 t_hit = 0.f;
            const bool box_hit = rayOBBIntersect(cam_origin, cam_at, center, rot, half, t_hit);
            // t_hit > eye_dist: the box must be in front of the eye, not in the
            // over-the-shoulder gap or behind the player.
            if (box_hit && t_hit > eye_dist && t_hit < best_dist)
            {
                best_dist = t_hit; // trusted target; exempt from the min clamp
                avatar_best = true;
            }
        }
    }

    // Smooth the convergence DEPTH (a scalar along the ray), not the 3D point, so
    // the target stays exactly under the crosshair while its depth eases across
    // discontinuities instead of snapping, and rough-geometry jitter stops
    // shaking the aim. Advanced once per frame: both callers (send_agent_update
    // and the line-of-sight (LOS) dot) invoke this every frame and must get the same value,
    // so we key the lerp on the frame counter and reuse the result within a frame.
    const F32 smooth_hl = OTS_CONVERGE_SMOOTHING_HALF_LIFE; // fixed: fair fire
    static U32 last_frame = 0;
    static F32 smoothed_dist = CONVERGE_RANGE;
    const U32 frame = LLFrameTimer::getFrameCount();
    if (!avatar_best)
    {
        // World geometry only: rough surfaces jitter and depth edges snap.
        if (frame != last_frame)
        {
            last_frame = frame;
            const F32 amt = LLSmoothInterpolation::getInterpolant(smooth_hl);
            smoothed_dist = lerp(smoothed_dist, best_dist, amt);
        }
        best_dist = smoothed_dist;
    }
    else
    {
        // Avatar hit (a stable, authoritative box) bypasses smoothing for a snappy
        // lock; keep the smoother in step so returning to world geometry is seamless.
        last_frame = frame;
        smoothed_dist = best_dist;
    }

    const LLVector3 result = cam_origin + cam_at * best_dist;

    // <SS:Skoomastream>
    s_solved_frame = LLFrameTimer::getFrameCount();
    s_solved_target = result;
    // </SS:Skoomastream>
    return result;
}

// static
void FSCombatAim::drawCrosshair(S32 view_width, S32 view_height)
{
    static LLCachedControl<F32> scale_setting(gSavedSettings, "FSHitMarkerScale", 1.0f);
    const F32 scale = llclamp((F32)scale_setting, 0.5f, 3.f);

    LLUIImagePtr dot = LLUI::getUIImage("FSCrosshairDot");
    LLUIImagePtr arrows = LLUI::getUIImage("FSCrosshairArrows");

    // Optional target-identification tint (contact set / group color of the
    // avatar under the crosshair, fed by the combat features pass).
    static LLCachedControl<bool> target_color(gSavedSettings, "FSCrosshairTargetColor", false);
    const LLColor4 color = target_color ? sCrosshairTint : LLColor4::white;

    // True-impact marker: the camera ray and the bullet (from the avatar,
    // which is what the sim is told) diverge near cover in OTS. Cast the
    // bullet's ray against world geometry and slide the marker (the dot, or
    // the chevrons with FSOTSTrueAimMoveChevrons) to where it actually lands. With nothing hit (open sky) the rays agree at
    // infinity and the dot rests at center, so there is always a dot to
    // aim with.
    F32 offset_x = 0.f;
    F32 offset_y = 0.f;
    static U32 s_confirmed = 0;     // <GMDX> consecutive answers that saw the shot blocked
    static U32 s_last_answer = 0;   // <GMDX> frame of the last counted answer
    static LLCachedControl<bool> true_aim(gSavedSettings, "FSOTSTrueAimDot", true);
    const bool is_sitting = isAgentAvatarValid() && gAgentAvatarp->isSitting();
    if (true_aim && !is_sitting && gAgentCamera.cameraOTS() && isAgentAvatarValid() && gAgentAvatarp->mHeadp
        && gAgent.getTeleportState() == LLAgent::TELEPORT_NONE) // never raycast mid-teleport
    {
        const F32 TRUE_AIM_RANGE = 256.f; // meters; past this the parallax is subpixel
        LLViewerCamera* camera = LLViewerCamera::getInstance();
        const LLVector3 eye = gAgentAvatarp->mHeadp->getWorldPosition();

        // Bullet direction mirrors what the sim is told: the eye aimed at the
        // camera ray's world target (converged). (<GMDX> The convergence camera is
        // always on now, fair fire; the parallel ray is only a degenerate fallback.)
        LLVector3 dir = camera->getAtAxis();
        LLVector3 aim_end = eye + dir * TRUE_AIM_RANGE; // parallel (true-fire) fallback end
        {
            // Mirror the reported eye camera exactly (send_agent_update uses
            // the same helper): aim from the eye at whatever is under the
            // crosshair, avatars included.
            const LLVector3 target = getOTSConvergenceTarget(camera->getOrigin(), camera->getAtAxis());
            LLVector3 converged = target - eye;
            if (converged.normalize() > 0.f)
            {
                dir = converged;
                aim_end = target; // stop the cover test at the target, not past it
            }
        }

        // Blocked-shot warning: solid world geometry between the eye and the
        // aim point is cover the camera sees over (avatars never block, so
        // they can't be mistaken for cover). With a clear path no hit is
        // returned and the dot rests at center, which in convergence mode is
        // the target under the crosshair. Casting only as far as aim_end is
        // what keeps terrain *behind* the target from pulling the dot off it.
        // Also skips the seated vehicle linkset so the vehicle canopy/windshield doesn't register as cover.
        const LLViewerObject* seated_root = (isAgentAvatarValid() && gAgentAvatarp->isSitting())
            ? (const LLViewerObject*)gAgentAvatarp->getRoot() : nullptr;
        // <GMDX> Only a definite block moves the marker: FSOTSTrueAimConfirmAnswers
        // consecutive fresh GPU answers (SSCombatRay objects are ~2 frames old, one new
        // answer a frame) must all report the shot blocked. Any frame without a known hit
        // (clear, or no recent answer) drops the count and the marker eases back to centre.
        static LLCachedControl<U32> confirm_answers(gSavedSettings, "FSOTSTrueAimConfirmAnswers", 3);
        F32 hit_dist = 0.f;
        bool known = false;
        U32 answer_frame = 0;
        if (SSCombatRay::cast(SSCombatRay::KEY_AIM_LOS, eye, aim_end, seated_root, hit_dist, &known, &answer_frame) && known)
        {
            if (answer_frame != s_last_answer)
            {
                s_last_answer = answer_frame;
                ++s_confirmed;
            }
        }
        else
        {
            s_confirmed = 0;
        }
        if (s_confirmed >= llclamp((U32)confirm_answers, 1U, 30U))
        {
            s_confirmed = llclamp((U32)confirm_answers, 1U, 30U); // no overflow while it holds
            const LLRect& world_rect = gViewerWindow->getWorldViewRectScaled();
            LLCoordGL screen(world_rect.getCenterX(), world_rect.getCenterY());
            camera->projectPosAgentToScreen(eye + dir * hit_dist, screen, true);
            offset_x = (F32)(screen.mX - world_rect.getCenterX());
            offset_y = (F32)(screen.mY - world_rect.getCenterY());
        }
        // </GMDX>
    }
    else
    {
        s_confirmed = 0; // <GMDX> no stale confirmation into the next OTS session
    }

    // Smooth the dot's travel (raycasts against rough geometry jitter) and
    // snap to center inside a small deadzone so it doesn't shimmer at rest.
    static F32 smoothed_x = 0.f;
    static F32 smoothed_y = 0.f;
    const F32 interp = LLSmoothInterpolation::getInterpolant(0.05f);
    smoothed_x = lerp(smoothed_x, offset_x, interp);
    smoothed_y = lerp(smoothed_y, offset_y, interp);
    const F32 DEADZONE = 1.5f; // ui px
    if (fabsf(smoothed_x) < DEADZONE && fabsf(smoothed_y) < DEADZONE &&
        fabsf(offset_x) < DEADZONE && fabsf(offset_y) < DEADZONE)
    {
        smoothed_x = 0.f;
        smoothed_y = 0.f;
    }

    // <GMDX> FSOTSTrueAimMoveChevrons: the chevrons carry the true-aim offset and the dot
    // stays on the camera ray; off, the dot moves (the original layout).
    static LLCachedControl<bool> move_chevrons(gSavedSettings, "FSOTSTrueAimMoveChevrons", true);
    const S32 dot_dx = move_chevrons ? 0 : (S32)smoothed_x;
    const S32 dot_dy = move_chevrons ? 0 : (S32)smoothed_y;
    const S32 arrows_dx = move_chevrons ? (S32)smoothed_x : 0;
    const S32 arrows_dy = move_chevrons ? (S32)smoothed_y : 0;
    // </GMDX>
    if (dot)
    {
        const S32 w = (S32)(dot->getWidth() * scale);
        const S32 h = (S32)(dot->getHeight() * scale);
        dot->draw((view_width - w) / 2 + dot_dx, (view_height - h) / 2 + dot_dy, w, h, color);
    }
    if (arrows)
    {
        const S32 w = (S32)(arrows->getWidth() * scale);
        const S32 h = (S32)(arrows->getHeight() * scale);
        arrows->draw((view_width - w) / 2 + arrows_dx, (view_height - h) / 2 + arrows_dy, w, h, color);
    }
}
