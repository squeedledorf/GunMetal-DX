/**
 * @file llrenderrecorder.h
 * @brief Retained UI: record a stretch of LLRenderBackend calls and replay it.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
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

#ifndef LL_LLRENDERRECORDER_H
#define LL_LLRENDERRECORDER_H

#include "llrenderbackend.h"
#include "llpointer.h"
#include <vector>
#include <functional>

class LLTextureData;

// RETAINED UI (Vulkan backend). Everything the UI draws reaches the GPU through the
// LLRenderBackend seam: geometry, matrices, blend / depth / scissor state and texture
// binds, with UI offsets already applied (absolute screen coordinates). A recording
// is the exact call stream a view produced while drawing; replaying it re-issues
// that stream to the real backend without running any of the view code (layout,
// text shaping, list walks...). The caller (LLFloaterView) decides when a recording
// is still valid.
class LLRenderRecording
{
public:
    void clear();
    bool empty() const { return mCmds.empty(); }
    size_t bytes() const;
    // Re-issue the recorded calls to `backend`. `live(i)` is called at each live-view
    // marker (LLRender::recordRetainedLive) to draw that content fresh.
    void replay(LLRenderBackend* backend, const std::function<void(U32)>& live = nullptr) const;

private:
    friend class LLRenderRecorder;
    friend class LLRender;
    enum Op : U8
    {
        OP_BEGIN, OP_END, OP_VERTEX, OP_TEXCOORD, OP_COLOR4UB, OP_COLOR4F, OP_BATCH,
        OP_MATRIX_MODE, OP_PUSH, OP_POP, OP_IDENTITY, OP_LOAD, OP_MULT, OP_TRANSLATE,
        OP_SCALE, OP_ROTATE, OP_ORTHO, OP_BLEND_FUNC, OP_SCENE_BLEND, OP_COLOR_MASK2,
        OP_COLOR_MASK4, OP_LINE_WIDTH, OP_FLUSH, OP_DEPTH, OP_SCISSOR, OP_ACTIVE_UNIT,
        OP_BIND, OP_UNBIND, OP_FILTER, OP_ADDRESS, OP_LIVE
    };
    struct Cmd
    {
        U8  op;
        U8  b[3];
        U32 u[2];
        F32 f[6];
    };
    std::vector<Cmd> mCmds;
    std::vector<F32> mF;                       // matrices (16) and batch pos (3n) / uv (2n)
    std::vector<U8>  mC;                       // batch colours (4n)
    std::vector<LLPointer<LLTextureData> > mImages;
    // LLRender-side texture bookkeeping as the recorded draw left it; restored after a
    // replay so LLTexUnit / font capture see what the backend actually has bound.
    static constexpr U32 kTrackedUnits = 8;
    LLPointer<LLTextureData> mEndBound[kTrackedUnits];
    LLPointer<LLTextureData> mEndImage0;
};

// Forwarding backend: every call goes to the real backend AND into a recording.
class LLRenderRecorder : public LLRenderBackend
{
public:
    LLRenderRecorder(LLRenderBackend* real, LLRenderRecording* out) : mReal(real), mOut(out) {}

    void beginFrame(U32 width, U32 height) override { mReal->beginFrame(width, height); mBroken = true; }
    void endFrame() override { mReal->endFrame(); mBroken = true; }

    void begin(U32 mode) override;
    void end() override;
    void vertex3f(F32 x, F32 y, F32 z) override;
    void texCoord2f(F32 u, F32 v) override;
    void color4ub(U8 r, U8 g, U8 b, U8 a) override;
    void color4f(F32 r, F32 g, F32 b, F32 a) override;
    void vertexBatchPreTransformed(const F32* pos, const F32* uv, const U8* col, S32 count) override;

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

    void blendFunc(U32 sfactor, U32 dfactor) override;
    void setSceneBlendType(U32 type) override;
    void setColorMask(bool writeColor, bool writeAlpha) override;
    void setColorMask(bool r, bool g, bool b, bool a) override;
    void setLineWidth(F32 w) override;
    void flush() override;
    void setDepthTest(bool enabled, bool writeEnabled, U32 func) override;
    void setScissor(bool enable, S32 x, S32 y, S32 w, S32 h) override;
    // Per-frame world state (#487): forwarded, not recordable.
    void setLabelAnchor(bool on, F32 x, F32 y, F32 z, F32 bias) override { mReal->setLabelAnchor(on, x, y, z, bias); mBroken = true; }

    void setActiveTextureUnit(U32 unit) override;
    void bindImageGL(LLTextureData* image) override;
    void unbindTexture() override;
    void setTextureFilter(U32 filter) override;
    void setTextureAddressMode(U32 mode) override;

    bool broken() const { return mBroken; }
    // Live sections: calls are forwarded but not recorded (a live-view marker stands
    // in for them).
    void setPaused(bool p) { mPaused = p; }
    bool paused() const { return mPaused; }
    void addLive(U32 index);

private:
    LLRenderRecording::Cmd& add(U8 op);
    LLRenderBackend*   mReal;
    LLRenderRecording* mOut;
    bool               mBroken = false;   // something unrecordable happened (frame boundary)
    bool               mPaused = false;
};

#endif // LL_LLRENDERRECORDER_H
