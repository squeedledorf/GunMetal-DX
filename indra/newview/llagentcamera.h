/**
 * @file llagent.h
 * @brief LLAgent class header file
 *
 * $LicenseInfo:firstyear=2000&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
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
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#ifndef LL_LLAGENTCAMERA_H
#define LL_LLAGENTCAMERA_H

#include <array>

#include "llflycam.h"
#include "llfollowcam.h"            // Ventrella
#include "llgamecontrol.h"          // LLGameControl::FLYCAM_NUM_CHANNELS
#include "llhudeffectlookat.h"      // EPointAtType
#include "llhudeffectpointat.h"     // ELookAtType
#include <boost/signals2/connection.hpp> // <SS:Skoomastream/> OTSEnabled listener

class LLPickInfo;
class LLVOAvatarSelf;
class LLControlVariable;

//--------------------------------------------------------------------
// Types
//--------------------------------------------------------------------
enum ECameraMode
{
    CAMERA_MODE_THIRD_PERSON,
    CAMERA_MODE_MOUSELOOK,
    CAMERA_MODE_CUSTOMIZE_AVATAR,
    CAMERA_MODE_FOLLOW,
    // <SS:Skoomastream>
    CAMERA_MODE_OTS          // Over-the-shoulder: mouselook input + third-person camera
    // </SS:Skoomastream>
};

/** Camera Presets for CAMERA_MODE_THIRD_PERSON */
enum ECameraPreset
{
    /** Default preset, what the Third Person Mode actually was */
    CAMERA_PRESET_REAR_VIEW,

    /** "Looking at the Avatar from the front" */
    CAMERA_PRESET_FRONT_VIEW,

    /** "Above and to the left, over the shoulder, pulled back a little on the zoom" */
    CAMERA_PRESET_GROUP_VIEW,

    /** Current view when a preset is saved */
    CAMERA_PRESET_CUSTOM,

// [RLVa:KB] - @setcam_eyeoffset and @setcam_focusoffset
    /* Used by RLVa */
    CAMERA_RLV_SETCAM_VIEW,
// [/RLVa:KB]
};

//------------------------------------------------------------------------
// LLAgentCamera
//------------------------------------------------------------------------
class LLAgentCamera
{
    LOG_CLASS(LLAgentCamera);

public:
    //--------------------------------------------------------------------
    // Constructors / Destructors
    //--------------------------------------------------------------------
public:
    LLAgentCamera();
    virtual         ~LLAgentCamera();
    void            init();
    void            cleanup();
    void            setAvatarObject(LLVOAvatarSelf* avatar);
    bool            isInitialized() { return mInitialized; }
private:
    bool            mInitialized;


