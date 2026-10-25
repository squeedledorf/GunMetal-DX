/**
 * @file lloutfitslist.cpp
 * @brief List of agent's outfits for My Appearance side panel.
 *
 * $LicenseInfo:firstyear=2010&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
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
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "lloutfitslist.h"

// llcommon
#include "llcommonutils.h"

#include "llaccordionctrl.h"
#include "llaccordionctrltab.h"
#include "llagentwearables.h"
#include "llaisapi.h"
#include "llappearancemgr.h"
#include "llappviewer.h"
#include "llfloaterreg.h"
#include "llfloatersaveoutfitas.h"
#include "llfloatersidepanelcontainer.h"
#include "llinspecttexture.h"
#include "llinventorymodelbackgroundfetch.h"
#include "llinventoryfunctions.h"
#include "llinventorymodel.h"
#include "llmenubutton.h"
#include "llnotificationsutil.h"
#include "lloutfitobserver.h"
#include "lltoggleablemenu.h"
#include "lltooldraganddrop.h"
#include "lltrans.h"
#include "lltextbox.h"
#include "llresmgr.h"
#include "lltransutil.h"
#include "llviewercontrol.h"
#include "llviewermenu.h"
#include "llvoavatar.h"
#include "llvoavatarself.h"
#include "llwearableitemslist.h"

static bool is_tab_header_clicked(LLAccordionCtrlTab* tab, S32 y);

static const LLOutfitTabNameComparator OUTFIT_TAB_NAME_COMPARATOR;
static const LLOutfitTabFavComparator OUTFIT_TAB_FAV_COMPARATOR;

static const S32 FOLDER_TREE_INDENT = 14;
static const std::string FOLDER_PATH_SEPARATOR(" > ");

// Folders of the My Outfits tree that are expanded in the list view, kept for the session.
static std::set<LLUUID> sExpandedOutfitFolders;

// Tabs carry their path from the My Outfits root, so a folder sorts right before its contents.
static bool compare_tree_tabs(const LLAccordionCtrlTab* tab1, const LLAccordionCtrlTab* tab2, bool favorites_first)
{
    const LLOutfitAccordionCtrlTab::sort_path_t& path1 = static_cast<const LLOutfitAccordionCtrlTab*>(tab1)->getSortPath();
    const LLOutfitAccordionCtrlTab::sort_path_t& path2 = static_cast<const LLOutfitAccordionCtrlTab*>(tab2)->getSortPath();
    const size_t count = llmin(path1.size(), path2.size());
    for (size_t i = 0; i < count; ++i)
    {
        const LLOutfitAccordionCtrlTab::SortKey& key1 = path1[i];
        const LLOutfitAccordionCtrlTab::SortKey& key2 = path2[i];
        if (key1.mID == key2.mID)
        {
            continue;
        }
        if (key1.mIsFolder != key2.mIsFolder)
        {
            return key1.mIsFolder;
        }
        if (favorites_first && key1.mIsFavorite != key2.mIsFavorite)
        {
            return key1.mIsFavorite;
        }
        S32 res = LLStringUtil::compareDict(key1.mName, key2.mName);
        if (res != 0)
        {
            return res < 0;
        }
        return key1.mID < key2.mID;
    }
    if (path1.size() != path2.size())
    {
        return path1.size() < path2.size();
    }
    return LLStringUtil::compareDict(tab1->getTitle(), tab2->getTitle()) < 0;
}

/*virtual*/
bool LLOutfitTabNameComparator::compare(const LLAccordionCtrlTab* tab1, const LLAccordionCtrlTab* tab2) const
{
    return compare_tree_tabs(tab1, tab2, false);
}

bool LLOutfitTabFavComparator::compare(const LLAccordionCtrlTab* tab1, const LLAccordionCtrlTab* tab2) const
{
    return compare_tree_tabs(tab1, tab2, true);
}

struct outfit_accordion_tab_params : public LLInitParam::Block<outfit_accordion_tab_params, LLOutfitAccordionCtrlTab::Params>
{
    Mandatory<LLWearableItemsList::Params> wearable_list;

    outfit_accordion_tab_params()
    :   wearable_list("wearable_items_list")
    {}
};

const outfit_accordion_tab_params& get_accordion_tab_params()
{
    static outfit_accordion_tab_params tab_params;
    static bool initialized = false;
    if (!initialized)
    {
        initialized = true;

        LLOutfitAccordionCtrlTab::sFavoriteIcon = LLUI::getUIImage("Inv_Favorite_Star_Full");
        LLOutfitAccordionCtrlTab::sFgColor = LLUIColorTable::instance().getColor("MenuItemEnabledColor", LLColor4U(255, 255, 255));

        LLXMLNodePtr xmlNode;
        if (LLUICtrlFactory::getLayeredXMLNode("outfit_accordion_tab.xml", xmlNode))
        {
            LLXUIParser parser;
            parser.readXUI(xmlNode, tab_params, "outfit_accordion_tab.xml");
        }
        else
        {
            LL_WARNS() << "Failed to read xml of Outfit's Accordion Tab from outfit_accordion_tab.xml" << LL_ENDL;
        }
    }

    return tab_params;
}


static LLPanelInjector<LLOutfitsList> t_outfits_list("outfits_list");

LLOutfitsList::LLOutfitsList()
    :   LLOutfitListBase()
    ,   mAccordion(NULL)
    ,   mListCommands(NULL)
    ,   mItemSelected(false)
    ,   mSortMenu(nullptr)
{
    LLControlVariable* ctrl = gSavedSettings.getControl("InventoryFavoritesColorText");
    if (ctrl)
    {
        mSavedSettingInvFavColor = ctrl->getSignal()->connect(boost::bind(&LLOutfitsList::handleInvFavColorChange, this));
    }
}

LLOutfitsList::~LLOutfitsList()
{
    delete mSortMenu;
    mSavedSettingInvFavColor.disconnect();
    mGearMenuConnection.disconnect();
}

bool LLOutfitsList::postBuild()
{
    mAccordion = getChild<LLAccordionCtrl>("outfits_accordion");
    mAccordion->setComparator(&OUTFIT_TAB_NAME_COMPARATOR);

    initComparator();

    return LLOutfitListBase::postBuild();
}

void LLOutfitsList::initComparator()
{
    S32 mode = gSavedSettings.getS32("OutfitListSortOrder");
    if (mode == 0)
    {
        mAccordion->setComparator(&OUTFIT_TAB_NAME_COMPARATOR);
    }
    else
    {
        mAccordion->setComparator(&OUTFIT_TAB_FAV_COMPARATOR);
    }
    sortOutfits();
}

//virtual
void LLOutfitsList::onOpen(const LLSD& info)
{
    if (!mIsInitialized)
    {
        // Start observing changes in Current Outfit category.
        LLOutfitObserver::instance().addCOFChangedCallback(boost::bind(&LLOutfitsList::onCOFChanged, this));
    }

    LLOutfitListBase::onOpen(info);

    LLAccordionCtrlTab* selected_tab = mAccordion->getSelectedTab();
    if (!selected_tab) return;

    // Pass focus to the selected outfit tab.
    selected_tab->showAndFocusHeader();
}


void LLOutfitsList::updateAddedCategory(LLUUID cat_id)
{
    LLViewerInventoryCategory *cat = gInventory.getCategory(cat_id);
    if (!cat) return;

    if (!isOutfitFolder(cat))
    {
        addFolderTab(cat);
        return;
    }

    std::string name = cat->getName();

    outfit_accordion_tab_params tab_params(get_accordion_tab_params());
    tab_params.cat_id = cat_id;
    LLOutfitAccordionCtrlTab *tab = LLUICtrlFactory::create<LLOutfitAccordionCtrlTab>(tab_params);
    if (!tab) return;
    LLWearableItemsList* wearable_list = LLUICtrlFactory::create<LLWearableItemsList>(tab_params.wearable_list);
    wearable_list->setShape(tab->getLocalRect());
    tab->addChild(wearable_list);

    tab->setName(name);
    tab->setTitle(name);
    tab->setFavorite(cat->getIsFavorite());

    // *TODO: LLUICtrlFactory::defaultBuilder does not use "display_children" from xml. Should be investigated.
    tab->setDisplayChildren(false);
    mAccordion->addCollapsibleCtrl(tab);

    // Start observing the new outfit category.
    LLWearableItemsList* list = tab->getChild<LLWearableItemsList>("wearable_items_list");
    if (!mCategoriesObserver->addCategory(cat_id, boost::bind(&LLWearableItemsList::updateList, list, cat_id)))
    {
        // Remove accordion tab if category could not be added to observer.
        mAccordion->removeCollapsibleCtrl(tab);

        // kill removed tab
        tab->die();
        return;
    }

    // Map the new tab with outfit category UUID.
    mOutfitsMap.insert(LLOutfitsList::outfits_map_value_t(cat_id, tab));

    tab->setRightMouseDownCallback(boost::bind(&LLOutfitListBase::outfitRightClickCallBack, this,
        _1, _2, _3, cat_id));

    // Setting tab focus callback to monitor currently selected outfit.
    tab->setFocusReceivedCallback(boost::bind(&LLOutfitListBase::ChangeOutfitSelection, this, list, cat_id));

    // Setting callback to reset items selection inside outfit on accordion collapsing and expanding (EXT-7875)
    tab->setDropDownStateChangedCallback(boost::bind(&LLOutfitsList::resetItemSelection, this, list, cat_id));

    // Depending on settings, force showing list items that don't match current filter(EXT-7158)
    static LLCachedControl<bool> list_filter(gSavedSettings, "OutfitListFilterFullList");
    list->setForceShowingUnmatchedItems(list_filter(), false);

    // Setting list commit callback to monitor currently selected wearable item.
    list->setCommitCallback(boost::bind(&LLOutfitsList::onListSelectionChange, this, _1));

    // Setting list refresh callback to apply filter on list change.
    list->setRefreshCompleteCallback(boost::bind(&LLOutfitsList::onRefreshComplete, this, _1));

    list->setRightMouseDownCallback(boost::bind(&LLOutfitsList::onWearableItemsListRightClick, this, _1, _2, _3));

    if (AISAPI::isAvailable() && LLInventoryModelBackgroundFetch::instance().folderFetchActive())
    {
        // for reliability just fetch it whole, linked items included
        LLInventoryModelBackgroundFetch::instance().fetchFolderAndLinks(cat_id, [cat_id, list]
        {
            if (list)
            {
                list->updateList(cat_id);
                list->setForceRefresh(true);
            }
        });
    }
    else
    {
        // Fetch the new outfit contents.
        cat->fetch();
        // Refresh the list of outfit items after fetch().
        // Further list updates will be triggered by the category observer.
        list->updateList(cat_id);
        list->setForceRefresh(true);
    }

    // If filter is currently applied we store the initial tab state.
    if (!getFilterSubString().empty())
    {
        tab->notifyChildren(LLSD().with("action", "store_state"));

        // Setting mForceRefresh flag will make the list refresh its contents
        // even if it is not currently visible. This is required to apply the
        // filter to the newly added list.
        list->setForceRefresh(true);

        list->setFilterSubString(getFilterSubString(), false);
    }
}

