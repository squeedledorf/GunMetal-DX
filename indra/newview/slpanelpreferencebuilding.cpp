/**
 * @file slpanelpreferencebuilding.cpp
 * @brief Preferences > Building: defaults for new prims and for new
 *        scripts (#466).
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

#include "indra_constants.h"
#include "llbutton.h"
#include "lllineeditor.h"
#include "llpermissions.h"
#include "lltrans.h"
#include "llui.h"

#include "llagent.h"
#include "llfloaterpreference.h"
#include "llinventorymodel.h"
#include "llstartup.h"
#include "lltexturectrl.h"
#include "llviewercontrol.h"
#include "llviewerinventory.h"

// A read-only line naming one inventory item, set by dragging an item from
// the agent's own inventory onto it. Only items the agent can copy are
// accepted (they are copied, never moved); scripts_only narrows it to
// scripts that can also be modified.
class SLInventoryItemDropTarget : public LLLineEditor
{
public:
    struct Params : public LLInitParam::Block<Params, LLLineEditor::Params>
    {
        Optional<bool> scripts_only;

        Params()
        :   scripts_only("scripts_only", false)
        {}
    };

    typedef boost::signals2::signal<void(const LLUUID& item_id)> drop_signal_t;

protected:
    SLInventoryItemDropTarget(const Params& p)
    :   LLLineEditor(p),
        mScriptsOnly(p.scripts_only)
    {}
    friend class LLUICtrlFactory;

public:
    boost::signals2::connection setDropCallback(const drop_signal_t::slot_type& cb)
    {
        return mDropSignal.connect(cb);
    }

    bool postBuild() override
    {
        setEnabled(false);
        return LLLineEditor::postBuild();
    }

    bool handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop,
                           EDragAndDropType cargo_type,
                           void* cargo_data,
                           EAcceptance* accept,
                           std::string& tooltip_msg) override
    {
        *accept = ACCEPT_NO;

        const LLInventoryItem* item = acceptableItem(cargo_type, cargo_data);
        if (item)
        {
            if (drop)
            {
                mDropSignal(item->getUUID());
            }
            *accept = ACCEPT_YES_SINGLE;
        }
        return true;
    }

private:
    const LLInventoryItem* acceptableItem(EDragAndDropType cargo_type, void* cargo_data) const
    {
        if (!cargo_data || LLStartUp::getStartupState() < STATE_STARTED)
        {
            return nullptr;
        }

        switch (cargo_type)
        {
            case DAD_TEXTURE:
            case DAD_SOUND:
            case DAD_CALLINGCARD:
            case DAD_LANDMARK:
            case DAD_SCRIPT:
            case DAD_CLOTHING:
            case DAD_OBJECT:
            case DAD_NOTECARD:
            case DAD_BODYPART:
            case DAD_ANIMATION:
            case DAD_GESTURE:
            case DAD_MESH:
            case DAD_SETTINGS:
            case DAD_MATERIAL:
                break;
            default:
                return nullptr;
        }
        if (mScriptsOnly && cargo_type != DAD_SCRIPT)
        {
            return nullptr;
        }

        const LLInventoryItem* item = static_cast<const LLInventoryItem*>(cargo_data);
        if (item->getIsLinkType()
            || !gInventory.isObjectDescendentOf(item->getUUID(), gInventory.getRootFolderID()))
        {
            // Not from the agent's own inventory (e.g. the Library), or a link.
            return nullptr;
        }

        const LLPermissions& perm = item->getPermissions();
        if (!perm.allowCopyBy(gAgent.getID())
            || (mScriptsOnly && !perm.allowModifyBy(gAgent.getID())))
        {
            return nullptr;
        }
        return item;
    }

    bool          mScriptsOnly;
    drop_signal_t mDropSignal;
};

static LLDefaultChildRegistry::Register<SLInventoryItemDropTarget> r_sl_item_drop_target("sl_inventory_item_drop_target");

class SLPanelPreferenceBuilding : public LLPanelPreference
{
public:
    bool postBuild() override;
    void saveSettings() override;
    void cancel(const std::vector<std::string> settings_to_skip = {}) override;
    void onVisibilityChange(bool new_visibility) override;

private:
    void refreshControls();
    void setItemSetting(const std::string& setting, const LLUUID& item_id);
    void refreshItemTarget(const std::string& target_name, const std::string& clear_name,
                           const std::string& setting, const std::string& not_set_string);
    void saveControl(LLControlGroup& group, const std::string& name);

    LLTextureCtrl* mTextureCtrl = nullptr;
};

static LLPanelInjector<SLPanelPreferenceBuilding> t_pref_building("panel_preference_building");

bool SLPanelPreferenceBuilding::postBuild()
{
    mTextureCtrl = getChild<LLTextureCtrl>("new_prim_texture");
    // Only textures the agent may copy and transfer: those can be put on a
    // prim by asset id without handing the texture item over.
    mTextureCtrl->setFilterPermissionMasks(PERM_COPY | PERM_TRANSFER);
    mTextureCtrl->setAllowLocalTexture(false);
    mTextureCtrl->setDefaultImageAssetID(DEFAULT_OBJECT_TEXTURE);
    mTextureCtrl->setCommitCallback([](LLUICtrl* ctrl, const LLSD&)
        {
            const LLUUID id = ctrl->getValue().asUUID();
            gSavedSettings.setString("SlipstreamNewPrimTexture", id.notNull() ? id.asString() : std::string());
        });

    if (SLInventoryItemDropTarget* target = findChild<SLInventoryItemDropTarget>("new_prim_item"))
    {
        target->setDropCallback([this](const LLUUID& item_id) { setItemSetting("SlipstreamNewPrimEmbedItem", item_id); });
    }
    getChild<LLButton>("clear_new_prim_item")->setCommitCallback([this](LLUICtrl*, const LLSD&)
        {
            setItemSetting("SlipstreamNewPrimEmbedItem", LLUUID::null);
        });

    if (SLInventoryItemDropTarget* target = findChild<SLInventoryItemDropTarget>("default_script_item"))
    {
        target->setDropCallback([this](const LLUUID& item_id) { setItemSetting("SlipstreamDefaultScriptItem", item_id); });
    }
    getChild<LLButton>("clear_default_script_item")->setCommitCallback([this](LLUICtrl*, const LLSD&)
        {
            setItemSetting("SlipstreamDefaultScriptItem", LLUUID::null);
        });

    refreshControls();
    return LLPanelPreference::postBuild();
}

void SLPanelPreferenceBuilding::saveControl(LLControlGroup& group, const std::string& name)
{
    LLControlVariable* control = group.getControl(name);
    if (control)
    {
        mSavedValues[control] = control->getValue();
    }
}

void SLPanelPreferenceBuilding::saveSettings()
{
    LLPanelPreference::saveSettings();

    // These are set from code, not bound to a widget, so Cancel would not
    // restore them on its own.
    saveControl(gSavedSettings, "SlipstreamNewPrimTexture");
    if (LLStartUp::getStartupState() == STATE_STARTED)
    {
        // Per-account settings only exist once logged in.
        saveControl(gSavedPerAccountSettings, "SlipstreamNewPrimEmbedItem");
        saveControl(gSavedPerAccountSettings, "SlipstreamDefaultScriptItem");
    }
    refreshControls();
}

void SLPanelPreferenceBuilding::cancel(const std::vector<std::string> settings_to_skip)
{
    LLPanelPreference::cancel(settings_to_skip);
    refreshControls();
}

void SLPanelPreferenceBuilding::onVisibilityChange(bool new_visibility)
{
    LLPanelPreference::onVisibilityChange(new_visibility);
    if (new_visibility)
    {
        refreshControls();
    }
}

void SLPanelPreferenceBuilding::setItemSetting(const std::string& setting, const LLUUID& item_id)
{
    if (LLStartUp::getStartupState() < STATE_STARTED)
    {
        return;
    }
    gSavedPerAccountSettings.setString(setting, item_id.notNull() ? item_id.asString() : std::string());
    refreshControls();
}

void SLPanelPreferenceBuilding::refreshItemTarget(const std::string& target_name, const std::string& clear_name,
                                                  const std::string& setting, const std::string& not_set_string)
{
    SLInventoryItemDropTarget* target = findChild<SLInventoryItemDropTarget>(target_name);
    LLButton* clear_btn = findChild<LLButton>(clear_name);
    if (!target || !clear_btn)
    {
        return;
    }

    if (LLStartUp::getStartupState() < STATE_STARTED)
    {
        target->setText(LLTrans::getString("BuildPrefsItemNotLoggedIn"));
        clear_btn->setEnabled(false);
        return;
    }

    const LLUUID item_id(gSavedPerAccountSettings.getString(setting));
    if (item_id.isNull())
    {
        target->setText(LLTrans::getString(not_set_string));
        clear_btn->setEnabled(false);
        return;
    }

    clear_btn->setEnabled(true);
    const LLViewerInventoryItem* item = gInventory.getItem(item_id);
    target->setText(item ? item->getName() : LLTrans::getString("BuildPrefsItemNotAvailable"));
}

void SLPanelPreferenceBuilding::refreshControls()
{
    if (mTextureCtrl)
    {
        mTextureCtrl->setValue(LLUUID(gSavedSettings.getString("SlipstreamNewPrimTexture")));
    }
    refreshItemTarget("new_prim_item", "clear_new_prim_item", "SlipstreamNewPrimEmbedItem", "BuildPrefsItemNotSet");
    refreshItemTarget("default_script_item", "clear_default_script_item", "SlipstreamDefaultScriptItem", "BuildPrefsScriptNotSet");
}