    //--------------------------------------------------------------------
    // Mode
    //--------------------------------------------------------------------
public:
    void            changeCameraToDefault();
    void            changeCameraToMouselook(bool animate = true);
    void            changeCameraToThirdPerson(bool animate = true);
    void            changeCameraToCustomizeAvatar(); // Trigger transition animation
    void            changeCameraToFollow(bool animate = true);  // Ventrella
    // <SS:Skoomastream>
    void            changeCameraToOTS(bool animate = true); // Over-the-shoulder aim mode (animate=false snaps)
    void            changeCameraFromOTS();           // Exit OTS back to third person
    bool            canUseOTS();                     // seat (sit camera) and RLVa camera distance rules allow OTS
private:
    bool            getOTSRlvMaxDistance(F32& max_dist) const; // RLVa camera distance cap that applies in OTS
    bool            rlvAllowsOTS() const;            // the RLVa cap leaves room for a usable shoulder camera
    LLQuaternion    getOTSFrameRotation() const;     // agent frame, plus the seat's rotation when seated
    void            onOTSEnabledChanged(const LLSD& new_value);
    boost::signals2::connection mOTSEnabledConnection;
public:
    // </SS:Skoomastream>
    bool            cameraThirdPerson() const       { return (mCameraMode == CAMERA_MODE_THIRD_PERSON && mLastCameraMode == CAMERA_MODE_THIRD_PERSON); }
    // <SS:Skoomastream>
    // Also true for OTS -- reuses mouselook input and UI behaviour; camera position handled separately.
    // The mLastCameraMode guard filters the third-person->mouselook entry animation, but it must NOT
    // exclude the OTS<->mouselook swap: during that swap mCameraMode is MOUSELOOK while
    // mLastCameraMode is still OTS, and reporting third-person there flips the held movement keys from
    // slide to turn (key_bindings.xml first_person=slide vs third_person=turn) -- yawing the avatar mid-swap.
    // Treat "came from OTS" as still mouselook so the swap stays first-person input throughout.
    bool            cameraMouselook() const         { return (mCameraMode == CAMERA_MODE_MOUSELOOK && (mLastCameraMode == CAMERA_MODE_MOUSELOOK || mLastCameraMode == CAMERA_MODE_OTS)) || mCameraMode == CAMERA_MODE_OTS; }
    // </SS:Skoomastream>
    bool            cameraCustomizeAvatar() const   { return (mCameraMode == CAMERA_MODE_CUSTOMIZE_AVATAR /*&& !mCameraAnimating*/); }
    bool            cameraFollow() const            { return (mCameraMode == CAMERA_MODE_FOLLOW && mLastCameraMode == CAMERA_MODE_FOLLOW); }
    // <SS:Skoomastream>
    bool            cameraOTS() const               { return mCameraMode == CAMERA_MODE_OTS; }
    // </SS:Skoomastream>
    ECameraMode     getCameraMode() const           { return mCameraMode; }
    ECameraMode     getLastCameraMode() const       { return mLastCameraMode; }
    void            updateCamera();                 // Call once per frame to update camera location/orientation
    void            resetCamera();                  // Slam camera into its default position
    void            updateLastCamera();             // Set last camera to current camera

private:
    ECameraMode     mCameraMode;                    // Target mode after transition animation is done
    ECameraMode     mLastCameraMode;

    //--------------------------------------------------------------------
    // Preset
    //--------------------------------------------------------------------
public:
    void switchCameraPreset(ECameraPreset preset);
    ECameraPreset getCameraPreset() const { return mCameraPreset; }
    /** Determines default camera offset depending on the current camera preset */
    LLVector3 getCameraOffsetInitial();
// [RLVa:KB] - @setcam_eyeoffsetscale
    /** Determines default camera offset scale depending on the current camera preset */
    F32 getCameraOffsetScale() const;
// [/RLVa:KB]
    /** Determines default focus offset depending on the current camera preset */
    LLVector3d getFocusOffsetInitial();

    LLVector3 getCurrentCameraOffset();
    LLVector3d getCurrentFocusOffset();
    LLQuaternion getCurrentAvatarRotation();
    bool isJoystickCameraUsed();
    void setInitSitRot(LLQuaternion sit_rot) { mInitSitRot = sit_rot; };
    void rotateToInitSitRot();

    /** Determines maximum camera distance from target for mouselook, opposite to LAND_MIN_ZOOM */
    // (#1076: public -- the VR fly cam keeps to the same limit.)
    F32 getCameraMaxZoomDistance();

private:
    /** Camera preset in Third Person Mode */
    ECameraPreset mCameraPreset;

    LLQuaternion mInitSitRot;

    //--------------------------------------------------------------------
    // Position
    //--------------------------------------------------------------------
public:
    LLVector3d      getCameraPositionGlobal() const;
    const LLVector3& getCameraPositionAgent() const;
    LLVector3d      calcCameraPositionTargetGlobal(bool *hit_limit = NULL); // Calculate the camera position target
    F32             getCameraMinOffGround();        // Minimum height off ground for this mode, meters
    void            setCameraCollidePlane(const LLVector4 &plane) { mCameraCollidePlane = plane; }
    bool            calcCameraMinDistance(F32 &obj_min_distance);
    F32             getCurrentCameraBuildOffset() const { return (F32)mCameraFocusOffset.length(); }
    void            clearCameraLag() { mCameraLag.clearVec(); }
    const LLVector3& getCameraUpVector() const { return mCameraUpVector; }
private:
    LLVector3       getAvatarRootPosition();

