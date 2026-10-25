 /**
 * @file llviewerjointmesh.cpp
 * @brief Implementation of LLViewerJointMesh class
 *
 * $LicenseInfo:firstyear=2001&license=viewerlgpl$
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

//-----------------------------------------------------------------------------
// Header Files
//-----------------------------------------------------------------------------
#include "llviewerprecompiledheaders.h"

#include "llfasttimer.h"
#include "llrender.h"

#include "llapr.h"
#include "llbox.h"
#include "lldrawable.h"
#include "lldynamictexture.h"
#include "llface.h"
#include "llviewertexlayer.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewertexturelist.h"
#include "llviewerjointmesh.h"
#include "llvoavatar.h"
#include "pipeline.h"
#include "llmath.h"
#include "v4math.h"
#include "m3math.h"
#include "m4math.h"
#include "llmatrix4a.h"
#include "llperfstats.h"

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
// LLViewerJointMesh
//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------


//-----------------------------------------------------------------------------
// LLViewerJointMesh()
//-----------------------------------------------------------------------------
LLViewerJointMesh::LLViewerJointMesh()
    :
    LLAvatarJointMesh()
{
}


//-----------------------------------------------------------------------------
// ~LLViewerJointMesh()
// Class Destructor
//-----------------------------------------------------------------------------
LLViewerJointMesh::~LLViewerJointMesh()
{
}

const S32 NUM_AXES = 3;

// register layoud
// rotation X 0-n
// rotation Y 0-n
// rotation Z 0-n
// pivot parent 0-n -- child = n+1

//-----------------------------------------------------------------------------
// #83 Vulkan previews
//-----------------------------------------------------------------------------
LLGLTexture* LLViewerJointMesh::getPreviewTexture(bool is_dummy) const
{
    LLViewerTexLayerSet* layerset = dynamic_cast<LLViewerTexLayerSet*>(mLayerSet);
    if (!is_dummy && layerset)
    {
        if (layerset->hasComposite())
        {
            return layerset->getViewerComposite();
        }
        return LLViewerTextureManager::getFetchedTexture(IMG_DEFAULT);
    }
    if (!is_dummy && mTexture.notNull())
    {
        return mTexture.get();
    }
    return LLViewerTextureManager::getFetchedTexture(IMG_DEFAULT);
}

LLColor4 LLViewerJointMesh::getPreviewColor(bool is_dummy) const
{
    return is_dummy ? LLVOAvatar::getDummyColor() : mColor;
}

//-----------------------------------------------------------------------------
// updateLOD()
//-----------------------------------------------------------------------------
bool LLViewerJointMesh::updateLOD(F32 pixel_area, bool activate)
{
    bool valid = mValid;
    setValid(activate, true);
    return (valid != activate);
}

void LLViewerJointMesh::dump()
{
    if (mValid)
    {
        LL_INFOS() << "Usable LOD " << mName << LL_ENDL;
    }
}

// End
