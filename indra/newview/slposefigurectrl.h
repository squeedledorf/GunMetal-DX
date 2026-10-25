/**
 * @file slposefigurectrl.h
 * @brief Pose Studio (#500): data-driven bone picker -- a figure with one
 *        clickable dot per bone, registered as @c sl_pose_figure.
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

#ifndef SL_POSEFIGURECTRL_H
#define SL_POSEFIGURECTRL_H

#include "lluictrl.h"
#include "lluicolor.h"
#include "lluiimage.h"
#include "v3math.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

/**
 * The figure is data, not art: app_settings/posestudio/<figure_file> lists views
 * (body, hands, face, ...) and per view the bones as normalised hotspots with an
 * optional parent to draw a line to and an optional background UI image. A pony
 * or quadruped figure is a new data file, not code. The widget's value is the
 * selected joint name; committing (a click on a dot) fires the commit signal.
 */
class SLPoseFigureCtrl : public LLUICtrl
{
public:
    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params>
    {
        Optional<std::string> figure_file;
        Optional<S32>         dot_radius;
        Optional<LLUIColor>   dot_color;
        Optional<LLUIColor>   changed_color;
        Optional<LLUIColor>   selected_color;
        Optional<LLUIColor>   hover_color;
        Optional<LLUIColor>   line_color;
        Optional<LLUIColor>   missing_color;
        Optional<LLUIColor>   background_color;

        Params();
    };

    struct Bone
    {
        std::string mJoint;
        std::string mLabel;
        std::string mParent;
        F32 mX = 0.5f;
        F32 mY = 0.5f;
    };

    struct View
    {
        std::string mId;
        std::string mLabel;
        F32 mAspect = 1.f;
        LLUIImagePtr mImage;
        std::vector<Bone> mBones;
    };

    enum EBoneState
    {
        BONE_NORMAL = 0,
        BONE_CHANGED,       // edited in the current pose
        BONE_MISSING,       // the avatar's skeleton does not have it
    };
    typedef std::function<EBoneState(const std::string& joint)> state_func_t;

    SLPoseFigureCtrl(const Params& p);
    ~SLPoseFigureCtrl() override = default;

    bool loadFigure(const std::string& file_name);
    const std::string& getFigureName() const { return mFigureName; }
    const std::vector<View>& getViews() const { return mViews; }

    void setView(const std::string& view_id);
    const std::string& getViewId() const;
    // Switches to the first view that has the joint, if the current one does not.
    void showJoint(const std::string& joint);

    void setStateFunc(state_func_t func) { mStateFunc = std::move(func); }

    void setValue(const LLSD& value) override;
    LLSD getValue() const override { return LLSD(mSelected); }

    void draw() override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleToolTip(S32 x, S32 y, MASK mask) override;
    void onMouseLeave(S32 x, S32 y, MASK mask) override;

private:
    const View* currentView() const;
    LLRect figureRect(const View& view) const;
    S32 boneAt(S32 x, S32 y) const;

    std::string mFigureName;
    std::vector<View> mViews;
    S32 mViewIndex = 0;
    std::string mSelected;
    S32 mHover = -1;
    state_func_t mStateFunc;

    S32 mDotRadius;
    LLUIColor mDotColor;
    LLUIColor mChangedColor;
    LLUIColor mSelectedColor;
    LLUIColor mHoverColor;
    LLUIColor mLineColor;
    LLUIColor mMissingColor;
    LLUIColor mBackgroundColor;
};

#endif // SL_POSEFIGURECTRL_H
