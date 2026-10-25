/**
 * @file llfloaterchatimage.cpp
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

#include "llviewerprecompiledheaders.h"

#include "llfloaterchatimage.h"

#include "llappviewer.h"
#include "llbutton.h"
#include "llchatimagegif.h"
#include "llchatimagewebp.h"
#include "lldir.h"
#include "llfile.h"
#include "llfilepicker.h"
#include "llfloaterreg.h"
#include "llimagepng.h"
#include "llnotificationsutil.h"
#include "llrender2dutils.h"
#include "lltextbox.h"
#include "lltimer.h"
#include "llurlaction.h"
#include "llviewercontrol.h"
#include "llviewermenufile.h"
#include "llviewertexture.h"

#include <cerrno>

namespace
{
    // Draws the floater's picture. A separate view so that a playing animation can
    // be a live section (LLRetainedLiveScope) of the retained floater: each replay
    // redraws just this view with the current frame.
    class LLChatImageView : public LLView
    {
    public:
        LLChatImageView(const LLView::Params& p, LLFloaterChatImage* owner)
        :   LLView(p), mOwner(owner)
        {
        }
        /*virtual*/ void draw() override
        {
            if (mOwner)
            {
                mOwner->drawImage(this);
            }
        }
    private:
        LLFloaterChatImage* mOwner;   // our parent: outlives us
    };
}

LLFloaterChatImage::LLFloaterChatImage(const LLSD& key)
:   LLFloater(key),
    mURL(key.asString())
{
}

LLFloaterChatImage::~LLFloaterChatImage()
{
    if (mTicket != 0 && LLChatImageCache::instanceExists())
    {
        LLChatImageCache::instance().cancelFull(mTicket);
        mTicket = 0;
    }
    releaseImage();
}

bool LLFloaterChatImage::postBuild()
{
    mImageArea = getChild<LLView>("image_area");
    mStatusText = getChild<LLTextBox>("status_text");
    mSaveButton = getChild<LLButton>("save_btn");
    mUploadButton = getChild<LLButton>("upload_btn");

    getChild<LLButton>("open_browser_btn")->setClickedCallback([this](LLUICtrl*, const LLSD&) { onOpenInBrowser(); });
    getChild<LLButton>("copy_link_btn")->setClickedCallback([this](LLUICtrl*, const LLSD&) { onCopyLink(); });
    mSaveButton->setClickedCallback([this](LLUICtrl*, const LLSD&) { onSave(); });
    mSaveButton->setEnabled(false);
    mUploadButton->setClickedCallback([this](LLUICtrl*, const LLSD&) { onUpload(); });
    mUploadButton->setEnabled(false);   // until the image has loaded

    // Title: the host + file name part of the link (the full URL is the tooltip).
    std::string label = mURL;
    const size_t scheme = label.find("://");
    if (scheme != std::string::npos)
    {
        label = label.substr(scheme + 3);
    }
    label = label.substr(0, label.find_first_of("?#"));
    if (label.size() > 64)
    {
        label = label.substr(0, 30) + "..." + label.substr(label.size() - 30);
    }
    LLStringUtil::format_map_t args;
    args["[NAME]"] = label;
    setTitle(getString("title", args));
    mImageArea->setToolTip(mURL);

    LLView::Params view_params;
    view_params.name = "chat_image_view";
    view_params.rect = mImageArea->getRect();
    view_params.follows.flags = FOLLOWS_ALL;
    view_params.mouse_opaque = false;
    mImageView = new LLChatImageView(view_params, this);
    addChild(mImageView);

    mStatusText->setValue(LLSD(getString("loading")));

    // Re-fetch (user-initiated): the thumbnail cache keeps no original bytes.
    LLHandle<LLFloater> handle = getHandle();
    mTicket = LLChatImageCache::instance().requestFull(mURL,
        [handle](const LLChatImageCache::FullResult& result)
        {
            if (LLFloaterChatImage* self = static_cast<LLFloaterChatImage*>(handle.get()))
            {
                self->onLoaded(result);
            }
        });
    return true;
}

