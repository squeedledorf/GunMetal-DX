/**
 * @file fuzz_llsd_auto.cpp
 * @brief libFuzzer harness: LLSDSerialize::deserialize, which sniffs the "<? LLSD/... ?>" header
 * (or the first token) and dispatches to the XML, notation or binary parser. This is the entry
 * point used for HTTP bodies of unknown flavour.
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
    LLSDSerialize::deserialize(sd, in, static_cast<llssize>(size));
    return 0;
}
