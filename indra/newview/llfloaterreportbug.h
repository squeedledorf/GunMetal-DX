/**
 * @file llfloaterreportbug.h
 * @brief Help > Report Bug: account-free bug reports through the Slipstream report relay.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Bluefast Studios
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

#ifndef LL_LLFLOATERREPORTBUG_H
#define LL_LLFLOATERREPORTBUG_H

#include "llfloater.h"
#include "llimage.h"
#include "llpointer.h"

#include <boost/signals2.hpp>

class LLButton;
class LLCheckBoxCtrl;
class LLLineEditor;
class LLScrollListCtrl;
class LLTabContainer;
class LLTextBox;
class LLTextEditor;
class LLView;
class LLViewerTexture;

// The report window. Collects a summary/description/steps, shows the system details,
// a scrubbed log excerpt and a screenshot (from the viewer's own snapshot code only),
// shows exactly what will be sent, and POSTs it to the relay, which files a public
// issue on the tracker. Nothing is sent until the user presses Send.
class LLFloaterReportBug final : public LLFloater
{
    friend class LLFloaterReg;
public:
    // Opens the window for a report about the previous session's crash.
    static void showForCrash();
    // Called once at the login screen: if the last session crashed, ask whether to report it.
    static void offerCrashReportIfNeeded();
    // The log excerpt's personal-information scrubber, line by line (names and keys of self,
    // friends and nearby avatars, addresses, credentials, machine ids, the OS user name).
    // #966: performance captures go through it before they are saved or attached.
    static std::string scrubText(const std::string& text);

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void draw() override;

private:
    LLFloaterReportBug(const LLSD& key);
    ~LLFloaterReportBug() override = default;

    void setCrashMode();
    void refreshSystemInfo();
    void refreshLog();
    void refreshPayloadView();
    void takeScreenshot();
    void updateControls();
    bool validate(std::string& error) const;
    LLSD buildPayload() const;           // everything sent, minus the screenshot bytes
    std::string describePayload(const LLSD& payload) const;

    void onSend();
    void onTabChanged();
    static void sendCoro(LLHandle<LLFloater> handle, std::string url, std::string body,
                         std::string version, std::string summary);
    void onSendResult(bool ok, S32 status, const std::string& issue_url, S32 issue_number,
                      const std::string& error);

    // MY REPORTS tab (#486 / #527): follow up on a report filed earlier from this viewer.
    void refreshMyReports();
    S32  selectedReport() const;
    void updateFollowupControls();
    void onFollowupSend();
    void onFollowupOpen();
    void onFollowupForget();
    static void followupCoro(LLHandle<LLFloater> handle, std::string url, std::string body, S32 number);
    void onFollowupResult(bool ok, S32 status, const std::string& error);

    // #966 performance reports: attach the latest performance capture (LLVKPerfCapture) to
    // the log field, or take one from here.
    bool attachPerf() const;
    void updatePerfControls();
    void onPerfCapture();
    void onPerfDone();

    LLTabContainer* mTabs = nullptr;
    LLLineEditor*   mSummary = nullptr;
    LLTextEditor*   mDescription = nullptr;
    LLTextEditor*   mSteps = nullptr;
    LLLineEditor*   mAvatar = nullptr;
    LLTextEditor*   mSystemInfo = nullptr;
    LLCheckBoxCtrl* mAttachLog = nullptr;
    LLCheckBoxCtrl* mAttachShot = nullptr;
    LLCheckBoxCtrl* mShotUI = nullptr;
    LLButton*       mRetakeBtn = nullptr;
    LLView*         mPreviewArea = nullptr;
    LLTextBox*      mShotNote = nullptr;
    LLTextEditor*   mPayloadView = nullptr;
    LLTextBox*      mStatus = nullptr;
    LLButton*       mSendBtn = nullptr;
    LLButton*       mCloseBtn = nullptr;
    LLScrollListCtrl* mMyReports = nullptr;
    LLTextEditor*   mFollowText = nullptr;
    LLButton*       mFollowSendBtn = nullptr;
    LLButton*       mFollowOpenBtn = nullptr;
    LLButton*       mFollowForgetBtn = nullptr;
    boost::signals2::scoped_connection mTrackerConn;
    bool            mFollowSending = false;
    LLCheckBoxCtrl* mAttachPerf = nullptr;
    LLButton*       mPerfCaptureBtn = nullptr;
    LLTextBox*      mPerfNote = nullptr;
    std::string     mPerfNoteText;               // last text set (draw updates it while capturing)
    bool            mPerfRequested = false;      // a capture started from this window is running
    boost::signals2::scoped_connection mPerfConn;

    LLSD                         mSystem;        // "system" object (incl. "settings")
    std::string                  mLogExcerpt;    // already filtered + scrubbed
    S32                          mLogLines = 0;
    std::string                  mCrashEvent;    // non-empty: report about the last session's crash
    LLPointer<LLImageRaw>        mShotRaw;       // captured screenshot
    std::vector<U8>              mShotJpeg;      // what is sent
    LLPointer<LLViewerTexture>   mShotTexture;   // preview
    bool                         mSending = false;
    bool                         mSent = false;
};

#endif // LL_LLFLOATERREPORTBUG_H
