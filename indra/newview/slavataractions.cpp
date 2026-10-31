/**
 * @file slavataractions.cpp
 * @brief Friend-related actions (add, remove, offer teleport, etc)
 *
 * $LicenseInfo:firstyear=2020&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2020, Rye Mutt <rye@alchemyviewer.org>
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
 * $/LicenseInfo$
 */


#include "llviewerprecompiledheaders.h"

#include "slavataractions.h"

#include "llavatarnamecache.h"
#include "llclipboard.h"
#include "llnotifications.h"
#include "llnotificationsutil.h"
#include "llmenugl.h"
#include "llparcel.h"
#include "llsdutil.h"
#include "lltrans.h"
#include "lltimer.h"
#include "roles_constants.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llappviewer.h"
#include "llavataractions.h"
#include "llcallbacklist.h"
#include "llfloaterreg.h"
#include "llfloaterregioninfo.h"
#include "llfloaterreporter.h"
#include "llfloaterwebcontent.h"
#include "llslurl.h"
#include "llviewercontrol.h"
#include "llviewermenu.h"
#include "llviewerobjectlist.h"
#include "llviewerparcelmgr.h"
#include "llviewerparceloverlay.h"
#include "llviewerregion.h"
#include "llworld.h"
#include "rlvactions.h"

#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <tuple>

