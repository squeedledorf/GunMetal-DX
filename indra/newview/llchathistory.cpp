/**
 * @file llchathistory.cpp
 * @brief LLTextEditor base class
 *
 * $LicenseInfo:firstyear=2001&license=viewerlgpl$
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

#include "llchathistory.h"
#include "llchatimagepreview.h"

#include <boost/signals2.hpp>

#include "llavatarnamecache.h"
#include "llinstantmessage.h"

#include "llimview.h"
#include "llcommandhandler.h"
#include "llpanel.h"
#include "lluictrlfactory.h"
#include "llscrollcontainer.h"
#include "llagent.h"
#include "llagentdata.h"
#include "llavataractions.h"
#include "llavatariconctrl.h"
#include "llcallingcard.h" //for LLAvatarTracker
#include "llgroupactions.h"
#include "llgroupmgr.h"
#include "llspeakers.h" //for LLIMSpeakerMgr
#include "lltrans.h"
#include "llfloaterreg.h"
#include "llfloaterreporter.h"
#include "llfloatersidepanelcontainer.h"
#include "llmutelist.h"
#include "llstylemap.h"
#include "llslurl.h"
#include "llfocusmgr.h"
#include "lllayoutstack.h"
#include "llnotifications.h"
#include "llnotificationsutil.h"
#include "lltoastnotifypanel.h"
#include "lltooltip.h"
#include "llviewerregion.h"
#include "llviewertexteditor.h"
#include "llworld.h"
#include "lluiconstants.h"
#include "llstring.h"
#include "llurlaction.h"
#include "llfloaterblocked.h"
#include "llviewercontrol.h"
#include "llviewermenu.h"
#include "llviewerobjectlist.h"
// [RLVa:KB] - Checked: 2010-04-22 (RLVa-1.2.0f)
#include "rlvactions.h"
#include "rlvcommon.h"
// [/RLVa:KB]

static LLDefaultChildRegistry::Register<LLChatHistory> r("chat_history");

const static std::string NEW_LINE(rawstr_to_utf8("\n"));

const static std::string SLURL_APP_AGENT = "secondlife:///app/agent/";
const static std::string SLURL_ABOUT = "/about";

// support for secondlife:///app/objectim/{UUID}/ SLapps
class LLObjectIMHandler : public LLCommandHandler
{
public:
    // requests will be throttled from a non-trusted browser
    LLObjectIMHandler() : LLCommandHandler("objectim", UNTRUSTED_THROTTLE) {}

    bool handle(const LLSD& params, const LLSD& query_map, const std::string& grid, LLMediaCtrl* web)
    {
        if (params.size() < 1)
        {
            return false;
        }

        LLUUID object_id;
        if (!object_id.set(params[0], false))
        {
            return false;
        }

        LLSD payload;
        payload["object_id"] = object_id;
        payload["owner_id"] = query_map["owner"];
// [RLVa:KB] - Checked: 2010-11-02 (RLVa-1.2.2a) | Modified: RLVa-1.2.2a
        if (query_map.has("rlv_shownames"))
            payload["rlv_shownames"] = query_map["rlv_shownames"];
// [/RLVa:KB]
        payload["name"] = query_map["name"];
        payload["slurl"] = LLWeb::escapeURL(query_map["slurl"]);
        payload["group_owned"] = query_map["groupowned"];
        LLFloaterReg::showInstance("inspect_remote_object", payload);
        return true;
    }
};
LLObjectIMHandler gObjectIMHandler;

class LLChatHistoryHeader: public LLPanel
{
public:
    LLChatHistoryHeader()
    :   LLPanel(),
        mInfoCtrl(NULL),
// [RLVa:KB] - Checked: 2010-04-22 (RLVa-1.2.2a) | Added: RLVa-1.2.0f
        mShowContextMenu(true),
        mShowInfoCtrl(true),
// [/RLVa:KB]
        mPopupMenuHandleAvatar(),
        mPopupMenuHandleObject(),
        mAvatarID(),
        mSourceType(CHAT_SOURCE_UNKNOWN),
        mFrom(),
        mSessionID(),
        mCreationTime(time_corrected()),
        mMinUserNameWidth(0),
        mUserNameFont(NULL),
        mUserNameTextBox(NULL),
        mTimeBoxTextBox(NULL),
        mNeedsTimeBox(true),
        mAvatarNameCacheConnection()
    {}

    static LLChatHistoryHeader* createInstance(const std::string& file_name)
    {
        LLChatHistoryHeader* pInstance = new LLChatHistoryHeader;
        pInstance->buildFromFile(file_name);
        return pInstance;
    }

    ~LLChatHistoryHeader()
    {
        if (mAvatarNameCacheConnection.connected())
        {
            mAvatarNameCacheConnection.disconnect();
        }
        auto menu = mPopupMenuHandleAvatar.get();
        if (menu)
        {
            menu->die();
            mPopupMenuHandleAvatar.markDead();
        }
        menu = mPopupMenuHandleObject.get();
        if (menu)
        {
            menu->die();
            mPopupMenuHandleObject.markDead();
        }
    }

    bool handleMouseUp(S32 x, S32 y, MASK mask)
    {
        return LLPanel::handleMouseUp(x,y,mask);
    }

    void onObjectIconContextMenuItemClicked(const LLSD& userdata)
    {
        std::string level = userdata.asString();

        if (level == "profile")
        {
            LLFloaterReg::showInstance("inspect_remote_object", mObjectData);
        }
        else if (level == "block")
        {
            LLMuteList::getInstance()->add(LLMute(getAvatarId(), mFrom, LLMute::OBJECT));

            LLFloaterBlocked::showMuteAndSelect(getAvatarId());
            //LLFloaterSidePanelContainer::showPanel("people", "panel_people",
            //  LLSD().with("people_panel_tab_name", "blocked_panel").with("blocked_to_select", getAvatarId()));
        }
        else if (level == "unblock")
        {
            LLMuteList::getInstance()->remove(LLMute(getAvatarId(), mFrom, LLMute::OBJECT));
        }
        else if (level == "map")
        {
            std::string url = "secondlife://" + mObjectData["slurl"].asString();
            LLUrlAction::showLocationOnMap(url);
        }
        else if (level == "teleport")
        {
            std::string url = "secondlife://" + mObjectData["slurl"].asString();
            LLUrlAction::teleportToLocation(url);
        }
        else if (level == "obj_zoom_in")
        {
            LLUUID obj_id = mObjectData["object_id"];
            if (obj_id.notNull())
            {
                handle_zoom_to_object(obj_id);
            }
        }
    }

    bool onObjectIconContextMenuItemVisible(const LLSD& userdata)
    {
        std::string level = userdata.asString();
        if (level == "is_blocked")
        {
            return LLMuteList::getInstance()->isMuted(getAvatarId(), mFrom, LLMute::flagTextChat);
        }
        else if (level == "not_blocked")
        {
            return !LLMuteList::getInstance()->isMuted(getAvatarId(), mFrom, LLMute::flagTextChat);
        }
        else if (level == "obj_zoom_in")
        {
            LLUUID obj_id = mObjectData["object_id"];
            if (obj_id.notNull())
            {
                return nullptr != gObjectList.findObject(mAvatarID);
            }
            return false;
        }
        return false;
    }

    void banGroupMember(const LLUUID& participant_uuid)
    {
        LLUUID group_uuid = mSessionID;
        LLGroupMgrGroupData* gdatap = LLGroupMgr::getInstance()->getGroupData(group_uuid);
        if (!gdatap)
        {
            // Not a group
            return;
        }

        gdatap->banMemberById(participant_uuid);
    }

    bool canBanInGroup()
    {
        LLUUID group_uuid = mSessionID;
        LLGroupMgrGroupData* gdatap = LLGroupMgr::getInstance()->getGroupData(group_uuid);
        if (!gdatap)
        {
            // Not a group
            return false;
        }

        if (gAgent.hasPowerInGroup(group_uuid, GP_ROLE_REMOVE_MEMBER)
            && gAgent.hasPowerInGroup(group_uuid, GP_GROUP_BAN_ACCESS))
        {
            return true;
        }

        return false;
    }

    bool canBanGroupMember(const LLUUID& participant_uuid)
    {
        LLUUID group_uuid = mSessionID;
        LLGroupMgrGroupData* gdatap = LLGroupMgr::getInstance()->getGroupData(group_uuid);
        if (!gdatap)
        {
            // Not a group
            return false;
        }

        if (gdatap->mPendingBanRequest)
        {
            return false;
        }

        if (gAgentID == getAvatarId())
        {
            //Don't ban self
            return false;
        }

        if (gdatap->isRoleMemberDataComplete())
        {
            if (gdatap->mMembers.size())
            {
                LLGroupMgrGroupData::member_list_t::iterator mi = gdatap->mMembers.find(participant_uuid);
                if (mi != gdatap->mMembers.end())
                {
                    LLGroupMemberData* member_data = (*mi).second;
                    // Is the member an owner?
                    if (member_data && member_data->isInRole(gdatap->mOwnerRole))
                    {
                        return false;
                    }

                    if (gAgent.hasPowerInGroup(group_uuid, GP_ROLE_REMOVE_MEMBER)
                        && gAgent.hasPowerInGroup(group_uuid, GP_GROUP_BAN_ACCESS))
                    {
                        return true;
                    }
                }
            }
        }

        LLSpeakerMgr * speaker_mgr = LLIMModel::getInstance()->getSpeakerManager(mSessionID);
        if (speaker_mgr)
        {
            LLSpeaker * speakerp = speaker_mgr->findSpeaker(participant_uuid).get();

            if (speakerp
                && gAgent.hasPowerInGroup(group_uuid, GP_ROLE_REMOVE_MEMBER)
                && gAgent.hasPowerInGroup(group_uuid, GP_GROUP_BAN_ACCESS))
            {
                return true;
            }
        }

        return false;
    }

    bool isGroupModerator()
    {
        LLSpeakerMgr * speaker_mgr = LLIMModel::getInstance()->getSpeakerManager(mSessionID);
        if (!speaker_mgr)
        {
            LL_WARNS() << "Speaker manager is missing" << LL_ENDL;
            return false;
        }

        // Is session a group call/chat?
        if(gAgent.isInGroup(mSessionID))
        {
            LLSpeaker * speakerp = speaker_mgr->findSpeaker(gAgentID).get();

            // Is agent a moderator?
            return speakerp && speakerp->mIsModerator;
        }

        return false;
    }

    bool canModerate(const std::string& userdata)
    {
        // only group moderators can perform actions related to this "enable callback"
        if (!isGroupModerator() || gAgentID == getAvatarId())
        {
            return false;
        }

        LLSpeakerMgr * speaker_mgr = LLIMModel::getInstance()->getSpeakerManager(mSessionID);
        if (!speaker_mgr)
        {
            return false;
        }

        LLSpeaker * speakerp = speaker_mgr->findSpeaker(getAvatarId()).get();
        if (!speakerp)
        {
            return false;
        }

        bool voice_channel = speakerp->isInVoiceChannel();

        if ("can_moderate_voice" == userdata)
        {
            return voice_channel;
        }
        else if ("can_mute" == userdata)
        {
            return voice_channel && (speakerp->mStatus != LLSpeaker::STATUS_MUTED);
        }
        else if ("can_unmute" == userdata)
        {
            return speakerp->mStatus == LLSpeaker::STATUS_MUTED;
        }
        else if ("can_allow_text_chat" == userdata)
        {
            return true;
        }

        return false;
    }

    void onAvatarIconContextMenuItemClicked(const LLSD& userdata)
    {
        std::string level = userdata.asString();

        if (level == "profile")
        {
            LLAvatarActions::showProfile(getAvatarId());
        }
        else if (level == "im")
        {
            LLAvatarActions::startIM(getAvatarId());
        }
        else if (level == "teleport")
        {
            LLAvatarActions::offerTeleport(getAvatarId());
        }
        else if (level == "request_teleport")
        {
            LLAvatarActions::teleportRequest(getAvatarId());
        }
        else if (level == "voice_call")
        {
            LLAvatarActions::startCall(getAvatarId());
        }
        else if (level == "chat_history")
        {
            LLAvatarActions::viewChatHistory(getAvatarId());
        }
        else if (level == "add")
        {
            LLAvatarActions::requestFriendshipDialog(getAvatarId(), mFrom);
        }
        else if (level == "remove")
        {
            LLAvatarActions::removeFriendDialog(getAvatarId());
        }
        else if (level == "invite_to_group")
        {
            LLAvatarActions::inviteToGroup(getAvatarId());
        }
        else if (level == "zoom_in")
        {
            handle_zoom_to_object(getAvatarId());
        }
        else if (level == "map")
        {
            LLAvatarActions::showOnMap(getAvatarId());
        }
        else if (level == "share")
        {
            LLAvatarActions::share(getAvatarId());
        }
        else if (level == "pay")
        {
            LLAvatarActions::pay(getAvatarId());
        }
        else if (level == "report_abuse")
        {
            std::string time_string;
            if (mTime > 0) // have frame time
            {
                time_t current_time = time_corrected();
                time_t message_time = (time_t)(current_time - LLFrameTimer::getElapsedSeconds() + mTime);

                time_string = "[" + LLTrans::getString("TimeMonth") + "]/["
                    + LLTrans::getString("TimeDay") + "]/["
                    + LLTrans::getString("TimeYear") + "] ["
                    + LLTrans::getString("TimeHour") + "]:["
                    + LLTrans::getString("TimeMin") + "]";

                LLSD substitution;

                substitution["datetime"] = (S32)message_time;
                LLStringUtil::format(time_string, substitution);
            }
            else
            {
                // From history. This might be empty or not full.
                // See LLChatLogParser::parse
                time_string = getChild<LLTextBox>("time_box")->getValue().asString();

                // Just add current date if not full.
                // Should be fine since both times are supposed to be stl
                if (!time_string.empty() && time_string.size() < 7)
                {
                    time_string = "[" + LLTrans::getString("TimeMonth") + "]/["
                        + LLTrans::getString("TimeDay") + "]/["
                        + LLTrans::getString("TimeYear") + "] " + time_string;

                    LLSD substitution;
                    // To avoid adding today's date to yesterday's timestamp,
                    // use creation time instead of current time
                    substitution["datetime"] = (S32)mCreationTime;
                    LLStringUtil::format(time_string, substitution);
                }
            }
            LLFloaterReporter::showFromChat(mAvatarID, mFrom, time_string, mText);
        }
        else if(level == "block_unblock")
        {
            LLAvatarActions::toggleMute(getAvatarId(), LLMute::flagVoiceChat);
        }
        else if(level == "mute_unmute")
        {
            LLAvatarActions::toggleMute(getAvatarId(), LLMute::flagTextChat);
        }
        else if(level == "toggle_allow_text_chat")
        {
            LLIMSpeakerMgr* speaker_mgr = LLIMModel::getInstance()->getSpeakerManager(mSessionID);
            speaker_mgr->toggleAllowTextChat(getAvatarId());
        }
        else if(level == "group_mute")
        {
            LLIMSpeakerMgr* speaker_mgr = LLIMModel::getInstance()->getSpeakerManager(mSessionID);
            if (speaker_mgr)
            {
                speaker_mgr->moderateVoiceParticipant(getAvatarId(), false);
            }
        }
        else if(level == "group_unmute")
        {
            LLIMSpeakerMgr* speaker_mgr = LLIMModel::getInstance()->getSpeakerManager(mSessionID);
            if (speaker_mgr)
            {
                speaker_mgr->moderateVoiceParticipant(getAvatarId(), true);
            }
        }
        else if(level == "ban_member")
        {
            banGroupMember(getAvatarId());
        }
    }

    bool onAvatarIconContextMenuItemChecked(const LLSD& userdata)
    {
        std::string level = userdata.asString();

        if (level == "is_blocked")
        {
            return LLMuteList::getInstance()->isMuted(getAvatarId(), LLMute::flagVoiceChat);
        }
        else if (level == "is_muted")
        {
            return LLMuteList::getInstance()->isMuted(getAvatarId(), LLMute::flagTextChat);
        }
        else if (level == "is_allowed_text_chat")
        {
            if (gAgent.isInGroup(mSessionID))
            {
                LLIMSpeakerMgr* speaker_mgr = LLIMModel::getInstance()->getSpeakerManager(mSessionID);
                if(speaker_mgr)
                {
                    const LLSpeaker * speakerp = speaker_mgr->findSpeaker(getAvatarId());
                    if (NULL != speakerp)
                    {
                        return !speakerp->mModeratorMutedText;
                    }
                }
            }
            return false;
        }
        return false;
    }

    bool onAvatarIconContextMenuItemEnabled(const LLSD& userdata)
    {
        std::string level = userdata.asString();

        if (level == "can_allow_text_chat" || level == "can_mute" || level == "can_unmute")
        {
            return canModerate(userdata);
        }
        else if (level == "report_abuse")
        {
            return gAgentID != mAvatarID;
        }
// [RLVa:KB] - @pay
        else if (level == "can_pay")
        {
            return RlvActions::canPayAvatar(getAvatarId());
        }
// [/RLVa:KB]
        else if (level == "can_ban_member")
        {
            return canBanGroupMember(getAvatarId());
        }
        return false;
    }

    bool onAvatarIconContextMenuItemVisible(const LLSD& userdata)
    {
        std::string level = userdata.asString();

        if (level == "show_mute")
        {
            LLSpeakerMgr * speaker_mgr = LLIMModel::getInstance()->getSpeakerManager(mSessionID);
            if (speaker_mgr)
            {
                LLSpeaker * speakerp = speaker_mgr->findSpeaker(getAvatarId()).get();
                if (speakerp)
                {
                    return speakerp->isInVoiceChannel() && speakerp->mStatus != LLSpeaker::STATUS_MUTED;
                }
            }
            return false;
        }
        else if (level == "show_unmute")
        {
            LLSpeakerMgr * speaker_mgr = LLIMModel::getInstance()->getSpeakerManager(mSessionID);
            if (speaker_mgr)
            {
                LLSpeaker * speakerp = speaker_mgr->findSpeaker(getAvatarId()).get();
                if (speakerp)
                {
                    return speakerp->mStatus == LLSpeaker::STATUS_MUTED;
                }
            }
            return false;
        }
        return false;
    }

    bool postBuild()
    {
        setDoubleClickCallback(boost::bind(&LLChatHistoryHeader::showInspector, this));

        setMouseEnterCallback(boost::bind(&LLChatHistoryHeader::showInfoCtrl, this));
        setMouseLeaveCallback(boost::bind(&LLChatHistoryHeader::hideInfoCtrl, this));

        mUserNameTextBox = getChild<LLTextBox>("user_name");
        mTimeBoxTextBox = getChild<LLTextBox>("time_box");

        // #369: a recycled header starts over from its state as built
        mPristineRect = getRect();
        mPristineUserNameRect = mUserNameTextBox->getRect();
        mPristineTimeBoxRect = mTimeBoxTextBox->getRect();
        mPristineTimeBoxVisible = mTimeBoxTextBox->getVisible();
        mPristineBgAlphaColor = getTransparentColor();

        mInfoCtrl = LLUICtrlFactory::getInstance()->createFromFile<LLUICtrl>("inspector_info_ctrl.xml", this, LLPanel::child_registry_t::instance());
        if (mInfoCtrl)
        {
            mInfoCtrl->setCommitCallback(boost::bind(&LLChatHistoryHeader::onClickInfoCtrl, mInfoCtrl));
            mInfoCtrl->setVisible(false);
        }
        else
        {
            LL_ERRS() << "Failed to create an interface element due to missing or corrupted file inspector_info_ctrl.xml" << LL_ENDL;
        }

        return LLPanel::postBuild();
    }

    bool pointInChild(const std::string& name,S32 x,S32 y)
    {
        LLUICtrl* child = findChild<LLUICtrl>(name);
        if(!child)
            return false;

        LLView* parent = child->getParent();
        if(parent!=this)
        {
            x-=parent->getRect().mLeft;
            y-=parent->getRect().mBottom;
        }

        S32 local_x = x - child->getRect().mLeft ;
        S32 local_y = y - child->getRect().mBottom ;
        return  child->pointInView(local_x, local_y);
    }

    bool handleRightMouseDown(S32 x, S32 y, MASK mask)
    {
        if(pointInChild("avatar_icon",x,y) || pointInChild("user_name",x,y))
        {
            showContextMenu(x,y);
            return true;
        }

        return LLPanel::handleRightMouseDown(x,y,mask);
    }

    void showInspector()
    {
//      if (mAvatarID.isNull() && CHAT_SOURCE_SYSTEM != mSourceType && CHAT_SOURCE_REGION != mSourceType) return;
// [RLVa:KB] - Checked: 2010-04-22 (RLVa-1.2.2a) | Added: RLVa-1.2.0f
        // Don't double-click show the inspector if we're not showing the info control
        if ( (!mShowInfoCtrl) || (mAvatarID.isNull() && CHAT_SOURCE_SYSTEM != mSourceType && CHAT_SOURCE_REGION != mSourceType) ) return;
// [/RLVa:KB]

        if (mSourceType == CHAT_SOURCE_OBJECT)
        {
            LLFloaterReg::showInstance("inspect_remote_object", mObjectData);
        }
        else if (mSourceType == CHAT_SOURCE_AGENT)
        {
            LLFloaterReg::showInstance("inspect_avatar", LLSD().with("avatar_id", mAvatarID));
        }
        //if chat source is system, you may add "else" here to define behaviour.
    }

    static void onClickInfoCtrl(LLUICtrl* info_ctrl)
    {
        if (!info_ctrl) return;

        LLChatHistoryHeader* header = dynamic_cast<LLChatHistoryHeader*>(info_ctrl->getParent());
        if (!header) return;

        header->showInspector();
    }


    const LLUUID&       getAvatarId () const { return mAvatarID;}

    // #369: back to the state buildFromFile() left it in, for setup() of another message.
    // setTimeField() and draw() resize the name and time boxes relative to where they are.
    void resetForReuse()
    {
        if (mAvatarNameCacheConnection.connected())
        {
            mAvatarNameCacheConnection.disconnect();
        }
        // context menus bound to the previous message
        if (auto menu = mPopupMenuHandleAvatar.get())
        {
            menu->die();
            mPopupMenuHandleAvatar.markDead();
        }
        if (auto menu = mPopupMenuHandleObject.get())
        {
            menu->die();
            mPopupMenuHandleObject.markDead();
        }
        setShape(mPristineRect);
        if (mUserNameTextBox)
        {
            mUserNameTextBox->setShape(mPristineUserNameRect);
            mUserNameTextBox->setToolTip(LLStringUtil::null);
        }
        if (mTimeBoxTextBox)
        {
            mTimeBoxTextBox->setShape(mPristineTimeBoxRect);
            mTimeBoxTextBox->setVisible(mPristineTimeBoxVisible);
        }
        setToolTip(LLStringUtil::null);
        setTransparentColor(mPristineBgAlphaColor);
        if (mInfoCtrl)
        {
            mInfoCtrl->setVisible(false);
        }
        // The icon must forget the previous message's avatar: left alone it stays registered
        // with LLAvatarPropertiesProcessor for that avatar while the header shows someone or
        // something else (a reply would put that avatar's picture on an object's header), and
        // the next setup() can move it straight from that avatar to "no avatar".
        LLAvatarIconCtrl* icon = getChild<LLAvatarIconCtrl>("avatar_icon");
        icon->clearAvatar();
        icon->setDrawTooltip(true);
// [RLVa:KB] - Checked: 2010-04-22 (RLVa-1.2.2a) | Added: RLVa-1.2.0f
        mShowContextMenu = true;
        mShowInfoCtrl = true;
// [/RLVa:KB]
        mAvatarID.setNull();
        mSessionID.setNull();
        mSourceType = CHAT_SOURCE_UNKNOWN;
        mObjectData = LLSD();
        mFrom.clear();
        mText.clear();
        mTime = 0.0;
        mCreationTime = time_corrected();
        mMinUserNameWidth = 0;
        mNeedsTimeBox = true;
    }

    void setup(const LLChat& chat, const LLStyle::Params& style_params, const LLSD& args)
    {
        mAvatarID = chat.mFromID;
        mSessionID = chat.mSessionID;
        mSourceType = chat.mSourceType;

        // To be able to report a message, we need a copy of it's text
        // and it's easier to store text directly than trying to get
        // it from a lltextsegment or chat's mEditor
        mText = chat.mText;
        mTime = chat.mTime;

        //*TODO overly defensive thing, source type should be maintained out there
        if((chat.mFromID.isNull() && chat.mFromName.empty()) || (chat.mFromName == SYSTEM_FROM && chat.mFromID.isNull()))
        {
            mSourceType = CHAT_SOURCE_SYSTEM;
        }

        mUserNameFont = style_params.font();
        if (!mUserNameTextBox)
        {
            mUserNameTextBox = getChild<LLTextBox>("user_name");
            mTimeBoxTextBox = getChild<LLTextBox>("time_box");
        }
        LLTextBox* user_name = mUserNameTextBox;
        user_name->setReadOnlyColor(style_params.readonly_color());
        user_name->setColor(style_params.color());

        if (mSourceType == CHAT_SOURCE_TELEPORT
            && chat.mChatStyle == CHAT_STYLE_TELEPORT_SEP)
        {
            mFrom = chat.mFromName;
            user_name->setValue(mFrom);
            updateMinUserNameWidth();
            LLUIColor sep_color = LLUIColorTable::instance().getColor("ChatTeleportSeparatorColor");
            setTransparentColor(sep_color);
        }
        else if (chat.mFromName.empty()
                 || mSourceType == CHAT_SOURCE_SYSTEM)
        {
            mFrom = LLTrans::getString("SECOND_LIFE");
            if(!chat.mFromName.empty() && (mFrom != chat.mFromName))
            {
                mFrom += " (" + chat.mFromName + ")";
            }
            user_name->setValue(mFrom);
            updateMinUserNameWidth();
        }
        else if (LLViewerChat::hideSenderName(chat))
        {
            // #463: unnamed object, header keeps the object icon and inspector but no name
            mFrom.clear();
            user_name->setValue(LLSD());
            updateMinUserNameWidth();
        }
        else if (mSourceType == CHAT_SOURCE_AGENT
                 && !mAvatarID.isNull()
                 && chat.mChatStyle != CHAT_STYLE_HISTORY)
        {
            // ...from a normal user, lookup the name and fill in later.
            // *NOTE: Do not do this for chat history logs, otherwise the viewer
            // will flood the People API with lookup requests on startup

            // Start with blank so sample data from XUI XML doesn't
            // flash on the screen
//          user_name->setValue( LLSD() );
//          fetchAvatarName();
// [RLVa:KB] - Checked: 2010-11-01 (RLVa-1.2.2a) | Added: RLVa-1.2.2a
            if (!chat.mRlvNamesFiltered)
            {
                user_name->setValue( LLSD() );
                fetchAvatarName();
            }
            else
            {
                // If the agent's chat was subject to @shownames=n we should display their anonimized name
                mFrom = chat.mFromName;
                user_name->setValue(mFrom);
                user_name->setToolTip(mFrom);
                setToolTip(mFrom);
                updateMinUserNameWidth();
            }
// [/RLVa:KB]
        }
        else if (chat.mChatStyle == CHAT_STYLE_HISTORY ||
                 mSourceType == CHAT_SOURCE_AGENT)
        {
            //if it's an avatar name with a username add formatting
            auto username_start = chat.mFromName.rfind(" (");
            auto username_end = chat.mFromName.rfind(')');

            if (username_start != std::string::npos &&
                username_end == (chat.mFromName.length() - 1))
            {
                mFrom = chat.mFromName.substr(0, username_start);
                user_name->setValue(mFrom);

                if (gSavedSettings.getBOOL("NameTagShowUsernames"))
                {
                    std::string username = chat.mFromName.substr(username_start + 2);
                    username = username.substr(0, username.length() - 1);
                    LLStyle::Params style_params_name;
                    LLUIColor userNameColor = LLUIColorTable::instance().getColor("EmphasisColor");
                    style_params_name.color(userNameColor);
                    style_params_name.font.name("SansSerifSmall");
                    style_params_name.font.style("NORMAL");
                    style_params_name.readonly_color(userNameColor);
                    user_name->appendText("  - " + username, false, style_params_name);
                }
            }
            else
            {
                mFrom = chat.mFromName;
                user_name->setValue(mFrom);
                updateMinUserNameWidth();
            }
        }
        else
        {
            // ...from an object, just use name as given
            mFrom = chat.mFromName;
            user_name->setValue(mFrom);
            updateMinUserNameWidth();
        }


        setTimeField(chat);

        // Set up the icon.
        LLAvatarIconCtrl* icon = getChild<LLAvatarIconCtrl>("avatar_icon");

        if(mSourceType != CHAT_SOURCE_AGENT ||  mAvatarID.isNull())
            icon->setDrawTooltip(false);

// [RLVa:KB] - Checked: 2010-04-22 (RLVa-1.2.2a) | Added: RLVa-1.2.0f
        // Don't show the context menu, info control or avatar icon tooltip if this chat was subject to @shownames=n
        if ( (chat.mRlvNamesFiltered) && ((CHAT_SOURCE_AGENT == mSourceType) || (CHAT_SOURCE_OBJECT == mSourceType))  )
        {
            mShowInfoCtrl = mShowContextMenu = false;
            icon->setDrawTooltip(false);
        }
// [/RLVa:KB]

        switch (mSourceType)
        {
            case CHAT_SOURCE_AGENT:
                icon->setValue(chat.mFromID);
                break;
            case CHAT_SOURCE_OBJECT:
                icon->setValue(LLSD("OBJECT_Icon"));
                break;
            case CHAT_SOURCE_SYSTEM:
            case CHAT_SOURCE_REGION:
                icon->setValue(LLSD("SL_Logo"));
                break;
            case CHAT_SOURCE_TELEPORT:
                icon->setValue(LLSD("Command_Destinations_Icon"));
                break;
            case CHAT_SOURCE_UNKNOWN:
                icon->setValue(LLSD("Unknown_Icon"));
        }

        // In case the message came from an object, save the object info
        // to be able properly show its profile.
        if ( chat.mSourceType == CHAT_SOURCE_OBJECT)
        {
            std::string slurl = args["slurl"].asString();
            if (slurl.empty() && LLWorld::instanceExists())
            {
                LLViewerRegion *region = LLWorld::getInstance()->getRegionFromPosAgent(chat.mPosAgent);
                if(region)
                {
                    LLSLURL region_slurl(region->getName(), chat.mPosAgent);
                    slurl = region_slurl.getLocationString();
                }
            }

            LLSD payload;
            payload["object_id"]    = chat.mFromID;
            payload["name"]         = chat.mFromName;
            payload["owner_id"]     = chat.mOwnerID;
            payload["slurl"]        = LLWeb::escapeURL(slurl);

            mObjectData = payload;
        }
    }

    /*virtual*/ void draw()
    {
        LLTextBox* user_name = mUserNameTextBox; //getChild<LLTextBox>("user_name");
        LLTextBox* time_box = mTimeBoxTextBox; //getChild<LLTextBox>("time_box");

        LLRect user_name_rect = user_name->getRect();
        S32 user_name_width = user_name_rect.getWidth();
        S32 time_box_width = time_box->getRect().getWidth();

        if (mNeedsTimeBox && !time_box->getVisible() && user_name_width > mMinUserNameWidth)
        {
            user_name_rect.mRight -= time_box_width;
            user_name->reshape(user_name_rect.getWidth(), user_name_rect.getHeight());
            user_name->setRect(user_name_rect);

            time_box->setVisible(true);
        }

        LLPanel::draw();
    }

    void updateMinUserNameWidth()
    {
        if (mUserNameFont)
        {
            LLTextBox* user_name = getChild<LLTextBox>("user_name");
            const LLWString& text = user_name->getWText();
            mMinUserNameWidth = mUserNameFont->getWidth(text.c_str()) + PADDING;
        }
    }

