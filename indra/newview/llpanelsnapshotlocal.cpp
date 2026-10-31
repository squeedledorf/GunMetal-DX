/**
 * @file llpanelsnapshotlocal.cpp
 * @brief The panel provides UI for saving snapshot to a local folder.
 *
 * $LicenseInfo:firstyear=2011&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2011, Linden Research, Inc.
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

#include "llcombobox.h"
#include "lldirpicker.h"
#include "llfile.h"
#include "llfontgl.h"
#include "lltextbox.h"
#include "lluri.h"
#include "llsidetraypanelcontainer.h"
#include "llsliderctrl.h"
#include "llspinctrl.h"

#include "llfloatersnapshot.h" // FIXME: replace with a snapshot storage model
#include "llnotificationsutil.h"
#include "llpanelsnapshot.h"
#include "llsnapshotlivepreview.h"
#include "llviewercontrol.h" // gSavedSettings
#include "llviewerwindow.h"

#if LL_SDL_WINDOW
#include "SDL3/SDL.h"
#endif

/**
 * The panel provides UI for saving snapshot to a local folder.
 */
class LLPanelSnapshotLocal
:   public LLPanelSnapshot
{
    LOG_CLASS(LLPanelSnapshotLocal);

public:
    LLPanelSnapshotLocal();
    bool postBuild() override;
    void onOpen(const LLSD& key) override;

private:
    std::string getWidthSpinnerName() const  override  { return "local_snapshot_width"; }
    std::string getHeightSpinnerName() const override  { return "local_snapshot_height"; }
    std::string getAspectRatioCBName() const override  { return "local_keep_aspect_check"; }
    std::string getImageSizeComboName() const override { return "local_size_combo"; }
    std::string getImageSizePanelName() const override { return "local_image_size_lp"; }
    LLSnapshotModel::ESnapshotFormat getImageFormat() const override;
    LLSnapshotModel::ESnapshotType getSnapshotType() override;
    void updateControls(const LLSD& info) override;

    S32 mLocalFormat;

    void onFormatComboCommit(LLUICtrl* ctrl);
    void onQualitySliderCommit(LLUICtrl* ctrl);
    void onSaveFlyoutCommit(LLUICtrl* ctrl);
    void performTimedAction(const LLSD& action) override;

    void onLocalSaved();
    void onLocalCanceled();

    // Output folder row (SnapshotBaseDir, per-account), showing the next file's path.
    void refreshFolderDisplay();
    void onBrowseFolder();
    void onOpenFolder();

    // #788: file name row (SnapshotFileNamePattern)
    void refreshNameCombo();
    void onNameComboCommit();
    void editCustomPattern();
    std::string getNextPath();

    boost::signals2::scoped_connection mSnapshotDirConnection;
    boost::signals2::scoped_connection mBaseNameConnection;
    boost::signals2::scoped_connection mPatternConnection;
    // The next path, kept while what it depends on is unchanged: finding it may stat many
    // files ({n} in a full folder). Cleared after a save and on opening.
    std::string mNextPathKey;
    std::string mNextPath;
};

static LLPanelInjector<LLPanelSnapshotLocal> panel_class("llpanelsnapshotlocal");

// SnapshotFileNamePattern, its default when empty
static std::string snapshot_name_pattern()
{
    std::string pattern = gSavedSettings.getString("SnapshotFileNamePattern");
    if (pattern.empty())
    {
        LLControlVariablePtr control = gSavedSettings.getControl("SnapshotFileNamePattern");
        pattern = control ? control->getDefault().asString() : std::string();
    }
    return pattern;
}

LLPanelSnapshotLocal::LLPanelSnapshotLocal()
{
    mLocalFormat = gSavedSettings.getS32("SnapshotFormat");
    mCommitCallbackRegistrar.add("Local.Cancel",    boost::bind(&LLPanelSnapshotLocal::cancel,      this));
    mCommitCallbackRegistrar.add("Local.BrowseFolder", boost::bind(&LLPanelSnapshotLocal::onBrowseFolder, this));
    mCommitCallbackRegistrar.add("Local.OpenFolder",   boost::bind(&LLPanelSnapshotLocal::onOpenFolder,   this));
}

