/**
 * @file slaosets.cpp
 * @brief Client-side animation overrider (#496): AO sets in inventory,
 *        per-account options, editing and notecard import.
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

#include "slaosets.h"

#include <algorithm>
#include <sstream>

#include "llagent.h"
#include "llassetstorage.h"
#include "llcallbacklist.h"
#include "lldir.h"
#include "llfile.h"
#include "llfilesystem.h"
#include "llinventoryfunctions.h"
#include "llinventorymodel.h"
#include "llinventorymodelbackgroundfetch.h"
#include "llnotecard.h"
#include "llnotificationsutil.h"
#include "llsdserialize.h"
#include "llsdutil.h"
#include "lltrans.h"
#include "llviewerinventory.h"

using namespace SLAO;

const std::string SLAOSets::ROOT_FOLDER_NAME = "#Slipstream AO";
const std::string SLAOSets::COPIES_FOLDER_NAME = "Animations";

namespace
{
    const std::string AO_FILE = "sl_ao.xml";
    // Folder layout used by other viewers' client AOs (Firestorm: #Firestorm/#AO)
    const std::string OTHER_VIEWER_PARENT = "#Firestorm";
    const std::string OTHER_VIEWER_ROOT = "#AO";

    constexpr F32 SAVE_DELAY_SECS = 2.f;
    constexpr F32 RESCAN_DELAY_SECS = 0.5f;
    constexpr F32 IMPORT_FETCH_TIMEOUT_SECS = 30.f;
    constexpr size_t MAX_IMPORTS = 4;
    constexpr size_t MAX_SNAPSHOT_ANIMS = 64;
    constexpr size_t MAX_SCRIPTED_AO_CHOICES = 32;
    constexpr size_t MAX_IGNORED_ANIMS = 256;
    constexpr size_t MAX_SET_NAME = 63; // DB_INV_ITEM_NAME_STR_LEN

    bool is_animation(const LLViewerInventoryItem* item)
    {
        // getType() follows links
        return item && item->getType() == LLAssetType::AT_ANIMATION;
    }

    class LLIsAnimation : public LLInventoryCollectFunctor
    {
    public:
        bool operator()(LLInventoryCategory*, LLInventoryItem* item) override
        {
            return is_animation(dynamic_cast<LLViewerInventoryItem*>(item));
        }
    };

    // Display name of a set folder. Other viewers may append ":"-separated
    // flags to the folder name; hide them.
    std::string display_name(const std::string& folder_name, bool other_viewer)
    {
        std::string name = folder_name;
        if (other_viewer)
        {
            const std::string::size_type colon = name.find(':');
            if (colon != std::string::npos && colon > 0)
            {
                name.erase(colon);
            }
        }
        LLStringUtil::trim(name);
        return name.empty() ? folder_name : name;
    }

    bool tree_fetched(const LLUUID& cat_id, S32 depth)
    {
        const LLViewerInventoryCategory* cat = gInventory.getCategory(cat_id);
        if (!cat || cat->getVersion() == LLViewerInventoryCategory::VERSION_UNKNOWN ||
            cat->getDescendentCount() == LLViewerInventoryCategory::DESCENDENT_COUNT_UNKNOWN ||
            cat->getDescendentCount() != cat->getViewerDescendentCount())
        {
            return false;
        }
        if (depth <= 0)
        {
            return true;
        }
        LLInventoryModel::cat_array_t* cats = nullptr;
        LLInventoryModel::item_array_t* items = nullptr;
        gInventory.getDirectDescendentsOf(cat_id, cats, items);
        if (cats)
        {
            for (const LLPointer<LLViewerInventoryCategory>& child : *cats)
            {
                if (!tree_fetched(child->getUUID(), depth - 1))
                {
                    return false;
                }
            }
        }
        return true;
    }

    LLUUID find_child_folder(const LLUUID& parent_id, const std::string& name)
    {
        LLInventoryModel::cat_array_t* cats = nullptr;
        LLInventoryModel::item_array_t* items = nullptr;
        gInventory.getDirectDescendentsOf(parent_id, cats, items);
        if (cats)
        {
            for (const LLPointer<LLViewerInventoryCategory>& cat : *cats)
            {
                if (cat->getName() == name)
                {
                    return cat->getUUID();
                }
            }
        }
        return LLUUID::null;
    }
}

SLAOSets::SLAOSets()
{
    mOptions = LLSD::emptyMap();
    mSnapshot = LLSD::emptyMap();
}

SLAOSets::~SLAOSets()
{
}

void SLAOSets::initSingleton()
{
    gInventory.addObserver(this);
}

void SLAOSets::cleanupSingleton()
{
    if (mSaveScheduled)
    {
        saveToDisk();
    }
    gInventory.removeObserver(this);
}

//-----------------------------------------------------------------------------
// Persistence
//-----------------------------------------------------------------------------

std::string SLAOSets::getFilename() const
{
    if (gDirUtilp->getExpandedFilename(LL_PATH_PER_SL_ACCOUNT, "").empty())
    {
        return std::string();
    }
    return gDirUtilp->getExpandedFilename(LL_PATH_PER_SL_ACCOUNT, AO_FILE);
}

void SLAOSets::loadFromDisk()
{
    const std::string filename = getFilename();
    if (filename.empty())
    {
        return;
    }

    mOptions = LLSD::emptyMap();
    mSnapshot = LLSD::emptyMap();
    mScriptedAOChoices.clear();
    mIgnoredAnims.clear();
    mSets.clear();

    llifstream file(filename.c_str());
    if (file.is_open())
    {
        LLSD data;
        if (LLSDSerialize::fromXML(data, file) > 0 && data.isMap())
        {
            if (data["sets"].isMap())
            {
                mOptions = data["sets"];
            }
            if (data["snapshot"].isMap())
            {
                mSnapshot = data["snapshot"];
            }
            const LLSD& ignored = data["ignored_anims"];
            for (LLSD::map_const_iterator it = ignored.beginMap(); it != ignored.endMap(); ++it)
            {
                const LLUUID id(it->first);
                if (id.notNull() && mIgnoredAnims.size() < MAX_IGNORED_ANIMS)
                {
                    mIgnoredAnims[id] = it->second.asString();
                }
            }
            const LLSD& choices = data["scripted_ao"];
            for (LLSD::map_const_iterator it = choices.beginMap(); it != choices.endMap(); ++it)
            {
                const LLUUID id(it->first);
                if (id.notNull() && mScriptedAOChoices.size() < MAX_SCRIPTED_AO_CHOICES)
                {
                    mScriptedAOChoices[id] = it->second.asString();
                }
            }
        }
    }

    // Until inventory is loaded, the snapshot stands in for the active set
    const LLUUID snap_id = mSnapshot["set"].asUUID();
    if (snap_id.notNull())
    {
        SLAOSet set;
        set.mFolderID = snap_id;
        set.mName = mSnapshot["name"].asString();
        set.mFromSnapshot = true;
        const LLSD& states = mSnapshot["states"];
        for (LLSD::map_const_iterator it = states.beginMap(); it != states.endMap(); ++it)
        {
            const EState state = stateFromFolderName(it->first);
            if (state == STATE_NONE)
            {
                continue;
            }
            const LLSD anims = it->second["anims"];
            for (const LLSD& entry : llsd::inArray(anims))
            {
                SLAOAnim anim;
                anim.mAssetID = entry["asset"].asUUID();
                anim.mName = entry["name"].asString();
                if (anim.mAssetID.notNull())
                {
                    set.mAnims[state].push_back(anim);
                }
            }
        }
        applyOptions(set);
        mSets[snap_id] = set;
    }

    mLoaded = true;
    notifyChanged();
    requestRescan();
}

void SLAOSets::scheduleSave()
{
    if (mSaveScheduled)
    {
        return;
    }
    mSaveScheduled = true;
    doAfterInterval([]()
        {
            if (SLAOSets::instanceExists())
            {
                SLAOSets::instance().saveToDisk();
            }
        }, SAVE_DELAY_SECS);
}

void SLAOSets::saveToDisk()
{
    mSaveScheduled = false;
    const std::string filename = getFilename();
    if (!mLoaded || filename.empty())
    {
        return;
    }

    LLSD data = LLSD::emptyMap();
    data["version"] = 1;
    data["sets"] = mOptions;
    data["snapshot"] = mSnapshot;
    LLSD& ignored = data["ignored_anims"] = LLSD::emptyMap();
    for (const auto& [id, label] : mIgnoredAnims)
    {
        ignored[id.asString()] = label;
    }
    LLSD& choices = data["scripted_ao"] = LLSD::emptyMap();
    for (const auto& [id, choice] : mScriptedAOChoices)
    {
        choices[id.asString()] = choice;
    }

    llofstream file(filename.c_str());
    if (file.is_open())
    {
        LLSDSerialize::toPrettyXML(data, file);
    }
    else
    {
        LL_WARNS() << "Unable to write " << filename << LL_ENDL;
    }
}

void SLAOSets::updateSnapshot(const LLUUID& set_id)
{
    const SLAOSet* set = getSet(set_id);
    if (!set || set->mFromSnapshot)
    {
        return;
    }
    LLSD snap = LLSD::emptyMap();
    snap["set"] = set_id;
    snap["name"] = set->mName;
    LLSD& states = snap["states"] = LLSD::emptyMap();
    for (S32 i = 0; i < STATE_COUNT; ++i)
    {
        if (set->mAnims[i].empty())
        {
            continue;
        }
        LLSD anims = LLSD::emptyArray();
        for (const SLAOAnim& anim : set->mAnims[i])
        {
            if (anims.size() >= (S32)MAX_SNAPSHOT_ANIMS)
            {
                break;
            }
            anims.append(LLSD().with("asset", anim.mAssetID).with("name", anim.mName));
        }
        states[stateFolderName((EState)i)]["anims"] = anims;
    }
    if (!llsd_equals(snap, mSnapshot))
    {
        mSnapshot = snap;
        scheduleSave();
    }
}

void SLAOSets::notifyChanged()
{
    mChangedSignal();
}

//-----------------------------------------------------------------------------
// Inventory scanning
//-----------------------------------------------------------------------------

LLUUID SLAOSets::findRootFolder() const
{
    if (!gInventory.isInventoryUsable())
    {
        return LLUUID::null;
    }
    return find_child_folder(gInventory.getRootFolderID(), ROOT_FOLDER_NAME);
}

LLUUID SLAOSets::findOtherViewerRoot() const
{
    if (!gInventory.isInventoryUsable())
    {
        return LLUUID::null;
    }
    const LLUUID parent = find_child_folder(gInventory.getRootFolderID(), OTHER_VIEWER_PARENT);
    return parent.notNull() ? find_child_folder(parent, OTHER_VIEWER_ROOT) : LLUUID::null;
}

void SLAOSets::ensureRootFolder(std::function<void(const LLUUID&)> cb)
{
    const LLUUID root = findRootFolder();
    if (root.notNull())
    {
        cb(root);
        return;
    }
    if (!gInventory.isInventoryUsable())
    {
        cb(LLUUID::null);
        return;
    }
    mRootWaiters.push_back(cb);
    if (mCreatingRoot)
    {
        return;
    }
    mCreatingRoot = true;
    gInventory.createNewCategory(gInventory.getRootFolderID(), LLFolderType::FT_NONE, ROOT_FOLDER_NAME,
        [](const LLUUID& new_id)
        {
            if (!SLAOSets::instanceExists())
            {
                return;
            }
            SLAOSets& self = SLAOSets::instance();
            self.mCreatingRoot = false;
            std::vector<std::function<void(const LLUUID&)>> waiters;
            waiters.swap(self.mRootWaiters);
            for (auto& waiter : waiters)
            {
                waiter(new_id);
            }
            self.requestRescan();
        });
}

void SLAOSets::fetchRoots()
{
    const LLUUID roots[] = { findRootFolder(), findOtherViewerRoot() };
    for (const LLUUID& root : roots)
    {
        if (root.notNull())
        {
            LLInventoryModelBackgroundFetch::instance().start(root, true);
            mRootFetchRequested = true;
        }
    }
}

void SLAOSets::requestRescan()
{
    if (mRescanScheduled)
    {
        return;
    }
    mRescanScheduled = true;
    doAfterInterval([]()
        {
            if (SLAOSets::instanceExists())
            {
                SLAOSets::instance().rescan();
            }
        }, RESCAN_DELAY_SECS);
}

void SLAOSets::changed(U32 mask)
{
    if (!mLoaded)
    {
        return;
    }
    // Cheap filter: anything that can move, rename, add or remove folders,
    // links or items. The rescan itself is debounced.
    if (mask & (LLInventoryObserver::ADD | LLInventoryObserver::REMOVE | LLInventoryObserver::STRUCTURE |
                LLInventoryObserver::LABEL | LLInventoryObserver::INTERNAL | LLInventoryObserver::REBUILD |
                LLInventoryObserver::CREATE))
    {
        requestRescan();
    }
}

void SLAOSets::scanRoot(const LLUUID& root_id, bool other_viewer, set_map_t& out) const
{
    LLInventoryModel::cat_array_t* set_cats = nullptr;
    LLInventoryModel::item_array_t* root_items = nullptr;
    gInventory.getDirectDescendentsOf(root_id, set_cats, root_items);
    if (!set_cats)
    {
        return;
    }

    for (const LLPointer<LLViewerInventoryCategory>& set_cat : *set_cats)
    {
        SLAOSet set;
        set.mFolderID = set_cat->getUUID();
        set.mName = display_name(set_cat->getName(), other_viewer);
        set.mFromOtherViewer = other_viewer;

        LLInventoryModel::cat_array_t* state_cats = nullptr;
        LLInventoryModel::item_array_t* set_items = nullptr;
        gInventory.getDirectDescendentsOf(set.mFolderID, state_cats, set_items);
        if (state_cats)
        {
            for (const LLPointer<LLViewerInventoryCategory>& state_cat : *state_cats)
            {
                if (state_cat->getName() == COPIES_FOLDER_NAME)
                {
                    set.mCopiesFolderID = state_cat->getUUID();
                    continue;
                }
                const EState state = stateFromFolderName(state_cat->getName());
                if (state == STATE_NONE)
                {
                    continue;
                }
                if (set.mStateFolders[state].isNull())
                {
                    set.mStateFolders[state] = state_cat->getUUID();
                }

                LLInventoryModel::cat_array_t* sub_cats = nullptr;
                LLInventoryModel::item_array_t* items = nullptr;
                gInventory.getDirectDescendentsOf(state_cat->getUUID(), sub_cats, items);
                if (!items)
                {
                    continue;
                }
                for (const LLPointer<LLViewerInventoryItem>& item : *items)
                {
                    if (!is_animation(item))
                    {
                        // Broken or not yet resolved link: the target fetch
                        // will trigger another rescan.
                        continue;
                    }
                    SLAOAnim anim;
                    anim.mItemID = item->getUUID();
                    anim.mTargetID = item->getLinkedUUID();
                    anim.mAssetID = item->getAssetUUID();
                    anim.mName = item->getName();
                    if (anim.mAssetID.notNull())
                    {
                        set.mAnims[state].push_back(anim);
                    }
                }
            }
        }
        // Copies taken from a HUD or object that no state links to yet
        if (set.mCopiesFolderID.notNull())
        {
            uuid_set_t linked;
            for (const auto& anims : set.mAnims)
            {
                for (const SLAOAnim& anim : anims)
                {
                    linked.insert(anim.mTargetID);
                }
            }
            LLInventoryModel::cat_array_t* sub_cats = nullptr;
            LLInventoryModel::item_array_t* items = nullptr;
            gInventory.getDirectDescendentsOf(set.mCopiesFolderID, sub_cats, items);
            if (items)
            {
                for (const LLPointer<LLViewerInventoryItem>& item : *items)
                {
                    if (!is_animation(item))
                    {
                        continue;
                    }
                    set.mHasCopies = true;
                    if (linked.count(item->getLinkedUUID()))
                    {
                        continue;
                    }
                    SLAOAnim anim;
                    anim.mItemID = item->getUUID();
                    anim.mTargetID = item->getLinkedUUID();
                    anim.mAssetID = item->getAssetUUID();
                    anim.mName = item->getName();
                    set.mUnsorted.push_back(anim);
                }
            }
            std::sort(set.mUnsorted.begin(), set.mUnsorted.end(), [](const SLAOAnim& a, const SLAOAnim& b)
                {
                    return LLStringUtil::compareDict(a.mName, b.mName) < 0;
                });
        }
        applyOptions(set);
        out[set.mFolderID] = set;
    }
}

void SLAOSets::rescan()
{
    mRescanScheduled = false;
    if (!mLoaded || !gInventory.isInventoryUsable())
    {
        return;
    }

    if (!mRootFetchRequested)
    {
        fetchRoots();
    }

    set_map_t sets;
    const LLUUID own_root = findRootFolder();
    const LLUUID other_root = findOtherViewerRoot();
    if (other_root.notNull())
    {
        scanRoot(other_root, true, sets);
    }
    if (own_root.notNull())
    {
        scanRoot(own_root, false, sets);
    }
    // Links point anywhere in inventory; their targets are only known once
    // the whole inventory is fetched.
    const bool complete = LLInventoryModelBackgroundFetch::instance().isEverythingFetched();

    if (!complete)
    {
        // Keep using the login snapshot for the active set until its real
        // contents are known, so the AO does not drop out during login.
        const LLUUID snap_id = mSnapshot["set"].asUUID();
        auto old_it = mSets.find(snap_id);
        if (snap_id.notNull() && old_it != mSets.end())
        {
            auto new_it = sets.find(snap_id);
            if (new_it == sets.end())
            {
                sets[snap_id] = old_it->second;
            }
            else
            {
                for (S32 i = 0; i < STATE_COUNT; ++i)
                {
                    if (new_it->second.mAnims[i].empty() && !old_it->second.mAnims[i].empty())
                    {
                        new_it->second.mAnims[i] = old_it->second.mAnims[i];
                    }
                }
            }
        }
        // Look again later even if no inventory change wakes us up
        if (!mRetryScheduled)
        {
            mRetryScheduled = true;
            doAfterInterval([]()
                {
                    if (SLAOSets::instanceExists())
                    {
                        SLAOSets::instance().mRetryScheduled = false;
                        SLAOSets::instance().requestRescan();
                    }
                }, 3.f);
        }
    }

    mSets.swap(sets);

    if (complete && !mScanned)
    {
        mScanned = true;
        // Drop options of sets that no longer exist (bounded storage)
        LLSD pruned = LLSD::emptyMap();
        for (LLSD::map_const_iterator it = mOptions.beginMap(); it != mOptions.endMap(); ++it)
        {
            if (mSets.count(LLUUID(it->first)))
            {
                pruned[it->first] = it->second;
            }
        }
        if (pruned.size() != mOptions.size())
        {
            mOptions = pruned;
            scheduleSave();
        }
        LL_INFOS() << "AO inventory scanned: " << mSets.size() << " sets" << LL_ENDL;
    }

    notifyChanged();
}

void SLAOSets::applyOptions(SLAOSet& set) const
{
    const LLSD& opts = mOptions[set.mFolderID.asString()];
    if (opts.isMap())
    {
        if (opts.has("sit_override"))
        {
            set.mSitOverride = opts["sit_override"].asBoolean();
        }
        if (opts.has("smart_sit"))
        {
            set.mSmartSit = opts["smart_sit"].asBoolean();
        }
        set.mNoStandsInMouselook = opts["no_stands_mouselook"].asBoolean();
        set.mNoStandsWhileTyping = opts["no_stands_typing"].asBoolean();
        if (opts.has("stop_system"))
        {
            set.mStopSystemAnims = opts["stop_system"].asBoolean();
        }
        if (opts.has("always"))
        {
            set.mAlwaysEnabled = opts["always"].asBoolean();
        }
        const LLSD& idle = opts["idle"];
        if (idle.isMap())
        {
            set.mIdleEnabled = idle["enabled"].asBoolean();
            set.mIdleMinDelay = llclamp((F32)idle["min"].asReal(), 5.f, 3600.f);
            set.mIdleMaxDelay = llclamp((F32)idle["max"].asReal(), set.mIdleMinDelay, 3600.f);
            set.mIdleMaxDuration = llclamp((F32)idle["max_duration"].asReal(), 1.f, 300.f);
            set.mIdleWhileSitting = idle["while_sitting"].asBoolean();
        }
    }

    for (S32 i = 0; i < STATE_COUNT; ++i)
    {
        const LLSD& state_opts = opts.isMap() ? opts["states"][stateFolderName((EState)i)] : LLSD();
        set.mStateConfig[i].fromLLSD(state_opts["config"]);
        if (i == IDLE && !state_opts["config"].has("idle_options"))
        {
            // Idles were always random before they had options: keep that
            set.mStateConfig[i].mCycle = true;
            set.mStateConfig[i].mRandom = true;
        }

        // Order: stored list of target item ids first, the rest by name
        std::map<LLUUID, S32> rank;
        S32 pos = 0;
        const LLSD order = state_opts["order"];
        for (const LLSD& id : llsd::inArray(order))
        {
            rank[id.asUUID()] = pos++;
        }
        std::stable_sort(set.mAnims[i].begin(), set.mAnims[i].end(),
            [&rank](const SLAOAnim& a, const SLAOAnim& b)
            {
                auto ra = rank.find(a.mTargetID);
                auto rb = rank.find(b.mTargetID);
                const bool ha = ra != rank.end();
                const bool hb = rb != rank.end();
                if (ha != hb)
                {
                    return ha;
                }
                if (ha)
                {
                    return ra->second < rb->second;
                }
                return LLStringUtil::compareDict(a.mName, b.mName) < 0;
            });
    }
}

void SLAOSets::storeOptions(const SLAOSet& set)
{
    if (set.mFromSnapshot)
    {
        return;
    }
    LLSD opts = LLSD::emptyMap();
    opts["name"] = set.mName; // informational only
    opts["sit_override"] = set.mSitOverride;
    opts["smart_sit"] = set.mSmartSit;
    opts["no_stands_mouselook"] = set.mNoStandsInMouselook;
    opts["no_stands_typing"] = set.mNoStandsWhileTyping;
    opts["always"] = set.mAlwaysEnabled;
    opts["stop_system"] = set.mStopSystemAnims;
    LLSD& idle = opts["idle"] = LLSD::emptyMap();
    idle["enabled"] = set.mIdleEnabled;
    idle["min"] = set.mIdleMinDelay;
    idle["max"] = set.mIdleMaxDelay;
    idle["max_duration"] = set.mIdleMaxDuration;
    idle["while_sitting"] = set.mIdleWhileSitting;
    LLSD& states = opts["states"] = LLSD::emptyMap();
    for (S32 i = 0; i < STATE_COUNT; ++i)
    {
        LLSD state_opts = LLSD::emptyMap();
        state_opts["config"] = set.mStateConfig[i].toLLSD();
        if (i == IDLE)
        {
            state_opts["config"]["idle_options"] = true; // options set by the user from here on
        }
        if (!set.mAnims[i].empty())
        {
            LLSD order = LLSD::emptyArray();
            for (const SLAOAnim& anim : set.mAnims[i])
            {
                order.append(anim.mTargetID);
            }
            state_opts["order"] = order;
        }
        states[stateFolderName((EState)i)] = state_opts;
    }
    mOptions[set.mFolderID.asString()] = opts;
    scheduleSave();
}

//-----------------------------------------------------------------------------
// Queries
//-----------------------------------------------------------------------------

const SLAOSet* SLAOSets::getSet(const LLUUID& set_id) const
{
    auto it = mSets.find(set_id);
    return (it != mSets.end()) ? &it->second : nullptr;
}

const SLAOSet* SLAOSets::findSetByName(const std::string& name) const
{
    std::string wanted = name;
    LLStringUtil::trim(wanted);
    for (const auto& [id, set] : mSets)
    {
        if (LLStringUtil::compareInsensitive(set.mName, wanted) == 0)
        {
            return &set;
        }
    }
    return nullptr;
}

std::vector<const SLAOSet*> SLAOSets::getSortedSets() const
{
    std::vector<const SLAOSet*> out;
    out.reserve(mSets.size());
    for (const auto& [id, set] : mSets)
    {
        out.push_back(&set);
    }
    std::sort(out.begin(), out.end(), [](const SLAOSet* a, const SLAOSet* b)
        {
            return LLStringUtil::compareDict(a->mName, b->mName) < 0;
        });
    return out;
}

std::string SLAOSets::uniqueSetName(const std::string& wanted) const
{
    std::string base = wanted;
    LLStringUtil::trim(base);
    // ':' is reserved for folder flags by other viewers
    LLStringUtil::replaceChar(base, ':', '-');
    if (base.empty())
    {
        base = "AO";
    }
    if (base.size() > MAX_SET_NAME - 5)
    {
        base.resize(MAX_SET_NAME - 5);
    }
    std::string name = base;
    for (S32 i = 2; findSetByName(name) && i < 1000; ++i)
    {
        name = llformat("%s (%d)", base.c_str(), i);
    }
    return name;
}

//-----------------------------------------------------------------------------
// Editing
//-----------------------------------------------------------------------------

void SLAOSets::createSet(const std::string& name, std::function<void(const LLUUID&)> cb)
{
    const std::string set_name = uniqueSetName(name);
    ensureRootFolder([set_name, cb](const LLUUID& root_id)
        {
            if (root_id.isNull())
            {
                LL_WARNS() << "No AO root folder, cannot create set '" << set_name << "'" << LL_ENDL;
                if (cb)
                {
                    cb(LLUUID::null);
                }
                return;
            }
            gInventory.createNewCategory(root_id, LLFolderType::FT_NONE, set_name,
                [cb](const LLUUID& new_id)
                {
                    if (cb)
                    {
                        cb(new_id);
                    }
                    if (SLAOSets::instanceExists())
                    {
                        SLAOSets::instance().requestRescan();
                    }
                });
        });
}

void SLAOSets::renameSet(const LLUUID& set_id, const std::string& name)
{
    const LLViewerInventoryCategory* cat = gInventory.getCategory(set_id);
    if (!cat)
    {
        return;
    }
    std::string new_name = name;
    LLStringUtil::trim(new_name);
    LLStringUtil::replaceChar(new_name, ':', '-');
    if (new_name.empty())
    {
        return;
    }
    // Keep another viewer's folder flags intact
    const std::string& old_name = cat->getName();
    const std::string::size_type colon = old_name.find(':');
    if (colon != std::string::npos && colon > 0)
    {
        new_name += old_name.substr(colon);
    }
    if (new_name.size() > MAX_SET_NAME)
    {
        new_name.resize(MAX_SET_NAME);
    }
    rename_category(&gInventory, set_id, new_name);
    requestRescan();
}

void SLAOSets::deleteSet(const LLUUID& set_id)
{
    if (gInventory.getCategory(set_id))
    {
        // Moves the folder (links only, for sets we made) to the trash
        gInventory.removeCategory(set_id);
    }
    mOptions.erase(set_id.asString());
    scheduleSave();
    requestRescan();
}

void SLAOSets::addAnimations(const LLUUID& set_id, EState state, const uuid_vec_t& item_ids)
{
    if (state <= STATE_NONE || state >= STATE_COUNT || set_id.isNull())
    {
        return;
    }
    uuid_vec_t targets;
    for (const LLUUID& id : item_ids)
    {
        const LLViewerInventoryItem* item = gInventory.getItem(id);
        if (!is_animation(item))
        {
            continue;
        }
        // Links can't point at links: always link the real item
        const LLUUID target = item->getLinkedUUID();
        if (std::find(targets.begin(), targets.end(), target) == targets.end())
        {
            targets.push_back(target);
        }
    }
    if (targets.empty())
    {
        return;
    }

    // Append to the stored order so new animations land at the end
    auto it = mSets.find(set_id);
    if (it != mSets.end())
    {
        SLAOSet& set = it->second;
        for (const LLUUID& target : targets)
        {
            const bool present = std::any_of(set.mAnims[state].begin(), set.mAnims[state].end(),
                [&target](const SLAOAnim& anim) { return anim.mTargetID == target; });
            if (!present)
            {
                const LLViewerInventoryItem* item = gInventory.getItem(target);
                SLAOAnim anim;
                anim.mTargetID = target;
                anim.mAssetID = item ? item->getAssetUUID() : LLUUID::null;
                anim.mName = item ? item->getName() : std::string();
                set.mAnims[state].push_back(anim); // provisional until the rescan
            }
        }
        storeOptions(set);
    }
    queueLinks(set_id, state, targets);
}

void SLAOSets::queueLinks(const LLUUID& set_id, EState state, const uuid_vec_t& target_ids)
{
    const SLAOSet* set = getSet(set_id);
    // A snapshot set's folder ids are not known to be valid yet
    const LLUUID folder_id = (set && !set->mFromSnapshot) ? set->mStateFolders[state] : LLUUID::null;

    const PendingKey key{ set_id, (S32)state };
    if (folder_id.notNull())
    {
        mPendingLinks[key].insert(mPendingLinks[key].end(), target_ids.begin(), target_ids.end());
        flushPendingLinks(set_id, state, folder_id);
        return;
    }

    const bool creating = mPendingLinks.count(key) != 0;
    uuid_vec_t& pending = mPendingLinks[key];
    pending.insert(pending.end(), target_ids.begin(), target_ids.end());
    if (creating)
    {
        return;
    }
    gInventory.createNewCategory(set_id, LLFolderType::FT_NONE, stateFolderName(state),
        [set_id, state](const LLUUID& new_id)
        {
            if (!SLAOSets::instanceExists())
            {
                return;
            }
            SLAOSets& self = SLAOSets::instance();
            if (new_id.isNull())
            {
                LL_WARNS() << "Could not create AO state folder " << stateFolderName(state) << LL_ENDL;
                self.mPendingLinks.erase(PendingKey{ set_id, (S32)state });
                return;
            }
            self.flushPendingLinks(set_id, state, new_id);
        });
}

void SLAOSets::flushPendingLinks(const LLUUID& set_id, EState state, const LLUUID& state_folder_id)
{
    auto it = mPendingLinks.find(PendingKey{ set_id, (S32)state });
    if (it == mPendingLinks.end())
    {
        return;
    }
    uuid_vec_t targets;
    targets.swap(it->second);
    mPendingLinks.erase(it);

    // Skip targets already linked in this folder
    uuid_set_t existing;
    LLInventoryModel::cat_array_t* cats = nullptr;
    LLInventoryModel::item_array_t* items = nullptr;
    gInventory.getDirectDescendentsOf(state_folder_id, cats, items);
    if (items)
    {
        for (const LLPointer<LLViewerInventoryItem>& item : *items)
        {
            existing.insert(item->getLinkedUUID());
        }
    }

    LLInventoryObject::const_object_list_t objects;
    for (const LLUUID& target : targets)
    {
        if (existing.count(target))
        {
            continue;
        }
        existing.insert(target);
        if (const LLViewerInventoryItem* item = gInventory.getItem(target))
        {
            objects.push_back(LLConstPointer<LLInventoryObject>(item));
        }
    }
    if (objects.empty())
    {
        requestRescan();
        return;
    }
    link_inventory_array(state_folder_id, objects,
        new LLBoostFuncInventoryCallback([](const LLUUID&)
            {
                if (SLAOSets::instanceExists())
                {
                    SLAOSets::instance().requestRescan();
                }
            }));
}

void SLAOSets::removeAnimation(const LLUUID& set_id, EState state, const LLUUID& item_id)
{
    auto it = mSets.find(set_id);
    if (it == mSets.end() || state <= STATE_NONE || state >= STATE_COUNT)
    {
        return;
    }
    SLAOSet& set = it->second;
    auto& anims = set.mAnims[state];
    auto anim_it = std::find_if(anims.begin(), anims.end(),
        [&item_id](const SLAOAnim& anim) { return anim.mItemID == item_id || anim.mTargetID == item_id; });
    if (anim_it == anims.end())
    {
        return;
    }
    const LLUUID inv_id = anim_it->mItemID;
    anims.erase(anim_it);
    storeOptions(set);

    LLViewerInventoryItem* item = gInventory.getItem(inv_id);
    if (item)
    {
        if (item->getIsLinkType())
        {
            remove_inventory_item(inv_id, nullptr);
        }
        else
        {
            // A real animation was placed in the state folder by hand:
            // never delete it, move it to the Animations folder instead.
            const LLUUID anim_folder = gInventory.findCategoryUUIDForType(LLFolderType::FT_ANIMATION);
            if (anim_folder.notNull())
            {
                move_inventory_item(gAgent.getID(), gAgent.getSessionID(), inv_id, anim_folder,
                                    item->getName(), nullptr);
            }
        }
    }
    notifyChanged();
    requestRescan();
}

void SLAOSets::moveAnimation(const LLUUID& set_id, EState state, const LLUUID& item_id, S32 delta)
{
    auto it = mSets.find(set_id);
    if (it == mSets.end() || state <= STATE_NONE || state >= STATE_COUNT || delta == 0)
    {
        return;
    }
    SLAOSet& set = it->second;
    auto& anims = set.mAnims[state];
    for (S32 i = 0; i < (S32)anims.size(); ++i)
    {
        if (anims[i].mItemID == item_id)
        {
            const S32 j = llclamp(i + delta, 0, (S32)anims.size() - 1);
            if (i != j)
            {
                SLAOAnim moved = anims[i];
                anims.erase(anims.begin() + i);
                anims.insert(anims.begin() + j, moved);
                storeOptions(set);
                notifyChanged();
            }
            return;
        }
    }
}

void SLAOSets::setStateConfig(const LLUUID& set_id, EState state, const SLAOStateConfig& config)
{
    auto it = mSets.find(set_id);
    if (it == mSets.end() || state <= STATE_NONE || state >= STATE_COUNT)
    {
        return;
    }
    it->second.mStateConfig[state] = config;
    storeOptions(it->second);
    notifyChanged();
}

void SLAOSets::setSetOptions(const LLUUID& set_id, bool sit_override, bool smart_sit,
                             bool no_stands_mouselook, bool no_stands_typing)
{
    auto it = mSets.find(set_id);
    if (it == mSets.end())
    {
        return;
    }
    SLAOSet& set = it->second;
    set.mSitOverride = sit_override;
    set.mSmartSit = smart_sit;
    set.mNoStandsInMouselook = no_stands_mouselook;
    set.mNoStandsWhileTyping = no_stands_typing;
    storeOptions(set);
    notifyChanged();
}

void SLAOSets::setStopSystemAnims(const LLUUID& set_id, bool enabled)
{
    auto it = mSets.find(set_id);
    if (it == mSets.end())
    {
        return;
    }
    it->second.mStopSystemAnims = enabled;
    storeOptions(it->second);
    notifyChanged();
}

void SLAOSets::setAlwaysEnabled(const LLUUID& set_id, bool enabled)
{
    auto it = mSets.find(set_id);
    if (it == mSets.end())
    {
        return;
    }
    it->second.mAlwaysEnabled = enabled;
    storeOptions(it->second);
    notifyChanged();
}

void SLAOSets::setIdleOptions(const LLUUID& set_id, bool enabled, F32 min_delay, F32 max_delay,
                              F32 max_duration, bool while_sitting)
{
    auto it = mSets.find(set_id);
    if (it == mSets.end())
    {
        return;
    }
    SLAOSet& set = it->second;
    set.mIdleEnabled = enabled;
    set.mIdleMinDelay = llclamp(min_delay, 5.f, 3600.f);
    set.mIdleMaxDelay = llclamp(max_delay, set.mIdleMinDelay, 3600.f);
    set.mIdleMaxDuration = llclamp(max_duration, 1.f, 300.f);
    set.mIdleWhileSitting = while_sitting;
    storeOptions(set);
    notifyChanged();
}

void SLAOSets::addIgnoredAnim(const LLUUID& asset_id, const std::string& label)
{
    if (asset_id.isNull())
    {
        return;
    }
    if (!mIgnoredAnims.count(asset_id) && mIgnoredAnims.size() >= MAX_IGNORED_ANIMS)
    {
        mIgnoredAnims.erase(mIgnoredAnims.begin());
    }
    mIgnoredAnims[asset_id] = label;
    scheduleSave();
    notifyChanged();
}

void SLAOSets::removeIgnoredAnim(const LLUUID& asset_id)
{
    if (mIgnoredAnims.erase(asset_id))
    {
        scheduleSave();
        notifyChanged();
    }
}

std::string SLAOSets::getScriptedAOChoice(const LLUUID& attachment_item_id) const
{
    auto it = mScriptedAOChoices.find(attachment_item_id);
    return (it != mScriptedAOChoices.end()) ? it->second : std::string();
}

void SLAOSets::setScriptedAOChoice(const LLUUID& attachment_item_id, const std::string& choice)
{
    if (attachment_item_id.isNull())
    {
        return;
    }
    if (choice.empty())
    {
        mScriptedAOChoices.erase(attachment_item_id);
    }
    else
    {
        if (!mScriptedAOChoices.count(attachment_item_id) && mScriptedAOChoices.size() >= MAX_SCRIPTED_AO_CHOICES)
        {
            mScriptedAOChoices.erase(mScriptedAOChoices.begin());
        }
        mScriptedAOChoices[attachment_item_id] = choice;
    }
    scheduleSave();
    notifyChanged();
}

//-----------------------------------------------------------------------------
// Import
//-----------------------------------------------------------------------------

void SLAOSets::importNotecard(const LLUUID& notecard_item_id)
{
    LLViewerInventoryItem* item = gInventory.getItem(notecard_item_id);
    if (item && item->getIsLinkType())
    {
        item = item->getLinkedItem();
    }
    LLSD args;
    if (!item || item->getType() != LLAssetType::AT_NOTECARD)
    {
        LLNotificationsUtil::add("SLAOImportNotNotecard");
        return;
    }
    args["NAME"] = item->getName();
    if (mImports.size() >= MAX_IMPORTS)
    {
        LLNotificationsUtil::add("SLAOImportBusy", args);
        return;
    }
    if (item->getAssetUUID().isNull())
    {
        LLNotificationsUtil::add("SLAOImportEmpty", args);
        return;
    }

    LLUUID job_id;
    job_id.generate();
    ImportJob& job = mImports[job_id];
    job.mNotecardItemID = item->getUUID();
    job.mSourceFolderID = item->getParentUUID();
    job.mSetName = item->getName();
    job.mStarted = (F64)LLTimer::getElapsedSeconds();

    LLNotificationsUtil::add("SLAOImportStarted", args);

    gAssetStorage->getInvItemAsset(LLHost(),
                                   gAgent.getID(),
                                   gAgent.getSessionID(),
                                   item->getPermissions().getOwner(),
                                   LLUUID::null,
                                   item->getUUID(),
                                   item->getAssetUUID(),
                                   item->getType(),
                                   &SLAOSets::onNotecardAsset,
                                   new LLUUID(job_id),
                                   true);
    notifyChanged();
}

// static
void SLAOSets::onNotecardAsset(const LLUUID& asset_uuid, LLAssetType::EType type,
                               void* user_data, S32 status, LLExtStat ext_status)
{
    LLUUID* job_ptr = static_cast<LLUUID*>(user_data);
    const LLUUID job_id = job_ptr ? *job_ptr : LLUUID::null;
    delete job_ptr;

    if (!SLAOSets::instanceExists())
    {
        return;
    }

    std::string text;
    bool ok = false;
    if (status == 0)
    {
        LLFileSystem file(asset_uuid, type, LLFileSystem::READ);
        const S32 size = file.getSize();
        if (size > 0)
        {
            std::string buffer(size, '\0');
            file.read((U8*)&buffer[0], size);
            if (buffer.compare(0, 19, "Linden text version") == 0)
            {
                LLNotecard notecard(LLNotecard::MAX_SIZE);
                std::istringstream stream(buffer);
                if (notecard.importStream(stream))
                {
                    text = notecard.getText();
                    ok = true;
                }
            }
            else
            {
                text = buffer;
                ok = true;
            }
        }
    }
    else
    {
        LL_WARNS() << "AO notecard fetch failed, status " << status << LL_ENDL;
    }
    SLAOSets::instance().onNotecardLoaded(job_id, ok, text);
}

void SLAOSets::onNotecardLoaded(const LLUUID& job_id, bool ok, const std::string& text)
{
    auto it = mImports.find(job_id);
    if (it == mImports.end())
    {
        return;
    }
    ImportJob& job = it->second;
    LLSD args;
    args["NAME"] = job.mSetName;
    if (!ok)
    {
        LLNotificationsUtil::add("SLAOImportFailed", args);
        mImports.erase(it);
        notifyChanged();
        return;
    }
    if (!job.mNotecard.parse(text))
    {
        LLNotificationsUtil::add("SLAOImportNothing", args);
        mImports.erase(it);
        notifyChanged();
        return;
    }

    // Names resolve against the notecard's folder first, then the whole
    // inventory, so wait for both to be loaded (bounded).
    if (job.mSourceFolderID.notNull())
    {
        LLInventoryModelBackgroundFetch::instance().start(job.mSourceFolderID, true);
    }
    job.mStarted = (F64)LLTimer::getElapsedSeconds();
    doPeriodically([job_id]() -> bool
        {
            if (!SLAOSets::instanceExists())
            {
                return true;
            }
            SLAOSets& self = SLAOSets::instance();
            auto job_it = self.mImports.find(job_id);
            if (job_it == self.mImports.end())
            {
                return true;
            }
            const bool folder_ready = job_it->second.mSourceFolderID.isNull() ||
                                      tree_fetched(job_it->second.mSourceFolderID, 8);
            const bool all_ready = LLInventoryModelBackgroundFetch::instance().isEverythingFetched();
            const F64 waited = (F64)LLTimer::getElapsedSeconds() - job_it->second.mStarted;
            // Everything loaded; or the notecard's own folder is (where AO
            // animations normally are) and the rest is slow; or give up waiting
            if (all_ready || (folder_ready && waited > 5.0) || waited > IMPORT_FETCH_TIMEOUT_SECS)
            {
                self.finishImport(job_id);
                return true;
            }
            return false;
        }, 0.5f);
}

void SLAOSets::finishImport(const LLUUID& job_id)
{
    auto it = mImports.find(job_id);
    if (it == mImports.end())
    {
        return;
    }
    ImportJob job = it->second;
    mImports.erase(it);

    // name -> real item id, local folder first
    typedef std::map<std::string, LLUUID> name_map_t;
    name_map_t local_exact, local_lower, global_exact, global_lower;
    LLIsAnimation is_anim;
    auto collect = [&is_anim](const LLUUID& folder, name_map_t& exact, name_map_t& lower)
    {
        LLInventoryModel::cat_array_t cats;
        LLInventoryModel::item_array_t items;
        gInventory.collectDescendentsIf(folder, cats, items, LLInventoryModel::EXCLUDE_TRASH, is_anim);
        for (const LLPointer<LLViewerInventoryItem>& item : items)
        {
            const std::string& name = item->getName();
            const LLUUID target = item->getLinkedUUID();
            exact.emplace(name, target);
            std::string key = name;
            LLStringUtil::toLower(key);
            LLStringUtil::trim(key);
            lower.emplace(key, target);
        }
    };
    if (job.mSourceFolderID.notNull())
    {
        collect(job.mSourceFolderID, local_exact, local_lower);
    }
    collect(gInventory.getRootFolderID(), global_exact, global_lower);

    auto resolve = [&](const std::string& name) -> LLUUID
    {
        auto found = local_exact.find(name);
        if (found != local_exact.end())
        {
            return found->second;
        }
        found = global_exact.find(name);
        if (found != global_exact.end())
        {
            return found->second;
        }
        std::string key = name;
        LLStringUtil::toLower(key);
        LLStringUtil::trim(key);
        found = local_lower.find(key);
        if (found != local_lower.end())
        {
            return found->second;
        }
        found = global_lower.find(key);
        return (found != global_lower.end()) ? found->second : LLUUID::null;
    };

    std::vector<std::pair<EState, uuid_vec_t>> resolved;
    std::vector<std::string> missing;
    S32 found_count = 0;
    for (S32 i = 0; i < STATE_COUNT; ++i)
    {
        uuid_vec_t targets;
        for (const std::string& name : job.mNotecard.mNames[i])
        {
            const LLUUID target = resolve(name);
            if (target.isNull())
            {
                if (missing.size() < 64)
                {
                    missing.push_back(name);
                }
                continue;
            }
            if (std::find(targets.begin(), targets.end(), target) == targets.end())
            {
                targets.push_back(target);
                ++found_count;
            }
        }
        if (!targets.empty())
        {
            resolved.emplace_back((EState)i, targets);
        }
    }

    LLSD args;
    args["NAME"] = job.mSetName;
    args["FORMAT"] = job.mNotecard.mFormat;
    if (resolved.empty())
    {
        args["MISSING"] = (S32)missing.size();
        LLNotificationsUtil::add("SLAOImportNoAnims", args);
        notifyChanged();
        return;
    }

    std::string missing_list;
    for (size_t i = 0; i < missing.size() && i < 12; ++i)
    {
        missing_list += (missing_list.empty() ? "" : ", ") + missing[i];
    }
    if (missing.size() > 12)
    {
        missing_list += ", ...";
    }
    std::string ignored_list;
    for (const std::string& section : job.mNotecard.mIgnoredSections)
    {
        ignored_list += (ignored_list.empty() ? "" : ", ") + section;
    }

    const std::string set_name = job.mSetName;
    createSet(set_name, [resolved, found_count, missing, missing_list, ignored_list, args](const LLUUID& set_id) mutable
        {
            if (!SLAOSets::instanceExists())
            {
                return;
            }
            if (set_id.isNull())
            {
                LLNotificationsUtil::add("SLAOImportFailed", args);
                return;
            }
            SLAOSets& self = SLAOSets::instance();
            // Record the notecard order before the links exist
            LLSD& opts = self.mOptions[set_id.asString()];
            for (const auto& [state, targets] : resolved)
            {
                LLSD order = LLSD::emptyArray();
                for (const LLUUID& target : targets)
                {
                    order.append(target);
                }
                opts["states"][stateFolderName(state)]["order"] = order;
                self.queueLinks(set_id, state, targets);
            }
            self.scheduleSave();

            args["COUNT"] = found_count;
            args["STATES"] = (S32)resolved.size();
            args["MISSING_COUNT"] = (S32)missing.size();
            args["MISSING"] = missing_list;
            args["IGNORED"] = ignored_list;
            {
                LLStringUtil::format_map_t fmt;
                fmt["[SECTIONS]"] = ignored_list;
                args["IGNORED_LINE"] = ignored_list.empty() ? std::string()
                                                            : LLTrans::getString("SLAOImportIgnoredLine", fmt);
            }
            LLNotificationsUtil::add(missing.empty() ? "SLAOImportDone" : "SLAOImportDoneMissing", args);
        });
    notifyChanged();
}

bool SLAOSets::importFolder(const LLUUID& folder_id)
{
    const LLViewerInventoryCategory* folder = gInventory.getCategory(folder_id);
    if (!folder)
    {
        return false;
    }

    // 1. An AO notecard in the folder
    LLInventoryModel::cat_array_t* cats = nullptr;
    LLInventoryModel::item_array_t* items = nullptr;
    gInventory.getDirectDescendentsOf(folder_id, cats, items);
    if (items)
    {
        LLViewerInventoryItem* best = nullptr;
        for (const LLPointer<LLViewerInventoryItem>& item : *items)
        {
            if (item->getType() != LLAssetType::AT_NOTECARD)
            {
                continue;
            }
            // Prefer obvious AO configuration cards
            std::string lower = item->getName();
            LLStringUtil::toLower(lower);
            const bool likely = lower.find("ao") != std::string::npos || lower.find("zhao") != std::string::npos ||
                                lower.find("config") != std::string::npos || lower.find("set") == 0 ||
                                lower.find("default") != std::string::npos;
            if (!best || likely)
            {
                best = item;
                if (likely)
                {
                    break;
                }
            }
        }
        if (best)
        {
            importNotecard(best->getUUID());
            return true;
        }
    }

    // 2. State-named subfolders (another viewer's AO set layout)
    if (cats)
    {
        std::vector<std::pair<EState, uuid_vec_t>> states;
        for (const LLPointer<LLViewerInventoryCategory>& cat : *cats)
        {
            const EState state = stateFromFolderName(cat->getName());
            if (state == STATE_NONE)
            {
                continue;
            }
            LLInventoryModel::cat_array_t* sub_cats = nullptr;
            LLInventoryModel::item_array_t* sub_items = nullptr;
            gInventory.getDirectDescendentsOf(cat->getUUID(), sub_cats, sub_items);
            uuid_vec_t targets;
            if (sub_items)
            {
                for (const LLPointer<LLViewerInventoryItem>& item : *sub_items)
                {
                    if (is_animation(item))
                    {
                        targets.push_back(item->getLinkedUUID());
                    }
                }
            }
            if (!targets.empty())
            {
                states.emplace_back(state, targets);
            }
        }
        if (!states.empty())
        {
            const std::string name = display_name(folder->getName(), true);
            createSet(name, [states](const LLUUID& set_id)
                {
                    if (set_id.isNull() || !SLAOSets::instanceExists())
                    {
                        return;
                    }
                    for (const auto& [state, targets] : states)
                    {
                        SLAOSets::instance().queueLinks(set_id, state, targets);
                    }
                });
            return true;
        }
    }
    return false;
}

void SLAOSets::ensureCopiesFolder(const LLUUID& set_id, std::function<void(const LLUUID&)> cb)
{
    const SLAOSet* set = getSet(set_id);
    if (set && set->mCopiesFolderID.notNull())
    {
        cb(set->mCopiesFolderID);
        return;
    }
    if (set_id.isNull() || !gInventory.getCategory(set_id))
    {
        cb(LLUUID::null);
        return;
    }
    // Not scanned yet (new set): look for it directly
    const LLUUID existing = find_child_folder(set_id, COPIES_FOLDER_NAME);
    if (existing.notNull())
    {
        cb(existing);
        return;
    }
    auto& waiters = mCopiesFolderWaiters[set_id];
    waiters.push_back(cb);
    if (waiters.size() > 1)
    {
        return; // already being created
    }
    gInventory.createNewCategory(set_id, LLFolderType::FT_NONE, COPIES_FOLDER_NAME,
        [set_id](const LLUUID& new_id)
        {
            if (!SLAOSets::instanceExists())
            {
                return;
            }
            SLAOSets& self = SLAOSets::instance();
            std::vector<std::function<void(const LLUUID&)>> pending;
            pending.swap(self.mCopiesFolderWaiters[set_id]);
            self.mCopiesFolderWaiters.erase(set_id);
            for (auto& waiter : pending)
            {
                waiter(new_id);
            }
            self.requestRescan();
        });
}
