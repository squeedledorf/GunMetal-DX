/**
 * @file fuzz_gltf_material.cpp
 * @brief libFuzzer harness: LLGLTFMaterial inputs from the network and from files.
 *
 * Byte 0, bit 0 selects the path:
 *   0: LLGLTFMaterial::fromJSON  (a glTF material asset, i.e. what the material editor, the
 *      asset fetch and local materials hand it; tinygltf parses the JSON)
 *   1: LLSDSerialize::deserialize -> LLGLTFMaterial::applyOverrideLLSD  (the per-face material
 *      override LLSD that arrives in object updates / ModifyMaterialParams replies)
 *
 * Not covered: the full glTF asset / scene loader (newview/gltf/asset.h), which is tied to the
 * viewer (see README).
 */
#include "fuzz_common.h"

// tinygltf's implementation lives in exactly one translation unit; the viewer defines it in
// newview/gltf/llgltfloader.cpp and llprimitive's own unit test does the same as below.
#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_USE_CPP14
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define TINYGLTF_NO_EXTERNAL_IMAGE 1
#include "tinygltf/tiny_gltf.h"

#include "llgltfmaterial.h"
#include "llsdserialize.h"

#include <sstream>

extern "C" int LLVMFuzzerInitialize(int*, char***)
{
    fuzz::initCommon();
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size < 2 || size > (4u << 20))
    {
        return 0;
    }
    const bool llsd_path = (data[0] & 1) != 0;
    const std::string body(reinterpret_cast<const char*>(data + 1), size - 1);

    LLGLTFMaterial material;
    if (!llsd_path)
    {
        std::string warn, error;
        if (material.fromJSON(body, warn, error))
        {
            // what the viewer does next with a good material: serialise it again
            const std::string json = material.asJSON();
            LLGLTFMaterial again;
            again.fromJSON(json, warn, error);
        }
    }
    else
    {
        std::istringstream in(body);
        LLSD sd;
        if (LLSDSerialize::deserialize(sd, in, static_cast<llssize>(body.size())))
        {
            material.applyOverrideLLSD(sd);
        }
    }
    return 0;
}
