/**
 * @file llglsandbox.cpp
 * @brief GL functionality access
 *
 * $LicenseInfo:firstyear=2003&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
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
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

/**
 * Contains ALL methods which directly access GL functionality
 * except for core rendering engine functionality.
 */

#include "llviewerprecompiledheaders.h"

#include "llviewercontrol.h"

#include "llgl.h"
#include "llrender.h"
#include "llwin32headers.h"
#include "llparcel.h"
#include "llui.h"

#include "lldrawable.h"
#include "lltextureentry.h"
#include "llviewercamera.h"

#include "llvoavatarself.h"
#include "llsky.h"
#include "llagent.h"
#include "lltoolmgr.h"
#include "llselectmgr.h"
#include "llhudmanager.h"
#include "llhudtext.h"
#include "llhudrender.h"
#include "llviewerobjectlist.h"
#include "llcleancursorpick.h"   // #704 drawn-geometry rectangle test
#include "vkgfx/llvulkancontext.h"   // LLVKRectQuery
#include "llmeshrepository.h"   // #585 released-read tripwire
#include "llvovolume.h"
#include "lltoolselectrect.h"
#include "llviewerwindow.h"
#include "llsurface.h"
#include "llwind.h"
#include "llworld.h"
#include "llviewerparcelmgr.h"
#include "llviewerregion.h"
#include "llpreviewtexture.h"
#include "llresmgr.h"
#include "pipeline.h"
#include "llrender.h"
// [RLVa:KB] - Checked: 2010-04-11 (RLVa-1.2.0e)
#include "rlvactions.h"
#include "rlvhandler.h"
#include "rlvmodifiers.h"
// [/RLVa:KB]

#include <cstring>
#include <unordered_map>
#include <vector>

// Height of the yellow selection highlight posts for land
constexpr F32 PARCEL_POST_HEIGHT = 0.666f;

