/**
 * @file lloutfitslist.h
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

#ifndef LL_LLOUTFITSLIST_H
#define LL_LLOUTFITSLIST_H

#include "llaccordionctrl.h"
#include "llpanel.h"

// newview
#include "llaccordionctrltab.h"
#include "llinventorymodel.h"
#include "lllistcontextmenu.h"
#include "llpanelappearancetab.h"
#include "lltoggleablemenu.h"
#include "llviewermenu.h"

class LLAccordionCtrlTab;
class LLInventoryCategoriesObserver;
class LLOutfitListGearMenuBase;
class LLOutfitListSortMenuBase;
class LLWearableItemsList;
class LLListContextMenu;


/**
 * @class LLOutfitTabNameComparator
 *
 * Comparator of outfit tabs.
 */
class LLOutfitTabNameComparator : public LLAccordionCtrl::LLTabComparator
{
    LOG_CLASS(LLOutfitTabNameComparator);

public:
    LLOutfitTabNameComparator() {};
    virtual ~LLOutfitTabNameComparator() {};

    /*virtual*/ bool compare(const LLAccordionCtrlTab* tab1, const LLAccordionCtrlTab* tab2) const;
};

class LLOutfitTabFavComparator : public LLAccordionCtrl::LLTabComparator
{
    LOG_CLASS(LLOutfitTabFavComparator);

public:
    LLOutfitTabFavComparator() {};
    virtual ~LLOutfitTabFavComparator() {};

    /*virtual*/ bool compare(const LLAccordionCtrlTab* tab1, const LLAccordionCtrlTab* tab2) const;
};

// One plain folder of the My Outfits tree, as listed by LLOutfitListBase::getFolderTree().
struct LLOutfitFolderEntry
{
    LLUUID      mID;
    std::string mPath;  // "Casual > Summer"; empty for the My Outfits root
    S32         mDepth; // 0 for the root
};

class LLOutfitListBase : public LLPanelAppearanceTab
{
public:
    typedef boost::function<void(const LLUUID&)> selection_change_callback_t;
    typedef boost::signals2::signal<void(const LLUUID&)> selection_change_signal_t;

    LLOutfitListBase();
    virtual ~LLOutfitListBase();

    /*virtual*/ bool postBuild();
    /*virtual*/ void onOpen(const LLSD& info);

    void refreshList(const LLUUID& category_id);
    void computeDifference(const LLInventoryModel::cat_array_t& vcats, uuid_vec_t& vadded, uuid_vec_t& vremoved);
    // highlights currently worn outfit in list and unhighlights previously worn
    void highlightBaseOutfit();
    void ChangeOutfitSelection(LLWearableItemsList* list, const LLUUID& category_id);


    virtual void getCurrentCategories(uuid_vec_t& vcur) = 0;
    virtual void updateAddedCategory(LLUUID cat_id) = 0;
    virtual void updateRemovedCategory(LLUUID cat_id) = 0;
    virtual void updateChangedCategoryName(LLViewerInventoryCategory *cat, std::string name) = 0;
    virtual void sortOutfits();

    void removeSelected();
    void setSelectedOutfitByUUID(const LLUUID& outfit_uuid);
    const LLUUID& getSelectedOutfitUUID() const { return mSelectedOutfitUUID; }

    // Folders of the My Outfits tree (plain categories that are not outfits).
    const LLUUID& getSelectedFolderUUID() const { return mSelectedFolderUUID; }
    void selectFolder(const LLUUID& folder_id);
    // Folder that new folders and "Save As" outfits go into by default.
    virtual LLUUID getCurrentFolderID() const;

    void createFolder(const LLUUID& parent_id);
    void renameFolder(const LLUUID& folder_id);
    void removeFolder(const LLUUID& folder_id);
    void groupOutfitsByPrefix();

