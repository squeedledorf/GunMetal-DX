/**
 * @file slposefigurectrl.cpp
 * @brief Pose Studio (#500): data-driven bone picker.
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

#include "slposefigurectrl.h"

#include "lldir.h"
#include "llrender.h"
#include "llrender2dutils.h"
#include "llsdserialize.h"
#include "llsdutil.h"
#include "lltooltip.h"
#include "llui.h"
#include "lluicolortable.h"
#include "v2math.h"

static LLDefaultChildRegistry::Register<SLPoseFigureCtrl> r("sl_pose_figure");

namespace
{
constexpr char FIGURE_SUBDIRECTORY[] = "posestudio";
constexpr S32 FIGURE_MARGIN = 6;
constexpr F32 PICK_SLOP = 1.8f;  // pick radius as a multiple of the dot radius
}

SLPoseFigureCtrl::Params::Params()
:   figure_file("figure_file", std::string("figure_human.xml")),
    dot_radius("dot_radius", 5),
    dot_color("dot_color", LLColor4(0.75f, 0.78f, 0.85f, 1.f)),
    changed_color("changed_color", LLColor4(0.30f, 0.80f, 1.00f, 1.f)),
    selected_color("selected_color", LLColor4(1.00f, 0.80f, 0.20f, 1.f)),
    hover_color("hover_color", LLColor4(1.f, 1.f, 1.f, 1.f)),
    line_color("line_color", LLColor4(0.55f, 0.58f, 0.65f, 0.8f)),
    missing_color("missing_color", LLColor4(0.4f, 0.4f, 0.4f, 0.35f)),
    background_color("background_color", LLColor4(0.f, 0.f, 0.f, 0.25f))
{
    changeDefault(mouse_opaque, true);
}

SLPoseFigureCtrl::SLPoseFigureCtrl(const Params& p)
:   LLUICtrl(p),
    mDotRadius(llmax(2, p.dot_radius())),
    mDotColor(p.dot_color),
    mChangedColor(p.changed_color),
    mSelectedColor(p.selected_color),
    mHoverColor(p.hover_color),
    mLineColor(p.line_color),
    mMissingColor(p.missing_color),
    mBackgroundColor(p.background_color)
{
    loadFigure(p.figure_file);
}

bool SLPoseFigureCtrl::loadFigure(const std::string& file_name)
{
    mViews.clear();
    mViewIndex = 0;
    mHover = -1;

    const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, FIGURE_SUBDIRECTORY, file_name);
    llifstream infile(path.c_str());
    if (!infile.is_open())
    {
        LL_WARNS("PoseStudio") << "Cannot open pose figure " << path << LL_ENDL;
        return false;
    }

    LLSD data;
    if (LLSDSerialize::fromXML(data, infile) == LLSDParser::PARSE_FAILURE || !data.isMap())
    {
        LL_WARNS("PoseStudio") << "Cannot parse pose figure " << path << LL_ENDL;
        return false;
    }

    mFigureName = data["name"].asString();
    for (const LLSD& view_data : llsd::inArray(data["views"]))
    {
        View view;
        view.mId = view_data["id"].asString();
        view.mLabel = view_data["label"].asString();
        view.mAspect = view_data.has("aspect") ? llclamp((F32)view_data["aspect"].asReal(), 0.2f, 5.f) : 1.f;
        const std::string image = view_data["image"].asString();
        if (!image.empty())
            view.mImage = LLUI::getUIImage(image);

        for (const LLSD& bone_data : llsd::inArray(view_data["bones"]))
        {
            Bone bone;
            bone.mJoint = bone_data["joint"].asString();
            bone.mLabel = bone_data.has("label") ? bone_data["label"].asString() : bone.mJoint;
            bone.mParent = bone_data["parent"].asString();
            bone.mX = llclamp((F32)bone_data["x"].asReal(), 0.f, 1.f);
            bone.mY = llclamp((F32)bone_data["y"].asReal(), 0.f, 1.f);
            if (!bone.mJoint.empty())
                view.mBones.push_back(bone);
        }
        if (!view.mBones.empty())
            mViews.push_back(view);
    }

    return !mViews.empty();
}

const SLPoseFigureCtrl::View* SLPoseFigureCtrl::currentView() const
{
    if (mViewIndex < 0 || mViewIndex >= (S32)mViews.size())
        return nullptr;
    return &mViews[mViewIndex];
}

void SLPoseFigureCtrl::setView(const std::string& view_id)
{
    for (S32 i = 0; i < (S32)mViews.size(); ++i)
    {
        if (mViews[i].mId == view_id)
        {
            mViewIndex = i;
            mHover = -1;
            return;
        }
    }
}

const std::string& SLPoseFigureCtrl::getViewId() const
{
    static const std::string empty;
    const View* view = currentView();
    return view ? view->mId : empty;
}

void SLPoseFigureCtrl::showJoint(const std::string& joint)
{
    if (const View* view = currentView())
    {
        for (const Bone& bone : view->mBones)
        {
            if (bone.mJoint == joint)
                return;
        }
    }
    for (S32 i = 0; i < (S32)mViews.size(); ++i)
    {
        for (const Bone& bone : mViews[i].mBones)
        {
            if (bone.mJoint == joint)
            {
                mViewIndex = i;
                mHover = -1;
                return;
            }
        }
    }
}

void SLPoseFigureCtrl::setValue(const LLSD& value)
{
    mSelected = value.asString();
}

LLRect SLPoseFigureCtrl::figureRect(const View& view) const
{
    // Largest rect of the view's aspect that fits, centred.
    const S32 avail_w = llmax(1, getRect().getWidth() - 2 * FIGURE_MARGIN);
    const S32 avail_h = llmax(1, getRect().getHeight() - 2 * FIGURE_MARGIN);
    S32 w = avail_w;
    S32 h = ll_round((F32)w / view.mAspect);
    if (h > avail_h)
    {
        h = avail_h;
        w = ll_round((F32)h * view.mAspect);
    }
    const S32 left = (getRect().getWidth() - w) / 2;
    const S32 bottom = (getRect().getHeight() - h) / 2;
    return LLRect(left, bottom + h, left + w, bottom);
}

S32 SLPoseFigureCtrl::boneAt(S32 x, S32 y) const
{
    const View* view = currentView();
    if (!view)
        return -1;

    const LLRect fr = figureRect(*view);
    const F32 pick = (F32)mDotRadius * PICK_SLOP;
    F32 best = pick * pick;
    S32 found = -1;
    for (S32 i = 0; i < (S32)view->mBones.size(); ++i)
    {
        const Bone& bone = view->mBones[i];
        const F32 dx = (F32)x - ((F32)fr.mLeft + bone.mX * (F32)fr.getWidth());
        const F32 dy = (F32)y - ((F32)fr.mBottom + bone.mY * (F32)fr.getHeight());
        const F32 d2 = dx * dx + dy * dy;
        if (d2 <= best)
        {
            best = d2;
            found = i;
        }
    }
    return found;
}

void SLPoseFigureCtrl::draw()
{
    const LLRect local(0, getRect().getHeight(), getRect().getWidth(), 0);
    gl_rect_2d(local, mBackgroundColor.get(), true);

    const View* view = currentView();
    if (!view)
    {
        LLUICtrl::draw();
        return;
    }

    const LLRect fr = figureRect(*view);
    if (view->mImage.notNull())
        view->mImage->draw(fr.mLeft, fr.mBottom, fr.getWidth(), fr.getHeight(), getEnabled() ? LLColor4::white : LLColor4(1.f, 1.f, 1.f, 0.5f));

    auto bone_pos = [&fr](const Bone& bone)
    {
        return LLVector2((F32)fr.mLeft + bone.mX * (F32)fr.getWidth(), (F32)fr.mBottom + bone.mY * (F32)fr.getHeight());
    };

    std::map<std::string, LLVector2> positions;
    for (const Bone& bone : view->mBones)
        positions[bone.mJoint] = bone_pos(bone);

    // Bones first, then the dots on top of them.
    const LLColor4 line = mLineColor.get();
    for (const Bone& bone : view->mBones)
    {
        if (bone.mParent.empty())
            continue;
        auto it = positions.find(bone.mParent);
        if (it == positions.end())
            continue;
        const LLVector2 a = positions[bone.mJoint];
        gl_line_2d(ll_round(a.mV[VX]), ll_round(a.mV[VY]), ll_round(it->second.mV[VX]), ll_round(it->second.mV[VY]), line);
    }

    for (S32 i = 0; i < (S32)view->mBones.size(); ++i)
    {
        const Bone& bone = view->mBones[i];
        const LLVector2 p = positions[bone.mJoint];
        const EBoneState state = mStateFunc ? mStateFunc(bone.mJoint) : BONE_NORMAL;
        const bool selected = (bone.mJoint == mSelected);
        const bool hovered = (i == mHover);

        LLColor4 color = mDotColor.get();
        if (state == BONE_MISSING)
            color = mMissingColor.get();
        else if (selected)
            color = mSelectedColor.get();
        else if (state == BONE_CHANGED)
            color = mChangedColor.get();
        if (!getEnabled())
            color.mV[VALPHA] *= 0.5f;

        const F32 radius = (F32)mDotRadius * (selected ? 1.3f : 1.f);
        gGL.color4fv(color.mV);
        gl_circle_2d(p.mV[VX], p.mV[VY], radius, 16, true);

        if (selected || hovered)
        {
            gGL.color4fv((hovered ? mHoverColor.get() : mSelectedColor.get()).mV);
            gl_circle_2d(p.mV[VX], p.mV[VY], radius + 2.5f, 20, false);
        }
    }

    LLUICtrl::draw();
}

bool SLPoseFigureCtrl::handleMouseDown(S32 x, S32 y, MASK mask)
{
    if (!getEnabled())
        return LLUICtrl::handleMouseDown(x, y, mask);

    const S32 index = boneAt(x, y);
    const View* view = currentView();
    if (index < 0 || !view)
        return true;

    const Bone& bone = view->mBones[index];
    if (mStateFunc && mStateFunc(bone.mJoint) == BONE_MISSING)
        return true;

    mSelected = bone.mJoint;
    onCommit();
    return true;
}

bool SLPoseFigureCtrl::handleHover(S32 x, S32 y, MASK mask)
{
    mHover = getEnabled() ? boneAt(x, y) : -1;
    return LLUICtrl::handleHover(x, y, mask);
}

void SLPoseFigureCtrl::onMouseLeave(S32 x, S32 y, MASK mask)
{
    mHover = -1;
    LLUICtrl::onMouseLeave(x, y, mask);
}

bool SLPoseFigureCtrl::handleToolTip(S32 x, S32 y, MASK mask)
{
    const View* view = currentView();
    const S32 index = boneAt(x, y);
    if (!view || index < 0)
        return LLUICtrl::handleToolTip(x, y, mask);

    LLToolTipMgr::instance().show(LLToolTip::Params().message(view->mBones[index].mLabel).sticky_rect(calcScreenRect()));
    return true;
}
