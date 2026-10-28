/**
 * @file gmdxhudeditpan.cpp
 * @brief HUD edit: keeps the manipulator on screen, shows the real screen bounds.
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

#include "llviewerprecompiledheaders.h"
#include "gmdxhudeditpan.h"

#include "llagentcamera.h"
#include "llfocusmgr.h"
#include "llfontgl.h"
#include "llrender2dutils.h"
#include "llselectmgr.h"
#include "llui.h"
#include "llviewercontrol.h"
#include "llviewerjointattachment.h"
#include "llviewerobject.h"
#include "llviewerwindow.h"
#include "llvoavatarself.h"

#include <algorithm>
#include <vector>

namespace
{
    // How far the manipulator reaches from its centre, in raw pixels at UI scale 1: the
    // rotate ring is 100 px (llmaniprotate.cpp RADIUS_PIXELS, times the UI scale) plus its
    // stroke; the translate arrows reach about 80 px (50 px * hotspot 1.2 * selected 1.3).
    constexpr F32 GIZMO_REACH_PX = 120.f;

    // As LLManip::getPivotPoint for a HUD selection.
    LLVector3 hud_pivot(LLObjectSelectionHandle& sel)
    {
        static LLCachedControl<bool> edit_root_axis(gSavedSettings, "SlipstreamEditRootAxis", false);
        if (edit_root_axis && sel->getObjectCount() > 1)
        {
            LLSelectNode* first = sel->getFirstNode();
            LLViewerObject* root = sel->getFirstRootObject(true);
            if (first && first->mSelectedGLTFNode == -1 && root && !root->isDead())
            {
                return root->getBoundingBoxAgent().getCenterAgent();
            }
        }
        return LLSelectMgr::getInstance()->getBBoxOfSelection().getCenterAgent();
    }
}

void gmdx_keep_hud_gizmo_on_screen(LLVector3& pan, float zoom)
{
    static LLCachedControl<bool> enabled(gSavedSettings, "GMDXHUDEditKeepGizmoOnScreen", true);
    // No clamp during a drag: the dragged object follows the mouse, so panning under it
    // would scroll the view on and on (edge scrolling).
    if (!enabled || !gViewerWindow || gFocusMgr.getMouseCapture() || zoom <= 0.f)
    {
        return;
    }
    LLObjectSelectionHandle sel = LLSelectMgr::getInstance()->getSelection();
    if (!sel->getObjectCount() || sel->getSelectType() != SELECT_TYPE_HUD)
    {
        return;
    }

    // HUD screen space: 1 unit = the HUD rect height, centred, x spanning +-aspect/2
    // (get_hud_matrices). A HUD-local point p shows at zoom * (p - pan).
    const F32 h = (F32)llmax(gViewerWindow->getHudRectRaw().getHeight(), 1);
    const F32 margin = GIZMO_REACH_PX * llmax(LLUI::getScaleFactor().mV[VX], 1.f) / h;
    const F32 half_w = llmax(0.5f * gViewerWindow->getHudProjectionAspect() - margin, 0.f) / zoom;
    const F32 half_h = llmax(0.5f - margin, 0.f) / zoom;

    const LLVector3 c = hud_pivot(sel);
    pan.mV[VY] = llclamp(pan.mV[VY], c.mV[VY] - half_w, c.mV[VY] + half_w);
    pan.mV[VZ] = llclamp(pan.mV[VZ], c.mV[VZ] - half_h, c.mV[VZ] + half_h);
}

void gmdx_draw_hud_edit_bounds()
{
    static LLCachedControl<bool> enabled(gSavedSettings, "GMDXHUDEditShowBounds", true);
    if (!enabled || !gViewerWindow || !isAgentAvatarValid() || !gAgentCamera.isEditingHUD())
    {
        return;
    }

    // HUD-local point -> UI pixel in the current (zoomed, panned) HUD view, as mousePointHUD
    // inverts it.
    const LLRect hud = gViewerWindow->getHudRectScaled();
    const F32 H = (F32)llmax(hud.getHeight(), 1);
    const F32 cx = 0.5f * (F32)(hud.mLeft + hud.mRight);
    const F32 cy = 0.5f * (F32)(hud.mBottom + hud.mTop);
    const F32 zoom = gAgentCamera.mHUDCurZoom;
    const LLVector3 pan = gAgentCamera.mHUDCurPan;
    auto px = [&](F32 y) { return ll_round(cx - zoom * (y - pan.mV[VY]) * H); };
    auto py = [&](F32 z) { return ll_round(cy + zoom * (z - pan.mV[VZ]) * H); };

    // The real screen in HUD-local units: the HUD rect at the normal zoom (HUDScaleFactor
    // alone) and no pan. +y is screen left.
    static LLCachedControl<F32> hud_scale_factor(gSavedSettings, "HUDScaleFactor");
    const F32 zoom0 = llmax((F32)hud_scale_factor, 0.01f);
    const F32 half_w = 0.5f * (F32)hud.getWidth() / H / zoom0;
    const F32 half_h = 0.5f / zoom0;

    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    const LLColor4 edge(1.f, 0.85f, 0.2f, 0.9f);
    const LLColor4 guide(1.f, 0.85f, 0.2f, 0.3f);
    const S32 left = px(half_w), right = px(-half_w), top = py(half_h), bottom = py(-half_h);
    gl_rect_2d(left, top, right, bottom, edge, false);
    gl_rect_2d(left - 1, top + 1, right + 1, bottom - 1, LLColor4(0.f, 0.f, 0.f, 0.5f), false);   // reads on bright HUDs
    gl_line_2d(px(0.f), top, px(0.f), bottom, guide);
    gl_line_2d(left, py(0.f), right, py(0.f), guide);

    const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
    font->renderUTF8("Screen (normal HUD view)", 0, left + 4, top - 4, edge,
                     LLFontGL::LEFT, LLFontGL::TOP, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW);

    // HUD attachment points: a small cross and the name(s); Center and Center 2 share a spot.
    std::vector<std::pair<LLVector3, std::string>> anchors;
    for (const auto& [id, point] : gAgentAvatarp->mAttachmentPoints)
    {
        if (!point || !point->getIsHUDAttachment())
        {
            continue;
        }
        const LLVector3 pos = point->getWorldPosition();
        auto same = std::find_if(anchors.begin(), anchors.end(),
                                 [&](const auto& a) { return dist_vec_squared(a.first, pos) < 1.e-8f; });
        if (same != anchors.end())
        {
            same->second += " / " + point->getName();
        }
        else
        {
            anchors.emplace_back(pos, point->getName());
        }
    }
    for (const auto& [pos, name] : anchors)
    {
        const S32 x = px(pos.mV[VY]), y = py(pos.mV[VZ]);
        gl_line_2d(x - 6, y, x + 7, y, edge);
        gl_line_2d(x, y - 6, x, y + 7, edge);
        // Labels lean inward so the edge and corner ones stay readable.
        const LLFontGL::HAlign ha = pos.mV[VY] > 0.01f ? LLFontGL::LEFT : (pos.mV[VY] < -0.01f ? LLFontGL::RIGHT : LLFontGL::HCENTER);
        const LLFontGL::VAlign va = pos.mV[VZ] > 0.01f ? LLFontGL::TOP : LLFontGL::BOTTOM;
        const S32 dx = ha == LLFontGL::LEFT ? 8 : (ha == LLFontGL::RIGHT ? -8 : 0);
        const S32 dy = va == LLFontGL::TOP ? -8 : 8;
        font->renderUTF8(name, 0, x + dx, y + dy, LLColor4(1.f, 0.85f, 0.2f, 0.75f), ha, va, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW);
    }

    // Selected prims the normal view does not fully show: red when wholly off screen,
    // orange when over an edge.
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    LLObjectSelectionHandle sel = LLSelectMgr::getInstance()->getSelection();
    for (LLObjectSelection::iterator it = sel->begin(); it != sel->end(); ++it)
    {
        LLViewerObject* obj = (*it)->getObject();
        if (!obj || obj->isDead() || !obj->isHUDAttachment())
        {
            continue;
        }
        const LLBBox box = obj->getBoundingBoxAgent();
        const LLVector3& lo = box.getMinLocal();
        const LLVector3& hi = box.getMaxLocal();
        F32 y0 = F32_MAX, y1 = -F32_MAX, z0 = F32_MAX, z1 = -F32_MAX;
        for (S32 i = 0; i < 8; ++i)
        {
            const LLVector3 c = box.localToAgent(LLVector3(i & 1 ? hi.mV[VX] : lo.mV[VX],
                                                           i & 2 ? hi.mV[VY] : lo.mV[VY],
                                                           i & 4 ? hi.mV[VZ] : lo.mV[VZ]));
            y0 = llmin(y0, c.mV[VY]); y1 = llmax(y1, c.mV[VY]);
            z0 = llmin(z0, c.mV[VZ]); z1 = llmax(z1, c.mV[VZ]);
        }
        const bool inside = y0 >= -half_w && y1 <= half_w && z0 >= -half_h && z1 <= half_h;
        if (inside)
        {
            continue;
        }
        const bool outside = y0 > half_w || y1 < -half_w || z0 > half_h || z1 < -half_h;
        const LLColor4 c = outside ? LLColor4(1.f, 0.2f, 0.2f, 0.9f) : LLColor4(1.f, 0.55f, 0.1f, 0.9f);
        gl_rect_2d(px(y1), py(z1), px(y0), py(z0), c, false);
        if (outside)
        {
            font->renderUTF8("off screen", 0, px(0.5f * (y0 + y1)), py(0.5f * (z0 + z1)), c,
                             LLFontGL::HCENTER, LLFontGL::VCENTER, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW);
            gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        }
    }
}
