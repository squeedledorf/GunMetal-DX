/**
 * @file slfxpresets.cpp
 * @brief ReShade-compatible effect presets (.ini) for the FX stack
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer Project.
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
#include "slfxpresets.h"

#include "llcallbacklist.h"
#include "lldir.h"
#include "llfile.h"
#include "llviewercontrol.h"
#include "slfxini.h"
#include "slfxstack.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <iomanip>
#include <locale>
#include <map>
#include <set>
#include <sstream>

namespace
{
const char* const SETTING_ACTIVE = "SLFxActivePreset";
const char* const SETTING_AUTOSAVE = "SLFxAutoSavePreset";
const char* const PRESET_EXTENSION = ".ini";
/// How long after the last edit auto-save writes: a drag is one write.
constexpr F32 AUTOSAVE_DELAY = 1.5f;

const char* const KEY_TECHNIQUES = "Techniques";
const char* const KEY_SORTING = "TechniqueSorting";
const char* const KEY_DEFINES = "PreprocessorDefinitions";

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

bool iequals(const std::string& a, const std::string& b)
{
    return lower(a) == lower(b);
}

std::string trim(const std::string& s)
{
    const size_t b = s.find_first_not_of(" \t");
    if (b == std::string::npos)
    {
        return std::string();
    }
    const size_t e = s.find_last_not_of(" \t");
    return s.substr(b, e - b + 1);
}

/// The techniques the runtime has, by "Technique@File.fx" (lowercased), and
/// by bare name for ReShade 4 entries (the first of each name).
struct TechniqueIndex
{
    std::vector<SLFxTechniqueInfo> mTechs;
    std::map<std::string, size_t> mById;
    std::map<std::string, size_t> mByName;

    explicit TechniqueIndex(std::vector<SLFxTechniqueInfo> techs)
    :   mTechs(std::move(techs))
    {
        for (size_t i = 0; i < mTechs.size(); ++i)
        {
            mById.emplace(lower(SLFxPresets::techniqueId(mTechs[i])), i);
            mByName.emplace(mTechs[i].mName, i);
        }
    }

    /// The technique a preset entry means, or -1.
    S32 find(const std::string& entry) const
    {
        const std::string e = trim(entry);
        if (e.find('@') != std::string::npos)
        {
            auto it = mById.find(lower(e));
            return it == mById.end() ? -1 : (S32)it->second;
        }
        auto it = mByName.find(e);
        return it == mByName.end() ? -1 : (S32)it->second;
    }
};

/// The new list for a Techniques= or TechniqueSorting= key: @a ours (ids of
/// techniques the runtime has, in order), with the entries of @a theirs that
/// name techniques not installed here put back after the entry they followed,
/// so a preset made with more effects keeps them.
std::vector<std::string> mergeList(const std::vector<std::string>& theirs, const std::vector<std::string>& ours,
                                   const TechniqueIndex& index)
{
    std::vector<std::string> result = ours;
    std::string prev_known; // the last of theirs that we know, as written in result
    size_t insert_front = 0;
    for (const std::string& entry : theirs)
    {
        const S32 found = index.find(entry);
        if (found >= 0)
        {
            prev_known = SLFxPresets::techniqueId(index.mTechs[found]);
            continue;
        }
        if (trim(entry).empty())
        {
            continue;
        }
        if (prev_known.empty())
        {
            result.insert(result.begin() + insert_front++, entry);
            continue;
        }
        auto it = std::find(result.begin(), result.end(), prev_known);
        if (it == result.end())
        {
            result.push_back(entry);
        }
        else
        {
            result.insert(it + 1, entry);
        }
        // Several unknown entries in a row stay in their order.
        prev_known = entry;
    }
    return result;
}

/// Whether a list key's text already says @a wanted, entry for entry (a
/// ReShade 4 bare name counts as the technique it resolves to).
bool listSays(const std::string* text, const std::vector<std::string>& wanted, const TechniqueIndex& index)
{
    if (!text)
    {
        return false;
    }
    const std::vector<std::string> have = SLFxIniDocument::splitList(*text);
    if (have.size() != wanted.size())
    {
        return false;
    }
    for (size_t i = 0; i < have.size(); ++i)
    {
        if (iequals(trim(have[i]), wanted[i]))
        {
            continue;
        }
        const S32 a = index.find(have[i]);
        const S32 b = index.find(wanted[i]);
        if (a < 0 || a != b)
        {
            return false;
        }
    }
    return true;
}

std::vector<LLSD> components(const LLSD& value, S32 count)
{
    std::vector<LLSD> out;
    if (value.isArray())
    {
        for (S32 i = 0; i < count; ++i)
        {
            out.push_back(i < (S32)value.size() ? value[i] : LLSD());
        }
    }
    else
    {
        out.push_back(value);
        for (S32 i = 1; i < count; ++i)
        {
            out.push_back(LLSD());
        }
    }
    return out;
}

bool parseNumber(const std::string& text, F64& out)
{
    const std::string t = trim(text);
    if (t.empty())
    {
        return false;
    }
    // Not strtod: a user's locale must not turn "0.5" into 0.
    std::istringstream in(t);
    in.imbue(std::locale::classic());
    in >> out;
    return !in.fail();
}
} // namespace

// ============================================================================
// SLFxPresetReport
// ============================================================================

bool SLFxPresetReport::hasWarnings() const
{
    return !mMissingTechniques.empty() || !mMissingEffects.empty() || !mUnknownUniforms.empty() ||
           !mBadValues.empty() || !mIgnoredDefinitions.empty();
}

std::string SLFxPresetReport::summary() const
{
    if (failed())
    {
        return mError;
    }
    std::vector<std::string> parts;
    auto count = [&parts](size_t n, const char* one, const char* many)
    {
        if (n == 1)
        {
            parts.push_back(one);
        }
        else if (n > 1)
        {
            parts.push_back(llformat(many, (S32)n));
        }
    };
    count(mMissingTechniques.size(), "1 technique not installed", "%d techniques not installed");
    count(mMissingEffects.size(), "1 effect not installed", "%d effects not installed");
    count(mUnknownUniforms.size(), "1 unknown setting", "%d unknown settings");
    count(mBadValues.size(), "1 unreadable value", "%d unreadable values");
    if (!mIgnoredDefinitions.empty())
    {
        parts.push_back("preprocessor definitions kept but not applied");
    }
    std::string out;
    for (const std::string& part : parts)
    {
        out += out.empty() ? part : "; " + part;
    }
    return out;
}

// ============================================================================
// SLFxPresets: value rules
// ============================================================================

// static
std::string SLFxPresets::effectFileName(const std::string& path)
{
    return gDirUtilp->getBaseFileName(path, false);
}

// static
std::string SLFxPresets::techniqueId(const SLFxTechniqueInfo& tech)
{
    return tech.mName + "@" + effectFileName(tech.mEffectPath);
}

// static
std::string SLFxPresets::techniqueKey(const SLFxTechniqueInfo& tech)
{
    // The runtime contract (slfxstack.h) takes a bare technique name. If it
    // grows a way to tell two "Sharpen"s in different files apart, this is
    // the one line to change.
    return tech.mName;
}

// static
SLFxPresets::EComponent SLFxPresets::componentKind(const SLFxUniformInfo& uniform)
{
    const std::string& type = uniform.mType;
    if (type == "bool")
    {
        return COMP_BOOL;
    }
    if (type.compare(0, 3, "int") == 0 || type.compare(0, 4, "uint") == 0)
    {
        return COMP_INT;
    }
    return COMP_FLOAT;
}

// static
S32 SLFxPresets::componentCount(const SLFxUniformInfo& uniform)
{
    // "float3", "color4", "int2": the trailing digit. A type we do not know
    // takes its shape from the default value.
    const std::string& type = uniform.mType;
    if (!type.empty() && type.back() >= '2' && type.back() <= '4')
    {
        return type.back() - '0';
    }
    if (uniform.mDefault.isArray())
    {
        return llmax(1, (S32)uniform.mDefault.size());
    }
    if (uniform.mValue.isArray())
    {
        return llmax(1, (S32)uniform.mValue.size());
    }
    return 1;
}

// static
std::string SLFxPresets::formatValue(const SLFxUniformInfo& uniform, const LLSD& value)
{
    const EComponent kind = componentKind(uniform);
    const S32 count = componentCount(uniform);
    std::ostringstream out;
    out.imbue(std::locale::classic());
    const std::vector<LLSD> comps = components(value, count);
    for (S32 i = 0; i < count; ++i)
    {
        if (i)
        {
            out << ',';
        }
        switch (kind)
        {
            case COMP_BOOL:
                out << (comps[i].asBoolean() ? 1 : 0);
                break;
            case COMP_INT:
                out << comps[i].asInteger();
                break;
            default:
                // std::to_string's "%f", which is what ReShade writes.
                out << std::fixed << std::setprecision(6) << (F32)comps[i].asReal();
                break;
        }
    }
    return out.str();
}

// static
bool SLFxPresets::parseValue(const SLFxUniformInfo& uniform, const std::string& text, LLSD& out)
{
    const EComponent kind = componentKind(uniform);
    const S32 count = componentCount(uniform);
    const std::vector<std::string> parts = SLFxIniDocument::splitList(text);
    const std::vector<LLSD> defaults = components(uniform.mDefault, count);

    std::vector<LLSD> comps;
    for (S32 i = 0; i < count; ++i)
    {
        if (i >= (S32)parts.size())
        {
            comps.push_back(defaults[i]);
            continue;
        }
        const std::string part = trim(parts[i]);
        if (kind == COMP_BOOL)
        {
            const std::string l = lower(part);
            if (l == "true")
            {
                comps.push_back(LLSD(true));
                continue;
            }
            if (l == "false")
            {
                comps.push_back(LLSD(false));
                continue;
            }
        }
        F64 number = 0.0;
        if (!parseNumber(part, number))
        {
            return false;
        }
        switch (kind)
        {
            case COMP_BOOL:
                comps.push_back(LLSD(number != 0.0));
                break;
            case COMP_INT:
                comps.push_back(LLSD((S32)llround((F32)number)));
                break;
            default:
                comps.push_back(LLSD(number));
                break;
        }
    }

    if (count == 1)
    {
        out = comps[0];
    }
    else
    {
        out = LLSD::emptyArray();
        for (const LLSD& c : comps)
        {
            out.append(c);
        }
    }
    return true;
}

// static
bool SLFxPresets::valuesEqual(const SLFxUniformInfo& uniform, const LLSD& a, const LLSD& b)
{
    return formatValue(uniform, a) == formatValue(uniform, b);
}

// ============================================================================
// SLFxPresets: files
// ============================================================================

SLFxPresets::SLFxPresets()
{
}

SLFxPresets::~SLFxPresets()
{
}

// static
std::string SLFxPresets::presetsDir()
{
    const std::string fx_dir = gDirUtilp->add(gDirUtilp->getOSUserAppDir(), "fx");
    const std::string dir = gDirUtilp->add(fx_dir, "presets");
    LLFile::mkdir(fx_dir);
    LLFile::mkdir(dir);
    return dir;
}

// static
std::string SLFxPresets::builtinPresetsDir()
{
    return gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "fx", "presets");
}

// static
bool SLFxPresets::isValidName(const std::string& name)
{
    const std::string t = trim(name);
    if (t.empty() || t != name || t == "." || t == "..")
    {
        return false;
    }
    // What LLDir would scrub, plus anything a shell or Windows dislikes.
    return LLDir::getScrubbedFileName(name) == name && name.find_first_of("/\\:*?\"<>|") == std::string::npos;
}

std::string SLFxPresets::pathFor(const std::string& name) const
{
    return gDirUtilp->add(presetsDir(), name + PRESET_EXTENSION);
}

std::string SLFxPresets::readPathFor(const std::string& name) const
{
    const std::string user = pathFor(name);
    if (LLFile::isfile(user))
    {
        return user;
    }
    const std::string builtin = gDirUtilp->add(builtinPresetsDir(), name + PRESET_EXTENSION);
    return LLFile::isfile(builtin) ? builtin : user;
}

bool SLFxPresets::isBuiltinOnly(const std::string& name) const
{
    return isValidName(name) && !LLFile::isfile(pathFor(name)) && readPathFor(name) != pathFor(name);
}

bool SLFxPresets::exists(const std::string& name) const
{
    return isValidName(name) && LLFile::isfile(readPathFor(name));
}

std::vector<std::string> SLFxPresets::listPresets() const
{
    std::vector<std::string> names;
    std::set<std::string> seen;
    const std::string ext = PRESET_EXTENSION;
    // The user's folder first: a name it has hides the shipped preset of that name.
    for (const std::string& dir : {presetsDir(), builtinPresetsDir()})
    {
        for (const std::string& file : gDirUtilp->getFilesInDir(dir))
        {
            if (file.size() > ext.size() && iequals(file.substr(file.size() - ext.size()), ext))
            {
                const std::string name = file.substr(0, file.size() - ext.size());
                if (seen.insert(lower(name)).second)
                {
                    names.push_back(name);
                }
            }
        }
    }
    std::sort(names.begin(), names.end(),
              [](const std::string& a, const std::string& b) { return lower(a) < lower(b); });
    return names;
}

std::string SLFxPresets::getActive() const
{
    return gSavedSettings.getString(SETTING_ACTIVE);
}

bool SLFxPresets::readDocument(const std::string& path, SLFxIniDocument& doc, std::string& error) const
{
    if (!LLFile::isfile(path))
    {
        error = "Preset file not found: " + path;
        return false;
    }
    doc.parse(LLFile::getContents(path));
    return true;
}

bool SLFxPresets::writeDocument(const std::string& path, const SLFxIniDocument& doc, std::string& error) const
{
    // Written beside and swapped in, so a crash mid-write never leaves half
    // a preset.
    const std::string text = doc.write();
    const std::string temp = path + ".tmp";
    LLFILE* fp = LLFile::fopen(temp, "wb");
    if (!fp)
    {
        error = "Could not write " + path;
        return false;
    }
    const size_t written = text.empty() ? 0 : fwrite(text.data(), 1, text.size(), fp);
    LLFile::close(fp);
    if (written != text.size() || LLFile::replace(temp, path) != 0)
    {
        LLFile::remove(temp, ENOENT);
        error = "Could not write " + path;
        return false;
    }
    return true;
}

// ============================================================================
// SLFxPresets: applying and capturing
// ============================================================================

void SLFxPresets::applyDocument(const SLFxIniDocument& doc, SLFxPresetReport& report)
{
    SLFxStack& stack = SLFxStack::instance();
    const TechniqueIndex index(stack.getTechniques());

    // --- Techniques: order, then which are on ---
    const std::string* techniques_text = doc.get("", KEY_TECHNIQUES);
    const std::string* sorting_text = doc.get("", KEY_SORTING);
    const std::vector<std::string> enabled_entries =
        techniques_text ? SLFxIniDocument::splitList(*techniques_text) : std::vector<std::string>();
    const std::vector<std::string> sorting_entries =
        sorting_text ? SLFxIniDocument::splitList(*sorting_text) : enabled_entries;

    std::vector<bool> placed(index.mTechs.size(), false);
    std::vector<std::string> order;
    for (const std::string& entry : sorting_entries)
    {
        const S32 found = index.find(entry);
        if (found >= 0 && !placed[found])
        {
            placed[found] = true;
            order.push_back(techniqueKey(index.mTechs[found]));
        }
    }
    // Enabled ones the sorting forgot, then everything else where it was:
    // ReShade appends what its sorting does not list.
    for (const std::string& entry : enabled_entries)
    {
        const S32 found = index.find(entry);
        if (found >= 0 && !placed[found])
        {
            placed[found] = true;
            order.push_back(techniqueKey(index.mTechs[found]));
        }
    }
    for (size_t i = 0; i < index.mTechs.size(); ++i)
    {
        if (!placed[i])
        {
            order.push_back(techniqueKey(index.mTechs[i]));
        }
    }
    if (!order.empty())
    {
        stack.setTechniqueOrder(order);
    }

    std::vector<bool> enable(index.mTechs.size(), false);
    for (const std::string& entry : enabled_entries)
    {
        if (trim(entry).empty())
        {
            continue;
        }
        const S32 found = index.find(entry);
        if (found < 0)
        {
            report.mMissingTechniques.push_back(trim(entry));
            continue;
        }
        enable[found] = true;
    }
    for (size_t i = 0; i < index.mTechs.size(); ++i)
    {
        if (index.mTechs[i].mEnabled != enable[i])
        {
            stack.setTechniqueEnabled(techniqueKey(index.mTechs[i]), enable[i]);
        }
    }

    // --- Definitions: kept, not applied (no runtime call for them yet) ---
    if (const std::string* defines = doc.get("", KEY_DEFINES))
    {
        if (!trim(*defines).empty())
        {
            report.mIgnoredDefinitions.push_back(*defines);
        }
    }

    // --- Uniforms: what the file says, everything else back to default ---
    std::set<std::string> matched_sections;
    for (const SLFxEffectInfo& effect : stack.getAvailableEffects())
    {
        if (!effect.mLoaded)
        {
            continue;
        }
        const std::string section = effectFileName(effect.mPath);
        const bool has_section = doc.hasSection(section);
        if (has_section)
        {
            matched_sections.insert(lower(section));
            if (const std::string* defines = doc.get(section, KEY_DEFINES))
            {
                if (!trim(*defines).empty())
                {
                    report.mIgnoredDefinitions.push_back(section + ": " + *defines);
                }
            }
        }

        const std::vector<SLFxUniformInfo> uniforms = stack.getUniforms(effect.mPath);
        std::set<std::string> known;
        for (const SLFxUniformInfo& uniform : uniforms)
        {
            known.insert(uniform.mName);
            LLSD wanted = uniform.mDefault;
            if (const std::string* text = has_section ? doc.get(section, uniform.mName) : nullptr)
            {
                LLSD parsed;
                if (parseValue(uniform, *text, parsed))
                {
                    wanted = parsed;
                }
                else
                {
                    report.mBadValues.push_back(section + ": " + uniform.mName);
                }
            }
            // Only what changes: a big pack has thousands of uniforms.
            if (!valuesEqual(uniform, wanted, uniform.mValue))
            {
                stack.setUniform(effect.mPath, uniform.mName, wanted);
            }
        }
        if (has_section)
        {
            for (const auto& entry : doc.entries(section))
            {
                if (entry.first != KEY_DEFINES && !known.count(entry.first))
                {
                    report.mUnknownUniforms.push_back(section + ": " + entry.first);
                }
            }
        }
    }
    for (const std::string& section : doc.sectionNames())
    {
        if (!section.empty() && !matched_sections.count(lower(section)))
        {
            report.mMissingEffects.push_back(section);
        }
    }
}

void SLFxPresets::captureInto(SLFxIniDocument& doc) const
{
    SLFxStack& stack = SLFxStack::instance();
    const TechniqueIndex index(stack.getTechniques());

    if (doc.sectionNames().empty())
    {
        // A new file: ReShade always writes this, empty or not. An existing
        // file without it (ReShade 4) is left without it.
        doc.set("", KEY_DEFINES, "");
    }

    std::vector<std::string> all_ids;
    std::vector<std::string> enabled_ids;
    std::set<std::string> enabled_effects;
    for (const SLFxTechniqueInfo& tech : index.mTechs)
    {
        all_ids.push_back(techniqueId(tech));
        if (tech.mEnabled)
        {
            enabled_ids.push_back(techniqueId(tech));
            enabled_effects.insert(lower(effectFileName(tech.mEffectPath)));
        }
    }

    const std::string* techniques_text = doc.get("", KEY_TECHNIQUES);
    const std::vector<std::string> enabled_list = mergeList(
        techniques_text ? SLFxIniDocument::splitList(*techniques_text) : std::vector<std::string>(), enabled_ids, index);
    if (!listSays(techniques_text, enabled_list, index))
    {
        doc.set("", KEY_TECHNIQUES, SLFxIniDocument::joinList(enabled_list));
    }

    const std::string* sorting_text = doc.get("", KEY_SORTING);
    const std::vector<std::string> sorting_list = mergeList(
        sorting_text ? SLFxIniDocument::splitList(*sorting_text) : std::vector<std::string>(), all_ids, index);
    if (!listSays(sorting_text, sorting_list, index))
    {
        doc.set("", KEY_SORTING, SLFxIniDocument::joinList(sorting_list));
    }

    for (const SLFxEffectInfo& effect : stack.getAvailableEffects())
    {
        if (!effect.mLoaded)
        {
            continue;
        }
        const std::string section = effectFileName(effect.mPath);
        const bool has_section = doc.hasSection(section);
        const bool enabled = enabled_effects.count(lower(section)) != 0;
        if (!has_section && !enabled)
        {
            // An effect that is off and that the preset never mentioned: its
            // values are whatever loading reset them to.
            continue;
        }
        for (const SLFxUniformInfo& uniform : stack.getUniforms(effect.mPath))
        {
            const std::string* text = doc.get(section, uniform.mName);
            if (text)
            {
                LLSD written;
                if (parseValue(uniform, *text, written) && valuesEqual(uniform, written, uniform.mValue))
                {
                    continue; // says the same: leave the bytes alone
                }
                doc.set(section, uniform.mName, formatValue(uniform, uniform.mValue));
            }
            else if (!has_section || !valuesEqual(uniform, uniform.mValue, uniform.mDefault))
            {
                // A new section gets every value, as ReShade writes one; an
                // existing one only what loading it would not reproduce (a
                // missing key loads as the default).
                doc.set(section, uniform.mName, formatValue(uniform, uniform.mValue));
            }
        }
    }
}

// ============================================================================
// SLFxPresets: operations
// ============================================================================

void SLFxPresets::setActiveQuietly(const std::string& name)
{
    if (gSavedSettings.getString(SETTING_ACTIVE) == name)
    {
        return;
    }
    mSettingActive = true;
    gSavedSettings.setString(SETTING_ACTIVE, name);
    mSettingActive = false;
}

bool SLFxPresets::load(const std::string& name)
{
    mReport.clear();
    if (!isValidName(name))
    {
        mReport.mError = "Not a usable preset name: " + name;
        notifyChanged();
        return false;
    }
    SLFxIniDocument doc;
    if (!readDocument(readPathFor(name), doc, mReport.mError))
    {
        notifyChanged();
        return false;
    }

    if (SLFxStack::instance().getTechniques().empty())
    {
        // The runtime has not scanned yet (FX is off, or still starting):
        // remember the choice and apply it when the effects arrive.
        setActiveQuietly(name);
        mRestored = false;
        mDirty = false;
        ++mSaveGeneration;
        notifyChanged();
        return true;
    }

    mRestored = true; // a load is a restore; none must follow it
    mApplying = true;
    applyDocument(doc, mReport);
    mApplying = false;

    setActiveQuietly(name);
    mDirty = false;
    ++mSaveGeneration; // cancels an auto-save of the preset before
    if (mReport.hasWarnings())
    {
        LL_INFOS("FX") << "Preset " << name << ": " << mReport.summary() << LL_ENDL;
    }
    notifyChanged();
    return true;
}

bool SLFxPresets::writePreset(const std::string& name)
{
    mReport.clear();
    if (!isValidName(name))
    {
        mReport.mError = "Not a usable preset name: " + name;
        return false;
    }
    const std::string path = pathFor(name);
    SLFxIniDocument doc;
    const std::string from = readPathFor(name); // a shipped preset is the base of the user's copy
    if (LLFile::isfile(from))
    {
        std::string ignored;
        readDocument(from, doc, ignored);
    }
    else
    {
        doc.parse(std::string());
    }
    captureInto(doc);
    return writeDocument(path, doc, mReport.mError);
}

bool SLFxPresets::save(const std::string& name)
{
    if (!writePreset(name))
    {
        notifyChanged();
        return false;
    }
    setActiveQuietly(name);
    mDirty = false;
    ++mSaveGeneration;
    notifyChanged();
    return true;
}

bool SLFxPresets::remove(const std::string& name)
{
    mReport.clear();
    if (!exists(name))
    {
        return false;
    }
    if (isBuiltinOnly(name))
    {
        mReport.mError = "'" + name + "' ships with the viewer and cannot be deleted.";
        notifyChanged();
        return false;
    }
    if (LLFile::remove(pathFor(name)) != 0)
    {
        mReport.mError = "Could not delete " + pathFor(name);
        notifyChanged();
        return false;
    }
    if (name == getActive())
    {
        // Nothing on disk now matches what is running.
        mDirty = true;
    }
    notifyChanged();
    return true;
}

bool SLFxPresets::importFile(const std::string& source_path, std::string& out_name)
{
    mReport.clear();
    std::string base = gDirUtilp->getBaseFileName(source_path, true);
    base = LLDir::getScrubbedFileName(base);
    for (char& c : base)
    {
        if (std::string("/\\:*?\"<>|").find(c) != std::string::npos)
        {
            c = '_';
        }
    }
    base = trim(base);
    if (base.empty())
    {
        base = "Imported";
    }
    std::string name = base;
    for (S32 n = 2; exists(name); ++n)
    {
        name = llformat("%s (%d)", base.c_str(), n);
    }

    SLFxIniDocument doc;
    if (!readDocument(source_path, doc, mReport.mError))
    {
        notifyChanged();
        return false;
    }
    // Copied as is, not re-written: the copy is the user's file.
    if (!LLFile::copy(source_path, pathFor(name)))
    {
        mReport.mError = "Could not copy " + source_path + " into " + presetsDir();
        notifyChanged();
        return false;
    }
    out_name = name;
    // Unsaved edits go to the preset being left first, as with any switch.
    if (mDirty && mRestored && gSavedSettings.getBOOL(SETTING_AUTOSAVE) && isValidName(getActive()))
    {
        writePreset(getActive());
    }
    return load(name);
}

bool SLFxPresets::exportTo(const std::string& dest_path)
{
    mReport.clear();
    SLFxIniDocument doc;
    const std::string active = getActive();
    if (exists(active))
    {
        std::string ignored;
        readDocument(readPathFor(active), doc, ignored);
    }
    else
    {
        doc.parse(std::string());
    }
    captureInto(doc);
    const bool ok = writeDocument(dest_path, doc, mReport.mError);
    notifyChanged();
    return ok;
}

void SLFxPresets::markDirty()
{
    if (mApplying)
    {
        return;
    }
    const bool was = mDirty;
    mDirty = true;
    scheduleAutoSave();
    if (!was)
    {
        notifyChanged();
    }
}

void SLFxPresets::scheduleAutoSave()
{
    if (!gSavedSettings.getBOOL(SETTING_AUTOSAVE) || !isValidName(getActive()))
    {
        return;
    }
    // Restoring must have happened first, or the save would write the
    // runtime's defaults over the preset it was about to load.
    if (!mRestored)
    {
        return;
    }
    const U32 generation = ++mSaveGeneration;
    doAfterInterval([generation]()
    {
        if (!SLFxPresets::instanceExists())
        {
            return;
        }
        SLFxPresets& self = SLFxPresets::instance();
        if (self.mSaveGeneration == generation && self.mDirty)
        {
            self.saveActive();
        }
    }, AUTOSAVE_DELAY);
}

boost::signals2::connection SLFxPresets::setChangedCallback(const std::function<void()>& cb)
{
    return mChangedSignal.connect(cb);
}

void SLFxPresets::notifyChanged()
{
    mChangedSignal();
}

// ============================================================================
// SLFxPresets: wiring
// ============================================================================

void SLFxPresets::init()
{
    if (mInitialized)
    {
        tryRestore();
        return;
    }
    mInitialized = true;

    if (LLControlVariable* controlp = gSavedSettings.getControl(SETTING_ACTIVE))
    {
        mActiveConnection = controlp->getSignal()->connect(
            [this](LLControlVariable*, const LLSD& new_value, const LLSD& old_value)
            { onActiveSettingChanged(old_value, new_value); });
    }
    mRuntimeConnection = SLFxStack::instance().setChangedCallback([this]() { onRuntimeChanged(); });
    tryRestore();
}

void SLFxPresets::onActiveSettingChanged(const LLSD& old_value, const LLSD& new_value)
{
    if (mSettingActive)
    {
        return;
    }
    // Someone else picked a preset: Preferences, or its Cancel. Unsaved
    // edits go to the one being left, if auto-save says they should.
    const std::string old_name = old_value.asString();
    if (mDirty && mRestored && gSavedSettings.getBOOL(SETTING_AUTOSAVE) && isValidName(old_name))
    {
        // Written without touching the setting, which is mid-change.
        writePreset(old_name);
    }
    const std::string name = new_value.asString();
    if (exists(name))
    {
        load(name);
    }
    else
    {
        // A name with no file yet: the next save creates it.
        mDirty = true;
        notifyChanged();
    }
}

void SLFxPresets::onRuntimeChanged()
{
    if (!mApplying)
    {
        tryRestore();
    }
}

void SLFxPresets::tryRestore()
{
    if (mRestored || mApplying)
    {
        return;
    }
    // Not before the runtime has effects to restore into: when FX starts
    // off, that is when it is first switched on and has scanned.
    if (!SLFxStack::instanceExists() || SLFxStack::instance().getTechniques().empty())
    {
        return;
    }
    const std::string name = getActive();
    if (exists(name))
    {
        load(name);
    }
    else
    {
        mRestored = true;
    }
}
