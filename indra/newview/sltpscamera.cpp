/**
 * @file sltpscamera.cpp
 * @brief Third-person shooter (TPS) camera mode
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Alchemy Viewer Source Code
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

#include "sltpscamera.h"
#include "rlvactions.h"

#include "llcriticaldamp.h"
#include "llfocusmgr.h"
#include "llfontgl.h"
#include "llrender.h"
#include "llkeyboard.h"
#include "llui.h"
#include "lluiimage.h"
#include "llwindow.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llappviewer.h"
#include "llfloatercamera.h"
#include "llfloaterreg.h"
#include "llfollowcam.h"
#include "llstartup.h"
#include "lltoolmgr.h"
#include "lltoolpie.h"
#include "lltrans.h"
#include "llselectmgr.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerjoystick.h"
#include "llviewerwindow.h"
#include "llvoavatarself.h"
#include "fscombatads.h"
#include "fscombataim.h"
#include "slcombatfx.h"
#include "sscombatray.h"
#include "rlvactions.h"

namespace
{
    LLFloater* preferences_for_view(const LLView* view)
    {
        if (!view) return nullptr;
        for (const char* name : {"preferences", "quick_prefs", "prefs_view_advanced"})
        {
            LLFloater* floater = LLFloaterReg::findInstance(name);
            if (floater && floater->getVisible() && (view == floater || view->hasAncestor(floater))) return floater;
        }
        return nullptr;
    }

    bool preferences_visible()
    {
        return LLFloaterReg::instanceVisible("preferences") || LLFloaterReg::instanceVisible("quick_prefs")
            || LLFloaterReg::instanceVisible("prefs_view_advanced");
    }
    // How fast the avatar turns toward the movement direction (half-life, seconds)
    constexpr F32 TPS_TURN_HALF_LIFE = 0.05f;
    // Body pitch limit while flying (keeps resetAxes away from the degenerate
    // look_at == up case, which it silently rejects)
    constexpr F32 TPS_BODY_PITCH_LIMIT = 80.f * DEG_TO_RAD;
    // Direction component needed before a movement control bit is sent. While
    // the body is still turning toward the target direction, the move is split
    // into forward/sideways bits so the avatar travels the right way at once.
    constexpr F32 TPS_MOVE_COMPONENT_THRESHOLD = 0.38f; // ~cos(67.5 deg)
    // How long the "right-click or Esc to release" hint stays up
    constexpr F32 TPS_HINT_SECONDS = 5.f;
    constexpr F32 TPS_HINT_FADE_SECONDS = 1.f;

    F32 yaw_of(const LLVector3& v)
    {
        return atan2f(v.mV[VY], v.mV[VX]);
    }

    // +pitch looks down (LLAgent::pitch convention)
    F32 pitch_of(const LLVector3& v)
    {
        LLVector3 n(v);
        n.normalize();
        return asinf(llclamp(-n.mV[VZ], -1.f, 1.f));
    }

    LLVector3 dir_from(F32 yaw, F32 pitch)
    {
        const F32 cp = cosf(pitch);
        return LLVector3(cp * cosf(yaw), cp * sinf(yaw), -sinf(pitch));
    }

    F32 wrap_pi(F32 a)
    {
        while (a > F_PI)  a -= F_TWO_PI;
        while (a < -F_PI) a += F_TWO_PI;
        return a;
    }

    // Roll-free rotation whose at-axis is look_at (world +Z up), as LLAgent::resetAxes.
    bool rotation_from_at(const LLVector3& look_at, LLQuaternion& rot)
    {
        LLVector3 at(look_at);
        if (at.normalize() < 0.001f)
        {
            return false;
        }
        LLVector3 left = LLVector3::z_axis % at;
        if (left.normalize() < 0.001f)
        {
            return false; // straight up/down
        }
        const LLVector3 up = at % left;
        rot = LLQuaternion(at, left, up);
        return true;
    }

    // Where the crosshair points: the real camera at-axis (screen centre).
    LLVector3 aim_axis()
    {
        return LLViewerCamera::getInstance()->getAtAxis();
    }

    // Turn the body to the aim heading (and, flying, the aim pitch) right now.
    void face_aim_now()
    {
        if (gAgent.rotateGrabbed())
        {
            return;
        }
        const LLVector3 aim = aim_axis();
        const F32 pitch = gAgent.getFlying() ? llclamp(pitch_of(aim), -TPS_BODY_PITCH_LIMIT, TPS_BODY_PITCH_LIMIT) : 0.f;
        gAgent.resetAxes(dir_from(yaw_of(aim), pitch));
    }
}

//=============================================================================
// SLTPSCamera
//=============================================================================

SLTPSCamera::SLTPSCamera()
{
    if (LLControlVariable* control = gSavedSettings.getControl("SlipstreamTPSCamera"))
    {
        mSettingConnection = control->getSignal()->connect(
            [this](LLControlVariable*, const LLSD& new_value, const LLSD&) { onSettingChanged(new_value); });
    }
    if (LLControlVariable* control = gSavedSettings.getControl("SlipstreamTPSCameraMode"))
    {
        mFramingConnection = control->getSignal()->connect(
            [this](LLControlVariable*, const LLSD&, const LLSD&)
            {
                mCollisionDistance = -1.f;
                if (mEngaged) gAgentCamera.startCameraAnimation();
            });
    }
}

SLTPSCamera::~SLTPSCamera()
{
}

// static
bool SLTPSCamera::isEnabled()
{
    static LLCachedControl<bool> enabled(gSavedSettings, "SlipstreamTPSCamera", false);
    return enabled;
}

// static
bool SLTPSCamera::isEngagedNow()
{
    return instanceExists() && getInstance()->isEngaged();
}

// static
bool SLTPSCamera::isSeated()
{
    return isAgentAvatarValid() && (gAgentAvatarp->isSitting() || gAgentAvatarp->getParent());
}

// static
bool SLTPSCamera::isEngagedOnFoot()
{
    return isEngagedNow() && !getInstance()->mInputSuspended && !isSeated();
}

// static
void SLTPSCamera::disengageIfEngaged(EDisengageReason reason)
{
    if (instanceExists())
    {
        getInstance()->disengage(reason);
    }
}

// static
void SLTPSCamera::applyAimToAgentUpdate(LLQuaternion& body_rotation, LLQuaternion& head_rotation)
{
    // On foot only: seated, the vehicle owns the frame (#95); flying, BodyRotation
    // pitch steers the flight, and TPS already pitches the body there itself.
    if (!isEngagedOnFoot() || !gAgentCamera.tpsCameraActive() || gAgent.getFlying())
    {
        return;
    }
    LLVector3 aim = aim_axis();
    if (SLCombatFX::enabled() && isShoulderActive() && gAgentAvatarp->mHeadp)
    {
        aim = FSCombatAim::getOTSConvergenceTarget(LLViewerCamera::getInstance()->getOrigin(), aim)
            - gAgentAvatarp->mHeadp->getWorldPosition();
        aim.normalize();
    }

    // HeadRotation: the camera look direction, like LLAgent::getHeadRotation in
    // mouselook (the avatar's head already looks there via the look-at target).
    LLQuaternion head;
    if (rotation_from_at(aim, head))
    {
        head_rotation = head;
    }

    // BodyRotation: the avatar's real heading, pitched to the aim -- what mouselook
    // sends. Walking with a pitched body is what mouselook does too; the simulator
    // keeps the avatar upright, and LLVOAvatar::updateOrientation flattens it locally.
    const F32 body_yaw = yaw_of(LLVector3::x_axis * body_rotation);
    const F32 aim_pitch = llclamp(pitch_of(aim), -TPS_BODY_PITCH_LIMIT, TPS_BODY_PITCH_LIMIT);
    LLQuaternion body;
    if (rotation_from_at(dir_from(body_yaw, aim_pitch), body))
    {
        body_rotation = body;
    }
}

// static
bool SLTPSCamera::isShoulderMode()
{
    // <GMDX> No shoulder framing in GunMetal-DX: its over-the-shoulder view is its own OTS aim camera
    // (gmdxots.h, real mouselook with the camera at the shoulder), so TPS frames behind the avatar only,
    // whatever SlipstreamTPSCameraMode says (a GunMetal-DX 0.3.0 user migrated to 1 there).
    return false;
    //static LLCachedControl<U32> mode(gSavedSettings, "SlipstreamTPSCameraMode", BEHIND_AVATAR);
    //return mode == OVER_SHOULDER;
    // </GMDX>
}

// static
bool SLTPSCamera::isShoulderActive()
{
    return isEngagedNow() && gAgentCamera.tpsCameraActive() && isShoulderMode();
}

// static
bool SLTPSCamera::isADSReturning()
{
    return FSCombatADS::returnsToTPS();
}

namespace
{
LLVector3 shoulder_pivot()
{
    LLVector3 pivot;
    if (!gAgentCamera.getTPSVehiclePivot(pivot))
    {
        pivot = gAgentCamera.getAvatarRootPosition();
    }
    return pivot;
}

F32 framing_value(const char* name, F32 lower, F32 upper)
{
    const F32 value = gSavedSettings.getF32(name);
    return llfinite(value) ? llclamp(value, lower, upper) : lower;
}
}

LLVector3 SLTPSCamera::getShoulderFocus() const
{
    const LLVector3 local(framing_value("SlipstreamTPSFocusDistance", 2.f, 50.f),
                          framing_value("SlipstreamTPSCameraSide", -2.f, 2.f),
                          framing_value("SlipstreamTPSCameraHeight", -1.f, 3.f));
    return shoulder_pivot() + local * gAgentCamera.getTPSRotation();
}

LLVector3 SLTPSCamera::getShoulderPosition()
{
    const F32 height = framing_value("SlipstreamTPSCameraHeight", -1.f, 3.f);
    const LLVector3 root = shoulder_pivot();
    const LLVector3 local(-framing_value("SlipstreamTPSCameraDistance", 0.5f, 10.f),
                          framing_value("SlipstreamTPSCameraSide", -2.f, 2.f), height);
    LLVector3 position = root + local * gAgentCamera.getTPSRotation();
    static LLCachedControl<bool> collision(gSavedSettings, "SlipstreamTPSCameraCollision", true);
    if (SLCombatFX::enabled() && collision && gAgent.getTeleportState() == LLAgent::TELEPORT_NONE)
    {
        const LLVector3 pivot = root + LLVector3(0.f, 0.f, height);
        LLVector3 direction = position - pivot;
        const F32 desired = direction.normalize();
        F32 target = desired;
        F32 hit_distance = desired;
        bool known = false;
        const LLViewerObject* skip_root = isSeated() ? (const LLViewerObject*)gAgentAvatarp->getRoot() : nullptr;
        const F32 margin = llmax(DEFAULT_NEAR_PLANE, LLViewerCamera::getInstance()->getNear() + 0.05f);
        if (SSCombatRay::cast(SSCombatRay::KEY_OTS_COLLISION, pivot, position, skip_root, hit_distance, &known))
        {
            target = llclamp(hit_distance - margin, llmin(0.30f, desired), desired);
        }
        else if (!known && mCollisionDistance >= 0.f)
        {
            target = llmin(mCollisionDistance, desired);
        }
        // Snap in to avoid clipping; recover only when the GPU knows the segment is clear.
        mCollisionDistance = mCollisionDistance < 0.f || target <= mCollisionDistance ? target
            : lerp(mCollisionDistance, target, LLSmoothInterpolation::getInterpolant(0.15f));
        position = pivot + direction * llmin(mCollisionDistance, desired);
    }
    else
    {
        mCollisionDistance = -1.f;
    }
    return constrainPosition(position);
}

// The final rendered camera is clamped too, so animation and smoothing cannot
// temporarily escape a restriction that arrived while TPS was already engaged.
// static
LLVector3 SLTPSCamera::constrainPosition(const LLVector3& position)
{
    if (!RlvActions::isRlvEnabled() || !RlvActions::isCameraDistanceClamped()) return position;
    LLVector3 result = position;
    const LLVector3 head = isAgentAvatarValid() && gAgentAvatarp->mHeadp
        ? gAgentAvatarp->mHeadp->getWorldPosition() : gAgent.getPositionAgent();
    auto clamp_distance = [&result](const LLVector3& reference, F32 lower, F32 upper)
    {
        LLVector3 direction = result - reference;
        const F32 distance = direction.normalize();
        if (distance < 0.001f) direction = -LLVector3::x_axis * gAgentCamera.getTPSYawRotation();
        result = reference + direction * llclamp(distance, lower, upper);
    };
    F32 av_min = 0.f, av_max = F32_MAX, origin_min = 0.f, origin_max = F32_MAX;
    const bool avatar_limit = RlvActions::getCameraAvatarDistanceLimits(av_min, av_max);
    const bool origin_limit = RlvActions::getCameraOriginDistanceLimits(origin_min, origin_max);
    // Shoulder focus is an aim target, not a camera origin. Its distance limits
    // are measured from the avatar's head, as for the convergence camera.
    if (isShoulderMode())
    {
        const F32 lower = llmax(avatar_limit ? av_min : 0.f, origin_limit ? origin_min : 0.f);
        const F32 upper = llmin(avatar_limit ? av_max : F32_MAX, origin_limit ? origin_max : F32_MAX);
        if (avatar_limit || origin_limit) clamp_distance(head, llmin(lower, upper), upper);
    }
    else
    {
        if (origin_limit && !(avatar_limit && av_min == av_max))
            clamp_distance(gAgentCamera.getTPSCameraOrigin(), origin_min, origin_max);
        if (avatar_limit && (av_min == av_max || !origin_limit)) clamp_distance(head, av_min, av_max);
    }
    return result;
}

void SLTPSCamera::zoomShoulder(F32 meters)
{
    gSavedSettings.setF32("SlipstreamTPSCameraDistance",
        llclamp(framing_value("SlipstreamTPSCameraDistance", 0.5f, 10.f) - meters, 0.5f, 10.f));
}

void SLTPSCamera::scrollShoulder(F32 notches)
{
    const F32 distance = framing_value("SlipstreamTPSCameraDistance", 0.5f, 10.f);
    zoomShoulder(distance * (1.f - powf(sqrtf(F_SQRT2), notches)));
}

bool SLTPSCamera::swapShoulder()
{
    if (!mEngaged || !gAgentCamera.tpsCameraActive() || !isShoulderMode()) return false;
    const char* setting = "SlipstreamTPSCameraSide";
    const F32 side = gSavedSettings.getF32(setting);
    if (side == 0.f) return false;
    gSavedSettings.setF32(setting, -side);
    mCollisionDistance = -1.f;
    gAgentCamera.startCameraAnimation();
    return true;
}

bool SLTPSCamera::returnFromADS(bool animate)
{
    // Restore the current persisted TPS choice without changing it. A sit camera,
    // teleport, build tool, focus loss or a new restriction cancels the return.
    if (!isEnabled() || !gViewerWindow || !gFocusMgr.getAppHasFocus()
        || gAgentCamera.getCameraMode() != CAMERA_MODE_MOUSELOOK) return false;
    gAgentCamera.changeCameraToThirdPerson(false);
    if (!canEngage()) return false;
    const LLVector3 aim = LLViewerCamera::getInstance()->getAtAxis();
    if (!engage()) return false;
    gAgentCamera.setTPSCameraEngaged(true, yaw_of(aim), pitch_of(aim));
    if (animate) gAgentCamera.startCameraAnimation();
    return true;
}

void SLTPSCamera::onSettingChanged(const LLSD& new_value)
{
    // #795: enabling only arms the feature (mouse stays free until the user engages
    // it with a self click, Esc or the keybind). Disabling releases at once.
    if (!new_value.asBoolean())
    {
        // Only an already engaged camera suspended by this floater can be
        // restored when the setting is switched back on (including Cancel).
        const bool retain_preview = mEngaged && mInputSuspended && mPreviewFloater.get();
        const LLHandle<LLFloater> floater = mPreviewFloater;
        const LLVector3 aim = LLVector3::x_axis * gAgentCamera.getTPSRotation();
        mPendingEngage = false;
        disengage(DISENGAGE_USER);
        if (retain_preview)
        {
            mPreviewFloater = floater;
            mPreviewParked = true;
            mPreviewYaw = yaw_of(aim);
            mPreviewPitch = pitch_of(aim);
            mPreviewFocus = gAgentCamera.getFocusTargetGlobal();
            mPreviewOffset = gAgentCamera.getCameraFocusOffsetTarget();
        }
    }
    else if (mPreviewParked)
    {
        if (parkedPreviewValid())
        {
            gAgentCamera.setFocusOnAvatar(true, false, false);
            gAgentCamera.setTPSCameraEngaged(true, mPreviewYaw, mPreviewPitch);
            mEngaged = true;
            mInputSuspended = true;
            mCollisionDistance = -1.f;
        }
        mPreviewParked = false;
    }
}

void SLTPSCamera::toggleEngaged()
{
    if (mEngaged)
    {
        disengage(DISENGAGE_USER);
        return;
    }
    if (!isEnabled())
    {
        // the keybind turns the feature on and engages it in one press
        gSavedSettings.setBOOL("SlipstreamTPSCamera", true);
    }
    requestEngage();
}

bool SLTPSCamera::canEngage() const
{
    return isEnabled() && canUseCamera();
}

bool SLTPSCamera::canUseCamera() const
{
    return !gDisconnected
        && LLStartUp::getStartupState() == STATE_STARTED
        && gViewerWindow
        && gFocusMgr.getAppHasFocus()
        && isAgentAvatarValid()
        // Seated / vehicles are supported (#95). A script-defined sit camera
        // (llSetCameraEyeOffset/AtOffset) or follow camera (llSetCameraParams) wins:
        // TPS yields (the follow camera is not CAMERA_MODE_THIRD_PERSON, below).
        && !gAgentCamera.sitCameraEnabled()
        && !LLFollowCamMgr::getInstance()->getActiveFollowCamParams()
        && !RlvActions::isCameraPresetLocked()
        && gAgentCamera.getCameraMode() == CAMERA_MODE_THIRD_PERSON
        && gAgent.getTeleportState() == LLAgent::TELEPORT_NONE
        && !LLToolMgr::getInstance()->inBuildMode()
        && !LLFloaterCamera::inFreeCameraMode()
        && !LLViewerJoystick::getInstance()->getOverrideCamera();
}

bool SLTPSCamera::parkedPreviewValid() const
{
    LLFloater* floater = mPreviewFloater.get();
    LLMouseHandler* captor = gFocusMgr.getMouseCapture();
    // Do not reclaim a free camera that was moved or assigned to another owner.
    return mPreviewParked && floater && floater->getVisible() && canUseCamera()
        && !gAgentCamera.getFocusOnAvatar()
        && (!captor || preferences_for_view(dynamic_cast<LLView*>(captor)))
        && (gAgentCamera.getFocusTargetGlobal() - mPreviewFocus).magVecSquared() < 0.000001
        && (gAgentCamera.getCameraFocusOffsetTarget() - mPreviewOffset).magVecSquared() < 0.000001;
}

bool SLTPSCamera::canStayEngaged() const
{
    // tpsCameraActive() additionally requires focus-on-avatar third person
    return canEngage() && gAgentCamera.tpsCameraActive();
}

bool SLTPSCamera::engage()
{
    mPendingEngage = false;
    if (mEngaged)
    {
        return true;
    }
    if (!canEngage())
    {
        return false;
    }

    // e.g. a pending mouse-steer camera transient from a self click
    LLToolMgr::getInstance()->clearTransientTool();

    F32 yaw = 0.f;
    F32 pitch = 0.f;
    if (gAgentCamera.getFocusOnAvatar())
    {
        // The stock third person camera is rigidly attached to the agent frame,
        // so starting from the agent frame is seamless.
        const LLVector3 at = gAgent.getAtAxis();
        yaw = yaw_of(at);
        pitch = pitch_of(at);
    }
    else
    {
        // Free camera (alt-cam, or a previous TPS release): keep looking the same
        // way, re-attach to the avatar without turning the avatar.
        yaw = yaw_of(LLViewerCamera::getInstance()->getAtAxis());
        gAgentCamera.setFocusOnAvatar(true, ANIMATE, /*reset_axes*/ false);
    }
    gAgentCamera.setTPSCameraEngaged(true, yaw, pitch);

    mCollisionDistance = -1.f;
    mEngaged = true;
    mInputSuspended = false;
    mLeftDown = false;
    mLeftFacesAim = false;
    resetMoveInput();

    // Own the mouse: every click/hover/scroll comes to SLToolTPS, and while
    // anything holds capture LLViewerWindow::updateMouseDelta keeps SDL
    // relative mouse mode alive. Hide first: moveCursorToCenter only enters
    // relative mode (clean Wayland deltas) when the cursor is hidden.
    SLToolTPS::getInstance()->setMouseCapture(true);
    gViewerWindow->hideCursor();
    gViewerWindow->moveCursorToCenter();
    gViewerWindow->getWindow()->setMouseClipping(true);

    mHintTimer.reset();
    mHintTimer.start();

    LL_INFOS("TPSCamera") << "Third-person camera engaged" << LL_ENDL;
    return true;
}