// Returns true if you got at least one object
void LLToolSelectRect::handleRectangleSelection(S32 x, S32 y, MASK mask)
{
// [RLVa:KB] - @edit* and @interact
    // Block rectangle selection if:
    //   - prevented from editing anything at all
    //   - prevented from interacting at all
    if (RlvActions::isRlvEnabled() && (RlvActions::canEdit(ERlvCheckType::Nothing) || RlvActions::hasBehaviour(RLV_BHVR_INTERACT)) )
    {
        return;
    }
// [/RLVa:KB]

    const LLVector3 av_pos = gAgent.getPositionAgent();
    bool limit_select_distance = gSavedSettings.getBOOL("LimitSelectDistance");
    F32 select_dist = gSavedSettings.getF32("MaxSelectDistance");
// [RLVa:KB] - Checked: 2010-04-11 (RLVa-1.2.0e) | Modified: RLVa-1.0.0g
    if (gRlvHandler.hasBehaviour(RLV_BHVR_FARTOUCH))
    {
        static RlvCachedBehaviourModifier<float> s_nFartouchDist(RLV_MODIFIER_FARTOUCHDIST);
        limit_select_distance = true;
        select_dist = s_nFartouchDist;
    }
// [/RLVa:KB]
    const F32 select_dist_squared = select_dist * select_dist;

    bool deselect = (mask == MASK_CONTROL);
    S32 left =  llmin(x, mDragStartX);
    S32 right = llmax(x, mDragStartX);
    S32 top =   llmax(y, mDragStartY);
    S32 bottom =llmin(y, mDragStartY);

    // Window pixels (the same space as the published camera viewport).
    left = ll_round((F32) left * LLUI::getScaleFactor().mV[VX]);
    right = ll_round((F32) right * LLUI::getScaleFactor().mV[VX]);
    top = ll_round((F32) top * LLUI::getScaleFactor().mV[VY]);
    bottom = ll_round((F32) bottom * LLUI::getScaleFactor().mV[VY]);

    S32 width = right - left + 1;
    S32 height = top - bottom + 1;

    bool grow_selection = (height > mDragLastHeight || width > mDragLastWidth);
    bool shrink_selection = (height < mDragLastHeight || width < mDragLastWidth);
    if (!grow_selection && !shrink_selection)
    {
        // nothing to do
        return;
    }
    mDragLastHeight = height;
    mDragLastWidth = width;

    // Screen-space selection with the live viewer camera (origin, axes, FOV, aspect) and
    // each prim's oriented bounding box. Independent of any published GL matrix state,
    // and cheap: 8 corners per candidate, no per-vertex work.
    const LLViewerCamera* cam = LLViewerCamera::getInstance();
    const LLVector3 cam_origin = cam->getOrigin();
    const LLVector3 at = cam->getAtAxis(), lf = cam->getLeftAxis(), up = cam->getUpAxis();
    const F32 tan_y = tanf(cam->getView() * 0.5f);
    const F32 tan_x = tan_y * cam->getAspect();
    const LLRect wv = gViewerWindow->getWorldViewRectRaw();
    const F32 vp_x = (F32)wv.mLeft, vp_y = (F32)wv.mBottom, vp_w = (F32)wv.getWidth(), vp_h = (F32)wv.getHeight();
    const F32 rl = (F32)left, rr = (F32)right, rb = (F32)bottom, rt = (F32)top;
    const F32 far_dist = cam->getFar();
    const F32 far_sq = far_dist * far_dist;

    // Project an agent-space point to window pixels; false if behind the near plane.
    auto to_screen = [&](const LLVector3& p, F32& sx, F32& sy) -> bool
    {
        const LLVector3 d = p - cam_origin;
        const F32 z = d * at;
        if (z <= 0.05f)
        {
            return false;
        }
        const F32 nx = -(d * lf) / (z * tan_x);   // left axis points to screen -x
        const F32 ny = (d * up) / (z * tan_y);
        sx = vp_x + (nx * 0.5f + 0.5f) * vp_w;
        sy = vp_y + (ny * 0.5f + 0.5f) * vp_h;
        return true;
    };

    auto in_rect = [&](F32 sx, F32 sy) { return sx >= rl && sx <= rr && sy >= rb && sy <= rt; };

    // Inclusive ("select partial"): any of the prim's vertices inside the rectangle.
    // Otherwise: all of them. Bounding-box corners settle the clear cases (box misses the
    // rectangle: no; box entirely inside: yes); only prims the box straddles test their
    // vertices, so a small box over a big street prim no longer selects the street.
    // 0 = no, 1 = yes, 2 = straddles (vertex test).
    auto boxVerdict = [&](LLViewerObject* vobjp) -> int
    {
        const LLVector3 center = vobjp->getPositionAgent();
        const LLQuaternion rot = vobjp->getRotationRegion();
        const LLVector3 half = vobjp->getScale() * 0.5f;
        F32 minx = FLT_MAX, maxx = -FLT_MAX, miny = FLT_MAX, maxy = -FLT_MAX;
        bool all_in = true, any_front = false;
        for (S32 c = 0; c < 8; ++c)
        {
            LLVector3 corner((c & 1) ? half.mV[VX] : -half.mV[VX],
                             (c & 2) ? half.mV[VY] : -half.mV[VY],
                             (c & 4) ? half.mV[VZ] : -half.mV[VZ]);
            corner = center + corner * rot;
            F32 sx, sy;
            if (!to_screen(corner, sx, sy))
            {
                all_in = false;
                continue;
            }
            any_front = true;
            all_in = all_in && in_rect(sx, sy);
            minx = llmin(minx, sx); maxx = llmax(maxx, sx);
            miny = llmin(miny, sy); maxy = llmax(maxy, sy);
        }
        if (!any_front)
        {
            return 0;
        }
        if (all_in)
        {
            return 1;    // whole box (so every vertex) inside
        }
        if (maxx < rl || minx > rr || maxy < rb || miny > rt)
        {
            return 0;   // footprint misses the rectangle
        }
        return 2;
    };

    // A straddling prim's vertices, LL's volume: only for a prim the renderer has no
    // geometry for (not ingested; #585: then LL's arrays may be released too -- nothing on
    // screen to test).
    auto llVertexVerdict = [&](LLViewerObject* vobjp) -> bool
    {
        const bool inclusive = LLSelectMgr::sRectSelectInclusive;
        LLDrawable* drawable = vobjp->mDrawable;
        LLVolume* volume = vobjp->getVolume();
        if (!drawable || !volume)
        {
            return false;
        }
        LLVOVolume* vo_volume = (LLVOVolume*)vobjp;
        vo_volume->updateRelativeXform();
        LLMatrix4a rel, render;
        rel.loadu(vo_volume->getRelativeXform());
        render.loadu(LLMatrix4(vo_volume->getRenderRotation(), LLVector4(vo_volume->getRenderPosition())));
        bool any = false;
        for (S32 i = 0; i < volume->getNumVolumeFaces(); i++)
        {
            const LLVolumeFace& face = volume->getVolumeFace(i);
            if (!face.mPositions)
            {
                if (face.isGeometryReleased())
                {
                    LLMeshRepository::noteReleasedRead(LLMeshRepository::RRS_RECT_SELECT);
                }
                continue;
            }
            for (S32 v = 0; v < face.mNumVertices; v++)
            {
                LLVector4a vec;
                rel.affineTransform(face.mPositions[v], vec);
                if (drawable->isActive())
                {
                    LLVector4a t = vec;
                    render.affineTransform(t, vec);
                }
                F32 sx, sy;
                const bool inside = to_screen(LLVector3(vec.getF32ptr()), sx, sy) && in_rect(sx, sy);
                if (inside && inclusive)
                {
                    return true;
                }
                if (!inside && !inclusive)
                {
                    return false;
                }
                any = true;
            }
        }
        return !inclusive && any;
    };

    // STRADDLING PRIMS (#704): their DRAWN vertices (the renderer's LOD of every face,
    // through the instance's model matrix) tested on the GPU -- one query for all of them,
    // after the bounding boxes have settled everything else.
    std::unordered_map<LLViewerObject*, bool> straddle_verdict;
    auto resolveStraddlers = [&](const std::vector<LLViewerObject*>& straddlers)
    {
        if (straddlers.empty())
        {
            return;
        }
        const bool inclusive = LLSelectMgr::sRectSelectInclusive;
        LLVKRectQuery rq;
        std::memcpy(rq.eye, cam_origin.mV, sizeof(rq.eye));
        std::memcpy(rq.at, at.mV, sizeof(rq.at));
        std::memcpy(rq.left, lf.mV, sizeof(rq.left));
        std::memcpy(rq.up, up.mV, sizeof(rq.up));
        rq.tan_x = tan_x;
        rq.tan_y = tan_y;
        rq.viewport[0] = vp_x; rq.viewport[1] = vp_y; rq.viewport[2] = vp_w; rq.viewport[3] = vp_h;
        rq.rect[0] = rl; rq.rect[1] = rr; rq.rect[2] = rb; rq.rect[3] = rt;
        std::vector<U8> any_in, any_out;
        const bool ran = LLCleanCursorPick::drawnRectTest(straddlers, rq, any_in, any_out);
        for (size_t i = 0; i < straddlers.size(); ++i)
        {
            LLViewerObject* o = straddlers[i];
            bool v;
            if (ran && (any_in[i] || any_out[i]))
            {
                v = inclusive ? any_in[i] != 0 : (any_out[i] == 0 && any_in[i] != 0);
            }
            else
            {
                v = llVertexVerdict(o);   // no drawn geometry for it
            }
            straddle_verdict[o] = v;
        }
    };

    auto selectable = [&](LLViewerObject* vobjp) -> bool
    {
        if (!vobjp || vobjp->isDead() || !vobjp->mDrawable ||
            vobjp->getPCode() != LL_PCODE_VOLUME || vobjp->isAttachment())
        {
            return false;
        }
        const LLVector3 pos = vobjp->mDrawable->getPositionAgent();
        if ((pos - cam_origin).lengthSquared() > far_sq)
        {
            return false;
        }
        if (limit_select_distance && dist_vec_squared(vobjp->mDrawable->getWorldPosition(), av_pos) > select_dist_squared)
        {
            return false;
        }
// [RLVa:KB] - Checked: 2010-11-29 (RLVa-1.3.0c) | Added: RLVa-1.3.0c
        if ( (RlvActions::isRlvEnabled()) && (!RlvActions::canEdit(vobjp)) )
        {
            return false;
        }
// [/RLVa:KB]
        return true;
    };

    if (shrink_selection)
    {
        // The highlighted prims, their boxes first, then the straddlers' vertices at once.
        struct collect : public LLSelectedObjectFunctor
        {
            std::vector<LLViewerObject*> mObjs;
            virtual bool apply(LLViewerObject* vobjp)
            {
                if (vobjp->mDrawable && vobjp->getPCode() == LL_PCODE_VOLUME && !vobjp->isAttachment())
                {
                    mObjs.push_back(vobjp);
                }
                return true;
            }
        } highlighted;
        LLSelectMgr::getInstance()->getHighlightedObjects()->applyToObjects(&highlighted);
        std::vector<LLViewerObject*> straddlers, drop;
        for (LLViewerObject* vobjp : highlighted.mObjs)
        {
            const int b = boxVerdict(vobjp);
            if (b == 0) drop.push_back(vobjp);
            else if (b == 2) straddlers.push_back(vobjp);
        }
        resolveStraddlers(straddlers);
        for (LLViewerObject* vobjp : straddlers)
        {
            if (!straddle_verdict[vobjp]) drop.push_back(vobjp);
        }
        for (LLViewerObject* vobjp : drop)
        {
            LLSelectMgr::getInstance()->unhighlightObjectOnly(vobjp);
        }
    }

    if (grow_selection)
    {
        std::vector<LLViewerObject*> straddlers;
        for (S32 i = 0, n = gObjectList.getNumObjects(); i < n; ++i)
        {
            LLViewerObject* vobjp = gObjectList.getObject(i);
            if (!selectable(vobjp) || (deselect && !vobjp->isSelected()))
            {
                continue;
            }
            const int b = boxVerdict(vobjp);
            if (b == 1)
            {
                LLSelectMgr::getInstance()->highlightObjectOnly(vobjp);
            }
            else if (b == 2)
            {
                straddlers.push_back(vobjp);
            }
        }
        // (a prim the shrink pass already decided keeps that verdict)
        std::vector<LLViewerObject*> todo;
        for (LLViewerObject* vobjp : straddlers)
        {
            if (!straddle_verdict.count(vobjp)) todo.push_back(vobjp);
        }
        resolveStraddlers(todo);
        for (LLViewerObject* vobjp : straddlers)
        {
            if (straddle_verdict[vobjp])
            {
                LLSelectMgr::getInstance()->highlightObjectOnly(vobjp);
            }
        }
    }
}

