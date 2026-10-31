/**
 * @file fuzz_unzip_llsd.cpp
 * @brief libFuzzer harness: LLUZipHelper::unzip_llsd (zlib inflate + binary LLSD).
 *
 * Mesh skin info, physics decomposition, physics shape and LOD blocks all funnel through this
 * (llmeshrepository.cpp skinInfoReceived / decompositionReceived, LLVolume::unpackVolumeFaces).
 */
#include "fuzz_common.h"

#include "llsdserialize.h"

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
    LLSD sd;
    try
    {
        LLUZipHelper::unzip_llsd(sd, data, static_cast<S32>(size));
    }
    catch (const std::bad_alloc&)
    {
        // The viewer treats allocation failure as a soft failure (#698); not a finding here.
    }
    return 0;
}