void LLOutfitsList::addFolderTab(LLViewerInventoryCategory* cat)
{
    const LLUUID cat_id = cat->getUUID();
    if (mFoldersMap.find(cat_id) != mFoldersMap.end())
    {
        return;
    }

    outfit_accordion_tab_params tab_params(get_accordion_tab_params());
    tab_params.cat_id = cat_id;
    LLOutfitAccordionCtrlTab* tab = LLUICtrlFactory::create<LLOutfitAccordionCtrlTab>(tab_params);
    if (!tab) return;

    // Header-only tab: "open" shows the folder's subfolders and outfits below it.
    tab->setIsFolder(true);
    tab->setName(cat->getName());
    tab->setTitle(cat->getName());
    tab->setFavorite(cat->getIsFavorite());
    tab->setHeaderIcon(LLUI::getUIImage("Inv_FolderClosed"));
    tab->setDisplayChildren(sExpandedOutfitFolders.count(cat_id) > 0);
    mAccordion->addCollapsibleCtrl(tab);
    mFoldersMap[cat_id] = tab;

    // Subfolders aren't covered by the My Outfits observer, watch them for added, moved or renamed entries.
    const LLUUID outfits = gInventory.findCategoryUUIDForType(LLFolderType::FT_MY_OUTFITS);
    mCategoriesObserver->addCategory(cat_id, [this, outfits]()
    {
        observerCallback(outfits);
    });

    tab->setRightMouseDownCallback(boost::bind(&LLOutfitListBase::folderRightClickCallBack, this, _1, _2, _3, cat_id));
    tab->setFocusReceivedCallback(boost::bind(&LLOutfitListBase::selectFolder, this, cat_id));
    tab->setDropDownStateChangedCallback(boost::bind(&LLOutfitsList::onFolderToggled, this, cat_id));

    if (!getFilterSubString().empty())
    {
        tab->notifyChildren(LLSD().with("action", "store_state"));
    }

    // Contents of a subfolder may not be loaded yet.
    cat->fetch();
}

void LLOutfitsList::updateRemovedCategory(LLUUID cat_id)
{
    folders_map_t::iterator folder_iter = mFoldersMap.find(cat_id);
    if (folder_iter != mFoldersMap.end())
    {
        LLOutfitAccordionCtrlTab* tab = folder_iter->second;
        mCategoriesObserver->removeCategory(cat_id);
        deselectOutfit(cat_id);
        mFoldersMap.erase(folder_iter);
        if (tab)
        {
            mAccordion->removeCollapsibleCtrl(tab);
            tab->die();
        }
        return;
    }

    outfits_map_t::iterator outfits_iter = mOutfitsMap.find(cat_id);
    if (outfits_iter != mOutfitsMap.end())
    {
        const LLUUID& outfit_id = outfits_iter->first;
        LLAccordionCtrlTab* tab = outfits_iter->second;

        // An outfit is removed from the list. Do the following:
        // 1. Remove outfit category from observer to stop monitoring its changes.
        mCategoriesObserver->removeCategory(outfit_id);

        // 2. Remove the outfit from selection.
        deselectOutfit(outfit_id);

        // 3. Remove category UUID to accordion tab mapping.
        mOutfitsMap.erase(outfits_iter);

        // 4. Remove outfit tab from accordion.
        mAccordion->removeCollapsibleCtrl(tab);

        // kill removed tab
        if (tab != NULL)
        {
            tab->die();
        }
    }
}

//virtual
void LLOutfitsList::onHighlightBaseOutfit(LLUUID base_id, LLUUID prev_id)
{
    LLOutfitAccordionCtrlTab* prev_tab = findTab(prev_id);
    if (prev_tab)
    {
        prev_tab->setOutfitSelected(false);
    }
    LLOutfitAccordionCtrlTab* base_tab = findTab(base_id);
    if (base_tab)
    {
        base_tab->setOutfitSelected(true);
    }
    updateTreeLayout(false);
}

void LLOutfitsList::onListSelectionChange(LLUICtrl* ctrl)
{
    LLWearableItemsList* list = dynamic_cast<LLWearableItemsList*>(ctrl);
    if (!list) return;

    LLViewerInventoryItem *item = gInventory.getItem(list->getSelectedUUID());
    if (!item) return;

    ChangeOutfitSelection(list, item->getParentUUID());
}

void LLOutfitListBase::performAction(std::string action)
{
    if (mSelectedOutfitUUID.isNull()) return;

    LLViewerInventoryCategory* cat = gInventory.getCategory(mSelectedOutfitUUID);
    if (!cat) return;

    if ("replaceoutfit" == action)
    {
        LLAppearanceMgr::instance().wearInventoryCategory( cat, false, false );
    }
    else if ("addtooutfit" == action)
    {
        LLAppearanceMgr::instance().wearInventoryCategory( cat, false, true );
    }
    else if ("rename_outfit" == action)
    {
        LLAppearanceMgr::instance().renameOutfit(mSelectedOutfitUUID);
    }
}

void LLOutfitsList::onSetSelectedOutfitByUUID(const LLUUID& outfit_uuid)
{
    // The outfit may sit in a collapsed folder.
    expandAncestors(outfit_uuid);

    for (outfits_map_t::iterator iter = mOutfitsMap.begin();
            iter != mOutfitsMap.end();
            ++iter)
    {
        if (outfit_uuid == iter->first)
        {
            LLAccordionCtrlTab* tab = iter->second;
            if (!tab) continue;

            LLWearableItemsList* list = dynamic_cast<LLWearableItemsList*>(tab->getAccordionView());
            if (!list) continue;

            tab->setFocus(true);
            ChangeOutfitSelection(list, outfit_uuid);

            tab->changeOpenClose(false);
        }
    }
}

void LLOutfitListBase::onAction(const LLSD& userdata)
{
    performAction(userdata.asString());
}

// virtual
bool LLOutfitListBase::isActionEnabled(const LLSD& userdata)
{
    const std::string command_name = userdata.asString();
    if (mSelectedOutfitUUID.isNull() && mSelectedFolderUUID.notNull())
    {
        if (command_name == "delete")
        {
            return isTreeFolder(mSelectedFolderUUID) && get_is_category_removable(&gInventory, mSelectedFolderUUID);
        }
        if (command_name == "rename")
        {
            return isTreeFolder(mSelectedFolderUUID) && get_is_category_renameable(&gInventory, mSelectedFolderUUID);
        }
        if (command_name == "save_outfit")
        {
            return !LLAppearanceMgr::getInstance()->isOutfitLocked() && LLAppearanceMgr::getInstance()->isOutfitDirty();
        }
        return false;
    }

    if (mSelectedOutfitUUID.isNull()) return false;

    if (command_name == "delete")
    {
        return !hasItemSelected() && LLAppearanceMgr::instance().getCanRemoveOutfit(mSelectedOutfitUUID);
    }
    if (command_name == "rename")
    {
        return get_is_category_renameable(&gInventory, mSelectedOutfitUUID);
    }
    if (command_name == "save_outfit")
    {
        bool outfit_locked = LLAppearanceMgr::getInstance()->isOutfitLocked();
        bool outfit_dirty = LLAppearanceMgr::getInstance()->isOutfitDirty();
        // allow save only if outfit isn't locked and is dirty
        return !outfit_locked && outfit_dirty;
    }
    if (command_name == "wear")
    {
        if (gAgentWearables.isCOFChangeInProgress())
        {
            return false;
        }

        if (hasItemSelected())
        {
            return canWearSelected();
        }

        // outfit selected
        return LLAppearanceMgr::instance().getCanReplaceCOF(mSelectedOutfitUUID);
    }
    if (command_name == "take_off")
    {
        // Enable "Take Off" if any of selected items can be taken off
        // or the selected outfit contains items that can be taken off.
        return ( hasItemSelected() && canTakeOffSelected() )
                || ( !hasItemSelected() && LLAppearanceMgr::getCanRemoveFromCOF(mSelectedOutfitUUID) );
    }

    if (command_name == "wear_add")
    {
        // *TODO: do we ever get here?
        return LLAppearanceMgr::getCanAddToCOF(mSelectedOutfitUUID);
    }

    return false;
}

void LLOutfitsList::getSelectedItemsUUIDs(uuid_vec_t& selected_uuids) const
{
    // Collect selected items from all selected lists.
    for (wearables_lists_map_t::const_iterator iter = mSelectedListsMap.begin();
            iter != mSelectedListsMap.end();
            ++iter)
    {
        uuid_vec_t uuids;
        (*iter).second->getSelectedUUIDs(uuids);

        auto prev_size = selected_uuids.size();
        selected_uuids.resize(prev_size + uuids.size());
        std::copy(uuids.begin(), uuids.end(), selected_uuids.begin() + prev_size);
    }
}

void LLOutfitsList::onCollapseAllFolders()
{
    for (outfits_map_t::iterator iter = mOutfitsMap.begin();
            iter != mOutfitsMap.end();
            ++iter)
    {
        LLAccordionCtrlTab* tab = iter->second;
        if(tab && tab->isExpanded())
        {
            tab->changeOpenClose(true);
        }
    }
    for (folders_map_t::value_type& entry : mFoldersMap)
    {
        if (entry.second && entry.second->getDisplayChildren())
        {
            entry.second->setDisplayChildren(false);
        }
    }
    sExpandedOutfitFolders.clear();
    updateTreeLayout(false);
    mAccordion->arrange();
}

void LLOutfitsList::onExpandAllFolders()
{
    for (folders_map_t::value_type& entry : mFoldersMap)
    {
        if (entry.second && !entry.second->getDisplayChildren())
        {
            entry.second->setDisplayChildren(true);
            sExpandedOutfitFolders.insert(entry.first);
        }
    }
    updateTreeLayout(false);

    for (outfits_map_t::iterator iter = mOutfitsMap.begin();
            iter != mOutfitsMap.end();
            ++iter)
    {
        LLAccordionCtrlTab* tab = iter->second;
        if(tab && !tab->isExpanded())
        {
            tab->changeOpenClose(false);
        }
    }
    mAccordion->arrange();
}

bool LLOutfitsList::hasItemSelected()
{
    return mItemSelected;
}

//////////////////////////////////////////////////////////////////////////
// Private methods
//////////////////////////////////////////////////////////////////////////

void LLOutfitsList::updateChangedCategoryName(LLViewerInventoryCategory *cat, std::string name)
{
    // Update tab name with the new category name.
    LLOutfitAccordionCtrlTab* tab = findTab(cat->getUUID());
    if (tab)
    {
        tab->setName(name);
        tab->setTitle(name);
        tab->setFavorite(cat->getIsFavorite());
    }
}

void LLOutfitsList::resetItemSelection(LLWearableItemsList* list, const LLUUID& category_id)
{
    list->resetSelection();
    mItemSelected = false;
    signalSelectionOutfitUUID(category_id);
}

void LLOutfitsList::onSelectFolder(const LLUUID& folder_id)
{
    for (wearables_lists_map_t::value_type& entry : mSelectedListsMap)
    {
        if (entry.second)
        {
            entry.second->resetSelection();
        }
    }
    mSelectedListsMap.clear();
    mItemSelected = false;
}

bool LLOutfitsList::hasFolderEntry(const LLUUID& cat_id) const
{
    return mFoldersMap.find(cat_id) != mFoldersMap.end();
}

LLOutfitAccordionCtrlTab* LLOutfitsList::findTab(const LLUUID& cat_id) const
{
    if (cat_id.isNull())
    {
        return nullptr;
    }
    outfits_map_t::const_iterator outfit_it = mOutfitsMap.find(cat_id);
    if (outfit_it != mOutfitsMap.end())
    {
        return static_cast<LLOutfitAccordionCtrlTab*>(outfit_it->second);
    }
    folders_map_t::const_iterator folder_it = mFoldersMap.find(cat_id);
    if (folder_it != mFoldersMap.end())
    {
        return folder_it->second;
    }
    return nullptr;
}

bool LLOutfitsList::isHiddenByCollapsedFolder(const LLUUID& cat_id) const
{
    const LLUUID root_id = gInventory.findCategoryUUIDForType(LLFolderType::FT_MY_OUTFITS);
    const LLViewerInventoryCategory* cat = gInventory.getCategory(cat_id);
    while (cat && cat->getParentUUID().notNull() && cat->getParentUUID() != root_id)
    {
        folders_map_t::const_iterator it = mFoldersMap.find(cat->getParentUUID());
        if (it != mFoldersMap.end() && it->second && !it->second->getDisplayChildren())
        {
            return true;
        }
        cat = gInventory.getCategory(cat->getParentUUID());
    }
    return false;
}

