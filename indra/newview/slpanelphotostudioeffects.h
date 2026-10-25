/**
 * @file slpanelphotostudioeffects.h
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

#ifndef SL_PANELPHOTOSTUDIOEFFECTS_H
#define SL_PANELPHOTOSTUDIOEFFECTS_H

#include "llpanel.h"
#include "llframetimer.h"

#include "slfxstack.h"

#include <map>
#include <set>
#include <string>
#include <vector>

class LLButton;
class LLComboBox;
class LLScrollContainer;
class LLScrollListCell;
class LLScrollListCtrl;
class LLSearchEditor;
class LLTextBox;
class SLFloaterPhotoStudio;

/**
 * The Effects section (panel_photo_studio_effects.xml).
 *
 * @par Responsiveness with big packs
 * Only the selected effect has controls: they are made when it is selected
 * and dropped when another is. The technique list is rebuilt only when the
 * set of techniques, their order or their switches change; GPU times and
 * values are updated in place, on a timer, never per frame.
 *
 * @par How annotations become widgets
 * - bool: a checkbox.
 * - int/float with ui_type combo, list or radio and ui_items: a dropdown.
 * - int/float with ui_type slider: a slider with its number.
 * - int/float with ui_type drag, input or none: a spinner.
 * - float2..4 (not colours): a row of spinners.
 * - color3/color4, or ui_type color: a swatch (plus an alpha spinner).
 * ui_label, ui_tooltip, ui_min/max/step become the label, tooltip and range;
 * ui_category groups rows under headers that fold; hidden uniforms are not
 * shown. Every row has a reset button, and the effect a Reset button.
 *
 * Everything the runtime is asked is asked of SLFxStack; this keeps no state
 * the runtime does not have, beyond a copy of the selected effect's uniforms
 * to compare against.
 */
class SLPanelPhotoStudioEffects final : public LLPanel
{
public:
    explicit SLPanelPhotoStudioEffects(SLFloaterPhotoStudio* studio = nullptr);
    ~SLPanelPhotoStudioEffects() override;

    bool postBuild() override;
    void draw() override;
    void reshape(S32 width, S32 height, bool called_from_parent = true) override;

private:
    SLFloaterPhotoStudio* studio() const;

    // --- Technique list ---
    /// Rebuild the list if what it shows has changed (or @a force), else
    /// update its switches and times in place.
    void refreshList(bool force);
    void onFilterChanged();
    void onListSelect();
    void onToggleTechnique(const LLSD& row_value, LLScrollListCell* cell);
    void onMove(S32 direction);
    /// The effect the selected row belongs to, or "".
    std::string selectedEffectPath() const;

    // --- Controls of the selected effect ---
    struct Row
    {
        enum EKind
        {
            HEADER,
            MESSAGE,
            CHECK,
            SLIDER,
            SPIN,
            COMBO,
            VECTOR,
            COLOR
        };
        EKind                   mKind = MESSAGE;
        LLView*                 mView = nullptr;  // the row's own panel, or the header button
        std::string             mCategory;
        S32                     mUniform = -1;    // index into mUniforms
        S32                     mHeight = 0;
        std::vector<LLUICtrl*>  mCtrls;           // the editing widgets, component order
        LLUICtrl*               mAlpha = nullptr; // a colour's alpha spinner
    };

    /// Show @a path's controls (rebuilding only when it changes, or @a force).
    void showEffect(const std::string& path, bool force);
    void clearControls();
    void buildControls();
    /// Place the rows top-down, folding collapsed categories, and size the
    /// scrolled panel to fit.
    void layoutControls();
    /// Put values the runtime holds now into the widgets, where they differ.
    void refreshControlValues();
    S32  controlsWidth() const;

    void addMessageRow(const std::string& text, bool error);
    void addHeaderRow(const std::string& category);
    void addUniformRow(S32 uniform);

    LLSD readRow(const Row& row) const;
    void writeRow(const Row& row, const LLSD& value);
    void onRowCommit(size_t row);
    void onRowReset(size_t row);
    void onResetEffect();
    void onToggleCategory(const std::string& category);
    /// Set one uniform, for undo and for the preset.
    void commitUniform(S32 uniform, const LLSD& value);
    std::string captionOf(S32 uniform) const;

    // --- Presets ---
    void refreshPresets();
    void refreshPresetStatus();
    void onPresetPicked();
    void onPresetSave();
    void onPresetDelete();
    void onPresetImport();
    void onPresetExport();

    LLHandle<LLFloater> mStudio;

    LLComboBox* mPresetCombo = nullptr;
    LLTextBox* mPresetStatus = nullptr;
    LLSearchEditor* mSearch = nullptr;
    LLScrollListCtrl* mList = nullptr;
    LLTextBox* mEffectTitle = nullptr;
    LLButton* mResetEffect = nullptr;
    LLScrollContainer* mScroll = nullptr;
    LLPanel* mControls = nullptr;

    boost::signals2::scoped_connection mRuntimeConnection;
    boost::signals2::scoped_connection mPresetsConnection;
    boost::signals2::scoped_connection mEnabledConnection;

    /// What the list last showed: every row's id and switch, in order, and
    /// the filter. A change rebuilds it.
    std::string mListSignature;
    std::string mFilter;
    /// The list rows, by row value: techniques ("t:Name@File.fx") and effects
    /// that failed to compile ("e:<path>").
    std::map<std::string, SLFxTechniqueInfo> mRowTechs;
    std::map<std::string, std::string> mRowFailed;

    /// The effect whose controls are shown, its uniforms as last read, and
    /// its rows.
    std::string mEffectPath;
    std::string mEffectError;
    std::vector<SLFxUniformInfo> mUniforms;
    std::vector<Row> mRows;
    /// Folded categories, as "<effect path>|<category>"; for the session.
    std::set<std::string> mCollapsed;
    S32 mBuiltWidth = 0;

    /// The runtime said something changed: refresh on the next draw (never
    /// from inside its callback, which may be inside one of our own writes).
    bool mRuntimeChanged = true;
    bool mPresetsChanged = true;
    /// Set while this panel writes to the runtime or its widgets.
    bool mWriting = false;
    LLFrameTimer mPollTimer;
};

#endif // SL_PANELPHOTOSTUDIOEFFECTS_H
