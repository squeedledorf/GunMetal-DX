/**
 * @file gltfscenemanager.cpp
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

#include "llviewerprecompiledheaders.h"

#include "gltfscenemanager.h"
#include "llviewermenufile.h"
#include "llappviewer.h"
#include "lltinygltfhelper.h"
#include "llselectmgr.h"
#include "llagent.h"
#include "llnotificationsutil.h"
#include "llvoavatarself.h"
#include "gltf/asset.h"
#include "llviewertexturelist.h"
#include "llimagej2c.h"
#include "llfloaterperms.h"
#include "llfloaterreg.h"
#include "llagentbenefits.h"
#include "llfilesystem.h"
#include "llviewercontrol.h"
#include "boost/json.hpp"
#include "llcleaningest.h"

#define GLTF_SIM_SUPPORT 1

using namespace LL;

// temporary location of LL GLTF Implementation
using namespace LL::GLTF;

void GLTFSceneManager::load()
{
    LLViewerObject* obj = LLSelectMgr::instance().getSelection()->getFirstRootObject();

    if (obj)
    {
        // Load a scene from disk
        LLFilePickerReplyThread::startPicker(
            [](const std::vector<std::string>& filenames, LLFilePicker::ELoadFilter load_filter, LLFilePicker::ESaveFilter save_filter)
            {
                if (LLAppViewer::instance()->quitRequested())
                {
                    return;
                }
                try
                {
                    if (filenames.size() > 0)
                    {
                        GLTFSceneManager::instance().load(filenames[0]);
                    }
                }
                catch (std::bad_alloc&)
                {
                    LLNotificationsUtil::add("CannotOpenFileTooBig");
                }
            },
            LLFilePicker::FFLOAD_GLTF,
            false);
    }
    else
    {
        LLNotificationsUtil::add("GLTFOpenSelection");
    }
}

void GLTFSceneManager::saveAs()
{
    LLViewerObject* obj = LLSelectMgr::instance().getSelection()->getFirstRootObject();
    if (obj && obj->mGLTFAsset)
    {
        LLFilePickerReplyThread::startPicker(
            [](const std::vector<std::string>& filenames, LLFilePicker::ELoadFilter load_filter, LLFilePicker::ESaveFilter save_filter)
            {
                if (LLAppViewer::instance()->quitRequested())
                {
                    return;
                }
                if (filenames.size() > 0)
                {
                    GLTFSceneManager::instance().save(filenames[0]);
                }
            },
            LLFilePicker::FFSAVE_GLTF,
            "scene.gltf");
    }
    else
    {
        LLNotificationsUtil::add("GLTFSaveSelection");
    }
}

void GLTFSceneManager::uploadSelection()
{
    if (mUploadingAsset)
    { // upload already in progress
        LLNotificationsUtil::add("GLTFUploadInProgress");
        return;
    }

    LLViewerObject* obj = LLSelectMgr::instance().getSelection()->getFirstRootObject();
    if (obj && obj->mGLTFAsset)
    {
        // make a copy of the asset prior to uploading
        mUploadingAsset = std::make_shared<Asset>();
        mUploadingObject = obj;
        *mUploadingAsset = *obj->mGLTFAsset;

        GLTF::Asset& asset = *mUploadingAsset;

        for (auto& image : asset.mImages)
        {
            if (image.mTexture.notNull())
            {
                mPendingImageUploads++;

                LLPointer<LLImageRaw> raw;

                if (image.mBufferView != INVALID_INDEX)
                {
                    BufferView& view = asset.mBufferViews[image.mBufferView];
                    Buffer& buffer = asset.mBuffers[view.mBuffer];

                    raw = LLViewerTextureManager::getRawImageFromMemory(buffer.mData.data() + view.mByteOffset, view.mByteLength, image.mMimeType);

                    image.clearData(asset);
                }
                else
                {
                    raw = image.mTexture->getRawImage();
                }

                if (raw.isNull())
                {
                    raw = image.mTexture->getSavedRawImage();
                }

                if (raw.notNull())
                {
                    LLPointer<LLImageJ2C> j2c = LLViewerTextureList::convertToUploadFile(raw);

                    std::string buffer;
                    buffer.assign((const char*)j2c->getData(), j2c->getDataSize());

                    LLUUID asset_id = LLUUID::generateNewID();

                    std::string name;
                    S32 idx = (S32)(&image - &asset.mImages[0]);

                    if (image.mName.empty())
                    {

                        name = llformat("Image_%d", idx);
                    }
                    else
                    {
                        name = image.mName;
                    }

                    LLNewBufferedResourceUploadInfo::uploadFailure_f failure = [this](LLUUID assetId, LLSD response, std::string reason)
                        {
                            // TODO: handle failure
                            mPendingImageUploads--;
                            return false;
                        };


                    LLNewBufferedResourceUploadInfo::uploadFinish_f finish = [this, idx, raw, j2c](LLUUID assetId, LLSD response)
                        {
                            if (mUploadingAsset && mUploadingAsset->mImages.size() > idx)
                            {
                                mUploadingAsset->mImages[idx].mUri = assetId.asString();
                                mPendingImageUploads--;
                            }
                        };

                    S32 expected_upload_cost = LLAgentBenefitsMgr::current().getTextureUploadCost(j2c);

                    LLResourceUploadInfo::ptr_t uploadInfo(std::make_shared<LLNewBufferedResourceUploadInfo>(
                        buffer,
                        asset_id,
                        name,
                        name,
                        0,
                        LLFolderType::FT_TEXTURE,
                        LLInventoryType::IT_TEXTURE,
                        LLAssetType::AT_TEXTURE,
                        LLFloaterPerms::getNextOwnerPerms("Uploads"),
                        LLFloaterPerms::getGroupPerms("Uploads"),
                        LLFloaterPerms::getEveryonePerms("Uploads"),
                        expected_upload_cost,
                        LLUUID::null,
                        false,
                        finish,
                        failure));

                    upload_new_resource(uploadInfo);
                }
            }
        }

        // upload .bin
        for (auto& bin : asset.mBuffers)
        {
            mPendingBinaryUploads++;

            S32 idx = (S32)(&bin - &asset.mBuffers[0]);

            std::string buffer;
            buffer.assign((const char*)bin.mData.data(), bin.mData.size());

            LLUUID asset_id = LLUUID::generateNewID();

            LLNewBufferedResourceUploadInfo::uploadFailure_f failure = [this](LLUUID assetId, LLSD response, std::string reason)
                {
                    // TODO: handle failure
                    mPendingBinaryUploads--;
                    mUploadingAsset = nullptr;
                    mUploadingObject = nullptr;
                    LL_WARNS("GLTF") << "Failed to upload GLTF binary: " << reason << LL_ENDL;
                    LL_WARNS("GLTF") << response << LL_ENDL;
                    return false;
                };

            LLNewBufferedResourceUploadInfo::uploadFinish_f finish = [this, idx](LLUUID assetId, LLSD response)
                {
                    if (mUploadingAsset && mUploadingAsset->mBuffers.size() > idx)
                    {
                        mUploadingAsset->mBuffers[idx].mUri = assetId.asString();
                        mPendingBinaryUploads--;

                        // HACK: save buffer to cache to emulate a successful download
                        LLFileSystem cache(assetId, LLAssetType::AT_GLTF_BIN, LLFileSystem::WRITE);
                        auto& data = mUploadingAsset->mBuffers[idx].mData;

                        llassert(data.size() <= size_t(S32_MAX));
                        cache.write((const U8 *) data.data(), S32(data.size()));
                    }
                };
#if GLTF_SIM_SUPPORT
            S32 expected_upload_cost = 1;

            LLResourceUploadInfo::ptr_t uploadInfo(std::make_shared<LLNewBufferedResourceUploadInfo>(
                buffer,
                asset_id,
                "",
                "",
                0,
                LLFolderType::FT_NONE,
                LLInventoryType::IT_GLTF_BIN,
                LLAssetType::AT_GLTF_BIN,
                LLFloaterPerms::getNextOwnerPerms("Uploads"),
                LLFloaterPerms::getGroupPerms("Uploads"),
                LLFloaterPerms::getEveryonePerms("Uploads"),
                expected_upload_cost,
                LLUUID::null,
                false,
                finish,
                failure));

            upload_new_resource(uploadInfo);
#else
            // dummy finish
            finish(LLUUID::generateNewID(), LLSD());
#endif
        }
    }
    else
    {
        LLNotificationsUtil::add("GLTFUploadSelection");
    }
}

void GLTFSceneManager::save(const std::string& filename)
{
    LLViewerObject* obj = LLSelectMgr::instance().getSelection()->getFirstRootObject();
    if (obj && obj->mGLTFAsset)
    {
        Asset* asset = obj->mGLTFAsset.get();
        if (!asset->save(filename))
        {
            LLNotificationsUtil::add("GLTFSaveFailed");
        }
    }
}

void GLTFSceneManager::load(const std::string& filename)
{
    std::shared_ptr<Asset> asset = std::make_shared<Asset>();

    if (asset->load(filename, true))
    {
        asset->updateTransforms();

        // hang the asset off the currently selected object, or off of the avatar if no object is selected
        LLViewerObject* obj = LLSelectMgr::instance().getSelection()->getFirstRootObject();

        if (obj)
        { // assign to self avatar
            obj->mGLTFAsset = asset;
            obj->markForUpdate();
            if (std::find(mObjects.begin(), mObjects.end(), obj) == mObjects.end())
            {
                mObjects.push_back(obj);
            }
            // VK (#39): ingest the local preview next frame, not on the sweep's lap.
            LLCleanIngest::instance().noteObjectChanged(obj);
            LLFloaterReg::showInstance("gltf_asset_editor");
        }
    }
    else
    {
        LLNotificationsUtil::add("GLTFLoadFailed");
    }
}

GLTFSceneManager::~GLTFSceneManager()
{
    mObjects.clear();
}

void GLTFSceneManager::addGLTFObject(LLViewerObject* obj, LLUUID gltf_id)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_GLTF;
    llassert(obj->getVolume()->getParams().getSculptID() == gltf_id);
    llassert(obj->getVolume()->getParams().getSculptType() == LL_SCULPT_TYPE_GLTF);

    if (obj->mGLTFAsset || obj->mIsGLTFAssetMissing )
    {
        // object already has a GLTF asset or load failed, don't reload it

        // TODO: below assertion fails on dupliate requests for assets -- possibly need to touch up asset loading state machine
        // llassert(std::find(mObjects.begin(), mObjects.end(), obj) != mObjects.end());
        return;
    }

    obj->ref();
    gAssetStorage->getAssetData(gltf_id, LLAssetType::AT_GLTF, onGLTFLoadComplete, obj);
}

//static
void GLTFSceneManager::onGLTFBinLoadComplete(const LLUUID& id, LLAssetType::EType asset_type, void* user_data, S32 status, LLExtStat ext_status)
{
    LLAppViewer::instance()->postToMainCoro([=]()
        {
            LLViewerObject* obj = (LLViewerObject*)user_data;
            llassert(asset_type == LLAssetType::AT_GLTF_BIN);

            if (status == LL_ERR_NOERR)
            {
                if (obj)
                {
                    // find the Buffer with the given id in the asset
                    if (obj->mGLTFAsset)
                    {
                        obj->mGLTFAsset->mPendingBuffers--;


                        if (obj->mGLTFAsset->mPendingBuffers == 0)
                        {
                            bool prepped = false;
                            try
                            {
                                prepped = obj->mGLTFAsset->prep();
                            }
                            catch (const std::exception& e)
                            {
                                LL_WARNS("GLTF") << "Exception preparing GLTF asset " << id << ": " << e.what() << LL_ENDL;
                                prepped = false;
                            }
                            if (prepped)
                            {
                                GLTFSceneManager& mgr = GLTFSceneManager::instance();
                                if (std::find(mgr.mObjects.begin(), mgr.mObjects.end(), obj) == mgr.mObjects.end())
                                {
                                    GLTFSceneManager::instance().mObjects.push_back(obj);
                                }
                                // VK (#39): the asset is drawable now -- ingest it next frame.
                                LLCleanIngest::instance().noteObjectChanged(obj);
                            }
                            else
                            {
                                LL_WARNS("GLTF") << "Failed to prepare GLTF asset: " << id << ". Marking as missing." << LL_ENDL;
                                obj->mIsGLTFAssetMissing = true;
                                obj->mGLTFAsset = nullptr;
                            }
                        }
                    }
                    obj->unref(); // todo: use LLPointer
                }
            }
            else
            {
                LL_WARNS("GLTF") << "Failed to load GLTF asset: " << id << ". Marking as missing." << LL_ENDL;
                obj->mIsGLTFAssetMissing = true;
                obj->unref();
            }
        });
}

//static
void GLTFSceneManager::onGLTFLoadComplete(const LLUUID& id, LLAssetType::EType asset_type, void* user_data, S32 status, LLExtStat ext_status)
{
    LLViewerObject* obj = (LLViewerObject*)user_data;
    llassert(asset_type == LLAssetType::AT_GLTF);

    if (status == LL_ERR_NOERR)
    {
        if (obj)
        {
            LLFileSystem file(id, asset_type, LLFileSystem::READ);
            std::string data;
            S32 file_size = file.getSize();
            // the JSON part of a scene; its geometry lives in separate buffers
            constexpr S32 MAX_GLTF_JSON_BYTES = 64 * 1024 * 1024;
            if (file_size <= 0 || file_size > MAX_GLTF_JSON_BYTES)
            {
                LL_WARNS("GLTF") << "GLTF asset " << id << " has implausible size " << file_size << ". Marking as missing." << LL_ENDL;
                obj->mIsGLTFAssetMissing = true;
                obj->unref();
                return;
            }
            data.resize(file_size);
            file.read((U8*)data.data(), file_size);

            std::shared_ptr<Asset> asset;
            try
            {
                boost::json::value json = boost::json::parse(data);
                asset = std::make_shared<Asset>(json);
            }
            catch (const std::exception& e)
            {
                LL_WARNS("GLTF") << "Failed to parse GLTF asset " << id << ": " << e.what() << ". Marking as missing." << LL_ENDL;
                obj->mIsGLTFAssetMissing = true;
                obj->unref();
                return;
            }

            // Reject a malformed scene before fetching any of its buffers.
            if (!asset->validate())
            {
                LL_WARNS("GLTF") << "Invalid GLTF asset " << id << ". Marking as missing." << LL_ENDL;
                obj->mIsGLTFAssetMissing = true;
                obj->unref();
                return;
            }

            obj->mGLTFAsset = asset;

            for (auto& buffer : asset->mBuffers)
            {
                // for now just assume the buffer is already in the asset cache
                LLUUID buffer_id;
                if (LLUUID::parseUUID(buffer.mUri, &buffer_id))
                {
                    asset->mPendingBuffers++;

                    gAssetStorage->getAssetData(buffer_id, LLAssetType::AT_GLTF_BIN, onGLTFBinLoadComplete, obj);
                }
                else
                {
                    LL_WARNS("GLTF") << "Buffer URI is not a valid UUID: " << buffer.mUri << " for asset id: " << id << ". Marking as missing." << LL_ENDL;
                    obj->mIsGLTFAssetMissing = true;
                    obj->unref();
                    return;
                }
            }
        }
    }
    else
    {
        LL_WARNS("GLTF") << "Failed to load GLTF asset: " << id << ". Marking as missing." << LL_ENDL;
        obj->mIsGLTFAssetMissing = true;
        obj->unref();
    }
}

void GLTFSceneManager::update()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_GLTF;

    for (U32 i = 0; i < mObjects.size(); ++i)
    {
        if (mObjects[i]->isDead() || mObjects[i]->mGLTFAsset == nullptr)
        {
            mObjects.erase(mObjects.begin() + i);
            --i;
            continue;
        }

        mObjects[i]->mGLTFAsset->update();
    }

    // process pending uploads
    if (mUploadingAsset && !mGLTFUploadPending)
    {
        if (mPendingImageUploads == 0 && mPendingBinaryUploads == 0)
        {
            boost::json::object obj;
            mUploadingAsset->serialize(obj);
            std::string buffer = boost::json::serialize(obj, {});

            LLNewBufferedResourceUploadInfo::uploadFailure_f failure = [this](LLUUID assetId, LLSD response, std::string reason)
                {
                    // TODO: handle failure
                    LL_WARNS("GLTF") << "Failed to upload GLTF json: " << reason << LL_ENDL;
                    LL_WARNS("GLTF") << response << LL_ENDL;

                    mUploadingAsset = nullptr;
                    mUploadingObject = nullptr;
                    mGLTFUploadPending = false;
                    return false;
                };

            LLNewBufferedResourceUploadInfo::uploadFinish_f finish = [this, buffer](LLUUID assetId, LLSD response)
            {
                LLAppViewer::instance()->postToMainCoro(
                    [=, this]()
                    {
                        if (mUploadingAsset)
                        {
                            // HACK: save buffer to cache to emulate a successful upload
                            LLFileSystem cache(assetId, LLAssetType::AT_GLTF, LLFileSystem::WRITE);

                            LL_INFOS("GLTF") << "Uploaded GLTF json: " << assetId << LL_ENDL;
                            llassert(buffer.size() <= size_t(S32_MAX));
                            cache.write((const U8 *) buffer.c_str(), S32(buffer.size()));

                            mUploadingAsset = nullptr;
                        }

                        if (mUploadingObject)
                        {
                            mUploadingObject->mGLTFAsset = nullptr;
                            mUploadingObject->mIsGLTFAssetMissing = false;
                            mUploadingObject->setGLTFAsset(assetId);
                            mUploadingObject->markForUpdate();
                            mUploadingObject = nullptr;
                        }

                        mGLTFUploadPending = false;
                    });
            };

#if GLTF_SIM_SUPPORT
            S32 expected_upload_cost = 1;
            LLUUID asset_id = LLUUID::generateNewID();

            mGLTFUploadPending = true;

            LLResourceUploadInfo::ptr_t uploadInfo(std::make_shared<LLNewBufferedResourceUploadInfo>(
                buffer,
                asset_id,
                "",
                "",
                0,
                LLFolderType::FT_NONE,
                LLInventoryType::IT_GLTF,
                LLAssetType::AT_GLTF,
                LLFloaterPerms::getNextOwnerPerms("Uploads"),
                LLFloaterPerms::getGroupPerms("Uploads"),
                LLFloaterPerms::getEveryonePerms("Uploads"),
                expected_upload_cost,
                LLUUID::null,
                false,
                finish,
                failure));

            upload_new_resource(uploadInfo);
#else
            // dummy finish
            finish(LLUUID::generateNewID(), LLSD());
#endif
        }
    }
}

LLMatrix4a inverse(const LLMatrix4a& mat)
{
    glm::mat4 m = glm::make_mat4((F32*)mat.mMatrix);
    m = glm::inverse(m);
    LLMatrix4a ret;
    ret.loadu(glm::value_ptr(m));
    return ret;
}