namespace
{
// Menu hints may be cached briefly. Every confirmed eject uses fresh simulator data.
struct ModerationParcel
{
    LLSD identity;
    F64 requested_at = 0.;
    bool pending = true;
};
using ModerationCell = std::tuple<LLUUID, LLUUID, LLUUID, S32, S32>;
std::map<ModerationCell, ModerationParcel> sModerationParcels;
LLNotificationPtr sMenuPermissionWait;
size_t sPendingMenuPermissions = 0;

struct ModerationMenu
{
    LLHandle<LLView> menu;
    std::shared_ptr<const uuid_vec_t> ids;
    LLUUID session;
};
std::vector<ModerationMenu> sModerationMenus;

void refreshModerationMenus()
{
    if (LLApp::isExiting() || gDisconnected)
        return;
    // Capacity/disconnect failures can complete a lookup synchronously during arrange.
    static bool refreshing = false;
    if (refreshing)
        return;
    refreshing = true;
    std::vector<LLHandle<LLView>> visible_contexts;
    for (auto it = sModerationMenus.begin(); it != sModerationMenus.end(); )
    {
        auto* menu = dynamic_cast<LLMenuGL*>(it->menu.get());
        if (!menu || it->session != gAgent.getSessionID())
        {
            it = sModerationMenus.erase(it);
            continue;
        }
        if (!menu->getVisible())
        {
            ++it;
            continue; // An old, closed Nearby menu must not generate lookup traffic.
        }
        if (LLView* manage = menu->findChildView("manage_menu", false))
            manage->setVisible(SLAvatarActions::canFreezeEject(*it->ids) ||
                               SLAvatarActions::canManageAvatarsEstate(*it->ids));
        menu->needsArrange();
        if (dynamic_cast<LLContextMenu*>(menu))
            visible_contexts.push_back(menu->getHandle());
        ++it;
    }
    // Context menus evaluate visibility on arrange, not on every frame. Refresh the
    // existing open avatar/radar menu; never close/reopen it or change its selection.
    if (gMenuHolder)
    {
        for (LLView* child : *gMenuHolder->getChildList())
        {
            auto* menu = dynamic_cast<LLMenuGL*>(child);
            if (!menu || (!menu->findChildView("eject", false) && !menu->findChildView("Eject...", false)))
                continue;
            menu->needsArrange();
            if (auto* context = dynamic_cast<LLContextMenu*>(child); context && context->getVisible())
                visible_contexts.push_back(context->getHandle());
        }
        // Revealing rows changes popup dimensions. Arrange the existing popup and
        // keep it inside the same viewport, without reopening or changing selection.
        // Use weak handles: arranging can create/remove spillover menu children.
        const LLRect bounds = gMenuHolder->getMenuRect();
        for (const auto& handle : visible_contexts)
        {
            auto* context = dynamic_cast<LLContextMenu*>(handle.get());
            if (!context || !context->getVisible())
                continue;
            context->arrangeAndClear();
            LLRect rect = context->getRect();
            S32 delta_x = llmin(0, bounds.mRight - rect.mRight);
            rect.translate(delta_x, 0);
            delta_x += llmax(0, bounds.mLeft - rect.mLeft);
            S32 delta_y = llmax(0, bounds.mBottom - rect.mBottom);
            rect.translate(0, delta_y);
            delta_y += llmin(0, bounds.mTop - rect.mTop);
            context->translate(delta_x, delta_y);
        }
    }
    refreshing = false;
}

bool moderationTarget(const LLUUID& id, LLSD& target, const char** failure = nullptr)
{
    if (id.isNull() || id == gAgent.getID() || gDisconnected || LLApp::isExiting())
    {
        if (failure)
            *failure = id.isNull() ? "null-target" : id == gAgent.getID() ? "self-target" :
                LLApp::isExiting() ? "application-exiting" : "disconnected";
        return false;
    }
    LLWorld::region_gpos_map_t avatars;
    LLWorld::getInstance()->getAvatars(&avatars);
    auto it = avatars.find(id);
    if (it == avatars.end() || !it->second.first)
    {
        if (failure)
            *failure = "target-unavailable";
        return false;
    }
    LLViewerRegion* region = it->second.first;
    const LLVector3 pos = region->getPosRegionFromGlobal(it->second.second);
    if (!std::isfinite(pos.mV[VX]) || !std::isfinite(pos.mV[VY]) ||
        pos.mV[VX] < 0.f || pos.mV[VY] < 0.f ||
        pos.mV[VX] >= region->getWidth() || pos.mV[VY] >= region->getWidth())
    {
        if (failure)
            *failure = "target-position-invalid";
        return false;
    }
    target["id"] = id;
    target["region_id"] = region->getRegionID();
    target["region_owner"] = region->getOwner();
    target["x"] = S32(pos.mV[VX] / PARCEL_GRID_STEP_METERS);
    target["y"] = S32(pos.mV[VY] / PARCEL_GRID_STEP_METERS);
    return true;
}

bool sameModerationTarget(const LLSD& target, bool parcel, const char** failure = nullptr)
{
    LLSD current;
    if (!moderationTarget(target["id"].asUUID(), current, failure))
        return false;
    const char* reason = nullptr;
    if (current["region_id"] != target["region_id"])
        reason = "target-region-changed";
    else if (current["region_owner"] != target["region_owner"])
        reason = "estate-owner-changed";
    else if (parcel && (current["x"] != target["x"] || current["y"] != target["y"]))
        reason = "target-cell-changed";
    if (reason && failure)
        *failure = reason;
    return !reason;
}

bool parcelPower(const LLSD& identity, U64 power)
{
    const LLUUID owner = identity["owner_id"].asUUID();
    return identity.isDefined() && identity["local_id"].asInteger() >= 0 &&
        (gAgent.isGodlike() || (owner.notNull() && (owner == gAgent.getID() ||
         (identity["group_owned"].asBoolean() && gAgent.hasPowerInGroup(owner, power)))));
}

bool canParcelEject(const LLSD& target, const LLSD& identity)
{
    LLViewerRegion* region = LLWorld::getInstance()->getRegionFromID(target["region_id"].asUUID());
    return region && (region->canManageEstate() || parcelPower(identity, GP_LAND_ADMIN));
}

LLVector3d moderationPosition(const LLSD& target, LLViewerRegion* region)
{
    // Query the centre of the target's 4 m parcel cell, never a boundary.
    return region->getPosGlobalFromRegion(LLVector3(
        (target["x"].asInteger() + .5f) * PARCEL_GRID_STEP_METERS,
        (target["y"].asInteger() + .5f) * PARCEL_GRID_STEP_METERS, 0.f));
}

LLSD menuParcelIdentity(const LLSD& target)
{
    static boost::signals2::scoped_connection overlay_connection(LLViewerParcelOverlay::setUpdateCallback(
        [](const LLViewerRegion* changed)
        {
            for (auto it = sModerationParcels.begin(); it != sModerationParcels.end(); )
            {
                if (changed && std::get<0>(it->first) == changed->getRegionID())
                    it = sModerationParcels.erase(it);
                else
                    ++it;
            }
            refreshModerationMenus();
        }));
    const F64 now = LLTimer::getTotalSeconds();
    const LLUUID session = gAgent.getSessionID();
    // Bound the table and free unused hints (including avatars that have left).
    for (auto it = sModerationParcels.begin(); it != sModerationParcels.end(); )
    {
        const F64 lifetime = it->second.identity.isDefined() ? 120. : 10.;
        if (std::get<2>(it->first) != session || now - it->second.requested_at > lifetime)
            it = sModerationParcels.erase(it);
        else
            ++it;
    }
    const ModerationCell cell(target["region_id"].asUUID(), target["region_owner"].asUUID(), session,
                              target["x"].asInteger(), target["y"].asInteger());
    auto it = sModerationParcels.find(cell);
    if (it != sModerationParcels.end())
        return it->second.identity;
    if (sModerationParcels.size() >= 64)
    {
        auto victim = sModerationParcels.end();
        for (auto candidate = sModerationParcels.begin(); candidate != sModerationParcels.end(); ++candidate)
            if (!candidate->second.pending && (victim == sModerationParcels.end() ||
                candidate->second.requested_at < victim->second.requested_at))
                victim = candidate;
        if (victim == sModerationParcels.end())
            return LLSD(); // An outstanding reply will refresh this same open menu.
        sModerationParcels.erase(victim);
    }
    LLViewerRegion* region = LLWorld::getInstance()->getRegionFromID(target["region_id"].asUUID());
    if (!region)
        return LLSD();
    sModerationParcels[cell] = {LLSD(), now};
    if (sPendingMenuPermissions++ == 0)
        sMenuPermissionWait = LLNotificationsUtil::add("AvatarModerationChecking");
    LLViewerParcelMgr::instance().requestParcelIdentity(moderationPosition(target, region),
        [cell, now, session](const LLSD& identity)
        {
            if (--sPendingMenuPermissions == 0)
            {
                if (!LLApp::isExiting())
                    LLNotificationsUtil::cancel(sMenuPermissionWait);
                sMenuPermissionWait.reset();
            }
            auto entry = sModerationParcels.find(cell);
            if (entry != sModerationParcels.end() &&
                entry->second.requested_at == now && session == gAgent.getSessionID())
            {
                entry->second.identity = identity;
                entry->second.pending = false;
                // A failed request is cached too; do not retry on every pie frame.
                refreshModerationMenus();
            }
        });
    return LLSD();
}

void logModerationAbort(const char* reason, size_t count = 0)
{
    LL_INFOS("Moderation") << "Action not sent: reason=" << reason << ", targets=" << count << LL_ENDL;
}

void moderationUnavailable(const char* reason, size_t count = 0)
{
    logModerationAbort(reason, count);
    if (!LLApp::isExiting())
        LLNotificationsUtil::add("AvatarModerationUnavailable");
}

bool moderationSession(const LLSD& payload)
{
    const char* reason = nullptr;
    if (LLApp::isExiting())
        reason = "application-exiting";
    else if (gDisconnected)
        reason = "disconnected";
    else if (!gMessageSystem)
        reason = "message-system-unavailable";
    else if (payload["agent_id"].asUUID() != gAgent.getID())
        reason = "agent-changed";
    else if (payload["session_id"].asUUID() != gAgent.getSessionID())
        reason = "session-changed";
    if (reason)
        logModerationAbort(reason, payload["targets"].size());
    return !reason;
}

bool moderationTargets(const uuid_vec_t& ids, LLSD& payload, bool parcel)
{
    if (ids.empty() || ids.size() > 64 || gDisconnected || !gMessageSystem)
    {
        logModerationAbort(ids.empty() ? "empty-selection" : ids.size() > 64 ? "selection-limit" : "disconnected", ids.size());
        return false;
    }
    payload["targets"] = LLSD::emptyArray();
    payload["agent_id"] = gAgent.getID();
    payload["session_id"] = gAgent.getSessionID();
    std::set<LLUUID> seen;
    for (const LLUUID& id : ids)
    {
        LLSD target;
        const char* reason = "estate-authority-unavailable";
        if (!moderationTarget(id, target, &reason) || (!parcel && !SLAvatarActions::canManageAvatarsEstate(id)))
        {
            logModerationAbort(reason, ids.size());
            return false;
        }
        if (seen.insert(id).second)
            payload["targets"].append(target);
    }
    return true;
}

// Fetch all parcels before proceeding, so a mixed selection cannot act on a hidden subset.
void lookupModerationParcels(LLSD payload, std::function<void(const LLSD&)> completion)
{
    struct Batch
    {
        LLSD payload;
        size_t remaining;
        const char* reason = nullptr;
        LLNotificationPtr waiting;
    };
    auto batch = std::make_shared<Batch>();
    batch->payload = payload;
    batch->remaining = payload["targets"].size();
    if (!batch->remaining)
    {
        logModerationAbort("empty-confirmed-selection");
        return;
    }
    batch->waiting = LLNotificationsUtil::add("AvatarModerationChecking");
    for (size_t i = 0; i < payload["targets"].size(); ++i)
    {
        LLSD target = payload["targets"][i];
        LLSD current;
        const bool available = sameModerationTarget(target, false) &&
            moderationTarget(target["id"].asUUID(), current);
        if (available)
        {
            // Walking within the confirmed parcel is fine. Query the current cell and
            // compare its identity with the confirmed parcel, rather than the old position.
            if (target.has("parcel"))
                current["parcel"] = target["parcel"];
            target = current;
            batch->payload["targets"][i] = target;
        }
        LLViewerRegion* region = available
            ? LLWorld::getInstance()->getRegionFromID(target["region_id"].asUUID()) : nullptr;
        auto received = [batch, i, target, completion](const LLSD& identity)
        {
            const char* mismatch = nullptr;
            if (!identity.isDefined())
                batch->reason = "parcel-identity-unavailable";
            else if (!sameModerationTarget(target, true, &mismatch))
                batch->reason = mismatch;
            else if (!canParcelEject(target, identity))
                batch->reason = "parcel-authority-lost";
            else if (target.has("parcel") && identity != target["parcel"])
                batch->reason = "parcel-identity-changed";
            batch->payload["targets"][i]["parcel"] = identity;
            if (--batch->remaining == 0)
            {
                // Recheck the entire selection after the last reply, including targets that
                // moved or lost authority while the other parcel requests were in flight.
                for (const LLSD& entry : llsd::inArray(batch->payload["targets"]))
                {
                    if (!sameModerationTarget(entry, true, &mismatch))
                        batch->reason = mismatch;
                    else if (!canParcelEject(entry, entry["parcel"]))
                        batch->reason = "parcel-authority-lost-during-batch";
                }
                if (!LLApp::isExiting())
                    LLNotificationsUtil::cancel(batch->waiting);
                if (!moderationSession(batch->payload))
                    batch->reason = "session-invalid";
                if (!batch->reason)
                    completion(batch->payload);
                else
                    moderationUnavailable(batch->reason, batch->payload["targets"].size());
            }
        };
        if (region)
            LLViewerParcelMgr::instance().requestParcelIdentity(moderationPosition(target, region), received);
        else
            received(LLSD());
    }
}

std::string moderationNames(const LLSD& payload)
{
    std::string names;
    for (const LLSD& target : llsd::inArray(payload["targets"]))
    {
        if (!names.empty())
            names += "\n";
        const LLUUID id = target["id"].asUUID();
        names += LLSLURL("agent", id,
            RlvActions::canShowName(RlvActions::SNC_DEFAULT, id) ? "about" : "rlvanonym").getSLURLString();
    }
    return names;
}
}

 // Flags for kick message
