/**
 * @file LLFloaterIMNearbyChat.cpp
 * @brief LLFloaterIMNearbyChat class implementation
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

#include "llviewerprecompiledheaders.h"

#include "message.h"

#include "lliconctrl.h"
#include "llappviewer.h"
#include "llchatentry.h"
#include "llfloaterreg.h"
#include "lltrans.h"
#include "llfloaterimcontainer.h"
#include "llfloatersidepanelcontainer.h"
#include "llfocusmgr.h"
#include "lllogchat.h"
#include "llresizebar.h"
#include "llresizehandle.h"
#include "lldraghandle.h"
#include "llmenugl.h"
#include "llviewermenu.h" // for gMenuHolder
#include "llfloaterimnearbychathandler.h"
#include "llchannelmanager.h"
#include "llchathistory.h"
#include "llstylemap.h"
#include "llavatarnamecache.h"
#include "llfloaterreg.h"
#include "lltrans.h"

#include "llfirstuse.h"
#include "llfloaterimnearbychat.h"
#include "slkeycommands.h"
#include "llfloaterimnearbychatlistener.h"
#include "llagent.h" // gAgent
#include "llagentcamera.h"
#include "lltoolcomp.h"
#include "llcallbacklist.h"
#include "llgesturemgr.h"
#include "llmultigesture.h"
#include "llkeyboard.h"
#include "llanimationstates.h"
#include "llviewerstats.h"
#include "llcommandhandler.h"
#include "llviewercontrol.h"
#include "llnavigationbar.h"
#include "llwindow.h"
#include "llviewerwindow.h"
#include "llrootview.h"
#include "llviewerchat.h"
#include "lltranslate.h"
#include "llautoreplace.h"
#include "lluiusage.h"
// [RLVa:KB] - Checked: 2010-02-27 (RLVa-1.2.0b)
#include "rlvactions.h"
#include "rlvcommon.h"
#include "rlvhandler.h"
// [/RLVa:KB]
#include "slchatcommand.h"
#include "fscommon.h"

S32 LLFloaterIMNearbyChat::sLastSpecialChatChannel = 0;
bool LLFloaterIMNearbyChat::sMouselookChatActive = false;

static LLFloaterIMNearbyChatListener sChatListener;

constexpr S32 EXPANDED_HEIGHT = 266;
constexpr S32 COLLAPSED_HEIGHT = 60;
constexpr S32 EXPANDED_MIN_HEIGHT = 150;

// legacy callback glue
//void send_chat_from_viewer(const std::string& utf8_out_text, EChatType type, S32 channel);
// [RLVa:KB] - Checked: 2010-02-27 (RLVa-0.2.2)
void send_chat_from_viewer(std::string utf8_out_text, EChatType type, S32 channel);
// [/RLVa:KB]

struct LLChatTypeTrigger {
    std::string name;
    EChatType type;
};

static LLChatTypeTrigger sChatTypeTriggers[] = {
    { "/whisper"    , CHAT_TYPE_WHISPER},
    { "/shout"  , CHAT_TYPE_SHOUT}
};

bool cb_do_nothing()
{
    return false;
}

LLFloaterIMNearbyChat::LLFloaterIMNearbyChat(const LLSD& llsd)
:   LLFloaterIMSessionTab(LLSD(LLUUID::null)),
    //mOutputMonitor(NULL),
    mSpeakerMgr(NULL),
    mExpandedHeight(COLLAPSED_HEIGHT + EXPANDED_HEIGHT)
{
    mIsP2PChat = false;
    mIsNearbyChat = true;
    mSpeakerMgr = LLLocalSpeakerMgr::getInstance();

    // Required by LLFloaterIMSessionTab::mGearBtn
    // But nearby floater has no 'per agent' menu items,
    mEnableCallbackRegistrar.add("Avatar.EnableGearItem", boost::bind(&cb_do_nothing));
    mCommitCallbackRegistrar.add("Avatar.GearDoToSelected", boost::bind(&cb_do_nothing));
    mEnableCallbackRegistrar.add("Avatar.CheckGearItem", boost::bind(&cb_do_nothing));

    mMinFloaterHeight = EXPANDED_MIN_HEIGHT;
}

//static
LLFloaterIMNearbyChat* LLFloaterIMNearbyChat::buildFloater(const LLSD& key)
{
    LLFloaterReg::getInstance("im_container");
    return new LLFloaterIMNearbyChat(key);
}

//virtual
bool LLFloaterIMNearbyChat::postBuild()
{
    setIsSingleInstance(true);
    bool result = LLFloaterIMSessionTab::postBuild();

    mInputEditor->setAutoreplaceCallback(boost::bind(&LLAutoReplace::autoreplaceCallback, LLAutoReplace::getInstance(), _1, _2, _3, _4, _5));
    mInputEditor->setCommitCallback(boost::bind(&LLFloaterIMNearbyChat::onChatBoxCommit, this));
    mInputEditor->setKeystrokeCallback(boost::bind(&LLFloaterIMNearbyChat::onChatBoxKeystroke, this));
    mInputEditor->setFocusLostCallback(boost::bind(&LLFloaterIMNearbyChat::onChatBoxFocusLost, this));
    mInputEditor->setFocusReceivedCallback(boost::bind(&LLFloaterIMNearbyChat::onChatBoxFocusReceived, this));
    // Nearby chat: Shift+Enter whispers unless ChatShiftEnterNewline is on (#165);
    // re-read on focus so a Preferences change applies without reopening
    static LLCachedControl<bool> shift_enter_newline(gSavedSettings, "ChatShiftEnterNewline", false);
    mInputEditor->setShiftEnterNewline(shift_enter_newline);
    std::string nearbyChatTitle(LLTrans::getString("NearbyChatTitle"));
    // <FS> Channel selector: hint shows the channel chat goes to (/setchannel, spinner)
    mInputEditor->setLabel(getChannelLabel(getDefaultChatChannel()));
    mChatChannelConnection = gSavedSettings.getControl("SlipstreamNearbyChatChannel")->getSignal()->connect(
        [this](LLControlVariable*, const LLSD& new_val, const LLSD&)
        {
            if (mInputEditor)
            {
                mInputEditor->setLabel(getChannelLabel(new_val.asInteger()));
            }
        });
    mShowChatChannelConnection = gSavedSettings.getControl("FSShowChatChannel")->getSignal()->connect(
        [this](LLControlVariable*, const LLSD&, const LLSD&) { updateChannelSelector(); });
    updateChannelSelector();
    // </FS>

    // #225: the mouselook chat window never outlives mouselook. The out signal fires before
    // LLAgent pops the floaters hidden on entry, so the pre-mouselook visibility is restored.
    mMouselookInConnection = gAgent.setMouselookModeInCallback(&LLFloaterIMNearbyChat::onMouselookModeChanged);
    mMouselookOutConnection = gAgent.setMouselookModeOutCallback(&LLFloaterIMNearbyChat::onMouselookModeChanged);

    // Title must be defined BEFORE call to addConversationListItem() because
    // it is used to show the item's name in the conversations list
    setTitle(nearbyChatTitle);

    // obsolete, but may be needed for backward compatibility?
    gSavedSettings.declareS32("nearbychat_showicons_and_names", 2, "NearByChat header settings", LLControlVariable::PERSIST_NONDFT);

    // #371: the log is frozen here (what is said from now on is live, not history) and its
    // newest page read off the main thread
    mLogSource = LLChatLogSource::create("chat", false);
    // (a windowed history pages the log itself, #369)
    if (!isWindowedHistory() && gSavedPerAccountSettings.getBOOL("LogShowHistory"))
    {
        requestLogTail();
    }

    return result;
}

// virtual
void LLFloaterIMNearbyChat::closeHostedFloater()
{
    // If detached from conversations window close anyway
    if (!getHost())
    {
        setVisible(false);
    }

    // Should check how many conversations are ongoing. Select next to "Nearby Chat" in case there are some other besides.
    // Close conversations window in case "Nearby Chat" is attached and the only conversation
    LLFloaterIMContainer* floater_container = LLFloaterIMContainer::getInstance();
    if (floater_container->getConversationListItemSize() == 1)
    {
        if (getHost())
        {
            floater_container->closeFloater();
        }
    }
    else
    {
        if (!getHost())
        {
            floater_container->selectNextConversationByID(LLUUID());
        }
    }
}

// virtual
void LLFloaterIMNearbyChat::refresh()
{
    displaySpeakingIndicator();
    updateCallBtnState(LLVoiceClient::getInstance()->getUserPTTState());

    // *HACK: Update transparency type depending on whether our children have focus.
    // This is needed because this floater is chrome and thus cannot accept focus, so
    // the transparency type setting code from LLFloater::setFocus() isn't reached.
    // #225: the mouselook chat window stays at inactive transparency so it hides less of the view.
    if (getTransparencyType() != TT_DEFAULT)
    {
        setTransparencyType(hasFocus() && !sMouselookChatActive ? TT_ACTIVE : TT_INACTIVE);
    }
}

void LLFloaterIMNearbyChat::reloadMessages(bool clean_messages/* = false*/)
{
    if (clean_messages)
    {
        // everything logged so far is history now (#371)
        mMessageArchive.clear();
        mLogTail = LLChatLogPage();
        ++mLogTailRequest;  // a page still being read is stale
        mLogSource = LLChatLogSource::create("chat", false);
        if (!isWindowedHistory() && gSavedPerAccountSettings.getBOOL("LogShowHistory"))
        {
            requestLogTail();
        }
    }

    if (isWindowedHistory())
    {
        // #369: the window shows the same messages again (the newest, after a clean reload)
        mChatHistory->reloadWindow(clean_messages);
        return;
    }

    mChatHistory->clear();

    for (const LLChatLogEntry& entry : mLogTail.mMessages)
    {
        appendLogEntry(entry);
    }

    LLSD do_not_log;
    do_not_log["do_not_log"] = true;
    for (const ArchivedChat& archived : mMessageArchive)
    {
        // Update the messages without re-writing them to a log file.
        addMessage(archived.mChat, false, do_not_log);
    }
}

