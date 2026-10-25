/**
 * @file slfloatergesturesets.h
 * @brief Gesture Sets floater (#237): manage and apply named gesture sets.
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

#ifndef SL_FLOATERGESTURESETS_H
#define SL_FLOATERGESTURESETS_H

#include <boost/signals2.hpp>

#include "llfloater.h"
#include "llframetimer.h"
#include "llgesturemgr.h"

class LLComboBox;
class LLRadioGroup;
class LLScrollListCell;
class LLScrollListCtrl;
class LLSearchEditor;
class LLTextBox;

class SLFloaterGestureSets : public LLFloater, public LLGestureManagerObserver
{
    LOG_CLASS(SLFloaterGestureSets);
public:
    SLFloaterGestureSets(const LLSD& key);
    ~SLFloaterGestureSets() override;

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void draw() override;

    // LLGestureManagerObserver
    void changed() override;

    // Asks for a name and creates a set from item_ids; shared with the
    // Gestures floater.
    static void promptNewSet(const uuid_vec_t& item_ids, const std::string& default_name);

private:
    void refreshSets();
    void refreshGestures();
    void refreshControls();
    void refreshConflicts();
    std::string getSelectedSet() const;
    void selectSet(const std::string& name);

    void onSetSelected();
    void onNew();
    void onSaveCurrent();
    void onApply();
    void onEditGesture();
    void onRename();
    void onDuplicate();
    void onDelete();
    void onModeChanged();
    void onCreateFromFolders();
    void onFilterChanged();
    void onCheckShown(bool check);
    void onMemberToggled(const LLSD& value, LLScrollListCell* cell);

    LLScrollListCtrl* mSetList = nullptr;
    LLScrollListCtrl* mGestureList = nullptr;
    LLSearchEditor* mFilterEdit = nullptr;
    LLComboBox* mFilterCombo = nullptr;
    LLRadioGroup* mModeRadio = nullptr;
    LLTextBox* mConflictText = nullptr;
    LLTextBox* mStatusText = nullptr;

    std::string mFilter;
    bool mGestureListDirty = false;
    LLFrameTimer mRefreshTimer;
    boost::signals2::scoped_connection mSetsConnection;
};

#endif // SL_FLOATERGESTURESETS_H
