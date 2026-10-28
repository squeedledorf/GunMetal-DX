/**
 * @file llvrcamera.cpp
 * @brief VR milestone M1 (#1074): the head-tracked viewer camera. See llvrcamera.h.
 *
 * Frames: OpenXR app space is right-handed, +Y up, -Z forward (metres). SL agent space is
 * X = at, Y = left, Z = up. A vector maps as SL = (-xr.z, -xr.x, xr.y) (a proper rotation).
 * Rotations are done on vectors with the standard quaternion formula, so no quaternion
 * multiplication convention is involved.
 */
#include "llviewerprecompiledheaders.h"

#include "llvrcamera.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llcriticaldamp.h"
#include "llformat.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerjoystick.h"
#include "llviewerwindow.h"
#include "llvoavatarself.h"
#include "llvrhands.h"
#include "m3math.h"
#include "vkgfx/llxrsession.h"

#include <chrono>
#include <cmath>

bool LLVRCamera::sActive = false;

namespace
{
struct VRCameraState
{
    // Recentre reference: the head's yaw and horizontal position (SL axes, app space) that
    // map onto the avatar's facing and eyes, and the floor height that maps onto the avatar's
    // eye height.
    bool recenterPending = true;   // the first located pose takes yaw + horizontal position
    bool recenterHeight = false;   // a user recentre also takes the height
    bool haveRef = false;
    bool refProvisional = false;   // #1076: taken from an untracked pose: redone on the first good one
    F32 refYaw = 0.f;
    F32 refX = 0.f, refY = 0.f;
    F32 refHeight = 1.6f;
    uint32_t spaceGeneration = 0;

    // The avatar's eye anchor relative to the avatar, in the rig's yaw frame, smoothed (the
    // head joint carries the animation's bob: walk cycles, breathing).
    bool anchorValid = false;
    LLVector3 anchorLocal;

    // Desktop camera parameters, restored when the head-tracked camera stops.
    bool saved = false;
    F32 savedNear = 0.f;

    // #1076: this frame's app space -> agent space mapping (apply()), shared by the camera,
    // the controllers (appToAgent / handPose) and the hands: a point maps as
    // anchor + rotZ(xy - ref, turn) + z (height - refHeight).
    bool mapValid = false;
    LLVector3 mapAnchor;
    F32 mapTurn = 0.f;
    F32 mapHeightAdd = 0.f;   // LOCAL space: + the configured eye height (a height above the floor)

    // #1076 fly cam: the rig detached from the avatar (anchor flyPos, yaw flyYaw).
    bool fly = false;         // requested (setFlyCam)
    bool flyApplied = false;  // what apply() last composed
    LLVector3 flyPos;
    F32 flyYaw = 0.f;

