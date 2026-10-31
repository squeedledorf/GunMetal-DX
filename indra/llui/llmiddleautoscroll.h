/**
 * @file llmiddleautoscroll.h
 * @brief Middle-button autoscroll for scrolling views (#1144)
 *
 *   middle click (press and release   toggles the mode: the view scrolls toward the mouse,
 *   without moving)                   faster the further it is from the anchor (a small dead
 *                                     zone around it); any click, Esc, the wheel or the mouse
 *                                     leaving the view ends it
 *   middle drag                       scrolls the same way, only while the button is held
 *
 * Started by LLScrollContainer (folder views, panels, and text editors / chat logs through
 * their scroller), LLScrollListCtrl and LLAccordionCtrl when a middle press reaches them and no
 * child used it. While on it is the mouse captor, so moves and clicks reach it wherever the
 * mouse is; LLViewerWindow ticks it once per frame, draws the anchor marker and gives it Esc.
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

#ifndef LL_LLMIDDLEAUTOSCROLL_H
#define LL_LLMIDDLEAUTOSCROLL_H

#include "llhandle.h"
#include "llmousehandler.h"
#include "llsingleton.h"

class LLScrollbar;
class LLView;

class LLMiddleAutoScroll final : public LLMouseHandler, public LLSingleton<LLMiddleAutoScroll>
{
    LLSINGLETON_EMPTY_CTOR(LLMiddleAutoScroll);
    ~LLMiddleAutoScroll();

public:
    // A middle press at (x, y), local to `owner`, on a view scrolled by these bars (either may
    // be null). px_per_unit: screen pixels per scrollbar unit (1 for pixel-scrolled views, the
    // row height for a list). True if it started: some bar can scroll.
    bool begin(LLView* owner, LLScrollbar* vertical, LLScrollbar* horizontal, F32 px_per_unit, S32 x, S32 y);
    void stop();

    // On (pressed, scrolling or ending). Safe before the singleton exists.
    static bool isActive();

    // Once per frame (LLViewerWindow::updateUI): scroll by the mouse's distance from the anchor.
    static void update(F32 dt);
    // The anchor marker, over the whole UI (LLViewerWindow::draw).
    static void drawAnchor();
    // Esc ends the mode. True if it did (the key goes no further).
    static bool handleEscape();

    // LLMouseHandler: the mouse captor while on; coordinates are screen coordinates.
    bool handleAnyMouseClick(S32 x, S32 y, MASK mask, EMouseClickType clicktype, bool down) override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override        { return handleAnyMouseClick(x, y, mask, CLICK_LEFT, true); }
    bool handleMouseUp(S32 x, S32 y, MASK mask) override          { return handleAnyMouseClick(x, y, mask, CLICK_LEFT, false); }
    bool handleMiddleMouseDown(S32 x, S32 y, MASK mask) override  { return handleAnyMouseClick(x, y, mask, CLICK_MIDDLE, true); }
    bool handleMiddleMouseUp(S32 x, S32 y, MASK mask) override    { return handleAnyMouseClick(x, y, mask, CLICK_MIDDLE, false); }
    bool handleRightMouseDown(S32 x, S32 y, MASK mask) override   { return handleAnyMouseClick(x, y, mask, CLICK_RIGHT, true); }
    bool handleRightMouseUp(S32 x, S32 y, MASK mask) override     { return handleAnyMouseClick(x, y, mask, CLICK_RIGHT, false); }
    bool handleDoubleClick(S32 x, S32 y, MASK mask) override      { return handleAnyMouseClick(x, y, mask, CLICK_DOUBLELEFT, true); }
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleScrollWheel(S32 x, S32 y, S32 clicks) override;
    bool handleScrollHWheel(S32 x, S32 y, S32 clicks) override;
    bool handleToolTip(S32 x, S32 y, MASK mask) override          { return true; }
    const std::string& getName() const override;
    void onMouseCaptureLost() override;
    void screenPointToLocal(S32 screen_x, S32 screen_y, S32* local_x, S32* local_y) const override;
    void localPointToScreen(S32 local_x, S32 local_y, S32* screen_x, S32* screen_y) const override;
    bool hasMouseCapture() override;

private:
    enum EState
    {
        IDLE,
        PRESSED,    // middle button down, not moved past the drag threshold yet
        DRAGGING,   // middle button held and moved: scrolls until it is released
        LATCHED,    // toggled on by a middle click: scrolls until a click / Esc / wheel / leave
        ENDING,     // a click ended LATCHED: keeps the mouse until that button's release
    };
    enum { V, H, AXES };

    void reset();
    void tick(F32 dt);
    void scrollAxis(S32 axis, S32 distance, F32 dt);
    bool canScroll(S32 axis) const;

    EState              mState = IDLE;
    LLHandle<LLView>    mOwner;
    LLHandle<LLScrollbar> mBar[AXES];
    bool                mAxisOn[AXES] = { false, false };   // could scroll when it started
    F32                 mRemainder[AXES] = { 0.f, 0.f };    // fraction of a unit / step not yet applied
    F32                 mPxPerUnit = 1.f;
    S32                 mAnchorX = 0;
    S32                 mAnchorY = 0;
    EMouseClickType     mEndButton = CLICK_NONE;            // ENDING: the release to wait for
};

#endif // LL_LLMIDDLEAUTOSCROLL_H