// <FS> Channel selector (the spinner lives in floater_im_session.xml and is only shown for nearby chat)
void LLFloaterIMNearbyChat::updateChannelSelector()
{
    if (LLView* panel = findChild<LLView>("chat_channel_layout_panel"))
    {
        panel->setVisible(gSavedSettings.getBOOL("FSShowChatChannel"));
    }
}

// static
S32 LLFloaterIMNearbyChat::getDefaultChatChannel()
{
    static LLCachedControl<S32> nearby_chat_channel(gSavedSettings, "SlipstreamNearbyChatChannel", 0);
    return nearby_chat_channel();
}

// static
std::string LLFloaterIMNearbyChat::getChannelLabel(S32 channel)
{
    if (channel == 0)
    {
        return LLTrans::getString("NearbyChatTitle");
    }
    LLStringUtil::format_map_t args;
    args["CHANNEL"] = llformat("%d", channel);
    return LLTrans::getString("NearbyChatTitleChannel", args);
}
// </FS>

void LLFloaterIMNearbyChat::requestLogTail()
{
    if (!mLogSource)
    {
        return;
    }
    const U32 request = ++mLogTailRequest;
    LLHandle<LLFloater> handle = getHandle();
    static LLCachedControl<S32> page_messages(gSavedSettings, "SLChatHistoryPageMessages", 50);
    mLogSource->loadPage(LLChatLogCursor::end(), llclamp((S32)page_messages, 10, 500) * 2,
        [handle, request](const LLChatLogPage& page)
        {
            LLFloaterIMNearbyChat* self = dynamic_cast<LLFloaterIMNearbyChat*>(handle.get());
            if (self && self->mLogTailRequest == request)
            {
                self->onLogTailLoaded(page);
            }
        });
}

