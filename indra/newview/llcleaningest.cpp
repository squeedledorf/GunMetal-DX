/**
 * @file llcleaningest.cpp
 * @brief Clean-pipeline ingest (Vulkan): feeds the clean stores from LL's object
 *        list -- priority/self-register sweep, moved/changed queues, terrain and
 *        water capture, geometry GC, ingest diagnostics. GL-free. Split out of the
 *        old GL clean renderer (GL-B3/B4); the renderer itself is LLCleanRendererVK.
 */

#include "llviewerprecompiledheaders.h"
#include "llcrossingtrace.h"
#include "llvkhitch.h"

#include "llcleaningest.h"
#include "llcleansysavatar.h"
#include "llcleangltfingest.h"
#include "llgltfmaterial.h"   // #772 appearance signature (getHash)
#include "llviewertexturelist.h"
#include "llvoavatar.h"
#include "llavatarnamecache.h"
#include "llavatarappearancedefines.h"
#include "llcleandebug.h"
#include "llcleanprovenance.h"

#include "llappviewer.h"
#include "llcleaninstancestore.h"
#include "llcleanmaterialstore.h"
#include "llcleanmeshasset.h"
#include "llcleanassetlife.h"            // #737 release grace (wall clock)
#include "llmemorygovernor.h"            // geometry-GC budget: RAM state
#include "vkgfx/llvulkancontext.h"      // geometry-GC budget: VRAM ladder
#include "llcleanterrainsmooth.h"
#include "llcleanparticlestore.h"
#include "llcleanskinstore.h"
#include "llcleantexture.h"
#include "llenvironment.h"
#include "llface.h"
#include "lltimer.h"
#include "llviewercontrol.h"
#include <iomanip>
#include <sstream>
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "lldrawable.h"
#include "llvovolume.h"
#include "llviewertextureanim.h"

#include "llvolumemgr.h"
#include "llmeshrepository.h"
#include "llviewerregion.h"
#include "llvosurfacepatch.h"
#include "llvotree.h"
#include "llvograss.h"
#include "llsurfacepatch.h"
#include "llsurface.h"
#include "llvlcomposition.h"
#include "llviewercamera.h"
#include "llcullcamera.h"
#include "pipeline.h"

#include "llselectmgr.h"
#include "llvoavatarself.h"
#include "llviewerjointattachment.h"
#include "llviewerwindow.h"
#include "llcontrolavatar.h"
#include <cstring>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <algorithm>
#include <vector>
#include <set>

#include "gmdxregioncapture.h"   // <GMDX> region map capture: the LOD epoch in the pre-signature

LLCleanIngest& LLCleanIngest::instance()
{
    static LLCleanIngest s;
    return s;
}

// Regions whose PBR terrain materials were boosted once (see vkIngestTerrainPatch).
static std::set<U64> sPBRTerrainBoosted;
// Terrain bake inputs per region (#811; LLCleanTerrainInputs). Bounded by the live regions:
// onRegionRemoved drops a region's entry.
static std::unordered_map<U64, LLCleanTerrainInputs> sTerrainInputs;

void LLCleanIngest::onRegionRemoved(U64 region_handle)
{
    // A reconnect with the same handle gets a new composition that needs its own boost.
    sPBRTerrainBoosted.erase(region_handle);
    sTerrainInputs.erase(region_handle);   // #811: free on liveness (the bake follows)
}

const LLCleanTerrainInputs* LLCleanIngest::terrainInputs(U64 region_handle)
{
    auto it = sTerrainInputs.find(region_handle);
    return it == sTerrainInputs.end() ? nullptr : &it->second;
}

// One patch's ingest: record its composition coords into the region grid and fold its
// content (heights + composition at its N x N grid points) into the region hash. Cheap
// (N^2 values) and idempotent: the sweep revisits settled patches with the same hash.
static void noteTerrainInputs(U64 region_handle, S32 gpe, S32 ppe, F32 mpg, S32 i0, S32 j0, S32 N,
                              const std::vector<LLVector3>& pos, const std::vector<LLVector2>& uv)
{
    if (gpe < 2 || N < 2) return;
    LLCleanTerrainInputs& ti = sTerrainInputs[region_handle];
    if (ti.gpe != gpe || ti.ppe != ppe || ti.mpg != mpg)
    {
        ti = LLCleanTerrainInputs{};   // a re-created surface: start over
        ti.gpe = gpe; ti.ppe = ppe; ti.mpg = mpg;
        ti.comp.assign((size_t)gpe * (size_t)gpe, 0.f);
    }
    U64 h = 0xcbf29ce484222325ull;
    auto mix = [&h](F32 f) { U32 u; std::memcpy(&u, &f, 4); h ^= u; h *= 0x100000001b3ull; };
    for (S32 jj = 0; jj < N; ++jj)
        for (S32 ii = 0; ii < N; ++ii)
        {
            const S32 v = jj * N + ii;
            const S32 gi = llclamp(i0 + ii, 0, gpe - 1), gj = llclamp(j0 + jj, 0, gpe - 1);
            ti.comp[(size_t)gj * (size_t)gpe + (size_t)gi] = uv[v].mV[VX];
            mix(pos[v].mV[VZ]);
            mix(uv[v].mV[VX]);
        }
    const U32 key = (U32)(j0 * gpe + i0);
    // Region hash = sum of per-patch terms: an update subtracts the old term, adds the new.
    auto term = [key](U64 ph)
    {
        U64 x = ph ^ ((U64)key * 0x9e3779b97f4a7c15ull);
        x ^= x >> 31; x *= 0xbf58476d1ce4e5b9ull;
        return x ^ (x >> 29);
    };
    auto it = ti.patch.find(key);
    if (it != ti.patch.end())
    {
        if (it->second == h) return;
        ti.hash -= term(it->second);
        it->second = h;
    }
    else ti.patch.emplace(key, h);
    ti.hash += term(h);
}

// CLEAN-ROOM TERRAIN INGEST (VK). LL's terrain path (LLTerrainPartition::getGeometry /
// LLVOSurfacePatch::updateGeometry) only runs from the diverted gPipeline rebuild AND
// reaches into an LL GL vertex buffer + uninitialized gPipeline -> it segfaults on a null
// GL PFN when pump-driven in VK. Instead, generate a full-resolution grid mesh for the
// patch straight from the region heightfield (region->getLand()->getZ), then hand it to
// the SAME clean capture + instance registration LL's ingest used (llvosurfacepatch.cpp
// ~1137-1210) -- zero GL, zero gPipeline. captureTerrain de-dups identical rebuilds, so
// calling this per sweep visit is cheap once a patch has settled. The clean draw pipeline
// uses CULL_MODE_NONE, so triangle winding is irrelevant. v1: ugly-but-visible (UVs are
// region-normalized, no splat blend); flag 32 routes it through the terrain FS branch.
static void vkIngestTerrainPatch(LLViewerObject* obj)
{
    LLVOSurfacePatch* patchp = dynamic_cast<LLVOSurfacePatch*>(obj);
    if (!patchp) return;
    LLSurfacePatch* sp = patchp->getPatch();
    if (!sp) return;
    LLSurface* surf = sp->getSurface();
    if (!surf) return;
    const S32 gpe  = surf->getGridsPerEdge();
    const S32 gppe = surf->getGridsPerPatchEdge();
    const F32 mpg  = surf->getMetersPerGrid();
    if (gppe < 1 || gpe < 2 || mpg <= 0.f) return;

    // Patch grid origin: region-local center -> lower corner -> grid index.
    const LLVector3 center = sp->getCenterRegion();
    const F32 half = 0.5f * (F32)gppe * mpg;
    S32 i0 = (S32)ll_round((center.mV[0] - half) / mpg);
    S32 j0 = (S32)ll_round((center.mV[1] - half) / mpg);
    i0 = llclamp(i0, 0, gpe - 1);
    j0 = llclamp(j0, 0, gpe - 1);

    const S32 N  = gppe + 1;                 // verts per patch edge (share the seam row)
    const U32 nv = (U32)(N * N);
    const U32 ni = (U32)(gppe * gppe * 6);
    if (nv < 3 || ni < 3) return;
    std::vector<LLVector3> pos(nv), nrm(nv);
    std::vector<LLVector2> uv(nv);
    std::vector<U16>       idx(ni);

    // The FS terrain branch reads v_uv.x as the per-vertex COMPOSITION coord
    // (range ~0..3, selecting/blending among the 4 detail textures) -- this is
    // LL's texcoord1, i.e. region->getCompositionXY(x,y) at the vertex's
    // region-local meter position. The detail UV itself is derived in the FS
    // from world XY, so uv.y is unused for terrain.
    LLViewerRegion* rgn = patchp->getRegion();
    LLVLComposition* compp = rgn ? rgn->getComposition() : nullptr;
    // Generate the composition GRID ourselves. LL builds it in
    // LLSurfacePatch::updateTexture() (generateHeights -> generateComposition), which is
    // driven from the diverted terrain render/update path -- so under VK it never runs
    // and getCompositionXY() returns default values, selecting the wrong detail texture
    // (all low-band, no grass). generateComposition() no-ops until the sim's terrain
    // params arrive (mParamsReady); the sweep re-ingest picks it up once they do.
    if (compp)
    {
        const LLVector3d org = sp->getOriginGlobal() - surf->getOriginGlobal();
        const F32 patch_size = mpg * (F32)(gppe + 1);
        compp->generateHeights((F32)org.mdV[0], (F32)org.mdV[1], patch_size, patch_size);
        compp->generateComposition();
    }
    const bool have_comp = (compp != nullptr);
    for (S32 jj = 0; jj < N; ++jj)
    for (S32 ii = 0; ii < N; ++ii)
    {
        const S32 gi = llclamp(i0 + ii, 0, gpe - 1);   // clamp at region edge (v1: no
        const S32 gj = llclamp(j0 + jj, 0, gpe - 1);   // neighbor-region sampling)
        const F32 wx = (F32)gi * mpg;                  // region-local position
        const F32 wy = (F32)gj * mpg;
        const S32 v  = jj * N + ii;
        pos[v].set(wx, wy, surf->getZ(gi, gj));
        // Central-difference normal from the heightfield.
        const F32 zl = surf->getZ(llclamp(gi - 1, 0, gpe - 1), gj);
        const F32 zr = surf->getZ(llclamp(gi + 1, 0, gpe - 1), gj);
        const F32 zd = surf->getZ(gi, llclamp(gj - 1, 0, gpe - 1));
        const F32 zu = surf->getZ(gi, llclamp(gj + 1, 0, gpe - 1));
        LLVector3 n((zl - zr) / (2.f * mpg), (zd - zu) / (2.f * mpg), 1.f);
        n.normVec();
        nrm[v] = n;
        const F32 comp = have_comp ? rgn->getCompositionXY((S32)wx, (S32)wy) : 0.f;
        uv[v].set(comp, 0.f);
    }
    U32 k = 0;
    for (S32 jj = 0; jj < gppe; ++jj)
    for (S32 ii = 0; ii < gppe; ++ii)
    {
        const U16 a = (U16)(jj * N + ii);
        const U16 b = (U16)(jj * N + ii + 1);
        const U16 c = (U16)((jj + 1) * N + ii);
        const U16 d = (U16)((jj + 1) * N + ii + 1);
        idx[k++] = a; idx[k++] = b; idx[k++] = c;
        idx[k++] = b; idx[k++] = d; idx[k++] = c;
    }

    // #811: the terrain bake's per-region inputs (composition grid + content hash).
    if (rgn) noteTerrainInputs(rgn->getHandle(), gpe, surf->getPatchesPerEdge(), mpg, i0, j0, N, pos, uv);

    const LLUUID patch_id = patchp->getID();
    // reclaimable: the patch dies with its region (markDead releases its instance), and
    // the geometry GC frees the slot once nothing has referenced it for keep_age.
    LLCleanMeshAsset::instance().captureTerrain(
        patch_id, 0, pos.data(), nrm.data(), uv.data(), nullptr, idx.data(), nv, ni,
        /*force=*/false, /*tight=*/false, /*reclaimable=*/true);

    // Instance registration -- mirrors llvosurfacepatch.cpp:1142-1210 exactly.
    LLCleanInstanceStore::Instance inst{};
    LLViewerRegion* regionp = patchp->getRegion();
    const LLMatrix4* rm = regionp ? &regionp->mRenderMatrix : nullptr;
    if (rm) std::memcpy(inst.model_matrix, rm->mMatrix, sizeof(inst.model_matrix));
    else { inst.model_matrix[0] = inst.model_matrix[5] = inst.model_matrix[10] = inst.model_matrix[15] = 1.f; }
    // The patch's own bounds (region-local, like pos): every view culls terrain per
    // patch. A region-sized placeholder box (+-512 m, -1024..4096 m) made every patch
    // pass every frustum -- the main view, shadow cascades, mirror, and each probe face
    // (a probe looking down drew two whole regions, 262k tris, for a 64 m capture).
    {
        LLVector3 lo = pos[0], hi = pos[0];
        for (const LLVector3& q : pos)
            for (int a = 0; a < 3; ++a) { lo.mV[a] = llmin(lo.mV[a], q.mV[a]); hi.mV[a] = llmax(hi.mV[a], q.mV[a]); }
        for (int a = 0; a < 3; ++a) { inst.aabb_min[a] = lo.mV[a] - 0.25f; inst.aabb_max[a] = hi.mV[a] + 0.25f; }
    }
    inst.tint[0] = inst.tint[1] = inst.tint[2] = inst.tint[3] = 1.f;
    inst.uv_matrix[0] = inst.uv_matrix[5] = inst.uv_matrix[10] = inst.uv_matrix[15] = 1.f;
    const LLCleanMeshAsset::Slot ms = LLCleanMeshAsset::instance().getSlot(patch_id, 0, 0);
    inst.lod_group_id = 0xFFFFFFFFu;
    if (ms.valid)
    {
        // SMOOTH TERRAIN (#211, RenderVKTerrainSmooth): the group's levels become the
        // smooth meshes (level 0 built here, finer ones by the residency tick) and the box
        // grows to hold every level + skirts. Off: the faceted capture at every level,
        // exactly as before.
        std::vector<F32> comp_grid(nv);
        for (U32 v = 0; v < nv; ++v) comp_grid[v] = uv[v].mV[VX];
        F32 slo[3], shi[3];
        if (LLCleanTerrainSmooth::publishPatch(patchp, ms.slot_id, i0, j0, comp_grid.data(), slo, shi))
            for (int a = 0; a < 3; ++a) { inst.aabb_min[a] = slo[a]; inst.aabb_max[a] = shi[a]; }
        inst.lod_group_id = LLCleanMeshAsset::instance().getLodGroupId(patch_id, 0);
    }
    inst.aabb_min[3] = 0.f;

    // TERRAIN RECORD (per region, draw set 0 binding 22; index in inst.skin_id, which
    // only rigged / worn-attachment instances otherwise read). Legacy detail UV =
    // region-local XY / RenderTerrainScale + fmod(region origin, scale) -- LL's
    // object-plane texgen (lldrawpoolterrain: tp0/tp1 with the agent region's origin
    // offset), expressed per region so the tiling is continuous across regions and
    // does not shift when the agent region changes.
    LLVLComposition* rcompp = regionp ? regionp->getComposition() : nullptr;
    LLCleanMaterialStore::TerrainRecord trec;
    {
        static LLCachedControl<F32> s_terrain_scale(gSavedSettings, "RenderTerrainScale", 12.f);
        const F64 meters = llmax((F64)(F32)s_terrain_scale, 0.01);
        const F32 scale  = (F32)(1.0 / meters);
        const LLVector3d rog = regionp ? regionp->getOriginGlobal() : LLVector3d::zero;
        trec.legacy[0] = (F32)(fmod(rog.mdV[VX], meters) / meters);
        trec.legacy[1] = (F32)(fmod(rog.mdV[VY], meters) / meters);
        trec.legacy[2] = scale;
        trec.legacy[3] = 0.f;
    }

    // PBR TERRAIN (LL llvlcomposition getMaterialType): the 4 detail ids are glTF
    // material assets when they load as materials and not as textures. Re-evaluated
    // every sweep visit, so the TEXTURE -> PBR flip lands once the materials arrive.
    // Not gated on RenderTerrainPBREnabled (that only feeds the Force/UI path in LL).
    bool pbr = false;
    if (rcompp && rcompp->getMaterialType() == LLTerrainMaterials::Type::PBR)
    {
        // Builds the render materials (region overrides applied) and fetches their
        // textures -- main thread (the ingest sweep) only. Boost ONCE per region, like
        // the legacy s_boosted guard: re-boosting every patch visit churns the texture
        // VK generation. After that the renderer's full terrain demand drives decode.
        std::set<U64>& s_pbr_boosted = sPBRTerrainBoosted;
        if (s_pbr_boosted.size() >= 1024) s_pbr_boosted.clear();   // bounded (a re-boost is benign)
        rcompp->makeMaterialsReady(s_pbr_boosted.insert(regionp->getHandle()).second, false);
        static LLCachedControl<F32> s_pbr_scale(gSavedSettings, "RenderTerrainPBRScale", 8.f);
        static LLCachedControl<S32> s_pbr_detail(gSavedSettings, "RenderTerrainPBRDetail", 0);
        const F32 pbr_scale = 1.f / llmax((F32)s_pbr_scale, 0.01f);
        for (S32 d = 0; d < 4; ++d)
        {
            const LLFetchedGLTFMaterial* gm = rcompp->getDetailRenderMaterial(d);
            const LLGLTFMaterial& g = gm ? *static_cast<const LLGLTFMaterial*>(gm) : LLGLTFMaterial::sDefault;
            // Missing slot -> LLGLTFMaterial::sDefault (LL renderFullShaderPBR): white
            // base, no maps (the FS then uses white MR/emissive and a flat normal).
            LLCleanMaterialStore::MaterialDesc pd;
            pd.gltf          = true;
            pd.base_color_id = g.mTextureId[LLGLTFMaterial::GLTF_TEXTURE_INFO_BASE_COLOR];
            pd.normal_id     = g.mTextureId[LLGLTFMaterial::GLTF_TEXTURE_INFO_NORMAL];
            pd.mr_id         = g.mTextureId[LLGLTFMaterial::GLTF_TEXTURE_INFO_METALLIC_ROUGHNESS];
            pd.emissive_id   = g.mTextureId[LLGLTFMaterial::GLTF_TEXTURE_INFO_EMISSIVE];
            // Factors live in the material (not the instance tint: one terrain
            // instance blends four materials).
            for (int c = 0; c < 4; ++c) pd.base_color_factor[c] = g.mBaseColor.mV[c];
            pd.emissive_factor[0] = g.mEmissiveColor.mV[0];
            pd.emissive_factor[1] = g.mEmissiveColor.mV[1];
            pd.emissive_factor[2] = g.mEmissiveColor.mV[2];
            pd.emissive_factor[3] = 0.f;
            pd.metallic_factor  = g.mMetallicFactor;
            pd.roughness_factor = g.mRoughnessFactor;
            // LL's minimum_alphas cutout is not applied yet (the depth pre-pass treats
            // terrain as opaque); kept in the material for when it is.
            pd.alpha_cutoff = (g.mAlphaMode == LLGLTFMaterial::ALPHA_MODE_MASK) ? g.mAlphaCutoff : -1.f;
            trec.mat[d] = LLCleanMaterialStore::instance().getOrCreate(pd);

            // One transform per material: the base-colour KHR_texture_transform with
            // the scale times 1/RenderTerrainPBRScale (lldrawpoolterrain PBR). LL applies
            // it as flip(khr(flip(xy))), flip(v) = -v (textureUtilV
            // terrain_texture_transform), which works out to
            //   u' =  c*sx*x - s*sy*y + ox,   v' = s*sx*x + c*sy*y - oy.
            const LLGLTFMaterial::TextureTransform& tt =
                g.mTextureTransform[LLGLTFMaterial::GLTF_TEXTURE_INFO_BASE_COLOR];
            const F32 c = cosf(tt.mRotation), sn = sinf(tt.mRotation);
            const F32 sx = tt.mScale.mV[VX] * pbr_scale, sy = tt.mScale.mV[VY] * pbr_scale;
            F32 (&x)[2][4] = trec.xform[d];
            x[0][0] = c * sx;  x[0][1] = -sn * sy; x[0][2] =  tt.mOffset.mV[VX]; x[0][3] = 0.f;
            x[1][0] = sn * sx; x[1][1] =  c * sy;  x[1][2] = -tt.mOffset.mV[VY]; x[1][3] = 0.f;
        }
        trec.mode       = 1u;
        trec.pbr_detail = (S32)s_pbr_detail;
        trec.paint_type = rcompp->getPaintType();
        pbr = true;
    }

    LLCleanMaterialStore::MaterialDesc mdesc;
    if (rcompp && !pbr)
    {
        LLVLComposition* compp = rcompp;
        mdesc.base_color_id = compp->getDetailAssetID(0);
        mdesc.normal_id     = compp->getDetailAssetID(1);
        mdesc.mr_id         = compp->getDetailAssetID(2);
        mdesc.emissive_id   = compp->getDetailAssetID(3);
        // Keep the 4 detail textures streaming. LLDrawPoolTerrain normally ticks
        // their stats every frame (BOOST_TERRAIN is below BOOST_HIGH, so stats are
        // NOT auto-forced -- they must be fed manually), but that pool is diverted
        // under VK, so nothing boosts them and they sit at lowest discard -> terrain
        // renders as a flat low-res color. Fetch each by id and drive its stats here.
        // Boost each detail texture ONCE (not every sweep visit): re-issuing
        // forceToSaveRawImage/addTextureStats every ingest re-triggers a decode and keeps
        // bumping the texture's VK generation, so the renderer's throttled re-resolve can
        // never catch up -> the grass stayed a 16x16 placeholder forever.
        static std::set<LLUUID> s_boosted;
        if (s_boosted.size() >= 4096) s_boosted.clear();   // bounded (a re-boost is benign)
        for (S32 d = 0; d < 4; ++d)
        {
            const LLUUID tid = compp->getDetailAssetID(d);
            if (tid.isNull()) continue;
            if (!s_boosted.insert(tid).second)
            {
                // Already boosted, unless ~LLTerrainMaterials of a removed region sharing
                // this id reset it to BOOST_NONE (nothing else boosts it under VK).
                LLViewerFetchedTexture* known = LLViewerTextureManager::findFetchedTexture(tid, TEX_LIST_STANDARD);
                if (known && known->getBoostLevel() != LLGLTexture::BOOST_NONE) continue;
            }
            LLViewerFetchedTexture* t = LLViewerTextureManager::getFetchedTexture(
                tid, FTT_DEFAULT, true, LLGLTexture::BOOST_HIGH);
            if (t)
            {
                // BOOST_TERRAIN + addTextureStats + forceToSaveRawImage still left them
                // pinned at the 16x16 placeholder: BOOST_TERRAIN is deliberately low
                // priority (LL never renders these under VK, so the fetch pipeline gives
                // them no decode demand). Raise to BOOST_HIGH -- which forces the decode
                // priority the way the sculpt path (BOOST_SCULPTED) does -- so they pull
                // down to full resolution.
                t->setBoostLevel(LLGLTexture::BOOST_HIGH);
                t->setKnownDrawSize(1024, 1024);
                t->addTextureStats(1024.f * 1024.f);
                t->forceToSaveRawImage(0, F32_MAX);   // discard 0 = full resolution
            }
        }
    }
    mdesc.base_color_factor[0] = mdesc.base_color_factor[1] =
        mdesc.base_color_factor[2] = mdesc.base_color_factor[3] = 1.f;
    mdesc.alpha_cutoff = -1.f;
    mdesc.color_all_slots = true;   // the 4 slots are color detail textures
    // PBR: the instance names a real glTF material (slot 0) instead of the packed
    // detail set -- those UUIDs are material assets, and resolving them as textures
    // would only fetch garbage. The renderer keeps all four record materials live.
    inst.material_id = pbr ? trec.mat[0] : LLCleanMaterialStore::instance().getOrCreate(mdesc);
    // #811: legacy terrain names its detail-texture material in the record too (legacy[3]
    // carries the id's bits; no shader reads the field otherwise), so the record alone
    // describes the region's surface: the terrain bake reads the material from it and its
    // input hash moves when the material does.
    if (!pbr) std::memcpy(&trec.legacy[3], &inst.material_id, sizeof(U32));
    inst.flags   = 32u;               // terrain splat mode (FS flag-32 branch)
    inst.skin_id = regionp ? LLCleanMaterialStore::instance().setTerrainRecord(regionp->getHandle(), trec)
                           : 0xFFFFFFFFu;   // no record: FS falls back to legacy defaults
    LLCleanInstanceStore::instance().upsert(patch_id, 0, inst, /*is_active=*/true);
}

