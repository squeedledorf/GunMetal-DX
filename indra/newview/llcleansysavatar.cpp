/**
 * @file llcleansysavatar.cpp
 * @brief Vulkan ingest of the classic system avatar body (see llcleansysavatar.h).
 *
 * MORPH CHOICE (documented per the GPU-driven rule). The first correct step uploads each
 * avatar's MORPHED reference vertex stream (pos / uv / normal, 48 B per vertex) as its
 * own slot, only when LLPolyMesh::mMorphGen moved -- shape edits, blinks (head only),
 * hand poses (upper body), avatar physics -- and never per frame otherwise. Indices,
 * weights and LODs stay shared. LL already evaluates the morphs on the CPU for its own
 * bookkeeping (volume morphs move collision volumes, the appearance editor reads them),
 * so this costs one memcpy-sized repack + a same-shape in-place VK placement. A GPU morph
 * (per-mesh delta tables of (vertex, dpos, dnormal, dbinormal, duv) x per-avatar weight
 * array, scaled-normal accumulate + renormalize, self-only morph masks as per-vertex
 * mask weights) would replace the CPU apply AND the upload; it is the follow-up, gated on
 * LL's differential apply (mLastWeight) being retired for VK avatars.
 *
 * DRAW RULES (LL's removed LLVOAvatar::renderSkinned / renderTransparent / renderRigid,
 * LLDrawPoolAvatar): head / upper / lower / eyes alpha-MASKED at LL's 0.2 minimum alpha
 * (deferred avatar pass), skirt / eyelashes / hair alpha-BLENDED at the same cutoff
 * (post-deferred avatar alpha pass); hair + skirt are LL's "transparent" meshes, drawn
 * two-sided. Legacy diffuse shading, no shininess. A part draws only while its bake is
 * visible (isTextureVisible: defined and not IMG_INVISIBLE -- server bakes hide fully
 * transparent regions with IMG_INVISIBLE, which is all LL tests; a transparent texel of
 * a visible bake is discarded by the 0.2 cutoff exactly as in LL), the skirt only while
 * one is worn, and nothing until the avatar is fully loaded (LL draws the loading cloud).
 *
 * <GMDX> DRAWN WHILE LOADING (GMDXDrawLoadingAvatars, on by default; llvoavatar.cpp then makes
 * no cloud). The fully-loaded rule was LL's (LLDrawPoolAvatar returned early for a loading
 * avatar and the particle cloud stood in), not a data guard: every part here already waits
 * for what it needs -- a built avatar, its mesh and LOD geometry, its morph stream, a skeleton
 * the skin store can resolve (buildSkin refuses a pose it cannot) -- and attachments draw mesh
 * by mesh as they arrive (llcleanclusterskinvk.cpp). So with the setting on, the whole-avatar
 * gate goes and a part's texture is decided per part:
 *  - its bake decoded: drawn with it, at whatever level has arrived (it sharpens in place);
 *  - its bake not here yet (the TE is still LL's default, or fetched with nothing decoded):
 *    while the avatar is not fully loaded, an opaque part (head, upper, lower, eyes) stands in
 *    plain grey (no texture, kStandInGrey tint) -- the "Ruth" grey older viewers showed;
 *    hair, eyelashes and skirt (alpha-blended) wait for their real bakes, a white or grey
 *    sheet of hair or lash quads would be worse than nothing;
 *  - a part already drawn with a real bake whose replacement has not decoded (an outfit
 *    change) keeps the old bake until the new one has pixels: no blink, no grey flash.
 * The stand-in turns into the bake on the frame the bake has its first pixels (same rows, a
 * material change, no hide in between). A part LL hides (IMG_INVISIBLE, a BOM attachment,
 * no skirt worn) hides as before, even while loading.
 */

#include "llviewerprecompiledheaders.h"

#include "llcleansysavatar.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llavatarappearancedefines.h"
#include "llavatarjointmesh.h"
#include "llcleaninstancestore.h"
#include "llcleanmaterialstore.h"
#include "llcleanmeshasset.h"
#include "llcleanskinstore.h"
#include "lldrawable.h"
#include "llpolymesh.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerjoint.h"
#include "llviewertexture.h"
#include "llvkhitch.h"
#include "llvoavatar.h"
#include "llwearabletype.h"
#include "m3math.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace LLAvatarAppearanceDefines;

