/**
 * @file llcleangltfingest.cpp
 * @brief Clean-pipeline (Vulkan) ingest of glTF scene assets (#39). See the header.
 *
 * WHAT IS RENDERED
 *   Every node reachable from the asset's scenes that has a mesh: one clean instance per
 *   (node, primitive). Geometry: triangle lists (strips / fans converted at capture),
 *   positions + normals + MikkTSpace tangents + TEXCOORD_0 (V flipped into SL's
 *   convention, as LL's GL upload did). Material: base colour / normal / metallic-
 *   roughness / emissive textures + factors, alpha mode (OPAQUE / MASK + cutoff / BLEND),
 *   double-sided, KHR_materials_unlit, per-map KHR_texture_transform. Pose: object pose
 *   (world, worn attachment or HUD) * node asset matrix; rigid node ANIMATION re-poses
 *   the instances each frame (dirty-range instance update, the mover pattern).
 *
 * DEFERRED (documented TODOs, render static / approximate):
 *   - SKINNING: a primitive whose node has a skin (or that carries JOINTS/WEIGHTS)
 *     renders STATIC in its bind pose, in asset space (glTF ignores a skinned mesh
 *     node's own transform). TODO(#39): route through the GPU pre-skin path -- a
 *     per-asset palette of jointAsset * inverseBind in the skin store, weights in the
 *     slot mirror (MirrorWeight), flag bit0 like rigged mesh.
 *   - Morph targets (not parsed by LL either), TEXCOORD_1 (one UV lane: maps that ask
 *     for set 1 sample set 0), COLOR_0 (MirrorVertex has no colour; the base-colour
 *     FACTOR goes through the instance tint), occlusion maps (no material slot),
 *     POINTS / LINES primitives (skipped), samplers (wrap / filter defaults).
 *   - A node with a negative-determinant matrix (mirrored) flips winding: it is drawn
 *     double-sided so it stays visible, but its lighting faces inward.
 */

#include "llviewerprecompiledheaders.h"

#include "llcleangltfingest.h"

#include "gltfscenemanager.h"
#include "gltf/asset.h"
#include "gltf/primitive.h"

#include "llagent.h"
#include "llcleaningest.h"
#include "llcleaninstancestore.h"
#include "llcleanmaterialstore.h"
#include "llcleanmeshasset.h"
#include "llcleanskinstore.h"
#include "lltimer.h"
#include "llviewerjointattachment.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewertexture.h"
#include "llvoavatar.h"
#include "llvoavatarself.h"
#include "llvovolume.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace LL::GLTF;

namespace
{
// The single LOD level glTF primitives are captured + published at. The cull's
// residency fallback draws the highest resident level for every requested level, and
// the GC unpublishes exactly slot[lod] of the captured lod -- so ONE level is correct.
constexpr S32 kGLTFLod = LLCleanMeshAsset::NUM_LOD - 1;

constexpr U32 FLAG_BLEND     = 2u;
constexpr U32 FLAG_MASK      = 4u;
constexpr U32 FLAG_HUD       = 8u;
constexpr U32 FLAG_FULLBRIGHT= 16u;
constexpr U32 FLAG_DSIDED    = 256u;
constexpr U32 FLAG_ATTACH    = 512u;
constexpr U32 FLAG_UNLIT     = 4096u;
constexpr U32 FLAG_SELF_PRIO = 8192u;

U64 mix64(U64 h, U64 v) { return h ^ (v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2)); }
U64 mixf(U64 h, F32 f) { U32 u; std::memcpy(&u, &f, 4); return mix64(h, u); }

// Texture UUID of a glTF texture slot (null when absent / not resolved).
LLUUID textureId(const Asset& asset, const TextureInfo& info)
{
    if (info.mIndex < 0 || info.mIndex >= (S32)asset.mTextures.size()) return LLUUID::null;
    const S32 src = asset.mTextures[info.mIndex].mSource;
    if (src < 0 || src >= (S32)asset.mImages.size()) return LLUUID::null;
    const Image& img = asset.mImages[src];
    return img.mTexture.notNull() ? img.mTexture->getID() : LLUUID::null;
}

