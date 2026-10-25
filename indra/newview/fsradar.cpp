/**
 * @file fsradar.cpp
 * @brief Firestorm radar implementation
 *
 * $LicenseInfo:firstyear=2013&license=viewerlgpl$
 * Copyright (c) 2013 Ansariel Hiller @ Second Life
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

#include "fsradar.h"
#include "fskeywords.h"

// libs
#include "llavatarnamecache.h"
#include "llanimationstates.h"
#include "llcommonutils.h"
#include "llnotificationsutil.h"
#include "lleventtimer.h"

// newview
#include "slavataractions.h"
#include "lggcontactsets.h"
#include "llappviewer.h"         // gFrameTimeSeconds
#include "llchat.h"
#include "llui.h"                // make_ui_sound
#include "fscommon.h"
#include "llagent.h"
#include "llavataractions.h"
#include "llcallingcard.h"
#include "llfontgl.h"
#include "llmutelist.h"
#include "llnotificationmanager.h"
#include "llslurl.h"
#include "lltracker.h"
#include "lltrans.h"
#include "lluicolortable.h"
#include "llviewercontrol.h"        // for gSavedSettings
#include "llviewermessage.h"
#include "llviewerobjectlist.h"
#include "llviewerparcelmgr.h"
#include "llvoavatar.h"
#include "llvoiceclient.h"
#include "llworld.h"
#include "llspeakers.h"
#include "llviewerregion.h"
#include "message.h"
#include "rlvactions.h"
#include "rlvcommon.h"
#include "rlvhandler.h"

namespace
{
    // <FS:PP> FIRE-6069: Radar alerts sounds. Our make_ui_sound has no PlayMode gate, so the
    // FS "PlayModeUISndRadar*" switches are honoured here (all default to off, as in FS).
    void play_radar_sound(const char* sound_name)
    {
        const std::string play_mode_setting = std::string("PlayMode") + sound_name;
        if (gSavedSettings.controlExists(play_mode_setting) && gSavedSettings.getBOOL(play_mode_setting))
        {
            make_ui_sound(sound_name);
        }
    }

    // Centralized voice power level (FS keeps this in LLVoiceClient)
    const char* voice_level_icon(const LLUUID& id)
    {
        LLVoiceClient* voice_client = LLVoiceClient::getInstance();
        if (voice_client->getOnMuteList(id))
        {
            return nullptr;
        }

        const F32 power = voice_client->getCurrentPower(id);
        if (power == 0.f && !voice_client->getIsSpeaking(id))
        {
            return "VoicePTT_Off";
        }
        else if (power < LLVoiceClient::OVERDRIVEN_POWER_LEVEL / 3.f)
        {
            return "VoicePTT_On";
        }
        else if (power < LLVoiceClient::OVERDRIVEN_POWER_LEVEL * 2.f / 3.f)
        {
            return "VoicePTT_Lvl1";
        }
        else if (power < LLVoiceClient::OVERDRIVEN_POWER_LEVEL)
        {
            return "VoicePTT_Lvl2";
        }
        return "VoicePTT_Lvl3";
    }
}

/**
 * Periodically updates the nearby people list while the Nearby tab is active.
 *
 * The period is defined by FS_RADAR_LIST_UPDATE_INTERVAL constant.
 */

constexpr F32 FS_RADAR_LIST_UPDATE_INTERVAL = 1.f;

class FSRadarListUpdater : public FSRadar::Updater, public LLEventTimer
{
    LOG_CLASS(FSRadarListUpdater);

public:
    FSRadarListUpdater(callback_t cb)
    :   LLEventTimer(FS_RADAR_LIST_UPDATE_INTERVAL),
        FSRadar::Updater(cb)
    {
        update();
        mEventTimer.start();
    }

    bool tick() override
    {
        update();
        return false;
    }
};

//=============================================================================

FSRadar::FSRadar() :
    mRadarAlertRequest(false),
    mRadarFrameCount(0),
    mRadarLastRequestTime(0.f),
    mShowUsernamesCallbackConnection(),
    mNameFormatCallbackConnection(),
    mAgeAlertCallbackConnection(),
    mRegionCapabilitiesReceivedCallbackConnection(),
    mRegionChangedCallbackConnection()
{
    // Use the callback from LLAvatarNameCache here or we might update the names too early!
    LLAvatarNameCache::getInstance()->addUseDisplayNamesCallback(boost::bind(&FSRadar::updateNames, this));
    mShowUsernamesCallbackConnection = gSavedSettings.getControl("NameTagShowUsernames")->getSignal()->connect(boost::bind(&FSRadar::updateNames, this));

    mNameFormatCallbackConnection = gSavedSettings.getControl("RadarNameFormat")->getSignal()->connect(boost::bind(&FSRadar::updateNames, this));
    mAgeAlertCallbackConnection = gSavedSettings.getControl("RadarAvatarAgeAlertValue")->getSignal()->connect(boost::bind(&FSRadar::updateAgeAlertCheck, this));

    mRegionChangedCallbackConnection = gAgent.addRegionChangedCallback([this]() { onRegionChanged(); });
}