namespace
{
constexpr U32 INV = 0xFFFFFFFFu;
constexpr F32 kMinAlpha = 0.2f;               // LLDrawPoolAvatar::sMinimumAlpha
constexpr F32 kLodTweakRange = 0.7f;          // llvoavatar.cpp AVATAR_LOD_TWEAK_RANGE
// Morph-stream uploads per frame, in vertices (self is never deferred). FIRST uploads (a
// part becoming drawable) and REFRESHES (morph gen moved: blinks, hand poses, avatar
// physics) have separate pools, so a crowd of bouncing avatars can only delay each
// other's refreshes -- never keep a newcomer's body from appearing.
constexpr U32 kMorphBudgetVerts = 48u * 1024u;
constexpr U32 kMorphFirstBudgetVerts = 48u * 1024u;
constexpr U32 kSkinRefreshFrames = 16;         // per-avatar skin meta re-verify cadence
constexpr F32 kStandInGrey = 0.5f;             // <GMDX> tint of a bake that has not arrived (header)

struct PartInfo { ETextureIndex tex; EBakedTextureIndex bake; bool blend; bool two_sided; bool head; };
// Indexed by EMeshIndex (MESH_ID_HAIR .. MESH_ID_SKIRT).
const PartInfo kPartInfo[LLCleanSysAvatar::kParts] = {
    { TEX_HAIR_BAKED,  BAKED_HAIR,  true,  true,  true  },   // MESH_ID_HAIR
    { TEX_HEAD_BAKED,  BAKED_HEAD,  false, false, true  },   // MESH_ID_HEAD
    { TEX_HEAD_BAKED,  BAKED_HEAD,  true,  false, true  },   // MESH_ID_EYELASH
    { TEX_UPPER_BAKED, BAKED_UPPER, false, false, false },   // MESH_ID_UPPER_BODY
    { TEX_LOWER_BAKED, BAKED_LOWER, false, false, false },   // MESH_ID_LOWER_BODY
    { TEX_EYES_BAKED,  BAKED_EYES,  false, false, true  },   // MESH_ID_EYEBALL_LEFT
    { TEX_EYES_BAKED,  BAKED_EYES,  false, false, true  },   // MESH_ID_EYEBALL_RIGHT
    { TEX_SKIRT_BAKED, BAKED_SKIRT, true,  true,  false },   // MESH_ID_SKIRT
};
static_assert(MESH_ID_NUM_INDICES == LLCleanSysAvatar::kParts, "system body part table");

LLAvatarJointMesh* meshPart(LLViewerJoint* lod_joint, S32 k)
{
    if (!lod_joint || k < 0 || k >= (S32)lod_joint->mMeshParts.size()) return nullptr;
    return lod_joint->mMeshParts[(size_t)k];
}

// The LOD index (within the part) of the mesh part holding `k`'s reference mesh.
S32 refLod(LLViewerJoint* lod_joint, S32 k)
{
    LLAvatarJointMesh* jm = meshPart(lod_joint, k);
    LLPolyMesh* mesh = jm ? jm->getMesh() : nullptr;
    if (!mesh) return -1;
    LLPolyMesh* ref = mesh->getReferenceMesh();
    const S32 n = (S32)lod_joint->mMeshParts.size();
    for (S32 r = 0; r < n && r < LLCleanSysAvatar::kMaxLod; ++r)
    {
        LLAvatarJointMesh* rj = lod_joint->mMeshParts[(size_t)r];
        if (rj && rj->getMesh() == ref) return r;
    }
    return -1;
}

// LL's getBaseSkeletonAncestor (llavatarjointmesh.cpp).
LLJoint* baseAncestor(LLJoint* j)
{
    LLJoint* a = j ? j->getParent() : nullptr;
    while (a && a->getParent() && a->getSupport() != LLJoint::SUPPORT_BASE) a = a->getParent();
    return a;
}

// Joint render entry i of a reference polymesh: the joint whose world matrix it uses and
// its skin pivot, as LL's software skinning built them (LLVKPreviewRender::skinJointMesh:
// an entry WITH a skin joint pivots at its root-to-joint skin offset; an ancestor entry
// at the root-to-parent offset of the skin joint that follows it).
bool jrdJoint(const std::vector<LLJointRenderData*>& jrd, size_t i, LLJoint*& joint, LLVector3& pivot)
{
    joint = nullptr;
    if (i >= jrd.size() || !jrd[i]) return false;
    if (LLSkinJoint* sj = jrd[i]->mSkinJoint)
    {
        joint = sj->mJoint;
        pivot = sj->mRootToJointSkinOffset;
        return joint != nullptr;
    }
    if (i + 1 < jrd.size() && jrd[i + 1] && jrd[i + 1]->mSkinJoint)
    {
        LLSkinJoint* nx = jrd[i + 1]->mSkinJoint;
        joint = baseAncestor(nx->mJoint);
        pivot = nx->mRootToParentJointSkinOffset;
        return joint != nullptr;
    }
    return false;
}

// Joint-table row (LLCleanSkinStore: av->getJoint(row)) of `joint`, or -1.
S32 jointRow(LLVOAvatar* av, LLJoint* joint)
{
    if (!av || !joint) return -1;
    const S32 n = joint->getJointNum();
    if (n >= 0 && n < (S32)LLCleanSkinStore::MAX_AVATAR_JOINTS && av->getJoint(n) == joint) return n;
    for (S32 j = 0; j < (S32)LLCleanSkinStore::MAX_AVATAR_JOINTS; ++j)
        if (av->getJoint(j) == joint) return j;
    return -1;
}

// Rigid eyeball: the mesh joint's world matrix expressed in its skeleton (eye) joint's
// frame, from the LOCAL transforms and LLXformMatrix's composition rules (child world
// rotation = rot * parent rot; position offset scaled by the parent's scale only when the
// parent scales child offsets; the world matrix carries only the joint's OWN scale). So
// the palette entry eye_world * M reproduces jm->getWorldMatrix() at any pose without
// the CPU pose. Column-major (== LLMatrix4's row-vector array).
bool rigidLocal(LLJoint* jm, F32 out[16])
{
    LLJoint* lj = jm ? jm->getParent() : nullptr;
    LLJoint* ej = lj ? lj->getParent() : nullptr;
    if (!ej) return false;
    const LLVector3 s_jm = jm->getScale(), s_lj = lj->getScale(), s_e = ej->getScale();
    LLVector3 p_lj = lj->getPosition();
    if (ej->getXform()->getScaleChildOffset()) p_lj.scaleVec(s_e);
    LLVector3 p_jm = jm->getPosition();
    if (lj->getXform()->getScaleChildOffset()) p_jm.scaleVec(s_lj);
    const LLQuaternion r_lj = lj->getRotation();
    const LLQuaternion q = jm->getRotation() * r_lj;
    const LLVector3 t = p_lj + p_jm * r_lj;
    const LLMatrix3 rq(q);
    for (S32 c = 0; c < 3; ++c)
        if (std::fabs(s_e.mV[c]) < 1e-6f) return false;
    for (S32 i = 0; i < 3; ++i)
    {
        for (S32 c = 0; c < 3; ++c) out[i * 4 + c] = s_jm.mV[i] * rq.mMatrix[i][c] / s_e.mV[c];
        out[i * 4 + 3] = 0.f;
    }
    for (S32 c = 0; c < 3; ++c) out[12 + c] = t.mV[c] / s_e.mV[c];
    out[15] = 1.f;
    return true;
}

LLUUID sharedKey(S32 part, S32 lod)
{
    LLUUID u;
    u.generate(llformat("slipstream-sysavi-body:%d:%d", part, lod));
    return u;
}

U64 mix64(U64 h, U64 v) { return (h ^ v) * 0x100000001B3ull; }
} // namespace

