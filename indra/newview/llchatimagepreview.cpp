/**
 * @file llchatimagepreview.cpp
 * @brief Chat inline image previews (#265): the inline widget under a chat
 *        message and the per-message load policy.
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

#include "llchatimagepreview.h"

#include "llagent.h"
#include "llagentdata.h"
#include "llavataractions.h"
#include "llchat.h"
#include "llchatimagecache.h"
#include "llfloater.h"
#include "llfloaterreg.h"
#include "llfontgl.h"
#include "llinstantmessage.h"
#include "lllocalcliprect.h"
#include "llmenugl.h"
#include "llmutelist.h"
#include "llrender2dutils.h"
#include "lltextbase.h"
#include "lltexteditor.h"
#include "lltimer.h"
#include "lltrans.h"
#include "llui.h"
#include "lluicolortable.h"
#include "lluictrlfactory.h"
#include "llurlaction.h"
#include "llurlmatch.h"
#include "llurlregistry.h"
#include "llviewercontrol.h"
#include "llviewermenu.h"
#include "llviewertexture.h"
#include "llwindow.h"
#include "rlvactions.h"
#include "rlvhandler.h"

#include <set>

namespace
{
    constexpr S32 MAX_PREVIEWS_PER_MESSAGE = 3;
    constexpr S32 BOX_MAX_WIDTH = 360;     // click-to-load / loading box
    constexpr S32 BOX_HEIGHT = 50;
    constexpr S32 BOX_BORDER = 2;
    constexpr S32 ICON_SIZE = 24;
    constexpr S32 PAD = 10;

    // RLVa: no preview whenever the restrictions hide or filter this chat, hide who
    // sent it, or block texture previews.
    bool rlv_allows_preview(const LLChat& chat)
    {
        if (!RlvActions::isRlvEnabled())
        {
            return true;
        }
        if (chat.mRlvNamesFiltered || chat.mRlvLocFiltered)
        {
            return false;   // the text was rewritten by @shownames / @showloc
        }
        if (!RlvActions::canPreviewTextures())
        {
            return false;   // @viewtexture
        }
        if (chat.mFromID == gAgentID)
        {
            return true;
        }
        if (chat.mSessionID.isNull())
        {
            // Nearby chat: same test as the receive path (llviewermessage.cpp).
            if ((gRlvHandler.hasBehaviour(RLV_BHVR_RECVCHAT) && !gRlvHandler.isException(RLV_BHVR_RECVCHAT, chat.mFromID)) ||
                (gRlvHandler.hasBehaviour(RLV_BHVR_RECVCHATFROM) && gRlvHandler.isException(RLV_BHVR_RECVCHATFROM, chat.mFromID)))
            {
                return false;
            }
        }
        else
        {
            const LLUUID sender = gAgent.isInGroup(chat.mSessionID) ? chat.mSessionID : chat.mFromID;
            if (!RlvActions::canReceiveIM(sender))
            {
                return false;
            }
        }
        if (chat.mFromID.notNull() && !RlvActions::canShowName(RlvActions::SNC_DEFAULT, chat.mFromID))
        {
            return false;
        }
        return true;
    }

    // By value: getColor() returns a temporary LLUIColor whose fallback colour
    // would dangle if returned by reference.
    LLColor4 ui_color(const char* name)
    {
        return LLUIColorTable::instance().getColor(name).get();
    }
}

// static
void LLChatImagePreview::appendPreviews(LLTextEditor* editor, const LLChat& chat, const std::string& text,
                                        S32 left_pad, S32 right_pad, S32 column_width)
{
    static LLCachedControl<bool> previews_enabled(gSavedSettings, "ChatImagePreviews", true);
    static LLCachedControl<bool> from_objects(gSavedSettings, "ChatImagePreviewsFromObjects", false);
    static LLCachedControl<bool> auto_friends(gSavedSettings, "ChatImageAutoLoadFriends", true);
    static LLCachedControl<bool> auto_trusted(gSavedSettings, "ChatImageAutoLoadTrusted", true);
    static LLCachedControl<bool> auto_all(gSavedSettings, "ChatImageAutoLoadAll", false);

    if (!previews_enabled || !editor || text.empty())
    {
        return;
    }
    if (chat.mMuted || chat.mNotifId.notNull() || chat.mFromName == SYSTEM_FROM)
    {
        return;
    }
    const bool from_me = chat.mFromID.notNull() && chat.mFromID == gAgentID;
    const bool from_object = (chat.mSourceType == CHAT_SOURCE_OBJECT);
    if (from_object)
    {
        if (!from_objects)
        {
            return;
        }
    }
    else if (chat.mSourceType != CHAT_SOURCE_AGENT)
    {
        return;   // system, region, teleport, ...
    }
    if (!from_me)
    {
        LLMuteList* mutes = LLMuteList::getInstance();
        if ((chat.mFromID.notNull() && mutes->isMuted(chat.mFromID, chat.mFromName)) ||
            (from_object && chat.mOwnerID.notNull() && mutes->isMuted(chat.mOwnerID)))
        {
            return;
        }
    }
    if (!rlv_allows_preview(chat))
    {
        return;
    }

    const bool from_friend = !from_object && chat.mFromID.notNull() && LLAvatarActions::isFriend(chat.mFromID);
    const S32 max_width = llmax(64, column_width);

    std::set<std::string> seen;
    S32 added = 0;
    std::string remaining = text;
    LLUrlMatch match;
    S32 guard = 0;
    while (added < MAX_PREVIEWS_PER_MESSAGE && !remaining.empty() && ++guard < 64 &&
           LLUrlRegistry::instance().findUrl(remaining, match))
    {
        const std::string url = match.getUrl();
        const size_t next = (size_t)match.getEnd() + 1;
        remaining = (next < remaining.size()) ? remaining.substr(next) : std::string();

        std::string host, reason;
        if (!LLChatImageCache::checkUrl(url, host, reason))
        {
            continue;   // not http(s), or never fetchable (local / private address)
        }
        if (!LLChatImageCache::looksLikeImage(url, host))
        {
            continue;
        }
        if (!seen.insert(url).second)
        {
            continue;
        }

        // Fetching reveals our IP address to the site: only by itself for our own
        // messages, friends, trusted image sites, or when the user allows everyone.
        const bool auto_load = from_me ||
                               (auto_friends && from_friend) ||
                               (auto_trusted && LLChatImageCache::isTrustedHost(host)) ||
                               auto_all;

        LLUICtrl::Params p;
        p.name = "chat_image_preview";
        p.rect = LLRect(0, BOX_HEIGHT, llmin(BOX_MAX_WIDTH, max_width), 0);
        p.mouse_opaque = true;
        p.tab_stop = false;
        LLChatImagePreview* preview = LLUICtrlFactory::create<LLChatImagePreview>(p);
        if (!preview)
        {
            continue;
        }
        preview->setup(url, host, auto_load, std::string(), max_width);

        LLInlineViewSegment::Params seg;
        seg.view = preview;
        seg.force_newline = true;
        seg.left_pad = left_pad;
        seg.right_pad = right_pad;
        seg.top_pad = 4;
        seg.bottom_pad = 2;
        editor->appendWidget(seg, "\n", false);
        ++added;
    }
}

LLChatImagePreview::LLChatImagePreview(const LLUICtrl::Params& p)
:   LLUICtrl(p)
{
}

LLChatImagePreview::~LLChatImagePreview()
{
    if (LLView* menu = mPopupMenuHandle.get())
    {
        menu->die();
        mPopupMenuHandle.markDead();
    }
}

void LLChatImagePreview::setup(const std::string& url, const std::string& host, bool auto_load,
                               const std::string& preset_failure, S32 max_width)
{
    mURL = url;
    mHost = host;
    mPresetFailure = preset_failure;
    mMaxWidth = max_width;
    setToolTip(url);

    if (LLChatImageCache::instance().getState(mURL) == LLChatImageCache::STATE_READY)
    {
        mWasShown = true;
    }
    mCacheConnection = LLChatImageCache::instance().addUpdateCallback(
        [this](const std::string& changed) { onCacheUpdate(changed); });

    if (mPresetFailure.empty() && auto_load)
    {
        startLoad();
    }
    refreshLayout();
}

LLChatImagePreview::EMode LLChatImagePreview::currentMode(std::string* reason) const
{
    if (mHidden)
    {
        return MODE_HIDDEN;
    }
    if (!mPresetFailure.empty())
    {
        if (reason)
        {
            *reason = mPresetFailure;
        }
        return MODE_FAILED;
    }
    std::string why;
    switch (LLChatImageCache::instance().getState(mURL, &why))
    {
    case LLChatImageCache::STATE_QUEUED:
    case LLChatImageCache::STATE_FETCHING:
    case LLChatImageCache::STATE_DECODING:
        return MODE_LOADING;
    case LLChatImageCache::STATE_READY:
        return MODE_THUMBNAIL;
    case LLChatImageCache::STATE_FAILED:
        if (reason)
        {
            *reason = why;
        }
        return MODE_FAILED;
    case LLChatImageCache::STATE_NONE:
    default:
        return (mRequested || mWasShown) ? MODE_RELOAD : MODE_CLICK_TO_LOAD;
    }
}

bool LLChatImagePreview::isClickable(EMode mode) const
{
    return mode == MODE_CLICK_TO_LOAD || mode == MODE_RELOAD || mode == MODE_THUMBNAIL;
}

void LLChatImagePreview::desiredSize(EMode mode, S32& width, S32& height) const
{
    const S32 box_width = llmin(BOX_MAX_WIDTH, mMaxWidth);
    switch (mode)
    {
    case MODE_HIDDEN:
        width = 1;
        height = 1;
        return;
    case MODE_FAILED:
        width = llmin(mMaxWidth, 420);
        height = (S32)LLFontGL::getFontSansSerifSmall()->getLineHeight() + 4;
        return;
    case MODE_THUMBNAIL:
    {
        S32 orig_w = 0, orig_h = 0;
        if (LLChatImageCache::instance().getThumbnail(mURL, orig_w, orig_h) && orig_w > 0 && orig_h > 0)
        {
            static LLCachedControl<S32> max_height_setting(gSavedSettings, "ChatImagePreviewMaxHeight", 160);
            const S32 max_height = llclamp((S32)max_height_setting(), 64, 400);
            F32 h = (F32)llmin(max_height, orig_h);
            F32 w = h * (F32)orig_w / (F32)orig_h;
            if (w > (F32)mMaxWidth)
            {
                w = (F32)mMaxWidth;
                h = w * (F32)orig_h / (F32)orig_w;
            }
            width = llmax(1, ll_round(w));
            height = llmax(1, ll_round(h));
            return;
        }
        break;
    }
    default:
        break;
    }
    width = box_width;
    height = BOX_HEIGHT;
}

void LLChatImagePreview::refreshLayout()
{
    const EMode mode = currentMode();
    if (mode == MODE_THUMBNAIL)
    {
        mWasShown = true;
    }
    S32 width = 0, height = 0;
    desiredSize(mode, width, height);
    setVisible(mode != MODE_HIDDEN);
    if (width != getRect().getWidth() || height != getRect().getHeight())
    {
        reshape(width, height);
        // The inline segment reads our rect on reflow; re-flow the chat text so the
        // lines below move (needsReflow also dirties the retained floater).
        if (LLTextBase* text = findTextBase())
        {
            text->needsReflow();
        }
    }
    onRetainedDirty();
}

void LLChatImagePreview::onCacheUpdate(const std::string& url)
{
    if (url == mURL)
    {
        refreshLayout();
    }
}

LLTextBase* LLChatImagePreview::findTextBase() const
{
    for (LLView* view = getParent(); view; view = view->getParent())
    {
        if (LLTextBase* text = dynamic_cast<LLTextBase*>(view))
        {
            return text;
        }
    }
    return nullptr;
}

void LLChatImagePreview::startLoad()
{
    if (mURL.empty() || !mPresetFailure.empty())
    {
        return;
    }
    if (LLChatImageCache::instance().request(mURL))
    {
        mRequested = true;
    }
}

void LLChatImagePreview::openPreview()
{
    LLFloaterReg::showInstance("chat_image_preview", LLSD(mURL), true);
}

void LLChatImagePreview::draw()
{
    std::string reason;
    const EMode mode = currentMode(&reason);
    switch (mode)
    {
    case MODE_HIDDEN:
        break;
    case MODE_FAILED:
        drawFailure(reason);
        break;
    case MODE_THUMBNAIL:
        drawThumbnail();
        break;
    default:
        drawBox(mode);
        break;
    }
}

void LLChatImagePreview::drawBox(EMode mode)
{
    const LLRect rect = getLocalRect();
    const bool clickable = isClickable(mode);
    const LLColor4 accent = ui_color("ChatImagePlaceholderBorderColor");
    const LLColor4 background = (mHovered && clickable) ? ui_color("ChatImagePlaceholderHoverColor")
                                                        : ui_color("ChatImagePlaceholderBgColor");

    gl_rect_2d(rect, background, true);
    // Border as four filled strips (no line primitives).
    gl_rect_2d(rect.mLeft, rect.mTop, rect.mRight, rect.mTop - BOX_BORDER, accent, true);
    gl_rect_2d(rect.mLeft, rect.mBottom + BOX_BORDER, rect.mRight, rect.mBottom, accent, true);
    gl_rect_2d(rect.mLeft, rect.mTop, rect.mLeft + BOX_BORDER, rect.mBottom, accent, true);
    gl_rect_2d(rect.mRight - BOX_BORDER, rect.mTop, rect.mRight, rect.mBottom, accent, true);

    S32 text_left = rect.mLeft + PAD;
    LLUIImagePtr icon = LLUI::getUIImage("Inv_Texture");
    if (icon.notNull())
    {
        icon->draw(rect.mLeft + PAD, rect.mBottom + (rect.getHeight() - ICON_SIZE) / 2, ICON_SIZE, ICON_SIZE, accent);
        text_left += ICON_SIZE + PAD;
    }

    std::string title;
    switch (mode)
    {
    case MODE_LOADING:  title = LLTrans::getString("ChatImageLoading"); break;
    case MODE_RELOAD:   title = LLTrans::getString("ChatImageClickToReload"); break;
    default:            title = LLTrans::getString("ChatImageClickToLoad"); break;
    }
    LLStringUtil::format_map_t args;
    args["[HOST]"] = mHost;
    const std::string subtitle = LLTrans::getString("ChatImageFromHost", args);

    const LLFontGL* title_font = LLFontGL::getFontSansSerifBold();
    const LLFontGL* sub_font = LLFontGL::getFontSansSerifSmall();
    const S32 max_pixels = llmax(0, rect.mRight - PAD - text_left);
    const F32 center = (F32)(rect.mBottom + rect.getHeight() / 2);
    title_font->renderUTF8(title, 0, (F32)text_left, center + 2.f, ui_color("ChatImagePlaceholderTextColor"),
                           LLFontGL::LEFT, LLFontGL::BOTTOM, LLFontGL::NORMAL, LLFontGL::NO_SHADOW,
                           S32_MAX, max_pixels, nullptr, true);
    sub_font->renderUTF8(subtitle, 0, (F32)text_left, center - 2.f, ui_color("ChatImagePlaceholderSubTextColor"),
                         LLFontGL::LEFT, LLFontGL::TOP, LLFontGL::NORMAL, LLFontGL::NO_SHADOW,
                         S32_MAX, max_pixels, nullptr, true);
}

bool LLChatImagePreview::isOnScreen() const
{
    // Our screen rect (LLFontGL::sCurOrigin is the current UI origin, the space the
    // clip stack is in). drawChildren only culls against the window, not against
    // the chat's scroll clip.
    const LLRect screen(LLFontGL::sCurOrigin.mX, LLFontGL::sCurOrigin.mY + getRect().getHeight(),
                        LLFontGL::sCurOrigin.mX + getRect().getWidth(), LLFontGL::sCurOrigin.mY);
    LLRect clip;
    if (LLScreenClipRect::currentClipRect(clip) && !clip.overlaps(screen))
    {
        return false;
    }
    return true;
}

LLViewerTexture* LLChatImagePreview::currentFrame(const LLChatImageAnim& anim, bool& live)
{
    static LLCachedControl<S32> animate_setting(gSavedSettings, "ChatImageAnimate", 2);
    live = false;
    const S32 animate = animate_setting();
    if (animate <= 0 || (animate == 1 && !mHovered))
    {
        // Never: the first frame. On hover: rest on the first frame and start over
        // at the next hover.
        mLastPlayDraw = 0.0;
        mPlayTime = 0.0;
        mPlayFinished = false;
        return anim.frames.front().get();
    }
    bool finished = false;
    if (!isOnScreen())
    {
        mLastPlayDraw = 0.0;   // paused while scrolled away
        return anim.frames[anim.frameAt(mPlayTime, finished)].get();
    }
    // Play time advances only across consecutive draws: a preview that was not
    // drawn for a while (scrolled away, floater closed) continues where it was.
    const F64 now = LLTimer::getTotalSeconds();
    if (mLastPlayDraw > 0.0)
    {
        const F64 dt = now - mLastPlayDraw;
        if (dt > 0.0 && dt < 0.5)
        {
            mPlayTime += dt;
        }
    }
    mLastPlayDraw = now;
    const size_t index = anim.frameAt(mPlayTime, finished);
    if (finished)
    {
        if (!mPlayFinished)
        {
            mPlayFinished = true;
            mLastPlayDraw = 0.0;
            // Played out during a replay: re-record once so the last frame is
            // replayed instead of drawn live. (While recording, the last frame is
            // being recorded right now; a dirty then would keep the floater live.)
            if (!LLFloaterView::sRetainedCapturing)
            {
                onRetainedDirty();
            }
        }
    }
    else
    {
        live = true;
    }
    return anim.frames[index].get();
}

void LLChatImagePreview::drawThumbnail()
{
    S32 orig_w = 0, orig_h = 0;
    const LLChatImageAnim* anim = nullptr;
    LLViewerTexture* thumb = LLChatImageCache::instance().getThumbnail(mURL, orig_w, orig_h, &anim);
    const LLRect rect = getLocalRect();
    if (!thumb)
    {
        return;   // dropped this frame; the cache signal re-lays us out
    }
    bool live = false;
    if (anim)
    {
        thumb = currentFrame(*anim, live);
    }
    // A playing animation is a live section of the retained floater: every replay
    // draws this view fresh (the current frame), nothing else is re-recorded. It
    // must open before anything is drawn. A no-op when not recording.
    LLRetainedLiveScope live_scope(live ? this : nullptr);

    // The texture is stretched to power-of-two dims; our rect has the image's aspect.
    gl_draw_scaled_image(rect.mLeft, rect.mBottom, rect.getWidth(), rect.getHeight(), thumb, UI_VERTEX_COLOR);
    if (mHovered)
    {
        const LLColor4 accent = ui_color("ChatImagePlaceholderBorderColor");
        gl_rect_2d(rect.mLeft, rect.mTop, rect.mRight, rect.mTop - 1, accent, true);
        gl_rect_2d(rect.mLeft, rect.mBottom + 1, rect.mRight, rect.mBottom, accent, true);
        gl_rect_2d(rect.mLeft, rect.mTop, rect.mLeft + 1, rect.mBottom, accent, true);
        gl_rect_2d(rect.mRight - 1, rect.mTop, rect.mRight, rect.mBottom, accent, true);
    }
}

void LLChatImagePreview::drawFailure(const std::string& reason)
{
    LLStringUtil::format_map_t args;
    args["[REASON]"] = reason;
    const std::string line = LLTrans::getString("ChatImageUnavailable", args);
    const LLRect rect = getLocalRect();
    LLFontGL::getFontSansSerifSmall()->renderUTF8(line, 0, (F32)rect.mLeft, (F32)(rect.mBottom + 2),
                                                  ui_color("ChatImagePlaceholderSubTextColor"),
                                                  LLFontGL::LEFT, LLFontGL::BOTTOM, LLFontGL::NORMAL, LLFontGL::NO_SHADOW,
                                                  S32_MAX, rect.getWidth(), nullptr, true);
}

bool LLChatImagePreview::handleMouseDown(S32 x, S32 y, MASK mask)
{
    if (isClickable(currentMode()))
    {
        mMouseDownInside = true;
        return true;   // don't start a text selection under the preview
    }
    return LLUICtrl::handleMouseDown(x, y, mask);
}

bool LLChatImagePreview::handleMouseUp(S32 x, S32 y, MASK mask)
{
    const bool was_down = mMouseDownInside;
    mMouseDownInside = false;
    const EMode mode = currentMode();
    if (was_down && isClickable(mode) && pointInView(x, y))
    {
        if (mode == MODE_THUMBNAIL)
        {
            openPreview();
        }
        else
        {
            startLoad();
            refreshLayout();
        }
        return true;
    }
    return LLUICtrl::handleMouseUp(x, y, mask);
}

bool LLChatImagePreview::handleRightMouseDown(S32 x, S32 y, MASK mask)
{
    if (currentMode() != MODE_HIDDEN)
    {
        showContextMenu(x, y);
        return true;
    }
    return LLUICtrl::handleRightMouseDown(x, y, mask);
}

bool LLChatImagePreview::handleHover(S32 x, S32 y, MASK mask)
{
    if (isClickable(currentMode()))
    {
        getWindow()->setCursor(UI_CURSOR_HAND);
        return true;
    }
    return LLUICtrl::handleHover(x, y, mask);
}

void LLChatImagePreview::onMouseEnter(S32 x, S32 y, MASK mask)
{
    LLUICtrl::onMouseEnter(x, y, mask);
    if (!mHovered)
    {
        mHovered = true;
        onRetainedDirty();
    }
}

void LLChatImagePreview::onMouseLeave(S32 x, S32 y, MASK mask)
{
    LLUICtrl::onMouseLeave(x, y, mask);
    mMouseDownInside = false;
    if (mHovered)
    {
        mHovered = false;
        onRetainedDirty();
    }
}

void LLChatImagePreview::showContextMenu(S32 x, S32 y)
{
    LLMenuGL* menu = (LLMenuGL*)mPopupMenuHandle.get();
    if (!menu)
    {
        LLUICtrl::CommitCallbackRegistry::ScopedRegistrar registrar;
        LLUICtrl::EnableCallbackRegistry::ScopedRegistrar registrar_enable;
        registrar.add("ChatImage.Action", [this](LLUICtrl*, const LLSD& param) { onMenuAction(param); });
        registrar_enable.add("ChatImage.Visible", [this](LLUICtrl*, const LLSD& param) { return onMenuVisible(param); });
        menu = LLUICtrlFactory::getInstance()->createFromFile<LLMenuGL>("menu_chat_image_preview.xml", gMenuHolder,
                                                                         LLViewerMenuHolderGL::child_registry_t::instance());
        if (!menu)
        {
            LL_WARNS("ChatImage") << "Failed to create menu_chat_image_preview.xml" << LL_ENDL;
            return;
        }
        mPopupMenuHandle = menu->getHandle();
        menu->updateParent(LLMenuGL::sMenuContainer);
    }
    LLMenuGL::showPopup(this, menu, x, y);
}

bool LLChatImagePreview::onMenuVisible(const LLSD& param)
{
    const std::string what = param.asString();
    if (what == "has_thumbnail")
    {
        return currentMode() == MODE_THUMBNAIL;
    }
    return true;
}

void LLChatImagePreview::onMenuAction(const LLSD& param)
{
    const std::string what = param.asString();
    if (what == "copy")
    {
        LLUrlAction::copyURLToClipboard(mURL);
    }
    else if (what == "open_browser")
    {
        LLUrlAction::openURLExternal(mURL);
    }
    else if (what == "open_preview")
    {
        openPreview();
    }
    else if (what == "hide")
    {
        mHidden = true;
        refreshLayout();
    }
}
