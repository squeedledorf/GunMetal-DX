/**
 * @file slscriptinfo.cpp
 * @brief Script info for objects in the region through the "#Slipstream Helper" bridge (#425).
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

#include "slscriptinfo.h"

#include "fscommon.h"
#include "llagent.h"
#include "lleventtimer.h"
#include "lltrans.h"
#include "slbridge.h"

#include <algorithm>
#include <functional>

namespace
{
constexpr char BRIDGE_USER[]        = "scriptinfo";
constexpr F32  TICK_PERIOD          = 0.25f;
constexpr size_t MAX_IN_FLIGHT      = 2;       // batches sent and not fully answered
constexpr F32  ANSWER_TIMEOUT       = 30.f;    // per batch, from when it was queued
constexpr S32  MAX_RETRIES          = 2;       // re-asks after a timeout or "busy"
constexpr F64  BUSY_BACKOFF         = 5.0;     // seconds; the helper's rate window is 10 s
constexpr F32  BRIDGE_WAIT_TIMEOUT  = 90.f;    // finding, wearing and installing the helper
constexpr F32  HOLD_AFTER_IDLE      = 120.f;   // keep the helper on for follow-up requests
constexpr F32  HOLD_AFTER_FAILURE   = 60.f;    // keep it (unavailable, item disabled) after a failure

class SLScriptInfoTicker : public LLEventTimer
{
public:
    SLScriptInfoTicker(F32 period, std::function<void()> cb) : LLEventTimer(period), mCallback(std::move(cb)) {}
    bool tick() override
    {
        if (mCallback)
            mCallback();
        return false;
    }

private:
    std::function<void()> mCallback;
};

bool parse_count(const std::string& text, S32& value)
{
    return LLStringUtil::convertToS32(text, value) && value >= 0;
}
}

SLScriptInfo::SLScriptInfo()
{
    SLBridge::instance().registerHandler("scripts", [this](const std::string& rest) { onReply(rest); });
}

SLScriptInfo::~SLScriptInfo()
{
    delete mTicker;
    mTicker = nullptr;
}

void SLScriptInfo::ensureTicker()
{
    if (!mTicker)
        mTicker = new SLScriptInfoTicker(TICK_PERIOD, [this]() { tick(); });
}

bool SLScriptInfo::canRequest(std::string* reason) const
{
    if (!gAgent.getRegion())
    {
        if (reason)
            *reason = LLTrans::getString("SLScriptInfoNoRegion");
        return false;
    }
    const SLBridge& bridge = SLBridge::instance();
    if (bridge.getState() == SLBridge::STATE_UNAVAILABLE && bridge.isWanted())
    {
        if (reason)
        {
            LLStringUtil::format_map_t args;
            args["[REASON]"] = bridge.getReason();
            *reason = LLTrans::getString("SLScriptInfoUnavailableTip", args);
        }
        return false;
    }
    if (reason)
        *reason = LLTrans::getString("SLScriptInfoTip");
    return true;
}

void SLScriptInfo::request(const std::vector<std::pair<LLUUID, std::string>>& objects)
{
    if (objects.empty())
        return;
    std::string reason;
    if (!canRequest(&reason))
    {
        FSCommon::report_to_nearby_chat(reason);
        return;
    }

    size_t pending = 0;
    for (const auto& [pending_id, pending_req] : mRequests)
        pending += pending_req.mOutstanding;

    Request req;
    req.mId = mNextRequestId++;
    for (const auto& [id, name] : objects)
    {
        if (id.isNull() || req.mNames.count(id))
            continue;
        if (pending + req.mOrder.size() >= MAX_PENDING_OBJECTS)
        {
            LLStringUtil::format_map_t args;
            args["[COUNT]"] = llformat("%d", (S32)(objects.size() - req.mOrder.size()));
            FSCommon::report_to_nearby_chat(LLTrans::getString("SLScriptInfoTooMany", args));
            break;
        }
        req.mOrder.push_back(id);
        req.mNames[id] = name;
        req.mResults[id] = Result();
    }
    if (req.mOrder.empty())
        return;
    req.mOutstanding = req.mOrder.size();

    for (size_t i = 0; i < req.mOrder.size(); i += MAX_KEYS_PER_BATCH)
    {
        Batch batch;
        batch.mId = mNextBatchId++;
        batch.mRequestId = req.mId;
        const size_t end = llmin(req.mOrder.size(), i + MAX_KEYS_PER_BATCH);
        batch.mKeys.assign(req.mOrder.begin() + i, req.mOrder.begin() + end);
        mBatches.push_back(batch);
    }
    mRequests[req.mId] = req;

    SLBridge& bridge = SLBridge::instance();
    if (!mHoldingBridge)
    {
        mHoldingBridge = true;
        bridge.addUser(BRIDGE_USER);
    }
    if (!bridge.isReady())
    {
        if (!mWaitingNoticeShown)
        {
            mWaitingNoticeShown = true;
            mWaitTimer.reset();
            FSCommon::report_to_nearby_chat(LLTrans::getString("SLScriptInfoStarting"));
        }
    }
    ensureTicker();
    tick();
}

SLScriptInfo::Request* SLScriptInfo::findRequest(U32 id)
{
    auto it = mRequests.find(id);
    return (it != mRequests.end()) ? &it->second : nullptr;
}

void SLScriptInfo::finishKey(Request& request, const LLUUID& id, const Result* result)
{
    auto it = request.mResults.find(id);
    if (it == request.mResults.end() || it->second.mDone)
        return;
    Result& slot = it->second;
    slot.mDone = true;
    if (request.mOutstanding > 0)
        --request.mOutstanding;

    LLStringUtil::format_map_t args;
    args["[NAME]"] = request.mNames[id];
    if (!result)
    {
        FSCommon::report_to_nearby_chat(LLTrans::getString("SLScriptInfoNoAnswer", args));
        return;
    }
    slot = *result;
    slot.mDone = true;
    if (!result->mFound)
    {
        FSCommon::report_to_nearby_chat(LLTrans::getString("SLScriptInfoNotFound", args));
        return;
    }
    args["[RUNNING]"] = llformat("%d", result->mRunning);
    args["[TOTAL]"] = llformat("%d", result->mTotal);
    args["[MEMORY]"] = llformat("%lld", (long long)(result->mMemory / 1024));
    args["[TIME]"] = llformat("%.3f", (F64)result->mTimeUs / 1000.0);
    FSCommon::report_to_nearby_chat(LLTrans::getString("SLScriptInfoObject", args));
}

void SLScriptInfo::finishRequest(Request& request)
{
    S32 found = 0;
    S32 running = 0;
    S32 total = 0;
    S64 memory = 0;
    S64 time_us = 0;
    for (const auto& [id, result] : request.mResults)
    {
        if (!result.mFound)
            continue;
        ++found;
        running += result.mRunning;
        total += result.mTotal;
        memory += result.mMemory;
        time_us += result.mTimeUs;
    }
    if (found > 1)
    {
        LLStringUtil::format_map_t args;
        args["[COUNT]"] = llformat("%d", found);
        args["[RUNNING]"] = llformat("%d", running);
        args["[TOTAL]"] = llformat("%d", total);
        args["[MEMORY]"] = llformat("%lld", (long long)(memory / 1024));
        args["[TIME]"] = llformat("%.3f", (F64)time_us / 1000.0);
        FSCommon::report_to_nearby_chat(LLTrans::getString("SLScriptInfoTotal", args));
    }
    mIdleTimer.reset();
    mHoldFor = HOLD_AFTER_IDLE;
}

void SLScriptInfo::failAll(const std::string& reason)
{
    if (!mRequests.empty())
    {
        LLStringUtil::format_map_t args;
        args["[REASON]"] = reason;
        FSCommon::report_to_nearby_chat(LLTrans::getString("SLScriptInfoFailed", args));
    }
    mBatches.clear();
    mRequests.clear();
    mWaitingNoticeShown = false;
    // Keep holding the bridge for a while: an unavailable bridge nobody holds goes back to
    // idle at once, and the menu item would be offered again only to fail the same way.
    // Held, it stays unavailable (item disabled, reason in the tooltip) and retries on a
    // region change by itself.
    mIdleTimer.reset();
    mHoldFor = HOLD_AFTER_FAILURE;
}

void SLScriptInfo::tick()
{
    SLBridge& bridge = SLBridge::instance();

    if (mRequests.empty())
    {
        mBatches.clear();
        if (mHoldingBridge && mIdleTimer.getElapsedTimeF32() > mHoldFor)
        {
            mHoldingBridge = false;
            bridge.removeUser(BRIDGE_USER);
        }
        return;
    }

    if (bridge.getState() == SLBridge::STATE_UNAVAILABLE)
    {
        failAll(bridge.getReason());
        return;
    }
    if (!bridge.isReady())
    {
        if (!mWaitingNoticeShown)
        {
            mWaitingNoticeShown = true;
            mWaitTimer.reset();
        }
        else if (mWaitTimer.getElapsedTimeF32() > BRIDGE_WAIT_TIMEOUT)
        {
            failAll(LLTrans::getString("SLScriptInfoNoStart"));
        }
        return;
    }
    mWaitingNoticeShown = false;

    // Timeouts first: re-ask a couple of times, then report no answer.
    const F64 now = LLTimer::getTotalSeconds();
    for (auto it = mBatches.begin(); it != mBatches.end(); )
    {
        Batch& batch = *it;
        if (batch.mSent && batch.mTimer.getElapsedTimeF32() > ANSWER_TIMEOUT)
        {
            if (batch.mRetries < MAX_RETRIES)
            {
                ++batch.mRetries;
                batch.mSent = false;
            }
            else
            {
                if (Request* request = findRequest(batch.mRequestId))
                {
                    for (const LLUUID& id : batch.mKeys)
                        finishKey(*request, id, nullptr);
                    if (request->mOutstanding == 0)
                    {
                        finishRequest(*request);
                        mRequests.erase(batch.mRequestId);
                    }
                }
                it = mBatches.erase(it);
                continue;
            }
        }
        ++it;
    }

    size_t in_flight = 0;
    for (const Batch& batch : mBatches)
        in_flight += batch.mSent ? 1 : 0;
    for (Batch& batch : mBatches)
    {
        if (in_flight >= MAX_IN_FLIGHT)
            break;
        if (batch.mSent || batch.mKeys.empty() || now < batch.mNotBefore)
            continue;
        std::string command = llformat("scripts|%u|", batch.mId);
        for (size_t i = 0; i < batch.mKeys.size(); ++i)
        {
            if (i)
                command += ",";
            command += batch.mKeys[i].asString();
        }
        // Control lines wait for the bridge and are never dropped for newer data.
        bridge.sendControl(command);
        batch.mSent = true;
        batch.mTimer.reset();
        ++in_flight;
    }
}

void SLScriptInfo::onReply(const std::string& rest)
{
    // <batch>|<key>,<running>,<total>,<memory>,<time us>|<key>,-|...   or   <batch>|busy
    std::vector<std::string> fields;
    LLStringUtil::getTokens(rest, fields, "|");
    U32 batch_id = 0;
    if (fields.size() < 2 || !LLStringUtil::convertToU32(fields[0], batch_id))
        return;

    auto batch_it = std::find_if(mBatches.begin(), mBatches.end(),
                                 [batch_id](const Batch& batch) { return batch.mId == batch_id && batch.mSent; });
    if (batch_it == mBatches.end())
        return;     // not ours, already answered, or timed out
    Batch& batch = *batch_it;

    Request* request = findRequest(batch.mRequestId);
    if (!request)
    {
        mBatches.erase(batch_it);
        return;
    }

    if (fields.size() == 2 && fields[1] == "busy")
    {
        // The helper's rate limit: ask again after a pause, a few times at most.
        if (++batch.mRetries <= MAX_RETRIES + 2)
        {
            batch.mSent = false;
            batch.mNotBefore = LLTimer::getTotalSeconds() + BUSY_BACKOFF;
            return;
        }
        for (const LLUUID& id : batch.mKeys)
            finishKey(*request, id, nullptr);
        batch.mKeys.clear();
    }

    for (size_t i = 1; i < fields.size(); ++i)
    {
        std::vector<std::string> parts;
        LLStringUtil::getTokens(fields[i], parts, ",");
        LLUUID id;
        if (parts.empty() || parts[0].size() != (size_t)(UUID_STR_LENGTH - 1) || !LLUUID::validate(parts[0]))
            continue;
        id.set(parts[0]);
        auto key_it = std::find(batch.mKeys.begin(), batch.mKeys.end(), id);
        if (key_it == batch.mKeys.end())
            continue;   // a key we did not ask about in this batch

        Result result;
        if (parts.size() == 2 && parts[1] == "-")
        {
            result.mFound = false;
        }
        else if (parts.size() == 5)
        {
            S32 memory = 0, time_us = 0;
            if (!parse_count(parts[1], result.mRunning) || !parse_count(parts[2], result.mTotal) ||
                !parse_count(parts[3], memory) || !parse_count(parts[4], time_us) || result.mRunning > result.mTotal)
            {
                continue;
            }
            result.mFound = true;
            result.mMemory = memory;
            result.mTimeUs = time_us;
        }
        else
        {
            continue;
        }
        batch.mKeys.erase(key_it);
        finishKey(*request, id, &result);
    }

    const U32 request_id = batch.mRequestId;
    if (batch.mKeys.empty())
        mBatches.erase(batch_it);
    if (request->mOutstanding == 0)
    {
        finishRequest(*request);
        mRequests.erase(request_id);
    }
}
