/**
 * @file slgesturesets.h
 * @brief Named gesture sets (#237): per-account storage and batched,
 *        rate-limited application through LLGestureMgr.
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

#ifndef SL_GESTURESETS_H
#define SL_GESTURESETS_H

#include <map>
#include <string>
#include <vector>

#include <boost/signals2.hpp>

#include "llgesturemgr.h"
#include "llinventorymodel.h"
#include "llinventoryobserver.h"
#include "llsingleton.h"
#include "lltimer.h"

class SLGestureSets
    : public LLSingleton<SLGestureSets>
    , public LLGestureManagerObserver
    , public LLInventoryObserver
{
    LLSINGLETON(SLGestureSets);
    ~SLGestureSets() override;
    LOG_CLASS(SLGestureSets);

public:
    enum EMode
    {
        MODE_EXACT = 0, // only this set's gestures are active
        MODE_ADD,       // activate this set's gestures, leave others alone
    };

    struct GestureSet
    {
        uuid_set_t mItems; // inventory item ids
        EMode mMode = MODE_EXACT;
    };
    typedef std::map<std::string, GestureSet> set_map_t;

    // Trigger/key of a gesture, remembered from when it was last loaded so
    // inactive gestures can be shown and conflict-checked without fetching.
    struct GestureMeta
    {
        LLUUID mAssetID;
        std::string mTrigger;
        KEY mKey = KEY_NONE;
        MASK mMask = MASK_NONE;
    };

    void loadFromDisk();

    const set_map_t& getSets() const { return mSets; }
    const GestureSet* getSet(const std::string& name) const;
    bool hasSet(const std::string& name) const { return mSets.count(name) != 0; }

    bool createSet(const std::string& name, const uuid_vec_t& items, EMode mode = MODE_EXACT);
    bool renameSet(const std::string& old_name, const std::string& new_name);
    bool duplicateSet(const std::string& src_name, const std::string& new_name);
    bool deleteSet(const std::string& name);
    void setMembers(const std::string& name, const uuid_vec_t& items, bool member);
    void setMode(const std::string& name, EMode mode);

    const std::string& getLastApplied() const { return mLastApplied; }

    // Queues an apply; a newer request replaces a pending one.
    void applySet(const std::string& name);
    bool isApplyInProgress() const { return mApplying || !mPendingName.empty(); }
    const std::string& getPendingName() const { return mPendingName; }

    // Whole agent inventory fetched, so "not found" really means not found.
    static bool isInventoryReady();
    // Real (non-link, non-trash) gesture items in the agent inventory.
    const LLInventoryModel::item_array_t& getInventoryGestures();
    const GestureMeta* getMeta(const LLUUID& item_id) const;
    uuid_vec_t getActiveIDs() const;

    // Gestures that would share a chat trigger or a key + modifier if the
    // given set were applied. Only gestures with known metadata are checked.
    struct Conflict
    {
        bool mIsKey = false;
        std::string mWhat; // trigger text or accelerator string
        std::vector<std::string> mNames;
    };
    typedef std::vector<Conflict> conflict_list_t;
    conflict_list_t findConflicts(const std::string& name);
    conflict_list_t findConflicts(const uuid_set_t& items);

    typedef boost::signals2::signal<void()> changed_signal_t;
    boost::signals2::connection setChangedCallback(const changed_signal_t::slot_type& cb)
    {
        return mChangedSignal.connect(cb);
    }

    // LLGestureManagerObserver
    void changed() override;
    // LLInventoryObserver
    void changed(U32 mask) override;

protected:
    void initSingleton() override;
    void cleanupSingleton() override;

private:
    std::string getFilename() const;
    void saveToDisk();
    void scheduleMetaSave();
    void notifyChanged();
    uuid_set_t filterToInventory(const uuid_vec_t& items);

    void scheduleApply();
    void waitForInventory();
    void startApply();
    bool stepApply();
    void finishApply();

    set_map_t mSets;
    std::map<LLUUID, GestureMeta> mMeta;
    std::string mLastApplied;
    bool mLoaded = false;
    bool mMetaSaveScheduled = false;

    LLInventoryModel::item_array_t mInvGestures;
    bool mInvGesturesDirty = true;

    // Apply pipeline
    std::string mPendingName;
    std::string mApplyingName;
    bool mApplying = false;
    bool mCooldownScheduled = false;
    bool mHasApplied = false;
    LLTimer mCooldownTimer;
    uuid_vec_t mToDeactivate;
    LLInventoryModel::item_array_t mToActivate;
    S32 mMissingCount = 0;
    boost::signals2::connection mFetchConnection;

    changed_signal_t mChangedSignal;
};

#endif // SL_GESTURESETS_H