LLCleanSysAvatar& LLCleanSysAvatar::instance()
{
    static LLCleanSysAvatar s;
    return s;
}

// ---- global palette ---------------------------------------------------------------

bool LLCleanSysAvatar::ensurePalette(LLVOAvatar* av, S32 part, S32 ref_lod)
{
    std::vector<U8>& map = mJrdPal[part][ref_lod];
    LLAvatarJointMesh* rj = meshPart(av->getViewerJoint(part), ref_lod);
    LLPolyMesh* ref = rj ? rj->getMesh() : nullptr;
    if (!ref) return false;
    const std::vector<LLJointRenderData*>& jrd = ref->mJointRenderData;
    if (jrd.empty()) return false;
    if (map.size() == jrd.size()) return true;
    std::vector<U8> m(jrd.size(), 0);
    for (size_t i = 0; i < jrd.size(); ++i)
    {
        LLJoint* joint = nullptr;
        LLVector3 pivot;
        if (!jrdJoint(jrd, i, joint, pivot)) return false;
        const std::string& name = joint->getName();
        size_t e = 0;
        for (; e < mPal.size(); ++e)
        {
            const PalEntry& p = mPal[e];
            if (!p.rigid && p.joint == name && std::fabs(p.pivot[0] - pivot.mV[0]) < 1e-5f
                && std::fabs(p.pivot[1] - pivot.mV[1]) < 1e-5f && std::fabs(p.pivot[2] - pivot.mV[2]) < 1e-5f)
                break;
        }
        if (e == mPal.size())
        {
            if (mPal.size() >= LLCleanSkinStore::MAX_JOINTS) return false;
            PalEntry p;
            p.joint = name;
            for (S32 c = 0; c < 3; ++c) p.pivot[c] = pivot.mV[c];
            p.part = part; p.lod = ref_lod; p.jrd = (S32)i;
            mPal.push_back(p);
            ++mPalGen;
        }
        m[i] = (U8)e;
    }
    map.swap(m);
    return true;
}

bool LLCleanSysAvatar::ensureRigidPal(LLVOAvatar* av, S32 part, S32 lod)
{
    if (mRigidPal[part][lod] >= 0) return true;
    LLAvatarJointMesh* jm = meshPart(av->getViewerJoint(part), lod);
    LLJoint* lj = jm ? jm->getParent() : nullptr;
    LLJoint* ej = lj ? lj->getParent() : nullptr;
    if (!ej || mPal.size() >= LLCleanSkinStore::MAX_JOINTS) return false;
    PalEntry p;
    p.joint = ej->getName();
    p.rigid = true;
    p.part = part; p.lod = lod;
    mRigidPal[part][lod] = (S16)mPal.size();
    mPal.push_back(p);
    ++mPalGen;
    return true;
}

// ---- shared (part, LOD) geometry ----------------------------------------------------