const U32 KICK_FLAGS_DEFAULT = 0x0;
const U32 KICK_FLAGS_FREEZE = 1 << 0;
const U32 KICK_FLAGS_UNFREEZE = 1 << 1;

// static
void SLAvatarActions::copyData(const LLUUID& id, ECopyDataType type)
{
    if (id.isNull())
        return;

    uuid_vec_t ids;
    ids.push_back(id);
    copyData(ids, type);
}

// static
void SLAvatarActions::copyData(const uuid_vec_t& ids, ECopyDataType type)
{
    if (!ids.empty())
    {
        std::string data_string;
        static LLCachedControl<std::string> seperator(gSavedSettings, "SlipstreamCopySeperator", ", ");
        for (const LLUUID& id : ids)
        {
            if (id.isNull())
                continue;

            if (!data_string.empty())
                data_string.append(seperator);

            switch (type)
            {
            case E_DATA_USER_NAME:
            {
                LLAvatarName av_name;
                LLAvatarNameCache::get(id, &av_name);
                data_string.append(av_name.getUserName());
                break;
            }
            case E_DATA_ACCOUNT_NAME:
            {
                LLAvatarName av_name;
                LLAvatarNameCache::get(id, &av_name);
                data_string.append(av_name.getAccountName());
                break;
            }
            case E_DATA_DISPLAY_NAME:
            {
                LLAvatarName av_name;
                LLAvatarNameCache::get(id, &av_name);
                data_string.append(av_name.getDisplayName(true));
                break;
            }
            case E_DATA_COMPLETE_NAME:
            {
                LLAvatarName av_name;
                LLAvatarNameCache::get(id, &av_name);
                data_string.append(av_name.getCompleteName(true, true));
                break;
            }
            case E_DATA_SLURL:
                data_string.append(LLSLURL("agent", id, "about").getSLURLString());
                break;
            case E_DATA_UUID:
                data_string.append(id.asString());
                break;
            default:
                break;
            }
        }

        if (!data_string.empty())
        {
            LLWString wdata_str = utf8str_to_wstring(data_string);
            LLClipboard::instance().copyToClipboard(wdata_str, 0, narrow(wdata_str.length()));
        }
    }
}

