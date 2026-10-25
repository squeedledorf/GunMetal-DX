/**
 * @file slviewerpresets.cpp
 * @brief Viewer presets (#132). See the header.
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

#include "slviewerpresets.h"

#include "llcontrol.h"
#include "lldate.h"
#include "llfile.h"
#include "llkeyboard.h"
#include "llkeyconflict.h"
#include "llsdserialize.h"
#include "llsdutil.h"
#include "llviewercontrol.h"
#include "llviewerinput.h"

#include <map>
#include <set>

namespace
{
const char* const BACKUP_SETTING = "FirstRunPresetBackup";

// Key binding modes a preset's bindings apply to (key_bindings.xml sections).
struct BindingMode
{
    const char* name;
    LLKeyConflictHandler::ESourceMode mode;
};
const BindingMode BINDING_MODES[] = {
    { "first_person", LLKeyConflictHandler::MODE_FIRST_PERSON },
    { "third_person", LLKeyConflictHandler::MODE_THIRD_PERSON },
    { "sitting",      LLKeyConflictHandler::MODE_SITTING },
    { "edit_avatar",  LLKeyConflictHandler::MODE_EDIT_AVATAR },
};
// A command holds a few key combinations; the preferences show 3, files may hold up to 6.
constexpr U32 MAX_BINDING_SLOTS = 8;

struct Input
{
    std::string     id;         // "MMB||NONE": mouse|key|mask as written in the preset
    LLSD            desc;       // {mouse, key, mask}, kept in the backup
    EMouseClickType mouse = CLICK_NONE;
    KEY             key = KEY_NONE;
    MASK            mask = MASK_NONE;
};

struct Binding
{
    Input                    input;
    std::string              command;
    std::vector<std::string> instead_of;
};

struct Preset
{
    bool                  ok = false;
    LLSD                  settings;     // name -> value
    std::vector<Binding>  keys;
    std::set<std::string> restart;
};

bool parseInput(const LLSD& desc, Input& out)
{
    const std::string mouse = desc["mouse"].asString();
    const std::string key   = desc["key"].asString();
    const std::string mask  = desc["mask"].asString().empty() ? std::string("NONE") : desc["mask"].asString();
    out = Input();
    if (!mouse.empty() && !LLViewerInput::mouseFromString(mouse, &out.mouse)) return false;
    if (!key.empty() && !LLKeyboard::keyFromString(key, &out.key)) return false;
    if (!LLKeyboard::maskFromString(mask, &out.mask)) return false;
    if (out.mouse == CLICK_NONE && out.key == KEY_NONE) return false;
    out.id = mouse + "|" + key + "|" + mask;
    out.desc = LLSD::emptyMap();
    out.desc["mouse"] = mouse;
    out.desc["key"] = key;
    out.desc["mask"] = mask;
    return true;
}

Preset loadPreset(const std::string& id)
{
    Preset preset;
    const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "viewer_presets", id + ".xml");
    llifstream in(path.c_str());
    LLSD sd;
    if (!in.is_open() || LLSDSerialize::fromXML(sd, in) == LLSDParser::PARSE_FAILURE || !sd.isMap())
    {
        LL_WARNS("ViewerPresets") << "Could not read viewer preset " << path << LL_ENDL;
        return preset;
    }
    preset.ok = true;
    preset.settings = sd["settings"].isMap() ? sd["settings"] : LLSD::emptyMap();
    for (LLSD::array_const_iterator it = sd["restart"].beginArray(); it != sd["restart"].endArray(); ++it)
    {
        preset.restart.insert(it->asString());
    }
    for (LLSD::array_const_iterator it = sd["key_bindings"].beginArray(); it != sd["key_bindings"].endArray(); ++it)
    {
        Binding binding;
        binding.command = (*it)["command"].asString();
        if (binding.command.empty() || !parseInput(*it, binding.input))
        {
            LL_WARNS("ViewerPresets") << id << ": skipping key binding with an unknown command or input (command '"
                                      << binding.command << "', mouse '" << (*it)["mouse"].asString() << "', key '"
                                      << (*it)["key"].asString() << "')" << LL_ENDL;
            continue;
        }
        const LLSD& instead = (*it)["instead_of"];
        for (LLSD::array_const_iterator c = instead.beginArray(); c != instead.endArray(); ++c)
        {
            binding.instead_of.push_back(c->asString());
        }
        preset.keys.push_back(binding);
    }
    return preset;
}

// Everything any preset touches: setting names, inputs, and the commands each input moves between.
struct Universe
{
    std::set<std::string>                        settings;
    std::map<std::string, Input>                 inputs;
    std::map<std::string, std::set<std::string>> commands;     // input id -> commands involved
    std::set<std::string>                        restart;
};

Universe loadUniverse()
{
    Universe u;
    for (const std::string& id : slpresets::presetIds())
    {
        const Preset preset = loadPreset(id);
        for (LLSD::map_const_iterator it = preset.settings.beginMap(); it != preset.settings.endMap(); ++it)
        {
            u.settings.insert(it->first);
        }
        for (const Binding& binding : preset.keys)
        {
            u.inputs[binding.input.id] = binding.input;
            std::set<std::string>& commands = u.commands[binding.input.id];
            commands.insert(binding.command);
            commands.insert(binding.instead_of.begin(), binding.instead_of.end());
        }
        u.restart.insert(preset.restart.begin(), preset.restart.end());
    }
    return u;
}

// A preset value converted to the setting's type (false: wrong kind of value for it).
bool coerce(const LLControlVariable& control, const LLSD& value, LLSD& out)
{
    switch (control.type())
    {
        case TYPE_BOOLEAN:
            if (!value.isBoolean() && !value.isInteger()) return false;
            out = LLSD(value.asBoolean());
            return true;
        case TYPE_S32:
        case TYPE_U32:
            if (!value.isInteger() && !value.isBoolean() && !value.isReal()) return false;
            out = LLSD(value.asInteger());
            return true;
        case TYPE_F32:
            if (!value.isReal() && !value.isInteger()) return false;
            out = LLSD(value.asReal());
            return true;
        case TYPE_STRING:
            if (!value.isString()) return false;
            out = LLSD(value.asString());
            return true;
        default:
            out = value;
            return true;
    }
}

// Set a setting if it differs; returns whether it changed.
bool setControl(LLControlVariable& control, const LLSD& value)
{
    LLSD current;
    if (!coerce(control, control.getValue(), current)) current = control.getValue();
    if (llsd_equals(current, value)) return false;
    control.setValue(value);
    return true;
}

bool sameInput(const LLKeyData& data, const Input& input)
{
    return data.mMouse == input.mouse && data.mKey == input.key && data.mMask == input.mask;
}

std::set<std::string> boundCommands(LLKeyConflictHandler& handler, const Input& input, const std::set<std::string>& commands)
{
    std::set<std::string> bound;
    for (const std::string& command : commands)
    {
        for (U32 i = 0; i < MAX_BINDING_SLOTS; ++i)
        {
            if (sameInput(handler.getControl(command, i), input))
            {
                bound.insert(command);
                break;
            }
        }
    }
    return bound;
}

// Make exactly the commands in `want` (out of `commands`) use the input. Returns whether
// anything changed. Other commands' bindings are left alone.
bool bindExactly(LLKeyConflictHandler& handler, const Input& input, const std::set<std::string>& commands,
                 const std::set<std::string>& want)
{
    bool changed = false;
    for (const std::string& command : commands)
    {
        if (want.count(command)) continue;
        for (U32 i = 0; i < MAX_BINDING_SLOTS; ++i)
        {
            if (sameInput(handler.getControl(command, i), input))
            {
                changed |= handler.clearControl(command, i);
            }
        }
    }
    for (const std::string& command : want)
    {
        bool has = false;
        S32 free_slot = -1;
        for (U32 i = 0; i < MAX_BINDING_SLOTS && !has; ++i)
        {
            const LLKeyData data = handler.getControl(command, i);
            has = sameInput(data, input);
            if (free_slot < 0 && data.isEmpty()) free_slot = (S32)i;
        }
        if (has) continue;
        if (free_slot >= 0 && handler.registerControl(command, (U32)free_slot, input.mouse, input.key, input.mask, true))
        {
            changed = true;
        }
        else
        {
            LL_WARNS("ViewerPresets") << "Could not bind " << input.id << " to " << command << LL_ENDL;
        }
    }
    return changed;
}

std::set<std::string> asSet(const LLSD& array)
{
    std::set<std::string> out;
    for (LLSD::array_const_iterator it = array.beginArray(); it != array.endArray(); ++it)
    {
        out.insert(it->asString());
    }
    return out;
}

LLSD asArray(const std::set<std::string>& values)
{
    LLSD out = LLSD::emptyArray();
    for (const std::string& v : values) out.append(v);
    return out;
}

void saveSettings()
{
    gSavedSettings.saveToFile(gSavedSettings.getString("ClientSettingsFile"), true);
}
}

namespace slpresets
{
const std::vector<std::string>& presetIds()
{
    static const std::vector<std::string> ids = { "firestorm", "alchemy", "blackdragon", "secondlife", "slipstream" };
    return ids;
}

bool hasBackup()
{
    const LLSD backup = gSavedSettings.getLLSD(BACKUP_SETTING);
    return backup.isMap() && (backup["settings"].size() > 0 || backup["keys"].size() > 0);
}

bool apply(const std::string& id, Result& result)
{
    result = Result();
    const Preset preset = loadPreset(id);
    if (!preset.ok) return false;
    const Preset defaults = (id == defaultsId()) ? preset : loadPreset(defaultsId());
    const Universe u = loadUniverse();

    LLSD backup = gSavedSettings.getLLSD(BACKUP_SETTING);
    if (!backup.isMap()) backup = LLSD::emptyMap();
    if (!backup.has("when")) backup["when"] = LLDate::now().asString();

    // Back up every setting not backed up yet (a newer release may name more of them).
    for (const std::string& name : u.settings)
    {
        LLControlVariablePtr control = gSavedSettings.getControl(name);
        if (!control)
        {
            LL_WARNS("ViewerPresets") << "Viewer presets name an unknown setting, skipped: " << name << LL_ENDL;
            continue;
        }
        if (!backup["settings"].has(name)) backup["settings"][name] = control->getValue();
    }

    // Key bindings first: saving third-person bindings rewrites DoubleClickTeleport (see
    // LLKeyConflictHandler::saveToSettings), which the settings below then set as intended.
    std::set<std::string> keys_changed;
    for (const BindingMode& mode : BINDING_MODES)
    {
        if (u.inputs.empty()) break;
        LLKeyConflictHandler handler(mode.mode);
        for (const auto& [input_id, input] : u.inputs)
        {
            const std::set<std::string>& commands = u.commands.at(input_id);
            if (!backup["keys"][mode.name].has(input_id))
            {
                backup["keys"][mode.name][input_id] = asArray(boundCommands(handler, input, commands));
                backup["inputs"][input_id] = input.desc;
            }
            std::set<std::string> want;
            bool listed = false;
            for (const Preset* source : { &preset, &defaults })
            {
                for (const Binding& binding : source->keys)
                {
                    if (binding.input.id != input_id) continue;
                    want.insert(binding.command);
                    listed = true;
                }
                if (listed) break;
            }
            if (!listed) continue;     // nobody says what this input should do: leave it
            if (bindExactly(handler, input, commands, want)) keys_changed.insert(input_id);
        }
        if (handler.hasUnsavedChanges()) handler.saveToSettings();
    }
    result.changed += (S32)keys_changed.size();
    gSavedSettings.setLLSD(BACKUP_SETTING, backup);

    for (const std::string& name : u.settings)
    {
        LLControlVariablePtr control = gSavedSettings.getControl(name);
        if (!control) continue;
        LLSD want;
        if (preset.settings.has(name))
        {
            if (!coerce(*control, preset.settings[name], want))
            {
                LL_WARNS("ViewerPresets") << id << ": wrong kind of value for " << name << ", skipped" << LL_ENDL;
                continue;
            }
        }
        else if (!coerce(*control, control->getDefault(), want))
        {
            want = control->getDefault();
        }
        if (setControl(*control, want))
        {
            ++result.changed;
            if (u.restart.count(name)) result.restart.push_back(name);
        }
    }

    gSavedSettings.setString("FirstRunViewerChoice", id);
    saveSettings();
    LL_INFOS("ViewerPresets") << "Applied viewer preset " << id << ": " << result.changed << " change(s)" << LL_ENDL;
    return true;
}

bool undo(Result& result)
{
    result = Result();
    const LLSD backup = gSavedSettings.getLLSD(BACKUP_SETTING);
    if (!backup.isMap()) return false;
    const Universe u = loadUniverse();

    std::set<std::string> keys_changed;
    for (const BindingMode& mode : BINDING_MODES)
    {
        const LLSD& saved = backup["keys"][mode.name];
        if (!saved.isMap() || saved.size() == 0) continue;
        LLKeyConflictHandler handler(mode.mode);
        for (LLSD::map_const_iterator it = saved.beginMap(); it != saved.endMap(); ++it)
        {
            Input input;
            if (!parseInput(backup["inputs"][it->first], input)) continue;
            const std::set<std::string> want = asSet(it->second);
            std::set<std::string> commands = want;
            const auto known = u.commands.find(it->first);
            if (known != u.commands.end()) commands.insert(known->second.begin(), known->second.end());
            if (bindExactly(handler, input, commands, want)) keys_changed.insert(it->first);
        }
        if (handler.hasUnsavedChanges()) handler.saveToSettings();
    }
    result.changed += (S32)keys_changed.size();

    const LLSD& settings = backup["settings"];
    for (LLSD::map_const_iterator it = settings.beginMap(); it != settings.endMap(); ++it)
    {
        LLControlVariablePtr control = gSavedSettings.getControl(it->first);
        if (!control) continue;
        LLSD want;
        if (!coerce(*control, it->second, want)) want = it->second;
        if (setControl(*control, want))
        {
            ++result.changed;
            if (u.restart.count(it->first)) result.restart.push_back(it->first);
        }
    }

    gSavedSettings.setLLSD(BACKUP_SETTING, LLSD::emptyMap());
    gSavedSettings.setString("FirstRunViewerChoice", std::string());
    saveSettings();
    LL_INFOS("ViewerPresets") << "Put back the settings from before the first viewer preset: " << result.changed << " change(s)" << LL_ENDL;
    return true;
}
}