    // #1121: this frame's head axes in agent space (with the rig turn), for the avatar's head
    // follow (headAngles) and the body turn (bodyFollowHead).
    LLVector3 headAt = LLVector3::x_axis;
    LLVector3 headUp = LLVector3::z_axis;
    bool bodyTurning = false;   // bodyFollowHead: turning the body toward the head
};
VRCameraState sVR;

LLVector3 xrToSL(F32 x, F32 y, F32 z)
{
    return LLVector3(-z, -x, y);
}

// Standard active rotation of v by the unit quaternion q = (x, y, z, w): v + 2w(u x v) + 2u x (u x v).
void rotateByQuat(const float q[4], const F32 v[3], F32 out[3])
{
    const F32 ux = q[0], uy = q[1], uz = q[2], w = q[3];
    const F32 tx = 2.f * (uy * v[2] - uz * v[1]);
    const F32 ty = 2.f * (uz * v[0] - ux * v[2]);
    const F32 tz = 2.f * (ux * v[1] - uy * v[0]);
    out[0] = v[0] + w * tx + (uy * tz - uz * ty);
    out[1] = v[1] + w * ty + (uz * tx - ux * tz);
    out[2] = v[2] + w * tz + (ux * ty - uy * tx);
}

// An OpenXR head-local direction, rotated by the head, in SL axes.
LLVector3 headDirSL(const float q[4], F32 x, F32 y, F32 z)
{
    const F32 v[3] = {x, y, z};
    F32 r[3];
    rotateByQuat(q, v, r);
    return xrToSL(r[0], r[1], r[2]);
}

LLVector3 rotZ(const LLVector3& v, F32 a)
{
    const F32 c = std::cos(a), s = std::sin(a);
    return LLVector3(v.mV[VX] * c - v.mV[VY] * s, v.mV[VX] * s + v.mV[VY] * c, v.mV[VZ]);
}

F32 yawOf(const LLVector3& fwd)
{
    return std::atan2(fwd.mV[VY], fwd.mV[VX]);
}

F32 yawDiff(F32 a, F32 b)
{
    F32 d = a - b;
    while (d > F_PI) d -= F_TWO_PI;
    while (d < -F_PI) d += F_TWO_PI;
    return d;
}

// #1076 diagnostic: how much the camera's rig moved on its own over the last 10 s. The UI wall
// is a quad layer fixed in the app space; the world is drawn from the avatar rig (the eye
// anchor at the avatar's head joint, the avatar's position and yaw). Anything that moves the
// rig while you only move your head makes the world swim against the wall, which then looks
// like it moves with the head. Logged every 10 s while the headset camera runs: the raw head
// joint anchor's and the smoothed anchor's largest excursion from the window's first frame,
// the avatar's (position, yaw) excursion -- non-zero while walking / turning, otherwise
// something else moves the rig -- and the span of the head's yaw for comparison.
struct RigStability
{
    bool started = false;
    std::chrono::steady_clock::time_point t0{};
    LLVector3 raw0, smooth0, pos0;
    F32 rigYaw0 = 0.f, headYaw0 = 0.f;
    F32 rawMax = 0.f, smoothMax = 0.f, posMax = 0.f, rigYawMax = 0.f, headYawMin = 0.f, headYawMax = 0.f;
    // The avatar's turning, summed frame to frame, split by whether it moved that frame (walk
    // turns are ours: LLVRCamera::rigTurnedKeepView) or stood still (something else turns it).
    LLVector3 prevPos;
    F32 prevRigYaw = 0.f;
    F32 turnStill = 0.f, turnMoving = 0.f;
    U32 frames = 0;
};
RigStability sRig;

void rigStabilityTick(const LLVector3& raw, const LLVector3& smooth, const LLVector3& pos, F32 rigYaw, F32 headYaw)
{
    const auto now = std::chrono::steady_clock::now();
    if (!sRig.started)
    {
        sRig = RigStability{};
        sRig.started = true;
        sRig.t0 = now;
        sRig.raw0 = raw;
        sRig.smooth0 = smooth;
        sRig.pos0 = pos;
        sRig.rigYaw0 = rigYaw;
        sRig.headYaw0 = headYaw;
        sRig.prevPos = pos;
        sRig.prevRigYaw = rigYaw;
    }
    const F32 dYaw = std::fabs(yawDiff(rigYaw, sRig.prevRigYaw));
    if ((pos - sRig.prevPos).magVec() > 0.002f) sRig.turnMoving += dYaw;
    else sRig.turnStill += dYaw;
    sRig.prevPos = pos;
    sRig.prevRigYaw = rigYaw;
    sRig.rawMax = llmax(sRig.rawMax, (raw - sRig.raw0).magVec());
    sRig.smoothMax = llmax(sRig.smoothMax, (smooth - sRig.smooth0).magVec());
    sRig.posMax = llmax(sRig.posMax, (pos - sRig.pos0).magVec());
    sRig.rigYawMax = llmax(sRig.rigYawMax, std::fabs(yawDiff(rigYaw, sRig.rigYaw0)));
    const F32 hy = yawDiff(headYaw, sRig.headYaw0);
    sRig.headYawMin = llmin(sRig.headYawMin, hy);
    sRig.headYawMax = llmax(sRig.headYawMax, hy);
    ++sRig.frames;
    if (std::chrono::duration<F32>(now - sRig.t0).count() < 10.f) return;
    LL_INFOS("VR") << llformat("rig over 10 s (%u frames): eye anchor (head joint) moved up to %.1f cm raw, %.1f cm "
                               "smoothed; avatar moved %.1f cm, turned %.1f deg (%.1f deg while standing still, %.1f deg "
                               "while moving); your head turned over %.0f deg; mouselook %d",
                               sRig.frames, sRig.rawMax * 100.f,
                               sRig.smoothMax * 100.f, sRig.posMax * 100.f, sRig.rigYawMax * RAD_TO_DEG,
                               sRig.turnStill * RAD_TO_DEG, sRig.turnMoving * RAD_TO_DEG,
                               (sRig.headYawMax - sRig.headYawMin) * RAD_TO_DEG, (int)gAgentCamera.cameraMouselook())
                   << LL_ENDL;
    sRig.started = false;
}

// Back to the desktop camera parameters (the session stopped, or no pose / avatar).
void leaveVRCamera()
{
    sVR.mapValid = false;
    sVR.bodyTurning = false;
    sVR.fly = sVR.flyApplied = false;   // #1076: a new session starts on the avatar
    sRig.started = false;
    LLVRHands::release();   // #1076 (no-op when none are registered)
    if (!sVR.saved) return;
    sVR.saved = false;
    sVR.anchorValid = false;
    LLViewerCamera* cam = LLViewerCamera::getInstance();
    cam->setNear(sVR.savedNear);
    if (gViewerWindow)
    {
        cam->setAspect(gViewerWindow->getWorldViewAspectRatio());
        cam->setViewHeightInPixels(gViewerWindow->getWorldViewHeightRaw());
    }
    cam->setView(cam->getDefaultFOV());
    LL_INFOS("VR") << "head-tracked camera off: the desktop camera is back" << LL_ENDL;
}
} // namespace

