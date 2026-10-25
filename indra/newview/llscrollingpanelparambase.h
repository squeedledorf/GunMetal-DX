/**
 * @file llscrollingpanelparam.h
 * @brief the scrolling panel containing a list of visual param
 *        panels
 *
 * $LicenseInfo:firstyear=2009&license=viewerlgpl$
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

#ifndef LL_SCROLLINGPANELPARAMBASE_H
#define LL_SCROLLINGPANELPARAMBASE_H

#include "llpanel.h"
#include "llscrollingpanellist.h"

#include <boost/signals2.hpp>

class LLViewerJointMesh;
class LLViewerVisualParam;
class LLWearable;
class LLVisualParamHint;
class LLViewerVisualParam;
class LLJoint;

class LLScrollingPanelParamBase : public LLScrollingPanel
{
public:
    LLScrollingPanelParamBase( const LLPanel::Params& panel_params,
                   LLViewerJointMesh* mesh, LLViewerVisualParam* param, bool allow_modify, LLWearable* wearable, LLJoint* jointp, bool use_hints = false );
    virtual ~LLScrollingPanelParamBase();

    void                updatePanel(bool allow_modify) override;

    static void         onSliderMoved(LLUICtrl* ctrl, void* userdata);

    F32                 weightToPercent( F32 weight );
    F32                 percentToWeight( F32 percent );

    /// Fired when this panel changed its parameter's weight (slider drag, or
    /// the hint buttons). The shape editor can show one section in several
    /// panels, each with panels of its own for the same parameters; the others
    /// follow the parameter through this.
    typedef boost::signals2::signal<void(LLScrollingPanelParamBase*)> param_moved_signal_t;
    static boost::signals2::connection connectParamMoved(const param_moved_signal_t::slot_type& slot);
    void                notifyParamMoved();

    /// Put the slider where the parameter's weight is, and nothing else (no
    /// hint renders): for a panel that did not move the parameter itself.
    void                syncSlider();

    S32                 getParamId() const;
    LLUICtrl*           getSlider() const { return mParamSlider; }

public:
    LLViewerVisualParam* mParam = nullptr;
protected:
    LLUICtrl* mParamSlider = nullptr;
    bool mAllowModify = false;
    LLWearable *mWearable = nullptr;
};

#endif
