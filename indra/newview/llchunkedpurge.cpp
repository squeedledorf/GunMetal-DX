/**
 * @file llchunkedpurge.cpp
 * @brief Deletes a folder tree in parts when the server refuses to delete it
 *        in one request ("Too many subcategories in request").
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors.
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
#include "llchunkedpurge.h"

#include "llaisapi.h"
#include "llappviewer.h"
#include "llcallbacklist.h"
#include "llfoldertype.h"
#include "llinventorymodel.h"
#include "llinventorymodelbackgroundfetch.h"
#include "llnotificationsutil.h"
#include "lltimer.h"
#include "llviewerinventory.h"

#include <deque>
#include <map>
#include <set>
#include <utility>
#include <vector>

///----------------------------------------------------------------------------
/// How it works
///
/// Each step snapshots the folder tree under the root from the local model,
/// then sends ONE request:
///  - the final request (the user's original PurgeDescendents(root), or
///    RemoveCategory(root) in REMOVE_FOLDER mode) once the whole remainder has
///    at most `threshold` subfolders and was not already refused at that size;
///  - otherwise RemoveCategory(F) for the first folder F (depth-first) under the
///    root whose subtree has at most `threshold` folders and that was never sent
///    before. Bigger subtrees are descended into.
/// The next step runs from the idle pump once the reply arrived; by then
/// AISUpdate has applied the server's reply to the model, so the next snapshot
/// reflects exactly what the server removed. On a "too many subcategories"
/// refusal the threshold halves (min 1) and the refused folder is split into
/// its pieces by the next plan; no request is ever sent twice.
///----------------------------------------------------------------------------

namespace
{
    constexpr S32 CHUNK_INITIAL_THRESHOLD = 500;   // folders per request to start with
    constexpr F32 CHUNK_PROGRESS_INTERVAL = 10.f;  // seconds between progress tips
    constexpr F32 CHUNK_FETCH_TIMEOUT = 120.f;     // wait for a background fetch of the root
    constexpr F32 CHUNK_COPY_RECHECK = 1.f;        // re-plan interval while blocked by copies
    constexpr F32 CHUNK_COPY_WAIT_LOG = 30.f;      // log interval while blocked by copies
    constexpr F32 CHUNK_REPLY_WATCHDOG = 600.f;    // AIS's own HTTP timeout is much shorter

    enum EPhase
    {
        PHASE_PLAN,
        PHASE_WAIT_REPLY,
        PHASE_WAIT_COPIES,
        PHASE_WAIT_FETCH
    };

    enum ERequest
    {
        REQ_REMOVE_SUBTREE,     // RemoveCategory(folder under the root)
        REQ_FINAL_PURGE_ROOT,   // PurgeDescendents(root)    (PURGE_CHILDREN mode)
        REQ_FINAL_REMOVE_ROOT   // RemoveCategory(root)      (REMOVE_FOLDER mode)
    };

    struct Job
    {
        LLUUID mRoot;
        LLChunkedPurge::EMode mMode = LLChunkedPurge::PURGE_CHILDREN;
        LLUUID mRootParent;     // the root must stay where the user confirmed it
        std::string mRootName;
        std::vector<LLPointer<LLInventoryCallback> > mDoneCBs;
    };

    struct State
    {
        std::deque<Job> mJobs;          // front() is the running job
        bool mIdleRegistered = false;
        bool mJobStarted = false;

        EPhase mPhase = PHASE_PLAN;
        S32 mThreshold = CHUNK_INITIAL_THRESHOLD;
        std::map<LLUUID, S32> mRefusedAt;       // folder -> its folder count (incl. itself) when refused
        std::set<LLUUID> mSucceeded;            // folders whose request succeeded
        std::set<LLUUID> mSkipped;              // failed the pre-send re-check
        bool mFetchTried = false;

        // The request in flight.
        U64 mToken = 0;
        LLUUID mTarget;
        S32 mTargetFolders = 0;
        ERequest mRequest = REQ_REMOVE_SUBTREE;
        bool mReplied = false;
        bool mReplyOK = false;
        S32 mReplyStatus = 0;
        std::string mReplyError;

        // Current job's stats.
        S32 mStartFolders = 0;
        S32 mStartItems = 0;
        U32 mRequestsOK = 0;
        U32 mRefusals = 0;
        LLTimer mJobTimer;
        LLTimer mProgressTimer;
        LLTimer mPhaseTimer;
        LLTimer mCopyLogTimer;
    };

    // Never destroyed: holds callers' callbacks (shutdown() releases them).
    State& state()
    {
        static State* sState = new State;
        return *sState;
    }

    struct Node
    {
        LLUUID mID;
        S32 mFolders = 1;           // folders in this subtree, including this one
        bool mProtected = false;    // this folder is a protected (system) type
        bool mHasProtected = false; // this folder or one below it is
    };

    // Pre-order snapshot of the folder tree under root (root at index 0). The
    // subtree of node i is the contiguous range [i, i + mFolders). Iterative on
    // purpose: trees left by runaway copies can be thousands of levels deep.
    void snapshot_tree(const LLUUID& root, std::vector<Node>& nodes, S32& items_total)
    {
        nodes.clear();
        items_total = 0;
        std::vector<S32> parent_of;
        std::vector<std::pair<LLUUID, S32> > stack;
        stack.emplace_back(root, -1);
        while (!stack.empty())
        {
            const std::pair<LLUUID, S32> entry = stack.back();
            stack.pop_back();
            const S32 index = (S32)nodes.size();

            Node n;
            n.mID = entry.first;
            const LLViewerInventoryCategory* cat = gInventory.getCategory(entry.first);
            n.mProtected = cat && LLFolderType::lookupIsProtectedType(cat->getPreferredType());
            n.mHasProtected = n.mProtected;
            nodes.push_back(n);
            parent_of.push_back(entry.second);

            LLInventoryModel::cat_array_t* cats = nullptr;
            LLInventoryModel::item_array_t* items = nullptr;
            gInventory.getDirectDescendentsOf(entry.first, cats, items);
            if (items)
            {
                items_total += (S32)items->size();
            }
            if (cats)
            {
                // Reversed so the first child is visited first.
                for (auto it = cats->rbegin(); it != cats->rend(); ++it)
                {
                    if (it->notNull())
                    {
                        stack.emplace_back((*it)->getUUID(), index);
                    }
                }
            }
        }
        // Children always have larger indices than their parent.
        for (S32 i = (S32)nodes.size() - 1; i > 0; --i)
        {
            Node& parent = nodes[parent_of[i]];
            parent.mFolders += nodes[i].mFolders;
            parent.mHasProtected = parent.mHasProtected || nodes[i].mHasProtected;
        }
    }

    struct Plan
    {
        enum EKind { NONE, TARGET, FINAL, DONE } mKind = NONE;
        LLUUID mTarget;
        S32 mTargetFolders = 0;
        S32 mFoldersLeft = 0;       // folders under the root (root excluded)
        S32 mItemsLeft = 0;
        bool mBlockedByCopies = false;
        bool mProtectedRemains = false;
    };

    // Never the identical request twice: a folder whose request succeeded is
    // never targeted again, and a refused folder only once its tree has shrunk
    // below the size the server refused (so it is a different, smaller request).
    bool may_target(const LLUUID& id, S32 folders)
    {
        const State& s = state();
        if (folders > s.mThreshold || s.mSucceeded.count(id))
        {
            return false;
        }
        auto refused = s.mRefusedAt.find(id);
        return refused == s.mRefusedAt.end() || folders < refused->second;
    }

    Plan make_plan(const Job& job)
    {
        State& s = state();
        Plan p;
        std::vector<Node> nodes;
        snapshot_tree(job.mRoot, nodes, p.mItemsLeft);
        p.mFoldersLeft = nodes[0].mFolders - 1;
        for (size_t i = 1; i < nodes.size(); ++i)
        {
            if (nodes[i].mProtected)
            {
                p.mProtectedRemains = true;
                break;
            }
        }

        // Nothing under the root can be a copy source unless the root itself
        // reports one: skip the per-folder checks (each walks the pending
        // sources) in the common case, where they would cost seconds per plan.
        const bool copies_under_root = LLInventoryCopyPacer::hasPendingCopyFrom(job.mRoot);

        if (job.mMode == LLChunkedPurge::PURGE_CHILDREN && p.mFoldersLeft == 0 && p.mItemsLeft == 0)
        {
            p.mKind = Plan::DONE;
            return p;
        }

        // The original request on the whole remainder, once it is small enough
        // and smaller than when the server refused it.
        // (The root is compared by its subfolders: it is never removed on its own
        // in PURGE_CHILDREN mode, and in REMOVE_FOLDER mode it is the last one.)
        if (p.mFoldersLeft <= s.mThreshold && may_target(job.mRoot, nodes[0].mFolders) && !p.mProtectedRemains)
        {
            if (!copies_under_root)
            {
                p.mKind = Plan::FINAL;
                p.mTarget = job.mRoot;
                p.mTargetFolders = nodes[0].mFolders;
                return p;
            }
            p.mBlockedByCopies = true;
        }

        // Otherwise the LARGEST subtree under the root that fits the working size, so each request
        // removes as much as the server allows (picking the first fit in pre-order deleted wide
        // trees one small leaf folder at a time). A fitting subtree is taken whole: its pieces are
        // never considered on their own, since they are smaller.
        size_t best = 0;
        size_t i = 1;
        while (i < nodes.size())
        {
            const Node& n = nodes[i];
            if (n.mProtected || s.mSkipped.count(n.mID))
            {
                i += n.mFolders;    // never touch a system folder or anything in it
                continue;
            }
            if (!n.mHasProtected && may_target(n.mID, n.mFolders))
            {
                if (!copies_under_root || !LLInventoryCopyPacer::hasPendingCopyFrom(n.mID))
                {
                    if (!best || n.mFolders > nodes[best].mFolders)
                    {
                        best = i;
                    }
                    i += n.mFolders;    // its pieces are smaller than it
                    continue;
                }
                p.mBlockedByCopies = true;
            }
            ++i;    // too big, refused, blocked or holds a system folder: try its pieces
        }
        if (best)
        {
            p.mKind = Plan::TARGET;
            p.mTarget = nodes[best].mID;
            p.mTargetFolders = nodes[best].mFolders;
        }
        return p;
    }

    void idle_pump(void*);

    void register_idle()
    {
        State& s = state();
        if (!s.mIdleRegistered)
        {
            gIdleCallbacks.addFunction(idle_pump, nullptr);
            s.mIdleRegistered = true;
        }
    }

    void unregister_idle()
    {
        State& s = state();
        if (s.mIdleRegistered)
        {
            gIdleCallbacks.deleteFunction(idle_pump, nullptr);
            s.mIdleRegistered = false;
        }
    }

    void reset_job_state()
    {
        State& s = state();
        s.mJobStarted = false;
        s.mPhase = PHASE_PLAN;
        s.mRefusedAt.clear();
        s.mSucceeded.clear();
        s.mSkipped.clear();
        s.mFetchTried = false;
        s.mTarget.setNull();
        s.mReplied = false;
        s.mRequestsOK = 0;
        s.mRefusals = 0;
        // mThreshold is kept: the server's limit does not change between jobs.
        // mToken keeps counting so a late reply never matches a new request.
    }

    void count_remaining(const LLUUID& root, S32& folders, S32& items)
    {
        folders = 0;
        items = 0;
        if (gInventory.getCategory(root))
        {
            std::vector<Node> nodes;
            snapshot_tree(root, nodes, items);
            folders = nodes[0].mFolders - 1;
        }
    }

    void fire_done(Job& job)
    {
        for (LLPointer<LLInventoryCallback>& cb : job.mDoneCBs)
        {
            if (cb.notNull())
            {
                cb->fire(LLUUID::null);
            }
        }
        job.mDoneCBs.clear();
    }

    void finish_job(const std::string& how)
    {
        State& s = state();
        Job job = s.mJobs.front();
        s.mJobs.pop_front();
        S32 folders = 0, items = 0;
        count_remaining(job.mRoot, folders, items);
        LL_INFOS("ChunkedPurge") << "Finished '" << job.mRootName << "' (" << job.mRoot << "): " << how
                                 << "; " << s.mRequestsOK << " requests succeeded, " << s.mRefusals
                                 << " refused and split, working size " << s.mThreshold << " folders, "
                                 << s.mJobTimer.getElapsedTimeF32() << "s; left in the model: "
                                 << folders << " folders, " << items << " items" << LL_ENDL;
        LLSD args;
        args["FOLDER"] = job.mRootName;
        args["REQUESTS"] = (S32)s.mRequestsOK;
        LLNotificationsUtil::add("SLChunkedPurgeDone", args);
        fire_done(job);
        reset_job_state();
        if (s.mJobs.empty())
        {
            unregister_idle();
        }
    }

    // Any failure other than the "too many subcategories" refusal ends
    // everything: the running job and the queued ones.
    void stop_all(const std::string& reason)
    {
        State& s = state();
        bool first = true;
        while (!s.mJobs.empty())
        {
            Job job = s.mJobs.front();
            s.mJobs.pop_front();
            S32 folders = 0, items = 0;
            count_remaining(job.mRoot, folders, items);
            LL_WARNS("ChunkedPurge") << "Stopped '" << job.mRootName << "' (" << job.mRoot << "): "
                                     << (first ? reason : std::string("an earlier job stopped (") + reason + ")")
                                     << "; " << (first ? s.mRequestsOK : 0) << " requests had succeeded; still there: "
                                     << folders << " folders, " << items << " items" << LL_ENDL;
            LLSD args;
            args["FOLDER"] = job.mRootName;
            args["REASON"] = first ? reason : std::string("an earlier delete in parts stopped");
            args["FOLDERS"] = folders;
            args["ITEMS"] = items;
            LLNotificationsUtil::add("SLChunkedPurgeStopped", args);
            fire_done(job);
            first = false;
        }
        reset_job_state();
        unregister_idle();
    }

    void begin_job(Job& job)
    {
        State& s = state();
        reset_job_state();
        s.mJobStarted = true;
        count_remaining(job.mRoot, s.mStartFolders, s.mStartItems);
        // The user's own request on the root was just refused at this size: the
        // final request may only go out once the tree is smaller than that.
        const S32 root_folders = s.mStartFolders + 1;
        s.mRefusedAt[job.mRoot] = root_folders;
        if (root_folders <= s.mThreshold)
        {
            s.mThreshold = llmax(1, root_folders / 2);
        }
        s.mJobTimer.reset();
        s.mProgressTimer.reset();
        LL_INFOS("ChunkedPurge") << "Start: '" << job.mRootName << "' (" << job.mRoot << ", "
                                 << (job.mMode == LLChunkedPurge::PURGE_CHILDREN ? "empty it" : "delete it")
                                 << "), " << s.mStartFolders << " folders and " << s.mStartItems
                                 << " items in the model; working size " << s.mThreshold << " folders" << LL_ENDL;
        LLSD args;
        args["FOLDER"] = job.mRootName;
        args["FOLDERS"] = s.mStartFolders;
        args["ITEMS"] = s.mStartItems;
        LLNotificationsUtil::add("SLChunkedPurgeStarted", args);
    }

    void on_reply(U64 token, bool ok, S32 status, const std::string& error)
    {
        State& s = state();
        if (s.mJobs.empty() || token != s.mToken || s.mPhase != PHASE_WAIT_REPLY || s.mReplied)
        {
            LL_INFOS("ChunkedPurge") << "Ignoring a reply for a request no longer awaited (token " << token
                                     << ", " << (ok ? "success" : "failure") << ")" << LL_ENDL;
            return;
        }
        // Only record it: the next request goes out from the idle pump, not
        // from inside the AIS coroutine.
        s.mReplied = true;
        s.mReplyOK = ok;
        s.mReplyStatus = status;
        s.mReplyError = error;
    }

    // The confinement re-check, against the model as it is right now. Returns
    // an empty string when target may be sent the given request.
    std::string recheck_target(const Job& job, const LLUUID& target, ERequest request)
    {
        const LLViewerInventoryCategory* cat = gInventory.getCategory(target);
        if (!cat)
        {
            return "it no longer exists";
        }
        if (LLFolderType::lookupIsProtectedType(cat->getPreferredType()))
        {
            if (target != job.mRoot || request == REQ_FINAL_REMOVE_ROOT)
            {
                return "it is a system folder";
            }
        }
        if (target == job.mRoot)
        {
            if (request == REQ_REMOVE_SUBTREE)
            {
                return "it is the confirmed folder itself";
            }
            if (request == REQ_FINAL_REMOVE_ROOT && job.mMode != LLChunkedPurge::REMOVE_FOLDER)
            {
                return "removing the confirmed folder itself was not requested";
            }
            if (cat->getParentUUID() != job.mRootParent)
            {
                return "it was moved";
            }
        }
        else
        {
            if (request != REQ_REMOVE_SUBTREE)
            {
                return "internal error: final request on a non-root folder";
            }
            if (!gInventory.isObjectDescendentOf(target, job.mRoot))
            {
                return "it is no longer inside the folder being emptied";
            }
        }
        if (LLInventoryCopyPacer::hasPendingCopyFrom(target))
        {
            return "item copies from it are still in progress";
        }
        return std::string();
    }

    void send_request(Job& job, const LLUUID& target, S32 folders, ERequest request)
    {
        State& s = state();
        const std::string why_not = recheck_target(job, target, request);
        if (!why_not.empty())
        {
            if (target == job.mRoot)
            {
                stop_all("'" + job.mRootName + "': " + why_not);
                return;
            }
            LL_INFOS("ChunkedPurge") << "Skipping folder " << target << ": " << why_not << LL_ENDL;
            s.mSkipped.insert(target);
            s.mPhase = PHASE_PLAN;
            return;
        }
        if (!AISAPI::isAvailable())
        {
            stop_all("the inventory service is not available");
            return;
        }

        const U64 token = ++s.mToken;
        s.mTarget = target;
        s.mTargetFolders = folders;
        s.mRequest = request;
        s.mReplied = false;
        s.mReplyOK = false;
        s.mReplyStatus = 0;
        s.mReplyError.clear();
        s.mPhase = PHASE_WAIT_REPLY;    // before the call: a failure may be reported synchronously
        s.mPhaseTimer.reset();

        const LLViewerInventoryCategory* cat = gInventory.getCategory(target);
        LL_INFOS("ChunkedPurge") << "Request " << (s.mRequestsOK + s.mRefusals + 1) << ": "
                                 << (request == REQ_FINAL_PURGE_ROOT ? "PurgeDescendents" : "RemoveCategory")
                                 << " " << target << " '" << (cat ? cat->getName() : std::string()) << "' ("
                                 << (request == REQ_REMOVE_SUBTREE ? folders : folders - 1)
                                 << (request == REQ_REMOVE_SUBTREE ? " folders" : " subfolders") << ")" << LL_ENDL;

        AISAPI::completion_t on_ok = [token](const LLUUID&) { on_reply(token, true, 0, std::string()); };
        AISAPI::failure_t on_fail = [token](S32 status, const std::string& error) { on_reply(token, false, status, error); };
        if (request == REQ_FINAL_PURGE_ROOT)
        {
            AISAPI::PurgeDescendents(target, on_ok, on_fail);
        }
        else
        {
            AISAPI::RemoveCategory(target, on_ok, on_fail);
        }
    }

    void handle_reply(Job& job)
    {
        State& s = state();
        s.mPhase = PHASE_PLAN;
        if (s.mReplyOK)
        {
            ++s.mRequestsOK;
            s.mSucceeded.insert(s.mTarget);
            if (s.mRequest != REQ_REMOVE_SUBTREE)
            {
                finish_job("final request succeeded");
                return;
            }
            if (gInventory.getCategory(s.mTarget))
            {
                // The model must follow the server's reply; if it did not, our
                // picture of what is left is wrong, so do not guess further.
                stop_all("the viewer's inventory did not reflect the server's reply (relog to resync)");
            }
            return;
        }

        if (AISAPI::isTooManySubcategories(s.mReplyStatus, s.mReplyError))
        {
            ++s.mRefusals;
            const S32 old_threshold = s.mThreshold;
            s.mThreshold = llmax(1, llmin(s.mThreshold, s.mTargetFolders) / 2);
            s.mRefusedAt[s.mTarget] = s.mTargetFolders;
            LL_INFOS("ChunkedPurge") << "Refused (" << s.mReplyStatus << " " << s.mReplyError << ") for "
                                     << s.mTarget << " with " << s.mTargetFolders << " folders; working size "
                                     << old_threshold << " -> " << s.mThreshold
                                     << " folders, splitting it into smaller requests" << LL_ENDL;
            return;
        }

        stop_all(llformat("the server returned an error (%d%s%s)", s.mReplyStatus,
                          s.mReplyError.empty() ? "" : ": ", s.mReplyError.c_str()));
    }

    void plan_and_send(Job& job)
    {
        State& s = state();
        const LLViewerInventoryCategory* root = gInventory.getCategory(job.mRoot);
        if (!root)
        {
            if (job.mMode == LLChunkedPurge::REMOVE_FOLDER)
            {
                finish_job("the folder is gone");
            }
            else
            {
                stop_all("'" + job.mRootName + "' no longer exists");
            }
            return;
        }
        if (root->getParentUUID() != job.mRootParent)
        {
            stop_all("'" + job.mRootName + "' was moved");
            return;
        }

        const Plan plan = make_plan(job);

        if (s.mProgressTimer.getElapsedTimeF32() > CHUNK_PROGRESS_INTERVAL)
        {
            s.mProgressTimer.reset();
            LLSD args;
            args["FOLDER"] = job.mRootName;
            args["COUNT"] = plan.mFoldersLeft;
            args["ITEMS"] = plan.mItemsLeft;
            LLNotificationsUtil::add("SLChunkedPurgeProgress", args);
        }

        switch (plan.mKind)
        {
        case Plan::DONE:
            finish_job("nothing left");
            return;
        case Plan::FINAL:
            send_request(job, plan.mTarget, plan.mTargetFolders,
                         job.mMode == LLChunkedPurge::PURGE_CHILDREN ? REQ_FINAL_PURGE_ROOT : REQ_FINAL_REMOVE_ROOT);
            return;
        case Plan::TARGET:
            send_request(job, plan.mTarget, plan.mTargetFolders, REQ_REMOVE_SUBTREE);
            return;
        case Plan::NONE:
            break;
        }

        if (plan.mBlockedByCopies)
        {
            if (s.mPhase != PHASE_WAIT_COPIES || s.mCopyLogTimer.getElapsedTimeF32() > CHUNK_COPY_WAIT_LOG)
            {
                LL_INFOS("ChunkedPurge") << "Waiting: item copies from inside '" << job.mRootName
                                         << "' are in progress (" << LLInventoryCopyPacer::pendingCount()
                                         << " copies pending)" << LL_ENDL;
                s.mCopyLogTimer.reset();
            }
            s.mPhase = PHASE_WAIT_COPIES;
            s.mPhaseTimer.reset();
            return;
        }
        if (!s.mFetchTried && !plan.mProtectedRemains)
        {
            // The server sees more subfolders than the model: load the tree
            // (read-only) and plan again.
            s.mFetchTried = true;
            LL_INFOS("ChunkedPurge") << "Nothing left to split in the model under " << job.mRoot
                                     << " but the server still refuses; fetching the folder tree" << LL_ENDL;
            LLInventoryModelBackgroundFetch::instance().start(job.mRoot, true);
            s.mPhase = PHASE_WAIT_FETCH;
            s.mPhaseTimer.reset();
            return;
        }
        stop_all(plan.mProtectedRemains
                     ? std::string("it contains a system folder, which cannot be deleted")
                     : std::string("the server refuses even the smallest piece the viewer can see"));
    }

    void idle_pump(void*)
    {
        State& s = state();
        if (s.mJobs.empty())
        {
            unregister_idle();
            return;
        }
        if (gDisconnected || LLApp::isExiting())
        {
            LLChunkedPurge::shutdown();
            return;
        }

        Job& job = s.mJobs.front();
        if (!s.mJobStarted)
        {
            begin_job(job);
        }

        switch (s.mPhase)
        {
        case PHASE_WAIT_REPLY:
            if (s.mReplied)
            {
                handle_reply(job);  // the next request goes out next frame
            }
            else if (s.mPhaseTimer.getElapsedTimeF32() > CHUNK_REPLY_WATCHDOG)
            {
                stop_all("no reply from the server");
            }
            return;
        case PHASE_WAIT_FETCH:
            if (LLInventoryModelBackgroundFetch::instance().folderFetchActive()
                && s.mPhaseTimer.getElapsedTimeF32() < CHUNK_FETCH_TIMEOUT)
            {
                return;
            }
            break;
        case PHASE_WAIT_COPIES:
            if (s.mPhaseTimer.getElapsedTimeF32() < CHUNK_COPY_RECHECK)
            {
                return;
            }
            break;
        case PHASE_PLAN:
            break;
        }
        plan_and_send(job);
    }
}

// static
void LLChunkedPurge::start(const LLUUID& root_id, EMode mode, LLPointer<LLInventoryCallback> done_cb)
{
    State& s = state();
    const LLViewerInventoryCategory* root = gInventory.getCategory(root_id);
    if (!root)
    {
        LL_WARNS("ChunkedPurge") << "Cannot delete " << root_id << " in parts: not in the model" << LL_ENDL;
        if (done_cb.notNull())
        {
            done_cb->fire(LLUUID::null);
        }
        return;
    }
    for (Job& job : s.mJobs)
    {
        if (job.mRoot == root_id)
        {
            LL_INFOS("ChunkedPurge") << "'" << job.mRootName << "' is already being deleted in parts" << LL_ENDL;
            job.mDoneCBs.push_back(done_cb);
            return;
        }
    }

    Job job;
    job.mRoot = root_id;
    job.mMode = mode;
    job.mRootParent = root->getParentUUID();
    job.mRootName = root->getName();
    job.mDoneCBs.push_back(done_cb);
    LL_INFOS("ChunkedPurge") << "The server refused to " << (mode == PURGE_CHILDREN ? "empty" : "delete")
                             << " '" << job.mRootName << "' (" << root_id << ") in one request; "
                             << (s.mJobs.empty() ? "deleting it in parts" : "queued behind the running job")
                             << LL_ENDL;
    s.mJobs.push_back(job);
    register_idle();
}

// static
bool LLChunkedPurge::overlapsActiveJob(const LLUUID& id)
{
    const State& s = state();
    for (const Job& job : s.mJobs)
    {
        if (id == job.mRoot
            || gInventory.isObjectDescendentOf(id, job.mRoot)
            || gInventory.isObjectDescendentOf(job.mRoot, id))
        {
            return true;
        }
    }
    return false;
}

// static
void LLChunkedPurge::shutdown()
{
    State& s = state();
    for (const Job& job : s.mJobs)
    {
        S32 folders = 0, items = 0;
        count_remaining(job.mRoot, folders, items);
        LL_WARNS("ChunkedPurge") << "Disconnecting: '" << job.mRootName << "' (" << job.mRoot
                                 << ") was not finished; still there: " << folders << " folders, "
                                 << items << " items" << LL_ENDL;
    }
    s.mJobs.clear();    // releases callbacks without firing them
    reset_job_state();
    unregister_idle();
}