void SLTPSCamera::disengage(EDisengageReason reason)
{
    mPendingEngage = false;
    mPreviewParked = false;
    mPreviewFloater = LLHandle<LLFloater>();
    if (!mEngaged)
    {
        return;
    }
    // Clear first: releasing capture calls back into onMouseCaptureLost.
    mEngaged = false;
    mInputSuspended = false;
    mCollisionDistance = -1.f;
    if (!FSCombatADS::isCameraSwap())
    {
        FSCombatADS::cancel();
    }

    setLeftButton(false);
    if (mRightHeld && gViewerWindow) gViewerWindow->consumeTPSRightMouseUp();
    mRightHeld = false;
    resetMoveInput();
    mHintTimer.stop();

    // "Leave the camera where it is": the TPS view generally differs from the
    // agent frame, so falling back to the stock follow camera would snap it
    // behind the avatar. Park it as a free camera instead; the next movement
    // key returns it behind the avatar exactly like after an alt-cam.
    const bool keep_camera = (reason == DISENGAGE_USER) && gAgentCamera.tpsCameraActive();
    if (keep_camera)
    {
        const LLVector3d cam_pos = gAgentCamera.getCameraPositionGlobal();
        const LLVector3d focus = gAgentCamera.getFocusGlobal();
        gAgentCamera.setFocusOnAvatar(false, false);
        gAgentCamera.setTPSCameraEngaged(false);
        gAgentCamera.setCameraPosAndFocusGlobal(cam_pos, focus, LLUUID::null);
        gAgentCamera.setLookAt(LOOKAT_TARGET_CLEAR);
    }
    else
    {
        if (reason == DISENGAGE_MOUSELOOK && gAgentCamera.tpsCameraActive() && !gAgent.rotateGrabbed())
        {
            // Mouselook builds its view from the agent frame: face where the
            // TPS camera was looking so "M to aim" keeps the aim direction.
            gAgent.resetAxes(LLVector3::x_axis * gAgentCamera.getTPSRotation());
        }
        gAgentCamera.setTPSCameraEngaged(false);
    }

    SLToolTPS* tool = SLToolTPS::getInstance();
    if (tool->hasMouseCapture())
    {
        tool->setMouseCapture(false);
    }

    if (reason != DISENGAGE_MOUSELOOK && gViewerWindow)
    {
        // Same order as LLToolGun::handleDeselect: SDL restores the pointer to
        // the last warped (center) position when relative mode lapses.
        gViewerWindow->moveCursorToCenter();
        gViewerWindow->showCursor();
        gViewerWindow->getWindow()->setMouseClipping(false);
    }

    LL_INFOS("TPSCamera") << "Third-person camera released (reason " << (S32)reason << ")" << LL_ENDL;
}