    F32             mCurrentCameraDistance;         // Current camera offset from avatar
    F32             mTargetCameraDistance;          // Target camera offset from avatar
    // <SS:Skoomastream>
    F32             mOTSCollisionDistance;          // Smoothed OTS camera collision pull-in distance (<0 = unset)
    // </SS:Skoomastream>
    F32             mCameraFOVZoomFactor;           // Amount of fov zoom applied to camera when zeroing in on an object
    F32             mCameraCurrentFOVZoomFactor;    // Interpolated fov zoom
    LLVector4       mCameraCollidePlane;            // Colliding plane for camera
    F32             mCameraZoomFraction;            // Mousewheel driven fraction of zoom
    LLVector3       mCameraVirtualPositionAgent;    // Camera virtual position (target) before performing FOV zoom
    LLVector3d      mCameraSmoothingLastPositionGlobal;
    LLVector3d      mCameraSmoothingLastPositionAgent;
    bool            mCameraSmoothingStop;
    LLVector3       mCameraLag;                     // Third person camera lag
    LLVector3       mCameraUpVector;                // Camera's up direction in world coordinates (determines the 'roll' of the view)

    //--------------------------------------------------------------------
    // Follow
    //--------------------------------------------------------------------
public:
    bool            isfollowCamLocked();
private:
    LLFollowCam     mFollowCam;             // Ventrella
    // Fractional scrollCamera() clicks not yet applied to mFollowCam, whose
    // zoom() only takes whole clicks.
    F32             mFollowCamScrollClicks { 0.f };

    //--------------------------------------------------------------------
    // Flycam
    //--------------------------------------------------------------------
    // Free-flying camera engine (mFlycam + toggle state below), shared by two
    // independent input sources that each feed it differently:
    //   - game-control/keyboard: updateFlycam() + mFlycamKeyboard* state here.
    //   - legacy NDOF joystick: applyNdofFlycamFrameDelta() below, called
    //     from LLViewerJoystick, whose OWN per-axis tuning state
    //     (mJoystickFlycamDelta/mJoystickFlycamLastDelta) lives over there,
    //     not on this class.
    // Orthogonal to mCameraMode above: while active it preempts
    // updateCamera() entirely (driven by updateFlycam()/
    // applyNdofFlycamFrameDelta() instead, called directly from the main
    // loop), rather than being a peer state in that mode's own
    // animated-transition machinery.
public:
    bool            isUsingFlycam() const { return mUsingFlycam; }
    void            toggleFlycam();

    // Game-control/keyboard-driven update -- see LLViewerJoystick::moveFlycam()
    // for the other (legacy NDOF joystick) input source, below.
    void            updateFlycam(F32 delta_time);
    void            setFlycamKeyInput(U8 channel, F32 value) { mFlycamKeyboardInput[channel] = value; }
    void            setFlycamKeyUnroll(bool unroll) { mFlycamKeyboardUnrollRequested = unroll; }
    // Snapshots the live LLViewerCamera transform into the flycam. Used both
    // when entering flycam (see toggleFlycam()) and whenever an input source
    // (e.g. the NDOF joystick regaining focus) needs to recalibrate its own
    // axis-delta state against the camera's current position/orientation.
    void            resetFlycamToCurrentView();

    // Entry point for input sources (the legacy NDOF joystick) that compute
    // their own per-axis dead-zone/scale/feathering deltas and only need this
    // class to own the resulting transform update + push to LLViewerCamera --
    // see LLFlycam::applyFrameDelta() for the delta format.
    void            applyNdofFlycamFrameDelta(const F32 local_delta[7],
                                               bool auto_level, F32 auto_level_fraction,
                                               bool direct_view, F32 direct_view_value);
    // Hides the mouse cursor while in CONTROL_MODE_FLYCAM (when enabled by
    // LLGameControl::isFlycamHideCursorEnabled()).  The cursor reappears while
    // the mouse itself moves and hides again when its timeout expires: the
    // timeout is reset to 5 seconds whenever the mouse moves and cut to at most
    // 1 second whenever the flycam camera moves.  Restored on leaving the mode.
    void            updateFlycamCursorVisibility(LLGameControl::AgentControlMode mode);

    // True when the self avatar should be hidden for a first-person view: in
    // mouselook, unless flycam has since taken the camera out of the head (see
    // mFlycamHidingAvatar). Use this rather than cameraMouselook() when deciding
    // whether to render the self avatar.
    bool            isHidingAvatarForFirstPerson() const;
private:
    // Clears mFlycamHidingAvatar once the self avatar is completely out of the
    // flycam's view, so it doesn't pop into existence on screen. Called after
    // each flycam transform update.
    void            updateFlycamAvatarVisibility();

