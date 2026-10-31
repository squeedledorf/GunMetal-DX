/**
 * @file fuzz_llsd_xml.cpp
 * @brief libFuzzer harness: LLSD XML parser (LLSDXMLParser, expat based).
 *
 * Reached from HTTP capability replies, the event queue, caches and settings files.
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
    const S32 result = LLSDSerialize::fromXML(sd, in, false);
    if (result > 0)
    {
        // Whatever parsed must format and re-parse without a crash (equality is not asserted:
        // reals, dates and binary legitimately normalise).
        std::ostringstream out;
        LLSDSerialize::toXML(sd, out);
        std::istringstream again(out.str());
        LLSD sd2;
        LLSDSerialize::fromXML(sd2, again, false);
    }
    return 0;
}