bool LLVRCamera::sessionRunning()
{
    return LLXrSession::active() && LLXrSession::instance().sessionRunning();
}

void LLVRCamera::recenter()
{
    if (!LLXrSession::active()) return;
    sVR.recenterPending = true;
    sVR.recenterHeight = true;
    LL_INFOS("VR") << "recentre requested" << LL_ENDL;
}

void LLVRCamera::apply()
{
    if (!LLXrSession::active())   // desktop: one static read
    {
        return;
    }
    LLXrSession& xr = LLXrSession::instance();

    LLXrHeadPose hp;
    if (!xr.headPose(hp) || !hp.renderWidth || !hp.renderHeight || !isAgentAvatarValid())
    {
        sActive = false;
        leaveVRCamera();
        return;
    }

    static LLCachedControl<F32> s_eye_height(gSavedSettings, "SlipstreamVREyeHeight", 1.6f);
    static LLCachedControl<F32> s_near_clip(gSavedSettings, "SlipstreamVRNearClip", 0.05f);
    static LLCachedControl<F32> s_smoothing(gSavedSettings, "SlipstreamVRHeadSmoothing", 0.2f);
    const F32 eye_height = llclamp((F32)s_eye_height, 0.5f, 2.5f);

    // ---- The head in SL axes (app space, before the recentre).
    const LLVector3 head_pos = xrToSL(hp.position[0], hp.position[1], hp.position[2]);
    // LOCAL has its origin at the head height at session start: add the configured eye height
    // so all spaces give a height above the floor.
    const F32 head_height = head_pos.mV[VZ] + (hp.floorSpace ? 0.f : eye_height);
    const LLVector3 head_at = headDirSL(hp.orientation, 0.f, 0.f, -1.f);   // OpenXR forward
    const LLVector3 head_up = headDirSL(hp.orientation, 0.f, 1.f, 0.f);    // OpenXR up

    // ---- Recentre: the first pose, the runtime re-centring its space, or the user's request.
    if (hp.spaceGeneration != sVR.spaceGeneration)
    {
        sVR.spaceGeneration = hp.spaceGeneration;
        sVR.recenterPending = true;
    }
    // #1076: a reference is only taken from a pose that can anchor it (LLXrHeadPose::placeable:
    // tracked, shown, a plausible height). The first headset run recentred on a 5 cm "tracked"
    // head while the session was not even visible. Without any reference yet the camera needs
    // one to render at all: a provisional one is taken (the configured eye height, never the
    // pose's) and replaced by the first placeable pose. A user recentre on a bad pose waits.
    const bool pose_good = hp.placeable();
    const bool redo_provisional = sVR.refProvisional && pose_good;
    if ((sVR.recenterPending && (pose_good || !sVR.haveRef)) || redo_provisional)
    {
        sVR.recenterPending = false;
        sVR.refYaw = yawOf(head_at);
        sVR.refX = head_pos.mV[VX];
        sVR.refY = head_pos.mV[VY];
        const bool take_height = sVR.recenterHeight && pose_good;
        if (take_height || !sVR.haveRef)
            sVR.refHeight = take_height ? head_height : eye_height;
        if (pose_good) sVR.recenterHeight = false;   // (a user's height recentre waits for a good pose)
        else sVR.recenterPending = sVR.recenterHeight;
        sVR.haveRef = true;
        sVR.refProvisional = !pose_good;
        LL_INFOS("VR") << llformat("recentred%s: head yaw %.1f deg, position (%.2f, %.2f), height %.2f m (reference %.2f m)",
                                   pose_good ? (redo_provisional ? " on the first tracked pose" : "")
                                             : " provisionally (head not tracked / shown yet, or an implausible height)",
                                   sVR.refYaw * RAD_TO_DEG, sVR.refX, sVR.refY, head_height, sVR.refHeight)
                       << LL_ENDL;
    }

    // ---- The rig: the avatar's position and yaw (the seat's rotation when sitting).
    const LLQuaternion rig_rot = gAgentAvatarp->isSitting() ? gAgentAvatarp->getRenderRotation()
                                                            : gAgent.getFrameAgent().getQuaternion();
    const F32 avatar_rig_yaw = yawOf(LLVector3::x_axis * rig_rot);
    // #1076 fly cam: entering keeps the current eye and facing (the rig's yaw is frozen at the
    // avatar's); leaving keeps your facing (the reference absorbs the yaw difference).
    if (sVR.fly != sVR.flyApplied)
    {
        if (sVR.fly)
        {
            if (!sVR.mapValid)
            {
                sVR.fly = false;   // (no eye yet to start from)
            }
            else
            {
                sVR.flyPos = sVR.mapAnchor;
                sVR.flyYaw = avatar_rig_yaw;
                LL_INFOS("VR") << "fly cam on" << LL_ENDL;
            }
        }
        else
        {
            sVR.refYaw += avatar_rig_yaw - sVR.flyYaw;
            sVR.anchorValid = false;   // straight onto the avatar's eyes (the view is dark: blink)
            LL_INFOS("VR") << "fly cam off: back on the avatar" << LL_ENDL;
        }
        sVR.flyApplied = sVR.fly;
    }
    const F32 rig_yaw = sVR.fly ? sVR.flyYaw : avatar_rig_yaw;
    const F32 turn = rig_yaw - sVR.refYaw;   // head app space -> agent space (about Z)

    // The avatar's eyes (mouselook: in front of and above the head joint), relative to the
    // avatar in the rig's frame, smoothed against the animation's head bob.
    const LLVector3 avatar_pos = gAgentAvatarp->getRenderPosition();
    LLVector3 eye_anchor = gAgentAvatarp->mHeadp ? gAgentAvatarp->mHeadp->getWorldPosition()
                                                 : avatar_pos + LLVector3(0.f, 0.f, 0.8f);
    // #1121: the avatar's neck and head follow the headset (SLPoseLookAt), and the head joint
    // swings with the neck: anchored on it, the eye would move with that turn on top of the
    // headset's own tracked motion (the world swims). Anchor on the neck pivot plus the head's
    // offset as if the neck itself were not turned (the skeleton's rest rotations are identity:
    // the neck's parent's rotation keeps a lean of the chest or a lying-down sit).
    LLJoint* neck_parent = gAgentAvatarp->mNeckp ? gAgentAvatarp->mNeckp->getParent() : nullptr;
    if (neck_parent && gAgentAvatarp->mHeadp)
    {
        const LLVector3 neck = gAgentAvatarp->mNeckp->getWorldPosition();
        const LLVector3 head_off = (gAgentAvatarp->mHeadp->getWorldPosition() - neck) * ~gAgentAvatarp->mNeckp->getWorldRotation();
        const LLVector3 anchor_head = neck + head_off * neck_parent->getWorldRotation();
        if (anchor_head.isFinite()) eye_anchor = anchor_head;
    }
    eye_anchor += rotZ(LLVector3(0.1f, 0.f, 0.06f), rig_yaw);
    const LLVector3 anchor_local = rotZ(eye_anchor - avatar_pos, -rig_yaw);
    if (!sVR.anchorValid || (anchor_local - sVR.anchorLocal).magVec() > 1.5f || s_smoothing <= 0.f)
    {
        sVR.anchorLocal = anchor_local;   // first frame, a sit / stand jump, or smoothing off
        sVR.anchorValid = true;
    }
    else
    {
        sVR.anchorLocal = lerp(sVR.anchorLocal, anchor_local,
                               LLSmoothInterpolation::getInterpolant(llclamp((F32)s_smoothing, 0.01f, 2.f)));
    }
    const LLVector3 anchor = sVR.fly ? sVR.flyPos : avatar_pos + rotZ(sVR.anchorLocal, rig_yaw);
    rigStabilityTick(anchor_local, sVR.anchorLocal, avatar_pos, avatar_rig_yaw, yawOf(head_at));

    // ---- The camera: the anchor plus the head's offset from the reference, turned onto the rig.
    const LLVector3 offset = rotZ(LLVector3(head_pos.mV[VX] - sVR.refX, head_pos.mV[VY] - sVR.refY, 0.f), turn);
    const LLVector3 eye = anchor + LLVector3(offset.mV[VX], offset.mV[VY], head_height - sVR.refHeight);
    const LLVector3 at = rotZ(head_at, turn);
    const LLVector3 up = rotZ(head_up, turn);
    sVR.headAt = at;   // #1121
    sVR.headUp = up;

    LLViewerCamera* cam = LLViewerCamera::getInstance();
    if (!sVR.saved)
    {
        sVR.saved = true;
        sVR.savedNear = cam->getNear();
        LL_INFOS("VR") << "head-tracked camera on: " << hp.renderWidth << "x" << hp.renderHeight << ", vertical FOV "
                       << 2.f * std::atan(hp.tanHalfY) * RAD_TO_DEG << " deg, aspect " << hp.aspect << ", near "
                       << llclamp((F32)s_near_clip, 0.01f, 0.5f) << " m" << LL_ENDL;
    }
    // The frustum before the location (updateCameraLocation derives the cull FOV terms from
    // it); aspect before the FOV (the FOV clamp depends on it). The VR frustum is never sent
    // to the simulator as the agent's FOV (setViewNoBroadcast), and the speed-FoV boost is a
    // desktop effect.
    cam->setSpeedFovBoost(0.f);
    cam->setAspect(hp.aspect);
    cam->setViewNoBroadcast(2.f * std::atan(hp.tanHalfY));
    cam->setNear(llclamp((F32)s_near_clip, 0.01f, 0.5f));
    cam->setViewHeightInPixels((S32)hp.renderHeight);
    LLViewerJoystick::getInstance()->setCameraNeedsUpdate(true);
    // The true up (with the head's roll): left = up x at and up = at x left rebuild exactly
    // these axes.
    cam->updateCameraLocation(eye, up, eye + at);

    sActive = true;
    xr.setWorldCameraApplied(true);

    // #1076: the mapping for the controllers, then the hands at this frame's grip poses.
    sVR.mapValid = true;
    sVR.mapAnchor = anchor;
    sVR.mapTurn = turn;
    sVR.mapHeightAdd = hp.floorSpace ? 0.f : eye_height;
    LLVRHands::update();
}