    // Set when flycam is entered from mouselook: the camera starts inside the
    // avatar's head, so the avatar stays hidden (as in mouselook) until
    // updateFlycamAvatarVisibility() finds it out of view.
    bool            mFlycamHidingAvatar { false };

    LLFlycam        mFlycam;
    bool            mUsingFlycam { false };

    // Keyboard's contribution to the shared/game-control flycam channels
    // (blended with LLGameControl::getFlycamInputs() each frame in
    // updateFlycam()) -- unrelated to the legacy joystick's own tuning state,
    // which lives on LLViewerJoystick (mJoystickFlycamDelta/LastDelta).
    std::array<F32, LLGameControl::FLYCAM_NUM_CHANNELS> mFlycamKeyboardInput {};
    bool            mFlycamKeyboardUnrollRequested { false };

    // updateFlycamCursorVisibility() state: mFlycamCursorHiding is true while
    // FlyCam cursor hiding is in effect, mFlycamCursorHidden while the cursor is
    // actually hidden by it.  Times are LLFrameTimer::getElapsedSeconds():
    // mFlycamCursorMouseMoveTime is the last mouse move seen, and
    // mFlycamCursorHideTime is when a visible cursor hides again.  The
    // mFlycamCursorLast* values are the previous frame's flycam transform and
    // view angle, used to detect flycam motion.
    bool            mFlycamCursorHiding { false };
    bool            mFlycamCursorHidden { false };
    F64             mFlycamCursorMouseMoveTime { 0.0 };
    F64             mFlycamCursorHideTime { 0.0 };
    LLVector3d      mFlycamCursorLastPosition;
    LLQuaternion    mFlycamCursorLastRotation;
    F32             mFlycamCursorLastView { 0.f };

    //--------------------------------------------------------------------
    // Sit
    //--------------------------------------------------------------------
public:
    void            setupSitCamera();
    bool            sitCameraEnabled()      { return mSitCameraEnabled; }
    void            setSitCamera(const LLUUID &object_id,
                                 const LLVector3 &camera_pos = LLVector3::zero, const LLVector3 &camera_focus = LLVector3::zero);
private:
    LLPointer<LLViewerObject> mSitCameraReferenceObject; // Object to which camera is related when sitting
    bool            mSitCameraEnabled;      // Use provided camera information when sitting?
    LLVector3       mSitCameraPos;          // Root relative camera pos when sitting
    LLVector3       mSitCameraFocus;        // Root relative camera target when sitting

    //--------------------------------------------------------------------
    // Animation
    //--------------------------------------------------------------------
public:
    void            setCameraAnimating(bool b)          { mCameraAnimating = b; }
    bool            getCameraAnimating()                { return mCameraAnimating; }
    void            setAnimationDuration(F32 seconds);
    // <GMDX> One-shot duration (seconds) for the NEXT startCameraAnimation; < 0 = ZoomTime. ADS swap.
    void            setNextCameraAnimationDuration(F32 seconds) { mNextAnimationDuration = seconds; }
    void            startCameraAnimation();
    void            stopCameraAnimation();
private:
    LLFrameTimer    mAnimationTimer;    // Seconds that transition animation has been active
    F32             mAnimationDuration; // In seconds
    F32             mNextAnimationDuration = -1.f; // <GMDX> one-shot override, < 0 = none
    bool            mCameraAnimating;                   // Camera is transitioning from one mode to another
    LLVector3d      mAnimationCameraStartGlobal;        // Camera start position, global coords
    LLVector3d      mAnimationFocusStartGlobal;         // Camera focus point, global coords
    // <SS:Skoomastream>
    // Avatar pose at animation start, used to re-anchor the start points to the
    // avatar's frame each step so turning/strafing during the swap (e.g. the
    // OTS<->mouselook glide) keeps the view tracking the avatar instead of staying
    // pinned to a stale world direction.
    LLVector3d      mAnimationStartRootGlobal;          // avatar root pos at animation start
    LLQuaternion    mAnimationStartAgentRot;            // agent frame rotation at animation start
    // </SS:Skoomastream>

