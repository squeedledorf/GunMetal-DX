/**
 * @file llcleantexture.h
 * @brief Clean-pipeline texture bookkeeping: per-UUID capture metadata and the
 *        auto-mask alpha-gradient classification the ingest signatures use.
 *        Pixels are not held here; the Vulkan renderer uploads from
 *        LLTextureData's own CPU copy (getVKCpuData).
 */

#pragma once

#include "stdtypes.h"
#include "lluuid.h"

#include <atomic>

class LLImageRaw;
class LLViewerTexture;

class LLCleanTexture
{
public:
    static LLCleanTexture& instance();

    // Note that `id` was (re)decoded at `discard_level` (diagnostic counter only; no
    // per-UUID state is kept -- it was never read and grew without bound).
    void capture(const LLUUID& id, const LLImageRaw* raw, S32 discard_level);

    // Number of capture() calls this session (diagnostic counter).
    U64 captureCount() const;

    // Gradient-alpha percent (0-100) of a resolved texture, 255 = not analysed yet.
    // Reads the LLTextureData's committed async alpha analysis (vkPostAlphaAnalysis).
    U32 alphaGradientPct(const LLViewerTexture* tex) const;
    // Auto-mask gradient class against the LIVE RenderCleanAutoMaskMaxGradient:
    // -1 = not analysed yet, 1 = gradient low enough to mask, 0 = too much gradient.
    // Mixed into the ingest signatures so a landing analysis re-classifies faces.
    S32 alphaGradientClass(const LLViewerTexture* tex) const;

private:
    LLCleanTexture() = default;
    LLCleanTexture(const LLCleanTexture&) = delete;
    LLCleanTexture& operator=(const LLCleanTexture&) = delete;

    std::atomic<U64> mCaptures{0};
};
