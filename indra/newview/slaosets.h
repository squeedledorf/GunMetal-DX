/**
 * @file slaosets.h
 * @brief Client-side animation overrider (#496): AO sets in inventory,
 *        per-account options, editing and notecard import.
 *
 * Storage
 *  - Animations live in inventory, so they follow the account to every
 *    machine: <root>/<set name>/<state name>/<links to animations>.
 *    Our root is "#Slipstream AO" at the top of My Inventory. Sets found
 *    under "#Firestorm/#AO" (same layout) are used in place.
 *  - Options (sit override, cycling, order, ...) are per-account, in
 *    sl_ao.xml next to the other per-account files, keyed by the set and
 *    state folder UUIDs. Inventory UUIDs are stable across machines, so a
 *    copied sl_ao.xml applies anywhere. Keeping options out of inventory
 *    means no asset upload per toggle, no notecard write races, instant
 *    startup, and no risk of mangling another viewer's folder flags.
 *  - The active set is also snapshotted (asset ids per state) in the same
 *    file, so the AO works right after login while inventory still loads.
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

#ifndef SL_AOSETS_H
#define SL_AOSETS_H

#include <functional>
#include <map>
#include <string>
#include <vector>

#include <boost/signals2.hpp>

#include "llassettype.h"
#include "llextendedstatus.h"
#include "llinventoryobserver.h"
#include "llsingleton.h"
#include "slaoset.h"

class SLAOSets
    : public LLSingleton<SLAOSets>
    , public LLInventoryObserver
{
    LLSINGLETON(SLAOSets);
    ~SLAOSets() override;
    LOG_CLASS(SLAOSets);

public:
    typedef std::map<LLUUID, SLAOSet> set_map_t; // keyed by set folder id

    static const std::string ROOT_FOLDER_NAME;
    // Subfolder of a set holding real copies taken from an object's contents
    static const std::string COPIES_FOLDER_NAME;

    // The set's copies folder, created on first use (callback gets null on failure).
    void ensureCopiesFolder(const LLUUID& set_id, std::function<void(const LLUUID&)> cb);

    // Called once per login, after the per-account directory is known.
    void loadFromDisk();

    const set_map_t& getSets() const { return mSets; }
    const SLAOSet* getSet(const LLUUID& set_id) const;
    const SLAOSet* findSetByName(const std::string& name) const; // case-insensitive
    // Sets in display order (by name).
    std::vector<const SLAOSet*> getSortedSets() const;
    // True once the AO roots and their contents were scanned from a loaded
    // inventory (before that only the login snapshot is available).
    bool isInventoryScanned() const { return mScanned; }

    // Editing. All inventory operations are asynchronous; the changed signal
    // fires once the inventory reflects them.
    void createSet(const std::string& name, std::function<void(const LLUUID&)> cb = nullptr);
    void renameSet(const LLUUID& set_id, const std::string& name);
    void deleteSet(const LLUUID& set_id);              // moves the set folder to the trash
    void addAnimations(const LLUUID& set_id, SLAO::EState state, const uuid_vec_t& item_ids);
    void removeAnimation(const LLUUID& set_id, SLAO::EState state, const LLUUID& item_id);
    void moveAnimation(const LLUUID& set_id, SLAO::EState state, const LLUUID& item_id, S32 delta);
    void setStateConfig(const LLUUID& set_id, SLAO::EState state, const SLAOStateConfig& config);
    void setSetOptions(const LLUUID& set_id, bool sit_override, bool smart_sit,
                       bool no_stands_mouselook, bool no_stands_typing);
    void setAlwaysEnabled(const LLUUID& set_id, bool enabled);
    void setStopSystemAnims(const LLUUID& set_id, bool enabled);
    void setIdleOptions(const LLUUID& set_id, bool enabled, F32 min_delay, F32 max_delay,
                        F32 max_duration, bool while_sitting);

    // Import an AO notecard (ZHAO-II / Oracul) as a new set; animations are
    // linked, never moved or copied. Result is reported as a notification.
    void importNotecard(const LLUUID& notecard_item_id);
    // Import a folder: an AO notecard inside it, else state-named subfolders,
    // else nothing (returns false).
    bool importFolder(const LLUUID& folder_id);
    bool isImporting() const { return !mImports.empty(); }

    // Remembered per-account choices about worn scripted AOs ("pause"/"ignore").
    std::string getScriptedAOChoice(const LLUUID& attachment_item_id) const;
    void setScriptedAOChoice(const LLUUID& attachment_item_id, const std::string& choice);
    const std::map<LLUUID, std::string>& getScriptedAOChoices() const { return mScriptedAOChoices; }

    // Per-account list of animations (asset ids) that never pause the AO
    bool isIgnoredAnim(const LLUUID& asset_id) const { return mIgnoredAnims.count(asset_id) != 0; }
    void addIgnoredAnim(const LLUUID& asset_id, const std::string& label);
    void removeIgnoredAnim(const LLUUID& asset_id);
    const std::map<LLUUID, std::string>& getIgnoredAnims() const { return mIgnoredAnims; }

    // Request a (debounced) inventory rescan.
    void requestRescan();

    typedef boost::signals2::signal<void()> changed_signal_t;
    boost::signals2::connection setChangedCallback(const changed_signal_t::slot_type& cb)
    {
        return mChangedSignal.connect(cb);
    }

    // LLInventoryObserver
    void changed(U32 mask) override;

    // Import state, shared with the asset callback
    struct ImportJob
    {
        LLUUID      mNotecardItemID;
        LLUUID      mSourceFolderID;
        std::string mSetName;
        SLAONotecard mNotecard;
        F64         mStarted = 0.0;
        bool        mWaitingForInventory = false;
    };

protected:
    void initSingleton() override;
    void cleanupSingleton() override;

private:
    std::string getFilename() const;
    void scheduleSave();
    void saveToDisk();
    void notifyChanged();

    LLUUID findRootFolder() const;
    LLUUID findOtherViewerRoot() const;
    void ensureRootFolder(std::function<void(const LLUUID&)> cb);
    void fetchRoots();
    void rescan();
    void scanRoot(const LLUUID& root_id, bool other_viewer, set_map_t& out) const;
    void applyOptions(SLAOSet& set) const;
    void storeOptions(const SLAOSet& set);
    std::string uniqueSetName(const std::string& wanted) const;

    void queueLinks(const LLUUID& set_id, SLAO::EState state, const uuid_vec_t& target_ids);
    void flushPendingLinks(const LLUUID& set_id, SLAO::EState state, const LLUUID& state_folder_id);

    void onNotecardLoaded(const LLUUID& job_id, bool ok, const std::string& text);
    void finishImport(const LLUUID& job_id);
    static void onNotecardAsset(const LLUUID& asset_uuid, LLAssetType::EType type,
                                void* user_data, S32 status, LLExtStat ext_status);

    set_map_t mSets;
    bool mLoaded = false;
    bool mScanned = false;
    bool mRescanScheduled = false;
    bool mRetryScheduled = false;
    bool mSaveScheduled = false;
    bool mRootFetchRequested = false;
    bool mCreatingRoot = false;
    std::vector<std::function<void(const LLUUID&)>> mRootWaiters;

    // Per-account options, keyed by set folder id (LLSD maps)
    LLSD mOptions;
    // Snapshot of the active set for use before inventory is loaded
    LLSD mSnapshot;
    std::map<LLUUID, std::string> mScriptedAOChoices;
    std::map<LLUUID, std::string> mIgnoredAnims;    // asset id -> where it was seen

    // Links waiting for a state folder that is being created
    struct PendingKey
    {
        LLUUID mSetID;
        S32 mState;
        bool operator<(const PendingKey& other) const
        {
            return (mSetID != other.mSetID) ? (mSetID < other.mSetID) : (mState < other.mState);
        }
    };
    std::map<PendingKey, uuid_vec_t> mPendingLinks;

    std::map<LLUUID, ImportJob> mImports; // keyed by a random job id
    std::map<LLUUID, std::vector<std::function<void(const LLUUID&)>>> mCopiesFolderWaiters;

    changed_signal_t mChangedSignal;

    friend class SLAOEngine;
public:
    // Engine helper: the snapshot is refreshed whenever the active set changes.
    void updateSnapshot(const LLUUID& set_id);
};

#endif // SL_AOSETS_H
