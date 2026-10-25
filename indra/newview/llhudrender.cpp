/**
 * @file llhudrender.cpp
 * @brief LLHUDRender class implementation
 *
 * $LicenseInfo:firstyear=2002&license=viewerlgpl$
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

#include "llviewerprecompiledheaders.h"

#include "llhudrender.h"

#include "llrender.h"
#include "llgl.h"
#include "llviewercamera.h"
#include "v3math.h"
#include "llquaternion.h"
#include "llfontgl.h"
#include "llwin32headers.h"
#include "llviewerwindow.h"
#include "llviewercontrol.h"
#include "lltimer.h"
#include "llui.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

void hud_render_utf8text(const std::string &str, const LLVector3 &pos_agent,
                     const LLFontGL &font,
                     const U8 style,
                     const LLFontGL::ShadowType shadow,
                     const F32 x_offset, const F32 y_offset,
                     const LLColor4& color,
                     const bool orthographic)
{
    LLWString wstr(utf8str_to_wstring(str));
    hud_render_text(wstr, pos_agent, font, style, shadow, x_offset, y_offset, color, orthographic);
}

void hud_render_text(const LLWString &wstr, const LLVector3 &pos_agent,
                    const LLFontGL &font,
                    const U8 style,
                    const LLFontGL::ShadowType shadow,
                    const F32 x_offset, const F32 y_offset,
                    const LLColor4& color,
                    const bool orthographic)
{
    LLViewerCamera* camera = LLViewerCamera::getInstance();
    // Do cheap plane culling
    LLVector3 dir_vec = pos_agent - camera->getOrigin();
    dir_vec /= dir_vec.magVec();

    if (wstr.empty() || (!orthographic && dir_vec * camera->getAtAxis() <= 0.f))
    {
        return;
    }

    LLVector3 right_axis;
    LLVector3 up_axis;
    if (orthographic)
    {
        // #919: one HUD unit = the HUD rect's height (LLViewerWindow::getHudRectScaled).
        const F32 hud_h = (F32)llmax(gViewerWindow->getHudRectScaled().getHeight(), 1);
        right_axis.setVec(0.f, -1.f / hud_h, 0.f);
        up_axis.setVec(0.f, 0.f, 1.f / hud_h);
    }
    else
    {
        camera->getPixelVectors(pos_agent, up_axis, right_axis);
    }
    LLCoordFrame render_frame = *camera;
    LLQuaternion rot;
    if (!orthographic)
    {
        rot = render_frame.getQuaternion();
        rot = rot * LLQuaternion(-F_PI_BY_TWO, camera->getYAxis());
        rot = rot * LLQuaternion(F_PI_BY_TWO, camera->getXAxis());
    }
    else
    {
        rot = LLQuaternion(-F_PI_BY_TWO, LLVector3(0.f, 0.f, 1.f));
        rot = rot * LLQuaternion(-F_PI_BY_TWO, LLVector3(0.f, 1.f, 0.f));
    }
    F32 angle;
    LLVector3 axis;
    rot.getAngleAxis(&angle, axis);

    LLVector3 render_pos = pos_agent + (floorf(x_offset) * right_axis) + (floorf(y_offset) * up_axis);

    //get the render_pos in screen space

    LLRect world_view_rect = gViewerWindow->getWorldViewRectRaw();
    // HUD text (orthographic) projects through the HUD camera over the HUD rect (#919),
    // the HUD pass's viewport; the 2D text layer below stays the world view's.
    const LLRect proj_rect = orthographic ? gViewerWindow->getHudRectRaw() : world_view_rect;
    glm::ivec4 viewport(proj_rect.mLeft, proj_rect.mBottom, proj_rect.getWidth(), proj_rect.getHeight());

    glm::vec3 win_coord = glm::project(glm::vec3(render_pos), get_current_modelview(), get_current_projection(), viewport);

    // RenderVKHudTextDiag: where HUD text lands (throttled, HUD only).
    if (orthographic)
    {
        static LLCachedControl<bool> s_diag(gSavedSettings, "RenderVKHudTextDiag", false);
        static F64 s_until = 0.0, s_next = 0.0;
        const F64 now = LLTimer::getTotalSeconds();
        if (s_diag && now >= s_next) { s_next = now + 2.0; s_until = now + 0.05; }
        if (s_diag && now <= s_until)
        {
            const glm::mat4 P = get_current_projection(), M = get_current_modelview();
            LL_INFOS("VKHudText") << "\"" << wstring_to_utf8str(wstr.substr(0, 24)) << "\" pos=" << pos_agent
                << " render=" << render_pos << " win=(" << win_coord.x << "," << win_coord.y << "," << win_coord.z
                << ") vp=(" << viewport.x << "," << viewport.y << "," << viewport.z << "," << viewport.w
                << ") scale=" << LLFontGL::sScaleX << "," << LLFontGL::sScaleY
                << " P00=" << P[0][0] << " P11=" << P[1][1] << " P22=" << P[2][2] << " P32=" << P[3][2]
                << " M30..32=" << M[3][0] << "," << M[3][1] << "," << M[3][2]
                << " color.a=" << color.mV[3] << LL_ENDL;
        }
    }

    //fonts all render orthographically, set up projection``
    gGL.matrixMode(LLRender::MM_PROJECTION);
    gGL.pushMatrix();
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.pushMatrix();
    LLUI::pushMatrix();

    gl_state_for_2d(world_view_rect.getWidth(), world_view_rect.getHeight());
    gViewerWindow->setup3DViewport();

    win_coord.x -= world_view_rect.mLeft;
    win_coord.y -= world_view_rect.mBottom;
    LLUI::loadIdentity();
    gGL.loadIdentity();
    // Depth: world text keeps LL's projected depth (in-world occlusion). HUD text is
    // never occluded (LLHUDText), and the HUD projection's depth (~0.02 -> +0.97 here)
    // put it at the far side of the 2D layer's range; draw it at the UI's own depth.
    const F32 text_z = orthographic ? 0.f : -(((F32) win_coord.z*2.f)-1.f);
    LLUI::translate((F32) win_coord.x*1.0f/LLFontGL::sScaleX, (F32) win_coord.y*1.0f/(LLFontGL::sScaleY), text_z);
    F32 right_x;

    font.render(wstr, 0, 0, 1, color, LLFontGL::LEFT, LLFontGL::BASELINE, style, shadow, static_cast<S32>(wstr.length()), 1000, &right_x, /*use_ellipses*/false, /*use_color*/true);

    LLUI::popMatrix();
    gGL.popMatrix();

    gGL.matrixMode(LLRender::MM_PROJECTION);
    gGL.popMatrix();
    gGL.matrixMode(LLRender::MM_MODELVIEW);
}
