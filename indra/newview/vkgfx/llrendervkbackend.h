/**
 * @file llrendervkbackend.h
 * @brief Concrete LLRenderBackend that drives the proven LLRenderVK facade (gVK).
 *
 * EXPERIMENTAL Vulkan integration. This lives in newview (it needs the facade,
 * which pulls in Vulkan) and is installed into LLRender via LLRender::setBackend()
 * when RenderUseVulkan is set. It translates LLRender's enum VALUES to the facade's
 * llrvk enums at the boundary and owns the LLTextureData -> VK texture resolution
 * (upload-on-first-bind, cache by LLTextureData*, re-upload on generation change).
 *
 * See indra/llrender/llrenderbackend.h.
 */
#ifndef LL_LLRENDERVKBACKEND_H
#define LL_LLRENDERVKBACKEND_H

#include "llrenderbackend.h"

#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

class LLTextureData;
class LLUUID;

class LLRenderVKBackend : public LLRenderBackend
{
public:
    LLRenderVKBackend();
    ~LLRenderVKBackend() override;
    // UI texture cache budget (MB); set each frame from RenderVKUITexCacheMB.
    static U32 sUICacheBudgetMB;
    // Idle release (#405): entries not drawn for this many seconds release their VK
    // image (re-uploaded from the CPU copy when drawn again); 0 = budget only. Set each
    // frame from RenderVKUITexIdleSec.
    static U32 sUIIdleEvictSec;
    // VRAM PRESSURE HOOK (#405, not wired into the ladder: the ladder owner decides
    // where it goes). 0 = off; 1 = soft: half the budget, a quarter of the idle window
    // (>= 2 s); 2 = release everything not drawn in the last two frames. Soft and
    // temporary -- settings untouched, the normal limits return when set back to 0.
    // Visual cost: none (what is on screen is never released); re-upload cost when a
    // released image is drawn again.
    static void setVramPressure(U32 level) { sPressure = level; }
    static U32 vramPressure() { return sPressure; }
    // UI-cache VRAM (logical bytes, all kinds) -- what pressure can release at most.
    static U64 cacheBytes() { return sCacheBytesPublished; }

    // frame lifecycle
    void beginFrame(U32 width, U32 height) override;
    void endFrame() override;

    // immediate-mode geometry
    void begin(U32 mode) override;
    void end() override;
    void vertex3f(F32 x, F32 y, F32 z) override;
    void texCoord2f(F32 u, F32 v) override;
    void color4ub(U8 r, U8 g, U8 b, U8 a) override;
    void color4f(F32 r, F32 g, F32 b, F32 a) override;
    void vertexBatchPreTransformed(const F32* pos, const F32* uv,
                                   const U8* col, S32 count) override;

    // matrix stack
    void matrixMode(U32 mode) override;
    void pushMatrix() override;
    void popMatrix() override;
    void loadIdentity() override;
    void loadMatrix(const F32* m16) override;
    void multMatrix(const F32* m16) override;
    void translatef(F32 x, F32 y, F32 z) override;
    void scalef(F32 x, F32 y, F32 z) override;
    void rotatef(F32 deg, F32 x, F32 y, F32 z) override;
    void ortho(F32 l, F32 r, F32 b, F32 t, F32 zn, F32 zf) override;

    // render state
    void blendFunc(U32 sfactor, U32 dfactor) override;
    void setSceneBlendType(U32 type) override;
    void setColorMask(bool writeColor, bool writeAlpha) override;
    void setColorMask(bool r, bool g, bool b, bool a) override;
    void setLineWidth(F32 w) override;
    void flush() override;
    void setDepthTest(bool enabled, bool writeEnabled, U32 func) override;
    void setScissor(bool enable, S32 x, S32 y, S32 w, S32 h) override;

    // texture units
    void setActiveTextureUnit(U32 unit) override;
    void bindImageGL(LLTextureData* image) override;
    void unbindTexture() override;
    void setTextureFilter(U32 filter) override;
    void setTextureAddressMode(U32 mode) override;
    void setMinimumAlpha(F32 minimum) override;
    void setLabelAnchor(bool on, F32 x, F32 y, F32 z, F32 bias) override;

