/**
 * @file slkeycommands.cpp
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

#include "llviewerprecompiledheaders.h"

#include "slkeycommands.h"

#include "llkeyboard.h"
#include "llcontrol.h"
#include "llmenugl.h"
#include "lluictrlfactory.h"
#include "llviewercontrol.h"
#include "llviewermenu.h"
#include "llxmlnode.h"

#include <algorithm>

// Menu files that carry accelerators: the file, the scope used in command ids, and the
// conflict context (commands of one context share the keyboard, different contexts do not:
// the login menu is gone after login, the script editor has its own menu, ...)
namespace
{
struct MenuFile
{
    const char* filename;
    const char* scope;
    const char* context;
};
const MenuFile menu_files[] =
{
    { "menu_viewer.xml",                "viewer",           "main" },
    { "menu_edit.xml",                  "edit",             "main" },
    { "menu_login.xml",                 "login",            "login" },
    { "menu_inventory_add.xml",         "inventory_add",    "inventory" },
    { "menu_inventory_gear_default.xml","inventory_gear",   "inventory" },
    { "panel_script_ed.xml",            "script_editor",    "script_editor" },
    // Same menu as panel_script_ed.xml, the ids merge
    { "panel_script_ed_preproc.xml",    "script_editor",    "script_editor" },
};
const char* const commands_file = "key_commands.xml";

std::string attr(LLXMLNodePtr node, const char* name)
{
    std::string value;
    node->getAttributeString(name, value);
    return value;
}

std::string node_tag(const LLXMLNodePtr& node)
{
    return node->getName() ? std::string(node->getName()->mString) : std::string();
}

bool is_menu_item(const std::string& name)
{
    return name.compare(0, 9, "menu_item") == 0 && name.find('.') == std::string::npos;
}

bool is_menu_container(const std::string& name)
{
    return name == "menu" || name == "context_menu" || name == "pie_menu";
}

struct Item
{
    std::string path;
    std::string label;
    std::string group;
    KEY key;
    MASK mask;
};

// Collects the menu items that have a shortcut. The item path is made of the names of the
// nested menus and the item, the same names LLMenuGL's item tree has, so the commands can
// be matched to the built menu (see SLKeyCommands::applyToMenuInternal).
void collect_items(const LLXMLNodePtr& node, const std::string& path, const std::string& label_path,
                   const std::string& group, std::vector<Item>& items)
{
    for (LLXMLNodePtr child = node->getFirstChild(); child.notNull(); child = child->getNextSibling())
    {
        const std::string name = node_tag(child);
        if (name.find('.') != std::string::npos)
        {
            // item parameters (menu_item_call.on_click and the like)
            continue;
        }
        if (is_menu_item(name))
        {
            std::string shortcut = attr(child, "shortcut");
            if (shortcut.empty())
            {
                continue;
            }
            Item item;
            item.key = KEY_NONE;
            item.mask = MASK_NONE;
            LLMenuItemGL::parseShortcut(shortcut, &item.key, &item.mask);
            if (item.key == KEY_NONE)
            {
                continue;
            }
            const std::string item_name = attr(child, "name");
            std::string label = attr(child, "label");
            if (label.empty())
            {
                label = item_name;
            }
            item.path = path.empty() ? item_name : path + "/" + item_name;
            item.label = label_path.empty() ? label : label_path + " > " + label;
            item.group = group;
            items.push_back(item);
        }
        else if (is_menu_container(name))
        {
            const std::string menu_name = attr(child, "name");
            std::string menu_label = attr(child, "label");
            if (menu_label.empty())
            {
                menu_label = menu_name;
            }
            const std::string sub_path = path.empty() ? menu_name : path + "/" + menu_name;
            if (group.empty())
            {
                // top level menu: its label is the group heading
                collect_items(child, sub_path, "", menu_label, items);
            }
            else
            {
                collect_items(child, sub_path, label_path.empty() ? menu_label : label_path + " > " + menu_label, group, items);
            }
        }
        else
        {
            // menu_bar, panels holding a menu bar, ...
            collect_items(child, path, label_path, group, items);
        }
    }
}
} // namespace

SLKeyCommands::SLKeyCommands()
:   mLoaded(false)
{
}

void SLKeyCommands::ensureLoaded()
{
    if (mLoaded)
    {
        return;
    }
    mLoaded = true;

    // Code commands first, they are the headings at the top of the panel
    scanCommandsFile(commands_file);
    for (const MenuFile& file : menu_files)
    {
        scanMenuFile(file.filename, file.scope, file.context);
    }

    // The fixed keys are the last heading
    if (!mFixedKeys.empty() && !mFixedGroup.empty())
    {
        mGroups.push_back(mFixedGroup);
    }

    // Keep each group's commands together, groups in order of appearance
    std::stable_sort(mCommands.begin(), mCommands.end(),
        [this](const SLKeyCommand& a, const SLKeyCommand& b)
        {
            const auto ia = std::find(mGroups.begin(), mGroups.end(), a.mGroup);
            const auto ib = std::find(mGroups.begin(), mGroups.end(), b.mGroup);
            return ia < ib;
        });
    mIndex.clear();
    for (size_t i = 0; i < mCommands.size(); ++i)
    {
        mIndex[mCommands[i].mId] = i;
    }

    // #948: Alt+Shift defaults stay Alt+Shift (LL / Firestorm parity) unless the user turned
    // on SlipstreamAltShiftRelocate, see resolveDefaults()
    for (SLKeyCommand& command : mCommands)
    {
        command.mBaseDefault = command.mDefault;
    }
    resolveDefaults();
    if (LLControlVariable* control = gSavedSettings.getControl("SlipstreamAltShiftRelocate"))
    {
        control->getSignal()->connect([this](LLControlVariable*, const LLSD&, const LLSD&)
        {
            onAltShiftRelocateChanged();
        });
    }

    LL_INFOS("KeyCommands") << "Registered " << mCommands.size() << " keyboard commands in " << mGroups.size() << " groups" << LL_ENDL;
}

void SLKeyCommands::resolveDefaults()
{
    // Moves the pure Alt+Shift defaults to Ctrl+Alt+Shift (for keyboards where Alt+Shift
    // switches the input language), or unbinds them when that slot is taken in the same
    // context. Only the defaults change: user overrides win in getBinding().
    const bool relocate = gSavedSettings.getBOOL("SlipstreamAltShiftRelocate");
    for (SLKeyCommand& command : mCommands)
    {
        command.mDefault = command.mBaseDefault;
    }
    if (!relocate)
    {
        return;
    }
    for (SLKeyCommand& command : mCommands)
    {
        if (!isDiscouraged(command.mBaseDefault))
        {
            continue;
        }
        LLKeyData moved = command.mBaseDefault;
        moved.mMask = MASK_CONTROL | MASK_ALT | MASK_SHIFT;
        bool taken = false;
        for (const SLKeyCommand& other : mCommands)
        {
            if (sameContext(command, other) && other.mBaseDefault.mKey == moved.mKey && other.mBaseDefault.mMask == moved.mMask)
            {
                taken = true;
                break;
            }
        }
        if (taken)
        {
            command.mDefault.reset();
            LL_INFOS("KeyCommands") << "No default for " << command.mId << " (Alt+Shift relocation, Ctrl+Alt+Shift slot taken)" << LL_ENDL;
        }
        else
        {
            command.mDefault = moved;
        }
    }
}

void SLKeyCommands::onAltShiftRelocateChanged()
{
    resolveDefaults();
    applyToMenus();
}

void SLKeyCommands::addCommand(SLKeyCommand&& command)
{
    if (std::find(mGroups.begin(), mGroups.end(), command.mGroup) == mGroups.end())
    {
        mGroups.push_back(command.mGroup);
    }
    // Two items of one menu that share a name (and so a path) get distinct ids
    const std::string base = command.mId;
    for (S32 n = 2; mIndex.count(command.mId) != 0; ++n)
    {
        command.mId = base + "#" + std::to_string(n);
    }
    mIndex[command.mId] = mCommands.size();
    mCommands.push_back(std::move(command));
}

void SLKeyCommands::scanMenuFile(const std::string& filename, const std::string& scope, const std::string& context)
{
    LLXMLNodePtr root;
    if (!LLUICtrlFactory::getLayeredXMLNode(filename, root))
    {
        LL_WARNS("KeyCommands") << "Failed to load " << filename << LL_ENDL;
        return;
    }

    std::vector<Item> items;
    if (is_menu_container(node_tag(root)))
    {
        // A menu file whose root is the menu itself: the root is not part of the item path
        std::string label = attr(root, "label");
        if (label.empty())
        {
            label = attr(root, "name");
        }
        collect_items(root, "", "", label, items);
    }
    else
    {
        collect_items(root, "", "", "", items);
    }

    for (const Item& item : items)
    {
        SLKeyCommand command;
        command.mId = scope + ":" + item.path;
        command.mLabel = item.label;
        command.mGroup = item.group;
        auto scope_group = mScopeGroups.find(scope);
        if (scope_group != mScopeGroups.end())
        {
            if (scope_group->second.second && item.group != scope_group->second.first)
            {
                command.mLabel = item.group + " > " + item.label;
            }
            command.mGroup = scope_group->second.first;
        }
        command.mContext = context;
        command.mScope = scope;
        command.mMenuPath = item.path;
        command.mDefault = LLKeyData(CLICK_NONE, item.key, item.mask & MASK_NORMALKEYS, true);
        if (mIndex.count(command.mId) != 0 && scope == "script_editor")
        {
            // the second script editor file repeats the first one's items
            continue;
        }
        addCommand(std::move(command));
    }
}

void SLKeyCommands::scanCommandsFile(const std::string& filename)
{
    LLXMLNodePtr root;
    if (!LLUICtrlFactory::getLayeredXMLNode(filename, root))
    {
        LL_WARNS("KeyCommands") << "Failed to load " << filename << LL_ENDL;
        return;
    }

    std::map<std::string, std::string> group_labels;
    for (LLXMLNodePtr child = root->getFirstChild(); child.notNull(); child = child->getNextSibling())
    {
        const std::string name = node_tag(child);
        if (name == "group")
        {
            const std::string group_name = attr(child, "name");
            std::string label = attr(child, "label");
            group_labels[group_name] = label.empty() ? group_name : label;
            if (std::find(mGroups.begin(), mGroups.end(), group_labels[group_name]) == mGroups.end())
            {
                mGroups.push_back(group_labels[group_name]);
            }
        }
        else if (name == "scope_group")
        {
            // Menu files that are not a menu bar of their own (login, inventory, script editor):
            // one heading for the whole file instead of one per menu
            const std::string scope = attr(child, "scope");
            mScopeGroups[scope] = std::make_pair(attr(child, "label"), attr(child, "prefix") == "true");
            if (std::find(mGroups.begin(), mGroups.end(), mScopeGroups[scope].first) == mGroups.end())
            {
                mGroups.push_back(mScopeGroups[scope].first);
            }
        }
        else if (name == "fixed_group")
        {
            mFixedGroup = attr(child, "label");
        }
        else if (name == "fixed_key")
        {
            SLFixedKey fixed;
            fixed.mLabel = attr(child, "label");
            fixed.mKeys = attr(child, "keys");
            mFixedKeys.push_back(fixed);
        }
        else if (name == "command")
        {
            SLKeyCommand command;
            command.mId = attr(child, "name");
            command.mLabel = attr(child, "label");
            const std::string group_name = attr(child, "group");
            command.mGroup = group_labels.count(group_name) ? group_labels[group_name] : group_name;
            command.mContext = attr(child, "context");
            if (command.mContext.empty())
            {
                command.mContext = "main";
            }
            KEY key = KEY_NONE;
            MASK mask = MASK_NONE;
            LLMenuItemGL::parseShortcut(attr(child, "shortcut"), &key, &mask);
            command.mDefault = LLKeyData(CLICK_NONE, key, mask & MASK_NORMALKEYS, true);
            if (command.mId.empty())
            {
                continue;
            }
            addCommand(std::move(command));
        }
    }
}

const std::vector<SLKeyCommand>& SLKeyCommands::getCommands()
{
    ensureLoaded();
    return mCommands;
}

const std::vector<std::string>& SLKeyCommands::getGroups()
{
    ensureLoaded();
    return mGroups;
}

const std::vector<SLFixedKey>& SLKeyCommands::getFixedKeys()
{
    ensureLoaded();
    return mFixedKeys;
}

const std::string& SLKeyCommands::getFixedGroup()
{
    ensureLoaded();
    return mFixedGroup;
}

const SLKeyCommand* SLKeyCommands::findCommand(const std::string& id)
{
    ensureLoaded();
    auto it = mIndex.find(id);
    return it == mIndex.end() ? nullptr : &mCommands[it->second];
}

LLKeyData SLKeyCommands::getBinding(const std::string& id)
{
    auto override_it = mOverrides.find(id);
    if (override_it != mOverrides.end())
    {
        return override_it->second;
    }
    const SLKeyCommand* command = findCommand(id);
    return command ? command->mDefault : LLKeyData();
}

bool SLKeyCommands::matches(const std::string& id, KEY key, MASK mask)
{
    if (key == KEY_NONE)
    {
        return false;
    }
    const LLKeyData binding = getBinding(id);
    return binding.mKey == key && binding.mMask == mask;
}

std::string SLKeyCommands::getBindingString(const std::string& id)
{
    const LLKeyData binding = getBinding(id);
    if (binding.mKey == KEY_NONE)
    {
        return std::string();
    }
    return LLKeyboard::stringFromAccelerator(binding.mMask, binding.mKey);
}

std::string SLKeyCommands::getMenuBindingString(const std::string& scope, const std::string& item_name)
{
    ensureLoaded();
    for (const SLKeyCommand& command : mCommands)
    {
        if (command.mScope != scope)
        {
            continue;
        }
        const size_t slash = command.mMenuPath.rfind('/');
        const std::string name = slash == std::string::npos ? command.mMenuPath : command.mMenuPath.substr(slash + 1);
        if (name == item_name)
        {
            return getBindingString(command.mId);
        }
    }
    return std::string();
}

bool SLKeyCommands::matchesMenuItem(const std::string& scope, const std::string& item_name, KEY key, MASK mask)
{
    if (key == KEY_NONE)
    {
        return false;
    }
    ensureLoaded();
    for (const SLKeyCommand& command : mCommands)
    {
        if (command.mScope != scope)
        {
            continue;
        }
        const size_t slash = command.mMenuPath.rfind('/');
        const std::string name = slash == std::string::npos ? command.mMenuPath : command.mMenuPath.substr(slash + 1);
        if (name == item_name && matches(command.mId, key, mask))
        {
            return true;
        }
    }
    return false;
}

void SLKeyCommands::setOverrides(const std::map<std::string, LLKeyData>& overrides)
{
    mOverrides = overrides;
    applyToMenus();
}

// static
std::map<std::string, LLKeyData> SLKeyCommands::parseOverrides(const LLViewerInput::KeyMode& commands)
{
    std::map<std::string, LLKeyData> overrides;
    for (LLInitParam::ParamIterator<LLViewerInput::KeyBinding>::const_iterator it = commands.bindings.begin(),
        end_it = commands.bindings.end();
        it != end_it;
        ++it)
    {
        const LLViewerInput::KeyBinding& binding = *it;
        KEY key = KEY_NONE;
        MASK mask = MASK_NONE;
        if (!binding.key.getValue().empty())
        {
            LLKeyboard::keyFromString(binding.key, &key);
        }
        LLKeyboard::maskFromString(binding.mask, &mask);
        // An empty key is a cleared command
        overrides[binding.command] = (key == KEY_NONE) ? LLKeyData() : LLKeyData(CLICK_NONE, key, mask, true);
    }
    return overrides;
}

void SLKeyCommands::loadOverrides(const LLViewerInput::KeyMode& commands)
{
    setOverrides(parseOverrides(commands));
}

void SLKeyCommands::applyToMenuInternal(LLMenuGL* menu, const std::string& scope, const std::string& path)
{
    const S32 count = static_cast<S32>(menu->getItemCount());
    for (S32 i = 0; i < count; ++i)
    {
        LLMenuItemGL* item = menu->getItem(i);
        if (!item)
        {
            continue;
        }
        if (LLMenuItemBranchGL* branch_item = dynamic_cast<LLMenuItemBranchGL*>(item))
        {
            if (LLMenuGL* branch = branch_item->getBranch())
            {
                const std::string name = branch->getName();
                applyToMenuInternal(branch, scope, path.empty() ? name : path + "/" + name);
            }
            continue;
        }
        const std::string item_path = path.empty() ? item->getName() : path + "/" + item->getName();
        const SLKeyCommand* command = findCommand(scope + ":" + item_path);
        if (!command)
        {
            continue;
        }
        const LLKeyData binding = getBinding(command->mId);
        item->setAccelerator(binding.mKey, binding.mMask);
    }
}

void SLKeyCommands::applyToMenu(LLMenuGL* menu, const std::string& scope)
{
    if (!menu)
    {
        return;
    }
    ensureLoaded();
    const LLHandle<LLView> handle = menu->getHandle();
    bool known = false;
    for (const auto& entry : mMenus)
    {
        if (entry.first == handle)
        {
            known = true;
            break;
        }
    }
    if (!known)
    {
        mMenus.push_back(std::make_pair(handle, scope));
    }
    applyToMenuInternal(menu, scope, "");
}

void SLKeyCommands::applyToMenus()
{
    // the global menus are rebuilt after a logout, so they are looked up each time
    if (!mLoaded && !gMenuBarView && !gLoginMenuBarView && !gEditMenu)
    {
        // nothing to apply to yet, the menus apply the overrides when they are built
        return;
    }
    applyToMenu(gMenuBarView, "viewer");
    applyToMenu(gLoginMenuBarView, "login");
    applyToMenu(gEditMenu, "edit");

    // drop the menus that are gone, apply to the others (script editors, inventory menus)
    for (size_t i = 0; i < mMenus.size();)
    {
        LLMenuGL* menu = dynamic_cast<LLMenuGL*>(mMenus[i].first.get());
        if (!menu)
        {
            mMenus.erase(mMenus.begin() + i);
            continue;
        }
        applyToMenuInternal(menu, mMenus[i].second, "");
        ++i;
    }
}

// static
bool SLKeyCommands::sameContext(const SLKeyCommand& a, const SLKeyCommand& b)
{
    return a.mContext == b.mContext;
}

std::vector<std::string> SLKeyCommands::findConflicts(const std::string& id, const LLKeyData& data)
{
    std::vector<std::string> result;
    const SLKeyCommand* command = findCommand(id);
    if (!command || data.mKey == KEY_NONE)
    {
        return result;
    }
    for (const SLKeyCommand& other : mCommands)
    {
        if (other.mId == id || !sameContext(*command, other))
        {
            continue;
        }
        const LLKeyData binding = getBinding(other.mId);
        if (binding.mKey == data.mKey && binding.mMask == data.mMask)
        {
            result.push_back(other.mId);
        }
    }
    return result;
}

std::string SLKeyCommands::findMovementConflict(const LLKeyData& data) const
{
    if (data.mKey == KEY_NONE)
    {
        return std::string();
    }
    return gViewerInput.findBoundCommand(data.mKey, data.mMask);
}

// static
bool SLKeyCommands::isDiscouraged(const LLKeyData& data)
{
    return data.mKey != KEY_NONE && data.mMask == (MASK_ALT | MASK_SHIFT);
}