// KHR_texture_transform as the affine {a,b,c,d,e,f} the clean shaders apply to
// SL-convention UVs: uv' = flip(offset + R * (S * flip(uv))), flip(v) = 1 - v -- the
// same formula registerCleanFace uses for PBR prim faces (cleanFillMapUV gltf6).
void gltfUV6(const TextureTransform& tt, F32 o[6])
{
    const F32 c = cosf(tt.mRotation), sn = sinf(tt.mRotation);
    const F32 sx = tt.mScale.x, sy = tt.mScale.y;
    const F32 ox = tt.mOffset.x, oy = tt.mOffset.y;
    o[0] = c * sx;  o[1] = -sn * sy; o[2] = sn * sy + ox;
    o[3] = sn * sx; o[4] = c * sy;   o[5] = 1.f - c * sy - oy;
}

U32 materialFor(const Asset& asset, S32 mat_idx, U32& flags_out, F32 tint_out[4], F32 uv_out[16],
                bool& texcoord1_out)
{
    LLCleanMaterialStore::MaterialDesc d;
    d.gltf = true;
    flags_out = 0;
    tint_out[0] = tint_out[1] = tint_out[2] = tint_out[3] = 1.f;
    std::memset(uv_out, 0, sizeof(F32) * 16);
    uv_out[0] = uv_out[5] = uv_out[10] = uv_out[15] = 1.f;
    texcoord1_out = false;
    if (mat_idx < 0 || mat_idx >= (S32)asset.mMaterials.size())
    {
        // glTF default material: white, metallic 1, roughness 1, opaque, single-sided.
        d.metallic_factor = 1.f;
        d.roughness_factor = 1.f;
        return LLCleanMaterialStore::instance().getOrCreate(d);
    }
    const Material& m = asset.mMaterials[mat_idx];
    const Material::PbrMetallicRoughness& pbr = m.mPbrMetallicRoughness;
    d.base_color_id = textureId(asset, pbr.mBaseColorTexture);
    d.normal_id     = textureId(asset, m.mNormalTexture);
    d.mr_id         = textureId(asset, pbr.mMetallicRoughnessTexture);
    d.emissive_id   = textureId(asset, m.mEmissiveTexture);
    // base_color_factor stays NEUTRAL: the colour goes through the instance tint (the
    // FS multiplies base by both -- setting both would square it, see registerCleanFace).
    for (int c = 0; c < 4; ++c) tint_out[c] = pbr.mBaseColorFactor[c];
    d.emissive_factor[0] = m.mEmissiveFactor.x;
    d.emissive_factor[1] = m.mEmissiveFactor.y;
    d.emissive_factor[2] = m.mEmissiveFactor.z;
    d.metallic_factor  = pbr.mMetallicFactor;
    d.roughness_factor = pbr.mRoughnessFactor;
    if (m.mAlphaMode == Material::AlphaMode::MASK)
    {
        d.alpha_cutoff = m.mAlphaCutoff;
        flags_out |= FLAG_MASK;
    }
    else if (m.mAlphaMode == Material::AlphaMode::BLEND)
    {
        d.alpha_blend = true;
        flags_out |= FLAG_BLEND;   // as authored -- no opaque-texture demotion for glTF
    }
    if (m.mDoubleSided) flags_out |= FLAG_DSIDED;
    if (m.mUnlit.mPresent)
    {
        flags_out |= FLAG_UNLIT;
        if (!(flags_out & FLAG_BLEND)) flags_out |= FLAG_FULLBRIGHT;
    }
    // Base-colour transform -> instance uv_matrix (column-major, LLMatrix4 layout);
    // normal / MR / emissive keep their own transforms (material UV rows).
    {
        F32 b[6];
        gltfUV6(pbr.mBaseColorTexture.mTextureTransform, b);
        uv_out[0] = b[0]; uv_out[4] = b[1]; uv_out[12] = b[2];
        uv_out[1] = b[3]; uv_out[5] = b[4]; uv_out[13] = b[5];
    }
    gltfUV6(m.mNormalTexture.mTextureTransform, d.uv_xform[0]);
    gltfUV6(pbr.mMetallicRoughnessTexture.mTextureTransform, d.uv_xform[1]);
    gltfUV6(m.mEmissiveTexture.mTextureTransform, d.uv_xform[2]);
    d.uv_own[0] = d.uv_own[1] = d.uv_own[2] = true;
    // TODO(#39): one UV lane -- maps asking for TEXCOORD_1 sample TEXCOORD_0.
    texcoord1_out = m.isMultiUV();
    return LLCleanMaterialStore::instance().getOrCreate(d);
}