// Clean-room water plane ingest (VK only). LLVOWater's captureWater runs inside the
// diverted LL rebuild path, so under VK nothing produces water geometry -- generate a
// flat region-sized quad grid at the region water height ourselves (mirrors the terrain
// ingest). Shaded by the FS flag-64 water branch; routed to the ALPHA bucket (flag bit1)
// so it blends translucently over whatever is below the surface.
static void vkIngestWaterPlane(LLViewerObject* obj)
{
    LLViewerRegion* regionp = obj->getRegion();
    if (!regionp) return;

    // Each LLVOWater object carries its OWN agent-space position and extent: the
    // region's own water object (256 m) PLUS the ring of void-water planes that
    // LLWorld::updateWaterObjects spawns around the agent (scale up to 8x256 m) to
    // cover off-sim area out to the horizon. Build the plane from THIS object's
    // position + scale, centered on its origin. The previous code slapped every
    // water object onto its region origin as a fixed 256 m quad, which (a) stacked
    // all the void-water planes on the agent region -> N overlapping alpha layers
    // read as opaque, and (b) meant void water never extended past the sim.
    const LLVector3 center = obj->getPositionAgent();
    F32 sx = obj->getScale().mV[0];
    F32 sy = obj->getScale().mV[1];
    const F32 sz = obj->getScale().mV[2];
    if (sx < 1.f) sx = 256.f;         // guard degenerate scale
    if (sy < 1.f) sy = 256.f;
    // LL's water vertical convention (LLVOWater::updateGeometry): the flat surface
    // sits at getPositionAgent().z - getScale().z*0.5. Region water has z-scale 0
    // (surface == position). Hole/void water is positioned at 256+water_height with
    // z-scale 512, so the -256 offset drops it back to water_height. Without this
    // the void-water planes ingest 256 m too high (up in the sky, off-screen).
    const F32 surface_z = center.mV[2] - sz * 0.5f;

    const S32 GN = 8;                 // quads per edge (grid for future wave displacement)
    const S32 N  = GN + 1;
    const U32 nv = (U32)(N * N);
    const U32 ni = (U32)(GN * GN * 6);
    std::vector<LLVector3> pos(nv), nrm(nv);
    std::vector<LLVector2> uv(nv);
    std::vector<U16>       idx(ni);
    for (S32 j = 0; j < N; ++j)
    for (S32 i = 0; i < N; ++i)
    {
        // Model-local, centered on the object origin; the instance model_matrix
        // translates to the object's agent-space center below.
        const F32 x = ((F32)i / (F32)GN - 0.5f) * sx;
        const F32 y = ((F32)j / (F32)GN - 0.5f) * sy;
        const S32 v = j * N + i;
        pos[v].set(x, y, 0.f);
        nrm[v].set(0.f, 0.f, 1.f);
        uv[v].set((F32)i / (F32)GN, (F32)j / (F32)GN);
    }
    U32 k = 0;
    for (S32 j = 0; j < GN; ++j)
    for (S32 i = 0; i < GN; ++i)
    {
        const U16 a = (U16)(j * N + i);
        const U16 b = (U16)(j * N + i + 1);
        const U16 c = (U16)((j + 1) * N + i);
        const U16 d = (U16)((j + 1) * N + i + 1);
        idx[k++] = a; idx[k++] = b; idx[k++] = c;
        idx[k++] = b; idx[k++] = d; idx[k++] = c;
    }

    const LLUUID wid = obj->getID();
    LLCleanMeshAsset::instance().captureWater(
        wid, 0, pos.data(), nrm.data(), uv.data(), idx.data(), nv, ni, /*reclaimable=*/true);

    LLCleanInstanceStore::Instance inst{};
    // Identity rotation + translate to the water object's agent-space center. The
    // cull transforms the model-space AABB by this matrix, and treats column 3 as
    // the object origin (#29 origin-visible override), so a real center is correct.
    inst.model_matrix[0] = inst.model_matrix[5] = inst.model_matrix[10] = inst.model_matrix[15] = 1.f;
    inst.model_matrix[12] = center.mV[0];
    inst.model_matrix[13] = center.mV[1];
    inst.model_matrix[14] = surface_z;
    inst.aabb_min[0] = -sx * 0.5f; inst.aabb_min[1] = -sy * 0.5f; inst.aabb_min[2] = -8.f;
    inst.aabb_max[0] =  sx * 0.5f; inst.aabb_max[1] =  sy * 0.5f; inst.aabb_max[2] =  8.f;
    inst.tint[0] = inst.tint[1] = inst.tint[2] = inst.tint[3] = 1.f;
    inst.uv_matrix[0] = inst.uv_matrix[5] = inst.uv_matrix[10] = inst.uv_matrix[15] = 1.f;
    const LLCleanMeshAsset::Slot ms = LLCleanMeshAsset::instance().getSlot(wid, 0, 0);
    inst.lod_group_id = 0xFFFFFFFFu;
    if (ms.valid)
        for (S32 _l = 0; _l < LLCleanMeshAsset::NUM_LOD; ++_l)
            inst.lod_group_id = LLCleanMeshAsset::instance().registerLodSlot(wid, 0, _l, ms.slot_id);
    inst.aabb_min[3] = 0.f;
    LLCleanMaterialStore::MaterialDesc mdesc;   // procedural water color -> no textures
    mdesc.base_color_factor[0] = mdesc.base_color_factor[1] =
        mdesc.base_color_factor[2] = mdesc.base_color_factor[3] = 1.f;
    mdesc.alpha_cutoff = -1.f;
    inst.material_id = LLCleanMaterialStore::instance().getOrCreate(mdesc);
    inst.flags   = 2048u | 2u;        // water branch (bit11) + alpha bucket (bit1)
                                      // NB: bit6 (64u) is TEX_GEN_PLANAR on regular
                                      // objects (llvovolume.cpp) — do NOT reuse it.
    inst.skin_id = 0xFFFFFFFFu;
    LLCleanInstanceStore::instance().upsert(wid, 0, inst, /*is_active=*/true);
}

// ---- LINDEN TREES + GRASS (VK, #20) ----
// Instance flag bit 15: vegetation. The cull gives it LL's tree trunk-LOD rule and the
// Trees / Grass render-type toggles instead of the Volumes ones. uv_matrix ROW 3
// (elements 3, 7, 11, 15 -- ignored by every UV transform, which only reads .xy of
// uv_matrix * vec4(uv, 0, 1)) carries the LOD inputs:
//   tree : (dist_lt0, dist_lt1, dist_lt2, detail_k) -- trunk LOD j while the origin
//          distance < dist_lt[j] (LL's range thresholds + LL radius), capped by
//          detailFromTan(detail_k / distance); see LLVOTree::vkLODParams.
//   grass: (0, 0, 0, -1)  -- detail_k < 0 marks grass (one slot for every level).
static constexpr U32 VK_VEGETATION_FLAG = 32768u;

static bool vegetationSigUpToDate(const LLUUID& id, U64 sig)
{
    return LLCleanInstanceStore::instance().isUpToDate(id, 0, sig);
}

static U64 vegetationSig(std::initializer_list<F32> fs, U64 seed)
{
    U64 h = 0xcbf29ce484222325ull ^ seed;
    for (F32 f : fs) { U32 u; std::memcpy(&u, &f, 4); h ^= u; h *= 0x100000001b3ull; }
    return h ? h : 1ull;   // 0 = "no signature"
}

// LL's tree mesh depends only on the species (the per-tree scale / rotation / base are
// baked in by updateMesh -- here they are the instance matrix). Each species' four
// trunk LODs are captured ONCE as shared slots (captureTerrain storage, U16 indices
// like LL; never GC'd) and published in one LOD group per species: group level 3-k =
// trunk LOD k (LL: 0 = best; cull: 3 = best).
struct VKTreeSpeciesGeom
{
    bool valid = false;
    U32  group_id = 0xFFFFFFFFu;
    F32  lo[3] = { 0.f, 0.f, 0.f };
    F32  hi[3] = { 0.f, 0.f, 0.f };
};

static const VKTreeSpeciesGeom& vkTreeSpeciesGeom(S32 species)
{
    static std::unordered_map<S32, VKTreeSpeciesGeom> s_geom;
    auto it = s_geom.find(species);
    if (it != s_geom.end()) return it->second;
    VKTreeSpeciesGeom& g = s_geom[species];
    LLCleanMeshAsset& ma = LLCleanMeshAsset::instance();
    const LLUUID group_key = LLUUID::generateNewID(llformat("slipstream-vk-tree-group-%d", species));
    F32 lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    std::vector<LLVector3> pos, nrm;
    std::vector<LLVector2> uv;
    std::vector<U16>       idx;
    U32 gid = 0xFFFFFFFFu;
    for (S32 k = 0; k < LLCleanMeshAsset::NUM_LOD; ++k)
    {
        if (!LLVOTree::buildLocalMesh(species, k, pos, nrm, uv, idx)) continue;
        const LLUUID lod_id = LLUUID::generateNewID(llformat("slipstream-vk-tree-%d-%d", species, k));
        ma.captureTerrain(lod_id, 0, pos.data(), nrm.data(), uv.data(), nullptr, idx.data(),
                          (U32)pos.size(), (U32)idx.size());
        const LLCleanMeshAsset::Slot ms = ma.getSlot(lod_id, 0, 0);
        if (!ms.valid) continue;
        gid = ma.registerLodSlot(group_key, 0, LLCleanMeshAsset::NUM_LOD - 1 - k, ms.slot_id);
        for (const LLVector3& p : pos)
            for (int c = 0; c < 3; ++c) { lo[c] = llmin(lo[c], p.mV[c]); hi[c] = llmax(hi[c], p.mV[c]); }
    }
    if (gid != 0xFFFFFFFFu)
    {
        g.valid = true;
        g.group_id = gid;
        for (int c = 0; c < 3; ++c) { g.lo[c] = lo[c]; g.hi[c] = hi[c]; }
    }
    if (!g.valid)
    {
        // Unknown / over-U16 species (LL could not draw it either) or the mesh store not
        // ready yet: nothing cached, a later visit retries.
        s_geom.erase(species);
        static const VKTreeSpeciesGeom s_invalid;
        return s_invalid;
    }
    return g;
}

// The species texture (bark + leaf atlas / grass sprite) as an alpha-mask material at
// LL's 0.5 cutoff. BOOST_NONE textures stream from the cull's texel demand
// (setVKDemand), so no manual boost is needed.
static U32 vkVegetationMaterial(const LLUUID& tex_id)
{
    LLCleanMaterialStore::MaterialDesc mdesc;
    mdesc.base_color_id = tex_id;
    mdesc.alpha_cutoff = 0.5f;
    return LLCleanMaterialStore::instance().getOrCreate(mdesc);
}

static void vkIngestTree(LLViewerObject* obj)
{
    LLVOTree* tree = dynamic_cast<LLVOTree*>(obj);
    if (!tree || !tree->getRegion()) return;
    const S32 species = tree->getSpecies();
    const LLVOTree::TreeSpeciesData* sp = LLVOTree::getSpeciesData(species);
    if (!sp) return;

    const LLQuaternion q = tree->getRotation();
    const LLVector3 sc = tree->getScale();
    // #568: the signature takes the REGION-local position (crossing-invariant); agent space
    // changes on every region crossing and re-ingested every tree for nothing.
    const LLVector3 pr = tree->getPositionRegion();
    const U64 sig = vegetationSig({ pr.mV[0], pr.mV[1], pr.mV[2], q.mQ[0], q.mQ[1], q.mQ[2], q.mQ[3],
                                    sc.mV[0], sc.mV[1], sc.mV[2], LLVOTree::sTreeFactor,
                                    LLVOVolume::sDistanceFactor }, 0x7EE0000ull + (U64)species);
    if (vegetationSigUpToDate(tree->getID(), sig)) return;

    const VKTreeSpeciesGeom& g = vkTreeSpeciesGeom(species);
    if (!g.valid) return;

    LLCleanInstanceStore::Instance inst{};
    const LLMatrix4 m = tree->treeLocalToAgent();
    std::memcpy(inst.model_matrix, m.mMatrix, sizeof(inst.model_matrix));
    // Bounds over all four LOD meshes; w = bounding radius about the origin, agent
    // units (RT TLAS reach). LL's trunk LOD metric lives in uv_matrix row 3.
    F32 r2 = 0.f;
    for (int c = 0; c < 3; ++c)
    {
        inst.aabb_min[c] = g.lo[c];
        inst.aabb_max[c] = g.hi[c];
        const F32 e = llmax(fabsf(g.lo[c]), fabsf(g.hi[c]));
        r2 += e * e;
    }
    inst.aabb_min[3] = sqrtf(r2) * sc.magVec() * 0.05f;
    inst.aabb_max[3] = 0.f;
    inst.tint[0] = inst.tint[1] = inst.tint[2] = inst.tint[3] = 1.f;
    inst.uv_matrix[0] = inst.uv_matrix[5] = inst.uv_matrix[10] = 1.f;
    F32 range_lt[3], detail_k, ll_radius;
    tree->vkLODParams(range_lt, detail_k, ll_radius);
    // range = distance - radius < t  <=>  distance < t + radius
    inst.uv_matrix[3]  = range_lt[0] + ll_radius;
    inst.uv_matrix[7]  = range_lt[1] + ll_radius;
    inst.uv_matrix[11] = range_lt[2] + ll_radius;
    inst.uv_matrix[15] = detail_k;
    inst.lod_group_id = g.group_id;
    inst.material_id  = vkVegetationMaterial(sp->mTextureID);
    // Mask bucket. NOT double-sided: LL's leaves already carry both windings (with
    // flipped normals) and back-face culling keeps one.
    inst.flags   = 4u | VK_VEGETATION_FLAG;
    inst.skin_id = 0xFFFFFFFFu;
    LLCleanInstanceStore::instance().upsert(tree->getID(), 0, inst, /*is_active=*/false, sig);
}

static void vkIngestGrass(LLViewerObject* obj)
{
    LLVOGrass* grass = dynamic_cast<LLVOGrass*>(obj);
    if (!grass) return;
    LLViewerRegion* regionp = grass->getRegion();
    if (!regionp) return;
    const LLUUID& tex_id = grass->getSpeciesTextureID();
    if (tex_id.isNull()) return;

    // Blade bases sit on the terrain: re-ingest when the land patch under the object
    // updates (LL: idleUpdate -> REBUILD_VOLUME on getLastUpdateTime change).
    const LLVector3 pa = grass->getPositionAgent();
    const LLVector3 sc = grass->getScale();
    const LLSurfacePatch* patchp = regionp->getLand().resolvePatchRegion(grass->getPositionRegion());
    const U64 pt = patchp ? patchp->getLastUpdateTime() : 0ull;
    const LLVector3 pr = grass->getPositionRegion();   // #568: crossing-invariant signature
    const U64 sig = vegetationSig({ pr.mV[0], pr.mV[1], pr.mV[2], sc.mV[0], sc.mV[1], sc.mV[2],
                                    (F32)(pt & 0xFFFFFFull), (F32)((pt >> 24) & 0xFFFFFFull) },
                                  0x6A55000ull + (U64)grass->mSpecies);
    if (vegetationSigUpToDate(grass->getID(), sig)) return;

    std::vector<LLVector3> pos, nrm;
    std::vector<LLVector2> uv;
    std::vector<U16>       idx;
    if (!grass->buildLocalBlades(pos, nrm, uv, idx)) return;
    const LLUUID gid = grass->getID();
    LLCleanMeshAsset::instance().captureTerrain(gid, 0, pos.data(), nrm.data(), uv.data(), nullptr,
                                                idx.data(), (U32)pos.size(), (U32)idx.size(),
                                                /*force=*/false, /*tight=*/false, /*reclaimable=*/true);
    const LLCleanMeshAsset::Slot ms = LLCleanMeshAsset::instance().getSlot(gid, 0, 0);
    if (!ms.valid) return;

    LLCleanInstanceStore::Instance inst{};
    inst.model_matrix[0] = inst.model_matrix[5] = inst.model_matrix[10] = inst.model_matrix[15] = 1.f;
    inst.model_matrix[12] = pa.mV[0];
    inst.model_matrix[13] = pa.mV[1];
    inst.model_matrix[14] = pa.mV[2];
    F32 lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    for (const LLVector3& p : pos)
        for (int c = 0; c < 3; ++c) { lo[c] = llmin(lo[c], p.mV[c]); hi[c] = llmax(hi[c], p.mV[c]); }
    F32 r2 = 0.f;
    for (int c = 0; c < 3; ++c)
    {
        inst.aabb_min[c] = lo[c];
        inst.aabb_max[c] = hi[c];
        const F32 e = llmax(fabsf(lo[c]), fabsf(hi[c]));
        r2 += e * e;
    }
    inst.aabb_min[3] = sqrtf(r2);
    inst.aabb_max[3] = 0.f;
    inst.tint[0] = inst.tint[1] = inst.tint[2] = inst.tint[3] = 1.f;
    inst.uv_matrix[0] = inst.uv_matrix[5] = inst.uv_matrix[10] = 1.f;
    inst.uv_matrix[15] = -1.f;   // grass marker (see VK_VEGETATION_FLAG)
    // One slot for every level (LL's blade-count LOD is not applied: all 32 blades).
    inst.lod_group_id = 0xFFFFFFFFu;
    for (S32 l = 0; l < LLCleanMeshAsset::NUM_LOD; ++l)
        inst.lod_group_id = LLCleanMeshAsset::instance().registerLodSlot(gid, 0, l, ms.slot_id);
    inst.material_id = vkVegetationMaterial(tex_id);
    inst.flags   = 4u | VK_VEGETATION_FLAG;   // mask; both windings are in the mesh
    inst.skin_id = 0xFFFFFFFFu;
    LLCleanInstanceStore::instance().upsert(gid, 0, inst, /*is_active=*/false, sig);
}

static bool vkIsVegetation(const LLViewerObject* obj)
{
    const U32 pt = obj->getPartitionType();
    return pt == LLViewerRegion::PARTITION_TREE || pt == LLViewerRegion::PARTITION_GRASS;
}

static void vkIngestVegetation(LLViewerObject* obj)
{
    if (obj->getPartitionType() == LLViewerRegion::PARTITION_TREE) vkIngestTree(obj);
    else                                                           vkIngestGrass(obj);
}

// RenderVKHudDiag: throttled log of what the clean pipeline does with HUD pose /
// colour changes (default off).
static bool vkHudDiag()
{
    static LLCachedControl<bool> s_diag(gSavedSettings, "RenderVKHudDiag", false);
    return s_diag;
}
static bool vkHudDiagBudget()
{
    static U32 s_frame = 0, s_n = 0;
    if ((U32)gFrameCount != s_frame) { s_frame = (U32)gFrameCount; s_n = 0; }
    return ++s_n <= 16u;   // at most 16 lines per frame
}

void LLCleanIngest::noteObjectMoved(LLViewerObject* obj, bool easing, bool via_parent)
{
    if (!obj || obj->isDead()) return;
    // Trees / grass bake their pose into the instance at ingest: re-ingest (changed
    // queue; the signature skips it when nothing moved).
    if (vkIsVegetation(obj)) { noteObjectChanged(obj); return; }
    // Only volumes have instances (drainMovedQueue drops everything else). Avatars are
    // markMoved every frame: queueing their UUIDs just to reject them later was a
    // per-frame hash insert per avatar, and recursing into their children only
    // re-visited attachments the attachment rule below skips anyway.
    if (obj->getPCode() != LL_PCODE_VOLUME) return;
    // Worn attachments and HUDs ride the GPU joint table / HUD attach point: their
    // stored pose is RELATIVE to the attach point, so avatar motion (which markMoved's
    // every attachment, every frame) doesn't change it. What DOES change it: an edit
    // (selection), LL's easing of a scripted move, the parent's move (via_parent), or
    // a scripted pose change of this prim itself (llSetLinkPrimitiveParams on a HUD
    // menu part) -- detected by comparing the local pose with the last one seen. That
    // last case was dropped before, so HUD parts only updated after a right-click
    // (selection) let the move through.
    const bool local_changed = obj->vkLocalPoseChanged();   // always refresh the cache
    if (obj->isAttachment() && !easing && !via_parent)
    {
        LLViewerObject* root = obj->getRootEdit();
        const bool selected = obj->isSelected() || (root && root->isSelected());
        // <GMDX> HUDs: every frame, as selection does. A HUD child's row is composed from its own
        // local pose, the ROOT's drawn pose and the attach point's matrix; only the first was
        // watched, so a root / attach-point change after the child's last refresh (a HUD script
        // moving the root just after attaching, an aspect change, a drawable catching up) left the
        // children drawn at the old pose until a right-click. HUDs are a few dozen prims, and
        // refreshObjectPose dirties a row only when its matrix actually changed.
        if (!selected && !local_changed && !obj->isHUDAttachment()) return;
        if (vkHudDiag() && obj->isHUDAttachment() && vkHudDiagBudget())
        {
            LL_INFOS("VKHudDiag") << "moved " << obj->getID() << (selected ? " selected" : "")
                << (local_changed ? " local-pose-changed" : "") << " pos=" << obj->getPosition()
                << " rot=" << obj->getRotation() << " scale=" << obj->getScale() << LL_ENDL;
        }
    }
    if (mMovedSet.insert(obj->getID()).second) mMovedQueue.push_back(obj->getID());
    // Linkset children inherit the root's motion without their own markMoved.
    for (LLViewerObject* child : obj->getChildren())
        noteObjectMoved(child, easing, /*via_parent=*/true);
}

void LLCleanIngest::noteHudLayoutChanged()
{
    if (!isAgentAvatarValid()) return;
    U32 n = 0;
    for (const auto& kv : gAgentAvatarp->mAttachmentPoints)
    {
        LLViewerJointAttachment* ap = kv.second;
        if (!ap || !ap->getIsHUDAttachment()) continue;
        for (const auto& o : ap->mAttachedObjects)
        {
            LLViewerObject* obj = o.get();
            if (!obj || obj->isDead()) continue;
            // via_parent: past the attachment rule (the local pose did not change, the
            // attach point did); recurses into the linkset's children.
            noteObjectMoved(obj, /*easing=*/false, /*via_parent=*/true);
            ++n;
        }
    }
    LL_INFOS("VKHud") << "HUD layout changed: re-posing " << n << " HUD attachment(s)" << LL_ENDL;
}

void LLCleanIngest::noteTerrainDirty(LLViewerObject* patch)
{
    if (!patch || patch->isDead()) return;
    if (mTerrainDirtySet.insert(patch->getID()).second) mTerrainDirty.push_back(patch->getID());
}

void LLCleanIngest::drainTerrainDirty()
{
    // Terrain texture-scale settings live (#39). They are read only when a patch is
    // ingested, into the per-REGION TerrainRecord (UV scale / PBR transform; the patch
    // geometry doesn't depend on them), so a change queues ONE patch per region: its
    // re-ingest rewrites the region's record, which every patch of the region reads.
    // Nothing re-ingests while the values hold.
    {
        static LLCachedControl<F32> s_scale(gSavedSettings, "RenderTerrainScale", 12.f);
        static LLCachedControl<F32> s_pbr_scale(gSavedSettings, "RenderTerrainPBRScale", 8.f);
        static LLCachedControl<S32> s_pbr_detail(gSavedSettings, "RenderTerrainPBRDetail", 0);
        static F32 s_last_scale = s_scale, s_last_pbr_scale = s_pbr_scale;
        static S32 s_last_pbr_detail = s_pbr_detail;
        if ((F32)s_scale != s_last_scale || (F32)s_pbr_scale != s_last_pbr_scale
            || (S32)s_pbr_detail != s_last_pbr_detail)
        {
            s_last_scale = s_scale; s_last_pbr_scale = s_pbr_scale; s_last_pbr_detail = s_pbr_detail;
            std::unordered_set<U64> regions;
            for (S32 i = 0, n = gObjectList.getNumObjects(); i < n; ++i)
            {
                LLViewerObject* o = gObjectList.getObject(i);
                if (o && !o->isDead() && o->getPartitionType() == LLViewerRegion::PARTITION_TERRAIN
                    && o->getRegion() && regions.insert(o->getRegion()->getHandle()).second)
                    noteTerrainDirty(o);
            }
            LL_INFOS("VKIngest") << "terrain scale settings changed: refreshing the terrain record of "
                                 << regions.size() << " region(s)" << LL_ENDL;
        }
    }
    if (mTerrainDirty.empty()) return;
    VK_HITCH_SCOPE("terrainDirty");
    // Bounded: a region's worth of layer data can dirty all 256 patches at once.
    constexpr size_t MAX_PER_FRAME = 64;
    const size_t n = std::min(MAX_PER_FRAME, mTerrainDirty.size());
    for (size_t i = 0; i < n; ++i)
    {
        const LLUUID& id = mTerrainDirty[i];
        mTerrainDirtySet.erase(id);
        if (LLViewerObject* obj = gObjectList.findObject(id))
            if (!obj->isDead()) vkIngestTerrainPatch(obj);
    }
    mTerrainDirty.erase(mTerrainDirty.begin(), mTerrainDirty.begin() + n);
    LLVKHitch::count("terrainPatchesDirty", (unsigned)n);
}

void LLCleanIngest::noteObjectChanged(LLViewerObject* obj)
{
    if (!obj || obj->isDead()) return;
    if (obj->getPCode() != LL_PCODE_VOLUME && !vkIsVegetation(obj)) return;
    if (vkHudDiag() && obj->isHUDAttachment() && vkHudDiagBudget())
    {
        std::ostringstream os;
        for (U8 i = 0; i < obj->getNumTEs() && i < 8; ++i)
            if (const LLTextureEntry* te = obj->getTE(i)) os << " a" << (int)i << "=" << te->getColor().mV[3];
        LL_INFOS("VKHudDiag") << "changed " << obj->getID() << os.str() << LL_ENDL;
    }
    // C1: the next sweep visit runs the heavy pre-steps even before the queue drains.
    {
        auto sit = mObjSig.find(obj->getID());
        if (sit != mObjSig.end()) sit->second.pre = 0;
    }
    if (!mChangedSet.insert(obj->getID()).second) return;
    if (obj->isHUDAttachment()) mChangedQueue.push_front(obj->getID());   // interactive: first
    else                        mChangedQueue.push_back(obj->getID());
}

// ANIMESH CONTROL AVATAR (VK). LL creates an animated object's control avatar only in
// LLVolumeGeometryManager::rebuildGeom (diverted under VK) or on the ObjectAnimation
// message -- which no-ops if the mesh/skin hasn't loaded yet. So an animesh whose mesh
// arrives after its animation message (common for other people's attachments) never got
// one: its rigged faces skinned to the WEARER's skeleton (rendered offset) until an
// unrelated rebuild. Do rebuildGeom's step here, with the same readiness gates.
static void vkEnsureControlAvatar(LLViewerObject* obj)
{
    if (!obj || obj->isDead()) return;
    LLVOVolume* vobj = dynamic_cast<LLVOVolume*>(obj);
    if (!vobj) return;
    LLViewerObject* root = vobj->getRootEdit();
    if (!root || !root->isAnimatedObject() || vobj->getControlAvatar()) return;
    if (vobj->isMesh())
    {
        if ((vobj->getVolume() && !vobj->getVolume()->isMeshAssetLoaded()) || !gMeshRepo.meshRezEnabled())
            return;   // waiting for the asset
        if (!vobj->getSkinInfo() && !vobj->isSkinInfoUnavaliable())
            return;   // waiting for skin info
    }
    vobj->updateControlAvatar();
}

// C1 (RenderVKSweepPreSig): pre-signature gate on the sweep's heavy pre-steps, and the
// "no LOD band changed" idle rule. 0 = the old behaviour.
static bool vkSweepPreSigOn()
{
    static LLCachedControl<bool> s_on(gSavedSettings, "RenderVKSweepPreSig", true);
    return s_on;
}

