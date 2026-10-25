/**
 * @file gmdxselectionoutline.cpp
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

// <GMDX> See gmdxselectionoutline.h. The drawing below is LL's renderSilhouettes /
// renderOneSilhouette (Firestorm's llselectmgr.cpp), minus the mesh wireframe branch, which
// the GPU renderer draws (vkgfx/gmdxseloutlinevk.cpp).

#include "llviewerprecompiledheaders.h"
#include "gmdxselectionoutline.h"

#include "llagentcamera.h"
#include "lldrawable.h"
#include "llfloaterinspect.h"
#include "llfloaterreg.h"
#include "llfocusmgr.h"
#include "llframetimer.h"
#include "llkeyboard.h"
#include "llrender.h"
#include "llselectmgr.h"
#include "llsidepaneltaskinfo.h"
#include "lluicolortable.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewermediafocus.h"
#include "llviewerobject.h"
#include "llviewertexture.h"
#include "llvoavatarself.h"
#include "llvolume.h"

namespace
{
// LL's SelectionHighlight* defaults (Firestorm settings.xml; LLSelectMgr doubles the alpha).
// Slipstream dropped those settings, so they live here as constants.
constexpr F32 HIGHLIGHT_THICKNESS = 0.01f;   // fraction of the view distance
constexpr F32 HIGHLIGHT_U_SCALE   = 0.1f;
constexpr F32 HIGHLIGHT_V_SCALE   = 1.0f;
constexpr F32 HIGHLIGHT_ALPHA     = 0.2f * 2.f;
constexpr F32 HIGHLIGHT_U_ANIM    = 0.0f;
constexpr F32 HIGHLIGHT_V_ANIM    = 0.5f;

// LLSelectNode::renderOneSilhouette.
void renderOneSilhouette(LLSelectNode* node, const LLColor4& color, bool render_hidden)
{
    LLViewerObject* objectp = node->getObject();
    if (!objectp || objectp->mDrawable.isNull() || !node->mSilhouetteExists)
        return;
    if (node->mSilhouetteVertices.empty() || node->mSilhouetteNormals.size() != node->mSilhouetteVertices.size())
        return;
    if (!objectp->getVolume())
        return;

    const bool is_hud_object = objectp->isHUDAttachment();
    const std::vector<LLVector3>& verts = node->mSilhouetteVertices;
    const std::vector<LLVector3>& norms = node->mSilhouetteNormals;

    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.pushMatrix();
    gGL.pushUIMatrix();
    gGL.loadUIIdentity();
    // LL reloads gGLModelView for world objects; the overlay already holds the world view.
    if (objectp->mDrawable->isActive())
    {
        gGL.multMatrix((F32*)objectp->getRenderMatrix().mMatrix);
    }

    LLViewerCamera& camera = LLViewerCamera::instance();
    F32 silhouette_thickness;
    if (isAgentAvatarValid() && is_hud_object)
    {
        silhouette_thickness = HIGHLIGHT_THICKNESS / gAgentCamera.mHUDCurZoom;
    }
    else
    {
        LLVector3 view_vector = camera.getOrigin() - objectp->getRenderPosition();
        silhouette_thickness = view_vector.magVec() * HIGHLIGHT_THICKNESS * (camera.getView() / camera.getDefaultFOV());
    }
    const F32 animation_time = (F32)LLFrameTimer::getElapsedSeconds();

    F32 u_coord = fmod(animation_time * HIGHLIGHT_U_ANIM, 1.f);
    const F32 v_coord = 1.f - fmod(animation_time * HIGHLIGHT_V_ANIM, 1.f);
    const F32 u_divisor = 1.f / ((F32)(verts.size() - 1));

    if (render_hidden)
    {
        // Occluded edges: thin lines, added where the scene is in front (LL: GL_GEQUAL).
        gGL.flush();
        gGL.blendFunc(LLRender::BF_SOURCE_COLOR, LLRender::BF_ONE);
        LLRender::DepthScope gls_depth(true, false, LLDepthFunc::GEQUAL);
        gGL.begin(LLRender::LINES);
        gGL.color4f(color.mV[VRED], color.mV[VGREEN], color.mV[VBLUE], 0.4f);
        for (size_t i = 0; i + 1 < verts.size(); i += 2)
        {
            u_coord += u_divisor * HIGHLIGHT_U_SCALE;
            gGL.texCoord2f(u_coord, v_coord);
            gGL.vertex3fv(verts[i].mV);
            u_coord += u_divisor * HIGHLIGHT_U_SCALE;
            gGL.texCoord2f(u_coord, v_coord);
            gGL.vertex3fv(verts[i + 1].mV);
        }
        gGL.end();
        gGL.flush();
        u_coord = fmod(animation_time * HIGHLIGHT_U_ANIM, 1.f);
    }

    // Visible edges: a ribbon pushed out along the surface normal, bright at the edge and
    // fading to nothing at its outer rim.
    gGL.flush();
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    {
        LLRender::DepthScope gls_depth(true, false);
        gGL.begin(LLRender::TRIANGLES);
        for (size_t i = 0; i + 1 < verts.size(); i += 2)
        {
            if (!norms[i].isFinite() || !norms[i + 1].isFinite())
            {   // skip skewed segments
                continue;
            }

            LLVector3 v[4];
            LLVector2 tc[4];
            v[0] = verts[i] + (norms[i] * silhouette_thickness);
            tc[0].set(u_coord, v_coord + HIGHLIGHT_V_SCALE);

            v[1] = verts[i];
            tc[1].set(u_coord, v_coord);

            u_coord += u_divisor * HIGHLIGHT_U_SCALE;

            v[2] = verts[i + 1] + (norms[i + 1] * silhouette_thickness);
            tc[2].set(u_coord, v_coord + HIGHLIGHT_V_SCALE);

            v[3] = verts[i + 1];
            tc[3].set(u_coord, v_coord);

            gGL.color4f(color.mV[VRED], color.mV[VGREEN], color.mV[VBLUE], 0.0f);
            gGL.texCoord2fv(tc[0].mV);
            gGL.vertex3fv(v[0].mV);

            gGL.color4f(color.mV[VRED] * 2, color.mV[VGREEN] * 2, color.mV[VBLUE] * 2, HIGHLIGHT_ALPHA);
            gGL.texCoord2fv(tc[1].mV);
            gGL.vertex3fv(v[1].mV);

            gGL.color4f(color.mV[VRED], color.mV[VGREEN], color.mV[VBLUE], 0.0f);
            gGL.texCoord2fv(tc[2].mV);
            gGL.vertex3fv(v[2].mV);

            gGL.vertex3fv(v[2].mV);

            gGL.color4f(color.mV[VRED] * 2, color.mV[VGREEN] * 2, color.mV[VBLUE] * 2, HIGHLIGHT_ALPHA);
            gGL.texCoord2fv(tc[1].mV);
            gGL.vertex3fv(v[1].mV);

            gGL.texCoord2fv(tc[3].mV);
            gGL.vertex3fv(v[3].mV);
        }
        gGL.end();
        gGL.flush();
    }

    gGL.popMatrix();
    gGL.popUIMatrix();
}
} // namespace

namespace GMDXSelectionOutline
{
bool enabled()
{
    static LLCachedControl<S32> style(gSavedSettings, "GMDXSelectionStyle", 0);
    return style <= 0;   // 0 Classic (1 Outline, 2 Wireframe: all on the GPU)
}

bool usesSilhouette(const LLViewerObject* objectp)
{
    // What LL builds a silhouette for (LLSelectMgr::generateSilhouette); mesh gets none.
    return enabled() && objectp && objectp->getPCode() == LL_PCODE_VOLUME && !objectp->isMesh();
}

LLColor4 parentColor()
{
    static const LLUIColor s_classic = LLUIColorTable::instance().getColor("GMDXClassicSilhouetteParentColor");
    return enabled() ? s_classic.get() : LLSelectMgr::sSilhouetteParentColor.get();
}

LLColor4 childColor()
{
    static const LLUIColor s_classic = LLUIColorTable::instance().getColor("GMDXClassicSilhouetteChildColor");
    return enabled() ? s_classic.get() : LLSelectMgr::sSilhouetteChildColor.get();
}

// LLSelectMgr::renderSilhouettes.
void render(bool for_hud)
{
    LLSelectMgr* sm = LLSelectMgr::getInstance();
    if (!enabled() || !sm->silhouettesEnabled())
        return;
    LLObjectSelectionHandle selected = sm->getSelection();
    LLObjectSelectionHandle highlighted = sm->getHighlightedObjects();
    if (selected->isEmpty() && highlighted->getNumNodes() == 0)
        return;

    // Looked up per frame (only while something is outlined): a static LLPointer would
    // outlive the texture list at shutdown.
    LLPointer<LLViewerFetchedTexture> silhouette_image =
        LLViewerTextureManager::getFetchedTextureFromFile("silhouette.j2c", FTT_LOCAL_FILE, true, LLGLTexture::BOOST_UI);
    // Firestorm's colours (see GMDXClassic* in colors.xml), not the skin's Highlight* /
    // Silhouette* ones: the Slipstream skin's pale child blue, doubled, drew white.
    static const LLUIColor s_highlight_inspect = LLUIColorTable::instance().getColor("GMDXClassicHighlightInspectColor");
    static const LLUIColor s_highlight_parent  = LLUIColorTable::instance().getColor("GMDXClassicHighlightParentColor");
    static const LLUIColor s_highlight_child   = LLUIColorTable::instance().getColor("GMDXClassicHighlightChildColor");
    static const LLUIColor s_context           = LLUIColorTable::instance().getColor("GMDXClassicContextSilhouetteColor");

    gGL.getTexUnit(0)->bind(silhouette_image);
    gGL.getTexUnit(0)->setTextureAddressMode(LLTexUnit::TAM_WRAP);   // u runs past 1 (LL's GL default)

    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.pushMatrix();
    if (for_hud && isAgentAvatarValid())
    {
        // LLViewerWindow::renderSelections set up the HUD frame; add the HUD zoom and pan
        // (#413) as its light-radius spheres do.
        const F32 zoom = gAgentCamera.mHUDCurZoom;
        gGL.scalef(zoom, zoom, zoom);
        const LLVector3& pan = gAgentCamera.mHUDCurPan;
        gGL.translatef(-pan.mV[VX], -pan.mV[VY], -pan.mV[VZ]);
    }

    if (!selected->isEmpty())
    {
        LLUUID inspect_item_id;
        LLFloaterInspect* inspect_instance = LLFloaterReg::findTypedInstance<LLFloaterInspect>("inspect");
        if (inspect_instance && inspect_instance->getVisible())
        {
            inspect_item_id = inspect_instance->getSelectedUUID();
        }
        else if (LLSidepanelTaskInfo* panel_task_info = LLSidepanelTaskInfo::getActivePanel())
        {
            inspect_item_id = panel_task_info->getSelectedUUID();
        }
        const LLUUID focus_item_id = LLViewerMediaFocus::getInstance()->getFocusedObjectID();

        for (LLObjectSelection::iterator iter = selected->begin(); iter != selected->end(); ++iter)
        {
            LLSelectNode* node = *iter;
            if (sm->getTEMode() && !node->hasSelectedTE())
                continue;
            LLViewerObject* objectp = node->getObject();
            if (!usesSilhouette(objectp) || objectp->isHUDAttachment() != for_hud)
                continue;

            if (objectp->getID() == focus_item_id)
                renderOneSilhouette(node, gFocusMgr.getFocusColor(), LLSelectMgr::sRenderHiddenSelections);
            else if (objectp->getID() == inspect_item_id)
                renderOneSilhouette(node, s_highlight_inspect.get(), LLSelectMgr::sRenderHiddenSelections);
            else if (node->isTransient())
                renderOneSilhouette(node, s_context.get(), false);   // Firestorm: not halved
            else if (objectp->isRootEdit())
                renderOneSilhouette(node, parentColor(), LLSelectMgr::sRenderHiddenSelections);
            else
                renderOneSilhouette(node, childColor(), LLSelectMgr::sRenderHiddenSelections);
        }
    }

    if (highlighted->getNumNodes())
    {
        // Box-select drag (and other highlights): red while Ctrl subtracts from the selection.
        const bool subtracting_from_selection = gKeyboard && (gKeyboard->currentMask(true) == MASK_CONTROL);
        // LL draws the highlights twice (its pass loop never uses the pass); kept for the
        // same brightness.
        for (S32 pass = 0; pass < 2; pass++)
        {
            for (LLObjectSelection::iterator iter = highlighted->begin(); iter != highlighted->end(); ++iter)
            {
                LLSelectNode* node = *iter;
                LLViewerObject* objectp = node->getObject();
                if (!usesSilhouette(objectp) || objectp->isHUDAttachment() != for_hud)
                    continue;
                if (subtracting_from_selection)
                    renderOneSilhouette(node, LLColor4::red, LLSelectMgr::sRenderHiddenSelections);
                else if (!objectp->isSelected())
                    renderOneSilhouette(node, objectp->isRoot() ? s_highlight_parent.get() : s_highlight_child.get(),
                                        LLSelectMgr::sRenderHiddenSelections);
            }
        }
    }

    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.popMatrix();
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
}
} // namespace GMDXSelectionOutline
