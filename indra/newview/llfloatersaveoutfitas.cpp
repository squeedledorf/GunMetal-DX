/**
 * @file llfloatersaveoutfitas.cpp
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

#include "llviewerprecompiledheaders.h"

#include "llfloatersaveoutfitas.h"

#include "llappearancemgr.h"
#include "llbutton.h"
#include "llcombobox.h"
#include "llfloaterreg.h"
#include "lllineeditor.h"
#include "lloutfitslist.h"
#include "llpaneloutfitsinventory.h"
#include "lltextvalidate.h"
#include "llviewerfoldertype.h"

LLFloaterSaveOutfitAs::LLFloaterSaveOutfitAs(const LLSD& key)
    : LLModalDialog(key)
{
}

// static
void LLFloaterSaveOutfitAs::show(const LLUUID& folder_id)
{
    LLFloaterSaveOutfitAs* floater = LLFloaterReg::getTypedInstance<LLFloaterSaveOutfitAs>("save_outfit_as");
    if (floater)
    {
        floater->mFolderID = folder_id;
        floater->openFloater();
    }
}

bool LLFloaterSaveOutfitAs::postBuild()
{
    mNameEditor = getChild<LLLineEditor>("name ed");
    mNameEditor->setPrevalidate(&LLTextValidate::validateASCII);
    mNameEditor->setKeystrokeCallback(boost::bind(&LLFloaterSaveOutfitAs::onNameEdited, this), NULL);

    mFolderCombo = getChild<LLComboBox>("folder_combo");

    mSaveButton = getChild<LLButton>("Save");
    mSaveButton->setCommitCallback(boost::bind(&LLFloaterSaveOutfitAs::onSave, this));
    getChild<LLButton>("Cancel")->setCommitCallback(boost::bind(&LLFloaterSaveOutfitAs::onCancel, this));

    return LLModalDialog::postBuild();
}

void LLFloaterSaveOutfitAs::onOpen(const LLSD& key)
{
    LLModalDialog::onOpen(key);

    std::string outfit_name;
    if (!LLAppearanceMgr::getInstance()->getBaseOutfitName(outfit_name))
    {
        outfit_name = LLViewerFolderType::lookupNewCategoryName(LLFolderType::FT_OUTFIT);
    }
    LLStringUtil::format_map_t args;
    args["[DESC]"] = outfit_name;
    mNameEditor->setText(getString("default_name", args));
    mNameEditor->selectAll();
    mNameEditor->setFocus(true);

    // Root first, then the folder tree as paths.
    mFolderCombo->removeall();
    std::vector<LLOutfitFolderEntry> folders;
    LLOutfitListBase::getFolderTree(folders);
    for (const LLOutfitFolderEntry& entry : folders)
    {
        mFolderCombo->add(entry.mDepth == 0 ? getString("root_folder") : entry.mPath, LLSD(entry.mID));
    }
    if (!LLOutfitListBase::isTreeFolder(mFolderID) || !mFolderCombo->selectByValue(LLSD(mFolderID)))
    {
        mFolderCombo->selectFirstItem();
    }

    onNameEdited();
}

void LLFloaterSaveOutfitAs::onNameEdited()
{
    std::string name = mNameEditor->getText();
    LLStringUtil::trim(name);
    mSaveButton->setEnabled(!name.empty());
}

void LLFloaterSaveOutfitAs::onSave()
{
    std::string outfit_name = mNameEditor->getText();
    LLStringUtil::trim(outfit_name);
    if (outfit_name.empty())
    {
        return;
    }

    const LLUUID folder_id = mFolderCombo->getValue().asUUID();
    LLPanelOutfitsInventory* panel = LLPanelOutfitsInventory::findInstance();
    if (panel)
    {
        panel->saveOutfitAs(outfit_name, folder_id);
    }
    else
    {
        LLAppearanceMgr::getInstance()->makeNewOutfitLinks(outfit_name, true, folder_id);
    }
    closeFloater();
}

void LLFloaterSaveOutfitAs::onCancel()
{
    closeFloater();
}