// Triangle-list indices for a primitive; false for points / lines (not drawable here).
bool triangleList(const Primitive& p, std::vector<U32>& out)
{
    const std::vector<U32>& in = p.mIndexArray;
    out.clear();
    switch (p.mMode)
    {
    case Primitive::Mode::TRIANGLES:
        out.assign(in.begin(), in.begin() + (in.size() / 3) * 3);
        return true;
    case Primitive::Mode::TRIANGLE_STRIP:
        for (size_t i = 2; i < in.size(); ++i)
        {
            // keep the winding consistent: odd triangles swap their first two vertices
            if (i & 1) out.insert(out.end(), { in[i - 1], in[i - 2], in[i] });
            else       out.insert(out.end(), { in[i - 2], in[i - 1], in[i] });
        }
        return true;
    case Primitive::Mode::TRIANGLE_FAN:
        for (size_t i = 2; i < in.size(); ++i) out.insert(out.end(), { in[0], in[i - 1], in[i] });
        return true;
    default:
        return false;
    }
}

bool assetPrepared(const LLViewerObject* obj)
{
    // GLTFSceneManager lists an object only once Asset::prep() succeeded (buffers in,
    // primitives decoded); obj->mGLTFAsset is set earlier, while buffers still stream.
    for (const auto& o : LL::GLTFSceneManager::instance().mObjects)
        if (o.get() == obj) return true;
    return false;
}

void collectSceneNodes(const Asset& asset, std::vector<S32>& out)
{
    std::vector<U8> seen(asset.mNodes.size(), 0);
    std::vector<S32> stack;
    for (const Scene& sc : asset.mScenes)
        for (S32 n : sc.mNodes) stack.push_back(n);
    while (!stack.empty())
    {
        const S32 n = stack.back();
        stack.pop_back();
        if (n < 0 || n >= (S32)asset.mNodes.size() || seen[n]) continue;
        seen[n] = 1;
        out.push_back(n);
        for (S32 c : asset.mNodes[n].mChildren) stack.push_back(c);
    }
    std::sort(out.begin(), out.end());   // stable instance order (node index)
}

bool isSkinnedPrim(const Node& node, const Primitive& p)
{
    return node.mSkin != INVALID_INDEX || !p.mWeights.empty();
}

void nodeLocal(const Node& node, bool skinned, F32 out[16])
{
    if (skinned)
    {
        // glTF: a skinned mesh's node transform is ignored; bind-pose vertices are in
        // asset space. TODO(#39): skinning (see the file header).
        std::memset(out, 0, sizeof(F32) * 16);
        out[0] = out[5] = out[10] = out[15] = 1.f;
        return;
    }
    std::memcpy(out, glm::value_ptr(node.mAssetMatrix), sizeof(F32) * 16);
}

F32 det3(const F32 m[16])
{
    return m[0] * (m[5] * m[10] - m[9] * m[6])
         - m[4] * (m[1] * m[10] - m[9] * m[2])
         + m[8] * (m[1] * m[6] - m[5] * m[2]);
}
} // namespace