    //--------------------------------------------------------------------
    // Focus
    //--------------------------------------------------------------------
public:
    LLVector3d      calcFocusPositionTargetGlobal();
    LLVector3       calcFocusOffset(LLViewerObject *object, LLVector3 pos_agent, S32 x, S32 y);
    bool            getFocusOnAvatar() const        { return mFocusOnAvatar; }
    LLPointer<LLViewerObject>&  getFocusObject()    { return mFocusObject; }
    F32             getFocusObjectDist() const      { return mFocusObjectDist; }
    void            updateFocusOffset();
    void            validateFocusObject();
    void            setFocusGlobal(const LLPickInfo& pick);
    void            setFocusGlobal(const LLVector3d &focus, const LLUUID &object_id = LLUUID::null);
    void            setFocusOnAvatar(bool focus, bool animate, bool reset_axes = true);
    void            setCameraPosAndFocusGlobal(const LLVector3d& pos, const LLVector3d& focus, const LLUUID &object_id);
    void            clearFocusObject();
    void            setFocusObject(LLViewerObject* object);
    void            setAllowChangeToFollow(bool focus)  { mAllowChangeToFollow = focus; }
    void            setObjectTracking(bool track)   { mTrackFocusObject = track; }
    const LLVector3d &getFocusGlobal() const        { return mFocusGlobal; }
    const LLVector3d &getFocusTargetGlobal() const  { return mFocusTargetGlobal; }
private:
    LLVector3d      mCameraFocusOffset;             // Offset from focus point in build mode
    LLVector3d      mCameraFocusOffsetTarget;       // Target towards which we are lerping the camera's focus offset
    bool            mFocusOnAvatar;
    bool            mAllowChangeToFollow;
    LLVector3d      mFocusGlobal;
    LLVector3d      mFocusTargetGlobal;
    LLPointer<LLViewerObject> mFocusObject;
    F32             mFocusObjectDist;
    LLVector3       mFocusObjectOffset;
    bool            mTrackFocusObject;

    //--------------------------------------------------------------------
    // Lookat / Pointat
    //--------------------------------------------------------------------
public:
    void            updateLookAt(const S32 mouse_x, const S32 mouse_y);
    bool            setLookAt(ELookAtType target_type, LLViewerObject *object = NULL, LLVector3 position = LLVector3::zero);
    ELookAtType     getLookAtType();
    void            lookAtLastChat();
    void            slamLookAt(const LLVector3 &look_at); // Set the physics data
    bool            setPointAt(EPointAtType target_type, LLViewerObject *object = NULL, LLVector3 position = LLVector3::zero);
    EPointAtType    getPointAtType();
    // Slipstream (#902): true while the camera is detached from the avatar (free camera,
    // flycam, camera recorder) and the look-at follows what the camera looks at.
    bool            freeCameraLookAtActive() const  { return mFreeCamLookAt; }
private:
    bool            updateFreeCameraLookAt();   // true: detached, the look-at is handled
    bool            mFreeCamLookAt = false;     // the free-camera look-at owned the target last frame
public:
    LLPointer<LLHUDEffectLookAt> mLookAt;
    LLPointer<LLHUDEffectPointAt> mPointAt;

    //--------------------------------------------------------------------
    // Third person
    //--------------------------------------------------------------------
public:
    LLVector3d      calcThirdPersonFocusOffset();
    void            setThirdPersonHeadOffset(LLVector3 offset)  { mThirdPersonHeadOffset = offset; }
private:
    LLVector3       mThirdPersonHeadOffset;                     // Head offset for third person camera position

    //--------------------------------------------------------------------
    // Third-person shooter (TPS) camera. While engaged (see
    // SLTPSCamera), the focus-on-avatar third person camera orbits using its
    // own yaw/pitch instead of the agent frame, so looking around does not
    // turn the avatar.
    //--------------------------------------------------------------------
public:
    void            setTPSCameraEngaged(bool engaged, F32 yaw = 0.f, F32 pitch = 0.f);
    bool            getTPSCameraEngaged() const { return mTPSCameraEngaged; }
    bool            getTPSVehiclePivot(LLVector3& pivot_agent) const; // seated: root prim of the sat-on object
    bool            tpsCameraActive() const;                   // engaged AND in a mode where the TPS rotation applies
    void            tpsCameraOrbit(F32 yaw_delta, F32 pitch_delta); // radians; +pitch looks down (LLAgent::pitch convention)
    F32             getTPSYaw() const   { return mTPSYaw; }
    F32             getTPSPitch() const { return mTPSPitch; }
    LLQuaternion    getTPSRotation() const;                     // pitch then yaw, world frame
    LLQuaternion    getTPSYawRotation() const;                  // yaw only, world frame
    LLVector3       getTPSShoulderOffset() const;               // world-space sideways offset (SlipstreamTPSShoulderOffset)
private:
    bool            mTPSCameraEngaged = false;
    F32             mTPSYaw = 0.f;
    F32             mTPSPitch = 0.f;

