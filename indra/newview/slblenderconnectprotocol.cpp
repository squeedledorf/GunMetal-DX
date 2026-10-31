// SPDX-License-Identifier: LGPL-2.1-only
#include "slblenderconnectprotocol.h"
#include <boost/json.hpp>
#include <boost/uuid/detail/sha1.hpp>
#include <tinygltf/tiny_gltf.h>
#include <cstdint>
#include <cstring>
#include <stdexcept>
namespace
{
    using boost::json::value;
    class Rejection final : public std::runtime_error { public: using std::runtime_error::runtime_error; };

    uint32_t le32(const char* p)
    {
        const auto* b = reinterpret_cast<const unsigned char*>(p);
        return uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
    }

    // Reject all URI-bearing resources, including data URIs. Only the GLB BIN
    // chunk may supply buffers/images. Restrict extensions to material features
    // our importer understands; compressed geometry and animation are excluded.
    void checkJSON(const value& v, size_t depth = 0, bool material = false, bool ignored = false)
    {
        if (depth > 32) throw Rejection("GLB JSON nesting exceeds 32");
        if (v.is_object())
        {
            if (v.as_object().size() > 128) throw Rejection("Too many object members");
            for (const auto& member : v.as_object())
            {
                const auto key = member.key();
                if (ignored) { checkJSON(member.value(), depth + 1, false, true); continue; }
                if (depth == 0 && key == "materials")
                {
                    for (const auto& entry : member.value().as_array()) checkJSON(entry, depth + 2, true);
                    continue;
                }
                if (key == "uri" || key == "sparse" || key == "targets")
                    throw Rejection("External resources, sparse accessors and morph targets are unsupported");
                if (key == "extensionsRequired")
                {
                    for (const auto& name : member.value().as_array())
                        if (name.as_string() != "KHR_materials_unlit" && name.as_string() != "KHR_texture_transform"
                            && name.as_string() != "KHR_materials_emissive_strength")
                            throw Rejection("Unsupported required extension");
                }
                if (key == "extensions")
                {
                    for (const auto& extension : member.value().as_object())
                    {
                        const auto name = extension.key();
                        if (name != "KHR_materials_unlit" && name != "KHR_texture_transform"
                            && name != "KHR_materials_emissive_strength" && !material)
                            throw Rejection("Unsupported geometry or texture-source extension");
                    }
                }
                checkJSON(member.value(), depth + 1, false, material && key == "extensions");
            }
        }
        else if (v.is_array())
        {
            if (v.as_array().size() > 4096) throw Rejection("Too many GLB elements");
            for (const auto& entry : v.as_array()) checkJSON(entry, depth + 1, false, ignored);
        }
    }

    // Both PNG and JPEG headers are checked before the image decoder can allocate
    // memory. A compressed image's file size alone does not bound its decoded size.
    size_t imagePixels(const char* p, size_t count)
    {
        const auto* b = reinterpret_cast<const unsigned char*>(p);
        auto be16 = [&](size_t i) { return size_t(b[i]) * 256 + b[i + 1]; };
        auto be32 = [&](size_t i) { return be16(i) * 65536 + be16(i + 2); };
        size_t width = 0, height = 0;
        if (count >= 24 && std::memcmp(p, "\x89PNG\r\n\x1a\n", 8) == 0
            && std::memcmp(p + 12, "IHDR", 4) == 0)
        {
            width = be32(16); height = be32(20);
        }
        else if (count >= 4 && b[0] == 0xff && b[1] == 0xd8)
        {
            for (size_t i = 2; i + 4 <= count;)
            {
                if (b[i++] != 0xff) break;
                while (i < count && b[i] == 0xff) ++i;
                if (i >= count) break;
                const unsigned marker = b[i++];
                if (marker == 0xd9 || marker == 0xda) break;
                if (i + 2 > count) break;
                const size_t length = be16(i);
                if (length < 2 || length > count - i) break;
                if (marker == 0xc0 || marker == 0xc1 || marker == 0xc2)
                {
                    if (length >= 8) { height = be16(i + 3); width = be16(i + 5); }
                    break;
                }
                i += length;
            }
        }
        if (!width || !height || width > 4096 || height > 4096)
            throw Rejection("Images must be PNG/JPEG, at most 4096 by 4096");
        return width * height;
    }