void SLTPSCamera::setLeftButton(bool down, bool face_aim)
{
    if (down == mLeftDown)
    {
        return;
    }
    if (down && RlvActions::isRlvEnabled() && !RlvActions::canChangeToMouselook()) return;
    mLeftDown = down;
    mLeftFacesAim = down && face_aim;
    if (down && face_aim && mEngaged && !isSeated())
    {
        // #881: shooting turns the avatar to the aim heading (shooter style) before
        // this frame's AgentUpdate goes out -- the control change forces it -- so
        // llGetRot already points where the crosshair does when the gun's control
        // event fires. While held, updateMovement keeps facing the aim.
        face_aim_now();
    }
    if (down)
    {
        // TPS aim must reach weapons taking the mouselook control even though
        // the render camera is third person. Also support ordinary mouse-control
        // scripts, including controls taken with pass_on. Always report shooter
        // fire as ML_LBUTTON, plus LBUTTON when subscribed. Deliver both in the
        // same packet and latch through release even if subscriptions change.
        mLeftControlDown = AGENT_CONTROL_ML_LBUTTON_DOWN;
        if (gAgent.leftButtonGrabbed(false)) mLeftControlDown |= AGENT_CONTROL_LBUTTON_DOWN;
    }
    const U32 edge = down ? mLeftControlDown : (mLeftControlDown << 1);
    queueFireEdge(edge);
    gAgent.setControlFlags(edge);
}

