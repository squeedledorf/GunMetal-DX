/**
 * @file slphotostudiosettings.cpp
 * @brief Photo Studio's renamed settings: the one-time migration from the
 *        upstream Alchemy names, and translation of Looks and graphics
 *        presets saved under them.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors.
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
#include "slphotostudiosettings.h"

#include "llcontrol.h"
#include "lldir.h"
#include "llfile.h"
#include "llsd.h"
#include "llsdserialize.h"
#include "llviewercontrol.h"

namespace
{
const char* const TABLE_FILE = "settings_renames_photostudio.xml";
const char* const MIGRATED_MARKER = "SLPhotoStudioSettingsMigrated";
/// The Lightbox's own window state, dropped rather than renamed.
const char* const DROPPED_STATE = "ALLightboxState";

LLSD readLLSDFile(const std::string& filename)
{
    LLSD contents;
    llifstream file(filename.c_str());
    if (file.is_open())
    {
        LLSDSerialize::fromXML(contents, file);
    }
    return contents;
}

/// A value saved under an old name, from a settings map (name -> { Value }).
bool savedValue(const LLSD& settings, const std::string& name, LLSD& value)
{
    if (!settings.isMap() || !settings.has(name))
    {
        return false;
    }
    const LLSD& entry = settings[name];
    if (!entry.isMap() || !entry.has("Value"))
    {
        return false;
    }
    value = entry["Value"];
    return true;
}

/// The old name is history once its value has moved: stop saving it. (The
/// user settings loader keeps any name it does not know, so without this it
/// would be written back to settings.xml on every save.)
void retireOldName(const std::string& old_name)
{
    if (LLControlVariable* old_control = gSavedSettings.getControl(old_name))
    {
        old_control->setPersist(LLControlVariable::PERSIST_NO);
    }
}
} // namespace

const std::map<std::string, std::string>& SLPhotoStudioSettings::renames()
{
    static std::map<std::string, std::string> table;
    static bool loaded = false;
    if (!loaded)
    {
        loaded = true;
        const LLSD contents = readLLSDFile(gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, TABLE_FILE));
        if (!contents.isMap())
        {
            LL_WARNS("Settings") << "Photo Studio: could not read " << TABLE_FILE << LL_ENDL;
        }
        for (LLSD::map_const_iterator it = contents.beginMap(); it != contents.endMap(); ++it)
        {
            if (it->second.isString() && !it->second.asString().empty())
            {
                table[it->first] = it->second.asString();
            }
        }
    }
    return table;
}

void SLPhotoStudioSettings::migrateRenamedSettings()
{
    const auto& table = renames();

    LLControlVariable* marker = gSavedSettings.getControl(MIGRATED_MARKER);
    if (marker && marker->getValue().asBoolean())
    {
        // Done before. A name the loader kept from the file still must not be
        // saved again.
        for (const auto& [old_name, new_name] : table)
        {
            retireOldName(old_name);
        }
        retireOldName(DROPPED_STATE);
        return;
    }

    // Straight from the file, not from gSavedSettings: what the user saved
    // under each old name, whatever this build declares.
    const std::string path = gSavedSettings.getString("ClientSettingsFile");
    const LLSD saved = path.empty() ? LLSD() : readLLSDFile(path);

    for (const auto& [old_name, new_name] : table)
    {
        LLSD value;
        if (!savedValue(saved, old_name, value))
        {
            continue;
        }
        LLControlVariable* new_control = gSavedSettings.getControl(new_name);
        if (!new_control)
        {
            LL_WARNS("Settings") << "Photo Studio: rename target " << new_name << " is not declared" << LL_ENDL;
            continue;
        }
        // A new name that already holds a value of its own was set on purpose
        // (by this build, or a migration that has already run): leave it.
        if (!new_control->isDefault())
        {
            retireOldName(old_name);
            continue;
        }
        new_control->setValue(value);
        if (!new_control->isPersisted())
        {
            new_control->setPersist(LLControlVariable::PERSIST_NONDFT);
        }
        retireOldName(old_name);
        LL_INFOS("Settings") << "Photo Studio: migrated setting " << old_name << " -> " << new_name
                             << " (" << value << ")" << LL_ENDL;
    }

    // Not migrated: the Lightbox's tab and window state means nothing to the
    // panel layout. Only stop saving it.
    retireOldName(DROPPED_STATE);

    if (marker)
    {
        marker->setValue(LLSD(true));
    }
}

void SLPhotoStudioSettings::translateSettingsMap(LLSD& settings)
{
    if (!settings.isMap())
    {
        return;
    }
    for (const auto& [old_name, new_name] : renames())
    {
        if (settings.has(old_name) && !settings.has(new_name))
        {
            settings[new_name] = settings[old_name];
        }
    }
}

void SLPhotoStudioSettings::applyRenamedFromFile(const std::string& filename)
{
    const LLSD contents = readLLSDFile(filename);
    for (const auto& [old_name, new_name] : renames())
    {
        LLSD value;
        if (!savedValue(contents, old_name, value) || contents.has(new_name))
        {
            continue;
        }
        if (LLControlVariable* new_control = gSavedSettings.getControl(new_name))
        {
            new_control->setValue(value);
        }
        // loadFromFile declared the old name when it met it; it is not ours to
        // save.
        retireOldName(old_name);
    }
}
