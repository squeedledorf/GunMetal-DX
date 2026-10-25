/**
 * @file llreporttracker.cpp
 * @brief Locally stored list of filed bug reports; follow-up transport and status poll (#486 / #527).
 */

#include "llviewerprecompiledheaders.h"

#include "llreporttracker.h"

#include "llapp.h"
#include "llcorehttputil.h"
#include "llcoros.h"
#include "lleventcoro.h"
#include "llnotificationsutil.h"
#include "llsdjson.h"
#include "llsdserialize.h"
#include "llstartup.h"
#include "llversioninfo.h"
#include "llviewercontrol.h"
#include "llweb.h"

#include <boost/json.hpp>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <algorithm>
#include <ctime>
#include <fstream>

namespace
{
    constexpr size_t MAX_REPORTS = 50;           // oldest are dropped
    constexpr size_t POLL_BATCH  = 20;           // relay limit per /status request
    constexpr F32    FIRST_POLL_DELAY = 90.f;    // seconds after startup; never during login
    constexpr F32    MIN_POLL_MINUTES = 30.f;

    std::string hmac_hex(const std::string& key, const std::string& msg)
    {
        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned int len = 0;
        if (!HMAC(EVP_sha256(), key.data(), (int)key.size(),
                  reinterpret_cast<const unsigned char*>(msg.data()), msg.size(), digest, &len))
        {
            return std::string();
        }
        std::string hex;
        for (unsigned int i = 0; i < len; ++i) hex += llformat("%02x", digest[i]);
        return hex;
    }

    std::string file_path()
    {
        return gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, "filed_reports.xml");
    }

    // Receipts are 32 lower-case hex chars; refuse anything else read from disk.
    bool plausible_receipt(const std::string& r)
    {
        return r.size() == 32 && std::all_of(r.begin(), r.end(), [](char c)
            { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
    }

    std::string clip_text(const std::string& s, size_t n)
    {
        if (s.size() <= n) return s;
        size_t cut = n;
        while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;   // UTF-8 safe
        return s.substr(0, cut) + "...";
    }
}

LLReportTracker::LLReportTracker()
{
}

// static
std::string LLReportTracker::relayEndpoint(const std::string& endpoint)
{
    std::string url = gSavedSettings.getString("ReportBugRelayURL");
    const size_t slash = url.rfind('/');
    if (slash == std::string::npos) return std::string();
    return url.substr(0, slash + 1) + endpoint;
}

// static
LLReportTracker::RelayReply LLReportTracker::postSigned(const std::string& url, const std::string& body)
{
    LLCore::HttpRequest::policy_t policy(LLCore::HttpRequest::DEFAULT_POLICY_ID);
    LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter(
        new LLCoreHttpUtil::HttpCoroutineAdapter("ReportTracker", policy));
    LLCore::HttpRequest::ptr_t request(new LLCore::HttpRequest);
    LLCore::HttpOptions::ptr_t options(new LLCore::HttpOptions);
    options->setTimeout(90);
    options->setRetries(0);   // a retry could post a follow-up twice
    LLCore::HttpHeaders::ptr_t headers(new LLCore::HttpHeaders);
    const std::string timestamp = llformat("%lld", (long long)time(nullptr));
    headers->append(HTTP_OUT_HEADER_CONTENT_TYPE, HTTP_CONTENT_JSON);
    headers->append("X-Slipstream-Version", LLVersionInfo::instance().getChannelAndVersion());
    headers->append("X-Slipstream-Timestamp", timestamp);
    headers->append("X-Slipstream-Signature", hmac_hex(SLIPSTREAM_REPORT_HMAC_KEY, timestamp + "\n" + body));
    LLCore::BufferArray::ptr_t raw(new LLCore::BufferArray);
    raw->append(body.data(), body.size());

    LLSD result = adapter->postRawAndSuspend(request, url, raw, options, headers);

    const LLSD& http = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS];
    const LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(http);
    RelayReply reply;
    // LLCore keeps the HTTP code in the status *type*; HTTP_RESULTS_STATUS is the transport
    // error code (0 on success). Reading the latter made every 201 look like "HTTP 0".
    reply.mStatus = status.isHttpStatus() ? (S32)status.getType() : 0;
    if (status)
    {
        const LLSD::Binary& bin = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary();
        reply.mText.assign(bin.begin(), bin.end());
    }
    else
    {
        reply.mText = http["error_body"].asString();
        reply.mError = status.toString();
    }
    return reply;
}

