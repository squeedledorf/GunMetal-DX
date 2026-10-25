/**
 * @file llfloaterchatimage.h
 * @brief Chat inline image previews (#265): full-size preview of an image link
 *        (Open in browser / Copy link / Save to disk).
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
 * $/LicenseInfo$
 */

#ifndef LL_LLFLOATERCHATIMAGE_H
#define LL_LLFLOATERCHATIMAGE_H

#include "llfloater.h"
#include "llchatimagecache.h"
#include "llpointer.h"

#include <memory>
#include <vector>

class LLButton;
class LLTextBox;
class LLViewerTexture;

// One instance per URL (LLFloaterReg key = the URL). "Upload to inventory" hands
// the picture to the standard texture upload floater (the same name / fee /
// permissions dialog as File > Upload > Image): see onUpload(). Re-fetches the image (the
// thumbnail cache keeps no original bytes) and holds the full image (<= 2048 per
// side) and the original bytes only while open. An animated GIF / WebP is decoded again
// for the floater at up to 1024 per side per frame, all frames together capped at
// LLChatImageCache::FULL_ANIM_MAX_BYTES (later frames are cut, the rest loops),
// and plays unless ChatImageAnimate is "never". The image is drawn by a child
// view so a playing animation is a live section of the retained floater (only
// the picture redraws each frame).
class LLFloaterChatImage : public LLFloater
{
    friend class LLFloaterReg;

public:
    LLFloaterChatImage(const LLSD& key);
    ~LLFloaterChatImage();

    /*virtual*/ bool postBuild() override;
    /*virtual*/ void onClose(bool app_quitting) override;

    // Draws the picture into `view` (the child covering "image_area").
    void drawImage(LLView* view);

private:
    void onLoaded(const LLChatImageCache::FullResult& result);
    void onOpenInBrowser();
    void onCopyLink();
    void onSave();
    void onUpload();
    // Sanitized file name (no extension) from the link's last path component.
    std::string proposedBaseName() const;
    void releaseImage();

    std::string mURL;
    U32 mTicket = 0;
    LLChatImageAnim mImage;        // one frame for a still
    F64 mPlayTime = 0.0;
    F64 mLastPlayDraw = 0.0;
    bool mPlayFinished = false;
    S32 mImageWidth = 0;
    S32 mImageHeight = 0;
    std::string mMime;
    std::shared_ptr<std::vector<U8> > mBytes;
    S64 mHeldBytes = 0;

    LLView* mImageArea = nullptr;
    LLView* mImageView = nullptr;
    LLTextBox* mStatusText = nullptr;
    LLButton* mSaveButton = nullptr;
    LLButton* mUploadButton = nullptr;
};

#endif // LL_LLFLOATERCHATIMAGE_H