void LLFloaterIMNearbyChat::onLogTailLoaded(const LLChatLogPage& page)
{
    mLogTail = page;
    // the history goes above what has been said since
    reloadMessages();
}

void LLFloaterIMNearbyChat::appendLogEntry(const LLChatLogEntry& entry)
{
    LLChat chat;
    chatFromLogEntry(entry, chat);

    LLSD do_not_log;
    do_not_log["do_not_log"] = true;
    addMessage(chat, false, do_not_log);
}

void LLFloaterIMNearbyChat::chatFromLogEntry(const LLChatLogEntry& entry, LLChat& chat) const
{
    LLUUID from_id = entry.mFromID;
    if (!entry.mHasFromID)
    {
        std::string legacy_name = gCacheName->buildLegacyName(entry.mFrom);
        from_id = LLAvatarNameCache::getInstance()->findIdByName(legacy_name);
    }

    chat.mFromName = entry.mFrom;
    chat.mFromID = from_id;
    chat.mText = entry.mText;
    chat.mTimeStr = entry.mTime.empty() ? appendTime() : entry.mTime;
    chat.mChatStyle = CHAT_STYLE_HISTORY;

    chat.mSourceType = CHAT_SOURCE_AGENT;
    if (from_id.isNull() && SYSTEM_FROM == entry.mFrom)
    {
        chat.mSourceType = CHAT_SOURCE_SYSTEM;
    }
    else if (from_id.isNull())
    {
        chat.mSourceType = isWordsName(entry.mFrom) ? CHAT_SOURCE_UNKNOWN : CHAT_SOURCE_OBJECT;
    }
}

bool LLFloaterIMNearbyChat::getLiveMessage(S32 index, LLChat& chat, LLSD& args)
{
    if (index < 0 || index >= (S32)mMessageArchive.size())
    {
        return false;
    }
    const ArchivedChat& archived = mMessageArchive[index];
    if (archived.mChat.mMuted)
    {
        return false;
    }
    chat = archived.mChat;
    if (!archived.mSLURL.empty())
    {
        args["slurl"] = archived.mSLURL;
    }
    return true;
}

LLChatLogSource::ptr_t LLFloaterIMNearbyChat::getHistoryLog() const
{
    return gSavedPerAccountSettings.getBOOL("LogShowHistory") ? mLogSource : nullptr;
}

void LLFloaterIMNearbyChat::removeScreenChat()
{
    LLNotificationsUI::LLScreenChannelBase* chat_channel = LLNotificationsUI::LLChannelManager::getInstance()->findChannelByID(
        LLNotificationsUI::NEARBY_CHAT_CHANNEL_UUID);
    if(chat_channel)
    {
        chat_channel->removeToastsFromChannel();
    }
}


void LLFloaterIMNearbyChat::setVisible(bool visible)
{
    LLFloaterIMSessionTab::setVisible(visible);

    if (visible && gSavedSettings.getBOOL("NearbyChatOverlayHideWhenVisible") && isMessagePaneExpanded())
    {
        removeScreenChat();
    }
}


void LLFloaterIMNearbyChat::setVisibleAndFrontmost(bool take_focus, const LLSD& key)
{
    LLFloaterIMSessionTab::setVisibleAndFrontmost(take_focus, key);

    if(matchesKey(key))
    {
        LLFloaterIMContainer::getInstance()->selectConversationPair(mSessionID, true, take_focus);
    }
}

// virtual
void LLFloaterIMNearbyChat::onTearOffClicked()
{
    LLFloaterIMSessionTab::onTearOffClicked();

    // see CHUI-170: Save torn-off state of the nearby chat between sessions
    bool in_the_multifloater(getHost());
    gSavedPerAccountSettings.setBOOL("NearbyChatIsNotTornOff", in_the_multifloater);
}


// virtual
void LLFloaterIMNearbyChat::onOpen(const LLSD& key)
{
    LLFloaterIMSessionTab::onOpen(key);
    if(!isMessagePaneExpanded())
    {
        restoreFloater();
        onCollapseToLine(this);
    }
}

// virtual
void LLFloaterIMNearbyChat::onClose(bool app_quitting)
{
    // Override LLFloaterIMSessionTab::onClose() so that Nearby Chat is not removed from the conversation floater
    LLFloaterIMSessionTab::restoreFloater();
    if (app_quitting)
    {
        // We are starting and closing floater in "expanded" state
        // Update expanded (restored) rect and position for use in next session
        forceReshape();
        storeRectControl();
    }
}

// virtual
void LLFloaterIMNearbyChat::onClickCloseBtn(bool)

{
    if (!isTornOff())
    {
        return;
    }
    closeHostedFloater();
}

void LLFloaterIMNearbyChat::onChatFontChange(LLFontGL* fontp)
{
    // Update things with the new font whohoo
    if (mInputEditor)
    {
        mInputEditor->setFont(fontp);
    }
}


void LLFloaterIMNearbyChat::show()
{
        openFloater(getKey());
}

bool LLFloaterIMNearbyChat::isChatVisible() const
{
    bool isVisible = false;
    LLFloaterIMContainer* im_box = LLFloaterIMContainer::getInstance();
    // Is the IM floater container ever null?
    llassert(im_box != NULL);
    if (im_box != NULL)
    {
        isVisible =
                isChatMultiTab() && gSavedPerAccountSettings.getBOOL("NearbyChatIsNotTornOff")?
                        im_box->getVisible() && !im_box->isMinimized() :
                        getVisible() && !isMinimized();
    }

    return isVisible;
}

void LLFloaterIMNearbyChat::showHistory()
{
    openFloater();
    LLFloaterIMContainer::getInstance()->selectConversation(LLUUID(NULL));

    if(!isMessagePaneExpanded())
    {
        restoreFloater();
        setFocus(true);
    }
    else
    {
        LLFloaterIMContainer::getInstance()->setFocus(true);
    }
    setResizeLimits(getMinWidth(), EXPANDED_MIN_HEIGHT);
}