LLCleanGLTFIngest& LLCleanGLTFIngest::instance()
{
    static LLCleanGLTFIngest s;
    return s;
}

bool LLCleanGLTFIngest::isGLTFScene(const LLViewerObject* obj)
{
    if (!obj || obj->getPCode() != LL_PCODE_VOLUME) return false;
    if (obj->mGLTFAsset) return true;
    const LLVolume* v = const_cast<LLViewerObject*>(obj)->getVolume();
    return v && (v->getParams().getSculptType() & LL_SCULPT_TYPE_MASK) == LL_SCULPT_TYPE_GLTF;
}

// Projector-shadow tile invalidation (the VK renderer consumes these spheres; see
// llcleaningest.cpp noteChangedSphere -- same bounded protocol).
extern std::vector<float> gVKChangedSpheres;
extern std::vector<uint8_t> gVKChangedSphereTags;
extern bool gVKChangedOverflow;
extern bool gVKProjShadowsUsed;

void LLCleanGLTFIngest::noteChanged(LLViewerObject* obj, const ObjRec& rec)
{
    if (gVKChangedOverflow || !gVKProjShadowsUsed || obj->isHUDAttachment()) return;
    if (gVKChangedSpheres.size() >= 4 * 8192)
    {
        gVKChangedOverflow = true; gVKChangedSpheres.clear(); gVKChangedSphereTags.clear();
        return;
    }
    const LLVector3 p = obj->getPositionAgent();
    gVKChangedSpheres.insert(gVKChangedSpheres.end(), { p.mV[0], p.mV[1], p.mV[2], llmax(rec.radius, 0.5f) });
    gVKChangedSphereTags.push_back(4);   // glTF (llcleaningest.cpp gVKChangedSphereTags)
}

void LLCleanGLTFIngest::releaseInstances(const LLUUID& id, ObjRec& rec, U32 keep)
{
    LLCleanInstanceStore& is = LLCleanInstanceStore::instance();
    for (U32 k = keep; k < (U32)rec.inst.size(); ++k) is.release(id, (S32)k);
    if (rec.inst.size() > keep) rec.inst.resize(keep);
}

void LLCleanGLTFIngest::forgetObject(const LLUUID& id)
{
    auto it = mObjs.find(id);
    if (it == mObjs.end()) return;
    LLCleanInstanceStore::instance().releaseObject(id);
    if (it->second.local_key && it->second.geom_key.notNull())
        LLCleanMeshAsset::instance().freeAsset(it->second.geom_key);
    LL_INFOS("VKGLTF") << "forget obj=" << id << " instances=" << it->second.inst.size() << LL_ENDL;
    mObjs.erase(it);
}

