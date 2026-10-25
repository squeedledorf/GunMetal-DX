/**
 * @file gmdxselectionoutline.h
 * @brief LL's edit-mode selection silhouettes, drawn through the Vulkan world overlay.
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

// <GMDX> The selection outline as LL / Firestorm draw it (LLSelectMgr::renderSilhouettes and
// LLSelectNode::renderOneSilhouette): for prims and sculpties, only the silhouette edges that
// LLVolume::generateSilhouetteVertices finds, as a glowing ribbon with LL's scrolling
// silhouette texture, plus dimmed occluded edges when "show hidden selection" is on.
//
// Only in GMDXSelectionStyle 0 (Classic). Mesh has no silhouette in LL either
// (generateSilhouetteVertices returns nothing for it; LL draws a mesh's triangle wireframe),
// so mesh, trees, grass and avatars get Firestorm's mesh wireframe from the Vulkan renderer
// (vkgfx/gmdxseloutlinevk.cpp). Style 1 (Outline) and 2 (Wireframe) draw everything on the
// GPU and no silhouettes. (updateSilhouettes still generates them for the selection, as LL
// does: gating it would clear the objects' MOVED flags every frame.)

#pragma once

#include "v4color.h"

class LLSelectNode;
class LLViewerObject;

namespace GMDXSelectionOutline
{
    // Silhouettes are on (GMDXSelectionStyle 0, Classic).
    bool enabled();
    // This object is outlined by a silhouette, so the GPU wireframe must skip it.
    bool usesSilhouette(const LLViewerObject* objectp);
    // The root / child colours of the GPU selection lists: Firestorm's in Classic
    // (GMDXClassicSilhouette*Color), the skin's Silhouette*Color in the other styles.
    LLColor4 parentColor();
    LLColor4 childColor();
    // LLSelectMgr::renderSilhouettes: call with gGL holding the world camera (world overlay
    // segment) or the HUD selection transform (for_hud).
    void render(bool for_hud);
}