// Bounding spheres (agent space: x, y, z, r) of objects moved / re-registered this
// frame, consumed (and cleared) by the VK renderer to invalidate cached projector
// shadow tiles. Bounded: past the cap it just flags an overflow (-> re-render all).
std::vector<float> gVKChangedSpheres;
// #567: the producer of each sphere (parallel to gVKChangedSpheres, one per sphere) for
// the renderer's per-producer tile-invalidation counters (projDirtBy*). Values:
// 0 changed queue (re-registration), 1 moved queue (pose changed), 2 seat linkset,
// 3 vegetation re-ingest, 4 glTF scene re-ingest, 5 mover class transition,
// 6 instance store registration (new / released / re-shaped instance, old + new row).
std::vector<uint8_t> gVKChangedSphereTags;
bool gVKChangedOverflow = false;
// C11: false while no projector-shadow tile is in use (set each frame by the renderer's
// invalidateProjShadows, which clears the list unread then): no spheres to collect. A
// tile that comes into use renders its static layer in full anyway.
bool gVKProjShadowsUsed = true;
enum : uint8_t { kSphChanged = 0, kSphMoved = 1, kSphSeat = 2, kSphVeg = 3 };   // (4 glTF, 5 mover)
// #567 dynamic shadow casters (LLCleanIngest::mDynCasters): agent-space bounding spheres
// (x, y, z, r) of every object currently in the dynamic class, rebuilt each frame by
// tickDynCasters; the VK renderer marks the projector tiles they touch as needing the
// per-frame overlay. Transitions into / out of the class go to gVKChangedSpheres (tag 5).
std::vector<float> gVKDynSpheres;
// #771 (RT sun shadow cache): parallel to gVKDynSpheres, 1 when the caster MOVED recently
// (a GPU spinner, or a pose change noted within the last two frames) -- a dynamic-class
// object settling (still for up to RenderVKShadowMoverSettleSecs) casts a static shadow.
std::vector<uint8_t> gVKDynSphereMoving;
static constexpr uint8_t kSphMover = 5;
// RenderVKShadowMoverSettleSecs: seconds an object must be still before it rejoins the
// static class (0 = no dynamic class: every move invalidates the static tiles around it).
static F32 vkDynCasterSettleSecs()
{
    static LLCachedControl<F32> s_secs(gSavedSettings, "RenderVKShadowMoverSettleSecs", 2.f);
    return llmax((F32)s_secs, 0.f);
}
static void noteChangedSphere(LLViewerObject* obj, uint8_t tag)
{
    if (gVKChangedOverflow || !gVKProjShadowsUsed) return;
    if (gVKChangedSpheres.size() >= 4 * 8192)
    {
        gVKChangedOverflow = true; gVKChangedSpheres.clear(); gVKChangedSphereTags.clear();
        return;
    }
    const LLVector3 p = obj->getPositionAgent();
    const F32 r = obj->mDrawable.notNull() ? llmax(obj->mDrawable->getRadius(), obj->getScale().length() * 0.5f)
                                           : obj->getScale().length() * 0.5f;
    gVKChangedSpheres.insert(gVKChangedSpheres.end(), { p.mV[0], p.mV[1], p.mV[2], r });
    gVKChangedSphereTags.push_back(tag);
    static const char* const kNames[] = { "projSphChanged", "projSphMoved", "projSphSeat", "projSphVeg",
                                          "projSphGltf", "projSphMover" };
    LLVKHitch::count(kNames[tag < 6 ? tag : 0]);
}

// #772: reflection-probe / planar-mirror change spheres for APPEARANCE changes -- the
// changed queue's re-registrations (colour, texture, material, any property): not
// shadow-relevant, so absent from gVKChangedSpheres while RenderVKProjShadowRegSpheres is
// on. Collected only while the VK renderer wants them (gVKProbeSpheresWanted, set per frame
// by setProbes), consumed and cleared by it (prbGatherChanges). Bounded: past the cap only
// an overflow flag (-> every probe content-dirty).
// Classification (gVKProbeSphereTags, parallel): 0 static appearance change, 1 vegetation,
// 2 DYNAMIC (the object is in the mover class: its change is motion, not a static edit --
// the renderer holds it as moving dynamic content). Worn attachments and animesh linksets
// emit nothing: they are avatar content (the renderer's avatar entries; only Dynamic probes
// draw them). gVKProbeSphereStats (cumulative, for the renderer's log): [0] static, [1]
// vegetation, [2] mover -> dynamic, [3] attachment skipped, [4] animesh skipped,
// [5] appearance updates compared, [6] of those unchanged (no sphere).
// ONLY REAL CHANGES: sims resend full object updates (movers, scripts setting the same
// colour / glow, texture-animated prims) that queue a re-registration without changing
// anything visible. A per-object VISIBLE-APPEARANCE signature (vkAppearanceSig: per face
// texture, colour / alpha, glow, fullbright / bump / shiny, texgen, media, texture
// transform, legacy material id, glTF material + override hashes) is compared with the
// last one; equal -> no sphere. Texture animation (llSetTextureAnim) is not in the TE
// state, so it never emits. An object's first visit emits (conservative: an unseen
// object's first edit). Bounded: dead objects are pruned every ~10 s, cap 128k entries.
std::vector<float> gVKProbeSpheres;
std::vector<uint8_t> gVKProbeSphereTags;
bool gVKProbeSpheresOverflow = false;
bool gVKProbeSpheresWanted = false;
uint32_t gVKProbeSphereStats[8] = {};
static std::unordered_map<LLUUID, U64> s_probeAppSig;
static U64 vkAppearanceSig(LLViewerObject* obj)
{
    U64 h = 0xcbf29ce484222325ull;
    auto mix = [&h](U64 v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); };
    auto mixf = [&mix](F32 f) { U32 u; std::memcpy(&u, &f, 4); mix(u); };
    const U8 nte = obj->getNumTEs();
    mix((U64)nte | ((U64)obj->getPCode() << 8));
    for (U8 i = 0; i < nte; ++i)
    {
        const LLTextureEntry* te = obj->getTE(i);
        if (!te) { mix(0ull); continue; }
        mix(te->getID().getDigest64());
        const LLColor4& c = te->getColor();
        mixf(c.mV[0]); mixf(c.mV[1]); mixf(c.mV[2]); mixf(c.mV[3]);
        F32 a, b;
        te->getScale(&a, &b); mixf(a); mixf(b);
        te->getOffset(&a, &b); mixf(a); mixf(b);
        mixf(te->getRotation()); mixf(te->getGlow());
        mix((U64)te->getBumpShinyFullbright() | ((U64)te->hasMedia() << 8) | ((U64)te->getTexGen() << 16)
            | ((U64)te->getFullbright() << 24));
        U64 mid = 0; std::memcpy(&mid, te->getMaterialID().get(), sizeof(mid)); mix(mid);
        if (const LLGLTFMaterial* m = te->getGLTFMaterial()) mix(m->getHash().getDigest64());
        else mix(1ull);
        if (const LLGLTFMaterial* o = te->getGLTFMaterialOverride()) mix(o->getHash().getDigest64());
        else mix(2ull);
        // BOM: a baked-image face shows the wearer's current bake
        if (LLAvatarAppearanceDefines::LLAvatarAppearanceDictionary::isBakedImageId(te->getID()))
        {
            LLViewerTexture* bake = obj->getBakedTextureForMagicId(te->getID());
            mix(bake ? (U64)bake->getID().getCRC32() : 0ull);
        }
    }
    return h;
}
// True when obj's visible appearance differs from its last recorded one (records it).
static bool vkAppearanceChanged(LLViewerObject* obj)
{
    ++gVKProbeSphereStats[5];
    const U64 sig = vkAppearanceSig(obj);
    if (s_probeAppSig.size() >= 131072) s_probeAppSig.clear();   // (runaway: everything re-emits once)
    auto ins = s_probeAppSig.try_emplace(obj->getID(), sig);
    if (!ins.second)
    {
        if (ins.first->second == sig) { ++gVKProbeSphereStats[6]; return false; }
        ins.first->second = sig;
    }
    return true;
}
// Drop the signatures of objects that are gone (every ~600 calls: a few thousand hash lookups).
static void vkAppearancePrune()
{
    static U32 s_tick = 0;
    if (++s_tick < 600u) return;
    s_tick = 0;
    if (!gVKProbeSpheresWanted) { s_probeAppSig.clear(); return; }
    for (auto it = s_probeAppSig.begin(); it != s_probeAppSig.end(); )
    {
        LLViewerObject* o = gObjectList.findObject(it->first);
        it = (!o || o->isDead()) ? s_probeAppSig.erase(it) : std::next(it);
    }
}
static void noteProbeSphere(LLViewerObject* obj, uint8_t tag)
{
    if (!gVKProbeSpheresWanted || gVKProbeSpheresOverflow || !obj || obj->isHUDAttachment()) return;
    if (obj->isAttachment()) { ++gVKProbeSphereStats[3]; return; }
    LLViewerObject* root = obj->getRootEdit();
    if (root && root->isAnimatedObject()) { ++gVKProbeSphereStats[4]; return; }
    if (!vkAppearanceChanged(obj)) return;   // a resent update: nothing visible changed
    if (gVKProbeSpheres.size() >= 4 * 8192)
    {
        gVKProbeSpheresOverflow = true; gVKProbeSpheres.clear(); gVKProbeSphereTags.clear();
        return;
    }
    const LLVector3 p = obj->getPositionAgent();
    const F32 r = obj->mDrawable.notNull() ? llmax(obj->mDrawable->getRadius(), obj->getScale().length() * 0.5f)
                                           : obj->getScale().length() * 0.5f;
    gVKProbeSpheres.insert(gVKProbeSpheres.end(), { p.mV[0], p.mV[1], p.mV[2], r });
    gVKProbeSphereTags.push_back(tag);
    ++gVKProbeSphereStats[tag < 3 ? tag : 0];
}

// Bounding sphere of an object for the dynamic-caster list: its own drawable / scale
// radius; a spinning linkset root covers its children (they spin about it).
static void vkDynSphere(LLViewerObject* obj, bool linkset, F32 out[4])
{
    const LLVector3 p = obj->getPositionAgent();
    F32 r = obj->mDrawable.notNull() ? llmax(obj->mDrawable->getRadius(), obj->getScale().length() * 0.5f)
                                     : obj->getScale().length() * 0.5f;
    if (linkset)
        for (LLViewerObject* c : obj->getChildren())
            if (c && !c->isDead() && !c->isAvatar())
                r = llmax(r, c->getPosition().length() + c->getScale().length() * 0.5f);
    out[0] = p.mV[0]; out[1] = p.mV[1]; out[2] = p.mV[2]; out[3] = llmax(r, 0.25f);
}

void LLCleanIngest::noteDynCaster(LLViewerObject* obj, bool spinner)
{
    const F64 now = LLFrameTimer::getElapsedSeconds();
    auto ins = mDynCasters.try_emplace(obj->getID());
    DynCaster& d = ins.first->second;
    d.last = now;
    d.frame = (U32)gFrameCount;
    d.spinner = d.spinner || spinner;
    if (!ins.second) return;
    // START of motion: out of the static layer (the static tiles holding it re-render
    // without it; from now on the overlay draws it).
    LLCleanInstanceStore& st = LLCleanInstanceStore::instance();
    st.setObjectDynamicCaster(obj->getID(), true);
    if (spinner)   // a spinning root: its children spin with it
        for (LLViewerObject* c : obj->getChildren())
            if (c && !c->isDead() && c->getPCode() == LL_PCODE_VOLUME) st.setObjectDynamicCaster(c->getID(), true);
    noteChangedSphere(obj, kSphMover);
    LLVKHitch::count("dynCasterStart");
}

void LLCleanIngest::noteSpinner(LLViewerObject* obj)
{
    if (!obj || obj->isDead() || !gVKProjShadowsUsed || vkDynCasterSettleSecs() <= 0.f) return;
    noteDynCaster(obj, true);
}

// <GMDX>
void LLCleanIngest::noteTexAnimCaster(LLViewerObject* obj)
{
    if (obj && !obj->isDead() && !obj->isAttachment()) mTexAnimCasters.insert(obj->getID());
}

bool LLCleanIngest::noteAnimated(LLViewerObject* obj)
{
    if (!obj || obj->isDead() || !gVKProjShadowsUsed || vkDynCasterSettleSecs() <= 0.f || obj->isAttachment())
        return false;
    noteDynCaster(obj, false);
    return true;
}

// #567: registration spheres (tag 6, LLCleanInstanceStore::takeShadowDirty) cover every
// SHADOW-relevant re-registration precisely (old + new row), so the changed queue's and
// the vegetation re-ingest's per-object spheres -- fired for any property change, a
// colour cycle included -- are redundant. RenderVKProjShadowRegSpheres 0 = both, as before.
static bool vkRegSpheresOn()
{
    static LLCachedControl<bool> s_on(gSavedSettings, "RenderVKProjShadowRegSpheres", true);
    return s_on;
}

// Once per frame (drainMovedQueue): expire settled casters (back into the static layer:
// the tiles around them re-render once WITH them), re-assert the bit on the live ones
// (instances registered while moving -- a new face, a LOD rebuild -- are created without
// it) and publish their spheres. Everything is released while no projector tile is in
// use (the static tiles are rebuilt from scratch when they come back).
void LLCleanIngest::tickDynCasters()
{
    gVKDynSpheres.clear();
    gVKDynSphereMoving.clear();
    // <GMDX> Texture-animated mask / blend casters move their shadow every frame: re-note
    // them each tick so they never settle into a cached static layer. Dropped once the
    // object is gone or its animation stops (then it settles like any mover).
    if (gVKProjShadowsUsed && vkDynCasterSettleSecs() > 0.f)
    {
        for (auto it = mTexAnimCasters.begin(); it != mTexAnimCasters.end();)
        {
            LLViewerObject* obj = gObjectList.findObject(*it);
            const LLVOVolume* vo = (obj && !obj->isDead()) ? dynamic_cast<LLVOVolume*>(obj) : nullptr;
            if (!vo || !vo->mTextureAnimp || !(vo->mTextureAnimp->mMode & LLTextureAnim::ON))
            {
                it = mTexAnimCasters.erase(it);
                continue;
            }
            // Kept alive without counting as motion: only its shadow changes (gVKDynSphereMoving 3).
            auto d = mDynCasters.find(*it);
            if (d == mDynCasters.end())
            {
                noteDynCaster(obj, false);
                d = mDynCasters.find(*it);
            }
            else
                d->second.last = LLFrameTimer::getElapsedSeconds();
            if (d != mDynCasters.end()) d->second.texanim = true;
            ++it;
        }
    }
    if (mDynCasters.empty()) return;
    LLCleanInstanceStore& st = LLCleanInstanceStore::instance();
    const F32 settle = vkDynCasterSettleSecs();
    const bool on = gVKProjShadowsUsed && settle > 0.f;
    const F64 now = LLFrameTimer::getElapsedSeconds();
    // (re-registrations of existing keys keep the bit in upsert; only brand-new keys of a
    // dynamic object -- a new face / LOD rebuild -- need this, so every 8th frame suffices)
    const bool reassert = (gFrameCount & 7u) == 0u;
    U32 n_live = 0, n_settled = 0;
    for (auto it = mDynCasters.begin(); it != mDynCasters.end();)
    {
        LLViewerObject* obj = gObjectList.findObject(it->first);
        const bool spinner = it->second.spinner;
        auto clearBits = [&](LLViewerObject* o) {
            st.setObjectDynamicCaster(it->first, false);
            if (spinner && o)
                for (LLViewerObject* c : o->getChildren())
                    if (c && c->getPCode() == LL_PCODE_VOLUME) st.setObjectDynamicCaster(c->getID(), false);
        };
        if (!obj || obj->isDead())
        {
            clearBits(nullptr);   // (its instances go with it; a kept row must not stay dynamic)
            it = mDynCasters.erase(it);
            continue;
        }
        if (!on || now - it->second.last > (F64)settle)
        {
            // SETTLED: back into the static layer -- the tiles around it bake it in.
            clearBits(obj);
            if (on) { noteChangedSphere(obj, kSphMover); ++n_settled; }
            it = mDynCasters.erase(it);
            continue;
        }
        if (reassert)
        {
            st.setObjectDynamicCaster(it->first, true);
            if (spinner)
                for (LLViewerObject* c : obj->getChildren())
                    if (c && !c->isDead() && c->getPCode() == LL_PCODE_VOLUME) st.setObjectDynamicCaster(c->getID(), true);
        }
        F32 s[4];
        vkDynSphere(obj, spinner, s);
        gVKDynSpheres.insert(gVKDynSpheres.end(), { s[0], s[1], s[2], s[3] });
        // (#771: 2 = a GPU spinner -- its sphere covers every rotation; 1 = moved within 2 frames;
        // <GMDX> 3 = still, texture-animated: its shadow changes, its looks don't need probes / mirrors)
        gVKDynSphereMoving.push_back(spinner ? 2u : (it->second.frame + 2u >= (U32)gFrameCount ? 1u
                                                     : it->second.texanim ? 3u : 0u));
        ++n_live;
        ++it;
    }
    LLVKHitch::count("dynCasters", n_live);
    LLVKHitch::count("dynCasterSettled", n_settled);
}

// #567: a moved-queue refresh that changed no instance row (refreshObjectPose false)
// changes nothing a shadow tile holds -- no sphere. RenderVKProjShadowSphereOnChange
// 0 = the old behaviour (every refresh invalidates the tiles around the object).
static bool vkSphereOnPoseChangeOnly()
{
    static LLCachedControl<bool> s_on(gSavedSettings, "RenderVKProjShadowSphereOnChange", true);
    return s_on;
}

// FACE BOUNDS + INCOMPLETE-OBJECT CHECK of a full ingest visit (selfRegisterSweep's
// per-object body), shared with drainChangedQueue (C9: the changed queue now records
// the object's fresh signature, so the sweep's next visit may skip it -- the drain does
// what that full visit would have done after its settled check).
static void vkRegenFaceBounds(LLDrawable* dr, S32 nf)
{
    // FACE BOUNDS (VK). A face's extents feed LLFace::calcPixelArea -> the texture
    // streamer's on-screen size for every texture on it. LL generates them only in
    // LLVOVolume::updateGeometry -> genBBoxes (the pipeline's rebuild queue, drained
    // by the diverted display()), so faces created after load -- a mesh that
    // arrived later, a rebuilt volume -- kept degenerate or STALE extents (bounds
    // of an earlier placeholder/LOD volume): wrong on-screen area, and their
    // textures stalled at low resolution, differently each run (LOD/load timing).
    // Regenerate them on every sweep visit so they track the live volume +
    // transform. Cheap (per-face extents transform plus the entry-data refresh). Rigged objects only when never bounded: their genBBoxes re-skins
    // the rigged volume on the CPU.
    if (LLVOVolume* bv = dr->getVOVolume())
    {
        if (bv->getVolume())
        {
            const bool rigged_obj = bv->isRiggedMesh();
            bool regen = !rigged_obj;
            for (S32 fi = 0; fi < nf && !regen; ++fi)
            {
                LLFace* f = dr->getFace(fi);
                regen = f && f->mExtents[0].equals3(f->mExtents[1]);
            }
            // Only bound a volume whose face extents exist (a mesh LOD still
            // loading has none yet -> inf/garbage bounds, face "off-screen").
            LLVolume* bvol = bv->getVolume();
            for (S32 i = 0; regen && i < bvol->getNumVolumeFaces(); ++i)
            {
                const LLVolumeFace& vf = bvol->getVolumeFace(i);
                regen = vf.mExtents && vf.mExtents[0].isFinite3() && vf.mExtents[1].isFinite3();
            }
            if (regen)
            {
                // As LLVOVolume::updateGeometry does: refresh the relative xform
                // FIRST. Face bounds = volume extents * mRelativeXform (for static
                // objects the full region-space transform), which is otherwise only
                // refreshed in updateGeometry -- never run under VK -- so it stayed
                // identity (faces landed at the region origin) or stale.
                if (!rigged_obj) bv->updateRelativeXform();
                // genVolumeBBoxes only computes while the drawable carries a
                // REBUILD_VOLUME/POSITION/RIGGED flag, which LL sets through
                // LLPipeline::markRebuild -- gated on the GL pipeline being
                // initialized, so under VK the flag is never set and genBBoxes
                // was a no-op (bounds stayed empty; which faces happened to carry
                // a flag from creation varied run to run). Raise it just for
                // this call.
                const bool had = dr->isState(LLDrawable::REBUILD_POSITION);
                if (!had) dr->setState(LLDrawable::REBUILD_POSITION);
                bv->genBBoxes(false);
                if (!had) dr->clearState(LLDrawable::REBUILD_POSITION);
            }
        }
    }
}

// INCOMPLETE OBJECTS ARE NOT SETTLED (VK, #88). The signature covers LL-side
// state only; a face still waiting on OUR side (decode pending/backoff, raw bytes
// not retained yet, capture refused) would otherwise be recorded as settled and
// skipped for up to kSigFullLaps laps (minutes at the idle pace) after its data
// arrived -- "doors appear late". Keep such objects on full visits every lap.
// Deterministic absences (face not in this LOD / beyond the volume) do settle.
// Returns true when a face still waits on OUR side (the object is not settled);
// `missing` counts the faces not ingested.
static bool vkObjectTransient(LLViewerObject* obj, LLDrawable* dr, S32 nf, U16& missing)
{
    missing = 0;
    bool transient = false;
    for (S32 fi = 0; fi < nf; ++fi)
    {
        const LLFace* f = dr->getFace(fi);
        if (!f) continue;
        const U8 o = f->mVKIngest;
        if (o == LLFace::VKI_INGESTED || o == LLFace::VKI_UP_TO_DATE) continue;
        ++missing;
        // VKI_NOT_DRAWN is a deterministic absence too: LL draws nothing for the face
        // (a worn / rigged invisiprim, the #500 bridge helper HUD, a glTF scene's
        // placeholder face), decided from TE texture / colour / material and object
        // identity -- all in the signature. Treating it as "waiting on our side" kept
        // every object with such a face unsettled forever: a full re-registration
        // every sweep lap, and (one worn object is enough) the self-attachment
        // priority pass never settled -- ~370 objects re-signed every frame (~1 ms).
        if (o != LLFace::VKI_FACE_BEYOND_VOLUME && o != LLFace::VKI_DECODE_LATCHED
            && o != LLFace::VKI_LOD_LACKS_FACE && o != LLFace::VKI_NOT_DRAWN)
            transient = true;
    }
    // A worn rigged mesh registered before OUR skin entry resolved (avatar not
    // live yet, skin info still streaming) got the no-skin sentinel: drawn
    // unskinned (bind pose at the attachment -- effectively invisible). The
    // LL-side signature doesn't change when the entry resolves later, so keep
    // the object unsettled until it has; registration then picks up the id.
    if (!transient)
    {
        LLVOVolume* rv = dr->getVOVolume();
        const LLMeshSkinInfo* sk = rv ? rv->getSkinInfo() : nullptr;
        if (sk && obj->isAttachment() && !rv->isAnimatedObject() && obj->getAvatar()
            && LLCleanSkinStore::instance().isPending(obj->getAvatar(), sk))
            transient = true;
    }
    return transient;
}

static U64 vkIngestSignature(LLViewerObject* obj, LLDrawable* dr);   // (below, with the sweep)
extern void llvkTEStashStats(std::ostream& os);                      // (llvovolume.cpp)

void LLCleanIngest::drainChangedQueue()
{
    if (mChangedQueue.empty()) return;
    VK_HITCH_SCOPE("changedQueue");
    constexpr size_t MAX_PER_FRAME = 96;
    // C9: a time budget on top of the count cap (a burst of changes -- a region of
    // scripted movers, an outfit change -- re-registered up to 96 objects in one frame),
    // with a floor so the queue (HUD objects at its front) always progresses.
    // RenderVKChangedQueueBudgetMs 0 = the count cap only.
    // #534: the floor is ONE object. A floor of 8 overrode the budget whenever the
    // front of the queue held heavy objects (8 re-registrations = 6.9 ms in one frame);
    // one per frame still guarantees progress, and HUD entries sit at the front.
    static LLCachedControl<F32> s_budget_ms(gSavedSettings, "RenderVKChangedQueueBudgetMs", 1.f);
    constexpr size_t MIN_PER_FRAME = 1;
    const F64 budget_s = (F64)llmax(0.f, (F32)s_budget_ms) * 0.001;
    // C9: record the object's FRESH signature after registering it here (and do the
    // bounds / incomplete-object steps a full sweep visit would), instead of erasing it:
    // the erase made the next sweep visit re-register the whole object again.
    // RenderVKChangedQueueKeepSig 0 = erase (old behaviour).
    static LLCachedControl<bool> s_keep_sig(gSavedSettings, "RenderVKChangedQueueKeepSig", true);
    const bool keep_sig = s_keep_sig;
    LLTimer timer;
    size_t n = 0;
    vkAppearancePrune();   // #772 (once per drain)
    while (!mChangedQueue.empty() && n < MAX_PER_FRAME)
    {
        if (budget_s > 0.0 && n >= MIN_PER_FRAME && timer.getElapsedTimeF64() > budget_s) break;
        const LLUUID id = mChangedQueue.front();
        mChangedQueue.pop_front();
        mChangedSet.erase(id);
        mObjSig.erase(id);   // next sweep visit is a full one (unless C9 records it below)
        LLViewerObject* obj = gObjectList.findObject(id);
        if (!obj || obj->isDead()) continue;
        if (vkIsVegetation(obj))
        {
            ++n;
            if (!vkRegSpheresOn()) noteChangedSphere(obj, kSphVeg);
            else noteProbeSphere(obj, 1);   // #772 (appearance changes)
            vkIngestVegetation(obj);
            continue;
        }
        LLDrawable* dr = obj->mDrawable;
        if (!dr || dr->isDead() || !obj->getVolume()) continue;
        if (LLCleanGLTFIngest::isGLTFScene(obj))
        {
            ++n;
            LLCleanGLTFIngest::instance().ingestObject(obj);   // #39: asset instances, not faces
            continue;
        }
        if (LLCleanGLTFIngest::instance().isTracked(id))
            LLCleanGLTFIngest::instance().forgetObject(id);   // left glTF state: faces below
        ++n;
        if (!vkRegSpheresOn()) noteChangedSphere(obj, kSphChanged);
        else if (gVKProbeSpheresWanted)   // #772 (appearance changes; a mover's are motion)
        {
            const LLViewerObject* root = obj->getRootEdit();
            const bool mover = mDynCasters.count(obj->getID()) != 0u
                            || (root && mDynCasters.count(root->getID()) != 0u);
            noteProbeSphere(obj, mover ? 2 : 0);
        }
        vkEnsureControlAvatar(obj);
        // C9: the signature of the state registered below (the sweep computes it before
        // its registration too). `pre` stays 0 (noteObjectChanged), so the next sweep
        // visit still runs its heavy pre-steps; a LOD / volume change they make changes
        // the signature and takes a full visit.
        const U64 sig = keep_sig ? vkIngestSignature(obj, dr) : 0ull;
        const S32 nf = dr->getNumFaces();
        if (keep_sig) vkRegenFaceBounds(dr, nf);
        for (S32 fi = 0; fi < nf; ++fi)
            if (LLFace* facep = dr->getFace(fi))
                LLCleanIngest::registerCleanFace(facep, /*force_reingest=*/false);
        if (keep_sig)
        {
            U16 missing = 0;
            const bool transient = vkObjectTransient(obj, dr, nf, missing);
            ObjSig& os = mObjSig[id];
            // Mid-ease (LL gliding a scripted move) or a face still waiting on our side:
            // not settled (as the sweep's full visit).
            os.sig = (transient || dr->isState(LLDrawable::ON_MOVE_LIST)) ? 0ull : sig;
            os.lap = mSweepLap;
            os.full_frame = (U32)gFrameCount;
            os.missing = missing;
        }
    }
    LLVKHitch::count("changedObjects", (unsigned)n);
    if (!mChangedQueue.empty()) LLVKHitch::count("changedQueueLeft", (unsigned)mChangedQueue.size());
}

