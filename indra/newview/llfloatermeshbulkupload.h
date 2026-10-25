/**
 * @file llfloatermeshbulkupload.h
 * @brief Bulk mesh upload (#780): batch list that drives the model uploader.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
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

#ifndef LL_LLFLOATERMESHBULKUPLOAD_H
#define LL_LLFLOATERMESHBULKUPLOAD_H

#include "llfloater.h"
#include "llmeshbulkupload.h"

class LLButton;
class LLCheckBoxCtrl;
class LLFloaterModelPreview;
class LLScrollListCtrl;
class LLTextBox;
class LLViewerTextEditor;

// The batch keeps a list of models (a visual file plus detected LOD / physics files)
// and loads them one at a time into the regular model uploader (LLFloaterModelPreview),
// applies the settings carried over from the previous model and asks for the fee
// quote. The user presses Upload there; or "Upload all remaining" quotes every
// remaining model, asks once for the total L$ and then uploads them one after another.
// Uploads are never retried automatically.
class LLFloaterMeshBulkUpload : public LLFloater
{
public:
    LLFloaterMeshBulkUpload(const LLSD& key);
    ~LLFloaterMeshBulkUpload() override;

    bool postBuild() override;
    void onClose(bool app_quitting) override;

    // Opens the batch; new uploads go into (a new folder under) dest_folder.
    static void showBulkUpload(const LLUUID& dest_folder = LLUUID::null);

    // Calls from LLFloaterModelPreview while it is in bulk mode.
    static bool onPreviewUploadRequested(LLFloaterModelPreview* fmp);   // false: hold the upload
    static void onPreviewFeeReceived(LLFloaterModelPreview* fmp);
    static void onPreviewFeeError(LLFloaterModelPreview* fmp, S32 status, const std::string& reason);
    static void onPreviewUploadDone(LLFloaterModelPreview* fmp, bool success, bool charge_unknown, const std::string& reason);
    static void onPreviewClosed(LLFloaterModelPreview* fmp);

private:
    enum EStatus
    {
        ST_PENDING,
        ST_SKIPPED,
        ST_NO_MODEL,        // companions without a model file; never uploaded
        ST_LOADING,
        ST_QUOTING,
        ST_READY,           // quoted, waiting for Upload
        ST_INVALID,         // failed to load or failed validation
        ST_QUOTE_FAILED,
        ST_UPLOADING,
        ST_UPLOADED,
        ST_FAILED,          // server rejected the upload, nothing created
        ST_FAILED_UNKNOWN,  // network/HTTP failure: may or may not have been created and charged
    };

    struct Entry
    {
        LLMeshBulk::Group mGroup;
        EStatus mStatus = ST_PENDING;
        S32 mQuotedFee = -1;
        F32 mLandImpact = -1.f;
        U32 mQuoteGen = 0;          // settings generation the quote was made with
        S32 mConfirmedFee = -1;     // fee the user confirmed for "upload all"
        S32 mChargedFee = -1;       // quoted fee of the upload that completed
        std::string mMessage;
    };

    // Steps of loading one model into the uploader
    enum EStep
    {
        STEP_IDLE,
        STEP_WAIT_MODEL,        // high LOD loading / LODs generating
        STEP_COMPANIONS,        // LOD / physics files loading
        STEP_SETTLE,            // carried settings applied, waiting for the preview to refresh
        STEP_ANALYZE,           // carried physics analysis running
        STEP_WAIT_FEE,
        STEP_WAIT_USER,         // quoted; the user presses Upload (or the batch does)
        STEP_WAIT_UPLOAD,
    };

    enum ERunMode
    {
        RUN_MANUAL,             // load + quote each model; the user presses Upload
        RUN_QUOTE_ALL,          // quote every remaining model, then confirm the total
        RUN_UPLOAD_ALL,         // upload every remaining model (after confirmation)
    };

    static LLFloaterMeshBulkUpload* findBatch();
    static void onIdle(void* userdata);
    void idle();

    // UI
    void onAddFiles();
    void onAddFolder();
    void addFiles(const std::vector<std::string>& files);
    void onRemove();
    void onClear();
    void onToggleSkip();
    void onSetPhysics();
    void onClearCompanions();
    void onSetLOD();
    void onLoadSelected();
    void onApplySettings();
    void onQuoteAll();
    void onUploadAll();
    void onPause();
    void onCancelBatch();
    void refreshList();
    void refreshButtons();
    void log(const std::string& line);
    void report(const std::string& heading);

    // Batch
    S32  findNextEntry(bool need_quote) const;
    bool isRemaining(const Entry& entry) const;
    bool isBusy() const;
    void startEntry(S32 index);
    void stepCurrent();
    void onEntryNotReady(const std::string& why);
    void advance();
    void stopRun(const std::string& why);
    void confirmUploadAll();
    void onConfirmUploadAll(const LLSD& notification, const LLSD& response);
    LLSD captureForCarry(LLFloaterModelPreview* fmp) const;
    void setCarriedSettings(const LLSD& settings);
    void ensureFolder();
    LLFloaterModelPreview* getPreview(bool create);
    std::string entryLabel(S32 index) const;
    static std::string statusText(const Entry& entry);

    std::vector<Entry> mEntries;
    S32 mCurrent = -1;
    EStep mStep = STEP_IDLE;
    ERunMode mRunMode = RUN_MANUAL;
    bool mPaused = false;
    bool mConfirmAfterQuote = false;   // RUN_QUOTE_ALL started by "Upload all remaining"

    // companion loads still to do for the current entry: (lod, file, same file as the model)
    struct CompanionLoad { S32 mLOD; std::string mFile; bool mSameFile; };
    std::vector<CompanionLoad> mCompanionQueue;
    bool mLODFromFile[4] = { false, false, false, false };
    bool mPhysicsFromFile = false;
    S32  mSettleFrames = 0;

    LLSD mCarried;              // settings carried to the next model
    U32  mCarriedGen = 1;

    LLUUID mParentFolder;       // where uploads (or the new folder) go; null = Objects
    LLUUID mDestFolder;         // resolved destination; null until known
    bool mFolderResolved = false;
    bool mCreatingFolder = false;
    bool mUploadAfterFolder = false;
    std::string mSourceDirName;

    LLScrollListCtrl* mList = nullptr;
    LLViewerTextEditor* mLog = nullptr;
    LLTextBox* mTotals = nullptr;
    LLCheckBoxCtrl* mCarryCheck = nullptr;
    LLCheckBoxCtrl* mNewFolderCheck = nullptr;
};

#endif // LL_LLFLOATERMESHBULKUPLOAD_H
