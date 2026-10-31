/**
 * @file llmiddleautoscroll.cpp
 * @brief Middle-button autoscroll for scrolling views (#1144)
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

#include "linden_common.h"

#include "llmiddleautoscroll.h"

#include "llfocusmgr.h"
#include "llrender.h"
#include "llrender2dutils.h"
#include "llscrollbar.h"
#include "llui.h"
#include "llview.h"
#include "llwindow.h"

namespace
{
    // Screen distance (px) a held press may move and still be a click (the mode toggles on).
    constexpr S32 kDragPx = 6;
    // No scrolling this close to the anchor (px, per axis); the anchor marker's ring shows it.
    constexpr F32 kDeadZonePx = 12.f;
    // Speed (px/s) at `reach` px past the dead zone: linear * reach + quad * reach^2. A short
    // reach reads comfortably (a few lines a second), a long one crosses a big inventory in seconds.
    constexpr F32 kSpeedLinear = 5.f;
    constexpr F32 kSpeedQuad = 0.06f;
    constexpr F32 kSpeedMax = 6000.f;
    // Longest frame the scroll integrates over (s): a hitch doesn't jump the view.
    constexpr F32 kMaxFrameTime = 0.1f;
}

LLMiddleAutoScroll::~LLMiddleAutoScroll()
{
    // Never leave the focus manager pointing at a dead captor.
    gFocusMgr.removeMouseCaptureWithoutCallback(this);
}

bool LLMiddleAutoScroll::begin(LLView* owner, LLScrollbar* vertical, LLScrollbar* horizontal, F32 px_per_unit, S32 x, S32 y)
{
    // Another captor (a drag in progress) keeps the mouse.
    if (!owner || mState != IDLE || gFocusMgr.getMouseCapture())
    {
        return false;
    }

    mBar[V] = vertical ? vertical->getDerivedHandle<LLScrollbar>() : LLHandle<LLScrollbar>();
    mBar[H] = horizontal ? horizontal->getDerivedHandle<LLScrollbar>() : LLHandle<LLScrollbar>();
    mAxisOn[V] = canScroll(V);
    mAxisOn[H] = canScroll(H);
    if (!mAxisOn[V] && !mAxisOn[H])
    {
        reset();
        return false;
    }

    mOwner = owner->getHandle();
    owner->localPointToScreen(x, y, &mAnchorX, &mAnchorY);
    mPxPerUnit = llmax(px_per_unit, 1.f);
    mRemainder[V] = mRemainder[H] = 0.f;
    mState = PRESSED;
    gFocusMgr.setMouseCapture(this);   // hover, clicks and the wheel reach us wherever the mouse is
    return true;
}

void LLMiddleAutoScroll::stop()
{
    reset();
    if (hasMouseCapture())
    {
        gFocusMgr.setMouseCapture(nullptr);   // onMouseCaptureLost() finds the state already idle
    }
}

void LLMiddleAutoScroll::reset()
{
    mState = IDLE;
    mOwner = LLHandle<LLView>();
    mBar[V] = mBar[H] = LLHandle<LLScrollbar>();
    mAxisOn[V] = mAxisOn[H] = false;
    mEndButton = CLICK_NONE;
}

// static
bool LLMiddleAutoScroll::isActive()
{
    return instanceExists() && getInstance()->mState != IDLE;
}

bool LLMiddleAutoScroll::canScroll(S32 axis) const
{
    const LLScrollbar* bar = mBar[axis].get();
    return bar && bar->getVisible() && bar->getEnabled() && bar->getDocPosMax() > 0;
}

// static
void LLMiddleAutoScroll::update(F32 dt)
{
    if (isActive())
    {
        getInstance()->tick(dt);
    }
}

void LLMiddleAutoScroll::tick(F32 dt)
{
    // The view went away or hid (its floater closed), or another captor took the mouse.
    LLView* owner = mOwner.get();
    if (!owner || !owner->isInVisibleChain() || !owner->isInEnabledChain() || !hasMouseCapture())
    {
        stop();
        return;
    }
    if (mState != DRAGGING && mState != LATCHED)
    {
        return;
    }

    S32 x, y;
    LLUI::getInstance()->getMousePositionScreen(&x, &y);
    if (mState == LATCHED && !owner->calcScreenRect().pointInRect(x, y))
    {
        stop();   // the mouse left the view (a held drag keeps going until released)
        return;
    }

    dt = llclamp(dt, 0.f, kMaxFrameTime);
    // Below the anchor scrolls down: screen y grows upward, the document position downward.
    scrollAxis(V, mAnchorY - y, dt);
    scrollAxis(H, x - mAnchorX, dt);
}

void LLMiddleAutoScroll::scrollAxis(S32 axis, S32 distance, F32 dt)
{
    LLScrollbar* bar = mBar[axis].get();
    const F32 reach = llmax((F32)llabs(distance) - kDeadZonePx, 0.f);
    if (!mAxisOn[axis] || !bar || !bar->getVisible() || reach <= 0.f)
    {
        mRemainder[axis] = 0.f;
        return;
    }

    const F32 speed = llmin(kSpeedLinear * reach + kSpeedQuad * reach * reach, kSpeedMax);
    mRemainder[axis] += (distance > 0 ? speed : -speed) * dt / mPxPerUnit;

    // Smooth scrolling (#727) on: one unit at a time (a pixel; a row in a list). Off: the bar's
    // own step, as the wheel moves it.
    static LLUICachedControl<bool> s_smooth("UISmoothScroll", true);
    const S32 step = s_smooth ? 1 : llmax(bar->getStepSize(), 1);
    const S32 move = (S32)(mRemainder[axis] / (F32)step) * step;
    if (move != 0)
    {
        const S32 before = bar->getDocPos();
        bar->setDocPos(before + move);   // supersedes any easing wheel scroll on the bar
        // At an end stop the backlog is dropped, so turning back responds at once.
        mRemainder[axis] = (bar->getDocPos() == before) ? 0.f : mRemainder[axis] - (F32)move;
    }
}

// static
void LLMiddleAutoScroll::drawAnchor()
{
    if (!isActive())
    {
        return;
    }
    const LLMiddleAutoScroll* self = getInstance();
    if (self->mState == ENDING)
    {
        return;
    }

    static const LLColor4 fill(0.f, 0.f, 0.f, 0.55f);
    static const LLColor4 ring(1.f, 1.f, 1.f, 0.8f);
    static const LLColor4 arrow_idle(1.f, 1.f, 1.f, 0.5f);
    static const LLColor4 arrow_busy(1.f, 1.f, 1.f, 1.f);
    constexpr F32 radius = kDeadZonePx;
    constexpr S32 tip = 9;    // arrow tip, from the centre
    constexpr S32 base = 4;   // arrow base, from the centre
    constexpr S32 half = 3;   // half the arrow's width

    const S32 cx = self->mAnchorX;
    const S32 cy = self->mAnchorY;
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gGL.color4fv(fill.mV);
    gl_circle_2d((F32)cx, (F32)cy, radius, 24, true);
    gGL.color4fv(ring.mV);
    gl_circle_2d((F32)cx, (F32)cy, radius, 24, false);
    gl_circle_2d((F32)cx, (F32)cy, 1.5f, 8, true);

    // An arrow for each way the view can scroll; the way it is going now is brighter.
    S32 mx = cx, my = cy;
    if (self->mState != PRESSED)
    {
        LLUI::getInstance()->getMousePositionScreen(&mx, &my);
    }
    const S32 dead = (S32)kDeadZonePx;
    if (self->mAxisOn[V])
    {
        gl_triangle_2d(cx - half, cy + base, cx + half, cy + base, cx, cy + tip, my > cy + dead ? arrow_busy : arrow_idle, true);
        gl_triangle_2d(cx - half, cy - base, cx, cy - tip, cx + half, cy - base, my < cy - dead ? arrow_busy : arrow_idle, true);
    }
    if (self->mAxisOn[H])
    {
        gl_triangle_2d(cx - base, cy - half, cx - tip, cy, cx - base, cy + half, mx < cx - dead ? arrow_busy : arrow_idle, true);
        gl_triangle_2d(cx + base, cy - half, cx + base, cy + half, cx + tip, cy, mx > cx + dead ? arrow_busy : arrow_idle, true);
    }
}

// static
bool LLMiddleAutoScroll::handleEscape()
{
    if (!isActive())
    {
        return false;
    }
    getInstance()->stop();
    return true;
}

bool LLMiddleAutoScroll::handleAnyMouseClick(S32 x, S32 y, MASK mask, EMouseClickType clicktype, bool down)
{
    if (clicktype == CLICK_DOUBLELEFT)
    {
        clicktype = CLICK_LEFT;
    }
    switch (mState)
    {
    case PRESSED:
    case DRAGGING:
        // The middle release decides: a click turns the mode on, the end of a drag ends it.
        // Other buttons meanwhile go nowhere.
        if (clicktype == CLICK_MIDDLE && !down)
        {
            if (mState == PRESSED)
            {
                mState = LATCHED;
            }
            else
            {
                stop();
            }
        }
        return true;
    case LATCHED:
        // Any click ends the mode and does nothing else; its release is swallowed too.
        if (down)
        {
            mState = ENDING;
            mEndButton = clicktype;
        }
        return true;
    case ENDING:
        if (!down && clicktype == mEndButton)
        {
            stop();
        }
        return true;
    default:
        return false;
    }
}

bool LLMiddleAutoScroll::handleHover(S32 x, S32 y, MASK mask)
{
    if (mState == PRESSED && (llabs(x - mAnchorX) > kDragPx || llabs(y - mAnchorY) > kDragPx))
    {
        mState = DRAGGING;
    }
    if (mState != IDLE)
    {
        ECursorType cursor = UI_CURSOR_ARROW;
        if (mState != ENDING)
        {
            cursor = mAxisOn[V] && mAxisOn[H] ? UI_CURSOR_SIZEALL : (mAxisOn[V] ? UI_CURSOR_SIZENS : UI_CURSOR_SIZEWE);
        }
        LLUI::getInstance()->getWindow()->setCursor(cursor);
    }
    return true;
}

bool LLMiddleAutoScroll::handleScrollWheel(S32 x, S32 y, S32 clicks)
{
    // The wheel ends the mode and scrolls as it would have (screen = root view coordinates).
    stop();
    LLView* root = LLUI::getInstance()->getRootView();
    return root && root->handleScrollWheel(x, y, clicks);
}

bool LLMiddleAutoScroll::handleScrollHWheel(S32 x, S32 y, S32 clicks)
{
    stop();
    LLView* root = LLUI::getInstance()->getRootView();
    return root && root->handleScrollHWheel(x, y, clicks);
}

const std::string& LLMiddleAutoScroll::getName() const
{
    static const std::string name("Middle-button autoscroll");
    return name;
}

void LLMiddleAutoScroll::onMouseCaptureLost()
{
    reset();
}

void LLMiddleAutoScroll::screenPointToLocal(S32 screen_x, S32 screen_y, S32* local_x, S32* local_y) const
{
    *local_x = screen_x;
    *local_y = screen_y;
}

void LLMiddleAutoScroll::localPointToScreen(S32 local_x, S32 local_y, S32* screen_x, S32* screen_y) const
{
    *screen_x = local_x;
    *screen_y = local_y;
}

bool LLMiddleAutoScroll::hasMouseCapture()
{
    return gFocusMgr.getMouseCapture() == this;
}