bool LLCleanSysAvatar::ensureShared(LLVOAvatar* av, S32 part, S32 lod)
{
    Shared& sh = mShared[part][lod];
    if (sh.group != INV) return true;
    if (sh.failed) return false;
    LLViewerJoint* lj = av->getViewerJoint(part);
    LLAvatarJointMesh* jm = meshPart(lj, lod);
    LLPolyMesh* mesh = jm ? jm->getMesh() : nullptr;
    if (!mesh) return false;
    const U32 nv = mesh->getNumVertices();
    const S32 nf = mesh->getNumFaces();
    const LLVector4a* pos = mesh->getBaseCoords();
    const LLVector4a* nrm = mesh->getBaseNormals();
    const LLVector2*  uv  = mesh->getBaseTexCoords();
    const LLPolyFace* faces = mesh->getFaces();
    if (nv < 3 || nf <= 0 || !pos || !nrm || !uv || !faces) return false;

    std::vector<LLCleanMeshAsset::MirrorWeight> wts(nv);
    if (mesh->hasWeights())
    {
        const S32 r = refLod(lj, lod);
        const F32* w = mesh->getWeights();
        if (r < 0 || !w || !ensurePalette(av, part, r)) return false;
        const std::vector<U8>& pal = mJrdPal[part][r];
        const S32 nj = (S32)pal.size();
        // LL's polymesh weight: integer part = joint render entry j, fraction = blend
        // toward entry j + 1 (a matrix lerp == two-influence LBS). Pre-skin decodes
        // weight4 as floor = palette index, fract = weight, normalized by the sum, so:
        // a blend is (g[j] + (1 - f), g[j + 1] + f) with f kept off 0 / 1 (a fraction of
        // 0 or 1 would floor onto the wrong joint), and a single influence is g[j] + 0.5.
        // (#582: built in that float form, then encoded to the 10 B mirror weight by the
        // same LLCleanQV::encodeWeight as rigged mesh; palette entries are U8, so it always
        // encodes.)
        for (U32 v = 0; v < nv; ++v)
        {
            const F32 wv = w[v];
            const S32 j = llclamp((S32)wv, 0, nj - 1);
            F32 f = wv - (F32)(S32)wv;
            F32 o[4] = {0.f, 0.f, 0.f, 0.f};
            if (f != 0.f && j + 1 < nj)
            {
                f = llclamp(f, 1e-3f, 1.f - 1e-3f);
                o[0] = (F32)pal[(size_t)j] + (1.f - f);
                o[1] = (F32)pal[(size_t)j + 1] + f;
            }
            else
            {
                o[0] = (F32)pal[(size_t)j] + 0.5f;
            }
            LLCleanQV::encodeWeight(o, wts[v]);
        }
    }
    else
    {
        // Rigid (eyeballs): every vertex on the part's own palette entry.
        if (!ensureRigidPal(av, part, lod)) return false;
        const F32 g[4] = {(F32)mRigidPal[part][lod] + 0.5f, 0.f, 0.f, 0.f};
        LLCleanMeshAsset::MirrorWeight rw;
        LLCleanQV::encodeWeight(g, rw);
        for (U32 v = 0; v < nv; ++v) wts[v] = rw;
    }

    std::vector<LLCleanMeshAsset::MirrorVertex> verts(nv);
    for (U32 v = 0; v < nv; ++v)
    {
        LLCleanMeshAsset::MirrorVertex& o = verts[v];
        const F32* p = pos[v].getF32ptr();
        const F32* n = nrm[v].getF32ptr();
        o.pos[0] = p[0]; o.pos[1] = p[1]; o.pos[2] = p[2];
        o.uv[0] = uv[v].mV[0]; o.uv[1] = uv[v].mV[1];
        o.normal[0] = n[0]; o.normal[1] = n[1]; o.normal[2] = n[2];
        o.tangent[0] = o.tangent[1] = o.tangent[2] = o.tangent[3] = 0.f;   // no normal map: FS derives a frame
    }
    std::vector<U32> idx((size_t)nf * 3);
    for (S32 f = 0; f < nf; ++f)
        for (S32 c = 0; c < 3; ++c) idx[(size_t)f * 3 + c] = (U32)faces[f][c];

    const LLUUID key = sharedKey(part, lod);
    const U32 slot = LLCleanMeshAsset::instance().captureRawSlot(key, 0, 0, verts.data(), wts.data(),
                                                                 idx.data(), nv, (U32)idx.size(), false);
    if (slot == INV)
    {
        sh.failed = true;
        LL_WARNS("VKSysAvatar") << "shared capture failed part " << part << " lod " << lod << " nv " << nv << LL_ENDL;
        return false;
    }
    sh.slot = slot;
    sh.nv = nv;
    sh.group = LLCleanMeshAsset::instance().registerLodSlot(key, 0, LLCleanMeshAsset::NUM_LOD - 1, slot);
    for (S32 c = 0; c < 3; ++c) { sh.lo[c] = 1e30f; sh.hi[c] = -1e30f; }
    for (U32 v = 0; v < nv; ++v)
        for (S32 c = 0; c < 3; ++c) { sh.lo[c] = llmin(sh.lo[c], verts[v].pos[c]); sh.hi[c] = llmax(sh.hi[c], verts[v].pos[c]); }
    LL_INFOS("VKSysAvatar") << "shared part " << part << " lod " << lod << ": slot " << slot << " group " << sh.group
        << " verts " << nv << " tris " << nf << (mesh->hasWeights() ? " skinned" : " rigid")
        << " | palette " << mPal.size() << LL_ENDL;
    return sh.group != INV;
}