bool LLVRCamera::firstPerson()
{
    return sActive && !sVR.flyApplied;
}

bool LLVRCamera::flyCam()
{
    return LLXrSession::active() && sVR.fly;
}

void LLVRCamera::setFlyCam(bool on)
{
    if (!LLXrSession::active()) return;
    sVR.fly = on;   // composed by the next apply()
}

void LLVRCamera::flyMove(const LLVector3& delta)
{
    if (!LLXrSession::active() || !sVR.fly || !sVR.flyApplied || !isAgentAvatarValid()) return;
    LLVector3 p = sVR.flyPos + delta;
    const LLVector3 avatar = gAgentAvatarp->getRenderPosition();
    const F32 max_dist = llmax(1.f, gAgentCamera.getCameraMaxZoomDistance());
    LLVector3 off = p - avatar;
    if (off.magVec() > max_dist)
    {
        off.normVec();
        p = avatar + off * max_dist;
    }
    sVR.flyPos = p;
}

void LLVRCamera::turnView(F32 radians)
{
    if (!LLXrSession::active()) return;
    sVR.refYaw -= radians;   // turn = rig_yaw - refYaw: the view turns by +radians
}

void LLVRCamera::rigTurnedKeepView(F32 radians)
{
    if (!LLXrSession::active()) return;
    sVR.refYaw += radians;   // the rig yaw grew by radians: turn (and so the view) unchanged
}