    size_t number(const boost::json::object& obj, const char* key, size_t fallback = 0)
    {
        const auto* v = obj.if_contains(key);
        if (!v) return fallback;
        if (v->is_int64() && v->as_int64() >= 0) return size_t(v->as_int64());
        if (v->is_uint64() && v->as_uint64() <= SLBlenderConnect::MAX_BYTES) return v->as_uint64();
        throw Rejection("Invalid GLB integer");
    }

    void validateDocument(std::string_view bytes, SLBlenderConnect::Document& snapshot)
    {
        if (bytes.size() > SLBlenderConnect::MAX_BYTES) throw Rejection("GLB exceeds 32 MiB");
        if (bytes.size() < 28 || le32(bytes.data()) != 0x46546c67
            || le32(bytes.data() + 4) != 2 || le32(bytes.data() + 8) != bytes.size())
            throw Rejection("Invalid GLB 2 header");
        const size_t json_size = le32(bytes.data() + 12);
        if (!json_size || json_size > 512 * 1024 || json_size % 4
            || json_size > bytes.size() - 28 || le32(bytes.data() + 16) != 0x4e4f534a)
            throw Rejection("Invalid GLB JSON chunk (limit 512 KiB)");
        const size_t bin_start = 28 + json_size;
        const size_t bin_size = le32(bytes.data() + 20 + json_size);
        if (bin_size != bytes.size() - bin_start || bin_size % 4
            || le32(bytes.data() + 24 + json_size) != 0x004e4942)
            throw Rejection("Expected one complete GLB BIN chunk");
        boost::json::parse_options options;
        options.max_depth = 32;
        const auto doc = boost::json::parse(std::string_view(bytes.data() + 20, json_size), {}, options);
        checkJSON(doc);
        const auto& root = doc.as_object();
        if (const auto* used = root.if_contains("extensionsUsed"))
            for (const auto& extension : used->as_array())
            {
                const auto name = extension.as_string();
                if (name == "KHR_mesh_quantization" || name == "KHR_draco_mesh_compression"
                    || name == "EXT_meshopt_compression" || name == "EXT_mesh_gpu_instancing"
                    || name == "KHR_texture_basisu" || name == "EXT_texture_webp")
                    throw Rejection("Unsupported geometry or texture-source extension");
            }
        if (root.at("asset").as_object().at("version").as_string() != "2.0")
            throw Rejection("Expected glTF 2.0");
        const auto* animations = root.if_contains("animations");
        if (animations && !animations->as_array().empty())
            throw Rejection("Export without animations");
        const auto& buffers = root.at("buffers").as_array();
        if (buffers.size() != 1 || number(buffers[0].as_object(), "byteLength") > bin_size)
            throw Rejection("Expected one embedded buffer");
        const size_t buffer_size = number(buffers[0].as_object(), "byteLength");
        if (bin_size - buffer_size > 3) throw Rejection("Invalid GLB buffer padding");
        if (const auto* materials = root.if_contains("materials"))
        {
            if (materials->as_array().size() > 16) throw Rejection("At most 16 materials are supported");
            for (const auto& material : materials->as_array())
            {
                const auto* name = material.as_object().if_contains("name");
                const auto binding = name && !name->as_string().empty()
                    ? std::string(name->as_string()) : "mat" + std::to_string(snapshot.materialBindings.size());
                snapshot.materialBindings.push_back(binding);
                auto identity = material.as_object();
                identity.erase("name");
                snapshot.materialKeys.push_back(boost::json::serialize(identity));
            }
        }
        bool skinned = false, unskinned = false;
        for (const auto& node : root.at("nodes").as_array())
        {
            if (!node.as_object().contains("mesh")) continue;
            if (node.as_object().contains("skin")) skinned = true;
            else unskinned = true;
        }
        if (skinned && unskinned) throw Rejection("Publish static and rigged meshes separately");
        // Accept one scene with unique root nodes and a bounded forest. Otherwise
        // repeated roots (including roots that are children of other roots) would
        // amplify the uploader's recursive traversal beyond our per-node budget.
        const auto& nodes = root.at("nodes").as_array();
        const auto& scenes = root.at("scenes").as_array();
        if (scenes.size() != 1 || number(root, "scene") != 0)
            throw Rejection("Export one scene");
        std::vector<unsigned char> parents(nodes.size(), 0), visited(nodes.size(), 0);
        for (const auto& node : nodes)
        {
            if (const auto* children = node.as_object().if_contains("children"))
            {
                for (const auto& child : children->as_array())
                {
                    const size_t index = size_t(child.as_int64());
                    if (index >= nodes.size() || parents[index]++)
                        throw Rejection("Invalid node forest");
                }
            }
        }
        std::vector<std::pair<size_t, size_t>> pending;
        for (const auto& entry : scenes[0].as_object().at("nodes").as_array())
        {
            const size_t index = size_t(entry.as_int64());
            if (index >= nodes.size() || parents[index]) throw Rejection("Invalid scene root");
            pending.emplace_back(index, 0);
        }
        size_t visits = 0;
        while (!pending.empty())
        {
            const auto [index, depth] = pending.back();
            pending.pop_back();
            if (depth > 128 || visited[index]++) throw Rejection("Repeated or excessively deep node traversal");
            ++visits;
            if (const auto* children = nodes[index].as_object().if_contains("children"))
                for (const auto& child : children->as_array()) pending.emplace_back(size_t(child.as_int64()), depth + 1);
        }
        if (visits != nodes.size()) throw Rejection("All nodes must belong to the exported scene");
        // The uploader expands every mesh-node instance into its own LLModel.
        // Budget those uses, not only the distinct accessors in the file.
        const auto& meshes = root.at("meshes").as_array();
        const auto& accessors = root.at("accessors").as_array();
        size_t vertices = 0, indices = 0, parts = 0;
        for (const auto& node : root.at("nodes").as_array())
        {
            if (!node.as_object().contains("mesh")) continue;
            const size_t mesh_index = number(node.as_object(), "mesh", meshes.size());
            if (mesh_index >= meshes.size()) throw Rejection("Invalid mesh reference");
            for (const auto& primitive : meshes[mesh_index].as_object().at("primitives").as_array())
            {
                const auto& obj = primitive.as_object();
                if (number(obj, "mode", 4) != 4) throw Rejection("Export triangles only");
                const size_t position = number(obj.at("attributes").as_object(), "POSITION", accessors.size());
                if (position >= accessors.size()) throw Rejection("Invalid position accessor");
                const size_t nv = number(accessors[position].as_object(), "count");
                const size_t index = number(obj, "indices", accessors.size());
                const size_t ni = index < accessors.size() ? number(accessors[index].as_object(), "count") : nv;
                if (nv > 500000 || vertices > 500000 - nv || ni > 1500000 || indices > 1500000 - ni)
                    throw Rejection("Instanced geometry exceeds Blender Connect budget");
                vertices += nv; indices += ni;
                parts += (nv + 65534) / 65535;
                if (parts > 128) throw Rejection("Too many generated mesh parts");
            }
        }
        if (!vertices || !indices) throw Rejection("No mesh geometry");
        const auto& views = root.at("bufferViews").as_array();
        for (const auto& view : views)
        {
            const auto& obj = view.as_object();
            const size_t offset = number(obj, "byteOffset"), length = number(obj, "byteLength");
            if (number(obj, "buffer") != 0 || offset > buffer_size || length > buffer_size - offset)
                throw Rejection("Invalid buffer view range");
        }
        size_t elements = 0;
        for (const auto& accessor : root.at("accessors").as_array())
        {
            const size_t count = number(accessor.as_object(), "count");
            if (count > 1000000 || elements > 4000000 - count)
                throw Rejection("Blender Connect exceeds geometry budget");
            elements += count;
        }
        size_t pixels = 0;
        if (const auto* images = root.if_contains("images"))
        {
            if (images->as_array().size() > 32) throw Rejection("At most 32 images are supported");
            for (const auto& image : images->as_array())
            {
                const auto& obj = image.as_object();
                const size_t index = number(obj, "bufferView", views.size());
                if (index >= views.size()) throw Rejection("Image must be embedded");
                const auto& view = views[index].as_object();
                pixels += imagePixels(bytes.data() + bin_start + number(view, "byteOffset"), number(view, "byteLength"));
                if (pixels > 32 * 1024 * 1024) throw Rejection("Decoded images exceed 32 megapixels");
            }
        }

        // Validate the ranges consumed by both the embedded decoder and the mesh
        // importer. These checks run before either parser can allocate/copy data.
        std::vector<size_t> counts, sizes, starts, strides;
        for (const auto& accessor : accessors)
        {
            const auto& obj = accessor.as_object();
            const size_t view_index = number(obj, "bufferView", views.size());
            if (view_index >= views.size()) throw Rejection("Accessor needs an embedded buffer view");
            const auto& view = views[view_index].as_object();
            const size_t component = number(obj, "componentType");
            const size_t unit = component == 5120 || component == 5121 ? 1
                : component == 5122 || component == 5123 ? 2 : component == 5125 || component == 5126 ? 4 : 0;
            const auto type = obj.at("type").as_string();
            const size_t lanes = type == "SCALAR" ? 1 : type == "VEC2" ? 2
                : type == "VEC3" ? 3 : type == "VEC4" ? 4 : type == "MAT4" ? 16 : 0;
            const size_t count = number(obj, "count");
            if (!unit || !lanes || !count) throw Rejection("Invalid accessor type or empty count");
            const size_t size = unit * lanes, stride = number(view, "byteStride", size);
            const size_t offset = number(obj, "byteOffset"), length = number(view, "byteLength");
            if (stride < size || stride > 252 || offset > length
                || (count - 1) * stride + size > length - offset)
                throw Rejection("Accessor exceeds embedded buffer view");
            counts.push_back(count); sizes.push_back(unit);
            starts.push_back(bin_start + number(view, "byteOffset") + offset); strides.push_back(stride);
        }
        for (const auto& mesh : meshes)
            for (const auto& primitive : mesh.as_object().at("primitives").as_array())
            {
                const auto& obj = primitive.as_object();
                const auto& attrs = obj.at("attributes").as_object();
                const size_t pos = number(attrs, "POSITION", accessors.size());
                if (pos >= counts.size()) throw Rejection("Missing position accessor");
                for (const auto& attribute : attrs)
                {
                    const size_t index = static_cast<size_t>(attribute.value().as_int64());
                    if (index >= counts.size() || counts[index] != counts[pos])
                        throw Rejection("Attribute count or reference mismatch");
                }
                if (obj.contains("indices"))
                {
                    const size_t index = number(obj, "indices", accessors.size());
                    if (index >= counts.size()) throw Rejection("Invalid triangle index accessor");
                    const auto& acc = accessors[index].as_object();
                    const size_t component = number(acc, "componentType");
                    if (acc.at("type").as_string() != "SCALAR" || (component != 5121 && component != 5123 && component != 5125)
                        || counts[index] % 3) throw Rejection("Invalid triangle index type/count");
                    for (size_t i = 0; i < counts[index]; ++i)
                    {
                        uint32_t vertex = 0;
                        for (size_t b = 0; b < sizes[index]; ++b)
                            vertex |= uint32_t(static_cast<unsigned char>(bytes[starts[index] + i * strides[index] + b])) << (b * 8);
                        if (vertex >= counts[pos]) throw Rejection("Triangle index exceeds vertex count");
                    }
                }
            }
        const auto* image_array = root.if_contains("images");
        const size_t image_count = image_array ? image_array->as_array().size() : 0;
        const auto* texture_array = root.if_contains("textures");
        const size_t texture_count = texture_array ? texture_array->as_array().size() : 0;
        if (texture_array)
            for (const auto& texture : texture_array->as_array())
                if (number(texture.as_object(), "source", image_count) >= image_count)
                    throw Rejection("Texture references missing embedded image");
        if (const auto* materials = root.if_contains("materials"))
            for (const auto& material : materials->as_array())
            {
                auto textureOK = [&](const boost::json::object& owner, const char* key)
                {
                    if (const auto* info = owner.if_contains(key))
                        if (number(info->as_object(), "index", texture_count) >= texture_count)
                            throw Rejection("Material references missing texture");
                };
                const auto& obj = material.as_object();
                for (const auto* key : {"normalTexture", "emissiveTexture", "occlusionTexture"}) textureOK(obj, key);
                if (const auto* pbr = obj.if_contains("pbrMetallicRoughness"))
                    for (const auto* key : {"baseColorTexture", "metallicRoughnessTexture"}) textureOK(pbr->as_object(), key);
            }
        if (const auto* skins = root.if_contains("skins"))
            for (const auto& skin : skins->as_array())
            {
                const auto& obj = skin.as_object();
                const auto& joints = obj.at("joints").as_array();
                if (joints.empty() || joints.size() > 256) throw Rejection("Skin joint limit exceeded");
                for (const auto& joint : joints)
                    if (joint.as_int64() < 0 || static_cast<uint64_t>(joint.as_int64()) >= nodes.size())
                        throw Rejection("Invalid skin joint reference");
                if (obj.contains("inverseBindMatrices"))
                {
                    const size_t index = number(obj, "inverseBindMatrices", counts.size());
                    if (index >= counts.size() || counts[index] < joints.size()
                        || accessors[index].as_object().at("type").as_string() != "MAT4")
                        throw Rejection("Invalid inverse bind accessor");
                }
            }

        std::string resources;
        for (const auto* key : {"textures", "samplers"})
            if (const auto* data = root.if_contains(key)) resources += boost::json::serialize(*data);
        if (const auto* images = root.if_contains("images"))
            for (const auto& image : images->as_array())
            {
                const auto& view = views[number(image.as_object(), "bufferView")].as_object();
                resources.append(bytes.data() + bin_start + number(view, "byteOffset"), number(view, "byteLength"));
            }
        const auto image_key = SLBlenderConnect::contentHash(resources);
        for (auto& key : snapshot.materialKeys) key = SLBlenderConnect::contentHash(key + image_key);
        // The normal GLTF Asset::validate/prep additionally checks accessor
        // ranges/types, indices, skins and the node forest before mesh decoding.
    }
}

