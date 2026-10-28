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

#include "llmenubutton.h"
#include "lltoggleablemenu.h"

#include "fscombatads.h"
#include "fscommon.h"
#include "llagent.h"
#include "llagentcamera.h"
#include "llavatarnamecache.h"
#include "llcachename.h"
#include "llchat.h"
#include "llclipboard.h"
#include "llfloater.h"
#include "llfloaterblocked.h"
#include "llfloaterimcontainer.h"
#include "llfloaterimnearbychat.h"
#include "llfloaterreg.h"
#include "llfocusmgr.h"
#include "llfontgl.h"
#include "llgroupcolormap.h"
#include "llimview.h"
#include "lllocalcliprect.h"
#include "llrender2dutils.h"
#include "lltoolbarview.h"
#include "lluictrlfactory.h"
#include "llviewercontrol.h"
#include "llviewerchat.h"
#include "llmenugl.h"
#include "llmutelist.h"
#include "llpanelpeoplemenus.h"
#include "llslurl.h"
#include "llurlaction.h"
#include "llviewermenu.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llweb.h"
#include "llworld.h"
#include "llwindow.h"
#include "lggcontactsets.h"
#include "slchatbar.h"
#include "rlvactions.h"
#include "rlvcommon.h"

#include <ctime>
#include <regex>

GMDXChatLog* GMDXChatLog::sInstance = nullptr;

namespace
{
constexpr size_t kHistory = 20000;        // entries kept (LLFloaterIMNearbyChat's MAX_WINDOWED_ARCHIVE)
constexpr S32 kHistoryPage = 100;         // lines read from the log at a time
constexpr S32 kRestScan = 500;            // entries looked at for the lines at rest
constexpr F64 kMergeWithin = 30.0;        // s: a speaker's next line merges under the name
constexpr F64 kRepeatWithin = 60.0;       // s: an object's same line counts up
constexpr F32 kFadeOut = 3.f;             // s: the last part of a line's time at rest
constexpr S32 kCombatLines = 3;
constexpr F64 kCombatFlash = 3.0;         // s a new line is at full strength in combat
constexpr F32 kCombatAlpha = 0.35f;
constexpr S32 kPad = 10, kTabH = 24, kGap = 3;
constexpr S32 kGrip = 7;                  // px inside an unlocked panel's edge that resize it
enum : U32 { EDGE_LEFT = 1, EDGE_RIGHT = 2, EDGE_BOTTOM = 4, EDGE_TOP = 8 };

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
const LLColor4 C_SHADE(0.020f, 0.024f, 0.031f, 1.f);      // the band under the lines at rest
const LLColor4 C_UNLOCKED(0.435f, 0.827f, 1.f, 0.9f);     // the unlocked frame (#6FD3FF)

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

// A translucent band over l..r, b..t whose edges fade out: fx past the left and right, and fy across
// the top and the bottom edge (half in, half out), so rows stacked edge to edge make one even shade.
void soft_band(F32 l, F32 t, F32 r, F32 b, const LLColor4& c, F32 fx, F32 fy)
{
    const F32 xs[4] = { l - fx, l, r, r + fx };
    const F32 ys[4] = { b - fy * 0.5f, b + fy * 0.5f, t - fy * 0.5f, t + fy * 0.5f };
    const LLColor4 clear = faded(c, 0.f);
    auto vertex = [&](S32 i, S32 j)
    {
        const bool inner = (i == 1 || i == 2) && (j == 1 || j == 2);
        gGL.color4fv((inner ? c : clear).mV);
        gGL.vertex2f(xs[i], ys[j]);
    };
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gGL.begin(LLRender::TRIANGLES);
    for (S32 i = 0; i < 3; ++i)
    {
        for (S32 j = 0; j < 3; ++j)
        {
            vertex(i, j); vertex(i + 1, j); vertex(i + 1, j + 1);
            vertex(i, j); vertex(i + 1, j + 1); vertex(i, j + 1);
        }
    }
    gGL.end();
}

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

// The IM tab (groups: the Group tab) is in view at its newest lines while typing: the conversations
// with a line drawn there are read, as when they are focused in Conversations (the unread IM well
// clears). A line only seen at rest, in the All tab or scrolled up is not.
void read_conversations(const std::vector<LLUUID>& drawn)
{
    bool cleared = false;
    LLFloaterIMContainer* im_box = nullptr;
    for (const LLUUID& id : drawn)
    {
        const LLIMModel::LLIMSession* session = LLIMModel::getInstance()->findIMSession(id);
        if (!session || session->mNumUnread <= 0) continue;   // (also a second line of the same one)
        if (!cleared) im_box = LLFloaterIMContainer::findInstance();
        cleared = true;
        LLIMModel::getInstance()->sendNoUnreadMessages(id);
        if (im_box) im_box->flashConversationItemWidget(id, false);
    }
    // and the Conversations button (it keeps flashing while the container is minimised)
    if (cleared && gToolBarView) gToolBarView->flashCommand(LLCommandId("chat"), false);
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
    // the log's history ends before the first line this log shows (the floater froze it when it opened)
    sInstance->takeHistory();
    sInstance->add(fromChat(chat));
}

// static
GMDXChatLog::Entry GMDXChatLog::fromChat(const LLChat& chat)
{
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
    else if (chat.mSourceType == CHAT_SOURCE_OBJECT)
    {
        e.channel = Channel::Object;
        e.tag = "OBJ";
        // for its menu, as LLChatHistoryHeader keeps it
        e.owner = chat.mOwnerID;
        if (chat.mFromID.notNull() && LLWorld::instanceExists())
        {
            if (LLViewerRegion* region = LLWorld::getInstance()->getRegionFromPosAgent(chat.mPosAgent))
            {
                e.slurl = LLSLURL(region->getName(), chat.mPosAgent).getLocationString();
            }
        }
    }
    else if (chat.mChatType == CHAT_TYPE_SHOUT) { e.channel = Channel::Shout; e.tag = "SHOUT"; }
    else if (chat.mChatType == CHAT_TYPE_WHISPER) { e.channel = Channel::Whisper; e.tag = "WHISPER"; }
    return e;
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
    e.session = session_id;
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
    e.seq = ++mSeq;
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
                    again.seq = e.seq;
                    again.merged = false;
                    again.rows_n = 0;   // (the count may wrap)
                    countUnread(*it, -1);
                    mEntries.erase(std::next(it).base());
                    mEntries.push_back(std::move(again));
                    countUnread(mEntries.back(), 1);
                    return;
                }
            }
        }
        const Entry& last = mEntries.back();
        e.merged = !e.name.empty() && !e.emote && !last.emote && last.from == e.from && last.name == e.name
                   && last.channel == e.channel && last.tag == e.tag && e.time - last.time < kMergeWithin;
    }
    mEntries.push_back(std::move(e));
    countUnread(mEntries.back(), 1);
    while (mEntries.size() > kHistory)
    {
        countUnread(mEntries.front(), -1);
        mEntries.pop_front();
        mDropped = true;
    }
}

