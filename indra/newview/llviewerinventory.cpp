/**
 * @file llviewerinventory.cpp
 * @brief Implementation of the viewer side inventory objects.
 *
 * $LicenseInfo:firstyear=2002&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2014, Linden Research, Inc.
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
#include "llviewerinventory.h"

#include "llnotificationsutil.h"
#include "llappviewer.h"
#include "llcallbacklist.h"
#include <deque>
#include <functional>
#include "llsdserialize.h"
#include "message.h"

#include "llaisapi.h"
#include "llchunkedpurge.h"
#include "llagent.h"
#include "llagentcamera.h"
#include "llagentwearables.h"
#include "llfloatersidepanelcontainer.h"
#include "llviewerfoldertype.h"
#include "llfloatersidepanelcontainer.h"
#include "llfolderview.h"
#include "llviewercontrol.h"
#include "llconsole.h"
#include "llinventorydefines.h"
#include "llinventoryfunctions.h"
#include "llinventorymodel.h"
#include "llinventorymodelbackgroundfetch.h"
#include "llgesturemgr.h"

#include "llinventorybridge.h"
#include "llinventorypanel.h"
#include "lllandmarkactions.h"

#include "llviewerassettype.h"
#include "llviewerregion.h"
#include "llviewerobjectlist.h"
#include "llpreviewgesture.h"
#include "llviewerwindow.h"
#include "lltrans.h"
#include "llappearancemgr.h"
#include "llcommandhandler.h"
#include "llviewermessage.h"
#include "llpanelmaininventory.h"
#include "llsidepanelappearance.h"
#include "llsidepanelinventory.h"
#include "llavatarnamecache.h"
#include "llavataractions.h"
#include "lllogininstance.h"
#include "llfavoritesbar.h"
#include "llfloaterperms.h"
#include "llclipboard.h"
#include "llhttpretrypolicy.h"
#include "llsettingsvo.h"
#include "llinventorylistener.h"
#include "llviewerassetupload.h"
#include "slprimdefaults.h"
// [RLVa:KB] - Checked: 2014-11-02 (RLVa-1.4.11)
#include "rlvcommon.h"
// [/RLVa:KB]

LLInventoryListener sInventoryListener;

// do-nothing ops for use in callbacks.
void no_op_inventory_func(const LLUUID&) {}
void no_op_llsd_func(const LLSD&) {}
void no_op() {}

static const char * const LOG_INV("Inventory");
static const char * const LOG_LOCAL("InventoryLocalize");
static const char * const LOG_NOTECARD("copy_inventory_from_notecard");

static const std::string INV_OWNER_ID("owner_id");
static const std::string INV_VERSION("version");

#if 1
// *TODO$: LLInventoryCallback should be deprecated to conform to the new boost::bind/coroutine model.
// temp code in transition
void doInventoryCb(LLPointer<LLInventoryCallback> cb, LLUUID id)
{
    if (cb.notNull())
        cb->fire(id);
}
#endif

///----------------------------------------------------------------------------
/// Helper class to store special inventory item names and their localized values.
///----------------------------------------------------------------------------
class LLLocalizedInventoryItemsDictionary : public LLSingleton<LLLocalizedInventoryItemsDictionary>
{
    LLSINGLETON(LLLocalizedInventoryItemsDictionary);
public:
    std::map<std::string, std::string> mInventoryItemsDict;

    /**
     * Finds passed name in dictionary and replaces it with found localized value.
     *
     * @param object_name - string to be localized.
     * @return true if passed name was found and localized, false otherwise.
     */
    bool localizeInventoryObjectName(std::string& object_name)
    {
        LL_DEBUGS(LOG_LOCAL) << "Searching for localization: " << object_name << LL_ENDL;

        std::map<std::string, std::string>::const_iterator dictionary_iter = mInventoryItemsDict.find(object_name);

        bool found = dictionary_iter != mInventoryItemsDict.end();
        if(found)
        {
            object_name = dictionary_iter->second;
            LL_DEBUGS(LOG_LOCAL) << "Found, new name is: " << object_name << LL_ENDL;
        }
        return found;
    }
};

LLLocalizedInventoryItemsDictionary::LLLocalizedInventoryItemsDictionary()
{
    mInventoryItemsDict["New Shape"]        = LLTrans::getString("New Shape");
    mInventoryItemsDict["New Skin"]         = LLTrans::getString("New Skin");
    mInventoryItemsDict["New Hair"]         = LLTrans::getString("New Hair");
    mInventoryItemsDict["New Eyes"]         = LLTrans::getString("New Eyes");
    mInventoryItemsDict["New Shirt"]        = LLTrans::getString("New Shirt");
    mInventoryItemsDict["New Pants"]        = LLTrans::getString("New Pants");
    mInventoryItemsDict["New Shoes"]        = LLTrans::getString("New Shoes");
    mInventoryItemsDict["New Socks"]        = LLTrans::getString("New Socks");
    mInventoryItemsDict["New Jacket"]       = LLTrans::getString("New Jacket");
    mInventoryItemsDict["New Gloves"]       = LLTrans::getString("New Gloves");
    mInventoryItemsDict["New Undershirt"]   = LLTrans::getString("New Undershirt");
    mInventoryItemsDict["New Underpants"]   = LLTrans::getString("New Underpants");
    mInventoryItemsDict["New Skirt"]        = LLTrans::getString("New Skirt");
    mInventoryItemsDict["New Alpha"]        = LLTrans::getString("New Alpha");
    mInventoryItemsDict["New Tattoo"]       = LLTrans::getString("New Tattoo");
    mInventoryItemsDict["New Universal"]    = LLTrans::getString("New Universal");
    mInventoryItemsDict["New Physics"]      = LLTrans::getString("New Physics");
    mInventoryItemsDict["Invalid Wearable"] = LLTrans::getString("Invalid Wearable");

    mInventoryItemsDict["New Gesture"]      = LLTrans::getString("New Gesture");
    mInventoryItemsDict["New Material"] = LLTrans::getString("New Material");
    mInventoryItemsDict["New Script"]       = LLTrans::getString("New Script");
    mInventoryItemsDict["New Folder"]       = LLTrans::getString("New Folder");
    mInventoryItemsDict["New Note"]         = LLTrans::getString("New Note");
    mInventoryItemsDict["Contents"]         = LLTrans::getString("Contents");

    mInventoryItemsDict["Gesture"]          = LLTrans::getString("Gesture");
    mInventoryItemsDict["Male Gestures"]    = LLTrans::getString("Male Gestures");
    mInventoryItemsDict["Female Gestures"]  = LLTrans::getString("Female Gestures");
    mInventoryItemsDict["Other Gestures"]   = LLTrans::getString("Other Gestures");
    mInventoryItemsDict["Speech Gestures"]  = LLTrans::getString("Speech Gestures");
    mInventoryItemsDict["Common Gestures"]  = LLTrans::getString("Common Gestures");

    //predefined gestures

    //male
    mInventoryItemsDict["Male - Excuse me"]         = LLTrans::getString("Male - Excuse me");
    mInventoryItemsDict["Male  - Get lost"]         = LLTrans::getString("Male - Get lost"); // double space after Male. EXT-8319
    mInventoryItemsDict["Male - Blow kiss"]         = LLTrans::getString("Male - Blow kiss");
    mInventoryItemsDict["Male - Boo"]               = LLTrans::getString("Male - Boo");
    mInventoryItemsDict["Male - Bored"]             = LLTrans::getString("Male - Bored");
    mInventoryItemsDict["Male - Hey"]               = LLTrans::getString("Male - Hey");
    mInventoryItemsDict["Male - Laugh"]             = LLTrans::getString("Male - Laugh");
    mInventoryItemsDict["Male - Repulsed"]          = LLTrans::getString("Male - Repulsed");
    mInventoryItemsDict["Male - Shrug"]             = LLTrans::getString("Male - Shrug");
    mInventoryItemsDict["Male - Stick tougue out"]  = LLTrans::getString("Male - Stick tougue out");
    mInventoryItemsDict["Male - Wow"]               = LLTrans::getString("Male - Wow");

    //female
    mInventoryItemsDict["Female - Chuckle"]         = LLTrans::getString("Female - Chuckle");
    mInventoryItemsDict["Female - Cry"]             = LLTrans::getString("Female - Cry");
    mInventoryItemsDict["Female - Embarrassed"]     = LLTrans::getString("Female - Embarrassed");
    mInventoryItemsDict["Female - Excuse me"]       = LLTrans::getString("Female - Excuse me");
    mInventoryItemsDict["Female  - Get lost"]       = LLTrans::getString("Female - Get lost"); // double space after Female. EXT-8319
    mInventoryItemsDict["Female - Blow kiss"]       = LLTrans::getString("Female - Blow kiss");
    mInventoryItemsDict["Female - Boo"]             = LLTrans::getString("Female - Boo");
    mInventoryItemsDict["Female - Bored"]           = LLTrans::getString("Female - Bored");
    mInventoryItemsDict["Female - Hey"]             = LLTrans::getString("Female - Hey");
    mInventoryItemsDict["Female - Hey baby"]        = LLTrans::getString("Female - Hey baby");
    mInventoryItemsDict["Female - Laugh"]           = LLTrans::getString("Female - Laugh");
    mInventoryItemsDict["Female - Looking good"]    = LLTrans::getString("Female - Looking good");
    mInventoryItemsDict["Female - Over here"]       = LLTrans::getString("Female - Over here");
    mInventoryItemsDict["Female - Please"]          = LLTrans::getString("Female - Please");
    mInventoryItemsDict["Female - Repulsed"]        = LLTrans::getString("Female - Repulsed");
    mInventoryItemsDict["Female - Shrug"]           = LLTrans::getString("Female - Shrug");
    mInventoryItemsDict["Female - Stick tougue out"]= LLTrans::getString("Female - Stick tougue out");
    mInventoryItemsDict["Female - Wow"]             = LLTrans::getString("Female - Wow");

    //common
    mInventoryItemsDict["/bow"]                     = LLTrans::getString("/bow");
    mInventoryItemsDict["/clap"]                    = LLTrans::getString("/clap");
    mInventoryItemsDict["/count"]                   = LLTrans::getString("/count");
    mInventoryItemsDict["/extinguish"]              = LLTrans::getString("/extinguish");
    mInventoryItemsDict["/kmb"]                     = LLTrans::getString("/kmb");
    mInventoryItemsDict["/muscle"]                  = LLTrans::getString("/muscle");
    mInventoryItemsDict["/no"]                      = LLTrans::getString("/no");
    mInventoryItemsDict["/no!"]                     = LLTrans::getString("/no!");
    mInventoryItemsDict["/paper"]                   = LLTrans::getString("/paper");
    mInventoryItemsDict["/pointme"]                 = LLTrans::getString("/pointme");
    mInventoryItemsDict["/pointyou"]                = LLTrans::getString("/pointyou");
    mInventoryItemsDict["/rock"]                    = LLTrans::getString("/rock");
    mInventoryItemsDict["/scissor"]                 = LLTrans::getString("/scissor");
    mInventoryItemsDict["/smoke"]                   = LLTrans::getString("/smoke");
    mInventoryItemsDict["/stretch"]                 = LLTrans::getString("/stretch");
    mInventoryItemsDict["/whistle"]                 = LLTrans::getString("/whistle");
    mInventoryItemsDict["/yes"]                     = LLTrans::getString("/yes");
    mInventoryItemsDict["/yes!"]                    = LLTrans::getString("/yes!");
    mInventoryItemsDict["afk"]                      = LLTrans::getString("afk");
    mInventoryItemsDict["dance1"]                   = LLTrans::getString("dance1");
    mInventoryItemsDict["dance2"]                   = LLTrans::getString("dance2");
    mInventoryItemsDict["dance3"]                   = LLTrans::getString("dance3");
    mInventoryItemsDict["dance4"]                   = LLTrans::getString("dance4");
    mInventoryItemsDict["dance5"]                   = LLTrans::getString("dance5");
    mInventoryItemsDict["dance6"]                   = LLTrans::getString("dance6");
    mInventoryItemsDict["dance7"]                   = LLTrans::getString("dance7");
    mInventoryItemsDict["dance8"]                   = LLTrans::getString("dance8");
}

///----------------------------------------------------------------------------
/// Local function declarations, constants, enums, and typedefs
///----------------------------------------------------------------------------

class LLInventoryHandler : public LLCommandHandler
{
public:
    LLInventoryHandler() : LLCommandHandler("inventory", UNTRUSTED_THROTTLE) { }

    virtual bool canHandleUntrusted(
        const LLSD& params,
        const LLSD& query_map,
        LLMediaCtrl* web,
        const std::string& nav_type)
    {
        if (params.size() < 1)
        {
            return true; // don't block, will fail later
        }

        if (nav_type == NAV_TYPE_CLICKED
            || nav_type == NAV_TYPE_EXTERNAL)
        {
            // NAV_TYPE_EXTERNAL will be throttled
            return true;
        }

        return false;
    }

    bool handle(const LLSD& params,
                const LLSD& query_map,
                const std::string& grid,
                LLMediaCtrl* web)
    {
        if (params.size() < 1)
        {
            return false;
        }

        if (!LLUI::getInstance()->mSettingGroups["config"]->getBOOL("EnableInventory"))
        {
                LLNotificationsUtil::add("NoInventory", LLSD(), LLSD(), std::string("SwitchToStandardSkinAndQuit"));
                return true;
        }

        // support secondlife:///app/inventory/show
        if (params[0].asString() == "show")
        {
            LLFloaterSidePanelContainer::showPanel("inventory", LLSD());
            return true;
        }

        if (params[0].asString() == "filters")
        {
            LLSidepanelInventory *sidepanel_inventory = LLFloaterSidePanelContainer::getPanel<LLSidepanelInventory>("inventory");
            if (sidepanel_inventory)
            {
                LLPanelMainInventory* main_inventory = sidepanel_inventory->getMainInventoryPanel();
                if (main_inventory)
                {
                    main_inventory->toggleFindOptions();
                }
            }
            return true;
        }

        // otherwise, we need a UUID and a verb...
        if (params.size() < 2)
        {
            return false;
        }
        LLUUID inventory_id;
        if (!inventory_id.set(params[0], false))
        {
            return false;
        }

        const std::string verb = params[1].asString();
        if (verb == "select")
        {
            uuid_vec_t items_to_open;
            items_to_open.push_back(inventory_id);
            //inventory_handler is just a stub, because we don't know from who this offer
            open_inventory_offer(items_to_open, "inventory_handler");
            return true;
        }

        return false;
    }
};
LLInventoryHandler gInventoryHandler;

///----------------------------------------------------------------------------
/// Class LLViewerInventoryItem
///----------------------------------------------------------------------------

LLViewerInventoryItem::LLViewerInventoryItem(const LLUUID& uuid,
                                             const LLUUID& parent_uuid,
                                             const LLPermissions& perm,
                                             const LLUUID& asset_uuid,
                                             LLAssetType::EType type,
                                             LLInventoryType::EType inv_type,
                                             const std::string& name,
                                             const std::string& desc,
                                             const LLSaleInfo& sale_info,
                                             U32 flags,
                                             time_t creation_date_utc) :
    LLInventoryItem(uuid, parent_uuid, perm, asset_uuid, type, inv_type,
                    name, desc, sale_info, flags, (S32)creation_date_utc),
    mIsComplete(true)
{
}

LLViewerInventoryItem::LLViewerInventoryItem(const LLUUID& item_id,
                                             const LLUUID& parent_id,
                                             const std::string& name,
                                             LLInventoryType::EType inv_type) :
    LLInventoryItem(),
    mIsComplete(false)
{
    mUUID = item_id;
    mParentUUID = parent_id;
    mInventoryType = inv_type;
    mName = name;
}

LLViewerInventoryItem::LLViewerInventoryItem() :
    LLInventoryItem(),
    mIsComplete(false)
{
}