// static
void SLAvatarActions::copyData(const LLUUID& id, const LLSD& userdata)
{
    const std::string item_name = userdata.asString();
    if (item_name == "user_name")
    {
        copyData(id, E_DATA_USER_NAME);
    }
    else if (item_name == "account_name")
    {
        copyData(id, E_DATA_ACCOUNT_NAME);
    }
    else if (item_name == "display_name")
    {
        copyData(id, E_DATA_DISPLAY_NAME);
    }
    else if (item_name == "full_name")
    {
        copyData(id, E_DATA_COMPLETE_NAME);
    }
    else if (item_name == "id")
    {
        copyData(id, E_DATA_UUID);
    }
    else if (item_name == "slurl")
    {
        copyData(id, E_DATA_SLURL);
    }
}

// static
void SLAvatarActions::copyData(const uuid_vec_t& ids, const LLSD& userdata)
{
    const std::string item_name = userdata.asString();
    if (item_name == "user_name")
    {
        copyData(ids, E_DATA_USER_NAME);
    }
    else if (item_name == "account_name")
    {
        copyData(ids, E_DATA_ACCOUNT_NAME);
    }
    else if (item_name == "display_name")
    {
        copyData(ids, E_DATA_DISPLAY_NAME);
    }
    else if (item_name == "id")
    {
        copyData(ids, E_DATA_UUID);
    }
    else if (item_name == "slurl")
    {
        copyData(ids, E_DATA_SLURL);
    }
}

// static
bool SLAvatarActions::canZoomIn(const LLUUID& idAgent)
{
    return gObjectList.findObject(idAgent) != nullptr;
}

// static
void SLAvatarActions::zoomIn(const LLUUID& idAgent)
{
    handle_zoom_to_object(idAgent);
}

// static
bool SLAvatarActions::canCamToAvatar(const LLUUID& avatar_id)
{
    if (avatar_id.isNull() || avatar_id == gAgentID)
        return false;

    // RLVa camera distance limits would pull the camera straight back
    if (RlvActions::isRlvEnabled() && RlvActions::isCameraDistanceClamped())
        return false;

    LLVector3d pos_global;
    return LLWorld::getInstance()->getAvatar(avatar_id, pos_global);
}

// static
void SLAvatarActions::camToAvatar(const LLUUID& avatar_id)
{
    if (avatar_id.isNull() || avatar_id == gAgentID)
        return;

    // Position at click time: the avatar may have left since the menu opened
    LLVector3d target_global;
    if (!LLWorld::getInstance()->getAvatar(avatar_id, target_global))
    {
        LLNotificationsUtil::add("CamToAvatarGone");
        return;
    }

    LLViewerObject* objectp = gObjectList.findObject(avatar_id);
    if (objectp && objectp->isAvatar())
    {
        // Loaded: the regular zoom frames the avatar and tracks it as it moves
        handle_zoom_to_object(avatar_id);
        return;
    }

    // Not loaded (outside draw distance): focus its map position instead. The sim's
    // interest list follows the camera, so the avatar streams in around it and the
    // camera then hands over to the regular zoom below.
    target_global.mdV[VZ] += 1.0; // map positions are at the feet, aim at the body

    const F64 VIEW_DISTANCE = 4.0;
    LLVector3d target_to_cam = gAgentCamera.getCameraPositionGlobal() - target_global;
    target_to_cam.mdV[VZ] = 0.0;
    if (target_to_cam.normalize() < 0.1)
    {
        target_to_cam = LLVector3d(1.0, 0.0, 0.0);
    }
    target_to_cam.mdV[VZ] = 0.5; // look slightly down at the avatar
    target_to_cam.normalize();

    gAgentCamera.setFocusOnAvatar(false, ANIMATE);
    gAgentCamera.setCameraPosAndFocusGlobal(target_global + target_to_cam * VIEW_DISTANCE, target_global, avatar_id);

    // Unless camera constraints are relaxed, LLAgentCamera keeps the camera within
    // draw distance of our own avatar, so a farther avatar can't be reached.
    static LLCachedControl<bool> disable_camera_constraints(gSavedSettings, "DisableCameraConstraints", false);
    static LLCachedControl<F32> draw_distance(gSavedSettings, "RenderFarClip", 128.f);
    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosGlobal(target_global);
    if (!disable_camera_constraints && !gAgent.isGodlike() && !(regionp && regionp->canManageEstate()) &&
        dist_vec(gAgent.getPositionGlobal(), target_global) > (F64)draw_distance)
    {
        LLNotificationsUtil::add("CamToAvatarOutOfRange");
    }

    // Hand over to the regular zoom once the avatar object arrives, unless the camera
    // was moved meanwhile or another request superseded this one.
    static U32 s_cam_request = 0;
    const U32 request = ++s_cam_request;
    const F64 expiry = LLFrameTimer::getTotalSeconds() + 15.0;
    doPeriodically([avatar_id, target_global, request, expiry]() -> bool
        {
            if (request != s_cam_request || LLFrameTimer::getTotalSeconds() > expiry)
                return true;
            if (gAgentCamera.getFocusOnAvatar() || gAgentCamera.getFocusObject().notNull() ||
                dist_vec_squared(gAgentCamera.getFocusTargetGlobal(), target_global) > 0.01)
                return true;
            LLViewerObject* loaded_avatar = gObjectList.findObject(avatar_id);
            if (loaded_avatar && loaded_avatar->isAvatar())
            {
                handle_zoom_to_object(avatar_id);
                return true;
            }
            return false;
        }, 0.25f);
}