constexpr F32 WIND_RELATIVE_ALTITUDE = 25.f;

void LLWind::renderVectors()
{
    // Renders the wind as vectors (used for debug)
    S32 i,j;
    F32 x,y;

    F32 region_width_meters = LLWorld::getInstance()->getRegionWidthInMeters();

    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gGL.pushMatrix();
    LLVector3 origin_agent;
    origin_agent = gAgent.getPosAgentFromGlobal(mOriginGlobal);
    gGL.translatef(origin_agent.mV[VX], origin_agent.mV[VY], gAgent.getPositionAgent().mV[VZ] + WIND_RELATIVE_ALTITUDE);
    for (j = 0; j < mSize; j++)
    {
        for (i = 0; i < mSize; i++)
        {
            x = mVelX[i + j*mSize] * WIND_SCALE_HACK;
            y = mVelY[i + j*mSize] * WIND_SCALE_HACK;
            gGL.pushMatrix();
            gGL.translatef((F32)i * region_width_meters/mSize, (F32)j * region_width_meters/mSize, 0.f);
            gGL.color3f(0.f, 1.f, 0.f);
            gGL.begin(LLRender::POINTS);
                gGL.vertex3f(0.f, 0.f, 0.f);
            gGL.end();
            gGL.color3f(1.f, 0.f, 0.f);
            gGL.begin(LLRender::LINES);
                gGL.vertex3f(x * 0.1f, y * 0.1f, 0.f);
                gGL.vertex3f(x, y, 0.f);
            gGL.end();
            gGL.popMatrix();
        }
    }
    gGL.popMatrix();
}




