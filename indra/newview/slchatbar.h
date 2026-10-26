/**
 * @file slchatbar.h
 * @brief SLChatBar class definition
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

#pragma once

#include "llframetimer.h"
#include "llchat.h"
#include "llfloater.h"

class LLChatEntry;
class LLMessageSystem;
class LLUICtrl;
class LLUUID;
class LLFrameTimer;
class SLChatBarGestureObserver;
class LLComboBox;
class LLTextEditor;

class SLChatBar final
:   public LLFloater
{
public:
    // constructor for inline chat-bars (e.g. hosted in chat history window)
    SLChatBar(const LLSD& key);

    bool        postBuild() override;
    void        onOpen(const LLSD& key) override;
    bool        handleKeyHere(KEY key, MASK mask) override;
    void        onFocusLost() override;

// [SL:KB] - Patch: Chat-NearbyToastWidth | Checked: 2010-11-10 (Catznip-2.4)
    /*virtual*/ void reshape(S32 width, S32 height, bool called_from_parent = true) override;

    typedef boost::signals2::signal<void (LLUICtrl* ctrl, S32 width, S32 height)> reshape_signal_t;
    boost::signals2::connection setReshapeCallback(const reshape_signal_t::slot_type& cb);
// [/SL:KB]

    void        refresh() override;
    void        draw() override;
    void        refreshGestures();

    // Move cursor into chat input field.
    void        setKeyboardFocus(bool b);

    bool        inputEditorHasFocus() const;
    std::string getCurrentChat() const;

    // since chat bar logic is reused for chat history
    // gesture combo box might not be a direct child
    void        setGestureCombo(LLComboBox* combo);

    // callbacks
    void onInputEditorKeystroke(LLTextEditor* caller);
    static void onInputEditorFocusLost();
    static void onInputEditorGainFocus();

    void onCommitGesture(LLUICtrl* ctrl);

    static void startChat(const char* line);
    static void stopChat();

    static void updateChatFont();

protected:
    ~SLChatBar();

    void sendChat(EChatType type, bool ooc = false); // <FS> ooc: wrap in FSOOCPrefix / FSOOCPostfix

    void applyShiftEnterNewline(); // #681: honour ChatShiftEnterNewline in the chat bar editor
    void changeChannelLabel(S32 channel);
    void updateChannelSelector(); // <FS> channel spinner

    LLChatEntry* mInputEditor;

    LLFrameTimer    mGestureLabelTimer;

    bool            mIsBuilt;
    LLComboBox*     mGestureCombo;

    SLChatBarGestureObserver* mObserver;

// [SL:KB] - Patch: Chat-NearbyToastWidth | Checked: 2010-11-10 (Catznip-2.4)
    reshape_signal_t*       mReshapeSignal = nullptr;
// [/SL:KB]

    boost::signals2::connection mChatChannelConnection;
    boost::signals2::connection mShowChatChannelConnection;
    boost::signals2::connection mLogStyleConnection;   // <GMDX> the chat log takes the gear (updateChannelSelector)
    boost::signals2::connection mChatFontSizeConnection;
};