void GMDXChatLog::countUnread(const Entry& e, S32 delta)
{
    if (e.seen) return;
    for (S32 t = 0; t < TAB_COUNT; ++t)
        if (inTab(e, t)) mUnread[t] += delta;
}

void GMDXChatLog::markSeen(Entry& e)
{
    if (e.seen) return;
    countUnread(e, -1);
    e.seen = true;
}

void GMDXChatLog::markTabSeen(S32 tab)
{
    if (mUnread[tab] <= 0) return;
    for (Entry& e : mEntries)
        if (inTab(e, tab)) markSeen(e);
}

S32 GMDXChatLog::rowCount(const Entry& e, F32 width) const
{
    if (e.segments) return 1;   // (a combat line never wraps)
    if (e.rows_n > 0 && e.rows_w == width && e.rows_font == mText) return e.rows_n;
    // ponytail: a resize drag changes the width every frame, which would lay out every line below the
    // view each frame. Meanwhile the count is scaled from the last width it was laid out at; the
    // first frame after the drag lays them out at the new width.
    if (mDragging && e.rows_n > 0 && e.rows_font == mText)
        return llmax(1, (S32)std::ceil(e.rows_n * e.rows_w / width));
    std::vector<Row> rows;
    layout(e, width, true, rows);
    keepRowCount(e, width, rows.size());
    return (S32)rows.size();
}

void GMDXChatLog::keepRowCount(const Entry& e, F32 width, size_t rows) const
{
    // ponytail: a line with a link still resolving is laid out again each frame until it resolves
    if (!e.links_done) return;
    e.rows_n = (S32)rows;
    e.rows_w = width;
    e.rows_font = mText;
}

void GMDXChatLog::takeHistory()
{
    if (mHistoryTaken) return;
    // (not built yet: no nearby line can have come in either, processChat builds it first)
    if (LLFloaterIMNearbyChat* nearby = LLFloaterReg::findTypedInstance<LLFloaterIMNearbyChat>("nearby_chat"))
    {
        mHistoryTaken = true;
        mHistory = nearby->getHistoryLog();   // (null: Preferences don't show the log in conversations)
    }
}

void GMDXChatLog::requestHistory()
{
    if (mHistoryLoading || mHistoryDone) return;
    takeHistory();
    if (!mHistoryTaken) return;   // (asked again next frame)
    if (!mHistory || mHistory->isStale() || mDropped)
    {
        mHistoryDone = true;
        return;
    }
    mHistoryLoading = true;
    mHistory->loadPage(mHistoryCursor, kHistoryPage, [](const LLChatLogPage& page)
    {
        if (sInstance) sInstance->onHistoryPage(page);
    });
}

void GMDXChatLog::onHistoryPage(const LLChatLogPage& page)
{
    mHistoryLoading = false;
    LLFloaterIMNearbyChat* nearby = LLFloaterReg::findTypedInstance<LLFloaterIMNearbyChat>("nearby_chat");
    if (!nearby || mDropped || page.mMessages.empty() || page.mAtStart) mHistoryDone = true;
    if (!nearby || mDropped || page.mMessages.empty()) return;
    mHistoryCursor = page.mMessages.front().mBegin;
    // above everything: the oldest first, as many as the cap leaves room for
    const size_t room = mEntries.size() < kHistory ? kHistory - mEntries.size() : 0;
    const size_t take = llmin(room, page.mMessages.size());
    if (take < page.mMessages.size()) mHistoryDone = true;
    std::vector<Entry> older;
    older.reserve(take);
    for (size_t i = page.mMessages.size() - take; i < page.mMessages.size(); ++i)
    {
        const LLChatLogEntry& line = page.mMessages[i];
        LLChat chat;
        nearby->chatFromLogEntry(line, chat);
        Entry e = fromChat(chat);
        e.history = true;
        e.seen = true;   // (never unread)
        e.stamp = line.mTime;   // "hh:mm:ss", the date before it when not today
        const size_t n = e.stamp.size();
        if (n >= 8 && e.stamp[n - 3] == ':' && e.stamp[n - 6] == ':') e.stamp.resize(n - 3);
        older.push_back(std::move(e));
    }
    mEntries.insert(mEntries.begin(), std::make_move_iterator(older.begin()), std::make_move_iterator(older.end()));
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
    row.entry = &e;
    F32 x = 0.f;
    const F32 dim = e.history ? 0.7f : 1.f;   // the log's lines, before this session
    auto put = [&](const LLFontGL* f, const std::string& text, const LLColor4& c, F32 gap)
    {
        row.segs.push_back({ x, { text, faded(c, dim) } });
        row.fonts.push_back(f);
        x += f->getWidthF32(text) + gap;
    };
    if (show_time && !e.stamp.empty())
    {
        put(mTag, e.stamp, C_TIME, 8.f);
    }
    else if (show_time)
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
        row.name_seg = (S32)row.segs.size();
        put(mBold, e.name, plain ? name_color(e.from) : chan, 0.f);
        put(mText, e.emote ? " " : ": ", C_DIM, 0.f);
    }
    // A merged line and every wrapped row start a short way in from the time (a hanging indent): under
    // the name they ran far right with a long name, the wrapped rows back at the left edge.
    const F32 hang = indent_merged + 14.f;
    if (merged)
    {
        x = hang;
    }
    const LLFontGL* body = (e.channel == Channel::Whisper || e.emote) ? mItalic
                         : e.channel == Channel::Shout ? mBold : mText;
    const LLColor4 body_color = e.channel == Channel::Say || e.channel == Channel::Whisper ? C_TEXT : chan;
    if (!e.links_done) e.shown = resolve_links(e.text, e.links_done);
    LLWString w = utf8str_to_wstring(e.links_done || !e.shown.empty() ? e.shown : e.text);
    LLWStringUtil::replaceChar(w, '\n', ' ');
    const std::string count = e.repeat > 1 ? llformat(" \xC3\x97%u", e.repeat) : std::string();   // " xN"
    const F32 wrap_x = hang;   // continuation rows
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
        row.entry = &e;
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