// static
bool SLAvatarActions::canTeleportTo(const LLUUID& avatar_id)
{
    if (avatar_id.isNull())
        return false;

    LLWorld::pos_map_t positions;
    LLWorld::getInstance()->getAvatars(&positions);
    auto iter = positions.find(avatar_id);
    if (iter != positions.cend())
    {
        const auto& id = iter->first;
        const auto& pos = iter->second;
        if (id != gAgent.getID() && !pos.isNull())
        {
            return true;
        }
    }
    return false;
}

// static
void SLAvatarActions::teleportTo(const LLUUID& avatar_id)
{
    if (avatar_id.isNull())
        return;

    LLWorld::pos_map_t positions;
    LLWorld::getInstance()->getAvatars(&positions);
    auto iter = positions.find(avatar_id);
    if (iter != positions.cend())
    {
        const auto& id = iter->first;
        const auto& pos = iter->second;
        if (id != gAgentID && !pos.isNull())
        {
            gAgent.teleportViaLocation(pos);
        }
    }
}

void SLAvatarActions::reportAbuse(const LLUUID& idAgent)
{
    LLAvatarName avName;
    LLAvatarNameCache::get(idAgent, &avName);

    LLFloaterReporter::showFromAvatar(idAgent, avName.getCompleteName());
}

// static
void SLAvatarActions::watchModerationMenu(LLMenuGL* menu, const uuid_vec_t& ids)
{
    sModerationMenus.erase(std::remove_if(sModerationMenus.begin(), sModerationMenus.end(),
        [](const ModerationMenu& entry) { return entry.menu.isDead(); }), sModerationMenus.end());
    if (sModerationMenus.size() >= 16)
        sModerationMenus.erase(sModerationMenus.begin());
    sModerationMenus.push_back({menu->getHandle(), std::make_shared<const uuid_vec_t>(ids), gAgent.getSessionID()});
    // createMenu runs before show(), so establish first-open visibility here.
    if (LLView* manage = menu->findChildView("manage_menu", false))
        manage->setVisible(canFreezeEject(ids) || canManageAvatarsEstate(ids));
    menu->needsArrange();
}

// static
bool SLAvatarActions::canFreezeEject(const LLUUID& avatar_id)
{
    if (avatar_id.isNull())
        return false;

    uuid_vec_t ids = { avatar_id };
    return canFreezeEject(ids);
}

// static
bool SLAvatarActions::canFreezeEject(const uuid_vec_t& ids)
{
    if (ids.empty() || ids.size() > 64)
        return false;
    bool allowed = true;
    for (const LLUUID& id : ids)
    {
        LLSD target;
        if (!moderationTarget(id, target))
            return false;
        LLViewerRegion* region = LLWorld::getInstance()->getRegionFromID(target["region_id"].asUUID());
        if (!region)
            return false;
        // Personal ownership and estate authority are available without a parcel lookup.
        const LLVector3 pos = region->getPosRegionFromGlobal(moderationPosition(target, region));
        if (region->canManageEstate() || region->isOwnedSelf(pos))
            continue;
        // The overlay does not carry the owning group's UUID. On the agent's
        // parcel, the exact group and its powers are already available synchronously.
        LLViewerParcelMgr& parcels = LLViewerParcelMgr::instance();
        const LLVector3d position = moderationPosition(target, region);
        const LLParcel* parcel = parcels.getAgentParcel();
        if (parcel && parcel->getRegionID() == region->getRegionID() && parcels.inAgentParcel(position))
        {
            allowed &= parcel->getOwnerID() == gAgent.getID() || (parcel->getIsGroupOwned() &&
                gAgent.hasPowerInGroup(parcel->getOwnerID(), GP_LAND_ADMIN));
            continue;
        }
        // No requests for unrelated residents' land. Rights in another group can
        // only justify resolving this unknown group, never exposing an action.
        const bool candidate = region->isOwnedGroup(pos) &&
            std::any_of(gAgent.mGroups.begin(), gAgent.mGroups.end(), [](const LLGroupData& group)
                { return gAgent.hasPowerInGroup(group.mID, GP_LAND_ADMIN); });
        if (!candidate)
            return false;
        allowed &= canParcelEject(target, menuParcelIdentity(target));
    }
    return allowed;
}

// static
void SLAvatarActions::parcelFreeze(const LLUUID& avatar_id)
{
    uuid_vec_t ids = { avatar_id };
    return parcelFreeze(ids);
}

