/**
 * @file slfloaterviewerimport.cpp
 * @brief "Import from another viewer" (#131, #231, #232). See the header.
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

#include "slfloaterviewerimport.h"

#include "llbutton.h"
#include "llcallbacklist.h"
#include "llcheckboxctrl.h"
#include "llcombobox.h"
#include "lldirpicker.h"   // <GMDX> chosen Firestorm profile folder
#include "llfiltereditor.h"
#include "llfloaterreg.h"
#include "llnotificationsutil.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "lltexteditor.h"

namespace fs = std::filesystem;

namespace
{
std::string utf8(const fs::path& p)
{
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

bool loggedIn()
{
    return !gDirUtilp->getLindenUserDir().empty();
}

bool matchesFilter(const slimport::PlanItem& item, const std::string& filter)
{
    if (filter.empty()) return true;
    for (const std::string* field : { &item.name, &item.note, &item.current, &item.imported })
    {
        std::string s = *field;
        LLStringUtil::toLower(s);
        if (s.find(filter) != std::string::npos) return true;
    }
    return false;
}
}

SLFloaterViewerImport::SLFloaterViewerImport(const LLSD& key)
:   LLFloater(key)
{
}

bool SLFloaterViewerImport::postBuild()
{
    mViewerCombo  = getChild<LLComboBox>("source_viewer");
    mAccountCombo = getChild<LLComboBox>("source_account");
    mCategoryList = getChild<LLScrollListCtrl>("category_list");
    mItemList     = getChild<LLScrollListCtrl>("item_list");
    mFilter       = getChild<LLFilterEditor>("item_filter");
    mShowAll      = getChild<LLCheckBoxCtrl>("show_all");
    mReport       = getChild<LLTextEditor>("report");
    mImportBtn    = getChild<LLButton>("import_btn");
    mPreviewBtn   = getChild<LLButton>("preview_btn");
    mUndoBtn      = getChild<LLButton>("undo_btn");

    mViewerCombo->setCommitCallback([this](LLUICtrl*, const LLSD&) { onViewerChanged(); });
    mAccountCombo->setCommitCallback([this](LLUICtrl*, const LLSD&) { onAccountChanged(); });
    mCategoryList->setCommitOnSelectionChange(true);
    mCategoryList->setCommitCallback([this](LLUICtrl*, const LLSD&) { onCategorySelected(); });
    // Clicking a row's checkbox selects the row too: pick the tick up and refresh the counts.
    mItemList->setCommitOnSelectionChange(true);
    mItemList->setCommitCallback([this](LLUICtrl*, const LLSD&) { syncChecks(); scheduleRefresh(); });
    mFilter->setCommitCallback([this](LLUICtrl*, const LLSD& value)
    {
        syncChecks();
        mFilterText = value.asString();
        LLStringUtil::toLower(mFilterText);
        LLStringUtil::trim(mFilterText);
        refreshItems();
    });
    mShowAll->setCommitCallback([this](LLUICtrl*, const LLSD&) { syncChecks(); refreshItems(); });
    getChild<LLUICtrl>("select_all")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSelectAll(true); });
    getChild<LLUICtrl>("select_none")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSelectAll(false); });
    mPreviewBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPreview(); });
    mImportBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onImport(); });
    mUndoBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onUndo(); });
    getChild<LLUICtrl>("close_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { closeFloater(); });
    getChild<LLUICtrl>("choose_folder_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onChooseFolder(); }); // <GMDX>
    return true;
}

// <GMDX> SoapStorm c6d090301f: when Firestorm's profile is not in the usual place (a copy,
// a portable install, a Windows install seen from Linux), pick its folder by hand.
void SLFloaterViewerImport::onChooseFolder()
{
    LLHandle<LLFloater> handle = getHandle();
    (new LLDirPickerThread([handle](const std::vector<std::string>& dirs, std::string)
    {
        SLFloaterViewerImport* self = static_cast<SLFloaterViewerImport*>(handle.get());
        if (self && !dirs.empty() && !dirs[0].empty())
        {
            self->addChosenFolder(dirs[0]);
        }
    }, gDirUtilp->getOSUserDir()))->getFile();
}

void SLFloaterViewerImport::addChosenFolder(const std::string& folder)
{
    fs::path dir = fs::u8path(folder).lexically_normal();
    if (dir.filename().empty()) dir = dir.parent_path();      // trailing separator
    if (dir.filename() == "user_settings") dir = dir.parent_path();   // picked one level too deep

    LLStringUtil::format_map_t args;
    args["[FOLDER]"] = utf8(dir);
    std::error_code ec;
    if (fs::equivalent(dir, fs::u8path(gDirUtilp->getOSUserAppDir()), ec))
    {
        mReport->setText(getString("folder_is_ours", args));
        return;
    }

    slimport::SourceViewer viewer;
    viewer.id = "firestorm";
    viewer.label = getString("chosen_folder_label");
    viewer.dir = dir;
    viewer.legacy_im_names = true;
    viewer.key_bindings_compatible = true;
    if (!fs::exists(dir / "user_settings" / "settings.xml", ec) && slimport::findAccounts(viewer).empty())
    {
        mReport->setText(getString("folder_not_profile", args));
        return;
    }
    for (size_t i = 0; i < mViewers.size(); ++i)
    {
        if (fs::equivalent(mViewers[i].dir, dir, ec))
        {
            mViewerCombo->setValue(LLSD((S32)i));   // already listed
            onViewerChanged();
            return;
        }
    }

    mViewers.push_back(viewer);
    const S32 index = (S32)mViewers.size() - 1;
    mViewerCombo->add(viewer.label + "  (" + utf8(dir) + ")", LLSD(index));
    mViewerCombo->setEnabled(true);
    mViewerCombo->setValue(LLSD(index));
    mReport->setText(getString("status_ready") + (loggedIn() ? std::string() : "\n" + getString("status_logged_out")));
    onViewerChanged();
}
// </GMDX>

void SLFloaterViewerImport::onOpen(const LLSD& key)
{
    refreshViewers();
    refreshUndo();
}

std::string SLFloaterViewerImport::categoryLabel(slimport::ECategory cat) const
{
    return getString(std::string("cat_") + slimport::categoryKey(cat));
}

std::string SLFloaterViewerImport::stateLabel(slimport::EStatus status) const
{
    switch (status)
    {
        case slimport::STATUS_OFFER:    return getString("state_offer");
        case slimport::STATUS_CONFLICT: return getString("state_conflict");
        case slimport::STATUS_SAME:     return getString("state_same");
        default:                        return getString("state_skip");
    }
}

// static
std::vector<slimport::SourceViewer> SLFloaterViewerImport::findInstalledViewers(std::vector<std::string>* search_log)
{
    const fs::path home = fs::u8path(gDirUtilp->getOSUserDir());
    std::vector<slimport::SearchBase> bases = { { home, std::string(), false } };
#if LL_WINDOWS
    // Firestorm_x64 may also live under %LOCALAPPDATA%.
    if (const auto local = LLStringUtil::getoptenv("LOCALAPPDATA")) bases.push_back({ fs::u8path(*local), std::string(), false });
#elif LL_LINUX
    for (const slimport::SearchBase& base : slimport::sandboxedAppBases(home)) bases.push_back(base);
#endif
    return slimport::findSourceViewers(bases, fs::u8path(gDirUtilp->getOSUserAppDir()), search_log);
}

// static
bool SLFloaterViewerImport::hasViewerData(const std::string& viewer_id)
{
    for (const slimport::SourceViewer& viewer : findInstalledViewers())
    {
        if (viewer.id == viewer_id) return true;
    }
    return false;
}

// static
void SLFloaterViewerImport::showForViewer(const std::string& viewer_id)
{
    // Opening runs refreshViewers(), which selects the first viewer found; pick ours after.
    if (SLFloaterViewerImport* floater = LLFloaterReg::showTypedInstance<SLFloaterViewerImport>("viewer_import"))
    {
        floater->selectViewer(viewer_id);
    }
}

void SLFloaterViewerImport::selectViewer(const std::string& viewer_id)
{
    for (size_t i = 0; i < mViewers.size(); ++i)
    {
        if (mViewers[i].id != viewer_id) continue;
        if (mViewerCombo->getValue().asInteger() != (S32)i)
        {
            mViewerCombo->setValue(LLSD((S32)i));
            onViewerChanged();
        }
        return;
    }
}

void SLFloaterViewerImport::refreshViewers()
{
    const fs::path home = fs::u8path(gDirUtilp->getOSUserDir());
    const slimport::Sandbox sandbox = slimport::detectSandbox();
    if (!sandbox.kind.empty())
    {
        LL_INFOS("ViewerImport") << "Running in a " << sandbox.kind << " sandbox (" << sandbox.app_id << "): other viewers' folders may be hidden" << LL_ENDL;
    }
    std::vector<std::string> search;
    mViewers = findInstalledViewers(&search);
    for (const std::string& line : search) LL_INFOS("ViewerImport") << line << LL_ENDL;
    LL_INFOS("ViewerImport") << mViewers.size() << " viewer(s) to import from" << LL_ENDL;

    // Not shown otherwise: folders we could not read, and what the sandbox needs.
    std::string problems;
    for (const std::string& line : search)
    {
        if (line.find(": permission denied") != std::string::npos) problems += "\nCould not read " + line;
    }
    bool have_firestorm = false;
    for (const slimport::SourceViewer& v : mViewers) have_firestorm |= v.id == "firestorm";
    if (!sandbox.kind.empty() && (!have_firestorm || !problems.empty()))
    {
        problems += "\n" + slimport::sandboxHint(sandbox, utf8(home / ".firestorm_x64"));
    }

    mViewerCombo->removeall();
    for (size_t i = 0; i < mViewers.size(); ++i)
    {
        mViewerCombo->add(mViewers[i].label + "  (" + utf8(mViewers[i].dir) + ")", LLSD((S32)i));
    }
    const bool any = !mViewers.empty();
    mViewerCombo->setEnabled(any);
    if (any)
    {
        mViewerCombo->selectFirstItem();
        mReport->setText(getString("status_ready") + (loggedIn() ? std::string() : "\n" + getString("status_logged_out")) + problems);
    }
    else
    {
        mReport->setText(getString("status_none") + problems);
    }
    onViewerChanged();
}

const slimport::SourceViewer* SLFloaterViewerImport::selectedViewer() const
{
    const S32 i = mViewerCombo->getValue().asInteger();
    return (!mViewers.empty() && i >= 0 && i < (S32)mViewers.size()) ? &mViewers[i] : nullptr;
}

const slimport::SourceAccount* SLFloaterViewerImport::selectedAccount() const
{
    const S32 i = mAccountCombo->getValue().asInteger();
    return (!mAccounts.empty() && i >= 0 && i < (S32)mAccounts.size()) ? &mAccounts[i] : nullptr;
}

void SLFloaterViewerImport::onViewerChanged()
{
    mAccounts.clear();
    mAccountCombo->removeall();
    const slimport::SourceViewer* viewer = selectedViewer();
    if (viewer)
    {
        mAccounts = slimport::findAccounts(*viewer);
        // Pre-select the account we're logged in as, if the other viewer has it.
        const std::string ours = utf8(fs::u8path(gDirUtilp->getLindenUserDir()).filename());
        S32 preselect = 0;
        for (size_t i = 0; i < mAccounts.size(); ++i)
        {
            mAccountCombo->add(mAccounts[i].name + llformat("  (%d transcripts)", (S32)mAccounts[i].transcript_count), LLSD((S32)i));
            if (mAccounts[i].name == ours) preselect = (S32)i;
        }
        if (!mAccounts.empty()) mAccountCombo->setValue(LLSD(preselect));
    }
    mAccountCombo->setEnabled(!mAccounts.empty());
    onAccountChanged();
}

void SLFloaterViewerImport::onAccountChanged()
{
    rebuildPlan();
}

void SLFloaterViewerImport::rebuildPlan()
{
    const slimport::SourceViewer* viewer = selectedViewer();
    mHavePlan = viewer != nullptr;
    mPlan = slimport::ImportPlan();
    if (viewer)
    {
        mPlan.build(*viewer, selectedAccount(), loggedIn());
        // Open on the first category that has something to review.
        mCategory = slimport::CAT_SETTINGS;
        for (S32 c = 0; c < slimport::CAT_COUNT; ++c)
        {
            const auto cat = (slimport::ECategory)c;
            if (mPlan.count(cat, slimport::STATUS_OFFER) + mPlan.count(cat, slimport::STATUS_CONFLICT) > 0)
            {
                mCategory = c;
                break;
            }
        }
    }
    mImportBtn->setEnabled(mHavePlan);
    mPreviewBtn->setEnabled(mHavePlan);
    refreshCategories();
    refreshItems();
}

void SLFloaterViewerImport::refreshCategories()
{
    const S32 scroll = mCategoryList->getScrollPos();
    mCategoryList->deleteAllItems();
    if (!mHavePlan) return;
    for (S32 c = 0; c < slimport::CAT_COUNT; ++c)
    {
        const auto cat = (slimport::ECategory)c;
        const S32 offers = mPlan.count(cat, slimport::STATUS_OFFER) + mPlan.count(cat, slimport::STATUS_CONFLICT);
        const S32 rows = offers + mPlan.count(cat, slimport::STATUS_SAME) + mPlan.count(cat, slimport::STATUS_SKIP);
        if (rows == 0) continue;   // nothing of this kind in their profile
        LLSD row;
        row["value"] = c;
        row["columns"][0]["column"] = "on";
        row["columns"][0]["type"]   = "checkbox";
        row["columns"][0]["value"]  = mPlan.mCategoryOn[c];
        row["columns"][0]["enabled"] = offers > 0;
        row["columns"][1]["column"] = "name";
        row["columns"][1]["value"]  = categoryLabel(cat);
        row["columns"][2]["column"] = "count";
        row["columns"][2]["value"]  = llformat("%d", mPlan.countSelected(cat));
        row["columns"][3]["column"] = "cat_index";
        row["columns"][3]["value"]  = c;
        mCategoryList->addElement(row);
    }
    mCategoryList->setCommitOnSelectionChange(false);
    mCategoryList->selectByValue(LLSD(mCategory));
    mCategoryList->setCommitOnSelectionChange(true);
    mCategoryList->setScrollPos(scroll);
}

void SLFloaterViewerImport::refreshItems()
{
    const S32 scroll = mItemList->getScrollPos();
    mItemList->deleteAllItems();
    if (!mHavePlan) return;
    const bool show_all = mShowAll->get();
    for (size_t i = 0; i < mPlan.mItems.size(); ++i)
    {
        const slimport::PlanItem& item = mPlan.mItems[i];
        if (item.cat != mCategory) continue;
        const bool actionable = item.status == slimport::STATUS_OFFER || item.status == slimport::STATUS_CONFLICT;
        if (!actionable && !show_all) continue;
        if (!matchesFilter(item, mFilterText)) continue;
        LLSD row;
        row["value"] = (S32)i;
        row["columns"][0]["column"] = "apply";
        row["columns"][0]["type"]   = "checkbox";
        row["columns"][0]["value"]  = actionable && item.apply;
        row["columns"][0]["enabled"] = actionable;
        row["columns"][1]["column"] = "name";
        row["columns"][1]["value"]  = item.name;
        row["columns"][2]["column"] = "current";
        row["columns"][2]["value"]  = item.current;
        row["columns"][3]["column"] = "imported";
        row["columns"][3]["value"]  = item.imported;
        row["columns"][4]["column"] = "note";
        row["columns"][4]["value"]  = (item.status == slimport::STATUS_OFFER) ? item.note
                                    : stateLabel(item.status) + (item.note.empty() ? std::string() : ": " + item.note);
        row["columns"][5]["column"] = "item_index";
        row["columns"][5]["value"]  = (S32)i;
        mItemList->addElement(row);
    }
    mItemList->setScrollPos(scroll);
}

void SLFloaterViewerImport::syncChecks()
{
    if (!mHavePlan) return;
    for (LLScrollListItem* row : mItemList->getAllData())
    {
        const S32 idx = row->getValue().asInteger();
        LLScrollListCell* check = row->getColumn(0);
        if (!check || idx < 0 || idx >= (S32)mPlan.mItems.size()) continue;
        slimport::PlanItem& item = mPlan.mItems[idx];
        if (item.status == slimport::STATUS_OFFER || item.status == slimport::STATUS_CONFLICT)
        {
            item.apply = check->getValue().asBoolean();
        }
    }
    for (LLScrollListItem* row : mCategoryList->getAllData())
    {
        const S32 cat = row->getValue().asInteger();
        LLScrollListCell* check = row->getColumn(0);
        if (check && cat >= 0 && cat < slimport::CAT_COUNT) mPlan.mCategoryOn[cat] = check->getValue().asBoolean();
    }
}

void SLFloaterViewerImport::onCategorySelected()
{
    syncChecks();
    if (LLScrollListItem* row = mCategoryList->getFirstSelected())
    {
        mCategory = row->getValue().asInteger();
    }
    scheduleRefresh();
}

// A row's checkbox commits from inside its own click handler: rebuilding that list right
// away deletes the cell still on the stack (crash on unticking a category). Rebuild on idle.
void SLFloaterViewerImport::scheduleRefresh()
{
    if (mRefreshPending) return;
    mRefreshPending = true;
    LLHandle<LLFloater> handle = getHandle();
    doOnIdleOneTime([handle]()
    {
        if (SLFloaterViewerImport* self = static_cast<SLFloaterViewerImport*>(handle.get()))
        {
            self->mRefreshPending = false;
            self->refreshCategories();
            self->refreshItems();
        }
    });
}

void SLFloaterViewerImport::onSelectAll(bool on)
{
    syncChecks();
    for (slimport::PlanItem& item : mPlan.mItems)
    {
        if (item.cat == mCategory && (item.status == slimport::STATUS_OFFER || item.status == slimport::STATUS_CONFLICT)
            && matchesFilter(item, mFilterText))
        {
            item.apply = on;
        }
    }
    if (on && mCategory >= 0 && mCategory < slimport::CAT_COUNT) mPlan.mCategoryOn[mCategory] = true;
    refreshCategories();
    refreshItems();
}

void SLFloaterViewerImport::showReport(const std::vector<std::string>& lines, const std::string& header)
{
    std::string text = header;
    for (const std::string& line : lines)
    {
        if (!text.empty()) text += "\n";
        text += line;
    }
    mReport->setText(text);
}

void SLFloaterViewerImport::onPreview()
{
    if (!mHavePlan) return;
    syncChecks();
    std::vector<std::string> lines;
    for (S32 c = 0; c < slimport::CAT_COUNT; ++c)
    {
        const auto cat = (slimport::ECategory)c;
        const S32 offers = mPlan.count(cat, slimport::STATUS_OFFER);
        const S32 conflicts = mPlan.count(cat, slimport::STATUS_CONFLICT);
        const S32 same = mPlan.count(cat, slimport::STATUS_SAME);
        const S32 skip = mPlan.count(cat, slimport::STATUS_SKIP);
        if (offers + conflicts + same + skip == 0) continue;
        LLStringUtil::format_map_t args;
        args["[CATEGORY]"] = categoryLabel(cat);
        args["[APPLY]"]    = llformat("%d", mPlan.countSelected(cat));
        if (!mPlan.mCategoryOn[c])
        {
            args["[APPLY]"] = llformat("%d", offers);
            lines.push_back(getString("preview_off", args));
            continue;
        }
        S32 conflicts_off = 0;
        for (const slimport::PlanItem& item : mPlan.mItems)
        {
            if (item.cat == cat && item.status == slimport::STATUS_CONFLICT && !item.apply) ++conflicts_off;
        }
        args["[CONFLICT]"] = llformat("%d", conflicts_off);
        args["[SAME]"]     = llformat("%d", same);
        args["[SKIP]"]     = llformat("%d", skip);
        lines.push_back(getString("preview_line", args));
    }
    showReport(lines, getString("preview_header"));
}

void SLFloaterViewerImport::onImport()
{
    if (!mHavePlan) return;
    syncChecks();
    bool needs_restart = false;
    const std::vector<std::string> lines = mPlan.apply(needs_restart);
    // Re-scan so the lists show the new state (an immediate re-import offers nothing).
    const S32 category = mCategory;
    rebuildPlan();
    mCategory = category;
    refreshCategories();
    refreshItems();
    showReport(lines);
    refreshUndo();
    if (needs_restart)
    {
        LLSD args;
        args["WHAT"] = getString("restart_what");
        LLNotificationsUtil::add("ViewerImportRestart", args);
    }
}

void SLFloaterViewerImport::refreshUndo()
{
    const std::string last = slimport::lastUndoableImport();
    mUndoBtn->setEnabled(!last.empty());
    LLStringUtil::format_map_t args;
    args["[WHEN]"] = last;
    mUndoBtn->setToolTip(last.empty() ? getString("undo_none") : getString("undo_tooltip", args));
}

void SLFloaterViewerImport::onUndo()
{
    const std::string last = slimport::lastUndoableImport();
    if (last.empty())
    {
        refreshUndo();
        return;
    }
    LLSD args;
    args["WHEN"] = last;
    LLHandle<LLFloater> handle = getHandle();
    LLNotificationsUtil::add("ViewerImportConfirmUndo", args, LLSD(), [handle](const LLSD& notification, const LLSD& response)
    {
        if (LLNotificationsUtil::getSelectedOption(notification, response) != 0) return false;
        bool needs_restart = false;
        const std::vector<std::string> lines = slimport::undoLastImport(needs_restart);
        if (SLFloaterViewerImport* self = static_cast<SLFloaterViewerImport*>(handle.get()))
        {
            self->rebuildPlan();
            self->showReport(lines);
            self->refreshUndo();
        }
        if (needs_restart)
        {
            LLSD restart_args;
            restart_args["WHAT"] = "Undone colours, dictionaries or lists";
            if (SLFloaterViewerImport* self = static_cast<SLFloaterViewerImport*>(handle.get()))
            {
                restart_args["WHAT"] = self->getString("restart_what");
            }
            LLNotificationsUtil::add("ViewerImportRestart", restart_args);
        }
        return false;
    });
}
