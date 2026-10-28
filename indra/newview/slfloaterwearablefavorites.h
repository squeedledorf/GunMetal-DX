/**
 * @file slfloaterwearablefavorites.h
 * @brief Favorite Wearables (#1132): the window that lists the favorites and wears them.
 *
 * The list is a real inventory panel rooted at the favorites folder (see slwearablefavorites.h),
 * so every row is the real item bridge of the favorite's original: the right-click menu is the
 * full inventory menu, and Copy puts the original on the clipboard (see
 * LLInvFVBridge::copyToClipboard), which is what makes "copy a favorite, Paste As Link into an
 * outfit folder" work in one step.
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

#ifndef SL_FLOATERWEARABLEFAVORITES_H
#define SL_FLOATERWEARABLEFAVORITES_H

#include "llfloater.h"
#include "llinventorypanel.h"

class LLButton;
class LLComboBox;
class LLFilterEditor;
class LLTextBox;

// Inventory panel rooted at the favorites folder. The folder is only known (or created) once
// the window opens, so the root is built later through setFavoritesFolder().
class SLWearableFavoritesPanel : public LLInventoryPanel
{
public:
    struct Params : public LLInitParam::Block<Params, LLInventoryPanel::Params>
    {};

    void initFromParams(const Params& p);
    void onVisibilityChange(bool new_visibility) override;
    void draw() override;
    // Nothing is ever dropped into the list itself: the window turns drops into links (see
    // SLFloaterWearableFavorites::handleDragAndDrop), so an item dragged onto it is never moved.
    bool handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop,
                           EDragAndDropType cargo_type, void* cargo_data,
                           EAcceptance* accept, std::string& tooltip_msg) override;

    // Builds the list for this folder (once; later calls are ignored).
    void setFavoritesFolder(const LLUUID& folder_id);
    bool hasFolder() const { return mRootInited; }

protected:
    SLWearableFavoritesPanel(const Params& p);
    friend class LLUICtrlFactory;
};

class SLFloaterWearableFavorites : public LLFloater
{
    LOG_CLASS(SLFloaterWearableFavorites);
public:
    SLFloaterWearableFavorites(const LLSD& key);
    ~SLFloaterWearableFavorites() override;

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void draw() override;
    bool handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop,
                           EDragAndDropType cargo_type, void* cargo_data,
                           EAcceptance* accept, std::string& tooltip_msg) override;

private:
    void attachFolder(const LLUUID& folder_id);
    void onFilterChanged();
    void onSortChanged();
    void applySortOrder();
    void doAction(const char* action);
    void updateButtons();

    SLWearableFavoritesPanel* mPanel = nullptr;
    LLFilterEditor* mFilter = nullptr;
    LLComboBox* mSortCombo = nullptr;
    LLButton* mWearBtn = nullptr;
    LLButton* mAddBtn = nullptr;
    LLButton* mTakeOffBtn = nullptr;
    LLTextBox* mStatusText = nullptr;
};

#endif // SL_FLOATERWEARABLEFAVORITES_H
