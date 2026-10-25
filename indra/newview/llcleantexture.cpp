/**
 * @file llcleantexture.cpp
 * @brief Implementation of LLCleanTexture.
 */

#include "llviewerprecompiledheaders.h"

#include "llcleantexture.h"

#include "llimage.h"
#include "lltexturedata.h"
#include "llviewercontrol.h"
#include "llviewertexture.h"

LLCleanTexture& LLCleanTexture::instance()
{
    static LLCleanTexture s;
    return s;
}

void LLCleanTexture::capture(const LLUUID& id, const LLImageRaw* raw, S32 discard_level)
{
    // Was a per-UUID metadata map that nothing read and nothing ever erased: one node per
    // texture UUID decoded in the session (unbounded under texture-cycling content). The
    // Vulkan renderer reads LLTextureData's own CPU copy; keep only a capture counter.
    (void)id; (void)discard_level;
    if (!raw || raw->getWidth() == 0 || raw->getHeight() == 0 || !raw->getData()) return;
    mCaptures.fetch_add(1, std::memory_order_relaxed);
}

U32 LLCleanTexture::alphaGradientPct(const LLViewerTexture* tex) const
{
    if (!tex) return 255u;
    const LLTextureData* gl = tex->getTextureData();
    return gl ? (U32)gl->getVKAlphaGradientPct() : 255u;
}

S32 LLCleanTexture::alphaGradientClass(const LLViewerTexture* tex) const
{
    const U32 pct = alphaGradientPct(tex);
    if (pct == 255u) return -1;
    // <SS:Skoomastream> Default 0 = off (LL parity: only canRenderAsMask's strict
    // mIsMask test demotes). pct is a rounded integer percent, so "pct <= 0" would still
    // pass textures with up to ~0.5% partial texels that LL's test may reject; at <= 0 an
    // analysed texture is always class 0 (stay blend), never class 1.
    static LLCachedControl<F32> s_max_grad(gSavedSettings, "RenderCleanAutoMaskMaxGradient", 0.f);
    if ((F32)s_max_grad <= 0.f) return 0;
    // </SS:Skoomastream>
    return pct <= (U32)llclamp((F32)s_max_grad * 100.f, 0.f, 100.f) ? 1 : 0;
}

U64 LLCleanTexture::captureCount() const
{
    return mCaptures.load(std::memory_order_relaxed);
}