// ---- per-avatar morph stream ----------------------------------------------------------

U32 LLCleanSysAvatar::ensureMorph(LLVOAvatar* av, AvState& st, S32 part, S32 ref_lod, U32& budget,
                                  U32& budget_first, U32& uploads)
{
    Morph& m = st.morph[part][ref_lod];
    LLAvatarJointMesh* rj = meshPart(av->getViewerJoint(part), ref_lod);
    LLPolyMesh* ref = rj ? rj->getMesh() : nullptr;
    if (!ref) return INV;
    if (m.slot != INV && m.gen == ref->mMorphGen) return m.slot;
    const U32 nv = ref->getNumVertices();
    U32& pool = m.slot == INV ? budget_first : budget;
    if (!av->isSelf() && pool < nv) return m.slot;   // refresh: stale this frame; first: waits its turn
    const LLVector4a* pos = ref->getCoords();
    const LLVector4a* nrm = ref->getNormals();
    const LLVector2*  uv  = ref->getTexCoords();
    if (nv < 3 || !pos || !nrm || !uv) return m.slot;
    static std::vector<LLCleanMeshAsset::MirrorVertex> s_verts;
    s_verts.resize(nv);
    for (U32 v = 0; v < nv; ++v)
    {
        LLCleanMeshAsset::MirrorVertex& o = s_verts[v];
        const F32* p = pos[v].getF32ptr();
        const F32* n = nrm[v].getF32ptr();
        o.pos[0] = p[0]; o.pos[1] = p[1]; o.pos[2] = p[2];
        o.uv[0] = uv[v].mV[0]; o.uv[1] = uv[v].mV[1];
        o.normal[0] = n[0]; o.normal[1] = n[1]; o.normal[2] = n[2];
        o.tangent[0] = o.tangent[1] = o.tangent[2] = o.tangent[3] = 0.f;
    }
    // Vertex stream only: the dummy triangle keeps the packer's "has geometry" rule; no
    // instance's LOD group names this slot, so it is never drawn.
    static const U32 kDummyIdx[3] = {0, 1, 2};
    if (m.key.isNull())
        m.key.generate(av->getID().asString() + llformat(":sysavi-morph:%d:%d", part, ref_lod));
    const U32 slot = LLCleanMeshAsset::instance().captureRawSlot(m.key, 0, 0, s_verts.data(), nullptr,
                                                                 kDummyIdx, nv, 3, false);
    if (slot == INV) return m.slot;
    m.slot = slot;
    m.gen = ref->mMorphGen;
    pool = pool > nv ? pool - nv : 0u;
    ++uploads;
    return m.slot;
}

// ---- per-avatar skin meta ---------------------------------------------------------------

