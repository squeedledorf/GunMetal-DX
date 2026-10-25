/**
 * @file slprimdefaults.cpp
 * @brief Defaults for prims rezzed with the Build tool and for new
 *        scripts (#466), set in Preferences > Building.
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

#include "slprimdefaults.h"

#include "llframetimer.h"
#include "llnotificationsutil.h"
#include "llpermissions.h"
#include "lltimer.h"
#include "lltrans.h"
#include "object_flags.h"
#include "xform.h"

#include "llagent.h"
#include "llinventorydefines.h"
#include "llinventorymodel.h"
#include "llstartup.h"
#include "llviewercontrol.h"
#include "llviewerinventory.h"
#include "llviewerobject.h"
#include "llviewerregion.h"
#include "llviewertexture.h"

namespace
{
    // How long a queued rez waits for the sim to report the new prim.
    constexpr F64 PENDING_REZ_TIMEOUT = 15.0;
    // Requested vs. reported size: the sim echoes the F32s we sent.
    constexpr F32 SCALE_MATCH_TOLERANCE = 0.001f;

    bool any_post_create_default()
    {
        return gSavedSettings.getBOOL("SlipstreamNewPrimPhantom")
            || gSavedSettings.getBOOL("SlipstreamNewPrimPhysical")
            || gSavedSettings.getBOOL("SlipstreamNewPrimTemporary")
            || LLUUID(gSavedSettings.getString("SlipstreamNewPrimTexture")).notNull()
            || LLUUID(gSavedPerAccountSettings.getString("SlipstreamNewPrimEmbedItem")).notNull();
    }

    // The item the setting names, following a link, if it is still in the
    // agent's own inventory (not the Library, not the Trash).
    LLViewerInventoryItem* find_own_item(const LLUUID& item_id)
    {
        LLViewerInventoryItem* item = gInventory.getItem(item_id);
        if (item && item->getIsLinkType())
        {
            item = item->getLinkedItem();
        }
        if (!item
            || !gInventory.isObjectDescendentOf(item->getUUID(), gInventory.getRootFolderID())
            || gInventory.isObjectDescendentOf(item->getUUID(), gInventory.findCategoryUUIDForType(LLFolderType::FT_TRASH)))
        {
            return nullptr;
        }
        return item;
    }
}

// static
LLVector3 SLPrimDefaults::getNewPrimScale(const LLVector3& stock_scale)
{
    if (!gSavedSettings.getBOOL("SlipstreamNewPrimSizeEnabled"))
    {
        return stock_scale;
    }
    return LLVector3(llclamp(gSavedSettings.getF32("SlipstreamNewPrimSizeX"), MIN_PRIM_SCALE, DEFAULT_MAX_PRIM_SCALE),
                     llclamp(gSavedSettings.getF32("SlipstreamNewPrimSizeY"), MIN_PRIM_SCALE, DEFAULT_MAX_PRIM_SCALE),
                     llclamp(gSavedSettings.getF32("SlipstreamNewPrimSizeZ"), MIN_PRIM_SCALE, DEFAULT_MAX_PRIM_SCALE));
}

// static
bool SLPrimDefaults::getNewPrimPhysical()
{
    return gSavedSettings.getBOOL("SlipstreamNewPrimPhysical");
}

void SLPrimDefaults::noteRezRequest(const LLViewerRegion* regionp, const LLVector3& scale)
{
    if (!regionp || !any_post_create_default())
    {
        return;
    }
    mPending.push_back({ regionp->getHandle(), scale, LLFrameTimer::getTotalSeconds() + PENDING_REZ_TIMEOUT });
}

void SLPrimDefaults::onCreateSelectedObject(LLViewerObject* objectp)
{
    if (mPending.empty() || !objectp || objectp->isDead())
    {
        return;
    }

    const F64 now = LLFrameTimer::getTotalSeconds();
    while (!mPending.empty() && mPending.front().mExpires < now)
    {
        mPending.pop_front();
    }

    LLViewerRegion* regionp = objectp->getRegion();
    if (!regionp
        || objectp->getPCode() != LL_PCODE_VOLUME
        || !objectp->isRoot()
        || !objectp->permYouOwner())
    {
        return;
    }

    for (auto it = mPending.begin(); it != mPending.end(); ++it)
    {
        if (it->mRegionHandle == regionp->getHandle()
            && dist_vec(it->mScale, objectp->getScale()) < SCALE_MATCH_TOLERANCE)
        {
            mPending.erase(it);
            applyDefaults(objectp);
            return;
        }
    }
}

void SLPrimDefaults::applyDefaults(LLViewerObject* objectp)
{
    LL_DEBUGS("PrimDefaults") << "Applying new prim defaults to " << objectp->getID() << LL_ENDL;

    // Flags: set locally, then one ObjectFlagUpdate carries all of them.
    const bool phantom = gSavedSettings.getBOOL("SlipstreamNewPrimPhantom");
    const bool physical = gSavedSettings.getBOOL("SlipstreamNewPrimPhysical");
    const bool temporary = gSavedSettings.getBOOL("SlipstreamNewPrimTemporary");
    if (phantom || physical || temporary)
    {
        if (phantom)
        {
            objectp->setFlagsWithoutUpdate(FLAGS_PHANTOM, true);
        }
        if (physical)
        {
            // Also asked for in ObjectAdd; resent so the sim and viewer agree.
            objectp->setFlagsWithoutUpdate(FLAGS_USE_PHYSICS, true);
        }
        if (temporary)
        {
            objectp->setFlagsWithoutUpdate(FLAGS_TEMPORARY_ON_REZ, true);
        }
        objectp->updateFlags();
    }

    // Texture on every face. The picker in Preferences only offers copy +
    // transfer textures, so applying the asset id directly is allowed.
    const LLUUID texture_id(gSavedSettings.getString("SlipstreamNewPrimTexture"));
    if (texture_id.notNull() && objectp->getNumTEs() > 0)
    {
        LLViewerFetchedTexture* image = LLViewerTextureManager::getFetchedTexture(texture_id);
        for (U8 te = 0; te < objectp->getNumTEs(); ++te)
        {
            objectp->setTEImage(te, image);
        }
        objectp->sendTEUpdate();
    }

    embedItem(objectp);
}

void SLPrimDefaults::embedItem(LLViewerObject* objectp)
{
    const LLUUID item_id(gSavedPerAccountSettings.getString("SlipstreamNewPrimEmbedItem"));
    if (item_id.isNull())
    {
        return;
    }

    LLViewerInventoryItem* item = find_own_item(item_id);
    if (!item || !item->getPermissions().allowCopyBy(gAgent.getID()))
    {
        // Never move a no-copy item: it would leave inventory for the first
        // prim and be gone for the next one. Tell once per item, not per prim.
        if (mWarnedEmbedItem != item_id)
        {
            mWarnedEmbedItem = item_id;
            LLSD args;
            args["REASON"] = LLTrans::getString(item ? "NewPrimItemNoCopy" : "NewPrimItemMissing");
            LLNotificationsUtil::add("NewPrimItemSkipped", args);
        }
        LL_WARNS("PrimDefaults") << "Not embedding item " << item_id << " in new prim: "
                                 << (item ? "not copyable" : "not in inventory") << LL_ENDL;
        return;
    }

    LLPointer<LLViewerInventoryItem> new_item = new LLViewerInventoryItem(item);
    if (LLAssetType::AT_LSL_TEXT == item->getType())
    {
        // RezScript: the copy starts running, as a dropped script does.
        objectp->saveScript(new_item, true, true);
    }
    else
    {
        new_item->setCreationDate(time_corrected());
        objectp->updateInventory(new_item, TASK_INVENTORY_ITEM_KEY, true);
    }
}

// static
LLViewerInventoryItem* SLPrimDefaults::getDefaultScriptItem()
{
    if (LLStartUp::getStartupState() < STATE_STARTED)
    {
        return nullptr;
    }

    const LLUUID item_id(gSavedPerAccountSettings.getString("SlipstreamDefaultScriptItem"));
    if (item_id.isNull())
    {
        return nullptr;
    }

    LLViewerInventoryItem* item = find_own_item(item_id);
    std::string reason;
    if (!item)
    {
        reason = "DefaultScriptMissing";
    }
    else if (item->getType() != LLAssetType::AT_LSL_TEXT)
    {
        reason = "DefaultScriptNotScript";
    }
    else if (!item->getPermissions().allowCopyBy(gAgent.getID())
             || !item->getPermissions().allowModifyBy(gAgent.getID()))
    {
        reason = "DefaultScriptNoCopyMod";
    }

    if (!reason.empty())
    {
        LLSD args;
        args["REASON"] = LLTrans::getString(reason);
        LLNotificationsUtil::add("DefaultScriptUnusable", args);
        LL_WARNS("PrimDefaults") << "Custom default script " << item_id << " unusable (" << reason
                                 << "), creating the stock script" << LL_ENDL;
        return nullptr;
    }
    return item;
}