FSRadar::~FSRadar()
{
    gAgent.removeRegionChangedCallback(mRegionChangedCallbackConnection);
    if (mRegionCapabilitiesReceivedCallbackConnection.connected())
    {
        mRegionCapabilitiesReceivedCallbackConnection.disconnect();
    }

    if (mShowUsernamesCallbackConnection.connected())
    {
        mShowUsernamesCallbackConnection.disconnect();
    }

    if (mNameFormatCallbackConnection.connected())
    {
        mNameFormatCallbackConnection.disconnect();
    }

    if (mAgeAlertCallbackConnection.connected())
    {
        mAgeAlertCallbackConnection.disconnect();
    }
}

void FSRadar::initSingleton()
{
    mRadarListUpdater = std::make_unique<FSRadarListUpdater>(std::bind(&FSRadar::updateRadarList, this));
}

void FSRadar::radarAlertMsg(const LLUUID& agent_id, const LLAvatarName& av_name, std::string_view postMsg)
{
    // If we're teleporting, we don't want to see alerts about EVERY agent leaving (carried over
    // from the old Alchemy region alerts this system replaces).
    if (gAgent.getTeleportState() != LLAgent::TELEPORT_NONE)
    {
        return;
    }

    // Clickable avatar name (FS:LO FIRE-1439); with @shownames the anonymised radar name is used instead
    const std::string name = RlvActions::canShowName(RlvActions::SNC_DEFAULT, agent_id)
        ? LLSLURL("agent", agent_id, "inspect").getSLURLString()
        : FSRadarEntry::getRadarName(av_name);

// <FS:CR> Milkshake-style radar alerts
    static LLCachedControl<bool> sFSMilkshakeRadarToasts(gSavedSettings, "FSMilkshakeRadarToasts", false);

    if (sFSMilkshakeRadarToasts)
    {
        // Shown through the radar toast panel (notification tag "radar" -> SLPanelRadarAlert)
        LLSD payload = agent_id;
        LLSD args;
        args["NAME"] = name;
        args["MESSAGE"] = static_cast<std::string>(postMsg);
        LLNotificationsUtil::add("RadarAlert",
                                    args,
                                    payload.with("respond_on_mousedown", true),
                                    boost::bind(&SLAvatarActions::zoomIn, agent_id));
    }
    else
    {
// </FS:CR>
        LLChat chat;
        chat.mText = name + " " + std::string(postMsg);
        chat.mSourceType = CHAT_SOURCE_SYSTEM;
        LLNotificationsUI::LLNotificationManager::instance().onChat(chat, LLSD());

        if (FSKeywords::getInstance()->chatContainsKeyword(chat, true))
        {
            FSKeywords::notify(chat);
        }
    } // </FS:CR>
    LL_INFOS("FSRadar") << "Alert " << agent_id << ": " << postMsg << LL_ENDL;
}