void LLReportTracker::load()
{
    if (mLoaded) return;
    mLoaded = true;
    mReports.clear();
    llifstream file(file_path().c_str());
    if (!file.is_open()) return;
    LLSD data;
    if (LLSDSerialize::fromXML(data, file) == LLSDParser::PARSE_FAILURE || !data.has("reports"))
    {
        LL_WARNS("ReportTracker") << "Ignoring unreadable " << file_path() << LL_ENDL;
        return;
    }
    for (const LLSD& e : llsd::inArray(data["reports"]))
    {
        LLFiledReport r;
        r.mNumber = e["number"].asInteger();
        r.mReceipt = e["receipt"].asString();
        if (r.mNumber <= 0 || !plausible_receipt(r.mReceipt)) continue;
        r.mUrl = e["url"].asString();
        r.mSummary = e["summary"].asString();
        r.mFiledAt = e["filed_at"].asInteger();
        r.mState = e["state"].asString().empty() ? "open" : e["state"].asString();
        r.mLabels = e["labels"].asString();
        r.mLastReplyAt = e["last_reply_at"].asString();
        r.mReplyCount = e["reply_count"].asInteger();
        r.mLastReplyId = e["last_reply_id"].asInteger();
        r.mCheckedAt = e["checked_at"].asInteger();
        r.mUnseen = e["unseen"].asBoolean();
        mReports.push_back(r);
    }
}

void LLReportTracker::save() const
{
    LLSD list = LLSD::emptyArray();
    for (const LLFiledReport& r : mReports)
    {
        LLSD e;
        e["number"] = r.mNumber;
        e["receipt"] = r.mReceipt;
        e["url"] = r.mUrl;
        e["summary"] = r.mSummary;
        e["filed_at"] = LLSD::Integer(r.mFiledAt);
        e["state"] = r.mState;
        e["labels"] = r.mLabels;
        e["last_reply_at"] = r.mLastReplyAt;
        e["reply_count"] = r.mReplyCount;
        e["last_reply_id"] = LLSD::Integer(r.mLastReplyId);
        e["checked_at"] = LLSD::Integer(r.mCheckedAt);
        e["unseen"] = r.mUnseen;
        list.append(e);
    }
    LLSD data;
    data["version"] = 1;
    data["reports"] = list;
    // Write to a temp file and rename so a crash never leaves a truncated list.
    const std::string path = file_path();
    const std::string tmp = path + ".tmp";
    {
        llofstream file(tmp.c_str());
        if (!file.is_open()) return;
        LLSDSerialize::toPrettyXML(data, file);
    }
    LLFile::rename(tmp, path);
}

void LLReportTracker::addReport(S32 number, const std::string& receipt, const std::string& url,
                                const std::string& summary)
{
    if (number <= 0 || !plausible_receipt(receipt)) return;   // relay without receipts: nothing to track
    load();
    mReports.erase(std::remove_if(mReports.begin(), mReports.end(),
                       [number](const LLFiledReport& r) { return r.mNumber == number; }), mReports.end());
    LLFiledReport r;
    r.mNumber = number;
    r.mReceipt = receipt;
    r.mUrl = url;
    r.mSummary = clip_text(summary, 120);
    r.mFiledAt = (S64)time(nullptr);
    mReports.push_back(r);
    while (mReports.size() > MAX_REPORTS) mReports.erase(mReports.begin());
    save();
    mChanged();
    pollSoon();
}

std::vector<LLFiledReport> LLReportTracker::reports() const
{
    const_cast<LLReportTracker*>(this)->load();
    std::vector<LLFiledReport> out(mReports.rbegin(), mReports.rend());
    return out;
}

bool LLReportTracker::find(S32 number, LLFiledReport& out) const
{
    const_cast<LLReportTracker*>(this)->load();
    for (const LLFiledReport& r : mReports)
    {
        if (r.mNumber == number) { out = r; return true; }
    }
    return false;
}

void LLReportTracker::markSeen(S32 number)
{
    load();
    for (LLFiledReport& r : mReports)
    {
        if (r.mNumber == number && r.mUnseen)
        {
            r.mUnseen = false;
            save();
            mChanged();
            return;
        }
    }
}

