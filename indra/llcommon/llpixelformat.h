/**
 * @file llpixelformat.h
 * @brief Renderer-neutral pixel format / pixel type / depth compare / render state enums.
 *
 * These replace the OpenGL constants the viewer used as data after the GL renderer
 * was removed (Vulkan is the only renderer). Names are our own so GL and Vulkan are
 * never confused.
 *
 * The numeric values are deliberately equal to the historical GL enum values:
 *   - LLPixelFormat / LLPixelType cross a process boundary as the "internalformat",
 *     "format" and "type" fields of the media plugin "texture_params" message
 *     (media_plugin_*.cpp -> LLPluginClassMedia -> LLViewerMedia), so the viewer and
 *     the plugin executables must agree. Treat the values as a WIRE FORMAT: never
 *     renumber an existing member.
 *   - The rest keeps the same numbers as the safest default for a mechanical change.
 *
 * Header-only, depends only on stdtypes so media plugins can include it.
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
 * $/LicenseInfo$
 */

#ifndef LL_LLPIXELFORMAT_H
#define LL_LLPIXELFORMAT_H

#include "stdtypes.h"

// Pixel data format (what used to be the GL "format") and image storage format
// (the GL "internalformat"). One enum covers both; NONE means "unset / derive".
// WIRE FORMAT: values are carried in media plugin messages, see file comment.
enum class LLPixelFormat : U32
{
    NONE                = 0,

    // pixel data formats
    COLOR_INDEX         = 0x1900,
    DEPTH_COMPONENT     = 0x1902,
    RED                 = 0x1903,
    ALPHA               = 0x1906,
    RGB                 = 0x1907,
    RGBA                = 0x1908,
    LUMINANCE           = 0x1909,
    LUMINANCE_ALPHA     = 0x190A,
    BGR                 = 0x80E0,
    BGRA                = 0x80E1,
    RG                  = 0x8227,
    SRGB                = 0x8C40,
    SRGB_ALPHA          = 0x8C42,

    // sized storage formats
    ALPHA8              = 0x803C,
    LUMINANCE8          = 0x8040,
    LUMINANCE8_ALPHA8   = 0x8045,
    RGB8                = 0x8051,
    RGBA8               = 0x8058,
    RGB10_A2            = 0x8059,
    RGBA16              = 0x805B,
    R8                  = 0x8229,
    RG8                 = 0x822B,
    R16F                = 0x822D,
    R32F                = 0x822E,
    RG16F               = 0x822F,
    RG32F               = 0x8230,
    DEPTH_COMPONENT24   = 0x81A6,
    R11F_G11F_B10F      = 0x8C3A,
    RGBA32F             = 0x8814,
    RGB32F              = 0x8815,
    RGBA16F             = 0x881A,
    RGB16F              = 0x881B,

    // generic compressed formats
    COMPRESSED_ALPHA            = 0x84E9,
    COMPRESSED_LUMINANCE        = 0x84EA,
    COMPRESSED_LUMINANCE_ALPHA  = 0x84EB,
    COMPRESSED_RED              = 0x8225,
    COMPRESSED_RG               = 0x8226,
    COMPRESSED_RGB              = 0x84ED,
    COMPRESSED_RGBA             = 0x84EE,
    COMPRESSED_SRGB             = 0x8C48,
    COMPRESSED_SRGB_ALPHA       = 0x8C49,

    // block compressed (S3TC) formats
    BC1_RGBA            = 0x83F1,   // DXT1
    BC2_RGBA            = 0x83F2,   // DXT3
    BC3_RGBA            = 0x83F3,   // DXT5
    BC1_SRGB_ALPHA      = 0x8C4D,
    BC2_SRGB_ALPHA      = 0x8C4E,
    BC3_SRGB_ALPHA      = 0x8C4F,
};

// Component type of the pixel data (the GL "type"). WIRE FORMAT as above.
enum class LLPixelType : U32
{
    NONE                = 0,        // unset: callers treat as UBYTE
    UBYTE               = 0x1401,
    USHORT              = 0x1403,
    UINT                = 0x1405,
    UINT_8888           = 0x8035,   // packed RGBA, 8 bits each
    UINT_8888_REV       = 0x8367,
};

// Depth comparison function.
enum class LLDepthFunc : U32
{
    NEVER               = 0x0200,
    LESS                = 0x0201,
    EQUAL               = 0x0202,
    LEQUAL              = 0x0203,
    GREATER             = 0x0204,
    NOTEQUAL            = 0x0205,
    GEQUAL              = 0x0206,
    ALWAYS              = 0x0207,
};

// Togglable render state scoped by LLRender::StateScope. NONE = no-op scope.
enum class LLRenderState : U32
{
    NONE                = 0,
    CULL_FACE           = 0x0B44,
    BLEND               = 0x0BE2,
    SCISSOR_TEST        = 0x0C11,
};

#endif // LL_LLPIXELFORMAT_H
