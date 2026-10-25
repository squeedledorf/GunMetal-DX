/**
 * @file slbridge.cpp
 * @brief The "#Slipstream Helper" bridge (#500, #428).
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

#include "slbridge.h"

#include "llagent.h"
#include "llappviewer.h"
#include "llattachmentsmgr.h"
#include "llchat.h"
#include "lldir.h"
#include "lleventtimer.h"
#include "llinventoryfunctions.h"
#include "llinventorymodel.h"
#include "llinventorymodelbackgroundfetch.h"
#include "llnotificationsutil.h"
#include "llcleaninstancestore.h"
#include "llpermissions.h"
#include "llviewerassetupload.h"
#include "llviewercontrol.h"
#include "llviewerinventory.h"
#include "llviewerjointattachment.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llvoavatarself.h"
#include "message.h"
#include "message_prehash.h"
#include "rlvactions.h"
#include "rlvcommon.h"
#include "rlvlocks.h"

#include <sstream>
#include <vector>

namespace
{
// <GMDX> own names: a Slipstream install on the same account keeps its own helper
constexpr char FOLDER_NAME[]        = "#GunMetal-DX";
constexpr char HELPER_NAME[]        = "#GunMetal-DX Helper";
constexpr char SCRIPT_NAME[]        = "#GunMetal-DX Helper Script";
// </GMDX>
constexpr char SCRIPT_FILE[]        = "slipstream_helper.lsl";
constexpr char SCRIPT_SUBDIR[]      = "slbridge";
constexpr char LINE_PREFIX[]        = "#slb:";
// The helper object is a copy of this Library prim (always present, full permissions).
constexpr char LIBRARY_PRIM_NAME[]  = "Rock - medium, round";
constexpr U8   HELPER_ATTACH_POINT  = 31;      // HUD "Center 2": invisible to others
constexpr F32  TICK_PERIOD          = 0.25f;
constexpr F32  COMMAND_INTERVAL     = 0.2f;    // at most five commands a second
constexpr size_t MAX_QUEUE          = 64;
constexpr size_t MAX_CONTROL_QUEUE  = 128;     // control lines are tiny; only a flood hits this
constexpr F32  FIND_TIMEOUT         = 60.f;
constexpr F32  ATTACH_TIMEOUT       = 30.f;
constexpr F32  HANDSHAKE_PROBE      = 3.f;     // probe a known channel with "hello" after this
constexpr F32  HELLO_ASK_INTERVAL   = 2.f;     // discovery-channel "hello" while handshaking
constexpr F32  HANDSHAKE_TIMEOUT    = 10.f;    // then reinstall
constexpr F32  INSTALL_TIMEOUT      = 60.f;
constexpr F32  UNAVAILABLE_HELLO_INTERVAL = 15.f; // <GMDX> re-ask a paused helper while unavailable
constexpr F32  VERIFY_TIMEOUT       = 20.f;    // the object's inventory should answer by then
constexpr S32  MAX_VERIFY_REINSTALLS = 1;      // a failed check reinstalls once, then refuses
constexpr F32  CONFIRM_WINDOW       = 10.f;    // avatar-spoken consent statements: at most
constexpr S32  CONFIRM_MAX_LINES    = 12;      // this many per window
// <GMDX> Upstream's 40 / 80 plus the helper's own combat budgets (8 deaths and 24 hit lines
// a second), so a busy fight with pose traffic never loses kill-feed or hit-marker lines here.
constexpr F32  ROUTE_RATE           = 72.f;    // routed helper lines a second ...
constexpr F32  ROUTE_BURST          = 112.f;   // ... and in one burst
// </GMDX>
constexpr F32  PING_INTERVAL        = 30.f;    // the script forwards only while pinged (60 s)
constexpr F32  STALE_CHECK_PERIOD   = 10.f;
constexpr S32  MAX_ATTACH_ATTEMPTS  = 3;
constexpr S32  MAX_CREATES          = 2;       // folder creations / prim copies per session
constexpr S32  MAX_USER_DETACHES    = 2;       // then stop putting it back on
constexpr F32  RETRY_BASE           = 30.f;    // UNAVAILABLE: first automatic retry after this,
constexpr F32  RETRY_MAX            = 300.f;   // doubling up to this (SLBridgeAutoRetry)

class SLBridgeTicker : public LLEventTimer
{
public:
    SLBridgeTicker(F32 period, std::function<void()> cb) : LLEventTimer(period), mCallback(std::move(cb)) {}
    bool tick() override
    {
        if (mCallback)
            mCallback();
        return false;
    }

private:
    std::function<void()> mCallback;
};

class SLNamedTypeCollector : public LLInventoryCollectFunctor
{
public:
    SLNamedTypeCollector(const std::string& name, LLAssetType::EType type) : mName(name), mType(type) {}
    bool operator()(LLInventoryCategory* cat, LLInventoryItem* item) override
    {
        return item && item->getType() == mType && item->getName() == mName;
    }

private:
    std::string mName;
    LLAssetType::EType mType;
};

// #461: a FOLDER_NAME folder, or a HELPER_NAME object.
class SLBridgeObjectCollector : public LLInventoryCollectFunctor
{
public:
    bool operator()(LLInventoryCategory* cat, LLInventoryItem* item) override
    {
        if (cat)
            return cat->getName() == FOLDER_NAME;
        return item && item->getType() == LLAssetType::AT_OBJECT && item->getName() == HELPER_NAME;
    }
};

// The helper's discovery channel: 0x60000000 + the first 7 hex digits of the owner's key.
// slipstream_helper.lsl computes the same number from llGetOwner() and hears only its
// owner's chat there, so it can stay silent until we ask for hello.
// <GMDX> 0x60000000, not Slipstream's 0x40000000: the two viewers' helpers never answer
// each other's hello (the private channels are disjoint too, see the script).
S32 helloChannel()
{
    const std::string id = gAgent.getID().asString();
    return 0x60000000 + (S32)std::stoul(id.substr(0, 7), nullptr, 16);
}

std::string readScriptTemplate()
{
    const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, SCRIPT_SUBDIR, SCRIPT_FILE);
    llifstream file(path.c_str());
    if (!file.is_open())
        return std::string();
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}
}

SLBridge::SLBridge()
{
    mRegionConnection = gAgent.addRegionChangedCallback([this]() { onRegionChanged(); });
}

SLBridge::~SLBridge()
{
    mRegionConnection.disconnect();
    removeVOInventoryListener();
    delete mTicker;
    mTicker = nullptr;
}

void SLBridge::ensureTicker()
{
    if (!mTicker)
        mTicker = new SLBridgeTicker(TICK_PERIOD, [this]() { tick(); });
}

void SLBridge::init()
{
    ensureTicker();
}

S32 SLBridge::getScriptVersion() const
{
    if (mScriptVersion == 0)
    {
        // FNV-1a over the script template: any edit to the .lsl changes the version.
        U32 hash = 2166136261u;
        for (unsigned char c : readScriptTemplate())
        {
            hash ^= c;
            hash *= 16777619u;
        }
        mScriptVersion = (S32)(hash & 0x7FFFFFFF);
        if (mScriptVersion == 0)
            mScriptVersion = 1;
    }
    return mScriptVersion;
}

void SLBridge::addUser(const std::string& feature)
{
    if (!mUsers.insert(feature).second)
        return;
    ensureTicker();
    // A feature switched on by the user is a fresh start, even after the user took the
    // helper off earlier.
    mUserRefused = false;
    mUserDetaches = 0;
    mAttachAttempts = 0;
    if (mState == STATE_IDLE || mState == STATE_UNAVAILABLE)
        setState(STATE_FIND);
}

void SLBridge::removeUser(const std::string& feature)
{
    if (mUsers.erase(feature) == 0)
        return;
    ensureTicker();     // the tick takes the helper off once nobody wants it
}

void SLBridge::registerHandler(const std::string& type, handler_t handler)
{
    mHandlers[type] = std::move(handler);
}

void SLBridge::setState(EState state, const std::string& reason)
{
    if (state == STATE_UNAVAILABLE)
        LL_INFOS("SLBridge") << "Bridge unavailable: " << reason << LL_ENDL;
    else if (state != mState)
        LL_INFOS("SLBridge") << "Bridge state " << (S32)mState << " -> " << (S32)state << LL_ENDL;
    mReason = reason;
    mStateTimer.reset();
    if (state == mState)
        return;
    mState = state;
    mProbeSent = false;
    mHelloAsks = 0;
    if (state != STATE_READY)
        mQueue.clear();     // stale pose data; control lines wait in mControlQueue
    if (state == STATE_FIND)
        mBusy = false;
    mStateSignal(state);
    if (state == STATE_READY && mNewRegion)
    {
        mNewRegion = false;
        mReadyInRegionSignal();
    }
}

void SLBridge::onRegionChanged()
{
    mNewRegion = true;
    mRetryInstalls = 0;     // <GMDX>
    // A no-script parcel or region may be behind us; try again (unless the user took it off).
    if (mState == STATE_UNAVAILABLE && isWanted() && !mUserRefused)
    {
        mAttachAttempts = 0;
        setState(STATE_FIND);
    }
    // <GMDX> A handshake, install or check that spans a teleport fails against the old region
    // (its caps, its inventory fetch, the drop) after this callback: start over here instead.
    // A crossing (no teleport in progress) keeps the old region as a neighbour and carries on.
    // Install generations turn the abandoned callbacks into no-ops.
    else if ((mState == STATE_HANDSHAKE || mState == STATE_INSTALL || mState == STATE_VERIFY) && isWanted()
             && gAgent.getTeleportState() != LLAgent::TELEPORT_NONE)
    {
        mOldScripts.clear();
        mBusy = false;
        mInventoryRequested = false;
        mVerifyRequested = false;
        removeVOInventoryListener();
        setState(STATE_FIND);
    }
    // </GMDX>
    else if (mState == STATE_READY)
    {
        // The private channel survives the region change (listens move with the
        // attachment); tell features that anyone listening here is new.
        mReadyInRegionSignal();
        mNewRegion = false;
    }
}

// ---------------------------------------------------------------------------
// Inventory and attachment
// ---------------------------------------------------------------------------

LLUUID SLBridge::findFolder() const
{
    LLInventoryModel::cat_array_t* cats = nullptr;
    LLInventoryModel::item_array_t* items = nullptr;
    gInventory.getDirectDescendentsOf(gInventory.getRootFolderID(), cats, items);
    if (cats)
    {
        for (const LLPointer<LLViewerInventoryCategory>& cat : *cats)
        {
            if (cat && cat->getName() == FOLDER_NAME)
                return cat->getUUID();
        }
    }
    return LLUUID::null;
}

bool SLBridge::isHelperInTrash() const
{
    // #461: the user threw the folder or the helper away. Making a new one would be a
    // fresh copy on every login; leave it until the Trash is emptied (or it is restored).
    const LLUUID trash_id = gInventory.findCategoryUUIDForType(LLFolderType::FT_TRASH);
    if (trash_id.isNull())
        return false;
    LLInventoryModel::cat_array_t cats;
    LLInventoryModel::item_array_t items;
    SLBridgeObjectCollector collector;
    gInventory.collectDescendentsIf(trash_id, cats, items, LLInventoryModel::INCLUDE_TRASH, collector);
    return !cats.empty() || !items.empty();
}

void SLBridge::refuseTrashed()
{
    LL_INFOS("InvCreate") << "Not making a " << HELPER_NAME << ": one is in the Trash (restore it, or empty the Trash, "
                             "for the viewer to make a new one)" << LL_ENDL;
    mUserRefused = true;    // no retry on region change this session
    setState(STATE_UNAVAILABLE, std::string(HELPER_NAME) + " is in the Trash");
}

LLUUID SLBridge::findHelperItem(const LLUUID& folder_id) const
{
    if (folder_id.isNull())
        return LLUUID::null;
    // The helper this viewer made (or verified) for the account, while it is still there.
    const LLUUID trusted = getTrustedItem();
    if (trusted.notNull() && !mRejectedItems.count(trusted) && isUsableHelperItem(trusted) &&
        gInventory.isObjectDescendentOf(trusted, folder_id))
    {
        return trusted;
    }

    // Otherwise (first run of this version, another computer) any usable one in our folder,
    // preferring a worn one. It is not trusted: it gets a fresh install and is checked
    // before it is used (mForceInstall).
    LLInventoryModel::cat_array_t cats;
    LLInventoryModel::item_array_t items;
    SLNamedTypeCollector collector(HELPER_NAME, LLAssetType::AT_OBJECT);
    gInventory.collectDescendentsIf(folder_id, cats, items, LLInventoryModel::EXCLUDE_TRASH, collector);
    LLUUID pick;
    for (const LLPointer<LLViewerInventoryItem>& item : items)
    {
        if (!item || mRejectedItems.count(item->getUUID()) || !isUsableHelperItem(item->getUUID()))
            continue;
        if (isAgentAvatarValid() && gAgentAvatarp->getWornAttachment(item->getUUID()))
            return item->getUUID();
        if (pick.isNull())
            pick = item->getUUID();
    }
    return pick;
}

bool SLBridge::isHelperItem(const LLUUID& item_id) const
{
    const LLViewerInventoryItem* item = gInventory.getItem(item_id);
    return item && item->getType() == LLAssetType::AT_OBJECT && item->getName() == HELPER_NAME;
}

bool SLBridge::isUsableHelperItem(const LLUUID& item_id) const
{
    // Our install strips the object's scripts and puts ours in; that needs modify rights.
    // A no-modify object could carry a script we can neither see nor remove.
    if (!isHelperItem(item_id))
        return false;
    const LLViewerInventoryItem* item = gInventory.getItem(item_id);
    const LLPermissions& perm = item->getPermissions();
    return perm.getOwner() == gAgentID && perm.allowModifyBy(gAgentID);
}

LLUUID SLBridge::getTrustedItem() const
{
    // Asked per drawn attachment (isHelperObject): parse the setting only now and then.
    if (!mTrustedLoaded || mTrustedReload.getElapsedTimeF32() > 5.f)
    {
        mTrustedReload.reset();
        mTrustedItem.setNull();
        if (gSavedPerAccountSettings.controlExists("SLBridgeHelperItem"))
        {
            mTrustedLoaded = true;
            LLUUID::parseUUID(gSavedPerAccountSettings.getString("SLBridgeHelperItem"), &mTrustedItem);
        }
    }
    return mTrustedItem;
}

void SLBridge::setTrustedItem(const LLUUID& item_id)
{
    if (gSavedPerAccountSettings.controlExists("SLBridgeHelperItem") && item_id != getTrustedItem())
    {
        LL_INFOS("SLBridge") << "Helper item " << item_id << " is now the account's helper" << LL_ENDL;
        gSavedPerAccountSettings.setString("SLBridgeHelperItem", item_id.notNull() ? item_id.asString() : std::string());
        mTrustedItem = item_id;
        mTrustedLoaded = true;
        mTrustedReload.reset();
    }
}

void SLBridge::rejectHelper(const LLUUID& item_id, const std::string& why)
{
    if (item_id.isNull() || !mRejectedItems.insert(item_id).second)
        return;
    LL_WARNS("SLBridge") << "Not using helper item " << item_id << ": " << why << LL_ENDL;
    if (item_id == getTrustedItem())
        setTrustedItem(LLUUID::null);
    if (isAgentAvatarValid() && gAgentAvatarp->getWornAttachment(item_id))
    {
        mSelfDetach = true;
        LLVOAvatarSelf::detachAttachmentIntoInventory(item_id);
        mSelfDetach = false;
    }
    if (item_id == mItemID)
    {
        mItemID.setNull();
        mObjectID.setNull();
    }
    LLSD args;
    args["REASON"] = why;
    LLNotificationsUtil::add("SLBridgeLookalike", args);
}

void SLBridge::createFolder()
{
    ++mFolderCreates;
    mBusy = true;
    LLInvCreateLog::Site inv_site("GunMetal-DX Helper folder"); // #461 <GMDX>
    gInventory.createNewCategory(gInventory.getRootFolderID(), LLFolderType::FT_NONE, FOLDER_NAME,
        [](const LLUUID& id)
        {
            if (!SLBridge::instanceExists())
                return;
            SLBridge& bridge = SLBridge::instance();
            bridge.mBusy = false;
            if (id.isNull())
            {
                bridge.setState(STATE_UNAVAILABLE, "could not create the " + std::string(FOLDER_NAME) + " folder");
                return;
            }
            bridge.mFolderID = id;
        });
}

void SLBridge::createHelperItem(const LLUUID& folder_id)
{
    const LLUUID library_root = gInventory.getLibraryRootFolderID();
    if (library_root.isNull())
    {
        setState(STATE_UNAVAILABLE, "no inventory library on this grid");
        return;
    }

    LLInventoryModel::cat_array_t cats;
    LLInventoryModel::item_array_t items;
    SLNamedTypeCollector collector(LIBRARY_PRIM_NAME, LLAssetType::AT_OBJECT);
    gInventory.collectDescendentsIf(library_root, cats, items, LLInventoryModel::EXCLUDE_TRASH, collector);
    if (items.empty())
    {
        // The library is fetched lazily; ask for it once and look again on later ticks
        // (tickFind times the wait out).
        if (!mLibraryFetchRequested)
        {
            mLibraryFetchRequested = true;
            LLInventoryModelBackgroundFetch::instance().start(library_root);
        }
        return;
    }

    ++mHelperCopies;
    mBusy = true;
    LLPointer<LLInventoryCallback> cb = new LLBoostFuncInventoryCallback([](const LLUUID& id)
        {
            if (!SLBridge::instanceExists())
                return;
            SLBridge& bridge = SLBridge::instance();
            bridge.mBusy = false;
            if (id.isNull())
                bridge.setState(STATE_UNAVAILABLE, "could not copy the helper prim");
            else
            {
                bridge.mItemID = id;    // the new copy, whatever the folder listing says yet
                bridge.setTrustedItem(id);  // made by us from the Library: the account's helper
            }
        });
    LLInvCreateLog::Site inv_site("GunMetal-DX Helper prim copy"); // #461 <GMDX>
    copy_inventory_item(gAgentID, gInventory.getLibraryOwnerID(), items.front()->getUUID(), folder_id, HELPER_NAME, cb);
}

LLViewerObject* SLBridge::getWornHelper() const
{
    if (!isAgentAvatarValid() || mItemID.isNull())
        return nullptr;
    LLViewerObject* object = gAgentAvatarp->getWornAttachment(mItemID);
    return (object && !object->isDead()) ? object : nullptr;
}

void SLBridge::attachHelper(const LLUUID& item_id)
{
    LLViewerInventoryItem* item = gInventory.getItem(item_id);
    if (!item)
        return;
    if (RlvActions::isRlvEnabled() &&
        (!rlvPredCanWearItem(item, RLV_WEAR_ADD) || gRlvAttachmentLocks.isLockedAttachmentPoint((S32)HELPER_ATTACH_POINT, RLV_LOCK_ADD)))
    {
        setState(STATE_UNAVAILABLE, "RLV does not allow attaching the helper");
        return;
    }
    LLAttachmentsMgr::instance().addAttachmentRequest(item_id, HELPER_ATTACH_POINT, true);
}

// <GMDX> The header promises it: no helper is left worn for the next session (which may be
// another viewer that doesn't know this helper's name).
void SLBridge::detachOnLogout()
{
    if (mState != STATE_IDLE)
        detachHelper();
}

void SLBridge::detachHelper()
{
    if (getWornHelper())
    {
        mSelfDetach = true;
        LLVOAvatarSelf::detachAttachmentIntoInventory(mItemID);
        mSelfDetach = false;
    }
    mObjectID.setNull();
}

bool SLBridge::isProtectedItem(const LLUUID& item_id) const
{
    // Only the helper we use (never strays, which detachStrayHelpers takes off), and only
    // while a feature holds the bridge.
    return !mSelfDetach && isWanted() && item_id.notNull() && item_id == mItemID;
}

bool SLBridge::isProtectedObject(const LLViewerObject* object) const
{
    if (!object || mSelfDetach || !isWanted())
        return false;
    return (mObjectID.notNull() && object->getID() == mObjectID) || isProtectedItem(object->getAttachmentItemID());
}

bool SLBridge::isHelperObject(const LLViewerObject* object) const
{
    // A child prim answers for its linkset: the attachment item belongs to the root.
    if (object && !object->isRootEdit())
        object = object->getRootEdit();
    if (!object || !object->isAttachment())
        return false;
    if (mObjectID.notNull() && object->getID() == mObjectID)
        return true;
    const LLUUID item_id = object->getAttachmentItemID();
    if (item_id.isNull())
        return false;
    if (item_id == mItemID || item_id == getTrustedItem())
        return true;
    // Not identified yet (inventory still loading at login): only something worn on a HUD
    // point under our name is hidden; any other attachment with that name stays visible
    // and selectable.
    return object->isHUDAttachment() && isHelperItem(item_id);
}

void SLBridge::notifyProtected() const
{
    if (mProtectNoticeShown && mProtectNoticeTimer.getElapsedTimeF32() < 10.f)
        return;
    mProtectNoticeShown = true;
    mProtectNoticeTimer.reset();
    LLNotificationsUtil::add("SLBridgeInUse");
}

void SLBridge::noteExplicitRemoval(const LLUUID& item_id)
{
    if (!isProtectedItem(item_id))
        return;
    mExplicitRemoval = true;
    mExplicitRemovalTimer.reset();
}

bool SLBridge::takeExplicitRemoval()
{
    // The outfit update follows the COF link removal asynchronously; a request older than
    // this was not what the current update is about.
    const bool explicit_request = mExplicitRemoval && mExplicitRemovalTimer.getElapsedTimeF32() < 30.f;
    mExplicitRemoval = false;
    return explicit_request;
}

void SLBridge::detachStrayHelpers()
{
    // Any worn "#Slipstream Helper" that is not the one we use (an older copy, one from
    // another session) comes off, so there is never more than one.
    if (!isAgentAvatarValid())
        return;
    std::vector<LLUUID> strays;
    for (const auto& [index, attachment] : gAgentAvatarp->mAttachmentPoints)
    {
        if (!attachment)
            continue;
        for (const auto& attached : attachment->mAttachedObjects)
        {
            LLViewerObject* object = attached.get();
            if (!object || object->isDead())
                continue;
            const LLUUID item_id = object->getAttachmentItemID();
            if (item_id.notNull() && item_id != mItemID && isHelperItem(item_id))
                strays.push_back(item_id);
        }
    }
    for (const LLUUID& item_id : strays)
    {
        // An older copy from our own folder comes off quietly; one from anywhere else is a
        // look-alike, and the user is told.
        const LLUUID folder_id = mFolderID.notNull() ? mFolderID : findFolder();  // (IDLE: not looked up yet)
        if (folder_id.notNull() && gInventory.isObjectDescendentOf(item_id, folder_id) && isUsableHelperItem(item_id))
        {
            LL_INFOS("SLBridge") << "Taking off a stray helper " << item_id << LL_ENDL;
            LLVOAvatarSelf::detachAttachmentIntoInventory(item_id);
        }
        else
        {
            rejectHelper(item_id, "it was not made by the viewer");
        }
    }
}

// ---------------------------------------------------------------------------
// Script install
// ---------------------------------------------------------------------------

std::string SLBridge::loadScriptSource() const
{
    std::string source = readScriptTemplate();
    if (source.empty())
        return source;
    LLStringUtil::replaceString(source, "[VERSION]", llformat("%d", getScriptVersion()));
    LLStringUtil::replaceString(source, "[POSE_CHANNEL]", llformat("%d", POSE_CHANNEL));
    LLStringUtil::replaceString(source, "[CONSENT_CHANNEL]", llformat("%d", CONSENT_CHANNEL));
    LLStringUtil::replaceString(source, "[CONFIRM_CHANNEL]", llformat("%d", CONFIRM_CHANNEL));
    return source;
}

void SLBridge::beginInstall(LLViewerObject* object)
{
    if (!object || mUsers.empty())
        return;
    if (object->numChildren() > 0)
    {
        // Install strips scripts from the root prim only; a linkset could keep others.
        rejectHelper(mItemID, "it is not a single prim");
        setState(STATE_FIND);
        return;
    }
    LL_INFOS("SLBridge") << "Installing the helper script (version " << getScriptVersion() << ")" << LL_ENDL;
    setState(STATE_INSTALL);
    ++mInstallGen;  // <GMDX>
    mOldScripts.clear();                                                        // <GMDX>
    mBusy = true;
    mScriptSaved = false;
    mInventoryRequested = true;
    mVerifyRequested = false;
    removeVOInventoryListener();
    registerVOInventoryListener(object, nullptr);
    // <GMDX> Never trust the cached task inventory: a drop doesn't add the new script to it,
    // so a second install on the same object would miss that script and leave it running.
    object->dirtyInventory();
    requestVOInventory();
}

void SLBridge::beginVerify(LLViewerObject* object)
{
    if (!object)
        return;
    // A hello with the right version is not enough: the hash is public, and any script can
    // say it. Look at the object itself before anything is routed.
    setState(STATE_VERIFY);
    mInventoryRequested = true;
    mVerifyRequested = true;
    removeVOInventoryListener();
    registerVOInventoryListener(object, nullptr);
    // From the sim, not our own cached copy (which an install just edited to look right).
    object->dirtyInventory();
    requestVOInventory();
}

void SLBridge::onVerifyInventory(LLViewerObject* object, LLInventoryObject::object_list_t* inventory)
{
    std::string problem;
    if (object->numChildren() > 0)
    {
        problem = "linkset";
    }
    else
    {
        S32 scripts = 0;
        bool ours = false;
        if (inventory)
        {
            for (const LLPointer<LLInventoryObject>& entry : *inventory)
            {
                if (!entry || entry->getType() != LLAssetType::AT_LSL_TEXT)
                    continue;
                ++scripts;
                const LLInventoryItem* item = dynamic_cast<const LLInventoryItem*>(entry.get());
                ours = item && item->getName() == SCRIPT_NAME && item->getPermissions().getCreator() == gAgentID;
            }
        }
        if (scripts != 1 || !ours)
            problem = llformat("%d script(s), ours %s", scripts, ours ? "yes" : "no");
    }

    if (problem.empty())
    {
        LL_INFOS("SLBridge") << "Helper checked: one script, ours" << LL_ENDL;
        mVerifyReinstalls = 0;
        mForceInstall = false;
        setTrustedItem(mItemID);
        if (autoRetry())
        {
            // The attach budget is for one attempt to get the helper on, not the session:
            // without this every re-attach (teleports, crossings) used one up for good.
            mAttachAttempts = 0;
            mRetries = 0;
        }
        setState(STATE_READY);
        // Acknowledge at once: the script stops waiting and starts forwarding.
        mPingTimer.reset();
        sendRawChat("ping|k", mChannel);
        return;
    }

    LL_WARNS("SLBridge") << "Helper check failed (" << problem << ")" << LL_ENDL;
    if (problem != "linkset" && mVerifyReinstalls < MAX_VERIFY_REINSTALLS)
    {
        ++mVerifyReinstalls;
        beginInstall(object);   // strips every script and puts ours back
        return;
    }
    rejectHelper(mItemID, problem == "linkset" ? "it is not a single prim" : "its scripts are not the viewer's");
    setState(STATE_FIND);
}

void SLBridge::inventoryChanged(LLViewerObject* object, LLInventoryObject::object_list_t* inventory, S32 serial_num, void* user_data)
{
    if (!mInventoryRequested || !object || object->getID() != mObjectID)
        return;
    mInventoryRequested = false;
    if (mVerifyRequested)
    {
        mVerifyRequested = false;
        removeVOInventoryListener();
        onVerifyInventory(object, inventory);
        return;
    }

    // <GMDX> Note the old scripts; they come out only when the new one has compiled and is
    // about to go in (onScriptUploaded), so a failed upload never leaves an empty helper.
    mOldScripts.clear();
    if (inventory)
    {
        for (const LLPointer<LLInventoryObject>& entry : *inventory)
        {
            if (entry && entry->getType() == LLAssetType::AT_LSL_TEXT)
                mOldScripts.push_back(entry->getUUID());
        }
    }
    removeVOInventoryListener();

    createScriptItem();
}

void SLBridge::createScriptItem()
{
    // Reuse our inventory copy of the script if it is still there.
    if (mFolderID.notNull())
    {
        LLInventoryModel::cat_array_t cats;
        LLInventoryModel::item_array_t items;
        SLNamedTypeCollector collector(SCRIPT_NAME, LLAssetType::AT_LSL_TEXT);
        gInventory.collectDescendentsIf(mFolderID, cats, items, LLInventoryModel::EXCLUDE_TRASH, collector);
        for (const LLPointer<LLViewerInventoryItem>& item : items)
        {
            // Only a script item we made ourselves (the installed copy must check out as ours).
            const LLPermissions& perm = item ? item->getPermissions() : LLPermissions::DEFAULT;
            if (item && perm.getCreator() == gAgentID && perm.getOwner() == gAgentID && perm.allowModifyBy(gAgentID))
            {
                uploadScript(item->getUUID(), mInstallGen);    // <GMDX> gen
                return;
            }
        }
    }

    LLPermissions perm;
    perm.init(gAgentID, gAgentID, LLUUID::null, LLUUID::null);
    perm.initMasks(PERM_ALL, PERM_ALL, PERM_ALL, PERM_ALL, PERM_ALL);

    const U32 gen = mInstallGen;   // <GMDX>
    LLPointer<LLInventoryCallback> cb = new LLBoostFuncInventoryCallback([gen](const LLUUID& id)
        {
            if (SLBridge::instanceExists())
                SLBridge::instance().uploadScript(id, gen);
        });
    LLInvCreateLog::Site inv_site("GunMetal-DX Helper script"); // #461 <GMDX>
    create_inventory_item(gAgentID, gAgentSessionID, mFolderID, LLTransactionID::tnull, SCRIPT_NAME, LLStringUtil::null,
                          LLAssetType::AT_LSL_TEXT, LLInventoryType::IT_LSL, NO_INV_SUBTYPE, perm.getMaskNextOwner(), cb);
}

void SLBridge::uploadScript(const LLUUID& item_id, U32 install_gen)
{
    if (!isCurrentInstall(install_gen))
        return;     // <GMDX> an overtaken install
    const std::string url = gAgent.getRegionCapability("UpdateScriptAgent");
    const std::string source = loadScriptSource();
    if (item_id.isNull() || url.empty() || source.empty())
    {
        mBusy = false;
        setState(STATE_UNAVAILABLE, source.empty() ? "helper script missing from the viewer install"
                                                   : (item_id.isNull() ? "could not create the script" : "cannot upload scripts here"));
        return;
    }

    LLBufferedAssetUploadInfo::invnUploadFinish_f finish = [install_gen](LLUUID itemId, LLUUID newAssetId, LLUUID newItemId, LLSD response)
        {
            if (SLBridge::instanceExists())
                SLBridge::instance().onScriptUploaded(newItemId.notNull() ? newItemId : itemId, response, install_gen);
        };
    LLBufferedAssetUploadInfo::uploadFailed_f failed = [install_gen](LLUUID itemId, LLUUID taskId, LLSD response, std::string reason)
        {
            if (SLBridge::instanceExists() && SLBridge::instance().isCurrentInstall(install_gen)) // <GMDX>
            {
                SLBridge::instance().mBusy = false;
                SLBridge::instance().setState(STATE_UNAVAILABLE, "script upload failed: " + reason);
            }
            return false;
        };
    LLResourceUploadInfo::ptr_t info(new LLScriptAssetUpload(item_id, "mono", source, finish, failed));
    LLViewerAssetUpload::EnqueueInventoryUpload(url, info);
}

void SLBridge::onScriptUploaded(const LLUUID& item_id, const LLSD& response, U32 install_gen)
{
    // <GMDX> The install was overtaken (a late hello from the old script made us READY, the
    // helper went away, a newer install began): dropping this script now would put a second
    // script, on another channel, into the helper.
    if (!isCurrentInstall(install_gen))
        return;
    // </GMDX>
    mBusy = false;
    if (response.has("compiled") && !response["compiled"].asBoolean())
    {
        // A broken script must not be installed (and must not be retried every region).
        LL_WARNS("SLBridge") << "Helper script did not compile: " << response["errors"] << LL_ENDL;
        mUserRefused = true;    // stop retrying this session; the viewer install is broken
        setState(STATE_UNAVAILABLE, "the helper script did not compile (see the log)");
        return;
    }

    LLViewerObject* object = getWornHelper();
    LLViewerInventoryItem* item = gInventory.getItem(item_id);
    if (!object || !item)
    {
        setState(STATE_FIND);
        return;
    }
    // Drop the compiled script into the worn helper. <GMDX> It never speaks first (d32dae2d8f),
    // so the INSTALL tick asks it for hello on the discovery channel until it answers.
    // #461: a create in the object's inventory; no answer is tracked (logged WAITING).
    LLInvCreateLog::Site inv_site("GunMetal-DX Helper script install"); // <GMDX>
    LLInvCreateLog::noteSent("object script", LLUUID::null, (S32)LLAssetType::AT_LSL_TEXT, item->getName(),
                             "into worn object " + object->getID().asString());
    // <GMDX> Out with the old scripts now (see inventoryChanged), then in with the new one.
    for (const LLUUID& id : mOldScripts)
        object->removeInventory(id);
    mOldScripts.clear();
    object->saveScript(item, true, true);
    mStateTimer.reset();
    mScriptSaved = true;    // the tick now asks the new script for its hello
    mHelloAsks = 0;
}

// ---------------------------------------------------------------------------
// State machine
// ---------------------------------------------------------------------------

void SLBridge::tickFind()
{
    if (mStateTimer.getElapsedTimeF32() > FIND_TIMEOUT)
    {
        setState(STATE_UNAVAILABLE, mBusy ? "inventory did not answer" : "could not find or make the helper");
        return;
    }
    if (!mBusy && !gInventory.isInventoryUsable() && autoRetry())
        mStateTimer.reset();    // waiting for the inventory to load is not a failure to find
    if (mBusy || !gInventory.isInventoryUsable())
        return;

    // Only the "#Slipstream" folder in the inventory root is ours; a folder of that name
    // anywhere else (an accepted offer, say) is never used.
    if (mFolderID.isNull())
        mFolderID = findFolder();
    if (mFolderID.isNull())
    {
        if (isHelperInTrash())
        {
            refuseTrashed();
            return;
        }
        if (mFolderCreates >= MAX_CREATES)
            setState(STATE_UNAVAILABLE, "could not create the " + std::string(FOLDER_NAME) + " folder");
        else
            createFolder();
        return;
    }

    // Never decide the helper is missing before the folder's contents are known; doing so
    // copies a new prim every login.
    if (!gInventory.isCategoryComplete(mFolderID))
    {
        if (!mFolderFetchRequested)
        {
            mFolderFetchRequested = true;
            LLInventoryModelBackgroundFetch::instance().start(mFolderID);
        }
        return;
    }

    // A copy we just made may not be listed yet: wait for it rather than copy again.
    if (mItemID.notNull() && !gInventory.getItem(mItemID))
        return;
    if (mItemID.isNull() || !isUsableHelperItem(mItemID) || mRejectedItems.count(mItemID))
        mItemID = findHelperItem(mFolderID);
    if (mItemID.isNull())
    {
        // Name-alikes in our folder that we may not use (not ours, no modify) are refused
        // once, with a note, before a fresh helper is made next to them.
        LLInventoryModel::cat_array_t cats;
        LLInventoryModel::item_array_t items;
        SLNamedTypeCollector collector(HELPER_NAME, LLAssetType::AT_OBJECT);
        gInventory.collectDescendentsIf(mFolderID, cats, items, LLInventoryModel::EXCLUDE_TRASH, collector);
        for (const LLPointer<LLViewerInventoryItem>& item : items)
        {
            if (item && !mRejectedItems.count(item->getUUID()) && !isUsableHelperItem(item->getUUID()))
                rejectHelper(item->getUUID(), "it cannot be modified");
        }
        if (isHelperInTrash())
        {
            refuseTrashed();
            return;
        }
        if (mHelperCopies >= MAX_CREATES)
            setState(STATE_UNAVAILABLE, "could not copy the helper prim");
        else
            createHelperItem(mFolderID);
        return;
    }

    // Anything not recorded as made by this viewer is installed fresh and checked.
    mForceInstall = (mItemID != getTrustedItem());
    detachStrayHelpers();
    if (LLViewerObject* object = getWornHelper())
    {
        mObjectID = object->getID();
        mVerifyReinstalls = 0;
        setState(STATE_HANDSHAKE);
    }
    else if (++mAttachAttempts > MAX_ATTACH_ATTEMPTS)
    {
        setState(STATE_UNAVAILABLE, "the helper keeps coming off");
    }
    else
    {
        attachHelper(mItemID);
        if (mState == STATE_FIND)
            setState(STATE_ATTACH);
    }
}

void SLBridge::tick()
{
    if (!isAgentAvatarValid() || !gAgent.getRegion() || LLApp::isExiting()
        || LLAppViewer::instance()->logoutRequestSent())   // <GMDX> the helper came off for logout: never put it back on
    {
        // FIND's timeout counts only time it could work (a feature can ask for the bridge
        // at startup, long before the avatar exists on a slow first login).
        if (mState == STATE_FIND && autoRetry())
            mStateTimer.reset();
        return;
    }

    // Once the helper is known, drop anything of it the renderer registered before we
    // could recognise it (at login the attachment arrives before inventory has loaded).
    if (mObjectID.notNull() && mObjectID != mHiddenObjectID)
    {
        LLCleanInstanceStore::instance().releaseObject(mObjectID);
        mHiddenObjectID = mObjectID;
    }

    if (mUsers.empty())
    {
        if (mState != STATE_IDLE)
        {
            if (mItemID.isNull())
                mItemID = findHelperItem(findFolder());
            detachHelper();
            setState(STATE_IDLE);
        }
        else if (mStaleCheckTimer.getElapsedTimeF32() > STALE_CHECK_PERIOD)
        {
            // Still worn from an earlier session (or put on by hand) while nothing needs
            // it: take it off. Inventory may still be loading early on, so keep looking.
            mStaleCheckTimer.reset();
            if (mItemID.isNull())
                mItemID = findHelperItem(findFolder());
            if (getWornHelper())
                detachHelper();
            detachStrayHelpers();
        }
        return;
    }

    switch (mState)
    {
    case STATE_IDLE:
        setState(STATE_FIND);
        break;

    case STATE_FIND:
        tickFind();
        break;

    case STATE_ATTACH:
        if (LLViewerObject* object = getWornHelper())
        {
            mObjectID = object->getID();
            mVerifyReinstalls = 0;
            setState(STATE_HANDSHAKE);
        }
        else if (mStateTimer.getElapsedTimeF32() > ATTACH_TIMEOUT)
        {
            setState(STATE_UNAVAILABLE, "could not attach the helper");
        }
        break;

    case STATE_HANDSHAKE:
    {
        LLViewerObject* object = getWornHelper();
        if (!object)
        {
            setState(STATE_FIND);
            break;
        }
        if (mForceInstall)
        {
            // Not known to be ours: whatever script it runs is replaced before any use.
            beginInstall(object);
            break;
        }
        if (mStateTimer.getElapsedTimeF32() >= mHelloAsks * HELLO_ASK_INTERVAL
            && mStateTimer.getElapsedTimeF32() <= HANDSHAKE_TIMEOUT)
        {
            ++mHelloAsks;
            sendRawChat("hello", helloChannel());
        }
        if (mChannel > 0 && !mProbeSent && mStateTimer.getElapsedTimeF32() > HANDSHAKE_PROBE)
        {
            // We know its channel from before: ask it to say hello rather than reinstall.
            mProbeSent = true;
            sendRawChat("hello", mChannel);
        }
        else if (mStateTimer.getElapsedTimeF32() > HANDSHAKE_TIMEOUT)
        {
            // No (current) script answered: install one.
            mObjectID = object->getID();
            beginInstall(object);
        }
        break;
    }

    case STATE_INSTALL:
        if (!getWornHelper())
        {
            mBusy = false;
            mInventoryRequested = false;
            removeVOInventoryListener();
            setState(STATE_FIND);
        }
        else if (mStateTimer.getElapsedTimeF32() > INSTALL_TIMEOUT)
        {
            mBusy = false;
            mInventoryRequested = false;
            removeVOInventoryListener();
            setState(STATE_UNAVAILABLE, "the helper script does not run here (no-script area?)");
        }
        else if (mScriptSaved && mStateTimer.getElapsedTimeF32() >= (mHelloAsks + 1) * HELLO_ASK_INTERVAL)
        {
            // The new script never speaks first: ask it for its hello.
            ++mHelloAsks;
            sendRawChat("hello", helloChannel());
        }
        break;

    case STATE_VERIFY:
        if (!getWornHelper())
        {
            mInventoryRequested = false;
            mVerifyRequested = false;
            removeVOInventoryListener();
            setState(STATE_FIND);
        }
        else if (mStateTimer.getElapsedTimeF32() > VERIFY_TIMEOUT)
        {
            mInventoryRequested = false;
            mVerifyRequested = false;
            removeVOInventoryListener();
            setState(STATE_UNAVAILABLE, "could not check the helper's contents");
        }
        break;

    case STATE_READY:
        if (!getWornHelper())
        {
            // We did not take it off: the user (or a script, or RLV) did.
            mObjectID.setNull();
            if (++mUserDetaches >= MAX_USER_DETACHES)
            {
                mUserRefused = true;
                setState(STATE_UNAVAILABLE, "you took the helper off");
                LLNotificationsUtil::add("SLBridgeTakenOff");
            }
            else
            {
                setState(STATE_FIND);
            }
            break;
        }
        // <GMDX> See mConfirmHello: the live script answers, and a new channel is adopted.
        if (mConfirmHello && mStateTimer.getElapsedTimeF32() > HELLO_ASK_INTERVAL)
        {
            mConfirmHello = false;
            sendRawChat("hello", helloChannel());
        }
        if (mPingTimer.getElapsedTimeF32() > PING_INTERVAL)
        {
            // Keeps the script forwarding (it goes quiet when nobody pings it).
            mPingTimer.reset();
            sendRawChat("ping|k", mChannel);
            // <GMDX> Liveness: a script that reset while worn has a new channel and has
            // forgotten what was asked of it. Its hello answer is caught below.
            sendRawChat("hello", helloChannel());
        }
        if (mSendTimer.getElapsedTimeF32() >= COMMAND_INTERVAL)
        {
            std::deque<std::string>& queue = !mControlQueue.empty() ? mControlQueue : mQueue;
            if (!queue.empty())
            {
                mSendTimer.reset();
                sendRawChat(queue.front(), mChannel);
                queue.pop_front();
            }
        }
        break;

    case STATE_UNAVAILABLE:
        // <GMDX> The worn script may only be paused (a no-script parcel at login or arrival):
        // keep asking now and then, so walking into script land brings the features back
        // within 15 s and without a reinstall (the backoff below reinstalls, and waits up to
        // 5 min). A hello answer is accepted here (processOwnerSay) and checked (VERIFY).
        if (!mUserRefused && isWanted() && getWornHelper()
            && mStateTimer.getElapsedTimeF32() >= (mHelloAsks + 1) * UNAVAILABLE_HELLO_INTERVAL)
        {
            ++mHelloAsks;
            sendRawChat("hello", helloChannel());
        }
        // </GMDX>
        // #902: UNAVAILABLE used to be left only by a region change or a feature being
        // switched on again, so a timeout at login (slow inventory, a slow attach) left
        // every feature dead for the session while its check box still showed on. Retry
        // after 30 s, doubling to 5 minutes; never after the user took the helper off, put
        // it in the Trash, or the script failed to compile (mUserRefused).
        if (autoRetry() && !mUserRefused)
        {
            const F32 delay = llmin(RETRY_BASE * (F32)(1 << llclamp(mRetries, 0, 4)), RETRY_MAX);
            if (mStateTimer.getElapsedTimeF32() >= delay)
            {
                ++mRetries;
                LL_INFOS("SLBridge") << "Retrying the helper (attempt " << mRetries << ", after " << (S32)delay
                                     << " s unavailable: " << mReason << ")" << LL_ENDL;
                mAttachAttempts = 0;
                setState(STATE_FIND);
            }
        }
        break;
    }
}

// static
bool SLBridge::autoRetry()
{
    static LLCachedControl<bool> retry(gSavedSettings, "SLBridgeAutoRetry", true);
    return retry;
}

bool SLBridge::sendCommand(const std::string& command)
{
    if (mState != STATE_READY || mChannel == 0 || command.empty() || command.size() > MAX_COMMAND_BYTES)
        return false;
    if (mQueue.size() >= MAX_QUEUE)
        mQueue.pop_front();     // oldest first: pose data is superseded by newer data anyway
    mQueue.push_back(command);
    ensureTicker();
    return true;
}

void SLBridge::sendControl(const std::string& command)
{
    if (command.empty() || command.size() > MAX_COMMAND_BYTES)
        return;
    if (mControlQueue.size() >= MAX_CONTROL_QUEUE)
    {
        LL_WARNS("SLBridge") << "Control queue full; dropping " << mControlQueue.front() << LL_ENDL;
        mControlQueue.pop_front();
    }
    mControlQueue.push_back(command);
    ensureTicker();
}

void SLBridge::sendRawChat(const std::string& text, S32 channel)
{
    // Our own private channel to our own attachment: built directly, so chat
    // redirection and channel restrictions meant for typed chat do not apply.
    LLMessageSystem* msg = gMessageSystem;
    if (!msg || channel <= 0)
        return;
    msg->newMessageFast(_PREHASH_ChatFromViewer);
    msg->nextBlockFast(_PREHASH_AgentData);
    msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
    msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
    msg->nextBlockFast(_PREHASH_ChatData);
    msg->addStringFast(_PREHASH_Message, text);
    msg->addU8Fast(_PREHASH_Type, CHAT_TYPE_WHISPER);
    msg->addS32("Channel", channel);
    gAgent.sendReliableMessage();
}

void SLBridge::sayConfirm(const std::string& line)
{
    // Grants respect RLV's channel restrictions; a revocation always goes out.
    const bool grant = line.compare(0, 3, "G1|") == 0;
    if (grant && RlvActions::isRlvEnabled() && !RlvActions::canSendChannel(CONFIRM_CHANNEL))
        return;
    // Chat from the avatar: never a flood, whatever asks for it.
    if (mConfirmTimer.getElapsedTimeF32() > CONFIRM_WINDOW)
    {
        mConfirmTimer.reset();
        mConfirmLines = 0;
    }
    if (++mConfirmLines > CONFIRM_MAX_LINES)
    {
        LL_WARNS_ONCE("SLBridge") << "Consent statements over budget; dropping some" << LL_ENDL;
        return;
    }
    LLMessageSystem* msg = gMessageSystem;
    if (!msg || !gAgent.getRegion())
        return;
    msg->newMessageFast(_PREHASH_ChatFromViewer);
    msg->nextBlockFast(_PREHASH_AgentData);
    msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
    msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
    msg->nextBlockFast(_PREHASH_ChatData);
    msg->addStringFast(_PREHASH_Message, line);
    msg->addU8Fast(_PREHASH_Type, CHAT_TYPE_SHOUT);
    msg->addS32("Channel", CONFIRM_CHANNEL);
    gAgent.sendReliableMessage();
}

bool SLBridge::processOwnerSay(const LLUUID& object_id, const std::string& message)
{
    static const size_t prefix_len = sizeof(LINE_PREFIX) - 1;
    if (message.compare(0, prefix_len, LINE_PREFIX) != 0)
        return false;

    LLViewerObject* object = gObjectList.findObject(object_id);
    const LLUUID item_id = object ? object->getAttachmentItemID() : LLUUID::null;

    // A worn helper we do not know yet (inventory still loading): adopt it only if it is
    // the one this viewer made for the account; its name alone counts for nothing.
    if (object && item_id.notNull() && item_id != mItemID && !mUsers.empty() && !getWornHelper() &&
        item_id == getTrustedItem() && !mRejectedItems.count(item_id))
    {
        LL_INFOS("SLBridge") << "Adopting worn helper " << item_id << LL_ENDL;
        mItemID = item_id;
    }

    // Only the worn helper speaks for the bridge. Anything else of ours that uses the
    // prefix (an old copy, a look-alike) is swallowed and ignored.
    if (!object || mItemID.isNull() || item_id != mItemID)
    {
        // Logged once per object (why a hello does not reach the handshake).
        static std::set<LLUUID> s_logged;
        if (s_logged.size() < 64 && s_logged.insert(object_id).second)
            LL_INFOS("SLBridge") << "Ignoring bridge line from " << object_id << (object ? "" : " (object not known yet)")
                                 << " item " << item_id << " expected " << mItemID << LL_ENDL;
        return true;
    }

    const std::string body = message.substr(prefix_len);
    const std::string::size_type bar = body.find('|');
    const std::string type = body.substr(0, bar);
    const std::string rest = (bar == std::string::npos) ? std::string() : body.substr(bar + 1);

    if (type == "hello")
    {
        // Nothing wants the bridge: the tick takes it off; never install for nobody.
        if (mUsers.empty())
            return true;
        // <GMDX> Only while we are looking for it: a liveness answer still in flight after the
        // helper came off (FIND / IDLE / UNAVAILABLE) must not make us READY again.
        if (mState != STATE_HANDSHAKE && mState != STATE_INSTALL && mState != STATE_READY
            && !(mState == STATE_UNAVAILABLE && !mUserRefused))
            return true;

        // hello|<version>|<channel>|<object key>
        std::vector<std::string> fields;
        LLStringUtil::getTokens(rest, fields, "|");
        S32 version = 0, channel = 0;
        if (fields.size() < 2 || !LLStringUtil::convertToS32(fields[0], version) || !LLStringUtil::convertToS32(fields[1], channel))
            return true;
        // <GMDX> READY: the same answer again is liveness only (every ping also asks). A new
        // channel (the worn script reset and forgot what it was asked) or another version goes
        // through the check or an install again; READY after it makes the features re-send
        // their state.
        const bool restarted = mState == STATE_READY && version == getScriptVersion() && channel > 0
                               && channel != mChannel;
        if (mState == STATE_READY && !restarted && version == mReportedVersion)
            return true;
        // </GMDX>
        mObjectID = object_id;
        mReportedVersion = version;
        // Before the drop, an answer comes from a script about to be replaced. <GMDX> Old
        // scripts are only removed at the drop, so a current answer from a helper that is
        // ours (not a forced install, not a reinstall after a failed check) is a working
        // script that answered late (a slow region): it cancels the install, and is checked.
        if (mState == STATE_INSTALL && !mScriptSaved && (mForceInstall || mVerifyReinstalls > 0))
            return true;
        if (version == getScriptVersion() && channel > 0 && !(mForceInstall && mState != STATE_INSTALL))
        {
            // <GMDX> Straight after a drop, this may be a late answer from the script the drop
            // just removed (and a "restart" a late answer from a dead channel): confirm with one
            // more hello soon rather than at the next ping.
            mConfirmHello = (mState == STATE_INSTALL && mScriptSaved) || restarted;
            if (restarted)
                LL_INFOS("SLBridge") << "Helper script answered on a new channel; checking it again" << LL_ENDL;
            // </GMDX>
            mChannel = channel;
            mBusy = false;
            mInventoryRequested = false;
            removeVOInventoryListener();
            // READY (and the first ping) only once the object checks out.
            beginVerify(object);
        }
        // <GMDX> From UNAVAILABLE (an old script answering after a failed install), retry the
        // install at most twice per region.
        else if (mState != STATE_INSTALL && (mState != STATE_UNAVAILABLE || mRetryInstalls++ < 2))
        {
            LL_INFOS("SLBridge") << "Helper script version " << version << " is not " << getScriptVersion() << "; updating" << LL_ENDL;
            beginInstall(object);
        }
        return true;
    }

    if (mState != STATE_READY)
        return true;

    // Defence in depth behind the helper's own budgets: at most ROUTE_RATE routed lines a
    // second (burst ROUTE_BURST) reach the features; the rest are dropped.
    const F32 now = mRouteClock.getElapsedTimeF32();
    mRouteTokens = llmin(ROUTE_BURST, mRouteTokens + (now - mRouteLast) * ROUTE_RATE);
    mRouteLast = now;
    if (mRouteTokens < 1.f)
    {
        if (++mRouteDropped == 1 || (mRouteDropped % 500) == 0)
            LL_WARNS("SLBridge") << "Bridge lines over budget; dropped " << mRouteDropped << " so far" << LL_ENDL;
        return true;
    }
    mRouteTokens -= 1.f;

    auto it = mHandlers.find(type);
    if (it != mHandlers.end() && it->second)
        it->second(rest);
    return true;
}