protected:
    static const S32 PADDING = 20;

    void showContextMenu(S32 x,S32 y)
    {
// [RLVa:KB] - Checked: 2010-04-22 (RLVa-1.2.2a) | Added: RLVa-1.2.0f
        if (!mShowContextMenu)
            return;
// [/RLVa:KB]
        if(mSourceType == CHAT_SOURCE_SYSTEM)
            showSystemContextMenu(x,y);
        if(mAvatarID.notNull() && mSourceType == CHAT_SOURCE_AGENT)
            showAvatarContextMenu(x,y);
        if(mAvatarID.notNull() && mSourceType == CHAT_SOURCE_OBJECT)
            showObjectContextMenu(x,y);
    }

    void showSystemContextMenu(S32 x,S32 y)
    {
    }

    void showObjectContextMenu(S32 x,S32 y)
    {
        LLMenuGL* menu = (LLMenuGL*)mPopupMenuHandleObject.get();
        if (!menu)
        {
            LLUICtrl::CommitCallbackRegistry::ScopedRegistrar registrar;
            LLUICtrl::EnableCallbackRegistry::ScopedRegistrar registrar_enable;
            registrar.add("ObjectIcon.Action", boost::bind(&LLChatHistoryHeader::onObjectIconContextMenuItemClicked, this, _2));
            registrar_enable.add("ObjectIcon.Visible", boost::bind(&LLChatHistoryHeader::onObjectIconContextMenuItemVisible, this, _2));

            menu = LLUICtrlFactory::getInstance()->createFromFile<LLMenuGL>("menu_object_icon.xml", gMenuHolder, LLViewerMenuHolderGL::child_registry_t::instance());
            if (menu)
            {
                mPopupMenuHandleObject = menu->getHandle();
                menu->updateParent(LLMenuGL::sMenuContainer);
                LLMenuGL::showPopup(this, menu, x, y);
            }
            else
            {
                LL_WARNS() << " Failed to create menu_object_icon.xml" << LL_ENDL;
            }
        }
        else
        {
            LLMenuGL::showPopup(this, menu, x, y);
        }
    }

    void showAvatarContextMenu(S32 x,S32 y)
    {
        LLMenuGL* menu = (LLMenuGL*)mPopupMenuHandleAvatar.get();
        if (!menu)
        {
            LLUICtrl::CommitCallbackRegistry::ScopedRegistrar registrar;
            LLUICtrl::EnableCallbackRegistry::ScopedRegistrar registrar_enable;
            registrar.add("AvatarIcon.Action", boost::bind(&LLChatHistoryHeader::onAvatarIconContextMenuItemClicked, this, _2));
            registrar_enable.add("AvatarIcon.Check", boost::bind(&LLChatHistoryHeader::onAvatarIconContextMenuItemChecked, this, _2));
            registrar_enable.add("AvatarIcon.Enable", boost::bind(&LLChatHistoryHeader::onAvatarIconContextMenuItemEnabled, this, _2));
            registrar_enable.add("AvatarIcon.Visible", boost::bind(&LLChatHistoryHeader::onAvatarIconContextMenuItemVisible, this, _2));

            menu = LLUICtrlFactory::getInstance()->createFromFile<LLMenuGL>("menu_avatar_icon.xml", gMenuHolder, LLViewerMenuHolderGL::child_registry_t::instance());
            if (menu)
            {
                mPopupMenuHandleAvatar = menu->getHandle();
            }
            else
            {
                LL_WARNS() << " Failed to create menu_avatar_icon.xml" << LL_ENDL;
            }
        }

        if(menu)
        {
            bool is_friend = LLAvatarActions::isFriend(mAvatarID);
            bool is_group_session = gAgent.isInGroup(mSessionID);

            menu->setItemEnabled("Add Friend", !is_friend);
            menu->setItemEnabled("Remove Friend", is_friend);
            menu->setItemVisible("Moderator Options Separator", is_group_session && isGroupModerator());
            menu->setItemVisible("Moderator Options", is_group_session && isGroupModerator());
            menu->setItemVisible("Group Ban Separator", is_group_session && canBanInGroup());
            menu->setItemVisible("BanMember", is_group_session && canBanInGroup());

            if(gAgentID == mAvatarID)
            {
                menu->setItemEnabled("Add Friend", false);
                menu->setItemEnabled("Send IM", false);
                menu->setItemEnabled("Remove Friend", false);
                menu->setItemEnabled("Offer Teleport",false);
                menu->setItemEnabled("Request Teleport",false);
                menu->setItemEnabled("Voice Call", false);
                menu->setItemEnabled("Chat History", false);
                menu->setItemEnabled("Invite Group", false);
                menu->setItemEnabled("Zoom In", true);
                menu->setItemEnabled("Share", false);
                menu->setItemEnabled("Pay", false);
                menu->setItemEnabled("Block Unblock", false);
                menu->setItemEnabled("Mute Text", false);
            }
            else
            {
                LLUUID currentSessionID = LLIMMgr::computeSessionID(IM_NOTHING_SPECIAL, mAvatarID);
                if (mSessionID == currentSessionID)
            {
                menu->setItemVisible("Send IM", false);
            }
                menu->setItemEnabled("Offer Teleport", LLAvatarActions::canOfferTeleport(mAvatarID));
                menu->setItemEnabled("Request Teleport", LLAvatarActions::canOfferTeleport(mAvatarID));
                menu->setItemEnabled("Voice Call", LLAvatarActions::canCall());

                // Only in nearby chat: this header's own session (null = nearby). LL
                // tested for ANY open IM session with the avatar, which hid Zoom In in
                // nearby chat for everyone you'd IM'd -- usually your friends.
                bool should_show_zoom = mSessionID.isNull();
                menu->setItemVisible("Zoom In", should_show_zoom && gObjectList.findObject(mAvatarID));
                menu->setItemEnabled("Block Unblock", LLAvatarActions::canBlock(mAvatarID));
                menu->setItemEnabled("Mute Text", LLAvatarActions::canBlock(mAvatarID));
                menu->setItemEnabled("Chat History", LLLogChat::isTranscriptExist(mAvatarID));
            }

            menu->setItemEnabled("Map", (LLAvatarTracker::instance().isBuddyOnline(mAvatarID) && LLAvatarActions::isAgentMappable(mAvatarID)) || gAgent.isGodlike() );
            menu->buildDrawLabels();
            menu->updateParent(LLMenuGL::sMenuContainer);
            LLMenuGL::showPopup(this, menu, x, y);
        }
    }

    void showInfoCtrl()
    {
//      const bool isVisible = !mAvatarID.isNull() && !mFrom.empty() && CHAT_SOURCE_SYSTEM != mSourceType && CHAT_SOURCE_REGION != mSourceType;
// [RLVa:KB] - Checked: 2010-04-22 (RLVa-1.2.2a) | Added: RLVa-1.2.0f
        const bool isVisible = mShowInfoCtrl && !mAvatarID.isNull() && !mFrom.empty() && CHAT_SOURCE_SYSTEM != mSourceType && CHAT_SOURCE_REGION != mSourceType;
// [/RLVa:KB]
        if (isVisible)
        {
            const LLRect sticky_rect = mUserNameTextBox->getRect();
            S32 icon_x = llmin(sticky_rect.mLeft + mUserNameTextBox->getTextBoundingRect().getWidth() + 7, sticky_rect.mRight - 3);
            mInfoCtrl->setOrigin(icon_x, sticky_rect.getCenterY() - mInfoCtrl->getRect().getHeight() / 2 ) ;
        }
        mInfoCtrl->setVisible(isVisible);
    }

    void hideInfoCtrl()
    {
        mInfoCtrl->setVisible(false);
    }