void LLOutfitsList::expandAncestors(const LLUUID& cat_id)
{
    bool changed = false;
    const LLUUID root_id = gInventory.findCategoryUUIDForType(LLFolderType::FT_MY_OUTFITS);
    const LLViewerInventoryCategory* cat = gInventory.getCategory(cat_id);
    while (cat && cat->getParentUUID().notNull() && cat->getParentUUID() != root_id)
    {
        folders_map_t::iterator it = mFoldersMap.find(cat->getParentUUID());
        if (it != mFoldersMap.end() && it->second && !it->second->getDisplayChildren())
        {
            it->second->setDisplayChildren(true);
            sExpandedOutfitFolders.insert(it->first);
            changed = true;
        }
        cat = gInventory.getCategory(cat->getParentUUID());
    }
    if (changed)
    {
        updateTreeLayout(false);
        mAccordion->arrange();
    }
}

void LLOutfitsList::onFolderToggled(const LLUUID& folder_id)
{
    LLOutfitAccordionCtrlTab* tab = findTab(folder_id);
    if (!tab) return;

    if (tab->getDisplayChildren())
    {
        sExpandedOutfitFolders.insert(folder_id);
    }
    else
    {
        sExpandedOutfitFolders.erase(folder_id);
    }
    updateTreeLayout(false);
    mAccordion->arrange();
}

void LLOutfitsList::onTreeRefreshed()
{
    updateTreeLayout(false);

    if (mPendingFolderSelection.notNull())
    {
        LLOutfitAccordionCtrlTab* tab = findTab(mPendingFolderSelection);
        if (tab)
        {
            mPendingFolderSelection.setNull();
            expandAncestors(tab->getFolderID());
            tab->showAndFocusHeader();
        }
    }
}

void LLOutfitsList::updateTreeLayout(bool sort)
{
    // The folders holding the worn outfit are shown bold too, so it can be found while collapsed.
    std::set<LLUUID> worn_path;
    const LLUUID root_id = gInventory.findCategoryUUIDForType(LLFolderType::FT_MY_OUTFITS);
    for (const LLViewerInventoryCategory* cat = gInventory.getCategory(mHighlightedOutfitUUID);
         cat && cat->getParentUUID().notNull() && cat->getParentUUID() != root_id;
         cat = gInventory.getCategory(cat->getParentUUID()))
    {
        worn_path.insert(cat->getParentUUID());
    }

    for (folders_map_t::value_type& entry : mFoldersMap)
    {
        if (entry.second)
        {
            entry.second->setOutfitSelected(worn_path.count(entry.first) > 0);
            updateTabTreeState(entry.second);
        }
    }
    for (outfits_map_t::value_type& entry : mOutfitsMap)
    {
        if (entry.second)
        {
            updateTabTreeState(static_cast<LLOutfitAccordionCtrlTab*>(entry.second));
        }
    }

    if (sort)
    {
        mAccordion->sort();
    }
}

void LLOutfitsList::updateTabTreeState(LLOutfitAccordionCtrlTab* tab)
{
    const LLUUID& cat_id = tab->getFolderID();
    LLViewerInventoryCategory* cat = gInventory.getCategory(cat_id);
    if (!cat) return;

    const LLUUID root_id = gInventory.findCategoryUUIDForType(LLFolderType::FT_MY_OUTFITS);
    LLOutfitAccordionCtrlTab::sort_path_t path;
    for (LLViewerInventoryCategory* node = cat; node && node->getUUID() != root_id; node = gInventory.getCategory(node->getParentUUID()))
    {
        LLOutfitAccordionCtrlTab::SortKey key;
        key.mIsFolder = (node != cat) || tab->isFolder();
        key.mIsFavorite = node->getIsFavorite();
        key.mName = node->getName();
        key.mID = node->getUUID();
        path.push_back(key);
    }
    std::reverse(path.begin(), path.end());
    tab->setSortPath(path);
    if (tab->isFolder())
    {
        tab->setHeaderIcon(LLUI::getUIImage(tab->getDisplayChildren() ? "Inv_FolderOpen" : "Inv_FolderClosed"));
    }

    const bool filtering = !getFilterSubString().empty();
    tab->setHeaderIndent(filtering || path.empty() ? 0 : (S32)(path.size() - 1) * FOLDER_TREE_INDENT);

    if (tab->isFolder())
    {
        // Matches are listed with their path while filtering, folder headers would only get in the way.
        tab->setVisible(!filtering && !isHiddenByCollapsedFolder(cat_id));
    }
    else if (filtering)
    {
        applyFilterToTab(cat_id, tab, getFilterSubString());
    }
    else
    {
        tab->setVisible(!isHiddenByCollapsedFolder(cat_id));
        if (tab->getTitle() != tab->getName())
        {
            tab->setTitle(tab->getName());
        }
    }
}

void LLOutfitsList::onChangeOutfitSelection(LLWearableItemsList* list, const LLUUID& category_id)
{
    MASK mask = gKeyboard->currentMask(true);

    // Reset selection in all previously selected tabs except for the current
    // if new selection is started.
    if (list && !(mask & MASK_CONTROL))
    {
        for (wearables_lists_map_t::iterator iter = mSelectedListsMap.begin();
                iter != mSelectedListsMap.end();
                ++iter)
        {
            LLWearableItemsList* selected_list = (*iter).second;
            if (selected_list != list)
            {
                selected_list->resetSelection();
            }
        }

        // Clear current selection.
        mSelectedListsMap.clear();
    }

    mItemSelected = list && (list->getSelectedItem() != NULL);

    mSelectedListsMap.insert(wearables_lists_map_value_t(category_id, list));
}

void LLOutfitsList::deselectOutfit(const LLUUID& category_id)
{
    // Remove selected lists map entry.
    mSelectedListsMap.erase(category_id);

    LLOutfitListBase::deselectOutfit(category_id);
}

void LLOutfitsList::restoreOutfitSelection(LLAccordionCtrlTab* tab, const LLUUID& category_id)
{
    // Try restoring outfit selection after filtering.
    if (mAccordion->getSelectedTab() == tab)
    {
        signalSelectionOutfitUUID(category_id);
    }
}

void LLOutfitsList::onRefreshComplete(LLUICtrl* ctrl)
{
    if (!ctrl || getFilterSubString().empty())
        return;

    for (outfits_map_t::iterator
             iter = mOutfitsMap.begin(),
             iter_end = mOutfitsMap.end();
         iter != iter_end; ++iter)
    {
        LLAccordionCtrlTab* tab = iter->second;
        if (!tab) continue;

        LLWearableItemsList* list = dynamic_cast<LLWearableItemsList*>(tab->getAccordionView());
        if (list != ctrl) continue;

        applyFilterToTab(iter->first, tab, getFilterSubString());
    }
}

// virtual
void LLOutfitsList::onFilterSubStringChanged(const std::string& new_string, const std::string& old_string)
{
    mAccordion->setFilterSubString(new_string);

    outfits_map_t::iterator iter = mOutfitsMap.begin(), iter_end = mOutfitsMap.end();
    while (iter != iter_end)
    {
        const LLUUID& category_id = iter->first;
        LLAccordionCtrlTab* tab = iter++->second;
        if (!tab) continue;

        LLWearableItemsList* list = dynamic_cast<LLWearableItemsList*>(tab->getAccordionView());
        if (list)
        {
            list->setFilterSubString(new_string, tab->getDisplayChildren());
        }

        if (old_string.empty())
        {
            // Store accordion tab state when filter is not empty
            tab->notifyChildren(LLSD().with("action", "store_state"));
        }

        if (!new_string.empty())
        {
            applyFilterToTab(category_id, tab, new_string);
        }
        else
        {
            // Restore tab title when filter is empty
            tab->setTitle(tab->getName());

            // Restore accordion state after all those accodrion tab manipulations
            tab->notifyChildren(LLSD().with("action", "restore_state"));

            // Try restoring the tab selection.
            restoreOutfitSelection(tab, category_id);
        }
    }

    for (folders_map_t::value_type& entry : mFoldersMap)
    {
        LLOutfitAccordionCtrlTab* tab = entry.second;
        if (!tab) continue;
        if (old_string.empty())
        {
            tab->notifyChildren(LLSD().with("action", "store_state"));
        }
        if (new_string.empty())
        {
            tab->notifyChildren(LLSD().with("action", "restore_state"));
        }
    }

    // Collapse state and indents come back once the filter is cleared.
    updateTreeLayout(false);
    mAccordion->arrange();
}

void LLOutfitsList::applyFilterToTab(
    const LLUUID&       category_id,
    LLAccordionCtrlTab* tab,
    const std::string&  filter_substring)
{
    if (!tab) return;
    LLWearableItemsList* list = dynamic_cast<LLWearableItemsList*>(tab->getAccordionView());
    if (!list) return;

    // Match the outfit name only, but show where the outfit lives.
    std::string name = tab->getName();
    std::string title = name;
    const std::string path = getFolderPath(category_id);
    if (!path.empty())
    {
        title = path + FOLDER_PATH_SEPARATOR + name;
    }
    LLStringUtil::toUpper(name);

    std::string cur_filter = filter_substring;
    LLStringUtil::toUpper(cur_filter);

    tab->setTitle(title, cur_filter);

    if (std::string::npos == name.find(cur_filter))
    {
        // Hide tab if its title doesn't pass filter
        // and it has no matched items
        tab->setVisible(list->hasMatchedItems());

        // Remove title highlighting because it might
        // have been previously highlighted by less restrictive filter
        tab->setTitle(title);

        // Remove the tab from selection.
        deselectOutfit(category_id);
    }
    else
    {
        // The name matches: show the tab again. A more restrictive filter (a typo, say) may
        // have hidden it, and so may a collapsed parent folder before filtering started.
        tab->setVisible(true);

        // Try restoring the tab selection.
        restoreOutfitSelection(tab, category_id);
    }
}

bool LLOutfitsList::canWearSelected()
{
    if (!isAgentAvatarValid())
    {
        return false;
    }

    uuid_vec_t selected_items;
    getSelectedItemsUUIDs(selected_items);
    S32 nonreplacable_objects = 0;

    for (uuid_vec_t::const_iterator it = selected_items.begin(); it != selected_items.end(); ++it)
    {
        const LLUUID& id = *it;

        // Check whether the item is worn.
        if (!get_can_item_be_worn(id))
        {
            return false;
        }

        const LLViewerInventoryItem* item = gInventory.getItem(id);
        if (!item)
        {
            return false;
        }

        if (item->getType() == LLAssetType::AT_OBJECT)
        {
            nonreplacable_objects++;
        }
    }

    // All selected items can be worn. But do we have enough space for them?
    return nonreplacable_objects == 0 || gAgentAvatarp->canAttachMoreObjects(nonreplacable_objects);
}

void LLOutfitsList::wearSelectedItems()
{
    uuid_vec_t selected_uuids;
    getSelectedItemsUUIDs(selected_uuids);

    if(selected_uuids.empty())
    {
        return;
    }

    wear_multiple(selected_uuids, false);
}

void LLOutfitsList::onWearableItemsListRightClick(LLUICtrl* ctrl, S32 x, S32 y)
{
    LLWearableItemsList* list = dynamic_cast<LLWearableItemsList*>(ctrl);
    if (!list) return;

    uuid_vec_t selected_uuids;

    getSelectedItemsUUIDs(selected_uuids);

    LLWearableItemsList::ContextMenu::instance().show(list, selected_uuids, x, y);
}

