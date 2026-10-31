/**
 * @file fuzz_mesh_skin.cpp
 * @brief libFuzzer harness: mesh skin info (joint names, inverse bind matrices, bind shape) and
 * physics decomposition built from a decoded LLSD, as llmeshrepository.cpp skinInfoReceivedImpl
 * and decompositionReceived do after LLUZipHelper::unzip_llsd.
 *
 * Input byte 0 selects skin info (even) or decomposition (odd); the rest is a zlib-compressed
 * binary LLSD blob, i.e. the on-the-wire form. Seeds are produced by indra/fuzz/make_seeds.py.
 */
#include "fuzz_common.h"

#include "llmodel.h"
#include "llsdserialize.h"

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

    LLSD sd;
    try
    {
        if (LLUZipHelper::unzip_llsd(sd, data + 1, static_cast<S32>(size - 1)) != LLUZipHelper::ZR_OK)
        {
            return 0;
        }

        if ((data[0] & 1) == 0)
        {
            LLMeshSkinInfo skin(LLUUID("11111111-2222-3333-4444-555555555555"), sd);
            // exercise the consumers of the parsed arrays
            LLSD back = skin.asLLSD(true, false);
            (void)back;
        }
        else
        {
            LLModel::Decomposition decomp(sd);
            LLSD back = decomp.asLLSD();
            (void)back;
        }
    }
    catch (const std::bad_alloc&)
    {
        // soft failure in the viewer (#698)
    }
    return 0;
}