void SLTPSCamera::queueFireEdge(U32 flags)
{
    const U32 down_mask = AGENT_CONTROL_ML_LBUTTON_DOWN | AGENT_CONTROL_LBUTTON_DOWN;
    mFireOwnedDown |= (flags & down_mask) ? flags : (flags >> 1);
    if (mFireCount == kFireQueueCapacity)
    {
        // Bound pathological input bursts without leaving a script held. Preserve
        // the last transmitted release and the final physical state, not the burst.
        mFireHead = mFireCount = 0;
        gAgent.clearControlFlags(mFireOwnedDown | (mFireOwnedDown << 1));
        if (mWireFireDown) queueFireEdge(mWireFireDown << 1);
        if (mLeftDown) queueFireEdge(mLeftControlDown);
        return;
    }
    mFireEdges[(mFireHead + mFireCount++) % kFireQueueCapacity] = { flags, ++mNextFireEdge };
}

void SLTPSCamera::cancelFire()
{
    mLeftDown = mLeftFacesAim = false;
    mFireHead = mFireCount = 0;
    gAgent.clearControlFlags(mFireOwnedDown | (mFireOwnedDown << 1));
    // Unsent presses are cancelled. A transmitted hold gets one matching release.
    if (mWireFireDown) queueFireEdge(mWireFireDown << 1);
}

