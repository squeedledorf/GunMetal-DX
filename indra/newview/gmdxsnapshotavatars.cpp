/**
 * @file gmdxsnapshotavatars.cpp
 * @brief Snapshot avatar layers (transparent avatar, holdout mattes) and the snapshot avatar scope.
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

// <GMDX> See gmdxsnapshotavatars.h. Ported from SoapStorm (Soap Frenzy).

#include "llviewerprecompiledheaders.h"

#include "gmdxsnapshotavatars.h"

#include "llagentcamera.h"
#include "llavatarnamecache.h"
#include "llbutton.h"
#include "llcachename.h"
#include "llcharacter.h"
#include "llcheckboxctrl.h"
#include "llcombobox.h"
#include "llfiltereditor.h"
#include "llfloaterreg.h"
#include "llfloatersnapshot.h"
#include "llimage.h"
#include "llpanelsnapshot.h"
#include "llscrolllistcell.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "llsnapshotlivepreview.h"
#include "llspinctrl.h"
#include "llviewercontrol.h"
#include "llviewerobjectlist.h"
#include "llviewerwindow.h"
#include "llvoavatar.h"
#include "pipeline.h"
#include "rlvactions.h"
#include "rlvcommon.h"
#include "vkgfx/llvulkancontext.h"

#include <algorithm>
#include <cmath>

std::set<LLUUID> gSnapshotSelectedAvatars;

// vkRenderOffscreenSnapshot and its sizing (llappviewer.cpp).
bool vkRenderOffscreenSnapshot(S32 w, S32 h, bool show_hud, std::vector<U8>& rgb,
                               std::vector<F32>* depth, F32 proj[2], U32 aa_frames);
U32 vkSnapshotAAFrames();
U32 vkSnapshotFit(S32& w, S32& h, U32 aa_frames);

namespace
{
// What the snapshot being rendered applies (main thread; between offscreen renders it is off).
struct SnapScope
{
    bool active = false;        // a snapshot's offscreen render is running
    U32  scope = SNAPSHOT_AVATARS_ALL;
    bool avatar_only = false;   // ... and it is an avatar layer
    bool gate = false;          // other avatars only when the warm-up showed them (seen)
    std::set<LLUUID> seen;
};
SnapScope s_scope;

U32 scopeSetting()
{
    static LLCachedControl<U32> s_setting(gSavedSettings, "FSSnapshotAvatarScope", SNAPSHOT_AVATARS_ALL);
    return llmin((U32)s_setting, (U32)SNAPSHOT_AVATARS_ALL);
}

// The avatar an avatar / animesh counts as: an animesh worn by someone is that wearer's.
LLVOAvatar* snapshotOwner(LLVOAvatar* av)
{
    if (av && av->isControlAvatar())
    {
        LLVOAvatar* wearer = av->getAttachedAvatar();
        if (wearer && !wearer->isDead()) return wearer;
    }
    return av;
}

// sRGB byte <-> linear (straight-alpha output of an averaged, premultiplied edge).
F32 srgbToLinear(U8 c)
{
    static F32 s_lut[256];
    static bool s_init = false;
    if (!s_init)
    {
        for (S32 i = 0; i < 256; ++i)
        {
            const F32 v = (F32)i / 255.f;
            s_lut[i] = v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f);
        }
        s_init = true;
    }
    return s_lut[c];
}

U8 linearToSrgb(F32 v)
{
    v = llclamp(v, 0.f, 1.f);
    const F32 s = v <= 0.0031308f ? v * 12.92f : 1.055f * powf(v, 1.f / 2.4f) - 0.055f;
    return (U8)llclamp(ll_round(s * 255.f), 0, 255);
}
} // namespace

bool gmdxIsAvatarLayer(LLSnapshotModel::ESnapshotLayerType type)
{
    return type == LLSnapshotModel::SNAPSHOT_TYPE_AVATAR_TRANSPARENT
        || type == LLSnapshotModel::SNAPSHOT_TYPE_AVATAR_MASK_BLACK
        || type == LLSnapshotModel::SNAPSHOT_TYPE_AVATAR_MASK_WHITE;
}

bool gmdxSnapshotHidesAvatar(LLVOAvatar* av)
{
    if (!s_scope.active || !av || av->isDead() || av->isUIAvatar()) return false;
    LLVOAvatar* owner = snapshotOwner(av);
    if (owner->isSelf()) return false;
    if (s_scope.scope == SNAPSHOT_AVATARS_SELF) return true;
    if (s_scope.scope == SNAPSHOT_AVATARS_SELECTED && !gSnapshotSelectedAvatars.count(owner->getID())) return true;
    // Avatar layer: nobody the view with the world did not show (no avatar through a wall).
    return s_scope.gate && !s_scope.seen.count(owner->getID());
}

bool gmdxSnapshotAvatarOnly()
{
    return s_scope.active && s_scope.avatar_only;
}

void gmdxSnapshotMatteSetSeen(const std::vector<LLUUID>& ids)
{
    if (!gmdxSnapshotAvatarOnly()) return;
    s_scope.seen.clear();
    for (const LLUUID& id : ids)
    {
        LLViewerObject* obj = gObjectList.findObject(id);
        LLVOAvatar* av = obj ? obj->asAvatar() : nullptr;
        if (av && !av->isDead()) s_scope.seen.insert(snapshotOwner(av)->getID());
    }
    s_scope.gate = true;
}

bool gmdxSnapshotWantsOffscreen(LLSnapshotModel::ESnapshotLayerType type)
{
    if (gmdxIsAvatarLayer(type)) return true;
    if (scopeSetting() != SNAPSHOT_AVATARS_ALL) return true;
    static LLCachedControl<bool> s_mattes(gSavedSettings, "FSSaveAvatarMasksWithSnapshot", false);
    return type == LLSnapshotModel::SNAPSHOT_TYPE_COLOR && s_mattes;
}

GMDXSnapshotScopeHold::GMDXSnapshotScopeHold(bool on) : mOn(on && !s_scope.active)
{
    if (!mOn) return;
    s_scope = SnapScope{};
    s_scope.active = true;
    s_scope.scope = scopeSetting();
}

GMDXSnapshotScopeHold::~GMDXSnapshotScopeHold()
{
    if (mOn) s_scope = SnapScope{};
}

bool gmdxRawAvatarSnapshot(LLImageRaw* raw, S32 image_width, S32 image_height,
                           LLSnapshotModel::ESnapshotLayerType type, S32 max_size)
{
    if (!raw || image_width <= 0 || image_height <= 0 || !gmdxIsAvatarLayer(type) || s_scope.active) return false;
    // The size the device / video memory / system memory allow (as the offscreen snapshot); the
    // rest is a scale at the end.
    S32 rw = image_width, rh = image_height;
    const U32 aa = vkSnapshotFit(rw, rh, vkSnapshotAAFrames());
    if (rw <= 0 || rh <= 0) return false;

    s_scope = SnapScope{};
    s_scope.active = true;
    s_scope.scope = scopeSetting();
    s_scope.avatar_only = true;
    // No sky (the background is cleared black) and no particles in front of the avatars.
    gPipeline.pushRenderTypeMask();
    gPipeline.clearRenderTypeMask(LLPipeline::RENDER_TYPE_SKY, LLPipeline::RENDER_TYPE_WL_SKY,
                                  LLPipeline::RENDER_TYPE_PARTICLES, LLPipeline::END_RENDER_TYPES);
    std::vector<U8> rgb;
    const bool got = vkRenderOffscreenSnapshot(rw, rh, false, rgb, nullptr, nullptr, aa);
    gPipeline.popRenderTypeMask();
    s_scope = SnapScope{};

    std::vector<U8> alpha;
    uint32_t aw = 0, ah = 0;
    const bool got_alpha = LLVulkanContext::instance().takeSnapshotMatte(alpha, aw, ah);   // (always: drops a stale one)
    if (!got || !got_alpha || (S32)aw != rw || (S32)ah != rh || rgb.size() < (size_t)rw * rh * 3
        || alpha.size() < (size_t)rw * rh)
    {
        LL_WARNS("Snapshot") << "avatar snapshot " << rw << "x" << rh << " failed (render " << got << ", matte "
                             << got_alpha << " " << aw << "x" << ah << ")" << LL_ENDL;
        return false;
    }
    LL_INFOS("Snapshot") << "avatar snapshot " << rw << "x" << rh << " (requested " << image_width << "x" << image_height
                         << ", layer " << (S32)type << ", antialias frames " << aa << ")" << LL_ENDL;

    const bool transparent = (type == LLSnapshotModel::SNAPSHOT_TYPE_AVATAR_TRANSPARENT);
    {
        LLImageDataLock lock(raw);
        raw->resize(rw, rh, transparent ? 4 : 3);
        if (raw->isBufferInvalid()) return false;
        U8* out = raw->getData();
        const size_t n = (size_t)rw * rh;
        for (size_t p = 0; p < n; ++p)
        {
            const U8 a = alpha[p];
            const U8* c = &rgb[p * 3];
            if (transparent)
            {
                U8* o = out + p * 4;
                if (a == 0)
                {
                    o[0] = o[1] = o[2] = o[3] = 0;   // nothing behind the avatars (SoapStorm: no stray colour)
                    continue;
                }
                if (a == 255)
                {
                    o[0] = c[0]; o[1] = c[1]; o[2] = c[2];
                }
                else
                {
                    // An antialiased edge averaged the avatar with the black background: straight
                    // alpha for PNG (in linear light; the tonemap makes it approximate).
                    const F32 inv = 255.f / (F32)a;
                    for (S32 k = 0; k < 3; ++k) o[k] = linearToSrgb(srgbToLinear(c[k]) * inv);
                }
                o[3] = a;
            }
            else
            {
                const U8 v = (type == LLSnapshotModel::SNAPSHOT_TYPE_AVATAR_MASK_BLACK) ? (U8)(255 - a) : a;
                U8* o = out + p * 3;
                o[0] = o[1] = o[2] = v;
            }
        }
    }
    // Pad the line length to a multiple of 4 bytes (BMP, 3 components only) and honour max_size,
    // as rawSnapshot does.
    if (!transparent) image_width += (image_width * 3) % 4;
    if (image_width > max_size || image_height > max_size)
    {
        const F32 f = llmin((F32)max_size / image_width, (F32)max_size / image_height);
        image_width = llmax(1, (S32)(image_width * f));
        image_height = llmax(1, (S32)(image_height * f));
    }
    if (image_width != rw || image_height != rh) return raw->scale(image_width, image_height, true);
    return true;
}

void gmdxSaveSnapshotMattes(LLImageFormatted* image, const std::string& filepath, const std::string& extension)
{
    static LLCachedControl<bool> s_mattes(gSavedSettings, "FSSaveAvatarMasksWithSnapshot", false);
    if (!s_mattes || !image || !gViewerWindow || filepath.size() <= extension.size()) return;
    // A saved avatar layer is its own matte.
    if (LLFloaterSnapshot* floater = LLFloaterSnapshot::findInstance())
    {
        LLUICtrl* layers = floater->findChild<LLUICtrl>("layer_types");
        if (layers && layers->getValue().asString() != "colors") return;
    }
    const std::string base = filepath.substr(0, filepath.length() - extension.length());
    LLPointer<LLImageRaw> avatar_raw = new LLImageRaw;
    if (!gViewerWindow->rawSnapshot(avatar_raw, image->getWidth(), image->getHeight(), true, false, false, false, false,
                                    false, false, LLSnapshotModel::SNAPSHOT_TYPE_AVATAR_TRANSPARENT)
        || avatar_raw->getComponents() != 4)
    {
        LL_WARNS("Snapshot") << "holdout mattes for " << filepath << " not saved: the avatar render failed" << LL_ENDL;
        return;
    }
    const S32 w = avatar_raw->getWidth(), h = avatar_raw->getHeight();
    LLPointer<LLImageRaw> matte_black = new LLImageRaw(w, h, 3);   // avatar black on white
    LLPointer<LLImageRaw> matte_white = new LLImageRaw(w, h, 3);   // avatar white on black
    if (matte_black->isBufferInvalid() || matte_white->isBufferInvalid()) return;
    const U8* src = avatar_raw->getData();
    U8* dst_b = matte_black->getData();
    U8* dst_w = matte_white->getData();
    const size_t n = (size_t)w * h;
    for (size_t p = 0; p < n; ++p)
    {
        const U8 a = src[p * 4 + 3];
        dst_b[p * 3 + 0] = dst_b[p * 3 + 1] = dst_b[p * 3 + 2] = (U8)(255 - a);
        dst_w[p * 3 + 0] = dst_w[p * 3 + 1] = dst_w[p * 3 + 2] = a;
    }
    LLPointer<LLImageFormatted> form_b = LLImageFormatted::createFromType(image->getCodec());
    if (form_b && form_b->encode(matte_black, 0)) form_b->save(base + "_matte" + extension);
    LLPointer<LLImageFormatted> form_w = LLImageFormatted::createFromType(image->getCodec());
    if (form_w && form_w->encode(matte_white, 0)) form_w->save(base + "_matte_inv" + extension);
}

// ---------------------------------------------------------------------------
// Snapshot floater helpers
// ---------------------------------------------------------------------------

void gmdxSnapshotSwapOrientation(LLFloaterSnapshotBase* floater)
{
    if (!floater || !floater->impl) return;
    LLSnapshotLivePreview* previewp = floater->getPreviewView();
    if (!previewp) return;
    S32 cur_w = 0, cur_h = 0;
    previewp->getSize(cur_w, cur_h);
    if ((cur_w <= 0 || cur_h <= 0) && gViewerWindow)
    {
        cur_w = gViewerWindow->getWindowWidthRaw();
        cur_h = gViewerWindow->getWindowHeightRaw();
    }
    if (cur_w <= 0 || cur_h <= 0) return;
    const S32 new_w = cur_h, new_h = cur_w;

    LLPanelSnapshot* panel = floater->impl->getActivePanel(floater);
    if (panel)
    {
        // A preset with the swapped size, else Custom with it.
        LLComboBox* combo = panel->findChild<LLComboBox>(panel->getImageSizeComboName());
        const std::string target = llformat("[i%d,i%d]", new_w, new_h);
        if (combo) combo->setValue(target);
        if (combo && combo->getValue().asString() == target)
        {
            combo->onCommit();
        }
        else
        {
            if (combo) combo->setValue("[i-1,i-1]");
            panel->enableAspectRatioCheckbox(true);
            LLSpinCtrl* wsp = panel->getWidthSpinner();
            LLSpinCtrl* hsp = panel->getHeightSpinner();
            if (wsp) { wsp->setEnabled(true); wsp->setValue(new_w); }
            if (hsp) { hsp->setEnabled(true); hsp->setValue(new_h); }
            LLSD info;
            info["w"] = new_w;
            info["h"] = new_h;
            floater->notify(LLSD().with("custom-res-change", info));
        }
    }
    else
    {
        previewp->setSize(new_w, new_h);
        floater->impl->setStatus(LLFloaterSnapshotBase::ImplBase::STATUS_READY);
        previewp->mForceUpdateSnapshot = true;
        previewp->updateSnapshot(true);
    }
    floater->impl->updateControls(floater);
}

void gmdxSnapshotUpdateControls(LLFloaterSnapshotBase* floater)
{
    if (!floater) return;
    // The orientation buttons show the current one (click: the other).
    S32 w = 0, h = 0;
    if (LLSnapshotLivePreview* previewp = floater->getPreviewView()) previewp->getSize(w, h);
    if ((w <= 0 || h <= 0) && gViewerWindow)
    {
        w = gViewerWindow->getWindowWidthRaw();
        h = gViewerWindow->getWindowHeightRaw();
    }
    const bool portrait = h > w;
    const std::string icon = portrait ? "Icon_Orientation_Portrait" : "Icon_Orientation_Landscape";
    const std::string tip = floater->getString(portrait ? "orientation_portrait_tip" : "orientation_landscape_tip");
    for (const char* name : { "swap_portrait_btn", "swap_resolution_btn" })
    {
        if (LLButton* btn = floater->findChild<LLButton>(name))
        {
            btn->setImageOverlay(icon, LLFontGL::HCENTER);
            btn->setToolTip(tip);
        }
    }
    if (LLView* popout = floater->findChild<LLView>("avatar_scope_popout_btn"))
    {
        popout->setEnabled(scopeSetting() == SNAPSHOT_AVATARS_SELECTED);
    }
}

void gmdxSnapshotShowPicker(LLFloaterSnapshotBase* floater)
{
    if (!floater) return;
    LLFloaterSnapshotAvatarSelect* picker = LLFloaterReg::showTypedInstance<LLFloaterSnapshotAvatarSelect>("snapshot_avatar_select");
    if (!picker) return;
    floater->addDependentFloater(picker);   // docked beside the Snapshot floater
    picker->refreshList();
}

void gmdxSnapshotScopeChanged(LLFloaterSnapshotBase* floater)
{
    if (!floater) return;
    // (the combo, not the setting: its control may not be written yet at commit time)
    U32 scope = scopeSetting();
    if (LLComboBox* combo = floater->findChild<LLComboBox>("avatar_scope_combo"))
        scope = (U32)llclamp(combo->getValue().asInteger(), (S32)SNAPSHOT_AVATARS_SELF, (S32)SNAPSHOT_AVATARS_ALL);
    if (scope == SNAPSHOT_AVATARS_SELECTED)
    {
        gmdxSnapshotShowPicker(floater);
    }
    else if (LLFloaterSnapshotAvatarSelect* picker = LLFloaterReg::findTypedInstance<LLFloaterSnapshotAvatarSelect>("snapshot_avatar_select"))
    {
        if (picker->isInVisibleChain()) picker->closeFloater();
    }
}

// ---------------------------------------------------------------------------
// LLFloaterSnapshotAvatarSelect
// ---------------------------------------------------------------------------

LLFloaterSnapshotAvatarSelect::LLFloaterSnapshotAvatarSelect(const LLSD& key)
:   LLFloater(key)
{
}

// static
void LLFloaterSnapshotAvatarSelect::onAvatarNameCacheCallback(LLHandle<LLFloaterSnapshotAvatarSelect> handle, const LLUUID& agent_id, const LLAvatarName& av_name)
{
    if (LLFloaterSnapshotAvatarSelect* floater = handle.get()) floater->onAvatarNameCache(agent_id, av_name);
}

// static
void LLFloaterSnapshotAvatarSelect::onCheckItemCallback(LLHandle<LLFloaterSnapshotAvatarSelect> handle, LLUICtrl* ctrl, const LLUUID& id)
{
    if (LLFloaterSnapshotAvatarSelect* floater = handle.get()) floater->onCheckItem(ctrl, id);
}

bool LLFloaterSnapshotAvatarSelect::postBuild()
{
    mAvatarList = getChild<LLScrollListCtrl>("avatar_list");
    mFilterEditor = getChild<LLFilterEditor>("avatar_filter");
    mFilterEditor->setCommitCallback(boost::bind(&LLFloaterSnapshotAvatarSelect::onFilterEdit, this, _2));
    childSetAction("select_all_btn", boost::bind(&LLFloaterSnapshotAvatarSelect::onSelectAll, this));
    childSetAction("clear_btn", boost::bind(&LLFloaterSnapshotAvatarSelect::onDeselectAll, this));
    childSetAction("refresh_btn", boost::bind(&LLFloaterSnapshotAvatarSelect::refreshList, this));
    return LLFloater::postBuild();
}

void LLFloaterSnapshotAvatarSelect::onOpen(const LLSD& key)
{
    refreshList();
}

// A display name for the list (RLVa @shownames: anonymized).
static std::string pickerName(const LLUUID& id, const std::string& display, const std::string& user)
{
    if (!RlvActions::canShowName(RlvActions::SNC_DEFAULT, id)) return RlvStrings::getAnonym(display);
    std::string full = display;
    if (!user.empty() && user != display) full += " (" + user + ")";
    return full;
}

void LLFloaterSnapshotAvatarSelect::refreshList()
{
    mNearbyAvatars.clear();
    const LLVector3d camera_pos = gAgentCamera.getCameraPositionGlobal();
    // RLVa @shownearby: nobody to list.
    if (RlvActions::canShowNearbyAgents())
    {
        std::set<LLUUID> seen_ids;
        for (LLCharacter* character : LLCharacter::sInstances)
        {
            LLVOAvatar* avatar = dynamic_cast<LLVOAvatar*>(character);
            if (!avatar || avatar->isDead() || avatar->mIsDummy || avatar->isOrphaned() || avatar->isControlAvatar()
                || avatar->isSelf() || avatar->isUIAvatar())
                continue;
            const LLUUID av_id = avatar->getID();
            if (!seen_ids.insert(av_id).second) continue;
            NearbyAvatarInfo info;
            info.id = av_id;
            info.distance = (F32)(avatar->getPositionGlobal() - camera_pos).magVec();
            LLAvatarName av_name;
            if (LLAvatarNameCache::get(av_id, &av_name))
            {
                info.displayName = av_name.getDisplayName();
                info.userName = av_name.getUserName();
            }
            else
            {
                info.displayName = gCacheName ? gCacheName->getDefaultName() : std::string("Loading...");
                LLAvatarNameCache::get(av_id, boost::bind(&LLFloaterSnapshotAvatarSelect::onAvatarNameCacheCallback,
                                                          getDerivedHandle<LLFloaterSnapshotAvatarSelect>(), _1, _2));
            }
            mNearbyAvatars.push_back(info);
        }
    }
    // Closest to the camera first.
    std::sort(mNearbyAvatars.begin(), mNearbyAvatars.end(),
              [](const NearbyAvatarInfo& a, const NearbyAvatarInfo& b) { return a.distance < b.distance; });

    if (!mAvatarList) return;
    mAvatarList->deleteAllItems();
    std::string filter_lower = mFilterString;
    LLStringUtil::toLower(filter_lower);
    for (const NearbyAvatarInfo& info : mNearbyAvatars)
    {
        const std::string name = pickerName(info.id, info.displayName, info.userName);
        if (!filter_lower.empty())
        {
            std::string name_lower = name;
            LLStringUtil::toLower(name_lower);
            if (name_lower.find(filter_lower) == std::string::npos) continue;
        }
        LLSD row;
        row["id"] = info.id;
        LLSD& columns = row["columns"];
        columns[0]["column"] = "check";
        columns[0]["type"] = "checkbox";
        columns[0]["value"] = gSnapshotSelectedAvatars.count(info.id) > 0;
        columns[1]["column"] = "name";
        columns[1]["type"] = "text";
        columns[1]["value"] = name;
        columns[2]["column"] = "distance";
        columns[2]["type"] = "text";
        columns[2]["value"] = llformat("%.1fm", info.distance);
        LLScrollListItem* item = mAvatarList->addElement(row);
        if (!item) continue;
        LLScrollListCheck* check_cell = dynamic_cast<LLScrollListCheck*>(item->getColumn(0));
        if (check_cell && check_cell->getCheckBox())
        {
            check_cell->getCheckBox()->setCommitCallback(boost::bind(&LLFloaterSnapshotAvatarSelect::onCheckItemCallback,
                                                                     getDerivedHandle<LLFloaterSnapshotAvatarSelect>(), _1, info.id));
        }
    }
}

void LLFloaterSnapshotAvatarSelect::onCheckItem(LLUICtrl* ctrl, const LLUUID& id)
{
    LLCheckBoxCtrl* checkbox = dynamic_cast<LLCheckBoxCtrl*>(ctrl);
    if (!checkbox) return;
    if (checkbox->get()) gSnapshotSelectedAvatars.insert(id);
    else gSnapshotSelectedAvatars.erase(id);
    updatePreview();
}

void LLFloaterSnapshotAvatarSelect::onSelectAll()
{
    for (const NearbyAvatarInfo& info : mNearbyAvatars) gSnapshotSelectedAvatars.insert(info.id);
    refreshList();
    updatePreview();
}

void LLFloaterSnapshotAvatarSelect::onDeselectAll()
{
    gSnapshotSelectedAvatars.clear();
    refreshList();
    updatePreview();
}

void LLFloaterSnapshotAvatarSelect::onFilterEdit(const LLSD& value)
{
    mFilterString = value.asString();
    refreshList();
}

void LLFloaterSnapshotAvatarSelect::onAvatarNameCache(const LLUUID& agent_id, const LLAvatarName& av_name)
{
    for (NearbyAvatarInfo& info : mNearbyAvatars)
    {
        if (info.id != agent_id) continue;
        info.displayName = av_name.getDisplayName();
        info.userName = av_name.getUserName();
        break;
    }
    if (!mAvatarList) return;
    if (LLScrollListItem* item = mAvatarList->getItem(agent_id))
    {
        if (LLScrollListCell* name_cell = item->getColumn(1))
            name_cell->setValue(pickerName(agent_id, av_name.getDisplayName(), av_name.getUserName()));
    }
}

void LLFloaterSnapshotAvatarSelect::updatePreview()
{
    LLFloaterSnapshot* snap_floater = LLFloaterSnapshot::findInstance();
    LLSnapshotLivePreview* previewp = snap_floater ? snap_floater->getPreviewView() : nullptr;
    if (previewp) previewp->updateSnapshot(true, true);
}
