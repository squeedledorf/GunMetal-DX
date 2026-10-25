/**
 * @file llrendervkbackend.cpp
 * @brief See llrendervkbackend.h. Translates LLRender enum values -> llrvk and
 *        drives the LLRenderVK facade (gVK).
 */
#include "linden_common.h"

#include "llrendervkbackend.h"
#include <unordered_set>

#include "llrender.h"        // LLRender enum values (TRIANGLES, BF_*, BT_*, MM_*)
#include "lltexturedata.h"
#include "llimage.h"         // LLImageRaw

#include "llrendervk/llrendervk.h"   // the facade: gVK, llrvk::*
#include "llvkdyntex.h"              // #83: world lookup of GPU dynamic textures
#include "lluuid.h"
#include "llframetimer.h"

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <mutex>
#include <utility>
#include <vector>

using namespace llrvk;

namespace {

// LLRender::eGeomModes -> llrvk::eGeomMode. (LLRender values: TRIANGLES=0,
// TRIANGLE_STRIP=1, TRIANGLE_FAN=2, POINTS=3, LINES=4, LINE_STRIP=5, LINE_LOOP=6.)
eGeomMode geomMode(U32 m)
{
    switch (m)
    {
        case LLRender::TRIANGLES:      return TRIANGLES;
        case LLRender::TRIANGLE_STRIP: return TRIANGLE_STRIP;
        case LLRender::TRIANGLE_FAN:   return TRIANGLE_FAN;
        case LLRender::POINTS:         return eGeomMode::POINTS;   // qualified: wingdi.h has a POINTS struct
        case LLRender::LINES:          return LINES;
        case LLRender::LINE_STRIP:     return LINE_STRIP;
        case LLRender::LINE_LOOP:      return LINE_LOOP;
        default:                       return TRIANGLES;
    }
}

// LLRender::eBlendFactor -> llrvk::eBlendFactor (values match 1:1 for the subset
// the facade supports; BF_UNDEF etc. clamp to ONE).
eBlendFactor blendFactor(U32 f)
{
    switch (f)
    {
        case LLRender::BF_ONE:                    return BF_ONE;
        case LLRender::BF_ZERO:                   return BF_ZERO;
        case LLRender::BF_DEST_COLOR:             return BF_DEST_COLOR;
        case LLRender::BF_SOURCE_COLOR:           return BF_SOURCE_COLOR;
        case LLRender::BF_ONE_MINUS_DEST_COLOR:   return BF_ONE_MINUS_DEST_COLOR;
        case LLRender::BF_ONE_MINUS_SOURCE_COLOR: return BF_ONE_MINUS_SOURCE_COLOR;
        case LLRender::BF_DEST_ALPHA:             return BF_DEST_ALPHA;
        case LLRender::BF_SOURCE_ALPHA:           return BF_SOURCE_ALPHA;
        case LLRender::BF_ONE_MINUS_DEST_ALPHA:   return BF_ONE_MINUS_DEST_ALPHA;
        case LLRender::BF_ONE_MINUS_SOURCE_ALPHA: return BF_ONE_MINUS_SOURCE_ALPHA;
        default:                                  return BF_ONE;
    }
}

// LLRender::eCompareFunc -> llrvk::eCompareFunc.
eCompareFunc compareFunc(U32 f)
{
    switch (f)
    {
        case LLRender::CF_NEVER:         return CF_NEVER;
        case LLRender::CF_ALWAYS:        return CF_ALWAYS;
        case LLRender::CF_LESS:          return CF_LESS;
        case LLRender::CF_LESS_EQUAL:    return CF_LESS_EQUAL;
        case LLRender::CF_EQUAL:         return CF_EQUAL;
        case LLRender::CF_NOT_EQUAL:     return CF_NOT_EQUAL;
        case LLRender::CF_GREATER_EQUAL: return CF_GREATER_EQUAL;
        case LLRender::CF_GREATER:       return CF_GREATER;
        default:                         return CF_LESS_EQUAL;   // CF_DEFAULT
    }
}

// Matrix mode: LLRender MM_MODELVIEW=0, MM_PROJECTION=1, MM_TEXTURE0..3=2..5,
// MM_TEXTURE(alias)=7. Facade enum matches for 0..5; alias forwarded as MM_TEXTURE.
LLRenderVK::eMatrixMode matrixModeVK(U32 m)
{
    switch (m)
    {
        case LLRender::MM_MODELVIEW:  return LLRenderVK::MM_MODELVIEW;
        case LLRender::MM_PROJECTION: return LLRenderVK::MM_PROJECTION;
        case LLRender::MM_TEXTURE0:   return LLRenderVK::MM_TEXTURE0;
        case LLRender::MM_TEXTURE1:   return LLRenderVK::MM_TEXTURE1;
        case LLRender::MM_TEXTURE2:   return LLRenderVK::MM_TEXTURE2;
        case LLRender::MM_TEXTURE3:   return LLRenderVK::MM_TEXTURE3;
        case LLRender::MM_TEXTURE:    return LLRenderVK::MM_TEXTURE;   // alias
        default:                      return LLRenderVK::MM_MODELVIEW;
    }
}

// #405: an LLImageRaw for the VK UI in the smallest format that samples exactly like
// its RGBA8 expansion (see llrvk::eTexelFormat): 1- and 2-component images stay 1 / 2
// bytes per texel, a grayscale glyph page -- luminance left at the 255 it was cleared
// to, coverage in alpha -- keeps only the coverage (the one case that scans the image).
// 3- and 4-component images are expanded / copied to RGBA8. LLFontGL treats coverage
// as living in the alpha channel tinted by vertex color.
eTexelFormat chooseVKFormat(const U8* src, U32 w, U32 h, S8 comps, U8 kind, bool alpha_only)
{
    if (comps == 2)
    {
        if (kind == LLTextureData::VKUI_FONT)
        {
            const size_t n = (size_t)w * h;
            U8 lmin = 255;
            for (size_t i = 0; i < n && lmin == 255; ++i) lmin = std::min(lmin, src[i * 2 + 0]);
            if (lmin == 255) return TXF_COVERAGE8;
            // Not a plain coverage page after all: keep both channels.
        }
        return TXF_LA8;
    }
    if (comps == 1) return alpha_only ? TXF_A8 : TXF_L8;
    return TXF_RGBA8;
}

// Write the rectangle (x, y, rw, rh) of `src` (a w-texel-wide raw of `comps` components)
// in format `fmt` (from chooseVKFormat) to `dst`, rows tightly packed (#458: straight
// into upload staging, no intermediate buffer).
void packRectForVK(const U8* src, U32 w, S8 comps, eTexelFormat fmt,
                   U32 x, U32 y, U32 rw, U32 rh, U8* dst)
{
    const size_t sbpp = (size_t)comps;
    const size_t dbpp = texelBytes(fmt);
    for (U32 r = 0; r < rh; ++r)
    {
        const U8* s = src + ((size_t)(y + r) * w + x) * sbpp;
        U8* d = dst + (size_t)r * rw * dbpp;
        if (fmt == TXF_COVERAGE8)   // (L, A) -> A
        {
            for (U32 i = 0; i < rw; ++i) d[i] = s[i * 2 + 1];
        }
        else if (fmt != TXF_RGBA8 || comps == 4)   // same layout: L8 / A8 / LA8 / RGBA
        {
            std::memcpy(d, s, (size_t)rw * dbpp);
        }
        else if (comps == 3)
        {
            for (U32 i = 0; i < rw; ++i)
            {
                d[i * 4 + 0] = s[i * 3 + 0];
                d[i * 4 + 1] = s[i * 3 + 1];
                d[i * 4 + 2] = s[i * 3 + 2];
                d[i * 4 + 3] = 255;
            }
        }
        else if (comps == 2)   // (L, A) -> (L, L, L, A) (chooseVKFormat never asks this)
        {
            for (U32 i = 0; i < rw; ++i)
            {
                const U8 l = s[i * 2 + 0];
                d[i * 4 + 0] = l; d[i * 4 + 1] = l; d[i * 4 + 2] = l;
                d[i * 4 + 3] = s[i * 2 + 1];
            }
        }
        else   // 1 component -> opaque gray (chooseVKFormat never asks this)
        {
            for (U32 i = 0; i < rw; ++i)
            {
                d[i * 4 + 0] = d[i * 4 + 1] = d[i * 4 + 2] = s[i];
                d[i * 4 + 3] = 255;
            }
        }
    }
}

// #405: names for LLTextureData::EVKUiKind (the UI line of the VKMem log).
const char* uiKindName(U32 k)
{
    static const char* const kNames[LLTextureData::VKUI_KIND_COUNT] = {
        "untagged", "font", "font-color", "sprite", "icon", "thumb", "map", "media", "avatar", "world"};
    return k < (U32)LLTextureData::VKUI_KIND_COUNT ? kNames[k] : "?";
}

const char* bppName(U32 bpp)
{
    return bpp == 1 ? "r8" : bpp == 2 ? "rg8" : "rgba8";
}

} // namespace