LLViewerInventoryItem::LLViewerInventoryItem(const LLViewerInventoryItem* other) :
    LLInventoryItem()
{
    copyViewerItem(other);
    if (!mIsComplete)
    {
        LL_WARNS(LOG_INV) << "LLViewerInventoryItem copy constructor for incomplete item"
                          << mUUID << LL_ENDL;
    }
}

LLViewerInventoryItem::LLViewerInventoryItem(const LLInventoryItem *other) :
    LLInventoryItem(other),
    mIsComplete(true)
{
}


LLViewerInventoryItem::~LLViewerInventoryItem()
{
}

void LLViewerInventoryItem::copyViewerItem(const LLViewerInventoryItem* other)
{
    LLInventoryItem::copyItem(other);
    mIsComplete = other->mIsComplete;
    mTransactionID = other->mTransactionID;
}

// Compares exactly the members copyViewerItem() assigns (LLInventoryObject::copyObject,
// LLInventoryItem::copyItem, plus mIsComplete/mTransactionID). Keep in sync with them.
bool LLViewerInventoryItem::isSameAsViewerItem(const LLViewerInventoryItem* other) const
{
    if (!other)
    {
        return false;
    }
    if (other == this)
    {
        return true;
    }
    return mUUID == other->mUUID
        && mParentUUID == other->mParentUUID
        && mType == other->mType
        && mName == other->mName
        && mThumbnailUUID == other->mThumbnailUUID
        && mFavorite == other->mFavorite
        && mPermissions == other->mPermissions
        && mAssetUUID == other->mAssetUUID
        && mDescription == other->mDescription
        && mSaleInfo == other->mSaleInfo
        && mInventoryType == other->mInventoryType
        && mFlags == other->mFlags
        && mCreationDate == other->mCreationDate
        && mIsComplete == other->mIsComplete
        && mTransactionID == other->mTransactionID;
}

// virtual
void LLViewerInventoryItem::copyItem(const LLInventoryItem *other)
{
    LLInventoryItem::copyItem(other);
    mIsComplete = true;
    mTransactionID.setNull();
}

void LLViewerInventoryItem::cloneViewerItem(LLPointer<LLViewerInventoryItem>& newitem) const
{
    newitem = new LLViewerInventoryItem(this);
    if(newitem.notNull())
    {
        LLUUID item_id;
        item_id.generate();
        newitem->setUUID(item_id);
    }
}

void LLViewerInventoryItem::updateServer(bool is_new) const
{
    if(!mIsComplete)
    {
        // *FIX: deal with this better.
        // If we're crashing here then the UI is incorrectly enabled.
        LL_ERRS(LOG_INV) << "LLViewerInventoryItem::updateServer() - for incomplete item"
                         << LL_ENDL;
        return;
    }
    if(gAgent.getID() != mPermissions.getOwner())
    {
        // *FIX: deal with this better.
        LL_WARNS(LOG_INV) << "LLViewerInventoryItem::updateServer() - for unowned item "
                          << ll_pretty_print_sd(this->asLLSD())
                          << LL_ENDL;
        return;
    }
    LLInventoryModel::LLCategoryUpdate up(mParentUUID, is_new ? 1 : 0);
    gInventory.accountForUpdate(up);

    LLSD updates = asLLSD();
    // Replace asset_id and/or shadow_id with transaction_id (hash_id)
    if (updates.has("asset_id"))
    {
        updates.erase("asset_id");
        if(getTransactionID().notNull())
        {
            updates["hash_id"] = getTransactionID();
        }
    }
    if (updates.has("shadow_id"))
    {
        updates.erase("shadow_id");
        if(getTransactionID().notNull())
        {
            updates["hash_id"] = getTransactionID();
        }
    }
    AISAPI::completion_t cr = boost::bind(&doInventoryCb, (LLPointer<LLInventoryCallback>)NULL, _1);
    AISAPI::UpdateItem(getUUID(), updates, cr);
}

void LLViewerInventoryItem::fetchFromServer(void) const
{
    if(!mIsComplete)
    {
        if (AISAPI::isAvailable()) // AIS v 3
        {
            LLInventoryModelBackgroundFetch::getInstance()->scheduleItemFetch(mUUID);
        }
        else
        {
            std::string url;

            LLViewerRegion* region = gAgent.getRegion();
            // we have to check region. It can be null after region was destroyed. See EXT-245
            if (region)
            {
                if (gAgent.getID() != mPermissions.getOwner())
                {
                    url = region->getCapability("FetchLib2");
                }
                else
                {
                    url = region->getCapability("FetchInventory2");
                }
            }
            else
            {
                LL_WARNS(LOG_INV) << "Agent Region is absent" << LL_ENDL;
            }

            if (!url.empty())
            {
                LLSD body;
                body["agent_id"] = gAgent.getID();
                body["items"][0]["owner_id"] = mPermissions.getOwner();
                body["items"][0]["item_id"] = mUUID;

                LLCore::HttpHandler::ptr_t handler(new LLInventoryModel::FetchItemHttpHandler(body));
                gInventory.requestPost(true, url, body, handler, "Inventory Item");
            }
        }
    }
}

// virtual
bool LLViewerInventoryItem::unpackMessage(const LLSD& item)
{
    bool rv = LLInventoryItem::fromLLSD(item);

    LLLocalizedInventoryItemsDictionary::getInstance()->localizeInventoryObjectName(mName);

    mIsComplete = true;
    return rv;
}

// virtual
bool LLViewerInventoryItem::unpackMessage(LLMessageSystem* msg, const char* block, S32 block_num)
{
    bool rv = LLInventoryItem::unpackMessage(msg, block, block_num);

    LLLocalizedInventoryItemsDictionary::getInstance()->localizeInventoryObjectName(mName);

    mIsComplete = true;
    return rv;
}

void LLViewerInventoryItem::setTransactionID(const LLTransactionID& transaction_id)
{
    mTransactionID = transaction_id;
}

void LLViewerInventoryItem::packMessage(LLMessageSystem* msg) const
{
    msg->addUUIDFast(_PREHASH_ItemID, mUUID);
    msg->addUUIDFast(_PREHASH_FolderID, mParentUUID);
    mPermissions.packMessage(msg);
    msg->addUUIDFast(_PREHASH_TransactionID, mTransactionID);
    S8 type = static_cast<S8>(mType);
    msg->addS8Fast(_PREHASH_Type, type);
    type = static_cast<S8>(mInventoryType);
    msg->addS8Fast(_PREHASH_InvType, type);
    msg->addU32Fast(_PREHASH_Flags, mFlags);
    mSaleInfo.packMessage(msg);
    msg->addStringFast(_PREHASH_Name, mName);
    msg->addStringFast(_PREHASH_Description, mDescription);
    msg->addS32Fast(_PREHASH_CreationDate, (S32)mCreationDate);
    U32 crc = getCRC32();
    msg->addU32Fast(_PREHASH_CRC, crc);
}

// virtual
bool LLViewerInventoryItem::importLegacyStream(std::istream& input_stream)
{
    bool rv = LLInventoryItem::importLegacyStream(input_stream);
    mIsComplete = true;
    return rv;
}

void LLViewerInventoryItem::updateParentOnServer(bool restamp) const
{
    LLMessageSystem* msg = gMessageSystem;
    msg->newMessageFast(_PREHASH_MoveInventoryItem);
    msg->nextBlockFast(_PREHASH_AgentData);
    msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
    msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
    msg->addBOOLFast(_PREHASH_Stamp, restamp);
    msg->nextBlockFast(_PREHASH_InventoryData);
    msg->addUUIDFast(_PREHASH_ItemID, mUUID);
    msg->addUUIDFast(_PREHASH_FolderID, mParentUUID);
    msg->addString("NewName", NULL);
    gAgent.sendReliableMessage();
}

//void LLViewerInventoryItem::setCloneCount(S32 clones)
//{
//  mClones = clones;
//}

//S32 LLViewerInventoryItem::getCloneCount() const
//{
//  return mClones;
//}

///----------------------------------------------------------------------------
/// Class LLViewerInventoryCategory
///----------------------------------------------------------------------------

LLViewerInventoryCategory::LLViewerInventoryCategory(const LLUUID& uuid,
                                                     const LLUUID& parent_uuid,
                                                     LLFolderType::EType pref,
                                                     const std::string& name,
                                                     const LLUUID& owner_id) :
    LLInventoryCategory(uuid, parent_uuid, pref, name),
    mOwnerID(owner_id),
    mVersion(LLViewerInventoryCategory::VERSION_UNKNOWN),
    mDescendentCount(LLViewerInventoryCategory::DESCENDENT_COUNT_UNKNOWN),
    mFetching(FETCH_NONE)
{
    mDescendentsRequested.reset();
}

LLViewerInventoryCategory::LLViewerInventoryCategory(const LLUUID& owner_id) :
    mOwnerID(owner_id),
    mVersion(LLViewerInventoryCategory::VERSION_UNKNOWN),
    mDescendentCount(LLViewerInventoryCategory::DESCENDENT_COUNT_UNKNOWN),
    mFetching(FETCH_NONE)
{
    mDescendentsRequested.reset();
}

LLViewerInventoryCategory::LLViewerInventoryCategory(const LLViewerInventoryCategory* other)
{
    copyViewerCategory(other);
    mFetching = FETCH_NONE;
}

LLViewerInventoryCategory::~LLViewerInventoryCategory()
{
}

void LLViewerInventoryCategory::copyViewerCategory(const LLViewerInventoryCategory* other)
{
    copyCategory(other);
    mOwnerID = other->mOwnerID;
    setVersion(other->getVersion());
    mDescendentCount = other->mDescendentCount;
    mDescendentsRequested = other->mDescendentsRequested;
}


void LLViewerInventoryCategory::packMessage(LLMessageSystem* msg) const
{
    msg->addUUIDFast(_PREHASH_FolderID, mUUID);
    msg->addUUIDFast(_PREHASH_ParentID, mParentUUID);
    S8 type = static_cast<S8>(mPreferredType);
    msg->addS8Fast(_PREHASH_Type, type);
    msg->addStringFast(_PREHASH_Name, mName);
}

void LLViewerInventoryCategory::updateParentOnServer(bool restamp) const
{
    LLMessageSystem* msg = gMessageSystem;
    msg->newMessageFast(_PREHASH_MoveInventoryFolder);
    msg->nextBlockFast(_PREHASH_AgentData);
    msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
    msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());

    msg->addBOOL("Stamp", restamp);
    msg->nextBlockFast(_PREHASH_InventoryData);
    msg->addUUIDFast(_PREHASH_FolderID, mUUID);
    msg->addUUIDFast(_PREHASH_ParentID, mParentUUID);
    gAgent.sendReliableMessage();
}

void LLViewerInventoryCategory::updateServer(bool is_new) const
{
    // communicate that change with the server.

    if (LLFolderType::lookupIsProtectedType(mPreferredType))
    {
        LLNotificationsUtil::add("CannotModifyProtectedCategories");
        return;
    }

    LLSD new_llsd = asLLSD();
    AISAPI::completion_t cr = boost::bind(&doInventoryCb, (LLPointer<LLInventoryCallback>)NULL, _1);
    AISAPI::UpdateCategory(getUUID(), new_llsd, cr);
}

S32 LLViewerInventoryCategory::getVersion() const
{
    return mVersion;
}

void LLViewerInventoryCategory::setVersion(S32 version)
{
    mVersion = version;
}

bool LLViewerInventoryCategory::fetch(S32 expiry_seconds)
{
    if((VERSION_UNKNOWN == getVersion())
       && mDescendentsRequested.hasExpired())   //Expired check prevents multiple downloads.
    {
        LL_DEBUGS(LOG_INV) << "Fetching category children: " << mName << ", UUID: " << mUUID << LL_ENDL;
        mDescendentsRequested.reset();
        mDescendentsRequested.setTimerExpirySec((F32)expiry_seconds);

        std::string url;
        if (gAgent.getRegion())
        {
            url = gAgent.getRegion()->getCapability("FetchInventoryDescendents2");
        }
        else
        {
            LL_WARNS_ONCE(LOG_INV) << "agent region is null" << LL_ENDL;
        }
        if (!url.empty() || AISAPI::isAvailable())
        {
            LLInventoryModelBackgroundFetch::instance().start(mUUID, false);
        }
        return true;
    }
    return false;
}

LLViewerInventoryCategory::EFetchType LLViewerInventoryCategory::getFetching()
{
    // if timer hasn't expired, request was scheduled, but not in progress
    // if mFetching request was actually started
    if (mDescendentsRequested.hasExpired())
    {
        mFetching = FETCH_NONE;
    }
    return mFetching;
}

void LLViewerInventoryCategory::setFetching(LLViewerInventoryCategory::EFetchType fetching)
{
    if (fetching == FETCH_FAILED)
    {
        const F32 FETCH_FAILURE_EXPIRY = 60.0f;
        mDescendentsRequested.setTimerExpirySec(FETCH_FAILURE_EXPIRY);
        mFetching = fetching;
    }
    else if (fetching > mFetching) // allow a switch from normal to recursive
    {
        if (mDescendentsRequested.hasExpired() || (mFetching == FETCH_NONE))
        {
            mDescendentsRequested.reset();
            if (AISAPI::isAvailable())
            {
                mDescendentsRequested.setTimerExpirySec((F32)AISAPI::HTTP_TIMEOUT);
            }
            else
            {
                const F32 FETCH_TIMER_EXPIRY = 30.0f;
                mDescendentsRequested.setTimerExpirySec(FETCH_TIMER_EXPIRY);
            }
        }
        mFetching = fetching;
    }
    else if (fetching == FETCH_NONE)
    {
        mDescendentsRequested.stop();
        mFetching = fetching;
    }
}

S32 LLViewerInventoryCategory::getViewerDescendentCount() const
{
    LLInventoryModel::cat_array_t* cats;
    LLInventoryModel::item_array_t* items;
    gInventory.getDirectDescendentsOf(getUUID(), cats, items);
    S32 descendents_actual = 0;
    if(cats && items)
    {
        descendents_actual = static_cast<S32>(cats->size() + items->size());
    }
    return descendents_actual;
}

void LLViewerInventoryCategory::exportLLSD(LLSD & cat_data) const
{
    LLInventoryCategory::exportLLSD(cat_data);
    cat_data[INV_OWNER_ID] = mOwnerID;
    cat_data[INV_VERSION] = mVersion;
}

bool LLViewerInventoryCategory::importLLSD(const std::string& label, const LLSD& value)
{
    if (LLInventoryCategory::importLLSD(label, value))
    {
        return true;
    }
    else if (label == INV_OWNER_ID)
    {
        mOwnerID = value.asUUID();
        return true;
    }
    else if (label == INV_VERSION)
    {
        setVersion(value.asInteger());
        return true;
    }
    return false;
}

bool LLViewerInventoryCategory::acceptItem(LLInventoryItem* inv_item)
{
    if (!inv_item)
    {
        return false;
    }

    // Only stock folders have limitation on which item they will accept
    bool accept = true;
    if (getPreferredType() == LLFolderType::FT_MARKETPLACE_STOCK)
    {
        // If the item is copyable (i.e. non stock) do not accept the drop in a stock folder
        if (inv_item->getPermissions().allowOperationBy(PERM_COPY, gAgent.getID(), gAgent.getGroupID()))
        {
            accept = false;
        }
        else
        {
            LLInventoryModel::cat_array_t* cat_array;
            LLInventoryModel::item_array_t* item_array;
            gInventory.getDirectDescendentsOf(getUUID(),cat_array,item_array);
            // Destination stock folder must be empty OR types of incoming and existing items must be identical and have the same permissions
            accept = (!item_array->size() ||
                      ((item_array->at(0)->getInventoryType() == inv_item->getInventoryType()) &&
                       (item_array->at(0)->getPermissions().getMaskNextOwner() == inv_item->getPermissions().getMaskNextOwner())));
        }
    }
    return accept;
}

