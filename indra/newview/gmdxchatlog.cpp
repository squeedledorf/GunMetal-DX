/**
 * @file gmdxchatlog.cpp
 * @brief GunMetal-DX: the MMO-style chat log in the bottom-left corner (see gmdxchatlog.h).
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

// <GMDX> whole file: see gmdxchatlog.h.

#include "llviewerprecompiledheaders.h"

#include "gmdxchatlog.h"

#include "fscombatads.h"
#include "fscommon.h"
#include "llagent.h"
#include "llagentcamera.h"
#include "llavatarnamecache.h"
#include "llcachename.h"
#include "llchat.h"
#include "llfloaterreg.h"
#include "llfontgl.h"
#include "llgroupcolormap.h"
#include "llimview.h"
#include "lllocalcliprect.h"
#include "llrender2dutils.h"
#include "lluictrlfactory.h"
#include "llviewercontrol.h"
#include "llviewerchat.h"
#include "lggcontactsets.h"
#include "slchatbar.h"
#include "rlvactions.h"
#include "rlvcommon.h"

#include <ctime>
#include <regex>

GMDXChatLog* GMDXChatLog::sInstance = nullptr;

namespace
{
constexpr size_t kHistory = 500;          // entries kept
constexpr F64 kMergeWithin = 30.0;        // s: a speaker's next line merges under the name
constexpr F64 kRepeatWithin = 60.0;       // s: an object's same line counts up
constexpr F32 kFadeOut = 3.f;             // s: the last part of a line's time at rest
constexpr S32 kCombatLines = 3;
constexpr F64 kCombatFlash = 3.0;         // s a new line is at full strength in combat
constexpr F32 kCombatAlpha = 0.35f;
constexpr S32 kPad = 10, kTabH = 24, kGap = 3;

const LLColor4 C_TEXT(0.933f, 0.945f, 0.957f, 1.f);       // #EEF1F4
const LLColor4 C_DIM(0.682f, 0.718f, 0.753f, 1.f);        // #AEB7C0
const LLColor4 C_TIME(0.490f, 0.529f, 0.573f, 1.f);       // #7D8792
const LLColor4 C_NAME(0.843f, 0.871f, 0.898f, 1.f);       // #D7DEE5 (no group / contact colour)
const LLColor4 C_SYS(0.961f, 0.843f, 0.431f, 1.f);        // #F5D76E
const LLColor4 C_OBJ(0.604f, 0.639f, 0.682f, 1.f);        // #9AA3AE
const LLColor4 C_SHOUT(1.f, 0.824f, 0.478f, 1.f);         // #FFD27A
const LLColor4 C_IM(0.906f, 0.651f, 1.f, 1.f);            // #E7A6FF
const LLColor4 C_GROUP(0.435f, 0.827f, 1.f, 1.f);         // #6FD3FF
const LLColor4 C_COUNT(0.788f, 0.827f, 0.867f, 1.f);      // #C9D3DD
const LLColor4 C_PANEL(0.063f, 0.075f, 0.090f, 0.82f);    // #101317
const LLColor4 C_EDGE(0.227f, 0.259f, 0.298f, 1.f);       // #3A424C
const LLColor4 C_RULE(0.173f, 0.200f, 0.231f, 1.f);       // #2C333B
const LLColor4 C_TAB_ON(0.165f, 0.192f, 0.224f, 1.f);     // #2A3139

const char* const kTabNames[GMDXChatLog::TAB_COUNT] = { "ALL", "LOCAL", "GROUP", "IM", "COMBAT" };

LLColor4 channel_color(GMDXChatLog::Channel c)
{
    using C = GMDXChatLog::Channel;
    switch (c)
    {
        case C::Shout:  return C_SHOUT;
        case C::Object: return C_OBJ;
        case C::System: return C_SYS;
        case C::Group:  return C_GROUP;
        case C::IM:     return C_IM;
        default:        return C_TEXT;
    }
}

// The mini-map's order: the group colour, then the contact set's, else the plain name colour.
LLColor4 name_color(const LLUUID& id)
{
    if (id.isNull()) return C_NAME;
    LLColor4 c = C_NAME;
    LGGContactSets::instance().hasFriendColorThatShouldShow(id, ContactSetType::MINIMAP, c);
    return LLGroupColorMap::instance().tint(id, c);
}

LLColor4 faded(const LLColor4& c, F32 a) { return LLColor4(c.mV[VRED], c.mV[VGREEN], c.mV[VBLUE], c.mV[VALPHA] * a); }

// The text with secondlife:///app/agent/<id>/<verb> and .../app/group/<id>/<verb> links as the names
// they stand for (what the chat history shows: an object announcing "<link> eliminated"), each
// name asked for once if not cached yet. done: every link resolved (the text can be kept).
std::string resolve_links(const std::string& text, bool& done)
{
    done = true;
    if (text.find("secondlife:///app/") == std::string::npos) return text;
    static const std::regex link(R"(secondlife:///app/(agent|group)/([0-9a-fA-F-]{36})/([A-Za-z]+))");
    std::string out;
    auto last = text.cbegin();
    for (std::sregex_iterator it(text.begin(), text.end(), link), end; it != end; ++it)
    {
        const std::smatch& m = *it;
        out.append(last, m[0].first);
        last = m[0].second;
        const LLUUID id(m[2].str());
        const std::string verb = m[3].str();
        std::string name;
        if (m[1] == "agent")
        {
            if (!RlvActions::canShowName(RlvActions::SNC_DEFAULT, id))
            {
                name = RlvStrings::getAnonym(LLStringUtil::null);
            }
            else
            {
                LLAvatarName av;
                if (LLAvatarNameCache::get(id, &av))   // (asks for it when not cached)
                {
                    name = verb == "username" ? av.getUserName() : verb == "displayname" ? av.getDisplayName()
                                                                                        : av.getCompleteName();
                }
            }
        }
        else if (gCacheName)
        {
            gCacheName->getGroupName(id, name);   // (asks for it when not cached)
        }
        if (name.empty())
        {
            done = false;
            name = "...";
        }
        out += name;
    }
    out.append(last, text.cend());
    return out;
}
} // namespace

GMDXChatLog::GMDXChatLog(const LLView::Params& p) : LLView(p)
{
    sInstance = this;
    mIMConnection = LLIMModel::getInstance()->addNewMsgCallback([this](const LLSD& msg) { onIM(msg); });
}

GMDXChatLog::~GMDXChatLog()
{
    mIMConnection.disconnect();
    if (sInstance == this) sInstance = nullptr;
}

// static
GMDXChatLog* GMDXChatLog::create()
{
    LLView::Params p;
    p.name("gmdx_chat_log");
    p.mouse_opaque(false);
    p.follows.flags(FOLLOWS_ALL);
    return LLUICtrlFactory::create<GMDXChatLog>(p);
}

// static
bool GMDXChatLog::active()
{
    static LLCachedControl<std::string> style(gSavedSettings, "NearbyChatOverlayStyle", "gmdx");
    return style() == "gmdx";
}

// static
void GMDXChatLog::addNearby(const LLChat& chat)
{
    if (!sInstance) return;
    Entry e;
    e.from = chat.mFromID;
    e.name = LLViewerChat::hideSenderName(chat) ? std::string() : chat.mFromName;
    e.text = chat.mText;
    if (FSCommon::is_irc_me_prefix(e.text))
    {
        e.emote = true;
        e.text.erase(0, 3);
        if (e.name.empty()) LLStringUtil::trimHead(e.text);
    }
    if (chat.mSourceType == CHAT_SOURCE_SYSTEM) { e.channel = Channel::System; e.tag = "SYS"; }
    else if (chat.mSourceType == CHAT_SOURCE_OBJECT) { e.channel = Channel::Object; e.tag = "OBJ"; }
    else if (chat.mChatType == CHAT_TYPE_SHOUT) { e.channel = Channel::Shout; e.tag = "SHOUT"; }
    else if (chat.mChatType == CHAT_TYPE_WHISPER) { e.channel = Channel::Whisper; e.tag = "WHISPER"; }
    sInstance->add(std::move(e));
}

// static
const LLFontGL* GMDXChatLog::chatBarFont()
{
    if (!active()) return nullptr;
    static LLCachedControl<S32> size(gSavedSettings, "GMDXChatLogTextSize", 1);
    static const char* const kSizes[] = { "Medium", "Large", "Huge" };
    return LLFontGL::getFont(LLFontDescriptor("Barlow", kSizes[llclamp((S32)size, 0, 2)], 0));
}

// static
bool GMDXChatLog::showsIM(const LLUUID& session_id)
{
    if (!sInstance || !active() || !LLViewerChat::isNearbyChatOverlayVisible()) return false;
    const LLIMModel::LLIMSession* session = LLIMModel::getInstance()->findIMSession(session_id);
    return session && (!session->isGroupSessionType() || session_id == gAgent.getGroupID());
}

// static
void GMDXChatLog::addCombat(std::function<std::vector<Segment>()> segments)
{
    if (!sInstance || !segments) return;
    Entry e;
    e.channel = Channel::Combat;
    e.in_all = false;   // muted: shown only in the Combat tab
    e.segments = std::move(segments);
    sInstance->add(std::move(e));
}

void GMDXChatLog::onIM(const LLSD& msg)
{
    if (msg["is_friend_status"].asBoolean()) return;   // (the online / offline notices)
    const LLUUID session_id = msg["session_id"].asUUID();
    const LLIMModel::LLIMSession* session = LLIMModel::getInstance()->findIMSession(session_id);
    if (!session) return;
    Entry e;
    e.from = msg["from_id"].asUUID();
    e.text = msg["message"].asString();
    if (FSCommon::is_irc_me_prefix(e.text)) { e.emote = true; e.text.erase(0, 3); }
    if (session->isGroupSessionType())
    {
        e.channel = Channel::Group;
        e.tag = "GROUP";
        e.name = msg["from"].asString();
        e.in_all = session_id == gAgent.getGroupID();   // the active group only (a group's session id is its id)
    }
    else
    {
        e.channel = Channel::IM;
        const bool outgoing = e.from == gAgentID;
        e.tag = outgoing ? "TO" : "FROM";
        // a conference shows its name before the speaker's
        e.name = outgoing ? LLIMModel::getInstance()->getName(session_id) : msg["from"].asString();
        if (session->isAdHocSessionType() && !outgoing) e.tag = LLIMModel::getInstance()->getName(session_id);
    }
    if (e.from.isNull() || e.name.empty()) { e.name.clear(); }
    add(std::move(e));
}

void GMDXChatLog::add(Entry e)
{
    e.time = LLFrameTimer::getTotalSeconds();
    e.clock = std::time(nullptr);
    if (!mEntries.empty())
    {
        // an object saying the same again: one line, counted
        if (e.channel == Channel::Object)
        {
            for (auto it = mEntries.rbegin(); it != mEntries.rend() && it - mEntries.rbegin() < 5; ++it)
            {
                if (it->channel == Channel::Object && it->from == e.from && it->text == e.text
                    && e.time - it->time < kRepeatWithin)
                {
                    Entry again = *it;
                    again.repeat++;
                    again.time = e.time;
                    again.clock = e.clock;
                    again.merged = false;
                    mEntries.erase(std::next(it).base());
                    mEntries.push_back(std::move(again));
                    return;
                }
            }
        }
        const Entry& last = mEntries.back();
        e.merged = !e.name.empty() && !e.emote && !last.emote && last.from == e.from && last.name == e.name
                   && last.channel == e.channel && last.tag == e.tag && e.time - last.time < kMergeWithin;
    }
    mEntries.push_back(std::move(e));
    while (mEntries.size() > kHistory) mEntries.pop_front();
}

bool GMDXChatLog::inTab(const Entry& e, S32 tab) const
{
    switch (tab)
    {
        case TAB_ALL:    return e.in_all;
        case TAB_LOCAL:  return e.channel != Channel::Group && e.channel != Channel::IM && e.channel != Channel::Combat;
        case TAB_GROUP:  return e.channel == Channel::Group;
        case TAB_IM:     return e.channel == Channel::IM;
        case TAB_COMBAT: return e.channel == Channel::Combat;
        default:         return false;
    }
}

void GMDXChatLog::loadFonts()
{
    static LLCachedControl<S32> size(gSavedSettings, "GMDXChatLogTextSize", 1);
    static const char* const kSizes[] = { "Medium", "Large", "Huge" };
    static const char* const kTagSizes[] = { "Medium", "Large", "Huge" };
    const S32 s = llclamp((S32)size, 0, 2);
    auto get = [](const char* family, const char* sz, U8 style, const LLFontGL* fallback)
    {
        const LLFontGL* f = LLFontGL::getFont(LLFontDescriptor(family, sz, style));
        return f ? f : fallback;
    };
    const LLFontGL* sans = LLFontGL::getFontSansSerif();
    mText = get("Barlow", kSizes[s], 0, sans);
    mBold = get("Barlow", kSizes[s], LLFontGL::BOLD, mText);
    mItalic = get("Barlow", kSizes[s], LLFontGL::ITALIC, mText);
    mTag = get("BarlowCondensed", kTagSizes[s], 0, LLFontGL::getFontSansSerifSmall());
    mTagBold = get("BarlowCondensed", kTagSizes[s], LLFontGL::BOLD, mTag);
}

// One entry as rows of coloured text, wrapped to width: [time] [tag] name: text [xN].
void GMDXChatLog::layout(const Entry& e, F32 width, bool show_time, std::vector<Row>& rows, bool head) const
{
    const bool merged = e.merged && !head;
    rows.clear();
    Row row;
    F32 x = 0.f;
    auto put = [&](const LLFontGL* f, const std::string& text, const LLColor4& c, F32 gap)
    {
        row.segs.push_back({ x, { text, c } });
        row.fonts.push_back(f);
        x += f->getWidthF32(text) + gap;
    };
    if (show_time)
    {
        char buf[8] = {};
        std::strftime(buf, sizeof(buf), "%H:%M", std::localtime(&e.clock));
        put(mTag, buf, C_TIME, 8.f);
    }
    const LLColor4 chan = channel_color(e.channel);
    if (e.segments)   // combat: the kill feed's line, as it is
    {
        for (const Segment& s : e.segments()) put(mBold, s.text, s.color, 0.f);
        rows.push_back(std::move(row));
        return;
    }
    const F32 indent_merged = x;
    if (!e.tag.empty() && !merged) put(mTagBold, e.tag, chan, 6.f);
    if (!e.name.empty() && !merged)
    {
        const bool plain = e.channel == Channel::Say || e.channel == Channel::Whisper;
        put(mBold, e.name, plain ? name_color(e.from) : chan, 0.f);
        put(mText, e.emote ? " " : ": ", C_DIM, 0.f);
    }
    if (merged)
    {
        // under the line before's text: as far in as its name reached (approximately: the name's width)
        x = indent_merged + (e.tag.empty() ? 0.f : mTagBold->getWidthF32(e.tag) + 6.f) + mBold->getWidthF32(e.name)
            + mText->getWidthF32(": ");
    }
    const LLFontGL* body = (e.channel == Channel::Whisper || e.emote) ? mItalic
                         : e.channel == Channel::Shout ? mBold : mText;
    const LLColor4 body_color = e.channel == Channel::Say || e.channel == Channel::Whisper ? C_TEXT : chan;
    if (!e.links_done) e.shown = resolve_links(e.text, e.links_done);
    LLWString w = utf8str_to_wstring(e.links_done || !e.shown.empty() ? e.shown : e.text);
    LLWStringUtil::replaceChar(w, '\n', ' ');
    const std::string count = e.repeat > 1 ? llformat(" \xC3\x97%u", e.repeat) : std::string();   // " xN"
    const F32 wrap_x = (x > width * 0.5f) ? 12.f : x;   // continuation rows
    S32 start = 0;
    const S32 len = (S32)w.size();
    while (true)
    {
        const F32 avail = llmax(width - x, 24.f);
        S32 n = body->maxDrawableChars(w.c_str() + start, avail, len - start, LLFontGL::WORD_BOUNDARY_IF_POSSIBLE);
        if (n <= 0) n = llmin(1, len - start);
        put(body, wstring_to_utf8str(w.substr(start, n)), body_color, 0.f);
        start += n;
        if (start >= len || n <= 0) break;
        rows.push_back(std::move(row));
        row = Row();
        x = wrap_x;
    }
    if (!count.empty()) put(mTagBold, count, C_COUNT, 0.f);
    rows.push_back(std::move(row));
}

LLRect GMDXChatLog::barRect() const
{
    // The chat bar's rect in this view, also while it is hidden (it keeps its place); before it
    // first opens, its default place (floater_sl_chatbar.xml: left 5, at the bottom, 450 wide).
    if (LLFloater* bar = LLFloaterReg::findInstance("chatbar"))
    {
        LLRect r;
        bar->localRectToOtherView(bar->getLocalRect(), &r, this);
        return r;
    }
    return LLRect(5, 28, 565, 1);
}

bool GMDXChatLog::typing() const
{
    SLChatBar* bar = LLFloaterReg::findTypedInstance<SLChatBar>("chatbar");
    return bar && bar->getVisible() && bar->inputEditorHasFocus();
}

LLRect GMDXChatLog::tabRect(S32 tab) const
{
    S32 x = mPanelRect.mLeft + 8;
    for (S32 i = 0; i < tab; ++i) x += mTagBold->getWidth(std::string(kTabNames[i])) + 34;
    const S32 w = mTagBold->getWidth(std::string(kTabNames[tab])) + 24;
    return LLRect(x, mPanelRect.mTop - 4, x + w, mPanelRect.mTop - kTabH);
}

void GMDXChatLog::draw()
{
    if (!active() || !LLViewerChat::isNearbyChatOverlayVisible()) { mPanelRect = LLRect(); return; }
    loadFonts();
    static LLCachedControl<U32> rest_lines(gSavedSettings, "GMDXChatLogLines", 7);
    static LLCachedControl<U32> typing_lines(gSavedSettings, "GMDXChatLogTypingLines", 10);
    static LLCachedControl<F32> persist(gSavedSettings, "GMDXChatLogPersist", 20.f);
    static LLCachedControl<S32> log_width(gSavedSettings, "GMDXChatLogWidth", 560);

    const F64 now = LLFrameTimer::getTotalSeconds();
    const LLRect bar = barRect();
    const bool is_typing = typing();
    const bool combat = !is_typing && (gAgentCamera.cameraMouselook() || FSCombatADS::isHeld());
    const S32 line_h = (S32)mText->getLineHeight() + kGap;
    const S32 left = llmax(bar.mLeft, 4);
    const S32 width = llclamp((S32)log_width, 200, llmax(getRect().getWidth() - left - 8, 200));
    std::vector<Row> rows;

    if (!is_typing)
    {
        mPanelRect = LLRect();
        mScroll = 0;
        // newest first, until the line budget or a line that has faded out
        const S32 budget = combat ? kCombatLines : (S32)llclamp((U32)rest_lines, 1u, 30u);
        const F32 hold = llmax((F32)persist, 1.f);
        S32 y = bar.mTop + 6;   // the bottom row's baseline area starts just above the chat bar's place
        S32 used = 0;
        for (auto it = mEntries.rbegin(); it != mEntries.rend() && used < budget; ++it)
        {
            if (!inTab(*it, TAB_ALL)) continue;
            const F32 age = (F32)(now - it->time);
            if (!combat && age > hold) break;
            F32 alpha = combat ? (age < kCombatFlash ? 1.f : kCombatAlpha)
                               : llclamp((hold - age) / kFadeOut, 0.f, 1.f);
            if (alpha <= 0.f) continue;
            // a merged line whose first line will not be drawn above it shows the name itself
            bool head = false;
            if (it->merged)
            {
                auto older = std::next(it);
                while (older != mEntries.rend() && !inTab(*older, TAB_ALL)) ++older;
                head = older == mEntries.rend() || (!combat && (F32)(now - older->time) > hold);
            }
            layout(*it, (F32)width, false, rows, head);
            if (used + (S32)rows.size() >= budget && it->merged) layout(*it, (F32)width, false, rows, true);   // (the last one that fits)
            it->seen = true;
            const S32 h = (S32)rows.size() * line_h;
            if (combat && age < kCombatFlash)
            {
                // the new line's flash: a channel-tinted backing that fades with the flash
                const F32 f = 1.f - (F32)(age / kCombatFlash);
                LLColor4 c = faded(channel_color(it->channel), 0.16f * f);
                gl_rect_2d(left - 6, y + h, left + width + 6, y, c);
            }
            for (size_t r = 0; r < rows.size(); ++r)
            {
                const F32 ry = (F32)(y + h - (S32)(r + 1) * line_h + kGap);
                for (size_t k = 0; k < rows[r].segs.size(); ++k)
                {
                    const auto& s = rows[r].segs[k];
                    rows[r].fonts[k]->renderUTF8(s.second.text, 0, (F32)left + s.first, ry, faded(s.second.color, alpha),
                                                 LLFontGL::LEFT, LLFontGL::BOTTOM, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW_SOFT);
                }
            }
            y += h;
            used += (S32)rows.size();
        }
        LLView::draw();
        return;
    }

    // ---- typing: the panel over the chat bar ----
    const S32 rows_shown = (S32)llclamp((U32)typing_lines, 3u, 40u);
    const S32 body_h = rows_shown * line_h;
    const S32 panel_w = llmax(bar.getWidth(), 200);   // the chat bar's width: one block with it
    mPanelRect.set(left, bar.mTop + kTabH + body_h + 2 * kPad, left + panel_w, bar.mTop);
    gl_rect_2d(mPanelRect, C_PANEL);
    gl_rect_2d(mPanelRect, C_EDGE, false);
    gl_line_2d(mPanelRect.mLeft, mPanelRect.mTop - kTabH, mPanelRect.mRight, mPanelRect.mTop - kTabH, C_RULE);

    // tabs, with the lines each has that were never drawn
    for (S32 t = 0; t < TAB_COUNT; ++t)
    {
        const LLRect r = tabRect(t);
        if (t == mTab) gl_rect_2d(r.mLeft, r.mTop, r.mRight, r.mBottom, C_TAB_ON);
        mTagBold->renderUTF8(kTabNames[t], 0, (F32)(r.mLeft + 12), (F32)r.mBottom + 4, t == mTab ? C_TEXT : C_DIM,
                             LLFontGL::LEFT, LLFontGL::BOTTOM, LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
        if (t != mTab && t != TAB_ALL)
        {
            S32 unread = 0;
            for (const Entry& e : mEntries)
                if (!e.seen && inTab(e, t)) ++unread;
            if (unread)
            {
                const LLColor4 c = t == TAB_IM ? C_IM : t == TAB_GROUP ? C_GROUP : C_COUNT;
                mTagBold->renderUTF8(llformat("%d", unread), 0, (F32)(r.mRight - 8), (F32)r.mBottom + 4, c,
                                     LLFontGL::LEFT, LLFontGL::BOTTOM, LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
            }
        }
    }

    // the history of this tab, newest at the bottom, mScroll rows back
    std::vector<Row> all;
    const S32 need = rows_shown + mScroll;
    for (auto it = mEntries.rbegin(); it != mEntries.rend() && (S32)all.size() < need + 64; ++it)
    {
        if (!inTab(*it, mTab)) continue;
        layout(*it, (F32)(panel_w - 2 * kPad), true, rows);
        it->seen = true;
        all.insert(all.begin(), std::make_move_iterator(rows.begin()), std::make_move_iterator(rows.end()));
    }
    mScroll = llclamp(mScroll, 0, llmax((S32)all.size() - rows_shown, 0));
    const S32 last = (S32)all.size() - mScroll;   // one past the bottom row shown
    const S32 first = llmax(last - rows_shown, 0);
    LLLocalClipRect clip(LLRect(mPanelRect.mLeft, mPanelRect.mTop - kTabH, mPanelRect.mRight, mPanelRect.mBottom));
    S32 y = bar.mTop + kPad;
    for (S32 r = last - 1; r >= first; --r, y += line_h)
    {
        for (size_t k = 0; k < all[r].segs.size(); ++k)
        {
            const auto& s = all[r].segs[k];
            all[r].fonts[k]->renderUTF8(s.second.text, 0, (F32)(left + kPad) + s.first, (F32)(y + kGap), s.second.color,
                                        LLFontGL::LEFT, LLFontGL::BOTTOM, LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
        }
    }
    if (all.empty())
    {
        mTag->renderUTF8(mTab == TAB_COMBAT ? "Combat events (the kill feed) show here." : "Nothing here yet.", 0,
                         (F32)(left + kPad), (F32)(bar.mTop + kPad + kGap), C_TIME, LLFontGL::LEFT, LLFontGL::BOTTOM,
                         LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
    }
    LLView::draw();
}

bool GMDXChatLog::handleMouseDown(S32 x, S32 y, MASK mask)
{
    if (mPanelRect.isEmpty() || !mPanelRect.pointInRect(x, y)) return false;   // click-through outside the panel
    for (S32 t = 0; t < TAB_COUNT; ++t)
    {
        if (tabRect(t).pointInRect(x, y))
        {
            mTab = t;
            mScroll = 0;
            break;
        }
    }
    // keep typing: the panel belongs to the chat bar
    if (!typing()) SLChatBar::startChat(nullptr);
    return true;
}

bool GMDXChatLog::handleScrollWheel(S32 x, S32 y, S32 clicks)
{
    if (mPanelRect.isEmpty() || !mPanelRect.pointInRect(x, y)) return false;
    mScroll = llmax(mScroll - clicks * 3, 0);
    return true;
}

bool GMDXChatLog::handleHover(S32 x, S32 y, MASK mask)
{
    return !mPanelRect.isEmpty() && mPanelRect.pointInRect(x, y);
}
