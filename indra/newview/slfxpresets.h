/**
 * @file slfxpresets.h
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

#ifndef SL_FXPRESETS_H
#define SL_FXPRESETS_H

#include "llsingleton.h"
#include "llsd.h"
#include "slfxstack.h" // SLFxTechniqueInfo, SLFxUniformInfo (the FX runtime, #544)

#include <boost/signals2.hpp>

#include <functional>
#include <string>
#include <vector>

class SLFxIniDocument;

/// What applying a preset could not do. None of it is fatal: a preset made on
/// a machine with more effects installed still loads everything it can, and
/// writing it back keeps what we could not use (see SLFxPresets::save).
struct SLFxPresetReport
{
    /// The file could not be read or written; nothing was applied.
    std::string mError;
    /// "Name@File.fx" entries of Techniques= with no such technique loaded.
    std::vector<std::string> mMissingTechniques;
    /// [File.fx] sections with no such effect loaded.
    std::vector<std::string> mMissingEffects;
    /// "File.fx: Name" keys with no such uniform in the effect.
    std::vector<std::string> mUnknownUniforms;
    /// "File.fx: Name" keys whose value could not be read.
    std::vector<std::string> mBadValues;
    /// The preset's PreprocessorDefinitions (global, then "File.fx: ..." for
    /// per-effect ones), which the runtime cannot apply yet: kept in the file,
    /// not applied.
    std::vector<std::string> mIgnoredDefinitions;

    bool failed() const { return !mError.empty(); }
    bool hasWarnings() const;
    /// One or two short lines for the UI; empty when there is nothing to say.
    std::string summary() const;
    void clear() { *this = SLFxPresetReport(); }
};

/**
 * Effect presets in ReShade's own .ini format, so a preset from a ReShade
 * install loads here and one saved here loads there.
 *
 * @par Where
 * ~/.slipstreamviewer/fx/presets/<name>.ini (presetsDir()). A preset from
 * anywhere else is imported: copied in, then loaded. The viewer also ships a few
 * presets in app_settings/fx/presets (builtinPresetsDir()): they are listed beside the
 * user's, a user preset of the same name wins, and every write (save, auto-save, import)
 * goes to the user folder only, so editing a shipped preset makes the user's own copy.
 *
 * @par The format (as ReShade 5/6 writes it)
 * @code
 * PreprocessorDefinitions=NAME=VALUE,NAME2=VALUE2
 * TechniqueSorting=Clarity@Clarity.fx,Vibrance@Vibrance.fx,LumaSharpen@LumaSharpen.fx
 * Techniques=Clarity@Clarity.fx,Vibrance@Vibrance.fx
 * KeyClarity@Clarity.fx=46,0,0,0
 *
 * [Clarity.fx]
 * ClarityStrength=0.400000
 * ClarityBlendMode=2
 *
 * [Vibrance.fx]
 * VibranceRGBBalance=1.000000,1.000000,1.000000
 * @endcode
 * Techniques= is the enabled techniques in render order; TechniqueSorting= is
 * every technique in the order the list shows them. Entries are
 * "Technique@File.fx"; a bare "Technique" (ReShade 4) matches the first
 * technique of that name. Values: bools 0/1, ints decimal, floats "%f",
 * vectors comma-separated. Anything else (Key... hotkeys, per-effect
 * PreprocessorDefinitions, keys from newer ReShade versions) is kept as is.
 *
 * @par Loading
 * Orders the techniques by TechniqueSorting (then Techniques), enables
 * exactly those in Techniques, sets every uniform the file names and resets
 * every other uniform to its default, which is what ReShade does. What it
 * cannot apply goes into the report.
 *
 * @par Saving
 * Into the file's own text: a value is re-written only when it differs from
 * what the file says, so saving an unchanged preset writes it back byte for
 * byte, and techniques or sections for effects not installed here survive.
 *
 * @par The active preset
 * SLFxActivePreset names it. Changing the setting (from Photo Studio, from
 * Preferences, or by Preferences' Cancel putting it back) loads it. With
 * SLFxAutoSavePreset on, edits are written to it a moment later, as ReShade
 * does with its current preset; that is also what carries effect settings
 * from one session to the next.
 */
class SLFxPresets final : public LLSingleton<SLFxPresets>
{
    LLSINGLETON(SLFxPresets);
    ~SLFxPresets() override;

public:
    /// Hook up the setting and the runtime, and restore the active preset
    /// once the runtime has effects. Idempotent; call it early (startup) and
    /// from anything that shows presets.
    void init();