void LLViewerInventoryCategory::determineFolderType()
{
    /* Do NOT uncomment this code.  This is for future 2.1 support of ensembles.
    llassert(false);
    LLFolderType::EType original_type = getPreferredType();
    if (LLFolderType::lookupIsProtectedType(original_type))
        return;

    U64 folder_valid = 0;
    U64 folder_invalid = 0;
    LLInventoryModel::cat_array_t category_array;
    LLInventoryModel::item_array_t item_array;
    gInventory.collectDescendents(getUUID(),category_array,item_array,false);

    // For ensembles
    if (category_array.empty())
    {
        for (LLInventoryModel::item_array_t::iterator item_iter = item_array.begin();
             item_iter != item_array.end();
             item_iter++)
        {
            const LLViewerInventoryItem *item = (*item_iter);
            if (item->getIsLinkType())
                return;
            if (item->isWearableType())
            {
                const LLWearableType::EType wearable_type = item->getWearableType();
                const std::string& wearable_name = LLWearableType::getTypeName(wearable_type);
                U64 valid_folder_types = LLViewerFolderType::lookupValidFolderTypes(wearable_name);
                folder_valid |= valid_folder_types;
                folder_invalid |= ~valid_folder_types;
            }
        }
        for (U8 i = LLFolderType::FT_ENSEMBLE_START; i <= LLFolderType::FT_ENSEMBLE_END; i++)
        {
            if ((folder_valid & (1LL << i)) &&
                !(folder_invalid & (1LL << i)))
            {
                changeType((LLFolderType::EType)i);
                return;
            }
        }
    }
    if (LLFolderType::lookupIsEnsembleType(original_type))
    {
        changeType(LLFolderType::FT_NONE);
    }
    llassert(false);
    */
}

void LLViewerInventoryCategory::changeType(LLFolderType::EType new_folder_type)
{
    const LLUUID &folder_id = getUUID();
    const LLUUID &parent_id = getParentUUID();
    const std::string &name = getName();

    LLPointer<LLViewerInventoryCategory> new_cat = new LLViewerInventoryCategory(folder_id,
                                                                                 parent_id,
                                                                                 new_folder_type,
                                                                                 name,
                                                                                 gAgent.getID());


    LLSD new_llsd = new_cat->asLLSD();
    AISAPI::completion_t cr = boost::bind(&doInventoryCb, (LLPointer<LLInventoryCallback>) NULL, _1);
    AISAPI::UpdateCategory(folder_id, new_llsd, cr);

    setPreferredType(new_folder_type);
    gInventory.addChangedMask(LLInventoryObserver::LABEL, folder_id);
}

void LLViewerInventoryCategory::localizeName()
{
    LLLocalizedInventoryItemsDictionary::getInstance()->localizeInventoryObjectName(mName);
}

// virtual
bool LLViewerInventoryCategory::unpackMessage(const LLSD& category)
{
    bool rv = LLInventoryCategory::fromLLSD(category);
    localizeName();
    return rv;
}

// virtual
void LLViewerInventoryCategory::unpackMessage(LLMessageSystem* msg, const char* block, S32 block_num)
{
    LLInventoryCategory::unpackMessage(msg, block, block_num);
    localizeName();
}

///----------------------------------------------------------------------------
/// Local function definitions
///----------------------------------------------------------------------------

LLInventoryCallbackManager *LLInventoryCallbackManager::sInstance = NULL;

LLInventoryCallbackManager::LLInventoryCallbackManager() :
    mLastCallback(0)
{
    if( sInstance != NULL )
    {
        LL_WARNS(LOG_INV) << "LLInventoryCallbackManager::LLInventoryCallbackManager: unexpected multiple instances" << LL_ENDL;
        return;
    }
    sInstance = this;
}

LLInventoryCallbackManager::~LLInventoryCallbackManager()
{
    if( sInstance != this )
    {
        LL_WARNS(LOG_INV) << "LLInventoryCallbackManager::~LLInventoryCallbackManager: unexpected multiple instances" << LL_ENDL;
        return;
    }
    sInstance = NULL;
}

//static
void LLInventoryCallbackManager::destroyClass()
{
    if (sInstance)
    {
        for (callback_map_t::iterator it = sInstance->mMap.begin(), end_it = sInstance->mMap.end(); it != end_it; ++it)
        {
            // drop LLPointer reference to callback
            it->second = NULL;
        }
        sInstance->mMap.clear();
    }
}


U32 LLInventoryCallbackManager::registerCB(LLPointer<LLInventoryCallback> cb)
{
    if (cb.isNull())
        return 0;

    mLastCallback++;
    if (!mLastCallback)
        mLastCallback++;

    mMap[mLastCallback] = cb;
    return mLastCallback;
}

void LLInventoryCallbackManager::fire(U32 callback_id, const LLUUID& item_id)
{
    if (!callback_id || item_id.isNull())
        return;

    std::map<U32, LLPointer<LLInventoryCallback> >::iterator i;

    i = mMap.find(callback_id);
    if (i != mMap.end())
    {
        (*i).second->fire(item_id);
        mMap.erase(i);
    }
}

void rez_attachment_cb(const LLUUID& inv_item, LLViewerJointAttachment *attachmentp, bool replace)
{
    if (inv_item.isNull())
        return;

    LLViewerInventoryItem *item = gInventory.getItem(inv_item);
    if (item)
    {
        rez_attachment(item, attachmentp, replace);
    }
}

void activate_gesture_cb(const LLUUID& inv_item)
{
    if (inv_item.isNull())
        return;
    LLViewerInventoryItem* item = gInventory.getItem(inv_item);
    if (!item)
        return;
    if (item->getType() != LLAssetType::AT_GESTURE)
        return;

    LLGestureMgr::instance().activateGesture(inv_item);
}

void set_default_permissions(LLViewerInventoryItem* item, std::string perm_type)
{
    llassert(item);
    LLPermissions perm = item->getPermissions();
    if (perm.getMaskEveryone() != LLFloaterPerms::getEveryonePerms(perm_type)
        || perm.getMaskGroup() != LLFloaterPerms::getGroupPerms(perm_type))
    {
        perm.setMaskEveryone(LLFloaterPerms::getEveryonePerms(perm_type));
        perm.setMaskGroup(LLFloaterPerms::getGroupPerms(perm_type));

        item->setPermissions(perm);

        item->updateServer(false);
    }
}

void create_script_cb(const LLUUID& inv_item)
{
    if (!inv_item.isNull())
    {
        LLViewerInventoryItem* item = gInventory.getItem(inv_item);
        if (item)
        {
            // ------------------------------------------------------------------------------------
            // Begin hack
            //
            // The current state of the server doesn't allow specifying a default script template,
            // so we have to update its code immediately after creation instead.
            //
            // This temporary workaround should be removed after a server-side fix.
            // See https://github.com/secondlife/viewer/issues/3731 for more information.
            //
            LLViewerRegion* region = gAgent.getRegion();
            if (region && region->simulatorFeaturesReceived())
            {
                LLSD simulatorFeatures;
                region->getSimulatorFeatures(simulatorFeatures);
                if (simulatorFeatures["LuaScriptsEnabled"].asBoolean())
                {

                    const std::string hello_lua_script =
                        "function state_entry()\n"
                        "   ll.Say(0, \"Hello, Avatar!\")\n"
                        "end\n"
                        "\n"
                        "function touch_start(total_number)\n"
                        "   ll.Say(0, \"Touched.\")\n"
                        "end\n"
                        "\n"
                        "-- Simulate the state_entry event\n"
                        "state_entry()\n";

                    std::string url = gAgent.getRegion()->getCapability("UpdateScriptAgent");
                    if (!url.empty())
                    {
                        LLResourceUploadInfo::ptr_t uploadInfo(std::make_shared<LLScriptAssetUpload>(
                            item->getUUID(),
                            "luau",
                            hello_lua_script,
                            nullptr,
                            nullptr));

                        LLViewerAssetUpload::EnqueueInventoryUpload(url, uploadInfo);
                    }
                }
            }
            //
            // End hack
            // ------------------------------------------------------------------------------------

            set_default_permissions(item, "Scripts");

            // item was just created, update even if permissions did not changed
            gInventory.updateItem(item);
            gInventory.notifyObservers();
        }
    }
}

// A copy of the custom default script (#466) needs the same finishing
// touches as a stock new script, but not the hello-world upload above.
static void copy_default_script_cb(const LLUUID& inv_item)
{
    if (!inv_item.isNull())
    {
        LLViewerInventoryItem* item = gInventory.getItem(inv_item);
        if (item)
        {
            set_default_permissions(item, "Scripts");

            gInventory.updateItem(item);
            gInventory.notifyObservers();
        }
    }
}

void create_gesture_cb(const LLUUID& inv_item)
{
    if (!inv_item.isNull())
    {
        LLGestureMgr::instance().activateGesture(inv_item);

        LLViewerInventoryItem* item = gInventory.getItem(inv_item);
        if (item)
        {
            set_default_permissions(item, "Gestures");

            gInventory.updateItem(item);
            gInventory.notifyObservers();

            LLPreviewGesture* preview = LLPreviewGesture::show(inv_item,  LLUUID::null);
            // Force to be entirely onscreen.
            gFloaterView->adjustToFitScreen(preview, false);
        }
    }
}


void create_notecard_cb(const LLUUID& inv_item)
{
    if (!inv_item.isNull())
        {
        LLViewerInventoryItem* item = gInventory.getItem(inv_item);
        if (item)
        {
            set_default_permissions(item, "Notecards");

            gInventory.updateItem(item);
            gInventory.notifyObservers();
        }
    }
}

void create_gltf_material_cb(const LLUUID& inv_item)
{
    if (!inv_item.isNull())
    {
        LLViewerInventoryItem* item = gInventory.getItem(inv_item);
        if (item)
        {
            set_default_permissions(item, "Materials");

            gInventory.updateItem(item);
            gInventory.notifyObservers();
        }
    }
}

LLInventoryCallbackManager gInventoryCallbacks;

///----------------------------------------------------------------------------
/// #461: InvCreate ledger (see llviewerinventory.h). Logging only.
///
/// The server answers a refused UDP create (CreateInventoryItem, CopyInventoryItem,
/// an accepted object offer) with nothing but an alert; the callback never fires.
/// So refusals are matched to requests by time: when the alert arrives, every
/// request still waiting for an answer, and every one sent in the last minute, is
/// logged. Bounded: the newest LEDGER_MAX requests are kept.
///----------------------------------------------------------------------------
namespace
{
    constexpr size_t INV_CREATE_LEDGER_MAX = 128;
    constexpr F32 INV_CREATE_LOGIN_WINDOW = 300.f;   // seconds after login completes
    constexpr F64 INV_CREATE_RECENT = 60.0;          // answered requests still shown on a refusal
    constexpr U32 INV_CREATE_MAX_INFO_LINES = 400;   // then DEBUG (a big copy at login)

    struct LLInvCreateEntry
    {
        U64 mToken = 0;
        std::string mKind;
        std::string mSite;
        LLUUID mParent;
        S32 mType = -1;
        std::string mName;
        std::string mDetail;
        F64 mSentAt = 0.0;
        bool mAnswered = false;
        LLUUID mResult;
    };

    struct LLInvCreateLedger
    {
        std::deque<LLInvCreateEntry> mEntries;
        U64 mNextToken = 1;
        U32 mInfoLines = 0;
        bool mInfoCapNoted = false;
        const char* mSite = nullptr;
    };

    // Never destroyed (callbacks may fire during shutdown).
    LLInvCreateLedger& inv_create_ledger()
    {
        static LLInvCreateLedger* sLedger = new LLInvCreateLedger;
        return *sLedger;
    }

    std::string inv_create_folder_name(const LLUUID& id)
    {
        if (id.isNull())
            return "(none: server default)";
        const LLViewerInventoryCategory* cat = gInventory.getCategory(id);
        return cat ? "'" + cat->getName() + "'" : "(unknown folder " + id.asString() + ")";
    }

    std::string inv_create_type_name(S32 type)
    {
        if (type < 0)
            return "-";
        const char* name = LLAssetType::lookup((LLAssetType::EType)type);
        return name ? std::string(name) : llformat("%d", type);
    }

    std::string inv_create_describe(const LLInvCreateEntry& e)
    {
        std::ostringstream out;
        out << e.mKind << " #" << e.mToken << " [" << (e.mSite.empty() ? "?" : e.mSite) << "] '" << e.mName
            << "' type " << inv_create_type_name(e.mType) << " in " << inv_create_folder_name(e.mParent);
        if (!e.mDetail.empty())
            out << " (" << e.mDetail << ")";
        return out.str();
    }

    // INFO while logging in, then DEBUG; a capped number of INFO lines per session.
    bool inv_create_info_line()
    {
        LLInvCreateLedger& l = inv_create_ledger();
        if (!LLInvCreateLog::verbose())
            return false;
        if (l.mInfoLines < INV_CREATE_MAX_INFO_LINES)
        {
            ++l.mInfoLines;
            return true;
        }
        if (!l.mInfoCapNoted)
        {
            l.mInfoCapNoted = true;
            LL_INFOS("InvCreate") << "Further inventory create lines this login are at DEBUG" << LL_ENDL;
        }
        return false;
    }

    class LLInvCreateLogCallback : public LLInventoryCallback
    {
    public:
        LLInvCreateLogCallback(U64 token, LLPointer<LLInventoryCallback> inner) : mToken(token), mInner(inner) {}

        void fire(const LLUUID& new_id) override
        {
            LLInvCreateLog::noteResult(mToken, new_id);
            if (mInner)
                mInner->fire(new_id);
        }

    private:
        U64 mToken;
        LLPointer<LLInventoryCallback> mInner;
    };
}

LLInvCreateLog::Site::Site(const char* name)
    : mPrevious(inv_create_ledger().mSite)
{
    inv_create_ledger().mSite = name;
}

LLInvCreateLog::Site::~Site()
{
    inv_create_ledger().mSite = mPrevious;
}

bool LLInvCreateLog::verbose()
{
    return !gLoggedInTime.getStarted() || gLoggedInTime.getElapsedTimeF32() < INV_CREATE_LOGIN_WINDOW;
}

U64 LLInvCreateLog::noteSent(const char* kind, const LLUUID& parent, S32 type, const std::string& name,
                             const std::string& detail)
{
    LLInvCreateLedger& l = inv_create_ledger();
    LLInvCreateEntry e;
    e.mToken = l.mNextToken++;
    e.mKind = kind ? kind : "?";
    e.mSite = l.mSite ? l.mSite : "";
    e.mParent = parent;
    e.mType = type;
    e.mName = name;
    e.mDetail = detail;
    e.mSentAt = LLTimer::getTotalSeconds();
    if (inv_create_info_line())
        LL_INFOS("InvCreate") << "Request " << inv_create_describe(e) << LL_ENDL;
    else
        LL_DEBUGS("InvCreate") << "Request " << inv_create_describe(e) << LL_ENDL;
    l.mEntries.push_back(std::move(e));
    while (l.mEntries.size() > INV_CREATE_LEDGER_MAX)
        l.mEntries.pop_front();
    return l.mEntries.back().mToken;
}

void LLInvCreateLog::noteResult(U64 token, const LLUUID& new_id)
{
    LLInvCreateLedger& l = inv_create_ledger();
    for (auto it = l.mEntries.rbegin(); it != l.mEntries.rend(); ++it)
    {
        if (it->mToken != token)
            continue;
        it->mAnswered = true;
        it->mResult = new_id;
        const F64 age = LLTimer::getTotalSeconds() - it->mSentAt;
        if (new_id.isNull())
            LL_WARNS("InvCreate") << "FAILED " << inv_create_describe(*it) << " after " << llformat("%.1f", age) << " s" << LL_ENDL;
        else if (inv_create_info_line())
            LL_INFOS("InvCreate") << "Created " << new_id << " for " << inv_create_describe(*it) << " after "
                                  << llformat("%.1f", age) << " s" << LL_ENDL;
        else
            LL_DEBUGS("InvCreate") << "Created " << new_id << " for " << inv_create_describe(*it) << LL_ENDL;
        return;
    }
    // Answer for a request that already left the bounded ledger.
    LL_DEBUGS("InvCreate") << "Answer for #" << token << " -> " << new_id << LL_ENDL;
}

