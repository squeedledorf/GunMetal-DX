/**
 * @file llviewerjointmesh.h
 * @brief Declaration of LLViewerJointMesh class
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

#ifndef LL_LLVIEWERJOINTMESH_H
#define LL_LLVIEWERJOINTMESH_H

#include "llviewerjoint.h"
#include "llviewertexture.h"
#include "llavatarjointmesh.h"
#include "llpolymesh.h"
#include "v4color.h"

class LLDrawable;
class LLFace;
class LLCharacter;
class LLViewerTexLayerSet;

//-----------------------------------------------------------------------------
// class LLViewerJointMesh
//-----------------------------------------------------------------------------
class LLViewerJointMesh : public LLAvatarJointMesh, public LLViewerJoint
{
public:
    // Constructor
    LLViewerJointMesh();

    // Destructor
    virtual ~LLViewerJointMesh();

    // necessary because MS's compiler warns on function inheritance via dominance in the diamond inheritance here.
    // warns even though LLViewerJoint holds the only non virtual implementation.

    // No updateFaceSizes / updateFaceData: the per-avatar vertex buffer copy of the
    // polymesh is gone under VK (LLVKPreviewRender skins LLPolyMesh directly).
    bool updateLOD(F32 pixel_area, bool activate) override;
    void dump() override;

    bool isAnimatable() const override { return false; }

    // #83 previews (LLVKPreviewRender::drawAvatar): the diffuse texture and colour
    // of this mesh.
    LLGLTexture* getPreviewTexture(bool is_dummy) const;
    LLColor4 getPreviewColor(bool is_dummy) const;
    bool isPreviewVisible() const { return mVisible; }
};

#endif // LL_LLVIEWERJOINTMESH_H
