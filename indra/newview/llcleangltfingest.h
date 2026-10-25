/**
 * @file llcleangltfingest.h
 * @brief Clean-pipeline ingest (Vulkan) of glTF SCENE ASSETS (#39): objects whose
 *        geometry is an LL::GLTF::Asset hung off LLViewerObject::mGLTFAsset (sculpt type
 *        LL_SCULPT_TYPE_GLTF from the sim, or a Develop > GLTF > Open... local preview).
 *
 *        LL's GL path (GLTFSceneManager::render) drew these per primitive with per-draw
 *        uniforms; under Vulkan nothing drew them. Here every (node, primitive) becomes
 *        an ordinary clean instance: the primitive's geometry is captured ONCE per asset
 *        into a mesh slot (LLCleanMeshAsset::captureGLTFPrimitive, LOD level 3 only --
 *        the cull's residency fallback draws it at every distance), its material into
 *        the material store, and the instance carries the node's asset-space matrix as a
 *        NODE-LOCAL pre-transform (LLCleanInstanceStore upsert local_xform) so every pose
 *        writer (moved queue, region shift) keeps model_matrix = object pose * node.
 *        From there the existing GPU-driven passes (gbuffer, shadows, mirror, probes,
 *        alpha, RT) draw them with no further code.
 *
 *        Lifetime: instances die with the object (markDead -> releaseObject) or when the
 *        object stops being a glTF scene (forgetObject); geometry is reclaimed by the
 *        mesh GC once unreferenced (the mesh pattern), local previews free theirs
 *        immediately when replaced.
 *
 *        NOT YET: skinning (skinned primitives render static in bind pose), morph
 *        targets, TEXCOORD_1, COLOR_0, occlusion maps, points/lines. See the .cpp.
 */

#pragma once

#include "stdtypes.h"
#include "lluuid.h"

#include <memory>
#include <unordered_map>
#include <vector>

class LLViewerObject;
namespace LL { namespace GLTF { class Asset; } }

class LLCleanGLTFIngest
{
public:
    static LLCleanGLTFIngest& instance();

    // True when obj's geometry is (or will be) a glTF scene asset: it carries one, or its
    // volume is LL_SCULPT_TYPE_GLTF (asset still downloading). Such objects never take
    // the per-face volume registration path (LL's GL pipeline skipped them too).
    static bool isGLTFScene(const LLViewerObject* obj);

    // Ingest / refresh obj's scene asset (the per-face registration substitute). Cheap
    // when nothing changed (object signature).
    void ingestObject(LLViewerObject* obj);
    // obj stopped being a glTF scene (or is gone): drop its instances + record (and a
    // local preview's geometry).
    void forgetObject(const LLUUID& id);
    bool isTracked(const LLUUID& id) const { return mObjs.count(id) != 0; }
    // The (node, primitive) of obj's instance `face` (the GPU pick reports instances, #704):
    // asset->mNodes / that node's mesh mPrimitives indices. False = not one of its instances.
    bool instanceNodePrim(const LLUUID& id, S32 face, S32& node, S32& prim) const;
    // Once per frame (VK ingest tick): prune dead records, re-pose animated nodes,
    // periodic VKGLTF summary.
    void tick();

private:
    LLCleanGLTFIngest() = default;

    struct InstRef { S32 node = -1; bool skinned = false; S32 prim = -1; };
    struct ObjRec
    {
        std::weak_ptr<LL::GLTF::Asset> asset;   // asset the instances were built from
        const LL::GLTF::Asset* asset_raw = nullptr;
        LLUUID geom_key;                        // mesh-store key of its primitives
        bool   local_key = false;               // minted for a local preview (freed on replace)
        bool   claimed   = false;               // regular-face instances cleared once
        bool   animated  = false;
        U64    sig       = 0;
        F32    radius    = 0.f;                 // agent-space bound about the object origin
        std::vector<InstRef> inst;              // instance face_idx -> node it follows
    };
    std::unordered_map<LLUUID, ObjRec> mObjs;

    // Totals for the periodic summary.
    U32 mIngests = 0, mPrimsCaptured = 0;
    F64 mNextSummary = 0.0;

    void releaseInstances(const LLUUID& id, ObjRec& rec, U32 keep);
    static void noteChanged(LLViewerObject* obj, const ObjRec& rec);
};
