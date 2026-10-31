/**
 * @file fuzz_mesh_asset.cpp
 * @brief libFuzzer harness: LLModel::loadModel, the whole-mesh-asset reader (binary LLSD header,
 * then skin info, decomposition and the chosen LOD block located through the header offsets).
 *
 * Input byte 0 selects the LOD (0..4: lowest, low, medium, high, physics), the rest is the asset.
 * The offsets come from the header, so header fields pointing past the end or backwards are in
 * scope. This is the model-upload / local-asset path; the network LOD path is fuzz_mesh_lod.
 */
#include "fuzz_common.h"

#include "llmodel.h"
#include "llvolume.h"

#include <sstream>

extern "C" int LLVMFuzzerInitialize(int*, char***)
{
    fuzz::initCommon();
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size < 2 || size > (16u << 20))
    {
        return 0;
    }

    LLVolumeParams params;
    params.setType(LL_PCODE_PROFILE_SQUARE, LL_PCODE_PATH_LINE);
    params.setSculptID(LLUUID("11111111-2222-3333-4444-555555555555"), LL_SCULPT_TYPE_MESH);

    const F32 lod = static_cast<F32>(data[0] % 5);   // LLModel reads its LOD from the volume detail
    LLPointer<LLModel> model = new LLModel(params, lod);

    std::istringstream in(std::string(reinterpret_cast<const char*>(data + 1), size - 1));
    model->loadModel(in);
    return 0;
}