void LLReportTracker::forget(S32 number)
{
    load();
    const size_t before = mReports.size();
    mReports.erase(std::remove_if(mReports.begin(), mReports.end(),
                       [number](const LLFiledReport& r) { return r.mNumber == number; }), mReports.end());
    if (mReports.size() != before)
    {
        save();
        mChanged();
    }
}

// static
std::string LLReportTracker::statusText(const LLFiledReport& r)
{
    std::string s = (r.mState == "closed") ? "closed" : "open";
    if (!r.mLabels.empty()) s += ", " + r.mLabels;
    if (r.mReplyCount > 0) s += llformat(", %d repl%s", r.mReplyCount, r.mReplyCount == 1 ? "y" : "ies");
    if (r.mUnseen) s += " (new)";
    return s;
}

std::string LLReportTracker::applyStatus(const LLSD& entry)
{
    load();
    const S32 number = entry["issue_number"].asInteger();
    for (LLFiledReport& r : mReports)
    {
        if (r.mNumber != number) continue;
        if (entry.has("error"))
        {
            // "unknown": the relay does not know this receipt (e.g. its key was rotated).
            // Keep the entry (it is still a record for the user) but stop treating it as live.
            return std::string();
        }
        // Everything below is copied from the relay's public answer; clip it.
        const std::string state = entry["state"].asString() == "closed" ? "closed" : "open";
        std::string labels;
        for (const LLSD& l : llsd::inArray(entry["labels"]))
        {
            if (!labels.empty()) labels += ",";
            labels += clip_text(l.asString(), 32);
        }
        const std::string last_reply = clip_text(entry["last_reply_at"].asString(), 40);
        const S32 replies = entry["reply_count"].asInteger();
        const std::string url = entry["url"].asString();
        const S64 reply_id = (S64)entry["last_reply_id"].asInteger();
        // "New follow-up" = a human comment NEWER than the newest one we already know. The count
        // is display-only: a deleted comment followed by a new one must not read as new, and
        // closing, label edits, board moves and updated_at bumps are never replies (#853).
        const bool reply = (!last_reply.empty() && last_reply > r.mLastReplyAt)
                        || (r.mLastReplyId > 0 && reply_id > r.mLastReplyId);
        const bool was_closed = r.mState == "closed";
        const bool reopened = was_closed && state == "open";
        std::string kind;
        if (reopened)   kind = reply ? "both" : "status";
        else if (reply) kind = "reply";

        // Data always updates below; these rules only decide whether to pop up. Closed reports
        // never pop up (the list still shows "(new)"), a label/state-to-closed change is not
        // news, and only one popup per report until the user has seen the previous one.
        bool notify = !kind.empty();
        if (notify && state == "closed")
        {
            LL_INFOS("ReportTracker") << "Suppressed notification for #" << number << " (" << kind
                                      << "): report is closed" << LL_ENDL;
            notify = false;
        }
        else if (notify && !reopened && r.mUnseen)
        {
            LL_INFOS("ReportTracker") << "Suppressed notification for #" << number << " (" << kind
                                      << "): earlier notification not seen yet" << LL_ENDL;
            notify = false;
        }
        r.mState = state;
        r.mLabels = labels;
        r.mReplyCount = replies;
        // High-water marks: never move backwards (a deleted comment must not re-arm "new").
        if (last_reply > r.mLastReplyAt) r.mLastReplyAt = last_reply;
        if (reply_id > r.mLastReplyId) r.mLastReplyId = reply_id;
        r.mCheckedAt = (S64)time(nullptr);
        // Only trust a https URL on our own tracker host for the link; otherwise keep the stored one.
        if (url.rfind("https://", 0) == 0 && url.find('/', 8) != std::string::npos) r.mUrl = url;
        if (!kind.empty()) r.mUnseen = true;   // persisted below: a restart will not re-notify this data (#853)
        save();
        mChanged();
        return notify ? kind : std::string();
    }
    return std::string();
}

void LLReportTracker::start()
{
    if (mRunning) return;
    mRunning = true;
    LLCoros::instance().launch("LLReportTracker::pollCoro", [this]() { pollCoro(); });
}

void LLReportTracker::pollSoon()
{
    mPollSoon = true;
}

