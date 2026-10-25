/**
 * @file fskeywords.cpp
 *
 * $LicenseInfo:firstyear=2011&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2011, The Phoenix Firestorm Project, Inc.
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

#include "fskeywords.h"
#include "llagent.h"
#include "llchat.h"
#include "llinstantmessage.h"
#include "llmutelist.h"
#include "llui.h"
#include "llviewercontrol.h"

#include <boost/regex.hpp>

FSKeywords::FSKeywords()
{
    gSavedPerAccountSettings.getControl("FSKeywords")->getSignal()->connect(boost::bind(&FSKeywords::updateKeywords, this));
    gSavedPerAccountSettings.getControl("FSKeywordCaseSensitive")->getSignal()->connect(boost::bind(&FSKeywords::updateKeywords, this));
    gSavedPerAccountSettings.getControl("FSKeywordMatchWholeWords")->getSignal()->connect(boost::bind(&FSKeywords::updateKeywords, this));
    updateKeywords();
}

void FSKeywords::updateKeywords()
{
    bool match_whole_words = gSavedPerAccountSettings.getBOOL("FSKeywordMatchWholeWords");
    std::string s = gSavedPerAccountSettings.getString("FSKeywords");
    if (!gSavedPerAccountSettings.getBOOL("FSKeywordCaseSensitive"))
    {
        LLStringUtil::toLower(s);
    }
    boost::regex re(",");
    boost::sregex_token_iterator begin(s.begin(), s.end(), re, -1), end;
    mWordList.clear();
    mWordRegexes.clear();
    while (begin != end)
    {
        std::string token(*begin++);
        LLStringUtil::trim(token);
        if (token.empty())
        {
            continue; // Slipstream: an empty entry ("a,,b" or a trailing comma) would match every message
        }

        if (match_whole_words)
        {
            mWordList.push_back(boost::regex_replace(token, boost::regex("[.^$|()\\[\\]{}*+?\\\\]"), "\\\\&", boost::match_default|boost::format_sed));
            mWordRegexes.emplace_back("\\b" + mWordList.back() + "\\b");
        }
        else
        {
            mWordList.push_back(token);
        }
    }
}

bool FSKeywords::chatContainsKeyword(const LLChat& chat, bool is_local)
{
    // Don't check if message is from us - unless it's a radar notification - or if it's on the debug channel
    if ((chat.mFromID == gAgentID && chat.mFromName != SYSTEM_FROM) || chat.mChatType == CHAT_TYPE_DEBUG_MSG)
    {
        return false;
    }

    static LLCachedControl<bool> sFSKeywordOn(gSavedPerAccountSettings, "FSKeywordOn", false);
    static LLCachedControl<bool> sFSKeywordInChat(gSavedPerAccountSettings, "FSKeywordInChat", false);
    static LLCachedControl<bool> sFSKeywordInIM(gSavedPerAccountSettings, "FSKeywordInIM", false);

    if (!sFSKeywordOn ||
        (is_local && !sFSKeywordInChat) ||
        (!is_local && !sFSKeywordInIM))
    {
        return false;
    }

    std::string source;
    static LLCachedControl<bool> sFSKeywordSpeakersName(gSavedPerAccountSettings, "FSKeywordSpeakersName", false);
    if (sFSKeywordSpeakersName)
    {
        source = chat.mFromName + " " + chat.mText;
    }
    else
    {
        source = chat.mText;
    }

    static LLCachedControl<bool> sFSKeywordCaseSensitive(gSavedPerAccountSettings, "FSKeywordCaseSensitive", false);
    if (!sFSKeywordCaseSensitive)
    {
        LLStringUtil::toLower(source);
    }

    static LLCachedControl<bool> sFSKeywordMatchWholeWords(gSavedPerAccountSettings, "FSKeywordMatchWholeWords", false);
    if (sFSKeywordMatchWholeWords)
    {
        if (mWordRegexes.size() == mWordList.size())
        {
            for (const auto& word_re : mWordRegexes)
            {
                if (boost::regex_search(source, word_re))
                {
                    return true;
                }
            }
        }
        else
        {
            // list was built for substring mode (setting change not yet applied)
            for (const auto& word : mWordList)
            {
                if (boost::regex_search(source, boost::regex("\\b" + word + "\\b")))
                {
                    return true;
                }
            }
        }
    }
    else
    {
        for (const auto& word : mWordList)
        {
            if (source.find(word) != std::string::npos)
            {
                return true;
            }
        }
    }

    return false;
}

void FSKeywords::notify(const LLChat& chat)
{
    if ((chat.mFromID != gAgentID || chat.mFromName == SYSTEM_FROM) && !chat.mMuted && !LLMuteList::getInstance()->isMuted(chat.mFromID))
    {
        static LLCachedControl<bool> PlayModeUISndFSKeywordSound(gSavedPerAccountSettings, "PlayModeUISndFSKeywordSound");
        if (PlayModeUISndFSKeywordSound && LLUI::getInstance()->mAudioCallback)
        {
            LLUI::getInstance()->mAudioCallback(LLUUID(gSavedPerAccountSettings.getString("UISndFSKeywordSound")));
        }
        // Slipstream: Firestorm's Growl notification is not ported.
    }
}