void LLOutfitsList::onCOFChanged()
{
    LLInventoryModel::cat_array_t cat_array;
    LLInventoryModel::item_array_t item_array;

    // Collect current COF items
    gInventory.collectDescendents(
        LLAppearanceMgr::instance().getCOF(),
        cat_array,
        item_array,
        LLInventoryModel::EXCLUDE_TRASH);

    uuid_vec_t vnew;
    uuid_vec_t vadded;
    uuid_vec_t vremoved;

    // From gInventory we get the UUIDs of links that are currently in COF.
    // These links UUIDs are not the same UUIDs that we have in each wearable items list.
    // So we collect base items' UUIDs to find them or links that point to them in wearable
    // items lists and update their worn state there.
    LLInventoryModel::item_array_t::const_iterator array_iter = item_array.begin(), array_end = item_array.end();
    while (array_iter < array_end)
    {
        vnew.push_back((*(array_iter++))->getLinkedUUID());
    }

    // We need to update only items that were added or removed from COF.
    LLCommonUtils::computeDifference(vnew, mCOFLinkedItems, vadded, vremoved);

    // Store the ids of items currently linked from COF.
    mCOFLinkedItems = vnew;

    // Append removed ids to added ids because we should update all of them.
    vadded.reserve(vadded.size() + vremoved.size());
    vadded.insert(vadded.end(), vremoved.begin(), vremoved.end());
    vremoved.clear();

    outfits_map_t::iterator map_iter = mOutfitsMap.begin(), map_end = mOutfitsMap.end();
    while (map_iter != map_end)
    {
        LLAccordionCtrlTab* tab = (map_iter++)->second;
        if (!tab) continue;

        LLWearableItemsList* list = dynamic_cast<LLWearableItemsList*>(tab->getAccordionView());
        if (!list) continue;

        // Every list updates the labels of changed items  or
        // the links that point to these items.
        list->updateChangedItems(vadded);
    }
}

void LLOutfitsList::getCurrentCategories(uuid_vec_t& vcur)
{
    // Creating a vector of currently displayed sub-categories UUIDs.
    for (outfits_map_t::const_iterator iter = mOutfitsMap.begin();
        iter != mOutfitsMap.end();
        iter++)
    {
        vcur.push_back((*iter).first);
    }
    for (const folders_map_t::value_type& entry : mFoldersMap)
    {
        vcur.push_back(entry.first);
    }
}


void LLOutfitsList::sortOutfits()
{
    mAccordion->sort();
}

void LLOutfitsList::onOutfitRightClick(LLUICtrl* ctrl, S32 x, S32 y, const LLUUID& cat_id)
{
    LLAccordionCtrlTab* tab = dynamic_cast<LLAccordionCtrlTab*>(ctrl);
    if (mOutfitMenu && is_tab_header_clicked(tab, y) && cat_id.notNull())
    {
        // Focus tab header to trigger tab selection change.
        LLUICtrl* header = tab->findChild<LLUICtrl>("dd_header");
        if (header)
        {
            header->setFocus(true);
        }

        uuid_vec_t selected_uuids;
        selected_uuids.push_back(cat_id);
        mOutfitMenu->show(ctrl, selected_uuids, x, y);
    }
}

void LLOutfitsList::handleInvFavColorChange()
{
    for (outfits_map_t::iterator iter = mOutfitsMap.begin();
        iter != mOutfitsMap.end();
        ++iter)
    {
        if (!iter->second) continue;
        LLOutfitAccordionCtrlTab* tab = (LLOutfitAccordionCtrlTab*)iter->second;

        // refresh font color
        tab->setFavorite(tab->getFavorite());
    }
    for (folders_map_t::value_type& entry : mFoldersMap)
    {
        if (entry.second)
        {
            entry.second->setFavorite(entry.second->getFavorite());
        }
    }
}

void LLOutfitsList::onChangeSortOrder(const LLSD& userdata)
{
    std::string sort_data = userdata.asString();
    if (sort_data == "favorites_to_top")
    {
        // at the moment this is a toggle
        S32 val = gSavedSettings.getS32("OutfitListSortOrder");
        gSavedSettings.setS32("OutfitListSortOrder", (val ? 0 : 1));

        initComparator();
    }
    else if (sort_data == "show_entire_outfit")
    {
        bool new_val = !gSavedSettings.getBOOL("OutfitListFilterFullList");
        gSavedSettings.setBOOL("OutfitListFilterFullList", new_val);

        if (!getFilterSubString().empty())
        {
            for (outfits_map_t::value_type& outfit : mOutfitsMap)
            {
                LLAccordionCtrlTab* tab = outfit.second;
                const LLUUID& category_id = outfit.first;
                if (!tab) continue;

                LLWearableItemsList* list = dynamic_cast<LLWearableItemsList*>(tab->getAccordionView());
                if (list)
                {
                    list->setForceRefresh(true);
                    list->setForceShowingUnmatchedItems(new_val, tab->getDisplayChildren());
                }
                applyFilterToTab(category_id, tab, getFilterSubString());
            }
            mAccordion->arrange();
        }
    }
}

LLToggleableMenu* LLOutfitsList::getSortMenu()
{
    if (!mSortMenu)
    {
        mSortMenu = new LLOutfitListSortMenu(this);
    }
    return mSortMenu->getMenu();
}

void LLOutfitsList::updateMenuItemsVisibility()
{
    if (mSortMenu)
    {
        mSortMenu->updateItemsVisibility();
    }
    LLOutfitListBase::updateMenuItemsVisibility();
}

LLOutfitListGearMenuBase* LLOutfitsList::createGearMenu()
{
    return new LLOutfitListGearMenu(this);
}

bool LLOutfitsList::handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop, EDragAndDropType cargo_type,
                                      void* cargo_data, EAcceptance* accept, std::string& tooltip_msg)
{
    // Children autoscroll and open folders on hover, but never accept anything.
    LLOutfitListBase::handleDragAndDrop(x, y, mask, drop, cargo_type, cargo_data, accept, tooltip_msg);

    // Over a folder: into it. Over an outfit: next to it. Anywhere else: the My Outfits root.
    LLUUID dest_id = gInventory.findCategoryUUIDForType(LLFolderType::FT_MY_OUTFITS);
    S32 screen_x, screen_y;
    localPointToScreen(x, y, &screen_x, &screen_y);
    auto hit = [screen_x, screen_y](LLAccordionCtrlTab* tab)
    {
        return tab && tab->isInVisibleChain() && tab->calcScreenRect().pointInRect(screen_x, screen_y);
    };
    bool found = false;
    for (folders_map_t::value_type& entry : mFoldersMap)
    {
        if (hit(entry.second))
        {
            dest_id = entry.first;
            found = true;
            break;
        }
    }
    if (!found)
    {
        for (outfits_map_t::value_type& entry : mOutfitsMap)
        {
            if (hit(entry.second))
            {
                const LLViewerInventoryCategory* cat = gInventory.getCategory(entry.first);
                if (cat && isTreeFolder(cat->getParentUUID()))
                {
                    dest_id = cat->getParentUUID();
                }
                break;
            }
        }
    }

    handleTreeDrop(dest_id, drop, cargo_type, cargo_data, accept, tooltip_msg);
    return true;
}


bool is_tab_header_clicked(LLAccordionCtrlTab* tab, S32 y)
{
    if(!tab || !tab->getHeaderVisible()) return false;

    S32 header_bottom = tab->getLocalRect().getHeight() - tab->getHeaderHeight();
    return y >= header_bottom;
}

LLOutfitListBase::LLOutfitListBase()
    :   LLPanelAppearanceTab()
    ,   mIsInitialized(false)
    ,   mGearMenu(nullptr)
{
    mCategoriesObserver = new LLInventoryCategoriesObserver();
    mOutfitMenu = new LLOutfitContextMenu(this);
    mFolderMenu = new LLOutfitFolderContextMenu(this);
}

LLOutfitListBase::~LLOutfitListBase()
{
    delete mOutfitMenu;
    delete mFolderMenu;
    delete mGearMenu;

    if (gInventory.containsObserver(mCategoriesObserver))
    {
        gInventory.removeObserver(mCategoriesObserver);
    }
    delete mCategoriesObserver;
}

void LLOutfitListBase::onOpen(const LLSD& info)
{
    if (!mIsInitialized)
    {
        // *TODO: I'm not sure is this check necessary but it never match while developing.
        if (!gInventory.isInventoryUsable())
            return;

        const LLUUID outfits = gInventory.findCategoryUUIDForType(LLFolderType::FT_MY_OUTFITS);

        // *TODO: I'm not sure is this check necessary but it never match while developing.
        LLViewerInventoryCategory* category = gInventory.getCategory(outfits);
        if (!category)
            return;

        gInventory.addObserver(mCategoriesObserver);

        // Start observing changes in "My Outfits" category.
        mCategoriesObserver->addCategory(outfits,
            boost::bind(&LLOutfitListBase::observerCallback, this, outfits));

        //const LLUUID cof = gInventory.findCategoryUUIDForType(LLFolderType::FT_CURRENT_OUTFIT);
        // Start observing changes in Current Outfit category.
        //mCategoriesObserver->addCategory(cof, boost::bind(&LLOutfitsList::onCOFChanged, this));

        LLOutfitObserver::instance().addBOFChangedCallback(boost::bind(&LLOutfitListBase::highlightBaseOutfit, this));
        LLOutfitObserver::instance().addBOFReplacedCallback(boost::bind(&LLOutfitListBase::highlightBaseOutfit, this));

        // Fetch "My Outfits" contents and refresh the list to display
        // initially fetched items. If not all items are fetched now
        // the observer will refresh the list as soon as the new items
        // arrive.
        category->fetch();
        refreshList(outfits);

        mIsInitialized = true;
    }
}

void LLOutfitListBase::observerCallback(const LLUUID& category_id)
{
    const LLInventoryModel::changed_items_t& changed_items = gInventory.getChangedIDs();
    mChangedItems.insert(changed_items.begin(), changed_items.end());
    refreshList(category_id);
}

// static
bool LLOutfitListBase::isOutfitFolder(const LLViewerInventoryCategory* cat)
{
    if (!cat)
    {
        return false;
    }
    if (cat->getPreferredType() == LLFolderType::FT_OUTFIT)
    {
        return true;
    }
    // assumes that folder is somewhere inside MyOutfits
    if (cat->getPreferredType() == LLFolderType::FT_NONE)
    {
        const LLViewerInventoryCategory* inv_cat = cat;
        if (inv_cat && inv_cat->getDescendentCount() > 3)
        {
            LLInventoryModel::cat_array_t* cats;
            LLInventoryModel::item_array_t* items;
            gInventory.getDirectDescendentsOf(inv_cat->getUUID(), cats, items);
            if (cats->empty() // protection against outfits inside
                && items->size() > 3) // arbitrary, if doesn't have at least base parts, not an outfit
            {
                // For now assume this to be an old style outfit, not a subfolder
                // but ideally no such 'outfits' should be left in My Outfits
                // Todo: stop counting FT_NONE as outfits,
                // convert obvious outfits into FT_OUTFIT
                return true;
            }
        }
    }
    return false;
}

// static
bool LLOutfitListBase::isTreeFolder(const LLUUID& cat_id)
{
    const LLUUID root_id = gInventory.findCategoryUUIDForType(LLFolderType::FT_MY_OUTFITS);
    if (cat_id.isNull() || cat_id == root_id)
    {
        return false;
    }
    const LLViewerInventoryCategory* cat = gInventory.getCategory(cat_id);
    if (!cat || isOutfitFolder(cat))
    {
        return false;
    }
    // Must reach My Outfits without passing through an outfit.
    LLUUID parent_id = cat->getParentUUID();
    while (parent_id.notNull())
    {
        if (parent_id == root_id)
        {
            return true;
        }
        const LLViewerInventoryCategory* parent = gInventory.getCategory(parent_id);
        if (!parent || isOutfitFolder(parent))
        {
            return false;
        }
        parent_id = parent->getParentUUID();
    }
    return false;
}

// static
std::string LLOutfitListBase::getFolderPath(const LLUUID& cat_id)
{
    std::string path;
    const LLUUID root_id = gInventory.findCategoryUUIDForType(LLFolderType::FT_MY_OUTFITS);
    const LLViewerInventoryCategory* cat = gInventory.getCategory(cat_id);
    while (cat && cat->getParentUUID().notNull() && cat->getParentUUID() != root_id)
    {
        cat = gInventory.getCategory(cat->getParentUUID());
        if (cat)
        {
            path = path.empty() ? cat->getName() : cat->getName() + FOLDER_PATH_SEPARATOR + path;
        }
    }
    return path;
}

