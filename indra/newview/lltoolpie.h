/**
 * @file lltoolpie.h
 * @brief LLToolPie class header file
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

#ifndef LL_TOOLPIE_H
#define LL_TOOLPIE_H

#include "lltool.h"
#include "lluuid.h"
#include "lltimer.h"
#include "llviewerwindow.h" // for LLPickInfo
#include "llhudeffectblob.h" // for LLPointer<LLHudEffectBlob>, apparently

class LLViewerObject;
class LLObjectSelection;

class LLToolPie : public LLTool, public LLSingleton<LLToolPie>
{
    LLSINGLETON(LLToolPie);
    LOG_CLASS(LLToolPie);
public:

    // Virtual functions inherited from LLMouseHandler
    virtual bool        handleAnyMouseClick(S32 x, S32 y, MASK mask, EMouseClickType clicktype, bool down) override;
    virtual bool        handleMouseDown(S32 x, S32 y, MASK mask) override;
    virtual bool        handleRightMouseDown(S32 x, S32 y, MASK mask) override;
    virtual bool        handleMouseUp(S32 x, S32 y, MASK mask) override;
    virtual bool        handleRightMouseUp(S32 x, S32 y, MASK mask) override;
    virtual bool        handleHover(S32 x, S32 y, MASK mask) override;
    virtual bool        handleDoubleClick(S32 x, S32 y, MASK mask) override;
    bool                handleScrollWheelAny(S32 x, S32 y, S32 clicks_x, S32 clicks_y);
    virtual bool        handleScrollWheel(S32 x, S32 y, S32 clicks) override;
    virtual bool        handleScrollHWheel(S32 x, S32 y, S32 clicks) override;
    virtual bool        handleToolTip(S32 x, S32 y, MASK mask) override;

    virtual void        render() override;

    virtual void        stopEditing() override;

    virtual void        onMouseCaptureLost() override;
    virtual void        handleSelect() override;
    virtual void        handleDeselect() override;
    virtual LLTool*     getOverrideTool(MASK mask) override;

    // Right button held with the right-drag orbit armed (no travel yet, or dragging).
    bool                isRightHeld() const { return mRightOrbitArmed; }
    // Right button held and the camera follows the avatar: A/D strafe instead of turning.
    bool                rightButtonSteering() const;
    // Held but not travelled yet: an Alt click may still take over (cancelRightPress).
    bool                rightPressPending() const { return mRightOrbitArmed && !mRightOrbiting; }
    void                cancelRightPress();
    // Context menu for what is under (x, y), as a right click there opens it.
    void                showContextMenuAt(S32 x, S32 y, MASK mask);

    LLPickInfo&         getPick() { return mPick; }
// [RLVa:KB] - Checked: 2010-03-06 (RLVa-1.2.0c) | Added: RLVa-1.2.0a
    LLPickInfo&         getHoverPick() { return mHoverPick; }
// [/RLVa:KB]
    U8                  getClickAction() { return mClickAction; }
    LLViewerObject*     getClickActionObject() { return mClickActionObject; }
    LLObjectSelection*  getLeftClickSelection() { return (LLObjectSelection*)mLeftClickSelection; }
    void                resetSelection();
    bool                walkToClickedLocation();
    bool                teleportToClickedLocation();
    void                stopClickToWalk();

    static void         selectionPropertiesReceived();

    static void         showAvatarInspector(const LLUUID& avatar_id);
    static void         showObjectInspector(const LLUUID& object_id);
    static void         showObjectInspector(const LLUUID& object_id, const S32& object_face);
    static void         playCurrentMedia(const LLPickInfo& info);
    static void         VisitHomePage(const LLPickInfo& info);

private:
    bool outsideSlop        (S32 x, S32 y, S32 start_x, S32 start_y);
    bool handleLeftClickPick();
    bool handleRightClickPick();
    bool useClickAction     (MASK mask, LLViewerObject* object,LLViewerObject* parent);

    void showVisualContextMenuEffect();
    ECursorType cursorFromObject(LLViewerObject* object);

    enum MediaFirstClickTypes
    {
        MEDIA_FIRST_CLICK_NONE       = 0,         // Special case: Feature is disabled
        MEDIA_FIRST_CLICK_HUD        = 1 << 0,    // 0b00000001 (1)
        MEDIA_FIRST_CLICK_OWN        = 1 << 1,    // 0b00000010 (2)
        MEDIA_FIRST_CLICK_FRIEND     = 1 << 2,    // 0b00000100 (4)
        MEDIA_FIRST_CLICK_GROUP      = 1 << 3,    // 0b00001000 (8)
        MEDIA_FIRST_CLICK_LAND       = 1 << 4,    // 0b00010000 (16)

        // Covers any object with PRIM_MEDIA_FIRST_CLICK_INTERACT (combines all previous flags)
        MEDIA_FIRST_CLICK_ANY        = (1 << 15) - 1, // 0b0111111111111111 (32767)

        // Covers all media regardless of other rules or PRIM_MEDIA_FIRST_CLICK_INTERACT
        MEDIA_FIRST_CLICK_BYPASS_MOAP_FLAG = 1 << 15  // 0b10000000000000000 (32768)
    };
    bool shouldAllowFirstMediaInteraction(const LLPickInfo& info, bool moap_flag);
    bool handleMediaClick(const LLPickInfo& info);
    bool handleMediaDblClick(const LLPickInfo& info);
    bool handleMediaHover(const LLPickInfo& info);
    bool handleMediaMouseUp();
    bool handleTooltipLand(std::string line, std::string tooltip_msg);
    bool handleTooltipObject( LLViewerObject* hover_object, std::string line, std::string tooltip_msg);

    void steerCameraWithMouse(S32 x, S32 y);
    void startCameraSteering();
    void stopCameraSteering();
    bool inCameraSteerMode();

private:
    // Right-click-drag orbit (RightClickDragOrbit): the right button is captured on
    // press, which changes nothing. Once the mouse has moved more than
    // RightClickOrbitSlopPx from the press point the camera drags around the CURRENT
    // focus (see applyRightDrag); released before that, the usual context menu opens
    // at the press point.
    bool                mRightOrbitArmed = false;
    bool                mRightOrbiting = false;
    S32                 mRightDownX = 0;
    S32                 mRightDownY = 0;
    void                startRightOrbit();
    void                applyRightDrag(S32 dx, S32 dy, MASK mask);
    void                endRightOrbit();

    bool                mMouseButtonDown;
    bool                mMouseOutsideSlop;      // for this drag, has mouse moved outside slop region
    S32                 mMouseDownX;
    S32                 mMouseDownY;
    S32                 mMouseSteerX;
    S32                 mMouseSteerY;
    LLPointer<LLHUDEffectBlob>  mAutoPilotDestination;
    LLPointer<LLHUDEffectBlob>  mMouseSteerGrabPoint;
    bool                mClockwise;
    LLUUID              mMediaMouseCaptureID;
    LLPickInfo          mPick;
    LLPickInfo          mHoverPick;
    // state of the last hover pick, so a still mouse under a still camera re-picks only on a timer
    S32                 mHoverPickX = -1;
    S32                 mHoverPickY = -1;
    MASK                mHoverPickMask = 0;
    U32                 mHoverPickSerial = 0;
    F32                 mHoverPickCamera[11] = {};
    LLTimer             mHoverPickTimer;
    LLPickInfo          mSteerPick;
    LLPointer<LLViewerObject> mClickActionObject;
    U8                  mClickAction;
    LLSafeHandle<LLObjectSelection> mLeftClickSelection;
    bool                mClickActionBuyEnabled;
    bool                mClickActionPayEnabled;
    LLFrameTimer mDoubleClickTimer;
};

// Right-click health (always on; lltoolpie.cpp): one rate-limited "right-click health:" log
// line when right-clicks keep producing no menu, naming the layer that failed (the press
// never reached the world / the pick found nothing / an object was picked but no menu
// opened or was already open), and one when menus open again.
namespace LLRightClickHealth
{
    void pressBegin();                                          // a right press enters handleAnyMouseClick
    void route(const char* what, const std::string& detail);    // who took (or declined) it
    void pressEnd();                                            // that press is over
}

#endif
