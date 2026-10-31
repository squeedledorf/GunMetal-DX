/**
 * @file fuzz_mesh_lod.cpp
 * @brief libFuzzer harness: LLVolume::unpackVolumeFaces(U8*, S32) on a zlib-compressed LLSD LOD
 * block, exactly what LLMeshRepoThread::lodReceivedImpl and physicsShapeReceived hand it from
 * the network (llmeshrepository.cpp). Input = the raw LOD / physics block bytes.
 *
 * Not covered: LLMeshHeader parse and strip_deprecated_header (newview only; see README).
 */
#include "fuzz_common.h"

#include "llvolume.h"
#include "llvolumemgr.h"

#include <vector>

extern "C" int LLVMFuzzerInitialize(int*, char***)
{
    fuzz::initCommon();
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size == 0 || size > (16u << 20))
    {
        return 0;
    }

    LLVolumeParams params;
    params.setType(LL_PCODE_PROFILE_SQUARE, LL_PCODE_PATH_LINE);
    params.setSculptID(LLUUID("11111111-2222-3333-4444-555555555555"), LL_SCULPT_TYPE_MESH);

    // the detail a mesh LOD 3 is requested at
    LLPointer<LLVolume> volume = new LLVolume(params, LLVolumeLODGroup::getVolumeScaleFromDetail(3));
    // unpackVolumeFaces takes a mutable pointer but does not write to it
    std::vector<U8> copy(data, data + size);
    volume->unpackVolumeFaces(copy.data(), static_cast<S32>(copy.size()));

    // the clean pipeline and the physics path read the result; touch every face
    const S32 faces = volume->getNumVolumeFaces();
    for (S32 i = 0; i < faces; ++i)
    {
        const LLVolumeFace& face = volume->getVolumeFace(i);
        volatile S32 sink = face.mNumVertices + face.mNumIndices;
        (void)sink;
    }
    return 0;
}