    //--------------------------------------------------------------------
    // Orbit
    //--------------------------------------------------------------------
public:
    void            cameraOrbitAround(const F32 radians);   // Rotate camera CCW radians about build focus point
    void            cameraOrbitOver(const F32 radians);     // Rotate camera forward radians over build focus point
    void            cameraOrbitIn(const F32 meters);        // Move camera in toward build focus point
    void            cameraRollOver(const F32 radians);      // Roll the camera
    void            resetCameraOrbit();
    void            resetOrbitDiff();
    void            resetCameraRoll();
    F32             getRollAngle() const { return mRollAngle; }         // <GMDX> (camera floater 90 degree portrait)
    void            setRollAngle(F32 radians) { mRollAngle = radians; } // <GMDX>
    //--------------------------------------------------------------------
    // Zoom
    //--------------------------------------------------------------------
public:
    void            handleScrollWheel(F32 clicks);                          // Mousewheel driven zoom (fractional: touchpads, CameraSmoothZoom)
    void            updateSmoothZoom();
    void            queueWheelOrbitZoom(F32 notches);
    void            applyWheelOrbitZoom(F32 notches);                                     // per frame: ease the pending wheel zoom
    // The wheel's camera distance change minus its wheel-only parts (flycam FOV, HUD edit zoom,
    // Shift/Ctrl offset tweaks): also the game controller's "Scroll" (fractional clicks).
    // Positive moves the camera out, same sign as handleScrollWheel().
    void            scrollCamera(F32 clicks);
    void            cameraZoomIn(const F32 factor);                         // Zoom in by fraction of current distance
    void            cameraZoomDrag(const F32 factor);                       // Same, also in the follow view (right-drag zoom)
    F32             getCameraZoomFraction(bool get_third_person = false);   // Get camera zoom as fraction of minimum and maximum zoom
    void            setCameraZoomFraction(F32 fraction);                    // Set camera zoom as fraction of minimum and maximum zoom
    F32             calcCameraFOVZoomFactor();
    F32             getAgentHUDTargetZoom();

    void            resetCameraZoomFraction();
    F32             getCurrentCameraZoomFraction() const { return mCameraZoomFraction; }

    //--------------------------------------------------------------------
    // Pan
    //--------------------------------------------------------------------
public:
    void            cameraPanIn(const F32 meters);
    void            cameraPanLeft(const F32 meters);
    void            cameraPanUp(const F32 meters);
    void            resetCameraPan();
    void            resetPanDiff();
    //--------------------------------------------------------------------
    // View
    //--------------------------------------------------------------------
public:
    // Called whenever the agent moves.  Puts camera back in default position, deselects items, etc.
    void            resetView(bool reset_camera = true, bool change_camera = false);
    // Called on camera movement.  Unlocks camera from the default position behind the avatar.
    void            unlockView();
public:
    F32             mDrawDistance;      // RENDER draw distance (culling, camera far, caches)