std::string LLFloaterIMNearbyChat::getCurrentChat()
{
    return mInputEditor ? mInputEditor->getText() : LLStringUtil::null;
}

// virtual
bool LLFloaterIMNearbyChat::handleKeyHere( KEY key, MASK mask )
{
    bool handled = handleChatSearchKey(key, mask); // <FS> Ctrl+F: search in chat history
    if (handled)
    {
        return true;
    }

    // <FS> Each Enter shortcut is optional (FSUseCtrlShout, FSUseShiftWhisper, FSUseAltOOC);
    // a disabled one just says the line
    static LLCachedControl<bool> use_ctrl_shout(gSavedSettings, "FSUseCtrlShout", true);
    static LLCachedControl<bool> use_shift_whisper(gSavedSettings, "FSUseShiftWhisper", true);
    static LLCachedControl<bool> use_alt_ooc(gSavedSettings, "FSUseAltOOC", false);
    SLKeyCommands& key_commands = SLKeyCommands::instance();
    if (key_commands.matches("chat_shout", key, mask))
    {
        // shout
        sendChat(use_ctrl_shout ? CHAT_TYPE_SHOUT : CHAT_TYPE_NORMAL);
        handled = true;
    }
    else if (key_commands.matches("chat_whisper", key, mask))
    {
        // whisper
        sendChat(use_shift_whisper ? CHAT_TYPE_WHISPER : CHAT_TYPE_NORMAL);
        handled = true;
    }
    else if (use_alt_ooc && key_commands.matches("chat_ooc", key, mask))
    {
        // out of character
        sendChat(CHAT_TYPE_NORMAL, true);
        handled = true;
    }
    // </FS>


    if((mask == MASK_ALT) && isTornOff())
    {
        LLFloaterIMContainer* floater_container = LLFloaterIMContainer::getInstance();
        if ((KEY_UP == key) || (KEY_LEFT == key))
        {
            floater_container->selectNextorPreviousConversation(false);
            handled = true;
        }
        if ((KEY_DOWN == key ) || (KEY_RIGHT == key))
        {
            floater_container->selectNextorPreviousConversation(true);
            handled = true;
        }
    }

    return handled;
}

bool LLFloaterIMNearbyChat::matchChatTypeTrigger(const std::string& in_str, std::string* out_str)
{
    auto in_len = in_str.length();
    S32 cnt = sizeof(sChatTypeTriggers) / sizeof(*sChatTypeTriggers);

    bool string_was_found = false;

    for (S32 n = 0; n < cnt && !string_was_found; n++)
    {
        if (in_len <= sChatTypeTriggers[n].name.length())
        {
            std::string trigger_trunc = sChatTypeTriggers[n].name;
            LLStringUtil::truncate(trigger_trunc, in_len);

            if (!LLStringUtil::compareInsensitive(in_str, trigger_trunc))
            {
                *out_str = sChatTypeTriggers[n].name;
                string_was_found = true;
            }
        }
    }

    return string_was_found;
}

void LLFloaterIMNearbyChat::onChatBoxKeystroke()
{
    LLFloaterIMContainer* im_box = LLFloaterIMContainer::findInstance();
    if (im_box)
    {
        im_box->flashConversationItemWidget(mSessionID,false);
    }

    LLFirstUse::otherAvatarChatFirst(false);

    LLWString raw_text = mInputEditor->getWText();

    // Can't trim the end, because that will cause autocompletion
    // to eat trailing spaces that might be part of a gesture.
    LLWStringUtil::trimHead(raw_text);

    auto length = raw_text.length();

    // <FS> only start the typing animation when chatting on channel 0
    if( (length > 0) && (raw_text[0] != '/') && (getDefaultChatChannel() == 0) )  // forward slash is used for escape (eg. emote) sequences
    {
        gAgent.startTyping();
    }
    else
    {
        gAgent.stopTyping();
    }

    /* Doesn't work -- can't tell the difference between a backspace
       that killed the selection vs. backspace at the end of line.
    if (length > 1
        && text[0] == '/'
        && key == KEY_BACKSPACE)
    {
        // the selection will already be deleted, but we need to trim
        // off the character before
        std::string new_text = raw_text.substr(0, length-1);
        mInputEditor->setText( new_text );
        mInputEditor->setCursorToEnd();
        length = length - 1;
    }
    */

    KEY key = gKeyboard->currentKey();

    // Ignore "special" keys, like backspace, arrows, etc.
    if (gSavedSettings.getBOOL("ChatAutocompleteGestures")
        && length > 1
        && raw_text[0] == '/'
        && key < KEY_SPECIAL)
    {
        // we're starting a gesture, attempt to autocomplete

        std::string utf8_trigger = wstring_to_utf8str(raw_text);
        std::string utf8_out_str(utf8_trigger);

        if (LLGestureMgr::instance().matchPrefix(utf8_trigger, &utf8_out_str))
        {
            std::string rest_of_match = utf8_out_str.substr(utf8_trigger.size());
            if (!rest_of_match.empty())
            {
                mInputEditor->setText(utf8_trigger + rest_of_match); // keep original capitalization for user-entered part
                // Select to end of line, starting from the character
                // after the last one the user typed.
                mInputEditor->selectByCursorPosition(static_cast<S32>(utf8_out_str.size() - rest_of_match.size()), static_cast<S32>(utf8_out_str.size()));
            }

        }
        else if (matchChatTypeTrigger(utf8_trigger, &utf8_out_str))
        {
            std::string rest_of_match = utf8_out_str.substr(utf8_trigger.size());
            mInputEditor->setText(utf8_trigger + rest_of_match + " "); // keep original capitalization for user-entered part
            mInputEditor->endOfDoc();
        }

        //LL_INFOS() << "GESTUREDEBUG " << trigger
        //  << " len " << length
        //  << " outlen " << out_str.getLength()
        //  << LL_ENDL;
    }
}