    // Accepts outfits and folders from the My Outfits tree into dest_id (root or a folder).
    static bool handleTreeDrop(const LLUUID& dest_id, bool drop, EDragAndDropType cargo_type, void* cargo_data,
                               EAcceptance* accept, std::string& tooltip_msg);

    static bool isOutfitFolder(const LLViewerInventoryCategory* cat);
    static bool isTreeFolder(const LLUUID& cat_id);
    static void getFolderTree(std::vector<LLOutfitFolderEntry>& folders);
    // Names of the folders between My Outfits and cat_id, joined with " > ".
    static std::string getFolderPath(const LLUUID& cat_id);
    boost::signals2::connection setSelectionChangeCallback(selection_change_callback_t cb);
    void outfitRightClickCallBack(LLUICtrl* ctrl, S32 x, S32 y, const LLUUID& cat_id);
    void folderRightClickCallBack(LLUICtrl* ctrl, S32 x, S32 y, const LLUUID& folder_id);

    void onAction(const LLSD& userdata);
    virtual bool isActionEnabled(const LLSD& userdata);
    virtual void performAction(std::string action);
    virtual bool hasItemSelected() = 0;
    virtual bool canWearSelected() = 0;

    virtual void deselectOutfit(const LLUUID& category_id);

    void signalSelectionOutfitUUID(const LLUUID& category_id);

    void collapseAllFolders();
    virtual void onCollapseAllFolders() = 0;

    void expandAllFolders();
    virtual void onExpandAllFolders() = 0;

    virtual bool getHasExpandableFolders() = 0;

    virtual void onChangeSortOrder(const LLSD& userdata) = 0;

    virtual void updateMenuItemsVisibility();
    virtual LLToggleableMenu* getGearMenu();
    virtual bool getTrashMenuVisible() { return true; };

protected:
    // Called once an idle refresh has applied all additions and removals; picks up moves and renames.
    virtual void onTreeRefreshed() {}
    virtual void onSelectFolder(const LLUUID& folder_id) = 0;
    virtual bool hasFolderEntry(const LLUUID& cat_id) const = 0;
    static void onFolderRemovalConfirmation(const LLSD& notification, const LLSD& response, const LLUUID& folder_id);
    void collectTree(const LLUUID& parent_id, LLInventoryModel::cat_array_t& cats);

    void observerCallback(const LLUUID& category_id);
    virtual LLOutfitListGearMenuBase* createGearMenu() = 0;
    virtual void onHighlightBaseOutfit(LLUUID base_id, LLUUID prev_id) = 0;
    virtual void onSetSelectedOutfitByUUID(const LLUUID& outfit_uuid) = 0;
    virtual void onOutfitRightClick(LLUICtrl* ctrl, S32 x, S32 y, const LLUUID& cat_id) = 0;
    void onOutfitsRemovalConfirmation(const LLSD& notification, const LLSD& response);
    virtual void onChangeOutfitSelection(LLWearableItemsList* list, const LLUUID& category_id) = 0;

    static void onIdle(void* userdata);
    void onIdleRefreshList();

    struct
    {
        LLUUID                      CategoryUUID;
        uuid_vec_t                  Added;
        uuid_vec_t                  Removed;
        uuid_vec_t::const_iterator  AddedIterator;
        uuid_vec_t::const_iterator  RemovedIterator;
    } mRefreshListState;
    std::set<LLUUID>                mChangedItems;

    bool                            mIsInitialized;
    LLInventoryCategoriesObserver*  mCategoriesObserver;
    LLUUID                          mSelectedOutfitUUID;
    LLUUID                          mSelectedFolderUUID;
    // Folder created from this panel, selected once its entry shows up.
    LLUUID                          mPendingFolderSelection;
    // id of currently highlited outfit
    LLUUID                          mHighlightedOutfitUUID;
    selection_change_signal_t       mSelectionChangeSignal;
    LLListContextMenu*              mOutfitMenu;
    LLListContextMenu*              mFolderMenu;
    LLOutfitListGearMenuBase*       mGearMenu;
    boost::signals2::connection     mGearMenuConnection;
};

