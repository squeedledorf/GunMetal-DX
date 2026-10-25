#pragma once

/**
 * @file gltfscenemanager.h
 * @brief Builds menus out of items.
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

#include "llsingleton.h"
#include "llviewerobject.h"
#include "gltf/common.h"

class LLVOVolume;
class LLDrawable;

namespace LL
{
    class GLTFSceneManager : public LLSimpleton<GLTFSceneManager>
    {
    public:
        ~GLTFSceneManager();
        // load GLTF file from disk

        void load(); // open filepicker to choose asset
        void load(const std::string& filename); // load asset from filename

        void saveAs(); // open filepicker and choose file to save selected asset to
        void save(const std::string& filename); // save selected asset to filename (suitable for use in external programs)
        void uploadSelection(); // decompose selected asset and upload to simulator

        void update();


        void addGLTFObject(LLViewerObject* object, LLUUID gltf_id);
        static void onGLTFLoadComplete(const LLUUID& id, LLAssetType::EType asset_type, void* user_data, S32 status, LLExtStat ext_status);
        static void onGLTFBinLoadComplete(const LLUUID& id, LLAssetType::EType asset_type, void* user_data, S32 status, LLExtStat ext_status);

        std::vector<LLPointer<LLViewerObject>> mObjects;

        std::shared_ptr<GLTF::Asset> mUploadingAsset;
        bool mGLTFUploadPending = false;
        LLPointer<LLViewerObject> mUploadingObject;
        U32 mPendingImageUploads = 0;
        U32 mPendingBinaryUploads = 0;
        U32 mPendingGLTFUploads = 0;

    };
}


