/**
 * @file asset.cpp
 * @brief LL GLTF Implementation
 *
 * $LicenseInfo:firstyear=2024&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2024, Linden Research, Inc.
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
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "../llviewerprecompiledheaders.h"

#include "asset.h"
#include "../llviewercontrol.h"
#include "../llworldclock.h"
#include "../llviewertexturelist.h"
#include "../lllocalbitmaps.h"
#include "buffer_util.h"
#include <boost/url.hpp>
#include "llimagejpeg.h"


using namespace LL::GLTF;
using namespace boost::json;


namespace LL
{
    namespace GLTF
    {
        static boost::unordered_set<std::string> ExtensionsSupported = {
            "KHR_materials_unlit",
            "KHR_texture_transform"
        };

        static boost::unordered_set<std::string> ExtensionsIgnored = {
            "KHR_materials_pbrSpecularGlossiness"
        };

        Material::AlphaMode gltf_alpha_mode_to_enum(const std::string& alpha_mode)
        {
            if (alpha_mode == "OPAQUE")
            {
                return Material::AlphaMode::OPAQUE;
            }
            else if (alpha_mode == "MASK")
            {
                return Material::AlphaMode::MASK;
            }
            else if (alpha_mode == "BLEND")
            {
                return Material::AlphaMode::BLEND;
            }
            else
            {
                return Material::AlphaMode::OPAQUE;
            }
        }

        std::string enum_to_gltf_alpha_mode(Material::AlphaMode alpha_mode)
        {
            switch (alpha_mode)
            {
            case Material::AlphaMode::OPAQUE:
                return "OPAQUE";
            case Material::AlphaMode::MASK:
                return "MASK";
            case Material::AlphaMode::BLEND:
                return "BLEND";
            default:
                return "OPAQUE";
            }
        }
    }
}

void Scene::updateTransforms(Asset& asset)
{
    mat4 identity = glm::identity<mat4>();

    for (auto& nodeIndex : mNodes)
    {
        Node& node = asset.mNodes[nodeIndex];
        node.updateTransforms(asset, identity);
    }
}

void Node::updateTransforms(Asset& asset, const mat4& parentMatrix)
{
    makeMatrixValid();
    mAssetMatrix = parentMatrix * mMatrix;

    mAssetMatrixInv = glm::inverse(mAssetMatrix);

    S32 my_index = (S32)(this - &asset.mNodes[0]);

    for (auto& childIndex : mChildren)
    {
        Node& child = asset.mNodes[childIndex];
        child.mParent = my_index;
        child.updateTransforms(asset, mAssetMatrix);
    }
}

void Asset::updateTransforms()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_GLTF;
    for (auto& scene : mScenes)
    {
        scene.updateTransforms(*this);
    }
}

void Node::makeMatrixValid()
{
    if (!mMatrixValid && mTRSValid)
    {
        mMatrix = glm::recompose(mScale, mRotation, mTranslation, vec3(0,0,0), vec4(0,0,0,1));
        mMatrixValid = true;
    }

    llassert(mMatrixValid);
}

void Node::makeTRSValid()
{
    if (!mTRSValid && mMatrixValid)
    {
        vec3 skew;
        vec4 perspective;
        glm::decompose(mMatrix, mScale, mRotation, mTranslation, skew, perspective);

        mTRSValid = true;
    }

    llassert(mTRSValid);
}

void Node::setRotation(const quat& q)
{
    makeTRSValid();
    mRotation = q;
    mMatrixValid = false;
}

void Node::setTranslation(const vec3& t)
{
    makeTRSValid();
    mTranslation = t;
    mMatrixValid = false;
}

void Node::setScale(const vec3& s)
{
    makeTRSValid();
    mScale = s;
    mMatrixValid = false;
}

void Node::serialize(object& dst) const
{
    write(mName, "name", dst);
    write(mMatrix, "matrix", dst, glm::identity<mat4>());
    write(mRotation, "rotation", dst, glm::identity<quat>());
    write(mTranslation, "translation", dst, glm::vec3(0.f, 0.f, 0.f));
    write(mScale, "scale", dst, vec3(1.f,1.f,1.f));
    write(mChildren, "children", dst);
    write(mMesh, "mesh", dst, INVALID_INDEX);
    write(mSkin, "skin", dst, INVALID_INDEX);
}

const Node& Node::operator=(const Value& src)
{
    copy(src, "name", mName);
    mMatrixValid = copy(src, "matrix", mMatrix);
    copy(src, "rotation", mRotation);
    copy(src, "translation", mTranslation);
    copy(src, "scale", mScale);
    copy(src, "children", mChildren);
    copy(src, "mesh", mMesh);
    copy(src, "skin", mSkin);

    if (!mMatrixValid)
    {
        mTRSValid = true;
    }

    return *this;
}

void Image::serialize(object& dst) const
{
    write(mUri, "uri", dst);
    write(mMimeType, "mimeType", dst);
    write(mBufferView, "bufferView", dst, INVALID_INDEX);
    write(mName, "name", dst);
    write(mWidth, "width", dst, -1);
    write(mHeight, "height", dst, -1);
    write(mComponent, "component", dst, -1);
    write(mBits, "bits", dst, -1);
    write(mPixelType, "pixelType", dst, -1);
}

const Image& Image::operator=(const Value& src)
{
    copy(src, "uri", mUri);
    copy(src, "mimeType", mMimeType);
    copy(src, "bufferView", mBufferView);
    copy(src, "name", mName);
    copy(src, "width", mWidth);
    copy(src, "height", mHeight);
    copy(src, "component", mComponent);
    copy(src, "bits", mBits);
    copy(src, "pixelType", mPixelType);

    return *this;
}

void Asset::update()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_GLTF;
    F32 dt = gFrameTimeSeconds - mLastUpdateTime;

    if (dt > 0.f)
    {
        mLastUpdateTime = gFrameTimeSeconds;
        if (mAnimations.size() > 0 && !LLWorldClock::isFrozen())   // world pause: hold the pose
        {
            static LLCachedControl<U32> anim_idx(gSavedSettings, "GLTFAnimationIndex", 0);
            static LLCachedControl<F32> anim_speed(gSavedSettings, "GLTFAnimationSpeed", 1.f);

            U32 idx = llclamp(anim_idx(), 0U, mAnimations.size() - 1);
            mAnimations[idx].update(*this, dt*anim_speed);
        }

        updateTransforms();

        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_GLTF("gltf - addTextureStats");

            for (auto& image : mImages)
            {
                if (image.mLoadIntoTexturePipe)
                {
                    if (image.mTexture.notNull())
                    { // HACK - force texture to be loaded full rez
                        // TODO: calculate actual vsize
                        image.mTexture->addTextureStats(2048.f * 2048.f);
                        image.mTexture->setBoostLevel(LLViewerTexture::BOOST_HIGH);
                    }
                }
            }
        }
    }
}

namespace
{
    // Limits for assets from the network. Generous for real content; they
    // exist so a small hostile file cannot demand unbounded work or memory.
    constexpr size_t GLTF_MAX_ELEMENTS = 1 << 16;          // per top-level array, and total primitives
    constexpr S64 GLTF_MAX_ACCESSOR_COUNT = 1 << 24;       // elements in one accessor
    constexpr S64 GLTF_MAX_BUFFER_BYTES = 256 * 1024 * 1024; // all buffers together
    constexpr S64 GLTF_MAX_DECODED_ELEMENTS = 1 << 25;     // accessor elements decoded, summed over every use
    constexpr S32 GLTF_MAX_NODE_DEPTH = 256;

    bool index_ok(S32 idx, size_t size)
    {
        return idx >= 0 && (size_t)idx < size;
    }

    bool opt_index_ok(S32 idx, size_t size)
    {
        return idx == INVALID_INDEX || index_ok(idx, size);
    }

    bool texture_ok(const TextureInfo& info, size_t texture_count)
    {
        return opt_index_ok(info.mIndex, texture_count);
    }
}

bool Asset::validate() const
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_GLTF;

    auto fail = [](const char* why)
    {
        LL_WARNS("GLTF") << "Rejecting GLTF asset: " << why << LL_ENDL;
        return false;
    };

    if (mScenes.size() > GLTF_MAX_ELEMENTS || mNodes.size() > GLTF_MAX_ELEMENTS
        || mMeshes.size() > GLTF_MAX_ELEMENTS || mMaterials.size() > GLTF_MAX_ELEMENTS
        || mBuffers.size() > GLTF_MAX_ELEMENTS || mBufferViews.size() > GLTF_MAX_ELEMENTS
        || mTextures.size() > GLTF_MAX_ELEMENTS || mSamplers.size() > GLTF_MAX_ELEMENTS
        || mImages.size() > GLTF_MAX_ELEMENTS || mAccessors.size() > GLTF_MAX_ELEMENTS
        || mAnimations.size() > GLTF_MAX_ELEMENTS || mSkins.size() > GLTF_MAX_ELEMENTS)
    {
        return fail("too many elements");
    }

    if (!opt_index_ok(mScene, mScenes.size()))
    {
        return fail("scene index out of range");
    }

    S64 total_buffer_bytes = 0;
    for (const Buffer& buffer : mBuffers)
    {
        if (buffer.mByteLength < 0)
        {
            return fail("negative buffer length");
        }
        total_buffer_bytes += buffer.mByteLength;
    }
    if (total_buffer_bytes > GLTF_MAX_BUFFER_BYTES)
    {
        return fail("buffers too large");
    }

    for (const BufferView& view : mBufferViews)
    {
        if (!index_ok(view.mBuffer, mBuffers.size()))
        {
            return fail("buffer view references a missing buffer");
        }
        if (view.mByteOffset < 0 || view.mByteLength < 0 || view.mByteStride < 0 || view.mByteStride > 255)
        {
            return fail("invalid buffer view range");
        }
        if ((S64)view.mByteOffset + (S64)view.mByteLength > (S64)mBuffers[view.mBuffer].mByteLength)
        {
            return fail("buffer view outside its buffer");
        }
    }

    for (const Accessor& accessor : mAccessors)
    {
        const S64 elem = accessor_component_size(accessor.mComponentType) * accessor_component_count(accessor.mType);
        if (elem <= 0)
        {
            return fail("invalid accessor component type");
        }
        if (accessor.mCount < 0 || accessor.mCount > GLTF_MAX_ACCESSOR_COUNT || accessor.mByteOffset < 0)
        {
            return fail("invalid accessor count or offset");
        }
        if (!opt_index_ok(accessor.mBufferView, mBufferViews.size()))
        {
            return fail("accessor references a missing buffer view");
        }
        if (accessor.mBufferView != INVALID_INDEX && accessor.mCount > 0)
        {
            const BufferView& view = mBufferViews[accessor.mBufferView];
            const S64 stride = view.mByteStride == 0 ? elem : (S64)view.mByteStride;
            const S64 end = (S64)accessor.mByteOffset + ((S64)accessor.mCount - 1) * stride + elem;
            if (end > (S64)view.mByteLength)
            {
                return fail("accessor data outside its buffer view");
            }
        }
    }

    for (const Image& image : mImages)
    {
        if (!opt_index_ok(image.mBufferView, mBufferViews.size()))
        {
            return fail("image references a missing buffer view");
        }
    }

    for (const Texture& texture : mTextures)
    {
        if (!opt_index_ok(texture.mSource, mImages.size()) || !opt_index_ok(texture.mSampler, mSamplers.size()))
        {
            return fail("texture references a missing image or sampler");
        }
    }

    for (const Material& material : mMaterials)
    {
        const size_t tc = mTextures.size();
        if (!texture_ok(material.mPbrMetallicRoughness.mBaseColorTexture, tc)
            || !texture_ok(material.mPbrMetallicRoughness.mMetallicRoughnessTexture, tc)
            || !texture_ok(material.mNormalTexture, tc)
            || !texture_ok(material.mOcclusionTexture, tc)
            || !texture_ok(material.mEmissiveTexture, tc))
        {
            return fail("material references a missing texture");
        }
    }

    S64 decoded = 0;
    size_t total_primitives = 0;
    for (const Mesh& mesh : mMeshes)
    {
        total_primitives += mesh.mPrimitives.size();
        if (total_primitives > GLTF_MAX_ELEMENTS)
        {
            return fail("too many primitives");
        }
        for (const Primitive& prim : mesh.mPrimitives)
        {
            if ((U32)prim.mMode > (U32)Primitive::Mode::TRIANGLE_FAN)
            {
                return fail("invalid primitive mode");
            }
            if (!opt_index_ok(prim.mMaterial, mMaterials.size()))
            {
                return fail("primitive references a missing material");
            }
            auto pos = prim.mAttributes.find("POSITION");
            if (pos == prim.mAttributes.end() || !index_ok(pos->second, mAccessors.size()))
            {
                return fail("primitive without positions");
            }
            const S32 vert_count = mAccessors[pos->second].mCount;
            if (vert_count < 1)
            {
                return fail("primitive without vertices");
            }
            for (const auto& attrib : prim.mAttributes)
            {
                if (!index_ok(attrib.second, mAccessors.size()))
                {
                    return fail("primitive attribute references a missing accessor");
                }
                const Accessor& accessor = mAccessors[attrib.second];
                if (accessor.mCount != vert_count)
                {
                    return fail("primitive attribute count differs from its vertex count");
                }
                decoded += accessor.mCount;
            }
            if (prim.mIndices != INVALID_INDEX)
            {
                if (!index_ok(prim.mIndices, mAccessors.size()))
                {
                    return fail("primitive references a missing index accessor");
                }
                const Accessor& accessor = mAccessors[prim.mIndices];
                if (accessor.mType != Accessor::Type::SCALAR
                    || (accessor.mComponentType != Accessor::ComponentType::UNSIGNED_BYTE
                        && accessor.mComponentType != Accessor::ComponentType::UNSIGNED_SHORT
                        && accessor.mComponentType != Accessor::ComponentType::UNSIGNED_INT))
                {
                    return fail("invalid index accessor type");
                }
                decoded += accessor.mCount;
            }
            else
            {
                decoded += vert_count;
            }
        }
    }

    for (const Skin& skin : mSkins)
    {
        if (!opt_index_ok(skin.mInverseBindMatrices, mAccessors.size()) || !opt_index_ok(skin.mSkeleton, mNodes.size()))
        {
            return fail("skin references a missing accessor or node");
        }
        for (S32 joint : skin.mJoints)
        {
            if (!index_ok(joint, mNodes.size()))
            {
                return fail("skin joint references a missing node");
            }
        }
        if (skin.mInverseBindMatrices != INVALID_INDEX)
        {
            decoded += mAccessors[skin.mInverseBindMatrices].mCount;
        }
    }

    for (const Animation& animation : mAnimations)
    {
        for (const Animation::Sampler& sampler : animation.mSamplers)
        {
            if (!index_ok(sampler.mInput, mAccessors.size()) || !index_ok(sampler.mOutput, mAccessors.size()))
            {
                return fail("animation sampler references a missing accessor");
            }
            decoded += mAccessors[sampler.mInput].mCount;
        }
        auto channels_ok = [&](const auto& channels) -> bool
        {
            for (const auto& channel : channels)
            {
                if (!index_ok(channel.mSampler, animation.mSamplers.size())
                    || !index_ok(channel.mTarget.mNode, mNodes.size()))
                {
                    return false;
                }
                decoded += mAccessors[animation.mSamplers[channel.mSampler].mOutput].mCount;
            }
            return true;
        };
        if (!channels_ok(animation.mRotationChannels) || !channels_ok(animation.mTranslationChannels)
            || !channels_ok(animation.mScaleChannels))
        {
            return fail("animation channel references a missing sampler or node");
        }
    }

    if (decoded > GLTF_MAX_DECODED_ELEMENTS)
    {
        return fail("too much geometry");
    }

    for (const Scene& scene : mScenes)
    {
        if (scene.mNodes.size() > GLTF_MAX_ELEMENTS)
        {
            return fail("too many scene nodes");
        }
        for (S32 node : scene.mNodes)
        {
            if (!index_ok(node, mNodes.size()))
            {
                return fail("scene references a missing node");
            }
        }
    }

    // The node hierarchy must be a forest: every child index valid, no node
    // with two parents, no cycles, bounded depth. Transform updates recurse
    // through it, so anything else would recurse without end or blow up.
    std::vector<U8> has_parent(mNodes.size(), 0);
    size_t total_children = 0;
    for (size_t i = 0; i < mNodes.size(); ++i)
    {
        const Node& node = mNodes[i];
        if (!opt_index_ok(node.mMesh, mMeshes.size()) || !opt_index_ok(node.mSkin, mSkins.size()))
        {
            return fail("node references a missing mesh or skin");
        }
        total_children += node.mChildren.size();
        if (total_children > GLTF_MAX_ELEMENTS)
        {
            return fail("too many node children");
        }
        for (S32 child : node.mChildren)
        {
            if (!index_ok(child, mNodes.size()) || (size_t)child == i)
            {
                return fail("invalid node child");
            }
            if (has_parent[child])
            {
                return fail("node with more than one parent");
            }
            has_parent[child] = 1;
        }
    }

    size_t visited = 0;
    std::vector<std::pair<S32, S32>> stack; // node, depth
    for (size_t i = 0; i < mNodes.size(); ++i)
    {
        if (has_parent[i])
        {
            continue;
        }
        stack.emplace_back((S32)i, 1);
        while (!stack.empty())
        {
            const auto [node, depth] = stack.back();
            stack.pop_back();
            ++visited;
            if (depth > GLTF_MAX_NODE_DEPTH)
            {
                return fail("node hierarchy too deep");
            }
            for (S32 child : mNodes[node].mChildren)
            {
                stack.emplace_back(child, depth + 1);
            }
        }
    }
    if (visited != mNodes.size())
    {
        // with at most one parent each, a node not reachable from a root is on a cycle
        return fail("node hierarchy has a cycle");
    }

    return true;
}

bool Asset::prep()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_GLTF;
    if (!validate())
    {
        return false;
    }

    // Unsupported conversions and out-of-range data in the copies below set
    // sCopyFailed instead of aborting; any of them fails the asset.
    sCopyFailed = false;

    // check required extensions
    for (auto& extension : mExtensionsRequired)
    {
        if (ExtensionsSupported.find(extension) == ExtensionsSupported.end())
        {
            if (ExtensionsIgnored.find(extension) == ExtensionsIgnored.end())
            {
                LL_WARNS() << "Unsupported extension: " << extension << LL_ENDL;
                mUnsupportedExtensions.push_back(extension);
            }
            else
            {
                mIgnoredExtensions.push_back(extension);
            }
        }
    }
    if (mUnsupportedExtensions.size() > 0)
    {
        return false;
    }

    // do buffers first as other resources depend on them
    for (auto& buffer : mBuffers)
    {
        if (!buffer.prep(*this))
        {
            return false;
        }
    }

    for (auto& image : mImages)
    {
        if (!image.prep(*this, mLoadIntoVRAM))
        {
            return false;
        }
    }

    for (auto& mesh : mMeshes)
    {
        if (!mesh.prep(*this))
        {
            return false;
        }
    }

    for (auto& animation : mAnimations)
    {
        if (!animation.prep(*this))
        {
            return false;
        }
    }

    for (auto& skin : mSkins)
    {
        if (!skin.prep(*this))
        {
            return false;
        }
    }

    if (sCopyFailed)
    {
        LL_WARNS("GLTF") << "GLTF asset data could not be decoded" << LL_ENDL;
        return false;
    }
    return true;
}

Asset::Asset(const Value& src)
{
    *this = src;
}

bool Asset::load(std::string_view filename, bool loadIntoVRAM)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_GLTF;
    mLoadIntoVRAM = loadIntoVRAM;
    mFilename = filename;
    std::string ext = gDirUtilp->getExtension(mFilename);

    llifstream file(filename.data(), std::ios::binary);
    if (file.is_open())
    {
        std::string str((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        file.close();

        if (ext == "gltf")
        {
            try
            {
                Value val = parse(str);
                *this = val;
                return prep();
            }
            catch (const std::exception& e)
            {
                LL_WARNS("GLTF") << "Failed to load " << filename << ": " << e.what() << LL_ENDL;
                return false;
            }
        }
        else if (ext == "glb")
        {
            return loadBinary(str, mLoadIntoVRAM);
        }
        else
        {
            LL_WARNS() << "Unsupported file type: " << ext << LL_ENDL;
            return false;
        }
    }
    else
    {
        LL_WARNS() << "Failed to open file: " << filename << LL_ENDL;
        return false;
    }

    return false;
}

bool Asset::loadBinary(const std::string& data, bool loadIntoVRAM)
{
    try
    {
        return loadBinaryChunks(data, loadIntoVRAM);
    }
    catch (const std::exception& e)
    {
        LL_WARNS("GLTF") << "Failed to load GLB: " << e.what() << LL_ENDL;
        return false;
    }
}

bool Asset::loadBinaryChunks(const std::string& data, bool loadIntoVRAM)
{
    mLoadIntoVRAM = loadIntoVRAM;
    // load from binary gltf
    const U8* ptr = (const U8*)data.data();
    const U8* end = ptr + data.size();

    if (end - ptr < 12)
    {
        LL_WARNS("GLTF") << "GLB file too short" << LL_ENDL;
        return false;
    }

    U32 magic = *(U32*)ptr;
    ptr += 4;

    if (magic != 0x46546C67)
    {
        LL_WARNS("GLTF") << "Invalid GLB magic" << LL_ENDL;
        return false;
    }

    U32 version = *(U32*)ptr;
    ptr += 4;

    if (version != 2)
    {
        LL_WARNS("GLTF") << "Unsupported GLB version" << LL_ENDL;
        return false;
    }

    U32 length = *(U32*)ptr;
    ptr += 4;

    if (length != data.size())
    {
        LL_WARNS("GLTF") << "GLB length mismatch" << LL_ENDL;
        return false;
    }

    if (end - ptr < 8)
    {
        LL_WARNS("GLTF") << "GLB chunk too short" << LL_ENDL;
        return false;
    }

    U32 chunkLength = *(U32*)ptr;
    ptr += 4;

    // 64-bit compare: chunkLength + 8 must not wrap
    if ((U64)(end - ptr) < (U64)chunkLength + 8)
    {
        LL_WARNS("GLTF") << "GLB chunk too short" << LL_ENDL;
        return false;
    }

    U32 chunkType = *(U32*)ptr;
    ptr += 4;

    if (chunkType != 0x4E4F534A)
    {
        LL_WARNS("GLTF") << "Invalid GLB chunk type" << LL_ENDL;
        return false;
    }

    Value val = parse(std::string_view((const char*)ptr, chunkLength));
    *this = val;

    if (mBuffers.size() > 0 && mBuffers[0].mUri.empty())
    {
        // load binary chunk
        ptr += chunkLength;

        if (end - ptr < 8)
        {
            LL_WARNS("GLTF") << "GLB chunk too short" << LL_ENDL;
            return false;
        }

        chunkLength = *(U32*)ptr;
        ptr += 4;

        chunkType = *(U32*)ptr;
        ptr += 4;

        if (chunkType != 0x004E4942)
        {
            LL_WARNS("GLTF") << "Invalid GLB chunk type" << LL_ENDL;
            return false;
        }

        auto& buffer = mBuffers[0];

        if (buffer.mByteLength >= 0
            && (U64)chunkLength <= (U64)(end - ptr)
            && (U64)buffer.mByteLength <= (U64)chunkLength)
        {
            buffer.mData.resize(buffer.mByteLength);
            memcpy(buffer.mData.data(), ptr, buffer.mByteLength);
            ptr += buffer.mByteLength;
        }
        else
        {
            LL_WARNS("GLTF") << "Buffer too short" << LL_ENDL;
            return false;
        }
    }

    return prep();
}

const Asset& Asset::operator=(const Value& src)
{
    if (src.is_object())
    {
        const object& obj = src.as_object();

        const auto it = obj.find("asset");

        if (it != obj.end())
        {
            const Value& asset = it->value();

            copy(asset, "version", mVersion);
            copy(asset, "minVersion", mMinVersion);
            copy(asset, "generator", mGenerator);
            copy(asset, "copyright", mCopyright);
            copy(asset, "extras", mExtras);
        }

        copy(obj, "scene", mScene);
        copy(obj, "scenes", mScenes);
        copy(obj, "nodes", mNodes);
        copy(obj, "meshes", mMeshes);
        copy(obj, "materials", mMaterials);
        copy(obj, "buffers", mBuffers);
        copy(obj, "bufferViews", mBufferViews);
        copy(obj, "textures", mTextures);
        copy(obj, "samplers", mSamplers);
        copy(obj, "images", mImages);
        copy(obj, "accessors", mAccessors);
        copy(obj, "animations", mAnimations);
        copy(obj, "skins", mSkins);
        copy(obj, "extensionsUsed", mExtensionsUsed);
        copy(obj, "extensionsRequired", mExtensionsRequired);
    }

    return *this;
}

void Asset::serialize(object& dst) const
{
    static const std::string sGenerator = "Linden Lab GLTF Prototype v0.1";

    dst["asset"] = object{};
    object& asset = dst["asset"].get_object();

    write(mVersion, "version", asset);
    write(mMinVersion, "minVersion", asset, std::string());
    write(sGenerator, "generator", asset);
    write(mScene, "scene", dst, INVALID_INDEX);
    write(mScenes, "scenes", dst);
    write(mNodes, "nodes", dst);
    write(mMeshes, "meshes", dst);
    write(mMaterials, "materials", dst);
    write(mBuffers, "buffers", dst);
    write(mBufferViews, "bufferViews", dst);
    write(mTextures, "textures", dst);
    write(mSamplers, "samplers", dst);
    write(mImages, "images", dst);
    write(mAccessors, "accessors", dst);
    write(mAnimations, "animations", dst);
    write(mSkins, "skins", dst);
    write(mExtensionsUsed, "extensionsUsed", dst);
    write(mExtensionsRequired, "extensionsRequired", dst);
}

bool Asset::save(const std::string& filename)
{
    // get folder path
    std::string folder = gDirUtilp->getDirName(filename);

    // save images
    for (auto& image : mImages)
    {
        if (!image.save(*this, folder))
        {
            return false;
        }
    }

    // save buffers
    // NOTE: save buffers after saving images as saving images
    // may remove image data from buffers
    for (auto& buffer : mBuffers)
    {
        if (!buffer.save(*this, folder))
        {
            return false;
        }
    }

    // save .gltf
    object obj;
    serialize(obj);
    std::string buffer = boost::json::serialize(obj, {});
    std::ofstream file(filename, std::ios::binary);
    file.write(buffer.c_str(), buffer.size());

    return true;
}

void Asset::eraseBufferView(S32 bufferView)
{
    mBufferViews.erase(mBufferViews.begin() + bufferView);

    for (auto& accessor : mAccessors)
    {
        if (accessor.mBufferView > bufferView)
        {
            accessor.mBufferView--;
        }
    }

    for (auto& image : mImages)
    {
        if (image.mBufferView > bufferView)
        {
            image.mBufferView--;
        }
    }

}

LLViewerFetchedTexture* fetch_texture(const LLUUID& id);

bool Image::prep(Asset& asset, bool loadIntoVRAM)
{
    mLoadIntoTexturePipe = loadIntoVRAM;
    LLUUID id;
    if (mUri.size() == UUID_STR_SIZE && LLUUID::parseUUID(mUri, &id) && id.notNull())
    { // loaded from an asset, fetch the texture from the asset system
        mTexture = fetch_texture(id);
    }
    else if (mUri.find("data:") == 0)
    { // embedded in a data URI, load the texture from the URI
        LL_WARNS() << "Data URIs not yet supported" << LL_ENDL;
        return false;
    }
    else if (mBufferView != INVALID_INDEX)
    { // embedded in a buffer, load the texture from the buffer
        BufferView& bufferView = asset.mBufferViews[mBufferView];
        Buffer& buffer = asset.mBuffers[bufferView.mBuffer];
        if (mLoadIntoTexturePipe)
        {
            U8* data = buffer.mData.data() + bufferView.mByteOffset;
            mTexture = LLViewerTextureManager::getFetchedTextureFromMemory(data, bufferView.mByteLength, mMimeType);
        }
        else if (mTexture.isNull() && mLoadIntoTexturePipe)
        {
            LL_WARNS("GLTF") << "Failed to load image from buffer:" << LL_ENDL;
            LL_WARNS("GLTF") << "  image: " << mName << LL_ENDL;
            LL_WARNS("GLTF") << "  mimeType: " << mMimeType << LL_ENDL;

            return false;
        }
    }
    else if (!asset.mFilename.empty() && !mUri.empty())
    { // loaded locally and not embedded, load the texture as a local preview
        std::string dir = gDirUtilp->getDirName(asset.mFilename);
        std::string img_file = dir + gDirUtilp->getDirDelimiter() + mUri;

        LLUUID tracking_id = LLLocalBitmapMgr::getInstance()->addUnit(img_file);
        if (tracking_id.notNull() && mLoadIntoTexturePipe)
        {
            LLUUID world_id = LLLocalBitmapMgr::getInstance()->getWorldID(tracking_id);
            mTexture = LLViewerTextureManager::getFetchedTexture(world_id);
        }
        else if (mLoadIntoTexturePipe)
        {
            LL_WARNS("GLTF") << "Failed to load image from file:" << LL_ENDL;
            LL_WARNS("GLTF") << "  image: " << mName << LL_ENDL;
            LL_WARNS("GLTF") << "  file: " << img_file << LL_ENDL;

            return false;
        }
    }
    else
    {
        LL_WARNS("GLTF") << "Failed to load image: " << mName << LL_ENDL;
        return false;
    }

    if (!asset.mFilename.empty() && mLoadIntoTexturePipe)
    { // local preview, boost image so it doesn't discard and force to save raw image in case we save out or upload
        mTexture->setBoostLevel(LLViewerTexture::BOOST_PREVIEW);
        mTexture->forceToSaveRawImage(0, F32_MAX);
    }

    return true;
}


void Image::clearData(Asset& asset)
{
    if (mBufferView != INVALID_INDEX)
    {
        // remove data from buffer
        BufferView& bufferView = asset.mBufferViews[mBufferView];
        Buffer& buffer = asset.mBuffers[bufferView.mBuffer];

        buffer.erase(asset, bufferView.mByteOffset, bufferView.mByteLength);

        asset.eraseBufferView(mBufferView);
    }

    mBufferView = INVALID_INDEX;
    mWidth = -1;
    mHeight = -1;
    mComponent = -1;
    mBits = -1;
    mPixelType = -1;
    mMimeType = "";
}

bool Image::save(Asset& asset, const std::string& folder)
{
    // NOTE:  this *MUST* be a lossless save
    // Artists use this to save their work repeatedly, so
    // adding any compression artifacts here will degrade
    // images over time.
    std::string name = mName;
    std::string error;
    const std::string& delim = gDirUtilp->getDirDelimiter();
    if (name.empty())
    {
        S32 idx = (S32)(this - asset.mImages.data());
        name = llformat("image_%d", idx);
    }

    if (mBufferView != INVALID_INDEX)
    {
        // we have the bytes of the original image, save that out in its
        // original format
        BufferView& bufferView = asset.mBufferViews[mBufferView];
        Buffer& buffer = asset.mBuffers[bufferView.mBuffer];

        std::string extension;

        if (mMimeType == "image/jpeg")
        {
            extension = ".jpg";
        }
        else if (mMimeType == "image/png")
        {
            extension = ".png";
        }
        else
        {
            error = "Unknown mime type, saved as .bin";
            extension = ".bin";
        }

        std::string filename = folder + delim + name + extension;

        // set URI to non-j2c file for now, but later we'll want to reference the j2c hash
        mUri = name + extension;

        std::ofstream file(filename, std::ios::binary);
        file.write((const char*)buffer.mData.data() + bufferView.mByteOffset, bufferView.mByteLength);
    }
    else if (mTexture.notNull())
    {
        auto bitmapmgr = LLLocalBitmapMgr::getInstance();
        if (bitmapmgr->isLocal(mTexture->getID()))
        {
            LLUUID tracking_id = bitmapmgr->getTrackingID(mTexture->getID());
            if (tracking_id.notNull())
            { // copy original file to destination folder
                std::string source = bitmapmgr->getFilename(tracking_id);
                if (gDirUtilp->fileExists(source))
                {
                    std::string filename = gDirUtilp->getBaseFileName(source);
                    std::string dest = folder + delim + filename;

                    LLFile::copy(source, dest);
                    mUri = filename;
                }
                else
                {
                    error = "File not found: " + source;
                }
            }
            else
            {
                error = "Local image missing.";
            }
        }
        else if (!mUri.empty())
        {
            std::string from_dir = gDirUtilp->getDirName(asset.mFilename);
            std::string base_filename = gDirUtilp->getBaseFileName(mUri);
            std::string filename = from_dir + delim + base_filename;
            if (gDirUtilp->fileExists(filename))
            {
                std::string dest = folder + delim + base_filename;
                LLFile::copy(filename, dest);
                mUri = base_filename;
            }
            else
            {
                error = "Original image file not found: " + filename;
            }
        }
        else
        {
            error = "Image is not a local image and has no uri, cannot save.";
        }
    }

    if (!error.empty())
    {
        LL_WARNS("GLTF") << "Failed to save " << name << ": " << error << LL_ENDL;
        return false;
    }

    clearData(asset);

    return true;
}

void TextureInfo::serialize(object& dst) const
{
    write(mIndex, "index", dst, INVALID_INDEX);
    write(mTexCoord, "texCoord", dst, 0);
    write_extensions(dst, &mTextureTransform, "KHR_texture_transform");
}

S32 TextureInfo::getTexCoord() const
{
    if (mTextureTransform.mPresent && mTextureTransform.mTexCoord != INVALID_INDEX)
    {
        return mTextureTransform.mTexCoord;
    }
    return mTexCoord;
}

bool Material::isMultiUV() const
{
    return mPbrMetallicRoughness.mBaseColorTexture.getTexCoord() != 0 ||
        mPbrMetallicRoughness.mMetallicRoughnessTexture.getTexCoord() != 0 ||
        mNormalTexture.getTexCoord() != 0 ||
        mOcclusionTexture.getTexCoord() != 0 ||
        mEmissiveTexture.getTexCoord() != 0;
}

const TextureInfo& TextureInfo::operator=(const Value& src)
{
    if (src.is_object())
    {
        copy(src, "index", mIndex);
        copy(src, "texCoord", mTexCoord);
        copy_extensions(src, "KHR_texture_transform", &mTextureTransform);
    }

    return *this;
}

bool TextureInfo::operator==(const TextureInfo& rhs) const
{
    return mIndex == rhs.mIndex && mTexCoord == rhs.mTexCoord;
}

bool TextureInfo::operator!=(const TextureInfo& rhs) const
{
    return !(*this == rhs);
}

void OcclusionTextureInfo::serialize(object& dst) const
{
    TextureInfo::serialize(dst);
    write(mStrength, "strength", dst, 1.f);
}

const OcclusionTextureInfo& OcclusionTextureInfo::operator=(const Value& src)
{
    TextureInfo::operator=(src);

    if (src.is_object())
    {
        copy(src, "strength", mStrength);
    }

    return *this;
}

void NormalTextureInfo::serialize(object& dst) const
{
    TextureInfo::serialize(dst);
    write(mScale, "scale", dst, 1.f);
}

const NormalTextureInfo& NormalTextureInfo::operator=(const Value& src)
{
    TextureInfo::operator=(src);
    if (src.is_object())
    {
        copy(src, "index", mIndex);
        copy(src, "texCoord", mTexCoord);
        copy(src, "scale", mScale);
    }

    return *this;
}

const Material::PbrMetallicRoughness& Material::PbrMetallicRoughness::operator=(const Value& src)
{
    if (src.is_object())
    {
        copy(src, "baseColorFactor", mBaseColorFactor);
        copy(src, "baseColorTexture", mBaseColorTexture);
        copy(src, "metallicFactor", mMetallicFactor);
        copy(src, "roughnessFactor", mRoughnessFactor);
        copy(src, "metallicRoughnessTexture", mMetallicRoughnessTexture);
    }

    return *this;
}

void Material::PbrMetallicRoughness::serialize(object& dst) const
{
    write(mBaseColorFactor, "baseColorFactor", dst, vec4(1.f, 1.f, 1.f, 1.f));
    write(mBaseColorTexture, "baseColorTexture", dst);
    write(mMetallicFactor, "metallicFactor", dst, 1.f);
    write(mRoughnessFactor, "roughnessFactor", dst, 1.f);
    write(mMetallicRoughnessTexture, "metallicRoughnessTexture", dst);
}

bool Material::PbrMetallicRoughness::operator==(const Material::PbrMetallicRoughness& rhs) const
{
    return mBaseColorFactor == rhs.mBaseColorFactor &&
        mBaseColorTexture == rhs.mBaseColorTexture &&
        mMetallicFactor == rhs.mMetallicFactor &&
        mRoughnessFactor == rhs.mRoughnessFactor &&
        mMetallicRoughnessTexture == rhs.mMetallicRoughnessTexture;
}

bool Material::PbrMetallicRoughness::operator!=(const Material::PbrMetallicRoughness& rhs) const
{
    return !(*this == rhs);
}

const Material::Unlit& Material::Unlit::operator=(const Value& src)
{
    mPresent = true;
    return *this;
}

void Material::Unlit::serialize(object& dst) const
{
    // no members and object has already been created, nothing to do
}

void TextureTransform::getPacked(vec4* packed) const
{
    packed[0] = vec4(mScale.x, mScale.y, mRotation, mOffset.x);
    packed[1] = vec4(mOffset.y, 0.f, 0.f, 0.f);
}

const TextureTransform& TextureTransform::operator=(const Value& src)
{
    mPresent = true;
    if (src.is_object())
    {
        copy(src, "offset", mOffset);
        copy(src, "rotation", mRotation);
        copy(src, "scale", mScale);
        copy(src, "texCoord", mTexCoord);
    }

    return *this;
}

void TextureTransform::serialize(object& dst) const
{
    write(mOffset, "offset", dst, vec2(0.f, 0.f));
    write(mRotation, "rotation", dst, 0.f);
    write(mScale, "scale", dst, vec2(1.f, 1.f));
    write(mTexCoord, "texCoord", dst, -1);
}


void Material::serialize(object& dst) const
{
    write(mName, "name", dst);
    write(mEmissiveFactor, "emissiveFactor", dst, vec3(0.f, 0.f, 0.f));
    write(mPbrMetallicRoughness, "pbrMetallicRoughness", dst);
    write(mNormalTexture, "normalTexture", dst);
    write(mOcclusionTexture, "occlusionTexture", dst);
    write(mEmissiveTexture, "emissiveTexture", dst);
    write(mAlphaMode, "alphaMode", dst, Material::AlphaMode::OPAQUE);
    write(mAlphaCutoff, "alphaCutoff", dst, 0.5f);
    write(mDoubleSided, "doubleSided", dst, false);
    write_extensions(dst, &mUnlit, "KHR_materials_unlit");
}

const Material& Material::operator=(const Value& src)
{
    if (src.is_object())
    {
        copy(src, "name", mName);
        copy(src, "emissiveFactor", mEmissiveFactor);
        copy(src, "pbrMetallicRoughness", mPbrMetallicRoughness);
        copy(src, "normalTexture", mNormalTexture);
        copy(src, "occlusionTexture", mOcclusionTexture);
        copy(src, "emissiveTexture", mEmissiveTexture);
        copy(src, "alphaMode", mAlphaMode);
        copy(src, "alphaCutoff", mAlphaCutoff);
        copy(src, "doubleSided", mDoubleSided);
        copy_extensions(src,
            "KHR_materials_unlit", &mUnlit );
    }
    return *this;
}


void Mesh::serialize(object& dst) const
{
    write(mPrimitives, "primitives", dst);
    write(mWeights, "weights", dst);
    write(mName, "name", dst);
}

const Mesh& Mesh::operator=(const Value& src)
{
    if (src.is_object())
    {
        copy(src, "primitives", mPrimitives);
        copy(src, "weights", mWeights);
        copy(src, "name", mName);
    }

    return *this;
}

bool Mesh::prep(Asset& asset)
{
    for (auto& primitive : mPrimitives)
    {
        if (!primitive.prep(asset))
        {
            return false;
        }
    }

    return true;
}

void Scene::serialize(object& dst) const
{
    write(mNodes, "nodes", dst);
    write(mName, "name", dst);
}

const Scene& Scene::operator=(const Value& src)
{
    copy(src, "nodes", mNodes);
    copy(src, "name", mName);

    return *this;
}

void Texture::serialize(object& dst) const
{
    write(mSampler, "sampler", dst, INVALID_INDEX);
    write(mSource, "source", dst, INVALID_INDEX);
    write(mName, "name", dst);
}

const Texture& Texture::operator=(const Value& src)
{
    if (src.is_object())
    {
        copy(src, "sampler", mSampler);
        copy(src, "source", mSource);
        copy(src, "name", mName);
    }

    return *this;
}

void Sampler::serialize(object& dst) const
{
    write(mMagFilter, "magFilter", dst, LINEAR);
    write(mMinFilter, "minFilter", dst, LINEAR_MIPMAP_LINEAR);
    write(mWrapS, "wrapS", dst, REPEAT);
    write(mWrapT, "wrapT", dst, REPEAT);
    write(mName, "name", dst);
}

const Sampler& Sampler::operator=(const Value& src)
{
    copy(src, "magFilter", mMagFilter);
    copy(src, "minFilter", mMinFilter);
    copy(src, "wrapS", mWrapS);
    copy(src, "wrapT", mWrapT);
    copy(src, "name", mName);

    return *this;
}