void FSRadar::updateRadarList()
{
    //Configuration
    LLWorld* world = LLWorld::getInstance();
    LLMuteList* mutelist = LLMuteList::getInstance();
    LLLocalSpeakerMgr* speakermgr = LLLocalSpeakerMgr::getInstance();
    LLVoiceClient* voice_client = LLVoiceClient::getInstance();
    LLViewerParcelMgr& parcelmgr = LLViewerParcelMgr::instance();
    LLUIColorTable& colortable = LLUIColorTable::instance();
    LLAvatarTracker& avatartracker = LLAvatarTracker::instance();

    // Region-provided chat ranges (FS: LFSimFeatureHandler)
    F32 chat_range_say = 20.f;
    F32 chat_range_shout = 100.f;
    if (LLViewerRegion* agent_region = gAgent.getRegion(); agent_region)
    {
        chat_range_say = (F32)agent_region->getChatRange();
        chat_range_shout = (F32)agent_region->getShoutRange();
    }

    static const std::string str_chat_entering =            LLTrans::getString("entering_chat_range");
    static const std::string str_chat_leaving =             LLTrans::getString("leaving_chat_range");
    static const std::string str_draw_distance_entering =   LLTrans::getString("entering_draw_distance");
    static const std::string str_draw_distance_leaving =    LLTrans::getString("leaving_draw_distance");
    static const std::string str_region_entering =          LLTrans::getString("entering_region");
    static const std::string str_region_entering_distance = LLTrans::getString("entering_region_distance");
    static const std::string str_region_leaving =           LLTrans::getString("leaving_region");
    static const std::string str_avatar_age_alert =         LLTrans::getString("avatar_age_alert");
    static const std::string str_avatar_age_hidden =        LLTrans::getString("avatar_age_not_available");

    static LLCachedControl<bool> sRadarReportChatRangeEnter(gSavedSettings, "RadarReportChatRangeEnter");
    static LLCachedControl<bool> sRadarReportChatRangeLeave(gSavedSettings, "RadarReportChatRangeLeave");
    static LLCachedControl<bool> sRadarReportDrawRangeEnter(gSavedSettings, "RadarReportDrawRangeEnter");
    static LLCachedControl<bool> sRadarReportDrawRangeLeave(gSavedSettings, "RadarReportDrawRangeLeave");
    static LLCachedControl<bool> sRadarReportSimRangeEnter(gSavedSettings, "RadarReportSimRangeEnter");
    static LLCachedControl<bool> sRadarReportSimRangeLeave(gSavedSettings, "RadarReportSimRangeLeave");
    static LLCachedControl<bool> sRadarEnterChannelAlert(gSavedSettings, "RadarEnterChannelAlert");
    static LLCachedControl<bool> sRadarLeaveChannelAlert(gSavedSettings, "RadarLeaveChannelAlert");
    static LLCachedControl<bool> sRadarAvatarAgeAlert(gSavedSettings, "RadarAvatarAgeAlert");
    static LLCachedControl<F32> sNearMeRange(gSavedSettings, "NearMeRange");
    static LLCachedControl<bool> sLimitRange(gSavedSettings, "LimitRadarByRange");
    static LLCachedControl<F32> sRenderFarClip(gSavedSettings, "RenderFarClip");
    static LLCachedControl<bool> sFSLegacyRadarFriendColoring(gSavedSettings, "FSLegacyRadarFriendColoring");
    static LLCachedControl<bool> sFSRadarColorNamesByDistance(gSavedSettings, "FSRadarColorNamesByDistance", false);
    static LLCachedControl<bool> sFSRadarShowMutedAndDerendered(gSavedSettings, "FSRadarShowMutedAndDerendered");

    F32 drawRadius(sRenderFarClip);
    const LLVector3d& posSelf = gAgent.getPositionGlobal();
    LLUUID regionSelf;
    if (LLViewerRegion* own_reg = gAgent.getRegion(); own_reg)
    {
        regionSelf = own_reg->getRegionID();
    }
    bool alertScripts = mRadarAlertRequest; // save the current value, so it doesn't get changed out from under us by another thread
    time_t now = time(nullptr);

    //STEP 0: Clear model data
    mRadarEnterAlerts.clear();
    mRadarLeaveAlerts.clear();
    mRadarEntriesData.clear();
    mAvatarStats.clear();

    //STEP 1: Update our basic data model: detect Avatars & Positions in our defined range
    std::vector<LLVector3d> positions;
    uuid_vec_t avatar_ids;
    if (RlvActions::canShowNearbyAgents())
    {
        if (sLimitRange)
        {
            world->getAvatars(&avatar_ids, &positions, posSelf, sNearMeRange);
        }
        else
        {
            world->getAvatars(&avatar_ids, &positions);
        }
    }

    // Determine lists of new added and removed avatars
    uuid_vec_t current_vec, added_vec, removed_vec;
    current_vec.reserve(mEntryList.size());
    for (const auto& [av_id, entry] : mEntryList)
    {
        current_vec.emplace_back(av_id);
    }
    LLCommonUtils::computeDifference(avatar_ids, current_vec, added_vec, removed_vec);

    // Remove old avatars from our list
    for (const auto& avid : removed_vec)
    {
        if (entry_map_t::iterator found = mEntryList.find(avid); found != mEntryList.end())
        {
            mEntryList.erase(found);
        }
    }

    // Add new avatars
    for (const auto& avid : added_vec)
    {
        mEntryList.emplace(avid, std::make_shared<FSRadarEntry>(avid));
    }

    speakermgr->update(true);

    //STEP 2: Transform detected model list data into more flexible multimap data structure;
    //TS: Count avatars in chat range and in the same region
    U32 inChatRange{ 0U };
    U32 inSameRegion{ 0U };
    std::vector<LLVector3d>::const_iterator
        pos_it = positions.begin(),
        pos_end = positions.end();
    uuid_vec_t::const_iterator
        item_it = avatar_ids.begin(),
        item_end = avatar_ids.end();
    for (;pos_it != pos_end && item_it != item_end; ++pos_it, ++item_it)
    {
        //
        //2a. For each detected av, gather up all data we would want to display or use to drive alerts
        //
        const LLUUID& avId   = static_cast<LLUUID>(*item_it);
        LLVector3d avPos     = static_cast<LLVector3d>(*pos_it);

        if (avId == gAgentID)
        {
            continue;
        }

        // Skip modelling this avatar if its basic data is either inaccessible, or it's a dummy placeholder
        auto ent = getEntry(avId);
        if (!ent) // don't update this radar listing if data is inaccessible
        {
            continue;
        }

        // Try to get the avatar's viewer object - we will need it anyway later
        LLVOAvatar* avVo = static_cast<LLVOAvatar*>(gObjectList.findObject(avId));

        if (avVo && avVo->mIsDummy)
        {
            continue;
        }

        // FS also hides asset-blacklisted (derendered) avatars here; our derender list is
        // object/selection based and has no per-avatar lookup, so only mutes are considered.
        const bool is_muted = mutelist->isMuted(avId);
        const bool should_be_ignored = is_muted;
        ent->mIgnore = should_be_ignored;
        if (!sFSRadarShowMutedAndDerendered && should_be_ignored)
        {
            continue;
        }

        LLUUID avRegion;
        if (LLViewerRegion* reg = world->getRegionFromPosGlobal(avPos))
        {
            avRegion = reg->getRegionID();
        }
        const bool isInSameRegion = (avRegion == regionSelf);
        const bool isOnSameParcel = parcelmgr.inAgentParcel(avPos);
        const S32 seentime = (S32)difftime(now, ent->mFirstSeen);
        const S32 hours = (S32)(seentime / 3600);
        const S32 mins = (S32)((seentime - hours * 3600) / 60);
        const S32 secs = (S32)((seentime - hours * 3600 - mins * 60));
        const std::string avSeenStr = llformat("%d:%02d:%02d", hours, mins, secs);
        const S32& avStatusFlags     = ent->mStatus;
        ERadarPaymentInfoFlag avFlag = FSRADAR_PAYMENT_INFO_NONE;
        if (avStatusFlags & AVATAR_TRANSACTED)
        {
            avFlag = FSRADAR_PAYMENT_INFO_USED;
        }
        else if (avStatusFlags & AVATAR_IDENTIFIED)
        {
            avFlag = FSRADAR_PAYMENT_INFO_FILLED;
        }
        const S32& avAge = ent->mAge;
        const std::string& avName = ent->mName;
        // (FS LSL-bridge Z-offset correction for avatars above 1020m is not ported)
        const F32 avRange = (F32)(avPos[VZ] != AVATAR_UNKNOWN_Z_OFFSET ? dist_vec(avPos, posSelf) : AVATAR_UNKNOWN_RANGE);
        ent->mRange = avRange;
        ent->mGlobalPos = avPos;
        ent->mRegion = avRegion;

        // Double-check range here since limiting range on calling LLWorld::getAvatars does
        // not work if other avatar is beyond draw distance and above 1020m height.
        if (sLimitRange && avRange > sNearMeRange)
        {
            continue;
        }

        //
        //2b. Process newly detected avatars
        //
        radarfields_map_t::iterator last_sweep_found_it = mLastRadarSweep.find(avId);
        if (last_sweep_found_it == mLastRadarSweep.end())
        {
            // chat alerts
            if (sRadarReportChatRangeEnter && (avRange <= chat_range_say) && avRange > AVATAR_UNKNOWN_RANGE)
            {
                LLStringUtil::format_map_t args;
                args["DISTANCE"] = llformat("%3.2f", avRange);
                std::string message = FSCommon::format_string(str_chat_entering, args);
                play_radar_sound("UISndRadarChatEnter"); // <FS:PP> FIRE-6069: Radar alerts sounds
                LLAvatarNameCache::get(avId, boost::bind(&FSRadar::radarAlertMsg, this, _1, _2, message));
            }
            if (sRadarReportDrawRangeEnter && (avRange <= drawRadius) && avRange > AVATAR_UNKNOWN_RANGE)
            {
                LLStringUtil::format_map_t args;
                args["DISTANCE"] = llformat("%3.2f", avRange);
                std::string message = FSCommon::format_string(str_draw_distance_entering, args);
                play_radar_sound("UISndRadarDrawEnter"); // <FS:PP> FIRE-6069: Radar alerts sounds
                LLAvatarNameCache::get(avId, boost::bind(&FSRadar::radarAlertMsg, this, _1, _2, message));
            }
            if (sRadarReportSimRangeEnter && isInSameRegion)
            {
                play_radar_sound("UISndRadarSimEnter"); // <FS:PP> FIRE-6069: Radar alerts sounds
                if (avRange != AVATAR_UNKNOWN_RANGE) // Don't report an inaccurate range in localchat, if the true range is not known.
                {
                    LLStringUtil::format_map_t args;
                    args["DISTANCE"] = llformat("%3.2f", avRange);
                    std::string message = FSCommon::format_string(str_region_entering_distance, args);
                    LLAvatarNameCache::get(avId, boost::bind(&FSRadar::radarAlertMsg, this, _1, _2, message));
                }
                else
                {
                    LLAvatarNameCache::get(avId, boost::bind(&FSRadar::radarAlertMsg, this, _1, _2, str_region_entering));
                }
            }
            if (sRadarEnterChannelAlert || (alertScripts))
            {
                // Autodetect Phoenix chat UUID compatibility.
                // If Leave channel alerts are not set, restrict reports to same-sim only.
                if (!sRadarLeaveChannelAlert)
                {
                    if (isInSameRegion)
                    {
                        mRadarEnterAlerts.push_back(avId);
                    }
                }
                else
                {
                    mRadarEnterAlerts.push_back(avId);
                }
            }
        }

        //
        // 2c. Process previously detected avatars
        //
        else
        {
            RadarFields rf = last_sweep_found_it->second;
            if (sRadarReportChatRangeEnter || sRadarReportChatRangeLeave)
            {
                if (sRadarReportChatRangeEnter && (avRange <= chat_range_say && avRange > AVATAR_UNKNOWN_RANGE) && (rf.lastDistance > chat_range_say || rf.lastDistance == AVATAR_UNKNOWN_RANGE))
                {
                    LLStringUtil::format_map_t args;
                    args["DISTANCE"] = llformat("%3.2f", avRange);
                    std::string message = FSCommon::format_string(str_chat_entering, args);
                    play_radar_sound("UISndRadarChatEnter"); // <FS:PP> FIRE-6069: Radar alerts sounds
                    LLAvatarNameCache::get(avId, boost::bind(&FSRadar::radarAlertMsg, this, _1, _2, message));
                }
                else if (sRadarReportChatRangeLeave && (avRange > chat_range_say || avRange == AVATAR_UNKNOWN_RANGE) && (rf.lastDistance <= chat_range_say && rf.lastDistance > AVATAR_UNKNOWN_RANGE))
                {
                    play_radar_sound("UISndRadarChatLeave"); // <FS:PP> FIRE-6069: Radar alerts sounds
                    LLAvatarNameCache::get(avId, boost::bind(&FSRadar::radarAlertMsg, this, _1, _2, str_chat_leaving));
                }
            }
            if (sRadarReportDrawRangeEnter || sRadarReportDrawRangeLeave)
            {
                if (sRadarReportDrawRangeEnter && (avRange <= drawRadius && avRange > AVATAR_UNKNOWN_RANGE) && (rf.lastDistance > drawRadius || rf.lastDistance == AVATAR_UNKNOWN_RANGE))
                {
                    LLStringUtil::format_map_t args;
                    args["DISTANCE"] = llformat("%3.2f", avRange);
                    std::string message = FSCommon::format_string(str_draw_distance_entering, args);
                    play_radar_sound("UISndRadarDrawEnter"); // <FS:PP> FIRE-6069: Radar alerts sounds
                    LLAvatarNameCache::get(avId, boost::bind(&FSRadar::radarAlertMsg, this, _1, _2, message));
                }
                else if (sRadarReportDrawRangeLeave && (avRange > drawRadius || avRange == AVATAR_UNKNOWN_RANGE) && (rf.lastDistance <= drawRadius && rf.lastDistance > AVATAR_UNKNOWN_RANGE))
                {
                    play_radar_sound("UISndRadarDrawLeave"); // <FS:PP> FIRE-6069: Radar alerts sounds
                    LLAvatarNameCache::get(avId, boost::bind(&FSRadar::radarAlertMsg, this, _1, _2, str_draw_distance_leaving));
                }
            }
            if (sRadarReportSimRangeEnter || sRadarReportSimRangeLeave)
            {
                if (sRadarReportSimRangeEnter && isInSameRegion && avRegion != rf.lastRegion && rf.lastRegion.notNull())
                {
                    play_radar_sound("UISndRadarSimEnter"); // <FS:PP> FIRE-6069: Radar alerts sounds
                    if (avRange != AVATAR_UNKNOWN_RANGE) // Don't report an inaccurate range in localchat, if the true range is not known.
                    {
                        LLStringUtil::format_map_t args;
                        args["DISTANCE"] = llformat("%3.2f", avRange);
                        std::string message = FSCommon::format_string(str_region_entering_distance, args);
                        LLAvatarNameCache::get(avId, boost::bind(&FSRadar::radarAlertMsg, this, _1, _2, message));
                    }
                    else
                    {
                        LLAvatarNameCache::get(avId, boost::bind(&FSRadar::radarAlertMsg, this, _1, _2, str_region_entering));
                    }
                }
                else if (sRadarReportSimRangeLeave && rf.lastRegion == regionSelf && !isInSameRegion && avRegion.notNull())
                {
                    play_radar_sound("UISndRadarSimLeave"); // <FS:PP> FIRE-6069: Radar alerts sounds
                    LLAvatarNameCache::get(avId, boost::bind(&FSRadar::radarAlertMsg, this, _1, _2, str_region_leaving));
                }
            }
            //If we were manually asked to update an external source for all existing avatars, add them to the queue.
            if (alertScripts)
            {
                mRadarEnterAlerts.push_back(avId);
            }
        }

        //
        //2d. Prepare data for presentation view for this avatar
        //
        if (isInSameRegion)
        {
            ++inSameRegion;
        }

        LLSD entry;
        LLSD entry_options;

        entry["id"] = avId;
        entry["name"] = avName;
        entry["in_region"] = isInSameRegion;
        entry["on_parcel"] = isOnSameParcel;
        entry["flags"] = avFlag;
        entry["seen"] = avSeenStr;
        entry["seen_secs"] = seentime;
        entry["range"] = (avRange > AVATAR_UNKNOWN_RANGE ? llformat("%3.2f", avRange) : llformat(">%3.2f", drawRadius));
        // Raw values for the panel's compact display / numeric sorting
        entry["range_known"] = (avRange > AVATAR_UNKNOWN_RANGE);
        entry["range_m"] = (avRange > AVATAR_UNKNOWN_RANGE ? avRange : drawRadius);
        entry["typing"] = (avVo && avVo->isTyping());
        entry["sitting"] = (avVo && (avVo->getParent() || avVo->isMotionActive(ANIM_AGENT_SIT_GROUND) || avVo->isMotionActive(ANIM_AGENT_SIT_GROUND_CONSTRAINED)));

        if (!gRlvHandler.hasBehaviour(RLV_BHVR_SHOWNAMES))
        {
            entry["notes"] = ent->getNotes();
            entry["age_days"] = avAge;
            if (avAge > -1)
                entry["age"] = llformat("%d", avAge);
            else if (avAge == -2)
                entry["age"] = str_avatar_age_hidden;
            else
                entry["age"] = "";
            if (ent->hasAlertAge())
            {
                entry_options["age_color"] = colortable.getColor("AvatarListItemAgeAlert", LLColor4::red).get().getValue();

                if (sRadarAvatarAgeAlert && !ent->hasAgeAlertPerformed())
                {
                    play_radar_sound("UISndRadarAgeAlert");
                    LLStringUtil::format_map_t args;
                    args["AGE"] = llformat("%d", avAge);
                    std::string message = FSCommon::format_string(str_avatar_age_alert, args);
                    LLAvatarNameCache::get(avId, boost::bind(&FSRadar::radarAlertMsg, this, _1, _2, message));
                }
                ent->mAgeAlertPerformed = true;
            }
        }
        else
        {
            entry["notes"] = LLStringUtil::null;
            entry["age"] = "---";
        }

        //AO: Set any range colors / styles
        LLUIColor range_color;
        if (avRange > AVATAR_UNKNOWN_RANGE)
        {
            if (avRange <= chat_range_say)
            {
                range_color = colortable.getColor("AvatarListItemChatRange", LLColor4::red);
                inChatRange++;
            }
            else if (avRange <= chat_range_shout)
            {
                range_color = colortable.getColor("AvatarListItemShoutRange", LLColor4::white);
            }
            else
            {
                range_color = colortable.getColor("AvatarListItemBeyondShoutRange", LLColor4::white);
            }
        }
        else
        {
            range_color = colortable.getColor("AvatarListItemBeyondShoutRange", LLColor4::white);
        }
        entry_options["range_color"] = range_color.get().getValue();

        // FS bolds the range of every avatar inside draw distance, which in practice bolds
        // nearly every row; the range colour already carries the chat/shout distinction.
        entry_options["range_style"] = LLFontGL::NORMAL;

        // Set friends colors / styles
        LLFontGL::StyleFlags nameCellStyle = LLFontGL::NORMAL;
        const LLRelationship* relation = avatartracker.getBuddyInfo(avId);
        if (relation && !sFSLegacyRadarFriendColoring && !gRlvHandler.hasBehaviour(RLV_BHVR_SHOWNAMES))
        {
            nameCellStyle = (LLFontGL::StyleFlags)(nameCellStyle | LLFontGL::BOLD);
        }
        if (is_muted)
        {
            nameCellStyle = (LLFontGL::StyleFlags)(nameCellStyle | LLFontGL::ITALIC);
        }
        entry_options["name_style"] = nameCellStyle;

        // Same base colour as names in the Friends / Groups / Recent avatar lists
        LLColor4 name_color = colortable.getColor("AvatarListItemOnlineColor", LLColor4::white).get();
        name_color = LGGContactSets::getInstance()->getAvatarColor(avId, (sFSRadarColorNamesByDistance ? range_color.get() : name_color), ContactSetType::RADAR);
        entry_options["name_color"] = name_color.getValue();

        // Voice power level indicator
        if (voice_client->voiceEnabled() && voice_client->isVoiceWorking())
        {
            if (LLSpeaker* speaker = speakermgr->findSpeaker(avId); speaker && speaker->isInVoiceChannel())
            {
                if (const char* icon = voice_level_icon(avId); icon)
                {
                    entry["voice_level_icon"] = icon;
                }
            }
        }

        // Save data for our listeners
        LLSD entry_data;
        entry_data["entry"] = entry;
        entry_data["options"] = entry_options;
        mRadarEntriesData.push_back(std::move(entry_data));
    } // End STEP 2, all model/presentation row processing complete.

    //
    //STEP 3, process any bulk actions that require the whole model to be known first
    //

    //
    //3b: process alerts for avatars that where here last frame, but gone this frame (ie, they left)
    //    as well as dispatch all earlier detected alerts for crossing range thresholds.
    //
    if (RlvActions::canShowNearbyAgents())
    {
        for (const auto& [prevId, rf] : mLastRadarSweep)
        {
            if ((sFSRadarShowMutedAndDerendered || !rf.lastIgnore) && mEntryList.find(prevId) == mEntryList.end())
            {
                if (sRadarReportChatRangeLeave && (rf.lastDistance <= chat_range_say) && rf.lastDistance > AVATAR_UNKNOWN_RANGE)
                {
                    play_radar_sound("UISndRadarChatLeave"); // <FS:PP> FIRE-6069: Radar alerts sounds
                    LLAvatarNameCache::get(prevId, boost::bind(&FSRadar::radarAlertMsg, this, _1, _2, str_chat_leaving));
                }
                if (sRadarReportDrawRangeLeave && (rf.lastDistance <= drawRadius) && rf.lastDistance > AVATAR_UNKNOWN_RANGE)
                {
                    play_radar_sound("UISndRadarDrawLeave"); // <FS:PP> FIRE-6069: Radar alerts sounds
                    LLAvatarNameCache::get(prevId, boost::bind(&FSRadar::radarAlertMsg, this, _1, _2, str_draw_distance_leaving));
                }
                if (sRadarReportSimRangeLeave && (rf.lastRegion == regionSelf || rf.lastRegion.isNull()))
                {
                    play_radar_sound("UISndRadarSimLeave"); // <FS:PP> FIRE-6069: Radar alerts sounds
                    LLAvatarNameCache::get(prevId, boost::bind(&FSRadar::radarAlertMsg, this, _1, _2, str_region_leaving));
                }

                if (sRadarLeaveChannelAlert)
                {
                    mRadarLeaveAlerts.push_back(prevId);
                }
            }
        }
    }

    static LLCachedControl<S32> sRadarAlertChannel(gSavedSettings, "RadarAlertChannel");
    if (U32 num_entering = (U32)mRadarEnterAlerts.size(); num_entering > 0)
    {
        mRadarFrameCount++;
        U32 num_this_pass = llmin(FSRADAR_MAX_AVATARS_PER_ALERT, num_entering);
        std::string msg = llformat("%d,%d", mRadarFrameCount, num_this_pass);
        U32 loop = 0;
        while (loop < num_entering)
        {
            for (U32 i = 0; i < num_this_pass; i++)
            {
                msg = llformat("%s,%s", msg.c_str(), mRadarEnterAlerts[loop + i].asString().c_str());
            }
            LLMessageSystem* msgs = gMessageSystem;
            msgs->newMessage("ScriptDialogReply");
            msgs->nextBlock("AgentData");
            msgs->addUUID("AgentID", gAgentID);
            msgs->addUUID("SessionID", gAgentSessionID);
            msgs->nextBlock("Data");
            msgs->addUUID("ObjectID", gAgentID);
            msgs->addS32("ChatChannel", sRadarAlertChannel());
            msgs->addS32("ButtonIndex", 1);
            msgs->addString("ButtonLabel", msg.c_str());
            gAgent.sendReliableMessage();
            loop += num_this_pass;
            num_this_pass = llmin(FSRADAR_MAX_AVATARS_PER_ALERT, num_entering - loop);
            msg = llformat("%d,%d", mRadarFrameCount, num_this_pass);
        }
    }

    if (U32 num_leaving = (U32)mRadarLeaveAlerts.size(); num_leaving > 0)
    {
        mRadarFrameCount++;
        U32 num_this_pass = llmin(FSRADAR_MAX_AVATARS_PER_ALERT, num_leaving);
        std::string msg = llformat("%d,-%d", mRadarFrameCount, llmin(FSRADAR_MAX_AVATARS_PER_ALERT, num_leaving));
        U32 loop = 0;
        while (loop < num_leaving)
        {
            for (U32 i = 0; i < num_this_pass; i++)
            {
                msg = llformat("%s,%s", msg.c_str(), mRadarLeaveAlerts[loop + i].asString().c_str());
            }
            LLMessageSystem* msgs = gMessageSystem;
            msgs->newMessage("ScriptDialogReply");
            msgs->nextBlock("AgentData");
            msgs->addUUID("AgentID", gAgentID);
            msgs->addUUID("SessionID", gAgentSessionID);
            msgs->nextBlock("Data");
            msgs->addUUID("ObjectID", gAgentID);
            msgs->addS32("ChatChannel", sRadarAlertChannel());
            msgs->addS32("ButtonIndex", 1);
            msgs->addString("ButtonLabel", msg.c_str());
            gAgent.sendReliableMessage();
            loop += num_this_pass;
            num_this_pass = llmin(FSRADAR_MAX_AVATARS_PER_ALERT, num_leaving - loop);
            msg = llformat("%d,-%d", mRadarFrameCount, num_this_pass);
        }
    }

    // reset any active alert requests
    if (alertScripts)
    {
        mRadarAlertRequest = false;
    }

    //
    //STEP 4: Cache our current model data, so we can compare it with the next fresh group of model data for fast change detection.
    //

    mLastRadarSweep.clear();
    for (const auto& [avid, entry] : mEntryList)
    {
        RadarFields rf;
        rf.lastDistance = entry->mRange;
        rf.lastIgnore = entry->mIgnore;
        rf.lastRegion.setNull();
        if (!entry->mGlobalPos.isNull())
        {
            if (LLViewerRegion* lastRegion = world->getRegionFromPosGlobal(entry->mGlobalPos))
            {
                rf.lastRegion = lastRegion->getRegionID();
            }
        }

        mLastRadarSweep[entry->mID] = rf;
    }

    //
    //STEP 5: Final data updates and notification of subscribers
    //
    if (RlvActions::canShowNearbyAgents())
    {
        const S32 total = static_cast<S32>(mLastRadarSweep.size()) - (mLastRadarSweep.count(gAgentID) ? 1 : 0);
        mAvatarStats["total"] = llformat("%d", total);
        mAvatarStats["region"] = llformat("%d", inSameRegion);
        mAvatarStats["chatrange"] = llformat("%d", inChatRange);
    }
    else
    {
        mAvatarStats["total"] = "-";
        mAvatarStats["region"] = "-";
        mAvatarStats["chatrange"] = "-";
    }

    checkTracking();

    // Inform our subscribers about updates
    if (!mUpdateSignal.empty())
    {
        mUpdateSignal(mRadarEntriesData, mAvatarStats);
    }
}

