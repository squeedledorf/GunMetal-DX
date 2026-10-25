/**
 * @file slfloaterspeakersetup.cpp
 * @brief #892: "Set up as speaker..." floater and its one-shot script install.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
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
 *
 * The script goes in the way the Slipstream Helper's does (slbridge.cpp): one script item in the
 * agent's Scripts folder gets the generated source through UpdateScriptAgent (compiled as Mono),
 * then RezScript copies it, running, into the prim (LLViewerObject::saveScript). The agent item is
 * reused for every speaker, so nothing piles up in the inventory. RezScript has no answer; success
 * is the prim's hover text arriving as the expected tag (or empty, for a removal).
 */

#include "llviewerprecompiledheaders.h"
#include "slfloaterspeakersetup.h"

#include "llagent.h"
#include "llappviewer.h"
#include "llbutton.h"
#include "llcallbacklist.h"
#include "llcheckboxctrl.h"
#include "llcombobox.h"
#include "llfloaterreg.h"
#include "llinventoryfunctions.h"
#include "llinventorymodel.h"
#include "lllineeditor.h"
#include "llnotificationsutil.h"
#include "llparcel.h"
#include "llregionflags.h"
#include "llpermissions.h"
#include "llselectmgr.h"
#include "llsliderctrl.h"
#include "llspinctrl.h"
#include "lltextbox.h"
#include "llviewerassetupload.h"
#include "llviewerinventory.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerparcelmgr.h"
#include "llviewerregion.h"
#include "llvoavatarself.h"
#include "rlvhandler.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>

