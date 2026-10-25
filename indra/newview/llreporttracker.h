/**
 * @file llreporttracker.h
 * @brief Locally stored list of filed bug reports (#486 / #527).
 *
 * The report relay returns a receipt with every filed report. The viewer keeps
 * {issue number, receipt, url, summary} in <user settings>/filed_reports.xml and uses the
 * receipt to (a) add a follow-up comment to that issue and (b) poll the relay at a low rate
 * for its public status. The viewer never talks to the tracker directly and holds no
 * tracker token. A notification (with a link) is shown when a reply arrives or the status
 * changes. Nothing is polled unless a receipt is stored.
 */

#ifndef LL_LLREPORTTRACKER_H
#define LL_LLREPORTTRACKER_H

#include "llsingleton.h"
#include "llsd.h"

#include <boost/signals2.hpp>
#include <string>
#include <vector>

// Viewer-origin check for the report relay: an HMAC-SHA256 over "timestamp\nbody".
// This is a DETERRENT, NOT SECURITY: the source is public, so anyone can read the key.
// It only keeps casual scripts off the relay's screenshot endpoint; the relay's real
// controls are rate limits, size caps, validation, receipts and re-encoding every image.
// Builds can override it with -DSLIPSTREAM_REPORT_HMAC_KEY=\"...\" (the relay's
// RELAY_HMAC_KEY must match).
#ifndef SLIPSTREAM_REPORT_HMAC_KEY
#define SLIPSTREAM_REPORT_HMAC_KEY "4fea8ee839c3c1c4faa285156855c680dc606878044442ed"
#endif

struct LLFiledReport
{
    S32         mNumber = 0;
    std::string mReceipt;
    std::string mUrl;
    std::string mSummary;
    S64         mFiledAt = 0;          // unix seconds
    std::string mState = "open";       // last state the relay reported: open / closed
    std::string mLabels;               // allow-listed status labels, comma separated
    std::string mLastReplyAt;          // ISO time of the newest reply we know of
    S32         mReplyCount = 0;       // display only
    S64         mLastReplyId = 0;      // newest human comment id we know of (high-water mark)
    S64         mCheckedAt = 0;        // unix seconds of the last successful poll
    bool        mUnseen = false;       // a reply / status change the user has not looked at
};

class LLReportTracker final : public LLSingleton<LLReportTracker>
{
    LLSINGLETON(LLReportTracker);
public:
    struct RelayReply
    {
        S32         mStatus = 0;       // HTTP status, 0 on a transport error
        std::string mText;             // response body (or error body)
        std::string mError;            // transport error text when mStatus == 0
    };

    // Signed POST to a relay endpoint. Must be called from a coroutine.
    static RelayReply postSigned(const std::string& url, const std::string& body);
    // Relay endpoint derived from ReportBugRelayURL (".../viewer" -> ".../<endpoint>").
    static std::string relayEndpoint(const std::string& endpoint);

    // Remember a freshly filed report. Ignored when the relay sent no receipt.
    void addReport(S32 number, const std::string& receipt, const std::string& url,
                   const std::string& summary);
    std::vector<LLFiledReport> reports() const;     // newest first
    bool find(S32 number, LLFiledReport& out) const;
    void markSeen(S32 number);
    void forget(S32 number);

    // Start the low-rate background poll (idempotent). Called once per run from startup.
    void start();
    // Poll as soon as possible (e.g. right after filing).
    void pollSoon();

    static std::string statusText(const LLFiledReport& report);

    typedef boost::signals2::signal<void()> changed_signal_t;
    boost::signals2::connection setChangedCallback(const changed_signal_t::slot_type& cb)
    {
        return mChanged.connect(cb);
    }

    // Applies one /status answer entry to the stored report; returns the notification kind
    // ("reply", "status", "both") or an empty string (nothing changed, or the change is
    // applied silently: closed report, or an earlier notification is still unseen).
    // Only a human comment newer than the stored high-water mark, or a closed->open reopen,
    // can notify; closing, label changes and updated_at bumps never do. Public for tests.
    std::string applyStatus(const LLSD& entry);

private:
    void load();
    void save() const;
    void pollCoro();
    bool pollOnce();

    std::vector<LLFiledReport> mReports;
    changed_signal_t           mChanged;
    bool                       mLoaded = false;
    bool                       mRunning = false;
    bool                       mPollSoon = false;
};

#endif // LL_LLREPORTTRACKER_H