static void append_tree_folders(const LLUUID& parent_id, const std::string& parent_path, S32 depth,
                                std::vector<LLOutfitFolderEntry>& folders)
{
    LLInventoryModel::cat_array_t* cats = nullptr;
    LLInventoryModel::item_array_t* items = nullptr;
    gInventory.getDirectDescendentsOf(parent_id, cats, items);
    if (!cats) return;

    std::vector<LLViewerInventoryCategory*> children;
    for (const LLPointer<LLViewerInventoryCategory>& cat : *cats)
    {
        if (cat && !LLOutfitListBase::isOutfitFolder(cat))
        {
            children.push_back(cat);
        }
    }
    std::sort(children.begin(), children.end(), [](const LLViewerInventoryCategory* a, const LLViewerInventoryCategory* b)
    {
        return LLStringUtil::compareDict(a->getName(), b->getName()) < 0;
    });

    for (LLViewerInventoryCategory* cat : children)
    {
        LLOutfitFolderEntry entry;
        entry.mID = cat->getUUID();
        entry.mPath = parent_path.empty() ? cat->getName() : parent_path + FOLDER_PATH_SEPARATOR + cat->getName();
        entry.mDepth = depth;
        folders.push_back(entry);
        append_tree_folders(entry.mID, entry.mPath, depth + 1, folders);
    }
}

// static
void LLOutfitListBase::getFolderTree(std::vector<LLOutfitFolderEntry>& folders)
{
    LLOutfitFolderEntry root;
    root.mID = gInventory.findCategoryUUIDForType(LLFolderType::FT_MY_OUTFITS);
    root.mDepth = 0;
    folders.push_back(root);
    append_tree_folders(root.mID, std::string(), 1, folders);
}

void LLOutfitListBase::collectTree(const LLUUID& parent_id, LLInventoryModel::cat_array_t& cats)
{
    LLInventoryModel::cat_array_t* sub_cats = nullptr;
    LLInventoryModel::item_array_t* items = nullptr;
    gInventory.getDirectDescendentsOf(parent_id, sub_cats, items);
    if (!sub_cats) return;

    // Outfits are leaves; anything else is a folder that may hold more outfits.
    const LLInventoryModel::cat_array_t children = *sub_cats;
    for (const LLPointer<LLViewerInventoryCategory>& cat : children)
    {
        if (!cat) continue;
        cats.push_back(cat);
        if (!isOutfitFolder(cat))
        {
            collectTree(cat->getUUID(), cats);
        }
    }
}

void LLOutfitListBase::refreshList(const LLUUID& category_id)
{
    if (LLAppViewer::instance()->quitRequested())
    {
        return;
    }
    bool wasNull = mRefreshListState.CategoryUUID.isNull();
    mRefreshListState.CategoryUUID.setNull();

    // Collect the outfits and folders of the tree under category_id.
    LLInventoryModel::cat_array_t cat_array;
    collectTree(category_id, cat_array);

    // <FS:Ansariel> FIRE-12939: Add outfit count to outfits list
    // (#429; counts outfits only, not the folders that group them)
    if (LLTextBox* count_text = findChild<LLTextBox>("OutfitcountText"))
    {
        S32 outfits = 0;
        for (const LLPointer<LLViewerInventoryCategory>& cat : cat_array)
        {
            if (isOutfitFolder(cat))
            {
                ++outfits;
            }
        }
        std::string count_string;
        LLLocale locale(LLLocale::USER_LOCALE);
        LLResMgr::getInstance()->getIntegerString(count_string, outfits);
        count_text->setTextArg("[COUNT]", count_string);
    }
    // </FS:Ansariel>

    // An unfetched plain folder can turn out to be an old style outfit (or back), rebuild those entries.
    uuid_vec_t vcur;
    getCurrentCategories(vcur);
    std::set<LLUUID> current(vcur.begin(), vcur.end());
    for (const LLPointer<LLViewerInventoryCategory>& cat : cat_array)
    {
        const LLUUID& cat_id = cat->getUUID();
        if (current.count(cat_id) && hasFolderEntry(cat_id) == isOutfitFolder(cat))
        {
            updateRemovedCategory(cat_id);
        }
    }

    // Memorize item names for each UUID
    std::map<LLUUID, std::string> names;
    for (const LLPointer<LLViewerInventoryCategory>& cat : cat_array)
    {
        names.emplace(std::make_pair(cat->getUUID(), cat->getName()));
    }

    // Fill added and removed items vectors.
    mRefreshListState.Added.clear();
    mRefreshListState.Removed.clear();
    computeDifference(cat_array, mRefreshListState.Added, mRefreshListState.Removed);
    // Sort added items vector by item name.
    // Capture by reference: std::sort copies its comparator freely, and a by-value capture
    // copied the whole name map each time (#357).
    std::sort(mRefreshListState.Added.begin(), mRefreshListState.Added.end(),
        [&names](const LLUUID& a, const LLUUID& b)
        {
            return LLStringUtil::compareDict(names.at(a), names.at(b)) < 0;
        });
    // Initialize iterators for added and removed items vectors.
    mRefreshListState.AddedIterator = mRefreshListState.Added.begin();
    mRefreshListState.RemovedIterator = mRefreshListState.Removed.begin();

    LL_INFOS() << "added: " << mRefreshListState.Added.size() <<
        ", removed: " << mRefreshListState.Removed.size() <<
        ", changed: " << gInventory.getChangedIDs().size() <<
        LL_ENDL;

    mRefreshListState.CategoryUUID = category_id;
    if (wasNull)
    {
        gIdleCallbacks.addFunction(onIdle, this);
    }
}

// static
void LLOutfitListBase::onIdle(void* userdata)
{
    LLOutfitListBase* self = (LLOutfitListBase*)userdata;

    self->onIdleRefreshList();
}

void LLOutfitListBase::onIdleRefreshList()
{
    if (LLAppViewer::instance()->quitRequested())
    {
        mRefreshListState.CategoryUUID.setNull();
        gIdleCallbacks.deleteFunction(onIdle, this);
        return;
    }
    if (mRefreshListState.CategoryUUID.isNull())
    {
        LL_WARNS() << "Called onIdleRefreshList without id" << LL_ENDL;
        gIdleCallbacks.deleteFunction(onIdle, this);
        return;
    }

    // #357: was a flat 50 ms per frame, visible or not. Build outfit tabs with a frame-sized
    // budget while the list is shown and a token one while hidden.
    const F64 MAX_TIME = isInVisibleChain() ? 0.005 : 0.001;
    F64 curent_time = LLTimer::getTotalSeconds();
    const F64 end_time = curent_time + MAX_TIME;

    // Handle added tabs.
    while (mRefreshListState.AddedIterator < mRefreshListState.Added.end())
    {
        const LLUUID cat_id = (*mRefreshListState.AddedIterator++);
        updateAddedCategory(cat_id);

        curent_time = LLTimer::getTotalSeconds();
        if (curent_time >= end_time)
            return;
    }
    mRefreshListState.Added.clear();
    mRefreshListState.AddedIterator = mRefreshListState.Added.end();

    // Handle removed tabs.
    while (mRefreshListState.RemovedIterator < mRefreshListState.Removed.end())
    {
        const LLUUID cat_id = (*mRefreshListState.RemovedIterator++);
        updateRemovedCategory(cat_id);

        curent_time = LLTimer::getTotalSeconds();
        if (curent_time >= end_time)
            return;
    }
    mRefreshListState.Removed.clear();
    mRefreshListState.RemovedIterator = mRefreshListState.Removed.end();

    // Get changed items from inventory model and update outfit tabs
    // which might have been renamed.
    while (!mChangedItems.empty())
    {
        std::set<LLUUID>::const_iterator items_iter = mChangedItems.begin();
        LLViewerInventoryCategory *cat = gInventory.getCategory(*items_iter);
        mChangedItems.erase(items_iter);

        // Links aren't supposed to be allowed here, check only cats
        if (cat)
        {
            std::string name = cat->getName();
            updateChangedCategoryName(cat, name);
        }

        curent_time = LLTimer::getTotalSeconds();
        if (curent_time >= end_time)
            return;
    }

    onTreeRefreshed();
    sortOutfits();
    highlightBaseOutfit();

    gIdleCallbacks.deleteFunction(onIdle, this);
    mRefreshListState.CategoryUUID.setNull();

    LL_INFOS() << "done" << LL_ENDL;
}

void LLOutfitListBase::computeDifference(
    const LLInventoryModel::cat_array_t& vcats,
    uuid_vec_t& vadded,
    uuid_vec_t& vremoved)
{
    uuid_vec_t vnew;
    // Creating a vector of newly collected sub-categories UUIDs.
    for (LLInventoryModel::cat_array_t::const_iterator iter = vcats.begin();
        iter != vcats.end();
        iter++)
    {
        vnew.push_back((*iter)->getUUID());
    }

    uuid_vec_t vcur;
    getCurrentCategories(vcur);

    LLCommonUtils::computeDifference(vnew, vcur, vadded, vremoved);
}

void LLOutfitListBase::sortOutfits()
{
}

void LLOutfitListBase::highlightBaseOutfit()
{
    // id of base outfit
    LLUUID base_id = LLAppearanceMgr::getInstance()->getBaseOutfitUUID();
    if (base_id != mHighlightedOutfitUUID)
    {
        LLUUID prev_id = mHighlightedOutfitUUID;
        mHighlightedOutfitUUID = base_id;
        onHighlightBaseOutfit(base_id, prev_id);
    }
}

void LLOutfitListBase::removeSelected()
{
    if (mSelectedOutfitUUID.isNull() && mSelectedFolderUUID.notNull())
    {
        removeFolder(mSelectedFolderUUID);
        return;
    }
    // <FS:Ansariel> FIRE-15888: Include outfit name in delete outfit confirmation dialog
    // (#429: only the confirmation text changes; the delete itself is untouched)
    if (LLViewerInventoryCategory* cat = gInventory.getCategory(mSelectedOutfitUUID))
    {
        LLSD args;
        args["NAME"] = cat->getName();
        LLNotificationsUtil::add("DeleteOutfitsWithName", args, LLSD(), boost::bind(&LLOutfitListBase::onOutfitsRemovalConfirmation, this, _1, _2));
        return;
    }
    // </FS:Ansariel>
    LLNotificationsUtil::add("DeleteOutfits", LLSD(), LLSD(), boost::bind(&LLOutfitListBase::onOutfitsRemovalConfirmation, this, _1, _2));
}

void LLOutfitListBase::onOutfitsRemovalConfirmation(const LLSD& notification, const LLSD& response)
{
    S32 option = LLNotificationsUtil::getSelectedOption(notification, response);
    if (option != 0) return; // canceled

    if (mSelectedOutfitUUID.notNull())
    {
        gInventory.removeCategory(mSelectedOutfitUUID);
    }
}

void LLOutfitListBase::setSelectedOutfitByUUID(const LLUUID& outfit_uuid)
{
    onSetSelectedOutfitByUUID(outfit_uuid);
}

boost::signals2::connection LLOutfitListBase::setSelectionChangeCallback(selection_change_callback_t cb)
{
    return mSelectionChangeSignal.connect(cb);
}

void LLOutfitListBase::signalSelectionOutfitUUID(const LLUUID& category_id)
{
    mSelectionChangeSignal(category_id);
}

void LLOutfitListBase::outfitRightClickCallBack(LLUICtrl* ctrl, S32 x, S32 y, const LLUUID& cat_id)
{
    onOutfitRightClick(ctrl, x, y, cat_id);
}

