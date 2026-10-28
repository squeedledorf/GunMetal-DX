/**
 * @file slchatbar.cpp
 * @brief SLChatBar class implementation
 *
 * $LicenseInfo:firstyear=2002&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
 *
 * Alchemy Viewer Source Code
 * Copyright © 2026, Rye <rye@alchemyviewer.org>
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

#include "slchatbar.h"
#include "gmdxchatlog.h" // <GMDX>
#include "slkeycommands.h"

#include "llautoreplace.h"
#include "llfontgl.h"
#include "llrect.h"
#include "llerror.h"
#include "llparcel.h"
#include "llstring.h"
#include "message.h"
#include "llfocusmgr.h"
#include "llfloater.h"
#include "llfloaterreg.h"
#include "lltrans.h"
#include "llchatentry.h"

#include "slchatcommand.h"
#include "fscommon.h"
#include "llagent.h"
#include "llbutton.h"
#include "llcombobox.h"
#include "llcommandhandler.h"   // secondlife:///app/chat/ support
#include "llviewercontrol.h"
#include "llgesturemgr.h"
#include "llfloaterimnearbychat.h"
#include "llkeyboard.h"
#include "lllineeditor.h"
#include "llrender2dutils.h"   // <GMDX>
#include "lluicolortable.h"    // <GMDX>
#include "llstatusbar.h"
#include "lltextbox.h"
#include "lltooldraganddrop.h"  // <GMDX>
#include "lluiconstants.h"
#include "llviewergesture.h"            // for triggering gestures
#include "llviewermenu.h"       // for deleting object with DEL key
#include "llviewerstats.h"
#include "llviewerwindow.h"
#include "llframetimer.h"
#include "llresmgr.h"
#include "llworld.h"
#include "llinventorymodel.h"
#include "llmultigesture.h"
#include "llui.h"
#include "lluictrlfactory.h"
#include "llviewerchat.h"
// [RLVa:KB] - Checked: 2010-02-27 (RLVa-1.2.0b)
#include "rlvactions.h"
#include "rlvcommon.h"
#include "llavatarnamecache.h"   // <GMDX> IM mode
#include "llimview.h"            // <GMDX>
#include "llinstantmessage.h"    // <GMDX>
#include "llslurl.h"             // <GMDX>
// [/RLVa:KB]

class SLChatBarGestureObserver final : public LLGestureManagerObserver
{
public:
    SLChatBarGestureObserver(SLChatBar* chat_barp) : mChatBar(chat_barp){}
    virtual ~SLChatBarGestureObserver() = default;
    void changed() override { mChatBar->refreshGestures(); }
private:
    SLChatBar* mChatBar;
};


//extern void send_chat_from_viewer(const std::string& utf8_out_text, EChatType type, S32 channel);
// [RLVa:KB] - Checked: 2010-02-27 (RLVa-0.2.2)
extern void send_chat_from_viewer(std::string utf8_out_text, EChatType type, S32 channel);
// [/RLVa:KB]

//
// Functions
//

SLChatBar::SLChatBar(const LLSD& key)
:   LLFloater(key),
    mInputEditor(nullptr),
    mGestureLabelTimer(),
    mIsBuilt(FALSE),
    mGestureCombo(nullptr),
    mObserver(nullptr)
{
    mCommitCallbackRegistrar.add("Chatbar.Shout", boost::bind(&SLChatBar::sendChat, this, CHAT_TYPE_SHOUT, false));
    mCommitCallbackRegistrar.add("Chatbar.Whisper", boost::bind(&SLChatBar::sendChat, this, CHAT_TYPE_WHISPER, false));
    mCommitCallbackRegistrar.add("Chatbar.ResetChatLogPosition", boost::bind(&SLChatBar::resetChatLogPosition, this));   // <GMDX>
}


SLChatBar::~SLChatBar()
{
    LLGestureMgr::instance().removeObserver(mObserver);
    delete mObserver;
    mObserver = nullptr;

    delete mReshapeSignal;
    mReshapeSignal = nullptr;

    mChatChannelConnection.disconnect();
    mShowChatChannelConnection.disconnect();
    mLogStyleConnection.disconnect();   // <GMDX>
    mChatFontSizeConnection.disconnect();
}

//-----------------------------------------------------------------------
// Overrides
//-----------------------------------------------------------------------

bool SLChatBar::postBuild()
{
    // * NOTE: mantipov: getChild with default parameters returns dummy widget.
    // Seems this class will be completle removed
    // attempt to bind to an existing combo box named gesture
    setGestureCombo(findChild<LLComboBox>("Gesture"));

    mInputEditor = getChild<LLChatEntry>("Chat Editor");
    mInputEditor->setAutoreplaceCallback(boost::bind(&LLAutoReplace::autoreplaceCallback, LLAutoReplace::getInstance(), _1, _2, _3, _4, _5));
    mInputEditor->setKeystrokeCallback(boost::bind(&SLChatBar::onInputEditorKeystroke, this, _1));
    mInputEditor->setFocusLostCallback(boost::bind(&SLChatBar::onInputEditorFocusLost));
    mInputEditor->setFocusReceivedCallback(boost::bind(&SLChatBar::onInputEditorGainFocus));
    mInputEditor->setCommitOnFocusLost( false );
    mInputEditor->setPassDelete(true);
    mInputEditor->setShowEmojiHelper(true);
    mInputEditor->enableSingleLineMode(true);
    // <FS> Channel selector: /setchannel and the optional spinner set SlipstreamNearbyChatChannel
    changeChannelLabel(LLFloaterIMNearbyChat::getDefaultChatChannel());
    mChatChannelConnection = gSavedSettings.getControl("SlipstreamNearbyChatChannel")->getSignal()->connect(
        [this](LLControlVariable*, const LLSD& new_val, const LLSD&) { changeChannelLabel(new_val.asInteger()); });
    mShowChatChannelConnection = gSavedSettings.getControl("FSShowChatChannel")->getSignal()->connect(
        [this](LLControlVariable*, const LLSD&, const LLSD&) { updateChannelSelector(); });
    mLogStyleConnection = gSavedSettings.getControl("NearbyChatOverlayStyle")->getSignal()->connect(   // <GMDX>
        [this](LLControlVariable*, const LLSD&, const LLSD&) { updateChannelSelector(); });
    updateChannelSelector();
    // </FS>
    mInputEditor->setMaxTextLength(DB_CHAT_MSG_STR_LEN * 5);

    mInputEditor->setFont(LLViewerChat::getChatFont());

    mChatFontSizeConnection = gSavedSettings.getControl("ChatFontSize")->getSignal()->connect([this](LLControlVariable* control, const LLSD& new_val, const LLSD& old_val) { mInputEditor->setFont(LLViewerChat::getChatFont()); });

    // <GMDX> Not moved by the user yet (no saved rect): 5 px from the WINDOW's left edge. The XUI
    // "left" counts from the floater snap region, which starts after the left toolbar, so every
    // shortcut there moved the chat bar (and the chat log over it) right. The toolbars never push
    // it either (LLFloaterView::adjustToFitScreen).
    const std::string rect_control = LLFloaterReg::getRectControlName("chatbar");
    if (!LLFloater::getControlGroup()->controlExists(rect_control) || LLFloater::getControlGroup()->getRect(rect_control).isEmpty())
    {
        setOrigin(5, getRect().mBottom);
    }
    // the channel spinner's number without a skin box, on the bar's own fill (draw)
    if (LLLineEditor* number = findChild<LLLineEditor>("SpinCtrl Editor"))
    {
        number->setBgImage(nullptr);
        number->setBgImageFocused(nullptr);
    }
    // IM mode's x: back to nearby chat
    if (LLButton* cancel = findChild<LLButton>("im_cancel"))
    {
        cancel->setCommitCallback([this](LLUICtrl*, const LLSD&) { cancelIM(); mInputEditor->setFocus(TRUE); });
    }
    // where Reset chat log position puts it (the saved rect, if any, is applied after postBuild)
    mDefaultRect = getRect();
    mDefaultRect.setOriginAndSize(5, mDefaultRect.mBottom, mDefaultRect.getWidth(), mDefaultRect.getHeight());
    // </GMDX>

    return TRUE;
}

void SLChatBar::onOpen(const LLSD& key)
{
    applyShiftEnterNewline();
    mInputEditor->setFocus(TRUE);
}

// virtual
bool SLChatBar::handleKeyHere( KEY key, MASK mask )
{
    bool handled = FALSE;

    // Whisper, shout and out of character are rebindable commands (#863), plain Enter says
    SLKeyCommands& key_commands = SLKeyCommands::instance();
    if (KEY_RETURN == key && mask == (MASK_CONTROL | MASK_SHIFT))
    {
        // whisper (older shortcut, kept next to the rebindable one)
        sendChat(CHAT_TYPE_WHISPER);
        handled = TRUE;
    }
    else if (key_commands.matches("chat_whisper", key, mask))
    {
        // whisper (#681/#219: was missing here, so Shift+Enter fell through unhandled
        // and nothing was sent). Same options as the nearby chat floater.
        static LLCachedControl<bool> use_shift_whisper(gSavedSettings, "FSUseShiftWhisper", true);
        sendChat(use_shift_whisper ? CHAT_TYPE_WHISPER : CHAT_TYPE_NORMAL);
        handled = TRUE;
    }
    else if (key_commands.matches("chat_shout", key, mask))
    {
        // shout (<FS> FSUseCtrlShout off: just say it)
        static LLCachedControl<bool> use_ctrl_shout(gSavedSettings, "FSUseCtrlShout", true);
        sendChat(use_ctrl_shout ? CHAT_TYPE_SHOUT : CHAT_TYPE_NORMAL);
        handled = TRUE;
    }
    else if (key_commands.matches("chat_ooc", key, mask) && gSavedSettings.getBOOL("FSUseAltOOC"))
    {
        // <FS> out of character
        sendChat(CHAT_TYPE_NORMAL, true);
        handled = TRUE;
    }
    else if (KEY_RETURN == key && mask == MASK_NONE)
    {
        // say
        sendChat( CHAT_TYPE_NORMAL );
        handled = TRUE;
    }
    // only do this in main chatbar
    else if ( KEY_ESCAPE == key)
    {
        // <GMDX> IM mode: back to nearby chat first
        if (mIMTarget.notNull()) cancelIM();
        else
        // </GMDX>
        stopChat();

        handled = TRUE;
    }
    return handled;
}

void SLChatBar::onFocusLost()
{
    stopChat();
}

void SLChatBar::refresh()
{
    // HACK: Leave the name of the gesture in place for a few seconds.
    const F32 SHOW_GESTURE_NAME_TIME = 2.f;
    if (mGestureLabelTimer.getStarted() && mGestureLabelTimer.getElapsedTimeF32() > SHOW_GESTURE_NAME_TIME)
    {
        LLCtrlListInterface* gestures = mGestureCombo ? mGestureCombo->getListInterface() : NULL;
        if (gestures) gestures->selectFirstItem();
        mGestureLabelTimer.stop();
    }

    if ((gAgent.getTypingTime() > LLAgent::TYPING_TIMEOUT_SECS) && (gAgent.getRenderState() & AGENT_STATE_TYPING))
    {
        gAgent.stopTyping();
    }
}

void SLChatBar::refreshGestures()
{
    if (mGestureCombo)
    {
        //store current selection so we can maintain it
        std::string cur_gesture = mGestureCombo->getValue().asString();
        mGestureCombo->selectFirstItem();

        // clear
        mGestureCombo->clearRows();

        // collect list of unique gestures
        std::map <std::string, bool> unique;
        LLGestureMgr::item_map_t::const_iterator it;
        const LLGestureMgr::item_map_t& active_gestures = LLGestureMgr::instance().getActiveGestures();
        for (it = active_gestures.begin(); it != active_gestures.end(); ++it)
        {
            LLMultiGesture* gesture = (*it).second;
            if (gesture)
            {
                if (!gesture->mTrigger.empty())
                {
                    unique[gesture->mTrigger] = TRUE;
                }
            }
        }

        // add unique gestures
        std::map <std::string, bool>::iterator it2;
        for (it2 = unique.begin(); it2 != unique.end(); ++it2)
        {
            mGestureCombo->addSimpleElement((*it2).first);
        }

        mGestureCombo->sortByName();
        // Insert label after sorting, at top, with separator below it
        mGestureCombo->addSeparator(ADD_TOP);
        mGestureCombo->addSimpleElement(getString("gesture_label"), ADD_TOP);

        if (!cur_gesture.empty())
        {
            mGestureCombo->selectByValue(LLSD(cur_gesture));
        }
        else
        {
            mGestureCombo->selectFirstItem();
        }
    }
}

// Move the cursor to the correct input field.
void SLChatBar::setKeyboardFocus(bool focus)
{
    if (focus)
    {
        if (mInputEditor)
        {
            mInputEditor->setFocus(TRUE);
            mInputEditor->selectAll();
        }
    }
    else if (gFocusMgr.childHasKeyboardFocus(this))
    {
        if (mInputEditor)
        {
            mInputEditor->deselect();
        }
        setFocus(FALSE);
    }
}

bool SLChatBar::inputEditorHasFocus() const
{
    return mInputEditor && mInputEditor->hasFocus();
}

std::string SLChatBar::getCurrentChat() const
{
    return mInputEditor ? mInputEditor->getText() : LLStringUtil::null;
}

void SLChatBar::setGestureCombo(LLComboBox* combo)
{
    mGestureCombo = combo;
    if (mGestureCombo)
    {
        mGestureCombo->setCommitCallback(boost::bind(&SLChatBar::onCommitGesture, this, _1));

        // now register observer since we have a place to put the results
        mObserver = new SLChatBarGestureObserver(this);
        LLGestureMgr::instance().addObserver(mObserver);

        // refresh list from current active gestures
        refreshGestures();
    }
}

//-----------------------------------------------------------------------
// Internal functions
//-----------------------------------------------------------------------

void SLChatBar::sendChat( EChatType type, bool ooc )
{
    if (mInputEditor)
    {
        LLWString text = mInputEditor->getConvertedText();
        LLWStringUtil::trim(text);
        LLWStringUtil::replaceChar(text, 182, '\n'); // Convert paragraph symbols back into newlines.
        // <GMDX> IM mode: to the person picked in the chat log, not nearby
        if (mIMTarget.notNull() && !text.empty())
        {
            mInputEditor->updateHistory();
            if (ooc) text = FSCommon::wrapOOC(text);
            sendIM(text);
            text.clear();
        }
        // </GMDX>
        if (!text.empty())
        {
            // store sent line for Ctrl+Up/Down recall, duplicates get filtered
            mInputEditor->updateHistory();
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
            LLFloaterIMNearbyChat::stripChannelNumber(text, &channel, &is_set);
            const S32 default_channel = LLFloaterIMNearbyChat::getDefaultChatChannel();
            if (!is_set)
            {
                channel = default_channel;
            }
            // </FS>

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
                if (!LLGestureMgr::instance().triggerAndReviseString(utf8text, &utf8_revised_text))
                {
                    utf8_revised_text = utf8text;
                }
            }
            else
            {
                utf8_revised_text = utf8text;
            }

            utf8_revised_text = utf8str_trim(utf8_revised_text);

            type = LLFloaterIMNearbyChat::processChatTypeTriggers(type, utf8_revised_text);

            if (!utf8_revised_text.empty() && !SLChatCommand::parseCommand(utf8_revised_text))
            {
                // Chat with animation
                LLFloaterIMNearbyChat::sendChatFromViewer(utf8_revised_text, type, gSavedSettings.getBOOL("PlayChatAnim"), default_channel);
            }
        }

        mInputEditor->setText(LLStringExplicit(""));
    }

    gAgent.stopTyping();

    // If the user wants to stop chatting on hitting return, lose focus
    // and go out of chat mode.
    if (gSavedSettings.getBOOL("CloseChatBarOnReturn"))
    {
        stopChat();
    }
}

void SLChatBar::changeChannelLabel(S32 channel)
{
    if (mInputEditor)
    {
        // <GMDX> IM mode names who it goes to
        mInputEditor->setLabel(mIMTarget.notNull() ? "To " + mIMName : LLFloaterIMNearbyChat::getChannelLabel(channel));
    }
}

// <FS> Channel selector: the spinner sits between the input line and the gear button
void SLChatBar::updateChannelSelector()
{
    LLView* spinner = findChild<LLView>("ChatChannel");
    LLView* gear = findChild<LLView>("chatbar_gear_menu");
    if (!spinner || !gear || !mInputEditor)
    {
        return;
    }
    const bool show = gSavedSettings.getBOOL("FSShowChatChannel");
    // <GMDX> with the chat log on, its gear is in the log's top-right corner: the line takes this one's room
    const bool log = GMDXChatLog::active();
    gear->setVisible(!log);
    const S32 right_limit = log ? gear->getRect().mRight : gear->getRect().mLeft - 2;
    // </GMDX>
    LLRect editor_rect = mInputEditor->getRect();
    if (show)
    {
        const S32 width = spinner->getRect().getWidth();
        LLRect spinner_rect = spinner->getRect();
        spinner_rect.setLeftTopAndSize(right_limit - width, editor_rect.mTop, width, editor_rect.getHeight());
        spinner->setShape(spinner_rect);
        editor_rect.mRight = spinner_rect.mLeft - 2;
    }
    else
    {
        editor_rect.mRight = right_limit;
    }
    // <GMDX> IM mode: its x (back to nearby chat) at the line's right end
    if (LLView* cancel = findChild<LLView>("im_cancel"))
    {
        const bool im = mIMTarget.notNull();
        cancel->setVisible(im);
        if (im)
        {
            LLRect cancel_rect;
            cancel_rect.setLeftTopAndSize(editor_rect.mRight - 22, editor_rect.mTop, 22, editor_rect.getHeight());
            cancel->setShape(cancel_rect);
            editor_rect.mRight -= 24;
        }
    }
    // </GMDX>
    mInputEditor->setShape(editor_rect);
    spinner->setVisible(show);
}
// </FS>

//-----------------------------------------------------------------------
// Static functions
//-----------------------------------------------------------------------

// static
void SLChatBar::applyShiftEnterNewline()
{
    // ChatShiftEnterNewline: the editor consumes Shift+Enter as a line break, so it never
    // reaches handleKeyHere (same behaviour as the nearby chat floater)
    static LLCachedControl<bool> shift_enter_newline(gSavedSettings, "ChatShiftEnterNewline", false);
    if (mInputEditor)
    {
        mInputEditor->setShiftEnterNewline(shift_enter_newline);
    }
}

void SLChatBar::startChat(const char* line)
{
    SLChatBar* bar = LLFloaterReg::getTypedInstance<SLChatBar>("chatbar");
    bar->applyShiftEnterNewline();
    // <GMDX> the chat log's font while it is on (gmdxchatlog.h), else the chat font
    const LLFontGL* log_font = GMDXChatLog::chatBarFont();
    bar->mInputEditor->setFont(log_font ? log_font : LLViewerChat::getChatFont());
    // </GMDX>
    bar->setVisible(TRUE);
    bar->setFocus(TRUE);
    bar->mInputEditor->setFocus(TRUE);

    if (line)
    {
        std::string line_string(line);
        bar->mInputEditor->setText(line_string);
    }
    // always move cursor to end so users don't obliterate chat when accidentally hitting WASD
    bar->mInputEditor->endOfDoc();
}


// Exit "chat mode" and do the appropriate focus changes
// static
void SLChatBar::stopChat()
{
    SLChatBar* bar = LLFloaterReg::getTypedInstance<SLChatBar>("chatbar");
    bar->mInputEditor->setFocus(FALSE);
    bar->setVisible(FALSE);
    bar->cancelIM();   // <GMDX> (closing ends IM mode)
    gAgent.stopTyping();
}

// <GMDX> IM mode (gmdxchatlog.cpp: a name clicked)
// static
void SLChatBar::startIM(const LLUUID& id, const std::string& name)
{
    if (id.isNull() || id == gAgentID)
    {
        return;
    }
    // [RLVa:KB] as LLAvatarActions::startIM
    if (!RlvActions::canStartIM(id))
    {
        make_ui_sound("UISndInvalidOp");
        RlvUtil::notifyBlocked(RlvStringKeys::Blocked::StartIm, LLSD().with("RECIPIENT", LLSLURL("agent", id, "completename").getSLURLString()));
        return;
    }
    startChat(nullptr);
    SLChatBar* bar = LLFloaterReg::getTypedInstance<SLChatBar>("chatbar");
    LLAvatarName av_name;
    bar->mIMTarget = id;
    bar->mIMName = LLAvatarNameCache::get(id, &av_name) ? av_name.getDisplayName() : name;
    bar->changeChannelLabel(0);
    bar->updateChannelSelector();
    gAgent.stopTyping();   // (no nearby typing animation for an IM)
}

void SLChatBar::cancelIM()
{
    if (mIMTarget.isNull())
    {
        return;
    }
    mIMTarget.setNull();
    mIMName.clear();
    changeChannelLabel(LLFloaterIMNearbyChat::getDefaultChatChannel());
    updateChannelSelector();
}

void SLChatBar::sendIM(const LLWString& text)
{
    // as LLFloaterIMSession::sendMsgFromInputEditor / sendMsg for a P2P session
    std::string utf8_text = wstring_to_utf8str(text);
    utf8_text = FSCommon::applyAutoCloseOoc(utf8_text);
    utf8_text = FSCommon::applyMuPose(utf8_text);
    utf8_text = utf8str_truncate(utf8_text, MAX_MSG_BUF_SIZE - 1);
    if (!RlvActions::canSendIM(mIMTarget))
    {
        utf8_text = RlvStrings::getString(RlvStringKeys::Blocked::SendIm);
    }
    // the conversation, new or the one there is (Conversations lists it; nothing is opened)
    const LLUUID session_id = gIMMgr->addSession(mIMName, IM_NOTHING_SPECIAL, mIMTarget);
    if (session_id.notNull())
    {
        LLIMModel::sendMessage(utf8_text, session_id, mIMTarget, IM_NOTHING_SPECIAL);
    }
}
// </GMDX>

// static
void SLChatBar::updateChatFont()
{
    SLChatBar* bar = LLFloaterReg::getTypedInstance<SLChatBar>("chatbar");
    if (bar)
    {
        bar->mInputEditor->setFont(LLViewerChat::getChatFont());
    }
}

void SLChatBar::onInputEditorKeystroke(LLTextEditor* caller)
{
    LLWString raw_text;
    if (mInputEditor) raw_text = mInputEditor->getWText();

    // Can't trim the end, because that will cause autocompletion
    // to eat trailing spaces that might be part of a gesture.
    LLWStringUtil::trimHead(raw_text);

    S32 length = narrow(raw_text.length());

    if( (length > 0)
        && (raw_text[0] != '/')     // forward slash is used for escape (eg. emote) sequences
            && (raw_text[0] != ':') // colon is used in for MUD poses
            && (LLFloaterIMNearbyChat::getDefaultChatChannel() == 0) // <FS> no typing animation off channel 0
            && mIMTarget.isNull() // <GMDX> nor for an IM
      )
    {
        gAgent.startTyping();
    }
    else
    {
        gAgent.stopTyping();
    }

    KEY key = gKeyboard->currentKey();

    // Ignore "special" keys, like backspace, arrows, etc.
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

        //LL_INFOS() << "GESTUREDEBUG " << trigger
        //  << " len " << length
        //  << " outlen " << out_str.getLength()
        //  << LL_ENDL;
    }
}

// static
void SLChatBar::onInputEditorFocusLost()
{
    // stop typing animation
    gAgent.stopTyping();
}

// static
void SLChatBar::onInputEditorGainFocus()
{

}

void SLChatBar::onCommitGesture(LLUICtrl* ctrl)
{
    LLCtrlListInterface* gestures = mGestureCombo ? mGestureCombo->getListInterface() : NULL;
    if (gestures)
    {
        S32 index = gestures->getFirstSelectedIndex();
        if (index == 0)
        {
            return;
        }
        const std::string& trigger = gestures->getSelectedValue().asString();

        // pretend the user chatted the trigger string, to invoke
        // substitution and logging.
        std::string text(trigger);
        std::string revised_text;
        LLGestureMgr::instance().triggerAndReviseString(text, &revised_text);

        revised_text = utf8str_trim(revised_text);
        if (!revised_text.empty() && !SLChatCommand::parseCommand(revised_text))
        {
            // Don't play nodding animation
            LLFloaterIMNearbyChat::sendChatFromViewer(revised_text, CHAT_TYPE_NORMAL, FALSE);
        }
    }
    mGestureLabelTimer.start();
    if (mGestureCombo != nullptr)
    {
        // free focus back to chat bar
        mGestureCombo->setFocus(FALSE);
    }
}

// #1017: hiding the UI lets the floater area grow over the toolbars, and floaters that follow the
// bottom edge move down with it; when the toolbars come back the area shrinks a frame or more
// later, after any one-shot fix-up, and the bar is left half below the bottom edge. The bottom
// chat bar must always be fully reachable, so keep it inside the floater area every frame (a
// no-op once it is inside). Not for a bar hosted inside another window.
void SLChatBar::draw()
{
    // <GMDX> With the chat log on, the bar moves and resizes with it (its lock), not on its own; once
    // the panel has been moved, the bar is right under it, as wide as it (the panel keeps that place
    // inside the floater area; while dragged, GMDXChatLog places the bar as the mouse moves).
    const bool owned = GMDXChatLog::active() && gFloaterView && getParent() == gFloaterView;
    if (owned != mDocked)
    {
        mDocked = owned;
        setCanDrag(!owned);
        setCanResize(!owned);
        if (LLView* grip = findChild<LLView>("drag")) grip->setVisible(!owned);
    }
    LLRect docked;
    const bool dock = owned && !isMinimized() && GMDXChatLog::dockedBarRect(gFloaterView, docked);
    if (dock && docked != getRect()) setShape(docked);
    // The chat log panel's bottom row: its fill and edge (no skin box, floater_sl_chatbar.xml), no
    // top edge while the panel sits on it (the panel draws the rule between them).
    {
        static LLUIColor fill = LLUIColorTable::instance().getColor("GMDXChatBarBackground");
        static LLUIColor edge = LLUIColorTable::instance().getColor("GMDXChatBarEdge");
        const S32 r = getRect().getWidth() - 1, t = getRect().getHeight() - 1;
        gl_rect_2d(getLocalRect(), fill.get());
        gl_line_2d(0, t, 0, 0, edge.get());
        gl_line_2d(0, 0, r, 0, edge.get());
        gl_line_2d(r, 0, r, t, edge.get());
        if (!GMDXChatLog::panelOnBar()) gl_line_2d(0, t, r, t, edge.get());
    }
    if (dock)
    {
        LLFloater::draw();
        return;
    }
    // </GMDX>
    // <GMDX> not while something is dragged: the toolbars that show during a drag (empty ones to drop
    // on) shrink the area for the drag only, and a fit would be kept (a too wide bar made narrower)
    if (gFloaterView && getParent() == gFloaterView && !isMinimized()
        && !LLToolDragAndDrop::getInstance()->hasMouseCapture())
    // </GMDX>
    {
        const LLRect area = gFloaterView->getLocalRect();   // <GMDX> the window, not the snap region (left of it: the toolbar strips)
        const LLRect& r = getRect();
        if (r.mBottom < area.mBottom || r.mTop > area.mTop || r.mLeft < area.mLeft || r.mRight > area.mRight)
        {
            gFloaterView->adjustToFitScreen(this, false);
        }
    }
    LLFloater::draw();
}

// <GMDX> the chat log's place back to over this bar, and this bar back to where it first opens
void SLChatBar::resetChatLogPosition()
{
    gSavedSettings.setRect("GMDXChatLogRect", LLRect());
    LLFloater::getControlGroup()->setRect(LLFloaterReg::getRectControlName("chatbar"), LLRect());
    setShape(mDefaultRect);
    if (gFloaterView && getParent() == gFloaterView) gFloaterView->adjustToFitScreen(this, false);
}
// </GMDX>

// [SL:KB] - Patch: Chat-NearbyToastWidth | Checked: 2010-11-10 (Catznip-2.4)
// virtual
void SLChatBar::reshape(S32 width, S32 height, bool called_from_parent)
{
    LLFloater::reshape(width, height, called_from_parent);

    if (mReshapeSignal)
    {
        (*mReshapeSignal)(this, width, height);
    }
}

boost::signals2::connection SLChatBar::setReshapeCallback(const reshape_signal_t::slot_type& cb)
{
    if (!mReshapeSignal)
    {
        mReshapeSignal = new reshape_signal_t();
    }
    return mReshapeSignal->connect(cb);
}
// [/SL:KB]