std::string SLBlenderConnect::contentHash(std::string_view bytes)
{
    boost::uuids::detail::sha1 hash;
    hash.process_bytes(bytes.data(), bytes.size());
    boost::uuids::detail::sha1::digest_type digest;
    hash.get_digest(digest);
    std::string result;
    static constexpr char hex[] = "0123456789abcdef";
    for (auto byte : digest) { result += hex[byte >> 4]; result += hex[byte & 15]; }
    return result;
}

SLBlenderConnect::Pulse SLBlenderConnect::parsePulse(std::string_view bytes, int64_t ageMilliseconds)
{
    Pulse result;
    if (bytes.size() > 1024) return result;
    try
    {
        boost::json::parse_options options;
        options.max_depth = 8;
        const auto value = boost::json::parse(bytes, {}, options);
        const auto& obj = value.as_object();
        if (obj.at("protocol").as_string() != "slipstream-blender-connect" || obj.at("version").as_int64() != 1) return result;
        result.session = std::string(obj.at("session").as_string());
        if (result.session.size() != 32) return {};
        const auto revision = obj.at("revision").as_int64();
        if (revision < 1) return {};
        result.revision = static_cast<uint64_t>(revision);
        if (obj.at("state").as_string() == "stopped") { result.state = SenderState::Stopped; return result; }
        if (obj.at("state").as_string() != "active") return result;
        result.digest = std::string(obj.at("digest").as_string());
        if (result.digest.size() != 40) return {};
        if (ageMilliseconds >= -2000 && ageMilliseconds < 15000) result.state = SenderState::Active;
    }
    catch (const std::exception&) { return {}; }
    return result;
}

bool SLBlenderConnect::validate(std::string_view bytes, Document& document, std::string& reason)
{
    document = {};
    try { validateDocument(bytes, document); reason.clear(); return true; }
    catch (const Rejection& error) { reason = error.what(); }
    catch (const std::bad_alloc&) { reason = "Memory allocation failed"; }
    catch (const std::exception&) { reason = "Malformed GLB JSON or field type"; }
    document = {};
    return false;
}

bool SLBlenderConnect::load(std::string_view bytes, tinygltf::Model& model, Document& document, std::string& reason)
{
    if (!validate(bytes, document, reason)) return false;
    try
    {
        tinygltf::TinyGLTF loader;
        std::string errors, warnings;
        // URIs have already been rejected. No caller-controlled path/base directory.
        if (!loader.LoadBinaryFromMemory(&model, &errors, &warnings,
                reinterpret_cast<const unsigned char*>(bytes.data()), static_cast<unsigned int>(bytes.size())))
        {
            reason = "Embedded GLB image or buffer decode failed";
            return false;
        }
        return true;
    }
    catch (const std::bad_alloc&) { reason = "Memory allocation failed"; return false; }
    catch (const std::exception&) { reason = "Embedded GLB decoder rejected the update"; return false; }
}