void LLOutfitListBase::ChangeOutfitSelection(LLWearableItemsList* list, const LLUUID& category_id)
{
    onChangeOutfitSelection(list, category_id);
    mSelectedOutfitUUID = category_id;
    mSelectedFolderUUID.setNull();
    signalSelectionOutfitUUID(category_id);
}

void LLOutfitListBase::selectFolder(const LLUUID& folder_id)
{
    if (mSelectedOutfitUUID.isNull() && mSelectedFolderUUID == folder_id)
    {
        return;
    }
    onSelectFolder(folder_id);
    mSelectedOutfitUUID.setNull();
    mSelectedFolderUUID = folder_id;
    signalSelectionOutfitUUID(LLUUID::null);
}

LLUUID LLOutfitListBase::getCurrentFolderID() const
{
    if (isTreeFolder(mSelectedFolderUUID))
    {
        return mSelectedFolderUUID;
    }
    if (mSelectedOutfitUUID.notNull())
    {
        const LLViewerInventoryCategory* cat = gInventory.getCategory(mSelectedOutfitUUID);
        if (cat && isTreeFolder(cat->getParentUUID()))
        {
            return cat->getParentUUID();
        }
    }
    return gInventory.findCategoryUUIDForType(LLFolderType::FT_MY_OUTFITS);
}

void LLOutfitListBase::folderRightClickCallBack(LLUICtrl* ctrl, S32 x, S32 y, const LLUUID& folder_id)
{
    if (!mFolderMenu || folder_id.isNull()) return;

    LLAccordionCtrlTab* tab = dynamic_cast<LLAccordionCtrlTab*>(ctrl);
    if (tab && !is_tab_header_clicked(tab, y)) return;
    if (tab)
    {
        LLUICtrl* header = tab->findChild<LLUICtrl>("dd_header");
        if (header)
        {
            header->setFocus(true);
        }
    }
    selectFolder(folder_id);

    uuid_vec_t selected_uuids;
    selected_uuids.push_back(folder_id);
    mFolderMenu->show(ctrl, selected_uuids, x, y);
}

void LLOutfitListBase::createFolder(const LLUUID& parent_id)
{
    const LLUUID dest_id = isTreeFolder(parent_id) ? parent_id : gInventory.findCategoryUUIDForType(LLFolderType::FT_MY_OUTFITS);
    if (dest_id.isNull()) return;

    LLSD args;
    args["DESC"] = LLTrans::getString("New Folder");
    LLHandle<LLPanel> handle = getHandle();
    LLNotificationsUtil::add("CreateSubfolder", args, LLSD(),
        [dest_id, handle](const LLSD& notification, const LLSD& response)
    {
        if (LLNotificationsUtil::getSelectedOption(notification, response) != 0) return;

        std::string name = response["message"].asString();
        LLInventoryObject::correctInventoryName(name);
        if (name.empty())
        {
            name = LLTrans::getString("New Folder");
        }
        gInventory.createNewCategory(dest_id, LLFolderType::FT_NONE, name, [handle](const LLUUID& new_id)
        {
            LLOutfitListBase* list = dynamic_cast<LLOutfitListBase*>(handle.get());
            if (list && new_id.notNull())
            {
                list->mPendingFolderSelection = new_id;
            }
        });
    });
}

void LLOutfitListBase::renameFolder(const LLUUID& folder_id)
{
    LLViewerInventoryCategory* cat = gInventory.getCategory(folder_id);
    if (!cat || !isTreeFolder(folder_id)) return;

    LLSD args;
    args["NAME"] = cat->getName();
    LLSD payload;
    payload["cat_id"] = folder_id;
    LLNotificationsUtil::add("RenameOutfitFolder", args, payload, [](const LLSD& notification, const LLSD& response)
    {
        if (LLNotificationsUtil::getSelectedOption(notification, response) != 0) return;

        std::string name = response["new_name"].asString();
        LLInventoryObject::correctInventoryName(name);
        if (!name.empty())
        {
            rename_category(&gInventory, notification["payload"]["cat_id"].asUUID(), name);
        }
    });
}

void LLOutfitListBase::removeFolder(const LLUUID& folder_id)
{
    LLViewerInventoryCategory* cat = gInventory.getCategory(folder_id);
    if (!cat || !isTreeFolder(folder_id) || !get_is_category_removable(&gInventory, folder_id)) return;

    // Contents move up to the parent, so every child has to be known first.
    LLInventoryModel::cat_array_t* cats = nullptr;
    LLInventoryModel::item_array_t* items = nullptr;
    gInventory.getDirectDescendentsOf(folder_id, cats, items);
    if (!cats || !items || !gInventory.isCategoryComplete(folder_id))
    {
        cat->fetch();
        LLNotificationsUtil::add("OutfitFolderNotLoaded");
        return;
    }

    if (cats->empty() && items->empty())
    {
        gInventory.removeCategory(folder_id);
        return;
    }

    LLSD args;
    args["NAME"] = cat->getName();
    LLNotificationsUtil::add("DeleteOutfitFolder", args, LLSD(),
        boost::bind(&LLOutfitListBase::onFolderRemovalConfirmation, _1, _2, folder_id));
}

// static
void LLOutfitListBase::onFolderRemovalConfirmation(const LLSD& notification, const LLSD& response, const LLUUID& folder_id)
{
    if (LLNotificationsUtil::getSelectedOption(notification, response) != 0) return;

    LLViewerInventoryCategory* folder = gInventory.getCategory(folder_id);
    if (!folder || !isTreeFolder(folder_id)) return;

    LLInventoryModel::cat_array_t* cats = nullptr;
    LLInventoryModel::item_array_t* items = nullptr;
    gInventory.getDirectDescendentsOf(folder_id, cats, items);
    if (!cats || !items || !gInventory.isCategoryComplete(folder_id))
    {
        LLNotificationsUtil::add("OutfitFolderNotLoaded");
        return;
    }

    // Never trash outfits with the folder: move everything up a level first.
    const LLUUID parent_id = folder->getParentUUID();
    const LLInventoryModel::cat_array_t child_cats = *cats;
    const LLInventoryModel::item_array_t child_items = *items;
    for (const LLPointer<LLViewerInventoryCategory>& child : child_cats)
    {
        gInventory.changeCategoryParent(child, parent_id, false);
    }
    for (const LLPointer<LLViewerInventoryItem>& child : child_items)
    {
        gInventory.changeItemParent(child, parent_id, false);
    }
    gInventory.removeCategory(folder_id);
}

void LLOutfitListBase::groupOutfitsByPrefix()
{
    static const std::string PREFIX_SEPARATOR(" - ");

    const LLUUID root_id = gInventory.findCategoryUUIDForType(LLFolderType::FT_MY_OUTFITS);
    LLInventoryModel::cat_array_t* cats = nullptr;
    LLInventoryModel::item_array_t* items = nullptr;
    gInventory.getDirectDescendentsOf(root_id, cats, items);
    if (!cats) return;

    // "Casual - Summer" -> folder "Casual", outfit "Summer". Only prefixes shared by two or more outfits.
    std::map<std::string, std::vector<std::pair<LLUUID, std::string> > > groups;
    std::map<std::string, LLUUID> existing_folders;
    for (const LLPointer<LLViewerInventoryCategory>& cat : *cats)
    {
        if (!cat) continue;
        if (!isOutfitFolder(cat))
        {
            existing_folders[cat->getName()] = cat->getUUID();
            continue;
        }
        const std::string& name = cat->getName();
        size_t pos = name.find(PREFIX_SEPARATOR);
        if (pos == std::string::npos || pos == 0) continue;
        std::string prefix = name.substr(0, pos);
        std::string rest = name.substr(pos + PREFIX_SEPARATOR.size());
        LLStringUtil::trim(prefix);
        LLStringUtil::trim(rest);
        if (prefix.empty() || rest.empty()) continue;
        groups[prefix].push_back(std::make_pair(cat->getUUID(), rest));
    }

    S32 outfit_count = 0;
    for (auto it = groups.begin(); it != groups.end();)
    {
        if (it->second.size() < 2)
        {
            it = groups.erase(it);
        }
        else
        {
            outfit_count += (S32)it->second.size();
            ++it;
        }
    }
    if (groups.empty())
    {
        LLNotificationsUtil::add("NoOutfitsToGroup");
        return;
    }

    LLSD args;
    args["COUNT"] = outfit_count;
    args["FOLDERS"] = (S32)groups.size();
    LLNotificationsUtil::add("GroupOutfitsByPrefix", args, LLSD(),
        [groups, existing_folders, root_id](const LLSD& notification, const LLSD& response)
    {
        if (LLNotificationsUtil::getSelectedOption(notification, response) != 0) return;

        for (const auto& group : groups)
        {
            const std::vector<std::pair<LLUUID, std::string> > outfits = group.second;
            auto move_into = [outfits](const LLUUID& folder_id)
            {
                if (folder_id.isNull()) return;
                for (const auto& outfit : outfits)
                {
                    LLViewerInventoryCategory* cat = gInventory.getCategory(outfit.first);
                    if (!cat) continue;
                    gInventory.changeCategoryParent(cat, folder_id, false);
                    rename_category(&gInventory, outfit.first, outfit.second);
                }
            };
            auto existing = existing_folders.find(group.first);
            if (existing != existing_folders.end() && gInventory.getCategory(existing->second))
            {
                move_into(existing->second);
            }
            else
            {
                gInventory.createNewCategory(root_id, LLFolderType::FT_NONE, group.first, move_into);
            }
        }
    });
}

// static
bool LLOutfitListBase::handleTreeDrop(const LLUUID& dest_id, bool drop, EDragAndDropType cargo_type, void* cargo_data,
                                      EAcceptance* accept, std::string& tooltip_msg)
{
    *accept = ACCEPT_NO;
    if (cargo_type != DAD_CATEGORY || !cargo_data) return false;

    LLInventoryObject* obj = static_cast<LLInventoryObject*>(cargo_data);
    LLViewerInventoryCategory* cat = gInventory.getCategory(obj->getUUID());
    if (!cat) return false;

    const LLUUID root_id = gInventory.findCategoryUUIDForType(LLFolderType::FT_MY_OUTFITS);
    const LLUUID& cat_id = cat->getUUID();

    // Only rearranges what is already in the My Outfits tree; making outfits from other folders stays in the inventory.
    const bool is_outfit = isOutfitFolder(cat) && (cat->getParentUUID() == root_id || isTreeFolder(cat->getParentUUID()));
    if (!is_outfit && !isTreeFolder(cat_id)) return false;
    if (dest_id != root_id && !isTreeFolder(dest_id)) return false;
    if (cat->getParentUUID() == dest_id) return false;

    if (dest_id == cat_id || gInventory.isObjectDescendentOf(dest_id, cat_id))
    {
        tooltip_msg = LLTrans::getString("TooltipOutfitFolderIntoItself");
        return false;
    }
    if (!get_is_category_removable(&gInventory, cat_id))
    {
        return false;
    }

    *accept = ACCEPT_YES_SINGLE;
    if (drop)
    {
        gInventory.changeCategoryParent(cat, dest_id, false);
    }
    return true;
}

bool LLOutfitListBase::postBuild()
{
    return true;
}

void LLOutfitListBase::collapseAllFolders()
{
    onCollapseAllFolders();
}

void LLOutfitListBase::expandAllFolders()
{
    onExpandAllFolders();
}

void LLOutfitListBase::updateMenuItemsVisibility()
{
    mGearMenu->updateItemsVisibility();
}

LLToggleableMenu* LLOutfitListBase::getGearMenu()
{
    if (!mGearMenu)
    {
        mGearMenu = createGearMenu();
    }
    return mGearMenu->getMenu();
};

void LLOutfitListBase::deselectOutfit(const LLUUID& category_id)
{
    // Reset selection if the outfit is selected.
    if (category_id == mSelectedOutfitUUID)
    {
        mSelectedOutfitUUID = LLUUID::null;
        signalSelectionOutfitUUID(mSelectedOutfitUUID);
    }
    else if (category_id.notNull() && category_id == mSelectedFolderUUID)
    {
        mSelectedFolderUUID.setNull();
        signalSelectionOutfitUUID(LLUUID::null);
    }
}

