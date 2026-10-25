/**
 * @file llfbxloader.cpp
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

// The importer only fills LLModel / LLModelInstance / LLMeshSkinInfo, the same
// structures LLDAELoader fills; serialization for upload (LLModel::writeModel,
// LLMeshUploadThread) is shared and unchanged. Where a choice exists, this file
// follows LLDAELoader line for line (face building, vertex welding, submodel
// split, normalization, skin weights, alternate bind matrices) so the same
// model exported as .dae and .fbx previews and uploads the same way.

#include "linden_common.h"

#include "llfbxloader.h"

#include "llfile.h"
#include "lljoint.h"
#include "llmatrix4a.h"
#include "llvolume.h"

#include "glm/mat4x4.hpp"
#include "glm/gtc/type_ptr.hpp"

#include "third_party/ufbx/ufbx.h"

#include <algorithm>
#include <memory>

namespace
{
    struct ModelSortFBX
    {
        bool operator()(const LLPointer<LLModel>& lhs, const LLPointer<LLModel>& rhs) const
        {
            if (lhs->mSubmodelID < rhs->mSubmodelID)
            {
                return true;
            }
            return LLStringUtil::compareInsensitive(lhs->mLabel, rhs->mLabel) < 0;
        }
    };

    struct SceneDeleter
    {
        void operator()(ufbx_scene* scene) const { ufbx_free_scene(scene); }
    };

    std::string fbx_str(const ufbx_string& s)
    {
        return s.length ? std::string(s.data, s.length) : std::string();
    }

    // ufbx_matrix is a column-major affine 3x4 for column vectors. LLMatrix4 is
    // used with row vectors (v * M, translation in mMatrix[3]), so its rows are
    // ufbx's columns -- the same transpose LLDAELoader applies to COLLADA's
    // row-major <matrix> values (mat.mMatrix[i][j] = dom[i + j*4]).
    LLMatrix4 to_llmatrix(const ufbx_matrix& m)
    {
        LLMatrix4 out;
        for (int c = 0; c < 4; ++c)
        {
            out.mMatrix[c][0] = (F32)m.cols[c].x;
            out.mMatrix[c][1] = (F32)m.cols[c].y;
            out.mMatrix[c][2] = (F32)m.cols[c].z;
            out.mMatrix[c][3] = (c == 3) ? 1.f : 0.f;
        }
        return out;
    }

    std::string material_name(const ufbx_mesh* mesh, uint32_t material_index)
    {
        if (material_index < mesh->materials.count && mesh->materials.data[material_index])
        {
            return fbx_str(mesh->materials.data[material_index]->name);
        }
        return std::string();
    }

    std::string dir_of(const std::string& path)
    {
        size_t sep = path.find_last_of("/\\");
        return sep == std::string::npos ? std::string() : path.substr(0, sep + 1);
    }
}

LLFBXLoader::LLFBXLoader(
    std::string                                      filename,
    S32                                              lod,
    load_callback_t                                  load_cb,
    joint_lookup_func_t                              joint_lookup_func,
    texture_load_func_t                              texture_load_func,
    state_callback_t                                 state_cb,
    void*                                            opaque_userdata,
    JointTransformMap&                               jointTransformMap,
    JointNameSet&                                    jointsFromNodes,
    std::map<std::string, std::string, std::less<>>& jointAliasMap,
    U32                                              maxJointsPerMesh,
    U32                                              modelLimit,
    U32                                              debugMode)
: LLModelLoader(
        filename,
        lod,
        load_cb,
        joint_lookup_func,
        texture_load_func,
        state_cb,
        opaque_userdata,
        jointTransformMap,
        jointsFromNodes,
        jointAliasMap,
        maxJointsPerMesh,
        modelLimit,
        debugMode)
{
}

LLFBXLoader::~LLFBXLoader()
{
}

bool LLFBXLoader::OpenFile(const std::string& filename)
{
    setLoadState(READING_FILE);

    ufbx_load_opts opts = {};
    // SL space: right-handed, +Z up, meters (what a .dae for SL is authored in).
    // MODIFY_GEOMETRY bakes the unit scale into vertex data and node/bind
    // transforms (ufbx also converts skin cluster bind matrices), and folds the
    // axis change into the root-level node transforms.
    opts.target_axes = ufbx_axes_right_handed_z_up;
    opts.target_unit_meters = 1.0f;
    opts.space_conversion = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
    opts.geometry_transform_handling = UFBX_GEOMETRY_TRANSFORM_HANDLING_MODIFY_GEOMETRY;
    opts.generate_missing_normals = true;
    opts.clean_skin_weights = true;
    // Never follow file references found inside the FBX (geometry caches etc.).
    opts.load_external_files = false;

    ufbx_error error;
    std::unique_ptr<ufbx_scene, SceneDeleter> scene(ufbx_load_file(filename.c_str(), &opts, &error));
    if (!scene)
    {
        char buf[1024];
        ufbx_format_error(buf, sizeof(buf), &error);
        LL_WARNS("Mesh") << "Failed to load FBX " << filename << ": " << buf << LL_ENDL;
        LLSD args;
        args["Message"] = "ParsingErrorFBX";
        args["FILENAME"] = filename;
        args["EXCEPTION"] = std::string(error.description.data, error.description.length);
        mWarningsArray.append(args);
        setLoadState(ERROR_PARSING);
        return false;
    }

    LL_INFOS("Mesh") << "FBX " << filename << ": " << scene->meshes.count << " meshes, "
                     << scene->nodes.count << " nodes, original unit " << scene->settings.original_unit_meters
                     << " m" << LL_ENDL;

    // Unit/axis conversion is already applied by ufbx.
    mTransform.setIdentity();

    const size_t mesh_count = scene->meshes.count;
    U32 submodel_limit = mesh_count > 0 ? mGeneratedModelLimit / (U32)mesh_count : 0;

    for (size_t idx = 0; idx < mesh_count; ++idx)
    { // build map of ufbx meshes to LLModel
        const ufbx_mesh* mesh = scene->meshes.data[idx];
        if (!mesh || mesh->num_triangles == 0)
        {
            continue;
        }

        std::vector<LLModel*> models;
        loadModelsFromMesh(mesh, models, submodel_limit);

        for (LLModel* mdl : models)
        {
            if (mdl->getStatus() != LLModel::NO_ERRORS)
            {
                setLoadState((U32)ERROR_MODEL + (U32)mdl->getStatus());
                return false; // abort
            }

            if (validate_model(mdl))
            {
                mModelList.push_back(mdl);
                mModelsMap[mesh].push_back(mdl);
            }
        }
    }

    std::sort(mModelList.begin(), mModelList.end(), ModelSortFBX());

    // Skinned meshes: one instance per model, skin info from the mesh's first skin deformer.
    S32 skin_count = 0;
    for (size_t idx = 0; idx < mesh_count; ++idx)
    {
        const ufbx_mesh* mesh = scene->meshes.data[idx];
        if (!mesh || mesh->skin_deformers.count == 0)
        {
            continue;
        }

        fbx_model_map::const_iterator it = mModelsMap.find(mesh);
        if (it == mModelsMap.end())
        {
            continue;
        }

        for (const LLPointer<LLModel>& model : it->second)
        {
            processSkinnedModel(model, scene.get(), mesh);
        }
        ++skin_count;
    }
    LL_INFOS("Mesh") << "FBX skins processed: " << skin_count << LL_ENDL;

    setLoadState(DONE);

    // Static meshes: one instance per node that references the mesh.
    bool bad_element = false;
    for (size_t idx = 0; idx < scene->nodes.count; ++idx)
    {
        const ufbx_node* node = scene->nodes.data[idx];
        if (node && node->mesh && node->mesh->skin_deformers.count == 0)
        {
            processStaticInstance(node, bad_element);
        }
    }

    if (bad_element)
    {
        LL_INFOS("Mesh") << "FBX scene could not be parsed" << LL_ENDL;
        LLSD args;
        args["Message"] = "ParsingErrorCantParseScene";
        mWarningsArray.append(args);
        setLoadState(ERROR_PARSING);
    }

    return true;
}

std::string LLFBXLoader::getLodlessLabel(const ufbx_mesh* mesh)
{
    // The node (object) name is what the creator sees and names with _LODn /
    // _PHYS; fall back to the mesh (data) name.
    std::string label;
    for (size_t i = 0; i < mesh->instances.count && label.empty(); ++i)
    {
        label = fbx_str(mesh->instances.data[i]->name);
    }
    if (label.empty())
    {
        label = fbx_str(mesh->name);
    }
    if (label.empty())
    {
        label = "object";
    }

    size_t ext_pos = getSuffixPosition(label);
    if (ext_pos != std::string::npos)
    {
        return label.substr(0, ext_pos);
    }
    return label;
}

bool LLFBXLoader::addVolumeFacesFromMesh(LLModel* pModel, const ufbx_mesh* mesh)
{
    std::vector<LLVolumeFace>& face_list = pModel->getVolumeFaces();
    std::vector<std::string>& materials = pModel->getMaterialList();

    const bool has_normals = mesh->vertex_normal.exists;
    const bool has_uvs = mesh->vertex_uv.exists;

    std::vector<uint32_t> tri_indices(mesh->max_face_triangles * 3);

    for (size_t part_idx = 0; part_idx < mesh->material_parts.count; ++part_idx)
    {
        const ufbx_mesh_part& part = mesh->material_parts.data[part_idx];
        if (part.num_triangles == 0)
        {
            continue;
        }

        const std::string material = material_name(mesh, part.index);

        LLVolumeFace face;
        std::vector<LLVolumeFace::VertexData> verts;
        std::vector<U16> indices;
        LLVolumeFace::VertexMapData::PointMap point_map;
        bool extents_set = false;

        auto emit_face = [&]()
        {
            materials.push_back(material);
            face_list.push_back(face);
            face_list.rbegin()->fillFromLegacyData(verts, indices);
            LLVolumeFace& new_face = *face_list.rbegin();
            if (!has_normals)
            {
                new_face.mNormals = NULL;
            }
            if (!has_uvs)
            {
                new_face.mTexCoords = NULL;
            }

            face = LLVolumeFace();
            verts.clear();
            indices.clear();
            point_map.clear();
            extents_set = false;
        };

        for (size_t f = 0; f < part.face_indices.count; ++f)
        {
            const ufbx_face fbx_face = mesh->faces.data[part.face_indices.data[f]];
            if (fbx_face.num_indices < 3)
            {
                continue; // points / lines
            }

            const uint32_t num_tris = ufbx_triangulate_face(tri_indices.data(), tri_indices.size(), mesh, fbx_face);
            for (uint32_t k = 0; k < num_tris * 3; ++k)
            {
                const uint32_t index = tri_indices[k];

                LLVolumeFace::VertexData cv;
                const ufbx_vec3 p = ufbx_get_vertex_vec3(&mesh->vertex_position, index);
                cv.setPosition(LLVector4a((F32)p.x, (F32)p.y, (F32)p.z));
                if (has_uvs)
                {
                    const ufbx_vec2 uv = ufbx_get_vertex_vec2(&mesh->vertex_uv, index);
                    cv.mTexCoord.setVec((F32)uv.x, (F32)uv.y);
                }
                if (has_normals)
                {
                    const ufbx_vec3 n = ufbx_get_vertex_vec3(&mesh->vertex_normal, index);
                    cv.setNormal(LLVector4a((F32)n.x, (F32)n.y, (F32)n.z));
                }

                if (!extents_set)
                {
                    // VFExtents change
                    face.mExtents[0] = cv.getPosition();
                    face.mExtents[1] = cv.getPosition();
                    extents_set = true;
                }

                bool found = false;

                LLVolumeFace::VertexMapData::PointMap::iterator point_iter;
                point_iter = point_map.find(LLVector3(cv.getPosition().getF32ptr()));

                if (point_iter != point_map.end())
                {
                    for (U32 j = 0; j < point_iter->second.size(); ++j)
                    {
                        // We have a matching loc
                        if ((point_iter->second)[j] == cv)
                        {
                            U16 shared_index = (point_iter->second)[j].mIndex;

                            // Don't share verts within the same tri, degenerate
                            U32 indx_size = static_cast<U32>(indices.size());
                            U32 verts_new_tri = indx_size % 3;
                            if ((verts_new_tri < 1 || indices[indx_size - 1] != shared_index)
                                && (verts_new_tri < 2 || indices[indx_size - 2] != shared_index))
                            {
                                found = true;
                                indices.push_back(shared_index);
                            }
                            break;
                        }
                    }
                }

                if (!found)
                {
                    // VFExtents change
                    update_min_max(face.mExtents[0], face.mExtents[1], cv.getPosition());
                    verts.push_back(cv);
                    if (verts.size() >= 65535)
                    {
                        pModel->mStatus = LLModel::VERTEX_NUMBER_OVERFLOW;
                        return false;
                    }
                    U16 new_index = (U16)(verts.size() - 1);
                    indices.push_back(new_index);

                    LLVolumeFace::VertexMapData d;
                    d.setPosition(cv.getPosition());
                    d.mTexCoord = cv.mTexCoord;
                    d.setNormal(cv.getNormal());
                    d.mIndex = new_index;
                    if (point_iter != point_map.end())
                    {
                        point_iter->second.push_back(d);
                    }
                    else
                    {
                        point_map[LLVector3(d.getPosition().getF32ptr())].push_back(d);
                    }
                }

                if (indices.size() % 3 == 0 && verts.size() >= 65532)
                {
                    emit_face();
                }
            }
        }

        if (!verts.empty())
        {
            emit_face();
        }
    }

    return true;
}

// Mirrors LLDAELoader::loadModelsFromDomMesh: creates multiple models when
// material counts spill over the 8 face server-side limit.
bool LLFBXLoader::loadModelsFromMesh(const ufbx_mesh* mesh, std::vector<LLModel*>& models_out, U32 submodel_limit)
{
    LLVolumeParams volume_params;
    volume_params.setType(LL_PCODE_PROFILE_SQUARE, LL_PCODE_PATH_LINE);

    models_out.clear();

    LLModel* ret = new LLModel(volume_params, 0.f);

    std::string model_name = getLodlessLabel(mesh);
    ret->mLabel = model_name + mLodSuffixes[mLod];

    llassert(!ret->mLabel.empty());

    ret->ClearFacesAndMaterials();

    if (!addVolumeFacesFromMesh(ret, mesh))
    {
        ret->ClearFacesAndMaterials();
        models_out.push_back(ret); // carries mStatus to the caller
        return false;
    }

    U32 volume_faces = ret->getNumVolumeFaces();

    // Side-steps all manner of issues when splitting models
    // and matching lower LOD materials to base models
    ret->sortVolumeFacesByMaterialName();

    bool normalized = false;

    int submodelID = 0;

    // remove all faces that definitely won't fit into one model and submodel limit
    U32 face_limit = (submodel_limit + 1) * LL_SCULPT_MESH_MAX_FACES;
    if (face_limit < volume_faces)
    {
        ret->setNumVolumeFaces(face_limit);
    }

    LLVolume::face_list_t remainder;
    do
    {
        // Insure we do this once with the whole gang and not per-model
        if (!normalized && !mNoNormalize)
        {
            normalized = true;
            ret->normalizeVolumeFaces();
        }

        ret->trimVolumeFacesToSize(LL_SCULPT_MESH_MAX_FACES, &remainder);

        // remove unused/redundant vertices after normalizing
        if (!mNoOptimize)
        {
            ret->remapVolumeFaces();
        }

        volume_faces = static_cast<U32>(remainder.size());

        models_out.push_back(ret);

        // If we have left-over volume faces, create another model
        // to absorb them...
        if (volume_faces)
        {
            LLModel* next = new LLModel(volume_params, 0.f);
            next->mSubmodelID = ++submodelID;
            next->mLabel = model_name + (char)((int)'a' + next->mSubmodelID) + mLodSuffixes[mLod];
            next->getVolumeFaces() = remainder;
            next->mNormalizedScale = ret->mNormalizedScale;
            next->mNormalizedTranslation = ret->mNormalizedTranslation;

            if (ret->mMaterialList.size() > LL_SCULPT_MESH_MAX_FACES)
            {
                next->mMaterialList.assign(ret->mMaterialList.begin() + LL_SCULPT_MESH_MAX_FACES, ret->mMaterialList.end());
            }
            ret = next;
        }

        remainder.clear();

    } while (volume_faces);

    return true;
}

// Mirrors the domInstance_geometry branch of LLDAELoader::processElement.
void LLFBXLoader::processStaticInstance(const ufbx_node* node, bool& bad_element)
{
    fbx_model_map::iterator it = mModelsMap.find(node->mesh);
    if (it == mModelsMap.end())
    {
        return;
    }

    const std::string node_label = fbx_str(node->name);

    for (LLModel* model : it->second)
    {
        LLMatrix4 transformation = to_llmatrix(node->geometry_to_world);

        if (transformation.determinant() < 0)
        { // negative scales are not supported
            LL_INFOS("Mesh") << "Negative scale detected, unsupported transform. FBX node: " << node_label << LL_ENDL;
            LLSD args;
            args["Message"] = "NegativeScaleTrans";
            args["LABEL"] = node_label;
            mWarningsArray.append(args);

            bad_element = true;
        }

        LLModelLoader::material_map materials = getMaterials(model, node->mesh);

        // adjust the transformation to compensate for mesh normalization
        LLVector3 mesh_scale_vector;
        LLVector3 mesh_translation_vector;
        model->getNormalizedScaleTranslation(mesh_scale_vector, mesh_translation_vector);

        LLMatrix4 mesh_translation;
        mesh_translation.setTranslation(mesh_translation_vector);
        mesh_translation *= transformation;
        transformation = mesh_translation;

        LLMatrix4 mesh_scale;
        mesh_scale.initScale(mesh_scale_vector);
        mesh_scale *= transformation;
        transformation = mesh_scale;

        if (transformation.determinant() < 0)
        { // negative scales are not supported
            LL_INFOS("Mesh") << "Negative scale detected, unsupported post-normalization transform. FBX node: " << node_label << LL_ENDL;
            LLSD args;
            args["Message"] = "NegativeScaleNormTrans";
            args["LABEL"] = node_label;
            mWarningsArray.append(args);
            bad_element = true;
        }

        // Don't change model's name, it will play havoc with scenes that already use said model.
        std::string label = model->mLabel;
        size_t ext_pos = getSuffixPosition(label);
        if (ext_pos != std::string::npos)
        {
            label = label.substr(0, ext_pos);
        }

        mScene[transformation].push_back(LLModelInstance(model, label, transformation, materials));
        stretch_extents(model, transformation);
    }
}

std::string LLFBXLoader::mapJointName(const std::string& name) const
{
    JointMap::const_iterator it = mJointMap.find(name);
    if (it != mJointMap.end())
    {
        return it->second;
    }

    // Some exporters namespace bones ("Character1:mPelvis").
    size_t colon = name.find_last_of(':');
    if (colon != std::string::npos && colon + 1 < name.size())
    {
        it = mJointMap.find(name.substr(colon + 1));
        if (it != mJointMap.end())
        {
            return it->second;
        }
    }
    return name;
}

// Joint positions from the scene's skeleton nodes (parent-relative translation,
// as LLDAELoader extracts from each joint's <translate>/<matrix>), keyed by the
// SL joint name. Feeds the alternate bind matrices and the preview avatar's
// joint position overrides.
void LLFBXLoader::collectJointTransforms(const ufbx_scene* scene)
{
    if (mJointsCollected)
    {
        return;
    }
    mJointsCollected = true;

    for (size_t i = 0; i < scene->nodes.count; ++i)
    {
        const ufbx_node* node = scene->nodes.data[i];
        if (!node || node->is_root)
        {
            continue;
        }

        const std::string name = fbx_str(node->name);
        const std::string sl_name = mapJointName(name);
        if (name.empty() || mJointMap.find(sl_name) == mJointMap.end())
        {
            continue;
        }

        const ufbx_vec3& t = node->node_to_parent.cols[3];
        LLMatrix4 working_transform;
        working_transform.setTranslation(LLVector3((F32)t.x, (F32)t.y, (F32)t.z));
        mJointList[sl_name] = working_transform;
        mJointsFromNode.push_front(name);
        mHasSkeleton = true;
    }
}

// Mirrors LLDAELoader::processDomModel.
void LLFBXLoader::processSkinnedModel(LLModel* model, const ufbx_scene* scene, const ufbx_mesh* mesh)
{
    llassert(model && scene && mesh);
    if (!model || mesh->skin_deformers.count == 0)
    {
        return;
    }

    const ufbx_skin_deformer* skin = mesh->skin_deformers.data[0];
    if (!skin || skin->clusters.count == 0)
    {
        return;
    }
    if (mesh->skin_deformers.count > 1)
    {
        LL_WARNS("Mesh") << "FBX mesh " << model->mLabel << " has " << mesh->skin_deformers.count
                         << " skin deformers; only the first is imported." << LL_ENDL;
    }

    LLVector3 mesh_scale_vector;
    LLVector3 mesh_translation_vector;
    model->getNormalizedScaleTranslation(mesh_scale_vector, mesh_translation_vector);

    LLMatrix4 normalized_transformation;
    normalized_transformation.setTranslation(mesh_translation_vector);

    LLMatrix4 mesh_scale;
    mesh_scale.initScale(mesh_scale_vector);
    mesh_scale *= normalized_transformation;
    normalized_transformation = mesh_scale;

    glm::mat4 inv_mat = glm::make_mat4((F32*)normalized_transformation.mMatrix);
    inv_mat = glm::inverse(inv_mat);
    LLMatrix4 inverse_normalized_transformation(glm::value_ptr(inv_mat));

    // Bind shape matrix (COLLADA sense): mesh geometry -> world at bind time.
    // ufbx gives each cluster geometry_to_bone (= inverse bind * bind shape) and
    // bind_to_world (the bone's bind pose); bind shape = bind_to_world * geometry_to_bone.
    const ufbx_skin_cluster* first_cluster = skin->clusters.data[0];
    const ufbx_matrix bind_shape = ufbx_matrix_mul(&first_cluster->bind_to_world, &first_cluster->geometry_to_bone);
    const ufbx_matrix inv_bind_shape = ufbx_matrix_invert(&bind_shape);

    LLMeshSkinInfo& skin_info = model->mSkinInfo;
    {
        LLMatrix4 mat = to_llmatrix(bind_shape);
        skin_info.mBindShapeMatrix.loadu(mat);

        LLMatrix4a trans(normalized_transformation);
        matMul(trans, skin_info.mBindShapeMatrix, skin_info.mBindShapeMatrix);
    }

    collectJointTransforms(scene);

    // Joint names and inverse bind matrices, in cluster order (the weights'
    // cluster_index is the joint index, like COLLADA's JOINT source order).
    // inverse bind = geometry_to_bone * inverse(bind shape), so that
    // inv_bind * bind_shape reproduces ufbx's per-bone geometry_to_bone exactly.
    for (size_t i = 0; i < skin->clusters.count; ++i)
    {
        const ufbx_skin_cluster* cluster = skin->clusters.data[i];
        std::string name = cluster->bone_node ? fbx_str(cluster->bone_node->name) : fbx_str(cluster->name);
        skin_info.mJointNames.push_back(mapJointName(name));
        skin_info.mJointNums.push_back(-1);

        const ufbx_matrix inv_bind = ufbx_matrix_mul(&cluster->geometry_to_bone, &inv_bind_shape);
        skin_info.mInvBindMatrix.push_back(LLMatrix4a(to_llmatrix(inv_bind)));
    }

    critiqueRigForUploadApplicability(skin_info.mJointNames);

    if (mHasSkeleton)
    {
        // FIXME (inherited from LLDAELoader): mesh_id is used to determine which
        // mesh gets to set the joint offset, in the event of a conflict. Since
        // we don't know the mesh id yet, we can't guarantee that joint offsets
        // will be applied with the same priority as in the uploaded model.
        LLUUID fake_mesh_id;
        fake_mesh_id.generate();

        // Set the joint translations on the avatar
        for (JointMap::const_iterator master_it = mJointMap.begin(); master_it != mJointMap.end(); ++master_it)
        {
            const std::string& looking_for_joint = master_it->first;

            JointTransformMap::iterator found = mJointList.find(looking_for_joint);
            if (found != mJointList.end())
            {
                LLJoint* pJoint = mJointLookupFunc(looking_for_joint, mOpaqueData);
                if (pJoint)
                {
                    const LLVector3& joint_pos = found->second.getTranslation();
                    if (pJoint->aboveJointPosThreshold(joint_pos))
                    {
                        bool override_changed; // not used
                        pJoint->addAttachmentPosOverride(joint_pos, fake_mesh_id, "", override_changed);
                        if (skin_info.mLockScaleIfJointPosition)
                        {
                            pJoint->addAttachmentScaleOverride(pJoint->getDefaultScale(), fake_mesh_id, "");
                        }
                    }
                }
                else
                {
                    LL_WARNS("Mesh") << "Tried to apply joint position from .fbx, but it did not exist in the avatar rig." << LL_ENDL;
                }
            }
        }
    }

    // Alternate bind matrices (inverse bind with the joint's position), in the
    // same order as the joint names -- see LLDAELoader::processDomModel.
    const int joint_count = static_cast<int>(skin_info.mJointNames.size());
    for (int i = 0; i < joint_count; ++i)
    {
        const std::string& looking_for_joint = skin_info.mJointNames[i];
        if (mJointMap.find(looking_for_joint) != mJointMap.end()
            && skin_info.mInvBindMatrix.size() > (size_t)i)
        {
            LLMatrix4 new_inverse = LLMatrix4(skin_info.mInvBindMatrix[i].getF32ptr());
            new_inverse.setTranslation(mJointList[looking_for_joint].getTranslation());
            skin_info.mAlternateBindMatrix.push_back(LLMatrix4a(new_inverse));
        }
        else
        {
            LL_DEBUGS("Mesh") << "Possibly misnamed/missing joint [" << looking_for_joint << "] " << LL_ENDL;
        }
    }

    size_t bind_count = skin_info.mAlternateBindMatrix.size();
    if (bind_count > 0 && bind_count != (size_t)joint_count)
    {
        LL_WARNS("Mesh") << "Model " << model->mLabel << " has invalid joint bind matrix list." << LL_ENDL;
    }

    // Raw positions, transformed into normalized volume space (the key of mSkinWeights).
    model->mPosition.clear();
    model->mPosition.reserve(mesh->num_vertices);
    for (size_t v = 0; v < mesh->num_vertices; ++v)
    {
        const ufbx_vec3& p = mesh->vertices.data[v];
        LLVector3 pos((F32)p.x, (F32)p.y, (F32)p.z);
        pos = pos * inverse_normalized_transformation;
        model->mPosition.push_back(pos);
    }

    // Skin weights: up to 4 most significant per vertex, normalized.
    const size_t weighted_vertices = llmin(skin->vertices.count, model->mPosition.size());
    for (size_t v = 0; v < weighted_vertices; ++v)
    {
        const ufbx_skin_vertex& sv = skin->vertices.data[v];

        LLModel::weight_list weight_list;
        for (uint32_t w = 0; w < sv.num_weights; ++w)
        {
            const ufbx_skin_weight& sw = skin->weights.data[sv.weight_begin + w];
            weight_list.push_back(LLModel::JointWeight((S32)sw.cluster_index, (F32)sw.weight));
        }

        // sort by joint weight
        std::sort(weight_list.begin(), weight_list.end(), LLModel::CompareWeightGreater());

        std::vector<LLModel::JointWeight> wght;
        F32 total = 0.f;
        for (U32 i = 0; i < llmin((U32)4, (U32)weight_list.size()); ++i)
        { // take up to 4 most significant weights
            if (weight_list[i].mWeight > 0.f)
            {
                wght.push_back(weight_list[i]);
                total += weight_list[i].mWeight;
            }
        }

        if (total > 0.f)
        {
            F32 scale = 1.f / total;
            if (scale != 1.f)
            { // normalize weights
                for (U32 i = 0; i < wght.size(); ++i)
                {
                    wght[i].mWeight *= scale;
                }
            }
        }

        model->mSkinWeights[model->mPosition[v]] = wght;
    }

    // Add instance to scene for this model: normalization, then geometry -> world
    // at bind time (the role LLDAELoader's root unit/up-axis transform plays).
    LLMatrix4 transformation;
    transformation.initScale(mesh_scale_vector);
    transformation.setTranslation(mesh_translation_vector);
    transformation *= to_llmatrix(bind_shape);

    material_map materials = getMaterials(model, mesh);

    std::string label = model->mLabel;
    size_t ext_pos = getSuffixPosition(label);
    if (ext_pos != std::string::npos)
    {
        label = label.substr(0, ext_pos);
    }

    mScene[transformation].push_back(LLModelInstance(model, label, transformation, materials));
    stretch_extents(model, transformation);
}

LLModelLoader::material_map LLFBXLoader::getMaterials(const LLModel* model, const ufbx_mesh* mesh)
{
    material_map materials;
    for (const std::string& binding : model->mMaterialList)
    {
        LLImportMaterial import_material;
        for (size_t i = 0; i < mesh->materials.count; ++i)
        {
            const ufbx_material* material = mesh->materials.data[i];
            if (material && fbx_str(material->name) == binding)
            {
                import_material = toImportMaterial(material);
                break;
            }
        }
        import_material.mBinding = binding;
        materials[binding] = import_material;
    }
    return materials;
}

// Mirrors LLDAELoader::profileToMaterial: diffuse color, diffuse texture file
// and "fullbright" from a bright emission color.
LLImportMaterial LLFBXLoader::toImportMaterial(const ufbx_material* material)
{
    LLImportMaterial mat;
    mat.mFullbright = false;

    const ufbx_material_map& diffuse = material->fbx.diffuse_color;
    if (diffuse.has_value)
    {
        mat.mDiffuseColor = LLColor4((F32)diffuse.value_vec3.x, (F32)diffuse.value_vec3.y, (F32)diffuse.value_vec3.z, 1.f);
    }

    const ufbx_texture* texture = diffuse.texture;
    if (texture && texture->type == UFBX_TEXTURE_FILE)
    {
        // ufbx's filename is already resolved against the .fbx location; also try
        // the absolute path recorded by the exporter and the relative one beside
        // the .fbx. Embedded image content is not supported (as for .dae).
        const std::string candidates[] = {
            fbx_str(texture->filename),
            fbx_str(texture->absolute_filename),
            texture->relative_filename.length ? dir_of(mFilename) + fbx_str(texture->relative_filename) : std::string(),
        };
        for (const std::string& path : candidates)
        {
            if (!path.empty() && LLFile::isfile(path))
            {
                mat.mDiffuseMapFilename = path;
                mat.mDiffuseMapLabel = fbx_str(material->name);
                break;
            }
        }
        if (mat.mDiffuseMapFilename.empty())
        {
            LL_WARNS("Mesh") << "FBX material " << fbx_str(material->name) << ": diffuse texture not found ("
                             << fbx_str(texture->filename) << ")" << LL_ENDL;
        }
    }

    const ufbx_material_map& emission = material->fbx.emission_color;
    if (emission.has_value)
    {
        const ufbx_material_map& factor = material->fbx.emission_factor;
        const F32 f = factor.has_value ? (F32)factor.value_real : 1.f;
        const F32 avg = f * (F32)(emission.value_vec3.x + emission.value_vec3.y + emission.value_vec3.z) / 3.f;
        if (avg > 0.25f)
        {
            mat.mFullbright = true;
        }
    }

    return mat;
}
