/**
 * @file lltoolfocus.cpp
 * @brief A tool to set the build focus point.
 *
 * $LicenseInfo:firstyear=2001&license=viewerlgpl$
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

#include "llviewerprecompiledheaders.h"

// File includes
#include "lltoolfocus.h"

// Library includes
#include "v3math.h"
#include "llfontgl.h"
#include "llui.h"

// Viewer includes
#include "llagent.h"
#include "llagentcamera.h"
#include "llbutton.h"
#include "llviewercontrol.h"
#include "lldrawable.h"
#include "lltooltip.h"
#include "llhudmanager.h"
#include "llfloatertools.h"
#include "llselectmgr.h"
#include "llstatusbar.h"
#include "lltoolmgr.h"
#include "llviewercamera.h"
#include "llviewerobject.h"
#include "llviewerwindow.h"
#include "llvoavatarself.h"
#include "llmorphview.h"
#include "llfloaterreg.h"
#include "llfloatercamera.h"
#include "llmenugl.h"
#include "sltpscamera.h"

// Globals
bool gCameraBtnZoom = true;
bool gCameraBtnOrbit = false;
bool gCameraBtnPan = false;

const S32 SLOP_RANGE = 4;

//
// Camera - shared functionality
//

LLToolCamera::LLToolCamera()
:   LLTool(std::string("Camera")),
    mAccumX(0),
    mAccumY(0),
    mMouseDownX(0),
    mMouseDownY(0),
    mOutsideSlopX(false),
    mOutsideSlopY(false),
    mValidClickPoint(false),
    mClickPickPending(false),
    mValidSelection(false),
    mMouseSteering(false),
    mMouseWalking(false),
    mMouseUpX(0),
    mMouseUpY(0),
    mMouseUpMask(MASK_NONE)
{ }


LLToolCamera::~LLToolCamera()
{ }

// virtual
void LLToolCamera::handleSelect()
{
    if (gFloaterTools)
    {
        gFloaterTools->setStatusText("camera");
        // in case we start from tools floater, we count any selection as valid
        mValidSelection = gFloaterTools->getVisible();
    }
}

// virtual
void LLToolCamera::handleDeselect()
{
//  gAgent.setLookingAtAvatar(false);

    // Make sure that temporary selection won't pass anywhere except pie tool.
    MASK override_mask = gKeyboard ? gKeyboard->currentMask(true) : 0;
    if (!mValidSelection && (override_mask != MASK_NONE || (gFloaterTools && gFloaterTools->getVisible())))
    {
        LLMenuGL::sMenuContainer->hideMenus();
        LLSelectMgr::getInstance()->validateSelection();
    }
}

bool LLToolCamera::handleMouseDown(S32 x, S32 y, MASK mask)
{
    // Ensure a mouseup
    setMouseCapture(true);

    // call the base class to propogate info to sim
    LLTool::handleMouseDown(x, y, mask);

    mAccumX = 0;
    mAccumY = 0;

    mOutsideSlopX = false;
    mOutsideSlopY = false;

    mValidClickPoint = false;

    // Sometimes Windows issues down and up events near simultaneously
    // without giving async pick a chance to trigged
    // Ex: mouse from numlock emulation
    mClickPickPending = true;

    // If mouse capture gets ripped away, claim we moused up
    // at the point we moused down. JC
    mMouseUpX = x;
    mMouseUpY = y;
    mMouseUpMask = mask;

    gViewerWindow->hideCursor();

    gViewerWindow->pickAsync(x, y, mask, pickCallback, /*bool pick_transparent*/ false, /*bool pick_rigged*/ false, /*bool pick_unselectable*/ true);

    return true;
}