namespace
{
constexpr size_t kMaxGroupLen = 24;         // slparcelspeakers.cpp
constexpr F32 kAnswerTimeout = 20.f;        // seconds to wait for the hover text to change
constexpr F32 kUploadTimeout = 45.f;        // seconds for the item create + upload to answer
const char* const kScriptName = "Slipstream speaker setup";

// The hover text: faintly visible, as slipstream_speaker_setup.lsl sets it (the sim drops
// near-invisible text; Slipstream does not draw the tag).
std::string setupSource(const std::string& tag)
{
    // The tag holds only [a-z0-9_.-=] and spaces: nothing in it needs escaping in an LSL string.
    return "// Slipstream speaker setup (#892, doc/parcel_speakers_protocol.md): sets the speaker tag\n"
           "// as hover text and removes itself. Put in by the viewer's \"Set up as speaker\".\n"
           "default\n"
           "{\n"
           "    state_entry()\n"
           "    {\n"
           "        llSetText(\"" + tag + "\", <1.0, 1.0, 1.0>, 0.1);\n"
           "        llRemoveInventory(llGetScriptName());\n"
           "    }\n"
           "}\n";
}

std::string removeSource()
{
    return "// Slipstream speaker setup (#892): clears the speaker tag (hover text) and removes itself.\n"
           "default\n"
           "{\n"
           "    state_entry()\n"
           "    {\n"
           "        llSetText(\"\", ZERO_VECTOR, 0.0);\n"
           "        llRemoveInventory(llGetScriptName());\n"
           "    }\n"
           "}\n";
}

std::string sanitizeGroup(const std::string& in)
{
    std::string out;
    for (unsigned char ch : in)
    {
        const char c = (char)std::tolower(ch);
        if (std::isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.')
            out += c;
        if (out.size() >= kMaxGroupLen)
            break;
    }
    return out;
}

// Short numbers: 0.8, 30, 12.5 (never "%f"); '.' whatever the locale, as the parser reads them.
std::string shortNumber(F32 v, S32 decimals)
{
    char buf[32];
    const auto r = std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::fixed, decimals);
    std::string s(buf, r.ec == std::errc() ? r.ptr : buf);
    if (s.find('.') != std::string::npos)
    {
        while (!s.empty() && s.back() == '0')
            s.pop_back();
        if (!s.empty() && s.back() == '.')
            s.pop_back();
    }
    return s;
}

bool isWorn(LLViewerObject* obj)
{
    if (!obj || obj->isAvatar() || obj->isAttachment() || obj->isHUDAttachment())
        return true;
    LLViewerObject* root = obj->getRootEdit();
    return root && (root->isAvatar() || root->isAttachment() || root->isHUDAttachment());
}

class SLScriptItemCollector : public LLInventoryCollectFunctor
{
public:
    bool operator()(LLInventoryCategory*, LLInventoryItem* item) override
    {
        if (!item || item->getType() != LLAssetType::AT_LSL_TEXT || item->getName() != kScriptName)
            return false;
        const LLPermissions& perm = item->getPermissions();
        return perm.getCreator() == gAgentID && perm.getOwner() == gAgentID && perm.allowModifyBy(gAgentID)
            && perm.allowCopyBy(gAgentID);
    }
};

// ---------------------------------------------------------------------------
// The install: free functions keyed by object id, so it outlives the floater.
// ---------------------------------------------------------------------------

struct Job
{
    LLUUID objectID;
    std::string source;
    std::string expected;       // hover text we wait for (empty: removal)
    bool removal = false;
    bool rezzed = false;        // RezScript sent: the hover-text watcher owns the job now
    std::string prevText;
    F32 prevAlpha = 1.f;
};

bool sBusy = false;
U32 sJobSerial = 0;     // a late callback of an abandoned job must not act on a newer one
Job sJob;

bool shuttingDown()
{
    return gDisconnected || LLAppViewer::instance()->quitRequested();
}

void fail(const std::string& reason)
{
    sBusy = false;
    LL_WARNS("ParcelAudio") << "speaker setup failed: " << reason << LL_ENDL;
    LLSD args;
    args["REASON"] = reason;
    LLNotificationsUtil::add("SpeakerSetupFailed", args);
}

void watchForAnswer()
{
    LLFrameTimer* timer = new LLFrameTimer();
    timer->reset();
    const U32 serial = sJobSerial;
    doPeriodically([timer, serial]()
        {
            bool done = false;
            if (shuttingDown() || serial != sJobSerial)
            {
                done = true;
            }
            else
            {
                LLViewerObject* obj = gObjectList.findObject(sJob.objectID);
                // Only a change counts: the same text could already be there (at another alpha).
                const bool arrived = obj && !obj->isDead()
                    && (sJob.removal ? obj->mHudText.empty()
                                     : obj->mHudText == sJob.expected
                                           && (sJob.prevText != sJob.expected
                                               || std::fabs(obj->mHudTextColor.mV[VALPHA] - sJob.prevAlpha) > 0.02f));
                if (arrived)
                {
                    LL_INFOS("ParcelAudio") << "speaker setup: " << sJob.objectID
                                            << (sJob.removal ? " tag removed" : " tag set: " + sJob.expected) << LL_ENDL;
                    LLSD args;
                    args["TAG"] = sJob.expected;
                    LLNotificationsUtil::add(sJob.removal ? "SpeakerTagRemoved" : "SpeakerSetupDone", args);
                    done = true;
                }
                else if (timer->getElapsedTimeF32() > kAnswerTimeout)
                {
                    LL_WARNS("ParcelAudio") << "speaker setup: no hover-text change on " << sJob.objectID << LL_ENDL;
                    LLSD args;
                    args["SCRIPT"] = kScriptName;
                    LLNotificationsUtil::add("SpeakerSetupNoAnswer", args);
                    done = true;
                }
            }
            if (done)
            {
                if (serial == sJobSerial)
                    sBusy = false;
                delete timer;
            }
            return done;
        }, 0.5f);
}

void onScriptUploaded(U32 serial, const LLUUID& item_id, const LLSD& response)
{
    if (serial != sJobSerial || !sBusy)
        return;     // given up on (timeout) or superseded
    if (shuttingDown())
    {
        sBusy = false;
        return;
    }
    if (response.has("compiled") && !response["compiled"].asBoolean())
    {
        LL_WARNS("ParcelAudio") << "speaker setup script did not compile: " << response["errors"] << LL_ENDL;
        fail("the setup script did not compile (see the log)");
        return;
    }
    LLViewerObject* obj = gObjectList.findObject(sJob.objectID);
    if (!obj || obj->isDead() || !obj->permModify())
    {
        fail("the object is gone or can no longer be modified");
        return;
    }
    LLViewerInventoryItem* item = gInventory.getItem(item_id);
    if (!item)
    {
        fail("the uploaded script is not in your inventory");
        return;
    }
    sJob.prevText = obj->mHudText;
    sJob.prevAlpha = obj->mHudTextColor.mV[VALPHA];
    // #461: a create in the object's inventory; no answer is tracked (logged WAITING).
    LLInvCreateLog::Site inv_site("Speaker setup script install");
    LLInvCreateLog::noteSent("object script", LLUUID::null, (S32)LLAssetType::AT_LSL_TEXT, item->getName(),
                             "into object " + obj->getID().asString());
    sJob.rezzed = true;
    obj->saveScript(item, true, true);
    watchForAnswer();
}

void uploadScript(U32 serial, const LLUUID& item_id)
{
    if (serial != sJobSerial || !sBusy)
        return;
    if (shuttingDown())
    {
        sBusy = false;
        return;
    }
    if (item_id.isNull())
    {
        fail("could not create the script in your inventory");
        return;
    }
    const std::string url = gAgent.getRegionCapability("UpdateScriptAgent");
    if (url.empty())
    {
        fail("this region does not take script uploads (no UpdateScriptAgent capability)");
        return;
    }
    LLBufferedAssetUploadInfo::invnUploadFinish_f finish = [serial](LLUUID itemId, LLUUID newAssetId, LLUUID newItemId, LLSD response)
        {
            onScriptUploaded(serial, newItemId.notNull() ? newItemId : itemId, response);
        };
    LLBufferedAssetUploadInfo::uploadFailed_f failed = [serial](LLUUID itemId, LLUUID taskId, LLSD response, std::string reason)
        {
            if (serial != sJobSerial || !sBusy)
                return false;
            if (!shuttingDown())
                fail("the script upload failed: " + reason);
            else
                sBusy = false;
            return false;
        };
    LLResourceUploadInfo::ptr_t info(new LLScriptAssetUpload(item_id, "mono", sJob.source, finish, failed));
    LLViewerAssetUpload::EnqueueInventoryUpload(url, info);
}

void startInstall(LLViewerObject* obj, const std::string& source, const std::string& expected, bool removal)
{
    if (sBusy)
    {
        LLNotificationsUtil::add("SpeakerSetupBusy");
        return;
    }
    if (!obj || obj->isDead() || obj->isLocalOnly() || !obj->getRegion())
    {
        fail("the object is gone");
        return;
    }
    if (!obj->permModify() || isWorn(obj))
    {
        fail("you cannot modify this object");
        return;
    }
    LLViewerRegion* region = obj->getRegion();
    if (region->getRegionFlag(REGION_FLAGS_SKIP_SCRIPTS) || region->getRegionFlag(REGION_FLAGS_ESTATE_SKIP_SCRIPTS))
    {
        fail("scripts are disabled in this region, so the setup script cannot run");
        return;
    }
// [RLVa:KB] - as LLPanelContents::onClickNewScript: no new scripts in a linkset we sit on under @unsit / @sittp
    if (rlv_handler_t::isEnabled() && (gRlvHandler.hasBehaviour(RLV_BHVR_UNSIT) || gRlvHandler.hasBehaviour(RLV_BHVR_SITTP))
        && isAgentAvatarValid() && gAgentAvatarp->isSitting() && gAgentAvatarp->getRoot() == obj->getRootEdit())
    {
        fail("RLV restrictions block adding scripts to the object you sit on");
        return;
    }
// [/RLVa:KB]
    if (gAgent.getRegionCapability("UpdateScriptAgent").empty())
    {
        fail("this region does not take script uploads (no UpdateScriptAgent capability)");
        return;
    }

    sBusy = true;
    const U32 serial = ++sJobSerial;
    sJob = Job();
    sJob.objectID = obj->getID();
    sJob.source = source;
    sJob.expected = expected;
    sJob.removal = removal;

    // The inventory create can go unanswered (a refused create has no callback): never stay busy.
    doAfterInterval([serial]()
        {
            if (serial == sJobSerial && sBusy && !sJob.rezzed && !shuttingDown())
            {
                ++sJobSerial;   // late answers of this job are ignored
                fail("no answer from the inventory or the script upload");
            }
        }, kUploadTimeout);

    // Reuse our script item in the Scripts folder when it is there.
    const LLUUID folder_id = gInventory.findCategoryUUIDForType(LLFolderType::FT_LSL_TEXT);
    if (folder_id.notNull())
    {
        LLInventoryModel::cat_array_t cats;
        LLInventoryModel::item_array_t items;
        SLScriptItemCollector collector;
        gInventory.collectDescendentsIf(folder_id, cats, items, LLInventoryModel::EXCLUDE_TRASH, collector);
        if (!items.empty() && items.front())
        {
            uploadScript(serial, items.front()->getUUID());
            return;
        }
    }

    LLPermissions perm;
    perm.init(gAgentID, gAgentID, LLUUID::null, LLUUID::null);
    perm.initMasks(PERM_ALL, PERM_ALL, PERM_ALL, PERM_ALL, PERM_ALL);
    LLPointer<LLInventoryCallback> cb = new LLBoostFuncInventoryCallback([serial](const LLUUID& id) { uploadScript(serial, id); });
    LLInvCreateLog::Site inv_site("Speaker setup script"); // #461
    create_inventory_item(gAgentID, gAgentSessionID, folder_id.notNull() ? folder_id : gInventory.getRootFolderID(),
                          LLTransactionID::tnull, kScriptName, LLStringUtil::null, LLAssetType::AT_LSL_TEXT,
                          LLInventoryType::IT_LSL, NO_INV_SUBTYPE, perm.getMaskNextOwner(), cb);
}
} // namespace

