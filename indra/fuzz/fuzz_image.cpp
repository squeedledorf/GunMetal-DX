/**
 * @file fuzz_image.cpp
 * @brief libFuzzer harness: an llimage decoder, selected at compile time by FUZZ_CODEC
 * (IMG_CODEC_TGA / PNG / BMP / JPEG / TIFF / J2C; DXT is supported but not built, see
 * CMakeLists.txt). CMake builds one binary per codec from this file: fuzz_image_tga,
 * fuzz_image_png, ...
 *
 * Mirrors the viewer: the bytes become the formatted image's data, updateData() parses the
 * header (the texture fetcher's first step, on network data), then decode() produces pixels
 * (the image worker's step). J2C goes through LLImageJ2COJ / OpenJPEG.
 *
 * A report on a tiny input whose header claims a huge image (allocation or rss_limit_mb) is a
 * real finding, not noise: the fetcher feeds these decoders unauthenticated server data.
 */
#include "fuzz_common.h"

#include "llimage.h"

#include <cstring>

extern "C" int LLVMFuzzerInitialize(int*, char***)
{
    fuzz::initCommon();
    LLImage::initClass();
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size == 0 || size > (32u << 20))
    {
        return 0;
    }

    LLPointer<LLImageFormatted> image = LLImageFormatted::createFromType(FUZZ_CODEC);
    if (image.isNull())
    {
        return 0;
    }

    U8* buffer = image->allocateData(static_cast<S32>(size));
    if (!buffer)
    {
        return 0;
    }
    std::memcpy(buffer, data, size);

    if (!image->updateData())
    {
        return 0;
    }

    LLPointer<LLImageRaw> raw = new LLImageRaw;
    image->decode(raw, 0.f);
    return 0;
}
