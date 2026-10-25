/**
 * @file llchatimagewebp.h
 * @brief Chat inline image previews (#265, #651): WebP decoding (still and
 *        animated) with libwebp (built from source, indra/cmake/WebP.cmake).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#ifndef LL_LLCHATIMAGEWEBP_H
#define LL_LLCHATIMAGEWEBP_H

#include "llchatimagegif.h"   // Params / Result / limits: same contract as the GIF decoder

#include <string>

// Decodes an untrusted WebP (lossy, lossless, with or without alpha, still or
// animated) into fully composited RGBA frames (non-premultiplied, like the PNG
// path's LLImageRaw), each fitted into a box and stretched to power-of-two dims,
// plus per-frame delays and the loop count. Same Params / Result and the same
// limits as LLChatImageGif::decode; a pure function of its input, safe on a
// worker thread.
//
// Compositing of animations (blend / dispose against the canvas) is libwebp's
// WebPAnimDecoder. Frame durations of 10 ms or less play at
// LLChatImageGif::DEFAULT_DELAY (as browsers do); the loop count is the file's
// (0 = forever).
//
// Limits, checked before the decoder allocates anything sized by the file:
//  - canvas (from the RIFF header: VP8 / VP8L / VP8X) <= max_dim per side;
//  - at most max_frames frames; the scaled frames together <= max_frame_bytes;
//    full-size compositing work <= LLChatImageGif::MAX_WORK_PIXELS canvas pixels
//    and LLChatImageGif::MAX_DECODE_SECONDS wall clock (the frame count is cut to
//    fit, `truncated` is set, the rest loops).
// A file libwebp cannot parse completely (truncated, malformed) fails: no preview.
// A later animation frame that fails to decode ends the animation there.
namespace LLChatImageWebP
{
    // "RIFF....WEBP" magic and the canvas size from the header (no decoding).
    // `animated` is set for a file with the animation flag.
    bool headerDims(const U8* data, size_t len, U32& width, U32& height, bool& animated);

    void decode(const U8* data, size_t len, const LLChatImageGif::Params& params,
                LLChatImageGif::Result& out);
}

#endif // LL_LLCHATIMAGEWEBP_H