private:
    void setTimeField(const LLChat& chat)
    {
        LLTextBox* time_box = getChild<LLTextBox>("time_box");

        static LLUIColor timestamp_color = LLUIColorTable::instance().getColor("ChatHeaderTimestampColor"); // <alchemy/>
        time_box->setColor(timestamp_color); // <alchemy/>

        LLRect rect_before = time_box->getRect();

        time_box->setValue(chat.mTimeStr);

        // set necessary textbox width to fit all text
        time_box->reshapeToFitText();
        LLRect rect_after = time_box->getRect();

        // move rect to the left to correct position...
        S32 delta_pos_x = rect_before.getWidth() - rect_after.getWidth();
        S32 delta_pos_y = rect_before.getHeight() - rect_after.getHeight();
        time_box->translate(delta_pos_x, delta_pos_y);

        //... & change width of the name control
        LLView* user_name = getChild<LLView>("user_name");
        const LLRect& user_rect = user_name->getRect();
        user_name->reshape(user_rect.getWidth() + delta_pos_x, user_rect.getHeight());
    }

    void fetchAvatarName()
    {
        if (mAvatarID.notNull())
        {
            if (mAvatarNameCacheConnection.connected())
            {
                mAvatarNameCacheConnection.disconnect();
            }
            mAvatarNameCacheConnection = LLAvatarNameCache::get(mAvatarID,
                boost::bind(&LLChatHistoryHeader::onAvatarNameCache, this, _1, _2));
        }
    }

    void onAvatarNameCache(const LLUUID& agent_id, const LLAvatarName& av_name)
    {
        mAvatarNameCacheConnection.disconnect();

        mFrom = av_name.getDisplayName();

        LLTextBox* user_name = getChild<LLTextBox>("user_name");
        user_name->setValue( LLSD(av_name.getDisplayName() ) );
        user_name->setToolTip( av_name.getUserName() );

        if (gSavedSettings.getBOOL("NameTagShowUsernames") &&
            av_name.useDisplayNames() &&
            !av_name.isDisplayNameDefault())
        {
            LLStyle::Params style_params_name;
            LLUIColor userNameColor = LLUIColorTable::instance().getColor("EmphasisColor");
            style_params_name.color(userNameColor);
            style_params_name.font.name("SansSerifSmall");
            style_params_name.font.style("NORMAL");
            style_params_name.readonly_color(userNameColor);
            user_name->appendText("  - " + av_name.getUserName(), false, style_params_name);
        }
        setToolTip( av_name.getUserName() );
        // name might have changed, update width
        updateMinUserNameWidth();
    }