void FSRadar::requestRadarChannelAlertSync()
{
    if (F32 timeNow = gFrameTimeSeconds; (timeNow - FSRADAR_CHAT_MIN_SPACING) > mRadarLastRequestTime)
    {
        mRadarLastRequestTime = timeNow;
        mRadarAlertRequest = true;
    }
}

std::shared_ptr<FSRadarEntry> FSRadar::getEntry(const LLUUID& avatar_id)
{
    if (entry_map_t::iterator found = mEntryList.find(avatar_id); found != mEntryList.end())
    {
        return found->second;
    }
    return nullptr;
}

//static
void FSRadar::onRadarNameFmtClicked(const LLSD& userdata)
{
    const std::string chosen_item = userdata.asString();
    if (chosen_item == "DN")
    {
        gSavedSettings.setU32("RadarNameFormat", FSRADAR_NAMEFORMAT_DISPLAYNAME);
    }
    else if (chosen_item == "UN")
    {
        gSavedSettings.setU32("RadarNameFormat", FSRADAR_NAMEFORMAT_USERNAME);
    }
    else if (chosen_item == "DNUN")
    {
        gSavedSettings.setU32("RadarNameFormat", FSRADAR_NAMEFORMAT_DISPLAYNAME_USERNAME);
    }
    else if (chosen_item == "UNDN")
    {
        gSavedSettings.setU32("RadarNameFormat", FSRADAR_NAMEFORMAT_USERNAME_DISPLAYNAME);
    }
}

