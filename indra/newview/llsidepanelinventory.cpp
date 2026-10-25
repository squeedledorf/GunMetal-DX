/**
 * @file LLSidepanelInventory.cpp
 * @brief Side Bar "Inventory" panel
 *
 * $LicenseInfo:firstyear=2009&license=viewerlgpl$
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
#include "llsidepanelinventory.h"

#include "llagent.h"
#include "llappearancemgr.h"
#include "llappviewer.h"
#include "llavataractions.h"
#include "llbutton.h"
#include "llcallbacklist.h"
#include "lldate.h"
#include "llfirstuse.h"
#include "llfloaterreg.h"
#include "llfloatersidepanelcontainer.h"
#include "llfoldertype.h"
#include "llfolderview.h"
#include "llinventorybridge.h"
#include "llinventoryfunctions.h"
#include "llinventorymodel.h"
#include "llinventorymodelbackgroundfetch.h"
#include "llinventoryobserver.h"
#include "llinventorypanel.h"
#include "lllayoutstack.h"
#include "lloutfitobserver.h"
#include "llpanelmaininventory.h"
#include "llselectmgr.h"
#include "llsidepaneliteminfo.h"
#include "llsidepaneltaskinfo.h"
#include "llstartup.h"
#include "llstring.h"
#include "lltabcontainer.h"
#include "lltextbox.h"
#include "lltrans.h"
#include "llviewermedia.h"
#include "llviewernetwork.h"
#include "llweb.h"

static LLPanelInjector<LLSidepanelInventory> t_inventory("sidepanel_inventory");

//
// Received Items (#489)
//
// The inbox used to have its own pane under the inventory tree, which also did the inbox
// bookkeeping. It is an ordinary folder of the tree now; what is left, once per session after
// login: put stray inbox folders back together (consolidateForType, as before), fetch its whole
// contents (creation dates for the "new" count on its label) and refresh that label whenever
// the folder changes. If the account has no inbox yet, wait for the server to create it.
//
namespace
{
bool sLoginCompleted = false;
LLInventoryCategoriesObserver* sInboxObserver = nullptr;

void watch_inbox(const LLUUID& inbox_id)
{
    if (sInboxObserver || inbox_id.isNull())
    {
        return;
    }
    gInventory.consolidateForType(inbox_id, LLFolderType::FT_INBOX);

    sInboxObserver = new LLInventoryCategoriesObserver();
    gInventory.addObserver(sInboxObserver);
    sInboxObserver->addCategory(inbox_id, [inbox_id]()
    {
        // Fetch new deliveries in full so their creation dates are known, and recount.
        LLInventoryModelBackgroundFetch::instance().start(inbox_id);
        relabel_inbox_folder();
    });
    LLInventoryModelBackgroundFetch::instance().start(inbox_id);
    relabel_inbox_folder();
}

class LLInboxAddedObserver : public LLInventoryCategoryAddedObserver
{
public:
    void done() override
    {
        for (LLViewerInventoryCategory* added_category : mAddedCategories)
        {
            if (added_category->getPreferredType() == LLFolderType::FT_INBOX)
            {
                const LLUUID inbox_id = added_category->getUUID();
                // Not from inside the observer notification: watch_inbox() adds an observer.
                doOnIdleOneTime([inbox_id]() { watch_inbox(inbox_id); });
            }
        }
    }
};
LLInboxAddedObserver* sInboxAddedObserver = nullptr;

void start_inbox_watch()
{
    if (sLoginCompleted)
    {
        return;
    }
    sLoginCompleted = true;
    const LLUUID inbox_id = gInventory.findCategoryUUIDForType(LLFolderType::FT_INBOX);
    if (inbox_id.notNull())
    {
        watch_inbox(inbox_id);
    }
    else if (!sInboxAddedObserver)
    {
        sInboxAddedObserver = new LLInboxAddedObserver();
        gInventory.addObserver(sInboxAddedObserver);
    }
}
}

//
// Implementation
//

LLSidepanelInventory::LLSidepanelInventory()
    : LLPanel()
    , mInventoryPanel(NULL)
    , mPanelMainInventory(NULL)
{
    //buildFromFile( "panel_inventory.xml"); // Called from LLRegisterPanelClass::defaultPanelClassBuilder()
}

LLSidepanelInventory::~LLSidepanelInventory()
{
}

bool LLSidepanelInventory::postBuild()
{
    // UI elements from inventory panel
    {
        mInventoryPanel = getChild<LLPanel>("sidepanel_inventory_panel");

        mPanelMainInventory = mInventoryPanel->getChild<LLPanelMainInventory>("panel_main_inventory");
        mPanelMainInventory->setSelectCallback(boost::bind(&LLSidepanelInventory::onSelectionChange, this, _1, _2));
        mPanelMainInventory->setParentSidepanel(this);
    }

    LLFloater *floater = dynamic_cast<LLFloater*>(getParent());
    if (floater && floater->getKey().isUndefined() && !sLoginCompleted)
    {
        // Prefill inventory for primary inventory floater
        // Other floaters should fill on visibility change
        //
        // see get_instance_num();
        // Primary inventory floater will have undefined key
        initInventoryViews();
    }

    // Received Items bookkeeping, once per session (see above).
    if (!sLoginCompleted)
    {
        static bool requested = false;
        if (LLStartUp::getStartupState() >= STATE_STARTED)
        {
            start_inbox_watch();    // this window was first built after login
        }
        else if (!requested)
        {
            requested = true;
            LLAppViewer::instance()->setOnLoginCompletedCallback(&start_inbox_watch);
        }
    }

    return true;
}

void LLSidepanelInventory::onOpen(const LLSD& key)
{
    LLFirstUse::newInventory(false);
    mPanelMainInventory->setFocusOnFilterEditor();

    gAgent.showLatestFeatureNotification("inventory");
}

void LLSidepanelInventory::performActionOnSelection(const std::string &action)
{
    LLFolderViewItem* current_item = mPanelMainInventory->getActivePanel()->getRootFolder()->getCurSelectedItem();
    if (!current_item)
    {
        return;
    }

    static_cast<LLFolderViewModelItemInventory*>(current_item->getViewModelItem())->performAction(mPanelMainInventory->getActivePanel()->getModel(), action);
}

void LLSidepanelInventory::onBackButtonClicked()
{
    showInventoryPanel();
}

void LLSidepanelInventory::onSelectionChange(const std::deque<LLFolderViewItem*> &items, bool user_action)
{

}

void LLSidepanelInventory::showInventoryPanel()
{
    mInventoryPanel->setVisible(true);
}

void LLSidepanelInventory::initInventoryViews()
{
    mPanelMainInventory->initInventoryViews();
}

bool LLSidepanelInventory::canShare()
{
    // Avoid flicker in the Recent tab while inventory is being loaded.
    if (!mPanelMainInventory || !mPanelMainInventory->getActivePanel()->getRootFolder()->hasVisibleChildren())
    {
        return false;
    }

    return LLAvatarActions::canShareSelectedItems(mPanelMainInventory->getActivePanel());
}


bool LLSidepanelInventory::canWearSelected()
{

    std::set<LLUUID> selected_uuids = LLAvatarActions::getInventorySelectedUUIDs();

    if (selected_uuids.empty())
        return false;

    for (std::set<LLUUID>::const_iterator it = selected_uuids.begin();
        it != selected_uuids.end();
        ++it)
    {
        if (!get_can_item_be_worn(*it)) return false;
    }

    return true;
}

LLInventoryItem *LLSidepanelInventory::getSelectedItem()
{
    LLFolderView* root = mPanelMainInventory->getActivePanel()->getRootFolder();
    if (!root)
    {
        return NULL;
    }
    LLFolderViewItem* current_item = root->getCurSelectedItem();
    if (!current_item)
    {
        return NULL;
    }
    const LLUUID &item_id = static_cast<LLFolderViewModelItemInventory*>(current_item->getViewModelItem())->getUUID();
    LLInventoryItem *item = gInventory.getItem(item_id);
    return item;
}

U32 LLSidepanelInventory::getSelectedCount()
{
    size_t count = 0;

    std::set<LLFolderViewItem*> selection_list = mPanelMainInventory->getActivePanel()->getRootFolder()->getSelectionList();
    count += selection_list.size();

    return static_cast<U32>(count);
}

LLInventoryPanel *LLSidepanelInventory::getActivePanel()
{
    if (!getVisible())
    {
        return NULL;
    }
    if (mInventoryPanel->getVisible())
    {
        return mPanelMainInventory->getActivePanel();
    }
    return NULL;
}

void LLSidepanelInventory::selectAllItemsPanel()
{
    if (!getVisible())
    {
        return;
    }
    if (mInventoryPanel->getVisible())
    {
         mPanelMainInventory->selectAllItemsPanel();
    }

}

bool LLSidepanelInventory::isMainInventoryPanelActive() const
{
    return mInventoryPanel->getVisible();
}

void LLSidepanelInventory::cleanup()
{
    LLFloaterReg::const_instance_list_t& inst_list = LLFloaterReg::getFloaterList("inventory");
    for (LLFloaterReg::const_instance_list_t::const_iterator iter = inst_list.begin(); iter != inst_list.end();)
    {
        LLFloaterSidePanelContainer* iv = dynamic_cast<LLFloaterSidePanelContainer*>(*iter++);
        if (iv)
        {
            iv->cleanup();
        }
    }
}
