/**
 * @file lltexturedata.h
 * @brief Texture image: CPU pixel copy + metadata the Vulkan backend uploads from
 *
 * $LicenseInfo:firstyear=2001&license=viewerlgpl$
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


#ifndef LL_LLTEXTUREDATA_H
#define LL_LLTEXTUREDATA_H

#include "llimage.h"

#include "llpixelformat.h"
#include "llpointer.h"
#include "llrefcount.h"
#include "v2math.h"
#include "llunits.h"
#include "llthreadsafequeue.h"
#include "llrender.h"
#include "threadpool.h"
#include "workqueue.h"
#include <vector>
#include <atomic>

#define BYTES_TO_MEGA_BYTES(x) ((x) >> 20)
#define MEGA_BYTES_TO_BYTES(x) ((x) << 20)


//============================================================================
// A texture's CPU-side record (formerly LLImageGL). There is no GPU object here: it
// keeps a CPU copy of the pixels (setVKCpuData / the owned copy made by setFromRaw),
// the size / pixel format / discard-level bookkeeping the texture fetcher reads, the
// generation + serial the Vulkan backend keys its uploads on, the alpha analysis and
// the pick alpha map. The Vulkan backend uploads from the CPU copy on first bind and
// re-uploads when the generation changes.
class LLTextureData : public LLRefCount
{
    friend class LLTexUnit;
public:

    // call once per frame
    static void updateClass();

    // Size calculation
    static S32 dataFormatBits(LLPixelFormat dataformat);
    static S64 dataFormatBytes(LLPixelFormat dataformat, S32 width, S32 height);
    static S32 dataFormatComponents(LLPixelFormat dataformat);

    F32 getTimePassedSinceLastBound();
    void forceUpdateBindStats(void) const;

    // needs to be called every frame
    static void updateStats(F32 current_time);

    static bool checkSize(S32 width, S32 height);

public:
    LLTextureData(bool usemipmaps = true, bool allow_compression = true);
    LLTextureData(U32 width, U32 height, U8 components, bool usemipmaps = true, bool allow_compression = true);
    LLTextureData(const LLImageRaw* imageraw, bool usemipmaps = true, bool allow_compression = true);

protected:
    virtual ~LLTextureData();

    void analyzeAlpha(const void* data_in, U32 w, U32 h);
    void calcAlphaChannelOffsetAndStride();

public:
    virtual void dump();    // debugging info to LL_INFOS()

    bool setSize(S32 width, S32 height, S32 ncomponents, S32 discard_level = -1);
    void setComponents(S32 ncomponents) { mComponents = (S8)ncomponents ;}

    // Take this image's pixels from `imageraw`: records size/format/discard, keeps an
    // owned copy of the pixels for the VK backend and runs the alpha analysis.
    // defer_copy: record the size/format only; the pixels arrive through setSubImage.
    bool setFromRaw(S32 discard_level, const LLImageRaw* imageraw, bool defer_copy = false);

    // Write a sub-rect into the owned CPU pixels (web media / dynamic textures) and bump
    // the generation so the backend re-uploads it.
    bool setSubImage(const LLImageRaw* imageraw, S32 x_pos, S32 y_pos, S32 width, S32 height);
    bool setSubImage(const U8* datap, S32 data_width, S32 data_height, S32 x_pos, S32 y_pos, S32 width, S32 height);

    // Drop the current discard level so the fetcher re-requests the image.
    void invalidateDiscard();

    void setExplicitFormat(LLPixelFormat internal_format, LLPixelFormat primary_format, LLPixelType type_format = LLPixelType::NONE, bool swap_bytes = false);
    void setComponents(S8 ncomponents) { mComponents = ncomponents; }

    S32  getDiscardLevel() const        { return mCurrentDiscardLevel; }
    S32  getMaxDiscardLevel() const     { return mMaxDiscardLevel; }

    // override the current discard level
    // should only be used for local textures where you know exactly what you're doing
    void setDiscardLevel(S32 level) { mCurrentDiscardLevel = level; }

    S32  getCurrentWidth() const { return mWidth ;}
    S32  getCurrentHeight() const { return mHeight ;}
    S32  getWidth(S32 discard_level = -1) const;
    S32  getHeight(S32 discard_level = -1) const;
    U8   getComponents() const { return mComponents; }
    S64  getBytes(S32 discard_level = -1) const;
    bool getBoundRecently() const;
    bool isJustBound() const;
    bool getHasExplicitFormat() const { return mHasExplicitFormat; }
    LLPixelFormat getPrimaryFormat() const { return mFormatPrimary; }

    bool getIsAlphaMask() const;
    // VK (#82): percent (0-100) of texels with partial alpha (16 < a < 240) from the
    // async alpha analysis, 255 = no committed analysis yet. Feeds the clean
    // pipeline's auto-mask gradient test (RenderCleanAutoMaskMaxGradient).
    U8 getVKAlphaGradientPct() const { return mVKAlphaGradientPct; }
    // VK: an alpha analysis (mIsMask + gradient %) is committed only from a decode at
    // least this many texels on its short side, or from the full-resolution decode
    // (discard 0) of a smaller image. Lower mips are box-filtered: their alpha reads
    // softer than the texture really is, so they never decide.
    static constexpr U16 VK_ALPHA_COMMIT_MIN_SIDE = 64;
    // GPU-ready texture cache (#274): the committed alpha analysis travels with a cached
    // GPU image (get), and is restored when that image is served from disk while LL only
    // decodes a coarse level (commit; same rule as a fresh analysis: never over one
    // committed from a larger decode). side = short side of the deciding decode, 0 = none.
    void getVKAlphaFacts(bool& is_mask, U8& grad_pct, U16& side) const
    { is_mask = mIsMask; grad_pct = mVKAlphaGradientPct; side = mVKAlphaCommittedSide; }
    void commitVKAlphaFacts(bool is_mask, U8 grad_pct, U16 side);

    LLTexUnit::eTextureType getTarget(void) const { return LLTexUnit::TT_TEXTURE; }
    bool isContentReady(void) const { return mContentReady ; }
    void setContentReady (bool initialized) { mContentReady = initialized; }

    bool getUseMipMaps() const { return mUseMipMaps; }
    void setUseMipMaps(bool usemips) { mUseMipMaps = usemips; }
    // Sets the addressing mode used to sample the texture
    //  (such as wrapping, mirrored wrapping, and clamp)
    // Note: this actually gets set the next time the texture is bound.
    void setAddressMode(LLTexUnit::eTextureAddressMode mode);
    LLTexUnit::eTextureAddressMode getAddressMode(void) const { return mAddressMode; }

    void init(bool usemipmaps);
    virtual void cleanup(); // Clean up the LLTextureData so it can be reinitialized.  Be careful when using this in derived class destructors

    // CPU pixel source for the VK backend. For texture sources that keep a CPU copy
    // of their pixels alive (the font glyph atlas, whose LLImageRaw lives in
    // LLFontBitmapCache), the owner registers that raw + a generation stamp here so
    // the backend can upload/refresh a VK image.
    void setVKCpuData(LLImageRaw* raw, S32 generation)
    {
        mVKCpuData = raw;
        if (raw) mVKCpuDropped = false;
        mVKGeneration = generation;
        mVKDirtyGen = VK_DIRTY_UNKNOWN;   // any texel may have changed
    }
    LLImageRaw* getVKCpuData() const { return mVKCpuData; }

    // CPU COPY LEASES (#707). The decoded copy setFromRaw keeps is an upload source, not
    // a resident: the GPU image (and the GPU-ready disk cache / J2C behind it) is the
    // texture. A copy exists only while a consumer holds a lease (counted, by reason);
    // the owner's policy (LLViewerFetchedTexture::vkCpuCopyStep) drops an unleased copy
    // once the GPU holds the texture, and re-sources it by an async decode when someone
    // needs pixels again -- never a silent thumbnail. Reasons are tags for the stats
    // (VKTex "cpu copies" line, Preferences); the policy adds the implicit ones it can
    // see (pending upload, UI not uploaded, callbacks, appearance editing). Main thread.
    enum EVKLease : U8
    {
        VKLEASE_MEDIA = 0,   // writable buffer (web media / setSubImage): the copy IS the texture
        VKLEASE_LOCAL,       // not re-sourceable (created from / injected raw pixels, not fetched)
        VKLEASE_STREAM,      // the world renderer's GPU image does not cover the demand yet
        VKLEASE_UI,          // the VK UI has not uploaded this generation (or a skin sprite)
        VKLEASE_ALPHA,       // alpha classification of the fresh decode still running
        VKLEASE_CALLBACK,    // LL load callbacks waiting for raw pixels (Save As, thumbnails...)
        VKLEASE_PREVIEW,     // a preview / texture picker floater shows it (explicit RAII)
        VKLEASE_APPEARANCE,  // own-avatar local compositing while editing appearance
        VKLEASE_COUNT
    };
    static const char* vkLeaseName(U8 reason);
    // Explicit, counted leases (see LLVKCpuLease, the RAII handle).
    void acquireVKLease(U8 reason);
    void releaseVKLease(U8 reason);
    // Bit (1 << reason) per reason this image holds itself: explicit leases, MEDIA
    // (writable) and ALPHA (an analysis job in flight).
    U32  getVKLeaseMask() const;
    // Drop the owned decode copy (a lease-free, immutable copy only; writable / non-owned
    // sources and leased images refuse). The discard level keeps naming what the GPU holds;
    // isVKCpuDropped() tells readers the pixels exist only on the GPU / disk. Main thread.
    bool dropVKCpuData();
    // (#707) A GPU-rendered dynamic texture's blank placeholder raw (only its size
    // mattered): freed outright -- not a drop, nothing will re-source it.
    void releaseVKPlaceholder();
    bool isVKCpuDropped() const { return mVKCpuDropped; }
    // The VK UI backend found no pixels to upload (copy dropped and its UI image gone):
    // LLTexUnit::bind asks the owner for a re-source (LLTexture::vkRequestCpuCopy).
    void noteVKUiNeedsCpu() { mVKUiNeedsCpu = true; }
    bool takeVKUiNeedsCpu() { const bool b = mVKUiNeedsCpu; mVKUiNeedsCpu = false; return b; }
    // The VK UI uploaded this generation into its own cache (the UI no longer needs the copy).
    void noteVKUiUploaded(S32 gen) { mVKUiUploadedGen = gen; }
    // UI lease: drawn by the UI within the last few seconds and not uploaded at this generation.
    bool vkUiNeedsCpu() const;
    // Sampler address mode for this texture. The VK UI keeps it per unit, so
    // LLTexUnit records the request here and re-applies it whenever this image is
    // bound. CLAMP = the VK UI default.
    LLTexUnit::eTextureAddressMode mVKAddressMode = LLTexUnit::TAM_CLAMP;
    S32 getVKGeneration() const { return mVKGeneration; }
    // #458 DIRTY RECTANGLE: the union of the texels setSubImage() changed since a
    // consumer's last upload, so a per-frame media update re-uploads only what changed.
    // The consumer that uploaded at generation `from_gen` (and called resetVKDirty()
    // then) gets true + the rectangle (w or h 0: no texel changed) when every change
    // since went through setSubImage on the same buffer; false = upload in full (any
    // other change: a new CPU copy, setVKCpuData, a resize). One consumer: the VK UI
    // backend (the world's media path streams whole frames and ignores this).
    bool getVKDirtyRect(S32 from_gen, S32& x, S32& y, S32& w, S32& h) const;
    void resetVKDirty();
    // Unique per-object serial (never reused, unlike the address): the VK backend
    // keys its UI texture cache by (pointer, serial) so a new image allocated at a
    // freed image's address can never be served the old texture.
    U64 getVKSerial() const { return mVKSerial; }
    // Called from ~LLTextureData (any thread) so the VK backend can drop its cached
    // texture for this image. Registered by LLRenderVKBackend.
    static void (*sVKOnDestroy)(const LLTextureData* image, U64 serial);
    // Vulkan texture residency: swap the retained CPU copy for a smaller one (a box-
    // filtered downscale of it, see LLViewerFetchedTexture::applyVKDemand) and record
    // the coarser discard level it now holds, so the fetcher re-decodes from the cache
    // when the texture is needed sharper again. Main thread.
    void replaceVKCpuData(LLImageRaw* raw, S32 discard_level);
    bool isVKCpuWritable() const { return mVKCpuWritable; }
    // Drawn by the VK UI backend (which uploads from the CPU copy on a generation change):
    // stamps the draw time the UI lease reads (#707: vkUiNeedsCpu).
    void markVKUiUsed();
    bool isVKUiUsed() const { return mVKUiUsed; }
    // What kind of UI image this is (#405: the UI VRAM breakdown in the VKMem log, and
    // the UI cache's policy for glyph pages). Set by LLTexUnit::bind(LLTexture*) from
    // the owner (LLTexture::getVKUiKind: boost level / media) and by the font glyph
    // cache for its pages. Bookkeeping only.
    enum EVKUiKind : U8
    {
        VKUI_UNTAGGED = 0,   // bound as a bare LLTextureData by someone other than the font cache
        VKUI_FONT,           // glyph atlas page (LLFontBitmapCache)
        VKUI_FONT_COLOR,     // colour (emoji) glyph atlas page
        VKUI_SPRITE,         // skin images / UI sprites (BOOST_UI)
        VKUI_ICON,           // avatar / group / profile icons (BOOST_ICON)
        VKUI_THUMB,          // inventory thumbnails, texture / snapshot previews
        VKUI_MAP,            // world map / minimap tiles
        VKUI_MEDIA,          // web / media textures (LLViewerMediaTexture)
        VKUI_AVATAR,         // avatar bakes / self textures drawn by the UI
        VKUI_WORLD,          // any other (world) texture drawn by the UI (pickers, HUD)
        VKUI_KIND_COUNT
    };
    void setVKUiKind(U8 kind) { mVKUiKind = kind; }
    U8 getVKUiKind() const { return mVKUiKind; }
    // Bytes / count of CPU pixel copies retained for Vulkan (all LLImageGLs).
    static S64 getVKCpuBytes() { return sVKCpuBytes; }
    static S32 getVKCpuCount() { return sVKCpuCount; }
    // (#707 diagnostics) visit every image currently owning a CPU copy (main thread;
    // the registry is locked for the duration of the walk).
    static void vkForEachOwned(void (*fn)(const LLTextureData* image, S64 bytes, void* ctx), void* ctx);

    void setNeedsAlphaAndPickMask(bool need_mask);

public:
    mutable F32  mLastBindTime; // last time this was bound, by discard level

private:

    // Non-owning CPU pixel source + generation (see setVKCpuData).
    LLImageRaw* mVKCpuData = nullptr;
    S32 mVKGeneration = -1;
    // Dirty-rectangle chain (see getVKDirtyRect): valid while mVKDirtyGen == mVKGeneration
    // (every bump since mVKDirtyFromGen was a tracked one). Rectangle [x0,x1) x [y0,y1).
    static constexpr S32 VK_DIRTY_UNKNOWN = -0x7FFFFFFF;
    S32 mVKDirtyFromGen = VK_DIRTY_UNKNOWN;
    S32 mVKDirtyGen = VK_DIRTY_UNKNOWN;
    S32 mVKDirtyX0 = 0, mVKDirtyY0 = 0, mVKDirtyX1 = 0, mVKDirtyY1 = 0;
    U64 mVKSerial = nextVKSerial();
    static U64 nextVKSerial();
    // Owning retention of the CPU pixels: setFromRaw() keeps a copy of the source
    // LLImageRaw alive here so the backend can upload it on bind.
    LLPointer<LLImageRaw> mVKCpuDataOwned;
    bool mVKCpuWritable = false;   // mVKCpuDataOwned is exclusively ours (setSubImageVK)
    bool mVKUiUsed = false;        // drawn by the VK UI backend (see markVKUiUsed)
    U8 mVKUiKind = VKUI_UNTAGGED;  // see setVKUiKind
    // #707 leases (see EVKLease)
    U16 mVKLeases[VKLEASE_COUNT] = {};
    U8  mVKAlphaJobs = 0;          // alpha analysis jobs in flight (ALPHA lease)
    bool mVKCpuDropped = false;    // the owned copy was dropped (GPU / disk hold the pixels)
    bool mVKUiNeedsCpu = false;    // see noteVKUiNeedsCpu
    S32 mVKUiUploadedGen = -1;     // generation the VK UI last uploaded
    F32 mVKUiDrawTime = -1.e9f;    // last VK UI draw (LLFrameTimer seconds)
    // Every mVKCpuDataOwned assignment goes through here to keep the RAM counters true.
    void setVKOwned(LLImageRaw* raw);
    bool setSubImageVK(const U8* datap, S32 data_width, S32 data_height, S32 x_pos, S32 y_pos, S32 width, S32 height);
    // Alpha classification + pick mask of a freshly retained decode run on the
    // "General" pool and are applied back on the main loop (they were ~2% of the main
    // thread while streaming). false -> caller does them synchronously.
    bool vkPostAlphaAnalysis(S32 w, S32 h);
    // Alpha analysis committed so far (see getVKAlphaGradientPct). A result from a
    // decode SMALLER than the committed one is ignored, so a residency down-res or an
    // out-of-order low mip never flips a texture back from mask to blend.
    U8  mVKAlphaGradientPct = 255;
    U16 mVKAlphaCommittedSide = 0;   // short side of the committed decode, 0 = none
    bool mVKAlphaLatchedOff = false; // an alpha-less (RGB) decode switched the analysis off (#717)
    S64 mVKOwnedBytes = 0;
    static std::atomic<S64> sVKCpuBytes;
    static std::atomic<S32> sVKCpuCount;
    S8 mUseMipMaps;
    bool mHasExplicitFormat; // If false (default), format is f(mComponents)

    bool mIsMask;
    bool mNeedsAlphaAndPickMask;
    S8   mAlphaStride ;
    S8   mAlphaOffset ;

    bool     mContentReady ;
    U16      mWidth;
    U16      mHeight;
    S8       mCurrentDiscardLevel;

protected:
    S8 mComponents;
    S8 mMaxDiscardLevel;

    LLTexUnit::eTextureAddressMode      mAddressMode;   // Defaults to TAM_WRAP

    // Pixel format, as GL enum values (bookkeeping only: the VK backend maps them).
    LLPixelFormat mFormatInternal; // storage format (was GL internalformat)
    LLPixelFormat mFormatPrimary;  // pixel data format
    LLPixelType   mFormatType;

    // STATICS
public:
    static S32 sCount;
    static U32 sFrameCount;
    static F32 sLastFrameTime;

public:
    static void initClass(bool skip_analyze_alpha = false);

private:
    static bool sSkipAnalyzeAlpha;
//
//****************************************************************************************************
//The below for texture auditing use only
//****************************************************************************************************
private:
    S32 mCategory ;
public:
    void setCategory(S32 category) {mCategory = category;}
    S32  getCategory()const {return mCategory;}

    //for debug use: show texture size distribution
    //----------------------------------------
    static S32 sCurTexSizeBar ;
    static S32 sCurTexPickSize ;

    static void setCurTexSizebar(S32 index, bool set_pick_size = true) ;
    static void resetCurTexSizebar();

//****************************************************************************************************
//End of definitions for texture auditing use only
//****************************************************************************************************

};

// #707: an explicit CPU copy lease (RAII). While held, the image's decoded copy is never
// dropped; a copy already dropped is re-sourced when a reader asks (the holder's reader
// path does that). Movable, not copyable. Main thread.
class LLVKCpuLease
{
public:
    LLVKCpuLease() = default;
    LLVKCpuLease(LLTextureData* image, U8 reason) { hold(image, reason); }
    ~LLVKCpuLease() { reset(); }
    LLVKCpuLease(const LLVKCpuLease&) = delete;
    LLVKCpuLease& operator=(const LLVKCpuLease&) = delete;
    LLVKCpuLease(LLVKCpuLease&& o) noexcept : mImage(std::move(o.mImage)), mReason(o.mReason) { o.mImage = nullptr; }
    LLVKCpuLease& operator=(LLVKCpuLease&& o) noexcept
    {
        if (this != &o) { reset(); mImage = std::move(o.mImage); mReason = o.mReason; o.mImage = nullptr; }
        return *this;
    }
    // Lease `image` for `reason` (the previous one is released); same image + reason: no-op.
    void hold(LLTextureData* image, U8 reason)
    {
        if (image == mImage.get() && reason == mReason) return;
        reset();
        if (!image) return;
        mImage = image;
        mReason = reason;
        mImage->acquireVKLease(reason);
    }
    void reset()
    {
        if (mImage.notNull()) mImage->releaseVKLease(mReason);
        mImage = nullptr;
    }
    LLTextureData* get() const { return mImage.get(); }

private:
    LLPointer<LLTextureData> mImage;
    U8 mReason = LLTextureData::VKLEASE_PREVIEW;
};

#endif // LL_LLTEXTUREDATA_H