U32 LLRenderVKBackend::sUICacheBudgetMB = 128;
U32 LLRenderVKBackend::sUIIdleEvictSec = 30;
U32 LLRenderVKBackend::sPressure = 0;
U64 LLRenderVKBackend::sCacheBytesPublished = 0;

// ---------------------------------------------------------------------------
// frame lifecycle
// ---------------------------------------------------------------------------
// static
LLRenderVKBackend::DestroyedQueue& LLRenderVKBackend::destroyedQueue()
{
    static DestroyedQueue* q = new DestroyedQueue();   // intentionally leaked (see header)
    return *q;
}

LLRenderVKBackend::LLRenderVKBackend()
{
    LLTextureData::sVKOnDestroy = &LLRenderVKBackend::onImageDestroyed;
}

LLRenderVKBackend::~LLRenderVKBackend()
{
    if (LLTextureData::sVKOnDestroy == &LLRenderVKBackend::onImageDestroyed)
        LLTextureData::sVKOnDestroy = nullptr;
}

// static -- may run on any thread (LLTextureData refs can drop off the main thread).
void LLRenderVKBackend::onImageDestroyed(const LLTextureData* image, U64 serial)
{
    DestroyedQueue& q = destroyedQueue();
    std::lock_guard<std::mutex> lk(q.mutex);
    q.images.emplace_back(image, serial);
}

