/**
 * @file slfloaterfirstrun.h
 * @brief Welcome window (#132): "Which viewer did you use before?" Applies a viewer preset
 *        (slviewerpresets) and offers the importer (slfloaterviewerimport) when that viewer's
 *        data is on this computer. Shown once on a fresh install, at the login screen (or right
 *        after the first login when the login screen was skipped); replaces the #420 help
 *        notice. Help > Set up Slipstream like my old viewer opens it again.
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

#ifndef SL_FLOATERFIRSTRUN_H
#define SL_FLOATERFIRSTRUN_H

#include "llfloater.h"

#include <map>
#include <string>

class LLButton;
class LLCheckBoxCtrl;
class LLTextBox;

class SLFloaterFirstRun final : public LLFloater
{
public:
    SLFloaterFirstRun(const LLSD& key);
    bool postBuild() override;
    void onOpen(const LLSD& key) override;

    // A fresh install (FirstLoginThisInstall) that has not seen the window yet: open it, and
    // mark it and the #420 help notice as shown. People upgrading never see it.
    static void showIfFirstRun();
    // After login: offer the chat history import asked for at the login screen, once.
    static void offerPendingImport();

private:
    ~SLFloaterFirstRun() override = default;

    void onChoose(const std::string& id);
    void onContinue();
    void onUndo();
    void refresh();
    std::string viewerName(const std::string& id) const;
    bool importOffered() const;

    std::string                  mChoice;
    std::map<std::string, bool>  mFound;    // viewer id -> its data is on this computer
    LLTextBox*      mDescription = nullptr;
    LLTextBox*      mImportText = nullptr;
    LLCheckBoxCtrl* mImportCheck = nullptr;
    LLButton*       mContinueBtn = nullptr;
    LLButton*       mUndoBtn = nullptr;
};

#endif
