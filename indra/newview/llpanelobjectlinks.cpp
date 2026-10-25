/**
 * @file llpanelobjectlinks.cpp
 * @brief Build floater "Links" tab (#252): linkset links + current link's faces.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
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
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "llpanelobjectlinks.h"

#include "llagent.h"
#include "llcallbacklist.h"
#include "llcheckboxctrl.h"
#include "llfiltereditor.h"
#include "llfloatertools.h"
#include "indra_constants.h"
#include "llgltfmaterial.h"
#include "lltexturedata.h"
#include "llkeyboard.h"
#include "llmaterial.h"
#include "llrender2dutils.h"
#include "llscrolllistcell.h"
#include "llscrolllistcolumn.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "llselectmgr.h"
#include "lltextbox.h"
#include "lltextureentry.h"
#include "lltoolface.h"
#include "lltoolmgr.h"
#include "lltooltip.h"
#include "llui.h"
#include "llviewborder.h"
#include "lluicolortable.h"
#include "lluictrlfactory.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llviewertexture.h"
#include "llviewertexturelist.h"
#include "message.h"
#include "rlvactions.h"

#include <deque>
#include <unordered_map>
#include <unordered_set>

LLPanelObjectLinks* LLPanelObjectLinks::sInstance = nullptr;

namespace
{
    // Link names / descriptions: LLSelectNode only holds them for selected prims, so
    // they're cached here whenever seen (whole linkset selected = every name arrives)
    // and fetched with RequestObjectPropertiesFamily for links never seen selected.
    struct NameDesc { std::string name, desc; };
    std::unordered_map<LLUUID, NameDesc> sNames;
    std::unordered_set<LLUUID>           sRequested;   // asked, reply pending
    std::deque<LLUUID>                   sQueue;       // to ask (rate-limited)
    bool                                 sDraining = false;
    constexpr size_t NAME_REQUESTS_PER_FRAME = 8;
    constexpr size_t NAME_CACHE_MAX = 8192;

    void cacheName(const LLUUID& id, const std::string& name, const std::string& desc)
    {
        if (sNames.size() > NAME_CACHE_MAX) sNames.clear();
        NameDesc& nd = sNames[id];
        nd.name = name;
        nd.desc = desc;
    }

    bool drainNameQueue()
    {
        size_t sent = 0;
        while (!sQueue.empty() && sent < NAME_REQUESTS_PER_FRAME)
        {
            const LLUUID id = sQueue.front();
            sQueue.pop_front();
            LLViewerObject* obj = gObjectList.findObject(id);
            LLViewerRegion* region = obj && !obj->isDead() ? obj->getRegion() : nullptr;
            if (!region || !gMessageSystem)
            {
                sRequested.erase(id);
                continue;
            }
            LLMessageSystem* msg = gMessageSystem;
            msg->newMessageFast(_PREHASH_RequestObjectPropertiesFamily);
            msg->nextBlockFast(_PREHASH_AgentData);
            msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
            msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
            msg->nextBlockFast(_PREHASH_ObjectData);
            msg->addU32Fast(_PREHASH_RequestFlags, 0);
            msg->addUUIDFast(_PREHASH_ObjectID, id);
            msg->sendReliable(region->getHost());
            ++sent;
        }
        sDraining = !sQueue.empty();
        return !sDraining;   // true = done, stop repeating
    }

    std::string lower(std::string s)
    {
        LLStringUtil::toLower(s);
        return s;
    }

    const char* alphaModeName(U8 legacy_mode)
    {
        switch (legacy_mode)
        {
        case LLMaterial::DIFFUSE_ALPHA_MODE_BLEND:    return "Blend";
        case LLMaterial::DIFFUSE_ALPHA_MODE_MASK:     return "Mask";
        case LLMaterial::DIFFUSE_ALPHA_MODE_EMISSIVE: return "Emissive";
        default:                                      return "None";
        }
    }

    // ---- Face rows: thumbnail + colour swatch (#252 follow-up) ---------------------
    // Every face cell is FACE_ROW_PX tall (the list sizes its rows from the tallest
    // cell) and draws its content centred in that height.
    constexpr S32 FACE_ROW_PX = 32;
    constexpr S32 FACE_THUMB_FETCH_PX = 256;   // thumbnail copies are scaled to <= this
    constexpr S32 FACE_SWATCH_PX = 18;

    const LLColor4& faceCellBorder()
    {
        static LLUIColor c = LLUIColorTable::instance().getColor("DefaultShadowDark");
        return c.get();
    }

    // Checkerboard behind alpha content; restores the UI's default clamp addressing
    // (gl_rect_2d_checkerboard leaves unit 0 on wrap, which would bleed the opposite
    // edge into a scaled thumbnail).
    void faceChecker(const LLRect& r, F32 a)
    {
        gl_rect_2d_checkerboard(r, a);
        gGL.getTexUnit(0)->setTextureAddressMode(LLTexUnit::TAM_CLAMP);
    }

    // LLScrollListItem's constructor is protected (the list builds items from params);
    // the face rows are built here so their custom cells exist before the list sizes
    // the row.
    struct FaceRowItem final : public LLScrollListItem
    {
        explicit FaceRowItem(const LLScrollListItem::Params& p) : LLScrollListItem(p) {}
    };

    // Text cell centred vertically in the tall face rows (stock text sits on the bottom).
    class FaceTextCell final : public LLScrollListText
    {
    public:
        explicit FaceTextCell(const LLScrollListCell::Params& p) : LLScrollListText(p)
        {
            if (p.value.isProvided()) setValue(p.value);
        }
        S32 getHeight() const override { return FACE_ROW_PX; }
        void draw(const LLColor4& color, const LLColor4& highlight_color) override
        {
            LLUI::pushMatrix();
            LLUI::translate(0.f, (F32)((FACE_ROW_PX - LLScrollListText::getHeight()) / 2));
            LLScrollListText::draw(color, highlight_color);
            LLUI::popMatrix();
        }
    };

    // The face's base texture: PBR base colour or legacy diffuse. Fetched as a
    // BOOST_THUMBNAIL copy (the scaled texture list, like inventory thumbnails): its own
    // instance, downscaled to FACE_THUMB_FETCH_PX before upload, so drawing it in the UI
    // (which keeps the UI texture's CPU copy) never pins the world texture's full-size
    // copy. Released with the row; the texture list flushes it 30 s later.
    class FaceThumbCell final : public LLScrollListCell
    {
    public:
        enum EKind : U8 { KIND_NONE, KIND_PENDING, KIND_TRANSPARENT, KIND_IMAGE };

        FaceThumbCell(const LLScrollListCell::Params& p, const LLUUID& tex, bool pending, bool dim)
        :   LLScrollListCell(p), mTexID(tex), mDim(dim)
        {
            if (pending)                                         mKind = KIND_PENDING;
            else if (tex.isNull())                               mKind = KIND_NONE;
            else if (tex == IMG_INVISIBLE || tex == IMG_TRANSPARENT) mKind = KIND_TRANSPARENT;
            else
            {
                mImage = LLViewerTextureManager::getFetchedTexture(tex, FTT_DEFAULT, MIPMAP_YES,
                                                                   LLGLTexture::BOOST_THUMBNAIL);
                if (mImage)
                {
                    mImage->setKnownDrawSize(FACE_THUMB_FETCH_PX, FACE_THUMB_FETCH_PX);
                    mKind = KIND_IMAGE;
                }
            }
        }

        S32 getHeight() const override { return FACE_ROW_PX; }
        const LLSD getValue() const override { return LLSD(mTexID); }
        const LLUUID& getTextureID() const { return mTexID; }
        // A real image the 256 px tooltip preview can show.
        bool hasPreview() const { return mKind == KIND_IMAGE && mImage && !mImage->isMissingAsset(); }

        // Idle tick while shown: keep the fetch prioritised (draw() doesn't run while the
        // floater replays its retained recording) and return a stamp of what draw() shows.
        U64 poll()
        {
            if (mKind != KIND_IMAGE || !mImage) return mKind;
            mImage->setKnownDrawSize(FACE_THUMB_FETCH_PX, FACE_THUMB_FETCH_PX);
            const LLTextureData* gl = mImage->getTextureData();
            U64 s = gl ? gl->getVKSerial() * 1000003ull + (U64)(S64)(gl->getVKGeneration() + 1) : 0;
            return s * 4 + (mImage->hasTextureData() ? 2 : 0) + (mImage->isMissingAsset() ? 1 : 0);
        }

        void draw(const LLColor4& color, const LLColor4& highlight_color) override
        {
            const S32 size = FACE_ROW_PX;
            const S32 left = llmax(0, (getWidth() - size) / 2);
            const LLRect frame(left, size, left + size, 0);
            LLRect in = frame;
            in.stretch(-1);
            const F32 a = mDim ? 0.5f : 1.f;
            gl_rect_2d(frame, faceCellBorder() % a, true);

            const LLColor4 loading(0.22f, 0.22f, 0.22f, a);
            switch (mKind)
            {
            case KIND_IMAGE:
                if (mImage->isMissingAsset())
                {
                    drawNone(in, a);
                }
                else if (!mImage->hasTextureData())
                {
                    gl_rect_2d(in, loading, true);   // still fetching
                }
                else
                {
                    if (mImage->getComponents() == 4) faceChecker(in, a);
                    gl_draw_scaled_image(in.mLeft, in.mBottom, in.getWidth(), in.getHeight(), mImage, UI_VERTEX_COLOR % a);
                }
                break;
            case KIND_TRANSPARENT:
                faceChecker(in, a);
                break;
            case KIND_PENDING:
                gl_rect_2d(in, loading, true);       // PBR material asset still loading
                break;
            default:
                drawNone(in, a);
                break;
            }
        }

    private:
        static void drawNone(const LLRect& r, F32 a)
        {
            gl_rect_2d(r, LLColor4::grey % a, true);
            gl_draw_x(r, LLColor4::black % a);
        }

        LLPointer<LLViewerFetchedTexture> mImage;
        LLUUID mTexID;
        EKind  mKind = KIND_NONE;
        bool   mDim = false;
    };

    // Hover preview of a face thumbnail at FACE_THUMB_FETCH_PX. Same BOOST_THUMBNAIL
    // instance and draw size as the row (LLTextureToolTip would grow that shared
    // instance to 4096 px and force-save its raw image).
    class FaceThumbPreview final : public LLView
    {
    public:
        explicit FaceThumbPreview(const LLView::Params& p) : LLView(p) {}

        void setTexture(const LLUUID& id)
        {
            mImage = id.notNull() ? LLViewerTextureManager::getFetchedTexture(id, FTT_DEFAULT, MIPMAP_YES,
                                                                              LLGLTexture::BOOST_THUMBNAIL)
                                  : nullptr;
            if (mImage) mImage->setKnownDrawSize(FACE_THUMB_FETCH_PX, FACE_THUMB_FETCH_PX);
        }

        void draw() override
        {
            LLView::draw();
            if (!mImage) return;
            const LLRect r = getLocalRect();
            if (!mImage->hasTextureData())
            {
                gl_rect_2d(r, LLColor4(0.22f, 0.22f, 0.22f, 1.f), true);
            }
            else
            {
                if (mImage->getComponents() == 4) faceChecker(r, 1.f);
                gl_draw_scaled_image(r.mLeft, r.mBottom, r.getWidth(), r.getHeight(), mImage);
            }
            mImage->setKnownDrawSize(FACE_THUMB_FETCH_PX, FACE_THUMB_FETCH_PX);
        }

    private:
        LLPointer<LLViewerFetchedTexture> mImage;
    };

    // Tooltip: the text (kind, UUID, media note) over a FACE_THUMB_FETCH_PX preview,
    // styled as the inventory texture tooltip. create_params = the texture UUID.
    class FaceThumbToolTip final : public LLToolTip
    {
    public:
        explicit FaceThumbToolTip(const LLToolTip::Params& p) : LLToolTip(p)
        {
            mMaxWidth = llmax(mMaxWidth, FACE_THUMB_FETCH_PX);
            setBackgroundColor(LLColor4::black);
            setTransparentColor(LLColor4::black);
            setBorderVisible(true);
        }

        void initFromParams(const LLToolTip::Params& p) override
        {
            LLToolTip::initFromParams(p);

            LLView::Params vp;
            vp.name = "face_thumb_preview";
            LLRect r;
            r.setOriginAndSize(mPadding, mTextBox->getRect().mTop, FACE_THUMB_FETCH_PX, FACE_THUMB_FETCH_PX);
            vp.rect = r;
            FaceThumbPreview* preview = LLUICtrlFactory::create<FaceThumbPreview>(vp);
            preview->setTexture(p.create_params().asUUID());
            addChild(preview);

            LLViewBorder::Params bp;
            bp.border_thickness(LLPANEL_BORDER_WIDTH);
            bp.highlight_light_color(LLColor4::white);
            bp.highlight_dark_color(LLColor4::white);
            bp.shadow_light_color(LLColor4::white);
            bp.shadow_dark_color(LLColor4::white);
            addBorder(bp);
            setBorderVisible(true);
            setBackgroundColor(LLColor4::black);
            setBackgroundVisible(true);
            setBackgroundOpaque(true);
            setBackgroundImage(nullptr);
            setTransparentImage(nullptr);
            mTextBox->setColor(LLColor4::white);

            snapToChildren();
        }
    };

    // Colour swatch: left half the opaque colour, right half the colour with its alpha
    // over a checkerboard.
    class FaceColorCell final : public LLScrollListCell
    {
    public:
        FaceColorCell(const LLScrollListCell::Params& p, const LLColor4& color, bool dim)
        :   LLScrollListCell(p), mColor(color), mDim(dim) {}

        S32 getHeight() const override { return FACE_ROW_PX; }
        const LLSD getValue() const override { return mColor.getValue(); }

        void draw(const LLColor4& color, const LLColor4& highlight_color) override
        {
            const S32 size = FACE_SWATCH_PX;
            const S32 left = llmax(0, (getWidth() - size) / 2);
            const S32 bottom = (FACE_ROW_PX - size) / 2;
            const LLRect frame(left, bottom + size, left + size, bottom);
            LLRect in = frame;
            in.stretch(-1);
            const F32 a = mDim ? 0.5f : 1.f;
            gl_rect_2d(frame, faceCellBorder() % a, true);

            const S32 mid = in.mLeft + in.getWidth() / 2;
            const LLRect solid(in.mLeft, in.mTop, mid, in.mBottom);
            const LLRect alpha(mid, in.mTop, in.mRight, in.mBottom);
            LLColor4 c = mColor;
            c.mV[VALPHA] = a;
            gl_rect_2d(solid, c, true);
            faceChecker(alpha, a);
            c.mV[VALPHA] = mColor.mV[VALPHA] * a;
            gl_rect_2d(alpha, c, true);
        }

    private:
        LLColor4 mColor;
        bool     mDim = false;
    };
}

LLPanelObjectLinks::LLPanelObjectLinks()
:   LLPanel()
{
    sInstance = this;
}

LLPanelObjectLinks::~LLPanelObjectLinks()
{
    gIdleCallbacks.deleteFunction(onIdleThumbs, this);
    if (sInstance == this) sInstance = nullptr;
}

bool LLPanelObjectLinks::postBuild()
{
    setMouseOpaque(false);

    mLinksList    = getChild<LLScrollListCtrl>("links_list");
    mFacesList    = getChild<LLScrollListCtrl>("faces_list");
    mFilterEditor = getChild<LLFilterEditor>("links_filter");
    mLinksCount   = getChild<LLTextBox>("links_count");
    mFacesLabel   = getChild<LLTextBox>("faces_label");

    mFilterEditor->setCommitCallback([this](LLUICtrl*, const LLSD&) { onFilterEdit(); });
    mLinksList->setCommitCallback([this](LLUICtrl*, const LLSD&) { onLinkCommit(); });
    mFacesList->setCommitCallback([this](LLUICtrl*, const LLSD&) { onFaceCommit(); });

    // Selection changed / properties arrived anywhere (in-world click, edit-linked
    // toggle, other panels): resync. The floater's refresh() also calls refresh().
    mSelectionConnection = LLSelectMgr::getInstance()->mUpdateSignal.connect([this]() { refresh(); });
    gIdleCallbacks.addFunction(onIdleThumbs, this);
    return true;
}

void LLPanelObjectLinks::onVisibilityChange(bool new_visibility)
{
    LLPanel::onVisibilityChange(new_visibility);
    if (new_visibility) refresh();
}

// static
bool LLPanelObjectLinks::linkEditable(LLViewerObject* obj)
{
    return linkSelectable(obj) && obj->permModify();
}

// Selecting a link (or a face) only needs what clicking the prim in world needs: no
// modify permission (the edit panels show a no-modify selection read-only, as in LL).
bool LLPanelObjectLinks::linkSelectable(LLViewerObject* obj)
{
    if (!obj || obj->isDead()) return false;
    if (RlvActions::isRlvEnabled() && !RlvActions::canEdit(obj)) return false;
    return true;
}

LLViewerObject* LLPanelObjectLinks::currentRoot() const
{
    LLObjectSelectionHandle sel = LLSelectMgr::getInstance()->getSelection();
    if (sel->isEmpty()) return nullptr;
    LLViewerObject* first = sel->getFirstObject();
    LLViewerObject* root = first ? first->getRootEdit() : nullptr;
    if (!root || root->isDead() || root->isAvatar()) return nullptr;
    return root;
}

void LLPanelObjectLinks::refresh()
{
    if (!mLinksList || mSyncing) return;

    // Harvest names from the selection's property replies -- even while the tab is
    // hidden, so a linkset selected whole on another tab has every name cached before
    // Edit linked narrows the selection.
    LLObjectSelectionHandle sel = LLSelectMgr::getInstance()->getSelection();
    for (LLObjectSelection::iterator it = sel->begin(); it != sel->end(); ++it)
    {
        LLSelectNode* node = *it;
        LLViewerObject* o = node ? node->getObject() : nullptr;
        if (o && node->mValid)
        {
            auto found = sNames.find(o->getID());
            if (found == sNames.end() || found->second.name != node->mName || found->second.desc != node->mDescription)
            {
                cacheName(o->getID(), node->mName, node->mDescription);
                updateNameCell(o->getID());
            }
        }
    }

    // Hidden tab / floater: nothing else to keep current (onVisibilityChange catches up).
    if (!isInVisibleChain()) return;

    rebuildLinks(currentRoot());
    rebuildFaces();
    syncListSelection();
    requestMissingNames();
}

void LLPanelObjectLinks::rebuildLinks(LLViewerObject* root)
{
    mLinks.clear();
    mRootID.setNull();
    if (root)
    {
        mRootID = root->getID();
        const LLViewerObject::const_child_list_t& children = root->getChildren();
        // LL link numbers: an unlinked prim is 0, the root 1, children 2.. in order.
        mLinks.push_back({ root->getID(), children.empty() ? 0 : 1, linkEditable(root) });
        S32 num = 1;
        for (LLViewerObject* child : children)
        {
            ++num;
            if (!child || child->isAvatar()) continue;   // seated avatars aren't links here
            mLinks.push_back({ child->getID(), num, linkEditable(child) });
        }
    }

    // Shown rows: the filter over number / name / desc.
    std::vector<const LinkRow*> shown;
    shown.reserve(mLinks.size());
    const bool digits = !mFilter.empty() && mFilter.find_first_not_of("0123456789") == std::string::npos;
    for (const LinkRow& r : mLinks)
    {
        if (!mFilter.empty())
        {
            bool match = digits && std::to_string(r.link_num).compare(0, mFilter.size(), mFilter) == 0;
            if (!match)
            {
                auto it = sNames.find(r.id);
                if (it != sNames.end())
                    match = lower(it->second.name).find(mFilter) != std::string::npos
                         || lower(it->second.desc).find(mFilter) != std::string::npos;
            }
            if (!match) continue;
        }
        shown.push_back(&r);
    }

    std::string key = mFilter;
    key.reserve(key.size() + shown.size() * 40);
    for (const LinkRow* r : shown)
    {
        key += r->id.asString();
        key += r->editable ? '+' : '-';
    }
    LLStringUtil::format_map_t args;
    args["[SHOWN]"] = std::to_string(shown.size());
    args["[TOTAL]"] = std::to_string(mLinks.size());
    mLinksCount->setText(mLinks.empty() ? getString("no_linkset") : getString("links_count", args));
    if (key == mLinksKey) return;   // unchanged: keep rows (and the hover highlight)
    mLinksKey = key;

    // The list draws only its visible rows (LLScrollListCtrl clips to the page), so 255
    // rows cost nothing per frame; rows are only rebuilt on a real change.
    const S32 scroll = mLinksList->getScrollPos();
    mLinksList->clearRows();
    const LLColor4 grey = LLUIColorTable::instance().getColor("LabelDisabledColor");
    const std::string not_editable = getString("not_editable");
    const std::string unnamed = getString("name_pending");
    for (const LinkRow* r : shown)
    {
        LLScrollListItem::Params row;
        row.value = LLSD(r->id);
        auto it = sNames.find(r->id);
        const std::string name = it != sNames.end() ? it->second.name : unnamed;
        std::string tip = it != sNames.end() ? it->second.desc : std::string();
        if (!r->editable) tip = tip.empty() ? not_editable : tip + "\n" + not_editable;

        LLScrollListCell::Params num;
        num.column = "num";
        num.value = LLSD(r->link_num);
        num.font_halign = LLFontGL::RIGHT;
        if (!r->editable) num.color = grey;
        row.columns.add(num);

        LLScrollListCell::Params nm;
        nm.column = "name";
        nm.value = LLSD(name);
        nm.tool_tip = tip.empty() ? name : name + "\n" + tip;
        if (!r->editable) nm.color = grey;
        row.columns.add(nm);
        // Items stay enabled even when not editable: LLScrollListCtrl::hitItem skips
        // disabled rows, which would also kill their hover preview. The commit
        // handlers refuse the selection instead.
        mLinksList->addRow(row);
    }
    mLinksList->setScrollPos(scroll);
}

void LLPanelObjectLinks::updateNameCell(const LLUUID& id)
{
    if (!mLinksList) return;
    LLScrollListItem* item = mLinksList->getItem(LLSD(id));
    if (!item) return;
    auto it = sNames.find(id);
    if (it == sNames.end()) return;
    if (LLScrollListCell* cell = item->getColumn(1))
    {
        cell->setValue(it->second.name);
        std::string tip = it->second.desc;
        cell->setToolTip(tip.empty() ? it->second.name : it->second.name + "\n" + tip);
    }
    mLinksList->onRetainedDirty();
    // A name that now matches / unmatches the filter: rebuild on the next refresh.
    if (!mFilter.empty()) mLinksKey.clear();
}

void LLPanelObjectLinks::requestMissingNames()
{
    for (const LinkRow& r : mLinks)
    {
        if (sNames.count(r.id) || sRequested.count(r.id)) continue;
        // Selected links get their name in the selection's ObjectProperties reply.
        LLViewerObject* o = gObjectList.findObject(r.id);
        if (o && o->isSelected()) continue;
        sRequested.insert(r.id);
        sQueue.push_back(r.id);
    }
    if (!sQueue.empty() && !sDraining)
    {
        sDraining = true;
        doOnIdleRepeating(drainNameQueue);
    }
}

// static
bool LLPanelObjectLinks::processPropertiesFamily(const LLUUID& id, const std::string& name, const std::string& desc)
{
    if (!sRequested.erase(id)) return false;
    cacheName(id, name, desc);
    if (sInstance) sInstance->updateNameCell(id);
    return true;
}

void LLPanelObjectLinks::rebuildFaces()
{
    // Face source: the link last clicked in the list while it stays selected, else the
    // first selected link (Edit linked / Select face) or the root.
    LLObjectSelectionHandle sel = LLSelectMgr::getInstance()->getSelection();
    LLViewerObject* obj = nullptr;
    if (mLastClickedLink.notNull())
    {
        LLViewerObject* o = gObjectList.findObject(mLastClickedLink);
        if (o && !o->isDead() && o->isSelected() && o->getRootEdit() && o->getRootEdit()->getID() == mRootID)
            obj = o;
    }
    if (!obj && mRootID.notNull())
    {
        const bool individual = gSavedSettings.getBOOL("EditLinkedParts") || LLSelectMgr::getInstance()->getTEMode();
        LLViewerObject* first = individual ? sel->getFirstObject() : nullptr;
        obj = first ? first : gObjectList.findObject(mRootID);
    }
    if (obj && (obj->isDead() || obj->isAvatar())) obj = nullptr;

    std::string key;
    S32 link_num = 0;
    if (obj)
    {
        mFaceLinkID = obj->getID();
        for (const LinkRow& r : mLinks)
            if (r.id == mFaceLinkID) { link_num = r.link_num; break; }
        key = mFaceLinkID.asString();
    }
    else
    {
        mFaceLinkID.setNull();
    }

    struct FaceInfo
    {
        S32 te;
        std::string type, alpha, tex_short;
        LLUUID tex;
        LLColor4 color;          // swatch: TE colour, or the PBR base colour factor (sRGB)
        bool pbr = false;
        bool pending = false;    // PBR material asset still loading
        bool media = false;
    };
    std::vector<FaceInfo> faces;
    if (obj)
    {
        const S32 nte = obj->getNumTEs();
        faces.reserve(nte);
        for (S32 te = 0; te < nte; ++te)
        {
            const LLTextureEntry* tep = obj->getTE((U8)te);
            if (!tep) continue;
            FaceInfo f;
            f.te = te;
            LLGLTFMaterial* gltf = tep->getGLTFRenderMaterial();
            f.media = tep->hasMedia();
            f.color = tep->getColor();
            if (gltf || obj->getRenderMaterialID((U8)te).notNull())
            {
                f.type = "PBR";
                f.pbr = true;
                if (gltf)
                {
                    f.alpha = gltf->mAlphaMode == LLGLTFMaterial::ALPHA_MODE_BLEND ? "Blend"
                            : gltf->mAlphaMode == LLGLTFMaterial::ALPHA_MODE_MASK  ? "Mask" : "Opaque";
                    f.tex = gltf->mTextureId[LLGLTFMaterial::GLTF_TEXTURE_INFO_BASE_COLOR];
                    f.color = srgbColor4(gltf->mBaseColor);   // linear factor -> display
                }
                else
                {
                    f.alpha = "...";   // material asset still loading
                    f.pending = true;
                }
            }
            else
            {
                f.type = tep->getMaterialParams().notNull() ? "Legacy+M" : "Legacy";
                if (tep->getMaterialParams().notNull())
                {
                    f.alpha = alphaModeName(tep->getMaterialParams()->getDiffuseAlphaMode());
                }
                else
                {
                    // No material: LL blends when the face colour or diffuse has alpha.
                    LLViewerTexture* img = obj->getTEImage((U8)te);
                    const bool blend = tep->getAlpha() < 0.999f || (img && img->getComponents() == 4);
                    f.alpha = blend ? "Blend" : "None";
                }
                f.tex = tep->getID();
            }
            f.tex_short = f.tex.notNull() ? f.tex.asString().substr(0, 8) : std::string("-");
            key += llformat("|%d%s%s%s%d%d%.3f,%.3f,%.3f,%.3f", te, f.type.c_str(), f.alpha.c_str(),
                            f.tex.asString().c_str(), (S32)f.pending, (S32)f.media,
                            f.color.mV[VRED], f.color.mV[VGREEN], f.color.mV[VBLUE], f.color.mV[VALPHA]);
            faces.push_back(std::move(f));
        }
    }

    if (obj)
    {
        LLStringUtil::format_map_t args;
        args["[NUM]"] = std::to_string(link_num);
        auto it = sNames.find(mFaceLinkID);
        args["[NAME]"] = it != sNames.end() ? it->second.name : getString("name_pending");
        mFacesLabel->setText(getString("faces_of_link", args));
    }
    else
    {
        mFacesLabel->setText(getString("faces_none"));
    }

    if (key == mFacesKey) return;
    mFacesKey = key;

    mFacesList->clearRows();
    const bool editable = linkEditable(obj);
    const LLColor4 grey = LLUIColorTable::instance().getColor("LabelDisabledColor");
    const S32 ncols = mFacesList->getNumColumns();
    for (const FaceInfo& f : faces)
    {
        LLScrollListItem::Params row;
        row.value = LLSD(f.te);
        LLScrollListItem* item = new FaceRowItem(row);
        item->setNumColumns(ncols);

        // Cells are built here (not from row.columns) so the tall thumbnail / swatch
        // cells are in place when the list sizes the row (addItem). With no
        // row.columns, addRow fills column 0 with a stock text cell of the item value;
        // ours replaces it after.
        LLScrollListCell* col0 = nullptr;
        auto put = [&](const char* column, LLScrollListCell* cell)
        {
            LLScrollListColumn* col = mFacesList->getColumn(std::string(column));
            if (!col || col->mIndex < 0 || col->mIndex >= ncols) { delete cell; return; }
            if (col->mIndex == 0) { delete col0; col0 = cell; }
            else item->setColumn(col->mIndex, cell);
        };
        auto params = [&](const char* column, const LLSD& value, const std::string& tip, bool right)
        {
            LLScrollListCell::Params c;
            c.column = column;
            c.value = value;
            if (LLScrollListColumn* col = mFacesList->getColumn(std::string(column))) c.width = col->getWidth();
            if (!tip.empty()) c.tool_tip = tip;
            if (right) c.font_halign = LLFontGL::RIGHT;
            if (!editable) c.color = grey;
            return c;
        };

        std::string thumb_tip;
        if (f.pending)            thumb_tip = getString("thumb_pending");
        else if (f.tex.isNull())  thumb_tip = getString("thumb_none");
        else
        {
            thumb_tip = getString(f.pbr ? "thumb_pbr" : "thumb_diffuse") + "\n" + f.tex.asString();
            if (f.tex == IMG_INVISIBLE || f.tex == IMG_TRANSPARENT) thumb_tip += "\n" + getString("thumb_transparent");
        }
        if (f.media) thumb_tip += "\n" + getString("thumb_media");

        LLStringUtil::format_map_t cargs;
        cargs["[R]"] = std::to_string(ll_round(f.color.mV[VRED] * 255.f));
        cargs["[G]"] = std::to_string(ll_round(f.color.mV[VGREEN] * 255.f));
        cargs["[B]"] = std::to_string(ll_round(f.color.mV[VBLUE] * 255.f));
        cargs["[A]"] = std::to_string(ll_round(f.color.mV[VALPHA] * 100.f));
        const std::string color_tip = getString(f.pbr ? "color_pbr" : "color_te", cargs);

        put("face",    new FaceTextCell(params("face", LLSD(f.te), std::string(), true)));
        put("thumb",   new FaceThumbCell(params("thumb", LLSD(f.tex), thumb_tip, false), f.tex, f.pending, !editable));
        put("color",   new FaceColorCell(params("color", f.color.getValue(), color_tip, false), f.color, !editable));
        put("type",    new FaceTextCell(params("type", LLSD(f.type), std::string(), false)));
        put("alpha",   new FaceTextCell(params("alpha", LLSD(f.alpha), std::string(), false)));
        put("texture", new FaceTextCell(params("texture", LLSD(f.tex_short),
                                               f.tex.notNull() ? f.tex.asString() : std::string("-"), false)));

        if (!mFacesList->addRow(item, row))
        {
            delete item;   // refused before the list took it
            delete col0;
            continue;
        }
        if (col0) item->setColumn(0, col0);
    }
    mThumbStamp = 0;   // new cells: re-stamp on the next idle poll
}

void LLPanelObjectLinks::syncListSelection()
{
    // Mirror LLSelectMgr into the lists (only when it differs, so the retained UI isn't
    // re-recorded on every properties reply).
    mSyncing = true;
    {
        std::vector<LLScrollListItem*> items = mLinksList->getAllData();
        bool differs = false;
        for (LLScrollListItem* item : items)
        {
            LLViewerObject* o = gObjectList.findObject(item->getUUID());
            const bool want = o && o->isSelected();
            if (want != item->getSelected()) { differs = true; break; }
        }
        if (differs)
        {
            mLinksList->deselectAllItems(true);
            uuid_vec_t ids;
            for (LLScrollListItem* item : items)
            {
                LLViewerObject* o = gObjectList.findObject(item->getUUID());
                if (o && o->isSelected()) ids.push_back(item->getUUID());
            }
            if (!ids.empty()) mLinksList->selectMultiple(ids);
        }
    }
    {
        LLViewerObject* obj = mFaceLinkID.notNull() ? gObjectList.findObject(mFaceLinkID) : nullptr;
        LLSelectNode* node = obj ? LLSelectMgr::getInstance()->getSelection()->findNode(obj) : nullptr;
        const bool te_mode = LLSelectMgr::getInstance()->getTEMode();
        std::vector<LLScrollListItem*> items = mFacesList->getAllData();
        for (LLScrollListItem* item : items)
        {
            const S32 te = item->getValue().asInteger();
            const bool want = node && te_mode && node->isTESelected(te);
            if (want != item->getSelected())
                mFacesList->setSelectedByValue(te, want);
        }
    }
    mSyncing = false;
}

void LLPanelObjectLinks::onFilterEdit()
{
    mFilter = lower(mFilterEditor->getText());
    LLStringUtil::trim(mFilter);
    mLinksKey.clear();
    refresh();
}

LLScrollListItem* LLPanelObjectLinks::clickedItem(LLScrollListCtrl* list) const
{
    // Mouse click: the row under the mouse (the list's own Shift range / Ctrl toggle is
    // overridden by the LLSelectMgr resync). Keyboard: the list's last selected row.
    S32 mx = 0, my = 0;
    LLUI::getInstance()->getMousePositionScreen(&mx, &my);
    if (LLScrollListItem* item = hoveredItem(list, mx, my)) return item;
    return list->getLastSelectedItem();
}

void LLPanelObjectLinks::onLinkCommit()
{
    if (mSyncing) return;
    LLScrollListItem* item = clickedItem(mLinksList);
    LLViewerObject* obj = item ? gObjectList.findObject(item->getUUID()) : nullptr;
    if (!obj || !linkSelectable(obj))
    {
        syncListSelection();   // refuse: put the list back
        return;
    }
    LLSelectMgr* sm = LLSelectMgr::getInstance();
    const MASK mask = gKeyboard ? gKeyboard->currentMask(true) : MASK_NONE;
    const bool extend = (mask & (MASK_SHIFT | MASK_CONTROL)) != 0;

    // "Edit linked parts" on, as clicking a prim with it on (LLToolSelect ignore_group).
    if (!gSavedSettings.getBOOL("EditLinkedParts"))
    {
        gSavedSettings.setBOOL("EditLinkedParts", true);
        if (gFloaterTools && gFloaterTools->mCheckSelectIndividual)
            gFloaterTools->mCheckSelectIndividual->set(true);
        sm->demoteSelectionToIndividuals();
        if (gFloaterTools) gFloaterTools->dirty();
    }

    mSyncing = true;
    if (extend)
    {
        if (obj->isSelected())
            sm->deselectObjectOnly(obj);
        else
            sm->selectObjectOnly(obj, SELECT_ALL_TES);
    }
    else
    {
        F32 target_zoom, current_zoom;
        LLVector3 target_pan, current_pan;
        sm->getAgentHUDZoom(target_zoom, current_zoom, target_pan, current_pan);
        sm->deselectAll();
        sm->selectObjectOnly(obj, SELECT_ALL_TES);
        sm->setAgentHUDZoom(target_zoom, current_zoom, target_pan, current_pan);
    }
    mSyncing = false;
    mLastClickedLink = obj->isSelected() ? obj->getID() : LLUUID::null;
    mFacePickCommit = false;
    refresh();
}

void LLPanelObjectLinks::onFaceCommit()
{
    if (mSyncing) return;
    LLScrollListItem* item = clickedItem(mFacesList);
    LLViewerObject* obj = mFaceLinkID.notNull() ? gObjectList.findObject(mFaceLinkID) : nullptr;
    if (!item || !obj || !linkSelectable(obj))
    {
        syncListSelection();
        return;
    }
    const S32 te = item->getValue().asInteger();
    if (te < 0 || te >= obj->getNumTEs()) return;

    LLSelectMgr* sm = LLSelectMgr::getInstance();
    const MASK mask = gKeyboard ? gKeyboard->currentMask(true) : MASK_NONE;
    // LLToolFace extends with Shift; Ctrl does the same here (list convention).
    const bool extend = (mask & (MASK_SHIFT | MASK_CONTROL)) != 0;

    mSyncing = true;
    // "Select face" mode, as the edit radio does (LLToolFace::handleSelect sets TE mode;
    // updatePopup re-syncs the radio from the current tool).
    if (LLToolMgr::getInstance()->getCurrentTool() != LLToolFace::getInstance())
        LLFloaterTools::setEditTool(LLToolFace::getInstance());

    // LLToolFace::pickCallback semantics.
    if (extend)
    {
        if (!obj->isSelected())
            sm->selectObjectOnly(obj, te);
        else if (!sm->getSelection()->contains(obj, te))
            sm->addAsIndividual(obj, te);
        else
            sm->remove(obj, te);
    }
    else
    {
        sm->deselectAll();
        sm->selectObjectOnly(obj, te);
    }
    mSyncing = false;
    mLastClickedLink = obj->getID();
    mFacePickCommit = true;
    refresh();
}

// #969: a link pick selects every face of the prim; a face pick (list or in-world face
// tool) leaves TE mode on with a partial face selection. A single-face prim is
// ambiguous, so the last list commit breaks the tie.
bool LLPanelObjectLinks::isFacePick() const
{
    LLSelectMgr* sm = LLSelectMgr::getInstance();
    if (!sm->getTEMode()) return false;
    LLObjectSelectionHandle sel = sm->getSelection();
    if (sel.isNull() || sel->isEmpty()) return false;
    for (LLObjectSelection::iterator it = sel->begin(); it != sel->end(); ++it)
    {
        LLSelectNode* node = *it;
        LLViewerObject* o = node ? node->getObject() : nullptr;
        if (!o) continue;
        const S32 n = o->getNumTEs();
        for (S32 te = 0; te < n; ++te)
        {
            if (!node->isTESelected(te)) return true;   // partial: faces were picked
        }
    }
    return mFacePickCommit;
}

// static
LLScrollListItem* LLPanelObjectLinks::hoveredItem(LLScrollListCtrl* list, S32 mx, S32 my)
{
    if (!list) return nullptr;
    const S32 idx = list->getHighlightedItemInx();
    if (idx < 0) return nullptr;
    // The highlight only clears on mouse-leave: below the last row it lingers.
    LLRect r = list->calcScreenRect();
    if (!r.pointInRect(mx, my)) return nullptr;
    S32 lx = 0, ly = 0;
    list->screenPointToLocal(mx, my, &lx, &ly);
    return list->hitItem(lx, ly);
}

// static
bool LLPanelObjectLinks::getHoverTarget(LLUUID& object_id, S32& te)
{
    LLPanelObjectLinks* self = sInstance;
    if (!self || !self->mLinksList || !self->isInVisibleChain()) return false;
    S32 mx = 0, my = 0;
    LLUI::getInstance()->getMousePositionScreen(&mx, &my);
    if (LLScrollListItem* item = hoveredItem(self->mFacesList, mx, my))
    {
        if (self->mFaceLinkID.isNull()) return false;
        object_id = self->mFaceLinkID;
        te = item->getValue().asInteger();
        return true;
    }
    if (LLScrollListItem* item = hoveredItem(self->mLinksList, mx, my))
    {
        object_id = item->getUUID();
        te = -1;
        return object_id.notNull();
    }
    return false;
}

// static
void LLPanelObjectLinks::onIdleThumbs(void* data)
{
    if (LLPanelObjectLinks* self = static_cast<LLPanelObjectLinks*>(data)) self->pollThumbs();
}

void LLPanelObjectLinks::pollThumbs()
{
    // Thumbnails load (and sharpen) after their rows were drawn; a retained floater
    // replays the old recording until told, so report the change.
    if (!mFacesList || mFacesList->getItemCount() == 0 || !isInVisibleChain()) return;
    LLScrollListColumn* col = mFacesList->getColumn(std::string("thumb"));
    if (!col) return;
    U64 stamp = 1469598103934665603ull;
    for (LLScrollListItem* item : mFacesList->getAllData())
    {
        if (FaceThumbCell* thumb = dynamic_cast<FaceThumbCell*>(item->getColumn(col->mIndex)))
            stamp = (stamp ^ thumb->poll()) * 1099511628211ull;
    }
    if (stamp != mThumbStamp)
    {
        mThumbStamp = stamp;
        mFacesList->onRetainedDirty();
    }
}

bool LLPanelObjectLinks::handleToolTip(S32 x, S32 y, MASK mask)
{
    // The list only tooltips text cells: thumbnail (256 px preview) and swatch here.
    if (mFacesList && mFacesList->getVisible())
    {
        S32 lx = 0, ly = 0;
        localPointToOtherView(x, y, &lx, &ly, mFacesList);
        LLScrollListItem* item = mFacesList->pointInView(lx, ly) ? mFacesList->hitItem(lx, ly) : nullptr;
        const S32 col = item ? mFacesList->getColumnIndexFromOffset(lx) : -1;
        LLScrollListCell* cell = item && col >= 0 ? item->getColumn(col) : nullptr;
        const FaceThumbCell* thumb = dynamic_cast<const FaceThumbCell*>(cell);
        if (cell && (thumb || dynamic_cast<const FaceColorCell*>(cell)) && !cell->getToolTip().empty())
        {
            LLRect sticky;
            mFacesList->localRectToScreen(mFacesList->getCellRect(mFacesList->getItemIndex(item), col), &sticky);
            LLToolTip::Params tp;
            tp.message = cell->getToolTip();
            tp.sticky_rect = sticky;
            tp.delay_time = 0.2f;
            if (thumb && thumb->hasPreview())
            {
                tp.create_params = LLSD(thumb->getTextureID());
                tp.create_callback = LLToolTip::Params::create_callback_t([](LLToolTip::Params p) -> LLToolTip*
                {
                    return LLUICtrlFactory::create<FaceThumbToolTip>(p);
                });
            }
            LLToolTipMgr::instance().show(tp);
            return true;
        }
    }
    return LLPanel::handleToolTip(x, y, mask);
}
