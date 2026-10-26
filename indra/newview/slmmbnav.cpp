/**
 * @file slmmbnav.cpp
 * @brief Blender-style middle-mouse camera navigation (#652)
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
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

#include "slmmbnav.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llappviewer.h"
#include "llfocusmgr.h"
#include "llui.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerinput.h"
#include "llviewermediafocus.h"
#include "llviewerobject.h"
#include "llviewerwindow.h"
#include "llwindow.h"
#include "lltoolpie.h"

namespace
{
    // Screen distance from the press (px) past which the press is a drag.
    constexpr S32 kDragPx = 6;
    // A press that stays put this long (s) starts push-to-talk.
    constexpr F32 kTalkHoldDelay = 0.25f;
}

SLMMBNav::SLMMBNav()
:   LLTool(std::string("MMBNav"))
{
}

// static
bool SLMMBNav::isEnabled()
{
    static LLCachedControl<bool> s_on(gSavedSettings, "SlipstreamMMBNav", false);
    return s_on;
}

// static
bool SLMMBNav::isActive()
{
    return instanceExists() && getInstance()->mState != IDLE;
}

bool SLMMBNav::tryBegin(S32 x, S32 y, MASK mask)
{
    if (!isEnabled() || gDisconnected) return false;
    if (mState != IDLE) endPress();

    // Mouselook and appearance editing have their own camera; a held left / right button
    // means a tool or an orbit is mid-drag.
    const ECameraMode mode = gAgentCamera.getCameraMode();
    if (mode == CAMERA_MODE_MOUSELOOK || mode == CAMERA_MODE_CUSTOMIZE_AVATAR) return false;
    if (gViewerWindow->getLeftMouseDown() || gViewerWindow->getRightMouseDown()) return false;
    if (gFocusMgr.getMouseCapture()) return false;

    // HUD attachments and the media prim that has focus keep the press (push-to-talk /
    // the page), as before.
    LLViewerObject* hover = LLToolPie::getInstance()->getHoverPick().getObject();
    if (hover)
    {
        if (hover->isHUDAttachment()) return false;
        if (LLViewerMediaFocus::getInstance()->getFocusedMediaImpl()
            && LLViewerMediaFocus::getInstance()->getFocusedObjectID() == hover->getID())
        {
            return false;
        }
    }

    // The orbit centre is picked NOW, while the press is being handled: rigged picks only get
    // their synchronous GPU answer inside handleAnyMouseClick's click scope (#255), and the
    // camera has not moved by the time the drag threshold is crossed.
    mPress = gViewerWindow->pickImmediate(x, y,
                                          /*pick_transparent*/ false,
                                          /*pick_rigged*/ true,
                                          /*pick_particle*/ false,
                                          /*pick_unselectable*/ true,
                                          /*pick_reflection_probe*/ false);

    mState = DECIDING;
    mDownX = x;
    mDownY = y;
    mDownMask = mask;
    mAccum = 0;
    mTalkDown = false;
    mHideCursor = false;
    mHoldTimer.reset();
    mHoldTimer.start();
    setMouseCapture(true);   // hover and the release reach us through the captor
    return true;
}

void SLMMBNav::firePushToTalk(bool down)
{
    if (down == mTalkDown) return;
    mTalkDown = down;
    // The same global bindings the press would have run (push-to-talk by default), with
    // the modifiers of the press so the release matches.
    gViewerInput.handleGlobalBindsMouse(CLICK_MIDDLE, mDownMask, down);
}

void SLMMBNav::beginNavigation()
{
    mState = NAVIGATING;

    // Orbit centre: the surface point under the cursor where the press happened (the
    // camera has not moved since); nothing there (sky): keep the current focus. Same
    // focus rule as an Alt-click, and the camera is left in free mode afterwards.
    if (!gAgentCamera.tpsCameraActive())
    {
        const LLPickInfo& pick = mPress;
        LLViewerObject* hit = pick.getObject();
        const bool hit_world = (hit && !hit->isHUDAttachment()) || (!hit && !pick.mPosGlobal.isExactlyZero());
        gAgentCamera.setFocusOnAvatar(false, ANIMATE);
        if (hit_world)
        {
            gAgentCamera.setFocusGlobal(pick);
        }
    }

    gViewerWindow->hideCursor();
    gViewerWindow->moveCursorToCenter();
    mHideCursor = true;
}

