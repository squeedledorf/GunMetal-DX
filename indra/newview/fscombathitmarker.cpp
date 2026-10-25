/**
 * @file fscombathitmarker.cpp
 * @brief Hitmarker and hit report for outgoing Combat 2.0 damage.
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

#include "fscombathitmarker.h"

#include "llagent.h"
#include "llaudioengine.h"
#include "llavatarnamecache.h"
#include "llfontgl.h"
#include "llframetimer.h"
#include "llrender.h"
#include "llsdjson.h"
#include "llui.h"
#include "lluiimage.h"
#include "llviewercontrol.h"
#include "llviewerwindow.h"
#include "slbridge.h"

#include <boost/json.hpp>

using DamageTypeInfo = FSCombatHitMarker::DamageTypeInfo;

namespace
{
constexpr char BRIDGE_USER[] = "hitmarker";

// Timing envelope (matches the user-tuned LSL original)
constexpr F64 BATCH_WINDOW   = 0.3;   // damage aggregation window
constexpr F64 FLASH_DURATION = 0.75;  // marker flash
constexpr F64 TEXT_HOLD      = 1.75;  // text fully opaque (flash + hold)
constexpr F64 TEXT_FADE      = 4.0;   // fade to zero after hold

// --- pending batch state --------------------------------------------------
bool     sBatching = false;
F64      sBatchStart = 0.0;
F32      sBatchDamage = 0.f;
S32      sBatchHits = 0;
S32      sLastType = 0;
LLUUID   sLastTarget;
bool     sLastIsAgent = true;   // the last hit landed on an avatar (objects are never named)
bool     sKillSeen = false;
LLUUID   sKillTarget;

// --- active display state -------------------------------------------------
F64          sFlashStart = -1000.0;
bool         sFlashIsKill = false;
F64          sTextStart = -1000.0;
// The report line is "<before><name><after>": the name is read from the cache at draw
// time, so a lookup that answers after the batch closed still shows the name.
bool         sTextShown = false;
std::string  sTextBefore;
LLUUID       sTextNameId;   // null: no name in the line
std::string  sTextAfter;

// Built-in damage type symbol defaults (user-curated set); type values and
// names per secondlife/lsl-definitions plus the community-adopted custom
// damage types 100-106 documented on the SL wiki's Combat 2.0 Damage Types
// page. Overridable via the FSHitMarkerEmojiMap setting (JSON, keyed by
// type number as a string).
const DamageTypeInfo DAMAGE_TYPES[] = {
    { -1, "Impact",      "\xF0\x9F\x92\xA5" },
    {  0, "Generic",     "\xF0\x9F\x92\xA5" },
    {  1, "Acid",        "\xF0\x9F\xA7\xAA" },
    {  2, "Bludgeoning", "\xF0\x9F\x94\xA8" },
    {  3, "Cold",        "\xE2\x9D\x84\xEF\xB8\x8F" },
    {  4, "Electric",    "\xE2\x9A\xA1\xEF\xB8\x8F" },
    {  5, "Fire",        "\xF0\x9F\x94\xA5" },
    {  6, "Force",       "\xE2\x98\x84\xEF\xB8\x8F" },
    {  7, "Necrotic",    "\xF0\x9F\x92\x80" },
    {  8, "Piercing",    "\xF0\x9F\x97\xA1" },
    {  9, "Poison",      "\xE2\x98\xA3\xEF\xB8\x8F" },
    { 10, "Psychic",     "\xF0\x9F\x94\xAE" },
    { 11, "Radiant",     "\xE2\x9C\xA8" },
    { 12, "Slashing",    "\xF0\x9F\x94\xAA" },
    { 13, "Sonic",       "\xF0\x9F\x94\x8A" },
    { 14, "Emotional",   "\xF0\x9F\x8E\xAD" },
    { 100, "Medical",     "\xF0\x9F\x92\x89" },
    { 101, "Repair",      "\xF0\x9F\x94\xA7" },
    { 102, "Explosive",   "\xF0\x9F\x92\xA3" },
    { 103, "Crushing",    "\xF0\x9F\xA4\x9C\xF0\x9F\xA4\x9B" },
    { 104, "Anti-Armor",  "\xF0\x9F\x92\xA5\xF0\x9F\x9B\xA1\xEF\xB8\x8F" },
    { 105, "Suffocation", "\xF0\x9F\x8C\xAC\xEF\xB8\x8F" },
    { 106, "Redeploy",    "\xF0\x9F\x92\xAA" },
};
}

// static
S32 FSCombatHitMarker::getDamageTypeCount()
{
    return (S32)(sizeof(DAMAGE_TYPES) / sizeof(DAMAGE_TYPES[0]));
}

// static
const DamageTypeInfo& FSCombatHitMarker::getDamageTypeInfo(S32 index)
{
    return DAMAGE_TYPES[llclamp(index, 0, getDamageTypeCount() - 1)];
}

// Damage type -> emoji, honoring the FSHitMarkerEmojiMap JSON override.
// static
std::string FSCombatHitMarker::getEmojiForType(S32 type)
{
    static std::string last_raw = "\x01"; // force first parse
    static LLSD overrides;

    static LLCachedControl<std::string> map_setting(gSavedSettings, "FSHitMarkerEmojiMap", "");
    if (map_setting() != last_raw)
    {
        last_raw = map_setting();
        overrides = LLSD();
        if (!last_raw.empty())
        {
            boost::system::error_code ec;
            boost::json::value jv = boost::json::parse(last_raw, ec);
            if (!ec && jv.is_object())
            {
                overrides = LlsdFromJson(jv);
            }
        }
    }

    const std::string key = llformat("%d", type);
    if (overrides.has(key))
    {
        return overrides[key].asString();
    }
    for (const auto& entry : DAMAGE_TYPES)
    {
        if (entry.mType == type)
        {
            return entry.mDefaultEmoji;
        }
    }
    return llformat("type %d", type);
}

// Read the override map setting as LLSD (empty map when unset/invalid).
static LLSD read_emoji_overrides()
{
    const std::string raw = gSavedSettings.getString("FSHitMarkerEmojiMap");
    if (!raw.empty())
    {
        boost::system::error_code ec;
        boost::json::value jv = boost::json::parse(raw, ec);
        if (!ec && jv.is_object())
        {
            return LlsdFromJson(jv);
        }
    }
    return LLSD::emptyMap();
}

// static
void FSCombatHitMarker::setEmojiOverride(S32 type, const std::string& symbol)
{
    LLSD overrides = read_emoji_overrides();
    const std::string key = llformat("%d", type);

    // Setting a symbol back to its built-in default clears the override.
    std::string default_symbol;
    for (const auto& entry : DAMAGE_TYPES)
    {
        if (entry.mType == type)
        {
            default_symbol = entry.mDefaultEmoji;
            break;
        }
    }

    if (symbol.empty() || symbol == default_symbol)
    {
        overrides.erase(key);
    }
    else
    {
        overrides[key] = symbol;
    }
    // Clearing the last override empties the setting entirely.
    std::string serialized;
    if (overrides.size() > 0)
    {
        serialized = boost::json::serialize(LlsdToJson(overrides));
    }
    gSavedSettings.setString("FSHitMarkerEmojiMap", serialized);
}

// static
void FSCombatHitMarker::clearEmojiOverrides()
{
    gSavedSettings.setString("FSHitMarkerEmojiMap", "");
}

// Ask the helper for the agent's own hits only while the hit marker is on; the helper
// is worn while this or another feature (the kill feed) needs it.
static void update_bridge_use()
{
    SLBridge& bridge = SLBridge::instance();
    if (gSavedSettings.getBOOL("FSHitMarkerEnabled"))
    {
        bridge.addUser(BRIDGE_USER);
        if (bridge.isReady())
        {
            bridge.sendControl("hits|1"); // otherwise sent when it becomes ready
        }
    }
    else
    {
        // Always queued (sent in order once ready, so a later "hits|1" still wins): the
        // helper may stay on for the kill feed, and one mid-handshake may still forward hits.
        bridge.sendControl("hits|0");
        bridge.removeUser(BRIDGE_USER);
    }
}

// static
void FSCombatHitMarker::init()
{
    static bool initialized = false;
    if (initialized)
    {
        return;
    }
    initialized = true;

    SLBridge& bridge = SLBridge::instance();
    bridge.registerHandler("hit", &FSCombatHitMarker::handleBridgeEvent);
    // A fresh helper script (login, reattach, reinstall) starts with the hits off.
    bridge.setStateCallback([](SLBridge::EState state)
        {
            // The current state either way: a lost hits|0 must not leave the hits on.
            if (state == SLBridge::STATE_READY)
            {
                SLBridge::instance().sendControl(gSavedSettings.getBOOL("FSHitMarkerEnabled") ? "hits|1" : "hits|0");
            }
        });
    // Again after a teleport or crossing (harmless if still on): the helper drops the
    // combat log when the viewer's pings stop for a minute, which a long teleport can cause.
    bridge.setReadyInRegionCallback([]()
        {
            SLBridge::instance().sendControl(gSavedSettings.getBOOL("FSHitMarkerEnabled") ? "hits|1" : "hits|0");
        });
    gSavedSettings.getControl("FSHitMarkerEnabled")->getSignal()->connect(
        [](LLControlVariable*, const LLSD&, const LLSD&) { update_bridge_use(); });
    update_bridge_use();
}

// allow_request: a cache miss fires a lookup (done once, when a line arrives); the per-frame
// draw only reads the cache.
static std::string hitmarker_name(const LLUUID& id, bool allow_request = true)
{
    if (id.isNull())
    {
        return "(unknown)";
    }
    LLAvatarName av_name;
    if (!LLAvatarNameCache::get(id, &av_name))
    {
        if (allow_request)
        {
            LLAvatarNameCache::get(id, [](const LLUUID&, const LLAvatarName&) {});
        }
        return "(resolving)";
    }
    std::string name = av_name.getDisplayName();
    static LLCachedControl<bool> short_names(gSavedSettings, "FSHitMarkerShortNames", true);
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

static void play_sound_setting_impl(const char* setting_name, bool respect_toggle)
{
    if (respect_toggle)
    {
        static LLCachedControl<bool> sounds_on(gSavedSettings, "FSHitMarkerSounds", true);
        if (!sounds_on)
        {
            return;
        }
    }
    if (!gAudiop)
    {
        return;
    }
    // An SL sound asset, fetched like any UI sound. Empty or malformed: silent.
    LLUUID sound_id;
    if (LLUUID::parseUUID(gSavedSettings.getString(setting_name), &sound_id) && sound_id.notNull())
    {
        static LLCachedControl<F32> gain(gSavedSettings, "FSHitMarkerSoundGain", 1.0f);
        gAudiop->triggerSound(sound_id, gAgent.getID(), llclamp((F32)gain, 0.f, 1.f), LLAudioEngine::AUDIO_TYPE_UI);
    }
}

static void play_sound_setting(const char* setting_name)
{
    play_sound_setting_impl(setting_name, true);
}

// static
void FSCombatHitMarker::playHitSoundPreview()
{
    play_sound_setting_impl("FSHitMarkerHitSound", false);
}

// static
void FSCombatHitMarker::playKillSoundPreview()
{
    play_sound_setting_impl("FSHitMarkerKillSound", false);
}

// Build the display text and arm the flash from the accumulated batch.
static void finalize_batch()
{
    sBatching = false;

    static LLCachedControl<bool> show_text(gSavedSettings, "FSHitMarkerText", true);
    static LLCachedControl<bool> show_name(gSavedSettings, "FSHitMarkerShowName", true);
    static LLCachedControl<bool> show_emoji(gSavedSettings, "FSHitMarkerShowEmoji", true);

    sFlashStart = LLFrameTimer::getTotalSeconds();
    sFlashIsKill = sKillSeen;

    if (show_text)
    {
        std::string before, after;
        LLUUID name_id;
        if (sKillSeen)
        {
            before = "Killed";
            if (show_name)
            {
                before += " ";
                name_id = sKillTarget;
            }
        }
        else
        {
            const bool named = show_name && sLastIsAgent;
            if (named)
            {
                name_id = sLastTarget;
            }
            if (show_emoji)
            {
                if (named) after += " ";
                after += FSCombatHitMarker::getEmojiForType(sLastType);
            }
            if (named || show_emoji) after += " ";
            after += llformat("(%.0fdmg", sBatchDamage);
            if (sBatchHits > 1)
            {
                after += llformat(", %d hits", sBatchHits);
            }
            after += ")";
        }
        sTextBefore = before;
        sTextNameId = name_id;
        sTextAfter = after;
        sTextShown = true;
        sTextStart = sFlashStart;
    }

    // Reset batch accumulators
    sBatchDamage = 0.f;
    sBatchHits = 0;
    sKillSeen = false;
    sKillTarget.setNull();
}

// Finalize a pending batch once its aggregation window closes, but never over a kill that is
// still flashing (it waits, still accumulating, until the red flash is over).
static void finalize_batch_if_due(F64 now)
{
    // A batch nobody saw close (UI hidden, window minimised) is old news by now: drop it
    // rather than flash a marker for a hit long past.
    if (sBatching && now - sBatchStart > BATCH_WINDOW + FLASH_DURATION + 0.5)
    {
        sBatching = false;
        sBatchDamage = 0.f;
        sBatchHits = 0;
        sKillSeen = false;
        sKillTarget.setNull();
        return;
    }
    if (sBatching && now - sBatchStart >= BATCH_WINDOW
        && !(sFlashIsKill && now - sFlashStart < FLASH_DURATION))
    {
        finalize_batch();
    }
}

// static
void FSCombatHitMarker::handleBridgeEvent(const std::string& line)
{
    static LLCachedControl<bool> enabled(gSavedSettings, "FSHitMarkerEnabled", false);
    if (!enabled)
    {
        return;
    }

    // <D, d or K>|<target>|<agent>|<type>|<damage>; type and damage may be empty.
    std::string fields[5];
    size_t start = 0;
    for (size_t i = 0; i < 5; ++i)
    {
        const size_t bar = line.find('|', start);
        if (bar == std::string::npos && i < 4)
        {
            return; // malformed
        }
        fields[i] = line.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
        start = bar + 1;
    }

    // The helper forwards only the agent's own damage, never self-damage; check anyway.
    LLUUID target;
    if (!LLUUID::parseUUID(fields[1], &target) || target.isNull() || target == gAgent.getID())
    {
        return; // self-damage is not a "hit"
    }

    // Object targets (your damage landing on a prim, not an avatar) should not
    // flash the marker or play a sound. The helper tags each event with whether
    // the target is an agent (llGetAgentSize).
    static LLCachedControl<bool> agent_only(gSavedSettings, "FSHitMarkerAgentTargetsOnly", true);
    const bool is_agent = fields[2] != "0";
    if (agent_only && !is_agent)
    {
        return;
    }
    if (is_agent)
    {
        hitmarker_name(target); // warm the name cache now; the report reads it when drawn
    }

    if (fields[0] == "K")
    {
        sKillSeen = true;
        sKillTarget = target;
        play_sound_setting("FSHitMarkerKillSound");
        // Kills display immediately; no reason to wait out the batch window.
        finalize_batch();
    }
    else if (fields[0] == "D" || fields[0] == "d")   // "d": a hit that is part of a kill in the same log message
    {
        S32 dmg_type = 0;
        LLStringUtil::convertToS32(fields[3], dmg_type);

        // Generic LL physics/collision damage (type -1, "Impact") comes from
        // bumps and falls, not scripted combat. Optionally drop it so it does
        // not flash a hitmarker or play a hit sound.
        static LLCachedControl<bool> ignore_collision(gSavedSettings, "FSHitMarkerIgnoreCollision", true);
        if (ignore_collision && dmg_type == -1)
        {
            return;
        }

        // A batch whose window closed while nothing drew (UI hidden) ends here, so this hit
        // starts a new one and plays its sound.
        finalize_batch_if_due(LLFrameTimer::getTotalSeconds());

        F32 damage = 0.f;
        LLStringUtil::convertToF32(fields[4], damage);
        sBatchDamage += damage;
        ++sBatchHits;
        sLastTarget = target;
        sLastIsAgent = is_agent;
        sLastType = dmg_type;
        if (!sBatching)
        {
            sBatching = true;
            sBatchStart = LLFrameTimer::getTotalSeconds();
            // Sound feedback is instant; only the text waits for the batch. A hit that is
            // part of a kill ("d", e.g. a one-shot kill) leaves the sound to the kill.
            if (fields[0] == "D")
            {
                play_sound_setting("FSHitMarkerHitSound");
            }
        }
    }
}

// static
void FSCombatHitMarker::draw()
{
    const F64 now = LLFrameTimer::getTotalSeconds();

    finalize_batch_if_due(now);

    const F64 flash_t = now - sFlashStart;
    const F64 text_t = now - sTextStart;
    const bool flashing = flash_t >= 0.0 && flash_t < FLASH_DURATION;
    const bool texting = sTextShown && text_t >= 0.0 && text_t < TEXT_HOLD + TEXT_FADE;
    if (!flashing && !texting)
    {
        return;
    }

    static LLCachedControl<F32> scale_setting(gSavedSettings, "FSHitMarkerScale", 1.0f);
    const F32 scale = llclamp((F32)scale_setting, 0.5f, 3.f);
    // The world view's centre, as the crosshair uses: toolbars can move the world view.
    const LLRect& world_rect = gViewerWindow->getWorldViewRectScaled();
    const S32 center_x = world_rect.getCenterX();
    const S32 center_y = world_rect.getCenterY();

    // Hitmarker flash
    if (flashing)
    {
        LLUIImagePtr marker = LLUI::getUIImage("FSHitMarker");
        if (marker)
        {
            const F32 alpha = 1.f - (F32)(flash_t / FLASH_DURATION);
            LLColor4 color = sFlashIsKill ? LLColor4(1.f, 0.f, 0.f, alpha) : LLColor4(1.f, 1.f, 1.f, alpha);
            const S32 w = (S32)(marker->getWidth() * scale);
            const S32 h = (S32)(marker->getHeight() * scale);
            marker->draw(center_x - w / 2, center_y - h / 2, w, h, color);
        }
    }

    // Fading hit report text (independent scale and weight)
    if (texting)
    {
        static LLCachedControl<F32> text_scale_setting(gSavedSettings, "FSHitMarkerTextScale", 1.0f);
        static LLCachedControl<bool> text_bold(gSavedSettings, "FSHitMarkerTextBold", true);
        const F32 text_scale = llclamp((F32)text_scale_setting, 0.5f, 3.f);

        F32 alpha = 1.f;
        if (text_t > TEXT_HOLD)
        {
            alpha = 1.f - (F32)((text_t - TEXT_HOLD) / TEXT_FADE);
        }
        const LLFontGL* font = LLFontGL::getFontSansSerif();
        const U8 style = text_bold ? LLFontGL::BOLD : LLFontGL::NORMAL;
        LLColor4 color = sFlashIsKill ? LLColor4(1.f, 0.2f, 0.2f, alpha) : LLColor4(1.f, 1.f, 1.f, alpha);

        gGL.pushMatrix();
        // Whole pixels, see FSFloaterKillFeed::drawOverlay()
        gGL.translatef((F32)center_x, (F32)ll_round((F32)center_y - 32.f * scale), 0.f);
        gGL.scalef(text_scale, text_scale, 1.f);
        std::string text = sTextBefore;
        if (sTextNameId.notNull())
        {
            text += hitmarker_name(sTextNameId, false);
        }
        text += sTextAfter;
        font->renderUTF8(text, 0, 0.f, 0.f, color,
                         LLFontGL::HCENTER, LLFontGL::TOP, style, LLFontGL::DROP_SHADOW_SOFT);
        gGL.popMatrix();
    }
}