protected:
    LLHandle<LLView>    mPopupMenuHandleAvatar;
    LLHandle<LLView>    mPopupMenuHandleObject;

    LLUICtrl*           mInfoCtrl;

    LLUUID              mAvatarID;
    LLSD                mObjectData;
    EChatSourceType     mSourceType;
    std::string         mFrom;
    LLUUID              mSessionID;
    std::string         mText;
    F64                 mTime; // IM's frame time
    time_t              mCreationTime; // Views's time
// [RLVa:KB] - Checked: 2010-04-22 (RLVa-1.2.2a) | Added: RLVa-1.2.0f
    bool                mShowContextMenu;
    bool                mShowInfoCtrl;
// [/RLVa:KB]

    S32                 mMinUserNameWidth;
    const LLFontGL*     mUserNameFont;
    LLTextBox*          mUserNameTextBox;
    LLTextBox*          mTimeBoxTextBox;

    bool                mNeedsTimeBox;

    // #369: state as built, for resetForReuse()
    LLRect              mPristineRect;
    LLRect              mPristineUserNameRect;
    LLRect              mPristineTimeBoxRect;
    bool                mPristineTimeBoxVisible = true;
    LLColor4            mPristineBgAlphaColor;

private:
    boost::signals2::connection mAvatarNameCacheConnection;
};

LLChatHistory::LLChatHistory(const LLChatHistory::Params& p)
:   LLUICtrl(p),
    mMessageHeaderFilename(p.message_header),
    mMessageSeparatorFilename(p.message_separator),
    mLeftTextPad(p.left_text_pad),
    mRightTextPad(p.right_text_pad),
    mLeftWidgetPad(p.left_widget_pad),
    mRightWidgetPad(p.right_widget_pad),
    mTopSeparatorPad(p.top_separator_pad),
    mBottomSeparatorPad(p.bottom_separator_pad),
    mTopHeaderPad(p.top_header_pad),
    mBottomHeaderPad(p.bottom_header_pad),
    mIsLastMessageFromLog(false),
    mNotifyAboutUnreadMsg(p.notify_unread_msg)
{
    LLTextEditor::Params editor_params(p);
    editor_params.rect = getLocalRect();
    editor_params.follows.flags = FOLLOWS_ALL;
    editor_params.enabled = false; // read only
    editor_params.show_context_menu = "true";
    editor_params.trusted_content = false;
    editor_params.text_valign = LLFontGL::VAlign::VCENTER;
    editor_params.use_color = true;
    mEditor = LLUICtrlFactory::create<LLTextEditor>(editor_params, this);
    mEditor->setIsFriendCallback(LLAvatarActions::isFriend);
    mEditor->setIsObjectBlockedCallback(boost::bind(&LLMuteList::isMuted, LLMuteList::getInstance(), _1, _2, 0));

}

LLSD LLChatHistory::getValue() const
{
    return LLSD(mEditor->getText());
}

LLChatHistory::~LLChatHistory()
{
    ++mPageRequest; // log pages still being read find nothing to go to
    this->clear();
    llassert_msg(mPooledInUse.empty(), "pooled chat views still handed out at destruction");

    // the recycled headers and separators not in the document belong to no one else
    for (LLView* view : mFreeHeaders)
    {
        delete view;
    }
    for (LLView* view : mFreeSeparators)
    {
        delete view;
    }
    mFreeHeaders.clear();
    mFreeSeparators.clear();
}

void LLChatHistory::initFromParams(const LLChatHistory::Params& p)
{
    static LLUICachedControl<S32> scrollbar_size ("UIScrollbarSize", 0);

    LLRect stack_rect = getLocalRect();
    stack_rect.mRight -= scrollbar_size;
    LLLayoutStack::Params layout_p;
    layout_p.rect = stack_rect;
    layout_p.follows.flags = FOLLOWS_ALL;
    layout_p.orientation = LLLayoutStack::VERTICAL;
    layout_p.mouse_opaque = false;

    LLLayoutStack* stackp = LLUICtrlFactory::create<LLLayoutStack>(layout_p, this);

    const S32 NEW_TEXT_NOTICE_HEIGHT = 20;

    // #369: "loading older messages" line over the top of a windowed history
    {
        LLLayoutPanel::Params loading_p;
        loading_p.name = "loading_notice_holder";
        LLRect loading_rect = getLocalRect();
        loading_rect.mBottom = loading_rect.mTop - NEW_TEXT_NOTICE_HEIGHT;
        loading_p.rect = loading_rect;
        loading_p.background_opaque = true;
        loading_p.background_visible = true;
        loading_p.has_border = false;
        loading_p.mouse_opaque = false;
        loading_p.visible = false;
        loading_p.min_dim = 0;
        loading_p.auto_resize = false;
        loading_p.user_resize = false;
        mLoadingPanel = LLUICtrlFactory::create<LLLayoutPanel>(loading_p);

        LLTextBox::Params loading_text_p(p.more_chat_text);
        loading_text_p.rect = mLoadingPanel->getLocalRect();
        loading_text_p.follows.flags = FOLLOWS_ALL;
        loading_text_p.name = "loading_text";
        loading_text_p.mouse_opaque = false;
        loading_text_p.initial_value = LLTrans::getString("chat_history_loading_older");
        LLUICtrlFactory::create<LLTextBox>(loading_text_p, mLoadingPanel);

        stackp->addPanel(mLoadingPanel, LLLayoutStack::ANIMATE);
    }

    LLLayoutPanel::Params panel_p;
    panel_p.name = "spacer";
    panel_p.background_visible = false;
    panel_p.has_border = false;
    panel_p.mouse_opaque = false;
    panel_p.min_dim = 30;
    panel_p.auto_resize = true;
    panel_p.user_resize = false;

    stackp->addPanel(LLUICtrlFactory::create<LLLayoutPanel>(panel_p), LLLayoutStack::ANIMATE);

    panel_p.name = "new_text_notice_holder";
    LLRect new_text_notice_rect = getLocalRect();
    new_text_notice_rect.mTop = new_text_notice_rect.mBottom + NEW_TEXT_NOTICE_HEIGHT;
    panel_p.rect = new_text_notice_rect;
    panel_p.background_opaque = true;
    panel_p.background_visible = true;
    panel_p.visible = false;
    panel_p.min_dim = 0;
    panel_p.auto_resize = false;
    panel_p.user_resize = false;
    mMoreChatPanel = LLUICtrlFactory::create<LLLayoutPanel>(panel_p);

    LLTextBox::Params text_p(p.more_chat_text);
    text_p.rect = mMoreChatPanel->getLocalRect();
    text_p.follows.flags = FOLLOWS_ALL;
    text_p.name = "more_chat_text";
    mMoreChatText = LLUICtrlFactory::create<LLTextBox>(text_p, mMoreChatPanel);
    mMoreChatText->setClickedCallback(boost::bind(&LLChatHistory::onClickMoreText, this));

    stackp->addPanel(mMoreChatPanel, LLLayoutStack::ANIMATE);
}


/*void LLChatHistory::updateTextRect()
{
    static LLUICachedControl<S32> texteditor_border ("UITextEditorBorder", 0);

    LLRect old_text_rect = mVisibleTextRect;
    mVisibleTextRect = mScroller->getContentWindowRect();
    mVisibleTextRect.stretch(-texteditor_border);
    mVisibleTextRect.mLeft += mLeftTextPad;
    mVisibleTextRect.mRight -= mRightTextPad;
    if (mVisibleTextRect != old_text_rect)
    {
        needsReflow();
    }
}*/

LLView* LLChatHistory::getSeparator()
{
    LLPanel* separator = LLUICtrlFactory::getInstance()->createFromFile<LLPanel>(mMessageSeparatorFilename, NULL, LLPanel::child_registry_t::instance());
    return separator;
}