void LLFloaterChatImage::onLoaded(const LLChatImageCache::FullResult& result)
{
    mTicket = 0;
    if (!result.ok || result.raw.isNull())
    {
        LLStringUtil::format_map_t args;
        args["[REASON]"] = result.reason.empty() ? std::string("unknown error") : result.reason;
        mStatusText->setValue(LLSD(getString("failed", args)));
        onRetainedDirty();
        return;
    }
    releaseImage();
    // One texture per frame (an animated GIF / WebP), else just the image; VKMem "thumb"
    // bucket, no setNoDelete().
    const bool animated = result.frames.size() > 1 && result.delays.size() == result.frames.size();
    S64 pixel_bytes = 0;
    const size_t count = animated ? result.frames.size() : 1;
    for (size_t i = 0; i < count; ++i)
    {
        const LLImageRaw* raw = animated ? result.frames[i].get() : result.raw.get();
        LLPointer<LLViewerTexture> tex = raw ? LLViewerTextureManager::getLocalTexture(raw, false)
                                             : LLPointer<LLViewerTexture>();
        if (tex.isNull())
        {
            break;   // keep the frames made so far
        }
        tex->setBoostLevel(LLGLTexture::BOOST_THUMBNAIL);
        mImage.frames.push_back(tex);
        pixel_bytes += (S64)raw->getWidth() * raw->getHeight() * raw->getComponents();
    }
    if (animated)
    {
        mImage.delays.assign(result.delays.begin(), result.delays.begin() + mImage.frames.size());
        mImage.loops = result.loops;
        mImage.updateDuration();
    }
    mPlayTime = 0.0;
    mLastPlayDraw = 0.0;
    mPlayFinished = false;
    mImageWidth = result.orig_w;
    mImageHeight = result.orig_h;
    mMime = result.mime;
    mBytes = result.bytes;
    mHeldBytes = pixel_bytes + (mBytes ? (S64)mBytes->size() : 0);
    LLChatImageCache::instance().addFloaterBytes(mHeldBytes);

    std::string type = "JPEG";
    if (mMime == "image/png")
    {
        type = "PNG";
    }
    else if (mMime == "image/gif" || mMime == "image/webp")
    {
        const char* name = (mMime == "image/gif") ? "GIF" : "WebP";
        type = mImage.isAnimated() ? llformat("%s, %d frames%s", name, (S32)mImage.frames.size(),
                                              result.truncated ? " (cut short)" : "")
                                   : std::string(name);
    }
    LLStringUtil::format_map_t args;
    args["[WIDTH]"] = llformat("%d", mImageWidth);
    args["[HEIGHT]"] = llformat("%d", mImageHeight);
    args["[TYPE]"] = type;
    args["[SIZE]"] = llformat("%.1f", mBytes ? (F32)mBytes->size() / 1024.f : 0.f);
    mStatusText->setValue(LLSD(getString("info", args)));
    mSaveButton->setEnabled(mBytes && !mBytes->empty());
    mUploadButton->setEnabled(mBytes && !mBytes->empty());
    onRetainedDirty();
}

void LLFloaterChatImage::releaseImage()
{
    if (mHeldBytes > 0 && LLChatImageCache::instanceExists())
    {
        LLChatImageCache::instance().addFloaterBytes(-mHeldBytes);
    }
    mHeldBytes = 0;
    mImage.clear();
    mBytes.reset();
}

void LLFloaterChatImage::onClose(bool app_quitting)
{
    if (mTicket != 0 && LLChatImageCache::instanceExists())
    {
        LLChatImageCache::instance().cancelFull(mTicket);
        mTicket = 0;
    }
    releaseImage();
    LLFloater::onClose(app_quitting);
}

