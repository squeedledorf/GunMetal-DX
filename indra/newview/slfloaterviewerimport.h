/**
 * @file slfloaterviewerimport.h
 * @brief "Import from another viewer" (#131, #231, #232): settings, per-account
 *        settings, notifications, toolbars, key bindings (merged), contact sets,
 *        teleport history, presets, poses, fonts, Windlight presets, auto-replace,
 *        dictionaries and chat transcripts from Firestorm, Black Dragon, Alchemy or
 *        Second Life. The plan itself lives in slviewerimportplan.
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

#ifndef SL_FLOATERVIEWERIMPORT_H
#define SL_FLOATERVIEWERIMPORT_H

#include "llfloater.h"
#include "slviewerimportcore.h"
#include "slviewerimportplan.h"

class LLButton;
class LLCheckBoxCtrl;
class LLComboBox;
class LLFilterEditor;
class LLScrollListCtrl;
class LLTextEditor;

class SLFloaterViewerImport final : public LLFloater
{
public:
    SLFloaterViewerImport(const LLSD& key);
    bool postBuild() override;
    void onOpen(const LLSD& key) override;

    // Other viewers' data directories on this computer (home, %LOCALAPPDATA%, Flatpak / Snap).
    // search_log: where it looked and what it found (paths only).
    static std::vector<slimport::SourceViewer> findInstalledViewers(std::vector<std::string>* search_log = nullptr);
    // Whether data of the viewer with this importer id ("firestorm", ...) is on this computer.
    static bool hasViewerData(const std::string& viewer_id);
    // Open the importer with that viewer selected (the first of its data directories).
    static void showForViewer(const std::string& viewer_id);
    // Select the first data directory of that viewer, if found.
    void selectViewer(const std::string& viewer_id);

private:
    ~SLFloaterViewerImport() override = default;

    void refreshViewers();
    void onViewerChanged();
    void onAccountChanged();
    void rebuildPlan();

    void refreshCategories();
    void refreshItems();
    void scheduleRefresh();
    void syncChecks();                  // list checkboxes -> mPlan
    void onCategorySelected();
    void onSelectAll(bool on);
    void onPreview();
    void onImport();
    void onUndo();
    void refreshUndo();
    void showReport(const std::vector<std::string>& lines, const std::string& header = std::string());
    std::string categoryLabel(slimport::ECategory cat) const;
    std::string stateLabel(slimport::EStatus status) const;

    const slimport::SourceViewer*  selectedViewer() const;
    const slimport::SourceAccount* selectedAccount() const;

    LLComboBox*       mViewerCombo = nullptr;
    LLComboBox*       mAccountCombo = nullptr;
    LLScrollListCtrl* mCategoryList = nullptr;
    LLScrollListCtrl* mItemList = nullptr;
    LLFilterEditor*   mFilter = nullptr;
    LLCheckBoxCtrl*   mShowAll = nullptr;
    LLTextEditor*     mReport = nullptr;
    LLButton*         mImportBtn = nullptr;
    LLButton*         mPreviewBtn = nullptr;
    LLButton*         mUndoBtn = nullptr;

    std::vector<slimport::SourceViewer>  mViewers;
    std::vector<slimport::SourceAccount> mAccounts;
    slimport::ImportPlan                 mPlan;
    bool                                 mHavePlan = false;
    bool                                 mRefreshPending = false;
    S32                                  mCategory = slimport::CAT_SETTINGS;
    std::string                          mFilterText;
};

#endif