void LLToolCamera::pickCallback(const LLPickInfo& pick_info)
{
    LLToolCamera* camera = LLToolCamera::getInstance();
    if (!camera->mClickPickPending)
    {
        return;
    }
    camera->mClickPickPending = false;
    const bool self_click = camera->mSelfClick;
    camera->mSelfClick = false;

    camera->mMouseDownX = pick_info.mMousePt.mX;
    camera->mMouseDownY = pick_info.mMousePt.mY;

    gViewerWindow->moveCursorToCenter();

    // Potentially recenter if click outside rectangle
    LLViewerObject* hit_obj = pick_info.getObject();

    // Check for hit the sky, or some other invalid point
    if (!hit_obj && pick_info.mPosGlobal.isExactlyZero())
    {
        camera->mValidClickPoint = false;
        return;
    }

    // check for hud attachments
    if (hit_obj && hit_obj->isHUDAttachment())
    {
        LLObjectSelectionHandle selection = LLSelectMgr::getInstance()->getSelection();
        if (!selection->getObjectCount() || selection->getSelectType() != SELECT_TYPE_HUD)
        {
            camera->mValidClickPoint = false;
            return;
        }
    }

    if( CAMERA_MODE_CUSTOMIZE_AVATAR == gAgentCamera.getCameraMode() )
    {
        bool good_customize_avatar_hit = false;
        if( hit_obj )
        {
            if (isAgentAvatarValid() && (hit_obj == gAgentAvatarp))
            {
                // It's you
                good_customize_avatar_hit = true;
            }
            else if (hit_obj->isAttachment() && hit_obj->permYouOwner())
            {
                // It's an attachment that you're wearing
                good_customize_avatar_hit = true;
            }
        }

        if( !good_customize_avatar_hit )
        {
            camera->mValidClickPoint = false;
            return;
        }

        if( gMorphView )
        {
            gMorphView->setCameraDrivenByKeys( false );
        }
    }
    //RN: check to see if this is mouse-driving as opposed to ALT-zoom or Focus tool
    else if (pick_info.mKeyMask & MASK_ALT ||
            (LLToolMgr::getInstance()->getCurrentTool()->getName() == "Camera"))
    {
        LLViewerObject* hit_obj = pick_info.getObject();
        if (self_click)
        {
            // A click on yourself (LLToolPie) never refocuses: the camera stays as it is.
        }
        else if (hit_obj && hit_obj->isAvatar())
        {
            // #958: an avatar (its bare body) is focused on the point that was hit, kept
            // within reach of the avatar. The object-relative offset used for prims is taken
            // from the avatar's drawable and added to its object position, and the two can
            // disagree for a GPU-animated avatar, throwing the focus (and the camera) away.
            LLVOAvatar* avatarp = hit_obj->asAvatar();
            const LLVector3d char_pos = gAgent.getPosGlobalFromAgent(avatarp->getCharacterPosition());
            LLVector3d focus = pick_info.mPosGlobal;
            const F64 MAX_FOCUS_DIST = 4.0;   // (collision volumes and the name tag lie within ~3 m)
            if (focus.isExactlyZero() || (focus - char_pos).length() > MAX_FOCUS_DIST)
            {
                focus = char_pos;
            }
            gAgentCamera.setFocusOnAvatar(false, ANIMATE);
            gAgentCamera.setFocusGlobal(focus, pick_info.mObjectID);
        }
        else if (hit_obj)
        {
            // ...clicked on a world object, so focus at its position
            if (!hit_obj->isHUDAttachment())
            {
                gAgentCamera.setFocusOnAvatar(false, ANIMATE);
                gAgentCamera.setFocusGlobal(pick_info);
            }
        }
        else if (!pick_info.mPosGlobal.isExactlyZero())
        {
            // Hit the ground
            gAgentCamera.setFocusOnAvatar(false, ANIMATE);
            gAgentCamera.setFocusGlobal(pick_info);
        }

        static LLCachedControl<bool> freeze_time(gSavedSettings, "FreezeTime", false);
        bool zoom_tool = gCameraBtnZoom && (LLToolMgr::getInstance()->getBaseTool() == LLToolCamera::getInstance());
        if (!(pick_info.mKeyMask & MASK_ALT) &&
            !LLFloaterCamera::inFreeCameraMode() &&
            !zoom_tool &&
            gAgentCamera.cameraThirdPerson() &&
            gViewerWindow->getLeftMouseDown() &&
            !freeze_time &&
            (hit_obj == gAgentAvatarp ||
             (hit_obj && hit_obj->isAttachment() && LLVOAvatar::findAvatarFromAttachment(hit_obj)->isSelf())))
        {
            LLToolCamera::getInstance()->mMouseSteering = true;
        }

    }

    camera->mValidClickPoint = true;

    if( CAMERA_MODE_CUSTOMIZE_AVATAR == gAgentCamera.getCameraMode() )
    {
        gAgentCamera.setFocusOnAvatar(false, false);

        LLVector3d cam_pos = gAgentCamera.getCameraPositionGlobal();

        gAgentCamera.setCameraPosAndFocusGlobal( cam_pos, pick_info.mPosGlobal, pick_info.mObjectID);
    }
}