LLPointer<LLInventoryCallback> LLInvCreateLog::wrap(U64 token, LLPointer<LLInventoryCallback> inner)
{
    return new LLInvCreateLogCallback(token, inner);
}

void LLInvCreateLog::onServerRefusal(const std::string& notification)
{
    LLInvCreateLedger& l = inv_create_ledger();
    const F64 now = LLTimer::getTotalSeconds();
    LL_WARNS("InvCreate") << "Server refused an inventory create (" << notification << "). Requests waiting or sent in the last "
                          << (S32)INV_CREATE_RECENT << " s, newest first:" << LL_ENDL;
    U32 shown = 0;
    for (auto it = l.mEntries.rbegin(); it != l.mEntries.rend(); ++it)
    {
        const F64 age = now - it->mSentAt;
        if (it->mAnswered && age > INV_CREATE_RECENT)
            continue;
        ++shown;
        LL_WARNS("InvCreate") << "  " << (it->mAnswered ? (it->mResult.notNull() ? "answered " : "FAILED   ") : "WAITING  ")
                              << inv_create_describe(*it) << ", sent " << llformat("%.1f", age) << " s ago" << LL_ENDL;
    }
    if (!shown)
    {
        LL_WARNS("InvCreate") << "  (none: the refused request was not one the viewer logs as a create -- e.g. an "
                                 "object giving inventory, a rez or take, or a script dropped into an object)" << LL_ENDL;
    }
}

void create_inventory_item(
    const LLUUID& agent_id,
    const LLUUID& session_id,
    const LLUUID& parent_id,
    const LLTransactionID& transaction_id,
    const std::string& name,
    const std::string& desc,
    LLAssetType::EType asset_type,
    LLInventoryType::EType inv_type,
    U8 subtype,
    U32 next_owner_perm,
    LLPointer<LLInventoryCallback> cb)
{
    //check if name is equal to one of special inventory items names
    //EXT-5839
    std::string server_name = name;

    {
        std::map<std::string, std::string>::const_iterator dictionary_iter;

        for (dictionary_iter = LLLocalizedInventoryItemsDictionary::getInstance()->mInventoryItemsDict.begin();
             dictionary_iter != LLLocalizedInventoryItemsDictionary::getInstance()->mInventoryItemsDict.end();
             dictionary_iter++)
        {
            const std::string& localized_name = dictionary_iter->second;
            if(localized_name == name)
            {
                server_name = dictionary_iter->first;
            }
        }
    }

    // #461: log the request and its answer. The callback is always registered now,
    // so CallbackID is non-zero even for callers without one (an opaque token the
    // server echoes back; the copy pacer below does the same).
    cb = LLInvCreateLog::wrap(LLInvCreateLog::noteSent("create", parent_id, (S32)asset_type, server_name), cb);

#ifdef USE_AIS_FOR_NC
    // D567 18.03.2023 not yet implemented within AIS3
    if (AISAPI::isAvailable())
    {
        LLSD new_inventory = LLSD::emptyMap();
        new_inventory["items"] = LLSD::emptyArray();

        LLPermissions perms;
        perms.init(
            gAgentID,
            gAgentID,
            LLUUID::null,
            LLUUID::null);
        perms.initMasks(
            PERM_ALL,
            PERM_ALL,
            PERM_NONE,
            PERM_NONE,
            next_owner_perm);

        LLUUID null_id;
        LLPointer<LLViewerInventoryItem> item = new LLViewerInventoryItem(
            null_id, /*don't know yet*/
            parent_id,
            perms,
            null_id, /*don't know yet*/
            asset_type,
            inv_type,
            server_name,
            desc,
            LLSaleInfo(),
            0,
            0 /*don't know yet, whenever server creates it*/);
        LLSD item_sd = item->asLLSD();
        new_inventory["items"].append(item_sd);
        AISAPI::completion_t cr = boost::bind(&doInventoryCb, cb, _1);
        AISAPI::CreateInventory(
            parent_id,
            new_inventory,
            cr);
        return;
    }
    else
    {
        LL_WARNS() << "AIS v3 not available" << LL_ENDL;
    }
#endif

    LLMessageSystem* msg = gMessageSystem;
    msg->newMessageFast(_PREHASH_CreateInventoryItem);
    msg->nextBlock(_PREHASH_AgentData);
    msg->addUUIDFast(_PREHASH_AgentID, agent_id);
    msg->addUUIDFast(_PREHASH_SessionID, session_id);
    msg->nextBlock(_PREHASH_InventoryBlock);
    msg->addU32Fast(_PREHASH_CallbackID, gInventoryCallbacks.registerCB(cb));
    msg->addUUIDFast(_PREHASH_FolderID, parent_id);
    msg->addUUIDFast(_PREHASH_TransactionID, transaction_id);
    msg->addU32Fast(_PREHASH_NextOwnerMask, next_owner_perm);
    msg->addS8Fast(_PREHASH_Type, (S8)asset_type);
    msg->addS8Fast(_PREHASH_InvType, (S8)inv_type);
    msg->addU8Fast(_PREHASH_WearableType, (U8)subtype);
    msg->addStringFast(_PREHASH_Name, server_name);
    msg->addStringFast(_PREHASH_Description, desc);

    gAgent.sendReliableMessage();
}

void create_inventory_callingcard_callback(LLPointer<LLInventoryCallback> cb,
                                           const LLUUID &parent,
                                           const LLUUID &avatar_id,
                                           const LLAvatarName &av_name)
{
    std::string item_desc = avatar_id.asString();
    LLInvCreateLog::Site inv_site("friend calling card"); // #461
    create_inventory_item(gAgent.getID(),
                          gAgent.getSessionID(),
                          parent,
                          LLTransactionID::tnull,
                          av_name.getUserName(),
                          item_desc,
                          LLAssetType::AT_CALLINGCARD,
                          LLInventoryType::IT_CALLINGCARD,
                          NO_INV_SUBTYPE,
                          PERM_MOVE | PERM_TRANSFER,
                          cb);
}

void create_inventory_callingcard(const LLUUID& avatar_id, const LLUUID& parent /*= LLUUID::null*/, LLPointer<LLInventoryCallback> cb/*=NULL*/)
{
    LLAvatarName av_name;
    LLAvatarNameCache::get(avatar_id, boost::bind(&create_inventory_callingcard_callback, cb, parent, _1, _2));
}

void create_inventory_wearable(const LLUUID& agent_id, const LLUUID& session_id,
    const LLUUID& parent, const LLTransactionID& transaction_id,
    const std::string& name,
    const std::string& desc, LLAssetType::EType asset_type,
    LLWearableType::EType wtype,
    U32 next_owner_perm,
    LLPointer<LLInventoryCallback> cb)
{
    create_inventory_item(agent_id, session_id, parent, transaction_id,
        name, desc, asset_type, LLInventoryType::IT_WEARABLE, static_cast<U8>(wtype),
        next_owner_perm, cb);
}

void create_inventory_settings(const LLUUID& agent_id, const LLUUID& session_id,
    const LLUUID& parent, const LLTransactionID& transaction_id,
    const std::string& name,
    const std::string& desc,
    LLSettingsType::type_e settype,
    U32 next_owner_perm,
    LLPointer<LLInventoryCallback> cb)
{
    create_inventory_item(agent_id, session_id, parent, transaction_id,
        name, desc, LLAssetType::AT_SETTINGS, LLInventoryType::IT_SETTINGS,
        static_cast<U8>(settype), next_owner_perm, cb);
}


///----------------------------------------------------------------------------
/// Item copy pacing (#538)
///
/// Copying a large folder used to send one reliable CopyInventoryItem per item
/// in the same frame; the simulator acks them all but silently drops the excess
/// (40k-item copy: ~45% of items never created, zero failed resends). Copies
/// now wait in a strict FIFO and at most SLInventoryCopyMaxOutstanding of them
/// await the server's reply at once.
///
/// Inventory-safety invariants (do not relax):
///  - every message is the same CopyInventoryItem with the same fields as before,
///    sent once, in the order copy_inventory_item() was called; only WHEN changes;
///  - nothing is retried or resent; nothing is batched unless the #353 multi-block
///    option below is switched on (then still one block, one CallbackID per copy);
///  - a queued copy is never dropped silently (disconnect logs every one).
///
/// A slot is freed when the copy's callback fires, i.e. when the server's
/// UpdateCreateInventoryItem / BulkUpdateInventory reply carrying our CallbackID
/// arrives (LLInventoryModel::processUpdateCreateInventoryItem and the two
/// BulkUpdateInventory handlers -> LLInventoryCallbackManager::fire). The server
/// sends nothing usable on failure (fire() ignores null item ids), so a copy with
/// no reply after COPY_PACER_TIMEOUT frees its slot and is reported as
/// unconfirmed. It is NOT resent.
///
/// #353 (setting SLInventoryCopyMultiBlock, default FALSE): CopyInventoryItem's
/// InventoryData block is Variable in the message template, so up to
/// SLInventoryCopyBlocksPerMessage queued copies can share one message. Each block
/// keeps its own CallbackID / pacer token / outstanding slot, so per-item
/// confirmation, the outstanding limit, the timeout and the unconfirmed report are
/// unchanged; only the number of packets drops. Off by default because LL's viewer
/// only ever sends one block per CopyInventoryItem (this function was its only
/// sender), so how the simulator treats several blocks is unproven; flip it for
/// alt-account testing only (see research/inventory_perf_test_plan.md).
///----------------------------------------------------------------------------

namespace
{
    constexpr F64 COPY_PACER_TIMEOUT = 60.0;               // seconds without a reply
    constexpr size_t COPY_PACER_PROGRESS_QUEUE = 200;      // show progress above this
    constexpr F32 COPY_PACER_PROGRESS_INTERVAL = 10.f;     // seconds between progress tips
    // A folder copy enqueues items folder by folder as AIS creates the folders,
    // so the pacer can briefly run empty mid-copy; report only after it has
    // stayed empty this long, so one copy gives one summary.
    constexpr F32 COPY_PACER_SETTLE = 3.f;

    struct LLQueuedItemCopy
    {
        LLUUID mAgentID;
        LLUUID mCurrentOwner;
        LLUUID mItemID;
        LLUUID mParentID;
        std::string mNewName;
        LLPointer<LLInventoryCallback> mCB;
    };

    struct LLOutstandingItemCopy
    {
        LLUUID mItemID;
        F64 mSentAt;
    };

    struct LLCopyPacerState
    {
        std::deque<LLQueuedItemCopy> mQueue;                  // not sent yet, FIFO
        std::map<U64, LLOutstandingItemCopy> mOutstanding;    // sent, awaiting reply
        std::map<U64, LLUUID> mUnconfirmed;                   // timed out, no reply yet
        std::map<LLUUID, U32> mPendingSources;                // source item -> copies in any of the above
        U64 mNextToken = 1;
        bool mIdleRegistered = false;

        // Current burst (from first enqueue until queue and outstanding are empty).
        bool mInBurst = false;
        U32 mBurstQueued = 0;
        U32 mBurstSent = 0;
        U32 mBurstConfirmed = 0;
        U32 mBurstMessages = 0;     // CopyInventoryItem messages (== mBurstSent unless multi-block)
        bool mBurstShowedProgress = false;
        bool mSettling = false;
        LLTimer mBurstTimer;
        LLTimer mProgressTimer;
        LLTimer mSettleTimer;
    };

    // Never destroyed: queued callbacks must not be released during static
    // destruction (LLInventoryCopyPacer::shutdown() releases them at disconnect).
    LLCopyPacerState& copy_pacer()
    {
        static LLCopyPacerState* sState = new LLCopyPacerState;
        return *sState;
    }

    void copy_pacer_forget_source(const LLUUID& item_id)
    {
        LLCopyPacerState& s = copy_pacer();
        auto it = s.mPendingSources.find(item_id);
        if (it != s.mPendingSources.end() && --it->second == 0)
        {
            s.mPendingSources.erase(it);
        }
    }

    // A server reply for the copy identified by token. Runs at most once per
    // token (LLInventoryCallbackManager::fire erases a callback after firing it),
    // and each token lives in at most one of mOutstanding / mUnconfirmed, so a
    // slot can never be freed twice.
    void copy_pacer_on_reply(U64 token)
    {
        LLCopyPacerState& s = copy_pacer();
        auto out = s.mOutstanding.find(token);
        if (out != s.mOutstanding.end())
        {
            copy_pacer_forget_source(out->second.mItemID);
            s.mOutstanding.erase(out);
            ++s.mBurstConfirmed;
            // The next copy goes out from the idle pump (next frame), not from
            // inside this incoming-message handler.
            return;
        }
        auto late = s.mUnconfirmed.find(token);
        if (late != s.mUnconfirmed.end())
        {
            LL_INFOS("InventoryCopyPacer") << "Late confirmation (after timeout) for copy of item "
                                           << late->second << LL_ENDL;
            copy_pacer_forget_source(late->second);
            s.mUnconfirmed.erase(late);
            ++s.mBurstConfirmed;
            return;
        }
        // Reply after its burst was already reported, or after shutdown().
        LL_INFOS("InventoryCopyPacer") << "Late confirmation for an already reported copy (token "
                                       << token << ")" << LL_ENDL;
    }

    // Registered in gInventoryCallbacks in place of the caller's callback: frees
    // the pacer slot, then forwards to the caller's callback (which may be null)
    // exactly as a direct registration would have. Stays registered after a
    // timeout so a late reply still reaches the caller.
    class LLCopyPacerCallback : public LLInventoryCallback
    {
    public:
        LLCopyPacerCallback(U64 token, LLPointer<LLInventoryCallback> inner)
            : mToken(token), mInner(inner)
        {
        }

        void fire(const LLUUID& new_item_id) override
        {
            copy_pacer_on_reply(mToken);
            if (mInner)
            {
                mInner->fire(new_item_id);
            }
        }

    private:
        U64 mToken;
        LLPointer<LLInventoryCallback> mInner;
    };

    bool copy_pacer_can_send()
    {
        return gMessageSystem
            && !gDisconnected
            && gAgent.getRegion()   // sendReliableMessage() silently drops without one
            && !LLApp::isExiting()
            && !(LLAppViewer::instance() && LLAppViewer::instance()->logoutRequestSent());
    }

    // The original copy_inventory_item() body, split so that several blocks can
    // share one message (#353, off by default). The only difference on the wire
    // with one block per message: CallbackID is now always non-zero (the wrapper
    // is registered even for callers that passed a null callback, so the reply can
    // free the slot). CallbackID is an opaque token the server echoes back, not
    // item content.
    void copy_pacer_begin_message(const LLUUID& agent_id)
    {
        LLMessageSystem* msg = gMessageSystem;
        msg->newMessageFast(_PREHASH_CopyInventoryItem);
        msg->nextBlockFast(_PREHASH_AgentData);
        msg->addUUIDFast(_PREHASH_AgentID, agent_id);
        msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
    }

    void copy_pacer_add_block(const LLQueuedItemCopy& c)
    {
        LLCopyPacerState& s = copy_pacer();
        const U64 token = s.mNextToken++;
        LLPointer<LLInventoryCallback> wrapper = new LLCopyPacerCallback(token, c.mCB);
        const F64 now = LLTimer::getTotalSeconds();
        s.mOutstanding[token] = LLOutstandingItemCopy{ c.mItemID, now };
        ++s.mBurstSent;

        LLMessageSystem* msg = gMessageSystem;
        msg->nextBlockFast(_PREHASH_InventoryData);
        msg->addU32Fast(_PREHASH_CallbackID, gInventoryCallbacks.registerCB(wrapper));
        msg->addUUIDFast(_PREHASH_OldAgentID, c.mCurrentOwner);
        msg->addUUIDFast(_PREHASH_OldItemID, c.mItemID);
        msg->addUUIDFast(_PREHASH_NewFolderID, c.mParentID);
        msg->addStringFast(_PREHASH_NewName, c.mNewName);
    }