// ---------------------------------------------------------------------------
// Selection / menu
// ---------------------------------------------------------------------------

// static
LLViewerObject* SLFloaterSpeakerSetup::getSelectedTarget()
{
    LLObjectSelectionHandle selection = LLSelectMgr::getInstance()->getSelection();
    if (selection.isNull() || (selection->getObjectCount() != 1 && selection->getRootObjectCount() != 1))
        return nullptr;
    // The clicked prim: selectObjectAndFamily makes it the primary object (not the root).
    LLViewerObject* obj = selection->getPrimaryObject();
    if (!obj)
        obj = selection->getFirstObject();
    if (!obj || obj->isDead() || obj->isLocalOnly() || isWorn(obj) || !obj->permModify())
        return nullptr;
    // A primary object left over from another linkset does not count.
    if (!selection->findNode(obj))
        return nullptr;
    return obj;
}

// static
bool SLFloaterSpeakerSetup::canSetUp()
{
    return getSelectedTarget() != nullptr;
}

// static
void SLFloaterSpeakerSetup::showForSelection()
{
    LLViewerObject* obj = getSelectedTarget();
    if (!obj)
        return;
    SLFloaterSpeakerSetup* floater = LLFloaterReg::showTypedInstance<SLFloaterSpeakerSetup>("sl_speaker_setup", LLSD(), TAKE_FOCUS_YES);
    if (floater)
        floater->setObject(obj);
}

