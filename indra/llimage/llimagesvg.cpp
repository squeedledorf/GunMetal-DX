/**
 * @file llimagesvg.cpp
 * @brief Vector (SVG) icon rasterizer for the UI (#1056).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Linden Research, Inc.
 *
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
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "linden_common.h"

#include "llimagesvg.h"
#include "llimage.h"

// NanoSVG is third-party C written for -Wall-clean-ish builds, not for -Werror ones.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

// The one NanoSVG implementation in the viewer (the prebuilt nanosvg package); llrender's
// SVG font glyphs use these symbols too.
#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include <nanosvg/nanosvg.h>
#include <nanosvg/nanosvgrast.h>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace
{
    // Largest side we agree to rasterize: far beyond any UI icon, stops a bad caller from
    // asking for a gigabyte.
    constexpr S32 MAX_SVG_RASTER_SIDE = 2048;
}

LLImageSVG::LLImageSVG(void* nsvg_image)
:   mImage(nsvg_image)
{
    // Ink bounds: Phosphor pads every glyph inside its 256 box by a different amount, so
    // placing by the viewBox would size icons unevenly. Fall back to the viewBox for an
    // image with no shapes.
    const NSVGimage* image = static_cast<const NSVGimage*>(mImage);
    bool any = false;
    mInk[0] = mInk[1] = 0.f;
    mInk[2] = image->width;
    mInk[3] = image->height;
    for (const NSVGshape* shape = image->shapes; shape; shape = shape->next)
    {
        if (!(shape->flags & NSVG_FLAGS_VISIBLE))
        {
            continue;
        }
        if (!any)
        {
            mInk[0] = shape->bounds[0];
            mInk[1] = shape->bounds[1];
            mInk[2] = shape->bounds[2];
            mInk[3] = shape->bounds[3];
            any = true;
        }
        else
        {
            mInk[0] = llmin(mInk[0], shape->bounds[0]);
            mInk[1] = llmin(mInk[1], shape->bounds[1]);
            mInk[2] = llmax(mInk[2], shape->bounds[2]);
            mInk[3] = llmax(mInk[3], shape->bounds[3]);
        }
    }
    if (mInk[2] - mInk[0] <= 0.f || mInk[3] - mInk[1] <= 0.f)
    {
        mInk[0] = mInk[1] = 0.f;
        mInk[2] = image->width;
        mInk[3] = image->height;
    }
}

LLImageSVG::~LLImageSVG()
{
    if (mImage)
    {
        nsvgDelete(static_cast<NSVGimage*>(mImage));
    }
}

// static
LLPointer<LLImageSVG> LLImageSVG::parse(const std::string& svg_text)
{
    if (svg_text.empty())
    {
        return nullptr;
    }
    // nsvgParse() edits its input in place, so it gets its own NUL-terminated copy.
    std::vector<char> buf(svg_text.begin(), svg_text.end());
    buf.push_back('\0');
    NSVGimage* image = nsvgParse(buf.data(), "px", 96.f);
    if (!image)
    {
        return nullptr;
    }
    if (image->width <= 0.f || image->height <= 0.f)
    {
        nsvgDelete(image);
        return nullptr;
    }
    return new LLImageSVG(image);
}

F32 LLImageSVG::getWidth() const
{
    return mImage ? static_cast<NSVGimage*>(mImage)->width : 0.f;
}

F32 LLImageSVG::getHeight() const
{
    return mImage ? static_cast<NSVGimage*>(mImage)->height : 0.f;
}

LLPointer<LLImageRaw> LLImageSVG::rasterize(S32 width, S32 height, const LLColor4U& color, const F32 box[4]) const
{
    if (!mImage || width <= 0 || height <= 0 || width > MAX_SVG_RASTER_SIDE || height > MAX_SVG_RASTER_SIDE)
    {
        return nullptr;
    }
    NSVGimage* image = static_cast<NSVGimage*>(mImage);

    LLPointer<LLImageRaw> raw = new LLImageRaw((U16)width, (U16)height, 4);
    U8* data = raw->getData();
    if (!data)
    {
        return nullptr;
    }

    NSVGrasterizer* rast = nsvgCreateRasterizer();
    if (!rast)
    {
        return nullptr;
    }
    // Target rectangle for the ink (pixels, origin top-left); a degenerate box means "all of it".
    F32 bx0 = llclamp(box[0], 0.f, 1.f), by0 = llclamp(box[1], 0.f, 1.f);
    F32 bx1 = llclamp(box[2], 0.f, 1.f), by1 = llclamp(box[3], 0.f, 1.f);
    if (bx1 - bx0 <= 0.f || by1 - by0 <= 0.f)
    {
        bx0 = by0 = 0.f;
        bx1 = by1 = 1.f;
    }
    const F32 target_w = (bx1 - bx0) * (F32)width;
    const F32 target_h = (by1 - by0) * (F32)height;
    const F32 ink_w = mInk[2] - mInk[0];
    const F32 ink_h = mInk[3] - mInk[1];

    // Uniform scale, ink centred in the target: a square glyph in a non-square box keeps its shape.
    const F32 scale = llmin(target_w / ink_w, target_h / ink_h);
    const F32 tx = bx0 * (F32)width + (target_w - ink_w * scale) * 0.5f - mInk[0] * scale;
    const F32 ty = by0 * (F32)height + (target_h - ink_h * scale) * 0.5f - mInk[1] * scale;
    nsvgRasterize(rast, image, tx, ty, scale, data, width, height, width * 4);
    nsvgDeleteRasterizer(rast);

    // The icon is a mask: keep the coverage as alpha (scaled by the baked opacity) and give
    // every texel the tint colour, so bilinear sampling never fringes toward black at the edge.
    const U32 count = (U32)width * (U32)height;
    const U32 opacity = color.mV[VALPHA];
    for (U32 i = 0; i < count; ++i)
    {
        U8* px = data + (size_t)i * 4;
        px[0] = color.mV[VRED];
        px[1] = color.mV[VGREEN];
        px[2] = color.mV[VBLUE];
        if (opacity != 255)
        {
            px[3] = (U8)((px[3] * opacity + 127) / 255);
        }
    }
    // NanoSVG writes rows top-down; LLImageRaw (like the decoded PNGs it replaces) is bottom-up.
    raw->verticalFlip();
    return raw;
}