bool SLTPSCamera::applyFireToAgentUpdate(U32& control_flags)
{
    if (!instanceExists()) return false;
    SLTPSCamera* camera = getInstance();
    camera->mPackedFireEdge = 0;
    camera->mPackedNativeEdge = 0;
    const U32 ml_down = AGENT_CONTROL_ML_LBUTTON_DOWN;
    const U32 ml_up = AGENT_CONTROL_ML_LBUTTON_UP;
    if ((camera->mFireCount || camera->mWireFireDown) && gAgentCamera.cameraMouselook()
        && (control_flags & ml_down))
    {
        // A new native press can arrive before TPS's pending release is sent.
        // Preserve it across that cleanup and the next ephemeral reset.
        camera->mDeferredNativeDown = true;
    }
    if (!camera->mFireCount && !camera->mWireFireDown)
    {
        if (!camera->mDeferredNativeDown && !camera->mDeferredNativeUp) return false;
        const MASK mask = gKeyboard ? gKeyboard->currentMask(true) : MASK_NONE;
        if (!gAgentCamera.cameraMouselook() || !gFocusMgr.getAppHasFocus()
            || (mask & (MASK_ALT | MASK_CONTROL))
            || (RlvActions::isRlvEnabled() && !RlvActions::canChangeToMouselook()))
        {
            camera->mDeferredNativeDown = camera->mDeferredNativeUp = false;
            return false;
        }
        if (control_flags & ml_up) camera->mDeferredNativeUp = true;
        const U32 edge = camera->mDeferredNativeDown ? ml_down : ml_up;
        control_flags = (control_flags & ~(ml_down | ml_up)) | edge;
        camera->mPackedNativeEdge = edge;
        return false; // Native mouselook supplies its own genuine camera context.
    }
    const MASK modifiers = gKeyboard ? gKeyboard->currentMask(true) : MASK_NONE;
    if (!camera->mEngaged || !camera->canStayEngaged()
        || !gFocusMgr.getAppHasFocus() || (modifiers & (MASK_ALT | MASK_CONTROL))
        || !SLToolTPS::getInstance()->hasMouseCapture()
        || (RlvActions::isRlvEnabled() && !RlvActions::canChangeToMouselook()))
    {
        camera->cancelFire();
    }
    // One owner at the packet boundary. Native handlers cannot end a TPS hold;
    // real transitions remain ordered even when several arrive between frames.
    const U32 owned = camera->mFireOwnedDown;
    control_flags &= ~(owned | (owned << 1));
    if (!camera->mFireCount && !camera->mWireFireDown) return false;
    // Button bits are edges, as resetControlFlags makes them for native input: the
    // press goes out once and the simulator holds it until the release. Repeating
    // the DOWN bits during a hold reads as fresh presses and restarts weapon loops.
    U32 buttons = 0;
    if (camera->mFireCount)
    {
        const FireEdge& edge = camera->mFireEdges[camera->mFireHead];
        buttons = edge.flags;
        camera->mPackedFireEdge = edge.serial;
    }
    control_flags |= buttons;

    // Supply the mouselook context expected by ML-controlled weapons. This affects
    // only the wire snapshot: keep the local camera, input owner and agent flags
    // in TPS. Include the final UP in that context, then clear it next update.
    control_flags |= AGENT_CONTROL_MOUSELOOK;
    return ((buttons | camera->mWireFireDown) & (AGENT_CONTROL_ML_LBUTTON_DOWN | AGENT_CONTROL_LBUTTON_DOWN)) != 0;
}

void SLTPSCamera::fireAgentUpdateSent(U32 control_flags)
{
    if (!instanceExists()) return;
    SLTPSCamera* camera = getInstance();
    if (camera->mPackedNativeEdge && (control_flags & camera->mPackedNativeEdge))
    {
        if (camera->mPackedNativeEdge == AGENT_CONTROL_ML_LBUTTON_DOWN) camera->mDeferredNativeDown = false;
        else camera->mDeferredNativeUp = false;
        gAgent.clearControlFlags(AGENT_CONTROL_ML_LBUTTON_DOWN | AGENT_CONTROL_ML_LBUTTON_UP);
        camera->mPackedNativeEdge = 0;
    }
    if (!camera->mFireCount) return;
    const FireEdge edge = camera->mFireEdges[camera->mFireHead];
    if (edge.serial != camera->mPackedFireEdge || (control_flags & edge.flags) != edge.flags) return;
    const U32 down_mask = AGENT_CONTROL_ML_LBUTTON_DOWN | AGENT_CONTROL_LBUTTON_DOWN;
    const bool down = (edge.flags & down_mask) != 0;
    if (down) camera->mWireFireDown = edge.flags;
    else camera->mWireFireDown &= ~(edge.flags >> 1);
    camera->mFireHead = (camera->mFireHead + 1) % kFireQueueCapacity;
    --camera->mFireCount;
    gAgent.clearControlFlags(down ? (edge.flags | (edge.flags << 1)) : (edge.flags | (edge.flags >> 1)));
    if (!camera->mFireCount && !camera->mWireFireDown) camera->mFireOwnedDown = 0;
}

