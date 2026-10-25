/**
 * @file slpanelgesturesetspulldown.cpp
 * @brief Status bar quick swap for gesture sets (#237).
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

#include "slpanelgesturesetspulldown.h"

#include "llfloaterreg.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "slgesturesets.h"

SLPanelGestureSetsPulldown::SLPanelGestureSetsPulldown()
{
    mHoverTimer.stop();

    mCommitCallbackRegistrar.add("GestureSets.RowClick", boost::bind(&SLPanelGestureSetsPulldown::onRowClick, this));
    mCommitCallbackRegistrar.add("GestureSets.Manage", boost::bind(&SLPanelGestureSetsPulldown::onManageClick, this));

    buildFromFile("panel_gesture_sets_pulldown.xml");
}

bool SLPanelGestureSetsPulldown::postBuild()
{
    mSetsConnection = SLGestureSets::instance().setChangedCallback(boost::bind(&SLPanelGestureSetsPulldown::populatePanel, this));
    populatePanel();

    return LLPanelPulldown::postBuild();
}

void SLPanelGestureSetsPulldown::populatePanel()
{
    LLScrollListCtrl* scroll = getChild<LLScrollListCtrl>("set_list");
    scroll->clearRows();

    const SLGestureSets& sets = SLGestureSets::instance();
    const std::string& last_applied = sets.getLastApplied();
    if (sets.getSets().empty())
    {
        scroll->setCommentText(getString("no_sets"));
        return;
    }

    for (const auto& entry : sets.getSets())
    {
        const std::string& name = entry.first;
        LLSD row;
        row["value"] = name;
        row["columns"][0]["column"] = "icon";
        if (name == last_applied)
        {
            row["columns"][0]["type"] = "icon";
            row["columns"][0]["value"] = "Check_Mark";
        }
        row["columns"][1]["column"] = "set_name";
        row["columns"][1]["value"] = name;

        LLScrollListItem* item = scroll->addElement(row);
        if (item && name == last_applied)
        {
            item->setSelected(true);
        }
    }
}

void SLPanelGestureSetsPulldown::onRowClick()
{
    LLScrollListItem* item = getChild<LLScrollListCtrl>("set_list")->getFirstSelected();
    if (!item)
    {
        return;
    }

    SLGestureSets::instance().applySet(item->getValue().asString());

    // Scroll grabbed focus, drop it to prevent selection of parent menu
    setFocus(false);
    setVisible(false);
}

void SLPanelGestureSetsPulldown::onManageClick()
{
    setVisible(false);
    LLFloaterReg::showInstance("gesture_sets");
}
