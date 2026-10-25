/**
 * @file llfloatersaveoutfitas.h
 * @brief Saves the current outfit as a new outfit in a chosen My Outfits folder
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

#ifndef LL_LLFLOATERSAVEOUTFITAS_H
#define LL_LLFLOATERSAVEOUTFITAS_H

#include "llmodaldialog.h"

class LLButton;
class LLComboBox;
class LLLineEditor;

class LLFloaterSaveOutfitAs : public LLModalDialog
{
public:
    LLFloaterSaveOutfitAs(const LLSD& key);

    bool postBuild() override;
    void onOpen(const LLSD& key) override;

    // Opens the dialog with folder_id (a My Outfits folder, or null for the root) preselected.
    static void show(const LLUUID& folder_id);

private:
    void onSave();
    void onCancel();
    void onNameEdited();

    LLLineEditor*   mNameEditor = nullptr;
    LLComboBox*     mFolderCombo = nullptr;
    LLButton*       mSaveButton = nullptr;
    LLUUID          mFolderID;
};

#endif // LL_LLFLOATERSAVEOUTFITAS_H