    // ---- #83: GPU render targets for LLViewerDynamicTexture -------------------
    // A target is a facade texture the dynamic texture renders into (offscreen
    // segment) and the UI / world sample directly -- no CPU readback, no upload of
    // the dynamic texture's blank CPU raw. Ids are facade texture ids (0 = none).
    static U32  createTarget(U32 width, U32 height, bool with_depth);
    static void releaseTarget(U32 target);
    // Open / close an offscreen segment on `target` (see LLRenderVK::beginOffscreen).
    // clear_rgba == nullptr keeps the previous contents.
    static bool beginTarget(U32 target, const F32* clear_rgba, bool clear_depth);
    static void endTarget();
    static bool inTarget();
    static void clearTargetDepth();   // glClear(GL_DEPTH_BUFFER_BIT) inside a target
    // Publish `image` (the dynamic texture's LLTextureData, UUID `id`) as rendered into
    // `target`, content generation `gen`: binds of `image` sample the target, and the
    // world can find it by UUID (llvkdyntex.h). detach on destruction.
    static void attachTarget(const LLTextureData* image, const LLUUID& id, U32 target, S32 gen);
    static void detachTarget(const LLTextureData* image, const LLUUID& id);

private:
    // Resolve an LLTextureData to a facade texture id, uploading its CPU pixels on
    // first bind and re-uploading when its VK generation changes. Returns 0
    // (white) for images with no CPU data (generic LLTextureData / UI icons).
    uint32_t resolveTexture(LLTextureData* image);

    struct CachedTex
    {
        uint32_t vkId = 0;
        S32 generation = -1;
        U32 width = 0, height = 0;
        U64 serial = 0;          // LLTextureData::getVKSerial() -- guards address reuse
        U64 bytes = 0;           // VK image size (w*h*bpp)
        U64 lastUsedFrame = 0;   // gVK.frameCounter() at last bind (LRU)
        F64 lastUsedSec = 0.0;   // wall time at last bind (#405: idle age)
        U8 kind = 0;             // LLTextureData::EVKUiKind at last bind (#405 breakdown)
        U8 bpp = 4;              // bytes per texel of the VK image
        U8 fmt = 0;              // llrvk::eTexelFormat of the VK image (#458: rect updates)
        bool cpu_gone = false;   // #707: the image's CPU copy was dropped -- this IS its UI copy
        U8 levels = 1;           // <GMDX> mip levels (use_mips art: a new generation recreates it)
    };
    std::unordered_map<const LLTextureData*, CachedTex> mTexCache;
    U64 mCacheBytes = 0;
    U64 mLastTickFrame = ~0ull;

    // #405: the UI line of the VKMem log (every 5 s from tickCache): the ui category
    // by kind (fonts, sprites, icons, thumbnails, map, media, ...), render targets, the
    // ring, page padding, the largest entries, and the cache traffic since the last line.
    void logMemory(F64 now);
    F64 mNextMemLog = 0.0;
    F64 mNextIdleSweep = 0.0;
    static U32 sPressure;
    static U64 sCacheBytesPublished;
    U32 mStatUploads = 0, mStatReuploads = 0, mStatPartial = 0;   // partial: dirty-rect re-uploads (#458)
    U64 mStatUploadBytes = 0;
    U32 mStatEvictDead = 0, mStatEvictIdle = 0, mStatEvictBudget = 0;

    // Release a cache entry's VK texture (deferred in the facade) and forget it.
    void dropEntry(std::unordered_map<const LLTextureData*, CachedTex>::iterator it);
    // Once per frame: drop entries of destroyed LLImageGLs, then LRU-evict down to
    // the RenderVKUITexCacheMB budget (never entries bound this frame).
    void tickCache();

    // ~LLTextureData (any thread) -> pending drops, drained on the main thread.
    static void onImageDestroyed(const LLTextureData* image, U64 serial);
    // Destroyed-image queue. Heap-allocated and never freed: LLImageGLs held by static
    // LLPointers (UI images) die during exit(), after ordinary statics are destroyed.
    struct DestroyedQueue
    {
        std::mutex mutex;
        std::vector<std::pair<const LLTextureData*, U64>> images;
    };
    static DestroyedQueue& destroyedQueue();

    // Image -> render target (#83), checked first by resolveTexture().
    static std::unordered_map<const LLTextureData*, U32>& targetsByImage();
};

#endif // LL_LLRENDERVKBACKEND_H
