/**
 * @file fscommon.cpp
 * @brief Central class for common used functions in Firestorm
 *
 * $LicenseInfo:firstyear=2012&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (c) 2012 Ansariel Hiller @ Second Life
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

#include "fscommon.h"

#include "llagent.h"
#include "llagentbenefits.h"
#include "llchat.h"
#include "llnotificationmanager.h"
#include "lltrans.h"
#include "llviewercontrol.h"

void FSCommon::report_to_nearby_chat(std::string_view message)
{
    LLChat chat;
    chat.mText = message;
    chat.mSourceType = CHAT_SOURCE_SYSTEM;
    LLNotificationsUI::LLNotificationManager::instance().onChat(chat, LLSD());
}

std::string FSCommon::format_string(std::string text, const LLStringUtil::format_map_t& args)
{
    LLStringUtil::format(text, args);
    return text;
}

// <FS> Chat bar helpers (ported from Firestorm's FSCommon)
bool FSCommon::is_irc_me_prefix(std::string_view text)
{
    const std::string_view prefix = text.substr(0, 4);
    return (prefix == "/me " || prefix == "/me'");
}

std::string FSCommon::applyAutoCloseOoc(std::string message)
{
    if (!gSavedSettings.getBOOL("AutoCloseOOC") || message.empty())
    {
        return message;
    }

    // Try to find any unclosed OOC chat (i.e. an opening
    // double parenthesis without a matching closing double
    // parenthesis.
    if (message.find("(( ") != std::string::npos && message.find("))") == std::string::npos)
    {
        // add the missing closing double parenthesis.
        message += " ))";
    }
    else if (message.find("((") != std::string::npos && message.find("))") == std::string::npos)
    {
        if (message.at(message.length() - 1) == ')')
        {
            // cosmetic: add a space first to avoid a closing triple parenthesis
            message += " ";
        }
        // add the missing closing double parenthesis.
        message += "))";
    }
    else if (message.find("[[ ") != std::string::npos && message.find("]]") == std::string::npos)
    {
        // add the missing closing double parenthesis.
        message += " ]]";
    }
    else if (message.find("[[") != std::string::npos && message.find("]]") == std::string::npos)
    {
        if (message.at(message.length() - 1) == ']')
        {
            // cosmetic: add a space first to avoid a closing triple parenthesis
            message += " ";
        }
        // add the missing closing double parenthesis.
        message += "]]";
    }

    return message;
}

std::string FSCommon::applyMuPose(std::string message)
{
    // Convert MU*s style poses into IRC emotes here.
    if (gSavedSettings.getBOOL("AllowMUpose") && message.find(':') == 0 && message.length() > 3)
    {
        if (message.find(":'") == 0)
        {
            message.replace(0, 1, "/me");
        }
        else if (!isdigit((U8)message.at(1)) && !ispunct((U8)message.at(1)) && !isspace((U8)message.at(1))) // Do not prevent smileys and such.
        {
            message.replace(0, 1, "/me ");
        }
    }

    return message;
}

LLWString FSCommon::wrapOOC(const LLWString& text)
{
    return utf8str_to_wstring(gSavedSettings.getString("FSOOCPrefix") + " ") + text
        + utf8str_to_wstring(" " + gSavedSettings.getString("FSOOCPostfix"));
}
// </FS>

LLSD FSCommon::populateGroupCount()
{
    LLStringUtil::format_map_t args;
    S32 groupcount = static_cast<S32>(gAgent.mGroups.size());
    S32 maxgroup = LLAgentBenefitsMgr::current().getGroupMembershipLimit();
    args["[COUNT]"] = llformat("%d", groupcount);
    args["[REMAINING]"] = llformat("%d", maxgroup > groupcount ? maxgroup - groupcount : 0);
    std::string groupcountstring =LLTrans::getString((maxgroup ? "groupcountstring" : "groupcountunlimitedstring"), args);
    return LLSD(groupcountstring);
}