// RenderVKMoverDiag: per-object pose-change counts near the camera, summarised every 2 s.
static bool vkMoverDiagOn()
{
    static LLCachedControl<bool> s_on(gSavedSettings, "RenderVKMoverDiag", false);
    return s_on;
}
namespace
{
    struct MoverStat { U32 changes = 0; F32 speed = 0.f; bool active = false; LLVector3 last; bool have_last = false; std::vector<F32> deltas_mm; F32 dist = 0.f; U32 net = 0; LLVector3 last_obj; bool have_obj = false; };
    std::unordered_map<LLUUID, MoverStat> sMoverStats;
    U64 sMoverFrames = 0, sMoverQueueSum = 0, sMoverDrainSum = 0, sMoverQueueMax = 0;
    F64 sMoverWindowStart = 0.0;
}
static void vkMoverDiagNote(LLViewerObject* obj)
{
    if ((obj->getPositionAgent() - LLViewerCamera::getInstance()->getOrigin()).magVecSquared() > 32.f * 32.f) return;
    MoverStat& m = sMoverStats[obj->getID()];
    ++m.changes;
    // Per-frame drawn movement (the pose we upload: the eased render pose) and the
    // object-state position the network last set -- to tell smooth motion from
    // step-and-ease.
    F32 pose[16], sa[4], sp[4];
    LLCleanInstanceStore::composeOwnedPose(obj, pose, sa, sp);   // the world pose we upload
    const LLVector3 rp(pose[12], pose[13], pose[14]);
    if (m.have_last)
    {
        const F32 d = (rp - m.last).magVec();
        m.dist += d;
        if (m.deltas_mm.size() < 48) m.deltas_mm.push_back(d * 1000.f);
    }
    m.last = rp; m.have_last = true;
    // Object-state position (set by network updates / LL interpolation): how often it
    // actually changes = the update rate the motion is built from.
    const LLVector3 op = obj->getPositionAgent();
    if (m.have_obj && op != m.last_obj) ++m.net;
    m.last_obj = op; m.have_obj = true;
    LLViewerObject* root = obj->getRootEdit();
    m.speed = root ? root->getVelocity().magVec() : 0.f;
    m.active = obj->mDrawable.notNull() && obj->mDrawable->isActive();
}
static void vkMoverDiagFrame(size_t at_drain, size_t drained)
{
    if (!vkMoverDiagOn()) { sMoverStats.clear(); sMoverFrames = 0; return; }
    const F64 now = LLTimer::getElapsedSeconds();
    if (sMoverFrames == 0) sMoverWindowStart = now;
    ++sMoverFrames;
    sMoverQueueSum += at_drain; sMoverDrainSum += drained;
    sMoverQueueMax = llmax(sMoverQueueMax, (U64)at_drain);
    const F64 span = now - sMoverWindowStart;
    if (span < 2.0) return;
    std::vector<std::pair<F32, LLUUID>> top;   // by distance travelled
    for (const auto& kv : sMoverStats) top.emplace_back(kv.second.dist, kv.first);
    std::sort(top.rbegin(), top.rend());
    std::ostringstream os;
    os << "fps " << (F32)(sMoverFrames / span) << " | moved queue avg " << (sMoverQueueSum / sMoverFrames)
       << " max " << sMoverQueueMax << " drained/frame " << (sMoverDrainSum / sMoverFrames)
       << " | movers within 32 m: " << sMoverStats.size();
    for (size_t i = 0; i < top.size() && i < 6; ++i)
    {
        const MoverStat& m = sMoverStats[top[i].second];
        LLViewerObject* o = gObjectList.findObject(top[i].second);
        os << " | " << top[i].second.asString().substr(0, 8) << " moved " << m.dist << "m, pose " << (F32)(m.changes / span)
           << "/s, obj-state " << (F32)(m.net / span) << "/s v="
           << m.speed << (m.active ? " active" : " static") << (o && o->isTempAttachment() ? " temp" : "")
           << (o && o->flagTemporaryOnRez() ? " temp-rez" : "");
    }
    // Frame-by-frame movement (mm) of the top 3 movers: even steps = smooth motion;
    // bursts separated by ~0 = the pose only advances at network rate.
    for (size_t i = 0; i < top.size() && i < 3; ++i)
    {
        const MoverStat& m = sMoverStats[top[i].second];
        std::ostringstream d;
        for (F32 x : m.deltas_mm) d << (S32)ll_round(x) << ' ';
        LL_INFOS("VKMoverDiag") << "  " << top[i].second.asString().substr(0, 8) << " mm/frame: " << d.str() << LL_ENDL;
    }
    LL_INFOS("VKMoverDiag") << os.str() << LL_ENDL;
    sMoverStats.clear();
    sMoverFrames = sMoverQueueSum = sMoverDrainSum = sMoverQueueMax = 0;
}

static bool vkVehicleDiagOn()
{
    static LLCachedControl<bool> s_on(gSavedSettings, "RenderVKVehicleDiag", false);
    return s_on;
}
namespace
{
    // RenderVKVehicleDiag: gFrameCount at which each object's instance pose last
    // CHANGED in drainMovedQueue (only maintained while the diag is on).
    std::unordered_map<LLUUID, U32> sVehDiagRefreshed;
}
static void vkVehicleDiagNoteRefresh(const LLViewerObject* o, bool changed)
{
    if (changed && vkVehicleDiagOn()) sVehDiagRefreshed[o->getID()] = (U32)gFrameCount;
}

// #825 SHADOW REGISTRATION DIAGNOSTIC (RenderVKShadowRegDiag, default off): what feeds the
// tag-6 registration spheres that re-render the event-driven sun cascades and projector tiles
// (shadowEvChgRegister). Every 5 s (VKShadowReg): frames with spheres, spheres by camera
// distance (a far-only producer shows as cascades 2 + 3 re-rendering together), the store's
// per-reason counts (LLCleanInstanceStore::ShadowRegDiag: new / release / pose / spin / LOD
// group / flags + the differing bits / bounds + whether the group was unchanged / material)
// and the top emitting objects with what they are (pcode, mesh / sculpt, attachment, active
// drawable, animesh, flexi, dead / gone, distance).
static void vkShadowRegDiagFrame(LLCleanInstanceStore& st, const std::vector<F32>& reg, bool over)
{
    static LLCachedControl<bool> s_on(gSavedSettings, "RenderVKShadowRegDiag", false);
    const bool on = s_on && gVKProjShadowsUsed && vkRegSpheresOn();
    st.setShadowRegDiag(on);
    static F64 s_start = 0.0;
    static U32 s_frames = 0, s_frames_with = 0, s_spheres = 0, s_over = 0;
    static U32 s_dist[5] = {};   // < 32, < 64, < 128, < 256, >= 256 m
    static F64 s_radius = 0.0;
    if (!on) { s_frames = 0; return; }
    const F64 now = LLTimer::getElapsedSeconds();
    if (s_frames == 0)
    {
        s_start = now; s_frames_with = s_spheres = s_over = 0; s_radius = 0.0;
        for (U32& d : s_dist) d = 0;
    }
    ++s_frames;
    const size_t n = reg.size() / 4;
    if (n || over) ++s_frames_with;
    if (over) ++s_over;
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    for (size_t i = 0; i < n; ++i)
    {
        const F32* q = &reg[i * 4];
        const F32 d = (LLVector3(q[0], q[1], q[2]) - cam).length();
        ++s_dist[d < 32.f ? 0 : d < 64.f ? 1 : d < 128.f ? 2 : d < 256.f ? 3 : 4];
        s_radius += q[3];
    }
    s_spheres += (U32)n;
    const F64 span = now - s_start;
    if (span < 5.0) return;
    LLCleanInstanceStore::ShadowRegDiag dg;
    st.takeShadowRegDiag(dg);
    static const char* const kWhy[LLCleanInstanceStore::SRD_COUNT] =
        { "new", "release", "pose", "spin", "group", "flags", "aabb", "material" };
    std::ostringstream os;
    os << std::fixed << std::setprecision(2) << "registration spheres " << span << "s: " << s_spheres << " ("
       << (F32)(s_spheres / span) << "/s) in " << s_frames_with << "/" << s_frames << " frames, overflow "
       << s_over << ", mean r " << (s_spheres ? (F32)(s_radius / s_spheres) : 0.f) << " m | by distance <32 "
       << s_dist[0] << " <64 " << s_dist[1] << " <128 " << s_dist[2] << " <256 " << s_dist[3] << " >=256 "
       << s_dist[4] << " | events " << dg.events << ":";
    for (U32 k = 0; k < LLCleanInstanceStore::SRD_COUNT; ++k)
        if (dg.why[k]) os << ' ' << kWhy[k] << ' ' << dg.why[k];
    if (dg.why[LLCleanInstanceStore::SRD_AABB])
        os << " (aabb same group " << dg.aabb_same_group << ", max move " << dg.aabb_max_delta << " m)";
    if (dg.why[LLCleanInstanceStore::SRD_FLAGS])
    {
        os << " | flag bits:";
        for (U32 b = 0; b < 32; ++b)
            if (dg.flag_bits[b]) os << " b" << b << "x" << dg.flag_bits[b];
    }
    // #825: what the shadow-relevance rules dropped (each emitted spheres before)
    os << " | dropped " << dg.dropped_events << ":";
    for (U32 k = 0; k < LLCleanInstanceStore::SRD_COUNT; ++k)
        if (dg.dropped[k]) os << ' ' << kWhy[k] << ' ' << dg.dropped[k];
    for (U32 b = 0; b < 32; ++b)
        if (dg.dropped_flag_bits[b]) os << " b" << b << "x" << dg.dropped_flag_bits[b];
    os << ", lod bounds absorbed " << dg.aabb_absorbed;
    os << " | top:";
    for (const LLCleanInstanceStore::ShadowRegDiag::Obj& o : dg.top)
    {
        os << " [" << o.id.asString().substr(0, 8) << " x" << o.n << " why";
        for (U32 k = 0; k < LLCleanInstanceStore::SRD_COUNT; ++k)
            if (o.why & (1u << k)) os << ' ' << kWhy[k];
        os << " flags 0x" << std::hex << o.row_flags;
        if (o.flag_xor) os << " xor 0x" << o.flag_xor;
        os << std::dec;
        if (o.why & (1u << LLCleanInstanceStore::SRD_AABB)) os << " daabb " << o.aabb_delta;
        LLViewerObject* obj = gObjectList.findObject(o.id);
        if (!obj) os << " gone";
        else
        {
            LLVOVolume* vo = obj->getPCode() == LL_PCODE_VOLUME ? (LLVOVolume*)obj : nullptr;
            os << " pc" << (U32)obj->getPCode() << (obj->isDead() ? " dead" : "")
               << (obj->isMesh() ? " mesh" : vo && vo->isSculpted() ? " sculpt" : "")
               << (obj->isAttachment() ? " att" : "") << (obj->getAvatarAncestor() ? " on-avatar" : "")
               << (obj->mDrawable.notNull() && obj->mDrawable->isActive() ? " active" : "")
               << (vo && vo->isAnimatedObject() ? " animesh" : "") << (vo && vo->isFlexible() ? " flexi" : "")
               << (obj->flagUsePhysics() ? " phys" : "") << (obj->flagTemporaryOnRez() ? " temp" : "")
               << " lod " << (vo ? vo->getLOD() : -1)
               << " d " << (obj->getPositionAgent() - cam).length() << "m";
        }
        os << "]";
    }
    LL_INFOS("VKShadowReg") << os.str() << LL_ENDL;
    s_frames = 0;
}

void LLCleanIngest::drainMovedQueue()
{
    VK_HITCH_SCOPE("moveQueue");
    LL_PROFILE_ZONE_NAMED_CATEGORY_PIPELINE("clean:movedQueue");
    mDiag = MovedDiag{};
    mDiag.at_drain = mMovedQueue.size();

    // #567: expire / publish the dynamic shadow casters, then route this frame's pose
    // changes: into the dynamic class (no static-tile invalidation while it moves), or --
    // with the class off -- a changed sphere per move as before.
    tickDynCasters();
    // Registration-side invalidation (new / released / re-shaped instances), tag 6.
    {
        LLCleanInstanceStore& st = LLCleanInstanceStore::instance();
        static std::vector<F32> s_reg;
        bool over = false;
        st.takeShadowDirty(s_reg, over);
        st.setShadowDirtyCollect(gVKProjShadowsUsed && vkRegSpheresOn());
        vkShadowRegDiagFrame(st, s_reg, over);   // #825 (RenderVKShadowRegDiag)
        if (gVKProjShadowsUsed && !gVKChangedOverflow)
        {
            if (over || gVKChangedSpheres.size() + s_reg.size() > 4 * 8192)
            {
                gVKChangedOverflow = true; gVKChangedSpheres.clear(); gVKChangedSphereTags.clear();
            }
            else
            {
                gVKChangedSpheres.insert(gVKChangedSpheres.end(), s_reg.begin(), s_reg.end());
                gVKChangedSphereTags.insert(gVKChangedSphereTags.end(), s_reg.size() / 4, (uint8_t)6);
            }
        }
        if (!s_reg.empty()) LLVKHitch::count("projSphRegister", (unsigned)(s_reg.size() / 4));
        s_reg.clear();
    }
    const bool dyn_on = gVKProjShadowsUsed && vkDynCasterSettleSecs() > 0.f;
    auto notePose = [&](LLViewerObject* o, bool ch, uint8_t tag)
    {
        if (!ch && vkSphereOnPoseChangeOnly())
        {
            if (gVKProjShadowsUsed) LLVKHitch::count("projSphSkipped");
            return;
        }
        if (dyn_on && ch)
        {
            // Worn attachments are overlay casters already (instance bit9): nothing static moved.
            if (!o->isAttachment()) noteDynCaster(o, false);
            return;
        }
        noteChangedSphere(o, tag);
    };

    // SEAT FIRST (#96): the linkset the agent sits on is refreshed EVERY frame, ahead
    // of the capped FIFO below. The seated avatar and the camera move every frame; a
    // vehicle that waited its turn in a backlogged queue (1-2k entries in busy
    // regions) only moved every 3rd-4th frame -> stutter relative to its driver.
    LLViewerObject* seat_root = (isAgentAvatarValid() && gAgentAvatarp->isSitting() && gAgentAvatarp->getParent())
        ? ((LLViewerObject*)gAgentAvatarp->getParent())->getRootEdit() : nullptr;
    if (seat_root && !seat_root->isDead() && seat_root->getPCode() == LL_PCODE_VOLUME)
    {
        auto refresh = [&](LLViewerObject* o)
        {
            if (!o || o->isDead() || o->getPCode() != LL_PCODE_VOLUME) return;
            const bool ch = LLCleanInstanceStore::instance().refreshObjectPose(o);
            if (ch) ++mDiag.seat_changed;
            vkVehicleDiagNoteRefresh(o, ch);
            ++mDiag.seat_prims;
            notePose(o, ch, kSphSeat);
        };
        refresh(seat_root);
        for (LLViewerObject* child : seat_root->getChildren()) refresh(child);
    }
    if (mMovedQueue.empty()) { vkMoverDiagFrame(0, 0); return; }

    // Bounded per frame so a region full of scripted movers can't spike the frame;
    // leftovers stay queued (FIFO) for the next frame. At the cap, the linkset in
    // progress is finished (noteObjectMoved queues a root then its children), so a
    // linkset is never drawn half at this frame's pose and half at an older one.
    constexpr size_t MAX_PER_FRAME = 512;
    size_t i = 0;
    U32 n_refreshed = 0, n_changed = 0;   // section log: pose refreshes / ones that changed a row
    const LLViewerObject* cur_root = nullptr;
    for (; i < mMovedQueue.size(); ++i)
    {
        const LLUUID id = mMovedQueue[i];
        LLViewerObject* obj = gObjectList.findObject(id);
        const LLViewerObject* root = obj ? obj->getRootEdit() : nullptr;
        if (i >= MAX_PER_FRAME && (!root || root != cur_root)) break;
        mMovedSet.erase(id);
        if (!obj || obj->isDead() || obj->getPCode() != LL_PCODE_VOLUME) continue;
        cur_root = root;
        if (seat_root && root == seat_root) { mDiag.seat_drained = true; continue; }   // done above
        // POSE ONLY. A move changes transforms, not geometry: re-running full
        // registration here (capture / mesh decode / tangents for any face whose slot
        // wasn't built yet) for every moving object every frame cost ~40% of the main
        // thread. Not-yet-registered objects are picked up by the ingest sweep.
        const bool changed = LLCleanInstanceStore::instance().refreshObjectPose(obj);
        ++n_refreshed;
        if (changed) ++n_changed;
        vkVehicleDiagNoteRefresh(obj, changed);
        if (!obj->isHUDAttachment()) notePose(obj, changed, kSphMoved);
        if (vkMoverDiagOn() && changed) vkMoverDiagNote(obj);
        if (vkVehicleDiagOn())
        {
            if (obj->isAttachment()) ++mDiag.attach;
            else if (obj->getParent()) ++mDiag.child;
            else ++mDiag.root;
            if (!root->getVelocity().isExactlyZero() || root->getAngularVelocity().magVecSquared() > 0.f) ++mDiag.moving;
            if (changed) ++mDiag.changed;
        }
    }
    mDiag.drained = i;
    LLVKHitch::count("movedRefreshed", n_refreshed);
    LLVKHitch::count("movedChanged", n_changed);
    vkMoverDiagFrame(mDiag.at_drain, i);
    mMovedQueue.erase(mMovedQueue.begin(), mMovedQueue.begin() + i);
}

// Rotation carried by an uploaded instance matrix (rows = scaled local axes).
static LLQuaternion vkDiagInstRot(const LLCleanInstanceStore::Instance& in)
{
    LLVector3 ax(in.model_matrix[0], in.model_matrix[1], in.model_matrix[2]);
    LLVector3 ay(in.model_matrix[4], in.model_matrix[5], in.model_matrix[6]);
    LLVector3 az(in.model_matrix[8], in.model_matrix[9], in.model_matrix[10]);
    ax.normVec(); ay.normVec(); az.normVec();
    LLMatrix3 m3;
    m3.setRows(ax, ay, az);
    LLQuaternion q(m3);
    q.normalize();
    return q;
}
static F32 vkDiagAngleDeg(const LLQuaternion& a, const LLQuaternion& b)
{
    const F32 d = llclamp(fabsf(dot(a, b)), 0.f, 1.f);
    return 2.f * acosf(d) * RAD_TO_DEG;
}
static bool vkDiagFirstInstance(const LLViewerObject* o, LLCleanInstanceStore::Instance& inst)
{
    for (S32 f = 0; f < 8; ++f)
        if (LLCleanInstanceStore::instance().debugGet(o->getID(), f, inst)) return true;
    return false;
}

void LLCleanIngest::vkVehicleDiagTick()
{
    if (!vkVehicleDiagOn()) { sVehDiagRefreshed.clear(); return; }
    // Subject: the linkset the agent sits on, else the selected linkset (#180: record
    // a scripted test prim without sitting on it).
    LLViewerObject* root = nullptr;
    bool seated = false;
    if (isAgentAvatarValid() && gAgentAvatarp->isSitting() && gAgentAvatarp->getParent())
    {
        root = ((LLViewerObject*)gAgentAvatarp->getParent())->getRootEdit();
        seated = true;
    }
    else
    {
        LLViewerObject* sel = LLSelectMgr::getInstance()->getSelection()->getFirstRootObject(true);
        root = sel ? sel->getRootEdit() : nullptr;
    }
    if (!root || root->isDead() || root->getPCode() != LL_PCODE_VOLUME) return;
    // Uploaded pose: the first registered face of the root (store copy after this
    // frame's drainMovedQueue; syncFromStores streams it into this frame's record).
    LLCleanInstanceStore::Instance inst;
    const bool have = vkDiagFirstInstance(root, inst);
    const LLVector3 obj = root->getPositionAgent();
    const LLDrawable* dr = root->mDrawable;
    const bool dr_ok = dr && !dr->isDead();
    const bool on_move = dr_ok && dr->isState(LLDrawable::ON_MOVE_LIST);
    const LLVector3 xf = dr_ok ? dr->mXform.getPositionW() : LLVector3::zero;
    const LLVector3 up = have ? LLVector3(inst.model_matrix[12], inst.model_matrix[13], inst.model_matrix[14]) : LLVector3::zero;
    // Root rotation: eased (what LL renders and the driver / camera follow) vs the
    // object target, and the uploaded instance vs eased. GPU-spun instances carry the
    // un-spun base pose, so instVsEased is meaningless while gpuSpin=1.
    LLVector3 epos; LLQuaternion erot;
    vkRenderLocalPose(root, epos, erot);
    const F32 root_lag_deg  = vkDiagAngleDeg(erot, root->getRotationRegion());
    const F32 root_inst_deg = have ? vkDiagAngleDeg(vkDiagInstRot(inst), erot) : -1.f;
    const bool root_gpu_spin = have && (inst.spin_axis[0] != 0.f || inst.spin_axis[1] != 0.f || inst.spin_axis[2] != 0.f);
    // Avatar pelvis / camera in the SEAT ROOT's frame (uploaded pose): constant while
    // seated unless the avatar, the camera and the vehicle come from different sources.
    const LLVector3 pelvis = (isAgentAvatarValid() && gAgentAvatarp->mPelvisp) ? gAgentAvatarp->mPelvisp->getWorldPosition() : LLVector3::zero;
    const LLVector3 rel = (pelvis - up) * ~root->getRotationRegion();
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    const LLVector3 cam_rel = (cam - up) * ~root->getRotationRegion();
    static LLVector3 s_prev_up, s_prev_obj;
    static F64 s_prev_t = 0.0;
    const F64 t = LLFrameTimer::getElapsedSeconds();
    const F32 dt = (F32)(t - s_prev_t);
    auto refreshed = [](const LLViewerObject* o)
    {
        auto it = sVehDiagRefreshed.find(o->getID());
        return it != sVehDiagRefreshed.end() && it->second == (U32)gFrameCount;
    };
    LL_INFOS("VKVehicleDiag") << "f=" << gFrameCount << " dt=" << dt * 1000.f
        << (seated ? " seat" : " selected")
        << " vel=" << root->getVelocity().length()
        << " omega=" << root->getAngularVelocity().length()
        << (root->flagUsePhysics() ? " phys" : "")
        << " obj=" << obj << " dObj=" << (obj - s_prev_obj).length()
        << " up=" << up << " dUp=" << (up - s_prev_up).length()
        << " upMinusObj=" << (up - obj).length()
        << " easeLagDeg=" << root_lag_deg << " instVsEasedDeg=" << root_inst_deg
        << " gpuSpin=" << root_gpu_spin << " refreshed=" << refreshed(root)
        << " xf=" << xf << " onMove=" << on_move << " active=" << (dr_ok && dr->isActive())
        << " q=" << mDiag.at_drain << "/" << mDiag.drained << " seatQueued=" << mDiag.seat_drained
        << " seat=" << mDiag.seat_changed << "/" << mDiag.seat_prims
        << " qRoot=" << mDiag.root << " qChild=" << mDiag.child << " qAtt=" << mDiag.attach
        << " qMoving=" << mDiag.moving << " qChanged=" << mDiag.changed
        << " pelvisRel=" << rel << " camRel=" << cam_rel
        << LL_ENDL;
    s_prev_up = up; s_prev_obj = obj; s_prev_t = t;

    // Children being animated (a steering wheel / gauge turned by the script): easing
    // now (drawable on the move list) or a rotation update in the last 30 frames. Per
    // child: rendered (instance) rotation vs LL's eased rotation (should be ~0), eased
    // vs object target (the remaining ease; a smooth turn shows it decaying over a few
    // frames, a snap shows 0 right after the update), and whether its instance pose
    // changed this frame. Capped at 8 lines per frame.
    U32 n = 0;
    for (LLViewerObject* c : root->getChildren())
    {
        if (n >= 8) break;
        if (!c || c->isDead() || c->getPCode() != LL_PCODE_VOLUME) continue;
        const LLDrawable* cd = c->mDrawable;
        const bool c_move = cd && !cd->isDead() && cd->isState(LLDrawable::ON_MOVE_LIST);
        const bool recent_rot = c->mVKRotUpdates > 0 && (U32)gFrameCount - c->mVKUpdFrame < 30u;
        if (!c_move && !recent_rot) continue;
        ++n;
        LLCleanInstanceStore::Instance ci;
        const bool c_have = vkDiagFirstInstance(c, ci);
        LLVector3 lp; LLQuaternion lr;
        vkRenderLocalPose(c, lp, lr);
        const LLQuaternion eased  = lr * erot;
        const LLQuaternion target = c->getRotation() * root->getRotationRegion();
        LL_INFOS("VKVehicleDiag") << "  child " << c->getID().asString().substr(0, 8)
            << " instVsEasedDeg=" << (c_have ? vkDiagAngleDeg(vkDiagInstRot(ci), eased) : -1.f)
            << " easedVsTargetDeg=" << vkDiagAngleDeg(eased, target)
            << " refreshed=" << refreshed(c)
            << " onMove=" << c_move << " active=" << (cd && !cd->isDead() && cd->isActive())
            << " gpuSpin=" << (c_have && (ci.spin_axis[0] != 0.f || ci.spin_axis[1] != 0.f || ci.spin_axis[2] != 0.f))
            << " rotUpdAgo=" << ((U32)gFrameCount - c->mVKUpdFrame) << "f"
            << LL_ENDL;
    }
}