// ---------------------------------------------------------------------------
// Floater
// ---------------------------------------------------------------------------

SLFloaterSpeakerSetup::SLFloaterSpeakerSetup(const LLSD& key)
    : LLFloater(key)
{
}

bool SLFloaterSpeakerSetup::postBuild()
{
    mGroupEdit = getChild<LLLineEditor>("group");
    mChannelCombo = getChild<LLComboBox>("channel");
    mVolumeSlider = getChild<LLSliderCtrl>("volume");
    mRangeSlider = getChild<LLSliderCtrl>("range");
    mDirCheck = getChild<LLCheckBoxCtrl>("use_dir");
    mDirSpin = getChild<LLSpinCtrl>("dir");
    mOffCheck = getChild<LLCheckBoxCtrl>("off");
    mObjectText = getChild<LLTextBox>("object_name");
    mPreviewText = getChild<LLTextBox>("preview");
    mTrustText = getChild<LLTextBox>("trust");
    mStatusText = getChild<LLTextBox>("status");
    mApplyBtn = getChild<LLButton>("apply");
    mRemoveBtn = getChild<LLButton>("remove");

    mGroupEdit->setMaxTextLength((S32)kMaxGroupLen);
    mGroupEdit->setKeystrokeCallback([this](LLLineEditor*, void*) { onGroupKeystroke(); }, nullptr);
    mGroupEdit->setCommitCallback([this](LLUICtrl*, const LLSD&) { onGroupKeystroke(); });
    mChannelCombo->setCommitCallback([this](LLUICtrl*, const LLSD&) { onChanged(); });
    mVolumeSlider->setCommitCallback([this](LLUICtrl*, const LLSD&) { onChanged(); });
    mRangeSlider->setCommitCallback([this](LLUICtrl*, const LLSD&) { onChanged(); });
    mDirCheck->setCommitCallback([this](LLUICtrl*, const LLSD&) { onChanged(); });
    mDirSpin->setCommitCallback([this](LLUICtrl*, const LLSD&) { onChanged(); });
    mOffCheck->setCommitCallback([this](LLUICtrl*, const LLSD&) { onChanged(); });
    mApplyBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onApply(); });
    mRemoveBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onRemove(); });

    tagToControls(SLParcelSpeakers::Tag());
    refreshState();
    return true;
}

