/**
 * @file slviewerimportplan.cpp
 * @brief Viewer importer (#131, #231, #232): build / apply / undo. See the header.
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

#include "slviewerimportplan.h"

#include "lggcontactsets.h"
#include "llautoreplace.h"
#include "llcommandmanager.h"
#include "llconversationlog.h"
#include "llfloaterreg.h"
#include "llkeyboard.h"
#include "llpresetsmanager.h"
#include "llsdserialize.h"
#include "llsdutil.h"
#include "llspellcheck.h"
#include "llteleporthistorystorage.h"
#include "lltoolbarview.h"
#include "lluicolortable.h"
#include "lluri.h"
#include "llviewercontrol.h"
#include "llviewerinput.h"
#include "llxmlnode.h"
#include "llxuiparser.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>

namespace slimport
{
namespace
{
namespace fs = std::filesystem;

const char* const CATEGORY_KEYS[CAT_COUNT] = {
    "settings", "account", "notifications", "colors", "layout", "toolbars", "keys", "contact_sets",
    "teleport_history", "graphics_presets", "camera_presets", "poses", "fonts", "windlight",
    "autoreplace", "spelling", "chat"
};
// Colours and window positions are skin / screen specific: offered, but off unless ticked.
const bool CATEGORY_DEFAULT_ON[CAT_COUNT] = {
    true, true, true, false, false, true, true, true,
    true, true, true, true, true, true,
    true, true, true
};

const char* const MODE_TAGS[MODE_COUNT]   = { "first_person", "third_person", "edit_avatar", "sitting" };
const char* const MODE_LABELS[MODE_COUNT] = { "First person", "Third person", "Edit avatar", "Sitting" };

const char* const BACKUP_DIR   = "import_backups";
// autoreplace.xml list keys (LLAutoReplaceSettings keeps its constants private).
const std::string AR_LIST_NAME = "name";
const std::string AR_LIST_REPLACEMENTS = "replacements";
const char* const JOURNAL_FILE = "undo.xml";
const char* const UNDONE_FILE  = "undone";

// ---------------------------------------------------------------------------
// Files

std::string utf8(const fs::path& p)
{
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

fs::path u8(const std::string& s)
{
    return fs::u8path(s);
}

bool readText(const fs::path& p, std::string& out)
{
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

// Write through a temporary next to the target, then rename over it.
bool writeText(const fs::path& p, const std::string& data)
{
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    fs::path tmp = p;
    tmp += ".import-tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << data;
        if (!out.good()) return false;
    }
    fs::rename(tmp, p, ec);
    if (ec)
    {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

LLSD readLLSDFile(const fs::path& p)
{
    LLSD sd;
    std::ifstream in(p, std::ios::binary);
    if (in)
    {
        LLSDSerialize::fromXMLDocument(sd, in, false);
    }
    return sd;
}

bool writeLLSDFile(const fs::path& p, const LLSD& sd)
{
    std::ostringstream out;
    LLSDSerialize::toPrettyXML(sd, out);
    return writeText(p, out.str());
}

bool sameFileContents(const fs::path& a, const fs::path& b)
{
    std::string x, y;
    return readText(a, x) && readText(b, y) && x == y;
}

fs::path userSettingsDir()
{
    return u8(gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, ""));
}

fs::path accountDir()
{
    const std::string dir = gDirUtilp->getLindenUserDir();
    return dir.empty() ? fs::path() : u8(dir);
}

std::string lower(std::string s)
{
    LLStringUtil::toLower(s);
    return s;
}

bool startsWith(const std::string& s, const std::string& prefix)
{
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool endsWith(const std::string& s, const std::string& suffix)
{
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string xmlEscape(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s)
    {
        switch (c)
        {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c; break;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Values

std::string displayValue(const LLSD& v)
{
    if (v.isArray())
    {
        std::string out;
        for (const LLSD& e : llsd::inArray(v))
        {
            if (!out.empty()) out += ", ";
            out += displayValue(e);
        }
        return out;
    }
    if (v.isMap()) return llformat("(%d entries)", (S32)v.size());
    if (v.isReal()) return llformat("%.4g", v.asReal());
    if (v.isBoolean()) return v.asBoolean() ? "on" : "off";
    std::string s = v.asString();
    if (s.size() > 60) s = s.substr(0, 57) + "...";
    return s;
}

bool valuesEqual(const LLSD& a, const LLSD& b)
{
    if (a.isArray() && b.isArray())
    {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
        {
            if (!valuesEqual(a[i], b[i])) return false;
        }
        return true;
    }
    if ((a.isReal() || b.isReal()) && (a.isReal() || a.isInteger()) && (b.isReal() || b.isInteger()))
    {
        const F64 x = a.asReal(), y = b.asReal();
        return std::fabs(x - y) <= 1e-5 * llmax(1.0, std::fabs(x));
    }
    if (a.isBoolean() || b.isBoolean())
    {
        return a.asBoolean() == b.asBoolean();
    }
    return llsd_equals(a, b);
}

const char* typeLabel(eControlType t)
{
    static std::string names[TYPE_COUNT + 1];
    const S32 i = (t >= 0 && t < TYPE_COUNT) ? (S32)t : (S32)TYPE_COUNT;
    if (names[i].empty()) names[i] = (i < TYPE_COUNT) ? LLControlGroup::typeEnumToString((eControlType)i) : std::string("unknown");
    return names[i].c_str();
}

// Same-named settings must have the same type (S32 / U32 interchange when the value
// fits); anything else needs an explicit conversion in the map.
bool typesCompatible(eControlType from, eControlType to)
{
    if (from == to) return true;
    return (from == TYPE_S32 || from == TYPE_U32) && (to == TYPE_S32 || to == TYPE_U32);
}

bool numberArray(const LLSD& v, size_t n, bool integer, LLSD& out)
{
    if (!v.isArray() || v.size() != n) return false;
    out = LLSD::emptyArray();
    for (size_t i = 0; i < n; ++i)
    {
        const LLSD& e = v[i];
        if (!(e.isReal() || e.isInteger())) return false;
        if (integer)
        {
            out.append(e.asInteger());
        }
        else
        {
            const F64 d = e.asReal();
            if (!std::isfinite(d)) return false;
            out.append(d);
        }
    }
    return true;
}

// The value in the shape our control expects. False: it cannot be represented.
bool coerce(const LLSD& v, eControlType to, LLSD& out)
{
    switch (to)
    {
        case TYPE_BOOLEAN:
            if (v.isBoolean() || v.isInteger() || v.isReal()) { out = LLSD(v.asBoolean()); return true; }
            if (v.isString()) { const std::string s = lower(v.asString()); if (s == "true" || s == "1") { out = LLSD(true); return true; }
                                if (s == "false" || s == "0") { out = LLSD(false); return true; } }
            return false;
        case TYPE_S32:
            if (v.isInteger() || v.isBoolean()) { out = LLSD(v.asInteger()); return true; }
            if (v.isReal() && std::isfinite(v.asReal()) && std::fabs(v.asReal()) < 2147483647.0) { out = LLSD((S32)ll_round(v.asReal())); return true; }
            return false;
        case TYPE_U32:
            if ((v.isInteger() || v.isBoolean()) && v.asInteger() >= 0) { out = LLSD(v.asInteger()); return true; }
            if (v.isReal() && std::isfinite(v.asReal()) && v.asReal() >= 0.0 && v.asReal() < 2147483647.0) { out = LLSD((S32)ll_round(v.asReal())); return true; }
            return false;
        case TYPE_F32:
            if ((v.isReal() || v.isInteger()) && std::isfinite(v.asReal())) { out = LLSD(v.asReal()); return true; }
            return false;
        case TYPE_STRING:
            if (v.isString() || v.isUUID() || v.isUndefined()) { out = LLSD(v.asString()); return out.asString().size() < 65536; }
            return false;
        case TYPE_UUID:
        {
            if (!v.isString() && !v.isUUID()) return false;
            if (!v.asString().empty() && !LLUUID::validate(v.asString())) return false;
            out = LLSD(v.asString());
            return true;
        }
        case TYPE_VEC3:  return numberArray(v, 3, false, out);
        case TYPE_VEC3D: return numberArray(v, 3, false, out);
        case TYPE_VEC4:  return numberArray(v, 4, false, out);
        case TYPE_QUAT:  return numberArray(v, 4, false, out);
        case TYPE_COL4:  return numberArray(v, 4, false, out);
        case TYPE_COL3:  return numberArray(v, 3, false, out);
        case TYPE_RECT:  return numberArray(v, 4, true, out);
        case TYPE_LLSD:  out = v; return true;
        default: return false;
    }
}

std::string enumKey(const LLSD& v)
{
    if (v.isBoolean()) return v.asBoolean() ? "1" : "0";
    if (v.isInteger()) return std::to_string(v.asInteger());
    if (v.isReal()) return llformat("%g", v.asReal());
    return v.asString();
}

// ---------------------------------------------------------------------------
// The map (app_settings/viewer_import_map.xml)

struct ImportMap
{
    LLSD rename, ranges, enums, validate, unmappable;
    LLSD toolbarCommands, toolbarNotes, keyCommands, bdModes;
    std::set<std::string> renderAllow, denyExact, core, restart;
    std::vector<std::regex> denyRegex;
    std::vector<std::string> denyContains;
    S32 maxPerCommand = 3;

    static const ImportMap& instance()
    {
        static const ImportMap m = load();
        return m;
    }

    static ImportMap load()
    {
        ImportMap m;
        const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "viewer_import_map.xml");
        const LLSD sd = readLLSDFile(u8(path));
        if (!sd.isMap())
        {
            LL_WARNS("ViewerImport") << "Could not read " << path << "; only same-named settings will be offered" << LL_ENDL;
        }
        const LLSD& s = sd["settings"];
        m.rename      = s["rename"];
        m.ranges      = s["ranges"];
        m.enums       = s["enums"];
        m.validate    = s["validate"];
        m.unmappable  = s["unmappable"];
        for (const LLSD& e : llsd::inArray(s["render_allow"])) m.renderAllow.insert(e.asString());
        for (const LLSD& e : llsd::inArray(s["deny_exact"])) m.denyExact.insert(e.asString());
        for (const LLSD& e : llsd::inArray(s["restart"])) m.restart.insert(e.asString());
        for (const LLSD& e : llsd::inArray(s["deny_contains"])) m.denyContains.push_back(lower(e.asString()));
        for (const LLSD& e : llsd::inArray(s["deny_regex"]))
        {
            try
            {
                m.denyRegex.emplace_back(e.asString());
            }
            catch (const std::regex_error&)
            {
                LL_WARNS("ViewerImport") << "Bad deny_regex in viewer_import_map.xml: " << e.asString() << LL_ENDL;
            }
        }
        m.toolbarCommands = sd["toolbars"]["commands"];
        m.toolbarNotes    = sd["toolbars"]["notes"];
        m.keyCommands     = sd["key_bindings"]["commands"];
        m.bdModes         = sd["key_bindings"]["bd_modes"];
        for (const LLSD& e : llsd::inArray(sd["key_bindings"]["core"])) m.core.insert(e.asString());
        if (sd["key_bindings"].has("max_per_command")) m.maxPerCommand = llmax(1, sd["key_bindings"]["max_per_command"].asInteger());
        return m;
    }

    bool denied(const std::string& name, std::string& why) const
    {
        const std::string low = lower(name);
        for (const std::string& c : denyContains)
        {
            if (!c.empty() && low.find(c) != std::string::npos)
            {
                why = "Credentials and account secrets are never imported";
                return true;
            }
        }
        if (denyExact.count(name))
        {
            why = "Specific to that install or machine";
            return true;
        }
        for (const std::regex& re : denyRegex)
        {
            if (std::regex_search(name, re))
            {
                why = "Specific to that install, machine or session";
                return true;
            }
        }
        return false;
    }

    std::string unmappableReason(const std::string& name) const
    {
        std::string best, why;
        for (LLSD::map_const_iterator it = unmappable.beginMap(); it != unmappable.endMap(); ++it)
        {
            if (startsWith(name, it->first) && it->first.size() > best.size())
            {
                best = it->first;
                why = it->second.asString();
            }
        }
        return why;
    }
};

bool viewerMatches(const LLSD& viewers, const std::string& id)
{
    if (!viewers.isArray() || viewers.size() == 0) return true;
    for (const LLSD& v : llsd::inArray(viewers))
    {
        if (v.asString() == id) return true;
    }
    return false;
}

// Apply a conversion from the map. source_file: the whole settings file, for conversions
// that depend on another of its settings ("if_source").
bool convertValue(const LLSD& conv, const LLSD& value, const LLSD& source_file, LLSD& out, std::string& why)
{
    const std::string type = conv["type"].asString();
    if (type == "prefix")
    {
        const std::string prefix = conv["prefix"].asString();
        std::string s = value.asString();
        if (!s.empty() && !startsWith(s, prefix)) s = prefix + s;
        out = LLSD(s);
        return true;
    }
    if (type == "bool")
    {
        out = LLSD(value.asBoolean());
        return true;
    }
    if (type == "invert")
    {
        out = LLSD(!value.asBoolean());
        return true;
    }
    if (type == "scale")
    {
        out = LLSD(value.asReal() * conv["factor"].asReal());
        return true;
    }
    if (type == "enum")
    {
        LLSD map = conv["map"];
        const LLSD& cond = conv["if_source"];
        if (cond.isMap())
        {
            const LLSD& other = source_file[cond["setting"].asString()]["Value"];
            const bool match = cond["value"].isBoolean() ? (other.isDefined() && other.asBoolean() == cond["value"].asBoolean())
                                                         : (other.asString() == cond["value"].asString());
            if (match) map = cond["map"];
        }
        const std::string key = enumKey(value);
        if (!map.has(key))
        {
            why = "Value " + key + " has no equivalent here";
            return false;
        }
        out = map[key];
        return true;
    }
    why = "Unknown conversion '" + type + "' in viewer_import_map.xml";
    return false;
}

// Range / enum / special checks. May clamp (note says so). False: not importable.
bool validateValue(const std::string& name, eControlType type, LLSD& value, std::string& note, std::string& why)
{
    const ImportMap& map = ImportMap::instance();
    if (map.enums.has(name) && (type == TYPE_S32 || type == TYPE_U32 || type == TYPE_F32 || type == TYPE_STRING))
    {
        bool ok = false;
        for (const LLSD& e : llsd::inArray(map.enums[name]))
        {
            if (type == TYPE_STRING ? (e.asString() == value.asString())
                                    : (std::fabs(atof(e.asString().c_str()) - value.asReal()) < 1e-6))
            {
                ok = true;
                break;
            }
        }
        if (!ok)
        {
            why = "Value " + displayValue(value) + " is not one this viewer offers";
            return false;
        }
    }
    if (map.ranges.has(name) && (type == TYPE_S32 || type == TYPE_U32 || type == TYPE_F32))
    {
        const F64 lo = map.ranges[name][0].asReal(), hi = map.ranges[name][1].asReal();
        const F64 v = value.asReal();
        if (v < lo || v > hi)
        {
            const F64 c = llclamp(v, lo, hi);
            note = "Clamped from " + displayValue(value) + " to this viewer's range";
            value = (type == TYPE_F32) ? LLSD(c) : LLSD((S32)ll_round(c));
        }
    }
    const std::string check = map.validate[name].asString();
    if (check == "fontset")
    {
        const std::string file = value.asString();
        const bool found = file == "fonts.xml"
            || gDirUtilp->fileExists(gDirUtilp->add(gDirUtilp->add(gDirUtilp->getAppRODataDir(), "fonts"), file))
            || gDirUtilp->fileExists(gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, "fonts", file));
        if (!found)
        {
            why = "Font set " + file + " is not installed here (import the Fonts category first)";
            return false;
        }
    }
    return true;
}

// floater_rect_<name> (Rect) / floater_pos_<name>_x|_y (F32): window positions.
bool layoutControl(const std::string& name, std::string& floater, bool& is_rect)
{
    if (startsWith(name, "floater_rect_"))
    {
        floater = name.substr(13);
        is_rect = true;
        return !floater.empty();
    }
    if (startsWith(name, "floater_pos_") && (endsWith(name, "_x") || endsWith(name, "_y")))
    {
        floater = name.substr(12, name.size() - 14);
        is_rect = false;
        return !floater.empty();
    }
    return false;
}

LLControlGroup* groupNamed(const std::string& group)
{
    if (group == "account") return &gSavedPerAccountSettings;
    if (group == "warnings") return &gWarningSettings;
    return &gSavedSettings;
}

// ---------------------------------------------------------------------------
// Key bindings

struct KeyEntry
{
    std::string     key, mask, mouse, command;  // as in the file
    bool            valid = false;              // names a real key or mouse button
    KEY             k = KEY_NONE;
    MASK            m = MASK_NONE;
    EMouseClickType c = CLICK_NONE;
};
using KeyModes = std::array<std::vector<KeyEntry>, MODE_COUNT>;

bool finishEntry(KeyEntry& e)
{
    e.valid = false;
    e.k = KEY_NONE;
    e.m = MASK_NONE;
    e.c = CLICK_NONE;
    if (!e.key.empty() && !LLKeyboard::keyFromString(e.key, &e.k)) return false;
    if (!e.mask.empty() && !LLKeyboard::maskFromString(e.mask, &e.m)) return false;
    if (!e.mouse.empty() && !LLViewerInput::mouseFromString(e.mouse, &e.c)) return false;
    e.valid = (e.k != KEY_NONE || e.c != CLICK_NONE);
    return e.valid;
}

bool sameCombo(const KeyEntry& a, const KeyEntry& b)
{
    return a.valid && b.valid && a.k == b.k && a.m == b.m && a.c == b.c;
}

// The file spelling LLViewerInput reads (LLKeyboard::stringFromMask is for the UI).
std::string maskString(MASK mask)
{
    std::string res;
    if (mask & MASK_CONTROL) res = "CTL";
    if (mask & MASK_ALT) { if (!res.empty()) res += "_"; res += "ALT"; }
    if (mask & MASK_SHIFT) { if (!res.empty()) res += "_"; res += "SHIFT"; }
    return res.empty() ? std::string("NONE") : res;
}

std::string comboLabel(const KeyEntry& e)
{
    if (e.c != CLICK_NONE) return LLKeyboard::stringFromAccelerator(e.m, e.c);
    return LLKeyboard::stringFromAccelerator(e.m, e.k);
}

S32 modeIndex(const std::string& tag)
{
    for (S32 i = 0; i < MODE_COUNT; ++i)
    {
        if (tag == MODE_TAGS[i]) return i;
    }
    return -1;
}

// key_bindings.xml, read leniently (attributes only; no LLInitParam validation, so
// another viewer's extra attributes or commands cannot make the whole file fail).
bool readKeyBindingsXml(const fs::path& p, KeyModes& out, std::array<bool, MODE_COUNT>& present, S32& version)
{
    LLXMLNodePtr root;
    if (!LLXMLNode::parseFile(utf8(p), root, nullptr) || root.isNull() || !root->hasName("keys")) return false;
    version = 0;
    root->getAttributeS32("xml_version", version);
    for (LLXMLNodePtr mode = root->getFirstChild(); mode.notNull(); mode = mode->getNextSibling())
    {
        S32 idx = -1;
        for (S32 i = 0; i < MODE_COUNT; ++i)
        {
            if (mode->hasName(MODE_TAGS[i])) idx = i;
        }
        if (idx < 0) continue;
        present[idx] = true;
        for (LLXMLNodePtr b = mode->getFirstChild(); b.notNull(); b = b->getNextSibling())
        {
            if (!b->hasName("binding")) continue;
            KeyEntry e;
            b->getAttributeString("key", e.key);
            b->getAttributeString("mask", e.mask);
            b->getAttributeString("mouse", e.mouse);
            b->getAttributeString("command", e.command);
            finishEntry(e);
            out[idx].push_back(e);
        }
    }
    return true;
}

// Black Dragon's controls.xml: one <llsd><map>function, key, mask, mode, mouse, slot</map></llsd> per line.
bool readBlackDragonControls(const fs::path& p, KeyModes& out)
{
    std::string text;
    if (!readText(p, text)) return false;
    const LLSD& modes = ImportMap::instance().bdModes;
    size_t pos = 0;
    while ((pos = text.find("<llsd>", pos)) != std::string::npos)
    {
        size_t end = text.find("</llsd>", pos);
        if (end == std::string::npos) break;
        end += 7;
        std::istringstream in(text.substr(pos, end - pos));
        pos = end;
        LLSD sd;
        if (LLSDSerialize::fromXML(sd, in, false) <= 0 || !sd.isMap()) continue;
        const S32 idx = modeIndex(modes[std::to_string(sd["mode"].asInteger())].asString());
        if (idx < 0) continue;   // Black Dragon's build-mode bindings: no such mode here
        KeyEntry e;
        e.key = sd["key"].asString();
        e.mask = sd["mask"].asString();
        const S32 mouse = sd.has("mouse") ? sd["mouse"].asInteger() : -1;
        if (mouse >= 0 && mouse < CLICK_COUNT) e.mouse = LLKeyboard::stringFromMouse((EMouseClickType)mouse, false);
        e.command = sd["function"].asString();
        finishEntry(e);
        out[idx].push_back(e);
    }
    return true;
}

std::string writeKeyBindingsXml(const KeyModes& modes, const std::array<bool, MODE_COUNT>& present, S32 version)
{
    std::ostringstream o;
    o << "<?xml version=\"1.0\" encoding=\"utf-8\" standalone=\"yes\" ?>\n";
    // Keep a pre-LMB file's version so LLViewerInput still adds its script-click bindings.
    if (version >= 1) o << "<keys xml_version=\"" << version << "\">\n";
    else o << "<keys>\n";
    // Same mode order as LL's file.
    static const S32 order[MODE_COUNT] = { MODE_FIRST_PERSON, MODE_THIRD_PERSON, MODE_SITTING, MODE_EDIT_AVATAR };
    for (S32 i : order)
    {
        if (!present[i]) continue;
        o << "  <" << MODE_TAGS[i] << ">\n";
        for (const KeyEntry& e : modes[i])
        {
            o << "    <binding key=\"" << xmlEscape(e.key) << "\" mask=\"" << xmlEscape(e.mask.empty() ? std::string("NONE") : e.mask) << "\"";
            if (!e.mouse.empty()) o << " mouse=\"" << xmlEscape(e.mouse) << "\"";
            o << " command=\"" << xmlEscape(e.command) << "\"/>\n";
        }
        o << "  </" << MODE_TAGS[i] << ">\n";
    }
    o << "</keys>\n";
    return o.str();
}

// key_bindings.xml also holds the user's menu and other shortcut changes in a <commands>
// section (#863). They are not an imported viewer's movement keys: carry the section over
// from the file being replaced.
std::string keepCommandsSection(const fs::path& old_file, std::string xml)
{
    std::string old_text;
    std::error_code ec;
    if (!fs::exists(old_file, ec) || !readText(old_file, old_text))
    {
        return xml;
    }
    const size_t begin = old_text.find("<commands");
    if (begin == std::string::npos)
    {
        return xml;
    }
    size_t end = old_text.find("</commands>", begin);
    if (end != std::string::npos)
    {
        end += std::string("</commands>").size();
    }
    else
    {
        // an empty <commands/>
        end = old_text.find("/>", begin);
        if (end == std::string::npos) return xml;
        end += 2;
    }
    const size_t close = xml.rfind("</keys>");
    if (close == std::string::npos)
    {
        return xml;
    }
    xml.insert(close, "  " + old_text.substr(begin, end - begin) + "\n");
    return xml;
}

S32 bindingCount(const std::vector<KeyEntry>& mode, const std::string& command)
{
    S32 n = 0;
    for (const KeyEntry& e : mode)
    {
        if (e.valid && e.command == command) ++n;
    }
    return n;
}

fs::path ourKeyBindingsFile(bool& user_file)
{
    const fs::path user = u8(gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, "key_bindings.xml"));
    std::error_code ec;
    user_file = fs::exists(user, ec);
    return user_file ? user : u8(gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "key_bindings.xml"));
}

// ---------------------------------------------------------------------------
// Toolbars

struct ToolbarDef
{
    bool provided = false;
    std::string display, layout;
    std::vector<std::string> commands;
};
const char* const TOOLBAR_TAGS[3] = { "left_toolbar", "right_toolbar", "bottom_toolbar" };

bool readToolbars(const fs::path& p, std::array<ToolbarDef, 3>& bars)
{
    LLXMLNodePtr root;
    if (!LLXMLNode::parseFile(utf8(p), root, nullptr) || root.isNull() || !root->hasName("toolbars")) return false;
    for (LLXMLNodePtr bar = root->getFirstChild(); bar.notNull(); bar = bar->getNextSibling())
    {
        for (S32 i = 0; i < 3; ++i)
        {
            if (!bar->hasName(TOOLBAR_TAGS[i])) continue;
            ToolbarDef& def = bars[i];
            def.provided = true;
            bar->getAttributeString("button_display_mode", def.display);
            bar->getAttributeString("button_layout_mode", def.layout);
            for (LLXMLNodePtr c = bar->getFirstChild(); c.notNull(); c = c->getNextSibling())
            {
                std::string name;
                if (c->hasName("command") && c->getAttributeString("name", name) && !name.empty()) def.commands.push_back(name);
            }
        }
    }
    return true;
}

std::string writeToolbars(const std::array<ToolbarDef, 3>& bars)
{
    std::ostringstream o;
    o << "<?xml version=\"1.0\" encoding=\"utf-8\" standalone=\"yes\" ?>\n<toolbars>\n";
    static const S32 order[3] = { 2, 0, 1 };   // bottom, left, right, as LL writes it
    for (S32 i : order)
    {
        const ToolbarDef& def = bars[i];
        if (!def.provided) continue;
        o << "  <" << TOOLBAR_TAGS[i];
        if (!def.display.empty()) o << "\n    button_display_mode=\"" << xmlEscape(def.display) << "\"";
        if (!def.layout.empty()) o << "\n    button_layout_mode=\"" << xmlEscape(def.layout) << "\"";
        o << ">\n";
        for (const std::string& c : def.commands) o << "    <command name=\"" << xmlEscape(c) << "\"/>\n";
        o << "  </" << TOOLBAR_TAGS[i] << ">\n";
    }
    o << "</toolbars>\n";
    return o.str();
}

std::string toolbarSummary(const std::array<ToolbarDef, 3>& bars)
{
    return llformat("left %d, right %d, bottom %d", (S32)bars[0].commands.size(), (S32)bars[1].commands.size(), (S32)bars[2].commands.size());
}

// ---------------------------------------------------------------------------
// Misc readers

std::vector<std::string> readDictionaryWords(const fs::path& p)
{
    std::vector<std::string> words;
    std::string text;
    if (!readText(p, text)) return words;
    std::istringstream in(text);
    std::string line;
    bool first = true;
    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (first)
        {
            first = false;
            // Hunspell .dic: the first line is the word count.
            if (!line.empty() && line.find_first_not_of("0123456789") == std::string::npos) continue;
        }
        if (!line.empty()) words.push_back(line);
    }
    return words;
}

std::vector<LLSD> readTeleportHistory(const fs::path& p)
{
    std::vector<LLSD> items;
    std::string text;
    if (!readText(p, text)) return items;
    std::istringstream in(text);
    std::string line;
    LLPointer<LLSDParser> parser = new LLSDNotationParser();
    while (std::getline(in, line))
    {
        if (line.empty()) continue;
        LLSD item;
        std::istringstream iss(line);
        if (parser->parse(iss, item, line.length()) == LLSDParser::PARSE_FAILURE) continue;
        if (item.has("title") && item.has("global_pos")) items.push_back(item);
    }
    return items;
}

// ---------------------------------------------------------------------------
// Backup + undo journal

class Backup
{
public:
    bool begin(const std::string& source_label)
    {
        char stamp[32];
        const std::time_t now = std::time(nullptr);
        std::strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H%M%S", std::localtime(&now));
        const fs::path base = userSettingsDir() / BACKUP_DIR;
        std::error_code ec;
        mDir = base / stamp;
        for (S32 n = 2; fs::exists(mDir, ec) && n < 100; ++n) mDir = base / (std::string(stamp) + "-" + std::to_string(n));
        if (!fs::create_directories(mDir, ec) || ec) return false;
        mJournal["source"] = source_label;
        mJournal["time"] = stamp;
        mJournal["account_dir"] = gDirUtilp->getLindenUserDir();
        mJournal["settings"] = LLSD::emptyArray();
        mJournal["colors"] = LLSD::emptyArray();
        mJournal["files"] = LLSD::emptyArray();
        mJournal["created"] = LLSD::emptyArray();
        mJournal["teleports"] = LLSD::emptyArray();
        return true;
    }

    // Keep a copy of a live file before we change it (once per import).
    void file(const fs::path& live, const std::string& tag, const std::string& reload)
    {
        if (mSaved.count(tag)) return;
        mSaved.insert(tag);
        std::error_code ec;
        const bool existed = fs::exists(live, ec);
        if (existed) fs::copy_file(live, mDir / tag, fs::copy_options::overwrite_existing, ec);
        LLSD e;
        e["tag"] = tag;
        e["path"] = utf8(live);
        e["existed"] = existed;
        e["reload"] = reload;
        mJournal["files"].append(e);
    }

    void created(const fs::path& p) { mJournal["created"].append(utf8(p)); }

    void teleportAdded(const std::string& title, const LLVector3d& pos)
    {
        LLSD e;
        e["title"] = title;
        e["global_pos"] = pos.getValue();
        mJournal["teleports"].append(e);
    }

    void setting(const std::string& group, const std::string& name, bool existed, const LLSD& previous)
    {
        LLSD e;
        e["group"] = group;
        e["name"] = name;
        e["existed"] = existed;
        e["value"] = previous;
        mJournal["settings"].append(e);
    }

    void color(const std::string& name, bool user_set, const LLColor4& previous)
    {
        LLSD e;
        e["name"] = name;
        e["user_set"] = user_set;
        e["value"] = previous.getValue();
        mJournal["colors"].append(e);
    }

    // Reference copies of what the settings were, for anyone restoring by hand.
    void snapshotSettings(bool logged_in)
    {
        gSavedSettings.saveToFile(utf8(mDir / "settings.xml"), true);
        gWarningSettings.saveToFile(utf8(mDir / "ignorable_dialogs.xml"), true);
        if (logged_in) gSavedPerAccountSettings.saveToFile(utf8(mDir / "settings_per_account.xml"), true);
    }

    bool commit() { return writeLLSDFile(mDir / JOURNAL_FILE, mJournal); }

    const fs::path& dir() const { return mDir; }

private:
    fs::path              mDir;
    LLSD                  mJournal;
    std::set<std::string> mSaved;
};

void reloadAfterFileChange(const std::string& what)
{
    if (what == "keys")
    {
        bool user_file = false;
        const fs::path p = ourKeyBindingsFile(user_file);
        gViewerInput.loadBindingsXML(utf8(p));
    }
    else if (what == "toolbars")
    {
        if (gToolBarView) gToolBarView->loadToolbars(false);
    }
    else if (what == "contact_sets")
    {
        LGGContactSets::getInstance()->reloadFromDisk();
    }
    else if (what == "autoreplace")
    {
        const LLSD sd = readLLSDFile(u8(gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, "autoreplace.xml")));
        LLAutoReplaceSettings settings;
        if (sd.isArray() && settings.setFromLLSD(sd)) LLAutoReplace::instance().setSettings(settings);
    }
}

void saveSettingGroups(bool logged_in)
{
    gSavedSettings.saveToFile(gSavedSettings.getString("ClientSettingsFile"), true);
    if (logged_in) gSavedPerAccountSettings.saveToFile(gSavedSettings.getString("PerAccountSettingsFile"), true);
    gWarningSettings.saveToFile(gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, "ignorable_dialogs.xml"), true);
}

PlanItem makeItem(ECategory cat, EStatus status, const std::string& name)
{
    PlanItem item;
    item.cat = cat;
    item.status = status;
    item.apply = (status == STATUS_OFFER);
    item.name = name;
    return item;
}
} // namespace

const char* categoryKey(ECategory cat)
{
    return (cat >= 0 && cat < CAT_COUNT) ? CATEGORY_KEYS[cat] : "";
}

bool categoryOnByDefault(ECategory cat)
{
    return (cat >= 0 && cat < CAT_COUNT) ? CATEGORY_DEFAULT_ON[cat] : false;
}

S32 ImportPlan::count(ECategory cat, EStatus status) const
{
    S32 n = 0;
    for (const PlanItem& item : mItems)
    {
        if (item.cat == cat && item.status == status) ++n;
    }
    return n;
}

S32 ImportPlan::countSelected(ECategory cat) const
{
    S32 n = 0;
    for (const PlanItem& item : mItems)
    {
        if (item.cat == cat && willApply(item)) ++n;
    }
    return n;
}

// ===========================================================================
// Build

void ImportPlan::build(const SourceViewer& viewer, const SourceAccount* account, bool logged_in)
{
    mItems.clear();
    for (S32 i = 0; i < CAT_COUNT; ++i) mCategoryOn[i] = CATEGORY_DEFAULT_ON[i];
    mViewer = viewer;
    mHasAccount = account != nullptr;
    mAccount = account ? *account : SourceAccount();
    mLoggedIn = logged_in;

    mSourceGlobal = readLLSDFile(mViewer.dir / "user_settings" / "settings.xml");
    mSourceAccount = mHasAccount ? readLLSDFile(mAccount.settings_dir / "settings_per_account.xml") : LLSD();

    buildSettingsFile(mSourceGlobal, false, logged_in);
    if (mHasAccount) buildSettingsFile(mSourceAccount, true, logged_in);
    buildNotifications();
    buildColors();
    buildToolbars();
    buildKeys();
    buildContactSets();
    buildTeleportHistory();
    buildPresets();
    if (mViewer.id == "firestorm" || mViewer.id == "alchemy")
    {
        // Firestorm-format poser files; Black Dragon's poses share the folder name but not the format.
        buildCopiedFiles(CAT_POSES, "poses", { ".xml", ".bvh" });
    }
    buildCopiedFiles(CAT_FONTS, "fonts", { ".xml", ".ttf", ".otf", ".ttc" });
    buildCopiedFiles(CAT_WINDLIGHT, "windlight", { ".xml" });
    buildAutoReplace();
    buildSpelling();
    buildChat();

    S32 offered = 0, same = 0, skipped = 0;
    for (const PlanItem& item : mItems)
    {
        if (item.status == STATUS_OFFER || item.status == STATUS_CONFLICT) ++offered;
        else if (item.status == STATUS_SAME) ++same;
        else ++skipped;
    }
    LL_INFOS("ViewerImport") << "Plan for " << mViewer.label << " (" << utf8(mViewer.dir) << ")"
                             << (mHasAccount ? ", account " + mAccount.name : std::string()) << ": "
                             << offered << " to review, " << same << " already the same, " << skipped << " not importable" << LL_ENDL;
    for (const PlanItem& item : mItems)
    {
        if (item.status == STATUS_SKIP || item.status == STATUS_CONFLICT)
        {
            LL_DEBUGS("ViewerImport") << "  " << categoryKey(item.cat) << " / " << item.name << ": "
                                      << (item.status == STATUS_SKIP ? "skipped" : "conflict") << " - " << item.note << LL_ENDL;
        }
    }
}

void ImportPlan::buildSettingsFile(const LLSD& file, bool account_file, bool logged_in)
{
    const ImportMap& map = ImportMap::instance();
    std::set<std::string> offered;   // "group:name" already offered (a later source can't override it)
    for (const PlanItem& item : mItems)
    {
        if (item.data.has("name")) offered.insert(item.data["group"].asString() + ":" + item.data["name"].asString());
    }

    for (LLSD::map_const_iterator it = file.beginMap(); it != file.endMap(); ++it)
    {
        const std::string& src_name = it->first;
        const LLSD& entry = it->second;
        if (!entry.isMap() || !entry.has("Value")) continue;

        std::string target = src_name;
        LLSD conv;
        std::string note;
        bool renamed = false;
        if (map.rename.has(src_name) && viewerMatches(map.rename[src_name]["viewers"], mViewer.id))
        {
            const LLSD& r = map.rename[src_name];
            target = r["to"].asString();
            conv = r["convert"];
            renamed = true;
            note = mViewer.label + ": " + src_name;
            if (r.has("note")) note += " (" + r["note"].asString() + ")";
        }

        PlanItem item = makeItem(account_file ? CAT_ACCOUNT : CAT_SETTINGS, STATUS_SKIP, target);
        item.imported = displayValue(entry["Value"]);
        item.note = note;
        auto skip = [&](const std::string& why)
        {
            item.status = STATUS_SKIP;
            item.apply = false;
            item.note = note.empty() ? why : note + " - " + why;
            mItems.push_back(item);
        };

        // Window positions have their own category.
        std::string floater;
        bool is_rect = false;
        if (!renamed && layoutControl(src_name, floater, is_rect))
        {
            item.cat = CAT_LAYOUT;
            if (account_file) { skip("Window positions are global here"); continue; }
            if (!LLFloaterReg::isRegistered(floater)) { skip("No window named " + floater + " here"); continue; }
            const eControlType want = is_rect ? TYPE_RECT : TYPE_F32;
            LLSD value;
            if (LLControlGroup::typeStringToEnum(entry["Type"].asString()) != want || !coerce(entry["Value"], want, value))
            {
                skip("Unexpected type");
                continue;
            }
            LLControlVariable* ctrl = gSavedSettings.getControl(src_name);
            item.current = ctrl ? displayValue(ctrl->getValue()) : std::string("(default)");
            item.data["group"] = "global";
            item.data["name"] = src_name;
            item.data["value"] = value;
            item.data["declare"] = is_rect ? "rect" : "f32";
            item.data["floater"] = floater;
            if (ctrl && valuesEqual(ctrl->getValue(), value)) item.status = STATUS_SAME;
            else { item.status = STATUS_OFFER; item.apply = true; }
            mItems.push_back(item);
            continue;
        }

        std::string why;
        if (!renamed && map.denied(src_name, why)) { skip(why); continue; }
        if (map.denied(target, why)) { skip(why); continue; }
        if (startsWith(target, "Render") && !map.renderAllow.count(target))
        {
            skip(map.unmappableReason("Render"));
            continue;
        }

        // Where our control lives (a viewer may keep a setting per account that we keep
        // globally, or the other way round).
        LLControlGroup* groups[2] = { account_file ? &gSavedPerAccountSettings : &gSavedSettings,
                                      account_file ? &gSavedSettings : &gSavedPerAccountSettings };
        LLControlVariable* ctrl = nullptr;
        LLControlGroup* group = nullptr;
        for (LLControlGroup* g : groups)
        {
            if (LLControlVariable* c = g->getControl(target))
            {
                ctrl = c;
                group = g;
                break;
            }
        }
        if (!ctrl)
        {
            std::string reason = renamed ? std::string() : map.unmappableReason(src_name);
            skip(reason.empty() ? std::string("No matching setting here") : reason);
            continue;
        }
        const bool per_account = (group == &gSavedPerAccountSettings);
        item.cat = per_account ? CAT_ACCOUNT : CAT_SETTINGS;
        if (per_account && !logged_in) { skip("Log in to import per-account settings"); continue; }
        if (!ctrl->isPersisted()) { skip("This viewer does not save it"); continue; }
        item.current = displayValue(ctrl->getValue());

        LLSD value = entry["Value"];
        eControlType from = LLControlGroup::typeStringToEnum(entry["Type"].asString());
        if (conv.isMap())
        {
            LLSD converted;
            if (!convertValue(conv, value, file, converted, why)) { skip(why); continue; }
            value = converted;
            from = ctrl->type();
        }
        if (!typesCompatible(from, ctrl->type()))
        {
            skip(llformat("Type differs (theirs %s, ours %s)", typeLabel(from), typeLabel(ctrl->type())));
            continue;
        }
        LLSD coerced;
        if (!coerce(value, ctrl->type(), coerced)) { skip("Value does not fit this setting"); continue; }
        std::string clamp_note;
        if (!validateValue(target, ctrl->type(), coerced, clamp_note, why)) { skip(why); continue; }
        if (!clamp_note.empty()) note = note.empty() ? clamp_note : note + " - " + clamp_note;

        const std::string key = std::string(per_account ? "account:" : "global:") + target;
        if (offered.count(key)) { skip("Already taken from another setting of theirs"); continue; }
        offered.insert(key);

        item.imported = displayValue(coerced);
        item.note = note;
        item.data["group"] = per_account ? "account" : "global";
        item.data["name"] = target;
        item.data["value"] = coerced;
        if (valuesEqual(ctrl->getValue(), coerced))
        {
            item.status = STATUS_SAME;
            item.apply = false;
        }
        else
        {
            item.status = STATUS_OFFER;
            item.apply = true;
        }
        mItems.push_back(item);
    }
}

void ImportPlan::buildNotifications()
{
    const LLSD file = readLLSDFile(mViewer.dir / "user_settings" / "ignorable_dialogs.xml");
    for (LLSD::map_const_iterator it = file.beginMap(); it != file.endMap(); ++it)
    {
        const LLSD& entry = it->second;
        if (!entry.isMap() || !entry.has("Value")) continue;
        PlanItem item = makeItem(CAT_NOTIFICATIONS, STATUS_SKIP, it->first);
        item.imported = displayValue(entry["Value"]);
        LLControlVariable* ctrl = gWarningSettings.getControl(it->first);
        LLSD value;
        if (!ctrl)
        {
            item.note = "No such notification here";
        }
        else if (!typesCompatible(LLControlGroup::typeStringToEnum(entry["Type"].asString()), ctrl->type())
                 || !coerce(entry["Value"], ctrl->type(), value))
        {
            item.note = "Type differs";
        }
        else
        {
            item.current = displayValue(ctrl->getValue());
            item.data["group"] = "warnings";
            item.data["name"] = it->first;
            item.data["value"] = value;
            item.status = valuesEqual(ctrl->getValue(), value) ? STATUS_SAME : STATUS_OFFER;
            item.apply = item.status == STATUS_OFFER;
        }
        mItems.push_back(item);
    }
}

void ImportPlan::buildColors()
{
    const fs::path path = mViewer.dir / "user_settings" / "colors.xml";
    std::error_code ec;
    if (!fs::exists(path, ec)) return;
    LLXMLNodePtr root;
    if (!LLXMLNode::parseFile(utf8(path), root, nullptr) || root.isNull() || !root->hasName("colors")) return;
    LLUIColorTable::Params params;
    LLXUIParser parser;
    parser.readXUI(root, params, utf8(path));
    if (!params.validateBlock(false)) return;
    LLUIColorTable& table = LLUIColorTable::instance();
    for (LLInitParam::ParamIterator<LLUIColorTable::ColorEntryParams>::const_iterator it = params.color_entries.begin();
         it != params.color_entries.end(); ++it)
    {
        const LLUIColorTable::ColorEntryParams& entry = *it;
        PlanItem item = makeItem(CAT_COLORS, STATUS_SKIP, entry.name());
        if (!entry.color.value.isChosen())
        {
            item.imported = entry.color.reference();
            item.note = "Refers to their skin's colour " + entry.color.reference();
            mItems.push_back(item);
            continue;
        }
        const LLColor4 theirs = entry.color.value();
        item.imported = displayValue(theirs.getValue());
        if (!table.colorExists(entry.name()))
        {
            item.note = "No such colour here";
            mItems.push_back(item);
            continue;
        }
        const LLColor4 ours = table.getColor(entry.name()).get();
        item.current = displayValue(ours.getValue());
        item.data["name"] = entry.name();
        item.data["value"] = theirs.getValue();
        item.status = valuesEqual(ours.getValue(), theirs.getValue()) ? STATUS_SAME : STATUS_OFFER;
        item.apply = item.status == STATUS_OFFER;
        mItems.push_back(item);
    }
}

void ImportPlan::buildToolbars()
{
    if (!mHasAccount) return;
    const fs::path src = mAccount.settings_dir / "toolbars.xml";
    std::error_code ec;
    if (!fs::exists(src, ec)) return;
    PlanItem item = makeItem(CAT_TOOLBARS, STATUS_SKIP, "Toolbar layout");
    if (!mLoggedIn)
    {
        item.note = "Log in to import the toolbars (they are kept per account)";
        mItems.push_back(item);
        return;
    }
    std::array<ToolbarDef, 3> theirs, ours;
    if (!readToolbars(src, theirs))
    {
        item.note = "Could not read " + utf8(src);
        mItems.push_back(item);
        return;
    }
    const fs::path ours_file = accountDir() / "toolbars.xml";
    if (!readToolbars(ours_file, ours)) readToolbars(u8(gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "toolbars.xml")), ours);

    const ImportMap& map = ImportMap::instance();
    static const std::set<std::string> display_modes = { "icons_with_text", "icons_only", "icons_only_small", "text_only" };
    static const std::set<std::string> layout_modes = { "none", "left", "right", "fill" };
    std::set<std::string> used;
    std::vector<std::string> dropped;
    std::array<ToolbarDef, 3> result = ours;
    for (S32 i = 0; i < 3; ++i)
    {
        const ToolbarDef& src_bar = theirs[i];
        if (!src_bar.provided) continue;   // keep ours for a bar their file doesn't define
        ToolbarDef bar;
        bar.provided = true;
        bar.display = display_modes.count(src_bar.display) ? src_bar.display : ours[i].display;
        bar.layout = layout_modes.count(src_bar.layout) ? src_bar.layout : ours[i].layout;
        for (const std::string& raw : src_bar.commands)
        {
            const std::string name = map.toolbarCommands.has(raw) ? map.toolbarCommands[raw].asString() : raw;
            if (!LLCommandManager::instance().getCommand(LLCommandId(name)))
            {
                const std::string why = map.toolbarNotes[raw].asString();
                dropped.push_back(why.empty() ? raw : raw + " (" + why + ")");
                continue;
            }
            if (!used.insert(name).second) continue;   // a button can be on one toolbar only
            bar.commands.push_back(name);
        }
        result[i] = bar;
    }
    item.current = toolbarSummary(ours);
    item.imported = toolbarSummary(result);
    if (!dropped.empty())
    {
        item.note = "Not available here: ";
        for (size_t i = 0; i < dropped.size(); ++i) item.note += (i ? ", " : "") + dropped[i];
    }
    const std::string xml = writeToolbars(result);
    bool same = true;
    for (S32 i = 0; i < 3; ++i)
    {
        same = same && result[i].commands == ours[i].commands && result[i].display == ours[i].display && result[i].layout == ours[i].layout;
    }
    item.data["xml"] = xml;
    item.status = same ? STATUS_SAME : STATUS_OFFER;
    item.apply = !same;
    mItems.push_back(item);
}

void ImportPlan::buildKeys()
{
    const bool bd = !mViewer.key_bindings_compatible;
    const fs::path src = mViewer.dir / "user_settings" / (bd ? "controls.xml" : "key_bindings.xml");
    std::error_code ec;
    if (!fs::exists(src, ec)) return;

    KeyModes theirs;
    std::array<bool, MODE_COUNT> their_present{};
    S32 their_version = 0;
    const bool read_ok = bd ? readBlackDragonControls(src, theirs) : readKeyBindingsXml(src, theirs, their_present, their_version);
    if (!read_ok)
    {
        PlanItem item = makeItem(CAT_KEYS, STATUS_SKIP, "Key bindings");
        item.note = "Could not read " + utf8(src) + "; yours are left as they are";
        mItems.push_back(item);
        return;
    }
    bool user_file = false;
    KeyModes ours;
    std::array<bool, MODE_COUNT> our_present{};
    S32 our_version = 0;
    readKeyBindingsXml(ourKeyBindingsFile(user_file), ours, our_present, our_version);

    const ImportMap& map = ImportMap::instance();
    std::set<std::string> seen;
    for (S32 mode = 0; mode < MODE_COUNT; ++mode)
    {
        std::map<std::string, S32> planned;   // additions per command in this mode
        for (const KeyEntry& e : theirs[mode])
        {
            if (!e.valid)
            {
                if (!e.command.empty() && !e.key.empty())
                {
                    PlanItem item = makeItem(CAT_KEYS, STATUS_SKIP, std::string(MODE_LABELS[mode]) + ": " + e.key);
                    item.imported = e.command;
                    item.note = "Unknown key or mouse button";
                    mItems.push_back(item);
                }
                continue;   // empty placeholder slots
            }
            const std::string command = map.keyCommands.has(e.command) ? map.keyCommands[e.command].asString() : e.command;
            const std::string label = std::string(MODE_LABELS[mode]) + ": " + comboLabel(e);
            const std::string dedupe = llformat("%d|%d|%d|%d|", mode, (S32)e.k, (S32)e.m, (S32)e.c) + command;
            if (!seen.insert(dedupe).second) continue;

            PlanItem item = makeItem(CAT_KEYS, STATUS_SKIP, label);
            item.imported = command;
            if (command != e.command) item.note = mViewer.label + ": " + e.command;
            item.data["mode"] = mode;
            item.data["key"] = e.k != KEY_NONE ? LLKeyboard::stringFromKey(e.k, false) : std::string();
            item.data["mask"] = maskString(e.m);
            item.data["mouse"] = e.c != CLICK_NONE ? LLKeyboard::stringFromMouse(e.c, false) : std::string();
            item.data["command"] = command;

            if (!LLViewerInput::isKnownCommand(command))
            {
                item.note = "No such action here (" + command + ")";
                mItems.push_back(item);
                continue;
            }
            const KeyEntry* other = nullptr;
            bool same = false;
            for (const KeyEntry& o : ours[mode])
            {
                if (!sameCombo(o, e)) continue;
                if (o.command == command) same = true;
                else if (!other) other = &o;
            }
            if (same)
            {
                item.current = command;
                item.status = STATUS_SAME;
                item.apply = false;
            }
            else if (other)
            {
                item.current = other->command;
                item.data["replace"] = true;
                if (map.core.count(other->command) && bindingCount(ours[mode], other->command) <= 1)
                {
                    item.note = "Would leave " + other->command + " without a key here";
                }
                else
                {
                    item.status = STATUS_CONFLICT;   // shown, but off unless ticked
                    item.apply = false;
                    item.note = "Replaces this viewer's " + other->command + " on that key";
                }
            }
            else if (bindingCount(ours[mode], command) + planned[command] >= map.maxPerCommand)
            {
                item.current = "(free)";
                item.note = llformat("%s already has %d keys here", command.c_str(), map.maxPerCommand);
            }
            else
            {
                item.current = "(free)";
                item.status = STATUS_OFFER;
                item.apply = true;
                ++planned[command];
            }
            mItems.push_back(item);
        }
    }
}

void ImportPlan::buildContactSets()
{
    if (!mHasAccount) return;
    const fs::path src = mAccount.settings_dir / "settings_friends_groups.xml";
    std::error_code ec;
    if (!fs::exists(src, ec)) return;
    const LLSD theirs = readLLSDFile(src);
    if (!mLoggedIn)
    {
        PlanItem item = makeItem(CAT_CONTACT_SETS, STATUS_SKIP, "Contact sets");
        item.note = "Log in to import contact sets (they are kept per account)";
        mItems.push_back(item);
        return;
    }
    LGGContactSets* sets = LGGContactSets::getInstance();
    for (LLSD::map_const_iterator it = theirs.beginMap(); it != theirs.endMap(); ++it)
    {
        const std::string& name = it->first;
        if (sets->isInternalSetName(name) || !it->second.isMap()) continue;
        S32 members = 0, missing = 0;
        const LLSD& friends = it->second["friends"];
        for (LLSD::map_const_iterator f = friends.beginMap(); f != friends.endMap(); ++f)
        {
            ++members;
            if (!sets->isValidSet(name) || !sets->isFriendInSet(LLUUID(f->first), name)) ++missing;
        }
        PlanItem item = makeItem(CAT_CONTACT_SETS, STATUS_OFFER, name);
        item.imported = llformat("%d members", members);
        item.data["set"] = name;
        if (!sets->isValidSet(name))
        {
            item.current = "(no such set)";
            item.note = "New set";
        }
        else
        {
            item.current = "exists";
            if (missing == 0) item.status = STATUS_SAME;
            else item.note = llformat("Adds %d members", missing);
        }
        item.apply = item.status == STATUS_OFFER;
        mItems.push_back(item);
    }
    S32 new_aliases = 0;
    const LLSD& aliases = theirs[CS_SET_PSEUDONYM];
    for (LLSD::map_const_iterator it = aliases.beginMap(); it != aliases.endMap(); ++it)
    {
        if (!it->second.asString().empty() && !sets->hasPseudonym(LLUUID(it->first))) ++new_aliases;
    }
    if (aliases.size() > 0)
    {
        PlanItem item = makeItem(CAT_CONTACT_SETS, new_aliases ? STATUS_OFFER : STATUS_SAME, "Aliases (pseudonyms)");
        item.imported = llformat("%d", (S32)aliases.size());
        item.note = new_aliases ? llformat("Adds %d aliases; existing ones are kept", new_aliases) : std::string();
        item.data["pseudonyms"] = true;
        mItems.push_back(item);
    }
}

void ImportPlan::buildTeleportHistory()
{
    if (!mHasAccount) return;
    const fs::path src = mAccount.settings_dir / "teleport_history.txt";
    std::error_code ec;
    if (!fs::exists(src, ec)) return;
    const std::vector<LLSD> theirs = readTeleportHistory(src);
    if (theirs.empty()) return;
    PlanItem item = makeItem(CAT_TELEPORT_HISTORY, STATUS_SKIP, "Teleport history");
    item.imported = llformat("%d places", (S32)theirs.size());
    if (!mLoggedIn)
    {
        item.note = "Log in to import the teleport history (it is kept per account)";
        mItems.push_back(item);
        return;
    }
    const auto& ours = LLTeleportHistoryStorage::getInstance()->getItems();
    item.current = llformat("%d places", (S32)ours.size());
    S32 fresh = 0;
    for (const LLSD& t : theirs)
    {
        const LLTeleportHistoryPersistentItem candidate(t);
        bool have = false;
        for (const LLTeleportHistoryPersistentItem& o : ours)
        {
            if (o.mTitle == candidate.mTitle && dist_vec(o.mGlobalPos, candidate.mGlobalPos) < 1.0)
            {
                have = true;
                break;
            }
        }
        if (!have) ++fresh;
    }
    item.status = fresh ? STATUS_OFFER : STATUS_SAME;
    item.apply = fresh > 0;
    if (fresh) item.note = llformat("Adds %d places, by date; nothing is removed", fresh);
    item.data["path"] = utf8(src);
    mItems.push_back(item);
}

void ImportPlan::buildPresets()
{
    const ImportMap& map = ImportMap::instance();
    // A preset key is kept when it would be importable as a setting: graphics presets
    // only carry renderer settings with a Vulkan equivalent (render_allow).
    auto filter = [&](const LLSD& preset, bool graphics, S32& dropped) -> LLSD
    {
        LLSD out = LLSD::emptyMap();
        dropped = 0;
        for (LLSD::map_const_iterator it = preset.beginMap(); it != preset.endMap(); ++it)
        {
            std::string name = it->first;
            if (map.rename.has(name) && viewerMatches(map.rename[name]["viewers"], mViewer.id)
                && !map.rename[name]["convert"].isMap())
            {
                name = map.rename[name]["to"].asString();
            }
            std::string why;
            LLControlVariable* ctrl = gSavedSettings.getControl(name);
            LLSD value;
            const bool render = startsWith(name, "Render");
            if (!ctrl || map.denied(name, why) || (render && !map.renderAllow.count(name)) || (graphics && !render)
                || !it->second.isMap() || !typesCompatible(LLControlGroup::typeStringToEnum(it->second["Type"].asString()), ctrl->type())
                || !coerce(it->second["Value"], ctrl->type(), value))
            {
                ++dropped;
                continue;
            }
            std::string note;
            if (!validateValue(name, ctrl->type(), value, note, why))
            {
                ++dropped;
                continue;
            }
            out[name]["Comment"] = ctrl->getComment();
            out[name]["Persist"] = 1;
            out[name]["Type"] = LLControlGroup::typeEnumToString(ctrl->type());
            out[name]["Value"] = value;
        }
        return out;
    };

    auto offer = [&](ECategory cat, const std::string& subdir, const std::string& name, const LLSD& content, S32 dropped)
    {
        PlanItem item = makeItem(cat, STATUS_SKIP, name);
        item.imported = llformat("%d settings", (S32)content.size());
        if (content.size() == 0)
        {
            item.note = "Nothing in it applies to this viewer";
            mItems.push_back(item);
            return;
        }
        if (name == PRESETS_DEFAULT || LLPresetsManager::getInstance()->isDefaultCameraPreset(name)
            || LLPresetsManager::getInstance()->isTemplateCameraPreset(name))
        {
            item.note = "Built-in preset name here";
            mItems.push_back(item);
            return;
        }
        const fs::path dst = u8(LLPresetsManager::getPresetsDir(subdir)) / u8(LLURI::escape(name) + ".xml");
        std::error_code ec;
        if (fs::exists(dst, ec))
        {
            const LLSD existing = readLLSDFile(dst);
            bool same = existing.isMap() && existing.size() == content.size();
            for (LLSD::map_const_iterator it = content.beginMap(); same && it != content.endMap(); ++it)
            {
                same = existing.has(it->first) && valuesEqual(existing[it->first]["Value"], it->second["Value"]);
            }
            item.current = "exists";
            item.status = same ? STATUS_SAME : STATUS_SKIP;
            if (!same) item.note = "A different preset with this name exists here";
            mItems.push_back(item);
            return;
        }
        item.current = "(none)";
        item.status = STATUS_OFFER;
        item.apply = true;
        if (dropped) item.note = llformat("%d of their settings left out (not applicable here)", dropped);
        item.data["path"] = utf8(dst);
        item.data["content"] = content;
        item.data["subdir"] = subdir;
        mItems.push_back(item);
    };

    std::error_code ec;
    for (const char* sub : { "graphic", "camera" })
    {
        const bool graphics = std::string(sub) == "graphic";
        const fs::path dir = mViewer.dir / "user_settings" / "presets" / sub;
        if (!fs::is_directory(dir, ec)) continue;
        for (const fs::directory_entry& e : fs::directory_iterator(dir, ec))
        {
            std::error_code eec;
            if (!e.is_regular_file(eec) || e.path().extension() != ".xml") continue;
            const std::string name = LLURI::unescape(utf8(e.path().stem()));
            const LLSD preset = readLLSDFile(e.path());
            if (!preset.isMap()) continue;
            S32 dropped = 0;
            const LLSD content = filter(preset, graphics, dropped);
            offer(graphics ? CAT_GRAPHICS_PRESETS : CAT_CAMERA_PRESETS, graphics ? PRESETS_GRAPHIC : PRESETS_CAMERA, name, content, dropped);
        }
    }

    // Black Dragon camera presets: {camera_offset, focus_offset} -> an LL camera preset.
    if (mViewer.id == "blackdragon")
    {
        const fs::path dir = mViewer.dir / "user_settings" / "camera";
        if (fs::is_directory(dir, ec))
        {
            for (const fs::directory_entry& e : fs::directory_iterator(dir, ec))
            {
                std::error_code eec;
                if (!e.is_regular_file(eec) || e.path().extension() != ".xml") continue;
                std::string name = LLURI::unescape(utf8(e.path().stem()));
                const LLSD preset = readLLSDFile(e.path());
                LLSD camera, focus;
                if (!preset.isMap() || preset["hidden"].asBoolean()
                    || !numberArray(preset["camera_offset"], 3, false, camera) || !numberArray(preset["focus_offset"], 3, false, focus))
                {
                    continue;
                }
                // Black Dragon's built-in views share names with ours; keep both.
                if (LLPresetsManager::getInstance()->isDefaultCameraPreset(name)
                    || LLPresetsManager::getInstance()->isTemplateCameraPreset(name))
                {
                    name += " (Black Dragon)";
                }
                LLSD content;
                content["CameraOffsetRearView"]["Comment"] = "Initial camera offset from avatar in Rear View";
                content["CameraOffsetRearView"]["Persist"] = 1;
                content["CameraOffsetRearView"]["Type"] = "Vector3";
                content["CameraOffsetRearView"]["Value"] = camera;
                content["FocusOffsetRearView"]["Comment"] = "Initial focus point offset relative to avatar for the camera preset Rear View (x-axis is forward)";
                content["FocusOffsetRearView"]["Persist"] = 1;
                content["FocusOffsetRearView"]["Type"] = "Vector3D";
                content["FocusOffsetRearView"]["Value"] = focus;
                offer(CAT_CAMERA_PRESETS, PRESETS_CAMERA, name, content, 0);
            }
        }
    }
}

void ImportPlan::buildCopiedFiles(ECategory cat, const std::string& sub_dir, const std::vector<std::string>& extensions)
{
    const fs::path src_root = mViewer.dir / "user_settings" / sub_dir;
    std::error_code ec;
    if (!fs::is_directory(src_root, ec)) return;
    const fs::path dst_root = userSettingsDir() / sub_dir;
    for (fs::recursive_directory_iterator it(src_root, ec), end; !ec && it != end; it.increment(ec))
    {
        std::error_code eec;
        if (!it->is_regular_file(eec)) continue;
        const std::string ext = lower(utf8(it->path().extension()));
        if (std::find(extensions.begin(), extensions.end(), ext) == extensions.end()) continue;
        const fs::path rel = fs::relative(it->path(), src_root, eec);
        if (eec) continue;
        const fs::path dst = dst_root / rel;
        PlanItem item = makeItem(cat, STATUS_OFFER, utf8(rel));
        item.imported = llformat("%d KB", (S32)((it->file_size(eec) + 1023) / 1024));
        if (fs::exists(dst, eec))
        {
            item.current = "exists";
            item.status = sameFileContents(it->path(), dst) ? STATUS_SAME : STATUS_SKIP;
            if (item.status == STATUS_SKIP) item.note = "A different file with this name exists here";
            item.apply = false;
        }
        else
        {
            item.current = "(none)";
            item.apply = true;
            if (cat == CAT_WINDLIGHT) item.note = "Copied to your windlight folder; load it with Import in the environment editors";
        }
        item.data["src"] = utf8(it->path());
        item.data["dst"] = utf8(dst);
        mItems.push_back(item);
    }
}

void ImportPlan::buildAutoReplace()
{
    const fs::path src = mViewer.dir / "user_settings" / "autoreplace.xml";
    std::error_code ec;
    if (!fs::exists(src, ec)) return;
    const LLSD theirs = readLLSDFile(src);
    if (!theirs.isArray()) return;
    LLAutoReplaceSettings ours = LLAutoReplace::instance().getSettings();
    for (const LLSD& list : llsd::inArray(theirs))
    {
        if (!ours.listIsValid(list)) continue;
        const std::string name = list[AR_LIST_NAME].asString();
        const LLSD& words = list[AR_LIST_REPLACEMENTS];
        PlanItem item = makeItem(CAT_AUTOREPLACE, STATUS_OFFER, name);
        item.imported = llformat("%d words", (S32)words.size());
        item.data["list"] = list;
        if (ours.listNameIsUnique(list))
        {
            item.current = "(no such list)";
            item.note = "New list";
        }
        else
        {
            S32 missing = 0;
            for (LLSD::map_const_iterator it = words.beginMap(); it != words.endMap(); ++it)
            {
                if (ours.replacementFor(it->first, name).empty()) ++missing;
            }
            item.current = "exists";
            if (missing == 0) item.status = STATUS_SAME;
            else item.note = llformat("Adds %d words; existing replacements are kept", missing);
        }
        item.apply = item.status == STATUS_OFFER;
        mItems.push_back(item);
    }
}

void ImportPlan::buildSpelling()
{
    const fs::path src_dir = mViewer.dir / "user_settings" / "dictionaries";
    const fs::path dst_dir = u8(LLSpellChecker::getDictionaryUserPath());
    static const char* const files[2][2] = { { "user_custom.dic", "Your custom dictionary" }, { "user_ignore.dic", "Ignored words" } };
    for (const auto& f : files)
    {
        const std::vector<std::string> theirs = readDictionaryWords(src_dir / f[0]);
        if (theirs.empty()) continue;
        const std::vector<std::string> ours = readDictionaryWords(dst_dir / f[0]);
        const std::set<std::string> have(ours.begin(), ours.end());
        LLSD fresh = LLSD::emptyArray();
        for (const std::string& w : theirs)
        {
            if (!have.count(w)) fresh.append(w);
        }
        PlanItem item = makeItem(CAT_SPELLING, fresh.size() ? STATUS_OFFER : STATUS_SAME, f[1]);
        item.current = llformat("%d words", (S32)ours.size());
        item.imported = llformat("%d words", (S32)theirs.size());
        if (fresh.size()) item.note = llformat("Adds %d words", (S32)fresh.size());
        item.data["words"] = fresh;
        item.data["ignore"] = std::string(f[0]) == "user_ignore.dic";
        mItems.push_back(item);
    }
}

namespace
{
std::vector<fs::path> chatDirs(const SourceAccount& account)
{
    return account.chat_dirs.empty() ? std::vector<fs::path>{ account.chat_dir } : account.chat_dirs;
}

// "a.txt: reason; b.txt: reason (+N more, see the log)" -- file names and fixed reasons only.
std::string chatNotes(const Stats& st)
{
    std::string out;
    const size_t shown = std::min<size_t>(st.notes.size(), 3);
    for (size_t i = 0; i < shown; ++i) out += (i ? "; " : "") + st.notes[i];
    if (st.notes.size() > shown) out += llformat(" (+%d more, see the log)", (S32)(st.notes.size() - shown));
    return out;
}
}

void ImportPlan::buildChat()
{
    if (!mHasAccount) return;
    for (const std::string& line : mAccount.chat_search)
    {
        LL_INFOS("ViewerImport") << "Chat for " << mAccount.name << ": " << line << LL_ENDL;
    }
    PlanItem item = makeItem(CAT_CHAT, STATUS_SKIP, "Chat transcripts and conversation list");
    item.imported = llformat("%d transcripts", (S32)mAccount.transcript_count);
    std::string unreachable;
    if (!mAccount.chat_unreachable.empty())
    {
        // Relocated by InstantMessageLogPath, but missing or denied here (a sandbox hides it too).
        const std::string hint = sandboxHint(detectSandbox(), mAccount.chat_unreachable);
        unreachable = "Chat log folder " + mAccount.chat_unreachable + ": " + dirProblem(u8(mAccount.chat_unreachable))
                    + (hint.empty() ? std::string() : ". " + hint);
    }
    const std::vector<fs::path> sources = chatDirs(mAccount);
    if (mAccount.transcript_count == 0)
    {
        std::string where;
        for (const fs::path& dir : sources) where += (where.empty() ? "" : ", ") + utf8(dir);
        item.note = "No transcripts found in " + where + (unreachable.empty() ? "" : ". " + unreachable);
        LL_INFOS("ViewerImport") << "Chat for " << mAccount.name << ": " << item.note << LL_ENDL;
        mItems.push_back(item);
        return;
    }
    const std::string target = gDirUtilp->getPerAccountChatLogsDir();
    if (!mLoggedIn || target.empty())
    {
        item.note = "Log in to import chat transcripts";
        mItems.push_back(item);
        return;
    }
    const Stats st = importChatLogs(sources, u8(target), mViewer.legacy_im_names, true);
    const S32 not_imported = st.skipped + st.failed + st.not_transcript;
    LL_INFOS("ViewerImport") << "Chat preview for " << mAccount.name << ": " << st.copied << " new, " << st.merged << " to merge, "
                             << st.same << " already here, " << st.separate << " kept separately, " << not_imported
                             << " not imported" << LL_ENDL;
    for (const std::string& note : st.notes) LL_INFOS("ViewerImport") << "  " << note << LL_ENDL;

    item.current = utf8(u8(target).filename());
    item.note = llformat("%d new, %d to merge, %d already here", st.copied, st.merged, st.same);
    if (st.separate) item.note += llformat(", %d kept as \"(imported)\" copies", st.separate);
    if (not_imported) item.note += llformat(", %d not imported", not_imported);
    if (!st.notes.empty()) item.note += ". " + chatNotes(st);
    if (!unreachable.empty()) item.note += ". " + unreachable;
    const bool offer = st.copied + st.merged + st.separate > 0;
    item.status = offer ? STATUS_OFFER : (st.same || !not_imported) ? STATUS_SAME : STATUS_SKIP;
    item.apply = offer;
    mItems.push_back(item);
}

// ===========================================================================
// Apply

std::vector<std::string> ImportPlan::apply(bool& needs_restart)
{
    std::vector<std::string> report;
    needs_restart = false;
    std::vector<PlanItem*> rows[CAT_COUNT];
    S32 total = 0;
    for (PlanItem& item : mItems)
    {
        if (willApply(item))
        {
            rows[item.cat].push_back(&item);
            ++total;
        }
    }
    if (total == 0)
    {
        report.push_back("Nothing selected to import.");
        return report;
    }

    Backup backup;
    if (!backup.begin(mViewer.label))
    {
        report.push_back("Could not create a backup folder in " + utf8(userSettingsDir() / BACKUP_DIR) + "; nothing was imported.");
        return report;
    }
    backup.snapshotSettings(mLoggedIn);
    const ImportMap& map = ImportMap::instance();
    LL_INFOS("ViewerImport") << "Importing " << total << " items from " << mViewer.label << "; backup in " << utf8(backup.dir()) << LL_ENDL;

    // --- Settings, notifications, window positions
    for (ECategory cat : { CAT_SETTINGS, CAT_ACCOUNT, CAT_NOTIFICATIONS, CAT_LAYOUT })
    {
        S32 applied = 0;
        for (PlanItem* item : rows[cat])
        {
            const std::string group_name = item->data["group"].asString();
            if (group_name == "account" && !mLoggedIn) continue;
            LLControlGroup* group = groupNamed(group_name);
            const std::string name = item->data["name"].asString();
            LLControlVariable* ctrl = group->getControl(name);
            const bool existed = ctrl != nullptr;
            if (!ctrl && item->data.has("declare"))
            {
                const std::string floater = item->data["floater"].asString();
                ctrl = (item->data["declare"].asString() == "rect")
                    ? group->declareRect(name, LLRect(), "Window Size for " + floater, LLControlVariable::PERSIST_NONDFT)
                    : group->declareF32(name, 10.f, "Window Position for " + floater, LLControlVariable::PERSIST_NONDFT);
            }
            if (!ctrl) continue;
            backup.setting(group_name, name, existed, existed ? ctrl->getValue() : LLSD());
            ctrl->setValue(item->data["value"]);
            LL_INFOS("ViewerImport") << "  " << name << " = " << item->imported << LL_ENDL;
            if (map.restart.count(name) || cat == CAT_LAYOUT) needs_restart = true;
            item->status = STATUS_SAME;
            item->current = item->imported;
            ++applied;
        }
        if (applied) report.push_back(llformat("%s: %d applied", categoryKey(cat), applied));
    }
    saveSettingGroups(mLoggedIn);

    // --- Colours
    if (!rows[CAT_COLORS].empty())
    {
        LLUIColorTable& table = LLUIColorTable::instance();
        backup.file(userSettingsDir() / "colors.xml", "colors.xml", "");
        for (PlanItem* item : rows[CAT_COLORS])
        {
            const std::string name = item->data["name"].asString();
            LL_INFOS("ViewerImport") << "  color " << name << " = " << item->imported << LL_ENDL;
            backup.color(name, !table.isDefault(name), table.getColor(name).get());
            table.setColor(name, LLColor4(item->data["value"]));
            item->status = STATUS_SAME;
        }
        table.saveUserSettings();
        needs_restart = true;
        report.push_back(llformat("colors: %d applied", (S32)rows[CAT_COLORS].size()));
    }

    // --- Toolbars
    for (PlanItem* item : rows[CAT_TOOLBARS])
    {
        const fs::path dst = accountDir() / "toolbars.xml";
        backup.file(dst, "toolbars.xml", "toolbars");
        if (writeText(dst, item->data["xml"].asString()))
        {
            if (gToolBarView) gToolBarView->loadToolbars(false);
            LL_INFOS("ViewerImport") << "  toolbars: " << item->imported << (item->note.empty() ? std::string() : "; " + item->note) << LL_ENDL;
            item->status = STATUS_SAME;
            report.push_back("toolbars: replaced (" + item->imported + ")");
        }
        else
        {
            report.push_back("toolbars: could not write " + utf8(dst));
        }
    }

    // --- Key bindings (#232): merge, write, reload, verify; revert on any loss.
    if (!rows[CAT_KEYS].empty())
    {
        bool user_file = false;
        const fs::path base = ourKeyBindingsFile(user_file);
        KeyModes merged;
        std::array<bool, MODE_COUNT> present{};
        S32 version = 0;
        if (!readKeyBindingsXml(base, merged, present, version))
        {
            report.push_back("keys: could not read " + utf8(base) + "; key bindings left unchanged");
        }
        else
        {
            bool had_core[MODE_COUNT][32] = {};
            std::vector<std::string> core(map.core.begin(), map.core.end());
            core.resize(llmin((S32)core.size(), 32));
            for (S32 m = 0; m < MODE_COUNT; ++m)
            {
                for (size_t c = 0; c < core.size(); ++c) had_core[m][c] = gViewerInput.hasBinding(m, core[c]);
            }
            S32 added = 0, replaced = 0, refused = 0;
            std::vector<PlanItem*> done;
            for (PlanItem* item : rows[CAT_KEYS])
            {
                const S32 m = item->data["mode"].asInteger();
                if (m < 0 || m >= MODE_COUNT) continue;
                KeyEntry e;
                e.key = item->data["key"].asString();
                e.mask = item->data["mask"].asString();
                e.mouse = item->data["mouse"].asString();
                e.command = item->data["command"].asString();
                if (!finishEntry(e)) { ++refused; continue; }
                std::vector<KeyEntry>& list = merged[m];
                bool taken = false, blocked = false;
                for (const KeyEntry& o : list)
                {
                    if (!sameCombo(o, e)) continue;
                    if (o.command == e.command) { taken = true; break; }
                    if (!item->data["replace"].asBoolean()) { blocked = true; break; }
                    if (map.core.count(o.command) && bindingCount(list, o.command) <= 1) { blocked = true; break; }
                }
                if (taken) { item->status = STATUS_SAME; continue; }
                if (blocked || bindingCount(list, e.command) >= map.maxPerCommand)
                {
                    LL_INFOS("ViewerImport") << "  key " << item->name << " -> " << e.command << ": refused ("
                                             << (blocked ? "key in use here" : "no free slot") << ")" << LL_ENDL;
                    ++refused;
                    continue;
                }
                LL_INFOS("ViewerImport") << "  key " << item->name << " -> " << e.command
                                         << (item->data["replace"].asBoolean() ? " (replaces " + item->current + ")" : std::string()) << LL_ENDL;
                if (item->data["replace"].asBoolean())
                {
                    list.erase(std::remove_if(list.begin(), list.end(), [&](const KeyEntry& o) { return sameCombo(o, e); }), list.end());
                    ++replaced;
                }
                else
                {
                    ++added;
                }
                list.push_back(e);
                present[m] = true;
                done.push_back(item);
            }
            if (!done.empty())
            {
                const fs::path dst = u8(gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, "key_bindings.xml"));
                backup.file(dst, "key_bindings.xml", "keys");
                bool ok = writeText(dst, keepCommandsSection(dst, writeKeyBindingsXml(merged, present, version)));
                std::string lost;
                if (ok)
                {
                    ok = gViewerInput.loadBindingsXML(utf8(dst)) > 0;
                    for (S32 m = 0; ok && m < MODE_COUNT; ++m)
                    {
                        for (size_t c = 0; c < core.size(); ++c)
                        {
                            if (had_core[m][c] && !gViewerInput.hasBinding(m, core[c]))
                            {
                                lost = std::string(MODE_LABELS[m]) + " " + core[c];
                                ok = false;
                                break;
                            }
                        }
                    }
                }
                if (ok)
                {
                    for (PlanItem* item : done)
                    {
                        item->status = STATUS_SAME;
                        item->current = item->imported;
                    }
                    report.push_back(llformat("keys: %d added, %d replaced, %d refused (see the log); your other bindings are unchanged",
                                              added, replaced, refused));
                }
                else
                {
                    // Put the previous file back (or none, if there was none) and reload it.
                    const fs::path saved = backup.dir() / "key_bindings.xml";
                    std::error_code ec;
                    if (user_file) fs::copy_file(saved, dst, fs::copy_options::overwrite_existing, ec);
                    else fs::remove(dst, ec);
                    reloadAfterFileChange("keys");
                    report.push_back("keys: import reverted, nothing changed" + (lost.empty() ? std::string() : " (it would have unbound " + lost + ")"));
                    LL_WARNS("ViewerImport") << "Key binding import reverted" << (lost.empty() ? "" : ": lost " + lost) << LL_ENDL;
                }
            }
            else if (refused)
            {
                report.push_back(llformat("keys: %d refused (key already used, or the action has no free slot)", refused));
            }
        }
    }

    // --- Contact sets
    if (!rows[CAT_CONTACT_SETS].empty() && mLoggedIn)
    {
        backup.file(accountDir() / "settings_friends_groups.xml", "settings_friends_groups.xml", "contact_sets");
        std::vector<std::string> names;
        bool aliases = false;
        for (PlanItem* item : rows[CAT_CONTACT_SETS])
        {
            if (item->data["pseudonyms"].asBoolean()) aliases = true;
            else names.push_back(item->data["set"].asString());
            LL_INFOS("ViewerImport") << "  contact sets: " << item->name << LL_ENDL;
            item->status = STATUS_SAME;
        }
        const LLSD theirs = readLLSDFile(mAccount.settings_dir / "settings_friends_groups.xml");
        const S32 changes = LGGContactSets::getInstance()->mergeImported(theirs, names, aliases);
        report.push_back(llformat("contact sets: %d changes (sets, members and aliases added)", changes));
    }

    // --- Teleport history
    for (PlanItem* item : rows[CAT_TELEPORT_HISTORY])
    {
        if (!mLoggedIn) continue;
        LLTeleportHistoryStorage* storage = LLTeleportHistoryStorage::getInstance();
        storage->save();
        S32 added = 0;
        for (const LLSD& t : readTeleportHistory(u8(item->data["path"].asString())))
        {
            const LLTeleportHistoryPersistentItem candidate(t);
            bool have = false;
            for (const LLTeleportHistoryPersistentItem& o : storage->getItems())
            {
                if (o.mTitle == candidate.mTitle && dist_vec(o.mGlobalPos, candidate.mGlobalPos) < 1.0)
                {
                    have = true;
                    break;
                }
            }
            if (have) continue;
            storage->addItem(candidate.mTitle, candidate.mGlobalPos, candidate.mDate);
            LL_INFOS("ViewerImport") << "  teleport history: " << candidate.mTitle << LL_ENDL;
            backup.teleportAdded(candidate.mTitle, candidate.mGlobalPos);
            ++added;
        }
        storage->save();
        item->status = STATUS_SAME;
        report.push_back(llformat("teleport history: %d places added", added));
    }

    // --- Presets
    for (ECategory cat : { CAT_GRAPHICS_PRESETS, CAT_CAMERA_PRESETS })
    {
        S32 written = 0;
        for (PlanItem* item : rows[cat])
        {
            const fs::path dst = u8(item->data["path"].asString());
            if (writeLLSDFile(dst, item->data["content"]))
            {
                LL_INFOS("ViewerImport") << "  " << categoryKey(cat) << ": " << utf8(dst) << LL_ENDL;
                backup.created(dst);
                item->status = STATUS_SAME;
                ++written;
            }
        }
        if (written)
        {
            if (cat == CAT_GRAPHICS_PRESETS) LLPresetsManager::getInstance()->triggerChangeSignal();
            else LLPresetsManager::getInstance()->triggerChangeCameraSignal();
            report.push_back(llformat("%s: %d added", categoryKey(cat), written));
        }
    }

    // --- Plain file copies
    for (ECategory cat : { CAT_POSES, CAT_FONTS, CAT_WINDLIGHT })
    {
        S32 copied = 0;
        for (PlanItem* item : rows[cat])
        {
            const fs::path src = u8(item->data["src"].asString());
            const fs::path dst = u8(item->data["dst"].asString());
            std::error_code ec;
            fs::create_directories(dst.parent_path(), ec);
            if (fs::exists(dst, ec)) continue;   // never overwrite
            if (fs::copy_file(src, dst, ec) && !ec)
            {
                LL_INFOS("ViewerImport") << "  " << categoryKey(cat) << ": " << utf8(src) << " -> " << utf8(dst) << LL_ENDL;
                backup.created(dst);
                item->status = STATUS_SAME;
                ++copied;
            }
        }
        if (copied) report.push_back(llformat("%s: %d files copied", categoryKey(cat), copied));
    }

    // --- Autoreplace
    if (!rows[CAT_AUTOREPLACE].empty())
    {
        backup.file(userSettingsDir() / "autoreplace.xml", "autoreplace.xml", "autoreplace");
        LLAutoReplaceSettings settings = LLAutoReplace::instance().getSettings();
        S32 lists = 0, words = 0;
        for (PlanItem* item : rows[CAT_AUTOREPLACE])
        {
            const LLSD& list = item->data["list"];
            const std::string name = list[AR_LIST_NAME].asString();
            LL_INFOS("ViewerImport") << "  autoreplace list: " << name << LL_ENDL;
            if (settings.listNameIsUnique(list))
            {
                if (settings.addList(list) == LLAutoReplaceSettings::AddListOk) ++lists;
            }
            else
            {
                const LLSD& replacements = list[AR_LIST_REPLACEMENTS];
                for (LLSD::map_const_iterator it = replacements.beginMap(); it != replacements.endMap(); ++it)
                {
                    if (settings.replacementFor(it->first, name).empty()
                        && settings.addEntryToList(utf8str_to_wstring(it->first), utf8str_to_wstring(it->second.asString()), name))
                    {
                        ++words;
                    }
                }
            }
            item->status = STATUS_SAME;
        }
        LLAutoReplace::instance().setSettings(settings);
        report.push_back(llformat("autoreplace: %d lists added, %d words added to existing lists", lists, words));
    }

    // --- Spelling
    if (!rows[CAT_SPELLING].empty())
    {
        const fs::path dict = u8(LLSpellChecker::getDictionaryUserPath());
        backup.file(dict / "user_custom.dic", "user_custom.dic", "");
        backup.file(dict / "user_ignore.dic", "user_ignore.dic", "");
        S32 words = 0;
        for (PlanItem* item : rows[CAT_SPELLING])
        {
            const bool ignore = item->data["ignore"].asBoolean();
            for (const LLSD& w : llsd::inArray(item->data["words"]))
            {
                if (ignore) LLSpellChecker::instance().addToIgnoreList(w.asString());
                else LLSpellChecker::instance().addToCustomDictionary(w.asString());
                ++words;
            }
            item->status = STATUS_SAME;
        }
        report.push_back(llformat("spelling: %d words added", words));
    }

    // --- Chat transcripts (merged entry by entry; existing files keep a .pre-import copy)
    for (PlanItem* item : rows[CAT_CHAT])
    {
        const fs::path target = u8(gDirUtilp->getPerAccountChatLogsDir());
        // The conversation list is rewritten from memory at logout: save it now, merge on
        // disk, then read it back so the imported conversations survive.
        LLConversationLog::instance().cache();
        const Stats st = importChatLogs(chatDirs(mAccount), target, mViewer.legacy_im_names, false);
        LLConversationLog::instance().reloadFromDisk();
        LL_INFOS("ViewerImport") << "Imported chat from " << utf8(mAccount.chat_dir) << " into " << utf8(target) << ": "
                                 << st.copied << " new, " << st.merged << " updated, " << st.same << " unchanged, "
                                 << st.separate << " kept separately, " << st.skipped << " skipped, " << st.failed << " failed, "
                                 << st.not_transcript << " not transcripts" << LL_ENDL;
        // Every file and its decision (names only, never contents).
        for (const std::string& line : st.log) LL_INFOS("ViewerImport") << "  " << line << LL_ENDL;
        item->status = STATUS_SAME;
        report.push_back(llformat("chat: %d new transcripts, %d updated, %d kept as \"(imported)\" copies, %d left as is",
                                  st.copied, st.merged, st.separate, st.skipped + st.failed + st.not_transcript));
        if (!st.notes.empty()) report.push_back("chat: " + chatNotes(st));
    }

    if (!backup.commit())
    {
        report.push_back("Warning: could not write the undo journal in " + utf8(backup.dir()));
    }
    report.push_back("Backup and undo data: " + utf8(backup.dir()));
    for (const std::string& line : report) LL_INFOS("ViewerImport") << line << LL_ENDL;
    return report;
}

// ===========================================================================
// Undo

std::string lastUndoableImport()
{
    const fs::path base = userSettingsDir() / BACKUP_DIR;
    std::error_code ec;
    if (!fs::is_directory(base, ec)) return std::string();
    std::vector<std::string> names;
    for (const fs::directory_entry& e : fs::directory_iterator(base, ec))
    {
        std::error_code eec;
        if (e.is_directory(eec) && fs::exists(e.path() / JOURNAL_FILE, eec) && !fs::exists(e.path() / UNDONE_FILE, eec))
        {
            names.push_back(utf8(e.path().filename()));
        }
    }
    if (names.empty()) return std::string();
    std::sort(names.begin(), names.end());
    return names.back();
}

std::vector<std::string> undoLastImport(bool& needs_restart)
{
    std::vector<std::string> report;
    needs_restart = false;
    const std::string name = lastUndoableImport();
    if (name.empty())
    {
        report.push_back("There is no import to undo.");
        return report;
    }
    const fs::path dir = userSettingsDir() / BACKUP_DIR / u8(name);
    const LLSD journal = readLLSDFile(dir / JOURNAL_FILE);
    const bool same_account = journal["account_dir"].asString() == gDirUtilp->getLindenUserDir();

    // Settings, newest change last: undo in reverse.
    S32 settings = 0, skipped = 0;
    const LLSD& changes = journal["settings"];
    for (S32 i = (S32)changes.size() - 1; i >= 0; --i)
    {
        const LLSD& e = changes[i];
        const std::string group = e["group"].asString();
        if (group == "account" && (!same_account || gDirUtilp->getLindenUserDir().empty()))
        {
            ++skipped;
            continue;
        }
        LLControlVariable* ctrl = groupNamed(group)->getControl(e["name"].asString());
        if (!ctrl) continue;
        if (e["existed"].asBoolean()) ctrl->setValue(e["value"]);
        else ctrl->resetToDefault(true);
        ++settings;
    }
    saveSettingGroups(!gDirUtilp->getLindenUserDir().empty());

    // Colours.
    const LLSD& colors = journal["colors"];
    if (colors.size() > 0)
    {
        LLUIColorTable& table = LLUIColorTable::instance();
        for (const LLSD& e : llsd::inArray(colors))
        {
            if (e["user_set"].asBoolean()) table.setColor(e["name"].asString(), LLColor4(e["value"]));
            else table.resetToDefault(e["name"].asString());
        }
        table.saveUserSettings();
        needs_restart = true;
    }

    // Files we changed: put back what was there (or remove what wasn't).
    S32 files = 0;
    for (const LLSD& e : llsd::inArray(journal["files"]))
    {
        const fs::path live = u8(e["path"].asString());
        const std::string reload = e["reload"].asString();
        if (!same_account && (reload == "toolbars" || reload == "contact_sets"))
        {
            ++skipped;
            continue;
        }
        std::error_code ec;
        if (e["existed"].asBoolean()) fs::copy_file(dir / u8(e["tag"].asString()), live, fs::copy_options::overwrite_existing, ec);
        else fs::remove(live, ec);
        if (ec) continue;
        ++files;
        reloadAfterFileChange(reload);
        // Colours are re-read by widgets at restart; added dictionary words stay loaded until
        // then; an autoreplace / contact set file that did not exist before has nothing to reload.
        if (reload.empty() || !e["existed"].asBoolean()) needs_restart = true;
    }

    // Teleport history: take out the places the import added (the live list is saved at
    // logout, so restoring the file alone would not stick).
    if (journal["teleports"].size() > 0 && same_account && !gDirUtilp->getLindenUserDir().empty())
    {
        LLTeleportHistoryStorage* storage = LLTeleportHistoryStorage::getInstance();
        for (const LLSD& t : llsd::inArray(journal["teleports"]))
        {
            const LLVector3d pos(t["global_pos"]);
            const auto& items = storage->getItems();
            for (S32 i = (S32)items.size() - 1; i >= 0; --i)
            {
                if (items[i].mTitle == t["title"].asString() && dist_vec(items[i].mGlobalPos, pos) < 1.0)
                {
                    storage->removeItem(i);
                    break;
                }
            }
        }
        storage->save();
    }

    // Files we created.
    S32 removed = 0;
    for (const LLSD& e : llsd::inArray(journal["created"]))
    {
        std::error_code ec;
        if (fs::remove(u8(e.asString()), ec)) ++removed;
    }

    writeText(dir / UNDONE_FILE, "undone\n");
    report.push_back(llformat("Undid the import of %s: %d settings restored, %d files restored, %d added files removed",
                              journal["time"].asString().c_str(), settings, files, removed));
    if (skipped) report.push_back(llformat("%d per-account items were left alone: log in as the account that imported them.", skipped));
    LL_INFOS("ViewerImport") << "Undo of " << utf8(dir) << ": " << settings << " settings, " << files << " files, " << removed << " removed" << LL_ENDL;
    return report;
}
}
