/**
 * @file slviewerimportcore.h
 * @brief Viewer importer (#131): discover another viewer's data directory and merge its
 *        chat transcripts into ours. Plain C++17 (no viewer dependencies) so the file
 *        logic can be tested on its own; the floater (slfloaterviewerimport) drives it.
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

#ifndef SL_VIEWERIMPORTCORE_H
#define SL_VIEWERIMPORTCORE_H

#include <filesystem>
#include <string>
#include <vector>

namespace slimport
{
namespace fs = std::filesystem;

// One viewer data directory found on this machine.
struct SourceViewer
{
    std::string id;             // "firestorm", "blackdragon", "alchemy", "secondlife" (viewer_import_map.xml)
    std::string label;          // "Firestorm", "Black Dragon", ...
    fs::path    dir;            // its per-user data directory
    bool        legacy_im_names = false;   // Firestorm: "John Smith.txt" IM logs by default
    bool        key_bindings_compatible = true;   // false: Black Dragon keeps its bindings in controls.xml (LLSD per line)
};

// One account inside a source viewer.
struct SourceAccount
{
    std::string name;           // per-account directory name, e.g. "john_smith"
    fs::path    settings_dir;   // <viewer dir>/<name> (settings_per_account.xml)
    fs::path    chat_dir;       // where its transcripts live (InstantMessageLogPath honoured)
    std::vector<fs::path>    chat_dirs;     // every folder to import from (account folder first, then chat_dir)
    std::vector<std::string> chat_search;   // where we looked and why (for the log; paths only)
    std::string chat_unreachable;           // InstantMessageLogPath folder we could not use (missing / denied)
    size_t      transcript_count = 0;
};

// base_dir: the OS user directory that holds viewer data dirs (~ on Linux,
// %APPDATA% on Windows, ~/Library/Application Support on macOS).
// exclude: our own data directory (never offered as a source).
std::vector<SourceViewer>  findSourceViewers(const fs::path& base_dir, const fs::path& exclude);

// A directory to look for viewer data dirs in. tag marks sources found there in their label
// ("Firestorm (Flatpak)"); undotted: also try "firestorm_x64" next to ".firestorm_x64".
struct SearchBase
{
    fs::path    dir;
    std::string tag;
    bool        undotted = false;
};
// Linux: other viewers installed as a Flatpak (~/.var/app/*/, data/, config/) or a Snap
// (~/snap/*/current/, common/) keep their data dir there instead of in home.
std::vector<SearchBase> sandboxedAppBases(const fs::path& home);
// Several base directories (Windows: %APPDATA% and %LOCALAPPDATA%); the same directory is
// listed once. search_log: where it looked and what it found (paths only).
std::vector<SourceViewer>  findSourceViewers(const std::vector<SearchBase>& bases, const fs::path& exclude,
                                             std::vector<std::string>* search_log = nullptr);

// Whether we run inside a Flatpak or Snap sandbox, which hides other viewers' folders.
struct Sandbox
{
    std::string kind;       // "", "Flatpak", "Snap"
    std::string app_id;     // FLATPAK_ID / SNAP_NAME
};
Sandbox detectSandbox();
// What to tell the user when `folder` cannot be read from inside the sandbox (empty: not sandboxed).
std::string sandboxHint(const Sandbox& sandbox, const std::string& folder);
// "permission denied", "not found", ... for a directory we could not use (empty: it is a readable directory).
std::string dirProblem(const fs::path& dir);

std::vector<SourceAccount> findAccounts(const SourceViewer& viewer);

// LL transcript file name for a (possibly Firestorm legacy-named) transcript file name.
// "John Smith.txt" -> "john_smith.txt", "John-2024-05.txt" -> "john-2024-05.txt",
// "john Resident.txt" -> "john.txt"; group ("... (group)[-YYYY-MM].txt"), nearby
// ("chat[-YYYY-MM-DD].txt") and conference logs are unchanged.
std::string mapTranscriptName(const std::string& filename, bool legacy_im_names);

// Rewrite a transcript line's timestamp to what our parser reads: 24-hour "H:MM", no
// seconds ("[2024/05/01 3:04:05 PM]  x" -> "[2024/05/01 15:04]  x"). Other lines unchanged.
std::string normalizeTimestamp(const std::string& line);

struct Stats
{
    int copied  = 0;    // transcripts that did not exist here yet
    int merged  = 0;    // existing transcripts that gained lines
    int same    = 0;    // nothing new in the source
    int skipped = 0;    // could not be merged safely (see notes)
    int failed  = 0;
    int separate = 0;       // could not be merged into ours (undated): kept as "<name> (imported).txt"
    int not_transcript = 0; // *.txt files that are not chat transcripts
    std::vector<std::string> notes;   // one per file not imported (or kept separately): "<file>: <reason>"
    std::vector<std::string> log;     // every file seen and what happened to it (file names only)
};

// Merge every transcript (*.txt) and conversation.log from src_dir into dst_dir.
// Never deletes anything; an existing file is backed up once to "<name>.pre-import"
// before it is first rewritten. dry_run: count only.
// Transcripts may be undated or time-only, start with a UTF-8 BOM and use CRLF. Date-split
// files ("Name-2026-10.txt", "chat-2026-10-04.txt") are merged into one transcript per
// conversation when their order is unambiguous, otherwise imported as they are. When ours
// or theirs is undated and neither contains the other, the import is kept next to ours
// as "<name> (imported).txt"; ours is left untouched.
Stats importChatLogs(const fs::path& src_dir, const fs::path& dst_dir, bool legacy_im_names, bool dry_run);
// Several source folders (earliest first), merged as one.
Stats importChatLogs(const std::vector<fs::path>& src_dirs, const fs::path& dst_dir, bool legacy_im_names, bool dry_run);

// Merge two transcripts' contents (exposed for tests). Returns false (and leaves out
// untouched) when either side has undated lines, which cannot be ordered.
bool mergeTranscripts(const std::string& ours, const std::string& theirs, std::string& out, bool& changed);

// Merge two conversation.log contents (exposed for tests): one entry per conversation
// (participant + session), newest wins; history file names mapped like transcripts.
std::string mergeConversationLogs(const std::string& ours, const std::string& theirs, bool legacy_im_names, bool& changed);

// Read the per-account InstantMessageLogPath out of a settings_per_account.xml (empty if unset).
std::string readInstantMessageLogPath(const fs::path& settings_per_account_xml);
// A string setting's value from a settings*.xml (empty if missing or unset).
std::string readSettingString(const fs::path& settings_xml, const std::string& key);
}

#endif
