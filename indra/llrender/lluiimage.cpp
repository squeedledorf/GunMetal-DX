/**
 * @file lluiimage.cpp
 * @brief UI implementation
 *
 * $LicenseInfo:firstyear=2007&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
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

// Utilities functions the user interface needs

//#include "llviewerprecompiledheaders.h"
#include "linden_common.h"

// Project includes
#include "lluiimage.h"

#include <algorithm>
#include <vector>

#include "llrender.h"
#include "lltimer.h"

LLUIImage::LLUIImage(const std::string& name, LLPointer<LLTexture> image)
:   mName(name),
    mImage(image),
    mScaleRegion(0.f, 1.f, 1.f, 0.f),
    mClipRegion(0.f, 1.f, 1.f, 0.f),
    mImageLoaded(NULL),
    mScaleStyle(SCALE_INNER),
    mCachedW(-1),
    mCachedH(-1)
{
    getTextureWidth();
    getTextureHeight();
}

LLUIImage::~LLUIImage()
{
    delete mImageLoaded;
    delete mVector;
}

// ---- #1056 vector icons ------------------------------------------------------------------

namespace
{
    // Rasters kept per icon. Shared images (a folder, an info "i", a close X) are drawn by many
    // widgets at their own sizes at once, and a cap below that count would re-rasterize the
    // overflow every frame; these are a few KB each, so the cap is generous.
    constexpr size_t MAX_VECTOR_SIZES_PER_IMAGE = 16;
    // Largest raster side (physical pixels). Icons are tiny; this only stops a stretched
    // draw rectangle from asking for a huge texture.
    constexpr S32 MAX_VECTOR_RASTER_SIDE = 1024;
    // A raster dropped from a cache may still be referenced by a draw already queued this
    // frame (the UI backend uploads / samples at flush time), so it lives on briefly.
    constexpr F64 VECTOR_RETIRE_SECONDS = 2.0;

    LLUIImage::vector_rasterizer_t sVectorRasterizer;
    U32 sVectorGeneration = 1;
    std::vector<std::pair<LLPointer<LLTexture>, F64> > sRetiredRasters;

    void purgeRetiredRasters()
    {
        if (sRetiredRasters.empty())
        {
            return;
        }
        const F64 now = LLTimer::getTotalSeconds();
        sRetiredRasters.erase(std::remove_if(sRetiredRasters.begin(), sRetiredRasters.end(),
                                             [now](const std::pair<LLPointer<LLTexture>, F64>& r)
                                             { return now - r.second > VECTOR_RETIRE_SECONDS; }),
                              sRetiredRasters.end());
    }

    void retireRaster(LLPointer<LLTexture>& tex)
    {
        if (tex.notNull())
        {
            sRetiredRasters.emplace_back(tex, (F64)LLTimer::getTotalSeconds());
            tex = nullptr;
        }
    }
}

struct LLUIImage::VectorData
{
    struct Entry
    {
        S32 mWidth;     // physical pixels
        S32 mHeight;
        LLPointer<LLTexture> mTexture;   // null: the rasterizer failed at this size, draw the bitmap
    };

    VectorSpec  mSpec;
    U32         mGeneration = 0;
    std::vector<Entry> mEntries;   // most recently used first
};

bool LLUIImage::sClassicIcons = false;

// static
void LLUIImage::setVectorRasterizer(const vector_rasterizer_t& rasterizer)
{
    sVectorRasterizer = rasterizer;
    invalidateVectorRasters();
}

// static
void LLUIImage::setClassicIcons(bool classic)
{
    if (sClassicIcons != classic)
    {
        sClassicIcons = classic;
        invalidateVectorRasters();
    }
}

// static
void LLUIImage::cleanUpVectorRasters()
{
    sRetiredRasters.clear();
    sVectorRasterizer = nullptr;
}

// static
void LLUIImage::invalidateVectorRasters()
{
    // Each image notices the new generation on its next vector draw and retires its rasters.
    ++sVectorGeneration;
}

void LLUIImage::setVectorSource(const VectorSpec& spec)
{
    if (spec.mFile.empty())
    {
        delete mVector;
        mVector = nullptr;
        return;
    }
    if (!mVector)
    {
        mVector = new VectorData;
    }
    mVector->mSpec = spec;
    for (VectorData::Entry& entry : mVector->mEntries)
    {
        retireRaster(entry.mTexture);
    }
    mVector->mEntries.clear();
}

LLTexture* LLUIImage::getVectorTexture(S32 width, S32 height) const
{
    if (sClassicIcons || !sVectorRasterizer || !mVector || width <= 0 || height <= 0)
    {
        return nullptr;
    }

    // physical pixels this draw will cover
    const LLVector3 ui_scale = gGL.getUIScale();
    const S32 pixel_w = llclamp(ll_round((F32)width * ui_scale.mV[VX]), 1, MAX_VECTOR_RASTER_SIDE);
    const S32 pixel_h = llclamp(ll_round((F32)height * ui_scale.mV[VY]), 1, MAX_VECTOR_RASTER_SIDE);

    VectorData& data = *mVector;
    if (data.mGeneration != sVectorGeneration)
    {
        for (VectorData::Entry& entry : data.mEntries)
        {
            retireRaster(entry.mTexture);
        }
        data.mEntries.clear();
        data.mGeneration = sVectorGeneration;
    }

    for (size_t i = 0; i < data.mEntries.size(); ++i)
    {
        VectorData::Entry& entry = data.mEntries[i];
        if (entry.mWidth == pixel_w && entry.mHeight == pixel_h)
        {
            if (i != 0)
            {
                std::swap(data.mEntries[0], entry);
            }
            return data.mEntries[0].mTexture.get();
        }
    }

    // First use of this icon at this pixel size (or first after a scale change): rasterize
    // once, keep it. Never reached on a steady-state frame.
    // (Log channel UIVectorIcons: this should go quiet a moment after the UI settles.)
    LL_DEBUGS("UIVectorIcons") << "rasterize " << mName << " (" << data.mSpec.mFile << ") " << pixel_w << "x" << pixel_h << LL_ENDL;
    purgeRetiredRasters();
    VectorData::Entry fresh;
    fresh.mWidth = pixel_w;
    fresh.mHeight = pixel_h;
    fresh.mTexture = sVectorRasterizer(data.mSpec, pixel_w, pixel_h);
    if (data.mEntries.size() >= MAX_VECTOR_SIZES_PER_IMAGE)
    {
        retireRaster(data.mEntries.back().mTexture);
        data.mEntries.pop_back();
    }
    data.mEntries.insert(data.mEntries.begin(), fresh);
    return data.mEntries[0].mTexture.get();
}

S32 LLUIImage::getWidth() const
{
    // return clipped dimensions of actual image area
    return ll_round((F32)mImage->getWidth(0) * mClipRegion.getWidth());
}

S32 LLUIImage::getHeight() const
{
    // return clipped dimensions of actual image area
    return ll_round((F32)mImage->getHeight(0) * mClipRegion.getHeight());
}

void LLUIImage::draw3D(const LLVector3& origin_agent, const LLVector3& x_axis, const LLVector3& y_axis,
                        const LLRect& rect, const LLColor4& color)
{
    F32 border_scale = 1.f;
    F32 border_height = (1.f - mScaleRegion.getHeight()) * getHeight();
    F32 border_width = (1.f - mScaleRegion.getWidth()) * getWidth();
    if (rect.getHeight() < border_height || rect.getWidth() < border_width)
    {
         if(border_height - rect.getHeight() > border_width - rect.getWidth())
         {
             border_scale = (F32)rect.getHeight() / border_height;
         }
         else
         {
            border_scale = (F32)rect.getWidth() / border_width;
         }
    }

    LLRender2D::pushMatrix();
    {
        LLVector3 rect_origin = origin_agent + ((F32)rect.mLeft * x_axis) + ((F32)rect.mBottom * y_axis);
        LLRender2D::translate(rect_origin.mV[VX],
                                            rect_origin.mV[VY],
                                            rect_origin.mV[VZ]);
        gGL.getTexUnit(0)->bind(getImage());
        gGL.color4fv(color.mV);

        LLRectf center_uv_rect(mClipRegion.mLeft + mScaleRegion.mLeft * mClipRegion.getWidth(),
                            mClipRegion.mBottom + mScaleRegion.mTop * mClipRegion.getHeight(),
                            mClipRegion.mLeft + mScaleRegion.mRight * mClipRegion.getWidth(),
                            mClipRegion.mBottom + mScaleRegion.mBottom * mClipRegion.getHeight());
        gl_segmented_rect_3d_tex(mClipRegion,
                                center_uv_rect,
                                LLRectf(border_width * border_scale * 0.5f / (F32)rect.getWidth(),
                                        (rect.getHeight() - (border_height * border_scale * 0.5f)) / (F32)rect.getHeight(),
                                        (rect.getWidth() - (border_width * border_scale * 0.5f)) / (F32)rect.getWidth(),
                                        (border_height * border_scale * 0.5f) / (F32)rect.getHeight()),
                                (F32)rect.getWidth() * x_axis,
                                (F32)rect.getHeight() * y_axis);

    } LLRender2D::popMatrix();
}

//#include "lluiimage.inl"

boost::signals2::connection LLUIImage::addLoadedCallback( const image_loaded_signal_t::slot_type& cb )
{
    if (!mImageLoaded)
    {
        mImageLoaded = new image_loaded_signal_t();
    }
    return mImageLoaded->connect(cb);
}


void LLUIImage::onImageLoaded()
{
    if (mImageLoaded)
    {
        (*mImageLoaded)();
    }
}

namespace LLInitParam
{
    void ParamValue<LLUIImage*>::updateValueFromBlock()
    {
        // The keyword "none" is specifically requesting a null image
        // do not default to current value. Used to overwrite template images.
        if (name() == "none")
        {
            updateValue(NULL);
            return;
        }

        LLUIImage* imagep =  LLRender2D::getInstance()->getUIImage(name());
        if (imagep)
        {
            updateValue(imagep);
        }
    }

    void ParamValue<LLUIImage*>::updateBlockFromValue(bool make_block_authoritative)
    {
        if (getValue() == NULL)
        {
            name.set("none", make_block_authoritative);
        }
        else
        {
            name.set(getValue()->getName(), make_block_authoritative);
        }
    }


    bool ParamCompare<LLUIImage*, false>::equals(
        LLUIImage* const &a,
        LLUIImage* const &b)
    {
        // force all LLUIImages for XML UI export to be "non-default"
        if (!a && !b)
            return false;
        else
            return (a == b);
    }
}