// SlipstreamTPSRightDragSteers: a right press while engaged does not release the camera.
// The mouse already steers the view; a release that travelled no more than the right-drag
// slop was a click, so the camera is handed back. This release gesture is consumed;
// a subsequent distinct right click can open the context menu.
void SLTPSCamera::setRightButton(bool down)
{
    if (down)
    {
        mRightHeld = mEngaged;
        mRightTravelX = 0;
        mRightTravelY = 0;
        mRightDownTime = LLFrameTimer::getTotalSeconds();
        return;
    }
    if (!mRightHeld)
    {
        return;
    }
    mRightHeld = false;
    // While engaged the mouse steers the view, so a click almost always travels a few pixels:
    // a press released within 250 ms is a click (as #861 decided for right-click orbit); a
    // longer hold is a click only if it stayed inside the slop.
    static LLCachedControl<S32> slop_setting(gSavedSettings, "RightClickOrbitSlopPx", 3);
    const S32 slop = llclamp((S32)slop_setting, 0, 24);
    const bool quick = LLFrameTimer::getTotalSeconds() - mRightDownTime <= 0.25;
    if (quick || (mRightTravelX <= slop && mRightTravelY <= slop))
    {
        disengage(DISENGAGE_USER);
    }
}

void SLTPSCamera::addMoveInput(S32 at, S32 left, bool nudge)
{
    mInputAt = llclamp(mInputAt + at, -1, 1);
    mInputLeft = llclamp(mInputLeft + left, -1, 1);
    mInputNudge = mInputNudge && nudge;
}

void SLTPSCamera::resetMoveInput()
{
    mInputAt = 0;
    mInputLeft = 0;
    mInputNudge = true;
}

bool SLTPSCamera::suspendForPreferences()
{
    if (!mEngaged || !canStayEngaged()) return false;
    LLFloater* floater = preferences_for_view(dynamic_cast<LLView*>(gFocusMgr.getMouseCapture()));
    if (!floater) floater = preferences_for_view(dynamic_cast<LLView*>(gFocusMgr.getKeyboardFocus()));
    if (!floater) return false;
    mPreviewFloater = floater->getHandle();
    // Keep the existing camera owner and rotation, but give all input to UI.
    mInputSuspended = true;
    FSCombatADS::cancel();
    setLeftButton(false);
    if (mRightHeld && gViewerWindow) gViewerWindow->consumeTPSRightMouseUp();
    mRightHeld = false;
    resetMoveInput();
    gAgent.clearControlFlags(AGENT_CONTROL_AT_POS | AGENT_CONTROL_AT_NEG | AGENT_CONTROL_FAST_AT
        | AGENT_CONTROL_LEFT_POS | AGENT_CONTROL_LEFT_NEG | AGENT_CONTROL_FAST_LEFT
        | AGENT_CONTROL_NUDGE_AT_POS | AGENT_CONTROL_NUDGE_AT_NEG
        | AGENT_CONTROL_NUDGE_LEFT_POS | AGENT_CONTROL_NUDGE_LEFT_NEG);
    mHintTimer.stop();
    if (SLToolTPS::getInstance()->hasMouseCapture()) SLToolTPS::getInstance()->setMouseCapture(false);
    if (gViewerWindow)
    {
        gViewerWindow->showCursor();
        gViewerWindow->getWindow()->setMouseClipping(false);
    }
    return true;
}

void SLTPSCamera::update()
{
    if (mPendingEngage && !mEngaged)
    {
        engage();
    }
    mPendingEngage = false;

    if (!mEngaged)
    {
        if (mPreviewParked && !parkedPreviewValid())
        {
            mPreviewParked = false;
            mPreviewFloater = LLHandle<LLFloater>();
        }
        resetMoveInput();
        return;
    }

    if (!isEnabled())
    {
        disengage(DISENGAGE_USER);
        return;
    }

    if (!canStayEngaged())
    {
        disengage(DISENGAGE_MODE_CHANGE);
        return;
    }

    if (!mInputSuspended && preferences_for_view(dynamic_cast<LLView*>(gFocusMgr.getKeyboardFocus())))
    {
        suspendForPreferences();
    }
    if (mInputSuspended)
    {
        resetMoveInput();
        LLMouseHandler* captor = gFocusMgr.getMouseCapture();
        if (captor && !preferences_for_view(dynamic_cast<LLView*>(captor)))
        {
            disengage(DISENGAGE_USER);
        }
        else if (!preferences_visible() && !captor)
        {
            // Resume only the engagement that was suspended, after UI lets go.
            mInputSuspended = false;
            mPreviewFloater = LLHandle<LLFloater>();
            SLToolTPS::getInstance()->setMouseCapture(true);
            gViewerWindow->hideCursor();
            gViewerWindow->moveCursorToCenter();
            gViewerWindow->getWindow()->setMouseClipping(true);
        }
        return;
    }

    if (!SLToolTPS::getInstance()->hasMouseCapture())
    {
        // something took the mouse (modal dialog, drag, ...): give the pointer back
        disengage(DISENGAGE_USER);
        return;
    }

    FSCombatADS::update();
    updateMouse();
    updateMovement();
    resetMoveInput();
}

void SLTPSCamera::updateMouse()
{
    // Something (a tool, resetView, ...) may have re-shown the pointer.
    if (!gViewerWindow->getCursorHidden())
    {
        gViewerWindow->hideCursor();
        gViewerWindow->moveCursorToCenter();
    }

    // Same sensitivity curve and invert handling as mouselook (LLToolGun)
    // Delta is read here (once per frame) rather than in the captor's
    // handleHover, which is skipped when the UI is hidden.
    const S32 dx = -gViewerWindow->getCurrentMouseDX();
    const S32 dy = -gViewerWindow->getCurrentMouseDY();
    if (dx == 0 && dy == 0)
    {
        return;
    }
    if (mRightHeld)
    {
        mRightTravelX += llabs(dx);
        mRightTravelY += llabs(dy);
    }

    constexpr F32 NOMINAL_MOUSE_SENSITIVITY = 0.0025f;
    static LLCachedControl<F32> mouse_sensitivity_setting(gSavedSettings, "MouseSensitivity");
    static LLCachedControl<bool> invert_mouse(gSavedSettings, "InvertMouse");
    const F32 mouse_sensitivity = clamp_rescale((F32)mouse_sensitivity_setting, 0.f, 15.f, 0.5f, 2.75f) * NOMINAL_MOUSE_SENSITIVITY;
    const F32 fov = LLViewerCamera::getInstance()->getView() / DEFAULT_FIELD_OF_VIEW;

    const F32 yaw_delta = mouse_sensitivity * fov * (F32)dx;
    const F32 pitch_delta = mouse_sensitivity * fov * (F32)(invert_mouse ? -dy : dy);
    gAgentCamera.tpsCameraOrbit(yaw_delta, pitch_delta);

    // consume the delta and re-assert relative mode (SDL can drop it on focus changes)
    gViewerWindow->moveCursorToCenter();
}