bool LLCleanSysAvatar::buildSkin(LLVOAvatar* av, AvState& st)
{
    const U32 n = (U32)mPal.size();
    if (n == 0) return false;
    static std::vector<S32> s_jn;
    static std::vector<F32> s_ib;
    s_jn.assign(n, 0);
    s_ib.assign((size_t)n * 16, 0.f);
    static const F32 IDENT[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    for (U32 e = 0; e < n; ++e)
    {
        const PalEntry& p = mPal[e];
        F32* ib = &s_ib[(size_t)e * 16];
        std::memcpy(ib, IDENT, sizeof(IDENT));
        LLViewerJoint* lj = av->getViewerJoint(p.part);
        LLAvatarJointMesh* jm = meshPart(lj, p.lod);
        LLJoint* joint = nullptr;
        if (p.rigid)
        {
            joint = (jm && jm->getParent()) ? jm->getParent()->getParent() : nullptr;
            if (!jm || !rigidLocal(jm, ib)) std::memcpy(ib, IDENT, sizeof(IDENT));
        }
        else
        {
            LLPolyMesh* ref = jm ? jm->getMesh() : nullptr;
            LLVector3 pivot;
            if (ref && jrdJoint(ref->mJointRenderData, (size_t)p.jrd, joint, pivot))
            {
                // palette = joint world * T(pivot) (LL: world translated by the pivot
                // rotated into the joint's frame).
                ib[12] = pivot.mV[0]; ib[13] = pivot.mV[1]; ib[14] = pivot.mV[2];
            }
        }
        const S32 row = jointRow(av, joint);
        if (row < 0) return false;   // skeleton not resolvable (yet): don't draw a wrong pose
        s_jn[e] = row;
    }
    const U32 id = LLCleanSkinStore::instance().sysBodySkin(av, n, s_jn.data(), s_ib.data());
    if (id == INV) return false;
    st.skin_id = id;
    st.skin_pal_gen = mPalGen;
    st.skin_frame = mFrame;
    return true;
}

// ---- lifecycle --------------------------------------------------------------------------

// #461 forensics: log (self only, on change) why each system body part is or is not drawn,
// so an intermittent "body shows through the alpha" can be read off the log afterwards.
void LLCleanSysAvatar::noteWhy(LLVOAvatar* av, AvState& st, S32 part, U8 why)
{
    if (st.why[part] == why) return;
    st.why[part] = why;
    if (!av->isSelf()) return;
    static const char* kWhy[] = {"drawn", "bake invisible/undefined", "no skirt worn",
                                 "joint hidden by BOM attachment", "bake not ready"};
    LL_INFOS("VKSysAvatar") << "#461 self system part " << part << ": " << kWhy[why < 5 ? why : 4]
        << " (bake tex " << (av->getTEImage(kPartInfo[part].tex) ? av->getTEImage(kPartInfo[part].tex)->getID() : LLUUID::null) << ", fullyLoaded "
        << av->isFullyLoaded() << ", usingLocal " << av->isUsingLocalAppearance() << ")" << LL_ENDL;
}

void LLCleanSysAvatar::hidePart(const LLUUID& id, AvState& st, S32 part)
{
    Part& pt = st.parts[part];
    if (pt.live) LLCleanInstanceStore::instance().release(id, kFaceBase + part);
    pt.live = false;
    pt.sig = 0;
    // The released row was the material's holder: the store may sweep (and later reuse)
    // the id while the part is hidden, so re-resolve it when the part comes back.
    pt.material = INV;
    // The part's morph streams go with it (an outfit change brings them back).
    for (S32 r = 0; r < kMaxLod; ++r)
    {
        Morph& m = st.morph[part][r];
        if (m.slot != INV) LLCleanMeshAsset::instance().freeAsset(m.key);
        m.slot = INV;
        m.gen = 0;
    }
}

void LLCleanSysAvatar::releaseAvatar(const LLUUID& id, AvState& st)
{
    for (S32 p = 0; p < kParts; ++p) hidePart(id, st, p);
}

void LLCleanSysAvatar::tick()
{
    if (!mPalInit)
    {
        for (S32 p = 0; p < kParts; ++p)
            for (S32 l = 0; l < kMaxLod; ++l) mRigidPal[p][l] = -1;
        mPalInit = true;
    }
    static LLCachedControl<bool> s_enabled(gSavedSettings, "RenderVKSystemAvatarBody", true);
    static LLCachedControl<bool> s_autoskip(gSavedSettings, "RenderVKSystemAvatarAutoSkip", true);
    static LLCachedControl<bool> s_draw_loading(gSavedSettings, "GMDXDrawLoadingAvatars", true);   // <GMDX>
    ++mFrame;
    U32 drawn = 0, skipped = 0, uploads = 0, drawn_avatars = 0;
    if (!s_enabled)
    {
        if (mWasEnabled)
        {
            for (auto& kv : mAv) releaseAvatar(kv.first, kv.second);
            mAv.clear();
            mWasEnabled = false;
            LL_INFOS("VKSysAvatar") << "system avatar bodies off (RenderVKSystemAvatarBody)" << LL_ENDL;
        }
        return;
    }
    mWasEnabled = true;

    LLCleanInstanceStore& is = LLCleanInstanceStore::instance();
    LLCleanMaterialStore& mm = LLCleanMaterialStore::instance();
    U32 budget = kMorphBudgetVerts, budget_first = kMorphFirstBudgetVerts;

    // LL's avatar LOD inputs (LLVOAvatar::updateJointLODs).
    const F32 lod_factor = LLVOAvatar::sLODFactor * kLodTweakRange + (1.f - kLodTweakRange);
    const F32 num_min = clamp_rescale(LLVOAvatar::sLODFactor, 0.f, 1.f, 0.25f, 0.6f);
    const F32 num_factor = clamp_rescale((F32)mLastDrawnAvatars, 8.f, 25.f, 1.f, num_min);
    const LLViewerCamera* cam = LLViewerCamera::getInstance();
    const F32 px_per_rad = cam->getView() > 0.f ? (F32)cam->getViewHeightInPixels() / cam->getView() : 1000.f;
    const LLVector3 cam_pos = cam->getOrigin();

    // Rotating start: the per-frame upload budgets go round the crowd, not to the same
    // avatars at the front of the list every frame.
    static std::vector<LLCharacter*> s_chars;
    s_chars.assign(LLCharacter::sInstances.begin(), LLCharacter::sInstances.end());
    const size_t n_chars = s_chars.size();
    const size_t start = n_chars ? (size_t)mFrame % n_chars : 0;
    for (size_t ci = 0; ci < n_chars; ++ci)
    {
        LLVOAvatar* av = (LLVOAvatar*)s_chars[(start + ci) % n_chars];
        if (!av || av->isDead() || av->isUIAvatar() || av->isControlAvatar() || av->mIsDummy
            || !av->isBuilt() || av->mDrawable.isNull())
            continue;
        const LLUUID& id = av->getID();
        AvState& st = mAv[id];
        st.seen = mFrame;

        // Whole-avatar rules: not before it is fully loaded (LL draws the loading cloud),
        // self hidden by RLVa showself outside mouselook (mouselook hiding is the main-
        // view rigged VS's, so shadows / mirrors keep the body as for rigged mesh).
        // <GMDX> GMDXDrawLoadingAvatars: drawn while it loads (header).
        const bool loading = !av->isFullyLoaded();
        bool show = !loading || s_draw_loading;
        if (show && av->isSelf() && !gAgent.needsRenderAvatar() && !gAgentCamera.cameraMouselook())
            show = false;
        if (!show)
        {
            for (S32 p = 0; p < kParts; ++p) if (st.parts[p].live) hidePart(id, st, p);
            skipped += kParts;
            continue;
        }

        // LOD: LL's adjusted pixel area against each mesh part's minimum (updateLOD).
        F32 area = 1e8f;
        if (!(av->isSelf() && (gAgentCamera.cameraCustomizeAvatar() || gAgentCamera.cameraMouselook())))
        {
            const LLVector4a* ext = av->mDrawable->getSpatialExtents();
            LLVector3 lo(ext[0].getF32ptr()), hi(ext[1].getF32ptr());
            if (!lo.isFinite() || !hi.isFinite() || hi.mV[0] < lo.mV[0] || (hi - lo).magVec() > 64.f)
            {
                const LLVector3 p = av->getRenderPosition();
                lo = p - LLVector3(0.5f, 0.5f, 1.f); hi = p + LLVector3(0.5f, 0.5f, 1.f);
            }
            const LLVector3 center = (lo + hi) * 0.5f, size = (hi - lo) * 0.5f;
            F32 dist = (center - cam_pos).magVec();
            if (dist < 16.f) { dist /= 16.f; dist *= dist; dist *= 16.f; }
            const F32 radius = atanf(size.magVec() / llmax(dist, 1e-3f)) * px_per_rad;
            area = radius * radius * F_PI * 0.16f * lod_factor * lod_factor * num_factor * num_factor;
            if (av->isVKReduced()) area *= LLVOAvatar::vkReducedLODScale() * LLVOAvatar::vkReducedLODScale();
        }

        // Parts: visibility, texture, LOD, shared geometry, morph stream.
        struct Want { bool on = false; S32 lod = 0; U32 morph = INV; LLUUID tid; bool stand_in = false; };   // <GMDX> tid / stand_in
        Want want[kParts];
        const bool skirt_worn = av->isWearingWearableType(LLWearableType::WT_SKIRT);
        for (S32 p = 0; p < kParts; ++p)
        {
            const PartInfo& pi = kPartInfo[p];
            // #461: why a part is not drawn (0 = drawn, 1 bake invisible/undefined, 2 no skirt
            // worn, 3 LL joint hidden by a BOM attachment, 4 bake not ready, 5 geometry).
            bool vis = !s_autoskip || av->isTextureVisible(pi.tex);
            // <GMDX> a bake that has not arrived yet (the TE still LL's default) may be stood in
            // for below; IMG_INVISIBLE is defined, so a hidden part stays hidden.
            if (s_draw_loading && !vis && !av->isTextureDefined(pi.tex)) vis = true;
            U8 why = 1;
            if (p == MESH_ID_SKIRT && !skirt_worn) { vis = false; why = 2; }
            // LL's drawShape() skipped any mesh whose joint updateMeshVisibility() hid (an
            // attachment textured with an IMG_USE_BAKED_* sentinel replaces that body part).
            // The GL joint draw this gate lived in is gone, so honour it here.
            if (vis && av->getViewerJoint(p) && !av->getViewerJoint(p)->getVisible()) { vis = false; why = 3; }
            if (!vis) { noteWhy(av, st, p, why); continue; }
            LLViewerTexture* tex = av->getBakedTexture((U8)pi.bake);
            // Not baked yet / a fetched bake with no decoded level yet (it would draw the
            // material's white fallback): wait for its first pixels (appearance-edit composites
            // are not fetched textures and are drawn as soon as they exist).
            bool ready = tex && !tex->isMissingAsset() && tex->getID().notNull();
            if (LLViewerFetchedTexture* ft = dynamic_cast<LLViewerFetchedTexture*>(tex); ready && ft && ft->getDiscardLevel() < 0)
                ready = false;
            LLUUID tid;
            bool stand_in = false;
            if (ready) tid = tex->getID();
            // <GMDX> GMDXDrawLoadingAvatars (header): keep the bake a part already shows, else a
            // grey stand-in for an opaque part while the avatar loads.
            else if (s_draw_loading && st.parts[p].live && st.parts[p].tex.notNull()) tid = st.parts[p].tex;
            else if (s_draw_loading && loading && !pi.blend) stand_in = true;
            // </GMDX>
            else { noteWhy(av, st, p, 4); continue; }
            LLViewerJoint* lj = av->getViewerJoint(p);
            if (!lj || lj->mMeshParts.empty()) continue;
            const S32 nparts = llmin((S32)lj->mMeshParts.size(), kMaxLod);
            S32 lod = nparts - 1;
            for (S32 k = 0; k < nparts; ++k)
            {
                LLAvatarJointMesh* jm = lj->mMeshParts[(size_t)k];
                if (jm && (LLAvatarJoint::sDisableLOD || area >= jm->getLOD())) { lod = k; break; }
            }
            if (!ensureShared(av, p, lod)) continue;
            const S32 r = refLod(lj, lod);
            if (r < 0) continue;
            const U32 ms = ensureMorph(av, st, p, r, budget, budget_first, uploads);
            if (ms == INV) continue;
            want[p].on = true; want[p].lod = lod; want[p].morph = ms; want[p].tid = tid; want[p].stand_in = stand_in;
            noteWhy(av, st, p, 0);
        }

        // Skin: built after the parts (a newly captured part can grow the palette).
        bool any = false;
        for (S32 p = 0; p < kParts; ++p) any |= want[p].on;
        if (any)
        {
            const bool stale = st.skin_id == INV || st.skin_pal_gen != mPalGen
                            || ((mFrame + (U32)(id.mData[0])) % kSkinRefreshFrames) == 0u;
            if (stale)
            {
                if (st.skin_id != INV && LLCleanSkinStore::instance().findSysBodySkin(id) != st.skin_id)
                    st.skin_id = INV;   // reaped / recycled behind our back
                if (!buildSkin(av, st)) st.skin_id = INV;
            }
        }

        bool av_drawn = false;
        for (S32 p = 0; p < kParts; ++p)
        {
            Part& pt = st.parts[p];
            if (!want[p].on || st.skin_id == INV)
            {
                if (pt.live) hidePart(id, st, p);
                ++skipped;
                continue;
            }
            const PartInfo& pi = kPartInfo[p];
            const Shared& sh = mShared[p][want[p].lod];
            const LLUUID& tid = want[p].tid;   // <GMDX> null for the stand-in (white, tinted grey)
            if (pt.material == INV || pt.tex != tid)
            {
                LLCleanMaterialStore::MaterialDesc md;
                md.base_color_id = tid;
                md.legacy = true;                     // LL's avatar shaders: diffuse, no shininess
                for (F32& c : md.spec_color_gloss) c = 0.f;
                md.env_intensity = 0.f;
                md.alpha_cutoff = kMinAlpha;
                md.alpha_blend = pi.blend;
                pt.material = mm.getOrCreate(md);
                pt.tex = tid;
            }
            F32 tint[4] = {1.f, 1.f, 1.f, 1.f};
            if (LLAvatarJointMesh* jm = meshPart(av->getViewerJoint(p), want[p].lod))
                jm->getColor(&tint[0], &tint[1], &tint[2], &tint[3]);
            if (want[p].stand_in) for (S32 c = 0; c < 3; ++c) tint[c] *= kStandInGrey;   // <GMDX>
            U32 flags = 1u | kInstFlag | (pi.blend ? 2u : 4u);
            if (pi.two_sided) flags |= 256u;
            if (av->isSelf()) flags |= 8192u | (pi.head ? kHeadFlag : 0u);
            U64 sig = 0xcbf29ce484222325ull;
            sig = mix64(sig, sh.group); sig = mix64(sig, want[p].morph); sig = mix64(sig, pt.material);
            sig = mix64(sig, st.skin_id); sig = mix64(sig, flags);
            for (F32 c : tint) { U32 b; std::memcpy(&b, &c, 4); sig = mix64(sig, b); }
            if (!pt.live || pt.sig != sig)
            {
                LLCleanInstanceStore::Instance inst{};
                static const F32 IDENT[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
                std::memcpy(inst.model_matrix, IDENT, sizeof(IDENT));
                std::memcpy(inst.uv_matrix, IDENT, sizeof(IDENT));
                const U32 ms = want[p].morph;
                std::memcpy(&inst.uv_matrix[14], &ms, sizeof(ms));   // clean_cull.comp sysaviSrcBase
                for (S32 c = 0; c < 3; ++c) { inst.aabb_min[c] = sh.lo[c]; inst.aabb_max[c] = sh.hi[c]; }
                for (S32 c = 0; c < 4; ++c) inst.tint[c] = tint[c];
                inst.lod_group_id = sh.group;
                inst.material_id = pt.material;
                inst.flags = flags;
                inst.skin_id = st.skin_id;
                is.upsert(id, kFaceBase + p, inst, /*is_active=*/false);
                pt.live = true;
                pt.sig = sig;
            }
            ++drawn;
            av_drawn = true;
        }
        if (av_drawn) ++drawn_avatars;
    }

    // Avatars gone (or no longer eligible) since the last tick.
    for (auto it = mAv.begin(); it != mAv.end();)
    {
        if (it->second.seen != mFrame)
        {
            releaseAvatar(it->first, it->second);
            it = mAv.erase(it);
        }
        else ++it;
    }
    mLastDrawnAvatars = drawn_avatars;
    LLVKHitch::count("avSysPartsDrawn", drawn);
    LLVKHitch::count("avSysPartsSkipped", skipped);
    LLVKHitch::count("avSysMorphUploads", uploads);
}