// static
void LLFloaterIMNearbyChat::onChatBoxFocusLost()
{
    // stop typing animation
    gAgent.stopTyping();

    // #225: Escape (handled by the enclosing panel, which just drops focus), empty Enter and
    // close-on-return all end up here. Hide on the next idle instead of inside the focus
    // manager's callback walk, and only if the input did not get focus back meanwhile.
    if (sMouselookChatActive)
    {
        doOnIdleOneTime([]() { LLFloaterIMNearbyChat::hideMouselookChatIfUnfocused(); });
    }
}

void LLFloaterIMNearbyChat::onChatBoxFocusReceived()
{
    mInputEditor->setEnabled(!gDisconnected);
    static LLCachedControl<bool> shift_enter_newline(gSavedSettings, "ChatShiftEnterNewline", false);
    mInputEditor->setShiftEnterNewline(shift_enter_newline);
}

EChatType LLFloaterIMNearbyChat::processChatTypeTriggers(EChatType type, std::string &str)
{
    auto length = str.length();
    S32 cnt = sizeof(sChatTypeTriggers) / sizeof(*sChatTypeTriggers);

    for (S32 n = 0; n < cnt; n++)
    {
        if (length >= sChatTypeTriggers[n].name.length())
        {
            std::string trigger = str.substr(0, sChatTypeTriggers[n].name.length());

            if (!LLStringUtil::compareInsensitive(trigger, sChatTypeTriggers[n].name))
            {
                auto trigger_length = sChatTypeTriggers[n].name.length();

                // It's to remove space after trigger name
                if (length > trigger_length && str[trigger_length] == ' ')
                    trigger_length++;

                str = str.substr(trigger_length, length);

                if (CHAT_TYPE_NORMAL == type)
                    return sChatTypeTriggers[n].type;
                else
                    break;
            }
        }
    }

    return type;
}

void LLFloaterIMNearbyChat::sendChat( EChatType type, bool ooc )
{
    bool sent_empty = true; // #225: empty Enter closes the mouselook chat window
    if (mInputEditor)
    {
        LLWString text = mInputEditor->getConvertedText();
        LLWStringUtil::trim(text);
        LLWStringUtil::replaceChar(text,182,'\n'); // Convert paragraph symbols back into newlines.
        sent_empty = text.empty();
        if (!text.empty())
        {
            // <FS> Alt+Enter: out-of-character chat
            if (ooc)
            {
                text = FSCommon::wrapOOC(text);
            }
            // </FS>
            // Check if this is destined for another channel
            S32 channel = 0;
            // <FS> Channel selector: no "/N" prefix -> the selected channel
            bool is_set = false;
            stripChannelNumber(text, &channel, &is_set);
            const S32 default_channel = getDefaultChatChannel();
            if (!is_set)
            {
                channel = default_channel;
            }
            // </FS>

            updateUsedEmojis(text);

            std::string utf8text = wstring_to_utf8str(text);
            // Try to trigger a gesture, if not chat to a script.
            std::string utf8_revised_text;
            if (0 == channel)
            {
                // <FS> Convert OOC and MU* style poses
                utf8text = FSCommon::applyAutoCloseOoc(utf8text);
                utf8text = FSCommon::applyMuPose(utf8text);
                // </FS>

                // discard returned "found" boolean
                if(!LLGestureMgr::instance().triggerAndReviseString(utf8text, &utf8_revised_text))
                {
                    utf8_revised_text = utf8text;
                }
            }
            else
            {
                utf8_revised_text = utf8text;
            }

            utf8_revised_text = utf8str_trim(utf8_revised_text);

            type = processChatTypeTriggers(type, utf8_revised_text);

            if (!utf8_revised_text.empty() && !SLChatCommand::parseCommand(utf8_revised_text))
            {
                // Chat with animation
                sendChatFromViewer(utf8_revised_text, type, gSavedSettings.getBOOL("PlayChatAnim"), default_channel);
            }
        }

        mInputEditor->setText(LLStringExplicit(""));
    }

    gAgent.stopTyping();

    // If the user wants to stop chatting on hitting return, lose focus
    // and go out of chat mode.
    // #225: the mouselook chat window stands in for the chat bar, so it also honours the
    // chat bar's close-on-return option, and an empty Enter closes it like Escape.
    if (gSavedSettings.getBOOL("CloseChatOnReturn")
        || (sMouselookChatActive && (sent_empty || gSavedSettings.getBOOL("CloseChatBarOnReturn"))))
    {
        stopChat();
    }
}

void LLFloaterIMNearbyChat::addMessage(const LLChat& chat,bool archive,const LLSD &args)
{
    if (isWindowedHistory())
    {
        // #369: the model keeps everything said; the window shows it from there
        if (archive)
        {
            ArchivedChat archived;
            archived.mChat = chat;
            if (archived.mChat.mTimeStr.empty())
            {
                archived.mChat.mTimeStr = appendTime(); // when it was said, not when it is shown
            }
            archived.mSLURL = args["slurl"].asString();
            mMessageArchive.push_back(std::move(archived));
            // Bounded: chat spam must not grow memory for the whole session. Older lines
            // stay readable from the log; the window rebuilds from the live end (it sees
            // the model shrink, as after a clear).
            constexpr size_t MAX_WINDOWED_ARCHIVE = 20000;
            if (mMessageArchive.size() > MAX_WINDOWED_ARCHIVE)
            {
                mMessageArchive.erase(mMessageArchive.begin(), mMessageArchive.begin() + MAX_WINDOWED_ARCHIVE / 2);
            }
            if (!chat.mMuted)
            {
                noteParticipantActivity(chat.mFromID);
            }
            mChatHistory->onLiveMessagesAdded();
        }
    }
    else
    {
        appendMessage(chat, args);

        if(archive)
        {
            ArchivedChat archived;
            archived.mChat = chat;
            mMessageArchive.push_back(std::move(archived));
            if(mMessageArchive.size() > 200)
            {
                mMessageArchive.pop_front();
            }
        }
    }

    // logging
    if (!args["do_not_log"].asBoolean() && gSavedPerAccountSettings.getS32("KeepConversationLogTranscripts") > 1)
    {
        std::string from_name = chat.mFromName;

        if (chat.mSourceType == CHAT_SOURCE_AGENT)
        {
            // if the chat is coming from an agent, log the complete name
            LLAvatarName av_name;
            LLAvatarNameCache::get(chat.mFromID, &av_name);

            if (!av_name.isDisplayNameDefault())
            {
                from_name = av_name.getCompleteName();
            }
        }

        LLLogChat::saveHistory("chat", from_name, chat.mFromID, chat.mText);
    }
}