//////////////////////////////////////////////////////////////////////////

class LLOutfitContextMenu : public LLListContextMenu
{
public:

    LLOutfitContextMenu(LLOutfitListBase* outfit_list)
        : LLListContextMenu(),
        mOutfitList(outfit_list)
    {}
protected:
    /* virtual */ LLContextMenu* createMenu();

    bool onEnable(LLSD::String param);
    bool onVisible(LLSD::String param);

    static void editOutfit();

    static void renameOutfit(const LLUUID& outfit_cat_id);

    void onThumbnail(const LLUUID &outfit_cat_id);
    void onFavorite(const LLUUID& outfit_cat_id);
    void onSave(const LLUUID &outfit_cat_id);

private:
    LLOutfitListBase*   mOutfitList;
};

class LLOutfitFolderContextMenu : public LLListContextMenu
{
public:
    LLOutfitFolderContextMenu(LLOutfitListBase* outfit_list)
        : LLListContextMenu(),
        mOutfitList(outfit_list)
    {}
protected:
    /* virtual */ LLContextMenu* createMenu();
    bool onEnable(LLSD::String param);

private:
    LLOutfitListBase*   mOutfitList;
};

class LLOutfitListGearMenuBase
{
public:
    LLOutfitListGearMenuBase(LLOutfitListBase* olist);
    virtual ~LLOutfitListGearMenuBase();

    void updateItemsVisibility();

    LLToggleableMenu* getMenu();

protected:
    virtual void onUpdateItemsVisibility();
    virtual void onThumbnail();
    virtual void onFavorite();
    virtual void onChangeSortOrder();

    const LLUUID& getSelectedOutfitID();

    LLOutfitListBase*       mOutfitList;
    LLToggleableMenu*       mMenu;
private:

    LLViewerInventoryCategory* getSelectedOutfit();

    void onWear();
    void onAdd();
    void onTakeOff();
    void onRename();
    void onSave();
    void onCreate(const LLSD& data);
    void onNewFolder();
    void onRenameFolder();
    void onDeleteFolder();
    void onGroupByPrefix();
    bool onEnable(LLSD::String param);
    bool onVisible(LLSD::String param);
};

class LLOutfitListSortMenu
{
public:
    LLOutfitListSortMenu(LLOutfitListBase* parent_panel);

    LLToggleableMenu* getMenu();
    void updateItemsVisibility();

private:
    void onUpdateItemsVisibility();
    bool onEnable(LLSD::String param);

    LLToggleableMenu* mMenu;
    LLHandle<LLPanel> mPanelHandle;
};


class LLOutfitListGearMenu : public LLOutfitListGearMenuBase
{
public:
    LLOutfitListGearMenu(LLOutfitListBase* olist);
    virtual ~LLOutfitListGearMenu();

protected:
    /*virtual*/ void onUpdateItemsVisibility();
};

class LLOutfitAccordionCtrlTab : public LLAccordionCtrlTab
{
public:
    struct Params : public LLInitParam::Block<Params, LLAccordionCtrlTab::Params>
    {
        Optional<LLUUID> cat_id;
        Params() : cat_id("cat_id") {}
    };

    // Per tree level: folders before outfits, then favorite, then name.
    struct SortKey
    {
        bool        mIsFolder;
        bool        mIsFavorite;
        std::string mName;
        LLUUID      mID;    // keeps same-named folders from interleaving
    };
    typedef std::vector<SortKey> sort_path_t;

    virtual void draw();
    virtual bool handleToolTip(S32 x, S32 y, MASK mask);
    virtual bool handleMouseDown(S32 x, S32 y, MASK mask);
    virtual bool handleMouseUp(S32 x, S32 y, MASK mask);
    virtual bool handleHover(S32 x, S32 y, MASK mask);
    virtual void setDisplayChildren(bool display);
    virtual bool getDisplayChildren() const;