F32 LLVRCamera::headYaw()
{
    return yawOf(LLViewerCamera::getInstance()->getAtAxis());
}

bool LLVRCamera::rigFollowsAgent()
{
    return isAgentAvatarValid() && !gAgentAvatarp->isSitting();
}

bool LLVRCamera::headAngles(const LLQuaternion& frame, F32& yaw, F32& pitch, F32& roll)
{
    if (!sActive || sVR.flyApplied || !sVR.mapValid) return false;
    const LLQuaternion inv = ~frame;
    LLVector3 at = sVR.headAt * inv;
    LLVector3 up = sVR.headUp * inv;
    if (at.normVec() < 1e-3f || up.normVec() < 1e-3f) return false;
    const LLVector3 left = up % at;
    yaw = std::atan2(at.mV[VY], at.mV[VX]);
    pitch = std::atan2(at.mV[VZ], std::sqrt(at.mV[VX] * at.mV[VX] + at.mV[VY] * at.mV[VY]));
    // Roll: the head's left axis against the level one (frame up x at, and at x that) about the
    // look axis.
    LLVector3 level_left = LLVector3::z_axis % at;
    roll = 0.f;
    if (level_left.normVec() > 1e-3f)
    {
        const LLVector3 level_up = at % level_left;
        roll = std::atan2(left * level_up, left * level_left);
    }
    return std::isfinite(yaw) && std::isfinite(pitch) && std::isfinite(roll);
}

