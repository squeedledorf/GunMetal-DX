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
// Names take the group colour (LLGroupColorMap, as the mini-map), else the contact set's. Channels
// have colours and tags (SYS, OBJ, SHOUT, FROM / TO, GROUP). One speaker's lines in a row merge
// under one name; an object repeating a line shows it once with a count. Group chat shows in All
// for the active group only; IMs show in All; combat events (the kill feed's) only in Combat.

#pragma once

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

    // The chat bar's font while the log is on (Barlow at the log's size); null: the chat font.
    static const LLFontGL* chatBarFont();

    // Nearby chat (LLFloaterIMNearbyChatHandler::processChat, where a toast would show).
    static void addNearby(const LLChat& chat);
    // A combat event: text segments, built when drawn (names resolve after the event).
    struct Segment { std::string text; LLColor4 color; };
    static void addCombat(std::function<std::vector<Segment>()> segments);

    void draw() override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
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
    };
    struct Row
    {
        std::vector<std::pair<F32, Segment>> segs;   // x offset, text
        std::vector<const LLFontGL*> fonts;
    };

    void add(Entry e);
    void onIM(const LLSD& msg);
    bool inTab(const Entry& e, S32 tab) const;
    // head: show the name even on a merged line (its first line is not drawn above it)
    void layout(const Entry& e, F32 width, bool show_time, std::vector<Row>& rows, bool head = false) const;
    void loadFonts();
    LLRect barRect() const;
    bool typing() const;
    LLRect tabRect(S32 tab) const;
    LLRect gearRect() const;   // the chat bar's options, in the tab row's right corner

    static GMDXChatLog* sInstance;
    std::deque<Entry> mEntries;
    S32 mTab = TAB_ALL;
    S32 mScroll = 0;            // rows scrolled back in the panel
    LLRect mPanelRect;          // while typing (this view's coordinates)
    // (looked up every draw: a font set change replaces the fonts)
    const LLFontGL* mText = nullptr;
    const LLFontGL* mBold = nullptr;
    const LLFontGL* mItalic = nullptr;
    const LLFontGL* mTag = nullptr;
    const LLFontGL* mTagBold = nullptr;
    boost::signals2::connection mIMConnection;
};
