/**
 * @file sltpscamera.h
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

#ifndef SL_TPSCAMERA_H
#define SL_TPSCAMERA_H

#include "llsingleton.h"
#include "lltool.h"
#include "llframetimer.h"
#include "llquaternion.h"

// Third-person shooter camera.
//
// While engaged the pointer is hidden, the window is in SDL relative mouse mode
// and the mouse orbits the (focus-on-avatar) third person camera around the
// avatar without turning it. Movement keys become camera relative: the avatar
// turns toward the direction it is walking. Left click sends the mouselook
// "ML_LBUTTON" control to scripts, right click / Esc releases the mouse (with
// SlipstreamTPSRightDragSteers a right press keeps it: a drag steers, a still click
// releases it and opens the context menu).
//
// Camera math lives in LLAgentCamera (tpsCameraActive / getTPSRotation); this
// class owns input capture, engagement state and movement.
class SLTPSCamera final : public LLSingleton<SLTPSCamera>
{
    LLSINGLETON(SLTPSCamera);
    ~SLTPSCamera();

public:
    enum EDisengageReason
    {
        DISENGAGE_USER,         // right click, Esc, setting/keybind toggle: leave the camera where it is
        DISENGAGE_MODE_CHANGE,  // sit, teleport, appearance, build, follow cam, ...: hand the camera back to LL
        DISENGAGE_MOUSELOOK,    // mouselook takes over: keep the cursor hidden
    };

    static bool isEnabled();                     // SlipstreamTPSCamera setting
    static bool isEngagedNow();                  // safe to call before the singleton exists
    static bool isSeated();                      // sitting / on an object (vehicle)
    // Engaged and on foot: only then do the movement keys become camera-relative.
    // Seated, WASD/arrows keep their stock behaviour (vehicle scripts, #95).
    static bool isEngagedOnFoot();
    static void disengageIfEngaged(EDisengageReason reason);

    // AgentUpdate (#881): while engaged on foot, report the aim the way mouselook
    // does. Mouselook pitches the agent frame (LLToolGun -> LLAgent::pitch), so its
    // BodyRotation carries the look pitch and HeadRotation is built from the camera
    // at-axis; scripts read that back through llGetRot. TPS keeps the body level, so
    // without this every shot went out flat. Pitch comes from the real camera
    // at-axis (the crosshair); body yaw stays the avatar's real heading (it turns
    // to the aim while shooting, see setLeftButton). Called from send_agent_update.
    static void applyAimToAgentUpdate(LLQuaternion& body_rotation, LLQuaternion& head_rotation);

    bool isEngaged() const { return mEngaged; }
    bool canEngage() const;
    bool engage();
    void disengage(EDisengageReason reason);
    void requestEngage() { mPendingEngage = true; } // engaged on the next update()
    void toggleEngaged();                        // keybinding

    // Per frame, from LLAppViewer::idle before send_agent_update().
    void update();

    // Movement key input while engaged (from llviewerinput.cpp).
    // at: +1 forward / -1 backward, left: +1 left / -1 right.
    void addMoveInput(S32 at, S32 left, bool nudge);

    // Left mouse (shoot) state
    // face_aim false: the click does not turn the avatar to the aim (a click on yourself)
    void setLeftButton(bool down, bool face_aim = true);
    // Right mouse (SlipstreamTPSRightDragSteers)
    void setRightButton(bool down);

    // Hint overlay + crosshair, drawn by LLViewerWindow.
    static void drawOverlay();

private:
    bool canStayEngaged() const;
    void updateMouse();
    void updateMovement();
    void resetMoveInput();
    void onSettingChanged(const LLSD& new_value);

    bool mEngaged = false;
    bool mPendingEngage = false;
    bool mLeftDown = false;
    bool mLeftFacesAim = false;
    bool mRightHeld = false;
    S32  mRightTravelX = 0;
    S32  mRightTravelY = 0;

    S32  mInputAt = 0;
    S32  mInputLeft = 0;
    bool mInputNudge = true;

    LLFrameTimer mHintTimer;
    boost::signals2::scoped_connection mSettingConnection;
};

// Mouse captor while the TPS camera is engaged. Not part of any toolset: it
// only holds gFocusMgr mouse capture so every click, hover and scroll comes
// here instead of the UI or the current tool.
class SLToolTPS final : public LLTool, public LLSingleton<SLToolTPS>
{
    LLSINGLETON(SLToolTPS);
    virtual ~SLToolTPS() = default;

public:
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleDoubleClick(S32 x, S32 y, MASK mask) override;
    bool handleRightMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleRightMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleMiddleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMiddleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleScrollWheel(S32 x, S32 y, S32 clicks) override;
    void onMouseCaptureLost() override;

    LLTool* getOverrideTool(MASK mask) override { return nullptr; }
    bool    clipMouseWhenDown() override { return false; }
};

#endif // SL_TPSCAMERA_H
