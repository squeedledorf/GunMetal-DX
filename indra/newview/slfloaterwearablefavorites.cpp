/**
 * @file slfloaterwearablefavorites.cpp
 * @brief Favorite Wearables (#1132): the window that lists the favorites and wears them.
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

#include "slfloaterwearablefavorites.h"

#include "llbutton.h"
#include "llclipboard.h"
#include "llcombobox.h"
#include "llfiltereditor.h"
#include "llfolderview.h"
#include "llfolderviewmodelinventory.h"
#include "llinventorybridge.h"
#include "llinventoryfunctions.h"
#include "llinventorymodel.h"
#include "llinventorymodelbackgroundfetch.h"
#include "lltextbox.h"
#include "lltrans.h"
#include "lluictrlfactory.h"
#include "llviewercontrol.h"
#include "llviewerinventory.h"
#include "slwearablefavorites.h"
#include "rlvactions.h"
#include "rlvdefines.h"

namespace
{
    const char* const SORT_SETTING = "FavoriteWearablesSortOrder";

    // Wearables, body parts and attachments: what a favorite can be. An item dragged out of an
    // outfit folder is a link (DAD_LINK); canFavorite() resolves it to its original.
    bool is_favorite_cargo(EDragAndDropType cargo_type)
    {
        return cargo_type == DAD_CLOTHING || cargo_type == DAD_BODYPART || cargo_type == DAD_OBJECT
            || cargo_type == DAD_LINK;
    }

    // The root (and any subfolder) of the favorites folder inside the list. Nothing may be moved
    // or copied into it by the generic folder code, so "paste" and drops become "add favorite
    // links", which only ever creates links to the originals.
    class SLWearableFavoritesFolderBridge : public LLFolderBridge
    {
    public:
        SLWearableFavoritesFolderBridge(LLInventoryType::EType type,
                                        LLInventoryPanel* inventory,
                                        LLFolderView* root,
                                        const LLUUID& uuid)
            : LLFolderBridge(inventory, root, uuid)
        {
            mInvType = type;
        }

        void pasteFromClipboard() override { pasteLinkFromClipboard(); }

        void pasteLinkFromClipboard() override
        {
            std::vector<LLUUID> objects;
            LLClipboard::instance().pasteFromClipboard(objects);
            // Cut mode is left alone: favorites are links, the cut items stay where they are
            // until they are pasted somewhere that moves them.
            SLWearableFavorites::add(objects);
        }

        bool isClipboardPasteable() const override { return isClipboardPasteableAsLink(); }

        bool isClipboardPasteableAsLink() const override
        {
            std::vector<LLUUID> objects;
            LLClipboard::instance().pasteFromClipboard(objects);
            for (const LLUUID& id : objects)
            {
                if (SLWearableFavorites::canFavorite(id) && !SLWearableFavorites::isFavorite(id))
                {
                    return true;
                }
            }
            return false;
        }

        bool dragOrDrop(MASK, bool, EDragAndDropType, void*, std::string&) override
        {
            return false; // the window handles drops (links); the folder code would move originals
        }
    };

    class SLWearableFavoritesBridgeBuilder : public LLInventoryFolderViewModelBuilder
    {
    public:
        LLInvFVBridge* createBridge(LLAssetType::EType asset_type,
                                    LLAssetType::EType actual_asset_type,
                                    LLInventoryType::EType inv_type,
                                    LLInventoryPanel* inventory,
                                    LLFolderViewModelInventory* view_model,
                                    LLFolderView* root,
                                    const LLUUID& uuid,
                                    U32 flags = 0x00) const override
        {
            if (asset_type == LLAssetType::AT_CATEGORY && actual_asset_type != LLAssetType::AT_LINK_FOLDER)
            {
                return new SLWearableFavoritesFolderBridge(inv_type, inventory, root, uuid);
            }
            return LLInventoryFolderViewModelBuilder::createBridge(asset_type, actual_asset_type, inv_type,
                                                                   inventory, view_model, root, uuid, flags);
        }
    };

    const SLWearableFavoritesBridgeBuilder FAVORITES_BRIDGE_BUILDER;

    LLDefaultChildRegistry::Register<SLWearableFavoritesPanel> t_wearable_favorites_panel("wearable_favorites_panel");
}

//=============================================================================
// SLWearableFavoritesPanel
//=============================================================================

SLWearableFavoritesPanel::SLWearableFavoritesPanel(const Params& p)
    : LLInventoryPanel(p)
{
    mInvFVBridgeBuilder = &FAVORITES_BRIDGE_BUILDER;
}

void SLWearableFavoritesPanel::initFromParams(const Params& p)
{
    // No folder root yet: the favorites folder is found (or created) when the window opens.
    mParams = p;
    LLPanel::initFromParams(mParams);
}

void SLWearableFavoritesPanel::onVisibilityChange(bool new_visibility)
{
    if (!mRootInited)
    {
        LLPanel::onVisibilityChange(new_visibility);
        return;
    }
    LLInventoryPanel::onVisibilityChange(new_visibility);
}

void SLWearableFavoritesPanel::draw()
{
    if (!mRootInited)
    {
        LLPanel::draw();
        return;
    }
    LLInventoryPanel::draw();
}

bool SLWearableFavoritesPanel::handleDragAndDrop(S32, S32, MASK, bool, EDragAndDropType, void*,
                                                 EAcceptance*, std::string&)
{
    return false;
}

void SLWearableFavoritesPanel::setFavoritesFolder(const LLUUID& folder_id)
{
    if (mRootInited || folder_id.isNull())
    {
        return;
    }
    mParams.start_folder.id = folder_id;
    initFolderRoot();
}

//=============================================================================
// SLFloaterWearableFavorites
//=============================================================================

SLFloaterWearableFavorites::SLFloaterWearableFavorites(const LLSD& key)
    : LLFloater(key)
{
}

SLFloaterWearableFavorites::~SLFloaterWearableFavorites()
{
}

bool SLFloaterWearableFavorites::postBuild()
{
    mPanel = getChild<SLWearableFavoritesPanel>("favorites_panel");
    mFilter = getChild<LLFilterEditor>("filter_editor");
    mSortCombo = getChild<LLComboBox>("sort_combo");
    mWearBtn = getChild<LLButton>("wear_btn");
    mAddBtn = getChild<LLButton>("add_btn");
    mTakeOffBtn = getChild<LLButton>("take_off_btn");
    mStatusText = getChild<LLTextBox>("status_text");

    mFilter->setCommitCallback([this](LLUICtrl*, const LLSD&) { onFilterChanged(); });
    mSortCombo->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSortChanged(); });
    mWearBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { doAction("wear"); });
    mAddBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { doAction("wear_add"); });
    // One entry for clothing and attachments, as in the Wearing tab: only worn items are affected
    mTakeOffBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { doAction("take_off"); });

    const std::string saved = std::to_string(gSavedSettings.getU32(SORT_SETTING));
    if (!mSortCombo->selectByValue(LLSD(saved)))
    {
        mSortCombo->selectByValue(LLSD(std::string("2")));
    }
    return true;
}

void SLFloaterWearableFavorites::onOpen(const LLSD& key)
{
    // The window lists inventory items: closed while @showinv is in force
    if (RlvActions::hasBehaviour(RLV_BHVR_SHOWINV))
    {
        closeFloater();
        return;
    }
    attachFolder(SLWearableFavorites::getFolderID());
}

void SLFloaterWearableFavorites::attachFolder(const LLUUID& folder_id)
{
    if (mPanel->hasFolder() || folder_id.isNull())
    {
        return;
    }
    // The folder's contents are not necessarily loaded yet; the panel follows the model.
    LLInventoryModelBackgroundFetch::instance().start(folder_id, true);
    mPanel->setFavoritesFolder(folder_id);
    applySortOrder();
    mPanel->setFilterSubString(mFilter->getText());
}

void SLFloaterWearableFavorites::draw()
{
    if (!mPanel->hasFolder())
    {
        // Created later by "Add to Favorite Wearables" (or by another viewer): pick it up.
        attachFolder(SLWearableFavorites::getFolderID());
    }
    updateButtons();
    LLFloater::draw();
}

void SLFloaterWearableFavorites::onFilterChanged()
{
    mPanel->setFilterSubString(mFilter->getText());
}

void SLFloaterWearableFavorites::onSortChanged()
{
    gSavedSettings.setU32(SORT_SETTING, (U32)mSortCombo->getSelectedValue().asInteger());
    applySortOrder();
}

void SLFloaterWearableFavorites::applySortOrder()
{
    if (mPanel->hasFolder())
    {
        mPanel->setSortOrder(gSavedSettings.getU32(SORT_SETTING));
    }
}

void SLFloaterWearableFavorites::doAction(const char* action)
{
    if (mPanel->hasFolder() && mPanel->getRootFolder())
    {
        // Acts on the favorites' selection, resolved to their originals by the item bridges
        mPanel->doToSelected(LLSD(std::string(action)));
    }
}

void SLFloaterWearableFavorites::updateButtons()
{
    bool can_wear = false;
    bool can_add = false;
    bool can_take_off = false;
    bool has_rows = false;

    LLFolderView* root = mPanel->hasFolder() ? mPanel->getRootFolder() : nullptr;
    if (root)
    {
        has_rows = mPanel->areViewsInitialized() && root->hasVisibleChildren();
        for (LLFolderViewItem* selected : root->getSelectionList())
        {
            const LLFolderViewModelItemInventory* model = selected
                ? dynamic_cast<const LLFolderViewModelItemInventory*>(selected->getViewModelItem()) : nullptr;
            const LLViewerInventoryItem* item = model ? gInventory.getItem(model->getUUID()) : nullptr;
            if (!item)
            {
                continue;
            }
            const LLAssetType::EType type = item->getType(); // follows links
            if (type != LLAssetType::AT_CLOTHING && type != LLAssetType::AT_BODYPART && type != LLAssetType::AT_OBJECT)
            {
                continue;
            }
            if (get_is_item_worn(item->getUUID()))
            {
                can_take_off |= (type != LLAssetType::AT_BODYPART);
            }
            else
            {
                can_wear = true;
                can_add |= (type != LLAssetType::AT_BODYPART);
            }
        }
    }
    mWearBtn->setEnabled(can_wear);
    mAddBtn->setEnabled(can_add);
    mTakeOffBtn->setEnabled(can_take_off);

    // Say so when there is nothing to show (not while the rows are still being built)
    const bool views_ready = !root || mPanel->areViewsInitialized();
    mStatusText->setVisible(!has_rows && views_ready);
    if (!has_rows)
    {
        mStatusText->setText(getString(mFilter->getText().empty() ? "empty_text" : "no_match_text"));
    }
}

bool SLFloaterWearableFavorites::handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop,
                                                   EDragAndDropType cargo_type, void* cargo_data,
                                                   EAcceptance* accept, std::string& tooltip_msg)
{
    if (!is_favorite_cargo(cargo_type))
    {
        return LLFloater::handleDragAndDrop(x, y, mask, drop, cargo_type, cargo_data, accept, tooltip_msg);
    }

    // Dropping an item makes a favorite LINK to it. The item itself is never moved or copied.
    const LLInventoryItem* item = static_cast<const LLInventoryItem*>(cargo_data);
    if (!item || !SLWearableFavorites::canFavorite(item->getUUID()))
    {
        *accept = ACCEPT_NO;
        tooltip_msg = LLTrans::getString("FavoriteWearablesDropNo");
        return true;
    }
    if (SLWearableFavorites::isFavorite(item->getUUID()))
    {
        *accept = ACCEPT_NO;
        tooltip_msg = LLTrans::getString("FavoriteWearablesDropAlready");
        return true;
    }
    *accept = ACCEPT_YES_MULTI;
    tooltip_msg = LLTrans::getString("FavoriteWearablesDrop");
    if (drop)
    {
        uuid_vec_t ids;
        ids.push_back(item->getUUID());
        SLWearableFavorites::add(ids);
    }
    return true;
}
