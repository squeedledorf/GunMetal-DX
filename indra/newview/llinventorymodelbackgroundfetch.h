/**
 * @file llinventorymodelbackgroundfetch.h
 * @brief LLInventoryModelBackgroundFetch class header file
 *
 * $LicenseInfo:firstyear=2002&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
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
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#ifndef LL_LLINVENTORYMODELBACKGROUNDFETCH_H
#define LL_LLINVENTORYMODELBACKGROUNDFETCH_H

#include <algorithm>
#include <deque>

#include "llsingleton.h"
#include "lltimer.h"
#include "lluuid.h"
#include "httpcommon.h"
#include "httprequest.h"
#include "httpoptions.h"
#include "httpheaders.h"
#include "httphandler.h"

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// Class LLInventoryModelBackgroundFetch
//
// This class handles background fetches, which are fetches of
// inventory folder.  Fetches can be recursive or not.
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
class LLInventoryModelBackgroundFetch : public LLSingleton<LLInventoryModelBackgroundFetch>
{
    LLSINGLETON(LLInventoryModelBackgroundFetch);
    ~LLInventoryModelBackgroundFetch();
public:

    // Start background breadth-first fetching of inventory contents.
    // This gets triggered when performing a filter-search.
    void start(const LLUUID& cat_id = LLUUID::null, bool recursive = true);
    void scheduleFolderFetch(const LLUUID& cat_id, bool forced = false);
    void scheduleItemFetch(const LLUUID& item_id, bool forced = false);

    typedef boost::function<void()> nullary_func_t;
    // AIS3 only, Fetches folder and everything links inside the folder point to
    // Intended for outfits
    void fetchFolderAndLinks(const LLUUID& cat_id, nullary_func_t callback);
    // AIS3 only
    void fetchCOF(nullary_func_t callback);

    bool folderFetchActive() const;
    bool isEverythingFetched() const; // completing the fetch once per session should be sufficient

    bool libraryFetchStarted() const;
    bool libraryFetchCompleted() const;
    bool libraryFetchInProgress() const;

    bool inventoryFetchStarted() const;
    bool inventoryFetchCompleted() const;
    bool inventoryFetchInProgress() const;

    void findLostItems();
    void incrFetchCount(S32 fetching);
    void incrFetchFolderCount(S32 fetching);

    bool isBulkFetchProcessingComplete() const;
    void setAllFoldersFetched();

    typedef boost::function<void()> folders_fetched_callback_t;
    boost::signals2::connection setFetchCompletionCallback(folders_fetched_callback_t cb);

    void addRequestAtFront(const LLUUID& id, bool recursive, bool is_category);
    void addRequestAtBack(const LLUUID& id, bool recursive, bool is_category);

protected:
    bool isFolderFetchProcessingComplete() const;

    typedef enum {
        FT_DEFAULT = 0,
        FT_FORCED, // request non-recursively even if already loaded
        FT_CONTENT_RECURSIVE, // request content recursively
        FT_FOLDER_AND_CONTENT, // request folder, then content recursively
        FT_RECURSIVE, // request everything recursively
    } EFetchType;
    struct FetchQueueInfo
    {
        FetchQueueInfo(const LLUUID& id, EFetchType recursive, bool is_category = true)
            : mUUID(id),
            mIsCategory(is_category),
            mFetchType(recursive),
            mQueuedAt(LLTimer::getTotalSeconds())
        {}

        LLUUID mUUID;
        bool mIsCategory;
        EFetchType mFetchType;
        F64 mQueuedAt; // #350: lets forced item refetches wait briefly for siblings
    };
    // std::deque with a modification counter, so answers derived from the queue
    // contents (fetchQueueContainsNoDescendentsOf) can be memoized. Every mutation goes
    // through this interface and bumps the counter; reads are const-only.
    class fetch_queue_t
    {
    public:
        typedef std::deque<FetchQueueInfo>::const_iterator const_iterator;
        template<typename... Args> void emplace_front(Args&&... args) { ++mVersion; mQueue.emplace_front(std::forward<Args>(args)...); }
        template<typename... Args> void emplace_back(Args&&... args) { ++mVersion; mQueue.emplace_back(std::forward<Args>(args)...); }
        void pop_front() { ++mVersion; mQueue.pop_front(); }
        void clear() { ++mVersion; mQueue.clear(); }
        template<typename Pred> size_t remove_if(Pred pred)
        {
            const size_t before = mQueue.size();
            mQueue.erase(std::remove_if(mQueue.begin(), mQueue.end(), pred), mQueue.end());
            if (mQueue.size() != before)
            {
                ++mVersion;
            }
            return before - mQueue.size();
        }
        bool empty() const { return mQueue.empty(); }
        size_t size() const { return mQueue.size(); }
        const FetchQueueInfo& front() const { return mQueue.front(); }
        const FetchQueueInfo& back() const { return mQueue.back(); }
        const_iterator begin() const { return mQueue.begin(); }
        const_iterator end() const { return mQueue.end(); }
        U64 version() const { return mVersion; }
    private:
        std::deque<FetchQueueInfo> mQueue;
        U64 mVersion = 0;
    };

    void onAISContentCalback(const LLUUID& request_id, const uuid_vec_t& content_ids, const LLUUID& response_id, EFetchType fetch_type);
    void onAISFolderCalback(const LLUUID& request_id, const LLUUID& response_id, EFetchType fetch_type);
    void bulkFetchViaAis();
    void bulkFetchViaAis(const FetchQueueInfo& fetch_info);
    // #350: replace >= SLInventoryCoalesceItemFetchMin queued item fetches that share a
    // parent folder with one AIS folder fetch (LL's LLInventoryFetchItemsObserver rule).
    void coalesceItemFetchesViaAis(U32 max_concurrent_fetches);
    void onAISCoalescedItemsCallback(const LLUUID& folder_id, const uuid_vec_t& item_ids,
                                     const uuid_vec_t& forced_ids, const LLUUID& response_id);
    void logItemFetchStats();
    void bulkFetch();

    void backgroundFetch();
    static void backgroundFetchCB(void*); // background fetch idle function

    bool fetchQueueContainsNoDescendentsOf(const LLUUID& cat_id) const;
    bool fetchQueueContainsNoDescendentsOfUncached(const LLUUID& cat_id) const;
    // Memo for fetchQueueContainsNoDescendentsOf(): inventory filters ask it once per
    // folder per pass during the initial fetch, and each answer walks both queues with
    // a parent-chain walk per entry. Valid while both queue versions and the inventory
    // structure generation are unchanged.
    struct NoDescendentsMemo
    {
        bool mValid = false;
        LLUUID mCatID;
        U64 mFolderQueueVersion = 0;
        U64 mItemQueueVersion = 0;
        U64 mModelGeneration = 0;
        bool mResult = false;
    };
    mutable NoDescendentsMemo mNoDescendentsMemo[2];

private:
    bool mRecursiveInventoryFetchStarted;
    bool mRecursiveLibraryFetchStarted;
    bool mRecursiveMarketplaceFetchStarted; // AIS3 specific
    bool mAllRecursiveFoldersFetched;
    typedef boost::signals2::signal<void()> folders_fetched_signal_t;
    folders_fetched_signal_t mFoldersFetchedSignal;

    bool mBackgroundFetchActive;
    bool mFolderFetchActive;
    S32 mFetchCount;
    S32 mLastFetchCount; // for debug
    S32 mFetchFolderCount;

    LLFrameTimer mFetchTimer;
    F32 mMinTimeBetweenFetches;
    fetch_queue_t mFetchFolderQueue;
    fetch_queue_t mFetchItemQueue;
    uuid_set_t mForceFetchSet;
    std::list<LLUUID> mExpectedFolderIds; // for debug, should this track time?

    // #350 item fetch coalescing.
    // Items whose coalesced folder fetch failed: fetched one by one from then on (one
    // fallback, no loop). Cleared when the item queue drains.
    uuid_set_t mNoCoalesceItems;
    struct ItemFetchStats
    {
        U32 mSingle = 0;            // GET /item/<id>
        U32 mCoalescedRequests = 0; // GET /category/<id>/children?depth=0 issued instead
        U32 mCoalescedItems = 0;    // item fetches those replaced
        U32 mFallbacks = 0;         // items re-queued singly after a failed/moved coalesced fetch
        LLTimer mTimer;
    };
    ItemFetchStats mItemFetchStats;
};

#endif // LL_LLINVENTORYMODELBACKGROUNDFETCH_H

