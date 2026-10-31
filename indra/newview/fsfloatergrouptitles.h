/**
 * @file fsfloatergrouptitles.h
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

#ifndef FS_FLOATERGROUPTITLES_H
#define FS_FLOATERGROUPTITLES_H

#include "llfloater.h"

#include "llagent.h"
#include "llgroupmgr.h"
#include <boost/signals2.hpp>
#include <deque>
#include <memory>
#include <set>

class FSFloaterGroupTitles;
class LLCheckBoxCtrl;
class LLContextMenu;
class LLEventTimer;
class LLFilterEditor;
class LLScrollListCtrl;

class FSGroupTitlesObserver : LLGroupMgrObserver
{

public:
    FSGroupTitlesObserver(const LLUUID& group_id, LLHandle<FSFloaterGroupTitles> parent);
    virtual ~FSGroupTitlesObserver();

    void changed(LLGroupChange gc) override;

protected:
    LLHandle<FSFloaterGroupTitles>  mParent;
};

class FSFloaterGroupTitles : public LLFloater, public LLOldEvents::LLSimpleListener
{

public:
    FSFloaterGroupTitles(const LLSD &);
    virtual ~FSFloaterGroupTitles();

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    bool handleKeyHere(KEY key, MASK mask) override;
    bool hasAccelerators() const override { return true; }

    bool handleEvent(LLPointer<LLOldEvents::LLEvent> event, const LLSD& userdata) override; // called on agent group list changes

    void processGroupTitleResults(const LLUUID& group_id);
    void invalidateGroupTitles(const LLUUID& group_id);

private:
    void addListItem(const LLUUID& group_id, const LLUUID& role_id, const std::string& title,
        const std::string& group_name, bool is_active, bool is_no_group = true);

    // FS filters inside its patched LLScrollListCtrl; we keep the rows here and rebuild the
    // visible list when the filter changes (a group's rows are replaced as its titles load).
    struct TitleEntry
    {
        LLUUID      mGroupID;
        LLUUID      mRoleID;
        std::string mTitle;
        std::string mGroupName;
        bool        mIsActive;
        bool        mIsGroup;
    };
    std::vector<TitleEntry> mEntries;
    void addListRow(const TitleEntry& entry);
    void rebuildList();

    // #868: titles load per group, a few requests at a time; until then (or when the request
    // times out) the group has one placeholder row
    enum class ETitlesState { QUEUED, LOADING, LOADED, FAILED };
    struct GroupTitlesState
    {
        std::string                             mName;
        U64                                     mPowers = 0;
        ETitlesState                            mState = ETitlesState::QUEUED;
        F64                                     mRequestTime = 0.0;
        bool                                    mRefreshAfterReply = false;
        std::unique_ptr<FSGroupTitlesObserver>  mObserver;
    };
    typedef std::map<LLUUID, GroupTitlesState> group_state_map_t;
    group_state_map_t   mGroupStates;
    std::deque<LLUUID>  mRequestQueue;
    LLEventTimer*       mRequestTimer = nullptr;
    bool                mSyncScheduled = false;
    std::set<LLUUID>    mInvalidatedGroups;
    bool                mInvalidationScheduled = false;

    void addGroup(const LLGroupData& group_data, bool force);
    void requestTitles(const LLUUID& group_id, bool force);
    void sendQueuedRequests();
    void checkRequestTimeouts();
    void removeGroupRows(const LLUUID& group_id);
    void setGroupRows(const LLUUID& group_id);
    ETitlesState getTitlesState(const LLUUID& group_id) const;
    void scheduleSync();
    void syncGroups();
    void updateActiveFlags();
    void onSortChanged();

    void refreshGroupTitles(bool force);
    void activateGroupTitle();
    void selectedTitleChanged();
    void openGroupInfo();
    void onFilterEdit(const std::string& search_string);

    void onSetRegion();
    void onSetRegionManual();
    void onClearRegion();
    void onUseDefaultToggle();
    void onSetAsDefault();
    void updateDefaultColumn();
    void updateRegionColumn();

    LLButton*           mActivateButton;
    LLButton*           mRefreshButton;
    LLButton*           mInfoButton;
    LLButton*           mSetRegionButton;
    LLButton*           mSetRegionManualButton;
    LLButton*           mClearRegionButton;
    LLButton*           mSetDefaultButton;
    LLCheckBoxCtrl*     mUseDefaultCheck;
    LLScrollListCtrl*   mTitleList;
    LLFilterEditor*     mFilterEditor;

    std::string         mFilterSubString;
    std::string         mFilterSubStringOrig;

    boost::signals2::connection mAssignmentsChangedConnection;
    LLHandle<LLContextMenu>     mClearRegionMenuHandle;
};

#endif // FS_FLOATERGROUPTITLES_H
