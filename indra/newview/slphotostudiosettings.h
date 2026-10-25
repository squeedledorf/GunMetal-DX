/**
 * @file slphotostudiosettings.h
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

#ifndef SL_PHOTOSTUDIOSETTINGS_H
#define SL_PHOTOSTUDIOSETTINGS_H

#include <map>
#include <string>

class LLSD;

/**
 * The table is app_settings/settings_renames_photostudio.xml (old -> new), and
 * this is everything that reads it. Self-contained so the general settings
 * rename (#126) can take the table over as data and drop this code.
 */
namespace SLPhotoStudioSettings
{
/// Old name -> new name, loaded once.
const std::map<std::string, std::string>& renames();

/// Once per user settings file: for each renamed setting the user's saved
/// settings.xml still has under its old name, copy the value to the new name
/// if that is still at its default, and log it. The old name is then no
/// longer saved. Idempotent (marker SLPhotoStudioSettingsMigrated). Call after
/// the user settings have been loaded.
void migrateRenamedSettings();

/// A Look or preset file's settings map (name -> { Value, ... }): give every
/// old name the file carries its new name too, unless the file has that
/// already. For the filtered Looks loader, before it reads the file.
void translateSettingsMap(LLSD& settings);

/// After gSavedSettings.loadFromFile() has applied a preset file: apply the
/// values the file holds under old names to the new names.
void applyRenamedFromFile(const std::string& filename);
}

#endif // SL_PHOTOSTUDIOSETTINGS_H