void LLRenderVKBackend::dropEntry(std::unordered_map<const LLTextureData*, CachedTex>::iterator it)
{
    if (it->second.vkId) gVK.destroyTexture(it->second.vkId);   // deferred past in-flight frames
    mCacheBytes -= std::min(mCacheBytes, it->second.bytes);
    mTexCache.erase(it);
}

void LLRenderVKBackend::tickCache()
{
    const U64 frame = gVK.frameCounter();
    if (frame == mLastTickFrame) return;
    mLastTickFrame = frame;

    std::vector<std::pair<const LLTextureData*, U64>> dead;
    {
        DestroyedQueue& q = destroyedQueue();
        std::lock_guard<std::mutex> lk(q.mutex);
        dead.swap(q.images);
    }
    for (const auto& d : dead)
    {
        auto it = mTexCache.find(d.first);
        if (it != mTexCache.end() && it->second.serial == d.second)
        {
            dropEntry(it);
            ++mStatEvictDead;
        }
    }

    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (now >= mNextMemLog)
    {
        if (mNextMemLog > 0.0) logMemory(now);
        mNextMemLog = now + 5.0;
    }

    // UI texture budget: map tiles, previews, thumbnails, profile pictures each hold a
    // full-size VK image; without a cap they accumulated for the whole session.
    // sUICacheBudgetMB / sUIIdleEvictSec are fed from RenderVKUITexCacheMB /
    // RenderVKUITexIdleSec by the app (this library has no viewer-settings access).
    // VRAM pressure (setVramPressure) tightens both, temporarily.
    U64 budget = (U64)sUICacheBudgetMB << 20;
    F64 idle = (F64)sUIIdleEvictSec;
    if (sPressure == 1)
    {
        budget /= 2;
        idle = idle > 0.0 ? std::max(2.0, idle / 4.0) : 8.0;
    }
    else if (sPressure >= 2)
    {
        budget = 0;   // everything not drawn in the last two frames
    }

    // #405 IDLE RELEASE: without it the cache only shrank above the budget, so it sat
    // near the budget all session (closed floaters, scrolled-away thumbnails, the login
    // screen's images...). An entry not drawn for `idle` seconds releases its image; the
    // next draw re-uploads it from the CPU copy, queued and recorded before the UI pass of
    // the same frame -- no visible change. #707: an entry whose CPU copy was dropped is the
    // only copy the UI has: only the budget / VRAM pressure release it (a later draw then
    // re-sources the pixels by a decode).
    if (idle > 0.0 && now >= mNextIdleSweep)
    {
        mNextIdleSweep = now + 1.0;
        for (auto it = mTexCache.begin(); it != mTexCache.end();)
        {
            const CachedTex& t = it->second;
            if (now - t.lastUsedSec > idle && t.lastUsedFrame + 1 < frame && (!t.cpu_gone || sPressure > 0))
            {
                auto next = std::next(it);
                dropEntry(it);
                ++mStatEvictIdle;
                it = next;
            }
            else
            {
                ++it;
            }
        }
    }
    sCacheBytesPublished = mCacheBytes;
    if (mCacheBytes <= budget) return;
    std::vector<std::pair<U64, const LLTextureData*>> order;
    order.reserve(mTexCache.size());
    for (const auto& kv : mTexCache)
        if (kv.second.lastUsedFrame + 1 < frame)   // never evict what's bound this/last frame
            order.emplace_back(kv.second.lastUsedFrame, kv.first);
    std::sort(order.begin(), order.end());
    for (const auto& o : order)
    {
        if (mCacheBytes <= budget) break;
        auto it = mTexCache.find(o.second);
        if (it != mTexCache.end())
        {
            dropEntry(it);
            ++mStatEvictBudget;
        }
    }
    sCacheBytesPublished = mCacheBytes;
}