    void copy_pacer_idle(void*);

    void copy_pacer_finish_burst()
    {
        LLCopyPacerState& s = copy_pacer();
        if (!s.mInBurst)
        {
            return;
        }
        s.mInBurst = false;
        s.mSettling = false;
        if (s.mIdleRegistered)
        {
            gIdleCallbacks.deleteFunction(copy_pacer_idle, nullptr);
            s.mIdleRegistered = false;
        }

        const U32 unconfirmed = (U32)s.mUnconfirmed.size();
        LL_INFOS("InventoryCopyPacer") << "Item copies done: queued " << s.mBurstQueued
                                       << ", sent " << s.mBurstSent
                                       << " in " << s.mBurstMessages << " messages"
                                       << ", confirmed " << s.mBurstConfirmed
                                       << ", unconfirmed " << unconfirmed
                                       << ", elapsed " << s.mBurstTimer.getElapsedTimeF32() << "s" << LL_ENDL;
        if (s.mBurstMessages && s.mBurstMessages != s.mBurstSent)
        {
            // #353 multi-block audit line: one per burst.
            LL_INFOS("InventoryPerf") << "CopyInventoryItem multi-block burst: " << s.mBurstSent << " copies in "
                                      << s.mBurstMessages << " messages, confirmed " << s.mBurstConfirmed
                                      << ", unconfirmed " << unconfirmed << LL_ENDL;
        }
        if (unconfirmed)
        {
            for (const auto& entry : s.mUnconfirmed)
            {
                LL_INFOS("InventoryCopyPacer") << "Unconfirmed copy of item " << entry.second << LL_ENDL;
                copy_pacer_forget_source(entry.second);
            }
            s.mUnconfirmed.clear();
            LLSD args;
            args["COUNT"] = (S32)unconfirmed;
            LLNotificationsUtil::add("SLInventoryCopyUnconfirmed", args);
        }
        else if (s.mBurstShowedProgress)
        {
            LLSD args;
            args["COUNT"] = (S32)s.mBurstConfirmed;
            LLNotificationsUtil::add("SLInventoryCopyFinished", args);
        }
    }

    void copy_pacer_pump()
    {
        LLCopyPacerState& s = copy_pacer();

        // Free the slots of copies that never got a reply. They are NOT resent.
        const F64 now = LLTimer::getTotalSeconds();
        for (auto it = s.mOutstanding.begin(); it != s.mOutstanding.end();)
        {
            if (now - it->second.mSentAt > COPY_PACER_TIMEOUT)
            {
                LL_INFOS("InventoryCopyPacer") << "No reply after " << COPY_PACER_TIMEOUT
                                               << "s for copy of item " << it->second.mItemID
                                               << "; freeing its slot (not resent)" << LL_ENDL;
                s.mUnconfirmed[it->first] = it->second.mItemID;
                it = s.mOutstanding.erase(it);
            }
            else
            {
                ++it;
            }
        }

        if (!s.mQueue.empty() && copy_pacer_can_send())
        {
            static LLCachedControl<U32> max_outstanding(gSavedSettings, "SLInventoryCopyMaxOutstanding", 32);
            static LLCachedControl<bool> multi_block(gSavedSettings, "SLInventoryCopyMultiBlock", false);
            static LLCachedControl<U32> blocks_per_message(gSavedSettings, "SLInventoryCopyBlocksPerMessage", 8);
            const size_t limit = llmax((U32)max_outstanding, 1U);
            const size_t per_message = multi_block ? (size_t)llclamp((U32)blocks_per_message, 1U, 16U) : 1;
            while (!s.mQueue.empty() && s.mOutstanding.size() < limit)
            {
                // Strict FIFO: blocks are taken from the front only, so the send order is
                // exactly the enqueue order whether or not they share a message.
                const LLUUID agent_id = s.mQueue.front().mAgentID;
                copy_pacer_begin_message(agent_id);
                size_t blocks = 0;
                while (!s.mQueue.empty() && s.mOutstanding.size() < limit && blocks < per_message
                       && s.mQueue.front().mAgentID == agent_id)
                {
                    LLQueuedItemCopy c = std::move(s.mQueue.front());
                    s.mQueue.pop_front();
                    copy_pacer_add_block(c);
                    ++blocks;
                    if (gMessageSystem->isSendFull(nullptr))
                    {
                        break;
                    }
                }
                gAgent.sendReliableMessage();
                ++s.mBurstMessages;
                if (blocks > 1)
                {
                    LL_DEBUGS("InventoryPerf") << "CopyInventoryItem sent with " << blocks << " blocks" << LL_ENDL;
                }
            }
        }

        if (s.mQueue.size() > COPY_PACER_PROGRESS_QUEUE
            && (!s.mBurstShowedProgress || s.mProgressTimer.getElapsedTimeF32() > COPY_PACER_PROGRESS_INTERVAL))
        {
            s.mBurstShowedProgress = true;
            s.mProgressTimer.reset();
            LLSD args;
            args["DONE"] = (S32)s.mBurstConfirmed;
            args["TOTAL"] = (S32)s.mBurstQueued;
            args["WAITING"] = (S32)s.mQueue.size();
            LLNotificationsUtil::add("SLInventoryCopyProgress", args);
        }

        if (!s.mQueue.empty() || !s.mOutstanding.empty())
        {
            s.mSettling = false;
        }
        else if (!s.mSettling)
        {
            s.mSettling = true;
            s.mSettleTimer.reset();
        }
        else if (s.mSettleTimer.getElapsedTimeF32() > COPY_PACER_SETTLE)
        {
            copy_pacer_finish_burst();
        }
    }

    void copy_pacer_idle(void*)
    {
        copy_pacer_pump();
    }
}

// static
size_t LLInventoryCopyPacer::pendingCount()
{
    const LLCopyPacerState& s = copy_pacer();
    return s.mQueue.size() + s.mOutstanding.size();
}

// static
bool LLInventoryCopyPacer::hasPendingCopyFrom(const LLUUID& object_id)
{
    const LLCopyPacerState& s = copy_pacer();
    if (s.mPendingSources.empty() || object_id.isNull())
    {
        return false;
    }
    if (s.mPendingSources.count(object_id))
    {
        return true;
    }
    if (gInventory.getCategory(object_id))
    {
        for (const auto& entry : s.mPendingSources)
        {
            if (gInventory.isObjectDescendentOf(entry.first, object_id))
            {
                return true;
            }
        }
    }
    return false;
}

namespace
{
    void deferred_delete_shutdown();    // deferred deletes (#538), defined below
}

// static
void LLInventoryCopyPacer::shutdown()
{
    LLCopyPacerState& s = copy_pacer();
    if (!s.mQueue.empty() || !s.mOutstanding.empty() || !s.mUnconfirmed.empty())
    {
        LL_WARNS("InventoryCopyPacer") << "Disconnecting with " << s.mQueue.size()
                                       << " item copies never sent, " << s.mOutstanding.size()
                                       << " sent without a reply yet and " << s.mUnconfirmed.size()
                                       << " unconfirmed. Burst so far: queued " << s.mBurstQueued
                                       << ", sent " << s.mBurstSent << ", confirmed " << s.mBurstConfirmed
                                       << LL_ENDL;
        for (const LLQueuedItemCopy& c : s.mQueue)
        {
            LL_INFOS("InventoryCopyPacer") << "Never sent: copy of item " << c.mItemID
                                           << " into folder " << c.mParentID << LL_ENDL;
        }
        for (const auto& entry : s.mOutstanding)
        {
            LL_INFOS("InventoryCopyPacer") << "Sent, no reply: copy of item " << entry.second.mItemID << LL_ENDL;
        }
        for (const auto& entry : s.mUnconfirmed)
        {
            LL_INFOS("InventoryCopyPacer") << "Unconfirmed copy of item " << entry.second << LL_ENDL;
        }
    }
    s.mQueue.clear();   // releases the callers' callbacks without sending
    s.mOutstanding.clear();
    s.mUnconfirmed.clear();
    s.mPendingSources.clear();
    deferred_delete_shutdown();
    s.mInBurst = false;
    s.mSettling = false;
    if (s.mIdleRegistered)
    {
        gIdleCallbacks.deleteFunction(copy_pacer_idle, nullptr);
        s.mIdleRegistered = false;
    }
}

void copy_inventory_item(
    const LLUUID& agent_id,
    const LLUUID& current_owner,
    const LLUUID& item_id,
    const LLUUID& parent_id,
    const std::string& new_name,
    LLPointer<LLInventoryCallback> cb)
{
    // #461: log the request (at enqueue: the caller's site is known only here) and its answer.
    {
        const LLViewerInventoryItem* src = gInventory.getItem(item_id);
        cb = LLInvCreateLog::wrap(
            LLInvCreateLog::noteSent("copy", parent_id, src ? (S32)src->getType() : -1,
                                     !new_name.empty() ? new_name : (src ? src->getName() : std::string()),
                                     "of " + item_id.asString() + (current_owner != agent_id ? " from another owner" : "")),
            cb);
    }

    LLCopyPacerState& s = copy_pacer();
    if (!s.mInBurst)
    {
        s.mInBurst = true;
        s.mBurstQueued = 0;
        s.mBurstSent = 0;
        s.mBurstConfirmed = 0;
        s.mBurstMessages = 0;
        s.mBurstShowedProgress = false;
        s.mBurstTimer.reset();
    }
    if (!s.mIdleRegistered)
    {
        gIdleCallbacks.addFunction(copy_pacer_idle, nullptr);
        s.mIdleRegistered = true;
    }

    s.mQueue.push_back(LLQueuedItemCopy{ agent_id, current_owner, item_id, parent_id, new_name, cb });
    ++s.mPendingSources[item_id];
    ++s.mBurstQueued;

    // Goes out right away when a slot is free (single copies behave as before);
    // otherwise the idle pump sends it in order.
    copy_pacer_pump();
}

// Create link to single inventory object.
void link_inventory_object(const LLUUID& category,
             LLConstPointer<LLInventoryObject> baseobj,
             LLPointer<LLInventoryCallback> cb)
{
    if (!baseobj)
    {
        LL_WARNS(LOG_INV) << "Attempt to link to non-existent object" << LL_ENDL;
        return;
    }

    LLInventoryObject::const_object_list_t obj_array;
    obj_array.push_back(baseobj);
    link_inventory_array(category, obj_array, cb);
}

void link_inventory_object(const LLUUID& category,
             const LLUUID& id,
             LLPointer<LLInventoryCallback> cb)
{
    LLConstPointer<LLInventoryObject> baseobj = gInventory.getObject(id);
    link_inventory_object(category, baseobj, cb);
}

// Create links to all listed inventory objects.
void link_inventory_array(const LLUUID& category,
             LLInventoryObject::const_object_list_t& baseobj_array,
             LLPointer<LLInventoryCallback> cb)
{
#ifndef LL_RELEASE_FOR_DOWNLOAD
    const LLViewerInventoryCategory *cat = gInventory.getCategory(category);
    const std::string cat_name = cat ? cat->getName() : "CAT NOT FOUND";
#endif
    LLInventoryObject::const_object_list_t::const_iterator it = baseobj_array.begin();
    LLInventoryObject::const_object_list_t::const_iterator end = baseobj_array.end();
    LLSD links = LLSD::emptyArray();
    for (; it != end; ++it)
    {
        const LLInventoryObject* baseobj = *it;
        if (!baseobj)
        {
            LL_WARNS(LOG_INV) << "attempt to link to unknown object" << LL_ENDL;
            continue;
        }

        if (!LLAssetType::lookupCanLink(baseobj->getType()))
        {
            // Fail if item can be found but is of a type that can't be linked.
            // Arguably should fail if the item can't be found too, but that could
            // be a larger behavioral change.
            LL_WARNS(LOG_INV) << "attempt to link an unlinkable object, type = " << baseobj->getActualType() << LL_ENDL;
            continue;
        }

        LLInventoryType::EType inv_type = LLInventoryType::IT_NONE;
        LLAssetType::EType asset_type = LLAssetType::AT_NONE;
        std::string new_desc;
        LLUUID linkee_id;
        if (dynamic_cast<const LLInventoryCategory *>(baseobj))
        {
            inv_type = LLInventoryType::IT_CATEGORY;
            asset_type = LLAssetType::AT_LINK_FOLDER;
            linkee_id = baseobj->getUUID();
        }
        else
        {
            const LLViewerInventoryItem *baseitem = dynamic_cast<const LLViewerInventoryItem *>(baseobj);
            if (baseitem)
            {
                inv_type = baseitem->getInventoryType();
                new_desc = baseitem->getActualDescription();
                switch (baseitem->getActualType())
                {
                    case LLAssetType::AT_LINK:
                    case LLAssetType::AT_LINK_FOLDER:
                        linkee_id = baseobj->getLinkedUUID();
                        asset_type = baseitem->getActualType();
                        break;
                    default:
                        linkee_id = baseobj->getUUID();
                        asset_type = LLAssetType::AT_LINK;
                        break;
                }
            }
            else
            {
                LL_WARNS(LOG_INV) << "could not convert object into an item or category: " << baseobj->getUUID() << LL_ENDL;
                continue;
            }
        }

        LLSD link = LLSD::emptyMap();
        link["linked_id"] = linkee_id;
        link["type"] = (S8)asset_type;
        link["inv_type"] = (S8)inv_type;
        link["name"] = baseobj->getName();
        link["desc"] = new_desc;
        links.append(link);

#ifndef LL_RELEASE_FOR_DOWNLOAD
        LL_DEBUGS(LOG_INV) << "Linking Object [ name:" << baseobj->getName()
                           << " UUID:" << baseobj->getUUID()
                           << " ] into Category [ name:" << cat_name
                           << " UUID:" << category << " ] " << LL_ENDL;
#endif
    }
    LLSD new_inventory = LLSD::emptyMap();
    new_inventory["links"] = links;

    // #461: one line per request (the names are in the "Linking Object" DEBUG lines).
    if (links.size() > 0)
    {
        const std::string first = links[0]["name"].asString();
        cb = LLInvCreateLog::wrap(
            LLInvCreateLog::noteSent("link", category, links[0]["type"].asInteger(),
                                     links.size() == 1 ? first : first + llformat(" +%d more", (S32)links.size() - 1),
                                     "to " + links[0]["linked_id"].asUUID().asString()),
            cb);
    }

    AISAPI::completion_t cr = boost::bind(&doInventoryCb, cb, _1);
    AISAPI::CreateInventory(category, new_inventory, cr);
}

///----------------------------------------------------------------------------
/// #353: verified link batches (server writes: AIS CreateInventory with a "links"
/// array, the request link_inventory_array() has always sent; LL's own viewer sends
/// many links in one request in LLAppearanceMgr::shallowCopyCategoryContents,
/// LLFolderBridge outfit-from-folder (llinventorybridge.cpp) and
/// LLInventoryGallery, and for every COF update in LLAppearanceMgr).
///----------------------------------------------------------------------------
namespace
{
    // One alert per burst of failed link batches (a tree copy during an AIS outage
    // could otherwise stack one modal per subfolder): counts are summed and shown
    // 2 s after the first failure. Every batch is still logged individually.
    U32 sLinkAlertMissing = 0;
    U32 sLinkAlertRequested = 0;
    bool sLinkAlertScheduled = false;

    void show_link_batch_alert()
    {
        sLinkAlertScheduled = false;
        if (!sLinkAlertMissing || LLApp::isExiting())
        {
            sLinkAlertMissing = 0;
            sLinkAlertRequested = 0;
            return;
        }
        LLSD args;
        args["MISSING"] = (S32)sLinkAlertMissing;
        args["TOTAL"] = (S32)sLinkAlertRequested;
        sLinkAlertMissing = 0;
        sLinkAlertRequested = 0;
        LLNotificationsUtil::add("SLInventoryCopyLinksIncomplete", args);
    }

