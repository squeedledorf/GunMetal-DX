/**
 * @file llfbxloader.h
 * @brief FBX mesh-upload importer (LLModelLoader over the vendored ufbx).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream contributors
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

#ifndef LL_LLFBXLOADER_H
#define LL_LLFBXLOADER_H

#include "llmodelloader.h"

struct ufbx_scene;
struct ufbx_mesh;
struct ufbx_node;
struct ufbx_material;

// Imports .fbx files for mesh upload. Produces the same LLModel / LLModelInstance /
// LLMeshSkinInfo data the DAE loader does (the upload serialization is untouched),
// mirroring LLDAELoader's conventions:
//  - one LLModel per FBX mesh, split into submodels past 8 materials;
//  - labels from the (first) instancing node's name, so the _LODn / _PHYS
//    suffix convention matches LOD and physics files exactly as for .dae;
//  - static instances carry the node's world transform (+ mesh normalization);
//  - skinned meshes: joint names through the joint alias map, inverse bind and
//    bind-shape matrices in the DAE sense, up to 4 normalized weights per vertex
//    keyed by normalized position, alternate bind matrices from joint positions.
// ufbx converts the file to SL's space (right-handed, Z up, meters) on load.
class LLFBXLoader : public LLModelLoader
{
public:
    LLFBXLoader(
        std::string                                      filename,
        S32                                              lod,
        LLModelLoader::load_callback_t                   load_cb,
        LLModelLoader::joint_lookup_func_t               joint_lookup_func,
        LLModelLoader::texture_load_func_t               texture_load_func,
        LLModelLoader::state_callback_t                  state_cb,
        void*                                            opaque_userdata,
        JointTransformMap&                               jointTransformMap,
        JointNameSet&                                    jointsFromNodes,
        std::map<std::string, std::string, std::less<>>& jointAliasMap,
        U32                                              maxJointsPerMesh,
        U32                                              modelLimit,
        U32                                              debugMode);
    virtual ~LLFBXLoader();

    virtual bool OpenFile(const std::string& filename);

protected:
    typedef std::map<const ufbx_mesh*, std::vector<LLPointer<LLModel> > > fbx_model_map;
    fbx_model_map mModelsMap;

    // Loads a mesh, breaking it into one or more models as necessary to get
    // around the 8-face limit while retaining more materials (as the DAE loader).
    bool loadModelsFromMesh(const ufbx_mesh* mesh, std::vector<LLModel*>& models_out, U32 submodel_limit);
    bool addVolumeFacesFromMesh(LLModel* model, const ufbx_mesh* mesh);

    void processStaticInstance(const ufbx_node* node, bool& bad_element);
    void processSkinnedModel(LLModel* model, const ufbx_scene* scene, const ufbx_mesh* mesh);
    void collectJointTransforms(const ufbx_scene* scene);

    material_map getMaterials(const LLModel* model, const ufbx_mesh* mesh);
    LLImportMaterial toImportMaterial(const ufbx_material* material);

    std::string mapJointName(const std::string& name) const;
    std::string getLodlessLabel(const ufbx_mesh* mesh);

    bool mJointsCollected = false;
    bool mHasSkeleton = false;
};

#endif // LL_LLFBXLOADER_H
