/**
 * @file llvkpreviewrender.h
 * @brief #83: shared 3D drawing for LLViewerDynamicTexture previews under Vulkan.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Linden Research, Inc.
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
 * $/LicenseInfo$
 */

#ifndef LL_LLVKPREVIEWRENDER_H
#define LL_LLVKPREVIEWRENDER_H

#include "llwin32headers.h"
#include "llgl.h"
#include "llrender.h"   // LLRender::DepthScope
#include "v3math.h"
#include "v4color.h"
#include "v4coloru.h"
#include "v2math.h"
#include "llvector4a.h"
#include "llmatrix4a.h"

#include <glm/glm.hpp>
#include <memory>
#include <vector>

class LLGLTexture;
class LLVOAvatar;
class LLViewerJointMesh;

// Under Vulkan, a preview's render() runs inside its render target's offscreen
// segment (LLViewerDynamicTexture::renderVK) and draws through gGL / the facade.
// The facade has no vertex buffers, shaders or lights of LL's kind, so 3D previews
// hand it world-space triangles with the lighting already evaluated per vertex --
// exactly what LL's preview shader (previewV.glsl) does on the GPU: up to three
// directional lights (PreviewDiffuse0-2 / PreviewDirection0-2), Gouraud, times the
// material colour, times the diffuse texture.
//
// Depth is the render target's own CONVENTIONAL depth (cleared to 1.0, LEQUAL);
// the main view's reversed-Z does not apply here. Back faces are culled on the CPU
// (LL enabled GL_CULL_FACE for these previews; the facade pipelines don't cull).
//
// Construct with the camera; the destructor restores gGL's matrices / depth state.
class LLVKPreviewRender
{
public:
    LLVKPreviewRender(S32 width, S32 height,
                      const LLVector3& eye, const LLVector3& look_at, const LLVector3& up,
                      F32 fov_y, F32 z_near, F32 z_far);
    ~LLVKPreviewRender();

    // Where PreviewDirectionN live. LL lit avatar / sculpt previews after the
    // camera was loaded (directions are world space) and the mesh-upload preview
    // before (eye space, i.e. they follow the camera).
    enum ELightSpace { LIGHTS_WORLD, LIGHTS_EYE };
    void setLights(ELightSpace space, F32 ambient);
    // One white-ish directional light (world space) plus ambient.
    void setSingleLight(const LLVector3& dir_world, const LLColor4& diffuse, F32 ambient);
    // Unlit: vertex colour = material colour.
    void setUnlit() { mLit = false; }
    // glClear(GL_DEPTH_BUFFER_BIT) inside the render target.
    void clearDepth();

    struct Mesh
    {
        const LLVector4a* pos = nullptr;     // positions
        const LLVector4a* norm = nullptr;    // normals (may be null: unlit)
        const LLVector2*  uv = nullptr;      // texcoords (may be null)
        U32 vert_count = 0;
        const U16* idx16 = nullptr;          // one of idx16 / idx32 / (none = sequential)
        const U32* idx32 = nullptr;
        U32 index_count = 0;
        const LLMatrix4a* model = nullptr;   // object -> world (null = identity)
    };
    // Draw a triangle list with `tex` (null = untextured) modulated by `color`.
    void drawTriangles(const Mesh& mesh, LLGLTexture* tex, const LLColor4& color, bool cull_back = true);
    // Its edges as lines (the mesh-upload preview's "show edges" / physics outline),
    // pulled a hair toward the camera so they win the depth test against their faces
    // (GL drew them with glPolygonMode(GL_LINE) at the faces' own depth).
    void drawEdges(const Mesh& mesh, const LLColor4& color, F32 line_width);

    // The system-avatar body (LLPolyMesh skin, CPU skinned with the current pose)
    // the way LL's (removed) LLVOAvatar::renderSkinned + renderRigid picked its meshes: highest LOD,
    // meshes that are visible, layer-set composite / baked / default textures and
    // the avatar alpha test. `override_mesh` (if set) draws with `override_tex`
    // instead (the image-upload "preview on avatar" modes).
    // `opaque`: no alpha test, no blending (the upload preview shows the material
    // colour where the image is transparent, as LL's GL preview did).
    void drawAvatar(LLVOAvatar* avatar, LLViewerJointMesh* override_mesh = nullptr,
                    LLGLTexture* override_tex = nullptr, bool opaque = false);

    const glm::mat4& viewMatrix() const { return mView; }

    // CPU skinning of one avatar mesh into world space (exposed for picking-free
    // callers; returns false when the mesh has no geometry).
    static bool skinJointMesh(LLViewerJointMesh* jm, std::vector<LLVector4a>& pos,
                              std::vector<LLVector4a>& norm);

private:
    void lightVertex(const LLVector4a& n_world, const LLColor4& color, LLColor4U& out, bool has_normal) const;

    LLVector3 mEye;
    glm::mat4 mView;
    glm::mat4 mProj;          // GL clip convention (for CPU culling)
    glm::mat4 mViewProj;
    bool mLit = true;
    F32 mAmbient = 0.f;
    LLVector3 mLightDir[3];   // world space, normalized
    LLColor4 mLightDiffuse[3];
    std::unique_ptr<LLRender::DepthScope> mDepth;
};

#endif // LL_LLVKPREVIEWRENDER_H
