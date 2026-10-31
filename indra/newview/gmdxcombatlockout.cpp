/**
 * @file gmdxcombatlockout.cpp
 * @brief Debug views that see through walls stay off while the player aims, and for 5 s after.
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

// <GMDX> See gmdxcombatlockout.h. From Alchemy: Skooma (Nexii Malthus), LGPL 2.1 (skcombatlockout.cpp).

#include "llviewerprecompiledheaders.h"

#include "gmdxcombatlockout.h"

#include "llagentcamera.h"
#include "llframetimer.h"
#include "pipeline.h"
#include "sltpscamera.h"

namespace
{
    // Long enough that flicking out of aim and back gains nothing, short enough not to slow down
    // building (Skooma's TAIL_SECONDS).
    constexpr F64 TAIL_SECONDS = 5.0;

    bool sActive = false;
    F64 sLastAimSeconds = -1.0e9;

    bool aiming()
    {
        return gAgentCamera.cameraMouselook()       // first person and the OTS shoulder camera, ADS included
            || SLTPSCamera::isEngagedNow();         // the shooter camera behind the avatar
    }
}

void GMDXCombatLockout::update()
{
    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (aiming())
    {
        sLastAimSeconds = now;
    }
    sActive = now - sLastAimSeconds < TAIL_SECONDS;
}

bool GMDXCombatLockout::active()
{
    return sActive || aiming();   // (aiming that began after this frame's update(), e.g. a forced mouselook)
}

bool GMDXCombatLockout::keepsRenderType(U32 type)
{
    if (!active())
    {
        return false;
    }
    // Skooma's LOCKED_RENDER_TYPES: any of them off is walls off with avatars still drawn. Avatars,
    // particles, sky and glow stay the user's to hide: hiding them only hurts the one who does.
    switch (type)
    {
    case LLPipeline::RENDER_TYPE_SIMPLE:
    case LLPipeline::RENDER_TYPE_MATERIALS:
    case LLPipeline::RENDER_TYPE_ALPHA:
    case LLPipeline::RENDER_TYPE_ALPHA_MASK:
    case LLPipeline::RENDER_TYPE_FULLBRIGHT_ALPHA_MASK:
    case LLPipeline::RENDER_TYPE_FULLBRIGHT:
    case LLPipeline::RENDER_TYPE_TREE:
    case LLPipeline::RENDER_TYPE_TERRAIN:
    case LLPipeline::RENDER_TYPE_WATER:
    case LLPipeline::RENDER_TYPE_VOIDWATER:
    case LLPipeline::RENDER_TYPE_VOLUME:
    case LLPipeline::RENDER_TYPE_GRASS:
    case LLPipeline::RENDER_TYPE_BUMP:
    case LLPipeline::RENDER_TYPE_GLTF_PBR:
        return true;
    default:
        return false;
    }
}