void LLFloaterChatImage::drawImage(LLView* view)
{
    if (isMinimized() || mImage.frames.empty() || !view || mImageWidth <= 0 || mImageHeight <= 0)
    {
        return;
    }
    const LLRect area = view->getLocalRect();
    if (area.getWidth() <= 0 || area.getHeight() <= 0)
    {
        return;
    }

    // Frame from play time (only advanced across consecutive draws, i.e. while
    // the floater is drawn). ChatImageAnimate "never" shows the first frame.
    static LLCachedControl<S32> animate(gSavedSettings, "ChatImageAnimate", 2);
    size_t index = 0;
    bool live = false;
    if (mImage.isAnimated() && animate() > 0)
    {
        const F64 now = LLTimer::getTotalSeconds();
        if (mLastPlayDraw > 0.0 && now - mLastPlayDraw > 0.0 && now - mLastPlayDraw < 0.5)
        {
            mPlayTime += now - mLastPlayDraw;
        }
        mLastPlayDraw = now;
        bool finished = false;
        index = mImage.frameAt(mPlayTime, finished);
        if (finished && !mPlayFinished)
        {
            mPlayFinished = true;
            if (!LLFloaterView::sRetainedCapturing)
            {
                onRetainedDirty();   // re-record once with the last frame
            }
        }
        live = !finished;
    }
    else
    {
        mLastPlayDraw = 0.0;
    }
    LLViewerTexture* tex = mImage.frames[llmin(index, mImage.frames.size() - 1)].get();
    // Before anything is drawn; a no-op when not recording.
    LLRetainedLiveScope live_scope(live ? view : nullptr);

    // Fit (never upscale past 1:1), centred, aspect preserved. The texture itself
    // is stretched to power-of-two dims; the rect restores the aspect.
    const F32 scale = llmin(1.f, llmin((F32)area.getWidth() / (F32)mImageWidth,
                                       (F32)area.getHeight() / (F32)mImageHeight));
    const S32 w = llmax(1, ll_round((F32)mImageWidth * scale));
    const S32 h = llmax(1, ll_round((F32)mImageHeight * scale));
    const S32 x = area.mLeft + (area.getWidth() - w) / 2;
    const S32 y = area.mBottom + (area.getHeight() - h) / 2;
    gl_draw_scaled_image(x, y, w, h, tex, UI_VERTEX_COLOR);
}

void LLFloaterChatImage::onOpenInBrowser()
{
    LLUrlAction::openURLExternal(mURL);
}

void LLFloaterChatImage::onCopyLink()
{
    LLUrlAction::copyURLToClipboard(mURL);
}

void LLFloaterChatImage::onSave()
{
    if (!mBytes || mBytes->empty())
    {
        return;
    }
    const bool png = (mMime == "image/png");
    const bool gif = (mMime == "image/gif");
    const bool webp = (mMime == "image/webp");
    const std::string ext = png ? ".png" : (gif ? ".gif" : (webp ? ".webp" : ".jpg"));

    // Proposed name: the link's last path component, sanitized, with the right
    // extension.
    const std::string proposed = proposedBaseName() + ext;

    // The picker reply may come after this floater closed: the bytes travel with it.
    const std::shared_ptr<std::vector<U8> > bytes = mBytes;
    LLFilePickerReplyThread::startPicker(
        [bytes](const std::vector<std::string>& filenames, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter)
        {
            if (LLAppViewer::instance()->quitRequested() || filenames.empty() || !bytes)
            {
                return;
            }
            const std::string& filename = filenames[0];
            LLFILE* fp = LLFile::fopen(filename, "wb");
            bool ok = false;
            if (fp)
            {
                ok = fwrite(bytes->data(), 1, bytes->size(), fp) == bytes->size();
                ok = (LLFile::close(fp) == 0) && ok;
            }
            if (!ok)
            {
                LL_WARNS("ChatImage") << "Could not save image to " << filename << LL_ENDL;
                LLSD args;
                args["MESSAGE"] = "Could not save the image to " + filename;
                LLNotificationsUtil::add("GenericAlert", args);
            }
        },
        png ? LLFilePicker::FFSAVE_PNG : ((gif || webp) ? LLFilePicker::FFSAVE_ALL : LLFilePicker::FFSAVE_JPEG),
        proposed);
}

