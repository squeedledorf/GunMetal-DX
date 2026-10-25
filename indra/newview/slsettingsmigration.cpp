/**
 * @file slsettingsmigration.cpp
 * @brief Carries settings saved under their pre-1.2 Alchemy* names over to the
 *        Slipstream* names (#126).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
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

#include "slsettingsmigration.h"

#include "llcontrol.h"
#include "lldir.h"
#include "llsd.h"

namespace SLSettingsMigration
{
const char* const MARKER = "SlipstreamSettingsMigration";

const std::map<std::string, std::string>& renames()
{
    static const std::map<std::string, std::string> s_renames = []
    {
        std::map<std::string, std::string> out;
        const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "settings_renames.xml");
        LLSD doc;
        if (!LLControlGroup::readSettingsFile(path, doc) || !doc["renames"].isMap())
        {
            LL_WARNS("SettingsMigration") << "Cannot read " << path
                                          << "; settings saved under their old names will not carry over" << LL_ENDL;
            return out;
        }
        const LLSD& table = doc["renames"];
        for (LLSD::map_const_iterator it = table.beginMap(); it != table.endMap(); ++it)
        {
            out[it->first] = it->second.asString();
        }
        return out;
    }();
    return s_renames;
}

namespace
{
// The old names a user file still carries were declared as unknown controls when it
// loaded; keep them (a downgrade still finds its values) but out of the debug settings.
void hideOldNames(LLControlGroup& group)
{
    for (const auto& [old_name, new_name] : renames())
    {
        if (LLControlVariable* control = group.getControl(old_name).get())
        {
            control->setHiddenFromSettingsEditor(true);
        }
    }
}
}

unsigned migrateSettingsFile(LLControlGroup& group, const std::string& filename)
{
    hideOldNames(group);
    if (!group.controlExists(MARKER))
    {
        LL_WARNS("SettingsMigration") << MARKER << " is not declared in " << group.getKey()
                                      << "; not migrating " << filename << LL_ENDL;
        return 0;
    }
    // The marker is read from the file itself, not the group: the per-account group is not
    // reset between login attempts, so it may still hold another account's marker.
    LLSD file;
    const bool have_file = LLControlGroup::readSettingsFile(filename, file);
    if (have_file && file.has(MARKER) && file[MARKER]["Value"].asInteger() >= (LLSD::Integer)VERSION)
    {
        LL_DEBUGS("SettingsMigration") << filename << " already migrated (" << MARKER << " "
                                       << file[MARKER]["Value"].asInteger() << ")" << LL_ENDL;
        return 0;
    }

    const unsigned migrated = have_file ? group.migrateRenamedControls(file, renames(), true, filename) : 0;
    // Also for a file that does not exist yet (new user) or had nothing to carry over:
    // the next start skips the scan.
    group.setU32(MARKER, VERSION);
    LL_INFOS("SettingsMigration") << "Renamed settings: carried " << migrated << " value(s) over from old names in "
                                  << (filename.empty() ? std::string("(no file)") : filename) << LL_ENDL;
    return migrated;
}

void migrateNearbyChatOverlay(LLControlGroup& group, const std::string& filename)
{
    if (auto* control = group.getControl("SlipstreamNearbyToastsWithBubbles").get())
    {
        control->setHiddenFromSettingsEditor(true);
    }
    if (group.getBOOL("NearbyChatOverlayMigrated"))
    {
        return;
    }
    LLSD file;
    if (LLControlGroup::readSettingsFile(filename, file) && !file.has("NearbyChatOverlayStyle"))
    {
        group.setString("NearbyChatOverlayStyle", "cards");
    }
    group.setBOOL("NearbyChatOverlayMigrated", true);
}

// <GMDX>
void migrateGMDXChatLog(LLControlGroup& group)
{
    if (group.getBOOL("GMDXChatLogMigrated"))
    {
        return;
    }
    group.setString("NearbyChatOverlayStyle", "gmdx");
    group.setBOOL("GMDXChatLogMigrated", true);
}

void migrateGMDXMouselookHud(LLControlGroup& group)
{
    if (group.getBOOL("GMDXMouselookHudMigrated"))
    {
        return;
    }
    group.setBOOL("SlipstreamMouselookInstructions", false);   // "Press ESC to return to World View"
    group.setBOOL("SlipstreamMouselookPosition", false);       // X / Y / Z at the top
    group.setBOOL("GMDXMouselookHudMigrated", true);
}
// </GMDX>

void migrateChatNotifications(LLControlGroup& group, const std::string& filename)
{
    static const char* const OLD_TASKBAR[] = { "SlipstreamFlashTaskbarOnIM", "SlipstreamFlashTaskbarOnConference",
                                               "SlipstreamFlashTaskbarOnGroupChat", "SlipstreamFlashTaskbarOnNearbyChat" };
    for (const char* name : OLD_TASKBAR)
    {
        if (LLControlVariable* control = group.getControl(name).get())
        {
            control->setHiddenFromSettingsEditor(true);
        }
    }
    if (group.getBOOL("ChatNotificationsMigrated"))
    {
        return;
    }
    LLSD file;
    if (LLControlGroup::readSettingsFile(filename, file))
    {
        // Values as the previous version saw them: saved in the file, or its defaults.
        auto old_value = [&file](const std::string& name, const LLSD& old_default)
        {
            return file.has(name) ? file[name]["Value"] : old_default;
        };
        const bool never_ping_nearby = old_value("ChatNeverPingNearby", false).asBoolean();
        const struct { const char* type; const char* taskbar; bool taskbar_default; } types[] = {
            { "FriendIM", OLD_TASKBAR[0], true },
            { "NonFriendIM", OLD_TASKBAR[0], true },
            { "ConferenceIM", OLD_TASKBAR[1], true },
            { "GroupChat", OLD_TASKBAR[2], false },
            { "NearbyChat", OLD_TASKBAR[3], false },
            { "ObjectIM", nullptr, false },
        };
        for (const auto& t : types)
        {
            const std::string prefix = std::string("Notification") + t.type;
            // ChatNeverPingNearby silenced all of nearby chat, objects included.
            const bool silenced = never_ping_nearby && (prefix == "NotificationNearbyChat" || prefix == "NotificationObjectIM");
            std::string show = old_value(prefix + "Options", "toast").asString();
            group.setBOOL(prefix + "FlashConversation", !silenced && show != "noaction");
            if (show == "flash" || (silenced && show == "openconversations"))
            {
                show = "noaction";
            }
            group.setString(prefix + "Options", show);
            if (t.taskbar)
            {
                group.setBOOL(prefix + "FlashTaskbar", !silenced && old_value(t.taskbar, t.taskbar_default).asBoolean());
            }
        }
        if (never_ping_nearby)
        {
            group.setBOOL("PlaySoundNearbyChatIM", false);
            group.setBOOL("PlaySoundObjectIM", false);
        }
        group.setBOOL("ChatPingOnlyUnfocused", old_value("ChatPingOnlyUnfocused", false).asBoolean());
        LL_INFOS("SettingsMigration") << "Converted chat notification preferences in " << filename << LL_ENDL;
    }
    group.setBOOL("ChatNotificationsMigrated", true);
}

unsigned migratePresetFile(LLControlGroup& group, const std::string& filename)
{
    hideOldNames(group);
    LLSD file;
    if (!LLControlGroup::readSettingsFile(filename, file))
    {
        return 0;
    }
    const unsigned migrated = group.migrateRenamedControls(file, renames(), false, filename);
    if (migrated)
    {
        LL_INFOS("SettingsMigration") << "Renamed settings: applied " << migrated
                                      << " value(s) saved under old names in preset " << filename << LL_ENDL;
    }
    return migrated;
}
}