void SLTPSCamera::updateMovement()
{
    // Face-camera mode (SlipstreamTPSFaceCamera): the body always turns to the camera
    // heading -- also while idle -- and the keys move relative to it (strafe /
    // back-pedal), shooter style.
    static LLCachedControl<bool> face_camera_setting(gSavedSettings, "SlipstreamTPSFaceCamera", false);
    // Holding the fire button also faces the camera heading (#881), so held/auto
    // fire keeps going where the crosshair is.
    const bool face_camera = face_camera_setting || mLeftFacesAim;
    if (isSeated())
    {
        // Seated / vehicle (#95): the mouse only steers the camera. Movement keys are the
        // stock ones (see isEngagedOnFoot), going to the vehicle script when it took the
        // controls; the avatar is never turned or driven from here.
        return;
    }
    const bool idle = (mInputAt == 0 && mInputLeft == 0);
    if (idle && !face_camera)
    {
        // Idle: never turn the avatar just because the camera looked around.
        return;
    }

    const bool flying = gAgent.getFlying();

    // Camera basis. Walking uses the flat heading; flying uses the full camera
    // direction (pitch included), like mouselook flight.
    const LLQuaternion yaw_rot = gAgentCamera.getTPSYawRotation();
    const LLVector3 cam_at = flying ? LLVector3::x_axis * gAgentCamera.getTPSRotation()
                                    : LLVector3::x_axis * yaw_rot;
    const LLVector3 cam_left = LLVector3::y_axis * yaw_rot;

    LLVector3 desired = cam_at * (F32)mInputAt + cam_left * (F32)mInputLeft;
    if (!idle && desired.normalize() < 0.001f)
    {
        return;
    }
    // Where the body turns: the camera heading (face-camera mode) or the movement
    // direction.
    const LLVector3 face = face_camera ? cam_at : desired;

    // Turn the body toward the movement direction with a fast critically
    // damped slerp (the visible pelvis additionally lags in LLVOAvatar).
    // Respect scripts that took the rotation controls.
    LLVector3 body_at = gAgent.getAtAxis();
    if (!gAgent.rotateGrabbed())
    {
        const F32 cur_yaw = yaw_of(body_at);
        const F32 cur_pitch = pitch_of(body_at);
        const F32 target_yaw = yaw_of(face);
        const F32 target_pitch = flying ? llclamp(pitch_of(face), -TPS_BODY_PITCH_LIMIT, TPS_BODY_PITCH_LIMIT) : 0.f;
        // Idle and already facing the camera: nothing to send (resetAxes each frame
        // would stream agent updates for no change).
        if (idle && fabsf(wrap_pi(target_yaw - cur_yaw)) < 0.002f && fabsf(target_pitch - cur_pitch) < 0.002f)
        {
            return;
        }

        const F32 t = LLSmoothInterpolation::getInterpolant(TPS_TURN_HALF_LIFE);
        const F32 new_yaw = cur_yaw + wrap_pi(target_yaw - cur_yaw) * t;
        const F32 new_pitch = llclamp(lerp(cur_pitch, target_pitch, t), -TPS_BODY_PITCH_LIMIT, TPS_BODY_PITCH_LIMIT);
        body_at = dir_from(new_yaw, new_pitch);
    }

    if (idle)
    {
        if (!gAgent.rotateGrabbed()) gAgent.resetAxes(body_at);
        return;
    }

    // Split the desired direction into body-relative control bits so the avatar
    // already travels the right way while it is still turning; once facing the
    // direction this collapses to plain forward.
    LLVector3 body_left = gAgent.getReferenceUpVector() % body_at;
    body_left.normalize();
    const F32 fwd = desired * body_at;
    const F32 side = desired * body_left;
    const S32 at_dir = fwd > TPS_MOVE_COMPONENT_THRESHOLD ? 1 : (fwd < -TPS_MOVE_COMPONENT_THRESHOLD ? -1 : 0);
    const S32 left_dir = side > TPS_MOVE_COMPONENT_THRESHOLD ? 1 : (side < -TPS_MOVE_COMPONENT_THRESHOLD ? -1 : 0);

    if (at_dir != 0)
    {
        if (mInputNudge)
        {
            gAgent.moveAtNudge(at_dir);
        }
        else
        {
            gAgent.moveAt(at_dir);
        }
    }
    if (left_dir != 0)
    {
        if (mInputNudge)
        {
            gAgent.moveLeftNudge(left_dir);
        }
        else
        {
            gAgent.moveLeft(left_dir);
        }
    }

    // Apply the heading last: moveAt()/moveLeft() -> resetView() lerps agent
    // pitch toward level, which would fight TPS flight pitch.
    if (!gAgent.rotateGrabbed())
    {
        gAgent.resetAxes(body_at);
    }
}

// static
void SLTPSCamera::drawOverlay()
{
    if (!isEngagedNow() || getInstance()->mInputSuspended || !gViewerWindow)
    {
        return;
    }
    SLTPSCamera* self = getInstance();
    const LLRect world_rect = gViewerWindow->getWorldViewRectScaled();

    static LLCachedControl<bool> show_crosshairs(gSavedSettings, "ShowCrosshairs");
    if (show_crosshairs)
    {
        static LLCachedControl<bool> custom_crosshair(gSavedSettings, "FSCustomCrosshair", false);
        if (SLCombatFX::enabled() && isShoulderActive() && custom_crosshair)
        {
            FSCombatAim::setCrosshairTint(LLColor4::white);
            gGL.pushMatrix();
            gGL.translatef((F32)world_rect.mLeft, (F32)world_rect.mBottom, 0.f);
            FSCombatAim::drawCrosshair(world_rect.getWidth(), world_rect.getHeight());
            gGL.popMatrix();
        }
        else
        {
            // Leaked on purpose: a static image ref would be released in static-destructor
            // order, after the texture statics it depends on (#275).
            static LLUIImagePtr* crosshair_p = new LLUIImagePtr(LLUI::getUIImage("crosshairs.tga"));
            const LLUIImagePtr& crosshair = *crosshair_p;
            if (crosshair)
            {
                LLColor4 color = LLColor4::white;
                color.mV[VALPHA] = 0.75f;
                // center of the world view == where the camera (and llGetCameraRot) aims
                crosshair->draw(world_rect.getCenterX() - crosshair->getWidth() / 2,
                                world_rect.getCenterY() - crosshair->getHeight() / 2, color);
            }
        }
    }

    if (self->mHintTimer.getStarted())
    {
        const F32 elapsed = self->mHintTimer.getElapsedTimeF32();
        if (elapsed > TPS_HINT_SECONDS)
        {
            self->mHintTimer.stop();
            return;
        }
        const F32 alpha = 0.75f * llclamp((TPS_HINT_SECONDS - elapsed) / TPS_HINT_FADE_SECONDS, 0.f, 1.f);
        static const std::string hint = LLTrans::getString("SlipstreamTPSCameraHint");
        constexpr S32 HINT_BOTTOM_PAD = 50;
        LLFontGL::getFontSansSerifBig()->renderUTF8(
            hint, 0,
            world_rect.getCenterX(),
            world_rect.mBottom + HINT_BOTTOM_PAD,
            LLColor4(1.f, 1.f, 1.f, alpha),
            LLFontGL::HCENTER, LLFontGL::TOP,
            LLFontGL::BOLD, LLFontGL::DROP_SHADOW);
    }
}

