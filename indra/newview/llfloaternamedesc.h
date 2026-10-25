/**
 * @file llfloaternamedesc.h
 * @brief LLFloaterNameDesc class definition
 *
 * $LicenseInfo:firstyear=2002&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
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

#ifndef LL_LLFLOATERNAMEDESC_H
#define LL_LLFLOATERNAMEDESC_H

#include "llfloater.h"
#include "llresizehandle.h"
#include "llstring.h"
#include "llassettype.h"

#include <memory>

class LLLineEditor;
class LLButton;
class LLRadioGroup;
class LLNewFileResourceUploadInfo;
struct LLAudioClip;

class LLFloaterNameDesc : public LLFloater
{
public:
    LLFloaterNameDesc(const LLSD& args);
    virtual ~LLFloaterNameDesc();
    bool postBuild() override;

    void        onBtnOK();
    void        onBtnCancel();
    void        doCommit();

    virtual S32 getExpectedUploadCost() const;

protected:
    // Hook for subclasses to adjust the upload before it is queued.
    virtual void        prepareUploadInfo(LLNewFileResourceUploadInfo&) {}
    virtual void        onCommit() override;

protected:
    bool        mIsAudio;
    bool        mIsText;

    std::string     mFilenameAndPath;
    std::string     mFilename;
    LLUUID          mDestinationFolderId;
};

class LLFloaterSoundPreview : public LLFloaterNameDesc
{
public:
    LLFloaterSoundPreview(const LLSD& filename );
    bool postBuild() override;

    // Decoded audio for the chosen file (set by upload_single_file after decoding).
    void setAudioClip(std::shared_ptr<const LLAudioClip> clip);

protected:
    void prepareUploadInfo(LLNewFileResourceUploadInfo& info) override;
    void onCommit() override { updateAudioInfo(); }   // name edited: refresh the part names shown

private:
    void updateAudioInfo();
    void onBtnSoundOK();

    std::shared_ptr<const LLAudioClip> mClip;
    S32  mSplitCount = 1;   // > 1: the clip is over the upload limit and will be split (#807)
};

class LLFloaterAnimPreview : public LLFloaterNameDesc
{
public:
    LLFloaterAnimPreview(const LLSD& filename );
    bool postBuild() override;
};

class LLFloaterScriptPreview : public LLFloaterNameDesc
{
public:
    LLFloaterScriptPreview(const LLSD& filename );
    bool postBuild() override;
};

#endif  // LL_LLFLOATERNAMEDESC_H
