/**
 * @file slphotostudiodirectory.h
 * @brief Where a Photo Studio section panel's sections and settings are, found once
 *
 * Slipstream Photo Studio: originally allightboxdirectory.h from Alchemy Viewer's Lightbox
 * (upstream Alchemy, credited); renamed for Slipstream.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Alchemy Viewer Source Code
 * Copyright (C) 2026, Alchemy Viewer Project.
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

#ifndef SL_PHOTOSTUDIODIRECTORY_H
#define SL_PHOTOSTUDIODIRECTORY_H

#include "stdtypes.h"

#include <array>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

class LLAccordionCtrl;
class LLAccordionCtrlTab;
class LLPanel;
class LLTextBox;
class LLUICtrl;
class LLView;

/**
 * Every section and every bound setting on a Photo Studio section panel, found by walking the
 * pages once and then held by pointer.
 *
 * @par Why once, and why by pointer
 * Each section panel instance (and the floater's catalog of every page) keeps
 * one of these, built in postBuild. Looking widgets up by pointer afterwards
 * keeps two panels that show the same section -- and so carry widgets with
 * the same names -- from ever finding each other's controls.
 *
 * @par Captions
 * A setting's caption is what its row calls it, read the way a person reads
 * the row: the control's own label if it has one ("Strength" on a slider, the
 * text beside a checkbox), else the text to its left on the same line (a
 * dropdown, a colour, a vector row), else the key made into words. Finding a setting by
 * name and naming an undo step both read it from here.
 */
class SLPhotoStudioDirectory
{
public:
    struct Section
    {
        /// The section panel's name: "sec_tone", or "sec_tone_adv" for the
        /// Advanced sibling, which is a section of its own here.
        std::string         mName;
        /// What its accordion header says.
        std::string         mTitle;
        /// Which tab page it is on, in tab order.
        size_t              mPage = 0;
        LLAccordionCtrl*    mAccordion = nullptr;
        LLAccordionCtrlTab* mTab = nullptr;
        LLPanel*            mPanel = nullptr;
    };

    struct Setting
    {
        /// The gSavedSettings name.
        std::string mKey;
        /// What its row calls it.
        std::string mCaption;
        /// Index into sections().
        size_t      mSection = 0;
        /// The control to put the keyboard in when going to the setting.
        LLUICtrl*   mCtrl = nullptr;
        /// The text the caption was read from, which is what to highlight;
        /// null when the caption came from a section title or the key.
        LLTextBox*  mCaptionBox = nullptr;
    };

    /// Walk @a pages, in tab order, and remember every section and setting on
    /// them. Rebuilding replaces what was there.
    void build(const std::vector<LLPanel*>& pages);

    /// In the order the XUI declares them, page by page.
    const std::vector<Section>& sections() const { return mSections; }
    /// In the order the XUI declares them, section by section; one entry per
    /// key, the first control found for it.
    const std::vector<Setting>& settings() const { return mSettings; }

    /// By panel name ("sec_tone"), or null.
    const Section* section(std::string_view name) const;
    /// By setting name, or null.
    const Setting* setting(std::string_view key) const;

    /// What a setting is called: its row's caption, or, for a setting no row
    /// here shows, its key made into words.
    std::string captionFor(const std::string& key) const;
    /// The section a setting's row is in, or null for one no row shows.
    const Section* sectionOf(std::string_view key) const;

    /// The widget naming contract of the vector rows, "vec3_<Setting>_<0|1|2>".
    /// Setting names never contain '_', so the parse is unambiguous.
    static bool parseVec3Name(const std::string& name, std::string& setting, S32& component);

    /// Every widget under @a view that follows the vector naming contract,
    /// by setting and component. Any LLUICtrl will do; the name is the whole
    /// contract.
    static void collectVec3Controls(LLView* view, std::map<std::string, std::array<LLUICtrl*, 3>>& rows);

    /// The gSavedSettings keys bound anywhere under @a view, plus the settings
    /// its vector rows name: what a section's Reset All resets.
    static void collectBoundKeys(LLView* view, std::set<std::string>& keys);

private:
    void addSection(LLAccordionCtrlTab* tab, size_t page);
    void addSettings(LLView* view, size_t section);
    /// The settings a graph under @a view edits without being bound to them
    /// (an LLSD point list cannot be), for any that no row of the section has
    /// already claimed.
    void addGraphSettings(LLView* view, size_t section);
    /// @a caption, where one is given, is the setting's name as it stands;
    /// left empty, the name is read off the control's label or the text beside it.
    void addSetting(const std::string& key, LLUICtrl* ctrl, size_t section, bool vector_row,
                    const std::string& caption = std::string());

    std::vector<Section> mSections;
    std::vector<Setting> mSettings;
    std::map<std::string, size_t, std::less<>> mSectionIndex;
    std::map<std::string, size_t, std::less<>> mSettingIndex;
};

#endif // SL_PHOTOSTUDIODIRECTORY_H
