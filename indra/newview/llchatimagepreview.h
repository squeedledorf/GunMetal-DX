/**
 * @file llchatimagepreview.h
 * @brief Chat inline image previews (#265): the inline widget under a chat
 *        message (click-to-load box, thumbnail, failure line) and the per-message
 *        load policy.
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

#ifndef LL_LLCHATIMAGEPREVIEW_H
#define LL_LLCHATIMAGEPREVIEW_H

#include "lluictrl.h"

#include <boost/signals2.hpp>

class LLChat;
struct LLChatImageAnim;
class LLTextBase;
class LLTextEditor;
class LLViewerTexture;

// One preview under a chat message. Inserted into the chat LLTextEditor as an
// inline widget (LLInlineViewSegment, like the chat headers). It never holds the
// thumbnail itself: every draw looks the URL up in LLChatImageCache, and the cache
// signal tells it when to re-layout (placeholder -> thumbnail -> evicted).
//
// Animated GIFs / WebPs (ChatImageAnimate: 0 never, 1 while hovered, 2 always) pick their
// frame from play time on every draw. Under the retained UI the chat floater is
// recorded once and replayed; a playing preview that is on screen draws itself as
// a live section (LLRetainedLiveScope), so each replay redraws only its one quad
// with the current frame and the rest of the floater (text) is not re-recorded
// or re-laid out. A preview that is scrolled out of the clip, in a closed or
// minimized floater, paused (hover mode, not hovered) or finished with its loops
// opens no live section: it is drawn (once, at the recording) like a still, and
// play time only advances across consecutive draws, so off-screen animations do
// no work at all.
class LLChatImagePreview : public LLUICtrl
{
public:
    // Scan `text` (the message body as shown) for image links and append one preview
    // widget per link (max 3 per message) to `editor`, after the message. Applies
    // the whole policy: master switch, source type (agents; objects only when
    // allowed), mute list, RLVa restrictions, URL safety, and auto-load vs
    // click-to-load (own message / friend / trusted site / everyone).
    // `column_width` is the widget column width of the chat history.
    static void appendPreviews(LLTextEditor* editor, const LLChat& chat, const std::string& text,
                               S32 left_pad, S32 right_pad, S32 column_width);

    LLChatImagePreview(const LLUICtrl::Params& p);
    ~LLChatImagePreview();

    /*virtual*/ void draw() override;
    /*virtual*/ bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    /*virtual*/ bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    /*virtual*/ bool handleRightMouseDown(S32 x, S32 y, MASK mask) override;
    /*virtual*/ bool handleHover(S32 x, S32 y, MASK mask) override;
    /*virtual*/ void onMouseEnter(S32 x, S32 y, MASK mask) override;
    /*virtual*/ void onMouseLeave(S32 x, S32 y, MASK mask) override;

private:
    enum EMode
    {
        MODE_CLICK_TO_LOAD,   // not loaded, the user decides
        MODE_RELOAD,          // was shown, dropped from the cache
        MODE_LOADING,
        MODE_THUMBNAIL,
        MODE_FAILED,
        MODE_HIDDEN           // "Hide this preview"
    };

    void setup(const std::string& url, const std::string& host, bool auto_load,
               const std::string& preset_failure, S32 max_width);
    EMode currentMode(std::string* reason = nullptr) const;
    void desiredSize(EMode mode, S32& width, S32& height) const;
    void refreshLayout();
    void onCacheUpdate(const std::string& url);
    void startLoad();
    void openPreview();
    void showContextMenu(S32 x, S32 y);
    void onMenuAction(const LLSD& param);
    bool onMenuVisible(const LLSD& param);
    LLTextBase* findTextBase() const;
    bool isClickable(EMode mode) const;

    void drawBox(EMode mode);
    void drawThumbnail();
    // The animation frame to draw now; `live` when it keeps playing on screen.
    LLViewerTexture* currentFrame(const LLChatImageAnim& anim, bool& live);
    bool isOnScreen() const;
    void drawFailure(const std::string& reason);

    std::string mURL;
    std::string mHost;
    std::string mPresetFailure;   // e.g. "WebP not supported yet" (no fetch)
    bool mRequested = false;      // a load was started for this widget
    bool mWasShown = false;       // a thumbnail was shown at least once
    bool mHidden = false;
    bool mHovered = false;
    bool mMouseDownInside = false;
    S32 mMaxWidth = 360;
    F64 mPlayTime = 0.0;          // seconds of animation played (visible draws only)
    F64 mLastPlayDraw = 0.0;      // wall clock of the last playing draw, 0 = paused
    bool mPlayFinished = false;   // all loops played (last frame stays)
    boost::signals2::scoped_connection mCacheConnection;
    LLHandle<LLView> mPopupMenuHandle;
};

#endif // LL_LLCHATIMAGEPREVIEW_H
