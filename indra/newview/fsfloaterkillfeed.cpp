/**
 * @file fsfloaterkillfeed.cpp
 * @brief Kill feed: Combat 2.0 DEATH events shown as a text-only screen
 *        overlay, fed by the bridge's combat log listener.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
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
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "fsfloaterkillfeed.h"

#include "fscombathitmarker.h"
#include "gmdxchatlog.h" // <GMDX>
#include "llagent.h"
#include "llavatarnamecache.h"
#include "llfloaterreg.h"
#include "llrender.h"
#include "llviewercontrol.h"
#include "llviewerwindow.h"
#include "lggcontactsets.h"
#include "llgroupcolormap.h"
#include "llviewerobjectlist.h"
#include "llvoavatar.h"
#include "slbridge.h"

static const size_t MAX_ENTRIES = 200;
static const char BRIDGE_USER[] = "killfeed";

FSFloaterKillFeed::entries_t FSFloaterKillFeed::sEntries;

FSFloaterKillFeed::FSFloaterKillFeed(const LLSD& key)
:   LLFloater(key)
{
}

// static
void FSFloaterKillFeed::init()
{
    static bool initialized = false;
    if (initialized)
    {
        return;
    }
    initialized = true;

    SLBridge& bridge = SLBridge::instance();
    bridge.registerHandler("combat", &FSFloaterKillFeed::handleBridgeEvent);
    // A fresh helper script (login, reattach, reinstall) starts with the combat log off.
    bridge.setStateCallback([](SLBridge::EState state)
        {
            // The current state either way: a lost combat|0 must not leave the feed on.
            if (state == SLBridge::STATE_READY)
            {
                SLBridge::instance().sendControl(gSavedSettings.getBOOL("FSKillFeedEnabled") ? "combat|1" : "combat|0");
            }
        });
    // Again after a teleport or crossing (harmless if it is still on): the helper drops the
    // combat log when the viewer's pings stop for a minute, which a long teleport can cause.
    bridge.setReadyInRegionCallback([]()
        {
            SLBridge::instance().sendControl(gSavedSettings.getBOOL("FSKillFeedEnabled") ? "combat|1" : "combat|0");
        });
    gSavedSettings.getControl("FSKillFeedEnabled")->getSignal()->connect(
        [](LLControlVariable*, const LLSD&, const LLSD&) { updateBridgeUse(); });
    updateBridgeUse();
}

// static
void FSFloaterKillFeed::updateBridgeUse()
{
    SLBridge& bridge = SLBridge::instance();
    if (gSavedSettings.getBOOL("FSKillFeedEnabled"))
    {
        bridge.addUser(BRIDGE_USER);
        if (bridge.isReady())
        {
            bridge.sendControl("combat|1"); // otherwise sent when it becomes ready
        }
    }
    else
    {
        // The helper may stay on for another feature: stop its combat log listen first.
        // Always queued (sent once the helper is ready, in order, so a later "combat|1"
        // still wins): a helper mid-handshake may still have the old script listening.
        bridge.sendControl("combat|0");
        bridge.removeUser(BRIDGE_USER);
    }
}

// static
void FSFloaterKillFeed::clearEntries()
{
    sEntries.clear();
}

// Apply the FSKillFeedShortNames setting: "Orion Starchild" becomes
// "Orion S.". Used for real names and the settings-panel example alike.
static std::string killfeed_maybe_shorten(std::string name)
{
    static LLCachedControl<bool> short_names(gSavedSettings, "FSKillFeedShortNames", true);
    if (short_names)
    {
        size_t last_space = name.find_last_of(' ');
        if (last_space != std::string::npos && last_space + 1 < name.size())
        {
            name = name.substr(0, name.find_first_of(' ') + 1) + name[last_space + 1] + ".";
        }
    }
    return name;
}

// Avatar display name (shortening setting applied), or a resolving
// placeholder. When allow_request is true a cache miss fires an async
// lookup; the per-frame overlay passes false since lookups are kicked
// once when the entry arrives.
static std::string killfeed_name(const LLUUID& id, bool allow_request = true)
{
    if (id.isNull())
    {
        return "(world)";
    }
    LLAvatarName av_name;
    if (LLAvatarNameCache::get(id, &av_name))
    {
        return killfeed_maybe_shorten(av_name.getDisplayName());
    }
    if (allow_request)
    {
        // The overlay reads the cache each frame; the name shows once it arrives.
        LLAvatarNameCache::get(id, [](const LLUUID&, const LLAvatarName&) {});
    }
    return "(resolving)";
}

// Live world position of the avatar behind a kill-feed id (self, or any rezzed avatar in the
// object list). Used for the killer->victim distance; the combat event's source_pos is the
// killing OBJECT (next to the victim at impact), not the killer avatar.
static bool killfeed_avatar_pos(const LLUUID& id, LLVector3d& out)
{
    if (id.isNull())
    {
        return false;
    }
    if (id == gAgent.getID())
    {
        out = gAgent.getPositionGlobal();
        return true;
    }
    // mKiller can carry the killing OBJECT's UUID (owner-less/deeded kills); a non-avatar
    // match would re-introduce the bogus near-zero distance this function exists to avoid.
    LLViewerObject* obj = gObjectList.findObject(id);
    if (obj && obj->asAvatar())
    {
        out = obj->getPositionGlobal();
        return true;
    }
    return false;
}

static void killfeed_to_chat_log(const FSFloaterKillFeed::KillEntry& entry);   // <GMDX> (below)

// static
void FSFloaterKillFeed::handleBridgeEvent(const std::string& line)
{
    // Lines in flight when the feed was turned off (or after a lost combat|0) are dropped.
    static LLCachedControl<bool> enabled(gSavedSettings, "FSKillFeedEnabled", false);
    if (!enabled)
    {
        return;
    }

    // <victim>|<killer>|<type>|<damage>|<weapon>; the weapon is last and may hold '|'.
    std::string fields[4];
    size_t start = 0;
    for (std::string& field : fields)
    {
        const size_t bar = line.find('|', start);
        if (bar == std::string::npos)
        {
            return; // malformed
        }
        field = line.substr(start, bar - start);
        start = bar + 1;
    }

    KillEntry entry;
    if (!LLUUID::parseUUID(fields[0], &entry.mVictim) || entry.mVictim.isNull())
    {
        return;
    }
    LLUUID::parseUUID(fields[1], &entry.mKiller);
    entry.mWeapon = line.substr(start);
    // The DEATH event's damage type; -1 (Impact) is a
    // valid type, so the unknown sentinel sits outside the real range.
    S32 type = 0;
    entry.mType = LLStringUtil::convertToS32(fields[2], type) ? type : -1000;
    F32 damage = 0.f;
    entry.mDamage = LLStringUtil::convertToF32(fields[3], damage) ? damage : -1.f;

    // Distance is KILLER avatar -> VICTIM avatar, NOT the killing object -> victim. The combat
    // event's source_pos is the projectile/weapon object (right next to the victim at impact, so it
    // read as ~0m); use the two avatars' live world positions instead. If either avatar is not
    // loaded (e.g. a distant killer), leave the distance unset rather than show a bogus one.
    entry.mDistance = -1.f;
    LLVector3d killer_pos, victim_pos;
    if (killfeed_avatar_pos(entry.mKiller, killer_pos) && killfeed_avatar_pos(entry.mVictim, victim_pos))
    {
        entry.mDistance = (F32)(killer_pos - victim_pos).length();
    }

    entry.mReceivedTime = LLFrameTimer::getTotalSeconds();

    // Warm the name cache now so the overlay (which never fires requests)
    // has names by the time it draws.
    killfeed_name(entry.mVictim);
    killfeed_name(entry.mKiller);

    sEntries.push_back(entry);
    killfeed_to_chat_log(entry);   // <GMDX>
    while (sEntries.size() > MAX_ENTRIES)
    {
        sEntries.pop_front();
    }
}

struct KFSegment
{
    std::string text;
    LLColor4    color;
};

// Resolve a kill-feed name color. Your own name uses your chosen color when
// FSKillFeedColorOwnName is on (kill feed only); everyone else - and you when
// that toggle is off - follows the contact set color, else the default.
static LLColor4 killfeed_name_color(const LLUUID& id, const LLColor4& default_color)
{
    if (id.isNull())
    {
        return default_color;
    }

    if (id == gAgent.getID())
    {
        static LLCachedControl<bool> color_own(gSavedSettings, "FSKillFeedColorOwnName", false);
        if (color_own)
        {
            static LLCachedControl<LLColor4> own_color(gSavedSettings, "FSKillFeedOwnNameColor", LLColor4(1.f, 0.9f, 0.4f, 1.f));
            return own_color;
        }
    }

    static LLCachedControl<bool> color_others(gSavedSettings, "FSKillFeedColorOtherNames", true);
    if (!color_others)
    {
        return default_color;
    }

    // SoapStorm's order minus the minimap mark (Slipstream has no marks): contact set, then group.
    LLColor4 c;
    if (LGGContactSets::instance().hasFriendColorThatShouldShow(id, ContactSetType::MINIMAP, c))
    {
        return c; // contact set
    }
    return LLGroupColorMap::getInstance()->tint(id, default_color); // group
}

// "Killer killed Victim with [symbol] Weapon (100dmg, 50m)" split into colored
// segments so the killer and victim names can be tinted independently of the
// surrounding text (which uses default_color).
static std::vector<KFSegment> killfeed_segments(const FSFloaterKillFeed::KillEntry& entry, const LLColor4& default_color)
{
    std::vector<KFSegment> segs;
    // The killer can be a group (deeded weapon) or an object (owner-less kill): the avatar name
    // lookup never answers those, so after a few seconds name the weapon instead.
    std::string killer = killfeed_name(entry.mKiller, false);
    if (killer == "(resolving)" && LLFrameTimer::getTotalSeconds() - entry.mReceivedTime > 5.0)
    {
        killer = entry.mWeapon.empty() ? std::string("(unknown)") : entry.mWeapon;
    }
    segs.push_back({ killer, killfeed_name_color(entry.mKiller, default_color) });
    segs.push_back({ " killed ", default_color });
    segs.push_back({ killfeed_name(entry.mVictim, false), killfeed_name_color(entry.mVictim, default_color) });

    std::string tail;

    // Damage type symbol from the shared hit marker symbol table.
    static LLCachedControl<bool> show_emoji(gSavedSettings, "FSKillFeedShowEmoji", true);
    std::string symbol;
    if (show_emoji && entry.mType > -1000)
    {
        symbol = FSCombatHitMarker::getEmojiForType(entry.mType);
    }

    if (!entry.mWeapon.empty() || !symbol.empty())
    {
        tail += " with";
        if (!symbol.empty())
        {
            tail += " " + symbol;
        }
        if (!entry.mWeapon.empty())
        {
            tail += " " + entry.mWeapon;
        }
    }

    std::string stats;
    if (entry.mDamage >= 0.f)
    {
        stats = llformat("%.0fdmg", entry.mDamage);
    }
    if (entry.mDistance >= 0.f)
    {
        if (!stats.empty())
        {
            stats += ", ";
        }
        stats += llformat("%.0fm", entry.mDistance);
    }
    if (!stats.empty())
    {
        tail += " (" + stats + ")";
    }

    if (!tail.empty())
    {
        segs.push_back({ tail, default_color });
    }
    return segs;
}

// <GMDX> the chat log's Combat tab (gmdxchatlog.h): the same line, its names resolved when drawn
static void killfeed_to_chat_log(const FSFloaterKillFeed::KillEntry& entry)
{
    GMDXChatLog::addCombat([entry]()
        {
            static LLCachedControl<LLColor4> text_color(gSavedSettings, "FSKillFeedTextColor", LLColor4::white);
            std::vector<GMDXChatLog::Segment> out;
            for (const KFSegment& seg : killfeed_segments(entry, text_color)) out.push_back({ seg.text, seg.color });
            return out;
        });
}
// </GMDX>

// static
void FSFloaterKillFeed::drawOverlay()
{
    static LLCachedControl<bool> enabled(gSavedSettings, "FSKillFeedEnabled", false);

    // The settings panel shows a live example so position, font and color
    // can be customized without waiting for someone to die.
    const bool panel_open = LLFloaterReg::instanceVisible("fs_kill_feed");
    if (!enabled && !panel_open)
    {
        return;
    }

    static LLCachedControl<F32> text_scale(gSavedSettings, "FSKillFeedTextScale", 1.0f);
    static LLCachedControl<F32> screen_x(gSavedSettings, "FSKillFeedScreenX", 0.25f);
    static LLCachedControl<F32> screen_y(gSavedSettings, "FSKillFeedScreenY", 0.9f);
    static LLCachedControl<U32> max_lines(gSavedSettings, "FSKillFeedMaxLines", 5);
    static LLCachedControl<F32> hold_time(gSavedSettings, "FSKillFeedHoldTime", 20.f);
    static LLCachedControl<std::string> font_name(gSavedSettings, "FSKillFeedFontName", "SansSerif");
    static LLCachedControl<LLColor4> text_color(gSavedSettings, "FSKillFeedTextColor", LLColor4::white);
    static LLCachedControl<bool> bold_text(gSavedSettings, "FSKillFeedBoldText", false);
    static LLCachedControl<bool> grow_up(gSavedSettings, "FSKillFeedGrowUp", true);

    const F64 now = LLFrameTimer::getTotalSeconds();
    const LLColor4 default_color = text_color;

    // Collect the visible lines (newest first) as colored segments.
    std::vector<std::vector<KFSegment>> seglines;
    if (enabled)
    {
        for (entries_t::const_reverse_iterator it = sEntries.rbegin();
             it != sEntries.rend() && seglines.size() < (size_t)max_lines;
             ++it)
        {
            // Hold (sec) of 0 means "never fade" -- keep the newest lines on screen
            // forever (capped by Max lines / MAX_ENTRIES), so skip the age cutoff.
            if (hold_time > 0.f && now - it->mReceivedTime > (F64)hold_time)
            {
                break; // entries are time-ordered; everything older follows
            }
            seglines.push_back(killfeed_segments(*it, default_color));
        }
    }

    if (seglines.empty())
    {
        if (!panel_open)
        {
            return;
        }
        // Placeholder preview while the settings panel is open; runs
        // through the same name shortening and symbol option as real lines.
        static LLCachedControl<bool> example_emoji(gSavedSettings, "FSKillFeedShowEmoji", true);
        std::string example_symbol;
        if (example_emoji)
        {
            example_symbol = FSCombatHitMarker::getEmojiForType(5) + " "; // Fire
        }
        // Make the agent the killer so the own-name color shows live as it is
        // tuned; the victim stays a generic example in the default color.
        std::vector<KFSegment> example_segs;
        example_segs.push_back({ killfeed_name(gAgent.getID(), false), killfeed_name_color(gAgent.getID(), default_color) });
        example_segs.push_back({ " killed ", default_color });
        example_segs.push_back({ killfeed_maybe_shorten("Guy Linden"), default_color });
        example_segs.push_back({ " with " + example_symbol + "Death (100dmg, 0m)", default_color });
        seglines.assign((size_t)llmax((U32)1, (U32)max_lines), example_segs);
    }

    const LLFontGL* font = LLFontGL::getFontSansSerif();
    if (font_name() == "SansSerifSmall")
    {
        font = LLFontGL::getFontSansSerifSmall();
    }
    else if (font_name() == "SansSerifBig")
    {
        font = LLFontGL::getFontSansSerifBig();
    }
    else if (font_name() == "SansSerifHuge")
    {
        font = LLFontGL::getFontSansSerifHuge();
    }
    else if (font_name() == "Monospace")
    {
        font = LLFontGL::getFontMonospace();
    }

    const U8 font_style = bold_text ? LLFontGL::BOLD : LLFontGL::NORMAL;

    const S32 view_width = gViewerWindow->getWorldViewRectScaled().getWidth();
    const S32 view_height = gViewerWindow->getWorldViewRectScaled().getHeight();
    const F32 line_height = (F32)(font->getLineHeight() + 2);
    const F32 scale = llclamp((F32)text_scale, 0.5f, 3.f);

    gGL.pushMatrix();
    // <SS:KillFeed> Whole pixels: LLFontGL snaps each glyph to the pixel grid in its own
    // coordinates, so a fractional origin here (a screen fraction of the view, which also
    // changes height going in and out of mouselook) put every glyph between pixel rows and
    // clipped their edges.
    gGL.translatef((F32)ll_round((F32)view_width * llclamp((F32)screen_x, 0.f, 1.f)),
                   (F32)ll_round((F32)view_height * llclamp((F32)screen_y, 0.f, 1.f)), 0.f);
    gGL.scalef(scale, scale, 1.f);

    // The block ALWAYS occupies max_lines rows stacking down from FSKillFeedScreenX/Y, whichever
    // way it grows, so toggling the direction never moves the feed on screen. Reserving the full
    // height (instead of sizing to the current line count) also keeps the newest line put as the
    // feed fills: grow down puts it in the TOP row, grow up in the BOTTOM row with older lines
    // stacking upward above it. seglines[0] is always the newest; the marker (left gutter, so it
    // never shifts the text) points it out.
    //
    // U+2022 and not a triangle: Ubuntu, Droid, OpenDyslexic, NotoMono and ocra all lack
    // U+25B2/25BC, and the fallback chain those font sets declare only reaches DejaVuSans on
    // Linux -- a triangle renders as tofu for those skins on Windows and Mac. U+2022 is in every
    // bundled font. Direction needs no glyph of its own: the marker sits in the top row growing
    // down and the bottom row growing up.
    const std::string newest_marker = "\xE2\x80\xA2"; // U+2022 BULLET
    const F32 marker_gutter = font->getWidthF32(newest_marker) + 4.f;
    const size_t n_lines = seglines.size();
    const size_t n_rows = llmax((size_t)(U32)max_lines, n_lines); // reserved block height
    for (size_t idx = 0; idx < n_lines; ++idx) // idx 0 = the newest line
    {
        const F32 y = -(F32)(grow_up ? (n_rows - 1 - idx) : idx) * line_height;
        if (idx == 0)
        {
            font->renderUTF8(newest_marker, 0, -marker_gutter, y, default_color,
                             LLFontGL::LEFT, LLFontGL::TOP, font_style, LLFontGL::DROP_SHADOW_SOFT);
        }
        F32 x = 0.f;
        for (const KFSegment& seg : seglines[idx])
        {
            font->renderUTF8(seg.text, 0, x, y, seg.color,
                             LLFontGL::LEFT, LLFontGL::TOP, font_style, LLFontGL::DROP_SHADOW_SOFT);
            x += font->getWidthF32(seg.text);
        }
    }

    gGL.popMatrix();
}
