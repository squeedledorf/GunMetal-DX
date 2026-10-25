/**
 * @file slpanelphotostudioeffects.cpp
 * @brief Photo Studio's Effects page: the FX stack's techniques, the selected
 *        effect's controls (made from its annotations) and ReShade presets
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
#include "slpanelphotostudioeffects.h"

#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llcolorswatch.h"
#include "llcombobox.h"
#include "lldir.h"
#include "llfile.h"
#include "llfocusmgr.h"
#include "llnotificationsutil.h"
#include "llscrollcontainer.h"
#include "llscrolllistcell.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "llsearcheditor.h"
#include "llsliderctrl.h"
#include "llspinctrl.h"
#include "lltextbox.h"
#include "lluicolortable.h"
#include "llviewercontrol.h"
#include "llviewermenufile.h" // LLFilePickerReplyThread
#include "slfloaterphotostudio.h"
#include "slfxpresets.h"
#include "slfxstack.h"

#include <algorithm>
#include <cmath>

namespace
{
constexpr S32 ROW_HEIGHT = 22;
constexpr S32 COLOR_ROW_HEIGHT = 26;
constexpr S32 HEADER_HEIGHT = 20;
constexpr S32 ROW_GAP = 2;
constexpr S32 RESET_WIDTH = 20;
constexpr S32 GAP = 4;
/// Room for the scroll container's bar beside the rows.
constexpr S32 SCROLLBAR_ROOM = 20;
/// How often GPU times and values are read back when nothing said they changed.
constexpr F32 POLL_SECONDS = 0.5f;

const char* const ROW_TECHNIQUE = "t:";
const char* const ROW_FAILED = "e:";

const LLColor4 ERROR_COLOR(1.f, 0.45f, 0.4f, 1.f);
const LLColor4 WARNING_COLOR(1.f, 0.8f, 0.35f, 1.f);

/// Holds a flag true for a scope.
struct ScopedTrue
{
    explicit ScopedTrue(bool& flag) : mFlag(flag) { mFlag = true; }
    ~ScopedTrue() { mFlag = false; }
    ScopedTrue(const ScopedTrue&) = delete;
    ScopedTrue& operator=(const ScopedTrue&) = delete;
    bool& mFlag;
};

bool startsWith(const std::string& s, const char* prefix)
{
    return s.compare(0, strlen(prefix), prefix) == 0;
}

std::string lowered(std::string s)
{
    LLStringUtil::toLower(s);
    return s;
}

LLSD component(const LLSD& value, S32 i)
{
    if (value.isArray())
    {
        return i < (S32)value.size() ? value[i] : LLSD();
    }
    return i == 0 ? value : LLSD();
}

/// A range annotation's first component (ReShade allows a scalar or a vector).
F32 rangeValue(const LLSD& value, F32 fallback)
{
    const LLSD v = component(value, 0);
    return v.isDefined() ? (F32)v.asReal() : fallback;
}

/// Digits enough to show one step.
S32 digitsFor(F32 step, bool is_float)
{
    if (!is_float)
    {
        return 0;
    }
    if (step <= 0.f)
    {
        return 3;
    }
    const S32 digits = (S32)std::ceil(-std::log10(step) - 1e-4f);
    return llclamp(digits, 1, 6);
}

/// The master switch, read without a warning if the runtime's setting is
/// missing.
bool fxEnabled()
{
    const LLControlVariable* controlp = gSavedSettings.getControl("SLFxEnabled");
    return controlp && controlp->getValue().asBoolean();
}

bool isColor(const SLFxUniformInfo& uniform)
{
    return uniform.mType == "color3" || uniform.mType == "color4" ||
           (uniform.mUIType == "color" && SLFxPresets::componentKind(uniform) == SLFxPresets::COMP_FLOAT &&
            SLFxPresets::componentCount(uniform) >= 3);
}
} // namespace

SLPanelPhotoStudioEffects::SLPanelPhotoStudioEffects(SLFloaterPhotoStudio* studio)
:   LLPanel()
{
    if (studio)
    {
        mStudio = studio->getHandle();
    }
}

SLPanelPhotoStudioEffects::~SLPanelPhotoStudioEffects()
{
}

SLFloaterPhotoStudio* SLPanelPhotoStudioEffects::studio() const
{
    return dynamic_cast<SLFloaterPhotoStudio*>(mStudio.get());
}

bool SLPanelPhotoStudioEffects::postBuild()
{
    mPresetCombo = getChild<LLComboBox>("fx_preset");
    mPresetStatus = getChild<LLTextBox>("fx_preset_status");
    mSearch = getChild<LLSearchEditor>("fx_search");
    mList = getChild<LLScrollListCtrl>("fx_list");
    mEffectTitle = getChild<LLTextBox>("fx_effect_title");
    mResetEffect = getChild<LLButton>("fx_reset_effect");
    mScroll = getChild<LLScrollContainer>("fx_scroll");
    mControls = getChild<LLPanel>("fx_controls");

    getChild<LLUICtrl>("fx_reload")->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        SLFxStack::instance().reloadAll();
        mRuntimeChanged = true;
    });
    getChild<LLUICtrl>("fx_folder")->setCommitCallback([](LLUICtrl*, const LLSD&)
    {
        const std::string dir = SLFxStack::instance().getEffectsDir();
        if (!dir.empty())
        {
            LLFile::mkdir(dir);
            gDirUtilp->openDir(dir);
        }
    });
    getChild<LLUICtrl>("fx_preset_folder")->setCommitCallback([](LLUICtrl*, const LLSD&)
    {
        gDirUtilp->openDir(SLFxPresets::presetsDir());
    });
    getChild<LLUICtrl>("fx_move_up")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onMove(-1); });
    getChild<LLUICtrl>("fx_move_down")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onMove(1); });
    mResetEffect->setCommitCallback([this](LLUICtrl*, const LLSD&) { onResetEffect(); });

    mPresetCombo->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPresetPicked(); });
    // The list is read when it opens, so a preset dropped into the folder
    // shows up without reopening the page.
    mPresetCombo->setPrearrangeCallback([this](LLUICtrl*, const LLSD&) { refreshPresets(); });
    getChild<LLUICtrl>("fx_preset_save")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPresetSave(); });
    getChild<LLUICtrl>("fx_preset_delete")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPresetDelete(); });
    getChild<LLUICtrl>("fx_preset_import")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPresetImport(); });
    getChild<LLUICtrl>("fx_preset_export")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPresetExport(); });

    mSearch->setKeystrokeCallback([this](LLUICtrl*, const LLSD&) { onFilterChanged(); });
    mSearch->setCommitCallback([this](LLUICtrl*, const LLSD&) { onFilterChanged(); });

    mList->setCommitOnSelectionChange(true);
    mList->setCommitCallback([this](LLUICtrl*, const LLSD&) { onListSelect(); });

    // Change notices only raise flags: the work is done in draw(), never
    // inside the runtime's callback, which may be inside one of our writes.
    mRuntimeConnection = SLFxStack::instance().setChangedCallback([this]() { mRuntimeChanged = true; });
    SLFxPresets::instance().init();
    mPresetsConnection = SLFxPresets::instance().setChangedCallback([this]() { mPresetsChanged = true; });
    if (LLControlVariable* controlp = gSavedSettings.getControl("SLFxEnabled"))
    {
        mEnabledConnection = controlp->getSignal()->connect(
            [this](LLControlVariable*, const LLSD&, const LLSD&) { mRuntimeChanged = true; });
    }

    refreshPresets();
    refreshList(true);
    showEffect(std::string(), true);
    return LLPanel::postBuild();
}

void SLPanelPhotoStudioEffects::draw()
{
    if (mPresetsChanged)
    {
        mPresetsChanged = false;
        refreshPresets();
    }
    if (mRuntimeChanged || mPollTimer.getElapsedTimeF32() > POLL_SECONDS)
    {
        mRuntimeChanged = false;
        mPollTimer.reset();
        refreshList(false);
        refreshControlValues();
    }
    if (mControls && controlsWidth() != mBuiltWidth)
    {
        buildControls();
    }
    LLPanel::draw();
}

void SLPanelPhotoStudioEffects::reshape(S32 width, S32 height, bool called_from_parent)
{
    LLPanel::reshape(width, height, called_from_parent);
    // The rows are rebuilt at the new width on the next draw.
}

// ============================================================================
// Technique list
// ============================================================================

void SLPanelPhotoStudioEffects::onFilterChanged()
{
    const std::string filter = lowered(mSearch ? mSearch->getText() : std::string());
    if (filter != mFilter)
    {
        mFilter = filter;
        refreshList(true);
    }
}

void SLPanelPhotoStudioEffects::refreshList(bool force)
{
    if (!mList)
    {
        return;
    }
    SLFxStack& stack = SLFxStack::instance();
    const std::vector<SLFxTechniqueInfo> techs = stack.getTechniques();
    const std::vector<SLFxEffectInfo> effects = stack.getAvailableEffects();

    std::vector<const SLFxEffectInfo*> failed;
    for (const SLFxEffectInfo& effect : effects)
    {
        // Failed = an error, or not loaded once compiling is over (a pending compile is not a failure).
        if (!effect.mError.empty() || (!effect.mLoaded && !effect.mCompiling))
        {
            failed.push_back(&effect);
        }
    }

    std::string signature = mFilter + "\n";
    for (const SLFxTechniqueInfo& tech : techs)
    {
        signature += SLFxPresets::techniqueId(tech) + (tech.mEnabled ? "+" : "-") + "\n";
    }
    for (const SLFxEffectInfo* effect : failed)
    {
        signature += effect->mPath + "!" + effect->mError + "\n";
    }

    const std::string ms_error = getString("ms_error");
    auto ms_text = [](const SLFxTechniqueInfo& tech)
    {
        return (tech.mEnabled && tech.mGpuMs > 0.f) ? llformat("%.2f", tech.mGpuMs) : std::string();
    };

    if (!force && signature == mListSignature)
    {
        // Same rows: only the times move.
        for (const SLFxTechniqueInfo& tech : techs)
        {
            const std::string value = ROW_TECHNIQUE + SLFxPresets::techniqueId(tech);
            mRowTechs[value] = tech;
            if (LLScrollListItem* item = mList->getItem(LLSD(value)))
            {
                if (LLScrollListCell* cell = item->getColumn(2))
                {
                    const std::string text = ms_text(tech);
                    if (cell->getValue().asString() != text)
                    {
                        cell->setValue(text);
                    }
                }
            }
        }
        return;
    }
    mListSignature = signature;

    const LLSD selected = mList->getSelectedValue();
    const S32 scroll = mList->getScrollPos();
    mList->deleteAllItems();
    mRowTechs.clear();
    mRowFailed.clear();

    auto matches = [this](const std::string& a, const std::string& b, const std::string& c)
    {
        return mFilter.empty() || lowered(a).find(mFilter) != std::string::npos ||
               lowered(b).find(mFilter) != std::string::npos || lowered(c).find(mFilter) != std::string::npos;
    };

    for (const SLFxTechniqueInfo& tech : techs)
    {
        const std::string file = SLFxPresets::effectFileName(tech.mEffectPath);
        const std::string label = tech.mLabel.empty() ? tech.mName : tech.mLabel;
        if (!matches(tech.mName, label, file))
        {
            continue;
        }
        const std::string value = ROW_TECHNIQUE + SLFxPresets::techniqueId(tech);
        mRowTechs[value] = tech;

        std::string tooltip = label + " (" + file + ")";
        if (!tech.mTooltip.empty())
        {
            tooltip += "\n" + tech.mTooltip;
        }

        LLSD row;
        row["value"] = value;
        row["columns"][0]["column"] = "on";
        row["columns"][0]["type"] = "checkbox";
        row["columns"][0]["value"] = tech.mEnabled;
        row["columns"][1]["column"] = "name";
        row["columns"][1]["value"] = label;
        row["columns"][1]["tool_tip"] = tooltip;
        row["columns"][2]["column"] = "ms";
        row["columns"][2]["value"] = ms_text(tech);
        mList->addElement(row, [this](const LLSD& v, LLScrollListCell* cell) { onToggleTechnique(v, cell); },
                          ADD_BOTTOM);
    }

    for (const SLFxEffectInfo* effect : failed)
    {
        const std::string file = SLFxPresets::effectFileName(effect->mPath);
        if (!matches(file, effect->mName, effect->mError))
        {
            continue;
        }
        const std::string value = ROW_FAILED + effect->mPath;
        mRowFailed[value] = effect->mPath;

        LLSD row;
        row["value"] = value;
        row["columns"][0]["column"] = "on";
        row["columns"][0]["value"] = "";
        row["columns"][1]["column"] = "name";
        row["columns"][1]["value"] = file + " " + getString("failed_suffix");
        row["columns"][1]["color"] = ERROR_COLOR.getValue();
        row["columns"][1]["tool_tip"] = effect->mError.empty() ? file : effect->mError;
        row["columns"][2]["column"] = "ms";
        row["columns"][2]["value"] = ms_error;
        row["columns"][2]["color"] = ERROR_COLOR.getValue();
        mList->addElement(row, ADD_BOTTOM);
    }

    // A selection the new rows no longer have (a reload, or the filter hid
    // it) leaves its effect's controls up until something else is picked;
    // refreshControlValues drops them if the effect itself is gone.
    if (selected.isDefined())
    {
        mList->selectByValue(selected);
    }
    mList->setScrollPos(scroll);

    if (mEffectPath.empty())
    {
        // The empty-state message depends on what there is to list.
        buildControls();
    }
}

void SLPanelPhotoStudioEffects::onListSelect()
{
    showEffect(selectedEffectPath(), false);
}

std::string SLPanelPhotoStudioEffects::selectedEffectPath() const
{
    const LLScrollListItem* item = mList ? mList->getFirstSelected() : nullptr;
    if (!item)
    {
        return std::string();
    }
    const std::string value = item->getValue().asString();
    if (startsWith(value, ROW_TECHNIQUE))
    {
        const auto found = mRowTechs.find(value);
        return found != mRowTechs.end() ? found->second.mEffectPath : std::string();
    }
    if (startsWith(value, ROW_FAILED))
    {
        const auto found = mRowFailed.find(value);
        return found != mRowFailed.end() ? found->second : std::string();
    }
    return std::string();
}

void SLPanelPhotoStudioEffects::onToggleTechnique(const LLSD& row_value, LLScrollListCell* cell)
{
    if (!cell || mWriting)
    {
        return;
    }
    const auto found = mRowTechs.find(row_value.asString());
    if (found == mRowTechs.end())
    {
        return;
    }
    SLFxTechniqueInfo& tech = found->second;
    const bool on = cell->getValue().asBoolean();
    if (on == tech.mEnabled)
    {
        return;
    }
    const std::string key = SLFxPresets::techniqueKey(tech);
    {
        ScopedTrue writing(mWriting);
        SLFxStack::instance().setTechniqueEnabled(key, on);
    }
    if (SLFloaterPhotoStudio* floater = studio())
    {
        const std::string label = tech.mLabel.empty() ? tech.mName : tech.mLabel;
        floater->recordEffectChange(SLFloaterPhotoStudio::fxTechniqueKey(key), label, LLSD(tech.mEnabled), LLSD(on));
    }
    tech.mEnabled = on;
    SLFxPresets::instance().markDirty();
    mRuntimeChanged = true;
}

void SLPanelPhotoStudioEffects::onMove(S32 direction)
{
    const LLScrollListItem* item = mList ? mList->getFirstSelected() : nullptr;
    if (!item)
    {
        return;
    }
    const std::string value = item->getValue().asString();
    const auto found = mRowTechs.find(value);
    if (found == mRowTechs.end())
    {
        return;
    }
    const std::string id = SLFxPresets::techniqueId(found->second);

    // Moved past its neighbour in the whole list, filtered out or not: the
    // order is the runtime's, and the filter only hides rows.
    SLFxStack& stack = SLFxStack::instance();
    std::vector<SLFxTechniqueInfo> techs = stack.getTechniques();
    S32 index = -1;
    for (size_t i = 0; i < techs.size(); ++i)
    {
        if (SLFxPresets::techniqueId(techs[i]) == id)
        {
            index = (S32)i;
            break;
        }
    }
    const S32 other = index + direction;
    if (index < 0 || other < 0 || other >= (S32)techs.size())
    {
        return;
    }
    std::swap(techs[index], techs[other]);

    std::vector<std::string> order;
    for (const SLFxTechniqueInfo& tech : techs)
    {
        order.push_back(SLFxPresets::techniqueKey(tech));
    }
    {
        ScopedTrue writing(mWriting);
        stack.setTechniqueOrder(order);
    }
    SLFxPresets::instance().markDirty();
    refreshList(true);
    mList->selectByValue(LLSD(value));
    mList->scrollToShowSelected();
}

// ============================================================================
// Controls of the selected effect
// ============================================================================

S32 SLPanelPhotoStudioEffects::controlsWidth() const
{
    return mScroll ? llmax(120, mScroll->getRect().getWidth() - SCROLLBAR_ROOM) : 120;
}

void SLPanelPhotoStudioEffects::showEffect(const std::string& path, bool force)
{
    if (path == mEffectPath && !force)
    {
        return;
    }
    mEffectPath = path;
    buildControls();
    if (mScroll)
    {
        mScroll->goToTop();
    }
}

void SLPanelPhotoStudioEffects::clearControls()
{
    if (!mControls)
    {
        return;
    }
    if (gFocusMgr.childHasKeyboardFocus(mControls))
    {
        gFocusMgr.setKeyboardFocus(nullptr);
    }
    if (gFocusMgr.childHasMouseCapture(mControls))
    {
        gFocusMgr.setMouseCapture(nullptr);
    }
    for (Row& row : mRows)
    {
        if (row.mView)
        {
            mControls->removeChild(row.mView);
            delete row.mView;
        }
    }
    mRows.clear();
}

void SLPanelPhotoStudioEffects::buildControls()
{
    if (!mControls)
    {
        return;
    }
    clearControls();
    mUniforms.clear();
    mEffectError.clear();
    mBuiltWidth = controlsWidth();

    SLFxStack& stack = SLFxStack::instance();

    if (mEffectPath.empty())
    {
        mEffectTitle->setText(LLStringUtil::null);
        mResetEffect->setEnabled(false);
        std::string message;
        if (mList && mList->getItemCount() > 0)
        {
            message = getString("select_effect");
        }
        else if (!fxEnabled() && stack.getAvailableEffects().empty())
        {
            message = getString("effects_off");
        }
        else
        {
            message = getString("no_effects") + "\n" + stack.getEffectsDir();
        }
        addMessageRow(message, false);
        layoutControls();
        return;
    }

    bool loaded = false;
    bool known = false;
    for (const SLFxEffectInfo& effect : stack.getAvailableEffects())
    {
        if (effect.mPath == mEffectPath)
        {
            known = true;
            loaded = effect.mLoaded;
            mEffectError = effect.mError;
            break;
        }
    }

    const std::string file = SLFxPresets::effectFileName(mEffectPath);
    if (!loaded && known)
    {
        LLStringUtil::format_map_t args;
        args["[FILE]"] = file;
        mEffectTitle->setText(getString("failed_title", args));
    }
    else
    {
        mEffectTitle->setText(file);
    }
    mEffectTitle->setToolTip(mEffectPath);

    if (!mEffectError.empty())
    {
        // Compile errors and warnings, as the compiler said them.
        addMessageRow(mEffectError, !loaded);
    }

    if (loaded)
    {
        mUniforms = stack.getUniforms(mEffectPath);

        // Categories in the order they first appear, which is how ReShade
        // lays them out; uniforms keep their order within each.
        std::vector<std::string> categories;
        for (const SLFxUniformInfo& uniform : mUniforms)
        {
            if (!uniform.mHidden &&
                std::find(categories.begin(), categories.end(), uniform.mCategory) == categories.end())
            {
                categories.push_back(uniform.mCategory);
            }
        }
        for (const std::string& category : categories)
        {
            if (!category.empty())
            {
                addHeaderRow(category);
            }
            for (size_t i = 0; i < mUniforms.size(); ++i)
            {
                if (!mUniforms[i].mHidden && mUniforms[i].mCategory == category)
                {
                    addUniformRow((S32)i);
                }
            }
        }
        if (categories.empty() && mEffectError.empty())
        {
            addMessageRow(getString("no_settings"), false);
        }
    }

    mResetEffect->setEnabled(!mUniforms.empty());
    layoutControls();
}

void SLPanelPhotoStudioEffects::layoutControls()
{
    if (!mControls)
    {
        return;
    }
    const S32 width = controlsWidth();
    auto shown = [this](const Row& row)
    {
        return row.mKind == Row::HEADER || row.mCategory.empty() ||
               !mCollapsed.count(mEffectPath + "|" + row.mCategory);
    };

    S32 total = ROW_GAP;
    for (const Row& row : mRows)
    {
        if (shown(row))
        {
            total += row.mHeight + ROW_GAP;
        }
    }
    const S32 height = llmax(total, 1);
    mControls->reshape(width, height);

    S32 top = height - ROW_GAP;
    for (Row& row : mRows)
    {
        if (!row.mView)
        {
            continue;
        }
        if (!shown(row))
        {
            row.mView->setVisible(false);
            continue;
        }
        row.mView->setVisible(true);
        row.mView->setRect(LLRect(0, top, row.mView->getRect().getWidth(), top - row.mHeight));
        top -= row.mHeight + ROW_GAP;

        if (row.mKind == Row::HEADER)
        {
            if (LLButton* button = dynamic_cast<LLButton*>(row.mView))
            {
                const bool collapsed = mCollapsed.count(mEffectPath + "|" + row.mCategory) != 0;
                button->setImageOverlay(collapsed ? "Accordion_ArrowClosed_Off" : "Accordion_ArrowOpened_Off",
                                        LLFontGL::LEFT);
            }
        }
    }
}

void SLPanelPhotoStudioEffects::onToggleCategory(const std::string& category)
{
    const std::string key = mEffectPath + "|" + category;
    if (!mCollapsed.erase(key))
    {
        mCollapsed.insert(key);
    }
    layoutControls();
}

void SLPanelPhotoStudioEffects::addMessageRow(const std::string& text, bool error)
{
    const S32 width = controlsWidth();
    LLTextBox::Params tp(LLUICtrlFactory::getDefaultParams<LLTextBox>());
    tp.name("fx_message");
    tp.rect(LLRect(GAP, 400, width - GAP, 0));
    tp.wrap(true);
    tp.initial_value(text);
    tp.follows.flags(FOLLOWS_LEFT | FOLLOWS_TOP);
    if (error)
    {
        tp.text_color(LLUIColor(ERROR_COLOR));
        tp.text_readonly_color(LLUIColor(ERROR_COLOR));
    }
    LLTextBox* box = LLUICtrlFactory::create<LLTextBox>(tp);
    mControls->addChild(box);
    const S32 height = llmax(16, box->getTextPixelHeight() + 4);
    box->reshape(width - 2 * GAP, height);

    Row row;
    row.mKind = Row::MESSAGE;
    row.mView = box;
    row.mHeight = height;
    mRows.push_back(row);
}

void SLPanelPhotoStudioEffects::addHeaderRow(const std::string& category)
{
    const S32 width = controlsWidth();
    LLButton::Params bp(LLUICtrlFactory::getDefaultParams<LLButton>());
    bp.name("fx_category");
    bp.label(category);
    bp.rect(LLRect(0, HEADER_HEIGHT, width, 0));
    bp.font_halign(LLFontGL::LEFT);
    bp.pad_left(22);
    bp.follows.flags(FOLLOWS_LEFT | FOLLOWS_TOP);
    bp.click_callback.function([this, category](LLUICtrl*, const LLSD&) { onToggleCategory(category); });
    LLButton* button = LLUICtrlFactory::create<LLButton>(bp);
    mControls->addChild(button);

    Row row;
    row.mKind = Row::HEADER;
    row.mView = button;
    row.mCategory = category;
    row.mHeight = HEADER_HEIGHT;
    mRows.push_back(row);
}

void SLPanelPhotoStudioEffects::addUniformRow(S32 index)
{
    const SLFxUniformInfo& uniform = mUniforms[index];
    const S32 width = controlsWidth();
    const S32 label_width = llclamp(width * 2 / 5, 80, 170);
    const S32 edit_right = width - RESET_WIDTH - GAP;
    const size_t row_index = mRows.size();

    const SLFxPresets::EComponent kind = SLFxPresets::componentKind(uniform);
    const S32 count = SLFxPresets::componentCount(uniform);
    const bool is_float = kind == SLFxPresets::COMP_FLOAT;
    const std::string label = uniform.mLabel.empty() ? uniform.mName : uniform.mLabel;
    const std::string tooltip = uniform.mTooltip.empty() ? uniform.mName : uniform.mTooltip + "\n(" + uniform.mName + ")";

    Row row;
    row.mUniform = index;
    row.mCategory = uniform.mCategory;
    if (kind == SLFxPresets::COMP_BOOL)
    {
        row.mKind = Row::CHECK;
    }
    else if (isColor(uniform))
    {
        row.mKind = Row::COLOR;
    }
    else if (count > 1)
    {
        row.mKind = Row::VECTOR;
    }
    else if ((uniform.mUIType == "combo" || uniform.mUIType == "list" || uniform.mUIType == "radio") &&
             !uniform.mItems.empty())
    {
        row.mKind = Row::COMBO;
    }
    else if (uniform.mUIType == "slider")
    {
        row.mKind = Row::SLIDER;
    }
    else
    {
        row.mKind = Row::SPIN;
    }
    row.mHeight = row.mKind == Row::COLOR ? COLOR_ROW_HEIGHT : ROW_HEIGHT;
    const S32 h = row.mHeight;

    // Ranges. A slider needs both ends; a spinner without them is open.
    const bool has_min = component(uniform.mMin, 0).isDefined();
    const bool has_max = component(uniform.mMax, 0).isDefined();
    F32 min_value = rangeValue(uniform.mMin, row.mKind == Row::SLIDER ? 0.f : -100000.f);
    F32 max_value = rangeValue(uniform.mMax, row.mKind == Row::SLIDER ? (is_float ? 1.f : 100.f) : 100000.f);
    if (max_value < min_value)
    {
        std::swap(min_value, max_value);
    }
    F32 step = rangeValue(uniform.mStep, 0.f);
    if (step <= 0.f)
    {
        if (!is_float)
        {
            step = 1.f;
        }
        else if (has_min && has_max && max_value > min_value)
        {
            step = (max_value - min_value) / 100.f;
        }
        else
        {
            step = 0.01f;
        }
    }
    const S32 digits = digitsFor(step, is_float);

    LLPanel::Params pp;
    pp.name("fx_row");
    pp.rect(LLRect(0, h, width, 0));
    pp.background_visible(false);
    pp.mouse_opaque(false);
    pp.follows.flags(FOLLOWS_LEFT | FOLLOWS_TOP);
    LLPanel* panel = LLUICtrlFactory::create<LLPanel>(pp);
    mControls->addChild(panel);
    row.mView = panel;

    auto commit = [this, row_index](LLUICtrl*, const LLSD&) { onRowCommit(row_index); };

    auto add_label = [&]()
    {
        LLTextBox::Params tp(LLUICtrlFactory::getDefaultParams<LLTextBox>());
        tp.name("fx_label");
        tp.rect(LLRect(2, h - 3, label_width, 2));
        tp.initial_value(label);
        tp.tool_tip(tooltip);
        tp.use_ellipses(true);
        panel->addChild(LLUICtrlFactory::create<LLTextBox>(tp));
    };

    auto make_spinner = [&](const std::string& name, const std::string& spin_label, S32 left, S32 right)
    {
        LLSpinCtrl::Params sp(LLUICtrlFactory::getDefaultParams<LLSpinCtrl>());
        sp.name(name);
        sp.label(spin_label);
        sp.label_width(spin_label.empty() ? 0 : label_width - 2);
        sp.rect(LLRect(left, h - 1, right, 1));
        sp.min_value(min_value);
        sp.max_value(max_value);
        sp.increment(step);
        sp.decimal_digits((U32)digits);
        sp.tool_tip(tooltip);
        sp.commit_callback.function(commit);
        LLSpinCtrl* spin = LLUICtrlFactory::create<LLSpinCtrl>(sp);
        panel->addChild(spin);
        return spin;
    };

    switch (row.mKind)
    {
        case Row::CHECK:
        {
            LLCheckBoxCtrl::Params cp(LLUICtrlFactory::getDefaultParams<LLCheckBoxCtrl>());
            cp.name("fx_value");
            cp.label(label);
            cp.rect(LLRect(2, h - 2, edit_right, 2));
            cp.tool_tip(tooltip);
            cp.commit_callback.function(commit);
            LLCheckBoxCtrl* check = LLUICtrlFactory::create<LLCheckBoxCtrl>(cp);
            panel->addChild(check);
            row.mCtrls.push_back(check);
            break;
        }
        case Row::SLIDER:
        {
            LLSliderCtrl::Params sp(LLUICtrlFactory::getDefaultParams<LLSliderCtrl>());
            sp.name("fx_value");
            sp.label(label);
            sp.label_width(label_width);
            sp.rect(LLRect(2, h - 1, edit_right, 1));
            sp.min_value(min_value);
            sp.max_value(max_value);
            sp.increment(step);
            sp.decimal_digits(digits);
            sp.show_text(true);
            sp.can_edit_text(true);
            sp.text_width(48);
            sp.tool_tip(tooltip);
            sp.commit_callback.function(commit);
            LLSliderCtrl* slider = LLUICtrlFactory::create<LLSliderCtrl>(sp);
            panel->addChild(slider);
            row.mCtrls.push_back(slider);
            break;
        }
        case Row::SPIN:
            row.mCtrls.push_back(make_spinner("fx_value", label, 2, llmin(edit_right, label_width + 100)));
            break;
        case Row::COMBO:
        {
            add_label();
            LLComboBox::Params cbp(LLUICtrlFactory::getDefaultParams<LLComboBox>());
            cbp.name("fx_value");
            cbp.rect(LLRect(label_width + GAP, h - 1, edit_right, 1));
            cbp.tool_tip(tooltip);
            cbp.commit_callback.function(commit);
            LLComboBox* combo = LLUICtrlFactory::create<LLComboBox>(cbp);
            for (size_t i = 0; i < uniform.mItems.size(); ++i)
            {
                combo->add(uniform.mItems[i], LLSD((S32)i));
            }
            panel->addChild(combo);
            row.mCtrls.push_back(combo);
            break;
        }
        case Row::VECTOR:
        {
            add_label();
            const S32 left = label_width + GAP;
            const S32 each = llmax(30, (edit_right - left) / count);
            for (S32 i = 0; i < count; ++i)
            {
                row.mCtrls.push_back(make_spinner(llformat("fx_value_%d", i), std::string(),
                                                  left + i * each, left + (i + 1) * each - 2));
            }
            break;
        }
        case Row::COLOR:
        {
            add_label();
            LLColorSwatchCtrl::Params csp(LLUICtrlFactory::getDefaultParams<LLColorSwatchCtrl>());
            csp.name("fx_value");
            csp.rect(LLRect(label_width + GAP, h - 1, label_width + GAP + 50, 1));
            csp.label_height(0);
            csp.can_apply_immediately(true);
            csp.tool_tip(tooltip);
            csp.commit_callback.function(commit);
            LLColorSwatchCtrl* swatch = LLUICtrlFactory::create<LLColorSwatchCtrl>(csp);
            panel->addChild(swatch);
            row.mCtrls.push_back(swatch);
            if (count >= 4)
            {
                LLSpinCtrl::Params sp(LLUICtrlFactory::getDefaultParams<LLSpinCtrl>());
                sp.name("fx_alpha");
                sp.label("A");
                sp.label_width(12);
                sp.rect(LLRect(label_width + GAP + 56, h - 3, llmin(edit_right, label_width + GAP + 56 + 80), 3));
                sp.min_value(0.f);
                sp.max_value(1.f);
                sp.increment(0.01f);
                sp.decimal_digits(2);
                sp.tool_tip(tooltip);
                sp.commit_callback.function(commit);
                row.mAlpha = LLUICtrlFactory::create<LLSpinCtrl>(sp);
                panel->addChild(row.mAlpha);
            }
            break;
        }
        default:
            break;
    }

    LLButton::Params bp(LLUICtrlFactory::getDefaultParams<LLButton>());
    bp.name("fx_reset");
    bp.label(std::string());
    bp.rect(LLRect(width - RESET_WIDTH, h - 1, width, 1));
    bp.tool_tip(getString("reset_tooltip"));
    bp.click_callback.function([this, row_index](LLUICtrl*, const LLSD&) { onRowReset(row_index); });
    LLButton* reset = LLUICtrlFactory::create<LLButton>(bp);
    reset->setImageOverlay("Refresh_Off", LLFontGL::HCENTER);
    panel->addChild(reset);

    mRows.push_back(row);
    writeRow(mRows.back(), uniform.mValue);
}

LLSD SLPanelPhotoStudioEffects::readRow(const Row& row) const
{
    if (row.mUniform < 0 || row.mUniform >= (S32)mUniforms.size() || row.mCtrls.empty())
    {
        return LLSD();
    }
    const SLFxUniformInfo& uniform = mUniforms[row.mUniform];
    const SLFxPresets::EComponent kind = SLFxPresets::componentKind(uniform);
    auto number = [kind](F64 v) { return kind == SLFxPresets::COMP_INT ? LLSD((S32)llround((F32)v)) : LLSD(v); };

    switch (row.mKind)
    {
        case Row::CHECK:
            return LLSD(row.mCtrls[0]->getValue().asBoolean());
        case Row::SLIDER:
        case Row::SPIN:
            return number(row.mCtrls[0]->getValue().asReal());
        case Row::COMBO:
            return number((F64)row.mCtrls[0]->getValue().asInteger());
        case Row::VECTOR:
        {
            LLSD out = LLSD::emptyArray();
            for (LLUICtrl* ctrl : row.mCtrls)
            {
                out.append(number(ctrl->getValue().asReal()));
            }
            return out;
        }
        case Row::COLOR:
        {
            LLColorSwatchCtrl* swatch = dynamic_cast<LLColorSwatchCtrl*>(row.mCtrls[0]);
            if (!swatch)
            {
                return LLSD();
            }
            const LLColor4 color = swatch->get();
            LLSD out = LLSD::emptyArray();
            out.append(LLSD((F64)color.mV[VRED]));
            out.append(LLSD((F64)color.mV[VGREEN]));
            out.append(LLSD((F64)color.mV[VBLUE]));
            if (SLFxPresets::componentCount(uniform) >= 4)
            {
                out.append(LLSD(row.mAlpha ? row.mAlpha->getValue().asReal() : component(uniform.mValue, 3).asReal()));
            }
            return out;
        }
        default:
            return LLSD();
    }
}

void SLPanelPhotoStudioEffects::writeRow(const Row& row, const LLSD& value)
{
    if (row.mCtrls.empty())
    {
        return;
    }
    ScopedTrue writing(mWriting);
    switch (row.mKind)
    {
        case Row::CHECK:
            row.mCtrls[0]->setValue(LLSD(component(value, 0).asBoolean()));
            break;
        case Row::SLIDER:
        case Row::SPIN:
            row.mCtrls[0]->setValue(LLSD(component(value, 0).asReal()));
            break;
        case Row::COMBO:
            row.mCtrls[0]->setValue(LLSD(component(value, 0).asInteger()));
            break;
        case Row::VECTOR:
            for (size_t i = 0; i < row.mCtrls.size(); ++i)
            {
                row.mCtrls[i]->setValue(LLSD(component(value, (S32)i).asReal()));
            }
            break;
        case Row::COLOR:
            if (LLColorSwatchCtrl* swatch = dynamic_cast<LLColorSwatchCtrl*>(row.mCtrls[0]))
            {
                swatch->set(LLColor4((F32)component(value, 0).asReal(), (F32)component(value, 1).asReal(),
                                     (F32)component(value, 2).asReal(), 1.f),
                            true);
            }
            if (row.mAlpha)
            {
                row.mAlpha->setValue(LLSD(component(value, 3).asReal()));
            }
            break;
        default:
            break;
    }
}

std::string SLPanelPhotoStudioEffects::captionOf(S32 uniform) const
{
    const SLFxUniformInfo& info = mUniforms[uniform];
    return SLFxPresets::effectFileName(mEffectPath) + ": " + (info.mLabel.empty() ? info.mName : info.mLabel);
}

void SLPanelPhotoStudioEffects::onRowCommit(size_t row_index)
{
    if (mWriting || row_index >= mRows.size())
    {
        return;
    }
    const Row& row = mRows[row_index];
    if (row.mUniform < 0)
    {
        return;
    }
    commitUniform(row.mUniform, readRow(row));
}

void SLPanelPhotoStudioEffects::onRowReset(size_t row_index)
{
    if (row_index >= mRows.size() || mRows[row_index].mUniform < 0)
    {
        return;
    }
    const S32 uniform = mRows[row_index].mUniform;
    commitUniform(uniform, mUniforms[uniform].mDefault);
    writeRow(mRows[row_index], mUniforms[uniform].mValue);
}

void SLPanelPhotoStudioEffects::onResetEffect()
{
    if (mUniforms.empty())
    {
        return;
    }
    SLFloaterPhotoStudio* floater = studio();
    if (floater)
    {
        LLStringUtil::format_map_t args;
        args["[EFFECT]"] = SLFxPresets::effectFileName(mEffectPath);
        floater->beginHistoryGroup(getString("reset_effect_step", args));
    }
    // Hidden ones too: a reset puts the effect back as its author shipped it.
    for (size_t i = 0; i < mUniforms.size(); ++i)
    {
        commitUniform((S32)i, mUniforms[i].mDefault);
    }
    if (floater)
    {
        floater->endHistoryGroup();
    }
    for (const Row& row : mRows)
    {
        if (row.mUniform >= 0)
        {
            writeRow(row, mUniforms[row.mUniform].mValue);
        }
    }
}

void SLPanelPhotoStudioEffects::commitUniform(S32 uniform, const LLSD& value)
{
    if (uniform < 0 || uniform >= (S32)mUniforms.size() || !value.isDefined())
    {
        return;
    }
    SLFxUniformInfo& info = mUniforms[uniform];
    if (SLFxPresets::valuesEqual(info, value, info.mValue))
    {
        return;
    }
    const LLSD before = info.mValue;
    {
        ScopedTrue writing(mWriting);
        SLFxStack::instance().setUniform(mEffectPath, info.mName, value);
    }
    info.mValue = value;
    if (SLFloaterPhotoStudio* floater = studio())
    {
        floater->recordEffectChange(SLFloaterPhotoStudio::fxUniformKey(mEffectPath, info.mName), captionOf(uniform),
                                    before, value);
    }
    SLFxPresets::instance().markDirty();
}

void SLPanelPhotoStudioEffects::refreshControlValues()
{
    if (mEffectPath.empty())
    {
        if (mRows.empty())
        {
            buildControls();
        }
        return;
    }

    SLFxStack& stack = SLFxStack::instance();
    std::string error;
    bool loaded = false;
    bool known = false;
    for (const SLFxEffectInfo& effect : stack.getAvailableEffects())
    {
        if (effect.mPath == mEffectPath)
        {
            known = true;
            loaded = effect.mLoaded;
            error = effect.mError;
            break;
        }
    }
    if (!known)
    {
        // Gone (deleted, or a reload renamed it).
        showEffect(std::string(), true);
        return;
    }

    const std::vector<SLFxUniformInfo> fresh = loaded ? stack.getUniforms(mEffectPath) : std::vector<SLFxUniformInfo>();
    bool same_shape = error == mEffectError && fresh.size() == mUniforms.size();
    for (size_t i = 0; same_shape && i < fresh.size(); ++i)
    {
        same_shape = fresh[i].mName == mUniforms[i].mName && fresh[i].mType == mUniforms[i].mType &&
                     fresh[i].mUIType == mUniforms[i].mUIType && fresh[i].mHidden == mUniforms[i].mHidden &&
                     fresh[i].mCategory == mUniforms[i].mCategory && fresh[i].mItems == mUniforms[i].mItems;
    }
    if (!same_shape)
    {
        // Recompiled into something else: start over (from draw, never from
        // a widget's own callback).
        buildControls();
        return;
    }

    for (size_t i = 0; i < fresh.size(); ++i)
    {
        if (SLFxPresets::valuesEqual(fresh[i], fresh[i].mValue, mUniforms[i].mValue))
        {
            continue;
        }
        mUniforms[i] = fresh[i];
        for (const Row& row : mRows)
        {
            if (row.mUniform == (S32)i)
            {
                writeRow(row, fresh[i].mValue);
            }
        }
    }
}

// ============================================================================
// Presets
// ============================================================================

void SLPanelPhotoStudioEffects::refreshPresets()
{
    if (!mPresetCombo)
    {
        return;
    }
    SLFxPresets& presets = SLFxPresets::instance();
    // Leave a name being typed alone.
    if (!mPresetCombo->hasFocus())
    {
        mPresetCombo->removeall();
        for (const std::string& name : presets.listPresets())
        {
            mPresetCombo->add(name, LLSD(name));
        }
        const std::string active = presets.getActive();
        if (!mPresetCombo->selectByValue(LLSD(active)))
        {
            mPresetCombo->setTextEntry(active);
        }
    }
    refreshPresetStatus();
}

void SLPanelPhotoStudioEffects::refreshPresetStatus()
{
    if (!mPresetStatus)
    {
        return;
    }
    SLFxPresets& presets = SLFxPresets::instance();
    const SLFxPresetReport& report = presets.lastReport();

    std::string text;
    LLColor4 color = LLUIColorTable::instance().getColor("LabelTextColor");
    std::string details;
    if (report.failed())
    {
        text = report.summary();
        color = ERROR_COLOR;
    }
    else if (report.hasWarnings())
    {
        text = report.summary();
        color = WARNING_COLOR;
        auto add = [&details](const std::vector<std::string>& items)
        {
            for (const std::string& item : items)
            {
                details += item + "\n";
            }
        };
        add(report.mMissingTechniques);
        add(report.mMissingEffects);
        add(report.mUnknownUniforms);
        add(report.mBadValues);
        add(report.mIgnoredDefinitions);
    }
    else if (presets.isDirty())
    {
        text = getString(gSavedSettings.getBOOL("SLFxAutoSavePreset") ? "status_autosave" : "status_dirty");
    }
    mPresetStatus->setText(text);
    mPresetStatus->setColor(color);
    mPresetStatus->setReadOnlyColor(color);
    mPresetStatus->setToolTip(details.empty() ? text : details);
}

void SLPanelPhotoStudioEffects::onPresetPicked()
{
    const std::string name = mPresetCombo->getSimple();
    SLFxPresets& presets = SLFxPresets::instance();
    if (!presets.exists(name))
    {
        // A new name being typed: Save makes it.
        return;
    }
    if (name == presets.getActive())
    {
        // Picking the preset in use again goes back to what it says.
        presets.load(name);
    }
    else
    {
        // Through the setting, so Preferences shows it and unsaved edits are
        // auto-saved to the preset being left (SLFxPresets).
        gSavedSettings.setString("SLFxActivePreset", name);
    }
    mRuntimeChanged = true;
}

void SLPanelPhotoStudioEffects::onPresetSave()
{
    std::string name = mPresetCombo->getSimple();
    LLStringUtil::trim(name);
    SLFxPresets& presets = SLFxPresets::instance();
    if (!SLFxPresets::isValidName(name))
    {
        mPresetStatus->setText(getString("status_bad_name"));
        mPresetStatus->setColor(ERROR_COLOR);
        mPresetStatus->setReadOnlyColor(ERROR_COLOR);
        return;
    }
    if (presets.exists(name) && name != presets.getActive())
    {
        LLStringUtil::format_map_t args;
        args["[NAME]"] = name;
        LLNotificationsUtil::add("GenericAlertYesCancel", LLSD().with("MESSAGE", getString("confirm_overwrite", args)),
                                 LLSD(), [name](const LLSD& notification, const LLSD& response)
        {
            if (LLNotificationsUtil::getSelectedOption(notification, response) == 0 && SLFxPresets::instanceExists())
            {
                SLFxPresets::instance().save(name);
            }
            return false;
        });
        return;
    }
    presets.save(name);
}

void SLPanelPhotoStudioEffects::onPresetDelete()
{
    const std::string name = mPresetCombo->getSimple();
    if (!SLFxPresets::instance().exists(name))
    {
        return;
    }
    LLStringUtil::format_map_t args;
    args["[NAME]"] = name;
    LLNotificationsUtil::add("GenericAlertYesCancel", LLSD().with("MESSAGE", getString("confirm_delete", args)),
                             LLSD(), [name](const LLSD& notification, const LLSD& response)
    {
        if (LLNotificationsUtil::getSelectedOption(notification, response) == 0 && SLFxPresets::instanceExists())
        {
            SLFxPresets::instance().remove(name);
        }
        return false;
    });
}

void SLPanelPhotoStudioEffects::onPresetImport()
{
    // No .ini filter exists in LLFilePicker; the import reads anything and
    // says what it could not use.
    LLFilePickerReplyThread::startPicker(
        [](const std::vector<std::string>& files, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter)
        {
            if (!files.empty() && SLFxPresets::instanceExists())
            {
                std::string name;
                SLFxPresets::instance().importFile(files.front(), name);
            }
        },
        LLFilePicker::FFLOAD_ALL, false);
}

void SLPanelPhotoStudioEffects::onPresetExport()
{
    std::string proposed = SLFxPresets::instance().getActive();
    if (!SLFxPresets::isValidName(proposed))
    {
        proposed = "ReShadePreset";
    }
    LLFilePickerReplyThread::startPicker(
        [](const std::vector<std::string>& files, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter)
        {
            if (!files.empty() && SLFxPresets::instanceExists())
            {
                SLFxPresets::instance().exportTo(files.front());
            }
        },
        LLFilePicker::FFSAVE_ALL, proposed + ".ini");
}