// Consumed by the VK renderer (particle sim push / SSR previous view), next record.
float gVKPartShift[3] = {0.f, 0.f, 0.f};
float gVKPrevViewShift[3] = {0.f, 0.f, 0.f};
// #895: consumed by the VK renderer's weather (LLCleanWeatherVK::shiftOrigin: the snow grids,
// ring and roof maps re-based instead of dropped), next record.
float gVKWeatherShift[3] = {0.f, 0.f, 0.f};

void LLCleanIngest::shiftOrigin(const LLVector3& offset)
{
    static LLCachedControl<bool> s_on(gSavedSettings, "RenderVKShiftOrigin", true);
    if (!s_on) return;
    if (offset.isExactlyZero()) return;
    CROSSING_SCOPE(LLCrossingTrace::CH_INGEST_SHIFT, "Crossing ingest shiftOrigin");
    const F32 off[3] = { offset.mV[VX], offset.mV[VY], offset.mV[VZ] };
    LLCleanInstanceStore& is = LLCleanInstanceStore::instance();
    // #568 RENDER-WORLD FRAME (RenderVKStableWorldFrame, default on): the instance store
    // keeps a stable frame (store = agent + d), so a crossing only changes d -- no row is
    // rewritten, dirtied, re-boxed, re-copied or re-streamed; the renderer moves its GPU rows
    // with one compute pass and re-bases its agent-space caches (shadow cascades, projector
    // tiles, TAA, skin history, TLAS camera) instead of invalidating them. The frame
    // RE-ANCHORS (d := 0) once the agent region is more than RenderVKReanchorMeters from the
    // anchor along x or y (float budget: llcleaninstancestore.h) -- a CPU-only O(N) add.
    // Off: the legacy path below (every row rewritten + dirtied, cascades / TAA reset).
    static LLCachedControl<bool> s_stable(gSavedSettings, "RenderVKStableWorldFrame", true);
    static LLCachedControl<F32> s_reanchor_m(gSavedSettings, "RenderVKReanchorMeters", 2048.f);
    if (s_stable)
    {
        is.noteAgentShift(off);
        const F32 span = is.renderOffsetSpan();
        const F32 limit = llmax((F32)s_reanchor_m, 0.f);
        std::ostringstream note;
        F32 d[3];
        if (span > limit)
        {
            const double t0 = LLCrossingTrace::nowMs();
            F32 delta[3] = {0.f, 0.f, 0.f};
            const U32 rows = is.reanchor(delta);
            const double ms = LLCrossingTrace::nowMs() - t0;
            LLCrossingTrace::addChunk(LLCrossingTrace::CH_REANCHOR, ms, rows);
            note << "stable reanchor=YES (span " << span << " m > " << limit << " m) delta=(" << delta[0] << ","
                 << delta[1] << ") rows=" << rows << " cost_ms=" << ms;
            LL_INFOS("VKShift") << "agent origin shift " << offset << ": render-world frame re-anchored (offset span "
                << span << " m > " << limit << " m), " << rows << " store rows moved by (" << delta[0] << ", "
                << delta[1] << ") in " << ms << " ms (CPU only; GPU rows unchanged)" << LL_ENDL;
        }
        is.renderOffset(d);
        note << (span > limit ? " " : "stable reanchor=no ") << "d=(" << d[0] << "," << d[1] << ") span_limit_m=" << limit;
        LLCrossingTrace::noteRenderFrame(note.str());
        crossing_scope_.setA(0);
        if (span <= limit)
            LL_INFOS("VKShift") << "agent origin shift " << offset << ": render-world frame offset now (" << d[0] << ", "
                << d[1] << "), no store rows touched" << LL_ENDL;
    }
    else
    {
        // LEGACY (A/B): the store must be in agent space first (d = 0) -- a no-op unless the
        // stable frame was on earlier this session.
        F32 d0[3];
        is.renderOffset(d0);
        if (d0[0] != 0.f || d0[1] != 0.f || d0[2] != 0.f) is.reanchor();
        extern U32 gVKOriginShiftGen;
        ++gVKOriginShiftGen;   // cached shadow cascades hold agent-space matrices
        const U32 n_inst = is.shiftOrigin(off);
        crossing_scope_.setA(n_inst);
        LLCrossingTrace::noteRenderFrame("legacy (RenderVKStableWorldFrame off): every row rewritten + dirtied");
        LL_INFOS("VKShift") << "agent origin shift " << offset << ": re-based " << n_inst
            << " instances + particles + SSR history (legacy)" << LL_ENDL;
    }
    LLCleanParticleStore::instance().shiftOrigin(off);
    for (int c = 0; c < 3; ++c) { gVKPartShift[c] += off[c]; gVKPrevViewShift[c] += off[c]; gVKWeatherShift[c] += off[c]; }
}

void LLCleanIngest::vkRematWakeTick()
{
    LLCleanMeshAsset& ms = LLCleanMeshAsset::instance();
    const F64 now = LLTimer::getTotalSeconds();
    const bool refresh = ms.rematPending() != 0u && now >= mRematRefreshAt;
    if (refresh) mRematRefreshAt = now + kRematRefreshSec;
    static std::vector<U32> s_new;
    ms.takeRematWake(s_new, refresh);
    LLCleanInstanceStore& is = LLCleanInstanceStore::instance();
    const U32 row_n = is.cpuInstanceCount();
    if (!s_new.empty())
    {
        try
        {
            mRematGroups.insert(mRematGroups.end(), s_new.begin(), s_new.end());
            std::sort(mRematGroups.begin(), mRematGroups.end());
            mRematGroups.erase(std::unique(mRematGroups.begin(), mRematGroups.end()), mRematGroups.end());
        }
        catch (const std::bad_alloc&) { mRematGroups.clear(); }   // (the next refresh wakes them)
        mRematRowsLeft = row_n;   // a whole lap from the cursor
    }
    if (mRematGroups.empty()) return;
    const LLCleanInstanceStore::Instance* rows =
        reinterpret_cast<const LLCleanInstanceStore::Instance*>(is.cpuInstanceBytes());
    if (!rows || row_n == 0) { mRematGroups.clear(); mRematRowsLeft = 0; return; }
    mRematRowsLeft = std::min(mRematRowsLeft, row_n);
    U32 woken = 0;
    for (U32 k = 0; k < kRematRowsPerFrame && mRematRowsLeft; ++k, --mRematRowsLeft)
    {
        if (mRematCursor >= row_n) mRematCursor = 0;
        const U32 id = mRematCursor++;
        const U32 gid = rows[id].lod_group_id;
        if (gid == 0xFFFFFFFFu || !std::binary_search(mRematGroups.begin(), mRematGroups.end(), gid)) continue;
        LLUUID obj_id;
        S32 face = 0;
        if (!is.keyForId(id, obj_id, face)) continue;
        if (LLViewerObject* o = gObjectList.findObject(obj_id))
        {
            noteObjectChanged(o);   // (deduplicated by the changed queue)
            ++woken;
        }
    }
    if (!mRematRowsLeft) mRematGroups.clear();
    LLVKHitch::count("rematWake", woken);
}

void LLCleanIngest::selfRegisterSweep()
{
    VK_HITCH_SCOPE("sweep_total");
    {
        static LLCachedControl<bool> s_sec(gSavedSettings, "RenderVKSectionLog", false);
        static F64 s_next = 0.0;
        const F64 now = LLTimer::getElapsedSeconds();
        if (s_sec && now > s_next)
        {
            s_next = now + 10.0;
            std::vector<std::pair<U32, const LLUUID*>> top;
            U32 why_n[5] = {0, 0, 0, 0, 0};
            // Only objects that took a full visit since the last log (walking the whole
            // signature map + resetting it was a 15 ms hitch every 10 s).
            for (const LLUUID& id : mChurnIds)
            {
                auto it = mObjSig.find(id);
                if (it != mObjSig.end() && it->second.fulls)
                { top.emplace_back(it->second.fulls, &it->first); why_n[it->second.why % 5] += it->second.fulls; }
            }
            std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            std::ostringstream os;
            os << "sweep churn 10s: full visits by reason sig=" << why_n[1] << " easing=" << why_n[2]
               << " lap=" << why_n[3] << " first=" << why_n[4] << " | top:";
            for (size_t i = 0; i < top.size() && i < 10; ++i)
            {
                LLViewerObject* o = gObjectList.findObject(*top[i].second);
                const ObjSig& s = mObjSig[*top[i].second];
                os << " [" << top[i].first << "x why" << (U32)s.why;
                if (o)
                {
                    LLVOVolume* vo = o->getPCode() == LL_PCODE_VOLUME ? (LLVOVolume*)o : nullptr;
                    os << " pc" << (U32)o->getPCode() << (vo && vo->isFlexible() ? " flexi" : "")
                       << (o->isAttachment() ? " att" : "") << (o->flagUsePhysics() ? " phys" : "")
                       << (o->mDrawable.notNull() && o->mDrawable->isState(LLDrawable::ON_MOVE_LIST) ? " moving" : "")
                       << (vo && vo->isAnimatedObject() ? " animesh" : "");
                }
                os << "]";
            }
            LL_INFOS("VKIngest") << os.str() << LL_ENDL;
            for (const LLUUID& id : mChurnIds)
            {
                auto it = mObjSig.find(id);
                if (it != mObjSig.end()) it->second.fulls = 0;
            }
            mChurnIds.clear();
        }
    }
    drainMovedQueue();
    // #566: SLIgnoreFullbright toggled -> queue every object with a fullbright face, so
    // the change shows now instead of when each one next changes or the sweep laps it.
    {
        static LLCachedControl<bool> s_ignore_fb(gSavedSettings, "SLIgnoreFullbright", false);
        static bool s_last_ignore_fb = s_ignore_fb;
        if ((bool)s_ignore_fb != s_last_ignore_fb)
        {
            s_last_ignore_fb = s_ignore_fb;
            const S32 n = (S32)gObjectList.getNumObjects();
            for (S32 i = 0; i < n; ++i)
            {
                LLViewerObject* o = gObjectList.getObject(i);
                if (!o || o->isDead() || o->getPCode() != LL_PCODE_VOLUME) continue;
                for (U8 t = 0; t < o->getNumTEs(); ++t)
                {
                    const LLTextureEntry* te = o->getTE(t);
                    if (te && te->getFullbright()) { noteObjectChanged(o); break; }
                }
            }
        }
    }
    // Objects whose async mesh decode (LLCleanMeshAsset::takeDecoded) completed since
    // last frame: re-ingest them now instead of waiting for the sweep cursor to lap.
    {
        VK_HITCH_SCOPE("ing_decodePump");
        static std::vector<LLUUID> s_wake;
        s_wake.clear();
        LLCleanMeshAsset::instance().pumpDecodes(s_wake);
        for (const LLUUID& id : s_wake)
            if (LLViewerObject* o = gObjectList.findObject(id)) noteObjectChanged(o);
        LLVKHitch::count("meshDecodeWake", (unsigned)s_wake.size());
    }
    // CLUSTER-OWNED MIRROR RELEASE (LLCleanMeshAsset::queueClusterMirrorRelease): the mirrors the
    // renderer queued last frame are freed here (no store-fill worker runs now), then the
    // objects naming re-materializing slots are woken (registerCleanFace re-decodes them).
    {
        VK_HITCH_SCOPE("ing_mirrorRelease");
        LLCleanMeshAsset::instance().pumpMirrorReleases();
        vkRematWakeTick();
    }
    // INGEST WORKER SYNC POINT (RenderVKIngestWorker): captures prepared on the worker
    // since last frame are committed to the mesh store HERE -- on the render thread,
    // inside vkIngestTick, i.e. before renderFrame starts the store-fill worker -- and
    // the objects that waited on them are re-ingested now (instances need their slots).
    {
        VK_HITCH_SCOPE("ing_captureCommit");
        static std::vector<LLUUID> s_cap_wake;
        s_cap_wake.clear();
        LLCleanIngest::vkPumpAsyncCaptures(s_cap_wake);
        std::sort(s_cap_wake.begin(), s_cap_wake.end());
        s_cap_wake.erase(std::unique(s_cap_wake.begin(), s_cap_wake.end()), s_cap_wake.end());
        for (const LLUUID& id : s_cap_wake)
            if (LLViewerObject* o = gObjectList.findObject(id)) noteObjectChanged(o);
        LLVKHitch::count("captureWake", (unsigned)s_cap_wake.size());
    }
    drainChangedQueue();
    drainTerrainDirty();
    LLCleanTerrainSmooth::tick();   // #211: smooth-terrain residency around the camera
    // CLEAN-PIPELINE-OWNED INGESTION. The clean instance for every face is now
    // built directly from the object/volume/TE state by registerCleanFace --
    // independent of LL's genDrawInfo, the spatial group, the LL vertex buffer,
    // and LL flagging GEOM_DIRTY. Each main frame WE re-ingest a bounded slice of
    // the world's decoded volume objects (force_reingest bypasses the dedup), so a
    // full cycle rebuilds every instance from authoritative state in ~1-2s. This
    // is the decoupling step: LL only decodes mesh/texture data; we own the entire
    // ingest->cull->draw pathway, and the continuous rebuild makes the pipeline
    // self-heal -- a dropped/corrupt instance is rebuilt within one cycle instead
    // of being permanent. Runs on the render thread (renderFrame), so the capture
    // glBufferSubData and the store mutations are correctly serialized with draw.
    const S32 num = (S32)gObjectList.getNumObjects();
    if (num <= 0) return;
    // force_reingest=FALSE: registerCleanFace still runs capture() + the LOD/geometry
    // re-capture check UNCONDITIONALLY (those are before the dedup), so geometry
    // coverage and LOD refresh are preserved -- only the redundant per-frame instance
    // re-upsert (material/skin/flags resolve + SSBO write) is skipped for unchanged
    // objects via the content-signature dedup. That makes processing an already-
    // ingested object a few cheap map lookups instead of a full rebuild, so we can
    // afford a much WIDER slice and reach newly-decoded geometry in a couple frames
    // instead of leaving it invisible until the cursor laps a big object list (the
    // "empty sim for a minute, one zoom fills it" load stall). New/changed objects
    // still get a full ingest (no signature -> dedup misses -> upsert runs).
    // PERF: SLICE must be << object count so the sweep AMORTIZES over frames.
    // At SLICE >= num, we re-register the entire object list every frame
    // (~14.5k registerCleanFace calls/frame at 3822 objs = ~40ms CPU, GPU idle
    // -- the 150->60fps regression). Live-tunable so the perf/heal-latency
    // tradeoff can be dialed without a rebuild; 256 => ~15-frame (0.25s) heal
    // cycle at this object count.
    static LLCachedControl<S32> sweep_slice(gSavedSettings, "RenderCleanSweepSlice", 256);
    S32 SLICE = llmax((S32)16, (S32)sweep_slice);
    // ADAPTIVE STEADY STATE (VK). Once a full cycle over the object list produced no
    // instance changes (no upserts) and the camera stayed put (LOD choices unchanged),
    // the world is settled: re-verifying everything at full rate was ~25% of the main
    // thread for zero work. Idle cycles run at 1/8 slice + budget; any upsert or real
    // camera movement restores full speed on the next cycle.
    const bool idle_sweep = mSweepIdle;
    if (idle_sweep) SLICE = llmax((S32)16, SLICE / 8);
    SweepStats st;
    // FUNNEL DIAG (behavior-neutral): does MESH geometry actually arrive in VK?
    // display() is diverted, so the mesh fetch/volume-rebuild pump may be starved.
    // mesh_seen  = PCODE_VOLUME objects that are mesh assets.
    // mesh_novol = mesh objects SKIPPED here because getVolume() is null (not decoded
    //              / volume never rebuilt) -- directly tests hypothesis (c).
    // prim_novol = non-mesh volume objects skipped for no volume (should be ~0).
    // (mesh_seen / mesh_novol / prim_novol / mesh_reissue / mesh_req live in st.)
    // TIME BUDGET (VK). Ingest cost per object varies wildly (a mesh arriving means
    // decode + cache-optimize + tangents), so a fixed object count spikes the frame
    // during load. Stop once the budget is spent (always at least 16 objects so load
    // still progresses); the cursor resumes next frame -- load stretches slightly,
    // frame time stays smooth.
    static LLCachedControl<F32> sweep_budget_ms(gSavedSettings, "RenderCleanSweepBudgetMs", 3.f);
    LLTimer sweep_timer;
    const F64 sweep_budget_s = llmax(0.5f, (F32)sweep_budget_ms) * 0.001
                               * (idle_sweep ? 0.125 : (mSweepLoading ? 3.0 : 1.0));
    U32 band_base = 0;   // st.band_changes already credited to the previous lap
    U32 sig_base = 0;    // st.sig_changes likewise
    for (S32 k = 0; k < SLICE && k < num; ++k)
    {
        if (k >= 16 && (k & 7) == 0
            && sweep_timer.getElapsedTimeF64() > sweep_budget_s)
            break;
        if (mSweepCursor >= (U32)num)
        {
            // Cycle boundary: decide the next cycle's pace.
            mSweepCursor = 0;
            ++mSweepLap;
            // Prune signatures of objects that are gone (bounded, once per lap).
            if (mObjSig.size() > (size_t)num + (size_t)num / 4 + 256)
            {
                VK_HITCH_SCOPE("sweep_prune");
                for (auto it = mObjSig.begin(); it != mObjSig.end(); )
                {
                    if (!gObjectList.findObject(it->first)) it = mObjSig.erase(it);
                    else ++it;
                }
            }
            const U32 ups = LLCleanInstanceStore::instance().upsertCount();
            const U32 news = LLCleanInstanceStore::instance().newInstanceCount();
            const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
            const bool moved = (cam - mSweepCycleCam).lengthSquared() > 4.f;   // > 2 m
            // Settled = no new instances + only background churn (scripted colour /
            // texture-anim / mover bounds -- all also push-updated) under 2% of objects.
            // Loading (new objects, textures/meshes landing) changes thousands per cycle.
            const U32 changes = ups - mSweepCycleUpserts;
            // What the SWEEP itself found changed this lap (signature changed / never seen).
            // The global upsert count above includes every push-updated mover, animesh and
            // scripted colour change in a busy sim, which kept the safety net at full rate
            // forever (~1.5 ms/frame) although its own visits were finding nothing.
            const U32 sig_changes = mSweepCycleSigChanges + (st.sig_changes - sig_base);
            sig_base = st.sig_changes;
            mSweepCycleSigChanges = 0;
            LLVKHitch::count("sweepCycleSigChanges", sig_changes);
            // A trickle of NEW instances (avatars arriving, temp rezzes, attachments) is
            // normal in a busy sim and is registered by the priority ingest, not by this
            // sweep: requiring zero kept a populated sim at full sweep rate forever
            // (~2 ms per frame). Loading still adds far more than this per cycle.
            const U32 new_instances = news - mSweepCycleNew;
            // C1: GPU LOD band changes seen this lap. With the pre-signature on, camera
            // movement alone no longer keeps the sweep at full rate: it idles unless an
            // object's LOD band actually changed (moving the camera without crossing a
            // band needs no re-convergence). Not moving still idles as before.
            const U32 band_changes = mSweepCycleBandChanges + (st.band_changes - band_base);
            band_base = st.band_changes;
            mSweepCycleBandChanges = 0;
            LLVKHitch::count("sweepCycleBandChanges", band_changes);
            const bool lod_still = !moved || (vkSweepPreSigOn() && band_changes == 0);
            static LLCachedControl<bool> s_sig_idle(gSavedSettings, "RenderVKSweepIdleOnSig", true);
            const bool quiet = s_sig_idle ? sig_changes < (U32)llmax(32, num / 50)   // ~2% of objects per lap
                                          : changes < (U32)llmax(64, num / 50);
            mSweepIdle = new_instances < (U32)llmax(16, num / 200) && quiet && lod_still;
            // Loading (many new instances per cycle: arrival, teleport, draw distance
            // raised): the sweep carries asset-load work, so it gets 3x its budget.
            mSweepLoading = new_instances >= (U32)llmax(64, num / 50);
            LLVKHitch::count("sweepCycleNew", new_instances);
            LLVKHitch::count("sweepCycleChanges", changes);
            mSweepCycleNew = news;

            mSweepCycleUpserts = ups;
            mSweepCycleCam = cam;
        }
        LLViewerObject* obj = gObjectList.getObject((S32)mSweepCursor++);
        if (!obj || obj->isDead()) continue;
        sweepIngestObject(obj, st);
    }
    mSweepCycleBandChanges += st.band_changes - band_base;
    mSweepCycleSigChanges += st.sig_changes - sig_base;
    LLVKHitch::count("sweepObjs", st.objs);
    LLVKHitch::count("sweepSkipped", st.skipped);
    LLVKHitch::count("sweepHeavySkipped", st.heavy_skipped);
    LLVKHitch::count("sweepSetVolSkipped", st.setvol_skipped);
    LLVKHitch::count("sweepIdle", idle_sweep ? 1u : 0u);
    static U32 s_tick = 0;
    if ((s_tick++ % 128) == 0)
        LL_INFOS("Clean29Ingest") << "selfRegisterSweep objs=" << st.objs
            << " faces=" << st.faces << " cursor=" << mSweepCursor
            << "/" << num
            << " | mesh_seen=" << st.mesh_seen << " mesh_novol=" << st.mesh_novol
            << " prim_novol=" << st.prim_novol
            << " | mesh_reissue=" << st.mesh_reissue << " mesh_req(loadMesh)=" << st.mesh_req
            << LL_ENDL;
}

// VK: the user's own attachments (every attachment point incl. HUD points, each
// attached object + its linkset children, incl. attached animesh) are ingested ahead
// of the world sweep. On mainland the list-order sweep can take many seconds to reach
// them; this pass visits them every frame under a small budget with its own cursor so
// a large outfit still amortizes. Already-registered faces dedup cheaply. It also
// drives LL's fetch priority for their textures: LL's own updateTextureVirtualSize
// (BOOST_HUD + pixel area) does run under VK, but from LLViewerObjectList::updateTextures'
// time-sliced walk over the whole object list, so on mainland it reaches self/HUD objects
// only occasionally. This supplements it every pass.
// Why a priority-pass object is still pending (RenderVKSectionLog diagnostic): the
// ingest state of each face, the skin-store wait, the move list.
static std::string vkPrioPendingWhy(LLViewerObject* obj, U16 missing)
{
    std::ostringstream os;
    os << obj->getID() << " local=" << obj->getLocalID() << " pc" << (U32)obj->getPCode()
       << (obj->isMesh() ? " mesh" : "") << (obj->isHUDAttachment() ? " hud" : "")
       << (obj->getVolume() ? "" : " NO_VOLUME") << " missing=" << missing;
    LLDrawable* dr = obj->mDrawable;
    if (!dr || dr->isDead()) { os << " no-drawable"; return os.str(); }
    if (dr->isState(LLDrawable::ON_MOVE_LIST)) os << " on-move-list";
    os << " faces:";
    for (S32 fi = 0; fi < dr->getNumFaces() && fi < 16; ++fi)
    {
        const LLFace* f = dr->getFace(fi);
        os << ' ' << (f ? (int)f->mVKIngest : -1);
    }
    LLVOVolume* rv = dr->getVOVolume();
    const LLMeshSkinInfo* sk = rv ? rv->getSkinInfo() : nullptr;
    if (sk && obj->getAvatar() && LLCleanSkinStore::instance().isPending(obj->getAvatar(), sk))
        os << " skin-pending";
    return os.str();
}