LLRect GMDXChatLog::gearRect() const
{
    return LLRect(mPanelRect.mRight - 28, mPanelRect.mTop - 2, mPanelRect.mRight - 6, mPanelRect.mTop - kTabH + 2);
}

LLRect GMDXChatLog::lockRect() const
{
    LLRect r = gearRect();
    return r.translate(-r.getWidth() - 2, 0);
}

LLRect GMDXChatLog::defaultPanelRect(const LLRect& bar, S32 line_h) const
{
    static LLCachedControl<U32> typing_lines(gSavedSettings, "GMDXChatLogTypingLines", 10);
    const S32 body_h = (S32)llclamp((U32)typing_lines, 3u, 40u) * line_h;
    const S32 left = llmax(bar.mLeft, 4);
    return LLRect(left, bar.mTop + kTabH + body_h + 2 * kPad, left + llmax(bar.getWidth(), 200), bar.mTop);
}

LLRect GMDXChatLog::savedRect() const
{
    static LLCachedControl<LLRect> saved(gSavedSettings, "GMDXChatLogRect", LLRect());
    const LLRect r = saved;
    return r.isEmpty() ? LLRect() : keepInView(r);
}

LLRect GMDXChatLog::keepInView(LLRect r) const
{
    const LLRect area = panelArea();
    r.mRight = r.mLeft + llclamp(r.getWidth(), llmin(minWidth(), area.getWidth()), area.getWidth());
    r.mBottom = r.mTop - llclamp(r.getHeight(), llmin(minHeight(), area.getHeight()), area.getHeight());
    r.translate(llclamp(r.mLeft, area.mLeft, area.mRight - r.getWidth()) - r.mLeft,
                llclamp(r.mBottom, area.mBottom, area.mTop - r.getHeight()) - r.mBottom);
    return r;
}

LLRect GMDXChatLog::panelArea() const
{
    // the whole floater view: the window below the menu and nav bars, edge to edge, its toolbar strips
    // included (the toolbars' buttons make room, gmdxtoolbarcluster.h), with room for the bar at the bottom
    LLRect area = getLocalRect();
    if (gFloaterView) gFloaterView->localRectToOtherView(gFloaterView->getLocalRect(), &area, this);
    area.mBottom += barHeight();
    return area;
}

S32 GMDXChatLog::barHeight() const
{
    LLFloater* bar = LLFloaterReg::findInstance("chatbar");
    return bar ? bar->getRect().getHeight() : 27;   // (floater_sl_chatbar.xml)
}

LLRect GMDXChatLog::unitRect() const
{
    // (the chat bar hidden or closed: no strip under the panel that takes the world's clicks)
    LLRect r = mPanelRect;
    LLFloater* bar = LLFloaterReg::findInstance("chatbar");
    if (!r.isEmpty() && bar && bar->isInVisibleChain()) r.mBottom -= barHeight();
    return r;
}

bool GMDXChatLog::onBarControl(S32 x, S32 y) const
{
    // unlocked, a press on the chat bar's editor or channel spinner is theirs; the rest of the bar and
    // the edges still move / resize the panel with it
    LLFloater* bar = LLFloaterReg::findInstance("chatbar");
    if (!mUnlocked || !bar || mPanelRect.pointInRect(x, y) || !unitRect().pointInRect(x, y) || grabEdges(x, y)) return false;
    S32 bx = 0, by = 0;
    localPointToOtherView(x, y, &bx, &by, bar);
    for (LLView* child : *bar->getChildList())
        if (child->isCtrl() && child->getVisible() && child->getRect().pointInRect(bx, by)) return true;
    return false;
}

void GMDXChatLog::placeBar() const
{
    // (SLChatBar::draw does it too, but after it has been put where it was: a frame behind the panel)
    LLFloater* bar = LLFloaterReg::findInstance("chatbar");
    LLRect r;
    if (bar && bar->getParent() && dockedBarRect(bar->getParent(), r) && r != bar->getRect()) bar->setShape(r);
}

// static
bool GMDXChatLog::unitPlace(const LLView* other, LLRect& rect)
{
    if (!sInstance || !other || sInstance->mUnitPlace.isEmpty()) return false;
    sInstance->localRectToOtherView(sInstance->mUnitPlace, &rect, other);
    return true;
}