    class LLLinkBatchVerifier : public LLInventoryCallback
    {
    public:
        LLLinkBatchVerifier(const LLUUID& category, U32 requested, LLPointer<LLInventoryCallback> inner)
            : mCategory(category), mRequested(requested), mInner(inner)
        {
        }

        // AISAPI::InvokeAISCommandCoroEx calls this once per id in _created_items, or
        // once with a null id when the request failed or created nothing.
        void fire(const LLUUID& id) override
        {
            if (id.notNull())
            {
                ++mCreated;
                if (mInner)
                {
                    mInner->fire(id);
                }
            }
            else
            {
                ++mNullReplies;
            }
        }

        // Runs when the AIS request (and with it the last reference) is done.
        ~LLLinkBatchVerifier() override
        {
            const U32 missing = (mCreated < mRequested) ? (mRequested - mCreated) : 0;
            if (!missing)
            {
                LL_INFOS("InventoryPerf") << "Link batch into " << mCategory << ": " << mCreated << " of "
                                          << mRequested << " links created" << LL_ENDL;
                return;
            }
            LL_WARNS("InventoryPerf") << "Link batch into " << mCategory << ": " << mCreated << " of "
                                      << mRequested << " links created, " << missing << " NOT created"
                                      << (mNullReplies ? " (request failed)" : " (reply listed fewer links)")
                                      << "; not retried" << LL_ENDL;
            if (LLApp::isExiting())
            {
                return;
            }
            // Same callbacks the per-link path made for links that failed (one null fire
            // each): keeps e.g. the marketplace validation countdown in step.
            for (U32 i = 0; i < missing && mInner; ++i)
            {
                mInner->fire(LLUUID::null);
            }
            sLinkAlertMissing += missing;
            sLinkAlertRequested += mRequested;
            if (!sLinkAlertScheduled)
            {
                sLinkAlertScheduled = true;
                doAfterInterval(show_link_batch_alert, 2.f);
            }
        }

    private:
        LLUUID mCategory;
        U32 mRequested;
        U32 mCreated = 0;
        U32 mNullReplies = 0;
        LLPointer<LLInventoryCallback> mInner;
    };
}

void link_inventory_array_verified(const LLUUID& category,
                                   const LLInventoryObject::const_object_list_t& baseobj_array,
                                   LLPointer<LLInventoryCallback> cb)
{
    static LLCachedControl<U32> batch_size_setting(gSavedSettings, "SLInventoryCopyLinkBatchSize", 50);
    const size_t batch_size = llclamp((U32)batch_size_setting, 1U, 100U);

    // Pre-filter exactly as link_inventory_array() would, so 'requested' counts only
    // links that are really sent. Per-object parity with link_inventory_object():
    // a missing object made no request and no callback; an unlinkable one made an
    // empty request whose reply fired the callback once with a null id.
    LLInventoryObject::const_object_list_t chunk;
    U32 unlinkable = 0;
    for (const LLConstPointer<LLInventoryObject>& baseobj : baseobj_array)
    {
        if (!baseobj)
        {
            LL_WARNS(LOG_INV) << "Attempt to link to non-existent object" << LL_ENDL;
            continue;
        }
        if (!LLAssetType::lookupCanLink(baseobj->getType()))
        {
            LL_WARNS(LOG_INV) << "attempt to link an unlinkable object, type = " << baseobj->getActualType() << LL_ENDL;
            ++unlinkable;
            continue;
        }
        chunk.push_back(baseobj);
        if (chunk.size() >= batch_size)
        {
            link_inventory_array(category, chunk, new LLLinkBatchVerifier(category, (U32)chunk.size(), cb));
            chunk.clear();
        }
    }
    if (!chunk.empty())
    {
        link_inventory_array(category, chunk, new LLLinkBatchVerifier(category, (U32)chunk.size(), cb));
    }
    for (U32 i = 0; i < unlinkable && cb; ++i)
    {
        cb->fire(LLUUID::null);
    }
}

void move_inventory_item(
    const LLUUID& agent_id,
    const LLUUID& session_id,
    const LLUUID& item_id,
    const LLUUID& parent_id,
    const std::string& new_name,
    LLPointer<LLInventoryCallback> cb)
{
    LLMessageSystem* msg = gMessageSystem;
    msg->newMessageFast(_PREHASH_MoveInventoryItem);
    msg->nextBlockFast(_PREHASH_AgentData);
    msg->addUUIDFast(_PREHASH_AgentID, agent_id);
    msg->addUUIDFast(_PREHASH_SessionID, session_id);
    msg->addBOOLFast(_PREHASH_Stamp, false);
    msg->nextBlockFast(_PREHASH_InventoryData);
    msg->addUUIDFast(_PREHASH_ItemID, item_id);
    msg->addUUIDFast(_PREHASH_FolderID, parent_id);
    msg->addStringFast(_PREHASH_NewName, new_name);
    gAgent.sendReliableMessage();
}

// Should call this with an update_item that's been copied and
// modified from an original source item, rather than modifying the
// source item directly.
void update_inventory_item(
    LLViewerInventoryItem *update_item,
    LLPointer<LLInventoryCallback> cb)
{
    const LLUUID& item_id = update_item->getUUID();

    LLSD updates = update_item->asLLSD();
    // Replace asset_id and/or shadow_id with transaction_id (hash_id)
    if (updates.has("asset_id"))
    {
        updates.erase("asset_id");
        if (update_item->getTransactionID().notNull())
        {
            updates["hash_id"] = update_item->getTransactionID();
        }
    }
    if (updates.has("shadow_id"))
    {
        updates.erase("shadow_id");
        if (update_item->getTransactionID().notNull())
        {
            updates["hash_id"] = update_item->getTransactionID();
        }
    }
    AISAPI::completion_t cr = boost::bind(&doInventoryCb, cb, _1);
    AISAPI::UpdateItem(item_id, updates, cr);
}

// Note this only supports updating an existing item. Goes through AISv3
// code path where available. Not all uses of item->updateServer() can
// easily be switched to this paradigm.
void update_inventory_item(
    const LLUUID& item_id,
    const LLSD& updates,
    LLPointer<LLInventoryCallback> cb)
{
    AISAPI::completion_t cr = boost::bind(&doInventoryCb, cb, _1);
    AISAPI::UpdateItem(item_id, updates, cr);
}

void update_inventory_category(
    const LLUUID& cat_id,
    const LLSD& updates,
    LLPointer<LLInventoryCallback> cb)
{
    LLPointer<LLViewerInventoryCategory> obj = gInventory.getCategory(cat_id);
    LL_DEBUGS(LOG_INV) << "cat_id: [" << cat_id << "] name " << (obj ? obj->getName() : "(NOT FOUND)") << LL_ENDL;
    if(obj)
    {
        if (LLFolderType::lookupIsProtectedType(obj->getPreferredType())
            && (updates.size() != 1
                || !(updates.has("thumbnail") || updates.has("favorite"))))
        {
            LLNotificationsUtil::add("CannotModifyProtectedCategories");
            return;
        }

        AISAPI::completion_t cr = boost::bind(&doInventoryCb, cb, _1);
        AISAPI::UpdateCategory(cat_id, updates, cr);
    }
}

// #538: copies are sent later than copy_inventory_item() is called, so a
// permanent delete issued meanwhile could remove a copy's source before the
// server reads it. Such deletes are deferred, not refused: they wait in one
// FIFO and run automatically, in order, once no copy from the object is
// pending any more. A copy stops being pending when the server confirms it, or
// after it timed out (COPY_PACER_TIMEOUT) AND its burst ended (settle), i.e.
// when the pacer forgets the source. Moving to the trash is unaffected.
namespace
{
    struct LLDeferredDelete
    {
        LLUUID mObjectID;
        LLUUID mParentAtRequest;        // must be unchanged when it runs
        std::string mName;
        std::function<void()> mRun;     // the original call, re-issued as is
    };

    std::deque<LLDeferredDelete>& deferred_deletes()
    {
        static std::deque<LLDeferredDelete>* sQueue = new std::deque<LLDeferredDelete>;
        return *sQueue;
    }

    bool sDeferredDeleteIdle = false;
    constexpr F32 DEFERRED_DELETE_RECHECK = 1.f;   // seconds; the check walks the pending copies

    LLTimer& deferred_delete_timer()
    {
        static LLTimer* sTimer = new LLTimer;
        return *sTimer;
    }

    void deferred_delete_idle(void*)
    {
        if (deferred_delete_timer().getElapsedTimeF32() < DEFERRED_DELETE_RECHECK)
        {
            return;
        }
        deferred_delete_timer().reset();
        std::deque<LLDeferredDelete>& queue = deferred_deletes();
        while (!queue.empty() && !LLInventoryCopyPacer::hasPendingCopyFrom(queue.front().mObjectID))
        {
            LLDeferredDelete entry = std::move(queue.front());
            queue.pop_front();
            const LLInventoryObject* obj = gInventory.getObject(entry.mObjectID);
            if (!obj)
            {
                LL_INFOS("InventoryCopyPacer") << "Deferred delete of " << entry.mObjectID << " '" << entry.mName
                                               << "' dropped: it no longer exists" << LL_ENDL;
                continue;
            }
            if (obj->getParentUUID() != entry.mParentAtRequest)
            {
                // Moved (e.g. restored from the trash) after the delete was
                // requested: the user's confirmation no longer applies.
                LL_WARNS("InventoryCopyPacer") << "Deferred delete of " << entry.mObjectID << " '" << entry.mName
                                               << "' cancelled: it was moved after the delete was requested" << LL_ENDL;
                LLSD args;
                args["NAME"] = entry.mName;
                LLNotificationsUtil::add("SLInventoryDeferredDeleteCancelled", args);
                continue;
            }
            LL_INFOS("InventoryCopyPacer") << "Running deferred delete of " << entry.mObjectID << " '"
                                           << entry.mName << "'" << LL_ENDL;
            entry.mRun();   // passes the copy check now (same frame)
        }
        if (queue.empty() && sDeferredDeleteIdle)
        {
            gIdleCallbacks.deleteFunction(deferred_delete_idle, nullptr);
            sDeferredDeleteIdle = false;
        }
    }

    // Disconnect: the deferred deletes are not run; log every one.
    void deferred_delete_shutdown()
    {
        std::deque<LLDeferredDelete>& queue = deferred_deletes();
        for (const LLDeferredDelete& entry : queue)
        {
            LL_WARNS("InventoryCopyPacer") << "Disconnecting: deferred delete of " << entry.mObjectID << " '"
                                           << entry.mName << "' was not run (nothing deleted)" << LL_ENDL;
        }
        queue.clear();
        if (sDeferredDeleteIdle)
        {
            gIdleCallbacks.deleteFunction(deferred_delete_idle, nullptr);
            sDeferredDeleteIdle = false;
        }
    }
}

// True if the delete was deferred (run is kept and called later); false if it
// may proceed now.
static bool defer_delete_of_pending_copy_source(const LLUUID& object_id, std::function<void()> run)
{
    if (!LLInventoryCopyPacer::hasPendingCopyFrom(object_id))
    {
        return false;
    }
    const LLInventoryObject* obj = gInventory.getObject(object_id);
    LLDeferredDelete entry;
    entry.mObjectID = object_id;
    entry.mParentAtRequest = obj ? obj->getParentUUID() : LLUUID::null;
    entry.mName = obj ? obj->getName() : std::string();
    entry.mRun = std::move(run);
    deferred_deletes().push_back(std::move(entry));
    LL_INFOS("InventoryCopyPacer") << "Deferring permanent delete of " << object_id
                                   << ": item copies from it are still in progress ("
                                   << deferred_deletes().size() << " deletes waiting)" << LL_ENDL;
    if (!sDeferredDeleteIdle)
    {
        deferred_delete_timer().reset();
        gIdleCallbacks.addFunction(deferred_delete_idle, nullptr);
        sDeferredDeleteIdle = true;
    }
    LLSD args;
    args["COUNT"] = (S32)LLInventoryCopyPacer::pendingCount();
    LLNotificationsUtil::add("SLInventoryDeleteWaitsForCopies", args);
    return true;
}

void remove_inventory_items(
    LLInventoryObject::object_list_t& items_to_kill,
    LLPointer<LLInventoryCallback> cb
    )
{
    for (LLInventoryObject::object_list_t::iterator it = items_to_kill.begin();
         it != items_to_kill.end();
         ++it)
    {
        remove_inventory_item(*it, cb);
    }
}

void remove_inventory_item(
    const LLUUID& item_id,
    LLPointer<LLInventoryCallback> cb,
    bool immediate_delete)
{
    LLPointer<LLInventoryObject> obj = gInventory.getItem(item_id);
    if (obj)
    {
        remove_inventory_item(obj, cb, immediate_delete);
    }
    else
    {
        LL_DEBUGS(LOG_INV) << "item_id: [" << item_id << "] name " << "(NOT FOUND)" << LL_ENDL;
    }
}

void remove_inventory_item(
    LLPointer<LLInventoryObject> obj,
    LLPointer<LLInventoryCallback> cb,
    bool immediate_delete)
{
    if(obj)
    {
        const LLUUID item_id(obj->getUUID());
        LL_DEBUGS(LOG_INV) << "item_id: [" << item_id << "] name " << obj->getName() << LL_ENDL;
        if (defer_delete_of_pending_copy_source(item_id,
                [item_id, cb, immediate_delete]() { remove_inventory_item(item_id, cb, immediate_delete); }))
        {
            return;
        }
        if (AISAPI::isAvailable())
        {
            AISAPI::completion_t cr = (cb) ? boost::bind(&doInventoryCb, cb, _1) : AISAPI::completion_t();
            AISAPI::RemoveItem(item_id, cr);

            if (immediate_delete)
            {
                gInventory.onObjectDeletedFromServer(item_id);
            }
        }
        else
        {
            LL_WARNS(LOG_INV) << "Tried to use inventory without AIS API" << LL_ENDL;
        }
    }
    else
    {
        // *TODO: Clean up callback?
        LL_WARNS(LOG_INV) << "remove_inventory_item called for invalid or nonexistent item." << LL_ENDL;
    }
}

class LLRemoveCategoryOnDestroy: public LLInventoryCallback
{
public:
    LLRemoveCategoryOnDestroy(const LLUUID& cat_id, LLPointer<LLInventoryCallback> cb):
        mID(cat_id),
        mCB(cb)
    {
    }
    /* virtual */ void fire(const LLUUID& item_id) {}
    ~LLRemoveCategoryOnDestroy()
    {
        LLInventoryModel::EHasChildren children = gInventory.categoryHasChildren(mID);
        if(children != LLInventoryModel::CHILDREN_NO)
        {
            LL_WARNS(LOG_INV) << "remove descendents failed, cannot remove category " << LL_ENDL;
        }
        else
        {
            remove_inventory_category(mID, mCB);
        }
    }
private:
    LLUUID mID;
    LLPointer<LLInventoryCallback> mCB;
};

