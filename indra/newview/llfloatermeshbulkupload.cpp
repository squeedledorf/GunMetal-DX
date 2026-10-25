/**
 * @file llfloatermeshbulkupload.cpp
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

#include "llviewerprecompiledheaders.h"

#include "llfloatermeshbulkupload.h"

#include "llbutton.h"
#include "llcallbacklist.h"
#include "llcheckboxctrl.h"
#include "llcombobox.h"
#include "lldir.h"
#include "lldirpicker.h"
#include "llfilepicker.h"
#include "llfloatermodelpreview.h"
#include "llfloaterreg.h"
#include "llinventorymodel.h"
#include "lllineeditor.h"
#include "llnotificationsutil.h"
#include "llscrolllistctrl.h"
#include "llsdutil.h"
#include "llstatusbar.h"
#include "lltextbox.h"
#include "llviewerinventory.h"
#include "llviewermenufile.h"
#include "llviewertexteditor.h"

namespace
{
    const char* const FLOATER_NAME = "mesh_bulk_upload";
    const char* const PREVIEW_NAME = "upload_model";
    // Idle frames the uploader must stay settled before the next step (its refresh is draw-driven).
    constexpr S32 SETTLE_FRAMES = 3;

    std::string short_name(const std::string& path)
    {
        return path.empty() ? std::string() : gDirUtilp->getBaseFileName(path, false);
    }
}

LLFloaterMeshBulkUpload::LLFloaterMeshBulkUpload(const LLSD& key)
    : LLFloater(key)
{
}

LLFloaterMeshBulkUpload::~LLFloaterMeshBulkUpload()
{
    gIdleCallbacks.deleteFunction(onIdle, this);
}

bool LLFloaterMeshBulkUpload::postBuild()
{
    mList = getChild<LLScrollListCtrl>("model_list");
    mList->setCommitCallback([this](LLUICtrl*, const LLSD&) { refreshButtons(); });
    mList->setDoubleClickCallback([this]() { onLoadSelected(); });
    mLog = getChild<LLViewerTextEditor>("batch_log");
    mTotals = getChild<LLTextBox>("totals_text");
    mCarryCheck = getChild<LLCheckBoxCtrl>("carry_settings_check");
    mNewFolderCheck = getChild<LLCheckBoxCtrl>("new_folder_check");
    mNewFolderCheck->setCommitCallback([this](LLUICtrl*, const LLSD&) { refreshButtons(); });
    getChild<LLComboBox>("lod_choice_combo")->selectFirstItem();

    getChild<LLButton>("add_files_btn")->setClickedCallback([this](LLUICtrl*, const LLSD&) { onAddFiles(); });
    getChild<LLButton>("add_folder_btn")->setClickedCallback([this](LLUICtrl*, const LLSD&) { onAddFolder(); });
    getChild<LLButton>("remove_btn")->setClickedCallback([this](LLUICtrl*, const LLSD&) { onRemove(); });
    getChild<LLButton>("clear_btn")->setClickedCallback([this](LLUICtrl*, const LLSD&) { onClear(); });
    getChild<LLButton>("skip_btn")->setClickedCallback([this](LLUICtrl*, const LLSD&) { onToggleSkip(); });
    getChild<LLButton>("set_physics_btn")->setClickedCallback([this](LLUICtrl*, const LLSD&) { onSetPhysics(); });
    getChild<LLButton>("set_lod_btn")->setClickedCallback([this](LLUICtrl*, const LLSD&) { onSetLOD(); });
    getChild<LLButton>("clear_companions_btn")->setClickedCallback([this](LLUICtrl*, const LLSD&) { onClearCompanions(); });
    getChild<LLButton>("load_btn")->setClickedCallback([this](LLUICtrl*, const LLSD&) { onLoadSelected(); });
    getChild<LLButton>("apply_settings_btn")->setClickedCallback([this](LLUICtrl*, const LLSD&) { onApplySettings(); });
    getChild<LLButton>("quote_all_btn")->setClickedCallback([this](LLUICtrl*, const LLSD&) { onQuoteAll(); });
    getChild<LLButton>("upload_all_btn")->setClickedCallback([this](LLUICtrl*, const LLSD&) { onUploadAll(); });
    getChild<LLButton>("pause_btn")->setClickedCallback([this](LLUICtrl*, const LLSD&) { onPause(); });
    getChild<LLButton>("cancel_batch_btn")->setClickedCallback([this](LLUICtrl*, const LLSD&) { onCancelBatch(); });

    gIdleCallbacks.addFunction(onIdle, this);

    refreshList();
    return true;
}

void LLFloaterMeshBulkUpload::onClose(bool app_quitting)
{
    if (app_quitting)
    {
        return;
    }
    // Closing the batch window ends the run; an upload already sent still reports here.
    if (mStep != STEP_WAIT_UPLOAD)
    {
        mStep = STEP_IDLE;
        if (mCurrent >= 0 && mCurrent < (S32)mEntries.size())
        {
            Entry& entry = mEntries[mCurrent];
            if (entry.mStatus == ST_LOADING || entry.mStatus == ST_QUOTING)
            {
                entry.mStatus = ST_PENDING;
            }
        }
    }
    mRunMode = RUN_MANUAL;
    if (mCurrent >= 0)
    {
        mPaused = true; // a running upload still reports here, but nothing new starts
    }
    if (LLFloaterModelPreview* fmp = getPreview(false))
    {
        if (fmp->isBulkMode())
        {
            fmp->setBulkMode(false); // the uploader goes back to single-model behaviour
        }
    }
}

//static
void LLFloaterMeshBulkUpload::showBulkUpload(const LLUUID& dest_folder)
{
    LLFloaterMeshBulkUpload* self = LLFloaterReg::showTypedInstance<LLFloaterMeshBulkUpload>(FLOATER_NAME);
    if (!self)
    {
        return;
    }
    if (!self->mFolderResolved && !self->mCreatingFolder && dest_folder.notNull())
    {
        self->mParentFolder = dest_folder;
    }
    self->refreshButtons();
}

//static
LLFloaterMeshBulkUpload* LLFloaterMeshBulkUpload::findBatch()
{
    return LLFloaterReg::findTypedInstance<LLFloaterMeshBulkUpload>(FLOATER_NAME);
}

LLFloaterModelPreview* LLFloaterMeshBulkUpload::getPreview(bool create)
{
    return create ? LLFloaterReg::getTypedInstance<LLFloaterModelPreview>(PREVIEW_NAME)
                  : LLFloaterReg::findTypedInstance<LLFloaterModelPreview>(PREVIEW_NAME);
}

//-----------------------------------------------------------------------------
// List building
//-----------------------------------------------------------------------------

void LLFloaterMeshBulkUpload::onAddFiles()
{
    LLHandle<LLFloater> handle = getHandle();
    LLFilePickerReplyThread::startPicker(
        [handle](const std::vector<std::string>& filenames, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter)
        {
            if (LLFloaterMeshBulkUpload* self = dynamic_cast<LLFloaterMeshBulkUpload*>(handle.get()))
            {
                self->addFiles(filenames);
            }
        },
        LLFilePicker::FFLOAD_MODEL, true);
}

void LLFloaterMeshBulkUpload::onAddFolder()
{
    LLHandle<LLFloater> handle = getHandle();
    (new LLDirPickerThread(
        [handle](const std::vector<std::string>& filenames, std::string)
        {
            LLFloaterMeshBulkUpload* self = dynamic_cast<LLFloaterMeshBulkUpload*>(handle.get());
            if (!self || filenames.empty() || filenames[0].empty())
            {
                return;
            }
            std::vector<std::string> files = LLMeshBulk::listModelFiles(filenames[0]);
            if (files.empty())
            {
                self->log("No model files (.dae .gltf .glb .fbx) in " + filenames[0] + " (subfolders are not scanned).");
                return;
            }
            self->addFiles(files);
        },
        std::string()))->getFile();
}

void LLFloaterMeshBulkUpload::addFiles(const std::vector<std::string>& files)
{
    if (files.empty())
    {
        return;
    }

    if (mSourceDirName.empty())
    {
        std::string dir = gDirUtilp->getDirName(files.front());
        mSourceDirName = gDirUtilp->getBaseFileName(dir, false);
        LLLineEditor* folder_name = getChild<LLLineEditor>("folder_name_editor");
        if (folder_name->getText().empty())
        {
            folder_name->setText(mSourceDirName.empty() ? std::string("Bulk upload") : mSourceDirName);
        }
    }

    std::vector<LLMeshBulk::Group> groups = LLMeshBulk::groupFiles(files);
    S32 added = 0;
    S32 merged = 0;
    for (LLMeshBulk::Group& group : groups)
    {
        for (const std::string& note : group.mNotes)
        {
            log(group.mName + ": " + note);
        }

        // Same base name as a row already in the list: fill in what that row is missing.
        const std::string key = LLMeshBulk::toLowerCopy(group.mName);
        Entry* existing = nullptr;
        for (Entry& entry : mEntries)
        {
            if (LLMeshBulk::toLowerCopy(entry.mGroup.mName) == key)
            {
                existing = &entry;
                break;
            }
        }
        if (existing)
        {
            if (existing->mStatus == ST_UPLOADING || existing->mStatus == ST_UPLOADED)
            {
                log(group.mName + ": already uploaded in this batch; new files for it were ignored.");
                continue;
            }
            LLMeshBulk::Group& target = existing->mGroup;
            if (target.mVisual.empty())
            {
                target.mVisual = group.mVisual;
            }
            for (S32 lod = 0; lod < 3; ++lod)
            {
                if (target.mLODFile[lod].empty())
                {
                    target.mLODFile[lod] = group.mLODFile[lod];
                }
            }
            if (target.mPhysicsFile.empty())
            {
                target.mPhysicsFile = group.mPhysicsFile;
            }
            if (existing->mStatus == ST_NO_MODEL && !target.mVisual.empty())
            {
                existing->mStatus = ST_PENDING;
            }
            existing->mQuotedFee = -1;
            ++merged;
            continue;
        }

        Entry entry;
        entry.mGroup = group;
        entry.mStatus = group.mVisual.empty() ? ST_NO_MODEL : ST_PENDING;
        if (entry.mStatus == ST_NO_MODEL)
        {
            entry.mMessage = "no model file with this name; not uploaded";
        }
        mEntries.push_back(entry);
        ++added;
    }

    S32 with_physics = 0;
    S32 models = 0;
    for (const Entry& entry : mEntries)
    {
        if (!entry.mGroup.mVisual.empty())
        {
            ++models;
            if (!entry.mGroup.mPhysicsFile.empty())
            {
                ++with_physics;
            }
        }
    }
    log(llformat("Added %d models (%d merged into existing rows). The list has %d models, %d with a physics file.",
                 added, merged, models, with_physics));
    refreshList();
}

void LLFloaterMeshBulkUpload::onRemove()
{
    const S32 index = mList->getFirstSelectedIndex();
    if (index < 0 || index >= (S32)mEntries.size())
    {
        return;
    }
    if (index == mCurrent && isBusy())
    {
        log("This model is being loaded or uploaded; pause or wait first.");
        return;
    }
    if (mEntries[index].mStatus == ST_UPLOADING)
    {
        return;
    }
    mEntries.erase(mEntries.begin() + index);
    if (mCurrent == index)
    {
        mCurrent = -1;
        mStep = STEP_IDLE;
    }
    else if (mCurrent > index)
    {
        --mCurrent;
    }
    refreshList();
}

void LLFloaterMeshBulkUpload::onClear()
{
    if (isBusy())
    {
        log("The batch is busy; pause or wait first.");
        return;
    }
    if (!mEntries.empty())
    {
        report("Batch cleared");
    }
    mEntries.clear();
    mCurrent = -1;
    mStep = STEP_IDLE;
    mRunMode = RUN_MANUAL;
    mPaused = false;
    mFolderResolved = false;
    mDestFolder.setNull();
    mSourceDirName.clear();
    getChild<LLLineEditor>("folder_name_editor")->setText(LLStringUtil::null);
    if (LLFloaterModelPreview* fmp = getPreview(false))
    {
        if (fmp->isBulkMode())
        {
            fmp->setBulkMode(false);
        }
    }
    refreshList();
}

void LLFloaterMeshBulkUpload::onToggleSkip()
{
    const S32 index = mList->getFirstSelectedIndex();
    if (index < 0 || index >= (S32)mEntries.size() || (index == mCurrent && isBusy()))
    {
        return;
    }
    Entry& entry = mEntries[index];
    switch (entry.mStatus)
    {
    case ST_SKIPPED:
        entry.mStatus = ST_PENDING;
        break;
    case ST_PENDING:
    case ST_READY:
    case ST_INVALID:
    case ST_QUOTE_FAILED:
        entry.mStatus = ST_SKIPPED;
        break;
    default:
        return;
    }
    refreshList();
}

void LLFloaterMeshBulkUpload::onSetPhysics()
{
    const S32 index = mList->getFirstSelectedIndex();
    if (index < 0 || index >= (S32)mEntries.size())
    {
        return;
    }
    const std::string name = mEntries[index].mGroup.mName;
    LLHandle<LLFloater> handle = getHandle();
    LLFilePickerReplyThread::startPicker(
        [handle, name](const std::vector<std::string>& filenames, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter)
        {
            LLFloaterMeshBulkUpload* self = dynamic_cast<LLFloaterMeshBulkUpload*>(handle.get());
            if (!self || filenames.empty())
            {
                return;
            }
            for (Entry& entry : self->mEntries)
            {
                if (entry.mGroup.mName == name && entry.mStatus != ST_UPLOADING && entry.mStatus != ST_UPLOADED)
                {
                    entry.mGroup.mPhysicsFile = filenames[0];
                    entry.mQuotedFee = -1;
                    if (entry.mStatus == ST_READY)
                    {
                        entry.mStatus = ST_PENDING;
                    }
                    self->log(name + ": physics file set to " + short_name(filenames[0]));
                    break;
                }
            }
            self->refreshList();
        },
        LLFilePicker::FFLOAD_MODEL, false);
}

void LLFloaterMeshBulkUpload::onSetLOD()
{
    const S32 index = mList->getFirstSelectedIndex();
    if (index < 0 || index >= (S32)mEntries.size())
    {
        return;
    }
    // lod_choice_combo: Medium, Low, Lowest
    const S32 choice = getChild<LLComboBox>("lod_choice_combo")->getCurrentIndex();
    const S32 lod = llclamp(2 - choice, 0, 2);
    const std::string name = mEntries[index].mGroup.mName;
    LLHandle<LLFloater> handle = getHandle();
    LLFilePickerReplyThread::startPicker(
        [handle, name, lod](const std::vector<std::string>& filenames, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter)
        {
            LLFloaterMeshBulkUpload* self = dynamic_cast<LLFloaterMeshBulkUpload*>(handle.get());
            if (!self || filenames.empty())
            {
                return;
            }
            for (Entry& entry : self->mEntries)
            {
                if (entry.mGroup.mName == name && entry.mStatus != ST_UPLOADING && entry.mStatus != ST_UPLOADED)
                {
                    entry.mGroup.mLODFile[lod] = filenames[0];
                    entry.mQuotedFee = -1;
                    if (entry.mStatus == ST_READY)
                    {
                        entry.mStatus = ST_PENDING;
                    }
                    self->log(llformat("%s: LOD%d file set to ", name.c_str(), lod) + short_name(filenames[0]));
                    break;
                }
            }
            self->refreshList();
        },
        LLFilePicker::FFLOAD_MODEL, false);
}

void LLFloaterMeshBulkUpload::onClearCompanions()
{
    const S32 index = mList->getFirstSelectedIndex();
    if (index < 0 || index >= (S32)mEntries.size() || (index == mCurrent && isBusy()))
    {
        return;
    }
    Entry& entry = mEntries[index];
    if (entry.mStatus == ST_UPLOADING || entry.mStatus == ST_UPLOADED)
    {
        return;
    }
    entry.mGroup.mPhysicsFile.clear();
    for (S32 lod = 0; lod < 4; ++lod)
    {
        entry.mGroup.mLODFile[lod].clear();
    }
    entry.mQuotedFee = -1;
    if (entry.mStatus == ST_READY)
    {
        entry.mStatus = ST_PENDING;
    }
    log(entry.mGroup.mName + ": LOD and physics files cleared; the carried settings apply.");
    refreshList();
}

//-----------------------------------------------------------------------------
// Display
//-----------------------------------------------------------------------------

//static
std::string LLFloaterMeshBulkUpload::statusText(const Entry& entry)
{
    std::string text;
    switch (entry.mStatus)
    {
    case ST_PENDING:        text = "Waiting"; break;
    case ST_SKIPPED:        text = "Skipped"; break;
    case ST_NO_MODEL:       text = "No model file"; break;
    case ST_LOADING:        text = "Loading..."; break;
    case ST_QUOTING:        text = "Getting fee..."; break;
    case ST_READY:          text = "Quoted"; break;
    case ST_INVALID:        text = "Problem"; break;
    case ST_QUOTE_FAILED:   text = "Fee request failed"; break;
    case ST_UPLOADING:      text = "Uploading..."; break;
    case ST_UPLOADED:       text = llformat("Uploaded, charged L$%d", entry.mChargedFee); break;
    case ST_FAILED:         text = "Upload failed, not charged"; break;
    case ST_FAILED_UNKNOWN: text = "Upload failed, charge unknown"; break;
    }
    if (!entry.mMessage.empty())
    {
        text += ": " + entry.mMessage;
    }
    return text;
}

std::string LLFloaterMeshBulkUpload::entryLabel(S32 index) const
{
    return llformat("%d of %d: ", index + 1, (S32)mEntries.size()) + mEntries[index].mGroup.mName;
}

void LLFloaterMeshBulkUpload::refreshList()
{
    const S32 selected = mList->getFirstSelectedIndex();
    mList->deleteAllItems();

    static const char* lod_short[] = { "lowest", "low", "medium" };
    for (S32 i = 0; i < (S32)mEntries.size(); ++i)
    {
        const Entry& entry = mEntries[i];
        const LLMeshBulk::Group& group = entry.mGroup;

        std::string lods;
        for (S32 lod = 2; lod >= 0; --lod)
        {
            if (!group.mLODFile[lod].empty())
            {
                lods += (lods.empty() ? "" : ", ") + std::string(lod_short[lod]);
            }
        }
        lods = lods.empty() ? std::string("generated") : lods + " from file";

        std::string physics = group.mPhysicsFile.empty() ? std::string("from settings") : short_name(group.mPhysicsFile);

        std::string fee = "-";
        if (entry.mStatus == ST_UPLOADED)
        {
            fee = llformat("L$%d", entry.mChargedFee);
        }
        else if (entry.mQuotedFee >= 0)
        {
            fee = llformat("L$%d", entry.mQuotedFee) + (entry.mQuoteGen == mCarriedGen ? "" : " (old settings)");
        }
        const std::string li = entry.mLandImpact >= 0.f ? llformat("%.1f", entry.mLandImpact) : std::string("-");

        LLSD row;
        row["value"] = i;
        row["columns"][0]["column"] = "num";
        row["columns"][0]["value"] = i + 1;
        row["columns"][1]["column"] = "name";
        row["columns"][1]["value"] = group.mName + (i == mCurrent ? " <" : "");
        row["columns"][2]["column"] = "lods";
        row["columns"][2]["value"] = group.mVisual.empty() ? std::string("-") : lods;
        row["columns"][3]["column"] = "physics";
        row["columns"][3]["value"] = group.mVisual.empty() ? std::string("-") : physics;
        row["columns"][4]["column"] = "li";
        row["columns"][4]["value"] = li;
        row["columns"][5]["column"] = "fee";
        row["columns"][5]["value"] = fee;
        row["columns"][6]["column"] = "status";
        row["columns"][6]["value"] = statusText(entry);
        mList->addElement(row);
    }
    if (selected >= 0 && selected < (S32)mEntries.size())
    {
        mList->selectNthItem(selected);
    }
    refreshButtons();
}

void LLFloaterMeshBulkUpload::refreshButtons()
{
    const bool busy = isBusy();
    const S32 index = mList->getFirstSelectedIndex();
    const bool has_sel = index >= 0 && index < (S32)mEntries.size();
    const bool sel_editable = has_sel && !(index == mCurrent && busy)
        && mEntries[index].mStatus != ST_UPLOADING && mEntries[index].mStatus != ST_UPLOADED;

    S32 models = 0, uploaded = 0, charged = 0, failed = 0, unknown = 0, remaining = 0, quoted = 0, quoted_fee = 0;
    F32 quoted_li = 0.f;
    for (const Entry& entry : mEntries)
    {
        if (entry.mGroup.mVisual.empty())
        {
            continue;
        }
        ++models;
        switch (entry.mStatus)
        {
        case ST_UPLOADED:
            ++uploaded;
            charged += llmax(entry.mChargedFee, 0);
            break;
        case ST_FAILED:
            ++failed;
            break;
        case ST_FAILED_UNKNOWN:
            ++unknown;
            break;
        case ST_PENDING:
        case ST_READY:
        case ST_LOADING:
        case ST_QUOTING:
            ++remaining;
            if (entry.mQuotedFee >= 0 && entry.mQuoteGen == mCarriedGen)
            {
                ++quoted;
                quoted_fee += entry.mQuotedFee;
                quoted_li += llmax(entry.mLandImpact, 0.f);
            }
            break;
        default:
            break;
        }
    }

    std::string totals = llformat("%d models. Uploaded %d (charged L$%d). Remaining %d: %d quoted, L$%d, land impact %.1f.",
                                  models, uploaded, charged, remaining, quoted, quoted_fee, quoted_li);
    if (failed || unknown)
    {
        totals += llformat(" Failed: %d not charged, %d charge unknown.", failed, unknown);
    }
    mTotals->setText(totals);

    getChildView("add_files_btn")->setEnabled(!busy);
    getChildView("add_folder_btn")->setEnabled(!busy);
    getChildView("clear_btn")->setEnabled(!busy && !mEntries.empty());
    getChildView("remove_btn")->setEnabled(sel_editable);
    getChildView("skip_btn")->setEnabled(sel_editable);
    getChildView("set_physics_btn")->setEnabled(sel_editable && !mEntries[index].mGroup.mVisual.empty());
    getChildView("set_lod_btn")->setEnabled(sel_editable && !mEntries[index].mGroup.mVisual.empty());
    getChildView("clear_companions_btn")->setEnabled(sel_editable);
    getChildView("load_btn")->setEnabled(!busy && (remaining > 0 || (has_sel && !mEntries[index].mGroup.mVisual.empty())));
    getChildView("quote_all_btn")->setEnabled(!busy && remaining > 0);
    getChildView("upload_all_btn")->setEnabled(!busy && remaining > 0);
    getChildView("apply_settings_btn")->setEnabled(mCurrent >= 0 && mStep == STEP_WAIT_USER);
    getChildView("cancel_batch_btn")->setEnabled(mRunMode != RUN_MANUAL || busy || mCurrent >= 0);
    LLButton* pause = getChild<LLButton>("pause_btn");
    pause->setLabel(mPaused ? getString("resume_label") : getString("pause_label"));
    pause->setEnabled(mPaused || mRunMode != RUN_MANUAL || busy);

    const bool folder_locked = mFolderResolved || mCreatingFolder;
    mNewFolderCheck->setEnabled(!folder_locked);
    getChildView("folder_name_editor")->setEnabled(!folder_locked && mNewFolderCheck->get());

    std::string dest;
    if (mFolderResolved)
    {
        const LLViewerInventoryCategory* cat = mDestFolder.notNull() ? gInventory.getCategory(mDestFolder) : nullptr;
        dest = cat ? cat->getName() : getString("default_folder");
    }
    else
    {
        const LLViewerInventoryCategory* cat = mParentFolder.notNull() ? gInventory.getCategory(mParentFolder) : nullptr;
        dest = cat ? cat->getName() : getString("default_folder");
        if (mNewFolderCheck->get())
        {
            dest += " / " + getChild<LLLineEditor>("folder_name_editor")->getText() + " (" + getString("created_on_first_upload") + ")";
        }
    }
    getChild<LLTextBox>("destination_text")->setTextArg("[FOLDER]", dest);
}

void LLFloaterMeshBulkUpload::log(const std::string& line)
{
    LL_INFOS("MeshBulkUpload") << line << LL_ENDL;
    if (mLog)
    {
        mLog->appendText(line, true);
    }
}

void LLFloaterMeshBulkUpload::report(const std::string& heading)
{
    std::string uploaded, failed, unknown, not_uploaded;
    S32 n_uploaded = 0, n_failed = 0, n_unknown = 0, n_not = 0, charged = 0;
    for (const Entry& entry : mEntries)
    {
        if (entry.mGroup.mVisual.empty())
        {
            continue;
        }
        const std::string& name = entry.mGroup.mName;
        switch (entry.mStatus)
        {
        case ST_UPLOADED:
            uploaded += (n_uploaded++ ? ", " : "") + name + llformat(" (L$%d)", entry.mChargedFee);
            charged += llmax(entry.mChargedFee, 0);
            break;
        case ST_FAILED:
            failed += (n_failed++ ? ", " : "") + name;
            break;
        case ST_FAILED_UNKNOWN:
        case ST_UPLOADING:
            unknown += (n_unknown++ ? ", " : "") + name;
            break;
        default:
            not_uploaded += (n_not++ ? ", " : "") + name;
            break;
        }
    }
    log("--- " + heading + " ---");
    log(llformat("Uploaded and charged (%d models, L$%d from the quoted fees): ", n_uploaded, charged)
        + (uploaded.empty() ? std::string("none") : uploaded));
    if (n_failed)
    {
        log(llformat("Failed, not charged (%d): ", n_failed) + failed);
    }
    if (n_unknown)
    {
        log(llformat("Failed or unfinished, charge UNKNOWN - check your inventory and L$ history before uploading these again (%d): ", n_unknown) + unknown);
    }
    log(llformat("Not uploaded (%d): ", n_not) + (not_uploaded.empty() ? std::string("none") : not_uploaded));
}

//-----------------------------------------------------------------------------
// Batch control
//-----------------------------------------------------------------------------

bool LLFloaterMeshBulkUpload::isBusy() const
{
    return mCreatingFolder
        || mStep == STEP_WAIT_MODEL || mStep == STEP_COMPANIONS || mStep == STEP_SETTLE
        || mStep == STEP_ANALYZE || mStep == STEP_WAIT_FEE || mStep == STEP_WAIT_UPLOAD;
}

bool LLFloaterMeshBulkUpload::isRemaining(const Entry& entry) const
{
    return !entry.mGroup.mVisual.empty() && (entry.mStatus == ST_PENDING || entry.mStatus == ST_READY);
}

S32 LLFloaterMeshBulkUpload::findNextEntry(bool need_quote) const
{
    for (S32 i = 0; i < (S32)mEntries.size(); ++i)
    {
        const Entry& entry = mEntries[i];
        if (!isRemaining(entry))
        {
            continue;
        }
        if (need_quote && entry.mStatus == ST_READY && entry.mQuotedFee >= 0 && entry.mQuoteGen == mCarriedGen)
        {
            continue; // already quoted with the current settings
        }
        if (mRunMode == RUN_UPLOAD_ALL && entry.mConfirmedFee < 0)
        {
            continue; // not part of what the user confirmed
        }
        return i;
    }
    return -1;
}

LLSD LLFloaterMeshBulkUpload::captureForCarry(LLFloaterModelPreview* fmp) const
{
    LLSD settings = fmp->captureBulkSettings();
    // Levels this model took from its own files say nothing about the next model: keep
    // what was carried for them before (a model with a _phys file must not wipe the
    // physics choice used for models without one).
    if (mPhysicsFromFile)
    {
        if (mCarried.has("physics_combo"))
        {
            settings["physics_combo"] = mCarried["physics_combo"];
        }
        else
        {
            settings.erase("physics_combo");
        }
    }
    const LLSD& old_lods = mCarried["lod"];
    for (S32 lod = 0; lod < 4; ++lod)
    {
        if (mLODFromFile[lod] && old_lods.isArray() && lod < old_lods.size())
        {
            settings["lod"][lod] = old_lods[lod];
        }
    }
    return settings;
}

void LLFloaterMeshBulkUpload::setCarriedSettings(const LLSD& settings)
{
    if (!llsd_equals(settings, mCarried))
    {
        mCarried = settings;
        ++mCarriedGen;
    }
}

void LLFloaterMeshBulkUpload::stopRun(const std::string& why)
{
    if (!why.empty())
    {
        log(why);
    }
    mRunMode = RUN_MANUAL;
    refreshButtons();
}

void LLFloaterMeshBulkUpload::startEntry(S32 index)
{
    if (index < 0 || index >= (S32)mEntries.size())
    {
        return;
    }
    LLFloaterModelPreview* fmp = getPreview(true);
    if (!fmp)
    {
        stopRun("Could not open the model uploader.");
        return;
    }

    mCurrent = index;
    Entry& entry = mEntries[index];
    entry.mStatus = ST_LOADING;
    entry.mMessage.clear();
    mCompanionQueue.clear();
    for (bool& from_file : mLODFromFile)
    {
        from_file = false;
    }
    mPhysicsFromFile = false;
    mSettleFrames = 0;
    mStep = STEP_WAIT_MODEL;

    log("Loading " + entryLabel(index) + " (" + short_name(entry.mGroup.mVisual) + ")");
    fmp->loadBulkModel(entry.mGroup.mVisual, mFolderResolved ? mDestFolder : mParentFolder, entryLabel(index));
    refreshList();
    mList->selectNthItem(index);
}

void LLFloaterMeshBulkUpload::advance()
{
    mStep = STEP_IDLE;
    if (mPaused)
    {
        log("Paused.");
        refreshList();
        return;
    }

    const S32 next = findNextEntry(mRunMode == RUN_QUOTE_ALL);
    if (next >= 0)
    {
        startEntry(next);
        return;
    }

    if (mRunMode == RUN_QUOTE_ALL)
    {
        mRunMode = RUN_MANUAL;
        refreshList();
        if (mConfirmAfterQuote)
        {
            confirmUploadAll();
        }
        else
        {
            log("Fee quotes done; see the totals above the log.");
        }
        return;
    }

    // Nothing left
    mRunMode = RUN_MANUAL;
    mCurrent = -1;
    report("Batch finished");
    if (LLFloaterModelPreview* fmp = getPreview(false))
    {
        if (fmp->isBulkMode())
        {
            fmp->setBulkMode(false);
            fmp->closeFloater(false);
        }
    }
    refreshList();
}

void LLFloaterMeshBulkUpload::onIdle(void* userdata)
{
    LLFloaterMeshBulkUpload* self = static_cast<LLFloaterMeshBulkUpload*>(userdata);
    if (self)
    {
        self->idle();
    }
}

void LLFloaterMeshBulkUpload::idle()
{
    switch (mStep)
    {
    case STEP_WAIT_MODEL:
    case STEP_COMPANIONS:
    case STEP_SETTLE:
    case STEP_ANALYZE:
        stepCurrent();
        break;
    default:
        break;
    }
}

void LLFloaterMeshBulkUpload::onEntryNotReady(const std::string& why)
{
    Entry& entry = mEntries[mCurrent];
    entry.mStatus = ST_INVALID;
    entry.mMessage = why;
    log(entry.mGroup.mName + ": " + why);
    if (mRunMode == RUN_MANUAL)
    {
        // Leave it in the uploader: the user can fix it there and press Calculate / Upload.
        mStep = STEP_WAIT_USER;
        refreshList();
    }
    else
    {
        log(entry.mGroup.mName + " is left out of this run.");
        advance();
    }
}

void LLFloaterMeshBulkUpload::stepCurrent()
{
    LLFloaterModelPreview* fmp = getPreview(false);
    if (!fmp || !fmp->isBulkMode() || mCurrent < 0 || mCurrent >= (S32)mEntries.size())
    {
        mStep = STEP_IDLE;
        refreshButtons();
        return;
    }
    if (!fmp->isBulkSettled())
    {
        mSettleFrames = 0;
        return;
    }
    if (++mSettleFrames < SETTLE_FRAMES)
    {
        return;
    }
    mSettleFrames = 0;

    Entry& entry = mEntries[mCurrent];
    const LLMeshBulk::Group& group = entry.mGroup;

    switch (mStep)
    {
    case STEP_WAIT_MODEL:
    {
        if (fmp->hasBulkLoadError())
        {
            onEntryNotReady("could not load " + short_name(group.mVisual) + " (see the uploader's log)");
            return;
        }
        // Separate files first; otherwise objects named for a LOD / physics inside the model's own file.
        const U32 in_file = fmp->getBulkInFileRoles();
        for (S32 lod = LLModel::LOD_MEDIUM; lod >= LLModel::LOD_IMPOSTOR; --lod)
        {
            if (!group.mLODFile[lod].empty())
            {
                mCompanionQueue.push_back({ lod, group.mLODFile[lod], false });
                mLODFromFile[lod] = true;
            }
            else if (in_file & (1u << lod))
            {
                mCompanionQueue.push_back({ lod, group.mVisual, true });
                mLODFromFile[lod] = true;
            }
        }
        if (!group.mPhysicsFile.empty())
        {
            mCompanionQueue.push_back({ LLModel::LOD_PHYSICS, group.mPhysicsFile, false });
            mPhysicsFromFile = true;
        }
        else if (in_file & (1u << LLMeshBulk::ROLE_PHYSICS))
        {
            mCompanionQueue.push_back({ LLModel::LOD_PHYSICS, group.mVisual, true });
            mPhysicsFromFile = true;
        }
        if (in_file)
        {
            log(group.mName + ": LOD / physics objects found inside " + short_name(group.mVisual));
        }
        mStep = STEP_COMPANIONS;
        break;
    }

    case STEP_COMPANIONS:
        if (!mCompanionQueue.empty())
        {
            const CompanionLoad load = mCompanionQueue.front();
            mCompanionQueue.erase(mCompanionQueue.begin());
            fmp->loadBulkCompanion(load.mLOD, load.mFile, load.mSameFile);
            break;
        }
        if (fmp->hasBulkLoadError())
        {
            onEntryNotReady("a LOD or physics file did not load (see the uploader's log)");
            return;
        }
        fmp->applyBulkSettings(mCarried, mLODFromFile, mPhysicsFromFile);
        mStep = STEP_SETTLE;
        break;

    case STEP_SETTLE:
        if (fmp->startBulkAnalyze(mCarried))
        {
            log(group.mName + ": analyzing the physics shape as for the previous model");
            mStep = STEP_ANALYZE;
            break;
        }
        [[fallthrough]]; // nothing to analyze
    case STEP_ANALYZE:
        if (!fmp->canBulkQuote())
        {
            onEntryNotReady("not uploadable: " + fmp->getBulkProblem());
            return;
        }
        entry.mStatus = ST_QUOTING;
        mStep = STEP_WAIT_FEE;
        fmp->requestBulkFeeQuote();
        refreshList();
        break;

    default:
        break;
    }
}

//-----------------------------------------------------------------------------
// User actions
//-----------------------------------------------------------------------------

void LLFloaterMeshBulkUpload::onLoadSelected()
{
    if (isBusy())
    {
        log("The batch is busy; wait for the current step or pause first.");
        return;
    }
    S32 index = mList->getFirstSelectedIndex();
    if (index < 0 || index >= (S32)mEntries.size() || mEntries[index].mGroup.mVisual.empty()
        || mEntries[index].mStatus == ST_SKIPPED)
    {
        mRunMode = RUN_MANUAL;
        index = findNextEntry(false);
    }
    if (index < 0)
    {
        log("Nothing left to load.");
        return;
    }
    Entry& entry = mEntries[index];
    if (entry.mStatus == ST_UPLOADED || entry.mStatus == ST_FAILED_UNKNOWN)
    {
        log(entry.mGroup.mName + " may already be in your inventory and paid for. Check your inventory and L$ history; "
            "pressing Upload again uploads and charges it again.");
    }
    mRunMode = RUN_MANUAL;
    mPaused = false;
    startEntry(index);
}

void LLFloaterMeshBulkUpload::onApplySettings()
{
    LLFloaterModelPreview* fmp = getPreview(false);
    if (!fmp || !fmp->isBulkMode() || mCurrent < 0 || mCurrent >= (S32)mEntries.size())
    {
        log("Load a model into the uploader first.");
        return;
    }
    setCarriedSettings(captureForCarry(fmp));
    log("The uploader's current settings (" + mEntries[mCurrent].mGroup.mName + ") now apply to every remaining model.");
    refreshList();
}

void LLFloaterMeshBulkUpload::onQuoteAll()
{
    if (isBusy())
    {
        return;
    }
    mPaused = false;
    mConfirmAfterQuote = false;
    mRunMode = RUN_QUOTE_ALL;
    log("Getting fee quotes for the remaining models (nothing is uploaded or charged)...");
    advance();
}

void LLFloaterMeshBulkUpload::onUploadAll()
{
    if (isBusy())
    {
        return;
    }
    // The model on screen may have been configured by hand: its settings apply to the rest.
    LLFloaterModelPreview* fmp = getPreview(false);
    if (fmp && fmp->isBulkMode() && mCurrent >= 0 && mStep == STEP_WAIT_USER && mCarryCheck->get())
    {
        setCarriedSettings(captureForCarry(fmp));
    }
    mPaused = false;
    mConfirmAfterQuote = true;
    mRunMode = RUN_QUOTE_ALL;
    if (findNextEntry(true) >= 0)
    {
        log("Getting fee quotes for the remaining models before asking for confirmation (nothing is charged yet)...");
        advance();
    }
    else
    {
        mRunMode = RUN_MANUAL;
        confirmUploadAll();
    }
}

void LLFloaterMeshBulkUpload::confirmUploadAll()
{
    S32 count = 0;
    S32 total = 0;
    F32 land_impact = 0.f;
    S32 not_quoted = 0;
    for (const Entry& entry : mEntries)
    {
        if (!isRemaining(entry))
        {
            if (!entry.mGroup.mVisual.empty() && (entry.mStatus == ST_INVALID || entry.mStatus == ST_QUOTE_FAILED))
            {
                ++not_quoted;
            }
            continue;
        }
        if (entry.mStatus == ST_READY && entry.mQuotedFee >= 0 && entry.mQuoteGen == mCarriedGen)
        {
            ++count;
            total += entry.mQuotedFee;
            land_impact += llmax(entry.mLandImpact, 0.f);
        }
        else
        {
            ++not_quoted;
        }
    }
    refreshList();
    if (count == 0)
    {
        stopRun("Nothing is ready to upload (no model has a current fee quote).");
        return;
    }

    LLSD args;
    args["COUNT"] = count;
    args["TOTAL"] = total;
    args["LI"] = llformat("%.1f", land_impact);
    args["BALANCE"] = gStatusBar ? llformat("%d", gStatusBar->getBalance()) : std::string("?");
    args["NOT_QUOTED"] = not_quoted > 0
        ? llformat("\n\n%d model(s) could not be quoted and are left out; see the batch log.", not_quoted)
        : std::string();
    log(llformat("Asking to confirm %d uploads, L$%d in total.", count, total));

    LLHandle<LLFloater> handle = getHandle();
    LLNotificationsUtil::add("MeshBulkUploadConfirm", args, LLSD(),
                             [handle](const LLSD& notification, const LLSD& response)
                             {
                                 if (LLFloaterMeshBulkUpload* self = dynamic_cast<LLFloaterMeshBulkUpload*>(handle.get()))
                                 {
                                     self->onConfirmUploadAll(notification, response);
                                 }
                             });
}

void LLFloaterMeshBulkUpload::onConfirmUploadAll(const LLSD& notification, const LLSD& response)
{
    if (LLNotificationsUtil::getSelectedOption(notification, response) != 0)
    {
        stopRun("Upload cancelled; nothing was charged.");
        return;
    }
    if (isBusy())
    {
        log("The batch became busy before the confirmation; press Upload all again.");
        return;
    }

    S32 count = 0;
    S32 total = 0;
    for (Entry& entry : mEntries)
    {
        entry.mConfirmedFee = -1;
        if (isRemaining(entry) && entry.mStatus == ST_READY && entry.mQuotedFee >= 0 && entry.mQuoteGen == mCarriedGen)
        {
            entry.mConfirmedFee = entry.mQuotedFee;
            ++count;
            total += entry.mQuotedFee;
        }
    }
    log(llformat("Confirmed: uploading %d models, L$%d in total. Each model is fee-checked again just before it uploads.", count, total));
    mPaused = false;
    mRunMode = RUN_UPLOAD_ALL;
    ensureFolder();
    advance();
}

void LLFloaterMeshBulkUpload::onPause()
{
    if (mPaused)
    {
        mPaused = false;
        log("Resumed.");
        if (mStep == STEP_IDLE && mRunMode != RUN_MANUAL)
        {
            advance();
        }
        else if (mStep == STEP_IDLE && mCurrent >= 0 && mEntries[mCurrent].mStatus == ST_UPLOADED)
        {
            advance();
        }
    }
    else
    {
        mPaused = true;
        log("Pausing after the current step (an upload already sent always finishes).");
    }
    refreshButtons();
}

void LLFloaterMeshBulkUpload::onCancelBatch()
{
    mPaused = false;
    if (mStep != STEP_WAIT_UPLOAD)
    {
        if (mCurrent >= 0 && mCurrent < (S32)mEntries.size())
        {
            Entry& entry = mEntries[mCurrent];
            if (entry.mStatus == ST_LOADING || entry.mStatus == ST_QUOTING)
            {
                entry.mStatus = ST_PENDING;
            }
        }
        mStep = STEP_IDLE;
        mCurrent = -1;
        mCompanionQueue.clear();
        if (LLFloaterModelPreview* fmp = getPreview(false))
        {
            if (fmp->isBulkMode())
            {
                fmp->setBulkMode(false);
                fmp->closeFloater(false);
            }
        }
    }
    else
    {
        log("An upload is in progress; it finishes and is reported, then the batch stops.");
        mPaused = true;
    }
    mRunMode = RUN_MANUAL;
    report("Batch stopped");
    refreshList();
}

void LLFloaterMeshBulkUpload::ensureFolder()
{
    if (mFolderResolved || mCreatingFolder)
    {
        return;
    }
    if (!mNewFolderCheck->get())
    {
        mDestFolder = mParentFolder;
        mFolderResolved = true;
        refreshButtons();
        return;
    }

    std::string name = getChild<LLLineEditor>("folder_name_editor")->getText();
    LLStringUtil::trim(name);
    if (name.empty())
    {
        name = "Bulk upload";
    }
    const LLUUID parent = mParentFolder.notNull()
        ? mParentFolder
        : gInventory.findUserDefinedCategoryUUIDForType(LLFolderType::FT_OBJECT);

    mCreatingFolder = true;
    log("Creating inventory folder \"" + name + "\" for the uploads...");
    LLHandle<LLFloater> handle = getHandle();
    gInventory.createNewCategory(parent, LLFolderType::FT_NONE, name,
        [handle, name](const LLUUID& new_id)
        {
            LLFloaterMeshBulkUpload* self = dynamic_cast<LLFloaterMeshBulkUpload*>(handle.get());
            if (!self)
            {
                return;
            }
            self->mCreatingFolder = false;
            self->mFolderResolved = true;
            if (new_id.isNull())
            {
                // Not retried: uploads go to the parent (or the default Objects folder) instead.
                self->mDestFolder = self->mParentFolder;
                self->log("Could not create the folder \"" + name + "\"; uploads go to "
                          + (self->mParentFolder.isNull() ? std::string("the default Objects folder") : std::string("the chosen folder")) + ".");
            }
            else
            {
                self->mDestFolder = new_id;
                self->log("Uploads go to the new folder \"" + name + "\".");
            }
            if (self->mUploadAfterFolder)
            {
                self->mUploadAfterFolder = false;
                // Next frame, and only if the held model is still the one in the uploader.
                const S32 held = self->mCurrent;
                doOnIdleOneTime([handle, held]()
                                {
                                    LLFloaterMeshBulkUpload* batch = dynamic_cast<LLFloaterMeshBulkUpload*>(handle.get());
                                    if (!batch || batch->mCurrent != held || held < 0 || held >= (S32)batch->mEntries.size()
                                        || batch->mEntries[held].mStatus == ST_UPLOADING)
                                    {
                                        return;
                                    }
                                    LLFloaterModelPreview* fmp = batch->getPreview(false);
                                    if (fmp && fmp->isBulkMode() && fmp->isModelUploadAllowed())
                                    {
                                        fmp->uploadNow();
                                    }
                                    else
                                    {
                                        batch->log("The folder is ready; press Upload in the uploader.");
                                    }
                                });
            }
            self->refreshButtons();
        });
    refreshButtons();
}

//-----------------------------------------------------------------------------
// Uploader callbacks
//-----------------------------------------------------------------------------

//static
bool LLFloaterMeshBulkUpload::onPreviewUploadRequested(LLFloaterModelPreview* fmp)
{
    LLFloaterMeshBulkUpload* self = findBatch();
    if (!self || self->mCurrent < 0 || self->mCurrent >= (S32)self->mEntries.size())
    {
        return true; // not a batch model any more: plain upload
    }
    if (!self->mFolderResolved && !self->mNewFolderCheck->get())
    {
        self->ensureFolder(); // no new folder: resolves right away, nothing is requested
    }
    if (!self->mFolderResolved)
    {
        // Hold this upload until the folder exists; the folder callback calls uploadNow()
        // again on a later frame. Always false here, so the upload is never sent twice.
        self->mUploadAfterFolder = true;
        self->ensureFolder();
        return false;
    }
    fmp->setUploadDestination(self->mDestFolder);

    Entry& entry = self->mEntries[self->mCurrent];
    if (entry.mStatus == ST_UPLOADED || entry.mStatus == ST_FAILED_UNKNOWN)
    {
        self->log(entry.mGroup.mName + ": uploading again as requested (it may now exist twice).");
    }
    if (self->mCarryCheck->get())
    {
        self->setCarriedSettings(self->captureForCarry(fmp));
    }
    entry.mQuotedFee = fmp->getQuotedFee();
    entry.mLandImpact = fmp->getQuotedLandImpact();
    entry.mQuoteGen = self->mCarriedGen;
    entry.mStatus = ST_UPLOADING;
    entry.mMessage.clear();
    self->mStep = STEP_WAIT_UPLOAD;
    self->log(llformat("Uploading %s (quoted L$%d)", self->entryLabel(self->mCurrent).c_str(), entry.mQuotedFee));
    self->refreshList();
    return true;
}

//static
void LLFloaterMeshBulkUpload::onPreviewFeeReceived(LLFloaterModelPreview* fmp)
{
    LLFloaterMeshBulkUpload* self = findBatch();
    if (!self || self->mCurrent < 0 || self->mCurrent >= (S32)self->mEntries.size())
    {
        return;
    }
    Entry& entry = self->mEntries[self->mCurrent];
    if (entry.mStatus == ST_UPLOADING || entry.mStatus == ST_UPLOADED)
    {
        return;
    }
    entry.mQuotedFee = fmp->getQuotedFee();
    entry.mLandImpact = fmp->getQuotedLandImpact();
    entry.mQuoteGen = self->mCarriedGen;
    entry.mStatus = ST_READY;
    entry.mMessage.clear();
    self->log(llformat("%s: fee L$%d, land impact %.1f", entry.mGroup.mName.c_str(), entry.mQuotedFee, entry.mLandImpact));

    if (self->mStep != STEP_WAIT_FEE)
    {
        self->refreshList(); // the user pressed Calculate in the uploader
        return;
    }

    switch (self->mRunMode)
    {
    case RUN_QUOTE_ALL:
        self->advance();
        return;

    case RUN_UPLOAD_ALL:
        self->mStep = STEP_WAIT_USER;
        if (self->mPaused)
        {
            self->stopRun("Paused before uploading " + entry.mGroup.mName + "; press Upload in the uploader or Resume.");
            break;
        }
        if (entry.mConfirmedFee < 0 || entry.mQuotedFee < 0 || entry.mQuotedFee > entry.mConfirmedFee)
        {
            self->stopRun(llformat("%s now costs L$%d, more than the L$%d you confirmed. Stopped; press Upload in the uploader "
                                   "to accept the new fee, or Upload all remaining to confirm again.",
                                   entry.mGroup.mName.c_str(), entry.mQuotedFee, entry.mConfirmedFee));
            break;
        }
        if (!fmp->isModelUploadAllowed())
        {
            self->stopRun("The uploader does not allow uploading " + entry.mGroup.mName + " (upload permission or a model problem). Stopped.");
            break;
        }
        fmp->uploadNow(); // onPreviewUploadRequested() records it
        return;

    case RUN_MANUAL:
    default:
        self->mStep = STEP_WAIT_USER;
        self->log("Ready: press Upload in the uploader to upload " + entry.mGroup.mName + ", or Upload all remaining.");
        break;
    }
    self->refreshList();
}

//static
void LLFloaterMeshBulkUpload::onPreviewFeeError(LLFloaterModelPreview* fmp, S32 status, const std::string& reason)
{
    LLFloaterMeshBulkUpload* self = findBatch();
    if (!self || self->mCurrent < 0 || self->mCurrent >= (S32)self->mEntries.size())
    {
        return;
    }
    Entry& entry = self->mEntries[self->mCurrent];
    if (entry.mStatus == ST_UPLOADING || entry.mStatus == ST_UPLOADED)
    {
        return;
    }
    entry.mStatus = ST_QUOTE_FAILED;
    entry.mQuotedFee = -1;
    entry.mMessage = llformat("%d ", status) + reason;
    self->log(entry.mGroup.mName + ": fee request failed (" + entry.mMessage + ")");
    if (self->mStep != STEP_WAIT_FEE)
    {
        self->refreshList();
        return;
    }
    if (self->mRunMode == RUN_QUOTE_ALL)
    {
        self->log(entry.mGroup.mName + " is left out of this run.");
        self->advance();
        return;
    }
    self->mStep = STEP_WAIT_USER;
    if (self->mRunMode == RUN_UPLOAD_ALL)
    {
        self->stopRun("Stopped: no fee for " + entry.mGroup.mName + ". Nothing was charged for it.");
    }
    self->refreshList();
}

//static
void LLFloaterMeshBulkUpload::onPreviewUploadDone(LLFloaterModelPreview* fmp, bool success, bool charge_unknown, const std::string& reason)
{
    LLFloaterMeshBulkUpload* self = findBatch();
    if (!self)
    {
        return;
    }
    S32 index = -1;
    for (S32 i = 0; i < (S32)self->mEntries.size(); ++i)
    {
        if (self->mEntries[i].mStatus == ST_UPLOADING)
        {
            index = i;
            break;
        }
    }
    if (index < 0)
    {
        return;
    }
    Entry& entry = self->mEntries[index];
    if (index == self->mCurrent && self->mStep == STEP_WAIT_UPLOAD)
    {
        self->mStep = STEP_IDLE;
    }

    if (success)
    {
        entry.mStatus = ST_UPLOADED;
        entry.mChargedFee = entry.mQuotedFee;
        self->log(llformat("Uploaded %s, charged L$%d (its quoted fee).", entry.mGroup.mName.c_str(), entry.mChargedFee));
        if (self->mStep == STEP_IDLE && index == self->mCurrent && fmp->isBulkMode())
        {
            // Manual mode loads the next model with the carried settings so Upload can be pressed
            // again; "upload all" carries on; a pause stops here.
            self->advance();
            return;
        }
    }
    else
    {
        entry.mStatus = charge_unknown ? ST_FAILED_UNKNOWN : ST_FAILED;
        entry.mMessage = reason;
        self->log(entry.mGroup.mName + (charge_unknown
            ? ": upload failed at the network level (" + reason + "). It may or may not have been created and charged: "
              "check your inventory and L$ history before uploading it again. Not retried."
            : ": the server did not accept the upload (" + reason + "). Nothing was created or charged."));
        if (index == self->mCurrent && fmp->isBulkMode())
        {
            self->mStep = STEP_WAIT_USER; // the uploader stays on this model; uploading again is the user's call
        }
        self->mRunMode = RUN_MANUAL;
        self->report("Batch stopped after a failed upload");
    }
    self->refreshList();
}

//static
void LLFloaterMeshBulkUpload::onPreviewClosed(LLFloaterModelPreview* fmp)
{
    LLFloaterMeshBulkUpload* self = findBatch();
    fmp->setBulkMode(false);
    if (!self)
    {
        return;
    }
    if (self->mStep == STEP_WAIT_UPLOAD)
    {
        self->mPaused = true;
        self->stopRun("Uploader closed while uploading; the result is still reported here. Paused.");
        return;
    }
    if (self->mCurrent >= 0 && self->mCurrent < (S32)self->mEntries.size())
    {
        Entry& entry = self->mEntries[self->mCurrent];
        if (entry.mStatus == ST_LOADING || entry.mStatus == ST_QUOTING)
        {
            entry.mStatus = ST_PENDING;
        }
    }
    const bool was_running = self->mRunMode != RUN_MANUAL || self->isBusy();
    self->mStep = STEP_IDLE;
    self->mCompanionQueue.clear();
    self->mCurrent = -1;
    self->stopRun(was_running ? "Uploader closed; batch stopped. Load a model or Upload all remaining to continue." : std::string());
    self->refreshList();
}