void LLCleanIngest::vkPriorityIngest()
{
    LL_PROFILE_ZONE_SCOPED;
    if (!isAgentAvatarValid()) return;
    // Strong refs: an attachment can be detached/killed while this pass runs (e.g. a
    // preferences apply that rebuilds avatars); raw pointers here dangled and the
    // ID hash in sweepIngestObject read freed memory (crash on setting anisotropy).
    // Holding LLPointers keeps each object alive for the pass; sweepIngestObject's
    // isDead() check then skips any that died meanwhile. Released after the pass.
    static std::vector<LLPointer<LLViewerObject>> objs;
    objs.clear();
    struct ClearOnExit { ~ClearOnExit() { objs.clear(); } } clear_on_exit;
    struct Walk
    {
        static void add(LLViewerObject* o, std::vector<LLPointer<LLViewerObject>>& out)
        {
            if (!o || o->isDead()) return;
            out.emplace_back(o);
            for (const LLPointer<LLViewerObject>& c : o->getChildren())
                add(c.get(), out);
        }
    };
    for (const auto& ap : gAgentAvatarp->mAttachmentPoints)
    {
        const LLViewerJointAttachment* att = ap.second;
        if (!att) continue;
        for (const LLPointer<LLViewerObject>& o : att->mAttachedObjects)
            Walk::add(o.get(), objs);
    }
    if (objs.empty())
    {
        mPriorityCursor = 0; mPrioSettled = false; mPrioLastObjCount = 0;
        mPrioPending.clear(); mPrioLapPending.clear();
        return;
    }

    // TEXTURE PRIORITY: every worn object, every frame -- a per-face pointer walk, cheap,
    // and independent of whether the object needs ingest work (it used to ride on the
    // ingest visit, which in practice was every object every frame: the pass never
    // settled). Keeps self/HUD textures at the top of LL's fetch order.
    constexpr F32 SELF_TEX_VSIZE = 1024.f * 1024.f;
    for (const LLPointer<LLViewerObject>& op : objs)
    {
        LLViewerObject* obj = op.get();
        if (obj->isDead() || !obj->mDrawable) continue;
        const bool hud = obj->isHUDAttachment();
        LLDrawable* dr = obj->mDrawable;
        for (S32 f = 0; f < dr->getNumFaces(); ++f)
        {
            LLFace* facep = dr->getFace(f);
            if (!facep) continue;
            for (U32 ch = 0; ch < LLRender::NUM_TEXTURE_CHANNELS; ++ch)
            {
                LLViewerTexture* t = facep->getTexture(ch);
                if (!t) continue;
                if (hud && t->getBoostLevel() < LLGLTexture::BOOST_HUD)
                    t->setBoostLevel(LLGLTexture::BOOST_HUD);
                else if (!hud)
                    t->addTextureStats(SELF_TEX_VSIZE);
            }
        }
    }

    // One ingest visit; true = the object still has work pending: an instance (re)built,
    // a volume missing, a mesh asset fetch issued, or a face still waiting on decode /
    // capture (the full signature was left unsettled). NOT st.mesh_reissue: it counts
    // every heavy visit of any mesh with a volume, and attachments are always heavy.
    auto visit = [this](LLViewerObject* obj, SweepStats& st) -> bool
    {
        const U32 up0 = LLCleanInstanceStore::instance().upsertCount();
        const SweepStats st0 = st;
        sweepIngestObject(obj, st);
        LLVKHitch::count("prioVisited");
        bool pending = LLCleanInstanceStore::instance().upsertCount() != up0 ||
            st.mesh_novol != st0.mesh_novol || st.prim_novol != st0.prim_novol ||
            st.mesh_req != st0.mesh_req;
        if (pending)
        {
            if (LLCleanInstanceStore::instance().upsertCount() != up0) LLVKHitch::count("prioPendUpsert");
            if (st.mesh_novol != st0.mesh_novol || st.prim_novol != st0.prim_novol) LLVKHitch::count("prioPendNoVol");
            if (st.mesh_req != st0.mesh_req) LLVKHitch::count("prioPendMeshReq");
        }
        else if (!obj->isDead())
        {
            // Unsettled signature = a face still waiting on decode / capture. Not when
            // the object is merely on LL's move list (sig is also cleared mid-ease): worn
            // attachments of a walking avatar sit there, and would keep the pass busy.
            auto sit = mObjSig.find(obj->getID());
            pending = sit != mObjSig.end() && sit->second.sig == 0
                && !(obj->mDrawable.notNull() && obj->mDrawable->isState(LLDrawable::ON_MOVE_LIST));
            if (pending) LLVKHitch::count("prioPendSig");
        }
        return pending;
    };

    // PER-OBJECT SETTLE (RenderVKPrioPendingSet). A full lap over the outfit records the
    // objects still pending; afterwards only THOSE are revisited each frame (with a
    // backoff for any that stay pending), and the whole outfit is re-walked every 30
    // frames at a low budget (self-heal: a linkset child that arrived late, a missed
    // change) or at once when the outfit changes. Real edits to worn objects arrive at
    // once through the moved/changed queues. Before: the pass settled only after a
    // lap with NOTHING pending, so one object that stayed pending (e.g. a worn
    // invisiprim, whose VKI_NOT_DRAWN face counted as transient) kept ~370 objects
    // re-signed every frame (~1 ms). 0 = that lap-level settle.
    static LLCachedControl<bool> s_pend_set(gSavedSettings, "RenderVKPrioPendingSet", true);
    const bool pend_set = s_pend_set;
    const U32 now = (U32)gFrameCount;
    const size_t n = objs.size();
    const U32 gen = LLViewerJointAttachment::sOccupancyGen;
    if (n != mPrioLastObjCount)
    {
        // Own outfit changed size (attach/detach, a linkset child arrived): full lap at
        // the loading budget.
        mPrioLastObjCount = n; mPrioGen = gen;
        mPrioSettled = false; mPriorityCursor = 0; mPrioLapCount = 0; mPrioLapClean = true;
        mPrioVerifyLap = false; mPrioLapPending.clear();
    }
    else if (gen != mPrioGen || (mPrioSettled && (U32)(now - mPrioSettledFrame) >= 30u))
    {
        // Self-heal re-walk / some avatar's attachment set changed (sOccupancyGen is
        // global; an equal-count swap of our own lands here too): verify lap, low budget.
        mPrioGen = gen;
        if (mPrioSettled)
        {
            mPrioSettled = false; mPriorityCursor = 0; mPrioLapCount = 0; mPrioLapClean = true;
            mPrioVerifyLap = true; mPrioLapPending.clear();
        }
    }

    const U64 t0 = LLTimer::getTotalTime();
    SweepStats st;
    if (!mPrioSettled)
    {
        // 1 ms/frame while loading (a full outfit laps in a few frames); a verify lap of
        // a settled outfit spreads over several frames instead of spiking one.
        const U64 budget_us = (pend_set && mPrioVerifyLap) ? 250 : 1000;
        if (mPriorityCursor >= n) mPriorityCursor = 0;
        for (size_t i = 0; i < n; ++i)
        {
            if (i > 0 && (LLTimer::getTotalTime() - t0) > budget_us) break;
            LLViewerObject* obj = objs[mPriorityCursor].get();
            if (++mPriorityCursor >= n) mPriorityCursor = 0;
            if (visit(obj, st))
            {
                mPrioLapClean = false;
                if (pend_set) mPrioLapPending.push_back(obj->getID());
            }
            if (++mPrioLapCount < n) continue;
            // Lap complete.
            mPrioLapCount = 0;
            if (mPrioLapClean) LLVKHitch::count("prioSettled");
            if (mPrioLapClean || pend_set)
            {
                mPrioSettled = true; mPrioSettledFrame = now; mPrioVerifyLap = false;
                // The lap's pending objects become the pending set (keeping the backoff
                // streak of those already in it).
                std::unordered_map<LLUUID, PrioPend> old;
                for (const PrioPend& p : mPrioPending) old.emplace(p.id, p);
                mPrioPending.clear();
                std::sort(mPrioLapPending.begin(), mPrioLapPending.end());
                mPrioLapPending.erase(std::unique(mPrioLapPending.begin(), mPrioLapPending.end()), mPrioLapPending.end());
                for (const LLUUID& id : mPrioLapPending)
                {
                    auto it = old.find(id);
                    mPrioPending.push_back(it != old.end() ? it->second : PrioPend{ id, now + 1, 0 });
                }
                mPrioLapPending.clear();
                break;
            }
            mPrioLapClean = true;   // lap-level settle (RenderVKPrioPendingSet off): lap again
        }
    }
    else if (!mPrioPending.empty())
    {
        // PENDING SET: revisit only the objects the last lap left pending; drop each as
        // soon as a visit finds nothing pending. One that stays pending (decode in
        // flight, a mesh still downloading) is revisited every frame for 8 visits, then
        // backs off to every 2..32 frames -- its arrival also wakes it through the
        // changed queue (meshDecodeWake / captureWake), so the backoff costs no latency
        // there; a perpetual one costs ~nothing and is named in the log.
        constexpr U64 BUDGET_US = 1000;
        size_t w = 0;
        for (size_t r = 0; r < mPrioPending.size(); ++r)
        {
            PrioPend p = mPrioPending[r];
            bool keep = true;
            if ((S32)(now - p.next) >= 0 && (r == 0 || (LLTimer::getTotalTime() - t0) <= BUDGET_US))
            {
                // Strong ref for the visit (see the objs comment above): a visit's side
                // effects may kill the object, and it is read again afterwards.
                LLPointer<LLViewerObject> op = gObjectList.findObject(p.id);
                LLViewerObject* obj = op.get();
                if (!obj || obj->isDead()) keep = false;
                else if (!visit(obj, st)) keep = false;
                else
                {
                    ++p.streak;
                    const U32 delay = p.streak <= 8 ? 1u : (1u << llmin<U32>(p.streak - 8u, 5u));
                    p.next = now + delay;
                    if (p.streak == 16)
                    {
                        LLVKHitch::count("prioPendStuck");
                        static LLCachedControl<bool> s_sec(gSavedSettings, "RenderVKSectionLog", false);
                        static F64 s_next_log = 0.0;
                        const F64 t = LLTimer::getElapsedSeconds();
                        if (s_sec && t > s_next_log)
                        {
                            s_next_log = t + 5.0;
                            auto sit = mObjSig.find(p.id);
                            LL_INFOS("VKIngest") << "priority pass: worn object pending for 16 visits: "
                                << vkPrioPendingWhy(obj, sit != mObjSig.end() ? sit->second.missing : 0)
                                << " (face states: LLFace::VKI_*)" << LL_ENDL;
                        }
                    }
                }
            }
            if (keep) mPrioPending[w++] = p;
        }
        mPrioPending.resize(w);
    }
    if (!mPrioPending.empty()) LLVKHitch::count("prioPendingSet", (unsigned)mPrioPending.size());
}

// One object of the ingest sweep (terrain/water dispatch, decoded-volume checks, LOD
// drive, mesh fetch re-issue, sculpt regen, face bounds, per-face registration).
// Shared by the list-order sweep and the self/HUD priority pass.
// Cheap per-object ingest signature (see mObjSig). Everything registerCleanFace and the
// bounds regen read for a settled object; not a content hash of the geometry itself --
// geometry changes arrive as a new volume, a changed LOD/detail/sculpt level, or
// changed per-face vertex/index counts.
// GPU-desired LOD band (#88): the level the GPU cull will select, same formula as
// clean_cull.comp. Shared by the full signature and the C1 pre-signature.
static U32 vkGpuLodBand(LLViewerObject* obj, LLVolume* v)
{
    const F32 radius = v->mLODScaleBias.scaledVec(obj->getScale()).length();
    const F32 dist = llmax((obj->getRenderPosition() - LLViewerCamera::getInstance()->getOrigin()).length(), 0.001f);
    const F32 tan_angle = ll_round((LLVOVolume::sLODFactor * radius) / dist, 0.01f);
    return (tan_angle <= 0.03f) ? 0u : (tan_angle <= 0.06f) ? 1u : (tan_angle <= 0.24f) ? 2u : 3u;
}

// C1 PRE-SIGNATURE: what the sweep's HEAVY pre-steps read (see ObjSig::pre). Cheap: no
// TE / face / texture walk. `band` receives the GPU LOD band.
static U64 vkIngestPreSignature(LLViewerObject* obj, LLDrawable* dr, LLVOVolume* vv, LLVolume* v, U32& band)
{
    U64 h = 0x84222325cbf29ce4ull;
    auto mix = [&h](U64 x) { h ^= x + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); };
    auto mixf = [&mix](F32 f) { U32 u; std::memcpy(&u, &f, 4); mix(u); };
    mix((U64)(uintptr_t)v);
    mixf(v->getDetail());
    mix((U64)v->isMeshAssetLoaded() | ((U64)(U32)v->getNumVolumeFaces() << 8) | ((U64)(U32)v->getNumFaces() << 32));
    mix((U64)(U32)vv->getLOD() | ((U64)obj->getNumTEs() << 8) | ((U64)(U32)dr->getNumFaces() << 16)
        | ((U64)(U32)vv->getNumFaces() << 40));
    mix((U64)(uintptr_t)obj->getParent());
    mixf(LLVOVolume::sLODFactor); mixf(LLVOVolume::sDistanceFactor);
    mix(GMDXRegionCapture::lodEpoch());   // <GMDX> the region map capture forced / released max LOD
    band = vkGpuLodBand(obj, v);
    mix(band);
    // LL's own LOD (calcLOD) runs on distance * sDistanceFactor with a near ramp, not the
    // GPU formula: re-run it whenever the radius/distance ratio moved by 1/8 octave.
    const F32 radius = v->mLODScaleBias.scaledVec(obj->getScale()).length();
    const F32 dist = llmax((obj->getRenderPosition() - LLViewerCamera::getInstance()->getOrigin()).length(), 0.001f);
    const F32 ratio = llmax(radius, 1e-6f) / dist;
    mix((U64)(S64)llfloor(log2f(ratio) * 8.f));
    return h | 1ull;   // never 0 (0 = "no pre-signature")
}

static U64 vkIngestSignature(LLViewerObject* obj, LLDrawable* dr)
{
    U64 h = 0xcbf29ce484222325ull;
    auto mix = [&h](U64 v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); };
    auto mixf = [&mix](F32 f) { U32 u; std::memcpy(&u, &f, 4); mix(u); };
    LLVolume* v = obj->getVolume();
    mix((U64)(uintptr_t)v);
    if (v)
    {
        mixf(v->getDetail());
        mix((U64)(U32)v->getSculptLevel() | ((U64)v->isMeshAssetLoaded() << 32));
        const S32 nvf = v->getNumVolumeFaces();
        mix((U64)nvf);
        for (S32 i = 0; i < nvf; ++i)
        {
            const LLVolumeFace& vf = v->getVolumeFace(i);
            mix((U64)(U32)vf.mNumVertices | ((U64)(U32)vf.mNumIndices << 32));
        }
    }
    if (LLVOVolume* vv = dr->getVOVolume())
    {
        mix((U64)(U32)vv->getLOD() | ((U64)(vv->getSkinInfo() != nullptr) << 32)
            | ((U64)vv->isRiggedMesh() << 33) | ((U64)vv->isAnimatedObject() << 34));
        // Rigged-routing inputs (registerCleanFace: rigged = animesh control avatar
        // PLAYING, or worn). A control avatar created / starting to play after the first
        // visit must re-ingest, or the faces stay classified static (#88 late data).
        LLControlAvatar* cav = vv->getControlAvatar();
        mix((U64)(uintptr_t)cav | ((U64)(cav && cav->mPlaying) << 63));
    }
    // Late-arriving pose inputs (#88 / propellers). GPU target omega is derived from the
    // object's OR its linkset root's angular velocity (composeOwnedPose); an omega that
    // arrives or changes without a pose change never re-ingested the instance, so its
    // spin record stayed stale until the full-visit lap. Also the parent identity.
    {
        const LLVector3 w = obj->getAngularVelocity();
        mixf(w.mV[0]); mixf(w.mV[1]); mixf(w.mV[2]);
        LLViewerObject* par = (LLViewerObject*)obj->getParent();
        mix((U64)(uintptr_t)par);
        if (par)
        {
            const LLVector3 pw = par->getAngularVelocity();
            mixf(pw.mV[0]); mixf(pw.mV[1]); mixf(pw.mV[2]);
        }
        mix((U64)obj->isAttachment() | ((U64)obj->isHUDAttachment() << 1));
        // #919: a HUD instance's pose bakes its attach point's HUD-space matrix, which
        // scales with the HUD aspect (mScreen joint): a layout change re-ingests it even
        // if the moved notice (noteHudLayoutChanged) was missed.
        if (obj->isHUDAttachment() && gViewerWindow) mixf(gViewerWindow->getHudAspectRatio());
    }
    const LLVector3 p = obj->getPosition(); const LLQuaternion q = obj->getRotation();
    const LLVector3 sc = obj->getScale();
    mixf(p.mV[0]); mixf(p.mV[1]); mixf(p.mV[2]);
    mixf(q.mQ[0]); mixf(q.mQ[1]); mixf(q.mQ[2]); mixf(q.mQ[3]);
    mixf(sc.mV[0]); mixf(sc.mV[1]); mixf(sc.mV[2]);
    mix((U64)obj->getNumTEs() | ((U64)(U32)obj->getNumFaces() << 8));
    // BOM: faces textured with an IMG_USE_BAKED_* sentinel sample the avatar's CURRENT
    // bake; when the avatar rebakes, the resolved texture changes but nothing else does.
    for (U8 i = 0; i < obj->getNumTEs(); ++i)
    {
        const LLTextureEntry* te = obj->getTE(i);
        if (te && LLAvatarAppearanceDefines::LLAvatarAppearanceDictionary::isBakedImageId(te->getID()))
        {
            LLViewerTexture* bake = obj->getBakedTextureForMagicId(te->getID());
            mix(bake ? (U64)bake->getID().getCRC32() | ((U64)bake->getDiscardLevel() << 40) : 0ull);
        }
    }
    // GPU-DESIRED LOD (#88). The converge block in registerCleanFace fetches/decodes/
    // captures the level the GPU cull will select (same formula). LL's mLOD is stale
    // under VK, so without this a settled object never re-converged when the camera
    // approached: a face empty at the far level stayed missing up close.
    if (v)
        mix(vkGpuLodBand(obj, v));
    const S32 nf = dr->getNumFaces();
    mix((U64)nf);
    for (S32 fi = 0; fi < nf; ++fi)
    {
        LLFace* f = dr->getFace(fi);
        mix((U64)(uintptr_t)f);
        if (!f) continue;
        mix((U64)(U32)f->getTEOffset());
        if (const LLTextureEntry* te = f->getTextureEntry())
        {
            mix(te->getID().getDigest64());
            const LLColor4& c = te->getColor();
            mixf(c.mV[0]); mixf(c.mV[1]); mixf(c.mV[2]); mixf(c.mV[3]);
            F32 a, b; te->getScale(&a, &b); mixf(a); mixf(b);
            te->getOffset(&a, &b); mixf(a); mixf(b);
            mixf(te->getRotation()); mixf(te->getGlow());
            mix((U64)te->getBumpShinyFullbright() | ((U64)te->hasMedia() << 8)
                | ((U64)te->getTexGen() << 16) | ((U64)te->getFullbright() << 24));
            U64 mid = 0; std::memcpy(&mid, te->getMaterialID().get(), sizeof(mid)); mix(mid);
            mix((U64)(uintptr_t)te->getGLTFMaterial());
            mix((U64)(uintptr_t)te->getGLTFMaterialOverride());
            mix((U64)(uintptr_t)te->getGLTFRenderMaterial());
        }
        for (U32 ch = 0; ch < LLRender::NUM_TEXTURE_CHANNELS; ++ch)
        {
            LLViewerTexture* t = f->getTexture(ch);
            mix((U64)(uintptr_t)t);
            // Alpha analysis lands after the texture (async, and only from a decent-
            // resolution decode): both the strict mask bit and the gradient class must
            // break the settled skip, or a demotion waits kSigFullLaps laps. Each
            // changes a bounded number of times (committed results are sticky).
            if (t) mix((U64)(U8)t->getComponents() | ((U64)t->getIsAlphaMask() << 8)
                       | ((U64)(U8)(LLCleanTexture::instance().alphaGradientClass(t) + 1) << 16));
        }
    }
    return h;
}

void LLCleanIngest::sweepIngestObject(LLViewerObject* obj, SweepStats& st)
{
    if (!obj || obj->isDead()) return;
        {
            // Clean-room terrain ingest.
            if (obj->getPartitionType() == LLViewerRegion::PARTITION_TERRAIN)
            {
                vkIngestTerrainPatch(obj);
                return;
            }
            if (obj->getPartitionType() == LLViewerRegion::PARTITION_WATER
                || obj->getPartitionType() == LLViewerRegion::PARTITION_VOIDWATER)
            {
                vkIngestWaterPlane(obj);
                return;
            }
            if (vkIsVegetation(obj))
            {
                vkIngestVegetation(obj);
                return;
            }
        }
        if (obj->getPCode() != LL_PCODE_VOLUME) return;   // prims/sculpts/mesh
        LLDrawable* dr = obj->mDrawable;
        if (!dr || dr->isDead()) return;
        // GLTF SCENE ASSETS (#39): (node, primitive) instances from the asset, never the
        // per-face volume path (its "volume" is a placeholder; LL's GL renderer skipped
        // these objects too). An object that stopped being one re-registers normally.
        LLCleanGLTFIngest& gi = LLCleanGLTFIngest::instance();
        if (LLCleanGLTFIngest::isGLTFScene(obj))
        {
            ++st.objs;
            gi.ingestObject(obj);
            return;
        }
        if (gi.isTracked(obj->getID()))
        {
            gi.forgetObject(obj->getID());
            mObjSig.erase(obj->getID());   // full per-face visit below
        }
        const bool is_mesh = obj->isMesh();
        if (is_mesh) ++st.mesh_seen;
        if (!obj->getVolume())                              // decoded geometry only
        {
            if (is_mesh) ++st.mesh_novol; else ++st.prim_novol;
            return;
        }
        ++st.objs;
        // C1 PRE-SIGNATURE GATE (VK). The heavy pre-steps below (updateDistance/calcLOD,
        // mesh loadMesh + setVolume, sculpt regen, face-list resync) only matter when
        // something they read changed; a settled object whose cheap pre-signature
        // matches the one recorded after its last heavy visit skips them. Always heavy:
        // active drawables (attachments, animesh, movers -- their LOD keys off avatar
        // distance / anim extents), flexis (their update rate reads the drawable
        // distance), image sculpties (sculpt-texture arrival has no hook), meshes whose
        // asset or skin info is still pending, objects mid-ease, objects not settled
        // (sig 0), and every kPreFullLaps laps per object (self-heal). Change hooks
        // (noteObjectChanged) clear `pre`, so an edit is never gated.
        bool heavy = true;
        bool pre_fresh = true;   // no settled history: setVolume runs unconditionally
        LLVOVolume* pre_vv = vkSweepPreSigOn() ? dr->getVOVolume() : nullptr;
        if (pre_vv)
        {
            LLVolume* pv = obj->getVolume();
            U32 band = 0xFF;
            const U64 pre = vkIngestPreSignature(obj, dr, pre_vv, pv, band);
            auto pit = mObjSig.find(obj->getID());
            if (pit != mObjSig.end())
            {
                ObjSig& pos = pit->second;
                if (pos.band != 0xFF && pos.band != (U8)band) ++st.band_changes;
                pos.band = (U8)band;
                pre_fresh = pos.sig == 0 || pos.pre == 0;
                const bool always_heavy = dr->isActive() || pre_vv->isFlexible() || obj->isAttachment()
                    || pre_vv->isAnimatedObject() || dr->isState(LLDrawable::ON_MOVE_LIST)
                    || (is_mesh ? (!pv->isMeshAssetLoaded()
                                   || (!pre_vv->getSkinInfo() && !pre_vv->isSkinInfoUnavaliable()))
                                : pre_vv->isSculpted());
                heavy = always_heavy || pre_fresh || pos.pre != pre || (mSweepLap - pos.pre_lap) >= kPreFullLaps;
            }
        }
        if (!heavy) ++st.heavy_skipped;
        // Drive LL's LOD for this object against the real world camera. With
        // registerFace disabled, LL's render-driven LOD update (pipeline's
        // updateDistance path) skips some of our objects, so they latch on a low
        // LOD and never fetch the higher-detail mesh ("stuck on low triangles,
        // never resolves even when zoomed in"). WE render it, so WE tell LL which
        // LOD our camera wants; calcLOD -> markRebuild(REBUILD_VOLUME) then makes LL
        // fetch the mesh + rebuild the volume, and registerCleanFace's geometry-
        // mismatch re-capture picks up the finer geometry. Naturally throttled --
        // each object is visited once per cursor cycle, not every frame.
        if (!heavy) {}   // C1: pre-signature unchanged (static drawables only)
        else if (!dr->isActive())
            dr->updateDistance(LLCullCamera::get(), false);   // (#479: the culling camera)
        else
        {
            // ACTIVE DRAWABLES (attachments, animesh, moving linksets). GL drives their
            // LOD from the cull (the spatial bridge -> updateDistance), diverted under VK,
            // so they latched on their creation LOD -- rigged bodies/clothes stuck on
            // the (often deliberately tiny) low LOD: "too small / deformed". Drive it.
            // Rigged volumes also need LLDrawable::RIGGED, which only rebuildGeom sets:
            // calcLOD keys the avatar-box distance/radius off it (without it a rigged
            // mesh LODs by its arbitrary prim scale -> low LOD).
            if (LLVOVolume* rv = dr->getVOVolume())
            {
                if (rv->isRiggedMesh() && !dr->isState(LLDrawable::RIGGED)
                    && (rv->isAttachment() || rv->isAnimatedObject()))
                {
                    dr->setState(LLDrawable::RIGGED);
                    LLDrawable* droot = dr->getRoot();
                    if (droot && droot != dr) droot->setState(LLDrawable::RIGGED_CHILD);
                }
            }
            dr->updateDistance(LLCullCamera::get(), false);   // (#479: the culling camera)
            vkEnsureControlAvatar(dr->getVObj());
        }
        // VK MESH FETCH (request side): the setVolume(params, mLOD) that actually
        // ISSUES the mesh fetch at the LOD calcLOD wants lives in the gPipeline-gated
        // rebuild (LLVOVolume::updateGeometry), which VK skips. updateDistance->calcLOD
        // above sets mLOD high, but the higher-LOD mesh is never requested -- only the
        // arrival-path LOD-0 load fired, leaving most mesh at placeholder (verts plateau
        // ~296k, mesh_kbytes=0). Re-issue setVolume here: while the volume's actual detail
        // is below mLOD, LLPrimitive::setVolume sees the change and calls gMeshRepo.loadMesh;
        // once resolved (detail==mLOD, asset loaded) it cheaply no-ops. loadMesh dedups +
        // high-water throttles, so per-pass re-issue is safe, and it also adopts the data
        // once a fetch completes (notifyMeshLoaded sets mSculptChanged, re-entering here).
        if (is_mesh)
        {
            if (heavy)
            {
                LLVOVolume* vvol = dr->getVOVolume();
                LLVolume*   v    = vvol ? vvol->getVolume() : nullptr;
                if (v)
                {
                    ++st.mesh_reissue;
                    // The real-triangle mesh asset isn't loaded: request it DIRECTLY.
                    // setVolume(params, mLOD) silently no-ops when the volume's detail
                    // already == mLOD (LLPrimitive::setVolume returns false), so it never
                    // reaches the loadMesh call for the stuck meshes -- go straight to
                    // gMeshRepo.loadMesh (it dedups pending requests + high-water throttles
                    // internally, so per-pass re-issue is safe and won't storm).
                    if (!v->isMeshAssetLoaded())
                    {
                        gMeshRepo.loadMesh(vvol, v->getParams(), vvol->getLOD());
                        ++st.mesh_req;
                    }
                    // Adopt the higher-detail data once a fetch completes: notifyMeshLoaded
                    // sets mSculptChanged, so this setVolume re-enters and swaps it in.
                    // C1: only when there is something to adopt -- the asset or skin info
                    // is not loaded yet, the volume's detail differs from the wanted LOD,
                    // or the object has no settled history (first visit / after a change
                    // hook). (mSculptChanged is never cleared under VK -- updateGeometry
                    // does that -- so every call re-ran the full adopt path.)
                    const bool want_setvol = !pre_vv || pre_fresh || !v->isMeshAssetLoaded()
                        || LLVolumeLODGroup::getVolumeDetailFromScale(v->getDetail()) != vvol->getLOD()
                        || (!vvol->getSkinInfo() && !vvol->isSkinInfoUnavaliable());
                    if (want_setvol) vvol->setVolume(v->getParams(), vvol->getLOD());
                    else ++st.setvol_skipped;
                }
            }
        }
        else
        {
            // VK SCULPTIE REGEN. A sculptie's real shape is generated by
            // LLVOVolume::sculpt() inside setVolume, which only re-runs through the
            // gPipeline-gated rebuild (updateGeometry) that VK skips. So a sculpt
            // whose texture streams in AFTER object creation is stuck at its initial
            // placeholder (blobby low-discard) shape -- "sculpts are broken". Drive
            // the regeneration ourselves: sculpt() cheaply no-ops when the volume's
            // sculpt discard already matches the resolved texture, else rebuilds the
            // LLVolume faces from the streamed sculpt data; registerCleanFace's
            // geometry-mismatch check then re-captures the corrected geometry.
            if (heavy)
            {
                LLVOVolume* vvol = dr->getVOVolume();
                if (vvol && vvol->isSculpted())
                {
                    LLVolume* v = vvol->getVolume();
                    const U8 stype = v ? (U8)(v->getParams().getSculptType() & LL_SCULPT_TYPE_MASK) : 0;
                    if (v && stype != LL_SCULPT_TYPE_MESH && stype != LL_SCULPT_TYPE_GLTF)
                    {
                        // Drive stock's sculpt SEQUENCING: setVolume builds the LOD-
                        // sized base grid, THEN sculpt() displaces it. calcLOD (which
                        // raises tessellation) runs in the diverted display path, so
                        // under VK an image sculpt is frozen at creation detail -> a
                        // too-coarse grid, and displacing it fans plane/cylinder stitch
                        // seams (the sim-surround spikes) + leaves it low-poly. The
                        // method rebuilds to detail 3 ONCE (gated on the sculpt raw
                        // being resident so it can't drop to a placeholder), else just
                        // refreshes. Amortize: detail-3 regen is heavy -> cap raises/frame.
                        static U32 s_frame = 0xFFFFFFFFu, s_raised = 0;
                        if (gFrameCount != s_frame) { s_frame = gFrameCount; s_raised = 0; }
                        if (s_raised < 8u)
                        {
                            if (vvol->vkEnsureSculptDetail(3)) ++s_raised;
                        }
                        else
                        {
                            vvol->sculpt();   // budget spent; refresh, defer raises
                        }
                    }
                }
            }
        }
        // FACE-LIST RESYNC (VK, #88). LL sizes a drawable's LLFace list from the TE
        // count at createDrawable, and the TE count of a mesh whose asset is NOT loaded
        // yet is its placeholder profile's face count (LLPrimitive::setVolume ->
        // setNumTEs(volume->getNumFaces())). When the asset lands LL repairs both in
        // LLVOVolume::lodOrSculptChanged (setNumTEs + regenFaces) -- reachable only from
        // updateGeometry via the pipeline rebuild queue, which Vulkan never runs (display()
        // is diverted; markRebuild queues nothing). So a mesh object created BEFORE its
        // asset arrived kept the placeholder face list forever: its extra faces had no
        // LLFace and were never ingested -- timing-dependent missing parts (tires, doors).
        // Do LL's repair here, on our schedule. Flexi (mVolumeImpl) excluded as in LL.
        if (heavy)
        {
            LLVOVolume* fv = dr->getVOVolume();
            LLVolume*   v  = fv ? fv->getVolume() : nullptr;
            auto rs_it = mObjSig.find(obj->getID());
            const bool resync_budget = rs_it == mObjSig.end() || rs_it->second.resyncs < 8;
            if (fv && v && !fv->isFlexible() && resync_budget)
            {
                bool resynced = false;
                const S32 old_tes = fv->getNumTEs();
                const S32 old_df  = dr->getNumFaces();
                if (is_mesh && v->isMeshAssetLoaded() && v->getNumFaces() > 0
                    && fv->getNumTEs() != v->getNumFaces())
                {
                    fv->setNumTEs((U8)llmin(v->getNumFaces(), 255));
                    resynced = true;
                }
                if (dr->getNumFaces() != (S32)fv->getNumTEs() || fv->getNumFaces() != (S32)fv->getNumTEs())
                {
                    fv->regenFaces();
                    for (S32 fi = dr->getNumFaces(); fi < old_df; ++fi)
                        LLCleanInstanceStore::instance().release(obj->getID(), fi);
                    resynced = true;
                }
                if (resynced)
                {
                    ObjSig& ros = mObjSig[obj->getID()];
                    ros.sig = 0;
                    ++ros.resyncs;
                    static U64 s_last = 0; static U32 s_n = 0;
                    ++s_n;
                    const U64 now = LLTimer::getTotalTime();
                    if (now - s_last > 5ull * 1000000ull)
                    {
                        std::ostringstream tes_os;
                        llvkTEStashStats(tes_os);   // (LLVOVolume::setNumTEs: TE entries kept across LODs)
                        LL_INFOS("VKIngest") << "face list resynced (" << s_n << " in last period), e.g. local="
                            << obj->getLocalID() << " " << obj->getID() << " tes " << old_tes << "->"
                            << (S32)fv->getNumTEs() << " drawable_faces " << old_df << "->" << dr->getNumFaces()
                            << " vol_faces=" << v->getNumFaces() << " mesh_loaded=" << (v->isMeshAssetLoaded() ? 1 : 0)
                            << " | " << tes_os.str()
                            << LL_ENDL;
                        s_last = now; s_n = 0;
                    }
                }
            }
        }
        // C1: record the pre-signature the heavy steps left behind (LOD, volume, face
        // counts after calcLOD / setVolume / resync), so the next visit can compare.
        if (pre_vv && heavy && dr->getVOVolume() == pre_vv)
        {
            if (LLVolume* pv2 = obj->getVolume())
            {
                U32 b2 = 0xFF;
                const U64 p2 = vkIngestPreSignature(obj, dr, pre_vv, pv2, b2);
                ObjSig& pos = mObjSig[obj->getID()];
                pos.pre = p2; pos.pre_lap = mSweepLap; pos.band = (U8)b2;
            }
        }
        // SETTLED SKIP (VK): unchanged since the last full visit -> nothing below would
        // do anything (bounds regen + per-face dedup'd re-registration).
        const U64 sig = vkIngestSignature(obj, dr);
        ObjSig& os = mObjSig[obj->getID()];
        if (os.sig == sig && os.sig != 0 && mSweepLap - os.lap < kSigFullLaps)
        {
            ++st.skipped;
            ++os.skips;
            os.skip_frame = (U32)gFrameCount;
            return;
        }
        // CHURN DIAGNOSTIC (section log): why this object takes a full visit --
        // 1 signature changed, 2 still easing (previous visit was mid-glide),
        // 3 periodic full lap, 4 first visit.
        os.why = os.sig == 0 ? (os.lap ? 2 : 4) : (os.sig != sig ? 1 : 3);
        if (os.why == 1 || os.why == 4) ++st.sig_changes;   // real work the safety net found
        {
            static LLCachedControl<bool> s_sec(gSavedSettings, "RenderVKSectionLog", false);
            if (os.fulls++ == 0 && s_sec) mChurnIds.push_back(obj->getID());
        }
        os.sig = sig; os.lap = mSweepLap;
        os.full_frame = (U32)gFrameCount;
        // Mid-ease (LL gliding a scripted move): the signature reads the FINAL
        // object pose, but registration below reads the eased drawable pose --
        // recording it as settled froze the instance mid-glide (HUD parts stuck
        // half-moved) until the full-visit lap. Keep visiting until it settles.
        if (dr->isState(LLDrawable::ON_MOVE_LIST)) os.sig = 0;
        const S32 nf = dr->getNumFaces();
        vkRegenFaceBounds(dr, nf);   // (C9: shared with drainChangedQueue)
        for (S32 fi = 0; fi < nf; ++fi)
        {
            LLFace* facep = dr->getFace(fi);
            if (!facep) continue;
            LLCleanIngest::registerCleanFace(facep, /*force_reingest=*/false);
            ++st.faces;
        }
        // INCOMPLETE OBJECTS ARE NOT SETTLED (VK, #88): see vkObjectTransient.
        U16 missing = 0;
        const bool transient = vkObjectTransient(obj, dr, nf, missing);
        auto it = mObjSig.find(obj->getID());
        if (it != mObjSig.end())
        {
            it->second.missing = missing;
            if (transient) it->second.sig = 0;
        }
}