// virtual
bool LLPanelSnapshotLocal::postBuild()
{
    getChild<LLUICtrl>("image_quality_slider")->setCommitCallback(boost::bind(&LLPanelSnapshotLocal::onQualitySliderCommit, this, _1));
    getChild<LLUICtrl>("local_format_combo")->setCommitCallback(boost::bind(&LLPanelSnapshotLocal::onFormatComboCommit, this, _1));
    getChild<LLUICtrl>("save_btn")->setCommitCallback(boost::bind(&LLPanelSnapshotLocal::onSaveFlyoutCommit, this, _1));
    getChild<LLUICtrl>("local_name_combo")->setCommitCallback(boost::bind(&LLPanelSnapshotLocal::onNameComboCommit, this));

    // Follow the folder setting so the first-save picker, "Save As..." and
    // a missing-folder reset (LLViewerWindow::saveImageLocal) all show up here.
    if (LLControlVariablePtr ctrl = gSavedPerAccountSettings.getControl("SnapshotBaseDir"))
    {
        mSnapshotDirConnection = ctrl->getSignal()->connect([this](LLControlVariable*, const LLSD&, const LLSD&)
        {
            refreshFolderDisplay();
        });
    }
    // "Save As..." names the snapshots ({name})
    if (LLControlVariablePtr ctrl = gSavedPerAccountSettings.getControl("SnapshotBaseName"))
    {
        mBaseNameConnection = ctrl->getSignal()->connect([this](LLControlVariable*, const LLSD&, const LLSD&)
        {
            refreshFolderDisplay();
        });
    }
    if (LLControlVariablePtr ctrl = gSavedSettings.getControl("SnapshotFileNamePattern"))
    {
        mPatternConnection = ctrl->getSignal()->connect([this](LLControlVariable*, const LLSD&, const LLSD&)
        {
            refreshNameCombo();
            refreshFolderDisplay();
        });
    }
    refreshNameCombo();
    refreshFolderDisplay();

    return LLPanelSnapshot::postBuild();
}

// virtual
void LLPanelSnapshotLocal::onOpen(const LLSD& key)
{
    if(gSavedSettings.getS32("SnapshotFormat") != mLocalFormat)
    {
        getChild<LLComboBox>("local_format_combo")->selectNthItem(mLocalFormat);
    }
    mNextPathKey.clear();
    refreshNameCombo();
    refreshFolderDisplay();
    LLPanelSnapshot::onOpen(key);
}

// virtual
LLSnapshotModel::ESnapshotFormat LLPanelSnapshotLocal::getImageFormat() const
{
    LLSnapshotModel::ESnapshotFormat fmt = LLSnapshotModel::SNAPSHOT_FORMAT_PNG;

    LLComboBox* local_format_combo = getChild<LLComboBox>("local_format_combo");
    const std::string id  = local_format_combo->getValue().asString();
    if (id == "PNG")
    {
        fmt = LLSnapshotModel::SNAPSHOT_FORMAT_PNG;
    }
    else if (id == "JPEG")
    {
        fmt = LLSnapshotModel::SNAPSHOT_FORMAT_JPEG;
    }
    else if (id == "BMP")
    {
        fmt = LLSnapshotModel::SNAPSHOT_FORMAT_BMP;
    }

    return fmt;
}

// virtual
void LLPanelSnapshotLocal::updateControls(const LLSD& info)
{
    LLSnapshotModel::ESnapshotFormat fmt =
        (LLSnapshotModel::ESnapshotFormat) gSavedSettings.getS32("SnapshotFormat");
    getChild<LLComboBox>("local_format_combo")->selectNthItem((S32) fmt);

    const bool show_quality_ctrls = (fmt == LLSnapshotModel::SNAPSHOT_FORMAT_JPEG);
    getChild<LLUICtrl>("image_quality_slider")->setVisible(show_quality_ctrls);
    getChild<LLUICtrl>("image_quality_level")->setVisible(show_quality_ctrls);

    getChild<LLUICtrl>("image_quality_slider")->setValue(gSavedSettings.getS32("SnapshotQuality"));
    updateImageQualityLevel();

    const bool have_snapshot = info.has("have-snapshot") ? info["have-snapshot"].asBoolean() : true;
    getChild<LLUICtrl>("save_btn")->setEnabled(have_snapshot);

    refreshFolderDisplay();
}

