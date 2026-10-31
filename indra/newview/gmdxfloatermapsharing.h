/**
 * @file gmdxfloatermapsharing.h
 * @brief GunMetal-DX Map Sharing window: who gets my map shares and pings, and whose I take.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
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

// <GMDX> whole file. gmdx_map_sharing (floater_gmdx_map_sharing.xml), over GMDXMapShare (gmdxmapshare.h):
// Send: my worn group, then the target groups and people (On, Keep, who is here), + Group, + Person
// nearby, Remove. Receive: the people whose shares are drawn without asking, the blocked senders,
// and what others shared now (Keep, Remove, Block sender, Clear all).

#pragma once

#include "llfloater.h"
#include "llframetimer.h"

class LLButton;
class LLCheckBoxCtrl;
class LLContextMenu;
class LLScrollListCtrl;

class GMDXFloaterMapSharing : public LLFloater
{
public:
    GMDXFloaterMapSharing(const LLSD& key);
    ~GMDXFloaterMapSharing();

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void draw() override;

private:
    void refresh() override;
    // A menu under the button: pick calls back with the chosen id ("empty" shown when there is none).
    void showMenu(LLButton* under, const std::vector<std::pair<std::string, LLUUID>>& choices, const std::string& empty,
                  const std::function<void(const LLUUID&)>& pick);
    void onAddGroup();
    void onAddPerson();

    LLCheckBoxCtrl* mMyGroup = nullptr;
    LLScrollListCtrl* mTargets = nullptr;
    LLScrollListCtrl* mAlways = nullptr;
    LLScrollListCtrl* mBlocked = nullptr;
    LLScrollListCtrl* mReceived = nullptr;
    std::string mShown[4];          // what each list shows (its rows, serialized): rebuilt only on a change
    LLFrameTimer mRefreshTimer;
    bool mDirty = false;            // refresh on the next frame (a list's own checkbox cannot rebuild it)
    LLHandle<LLContextMenu> mMenu;
};