// Used by lltoolselectland
void LLViewerParcelMgr::renderRect(const LLVector3d &west_south_bottom_global,
                                   const LLVector3d &east_north_top_global)
{
    LLRender::UIScope gls_ui;
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    LLRender::DepthScope gls_depth(true);

    LLVector3 west_south_bottom_agent = gAgent.getPosAgentFromGlobal(west_south_bottom_global);
    F32 west    = west_south_bottom_agent.mV[VX];
    F32 south   = west_south_bottom_agent.mV[VY];
//  F32 bottom  = west_south_bottom_agent.mV[VZ] - 1.f;

    LLVector3 east_north_top_agent = gAgent.getPosAgentFromGlobal(east_north_top_global);
    F32 east    = east_north_top_agent.mV[VX];
    F32 north   = east_north_top_agent.mV[VY];
//  F32 top     = east_north_top_agent.mV[VZ] + 1.f;

    // HACK: At edge of last region of world, we need to make sure the region
    // resolves correctly so we can get a height value.
    const F32 FUDGE = 0.01f;

    F32 sw_bottom = LLWorld::getInstance()->resolveLandHeightAgent( LLVector3( west, south, 0.f ) );
    F32 se_bottom = LLWorld::getInstance()->resolveLandHeightAgent( LLVector3( east-FUDGE, south, 0.f ) );
    F32 ne_bottom = LLWorld::getInstance()->resolveLandHeightAgent( LLVector3( east-FUDGE, north-FUDGE, 0.f ) );
    F32 nw_bottom = LLWorld::getInstance()->resolveLandHeightAgent( LLVector3( west, north-FUDGE, 0.f ) );

    F32 sw_top = sw_bottom + PARCEL_POST_HEIGHT;
    F32 se_top = se_bottom + PARCEL_POST_HEIGHT;
    F32 ne_top = ne_bottom + PARCEL_POST_HEIGHT;
    F32 nw_top = nw_bottom + PARCEL_POST_HEIGHT;

    LLUI::setLineWidth(2.f);
    gGL.color4f(1.f, 1.f, 0.f, 1.f);

    // Cheat and give this the same pick-name as land
    gGL.begin(LLRender::LINES);

    gGL.vertex3f(west, north, nw_bottom);
    gGL.vertex3f(west, north, nw_top);

    gGL.vertex3f(east, north, ne_bottom);
    gGL.vertex3f(east, north, ne_top);

    gGL.vertex3f(east, south, se_bottom);
    gGL.vertex3f(east, south, se_top);

    gGL.vertex3f(west, south, sw_bottom);
    gGL.vertex3f(west, south, sw_top);

    gGL.end();

    gGL.color4f(1.f, 1.f, 0.f, 0.2f);
    gGL.begin(LLRender::TRIANGLE_STRIP);
    {
        gGL.vertex3f(west, north, nw_bottom);
        gGL.vertex3f(west, north, nw_top);
        gGL.vertex3f(east, north, ne_bottom);
        gGL.vertex3f(east, north, ne_top);
        gGL.vertex3f(east, south, se_bottom);
        gGL.vertex3f(east, south, se_top);
        gGL.vertex3f(west, south, sw_top);
        gGL.vertex3f(west, south, sw_bottom);
        gGL.vertex3f(west, north, nw_top);
        gGL.vertex3f(west, north, nw_bottom);
    }
    gGL.end();

    LLUI::setLineWidth(1.f);
}


