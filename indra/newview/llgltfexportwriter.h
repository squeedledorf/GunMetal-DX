/**
 * @file llgltfexportwriter.h
 * @brief Viewer-independent glTF 2.0 scene description + writer (#908).
 *
 * The exporter (llgltfexporter.cpp) gathers geometry, materials and encoded
 * images from the viewer into these plain structs; this module turns them into
 * a tinygltf::Model and writes .glb (default) or .gltf (+ side files). Nothing
 * here touches viewer state, so it can be compiled and tested on its own.
 *
 * Conventions (the exporter fills the structs in these):
 *  - Node transforms and vertex data are in Second Life's frame (Z up, right
 *    handed). When Scene::z_up_to_y_up is set the writer adds a single top-level
 *    node that rotates the whole scene -90 degrees about X into glTF's Y-up frame.
 *  - Texture coordinates are already in glTF convention (origin top-left, i.e.
 *    v = 1 - SL v); images are PNG bytes with the top row first.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors.
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

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace LLGLTFExport
{
    // One textureInfo slot of a material.
    struct TextureRef
    {
        int32_t image = -1;          // index into Scene::images, -1 = no texture
        int32_t tex_coord = 0;       // TEXCOORD_<n> set of the primitive
        bool    has_transform = false; // emit KHR_texture_transform
        float   offset[2] = { 0.f, 0.f };
        float   rotation = 0.f;
        float   scale[2] = { 1.f, 1.f };

        bool operator==(const TextureRef& o) const;
    };

    struct Material
    {
        std::string name;
        float      base_color[4] = { 1.f, 1.f, 1.f, 1.f };
        float      metallic = 1.f;
        float      roughness = 1.f;
        float      emissive[3] = { 0.f, 0.f, 0.f };
        TextureRef base_color_tex;
        TextureRef metallic_roughness_tex;
        TextureRef normal_tex;
        TextureRef occlusion_tex;
        TextureRef emissive_tex;
        std::string alpha_mode = "OPAQUE"; // OPAQUE / MASK / BLEND
        float      alpha_cutoff = 0.5f;
        bool       double_sided = false;
        // string extras (e.g. "sl_material": "pbr" / "blinn-phong")
        std::vector<std::pair<std::string, std::string> > extras;

        bool operator==(const Material& o) const;
    };

    struct Primitive
    {
        std::vector<float>    positions; // xyz per vertex
        std::vector<float>    normals;   // xyz per vertex, or empty
        std::vector<float>    tangents;  // xyzw per vertex, or empty
        std::vector<std::vector<float> > uvs; // uv per vertex per set (TEXCOORD_n)
        std::vector<uint32_t> indices;   // triangle list
        int32_t               material = -1;

        size_t vertexCount() const { return positions.size() / 3; }
    };

    struct Node
    {
        std::string name;
        int32_t     parent = -1;         // index into Scene::nodes, -1 = top level
        float       translation[3] = { 0.f, 0.f, 0.f };
        float       rotation[4] = { 0.f, 0.f, 0.f, 1.f }; // x y z w
        float       scale[3] = { 1.f, 1.f, 1.f };
        std::string mesh_name;
        std::vector<Primitive> primitives; // empty = no mesh
        std::vector<std::pair<std::string, std::string> > extras_str;
        std::vector<std::pair<std::string, int32_t> >     extras_int;
    };

    struct Image
    {
        std::string          name;  // also the side-file stem for .gltf output
        std::vector<uint8_t> png;   // encoded PNG
    };

    struct Scene
    {
        std::string name = "Scene";
        std::string generator = "glTF exporter";
        bool        z_up_to_y_up = true;
        std::vector<Node>     nodes;
        std::vector<Material> materials;
        std::vector<Image>    images;
    };

    // Adds `m` unless an identical material exists; returns its index.
    int32_t addMaterialDedup(Scene& scene, const Material& m);

    // Serialises the scene to an in-memory GLB.
    bool toGLB(const Scene& scene, std::vector<uint8_t>& out, std::string& error);

    // Serialises the scene to a self-contained .gltf (buffers + images as data URIs).
    bool toEmbeddedGLTF(const Scene& scene, std::string& out, std::string& error);

    // Writes `path`. A ".gltf" extension writes JSON with a .bin and one PNG per
    // image next to it; if those side files cannot be written (e.g. a sandboxed
    // file portal that only grants the chosen path) it falls back to a single
    // self-contained .gltf and sets *note. Anything else writes binary glTF.
    // Nothing is written until the whole model has been built; on a failed write
    // the partial main file is removed.
    bool writeScene(const Scene& scene, const std::string& path, std::string& error, std::string* note);
}
