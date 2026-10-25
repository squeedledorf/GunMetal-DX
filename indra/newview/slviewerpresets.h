/**
 * @file slviewerpresets.h
 * @brief Viewer presets (#132): make Slipstream feel like the viewer someone used before.
 *        One data file per viewer in app_settings/viewer_presets/ (settings + key bindings,
 *        format described in firestorm.xml). Applying one is reversible: the values it
 *        replaces are backed up once (FirstRunPresetBackup) and "slipstream" restores
 *        Slipstream's defaults. The welcome window (slfloaterfirstrun) drives it.
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

#ifndef SL_VIEWERPRESETS_H
#define SL_VIEWERPRESETS_H

#include "stdtypes.h"

#include <string>
#include <vector>

namespace slpresets
{
// Preset ids, in the order the welcome window lists them. They match the importer's viewer
// ids (slimport::SourceViewer::id), plus "slipstream" for Slipstream's own defaults.
const std::vector<std::string>& presetIds();
inline const std::string& defaultsId() { static const std::string id("slipstream"); return id; }

struct Result
{
    S32 changed = 0;                    // settings and key bindings that now differ from before
    std::vector<std::string> restart;   // changed settings that only take effect after a restart
};

// Apply a preset: back up what it replaces (first time only), then set the key bindings and
// the settings. Every setting or input any preset touches and this one does not name goes
// back to Slipstream's default. Saves settings.xml. False if the preset could not be read.
bool apply(const std::string& id, Result& result);

// Whether "put back how it was" has something to restore.
bool hasBackup();
// Restore the backed-up settings and key bindings, then forget the backup. Saves settings.xml.
bool undo(Result& result);
}

#endif