void SLFloaterSpeakerSetup::setObject(LLViewerObject* obj)
{
    mObjectID = obj ? obj->getID() : LLUUID::null;
    SLParcelSpeakers::Tag tag;
    const bool tagged = obj && SLParcelSpeakers::parseTag(obj->mHudText, tag);
    tagToControls(tagged ? tag : SLParcelSpeakers::Tag());
    refreshState();
}

std::string SLFloaterSpeakerSetup::statusFor(LLViewerObject* obj, const std::string& tag_str) const
{
    if (sBusy)
        return getString("status_working");
    if (!obj)
        return std::string();
    SLParcelSpeakers::Tag tag;
    std::string errors;
    if (!SLParcelSpeakers::parseTag(obj->mHudText, tag, &errors))
        return getString(obj->mHudText.empty() ? "status_untagged" : "status_has_text");
    if (obj->mHudText == tag_str && obj->mHudTextColor.mV[VALPHA] > 0.05f)
        return getString("status_same");
    return errors.empty() ? getString("status_tagged") : getString("status_tagged_errors") + " " + errors;
}

void SLFloaterSpeakerSetup::tagToControls(const SLParcelSpeakers::Tag& tag)
{
    mGroupEdit->setText(tag.group);
    const char* ch = "M";
    switch (tag.role)
    {
        case SLParcelSpeakers::ROLE_LEFT: ch = "L"; break;
        case SLParcelSpeakers::ROLE_RIGHT: ch = "R"; break;
        case SLParcelSpeakers::ROLE_SUB: ch = "S"; break;
        default: break;
    }
    mChannelCombo->setValue(LLSD(ch));
    mVolumeSlider->setValue(LLSD(tag.vol));
    mRangeSlider->setValue(LLSD(tag.range));
    mDirCheck->set(tag.hasDir);
    mDirSpin->set(tag.hasDir ? tag.dirDeg : 90.f);
    mOffCheck->set(tag.off);
    mHasT0 = tag.hasT0;
    mKeepT0 = tag.t0;
}

SLParcelSpeakers::Tag SLFloaterSpeakerSetup::tagFromControls() const
{
    SLParcelSpeakers::Tag tag;
    const std::string grp = sanitizeGroup(mGroupEdit->getText());
    tag.group = grp.empty() ? std::string("main") : grp;
    const std::string ch = mChannelCombo->getValue().asString();
    tag.role = ch == "L" ? SLParcelSpeakers::ROLE_LEFT
             : ch == "R" ? SLParcelSpeakers::ROLE_RIGHT
             : ch == "S" ? SLParcelSpeakers::ROLE_SUB
                         : SLParcelSpeakers::ROLE_MONO;
    // Quantized to what the tag string carries, so the preview parses back to the same values.
    tag.vol = std::round(llclamp(mVolumeSlider->getValueF32(), 0.f, SLParcelSpeakers::kMaxVol) * 100.f) / 100.f;
    tag.range = std::round(llclamp(mRangeSlider->getValueF32(), 5.f, 100.f) * 10.f) / 10.f;
    tag.dirDeg = std::round(llclamp(mDirSpin->get(), 1.f, 360.f));
    tag.hasDir = mDirCheck->get() && tag.dirDeg < 360.f;
    if (!tag.hasDir)
        tag.dirDeg = 360.f;
    tag.off = mOffCheck->get();
    tag.hasT0 = mHasT0;
    tag.t0 = mKeepT0;
    return tag;
}

// static
std::string SLFloaterSpeakerSetup::makeTagString(const SLParcelSpeakers::Tag& tag)
{
    static const char* const roles[] = { "M", "L", "R", "S" };
    std::string s = "[slspk grp=" + tag.group;
    s += std::string(" ch=") + roles[llclamp((S32)tag.role, 0, 3)];
    s += " vol=" + shortNumber(tag.vol, 2);
    s += " r=" + shortNumber(tag.range, 1);
    if (tag.hasDir)
        s += " dir=" + shortNumber(tag.dirDeg, 0);
    if (tag.hasT0)
        s += llformat(" t0=%lld", (long long)tag.t0);
    if (tag.off)
        s += " off";
    s += "]";
    return s;
}

