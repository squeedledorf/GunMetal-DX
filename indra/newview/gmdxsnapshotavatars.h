/**
 * @file gmdxsnapshotavatars.h
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

// <GMDX> Ported from SoapStorm (Soap Frenzy): transparent avatar snapshots, holdout alpha mattes
// (FSSaveAvatarMasksWithSnapshot), the snapshot avatar scope (FSSnapshotAvatarScope: all, self +
// selected, self only) with its docked picker (floater_snapshot_avatar_select.xml), and the
// portrait / landscape swap. SoapStorm did the rendering in GL; here it is the Vulkan renderer:
//  - AVATAR LAYERS ("Avatar (Transparent)", "Avatar Matte (Black on White / White on Black)"):
//    an offscreen render (vkRenderOffscreenSnapshot) whose main view keeps avatars only (the world
//    still lights and shadows them; no sky, no particles, no bloom / glow / DoF / lens effects),
//    and whose matte is the avatar-id pass's coverage averaged over the supersampled recordings
//    (vkgfx/gmdxsnapmattevk.cpp). Other avatars are only in it when the same view WITH the world
//    shows them (the offscreen warm-up recording's avatar ids): an avatar-only render never shows
//    anyone through a wall.
//  - SCOPE: avatars outside FSSnapshotAvatarScope leave every pass of a snapshot's offscreen render
//    (the renderer's avatar render policy, gmdxSnapshotHidesAvatar); a filtered world snapshot
//    therefore always renders offscreen. Snapshots with the interface are the presented frame and
//    are not filtered.

#ifndef GMDX_SNAPSHOTAVATARS_H
#define GMDX_SNAPSHOTAVATARS_H

#include "llfloater.h"
#include "llsnapshotmodel.h"
#include "lluuid.h"

#include <set>
#include <string>
#include <vector>

class LLAvatarName;
class LLFilterEditor;
class LLFloaterSnapshotBase;
class LLImageFormatted;
class LLImageRaw;
class LLScrollListCtrl;
class LLVOAvatar;

// FSSnapshotAvatarScope values.
enum ESnapshotAvatarScope
{
    SNAPSHOT_AVATARS_SELF = 0,
    SNAPSHOT_AVATARS_SELECTED = 1,
    SNAPSHOT_AVATARS_ALL = 2
};

// The avatars "Self + Selected" adds (session only; the picker edits it).
extern std::set<LLUUID> gSnapshotSelectedAvatars;

bool gmdxIsAvatarLayer(LLSnapshotModel::ESnapshotLayerType type);

// While a snapshot renders: true = leave this avatar (or animesh) out of every pass. The Vulkan
// renderer's avatar render policy asks (llcleanrenderervk.cpp avatarHiddenByPolicy). Your own
// avatar and what it wears is never left out.
bool gmdxSnapshotHidesAvatar(LLVOAvatar* av);
// The offscreen render currently recording is an avatar layer (vkRenderOffscreenSnapshot).
bool gmdxSnapshotAvatarOnly();
// The renderer, after the avatar layer's warm-up recording: the avatars its main view showed.
void gmdxSnapshotMatteSetSeen(const std::vector<LLUUID>& ids);

// LLViewerWindow::rawSnapshot. A world (non-UI) snapshot of `type` must render offscreen: an
// avatar layer, a scope that leaves avatars out, or mattes saved with it (they come from an
// offscreen render: the snapshot must frame exactly as they do).
bool gmdxSnapshotWantsOffscreen(LLSnapshotModel::ESnapshotLayerType type);
// Applies FSSnapshotAvatarScope to the offscreen renders while it lives (rawSnapshot).
class GMDXSnapshotScopeHold
{
public:
    explicit GMDXSnapshotScopeHold(bool on);
    ~GMDXSnapshotScopeHold();
private:
    bool mOn;
};
// An avatar layer at image_width x image_height (rows bottom-up, as rawSnapshot): RGBA for the
// transparent avatar, grey for the mattes.
bool gmdxRawAvatarSnapshot(LLImageRaw* raw, S32 image_width, S32 image_height,
                           LLSnapshotModel::ESnapshotLayerType type, S32 max_size);
// LLViewerWindow::saveImageLocal, after `image` was written to `filepath`: with
// FSSaveAvatarMasksWithSnapshot, <name>_matte and <name>_matte_inv beside it.
void gmdxSaveSnapshotMattes(LLImageFormatted* image, const std::string& filepath, const std::string& extension);

// Snapshot floater helpers.
void gmdxSnapshotSwapOrientation(LLFloaterSnapshotBase* floater);   // landscape <-> portrait
void gmdxSnapshotUpdateControls(LLFloaterSnapshotBase* floater);    // orientation icons, picker button
void gmdxSnapshotScopeChanged(LLFloaterSnapshotBase* floater);      // picker shown / closed
void gmdxSnapshotShowPicker(LLFloaterSnapshotBase* floater);

// "Select Avatars": the nearby avatars "Self + Selected" may add, docked beside the Snapshot floater.
class LLFloaterSnapshotAvatarSelect : public LLFloater
{
public:
    LLFloaterSnapshotAvatarSelect(const LLSD& key);
    ~LLFloaterSnapshotAvatarSelect() override = default;

    bool postBuild() override;
    void onOpen(const LLSD& key) override;

    void refreshList();
    void updatePreview();

private:
    static void onAvatarNameCacheCallback(LLHandle<LLFloaterSnapshotAvatarSelect> handle, const LLUUID& agent_id, const LLAvatarName& av_name);
    static void onCheckItemCallback(LLHandle<LLFloaterSnapshotAvatarSelect> handle, LLUICtrl* ctrl, const LLUUID& id);

    void onCheckItem(LLUICtrl* ctrl, const LLUUID& id);
    void onSelectAll();
    void onDeselectAll();
    void onFilterEdit(const LLSD& value);
    void onAvatarNameCache(const LLUUID& agent_id, const LLAvatarName& av_name);

    struct NearbyAvatarInfo
    {
        LLUUID id;
        std::string displayName;
        std::string userName;
        F32 distance = 0.f;
    };

    LLScrollListCtrl* mAvatarList = nullptr;
    LLFilterEditor*   mFilterEditor = nullptr;
    std::string       mFilterString;
    std::vector<NearbyAvatarInfo> mNearbyAvatars;
};

#endif // GMDX_SNAPSHOTAVATARS_H