void remove_inventory_category(
    const LLUUID& cat_id,
    LLPointer<LLInventoryCallback> cb)
{
    LL_DEBUGS(LOG_INV) << "cat_id: [" << cat_id << "] " << LL_ENDL;
    LLPointer<LLViewerInventoryCategory> obj = gInventory.getCategory(cat_id);
    if(obj)
    {
        if (!gInventory.isCategoryComplete(cat_id))
        {
            LL_WARNS() << "Removing (purging) incomplete category " << obj->getName() << LL_ENDL;
        }
        if(LLFolderType::lookupIsProtectedType(obj->getPreferredType()))
        {
            LLNotificationsUtil::add("CannotRemoveProtectedCategories");
            return;
        }
        if (defer_delete_of_pending_copy_source(cat_id, [cat_id, cb]() { remove_inventory_category(cat_id, cb); }))
        {
            return;
        }
        if (LLChunkedPurge::overlapsActiveJob(cat_id))
        {
            LL_INFOS("ChunkedPurge") << "Not removing " << cat_id << " now: a delete in parts of it, or around it, is running" << LL_ENDL;
            LLSD args;
            args["FOLDER"] = obj->getName();
            LLNotificationsUtil::add("SLChunkedPurgeBusy", args);
            return;
        }
        AISAPI::completion_t cr = boost::bind(&doInventoryCb, cb, _1);
        // "Too many subcategories": delete the same folder in parts instead
        // (the user already confirmed deleting it). Any other failure fires the
        // callback as before.
        AISAPI::failure_t on_fail = [cat_id, cb](S32 status, const std::string& error)
        {
            if (AISAPI::isTooManySubcategories(status, error))
            {
                LLChunkedPurge::start(cat_id, LLChunkedPurge::REMOVE_FOLDER, cb);
            }
            else
            {
                doInventoryCb(cb, LLUUID::null);
            }
        };
        AISAPI::RemoveCategory(cat_id, cr, on_fail);
    }
    else
    {
        LL_WARNS(LOG_INV) << "remove_inventory_category called for invalid or nonexistent item " << cat_id << LL_ENDL;
    }
}

void remove_inventory_object(
    const LLUUID& object_id,
    LLPointer<LLInventoryCallback> cb)
{
    if (gInventory.getCategory(object_id))
    {
        remove_inventory_category(object_id, cb);
    }
    else
    {
        remove_inventory_item(object_id, cb);
    }
}

// This is a method which collects the descendents of the id
// provided. If the category is not found, no action is
// taken. This method goes through the long winded process of
// cancelling any calling cards, removing server representation of
// folders, items, etc in a fairly efficient manner.
void purge_descendents_of(const LLUUID& id, LLPointer<LLInventoryCallback> cb)
{
    LLInventoryModel::EHasChildren children = gInventory.categoryHasChildren(id);
    if(children == LLInventoryModel::CHILDREN_NO)
    {
        LL_DEBUGS(LOG_INV) << "No descendents to purge for " << id << LL_ENDL;
        return;
    }
    if (defer_delete_of_pending_copy_source(id, [id, cb]() { purge_descendents_of(id, cb); }))
    {
        return;
    }
    if (LLChunkedPurge::overlapsActiveJob(id))
    {
        LL_INFOS("ChunkedPurge") << "Not purging " << id << " now: a delete in parts of it, or around it, is running" << LL_ENDL;
        LLViewerInventoryCategory* busy = gInventory.getCategory(id);
        LLSD args;
        args["FOLDER"] = busy ? busy->getName() : std::string();
        LLNotificationsUtil::add("SLChunkedPurgeBusy", args);
        return;
    }
    LLPointer<LLViewerInventoryCategory> cat = gInventory.getCategory(id);
    if (cat.notNull())
    {
        if (LLClipboard::instance().hasContents())
        {
            // Remove items from clipboard or it will remain active even if there is nothing to paste/copy
            LLInventoryModel::cat_array_t categories;
            LLInventoryModel::item_array_t items;
            gInventory.collectDescendents(id, categories, items, true);

            for (LLInventoryModel::cat_array_t::const_iterator it = categories.begin(); it != categories.end(); ++it)
            {
                if (LLClipboard::instance().isOnClipboard((*it)->getUUID()))
                {
                    // No sense in removing single items, partial 'paste' will result in confusion only
                    LLClipboard::instance().reset();
                    break;
                }
            }
            if (LLClipboard::instance().hasContents())
            {
                for (LLInventoryModel::item_array_t::const_iterator it = items.begin(); it != items.end(); ++it)
                {
                    if (LLClipboard::instance().isOnClipboard((*it)->getUUID()))
                    {
                        LLClipboard::instance().reset();
                        break;
                    }
                }
            }
        }

        if (AISAPI::isAvailable())
        {
            if (cat->getVersion() == LLViewerInventoryCategory::VERSION_UNKNOWN)
            {
                LL_WARNS() << "Purging not fetched folder: " << cat->getName() << LL_ENDL;
            }
            AISAPI::completion_t cr = (cb) ? boost::bind(&doInventoryCb, cb, _1) : AISAPI::completion_t();
            // "Too many subcategories" (e.g. Empty Trash on a 14k-folder Trash):
            // empty the same folder in parts instead (the user already confirmed
            // it). Any other failure fires the callback as before.
            AISAPI::failure_t on_fail = [id, cb](S32 status, const std::string& error)
            {
                if (AISAPI::isTooManySubcategories(status, error))
                {
                    LLChunkedPurge::start(id, LLChunkedPurge::PURGE_CHILDREN, cb);
                }
                else
                {
                    doInventoryCb(cb, LLUUID::null);
                }
            };
            AISAPI::PurgeDescendents(id, cr, on_fail);
        }
        else
        {
            LL_WARNS(LOG_INV) << "Tried to use inventory without AIS API" << LL_ENDL;
        }
    }
}

const LLUUID get_folder_by_itemtype(const LLInventoryItem *src)
{
    LLUUID retval = LLUUID::null;

    if (src)
    {
        retval = gInventory.findCategoryUUIDForType(LLFolderType::assetTypeToFolderType(src->getType()));
    }

    return retval;
}

void copy_inventory_from_notecard(const LLUUID& destination_id,
                                  const LLUUID& object_id,
                                  const LLUUID& notecard_inv_id,
                                  const LLInventoryItem *src,
                                  U32 callback_id)
{
    if (NULL == src)
    {
        LL_WARNS(LOG_NOTECARD) << "Null pointer to item was passed for object_id "
                               << object_id << " and notecard_inv_id "
                               << notecard_inv_id << LL_ENDL;
        return;
    }

    LLViewerRegion* viewer_region = NULL;
    LLViewerObject* vo = NULL;
    if (object_id.notNull() && (vo = gObjectList.findObject(object_id)) != NULL)
    {
        viewer_region = vo->getRegion();
    }

    // Fallback to the agents region if for some reason the
    // object isn't found in the viewer.
    if (! viewer_region)
    {
        viewer_region = gAgent.getRegion();
    }

    if (! viewer_region)
    {
        LL_WARNS(LOG_NOTECARD) << "Can't find region from object_id "
                               << object_id << " or gAgent"
                               << LL_ENDL;
        return;
    }

    LLSD body;
    body["notecard-id"] = notecard_inv_id;
    body["object-id"] = object_id;
    body["item-id"] = src->getUUID();
    body["folder-id"] = destination_id;
    body["callback-id"] = (LLSD::Integer)callback_id;

    /// *TODO: RIDER: This posts the request under the agents policy.
    /// When I convert the inventory over this call should be moved under that
    /// policy as well.
    if (!gAgent.requestPostCapability("CopyInventoryFromNotecard", body))
    {
        LL_WARNS() << "SIM does not have the capability to copy from notecard." << LL_ENDL;
    }
}

void create_new_item(const std::string& name,
                   const LLUUID& parent_id,
                   LLAssetType::EType asset_type,
                   LLInventoryType::EType inv_type,
                   U32 next_owner_perm,
                   std::function<void(const LLUUID&)> created_cb = NULL)
{
    std::string desc;
    LLViewerAssetType::generateDescriptionFor(asset_type, desc);
    next_owner_perm = (next_owner_perm) ? next_owner_perm : PERM_MOVE | PERM_TRANSFER;

    LLPointer<LLBoostFuncInventoryCallback> cb = NULL;

    switch (inv_type)
    {
        case LLInventoryType::IT_LSL:
        {
            cb = new LLBoostFuncInventoryCallback(create_script_cb);
            next_owner_perm = LLFloaterPerms::getNextOwnerPerms("Scripts");
            break;
        }

        case LLInventoryType::IT_GESTURE:
        {
            cb = new LLBoostFuncInventoryCallback(create_gesture_cb);
            next_owner_perm = LLFloaterPerms::getNextOwnerPerms("Gestures");
            break;
        }

        case LLInventoryType::IT_NOTECARD:
        {
            cb = new LLBoostFuncInventoryCallback(create_notecard_cb);
            next_owner_perm = LLFloaterPerms::getNextOwnerPerms("Notecards");
            break;
        }

        case LLInventoryType::IT_MATERIAL:
        {
            cb = new LLBoostFuncInventoryCallback(create_gltf_material_cb);
            next_owner_perm = LLFloaterPerms::getNextOwnerPerms("Materials");
            break;
        }
        default:
        {
            cb = new LLBoostFuncInventoryCallback();
            break;
        }
    }
    if (created_cb != NULL)
    {
        cb->addOnFireFunc(created_cb);
    }

    create_inventory_item(gAgent.getID(),
                          gAgent.getSessionID(),
                          parent_id,
                          LLTransactionID::tnull,
                          name,
                          desc,
                          asset_type,
                          inv_type,
                          NO_INV_SUBTYPE,
                          next_owner_perm,
                          cb);
}

// [RLVa:KB] - Checked: 2014-11-02 (RLVa-1.4.11)
void sync_inventory_folder(const LLUUID& folder_id, const LLInventoryModel::item_array_t& items, LLInventoryModel::item_array_t& items_to_add, LLInventoryModel::item_array_t& items_to_remove)
{
    LLInventoryModel::item_array_t curItems, newItems = items;

    // Grab the current contents
    LLInventoryModel::cat_array_t cats;
    gInventory.collectDescendents(folder_id, cats, curItems, LLInventoryModel::EXCLUDE_TRASH);

    // Purge everything in curItems that isn't part of newItems
    for (LLInventoryModel::item_array_t::const_iterator itCurItem = curItems.begin(); itCurItem != curItems.end(); ++itCurItem)
    {
        LLViewerInventoryItem* pItem = *itCurItem;
        if (std::find_if(newItems.begin(), newItems.end(), RlvPredIsEqualOrLinkedItem(pItem)) == newItems.end())
        {
            // Item doesn't exist in newItems => purge (if it's a link)
            if ( (pItem->getIsLinkType()) &&
                 (LLAssetType::AT_LINK_FOLDER != pItem->getActualType()) &&
                 (items_to_remove.end() == std::find(items_to_remove.begin(), items_to_remove.end(), pItem)) )
            {
                items_to_remove.push_back(pItem);
            }
        }
        else
        {
            // Item exists in newItems => remove *all* occurances in newItems (removes duplicate COF links to this item as well)
            newItems.erase(std::remove_if(newItems.begin(), newItems.end(), RlvPredIsEqualOrLinkedItem(pItem)), newItems.end());
        }
    }

    // Whatever remains in newItems will need to have a link created
    for (LLInventoryModel::item_array_t::const_iterator itNewItem = newItems.begin(); itNewItem != newItems.end(); ++itNewItem)
    {
        LLViewerInventoryItem* pItem = *itNewItem;
        if (items_to_add.end() == std::find(items_to_add.begin(), items_to_add.end(), pItem))
            items_to_add.push_back(pItem);
    }
}

void link_inventory_items(const LLUUID& folder_id, const LLInventoryModel::item_array_t& items, LLPointer<LLInventoryCallback> cb)
{
    for (LLInventoryModel::item_array_t::const_iterator itItem = items.begin(); itItem != items.end(); ++itItem)
    {
        const LLViewerInventoryItem* pItem = *itItem;
        link_inventory_object(folder_id, pItem, cb);
    }
}

void remove_inventory_items(const LLInventoryModel::item_array_t& items, LLPointer<LLInventoryCallback> cb)
{
    for (LLInventoryModel::item_array_t::const_iterator itItem = items.begin(); itItem != items.end(); ++itItem)
    {
        const LLViewerInventoryItem* pItem = *itItem;
        if (pItem->getIsLinkType())
            remove_inventory_item(pItem->getUUID(), cb);
    }
}
// [/RLVa:KB]

void slam_inventory_folder(const LLUUID& folder_id,
                           const LLSD& contents,
                           LLPointer<LLInventoryCallback> cb)
{
    LL_DEBUGS(LOG_INV) << "using AISv3 to slam folder, id " << folder_id
                       << " new contents: " << ll_pretty_print_sd(contents) << LL_ENDL;

    // #461: a slam replaces the folder's links with these (the COF rebuild at login).
    const S32 link_count = contents.has("links") ? (S32)contents["links"].size() : 0;
    cb = LLInvCreateLog::wrap(LLInvCreateLog::noteSent("slam", folder_id, (S32)LLAssetType::AT_LINK,
                                                       llformat("%d links", link_count)),
                              cb);

    AISAPI::completion_t cr = boost::bind(&doInventoryCb, cb, _1);
    AISAPI::SlamFolder(folder_id, contents, cr);
}

void remove_folder_contents(const LLUUID& category, bool keep_outfit_links,
                            LLPointer<LLInventoryCallback> cb)
{
    LLInventoryModel::cat_array_t cats;
    LLInventoryModel::item_array_t items;
    gInventory.collectDescendents(category, cats, items,
                                  LLInventoryModel::EXCLUDE_TRASH);
    for (S32 i = 0; i < items.size(); ++i)
    {
        LLViewerInventoryItem *item = items.at(i);
        if (keep_outfit_links && (item->getActualType() == LLAssetType::AT_LINK_FOLDER))
            continue;
        if (item->getIsLinkType())
        {
            remove_inventory_item(item->getUUID(), cb);
        }
    }
}

const std::string NEW_LSL_NAME = "New Script"; // *TODO:Translate? (probably not)
const std::string NEW_NOTECARD_NAME = "New Note"; // *TODO:Translate? (probably not)
const std::string NEW_GESTURE_NAME = "New Gesture"; // *TODO:Translate? (probably not)
const std::string NEW_MATERIAL_NAME = "New Material"; // *TODO:Translate? (probably not)

// ! REFACTOR ! Really need to refactor this so that it's not a bunch of if-then statements...
void menu_create_inventory_item(LLInventoryPanel* panel, LLFolderBridge *bridge, const LLSD& userdata, const LLUUID& default_parent_uuid)
{
    menu_create_inventory_item(panel, bridge ? bridge->getUUID() : LLUUID::null, userdata, default_parent_uuid);
}

