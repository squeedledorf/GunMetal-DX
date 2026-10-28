/**
 * @file gmdximwell.cpp
 * @brief GunMetal-DX: the unread IM indicator in the chiclet bar, left of the notifications well.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
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

// <GMDX> whole file. The Slipstream (LL) chiclet bar has only the notifications well: since the
// Conversations rework the only unread-IM sign is the Conversations toolbar button flashing.
// This is the IM well Firestorm keeps (fs_chiclet_im_well), redone on LLSysWellChiclet: a chat icon
// with a count badge, shown while Conversations holds unread messages, flashing once per new one.
// A click opens Conversations at the newest unread conversation; reading it there clears it.
// Counted: messages from residents (not system notices or your own lines), in conversations of a
// type that flashes in Conversations (Preferences > Chat > Notifications, Notification*FlashConversation);
// a group on the block list does not count. Do Not Disturb: counted as the notifications well counts,
// but no flash.
// The icon is the newest unread conversation's face: the sender's profile picture (round), the group's
// insignia, a people glyph for a conference; the chat glyph while a picture loads, when there is none,
// and for a sender whose name RLVa @shownames hides. The tooltip lists the unread conversations.

#include "llviewerprecompiledheaders.h"

#include "llchiclet.h"

#include "llagent.h"
#include "llavatariconctrl.h"
#include "llavatarpropertiesprocessor.h"
#include "llcallbacklist.h"
#include "llcallingcard.h"
#include "llfloaterimcontainer.h"
#include "llfloaterreg.h"
#include "llfontgl.h"
#include "llimview.h"
#include "llmutelist.h"
#include "llrender2dutils.h"
#include "lluicolortable.h"
#include "llviewercontrol.h"
#include "llviewertexture.h"
#include "rlvactions.h"
#include "rlvcommon.h"

#include <algorithm>

namespace
{
    // The unread messages of a conversation that count (see the top of the file).
    S32 unread_in(const LLUUID& session_id, const LLIMModel::LLIMSession* s)
    {
        if (session_id.isNull() || !s || s->mParticipantUnreadMessageCount <= 0) return 0;
        const char* type = s->isP2PSessionType()
            ? (LLAvatarTracker::instance().isBuddy(s->mOtherParticipantID) ? "FriendIM" : "NonFriendIM")
            : s->isAdHocSessionType() ? "ConferenceIM" : s->isGroupSessionType() ? "GroupChat" : nullptr;
        if (!type || !gSavedSettings.getBOOL(llformat("Notification%sFlashConversation", type))) return 0;
        if (s->isGroupSessionType() && LLMuteList::getInstance()->isMuted(session_id)) return 0;
        return s->mParticipantUnreadMessageCount;
    }

    S32 unread_in(const LLUUID& session_id)
    {
        return unread_in(session_id, LLIMModel::getInstance()->findIMSession(session_id));
    }

    // A P2P conversation whose other side RLVa @shownames hides: no picture, an anonym for a name.
    bool name_hidden(const LLIMModel::LLIMSession* s)
    {
        return s->isP2PSessionType() && !RlvActions::canShowName(RlvActions::SNC_DEFAULT, s->mOtherParticipantID);
    }

    // img drawn as a disc of radius r around (cx, cy)
    void draw_round_image(LLUIImage* img, F32 cx, F32 cy, F32 r, const LLColor4& color)
    {
        constexpr S32 kSteps = 32;
        gGL.getTexUnit(0)->bind(img->getImage());
        gGL.color4fv(color.mV);
        gGL.begin(LLRender::TRIANGLES);
        for (S32 i = 0; i < kSteps; ++i)
        {
            const F32 a0 = F_TWO_PI * i / kSteps, a1 = F_TWO_PI * (i + 1) / kSteps;
            gGL.texCoord2f(0.5f, 0.5f);
            gGL.vertex2f(cx, cy);
            gGL.texCoord2f(0.5f + 0.5f * cosf(a0), 0.5f + 0.5f * sinf(a0));
            gGL.vertex2f(cx + r * cosf(a0), cy + r * sinf(a0));
            gGL.texCoord2f(0.5f + 0.5f * cosf(a1), 0.5f + 0.5f * sinf(a1));
            gGL.vertex2f(cx + r * cosf(a1), cy + r * sinf(a1));
        }
        gGL.end();
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    }
}

class GMDXIMWellChiclet : public LLSysWellChiclet, public LLIMSessionObserver, public LLMuteListObserver,
                          public LLAvatarPropertiesObserver
{
public:
    struct Params : public LLInitParam::Block<Params, LLSysWellChiclet::Params> {};

    void draw() override;

    void sessionAdded(const LLUUID&, const std::string&, const LLUUID&, bool) override {}
    void sessionActivated(const LLUUID&, const std::string&, const LLUUID&) override {}
    void sessionVoiceOrIMStarted(const LLUUID&) override {}
    void sessionRemoved(const LLUUID&) override { update(LLUUID::null); }
    void sessionIDUpdated(const LLUUID& old_id, const LLUUID& new_id) override
    {
        std::replace(mOrder.begin(), mOrder.end(), old_id, new_id);
        update(LLUUID::null);
    }
    void onChange() override { update(LLUUID::null); }   // a group blocked or unblocked
    void processProperties(void* data, EAvatarProcessorType type) override;   // a sender's profile picture id

protected:
    GMDXIMWellChiclet(const Params& p);
    ~GMDXIMWellChiclet();
    friend class LLUICtrlFactory;

    void createMenu() override {}   // (no context menu)

private:
    void update(const LLUUID& new_msg_session);   // null: no new message, a count went down
    void onClick();
    LLUUID newestUnread() const;
    LLUIImagePtr picture(const LLUUID& session_id);   // null: the glyph
    void refreshToolTip();

    boost::signals2::scoped_connection mNewMsg, mNoUnread;
    std::vector<boost::signals2::scoped_connection> mSettings;   // Notification*FlashConversation
    std::vector<LLUUID> mOrder;   // conversations by their last message, newest last
    LLUUID mRequested;            // the avatar whose profile picture id was asked for
    LLUUID mPictureId;            // mPicture's texture
    LLUIImagePtr mPicture;
    std::string mOverlay;         // the button's glyph now ("": a picture is drawn)
    std::string mToolTip;
};

static LLDefaultChildRegistry::Register<GMDXIMWellChiclet> r_gmdx_im_well("gmdx_chiclet_im_well");

GMDXIMWellChiclet::GMDXIMWellChiclet(const Params& p)
    : LLSysWellChiclet(p)
{
    mNewMsg = LLIMModel::instance().addNewMsgCallback([this](const LLSD& msg) { update(msg["session_id"].asUUID()); });
    mNoUnread = LLIMModel::instance().addNoUnreadMsgsCallback([this](const LLSD&) { update(LLUUID::null); });
    LLIMMgr::getInstance()->addSessionObserver(this);
    LLMuteList::getInstance()->addObserver(this);
    for (const char* type : { "FriendIM", "NonFriendIM", "ConferenceIM", "GroupChat" })
    {
        if (LLControlVariable* control = gSavedSettings.getControl(llformat("Notification%sFlashConversation", type)))
        {
            mSettings.emplace_back(control->getSignal()->connect(
                [this](LLControlVariable*, const LLSD&, const LLSD&) { update(LLUUID::null); }));
        }
    }
    setClickCallback([this](LLUICtrl*, const LLSD&) { onClick(); });
    // ponytail: a recount every second besides the signals. The count and the well's visibility then
    // follow the model whatever changed it (offline IMs at login included); cost: a walk over the open
    // conversations once a second.
    LLHandle<LLView> handle = getHandle();
    doPeriodically([handle]()
                   {
                       GMDXIMWellChiclet* well = static_cast<GMDXIMWellChiclet*>(handle.get());
                       if (!well) return true;   // (gone: stop)
                       well->update(LLUUID::null);
                       return false;
                   }, 1.f);
}

GMDXIMWellChiclet::~GMDXIMWellChiclet()
{
    if (LLIMMgr::instanceExists()) LLIMMgr::getInstance()->removeSessionObserver(this);
    if (LLMuteList::instanceExists()) LLMuteList::getInstance()->removeObserver(this);
    if (mRequested.notNull() && LLAvatarPropertiesProcessor::instanceExists())
        LLAvatarPropertiesProcessor::getInstance()->removeObserver(mRequested, this);
}

void GMDXIMWellChiclet::update(const LLUUID& new_msg_session)
{
    if (new_msg_session.notNull())
    {
        mOrder.erase(std::remove(mOrder.begin(), mOrder.end(), new_msg_session), mOrder.end());
        mOrder.push_back(new_msg_session);
    }
    mOrder.erase(std::remove_if(mOrder.begin(), mOrder.end(),
                                [](const LLUUID& id) { return !LLIMModel::getInstance()->findIMSession(id); }),
                 mOrder.end());

    S32 total = 0;
    for (const auto& session : LLIMModel::getInstance()->mId2SessionMap)
    {
        const S32 n = unread_in(session.first, session.second);
        total += n;
        // an unread conversation the signals never named (it got its lines before the well listened)
        if (n > 0 && std::find(mOrder.begin(), mOrder.end(), session.first) == mOrder.end())
            mOrder.insert(mOrder.begin(), session.first);
    }

    if (total > mCounter && new_msg_session.notNull() && !gAgent.isDoNotDisturb())
    {
        LLFloaterIMContainer* im_box = LLFloaterIMContainer::findInstance();
        if (!im_box || !im_box->isConversationVisible(new_msg_session))
        {
            // a fresh flash from unlit: an even number of toggles ends unlit again
            mFlashToLitTimer->stopFlashing();
            setNewMessagesState(false);
            mFlashToLitTimer->startFlashing();
        }
    }
    else if (total == 0)
    {
        mFlashToLitTimer->stopFlashing();
        setNewMessagesState(false);
    }
    mCounter = total;   // (drawn as a badge, not as the button's label)
    if (LLView* panel = getParent()) panel->setVisible(total > 0);   // im_well_panel, as the notifications well hides
}

LLUUID GMDXIMWellChiclet::newestUnread() const
{
    for (auto it = mOrder.rbegin(); it != mOrder.rend(); ++it)
        if (unread_in(*it) > 0) return *it;
    return LLUUID::null;
}

void GMDXIMWellChiclet::onClick()
{
    mFlashToLitTimer->stopFlashing();
    setNewMessagesState(false);
    const LLUUID newest = newestUnread();
    if (newest.notNull())
        LLFloaterIMContainer::getInstance()->showConversation(newest);
    else
        LLFloaterReg::showInstance("im_container");
}

void GMDXIMWellChiclet::processProperties(void* data, EAvatarProcessorType type)
{
    // as LLAvatarIconCtrl: both replies carry the picture id
    LLUUID avatar_id, image_id;
    if (APT_PROPERTIES_LEGACY == type && data)
    {
        avatar_id = static_cast<LLAvatarLegacyData*>(data)->avatar_id;
        image_id = static_cast<LLAvatarLegacyData*>(data)->image_id;
    }
    else if (APT_PROPERTIES == type && data)
    {
        avatar_id = static_cast<LLAvatarData*>(data)->avatar_id;
        image_id = static_cast<LLAvatarData*>(data)->image_id;
    }
    if (avatar_id.notNull() && avatar_id == mRequested) LLAvatarIconIDCache::getInstance()->add(avatar_id, image_id);
}

LLUIImagePtr GMDXIMWellChiclet::picture(const LLUUID& session_id)
{
    const LLIMModel::LLIMSession* s = LLIMModel::getInstance()->findIMSession(session_id);
    if (!s || name_hidden(s)) return nullptr;
    LLUUID image_id;
    if (s->isP2PSessionType())
    {
        const LLUUID& avatar_id = s->mOtherParticipantID;
        const LLUUID* cached = LLAvatarIconIDCache::getInstance()->get(avatar_id);
        if (!cached)
        {
            if (mRequested != avatar_id)   // (once per sender; the reply fills the cache)
            {
                LLAvatarPropertiesProcessor* app = LLAvatarPropertiesProcessor::getInstance();
                if (mRequested.notNull()) app->removeObserver(mRequested, this);
                mRequested = avatar_id;
                app->addObserver(mRequested, this);
                app->sendAvatarLegacyPropertiesRequest(mRequested);
            }
            return nullptr;
        }
        image_id = *cached;
    }
    else if (s->isGroupSessionType())
    {
        LLGroupData group;
        if (gAgent.getGroupData(session_id, group)) image_id = group.mInsigniaID;
    }
    if (image_id.isNull()) return nullptr;

    if (image_id != mPictureId)
    {
        mPictureId = image_id;
        mPicture = LLUI::getUIImageByID(image_id, LLGLTexture::BOOST_ICON);
        if (mPicture.notNull() && mPicture->getImage().notNull())
        {
            const LLVector2& scale = LLUI::getScaleFactor();
            mPicture->getImage()->setKnownDrawSize(ll_round(32 * scale.mV[VX]), ll_round(32 * scale.mV[VY]));
        }
    }
    // the glyph until the picture has pixels
    const LLViewerFetchedTexture* tex = mPicture.notNull() ? dynamic_cast<LLViewerFetchedTexture*>(mPicture->getImage().get()) : nullptr;
    if (!tex || tex->isMissingAsset() || tex->getDiscardLevel() < 0) return nullptr;
    return mPicture;
}

void GMDXIMWellChiclet::refreshToolTip()
{
    constexpr S32 kListed = 5;
    std::string tip;
    S32 listed = 0, more = 0;
    for (auto it = mOrder.rbegin(); it != mOrder.rend(); ++it)
    {
        const S32 n = unread_in(*it);
        if (n <= 0) continue;
        if (listed == kListed) { ++more; continue; }
        const LLIMModel::LLIMSession* s = LLIMModel::getInstance()->findIMSession(*it);
        const std::string name = LLIMModel::getInstance()->getName(*it);
        if (!tip.empty()) tip += "\n";
        tip += llformat("%s (%d)", (s && name_hidden(s) ? RlvStrings::getAnonym(name) : name).c_str(), n);
        ++listed;
    }
    if (more > 0) tip += llformat("\nand %d more", more);
    if (tip != mToolTip)
    {
        mToolTip = tip;
        mButton->setToolTip(tip);
    }
}

void GMDXIMWellChiclet::draw()
{
    // the face: the newest unread conversation's picture, else its glyph
    const LLUUID newest = newestUnread();
    LLUIImagePtr face = newest.notNull() ? picture(newest) : nullptr;
    const LLIMModel::LLIMSession* s = newest.notNull() ? LLIMModel::getInstance()->findIMSession(newest) : nullptr;
    const std::string overlay = face.notNull() ? "" : (s && s->isAdHocSessionType() ? "GMDX_IM_Conference" : "GMDX_IM_Unread");
    if (overlay != mOverlay)
    {
        mOverlay = overlay;
        mButton->setImageOverlay(overlay);
    }
    refreshToolTip();

    LLSysWellChiclet::draw();
    if (face.notNull())
    {
        // where the button draws its glyph: centred
        const LLRect& b = mButton->getRect();
        const F32 cx = (F32)b.getCenterX(), cy = (F32)b.getCenterY(), r = 8.f;
        draw_round_image(face.get(), cx, cy, r, LLColor4::white);
        static LLUIColor ring = LLUIColorTable::instance().getColor("IconColor");
        gGL.color4fv((ring.get() % 0.6f).mV);
        gl_circle_2d(cx, cy, r, 32, false);
    }
    if (mCounter <= 0) return;

    // the count on a pill in the top right corner
    const LLFontGL* font = LLFontGL::getFontSansSerifSmallBold();
    const std::string label = mCounter > mMaxDisplayedCount ? llformat("%d+", mMaxDisplayedCount) : llformat("%d", mCounter);
    const F32 h = (F32)llmin(font->getLineHeight() + 2, getRect().getHeight());
    const F32 r = h * 0.5f;
    const F32 w = llmax(h, (F32)font->getWidth(label) + 6.f);
    const F32 right = (F32)getRect().getWidth() - 1.f, top = (F32)getRect().getHeight();
    static LLUIColor fill = LLUIColorTable::instance().getColor("BadgeImageColor");
    static LLUIColor text = LLUIColorTable::instance().getColor("BadgeLabelColor");
    gGL.color4fv(fill.get().mV);
    gl_circle_2d(right - w + r, top - r, r, 24, true);
    gl_circle_2d(right - r, top - r, r, 24, true);
    gl_rect_2d((S32)(right - w + r), (S32)top, (S32)(right - r), (S32)(top - h), fill.get(), true);
    font->renderUTF8(label, 0, right - w * 0.5f, top - r, text.get(), LLFontGL::HCENTER, LLFontGL::VCENTER,
                     LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
}
