/**
 * @file llimagetiff.h
 * @brief Baseline TIFF decoder (read only).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

#ifndef LL_LLIMAGETIFF_H
#define LL_LLIMAGETIFF_H

#include "llimage.h"

// Decodes the first page of a baseline TIFF into RGB or RGBA.
// Uncompressed, PackBits, LZW and Deflate; strips or tiles; 1, 8 or 16 bit
// greyscale, palette or RGB, with an optional alpha channel.
class LLImageTIFF : public LLImageFormatted
{
protected:
    virtual ~LLImageTIFF();

public:
    LLImageTIFF();

    /*virtual*/ std::string getExtension() { return std::string("tif"); }
    /*virtual*/ bool updateData();
    /*virtual*/ bool decode(LLImageRaw* raw_image, F32 decode_time = 0.0);
    /*virtual*/ bool encode(const LLImageRaw* raw_image, F32 encode_time = 0.0);

private:
    // Parses the header; with raw_image set, also decodes the pixels into it.
    bool parse(LLImageRaw* raw_image);
};

#endif