    //--------------------------------------------------------------------
    // Network draw distance
    //--------------------------------------------------------------------
    // What we ask the SERVER for (AgentUpdate Far): the simulator enables a child agent
    // in every neighbour within Far and fills its interest list out to Far. Kept apart
    // from the render draw distance: at 4096 m on mainland Far makes the root sim stand
    // up child agents in hundreds of neighbours, re-done on every crossing.
    //   min(render DD, SLNetDrawDistanceMax, and while the agent moves faster than
    //   SLNetDrawDistanceFastSpeed: clamp(speed x SLNetDrawDistanceFastLeadSeconds +
    //   SLNetDrawDistanceFastMargin, SLNetDrawDistanceFastMin, SLNetDrawDistanceFastMax))
    // This is the TARGET; send_agent_update lowers Far toward it at a limited rate
    // (SLNetDrawDistanceDecreaseRate), ramps it up (10 % per second) and halves it on
    // teleport arrival. The region ring uses it for its connect rules (#850).
    F32             getNetworkDrawDistance();
    bool            isNetFastMoving() const             { return mNetFast; }
    F32             getNetMoveSpeed() const             { return mNetSpeed; }
    // Far as last sent in AgentUpdate (-1 = none yet). Can lag the target while it ramps.
    void            setNetFarSent(F32 far_m)            { mNetFarSent = far_m; }
    F32             getNetFarSent() const               { return mNetFarSent; }
private:
    void            updateNetMoveSpeed();
    LLVector3d      mNetLastPos;
    F64             mNetLastSample = -1.0;
    F64             mNetFastCandidateSince = -1.0;  // above the threshold since (enter hysteresis)
    F64             mNetSlowCandidateSince = -1.0;  // below the exit threshold since (leave hysteresis)
    F32             mNetSpeed = 0.f;                // smoothed horizontal speed, m/s
    bool            mNetFast = false;
    F32             mNetFarSent = -1.f;
public:

    //--------------------------------------------------------------------
    // Mouselook
    //--------------------------------------------------------------------
public:
    bool            getForceMouselook() const           { return mForceMouselook; }
    void            setForceMouselook(bool mouselook)   { mForceMouselook = mouselook; }
private:
    bool            mForceMouselook;

    //--------------------------------------------------------------------
    // HUD
    //--------------------------------------------------------------------
public:
    // HUD edit zoom (#413): the mouse wheel sets the target while a HUD selection is
    // edited; updateHUDZoom() eases the current zoom toward it once per frame. Every
    // HUD-space consumer (get_hud_matrices -> VK HUD pass + outline + HUD text,
    // mousePointHUD -> HUD pick, the manipulators) reads mHUDCurZoom.
    static constexpr F32 HUD_ZOOM_MIN = 0.1f;   // zoom out far enough to find off-screen HUDs
    static constexpr F32 HUD_ZOOM_MAX = 10.f;   // zoom in for fine work
    void            updateHUDZoom();
    bool            isEditingHUD() const;   // build mode with a HUD selection
    // Set the HUD target zoom. about_cursor: zooming in keeps the HUD point under the
    // mouse fixed (else the view centre). Zooming out walks the pan back so it is
    // zero again at 1x.
    void            setHUDTargetZoom(F32 target_zoom, bool about_cursor);
    void            resetHUDZoom();         // target 1x, no pan (the current values ease home)
    F32             mHUDTargetZoom; // Target zoom level for HUD objects (used when editing)
    F32             mScrollAccum = 0.f;     // fractional wheel carried between events for the whole-step branches
    F32             mPendingWheelZoom = 0.f; // wheel zoom (notches) not yet applied (CameraSmoothZoom)
    F32             mHUDCurZoom;    // Current animated zoom level for HUD objects
    // HUD-local point (x = 0) at the centre of the zoomed HUD view. A HUD-local point p
    // shows at HUD-screen offset mHUDCurZoom * (p - mHUDCurPan) (get_hud_matrices).
    LLVector3       mHUDTargetPan;
    LLVector3       mHUDCurPan;

// [RLVa:KB] - Checked: RLVa-2.0.0
    //--------------------------------------------------------------------
    // RLVa
    //--------------------------------------------------------------------
protected:
    bool allowFocusOffsetChange(const LLVector3d& offsetFocus);
    bool clampCameraPosition(LLVector3d& posCamGlobal, const LLVector3d posCamRefGlobal, float nDistMin, float nDistMax);

    bool m_fRlvMaxDist;             // True if the camera is at max distance
    bool m_fRlvMinDist;             // True if the camera is at min distance
    LLVector3d m_posRlvRefGlobal;       // Current reference point for distance calculations
// [/RLVa:KB]

/********************************************************************************
 **                                                                            **
 **                    KEYS
 **/

public:
    S32             getAtKey() const        { return mAtKey; }
    S32             getWalkKey() const      { return mWalkKey; }
    S32             getLeftKey() const      { return mLeftKey; }
    S32             getUpKey() const        { return mUpKey; }
    F32             getYawKey() const       { return mYawKey; }
    F32             getPitchKey() const     { return mPitchKey; }