void LLCleanGLTFIngest::ingestObject(LLViewerObject* obj)
{
    if (!obj || obj->isDead() || obj->getPCode() != LL_PCODE_VOLUME) return;
    const LLUUID id = obj->getID();
    ObjRec& rec = mObjs[id];
    if (!rec.claimed)
    {
        // The object may have been ingested as a regular prim before it became a glTF
        // scene (local preview on an existing prim, a fresh upload): its per-face
        // instances share our face-index namespace. Clear them once.
        LLCleanInstanceStore::instance().releaseObject(id);
        rec.claimed = true;
        rec.inst.clear();
    }

    std::shared_ptr<Asset> asset = obj->mGLTFAsset;
    if (!asset || !assetPrepared(obj))
    {
        // Still downloading / failed / removed: draw nothing (LL draws nothing for a
        // pending scene asset either). Drop instances of a previous asset.
        if (!rec.inst.empty())
        {
            releaseInstances(id, rec, 0);
            rec.sig = 0;
        }
        return;
    }

    // Asset replaced (upload finished, local preview reloaded): rebuild from scratch.
    if (rec.asset_raw != asset.get() || rec.asset.expired())
    {
        releaseInstances(id, rec, 0);
        if (rec.local_key && rec.geom_key.notNull())
            LLCleanMeshAsset::instance().freeAsset(rec.geom_key);
        rec.asset = asset;
        rec.asset_raw = asset.get();
        rec.sig = 0;
        const LLVolume* v = obj->getVolume();
        const bool sim_asset = v && asset->mFilename.empty()
            && (v->getParams().getSculptType() & LL_SCULPT_TYPE_MASK) == LL_SCULPT_TYPE_GLTF
            && v->getParams().getSculptID().notNull();
        if (sim_asset)
        {
            // Shared by every object that rezzes this asset id: captured once.
            static const LLUUID s_salt = LLUUID::generateNewID("slipstream-vk-gltf-scene");
            v->getParams().getSculptID().combine(s_salt, rec.geom_key);
            rec.local_key = false;
        }
        else
        {
            // Local preview (or an asset without an id): private geometry, freed on replace.
            rec.geom_key.generate();
            rec.local_key = true;
        }
    }

    // The node hierarchy's asset matrices (Asset::update runs this every frame for
    // prepped assets, but our visit can come first). CPU only under VK.
    asset->updateTransforms();
    rec.animated = !asset->mAnimations.empty();

    // ---- object signature: skip when nothing we read changed ----
    U64 sig = mix64(0xcbf29ce484222325ull, (U64)(uintptr_t)asset.get());
    {
        const LLVector3 p = obj->getPosition(); const LLQuaternion q = obj->getRotation();
        const LLVector3 sc = obj->getScale();
        for (int c = 0; c < 3; ++c) { sig = mixf(sig, p.mV[c]); sig = mixf(sig, sc.mV[c]); }
        for (int c = 0; c < 4; ++c) sig = mixf(sig, q.mQ[c]);
        sig = mix64(sig, (U64)(uintptr_t)obj->getParent());
        sig = mix64(sig, (U64)obj->isAttachment() | ((U64)obj->isHUDAttachment() << 1));
        LLVOAvatar* av = obj->getAvatarAncestor();
        sig = mix64(sig, (U64)(uintptr_t)av);
        for (const Image& img : asset->mImages) sig = mix64(sig, (U64)(uintptr_t)img.mTexture.get());
        if (!rec.animated)
            for (const Node& n : asset->mNodes)
            {
                const F32* m = glm::value_ptr(n.mAssetMatrix);
                for (int k = 0; k < 16; ++k) sig = mixf(sig, m[k]);
            }
        sig = sig ? sig : 1ull;
    }
    if (sig == rec.sig) return;

    LLCleanMeshAsset& ms = LLCleanMeshAsset::instance();
    LLCleanInstanceStore& is = LLCleanInstanceStore::instance();

    // ---- geometry: one slot per (mesh, primitive), captured once per asset ----
    std::vector<S32> mesh_base(asset->mMeshes.size(), 0);
    {
        S32 f = 0;
        for (size_t mi = 0; mi < asset->mMeshes.size(); ++mi)
        {
            mesh_base[mi] = f;
            f += (S32)asset->mMeshes[mi].mPrimitives.size();
        }
    }
    struct GeomInfo { bool tried = false, valid = false; U32 group = 0xFFFFFFFFu, uv_density = 0; F32 lo[3], hi[3]; };
    std::vector<GeomInfo> geom(mesh_base.empty() ? 0
        : (size_t)(mesh_base.back() + (S32)asset->mMeshes.back().mPrimitives.size()));
    U32 n_captured = 0, n_skip_mode = 0, n_skip_data = 0;
    std::vector<U32> tri;
    std::vector<LLVector2> uv;
    auto ensureGeom = [&](S32 mi, S32 pi) -> GeomInfo&
    {
        const S32 face = mesh_base[mi] + pi;
        GeomInfo& g = geom[face];
        if (g.tried) return g;
        g.tried = true;
        LLCleanMeshAsset::Slot s = ms.getSlot(rec.geom_key, kGLTFLod, face);
        if (!s.valid)
        {
            const Primitive& p = asset->mMeshes[mi].mPrimitives[pi];
            if (!triangleList(p, tri)) { ++n_skip_mode; return g; }
            const U32 nv = (U32)p.mPositions.size();
            // V flip into SL's convention (LL's GL upload flipped the same way).
            uv.resize(nv);
            for (U32 v = 0; v < nv; ++v)
            {
                const LLVector2 t = v < p.mTexCoords0.size() ? p.mTexCoords0[v] : LLVector2(0.f, 0.f);
                uv[v].set(t.mV[0], 1.f - t.mV[1]);
            }
            const bool has_n = p.mNormals.size() == nv;
            const bool has_t = p.mTangents.size() == nv;
            if (!ms.captureGLTFPrimitive(rec.geom_key, kGLTFLod, face, p.mPositions.data(),
                                         has_n ? p.mNormals.data() : nullptr,
                                         has_t ? p.mTangents.data() : nullptr, uv.data(),
                                         tri.data(), nv, (U32)tri.size()))
            {
                ++n_skip_data;
                return g;
            }
            ++n_captured;
            ++mPrimsCaptured;
            s = ms.getSlot(rec.geom_key, kGLTFLod, face);
            if (!s.valid) { ++n_skip_data; return g; }
        }
        g.valid = true;
        g.group = ms.registerLodSlot(rec.geom_key, face, kGLTFLod, s.slot_id);
        for (int c = 0; c < 3; ++c) { g.lo[c] = s.cap_lo[c]; g.hi[c] = s.cap_hi[c]; }
        g.uv_density = s.uv_density;
        return g;
    };

    // ---- pose class: world / worn attachment (joint-relative, GPU joint table) / HUD ----
    U32 pose_flags = 0, attach_slot = 0xFFFFFFFFu;
    S32 attach_bone = -1;
    if (obj->isHUDAttachment())
    {
        pose_flags |= FLAG_HUD;
    }
    else if (LLVOAvatar* av = obj->getAvatarAncestor(); av && !av->isDead())
    {
        LLViewerObject* root = obj->getRootEdit();
        if (LLViewerJointAttachment* ap = av->getTargetAttachmentPoint(root ? root : obj))
        {
            const S32 jn = ap->getJointNum();
            if (jn >= 0 && jn < (S32)LLCleanSkinStore::MAX_AVATAR_JOINTS)
            {
                attach_slot = LLCleanSkinStore::instance().ensureAvatarSlot(av);
                if (attach_slot != 0xFFFFFFFFu) { attach_bone = jn; pose_flags |= FLAG_ATTACH; }
            }
        }
    }
    if (obj->isHUDAttachment() || (isAgentAvatarValid() && obj->getAvatarAncestor() == gAgentAvatarp.get()))
        pose_flags |= FLAG_SELF_PRIO;
    const LLVector3 osc = obj->getScale();
    const F32 obj_scale = llmax(osc.mV[0], llmax(osc.mV[1], osc.mV[2]));

    // ---- instances: one per (scene node with a mesh, primitive) ----
    std::vector<S32> nodes;
    collectSceneNodes(*asset, nodes);
    std::vector<InstRef> inst_refs;
    F32 asset_radius = 0.f;
    U32 n_skinned = 0, n_texcoord1 = 0, n_mirrored = 0, n_blend = 0, n_mask = 0, n_unlit = 0;
    for (S32 ni : nodes)
    {
        const Node& node = asset->mNodes[ni];
        if (node.mMesh < 0 || node.mMesh >= (S32)asset->mMeshes.size()) continue;
        const Mesh& mesh = asset->mMeshes[node.mMesh];
        for (S32 pi = 0; pi < (S32)mesh.mPrimitives.size(); ++pi)
        {
            const GeomInfo& g = ensureGeom(node.mMesh, pi);
            if (!g.valid) continue;
            const Primitive& p = mesh.mPrimitives[pi];
            const bool skinned = isSkinnedPrim(node, p);
            if (skinned) ++n_skinned;

            LLCleanInstanceStore::Instance inst{};   // value-init: every lane zero
            F32 local[16];
            nodeLocal(node, skinned, local);
            // Placeholder pose (the local alone); refreshObjectPose below composes
            // object pose * local through the store's own pose writer.
            std::memcpy(inst.model_matrix, local, sizeof(local));
            U32 mflags = 0;
            bool tc1 = false;
            inst.material_id = materialFor(*asset, p.mMaterial, mflags, inst.tint, inst.uv_matrix, tc1);
            if (tc1) ++n_texcoord1;
            if (det3(local) < 0.f) { mflags |= FLAG_DSIDED; ++n_mirrored; }
            if (mflags & FLAG_BLEND) ++n_blend;
            if (mflags & FLAG_MASK) ++n_mask;
            if (mflags & FLAG_UNLIT) ++n_unlit;
            inst.flags = mflags | pose_flags;
            inst.lod_group_id = g.group;
            inst.skin_id = (pose_flags & FLAG_ATTACH) ? attach_slot : 0xFFFFFFFFu;
            F32 r2 = 0.f;
            for (int c = 0; c < 3; ++c)
            {
                inst.aabb_min[c] = g.lo[c];
                inst.aabb_max[c] = g.hi[c];
                const F32 e = 0.5f * (g.hi[c] - g.lo[c]);
                r2 += e * e;
            }
            // LOD radius lane (single resident level: only feeds the cull's level pick,
            // which then falls back to level 3) -- keep it positive and sane.
            inst.aabb_min[3] = llmax(sqrtf(r2) * obj_scale, 0.01f);
            inst.aabb_max[3] = (pose_flags & FLAG_ATTACH) ? (F32)attach_bone : 0.f;
            std::memcpy(&inst.spin_pivot[3], &g.uv_density, sizeof(F32));   // texel demand (0 = none)

            {
                // Asset-space bounding radius about the object origin (projector-shadow
                // invalidation sphere): local-AABB centre through the node matrix plus the
                // half-diagonal scaled by the node's largest axis.
                F32 c[3], ctr[3];
                for (int k = 0; k < 3; ++k) c[k] = 0.5f * (g.lo[k] + g.hi[k]);
                for (int k = 0; k < 3; ++k)
                    ctr[k] = local[k] * c[0] + local[4 + k] * c[1] + local[8 + k] * c[2] + local[12 + k];
                F32 axis = 0.f;
                for (int col = 0; col < 3; ++col)
                    axis = llmax(axis, sqrtf(local[col * 4] * local[col * 4] + local[col * 4 + 1] * local[col * 4 + 1]
                                             + local[col * 4 + 2] * local[col * 4 + 2]));
                asset_radius = llmax(asset_radius, sqrtf(ctr[0] * ctr[0] + ctr[1] * ctr[1] + ctr[2] * ctr[2])
                                                   + sqrtf(r2) * axis);
            }
            const S32 face = (S32)inst_refs.size();
            is.upsert(id, face, inst, /*is_active=*/false, /*sig=*/0, local);
            inst_refs.push_back(InstRef{ ni, skinned, pi });
        }
    }
    // Drop instances the new build no longer has, then compose every pose.
    if (rec.inst.size() > inst_refs.size()) releaseInstances(id, rec, (U32)inst_refs.size());
    rec.inst.swap(inst_refs);
    is.refreshObjectPose(obj);
    rec.radius = asset_radius * obj_scale;
    noteChanged(obj, rec);

    // A visit that could not build everything (geometry refused) stays unsettled.
    rec.sig = (n_skip_data == 0) ? sig : 0;
    ++mIngests;

    LL_INFOS("VKGLTF") << "ingest obj=" << id << " local=" << obj->getLocalID()
        << " asset=" << (rec.local_key ? std::string("local:") + asset->mFilename : rec.geom_key.asString())
        << " scenes=" << asset->mScenes.size() << " nodes=" << nodes.size() << "/" << asset->mNodes.size()
        << " meshes=" << asset->mMeshes.size() << " prim_slots=" << geom.size()
        << " captured_new=" << n_captured << " instances=" << rec.inst.size()
        << " materials=" << asset->mMaterials.size() << " blend=" << n_blend << " mask=" << n_mask
        << " unlit=" << n_unlit << " skinned_static=" << n_skinned << " texcoord1=" << n_texcoord1
        << " mirrored=" << n_mirrored << " skip_points_lines=" << n_skip_mode << " skip_bad=" << n_skip_data
        << " animated=" << (rec.animated ? 1 : 0)
        << " pose=" << ((pose_flags & FLAG_HUD) ? "hud" : (pose_flags & FLAG_ATTACH) ? "attach" : "world")
        << LL_ENDL;
}