LLView* LLChatHistory::getHeader(const LLChat& chat,const LLStyle::Params& style_params, const LLSD& args)
{
    LL_PROFILE_ZONE_SCOPED;
    LLChatHistoryHeader* header = LLChatHistoryHeader::createInstance(mMessageHeaderFilename);
    if (header)
        header->setup(chat, style_params, args);
    return header;
}

void LLChatHistory::onClickMoreText()
{
    if (mSource && !mAtLiveEnd)
    {
        // the new messages are not in the window yet
        jumpToLiveEnd();
        return;
    }
    mEditor->endOfDoc();
}

void LLChatHistory::clear()
{
    mLastFromName.clear();
    if (!mWindow.empty())
    {
        clearWindow();
    }
    mEditor->clear();
    mLastFromID = LLUUID::null;
}

void LLChatHistory::noteUnread(const LLChat& chat)
{
    const bool from_me = chat.mFromID == gAgent.getID();
    if (!mNotifyAboutUnreadMsg || from_me || chat.mFromName.empty())
    {
        return;
    }
    mUnreadChatSources.insert(chat.mFromName);
    mMoreChatPanel->setVisible(true);
    std::string chatters;
    for (const std::string& source : mUnreadChatSources)
    {
        chatters += chatters.size() ? ", " + source : source;
    }
    LLStringUtil::format_map_t args;
    args["SOURCES"] = chatters;

    std::string xml_desc = mUnreadChatSources.size() == 1 ?
        "unread_chat_single" : "unread_chat_multiple";
    mMoreChatText->setValue(LLTrans::getString(xml_desc, args));
    S32 height = mMoreChatText->getTextPixelHeight() + 5;
    mMoreChatPanel->reshape(mMoreChatPanel->getRect().getWidth(), height);
}

static LLTrace::BlockTimerStatHandle FTM_APPEND_MESSAGE("Append Chat Message");

void LLChatHistory::appendMessage(const LLChat& chat, const LLSD &args, const LLStyle::Params& input_append_params)
{
    LL_RECORD_BLOCK_TIME(FTM_APPEND_MESSAGE);
    bool use_plain_text_chat_history = args["use_plain_text_chat_history"].asBoolean();
    bool square_brackets = false; // square brackets necessary for a system messages

    llassert(mEditor);
    if (!mEditor)
        return;

    bool from_me = chat.mFromID == gAgent.getID();
    mEditor->setPlainText(use_plain_text_chat_history);

    // #369: rendered by the window (paged in, or shown again) rather than arriving live
    RenderContext* window_ctx = mRenderCtx;
    const bool live_message = !window_ctx || window_ctx->mLive;

    if (live_message && !mEditor->scrolledToEnd())
    {
        noteUnread(chat);
    }

    F32 alpha = 1.f;
    LLUIColor txt_color = LLUIColorTable::instance().getColor("White");
    LLUIColor name_color(txt_color);
    LLViewerChat::getChatColor(chat, txt_color, alpha);

    LLFontGL* fontp = LLViewerChat::getChatFont();
    std::string font_name = LLFontGL::nameFromFont(fontp);
    std::string font_size = LLFontGL::sizeFromFont(fontp);

    LLStyle::Params body_message_params;
    body_message_params.color(txt_color);
    body_message_params.readonly_color(txt_color);
    body_message_params.alpha(alpha);
    body_message_params.font.name(font_name);
    body_message_params.font.size(font_size);
    body_message_params.font.style(input_append_params.font.style);

    LLStyle::Params name_params(body_message_params);
    name_params.color(name_color);
    name_params.readonly_color(name_color);

    std::string prefix = chat.mText.substr(0, 4);

    //IRC styled /me messages.
    bool irc_me = prefix == "/me " || prefix == "/me'";

    // Delimiter after a name in header copy/past and in plain text mode
    std::string delimiter = ": ";
    std::string shout = LLTrans::getString("shout");
    std::string whisper = LLTrans::getString("whisper");
    if (chat.mChatType == CHAT_TYPE_SHOUT ||
        chat.mChatType == CHAT_TYPE_WHISPER ||
        chat.mText.compare(0, shout.length(), shout) == 0 ||
        chat.mText.compare(0, whisper.length(), whisper) == 0)
    {
        delimiter = " ";
    }

    // Don't add any delimiter after name in irc styled messages
    if (irc_me || chat.mChatStyle == CHAT_STYLE_IRC)
    {
        delimiter = LLStringUtil::null;
        body_message_params.font.style = "ITALIC";
    }

    if (chat.mChatType == CHAT_TYPE_WHISPER)
    {
        body_message_params.font.style = "ITALIC";
    }
    else if (chat.mChatType == CHAT_TYPE_SHOUT)
    {
        body_message_params.font.style = "BOLD";
    }

    // <FS:HG> FS-1734 separate name and text styles for moderator (FSHighlightGroupMods)
    const bool moderator_style_active = chat.mChatStyle == CHAT_STYLE_MODERATOR;
    std::string moderator_name_style;
    if (moderator_style_active)
    {
        // FSModNameStyle / FSModTextStyle: 0 normal, bit 1 bold, bit 2 italic, bit 4 underline
        auto style_string = [](U32 style, bool italic)
        {
            std::string s;
            if (style & 1) s += "BOLD";
            if ((style & 2) || italic) s += "ITALIC";
            if (style & 4) s += "UNDERLINE";
            return s.empty() ? std::string("NORMAL") : s;
        };
        const bool italic = irc_me || chat.mChatStyle == CHAT_STYLE_IRC || chat.mChatType == CHAT_TYPE_WHISPER;
        moderator_name_style = style_string(gSavedSettings.getU32("FSModNameStyle"), italic);
        body_message_params.font.style = style_string(gSavedSettings.getU32("FSModTextStyle"), italic);
        name_params.font.style = moderator_name_style;
    }
    // </FS:HG>

    bool message_from_log = chat.mChatStyle == CHAT_STYLE_HISTORY;
    bool teleport_separator = chat.mSourceType == CHAT_SOURCE_TELEPORT;
    // We graying out chat history by graying out messages that contains full date in a time string
    if (message_from_log)
    {
        txt_color = LLColor4::grey;
        body_message_params.color(txt_color);
        body_message_params.readonly_color(txt_color);
        name_params.color(txt_color);
        name_params.readonly_color(txt_color);
    }

    // (the length, not getText(): that converts the whole document to UTF-8 per message, #369.
    // The window puts its own line break between plain text messages.)
    bool prependNewLineState = !window_ctx && mEditor->getLength() != 0;

    // #463: unnamed objects print the message alone (no name, no delimiter)
    const bool hide_sender_name = LLViewerChat::hideSenderName(chat);

    // compact mode: show a timestamp and name
    if (use_plain_text_chat_history)
    {
        square_brackets = chat.mSourceType == CHAT_SOURCE_SYSTEM && gSavedSettings.getBOOL("ChatSystemMessageBrackets");

        LLStyle::Params timestamp_style(body_message_params);

        // out of the timestamp
        if (args["show_time"].asBoolean() && !teleport_separator)
        {
            if (!message_from_log)
            {
                LLUIColor timestamp_color = LLUIColorTable::instance().getColor("ChatTimestampColor");
                timestamp_style.color(timestamp_color);
                timestamp_style.readonly_color(timestamp_color);
            }
            mEditor->appendText("[" + chat.mTimeStr + "] ", prependNewLineState, timestamp_style);
            prependNewLineState = false;
        }

        // out the opening square bracket (if need)
        if (square_brackets)
        {
            mEditor->appendText("[", prependNewLineState, body_message_params);
            prependNewLineState = false;
        }

        // names showing
        if (args["show_names_for_p2p_conv"].asBoolean() && utf8str_trim(chat.mFromName).size() && !hide_sender_name)
        {
            // Don't hotlink any messages from the system (e.g. "Second Life:"), so just add those in plain text.
            if (chat.mSourceType == CHAT_SOURCE_OBJECT && chat.mFromID.notNull())
            {
// [RLVa:KB] - Checked: 2010-04-22 (RLVa-1.2.0f) | Added: RLVa-1.2.0f
                // NOTE-RLVa: we don't need to do any @shownames or @showloc filtering here because we'll already have an existing URL
                std::string url = chat.mURL;
                RLV_ASSERT( (url.empty()) || (std::string::npos != url.find("objectim")) );
                if ( (url.empty()) || (std::string::npos == url.find("objectim")) )
                {
// [/RLVa:KB]
                    // for object IMs, create a secondlife:///app/objectim SLapp
                    /*std::string*/ url = LLViewerChat::getSenderSLURL(chat, args);
// [RLVa:KB] - Checked: 2010-04-22 (RLVa-1.2.0f) | Added: RLVa-1.2.0f
                }
// [/RLVa:KB]

                // set the link for the object name to be the objectim SLapp
                // (don't let object names with hyperlinks override our objectim Url)
                LLStyle::Params link_params(body_message_params);
                LLUIColor link_color = LLUIColorTable::instance().getColor("HTMLLinkColor");
                link_params.color = link_color;
                link_params.readonly_color = link_color;
                link_params.is_link = true;
                link_params.link_href = url;

                mEditor->appendText(chat.mFromName + delimiter, prependNewLineState, link_params);
                prependNewLineState = false;
            }
//          else if ( chat.mFromName != SYSTEM_FROM && chat.mFromID.notNull() && !message_from_log && chat.mSourceType != CHAT_SOURCE_REGION)
// [RLVa:KB] - Checked: 2010-04-22 (RLVa-1.2.0f) | Added: RLVa-1.2.0f
            else if ( chat.mFromName != SYSTEM_FROM && chat.mFromID.notNull() && !message_from_log && chat.mSourceType != CHAT_SOURCE_REGION && !chat.mRlvNamesFiltered)
// [/RLVa:KB]
            {
                LLStyle::Params link_params(body_message_params);
                link_params.overwriteFrom(LLStyleMap::instance().lookupAgent(chat.mFromID));
                // <FS:HG> moderator name style
                if (moderator_style_active)
                {
                    link_params.font.style = moderator_name_style;
                }
                // <FS:Ansariel> FIRE-30611: own name as "(You)" via the agentself Url (FSChatHistoryShowYou)
                static LLCachedControl<bool> show_you(gSavedSettings, "FSChatHistoryShowYou", false);
                if (from_me && show_you)
                {
                    std::string href = link_params.link_href;
                    LLStringUtil::replaceString(href, "/agent/", "/agentself/");
                    link_params.link_href = href;
                }
                // </FS:Ansariel>

                // Add link to avatar's inspector and delimiter to message.
                mEditor->appendText(std::string(link_params.link_href) + delimiter,
                    prependNewLineState, link_params);
                prependNewLineState = false;
            }
            else if (teleport_separator)
            {
                std::string tp_text = LLTrans::getString("teleport_preamble_compact_chat");
                mEditor->appendText(tp_text + " <nolink>" + chat.mFromName + "</nolink>",
                    prependNewLineState, body_message_params);
                                prependNewLineState = false;
            }
            else
            {
                mEditor->appendText("<nolink>" + chat.mFromName + "</nolink>" + delimiter,
                        prependNewLineState, body_message_params);
                prependNewLineState = false;
            }
        }
    }
    else // showing timestamp and name in the expanded mode
    {
        prependNewLineState = false;
        LLView* view = NULL;
        LLInlineViewSegment::Params p;
        p.force_newline = true;
        p.left_pad = mLeftWidgetPad;
        p.right_pad = mRightWidgetPad;

        LLDate new_message_time = LLDate::now();
        bool continuation = false;
        if (window_ctx)
        {
            // #369: the window decides from the message before it in the document, which is not
            // the last one appended when older messages are paged in above
            const WindowEntry* previous = window_ctx->mPrevious;
            continuation = !window_ctx->mForceHeader
                && previous
                && !teleport_separator
                && previous->mChat.mFromName == chat.mFromName
                && previous->mChat.mFromID == chat.mFromID
                && (new_message_time.secondsSinceEpoch() - previous->mShownAt) < 60.0
                && (previous->mChat.mChatStyle == CHAT_STYLE_HISTORY) == message_from_log;
        }
        else
        {
            continuation = !teleport_separator
                && mLastFromName == chat.mFromName
                && mLastFromID == chat.mFromID
                && mLastMessageTime.notNull()
                && (new_message_time.secondsSinceEpoch() - mLastMessageTime.secondsSinceEpoch()) < 60.0
                && mIsLastMessageFromLog == message_from_log;  //distinguish between current and previous chat session's histories
        }
        if (continuation)
        {
            view = window_ctx ? acquireSeparator() : getSeparator();
            if (!view)
            {
                // Might be wiser to make this LL_ERRS, getSeparator() should work in case of correct instalation.
                LL_WARNS() << "Failed to create separator from " << mMessageSeparatorFilename << ": can't append to history" << LL_ENDL;
                return;
            }

            p.top_pad = mTopSeparatorPad;
            p.bottom_pad = mBottomSeparatorPad;
        }
        else
        {
            view = window_ctx ? acquireHeader(chat, name_params, args) : getHeader(chat, name_params, args);
            if (!view)
            {
                LL_WARNS() << "Failed to create header from " << mMessageHeaderFilename << ": can't append to history" << LL_ENDL;
                return;
            }

            // (a window's first message moves down as older ones come in above: it keeps the pad)
            p.top_pad = (window_ctx || mEditor->getLength()) ? mTopHeaderPad : 0;
            p.bottom_pad = teleport_separator ? mBottomSeparatorPad : mBottomHeaderPad;
        }
        p.view = view;
        if (window_ctx && window_ctx->mEntry)
        {
            // recycled when the message leaves the window
            p.pooled_view = true;
            window_ctx->mEntry->mPooledView = view;
            window_ctx->mEntry->mIsSeparator = continuation;
        }

        //Prepare the rect for the view
        LLRect target_rect = mEditor->getDocumentView()->getRect();
        // squeeze down the widget by subtracting padding off left and right
        target_rect.mLeft += mLeftWidgetPad + mEditor->getHPad();
        target_rect.mRight -= mRightWidgetPad;
        view->reshape(target_rect.getWidth(), view->getRect().getHeight());
        view->setOrigin(target_rect.mLeft, view->getRect().mBottom);

        std::string widget_associated_text = "\n[" + chat.mTimeStr + "] ";
        if (utf8str_trim(chat.mFromName).size() != 0 && chat.mFromName != SYSTEM_FROM && !hide_sender_name)
            widget_associated_text += chat.mFromName + delimiter;

        mEditor->appendWidget(p, widget_associated_text, false);
        mLastFromName = chat.mFromName;
        mLastFromID = chat.mFromID;
        mLastMessageTime = new_message_time;
        mIsLastMessageFromLog = message_from_log;
    }

    // body of the message processing

    // notify processing
    if (chat.mNotifId.notNull())
    {
        LLNotificationPtr notification = LLNotificationsUtil::find(chat.mNotifId);
        if (notification != NULL)
        {
            bool create_toast = true;
            if (notification->getName() == "OfferFriendship")
            {
                // We don't want multiple friendship offers to appear, this code checks if there are previous offers
                // by iterating though all panels.
                // Note: it might be better to simply add a "pending offer" flag somewhere
                for (auto& panel : LLToastNotifyPanel::instance_snapshot())
                {
                    LLIMToastNotifyPanel * imtoastp = dynamic_cast<LLIMToastNotifyPanel *>(&panel);
                    const std::string& notification_name = panel.getNotificationName();
                    if (notification_name == "OfferFriendship"
                        && panel.isControlPanelEnabled()
                        && imtoastp)
                    {
                        create_toast = false;
                        break;
                    }
                }
            }

            if (create_toast)
            {
                LLIMToastNotifyPanel* notify_box = new LLIMToastNotifyPanel(
                        notification, chat.mSessionID, LLRect::null, !use_plain_text_chat_history, mEditor);

                //Prepare the rect for the view
                LLRect target_rect = mEditor->getDocumentView()->getRect();
                // squeeze down the widget by subtracting padding off left and right
                target_rect.mLeft += mLeftWidgetPad + mEditor->getHPad();
                target_rect.mRight -= mRightWidgetPad;
                notify_box->reshape(target_rect.getWidth(), notify_box->getRect().getHeight());
                notify_box->setOrigin(target_rect.mLeft, notify_box->getRect().mBottom);

                LLInlineViewSegment::Params params;
                params.view = notify_box;
                params.left_pad = mLeftWidgetPad;
                params.right_pad = mRightWidgetPad;
                mEditor->appendWidget(params, "\n", false);
            }
        }
    }
    // usual messages showing
    else if (!teleport_separator)
    {
        std::string message = irc_me ? chat.mText.substr(3) : chat.mText;

        //MESSAGE TEXT PROCESSING
        //*HACK getting rid of redundant sender names in system notifications sent using sender name (see EXT-5010)
        if (use_plain_text_chat_history && !from_me && chat.mFromID.notNull())
        {
            std::string slurl_about = SLURL_APP_AGENT + chat.mFromID.asString() + SLURL_ABOUT;
            if (message.length() > slurl_about.length() &&
                message.compare(0, slurl_about.length(), slurl_about) == 0)
            {
                message = message.substr(slurl_about.length(), message.length()-1);
            }
        }

        if (irc_me && !use_plain_text_chat_history)
        {
            std::string from_name = hide_sender_name ? std::string() : chat.mFromName;
            LLAvatarName av_name;
            if (!chat.mFromID.isNull() &&
                        LLAvatarNameCache::get(chat.mFromID, &av_name) &&
                        !av_name.isDisplayNameDefault())
            {
                from_name = av_name.getCompleteName();
            }
            message = from_name + message;
        }

        if (irc_me && hide_sender_name)
        {
            // "/me waves" from a nameless object reads "waves", not " waves"
            LLStringUtil::trimHead(message);
        }

        if (square_brackets)
        {
            message += "]";
        }

        mEditor->appendText(message, prependNewLineState, body_message_params);
        prependNewLineState = false;

        // #265: inline previews of image links, under the message (policy, safety
        // and load decision live in LLChatImagePreview / LLChatImageCache).
        LLRect column_rect = mEditor->getDocumentView()->getRect();
        column_rect.mLeft += mLeftWidgetPad + mEditor->getHPad();
        column_rect.mRight -= mRightWidgetPad;
        LLChatImagePreview::appendPreviews(mEditor, chat, message, mLeftWidgetPad, mRightWidgetPad,
                                           column_rect.getWidth());
    }

    mEditor->blockUndo();

    // automatically scroll to end when receiving chat from myself
    if (from_me && live_message)
    {
        mEditor->setCursorAndScrollToEnd();
    }
}