void LLRenderVKBackend::logMemory(F64 now)
{
    if (!gVK.isInitialized()) return;
    // One line, cheap (a walk of the UI cache + one tracker snapshot every 5 s). Bytes
    // are allocation sizes (what the ui category counts); page padding is estimated
    // from the driver granularity per allocation (the tracker's exact ui total is shown).
    // Textures suballocated from the texture pools (#411) have no padding of their own.
    const VkDeviceContext::MemStats ms = gVK.ctx().memStats();
    const U64 gran = ms.allocGranularity ? ms.allocGranularity : (64ull << 10);
    auto padOf = [gran](U64 b) -> U64 { return b ? (b + gran - 1) / gran * gran - b : 0; };
    auto mb = [](U64 b) -> F64 { return (F64)b / (1024.0 * 1024.0); };

    struct Sum { U32 n = 0; U64 bytes = 0; U64 pad = 0; };
    Sum kinds[LLTextureData::VKUI_KIND_COUNT];
    Sum cache;
    U32 pooled = 0;
    U32 by_bpp[5] = {};
    struct Big { U64 bytes; const CachedTex* t; };
    std::vector<Big> big;
    big.reserve(mTexCache.size());
    for (const auto& kv : mTexCache)
    {
        const CachedTex& t = kv.second;
        const U64 b = gVK.textureAllocBytes(t.vkId);
        const bool in_pool = gVK.texturePooled(t.vkId);
        pooled += in_pool ? 1u : 0u;
        const U64 pad = in_pool ? 0 : padOf(b);
        Sum& k = kinds[t.kind < (U8)LLTextureData::VKUI_KIND_COUNT ? t.kind : 0];
        ++k.n; k.bytes += b; k.pad += pad;
        ++cache.n; cache.bytes += b; cache.pad += pad;
        ++by_bpp[t.bpp <= 4 ? t.bpp : 4];
        big.push_back({b, &t});
    }
    const size_t top = std::min<size_t>(5, big.size());
    std::partial_sort(big.begin(), big.begin() + top, big.end(),
                      [](const Big& a, const Big& b) { return a.bytes > b.bytes; });

    LLRenderVK::MemStats fs;
    gVK.memStats(fs);
    const U64 other_bytes = fs.texBytes > cache.bytes ? fs.texBytes - cache.bytes : 0;
    const U32 other_n = fs.texCount > cache.n ? fs.texCount - cache.n : 0;

    std::ostringstream o;
    o << std::fixed << std::setprecision(1)
      << "UI VRAM: ui " << mb(ms.catDev[VKMEM_UI]) << "MB device (+pad " << mb(ms.catPad[VKMEM_UI]) << "MB, "
      << ms.catCount[VKMEM_UI] << " images; peak " << mb(ms.catPeak[VKMEM_UI]) << "MB) = cache " << cache.n << "/"
      << mb(cache.bytes) << "MB (+pad~" << mb(cache.pad) << ", " << pooled << " pooled) [";
    bool first = true;
    for (U32 k = 0; k < (U32)LLTextureData::VKUI_KIND_COUNT; ++k)
    {
        if (!kinds[k].n) continue;
        o << (first ? "" : ", ") << uiKindName(k) << " " << kinds[k].n << "/" << mb(kinds[k].bytes);
        first = false;
    }
    o << "] fmt r8 " << by_bpp[1] << " rg8 " << by_bpp[2] << " rgba8 " << by_bpp[4]
      << " + targets " << fs.rtCount << "/" << mb(fs.rtBytes) << "MB (+depth " << fs.rtDepthCount << "/"
      << mb(fs.rtDepthBytes) << ") + other " << other_n << "/" << mb(other_bytes) << "MB + pending-free "
      << fs.pendingCount << "/" << mb(fs.pendingBytes) << "MB"
      << " | host: ring " << mb(fs.ringBytes) << "MB staging " << mb(fs.stagingBytes) << "MB"
      << " upload-ring " << mb(fs.stageRingCap) << "MB (used " << mb(fs.stageRingUsed) << " peak "
      << mb(fs.stageRingPeak) << " last-frame " << mb(fs.stageFrameBytes) << "MB, fallbacks "
      << fs.stageFallbacks << " deferred " << fs.stageDeferred << ", uploads " << fs.stageUploads
      << " rect " << fs.stagePartial << ")"
      << " | budget " << sUICacheBudgetMB << "MB idle " << sUIIdleEvictSec << "s pressure " << sPressure
      << " | last " << std::setprecision(0) << (now - mNextMemLog + 5.0) << "s: uploads " << mStatUploads
      << " re-uploads " << mStatReuploads << " (rect " << mStatPartial << "; " << std::setprecision(1)
      << mb(mStatUploadBytes) << "MB), evicted dead "
      << mStatEvictDead << " idle " << mStatEvictIdle << " budget " << mStatEvictBudget << " | largest:";
    for (size_t i = 0; i < top; ++i)
    {
        const CachedTex& t = *big[i].t;
        o << (i ? "," : "") << " " << uiKindName(t.kind) << " " << t.width << "x" << t.height << " " << bppName(t.bpp)
          << " " << mb(big[i].bytes) << "MB idle " << std::setprecision(0) << (now - t.lastUsedSec) << "s"
          << std::setprecision(1);
    }
    LL_INFOS("VKMem") << o.str() << LL_ENDL;
    mStatUploads = mStatReuploads = mStatPartial = 0;
    mStatUploadBytes = 0;
    mStatEvictDead = mStatEvictIdle = mStatEvictBudget = 0;
}

void LLRenderVKBackend::beginFrame(U32 width, U32 height)
{
    // The concrete color view/format + swapchain record are owned by
    // LLVulkanContext; it calls gVK.beginFrameToView() itself around the UI pass.
    // This entry is kept for symmetry / future non-swapchain targets. No-op here.
    (void)width; (void)height;
}

void LLRenderVKBackend::endFrame()
{
    // Recording into the swapchain command buffer is driven by LLVulkanContext
    // (gVK.recordFrame(cmd)). No-op here.
}