//=============================================================================
// SLToolTPS
//=============================================================================

SLToolTPS::SLToolTPS()
:   LLTool(std::string("TPSCamera"))
{
}

namespace
{
bool crosshair_on_self()
{
    if (!isAgentAvatarValid())
    {
        return false;
    }
    const LLPickInfo pick = gViewerWindow->pickImmediate(gViewerWindow->getWorldViewWidthScaled() / 2,
                                                         gViewerWindow->getWorldViewHeightScaled() / 2,
                                                         /*pick_transparent*/ false, /*pick_rigged*/ true,
                                                         /*pick_particle*/ false, /*pick_unselectable*/ true,
                                                         /*pick_reflection_probe*/ false);
    LLViewerObject* obj = pick.getObject();
    while (obj && !obj->isAvatar())
    {
        obj = (LLViewerObject*)obj->getParent();
    }
    return obj && obj == gAgentAvatarp.get();
}
}

bool SLToolTPS::handleMouseDown(S32 x, S32 y, MASK mask)
{
    if (mask & MASK_ALT)
    {
        // Alt camera tools: release, then let the (ALT-overridden) current
        // tool - LLToolCamera - take this click as usual.
        SLTPSCamera::disengageIfEngaged(SLTPSCamera::DISENGAGE_USER);
        return LLToolMgr::getInstance()->getCurrentTool()->handleMouseDown(x, y, mask);
    }
    // Returning true keeps LLViewerInput from also firing a script binding
    // for this click. The avatar turns to the
    // aim for a shot, but not for a click on yourself (crosshair over your own avatar or
    // attachments): that must leave the heading alone, like a self click anywhere else.
    SLTPSCamera::getInstance()->setLeftButton(true, !crosshair_on_self());
    return true;
}

bool SLToolTPS::handleMouseUp(S32 x, S32 y, MASK mask)
{
    // (also receives the release of the self-click that engaged TPS; there is
    // no outstanding press then, so nothing is sent)
    SLTPSCamera::getInstance()->setLeftButton(false);
    return true;
}

bool SLToolTPS::handleDoubleClick(S32 x, S32 y, MASK mask)
{
    return handleMouseDown(x, y, mask);
}

bool SLToolTPS::handleRightMouseDown(S32 x, S32 y, MASK mask)
{
    static LLCachedControl<bool> right_drag_steers(gSavedSettings, "SlipstreamTPSRightDragSteers", false);
    if (SLTPSCamera::isShoulderActive() && SLCombatFX::enabled() && !(mask & MASK_ALT))
    {
        FSCombatADS::onRightMouseDown(LLViewerCamera::getInstance()->getDefaultFOV());
        return true;
    }
    if (right_drag_steers)
    {
        // Keep the camera: a drag steers, a click only releases TPS.
        SLTPSCamera::getInstance()->setRightButton(true);
        return true;
    }
    // Give the mouse back. Consuming the click keeps the context menu from
    // opening on this same click, including its up after capture changes.
    gViewerWindow->consumeTPSRightMouseUp();
    SLTPSCamera::disengageIfEngaged(SLTPSCamera::DISENGAGE_USER);
    return true;
}

bool SLToolTPS::handleRightMouseUp(S32 x, S32 y, MASK mask)
{
    if (SLTPSCamera::isShoulderActive() && SLCombatFX::enabled() && !(mask & MASK_ALT))
    {
        FSCombatADS::onRightMouseUp();
        return true;
    }
    SLTPSCamera::getInstance()->setRightButton(false);
    return true;
}

bool SLToolTPS::handleMiddleMouseDown(S32 x, S32 y, MASK mask)
{
    // not handled: lets the MMB key binding (voice) see it
    return false;
}

bool SLToolTPS::handleMiddleMouseUp(S32 x, S32 y, MASK mask)
{
    return false;
}

bool SLToolTPS::handleHover(S32 x, S32 y, MASK mask)
{
    // Mouse deltas are consumed in SLTPSCamera::update(). The cursor is hidden;
    // this only satisfies the "cursor set every hover" check.
    gViewerWindow->setCursor(UI_CURSOR_ARROW);
    return true;
}

bool SLToolTPS::handleScrollWheel(S32 x, S32 y, S32 clicks)
{
    if (FSCombatADS::onScrollWheel(clicks)) return true;
    // Zoom the orbit distance (clamped at the minimum; no forced mouselook)
    gAgentCamera.handleScrollWheel(clicks);
    return true;
}

void SLToolTPS::onMouseCaptureLost()
{
    // focus loss (alt-tab), another captor, ... : give the pointer back, unless
    // it was mouselook's tool taking over (it keeps the cursor hidden itself)
    if (!SLTPSCamera::instanceExists()) return;
    if (SLTPSCamera::getInstance()->isInputSuspended() && !gFocusMgr.getMouseCapture()) return;
    if (preferences_for_view(dynamic_cast<LLView*>(gFocusMgr.getMouseCapture()))
        && SLTPSCamera::getInstance()->suspendForPreferences()) return;
    SLTPSCamera::disengageIfEngaged(gAgentCamera.getCameraMode() == CAMERA_MODE_MOUSELOOK
                                    ? SLTPSCamera::DISENGAGE_MOUSELOOK
                                    : SLTPSCamera::DISENGAGE_USER);
}
