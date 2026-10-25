/**
 * @file slposedialctrl.cpp
 * @brief Pose Studio (#500): half-ring rotation dial with a pointer.
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

#include "slposedialctrl.h"

#include "llfocusmgr.h"
#include "llfontgl.h"
#include "llkeyboard.h"
#include "llrender.h"
#include "llrender2dutils.h"
#include "llwindow.h"
#include "v2math.h"

#include <cmath>

static LLDefaultChildRegistry::Register<SLPoseDialCtrl> r("sl_pose_dial");

namespace
{
constexpr S32 DIAL_STEPS = 48;
constexpr S32 LABEL_SPACE = 14;     // pixels below the ring's centre line for label + value
}

SLPoseDialCtrl::Params::Params()
:   min_value("min_value", -180.f),
    max_value("max_value", 180.f),
    ring_color("ring_color", LLColor4(1.f, 0.3f, 0.3f, 1.f)),
    track_color("track_color", LLColor4(0.25f, 0.25f, 0.28f, 1.f)),
    pointer_color("pointer_color", LLColor4(1.f, 1.f, 1.f, 1.f)),
    text_color("text_color", LLColor4(0.85f, 0.85f, 0.85f, 1.f)),
    ring_thickness("ring_thickness", 7)
{
    changeDefault(mouse_opaque, true);
}

SLPoseDialCtrl::SLPoseDialCtrl(const Params& p)
:   LLUICtrl(p),
    mMin(p.min_value),
    mMax(llmax(p.min_value() + 1.f, p.max_value())),
    mLabel(p.label()),
    mRingThickness(llmax(2, p.ring_thickness())),
    mRingColor(p.ring_color),
    mTrackColor(p.track_color),
    mPointerColor(p.pointer_color),
    mTextColor(p.text_color)
{
    mValue = llclamp(0.f, mMin, mMax);
}

void SLPoseDialCtrl::setValue(const LLSD& value)
{
    mValue = llclamp((F32)value.asReal(), mMin, mMax);
}

void SLPoseDialCtrl::setRange(F32 min_value, F32 max_value)
{
    mMin = min_value;
    mMax = llmax(min_value + 1.f, max_value);
    mValue = llclamp(mValue, mMin, mMax);
}

void SLPoseDialCtrl::resetToZero()
{
    const F32 zero = llclamp(0.f, mMin, mMax);
    if (zero != mValue)
    {
        mValue = zero;
        onCommit();
    }
    mReleaseSignal(this);
}

void SLPoseDialCtrl::geometry(F32& cx, F32& cy, F32& radius) const
{
    const F32 w = (F32)getRect().getWidth();
    const F32 h = (F32)getRect().getHeight();
    cx = w * 0.5f;
    cy = (F32)LABEL_SPACE + 2.f;
    radius = llmax(4.f, llmin(w * 0.5f - 3.f, h - cy - 3.f));
}

F32 SLPoseDialCtrl::valueToAngle(F32 value) const
{
    // min at the left end (pi), max at the right end (0).
    return F_PI * (1.f - (value - mMin) / (mMax - mMin));
}

F32 SLPoseDialCtrl::valueFromPoint(S32 x, S32 y) const
{
    F32 cx, cy, radius;
    geometry(cx, cy, radius);
    F32 angle = atan2f((F32)y - cy, (F32)x - cx);
    if (angle < 0.f)
    {
        // Below the centre line: pin to whichever end is nearer.
        angle = (angle < -F_PI_BY_TWO) ? F_PI : 0.f;
    }
    return mMin + (1.f - angle / F_PI) * (mMax - mMin);
}

F32 SLPoseDialCtrl::applySnap(F32 value) const
{
    if (mSnap > 0.f)
        value = mSnap * ll_round(value / mSnap);
    return llclamp(value, mMin, mMax);
}

void SLPoseDialCtrl::setFromUser(F32 value)
{
    value = applySnap(value);
    if (value == mValue)
        return;
    mValue = value;
    onCommit();
}

void SLPoseDialCtrl::draw()
{
    F32 cx, cy, radius;
    geometry(cx, cy, radius);
    const F32 inner = llmax(1.f, radius - (F32)mRingThickness);
    const F32 alpha = getEnabled() ? 1.f : 0.4f;

    LLColor4 track = mTrackColor.get();
    LLColor4 ring = mRingColor.get();
    LLColor4 pointer = mPointerColor.get();
    LLColor4 text = mTextColor.get();
    track.mV[VALPHA] *= alpha;
    ring.mV[VALPHA] *= alpha;
    pointer.mV[VALPHA] *= alpha;
    text.mV[VALPHA] *= alpha;

    const F32 zero_angle = valueToAngle(llclamp(0.f, mMin, mMax));
    const F32 value_angle = valueToAngle(mValue);

    // Washers draw around the current origin: translateUI paired with push/popUIMatrix
    // (see SLColorWheelCtrl::draw for why it must not be translatef).
    gGL.pushUIMatrix();
    {
        gGL.translateUI(cx, cy, 0.f);
        gl_washer_segment_2d(radius, inner, 0.f, F_PI, DIAL_STEPS, track, track);
        const F32 a0 = llmin(zero_angle, value_angle);
        const F32 a1 = llmax(zero_angle, value_angle);
        if (a1 - a0 > 0.001f)
        {
            const S32 steps = llmax(2, (S32)(DIAL_STEPS * (a1 - a0) / F_PI));
            gl_washer_segment_2d(radius, inner, a0, a1, steps, ring, ring);
        }
    }
    gGL.popUIMatrix();

    // Zero tick and the pointer.
    const F32 tick_in = inner - 3.f;
    gl_line_2d(ll_round(cx + cosf(zero_angle) * tick_in), ll_round(cy + sinf(zero_angle) * tick_in),
               ll_round(cx + cosf(zero_angle) * (radius + 2.f)), ll_round(cy + sinf(zero_angle) * (radius + 2.f)), text);

    const F32 px = cx + cosf(value_angle) * radius;
    const F32 py = cy + sinf(value_angle) * radius;
    gl_line_2d(ll_round(cx), ll_round(cy), ll_round(px), ll_round(py), pointer);
    gGL.color4fv(ring.mV);
    gl_circle_2d(px, py, mDragging ? 5.f : 4.f, 12, true);
    gGL.color4fv(pointer.mV);
    gl_circle_2d(px, py, mDragging ? 5.f : 4.f, 12, false);
    gl_circle_2d(cx, cy, 2.f, 8, true);

    // Label and value under the centre line.
    const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
    const std::string caption = mLabel.empty() ? llformat("%.1f", mValue) : llformat("%s  %.1f", mLabel.c_str(), mValue);
    font->renderUTF8(caption, 0, ll_round(cx), 1, ring, LLFontGL::HCENTER, LLFontGL::BOTTOM);

    LLUICtrl::draw();
}

bool SLPoseDialCtrl::handleMouseDown(S32 x, S32 y, MASK mask)
{
    if (!getEnabled())
        return LLUICtrl::handleMouseDown(x, y, mask);

    F32 cx, cy, radius;
    geometry(cx, cy, radius);
    const F32 hub = llmax(6.f, (radius - (F32)mRingThickness) * 0.3f);
    if (dist_vec_squared(LLVector2((F32)x, (F32)y), LLVector2(cx, cy)) <= hub * hub)
    {
        // The hub resets this axis.
        resetToZero();
        return true;
    }

    const F32 angle = valueToAngle(mValue);
    const LLVector2 knob(cx + cosf(angle) * radius, cy + sinf(angle) * radius);
    mPressedOnKnob = dist_vec_squared(LLVector2((F32)x, (F32)y), knob) <= 49.f;
    mMoved = false;
    mPressX = x;
    mPressY = y;

    gFocusMgr.setMouseCapture(this);
    mDragging = true;
    if (!mPressedOnKnob)
        setFromUser(valueFromPoint(x, y));
    return true;
}

bool SLPoseDialCtrl::handleHover(S32 x, S32 y, MASK mask)
{
    if (mDragging && hasMouseCapture())
    {
        if (!mMoved && llabs(x - mPressX) + llabs(y - mPressY) < 3)
            return true;
        mMoved = true;
        setFromUser(valueFromPoint(x, y));
        getWindow()->setCursor(UI_CURSOR_ARROW);
        return true;
    }
    return LLUICtrl::handleHover(x, y, mask);
}

bool SLPoseDialCtrl::handleMouseUp(S32 x, S32 y, MASK mask)
{
    if (mDragging)
    {
        mDragging = false;
        if (hasMouseCapture())
            gFocusMgr.setMouseCapture(nullptr);
        if (mPressedOnKnob && !mMoved)
            resetToZero();      // a click on the indicator, as on the HUD
        else
            mReleaseSignal(this);
        mPressedOnKnob = false;
        return true;
    }
    return LLUICtrl::handleMouseUp(x, y, mask);
}

bool SLPoseDialCtrl::handleDoubleClick(S32 x, S32 y, MASK mask)
{
    if (!getEnabled())
        return false;
    resetToZero();
    return true;
}

bool SLPoseDialCtrl::handleScrollWheel(S32 x, S32 y, S32 clicks)
{
    if (!getEnabled())
        return false;
    F32 step = (mSnap > 0.f) ? mSnap : 1.f;
    if (gKeyboard && (gKeyboard->currentMask(false) & MASK_SHIFT))
        step *= 10.f;
    // Wheel up turns the pointer clockwise, i.e. toward max.
    const F32 old_snap = mSnap;
    mSnap = 0.f;
    setFromUser(mValue - (F32)clicks * step);
    mSnap = old_snap;
    mReleaseSignal(this);
    return true;
}