void menu_create_inventory_item(LLInventoryPanel* panel, LLUUID dest_id, const LLSD& userdata, const LLUUID& default_parent_uuid, std::function<void(const LLUUID&)> created_cb)
{
    std::string type_name = userdata.asString();

    if (("inbox" == type_name) || ("category" == type_name) || ("current" == type_name) || ("outfit" == type_name) || ("my_otfts" == type_name))
    {
        LLFolderType::EType preferred_type = LLFolderType::lookup(type_name);

        LLUUID parent_id;
        if (dest_id.notNull())
        {
            parent_id = dest_id;
        }
        else if (default_parent_uuid.notNull())
        {
            parent_id = default_parent_uuid;
        }
        else
        {
            parent_id = gInventory.getRootFolderID();
        }

        std::function<void(const LLUUID&)> callback_cat_created = NULL;
        if (panel)
        {
            LLHandle<LLPanel> handle = panel->getHandle();
            callback_cat_created = [handle](const LLUUID& new_category_id)
            {
                gInventory.notifyObservers();
                LLInventoryPanel* panel = static_cast<LLInventoryPanel*>(handle.get());
                if (panel)
                {
                    panel->setSelectionByID(new_category_id, true);
                }
                LL_DEBUGS(LOG_INV) << "Done creating inventory: " << new_category_id << LL_ENDL;
            };
        }
        else if (created_cb != NULL)
        {
            callback_cat_created = created_cb;
        }
        gInventory.createNewCategory(
            parent_id,
            preferred_type,
            LLStringUtil::null,
            callback_cat_created);
    }
    else if ("lsl" == type_name)
    {
        const LLUUID parent_id = dest_id.notNull() ? dest_id : gInventory.findCategoryUUIDForType(LLFolderType::FT_LSL_TEXT);
        if (LLViewerInventoryItem* default_script = SLPrimDefaults::getDefaultScriptItem())
        {
            // Custom default new script (Preferences > Building, #466): a
            // server-side copy named like a stock new script.
            LLPointer<LLBoostFuncInventoryCallback> cb = new LLBoostFuncInventoryCallback(copy_default_script_cb);
            if (created_cb != NULL)
            {
                cb->addOnFireFunc(created_cb);
            }
            copy_inventory_item(gAgent.getID(),
                                default_script->getPermissions().getOwner(),
                                default_script->getUUID(),
                                parent_id,
                                NEW_LSL_NAME,
                                cb);
        }
        else
        {
            create_new_item(NEW_LSL_NAME,
                          parent_id,
                          LLAssetType::AT_LSL_TEXT,
                          LLInventoryType::IT_LSL,
                          PERM_MOVE | PERM_TRANSFER,
                          created_cb);    // overridden in create_new_item
        }
    }
    else if ("notecard" == type_name)
    {
        const LLUUID parent_id = dest_id.notNull() ? dest_id : gInventory.findCategoryUUIDForType(LLFolderType::FT_NOTECARD);
        create_new_item(NEW_NOTECARD_NAME,
                      parent_id,
                      LLAssetType::AT_NOTECARD,
                      LLInventoryType::IT_NOTECARD,
                      PERM_ALL,
                      created_cb);    // overridden in create_new_item
    }
    else if ("gesture" == type_name)
    {
        const LLUUID parent_id = dest_id.notNull() ? dest_id : gInventory.findCategoryUUIDForType(LLFolderType::FT_GESTURE);
        create_new_item(NEW_GESTURE_NAME,
                      parent_id,
                      LLAssetType::AT_GESTURE,
                      LLInventoryType::IT_GESTURE,
                      PERM_ALL,
                      created_cb);    // overridden in create_new_item
    }
    else if ("material" == type_name)
    {
        const LLUUID parent_id = dest_id.notNull() ? dest_id : gInventory.findCategoryUUIDForType(LLFolderType::FT_MATERIAL);
        create_new_item(NEW_MATERIAL_NAME,
            parent_id,
            LLAssetType::AT_MATERIAL,
            LLInventoryType::IT_MATERIAL,
            PERM_ALL,
            created_cb);    // overridden in create_new_item
    }
    else if (("sky" == type_name) || ("water" == type_name) || ("daycycle" == type_name))
    {
        LLSettingsType::type_e stype(LLSettingsType::ST_NONE);

        if ("sky" == type_name)
        {
            stype = LLSettingsType::ST_SKY;
        }
        else if ("water" == type_name)
        {
            stype = LLSettingsType::ST_WATER;
        }
        else if ("daycycle" == type_name)
        {
            stype = LLSettingsType::ST_DAYCYCLE;
        }
        else
        {
            LL_ERRS(LOG_INV) << "Unknown settings type: '" << type_name << "'" << LL_ENDL;
            return;
        }

        LLUUID parent_id = dest_id.notNull() ? dest_id : gInventory.findCategoryUUIDForType(LLFolderType::FT_SETTINGS);

        LLSettingsVOBase::createNewInventoryItem(stype, parent_id, created_cb);
    }
    else
    {
        // Use for all clothing and body parts.  Adding new wearable types requires updating LLWearableDictionary.
        LLWearableType::EType wearable_type = LLWearableType::getInstance()->typeNameToType(type_name);
        if (wearable_type >= LLWearableType::WT_SHAPE && wearable_type < LLWearableType::WT_COUNT)
        {
            const LLUUID parent_id = dest_id;
            LLAgentWearables::createWearable(wearable_type, false, parent_id, created_cb);
        }
        else
        {
            LL_WARNS(LOG_INV) << "Can't create unrecognized type " << type_name << LL_ENDL;
        }
    }
    if(panel)
    {
        panel->getRootFolder()->setNeedsAutoRename(true);
    }
}

LLAssetType::EType LLViewerInventoryItem::getType() const
{
    if (const LLViewerInventoryItem *linked_item = getLinkedItem())
    {
        return linked_item->getType();
    }
    if (const LLViewerInventoryCategory *linked_category = getLinkedCategory())
    {
        return linked_category->getType();
    }
    return LLInventoryItem::getType();
}

const LLUUID& LLViewerInventoryItem::getAssetUUID() const
{
    if (const LLViewerInventoryItem *linked_item = getLinkedItem())
    {
        return linked_item->getAssetUUID();
    }

    return LLInventoryItem::getAssetUUID();
}

const LLUUID& LLViewerInventoryItem::getProtectedAssetUUID() const
{
    if (const LLViewerInventoryItem *linked_item = getLinkedItem())
    {
        return linked_item->getProtectedAssetUUID();
    }

    // check for conditions under which we may return a visible UUID to the user
    bool item_is_fullperm = getIsFullPerm();
    bool agent_is_godlike = gAgent.isGodlikeWithoutAdminMenuFakery();
    if (item_is_fullperm || agent_is_godlike)
    {
        return LLInventoryItem::getAssetUUID();
    }

    return LLUUID::null;
}

const bool LLViewerInventoryItem::getIsFullPerm() const
{
    LLPermissions item_permissions = getPermissions();

    // modify-ok & copy-ok & transfer-ok
    return ( item_permissions.allowOperationBy(PERM_MODIFY,
                           gAgent.getID(),
                           gAgent.getGroupID()) &&
         item_permissions.allowOperationBy(PERM_COPY,
                           gAgent.getID(),
                           gAgent.getGroupID()) &&
         item_permissions.allowOperationBy(PERM_TRANSFER,
                           gAgent.getID(),
                           gAgent.getGroupID()) );
}

const std::string& LLViewerInventoryItem::getName() const
{
    if (const LLViewerInventoryItem *linked_item = getLinkedItem())
    {
        return linked_item->getName();
    }
    if (const LLViewerInventoryCategory *linked_category = getLinkedCategory())
    {
        return linked_category->getName();
    }

    return  LLInventoryItem::getName();
}

S32 LLViewerInventoryItem::getSortField() const
{
    return LLFavoritesOrderStorage::instance().getSortIndex(mUUID);
}

//void LLViewerInventoryItem::setSortField(S32 sortField)
//{
//  LLFavoritesOrderStorage::instance().setSortIndex(mUUID, sortField);
//  getSLURL();
//}

void LLViewerInventoryItem::getSLURL()
{
    LLFavoritesOrderStorage::instance().getSLURL(mAssetUUID);
}

const LLPermissions& LLViewerInventoryItem::getPermissions() const
{
    // Use the actual permissions of the symlink, not its parent.
    return LLInventoryItem::getPermissions();
}

const LLUUID& LLViewerInventoryItem::getCreatorUUID() const
{
    if (const LLViewerInventoryItem *linked_item = getLinkedItem())
    {
        return linked_item->getCreatorUUID();
    }

    return LLInventoryItem::getCreatorUUID();
}

const std::string& LLViewerInventoryItem::getDescription() const
{
    if (const LLViewerInventoryItem *linked_item = getLinkedItem())
    {
        return linked_item->getDescription();
    }

    return LLInventoryItem::getDescription();
}

const LLSaleInfo& LLViewerInventoryItem::getSaleInfo() const
{
    if (const LLViewerInventoryItem *linked_item = getLinkedItem())
    {
        return linked_item->getSaleInfo();
    }

    return LLInventoryItem::getSaleInfo();
}

const LLUUID& LLViewerInventoryItem::getThumbnailUUID() const
{
    if (mThumbnailUUID.isNull() && mType == LLAssetType::AT_TEXTURE)
    {
        return mAssetUUID;
    }
    if (mThumbnailUUID.isNull() && mType == LLAssetType::AT_LINK)
    {
        LLViewerInventoryItem *linked_item = gInventory.getItem(mAssetUUID);
        return linked_item ? linked_item->getThumbnailUUID() : LLUUID::null;
    }
    if (mThumbnailUUID.isNull() && mType == LLAssetType::AT_LINK_FOLDER)
    {
        LLViewerInventoryCategory *linked_cat = gInventory.getCategory(mAssetUUID);
        return linked_cat ? linked_cat->getThumbnailUUID() : LLUUID::null;
    }
    return mThumbnailUUID;
}

LLInventoryType::EType LLViewerInventoryItem::getInventoryType() const
{
    if (const LLViewerInventoryItem *linked_item = getLinkedItem())
    {
        return linked_item->getInventoryType();
    }

    // Categories don't have types.  If this item is an AT_FOLDER_LINK,
    // treat it as a category.
    if (getLinkedCategory())
    {
        return LLInventoryType::IT_CATEGORY;
    }

    return LLInventoryItem::getInventoryType();
}

U32 LLViewerInventoryItem::getFlags() const
{
    if (const LLViewerInventoryItem *linked_item = getLinkedItem())
    {
        return linked_item->getFlags();
    }
    return LLInventoryItem::getFlags();
}

bool LLViewerInventoryItem::isWearableType() const
{
    return (getInventoryType() == LLInventoryType::IT_WEARABLE);
}

LLWearableType::EType LLViewerInventoryItem::getWearableType() const
{
    if (!isWearableType())
    {
        return LLWearableType::WT_INVALID;
    }
    return LLWearableType::inventoryFlagsToWearableType(getFlags());
}

bool LLViewerInventoryItem::isSettingsType() const
{
    return (getInventoryType() == LLInventoryType::IT_SETTINGS);
}

LLSettingsType::type_e LLViewerInventoryItem::getSettingsType() const
{
    if (!isSettingsType())
    {
        return LLSettingsType::ST_NONE;
    }
    return LLSettingsType::fromInventoryFlags(getFlags());
}

time_t LLViewerInventoryItem::getCreationDate() const
{
    return LLInventoryItem::getCreationDate();
}

U32 LLViewerInventoryItem::getCRC32() const
{
    return LLInventoryItem::getCRC32();
}

// *TODO: mantipov: should be removed with LMSortPrefix patch in llinventorymodel.cpp, EXT-3985
static char getSeparator() { return '@'; }
bool LLViewerInventoryItem::extractSortFieldAndDisplayName(const std::string& name, S32* sortField, std::string* displayName)
{
    using std::string;
    using std::stringstream;

    const char separator = getSeparator();
    const string::size_type separatorPos = name.find(separator, 0);

    bool result = false;

    if (separatorPos < string::npos)
    {
        if (sortField)
        {
            /*
             * The conversion from string to S32 is made this way instead of old plain
             * atoi() to ensure portability. If on some other platform S32 will not be
             * defined to be signed int, this conversion will still work because of
             * operators overloading, but atoi() may fail.
             */
            stringstream ss(name.substr(0, separatorPos));
            ss >> *sortField;
        }

        if (displayName)
        {
            *displayName = name.substr(separatorPos + 1, string::npos);
        }

        result = true;
    }

    return result;
}

// This returns true if the item that this item points to
// doesn't exist in memory (i.e. LLInventoryModel).  The baseitem
// might still be in the database but just not loaded yet.
bool LLViewerInventoryItem::getIsBrokenLink() const
{
    // If the item's type resolves to be a link, that means either:
    // A. It wasn't able to perform indirection, i.e. the baseobj doesn't exist in memory.
    // B. It's pointing to another link, which is illegal.
    return LLAssetType::lookupIsLinkType(getType());
}

LLViewerInventoryItem *LLViewerInventoryItem::getLinkedItem() const
{
    if (mType == LLAssetType::AT_LINK)
    {
        LLViewerInventoryItem *linked_item = gInventory.getItem(mAssetUUID);
        if (linked_item && linked_item->getIsLinkType())
        {
            LL_WARNS(LOG_INV) << "Warning: Accessing link to link" << LL_ENDL;
            return NULL;
        }
        return linked_item;
    }
    return NULL;
}

LLViewerInventoryCategory *LLViewerInventoryItem::getLinkedCategory() const
{
    if (mType == LLAssetType::AT_LINK_FOLDER)
    {
        LLViewerInventoryCategory *linked_category = gInventory.getCategory(mAssetUUID);
        return linked_category;
    }
    return NULL;
}

bool LLViewerInventoryItem::checkPermissionsSet(PermissionMask mask) const
{
    const LLPermissions& perm = getPermissions();
    PermissionMask curr_mask = PERM_NONE;
    if(perm.getOwner() == gAgent.getID())
    {
        curr_mask = perm.getMaskBase();
    }
    else if(gAgent.isInGroup(perm.getGroup()))
    {
        curr_mask = perm.getMaskGroup();
    }
    else
    {
        curr_mask = perm.getMaskEveryone();
    }
    return ((curr_mask & mask) == mask);
}

PermissionMask LLViewerInventoryItem::getPermissionMask() const
{
    const LLPermissions& permissions = getPermissions();

    bool copy = permissions.allowCopyBy(gAgent.getID());
    bool mod = permissions.allowModifyBy(gAgent.getID());
    bool xfer = permissions.allowOperationBy(PERM_TRANSFER, gAgent.getID());
    PermissionMask perm_mask = 0;
    if (copy) perm_mask |= PERM_COPY;
    if (mod)  perm_mask |= PERM_MODIFY;
    if (xfer) perm_mask |= PERM_TRANSFER;
    return perm_mask;
}

//----------

void LLViewerInventoryItem::onCallingCardNameLookup(const LLUUID& id, const LLAvatarName& name)
{
    rename(name.getUserName());
    gInventory.addChangedMask(LLInventoryObserver::LABEL, getUUID());
    gInventory.notifyObservers();
}

class LLRegenerateLinkCollector : public LLInventoryCollectFunctor
{
public:
    LLRegenerateLinkCollector(const LLViewerInventoryItem *target_item) : mTargetItem(target_item) {}
    virtual ~LLRegenerateLinkCollector() {}
    virtual bool operator()(LLInventoryCategory* cat,
                            LLInventoryItem* item)
    {
        if (item)
        {
            if ((item->getName() == mTargetItem->getName()) &&
                (item->getInventoryType() == mTargetItem->getInventoryType()) &&
                (!item->getIsLinkType()))
            {
                return true;
            }
        }
        return false;
    }
protected:
    const LLViewerInventoryItem* mTargetItem;
};

LLUUID find_possible_item_for_regeneration(const LLViewerInventoryItem *target_item)
{
    LLViewerInventoryCategory::cat_array_t cats;
    LLViewerInventoryItem::item_array_t items;

    LLRegenerateLinkCollector candidate_matches(target_item);
    gInventory.collectDescendentsIf(gInventory.getRootFolderID(),
                                    cats,
                                    items,
                                    LLInventoryModel::EXCLUDE_TRASH,
                                    candidate_matches);
    for (LLViewerInventoryItem::item_array_t::const_iterator item_iter = items.begin();
         item_iter != items.end();
         ++item_iter)
    {
        const LLViewerInventoryItem *item = (*item_iter);
        if (true) return item->getUUID();
    }
    return LLUUID::null;
}

// This currently dosen't work, because the sim does not allow us
// to change an item's assetID.
bool LLViewerInventoryItem::regenerateLink()
{
    const LLUUID target_item_id = find_possible_item_for_regeneration(this);
    if (target_item_id.isNull())
        return false;
    LLViewerInventoryCategory::cat_array_t cats;
    LLViewerInventoryItem::item_array_t items;
    LLAssetIDMatches asset_id_matches(getAssetUUID());
    gInventory.collectDescendentsIf(gInventory.getRootFolderID(),
                                    cats,
                                    items,
                                    LLInventoryModel::EXCLUDE_TRASH,
                                    asset_id_matches);
    for (LLViewerInventoryItem::item_array_t::iterator item_iter = items.begin();
         item_iter != items.end();
         item_iter++)
    {
        LLViewerInventoryItem *item = (*item_iter);
        item->setAssetUUID(target_item_id);
        item->updateServer(false);
        gInventory.addChangedMask(LLInventoryObserver::REBUILD, item->getUUID());
    }
    gInventory.notifyObservers();
    return true;
}