    /// ~/.slipstreamviewer/fx/presets, created if missing.
    static std::string presetsDir();
    /// The read-only presets that ship with the viewer (app_settings/fx/presets).
    static std::string builtinPresetsDir();
    /// Preset names (file names without ".ini") of both folders, sorted case-insensitively.
    std::vector<std::string> listPresets() const;
    /// Where a preset is written: always the user's folder.
    std::string pathFor(const std::string& name) const;
    /// Where a preset is read from: the user's file, else the shipped one.
    std::string readPathFor(const std::string& name) const;
    /// True for a shipped preset the user has not copied or edited.
    bool isBuiltinOnly(const std::string& name) const;
    /// Usable as a file name on every platform, and not empty.
    static bool isValidName(const std::string& name);
    bool exists(const std::string& name) const;

    /// The active preset's name (may name a file that does not exist yet).
    std::string getActive() const;

    /// Load @a name and make it the active preset. False (and the reason in
    /// the report) if the file could not be read.
    bool load(const std::string& name);
    /// Write the current state into @a name (keeping what its file already
    /// holds, see the class comment) and make it the active preset.
    bool save(const std::string& name);
    bool saveActive() { return save(getActive()); }
    /// Delete the file. Deleting the active preset leaves it active but
    /// unsaved, so the next save creates it again.
    bool remove(const std::string& name);
    /// Copy @a source_path into the presets folder (renamed if the name is
    /// taken) and load it. @a out_name is the name it got.
    bool importFile(const std::string& source_path, std::string& out_name);
    /// Write the current state, on top of the active preset's file, to
    /// @a dest_path.
    bool exportTo(const std::string& dest_path);

    /// The user changed something the preset holds: the active preset is now
    /// unsaved, and auto-save (SLFxAutoSavePreset) is scheduled.
    void markDirty();
    bool isDirty() const { return mDirty; }

    /// What the last load, save, import or export could not do.
    const SLFxPresetReport& lastReport() const { return mReport; }

    /// The list of presets, the active one, its dirty state or the report
    /// changed. Fired on the main thread.
    boost::signals2::connection setChangedCallback(const std::function<void()>& cb);

    // --- The value rules, public for the Effects page ---

    /// "Technique@File.fx".
    static std::string techniqueId(const SLFxTechniqueInfo& tech);
    /// The name the runtime's technique calls take. One place, because the
    /// runtime contract keys techniques by bare name and names repeat across
    /// effect files; see the report to #545.
    static std::string techniqueKey(const SLFxTechniqueInfo& tech);
    /// "Clarity.fx" from any path.
    static std::string effectFileName(const std::string& path);
    /// How many components, and of what kind, the uniform's values have.
    enum EComponent { COMP_BOOL, COMP_INT, COMP_FLOAT };
    static EComponent componentKind(const SLFxUniformInfo& uniform);
    static S32 componentCount(const SLFxUniformInfo& uniform);
    /// The value as ReShade writes it ("0.500000", "1,0,0", "1").
    static std::string formatValue(const SLFxUniformInfo& uniform, const LLSD& value);
    /// The value from what ReShade wrote. Missing components keep the
    /// default's. False if a component is not a number.
    static bool parseValue(const SLFxUniformInfo& uniform, const std::string& text, LLSD& out);
    /// Equal as ReShade would write them.
    static bool valuesEqual(const SLFxUniformInfo& uniform, const LLSD& a, const LLSD& b);

private:
    bool readDocument(const std::string& path, SLFxIniDocument& doc, std::string& error) const;
    bool writeDocument(const std::string& path, const SLFxIniDocument& doc, std::string& error) const;
    /// Capture into @a name's file; nothing else (save() also makes it active).
    bool writePreset(const std::string& name);
    void applyDocument(const SLFxIniDocument& doc, SLFxPresetReport& report);
    void captureInto(SLFxIniDocument& doc) const;
    void setActiveQuietly(const std::string& name);
    void onActiveSettingChanged(const LLSD& old_value, const LLSD& new_value);
    void onRuntimeChanged();
    void tryRestore();
    void scheduleAutoSave();
    void notifyChanged();

    boost::signals2::signal<void()> mChangedSignal;
    boost::signals2::scoped_connection mActiveConnection;
    boost::signals2::scoped_connection mRuntimeConnection;
    SLFxPresetReport mReport;
    bool mInitialized = false;
    bool mRestored = false;
    bool mDirty = false;
    /// Set while we write SLFxActivePreset ourselves, so its signal does not
    /// load what we just saved.
    bool mSettingActive = false;
    /// Set while a preset is applied, so runtime change notices it causes are
    /// not taken for anything else.
    bool mApplying = false;
    U32 mSaveGeneration = 0;
};

#endif // SL_FXPRESETS_H
