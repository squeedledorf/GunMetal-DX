/**
 * @file slgesturesets.cpp
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

#include "llviewerprecompiledheaders.h"

#include "slgesturesets.h"

#include <algorithm>

#include "llcallbacklist.h"
#include "lldir.h"
#include "llfile.h"
#include "llinventoryfunctions.h"
#include "llinventorymodelbackgroundfetch.h"
#include "llkeyboard.h"
#include "llmultigesture.h"
#include "llnotificationsutil.h"
#include "llsdserialize.h"
#include "llviewerinventory.h"

namespace
{
    const std::string GESTURE_SETS_FILE = "gesture_sets.xml";

    constexpr F32 SWITCH_COOLDOWN_SECS = 2.f;
    constexpr F32 META_SAVE_DELAY_SECS = 10.f;

    // Large sets are spread over frames: about three messages per frame.
    constexpr S32 MSGS_PER_FRAME = 3;
    constexpr S32 ACTIVATE_BLOCKS_PER_MSG = 32; // MTU-bound, see LLGestureMgr::activateGestures
    constexpr size_t DEACTIVATE_PER_FRAME = MSGS_PER_FRAME * LLGestureMgr::GESTURE_MSG_MAX_BLOCKS;
    constexpr size_t ACTIVATE_PER_FRAME = MSGS_PER_FRAME * ACTIVATE_BLOCKS_PER_MSG;

    class LLIsRealGesture : public LLInventoryCollectFunctor
    {
    public:
        bool operator()(LLInventoryCategory*, LLInventoryItem* item) override
        {
            return item && item->getType() == LLAssetType::AT_GESTURE && !item->getIsLinkType();
        }
    };
}

SLGestureSets::SLGestureSets()
{
}

SLGestureSets::~SLGestureSets()
{
}

void SLGestureSets::initSingleton()
{
    LLGestureMgr::instance().addObserver(this);
    gInventory.addObserver(this);
}

void SLGestureSets::cleanupSingleton()
{
    mFetchConnection.disconnect();
    if (LLGestureMgr::instanceExists())
    {
        LLGestureMgr::instance().removeObserver(this);
    }
    gInventory.removeObserver(this);
}

std::string SLGestureSets::getFilename() const
{
    if (gDirUtilp->getExpandedFilename(LL_PATH_PER_SL_ACCOUNT, "").empty())
    {
        return std::string();
    }
    return gDirUtilp->getExpandedFilename(LL_PATH_PER_SL_ACCOUNT, GESTURE_SETS_FILE);
}

void SLGestureSets::loadFromDisk()
{
    const std::string filename = getFilename();
    if (filename.empty())
    {
        return;
    }

    mSets.clear();
    mMeta.clear();
    mLastApplied.clear();

    llifstream file(filename.c_str());
    if (file.is_open())
    {
        LLSD data;
        if (LLSDSerialize::fromXML(data, file) > 0 && data.isMap())
        {
            const LLSD& sets = data["sets"];
            for (LLSD::map_const_iterator it = sets.beginMap(); it != sets.endMap(); ++it)
            {
                GestureSet set;
                set.mMode = (it->second["mode"].asString() == "add") ? MODE_ADD : MODE_EXACT;
                const LLSD& items = it->second["items"];
                for (LLSD::array_const_iterator item_it = items.beginArray(); item_it != items.endArray(); ++item_it)
                {
                    const LLUUID id = item_it->asUUID();
                    if (id.notNull())
                    {
                        set.mItems.insert(id);
                    }
                }
                if (!it->first.empty())
                {
                    mSets[it->first] = set;
                }
            }

            const LLSD& meta = data["meta"];
            for (LLSD::map_const_iterator it = meta.beginMap(); it != meta.endMap(); ++it)
            {
                GestureMeta entry;
                entry.mAssetID = it->second["asset"].asUUID();
                entry.mTrigger = it->second["trigger"].asString();
                entry.mKey = (KEY)it->second["key"].asInteger();
                entry.mMask = (MASK)it->second["mask"].asInteger();
                mMeta[LLUUID(it->first)] = entry;
            }

            mLastApplied = data["last_applied"].asString();
            if (!hasSet(mLastApplied))
            {
                mLastApplied.clear();
            }
        }
    }

    mLoaded = true;
    // Pick up gestures that finished loading before we were constructed
    changed();
    notifyChanged();
}

void SLGestureSets::saveToDisk()
{
    const std::string filename = getFilename();
    if (!mLoaded || filename.empty())
    {
        return;
    }

    // Keep the metadata cache bounded by the gestures actually in inventory
    if (isInventoryReady())
    {
        uuid_set_t inv_ids;
        for (const LLPointer<LLViewerInventoryItem>& item : getInventoryGestures())
        {
            inv_ids.insert(item->getUUID());
        }
        for (auto it = mMeta.begin(); it != mMeta.end(); )
        {
            it = inv_ids.count(it->first) ? std::next(it) : mMeta.erase(it);
        }
    }

    LLSD data = LLSD::emptyMap();
    data["version"] = 1;
    data["last_applied"] = mLastApplied;
    LLSD& sets = data["sets"] = LLSD::emptyMap();
    for (const auto& [name, set] : mSets)
    {
        LLSD entry;
        entry["mode"] = (set.mMode == MODE_ADD) ? "add" : "exact";
        LLSD& items = entry["items"] = LLSD::emptyArray();
        for (const LLUUID& id : set.mItems)
        {
            items.append(id);
        }
        sets[name] = entry;
    }
    LLSD& meta = data["meta"] = LLSD::emptyMap();
    for (const auto& [id, entry] : mMeta)
    {
        LLSD m;
        m["asset"] = entry.mAssetID;
        m["trigger"] = entry.mTrigger;
        m["key"] = (S32)entry.mKey;
        m["mask"] = (S32)entry.mMask;
        meta[id.asString()] = m;
    }

    llofstream file(filename.c_str());
    if (!file.is_open())
    {
        LL_WARNS() << "Unable to write " << filename << LL_ENDL;
        return;
    }
    LLSDSerialize::toPrettyXML(data, file);
}

void SLGestureSets::scheduleMetaSave()
{
    if (mMetaSaveScheduled)
    {
        return;
    }
    mMetaSaveScheduled = true;
    doAfterInterval([]()
        {
            if (SLGestureSets::instanceExists())
            {
                SLGestureSets& self = SLGestureSets::instance();
                self.mMetaSaveScheduled = false;
                self.saveToDisk();
            }
        }, META_SAVE_DELAY_SECS);
}

void SLGestureSets::notifyChanged()
{
    mChangedSignal();
}

const SLGestureSets::GestureSet* SLGestureSets::getSet(const std::string& name) const
{
    set_map_t::const_iterator it = mSets.find(name);
    return (it != mSets.end()) ? &it->second : nullptr;
}

uuid_set_t SLGestureSets::filterToInventory(const uuid_vec_t& items)
{
    uuid_set_t result;
    if (!isInventoryReady())
    {
        for (const LLUUID& id : items)
        {
            result.insert(gInventory.getLinkedItemID(id));
        }
        return result;
    }

    uuid_set_t inv_ids;
    for (const LLPointer<LLViewerInventoryItem>& item : getInventoryGestures())
    {
        inv_ids.insert(item->getUUID());
    }
    for (const LLUUID& id : items)
    {
        const LLUUID& base_id = gInventory.getLinkedItemID(id);
        if (inv_ids.count(base_id))
        {
            result.insert(base_id);
        }
    }
    return result;
}

bool SLGestureSets::createSet(const std::string& name, const uuid_vec_t& items, EMode mode)
{
    if (name.empty() || hasSet(name))
    {
        return false;
    }
    GestureSet& set = mSets[name];
    set.mItems = filterToInventory(items);
    set.mMode = mode;
    saveToDisk();
    notifyChanged();
    return true;
}

bool SLGestureSets::renameSet(const std::string& old_name, const std::string& new_name)
{
    set_map_t::iterator it = mSets.find(old_name);
    if (new_name.empty() || it == mSets.end() || hasSet(new_name))
    {
        return false;
    }
    GestureSet set = std::move(it->second);
    mSets.erase(it);
    mSets[new_name] = std::move(set);
    if (mLastApplied == old_name)
    {
        mLastApplied = new_name;
    }
    if (mPendingName == old_name)
    {
        mPendingName = new_name;
    }
    saveToDisk();
    notifyChanged();
    return true;
}

bool SLGestureSets::duplicateSet(const std::string& src_name, const std::string& new_name)
{
    const GestureSet* src = getSet(src_name);
    if (!src || new_name.empty() || hasSet(new_name))
    {
        return false;
    }
    mSets[new_name] = *src;
    saveToDisk();
    notifyChanged();
    return true;
}

bool SLGestureSets::deleteSet(const std::string& name)
{
    if (!mSets.erase(name))
    {
        return false;
    }
    if (mLastApplied == name)
    {
        mLastApplied.clear();
    }
    if (mPendingName == name)
    {
        mPendingName.clear();
    }
    saveToDisk();
    notifyChanged();
    return true;
}

void SLGestureSets::setMembers(const std::string& name, const uuid_vec_t& items, bool member)
{
    set_map_t::iterator it = mSets.find(name);
    if (it == mSets.end() || items.empty())
    {
        return;
    }
    if (member)
    {
        const uuid_set_t filtered = filterToInventory(items);
        it->second.mItems.insert(filtered.begin(), filtered.end());
    }
    else
    {
        for (const LLUUID& id : items)
        {
            it->second.mItems.erase(gInventory.getLinkedItemID(id));
        }
    }
    saveToDisk();
    notifyChanged();
}

void SLGestureSets::setMode(const std::string& name, EMode mode)
{
    set_map_t::iterator it = mSets.find(name);
    if (it == mSets.end() || it->second.mMode == mode)
    {
        return;
    }
    it->second.mMode = mode;
    saveToDisk();
    notifyChanged();
}

// static
bool SLGestureSets::isInventoryReady()
{
    return LLInventoryModelBackgroundFetch::instance().isEverythingFetched();
}

const LLInventoryModel::item_array_t& SLGestureSets::getInventoryGestures()
{
    if (mInvGesturesDirty && gInventory.isInventoryUsable())
    {
        mInvGestures.clear();
        LLInventoryModel::cat_array_t cats;
        LLIsRealGesture is_gesture;
        gInventory.collectDescendentsIf(gInventory.getRootFolderID(), cats, mInvGestures,
                                        LLInventoryModel::EXCLUDE_TRASH, is_gesture);
        mInvGesturesDirty = false;
    }
    return mInvGestures;
}

const SLGestureSets::GestureMeta* SLGestureSets::getMeta(const LLUUID& item_id) const
{
    std::map<LLUUID, GestureMeta>::const_iterator it = mMeta.find(item_id);
    if (it == mMeta.end())
    {
        return nullptr;
    }
    // Asset ids change when a gesture is edited; stale data is worse than none
    const LLViewerInventoryItem* item = gInventory.getItem(item_id);
    if (item && item->getAssetUUID() != it->second.mAssetID)
    {
        return nullptr;
    }
    return &it->second;
}

uuid_vec_t SLGestureSets::getActiveIDs() const
{
    uuid_vec_t ids;
    LLGestureMgr::instance().getItemIDs(&ids);
    return ids;
}

SLGestureSets::conflict_list_t SLGestureSets::findConflicts(const std::string& name)
{
    const GestureSet* set = getSet(name);
    if (!set)
    {
        return conflict_list_t();
    }
    uuid_set_t effective = set->mItems;
    if (set->mMode == MODE_ADD)
    {
        for (const auto& entry : LLGestureMgr::instance().getActiveGestures())
        {
            effective.insert(entry.first);
        }
    }
    return findConflicts(effective);
}

SLGestureSets::conflict_list_t SLGestureSets::findConflicts(const uuid_set_t& items)
{
    std::map<std::string, std::vector<std::string>> by_trigger;
    std::map<std::pair<KEY, MASK>, std::vector<std::string>> by_key;
    std::map<std::string, std::string> trigger_display;

    for (const LLUUID& id : items)
    {
        const LLViewerInventoryItem* item = gInventory.getItem(id);
        const GestureMeta* meta = getMeta(id);
        if (!item || !meta)
        {
            continue;
        }
        if (!meta->mTrigger.empty())
        {
            std::string lower = meta->mTrigger;
            LLStringUtil::toLower(lower);
            by_trigger[lower].push_back(item->getName());
            trigger_display.emplace(lower, meta->mTrigger);
        }
        if (meta->mKey != KEY_NONE)
        {
            by_key[std::make_pair(meta->mKey, meta->mMask)].push_back(item->getName());
        }
    }

    conflict_list_t conflicts;
    for (auto& [trigger, names] : by_trigger)
    {
        if (names.size() > 1)
        {
            Conflict c;
            c.mWhat = trigger_display[trigger];
            c.mNames = std::move(names);
            conflicts.push_back(std::move(c));
        }
    }
    for (auto& [key, names] : by_key)
    {
        if (names.size() > 1)
        {
            Conflict c;
            c.mIsKey = true;
            c.mWhat = LLKeyboard::stringFromAccelerator(key.second, key.first);
            c.mNames = std::move(names);
            conflicts.push_back(std::move(c));
        }
    }
    return conflicts;
}

// LLGestureManagerObserver: remember trigger/key of every loaded gesture
void SLGestureSets::changed()
{
    if (!mLoaded)
    {
        return;
    }
    bool dirty = false;
    for (const auto& [id, gesture] : LLGestureMgr::instance().getActiveGestures())
    {
        const LLViewerInventoryItem* item = gesture ? gInventory.getItem(id) : nullptr;
        if (!item)
        {
            continue;
        }
        GestureMeta& meta = mMeta[id];
        if (meta.mAssetID != item->getAssetUUID() || meta.mTrigger != gesture->mTrigger
            || meta.mKey != gesture->mKey || meta.mMask != gesture->mMask)
        {
            meta.mAssetID = item->getAssetUUID();
            meta.mTrigger = gesture->mTrigger;
            meta.mKey = gesture->mKey;
            meta.mMask = gesture->mMask;
            dirty = true;
        }
    }
    if (dirty)
    {
        scheduleMetaSave();
    }
}

// LLInventoryObserver: only invalidate, the list is rebuilt on demand
void SLGestureSets::changed(U32 mask)
{
    if (mask & (LLInventoryObserver::ADD | LLInventoryObserver::REMOVE | LLInventoryObserver::STRUCTURE
                | LLInventoryObserver::GESTURE | LLInventoryObserver::REBUILD | LLInventoryObserver::CREATE))
    {
        mInvGesturesDirty = true;
    }
}

void SLGestureSets::applySet(const std::string& name)
{
    if (!hasSet(name))
    {
        return;
    }
    mPendingName = name;
    notifyChanged();
    scheduleApply();
}

void SLGestureSets::scheduleApply()
{
    if (mApplying || mPendingName.empty())
    {
        return;
    }

    // Applying against a partial inventory would treat unfetched gestures as
    // missing and, in exact mode, deactivate everything else.
    if (!isInventoryReady())
    {
        waitForInventory();
        return;
    }

    if (mHasApplied)
    {
        const F32 remaining = SWITCH_COOLDOWN_SECS - mCooldownTimer.getElapsedTimeF32();
        if (remaining > 0.f)
        {
            if (!mCooldownScheduled)
            {
                mCooldownScheduled = true;
                doAfterInterval([]()
                    {
                        if (SLGestureSets::instanceExists())
                        {
                            SLGestureSets& self = SLGestureSets::instance();
                            self.mCooldownScheduled = false;
                            self.scheduleApply();
                        }
                    }, remaining);
            }
            return;
        }
    }

    startApply();
}

void SLGestureSets::waitForInventory()
{
    if (mFetchConnection.connected())
    {
        return;
    }
    LL_INFOS() << "Waiting for inventory fetch before applying gesture set '" << mPendingName << "'" << LL_ENDL;
    LLInventoryModelBackgroundFetch& fetcher = LLInventoryModelBackgroundFetch::instance();
    mFetchConnection = fetcher.setFetchCompletionCallback([]()
        {
            if (!SLGestureSets::instanceExists() || !isInventoryReady())
            {
                return;
            }
            SLGestureSets::instance().mFetchConnection.disconnect();
            doOnIdleOneTime([]()
                {
                    if (SLGestureSets::instanceExists())
                    {
                        SLGestureSets::instance().scheduleApply();
                    }
                });
        });
    fetcher.start();
}

void SLGestureSets::startApply()
{
    mApplyingName = mPendingName;
    mPendingName.clear();

    const GestureSet* set = getSet(mApplyingName);
    if (!set)
    {
        notifyChanged();
        return;
    }

    std::map<LLUUID, LLPointer<LLViewerInventoryItem>> inv;
    for (const LLPointer<LLViewerInventoryItem>& item : getInventoryGestures())
    {
        inv[item->getUUID()] = item;
    }

    LLGestureMgr& gm = LLGestureMgr::instance();
    mToActivate.clear();
    mToDeactivate.clear();
    mMissingCount = 0;

    uuid_set_t wanted;
    for (const LLUUID& id : set->mItems)
    {
        auto it = inv.find(id);
        if (it == inv.end())
        {
            ++mMissingCount;
            continue;
        }
        wanted.insert(id);
        if (!gm.isGestureActive(id))
        {
            mToActivate.push_back(it->second);
        }
    }

    if (set->mMode == MODE_EXACT)
    {
        for (const auto& entry : gm.getActiveGestures())
        {
            if (!wanted.count(entry.first))
            {
                mToDeactivate.push_back(entry.first);
            }
        }
    }

    LL_INFOS() << "Applying gesture set '" << mApplyingName << "': " << mToActivate.size() << " to activate, "
               << mToDeactivate.size() << " to deactivate, " << mMissingCount << " not found" << LL_ENDL;

    const conflict_list_t conflicts = findConflicts(mApplyingName);
    if (!conflicts.empty())
    {
        constexpr size_t MAX_LINES = 6;
        std::string list;
        for (size_t i = 0; i < conflicts.size() && i < MAX_LINES; ++i)
        {
            const Conflict& conflict = conflicts[i];
            list += conflict.mIsKey ? conflict.mWhat : ("\"" + conflict.mWhat + "\"");
            list += ": ";
            for (size_t n = 0; n < conflict.mNames.size(); ++n)
            {
                list += (n ? ", " : "") + conflict.mNames[n];
            }
            list += "\n";
        }
        if (conflicts.size() > MAX_LINES)
        {
            list += "...\n";
        }
        LLSD args;
        args["NAME"] = mApplyingName;
        args["LIST"] = list;
        doOnIdleOneTime([args]()
            {
                LLNotificationsUtil::add("GestureSetConflicts", args);
            });
    }

    mApplying = true;
    mLastApplied = mApplyingName;
    saveToDisk();
    notifyChanged();

    doOnIdleRepeating([]()
        {
            return !SLGestureSets::instanceExists() || SLGestureSets::instance().stepApply();
        });
}

bool SLGestureSets::stepApply()
{
    LLGestureMgr& gm = LLGestureMgr::instance();

    // Deactivate first so the new set never transiently overlaps the old one
    if (!mToDeactivate.empty())
    {
        const size_t count = std::min(mToDeactivate.size(), DEACTIVATE_PER_FRAME);
        uuid_vec_t chunk(mToDeactivate.begin(), mToDeactivate.begin() + count);
        mToDeactivate.erase(mToDeactivate.begin(), mToDeactivate.begin() + count);
        gm.deactivateGestures(chunk);
    }
    else if (!mToActivate.empty())
    {
        const size_t count = std::min(mToActivate.size(), ACTIVATE_PER_FRAME);
        LLViewerInventoryItem::item_array_t chunk(mToActivate.begin(), mToActivate.begin() + count);
        mToActivate.erase(mToActivate.begin(), mToActivate.begin() + count);
        constexpr bool deactivate_similar = false;
        gm.activateGestures(chunk, deactivate_similar);
    }

    if (mToDeactivate.empty() && mToActivate.empty())
    {
        finishApply();
        return true;
    }
    return false;
}

void SLGestureSets::finishApply()
{
    mApplying = false;
    mHasApplied = true;
    mCooldownTimer.reset();

    if (mMissingCount > 0)
    {
        LLSD args;
        args["NAME"] = mApplyingName;
        args["COUNT"] = mMissingCount;
        doOnIdleOneTime([args]()
            {
                LLNotificationsUtil::add("GestureSetAppliedMissing", args);
            });
    }

    notifyChanged();

    if (!mPendingName.empty())
    {
        scheduleApply();
    }
}