void LLVRCamera::bodyFollowHead(F32 dt, bool walking)
{
    // Past START the body starts turning and keeps on until it faces the head within STOP
    // (a wide hysteresis: small head turns never move the body, and a turn, once started,
    // does not stop half way and restart). The rate eases with the remaining angle (TAU),
    // between MIN_RATE and MAX_RATE.
    constexpr F32 START = 70.f * DEG_TO_RAD;
    constexpr F32 STOP = 4.f * DEG_TO_RAD;
    constexpr F32 TAU = 0.25f;
    constexpr F32 MIN_RATE = 30.f * DEG_TO_RAD;
    constexpr F32 MAX_RATE = 150.f * DEG_TO_RAD;
    constexpr F32 MIN_HORIZONTAL = 0.3f;   // looking nearly straight up / down: no yaw to follow

    if (!sActive || sVR.flyApplied || !sVR.mapValid || walking || dt <= 0.f || !rigFollowsAgent() || gAgent.getAutoPilot())
    {
        sVR.bodyTurning = false;
        return;
    }
    const LLVector3& at = sVR.headAt;
    if (std::sqrt(at.mV[VX] * at.mV[VX] + at.mV[VY] * at.mV[VY]) < MIN_HORIZONTAL) return;
    const LLVector3 body = gAgent.getAtAxis();
    const F32 rel = yawDiff(yawOf(at), yawOf(body));
    if (!sVR.bodyTurning)
    {
        if (std::fabs(rel) <= START) return;
        sVR.bodyTurning = true;
        LL_DEBUGS("VR") << "body turns to follow the head (" << rel * RAD_TO_DEG << " deg)" << LL_ENDL;
    }
    if (std::fabs(rel) < STOP)
    {
        sVR.bodyTurning = false;
        return;
    }
    const F32 eased = std::fabs(rel) * (1.f - std::exp(-dt / TAU));
    const F32 mag = llmin(std::fabs(rel), llclamp(eased, MIN_RATE * dt, MAX_RATE * dt));
    const F32 step = rel > 0.f ? mag : -mag;
    gAgent.rotate(step, LLVector3::z_axis);
    rigTurnedKeepView(step);
    // As a key / joystick / autopilot turn: the sim and the AO play the turn animations (the
    // flag lasts this frame's agent update).
    gAgent.setControlFlags(step > 0.f ? AGENT_CONTROL_YAW_POS : AGENT_CONTROL_YAW_NEG);
}

