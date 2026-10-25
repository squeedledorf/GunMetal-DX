/**
 * @file llchatimagegif.cpp
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

#include "llviewerprecompiledheaders.h"

#include "llchatimagegif.h"

#include "llwuffs.h"

#include <chrono>
#include <cstring>

namespace
{
    // Owns a heap-allocated, initialized Wuffs GIF decoder (the struct is opaque:
    // C++ construction is deleted outside the implementation TU).
    struct GifDecoder
    {
        wuffs_gif__decoder* dec = wuffs_gif__decoder__alloc();
        ~GifDecoder() { if (dec) free(dec); }
        GifDecoder() = default;
        GifDecoder(const GifDecoder&) = delete;
        GifDecoder& operator=(const GifDecoder&) = delete;
    };

    bool is_ok(const wuffs_base__status& st)
    {
        return wuffs_base__status__is_ok(&st);
    }

    // GIF delay (flicks) -> seconds; 0 or 1 centisecond plays at 10 cs, as browsers do.
    F32 delay_seconds(wuffs_base__flicks flicks)
    {
        const wuffs_base__flicks one_cs = WUFFS_BASE__FLICKS_PER_SECOND / 100;
        if (flicks <= one_cs)
        {
            return LLChatImageGif::DEFAULT_DELAY;
        }
        return (F32)((F64)flicks / (F64)WUFFS_BASE__FLICKS_PER_SECOND);
    }

    // A frame's rect clipped to the canvas; false when empty.
    bool clip_rect(const wuffs_base__rect_ie_u32& r, U32 w, U32 h, U32& x0, U32& y0, U32& x1, U32& y1)
    {
        x0 = llmin(r.min_incl_x, w);
        x1 = llmin(r.max_excl_x, w);
        y0 = llmin(r.min_incl_y, h);
        y1 = llmin(r.max_excl_y, h);
        return x0 < x1 && y0 < y1;
    }

    // Clears a frame's rect (clipped to the canvas) to transparent.
    void clear_rect(std::vector<U8>& canvas, U32 w, U32 h, const wuffs_base__rect_ie_u32& r)
    {
        U32 x0, y0, x1, y1;
        if (!clip_rect(r, w, h, x0, y0, x1, y1))
        {
            return;
        }
        for (U32 y = y0; y < y1; ++y)
        {
            memset(canvas.data() + ((size_t)y * w + x0) * 4, 0, (size_t)(x1 - x0) * 4);
        }
    }

    // Copies a frame's rect (clipped) out of the canvas (`save`) or back into it.
    // A GIF frame only draws inside its rect, so RESTORE_PREVIOUS needs no more.
    void copy_rect(std::vector<U8>& canvas, U32 w, U32 h, const wuffs_base__rect_ie_u32& r,
                   std::vector<U8>& saved, bool save)
    {
        U32 x0, y0, x1, y1;
        if (!clip_rect(r, w, h, x0, y0, x1, y1))
        {
            saved.clear();
            return;
        }
        const size_t row = (size_t)(x1 - x0) * 4;
        if (save)
        {
            saved.resize(row * (size_t)(y1 - y0));
        }
        else if (saved.size() != row * (size_t)(y1 - y0))
        {
            return;
        }
        for (U32 y = y0; y < y1; ++y)
        {
            U8* c = canvas.data() + ((size_t)y * w + x0) * 4;
            U8* s = saved.data() + (size_t)(y - y0) * row;
            if (save)
            {
                memcpy(s, c, row);
            }
            else
            {
                memcpy(c, s, row);
            }
        }
    }
}

// Area-average downscale of a non-premultiplied RGBA canvas into `dst`
// (dst_w <= src_w, dst_h <= src_h). Colour is alpha-weighted so transparent
// pixels don't darken the edges of what is drawn next to them.
void LLChatImageGif::boxDownscale(const U8* src, S32 src_w, S32 src_h, U8* dst, S32 dst_w, S32 dst_h)
{
    if (dst_w == src_w && dst_h == src_h)
    {
        memcpy(dst, src, (size_t)src_w * (size_t)src_h * 4);
        return;
    }
    for (S32 ty = 0; ty < dst_h; ++ty)
    {
        const S32 y0 = (S32)((S64)ty * src_h / dst_h);
        const S32 y1 = llmax(y0 + 1, (S32)((S64)(ty + 1) * src_h / dst_h));
        for (S32 tx = 0; tx < dst_w; ++tx)
        {
            const S32 x0 = (S32)((S64)tx * src_w / dst_w);
            const S32 x1 = llmax(x0 + 1, (S32)((S64)(tx + 1) * src_w / dst_w));
            U64 r = 0, g = 0, b = 0, a = 0;
            for (S32 y = y0; y < y1; ++y)
            {
                const U8* p = src + ((size_t)y * (size_t)src_w + (size_t)x0) * 4;
                for (S32 x = x0; x < x1; ++x, p += 4)
                {
                    const U64 pa = p[3];
                    r += (U64)p[0] * pa;
                    g += (U64)p[1] * pa;
                    b += (U64)p[2] * pa;
                    a += pa;
                }
            }
            U8* q = dst + ((size_t)ty * (size_t)dst_w + (size_t)tx) * 4;
            if (a == 0)
            {
                q[0] = q[1] = q[2] = q[3] = 0;
                continue;
            }
            const U64 count = (U64)(y1 - y0) * (U64)(x1 - x0);
            q[0] = (U8)((r + a / 2) / a);
            q[1] = (U8)((g + a / 2) / a);
            q[2] = (U8)((b + a / 2) / a);
            q[3] = (U8)((a + count / 2) / count);
        }
    }
}

// static
bool LLChatImageGif::headerDims(const U8* data, size_t len, U32& width, U32& height)
{
    // Header (6) + logical screen descriptor: width, height (u16 little-endian), ...
    if (!data || len < 13 || (memcmp(data, "GIF87a", 6) != 0 && memcmp(data, "GIF89a", 6) != 0))
    {
        return false;
    }
    width = U32(data[6]) | (U32(data[7]) << 8);
    height = U32(data[8]) | (U32(data[9]) << 8);
    return true;
}

void LLChatImageGif::decode(const U8* data, size_t len, const Params& params, Result& out)
{
    out = Result();
    U32 hdr_w = 0, hdr_h = 0;
    if (!headerDims(data, len, hdr_w, hdr_h))
    {
        out.reason = "not a valid image";
        return;
    }
    if (hdr_w > (U32)params.max_dim || hdr_h > (U32)params.max_dim)
    {
        out.reason = llformat("too large (%u x %u)", hdr_w, hdr_h);
        return;
    }
    // The reader never writes through the pointer.
    U8* bytes = const_cast<U8*>(data);

    // ---- pass 1: frame configs only (skips the pixel data): count + loop count ----
    U32 width = 0, height = 0;
    S32 frame_count = 0;
    bool more_frames = false;   // stopped counting at the cap
    {
        GifDecoder gif;
        if (!gif.dec)
        {
            out.reason = "out of memory";
            return;
        }
        wuffs_base__io_buffer src = wuffs_base__ptr_u8__reader(bytes, len, true);
        wuffs_base__image_config ic = wuffs_base__null_image_config();
        if (!is_ok(wuffs_gif__decoder__decode_image_config(gif.dec, &ic, &src)))
        {
            out.reason = "not a valid image";
            return;
        }
        // Wuffs grows the canvas to cover the first frame (non-strict bounds): check again.
        width = wuffs_base__pixel_config__width(&ic.pixcfg);
        height = wuffs_base__pixel_config__height(&ic.pixcfg);
        if (width == 0 || height == 0)
        {
            out.reason = "not a valid image";
            return;
        }
        if (width > (U32)params.max_dim || height > (U32)params.max_dim)
        {
            out.reason = llformat("too large (%u x %u)", width, height);
            return;
        }
        const S32 cap = llmax(1, params.max_frames);
        while (true)
        {
            wuffs_base__frame_config fc = wuffs_base__null_frame_config();
            const wuffs_base__status st = wuffs_gif__decoder__decode_frame_config(gif.dec, &fc, &src);
            if (!is_ok(st))
            {
                break;   // end of data, or an error / truncation: frames so far count
            }
            if (frame_count >= cap)
            {
                more_frames = true;
                break;
            }
            ++frame_count;
        }
        // Read after the frame configs: the NETSCAPE loop extension can come late.
        // Wuffs reports plays (the extension's value + 1), 0 = forever, 1 without it.
        out.loops = wuffs_gif__decoder__num_animation_loops(gif.dec);
    }
    if (frame_count == 0)
    {
        out.reason = "not a valid image";
        return;
    }

    out.orig_w = (S32)width;
    out.orig_h = (S32)height;
    const bool animated = frame_count > 1;

    // Output size (as the PNG path: fit the box, never upscale, then stretch to
    // power-of-two dims), known before any pixel is decoded.
    const S32 box_w = animated ? params.anim_box_w : params.still_box_w;
    const S32 box_h = animated ? params.anim_box_h : params.still_box_h;
    const F32 fit = llmin(1.f, llmin((F32)box_w / (F32)width, (F32)box_h / (F32)height));
    const S32 target_w = llclamp(ll_round((F32)width * fit), 1, (S32)width);
    const S32 target_h = llclamp(ll_round((F32)height * fit), 1, (S32)height);
    const S32 out_w = params.pot_max > 0 ? LLImageRaw::biasedDimToPowerOfTwo(target_w, params.pot_max) : target_w;
    const S32 out_h = params.pot_max > 0 ? LLImageRaw::biasedDimToPowerOfTwo(target_h, params.pot_max) : target_h;

    // Frame budget: stored bytes, compositing work, frame cap.
    const U64 frame_bytes = (U64)out_w * (U64)out_h * 4;
    const U64 canvas_pixels = (U64)width * (U64)height;
    S32 keep = frame_count;
    keep = (S32)llmin((U64)keep, llmax((U64)1, params.max_frame_bytes / llmax((U64)1, frame_bytes)));
    keep = (S32)llmin((U64)keep, llmax((U64)1, MAX_WORK_PIXELS / canvas_pixels));
    out.truncated = more_frames || keep < frame_count;

    // ---- pass 2: decode and composite `keep` frames ----
    GifDecoder gif;
    if (!gif.dec)
    {
        out.reason = "out of memory";
        return;
    }
    wuffs_base__io_buffer src = wuffs_base__ptr_u8__reader(bytes, len, true);
    wuffs_base__image_config ic = wuffs_base__null_image_config();
    if (!is_ok(wuffs_gif__decoder__decode_image_config(gif.dec, &ic, &src)) ||
        wuffs_base__pixel_config__width(&ic.pixcfg) != width ||
        wuffs_base__pixel_config__height(&ic.pixcfg) != height)
    {
        out.reason = "not a valid image";
        return;
    }
    wuffs_base__pixel_config__set(&ic.pixcfg, WUFFS_BASE__PIXEL_FORMAT__RGBA_NONPREMUL,
                                  WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, width, height);

    std::vector<U8> canvas((size_t)canvas_pixels * 4, 0);   // transparent
    std::vector<U8> previous;                               // RESTORE_PREVIOUS: the frame rect before it
    wuffs_base__pixel_buffer pb;
    if (!is_ok(wuffs_base__pixel_buffer__set_from_slice(&pb, &ic.pixcfg,
                                                         wuffs_base__make_slice_u8(canvas.data(), canvas.size()))))
    {
        out.reason = "could not decode";
        return;
    }

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds((S64)(MAX_DECODE_SECONDS * 1000.0));
    out.frames.reserve((size_t)keep);
    out.delays.reserve((size_t)keep);
    for (S32 i = 0; i < keep; ++i)
    {
        wuffs_base__frame_config fc = wuffs_base__null_frame_config();
        if (!is_ok(wuffs_gif__decoder__decode_frame_config(gif.dec, &fc, &src)))
        {
            out.truncated = true;
            break;
        }
        const wuffs_base__animation_disposal disposal = wuffs_base__frame_config__disposal(&fc);
        if (disposal == WUFFS_BASE__ANIMATION_DISPOSAL__RESTORE_PREVIOUS)
        {
            copy_rect(canvas, width, height, wuffs_base__frame_config__bounds(&fc), previous, true);
        }
        const wuffs_base__pixel_blend blend = wuffs_base__frame_config__overwrite_instead_of_blend(&fc)
                                              ? WUFFS_BASE__PIXEL_BLEND__SRC
                                              : WUFFS_BASE__PIXEL_BLEND__SRC_OVER;
        const wuffs_base__status st = wuffs_gif__decoder__decode_frame(gif.dec, &pb, &src, blend,
                                                                       wuffs_base__empty_slice_u8(), nullptr);
        const bool complete = is_ok(st);
        if (!complete && !out.frames.empty())
        {
            out.truncated = true;   // a broken later frame: play what we have
            break;
        }

        LLPointer<LLImageRaw> raw = new LLImageRaw((U16)target_w, (U16)target_h, 4);
        if (raw->isBufferInvalid())
        {
            out.reason = "out of memory";
            break;
        }
        boxDownscale(canvas.data(), (S32)width, (S32)height, raw->getData(), target_w, target_h);
        // The canvas is top row first; LLImageRaw is bottom row first (as the PNG/JPEG decoders write it).
        raw->verticalFlip();
        if ((out_w != target_w || out_h != target_h) && !raw->scale(out_w, out_h))
        {
            out.reason = "could not decode";
            break;
        }
        out.frames.push_back(raw);
        out.delays.push_back(delay_seconds(wuffs_base__frame_config__duration(&fc)));

        if (!complete)
        {
            out.truncated = true;   // first frame cut short: show what was drawn, as a still
            break;
        }
        // Dispose of the frame before the next one is drawn.
        if (disposal == WUFFS_BASE__ANIMATION_DISPOSAL__RESTORE_BACKGROUND)
        {
            clear_rect(canvas, width, height, wuffs_base__frame_config__bounds(&fc));
        }
        else if (disposal == WUFFS_BASE__ANIMATION_DISPOSAL__RESTORE_PREVIOUS)
        {
            copy_rect(canvas, width, height, wuffs_base__frame_config__bounds(&fc), previous, false);
        }
        if (i + 1 < keep && std::chrono::steady_clock::now() > deadline)
        {
            out.truncated = true;
            break;
        }
    }

    if (out.frames.empty())
    {
        if (out.reason.empty())
        {
            out.reason = "not a valid image";
        }
        return;
    }
    out.reason.clear();
    out.ok = true;
}
