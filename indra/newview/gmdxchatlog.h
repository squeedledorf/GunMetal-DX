/**
 * @file gmdxchatlog.h
 * @brief GunMetal-DX: the MMO-style chat log in the bottom-left corner (NearbyChatOverlayStyle "gmdx").
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
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
 * $/LicenseInfo$
 */

// <GMDX> One chat log in place of the nearby chat toasts, drawn over the world above the chat bar:
//  - at rest: the newest lines (GMDXChatLogLines) on no background, with a soft shadow, each fading
//    out GMDXChatLogPersist seconds after it arrived;
//  - typing (the chat bar has focus): a gunmetal panel over the chat bar with tabs (All / Local /
//    Group / IM / Combat, unread counts), timestamps and the history (mouse wheel scrolls);
//  - combat (mouselook / over the shoulder, or aiming down sights): the 3 newest lines, faint; a new
//    line shows at full strength for a few seconds.
// The lock left of the panel's gear unlocks it: the panel then stays up with a frame and handles, is
// dragged to move and by its edges / corners to resize (GMDXChatLogRect, kept inside the window; the
// gear menu resets it). The lines at rest and in combat then sit in the panel's place. The chat bar is
// part of it: once the panel has been moved, the bar sits right under it, as wide as it (SLChatBar::draw
// asks dockedBarRect), the frame and handles take in both, and the panel stays where the bar fits.
// Names take the group colour (LLGroupColorMap, as the mini-map), else the contact set's. Channels
// have colours and tags (SYS, OBJ, SHOUT, FROM / TO, GROUP). One speaker's lines in a row merge
// under one name; an object repeating a line shows it once with a count. Group chat shows in All
// for the active group only; IMs show in All; combat events (the kill feed's) only in Combat.
// The panel keeps as much as the Conversations floater's nearby chat (20000 lines) and, scrolled to
// the top of All or Local, pages older nearby chat in from the same log (LogShowHistory). Only the
// rows in view are laid out each frame. Scrolled up more than a panelful, a button at its bottom right
// jumps back to the newest line, with the count of lines said since that are below the view.
// Names in the panel (locked) are links: a click sets the chat bar up to IM them (SLChatBar::startIM),
// a right click opens the people menu (an avatar) or the chat history's object menu (an object).

#pragma once

#include "lllogchat.h"
#include "llview.h"
#include "v4color.h"

#include <deque>
#include <functional>
#include <string>
#include <vector>

class LLChat;
class LLFontGL;

class GMDXChatLog : public LLView
{
public:
    // LLViewerWindow: the log, a child of the root view over the whole window.
    static GMDXChatLog* create();
    static GMDXChatLog* instance() { return sInstance; }

    // NearbyChatOverlayStyle is "gmdx".
    static bool active();

    // The log is showing and puts this session's IMs in All (an IM or conference, or the active
    // group's chat): LLIMHandler shows no toast for them.
    static bool showsIM(const LLUUID& session_id);

    // The chat bar's place under the moved panel, in parent's coordinates; false: the panel has not
    // been moved (the bar is where the user put it, the panel goes over it).
    static bool dockedBarRect(const LLView* parent, LLRect& rect);
    // Where the unit (the panel and the chat bar under it) is, also while it is dragged and at rest
    // with no panel showing, in other's coordinates; false: the log is off. The toolbars' buttons
    // keep out of it (gmdxtoolbarcluster.h).
    static bool unitPlace(const LLView* other, LLRect& rect);
    // The panel showed last frame right on top of the chat bar, as wide as it: the bar draws no top
    // edge (the panel's bottom is the rule between them).
    static bool panelOnBar() { return sInstance && sInstance->mOnBar; }

    // The chat bar's font while the log is on (Barlow at the log's size); null: the chat font.
    static const LLFontGL* chatBarFont();

    // Nearby chat (LLFloaterIMNearbyChatHandler::processChat, where a toast would show).
    static void addNearby(const LLChat& chat);
    // A combat event: text segments, built when drawn (names resolve after the event).
    struct Segment { std::string text; LLColor4 color; };
    static void addCombat(std::function<std::vector<Segment>()> segments);

    void draw() override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleRightMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    void onMouseCaptureLost() override;   // a drag ends: its place is kept
    bool handleScrollWheel(S32 x, S32 y, S32 clicks) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleToolTip(S32 x, S32 y, MASK mask) override { return false; }

    enum class Channel { Say, Shout, Whisper, Object, System, Group, IM, Combat };
    enum Tab { TAB_ALL, TAB_LOCAL, TAB_GROUP, TAB_IM, TAB_COMBAT, TAB_COUNT };

protected:
    GMDXChatLog(const LLView::Params& p);
    ~GMDXChatLog();
    friend class LLUICtrlFactory;

private:
    struct Entry
    {
        F64 time = 0.0;             // LLFrameTimer::getTotalSeconds
        time_t clock = 0;           // wall clock, for the timestamp
        Channel channel = Channel::Say;
        LLUUID from;
        LLUUID session;             // an IM or group chat line's conversation (null: nearby, combat)
        LLUUID owner;               // an object's owner, and where it spoke (its menu)
        std::string slurl;
        std::string name;           // "" = no name shown
        std::string tag;            // "" = none (say)
        std::string text;
        mutable std::string shown;          // text with its agent / group links as names (layout)
        mutable bool links_done = false;    // every link in shown resolved: not looked up again
        bool emote = false;         // "/me": name then text, no colon
        bool in_all = true;         // shown in the All tab and at rest
        bool merged = false;        // same speaker as the line before: name hidden
        U32 repeat = 1;             // an object repeating itself
        bool seen = false;          // drawn (at rest or in the panel): not counted as unread
        std::function<std::vector<Segment>()> segments;   // combat lines
        U64 seq = 0;                // order said in (0: from the log)
        bool history = false;       // from the log, said before this session: never at rest
        std::string stamp;          // the log's time ("" = from clock)
        mutable S32 rows_n = 0;     // rows in the panel, laid out at rows_w with rows_font (0 = not known)
        mutable F32 rows_w = 0.f;
        mutable const LLFontGL* rows_font = nullptr;
    };
    struct Row
    {
        std::vector<std::pair<F32, Segment>> segs;   // x offset, text
        std::vector<const LLFontGL*> fonts;
        const Entry* entry = nullptr;   // (valid while the frame that laid it out draws)
        S32 name_seg = -1;              // the segment holding the name, if this row shows it
    };
    // a name drawn in the panel this frame, and who it stands for
    struct NameHit
    {
        LLRect rect;
        LLUUID id;
        std::string name;
        bool object = false;
        LLUUID owner;
        std::string slurl;
        LLColor4 color;
    };