void LLFloaterIMNearbyChat::onChatBoxCommit()
{
    sendChat(CHAT_TYPE_NORMAL);

    gAgent.stopTyping();
}

void LLFloaterIMNearbyChat::displaySpeakingIndicator()
{
    LLSpeakerMgr::speaker_list_t speaker_list;
    LLUUID id;

    id.setNull();
    mSpeakerMgr->update(false);
    mSpeakerMgr->getSpeakerList(&speaker_list, false);

    for (LLSpeakerMgr::speaker_list_t::iterator i = speaker_list.begin(); i != speaker_list.end(); ++i)
    {
        LLPointer<LLSpeaker> s = *i;
        if (s->mSpeechVolume > 0 || s->mStatus == LLSpeaker::STATUS_SPEAKING)
        {
            id = s->mID;
            break;
        }
    }
}

void LLFloaterIMNearbyChat::sendChatFromViewer(const std::string &utf8text, EChatType type, bool animate)
{
    sendChatFromViewer(utf8text, type, animate, 0);
}

void LLFloaterIMNearbyChat::sendChatFromViewer(const LLWString &wtext, EChatType type, bool animate)
{
    sendChatFromViewer(wstring_to_utf8str(wtext), type, animate, 0);
}

void LLFloaterIMNearbyChat::sendChatFromViewer(const std::string &utf8text, EChatType type, bool animate, S32 default_channel)
{
    const LLWString wtext = utf8str_to_wstring(utf8text);
    // Look for "/20 foo" channel chats.
    S32 channel = 0;
    bool is_set = false;
    LLWString out_text = stripChannelNumber(wtext, &channel, &is_set);
    if (!is_set)
    {
        channel = default_channel;
    }
    std::string utf8_out_text = wstring_to_utf8str(out_text);
    std::string utf8_text = wstring_to_utf8str(wtext);

    utf8_text = utf8str_trim(utf8_text);
    if (!utf8_text.empty())
    {
        utf8_text = utf8str_truncate(utf8_text, MAX_STRING - 1);
    }

// [RLVa:KB] - Checked: 2010-03-27 (RLVa-1.2.0b) | Modified: RLVa-1.2.0b
    if ( (0 == channel) && (RlvActions::isRlvEnabled()) )
    {
        // Adjust the (public) chat "volume" on chat and gestures (also takes care of playing the proper animation)
        type = RlvActions::checkChatVolume(type);
        animate &= !RlvActions::hasBehaviour( (!RlvUtil::isEmote(utf8_text)) ? RLV_BHVR_REDIRCHAT : RLV_BHVR_REDIREMOTE );
    }
// [/RLVa:KB]

    // Don't animate for chats people can't hear (chat to scripts)
    if (animate && (channel == 0))
    {
        if (type == CHAT_TYPE_WHISPER)
        {
            LL_DEBUGS() << "You whisper " << utf8_text << LL_ENDL;
            gAgent.sendAnimationRequest(ANIM_AGENT_WHISPER, ANIM_REQUEST_START);
        }
        else if (type == CHAT_TYPE_NORMAL)
        {
            LL_DEBUGS() << "You say " << utf8_text << LL_ENDL;
            gAgent.sendAnimationRequest(ANIM_AGENT_TALK, ANIM_REQUEST_START);
        }
        else if (type == CHAT_TYPE_SHOUT)
        {
            LL_DEBUGS() << "You shout " << utf8_text << LL_ENDL;
            gAgent.sendAnimationRequest(ANIM_AGENT_SHOUT, ANIM_REQUEST_START);
        }
        else
        {
            LL_INFOS() << "send_chat_from_viewer() - invalid volume" << LL_ENDL;
            return;
        }
    }
    else
    {
        if (type != CHAT_TYPE_START && type != CHAT_TYPE_STOP)
        {
            LL_DEBUGS() << "Channel chat: " << utf8_text << LL_ENDL;
        }
    }

    send_chat_from_viewer(utf8_out_text, type, channel);
}

// static
bool LLFloaterIMNearbyChat::isWordsName(const std::string& name)
{
    // checking to see if it's display name plus username in parentheses
    auto open_paren = name.find(" (", 0);
    auto close_paren = name.find(')', 0);

    if (open_paren != std::string::npos &&
        close_paren == name.length()-1)
    {
        return true;
    }
    else
    {
        //checking for a single space
        auto pos = name.find(' ', 0);
        return std::string::npos != pos && name.rfind(' ', name.length()) == pos && 0 != pos && name.length()-1 != pos;
    }
}

// static
void LLFloaterIMNearbyChat::startChat(const char* line)
{
    LLFloaterIMNearbyChat* nearby_chat = LLFloaterReg::getTypedInstance<LLFloaterIMNearbyChat>("nearby_chat");
    if (nearby_chat)
    {
        if(!nearby_chat->isTornOff())
        {
            LLFloaterIMContainer* container = LLFloaterIMContainer::getInstance();
            container->selectConversation(LLUUID(NULL));
            // #489: the chat key always brings Conversations up, even when minimized or buried
            if (container->isMinimized())
            {
                container->setMinimized(false);
            }
            container->setVisibleAndFrontmost(true);
        }
        if(nearby_chat->isMinimized())
        {
            nearby_chat->setMinimized(false);
        }
        nearby_chat->show();
        nearby_chat->setFocus(true);

        if (line)
        {
            std::string line_string(line);
            nearby_chat->mInputEditor->setText(line_string);
        }

        nearby_chat->mInputEditor->endOfDoc();
    }
}