    void setFavorite(bool is_favorite);
    bool getFavorite() const { return mIsFavorite; }
    void setOutfitSelected(bool val);
    bool getOutfitSelected() const { return mIsSelected; }

    void setIsFolder(bool is_folder) { mIsFolder = is_folder; }
    bool isFolder() const { return mIsFolder; }
    const LLUUID& getFolderID() const { return mFolderID; }
    void setSortPath(const sort_path_t& path) { mSortPath = path; }
    const sort_path_t& getSortPath() const { return mSortPath; }

    static LLUIImage* sFavoriteIcon;
    static LLUIColor sFgColor;

 protected:
    LLOutfitAccordionCtrlTab(const LLOutfitAccordionCtrlTab::Params &p)
        : LLAccordionCtrlTab(p),
          mFolderID(p.cat_id)
    {}
    friend class LLUICtrlFactory;

    void updateTitleColor();
    void drawFavoriteIcon();

    bool isInHeader(S32 y);

    LLUUID mFolderID;
    sort_path_t mSortPath;
    bool mIsFavorite = false;
    bool mIsSelected = false;
    bool mIsFolder = false;
    bool mFolderOpen = false;
    bool mClickPending = false;
    S32  mMouseDownX = 0;
    S32  mMouseDownY = 0;
};
  /**
 * @class LLOutfitsList
 *
 * A list of agents's outfits from "My Outfits" inventory category
 * which displays each outfit in an accordion tab with a flat list
 * of items inside it.
 *
 * Starts fetching necessary inventory content on first opening.
 */
class LLOutfitsList : public LLOutfitListBase
{
public:

    LLOutfitsList();
    virtual ~LLOutfitsList();

    /*virtual*/ bool postBuild();
    void initComparator();

    /*virtual*/ void onOpen(const LLSD& info);


    //virtual void refreshList(const LLUUID& category_id);

    /*virtual*/ void updateAddedCategory(LLUUID cat_id);
    /*virtual*/ void updateRemovedCategory(LLUUID cat_id);

    // highlits currently worn outfit tab text and unhighlights previously worn
    /*virtual*/ void onHighlightBaseOutfit(LLUUID base_id, LLUUID prev_id);

    //void performAction(std::string action);


    /*virtual*/ void onFilterSubStringChanged(const std::string& new_string, const std::string& old_string);

    /*virtual*/ void getSelectedItemsUUIDs(uuid_vec_t& selected_uuids) const;

    // Collects selected items from all selected lists and wears them(if possible- adds, else replaces)
    void wearSelectedItems();

    /**
     * Returns true if there is a selection inside currently selected outfit
     */
    /*virtual*/ bool hasItemSelected();

    /**
    Collapses all outfit accordions.
    */
    /*virtual*/ void onCollapseAllFolders();
    /**
    Expands all outfit accordions.
    */
    void onExpandAllFolders();

    /*virtual*/ bool getHasExpandableFolders() { return true; }

    /*virtual*/ void onChangeSortOrder(const LLSD& userdata);
    virtual LLToggleableMenu* getSortMenu();
    void updateMenuItemsVisibility();

    /*virtual*/ bool handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop, EDragAndDropType cargo_type,
                                       void* cargo_data, EAcceptance* accept, std::string& tooltip_msg);

protected:
    LLOutfitListGearMenuBase* createGearMenu();
    /*virtual*/ void onTreeRefreshed();
    /*virtual*/ void onSelectFolder(const LLUUID& folder_id);
    /*virtual*/ bool hasFolderEntry(const LLUUID& cat_id) const;