// static
void SLAvatarActions::parcelFreeze(const uuid_vec_t& ids)
{
    LLSD payload;
    if (!moderationTargets(ids, payload, true))
    {
        moderationUnavailable("selection-invalid", ids.size());
        return;
    }
    lookupModerationParcels(payload, [](const LLSD& checked)
    {
        LLSD args;
        args["AVATAR_NAMES"] = moderationNames(checked);
        LLNotificationsUtil::add(checked["targets"].size() == 1 ? "FreezeAvatar" : "FreezeAvatarMultiple",
                                 args, checked, handleParcelFreeze);
    });
}

// static
void SLAvatarActions::parcelEject(const LLUUID& avatar_id)
{
    uuid_vec_t ids = { avatar_id };
    return parcelEject(ids);
}

// static
void SLAvatarActions::parcelEject(const uuid_vec_t& ids)
{
    LLSD payload;
    if (!moderationTargets(ids, payload, true))
    {
        moderationUnavailable("selection-invalid", ids.size());
        return;
    }
    lookupModerationParcels(payload, [](const LLSD& checked)
    {
        LLSD confirmation = checked;
        bool ban_enabled = true;
        for (const LLSD& target : llsd::inArray(confirmation["targets"]))
            ban_enabled &= parcelPower(target["parcel"], GP_LAND_MANAGE_BANNED);
        confirmation["ban_enabled"] = ban_enabled;
        LLSD args;
        args["AVATAR_NAMES"] = moderationNames(confirmation);
        // Existing single-resident translations still use the singular token.
        args["AVATAR_NAME"] = args["AVATAR_NAMES"];
        const bool single = confirmation["targets"].size() == 1;
        const std::string notification = ban_enabled
            ? (single ? "EjectAvatarFullname" : "EjectAvatarMultiple")
            : (single ? "EjectAvatarFullnameNoBan" : "EjectAvatarMultipleNoBan");
        LLNotificationsUtil::add(notification, args, confirmation, handleParcelEject);
    });
}

// static
bool SLAvatarActions::canManageAvatarsEstate(const LLUUID& avatar_id)
{
    if (avatar_id.isNull())
        return false;

    uuid_vec_t ids = { avatar_id };
    return canManageAvatarsEstate(ids);
}

// static
bool SLAvatarActions::canManageAvatarsEstate(const uuid_vec_t& ids)
{
    if (ids.empty() || ids.size() > 64)
        return false;
    for (const LLUUID& id : ids)
    {
        LLSD target;
        if (!moderationTarget(id, target))
            return false;
        LLViewerRegion* region = LLWorld::getInstance()->getRegionFromID(target["region_id"].asUUID());
        if (!region || region->getOwner() == id || !region->canManageEstate())
            return false;
    }
    return true;
}

// static
void SLAvatarActions::estateTeleportHome(const LLUUID& avatar_id)
{
    uuid_vec_t ids = { avatar_id };
    estateTeleportHome(ids);
}

// static
void SLAvatarActions::estateTeleportHome(const uuid_vec_t& ids)
{
    LLSD payload;
    if (!moderationTargets(ids, payload, false))
    {
        moderationUnavailable("selection-invalid", ids.size());
        return;
    }
    LLSD args;
    args["AVATAR_NAMES"] = moderationNames(payload);
    LLNotificationsUtil::add(payload["targets"].size() == 1 ? "EstateTeleportHomeSingle" : "EstateTeleportHomeMultiple",
                             args, payload, handleEstateTeleportHome);
}

// static
void SLAvatarActions::estateKick(const LLUUID& avatar_id)
{
    uuid_vec_t ids = { avatar_id };
    estateKick(ids);
}

// static
void SLAvatarActions::estateKick(const uuid_vec_t& ids)
{
    LLSD payload;
    if (!moderationTargets(ids, payload, false))
    {
        moderationUnavailable("selection-invalid", ids.size());
        return;
    }
    LLSD args;
    args["AVATAR_NAMES"] = moderationNames(payload);
    LLNotificationsUtil::add(payload["targets"].size() == 1 ? "EstateKickSingle" : "EstateKickMultiple",
                             args, payload, handleEstateKick);
}

// static
void SLAvatarActions::estateBan(const LLUUID& avatar_id)
{
    if (avatar_id.isNull())
        return;

    uuid_vec_t ids = { avatar_id };
    estateBan(ids);
}

// static
void SLAvatarActions::estateBan(const uuid_vec_t& ids)
{
    if (ids.empty())
        return;

    auto region = gAgent.getRegion();
    if (!region)
        return;

    LLSD payload;
    payload["avatar_ids"] = LLSD::emptyArray();
    std::string avatars;
    for (auto id : ids)
    {
        if (id.notNull())
        {
            payload["avatar_ids"].append(id);

            if (!avatars.empty())
                avatars += "\n";

            avatars += LLSLURL("agent", id, "about").getSLURLString();
        }
    }

    LLSD args;
    args["AVATAR_NAMES"] = avatars;
    std::string owner = LLSLURL("agent", region->getOwner(), "inspect").getSLURLString();
    if (gAgent.isGodlike())
    {
        LLStringUtil::format_map_t owner_args;
        owner_args["[OWNER]"] = owner;
        args["ALL_ESTATES"] = LLTrans::getString("RegionInfoAllEstatesOwnedBy", owner_args);
    }
    else if (region->getOwner() == gAgent.getID())
    {
        args["ALL_ESTATES"] = LLTrans::getString("RegionInfoAllEstatesYouOwn");
    }
    else if (region->isEstateManager())
    {
        LLStringUtil::format_map_t owner_args;
        owner_args["[OWNER]"] = owner.c_str();
        args["ALL_ESTATES"] = LLTrans::getString("RegionInfoAllEstatesYouManage", owner_args);
    }

    bool single_user = (payload["avatar_ids"].size() == 1);
    LLNotificationsUtil::add(single_user ? "EstateBanSingle" : "EstateBanMultiple", args, payload, handleEstateBan);
}

