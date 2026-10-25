/**
 * @file llchatimagegif.h
 * @brief Chat inline image previews (#265): GIF decoding (still and animated)
 *        with the vendored Wuffs decoder.
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

#ifndef LL_LLCHATIMAGEGIF_H
#define LL_LLCHATIMAGEGIF_H

#include "llimage.h"
#include "llpointer.h"

#include <string>
#include <vector>

// Decodes an untrusted GIF into fully composited RGBA frames (non-premultiplied,
// like the PNG path's LLImageRaw), each fitted into a box and stretched to
// power-of-two dims, plus per-frame delays and the loop count. Pure function of
// its input: safe on a worker thread (reads no settings).
//
// Compositing follows the GIF rules as browsers apply them: the canvas starts
// transparent; each frame is drawn over it (or replaces its rect when the frame
// says so); after a frame is shown its disposal runs: none (keep), restore to
// background (its rect becomes transparent; the GIF background colour is ignored,
// as browsers do), restore to previous (the canvas goes back to what it was
// before the frame).
//
// Limits (all checked before or while decoding, never after allocating
// unbounded memory):
//  - canvas (logical screen, and Wuffs' enlarged first-frame bounds) <= max_dim
//    per side; the caller checks the header too;
//  - at most max_frames frames;
//  - the scaled frames together <= max_frame_bytes (the frame count is cut to fit,
//    the size of one frame is known before decoding);
//  - full-size compositing work <= MAX_WORK_PIXELS canvas pixels over all frames,
//    and MAX_DECODE_SECONDS wall clock.
// When a limit (or a decode error after the first frame, e.g. a truncated file)
// stops the animation early, the frames decoded so far are kept and loop
// (`truncated` is set). A file whose first frame is cut short shows that partial
// frame as a still image. Only a file with no decodable frame fails.
namespace LLChatImageGif
{
    constexpr S32 MAX_FRAMES = 300;
    constexpr U64 MAX_WORK_PIXELS = U64(256) * 1024 * 1024;   // ~1 GB of RGBA traffic
    constexpr F64 MAX_DECODE_SECONDS = 3.0;
    constexpr F32 DEFAULT_DELAY = 0.1f;   // delays of 0 or 1 cs play at 10 cs (browsers)

    struct Params
    {
        S32 still_box_w = 720, still_box_h = 320;   // fit box of a one-frame GIF
        S32 anim_box_w = 360, anim_box_h = 160;     // fit box of an animated GIF's frames
        S32 pot_max = 1024;          // power-of-two stretch cap; 0 = keep the fitted size
        S32 max_dim = 4096;          // canvas limit per side
        S32 max_frames = MAX_FRAMES;
        U64 max_frame_bytes = U64(32) * 1024 * 1024;   // all stored (scaled) frames
    };

    struct Result
    {
        bool ok = false;
        std::string reason;                           // user-facing, when !ok
        S32 orig_w = 0, orig_h = 0;                   // canvas size
        std::vector<LLPointer<LLImageRaw> > frames;   // 4 components, same dims
        std::vector<F32> delays;                      // seconds, one per frame
        U32 loops = 0;                                // times to play; 0 = forever
        bool truncated = false;                       // limits / errors cut it short
    };

    // "GIF87a"/"GIF89a" magic and the logical screen size from the header (no
    // decoding).
    bool headerDims(const U8* data, size_t len, U32& width, U32& height);

    void decode(const U8* data, size_t len, const Params& params, Result& out);

    // Area-average downscale of a non-premultiplied RGBA canvas (top row first)
    // into `dst` (dst_w <= src_w, dst_h <= src_h), alpha-weighted. Shared with the
    // WebP decoder (llchatimagewebp.cpp).
    void boxDownscale(const U8* src, S32 src_w, S32 src_h, U8* dst, S32 dst_w, S32 dst_h);
}

#endif // LL_LLCHATIMAGEGIF_H
