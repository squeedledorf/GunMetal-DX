/**
 * @file llgltfexportwriter.cpp
 * @brief Viewer-independent glTF 2.0 scene writer (#908). See the header.
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

#ifndef LLGLTFEXPORT_STANDALONE
#include "llviewerprecompiledheaders.h"
#include "llfile.h"
#endif

#include "llgltfexportwriter.h"

// The tinygltf implementation (and stb) is compiled once, in gltf/llgltfloader.cpp.
#include "tinygltf/tiny_gltf.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>

namespace LLGLTFExport
{

bool TextureRef::operator==(const TextureRef& o) const
{
    return image == o.image && tex_coord == o.tex_coord && has_transform == o.has_transform
        && offset[0] == o.offset[0] && offset[1] == o.offset[1] && rotation == o.rotation
        && scale[0] == o.scale[0] && scale[1] == o.scale[1];
}

bool Material::operator==(const Material& o) const
{
    for (int i = 0; i < 4; ++i) if (base_color[i] != o.base_color[i]) return false;
    for (int i = 0; i < 3; ++i) if (emissive[i] != o.emissive[i]) return false;
    return name == o.name && metallic == o.metallic && roughness == o.roughness
        && base_color_tex == o.base_color_tex && metallic_roughness_tex == o.metallic_roughness_tex
        && normal_tex == o.normal_tex && occlusion_tex == o.occlusion_tex && emissive_tex == o.emissive_tex
        && alpha_mode == o.alpha_mode && alpha_cutoff == o.alpha_cutoff && double_sided == o.double_sided
        && extras == o.extras;
}

int32_t addMaterialDedup(Scene& scene, const Material& m)
{
    for (size_t i = 0; i < scene.materials.size(); ++i)
    {
        if (scene.materials[i] == m) return (int32_t)i;
    }
    scene.materials.push_back(m);
    return (int32_t)scene.materials.size() - 1;
}

namespace
{
    const char* const EXT_TEXTURE_TRANSFORM = "KHR_texture_transform";

    // Appends raw bytes to buffer 0 as a new bufferView (4-byte aligned).
    int addBufferView(tinygltf::Model& model, const void* data, size_t bytes, int target, int byte_stride = 0)
    {
        tinygltf::Buffer& buf = model.buffers[0];
        while (buf.data.size() % 4) buf.data.push_back(0);
        const size_t offset = buf.data.size();
        buf.data.resize(offset + bytes);
        if (bytes) memcpy(&buf.data[offset], data, bytes);

        tinygltf::BufferView view;
        view.buffer = 0;
        view.byteOffset = offset;
        view.byteLength = bytes;
        view.byteStride = byte_stride;
        view.target = target;
        model.bufferViews.push_back(view);
        return (int)model.bufferViews.size() - 1;
    }

    int addFloatAccessor(tinygltf::Model& model, const std::vector<float>& v, int comps, int type, bool minmax)
    {
        const size_t count = v.size() / comps;
        const int view = addBufferView(model, v.data(), v.size() * sizeof(float), TINYGLTF_TARGET_ARRAY_BUFFER);
        tinygltf::Accessor acc;
        acc.bufferView = view;
        acc.byteOffset = 0;
        acc.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
        acc.count = count;
        acc.type = type;
        if (minmax && count)
        {
            acc.minValues.assign(comps, std::numeric_limits<double>::max());
            acc.maxValues.assign(comps, -std::numeric_limits<double>::max());
            for (size_t i = 0; i < count; ++i)
            {
                for (int c = 0; c < comps; ++c)
                {
                    const double x = v[i * comps + c];
                    acc.minValues[c] = std::min(acc.minValues[c], x);
                    acc.maxValues[c] = std::max(acc.maxValues[c], x);
                }
            }
        }
        model.accessors.push_back(acc);
        return (int)model.accessors.size() - 1;
    }

    int addIndexAccessor(tinygltf::Model& model, const std::vector<uint32_t>& idx, size_t vertex_count)
    {
        tinygltf::Accessor acc;
        acc.byteOffset = 0;
        acc.count = idx.size();
        acc.type = TINYGLTF_TYPE_SCALAR;
        if (vertex_count <= 0xFFFF)
        {
            std::vector<uint16_t> idx16(idx.begin(), idx.end());
            acc.bufferView = addBufferView(model, idx16.data(), idx16.size() * sizeof(uint16_t), TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER);
            acc.componentType = TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT;
        }
        else
        {
            acc.bufferView = addBufferView(model, idx.data(), idx.size() * sizeof(uint32_t), TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER);
            acc.componentType = TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT;
        }
        model.accessors.push_back(acc);
        return (int)model.accessors.size() - 1;
    }

    template <typename T>
    bool fillTextureInfo(const Scene& scene, T& info, const TextureRef& ref,
                         const std::vector<int>& image_to_texture, bool& used_transform)
    {
        if (ref.image < 0 || ref.image >= (int32_t)scene.images.size()) return false;
        info.index = image_to_texture[ref.image];
        info.texCoord = ref.tex_coord;
        if (ref.has_transform)
        {
            tinygltf::Value::Object t;
            t["offset"] = tinygltf::Value(tinygltf::Value::Array({ tinygltf::Value((double)ref.offset[0]), tinygltf::Value((double)ref.offset[1]) }));
            t["scale"] = tinygltf::Value(tinygltf::Value::Array({ tinygltf::Value((double)ref.scale[0]), tinygltf::Value((double)ref.scale[1]) }));
            t["rotation"] = tinygltf::Value((double)ref.rotation);
            info.extensions[EXT_TEXTURE_TRANSFORM] = tinygltf::Value(t);
            used_transform = true;
        }
        return true;
    }

    tinygltf::Value makeExtras(const std::vector<std::pair<std::string, std::string> >& s,
                               const std::vector<std::pair<std::string, int32_t> >& i)
    {
        tinygltf::Value::Object o;
        for (const auto& kv : s) o[kv.first] = tinygltf::Value(kv.second);
        for (const auto& kv : i) o[kv.first] = tinygltf::Value((int)kv.second);
        return tinygltf::Value(o);
    }

    std::string fileStem(const std::string& path)
    {
        size_t slash = path.find_last_of("/\\");
        std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
        size_t dot = name.rfind('.');
        if (dot != std::string::npos && dot > 0) name = name.substr(0, dot);
        return name;
    }

    std::string lowerExtension(const std::string& path)
    {
        size_t slash = path.find_last_of("/\\");
        size_t dot = path.rfind('.');
        if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return std::string();
        std::string ext = path.substr(dot + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)tolower(c); });
        return ext;
    }

    // File-name-safe version of an image name.
    std::string safeName(const std::string& in)
    {
        std::string out;
        for (char c : in)
        {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
            out.push_back(ok ? c : '_');
        }
        return out.empty() ? std::string("image") : out;
    }

    enum EImageMode
    {
        IMAGES_IN_BUFFER,   // GLB / self-contained: PNG bytes in buffer 0 via bufferViews
        IMAGES_SIDE_FILES   // .gltf + PNG files next to it
    };

    bool buildModel(const Scene& scene, tinygltf::Model& model, EImageMode image_mode,
                    const std::string& side_stem, std::string& error)
    {
        model = tinygltf::Model();
        model.asset.version = "2.0";
        model.asset.generator = scene.generator;
        model.buffers.resize(1);

        bool used_transform = false;

        // Sampler: repeat + trilinear, matching how the viewer samples object textures.
        tinygltf::Sampler sampler;
        sampler.magFilter = TINYGLTF_TEXTURE_FILTER_LINEAR;
        sampler.minFilter = TINYGLTF_TEXTURE_FILTER_LINEAR_MIPMAP_LINEAR;
        sampler.wrapS = TINYGLTF_TEXTURE_WRAP_REPEAT;
        sampler.wrapT = TINYGLTF_TEXTURE_WRAP_REPEAT;
        model.samplers.push_back(sampler);

        // Images + textures (one texture per image).
        std::vector<int> image_to_texture(scene.images.size(), -1);
        for (size_t i = 0; i < scene.images.size(); ++i)
        {
            const Image& src = scene.images[i];
            if (src.png.empty())
            {
                error = "image " + src.name + " has no data";
                return false;
            }
            tinygltf::Image img;
            img.name = src.name;
            img.mimeType = "image/png";
            if (image_mode == IMAGES_IN_BUFFER)
            {
                img.bufferView = addBufferView(model, src.png.data(), src.png.size(), 0);
            }
            else
            {
                img.as_is = true;
                img.image = src.png;
                img.uri = side_stem + "_" + safeName(src.name) + ".png";
            }
            model.images.push_back(img);

            tinygltf::Texture tex;
            tex.source = (int)i;
            tex.sampler = 0;
            model.textures.push_back(tex);
            image_to_texture[i] = (int)model.textures.size() - 1;
        }

        // Materials.
        for (const Material& m : scene.materials)
        {
            tinygltf::Material out;
            out.name = m.name;
            out.pbrMetallicRoughness.baseColorFactor = { m.base_color[0], m.base_color[1], m.base_color[2], m.base_color[3] };
            out.pbrMetallicRoughness.metallicFactor = m.metallic;
            out.pbrMetallicRoughness.roughnessFactor = m.roughness;
            if (m.emissive[0] > 0.f || m.emissive[1] > 0.f || m.emissive[2] > 0.f)
            {
                out.emissiveFactor = { m.emissive[0], m.emissive[1], m.emissive[2] };
            }
            fillTextureInfo(scene, out.pbrMetallicRoughness.baseColorTexture, m.base_color_tex, image_to_texture, used_transform);
            fillTextureInfo(scene, out.pbrMetallicRoughness.metallicRoughnessTexture, m.metallic_roughness_tex, image_to_texture, used_transform);
            fillTextureInfo(scene, out.normalTexture, m.normal_tex, image_to_texture, used_transform);
            fillTextureInfo(scene, out.occlusionTexture, m.occlusion_tex, image_to_texture, used_transform);
            fillTextureInfo(scene, out.emissiveTexture, m.emissive_tex, image_to_texture, used_transform);
            out.alphaMode = m.alpha_mode;
            out.alphaCutoff = m.alpha_cutoff;
            out.doubleSided = m.double_sided;
            if (!m.extras.empty())
            {
                out.extras = makeExtras(m.extras, {});
            }
            model.materials.push_back(out);
        }

        // Nodes + meshes. The optional axis-conversion node goes first.
        const int base = scene.z_up_to_y_up ? 1 : 0;
        if (scene.z_up_to_y_up)
        {
            tinygltf::Node axis;
            axis.name = scene.name;
            // -90 degrees about X: SL (x, y, z) -> glTF (x, z, -y).
            const double h = std::sqrt(0.5);
            axis.rotation = { -h, 0.0, 0.0, h };
            model.nodes.push_back(axis);
        }

        for (size_t n = 0; n < scene.nodes.size(); ++n)
        {
            const Node& src = scene.nodes[n];
            if (src.parent >= (int32_t)n)
            {
                error = "node " + src.name + " lists a parent that does not precede it";
                return false;
            }
            tinygltf::Node out;
            out.name = src.name;
            if (src.translation[0] != 0.f || src.translation[1] != 0.f || src.translation[2] != 0.f)
            {
                out.translation = { src.translation[0], src.translation[1], src.translation[2] };
            }
            if (src.rotation[0] != 0.f || src.rotation[1] != 0.f || src.rotation[2] != 0.f || src.rotation[3] != 1.f)
            {
                // glTF requires a unit quaternion.
                double q[4] = { src.rotation[0], src.rotation[1], src.rotation[2], src.rotation[3] };
                const double len = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
                if (len > 0.0) for (double& c : q) c /= len;
                out.rotation = { q[0], q[1], q[2], q[3] };
            }
            if (src.scale[0] != 1.f || src.scale[1] != 1.f || src.scale[2] != 1.f)
            {
                out.scale = { src.scale[0], src.scale[1], src.scale[2] };
            }
            if (!src.extras_str.empty() || !src.extras_int.empty())
            {
                out.extras = makeExtras(src.extras_str, src.extras_int);
            }

            if (!src.primitives.empty())
            {
                tinygltf::Mesh mesh;
                mesh.name = src.mesh_name.empty() ? src.name : src.mesh_name;
                for (const Primitive& p : src.primitives)
                {
                    const size_t nv = p.vertexCount();
                    if (!nv || p.indices.empty() || p.indices.size() % 3)
                    {
                        continue; // nothing drawable
                    }
                    for (uint32_t i : p.indices)
                    {
                        if (i >= nv)
                        {
                            error = "node " + src.name + " has an out-of-range index";
                            return false;
                        }
                    }
                    tinygltf::Primitive prim;
                    prim.mode = TINYGLTF_MODE_TRIANGLES;
                    prim.attributes["POSITION"] = addFloatAccessor(model, p.positions, 3, TINYGLTF_TYPE_VEC3, true);
                    if (p.normals.size() == nv * 3)
                    {
                        prim.attributes["NORMAL"] = addFloatAccessor(model, p.normals, 3, TINYGLTF_TYPE_VEC3, false);
                    }
                    if (p.tangents.size() == nv * 4 && p.normals.size() == nv * 3)
                    {
                        prim.attributes["TANGENT"] = addFloatAccessor(model, p.tangents, 4, TINYGLTF_TYPE_VEC4, false);
                    }
                    for (size_t s = 0; s < p.uvs.size(); ++s)
                    {
                        if (p.uvs[s].size() != nv * 2) break; // sets must be contiguous
                        prim.attributes["TEXCOORD_" + std::to_string(s)] = addFloatAccessor(model, p.uvs[s], 2, TINYGLTF_TYPE_VEC2, false);
                    }
                    prim.indices = addIndexAccessor(model, p.indices, nv);
                    if (p.material >= 0 && p.material < (int32_t)scene.materials.size())
                    {
                        prim.material = p.material;
                    }
                    mesh.primitives.push_back(prim);
                }
                if (!mesh.primitives.empty())
                {
                    model.meshes.push_back(mesh);
                    out.mesh = (int)model.meshes.size() - 1;
                }
            }
            model.nodes.push_back(out);
        }

        // Children lists.
        tinygltf::Scene gscene;
        gscene.name = scene.name;
        if (scene.z_up_to_y_up)
        {
            gscene.nodes.push_back(0);
        }
        for (size_t n = 0; n < scene.nodes.size(); ++n)
        {
            const int idx = (int)n + base;
            const int32_t parent = scene.nodes[n].parent;
            if (parent >= 0)
            {
                model.nodes[parent + base].children.push_back(idx);
            }
            else if (scene.z_up_to_y_up)
            {
                model.nodes[0].children.push_back(idx);
            }
            else
            {
                gscene.nodes.push_back(idx);
            }
        }
        model.scenes.push_back(gscene);
        model.defaultScene = 0;

        if (used_transform)
        {
            model.extensionsUsed.push_back(EXT_TEXTURE_TRANSFORM);
        }

        if (model.buffers[0].data.empty())
        {
            model.buffers.clear();
        }
        return true;
    }

    bool writeBytes(const std::string& path, const char* data, size_t size, std::string& error)
    {
#ifdef LLGLTFEXPORT_STANDALONE
        std::ofstream out(path.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
#else
        llofstream out(path.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
#endif
        if (!out.is_open())
        {
            error = "cannot open " + path + " for writing";
            return false;
        }
        out.write(data, (std::streamsize)size);
        out.close();
        if (out.fail())
        {
            error = "write to " + path + " failed (disk full?)";
#ifdef LLGLTFEXPORT_STANDALONE
            std::remove(path.c_str());
#else
            LLFile::remove(path);
#endif
            return false;
        }
        return true;
    }
}

bool toGLB(const Scene& scene, std::vector<uint8_t>& out, std::string& error)
{
    tinygltf::Model model;
    if (!buildModel(scene, model, IMAGES_IN_BUFFER, std::string(), error)) return false;
    tinygltf::TinyGLTF gltf;
    std::ostringstream str;
    if (!gltf.WriteGltfSceneToStream(&model, str, false, true))
    {
        error = "glTF serialisation failed";
        return false;
    }
    const std::string s = str.str();
    out.assign(s.begin(), s.end());
    return true;
}

bool toEmbeddedGLTF(const Scene& scene, std::string& out, std::string& error)
{
    tinygltf::Model model;
    if (!buildModel(scene, model, IMAGES_IN_BUFFER, std::string(), error)) return false;
    tinygltf::TinyGLTF gltf;
    std::ostringstream str;
    if (!gltf.WriteGltfSceneToStream(&model, str, true, false))
    {
        error = "glTF serialisation failed";
        return false;
    }
    out = str.str();
    return true;
}

bool writeScene(const Scene& scene, const std::string& path, std::string& error, std::string* note)
{
    const std::string ext = lowerExtension(path);
    if (ext == "gltf")
    {
        // JSON + .bin + one PNG per image next to it.
        tinygltf::Model model;
        if (!buildModel(scene, model, IMAGES_SIDE_FILES, safeName(fileStem(path)), error)) return false;
        tinygltf::TinyGLTF gltf;
        if (gltf.WriteGltfSceneToFile(&model, path, false, false, true, false))
        {
            return true;
        }
        // Side files refused (sandboxed portal that only grants the chosen path, a
        // read-only directory, ...): one self-contained file at exactly that path.
        std::string json;
        if (!toEmbeddedGLTF(scene, json, error)) return false;
        if (!writeBytes(path, json.data(), json.size(), error)) return false;
        if (note) *note = "side files could not be written; textures and geometry are embedded in the .gltf";
        return true;
    }

    std::vector<uint8_t> glb;
    if (!toGLB(scene, glb, error)) return false;
    return writeBytes(path, (const char*)glb.data(), glb.size(), error);
}

} // namespace LLGLTFExport