bool LLVRCamera::appToAgent(const float p[3], LLVector3& out)
{
    if (!sVR.mapValid) return false;
    const LLVector3 s = xrToSL(p[0], p[1], p[2]);
    const LLVector3 off = rotZ(LLVector3(s.mV[VX] - sVR.refX, s.mV[VY] - sVR.refY, 0.f), sVR.mapTurn);
    out = sVR.mapAnchor + LLVector3(off.mV[VX], off.mV[VY], s.mV[VZ] + sVR.mapHeightAdd - sVR.refHeight);
    return true;
}

LLVector3 LLVRCamera::appDirToAgent(const float d[3])
{
    return rotZ(xrToSL(d[0], d[1], d[2]), sVR.mapValid ? sVR.mapTurn : 0.f);
}

LLVector3 LLVRCamera::appAxisToAgent(const float q[4], F32 x, F32 y, F32 z)
{
    return rotZ(headDirSL(q, x, y, z), sVR.mapValid ? sVR.mapTurn : 0.f);
}

bool LLVRCamera::eyeView(U32 eye, LLVector3& pos, LLVector3& at, LLVector3& left, LLVector3& up, F32 tan[4])
{
    if (!sActive || !sVR.mapValid || eye > 1u) return false;
    LLXrHeadPose hp;
    if (!LLXrSession::instance().headPose(hp) || !hp.eyesValid) return false;
    if (!appToAgent(hp.eyePos[eye], pos)) return false;
    // The eye's own axes (OpenXR -Z forward, -X left, +Y up), turned onto the rig like the head's.
    at = appAxisToAgent(hp.eyeOri[eye], 0.f, 0.f, -1.f);
    left = appAxisToAgent(hp.eyeOri[eye], -1.f, 0.f, 0.f);
    up = appAxisToAgent(hp.eyeOri[eye], 0.f, 1.f, 0.f);
    const F32* fv = hp.eyeFov[eye];
    tan[0] = std::tan(fv[0]);   // left (negative)
    tan[1] = std::tan(fv[1]);   // right
    tan[2] = std::tan(fv[3]);   // down (negative)
    tan[3] = std::tan(fv[2]);   // up
    return std::isfinite(tan[0] + tan[1] + tan[2] + tan[3]) && tan[1] > tan[0] && tan[3] > tan[2];
}

bool LLVRCamera::handPose(U32 hand, bool aim, LLVector3& pos, LLQuaternion& rot)
{
    if (!sActive || !sVR.mapValid || hand >= LLXR_HAND_COUNT) return false;
    LLXrInputState in;
    if (!LLXrSession::instance().inputState(in)) return false;
    const LLXrPose& p = aim ? in.hand[hand].aim : in.hand[hand].grip;
    if (!in.hand[hand].active || !p.valid || !appToAgent(p.position, pos)) return false;
    // The pose's own axes in SL terms: OpenXR -Z (forward) = at, -X = left, +Y = up.
    LLMatrix3 m;
    m.setRows(appAxisToAgent(p.orientation, 0.f, 0.f, -1.f), appAxisToAgent(p.orientation, -1.f, 0.f, 0.f),
              appAxisToAgent(p.orientation, 0.f, 1.f, 0.f));
    rot = m.quaternion();
    rot.normalize();
    return true;
}
