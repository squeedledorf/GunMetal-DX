/**
 * @file llchatimagewebp.cpp
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

#include "llviewerprecompiledheaders.h"

#include "llchatimagewebp.h"

#include <webp/decode.h>
#include <webp/demux.h>

#include <chrono>
#include <cstring>
#include <memory>
#include <new>

namespace
{
    struct AnimDecoderDeleter
    {
        void operator()(WebPAnimDecoder* dec) const { WebPAnimDecoderDelete(dec); }
    };
    typedef std::unique_ptr<WebPAnimDecoder, AnimDecoderDeleter> anim_decoder_ptr_t;

    // Frame duration (ms) -> seconds; 10 ms or less plays at 100 ms, as browsers do
    // (the same rule as GIF delays of 0 or 1 cs).
    F32 delay_seconds(int duration_ms)
    {
        if (duration_ms <= 10)
        {
            return LLChatImageGif::DEFAULT_DELAY;
        }
        return (F32)((F64)duration_ms / 1000.0);
    }

    // Output size for a canvas (as the PNG and GIF paths: fit the box, never
    // upscale, then stretch to power-of-two dims unless pot_max is 0).
    struct OutSize
    {
        S32 target_w = 1, target_h = 1;   // fitted
        S32 out_w = 1, out_h = 1;         // stored (power-of-two stretched)
    };

    OutSize fit_size(U32 width, U32 height, S32 box_w, S32 box_h, S32 pot_max)
    {
        OutSize s;
        const F32 fit = llmin(1.f, llmin((F32)box_w / (F32)width, (F32)box_h / (F32)height));
        s.target_w = llclamp(ll_round((F32)width * fit), 1, (S32)width);
        s.target_h = llclamp(ll_round((F32)height * fit), 1, (S32)height);
        s.out_w = pot_max > 0 ? LLImageRaw::biasedDimToPowerOfTwo(s.target_w, pot_max) : s.target_w;
        s.out_h = pot_max > 0 ? LLImageRaw::biasedDimToPowerOfTwo(s.target_h, pot_max) : s.target_h;
        return s;
    }

    // One full-size RGBA canvas (top row first) -> a fitted, flipped, stretched
    // LLImageRaw. Null (with `reason`) on failure.
    LLPointer<LLImageRaw> make_frame(const U8* canvas, U32 width, U32 height, const OutSize& size,
                                     std::string& reason)
    {
        LLPointer<LLImageRaw> raw = new LLImageRaw((U16)size.target_w, (U16)size.target_h, 4);
        if (raw->isBufferInvalid())
        {
            reason = "out of memory";
            return LLPointer<LLImageRaw>();
        }
        LLChatImageGif::boxDownscale(canvas, (S32)width, (S32)height, raw->getData(),
                                     size.target_w, size.target_h);
        // The canvas is top row first; LLImageRaw is bottom row first.
        raw->verticalFlip();
        if ((size.out_w != size.target_w || size.out_h != size.target_h) && !raw->scale(size.out_w, size.out_h))
        {
            reason = "could not decode";
            return LLPointer<LLImageRaw>();
        }
        return raw;
    }

    void decode_still(const U8* data, size_t len, U32 width, U32 height,
                      const LLChatImageGif::Params& params, LLChatImageGif::Result& out)
    {
        const OutSize size = fit_size(width, height, params.still_box_w, params.still_box_h, params.pot_max);

        // Full-size canvas, bounded by the header check (<= max_dim^2 * 4 bytes, the
        // same peak as the PNG path).
        std::vector<U8> canvas;
        try
        {
            canvas.resize((size_t)width * (size_t)height * 4);
        }
        catch (const std::bad_alloc&)
        {
            out.reason = "out of memory";
            return;
        }
        // Non-premultiplied RGBA; libwebp checks the bitstream's size against the
        // buffer and fails on truncated or malformed data.
        if (!WebPDecodeRGBAInto(data, len, canvas.data(), canvas.size(), (int)(width * 4)))
        {
            out.reason = "could not decode";
            return;
        }
        LLPointer<LLImageRaw> raw = make_frame(canvas.data(), width, height, size, out.reason);
        if (raw.isNull())
        {
            return;
        }
        out.frames.push_back(raw);
        out.delays.push_back(LLChatImageGif::DEFAULT_DELAY);
        out.loops = 0;
        out.ok = true;
    }

    void decode_animated(const U8* data, size_t len, U32 width, U32 height,
                         const LLChatImageGif::Params& params, LLChatImageGif::Result& out)
    {
        WebPAnimDecoderOptions options;
        if (!WebPAnimDecoderOptionsInit(&options))
        {
            out.reason = "could not decode";
            return;
        }
        options.color_mode = MODE_RGBA;   // non-premultiplied, like LLImageRaw from PNG
        options.use_threads = 0;

        // Parses (and validates) the whole container: every frame must lie inside
        // the canvas checked above. Allocates the canvas (and its dispose copy) at
        // that size. Truncated / malformed files fail here.
        WebPData webp_data;
        webp_data.bytes = data;
        webp_data.size = len;
        anim_decoder_ptr_t dec(WebPAnimDecoderNew(&webp_data, &options));
        if (!dec)
        {
            out.reason = "not a valid image";
            return;
        }
        WebPAnimInfo info;
        if (!WebPAnimDecoderGetInfo(dec.get(), &info) || info.frame_count == 0 ||
            info.canvas_width != width || info.canvas_height != height)
        {
            out.reason = "not a valid image";
            return;
        }
        const WebPDemuxer* demux = WebPAnimDecoderGetDemuxer(dec.get());
        if (!demux)
        {
            out.reason = "not a valid image";
            return;
        }

        const U32 cap = (U32)llmax(1, params.max_frames);
        const S32 frame_count = (S32)llmin(info.frame_count, cap);
        const bool more_frames = info.frame_count > cap;
        const bool animated = frame_count > 1;
        out.loops = info.loop_count;   // times to play, 0 = forever

        const OutSize size = animated
                             ? fit_size(width, height, params.anim_box_w, params.anim_box_h, params.pot_max)
                             : fit_size(width, height, params.still_box_w, params.still_box_h, params.pot_max);

        // Frame budget (as GIF): stored bytes, compositing work, frame cap.
        const U64 frame_bytes = (U64)size.out_w * (U64)size.out_h * 4;
        const U64 canvas_pixels = (U64)width * (U64)height;
        S32 keep = frame_count;
        keep = (S32)llmin((U64)keep, llmax((U64)1, params.max_frame_bytes / llmax((U64)1, frame_bytes)));
        keep = (S32)llmin((U64)keep, llmax((U64)1, LLChatImageGif::MAX_WORK_PIXELS / canvas_pixels));
        out.truncated = more_frames || keep < frame_count;

        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds((S64)(LLChatImageGif::MAX_DECODE_SECONDS * 1000.0));
        out.frames.reserve((size_t)keep);
        out.delays.reserve((size_t)keep);
        for (S32 i = 0; i < keep; ++i)
        {
            uint8_t* canvas = nullptr;
            int timestamp = 0;
            if (!WebPAnimDecoderHasMoreFrames(dec.get()) ||
                !WebPAnimDecoderGetNext(dec.get(), &canvas, &timestamp) || !canvas)
            {
                out.truncated = true;   // a broken frame: play what we have
                break;
            }
            // The frame's own duration (the decoder's timestamps are cumulative).
            int duration_ms = 0;
            WebPIterator iter;
            if (WebPDemuxGetFrame(demux, i + 1, &iter))
            {
                duration_ms = iter.duration;
                WebPDemuxReleaseIterator(&iter);
            }

            LLPointer<LLImageRaw> raw = make_frame(canvas, width, height, size, out.reason);
            if (raw.isNull())
            {
                break;
            }
            out.frames.push_back(raw);
            out.delays.push_back(delay_seconds(duration_ms));

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
}

// static
bool LLChatImageWebP::headerDims(const U8* data, size_t len, U32& width, U32& height, bool& animated)
{
    animated = false;
    if (!data || len < 12 || memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WEBP", 4) != 0)
    {
        return false;
    }
    // Reads the RIFF / VP8X / VP8 / VP8L headers only (bounded, no allocation).
    WebPBitstreamFeatures features;
    if (WebPGetFeatures(data, len, &features) != VP8_STATUS_OK || features.width <= 0 || features.height <= 0)
    {
        return false;
    }
    width = (U32)features.width;
    height = (U32)features.height;
    animated = features.has_animation != 0;
    return true;
}

void LLChatImageWebP::decode(const U8* data, size_t len, const LLChatImageGif::Params& params,
                             LLChatImageGif::Result& out)
{
    out = LLChatImageGif::Result();
    U32 width = 0, height = 0;
    bool animated = false;
    if (!headerDims(data, len, width, height, animated))
    {
        out.reason = "not a valid image";
        return;
    }
    // Size limit before any decoder state sized by the file exists.
    if (width > (U32)params.max_dim || height > (U32)params.max_dim)
    {
        out.reason = llformat("too large (%u x %u)", width, height);
        return;
    }
    out.orig_w = (S32)width;
    out.orig_h = (S32)height;
    if (animated)
    {
        decode_animated(data, len, width, height, params, out);
    }
    else
    {
        decode_still(data, len, width, height, params, out);
    }
    if (!out.ok)
    {
        out.frames.clear();
        out.delays.clear();
    }
}