// ---------------------------------------------------------------------------
// immediate-mode geometry
// ---------------------------------------------------------------------------
void LLRenderVKBackend::begin(U32 mode)          { gVK.begin(geomMode(mode)); }
void LLRenderVKBackend::end()                    { gVK.end(); }
void LLRenderVKBackend::vertex3f(F32 x, F32 y, F32 z) { gVK.vertex3f(x, y, z); }
void LLRenderVKBackend::texCoord2f(F32 u, F32 v) { gVK.texCoord2f(u, v); }
void LLRenderVKBackend::color4ub(U8 r, U8 g, U8 b, U8 a) { gVK.color4ub(r, g, b, a); }
void LLRenderVKBackend::color4f(F32 r, F32 g, F32 b, F32 a) { gVK.color4f(r, g, b, a); }

void LLRenderVKBackend::vertexBatchPreTransformed(const F32* pos, const F32* uv,
                                                  const U8* col, S32 count)
{
    // Pack the parallel flat arrays into the facade's PreVert form.
    std::vector<LLRenderVK::PreVert> verts((size_t)count);
    for (S32 i = 0; i < count; ++i)
    {
        LLRenderVK::PreVert& v = verts[i];
        v.x = pos[i * 3 + 0];
        v.y = pos[i * 3 + 1];
        v.z = pos[i * 3 + 2];
        v.u = uv[i * 2 + 0];
        v.v = uv[i * 2 + 1];
        v.r = col[i * 4 + 0];
        v.g = col[i * 4 + 1];
        v.b = col[i * 4 + 2];
        v.a = col[i * 4 + 3];
    }
    gVK.vertexBatchPreTransformed(verts.data(), count);
}

// ---------------------------------------------------------------------------
// matrix stack
// ---------------------------------------------------------------------------
void LLRenderVKBackend::matrixMode(U32 mode) { gVK.matrixMode(matrixModeVK(mode)); }
void LLRenderVKBackend::pushMatrix()         { gVK.pushMatrix(); }
void LLRenderVKBackend::popMatrix()          { gVK.popMatrix(); }
void LLRenderVKBackend::loadIdentity()       { gVK.loadIdentity(); }
void LLRenderVKBackend::loadMatrix(const F32* m16) { gVK.loadMatrix(m16); }
void LLRenderVKBackend::multMatrix(const F32* m16) { gVK.multMatrix(m16); }
void LLRenderVKBackend::translatef(F32 x, F32 y, F32 z) { gVK.translatef(x, y, z); }
void LLRenderVKBackend::scalef(F32 x, F32 y, F32 z)     { gVK.scalef(x, y, z); }
void LLRenderVKBackend::rotatef(F32 d, F32 x, F32 y, F32 z) { gVK.rotatef(d, x, y, z); }
void LLRenderVKBackend::ortho(F32 l, F32 r, F32 b, F32 t, F32 zn, F32 zf)
{
    gVK.ortho(l, r, b, t, zn, zf);
}

// ---------------------------------------------------------------------------
// render state
// ---------------------------------------------------------------------------
void LLRenderVKBackend::blendFunc(U32 s, U32 d)
{
    gVK.blendFunc(blendFactor(s), blendFactor(d));
}
void LLRenderVKBackend::setSceneBlendType(U32 type)
{
    if (gVK.inOffscreen())
    {
        // Render targets (#83): LL's exact factors -- the avatar compositor builds
        // the composite's alpha with BT_MULT_ALPHA / BT_ADD on the alpha channel.
        switch (type)
        {
            case LLRender::BT_ALPHA:          gVK.blendFunc(BF_SOURCE_ALPHA, BF_ONE_MINUS_SOURCE_ALPHA); return;
            case LLRender::BT_ADD:            gVK.blendFunc(BF_ONE, BF_ONE); return;
            case LLRender::BT_ADD_WITH_ALPHA: gVK.blendFunc(BF_SOURCE_ALPHA, BF_ONE); return;
            case LLRender::BT_MULT:           gVK.blendFunc(BF_DEST_COLOR, BF_ZERO); return;
            case LLRender::BT_MULT_ALPHA:     gVK.blendFunc(BF_DEST_ALPHA, BF_ZERO); return;
            case LLRender::BT_MULT_X2:        gVK.blendFunc(BF_DEST_COLOR, BF_SOURCE_COLOR); return;
            case LLRender::BT_REPLACE:        gVK.setSceneBlendType(BT_REPLACE); return;
            default:                          gVK.blendFunc(BF_SOURCE_ALPHA, BF_ONE_MINUS_SOURCE_ALPHA); return;
        }
    }
    // LLRender has more blend-type enums than the facade; map the common ones and
    // fall back to ALPHA. (BT_ALPHA=0, BT_ADD=1, BT_ADD_WITH_ALPHA=2, BT_MULT=3,
    // BT_MULT_ALPHA=4, BT_MULT_X2=5, BT_REPLACE=6.)
    switch (type)
    {
        case LLRender::BT_ALPHA:   gVK.setSceneBlendType(BT_ALPHA);   break;
        case LLRender::BT_ADD:
        case LLRender::BT_ADD_WITH_ALPHA: gVK.setSceneBlendType(BT_ADD); break;
        case LLRender::BT_MULT:
        case LLRender::BT_MULT_ALPHA:
        case LLRender::BT_MULT_X2: gVK.setSceneBlendType(BT_MULT);     break;
        case LLRender::BT_REPLACE: gVK.setSceneBlendType(BT_REPLACE);  break;
        default:                   gVK.setSceneBlendType(BT_ALPHA);    break;
    }
}
void LLRenderVKBackend::setColorMask(bool c, bool a) { gVK.setColorMask(c, a); }
void LLRenderVKBackend::setColorMask(bool r, bool g, bool b, bool a) { gVK.setColorMask(r, g, b, a); }
void LLRenderVKBackend::setLineWidth(F32 w) { gVK.setLineWidth(w); }
void LLRenderVKBackend::flush() { gVK.flush(); }
void LLRenderVKBackend::setDepthTest(bool enabled, bool writeEnabled, U32 func)
{
    gVK.setDepthTest(enabled, writeEnabled, compareFunc(func));
}
void LLRenderVKBackend::setScissor(bool enable, S32 x, S32 y, S32 w, S32 h)
{
    gVK.setScissor(enable, x, y, w, h);
}