//static
bool FSRadar::radarNameFmtCheck(const LLSD& userdata)
{
    const std::string menu_item = userdata.asString();
    U32 name_format = gSavedSettings.getU32("RadarNameFormat");
    switch (name_format)
    {
        case FSRADAR_NAMEFORMAT_DISPLAYNAME:
            return (menu_item == "DN");
        case FSRADAR_NAMEFORMAT_USERNAME:
            return (menu_item == "UN");
        case FSRADAR_NAMEFORMAT_DISPLAYNAME_USERNAME:
            return (menu_item == "DNUN");
        case FSRADAR_NAMEFORMAT_USERNAME_DISPLAYNAME:
            return (menu_item == "UNDN");
        default:
            break;
    }
    return false;
}

// <FS:CR> Milkshake-style radar alerts
//static
void FSRadar::onRadarReportToClicked(const LLSD& userdata)
{
    const std::string chosen_item = userdata.asString();
    if (chosen_item == "radar_toasts")
    {
        gSavedSettings.setBOOL("FSMilkshakeRadarToasts", true);
    }
    else if (chosen_item == "radar_nearby_chat")
    {
        gSavedSettings.setBOOL("FSMilkshakeRadarToasts", false);
    }
}

//static
bool FSRadar::radarReportToCheck(const LLSD& userdata)
{
    const std::string menu_item = userdata.asString();
    if (gSavedSettings.getBOOL("FSMilkshakeRadarToasts"))
    {
        return (menu_item == "radar_toasts");
    }
    else
    {
        return (menu_item == "radar_nearby_chat");
    }
}
// </FS:CR>

