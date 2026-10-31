/**
 * @file fuzz_llsd_binary.cpp
 * @brief libFuzzer harness: LLSD binary parser (LLSDBinaryParser).
 *
 * Reached from mesh headers (llmeshrepository), asset caches, and zlib'd LLSD blobs.
 */
#include "fuzz_common.h"

#include "llsdserialize.h"

#include <sstream>

extern "C" int LLVMFuzzerInitialize(int*, char***)
{
    fuzz::initCommon();
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    std::istringstream in(std::string(reinterpret_cast<const char*>(data), size));
    LLSD sd;
    const S32 result = LLSDSerialize::fromBinary(sd, in, static_cast<llssize>(size));
    if (result > 0)
    {
        std::ostringstream out;
        LLSDSerialize::toBinary(sd, out);
        std::istringstream again(out.str());
        LLSD sd2;
        LLSDSerialize::fromBinary(sd2, again, static_cast<llssize>(out.str().size()));
    }
    return 0;
}