// north = a wall going north/south.  Need that info to set up texture
// coordinates correctly.
void LLViewerParcelMgr::renderOneSegment(F32 x1, F32 y1, F32 x2, F32 y2, F32 height, U8 direction, LLViewerRegion* regionp)
{
    // HACK: At edge of last region of world, we need to make sure the region
    // resolves correctly so we can get a height value.
    const F32 BORDER = REGION_WIDTH_METERS - 0.1f;

    F32 clamped_x1 = x1;
    F32 clamped_y1 = y1;
    F32 clamped_x2 = x2;
    F32 clamped_y2 = y2;

    if (clamped_x1 > BORDER) clamped_x1 = BORDER;
    if (clamped_y1 > BORDER) clamped_y1 = BORDER;
    if (clamped_x2 > BORDER) clamped_x2 = BORDER;
    if (clamped_y2 > BORDER) clamped_y2 = BORDER;

    F32 z;
    F32 z1;
    F32 z2;

    z1 = regionp->getLand().resolveHeightRegion( LLVector3( clamped_x1, clamped_y1, 0.f ) );
    z2 = regionp->getLand().resolveHeightRegion( LLVector3( clamped_x2, clamped_y2, 0.f ) );

    // Convert x1 and x2 from region-local to agent coords.
    LLVector3 origin = regionp->getOriginAgent();
    x1 += origin.mV[VX];
    x2 += origin.mV[VX];
    y1 += origin.mV[VY];
    y2 += origin.mV[VY];

    if (height < 1.f)
    {
        z = z1+height;
        gGL.vertex3f(x1, y1, z);

        gGL.vertex3f(x1, y1, z1);

        gGL.vertex3f(x2, y2, z2);

        gGL.vertex3f(x1, y1, z);

        gGL.vertex3f(x2, y2, z2);

        z = z2+height;
        gGL.vertex3f(x2, y2, z);
    }
    else
    {
        F32 tex_coord1;
        F32 tex_coord2;

        if (WEST_MASK == direction)
        {
            tex_coord1 = y1;
            tex_coord2 = y2;
        }
        else if (SOUTH_MASK == direction)
        {
            tex_coord1 = x1;
            tex_coord2 = x2;
        }
        else if (EAST_MASK == direction)
        {
            tex_coord1 = y2;
            tex_coord2 = y1;
        }
        else /* (NORTH_MASK == direction) */
        {
            tex_coord1 = x2;
            tex_coord2 = x1;
        }


        gGL.texCoord2f(tex_coord1 * 0.5f + 0.5f, z1 * 0.5f);
        gGL.vertex3f(x1, y1, z1);

        gGL.texCoord2f(tex_coord2 * 0.5f + 0.5f, z2 * 0.5f);
        gGL.vertex3f(x2, y2, z2);

        // top edge stairsteps
        z = llmax(z2 + height, z1 + height);
        gGL.texCoord2f(tex_coord2 * 0.5f + 0.5f, z * 0.5f);
        gGL.vertex3f(x2, y2, z);

        gGL.texCoord2f(tex_coord1 * 0.5f + 0.5f, z1 * 0.5f);
        gGL.vertex3f(x1, y1, z1);

        gGL.texCoord2f(tex_coord2 * 0.5f + 0.5f, z * 0.5f);
        gGL.vertex3f(x2, y2, z);

        gGL.texCoord2f(tex_coord1 * 0.5f + 0.5f, z * 0.5f);
        gGL.vertex3f(x1, y1, z);
    }
}


void LLViewerParcelMgr::renderHighlightSegments(const U8* segments, LLViewerRegion* regionp)
{
    S32 x, y;
    F32 x1, y1; // start point
    F32 x2, y2; // end point
    bool has_segments = false;

    LLRender::UIScope gls_ui;
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    LLRender::DepthScope gls_depth(true);

    gGL.color4f(1.f, 1.f, 0.f, 0.2f);

    const S32 STRIDE = (mParcelsPerEdge+1);

    // Cheat and give this the same pick-name as land


    for (y = 0; y < STRIDE; y++)
    {
        for (x = 0; x < STRIDE; x++)
        {
            U8 segment_mask = segments[x + y*STRIDE];

            if (segment_mask & SOUTH_MASK)
            {
                x1 = x * PARCEL_GRID_STEP_METERS;
                y1 = y * PARCEL_GRID_STEP_METERS;

                x2 = x1 + PARCEL_GRID_STEP_METERS;
                y2 = y1;

                if (!has_segments)
                {
                    has_segments = true;
                    gGL.begin(LLRender::TRIANGLES);
                }
                renderOneSegment(x1, y1, x2, y2, PARCEL_POST_HEIGHT, SOUTH_MASK, regionp);
            }

            if (segment_mask & WEST_MASK)
            {
                x1 = x * PARCEL_GRID_STEP_METERS;
                y1 = y * PARCEL_GRID_STEP_METERS;

                x2 = x1;
                y2 = y1 + PARCEL_GRID_STEP_METERS;

                if (!has_segments)
                {
                    has_segments = true;
                    gGL.begin(LLRender::TRIANGLES);
                }
                renderOneSegment(x1, y1, x2, y2, PARCEL_POST_HEIGHT, WEST_MASK, regionp);
            }
        }
    }

    if (has_segments)
    {
        gGL.end();
    }
}


