/**
 * @file slfloatergesturesets.cpp
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

#include "llviewerprecompiledheaders.h"

#include "slfloatergesturesets.h"

#include <functional>

#include "llcallbacklist.h"
#include "llcombobox.h"
#include "llfloaterreg.h"
#include "llinventorymodel.h"
#include "llinventorymodelbackgroundfetch.h"
#include "llkeyboard.h"
#include "llmultigesture.h"
#include "llnotificationsutil.h"
#include "llpreviewgesture.h"
#include "llradiogroup.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "llsearcheditor.h"
#include "lltextbox.h"
#include "lltrans.h"
#include "llviewerinventory.h"
#include "slgesturesets.h"

namespace
{
    constexpr size_t MAX_SET_NAME_LENGTH = 63;
    constexpr size_t MAX_CONFLICT_LINES = 6;
    constexpr F32 LIST_REFRESH_INTERVAL = 0.25f;

    typedef std::function<void(const std::string&)> name_callback_t;

    void prompt_set_name(const std::string& prompt, const std::string& default_name, name_callback_t cb)
    {
        LLSD args;
        args["PROMPT"] = prompt;
        args["NAME"] = default_name;
        LLNotificationsUtil::add("GestureSetNamePrompt", args, LLSD(),
            [cb](const LLSD& notification, const LLSD& response)
            {
                if (LLNotificationsUtil::getSelectedOption(notification, response) != 0)
                {
                    return;
                }
                std::string name = response["new_name"].asString();
                LLStringUtil::trim(name);
                if (name.size() > MAX_SET_NAME_LENGTH)
                {
                    name.resize(MAX_SET_NAME_LENGTH);
                }
                if (name.empty())
                {
                    return;
                }
                if (SLGestureSets::instance().hasSet(name))
                {
                    LLSD exists_args;
                    exists_args["NAME"] = name;
                    LLNotificationsUtil::add("GestureSetNameExists", exists_args);
                    return;
                }
                cb(name);
            });
    }

    std::string key_string(KEY key, MASK mask)
    {
        return (key == KEY_NONE) ? std::string() : LLKeyboard::stringFromAccelerator(mask, key);
    }

    SLFloaterGestureSets* find_floater()
    {
        return LLFloaterReg::findTypedInstance<SLFloaterGestureSets>("gesture_sets");
    }
}

SLFloaterGestureSets::SLFloaterGestureSets(const LLSD& key)
    : LLFloater(key)
{
}

SLFloaterGestureSets::~SLFloaterGestureSets()
{
    if (LLGestureMgr::instanceExists())
    {
        LLGestureMgr::instance().removeObserver(this);
    }
}

bool SLFloaterGestureSets::postBuild()
{
    mSetList = getChild<LLScrollListCtrl>("set_list");
    mSetList->setCommitCallback(boost::bind(&SLFloaterGestureSets::onSetSelected, this));
    mSetList->setDoubleClickCallback(boost::bind(&SLFloaterGestureSets::onApply, this));

    mGestureList = getChild<LLScrollListCtrl>("gesture_list");
    mGestureList->setDoubleClickCallback(boost::bind(&SLFloaterGestureSets::onEditGesture, this));
    mFilterEdit = getChild<LLSearchEditor>("filter_edit");
    mFilterEdit->setKeystrokeCallback(boost::bind(&SLFloaterGestureSets::onFilterChanged, this));
    mFilterEdit->setCommitCallback(boost::bind(&SLFloaterGestureSets::onFilterChanged, this));
    mFilterCombo = getChild<LLComboBox>("filter_combo");
    mFilterCombo->setValue(LLSD("all"));
    mFilterCombo->setCommitCallback(boost::bind(&SLFloaterGestureSets::onFilterChanged, this));
    mModeRadio = getChild<LLRadioGroup>("mode_radio");
    mModeRadio->setCommitCallback(boost::bind(&SLFloaterGestureSets::onModeChanged, this));
    mConflictText = getChild<LLTextBox>("conflict_text");
    mStatusText = getChild<LLTextBox>("status_text");

    getChild<LLUICtrl>("new_btn")->setCommitCallback(boost::bind(&SLFloaterGestureSets::onNew, this));
    getChild<LLUICtrl>("save_current_btn")->setCommitCallback(boost::bind(&SLFloaterGestureSets::onSaveCurrent, this));
    getChild<LLUICtrl>("apply_btn")->setCommitCallback(boost::bind(&SLFloaterGestureSets::onApply, this));
    getChild<LLUICtrl>("rename_btn")->setCommitCallback(boost::bind(&SLFloaterGestureSets::onRename, this));
    getChild<LLUICtrl>("duplicate_btn")->setCommitCallback(boost::bind(&SLFloaterGestureSets::onDuplicate, this));
    getChild<LLUICtrl>("delete_btn")->setCommitCallback(boost::bind(&SLFloaterGestureSets::onDelete, this));
    getChild<LLUICtrl>("from_folders_btn")->setCommitCallback(boost::bind(&SLFloaterGestureSets::onCreateFromFolders, this));
    getChild<LLUICtrl>("check_shown_btn")->setCommitCallback(boost::bind(&SLFloaterGestureSets::onCheckShown, this, true));
    getChild<LLUICtrl>("uncheck_shown_btn")->setCommitCallback(boost::bind(&SLFloaterGestureSets::onCheckShown, this, false));

    mSetsConnection = SLGestureSets::instance().setChangedCallback([this]()
        {
            refreshSets();
            mGestureListDirty = true;
        });
    LLGestureMgr::instance().addObserver(this);

    refreshSets();
    refreshGestures();
    return true;
}

void SLFloaterGestureSets::onOpen(const LLSD& key)
{
    if (!SLGestureSets::isInventoryReady())
    {
        LLInventoryModelBackgroundFetch::instance().start();
    }
    mGestureListDirty = true;
}

void SLFloaterGestureSets::draw()
{
    if (mGestureListDirty && mRefreshTimer.getElapsedTimeF32() > LIST_REFRESH_INTERVAL)
    {
        refreshGestures();
    }
    LLFloater::draw();
}

void SLFloaterGestureSets::changed()
{
    mGestureListDirty = true;
}

std::string SLFloaterGestureSets::getSelectedSet() const
{
    LLScrollListItem* item = mSetList ? mSetList->getFirstSelected() : nullptr;
    return item ? item->getValue().asString() : std::string();
}

void SLFloaterGestureSets::selectSet(const std::string& name)
{
    mSetList->deselectAllItems(true);
    mSetList->selectByValue(LLSD(name));
    mSetList->scrollToShowSelected();
    onSetSelected();
}

void SLFloaterGestureSets::refreshSets()
{
    const std::string selected = getSelectedSet();
    const S32 scroll_pos = mSetList->getScrollPos();
    SLGestureSets& sets = SLGestureSets::instance();
    const std::string& last_applied = sets.getLastApplied();

    mSetList->deleteAllItems();
    for (const auto& [name, set] : sets.getSets())
    {
        LLSD row;
        row["value"] = name;
        row["columns"][0]["column"] = "applied";
        if (name == last_applied)
        {
            row["columns"][0]["type"] = "icon";
            row["columns"][0]["value"] = "Check_Mark";
        }
        row["columns"][1]["column"] = "name";
        row["columns"][1]["value"] = name;
        row["columns"][2]["column"] = "count";
        row["columns"][2]["value"] = llformat("%d", (S32)set.mItems.size());
        mSetList->addElement(row);
    }
    if (!selected.empty())
    {
        mSetList->selectByValue(LLSD(selected));
    }
    mSetList->setScrollPos(scroll_pos);
    refreshControls();
}

void SLFloaterGestureSets::refreshControls()
{
    SLGestureSets& sets = SLGestureSets::instance();
    const SLGestureSets::GestureSet* set = sets.getSet(getSelectedSet());
    const bool has_set = set != nullptr;

    getChildView("apply_btn")->setEnabled(has_set);
    getChildView("rename_btn")->setEnabled(has_set);
    getChildView("duplicate_btn")->setEnabled(has_set);
    getChildView("delete_btn")->setEnabled(has_set);
    getChildView("check_shown_btn")->setEnabled(has_set);
    getChildView("uncheck_shown_btn")->setEnabled(has_set);
    getChildView("from_folders_btn")->setEnabled(SLGestureSets::isInventoryReady());
    mModeRadio->setEnabled(has_set);
    mModeRadio->setSelectedIndex(has_set && set->mMode == SLGestureSets::MODE_ADD ? 1 : 0);

    std::string status;
    if (sets.isApplyInProgress())
    {
        LLStringUtil::format_map_t args;
        args["[NAME]"] = sets.getPendingName().empty() ? sets.getLastApplied() : sets.getPendingName();
        status = getString(SLGestureSets::isInventoryReady() ? "applying" : "waiting_inventory", args);
    }
    else if (!SLGestureSets::isInventoryReady())
    {
        status = getString("loading_inventory");
    }
    mStatusText->setText(status);

    refreshConflicts();
}

void SLFloaterGestureSets::refreshConflicts()
{
    const std::string set_name = getSelectedSet();
    if (set_name.empty())
    {
        mConflictText->setText(LLStringUtil::null);
        return;
    }

    const SLGestureSets::conflict_list_t conflicts = SLGestureSets::instance().findConflicts(set_name);
    if (conflicts.empty())
    {
        mConflictText->setText(LLStringUtil::null);
        return;
    }

    std::string text = getString("conflict_header");
    for (size_t i = 0; i < conflicts.size() && i < MAX_CONFLICT_LINES; ++i)
    {
        const SLGestureSets::Conflict& conflict = conflicts[i];
        std::string names;
        for (const std::string& name : conflict.mNames)
        {
            if (!names.empty())
            {
                names += ", ";
            }
            names += name;
        }
        LLStringUtil::format_map_t args;
        args["[WHAT]"] = conflict.mWhat;
        args["[NAMES]"] = names;
        text += "\n" + getString(conflict.mIsKey ? "conflict_key" : "conflict_trigger", args);
    }
    if (conflicts.size() > MAX_CONFLICT_LINES)
    {
        LLStringUtil::format_map_t args;
        args["[COUNT]"] = llformat("%d", (S32)(conflicts.size() - MAX_CONFLICT_LINES));
        text += "\n" + getString("conflict_more", args);
    }
    mConflictText->setText(text);
}

void SLFloaterGestureSets::refreshGestures()
{
    mGestureListDirty = false;
    mRefreshTimer.reset();

    SLGestureSets& sets = SLGestureSets::instance();
    const SLGestureSets::GestureSet* set = sets.getSet(getSelectedSet());
    const std::string filter_mode = mFilterCombo->getValue().asString();
    const LLGestureMgr::item_map_t& active = LLGestureMgr::instance().getActiveGestures();
    const std::string yes = getString("yes");
    const std::string no = getString("no");

    uuid_set_t assigned;
    if (filter_mode == "unassigned")
    {
        for (const auto& entry : sets.getSets())
        {
            assigned.insert(entry.second.mItems.begin(), entry.second.mItems.end());
        }
    }

    const S32 scroll_pos = mGestureList->getScrollPos();
    uuid_vec_t selected;
    for (LLScrollListItem* item : mGestureList->getAllSelected())
    {
        selected.push_back(item->getUUID());
    }

    mGestureList->deleteAllItems();
    const LLInventoryModel::item_array_t& gestures = sets.getInventoryGestures();
    S32 shown = 0;
    for (const LLPointer<LLViewerInventoryItem>& item : gestures)
    {
        const LLUUID& id = item->getUUID();
        LLGestureMgr::item_map_t::const_iterator active_it = active.find(id);
        const bool is_active = active_it != active.end();
        const bool is_member = set && set->mItems.count(id);

        if ((filter_mode == "active" && !is_active)
            || (filter_mode == "inactive" && is_active)
            || (filter_mode == "in_set" && !is_member)
            || (filter_mode == "not_in_set" && (!set || is_member))
            || (filter_mode == "unassigned" && assigned.count(id)))
        {
            continue;
        }

        std::string trigger;
        std::string shortcut;
        const LLMultiGesture* gesture = is_active ? active_it->second : nullptr;
        if (gesture)
        {
            trigger = gesture->mTrigger;
            shortcut = key_string(gesture->mKey, gesture->mMask);
        }
        else if (const SLGestureSets::GestureMeta* meta = sets.getMeta(id))
        {
            trigger = meta->mTrigger;
            shortcut = key_string(meta->mKey, meta->mMask);
        }

        const LLViewerInventoryCategory* cat = gInventory.getCategory(item->getParentUUID());
        const std::string folder = cat ? cat->getName() : std::string();

        if (!mFilter.empty())
        {
            std::string haystack = item->getName() + "\n" + trigger + "\n" + shortcut + "\n" + folder;
            LLStringUtil::toLower(haystack);
            if (haystack.find(mFilter) == std::string::npos)
            {
                continue;
            }
        }

        LLSD row;
        row["value"] = id;
        LLSD& columns = row["columns"];
        columns[0]["column"] = "member";
        columns[0]["type"] = "checkbox";
        columns[0]["value"] = is_member;
        columns[0]["enabled"] = set != nullptr;
        columns[1]["column"] = "name";
        columns[1]["value"] = item->getName();
        columns[2]["column"] = "trigger";
        columns[2]["value"] = trigger;
        columns[3]["column"] = "shortcut";
        columns[3]["value"] = shortcut;
        columns[4]["column"] = "folder";
        columns[4]["value"] = folder;
        columns[5]["column"] = "active";
        columns[5]["value"] = is_active ? yes : no;
        mGestureList->addElement(row, boost::bind(&SLFloaterGestureSets::onMemberToggled, this, _1, _2));
        ++shown;
    }

    for (const LLUUID& id : selected)
    {
        mGestureList->selectByID(id);
    }
    mGestureList->setScrollPos(scroll_pos);

    LLStringUtil::format_map_t args;
    args["[SHOWN]"] = llformat("%d", shown);
    args["[TOTAL]"] = llformat("%d", (S32)gestures.size());
    args["[MEMBERS]"] = llformat("%d", set ? (S32)set->mItems.size() : 0);
    getChild<LLTextBox>("count_text")->setText(getString(set ? "count_with_set" : "count", args));

    refreshControls();
}

void SLFloaterGestureSets::onSetSelected()
{
    refreshControls();
    mGestureListDirty = true;
    mRefreshTimer.reset();
    refreshGestures();
}

// static
void SLFloaterGestureSets::promptNewSet(const uuid_vec_t& item_ids, const std::string& default_name)
{
    prompt_set_name(LLTrans::getString("GestureSetPromptNew"), default_name, [item_ids](const std::string& name)
        {
            if (SLGestureSets::instance().createSet(name, item_ids))
            {
                if (SLFloaterGestureSets* floater = find_floater())
                {
                    floater->selectSet(name);
                }
            }
        });
}

void SLFloaterGestureSets::onNew()
{
    promptNewSet(uuid_vec_t(), LLTrans::getString("GestureSetDefaultNew"));
}

void SLFloaterGestureSets::onSaveCurrent()
{
    const std::string everything = LLTrans::getString("GestureSetDefaultEverything");
    promptNewSet(SLGestureSets::instance().getActiveIDs(),
                 SLGestureSets::instance().hasSet(everything) ? std::string() : everything);
}

void SLFloaterGestureSets::onApply()
{
    const std::string name = getSelectedSet();
    if (!name.empty())
    {
        SLGestureSets::instance().applySet(name);
    }
}

void SLFloaterGestureSets::onRename()
{
    const std::string old_name = getSelectedSet();
    if (old_name.empty())
    {
        return;
    }
    LLStringUtil::format_map_t prompt_args;
    prompt_args["[NAME]"] = old_name;
    prompt_set_name(LLTrans::getString("GestureSetPromptRename", prompt_args), old_name, [old_name](const std::string& name)
        {
            if (SLGestureSets::instance().renameSet(old_name, name))
            {
                if (SLFloaterGestureSets* floater = find_floater())
                {
                    floater->selectSet(name);
                }
            }
        });
}

void SLFloaterGestureSets::onDuplicate()
{
    const std::string src_name = getSelectedSet();
    if (src_name.empty())
    {
        return;
    }
    LLStringUtil::format_map_t args;
    args["[NAME]"] = src_name;
    prompt_set_name(LLTrans::getString("GestureSetPromptNew"), LLTrans::getString("GestureSetCopyName", args),
        [src_name](const std::string& name)
        {
            if (SLGestureSets::instance().duplicateSet(src_name, name))
            {
                if (SLFloaterGestureSets* floater = find_floater())
                {
                    floater->selectSet(name);
                }
            }
        });
}

void SLFloaterGestureSets::onDelete()
{
    const std::string name = getSelectedSet();
    if (name.empty())
    {
        return;
    }
    LLSD args;
    args["NAME"] = name;
    LLNotificationsUtil::add("GestureSetDeleteConfirm", args, LLSD(),
        [name](const LLSD& notification, const LLSD& response)
        {
            if (LLNotificationsUtil::getSelectedOption(notification, response) == 0)
            {
                SLGestureSets::instance().deleteSet(name);
            }
        });
}

void SLFloaterGestureSets::onModeChanged()
{
    const std::string name = getSelectedSet();
    if (!name.empty())
    {
        SLGestureSets::instance().setMode(name, mModeRadio->getSelectedIndex() == 1 ? SLGestureSets::MODE_ADD
                                                                                    : SLGestureSets::MODE_EXACT);
    }
}

void SLFloaterGestureSets::onCreateFromFolders()
{
    SLGestureSets& sets = SLGestureSets::instance();
    std::map<std::string, uuid_vec_t> to_create;
    uuid_set_t skipped_folders;
    for (const LLPointer<LLViewerInventoryItem>& item : sets.getInventoryGestures())
    {
        const LLViewerInventoryCategory* cat = gInventory.getCategory(item->getParentUUID());
        if (!cat || cat->getName().empty())
        {
            continue;
        }
        if (sets.hasSet(cat->getName()))
        {
            skipped_folders.insert(cat->getUUID());
            continue;
        }
        to_create[cat->getName()].push_back(item->getUUID());
    }

    if (to_create.empty())
    {
        LLNotificationsUtil::add("GestureSetsFromFoldersNone");
        return;
    }

    LLSD args;
    args["COUNT"] = (S32)to_create.size();
    args["SKIPPED"] = (S32)skipped_folders.size();
    LLNotificationsUtil::add("GestureSetsFromFoldersConfirm", args, LLSD(),
        [to_create](const LLSD& notification, const LLSD& response)
        {
            if (LLNotificationsUtil::getSelectedOption(notification, response) != 0)
            {
                return;
            }
            for (const auto& [name, ids] : to_create)
            {
                SLGestureSets::instance().createSet(name, ids);
            }
        });
}

void SLFloaterGestureSets::onFilterChanged()
{
    mFilter = mFilterEdit->getText();
    LLStringUtil::trim(mFilter);
    LLStringUtil::toLower(mFilter);
    refreshGestures();
}

void SLFloaterGestureSets::onCheckShown(bool check)
{
    const std::string name = getSelectedSet();
    if (name.empty())
    {
        return;
    }
    uuid_vec_t ids;
    for (LLScrollListItem* item : mGestureList->getAllData())
    {
        ids.push_back(item->getUUID());
    }
    SLGestureSets::instance().setMembers(name, ids, check);
}

void SLFloaterGestureSets::onMemberToggled(const LLSD& value, LLScrollListCell* cell)
{
    const std::string name = getSelectedSet();
    if (name.empty() || !cell)
    {
        return;
    }
    // Deferred: the change signal rebuilds the list that owns this cell
    const uuid_vec_t ids{ value.asUUID() };
    const bool member = cell->getValue().asBoolean();
    doOnIdleOneTime([name, ids, member]()
        {
            SLGestureSets::instance().setMembers(name, ids, member);
        });
}

void SLFloaterGestureSets::onEditGesture()
{
    const LLUUID item_id = mGestureList->getCurrentID();
    if (item_id.isNull() || !gInventory.getItem(item_id))
    {
        return;
    }
    LLPreviewGesture* previewp = LLPreviewGesture::show(item_id, LLUUID::null);
    if (previewp && !previewp->getHost())
    {
        previewp->setRect(gFloaterView->findNeighboringPosition(this, previewp));
    }
}