void LLReportTracker::pollCoro()
{
    // Sleep in short steps so pollSoon() and shutdown are noticed.
    auto nap = [this](F32 seconds)
    {
        for (F32 t = 0.f; t < seconds && !mPollSoon && !LLApp::isExiting(); t += 5.f)
        {
            llcoro::suspendUntilTimeout(5.f);
        }
    };
    // Never poll from the login screen: popups raised there queue up and burst at login.
    auto wait_logged_in = [this]()
    {
        while (!LLApp::isExiting() && LLStartUp::getStartupState() != STATE_STARTED)
        {
            llcoro::suspendUntilTimeout(5.f);
        }
    };
    wait_logged_in();
    nap(FIRST_POLL_DELAY);   // measured from login
    while (!LLApp::isExiting())
    {
        mPollSoon = false;
        wait_logged_in();
        if (gSavedSettings.getBOOL("ReportTrackEnabled") && !reports().empty())
        {
            pollOnce();
        }
        const F32 minutes = std::max(MIN_POLL_MINUTES, (F32)gSavedSettings.getU32("ReportTrackPollMinutes"));
        nap(minutes * 60.f);
        if (mPollSoon)
        {
            mPollSoon = false;
            nap(20.f);   // let a just-filed report settle on the tracker first
        }
    }
}

bool LLReportTracker::pollOnce()
{
    const std::string url = relayEndpoint("status");
    if (url.empty()) return false;
    std::vector<LLFiledReport> live = reports();
    // Closed reports are still polled (they can be reopened) but oldest-first rotation keeps
    // the request small: newest POLL_BATCH reports only.
    if (live.size() > POLL_BATCH) live.resize(POLL_BATCH);

    LLSD req;
    req["schema"] = 1;
    req["reports"] = LLSD::emptyArray();
    for (const LLFiledReport& r : live)
    {
        LLSD e;
        e["issue_number"] = r.mNumber;
        e["receipt"] = r.mReceipt;
        req["reports"].append(e);
    }
    const std::string body = boost::json::serialize(LlsdToJson(req));
    const RelayReply reply = postSigned(url, body);
    if (reply.mStatus != 200)
    {
        LL_INFOS("ReportTracker") << "Status poll failed: HTTP " << reply.mStatus << " " << reply.mError << LL_ENDL;
        return false;
    }
    boost::system::error_code ec;
    const boost::json::value doc = boost::json::parse(reply.mText, ec);
    if (ec || !doc.is_object()) return false;
    const LLSD answer = LlsdFromJson(doc);
    // Apply every answer first, then pop up: several reports changing in one poll give ONE popup.
    std::vector<std::pair<S32, std::string>> news;
    for (const LLSD& entry : llsd::inArray(answer["reports"]))
    {
        const std::string kind = applyStatus(entry);
        if (!kind.empty()) news.emplace_back(entry["issue_number"].asInteger(), kind);
    }
    if (news.size() > 1)
    {
        LLSD args;
        args["COUNT"] = (S32)news.size();
        LLSD payload;
        payload["numbers"] = LLSD::emptyArray();
        for (const auto& n : news) payload["numbers"].append(n.first);
        LLNotificationsUtil::add("ReportBugUpdatedMany", args, payload,
            [](const LLSD& notification, const LLSD& response)
            {
                for (const LLSD& n : llsd::inArray(notification["payload"]["numbers"]))
                {
                    LLReportTracker::instance().markSeen(n.asInteger());
                }
                return false;
            });
    }
    else if (news.size() == 1)
    {
        const std::string& kind = news[0].second;
        LLFiledReport r;
        if (find(news[0].first, r))
        {
            LLSD args;
            args["NUMBER"] = r.mNumber;
            args["SUMMARY"] = clip_text(r.mSummary, 60);
            if (kind == "reply") args["WHAT"] = "a new reply";
            else if (kind == "both") args["WHAT"] = "been reopened and has a new reply";
            else args["WHAT"] = "been reopened";
            args["URL"] = r.mUrl;
            LLSD payload;
            payload["url"] = r.mUrl;
            payload["number"] = r.mNumber;
            LLNotificationsUtil::add("ReportBugUpdated", args, payload,
                [](const LLSD& notification, const LLSD& response)
                {
                    const S32 option = LLNotificationsUtil::getSelectedOption(notification, response);
                    const std::string link = notification["payload"]["url"].asString();
                    if (option == 0 && link.rfind("https://", 0) == 0)
                    {
                        LLWeb::loadURLExternal(link);
                    }
                    LLReportTracker::instance().markSeen(notification["payload"]["number"].asInteger());
                    return false;
                });
        }
    }
    return true;
}
