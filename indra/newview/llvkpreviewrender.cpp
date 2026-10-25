/**
 * @file llvkpreviewrender.cpp
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

#include "llviewerprecompiledheaders.h"

#include "llvkpreviewrender.h"

#include "llrender.h"
#include "llrenderbackend.h"
#include "llgltexture.h"
#include "llpolymesh.h"
#include "llviewercontrol.h"
#include "llviewerjointmesh.h"
#include "llvoavatar.h"
#include "vkgfx/llrendervkbackend.h"
#include "m4math.h"
#include "m3math.h"
#include "v4math.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

using namespace LLAvatarAppearanceDefines;

namespace
{
// Triangles per gGL batch (bounds the temporary arrays; the facade coalesces
// consecutive batches with the same state into one draw anyway).
constexpr U32 kBatchTris = 8192;

glm::vec3 toGlm(const LLVector3& v) { return glm::vec3(v.mV[0], v.mV[1], v.mV[2]); }

// LL's avatar pass alpha-tested the body at LLDrawPoolAvatar::sMinimumAlpha.
constexpr F32 kAvatarMinimumAlpha = 0.2f;
} // namespace

LLVKPreviewRender::LLVKPreviewRender(S32 width, S32 height,
                                     const LLVector3& eye, const LLVector3& look_at, const LLVector3& up,
                                     F32 fov_y, F32 z_near, F32 z_far)
{
    mEye = eye;
    const F32 aspect = (F32)llmax(width, 1) / (F32)llmax(height, 1);
    z_near = llmax(z_near, 0.0001f);
    z_far = llmax(z_far, z_near * 2.f);
    mView = glm::lookAt(toGlm(eye), toGlm(look_at), toGlm(up));
    // GL-convention perspective, built by hand so the clip-space Z convention is
    // explicit (no dependence on GLM_FORCE_DEPTH_ZERO_TO_ONE in this TU).
    const F32 f = 1.f / tanf(fov_y * 0.5f);
    mProj = glm::mat4(0.f);
    mProj[0][0] = f / aspect;
    mProj[1][1] = f;
    mProj[2][2] = (z_far + z_near) / (z_near - z_far);
    mProj[2][3] = -1.f;
    mProj[3][2] = 2.f * z_far * z_near / (z_near - z_far);
    mViewProj = mProj * mView;

    // The facade's matrix convention (its ortho()): clip y negated, clip z in [0,1].
    // Offscreen segments undo the y flip so GL's bottom row lands in image row 0;
    // z [0,1] with near -> 0 is CONVENTIONAL depth (render-target depth clears to 1).
    glm::mat4 to_facade(1.f);
    to_facade[1][1] = -1.f;
    to_facade[2][2] = 0.5f;
    to_facade[3][2] = 0.5f;
    const glm::mat4 proj = to_facade * mProj;

    gGL.matrixMode(LLRender::MM_PROJECTION);
    gGL.pushMatrix();
    gGL.loadMatrix(glm::value_ptr(proj));
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.pushMatrix();
    gGL.loadMatrix(glm::value_ptr(mView));

    mDepth = std::make_unique<LLRender::DepthScope>(true, true, LLDepthFunc::LEQUAL);
    setLights(LIGHTS_WORLD, 0.f);
}

LLVKPreviewRender::~LLVKPreviewRender()
{
    gGL.flush();
    gGL.setMinimumAlpha(0.f);
    mDepth.reset();
    gGL.matrixMode(LLRender::MM_PROJECTION);
    gGL.popMatrix();
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.popMatrix();
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
}

void LLVKPreviewRender::clearDepth()
{
    gGL.flush();
    LLRenderVKBackend::clearTargetDepth();
}

void LLVKPreviewRender::setLights(ELightSpace space, F32 ambient)
{
    mLit = true;
    mAmbient = ambient;
    static const char* kDir[3] = { "PreviewDirection0", "PreviewDirection1", "PreviewDirection2" };
    static const char* kDiff[3] = { "PreviewDiffuse0", "PreviewDiffuse1", "PreviewDiffuse2" };
    // Eye -> world: the transpose of the view's rotation.
    const glm::mat3 eye_to_world = glm::transpose(glm::mat3(mView));
    for (S32 i = 0; i < 3; ++i)
    {
        LLVector3 d = gSavedSettings.getVector3(kDir[i]);
        d.normVec();
        if (space == LIGHTS_EYE)
        {
            const glm::vec3 w = eye_to_world * toGlm(d);
            d.set(w.x, w.y, w.z);
            d.normVec();
        }
        mLightDir[i] = d;
        mLightDiffuse[i] = gSavedSettings.getColor4(kDiff[i]);
    }
}

void LLVKPreviewRender::setSingleLight(const LLVector3& dir_world, const LLColor4& diffuse, F32 ambient)
{
    mLit = true;
    mAmbient = ambient;
    mLightDir[0] = dir_world;
    mLightDir[0].normVec();
    mLightDiffuse[0] = diffuse;
    for (S32 i = 1; i < 3; ++i)
    {
        mLightDir[i].setVec(0.f, 0.f, 1.f);
        mLightDiffuse[i].setVec(0.f, 0.f, 0.f, 0.f);
    }
}

void LLVKPreviewRender::lightVertex(const LLVector4a& n_world, const LLColor4& color, LLColor4U& out,
                                    bool has_normal) const
{
    LLColor4 c = color;
    if (mLit && has_normal)
    {
        const LLVector3 n(n_world.getF32ptr());
        F32 r = mAmbient, g = mAmbient, b = mAmbient;
        for (S32 i = 0; i < 3; ++i)
        {
            const F32 k = llmax(n * mLightDir[i], 0.f);
            r += mLightDiffuse[i].mV[0] * k;
            g += mLightDiffuse[i].mV[1] * k;
            b += mLightDiffuse[i].mV[2] * k;
        }
        c.mV[0] *= llclamp(r, 0.f, 1.f);
        c.mV[1] *= llclamp(g, 0.f, 1.f);
        c.mV[2] *= llclamp(b, 0.f, 1.f);
    }
    out.set((U8)llclamp(ll_round(c.mV[0] * 255.f), 0, 255),
            (U8)llclamp(ll_round(c.mV[1] * 255.f), 0, 255),
            (U8)llclamp(ll_round(c.mV[2] * 255.f), 0, 255),
            (U8)llclamp(ll_round(c.mV[3] * 255.f), 0, 255));
}

void LLVKPreviewRender::drawTriangles(const Mesh& mesh, LLGLTexture* tex, const LLColor4& color, bool cull_back)
{
    if (!mesh.pos || mesh.vert_count == 0) return;
    const U32 index_count = (mesh.idx16 || mesh.idx32) ? mesh.index_count : mesh.vert_count;
    if (index_count < 3) return;

    // Per-vertex world position, lit colour and clip position.
    std::vector<LLVector4a> wpos(mesh.vert_count);
    std::vector<LLColor4U> col(mesh.vert_count);
    std::vector<glm::vec4> clip(mesh.vert_count);
    for (U32 v = 0; v < mesh.vert_count; ++v)
    {
        LLVector4a p = mesh.pos[v];
        if (mesh.model) mesh.model->affineTransform(mesh.pos[v], p);
        wpos[v] = p;
        clip[v] = mViewProj * glm::vec4(p[0], p[1], p[2], 1.f);
        LLVector4a n;
        if (mesh.norm)
        {
            n = mesh.norm[v];
            if (mesh.model) mesh.model->rotate(mesh.norm[v], n);
            n.normalize3fast();
        }
        else
        {
            n.set(0.f, 0.f, 1.f);
        }
        lightVertex(n, color, col[v], mesh.norm != nullptr);
    }

    if (tex)
    {
        gGL.getTexUnit(0)->bind(tex);
        // Mesh / material UVs tile (LL textures default to wrap); the UI default is clamp.
        LLRender::sBackend->setTextureAddressMode(0u);
    }
    else
    {
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    }

    std::vector<LLVector4a> bv;
    std::vector<LLVector2> buv;
    std::vector<LLColor4U> bcol;
    bv.reserve(kBatchTris * 3); buv.reserve(kBatchTris * 3); bcol.reserve(kBatchTris * 3);
    auto flush_batch = [&]()
    {
        if (bv.empty()) return;
        gGL.begin(LLRender::TRIANGLES);
        gGL.vertexBatchPreTransformed(bv.data(), buv.data(), bcol.data(), (S32)bv.size());
        gGL.end();
        bv.clear(); buv.clear(); bcol.clear();
    };
    auto index_at = [&](U32 i) -> U32
    {
        return mesh.idx16 ? (U32)mesh.idx16[i] : mesh.idx32 ? mesh.idx32[i] : i;
    };
    const LLVector2 zero_uv(0.f, 0.f);
    for (U32 i = 0; i + 2 < index_count; i += 3)
    {
        const U32 a = index_at(i), b = index_at(i + 1), c = index_at(i + 2);
        if (a >= mesh.vert_count || b >= mesh.vert_count || c >= mesh.vert_count) continue;
        if (cull_back && clip[a].w > 0.f && clip[b].w > 0.f && clip[c].w > 0.f)
        {
            // Signed area in NDC, GL convention (counter-clockwise = front).
            const glm::vec2 pa(clip[a].x / clip[a].w, clip[a].y / clip[a].w);
            const glm::vec2 pb(clip[b].x / clip[b].w, clip[b].y / clip[b].w);
            const glm::vec2 pc(clip[c].x / clip[c].w, clip[c].y / clip[c].w);
            const F32 area = (pb.x - pa.x) * (pc.y - pa.y) - (pc.x - pa.x) * (pb.y - pa.y);
            if (area < 0.f) continue;
        }
        for (U32 k : { a, b, c })
        {
            bv.push_back(wpos[k]);
            buv.push_back(mesh.uv ? mesh.uv[k] : zero_uv);
            bcol.push_back(col[k]);
        }
        if (bv.size() >= kBatchTris * 3) flush_batch();
    }
    flush_batch();
}

void LLVKPreviewRender::drawEdges(const Mesh& mesh, const LLColor4& color, F32 line_width)
{
    if (!mesh.pos || mesh.vert_count == 0) return;
    const U32 index_count = (mesh.idx16 || mesh.idx32) ? mesh.index_count : mesh.vert_count;
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gGL.setLineWidth(line_width);
    gGL.color4fv(color.mV);
    auto index_at = [&](U32 i) -> U32
    {
        return mesh.idx16 ? (U32)mesh.idx16[i] : mesh.idx32 ? mesh.idx32[i] : i;
    };
    std::vector<LLVector4a> wpos(mesh.vert_count);
    LLVector4a eye;
    eye.load3(mEye.mV);
    for (U32 v = 0; v < mesh.vert_count; ++v)
    {
        wpos[v] = mesh.pos[v];
        if (mesh.model) mesh.model->affineTransform(mesh.pos[v], wpos[v]);
        LLVector4a to_eye;
        to_eye.setSub(eye, wpos[v]);
        to_eye.mul(0.002f);
        wpos[v].add(to_eye);
    }
    gGL.begin(LLRender::LINES);
    for (U32 i = 0; i + 2 < index_count; i += 3)
    {
        const U32 t[3] = { index_at(i), index_at(i + 1), index_at(i + 2) };
        if (t[0] >= mesh.vert_count || t[1] >= mesh.vert_count || t[2] >= mesh.vert_count) continue;
        for (S32 e = 0; e < 3; ++e)
        {
            const LLVector4a& p0 = wpos[t[e]];
            const LLVector4a& p1 = wpos[t[(e + 1) % 3]];
            gGL.vertex3f(p0[0], p0[1], p0[2]);
            gGL.vertex3f(p1[0], p1[1], p1[2]);
        }
    }
    gGL.end();
    gGL.setLineWidth(1.f);
}

// static
bool LLVKPreviewRender::skinJointMesh(LLViewerJointMesh* jm, std::vector<LLVector4a>& pos,
                                      std::vector<LLVector4a>& norm)
{
    LLPolyMesh* mesh = jm ? jm->getMesh() : nullptr;
    if (!mesh) return false;
    const U32 nv = (U32)mesh->getNumVertices();
    if (nv == 0 || mesh->getNumFaces() == 0) return false;
    const LLVector4a* coords = mesh->getCoords();
    const LLVector4a* normals = mesh->getNormals();
    if (!coords || !normals) return false;
    pos.resize(nv);
    norm.resize(nv);

    if (!mesh->hasWeights())
    {
        // Rigid (eyeballs): the mesh joint's world matrix.
        LLMatrix4a m;
        m.loadu(jm->getWorldMatrix());
        for (U32 i = 0; i < nv; ++i)
        {
            m.affineTransform(coords[i], pos[i]);
            m.rotate(normals[i], norm[i]);
        }
        return true;
    }

    // The software-skinning palette (LL's former LLViewerJointMesh::uploadJointMatrices):
    // each skin joint's world matrix, translated by its (rotated) skin pivot.
    LLPolyMesh* ref = mesh->getReferenceMesh();
    const size_t nj = ref->mJointRenderData.size();
    if (nj == 0) return false;
    std::vector<LLMatrix4> mats(nj);
    std::vector<LLMatrix3> rots(nj);
    for (size_t j = 0; j < nj; ++j)
    {
        mats[j] = *ref->mJointRenderData[j]->mWorldMatrix;
        rots[j] = mats[j].getMat3();
    }
    std::vector<LLVector4> pivots;
    pivots.reserve(nj * 2);
    bool last_pivot_uploaded = false;
    for (size_t j = 0; j < nj; ++j)
    {
        LLSkinJoint* sj = ref->mJointRenderData[j]->mSkinJoint;
        if (sj)
        {
            if (!last_pivot_uploaded)
            {
                LLVector4 parent_pivot(sj->mRootToParentJointSkinOffset);
                parent_pivot.mV[VW] = 0.f;
                pivots.push_back(parent_pivot);
            }
            LLVector4 child_pivot(sj->mRootToJointSkinOffset);
            child_pivot.mV[VW] = 0.f;
            pivots.push_back(child_pivot);
            last_pivot_uploaded = true;
        }
        else
        {
            last_pivot_uploaded = false;
        }
    }
    const size_t np = llmin(pivots.size(), nj);
    for (size_t i = 0; i < np; ++i)
    {
        LLVector3 pivot(pivots[i]);
        pivot = pivot * rots[i];
        mats[i].translate(pivot);
    }
    std::vector<LLMatrix4a> palette(nj);
    for (size_t j = 0; j < nj; ++j) palette[j].loadu(mats[j]);

    const F32* weights = mesh->getWeights();
    for (U32 i = 0; i < nv; ++i)
    {
        const F32 wv = weights ? weights[i] : 0.f;
        S32 joint = llclamp((S32)wv, 0, (S32)nj - 1);
        const F32 w = wv - (F32)(S32)wv;
        if (w != 0.f && joint + 1 < (S32)nj)
        {
            LLMatrix4a blend;
            blend.setLerp(palette[joint], palette[joint + 1], w);
            blend.affineTransform(coords[i], pos[i]);
            blend.rotate(normals[i], norm[i]);
        }
        else
        {
            palette[joint].affineTransform(coords[i], pos[i]);
            palette[joint].rotate(normals[i], norm[i]);
        }
    }
    return true;
}

void LLVKPreviewRender::drawAvatar(LLVOAvatar* avatar, LLViewerJointMesh* override_mesh, LLGLTexture* override_tex,
                                   bool opaque)
{
    if (!avatar || avatar->isDead()) return;
    // The skin palette reads the joints' world matrices: bring them up to date with
    // the pose / visual params applied just before this render.
    if (avatar->mRoot) avatar->mRoot->updateWorldMatrixChildren();
    const bool is_dummy = avatar->mIsDummy;
    const bool ui = avatar->isUIAvatar();

    // Mesh selection as LL's (removed) LLVOAvatar::renderSkinned / renderTransparent / renderRigid
    // (the head is always drawn: previews are never first person).
    struct Part { S32 id; bool draw; };
    const bool skirt = avatar->isWearingWearableType(LLWearableType::WT_SKIRT)
                       && (ui || avatar->isTextureVisible(TEX_SKIRT_BAKED));
    const Part parts[] = {
        { MESH_ID_HEAD,          ui || avatar->isTextureVisible(TEX_HEAD_BAKED) },
        { MESH_ID_UPPER_BODY,    ui || avatar->isTextureVisible(TEX_UPPER_BAKED) },
        { MESH_ID_LOWER_BODY,    ui || avatar->isTextureVisible(TEX_LOWER_BAKED) },
        { MESH_ID_EYEBALL_LEFT,  ui || avatar->isTextureVisible(TEX_EYES_BAKED) },
        { MESH_ID_EYEBALL_RIGHT, ui || avatar->isTextureVisible(TEX_EYES_BAKED) },
        { MESH_ID_SKIRT,         skirt },
        { MESH_ID_EYELASH,       avatar->isTextureVisible(TEX_HEAD_BAKED) },
        { MESH_ID_HAIR,          (ui && is_dummy) || avatar->isTextureVisible(TEX_HAIR_BAKED) },
    };

    if (opaque)
    {
        gGL.setSceneBlendType(LLRender::BT_REPLACE);
    }
    else
    {
        gGL.setMinimumAlpha(kAvatarMinimumAlpha);
    }
    std::vector<LLVector4a> pos, norm;
    std::vector<U32> idx;
    for (const Part& part : parts)
    {
        if (!part.draw) continue;
        LLViewerJoint* lod_joint = avatar->getViewerJoint(part.id);
        if (!lod_joint || lod_joint->mMeshParts.empty()) continue;
        // Highest detail: previews are close-ups.
        LLViewerJointMesh* jm = dynamic_cast<LLViewerJointMesh*>(lod_joint->mMeshParts.front());
        if (!jm || !jm->isPreviewVisible()) continue;
        if (!skinJointMesh(jm, pos, norm)) continue;

        LLPolyMesh* mesh = jm->getMesh();
        const S32 nf = mesh->getNumFaces();
        const LLPolyFace* faces = mesh->getFaces();
        idx.resize((size_t)nf * 3);
        for (S32 f = 0; f < nf; ++f)
        {
            idx[f * 3 + 0] = (U32)faces[f][0];
            idx[f * 3 + 1] = (U32)faces[f][1];
            idx[f * 3 + 2] = (U32)faces[f][2];
        }

        LLGLTexture* tex = nullptr;
        LLColor4 color;
        if (jm == override_mesh && override_tex)
        {
            tex = override_tex;
            // LL's upload test-image colours.
            color = jm->isTransparent() ? LLColor4::white : LLColor4(0.7f, 0.6f, 0.3f, 1.f);
        }
        else
        {
            tex = jm->getPreviewTexture(is_dummy);
            color = jm->getPreviewColor(is_dummy);
        }

        Mesh m;
        m.pos = pos.data();
        m.norm = norm.data();
        m.uv = mesh->getTexCoords();
        m.vert_count = (U32)pos.size();
        m.idx32 = idx.data();
        m.index_count = (U32)idx.size();
        // Hair / skirt are two-sided (LL drew them with culling off).
        drawTriangles(m, tex, color, !jm->isTransparent());
    }
    gGL.flush();
    gGL.setMinimumAlpha(0.f);
    if (opaque) gGL.setSceneBlendType(LLRender::BT_ALPHA);
}
