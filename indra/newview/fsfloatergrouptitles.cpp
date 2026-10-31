/**
 * @file fsfloatergrouptitles.cpp
 * @brief Group title overview and changer
 *
 * $LicenseInfo:firstyear=2012&license=viewerlgpl$
 * Copyright (c) 2012 Ansariel Hiller @ Second Life
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
 * The Phoenix Firestorm Project, Inc., 1831 Oakwood Drive, Fairmont, Minnesota 56031-3225 USA
 * http://www.firestormviewer.org
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "fsfloatergrouptitles.h"
#include "slkeycommands.h"
#include "fsgrouptitleregionmgr.h"
#include "llcallbacklist.h"
#include "llcheckboxctrl.h"
#include "lleventtimer.h"
#include "llfiltereditor.h"
#include "llgroupactions.h"
#include "llmenugl.h"
#include "llscrolllistctrl.h"
#include "lltrans.h"
#include "llviewercontrol.h"
#include "llviewermenu.h"

// #868: titles requests out at once (Premium Plus allows 140 groups), how long one may take before
// its group shows a retry, and how long titles already fetched are reused instead of asked again
static constexpr S32 MAX_TITLE_REQUESTS_IN_FLIGHT = 4;
static constexpr F64 TITLES_REQUEST_TIMEOUT       = 20.0;
static constexpr F64 TITLES_FRESH_SECONDS         = 300.0;

/////////////////////////////////////////////////////
// FSGroupTitlesObserver class
//
FSGroupTitlesObserver::FSGroupTitlesObserver(const LLUUID& group_id, LLHandle<FSFloaterGroupTitles> parent) :
    LLGroupMgrObserver(group_id),
    mParent(parent)
{
    LLGroupMgr::instance().addObserver(this);
}

FSGroupTitlesObserver::~FSGroupTitlesObserver()
{
    LLGroupMgr::instance().removeObserver(this);
}

void FSGroupTitlesObserver::changed(LLGroupChange gc)
{
    if (gc == GC_TITLES && !mParent.isDead())
    {
        mParent.get()->processGroupTitleResults(mID);
    }
    else if ((gc == GC_ROLE_DATA || gc == GC_ROLE_MEMBER_DATA || gc == GC_MEMBER_DATA) && !mParent.isDead())
    {
        mParent.get()->invalidateGroupTitles(mID);
    }
}


/////////////////////////////////////////////////////
// FSGroupTitles class
//
FSFloaterGroupTitles::FSFloaterGroupTitles(const LLSD& key) :
    LLFloater(key),
    mFilterSubString(LLStringUtil::null),
    mFilterSubStringOrig(LLStringUtil::null)
{
    // #868: FS fires its own "update grouptitle list" event from llagent.cpp. Here the agent's
    // "new group" event (group list, active group and title updates) schedules a sync on idle, as
    // it fires once per group on a group list update. Titles replies come through the per-group
    // FSGroupTitlesObserver.
    gAgent.addListener(this, "new group");
}

FSFloaterGroupTitles::~FSFloaterGroupTitles()
{
    gAgent.removeListener(this);
    delete mRequestTimer;

    if (mAssignmentsChangedConnection.connected())
    {
        mAssignmentsChangedConnection.disconnect();
    }

    if (LLContextMenu* menu = mClearRegionMenuHandle.get())
    {
        gMenuHolder->removeChild(menu);
        delete menu;
    }

    // Clean up still registered observers
    mGroupStates.clear();
}

bool FSFloaterGroupTitles::postBuild()
{
    mActivateButton = getChild<LLButton>("btnActivate");
    mRefreshButton = getChild<LLButton>("btnRefresh");
    mInfoButton = getChild<LLButton>("btnInfo");
    mSetRegionButton = getChild<LLButton>("btnSetRegion");
    mSetRegionManualButton = getChild<LLButton>("btnSetRegionManual");
    mClearRegionButton = getChild<LLButton>("btnClearRegion");
    mSetDefaultButton = getChild<LLButton>("btnSetDefault");
    mUseDefaultCheck = getChild<LLCheckBoxCtrl>("use_default_title");
    mTitleList = getChild<LLScrollListCtrl>("title_list");
    mFilterEditor = getChild<LLFilterEditor>("filter_input");

    mActivateButton->setCommitCallback(boost::bind(&FSFloaterGroupTitles::activateGroupTitle, this));
    mRefreshButton->setCommitCallback(boost::bind(&FSFloaterGroupTitles::refreshGroupTitles, this, true));
    mInfoButton->setCommitCallback(boost::bind(&FSFloaterGroupTitles::openGroupInfo, this));
    mSetRegionButton->setCommitCallback(boost::bind(&FSFloaterGroupTitles::onSetRegion, this));
    mSetRegionManualButton->setCommitCallback(boost::bind(&FSFloaterGroupTitles::onSetRegionManual, this));
    mClearRegionButton->setCommitCallback(boost::bind(&FSFloaterGroupTitles::onClearRegion, this));
    mSetDefaultButton->setCommitCallback(boost::bind(&FSFloaterGroupTitles::onSetAsDefault, this));
    mUseDefaultCheck->setCommitCallback(boost::bind(&FSFloaterGroupTitles::onUseDefaultToggle, this));
    mTitleList->setDoubleClickCallback(boost::bind(&FSFloaterGroupTitles::activateGroupTitle, this));
    mTitleList->setCommitCallback(boost::bind(&FSFloaterGroupTitles::selectedTitleChanged, this));
    mFilterEditor->setCommitCallback(boost::bind(&FSFloaterGroupTitles::onFilterEdit, this, _2));

    mSetRegionButton->setEnabled(false);
    mSetRegionManualButton->setEnabled(false);
    mClearRegionButton->setEnabled(false);
    mSetDefaultButton->setEnabled(false);
    mUseDefaultCheck->set(FSGroupTitleRegionMgr::getInstance()->getEnabled());
    mAssignmentsChangedConnection = FSGroupTitleRegionMgr::getInstance()->setAssignmentsChangedCallback([this]() { updateRegionColumn(); });

    // #868: the sort order (set by a header click) is kept in settings
    const std::string sort_column = gSavedSettings.getString("SlipstreamGroupTitlesSortColumn");
    mTitleList->sortByColumn(mTitleList->getColumn(sort_column) ? sort_column : "title_sort_column",
                             gSavedSettings.getBOOL("SlipstreamGroupTitlesSortAscending"));
    mTitleList->setSortChangedCallback(boost::bind(&FSFloaterGroupTitles::onSortChanged, this));

    mRequestTimer = LLEventTimer::run_every(1.f, [this]() { checkRequestTimeouts(); });

    refreshGroupTitles(false);

    return true;
}

void FSFloaterGroupTitles::onOpen(const LLSD& key)
{
    LLFloater::onOpen(key);

    mTitleList->setFocus(true);
}

bool FSFloaterGroupTitles::handleKeyHere(KEY key, MASK mask)
{
    // Ctrl+F focusses the filter (FS: FSCommon::isFilterEditorKeyCombo)
    if (SLKeyCommands::instance().matches("find_in_floater", key, mask))
    {
        mFilterEditor->setFocus(true);
        return true;
    }

    return LLFloater::handleKeyHere(key, mask);
}

bool FSFloaterGroupTitles::handleEvent(LLPointer<LLOldEvents::LLEvent> event, const LLSD& userdata)
{
    if (event->desc() == "new group")
    {
        scheduleSync();
    }
    return false;
}

void FSFloaterGroupTitles::scheduleSync()
{
    if (mSyncScheduled)
    {
        return;
    }
    mSyncScheduled = true;

    LLHandle<FSFloaterGroupTitles> handle = getDerivedHandle<FSFloaterGroupTitles>();
    doOnIdleOneTime([handle]()
    {
        if (FSFloaterGroupTitles* floater = handle.get())
        {
            floater->syncGroups();
        }
    });
}

// #868: follow group joins and leaves, role changes and active group or title changes made
// elsewhere (group profile, chat command, RLV) without reopening
void FSFloaterGroupTitles::syncGroups()
{
    mSyncScheduled = false;

    for (group_state_map_t::iterator it = mGroupStates.begin(); it != mGroupStates.end(); )
    {
        if (gAgent.isInGroup(it->first, true))
        {
            ++it;
            continue;
        }
        removeGroupRows(it->first);
        mRequestQueue.erase(std::remove(mRequestQueue.begin(), mRequestQueue.end(), it->first), mRequestQueue.end());
        it = mGroupStates.erase(it);
    }

    // A role given or taken shows as a change in the group's powers (one that leaves them as they
    // were only shows on Refresh)
    for (const LLGroupData& group_data : gAgent.mGroups)
    {
        group_state_map_t::iterator found_it = mGroupStates.find(group_data.mID);
        if (found_it == mGroupStates.end())
        {
            addGroup(group_data, false);
        }
        else if (found_it->second.mPowers != group_data.mPowers)
        {
            found_it->second.mPowers = group_data.mPowers;
            requestTitles(group_data.mID, true);
        }
    }
    sendQueuedRequests();

    updateActiveFlags();
    selectedTitleChanged();
}

void FSFloaterGroupTitles::updateActiveFlags()
{
    const LLUUID& active_group_id = gAgent.getGroupID();
    bool changed = false;
    for (TitleEntry& entry : mEntries)
    {
        bool is_active = false;
        if (!entry.mIsGroup)
        {
            is_active = active_group_id.isNull();
        }
        else if (entry.mGroupID == active_group_id && getTitlesState(entry.mGroupID) == ETitlesState::LOADED)
        {
            // The selection sendGroupTitleUpdate() saved, or the last titles reply's; while titles
            // are being fetched again (cleared) the row keeps its state
            const LLGroupMgrGroupData* gmgr_data = LLGroupMgr::getInstance()->getGroupData(entry.mGroupID);
            if (!gmgr_data || !gmgr_data->hasTitlesReply())
            {
                is_active = entry.mIsActive;
            }
            else
            {
                is_active = gmgr_data->mTitles.empty(); // only the blank-title row
                for (const LLGroupTitle& group_title : gmgr_data->mTitles)
                {
                    if (group_title.mRoleID == entry.mRoleID)
                    {
                        is_active = group_title.mSelected;
                        break;
                    }
                }
            }
        }

        if (is_active != entry.mIsActive)
        {
            entry.mIsActive = is_active;
            changed = true;
        }
    }

    if (changed)
    {
        const S32 scroll_pos = mTitleList->getScrollPos();
        rebuildList();
        mTitleList->setScrollPos(scroll_pos);
    }
}

void FSFloaterGroupTitles::addListItem(const LLUUID& group_id, const LLUUID& role_id, const std::string& title,
    const std::string& group_name, bool is_active, bool is_group)
{
    TitleEntry entry{ group_id, role_id, title, group_name, is_active, is_group };
    mEntries.push_back(entry);
    addListRow(entry);
}

void FSFloaterGroupTitles::addListRow(const TitleEntry& entry)
{
    const LLUUID& group_id = entry.mGroupID;
    const LLUUID& role_id = entry.mRoleID;
    const std::string& title = entry.mTitle;
    const std::string& group_name = entry.mGroupName;
    const bool is_active = entry.mIsActive;
    const bool is_group = entry.mIsGroup;
    // #868: a group's "loading" or "retry" row, sorted after the titles
    const bool is_placeholder = is_group && getTitlesState(group_id) != ETitlesState::LOADED;

    std::string font_style = (is_active ? "BOLD" : (is_placeholder ? "ITALIC" : "NORMAL"));
    std::string region_name = FSGroupTitleRegionMgr::getInstance()->getRegionForTitle(group_id, role_id);

    if (!mFilterSubString.empty())
    {
        std::string filter_text = title + " " + group_name + " " + region_name;
        LLStringUtil::toUpper(filter_text);
        if (filter_text.find(mFilterSubString) == std::string::npos)
        {
            return;
        }
    }

    LLSD item;
    item["id"] = group_id.asString() + role_id.asString(); // Only combination of group id and role id is unique!
    item["columns"][0]["column"] = "grouptitle";
    item["columns"][0]["type"] = "text";
    item["columns"][0]["font"]["style"] = font_style;
    item["columns"][0]["value"] = title;
    item["columns"][1]["column"] = "groupname";
    item["columns"][1]["type"] = "text";
    item["columns"][1]["font"]["style"] = font_style;
    item["columns"][1]["value"] = group_name;
    item["columns"][2]["column"] = "regionname";
    item["columns"][2]["type"] = "text";
    item["columns"][2]["font"]["style"] = font_style;
    item["columns"][2]["value"] = region_name;
    item["columns"][3]["column"] = "role_id";
    item["columns"][3]["type"] = "text";
    item["columns"][3]["value"] = role_id;
    item["columns"][4]["column"] = "group_id";
    item["columns"][4]["type"] = "text";
    item["columns"][4]["value"] = group_id;
    item["columns"][5]["column"] = "title_sort_column";
    item["columns"][5]["type"] = "text";
    item["columns"][5]["value"] = (is_group ? ((is_placeholder ? "2_" : "1_") + title) : "0");
    item["columns"][6]["column"] = "name_sort_column";
    item["columns"][6]["type"] = "text";
    item["columns"][6]["value"] = (is_group ? ("1_" + group_name) : "0");
    item["columns"][7]["column"] = "filter_text";
    item["columns"][7]["type"] = "text";
    item["columns"][7]["value"] = title + " " + group_name + " " + region_name;
    item["columns"][8]["column"] = "default_marker";
    item["columns"][8]["type"] = "icon";
    item["columns"][8]["halign"] = "center";
    item["columns"][8]["value"] = LLStringUtil::null;

    mTitleList->addElement(item);

    // Need to do use the selectByValue method or there would be multiple
    // selections on login.
    // #868: only while nothing is selected, as rows now arrive group by group
    if (is_active && !mTitleList->getFirstSelected())
    {
        mTitleList->selectByValue(group_id.asString() + role_id.asString());
        mTitleList->scrollToShowSelected();
    }
}

void FSFloaterGroupTitles::rebuildList()
{
    const LLSD selected_value = mTitleList->getSelectedValue();
    mTitleList->clearRows();
    for (const TitleEntry& entry : mEntries)
    {
        addListRow(entry);
    }
    if (selected_value.isDefined())
    {
        mTitleList->selectByValue(selected_value);
    }
    updateDefaultColumn();
    selectedTitleChanged();
}

void FSFloaterGroupTitles::invalidateGroupTitles(const LLUUID& group_id)
{
    if (LLGroupMgrGroupData* data = LLGroupMgr::getInstance()->getGroupData(group_id))
    {
        data->invalidateTitlesCache();
    }
    mInvalidatedGroups.insert(group_id);
    if (mInvalidationScheduled) return;
    mInvalidationScheduled = true;
    LLHandle<FSFloaterGroupTitles> handle = getDerivedHandle<FSFloaterGroupTitles>();
    doOnIdleOneTime([handle]()
    {
        if (FSFloaterGroupTitles* floater = handle.get())
        {
            floater->mInvalidationScheduled = false;
            std::set<LLUUID> groups;
            groups.swap(floater->mInvalidatedGroups);
            for (const LLUUID& id : groups) floater->requestTitles(id, true);
            floater->sendQueuedRequests();
        }
    });
}

void FSFloaterGroupTitles::processGroupTitleResults(const LLUUID& group_id)
{
    // #868: GC_TITLES can also come for a group whose titles were just cleared for a new request
    group_state_map_t::iterator found_it = mGroupStates.find(group_id);
    const LLGroupMgrGroupData* gmgr_data = LLGroupMgr::getInstance()->getGroupData(group_id);
    if (found_it == mGroupStates.end() || !gmgr_data || gmgr_data->isTitlesReplyPending())
    {
        return;
    }
    if (found_it->second.mState == ETitlesState::QUEUED) return;
    if (!gmgr_data->hasTitlesReply())
    {
        // A role/member change invalidated this reply while it was in flight.
        if (found_it->second.mState == ETitlesState::LOADING)
        {
            mInvalidatedGroups.erase(group_id);
            found_it->second.mRefreshAfterReply = false;
            found_it->second.mState = ETitlesState::FAILED;
            requestTitles(group_id, true);
            sendQueuedRequests();
        }
        return;
    }


    if (found_it->second.mRefreshAfterReply)
    {
        found_it->second.mRefreshAfterReply = false;
        found_it->second.mState = ETitlesState::FAILED;
        requestTitles(group_id, true);
    }
    else
    {
        found_it->second.mState = ETitlesState::LOADED;
        setGroupRows(group_id);
    }
    sendQueuedRequests();
}

void FSFloaterGroupTitles::addGroup(const LLGroupData& group_data, bool force)
{
    GroupTitlesState& state = mGroupStates[group_data.mID];
    state.mName = group_data.mName;
    state.mPowers = group_data.mPowers;
    // Kept while the floater is open: titles fetched elsewhere (a group profile) update the rows,
    // and LLGroupMgr doesn't evict an observed group's data
    state.mObserver = std::make_unique<FSGroupTitlesObserver>(group_data.mID, getDerivedHandle<FSFloaterGroupTitles>());
    requestTitles(group_data.mID, force);
}

void FSFloaterGroupTitles::requestTitles(const LLUUID& group_id, bool force)
{
    group_state_map_t::iterator found_it = mGroupStates.find(group_id);
    if (found_it == mGroupStates.end())
    {
        return;
    }

    if (found_it->second.mState == ETitlesState::LOADING)
    {
        if (force) found_it->second.mRefreshAfterReply = true;
        return;
    }
    const LLGroupMgrGroupData* gmgr_data = LLGroupMgr::getInstance()->getGroupData(group_id);
    if (!force && gmgr_data && gmgr_data->hasTitlesReply()
        && LLFrameTimer::getElapsedSeconds() - gmgr_data->getTitlesRequestTime() < TITLES_FRESH_SECONDS)
    {
        found_it->second.mState = ETitlesState::LOADED;
    }
    else
    {
        found_it->second.mState = ETitlesState::QUEUED;
        if (std::find(mRequestQueue.begin(), mRequestQueue.end(), group_id) == mRequestQueue.end())
        {
            mRequestQueue.push_back(group_id);
        }
    }
    setGroupRows(group_id);
}

void FSFloaterGroupTitles::sendQueuedRequests()
{
    S32 in_flight = 0;
    for (const auto& [group_id, state] : mGroupStates)
    {
        if (state.mState == ETitlesState::LOADING)
        {
            ++in_flight;
        }
    }

    while (in_flight < MAX_TITLE_REQUESTS_IN_FLIGHT && !mRequestQueue.empty())
    {
        const LLUUID group_id = mRequestQueue.front();
        mRequestQueue.pop_front();

        // Skip groups left, and titles that arrived (or were queued again) meanwhile
        group_state_map_t::iterator found_it = mGroupStates.find(group_id);
        if (found_it == mGroupStates.end() || found_it->second.mState != ETitlesState::QUEUED)
        {
            continue;
        }

        found_it->second.mState = ETitlesState::LOADING;
        found_it->second.mRequestTime = LLFrameTimer::getElapsedSeconds();
        LLGroupMgr::getInstance()->sendGroupTitlesRequest(group_id);
        ++in_flight;
    }
}

void FSFloaterGroupTitles::checkRequestTimeouts()
{
    const F64 now = LLFrameTimer::getElapsedSeconds();
    bool timed_out = false;
    for (auto& [group_id, state] : mGroupStates)
    {
        if (state.mState == ETitlesState::LOADING && now - state.mRequestTime > TITLES_REQUEST_TIMEOUT)
        {
            // A late reply still fills the group in
            LL_WARNS("GrpMgr") << "Titles request for group " << group_id << " timed out" << LL_ENDL;
            state.mState = ETitlesState::FAILED;
            if (state.mRefreshAfterReply)
            {
                state.mRefreshAfterReply = false;
                requestTitles(group_id, true);
            }
            else setGroupRows(group_id);
            timed_out = true;
        }
    }

    if (timed_out)
    {
        sendQueuedRequests();
    }
}

void FSFloaterGroupTitles::removeGroupRows(const LLUUID& group_id)
{
    mEntries.erase(std::remove_if(mEntries.begin(), mEntries.end(),
                                  [&group_id](const TitleEntry& entry) { return entry.mGroupID == group_id; }),
                   mEntries.end());

    const S32 group_id_col_idx = mTitleList->getColumn("group_id")->mIndex;
    for (LLScrollListItem* item : mTitleList->getAllData())
    {
        if (item->getColumn(group_id_col_idx)->getValue().asUUID() == group_id)
        {
            mTitleList->deleteSingleItem(item);
        }
    }
}

void FSFloaterGroupTitles::setGroupRows(const LLUUID& group_id)
{
    group_state_map_t::const_iterator found_it = mGroupStates.find(group_id);
    if (found_it == mGroupStates.end())
    {
        return;
    }
    const GroupTitlesState& state = found_it->second;

    const LLSD selected_value = mTitleList->getSelectedValue();
    removeGroupRows(group_id);

    if (state.mState != ETitlesState::LOADED)
    {
        // One row for the group; activating it retries a failed request
        addListItem(group_id, LLUUID::null, getString(state.mState == ETitlesState::FAILED ? "TitlesFailed" : "TitlesLoading"),
                    state.mName, false);
    }
    else
    {
        const bool is_active_group = (group_id == gAgent.getGroupID());
        const LLGroupMgrGroupData* gmgr_data = LLGroupMgr::getInstance()->getGroupData(group_id);
        if (gmgr_data && !gmgr_data->mTitles.empty())
        {
            for (const LLGroupTitle& group_title : gmgr_data->mTitles)
            {
                addListItem(group_id, group_title.mRoleID, group_title.mTitle, state.mName, is_active_group && group_title.mSelected);
            }
        }
        else
        {
            // Every title held is blank (the reply leaves those out): the group can still be worn
            addListItem(group_id, LLUUID::null, getString("BlankTitle"), state.mName, is_active_group);
        }
    }

    // A title of this group that was selected stays selected when titles are fetched again
    if (selected_value.isDefined() && mTitleList->getItem(selected_value))
    {
        mTitleList->selectByValue(selected_value);
    }

    updateDefaultColumn();
    selectedTitleChanged();
}

FSFloaterGroupTitles::ETitlesState FSFloaterGroupTitles::getTitlesState(const LLUUID& group_id) const
{
    group_state_map_t::const_iterator found_it = mGroupStates.find(group_id);
    return (found_it != mGroupStates.end() ? found_it->second.mState : ETitlesState::LOADED);
}

void FSFloaterGroupTitles::activateGroupTitle()
{
    LLScrollListItem* selected_item = mTitleList->getFirstSelected();
    if (selected_item)
    {
        LLUUID group_id = selected_item->getColumn(mTitleList->getColumn("group_id")->mIndex)->getValue().asUUID();
        LLUUID role_id = selected_item->getColumn(mTitleList->getColumn("role_id")->mIndex)->getValue().asUUID();

        // #868: a group's placeholder row: retry its titles if they didn't load
        const ETitlesState state = getTitlesState(group_id);
        if (state != ETitlesState::LOADED)
        {
            if (state == ETitlesState::FAILED)
            {
                requestTitles(group_id, true);
                sendQueuedRequests();
            }
            return;
        }

        // #868: FS sent the title update and then ActivateGroup; the region-title manager now
        // activates first and sends the title once the group is active ("no group" has no title)
        FSGroupTitleRegionMgr::getInstance()->applyGroupTitle(group_id, role_id);

        // A title change in the already active group is saved locally right away
        updateActiveFlags();
    }
}

void FSFloaterGroupTitles::refreshGroupTitles(bool force)
{
    if (!mGroupStates.empty())
    {
        for (const LLGroupData& group_data : gAgent.mGroups)
        {
            if (mGroupStates.find(group_data.mID) == mGroupStates.end()) addGroup(group_data, force);
            else requestTitles(group_data.mID, force);
        }
        sendQueuedRequests();
        return;
    }
    mRequestQueue.clear();
    mTitleList->clearRows();
    mEntries.clear();

    // Add "no group"
    addListItem(LLUUID::null, LLUUID::null, getString("NoGroupTitle"), LLTrans::getString("GroupsNone"), gAgent.getGroupID().isNull(), false);

    // #868: each group gets a row at once ("loading" until its titles arrive); titles fetched in the
    // last few minutes are reused unless refreshing
    for (const LLGroupData& group_data : gAgent.mGroups)
    {
        addGroup(group_data, force);
    }
    sendQueuedRequests();

    updateDefaultColumn();
}

void FSFloaterGroupTitles::onSortChanged()
{
    // getSortColumnName() is the sort key column (title_sort_column for the Title header)
    const std::string sort_column = mTitleList->getSortColumnName();
    if (!sort_column.empty())
    {
        gSavedSettings.setString("SlipstreamGroupTitlesSortColumn", sort_column);
        gSavedSettings.setBOOL("SlipstreamGroupTitlesSortAscending", mTitleList->getSortAscending());
    }
}

void FSFloaterGroupTitles::selectedTitleChanged()
{
    LLScrollListItem* selected_item = mTitleList->getFirstSelected();
    if (selected_item)
    {
        LLUUID group_id = selected_item->getColumn(mTitleList->getColumn("group_id")->mIndex)->getValue().asUUID();
        LLUUID role_id  = selected_item->getColumn(mTitleList->getColumn("role_id")->mIndex)->getValue().asUUID();
        // #868: a placeholder row has no title to assign; only a failed one activates (a retry)
        const ETitlesState state = getTitlesState(group_id);
        const bool is_title = (state == ETitlesState::LOADED);
        mActivateButton->setEnabled(is_title || state == ETitlesState::FAILED);
        mInfoButton->setEnabled(group_id.notNull());
        const bool has_region = !FSGroupTitleRegionMgr::getInstance()->getRegionForTitle(group_id, role_id).empty();
        mSetRegionButton->setEnabled(is_title);
        mSetRegionManualButton->setEnabled(is_title);
        mClearRegionButton->setEnabled(is_title && has_region);
        mSetDefaultButton->setEnabled(is_title);
    }
    else
    {
        mActivateButton->setEnabled(false);
        mInfoButton->setEnabled(false);
        mSetRegionButton->setEnabled(false);
        mSetRegionManualButton->setEnabled(false);
        mClearRegionButton->setEnabled(false);
        mSetDefaultButton->setEnabled(false);
    }
}

void FSFloaterGroupTitles::openGroupInfo()
{
    LLScrollListItem* selected_item = mTitleList->getFirstSelected();
    if (selected_item)
    {
        LLUUID group_id = selected_item->getColumn(mTitleList->getColumn("group_id")->mIndex)->getValue().asUUID();
        LLGroupActions::show(group_id);
    }
}

void FSFloaterGroupTitles::onFilterEdit(const std::string& search_string)
{
    mFilterSubStringOrig = search_string;
    LLStringUtil::trimHead(mFilterSubStringOrig);
    // Searches are case-insensitive
    std::string search_upper = mFilterSubStringOrig;
    LLStringUtil::toUpper(search_upper);

    if (mFilterSubString == search_upper)
    {
        return;
    }

    mFilterSubString = search_upper;

    // Apply new filter: rebuild the visible rows from the cached entries
    rebuildList();
}

void FSFloaterGroupTitles::onSetRegion()
{
    auto* selected_item = mTitleList->getFirstSelected();
    if (!selected_item)
    {
        return;
    }
    const auto group_id = selected_item->getColumn(mTitleList->getColumn("group_id")->mIndex)->getValue().asUUID();
    const auto role_id  = selected_item->getColumn(mTitleList->getColumn("role_id")->mIndex)->getValue().asUUID();
    FSGroupTitleRegionMgr::getInstance()->setAssignmentForCurrentRegion(group_id, role_id);
    selectedTitleChanged();
}

void FSFloaterGroupTitles::onSetRegionManual()
{
    auto* selected_item = mTitleList->getFirstSelected();
    if (!selected_item)
    {
        return;
    }
    const auto group_id = selected_item->getColumn(mTitleList->getColumn("group_id")->mIndex)->getValue().asUUID();
    const auto role_id  = selected_item->getColumn(mTitleList->getColumn("role_id")->mIndex)->getValue().asUUID();
    FSGroupTitleRegionMgr::getInstance()->showRegionInputDialog(group_id, role_id);
}

void FSFloaterGroupTitles::onClearRegion()
{
    auto* selected_item = mTitleList->getFirstSelected();
    if (!selected_item)
    {
        return;
    }
    const auto group_id = selected_item->getColumn(mTitleList->getColumn("group_id")->mIndex)->getValue().asUUID();
    const auto role_id  = selected_item->getColumn(mTitleList->getColumn("role_id")->mIndex)->getValue().asUUID();
    auto* mgr = FSGroupTitleRegionMgr::getInstance();
    std::vector<std::string> regions = mgr->getRegionDisplayNamesForTitle(group_id, role_id);

    if (regions.empty())
    {
        return;
    }

    if (regions.size() == 1)
    {
        mgr->clearAssignmentByRegion(regions[0]);
        selectedTitleChanged();
        return;
    }

    // Multiple regions, build a popup menu
    if (LLContextMenu* old_menu = mClearRegionMenuHandle.get())
    {
        gMenuHolder->removeChild(old_menu);
        delete old_menu;
    }

    LLContextMenu::Params menu_params;
    menu_params.name("clear_region_menu");
    menu_params.visible(false);
    LLContextMenu* menu = LLUICtrlFactory::create<LLContextMenu>(menu_params);

    for (const auto& region : regions)
    {
        LLMenuItemCallGL::Params item_params;
        item_params.label(region);
        item_params.name(region);
        item_params.on_click.function(
            [mgr, region](LLUICtrl*, const LLSD&)
            {
                mgr->clearAssignmentByRegion(region);
            });
        menu->addChild(LLUICtrlFactory::create<LLMenuItemCallGL>(item_params));
    }

    menu->addSeparator();

    LLMenuItemCallGL::Params clear_all_params;
    clear_all_params.label(getString("ClearAllRegions"));
    clear_all_params.name("clear_all_regions");
    clear_all_params.on_click.function(
        [mgr, group_id, role_id](LLUICtrl*, const LLSD&)
        {
            mgr->clearAssignment(group_id, role_id);
        });
    menu->addChild(LLUICtrlFactory::create<LLMenuItemCallGL>(clear_all_params));

    mClearRegionMenuHandle = menu->getHandle();
    gMenuHolder->addChild(menu);
    S32 screen_x, screen_y;
    mClearRegionButton->localPointToScreen(0, mClearRegionButton->getRect().getHeight(), &screen_x, &screen_y);
    menu->show(screen_x, screen_y, mClearRegionButton);
}

void FSFloaterGroupTitles::onUseDefaultToggle()
{
    FSGroupTitleRegionMgr::getInstance()->setEnabled(mUseDefaultCheck->get());
    updateDefaultColumn();
}

void FSFloaterGroupTitles::onSetAsDefault()
{
    LLScrollListItem* selected_item = mTitleList->getFirstSelected();
    if (!selected_item)
    {
        return;
    }
    const LLUUID group_id = selected_item->getColumn(mTitleList->getColumn("group_id")->mIndex)->getValue().asUUID();
    const LLUUID role_id  = selected_item->getColumn(mTitleList->getColumn("role_id")->mIndex)->getValue().asUUID();
    auto* mgr = FSGroupTitleRegionMgr::getInstance();
    mgr->setDefaultTitle(group_id, role_id);
    mgr->setEnabled(true);
    mUseDefaultCheck->set(true);
    updateDefaultColumn();
}

void FSFloaterGroupTitles::updateDefaultColumn()
{
    LLScrollListColumn* default_col = mTitleList->getColumn("default_marker");
    if (!default_col)
    {
        return;
    }

    const auto default_col_idx  = default_col->mIndex;
    const auto group_id_col_idx = mTitleList->getColumn("group_id")->mIndex;
    const auto role_id_col_idx  = mTitleList->getColumn("role_id")->mIndex;

    auto* mgr = FSGroupTitleRegionMgr::getInstance();
    const bool enabled = mgr->getEnabled();
    const LLUUID default_group = mgr->getDefaultGroupID();
    const LLUUID default_role = mgr->getDefaultRoleID();

    for (auto* item : mTitleList->getAllData())
    {
        auto* cell = item->getColumn(default_col_idx);
        if (!cell)
        {
            continue;
        }
        const LLUUID group_id = item->getColumn(group_id_col_idx)->getValue().asUUID();
        const LLUUID role_id = item->getColumn(role_id_col_idx)->getValue().asUUID();
        const bool is_default = enabled && group_id == default_group && role_id == default_role;
        cell->setValue(is_default ? LLSD("Check_Mark") : LLSD(LLStringUtil::null));
    }
}

void FSFloaterGroupTitles::updateRegionColumn()
{
    const auto region_col_idx   = mTitleList->getColumn("regionname")->mIndex;
    const auto title_col_idx    = mTitleList->getColumn("grouptitle")->mIndex;
    const auto group_col_idx    = mTitleList->getColumn("groupname")->mIndex;
    const auto filter_col_idx   = mTitleList->getColumn("filter_text")->mIndex;
    const auto group_id_col_idx = mTitleList->getColumn("group_id")->mIndex;
    const auto role_id_col_idx  = mTitleList->getColumn("role_id")->mIndex;
    auto* mgr = FSGroupTitleRegionMgr::getInstance();
    for (auto* item : mTitleList->getAllData())
    {
        const auto group_id = item->getColumn(group_id_col_idx)->getValue().asUUID();
        const auto role_id  = item->getColumn(role_id_col_idx)->getValue().asUUID();
        const std::string region_name = mgr->getRegionForTitle(group_id, role_id);

        if (auto* cell = item->getColumn(region_col_idx))
        {
            cell->setValue(region_name);
        }

        if (auto* filter_cell = item->getColumn(filter_col_idx))
        {
            const std::string title = item->getColumn(title_col_idx)->getValue().asString();
            const std::string group_name = item->getColumn(group_col_idx)->getValue().asString();
            filter_cell->setValue(title + " " + group_name + " " + region_name);
        }
    }
}
