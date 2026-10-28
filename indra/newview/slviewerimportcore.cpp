/**
 * @file slviewerimportcore.cpp
 * @brief Viewer importer (#131): chat transcript discovery and merge. See the header.
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

#include "slviewerimportcore.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_map>

namespace slimport
{
namespace
{
std::string utf8(const fs::path& p)
{
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

bool readFile(const fs::path& p, std::string& out)
{
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

// Why a file could not be read (the stream does not say; errno usually does).
std::string readProblem(const fs::path& p)
{
    const int err = errno;
    std::error_code ec;
    if (!fs::exists(p, ec)) return ec == std::errc::permission_denied ? "permission denied" : "not found";
    return err == EACCES || err == EPERM ? "permission denied" : "unreadable";
}

std::string stripBom(std::string s)
{
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) s.erase(0, 3);
    return s;
}

bool startsWith(const std::string& s, const std::string& prefix)
{
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

// Write through a temporary next to the target, then rename over it.
bool writeFile(const fs::path& p, const std::string& data)
{
    fs::path tmp = p;
    tmp += ".import-tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << data;
        if (!out.good()) return false;
    }
    std::error_code ec;
    fs::rename(tmp, p, ec);
    if (ec)
    {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

std::vector<std::string> splitLines(const std::string& s)
{
    std::vector<std::string> lines;
    std::string cur;
    for (char c : s)
    {
        if (c == '\n')
        {
            if (!cur.empty() && cur.back() == '\r') cur.pop_back();
            lines.push_back(cur);
            cur.clear();
        }
        else
        {
            cur += c;
        }
    }
    if (!cur.empty())
    {
        if (cur.back() == '\r') cur.pop_back();
        lines.push_back(cur);
    }
    return lines;
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

bool endsWith(const std::string& s, const std::string& suffix)
{
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// A transcript line's leading "[...]" timestamp, parsed by hand (std::regex per line
// took ~20 s for a 200 MB log folder, and the login-time merge runs on the main thread).
// Accepts "[YYYY/M/D H:MM]", "[H:MM]", optional ":SS", optional " AM"/"PM".
struct Stamp
{
    bool dated = false, secs = false, ampm = false, pm = false;
    int  y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
    size_t date_end = 0;   // index after the date's digits (before the separator)
    size_t time_begin = 0; // index of the hour
    size_t close = 0;      // index of ']'
};

bool readDigits(const std::string& s, size_t& i, int min_n, int max_n, int& out)
{
    int n = 0, v = 0;
    while (i < s.size() && n < max_n && s[i] >= '0' && s[i] <= '9') { v = v * 10 + (s[i] - '0'); ++i; ++n; }
    out = v;
    return n >= min_n;
}

bool parseStamp(const std::string& s, Stamp& st)
{
    if (s.size() < 6 || s[0] != '[') return false;
    size_t i = 1;
    {
        size_t j = 1;
        int y, mo, d;
        if (readDigits(s, j, 4, 4, y) && j < s.size() && s[j] == '/' && readDigits(s, ++j, 1, 2, mo)
            && j < s.size() && s[j] == '/' && readDigits(s, ++j, 1, 2, d)
            && j < s.size() && std::isspace((unsigned char)s[j]))
        {
            st.dated = true; st.y = y; st.mo = mo; st.d = d; st.date_end = j;
            while (j < s.size() && std::isspace((unsigned char)s[j])) ++j;
            i = j;
        }
    }
    st.time_begin = i;
    if (!readDigits(s, i, 1, 2, st.h) || i >= s.size() || s[i] != ':') return false;
    if (!readDigits(s, ++i, 2, 2, st.mi)) return false;
    if (i < s.size() && s[i] == ':')
    {
        if (!readDigits(s, ++i, 2, 2, st.sec)) return false;
        st.secs = true;
    }
    size_t k = i;
    while (k < s.size() && std::isspace((unsigned char)s[k])) ++k;
    if (k + 1 < s.size() && (s[k] == 'A' || s[k] == 'a' || s[k] == 'P' || s[k] == 'p') && (s[k + 1] == 'M' || s[k + 1] == 'm'))
    {
        st.ampm = true;
        st.pm = (s[k] == 'P' || s[k] == 'p');
        i = k + 2;
    }
    if (i >= s.size() || s[i] != ']') return false;
    st.close = i;
    return true;
}

// A transcript entry: its header line plus continuation lines.
struct Record
{
    long long   key = -1;       // yyyymmddHHMM, -1 = undated
    std::string text;
};

// Returns false if the transcript has entries that carry no date.
bool parseRecords(const std::string& content, std::vector<Record>& out)
{
    bool dated = true;
    for (const std::string& line : splitLines(content))
    {
        Stamp st;
        if (parseStamp(line, st))
        {
            Record r;
            if (st.dated)
                r.key = st.y * 100000000LL + st.mo * 1000000LL + st.d * 10000LL + st.h * 100LL + st.mi;
            else
                dated = false;
            r.text = line;
            out.push_back(std::move(r));
        }
        else if (!out.empty())
        {
            out.back().text += "\n";
            out.back().text += line;
        }
        else if (!line.empty())
        {
            dated = false;     // text before any timestamp: cannot be placed
            Record r;
            r.text = line;
            out.push_back(std::move(r));
        }
    }
    return dated;
}

std::string joinRecords(const std::vector<Record>& recs)
{
    std::string out;
    for (const Record& r : recs)
    {
        out += r.text;
        out += "\n";
    }
    return out;
}

// LLURI::escape-compatible for the characters that appear in names/file names.
std::string uriEscape(const std::string& s)
{
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s)
    {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
        {
            out += (char)c;
        }
        else
        {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string uriUnescape(const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit((unsigned char)s[i + 1]) && std::isxdigit((unsigned char)s[i + 2]))
        {
            out += (char)std::stoi(s.substr(i + 1, 2), nullptr, 16);
            i += 2;
        }
        else
        {
            out += s[i];
        }
    }
    return out;
}

// A "[...]" timestamp rewritten for our parser. keep_seconds: only 12-hour -> 24-hour
// (lossless; our parser reads seconds but not AM/PM); otherwise also drop the seconds,
// which is what dated transcripts are merged on.
std::string normalizeStamp(const std::string& line, bool keep_seconds)
{
    Stamp st;
    if (!parseStamp(line, st) || (!st.ampm && (keep_seconds || !st.secs))) return line;

    int hour = st.h;
    if (st.ampm)
    {
        if (hour == 12) hour = st.pm ? 12 : 0;
        else if (st.pm) hour += 12;
    }
    // Date as written, one space before the time (as LL writes it).
    std::string out = st.dated ? line.substr(0, st.date_end) + " " : std::string("[");
    char hm[16];
    if (keep_seconds && st.secs) std::snprintf(hm, sizeof(hm), "%02d:%02d:%02d", hour, st.mi, st.sec);
    else std::snprintf(hm, sizeof(hm), "%02d:%02d", hour, st.mi);
    out += hm;
    out += line.substr(st.close);
    return out;
}

// LF line ends, no BOM, every line normalized as above.
std::string normalizeContent(const std::string& content, bool keep_seconds)
{
    std::string out;
    out.reserve(content.size());
    for (const std::string& line : splitLines(stripBom(content)))
    {
        out += normalizeStamp(line, keep_seconds);
        out += "\n";
    }
    return out;
}

enum class Kind { NotTranscript, Empty, Dated, Undated };

// "Name: text" (no timestamp): a name of reasonable length before the first ": ".
bool looksLikeChatLine(const std::string& line)
{
    if (line.empty() || std::isspace((unsigned char)line[0])) return false;
    const char c0 = line[0];
    if (c0 == '<' || c0 == '{' || c0 == '[' || c0 == '#' || c0 == ';') return false;
    size_t colon = line.find(": ");
    if (colon == std::string::npos && line.back() == ':') colon = line.size() - 1;
    return colon != std::string::npos && colon > 0 && colon <= 100;
}

// Transcripts start with a "[...]" timestamp, or (logged without timestamps) read like
// "Name: text" lines. Dated: every entry carries a date, so it can be merged by time.
// Undated covers time-only ("show date in log" off), no timestamps and a mix.
Kind classifyTranscript(const std::string& content)
{
    if (content.find('\0') != std::string::npos) return Kind::NotTranscript;
    const std::vector<std::string> lines = splitLines(content);
    bool seen = false, dated = true, first_stamp = false;
    int sampled = 0, chatty = 0;
    for (const std::string& line : lines)
    {
        Stamp st;
        const bool stamp = parseStamp(line, st);
        if (!seen)
        {
            if (line.empty()) continue;
            seen = true;
            first_stamp = stamp;
            if (!stamp)
            {
                dated = false;
                if (line[0] == '<' || line[0] == '{') return Kind::NotTranscript;
            }
        }
        if (stamp && !st.dated) dated = false;
        if (sampled < 20 && !line.empty())
        {
            ++sampled;
            if (stamp || looksLikeChatLine(line)) ++chatty;
        }
        if (!dated && sampled >= 20) break;
    }
    if (!seen) return Kind::Empty;
    if (first_stamp) return dated ? Kind::Dated : Kind::Undated;
    return chatty * 2 >= sampled ? Kind::Undated : Kind::NotTranscript;
}

bool contentDated(const std::string& content)
{
    std::vector<Record> recs;
    return parseRecords(content, recs);
}

// Viewer data files that share the per-account folder but are not transcripts.
bool isViewerDataFile(const std::string& name)
{
    static const char* const names[] = { "teleport_history.txt", "typed_locations.txt", "render_mute_settings.txt", "search_history.txt" };
    for (const char* n : names) if (name == n) return true;
    return false;
}

size_t countTranscripts(const fs::path& dir)
{
    size_t n = 0;
    std::error_code ec;
    for (const fs::directory_entry& f : fs::directory_iterator(dir, ec))
    {
        std::error_code fec;
        if (f.is_regular_file(fec) && f.path().extension() == ".txt" && !isViewerDataFile(utf8(f.path().filename()))) ++n;
    }
    return n;
}

// Whether a folder holds transcripts itself (an InstantMessageLogPath that names the
// account's chat folder rather than its parent). Strict: random notes must not qualify.
bool looksLikeChatFolder(const fs::path& dir, std::string& why)
{
    std::error_code ec;
    if (fs::is_regular_file(dir / "conversation.log", ec))
    {
        why = "it has a conversation.log";
        return true;
    }
    int looked = 0;
    for (const fs::directory_entry& f : fs::directory_iterator(dir, ec))
    {
        std::error_code fec;
        if (!f.is_regular_file(fec) || f.path().extension() != ".txt") continue;
        if (++looked > 200) break;
        std::ifstream in(f.path(), std::ios::binary);
        std::string line;
        while (in && std::getline(in, line))
        {
            line = stripBom(line);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            Stamp st;
            if (parseStamp(line, st) && st.dated)
            {
                why = "it has dated transcripts";
                return true;
            }
            break;
        }
    }
    return false;
}

// "john_smith-2026-10" -> "john_smith" + "-2026-10"; nearby chat: "chat-2026-10-04".
// LogFileNamewithDate appends these; LL never date-stamps conference logs.
bool splitDateSuffix(const std::string& stem, std::string& base, std::string& suffix)
{
    auto digits = [&](size_t at, size_t n)
    {
        for (size_t i = at; i < at + n; ++i) if (stem[i] < '0' || stem[i] > '9') return false;
        return true;
    };
    auto month_ok = [&](size_t at) { const int m = (stem[at] - '0') * 10 + (stem[at + 1] - '0'); return m >= 1 && m <= 12; };
    const size_t n = stem.size();
    if (n > 11 && stem[n - 11] == '-' && digits(n - 10, 4) && stem[n - 6] == '-' && digits(n - 5, 2) && stem[n - 3] == '-'
        && digits(n - 2, 2) && month_ok(n - 5) && stem.compare(0, n - 11, "chat") == 0)
    {
        base = stem.substr(0, n - 11);
        suffix = stem.substr(n - 11);
        return true;
    }
    if (n > 8 && stem[n - 8] == '-' && digits(n - 7, 4) && stem[n - 3] == '-' && digits(n - 2, 2) && month_ok(n - 2))
    {
        base = stem.substr(0, n - 8);
        suffix = stem.substr(n - 8);
        return true;
    }
    base = stem;
    suffix.clear();
    return false;
}

bool backupOnce(const fs::path& p)
{
    fs::path bak = p;
    bak += ".pre-import";
    std::error_code ec;
    if (fs::exists(bak, ec)) return true;
    return fs::copy_file(p, bak, ec) && !ec;
}
}

std::string dirProblem(const fs::path& dir)
{
    std::error_code ec;
    const fs::file_status st = fs::status(dir, ec);
    if (ec == std::errc::permission_denied) return "permission denied";
    if (st.type() == fs::file_type::not_found || ec == std::errc::no_such_file_or_directory) return "not found";
    if (ec) return ec.message();
    if (!fs::is_directory(st)) return "not a folder";
    fs::directory_iterator it(dir, ec);
    if (ec == std::errc::permission_denied) return "permission denied";
    if (ec) return ec.message();
    return {};
}

namespace
{
struct Candidate { const char* dir; const char* id; const char* label; bool legacy_names; bool key_bindings; };
const Candidate CANDIDATES[] = {
#if defined(_WIN32) || defined(__APPLE__)
    { "Firestorm_x64", "firestorm", "Firestorm", true, true },
    { "Firestorm", "firestorm", "Firestorm", true, true },
    { "FirestormOS_x64", "firestorm", "Firestorm (OpenSim build)", true, true },   // <GMDX> SoapStorm c6d090301f; only its SL accounts are used
    { "BlackDragon", "blackdragon", "Black Dragon", false, false },
    { "AlchemyViewer", "alchemy", "Alchemy", false, true },
    { "AlchemyNext", "alchemy", "Alchemy (older)", false, true },
    { "SecondLife", "secondlife", "Second Life", false, true },
#else
    { ".firestorm_x64", "firestorm", "Firestorm", true, true },
    { ".firestorm", "firestorm", "Firestorm (32-bit)", true, true },
    { ".firestormos_x64", "firestorm", "Firestorm (OpenSim build)", true, true },  // <GMDX> SoapStorm c6d090301f; only its SL accounts are used
    { ".blackdragon", "blackdragon", "Black Dragon", false, false },
    { ".alchemyviewer", "alchemy", "Alchemy", false, true },
    { ".alchemynext", "alchemy", "Alchemy (older)", false, true },
    { ".secondlife", "secondlife", "Second Life", false, true },
#endif
};

void findInBase(const SearchBase& base, const fs::path& exclude, std::vector<SourceViewer>& found, std::vector<std::string>* log)
{
    for (const Candidate& c : CANDIDATES)
    {
        std::vector<std::string> names = { c.dir };
        if (base.undotted && c.dir[0] == '.') names.push_back(c.dir + 1);
        for (const std::string& name : names)
        {
            const fs::path dir = base.dir / name;
            std::error_code ec;
            if (!fs::is_directory(dir, ec))
            {
                const std::string why = dirProblem(dir);
                if (log && why != "not found") log->push_back(utf8(dir) + ": " + why);
                continue;
            }
            if (!exclude.empty() && fs::equivalent(dir, exclude, ec)) continue;
            // Case-insensitive filesystems (Windows, macOS) list "Firestorm" twice otherwise.
            bool dup = false;
            for (const SourceViewer& v : found) dup |= fs::equivalent(v.dir, dir, ec);
            if (dup) continue;
            SourceViewer v;
            v.id = c.id;
            v.label = base.tag.empty() ? std::string(c.label) : std::string(c.label) + " (" + base.tag + ")";
            v.dir = dir;
            v.legacy_im_names = c.legacy_names;
            v.key_bindings_compatible = c.key_bindings;
            const size_t accounts = findAccounts(v).size();
            if (log)
            {
                const std::string why = dirProblem(dir);
                log->push_back(utf8(dir) + ": " + (!why.empty() ? why : accounts ? std::to_string(accounts) + " account(s)"
                                                                                  : std::string("no Second Life accounts")));
            }
            if (accounts) found.push_back(std::move(v));
        }
    }
}
}

std::vector<SourceViewer> findSourceViewers(const fs::path& base_dir, const fs::path& exclude)
{
    return findSourceViewers(std::vector<SearchBase>{ { base_dir, std::string(), false } }, exclude);
}

std::vector<SourceViewer> findSourceViewers(const std::vector<SearchBase>& bases, const fs::path& exclude, std::vector<std::string>* search_log)
{
    std::vector<SourceViewer> found;
    for (const SearchBase& base : bases)
    {
        const std::string why = dirProblem(base.dir);
        if (search_log && why != "not found")
        {
            search_log->push_back("Looking in " + utf8(base.dir) + (base.tag.empty() ? "" : " (" + base.tag + ")")
                                  + (why.empty() ? "" : ": " + why));
        }
        if (why.empty()) findInBase(base, exclude, found, search_log);
    }
    return found;
}

std::vector<SearchBase> sandboxedAppBases(const fs::path& home)
{
    std::vector<SearchBase> bases;
    std::error_code ec;
    for (const fs::directory_entry& app : fs::directory_iterator(home / ".var" / "app", ec))
    {
        std::error_code aec;
        if (!app.is_directory(aec)) continue;
        bases.push_back({ app.path(), "Flatpak", false });
        bases.push_back({ app.path() / "data", "Flatpak", true });
        bases.push_back({ app.path() / "config", "Flatpak", true });
    }
    for (const fs::directory_entry& snap : fs::directory_iterator(home / "snap", ec))
    {
        std::error_code sec;
        if (!snap.is_directory(sec)) continue;
        bases.push_back({ snap.path() / "current", "Snap", false });
        bases.push_back({ snap.path() / "common", "Snap", false });
    }
    return bases;
}

Sandbox detectSandbox()
{
    Sandbox sb;
#ifndef _WIN32
    if (const char* id = std::getenv("FLATPAK_ID"))
    {
        sb.kind = "Flatpak";
        sb.app_id = id;
    }
    std::error_code ec;
    if (sb.kind.empty() && fs::exists("/.flatpak-info", ec))
    {
        sb.kind = "Flatpak";
        std::string info;
        if (readFile("/.flatpak-info", info))
        {
            bool app = false;
            for (const std::string& line : splitLines(info))
            {
                if (!line.empty() && line[0] == '[') app = line == "[Application]";
                else if (app && startsWith(line, "name=")) sb.app_id = line.substr(5);
            }
        }
    }
    if (sb.kind.empty() && std::getenv("SNAP"))
    {
        sb.kind = "Snap";
        if (const char* name = std::getenv("SNAP_NAME")) sb.app_id = name;
    }
#endif
    return sb;
}

std::string sandboxHint(const Sandbox& sandbox, const std::string& folder)
{
    std::string shown = folder;
#ifndef _WIN32
    if (const char* home = std::getenv("HOME"))
    {
        const std::string h = home;
        if (!h.empty() && startsWith(shown, h + "/")) shown = "~" + shown.substr(h.size());
    }
#endif
    if (sandbox.kind == "Flatpak")
    {
        const std::string id = sandbox.app_id.empty() ? std::string("<app id>") : sandbox.app_id;
        const std::string arg = shown.find(' ') != std::string::npos ? "\"" + shown + ":ro\"" : shown + ":ro";
        return "Slipstream is running in a Flatpak sandbox and can't read " + shown + ". Grant access with: flatpak override --user --filesystem="
               + arg + " " + id + " (or --filesystem=home:ro for every viewer), then restart Slipstream.";
    }
    if (sandbox.kind == "Snap")
    {
        return "Slipstream is running as a Snap, which can't read hidden folders in your home such as " + shown
               + ". Import with a non-Snap build of Slipstream.";
    }
    return {};
}

std::vector<SourceAccount> findAccounts(const SourceViewer& viewer)
{
    std::vector<SourceAccount> accounts;
    std::error_code ec;
    for (const fs::directory_entry& e : fs::directory_iterator(viewer.dir, ec))
    {
        std::error_code eec;
        if (!e.is_directory(eec)) continue;
        const std::string name = utf8(e.path().filename());
        // Other grids get "<name>.<grid>" directories (Firestorm); only Second Life accounts.
        if (name.find('.') != std::string::npos) continue;
        const fs::path settings = e.path() / "settings_per_account.xml";
        if (!fs::exists(settings, eec)) continue;

        SourceAccount a;
        a.name = name;
        a.settings_dir = e.path();
        a.chat_dir = e.path();
        const size_t own = countTranscripts(e.path());
        a.chat_search.push_back("Account folder " + utf8(e.path()) + ": " + std::to_string(own) + " transcript file(s)");

        // LL keeps InstantMessageLogPath per account; Firestorm may keep it in the global settings.
        static const char* const sources[] = { "", "settings_firestorm.xml", "settings.xml" };
        std::string log_path, from;
        for (const char* src : sources)
        {
            const fs::path file = *src ? viewer.dir / "user_settings" / src : settings;
            log_path = readSettingString(file, "InstantMessageLogPath");
            if (!log_path.empty())
            {
                from = *src ? std::string("user_settings/") + src : std::string("settings_per_account.xml");
                break;
            }
        }
        if (log_path.empty())
        {
            a.chat_search.push_back("InstantMessageLogPath not set (settings_per_account.xml, user_settings/settings_firestorm.xml, "
                                    "user_settings/settings.xml): using the account folder");
        }
        else
        {
            fs::path base = fs::u8path(log_path).lexically_normal();
            if (base.filename().empty()) base = base.parent_path();   // trailing separator
            const fs::path sub = base / name;
            const std::string prefix = "InstantMessageLogPath " + log_path + " (" + from + "): ";
            std::string why;
            fs::path relocated;
            if (fs::is_directory(sub, eec))
            {
                relocated = sub;
                a.chat_search.push_back(prefix + "using " + utf8(sub));
            }
            else if (dirProblem(base).empty() && looksLikeChatFolder(base, why))
            {
                relocated = base;
                a.chat_search.push_back(prefix + "no " + name + " subfolder; using the folder itself (" + why + ")");
            }
            else
            {
                const std::string sub_why = dirProblem(sub), base_why = dirProblem(base);
                if (!base_why.empty())
                {
                    a.chat_unreachable = utf8(base);
                    a.chat_search.push_back(prefix + "folder " + base_why + "; using the account folder");
                }
                else
                {
                    if (sub_why == "permission denied") a.chat_unreachable = utf8(sub);
                    a.chat_search.push_back(prefix + utf8(sub) + " " + sub_why + " and the folder itself has no conversation.log "
                                            "or dated transcripts; using the account folder");
                }
            }
            if (!relocated.empty() && !fs::equivalent(relocated, e.path(), eec))
            {
                a.chat_dir = relocated;
                if (own) a.chat_dirs.push_back(e.path());   // older transcripts left behind before the move
            }
            else if (!relocated.empty())
            {
                a.chat_search.push_back("  (that is the account folder)");
            }
        }
        a.chat_dirs.push_back(a.chat_dir);
        for (const fs::path& dir : a.chat_dirs)
        {
            a.transcript_count += dir == e.path() ? own : countTranscripts(dir);
        }
        if (a.chat_dirs.size() > 1) a.chat_search.push_back("Importing from both the account folder and " + utf8(a.chat_dir));
        accounts.push_back(std::move(a));
    }
    std::sort(accounts.begin(), accounts.end(), [](const SourceAccount& a, const SourceAccount& b) { return a.name < b.name; });
    return accounts;
}

std::string mapTranscriptName(const std::string& filename, bool legacy_im_names)
{
    if (!legacy_im_names || !endsWith(filename, ".txt")) return filename;
    // Keep a date suffix ("-2024-05") as is.
    std::string stem, suffix;
    splitDateSuffix(filename.substr(0, filename.size() - 4), stem, suffix);
    if (endsWith(stem, " (group)")) return filename;
    if (stem == "chat") return filename;
    if (stem.find(" Conference ") != std::string::npos || stem.rfind("Ad-hoc Conference", 0) == 0) return filename;

    // Legacy "first last"; LL drops the "Resident" last name.
    if (endsWith(stem, " Resident") && stem.size() > 9) stem.resize(stem.size() - 9);
    std::string mapped = lower(stem);
    std::replace(mapped.begin(), mapped.end(), ' ', '_');
    return mapped + suffix + ".txt";
}

std::string normalizeTimestamp(const std::string& line)
{
    return normalizeStamp(line, false);
}

bool mergeTranscripts(const std::string& ours, const std::string& theirs, std::string& out, bool& changed)
{
    changed = false;
    std::vector<Record> a, b;
    const bool a_dated = parseRecords(ours, a);
    const bool b_dated = parseRecords(theirs, b);
    if (b.empty())
    {
        out = ours;
        return true;
    }
    if (!a_dated || !b_dated) return false;

    // Drop what we already have (as a multiset: a repeated "lol" in one minute is kept
    // as many times as the source has it beyond ours). Compared as normalized, so our
    // "[.. 15:04:05]" matches their "[.. 3:04 PM]".
    std::unordered_map<std::string, int> have;
    for (const Record& r : a) ++have[normalizeStamp(r.text, false)];
    std::vector<Record> fresh;
    for (Record& r : b)
    {
        auto it = have.find(normalizeStamp(r.text, false));
        if (it != have.end() && it->second > 0) { --it->second; continue; }
        fresh.push_back(std::move(r));
    }
    if (fresh.empty())
    {
        out = ours;
        return true;
    }
    std::stable_sort(fresh.begin(), fresh.end(), [](const Record& x, const Record& y) { return x.key < y.key; });

    // Merge without reordering our own entries.
    std::vector<Record> merged;
    merged.reserve(a.size() + fresh.size());
    size_t i = 0, j = 0;
    while (i < a.size() || j < fresh.size())
    {
        if (j < fresh.size() && (i >= a.size() || fresh[j].key < a[i].key)) merged.push_back(fresh[j++]);
        else merged.push_back(a[i++]);
    }
    out = joinRecords(merged);
    changed = true;
    return true;
}

std::string mergeConversationLogs(const std::string& ours, const std::string& theirs, bool legacy_im_names, bool& changed)
{
    changed = false;
    static const std::regex entry("^\\[(\\d+)\\] (\\d+) (\\d+) (\\d+) ([^|]*)\\| (\\S+) (\\S+) ([^|]*)\\|$");
    struct Entry { long long time; std::string participant; std::string line; };

    std::vector<Entry> entries;
    std::map<std::string, size_t> by_participant;
    std::smatch m;
    for (const std::string& line : splitLines(ours))
    {
        if (line.empty()) continue;
        if (!std::regex_match(line, m, entry)) return ours;   // unknown format: leave ours alone
        by_participant[m[6]] = entries.size();
        entries.push_back({ std::stoll(m[1]), m[6], line });
    }
    for (const std::string& line : splitLines(theirs))
    {
        if (line.empty() || !std::regex_match(line, m, entry)) continue;
        const long long t = std::stoll(m[1]);
        std::string history = uriUnescape(m[8]);
        history = mapTranscriptName(history + ".txt", legacy_im_names);
        history.resize(history.size() - 4);
        const std::string rewritten = "[" + m[1].str() + "] " + m[2].str() + " " + m[3].str() + " " + m[4].str() + " "
                                    + m[5].str() + "| " + m[6].str() + " " + m[7].str() + " " + uriEscape(history) + "|";
        auto it = by_participant.find(m[6]);
        if (it == by_participant.end())
        {
            by_participant[m[6]] = entries.size();
            entries.push_back({ t, m[6], rewritten });
            changed = true;
        }
        else if (t > entries[it->second].time)
        {
            entries[it->second] = { t, m[6], rewritten };
            changed = true;
        }
    }
    if (!changed) return ours;
    std::stable_sort(entries.begin(), entries.end(), [](const Entry& x, const Entry& y) { return x.time > y.time; });
    std::string out;
    for (const Entry& e : entries)
    {
        out += e.line;
        out += "\n";
    }
    return out;
}

std::string readInstantMessageLogPath(const fs::path& settings_per_account_xml)
{
    return readSettingString(settings_per_account_xml, "InstantMessageLogPath");
}

std::string readSettingString(const fs::path& settings_xml, const std::string& key)
{
    std::string xml;
    if (!readFile(settings_xml, xml)) return {};
    const size_t k = xml.find("<key>" + key + "</key>");
    if (k == std::string::npos) return {};
    const size_t end_map = xml.find("</map>", k);
    const size_t v = xml.find("<key>Value</key>", k);
    if (v == std::string::npos || (end_map != std::string::npos && v > end_map)) return {};
    // The value right after it ("<string />" is empty; don't run into the next setting).
    size_t s = v + 16;
    while (s < xml.size() && std::isspace((unsigned char)xml[s])) ++s;
    if (xml.compare(s, 8, "<string>") != 0) return {};
    const size_t e = xml.find("</string>", s);
    if (e == std::string::npos) return {};
    std::string val = xml.substr(s + 8, e - s - 8);
    static const std::pair<const char*, const char*> ents[] = {
        { "&lt;", "<" }, { "&gt;", ">" }, { "&quot;", "\"" }, { "&apos;", "'" }, { "&amp;", "&" } };
    for (const auto& ent : ents)
    {
        size_t pos = 0;
        const std::string from = ent.first;
        while ((pos = val.find(from, pos)) != std::string::npos)
        {
            val.replace(pos, from.size(), ent.second);
            pos += 1;
        }
    }
    return val;
}

namespace
{
// One source transcript file.
struct Part
{
    fs::path    path;
    std::string file;       // file name
    std::string shown;      // file name, with its folder when importing from several
    std::string suffix;     // "-2026-10" (LogFileNamewithDate), or empty
    bool        dated = false;
    size_t      dir = 0;    // source folder index
};

// Writes transcripts into dst. A dry run remembers what it would write where a later file
// may aim at the same transcript (remember), so it counts what the real run will do.
class TranscriptWriter
{
public:
    TranscriptWriter(const fs::path& dst, bool dry_run, Stats& st) : mDst(dst), mDryRun(dry_run), mStats(st) {}

    void import(const std::string& name, const std::string& content, bool dated, const std::string& shown, bool remember = false);

private:
    bool read(const fs::path& p, std::string& out, bool& exists, std::string& why);
    bool put(const fs::path& p, const std::string& data, bool existed, bool remember);
    void note(const std::string& line) { mStats.notes.push_back(line); mStats.log.push_back(line); }
    void fail(const std::string& line) { ++mStats.failed; note(line); }

    fs::path    mDst;
    bool        mDryRun;
    Stats&      mStats;
    std::map<fs::path, std::string> mWritten;
    std::set<fs::path> mCreated;
};

bool TranscriptWriter::read(const fs::path& p, std::string& out, bool& exists, std::string& why)
{
    const auto it = mWritten.find(p);
    if (it != mWritten.end())
    {
        exists = true;
        out = it->second;
        return true;
    }
    std::error_code ec;
    exists = fs::exists(p, ec);
    if (!exists) return true;
    if (!readFile(p, out))
    {
        why = readProblem(p);
        return false;
    }
    out = stripBom(out);
    return true;
}

bool TranscriptWriter::put(const fs::path& p, const std::string& data, bool existed, bool remember)
{
    if (mDryRun)
    {
        if (remember) mWritten[p] = data;
        return true;
    }
    // No backup of a file this run created.
    if (existed && !mCreated.count(p) && !backupOnce(p)) return false;
    if (!writeFile(p, data)) return false;
    if (!existed) mCreated.insert(p);
    return true;
}

void TranscriptWriter::import(const std::string& name, const std::string& content, bool dated, const std::string& shown, bool remember)
{
    const fs::path target = mDst / fs::u8path(name);
    std::string ours, why;
    bool have = false;
    if (!read(target, ours, have, why)) return fail(shown + ": could not read our " + name + " (" + why + ")");
    if (!have)
    {
        if (!put(target, content, false, remember)) return fail(shown + ": could not write " + name);
        ++mStats.copied;
        mStats.log.push_back(shown + ": new -> " + name + (dated ? "" : " (undated, copied as is)"));
        return;
    }
    if (ours == content)
    {
        ++mStats.same;
        mStats.log.push_back(shown + ": already here (" + name + ")");
        return;
    }
    if (dated && contentDated(ours))
    {
        std::string out;
        bool changed = false;
        mergeTranscripts(ours, content, out, changed);
        if (!changed)
        {
            ++mStats.same;
            mStats.log.push_back(shown + ": nothing new for " + name);
            return;
        }
        if (!put(target, out, true, remember)) return fail(shown + ": could not update " + name);
        ++mStats.merged;
        mStats.log.push_back(shown + ": merged into " + name);
        return;
    }

    // Undated on either side: entries can't be placed by time. Take it only when one
    // contains the other; otherwise keep the import next to ours, untouched.
    const std::string o = normalizeContent(ours, true);
    if (startsWith(o, content))
    {
        ++mStats.same;
        mStats.log.push_back(shown + ": already contained in " + name + " (undated)");
        return;
    }
    if (startsWith(content, o))
    {
        if (!put(target, content, true, remember)) return fail(shown + ": could not update " + name);
        ++mStats.merged;
        mStats.log.push_back(shown + ": continues " + name + " (undated); added the rest");
        return;
    }
    const std::string sep_name = name.substr(0, name.size() - 4) + " (imported).txt";
    const fs::path sep = mDst / fs::u8path(sep_name);
    std::string prev;
    bool have_sep = false;
    if (!read(sep, prev, have_sep, why)) return fail(shown + ": could not read our " + sep_name + " (" + why + ")");
    if (have_sep)
    {
        prev = normalizeContent(prev, true);
        if (startsWith(prev, content))
        {
            ++mStats.same;
            mStats.log.push_back(shown + ": already kept as " + sep_name);
            return;
        }
        if (!startsWith(content, prev))
        {
            ++mStats.skipped;
            return note(shown + ": undated entries, differs from both " + name + " and " + sep_name + "; left as is");
        }
    }
    if (!put(sep, content, have_sep, true)) return fail(shown + ": could not write " + sep_name);
    ++mStats.separate;
    note(shown + ": undated entries (here or there) can't be merged into " + name + "; kept as " + sep_name);
}
}

Stats importChatLogs(const fs::path& src_dir, const fs::path& dst_dir, bool legacy_im_names, bool dry_run)
{
    return importChatLogs(std::vector<fs::path>{ src_dir }, dst_dir, legacy_im_names, dry_run);
}

Stats importChatLogs(const std::vector<fs::path>& src_dirs, const fs::path& dst_dir, bool legacy_im_names, bool dry_run)
{
    Stats st;
    std::error_code ec;
    if (!dry_run) fs::create_directories(dst_dir, ec);

    // Our transcript name (date suffix dropped) -> its source files.
    std::map<std::string, std::vector<Part>> groups;
    for (size_t d = 0; d < src_dirs.size(); ++d)
    {
        const fs::path& src = src_dirs[d];
        const std::string problem = dirProblem(src);
        if (!problem.empty())
        {
            st.notes.push_back("Source folder " + problem + ": " + utf8(src));
            st.log.push_back(st.notes.back());
            continue;
        }
        st.log.push_back("Reading " + utf8(src));
        std::vector<fs::path> files;
        for (const fs::directory_entry& e : fs::directory_iterator(src, ec))
        {
            std::error_code eec;
            if (e.is_regular_file(eec) && endsWith(utf8(e.path().filename()), ".txt")) files.push_back(e.path());
        }
        std::sort(files.begin(), files.end());
        for (const fs::path& path : files)
        {
            Part part;
            part.path = path;
            part.file = utf8(path.filename());
            part.shown = src_dirs.size() > 1 ? utf8(src.filename()) + "/" + part.file : part.file;
            part.dir = d;
            if (isViewerDataFile(part.file))
            {
                st.log.push_back(part.shown + ": viewer data file, not a transcript");
                continue;
            }
            std::string content;
            if (!readFile(path, content))
            {
                ++st.failed;
                st.notes.push_back(part.shown + ": " + readProblem(path));
                st.log.push_back(st.notes.back());
                continue;
            }
            // Read again when merged: keeps one conversation in memory, not the whole folder.
            const Kind kind = classifyTranscript(stripBom(content));
            if (kind == Kind::NotTranscript)
            {
                ++st.not_transcript;
                st.notes.push_back(part.shown + ": not a chat transcript");
                st.log.push_back(st.notes.back());
                continue;
            }
            if (kind == Kind::Empty)
            {
                st.log.push_back(part.shown + ": empty, nothing to import");
                continue;
            }
            part.dated = kind == Kind::Dated;
            std::string base;
            splitDateSuffix(part.file.substr(0, part.file.size() - 4), base, part.suffix);
            groups[mapTranscriptName(base + ".txt", legacy_im_names)].push_back(std::move(part));
        }
    }

    TranscriptWriter writer(dst_dir, dry_run, st);
    auto load = [&](const Part& p, bool keep_seconds, std::string& out)
    {
        if (readFile(p.path, out))
        {
            out = normalizeContent(out, keep_seconds);
            return true;
        }
        ++st.failed;
        st.notes.push_back(p.shown + ": " + readProblem(p.path));
        st.log.push_back(st.notes.back());
        return false;
    };
    for (auto& group : groups)
    {
        const std::string& name = group.first;
        std::vector<Part>& parts = group.second;
        if (parts.size() == 1)
        {
            const Part& p = parts[0];
            std::string content;
            if (load(p, !p.dated, content)) writer.import(name, content, p.dated, p.shown);
            continue;
        }

        // Several files for one conversation: date-split parts, and/or one per source folder.
        std::stable_sort(parts.begin(), parts.end(), [](const Part& a, const Part& b)
        {
            if (a.suffix.empty() != b.suffix.empty()) return a.suffix.empty();
            return a.suffix < b.suffix;
        });
        bool all_dated = true, any_plain = false, unique = true;
        for (size_t i = 0; i < parts.size(); ++i)
        {
            all_dated &= parts[i].dated;
            any_plain |= parts[i].suffix.empty();
            if (i && parts[i].suffix == parts[i - 1].suffix) unique = false;
        }
        std::string shown = parts.front().shown;
        for (size_t i = 1; i < parts.size(); ++i) shown += ", " + parts[i].shown;
        if (shown.size() > 160) shown = parts.front().shown + " .. " + parts.back().shown + " (" + std::to_string(parts.size()) + " files)";

        if (all_dated)
        {
            // Per folder, the date parts in order; plain files merged in by time.
            std::string content, next;
            bool ok = true;
            auto fold = [&](const std::string& more)
            {
                std::string out;
                bool changed = false;
                if (content.empty()) content = more;
                else if (mergeTranscripts(content, more, out, changed) && changed) content = out;
            };
            for (size_t d = 0; d < src_dirs.size() && ok; ++d)
            {
                std::string dated_parts;
                for (const Part& p : parts)
                {
                    if (p.dir != d) continue;
                    if (!(ok = load(p, false, next))) break;
                    if (p.suffix.empty()) fold(next);
                    else dated_parts += next;
                }
                if (ok && !dated_parts.empty()) fold(dated_parts);
            }
            if (ok) writer.import(name, content, true, shown);
        }
        else if (!any_plain && unique)
        {
            // Undated date-split parts: their names give the order.
            std::string content, next;
            bool ok = true;
            for (const Part& p : parts)
            {
                if (!(ok = load(p, true, next))) break;
                content += next;
            }
            if (ok) writer.import(name, content, false, shown);
        }
        else
        {
            st.log.push_back(shown + ": several files for " + name + " without full dates, order unknown; importing each as it is");
            for (const Part& p : parts)
            {
                std::string content;
                if (load(p, !p.dated, content)) writer.import(mapTranscriptName(p.file, legacy_im_names), content, p.dated, p.shown, true);
            }
        }
    }

    for (const fs::path& src_dir : src_dirs)
    {
        const fs::path src_log = src_dir / "conversation.log";
        if (!fs::exists(src_log, ec)) continue;
        std::string theirs, ours;
        const fs::path dst_log = dst_dir / "conversation.log";
        if (!readFile(src_log, theirs))
        {
            st.log.push_back("conversation.log in " + utf8(src_dir) + ": " + readProblem(src_log));
            continue;
        }
        const bool had = fs::exists(dst_log, ec) && readFile(dst_log, ours);
        bool changed = false;
        const std::string out = mergeConversationLogs(ours, theirs, legacy_im_names, changed);
        if (changed && !dry_run && ((had && !backupOnce(dst_log)) || !writeFile(dst_log, out)))
        {
            ++st.failed;
            st.notes.push_back("Could not update conversation.log");
            st.log.push_back(st.notes.back());
            continue;
        }
        st.log.push_back("conversation.log in " + utf8(src_dir) + (changed ? ": merged" : ": nothing new"));
    }
    return st;
}
}