// ---------------------------------------------------------------------------
// texture units
// ---------------------------------------------------------------------------
void LLRenderVKBackend::setActiveTextureUnit(U32 unit) { gVK.setActiveTextureUnit(unit); }

void LLRenderVKBackend::bindImageGL(LLTextureData* image)
{
    uint32_t id = resolveTexture(image);
    gVK.bindTexture(id);
}

void LLRenderVKBackend::unbindTexture() { gVK.bindTexture(0); } // 0 == white

void LLRenderVKBackend::setMinimumAlpha(F32 minimum) { gVK.setMinimumAlpha(minimum); }
void LLRenderVKBackend::setLabelAnchor(bool on, F32 x, F32 y, F32 z, F32 bias) { gVK.setLabelAnchor(on, x, y, z, bias); }

// ---------------------------------------------------------------------------
// #83: GPU render targets for LLViewerDynamicTexture
// ---------------------------------------------------------------------------
namespace
{
struct DynEntry
{
    const LLTextureData* image = nullptr;
    U32 target = 0;
    S32 gen = -1;
};
// Intentionally leaked (like destroyedQueue()): ~LLViewerDynamicTexture reaches this via
// detach/releaseTarget, and a dynamic texture can die during static destruction (#275).
std::unordered_map<LLUUID, DynEntry>& dynById()
{
    static std::unordered_map<LLUUID, DynEntry>* m = new std::unordered_map<LLUUID, DynEntry>();
    return *m;
}
} // namespace

// static
std::unordered_map<const LLTextureData*, U32>& LLRenderVKBackend::targetsByImage()
{
    // Intentionally leaked: see dynById().
    static std::unordered_map<const LLTextureData*, U32>* m = new std::unordered_map<const LLTextureData*, U32>();
    return *m;
}

// static
U32 LLRenderVKBackend::createTarget(U32 width, U32 height, bool with_depth)
{
    if (!gVK.isInitialized()) return 0;
    return gVK.createRenderTarget(width, height, with_depth);
}

// static
void LLRenderVKBackend::releaseTarget(U32 target)
{
    if (!target) return;
    auto& byImg = targetsByImage();
    for (auto it = byImg.begin(); it != byImg.end();)
        it = (it->second == target) ? byImg.erase(it) : std::next(it);
    auto& byId = dynById();
    for (auto it = byId.begin(); it != byId.end();)
        it = (it->second.target == target) ? byId.erase(it) : std::next(it);
    if (gVK.isInitialized()) gVK.destroyTexture(target);   // deferred past in-flight frames
}

// static
bool LLRenderVKBackend::beginTarget(U32 target, const F32* clear_rgba, bool clear_depth)
{
    if (!target || !gVK.isInitialized()) return false;
    return gVK.beginOffscreen(target, clear_rgba != nullptr, clear_rgba, clear_depth);
}

// static
void LLRenderVKBackend::endTarget()
{
    if (gVK.isInitialized()) gVK.endOffscreen();
}

// static
void LLRenderVKBackend::clearTargetDepth()
{
    if (gVK.isInitialized()) gVK.clearOffscreenDepth();
}

// static
bool LLRenderVKBackend::inTarget()
{
    return gVK.isInitialized() && gVK.inOffscreen();
}

// static
void LLRenderVKBackend::attachTarget(const LLTextureData* image, const LLUUID& id, U32 target, S32 gen)
{
    if (image) targetsByImage()[image] = target;   // target 0: not rendered yet (binds white)
    if (id.notNull())
    {
        DynEntry& e = dynById()[id];
        e.image = image; e.target = target; e.gen = gen;
    }
}

// static
void LLRenderVKBackend::detachTarget(const LLTextureData* image, const LLUUID& id)
{
    if (image) targetsByImage().erase(image);
    if (id.notNull()) dynById().erase(id);
}

bool llvkFindDynamicTexture(const LLUUID& id, LLVKDynTexSource& out)
{
    auto& byId = dynById();
    auto it = byId.find(id);
    if (it == byId.end()) return false;
    const LLTextureData* img = it->second.image;
    if (!img || img->getWidth() <= 0 || img->getHeight() <= 0) return false;   // (#707: size, not the placeholder raw)
    // Registered from construction: known but not rendered yet -> no image (the
    // world makes its slot from the blank raw and copies once the target is ready).
    out.image = (it->second.target && gVK.isInitialized() && gVK.isRenderTargetReady(it->second.target))
              ? gVK.textureImage(it->second.target) : VK_NULL_HANDLE;
    out.width = (uint32_t)img->getWidth();
    out.height = (uint32_t)img->getHeight();
    out.gen = it->second.gen;
    out.gl = it->second.image;
    return true;
}

