/**
 * @file gmdxots.cpp
 * @brief GunMetal-DX's over-the-shoulder (OTS) aim camera: mouselook with the camera at the shoulder.
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

// <GMDX> See gmdxots.h. GunMetal-DX 0.3.0's OTS functions (llagentcamera.cpp, llviewermenu.cpp and
// llviewerinput.cpp in v0.3.0, <SS:Skoomastream>), moved here almost verbatim. What changed: the camera
// mode stays mouselook (sOn says the camera is at the shoulder), the framing reads the
// SlipstreamTPSCamera* settings with Slipstream's clamps, and CombatFX gates it.

#include "llviewerprecompiledheaders.h"

#include "gmdxots.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llcriticaldamp.h"
#include "lltoolmgr.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llvoavatarself.h"
#include "llvrcamera.h"
#include "rlvactions.h"
#include "slcombatfx.h"
#include "sltpscamera.h"
#include "sscombatray.h"

namespace
{
bool sOn = false;                   // in mouselook, the camera is at the shoulder
bool sAnimFromShoulder = false;     // the camera animation started over the shoulder
bool sAnimFromFirstPerson = false;  // ... or in first person
bool sEscLeftAim = false;           // Dynamic camera: the last Esc left aim mode
bool sRenderedOnShoulder = false;   // this frame's (or, before updateCamera, the last frame's) camera is at the shoulder
F32  sCollisionDistance = -1.f;     // smoothed camera collision pull-in distance (< 0 = unset)
LLVector3 sSitCameraEye;            // the seat's sit camera eye offset (valid while sitCameraEnabled())

// The framing, clamped as Slipstream's TPS framing is (sltpscamera.cpp framing_value).
F32 framing(F32 value, F32 lower, F32 upper)
{
    return llfinite(value) ? llclamp(value, lower, upper) : lower;
}

F32 camera_distance()
{
    static LLCachedControl<F32> distance(gSavedSettings, "SlipstreamTPSCameraDistance", 3.f);
    return framing(distance, 0.5f, 10.f);
}

F32 camera_side()
{
    static LLCachedControl<F32> side(gSavedSettings, "SlipstreamTPSCameraSide", -0.5f);
    return framing(side, -2.f, 2.f);
}

F32 camera_height()
{
    static LLCachedControl<F32> height(gSavedSettings, "SlipstreamTPSCameraHeight", 0.5f);
    return framing(height, -1.f, 3.f);
}

F32 focus_distance()
{
    static LLCachedControl<F32> distance(gSavedSettings, "SlipstreamTPSFocusDistance", 10.f);
    return framing(distance, 2.f, 50.f);
}

// The headset camera sits at the head whatever the desktop camera does: no shoulder view there.
bool in_vr()
{
    return LLVRCamera::firstPerson() || LLVRCamera::flyCam();
}

// The RLVa camera distance cap that applies to the OTS camera, measured from the head (the avatar
// distance limit; the origin distance limit is applied the same way, since the OTS focus is a point in
// front of the avatar rather than a camera origin). False when there is none. (getOTSRlvMaxDistance)
bool rlv_max_distance(F32& max_dist)
{
    if (!RlvActions::isRlvEnabled() || !RlvActions::isCameraDistanceClamped())
    {
        return false;
    }
    max_dist = F32_MAX;
    F32 dist_min = 0.f, dist_max = F32_MAX;
    if (RlvActions::getCameraAvatarDistanceLimits(dist_min, dist_max))
    {
        max_dist = llmin(max_dist, dist_max);
    }
    if (RlvActions::getCameraOriginDistanceLimits(dist_min, dist_max))
    {
        max_dist = llmin(max_dist, dist_max);
    }
    return max_dist < F32_MAX;
}

// (rlvAllowsOTS)
bool rlv_allows()
{
    // Below this the camera would sit inside the head: not a shoulder view.
    const F32 OTS_RLV_MIN_SHOULDER_DISTANCE = 0.5f;
    F32 max_dist = F32_MAX;
    return !rlv_max_distance(max_dist) || max_dist >= OTS_RLV_MIN_SHOULDER_DISTANCE;
}

// The rotation the OTS offsets are expressed in. Seated, the agent frame is relative to the seat, so
// add the seat's rotation unless the vehicle decoupled the camera -- the same rule as the mouselook
// focus. (getOTSFrameRotation)
LLQuaternion frame_rotation()
{
    LLQuaternion agent_rot = gAgent.getFrameAgent().getQuaternion();
    if (isAgentAvatarValid() && gAgentAvatarp->getParent())
    {
        LLViewerObject* root_object = (LLViewerObject*)gAgentAvatarp->getRoot();
        if (!root_object->flagCameraDecoupled())
        {
            agent_rot *= ((LLViewerObject*)(gAgentAvatarp->getParent()))->getRenderRotation();
        }
    }
    return agent_rot;
}

bool seated_with_sit_camera()
{
    return isAgentAvatarValid() && gAgentAvatarp->isSitting() && gAgentCamera.sitCameraEnabled();
}

// M goes to the shoulder: OTS is on and, with OTSRememberLastView, Ctrl+Space did not last choose first person.
bool wants_shoulder()
{
    return gSavedSettings.getBOOL("OTSEnabled")
        && !(gSavedSettings.getBOOL("OTSRememberLastView") && gSavedSettings.getBOOL("OTSLastViewFirstPerson"));
}

// Aim mode, as M enters it: the shoulder camera when OTS is on and can be used, else first-person mouselook.
// True when it got there (mouselook may be refused: RLVa, EnableMouselook).
bool enter_aim_mode()
{
    if (wants_shoulder() && GMDXOTS::canUse())
    {
        GMDXOTS::enter();
    }
    else
    {
        gAgentCamera.changeCameraToMouselook();
    }
    return gAgentCamera.getCameraMode() == CAMERA_MODE_MOUSELOOK;
}
}

bool GMDXOTS::active()
{
    return sOn && gAgentCamera.getCameraMode() == CAMERA_MODE_MOUSELOOK && isAgentAvatarValid();
}

// (sRenderedOnShoulder: send_agent_update runs before updateCamera, so right after a snap into first
// person the camera it reads is still the last frame's, at the shoulder)
bool GMDXOTS::shoulderCamera()
{
    return active() || sRenderedOnShoulder
        || (sAnimFromShoulder && gAgentCamera.getCameraAnimating() && gAgentCamera.getCameraMode() == CAMERA_MODE_MOUSELOOK);
}

// Seated on an object that set no sit camera (llSetCameraEyeOffset), the shoulder camera has nothing
// sensible to sit on and clips into the seat: first person mouselook (cockpit view) instead.
bool GMDXOTS::canUse()
{
    const bool is_sitting = isAgentAvatarValid() && gAgentAvatarp->isSitting();
    return SLCombatFX::enabled() && !in_vr() && (!is_sitting || gAgentCamera.sitCameraEnabled()) && rlv_allows();
}

// changeCameraToMouselook gives the shoulder camera ALL of mouselook's input setup (cursor hiding, mouse
// capture, control flags, focus management, the UI); the camera mode stays mouselook and only the camera
// position and focus come from here. (changeCameraToOTS)
bool GMDXOTS::enter(bool animate)
{
    if (active())
    {
        return true;
    }
    const bool from_first_person = gAgentCamera.cameraMouselook();
    // (From first person, the toggle and the return from aim down sights, mouselook is set up already:
    // the shoulder is only the camera moving there.)
    if (gAgentCamera.getCameraMode() != CAMERA_MODE_MOUSELOOK)
    {
        gAgentCamera.changeCameraToMouselook(false);
    }
    // Mouselook refused (EnableMouselook off, RLVa, joystick camera): the aim mode is not available either.
    if (gAgentCamera.getCameraMode() != CAMERA_MODE_MOUSELOOK)
    {
        return false;
    }
    // An RLVa camera distance cap too small for a shoulder camera: stay in the first person view
    // mouselook just set up.
    if (!rlv_allows())
    {
        return false;
    }
    // Undo a first person head-tracking pelvis shift (the shoulder <- first person toggle), as
    // changeCameraToThirdPerson / changeCameraToFollow do.
    if (isAgentAvatarValid() && !gAgentAvatarp->isSitting())
    {
        gAgentAvatarp->mPelvisp->setPosition(LLVector3::zero);
    }
    sOn = true;
    // Restart collision smoothing fresh; a stale pull-in distance from the last session would briefly
    // hold the camera too close.
    sCollisionDistance = -1.f;
    // (Own avatar and attachments stay drawn and pickable on the shoulder: LLAgent::needsRenderAvatar
    // and LLVOAvatar::selfHiddenInMainView exempt it.)
    if (animate)
    {
        // The rendered camera is still at the old position this frame, so startCameraAnimation
        // snapshots it as the start and updateCamera glides to the shoulder over ZoomTime seconds.
        gAgentCamera.startCameraAnimation();
        sAnimFromShoulder = false;
        sAnimFromFirstPerson = from_first_person;
    }
    else
    {
        gAgentCamera.stopCameraAnimation();
        gAgent.endAnimationUpdateUI();
    }
    return true;
}

void GMDXOTS::toFirstPerson(bool animate)
{
    if (!active())
    {
        return;
    }
    if (animate)
    {
        gAgentCamera.startCameraAnimation();    // (from the shoulder: sOn is still set)
    }
    else
    {
        gAgentCamera.stopCameraAnimation();
        gAgent.endAnimationUpdateUI();
    }
    sOn = false;
}

bool GMDXOTS::takesKeys()
{
    return SLCombatFX::enabled() && gAgentCamera.cameraMouselook();
}

// Switch between the shoulder camera and true first-person mouselook without leaving aim mode
// (precision-aim style). (ots_toggle_view)
void GMDXOTS::toggleView()
{
    if (active())
    {
        toFirstPerson();
        gSavedSettings.setBOOL("OTSLastViewFirstPerson", true);
    }
    // Same rule as M: seated without a sit camera (or under a tight RLVa camera distance cap) stays
    // first person.
    else if (gAgentCamera.cameraMouselook() && canUse())
    {
        if (enter())
        {
            gSavedSettings.setBOOL("OTSLastViewFirstPerson", false);
        }
    }
}

// Mirror the shoulder camera to the avatar's other side. The side is read live every frame, so negating
// it moves the camera and the focus at once; the camera animation makes it a glide instead of a snap.
// (ots_swap_shoulder)
void GMDXOTS::swapShoulder()
{
    gSavedSettings.setF32("SlipstreamTPSCameraSide", -gSavedSettings.getF32("SlipstreamTPSCameraSide"));
    if (active())
    {
        gAgentCamera.startCameraAnimation();
    }
}

// M toggles aim mode: from either aim view (the shoulder or toggled-to first person) it leaves; into it,
// the shoulder camera when OTS is on and can be used (with OTSRememberLastView, whichever view was last
// toggled into), else the stock first-person mouselook.
bool GMDXOTS::onMouselookKey()
{
    sEscLeftAim = false;    // (Dynamic camera: M, not Esc)
    if (active())
    {
        gAgentCamera.changeCameraToDefault();
        return true;
    }
    if (!gAgentCamera.cameraMouselook() && wants_shoulder() && canUse())
    {
        enter();
        return true;
    }
    return false;
}

// Dynamic camera (SlipstreamTPSDynamicCamera): the last Esc left aim mode, so the next one (on the follow
// camera, not building, not on the engaged shooter camera, which Esc releases) goes back in. M clears it.
bool GMDXOTS::onEsc()
{
    static LLCachedControl<bool> dynamic_camera(gSavedSettings, "SlipstreamTPSDynamicCamera", false);
    if (dynamic_camera && sEscLeftAim && gAgentCamera.getCameraMode() == CAMERA_MODE_THIRD_PERSON
        && !SLTPSCamera::isEngagedNow() && !LLToolMgr::getInstance()->inBuildMode() && !gAgentCamera.cameraCustomizeAvatar())
    {
        sEscLeftAim = false;
        if (enter_aim_mode()) return true;  // (refused, e.g. by RLVa: the ordinary reset)
    }
    sEscLeftAim = dynamic_camera && gAgentCamera.cameraMouselook();   // (the shoulder too: the reset that follows leaves aim mode)
    return false;
}

void GMDXOTS::onWheelIn()
{
    toFirstPerson();
}

void GMDXOTS::update()
{
    sRenderedOnShoulder = false;    // (getCameraPositionGlobal sets it again while the camera is there)
    if (sOn && gAgentCamera.getCameraMode() != CAMERA_MODE_MOUSELOOK)
    {
        sOn = false;    // mouselook ended (M, Esc, building, appearance, ...): the shoulder view with it
    }
    // Turning OTS off (Preferences, @setdebug_otsenabled) while on the shoulder leaves it for first
    // person: the user is still aiming, only the shoulder view was switched off (onOTSEnabledChanged).
    // CombatFX off or the headset camera taking over does the same.
    static LLCachedControl<bool> ots_enabled(gSavedSettings, "OTSEnabled", true);
    static bool s_was_enabled = true;
    const bool switched_off = s_was_enabled && !ots_enabled;
    s_was_enabled = ots_enabled;
    if (!active())
    {
        return;
    }
    // Sat down on the shoulder on a seat whose creator set no sit camera: first person, as the shoulder
    // is never entered there (the sit response sets the sit camera before the avatar sits).
    const bool seat_without_camera = gAgentAvatarp->isSitting() && !gAgentCamera.sitCameraEnabled();
    if (switched_off || !SLCombatFX::enabled() || in_vr() || seat_without_camera)
    {
        gAgentCamera.changeCameraToMouselook();
        if (active())
        {
            gAgentCamera.changeCameraToDefault(); // mouselook refused (e.g. RLVa): leave the aim mode instead
        }
    }
    // An RLVa camera distance cap that arrives (or tightens) on the shoulder and leaves no room for a
    // shoulder camera drops the aim view to first person. Smaller caps pull the camera in
    // (getCameraPositionGlobal).
    else if (!rlv_allows())
    {
        gAgentCamera.changeCameraToMouselook(false);
        if (active())
        {
            gAgentCamera.changeCameraToDefault(); // mouselook refused too: leave the aim mode
        }
    }
}

void GMDXOTS::onChangeToMouselook(bool animate)
{
    // (in GunMetal-DX 0.3.0 changeCameraToMouselook left CAMERA_MODE_OTS: a seat's forced mouselook,
    // @setcam_mode, the camera floater's button and the like all end in first person)
    toFirstPerson(animate);
    sOn = false;
}

void GMDXOTS::onCameraAnimation()
{
    sAnimFromShoulder = active();
    sAnimFromFirstPerson = gAgentCamera.cameraMouselook() && !sAnimFromShoulder;
}

bool GMDXOTS::animatesFromFirstPerson()
{
    return sAnimFromFirstPerson;
}

LLVector3d GMDXOTS::getFocusGlobal()
{
    // Focus in front of the avatar at the camera's own height and side (SoapStorm 557eddcac5), so the
    // view looks parallel to the avatar's facing instead of angling in toward it.
    F32 focus_height = camera_height();
    F32 focus_side = camera_side();
    if (seated_with_sit_camera())
    {
        focus_height = sSitCameraEye.mV[VZ];
        focus_side = sSitCameraEye.mV[VY];
    }
    const LLVector3 focus_local(focus_distance(), focus_side, focus_height);
    const LLVector3 focus_world = focus_local * frame_rotation(); // seat-relative when seated, as mouselook
    return gAgent.getPosGlobalFromAgent(gAgentCamera.getAvatarRootPosition()) + LLVector3d(focus_world);
}

LLVector3d GMDXOTS::getCameraPositionGlobal(bool& constrained)
{
    // Shoulder offset camera -- avatar-local space: X=forward, Y=left, Z=up
    LLVector3 local_offset(-camera_distance(), camera_side(), camera_height());
    F32 pivot_height = camera_height();

    const bool is_sitting = isAgentAvatarValid() && gAgentAvatarp->isSitting();
    const LLViewerObject* seated_root = is_sitting ? (const LLViewerObject*)gAgentAvatarp->getRoot() : nullptr;

    if (seated_with_sit_camera())
    {
        // Vehicle creator specified a sit camera offset: honor their exact intended
        // X (distance back), Y (side offset), and Z (elevation).
        local_offset.setVec(-fabsf(sSitCameraEye.mV[VX]), sSitCameraEye.mV[VY], sSitCameraEye.mV[VZ]);
        pivot_height = sSitCameraEye.mV[VZ];
    }

    const LLVector3 world_offset = local_offset * frame_rotation(); // seat-relative when seated, as mouselook
    const LLVector3 avatar_pos_agent = gAgentCamera.getAvatarRootPosition();
    LLVector3 cam_pos_agent = avatar_pos_agent + world_offset;

    // Camera collision: the agent frame pitches with the look direction in mouselook, so pitching up
    // swings the shoulder camera down toward the ground (and strafing near walls swings it into
    // geometry). Raycast from a pivot at shoulder height out to the desired camera position and pull the
    // camera in just in front of anything solid. World geometry only; avatars and attachments never
    // block the camera. Also skips the seated vehicle linkset.
    static LLCachedControl<bool> collision(gSavedSettings, "SlipstreamTPSCameraCollision", true);
    if (collision && gAgent.getTeleportState() == LLAgent::TELEPORT_NONE) // never raycast mid-teleport
    {
        // The margin keeps the near clip plane out of the surface the camera stopped in front of, so it
        // has to stay at or above the near clip or the plane pokes through and the wall goes see-through.
        const F32 OTS_COLLISION_MARGIN = llmax(DEFAULT_NEAR_PLANE, LLViewerCamera::getInstance()->getNear() + 0.05f);
        const F32 OTS_MIN_CAMERA_DISTANCE = 0.30f;          // never pull in closer than just behind the head
        const F32 OTS_COLLISION_EASE_OUT_HALF_LIFE = 0.15f; // seconds; recovery speed when an obstruction clears

        const LLVector3 pivot = avatar_pos_agent + LLVector3(0.f, 0.f, pivot_height);
        LLVector3 dir = cam_pos_agent - pivot;
        const F32 desired_dist = dir.normalize();
        F32 target_dist = desired_dist;

        // GPU pick (SSCombatRay): objects answer ~2 frames late, terrain at once. With no recent object
        // answer, hold the last distance instead of easing out.
        F32 hit_dist = desired_dist;
        bool known = false;
        if (SSCombatRay::cast(SSCombatRay::KEY_OTS_COLLISION, pivot, cam_pos_agent, seated_root, hit_dist, &known))
        {
            target_dist = llclamp(hit_dist - OTS_COLLISION_MARGIN, OTS_MIN_CAMERA_DISTANCE, desired_dist);
        }
        else if (!known && sCollisionDistance >= 0.f)
        {
            target_dist = llmin(sCollisionDistance, desired_dist);
        }

        // Asymmetric smoothing: snap in instantly so the camera never clips, but ease back out so uneven
        // surfaces don't jitter the camera while turning or strafing.
        if (sCollisionDistance < 0.f || target_dist <= sCollisionDistance)
        {
            sCollisionDistance = target_dist;
        }
        else
        {
            const F32 ease_amt = LLSmoothInterpolation::getInterpolant(OTS_COLLISION_EASE_OUT_HALF_LIFE);
            sCollisionDistance = lerp(sCollisionDistance, target_dist, ease_amt);
        }

        cam_pos_agent = pivot + dir * llmin(sCollisionDistance, desired_dist);
    }
    else
    {
        sCollisionDistance = -1.f; // restart smoothing fresh if collision is re-enabled
    }

    // RLVa camera distance cap (@setcam_avdistmax / @setcam_origindistmax): pull the camera toward the
    // head so it never sits further out than allowed, whatever the distance setting (or an @setdebug
    // write to it) says. A cap too small for a shoulder camera drops it altogether (update, enter); the
    // minimums already forbid mouselook, and so the shoulder.
    F32 rlv_max_dist = F32_MAX;
    if (rlv_max_distance(rlv_max_dist) && isAgentAvatarValid() && gAgentAvatarp->mHeadp)
    {
        const LLVector3 head = gAgentAvatarp->mHeadp->getWorldPosition();
        LLVector3 from_head = cam_pos_agent - head;
        const F32 head_dist = from_head.normalize();
        if (head_dist > rlv_max_dist)
        {
            cam_pos_agent = head + from_head * rlv_max_dist;
            constrained = true;
        }
    }

    sRenderedOnShoulder = true;
    return gAgent.getPosGlobalFromAgent(cam_pos_agent);
}

void GMDXOTS::setSitCameraEye(const LLVector3& eye)
{
    sSitCameraEye = eye;
}
