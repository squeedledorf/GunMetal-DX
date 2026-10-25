#pragma once

/**
 * @file primitive.h
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

#include "llrender.h"
#include <boost/unordered_map.hpp>
#include "boost/json.hpp"

// LL GLTF Implementation
namespace LL
{
    namespace GLTF
    {
        using Value = boost::json::value;
        class Asset;

        class Primitive
        {
        public:
            enum class Mode : U8
            {
                POINTS,
                LINES,
                LINE_LOOP,
                LINE_STRIP,
                TRIANGLES,
                TRIANGLE_STRIP,
                TRIANGLE_FAN
            };

            ~Primitive();

            // CPU copy of mesh data
            std::vector<LLVector2> mTexCoords0;
            std::vector<LLVector2> mTexCoords1;
            std::vector<LLVector4a> mNormals;
            std::vector<LLVector4a> mTangents;
            std::vector<LLVector4a> mPositions;
            std::vector<U64> mJoints;
            std::vector<LLVector4a> mWeights;
            std::vector<LLColor4U> mColors;
            std::vector<U32> mIndexArray;

            // (No raycast octree: glTF scene objects are picked on the GPU, #704.)

            S32 mMaterial = -1;
            Mode mMode = Mode::TRIANGLES; // default to triangles
            // topology of the CPU arrays: mMode, or TRIANGLES once MikktSpace normal
            // generation unrolled them into a flat-shaded triangle list
            Mode mArrayMode = Mode::TRIANGLES;
            S32 mIndices = -1;

            U32 getVertexCount() const { return (U32) mPositions.size(); }
            U32 getIndexCount() const { return (U32) mIndexArray.size(); }

            boost::unordered_map<std::string, S32> mAttributes;


            void serialize(boost::json::object& obj) const;
            const Primitive& operator=(const Value& src);

            bool prep(Asset& asset);
        };
    }
}