// static
void SLAvatarActions::godFreeze(const LLUUID& id)
{
    LLSD args;
    args["AVATAR_NAME"] = LLSLURL("agent", id, "about").getSLURLString();

    LLSD payload;
    payload["type"] = static_cast<LLSD::Integer>(KICK_FLAGS_FREEZE);
    payload["avatar_id"] = id;
    LLNotifications::instance().add("FreezeUser", args, payload, handleGodKick);
}

// static
void SLAvatarActions::godUnfreeze(const LLUUID& id)
{
    LLSD args;
    args["AVATAR_NAME"] = LLSLURL("agent", id, "about").getSLURLString();

    LLSD payload;
    payload["type"] = static_cast<LLSD::Integer>(KICK_FLAGS_UNFREEZE);
    payload["avatar_id"] = id;
    LLNotifications::instance().add("UnFreezeUser", args, payload, handleGodKick);
}

// static
void SLAvatarActions::godKick(const LLUUID& id)
{
    LLSD args;
    args["AVATAR_NAME"] = LLSLURL("agent", id, "about").getSLURLString();

    LLSD payload;
    payload["type"] = static_cast<LLSD::Integer>(KICK_FLAGS_DEFAULT);
    payload["avatar_id"] = id;
    LLNotifications::instance().add("KickUser", args, payload, handleGodKick);
}

// static
bool SLAvatarActions::handleParcelFreeze(const LLSD& notification, const LLSD& response)
{
    const S32 option = LLNotificationsUtil::getSelectedOption(notification, response);
    const LLSD payload = notification["payload"];
    if (option != 0 && option != 1)
    {
        logModerationAbort(option < 0 ? "confirmation-dismissed" : "confirmation-cancelled", payload["targets"].size());
        return false;
    }
    if (!moderationSession(payload))
        return false;
    lookupModerationParcels(payload, [option](const LLSD& checked)
    {
        for (const LLSD& target : llsd::inArray(checked["targets"]))
        {
            LLViewerRegion* region = LLWorld::getInstance()->getRegionFromID(target["region_id"].asUUID());
            LLMessageSystem* msg = gMessageSystem;
            msg->newMessageFast(_PREHASH_FreezeUser);
            msg->nextBlockFast(_PREHASH_AgentData);
            msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
            msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
            msg->nextBlockFast(_PREHASH_Data);
            msg->addUUIDFast(_PREHASH_TargetID, target["id"].asUUID());
            msg->addU32Fast(_PREHASH_Flags, option == 1 ? 0x1 : 0x0);
            msg->sendReliable(region->getHost());
        }
    });
    return false;
}

// static
bool SLAvatarActions::handleParcelEject(const LLSD& notification, const LLSD& response)
{
    const S32 option = LLNotificationsUtil::getSelectedOption(notification, response);
    const LLSD payload = notification["payload"];
    // No-ban dialogs use option 1 for Cancel; full dialogs use it for Eject and Ban.
    if (option != 0 && !(option == 1 && payload["ban_enabled"].asBoolean()))
    {
        logModerationAbort(option < 0 ? "confirmation-dismissed" : "confirmation-cancelled", payload["targets"].size());
        return false;
    }
    if (!moderationSession(payload))
        return false;

    lookupModerationParcels(payload, [option](const LLSD& checked)
    {
        if (option == 1)
        {
            for (const LLSD& target : llsd::inArray(checked["targets"]))
            {
                if (!parcelPower(target["parcel"], GP_LAND_MANAGE_BANNED))
                {
                    moderationUnavailable("ban-authority-lost", checked["targets"].size());
                    return; // Never silently downgrade an explicit ban to an eject.
                }
            }
        }
        for (const LLSD& target : llsd::inArray(checked["targets"]))
        {
            LLViewerRegion* region = LLWorld::getInstance()->getRegionFromID(target["region_id"].asUUID());
            LLMessageSystem* msg = gMessageSystem;
            msg->newMessageFast(_PREHASH_EjectUser);
            msg->nextBlockFast(_PREHASH_AgentData);
            msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
            msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
            msg->nextBlockFast(_PREHASH_Data);
            msg->addUUIDFast(_PREHASH_TargetID, target["id"].asUUID());
            msg->addU32Fast(_PREHASH_Flags, option == 1 ? 0x1 : 0x0);
            msg->sendReliable(region->getHost());
        }
    });
    return false;
}

// static
bool SLAvatarActions::handleEstateTeleportHome(const LLSD& notification, const LLSD& response)
{
    if (LLNotificationsUtil::getSelectedOption(notification, response) != 0)
    {
        logModerationAbort("confirmation-cancelled-or-dismissed", notification["payload"]["targets"].size());
        return false;
    }
    const LLSD& payload = notification["payload"];
    if (!moderationSession(payload) || !payload["targets"].size())
        return false;
    for (const LLSD& target : llsd::inArray(payload["targets"]))
    {
        const char* reason = "estate-authority-lost";
        if (!sameModerationTarget(target, false, &reason) ||
            !canManageAvatarsEstate(target["id"].asUUID()))
        {
            moderationUnavailable(reason, payload["targets"].size());
            return false;
        }
    }
    for (const LLSD& target : llsd::inArray(payload["targets"]))
    {
        LLViewerRegion* region = LLWorld::getInstance()->getRegionFromID(target["region_id"].asUUID());
        LLMessageSystem* msg = gMessageSystem;
        msg->newMessageFast(_PREHASH_EstateOwnerMessage);
        msg->nextBlockFast(_PREHASH_AgentData);
        msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
        msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
        msg->addUUIDFast(_PREHASH_TransactionID, LLUUID::null);
        msg->nextBlockFast(_PREHASH_MethodData);
        msg->addStringFast(_PREHASH_Method, "teleporthomeuser");
        msg->addUUIDFast(_PREHASH_Invoice, LLUUID::null);
        msg->nextBlockFast(_PREHASH_ParamList);
        msg->addStringFast(_PREHASH_Parameter, gAgent.getID().asString());
        msg->nextBlockFast(_PREHASH_ParamList);
        msg->addStringFast(_PREHASH_Parameter, target["id"].asUUID().asString());
        msg->sendReliable(region->getHost());
    }
    return false;
}

