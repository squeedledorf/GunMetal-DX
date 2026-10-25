/**
 * @file llfloaterimnearbychat.h
 * @brief LLFloaterIMNearbyChat class definition
 *
 * $LicenseInfo:firstyear=2002&license=viewerlgpl$
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

#ifndef LL_LLFLOATERIMNEARBYCHAT_H
#define LL_LLFLOATERIMNEARBYCHAT_H

#include "llfloaterimsessiontab.h"
#include "llcombobox.h"
#include "llgesturemgr.h"
#include "llchat.h"
#include "llvoiceclient.h"
#include "lloutputmonitorctrl.h"
#include "llspeakers.h"
#include "llscrollbar.h"
#include "llviewerchat.h"
#include "llpanel.h"

#include <deque>

class LLResizeBar;

class LLFloaterIMNearbyChat
    :   public LLFloaterIMSessionTab
{
public:
    // constructor for inline chat-bars (e.g. hosted in chat history window)
    LLFloaterIMNearbyChat(const LLSD& key = LLSD(LLUUID()));
    ~LLFloaterIMNearbyChat() {}

    static LLFloaterIMNearbyChat* buildFloater(const LLSD& key);

    /*virtual*/ bool postBuild();
    /*virtual*/ void onOpen(const LLSD& key);
    /*virtual*/ void onClose(bool app_quitting);
    /*virtual*/ void setVisible(bool visible);
    /*virtual*/ void setVisibleAndFrontmost(bool take_focus=true, const LLSD& key = LLSD());
    /*virtual*/ void closeHostedFloater();

    void reloadMessages(bool clean_messages = false);
    void removeScreenChat();

    void show();
    bool isChatVisible() const;

    /** @param archive true - to save a message to the chat history log */
    void    addMessage          (const LLChat& message,bool archive = true, const LLSD &args = LLSD());

    LLChatEntry* getChatBox() { return mInputEditor; }

    std::string getCurrentChat();
    S32 getMessageArchiveLength() { return static_cast<S32>(mMessageArchive.size()); }

    virtual bool handleKeyHere( KEY key, MASK mask );

    static void startChat(const char* line);
    // ChatKeyFocusesOpenChatWindow (#489): if the Nearby Chat panel is showing, focus its input
    // (seeded with `line` if given) and return true; otherwise do nothing and return false.
    static bool focusIfShowing(const char* line);
    static void stopChat();

    // Mouselook chat window (SlipstreamMouselookChatWindow, #225): Enter in mouselook opens
    // this window (with history) instead of the chat bar. It hides again as soon as the
    // input loses focus (Escape, empty Enter, close-on-return) and on leaving mouselook.
    static void startMouselookChat(const char* line);
    static bool isMouselookChatActive() { return sMouselookChatActive; }

    static void sendChatFromViewer(const std::string &utf8text, EChatType type, bool animate);
    static void sendChatFromViewer(const LLWString &wtext, EChatType type, bool animate);
    // <FS> Channel selector: text typed into a chat bar goes to default_channel unless it
    // starts with an explicit "/N" or "//" prefix
    static void sendChatFromViewer(const std::string &utf8text, EChatType type, bool animate, S32 default_channel);
    // Channel nearby chat bars send to when no "/N" prefix is given (/setchannel, channel spinner)
    static S32 getDefaultChatChannel();
    // </FS>

    static bool isWordsName(const std::string& name);

    void showHistory();

    // LLChatHistorySource (#369): what was said nearby this session, and the "chat" log
    S32  getLiveMessageCount() const override { return static_cast<S32>(mMessageArchive.size()); }
    bool getLiveMessage(S32 index, LLChat& chat, LLSD& args) override;
    LLChatLogSource::ptr_t getHistoryLog() const override;
    void chatFromLogEntry(const LLChatLogEntry& entry, LLChat& chat) const override;

protected:
    static bool matchChatTypeTrigger(const std::string& in_str, std::string* out_str);
    void onChatBoxKeystroke();
    void onChatBoxFocusLost();
    void onChatBoxFocusReceived();

    void sendChat( EChatType type, bool ooc = false ); // <FS> ooc: wrap in FSOOCPrefix / FSOOCPostfix
    void onChatBoxCommit();
    void onChatFontChange(LLFontGL* fontp);

    /*virtual*/ void onTearOffClicked();
    /*virtual*/ void onClickCloseBtn(bool app_qutting = false);

public:
    // Shared with SLChatBar (the nearby chat bar).
    static LLWString stripChannelNumber(const LLWString &mesg, S32* channel, bool* is_set = nullptr);
    // "Nearby chat" / "Nearby chat (channel N)" hint text for a chat bar
    static std::string getChannelLabel(S32 channel);
    static EChatType processChatTypeTriggers(EChatType type, std::string &str);

protected:
    void displaySpeakingIndicator();
    void updateChannelSelector();

    // Which non-zero channel did we last chat on?
    static S32 sLastSpecialChatChannel;

    // #225: window was opened by Enter in mouselook and should hide when the input loses focus
    static bool sMouselookChatActive;
    static void hideMouselookChatIfUnfocused();
    static void onMouselookModeChanged();

    LLOutputMonitorCtrl*    mOutputMonitor;
    LLLocalSpeakerMgr*      mSpeakerMgr;

    S32 mExpandedHeight;

private:
    /*virtual*/ void refresh();

    // #371: nearby chat's history (the "chat" log), read off the main thread
    void requestLogTail();
    void onLogTailLoaded(const LLChatLogPage& page);
    void appendLogEntry(const LLChatLogEntry& entry);

    // Nearby chat's model: everything said nearby this session (#369: a windowed history keeps
    // all of it; the unwindowed one the last 200, as before)
    struct ArchivedChat
    {
        LLChat      mChat;
        std::string mSLURL;     // an object's location, when appendMessage() was given one
    };
    std::deque<ArchivedChat> mMessageArchive;

    LLChatLogSource::ptr_t  mLogSource;
    LLChatLogPage           mLogTail;
    U32                     mLogTailRequest = 0;

    boost::signals2::scoped_connection mChatChannelConnection;
    boost::signals2::scoped_connection mShowChatChannelConnection;
    boost::signals2::scoped_connection mMouselookInConnection;
    boost::signals2::scoped_connection mMouselookOutConnection;
};

#endif // LL_LLFLOATERIMNEARBYCHAT_H