bool SLMMBNav::handleHover(S32 x, S32 y, MASK mask)
{
    if (mState == IDLE) return true;

    const ECameraMode mode = gAgentCamera.getCameraMode();
    if (mode == CAMERA_MODE_MOUSELOOK || mode == CAMERA_MODE_CUSTOMIZE_AVATAR)
    {
        // The camera mode changed under the press (M for mouselook, appearance): drop it.
        endPress();
        return true;
    }

    if (mState == TALKING)
    {
        // Sticky: once the microphone is keyed the press is push-to-talk until release, so
        // moving the mouse while talking never cuts the mic. To navigate, start moving at once.
        return true;
    }
    if (mState == DECIDING)
    {
        const S32 dist = llmax(llabs(x - mDownX), llabs(y - mDownY));
        if (dist >= kDragPx)
        {
            beginNavigation();
        }
        else if (mHoldTimer.getElapsedTimeF32() >= kTalkHoldDelay)
        {
            firePushToTalk(true);
            mState = TALKING;
        }
        return true;
    }

    // NAVIGATING
    const S32 dx = gViewerWindow->getCurrentMouseDX();
    const S32 dy = gViewerWindow->getCurrentMouseDY();
    static LLCachedControl<F32> s_sens(gSavedSettings, "SlipstreamMMBNavSensitivity", 1.f);
    const F32 sens = llclamp((F32)s_sens, 0.1f, 5.f);

    if (dx != 0 || dy != 0)
    {
        if (mask & MASK_SHIFT)
        {
            // Pan in the view plane. One screen pixel is dist * 2 tan(fov / 2) / height metres at
            // the focus distance, so the grabbed point follows the cursor.
            LLVector3d camera_to_focus = gAgentCamera.getCameraPositionGlobal() - gAgentCamera.getFocusGlobal();
            const F32 dist = llmax((F32)camera_to_focus.magVec(), 1.f);
            const F32 view_h = (F32)llmax(gViewerWindow->getWorldViewHeightRaw(), 1);
            const F32 meters_per_pixel = 2.f * dist * tanf(LLViewerCamera::getInstance()->getView() * 0.5f) / view_h;
            if (dx != 0) gAgentCamera.cameraPanLeft(dx * meters_per_pixel);
            if (dy != 0) gAgentCamera.cameraPanUp(-dy * meters_per_pixel);
        }
        else if (mask & MASK_CONTROL)
        {
            // Dolly (same response as the camera tool's zoom).
            if (dy != 0) gAgentCamera.cameraZoomIn((F32)pow(0.99, (F64)dy * sens));
        }
        else
        {
            const F32 radians_per_pixel = sens * 360.f * DEG_TO_RAD / gViewerWindow->getWorldViewWidthScaled();
            if (dx != 0) gAgentCamera.cameraOrbitAround(-dx * radians_per_pixel);
            if (dy != 0) gAgentCamera.cameraOrbitOver(-dy * radians_per_pixel);
        }
    }
    gViewerWindow->moveCursorToCenter();
    return true;
}

bool SLMMBNav::handleMiddleMouseUp(S32 x, S32 y, MASK mask)
{
    if (mState == DECIDING)
    {
        // A click that never moved or held: the push-to-talk tap it always was.
        firePushToTalk(true);
    }
    endPress();
    return true;
}

void SLMMBNav::onMouseCaptureLost()
{
    endPress();
}

void SLMMBNav::endPress()
{
    const EState was = mState;
    mState = IDLE;
    if (mTalkDown) firePushToTalk(false);
    if (was == NAVIGATING && mHideCursor)
    {
        // Centre of the window (#871); with MouseReleaseToCenter off, back where the
        // press started, as the camera tool does.
        if (!gViewerWindow->releaseCursorToCenter())
        {
            LLUI::getInstance()->setMousePositionScreen(mDownX, mDownY);
            gViewerWindow->showCursor();
        }
    }
    mHideCursor = false;
    if (hasMouseCapture())
    {
        setMouseCapture(false);   // onMouseCaptureLost() re-enters as a no-op: state is IDLE
    }
}
