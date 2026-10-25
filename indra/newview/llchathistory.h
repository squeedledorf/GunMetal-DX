/**
 * @file llchathistory.h
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

#ifndef LLCHATHISTORY_H_
#define LLCHATHISTORY_H_

#include "lltexteditor.h"
#include "lltextbox.h"
#include "llviewerchat.h"
#include "lllogchat.h"

#include <deque>
#include <unordered_set>

// What a windowed chat history (#369) shows: the conversation's live messages, held in
// memory by its model, and its earlier sessions, read from the log a page at a time.
class LLChatHistorySource
{
public:
    virtual ~LLChatHistorySource() = default;
    // The live session's messages, oldest first. getLiveMessage() fills the message as it is
    // shown, with its own appendMessage() args (merged over getAppendArgs()); false for a
    // message that is not shown (muted, an offer whose notification is gone).
    virtual S32  getLiveMessageCount() const = 0;
    virtual bool getLiveMessage(S32 index, LLChat& chat, LLSD& args) = 0;
    // The conversation's log, null when history is not shown
    virtual LLChatLogSource::ptr_t getHistoryLog() const = 0;
    virtual void chatFromLogEntry(const LLChatLogEntry& entry, LLChat& chat) const = 0;
    // appendMessage() args every message gets (plain text, timestamps, names)
    virtual LLSD getAppendArgs() const = 0;
};

//Chat log widget allowing addition of a message as a widget
class LLChatHistory : public LLUICtrl
{
    public:
        struct Params : public LLInitParam::Block<Params, LLTextEditor::Params>
        {
            //Message header filename
            Optional<std::string>   message_header;
            //Message separator filename
            Optional<std::string>   message_separator;
            //Text left padding from the scroll rect
            Optional<S32>           left_text_pad;
            //Text right padding from the scroll rect
            Optional<S32>           right_text_pad;
            //Widget left padding from the scroll rect
            Optional<S32>           left_widget_pad;
            //Widget right padding from the scroll rect
            Optional<S32>           right_widget_pad;
            //Separator top padding
            Optional<S32>           top_separator_pad;
            //Separator bottom padding
            Optional<S32>           bottom_separator_pad;
            //Header top padding
            Optional<S32>           top_header_pad;
            //Header bottom padding
            Optional<S32>           bottom_header_pad;

            Optional<LLTextBox::Params> more_chat_text;

            Optional<bool>          notify_unread_msg;

            Params()
            :   message_header("message_header"),
                message_separator("message_separator"),
                left_text_pad("left_text_pad"),
                right_text_pad("right_text_pad"),
                left_widget_pad("left_widget_pad"),
                right_widget_pad("right_widget_pad"),
                top_separator_pad("top_separator_pad"),
                bottom_separator_pad("bottom_separator_pad"),
                top_header_pad("top_header_pad"),
                bottom_header_pad("bottom_header_pad"),
                more_chat_text("more_chat_text"),
                notify_unread_msg("notify_unread_msg", true)
            {}

        };
    protected:
        LLChatHistory(const Params&);
        friend class LLUICtrlFactory;

        /*virtual*/ void draw();
        /**
         * Redefinition of LLTextEditor::updateTextRect() to considerate text
         * left/right padding params.
         */
        //virtual void  updateTextRect();
        /**
         * Builds a message separator.
         * @return pointer to LLView separator object.
         */
        LLView* getSeparator();

        void onClickMoreText();

    private:
        /**
         * Builds a message header.
         * @return pointer to LLView header object.
         */
        LLView* getHeader(const LLChat& chat,const LLStyle::Params& style_params, const LLSD& args);
    public:
        ~LLChatHistory();
        LLSD getValue() const;
        void initFromParams(const Params&);

        /**
         * Appends a widget message.
         * If last user appended message, concurs with current user,
         * separator is added before the message, otherwise header is added.
         * The args LLSD contains:
         * - use_plain_text_chat_history (bool) - whether to add message as plain text.
         * - owner_id (LLUUID) - the owner ID for object chat
         * @param chat - base chat message.
         * @param args - additional arguments
         * @param input_append_params - font style.
         */
        void appendMessage(const LLChat& chat, const LLSD &args = LLSD(), const LLStyle::Params& input_append_params = LLStyle::Params());
        LLTextEditor* getEditor() const { return mEditor; } // <FS> search in chat history
        /*virtual*/ void clear();

        // #369 windowed history (SLChatWindowedHistory). The document holds a window of at most
        // SLChatWindowMessages messages of the conversation, pulled from the source; nothing is
        // appended to it directly. Scrolling near the top pages older messages in (live ones
        // from memory, earlier sessions from the log, off the main thread) and drops as many
        // from the bottom; near the bottom, newer ones come back. What the viewport shows stays
        // put while the window moves. The source unsets itself before it goes away.
        void setWindowSource(LLChatHistorySource* source);
        bool isWindowed() const { return mSource != nullptr; }
        // The source has new live messages: shown when the window is at the live end
        void onLiveMessagesAdded();
        // Shows the same messages again (a style setting changed); reset: the newest ones, the
        // log read again (the model was rebuilt or reordered)
        void reloadWindow(bool reset);
        void jumpToLiveEnd();
        // Find in chat over the whole conversation: the next match in the window, else the
        // window moves to the next match in the model (live messages, then the log)
        void findNext(const std::string& text, bool case_insensitive, bool search_up);

    private:
        // A pooled header or separator held by one window entry. Move-only, and a move empties
        // the source: a view has exactly one holder (or is in the free lists), so a moved-from
        // entry can never release it a second time.
        struct PooledViewRef
        {
            LLView* mView = nullptr;

            PooledViewRef() = default;
            PooledViewRef(const PooledViewRef&) = delete;
            PooledViewRef& operator=(const PooledViewRef&) = delete;
            PooledViewRef(PooledViewRef&& other) noexcept : mView(other.mView) { other.mView = nullptr; }
            PooledViewRef& operator=(PooledViewRef&& other) noexcept
            {
                if (this != &other)
                {
                    llassert_msg(!mView, "pooled chat view overwritten while held");
                    mView = other.mView;
                    other.mView = nullptr;
                }
                return *this;
            }
            PooledViewRef& operator=(LLView* view) { mView = view; return *this; }
            operator LLView*() const { return mView; }
            LLView* operator->() const { return mView; }
        };

        struct WindowEntry
        {
            bool             mFromLog = false;
            S32              mLiveIndex = -1;       // live message index in the source
            LLChatLogCursor  mLogBegin;             // log message: where it is in the log
            LLChatLogCursor  mLogEnd;
            LLChat           mChat;                 // as shown
            LLSD             mArgs;                 // its own appendMessage() args
            LLTextSegmentPtr mMarker;               // its first segment (header widget or separating
                                                    // line break), whose start follows edits; null
                                                    // for a plain text message starting the document
            PooledViewRef    mPooledView;           // its header or separator
            bool             mIsSeparator = false;  // continues the previous message (no header)
            F64              mShownAt = 0.0;        // header or separator: as appendMessage decides it
        };
        typedef std::deque<WindowEntry> window_t;

        // what the window tells appendMessage() about the message being rendered
        struct RenderContext
        {
            const WindowEntry*  mPrevious = nullptr;    // the message before it in the document
            bool                mLive = false;          // a new live message at the end
            bool                mForceHeader = false;
            WindowEntry*        mEntry = nullptr;
        };

        // the viewport's first line, as an entry and an offset into it
        struct WindowAnchor
        {
            LLTextBase::ScrollAnchor mScroll;
            S32 mEntry = -1;
            S32 mOffset = 0;
        };

        static S32 windowMessages();
        static S32 pageMessages();
        bool renderEntry(WindowEntry& entry, const WindowEntry* previous, bool live, bool force_header);
        S32  entryStart(size_t index) const;
        S32  entryEnd(size_t index) const;
        bool makeLiveEntry(S32 index, WindowEntry& entry);
        void makeLogEntry(const LLChatLogEntry& log, WindowEntry& entry);
        void appendEntries(std::vector<WindowEntry>& entries, bool live);
        void prependEntries(std::vector<WindowEntry>& entries);
        void appendLiveFrom(S32 from, S32 count);
        void trimTop(S32 count);
        void trimBottom(S32 count);
        void reheadFront();
        void releaseEntry(WindowEntry& entry);
        void clearWindow();
        void fillFromLiveEnd();
        void requestOlder(S32 count);
        void requestNewer(S32 count);
        void onLogPage(const LLChatLogPage& page, bool older);
        void updateWindowPaging();
        void showLoading(bool loading);
        void noteUnread(const LLChat& chat);
        WindowAnchor captureAnchor();
        void restoreAnchor(const WindowAnchor& anchor, S32 entry_shift);
        // find in chat beyond the window
        bool liveMatches(S32 index, const LLWString& needle, bool case_insensitive);
        void showLiveAt(S32 index, const std::string& text, bool case_insensitive, bool search_up);
        void findInLog(const LLChatLogCursor& from, const std::string& text, bool case_insensitive, bool search_up);
        void jumpToLog(const LLChatLogCursor& where, const std::string& text, bool case_insensitive, bool search_up);
        void selectInEntry(S32 index, const std::string& text, bool case_insensitive, bool search_up);

        // headers and separators are recycled, not built from XML per message
        LLView* acquireHeader(const LLChat& chat, const LLStyle::Params& style_params, const LLSD& args);
        LLView* acquireSeparator();
        void releaseView(LLView* view, bool separator);

        LLChatHistorySource*    mSource = nullptr;
        LLChatLogSource::ptr_t  mLog;
        window_t                mWindow;
        RenderContext*          mRenderCtx = nullptr;
        bool                    mNeedsFill = false;     // filled when first drawn
        bool                    mAtLiveEnd = true;      // the window ends with the newest live message
        bool                    mAtLogStart = true;     // nothing is older than the window
        S32                     mLiveSeen = -1;         // newest live message shown or counted as unread
        enum EPaging
        {
            PAGING_NONE,
            PAGING_OLDER,
            PAGING_NEWER,
            PAGING_JUMP
        };
        EPaging                 mPaging = PAGING_NONE;
        U32                     mPageRequest = 0;       // tells stale log callbacks apart
        class LLLayoutPanel*    mLoadingPanel = nullptr;
        std::vector<LLView*>    mFreeHeaders;
        std::vector<LLView*>    mFreeSeparators;
        // pooled views handed out and not yet released: a view is either here (held by exactly
        // one window entry) or in a free list, never both; a release of a view not in here is
        // refused instead of corrupting the free lists
        std::unordered_set<LLView*> mPooledInUse;

    private:
        std::string mLastFromName;
        LLUUID mLastFromID;
        LLDate mLastMessageTime;
        bool mIsLastMessageFromLog;
        bool mNotifyAboutUnreadMsg;
        //std::string mLastMessageTimeStr;

        std::string mMessageHeaderFilename;
        std::string mMessageSeparatorFilename;

        S32 mLeftTextPad;
        S32 mRightTextPad;

        S32 mLeftWidgetPad;
        S32 mRightWidgetPad;

        S32 mTopSeparatorPad;
        S32 mBottomSeparatorPad;
        S32 mTopHeaderPad;
        S32 mBottomHeaderPad;

        class LLLayoutPanel*    mMoreChatPanel;
        LLTextBox*      mMoreChatText;
        LLTextEditor*   mEditor;
        typedef std::set<std::string> unread_chat_source_t;
        unread_chat_source_t mUnreadChatSources;
};
#endif /* LLCHATHISTORY_H_ */