void FSRadar::startTracking(const LLUUID& avatar_id)
{
    if (getEntry(avatar_id))
    {
        mTrackedAvatarId = avatar_id;
        updateTracking();
    }
    else
    {
        LLNotificationsUtil::add("TrackAvatarNotPossible");
    }
}

void FSRadar::checkTracking()
{
    if (LLTracker::getTrackingStatus() == LLTracker::TRACKING_LOCATION
        && LLTracker::getTrackedLocationType() == LLTracker::LOCATION_AVATAR)
    {
        updateTracking();
    }
}

void FSRadar::updateTracking()
{
    if (auto entry = getEntry(mTrackedAvatarId))
    {
        if (LLTracker::getTrackedPositionGlobal() != entry->mGlobalPos)
        {
            LLTracker::trackLocation(entry->mGlobalPos, entry->mName, "", LLTracker::LOCATION_AVATAR);
        }
    }
    else
    {
        LLTracker::stopTracking(false);
    }
}

void FSRadar::zoomAvatar(const LLUUID& avatar_id, std::string_view name)
{
    if (SLAvatarActions::canZoomIn(avatar_id))
    {
        SLAvatarActions::zoomIn(avatar_id);
    }
    else
    {
        LLStringUtil::format_map_t args;
        args["AVATARNAME"] = static_cast<std::string>(name);
        FSCommon::report_to_nearby_chat(LLTrans::getString("camera_no_focus", args));
    }
}