void LLChatHistory::draw()
{
    if (mSource)
    {
        updateWindowPaging();
    }

    if (mEditor->scrolledToEnd() && (!mSource || mAtLiveEnd))
    {
        mUnreadChatSources.clear();
        mMoreChatPanel->setVisible(false);
    }

    LLUICtrl::draw();
}

//
// #369: windowed history
//

static const S32 WINDOW_SLACK = 25;     // the window is trimmed in batches of at least this many
static const size_t MAX_FREE_VIEWS = 32;

// static
S32 LLChatHistory::windowMessages()
{
    static LLCachedControl<S32> window_messages(gSavedSettings, "SLChatWindowMessages", 250);
    return llclamp((S32)window_messages, 50, 2000);
}

// static
S32 LLChatHistory::pageMessages()
{
    static LLCachedControl<S32> page_messages(gSavedSettings, "SLChatHistoryPageMessages", 50);
    return llclamp((S32)page_messages, 10, 500);
}

void LLChatHistory::setWindowSource(LLChatHistorySource* source)
{
    if (mSource == source)
    {
        return;
    }
    ++mPageRequest;
    mPaging = PAGING_NONE;
    if (!source)
    {
        // the source is going away: keep what is shown, page nothing more
        mSource = nullptr;
        mLog.reset();
        showLoading(false);
        return;
    }
    clear();
    mSource = source;
    mNeedsFill = true;  // when first drawn: a hidden conversation reads nothing
}

LLView* LLChatHistory::acquireHeader(const LLChat& chat, const LLStyle::Params& style_params, const LLSD& args)
{
    LL_PROFILE_ZONE_SCOPED;
    LLChatHistoryHeader* header = nullptr;
    if (!mFreeHeaders.empty())
    {
        header = static_cast<LLChatHistoryHeader*>(mFreeHeaders.back());
        mFreeHeaders.pop_back();
        header->resetForReuse();
    }
    else
    {
        header = LLChatHistoryHeader::createInstance(mMessageHeaderFilename);
    }
    if (header)
    {
        const bool inserted = mPooledInUse.insert(header).second;
        llassert_msg(inserted, "chat header handed out twice");
        (void)inserted;
        header->setup(chat, style_params, args);
    }
    return header;
}

LLView* LLChatHistory::acquireSeparator()
{
    LLView* separator = nullptr;
    if (!mFreeSeparators.empty())
    {
        separator = mFreeSeparators.back();
        mFreeSeparators.pop_back();
    }
    else
    {
        separator = getSeparator();
    }
    if (separator)
    {
        const bool inserted = mPooledInUse.insert(separator).second;
        llassert_msg(inserted, "chat separator handed out twice");
        (void)inserted;
    }
    return separator;
}

void LLChatHistory::releaseView(LLView* view, bool separator)
{
    if (!view)
    {
        return;
    }
    // Exactly one release per acquire: a view released twice would sit in a free list twice
    // (two messages sharing one header), or in a free list after being destroyed.
    if (mPooledInUse.erase(view) == 0)
    {
        LL_WARNS("ChatHistory") << "Refusing to release a chat view that is not handed out (double release)" << LL_ENDL;
        llassert(false);
        return;
    }
    if (LLView* parent = view->getParent())
    {
        // the segment showing it unlinks it as it goes (clearWindow unlinks first): still in the
        // document means a segment that still shows it outlived its message
        LL_WARNS("ChatHistory") << "Chat view released while still in the document" << LL_ENDL;
        parent->removeChild(view);
    }
    if (!separator)
    {
        // nothing in the pool stays registered for avatar properties or name lookups
        static_cast<LLChatHistoryHeader*>(view)->resetForReuse();
    }
    std::vector<LLView*>& free_views = separator ? mFreeSeparators : mFreeHeaders;
    if (free_views.size() < MAX_FREE_VIEWS)
    {
        free_views.push_back(view);
    }
    else
    {
        // deferred like every other widget that leaves a text document (LLInlineViewSegment
        // die()s its view): this can run inside callbacks still using it
        view->die();
    }
}

void LLChatHistory::releaseEntry(WindowEntry& entry)
{
    releaseView(entry.mPooledView, entry.mIsSeparator);
    entry.mPooledView = nullptr;
    entry.mMarker = nullptr;
}

void LLChatHistory::clearWindow()
{
    // the recycled views leave the document before the segments holding them go
    std::vector<std::pair<LLView*, bool>> views;
    for (WindowEntry& entry : mWindow)
    {
        if (entry.mPooledView)
        {
            if (LLView* parent = entry.mPooledView->getParent())
            {
                parent->removeChild(entry.mPooledView);
            }
            views.emplace_back(entry.mPooledView, entry.mIsSeparator);
            entry.mPooledView = nullptr;
        }
    }
    mWindow.clear();
    mEditor->setInsertPos(-1);
    mEditor->clear();
    for (const auto& view : views)
    {
        releaseView(view.first, view.second);
    }
}

S32 LLChatHistory::entryStart(size_t index) const
{
    if (index >= mWindow.size())
    {
        return mEditor->getLength();
    }
    const WindowEntry& entry = mWindow[index];
    if (entry.mMarker.notNull())
    {
        return entry.mMarker->getStart();
    }
    // only a plain text message starting the document has no marker
    llassert(index == 0);
    return 0;
}

S32 LLChatHistory::entryEnd(size_t index) const
{
    return entryStart(index + 1);
}

bool LLChatHistory::makeLiveEntry(S32 index, WindowEntry& entry)
{
    entry.mFromLog = false;
    entry.mLiveIndex = index;
    LLSD args;
    if (!mSource->getLiveMessage(index, entry.mChat, args))
    {
        return false;
    }
    entry.mArgs = args;
    return true;
}

void LLChatHistory::makeLogEntry(const LLChatLogEntry& log, WindowEntry& entry)
{
    entry.mFromLog = true;
    entry.mLogBegin = log.mBegin;
    entry.mLogEnd = log.mEnd;
    mSource->chatFromLogEntry(log, entry.mChat);
}