LLContextMenu* LLOutfitFolderContextMenu::createMenu()
{
    LLUICtrl::CommitCallbackRegistry::ScopedRegistrar registrar;
    LLUICtrl::EnableCallbackRegistry::ScopedRegistrar enable_registrar;
    const LLUUID selected_id = mUUIDs.front();
    LLOutfitListBase* list = mOutfitList;

    registrar.add("OutfitFolder.NewFolder", [list, selected_id](LLUICtrl*, const LLSD&) { list->createFolder(selected_id); });
    registrar.add("OutfitFolder.Rename", [list, selected_id](LLUICtrl*, const LLSD&) { list->renameFolder(selected_id); });
    registrar.add("OutfitFolder.Delete", [list, selected_id](LLUICtrl*, const LLSD&) { list->removeFolder(selected_id); });
    registrar.add("OutfitFolder.Favorite", [selected_id](LLUICtrl*, const LLSD&) { toggle_favorite(selected_id); });
    registrar.add("OutfitFolder.SaveHere", [selected_id](LLUICtrl*, const LLSD&)
    {
        LLFloaterSaveOutfitAs::show(selected_id);
    });
    enable_registrar.add("OutfitFolder.OnEnable", boost::bind(&LLOutfitFolderContextMenu::onEnable, this, _2));

    return createFromFile("menu_outfit_folder.xml");
}

bool LLOutfitFolderContextMenu::onEnable(LLSD::String param)
{
    const LLUUID folder_id = mUUIDs.back();
    if ("rename" == param)
    {
        return get_is_category_renameable(&gInventory, folder_id);
    }
    if ("delete" == param)
    {
        return get_is_category_removable(&gInventory, folder_id);
    }
    if ("save_here" == param)
    {
        return !LLAppearanceMgr::instance().isOutfitLocked();
    }
    if ("favorites_add" == param || "favorites_remove" == param)
    {
        LLViewerInventoryCategory* cat = gInventory.getCategory(folder_id);
        return cat && (cat->getIsFavorite() == ("favorites_remove" == param));
    }
    return true;
}

LLContextMenu* LLOutfitContextMenu::createMenu()
{
    LLUICtrl::CommitCallbackRegistry::ScopedRegistrar registrar;
    LLUICtrl::EnableCallbackRegistry::ScopedRegistrar enable_registrar;
    LLUUID selected_id = mUUIDs.front();

    registrar.add("Outfit.WearReplace",
        boost::bind(&LLAppearanceMgr::replaceCurrentOutfit, &LLAppearanceMgr::instance(), selected_id));
    registrar.add("Outfit.WearAdd",
        boost::bind(&LLAppearanceMgr::addCategoryToCurrentOutfit, &LLAppearanceMgr::instance(), selected_id));
    registrar.add("Outfit.TakeOff",
        boost::bind(&LLAppearanceMgr::takeOffOutfit, &LLAppearanceMgr::instance(), selected_id));
    registrar.add("Outfit.Edit", boost::bind(editOutfit));
    registrar.add("Outfit.Rename", boost::bind(renameOutfit, selected_id));
    registrar.add("Outfit.Delete", boost::bind(&LLOutfitListBase::removeSelected, mOutfitList));
    registrar.add("Outfit.Thumbnail", boost::bind(&LLOutfitContextMenu::onThumbnail, this, selected_id));
    registrar.add("Outfit.Favorite", boost::bind(&LLOutfitContextMenu::onFavorite, this, selected_id));
    registrar.add("Outfit.Save", boost::bind(&LLOutfitContextMenu::onSave, this, selected_id));

    enable_registrar.add("Outfit.OnEnable", boost::bind(&LLOutfitContextMenu::onEnable, this, _2));
    enable_registrar.add("Outfit.OnVisible", boost::bind(&LLOutfitContextMenu::onVisible, this, _2));

    return createFromFile("menu_outfit_tab.xml");

}

bool LLOutfitContextMenu::onEnable(LLSD::String param)
{
    LLUUID outfit_cat_id = mUUIDs.back();

    if ("rename" == param)
    {
        return get_is_category_renameable(&gInventory, outfit_cat_id);
    }
    else if ("wear_replace" == param)
    {
        return LLAppearanceMgr::instance().getCanReplaceCOF(outfit_cat_id);
    }
    else if ("wear_add" == param)
    {
        return LLAppearanceMgr::getCanAddToCOF(outfit_cat_id);
    }
    else if ("take_off" == param)
    {
        return LLAppearanceMgr::getCanRemoveFromCOF(outfit_cat_id);
    }

    return true;
}

bool LLOutfitContextMenu::onVisible(LLSD::String param)
{
    LLUUID outfit_cat_id = mUUIDs.back();

    if ("edit" == param)
    {
        bool is_worn = LLAppearanceMgr::instance().getBaseOutfitUUID() == outfit_cat_id;
        return is_worn;
    }
    else if ("wear_replace" == param)
    {
        return true;
    }
    else if ("delete" == param)
    {
        return LLAppearanceMgr::instance().getCanRemoveOutfit(outfit_cat_id);
    }
    else if ("favorites_add" == param)
    {
        LLViewerInventoryCategory* cat = gInventory.getCategory(outfit_cat_id);
        return cat && !cat->getIsFavorite();
    }
    else if ("favorites_remove" == param)
    {
        LLViewerInventoryCategory* cat = gInventory.getCategory(outfit_cat_id);
        return cat && cat->getIsFavorite();
    }

    return true;
}

//static
void LLOutfitContextMenu::editOutfit()
{
    LLFloaterSidePanelContainer::showPanel("appearance", LLSD().with("type", "edit_outfit"));
}

void LLOutfitContextMenu::renameOutfit(const LLUUID& outfit_cat_id)
{
    LLAppearanceMgr::instance().renameOutfit(outfit_cat_id);
}

void LLOutfitContextMenu::onThumbnail(const LLUUID &outfit_cat_id)
{
    if (outfit_cat_id.notNull())
    {
        LLSD data(outfit_cat_id);
        LLFloaterReg::showInstance("change_item_thumbnail", data);
    }
}

void LLOutfitContextMenu::onFavorite(const LLUUID& outfit_cat_id)
{
    if (outfit_cat_id.notNull())
    {
        toggle_favorite(outfit_cat_id);
    }
}

void LLOutfitContextMenu::onSave(const LLUUID &outfit_cat_id)
{
    if (outfit_cat_id.notNull())
    {
        LLNotificationsUtil::add("ConfirmOverwriteOutfit", LLSD(), LLSD(),
            [outfit_cat_id](const LLSD &notif, const LLSD &resp)
        {
            S32 opt = LLNotificationsUtil::getSelectedOption(notif, resp);
            if (opt == 0)
            {
                LLAppearanceMgr::getInstance()->onOutfitFolderCreated(outfit_cat_id, true);
            }
        });
    }
}

LLOutfitListGearMenuBase::LLOutfitListGearMenuBase(LLOutfitListBase* olist)
    :   mOutfitList(olist),
        mMenu(NULL)
{
    llassert_always(mOutfitList);

    LLUICtrl::CommitCallbackRegistry::ScopedRegistrar registrar;
    LLUICtrl::EnableCallbackRegistry::ScopedRegistrar enable_registrar;

    registrar.add("Gear.Wear", boost::bind(&LLOutfitListGearMenuBase::onWear, this));
    registrar.add("Gear.TakeOff", boost::bind(&LLOutfitListGearMenuBase::onTakeOff, this));
    registrar.add("Gear.Rename", boost::bind(&LLOutfitListGearMenuBase::onRename, this));
    registrar.add("Gear.Delete", boost::bind(&LLOutfitListBase::removeSelected, mOutfitList));
    registrar.add("Gear.Create", boost::bind(&LLOutfitListGearMenuBase::onCreate, this, _2));

    registrar.add("Gear.WearAdd", boost::bind(&LLOutfitListGearMenuBase::onAdd, this));
    registrar.add("Gear.Save", boost::bind(&LLOutfitListGearMenuBase::onSave, this));

    registrar.add("Gear.Thumbnail", boost::bind(&LLOutfitListGearMenuBase::onThumbnail, this));
    registrar.add("Gear.Favorite", boost::bind(&LLOutfitListGearMenuBase::onFavorite, this));
    registrar.add("Gear.SortByImage", boost::bind(&LLOutfitListGearMenuBase::onChangeSortOrder, this));
    registrar.add("Gear.NewFolder", boost::bind(&LLOutfitListGearMenuBase::onNewFolder, this));
    registrar.add("Gear.RenameFolder", boost::bind(&LLOutfitListGearMenuBase::onRenameFolder, this));
    registrar.add("Gear.DeleteFolder", boost::bind(&LLOutfitListGearMenuBase::onDeleteFolder, this));
    registrar.add("Gear.GroupByPrefix", boost::bind(&LLOutfitListGearMenuBase::onGroupByPrefix, this));

    enable_registrar.add("Gear.OnEnable", boost::bind(&LLOutfitListGearMenuBase::onEnable, this, _2));
    enable_registrar.add("Gear.OnVisible", boost::bind(&LLOutfitListGearMenuBase::onVisible, this, _2));

    mMenu = LLUICtrlFactory::getInstance()->createFromFile<LLToggleableMenu>(
        "menu_outfit_gear.xml", gMenuHolder, LLViewerMenuHolderGL::child_registry_t::instance());
    llassert(mMenu);
}

LLOutfitListGearMenuBase::~LLOutfitListGearMenuBase()
{}

void LLOutfitListGearMenuBase::updateItemsVisibility()
{
    onUpdateItemsVisibility();
}

void LLOutfitListGearMenuBase::onUpdateItemsVisibility()
{
    if (!mMenu) return;

    bool have_selection = getSelectedOutfitID().notNull();
    mMenu->setItemVisible("wear_separator", have_selection);
    mMenu->arrangeAndClear(); // update menu height
}

LLToggleableMenu* LLOutfitListGearMenuBase::getMenu()
{
    return mMenu;
}
const LLUUID& LLOutfitListGearMenuBase::getSelectedOutfitID()
{
    return mOutfitList->getSelectedOutfitUUID();
}

LLViewerInventoryCategory* LLOutfitListGearMenuBase::getSelectedOutfit()
{
    const LLUUID& selected_outfit_id = getSelectedOutfitID();
    if (selected_outfit_id.isNull())
    {
        return NULL;
    }

    LLViewerInventoryCategory* cat = gInventory.getCategory(selected_outfit_id);
    return cat;
}

void LLOutfitListGearMenuBase::onWear()
{
    LLViewerInventoryCategory* selected_outfit = getSelectedOutfit();
    if (selected_outfit)
    {
        LLAppearanceMgr::instance().wearInventoryCategory(
            selected_outfit, /*copy=*/ false, /*append=*/ false);
    }
}

void LLOutfitListGearMenuBase::onAdd()
{
    const LLUUID& selected_id = getSelectedOutfitID();

    if (selected_id.notNull())
    {
        LLAppearanceMgr::getInstance()->addCategoryToCurrentOutfit(selected_id);
    }
}

void LLOutfitListGearMenuBase::onSave()
{
    const LLUUID &selected_id = getSelectedOutfitID();
    LLNotificationsUtil::add("ConfirmOverwriteOutfit", LLSD(), LLSD(),
        [selected_id](const LLSD &notif, const LLSD &resp)
    {
        S32 opt = LLNotificationsUtil::getSelectedOption(notif, resp);
        if (opt == 0)
        {
            LLAppearanceMgr::getInstance()->onOutfitFolderCreated(selected_id, true);
        }
    });
}

void LLOutfitListGearMenuBase::onTakeOff()
{
    // Take off selected outfit.
    const LLUUID& selected_outfit_id = getSelectedOutfitID();
    if (selected_outfit_id.notNull())
    {
        LLAppearanceMgr::instance().takeOffOutfit(selected_outfit_id);
    }
}

