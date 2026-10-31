/**
 * @file fuzz_inventory.cpp
 * @brief libFuzzer harness: the legacy inventory text format ("inv_item", "inv_category" blocks
 * with permissions / sale_info / key-value lines), LLInventoryItem / LLInventoryCategory
 * ::importLegacyStream. Used for embedded notecard items, task inventory (llsd-less sims) and
 * inventory cache files.
 *
 * Byte 0, bit 0 selects item (0) or category (1); the rest is the text.
 */
#include "fuzz_common.h"

#include "llinventory.h"

#include <sstream>

extern "C" int LLVMFuzzerInitialize(int*, char***)
{
    fuzz::initCommon();
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size < 1 || size > (1u << 20))
    {
        return 0;
    }
    const bool category = (data[0] & 1) != 0;
    std::istringstream in(std::string(reinterpret_cast<const char*>(data + 1), size - 1));

    std::ostringstream out;
    if (category)
    {
        LLPointer<LLInventoryCategory> cat = new LLInventoryCategory;
        if (cat->importLegacyStream(in))
        {
            cat->exportLegacyStream(out);
        }
    }
    else
    {
        LLPointer<LLInventoryItem> item = new LLInventoryItem;
        if (item->importLegacyStream(in))
        {
            item->exportLegacyStream(out);
            // and back: the saved cache must re-import
            std::istringstream again(out.str());
            LLPointer<LLInventoryItem> item2 = new LLInventoryItem;
            item2->importLegacyStream(again);
        }
    }
    return 0;
}
