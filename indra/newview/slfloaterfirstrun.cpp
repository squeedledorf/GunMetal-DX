/**
 * @file slfloaterfirstrun.cpp
 * @brief Welcome window (#132). See the header.
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

#include "slfloaterfirstrun.h"

#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llfloaterreg.h"
#include "llnotificationsutil.h"
#include "lltextbox.h"
#include "llviewercontrol.h"
#include "llweb.h"
#include "slfloaterviewerimport.h"
#include "slviewerpresets.h"

#include <algorithm>

namespace
{
const char* const HELP_URL = "https://slipstreamviewer.org/wiki/";

bool loggedIn()
{
    // Same test as the importer: per-account data (chat logs) exists once we know the account.
    return !gDirUtilp->getLindenUserDir().empty();
}

void saveSettings()
{
    gSavedSettings.saveToFile(gSavedSettings.getString("ClientSettingsFile"), true);
}
}

SLFloaterFirstRun::SLFloaterFirstRun(const LLSD& key)
:   LLFloater(key)
{
}

bool SLFloaterFirstRun::postBuild()
{
    mDescription = getChild<LLTextBox>("description");
    mImportText  = getChild<LLTextBox>("import_text");
    mImportCheck = getChild<LLCheckBoxCtrl>("import_check");
    mContinueBtn = getChild<LLButton>("continue_btn");
    mUndoBtn     = getChild<LLButton>("undo_btn");

    for (const std::string& id : slpresets::presetIds())
    {
        getChild<LLUICtrl>("choice_" + id)->setCommitCallback([this, id](LLUICtrl*, const LLSD&) { onChoose(id); });
    }
    getChild<LLUICtrl>("help_btn")->setCommitCallback([](LLUICtrl*, const LLSD&) { LLWeb::loadURLExternal(std::string(HELP_URL)); });
    getChild<LLUICtrl>("skip_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { closeFloater(); });
    mContinueBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onContinue(); });
    mUndoBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onUndo(); });
    return true;
}

void SLFloaterFirstRun::onOpen(const LLSD& key)
{
    // Look for other viewers' data once per opening (it reads a few directories).
    mFound.clear();
    for (const slimport::SourceViewer& viewer : SLFloaterViewerImport::findInstalledViewers())
    {
        mFound[viewer.id] = true;
    }
    // Re-opened from the menu: start from what was picked last time.
    mChoice = gSavedSettings.getString("FirstRunViewerChoice");
    const auto& ids = slpresets::presetIds();
    if (std::find(ids.begin(), ids.end(), mChoice) == ids.end()) mChoice.clear();
    refresh();
}

std::string SLFloaterFirstRun::viewerName(const std::string& id) const
{
    return getString("name_" + id);
}

bool SLFloaterFirstRun::importOffered() const
{
    if (mChoice.empty() || mChoice == slpresets::defaultsId()) return false;
    const auto it = mFound.find(mChoice);
    return it != mFound.end() && it->second;
}

void SLFloaterFirstRun::onChoose(const std::string& id)
{
    mChoice = id;
    refresh();
}

void SLFloaterFirstRun::refresh()
{
    for (const std::string& id : slpresets::presetIds())
    {
        getChild<LLButton>("choice_" + id)->setToggleState(id == mChoice);
    }
    mDescription->setText(getString(mChoice.empty() ? std::string("desc_none") : "desc_" + mChoice));

    const bool offer = importOffered();
    if (offer)
    {
        LLStringUtil::format_map_t args;
        args["[VIEWER]"] = viewerName(mChoice);
        mImportText->setText(getString(loggedIn() ? "import_found" : "import_found_login", args));
    }
    mImportText->setVisible(offer);
    mImportCheck->setVisible(offer);

    mContinueBtn->setEnabled(!mChoice.empty());
    mUndoBtn->setVisible(slpresets::hasBackup());
}

void SLFloaterFirstRun::onContinue()
{
    if (mChoice.empty()) return;
    const std::string choice = mChoice;
    const bool import = importOffered() && mImportCheck->get();

    slpresets::Result result;
    if (!slpresets::apply(choice, result))
    {
        LL_WARNS("ViewerPresets") << "Could not apply viewer preset " << choice << LL_ENDL;
    }

    LLStringUtil::format_map_t what_args;
    what_args["[VIEWER]"] = viewerName(choice);
    LLSD args;
    args["WHAT"] = (choice == slpresets::defaultsId()) ? getString("applied_defaults") : getString("applied_viewer", what_args);
    args["RESTART"] = result.restart.empty() ? std::string() : " " + getString("restart_note");

    if (import)
    {
        // Settings, key bindings and the rest of the global data can be imported right away;
        // chat logs are per account, so before login we offer them again after the next login.
        if (!loggedIn())
        {
            gSavedSettings.setString("FirstRunImportPending", choice);
            saveSettings();
        }
        SLFloaterViewerImport::showForViewer(choice);
    }
    closeFloater();
    LLNotificationsUtil::add("FirstRunPresetApplied", args);
}

void SLFloaterFirstRun::onUndo()
{
    if (!slpresets::hasBackup())
    {
        refresh();
        return;
    }
    LLHandle<LLFloater> handle = getHandle();
    LLNotificationsUtil::add("FirstRunConfirmUndo", LLSD(), LLSD(), [handle](const LLSD& notification, const LLSD& response)
    {
        if (LLNotificationsUtil::getSelectedOption(notification, response) != 0) return false;
        slpresets::Result result;
        slpresets::undo(result);
        if (SLFloaterFirstRun* self = static_cast<SLFloaterFirstRun*>(handle.get()))
        {
            self->mChoice.clear();
            self->refresh();
        }
        return false;
    });
}

// static
void SLFloaterFirstRun::showIfFirstRun()
{
    if (!gSavedSettings.getBOOL("FirstLoginThisInstall") || gSavedSettings.getBOOL("FirstRunChooserShown"))
    {
        return;
    }
    // Marked when shown, not on Continue: the login screen can come back (failed login), and
    // the window links the help pages, so the #420 "New to Slipstream?" notice is not needed.
    gSavedSettings.setBOOL("FirstRunChooserShown", true);
    gSavedSettings.setBOOL("HelpWelcomeShown", true);
    saveSettings();
    LLFloaterReg::showInstance("sl_first_run");
}

// static
void SLFloaterFirstRun::offerPendingImport()
{
    const std::string viewer_id = gSavedSettings.getString("FirstRunImportPending");
    if (viewer_id.empty() || !loggedIn()) return;
    gSavedSettings.setString("FirstRunImportPending", std::string());

    std::string label;
    for (const slimport::SourceViewer& viewer : SLFloaterViewerImport::findInstalledViewers())
    {
        if (viewer.id == viewer_id)
        {
            label = viewer.label;
            break;
        }
    }
    if (label.empty()) return;     // their data is gone: nothing to offer

    LLSD args;
    args["VIEWER"] = label;
    LLNotificationsUtil::add("FirstRunImportChat", args, LLSD(), [viewer_id](const LLSD& notification, const LLSD& response)
    {
        if (LLNotificationsUtil::getSelectedOption(notification, response) == 0)
        {
            SLFloaterViewerImport::showForViewer(viewer_id);
        }
        return false;
    });
}
