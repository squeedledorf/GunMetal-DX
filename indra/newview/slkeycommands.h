/**
 * @file slkeycommands.h
 * @brief Named, rebindable keyboard commands (menu accelerators and former hard-coded keys, #863)
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Linden Research, Inc.
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
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#ifndef SL_SLKEYCOMMANDS_H
#define SL_SLKEYCOMMANDS_H

#include "llkeybind.h"
#include "llhandle.h"
#include "llsingleton.h"
#include "llviewerinput.h"

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

class LLMenuGL;
class LLView;

// One rebindable keyboard command that is not a movement/camera action (those live in
// LLViewerInput's modes). Two kinds:
//  - menu accelerators: discovered from the menu XUI files' shortcut= attributes
//  - code commands: former hard-coded key checks, declared in key_commands.xml
// The default key always comes from XUI; user changes are stored as overrides in the
// <commands> section of key_bindings.xml (only the differences, so new defaults merge in).
struct SLKeyCommand
{
    std::string mId;        // stable id: "<scope>:<menu path>" or the code command name
    std::string mLabel;     // text shown in Preferences > Controls
    std::string mGroup;     // group heading shown in Preferences > Controls
    std::string mContext;   // commands only conflict inside one context
    std::string mScope;     // menu file scope ("viewer", "login", ...), empty for code commands
    std::string mMenuPath;  // item path inside the menu file (menu items only)
    LLKeyData   mDefault;   // effective default: mBaseDefault, relocated when SlipstreamAltShiftRelocate is on
    LLKeyData   mBaseDefault; // the default as declared in XUI
};

// A key that stays fixed (dialog Enter/Escape, list navigation, standard text editing, ...).
// Only listed in Preferences > Controls so that every shortcut is documented there.
struct SLFixedKey
{
    std::string mLabel;
    std::string mKeys;
};

class SLKeyCommands : public LLSingleton<SLKeyCommands>
{
    LLSINGLETON(SLKeyCommands);
    LOG_CLASS(SLKeyCommands);

public:
    // All commands in display order (grouped, see getGroups())
    const std::vector<SLKeyCommand>& getCommands();
    const SLKeyCommand* findCommand(const std::string& id);
    // Group headings in display order (code command groups first, then one per menu)
    const std::vector<std::string>& getGroups();

    // Keys that cannot be rebound, documented in the panel (group heading from getFixedGroup())
    const std::vector<SLFixedKey>& getFixedKeys();
    const std::string& getFixedGroup();

    // Current binding: the user's override, else the default. May be empty (cleared).
    LLKeyData getBinding(const std::string& id);
    // True when key+mask is the current binding of the command. For code commands.
    bool matches(const std::string& id, KEY key, MASK mask);
    // Localized key text for a command ("Ctrl+Shift+F"), empty when unbound. For messages.
    std::string getBindingString(const std::string& id);

    // Localized key text of a menu item, found by the name of the item in a menu file scope
    // ("viewer", "Freeze World"). For messages that name a shortcut. Empty when unbound.
    std::string getMenuBindingString(const std::string& scope, const std::string& item_name);

    // True when key+mask is the current binding of a menu item, found like
    // getMenuBindingString (#919: the fullscreen key is handled before the focus chain)
    bool matchesMenuItem(const std::string& scope, const std::string& item_name, KEY key, MASK mask);

    // Replaces the user overrides (the <commands> section of key_bindings.xml) and
    // re-applies them to the menus
    void setOverrides(const std::map<std::string, LLKeyData>& overrides);
    void loadOverrides(const LLViewerInput::KeyMode& commands);
    // The <commands> section as id -> binding (an empty binding is a cleared command)
    static std::map<std::string, LLKeyData> parseOverrides(const LLViewerInput::KeyMode& commands);
    const std::map<std::string, LLKeyData>& getOverrides() const { return mOverrides; }

    // Sets the accelerator of every item of 'menu' that has a command, for the menu file
    // scope the menu was built from. Call after building a menu; the menu is remembered so
    // that later changes of a binding reach it too.
    void applyToMenu(LLMenuGL* menu, const std::string& scope);
    // Re-applies to every menu that was applied before (and the viewer, login and edit menus)
    void applyToMenus();

    // Whether commands are in the same context (the same key would be a conflict)
    static bool sameContext(const SLKeyCommand& a, const SLKeyCommand& b);
    // Other commands (same context) whose current binding is this key
    std::vector<std::string> findConflicts(const std::string& id, const LLKeyData& data);
    // The movement/camera action (any mode) bound to this key, empty when none
    std::string findMovementConflict(const LLKeyData& data) const;

    // Alt+Shift is the input language switch on Windows (and some Linux desktops): pure
    // Alt+Shift chords are flagged
    static bool isDiscouraged(const LLKeyData& data);

private:
    void ensureLoaded();
    // #948: recomputes every mDefault from mBaseDefault and SlipstreamAltShiftRelocate (the
    // user's overrides are never touched)
    void resolveDefaults();
    void onAltShiftRelocateChanged();
    void scanMenuFile(const std::string& filename, const std::string& scope, const std::string& context);
    void scanCommandsFile(const std::string& filename);
    void addCommand(SLKeyCommand&& command);
    void applyToMenuInternal(LLMenuGL* menu, const std::string& scope, const std::string& path);

    bool mLoaded;
    std::vector<std::pair<LLHandle<LLView>, std::string> > mMenus; // applied menus and their scope
    std::vector<SLKeyCommand> mCommands;
    std::unordered_map<std::string, size_t> mIndex;
    std::vector<std::string> mGroups;
    std::vector<SLFixedKey> mFixedKeys;
    std::string mFixedGroup;
    std::map<std::string, LLKeyData> mOverrides;
    // menu file scope -> (group heading, prefix items with their menu name), from key_commands.xml
    std::map<std::string, std::pair<std::string, bool> > mScopeGroups;
};

#endif // SL_SLKEYCOMMANDS_H