// static
bool SLAvatarActions::handleEstateKick(const LLSD& notification, const LLSD& response)
{
    if (LLNotificationsUtil::getSelectedOption(notification, response) != 0)
    {
        logModerationAbort("confirmation-cancelled-or-dismissed", notification["payload"]["targets"].size());
        return false;
    }
    const LLSD& payload = notification["payload"];
    if (!moderationSession(payload) || !payload["targets"].size())
        return false;
    for (const LLSD& target : llsd::inArray(payload["targets"]))
    {
        const char* reason = "estate-authority-lost";
        if (!sameModerationTarget(target, false, &reason) ||
            !canManageAvatarsEstate(target["id"].asUUID()))
        {
            moderationUnavailable(reason, payload["targets"].size());
            return false;
        }
    }
    for (const LLSD& target : llsd::inArray(payload["targets"]))
    {
        LLViewerRegion* region = LLWorld::getInstance()->getRegionFromID(target["region_id"].asUUID());
        LLMessageSystem* msg = gMessageSystem;
        msg->newMessageFast(_PREHASH_EstateOwnerMessage);
        msg->nextBlockFast(_PREHASH_AgentData);
        msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
        msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
        msg->addUUIDFast(_PREHASH_TransactionID, LLUUID::null);
        msg->nextBlockFast(_PREHASH_MethodData);
        msg->addStringFast(_PREHASH_Method, "kickestate");
        msg->addUUIDFast(_PREHASH_Invoice, LLUUID::null);
        msg->nextBlockFast(_PREHASH_ParamList);
        msg->addStringFast(_PREHASH_Parameter, target["id"].asUUID().asString());
        msg->sendReliable(region->getHost());
    }
    return false;
}

// static
bool SLAvatarActions::handleEstateBan(const LLSD& notification, const LLSD& response)
{
    LLViewerRegion* region = gAgent.getRegion();
    if (!region)
        return false;

    S32 option = LLNotification::getSelectedOption(notification, response);

    if (0 == option || 1 == option)
    {
        const auto& avatar_ids = notification["payload"]["avatar_ids"];
        for (LLSD::array_const_iterator it = avatar_ids.beginArray(), it_end = avatar_ids.endArray(); it != it_end; ++it)
        {
            const auto& id = it->asUUID();
            if (region->getOwner() == id)
            {
                // Can't ban the owner!
                continue;
            }

            U32 flags = ESTATE_ACCESS_BANNED_AGENT_ADD | ESTATE_ACCESS_ALLOWED_AGENT_REMOVE;

            if (it + 1 != it_end)
            {
                flags |= ESTATE_ACCESS_NO_REPLY;
            }

            if (option == 1)
            {
                if (region->getOwner() == gAgent.getID() || gAgent.isGodlike())
                {
                    flags |= ESTATE_ACCESS_APPLY_TO_ALL_ESTATES;
                }
                else if (region->isEstateManager())
                {
                    flags |= ESTATE_ACCESS_APPLY_TO_MANAGED_ESTATES;
                }
            }

            LLFloaterRegionInfo::nextInvoice();
            LLPanelEstateAccess::sendEstateAccessDelta(flags, id);
        }
    }
    return false;
}

// static
bool SLAvatarActions::handleGodKick(const LLSD& notification, const LLSD& response)
{
    S32 option = LLNotification::getSelectedOption(notification, response);

    if (option == 0)
    {
        LLUUID avatar_id = notification["payload"]["avatar_id"].asUUID();
        U32 type = static_cast<U32>(notification["payload"]["type"].asInteger());
        LLMessageSystem* msg = gMessageSystem;

        msg->newMessageFast(_PREHASH_GodKickUser);
        msg->nextBlockFast(_PREHASH_UserInfo);
        msg->addUUIDFast(_PREHASH_GodID, gAgent.getID());
        msg->addUUIDFast(_PREHASH_GodSessionID, gAgent.getSessionID());
        msg->addUUIDFast(_PREHASH_AgentID, avatar_id);
        msg->addU32Fast(_PREHASH_KickFlags, type);
        msg->addStringFast(_PREHASH_Reason, response["message"].asString());
        gAgent.sendReliableMessage();
    }
    return false;
}

// Webprofile junk... for posterity!

static void on_avatar_name_show_profile(const LLUUID& agent_id, const LLAvatarName& av_name)
{
    LLFloaterWebContent::Params p;
    p.url(getProfileURL(av_name.getAccountName())).id(agent_id.asString());
    LLFloaterReg::showInstance("webprofile", p);
}

// static
void SLAvatarActions::showWebProfile(const LLUUID& id)
{
    if (id.notNull())
    {
        LLAvatarNameCache::get(id, boost::bind(&on_avatar_name_show_profile, _1, _2));
    }
}