bool LLCleanGLTFIngest::instanceNodePrim(const LLUUID& id, S32 face, S32& node, S32& prim) const
{
    node = prim = -1;
    auto it = mObjs.find(id);
    if (it == mObjs.end() || face < 0 || face >= (S32)it->second.inst.size()) return false;
    node = it->second.inst[face].node;
    prim = it->second.inst[face].prim;
    return true;
}

void LLCleanGLTFIngest::tick()
{
    if (mObjs.empty()) return;
    LLCleanInstanceStore& is = LLCleanInstanceStore::instance();
    U32 n_inst = 0, n_anim = 0, n_ready = 0;
    for (auto it = mObjs.begin(); it != mObjs.end(); )
    {
        LLViewerObject* obj = gObjectList.findObject(it->first);
        if (!obj || obj->isDead())
        {
            // markDead already released the instances; drop the record (and a local
            // preview's private geometry -- sim assets' geometry is left to the mesh GC,
            // which frees it once no instance references it).
            if (it->second.local_key && it->second.geom_key.notNull())
                LLCleanMeshAsset::instance().freeAsset(it->second.geom_key);
            it = mObjs.erase(it);
            continue;
        }
        ObjRec& rec = it->second;
        n_inst += (U32)rec.inst.size();
        if (!rec.inst.empty()) ++n_ready;
        // RIGID NODE ANIMATION: Asset::update (GLTFSceneManager::update, every frame)
        // advanced the node matrices; push the new node-local transforms and recompose.
        std::shared_ptr<Asset> asset = rec.asset.lock();
        if (rec.animated && asset && asset.get() == obj->mGLTFAsset.get() && !rec.inst.empty())
        {
            ++n_anim;
            bool any = false;
            for (U32 k = 0; k < (U32)rec.inst.size(); ++k)
            {
                const InstRef& r = rec.inst[k];
                if (r.skinned || r.node < 0 || r.node >= (S32)asset->mNodes.size()) continue;
                F32 local[16];
                nodeLocal(asset->mNodes[r.node], false, local);
                any |= is.setLocalXform(it->first, (S32)k, local);
            }
            if (any)
            {
                is.refreshObjectPose(obj);
                // #567: an animated scene is a dynamic shadow caster (overlay), not a
                // per-frame invalidation of every static tile around it.
                if (!LLCleanIngest::instance().noteAnimated(obj)) noteChanged(obj, rec);
            }
        }
        ++it;
    }
    const F64 now = LLTimer::getTotalSeconds();
    if (now >= mNextSummary)
    {
        mNextSummary = now + 10.0;
        LL_INFOS("VKGLTF") << "summary objects=" << mObjs.size() << " drawing=" << n_ready
            << " instances=" << n_inst << " animated=" << n_anim << " ingests=" << mIngests
            << " prims_captured=" << mPrimsCaptured << LL_ENDL;
    }
}