bool LLChatHistory::renderEntry(WindowEntry& entry, const WindowEntry* previous, bool live, bool force_header)
{
    LLSD args = mSource->getAppendArgs();
    for (LLSD::map_const_iterator it = entry.mArgs.beginMap(); it != entry.mArgs.endMap(); ++it)
    {
        args[it->first] = it->second;
    }
    const bool plain = args["use_plain_text_chat_history"].asBoolean();

    const S32 start = mEditor->getInsertPos() >= 0 ? mEditor->getInsertPos() : mEditor->getLength();
    const bool starts_document = start == 0;
    if (plain && !starts_document)
    {
        // plain text messages are separated by a line break, which belongs to the message after
        // it: it marks where that message starts
        LLFontGL* fontp = LLViewerChat::getChatFont();
        LLStyle::Params line_break_style;
        line_break_style.font.name(LLFontGL::nameFromFont(fontp));
        line_break_style.font.size(LLFontGL::sizeFromFont(fontp));
        mEditor->appendLineBreakSegment(line_break_style);
    }
    const S32 body_start = mEditor->getInsertPos() >= 0 ? mEditor->getInsertPos() : mEditor->getLength();

    RenderContext ctx;
    ctx.mPrevious = previous;
    ctx.mLive = live;
    ctx.mForceHeader = force_header;
    ctx.mEntry = &entry;
    // an entry is rendered fresh or after releaseEntry(): it holds no view of its own here
    llassert_msg(!entry.mPooledView, "chat entry rendered while holding a pooled view");
    llassert(!mRenderCtx);
    entry.mPooledView = nullptr;
    entry.mIsSeparator = false;
    entry.mMarker = nullptr;

    mRenderCtx = &ctx;
    appendMessage(entry.mChat, args, LLStyle::Params());
    mRenderCtx = nullptr;

    const S32 end = mEditor->getInsertPos() >= 0 ? mEditor->getInsertPos() : mEditor->getLength();
    if (end <= body_start)
    {
        // nothing shown (a muted line, a header that failed to build): take the line break back
        if (end > start)
        {
            mEditor->removeTextRange(start, end - start);
        }
        releaseEntry(entry);
        return false;
    }
    if (!plain || !starts_document)
    {
        entry.mMarker = mEditor->getSegmentStartingAt(start);
    }
    entry.mShownAt = LLDate::now().secondsSinceEpoch();
    return true;
}

LLChatHistory::WindowAnchor LLChatHistory::captureAnchor()
{
    WindowAnchor anchor;
    anchor.mScroll = mEditor->captureScrollAnchor();
    if (!anchor.mScroll.mValid)
    {
        return anchor;
    }
    for (S32 i = (S32)mWindow.size() - 1; i >= 0; --i)
    {
        const S32 start = entryStart(i);
        if (start <= anchor.mScroll.mDocIndex)
        {
            anchor.mEntry = i;
            anchor.mOffset = anchor.mScroll.mDocIndex - start;
            break;
        }
    }
    return anchor;
}

void LLChatHistory::restoreAnchor(const WindowAnchor& anchor, S32 entry_shift)
{
    if (!anchor.mScroll.mValid)
    {
        return;
    }
    S32 index = 0;
    const S32 entry = anchor.mEntry + entry_shift;
    if (anchor.mEntry >= 0 && entry >= 0 && entry < (S32)mWindow.size())
    {
        index = llmin(entryStart(entry) + anchor.mOffset, entryEnd(entry));
    }
    mEditor->restoreScrollAnchor(anchor.mScroll, index);
}

void LLChatHistory::appendEntries(std::vector<WindowEntry>& entries, bool live)
{
    if (entries.empty())
    {
        return;
    }
    S32 appended = 0;
    for (WindowEntry& entry : entries)
    {
        const WindowEntry* previous = mWindow.empty() ? nullptr : &mWindow.back();
        if (renderEntry(entry, previous, live, false))
        {
            mWindow.push_back(std::move(entry));
            ++appended;
        }
    }

    const S32 max_messages = windowMessages();
    if ((S32)mWindow.size() <= max_messages + WINDOW_SLACK)
    {
        return;
    }
    const S32 excess = (S32)mWindow.size() - max_messages;
    WindowAnchor anchor = captureAnchor();
    if (!anchor.mScroll.mAtBottom && anchor.mEntry >= 0 && anchor.mEntry < excess)
    {
        // what is being read would go: the newest messages leave instead, and wait below
        if (live)
        {
            for (S32 i = llmax((S32)mWindow.size() - appended, 0); i < (S32)mWindow.size(); ++i)
            {
                noteUnread(mWindow[i].mChat);
            }
        }
        trimBottom(excess);
        return;
    }
    trimTop(excess);
    restoreAnchor(anchor, -excess);
}

void LLChatHistory::prependEntries(std::vector<WindowEntry>& entries)
{
    if (entries.empty())
    {
        return;
    }
    WindowAnchor anchor = captureAnchor();
    const bool plain = mSource->getAppendArgs()["use_plain_text_chat_history"].asBoolean();

    std::deque<WindowEntry> rendered;
    mEditor->setInsertPos(0);
    for (WindowEntry& entry : entries)
    {
        const WindowEntry* previous = rendered.empty() ? nullptr : &rendered.back();
        if (renderEntry(entry, previous, false, false))
        {
            rendered.push_back(std::move(entry));
        }
    }
    if (plain && !rendered.empty() && !mWindow.empty())
    {
        // the message that started the document now follows: it gets its line break
        LLFontGL* fontp = LLViewerChat::getChatFont();
        LLStyle::Params line_break_style;
        line_break_style.font.name(LLFontGL::nameFromFont(fontp));
        line_break_style.font.size(LLFontGL::sizeFromFont(fontp));
        const S32 pos = mEditor->getInsertPos();
        mEditor->appendLineBreakSegment(line_break_style);
        mWindow.front().mMarker = mEditor->getSegmentStartingAt(pos);
    }
    mEditor->setInsertPos(-1);

    const S32 added = (S32)rendered.size();
    for (auto it = rendered.rbegin(); it != rendered.rend(); ++it)
    {
        mWindow.push_front(std::move(*it));
    }
    restoreAnchor(anchor, added);

    // the window keeps its size: as many leave at the bottom
    const S32 max_messages = windowMessages();
    if ((S32)mWindow.size() > max_messages + WINDOW_SLACK)
    {
        WindowAnchor bottom_anchor = captureAnchor();
        trimBottom((S32)mWindow.size() - max_messages);
        restoreAnchor(bottom_anchor, 0);
    }
}

void LLChatHistory::trimTop(S32 count)
{
    count = llmin(count, (S32)mWindow.size());
    if (count <= 0)
    {
        return;
    }
    S32 remove_end = entryStart(count);
    if (count < (S32)mWindow.size())
    {
        WindowEntry& next = mWindow[count];
        if (next.mMarker.notNull() && !next.mPooledView)
        {
            // a plain text message's line break goes when it starts the document
            remove_end = next.mMarker->getEnd();
            next.mMarker = nullptr;
        }
    }

    std::vector<WindowEntry> removed;
    removed.reserve(count);
    for (S32 i = 0; i < count; ++i)
    {
        removed.push_back(std::move(mWindow.front()));
        mWindow.pop_front();
    }
    mEditor->removeTextRange(0, remove_end);
    for (WindowEntry& entry : removed)
    {
        releaseEntry(entry);
    }
    mAtLogStart = false;

    if (!mWindow.empty() && mWindow.front().mIsSeparator)
    {
        reheadFront();
    }
}

void LLChatHistory::reheadFront()
{
    // a continuation that now starts the window shows who said it
    WindowEntry entry = std::move(mWindow.front());
    mWindow.pop_front();
    const S32 end = entryStart(0);
    mEditor->removeTextRange(0, end);
    releaseEntry(entry);

    mEditor->setInsertPos(0);
    const bool shown = renderEntry(entry, nullptr, false, true);
    mEditor->setInsertPos(-1);
    if (shown)
    {
        mWindow.push_front(std::move(entry));
    }
}

void LLChatHistory::trimBottom(S32 count)
{
    count = llmin(count, (S32)mWindow.size());
    if (count <= 0)
    {
        return;
    }
    const S32 start = entryStart(mWindow.size() - count);
    std::vector<WindowEntry> removed;
    removed.reserve(count);
    for (S32 i = 0; i < count; ++i)
    {
        removed.push_back(std::move(mWindow.back()));
        mWindow.pop_back();
    }
    mEditor->removeTextRange(start, mEditor->getLength() - start);
    for (WindowEntry& entry : removed)
    {
        releaseEntry(entry);
    }
    mAtLiveEnd = false;
}

void LLChatHistory::showLoading(bool loading)
{
    if (mLoadingPanel)
    {
        mLoadingPanel->setVisible(loading);
    }
}

void LLChatHistory::fillFromLiveEnd()
{
    mNeedsFill = false;
    ++mPageRequest;
    mPaging = PAGING_NONE;
    showLoading(false);
    clearWindow();

    mLog = mSource->getHistoryLog();
    const S32 count = mSource->getLiveMessageCount();
    const S32 first = llmax(0, count - windowMessages());
    std::vector<WindowEntry> entries;
    entries.reserve(count - first);
    for (S32 i = first; i < count; ++i)
    {
        WindowEntry entry;
        if (makeLiveEntry(i, entry))
        {
            entries.push_back(std::move(entry));
        }
    }
    mAtLiveEnd = true;
    mLiveSeen = count - 1;
    // older live messages, or the log, come in as the top is scrolled to (right away when
    // the window does not fill the view)
    mAtLogStart = first == 0 && !mLog;
    appendEntries(entries, false);
    mEditor->endOfDoc();
}

void LLChatHistory::jumpToLiveEnd()
{
    if (!mSource)
    {
        mEditor->endOfDoc();
        return;
    }
    fillFromLiveEnd();
    mUnreadChatSources.clear();
    mMoreChatPanel->setVisible(false);
}

void LLChatHistory::onLiveMessagesAdded()
{
    if (!mSource || mNeedsFill)
    {
        return;
    }
    const S32 count = mSource->getLiveMessageCount();
    if (count - 1 < mLiveSeen)
    {
        // the model was rebuilt
        fillFromLiveEnd();
        return;
    }
    if (!mAtLiveEnd)
    {
        // the window is back in the conversation: the new messages wait below it
        for (S32 i = mLiveSeen + 1; i < count; ++i)
        {
            WindowEntry entry;
            if (!makeLiveEntry(i, entry))
            {
                continue;
            }
            if (entry.mChat.mFromID == gAgent.getID())
            {
                // what we say ourselves is shown, as appendMessage() scrolls to it
                jumpToLiveEnd();
                return;
            }
            noteUnread(entry.mChat);
        }
        mLiveSeen = count - 1;
        return;
    }

    std::vector<WindowEntry> entries;
    for (S32 i = mLiveSeen + 1; i < count; ++i)
    {
        WindowEntry entry;
        if (makeLiveEntry(i, entry))
        {
            entries.push_back(std::move(entry));
        }
    }
    mLiveSeen = count - 1;
    appendEntries(entries, true);
}

void LLChatHistory::reloadWindow(bool reset)
{
    if (!mSource)
    {
        return;
    }
    if (mNeedsFill)
    {
        return; // filled when first drawn
    }
    if (reset || mWindow.empty())
    {
        fillFromLiveEnd();
        return;
    }

    // the same messages, shown again (timestamps, plain text, fonts...)
    const bool at_bottom = mEditor->scrolledToEnd();
    WindowAnchor anchor = captureAnchor();
    std::vector<WindowEntry> entries;
    entries.reserve(mWindow.size());
    for (const WindowEntry& shown : mWindow)
    {
        WindowEntry entry;
        if (shown.mFromLog)
        {
            entry.mFromLog = true;
            entry.mLogBegin = shown.mLogBegin;
            entry.mLogEnd = shown.mLogEnd;
            entry.mChat = shown.mChat;
            entries.push_back(std::move(entry));
        }
        else if (makeLiveEntry(shown.mLiveIndex, entry))
        {
            entries.push_back(std::move(entry));
        }
    }
    ++mPageRequest;
    mPaging = PAGING_NONE;
    showLoading(false);
    const bool at_live_end = mAtLiveEnd;
    const bool at_log_start = mAtLogStart;
    clearWindow();
    appendEntries(entries, false);
    mAtLiveEnd = at_live_end;
    mAtLogStart = at_log_start;
    if (at_bottom)
    {
        mEditor->endOfDoc();
    }
    else
    {
        restoreAnchor(anchor, 0);
    }
}