// static
bool GMDXChatLog::dockedBarRect(const LLView* parent, LLRect& rect)
{
    if (!sInstance || !parent || !active()) return false;
    const LLRect panel = sInstance->mDragging ? sInstance->mDragRect : sInstance->savedRect();
    if (panel.isEmpty()) return false;
    const LLRect bar(panel.mLeft, panel.mBottom, panel.mRight, panel.mBottom - sInstance->barHeight());
    sInstance->localRectToOtherView(bar, &rect, parent);
    return true;
}

S32 GMDXChatLog::minWidth() const
{
    if (!mTagBold) return 360;
    S32 tabs = 8;   // as tabRect lays them out
    for (S32 i = 0; i < TAB_COUNT; ++i) tabs += mTagBold->getWidth(std::string(kTabNames[i])) + 34;
    return tabs + 56;   // + the lock and the gear
}

S32 GMDXChatLog::minHeight() const
{
    return kTabH + 2 * kPad + 3 * llmax(mLineH, 12);
}

U32 GMDXChatLog::grabEdges(S32 x, S32 y) const
{
    const LLRect unit = unitRect();   // (the bottom edge is the chat bar's)
    U32 edges = 0;
    if (x < unit.mLeft + kGrip) edges |= EDGE_LEFT;
    else if (x >= unit.mRight - kGrip) edges |= EDGE_RIGHT;
    if (y < unit.mBottom + kGrip) edges |= EDGE_BOTTOM;
    else if (y >= unit.mTop - kGrip) edges |= EDGE_TOP;
    return edges;
}

void GMDXChatLog::lock()
{
    mUnlocked = false;
    if (mDragging && hasMouseCapture()) gFocusMgr.setMouseCapture(nullptr);
    onMouseCaptureLost();   // (a drag in progress keeps where it got to)
}