void FSRadar::updateNames()
{
    for (auto& [av_id, entry] : mEntryList)
    {
        entry->updateName();
    }
}

void FSRadar::updateName(const LLUUID& avatar_id)
{
    if (auto entry = getEntry(avatar_id))
    {
        entry->updateName();
    }
}

void FSRadar::updateAgeAlertCheck()
{
    for (auto& [av_id, entry] : mEntryList)
    {
        entry->checkAge();
    }
}

void FSRadar::updateNotes(const LLUUID& avatar_id, std::string_view notes)
{
    if (auto entry = getEntry(avatar_id))
    {
        entry->setNotes(notes);
    }
}

void FSRadar::onRegionChanged()
{
    if (mRegionCapabilitiesReceivedCallbackConnection.connected())
    {
        mRegionCapabilitiesReceivedCallbackConnection.disconnect();
    }

    if (auto region = gAgent.getRegion())
    {
        if (region->capabilitiesReceived())
        {
            for (auto& [id, entry] : mEntryList)
                entry->requestProperties();
        }
        else
        {
            mRegionCapabilitiesReceivedCallbackConnection = region->setCapabilitiesReceivedCallback(
                [this](const LLUUID&, LLViewerRegion*)
                {
                    mRegionCapabilitiesReceivedCallbackConnection.disconnect();
                    for (auto& [id, entry] : mEntryList)
                        entry->requestProperties();
                });
        }
    }
}