void LLViewerParcelMgr::renderCollisionSegments(U8* segments, bool use_pass, LLViewerRegion* regionp)
{

    S32 x, y;
    F32 x1, y1; // start point
    F32 x2, y2; // end point
    F32 alpha = 0;
    F32 dist = 0;
    F32 dx, dy;
    F32 collision_height;

    const S32 STRIDE = (mParcelsPerEdge+1);

    LLVector3 pos = gAgent.getPositionAgent();

    F32 pos_x = pos.mV[VX];
    F32 pos_y = pos.mV[VY];

    LLRender::UIScope gls_ui;
    LLRender::DepthScope gls_depth(true, false);
    LLRender::CullScope cull(false);

    if (mCollisionBanned == BA_BANNED ||
        regionp->getRegionFlag(REGION_FLAGS_BLOCK_FLYOVER))
    {
        collision_height = BAN_HEIGHT;
    }
    else
    {
        collision_height = PARCEL_HEIGHT;
    }


    if (use_pass && (mCollisionBanned == BA_NOT_ON_LIST))
    {
        gGL.getTexUnit(0)->bind(mPassImage);
    }
    else
    {
        gGL.getTexUnit(0)->bind(mBlockedImage);
    }

    gGL.begin(LLRender::TRIANGLES);

    for (y = 0; y < STRIDE; y++)
    {
        for (x = 0; x < STRIDE; x++)
        {
            U8 segment_mask = segments[x + y*STRIDE];
            U8 direction;
            const F32 MAX_ALPHA = 0.95f;
            const S32 DIST_OFFSET = 5;
            const S32 MIN_DIST_SQ = DIST_OFFSET*DIST_OFFSET;
            const S32 MAX_DIST_SQ = 169;

            if (segment_mask & SOUTH_MASK)
            {
                x1 = x * PARCEL_GRID_STEP_METERS;
                y1 = y * PARCEL_GRID_STEP_METERS;

                x2 = x1 + PARCEL_GRID_STEP_METERS;
                y2 = y1;

                dy = (pos_y - y1) + DIST_OFFSET;

                if (pos_x < x1)
                    dx = pos_x - x1;
                else if (pos_x > x2)
                    dx = pos_x - x2;
                else
                    dx = 0;

                dist = dx*dx+dy*dy;

                if (dist < MIN_DIST_SQ)
                    alpha = MAX_ALPHA;
                else if (dist > MAX_DIST_SQ)
                    alpha = 0.0f;
                else
                    alpha = 30/dist;

                alpha = llclamp(alpha, 0.0f, MAX_ALPHA);

                gGL.color4f(1.f, 1.f, 1.f, alpha);

                if ((pos_y - y1) < 0) direction = SOUTH_MASK;
                else        direction = NORTH_MASK;

                // avoid Z fighting
                renderOneSegment(x1+0.1f, y1+0.1f, x2+0.1f, y2+0.1f, collision_height, direction, regionp);

            }

            if (segment_mask & WEST_MASK)
            {
                x1 = x * PARCEL_GRID_STEP_METERS;
                y1 = y * PARCEL_GRID_STEP_METERS;

                x2 = x1;
                y2 = y1 + PARCEL_GRID_STEP_METERS;

                dx = (pos_x - x1) + DIST_OFFSET;

                if (pos_y < y1)
                    dy = pos_y - y1;
                else if (pos_y > y2)
                    dy = pos_y - y2;
                else
                    dy = 0;

                dist = dx*dx+dy*dy;

                if (dist < MIN_DIST_SQ)
                    alpha = MAX_ALPHA;
                else if (dist > MAX_DIST_SQ)
                    alpha = 0.0f;
                else
                    alpha = 30/dist;

                alpha = llclamp(alpha, 0.0f, MAX_ALPHA);

                gGL.color4f(1.f, 1.f, 1.f, alpha);

                if ((pos_x - x1) > 0) direction = WEST_MASK;
                else        direction = EAST_MASK;

                // avoid Z fighting
                renderOneSegment(x1+0.1f, y1+0.1f, x2+0.1f, y2+0.1f, collision_height, direction, regionp);

            }
        }
    }

    gGL.end();
}

void LLViewerParcelMgr::resetCollisionTimer()
{
    mCollisionTimer.reset();
    mRenderCollision = true;
}