private:
    void addFolderTab(LLViewerInventoryCategory* cat);
    void onFolderToggled(const LLUUID& folder_id);
    // Applies indent, sort keys, titles and collapse/filter visibility to every tab.
    void updateTreeLayout(bool sort = true);
    void updateTabTreeState(LLOutfitAccordionCtrlTab* tab);
    bool isHiddenByCollapsedFolder(const LLUUID& cat_id) const;
    void expandAncestors(const LLUUID& cat_id);
    LLOutfitAccordionCtrlTab* findTab(const LLUUID& cat_id) const;

    /**
     * Wrapper for LLCommonUtils::computeDifference. @see LLCommonUtils::computeDifference
     */
    //void computeDifference(const LLInventoryModel::cat_array_t& vcats, uuid_vec_t& vadded, uuid_vec_t& vremoved);

    void getCurrentCategories(uuid_vec_t& vcur);

    /**
     * Updates tab displaying outfit identified by category_id.
     */
    /*virtual*/ void updateChangedCategoryName(LLViewerInventoryCategory *cat, std::string name);

    /*virtual*/ void sortOutfits();

    /*virtual*/ void onSetSelectedOutfitByUUID(const LLUUID& outfit_uuid);

    /**
     * Resets previous selection and stores newly selected list and outfit id.
     */
    /*virtual*/ void onChangeOutfitSelection(LLWearableItemsList* list, const LLUUID& category_id);

    /**
     *Resets items selection inside outfit
     */
    void resetItemSelection(LLWearableItemsList* list, const LLUUID& category_id);

    /**
     * Removes the outfit from selection.
     */
    /*virtual*/ void deselectOutfit(const LLUUID& category_id);

    /**
     * Try restoring selection for a temporary hidden tab.
     *
     * A tab may be hidden if it doesn't match current filter.
     */
    void restoreOutfitSelection(LLAccordionCtrlTab* tab, const LLUUID& category_id);

    /**
     * Called upon list refresh event to update tab visibility depending on
     * the results of applying filter to the title and list items of the tab.
     */
    void onRefreshComplete(LLUICtrl* ctrl);

    /**
     * Applies filter to the given tab
     *
     * @see applyFilter()
     */
    void applyFilterToTab(const LLUUID& category_id, LLAccordionCtrlTab* tab, const std::string& filter_substring);

    /**
     * Returns true if all selected items can be worn.
     */
    bool canWearSelected();

    void onWearableItemsListRightClick(LLUICtrl* ctrl, S32 x, S32 y);
    void onCOFChanged();

    void onListSelectionChange(LLUICtrl* ctrl);

    /*virtual*/ void onOutfitRightClick(LLUICtrl* ctrl, S32 x, S32 y, const LLUUID& cat_id);

    static void onOutfitRename(const LLSD& notification, const LLSD& response);

    void handleInvFavColorChange();

    //LLInventoryCategoriesObserver*    mCategoriesObserver;

    LLAccordionCtrl*                mAccordion;
    LLPanel*                        mListCommands;

    typedef std::map<LLUUID, LLWearableItemsList*>      wearables_lists_map_t;
    typedef wearables_lists_map_t::value_type           wearables_lists_map_value_t;
    wearables_lists_map_t           mSelectedListsMap;

    typedef std::map<LLUUID, LLAccordionCtrlTab*>       outfits_map_t;
    typedef outfits_map_t::value_type                   outfits_map_value_t;
    outfits_map_t                   mOutfitsMap;

    // Folder header tabs of the My Outfits tree.
    typedef std::map<LLUUID, LLOutfitAccordionCtrlTab*> folders_map_t;
    folders_map_t                   mFoldersMap;

    // IDs of original items which are worn and linked in COF.
    // Used to monitor COF changes for updating items worn state. See EXT-8636.
    uuid_vec_t                      mCOFLinkedItems;

    LLOutfitListSortMenu*         mSortMenu;

    //bool                          mIsInitialized;
    /**
     * True if there is a selection inside currently selected outfit
     */
    bool                            mItemSelected;

    boost::signals2::connection                   mSavedSettingInvFavColor;
};

#endif //LL_LLOUTFITSLIST_H
