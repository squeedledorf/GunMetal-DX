/**
 * @file llvrcamera.h
 * @brief VR milestone M1 (#1074, epic #1072): the head-tracked viewer camera.
 *
 * While an OpenXR session runs (LLXrSession), the viewer camera is the headset: VR camera mode,
 * modelled on mouselook. The rig is the avatar -- its position and yaw (the seat's when
 * sitting) -- with the eye at the avatar's head (head hidden, as in mouselook); the HMD pose,
 * relative to a recentre reference, is applied in agent space on top. The result goes into
 * LLViewerCamera (origin, axes, the VR frustum, the VR near plane), so audio, LOD, picking and
 * name tags follow the head. Mono: the camera is the cyclopean (between the eyes) pose; both
 * eyes get the same image (llxrsession.h).
 *
 * Desktop (no VR chosen at startup): apply() returns on one static read, active() is false.
 */
#pragma once

#include "stdtypes.h"
#include "v3math.h"
#include "llquaternion.h"

class LLVRCamera
{
public:
    // The head-tracked camera drove this frame's viewer camera. False on the desktop.
    static bool active() { return sActive; }
    // A VR session is running (menu items). False on the desktop.
    static bool sessionRunning();

    // idle(), after the desktop camera was computed (updateCamera / flycam / camera pilot):
    // compose this frame's head pose onto the avatar rig and write it into LLViewerCamera.
    // Restores the desktop camera parameters when the session stops. No-op without VR.
    static void apply();

    // Re-zero yaw and position (including the height) on the current head pose: what you
    // face becomes the avatar's facing, where your head is becomes the avatar's eyes.
    static void recenter();

    // ---- #1076 (M3): app (OpenXR) space -> agent space, with THIS frame's rig mapping (the
    // one the camera used). Valid after apply() put the head pose into the camera; false /
    // unrotated otherwise. App space: OpenXR axes, metres; agent space: SL region metres.
    static bool appToAgent(const float p[3], LLVector3& out);   // a point
    static LLVector3 appDirToAgent(const float d[3]);           // a direction
    // The axis (x, y, z) of an app-space orientation (quaternion x y z w), in agent space.
    static LLVector3 appAxisToAgent(const float q[4], F32 x, F32 y, F32 z);
    // A controller's pose in agent space (hand: LLXR_HAND_LEFT / RIGHT; aim: the pointing ray
    // pose, else the grip / hand pose). rot maps SL local axes (X at = the pose's forward,
    // Y left, Z up) into agent space. False: no session, the hand is not tracked, or the
    // head-tracked camera did not run this frame. Main thread. This is the hook for Pose
    // Studio (avatar hands from the controllers, later from body tracking).
    static bool handPose(U32 hand, bool aim, LLVector3& pos, LLQuaternion& rot);
    // #1075 LAYERED STEREO: eye `eye`'s view (0 = left, 1 = right) for THIS frame's camera, in
    // agent space -- its located position, axes (at, left, up; SL frame) and FOV tangents
    // (tanLeft < 0 < tanRight, tanDown < 0 < tanUp) -- with the rig mapping apply() used, so the
    // two eyes sit around the camera origin (the cyclopean head). False: no session, the
    // head-tracked camera did not run this frame, or the runtime located no eyes.
    static bool eyeView(U32 eye, LLVector3& pos, LLVector3& at, LLVector3& left, LLVector3& up, F32 tan[4]);

    // ---- #1076 locomotion (LLVRInput, before idle()) ----
    // Turn the view by `radians` about the up axis (+ = left): snap / smooth turn. Turns the
    // rig mapping, not the avatar (also works while sitting).
    static void turnView(F32 radians);
    // The avatar (the rig, when standing) was just turned by `radians`: keep the view where it
    // is (the body follows the head when you walk).
    static void rigTurnedKeepView(F32 radians);
    // The head's facing in agent space (yaw about Z, radians), from this frame's camera.
    static F32 headYaw();
    // The rig follows the avatar's yaw (standing); false while sitting (the seat's rotation).
    static bool rigFollowsAgent();

    // ---- #1121 the avatar's head follows the headset ----
    // This frame's head orientation expressed in `frame`'s axes (a rotation in agent space:
    // the avatar's root joint for the head drive, LLQuaternion::DEFAULT for agent axes): yaw
    // about up (+ = left), pitch (+ = up), roll about the look axis (+ = the left ear up),
    // radians. False unless the head-tracked camera drove this frame from the avatar
    // (firstPerson(); not in fly cam, not on the desktop).
    static bool headAngles(const LLQuaternion& frame, F32& yaw, F32& pitch, F32& roll);
    // LLVRInput locomotion, every frame: once the head has turned past the neck's comfortable
    // range from the standing body (70 degrees), the body (the agent's rotation, sent to the
    // sim) turns smoothly until it faces where you look; the view stays put
    // (rigTurnedKeepView). Never while sitting, walking with the stick (that already turns the
    // body to the head), in fly cam or under autopilot.
    static void bodyFollowHead(F32 dt, bool walking);

    // ---- #1076 fly cam: the view leaves the body and flies like desktop camming ----
    // The head-tracked camera drove this frame from the avatar's head (as mouselook: own head /
    // avatar hidden). False in fly cam and on the desktop.
    static bool firstPerson();
    static bool flyCam();
    // On: the rig detaches at the current eye position and facing (no jump). Off: back on the
    // avatar at once, facing where you faced -- LLVRInput blinks the view to black around it
    // (never a fly-back: a rush through space is a motion-sickness trigger).
    static void setFlyCam(bool on);
    // Move the detached rig (agent space metres), kept within the desktop camera's distance
    // limit from the avatar (getCameraMaxZoomDistance; the interest list stays on the avatar).
    static void flyMove(const LLVector3& delta);

private:
    static bool sActive;
};