void draw_line_cube(F32 width, const LLVector3& center)
{
    width = 0.5f * width;
    gGL.vertex3f(center.mV[VX] + width ,center.mV[VY] + width,center.mV[VZ] + width);
    gGL.vertex3f(center.mV[VX] - width ,center.mV[VY] + width,center.mV[VZ] + width);
    gGL.vertex3f(center.mV[VX] - width ,center.mV[VY] + width,center.mV[VZ] + width);
    gGL.vertex3f(center.mV[VX] - width ,center.mV[VY] - width,center.mV[VZ] + width);
    gGL.vertex3f(center.mV[VX] - width ,center.mV[VY] - width,center.mV[VZ] + width);
    gGL.vertex3f(center.mV[VX] + width ,center.mV[VY] - width,center.mV[VZ] + width);
    gGL.vertex3f(center.mV[VX] + width ,center.mV[VY] - width,center.mV[VZ] + width);
    gGL.vertex3f(center.mV[VX] + width ,center.mV[VY] + width,center.mV[VZ] + width);

    gGL.vertex3f(center.mV[VX] + width ,center.mV[VY] + width,center.mV[VZ] - width);
    gGL.vertex3f(center.mV[VX] - width ,center.mV[VY] + width,center.mV[VZ] - width);
    gGL.vertex3f(center.mV[VX] - width ,center.mV[VY] + width,center.mV[VZ] - width);
    gGL.vertex3f(center.mV[VX] - width ,center.mV[VY] - width,center.mV[VZ] - width);
    gGL.vertex3f(center.mV[VX] - width ,center.mV[VY] - width,center.mV[VZ] - width);
    gGL.vertex3f(center.mV[VX] + width ,center.mV[VY] - width,center.mV[VZ] - width);
    gGL.vertex3f(center.mV[VX] + width ,center.mV[VY] - width,center.mV[VZ] - width);
    gGL.vertex3f(center.mV[VX] + width ,center.mV[VY] + width,center.mV[VZ] - width);

    gGL.vertex3f(center.mV[VX] + width ,center.mV[VY] + width,center.mV[VZ] + width);
    gGL.vertex3f(center.mV[VX] + width ,center.mV[VY] + width,center.mV[VZ] - width);
    gGL.vertex3f(center.mV[VX] - width ,center.mV[VY] + width,center.mV[VZ] + width);
    gGL.vertex3f(center.mV[VX] - width ,center.mV[VY] + width,center.mV[VZ] - width);
    gGL.vertex3f(center.mV[VX] - width ,center.mV[VY] - width,center.mV[VZ] + width);
    gGL.vertex3f(center.mV[VX] - width ,center.mV[VY] - width,center.mV[VZ] - width);
    gGL.vertex3f(center.mV[VX] + width ,center.mV[VY] - width,center.mV[VZ] + width);
    gGL.vertex3f(center.mV[VX] + width ,center.mV[VY] - width,center.mV[VZ] - width);
}

void draw_cross_lines(const LLVector3& center, F32 dx, F32 dy, F32 dz)
{
    gGL.vertex3f(center.mV[VX] - dx, center.mV[VY], center.mV[VZ]);
    gGL.vertex3f(center.mV[VX] + dx, center.mV[VY], center.mV[VZ]);
    gGL.vertex3f(center.mV[VX], center.mV[VY] - dy, center.mV[VZ]);
    gGL.vertex3f(center.mV[VX], center.mV[VY] + dy, center.mV[VZ]);
    gGL.vertex3f(center.mV[VX], center.mV[VY], center.mV[VZ] - dz);
    gGL.vertex3f(center.mV[VX], center.mV[VY], center.mV[VZ] + dz);
}

// Debug beacons (area search, pathfinding, Develop/View > Beacons, telehub, poser).
// Drawn from the VK world-overlay pass (LLAppViewer's vk_worldOverlays) with the
// world camera loaded: LL's two-pass look -- a faint tall cross visible through
// geometry, then a depth-tested bright core. All beacons go out in one LINES batch
// per distinct line width (stable-sorted by width), not one begin/end per beacon.
void LLViewerObjectList::renderObjectBeacons()
{
    if (mDebugBeacons.empty())
    {
        return;
    }

    // Resolve object-tracking beacons to the pose the renderer draws this frame;
    // drop the ones whose object is gone (dead, out of range, HUD attachment).
    mDebugBeacons.erase(std::remove_if(mDebugBeacons.begin(), mDebugBeacons.end(),
        [this](LLDebugBeacon& beacon)
        {
            if (beacon.mObjectID.isNull())
            {
                return false;
            }
            return !getBeaconPositionAgent(findObject(beacon.mObjectID), beacon.mPositionAgent);
        }), mDebugBeacons.end());
    if (mDebugBeacons.empty())
    {
        return;
    }
    std::stable_sort(mDebugBeacons.begin(), mDebugBeacons.end(),
        [](const LLDebugBeacon& a, const LLDebugBeacon& b) { return a.mLineWidth < b.mLineWidth; });

    LLRender::UIScope gls_ui;
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

    auto draw_pass = [this](F32 alpha_scale, F32 dx, F32 dy, F32 dz)
    {
        size_t i = 0;
        const size_t n = mDebugBeacons.size();
        while (i < n)
        {
            const S32 line_width = mDebugBeacons[i].mLineWidth;
            gGL.flush();
            gGL.setLineWidth((F32)llmax(line_width, 1));
            gGL.begin(LLRender::LINES);
            for (; i < n && mDebugBeacons[i].mLineWidth == line_width; ++i)
            {
                const LLDebugBeacon& beacon = mDebugBeacons[i];
                LLColor4 color = beacon.mColor;
                color.mV[VALPHA] *= alpha_scale;
                gGL.color4fv(color.mV);
                draw_cross_lines(beacon.mPositionAgent, dx, dy, dz);
                draw_line_cube(0.10f, beacon.mPositionAgent);
            }
            gGL.end();
        }
    };

    {
        // Through geometry: the tall beam (+-50 m vertical) at quarter alpha.
        LLRender::DepthScope gls_depth(false, false);
        draw_pass(0.25f, 2.0f, 2.0f, 50.f);
    }
    {
        // Depth-tested core at full beacon alpha.
        LLRender::DepthScope gls_depth(true, false);
        draw_pass(1.f, 0.5f, 0.5f, 0.5f);
    }
    gGL.flush();
    gGL.setLineWidth(1.f);
}