    void            setAtKey(S32 mag)       { mAtKey = mag; }
    void            setWalkKey(S32 mag)     { mWalkKey = mag; }
    void            setLeftKey(S32 mag)     { mLeftKey = mag; }
    void            setUpKey(S32 mag)       { mUpKey = mag; }
    void            setYawKey(F32 mag)      { mYawKey = mag; }
    void            setPitchKey(F32 mag)    { mPitchKey = mag; }

    void            clearGeneralKeys();
    static S32      directionToKey(S32 direction); // Changes direction to -1/0/1

private:
    S32             mAtKey;             // Either 1, 0, or -1. Indicates that movement key is pressed
    S32             mWalkKey;           // Like AtKey, but causes less forward thrust
    S32             mLeftKey;
    S32             mUpKey;
    F32             mYawKey;
    F32             mPitchKey;

    //--------------------------------------------------------------------
    // Orbit
    //--------------------------------------------------------------------
public:
    F32             getOrbitLeftKey() const     { return mOrbitLeftKey; }
    F32             getOrbitRightKey() const    { return mOrbitRightKey; }
    F32             getOrbitUpKey() const       { return mOrbitUpKey; }
    F32             getOrbitDownKey() const     { return mOrbitDownKey; }
    F32             getOrbitInKey() const       { return mOrbitInKey; }
    F32             getOrbitOutKey() const      { return mOrbitOutKey; }
    F32             getRollLeftKey() const      { return mRollLeftKey; }
    F32             getRollRightKey() const     { return mRollRightKey; }

    void            setOrbitLeftKey(F32 mag)    { mOrbitLeftKey = mag; }
    void            setOrbitRightKey(F32 mag)   { mOrbitRightKey = mag; }
    void            setOrbitUpKey(F32 mag)      { mOrbitUpKey = mag; }
    void            setOrbitDownKey(F32 mag)    { mOrbitDownKey = mag; }
    void            setOrbitInKey(F32 mag)      { mOrbitInKey = mag; }
    void            setOrbitOutKey(F32 mag)     { mOrbitOutKey = mag; }
    void            setRollLeftKey(F32 mag) { mRollLeftKey = mag; }
    void            setRollRightKey(F32 mag) { mRollRightKey = mag; }

    void            clearOrbitKeys();
private:
    F32             mOrbitLeftKey;
    F32             mOrbitRightKey;
    F32             mOrbitUpKey;
    F32             mOrbitDownKey;
    F32             mOrbitInKey;
    F32             mOrbitOutKey;

    F32             mOrbitAroundRadians;
    F32             mOrbitOverAngle;

    F32             mRollLeftKey;
    F32             mRollRightKey;
    F32             mRollAngle = 0.f;

    //--------------------------------------------------------------------
    // Pan
    //--------------------------------------------------------------------
public:
    F32             getPanLeftKey() const       { return mPanLeftKey; }
    F32             getPanRightKey() const  { return mPanRightKey; }
    F32             getPanUpKey() const     { return mPanUpKey; }
    F32             getPanDownKey() const       { return mPanDownKey; }
    F32             getPanInKey() const     { return mPanInKey; }
    F32             getPanOutKey() const        { return mPanOutKey; }

    void            setPanLeftKey(F32 mag)      { mPanLeftKey = mag; }
    void            setPanRightKey(F32 mag)     { mPanRightKey = mag; }
    void            setPanUpKey(F32 mag)        { mPanUpKey = mag; }
    void            setPanDownKey(F32 mag)      { mPanDownKey = mag; }
    void            setPanInKey(F32 mag)        { mPanInKey = mag; }
    void            setPanOutKey(F32 mag)       { mPanOutKey = mag; }

    void            clearPanKeys();
private:
    F32             mPanUpKey;
    F32             mPanDownKey;
    F32             mPanLeftKey;
    F32             mPanRightKey;
    F32             mPanInKey;
    F32             mPanOutKey;

    LLVector3d      mPanFocusDiff;

/**                    Keys
 **                                                                            **
 *******************************************************************************/
public:
    void            storeCameraPosition();
    void            loadCameraPosition();
};

extern LLAgentCamera gAgentCamera;

#endif