void SLFloaterSpeakerSetup::onGroupKeystroke()
{
    const std::string text = mGroupEdit->getText();
    const std::string clean = sanitizeGroup(text);
    if (clean != text)
        mGroupEdit->setText(clean);
    refreshState();
}

void SLFloaterSpeakerSetup::onChanged()
{
    refreshState();
}

std::string SLFloaterSpeakerSetup::trustHint(LLViewerObject* obj) const
{
    LLViewerParcelMgr* pm = LLViewerParcelMgr::getInstance();
    LLParcel* parcel = pm->getAgentParcel();
    if (!obj || !parcel)
        return getString("trust_unknown");
    if (!pm->inAgentParcel(obj->getPositionGlobal()))
        return getString("trust_other_parcel");
    LLSelectNode* node = LLSelectMgr::getInstance()->getSelection()->findNode(obj);
    if (!node || !node->mValid || !node->mPermissions)
        return getString("trust_unknown");
    const LLUUID owner = node->mPermissions->getOwner();
    const LLUUID group = node->mPermissions->getGroup();
    const bool qualifies = (owner.notNull() && owner == parcel->getOwnerID())
        || (group.notNull() && group == parcel->getGroupID());
    std::string hint = getString(qualifies ? "trust_yes" : "trust_no");
    // The script has to run on this parcel too (no-script land).
    const bool scripts_run = parcel->getAllowOtherScripts() || owner == parcel->getOwnerID()
        || (parcel->getAllowGroupScripts() && group.notNull() && group == parcel->getGroupID());
    if (!scripts_run)
        hint += " " + getString("trust_no_scripts");
    return hint;
}

void SLFloaterSpeakerSetup::refreshState()
{
    LLViewerObject* obj = mObjectID.notNull() ? gObjectList.findObject(mObjectID) : nullptr;
    const bool usable = obj && !obj->isDead() && obj->permModify() && !isWorn(obj);

    const SLParcelSpeakers::Tag tag = tagFromControls();
    const std::string tag_str = makeTagString(tag);
    // Self-check: the preview must read back as exactly what the controls say.
    SLParcelSpeakers::Tag back;
    std::string errors;
    const bool round_trip = SLParcelSpeakers::parseTag(tag_str, back, &errors) && back == tag && errors.empty();
    mPreviewText->setText(round_trip ? tag_str : tag_str + "  (" + getString("preview_mismatch") + ")");

    mDirSpin->setEnabled(mDirCheck->get());

    if (!obj || obj->isDead())
        mObjectText->setText(getString("no_object"));
    else
    {
        LLSelectNode* node = LLSelectMgr::getInstance()->getSelection()->findNode(obj);
        mObjectText->setText(node && node->mValid && !node->mName.empty() ? node->mName : obj->getID().asString());
    }

    SLParcelSpeakers::Tag current;
    const bool tagged = usable && SLParcelSpeakers::parseTag(obj->mHudText, current);
    const bool same = usable && obj->mHudText == tag_str && obj->mHudTextColor.mV[VALPHA] > 0.05f;
    mApplyBtn->setEnabled(usable && round_trip && !same && !sBusy);
    mStatusText->setText(usable ? statusFor(obj, tag_str) : std::string());
    mRemoveBtn->setEnabled(tagged && !sBusy);
    mTrustText->setText(usable ? trustHint(obj) : getString("trust_unknown"));
}

void SLFloaterSpeakerSetup::draw()
{
    // Follow the selection: with the floater left open, clicking another prim retargets it
    // (otherwise every Apply kept going to the prim the floater was opened on). Not while an
    // install is running, and not when nothing usable is selected (keep the last target).
    if (!sBusy)
    {
        LLViewerObject* sel = getSelectedTarget();
        if (sel && sel->getID() != mObjectID)
            setObject(sel);
    }
    if (mRefreshTimer.getElapsedTimeF32() > 0.5f)
    {
        mRefreshTimer.reset();
        refreshState();
    }
    LLFloater::draw();
}

void SLFloaterSpeakerSetup::onApply()
{
    LLViewerObject* obj = gObjectList.findObject(mObjectID);
    const std::string tag_str = makeTagString(tagFromControls());
    startInstall(obj, setupSource(tag_str), tag_str, false);
    refreshState();
}

void SLFloaterSpeakerSetup::onRemove()
{
    LLViewerObject* obj = gObjectList.findObject(mObjectID);
    startInstall(obj, removeSource(), std::string(), true);
    refreshState();
}