// static
bool LLFloaterIMNearbyChat::focusIfShowing(const char* line)
{
    static LLCachedControl<bool> sFocusOpenWindow(gSavedSettings, "ChatKeyFocusesOpenChatWindow", true);
    if (!sFocusOpenWindow)
    {
        return false;
    }
    LLFloaterIMNearbyChat* nearby_chat = LLFloaterReg::findTypedInstance<LLFloaterIMNearbyChat>("nearby_chat");
    if (!nearby_chat || !nearby_chat->mInputEditor || !nearby_chat->isInVisibleChain())
    {
        return false;
    }
    if (nearby_chat->isTornOff())
    {
        if (nearby_chat->isMinimized())
        {
            return false;
        }
    }
    else
    {
        // Docked in Conversations: the window must be up, not minimized, and Nearby Chat must
        // be the selected conversation (another tab hides it).
        LLFloaterIMContainer* container = LLFloaterIMContainer::findInstance();
        if (!container || !container->isInVisibleChain() || container->isMinimized()
            || container->getSelectedSession().notNull())
        {
            return false;
        }
        container->setVisibleAndFrontmost(true);
    }
    startChat(line);
    return true;
}

// #225: Enter in mouselook with SlipstreamMouselookChatWindow on. startChat() opens the
// window (LLFloater::openFloater is not subject to the mouselook show block); LLAgent's
// push/popVisibleAll on mouselook enter/exit still owns the window's visibility outside
// mouselook, so we only ever show/hide it in between and never touch the skip lists.
// static
void LLFloaterIMNearbyChat::startMouselookChat(const char* line)
{
    // Ignore focus churn while the window opens and takes focus
    sMouselookChatActive = false;
    startChat(line);

    LLFloaterIMNearbyChat* nearby_chat = LLFloaterReg::findTypedInstance<LLFloaterIMNearbyChat>("nearby_chat");
    if (!nearby_chat || !nearby_chat->mInputEditor || !gAgentCamera.cameraMouselook())
    {
        return;
    }
    sMouselookChatActive = true;

    // Free the mouse so conversations can be picked and history scrolled without steering;
    // clicking back into the world recaptures it.
    LLToolCompGun::getInstance()->freeMouseForUI();

    // Inactive floater transparency while in mouselook so less of the view is covered
    LLFloater* window = nearby_chat->getHost();
    if (!window)
    {
        window = nearby_chat;
    }
    window->updateTransparency(TT_INACTIVE);
}

// static
void LLFloaterIMNearbyChat::hideMouselookChatIfUnfocused()
{
    if (!sMouselookChatActive)
    {
        return;
    }

    LLFloaterIMNearbyChat* nearby_chat = LLFloaterReg::findTypedInstance<LLFloaterIMNearbyChat>("nearby_chat");
    if (!nearby_chat || !nearby_chat->mInputEditor || !gAgentCamera.cameraMouselook())
    {
        sMouselookChatActive = false;
        return;
    }

    LLFloater* window = nearby_chat->getHost();
    if (!window)
    {
        window = nearby_chat;
    }

    // Focus still inside the window (input, another conversation, the list): keep chatting
    LLView* focus = dynamic_cast<LLView*>(gFocusMgr.getKeyboardFocus());
    if (focus && (focus == window || focus->hasAncestor(window)))
    {
        return;
    }

    sMouselookChatActive = false;
    gAgent.stopTyping();

    // Hide (not close: both floaters' onClose have side effects). Leaving mouselook pops the
    // pre-mouselook visibility back regardless of what we do here.
    window->setVisible(false);
    LLToolCompGun::getInstance()->recaptureMouse();
}

// static
void LLFloaterIMNearbyChat::onMouselookModeChanged()
{
    if (!sMouselookChatActive)
    {
        return;
    }
    // Clear first so the focus loss below does not schedule a hide
    sMouselookChatActive = false;

    LLFloaterIMNearbyChat* nearby_chat = LLFloaterReg::findTypedInstance<LLFloaterIMNearbyChat>("nearby_chat");
    if (nearby_chat && nearby_chat->mInputEditor && nearby_chat->mInputEditor->hasFocus())
    {
        nearby_chat->mInputEditor->setFocus(false);
    }
    gAgent.stopTyping();
}

// Exit "chat mode" and do the appropriate focus changes
// static
void LLFloaterIMNearbyChat::stopChat()
{
    LLFloaterIMNearbyChat* nearby_chat = LLFloaterReg::getTypedInstance<LLFloaterIMNearbyChat>("nearby_chat");
    if (nearby_chat)
    {
        nearby_chat->mInputEditor->setFocus(false);
        gAgent.stopTyping();
    }
}

// If input of the form "/20foo" or "/20 foo", returns "foo" and channel 20.
// Otherwise returns input and channel 0.
LLWString LLFloaterIMNearbyChat::stripChannelNumber(const LLWString &mesg, S32* channel, bool* is_set)
{
    if (is_set)
    {
        *is_set = false;
    }
    if (mesg[0] == '/'
        && mesg[1] == '/')
    {
        // This is a "repeat channel send"
        if (is_set)
        {
            *is_set = true;
        }
        *channel = sLastSpecialChatChannel;
        return mesg.substr(2, mesg.length() - 2);
    }
    else if (mesg[0] == '/'
             && mesg[1]
             && (LLStringOps::isDigit(mesg[1])
                 || (mesg[1] == '-' && mesg[2] && LLStringOps::isDigit(mesg[2]))))
    {
        // This a special "/20" speak on a channel
        S32 pos = 0;

        // Copy the channel number into a string
        LLWString channel_string;
        llwchar c;
        do
        {
            c = mesg[pos+1];
            channel_string.push_back(c);
            pos++;
        }
        while(c && pos < 64 && (LLStringOps::isDigit(c) || (pos==1 && c =='-')));

        // Move the pointer forward to the first non-whitespace char
        // Check isspace before looping, so we can handle "/33foo"
        // as well as "/33 foo"
        while(c && iswspace(c))
        {
            c = mesg[pos+1];
            pos++;
        }

        sLastSpecialChatChannel = strtol(wstring_to_utf8str(channel_string).c_str(), NULL, 10);
        *channel = sLastSpecialChatChannel;
        if (is_set)
        {
            *is_set = true;
        }
        return mesg.substr(pos, mesg.length() - pos);
    }
    else
    {
        // This is normal chat.
        *channel = 0;
        return mesg;
    }
}

