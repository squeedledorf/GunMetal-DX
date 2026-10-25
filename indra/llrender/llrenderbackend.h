/**
 * @file llrenderbackend.h
 * @brief Abstract render backend seam for LLRender (the gGL surface).
 *
 * LLRender (gGL) is the immediate-mode UI drawing API; it does no drawing itself.
 * newview constructs a concrete backend wrapping the LLRenderVK facade (gVK) and
 * installs it via LLRender::setBackend(). Every immediate-mode + state method in
 * llrender.cpp forwards to it unconditionally (LLRender::sBackend is never null: a
 * no-op backend stands in until the real one is installed):
 *
 *     sBackend->vertex3f(x,y,z);
 *
 * The method names/semantics mirror LLRender. Enum VALUES are the LLRender enum
 * values (the concrete backend translates them to the facade's own enums at the
 * boundary), so this header needs no Vulkan types -- only primitives -- and stays
 * in the llrender library cleanly.
 */
#ifndef LL_LLRENDERBACKEND_H
#define LL_LLRENDERBACKEND_H

#include "stdtypes.h"

class LLTextureData;

// Pure virtual seam. All args use LLRender's own enum VALUES (passed as U32) and
// primitive scalars, so llrender needs no backend-specific headers.
class LLRenderBackend
{
public:
    virtual ~LLRenderBackend() {}

    // ---- frame lifecycle (driven by the viewer's VK present loop) -----------
    // beginFrame is called once per swapchain frame with the target extent; the
    // concrete backend opens the facade frame targeting the acquired image. The
    // record/submit/present is owned by the viewer's LLVulkanContext, not here.
    virtual void beginFrame(U32 width, U32 height) = 0;
    virtual void endFrame() = 0;

    // ---- immediate-mode geometry (mirror LLRender) --------------------------
    virtual void begin(U32 mode) = 0;
    virtual void end() = 0;
    virtual void vertex3f(F32 x, F32 y, F32 z) = 0;
    virtual void texCoord2f(F32 u, F32 v) = 0;
    virtual void color4ub(U8 r, U8 g, U8 b, U8 a) = 0;
    virtual void color4f(F32 r, F32 g, F32 b, F32 a) = 0;
    // Pre-transformed glyph/UI batch (mirror the 3-array gGL overload LLFontGL
    // uses). pos is xyz*count, uv is uv*count, col is rgba8*count.
    virtual void vertexBatchPreTransformed(const F32* pos, const F32* uv,
                                           const U8* col, S32 count) = 0;

    // ---- matrix stack (mirror LLRender) -------------------------------------
    virtual void matrixMode(U32 mode) = 0;
    virtual void pushMatrix() = 0;
    virtual void popMatrix() = 0;
    virtual void loadIdentity() = 0;
    virtual void loadMatrix(const F32* m16) = 0;
    virtual void multMatrix(const F32* m16) = 0;
    virtual void translatef(F32 x, F32 y, F32 z) = 0;
    virtual void scalef(F32 x, F32 y, F32 z) = 0;
    virtual void rotatef(F32 deg, F32 x, F32 y, F32 z) = 0;
    virtual void ortho(F32 l, F32 r, F32 b, F32 t, F32 zn, F32 zf) = 0;

    // ---- render state (mirror LLRender) -------------------------------------
    // blendFunc/setSceneBlendType/setColorMask take LLRender enum values as U32.
    virtual void blendFunc(U32 sfactor, U32 dfactor) = 0;
    virtual void setSceneBlendType(U32 type) = 0;
    virtual void setColorMask(bool writeColor, bool writeAlpha) = 0;
    virtual void setColorMask(bool r, bool g, bool b, bool a) = 0;
    virtual void setLineWidth(F32 w) = 0;
    virtual void flush() = 0;

    // depth (mirror LLRender::DepthScope, funnelled through the backend).
    virtual void setDepthTest(bool enabled, bool writeEnabled, U32 func) = 0;

    // scissor (GL bottom-left origin; backend flips to VK top-left).
    virtual void setScissor(bool enable, S32 x, S32 y, S32 w, S32 h) = 0;

    // ---- texture units (mirror gGL.getTexUnit(i) subset) --------------------
    virtual void setActiveTextureUnit(U32 unit) = 0;
    // Resolve an LLTextureData to a VK texture (upload-on-first-bind, cache by image)
    // and bind it to the active unit. `image==nullptr` binds white (unbind).
    virtual void bindImageGL(LLTextureData* image) = 0;
    virtual void unbindTexture() = 0;
    // Per-unit filter: 0=point/nearest, 1=bilinear/linear.
    virtual void setTextureFilter(U32 filter) = 0;
    // Per-unit address: 0=wrap, 1=mirror, 2=clamp.
    virtual void setTextureAddressMode(U32 mode) = 0;

    // Alpha test threshold (LLRender::setMinimumAlpha). Only offscreen render
    // targets honour it (#83: the avatar compositor); the 2D UI never alpha-tests.
    virtual void setMinimumAlpha(F32 minimum) {}

    // In-world label anchor (#487): the agent-space point a name tag / hover text is
    // attached to. Depth-tested drawing in the backend's in-world text layer is then
    // occluded by the scene as a whole, tested once at this point. on=false clears it.
    virtual void setLabelAnchor(bool on, F32 x, F32 y, F32 z, F32 bias) {}
};

#endif // LL_LLRENDERBACKEND_H