// "Let go" of the mouse, for example on mouse up or when
// we lose mouse capture.  This ensures that cursor becomes visible
// if a modal dialog pops up during Alt-Zoom. JC
void LLToolCamera::releaseMouse()
{
    // Need to tell the sim that the mouse button is up, since this
    // tool is no longer working and cursor is visible (despite actual
    // mouse button status).
    LLTool::handleMouseUp(mMouseUpX, mMouseUpY, mMouseUpMask);

    gViewerWindow->showCursor();

    //for the situation when left click was performed on the Agent
    if (!LLFloaterCamera::inFreeCameraMode())
    {
        LLToolMgr::getInstance()->clearTransientTool();
    }

    mMouseSteering = false;
    mMouseWalking = false;
    mValidClickPoint = false;
    mOutsideSlopX = false;
    mOutsideSlopY = false;
}


bool LLToolCamera::handleMouseUp(S32 x, S32 y, MASK mask)
{
    // Claim that we're mousing up somewhere
    mMouseUpX = x;
    mMouseUpY = y;
    mMouseUpMask = mask;

    if (hasMouseCapture())
    {
        // Do not move camera if we haven't gotten a pick
        if (!mClickPickPending)
        {
            if (mValidClickPoint)
            {
                if (CAMERA_MODE_CUSTOMIZE_AVATAR == gAgentCamera.getCameraMode())
                {
                    LLCoordGL mouse_pos;
                    LLVector3 focus_pos = gAgent.getPosAgentFromGlobal(gAgentCamera.getFocusGlobal());
                    bool success = LLViewerCamera::getInstance()->projectPosAgentToScreen(focus_pos, mouse_pos);
                    if (success)
                    {
                        LLUI::getInstance()->setMousePositionScreen(mouse_pos.mX, mouse_pos.mY);
                    }
                }
                else if (mMouseSteering)
                {
                    LLUI::getInstance()->setMousePositionScreen(mMouseDownX, mMouseDownY);
                }
                else
                {
                    gViewerWindow->moveCursorToCenter();
                }
            }
            else
            {
                // not a valid zoomable object
                LLUI::getInstance()->setMousePositionScreen(mMouseDownX, mMouseDownY);
            }
        }

        // calls releaseMouse() internally
        setMouseCapture(false);
    }
    else
    {
        releaseMouse();
    }

    return true;
}


bool LLToolCamera::handleHover(S32 x, S32 y, MASK mask)
{
    S32 dx = gViewerWindow->getCurrentMouseDX();
    S32 dy = gViewerWindow->getCurrentMouseDY();

    if (hasMouseCapture() && mValidClickPoint)
    {
        mAccumX += llabs(dx);
        mAccumY += llabs(dy);

        if (mAccumX >= SLOP_RANGE)
        {
            mOutsideSlopX = true;
        }

        if (mAccumY >= SLOP_RANGE)
        {
            mOutsideSlopY = true;
        }
    }

    if (mOutsideSlopX || mOutsideSlopY)
    {
        if (!mValidClickPoint)
        {
            LL_DEBUGS("UserInput") << "hover handled by LLToolFocus [invalid point]" << LL_ENDL;
            gViewerWindow->setCursor(UI_CURSOR_NO);
            gViewerWindow->showCursor();
            return true;
        }

        if (gCameraBtnOrbit ||
            mask == MASK_ORBIT ||
            mask == (MASK_ALT | MASK_ORBIT))
        {
            // Orbit tool
            if (hasMouseCapture())
            {
                const F32 RADIANS_PER_PIXEL = 360.f * DEG_TO_RAD / gViewerWindow->getWorldViewWidthScaled();

                if (dx != 0)
                {
                    gAgentCamera.cameraOrbitAround( -dx * RADIANS_PER_PIXEL );
                }

                if (dy != 0)
                {
                    gAgentCamera.cameraOrbitOver( -dy * RADIANS_PER_PIXEL );
                }

                gViewerWindow->moveCursorToCenter();
            }
            LL_DEBUGS("UserInput") << "hover handled by LLToolFocus [active]" << LL_ENDL;
        }
        else if (   gCameraBtnPan ||
                    mask == MASK_PAN ||
                    mask == (MASK_PAN | MASK_ALT) )
        {
            // Pan tool
            if (hasMouseCapture())
            {
                LLVector3d camera_to_focus = gAgentCamera.getCameraPositionGlobal();
                camera_to_focus -= gAgentCamera.getFocusGlobal();
                F32 dist = (F32) camera_to_focus.normVec();

                // Fudge factor for pan
                F32 meters_per_pixel = 3.f * dist / gViewerWindow->getWorldViewWidthScaled();

                if (dx != 0)
                {
                    gAgentCamera.cameraPanLeft( dx * meters_per_pixel );
                }

                if (dy != 0)
                {
                    gAgentCamera.cameraPanUp( -dy * meters_per_pixel );
                }

                gViewerWindow->moveCursorToCenter();
            }
            LL_DEBUGS("UserInput") << "hover handled by LLToolPan" << LL_ENDL;
        }
        else if (gCameraBtnZoom)
        {
            // Zoom tool
            if (hasMouseCapture())
            {

                const F32 RADIANS_PER_PIXEL = 360.f * DEG_TO_RAD / gViewerWindow->getWorldViewWidthScaled();

                if (dx != 0)
                {
                    gAgentCamera.cameraOrbitAround( -dx * RADIANS_PER_PIXEL );
                }

                const F32 IN_FACTOR = 0.99f;

                if (dy != 0 && mOutsideSlopY )
                {
                    if (mMouseSteering)
                    {
                        gAgentCamera.cameraOrbitOver( -dy * RADIANS_PER_PIXEL );
                    }
                    else
                    {
                        gAgentCamera.cameraZoomIn((F32)pow( IN_FACTOR, dy ) );
                    }
                }

                gViewerWindow->moveCursorToCenter();
            }

            LL_DEBUGS("UserInput") << "hover handled by LLToolZoom" << LL_ENDL;
        }
    }

    if (gCameraBtnOrbit ||
        mask == MASK_ORBIT ||
        mask == (MASK_ALT | MASK_ORBIT))
    {
        gViewerWindow->setCursor(UI_CURSOR_TOOLCAMERA);
    }
    else if (   gCameraBtnPan ||
                mask == MASK_PAN ||
                mask == (MASK_PAN | MASK_ALT) )
    {
        gViewerWindow->setCursor(UI_CURSOR_TOOLPAN);
    }
    else
    {
        gViewerWindow->setCursor(UI_CURSOR_TOOLZOOMIN);
    }

    return true;
}