void LLChatHistory::appendLiveFrom(S32 from, S32 count)
{
    const S32 total = mSource->getLiveMessageCount();
    const S32 to = llmin(total, from + count);
    std::vector<WindowEntry> entries;
    for (S32 i = llmax(0, from); i < to; ++i)
    {
        WindowEntry entry;
        if (makeLiveEntry(i, entry))
        {
            entries.push_back(std::move(entry));
        }
    }
    if (to >= total)
    {
        // back at the live end (unless the append has to leave it again)
        mAtLiveEnd = true;
        mLiveSeen = total - 1;
    }
    appendEntries(entries, false);
}

void LLChatHistory::requestOlder(S32 count)
{
    if (mPaging != PAGING_NONE || mAtLogStart)
    {
        return;
    }

    // older live messages are in memory
    if (!mWindow.empty() && !mWindow.front().mFromLog && mWindow.front().mLiveIndex > 0)
    {
        std::vector<WindowEntry> entries;
        S32 from = mWindow.front().mLiveIndex;
        while (from > 0 && (S32)entries.size() < count)
        {
            const S32 batch_from = llmax(0, from - count);
            std::vector<WindowEntry> batch;
            for (S32 i = batch_from; i < from; ++i)
            {
                WindowEntry entry;
                if (makeLiveEntry(i, entry))
                {
                    batch.push_back(std::move(entry));
                }
            }
            entries.insert(entries.begin(), std::make_move_iterator(batch.begin()), std::make_move_iterator(batch.end()));
            from = batch_from;
        }
        if (!entries.empty())
        {
            prependEntries(entries);
            return;
        }
        // only messages that are not shown before the window: the log is next
    }

    if (!mLog)
    {
        mAtLogStart = true;
        return;
    }
    const LLChatLogCursor before = (!mWindow.empty() && mWindow.front().mFromLog)
        ? mWindow.front().mLogBegin : LLChatLogCursor::end();
    mPaging = PAGING_OLDER;
    const U32 request = ++mPageRequest;
    showLoading(true);
    LLHandle<LLView> handle = getHandle();
    mLog->loadPage(before, count, [handle, request](const LLChatLogPage& page)
        {
            LLChatHistory* self = dynamic_cast<LLChatHistory*>(handle.get());
            if (self && self->mSource && self->mPageRequest == request)
            {
                self->onLogPage(page, true);
            }
        });
}

void LLChatHistory::requestNewer(S32 count)
{
    if (mPaging != PAGING_NONE || mAtLiveEnd)
    {
        return;
    }
    if (mWindow.empty())
    {
        jumpToLiveEnd();
        return;
    }
    const WindowEntry& last = mWindow.back();
    if (!last.mFromLog)
    {
        appendLiveFrom(last.mLiveIndex + 1, count);
        return;
    }
    if (!mLog)
    {
        appendLiveFrom(0, count);
        return;
    }
    mPaging = PAGING_NEWER;
    const U32 request = ++mPageRequest;
    LLHandle<LLView> handle = getHandle();
    mLog->loadPageAfter(last.mLogEnd, count, [handle, request](const LLChatLogPage& page)
        {
            LLChatHistory* self = dynamic_cast<LLChatHistory*>(handle.get());
            if (self && self->mSource && self->mPageRequest == request)
            {
                self->onLogPage(page, false);
            }
        });
}

void LLChatHistory::onLogPage(const LLChatLogPage& page, bool older)
{
    mPaging = PAGING_NONE;
    showLoading(false);

    std::vector<WindowEntry> entries;
    entries.reserve(page.mMessages.size());
    for (const LLChatLogEntry& log : page.mMessages)
    {
        WindowEntry entry;
        makeLogEntry(log, entry);
        entries.push_back(std::move(entry));
    }

    if (older)
    {
        mAtLogStart = page.mAtStart || page.mMessages.empty();
        prependEntries(entries);
        return;
    }

    appendEntries(entries, false);
    if (page.mAtEnd)
    {
        // the log is done: the live session follows it
        appendLiveFrom(0, llmax(1, pageMessages() - (S32)page.mMessages.size()));
    }
}

void LLChatHistory::updateWindowPaging()
{
    if (mNeedsFill)
    {
        fillFromLiveEnd();
        return;
    }
    if (mPaging != PAGING_NONE || !mEditor->getScrollContainer())
    {
        return;
    }
    // not while the user drags the scrollbar or selects text: the layout would move under them
    LLView* captor = dynamic_cast<LLView*>(gFocusMgr.getMouseCapture());
    if (captor && (captor == this || captor->hasAncestor(this)))
    {
        return;
    }
    // The decision below reads the document's geometry: it has to be the document as it is now
    // (the window was just cleared, refilled or trimmed), not as it was laid out last frame.
    mEditor->updateLayoutNow();
    const LLRect visible = mEditor->getVisibleDocumentRect();
    const S32 doc_height = mEditor->getDocumentView()->getRect().getHeight();
    const S32 margin = llmax(visible.getHeight(), 1);
    if (!mAtLogStart && doc_height - visible.mTop < margin)
    {
        // near the top (or the window does not fill the view)
        requestOlder(pageMessages());
    }
    else if (!mAtLiveEnd && visible.mBottom < margin)
    {
        requestNewer(pageMessages());
    }
}

//
// #369: find in chat over the whole conversation
//

bool LLChatHistory::liveMatches(S32 index, const LLWString& needle, bool case_insensitive)
{
    WindowEntry entry;
    if (!makeLiveEntry(index, entry))
    {
        return false;
    }
    LLWString hay = utf8str_to_wstring(entry.mChat.mFromName + ": " + entry.mChat.mText);
    if (case_insensitive)
    {
        LLWStringUtil::toLower(hay);
    }
    return hay.find(needle) != LLWString::npos;
}

void LLChatHistory::findNext(const std::string& text, bool case_insensitive, bool search_up)
{
    if (text.empty())
    {
        return;
    }
    if (!mSource)
    {
        mEditor->selectNext(text, case_insensitive, true, search_up);
        return;
    }
    // in the window first
    if (mEditor->selectNext(text, case_insensitive, false, search_up, true))
    {
        return;
    }
    if (mPaging != PAGING_NONE)
    {
        return; // the window is moving already
    }

    LLWString needle = utf8str_to_wstring(text);
    if (case_insensitive)
    {
        LLWStringUtil::toLower(needle);
    }
    const S32 live_count = mSource->getLiveMessageCount();
    if (search_up)
    {
        // older: live messages before the window, then the log
        const bool front_is_live = !mWindow.empty() && !mWindow.front().mFromLog;
        const S32 live_before = mWindow.empty() ? live_count : (front_is_live ? mWindow.front().mLiveIndex : 0);
        for (S32 i = live_before - 1; i >= 0; --i)
        {
            if (liveMatches(i, needle, case_insensitive))
            {
                showLiveAt(i, text, case_insensitive, search_up);
                return;
            }
        }
        if (mLog)
        {
            const LLChatLogCursor from = (!mWindow.empty() && mWindow.front().mFromLog)
                ? mWindow.front().mLogBegin : LLChatLogCursor::end();
            findInLog(from, text, case_insensitive, search_up);
            return;
        }
    }
    else
    {
        // newer: the rest of the log, then live messages after the window
        if (!mWindow.empty() && mWindow.back().mFromLog && mLog)
        {
            findInLog(mWindow.back().mLogEnd, text, case_insensitive, search_up);
            return;
        }
        const S32 live_after = mWindow.empty() ? 0 : (mWindow.back().mFromLog ? 0 : mWindow.back().mLiveIndex + 1);
        for (S32 i = live_after; i < live_count; ++i)
        {
            if (liveMatches(i, needle, case_insensitive))
            {
                showLiveAt(i, text, case_insensitive, search_up);
                return;
            }
        }
    }
    // nothing beyond the window: around inside it, as before
    mEditor->selectNext(text, case_insensitive, true, search_up);
}

void LLChatHistory::findInLog(const LLChatLogCursor& from, const std::string& text, bool case_insensitive, bool search_up)
{
    mPaging = PAGING_JUMP;
    const U32 request = ++mPageRequest;
    LLHandle<LLView> handle = getHandle();
    mLog->find(text, case_insensitive, search_up, from,
        [handle, request, text, case_insensitive, search_up](bool found, const LLChatLogCursor& where)
        {
            LLChatHistory* self = dynamic_cast<LLChatHistory*>(handle.get());
            if (!self || !self->mSource || self->mPageRequest != request)
            {
                return;
            }
            self->mPaging = PAGING_NONE;
            if (found)
            {
                self->jumpToLog(where, text, case_insensitive, search_up);
                return;
            }
            if (!search_up)
            {
                // past the end of the log: the live session
                LLWString needle = utf8str_to_wstring(text);
                if (case_insensitive)
                {
                    LLWStringUtil::toLower(needle);
                }
                const S32 live_count = self->mSource->getLiveMessageCount();
                for (S32 i = 0; i < live_count; ++i)
                {
                    if (self->liveMatches(i, needle, case_insensitive))
                    {
                        self->showLiveAt(i, text, case_insensitive, search_up);
                        return;
                    }
                }
            }
            self->mEditor->selectNext(text, case_insensitive, true, search_up);
        });
}

void LLChatHistory::jumpToLog(const LLChatLogCursor& where, const std::string& text, bool case_insensitive, bool search_up)
{
    mPaging = PAGING_JUMP;
    const U32 request = ++mPageRequest;
    showLoading(true);
    LLHandle<LLView> handle = getHandle();
    const S32 page = pageMessages();
    // a page before the match, then the match and what follows it
    mLog->loadPage(where, page, [handle, request, where, page, text, case_insensitive, search_up](const LLChatLogPage& before)
        {
            LLChatHistory* self = dynamic_cast<LLChatHistory*>(handle.get());
            if (!self || !self->mSource || self->mPageRequest != request || !self->mLog)
            {
                return;
            }
            self->mLog->loadPageAfter(where, page,
                [handle, request, where, before, text, case_insensitive, search_up](const LLChatLogPage& after)
                {
                    LLChatHistory* history = dynamic_cast<LLChatHistory*>(handle.get());
                    if (!history || !history->mSource || history->mPageRequest != request)
                    {
                        return;
                    }
                    history->mPaging = PAGING_NONE;
                    history->showLoading(false);
                    history->clearWindow();

                    std::vector<WindowEntry> entries;
                    for (const LLChatLogPage* log_page : { &before, &after })
                    {
                        for (const LLChatLogEntry& log : log_page->mMessages)
                        {
                            WindowEntry entry;
                            history->makeLogEntry(log, entry);
                            entries.push_back(std::move(entry));
                        }
                    }
                    history->mAtLiveEnd = false;
                    history->mAtLogStart = before.mAtStart;
                    history->mLiveSeen = history->mSource->getLiveMessageCount() - 1;
                    history->appendEntries(entries, false);
                    for (S32 i = 0; i < (S32)history->mWindow.size(); ++i)
                    {
                        if (history->mWindow[i].mFromLog && history->mWindow[i].mLogBegin == where)
                        {
                            history->selectInEntry(i, text, case_insensitive, search_up);
                            break;
                        }
                    }
                });
        });
}

void LLChatHistory::showLiveAt(S32 index, const std::string& text, bool case_insensitive, bool search_up)
{
    ++mPageRequest;
    mPaging = PAGING_NONE;
    showLoading(false);
    clearWindow();

    const S32 count = mSource->getLiveMessageCount();
    const S32 window = windowMessages();
    const S32 from = llclamp(index - window / 2, 0, llmax(0, count - window));
    const S32 to = llmin(count, from + window);
    std::vector<WindowEntry> entries;
    for (S32 i = from; i < to; ++i)
    {
        WindowEntry entry;
        if (makeLiveEntry(i, entry))
        {
            entries.push_back(std::move(entry));
        }
    }
    mAtLiveEnd = to >= count;
    mLiveSeen = count - 1;
    mAtLogStart = from == 0 && !mLog;
    appendEntries(entries, false);
    for (S32 i = 0; i < (S32)mWindow.size(); ++i)
    {
        if (!mWindow[i].mFromLog && mWindow[i].mLiveIndex >= index)
        {
            selectInEntry(i, text, case_insensitive, search_up);
            break;
        }
    }
}

void LLChatHistory::selectInEntry(S32 index, const std::string& text, bool case_insensitive, bool search_up)
{
    // lay the new window out first: selecting scrolls to the selection
    mEditor->updateLayoutNow();
    mEditor->deselect();
    mEditor->setCursorPos(search_up ? entryEnd(index) : entryStart(index));
    mEditor->selectNext(text, case_insensitive, false, search_up);
}
