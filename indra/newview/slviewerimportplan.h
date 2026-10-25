/**
 * @file slviewerimportplan.h
 * @brief Viewer importer (#131, #231, #232): everything we can carry over from another
 *        viewer's data directory, as a plan of reviewable items per category, then the
 *        apply / undo steps.
 *
 * What maps to what lives in app_settings/viewer_import_map.xml (renames, value
 * conversions, the deny list, the renderer allowlist, ranges, toolbar and key command
 * renames). Settings are offered when the key exists here (directly or through the map)
 * with a compatible type and an in-range value that differs from ours; files are merged,
 * never replaced blindly. Every apply first takes a timestamped backup with an undo
 * journal (user_settings/import_backups/<stamp>/), so the last import can be undone.
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

#ifndef SL_VIEWERIMPORTPLAN_H
#define SL_VIEWERIMPORTPLAN_H

#include "llsd.h"
#include "slviewerimportcore.h"

#include <string>
#include <vector>

namespace slimport
{
enum ECategory
{
    CAT_SETTINGS = 0,       // settings.xml -> gSavedSettings
    CAT_ACCOUNT,            // settings_per_account.xml -> gSavedPerAccountSettings
    CAT_NOTIFICATIONS,      // ignorable_dialogs.xml -> gWarningSettings
    CAT_COLORS,             // user colors.xml -> LLUIColorTable
    CAT_LAYOUT,             // floater_rect_* / floater_pos_* window positions
    CAT_TOOLBARS,           // toolbars.xml (per account)
    CAT_KEYS,               // key_bindings.xml (Black Dragon: controls.xml), merged
    CAT_CONTACT_SETS,       // settings_friends_groups.xml (Firestorm contact sets)
    CAT_TELEPORT_HISTORY,   // teleport_history.txt (per account)
    CAT_GRAPHICS_PRESETS,   // presets/graphic/*.xml, compatible keys only
    CAT_CAMERA_PRESETS,     // presets/camera/*.xml; Black Dragon camera/*.xml converted
    CAT_POSES,              // Firestorm poser poses (poses/**)
    CAT_FONTS,              // user font sets (fonts/*)
    CAT_WINDLIGHT,          // legacy Windlight skies / water / day cycles (windlight/**)
    CAT_AUTOREPLACE,        // autoreplace.xml lists
    CAT_SPELLING,           // dictionaries/user_custom.dic, user_ignore.dic
    CAT_CHAT,               // chat transcripts + conversation.log (slviewerimportcore)
    CAT_COUNT
};

// Stable key ("settings", "account", ...): the floater looks up "cat_<key>" strings.
const char* categoryKey(ECategory cat);
// Categories whose rows are applied unless the user unticks the category.
bool categoryOnByDefault(ECategory cat);

enum EStatus
{
    STATUS_OFFER = 0,   // differs from ours; applied when ticked
    STATUS_CONFLICT,    // key binding already used here by another command (off by default)
    STATUS_SAME,        // already identical here (re-import is a no-op)
    STATUS_SKIP         // not importable; note says why
};

struct PlanItem
{
    ECategory   cat = CAT_SETTINGS;
    EStatus     status = STATUS_SKIP;
    bool        apply = false;
    std::string name;       // what the row shows
    std::string current;    // our value / state
    std::string imported;   // their value / state
    std::string note;       // rename, conversion, reason for skipping
    LLSD        data;       // what apply() needs
};

class ImportPlan
{
public:
    // Scan the source viewer (and one of its accounts, if any). logged_in: whether our
    // per-account data (settings, toolbars, contact sets, teleport history) is available.
    void build(const SourceViewer& viewer, const SourceAccount* account, bool logged_in);

    std::vector<PlanItem> mItems;
    bool                  mCategoryOn[CAT_COUNT];

    S32  count(ECategory cat, EStatus status) const;
    S32  countSelected(ECategory cat) const;   // rows that apply() would act on
    bool willApply(const PlanItem& item) const { return mCategoryOn[item.cat] && item.apply && (item.status == STATUS_OFFER || item.status == STATUS_CONFLICT); }

    // Apply every selected row. Backs up first; returns report lines. needs_restart is
    // set when something only takes full effect after a restart.
    std::vector<std::string> apply(bool& needs_restart);

private:
    void buildSettingsFile(const LLSD& file, bool account_file, bool logged_in);
    void buildNotifications();
    void buildColors();
    void buildToolbars();
    void buildKeys();
    void buildContactSets();
    void buildTeleportHistory();
    void buildPresets();
    void buildCopiedFiles(ECategory cat, const std::string& sub_dir, const std::vector<std::string>& extensions);
    void buildAutoReplace();
    void buildSpelling();
    void buildChat();

    SourceViewer  mViewer;
    SourceAccount mAccount;
    bool          mHasAccount = false;
    bool          mLoggedIn = false;
    LLSD          mSourceGlobal;    // their settings.xml
    LLSD          mSourceAccount;   // their settings_per_account.xml
};

// The last import that has not been undone yet (empty: none).
std::string lastUndoableImport();
// Undo it: settings back to their previous values, files restored from the backup,
// created files removed. Returns report lines.
std::vector<std::string> undoLastImport(bool& needs_restart);
}

#endif