void LLOutfitListGearMenuBase::onRename()
{
    const LLUUID& selected_outfit_id = getSelectedOutfitID();
    if (selected_outfit_id.notNull())
    {
        LLAppearanceMgr::instance().renameOutfit(selected_outfit_id);
    }
}

void LLOutfitListGearMenuBase::onCreate(const LLSD& data)
{
    LLWearableType::EType type = LLWearableType::getInstance()->typeNameToType(data.asString());
    if (type == LLWearableType::WT_NONE)
    {
        LL_WARNS() << "Invalid wearable type" << LL_ENDL;
        return;
    }

    LLAgentWearables::createWearable(type, true);
}

void LLOutfitListGearMenuBase::onNewFolder()
{
    mOutfitList->createFolder(mOutfitList->getCurrentFolderID());
}

void LLOutfitListGearMenuBase::onRenameFolder()
{
    mOutfitList->renameFolder(mOutfitList->getSelectedFolderUUID());
}

void LLOutfitListGearMenuBase::onDeleteFolder()
{
    mOutfitList->removeFolder(mOutfitList->getSelectedFolderUUID());
}

void LLOutfitListGearMenuBase::onGroupByPrefix()
{
    mOutfitList->groupOutfitsByPrefix();
}

bool LLOutfitListGearMenuBase::onEnable(LLSD::String param)
{
    // Handle the "Wear - Replace Current Outfit" menu option specially
    // because LLOutfitList::isActionEnabled() checks whether it's allowed
    // to wear selected outfit OR selected items, while we're only
    // interested in the outfit (STORM-183).
    if ("wear" == param)
    {
        return LLAppearanceMgr::instance().getCanReplaceCOF(mOutfitList->getSelectedOutfitUUID());
    }

    return mOutfitList->isActionEnabled(param);
}

bool LLOutfitListGearMenuBase::onVisible(LLSD::String param)
{
    if ("new_folder" == param || "group_by_prefix" == param)
    {
        return true;
    }
    if ("folder_selected" == param)
    {
        return getSelectedOutfitID().isNull() && mOutfitList->getSelectedFolderUUID().notNull();
    }

    const LLUUID& selected_outfit_id = getSelectedOutfitID();
    if (selected_outfit_id.isNull()) // no selection or invalid outfit selected
    {
        return false;
    }
    else if ("favorites_add" == param)
    {
        LLViewerInventoryCategory* cat = gInventory.getCategory(selected_outfit_id);
        return cat && !cat->getIsFavorite();
    }
    else if ("favorites_remove" == param)
    {
        LLViewerInventoryCategory* cat = gInventory.getCategory(selected_outfit_id);
        return cat && cat->getIsFavorite();
    }

    return true;
}

void LLOutfitListGearMenuBase::onThumbnail()
{
    const LLUUID& selected_outfit_id = getSelectedOutfitID();
    LLSD data(selected_outfit_id);
    LLFloaterReg::showInstance("change_item_thumbnail", data);
}

void LLOutfitListGearMenuBase::onFavorite()
{
    const LLUUID& selected_outfit_id = getSelectedOutfitID();
    toggle_favorite(selected_outfit_id);
}

void LLOutfitListGearMenuBase::onChangeSortOrder()
{

}

LLOutfitListGearMenu::LLOutfitListGearMenu(LLOutfitListBase* olist)
    : LLOutfitListGearMenuBase(olist)
{}

LLOutfitListGearMenu::~LLOutfitListGearMenu()
{}

void LLOutfitListGearMenu::onUpdateItemsVisibility()
{
    if (!mMenu) return;
    mMenu->setItemVisible("thumbnail", getSelectedOutfitID().notNull());
    mMenu->setItemVisible("favorite", getSelectedOutfitID().notNull());
    mMenu->setItemVisible("sepatator3", false);
    mMenu->setItemVisible("sort_folders_by_name", false);
    LLOutfitListGearMenuBase::onUpdateItemsVisibility();
}

//////////////////// LLOutfitListSortMenu ////////////////////

LLOutfitListSortMenu::LLOutfitListSortMenu(LLOutfitListBase* parent_panel)
    : mPanelHandle(parent_panel->getHandle())
{
    LLUICtrl::CommitCallbackRegistry::ScopedRegistrar registrar;
    LLUICtrl::EnableCallbackRegistry::ScopedRegistrar enable_registrar;

    registrar.add("Sort.Collapse", boost::bind(&LLOutfitListBase::onCollapseAllFolders, parent_panel));
    registrar.add("Sort.Expand", boost::bind(&LLOutfitListBase::onExpandAllFolders, parent_panel));
    registrar.add("Sort.OnSort", boost::bind(&LLOutfitListBase::onChangeSortOrder, parent_panel, _2));
    enable_registrar.add("Sort.OnEnable", boost::bind(&LLOutfitListSortMenu::onEnable, this, _2));

    mMenu = LLUICtrlFactory::getInstance()->createFromFile<LLToggleableMenu>(
        "menu_outfit_list_sort.xml", gMenuHolder, LLViewerMenuHolderGL::child_registry_t::instance());
    llassert(mMenu);
}


LLToggleableMenu* LLOutfitListSortMenu::getMenu()
{
    return mMenu;
}

void LLOutfitListSortMenu::updateItemsVisibility()
{
    onUpdateItemsVisibility();
}

void LLOutfitListSortMenu::onUpdateItemsVisibility()
{
    if (!mMenu) return;
    mMenu->setItemVisible("expand", true);
    mMenu->setItemVisible("collapse", true);
    mMenu->setItemVisible("sort_favorites_to_top", true);
    mMenu->setItemVisible("show_entire_outfit_in_search", true);
}

bool LLOutfitListSortMenu::onEnable(LLSD::String param)
{
    if ("favorites_to_top" == param)
    {
        static LLCachedControl<S32> sort_order(gSavedSettings, "OutfitListSortOrder", 0);
        return sort_order == 1;
    }
    else if ("show_entire_outfit" == param)
    {
        static LLCachedControl<bool> filter_mode(gSavedSettings, "OutfitListFilterFullList", 0);
        return filter_mode;
    }

    return false;
}


//////////////////// LLOutfitAccordionCtrlTab ////////////////////

LLUIImage* LLOutfitAccordionCtrlTab::sFavoriteIcon;
LLUIColor LLOutfitAccordionCtrlTab::sFgColor;

void LLOutfitAccordionCtrlTab::draw()
{
    LLAccordionCtrlTab::draw();
    drawFavoriteIcon();
}

bool LLOutfitAccordionCtrlTab::handleToolTip(S32 x, S32 y, MASK mask)
{
    if (y >= getLocalRect().getHeight() - getHeaderHeight())
    {
        LLSD params;
        params["inv_type"] = LLInventoryType::IT_CATEGORY;
        LLViewerInventoryCategory* cat = gInventory.getCategory(mFolderID);
        if (cat)
        {
            params["thumbnail_id"] = cat->getThumbnailUUID();
        }
        // else consider returning
        params["item_id"] = mFolderID;

        LLToolTipMgr::instance().show(LLToolTip::Params()
                                    .message(getToolTip())
                                    .sticky_rect(calcScreenRect())
                                    .delay_time(LLView::getTooltipTimeout())
                                    .create_callback(boost::bind(&LLInspectTextureUtil::createInventoryToolTip, _1))
                                    .create_params(params));
        return true;
    }

    return LLAccordionCtrlTab::handleToolTip(x, y, mask);
}

bool LLOutfitAccordionCtrlTab::isInHeader(S32 y)
{
    return getHeaderVisible() && y >= getLocalRect().getHeight() - getHeaderHeight();
}

bool LLOutfitAccordionCtrlTab::handleMouseDown(S32 x, S32 y, MASK mask)
{
    if (!isInHeader(y) || !getCollapsible() || !canOpenClose())
    {
        return LLAccordionCtrlTab::handleMouseDown(x, y, mask);
    }

    // Open/close waits for mouse up, so dragging the header doesn't toggle the tab.
    LLUICtrl* header = findChild<LLUICtrl>("dd_header");
    if (header)
    {
        header->setFocus(true);
    }
    gFocusMgr.setMouseCapture(this);
    S32 screen_x, screen_y;
    localPointToScreen(x, y, &screen_x, &screen_y);
    LLToolDragAndDrop::getInstance()->setDragStart(screen_x, screen_y);
    mMouseDownX = x;
    mMouseDownY = y;
    mClickPending = true;
    return true;
}

bool LLOutfitAccordionCtrlTab::handleMouseUp(S32 x, S32 y, MASK mask)
{
    if (hasMouseCapture())
    {
        gFocusMgr.setMouseCapture(NULL);
        if (mClickPending)
        {
            mClickPending = false;
            return LLAccordionCtrlTab::handleMouseDown(mMouseDownX, mMouseDownY, mask);
        }
        return true;
    }
    return LLAccordionCtrlTab::handleMouseUp(x, y, mask);
}

bool LLOutfitAccordionCtrlTab::handleHover(S32 x, S32 y, MASK mask)
{
    if (hasMouseCapture() && mClickPending)
    {
        S32 screen_x, screen_y;
        localPointToScreen(x, y, &screen_x, &screen_y);
        if (LLToolDragAndDrop::getInstance()->isOverThreshold(screen_x, screen_y) && mFolderID.notNull())
        {
            mClickPending = false;
            LLToolDragAndDrop::getInstance()->beginDrag(DAD_CATEGORY, mFolderID, LLToolDragAndDrop::SOURCE_AGENT);
            return LLToolDragAndDrop::getInstance()->handleHover(x, y, mask);
        }
        return true;
    }
    return LLAccordionCtrlTab::handleHover(x, y, mask);
}

void LLOutfitAccordionCtrlTab::setDisplayChildren(bool display)
{
    if (mIsFolder)
    {
        // Folder headers have no body of their own; "open" shows their tree children instead.
        mFolderOpen = display;
        LLAccordionCtrlTab::setDisplayChildren(false);
        return;
    }
    LLAccordionCtrlTab::setDisplayChildren(display);
}

bool LLOutfitAccordionCtrlTab::getDisplayChildren() const
{
    return mIsFolder ? mFolderOpen : LLAccordionCtrlTab::getDisplayChildren();
}

void LLOutfitAccordionCtrlTab::setFavorite(bool is_favorite)
{
    mIsFavorite = is_favorite;
    updateTitleColor();
}

void LLOutfitAccordionCtrlTab::setOutfitSelected(bool val)
{
    if (mIsSelected == val)
    {
        return;
    }
    mIsSelected = val;
    setTitleFontStyle(mIsSelected ? "BOLD" : "NORMAL");
    updateTitleColor();
    }

void LLOutfitAccordionCtrlTab::updateTitleColor()
    {
        static LLUICachedControl<bool> highlight_color("InventoryFavoritesColorText", true);
        if (mIsFavorite && highlight_color())
        {
            setTitleColor(LLUIColorTable::instance().getColor("InventoryFavoriteColor"));
        }
    else if (mIsSelected)
    {
        setTitleColor(LLUIColorTable::instance().getColor("SelectedOutfitTextColor"));
    }
        else
        {
            setTitleColor(LLUIColorTable::instance().getColor("AccordionHeaderTextColor"));
        }
    }

void LLOutfitAccordionCtrlTab::drawFavoriteIcon()
{
    if (!mIsFavorite)
    {
        return;
    }
    static LLUICachedControl<bool> draw_star("InventoryFavoritesUseStar", true);
    if (!draw_star)
    {
        return;
    }

    const S32 PAD = 2;
    const S32 image_size = 18;

    gl_draw_scaled_image(
        getRect().getWidth() - image_size - PAD, getRect().getHeight() - image_size - PAD,
        image_size, image_size, sFavoriteIcon->getImage(), sFgColor);
}
// EOF
