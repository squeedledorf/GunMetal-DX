/**
 * @file llimagesvg.h
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

#ifndef LL_LLIMAGESVG_H
#define LL_LLIMAGESVG_H

#include "llpointer.h"
#include "llrefcount.h"
#include "v4coloru.h"
#include <string>

class LLImageRaw;

// A parsed SVG document (NanoSVG, zlib licence; indra/llimage/nanosvg). Parse once, then
// rasterize it at as many pixel sizes as the UI needs. Not thread safe: one document per
// thread, or serialize the calls.
class LLImageSVG : public LLRefCount
{
public:
    // Parse SVG source text. Returns null when the text is not a usable SVG.
    static LLPointer<LLImageSVG> parse(const std::string& svg_text);

    // The document's own size (its viewBox / width / height) in SVG user units.
    F32 getWidth() const;
    F32 getHeight() const;

    // Rasterize into a width x height RGBA image (4 components, straight alpha). The
    // artwork's ink (the bounds of its shapes, not its padded viewBox) is scaled uniformly to
    // fit inside `box` -- { left, top, right, bottom } as fractions of the image, origin
    // top-left -- and centred in it, so a glyph asked for a differently shaped box is never
    // distorted. Every pixel's RGB is `color` (the icon is a mask); the alpha is the
    // artwork's coverage times color.a / 255, so the result tints and filters cleanly.
    // Returns null on a bad size or an allocation failure.
    LLPointer<LLImageRaw> rasterize(S32 width, S32 height, const LLColor4U& color, const F32 box[4]) const;

protected:
    LLImageSVG(void* nsvg_image);
    ~LLImageSVG() override;

private:
    void* mImage;   // NSVGimage*
    F32   mInk[4];  // union of the shapes' bounds (SVG user units): min x, min y, max x, max y
};

#endif // LL_LLIMAGESVG_H