void LLPanelSnapshotLocal::refreshFolderDisplay()
{
    LLTextBox* path_text = getChild<LLTextBox>("local_folder_path");
    const std::string dir = gSavedPerAccountSettings.getString("SnapshotBaseDir");

    if (dir.empty())
    {
        path_text->setText(getString("folder_unset"));
        path_text->setToolTip(getString("folder_unset"));
        getChildView("local_folder_open_btn")->setEnabled(false);
        return;
    }

    // #788: the folder and the name the next snapshot gets there, as an example of the
    // file name setting
    const std::string next_path = getNextPath();

    // Paths are interesting at the tail: keep the end and trim from the front.
    LLWString display = utf8str_to_wstring(next_path);
    const LLFontGL* font = path_text->getFont();
    const S32 avail = path_text->getRect().getWidth() - 2 * path_text->getHPad() - 2;
    if (font && avail > 0 && font->getWidth(display.c_str()) > avail)
    {
        static const LLWString ellipsis = utf8str_to_wstring("...");
        while (display.size() > 1 && font->getWidth((ellipsis + display).c_str()) > avail)
        {
            display.erase(0, 1);
        }
        display = ellipsis + display;
    }
    path_text->setText(wstring_to_utf8str(display));
    LLStringUtil::format_map_t args;
    args["[PATH]"] = next_path;
    path_text->setToolTip(getString("next_file_tooltip", args));

    getChildView("local_folder_open_btn")->setEnabled(LLFile::isdir(dir));
}

std::string LLPanelSnapshotLocal::getNextPath()
{
    std::string extension;
    switch (getImageFormat())
    {
        case LLSnapshotModel::SNAPSHOT_FORMAT_JPEG: extension = "jpg"; break;
        case LLSnapshotModel::SNAPSHOT_FORMAT_BMP:  extension = "bmp"; break;
        default:                                    extension = "png"; break;
    }
    const std::string pattern = gSavedSettings.getString("SnapshotFileNamePattern");
    const S32 width = getTypedPreviewWidth();
    const S32 height = getTypedPreviewHeight();
    const std::string key = llformat("%s|%s|%s|%s|%dx%d", pattern.c_str(), extension.c_str(),
                                     gSavedPerAccountSettings.getString("SnapshotBaseDir").c_str(),
                                     gSavedPerAccountSettings.getString("SnapshotBaseName").c_str(), width, height);
    if (key != mNextPathKey)
    {
        mNextPathKey = key;
        mNextPath = gViewerWindow->getNextSnapshotPath(pattern, width, height, extension);
    }
    return mNextPath;
}

void LLPanelSnapshotLocal::refreshNameCombo()
{
    LLComboBox* combo = getChild<LLComboBox>("local_name_combo");
    const std::string pattern = snapshot_name_pattern();
    // A pattern none of the presets has is the custom one
    if (!combo->selectByValue(pattern))
    {
        combo->selectByValue("custom");
    }
    combo->setToolTip(getString("name_combo_tooltip") + "\n" + pattern);
}

void LLPanelSnapshotLocal::onNameComboCommit()
{
    const std::string value = getChild<LLUICtrl>("local_name_combo")->getValue().asString();
    if (value == "custom")
    {
        editCustomPattern();
        return;
    }
    gSavedSettings.setString("SnapshotFileNamePattern", value);
}

void LLPanelSnapshotLocal::editCustomPattern()
{
    LLSD args;
    args["PATTERN"] = snapshot_name_pattern();
    args["BASE_NAME"] = gSavedPerAccountSettings.getString("SnapshotBaseName");
    // The setting's signal refreshes the panel. The combo is put back on the pattern in use
    // also when that did not change (Cancel, or OK on the same pattern).
    LLHandle<LLPanelSnapshotLocal> handle = getDerivedHandle<LLPanelSnapshotLocal>();
    LLNotificationsUtil::add("SnapshotFileNamePattern", args, LLSD(), [handle](const LLSD& notification, const LLSD& response)
    {
        if (LLNotificationsUtil::getSelectedOption(notification, response) == 0)
        {
            std::string pattern = response["pattern"].asString();
            LLStringUtil::trim(pattern);
            gSavedSettings.setString("SnapshotFileNamePattern", pattern);
        }
        if (LLPanelSnapshotLocal* panel = handle.get())
        {
            panel->refreshNameCombo();
        }
    });
}