void LLRenderVKBackend::setTextureFilter(U32 filter)
{
    gVK.setTextureFilter(filter == 0 ? TF_POINT : TF_BILINEAR);
}
void LLRenderVKBackend::setTextureAddressMode(U32 mode)
{
    // 0=wrap, 1=mirror, 2=clamp (matches llrvk::eTextureAddressMode order).
    eTextureAddressMode a = (mode == 0) ? TAM_WRAP
                          : (mode == 1) ? TAM_MIRROR
                                        : TAM_CLAMP;
    gVK.setTextureAddressMode(a);
}

uint32_t LLRenderVKBackend::resolveTexture(LLTextureData* image)
{
    if (!image) return 0; // white
    // #83: a dynamic texture rendered on the GPU samples its render target (its CPU
    // raw is a blank placeholder).
    {
        auto& targets = targetsByImage();
        auto t = targets.find(image);
        if (t != targets.end())
            return gVK.isRenderTargetReady(t->second) ? t->second : 0;
    }
    image->markVKUiUsed();   // (#707: the UI lease holds the copy until it is uploaded)

    LLImageRaw* raw = image->getVKCpuData();
    // #707: the CPU copy was dropped (the GPU holds the texture): an up-to-date UI image
    // keeps drawing -- it is the UI's copy now (never idle-evicted, see tickCache). Gone
    // too: white until the owner re-sources the pixels (LLTexUnit::bind asks it).
    if (!raw && image->isVKCpuDropped())
    {
        auto cit = mTexCache.find(image);
        if (cit != mTexCache.end() && cit->second.serial == image->getVKSerial()
            && cit->second.generation == image->getVKGeneration())
        {
            cit->second.cpu_gone = true;
            cit->second.lastUsedFrame = gVK.frameCounter();
            cit->second.lastUsedSec = LLFrameTimer::getElapsedSeconds();
            cit->second.kind = image->getVKUiKind();
            return cit->second.vkId;
        }
        image->noteVKUiNeedsCpu();
        return 0;
    }
    if (!raw || !raw->getData() || raw->getWidth() == 0)
    {
        // DIAG (post GL-removal white UI): log each image that resolves to white, once.
        static std::unordered_set<const LLTextureData*> s_logged;
        if (s_logged.size() < 64 && s_logged.insert(image).second)
        {
            LL_WARNS("VKWhite") << "resolveTexture -> white: img=" << (const void*)image
                << " raw=" << (const void*)raw
                << " rawdata=" << (raw ? (const void*)raw->getData() : nullptr)
                << " raw_wh=" << (raw ? raw->getWidth() : -1) << "x" << (raw ? raw->getHeight() : -1)
                << " img_wh=" << image->getWidth() << "x" << image->getHeight()
                << " comps=" << (S32)image->getComponents()
                << " discard=" << image->getDiscardLevel()
                << " gen=" << image->getVKGeneration() << LL_ENDL;
        }
    }
    if (!raw)
    {
        // No CPU pixels available (generic LLTextureData / UI icon whose LLImageRaw was
        // discarded after GL upload). Render white — solid UI + text still work;
        // textured icons are a known gap for this milestone.
        return 0;
    }

    tickCache();

    S32 gen = image->getVKGeneration();
    const U64 serial = image->getVKSerial();
    auto it = mTexCache.find(image);
    // Address reused by a different (newer) LLTextureData: the entry is stale.
    if (it != mTexCache.end() && it->second.serial != serial)
    {
        dropEntry(it);
        it = mTexCache.end();
    }

    U32 w = (U32)raw->getWidth();
    U32 h = (U32)raw->getHeight();
    S8  comps = raw->getComponents();
    if (w == 0 || h == 0 || !raw->getData()) return 0;

    if (it != mTexCache.end() && it->second.generation == gen &&
        it->second.width == w && it->second.height == h)
    {
        it->second.lastUsedFrame = gVK.frameCounter();
        it->second.lastUsedSec = LLFrameTimer::getElapsedSeconds();
        it->second.kind = image->getVKUiKind();
        it->second.cpu_gone = false;
        image->noteVKUiUploaded(gen);   // #707
        return it->second.vkId; // up to date
    }

    const U8* src = raw->getData();
    const eTexelFormat fmt = chooseVKFormat(src, w, h, comps, image->getVKUiKind(),
                                            comps == 1 && image->getPrimaryFormat() == LLPixelFormat::ALPHA);
    const U8 bpp = (U8)texelBytes(fmt);

    const U64 bytes = (U64)w * h * bpp;
    if (it == mTexCache.end())
    {
        // Font atlas is point-sampled (TFO_POINT). Create nearest. Pixels go straight
        // into the upload staging (no packed copy).
        uint32_t vkId = 0;
        if (fmt == TXF_COVERAGE8 || (fmt == TXF_RGBA8 && comps != 4))
        {
            // the image without pixels, then the converted texels in full
            vkId = gVK.createTexture(w, h, nullptr, /*nearest=*/true, fmt);
            U8* dst = gVK.beginTextureUpdate(vkId, w, h, fmt, 0, 0, w, h, /*mayDefer=*/false);
            if (!dst)
            {
                // no staging (out of memory): never cache an image without contents --
                // white this bind, retried on the next one
                gVK.destroyTexture(vkId);
                return 0;
            }
            packRectForVK(src, w, comps, fmt, 0, 0, w, h, dst);
            gVK.commitTextureUpdate();
        }
        else
        {
            vkId = gVK.createTexture(w, h, src, /*nearest=*/true, fmt);
        }
        image->resetVKDirty();
        CachedTex ct;
        ct.bpp = bpp;
        ct.fmt = (U8)fmt;
        ct.vkId = vkId; ct.generation = gen; ct.width = w; ct.height = h;
        ct.serial = serial; ct.bytes = bytes; ct.lastUsedFrame = gVK.frameCounter();
        ct.lastUsedSec = LLFrameTimer::getElapsedSeconds();
        ct.kind = image->getVKUiKind();
        mTexCache[image] = ct;
        mCacheBytes += bytes;
        image->noteVKUiUploaded(gen);   // #707: the UI holds its own copy now
        ++mStatUploads;
        mStatUploadBytes += bytes;
        return vkId;
    }
    else
    {
        // Generation changed (new glyphs / atlas grew / a media frame): refresh the VK
        // image. #458: only the rectangle setSubImage changed since our last upload
        // when that is known (same size + format), else in full.
        CachedTex& ct = it->second;
        S32 rx = 0, ry = 0, rw = 0, rh = 0;
        const bool same = ct.width == w && ct.height == h && ct.fmt == (U8)fmt;
        const bool rect = same && image->getVKDirtyRect(ct.generation, rx, ry, rw, rh);
        U64 sent = 0;
        if (rect && (rw <= 0 || rh <= 0))
        {
            // nothing changed (e.g. a media frame with no dirty texels)
        }
        else
        {
            if (!rect) { rx = ry = 0; rw = (S32)w; rh = (S32)h; }
            U8* dst = gVK.beginTextureUpdate(ct.vkId, w, h, fmt, (U32)rx, (U32)ry, (U32)rw, (U32)rh);
            // No staging (ring full -- deferred -- or out of memory): keep the old
            // generation, so a later bind retries.
            if (!dst) return ct.vkId;
            packRectForVK(src, w, comps, fmt, (U32)rx, (U32)ry, (U32)rw, (U32)rh, dst);
            gVK.commitTextureUpdate();
            sent = (U64)rw * rh * bpp;
            if (rect) ++mStatPartial;
        }
        image->resetVKDirty();
        ct.bpp = bpp;
        ct.fmt = (U8)fmt;
        mCacheBytes -= std::min(mCacheBytes, ct.bytes);
        mCacheBytes += bytes;
        ct.generation = gen;
        ct.width = w;
        ct.height = h;
        ct.bytes = bytes;
        ct.lastUsedFrame = gVK.frameCounter();
        ct.lastUsedSec = LLFrameTimer::getElapsedSeconds();
        ct.kind = image->getVKUiKind();
        ct.cpu_gone = false;
        image->noteVKUiUploaded(gen);   // #707
        ++mStatReuploads;
        mStatUploadBytes += sent;
        return ct.vkId;
    }
}