void LLCleanIngest::vkIngestTick()
{
    { VK_HITCH_SCOPE("ing_priority"); vkPriorityIngest(); }
    selfRegisterSweep();   // own scope: sweep_total
    { VK_HITCH_SCOPE("ing_gltf"); vkGLTFTick(); }
    { VK_HITCH_SCOPE("ing_sysavi"); LLCleanSysAvatar::instance().tick(); }   // system avatar bodies
    { VK_HITCH_SCOPE("ing_gc"); vkGeometryGC(); }
    { VK_HITCH_SCOPE("ing_diag"); vkIngestDiagTick(); vkVehicleDiagTick(); }
}

void LLCleanIngest::vkGLTFTick()
{
    LLCleanGLTFIngest::instance().tick();
}

// Effective geometry-GC budget. RenderVKGeometryBudgetMB is the floor: while system RAM
// (memory governor COMFORTABLE) and video memory (VRAM ladder calm, no rung engaged) both
// have room, the budget scales to min(VRAM budget / 8, total RAM / 32) -- unreferenced
// geometry is only worth reclaiming when memory is short, and the reclaim pass is what
// costs (it used to run every few seconds a little over a 512 MB floor, freeing ~15 MB a
// pass, re-captured soon after). Pressure (TIGHT / CRITICAL, or a ladder step) drops it
// back to the floor at once and the next pass reclaims; it relaxes again on its own when
// the pressure ends (nothing is written to settings). Still a bound: a fraction of the
// device's budget and of RAM.
static U64 vk_geometry_gc_budget(U32 floor_mb, bool& relaxed)
{
    const U64 floor = (U64)floor_mb << 20;
    const LLVKVramSummary v = LLVulkanContext::instance().vramSummary();
    const U64 ram = LLMemoryGovernor::getTotalRAM();
    relaxed = v.valid && v.mode == 0u && v.rung == 0u && ram > 0
              && LLMemoryGovernor::getState() == LLMemoryGovernor::COMFORTABLE;
    return relaxed ? std::max(floor, std::min<U64>(v.budget / 8u, ram / 32u)) : floor;
}

void LLCleanIngest::vkGeometryGC()
{
    // VK twin of the GL mesh-GC block in the frame loop. Throttled (every 300
    // frames) and PRESSURE-GATED: geometry is only reclaimed once resident mesh
    // bytes exceed the effective budget (vk_geometry_gc_budget) or the VRAM ladder trims,
    // so a normal scene never churns re-captures. keep_age is long (3600 frames, about
    // 30-60 s): only assets NO live instance's LOD group has named for that long
    // are freed. garbageCollect clears their CPU mirrors (-> VK packer counts the
    // dead ranges as garbage and repacks) and unpublishes the freed slot_ids from
    // their LOD groups so a recycled id can never be drawn through a stale group.
    // SLICED: the budget pass walks the asset table RenderVKGeometryGCSliceMs per frame
    // (LLCleanMeshAsset::garbageCollectSlice: in-use from the instance store's group
    // refcounts, no instance walk) -- the whole-table pass was 17-320 ms in one frame.
    // BACKOFF: a busy sim can sit above the budget with nothing old enough to free; a
    // pass that frees under 64 MB doubles the interval (up to 4800 frames); a pass
    // freeing >= 64 MB resets it.
    // LIVENESS (#737): under the budget the same sliced pass still runs every
    // kLivePassSec, with keep_age = RenderVKAssetReleaseGraceSec of wall time counted from
    // the moment the last instance of a group publishing the slot let go
    // (LLCleanMeshAsset::mGroupUnrefFrame). A left sim's geometry -- slots, then their
    // emptied LOD groups (gcLodGroupsSlice), the CPU mirror, then the packed VK buffers
    // (the renderer's liveness compaction) -- goes within ~1 min instead of waiting for a
    // budget that is now GBs. The budget stays as the pressure backstop (shorter ages).
    static LLCachedControl<bool> s_on(gSavedSettings, "RenderVKGeometryGC", true);
    static LLCachedControl<F32>  s_grace_s(gSavedSettings, "RenderVKAssetReleaseGraceSec", 30.f);
    constexpr F64 kLivePassSec = 10.0;
    static F64 s_live_last = 0.0;
    static LLCachedControl<U32>  s_budget_mb(gSavedSettings, "RenderVKGeometryBudgetMB", 512);
    static LLCachedControl<F32>  s_slice_ms(gSavedSettings, "RenderVKGeometryGCSliceMs", 1.f);
    static U32 s_last = 0;
    static U32 s_interval = 300;
    static U32 s_last_groups = 0;
    const U32 frame = (U32)gFrameCount;
    // #737 liveness audit (diagnostic): rows whose object is gone, counted in the
    // renderer's per-minute CleanRelease line. ~1k rows a frame (one lookup per object).
    { VK_HITCH_SCOPE("vk_orphanAudit"); LLCleanInstanceStore::instance().auditOrphansSlice(1024u, frame); }
    if (!s_on) return;
    LLCleanMeshAsset& ms = LLCleanMeshAsset::instance();
    const F64 slice_s = llmax((F32)s_slice_ms, 0.1f) * 0.001;
    // LOD GROUPS: refcounted by the instance store, so freeing the unused ones needs no
    // instance walk -- a scan of the group table, independent of the geometry budget
    // (groups go once their slots were freed: here, or by an owner's freeAsset). One pass
    // every 600 frames, 8192 group ids per frame.
    // Retired keys (a resized flexi's old geometry): freed once unreferenced. Cheap when
    // the queue is empty or nothing is due.
    ms.reapRetiredAssets(frame);
    static bool s_group_pass = false;
    static U32 s_group_freed = 0;
    if (!s_group_pass && (frame - s_last_groups) >= 600u)
    {
        s_group_pass = true;
        s_group_freed = 0;
    }
    if (s_group_pass)
    {
        VK_HITCH_SCOPE("vk_meshGroupGC");
        if (ms.gcLodGroupsSlice(frame, 8192u, s_group_freed))
        {
            s_group_pass = false;
            s_last_groups = frame;
            if (s_group_freed)
                LL_DEBUGS("CleanMeshGC") << "VK freed " << s_group_freed << " unused LOD groups" << LL_ENDL;
        }
    }
    // VRAM SOFT LADDER (#405 mesh detail / far geometry, LLCleanMeshAsset::
    // setVramPressureTrim): the renderer is short of video memory and has lowered the
    // effective LOD factor and/or draw distance. Run every 120 frames whatever the
    // geometry budget, count only the levels instances can still select at the
    // effective LOD factor, and free after ~600 frames unreferenced (still well above the
    // capture -> publish window). The pack then compacts (renderer, trim mode).
    const bool trim = ms.vramPressureTrim();
    static bool s_was_trim = false;
    if (trim && !s_was_trim) s_interval = 120;   // react now, not after a long backoff
    s_was_trim = trim;

    // The sliced budget pass (no trim).
    static bool s_pass = false;
    static bool s_pass_emergency = false;
    static bool s_pass_live = false;     // #737 liveness pass (under budget)
    static U32  s_pass_keep = 3600u;     // keep_age of the pass in progress (frames)
    static U64  s_pass_freed = 0;
    static U64  s_pass_resident = 0;
    static U64  s_pass_budget = 0;
    static U32  s_pass_frames = 0;
    static F32  s_pass_max_ms = 0.f;
    static bool s_relaxed_logged = false, s_relaxed_known = false;
    auto budget_slice = [&]() -> bool
    {
        VK_HITCH_SCOPE("vk_meshGC");
        LL_PROFILE_ZONE_NAMED_CATEGORY_PIPELINE("clean:vkMeshGCSlice");
        LLTimer t;
        // EMERGENCY (> 2x budget, e.g. a script minting new shapes / mesh ids faster than a
        // 1-minute age can retire them): shorter keep_age. Liveness: the release grace.
        const bool done = ms.garbageCollectSlice((U32)gFrameCount, s_pass_keep, slice_s, s_pass_freed);
        ++s_pass_frames;
        s_pass_max_ms = llmax(s_pass_max_ms, t.getElapsedTimeF32().value() * 1000.f);
        if (!done) return false;
        s_pass = false;
        const U64 freed = s_pass_freed;
        if (s_pass_live)
            s_interval = 300u;   // (liveness passes pace themselves by kLivePassSec)
        else
            s_interval = (freed >= (64ull << 20) || (s_pass_emergency && freed > 0)) ? 300u
                                                                                     : std::min<U32>(s_interval * 2, 4800u);
        const U32 recaptured = ms.takeGcRecaptures();
        if ((s_pass_live && freed >= (1ull << 20)) || (!s_pass_live && freed > 0) || recaptured > 0)
        {
            LL_INFOS("CleanMeshGC") << "VK freed_MB=" << (freed >> 20) << " resident_MB_before=" << (s_pass_resident >> 20)
                                    << " resident_MB_after=" << (ms.residentGeometryBytes() >> 20) << " budget_MB="
                                    << (s_pass_budget >> 20) << " assets=" << ms.assetCount() << " slots=" << ms.slotCount()
                                    << " recaptured_after_gc=" << recaptured << " (" << s_pass_frames
                                    << " slices, slowest " << s_pass_max_ms << " ms)"
                                    << (s_pass_live ? " (liveness, keep " + std::to_string(s_pass_keep) + " frames)" : std::string())
                                    << (s_pass_emergency ? " (emergency)" : "") << LL_ENDL;
        }
        return true;
    };
    if (s_pass && !trim)
    {
        budget_slice();
        return;
    }
    s_pass = false;   // trim's own full pass covers whatever a budget pass had left
    if ((frame - s_last) < s_interval) return;
    s_last = frame;
    const U64 resident = ms.residentGeometryBytes();
    bool relaxed = false;
    const U64 budget = vk_geometry_gc_budget(s_budget_mb, relaxed);
    if (!s_relaxed_known || relaxed != s_relaxed_logged)
    {
        LL_INFOS("CleanMeshGC") << "geometry GC budget " << (budget >> 20) << " MB ("
                                << (relaxed ? "memory has room: scaled to VRAM / RAM" : "memory pressure: RenderVKGeometryBudgetMB floor")
                                << "), resident " << (resident >> 20) << " MB" << LL_ENDL;
        s_relaxed_logged = relaxed;
        s_relaxed_known = true;
    }
    if (resident < budget && !trim)
    {
        s_interval = 300;
        // #737 liveness pass: free what no live instance has used for the grace.
        const F64 now_s = LLCleanAssetLife::nowSeconds();
        if (now_s - s_live_last < kLivePassSec) return;
        s_live_last = now_s;
        s_pass = true;
        s_pass_live = true;
        s_pass_emergency = false;
        // Floor: a minimised / stalled stretch spans few frames, and the capture -> publish
        // window still needs its frames.
        s_pass_keep = std::max<U32>(600u, LLCleanAssetLife::framesSpanning(llclamp((F32)s_grace_s, 5.f, 600.f), frame));
        s_pass_freed = 0;
        s_pass_resident = resident;
        s_pass_budget = budget;
        s_pass_frames = 0;
        s_pass_max_ms = 0.f;
        budget_slice();
        return;
    }
    if (!trim)
    {
        s_pass = true;
        s_pass_live = false;
        s_pass_emergency = resident > budget * 2;
        // Budget pressure: the fixed ages, but never longer than the release grace.
        s_pass_keep = std::min<U32>(s_pass_emergency ? 1200u : 3600u,
                                    std::max<U32>(600u, LLCleanAssetLife::framesSpanning(llclamp((F32)s_grace_s, 5.f, 600.f), frame)));
        s_pass_freed = 0;
        s_pass_resident = resident;
        s_pass_budget = budget;
        s_pass_frames = 0;
        s_pass_max_ms = 0.f;
        budget_slice();
        return;
    }
    // VRAM ladder trim: a full LOD-aware pass (the in-use set depends on each instance's
    // distance, so it is collected in one go) -- the pressure response.
    VK_HITCH_SCOPE("vk_meshGC");
    LL_PROFILE_ZONE_NAMED_CATEGORY_PIPELINE("clean:vkMeshGC");
    std::unordered_set<U32> in_use;
    {
        const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
        LLCleanInstanceStore::instance().collectInUseMeshSlotsLod(in_use, cam.mV, LLVOVolume::sLODFactor);
    }
    const U32 keep_age = 600u;
    const U64 freed = ms.garbageCollect(in_use, frame, keep_age);
    // Trim mode: at most 600 frames apart (instances keep releasing levels as the camera
    // moves / far objects unload).
    s_interval = freed > 0 ? 120u : std::min<U32>(std::min<U32>(s_interval * 2, 4800u), 600u);
    // Instance id range (#405 scene tables): unloaded objects free their instance ids;
    // the lowest-first free list packs the live ones low, and once the top half of the
    // range is free the store (and the renderer's id-indexed tables after it) shrinks.
    // Every ~600 frames while trimming (an O(capacity) scan).
    {
        static U32 s_last_tail = 0;
        if ((frame - s_last_tail) >= 600u)
        {
            s_last_tail = frame;
            LLCleanInstanceStore& is = LLCleanInstanceStore::instance();
            const U32 cap_before = is.capacity();
            if (const U32 cap_after = is.trimTail())
                LL_INFOS("VKMem") << "VRAM ladder: instance id range " << cap_before << " -> " << cap_after
                                  << " (" << is.liveCount() << " live)" << LL_ENDL;
        }
    }
    // GC thrash probe: assets the GC freed that were captured again within ~20 s (the
    // in-use set and the level the ingest captures disagree). Should stay ~0 under trim.
    const U32 recaptured = ms.takeGcRecaptures();
    if (freed > 0 || recaptured > 0)
    {
        LL_INFOS("CleanMeshGC") << "VK freed_MB=" << (freed >> 20)
            << " resident_MB_before=" << (resident >> 20)
            << " resident_MB_after=" << (ms.residentGeometryBytes() >> 20)
            << " assets=" << ms.assetCount()
            << " slots=" << ms.slotCount() << " recaptured_after_gc=" << recaptured
            << " (VRAM ladder trim)" << LL_ENDL;
    }
}

// ===========================================================================
// SELECT-TO-DIAGNOSE (VKIngestDiag, #88)
// ===========================================================================
// Select an object (or a whole linkset) in build/edit and the log gets one INFO block
// per selected object -- every linkset member, not only the root -- describing exactly
// where its faces stand in the clean ingest (volume / face list / decode / capture /
// slot / LOD group / instance / rigged routing / GPU spin), followed by a one-line
// summary naming the objects that are NOT in the GPU scene and why. Fires only when the
// selection set changes; zero per-frame cost otherwise. Gate: RenderVKIngestDiag.
extern bool cleanCaptureBackoffInfo(const LLUUID& mesh, S32 lod, S32 face, U32& age_frames);

// RenderVKDumpAvatar = an avatar's user name, display name, legacy name or UUID: dump
// the ingest state of every attachment it wears (the selection dump can't reach other
// people's attachments). Once per value; retried every 5 s while the avatar isn't found.
void LLCleanIngest::vkAvatarDumpTick()
{
    static LLCachedControl<std::string> s_who(gSavedSettings, "RenderVKDumpAvatar", "");
    std::string who = s_who;
    LLStringUtil::trim(who);
    if (who.empty()) { mAvDumpDone.clear(); return; }
    if (who == mAvDumpDone) return;
    const F64 now = LLFrameTimer::getTotalSeconds();
    if (now < mAvDumpRetry) return;
    mAvDumpRetry = now + 5.0;

    auto norm = [](std::string v)
    {
        LLStringUtil::toLower(v);
        v.erase(std::remove_if(v.begin(), v.end(), [](char c) { return c == ' ' || c == '.'; }), v.end());
        if (v.size() > 8 && v.compare(v.size() - 8, 8, "resident") == 0) v.resize(v.size() - 8);
        return v;
    };
    const std::string want = norm(who);
    LLUUID want_id;
    want_id.set(who, false);
    LLVOAvatar* found = nullptr;
    for (LLCharacter* ch : LLCharacter::sInstances)
    {
        LLVOAvatar* av = dynamic_cast<LLVOAvatar*>(ch);
        if (!av || av->isDead() || av->isControlAvatar() || av->isUIAvatar()) continue;
        if (want_id.notNull() && av->getID() == want_id) { found = av; break; }
        LLAvatarName n;
        if (!LLAvatarNameCache::get(av->getID(), &n)) continue;
        if (norm(n.getAccountName()) == want || norm(n.getUserName()) == want
            || norm(n.getDisplayName()) == want || norm(n.getLegacyName()) == want)
        { found = av; break; }
    }
    if (!found)
    {
        LL_INFOS("VKIngestDiag") << "RenderVKDumpAvatar: no avatar in view matches \"" << who << "\" (retrying)" << LL_ENDL;
        return;
    }
    std::vector<LLViewerObject*> roots;
    for (const auto& ap : found->mAttachmentPoints)
        if (ap.second)
            for (LLViewerObject* o : ap.second->mAttachedObjects) if (o) roots.push_back(o);
    vkIngestDiagDump(&roots, "avatar " + who + " (" + found->getID().asString() + ") attachments");
    mAvDumpDone = who;
}

void LLCleanIngest::vkIngestDiagTick()
{
    vkAvatarDumpTick();
    static LLCachedControl<bool> s_on(gSavedSettings, "RenderVKIngestDiag", false);
    if (!s_on) return;
    if (!mDiagSelHooked)
    {
        mDiagSelHooked = true;
        LLSelectMgr::getInstance()->mUpdateSignal.connect([this]() { mDiagSelDirty = true; });
        mDiagSelDirty = true;
    }
    if (!mDiagSelDirty) return;
    mDiagSelDirty = false;
    // Selection-set signature: the sorted object ids.
    LLObjectSelectionHandle sel = LLSelectMgr::getInstance()->getSelection();
    std::vector<U32> ids;
    if (sel.notNull())
        for (LLObjectSelection::iterator it = sel->begin(); it != sel->end(); ++it)
            if (LLViewerObject* o = (*it)->getObject()) ids.push_back(o->getLocalID());
    std::sort(ids.begin(), ids.end());
    U64 h = 0xcbf29ce484222325ull;
    for (U32 id : ids) { h ^= id; h *= 0x100000001b3ull; }
    if (ids.empty()) { mDiagSelSig = 0; return; }
    if (h == mDiagSelSig) return;
    mDiagSelSig = h;
    vkIngestDiagDump();
}