std::string LLFloaterChatImage::proposedBaseName() const
{
    std::string name = mURL.substr(0, mURL.find_first_of("?#"));
    const size_t slash = name.rfind('/');
    name = (slash == std::string::npos) ? std::string() : name.substr(slash + 1);
    std::string clean;
    for (char c : name)
    {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')
        {
            clean += c;
        }
    }
    if (clean.size() > 64)
    {
        clean = clean.substr(0, 64);
    }
    const size_t dot = clean.rfind('.');
    if (dot != std::string::npos)
    {
        clean = clean.substr(0, dot);
    }
    if (clean.empty() || clean[0] == '.')
    {
        clean = "image";
    }
    return clean;
}

// Upload through the viewer's standard texture upload: the "upload_image" floater
// (LLFloaterImagePreview) with its name / description / fee / permissions and the
// usual L$ confirmation. It takes a local file, so the original bytes go to a
// uniquely named temp file (the folder is unique, the file keeps the link's name
// so the floater proposes it as the asset name). The floater reads and decodes the
// file in its constructor and only uses that decoded copy afterwards (onBtnOK
// encodes mRawImagep), so the file is deleted as soon as it has opened.
// LL's uploader has no GIF or WebP codec: a GIF / WebP uploads its first frame
// (composited, at its original size) as PNG. No RLV restriction applies to uploads (File > Upload
// is always enabled), so the button only waits for the image to load.
void LLFloaterChatImage::onUpload()
{
    if (!mBytes || mBytes->empty())
    {
        return;
    }
    const bool png = (mMime == "image/png");
    const bool gif = (mMime == "image/gif");
    const bool webp = (mMime == "image/webp");
    const std::string ext = (png || gif || webp) ? ".png" : ".jpg";

    std::string uuid_str;
    LLUUID::generateNewID().toString(uuid_str);
    const std::string dir = gDirUtilp->add(gDirUtilp->getTempDir(), "chatimage-" + uuid_str);
    if (LLFile::mkdir(dir) != 0)
    {
        LL_WARNS("ChatImage") << "Could not create a temp folder for the upload" << LL_ENDL;
        return;
    }
    const std::string path = gDirUtilp->add(dir, proposedBaseName() + ext);

    bool written = false;
    if (gif || webp)
    {
        // First frame at its original size (no box fit, no power-of-two stretch).
        LLChatImageGif::Params params;
        params.still_box_w = params.still_box_h = LLChatImageCache::MAX_DIMENSION;
        params.anim_box_w = params.anim_box_h = LLChatImageCache::MAX_DIMENSION;
        params.pot_max = 0;
        params.max_dim = LLChatImageCache::MAX_DIMENSION;
        params.max_frames = 1;
        params.max_frame_bytes = (U64)LLChatImageCache::MAX_DIMENSION * LLChatImageCache::MAX_DIMENSION * 4;
        LLChatImageGif::Result first;
        if (gif)
        {
            LLChatImageGif::decode(mBytes->data(), mBytes->size(), params, first);
        }
        else
        {
            LLChatImageWebP::decode(mBytes->data(), mBytes->size(), params, first);
        }
        if (first.ok && !first.frames.empty())
        {
            LLPointer<LLImagePNG> encoded = new LLImagePNG();
            written = encoded->encode(first.frames[0], 0.f) && encoded->save(path);
        }
    }
    else
    {
        LLFILE* fp = LLFile::fopen(path, "wb");
        if (fp)
        {
            written = fwrite(mBytes->data(), 1, mBytes->size(), fp) == mBytes->size();
            written = (LLFile::close(fp) == 0) && written;
        }
    }

    if (written)
    {
        LLSD args;
        args["filename"] = path;
        args["dest"] = LLUUID::null;   // default Textures folder, as File > Upload
        LLFloaterReg::showInstance("upload_image", args);
    }
    else
    {
        LL_WARNS("ChatImage") << "Could not prepare the image for upload" << LL_ENDL;
        LLSD args;
        args["MESSAGE"] = "Could not prepare the image for upload.";
        LLNotificationsUtil::add("GenericAlert", args);
    }
    // The upload floater has its own decoded copy by now (or failed to open).
    LLFile::remove(path, ENOENT);
    LLFile::rmdir(dir);
}
