/**
 * @file llvkdyntex.h
 * @brief #83: look up a GPU-rendered LLViewerDynamicTexture (avatar appearance
 *        composite) by UUID, for the Vulkan world renderer.
 *
 * While the appearance editor is open, BOM faces resolve their IMG_USE_BAKED_*
 * sentinel to the self avatar's local composite (LLViewerTexLayerSetBuffer, a
 * dynamic texture with a generated UUID). Under Vulkan the compositor renders into
 * a facade render target (LLRenderVKBackend::createTarget); the world renderer
 * gives it a streamed slot like live media and copies the target's image into it
 * GPU-side whenever `gen` moves (no CPU readback).
 */
#ifndef LL_LLVKDYNTEX_H
#define LL_LLVKDYNTEX_H

#include "vk_common.h"
#include "stdtypes.h"

class LLUUID;
class LLTextureData;

struct LLVKDynTexSource
{
    VkImage image = VK_NULL_HANDLE;   // R8G8B8A8_UNORM, SHADER_READ_ONLY_OPTIMAL between frames; null = not rendered yet
    uint32_t width = 0, height = 0;
    S32 gen = -1;                     // bumps on every re-render
    const LLTextureData* gl = nullptr;    // its LLTextureData (CPU raw = blank, slot creation size)
};

// True when `id` is a (Vulkan) dynamic texture -- registered from construction, so
// it is never mistaken for an asset. `image` is null until a recorded frame has
// rendered it.
bool llvkFindDynamicTexture(const LLUUID& id, LLVKDynTexSource& out);

// Record a copy of `src` into level 0 of `dst` (same size; UNORM -> UNORM/SRGB is a
// raw-bit copy, no gamma conversion) plus a blit mip cascade over `levels`, leaving
// every level of `dst` SHADER_READ_ONLY and `src` back in SHADER_READ_ONLY. `dst`'s
// previous contents are discarded (prior fragment reads are waited on).
void llvkRecordDynamicTextureCopy(VkCommandBuffer cmd, const LLVKDynTexSource& src,
                                  VkImage dst, uint32_t levels);

#endif // LL_LLVKDYNTEX_H
