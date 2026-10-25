/**
 * @file primitive.cpp
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
#include "buffer_util.h"

#include "mikktspace/mikktspace.hh"

#include "meshoptimizer.h"


using namespace LL::GLTF;
using namespace boost::json;


// Mesh data useful for Mikktspace tangent generation (and flat normal generation)
struct MikktMesh
{
    std::vector<LLVector3> p;       //positions
    std::vector<LLVector3> n;       //normals
    std::vector<LLVector4> t;       //tangents
    std::vector<LLVector2> tc0;     //texcoords 0
    std::vector<LLVector2> tc1;     //texcoords 1
    std::vector<LLColor4U> c;       //colors
    std::vector<LLVector4> w;       //weights
    std::vector<U64> j;             //joints

    // initialize from src primitive and make an unrolled triangle list
    // returns false if the Primitive cannot be converted to a triangle list
    bool copy(const Primitive* prim)
    {
        bool indexed = !prim->mIndexArray.empty();
        size_t vert_count = indexed ? prim->mIndexArray.size() : prim->mPositions.size();

        size_t triangle_count = 0;

        if (prim->mMode == Primitive::Mode::TRIANGLE_STRIP ||
            prim->mMode == Primitive::Mode::TRIANGLE_FAN)
        {
            triangle_count = vert_count >= 3 ? vert_count - 2 : 0;
        }
        else if (prim->mMode == Primitive::Mode::TRIANGLES)
        {
            triangle_count = vert_count / 3;
        }
        else
        {
            LL_WARNS("GLTF") << "Unsupported primitive mode for conversion to triangles: " << (S32)prim->mMode << LL_ENDL;
            return false;
        }

        if (triangle_count == 0)
        {
            LL_WARNS("GLTF") << "Primitive has no triangles" << LL_ENDL;
            return false;
        }

        // every per-vertex array must cover the indices used below
        const size_t nv = prim->mPositions.size();
        if (prim->mTexCoords0.size() < nv || prim->mColors.size() < nv
            || (!prim->mNormals.empty() && prim->mNormals.size() < nv)
            || (!prim->mTexCoords1.empty() && prim->mTexCoords1.size() < nv)
            || (!prim->mWeights.empty() && (prim->mWeights.size() < nv || prim->mJoints.size() < nv)))
        {
            LL_WARNS("GLTF") << "Primitive vertex attributes are shorter than its positions" << LL_ENDL;
            return false;
        }

        vert_count = triangle_count * 3;
        llassert(vert_count <= size_t(U32_MAX));  // triangle_count will also naturally be under the limit

        p.resize(vert_count);
        n.resize(vert_count);
        tc0.resize(vert_count);
        c.resize(vert_count);

        bool has_normals = !prim->mNormals.empty();
        if (has_normals)
        {
            n.resize(vert_count);
        }
        bool has_tangents = !prim->mTangents.empty();
        if (has_tangents)
        {
            t.resize(vert_count);
        }

        bool rigged = !prim->mWeights.empty();
        if (rigged)
        {
            w.resize(vert_count);
            j.resize(vert_count);
        }

        bool multi_uv = !prim->mTexCoords1.empty();
        if (multi_uv)
        {
            tc1.resize(vert_count);
        }

        for (U32 tri_idx = 0; tri_idx < U32(triangle_count); ++tri_idx)
        {
            U32 idx[3] = {0, 0, 0};

            if (prim->mMode == Primitive::Mode::TRIANGLES)
            {
                idx[0] = tri_idx * 3;
                idx[1] = tri_idx * 3 + 1;
                idx[2] = tri_idx * 3 + 2;
            }
            else if (prim->mMode == Primitive::Mode::TRIANGLE_STRIP)
            {
                idx[0] = tri_idx;
                idx[1] = tri_idx + 1;
                idx[2] = tri_idx + 2;

                if (tri_idx % 2 != 0)
                {
                    std::swap(idx[1], idx[2]);
                }
            }
            else if (prim->mMode == Primitive::Mode::TRIANGLE_FAN)
            {
                idx[0] = 0;
                idx[1] = tri_idx + 1;
                idx[2] = tri_idx + 2;
            }

            if (indexed)
            {
                idx[0] = prim->mIndexArray[idx[0]];
                idx[1] = prim->mIndexArray[idx[1]];
                idx[2] = prim->mIndexArray[idx[2]];
            }

            if (idx[0] >= nv || idx[1] >= nv || idx[2] >= nv)
            {
                LL_WARNS("GLTF") << "Primitive index out of range" << LL_ENDL;
                return false;
            }

            for (U32 v = 0; v < 3; ++v)
            {
                U32 i = tri_idx * 3 + v;
                p[i].set(prim->mPositions[idx[v]].getF32ptr());
                tc0[i].set(prim->mTexCoords0[idx[v]]);
                c[i] = prim->mColors[idx[v]];

                if (multi_uv)
                {
                    tc1[i].set(prim->mTexCoords1[idx[v]]);
                }

                if (has_normals)
                {
                    n[i].set(prim->mNormals[idx[v]].getF32ptr());
                }

                if (rigged)
                {
                    w[i].set(prim->mWeights[idx[v]].getF32ptr());
                    j[i] = prim->mJoints[idx[v]];
                }
            }
        }

        return true;
    }

    void genNormals()
    {
        size_t tri_count = p.size() / 3;
        for (size_t i = 0; i < tri_count; ++i)
        {
            LLVector3 v0 = p[i * 3];
            LLVector3 v1 = p[i * 3 + 1];
            LLVector3 v2 = p[i * 3 + 2];

            LLVector3 normal = (v1 - v0) % (v2 - v0);
            normal.normalize();

            n[i * 3] = normal;
            n[i * 3 + 1] = normal;
            n[i * 3 + 2] = normal;
        }
    }

    void genTangents()
    {
        t.resize(p.size());
        mikk::Mikktspace ctx(*this);
        ctx.genTangSpace();
    }

    // write to target primitive as an indexed triangle list
    // Only modifies runtime data, does not modify the original GLTF data
    void write(Primitive* prim) const
    {
        //re-weld
        std::vector<meshopt_Stream> mos =
        {
            { &p[0], sizeof(LLVector3), sizeof(LLVector3) },
            { &n[0], sizeof(LLVector3), sizeof(LLVector3) },
            { &t[0], sizeof(LLVector4), sizeof(LLVector4) },
            { &tc0[0], sizeof(LLVector2), sizeof(LLVector2) },
            { &c[0], sizeof(LLColor4U), sizeof(LLColor4U) }
        };

        if (!w.empty())
        {
            mos.push_back({ &w[0], sizeof(LLVector4), sizeof(LLVector4) });
            mos.push_back({ &j[0], sizeof(U64), sizeof(U64) });
        }

        if (!tc1.empty())
        {
            mos.push_back({ &tc1[0], sizeof(LLVector2), sizeof(LLVector2) });
        }

        std::vector<U32> remap;
        remap.resize(p.size());

        size_t stream_count = mos.size();

        size_t vert_count = meshopt_generateVertexRemapMulti(&remap[0], nullptr, p.size(), p.size(), mos.data(), stream_count);

        prim->mTexCoords0.resize(vert_count);
        prim->mNormals.resize(vert_count);
        prim->mTangents.resize(vert_count);
        prim->mPositions.resize(vert_count);
        prim->mColors.resize(vert_count);
        if (!w.empty())
        {
            prim->mWeights.resize(vert_count);
            prim->mJoints.resize(vert_count);
        }
        if (!tc1.empty())
                    {
            prim->mTexCoords1.resize(vert_count);
        }

        prim->mIndexArray.resize(remap.size());

        for (int i = 0; i < remap.size(); ++i)
        {
            U32 src_idx = i;
            U32 dst_idx = remap[i];

            prim->mIndexArray[i] = dst_idx;

            prim->mPositions[dst_idx].load3(p[src_idx].mV);
            prim->mNormals[dst_idx].load3(n[src_idx].mV);
            prim->mTexCoords0[dst_idx] = tc0[src_idx];
            prim->mTangents[dst_idx].loadua(t[src_idx].mV);
            prim->mColors[dst_idx] = c[src_idx];

            if (!w.empty())
            {
                prim->mWeights[dst_idx].loadua(w[src_idx].mV);
                prim->mJoints[dst_idx] = j[src_idx];
            }

            if (!tc1.empty())
            {
                prim->mTexCoords1[dst_idx] = tc1[src_idx];
            }
        }

        prim->mArrayMode = Primitive::Mode::TRIANGLES;
    }

    uint32_t GetNumFaces()
    {
        return uint32_t(p.size()/3);
    }

    uint32_t GetNumVerticesOfFace(const uint32_t face_num)
    {
        return 3;
    }

    mikk::float3 GetPosition(const uint32_t face_num, const uint32_t vert_num)
    {
        F32* v = p[face_num * 3 + vert_num].mV;
        return mikk::float3(v);
    }

    mikk::float3 GetTexCoord(const uint32_t face_num, const uint32_t vert_num)
    {
        F32* uv = tc0[face_num * 3 + vert_num].mV;
        return mikk::float3(uv[0], 1.f-uv[1], 1.0f);
    }

    mikk::float3 GetNormal(const uint32_t face_num, const uint32_t vert_num)
    {
        F32* normal = n[face_num * 3 + vert_num].mV;
        return mikk::float3(normal);
    }

    void SetTangentSpace(const uint32_t face_num, const uint32_t vert_num, mikk::float3 T, bool orientation)
    {
        S32 i = face_num * 3 + vert_num;
        t[i].set(T.x, T.y, T.z, orientation ? 1.0f : -1.0f);
    }
};


bool Primitive::prep(Asset& asset)
{
    // Unpack the buffer views into per-attribute CPU arrays (the clean ingest captures
    // them into mesh slots, picking walks the octree built from them).

    // load vertex data
    for (auto& it : mAttributes)
    {
        const std::string& attribName = it.first;
        Accessor& accessor = asset.mAccessors[it.second];

        // load vertex data
        if (attribName == "POSITION")
        {
            copy(asset, accessor, mPositions);
        }
        else if (attribName == "NORMAL")
        {
            copy(asset, accessor, mNormals);
        }
        else if (attribName == "TANGENT")
        {
            copy(asset, accessor, mTangents);
        }
        else if (attribName == "COLOR_0")
        {
            copy(asset, accessor, mColors);
        }
        else if (attribName == "TEXCOORD_0")
        {
            copy(asset, accessor, mTexCoords0);
        }
        else if (attribName == "TEXCOORD_1")
        {
            copy(asset, accessor, mTexCoords1);
        }
        else if (attribName == "JOINTS_0")
        {
            copy(asset, accessor, mJoints);
        }
        else if (attribName == "WEIGHTS_0")
        {
            copy(asset, accessor, mWeights);
        }
    }

    if (sCopyFailed)
    {
        return false;
    }

    // copy index buffer
    if (mIndices != INVALID_INDEX)
    {
        Accessor& accessor = asset.mAccessors[mIndices];
        copy(asset, accessor, mIndexArray);
        if (sCopyFailed)
        {
            return false;
        }

        for (auto& idx : mIndexArray)
        {
            if (idx >= mPositions.size())
            {
                LL_WARNS("GLTF") << "Invalid index array" << LL_ENDL;
                return false;
            }
        }
    }
    else
    { //everything must be indexed at runtime
        mIndexArray.resize(mPositions.size());
        for (U32 i = 0; i < mPositions.size(); ++i)
        {
            mIndexArray[i] = i;
        }
    }

    if (mTexCoords0.empty())
    {
        mTexCoords0.resize(mPositions.size());
    }

    if (mColors.empty())
    {
        mColors.resize(mPositions.size(), LLColor4U::white);
    }

    bool unlit = false;

    // bake material basecolor into color array
    if (mMaterial != INVALID_INDEX)
    {
        const Material& material = asset.mMaterials[mMaterial];
        LLColor4 baseColor(glm::value_ptr(material.mPbrMetallicRoughness.mBaseColorFactor));
        for (auto& dst : mColors)
        {
            dst = LLColor4U(baseColor * LLColor4(dst));
        }

        if (material.mUnlit.mPresent)
        { // material uses KHR_materials_unlit
            unlit = true;
        }
    }

    if (mNormals.empty() && !unlit)
    {
        mTangents.clear();

        if (mMode == Mode::POINTS || mMode == Mode::LINES || mMode == Mode::LINE_LOOP || mMode == Mode::LINE_STRIP)
        { //no normals and no surfaces, this primitive is unlit
            mTangents.clear();
            unlit = true;
        }
        else
        {
            // unroll into non-indexed array of flat shaded triangles
            MikktMesh data;
            if (!data.copy(this))
            {
                return false;
            }

            data.genNormals();
            data.genTangents();
            data.write(this);
        }
    }

    if (mTangents.empty() && !unlit)
    { // NOTE: must be done last because tangent generation rewrites the other arrays
        // adapted from usage of Mikktspace in llvolume.cpp
        if (mMode == Mode::POINTS || mMode == Mode::LINES || mMode == Mode::LINE_LOOP || mMode == Mode::LINE_STRIP)
        {
            // for points and lines, just make sure tangent is perpendicular to normal
            mTangents.resize(mNormals.size());
            LLVector4a up(0.f, 0.f, 1.f, 0.f);
            LLVector4a left(1.f, 0.f, 0.f, 0.f);
            for (U32 i = 0; i < mNormals.size(); ++i)
            {
                if (fabsf(mNormals[i].getF32ptr()[2]) < 0.999f)
                {
                    mTangents[i] = up.cross3(mNormals[i]);
                }
                else
                {
                    mTangents[i] = left.cross3(mNormals[i]);
                }

                mTangents[i].getF32ptr()[3] = 1.f;
            }
        }
        else
        {
            MikktMesh data;
            if (!data.copy(this))
            {
                return false;
            }

            data.genTangents();
            data.write(this);
        }
    }

    return true;
}

Primitive::~Primitive()
{
}

void Primitive::serialize(boost::json::object& dst) const
{
    write(mMaterial, "material", dst, -1);
    write(mMode, "mode", dst, Primitive::Mode::TRIANGLES);
    write(mIndices, "indices", dst, INVALID_INDEX);
    write(mAttributes, "attributes", dst);
}

const Primitive& Primitive::operator=(const Value& src)
{
    if (src.is_object())
    {
        copy(src, "material", mMaterial);
        copy(src, "mode", mMode);
        copy(src, "indices", mIndices);
        copy(src, "attributes", mAttributes);

        mArrayMode = mMode;
    }
    return *this;
}