//void send_chat_from_viewer(const std::string& utf8_out_text, EChatType type, S32 channel)
// [RLVa:KB] - Checked: 2010-02-27 (RLVa-1.2.0b) | Modified: RLVa-0.2.2a
void send_chat_from_viewer(std::string utf8_out_text, EChatType type, S32 channel)
// [/RLVa:KB]
{
// [RLVa:KB] - Checked: 2010-02-27 (RLVa-1.2.0b) | Modified: RLVa-1.2.0a
    // Only process chat messages (ie not CHAT_TYPE_START, CHAT_TYPE_STOP, etc)
    if ( (RlvActions::isRlvEnabled()) && ( (CHAT_TYPE_WHISPER == type) || (CHAT_TYPE_NORMAL == type) || (CHAT_TYPE_SHOUT == type) ) )
    {
        if (0 == channel)
        {
            // Clamp the volume of the chat if needed
            type = RlvActions::checkChatVolume(type);

            // Redirect chat if needed
            if ( ( (gRlvHandler.hasBehaviour(RLV_BHVR_REDIRCHAT) || (gRlvHandler.hasBehaviour(RLV_BHVR_REDIREMOTE)) ) &&
                 (gRlvHandler.redirectChatOrEmote(utf8_out_text)) ) )
            {
                return;
            }

            // Filter public chat if sendchat restricted
            if (gRlvHandler.hasBehaviour(RLV_BHVR_SENDCHAT))
                gRlvHandler.filterChat(utf8_out_text, true);
        }
        else
        {
            // Don't allow chat on a non-public channel if sendchannel restricted (unless the channel is an exception)
            if (!RlvActions::canSendChannel(channel))
                return;

            // Don't allow chat on debug channel if @sendchat, @redirchat or @rediremote restricted (shows as public chat on viewers)
            if (CHAT_CHANNEL_DEBUG == channel)
            {
                bool fIsEmote = RlvUtil::isEmote(utf8_out_text);
                if ( (gRlvHandler.hasBehaviour(RLV_BHVR_SENDCHAT)) ||
                     ((!fIsEmote) && (gRlvHandler.hasBehaviour(RLV_BHVR_REDIRCHAT))) ||
                     ((fIsEmote) && (gRlvHandler.hasBehaviour(RLV_BHVR_REDIREMOTE))) )
                {
                    return;
                }
            }
        }
    }
// [/RLVa:KB]

    LL_DEBUGS("UIUsage") << "Nearby chat, text " << utf8_out_text << " type " << type << " channel " << channel << LL_ENDL;
    if (type != CHAT_TYPE_START && type != CHAT_TYPE_STOP) // prune back some redundant logging
    {
        LLUIUsage::instance().logCommand("Chat.SendNearby"); // pseuo-command
    }

    LLMessageSystem* msg = gMessageSystem;

    if (channel >= 0)
    {
        msg->newMessageFast(_PREHASH_ChatFromViewer);
        msg->nextBlockFast(_PREHASH_AgentData);
        msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
        msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
        msg->nextBlockFast(_PREHASH_ChatData);
        msg->addStringFast(_PREHASH_Message, utf8_out_text);
        msg->addU8Fast(_PREHASH_Type, type);
        msg->addS32("Channel", channel);

    }
    else
    {
        // Hack: ChatFromViewer doesn't allow negative channels
        msg->newMessage("ScriptDialogReply");
        msg->nextBlock("AgentData");
        msg->addUUID("AgentID", gAgentID);
        msg->addUUID("SessionID", gAgentSessionID);
        msg->nextBlock("Data");
        msg->addUUID("ObjectID", gAgentID);
        msg->addS32("ChatChannel", channel);
        msg->addS32("ButtonIndex", 0);
        msg->addString("ButtonLabel", utf8_out_text);
    }

    gAgent.sendReliableMessage();
    add(LLStatViewer::CHAT_COUNT, 1);
}

class LLChatCommandHandler : public LLCommandHandler
{
public:
    // not allowed from outside the app
    LLChatCommandHandler() : LLCommandHandler("chat", UNTRUSTED_BLOCK) { }

    // Your code here
    bool handle(const LLSD& tokens,
                const LLSD& query_map,
                const std::string& grid,
                LLMediaCtrl* web)
    {
        bool retval = false;
        // Need at least 2 tokens to have a valid message.
        if (tokens.size() < 2)
        {
            retval = false;
        }
        else
        {
        S32 channel = tokens[0].asInteger();
            // VWR-19499 Restrict function to chat channels greater than 0.
            if ((channel > 0) && (channel < CHAT_CHANNEL_DEBUG))
            {
                retval = true;
        // Send unescaped message, see EXT-6353.
        std::string unescaped_mesg (LLURI::unescape(tokens[1].asString()));
        send_chat_from_viewer(unescaped_mesg, CHAT_TYPE_NORMAL, channel);
            }
            else
            {
                retval = false;
                // Tell us this is an unsupported SLurl.
            }
        }
        return retval;
    }
};

// Creating the object registers with the dispatcher.
LLChatCommandHandler gChatHandler;