// Beacon labels, drawn from the VK HUD-text pass (world camera loaded; hud_render_text
// projects to a 2D ortho, no depth -- labels show through geometry like the beam).
// Only on-screen labels, nearest kMaxBeaconLabels first: area search can queue
// hundreds of beacons and text is the expensive part.
void LLViewerObjectList::renderObjectBeaconLabels()
{
    constexpr size_t kMaxBeaconLabels = 64;
    if (mDebugBeacons.empty())
    {
        return;
    }

    LLViewerCamera* camera = LLViewerCamera::getInstance();
    const LLVector3 cam_origin = camera->getOrigin();
    std::vector<std::pair<F32, const LLDebugBeacon*>> labels;
    for (const LLDebugBeacon& beacon : mDebugBeacons)
    {
        if (beacon.mString.empty())
        {
            continue;
        }
        LLCoordGL screen;
        if (!camera->projectPosAgentToScreen(beacon.mPositionAgent, screen, false))
        {
            continue;   // behind the camera / off screen
        }
        labels.emplace_back((beacon.mPositionAgent - cam_origin).magVecSquared(), &beacon);
    }
    if (labels.empty())
    {
        return;
    }
    if (labels.size() > kMaxBeaconLabels)
    {
        std::nth_element(labels.begin(), labels.begin() + kMaxBeaconLabels, labels.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
        labels.resize(kMaxBeaconLabels);
    }
    // Back to front so nearer labels draw on top.
    std::sort(labels.begin(), labels.end(), [](const auto& a, const auto& b) { return a.first > b.first; });

    const LLFontGL* font = LLFontGL::getFontSansSerif();
    if (!font)
    {
        return;
    }
    LLRender::UIScope gls_ui;
    LLRender::DepthScope gls_depth(false, false);
    gGL.getTexUnit(0)->enable(LLTexUnit::TT_TEXTURE);
    for (const auto& entry : labels)
    {
        const LLDebugBeacon& beacon = *entry.second;
        const LLWString wstr = utf8str_to_wstring(beacon.mString);
        const F32 x_offset = -0.5f * font->getWidthF32(wstr.c_str());
        hud_render_text(wstr, beacon.mPositionAgent, *font, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW,
                        x_offset, 0.f, beacon.mTextColor, false);
    }
    gGL.color4f(1.f, 1.f, 1.f, 1.f);
}

void LLSky::renderSunMoonBeacons(const LLVector3& pos_agent, const LLVector3& direction, LLColor4 color)
{
    LLRender::UIScope gls_ui;
    gGL.flush();
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

    LLVector3 pos_end;
    for (S32 i = 0; i < 3; ++i)
    {
        pos_end.mV[i] = pos_agent.mV[i] + (50 * direction.mV[i]);
    }
    // Read the setting live: LLPipeline::DebugBeaconLineWidth is only refreshed by
    // the feature manager / a menu, not on DebugBeaconLineWidth changes.
    static LLCachedControl<S32> beacon_line_width(gSavedSettings, "DebugBeaconLineWidth", 1);
    gGL.setLineWidth((F32)llmax((S32)beacon_line_width, 1));
    gGL.begin(LLRender::LINES);
    color.mV[3] *= 0.5f;
    gGL.color4fv(color.mV);
    draw_cross_lines(pos_agent, 0.5f, 0.5f, 0.5f);
    draw_cross_lines(pos_end, 2.f, 2.f, 2.f);
    gGL.vertex3fv(pos_agent.mV);
    gGL.vertex3fv(pos_end.mV);
    gGL.end();

    gGL.flush();
    gGL.setLineWidth(1.f);

}