void LLCleanIngest::vkIngestDiagDump(const std::vector<LLViewerObject*>* roots, const std::string& label)
{
    LLObjectSelectionHandle sel = LLSelectMgr::getInstance()->getSelection();
    if (!roots && sel.isNull()) return;

    // Every selected node, plus the whole linkset of each (root + all descendants).
    // Actually-selected objects are listed FIRST and marked [SELECTED].
    std::vector<LLViewerObject*> objs;
    std::unordered_set<LLViewerObject*> seen;
    std::unordered_set<LLViewerObject*> selected;
    if (!roots)
        for (LLObjectSelection::iterator it = sel->begin(); it != sel->end(); ++it)
            if (LLViewerObject* o = (*it)->getObject()) selected.insert(o);
    std::function<void(LLViewerObject*)> add = [&](LLViewerObject* o)
    {
        if (!o || !seen.insert(o).second) return;
        objs.push_back(o);
        for (const LLPointer<LLViewerObject>& c : o->getChildren()) add(c.get());
    };
    if (roots)
    {
        for (LLViewerObject* o : *roots) if (o) add(o->getRootEdit() ? o->getRootEdit() : o);
    }
    else
    {
        for (LLObjectSelection::iterator it = sel->begin(); it != sel->end(); ++it)
        {
            LLViewerObject* o = (*it)->getObject();
            if (!o) continue;
            add(o->getRootEdit() ? o->getRootEdit() : o);
            add(o);
        }
    }
    // Actually-selected objects first (stable), then the rest of their linksets.
    std::stable_partition(objs.begin(), objs.end(),
                          [&](LLViewerObject* o) { return selected.count(o) != 0; });
    std::ostringstream sel_ids;
    for (LLViewerObject* o : objs) if (selected.count(o)) sel_ids << (sel_ids.tellp() > 0 ? "," : "") << o->getLocalID();

    LLCleanMeshAsset&     ms = LLCleanMeshAsset::instance();
    LLCleanInstanceStore& is = LLCleanInstanceStore::instance();
    const U32 now = (U32)gFrameCount;
    U32 total_faces = 0;
    std::ostringstream missing_list;
    U32 n_missing = 0;
    std::ostringstream rigged_list;
    U32 n_rigged = 0;

    LL_INFOS("VKIngestDiag") << "==== VKIngestDiag: " << (roots ? label : std::string("selection changed")) << ", " << objs.size()
        << " object(s) (whole linksets), frame=" << now << " sweep_lap=" << mSweepLap
        << " sweep_idle=" << (mSweepIdle ? 1 : 0) << " ====" << LL_ENDL;

    for (LLViewerObject* obj : objs)
    {
        std::ostringstream os;
        std::string short_reason;   // empty = fully in the GPU scene
        os << "obj local=" << obj->getLocalID() << (selected.count(obj) ? " [SELECTED]" : "") << " id=" << obj->getID()
           << " pcode=" << (S32)obj->getPCode();
        {
            LLSelectNode* node = sel.notNull() ? sel->findNode(obj) : nullptr;
            os << " name=\"" << ((node && !node->mName.empty()) ? node->mName : std::string("?")) << "\"";
            // Target omega vs scripted rotation vs neither (raw, from the object updates;
            // LLVOVolume::processUpdateMessage covers full, compressed, terse and cache loads).
            // NOTE: selecting an object makes the SIM zero its omega (and LLSelectMgr zaps it
            // locally on deselect), so omega while selected is always 0 -- read last_nonzero.
            const LLVector3& uw = obj->mVKUpdAngVel;
            const LLVector3& nz = obj->mVKLastNonzeroAngVel;
            os << " | updates: total=" << obj->mVKUpdCount << " since_prev_dump=" << (obj->mVKUpdCount - obj->mVKDiagPrevUpd)
               << " last=" << (obj->mVKUpdFrame ? (S32)(gFrameCount - obj->mVKUpdFrame) : -1) << "f_ago"
               << " omega_last_update=(" << uw.mV[0] << "," << uw.mV[1] << "," << uw.mV[2] << ")"
               << " nonzero_omega_updates=" << obj->mVKOmegaUpdates << " since_prev_dump=" << (obj->mVKOmegaUpdates - obj->mVKDiagPrevOmega)
               << " last_nonzero_omega=(" << nz.mV[0] << "," << nz.mV[1] << "," << nz.mV[2] << ")@"
               << (obj->mVKLastNonzeroFrame ? (S32)(gFrameCount - obj->mVKLastNonzeroFrame) : -1) << "f_ago"
               << " rot_changing_updates=" << obj->mVKRotUpdates << " since_prev_dump=" << (obj->mVKRotUpdates - obj->mVKDiagPrevRot);
            obj->mVKDiagPrevUpd = obj->mVKUpdCount;
            obj->mVKDiagPrevOmega = obj->mVKOmegaUpdates;
            obj->mVKDiagPrevRot = obj->mVKRotUpdates;
        }
        LLViewerObject* root = obj->getRootEdit();
        os << " | linkset: " << (root == obj ? "ROOT" : "child") << " root_local="
           << (root ? root->getLocalID() : 0) << " children=" << obj->getChildren().size()
           << " attach=" << (obj->isAttachment() ? 1 : 0) << " hud=" << (obj->isHUDAttachment() ? 1 : 0)
           << " dead=" << (obj->isDead() ? 1 : 0);
        if (obj->isDead()) short_reason = "object dead";

        LLVOVolume* vo = dynamic_cast<LLVOVolume*>(obj);
        LLDrawable* dr = obj->mDrawable;
        LLVolume* live = obj->getVolume();
        if (obj->getPCode() != LL_PCODE_VOLUME || !vo)
        {
            os << " | not a volume (no clean ingest)";
            LL_INFOS("VKIngestDiag") << os.str() << LL_ENDL;
            continue;
        }
        // Volume type.
        const LLVolumeParams* vp = live ? &live->getParams() : nullptr;
        const U8 stype = vp ? vp->getSculptType() : 0;
        if (vo->isFlexible()) os << " | type=FLEXI";
        else if (obj->isMesh()) os << " | type=MESH asset=" << (vp ? vp->getSculptID() : LLUUID::null)
                                   << " sculpt_flags=0x" << std::hex << (U32)stype << std::dec;
        else if (vo->isSculpted()) os << " | type=SCULPT map=" << (vp ? vp->getSculptID() : LLUUID::null)
                                      << " sculpt_type=0x" << std::hex << (U32)stype << std::dec
                                      << " sculpt_level=" << (live ? live->getSculptLevel() : -99);
        else os << " | type=PRIM";
        // Drawable / movement.
        if (!dr) { os << " | drawable=NULL"; if (short_reason.empty()) short_reason = "no drawable"; }
        else
        {
            os << " | drawable dead=" << (dr->isDead() ? 1 : 0) << " active=" << (dr->isActive() ? 1 : 0)
               << " animated_child=" << (dr->isState(LLDrawable::ANIMATED_CHILD) ? 1 : 0)
               << " on_move_list=" << (dr->isState(LLDrawable::ON_MOVE_LIST) ? 1 : 0)
               << " rigged_state=" << (dr->isState(LLDrawable::RIGGED) ? 1 : 0);
            if (dr->isDead() && short_reason.empty()) short_reason = "drawable dead";
        }
        // Face-list consistency (the #88 resync discriminator).
        os << " | TEs=" << (S32)obj->getNumTEs() << " obj_faces=" << obj->getNumFaces()
           << " drawable_faces=" << (dr ? dr->getNumFaces() : -1);
        if (!live) { os << " | volume=NULL"; if (short_reason.empty()) short_reason = "no volume (not decoded)"; }
        S32 mesh_lod = -1;
        LLUUID mesh_key;
        if (live)
        {
            mesh_lod = LLVolumeLODGroup::getVolumeDetailFromScale(live->getDetail());
            os << " vol_faces=" << live->getNumFaces() << " vol_volume_faces=" << live->getNumVolumeFaces()
               << " mesh_loaded=" << (live->isMeshAssetLoaded() ? 1 : 0);
            mesh_key = LLCleanMeshAsset::geometryKey(live->getParams());
            // LOD: LL's mLOD, the live volume's level (= the ingest key), our GPU-select level.
            const F32 radius = live->mLODScaleBias.scaledVec(obj->getScale()).length();
            const F32 dist = llmax((obj->getRenderPosition() - LLViewerCamera::getInstance()->getOrigin()).length(), 0.001f);
            const F32 tan_angle = ll_round((LLVOVolume::sLODFactor * radius) / dist, 0.01f);
            const S32 desired = (tan_angle <= 0.03f) ? 0 : (tan_angle <= 0.06f) ? 1 : (tan_angle <= 0.24f) ? 2 : 3;
            os << " | LOD ll_mLOD=" << vo->getLOD() << " live(ingest key)=" << mesh_lod << " gpu_desired=" << desired;
            if (obj->isMesh())
            {
                os << " actual_mesh_lod[0..3]=";
                for (S32 l = 0; l < 4; ++l) os << (l ? "," : "") << gMeshRepo.getActualMeshLOD(live->getParams(), l);
                os << " raw_bytes[0..3]=";
                for (S32 l = 0; l < 4; ++l) os << (l ? "," : "") << (ms.hasRawMesh(mesh_key, l) ? 1 : 0);
            }
        }
        // Rigged routing inputs (registerCleanFace's `rigged` decision).
        {
            LLControlAvatar* cav = vo->getControlAvatar();
            const bool skin = vo->getSkinInfo() != nullptr;
            bool rigged = false;
            if (obj->isMesh() && skin)
                rigged = vo->isAnimatedObject() ? (vo->isRiggedMesh() && cav && cav->mPlaying) : vo->isAttachment();
            os << " | skin: skin_info=" << (skin ? 1 : 0) << " skin_unavailable=" << (vo->isSkinInfoUnavaliable() ? 1 : 0)
               << " rigged_mesh=" << (vo->isRiggedMesh() ? 1 : 0) << " animesh=" << (vo->isAnimatedObject() ? 1 : 0)
               << " control_avatar=" << (cav ? 1 : 0) << " cav_playing=" << (cav && cav->mPlaying ? 1 : 0)
               << " => rigged=" << (rigged ? 1 : 0);
            if (rigged)
                os << " skin_store=" << LLCleanSkinStore::instance().describe(obj->getAvatar(), vo->getSkinInfo());
        }
        // Sweep trace (settled skip).
        {
            auto sit = mObjSig.find(obj->getID());
            if (sit == mObjSig.end()) os << " | sweep: no signature (never fully visited, or reset by a change)";
            else
            {
                const ObjSig& s = sit->second;
                os << " | sweep: settled=" << (s.sig != 0 ? 1 : 0)
                   << " last_full_visit=" << (s.full_frame ? (S32)(now - s.full_frame) : -1) << "f_ago"
                   << " last_skip=" << (s.skip_frame ? (S32)(now - s.skip_frame) : -1) << "f_ago"
                   << " skips=" << s.skips << " missing_at_last_visit=" << s.missing
                   << " face_resyncs=" << s.resyncs << " laps_since=" << (mSweepLap - s.lap);
            }
        }
        // Target omega: LL angular velocity (own + linkset root) vs the GPU spin record.
        const LLVector3 w_own = obj->getAngularVelocity();
        LLViewerObject* par = (LLViewerObject*)obj->getParent();
        const LLVector3 w_par = par ? par->getAngularVelocity() : LLVector3::zero;
        LL_INFOS("VKIngestDiag") << os.str() << LL_ENDL;

        // ---- per face ----
        U32 faces_in = 0, faces_missing = 0, faces_rigged = 0, faces_noslot_draw = 0;
        std::ostringstream face_lods;   // per-face resident levels, for the object line
        std::string first_missing;
        const S32 nf = dr ? dr->getNumFaces() : 0;
        for (S32 fi = 0; fi < nf; ++fi)
        {
            LLFace* facep = dr->getFace(fi);
            if (!facep) { LL_INFOS("VKIngestDiag") << "  face " << fi << ": LLFace NULL" << LL_ENDL; continue; }
            ++total_faces;
            std::ostringstream fs;
            const S32 te_idx = facep->getTEOffset();
            fs << "  face " << fi << " te=" << te_idx;
            const LLTextureEntry* te = facep->getTextureEntry();
            if (te)
            {
                LLGLTFMaterial* gm = te->getGLTFRenderMaterial();
                const LLMaterial* lm = te->getMaterialParams().get();
                fs << " mat=" << (gm ? "PBR" : (lm ? "legacy+matparams" : "legacy"));
                if (gm) fs << " alpha_mode=" << (S32)gm->mAlphaMode << " dsided=" << (gm->mDoubleSided ? 1 : 0);
                else if (lm) fs << " diffuse_alpha_mode=" << (S32)lm->getDiffuseAlphaMode();
                fs << " tint_a=" << te->getColor().mV[3] << " tex=" << te->getID();
                // PBR: each map's texture and its streaming state (why it isn't sharp).
                // BOM sentinels are shown with the bake they resolve to.
                if (gm)
                {
                    static const char* const kMapName[] = { "base", "normal", "orm", "emissive" };
                    for (S32 m = 0; m < 4; ++m)
                    {
                        const LLUUID id = gm->mTextureId[m];
                        if (id.isNull()) continue;
                        LLUUID rid = id;
                        const bool bom = LLAvatarAppearanceDefines::LLAvatarAppearanceDictionary::isBakedImageId(id);
                        if (bom)
                        {
                            LLViewerTexture* bake = obj->getBakedTextureForMagicId(id);
                            rid = bake ? bake->getID() : LLUUID::null;
                        }
                        LLViewerFetchedTexture* ft = rid.notNull()
                            ? LLViewerTextureManager::findFetchedTexture(rid, TEX_LIST_STANDARD) : nullptr;
                        fs << "\n      " << kMapName[m] << "=" << id.asString().substr(0, 8)
                           << (bom ? " (BOM -> " + (rid.notNull() ? rid.asString().substr(0, 8) : std::string("none")) + ")" : std::string())
                           << ": " << (ft ? ft->fetchDiagString() : std::string("not in texture list"));
                    }
                }
                else
                {
                    // Legacy (Blinn-Phong): diffuse (BOM -> bake), normal, specular, with
                    // their streaming state -- the same "why isn't it sharp" answer.
                    auto diag = [&](const char* name, const LLUUID& id)
                    {
                        if (id.isNull()) return;
                        LLUUID rid = id;
                        const bool bom = LLAvatarAppearanceDefines::LLAvatarAppearanceDictionary::isBakedImageId(id);
                        if (bom)
                        {
                            LLViewerTexture* bake = obj->getBakedTextureForMagicId(id);
                            rid = bake ? bake->getID() : LLUUID::null;
                        }
                        LLViewerFetchedTexture* ft = rid.notNull()
                            ? LLViewerTextureManager::findFetchedTexture(rid, TEX_LIST_STANDARD) : nullptr;
                        fs << "\n      " << name << "=" << id.asString().substr(0, 8)
                           << (bom ? " (BOM -> " + (rid.notNull() ? rid.asString().substr(0, 8) : std::string("none")) + ")" : std::string())
                           << ": " << (ft ? ft->fetchDiagString() : std::string("not in texture list"));
                    };
                    diag("diffuse", te->getID());
                    if (lm)
                    {
                        diag("normal", lm->getNormalID());
                        diag("specular", lm->getSpecularID());
                    }
                }
            }
            // Per-level geometry of this face at its own key: Y = resident slot,
            // E = captured-empty at that LOD (placeholder), - = not captured.
            if (live && te_idx >= 0)
            {
                fs << " key_lods[0..3]=";
                for (S32 l = 0; l < 4; ++l)
                {
                    const LLCleanMeshAsset::Slot ls = ms.getSlot(mesh_key, l, te_idx);
                    char c = '-';
                    if (ls.valid && ls.num_indices) c = 'Y';
                    else
                    {
                        LLCleanMeshAsset::IngestDiag ld;
                        ms.diagQuery(mesh_key, l, stype, te_idx, ld);
                        if (ld.fail_reason == LLCleanMeshAsset::CAPF_EMPTY_FACE) c = 'E';
                        else if (ld.fail_reason) c = 'R';
                    }
                    fs << c;
                }
                face_lods << " f" << fi << ":";
                for (S32 l = 0; l < 4; ++l)
                {
                    const LLCleanMeshAsset::Slot ls = ms.getSlot(mesh_key, l, te_idx);
                    face_lods << ((ls.valid && ls.num_indices) ? 'Y' : '-');
                }
            }
            fs << " | ingest=" << LLFace::vkIngestName(facep->mVKIngest)
               << " (" << (facep->mVKIngestFrame ? (S32)(now - facep->mVKIngestFrame) : -1) << "f_ago)";
            // Slot at the ingest key.
            bool have_slot = false;
            if (live && mesh_lod >= 0 && te_idx >= 0)
            {
                const LLCleanMeshAsset::Slot sl = ms.getSlot(mesh_key, mesh_lod, te_idx);
                have_slot = sl.valid;
                fs << " | slot@L" << mesh_lod << "=" << (sl.valid ? (S32)sl.slot_id : -1);
                if (sl.valid) fs << " nv=" << sl.num_verts << " ni=" << sl.num_indices << " w=" << (sl.has_weights ? 1 : 0);
                // LOD-group residency (what the cull can draw).
                const U32 gid = ms.getLodGroupId(mesh_key, te_idx);
                fs << " | group=" << (gid == LLCleanMeshAsset::LOD_SENTINEL ? -1 : (S32)gid) << " levels[0..3]=";
                for (S32 l = 0; l < 4; ++l)
                {
                    const U32 s = ms.groupSlotAt(mesh_key, te_idx, l);
                    if (s == LLCleanMeshAsset::LOD_SENTINEL) { fs << (l ? "," : "") << "-"; continue; }
                    LLCleanMeshAsset::SlotInfo si{};
                    const bool ok = ms.debugGetSlotInfo(s, si);
                    fs << (l ? "," : "") << s << (ok && si.index_count ? "" : "(EMPTY)");
                }
                // Store-side state: latch, decode, capture refusal, backoff.
                LLCleanMeshAsset::IngestDiag d;
                ms.diagQuery(mesh_key, mesh_lod, stype, te_idx, d);
                fs << " | latch=" << (d.latched ? "LATCHED" : (d.latch_known ? "unlatched" : "none"));
                if (d.unlatch_count)
                    fs << " last_unlatch=" << LLCleanMeshAsset::unlatchSiteName(d.unlatch_site)
                       << "@" << (S32)(now - d.unlatch_frame) << "f_ago x" << d.unlatch_count;
                if (obj->isMesh())
                {
                    fs << " raw=" << (d.raw ? 1 : 0) << " decode=" << LLCleanMeshAsset::decodeStateName(d.decode_state);
                    if (d.decode_state >= 0) fs << (d.decode_started ? "(started)" : "(queued)") << " waiters=" << d.decode_waiters;
                    if (d.hist_decodes)
                        fs << " decodes=" << d.hist_decodes << " decoded_nfaces=" << d.hist_nfaces
                           << " prev_end=" << LLCleanMeshAsset::decodeEndName(d.hist_last_end);
                    U32 age = 0;
                    if (cleanCaptureBackoffInfo(mesh_key, mesh_lod, te_idx, age)) fs << " BACKOFF(" << age << "f)";
                }
                if (d.fail_reason)
                    fs << " | capture=REFUSED/SKIPPED reason=" << LLCleanMeshAsset::captureFailName(d.fail_reason)
                       << " nv=" << d.fail_nv << " ni=" << d.fail_ni << " x" << d.fail_count
                       << " @" << (S32)(now - d.fail_frame) << "f_ago";
                else fs << " | capture=" << (have_slot ? "accepted" : "no_record");
            }
            // Instance.
            LLCleanInstanceStore::Instance inst;
            U32 iid = 0;
            if (is.debugGet(obj->getID(), te_idx, inst, &iid) && inst.lod_group_id != LLCleanMeshAsset::LOD_SENTINEL)
            {
                ++faces_in;
                const bool irig = (inst.flags & 1u) != 0u;
                if (irig) ++faces_rigged;
                bool drawable_level = false;
                for (S32 l = 0; l < 4 && !drawable_level; ++l)
                {
                    const U32 s = ms.groupSlotAt(mesh_key, te_idx, l);
                    LLCleanMeshAsset::SlotInfo si{};
                    if (s != LLCleanMeshAsset::LOD_SENTINEL && ms.debugGetSlotInfo(s, si) && si.index_count) drawable_level = true;
                }
                if (!drawable_level) ++faces_noslot_draw;
                fs << " | INSTANCE id=" << iid << " group=" << inst.lod_group_id << " flags=0x" << std::hex << inst.flags << std::dec
                   << (irig ? " RIGGED(outline skips)" : "") << " skin_id="
                   << (inst.skin_id == 0xFFFFFFFFu ? std::string("unresolved") : std::to_string(inst.skin_id))
                   << " mat=" << inst.material_id;
                // Cull / pick bounds: the instance's local AABB, the LOD slots' half-float
                // bounds (localBounds() unions them on the GPU), and the model matrix
                // (scale basis lengths + translation).
                fs << " aabb=(" << inst.aabb_min[0] << "," << inst.aabb_min[1] << "," << inst.aabb_min[2] << ")-("
                   << inst.aabb_max[0] << "," << inst.aabb_max[1] << "," << inst.aabb_max[2] << ")";
                {
                    const F32* m = inst.model_matrix;
                    fs << " mm_scale=(" << std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]) << ","
                       << std::sqrt(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]) << ","
                       << std::sqrt(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]) << ") mm_pos=("
                       << m[12] << "," << m[13] << "," << m[14] << ")";
                    for (S32 l = 0; l < 4; ++l)
                    {
                        const U32 s = ms.groupSlotAt(mesh_key, te_idx, l);
                        LLCleanMeshAsset::SlotInfo bsi{};
                        if (s == LLCleanMeshAsset::LOD_SENTINEL || !ms.debugGetSlotInfo(s, bsi) || !bsi.index_count) continue;
                        fs << " slot" << l << "_bounds=0x" << std::hex << bsi._pad0 << "," << bsi._pad1 << "," << bsi._pad2 << std::dec;
                    }
                }
                // Texture animation: GPU-parametric (flag 1024, params in uv_matrix) or
                // a static uv_matrix refreshed at ingest.
                if (const LLVOVolume* tvo = dr->getVOVolume(); tvo && tvo->mTextureAnimp)
                {
                    const LLViewerTextureAnim* ta = tvo->mTextureAnimp;
                    fs << " texanim=" << ((inst.flags & 1024u) ? "gpu" : "cpu")
                       << "(mode=0x" << std::hex << (U32)ta->mMode << std::dec << " face=" << (S32)ta->mFace
                       << " size=" << (S32)ta->mSizeX << "x" << (S32)ta->mSizeY << " start=" << ta->mStart
                       << " len=" << ta->mLength << " rate=" << ta->mRate
                       << " run=0x" << std::hex << (U32)tvo->mTexAnimMode << std::dec
                       << " texmat=" << (facep->mTextureMatrix ? 1 : 0)
                       << " ll_state=" << (facep->isState(LLFace::TEXTURE_ANIM) ? 1 : 0)
                       << " vsize=" << facep->getVirtualSize() << ")";
                }
                fs << " spin_gpu=(" << inst.spin_axis[0] << "," << inst.spin_axis[1] << "," << inst.spin_axis[2] << ")"
                   << " omega_ll_own=(" << w_own.mV[0] << "," << w_own.mV[1] << "," << w_own.mV[2] << ")"
                   << " omega_ll_parent=(" << w_par.mV[0] << "," << w_par.mV[1] << "," << w_par.mV[2] << ")";
                const bool ll_spins = w_own.magVecSquared() > 0.00001f || w_par.magVecSquared() > 0.00001f;
                const bool gpu_spins = (inst.spin_axis[0] * inst.spin_axis[0] + inst.spin_axis[1] * inst.spin_axis[1]
                                      + inst.spin_axis[2] * inst.spin_axis[2]) > 0.00001f;
                if (ll_spins != gpu_spins && !irig && !obj->isAttachment())
                    fs << " SPIN_MISMATCH(ll=" << (ll_spins ? 1 : 0) << " gpu=" << (gpu_spins ? 1 : 0) << ")";
            }
            else
            {
                ++faces_missing;
                fs << " | INSTANCE=NONE (not in GPU scene)";
                if (first_missing.empty())
                {
                    std::ostringstream r;
                    r << "face" << fi << ":" << LLFace::vkIngestName(facep->mVKIngest);
                    if (facep->mVKIngest == LLFace::VKI_NEVER)
                    {
                        auto sit = mObjSig.find(obj->getID());
                        r << (sit == mObjSig.end() ? "(never_attempted)"
                                                   : (sit->second.sig != 0 ? "(settled_skip_before_attempt)" : ""));
                    }
                    first_missing = r.str();
                }
            }
            LL_INFOS("VKIngestDiag") << fs.str() << LL_ENDL;
        }
        if (short_reason.empty() && nf == 0 && dr) short_reason = "drawable has 0 faces";
        if (short_reason.empty() && faces_missing)
        {
            std::ostringstream r;
            r << faces_missing << "/" << nf << " faces missing, first " << first_missing;
            if (dr && (S32)obj->getNumTEs() != dr->getNumFaces())
                r << " [TEs=" << (S32)obj->getNumTEs() << " != drawable_faces=" << dr->getNumFaces() << "]";
            short_reason = r.str();
        }
        if (short_reason.empty() && live && nf < live->getNumVolumeFaces() && obj->isMesh())
        {
            std::ostringstream r;
            r << "face list short: drawable_faces=" << nf << " < mesh faces=" << live->getNumVolumeFaces();
            short_reason = r.str();
        }
        if (short_reason.empty() && faces_in && faces_noslot_draw == faces_in)
            short_reason = "in scene but no LOD level has drawable geometry";
        if (!short_reason.empty())
        {
            missing_list << (n_missing ? "; " : "") << obj->getLocalID() << " (" << short_reason << ")";
            ++n_missing;
        }
        if (faces_rigged)
        {
            rigged_list << (n_rigged ? ", " : "") << obj->getLocalID();
            ++n_rigged;
        }
        LL_INFOS("VKIngestDiag") << "  => obj local=" << obj->getLocalID() << " faces_in_scene=" << faces_in << "/" << nf
            << " rigged_faces=" << faces_rigged << " resident_lods[0..3]:" << face_lods.str()
            << (short_reason.empty() ? " OK" : (" PROBLEM: " + short_reason)) << LL_ENDL;
    }

    LL_INFOS("VKIngestDiag") << "VKIngestDiag summary: selected=[" << sel_ids.str() << "] " << objs.size() << " objects, " << total_faces
        << " faces; NOT in GPU scene: " << (n_missing ? missing_list.str() : std::string("none"))
        << " | in scene but RIGGED (selection outline never draws rigged): "
        << (n_rigged ? rigged_list.str() : std::string("none")) << LL_ENDL;
}