    void add(Entry e);
    static Entry fromChat(const LLChat& chat);
    S32 rowCount(const Entry& e, F32 width) const;   // in the panel; laid out only when not known
    void keepRowCount(const Entry& e, F32 width, size_t rows) const;
    void countUnread(const Entry& e, S32 delta);
    void markSeen(Entry& e);
    void markTabSeen(S32 tab);   // the tab opened: what it holds counts as read
    void takeHistory();
    void requestHistory();
    void onHistoryPage(const LLChatLogPage& page);
    void onIM(const LLSD& msg);
    bool inTab(const Entry& e, S32 tab) const;
    // head: show the name even on a merged line (its first line is not drawn above it)
    void layout(const Entry& e, F32 width, bool show_time, std::vector<Row>& rows, bool head = false) const;
    void loadFonts();
    LLRect barRect() const;
    bool typing() const;
    LLRect tabRect(S32 tab) const;
    LLRect gearRect() const;   // the chat bar's options, in the tab row's right corner
    LLRect lockRect() const;   // left of the gear
    // the panel's place: over the chat bar, as wide as it, or the saved one (GMDXChatLogRect)
    LLRect defaultPanelRect(const LLRect& bar, S32 line_h) const;
    LLRect savedRect() const;                  // empty: none saved
    LLRect keepInView(LLRect r) const;         // r moved (shrunk if need be) into panelArea
    LLRect panelArea() const;                  // the floater view (edge to edge), less the chat bar's height at the bottom
    S32 barHeight() const;
    LLRect unitRect() const;                   // the panel and, while the chat bar shows, its place under it
    S32 minWidth() const;                      // the tabs, the lock and the gear fit
    S32 minHeight() const;                     // the tab row and 3 lines
    U32 grabEdges(S32 x, S32 y) const;         // unlocked: the edges a press there resizes (0: it moves)
    bool onBarControl(S32 x, S32 y) const;     // unlocked: one of the chat bar's controls, not an edge, is there
    void lock();
    bool nameTarget(const Entry& e, NameHit& hit) const;   // false: the name is not a link (system, you, RLVa)
    const NameHit* hitAt(S32 x, S32 y) const;
    void showObjectMenu(const NameHit& hit, S32 x, S32 y);
    void placeBar() const;                     // the chat bar under the panel now (while dragged)

    static GMDXChatLog* sInstance;
    std::deque<Entry> mEntries;
    S32 mTab = TAB_ALL;
    S32 mScroll = 0;            // rows scrolled back in the panel
    U64 mSeq = 0;               // the newest entry's seq
    U64 mDrawnSeq = 0;          // the newest entry when the panel last drew (newer: said since)
    S32 mUnread[TAB_COUNT] = {};   // entries never drawn, per tab
    bool mPanelShown = false;   // the panel drew last frame
    bool mOnBar = false;        // ... on top of the chat bar (panelOnBar)
    // older nearby chat: the log the Conversations floater reads, frozen when it opened
    LLChatLogSource::ptr_t mHistory;
    LLChatLogCursor mHistoryCursor;   // the oldest line paged in so far
    bool mHistoryTaken = false, mHistoryLoading = false, mHistoryDone = false;
    bool mDropped = false;      // the oldest entries were dropped (the cap): the log no longer joins on
    LLRect mPanelRect;          // while typing or unlocked (this view's coordinates)
    LLRect mUnitPlace;          // the panel's place and the bar's, shown or not (unitPlace)
    LLRect mJumpRect;           // "jump to latest", while scrolled up more than the panel shows
    std::vector<NameHit> mHits; // the panel's names this frame (none while unlocked)
    LLSD mObjectMenuData;       // the object the object menu is for
    LLHandle<LLView> mObjectMenu;
    bool mUnlocked = false;     // the panel can be moved and resized (not kept between sessions)
    bool mDragging = false;
    U32 mDragEdges = 0;         // EDGE_* being dragged, 0: the whole panel
    S32 mDragX = 0, mDragY = 0; // where the drag started
    LLRect mDragFrom, mDragRect;
    S32 mLineH = 0;             // the last draw's line height
    // (looked up every draw: a font set change replaces the fonts)
    const LLFontGL* mText = nullptr;
    const LLFontGL* mBold = nullptr;
    const LLFontGL* mItalic = nullptr;
    const LLFontGL* mTag = nullptr;
    const LLFontGL* mTagBold = nullptr;
    boost::signals2::connection mIMConnection;
};
