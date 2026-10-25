/**
 * @file llimagetiff.cpp
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

#include "linden_common.h"

#include "llimagetiff.h"

#include <algorithm>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#ifdef LL_USESYSTEMLIBS
# include <zlib.h>
#else
# include "zlib-ng/zlib.h"
#endif

// Everything read from the file is untrusted: every offset and count is
// checked against the buffer before use, sizes are computed in 64 bits, and
// only the first IFD is read, so a looping IFD chain cannot hang us.

namespace
{
constexpr U32 TIFF_MAX_DIM = 8192;              // same ceiling as the 256 MB raw buffer limit at 4 channels
constexpr U32 TIFF_MAX_ENTRIES = 4096;          // IFD entries
constexpr U32 TIFF_MAX_SAMPLES = 8;             // samples per pixel
constexpr U64 TIFF_MAX_CHUNKS = 1 << 20;        // strips or tiles
constexpr U64 TIFF_MAX_CHUNK_BYTES = 256u << 20; // one decompressed strip or tile

enum : U16
{
    TAG_WIDTH = 256, TAG_HEIGHT = 257, TAG_BITS = 258, TAG_COMPRESSION = 259,
    TAG_PHOTOMETRIC = 262, TAG_FILL_ORDER = 266, TAG_STRIP_OFFSETS = 273,
    TAG_SAMPLES = 277, TAG_ROWS_PER_STRIP = 278, TAG_STRIP_COUNTS = 279,
    TAG_PLANAR = 284, TAG_PREDICTOR = 317, TAG_COLORMAP = 320,
    TAG_TILE_WIDTH = 322, TAG_TILE_LENGTH = 323, TAG_TILE_OFFSETS = 324,
    TAG_TILE_COUNTS = 325, TAG_EXTRA_SAMPLES = 338, TAG_SAMPLE_FORMAT = 339
};

class TiffDecoder
{
public:
    TiffDecoder(const U8* data, size_t size) : mData(data), mSize(size) {}

    bool readHeader();
    bool decode(U8* out); // out: mWidth * mHeight * outComponents(), bottom-up rows

    S32 outComponents() const { return mHasAlpha ? 4 : 3; }

    std::string mError;
    U32 mWidth = 0;
    U32 mHeight = 0;

private:
    bool fail(const std::string& msg) { mError = msg; return false; }

    U16 rd16(size_t o) const
    {
        return mBigEndian ? U16(mData[o] << 8 | mData[o + 1]) : U16(mData[o] | mData[o + 1] << 8);
    }
    U32 rd32(size_t o) const
    {
        return mBigEndian
            ? (U32(mData[o]) << 24 | U32(mData[o + 1]) << 16 | U32(mData[o + 2]) << 8 | U32(mData[o + 3]))
            : (U32(mData[o]) | U32(mData[o + 1]) << 8 | U32(mData[o + 2]) << 16 | U32(mData[o + 3]) << 24);
    }

    const U8* findEntry(U16 tag) const;
    bool getValues(U16 tag, std::vector<U32>& out, U64 max_count);
    bool getScalar(U16 tag, U32 def, U32& out);

    bool decompress(U32 index, std::vector<U8>& buf, size_t need);
    bool lzw(const U8* in, size_t n, U8* out, size_t need);
    bool packbits(const U8* in, size_t n, U8* out, size_t need);
    bool inflateChunk(const U8* in, size_t n, U8* out, size_t need);
    void unpredict(U8* buf, size_t rowbytes, U32 rows, U32 samples_per_row, U32 stride);

    const U8* mData;
    size_t mSize;
    bool mBigEndian = false;
    size_t mIFD = 0;
    U32 mEntries = 0;

    U32 mBits = 0;
    U32 mSamples = 1;
    U32 mCompression = 1;
    U32 mPhotometric = 1;
    U32 mPlanar = 1;
    U32 mPredictor = 1;
    U32 mBase = 1;       // colour channels: 1 grey/palette, 3 RGB
    U32 mKept = 1;       // channels we keep: base, plus one if alpha
    bool mHasAlpha = false;
    bool mAssociated = false;
    bool mTiled = false;
    U32 mChunkWidth = 0; // tile width, or image width for strips
    U32 mChunkHeight = 0; // tile length, or rows per strip
    std::vector<U32> mOffsets;
    std::vector<U32> mCounts;
    std::vector<U32> mColorMap;
};

const U8* TiffDecoder::findEntry(U16 tag) const
{
    for (U32 i = 0; i < mEntries; ++i)
    {
        const U8* e = mData + mIFD + 2 + 12 * size_t(i);
        if (rd16(e - mData) == tag)
        {
            return e;
        }
    }
    return nullptr;
}

// Reads a BYTE, SHORT or LONG array. An absent tag gives an empty array.
bool TiffDecoder::getValues(U16 tag, std::vector<U32>& out, U64 max_count)
{
    out.clear();
    const U8* e = findEntry(tag);
    if (!e)
    {
        return true;
    }
    size_t eo = e - mData;
    U16 type = rd16(eo + 2);
    U64 count = rd32(eo + 4);
    U64 elem = (type == 1) ? 1 : (type == 3) ? 2 : (type == 4) ? 4 : 0;
    if (!elem)
    {
        return fail("TIFF tag " + std::to_string(tag) + " has an unsupported field type");
    }
    if (count == 0 || count > max_count)
    {
        return fail("TIFF tag " + std::to_string(tag) + " has a bad value count");
    }
    U64 bytes = count * elem;
    U64 vo = (bytes <= 4) ? eo + 8 : rd32(eo + 8);
    if (vo + bytes > mSize)
    {
        return fail("TIFF tag " + std::to_string(tag) + " points outside the file");
    }
    out.resize(size_t(count));
    for (size_t i = 0; i < out.size(); ++i)
    {
        size_t o = size_t(vo + i * elem);
        out[i] = (elem == 1) ? mData[o] : (elem == 2) ? rd16(o) : rd32(o);
    }
    return true;
}

bool TiffDecoder::getScalar(U16 tag, U32 def, U32& out)
{
    std::vector<U32> v;
    if (!getValues(tag, v, 1 << 16))
    {
        return false;
    }
    out = v.empty() ? def : v[0];
    return true;
}

bool TiffDecoder::readHeader()
{
    if (mSize < 8)
    {
        return fail("TIFF file is too short");
    }
    if (mData[0] == 'I' && mData[1] == 'I')
    {
        mBigEndian = false;
    }
    else if (mData[0] == 'M' && mData[1] == 'M')
    {
        mBigEndian = true;
    }
    else
    {
        return fail("Not a TIFF file");
    }
    U16 magic = rd16(2);
    if (magic == 43)
    {
        return fail("BigTIFF files are not supported");
    }
    if (magic != 42)
    {
        return fail("Not a TIFF file");
    }

    // First IFD only: multi-page files give their first page.
    mIFD = rd32(4);
    if (mIFD < 8 || U64(mIFD) + 2 > mSize)
    {
        return fail("TIFF directory offset is invalid");
    }
    mEntries = rd16(mIFD);
    if (mEntries == 0 || mEntries > TIFF_MAX_ENTRIES || U64(mIFD) + 2 + 12 * U64(mEntries) > mSize)
    {
        return fail("TIFF directory is invalid or truncated");
    }

    U32 compression, photometric, fill_order, rows_per_strip, planar, predictor;
    std::vector<U32> bits, extra, format;
    if (!getScalar(TAG_WIDTH, 0, mWidth) || !getScalar(TAG_HEIGHT, 0, mHeight)
        || !getScalar(TAG_SAMPLES, 1, mSamples) || !getValues(TAG_BITS, bits, TIFF_MAX_SAMPLES)
        || !getScalar(TAG_COMPRESSION, 1, compression) || !getScalar(TAG_PHOTOMETRIC, 0xFFFF, photometric)
        || !getScalar(TAG_FILL_ORDER, 1, fill_order) || !getScalar(TAG_PLANAR, 1, planar)
        || !getScalar(TAG_PREDICTOR, 1, predictor) || !getValues(TAG_EXTRA_SAMPLES, extra, TIFF_MAX_SAMPLES)
        || !getValues(TAG_SAMPLE_FORMAT, format, TIFF_MAX_SAMPLES))
    {
        return false;
    }

    if (mWidth == 0 || mHeight == 0)
    {
        return fail("TIFF image has no size");
    }
    if (mWidth > TIFF_MAX_DIM || mHeight > TIFF_MAX_DIM)
    {
        return fail("TIFF image is larger than " + std::to_string(TIFF_MAX_DIM) + " pixels on a side");
    }
    if (mSamples == 0 || mSamples > TIFF_MAX_SAMPLES)
    {
        return fail("TIFF image has an unsupported number of channels");
    }

    mBits = bits.empty() ? 1 : bits[0];
    for (U32 b : bits)
    {
        if (b != mBits)
        {
            return fail("TIFF images with mixed bit depths are not supported");
        }
    }
    if (mBits != 1 && mBits != 8 && mBits != 16)
    {
        return fail("TIFF images with " + std::to_string(mBits) + " bits per sample are not supported (only 1, 8 and 16)");
    }
    for (U32 f : format)
    {
        if (f != 1)
        {
            return fail("Floating point and signed TIFF images are not supported");
        }
    }

    switch (compression)
    {
    case 1: case 5: case 8: case 32946: case 32773:
        break;
    case 6: case 7:
        return fail("JPEG-compressed TIFF images are not supported");
    default:
        return fail("TIFF compression type " + std::to_string(compression) + " is not supported");
    }
    mCompression = compression;

    if (photometric == 0xFFFF) // missing; guess like most readers do
    {
        photometric = (mSamples >= 3) ? 2 : 1;
    }
    switch (photometric)
    {
    case 0: case 1:
        mBase = 1;
        break;
    case 2:
        mBase = 3;
        break;
    case 3:
        mBase = 1;
        break;
    case 5:
        return fail("CMYK TIFF images are not supported");
    case 6:
        return fail("YCbCr TIFF images are not supported");
    default:
        return fail("TIFF colour space " + std::to_string(photometric) + " is not supported");
    }
    mPhotometric = photometric;
    if (mSamples < mBase)
    {
        return fail("TIFF image has too few channels for its colour space");
    }
    if (mBits == 1 && (mSamples != 1 || photometric > 1))
    {
        return fail("1-bit TIFF images must be single-channel black and white");
    }
    if (mBits == 1 && fill_order == 2)
    {
        return fail("TIFF bit fill order 2 is not supported");
    }

    if (photometric == 3)
    {
        if (mBits != 8)
        {
            return fail("Only 8-bit palette TIFF images are supported");
        }
        if (!getValues(TAG_COLORMAP, mColorMap, 3 * 256))
        {
            return false;
        }
        if (mColorMap.size() != 3 * 256)
        {
            return fail("TIFF palette is missing or the wrong size");
        }
    }

    // The first extra sample is alpha if it says so; others are ignored.
    if (mSamples > mBase && !extra.empty() && (extra[0] == 1 || extra[0] == 2))
    {
        mHasAlpha = true;
        mAssociated = (extra[0] == 1);
    }
    mKept = mBase + (mHasAlpha ? 1 : 0);

    if (planar != 1 && planar != 2)
    {
        return fail("TIFF planar configuration is invalid");
    }
    mPlanar = (mSamples == 1) ? 1 : planar;

    if (predictor != 1 && predictor != 2)
    {
        return fail("TIFF predictor " + std::to_string(predictor) + " is not supported");
    }
    if (predictor == 2 && mBits == 1)
    {
        return fail("TIFF predictor on 1-bit images is not supported");
    }
    mPredictor = (mCompression == 1 || mCompression == 32773) ? 1 : predictor;

    // Strip or tile layout.
    mTiled = findEntry(TAG_TILE_OFFSETS) != nullptr;
    if (mTiled)
    {
        if (!getScalar(TAG_TILE_WIDTH, 0, mChunkWidth) || !getScalar(TAG_TILE_LENGTH, 0, mChunkHeight)
            || !getValues(TAG_TILE_OFFSETS, mOffsets, TIFF_MAX_CHUNKS)
            || !getValues(TAG_TILE_COUNTS, mCounts, TIFF_MAX_CHUNKS))
        {
            return false;
        }
        if (mChunkWidth == 0 || mChunkHeight == 0 || mChunkWidth > TIFF_MAX_DIM || mChunkHeight > TIFF_MAX_DIM)
        {
            return fail("TIFF tile size is invalid");
        }
        // A tile larger than the image is legal (a small image in one 256 x 256 tile), but one far larger only
        // makes a tiny file allocate a huge chunk buffer: allow up to the image size (tiles are multiples of 16)
        // or 1024, whichever is bigger.
        const auto tile_cap = [](U32 dim) { return std::max<U32>(((dim + 15u) / 16u) * 16u, 1024u); };
        if (mChunkWidth > tile_cap(mWidth) || mChunkHeight > tile_cap(mHeight))
        {
            return fail("TIFF tiles are much larger than the image");
        }
    }
    else
    {
        if (!getScalar(TAG_ROWS_PER_STRIP, mHeight, rows_per_strip)
            || !getValues(TAG_STRIP_OFFSETS, mOffsets, TIFF_MAX_CHUNKS)
            || !getValues(TAG_STRIP_COUNTS, mCounts, TIFF_MAX_CHUNKS))
        {
            return false;
        }
        if (rows_per_strip == 0)
        {
            return fail("TIFF rows per strip is zero");
        }
        mChunkWidth = mWidth;
        mChunkHeight = rows_per_strip < mHeight ? rows_per_strip : mHeight;
    }

    U64 across = (U64(mWidth) + mChunkWidth - 1) / mChunkWidth;
    U64 down = (U64(mHeight) + mChunkHeight - 1) / mChunkHeight;
    U64 planes = (mPlanar == 2) ? mSamples : 1;
    U64 chunks = across * down * planes;
    if (chunks > TIFF_MAX_CHUNKS)
    {
        return fail("TIFF image has too many strips or tiles");
    }
    if (mOffsets.size() < chunks || mCounts.size() < chunks)
    {
        return fail("TIFF strip or tile table is missing or too short");
    }
    U64 row_bytes = (U64(mChunkWidth) * (mPlanar == 2 ? 1 : mSamples) * mBits + 7) / 8;
    if (row_bytes * mChunkHeight > TIFF_MAX_CHUNK_BYTES)
    {
        return fail("TIFF strips or tiles are too large");
    }
    for (U64 i = 0; i < chunks; ++i)
    {
        if (U64(mOffsets[i]) + mCounts[i] > mSize)
        {
            return fail("TIFF file is truncated");
        }
    }
    return true;
}

bool TiffDecoder::packbits(const U8* in, size_t n, U8* out, size_t need)
{
    size_t i = 0, pos = 0;
    while (pos < need && i < n)
    {
        S32 c = S8(in[i++]);
        if (c >= 0)
        {
            size_t len = size_t(c) + 1;
            if (len > n - i || len > need - pos)
            {
                return false;
            }
            memcpy(out + pos, in + i, len);
            i += len;
            pos += len;
        }
        else if (c != -128)
        {
            size_t len = size_t(1 - c);
            if (i >= n || len > need - pos)
            {
                return false;
            }
            memset(out + pos, in[i++], len);
            pos += len;
        }
    }
    return pos == need;
}

bool TiffDecoder::lzw(const U8* in, size_t n, U8* out, size_t need)
{
    if (n >= 2 && in[0] == 0 && (in[1] & 1))
    {
        return fail("Old-style LZW TIFF images are not supported");
    }

    // Dictionary: each code is its prefix code plus one byte.
    std::vector<U16> prefix(4096), length(4096);
    std::vector<U8> suffix(4096), first(4096);
    for (U32 i = 0; i < 256; ++i)
    {
        prefix[i] = 0xFFFF;
        length[i] = 1;
        suffix[i] = first[i] = U8(i);
    }

    const U64 total_bits = U64(n) * 8;
    U64 bitpos = 0;
    U32 nbits = 9;
    U32 next = 258;
    S32 prev = -1;
    size_t pos = 0;
    while (pos < need && bitpos + nbits <= total_bits)
    {
        size_t byte = size_t(bitpos >> 3);
        U32 window = U32(in[byte]) << 16
            | (byte + 1 < n ? U32(in[byte + 1]) << 8 : 0)
            | (byte + 2 < n ? U32(in[byte + 2]) : 0);
        U32 code = (window >> (24 - (bitpos & 7) - nbits)) & ((1u << nbits) - 1);
        bitpos += nbits;

        if (code == 257) // end of information
        {
            break;
        }
        if (code == 256) // clear
        {
            nbits = 9;
            next = 258;
            prev = -1;
            continue;
        }
        if (prev < 0)
        {
            if (code > 255)
            {
                return false;
            }
            out[pos++] = U8(code);
            prev = code;
            continue;
        }
        if (code > next || (code == next && next >= 4096))
        {
            return false;
        }
        if (next < 4096)
        {
            // code == next is the KwKwK case: prev's string plus its first byte.
            prefix[next] = U16(prev);
            suffix[next] = (code == next) ? first[prev] : first[code];
            first[next] = first[prev];
            length[next] = length[prev] + 1;
            ++next;
        }

        // Emit the string for code, written back to front, clipped to the buffer.
        U32 len = length[code];
        U32 k = code;
        for (U32 j = len; j-- > 0;)
        {
            if (pos + j < need)
            {
                out[pos + j] = suffix[k];
            }
            k = prefix[k];
        }
        pos = (len > need - pos) ? need : pos + len;
        prev = code;

        // TIFF's "early change": widen one code before the table fills.
        if (next >= (1u << nbits) - 1 && nbits < 12)
        {
            ++nbits;
        }
    }
    return pos == need;
}

bool TiffDecoder::inflateChunk(const U8* in, size_t n, U8* out, size_t need)
{
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (inflateInit(&zs) != Z_OK)
    {
        return false;
    }
    zs.next_in = const_cast<Bytef*>(in);
    zs.avail_in = uInt(n);
    zs.next_out = out;
    zs.avail_out = uInt(need);
    S32 ret = inflate(&zs, Z_FINISH);
    bool ok = (ret == Z_STREAM_END || ret == Z_BUF_ERROR || ret == Z_OK) && zs.total_out == need;
    inflateEnd(&zs);
    return ok;
}

bool TiffDecoder::decompress(U32 index, std::vector<U8>& buf, size_t need)
{
    const U8* in = mData + mOffsets[index];
    size_t n = mCounts[index];
    buf.assign(need, 0);
    bool ok = false;
    switch (mCompression)
    {
    case 1:
        ok = n >= need;
        if (ok)
        {
            memcpy(buf.data(), in, need);
        }
        break;
    case 32773:
        ok = packbits(in, n, buf.data(), need);
        break;
    case 5:
        ok = lzw(in, n, buf.data(), need);
        break;
    case 8: case 32946:
        ok = inflateChunk(in, n, buf.data(), need);
        break;
    }
    if (!ok && mError.empty())
    {
        mError = "TIFF image data is truncated or corrupt";
    }
    return ok;
}

// Undo horizontal differencing (predictor 2) on 8 or 16 bit samples.
void TiffDecoder::unpredict(U8* buf, size_t rowbytes, U32 rows, U32 samples_per_row, U32 stride)
{
    for (U32 r = 0; r < rows; ++r)
    {
        U8* row = buf + r * rowbytes;
        if (mBits == 8)
        {
            for (U32 i = stride; i < samples_per_row; ++i)
            {
                row[i] = U8(row[i] + row[i - stride]);
            }
        }
        else
        {
            const U32 hi = mBigEndian ? 0 : 1;
            for (U32 i = stride; i < samples_per_row; ++i)
            {
                U8* cur = row + 2 * i;
                const U8* left = row + 2 * (i - stride);
                U16 v = U16((cur[hi] << 8 | cur[1 - hi]) + (left[hi] << 8 | left[1 - hi]));
                cur[hi] = U8(v >> 8);
                cur[1 - hi] = U8(v);
            }
        }
    }
}

bool TiffDecoder::decode(U8* out)
{
    const U32 oc = outComponents();
    const U32 planes = (mPlanar == 2) ? mSamples : 1;
    const U32 cs = (mPlanar == 2) ? 1 : mSamples; // samples per pixel inside one chunk
    const U32 across = (mWidth + mChunkWidth - 1) / mChunkWidth;
    const U32 down = (mHeight + mChunkHeight - 1) / mChunkHeight;
    const size_t rowbytes = (size_t(mChunkWidth) * cs * mBits + 7) / 8;
    const U32 hi = mBigEndian ? 0 : 1; // high byte of a 16-bit sample

    std::vector<U8> buf;
    for (U32 p = 0; p < planes; ++p)
    {
        for (U32 d = 0; d < down; ++d)
        {
            const U32 y0 = d * mChunkHeight;
            // Strips stop at the image edge; tiles are always full size.
            const U32 rows = mTiled ? mChunkHeight : std::min(mChunkHeight, mHeight - y0);
            for (U32 a = 0; a < across; ++a)
            {
                const U32 index = (p * down + d) * across + a;
                if (!decompress(index, buf, rowbytes * rows))
                {
                    return false;
                }
                if (mPredictor == 2)
                {
                    unpredict(buf.data(), rowbytes, rows, mChunkWidth * cs, cs);
                }

                const U32 x0 = a * mChunkWidth;
                for (U32 r = 0; r < rows && y0 + r < mHeight; ++r)
                {
                    const U8* src = buf.data() + r * rowbytes;
                    U8* dst = out + (size_t(mHeight - 1 - (y0 + r)) * mWidth + x0) * oc;
                    for (U32 c = 0; c < mChunkWidth && x0 + c < mWidth; ++c, dst += oc)
                    {
                        for (U32 s = 0; s < cs; ++s)
                        {
                            const U32 ch = (mPlanar == 2) ? p : s;
                            if (ch >= mKept)
                            {
                                continue;
                            }
                            const size_t i = size_t(c) * cs + s;
                            U8 v;
                            if (mBits == 8)
                            {
                                v = src[i];
                            }
                            else if (mBits == 16)
                            {
                                v = src[2 * i + hi];
                            }
                            else
                            {
                                v = ((src[i >> 3] >> (7 - (i & 7))) & 1) ? 255 : 0;
                            }
                            dst[ch < mBase ? ch : 3] = v;
                        }
                    }
                }
            }
        }
    }

    // Expand grey and palette to RGB, flip WhiteIsZero, un-premultiply alpha.
    const size_t pixels = size_t(mWidth) * mHeight;
    U8* px = out;
    for (size_t i = 0; i < pixels; ++i, px += oc)
    {
        U32 r, g, b;
        if (mPhotometric == 3)
        {
            r = mColorMap[px[0]] >> 8;
            g = mColorMap[256 + px[0]] >> 8;
            b = mColorMap[512 + px[0]] >> 8;
        }
        else if (mBase == 1)
        {
            r = g = b = (mPhotometric == 0) ? 255 - px[0] : px[0];
        }
        else
        {
            r = px[0];
            g = px[1];
            b = px[2];
        }
        if (mHasAlpha && mAssociated && px[3] < 255)
        {
            const U32 alpha = px[3];
            if (alpha == 0)
            {
                r = g = b = 0;
            }
            else
            {
                r = std::min(255u, (r * 255 + alpha / 2) / alpha);
                g = std::min(255u, (g * 255 + alpha / 2) / alpha);
                b = std::min(255u, (b * 255 + alpha / 2) / alpha);
            }
        }
        px[0] = U8(r);
        px[1] = U8(g);
        px[2] = U8(b);
    }
    return true;
}

} // namespace

LLImageTIFF::LLImageTIFF()
    : LLImageFormatted(IMG_CODEC_TIFF)
{
}

LLImageTIFF::~LLImageTIFF()
{
}

bool LLImageTIFF::parse(LLImageRaw* raw_image)
{
    if (!getData() || getDataSize() <= 0)
    {
        setLastError("LLImageTIFF has no data");
        return false;
    }

    TiffDecoder tiff(getData(), size_t(getDataSize()));
    if (!tiff.readHeader())
    {
        setLastError(tiff.mError);
        return false;
    }
    if (!raw_image)
    {
        setSize(tiff.mWidth, tiff.mHeight, tiff.outComponents());
        return true;
    }

    if (!raw_image->resize(U16(tiff.mWidth), U16(tiff.mHeight), S8(tiff.outComponents())))
    {
        setLastError("LLImageTIFF failed to allocate the image");
        return false;
    }
    try
    {
        if (!tiff.decode(raw_image->getData()))
        {
            setLastError(tiff.mError);
            return false;
        }
    }
    catch (const std::bad_alloc&)
    {
        setLastError("LLImageTIFF ran out of memory");
        return false;
    }
    return true;
}

bool LLImageTIFF::updateData()
{
    resetLastError();
    LLImageDataLock lock(this);
    return parse(nullptr);
}

bool LLImageTIFF::decode(LLImageRaw* raw_image, F32 decode_time)
{
    llassert_always(raw_image);
    resetLastError();
    LLImageDataSharedLock lockIn(this);
    LLImageDataLock lockOut(raw_image);
    return parse(raw_image);
}

bool LLImageTIFF::encode(const LLImageRaw* raw_image, F32 encode_time)
{
    setLastError("Saving TIFF images is not supported");
    return false;
}