void LLPanelSnapshotLocal::onBrowseFolder()
{
    // The picker reply is delivered on the main loop (LLDirPickerThread::clearDead).
    // Only write the setting -- the control signal refreshes this panel, so a
    // floater closed while the dialog is up leaves nothing dangling.
    (new LLDirPickerThread([](const std::vector<std::string>& filenames, std::string)
    {
        if (!filenames.empty() && !filenames[0].empty())
        {
            gSavedPerAccountSettings.setString("SnapshotBaseDir", filenames[0]);
        }
    }, gSavedPerAccountSettings.getString("SnapshotBaseDir")))->getFile();
}

void LLPanelSnapshotLocal::onOpenFolder()
{
    std::string dir = gSavedPerAccountSettings.getString("SnapshotBaseDir");
    if (dir.empty() || !LLFile::isdir(dir))
    {
        refreshFolderDisplay();
        return;
    }

#if LL_SDL_WINDOW
    // file: is not on the LLWindow::spawnWebBrowser whitelist, so go to SDL
    // directly; SDL_OpenURL hands file URLs to the desktop's file manager.
#if LL_WINDOWS
    std::replace(dir.begin(), dir.end(), '\\', '/');
    std::string url = "file:///";
#else
    std::string url = "file://";
#endif
    static const std::string allowed =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~/:";
    url += LLURI::escape(dir, allowed);
    LL_INFOS() << "Opening snapshot folder " << url << LL_ENDL;
    if (!SDL_OpenURL(url.c_str()))
    {
        LL_WARNS() << "Could not open snapshot folder " << url << ": " << SDL_GetError() << LL_ENDL;
    }
#else
    LL_WARNS() << "Opening the snapshot folder is not supported on this window backend" << LL_ENDL;
#endif
}

void LLPanelSnapshotLocal::onFormatComboCommit(LLUICtrl* ctrl)
{
    mLocalFormat = getImageFormat();
    // will call updateControls()
    LLFloaterSnapshot::getInstance()->notify(LLSD().with("image-format-change", true));
}

void LLPanelSnapshotLocal::onQualitySliderCommit(LLUICtrl* ctrl)
{
    updateImageQualityLevel();

    LLSliderCtrl* slider = (LLSliderCtrl*)ctrl;
    S32 quality_val = llfloor((F32)slider->getValue().asReal());
    LLSD info;
    info["image-quality-change"] = quality_val;
    LLFloaterSnapshot::getInstance()->notify(info);
}

void LLPanelSnapshotLocal::onSaveFlyoutCommit(LLUICtrl* ctrl)
{
    LLSD action = LLSD().with("type", "local").with("save_as", ctrl->getValue().asString() == "save as");
    LLFloaterSnapshot* floater = LLFloaterSnapshot::getInstance();
    if (floater->startTimedAction(this, action)) return;
    performTimedAction(action);
}

void LLPanelSnapshotLocal::performTimedAction(const LLSD& action)
{
    if (action["save_as"].asBoolean())
    {
        gViewerWindow->resetSnapshotLoc();
    }

    LLFloaterSnapshot* floater = LLFloaterSnapshot::getInstance();

    floater->notify(LLSD().with("set-working", true));
    LLHandle<LLPanel> panel = getHandle();
    floater->saveLocal([panel]()
    {
        if (auto* self = dynamic_cast<LLPanelSnapshotLocal*>(panel.get()); self && self->isInVisibleChain()) self->onLocalSaved();
    }, [panel]()
    {
        if (auto* self = dynamic_cast<LLPanelSnapshotLocal*>(panel.get()); self && self->isInVisibleChain()) self->onLocalCanceled();
    });
}

void LLPanelSnapshotLocal::onLocalSaved()
{
    // the file just saved takes the name shown, find the next one
    mNextPathKey.clear();
    refreshFolderDisplay();
    mSnapshotFloater->postSave();
    LLFloaterSnapshot::getInstance()->notify(LLSD().with("set-finished", LLSD().with("ok", true).with("msg", "local")));
}

void LLPanelSnapshotLocal::onLocalCanceled()
{
    cancel();
    LLFloaterSnapshot::getInstance()->notify(LLSD().with("set-finished", LLSD().with("ok", false).with("msg", "local")));
}


LLSnapshotModel::ESnapshotType LLPanelSnapshotLocal::getSnapshotType()
{
    return LLSnapshotModel::SNAPSHOT_LOCAL;
}