void llvkRecordDynamicTextureCopy(VkCommandBuffer cmd, const LLVKDynTexSource& src,
                                  VkImage dst, uint32_t levels)
{
    if (!cmd || !src.image || !dst || !src.width || !src.height) return;
    levels = std::max(levels, 1u);
    auto barrier = [&](VkImage image, uint32_t level, uint32_t count, VkImageLayout from, VkImageLayout to,
                       VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da)
    {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = ss; b.srcAccessMask = sa; b.dstStageMask = ds; b.dstAccessMask = da;
        b.oldLayout = from; b.newLayout = to; b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, count, 0, 1};
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.imageMemoryBarrierCount = 1; d.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &d);
    };
    // Source: rendered this frame (colour attachment) or earlier, sampled by the UI.
    barrier(src.image, 0, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    // Destination: prior frames' fragment reads, its creation upload.
    barrier(dst, 0, levels, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageCopy r{};
    r.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.extent = {src.width, src.height, 1};
    vkCmdCopyImage(cmd, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    barrier(src.image, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COPY_BIT, 0,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    int32_t mw = (int32_t)src.width, mh = (int32_t)src.height;
    for (uint32_t i = 1; i < levels; ++i)
    {
        barrier(dst, i - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        const int32_t nw = std::max(mw / 2, 1), nh = std::max(mh / 2, 1);
        VkImageBlit bl{};
        bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 0, 1}; bl.srcOffsets[1] = {mw, mh, 1};
        bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};     bl.dstOffsets[1] = {nw, nh, 1};
        vkCmdBlitImage(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
        mw = nw; mh = nh;
    }
    if (levels > 1)
        barrier(dst, 0, levels - 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    barrier(dst, levels - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}
