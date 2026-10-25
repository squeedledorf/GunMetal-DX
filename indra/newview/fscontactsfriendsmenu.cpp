/**
 * @file fscontactsfriendsmenu.cpp
 * @brief Menu used by Firestorm contacts floater
 *
 * $LicenseInfo:firstyear=2014&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (c) 2014 Ansariel Hiller @ Second Life
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
 * The Phoenix Firestorm Project, Inc., 1831 Oakwood Drive, Fairmont, Minnesota 56031-3225 USA
 * http://www.firestormviewer.org
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "fscontactsfriendsmenu.h"

#include "fsradar.h"
#include "llavataractions.h"
#include "lllogchat.h"
#include "llmenugl.h"
#include "llslurl.h"
#include "llurlaction.h"
#include "llviewercontrol.h"
#include "llcallingcard.h"
#include "rlvactions.h"
#include "rlvhandler.h"
#include "slavataractions.h"

FSContactsFriendsMenu gFSContactsFriendsMenu;

LLContextMenu* FSContactsFriendsMenu::createMenu()
{
    LLUICtrl::CommitCallbackRegistry::ScopedRegistrar registrar;
    LLUICtrl::EnableCallbackRegistry::ScopedRegistrar enable_registrar;

    if (mUUIDs.size() == 1)
    {
        const LLUUID& id = mUUIDs.front();
        registrar.add("Contacts.Friends.ShowProfile",           boost::bind(&LLAvatarActions::showProfile,                      id));
        registrar.add("Contacts.Friends.RemoveFriend",          boost::bind(&LLAvatarActions::removeFriendDialog,               id));
        registrar.add("Contacts.Friends.SendIM",                boost::bind(&LLAvatarActions::startIM,                          id));
        registrar.add("Contacts.Friends.Calllog",               boost::bind(&LLAvatarActions::viewChatHistory,                  id));
        registrar.add("Contacts.Friends.OfferTeleport",         boost::bind(&FSContactsFriendsMenu::offerTeleport,              this));
        registrar.add("Contacts.Friends.RequestTeleport",       boost::bind(&LLAvatarActions::teleportRequest,                  id));
        registrar.add("Contacts.Friends.ZoomIn",                boost::bind(&SLAvatarActions::zoomIn,                           id));
        registrar.add("Contacts.Friends.Pay",                   boost::bind(&LLAvatarActions::pay,                              id));
        registrar.add("Contacts.Friends.ShowOnMap",             boost::bind(&LLAvatarActions::showOnMap,                        id));
        registrar.add("Contacts.Friends.AddToContactSet",       boost::bind(&FSContactsFriendsMenu::addToContactSet,            this));
        registrar.add("Contacts.Friends.TeleportToTarget",      boost::bind(&FSContactsFriendsMenu::teleportToAvatar,           this));
        registrar.add("Contacts.Friends.TrackAvatar",           boost::bind(&FSContactsFriendsMenu::onTrackAvatarMenuItemClick, this));
        registrar.add("Contacts.Friends.CopyLabel",             boost::bind(&FSContactsFriendsMenu::copyNameToClipboard,        this, id));
        registrar.add("Contacts.Friends.CopyUrl",               boost::bind(&FSContactsFriendsMenu::copySLURLToClipboard,       this, id));
        registrar.add("Mention.CopyURI",                        boost::bind(&FSContactsFriendsMenu::copyURLToClipboard,         this));

        enable_registrar.add("Contacts.Friends.EnableItem",     boost::bind(&FSContactsFriendsMenu::enableContextMenuItem,      this, _2));
        enable_registrar.add("Contacts.Friends.EnableZoomIn",   boost::bind(&SLAvatarActions::canZoomIn,                        id));

        return createFromFile("menu_fs_contacts_friends.xml");
    }
    else
    {
        registrar.add("Contacts.Friends.SendIM",                boost::bind(&LLAvatarActions::startConference,                  mUUIDs, LLUUID::null));
        registrar.add("Contacts.Friends.OfferTeleport",         boost::bind(&FSContactsFriendsMenu::offerTeleport,              this));
        registrar.add("Contacts.Friends.RemoveFriend",          boost::bind(&LLAvatarActions::removeFriendsDialog,              mUUIDs));
        registrar.add("Contacts.Friends.AddToContactSet",       boost::bind(&FSContactsFriendsMenu::addToContactSet,            this));

        enable_registrar.add("Contacts.Friends.EnableItem",     boost::bind(&FSContactsFriendsMenu::enableContextMenuItem,      this, _2));

        return createFromFile("menu_fs_contacts_friends_multiselect.xml");
    }
}

bool FSContactsFriendsMenu::enableContextMenuItem(const LLSD& userdata)
{
    std::string item = userdata.asString();

    if (item == "remove_friend")
    {
        bool result = (mUUIDs.size() > 0);

        for (uuid_vec_t::const_iterator it = mUUIDs.begin(); it != mUUIDs.end(); ++it)
        {
            if (!LLAvatarActions::isFriend(*it))
            {
                result = false;
                break;
            }
        }

        return result;
    }
    else if (item == "teleport_to")
    {
        if (mUUIDs.size() == 1)
        {
            return SLAvatarActions::canTeleportTo(mUUIDs.front());
        }
        return false;
    }
    else if (item == "offer_teleport")
    {
        if (!LLAvatarActions::canOfferTeleport(mUUIDs))
        {
            return false;
        }
        if (gRlvHandler.hasBehaviour(RLV_BHVR_SHOWLOC))
        {
            for (const LLUUID& id : mUUIDs)
            {
                const LLRelationship* relationship = LLAvatarTracker::instance().getBuddyInfo(id);
                if (!gRlvHandler.isException(RLV_BHVR_TPLURE, id, ERlvExceptionCheck::Permissive)
                    && !(relationship && relationship->isRightGrantedTo(LLRelationship::GRANT_MAP_LOCATION)))
                {
                    return false;
                }
            }
        }
        return true;
    }
    else if (item == "show_on_map")
    {
        return mUUIDs.size() == 1 && LLAvatarActions::isAgentMappable(mUUIDs.front())
            && !gRlvHandler.hasBehaviour(RLV_BHVR_SHOWWORLDMAP);
    }
    else if (item == "request_teleport")
    {
        if (mUUIDs.size() == 1)
        {
            return LLAvatarActions::canOfferTeleport(mUUIDs.front());
        }
        return false;
    }
    else if (item == "track_avatar")
    {
        if (mUUIDs.size() == 1)
        {
            return FSRadar::getInstance()->getEntry(mUUIDs.front()) != nullptr;
        }
        return false;
    }
    else if (item == "can_pay")
    {
        return mUUIDs.size() == 1 && RlvActions::canPayAvatar(mUUIDs.front());
    }
    else if (item == "can_callog")
    {
        if (mUUIDs.size() == 1)
        {
            return LLLogChat::isTranscriptExist(mUUIDs.front());
        }
        return false;
    }
    return false;
}

void FSContactsFriendsMenu::offerTeleport()
{
    LLAvatarActions::offerTeleport(mUUIDs);
}

void FSContactsFriendsMenu::teleportToAvatar()
{
    SLAvatarActions::teleportTo(mUUIDs.front());
}

void FSContactsFriendsMenu::onTrackAvatarMenuItemClick()
{
    FSRadar::getInstance()->startTracking(mUUIDs.front());
}

void FSContactsFriendsMenu::addToContactSet()
{
    LLAvatarActions::addToContactSet(mUUIDs);
}

void FSContactsFriendsMenu::copyNameToClipboard(const LLUUID& id)
{
    LLAvatarName av_name;
    LLAvatarNameCache::get(id, &av_name);
    LLUrlAction::copyURLToClipboard(av_name.getAccountName());
}

void FSContactsFriendsMenu::copySLURLToClipboard(const LLUUID& id)
{
    LLUrlAction::copyURLToClipboard(LLSLURL("agent", id, "about").getSLURLString());
}

void FSContactsFriendsMenu::copyURLToClipboard()
{
    LLUrlAction::copyURLToClipboard("secondlife:///app/agent/" + mUUIDs.front().asString() + "/mention");
}
