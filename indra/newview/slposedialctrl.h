/**
 * @file slposedialctrl.h
 * @brief Pose Studio (#500): half-ring rotation dial with a pointer,
 *        registered as @c sl_pose_dial.
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

#ifndef SL_POSEDIALCTRL_H
#define SL_POSEDIALCTRL_H

#include "lluictrl.h"
#include "lluicolor.h"

#include <boost/signals2.hpp>

/**
 * The value (degrees) runs left to right over the upper half ring, min_value at the
 * left end, max_value at the right, zero marked with a tick. Drag anywhere on the
 * dial to aim the pointer; the commit signal fires continuously while dragging
 * (local preview) and the release signal once when the button comes up. Clicking the
 * centre hub, or clicking the pointer's knob without dragging it, resets to zero (as
 * on the OP Pose HUD). Scroll wheel nudges by one snap step (1 degree when snapping is
 * off, x10 with shift); double-click also returns to zero.
 */
class SLPoseDialCtrl : public LLUICtrl
{
public:
    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params>
    {
        Optional<F32>         min_value;
        Optional<F32>         max_value;
        Optional<LLUIColor>   ring_color;
        Optional<LLUIColor>   track_color;
        Optional<LLUIColor>   pointer_color;
        Optional<LLUIColor>   text_color;
        Optional<S32>         ring_thickness;

        Params();
    };

    SLPoseDialCtrl(const Params& p);
    ~SLPoseDialCtrl() override = default;

    void setValue(const LLSD& value) override;
    LLSD getValue() const override { return LLSD(mValue); }
    F32 getValueF32() const { return mValue; }

    void setSnap(F32 step_degrees) { mSnap = llmax(0.f, step_degrees); }
    // Changes the span (per bone); the value is clamped into it.
    void setRange(F32 min_value, F32 max_value);
    bool isDragging() const { return mDragging; }

    typedef boost::signals2::signal<void(SLPoseDialCtrl*)> release_signal_t;
    boost::signals2::connection setReleaseCallback(const release_signal_t::slot_type& cb) { return mReleaseSignal.connect(cb); }

    void draw() override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleDoubleClick(S32 x, S32 y, MASK mask) override;
    bool handleScrollWheel(S32 x, S32 y, S32 clicks) override;

private:
    void geometry(F32& cx, F32& cy, F32& radius) const;
    F32 valueToAngle(F32 value) const;
    F32 valueFromPoint(S32 x, S32 y) const;
    F32 applySnap(F32 value) const;
    void setFromUser(F32 value);
    void resetToZero();

    F32 mMin;
    F32 mMax;
    F32 mValue = 0.f;
    F32 mSnap = 0.f;
    bool mDragging = false;
    bool mPressedOnKnob = false;    // a click on the knob without a drag resets to zero
    bool mMoved = false;
    S32 mPressX = 0;
    S32 mPressY = 0;
    std::string mLabel;
    S32 mRingThickness;
    LLUIColor mRingColor;
    LLUIColor mTrackColor;
    LLUIColor mPointerColor;
    LLUIColor mTextColor;
    release_signal_t mReleaseSignal;
};

#endif // SL_POSEDIALCTRL_H
