/**
 * @file fuzz_notecard.cpp
 * @brief libFuzzer harness: LLNotecard::importStream, the "Linden text version N" notecard
 * format (embedded inventory item list + length-prefixed text), parsed from asset downloads.
 */
#include "fuzz_common.h"

#include "llnotecard.h"

#include <sstream>

extern "C" int LLVMFuzzerInitialize(int*, char***)
{
    fuzz::initCommon();
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size > (4u << 20))
    {
        return 0;
    }
    std::istringstream in(std::string(reinterpret_cast<const char*>(data), size));
    LLNotecard notecard(LLNotecard::MAX_SIZE);
    if (notecard.importStream(in))
    {
        // the editor reads the text and the embedded items back out
        volatile size_t sink = notecard.getText().size() + notecard.getItems().size();
        (void)sink;
    }
    return 0;
}