void GMDXChatLog::draw()
{
    // (only hidden for now, e.g. Do Not Disturb: the unit keeps its place, the toolbars stay as they are)
    if (!active()) mUnitPlace = LLRect();
    if (!active() || !LLViewerChat::isNearbyChatOverlayVisible()) { lock(); mPanelRect = LLRect(); mOnBar = false; mHits.clear(); return; }
    loadFonts();
    static LLCachedControl<U32> rest_lines(gSavedSettings, "GMDXChatLogLines", 7);
    static LLCachedControl<F32> persist(gSavedSettings, "GMDXChatLogPersist", 20.f);
    static LLCachedControl<S32> log_width(gSavedSettings, "GMDXChatLogWidth", 560);

    const F64 now = LLFrameTimer::getTotalSeconds();
    const LLRect bar = barRect();
    if (gAgentCamera.cameraMouselook()) lock();   // (mouselook has the mouse)
    const bool is_typing = typing();
    const bool show_panel = is_typing || mUnlocked;
    const bool combat = !show_panel && (gAgentCamera.cameraMouselook() || FSCombatADS::isHeld());
    const S32 line_h = (S32)mText->getLineHeight() + kGap;
    mLineH = line_h;
    // the panel's place: being dragged, saved, or over the chat bar
    const LLRect saved = mDragging ? mDragRect : savedRect();
    const LLRect place = saved.isEmpty() ? defaultPanelRect(bar, line_h) : saved;
    // with the chat bar under it (unitPlace): docked right under a moved panel, else where it is
    mUnitPlace = place;
    mUnitPlace.mBottom -= barHeight();
    if (saved.isEmpty()) mUnitPlace.unionWith(bar);
    std::vector<Row> rows;

    if (!show_panel)
    {
        mPanelRect = LLRect();
        mOnBar = false;
        mJumpRect = LLRect();
        mScroll = 0;
        mPanelShown = false;
        mHits.clear();
        // newest first, until the line budget or a line that has faded out
        S32 budget = combat ? kCombatLines : (S32)llclamp((U32)rest_lines, 1u, 30u);
        const F32 hold = llmax((F32)persist, 1.f);
        // over the chat bar, or in the moved panel's place (its lines, at most as many as it shows)
        S32 left = llmax(bar.mLeft, 4);
        S32 width = llclamp((S32)log_width, 200, llmax(getRect().getWidth() - left - 8, 200));
        S32 y = bar.mTop + 6;   // the bottom row's baseline area starts just above the chat bar's place
        if (!saved.isEmpty())
        {
            left = place.mLeft + kPad;
            width = place.getWidth() - 2 * kPad;
            y = place.mBottom + kPad;
            budget = llmin(budget, llmax((place.getHeight() - kTabH - 2 * kPad) / line_h, 1));
        }
        S32 used = 0;
        struct DrawnRow { Row row; S32 y; F32 alpha; };   // y: the row's bottom
        std::vector<DrawnRow> drawn;
        mDrawnSeq = mSeq;
        S32 scanned = 0;   // ponytail: in combat, a long run of other tabs' lines (group spam) ends the look-back
        for (auto it = mEntries.rbegin(); it != mEntries.rend() && used < budget && scanned++ < kRestScan; ++it)
        {
            if (it->history) break;   // (said before this session)
            const F32 age = (F32)(now - it->time);
            if (!combat && age > hold) break;
            if (!inTab(*it, TAB_ALL)) continue;
            F32 alpha = combat ? (age < kCombatFlash ? 1.f : kCombatAlpha)
                               : llclamp((hold - age) / kFadeOut, 0.f, 1.f);
            if (alpha <= 0.f) continue;
            // a merged line whose first line will not be drawn above it shows the name itself
            bool head = false;
            if (it->merged)
            {
                auto older = std::next(it);
                for (S32 n = 0; older != mEntries.rend() && !inTab(*older, TAB_ALL) && n < kRestScan; ++n) ++older;
                head = older == mEntries.rend() || older->history || !inTab(*older, TAB_ALL)
                       || (!combat && (F32)(now - older->time) > hold);
            }
            layout(*it, (F32)width, false, rows, head);
            if (used + (S32)rows.size() >= budget && it->merged) layout(*it, (F32)width, false, rows, true);   // (the last one that fits)
            markSeen(*it);
            const S32 h = (S32)rows.size() * line_h;
            if (combat && age < kCombatFlash)
            {
                // the new line's flash: a channel-tinted backing that fades with the flash
                const F32 f = 1.f - (F32)(age / kCombatFlash);
                LLColor4 c = faded(channel_color(it->channel), 0.16f * f);
                gl_rect_2d(left - 6, y + h, left + width + 6, y, c);
            }
            used += (S32)rows.size();
            for (size_t r = 0; r < rows.size(); ++r)
            {
                drawn.push_back({ std::move(rows[r]), y + h - (S32)(r + 1) * line_h, alpha });
            }
            y += h;
        }
        // Legibility over a bright world: a soft dark band under each row's text (its edges fade out,
        // stacked rows blend into one), all of them before any text so no band dims a row above.
        static LLCachedControl<F32> shade(gSavedSettings, "GMDXChatLogShade", 0.4f);
        const F32 band = llclamp((F32)shade, 0.f, 1.f);
        if (band > 0.f)
        {
            for (const DrawnRow& d : drawn)
            {
                if (d.row.segs.empty()) continue;
                const auto& end = d.row.segs.back();
                const F32 w = end.first + d.row.fonts.back()->getWidthF32(end.second.text);
                soft_band((F32)left - 4.f, (F32)(d.y + line_h), (F32)left + w + 4.f, (F32)d.y, faded(C_SHADE, band * d.alpha), 10.f, 6.f);
            }
        }
        for (const DrawnRow& d : drawn)
        {
            const F32 ry = (F32)(d.y + kGap);
            for (size_t k = 0; k < d.row.segs.size(); ++k)
            {
                const auto& s = d.row.segs[k];
                d.row.fonts[k]->renderUTF8(s.second.text, 0, (F32)left + s.first, ry, faded(s.second.color, d.alpha),
                                           LLFontGL::LEFT, LLFontGL::BOTTOM, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW_SOFT);
            }
        }
        LLView::draw();
        return;
    }

    // ---- typing (or unlocked): the panel, by default over the chat bar and as wide as it ----
    mPanelRect = place;
    if (!mPanelShown) markTabSeen(mTab);   // (opened: as when a tab is clicked)
    mPanelShown = true;
    const S32 rows_shown = llmax((mPanelRect.getHeight() - kTabH - 2 * kPad) / line_h, 1);
    const S32 panel_w = mPanelRect.getWidth();
    const S32 left = mPanelRect.mLeft;
    const S32 body_y = mPanelRect.mBottom + kPad;   // the bottom row
    gl_rect_2d(mPanelRect, C_PANEL);
    {
        // one frame with the chat bar under it: the panel's bottom edge is the rule between them
        LLFloater* bar_floater = LLFloaterReg::findInstance("chatbar");
        mOnBar = bar_floater && bar_floater->getVisible() && bar.mTop == mPanelRect.mBottom && bar.mLeft == mPanelRect.mLeft
                 && bar.mRight == mPanelRect.mRight;
        const S32 l = mPanelRect.mLeft, r = mPanelRect.mRight - 1, t = mPanelRect.mTop - 1, b = mPanelRect.mBottom;
        gl_line_2d(l, b, l, t, C_EDGE);
        gl_line_2d(l, t, r, t, C_EDGE);
        gl_line_2d(r, t, r, b, C_EDGE);
        gl_line_2d(l, b, r, b, mOnBar ? C_RULE : C_EDGE);
    }
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
            const S32 unread = mUnread[t];
            if (unread > 0)
            {
                const LLColor4 c = t == TAB_IM ? C_IM : t == TAB_GROUP ? C_GROUP : C_COUNT;
                mTagBold->renderUTF8(llformat("%d", unread), 0, (F32)(r.mRight - 8), (F32)r.mBottom + 4, c,
                                     LLFontGL::LEFT, LLFontGL::BOTTOM, LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
            }
        }
    }

    // the chat bar's options (its own gear is hidden while the log is on: SLChatBar::updateChannelSelector)
    // and left of it the lock: unlocked, the panel moves and resizes
    S32 mx = 0, my = 0;
    LLUI::getInstance()->getMousePositionLocal(this, &mx, &my);
    auto icon = [&](const char* name, const LLRect& r, const LLColor4& color)
    {
        if (LLUIImagePtr image = LLUI::getUIImage(name))
        {
            const S32 side = llmin(r.getWidth(), r.getHeight());
            image->draw(r.getCenterX() - side / 2, r.getCenterY() - side / 2, side, side, color);
        }
    };
    const LLRect gear = gearRect(), lock_r = lockRect();
    icon("OptionsMenu_Off", gear, gear.pointInRect(mx, my) ? C_TEXT : C_DIM);
    icon(mUnlocked ? "GMDX_ChatLog_Unlocked" : "GMDX_ChatLog_Locked", lock_r,
         mUnlocked ? C_UNLOCKED : lock_r.pointInRect(mx, my) ? C_TEXT : C_DIM);

    // The history of this tab, newest at the bottom, mScroll rows back. Only the rows in view are laid
    // out: the ones below it are passed by their row count, known per entry once laid out.
    const F32 text_w = (F32)(panel_w - 2 * kPad);
    std::vector<Row> shown;   // bottom up
    std::vector<LLUUID> drawn_sessions;   // the conversations of the lines shown
    S32 unread_below = 0;     // lines below the view never drawn: said since scrolling up
    for (S32 pass = 0; pass < 2; ++pass)
    {
        shown.clear();
        drawn_sessions.clear();
        unread_below = 0;
        S32 below = mScroll;   // rows still to pass under the view
        auto it = mEntries.rbegin();
        for (; it != mEntries.rend(); ++it)
        {
            if (!inTab(*it, mTab)) continue;
            const S32 n = rowCount(*it, text_w);
            if (pass == 0 && mScroll > 0 && it->seq > mDrawnSeq)
            {
                // said since the last frame while scrolled up: the view stays on what it shows
                mScroll += n;
                below += n;
            }
            if (below < n) break;
            below -= n;
            if (!it->seen) ++unread_below;
        }
        if (pass == 0) mDrawnSeq = mSeq;
        for (; it != mEntries.rend() && (S32)shown.size() < rows_shown; ++it)
        {
            if (!inTab(*it, mTab)) continue;
            layout(*it, text_w, true, rows);
            keepRowCount(*it, text_w, rows.size());
            markSeen(*it);
            if (it->session.notNull()) drawn_sessions.push_back(it->session);
            for (S32 r = (S32)rows.size() - 1 - below; r >= 0 && (S32)shown.size() < rows_shown; --r)
            {
                shown.push_back(std::move(rows[r]));
            }
            below = 0;
        }
        // at the top of what is kept: older nearby chat from the log
        if ((it == mEntries.rend() || (S32)shown.size() < rows_shown) && (mTab == TAB_ALL || mTab == TAB_LOCAL))
        {
            requestHistory();
        }
        // scrolled past the top: back down until the view is full, and again
        if ((S32)shown.size() >= rows_shown || mScroll == 0) break;
        mScroll = llmax(mScroll - (rows_shown - (S32)shown.size()), 0);
    }
    if (is_typing && mScroll == 0 && (mTab == TAB_IM || mTab == TAB_GROUP)) read_conversations(drawn_sessions);
    {
        LLLocalClipRect clip(LLRect(mPanelRect.mLeft, mPanelRect.mTop - kTabH, mPanelRect.mRight, mPanelRect.mBottom));
        S32 y = body_y;
        mHits.clear();
        for (const Row& row : shown)
        {
            for (size_t k = 0; k < row.segs.size(); ++k)
            {
                const auto& s = row.segs[k];
                row.fonts[k]->renderUTF8(s.second.text, 0, (F32)(left + kPad) + s.first, (F32)(y + kGap), s.second.color,
                                         LLFontGL::LEFT, LLFontGL::BOTTOM, LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
            }
            // its name, a link while locked (a click IMs them, a right click opens their menu)
            NameHit hit;
            if (!mUnlocked && row.entry && row.name_seg >= 0 && nameTarget(*row.entry, hit))
            {
                const auto& s = row.segs[row.name_seg];
                const LLFontGL* font = row.fonts[row.name_seg];
                hit.rect.setOriginAndSize(left + kPad + (S32)s.first, y + kGap, font->getWidth(s.second.text), (S32)font->getLineHeight());
                hit.color = s.second.color;
                if (hit.rect.pointInRect(mx, my))
                {
                    // hovered: underlined
                    const S32 uy = hit.rect.mBottom + (S32)font->getDescenderHeight() - 1;
                    gl_line_2d(hit.rect.mLeft, uy, hit.rect.mRight, uy, hit.color);
                }
                mHits.push_back(std::move(hit));
            }
            y += line_h;
        }
        if (shown.empty())
        {
            mTag->renderUTF8(mTab == TAB_COMBAT ? "Combat events (the kill feed) show here." : "Nothing here yet.", 0,
                             (F32)(left + kPad), (F32)(body_y + kGap), C_TIME, LLFontGL::LEFT, LLFontGL::BOTTOM,
                             LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
        }
    } // (the clip ends here: the jump button and the unlocked frame reach outside the rows)

    // scrolled up more than a panelful: back to the newest line, with what was said since
    mJumpRect = LLRect();
    if (mScroll > rows_shown)
    {
        const std::string label = unread_below > 0 ? llformat("%d NEW", unread_below) : std::string("LATEST");
        const S32 arrow = 12;
        const S32 h = (S32)mTagBold->getLineHeight() + 6;
        const S32 w = mTagBold->getWidth(label) + arrow + 16;
        mJumpRect.setOriginAndSize(mPanelRect.mRight - kPad - w, mPanelRect.mBottom + 6, w, h);
        const bool hover = mJumpRect.pointInRect(mx, my);
        const LLColor4& ink = unread_below > 0 ? C_GROUP : hover ? C_TEXT : C_DIM;
        gl_rect_2d(mJumpRect, C_TAB_ON);
        gl_rect_2d(mJumpRect, hover ? C_DIM : C_EDGE, false);
        icon("Arrow_Down", LLRect(mJumpRect.mLeft + 5, mJumpRect.getCenterY() + arrow / 2, mJumpRect.mLeft + 5 + arrow,
                                  mJumpRect.getCenterY() - arrow / 2), ink);
        mTagBold->renderUTF8(label, 0, (F32)(mJumpRect.mLeft + arrow + 10), (F32)(mJumpRect.mBottom + 3), ink,
                             LLFontGL::LEFT, LLFontGL::BOTTOM, LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
    }
    if (mUnlocked)
    {
        // the frame, and a handle at each corner and edge middle, round the panel and the chat bar
        const LLRect p = unitRect();
        gl_rect_2d(p, C_UNLOCKED, false);
        gl_rect_2d(p.mLeft + 1, p.mTop - 1, p.mRight - 1, p.mBottom + 1, faded(C_UNLOCKED, 0.4f), false);
        const S32 xs[3] = { p.mLeft, p.getCenterX(), p.mRight };
        const S32 ys[3] = { p.mBottom, p.getCenterY(), p.mTop };
        for (S32 i = 0; i < 3; ++i)
            for (S32 j = 0; j < 3; ++j)
                if (i != 1 || j != 1)
                    gl_rect_2d(xs[i] - 3, ys[j] + 3, xs[i] + 3, ys[j] - 3, C_UNLOCKED);
    }
    LLView::draw();
}

bool GMDXChatLog::nameTarget(const Entry& e, NameHit& hit) const
{
    if (e.segments || e.name.empty() || e.channel == Channel::System || e.channel == Channel::Combat) return false;
    hit.name = e.name;
    if (e.channel == Channel::Object)
    {
        hit.id = e.from;
        hit.object = true;
        hit.owner = e.owner;
        hit.slurl = e.slurl;
        return hit.id.notNull();
    }
    // an IM you sent names the other person (a conference has no one to name)
    hit.id = e.channel == Channel::IM && e.from == gAgentID ? LLIMModel::getInstance()->getOtherParticipantID(e.session) : e.from;
    if (hit.id.isNull() || hit.id == gAgentID) return false;
    // [RLVa:KB] @shownames: no menu for a name nearby or in group chat it hides (as the chat history)
    if (e.channel != Channel::IM && !RlvActions::canShowName(RlvActions::SNC_DEFAULT, hit.id)) return false;
    return true;
}

const GMDXChatLog::NameHit* GMDXChatLog::hitAt(S32 x, S32 y) const
{
    if (mUnlocked) return nullptr;   // (unlocked, a press moves the panel)
    for (const NameHit& hit : mHits)
        if (hit.rect.pointInRect(x, y)) return &hit;
    return nullptr;
}

void GMDXChatLog::showObjectMenu(const NameHit& hit, S32 x, S32 y)
{
    // the chat history's object menu (menu_object_icon.xml, LLChatHistoryHeader's actions), and Copy Name
    LLSD data;
    data["object_id"] = hit.id;
    data["name"] = hit.name;
    data["owner_id"] = hit.owner;
    data["slurl"] = LLWeb::escapeURL(hit.slurl);
    mObjectMenuData = data;
    LLMenuGL* menu = static_cast<LLMenuGL*>(mObjectMenu.get());
    if (!menu)
    {
        LLUICtrl::CommitCallbackRegistry::ScopedRegistrar registrar;
        LLUICtrl::EnableCallbackRegistry::ScopedRegistrar registrar_enable;
        registrar.add("ObjectIcon.Action", [this](LLUICtrl*, const LLSD& param)
        {
            const std::string level = param.asString();
            const LLUUID id = mObjectMenuData["object_id"].asUUID();
            const std::string name = mObjectMenuData["name"].asString();
            if (level == "profile") LLFloaterReg::showInstance("inspect_remote_object", mObjectMenuData);
            else if (level == "block")
            {
                LLMuteList::getInstance()->add(LLMute(id, name, LLMute::OBJECT));
                LLFloaterBlocked::showMuteAndSelect(id);
            }
            else if (level == "unblock") LLMuteList::getInstance()->remove(LLMute(id, name, LLMute::OBJECT));
            else if (level == "map") LLUrlAction::showLocationOnMap("secondlife://" + mObjectMenuData["slurl"].asString());
            else if (level == "teleport") LLUrlAction::teleportToLocation("secondlife://" + mObjectMenuData["slurl"].asString());
            else if (level == "obj_zoom_in" && id.notNull()) handle_zoom_to_object(id);
        });
        registrar_enable.add("ObjectIcon.Visible", [this](LLUICtrl*, const LLSD& param)
        {
            const std::string level = param.asString();
            const LLUUID id = mObjectMenuData["object_id"].asUUID();
            const std::string name = mObjectMenuData["name"].asString();
            if (level == "is_blocked") return LLMuteList::getInstance()->isMuted(id, name, LLMute::flagTextChat);
            if (level == "not_blocked") return !LLMuteList::getInstance()->isMuted(id, name, LLMute::flagTextChat);
            if (level == "obj_zoom_in") return id.notNull() && gObjectList.findObject(id) != nullptr;
            return false;
        });
        menu = LLUICtrlFactory::getInstance()->createFromFile<LLMenuGL>("menu_object_icon.xml", gMenuHolder,
                                                                         LLViewerMenuHolderGL::child_registry_t::instance());
        if (!menu)
        {
            LL_WARNS() << "Failed to create menu_object_icon.xml" << LL_ENDL;
            return;
        }
        LLMenuItemCallGL::Params copy;
        copy.name("copy_name");
        copy.label("Copy Name");
        copy.on_click.function([this](LLUICtrl*, const LLSD&)
        {
            const LLWString name = utf8str_to_wstring(mObjectMenuData["name"].asString());
            LLClipboard::instance().copyToClipboard(name, 0, (S32)name.size());
        });
        menu->addSeparator();
        menu->addChild(LLUICtrlFactory::create<LLMenuItemCallGL>(copy));
        mObjectMenu = menu->getHandle();
        menu->updateParent(LLMenuGL::sMenuContainer);
    }
    LLMenuGL::showPopup(this, menu, x, y);
}

bool GMDXChatLog::handleRightMouseDown(S32 x, S32 y, MASK mask)
{
    if (mPanelRect.isEmpty() || !mPanelRect.pointInRect(x, y)) return false;   // (the world's menu)
    if (const NameHit* hit = hitAt(x, y))
    {
        const NameHit target = *hit;   // (the menu may redraw the panel)
        if (target.object)
        {
            showObjectMenu(target, x, y);
        }
        else
        {
            // the people list's menu (nearby: zoom, map, pay, teleports, block, report, copy...)
            LLPanelPeopleMenus::gNearbyPeopleContextMenu.show(this, uuid_vec_t{ target.id }, x, y);
        }
    }
    return true;   // (the panel's: no world menu through it)
}

bool GMDXChatLog::handleMouseDown(S32 x, S32 y, MASK mask)
{
    // click-through outside the panel (unlocked: and the chat bar under it, which then moves with it)
    if (mPanelRect.isEmpty() || !(mUnlocked ? unitRect() : mPanelRect).pointInRect(x, y)) return false;
    if (onBarControl(x, y)) return false;
    if (gearRect().pointInRect(x, y))
    {
        SLChatBar* bar = LLFloaterReg::findTypedInstance<SLChatBar>("chatbar");
        LLMenuButton* button = bar ? bar->findChild<LLMenuButton>("chatbar_gear_menu") : nullptr;
        if (LLToggleableMenu* menu = button ? button->getMenu() : nullptr)
        {
            LLMenuGL::showPopup(this, menu, x, y);
        }
        return true;
    }
    if (mJumpRect.pointInRect(x, y))
    {
        mScroll = 0;
    }
    else if (const NameHit* hit = hitAt(x, y))
    {
        // a name: the chat bar set up to IM them (an object's name: just keep typing)
        if (!hit->object)
        {
            const NameHit target = *hit;
            SLChatBar::startIM(target.id, target.name);
            return true;
        }
    }
    else if (lockRect().pointInRect(x, y))
    {
        if (mUnlocked) lock();
        else mUnlocked = true;
    }
    else
    {
        bool on_tab = false;
        for (S32 t = 0; t < TAB_COUNT && !on_tab; ++t)
        {
            if (tabRect(t).pointInRect(x, y))
            {
                mTab = t;
                mScroll = 0;
                markTabSeen(t);
                on_tab = true;
            }
        }
        if (mUnlocked && !on_tab)
        {
            // move, or resize by the edge / corner grabbed
            mDragging = true;
            mDragEdges = grabEdges(x, y);
            mDragX = x;
            mDragY = y;
            mDragFrom = mDragRect = mPanelRect;
            gFocusMgr.setMouseCapture(this);
            return true;
        }
    }
    // keep typing: the panel belongs to the chat bar
    if (!typing()) SLChatBar::startChat(nullptr);
    return true;
}

bool GMDXChatLog::handleMouseUp(S32 x, S32 y, MASK mask)
{
    if (!mDragging) return false;
    if (hasMouseCapture()) gFocusMgr.setMouseCapture(nullptr);   // (ends the drag: onMouseCaptureLost)
    onMouseCaptureLost();
    return true;
}

void GMDXChatLog::onMouseCaptureLost()
{
    if (!mDragging) return;
    mDragging = false;
    if (mDragRect != mDragFrom) gSavedSettings.setRect("GMDXChatLogRect", mDragRect);   // (a click leaves it where it is)
}

bool GMDXChatLog::handleScrollWheel(S32 x, S32 y, S32 clicks)
{
    if (mPanelRect.isEmpty() || !mPanelRect.pointInRect(x, y)) return false;
    mScroll = llmax(mScroll - clicks * 3, 0);
    return true;
}

bool GMDXChatLog::handleHover(S32 x, S32 y, MASK mask)
{
    if (!mDragging && onBarControl(x, y)) return false;
    if (!mDragging && hitAt(x, y))
    {
        getWindow()->setCursor(UI_CURSOR_HAND);   // (and underlined, draw)
        return true;
    }
    const U32 edges = mDragging ? mDragEdges
                    : mUnlocked && unitRect().pointInRect(x, y) && !lockRect().pointInRect(x, y) && !gearRect().pointInRect(x, y)
                                  && !mJumpRect.pointInRect(x, y)
                          ? grabEdges(x, y) : ~0u;
    if (mDragging)
    {
        const LLRect area = panelArea();   // (the panel's: the bar's room is under it)
        const S32 dx = x - mDragX, dy = y - mDragY;
        LLRect r = mDragFrom;
        if (!edges)
        {
            r.translate(dx, dy);
        }
        else
        {
            // the dragged edges follow the mouse, inside the floater area, no closer than the smallest size
            if (edges & EDGE_LEFT) r.mLeft = llclamp(r.mLeft + dx, area.mLeft, r.mRight - minWidth());
            if (edges & EDGE_RIGHT) r.mRight = llclamp(r.mRight + dx, r.mLeft + minWidth(), area.mRight);
            if (edges & EDGE_BOTTOM) r.mBottom = llclamp(r.mBottom + dy, area.mBottom, r.mTop - minHeight());
            if (edges & EDGE_TOP) r.mTop = llclamp(r.mTop + dy, r.mBottom + minHeight(), area.mTop);
        }
        mDragRect = keepInView(r);
        placeBar();
    }
    if (edges != ~0u)
    {
        const bool h = edges & (EDGE_LEFT | EDGE_RIGHT), v = edges & (EDGE_BOTTOM | EDGE_TOP);
        const bool nwse = (edges & EDGE_LEFT && edges & EDGE_TOP) || (edges & EDGE_RIGHT && edges & EDGE_BOTTOM);
        getWindow()->setCursor(h && v ? (nwse ? UI_CURSOR_SIZENWSE : UI_CURSOR_SIZENESW)
                               : h ? UI_CURSOR_SIZEWE : v ? UI_CURSOR_SIZENS : UI_CURSOR_SIZEALL);
        return true;
    }
    return !mPanelRect.isEmpty() && (mUnlocked ? unitRect() : mPanelRect).pointInRect(x, y);
}
