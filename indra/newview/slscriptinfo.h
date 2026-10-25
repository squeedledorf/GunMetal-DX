/**
 * @file slscriptinfo.h
 * @brief Script info for objects in the region through the "#Slipstream Helper" bridge
 *        (#425): running / total script counts, script memory and script time.
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

#ifndef SL_SCRIPTINFO_H
#define SL_SCRIPTINFO_H

#include <deque>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "llsingleton.h"
#include "lltimer.h"
#include "lluuid.h"

class LLEventTimer;

/**
 * Wire protocol (private bridge channel, see slipstream_helper.lsl):
 *   viewer -> helper : scripts|<batch>|<key>,<key>,...        (at most MAX_KEYS_PER_BATCH keys)
 *   helper -> viewer : #slb:scripts|<batch>|<key>,<running>,<total>,<memory bytes>,<time us>|<key>,-|...
 *                      #slb:scripts|<batch>|busy               (the helper's rate limit: retried)
 * "<key>,-" means the object is not in the helper's region. Only keys of a batch we sent and
 * have not had an answer for are accepted; everything else is ignored.
 *
 * The bridge is held ("scriptinfo" feature) while requests are outstanding and for a short
 * while after, so follow-up clicks do not wear and take off the helper each time. Results
 * are reported as system lines in nearby chat, one per object plus a total for several.
 */
class SLScriptInfo : public LLSingleton<SLScriptInfo>
{
    LLSINGLETON(SLScriptInfo);
    ~SLScriptInfo() override;
    LOG_CLASS(SLScriptInfo);

public:
    static constexpr size_t MAX_KEYS_PER_BATCH = 25;    // 25 * 37 bytes stays under the command cap
    static constexpr size_t MAX_PENDING_OBJECTS = 1000;

    // Whether the action can be offered now. When not, `reason` says why (for a tooltip).
    // A bridge that is unavailable while nothing holds it is not a reason: asking again
    // makes it retry.
    bool canRequest(std::string* reason = nullptr) const;

    // Objects (id, display name) to report on, in the order given.
    void request(const std::vector<std::pair<LLUUID, std::string>>& objects);

private:
    struct Result
    {
        bool mDone = false;
        bool mFound = false;
        S32  mRunning = 0;
        S32  mTotal = 0;
        S64  mMemory = 0;       // bytes
        S64  mTimeUs = 0;       // microseconds
    };

    struct Request
    {
        U32 mId = 0;
        std::vector<LLUUID> mOrder;
        std::map<LLUUID, std::string> mNames;
        std::map<LLUUID, Result> mResults;
        size_t mOutstanding = 0;
    };

    struct Batch
    {
        U32 mId = 0;
        U32 mRequestId = 0;
        std::vector<LLUUID> mKeys;      // unanswered keys
        bool mSent = false;
        S32 mRetries = 0;
        F64 mNotBefore = 0.0;           // retry backoff after "busy"
        LLTimer mTimer;                 // since sent
    };

    void ensureTicker();
    void tick();
    void onReply(const std::string& rest);
    void finishKey(Request& request, const LLUUID& id, const Result* result);
    void finishRequest(Request& request);
    void failAll(const std::string& reason);
    Request* findRequest(U32 id);

    std::deque<Batch> mBatches;
    std::map<U32, Request> mRequests;
    U32 mNextBatchId = 1;
    U32 mNextRequestId = 1;
    bool mHoldingBridge = false;
    bool mWaitingNoticeShown = false;
    LLTimer mIdleTimer;                 // since the last request finished
    F32 mHoldFor = 120.f;               // seconds to keep holding the bridge after that
    LLTimer mWaitTimer;                 // since the bridge was first needed and not ready
    LLEventTimer* mTicker = nullptr;
};

#endif // SL_SCRIPTINFO_H