void LLToolCamera::onMouseCaptureLost()
{
    releaseMouse();
}

// Mouse walk (#254): both buttons held = walk forward, the classic MMO control.
// Only while the left button steers your own avatar (click-and-hold on self,
// which hands the mouse to this tool with mMouseSteering set), in the normal
// third-person camera; the TPS camera has its own scheme.
bool LLToolCamera::canMouseWalk()
{
    static LLCachedControl<bool> mouse_walk(gSavedSettings, "SlipstreamMouseWalk", true);
    return mouse_walk
        && mMouseSteering
        && hasMouseCapture()
        && gViewerWindow->getLeftMouseDown()
        && gViewerWindow->getActive()
        && gAgentCamera.cameraThirdPerson()
        && !SLTPSCamera::isEngagedNow()
        && !LLFloaterCamera::inFreeCameraMode();
}

bool LLToolCamera::handleRightMouseDown(S32 x, S32 y, MASK mask)
{
    if (canMouseWalk())
    {
        // Consumed: returning true keeps LLViewerWindow from opening the
        // context menu and keeps the button out of the mouse bindings.
        mMouseWalking = true;
        mMouseWalkTimer.reset();
        return true;
    }
    return LLTool::handleRightMouseDown(x, y, mask);
}

bool LLToolCamera::handleRightMouseUp(S32 x, S32 y, MASK mask)
{
    if (mMouseWalking)
    {
        mMouseWalking = false;
        return true;
    }
    return LLTool::handleRightMouseUp(x, y, mask);
}

void LLToolCamera::updateMouseWalk()
{
    if (!mMouseWalking)
    {
        return;
    }
    // Stop as soon as either button, the steering or the window focus goes
    // (a lost button-up must never leave the avatar walking on its own).
    if (!canMouseWalk() || !gViewerWindow->getRightMouseDown())
    {
        mMouseWalking = false;
        return;
    }
    if (gAgent.isMovementLocked())
    {
        return;
    }

    // As the forward key does: a short press nudges, a held one walks (or runs
    // / flies, per the agent's current state). moveAt() resets the view the
    // same way the key does; resetView() keeps mouse-steer mode while the left
    // button is down.
    const F32 MOUSE_WALK_NUDGE_TIME = 0.25f;
    if (mMouseWalkTimer.getElapsedTimeF32() < MOUSE_WALK_NUDGE_TIME)
    {
        gAgent.moveAtNudge(1);
    }
    else
    {
        gAgent.moveAt(1);
        // resetView() shows the cursor; steering keeps it hidden (the pointer
        // is warped to the centre every hover).
        gViewerWindow->hideCursor();
    }
}
